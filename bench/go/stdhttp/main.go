// What a Go programmer writes with net/http: one handler, the standard server. net/http reads the next request of a connection only after the
// previous one has been answered, so pipelined requests are served one at a time (no concurrency within a connection) - that is Go's design, not a flaw
// of this program. Same routes as the other servers; a panic in a handler (GET /boom) is recovered and answered with a 500 so the connection survives.
//
//	stdhttp port=18080 workers=1 work=0 sleep_us=0 slow=1250000
package main

import (
	"fmt"
	"net/http"
	"os"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

var (
	work    uint32
	sleepUS int
	slow    uint32 = 1250000
	sink    uint64
)

func spinWork(n uint32) {
	x := uint64(1)
	for i := uint32(0); i < n; i++ {
		x = x*6364136223846793005 + 1442695040888963407
	}
	atomic.AddUint64(&sink, x)
}

func recoverTo500(next http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		defer func() {
			if e := recover(); e != nil {
				http.Error(w, "internal error", 500)
			}
		}()
		next(w, r)
	}
}

func handle(w http.ResponseWriter, r *http.Request) {
	if work > 0 {
		spinWork(work)
	}
	p := r.URL.Path
	us := sleepUS
	var echoN uint64
	isEcho := strings.HasPrefix(p, "/echo/") && len(p) > 6
	if isEcho {
		echoN, _ = strconv.ParseUint(p[6:], 10, 32)
		if s := r.URL.Query().Get("s"); s != "" {
			if v, err := strconv.Atoi(s); err == nil && v > 0 {
				us = v
			}
		}
	}
	if p == "/slow" && r.Method == "GET" {
		spinWork(slow)
	}
	if us > 0 {
		time.Sleep(time.Duration(us) * time.Microsecond)
	}
	switch {
	case p == "/boom" && r.Method == "GET":
		panic("boom")
	case isEcho:
		fmt.Fprintf(w, "%d\n", echoN)
	case p == "/" || (p == "/slow" && r.Method == "GET"):
		w.Header().Set("Content-Type", "text/plain")
		w.Write([]byte("Hello, world\n"))
	default:
		http.NotFound(w, r)
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
	w, _ := strconv.Atoi(arg("work", "0"))
	work = uint32(w)
	sleepUS, _ = strconv.Atoi(arg("sleep_us", "0"))
	s, _ := strconv.Atoi(arg("slow", "1250000"))
	slow = uint32(s)
	runtime.GOMAXPROCS(workers)
	fmt.Fprintf(os.Stderr, "go stdhttp: port=%s GOMAXPROCS=%d\n", port, workers)
	srv := &http.Server{Addr: ":" + port, Handler: recoverTo500(handle), MaxHeaderBytes: 16384} // net/http applies this loosely: with 12288 it still accepted a 20 KB header block and refused 60 KB (431)
	if err := srv.ListenAndServe(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}
