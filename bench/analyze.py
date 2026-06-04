#!/usr/bin/env python3
# 解析 run_bench.sh 的输出,对每个 (workload, 并发) cell:
#   - 取 3 次重复的中位数(rps / p99 / p999),并记录 min~max 反映波动
#   - ON vs OFF 对照:rps 越高越好,p99/p999 越低越好,给出 % 变化
import sys, re, statistics
from collections import defaultdict

def parse(path, store):
    with open(path) as f:
        for line in f:
            if 'RESULT' not in line: continue
            d = dict(re.findall(r'(\w+)=([-\d.]+)', line))
            m = re.search(r'wl=(\w+)', line);
            label = line.split()[0]
            wl = m.group(1)
            conns = int(d['conns'])
            store[(label, wl, conns)].append(d)

def med(xs): return statistics.median(xs)

def main():
    on, off = defaultdict(list), defaultdict(list)
    parse(sys.argv[1], on)
    parse(sys.argv[2], off)

    cells = sorted({(wl,c) for (_,wl,c) in list(on)+list(off)})
    hdr = f"{'workload':10} {'conns':>5} | {'RPS_on':>10} {'RPS_off':>10} {'ΔRPS%':>7} | {'p99_on':>7} {'p99_off':>7} {'Δp99%':>7} | {'p999_on':>8} {'p999_off':>8} {'err':>4}"
    print(hdr); print('-'*len(hdr))
    for wl,c in cells:
        ro = on.get(('POOL_ON',wl,c)); rf = off.get(('POOL_OFF',wl,c))
        if not ro or not rf: continue
        rps_on  = med([float(x['rps']) for x in ro]); rps_off = med([float(x['rps']) for x in rf])
        p99_on  = med([float(x['p99']) for x in ro]); p99_off = med([float(x['p99']) for x in rf])
        p999_on = med([float(x['p999']) for x in ro]); p999_off= med([float(x['p999']) for x in rf])
        errs = sum(int(float(x[k])) for x in ro+rf for k in
                   ('err_connect','err_read','err_write','err_status','err_timeout'))
        drps = (rps_on-rps_off)/rps_off*100
        dp99 = (p99_on-p99_off)/p99_off*100
        print(f"{wl:10} {c:>5} | {rps_on:>10.0f} {rps_off:>10.0f} {drps:>+6.1f}% | "
              f"{p99_on:>7.3f} {p99_off:>7.3f} {dp99:>+6.1f}% | {p999_on:>8.3f} {p999_off:>8.3f} {errs:>4}")

    # 波动(变异系数 CV%)抽查:每 cell 的 rps 3 次的 min~max
    print("\n波动(rps 3 次 min~max,看测量稳定性):")
    for wl,c in cells:
        for lab,store in (('ON',on.get(('POOL_ON',wl,c))),('OFF',off.get(('POOL_OFF',wl,c)))):
            if not store: continue
            xs=[float(x['rps']) for x in store]
            cv = (statistics.pstdev(xs)/statistics.mean(xs)*100) if len(xs)>1 else 0
            print(f"  {wl:10} c={c:<4} {lab:3} rps={[f'{x:.0f}' for x in xs]} cv={cv:.1f}%")

if __name__=='__main__': main()
