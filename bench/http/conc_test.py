# Correctness tests for concurrent pipelined request handling. usage: conc_test.py <port> <test>
import socket, sys, time, re
port, test = int(sys.argv[1]), sys.argv[2]

import os as _os, re as _re
HOST_HDR = _os.environ.get("HOST_HDR") == "1"        # net/http (and any strict server) refuses an HTTP/1.1 request without a Host header
_HEAD = _re.compile(rb"(?:GET|POST) [^\r\n]* HTTP/1\.[01]\r\n(?:[^\r\n]+\r\n)*\r\n")
def fix_host(b):
    def one(m):
        h = m.group(0)
        return h if _re.search(rb"\r\nhost:", h, _re.I) else h.replace(b"\r\n", b"\r\nHost: t\r\n", 1)
    return _HEAD.sub(one, b)
if HOST_HDR:
    _sendall = socket.socket.sendall
    socket.socket.sendall = lambda self, data, *a: _sendall(self, fix_host(data), *a)

def run(reqs, nresp, timeout=5):
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(timeout)
    t0 = time.time(); s.sendall(b"".join(reqs)); buf = b""
    while len(re.findall(rb"HTTP/1.1 200", buf)) < nresp:
        d = s.recv(65536)
        if not d: break
        buf += d
    el = (time.time() - t0) * 1000
    s.close()
    return buf, el

def bodies(buf): return [int(x) for x in re.findall(rb"\r\n\r\n(\d+)\n", buf)]

def get(i, us): return b"GET /echo/%d?s=%d HTTP/1.1\r\nHost: t\r\n\r\n" % (i, us)
def post(i, us): return b"POST /echo/%d?s=%d HTTP/1.1\r\nHost: t\r\n\r\n" % (i, us)

def padded(i, pad): return b"GET /echo/%d HTTP/1.1\r\nX-Pad: " % i + b"a" * pad + b"\r\n\r\n"

def statuses(buf): return [int(x) for x in re.findall(rb"HTTP/1\.1 (\d{3})", buf)]

if test == "bigheader":                   # headers that force growth through 512 B -> 2 KB -> 8 KB -> 16 KB, mid-pipeline, in fragments
    import random
    random.seed(7)
    pads = [10, 3000, 20, 10000, 30, 700, 15500, 40]               # 15.5 KB + request line stays below the 16 KB class
    reqs = b"".join(padded(i, p) for i, p in enumerate(pads))
    if HOST_HDR: reqs = fix_host(reqs)
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    i = 0; buf = b""
    try:
        while i < len(reqs):
            k = random.choice([1, 7, 100, 700, 3000, 9000]); s.sendall(reqs[i:i+k]); i += k
            if random.random() < 0.15: time.sleep(0.001)
        while len(statuses(buf)) < len(pads):
            d = s.recv(65536)
            if not d: break
            buf += d
    except (BrokenPipeError, ConnectionResetError):
        pass                                                        # the server dropped the connection: the statuses below show it
    st = statuses(buf); b = bodies(buf)
    ok = st == [200] * len(pads) and (b == list(range(len(pads))) or b == [])    # bodies are only echoed in conc mode
    print(f"bigheader: statuses={st} bodies={b} -> {'PASS' if ok else 'FAIL'}")
elif test == "maxhead":                   # server started with maxhead=2048: 1500 B header is fine, 3000 B is a 431
    r1, _ = run([padded(1, 1500)], 1)
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5); s.sendall(padded(2, 3000)); buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            buf += d
    except ConnectionResetError:
        pass
    ok = statuses(r1) == [200] and statuses(buf) == [431]
    print(f"maxhead: 1500 B -> {statuses(r1)}, 3000 B -> {statuses(buf)} (expect [200] and [431]) -> {'PASS' if ok else 'FAIL'}")
elif test == "boom":                        # a handler that throws -> 500 for that request only; the connection survives
    reqs = [b"GET / HTTP/1.1\r\n\r\n", b"GET /boom HTTP/1.1\r\n\r\n", b"GET / HTTP/1.1\r\n\r\n", b"GET /boom HTTP/1.1\r\n\r\n",
            b"GET / HTTP/1.1\r\nConnection: close\r\n\r\n"]
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5); s.sendall(b"".join(reqs)); buf = b""
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
    st = statuses(buf)
    ok = st == [200, 500, 200, 500, 200]
    print(f"boom: statuses={st} (expect 200,500,200,500,200; the connection served all five) -> {'PASS' if ok else 'FAIL'}")
elif test == "toolarge":                  # header block larger than the buffer -> 431, then the connection closes
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5)
    s.sendall(b"GET / HTTP/1.1\r\nX-Pad: " + b"a" * int(_os.environ.get("TOOLARGE_PAD", "20000")) + b"\r\n\r\n"); buf = b""
    try:
        while True:
            d = s.recv(65536)
            if not d: break
            buf += d
    except ConnectionResetError:
        pass
    st = statuses(buf)
    ok = st == [431]
    print(f"toolarge: statuses={st} (expect [431], then close) -> {'PASS' if ok else 'FAIL'}")
elif test == "copy":                      # children read the request AFTER waiting; the connection reuses its buffer meanwhile
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(5)
    s.sendall(b"".join(get(i, 60000) for i in (1, 2, 3, 4)))          # four handlers that wait 60 ms
    time.sleep(0.02)
    s.sendall(get(91, 1000))                                         # arrives while they wait: it is read into the SAME buffer block
    buf = b""
    while len(re.findall(rb"HTTP/1.1 200", buf)) < 5:
        d = s.recv(65536)
        if not d: break
        buf += d
    b = bodies(buf)
    ok = b == [1, 2, 3, 4, 91]
    print(f"copy: bodies={b} (expect [1, 2, 3, 4, 91]) -> {'PASS' if ok else 'FAIL'}")
elif test == "order":                       # handlers finish in reverse order; responses must still be in request order
    n = 16
    buf, el = run([get(i, (n - i) * 2000) for i in range(n)], n)
    b = bodies(buf)
    ok = b == list(range(n)) and el < 100
    print(f"order: bodies={b} elapsed={el:.0f}ms (concurrent ~32, sequential ~272) -> {'PASS' if ok else 'FAIL'}")
elif test == "barrier":                   # G G G P G G, 20 ms each: barrier => ~60 ms; overlapping => ~20 ms
    reqs = [get(0, 20000), get(1, 20000), get(2, 20000), post(3, 20000), get(4, 20000), get(5, 20000)]
    buf, el = run(reqs, 6)
    b = bodies(buf)
    ok = b == list(range(6)) and 50 <= el <= 90
    print(f"barrier: bodies={b} elapsed={el:.0f}ms (expect ~60 with the barrier, ~20 without) -> {'PASS' if ok else 'FAIL'}")
elif test == "depth":                     # depth=4 (server arg): 16 requests of 10 ms => ~40 ms; depth 16 would be ~10 ms
    buf, el = run([get(i, 10000) for i in range(16)], 16)
    b = bodies(buf)
    ok = b == list(range(16)) and 33 <= el <= 70
    print(f"depth: bodies_in_order={b == list(range(16))} elapsed={el:.0f}ms (expect ~40 at depth 4) -> {'PASS' if ok else 'FAIL'}")
elif test == "close":                     # a Connection: close request ends the stream; nothing after it is answered
    reqs = [get(0, 5000), get(1, 5000), b"GET /echo/2 HTTP/1.1\r\nConnection: close\r\n\r\n", get(3, 0)]
    buf, el = run(reqs, 3)
    s_ = buf.decode(errors="ignore")
    ok = bodies(buf) == [0, 1, 2] and "Connection: close" in s_
    print(f"close: bodies={bodies(buf)} -> {'PASS' if ok else 'FAIL'}")
