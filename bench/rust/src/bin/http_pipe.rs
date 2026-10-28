// The Rust equivalent of bench/c/http/server.stra: same protocol, routes, and behaviour as server.c.
//
//   http_pipe port=18080 workers=4 cpu0=0 conc=1 depth=16 inline=1 work=0 sleep_us=0 slow=1250000
//
//   conc=0   one request at a time, every response flushed at once          (server.c conc=0)
//   conc=1   pipelined requests run as their own tasks, completed prefix of responses sent as one write
//            (server.c conc=1): up to `depth` in flight, POST is a barrier both ways, GET /slow is a barrier before it,
//            a lone request runs inline, 431 for an oversized header block, a handler error becomes a 500.
//
// One current-thread runtime per core, each with its own SO_REUSEPORT listener (the closest layout to server.c).
// Routes: GET|POST /  (cheap), /echo/N?s=US (sleeps s us, then echoes N), GET /slow (~1 ms of CPU), GET /boom (handler error).
use std::collections::VecDeque;
use std::io::{ErrorKind, Write};
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio::net::{TcpListener, TcpSocket, TcpStream};
use tokio::task::JoinHandle;

const BUFSZ: usize = 2048;

#[derive(Clone, Copy)]
struct Cfg { conc: bool, depth: usize, inline: bool, nospawn: bool, work: u32, sleep_us: u64, slow: u32 }

// ---------------------------------------------------------------- responses
struct Resps { r200: &'static [u8], r200_ka: &'static [u8], r200_close: &'static [u8], r404: &'static [u8], r400: &'static [u8], r431: &'static [u8], r500: &'static [u8] }

fn reason_phrase(code: u32) -> &'static str {
    match code {
        200 => "OK",
        301 | 302 | 307 => "Redirect",
        400 => "Bad Request",
        404 => "Not Found",
        431 => "Request Header Fields Too Large",
        500 => "Internal Server Error",
        _ => "Other",
    }
}
fn mk(code: u32, conn: &str, body: &str) -> &'static [u8] {
    let s = format!("HTTP/1.1 {code} {}\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n{conn}\r\n{body}", reason_phrase(code), body.len());
    Box::leak(s.into_bytes().into_boxed_slice())
}
fn resps() -> &'static Resps {
    Box::leak(Box::new(Resps {
        r200: mk(200, "", "Hello, world\n"),
        r200_ka: mk(200, "Connection: keep-alive\r\n", "Hello, world\n"),
        r200_close: mk(200, "Connection: close\r\n", "Hello, world\n"),
        r404: mk(404, "Connection: close\r\n", "not found\n"),
        r400: mk(400, "Connection: close\r\n", "bad request\n"),
        r431: mk(431, "Connection: close\r\n", "headers too large\n"),
        r500: mk(500, "", "internal error\n"),
    }))
}

enum Resp { Static(&'static [u8]), Echo(u32, bool) }
impl Resp {
    fn append(&self, out: &mut Vec<u8>) {
        match self {
            Resp::Static(b) => out.extend_from_slice(b),
            Resp::Echo(n, keep) => {
                let body = format!("{n}\n");
                let _ = write!(out, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n{}\r\n{}",
                               body.len(), if *keep { "" } else { "Connection: close\r\n" }, body);
            }
        }
    }
}

// ---------------------------------------------------------------- request parsing and routing (pure)
fn find(h: &[u8], n: &[u8]) -> Option<usize> { h.windows(n.len()).position(|w| w == n) }

fn conn_header(b: &[u8]) -> (bool, bool) {
    for line in b.split(|&c| c == b'\n').skip(1) {
        if line.len() > 11 && line[..11].eq_ignore_ascii_case(b"connection:") {
            let v = &line[11..];
            return (v.windows(5).any(|w| w.eq_ignore_ascii_case(b"close")), v.windows(10).any(|w| w.eq_ignore_ascii_case(b"keep-alive")));
        }
    }
    (false, false)
}

#[derive(Clone, Copy, PartialEq)]
enum Kind { Index, Echo, Slow, Boom, NotFound, Bad }
#[derive(Clone, Copy)]
struct Route { kind: Kind, post: bool, keep: bool, http11: bool }
impl Route {
    fn unsafe_method(&self) -> bool { self.post }
    fn unbounded(&self) -> bool { self.kind == Kind::Slow }
}

fn lookup(b: &[u8]) -> Route {
    let bad = Route { kind: Kind::Bad, post: false, keep: false, http11: false };
    let off = if b.starts_with(b"POST ") { 5 } else if b.starts_with(b"GET ") { 4 } else { 0 };
    if b.len() < 14 || off == 0 { return bad; }
    let rest = &b[off..];
    let Some(sp) = rest.iter().position(|&c| c == b' ') else { return bad };
    let path = &rest[..sp];
    let http11 = rest.len() >= sp + 9 && &rest[sp + 1..sp + 9] == b"HTTP/1.1";
    let (close, ka) = conn_header(b);
    let keep = if http11 { !close } else { ka };
    let post = off == 5;
    let kind = if path == b"/" { Kind::Index }
        else if !post && path == b"/slow" { Kind::Slow }
        else if !post && path == b"/boom" { Kind::Boom }
        else if path.len() > 6 && path.starts_with(b"/echo/") { Kind::Echo }
        else { return Route { kind: Kind::NotFound, post, keep: false, http11 } };
    Route { kind, post, keep, http11 }
}

fn parse_echo(head: &[u8]) -> Option<(u32, u64)> {      // GET /echo/N[?s=US]
    let off = if head.starts_with(b"POST ") { 5 } else if head.starts_with(b"GET ") { 4 } else { return None };
    let p = head.get(off..)?;
    let p = p.strip_prefix(b"/echo/")?;
    let digits = p.iter().take_while(|c| c.is_ascii_digit()).count();
    let n: u32 = std::str::from_utf8(&p[..digits]).ok()?.parse().unwrap_or(0);
    let mut us = 0u64;
    if let Some(q) = p[digits..].strip_prefix(b"?s=") {
        let d = q.iter().take_while(|c| c.is_ascii_digit()).count();
        us = std::str::from_utf8(&q[..d]).ok()?.parse().unwrap_or(0);
    }
    Some((n, us))
}

fn spin_work(n: u32) {
    let mut x: u64 = 1;
    for _ in 0..n {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        unsafe { std::arch::asm!("/* {0} */", inout(reg) x, options(nomem, nostack)); }     // opaque to the optimiser: the loop really runs
    }
    std::hint::black_box(x);
}

// ---------------------------------------------------------------- handlers
// A handler can fail (Err); run_handler turns that into a 500 and the connection survives.
async fn handler(rt: &Route, head: &[u8], cfg: Cfg, rs: &'static Resps) -> Result<Resp, ()> {
    if cfg.work > 0 { spin_work(cfg.work); }
    if rt.kind == Kind::Slow { spin_work(cfg.slow); }
    let mut us = cfg.sleep_us;
    if rt.kind == Kind::Echo { if let Some((_, es)) = parse_echo(head) { if es > 0 { us = es; } } }
    if us > 0 { tokio::time::sleep(Duration::from_micros(us)).await; }
    match rt.kind {
        Kind::Boom => Err(()),
        Kind::Echo => { let (n, _) = parse_echo(head).unwrap_or((0, 0)); Ok(Resp::Echo(n, rt.keep)) }   // reads the head after waiting
        Kind::Index | Kind::Slow => Ok(Resp::Static(if !rt.keep { rs.r200_close } else if rt.http11 { rs.r200 } else { rs.r200_ka })),
        Kind::NotFound => Ok(Resp::Static(rs.r404)),
        Kind::Bad => Ok(Resp::Static(rs.r400)),
    }
}
async fn run_handler(rt: &Route, head: &[u8], cfg: Cfg, rs: &'static Resps) -> Resp {
    match handler(rt, head, cfg, rs).await { Ok(r) => r, Err(()) => Resp::Static(rs.r500) }
}

// ---------------------------------------------------------------- reading
// Unconsumed input is carry[start..]. Nothing is allocated while the connection is idle.
enum Read { Got, WouldBlock, Eof }
fn try_fill(s: &TcpStream, carry: &mut Vec<u8>, start: usize) -> Read {
    let mut scratch = [0u8; BUFSZ];
    let room = BUFSZ - (carry.len() - start);
    match s.try_read(&mut scratch[..room]) {
        Ok(0) => Read::Eof,
        Ok(n) => { carry.extend_from_slice(&scratch[..n]); Read::Got }
        Err(e) if e.kind() == ErrorKind::WouldBlock => Read::WouldBlock,
        Err(_) => Read::Eof,
    }
}

// ---------------------------------------------------------------- conc=0: one request at a time
async fn serve_seq(mut s: TcpStream, cfg: Cfg, rs: &'static Resps) {
    let mut carry: Vec<u8> = Vec::new();
    let mut start = 0usize;
    'conn: loop {
        let hdr_end = loop {
            if let Some(i) = find(&carry[start..], b"\r\n\r\n") { break start + i + 4; }
            if carry.len() - start >= BUFSZ { let _ = s.write_all(rs.r431).await; break 'conn; }
            if s.readable().await.is_err() { break 'conn; }
            if carry.capacity() == 0 { carry.reserve(256); }
            if let Read::Eof = try_fill(&s, &mut carry, start) { break 'conn; }
        };
        let rt = lookup(&carry[start..hdr_end]);
        let resp = run_handler(&rt, &carry[start..hdr_end], cfg, rs).await;
        let mut out = Vec::with_capacity(256);
        resp.append(&mut out);
        if s.write_all(&out).await.is_err() || !rt.keep { break; }
        start = hdr_end;
        if start == carry.len() { carry = Vec::new(); start = 0; }
    }
}

// ---------------------------------------------------------------- conc=1: pipelined, concurrent, responses in order
enum Slot { Done(Resp), Task(JoinHandle<Resp>) }

struct Inflight { q: VecDeque<Slot>, unsafe_busy: bool }

async fn serve_conc(mut s: TcpStream, cfg: Cfg, rs: &'static Resps) {
    let mut carry: Vec<u8> = Vec::new();
    let mut start = 0usize;
    let mut inf = Inflight { q: VecDeque::new(), unsafe_busy: false };
    let (mut closing, mut eof, mut broken) = (false, false, false);
    loop {
        // (a) collect the completed prefix, in request order
        let mut out: Vec<u8> = Vec::new();
        while let Some(front) = inf.q.front() {
            let done = match front { Slot::Done(_) => true, Slot::Task(h) => h.is_finished() };
            if !done { break; }
            let resp = match inf.q.pop_front().unwrap() {
                Slot::Done(r) => r,
                Slot::Task(h) => h.await.unwrap_or(Resp::Static(rs.r500)),      // already finished: returns at once
            };
            resp.append(&mut out);
        }
        if inf.q.is_empty() { inf.unsafe_busy = false; }
        // (b) send it as one write
        if !out.is_empty() {
            if s.write_all(&out).await.is_err() { broken = true; }
            if broken { break; }
            continue;
        }
        // (c) admit new requests
        let mut need_input = false;
        while !closing && inf.q.len() < cfg.depth && !inf.unsafe_busy {
            let Some(i) = find(&carry[start..], b"\r\n\r\n") else {
                if eof { break; }
                if carry.len() - start >= BUFSZ { inf.q.push_back(Slot::Done(Resp::Static(rs.r431))); closing = true; break; }
                if start > 0 && carry.len() == BUFSZ { carry.drain(..start); start = 0; }      // the only copy
                if carry.capacity() == 0 { carry.reserve(256); }
                match try_fill(&s, &mut carry, start) {
                    Read::Got => continue,
                    Read::WouldBlock => { need_input = true; break; }
                    Read::Eof => { eof = true; break; }
                }
            };
            let hdr_end = start + i + 4;
            let rt = lookup(&carry[start..hdr_end]);
            if (rt.unsafe_method() || rt.unbounded()) && !inf.q.is_empty() { break; }     // barrier: earlier requests finish and are sent first
            let head = &carry[start..hdr_end];
            let has_next = find(&carry[hdr_end..], b"\r\n\r\n").is_some();
            if !rt.keep { closing = true; }
            if rt.unsafe_method() { inf.unsafe_busy = true; }
            if cfg.nospawn || (cfg.inline && inf.q.is_empty() && !has_next) {      // nospawn: diagnostic only, runs pipelined requests one after another
                let resp = run_handler(&rt, head, cfg, rs).await;                          // inline: runs in this task, no spawn
                inf.q.push_back(Slot::Done(resp));
            } else {
                let owned = head.to_vec();                                                  // the task owns a copy of the head
                inf.q.push_back(Slot::Task(tokio::spawn(async move { run_handler(&rt, &owned, cfg, rs).await })));
            }
            start = hdr_end;
        }
        if start == carry.len() && start > 0 { carry = Vec::new(); start = 0; }              // idle: no buffer
        // (d) finished, or wait for input and/or the oldest request
        if matches!(inf.q.front(), Some(Slot::Done(_))) { continue; }
        if inf.q.is_empty() && (closing || eof) { break; }
        match inf.q.front_mut() {
            Some(Slot::Task(h)) => {
                let mut finished: Option<Resp> = None;
                if need_input {
                    tokio::select! {
                        r = &mut *h => { finished = Some(r.unwrap_or(Resp::Static(rs.r500))); }
                        _ = s.readable() => {}
                    }
                } else {
                    finished = Some(h.await.unwrap_or(Resp::Static(rs.r500)));
                }
                if let Some(r) = finished { *inf.q.front_mut().unwrap() = Slot::Done(r); }
            }
            Some(Slot::Done(_)) => unreachable!(),
            None => { if need_input { if s.readable().await.is_err() { break; } } else { break; } }
        }
    }
    for slot in inf.q { if let Slot::Task(h) = slot { h.abort(); } }       // a failed connection cancels its remaining requests
}

// ---------------------------------------------------------------- main
fn listener(port: u16) -> TcpListener {
    let sock = TcpSocket::new_v4().unwrap();
    sock.set_reuseaddr(true).unwrap();
    sock.set_reuseport(true).unwrap();
    sock.bind(format!("0.0.0.0:{port}").parse().unwrap()).unwrap();
    sock.listen(4096).unwrap()
}
#[repr(C)] struct CpuSet { bits: [u64; 16] }
extern "C" { fn sched_setaffinity(pid: i32, sz: usize, set: *const CpuSet) -> i32; }
fn pin(cpu: usize) {
    unsafe {
        let mut cs: CpuSet = std::mem::zeroed();
        cs.bits[cpu / 64] |= 1u64 << (cpu % 64);
        sched_setaffinity(0, std::mem::size_of::<CpuSet>(), &cs);
    }
}
fn arg(k: &str, d: &str) -> String {
    std::env::args().find_map(|a| a.strip_prefix(&format!("{k}=")).map(|v| v.to_string())).unwrap_or(d.to_string())
}

fn main() {
    let port: u16 = arg("port", "18080").parse().unwrap();
    let workers: usize = arg("workers", "4").parse().unwrap();
    let cpu0: usize = arg("cpu0", "0").parse().unwrap();
    let cfg = Cfg {
        conc: arg("conc", "1") == "1",
        depth: arg("depth", "16").parse::<usize>().unwrap().clamp(1, 32),
        inline: arg("inline", "1") == "1",
        nospawn: arg("nospawn", "0") == "1",
        work: arg("work", "0").parse().unwrap(),
        sleep_us: arg("sleep_us", "0").parse().unwrap(),
        slow: arg("slow", "1250000").parse().unwrap(),
    };
    let rs = resps();
    if arg("sizes", "0") == "1" {
        let std_ = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        rt.block_on(async {
            let c = std::net::TcpStream::connect(std_.local_addr().unwrap()).unwrap();
            let (a, _) = std_.accept().unwrap(); a.set_nonblocking(true).unwrap();
            let f1 = serve_seq(TcpStream::from_std(a).unwrap(), cfg, rs);
            println!("size_of future: serve_seq = {} B", std::mem::size_of_val(&f1));
            c.set_nonblocking(true).unwrap();
            let f2 = serve_conc(TcpStream::from_std(c).unwrap(), cfg, rs);
            println!("size_of future: serve_conc = {} B", std::mem::size_of_val(&f2));
            let rt_ = lookup(b"GET / HTTP/1.1\r\n\r\n");
            let f3 = run_handler(&rt_, b"", cfg, rs);
            println!("size_of future: run_handler = {} B   (contains the tokio::time::sleep state)", std::mem::size_of_val(&f3));
            println!("size_of JoinHandle<Resp> = {} B, Slot = {} B", std::mem::size_of::<JoinHandle<Resp>>(), std::mem::size_of::<Slot>());
        });
        return;
    }
    eprintln!("rust http_pipe: port={port} workers={workers} conc={} depth={} inline={}", cfg.conc as u8, cfg.depth, cfg.inline as u8);
    let mut hs = vec![];
    for i in 0..workers {
        hs.push(std::thread::spawn(move || {
            pin(cpu0 + i);
            let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
            rt.block_on(async move {
                let l = listener(port);
                loop {
                    let Ok((s, _)) = l.accept().await else { continue };
                    let _ = s.set_nodelay(true);
                    if cfg.conc { tokio::spawn(serve_conc(s, cfg, rs)); } else { tokio::spawn(serve_seq(s, cfg, rs)); }
                }
            });
        }));
    }
    for h in hs { let _ = h.join(); }
}
