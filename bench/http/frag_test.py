import socket, random, sys, time
port, n, seed = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
random.seed(seed)

import os as _os, re as _re
HOST_HDR = _os.environ.get("HOST_HDR") == "1"        # net/http (and any strict server) refuses an HTTP/1.1 request without a Host header
_HEAD = _re.compile(rb"(?:GET|POST) [^\r\n]* HTTP/1\.[01]\r\n(?:[^\r\n]+\r\n)*\r\n")
def fix_host(b):
    def one(m):
        h = m.group(0)
        return h if _re.search(rb"\r\nhost:", h, _re.I) else h.replace(b"\r\n", b"\r\nHost: t\r\n", 1)
    return _HEAD.sub(one, b)
reqs = b"".join(b"GET / HTTP/1.1\r\nHost: h%d\r\n\r\n" % i if i % 3 else b"GET / HTTP/1.1\r\n\r\n" for i in range(n))
reqs += b"GET /x HTTP/1.1\r\nHost: z\r\n\r\n"      # final 404 closes the connection
if HOST_HDR: reqs = fix_host(reqs)
s = socket.create_connection(("127.0.0.1", port)); s.settimeout(10)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
import threading
buf = bytearray()
def rd():
    while True:
        try: d = s.recv(65536)
        except Exception: break
        if not d: break
        buf.extend(d)
t = threading.Thread(target=rd); t.start()
i = 0
while i < len(reqs):
    k = random.choice([1, 2, 3, 5, 7, 13, 64, 500, 3000])
    s.sendall(reqs[i:i+k]); i += k
    if random.random() < 0.02: time.sleep(0.001)
t.join()
ok = buf.count(b"HTTP/1.1 200 OK"); nf = buf.count(b"HTTP/1.1 404"); hello = buf.count(b"Hello, world\n")
print(f"sent={n}+1 got_200={ok} got_404={nf} hello_bodies={hello} -> {'PASS' if ok==n and nf==1 and hello==n else 'FAIL'}")
