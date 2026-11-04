"""Read the F9 timeline from a run log: per-frame RSX/main/workload summary and an ASCII chart.

    python tools/timeline.py build/run_user237.log
"""
import re,sys,collections
ev=[]
for l in open(sys.argv[1],errors='replace'):
    m=re.match(r'\[tl\] t\d+:\s+([0-9.]+) ms  (.*)',l)
    if m: ev.append((float(m.group(1)),m.group(2).strip()))
ev.sort(key=lambda e:e[0])
flips=[t for t,s in ev if s.startswith('rsx flip')]
print('flips',flips, 'gaps',[round(b-a,1) for a,b in zip(flips,flips[1:])])
kinds=collections.Counter(re.sub(r'[0-9a-fx.]{3,}','#',s)[:34] for t,s in ev)
print(kinds.most_common(24))
def overlap(iv,a,b): return sum(max(0,min(y,b)-max(x,a)) for x,y in iv)
for a,b in zip(flips,flips[1:]):
    print(f'\n=== frame {a:.1f} .. {b:.1f} ({b-a:.1f} ms)')
    # rsx parked
    park=[];t0=None
    for t,s in ev:
        if s.startswith('rsx parked'): t0=t;w=s
        elif s.startswith('rsx resumes') and t0 is not None:
            park.append((t0,t,w)); t0=None
    pk=[(x,y) for x,y,_ in park]
    print(f' rsx parked {overlap(pk,a,b):.1f} ms, working {b-a-overlap(pk,a,b):.1f} ms')
    for x,y,w in park:
        if y>a and x<b: print(f'   parked {x-a:6.1f}..{y-a:6.1f} ({y-x:5.1f} ms) {w[11:40]}')
    # main sleep
    sl=[]
    for t,s in ev:
        m=re.match(r'main (\S+) ([0-9.]+) ms',s)
        if m: sl.append((t-float(m.group(2)),t,m.group(1)))
    us=[(x,y) for x,y,n in sl]
    print(f' main waiting (OS calls) {overlap(us,a,b):.1f} ms, running {b-a-overlap(us,a,b):.1f} ms')
    by=collections.Counter()
    for x,y,n in sl: by[n]+=max(0,min(y,b)-max(x,a))
    print('   ',[(k,round(v,1)) for k,v in by.most_common(6)])
    # workload intervals
    st={};iv=collections.defaultdict(list)
    for t,s in ev:
        m=re.match(r'(.+?) starts',s)
        if m: st.setdefault(m.group(1),t); continue
        m=re.match(r'(.+?) idle',s)
        if m and m.group(1) in st: iv[m.group(1)].append((st.pop(m.group(1)),t))
    print(' workload active (ms in frame):',[(k,round(overlap(v,a,b),1)) for k,v in iv.items() if overlap(v,a,b)>0.3])

a,b=flips[0]-10,flips[1]+3
N=int((b-a)*2)+1
def row(iv,ch='#'):
    r=[' ']*N
    for x,y in iv:
        for i in range(max(0,int((x-a)*2)),min(N,int((y-a)*2)+1)): r[i]=ch
    return ''.join(r)
print('\nchart, 1 column = 0.5 ms, | = flip')
axis=[' ']*N
for f in flips:
    if a<=f<b: axis[int((f-a)*2)]='|'
print(f"{'flip':14}"+''.join(axis))
pk=[(x,y) for x,y,_ in park]
# rsx working = between parks
work=[];prev=a
for x,y,_ in park:
    if y<a or x>b: continue
    if x>prev: work.append((prev,x))
    prev=max(prev,y)
work.append((prev,b))
print(f"{'rsx working':14}"+row(work))
print(f"{'rsx parked':14}"+row(pk,'.'))
run=[];prev=a
for x,y,n in sorted(sl):
    if y<a or x>b: continue
    if x-prev>0.15: run.append((prev,x))
    prev=max(prev,y)
run.append((prev,b))
print(f"{'main running':14}"+row(run))
print(f"{'main waits':14}"+row(us,'.'))
for k in ('jm_gfx','jm_post','jm_particles','jm_havok','jm_anim','no name'):
    print(f"{k:14}"+row(iv.get(k,[])))
print('\nmain wait sites in window:')
for x,y,n in sorted(sl):
    pass
c=collections.Counter()
for x,y,n in sl:
    if a<=x<b: c[n]+=y-x
print(c.most_common(8))
