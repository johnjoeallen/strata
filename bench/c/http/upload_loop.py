# Keep uploading 200 MB (chunked) bodies in a loop until told to stop. usage: upload_loop.py port seconds
import socket, os, sys, time, re, zlib
port, secs = int(sys.argv[1]), float(sys.argv[2])
block = os.urandom(1 << 20); n = 0; t_end = time.time() + secs
while time.time() < t_end:
    s = socket.create_connection(("127.0.0.1", port)); s.settimeout(30)
    s.sendall(b"POST /upload HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n")
    for i in range(200): s.sendall(b"%x\r\n" % len(block) + block + b"\r\n")
    s.sendall(b"0\r\n\r\n"); s.recv(4096); s.close(); n += 1
print(f"uploads completed: {n}")
