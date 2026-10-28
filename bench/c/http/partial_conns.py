# Hold N connections open, each with a PARTIAL request header of `size` bytes (no terminator). usage: partial_conns.py port N size hold_seconds
import socket, sys, time, resource
port, n, size, hold = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), float(sys.argv[4])
resource.setrlimit(resource.RLIMIT_NOFILE, (200000, 200000))
payload = (b"GET / HTTP/1.1\r\nX-Pad: " + b"a" * max(0, size - 24))[:size]
socks = []
for i in range(n):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
    s.bind(("127.1.%d.1" % (i % 8), 0)); s.connect(("127.0.0.1", port)); s.sendall(payload); socks.append(s)
time.sleep(1.0)
print("READY", flush=True)
time.sleep(hold)
