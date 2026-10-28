import re, sys, collections
src = open(sys.argv[1]).read().splitlines()
fn_cls = collections.OrderedDict(); frames = {}
calls = collections.Counter(); per_fn = collections.defaultdict(collections.Counter)
cur = None
for ln in src:
    if ln.lstrip().startswith('//'): continue
    m = re.match(r'fn (\w+)\(', ln)
    if m:
        cur = m.group(1)
        t = re.search(r'//@([NP])(?: frame=(\d+))?', ln)
        fn_cls[cur] = t.group(1)
        if t.group(2): frames[cur] = int(t.group(2))
    for tag in re.findall(r'/\*(N|P|B|I:[NP])\*/', ln):
        k = 'I' if tag.startswith('I') else tag
        calls[k] += 1
        if cur: per_fn[cur][k] += 1
n = sum(1 for c in fn_cls.values() if c == 'N'); p = len(fn_cls) - n
print(f"functions: {len(fn_cls)}   non-parking (native stack): {n}   parking (frames): {p}")
tot = sum(calls.values())
print(f"call sites: {tot}   N={calls['N']}  P={calls['P']}  B={calls['B']}  indirect={calls['I']}")
print(f"parking-class call sites (P+B+I): {calls['P']+calls['B']+calls['I']}  ({100*(calls['P']+calls['B']+calls['I'])/tot:.0f}%)")
print(f"sum of frame sizes, all parking fns: {sum(frames.values())} B   mean {sum(frames.values())/len(frames):.0f} B   max {max(frames.values())} B")
print("\nper-function:")
for f, c in fn_cls.items():
    print(f"  {f:16s} {c}  frame={frames.get(f,'-'):>4}  calls: " + " ".join(f"{k}={v}" for k, v in sorted(per_fn[f].items())))
