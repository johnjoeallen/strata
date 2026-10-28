// Tokio comparator for c/http/server.c: same protocol, same responses, same keep-alive behaviour.
//
//   http_server port=18080 workers=4 cpu0=0 sched=mt|pc buf=inline|lazy
//   sched=mt  one listener, multi-thread work-stealing runtime (the idiomatic setup)
//   sched=pc  one single-thread runtime per core, each with its own SO_REUSEPORT listener (closest to server.c)
//   buf=inline  2 KB buffer is a local of the connection task (held across every await, like a naive handler)
//   buf=lazy    wait for readability first, take a Vec only while a request is in flight (what server.c does)
use std::io::ErrorKind;
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
use std::sync::Arc;
use tokio::io::AsyncWriteExt;
use tokio::net::{TcpListener, TcpSocket, TcpStream};

const BUFSZ: usize = 2048;
static ACCEPTED: AtomicU64 = AtomicU64::new(0);
static LIVE: AtomicU64 = AtomicU64::new(0);

struct Resps { r200: Vec<u8>, r200_ka: Vec<u8>, r200_close: Vec<u8>, r404: Vec<u8>, r400: Vec<u8> }
fn mk(status: &str, conn: &str, body: &str) -> Vec<u8> {
    format!("HTTP/1.1 {status}\r\nContent-Type: text/plain\r\nContent-Length: {}\r\n{conn}\r\n{body}", body.len()).into_bytes()
}
fn resps() -> Resps {
    Resps {
        r200: mk("200 OK", "", "Hello, world\n"),
        r200_ka: mk("200 OK", "Connection: keep-alive\r\n", "Hello, world\n"),
        r200_close: mk("200 OK", "Connection: close\r\n", "Hello, world\n"),
        r404: mk("404 Not Found", "Connection: close\r\n", "not found\n"),
        r400: mk("400 Bad Request", "Connection: close\r\n", "bad request\n"),
    }
}

fn find(h: &[u8], n: &[u8]) -> Option<usize> { h.windows(n.len()).position(|w| w == n) }

fn conn_header(b: &[u8]) -> (bool, bool) {
    let (mut close, mut ka) = (false, false);
    for line in b.split(|&c| c == b'\n').skip(1) {
        if line.len() > 11 && line[..11].eq_ignore_ascii_case(b"connection:") {
            let v = &line[11..];
            close = v.windows(5).any(|w| w.eq_ignore_ascii_case(b"close"));
            ka = v.windows(10).any(|w| w.eq_ignore_ascii_case(b"keep-alive"));
            break;
        }
    }
    (close, ka)
}

fn route<'a>(b: &[u8], r: &'a Resps) -> (&'a [u8], bool) {
    if b.len() < 14 || &b[..4] != b"GET " { return (&r.r400, false); }
    let Some(sp) = b[4..].iter().position(|&c| c == b' ') else { return (&r.r400, false) };
    let sp = sp + 4;
    let http11 = b.len() >= sp + 9 && &b[sp + 1..sp + 9] == b"HTTP/1.1";
    let (close, ka) = conn_header(b);
    let keep = if http11 { !close } else { ka };
    if sp == 5 && b[4] == b'/' {
        let resp: &[u8] = if !keep { &r.r200_close } else if http11 { &r.r200 } else { &r.r200_ka };
        return (resp, keep);
    }
    (&r.r404, false)
}

// Try to complete a request from `buf[..len]`, reading more if needed. Returns header end or None on EOF/error.
async fn read_request(s: &mut TcpStream, buf: &mut [u8], len: &mut usize) -> Option<usize> {
    loop {
        if *len > 0 { if let Some(e) = find(&buf[..*len], b"\r\n\r\n") { return Some(e + 4); } }
        if *len >= buf.len() { return None; }
        s.readable().await.ok()?;
        match s.try_read(&mut buf[*len..]) {
            Ok(0) => return None,
            Ok(n) => *len += n,
            Err(e) if e.kind() == ErrorKind::WouldBlock => {}
            Err(_) => return None,
        }
    }
}

async fn handle_inline(mut s: TcpStream, r: Arc<Resps>) {
    let mut buf = [0u8; BUFSZ];
    let mut len = 0usize;
    loop {
        let Some(end) = read_request(&mut s, &mut buf, &mut len).await else { break };
        let (resp, keep) = route(&buf[..end], &r);
        if s.write_all(resp).await.is_err() || !keep { break; }
        buf.copy_within(end..len, 0);
        len -= end;
    }
    LIVE.fetch_sub(1, Relaxed);
}

async fn handle_lazy(mut s: TcpStream, r: Arc<Resps>) {
    // No buffer is held across an await: bytes are read into a stack scratch inside a synchronous scope and
    // only the bytes of a request in flight are kept (empty, unallocated Vec while the connection is idle).
    let mut carry: Vec<u8> = Vec::new();
    'conn: loop {
        let end = loop {
            if let Some(e) = find(&carry, b"\r\n\r\n") { break e + 4; }
            if carry.len() >= BUFSZ { break 'conn; }
            if s.readable().await.is_err() { break 'conn; }
            let mut scratch = [0u8; BUFSZ];
            match s.try_read(&mut scratch) {
                Ok(0) => break 'conn,
                Ok(n) => carry.extend_from_slice(&scratch[..n]),
                Err(e) if e.kind() == ErrorKind::WouldBlock => {}
                Err(_) => break 'conn,
            }
        };
        let (resp, keep) = route(&carry[..end], &r);
        if s.write_all(resp).await.is_err() || !keep { break; }
        carry.drain(..end);
        if carry.is_empty() { carry = Vec::new(); }
    }
    LIVE.fetch_sub(1, Relaxed);
}

async fn serve(l: TcpListener, r: Arc<Resps>, lazy: bool) {
    loop {
        let Ok((s, _)) = l.accept().await else { continue };
        let _ = s.set_nodelay(true);
        ACCEPTED.fetch_add(1, Relaxed);
        LIVE.fetch_add(1, Relaxed);
        let r = r.clone();
        if lazy { tokio::spawn(handle_lazy(s, r)); } else { tokio::spawn(handle_inline(s, r)); }
    }
}

fn listener(port: u16) -> TcpListener {
    let sock = TcpSocket::new_v4().unwrap();
    sock.set_reuseaddr(true).unwrap();
    sock.set_reuseport(true).unwrap();
    sock.bind(format!("0.0.0.0:{port}").parse().unwrap()).unwrap();
    sock.listen(4096).unwrap()
}

fn pin(cpu: usize) {
    unsafe {
        let mut cs: libc_cpuset = std::mem::zeroed();
        cs.bits[cpu / 64] |= 1u64 << (cpu % 64);
        sched_setaffinity(0, std::mem::size_of::<libc_cpuset>(), &cs);
    }
}
#[repr(C)] struct libc_cpuset { bits: [u64; 16] }
extern "C" { fn sched_setaffinity(pid: i32, sz: usize, set: *const libc_cpuset) -> i32; }

fn arg(k: &str, d: &str) -> String {
    std::env::args().find_map(|a| a.strip_prefix(&format!("{k}=")).map(|v| v.to_string())).unwrap_or(d.to_string())
}

fn main() {
    let port: u16 = arg("port", "18080").parse().unwrap();
    let workers: usize = arg("workers", "4").parse().unwrap();
    let cpu0: usize = arg("cpu0", "0").parse().unwrap();
    let sched = arg("sched", "mt");
    let lazy = arg("buf", "inline") == "lazy";
    let r = Arc::new(resps());
    if arg("sizes", "0") == "1" {
        let std_ = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        rt.block_on(async {
            let c = std::net::TcpStream::connect(std_.local_addr().unwrap()).unwrap();
            let (a, _) = std_.accept().unwrap(); a.set_nonblocking(true).unwrap();
            let s = TcpStream::from_std(a).unwrap();
            let f = handle_lazy(s, r.clone());
            println!("size_of future: lazy={}", std::mem::size_of_val(&f));
            drop(f);
            let s2 = TcpStream::from_std(c).unwrap();
            let f2 = handle_inline(s2, r.clone());
            println!("size_of future: inline={}", std::mem::size_of_val(&f2));
        });
        return;
    }
    eprintln!("tokio server: port={port} workers={workers} sched={sched} buf={}", if lazy { "lazy" } else { "inline" });
    if sched == "pc" {
        let mut hs = vec![];
        for i in 0..workers {
            let r = r.clone();
            hs.push(std::thread::spawn(move || {
                pin(cpu0 + i);
                let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
                rt.block_on(async move { serve(listener(port), r, lazy).await });
            }));
        }
        monitor();
    } else {
        let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(workers).enable_all()
            .on_thread_start(move || {
                use std::sync::atomic::AtomicUsize;
                static N: AtomicUsize = AtomicUsize::new(0);
                pin(cpu0 + N.fetch_add(1, Relaxed) % workers);
            })
            .build().unwrap();
        rt.spawn(async move { serve(listener(port), r, lazy).await });
        monitor();
    }
}

fn monitor() {
    let stats = arg("stats", "0") == "1";
    loop {
        std::thread::sleep(std::time::Duration::from_secs(1));
        if stats { println!("live={} accepted={}", LIVE.load(Relaxed), ACCEPTED.load(Relaxed)); }
    }
}
