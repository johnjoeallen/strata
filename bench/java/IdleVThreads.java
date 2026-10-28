// Group 1 comparator: Java virtual threads as the "idle connection" unit.
// usage: java IdleVThreads <n> <footprint_bytes> <rounds>
import java.nio.file.*;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.LockSupport;

public class IdleVThreads {
    static long statusKb(String key) throws Exception {
        for (String l : Files.readAllLines(Path.of("/proc/self/status")))
            if (l.startsWith(key)) return Long.parseLong(l.split("\\s+")[1]);
        return -1;
    }
    static long vmas() throws Exception { return Files.readAllLines(Path.of("/proc/self/maps")).size(); }
    static boolean gateOpen(boolean[] g) { synchronized (g) { return g[0]; } }
    static long heapUsed() {
        Runtime r = Runtime.getRuntime();
        return r.totalMemory() - r.freeMemory();
    }
    static void settle() throws Exception {
        for (int i = 0; i < 3; i++) { System.gc(); Thread.sleep(200); }
    }

    public static void main(String[] a) throws Exception {
        int n = Integer.parseInt(a[0]), foot = Integer.parseInt(a[1]), rounds = Integer.parseInt(a[2]);
        AtomicLong started = new AtomicLong(), yields = new AtomicLong();
        final boolean[] gate = new boolean[1];  // released by main after the idle measurement
        settle();
        long rss0 = statusKb("VmRSS:"), heap0 = heapUsed(), vma0 = vmas();
        long t0 = System.nanoTime();
        Thread[] ts = new Thread[n];
        for (int i = 0; i < n; i++) {
            ts[i] = Thread.ofVirtual().unstarted(() -> {
                byte[] buf = new byte[foot];
                for (int j = 0; j < foot; j += 64) buf[j] = 1;
                started.incrementAndGet();
                while (!gateOpen(gate)) LockSupport.park();
                for (int r = 0; r < rounds; r++) {
                    Thread.yield();
                    yields.incrementAndGet();
                    if (buf.length > 0 && buf[0] == 7) System.out.print("");
                }
                LockSupport.park();
                if (buf.length > 0 && buf[0] == 7) System.out.print("");
            });
        }
        for (Thread t : ts) t.start();
        double spawnNs = (System.nanoTime() - t0) / (double) n;
        while (started.get() < n) Thread.sleep(5);
        settle();
        long rss1 = statusKb("VmRSS:"), heap1 = heapUsed(), vma1 = vmas();
        long t1 = System.nanoTime();
        synchronized (gate) { gate[0] = true; }
        for (Thread t : ts) LockSupport.unpark(t);
        while (yields.get() < (long) n * rounds) Thread.sleep(5);
        double yieldNs = (System.nanoTime() - t1) / (double) ((long) n * rounds);
        System.out.printf("java-vthread n=%d footprint_B=%d heap_per_thread_B=%.0f rss_per_thread_B=%.0f vmas=%d spawn_ns=%.0f yield_ns=%.1f%n",
            n, foot, (heap1 - heap0) / (double) n, (rss1 - rss0) * 1024.0 / n, vma1 - vma0, spawnNs, yieldNs);
        System.exit(0);
    }
}
