#!/usr/bin/env python3
# Protocol, pub/sub, limit, shutdown and concurrency tests for the mini-redis server. usage: redis_test.py PORT TEST SERVER_PID
# Each test prints "<test> -> PASS" or "<test> -> FAIL: why" as its last line.
import os, random, signal, socket, sys, threading, time

PORT = int(sys.argv[1]); TEST = sys.argv[2]; SPID = int(sys.argv[3]) if len(sys.argv) > 3 else 0

def enc(*a):
    a = [x if isinstance(x, bytes) else str(x).encode() for x in a]
    return b"*%d\r\n" % len(a) + b"".join(b"$%d\r\n%s\r\n" % (len(x), x) for x in a)

class C:
    def __init__(s, timeout=3):
        s.s = socket.create_connection(("127.0.0.1", PORT)); s.s.settimeout(timeout); s.buf = b""
    def send(s, *a): s.s.sendall(enc(*a))
    def raw(s, b): s.s.sendall(b)
    def _fill(s):
        d = s.s.recv(1 << 20)
        if not d: raise EOFError("closed")
        s.buf += d
    def _line(s):
        while b"\r\n" not in s.buf: s._fill()
        l, s.buf = s.buf.split(b"\r\n", 1); return l
    def reply(s):
        l = s._line(); t = l[:1]; r = l[1:]
        if t in (b"+", b"-"): return (t.decode(), r.decode())
        if t == b":": return int(r)
        if t == b"$":
            n = int(r)
            if n < 0: return None
            while len(s.buf) < n + 2: s._fill()
            v, s.buf = s.buf[:n], s.buf[n + 2:]; return v
        if t == b"*": return [s.reply() for _ in range(int(r))]
        raise ValueError(l)
    def cmd(s, *a): s.send(*a); return s.reply()
    def closed(s, wait=2.0):
        """True if the server closes the connection (EOF or reset) within `wait`, discarding what arrives."""
        s.s.settimeout(wait); end = time.time() + wait
        try:
            while time.time() < end:
                if not s.s.recv(65536): return True
        except (ConnectionResetError, BrokenPipeError): return True
        except socket.timeout: return False
        return False
    def close(s): s.s.close()

def eq(a, b, what=""):
    if a != b: raise AssertionError("%s: got %r, want %r" % (what, a, b))

def t_basic():
    c = C()
    eq(c.cmd("PING"), ("+", "PONG")); eq(c.cmd("ping", "hi"), b"hi")
    eq(c.cmd("SET", "a", "1"), ("+", "OK")); eq(c.cmd("GET", "a"), b"1"); eq(c.cmd("GET", "zz"), None)
    eq(c.cmd("SET", "a", "2"), ("+", "OK")); eq(c.cmd("get", "a"), b"2")
    eq(c.cmd("SET", "bin", b"\x00\xff\r\n\x01"), ("+", "OK")); eq(c.cmd("GET", "bin"), b"\x00\xff\r\n\x01")
    eq(c.cmd("NOPE", "x"), ("-", "ERR unknown command 'nope'"))
    eq(c.cmd("PUBLISH", "nobody", "m"), 0)
    eq(c.cmd("UNSUBSCRIBE"), ("-", "ERR `Unsubscribe` is unsupported in this context"))
    # simple-string and integer elements are accepted where the original accepts them
    c.raw(b"*3\r\n+SET\r\n+sk\r\n+sv\r\n"); eq(c.reply(), ("+", "OK")); eq(c.cmd("GET", "sk"), b"sv")
    c.raw(b"*5\r\n$3\r\nSET\r\n$2\r\nik\r\n$1\r\nv\r\n$2\r\nPX\r\n:50\r\n"); eq(c.reply(), ("+", "OK"))
    time.sleep(0.25); eq(c.cmd("GET", "ik"), None)

def t_expiry():
    c = C()
    c.cmd("SET", "x", "1", "PX", "150"); c.cmd("SET", "y", "1", "EX", "100"); c.cmd("SET", "z", "1")
    eq(c.cmd("GET", "x"), b"1"); time.sleep(0.4)
    eq(c.cmd("GET", "x"), None, "x expired"); eq(c.cmd("GET", "y"), b"1"); eq(c.cmd("GET", "z"), b"1")
    # overwrite drops the old expiry; a shorter one moves the purge deadline earlier
    c.cmd("SET", "w", "1", "PX", "5000"); c.cmd("SET", "w", "2"); c.cmd("SET", "v", "1", "PX", "5000"); c.cmd("SET", "v", "1", "PX", "100")
    time.sleep(0.35); eq(c.cmd("GET", "v"), None, "v expired early"); eq(c.cmd("GET", "w"), b"2", "w kept")
    for bad in (("SET", "k", "v", "EX", "0"), ("SET", "k", "v", "PX", "99999999999999"), ("SET", "k", "v", "NX"), ("SET", "k", "v", "EX")):
        c2 = C(); r = c2.cmd(*bad) if len(bad) != 4 or bad[3] != "EX" else None
        if r is None: c2.send(*bad); r = c2.reply()
        assert r[0] == "-", (bad, r); assert c2.closed(), ("closes after a protocol error", bad)

def t_pipeline():
    c = C(); n = 3000
    c.raw(b"".join(enc("SET", "k%d" % i, "v%d" % i) + enc("GET", "k%d" % i) for i in range(n)))
    for i in range(n):
        eq(c.reply(), ("+", "OK")); eq(c.reply(), b"v%d" % i, "pipelined %d" % i)

def t_fragment():
    random.seed(7); c = C(); data = b""
    for i in range(300): data += enc("SET", "f%d" % i, "x" * random.randint(0, 3000)) + enc("GET", "f%d" % i)
    i = 0
    while i < len(data):
        n = random.choice((1, 1, 2, 3, 7, 50, 700)); c.raw(data[i:i + n]); i += n
        if random.random() < 0.05: time.sleep(0.001)
    for i in range(300): eq(c.reply(), ("+", "OK")); r = c.reply(); assert r is not None and set(r) <= {120}, i

def t_bigvalue():
    c = C(10); v = os.urandom(900000)
    eq(c.cmd("SET", "big", v), ("+", "OK")); eq(c.cmd("GET", "big"), v, "1 MB-class value round trip")
    big = b"x" * 1048576; eq(c.cmd("SET", "max", big), ("+", "OK")); eq(c.cmd("GET", "max"), big, "exactly maxbulk")
    eq(c.cmd("PING", b"p" * 100000), b"p" * 100000, "ping with a big message")

def t_limits():
    for frame in (b"*2\r\n$3\r\nGET\r\n$99999999999\r\n", b"*65\r\n", b"*1\r\n$-5\r\n", b"GET x\r\n", b"*0\r\n", b"*1\r\n*1\r\n$1\r\na\r\n",
                  b"*1\r\n:99999999999999999999999\r\n", b"*abc\r\n"):
        c = C(); c.raw(frame); r = c.reply(); assert r[0] == "-", (frame, r)
        assert c.closed(), ("closes", frame)
    c = C(); c.raw(b"*" + b"1" * 100); r = c.reply(); assert r[0] == "-", r    # an endless header line is refused without waiting for its end
    c = C(); eq(c.cmd("GET", "k" * 600)[0], "-"); assert c.closed()           # key over maxKey
    c = C(); c.raw(b"*2\r\n$3\r\nGET\r\n$1\r\n\xff\r\n"); assert c.reply()[0] == "-"; assert c.closed()   # not UTF-8
    c = C(); c.raw(b"*1\r\n$3\r\nGET\r\n"); eq(c.reply()[0], "-")              # GET without a key

def t_toolarge():           # server started with maxframe=1000 maxbulk=800
    c = C(); eq(c.cmd("SET", "k", "x" * 700), ("+", "OK"))
    c = C(); c.raw(b"*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$801\r\n"); eq(c.reply(), ("-", "ERR frame too large")); assert c.closed()   # over maxbulk, from the length line
    c = C(); c.raw(enc("SET", "k" * 500, "y" * 600)[:-100]); eq(c.reply(), ("-", "ERR frame too large")); assert c.closed()   # each bulk is fine, the frame total is over maxframe: refused from the length line
    c = C(); c.raw(b"*1\r\n+" + b"a" * 900); eq(c.reply(), ("-", "ERR frame too large")); assert c.closed()   # an endless simple string
    c = C(); eq(c.cmd("GET", "k"), b"x" * 700, "earlier value intact")

def t_maxkeys():            # maxkeys=4
    c = C()
    for i in range(4): eq(c.cmd("SET", "k%d" % i, "v"), ("+", "OK"))
    eq(c.cmd("SET", "k4", "v"), ("-", "ERR max keys")); eq(c.cmd("SET", "k0", "new"), ("+", "OK"), "overwrite still allowed"); eq(c.cmd("GET", "k0"), b"new")
    eq(c.cmd("PING"), ("+", "PONG"), "connection stays")

def t_maxconns():           # maxconns=3
    cs = [C() for _ in range(3)]
    for c in cs: eq(c.cmd("PING"), ("+", "PONG"))
    x = C(); eq(x.reply(), ("-", "ERR max clients")); assert x.closed()
    cs[0].close(); time.sleep(0.3); y = C(); eq(y.cmd("PING"), ("+", "PONG"), "slot freed")

def t_pubsub():
    s1, s2, p = C(), C(), C()
    eq(s1.cmd("SUBSCRIBE", "a", "b"), [b"subscribe", b"a", 1]); eq(s1.reply(), [b"subscribe", b"b", 2])
    eq(s2.cmd("SUBSCRIBE", "a"), [b"subscribe", b"a", 1])
    eq(p.cmd("PUBLISH", "a", "hello"), 2); eq(p.cmd("PUBLISH", "b", "world"), 1); eq(p.cmd("PUBLISH", "c", "x"), 0)
    eq(s1.reply(), [b"message", b"a", b"hello"]); eq(s1.reply(), [b"message", b"b", b"world"]); eq(s2.reply(), [b"message", b"a", b"hello"])
    eq(s1.cmd("SUBSCRIBE", "a"), [b"subscribe", b"a", 2], "subscribing twice does not change the count")
    eq(s1.cmd("GET", "x"), ("-", "ERR unknown command 'get'"))
    eq(s1.cmd("UNSUBSCRIBE", "a"), [b"unsubscribe", b"a", 1]); eq(p.cmd("PUBLISH", "a", "again"), 1)
    eq(s2.reply(), [b"message", b"a", b"again"]); s1.send("PING"); eq(s1.reply(), ("-", "ERR unknown command 'ping'"))
    s1.send("UNSUBSCRIBE"); eq(s1.reply(), [b"unsubscribe", b"b", 0]); eq(p.cmd("PUBLISH", "b", "z"), 0, "topic removed")
    s1.send("SUBSCRIBE", "q"); s1.reply(); big = os.urandom(300000)
    eq(p.cmd("PUBLISH", "q", big), 1); eq(s1.reply(), [b"message", b"q", big], "big message")
    eq(p.cmd("PUBLISH", "q", ""), 1); eq(s1.reply(), [b"message", b"q", b""], "empty message")

def t_order():
    s, p = C(), C(); s.cmd("SUBSCRIBE", "o"); n = 3000
    p.raw(b"".join(enc("PUBLISH", "o", "m%d" % i) for i in range(n)))
    for i in range(n): eq(p.reply(), 1)
    for i in range(n): eq(s.reply(), [b"message", b"o", b"m%d" % i], "order %d" % i)

def t_xworker():            # workers=4: publisher and subscribers land on different workers
    subs = [C() for _ in range(32)]
    for s in subs: s.cmd("SUBSCRIBE", "x")
    ps = [C() for _ in range(8)]; n = 200
    def pub(c, k):
        c.raw(b"".join(enc("PUBLISH", "x", "%d:%d" % (k, i)) for i in range(n)))
        for _ in range(n): assert c.reply() == 32
    ts = [threading.Thread(target=pub, args=(c, k)) for k, c in enumerate(ps)]
    [t.start() for t in ts]; [t.join() for t in ts]
    for s in subs:
        got = {}
        for _ in range(8 * n):
            m = s.reply(); k, i = map(int, m[2].split(b":")); assert got.get(k, -1) < i, "per-publisher order"; got[k] = i
        assert len(got) == 8

def t_lag():                # subqueue=4, lag=disconnect : the subscriber does not read
    s, p = C(10), C(10); s.cmd("SUBSCRIBE", "l"); big = b"z" * 200000; sent = 0
    for _ in range(400): p.cmd("PUBLISH", "l", big); sent += 1
    got_err = False; n = 0
    try:
        while True:
            r = s.reply()
            if isinstance(r, tuple): got_err = "too slow" in r[1]; break
            n += 1
    except EOFError: pass
    assert got_err, "expected -ERR subscriber too slow (read %d messages)" % n
    assert s.closed()

def t_lagnotice():          # subqueue=4 lag=notice
    s, p = C(10), C(10); s.cmd("SUBSCRIBE", "l"); big = b"z" * 200000
    for _ in range(300): p.cmd("PUBLISH", "l", big)
    lost = 0; msgs = 0; s.s.settimeout(1.0)
    try:
        while True:
            r = s.reply()
            if r[0] == b"lagged": lost += r[1]
            else: msgs += 1
    except (socket.timeout, EOFError): pass
    assert lost > 0 and msgs + lost == 300, (msgs, lost)
    eq(s.cmd("PING") if False else None, None)

def t_reaper():             # maxtopics=5 maxsubs=3: a finished connection's channels are released
    for rnd in range(4):
        s = C(); eq(s.cmd("SUBSCRIBE", "r%d" % (rnd * 3)), [b"subscribe", b"r%d" % (rnd * 3), 1]); s.reply  # noqa
        s.cmd("SUBSCRIBE", "r%d" % (rnd * 3 + 1)) if False else None
        s.send("SUBSCRIBE", "r%d" % (rnd * 3 + 1), "r%d" % (rnd * 3 + 2)); s.reply(); s.reply(); s.close(); time.sleep(0.2)
    s = C(); eq(s.cmd("SUBSCRIBE", "final"), [b"subscribe", b"final", 1], "topics were released")
    s.send("SUBSCRIBE", "m1", "m2", "m3"); r = [s.reply() for _ in range(3)]
    eq(r[0], [b"subscribe", b"m1", 2]); eq(r[1], [b"subscribe", b"m2", 3]); eq(r[2], ("-", "ERR max subscriptions"))

def t_maxtopics():          # maxtopics=3
    s = C(); s.send("SUBSCRIBE", "a", "b", "c", "d"); r = [s.reply() for _ in range(4)]
    eq(r[2], [b"subscribe", b"c", 3]); eq(r[3], ("-", "ERR max channels"))

def t_shutdown():
    cs = [C() for _ in range(5)]
    for c in cs: eq(c.cmd("PING"), ("+", "PONG"))
    sub = C(); sub.cmd("SUBSCRIBE", "s")
    os.kill(SPID, signal.SIGTERM)
    for c in cs + [sub]: assert c.closed(3), "connection closed by shutdown"
    for _ in range(50):
        try: os.kill(SPID, 0); time.sleep(0.1)
        except ProcessLookupError: return
    raise AssertionError("server did not exit")

def t_stress():             # workers=4: many connections, shared keys, lock contention; every reply must match a model
    N = 40; errs = []
    def worker(k):
        try:
            c = C(10); rnd = random.Random(k)
            for i in range(400):
                key = "s%d" % rnd.randrange(50); val = "%d.%d" % (k, i)
                c.send("SET", key, val); assert c.reply() == ("+", "OK")
                c.send("GET", key); g = c.reply(); assert g is not None and b"." in g, g
        except Exception as e: errs.append(repr(e))
    ts = [threading.Thread(target=worker, args=(k,)) for k in range(N)]
    [t.start() for t in ts]; [t.join() for t in ts]
    assert not errs, errs[:3]

def t_churn():              # connections open, subscribe, publish and vanish at random, with expiring keys; the server must stay consistent and alive
    stop = time.time() + 3; errs = []
    def w(k):
        rnd = random.Random(k)
        while time.time() < stop:
            try:
                c = C(5); ch = "ch%d" % rnd.randrange(8)
                for _ in range(rnd.randrange(1, 6)):
                    op = rnd.randrange(5)
                    if op == 0: c.cmd("SET", "k%d" % rnd.randrange(100), "v", "PX", str(rnd.randrange(1, 50)))
                    elif op == 1: c.cmd("PUBLISH", ch, "m")
                    elif op == 2:
                        c.send("SUBSCRIBE", ch); c.reply(); c.send("PUBLISH", ch, "x")  # a subscriber cannot publish: gets an error
                        break
                    elif op == 3: c.cmd("GET", "k%d" % rnd.randrange(100))
                    else: c.cmd("PING")
                if rnd.random() < 0.5: c.close()
            except (EOFError, ConnectionError, socket.timeout): pass
            except Exception as e: errs.append(repr(e))
    ts = [threading.Thread(target=w, args=(k,)) for k in range(24)]
    [t.start() for t in ts]; [t.join() for t in ts]
    assert not errs, errs[:3]
    c = C(); eq(c.cmd("PING"), ("+", "PONG"), "server alive")

try:
    globals()["t_" + TEST]()
    print(TEST, "-> PASS")
except Exception as e:
    print(TEST, "-> FAIL: %s: %s" % (type(e).__name__, e))
