// Group 1 comparator: Tokio tasks as the "idle connection" unit.
// usage: tokio_idle <ntasks> <footprint_bytes> <rounds> [threads]
//   footprint_bytes: state held across an await (what an idle task keeps alive)
use std::hint::black_box;
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::sync::Arc;
use std::time::Instant;

fn status_kb(key: &str) -> i64 {
    std::fs::read_to_string("/proc/self/status")
        .unwrap()
        .lines()
        .find(|l| l.starts_with(key))
        .and_then(|l| l.split_whitespace().nth(1))
        .and_then(|v| v.parse().ok())
        .unwrap_or(-1)
}

fn vmas() -> i64 {
    std::fs::read_to_string("/proc/self/maps").unwrap().lines().count() as i64
}

// Footprint is a const generic so it lives inside the future, like a stack frame would.
async fn task<const N: usize>(rounds: usize, started: Arc<AtomicUsize>, yields: Arc<AtomicUsize>) {
    let mut buf = [0u8; N];
    for i in (0..N).step_by(64) {
        buf[i] = 1;
    }
    black_box(&mut buf);
    started.fetch_add(1, Relaxed);
    for _ in 0..rounds {
        tokio::task::yield_now().await;
        yields.fetch_add(1, Relaxed);
        black_box(&buf);
    }
    std::future::pending::<()>().await;
    black_box(&buf);
}

fn spawn_task(n: usize, rounds: usize, s: &Arc<AtomicUsize>, y: &Arc<AtomicUsize>) {
    let (s, y) = (s.clone(), y.clone());
    match n {
        0 => drop(tokio::spawn(task::<1>(rounds, s, y))),
        1024 => drop(tokio::spawn(task::<1024>(rounds, s, y))),
        4096 => drop(tokio::spawn(task::<4096>(rounds, s, y))),
        16384 => drop(tokio::spawn(task::<16384>(rounds, s, y))),
        _ => panic!("footprint must be 0, 1024, 4096 or 16384"),
    }
}

fn main() {
    let a: Vec<usize> = std::env::args().skip(1).map(|v| v.parse().unwrap()).collect();
    let (n, foot, rounds) = (a[0], a[1], a[2]);
    let threads = a.get(3).copied().unwrap_or(1);
    let rt = if threads <= 1 {
        tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap()
    } else {
        tokio::runtime::Builder::new_multi_thread().worker_threads(threads).enable_all().build().unwrap()
    };
    rt.block_on(async {
        let started = Arc::new(AtomicUsize::new(0));
        let yields = Arc::new(AtomicUsize::new(0));
        let (rss0, vsz0, vma0) = (status_kb("VmRSS:"), status_kb("VmSize:"), vmas());
        let t0 = Instant::now();
        for _ in 0..n {
            spawn_task(foot, rounds, &started, &yields);
        }
        let t_spawn = t0.elapsed();
        // wait until every task has run once (is parked on its first yield)
        while started.load(Relaxed) < n {
            tokio::task::yield_now().await;
        }
        let (rss1, vsz1, vma1) = (status_kb("VmRSS:"), status_kb("VmSize:"), vmas());
        let y0 = yields.load(Relaxed);
        let t1 = Instant::now();
        while yields.load(Relaxed) < n * rounds {
            tokio::task::yield_now().await;
        }
        let t_run = t1.elapsed();
        println!(
            "tokio threads={} n={} footprint_B={} rss_per_task_B={:.0} vsz_per_task_B={:.0} vmas={} \
             spawn_ns={:.0} yield_ns={:.1}",
            threads, n, foot,
            (rss1 - rss0) as f64 * 1024.0 / n as f64,
            (vsz1 - vsz0) as f64 * 1024.0 / n as f64,
            vma1 - vma0,
            t_spawn.as_nanos() as f64 / n as f64,
            t_run.as_nanos() as f64 / (n * rounds - y0).max(1) as f64
        );
    });
    std::process::exit(0); // skip task drop cost; teardown is not what is measured here
}
