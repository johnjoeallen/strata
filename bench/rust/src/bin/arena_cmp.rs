// Group 2 comparator: slab::Slab<[u64;7]> (64-byte entries) running the same workloads as arena_bench.c.
// usage: arena_cmp <fill|churn|ramp|mixed> <n> [random|fifo|group]
use slab::Slab;
use std::hint::black_box;
use std::time::Instant;

type T = [u64; 7];

fn rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").unwrap().lines()
        .find(|l| l.starts_with("VmRSS:")).and_then(|l| l.split_whitespace().nth(1))
        .and_then(|v| v.parse().ok()).unwrap_or(-1)
}
static mut RNG: u64 = 88172645463325252;
fn rnd() -> u64 {
    unsafe { let mut x = RNG; x ^= x << 13; x ^= x >> 7; x ^= x << 17; RNG = x; x }
}
fn mb(b: f64) -> f64 { b / 1048576.0 }
fn rss_delta(base: i64) -> f64 { (rss_kb() - base) as f64 / 1024.0 }
fn report(tag: &str, rss: f64, live: f64) {
    println!("  {:<14} rss={:8.1} MB  live={:8.1} MB  ratio={:5.2}", tag, rss, live, if live > 0.0 { rss / live } else { 0.0 });
}
fn shuffle(v: &mut [u32]) {
    for i in (1..v.len()).rev() { let j = (rnd() % (i as u64 + 1)) as usize; v.swap(i, j); }
}
fn val(v: u64) -> T { let mut t = [0u64; 7]; t[0] = v; t }

fn main() {
    let a: Vec<String> = std::env::args().collect();
    let work = a[1].as_str();
    let n: usize = a[2].parse().unwrap();
    let pattern = a.get(3).map(|s| s.as_str()).unwrap_or("random");
    let slot = 64.0;
    println!("config: slab work={} n={} pattern={}", work, n, pattern);
    match work {
        "fill" => {
            let mut s: Slab<T> = Slab::new();
            let mut keys = vec![1u32; n];
            let base = rss_kb();
            let t0 = Instant::now();
            for i in 0..n { keys[i] = s.insert(val(i as u64)) as u32; }
            let t1 = t0.elapsed();
            let rss_full = rss_delta(base);
            shuffle(&mut keys);
            let t2 = Instant::now();
            let mut bad = 0u64;
            for &k in &keys { let v = s.remove(k as usize); bad += black_box(v[0]) & 0; }
            let t3 = t2.elapsed();
            println!("fill: alloc+touch {:.1} ns/op, free (random order) {:.1} ns/op, corrupt={}",
                t1.as_nanos() as f64 / n as f64, t3.as_nanos() as f64 / n as f64, bad);
            report("full", rss_full, mb(n as f64 * slot));
            report("after free all", rss_delta(base), 0.0);
        }
        "churn" => {
            let mut s: Slab<T> = Slab::new();
            let mut keys = vec![1u32; n];
            let base = rss_kb();
            for i in 0..n { keys[i] = s.insert(val(i as u64)) as u32; }
            let ops = 4 * n;
            let t0 = Instant::now();
            for _ in 0..ops {
                let i = (rnd() % n as u64) as usize;
                let v = s.remove(keys[i] as usize);
                black_box(v[0]);
                keys[i] = s.insert(val(i as u64)) as u32;
            }
            println!("churn: {:.1} ns per free+alloc pair", t0.elapsed().as_nanos() as f64 / ops as f64);
            report("after churn", rss_delta(base), mb(n as f64 * slot));
        }
        "ramp" => {
            let mut s: Slab<T> = Slab::new();
            let mut keys = vec![1u32; 2 * n];
            let mut alive = vec![true; 2 * n];
            let mut ord = vec![1u32; n];
            let base = rss_kb();
            for i in 0..n { keys[i] = s.insert(val(i as u64)) as u32; }
            let rss_full = rss_delta(base);
            let mut freed = 0usize;
            match pattern {
                "random" => {
                    for i in 0..n { ord[i] = i as u32; }
                    shuffle(&mut ord);
                    for k in 0..n * 9 / 10 { let i = ord[k] as usize; s.remove(keys[i] as usize); alive[i] = false; freed += 1; }
                }
                "fifo" => { for i in 0..n * 9 / 10 { s.remove(keys[i] as usize); alive[i] = false; freed += 1; } }
                _ => { for i in 0..n { if (i / 100) % 10 != 0 { s.remove(keys[i] as usize); alive[i] = false; freed += 1; } } }
            }
            let live = n - freed;
            let rss_freed = rss_delta(base);
            let m = n / 2;
            for i in 0..m { keys[n + i] = s.insert(val((n + i) as u64)) as u32; }
            let rss_refill = rss_delta(base);
            println!("ramp({}): live after free={} ({:.0}%)", pattern, live, 100.0 * live as f64 / n as f64);
            report("full", rss_full, mb(n as f64 * slot));
            report("after free 90%", rss_freed, mb(live as f64 * slot));
            report("after refill", rss_refill, mb((live + m) as f64 * slot));
        }
        "mixed" => {
            let steps = 5 * n;
            let mut s: Slab<T> = Slab::new();
            let mut ring = vec![1u32; n];
            let mut lng: Vec<u32> = Vec::with_capacity(steps / 40);
            let (mut head, mut count) = (0usize, 0usize);
            let base = rss_kb();
            let t0 = Instant::now();
            for k in 0..steps {
                let key = s.insert(val(k as u64)) as u32;
                if rnd() % 50 == 0 && lng.len() < steps / 40 { lng.push(key); continue; }
                if count == n { s.remove(ring[head] as usize); ring[head] = key; head = (head + 1) % n; }
                else { ring[(head + count) % n] = key; count += 1; }
            }
            println!("mixed: {:.1} ns/step, short live={}, immortal={}", t0.elapsed().as_nanos() as f64 / steps as f64, count, lng.len());
            report("steady state", rss_delta(base), mb((count + lng.len()) as f64 * slot));
            for i in 0..count { s.remove(ring[(head + i) % n] as usize); }
            report("after burst", rss_delta(base), mb(lng.len() as f64 * slot));
        }
        "compact" => {
            let (m, nsteps) = (n * 7 / 10, 6 * n);
            let mut s: Slab<T> = Slab::new();
            let mut keys = vec![1u32; n];
            let mut ord = vec![1u32; n];
            let mut lng: Vec<u32> = Vec::with_capacity(nsteps / 40);
            let mut sh: Vec<u32> = vec![1u32; m];
            lng.resize(nsteps / 40, 1);
            lng.clear();
            let base = rss_kb();
            for i in 0..n { keys[i] = s.insert(val(i as u64)) as u32; ord[i] = i as u32; }
            shuffle(&mut ord);
            for i in 0..n { let k = keys[ord[i] as usize]; if i < m { sh[i] = k } else { s.remove(k as usize); } }
            println!("compact: n={}, live short={} after 30% random kill", n, m);
            report("after kill", rss_delta(base), mb(m as f64 * slot));
            let t0 = Instant::now();
            for k in 1..=nsteps {
                let i = (rnd() % m as u64) as usize;
                let v = s.remove(sh[i] as usize); black_box(v[0]);
                sh[i] = s.insert(val(k as u64)) as u32;
                if rnd() % 50 == 0 && lng.len() < nsteps / 40 { lng.push(s.insert(val(k as u64)) as u32); }
                if k == n || k == 3 * n || k == nsteps {
                    report(&format!("churn {}n", k / n), rss_delta(base), mb((m + lng.len()) as f64 * slot));
                }
            }
            println!("  {:.1} ns/step", t0.elapsed().as_nanos() as f64 / nsteps as f64);
            for &k in &sh { s.remove(k as usize); }
            report("after burst", rss_delta(base), mb(lng.len() as f64 * slot));
        }
        _ => panic!("unknown work"),
    }
}
