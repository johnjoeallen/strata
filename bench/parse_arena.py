import re, sys, collections
def load(path):
    runs=[]; cur=None
    for l in open(path):
        l=l.rstrip('\n')
        if l.startswith('## '):
            cur={'cmd':dict(kv.split('=',1) for kv in l[3:].split() if '=' in kv),'m':{}}
            if l[3:].split()[0] in ('fill','churn','mixed') or ' ' in l[3:] and '=' not in l[3:].split()[0]:
                pass
            runs.append(cur); continue
        if cur is None: continue
        m=re.match(r'fill: alloc\+touch ([\d.]+) ns/op, free \(random order\) ([\d.]+)',l)
        if m: cur['m']['fill_alloc']=float(m[1]); cur['m']['fill_free']=float(m[2])
        m=re.match(r'churn: ([\d.]+) ns',l)
        if m: cur['m']['churn']=float(m[1])
        m=re.match(r'mixed: ([\d.]+) ns/step',l)
        if m: cur['m']['mixed_ns']=float(m[1])
        m=re.match(r'\s+(full|after free all|after free 90%|after refill|after churn|steady state|after burst)\s+rss=\s*([\d.]+) MB\s+live=\s*([\d.]+) MB\s+ratio=\s*([\d.]+)',l)
        if m:
            k=m[1]; cur['m'][k]=(float(m[2]),float(m[3]),float(m[4]))
        m=re.match(r'\s+chunks=(\d+) decommits=(\d+)',l)
        if m: cur['m']['decommits']=int(m[2])
    return runs
def find(runs,**kw):
    for r in runs:
        c=r['cmd']
        if all(c.get(k)==str(v) for k,v in kw.items()): return r
    return None
def f(x,fmt='{:.1f}'): return fmt.format(x) if x is not None else '-'
