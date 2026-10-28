import socket, sys, time
port, nreq = int(sys.argv[1]), int(sys.argv[2])
s = socket.create_connection(("127.0.0.1", port)); s.settimeout(3)
s.sendall(b"GET / HTTP/1.1\r\n\r\n" * nreq)        # all requests in one write
buf = b""
try:
    while True:
        d = s.recv(65536)
        if not d: break
        buf += d
except socket.timeout:
    buf += b"<timeout>"
print(f"sent={nreq} responses={buf.count(b'HTTP/1.1 200 OK')} closed={'<timeout>' not in buf.decode(errors='ignore')}")
