# Lingering-close tests. usage: linger_test.py <port> <test> [server-pid]
import socket, sys, time, re, os, threading, random
port, test = int(sys.argv[1]), sys.argv[2]
pid = int(sys.argv[3]) if len(sys.argv) > 3 else None
def _hook(tp, e, tb): print(f"{test}: exception {tp.__name__}: {e} -> FAIL")
sys.excepthook = _hook
def conn():
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(20); return s
def finish(name, ok, detail=""): print(f"{name}: {detail} -> {'PASS' if ok else 'FAIL'}")
def nfd(): return len(os.listdir(f"/proc/{pid}/fd"))

def chunked_body(total, piece=65536):
    block = os.urandom(piece); out = []
    n = 0
    while n < total:
        k = min(piece, total - n); out.append(b"%x\r\n" % k + block[:k] + b"\r\n"); n += k
    out.append(b"0\r\n\r\n"); return b"".join(out)

def naive_client(request_bytes):
    """What most HTTP clients do: write the entire request, THEN read the response."""
    s = conn(); sent_ok = True; err = None
    try: s.sendall(request_bytes)
    except Exception as e: sent_ok = False; err = type(e).__name__
    buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            buf += d
    except Exception as e: err = err or type(e).__name__
    s.close()
    m = re.match(rb"HTTP/1\.1 (\d{3})", buf)
    return (int(m.group(1)) if m else None), sent_ok, err

if test in ("answer413", "answer413-upload", "answer431", "answer400", "closeheader"):
    N = 40 << 20                                                                       # bigger than the socket buffers on both sides
    if test == "answer413":        req, want = b"POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked_body(N), 413
    elif test == "answer413-upload": req, want = b"POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n" + chunked_body(N), 413
    elif test == "answer431":      req, want = b"GET / HTTP/1.1\r\nX-Pad: " + b"a" * 20000 + b"\r\n\r\n" + os.urandom(N), 431
    elif test == "answer400":      req, want = b"POST /api HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n" + os.urandom(N), 400
    else:                          req, want = b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n" + os.urandom(N), 200   # Connection: close, then more bytes the server never reads
    t0 = time.time(); status, sent_ok, err = naive_client(req)
    finish(test, status == want and sent_ok, f"client wrote {len(req) >> 20} MB then read: status={status} (want {want}), whole request written={sent_ok}, error={err}, {time.time() - t0:.2f}s")
elif test == "forever":                  # a client that never stops sending: the server gives up after the time limit and closes
    s = conn(); s.sendall(b"POST /api HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n")        # 413 immediately
    st = Reader = None
    buf = b""
    while b"\r\n\r\n" not in buf: buf += s.recv(4096)
    got413 = buf.startswith(b"HTTP/1.1 413")
    t0 = time.time(); blk = os.urandom(65536); gave_up = None
    try:
        while time.time() - t0 < 10:
            s.sendall(blk)
    except Exception as e: gave_up = time.time() - t0
    s.close(); time.sleep(0.5)
    finish("forever", got413 and gave_up is not None and gave_up < float(sys.argv[4]) + 1.5, f"413 answered={got413}; the server closed on the endless sender after {gave_up if gave_up is None else round(gave_up, 2)} s (limit {sys.argv[4]} s)")
elif test == "bytecap":                  # lingerbytes= bounds how much it will discard
    s = conn(); s.sendall(b"POST /api HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n")
    buf = b""
    while b"\r\n\r\n" not in buf: buf += s.recv(4096)
    sent = 0; blk = os.urandom(65536); gave_up = False
    try:
        while sent < (64 << 20): s.sendall(blk); sent += len(blk)
    except Exception: gave_up = True
    s.close()
    finish("bytecap", buf.startswith(b"HTTP/1.1 413") and gave_up and sent < (30 << 20), f"server stopped accepting after about {sent >> 20} MB (lingerbytes={sys.argv[4]})")
elif test == "early":                    # many connections that answer 413 and are closed by the client at once: nothing leaks, nothing stale fires
    before = nfd()
    def one():
        for _ in range(250):
            s = conn(); s.sendall(b"POST /api HTTP/1.1\r\nContent-Length: 999999999\r\n\r\n")
            buf = b""
            while b"\r\n\r\n" not in buf: buf += s.recv(4096)
            s.close()
    ths = [threading.Thread(target=one) for _ in range(8)]
    [t.start() for t in ths]; [t.join() for t in ths]
    time.sleep(float(sys.argv[4]) + 0.5)                      # past the linger deadline: any stale timer would have fired by now
    alive = True
    try:
        s = conn(); s.sendall(b"GET / HTTP/1.1\r\n\r\n"); alive = s.recv(4096).startswith(b"HTTP/1.1 200"); s.close()
    except Exception: alive = False
    after = nfd()
    finish("early", alive and after <= before + 1, f"2000 answered-and-closed connections; server fds {before}->{after}, still serving={alive}")
