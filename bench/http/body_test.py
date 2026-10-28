# Request body tests for the two readers. usage: body_test.py <port> <test> [server-pid]
#   POST /api     the standard reader: in memory, limited by maxbody (1 MB by default)
#   POST /upload  the upload reader: spooled to an unnamed temporary file, limited by uploadmax (4 GB by default)
import socket, sys, time, re, zlib, random, os, threading
port, test = int(sys.argv[1]), sys.argv[2]
pid = int(sys.argv[3]) if len(sys.argv) > 3 else None
random.seed(11)

def _hook(tp, e, tb): print(f"{test}: exception {tp.__name__}: {e} -> FAIL")
sys.excepthook = _hook
threading.excepthook = lambda a: None

def conn():
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(30); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1); return s

def post_fixed(path, data, extra=b""):
    return b"POST %s HTTP/1.1\r\nHost: t\r\nContent-Length: %d\r\n%s\r\n" % (path, len(data), extra) + data

def chunk(data, sizes, ext=False, trailers=False):
    out = b""; i = 0
    while i < len(data):
        k = min(random.choice(sizes), len(data) - i)
        out += (b"%x" % k) + (b";name=val" if ext and random.random() < .3 else b"") + b"\r\n" + data[i:i+k] + b"\r\n"; i += k
    return out + b"0\r\n" + (b"X-Trailer: abc\r\nX-More: 1\r\n" if trailers else b"") + b"\r\n"

def post_chunked(path, data, sizes=(1, 7, 100, 5000), **kw):
    return b"POST %s HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n" % path + chunk(data, list(sizes), **kw)

class Reader:
    def __init__(self, s): self.s = s; self.buf = b""
    def fill(self):
        d = self.s.recv(65536)
        if not d: raise EOFError
        self.buf += d
    def response(self):
        while b"\r\n\r\n" not in self.buf: self.fill()
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        m = re.search(rb"Content-Length: (\d+)", head); n = int(m.group(1)) if m else 0
        self.buf = rest
        while len(self.buf) < n: self.fill()
        body, self.buf = self.buf[:n], self.buf[n:]
        return int(head.split()[1]), head, body

def parse_stored(body):
    m = re.match(rb"stored (\d+) bytes in (\w+) crc32=([0-9a-f]{8})", body)
    return (int(m.group(1)), m.group(2).decode(), m.group(3).decode()) if m else None

def ok_stored(r, data, where):
    st, _, body = r
    return st == 200 and parse_stored(body) == (len(data), where, "%08x" % zlib.crc32(data))

def send_fragmented(s, reqs):
    i = 0
    while i < len(reqs):
        k = random.choice([1, 7, 100, 700, 3000, 9000, 40000]); s.sendall(reqs[i:i+k]); i += k
        if random.random() < 0.1: time.sleep(0.001)

def finish(name, ok, detail=""): print(f"{name}: {detail} -> {'PASS' if ok else 'FAIL'}")
API, UP = b"/api", b"/upload"

def one(path, data, framing, where):
    s = conn(); s.sendall(post_fixed(path, data) if framing == "fixed" else post_chunked(path, data, ext=True, trailers=True))
    r = Reader(s).response(); s.close(); return ok_stored(r, data, where)

if test == "sizes":                       # each reader places the body where its contract says, both framings
    res = []
    for path, where, sizes in [(API, "memory", [0, 1, 100, 16385, 300000, 1000000]), (UP, "file", [0, 1, 100, 16385, 300000, 5000000])]:
        for n in sizes:
            for framing in ("fixed", "chunked"):
                if framing == "chunked" and n == 0: continue
                res.append((path.decode(), n, framing, one(path, os.urandom(n), framing, where)))
    bad = [x for x in res if not x[3]]
    finish("sizes", not bad, f"{len(res)} cases, failures={bad}")
elif test == "defaultcap":                # default maxbody is 1 MB: the SAME 2 MB body is refused by the API reader and accepted by the upload reader
    out = {}
    s = conn(); s.sendall(b"POST /api HTTP/1.1\r\nContent-Length: 2097152\r\n\r\n"); out["api CL 2MB"] = Reader(s).response()[0]; s.close()
    d = os.urandom(2 << 20)
    s = conn(); resp = []
    def rd():
        try: resp.append(Reader(s).response()[0])
        except Exception as e: resp.append(repr(e))
    th = threading.Thread(target=rd); th.start()
    try: s.sendall(post_chunked(API, d, sizes=(65536,)))
    except Exception: pass
    th.join(); out["api chunked 2MB"] = resp[0]; s.close()
    out["upload fixed 2MB"] = one(UP, d, "fixed", "file")
    out["upload chunked 2MB"] = one(UP, d, "chunked", "file")
    out["api 1MB"] = one(API, d[: 1 << 20], "fixed", "memory")
    ok = out["api CL 2MB"] == 413 and out["api chunked 2MB"] == 413 and out["upload fixed 2MB"] and out["upload chunked 2MB"] and out["api 1MB"]
    finish("defaultcap", ok, str(out))
elif test == "limits":                    # server started with maxbody=100000 uploadmax=500000: two independent limits
    out = {}
    def hdr_only(path, n):
        s = conn(); s.sendall(b"POST %s HTTP/1.1\r\nContent-Length: %d\r\n\r\n" % (path, n)); r = Reader(s).response()[0]
        try: closed = s.recv(10) == b""
        except Exception: closed = True
        s.close(); return r, closed
    def chunked_over(path, n):
        d = os.urandom(n); s = conn(); resp = []
        def rd():
            try: resp.append(Reader(s).response()[0])
            except Exception as e: resp.append(repr(e))
        th = threading.Thread(target=rd); th.start()
        try: s.sendall(post_chunked(path, d, sizes=(4096,)))
        except Exception: pass
        th.join(); s.close(); return resp[0]
    out["api CL 200000"] = hdr_only(API, 200000)
    out["api CL==max"] = one(API, os.urandom(100000), "fixed", "memory")
    out["api chunked==max"] = one(API, os.urandom(100000), "chunked", "memory")
    out["api chunked 150000"] = chunked_over(API, 150000)
    out["upload CL 600000"] = hdr_only(UP, 600000)
    out["upload CL 400000"] = one(UP, os.urandom(400000), "fixed", "file")
    out["upload chunked 500000"] = one(UP, os.urandom(500000), "chunked", "file")
    out["upload chunked 700000"] = chunked_over(UP, 700000)
    s = conn(); s.sendall(b"GET / HTTP/1.1\r\n\r\n"); out["alive"] = Reader(s).response()[0]; s.close()
    ok = (out["api CL 200000"] == (413, True) and out["api CL==max"] and out["api chunked==max"] and out["api chunked 150000"] == 413
          and out["upload CL 600000"] == (413, True) and out["upload CL 400000"] and out["upload chunked 500000"] and out["upload chunked 700000"] == 413
          and out["alive"] == 200)
    finish("limits", ok, str(out))
elif test == "pipelined":                 # both readers and ordinary requests in one pipeline, one write, random fragments
    parts = []; expect = []
    for i in range(24):
        kind = i % 4
        if kind == 0: d = os.urandom(random.choice([0, 50, 5000, 20000, 70000])); parts.append(post_fixed(API, d)); expect.append(("api", d))
        elif kind == 1: parts.append(b"GET / HTTP/1.1\r\n\r\n"); expect.append(("hello", None))
        elif kind == 2: d = os.urandom(random.choice([10, 3000, 30000, 200000])); parts.append(post_chunked(UP, d, ext=True, trailers=(i % 8 == 2))); expect.append(("up", d))
        else: parts.append(b"GET /echo/%d HTTP/1.1\r\n\r\n" % i); expect.append(("echo", i))
    s = conn(); rdr = Reader(s)
    t = threading.Thread(target=send_fragmented, args=(s, b"".join(parts))); t.start()
    ok = True; got = 0
    for kind, d in expect:
        st, _, body = rdr.response(); got += 1
        if kind in ("api", "up"):
            p = parse_stored(body); ok &= (st == 200 and p is not None and p[0] == len(d) and p[2] == "%08x" % zlib.crc32(d) and p[1] == ("memory" if kind == "api" else "file"))
        elif kind == "hello": ok &= (st == 200 and body == b"Hello, world\n")
        else: ok &= (st == 200 and body == b"%d\n" % d)
    t.join(); finish("pipelined", ok, f"{got} responses in request order")
elif test == "malformed":
    cases = {}
    for name, path in (("api", b"/api"), ("upload", b"/upload")):
        cases[name + " CL+TE"]      = b"POST %s HTTP/1.1\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n" % path
        cases[name + " bad hex"]    = b"POST %s HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n0\r\n\r\n" % path
        cases[name + " no CRLF"]    = b"POST %s HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhelloXX0\r\n\r\n" % path
        cases[name + " no framing"] = b"POST %s HTTP/1.1\r\n\r\n" % path
        cases[name + " dup CL"]     = b"POST %s HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx" % path
        cases[name + " bad CL"]     = b"POST %s HTTP/1.1\r\nContent-Length: 12abc\r\n\r\nx" % path
        cases[name + " huge chunk"] = b"POST %s HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" % path + b"f" * 20 + b"\r\n"
    res = {}
    for name, req in cases.items():
        s = conn(); s.sendall(req)
        try: res[name] = Reader(s).response()[0]
        except Exception as e: res[name] = repr(e)
        s.close()
    finish("malformed", all(v == 400 for v in res.values()), f"{len(res)} cases, non-400: { {k: v for k, v in res.items() if v != 400} }")
elif test == "expect":
    d = os.urandom(50000); s = conn()
    s.sendall(b"POST /upload HTTP/1.1\r\nContent-Length: %d\r\nExpect: 100-continue\r\n\r\n" % len(d))
    first = b""
    while b"\r\n\r\n" not in first: first += s.recv(100)
    s.sendall(d); r = Reader(s).response(); s.close()
    finish("expect", first.startswith(b"HTTP/1.1 100 Continue") and ok_stored(r, d, "file"), f"interim={first[:25]!r}")
elif test == "eofmid":                    # the client vanishes mid-body, on both readers: nothing leaks, the server keeps serving
    def nfd(): return len(os.listdir(f"/proc/{pid}/fd"))
    def rssm(): return int(re.search(r"VmRSS:\s+(\d+)", open(f"/proc/{pid}/status").read()).group(1)) // 1024
    before, rb = nfd(), rssm()
    for i in range(20):
        for path in (API, UP):
            s = conn(); d = os.urandom(90000)
            s.sendall(b"POST %s HTTP/1.1\r\nContent-Length: 400000\r\n\r\n" % path + d); time.sleep(0.005); s.close()
            s = conn(); s.sendall(b"POST %s HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" % path + chunk(d, [5000])[:60000]); time.sleep(0.005); s.close()
    time.sleep(0.5)
    s = conn(); s.sendall(b"GET / HTTP/1.1\r\n\r\n"); alive = Reader(s).response()[0] == 200; s.close(); time.sleep(0.3)
    after, ra = nfd(), rssm()
    finish("eofmid", alive and after <= before + 1 and ra - rb < 10, f"server fds {before}->{after}, RSS {rb}->{ra} MB, still serving={alive}")
elif test == "large":                     # 200 MB chunked upload: the server's memory stays flat
    def rss(): return int(re.search(r"VmRSS:\s+(\d+)", open(f"/proc/{pid}/status").read()).group(1)) // 1024
    r0 = rss(); crc = 0; total = 200 * (1 << 20); t0 = time.time()
    s = conn(); s.sendall(b"POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
    block = os.urandom(1 << 20); peak = r0
    for i in range(200):
        piece = block[i:] + block[:i]; crc = zlib.crc32(piece, crc)
        s.sendall(b"%x\r\n" % len(piece) + piece + b"\r\n")
        if i % 20 == 0: peak = max(peak, rss())
    s.sendall(b"0\r\n\r\n"); r = Reader(s).response(); el = time.time() - t0; peak = max(peak, rss()); s.close()
    up = parse_stored(r[2])
    finish("large", r[0] == 200 and up == (total, "file", "%08x" % crc) and peak - r0 < 20,
           f"{total >> 20} MB in {el:.2f}s ({total / el / 1e6:.0f} MB/s), server RSS {r0} -> peak {peak} MB, reply={up}")
elif test == "apimem":                    # the standard reader's memory is bounded by maxbody x connections: measure it
    def rss(): return int(re.search(r"VmRSS:\s+(\d+)", open(f"/proc/{pid}/status").read()).group(1))
    def vsz(): return int(re.search(r"VmSize:\s+(\d+)", open(f"/proc/{pid}/status").read()).group(1))
    r0, v0 = rss(), vsz(); socks = []
    N = 500
    for i in range(N):                     # each declares a 1 MB body and sends 100 KB of it, then waits
        s = conn(); s.sendall(b"POST /api HTTP/1.1\r\nContent-Length: 1000000\r\n\r\n" + os.urandom(100000)); socks.append(s)
    time.sleep(1.0); r1, v1 = rss(), vsz()
    for s in socks: s.close()
    time.sleep(0.5); r2 = rss()
    print(f"apimem: {N} connections, each declared 1,000,000 B and sent 100,000 B: server RSS +{(r1 - r0) // 1024} MB ({(r1 - r0) * 1024 // N} B/conn), VmSize +{(v1 - v0) // 1024} MB; after they close: RSS +{(r2 - r0) // 1024} MB -> INFO")
