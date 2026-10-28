// The Go equivalent of bench/c/http/server.stra, written the way a Go programmer would (goroutines and channels, the standard library only):
// same protocol, routes and behaviour as server.c and the Rust server.
//
//	pipe port=18080 workers=1 conc=1 depth=16 inline=1 batch=1 work=0 sleep_us=0 slow=1250000
//
//	conc=0   one request at a time, every response flushed at once                       (server.c conc=0)
//	conc=1   pipelined requests run as their own goroutines; the completed prefix of responses is sent as one write (server.c conc=1):
//	         up to `depth` in flight, POST is a barrier both ways, GET /slow is a barrier before it, a lone request runs inline,
//	         431 for a header block over 16 KB, a handler error becomes a 500, each goroutine owns a copy of the request head.
//	batch=1  after the oldest request finishes, runtime.Gosched() once so the other ready goroutines finish first and their responses
//	         go out in the same write. Without it the Go scheduler runs the woken connection goroutine next (runnext) and responses go out one by one.
//
// workers= sets GOMAXPROCS. Pin the process from outside (taskset -c 0 for one worker); Go has no portable way to pin from inside.
// Routes: GET|POST / (cheap), /echo/N?s=US (sleeps s us, then echoes N), GET /slow (~1 ms of CPU), GET /boom (handler error).
package main

import (
	"bytes"
	"fmt"
	"net"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

const maxHead = 16384

var (
	conc    = true
	depth   = 16
	inline  = true
	batch   = true
	work    uint32
	sleepUS int
	slow    uint32 = 1250000
	sink    uint64
)

// ---------------------------------------------------------------- size-class input buffers (the Go equivalent of runtime/rt_buf.h)
var classes = [...]int{512, 2048, 8192, 16384}

type bufObj struct {
	b   []byte
	cls int
}

var pools [len(classes)]sync.Pool

func getBuf(cls int) *bufObj {
	if v := pools[cls].Get(); v != nil {
		return v.(*bufObj)
	}
	return &bufObj{b: make([]byte, classes[cls]), cls: cls}
}
func putBuf(o *bufObj) { pools[o.cls].Put(o) }

// makeRoom: free space at the tail. Compacts, or moves to the next class; false at the limit.
func makeRoom(o **bufObj, start, end *int) bool {
	b := *o
	if *end < len(b.b) {
		return true
	}
	if *start > 0 {
		copy(b.b, b.b[*start:*end])
		*end -= *start
		*start = 0
		return true
	}
	if b.cls+1 >= len(classes) {
		return false
	}
	nb := getBuf(b.cls + 1)
	copy(nb.b, b.b[:*end])
	putBuf(b)
	*o = nb
	return true
}

// ---------------------------------------------------------------- responses
func reason(code int) string {
	switch code {
	case 200:
		return "OK"
	case 400:
		return "Bad Request"
	case 404:
		return "Not Found"
	case 431:
		return "Request Header Fields Too Large"
	case 500:
		return "Internal Server Error"
	}
	return "Other"
}
func mk(code int, conn, body string) []byte {
	return []byte(fmt.Sprintf("HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n%s\r\n%s", code, reason(code), len(body), conn, body))
}

var r200, r200ka, r200close, r404, r400, r431, r500 []byte

func init() {
	r200 = mk(200, "", "Hello, world\n")
	r200ka = mk(200, "Connection: keep-alive\r\n", "Hello, world\n")
	r200close = mk(200, "Connection: close\r\n", "Hello, world\n")
	r404 = mk(404, "Connection: close\r\n", "not found\n")
	r400 = mk(400, "Connection: close\r\n", "bad request\n")
	r431 = mk(431, "Connection: close\r\n", "headers too large\n")
	r500 = mk(500, "", "internal error\n")
}

type resp struct {
	static []byte
	echo   bool
	n      uint32
	keep   bool
}

func appendResp(out []byte, r resp) []byte {
	if !r.echo {
		return append(out, r.static...)
	}
	body := strconv.AppendUint(nil, uint64(r.n), 10)
	body = append(body, '\n')
	out = append(out, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "...)
	out = strconv.AppendInt(out, int64(len(body)), 10)
	out = append(out, "\r\n"...)
	if !r.keep {
		out = append(out, "Connection: close\r\n"...)
	}
	out = append(out, "\r\n"...)
	return append(out, body...)
}

// ---------------------------------------------------------------- request parsing and routing
var crlf2 = []byte("\r\n\r\n")

func connHeader(b []byte) (closeH, ka bool) {
	i := bytes.IndexByte(b, '\n')
	for i >= 0 && i+1 < len(b) {
		line := b[i+1:]
		e := bytes.IndexByte(line, '\n')
		last := e < 0
		if last {
			e = len(line)
		}
		l := line[:e]
		if len(l) > 11 && bytes.EqualFold(l[:11], []byte("connection:")) {
			v := bytes.ToLower(l[11:])
			return bytes.Contains(v, []byte("close")), bytes.Contains(v, []byte("keep-alive"))
		}
		if last {
			break
		}
		i += 1 + e
	}
	return
}

type kind int

const (
	kIndex kind = iota
	kEcho
	kSlow
	kBoom
	kNotFound
	kBad
)

type route struct {
	k      kind
	post   bool
	keep   bool
	http11 bool
}

func lookup(b []byte) route {
	off := 0
	if bytes.HasPrefix(b, []byte("POST ")) {
		off = 5
	} else if bytes.HasPrefix(b, []byte("GET ")) {
		off = 4
	}
	if len(b) < 14 || off == 0 {
		return route{k: kBad}
	}
	rest := b[off:]
	sp := bytes.IndexByte(rest, ' ')
	if sp < 0 {
		return route{k: kBad}
	}
	path := rest[:sp]
	http11 := len(rest) >= sp+9 && string(rest[sp+1:sp+9]) == "HTTP/1.1"
	closeH, ka := connHeader(b)
	keep := ka
	if http11 {
		keep = !closeH
	}
	post := off == 5
	var k kind
	switch {
	case string(path) == "/":
		k = kIndex
	case !post && string(path) == "/slow":
		k = kSlow
	case !post && string(path) == "/boom":
		k = kBoom
	case len(path) > 6 && bytes.HasPrefix(path, []byte("/echo/")):
		k = kEcho
	default:
		return route{k: kNotFound, post: post, http11: http11}
	}
	return route{k, post, keep, http11}
}

func parseEcho(head []byte) (n uint32, us int, ok bool) {
	off := 0
	if bytes.HasPrefix(head, []byte("POST ")) {
		off = 5
	} else if bytes.HasPrefix(head, []byte("GET ")) {
		off = 4
	} else {
		return
	}
	p := head[off:]
	if !bytes.HasPrefix(p, []byte("/echo/")) {
		return
	}
	p = p[6:]
	i := 0
	for i < len(p) && p[i] >= '0' && p[i] <= '9' {
		n = n*10 + uint32(p[i]-'0')
		i++
	}
	ok = true
	if bytes.HasPrefix(p[i:], []byte("?s=")) {
		for j := i + 3; j < len(p) && p[j] >= '0' && p[j] <= '9'; j++ {
			us = us*10 + int(p[j]-'0')
		}
	}
	return
}

func spinWork(n uint32) {
	x := uint64(1)
	for i := uint32(0); i < n; i++ {
		x = x*6364136223846793005 + 1442695040888963407
	}
	atomic.AddUint64(&sink, x)
}

// ---------------------------------------------------------------- handlers
// A handler can fail (error); runHandler turns that into a 500 and the connection survives.
func handler(rt route, head []byte) (resp, error) {
	if work > 0 {
		spinWork(work)
	}
	if rt.k == kSlow {
		spinWork(slow)
	}
	us := sleepUS
	if rt.k == kEcho {
		if _, es, ok := parseEcho(head); ok && es > 0 {
			us = es
		}
	}
	if us > 0 {
		time.Sleep(time.Duration(us) * time.Microsecond)
	}
	switch rt.k {
	case kBoom:
		return resp{}, fmt.Errorf("boom")
	case kEcho:
		n, _, _ := parseEcho(head) // reads the head after waiting
		return resp{echo: true, n: n, keep: rt.keep}, nil
	case kIndex, kSlow:
		switch {
		case !rt.keep:
			return resp{static: r200close}, nil
		case rt.http11:
			return resp{static: r200}, nil
		}
		return resp{static: r200ka}, nil
	case kNotFound:
		return resp{static: r404}, nil
	}
	return resp{static: r400}, nil
}
func runHandler(rt route, head []byte) resp {
	r, err := handler(rt, head)
	if err != nil {
		return resp{static: r500}
	}
	return r
}

// ---------------------------------------------------------------- conc=0: one request at a time
func serveSeq(c net.Conn) {
	defer c.Close()
	var o *bufObj
	start, end := 0, 0
	defer func() {
		if o != nil {
			putBuf(o)
		}
	}()
	var out []byte
	for {
		var idx int
		for {
			if o != nil && end > start {
				if i := bytes.Index(o.b[start:end], crlf2); i >= 0 {
					idx = start + i + 4
					break
				}
			}
			if o == nil {
				o = getBuf(0)
				start, end = 0, 0
			}
			if !makeRoom(&o, &start, &end) {
				c.Write(r431)
				return
			}
			n, err := c.Read(o.b[end:])
			end += n
			if err != nil && n == 0 {
				return
			}
		}
		head := o.b[start:idx]
		rt := lookup(head)
		r := runHandler(rt, head)
		out = appendResp(out[:0], r)
		if _, err := c.Write(out); err != nil {
			return
		}
		start = idx
		if !rt.keep {
			return
		}
		if start == end {
			putBuf(o)
			o = nil
			start, end = 0, 0
		}
	}
}

// ---------------------------------------------------------------- conc=1: pipelined, concurrent, responses in order
type slot struct {
	r    resp
	done chan struct{} // nil: already complete (an inline request or an error answer)
}

func isDone(s *slot) bool {
	if s.done == nil {
		return true
	}
	select {
	case <-s.done:
		return true
	default:
		return false
	}
}

func serveConc(c net.Conn) {
	defer c.Close()
	var o *bufObj
	start, end := 0, 0
	defer func() {
		if o != nil {
			putBuf(o)
		}
	}()
	var inflight []*slot
	unsafeBusy, closing, eof := false, false, false
	var out []byte
	for {
		// (a) collect the completed prefix, in request order
		out = out[:0]
		for len(inflight) > 0 && isDone(inflight[0]) {
			out = appendResp(out, inflight[0].r)
			inflight = inflight[1:]
		}
		if len(inflight) == 0 {
			unsafeBusy = false
		}
		// (b) send it as one write
		if len(out) > 0 {
			if _, err := c.Write(out); err != nil {
				return
			}
			continue
		}
		// (c) admit new requests
	admit:
		for !closing && len(inflight) < depth && !unsafeBusy {
			var i int = -1
			if o != nil && end > start {
				i = bytes.Index(o.b[start:end], crlf2)
			}
			if i < 0 {
				if eof || len(inflight) > 0 { // never block on the socket while requests are in flight
					break admit
				}
				if o == nil {
					o = getBuf(0)
					start, end = 0, 0
				}
				if !makeRoom(&o, &start, &end) {
					inflight = append(inflight, &slot{r: resp{static: r431}})
					closing = true
					break admit
				}
				n, err := c.Read(o.b[end:])
				end += n
				if err != nil {
					eof = true
				}
				continue
			}
			hdrEnd := start + i + 4
			head := o.b[start:hdrEnd]
			rt := lookup(head)
			if (rt.post || rt.k == kSlow) && len(inflight) > 0 {
				break admit // barrier: earlier requests finish and are sent first
			}
			hasNext := bytes.Contains(o.b[hdrEnd:end], crlf2)
			if !rt.keep {
				closing = true
			}
			if rt.post {
				unsafeBusy = true
			}
			if inline && len(inflight) == 0 && !hasNext {
				inflight = append(inflight, &slot{r: runHandler(rt, head)}) // inline: runs in this goroutine
			} else {
				own := append([]byte(nil), head...) // the goroutine owns a copy of the head
				s := &slot{done: make(chan struct{})}
				go func() { s.r = runHandler(rt, own); close(s.done) }()
				inflight = append(inflight, s)
			}
			start = hdrEnd
		}
		if o != nil && start == end { // idle: no buffer
			putBuf(o)
			o = nil
			start, end = 0, 0
		}
		// (d) finished, or wait for the oldest request
		if len(inflight) == 0 {
			if closing || eof {
				return
			}
			continue
		}
		if !isDone(inflight[0]) {
			<-inflight[0].done
			if batch {
				runtime.Gosched() // let the other ready goroutines finish first, so their responses go out in the same write
			}
		}
	}
}

func arg(k, d string) string {
	p := k + "="
	for _, a := range os.Args[1:] {
		if len(a) > len(p) && a[:len(p)] == p {
			return a[len(p):]
		}
	}
	return d
}

func main() {
	port := arg("port", "18080")
	workers, _ := strconv.Atoi(arg("workers", "1"))
	conc = arg("conc", "1") == "1"
	depth, _ = strconv.Atoi(arg("depth", "16"))
	if depth < 1 {
		depth = 1
	} else if depth > 32 {
		depth = 32
	}
	inline = arg("inline", "1") == "1"
	batch = arg("batch", "1") == "1"
	w, _ := strconv.Atoi(arg("work", "0"))
	work = uint32(w)
	sleepUS, _ = strconv.Atoi(arg("sleep_us", "0"))
	s, _ := strconv.Atoi(arg("slow", "1250000"))
	slow = uint32(s)
	runtime.GOMAXPROCS(workers)
	ln, err := net.Listen("tcp", ":"+port)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	fmt.Fprintf(os.Stderr, "go pipe: port=%s GOMAXPROCS=%d conc=%v depth=%d inline=%v batch=%v\n", port, workers, conc, depth, inline, batch)
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		if conc {
			go serveConc(c)
		} else {
			go serveSeq(c)
		}
	}
}
