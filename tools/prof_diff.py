"""Compare two call-chain profiles (TOTK_PROFILE_CHAINS=1) by guest function: inclusive share of samples in each.

usage: prof_diff.py <a.prof> <b.prof> <totk.map> [--top 60] [--min 0.5]

Prints the guest functions (f_<address> labels) whose inclusive share (percent of the profile's chain samples that have
the function anywhere in the chain) drops most from a to b, then those that rise most. Use it to find code that stops
running in one game state, such as the world draw while a menu is open.
"""
import argparse
import bisect
import collections
import re

ALL = False
SYM = re.compile(r'^([0-9a-f]{8}) +[0-9a-f]{8} +\d+ {2,}(f_[0-9a-f]+)$')


def load_map(path):
    syms = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SYM.match(line.rstrip('\n'))
            if m:
                syms.setdefault(int(m.group(1), 16), m.group(2))
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def load_prof(path, addrs, names):
    base, total = 0, 0
    incl = collections.Counter()
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == 'base':
                base = int(p[1], 16)
            elif p[0] == 'stack':
                n = int(p[1])
                # Only chains that were running guest code: a leaf outside the exe is a thread waiting in the system.
                leaf = int(p[2], 16) - base
                if not ALL and (not (0 <= leaf < addrs[-1] + 0x100000) or bisect.bisect_right(addrs, leaf) == 0):
                    continue
                total += n
                seen = set()
                for rip in p[2:]:
                    rva = int(rip, 16) - base
                    if rva < 0:
                        continue
                    i = bisect.bisect_right(addrs, rva) - 1
                    if i >= 0:
                        seen.add(names[i])
                for s in seen:
                    incl[s] += n
    return total, incl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('map')
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--all', action='store_true', help='also chains whose leaf is outside guest code (waits, host calls)')
    ap.add_argument('--min', type=float, default=0.5, help='percent in the larger profile')
    args = ap.parse_args()
    global ALL
    ALL = args.all
    addrs, names = load_map(args.map)
    ta, ia = load_prof(args.a, addrs, names)
    tb, ib = load_prof(args.b, addrs, names)
    print(f'{args.a}: {ta} chain samples; {args.b}: {tb}')
    rows = []
    for s in set(ia) | set(ib):
        pa, pb = 100 * ia[s] / max(ta, 1), 100 * ib[s] / max(tb, 1)
        if max(pa, pb) >= args.min:
            rows.append((pb - pa, pa, pb, s))
    rows.sort()
    print('\nless in b:')
    for d, pa, pb, s in rows[:args.top]:
        print(f'  {s:16} {pa:6.2f}% -> {pb:6.2f}%')
    print('\nmore in b:')
    for d, pa, pb, s in rows[::-1][:args.top // 3]:
        print(f'  {s:16} {pa:6.2f}% -> {pb:6.2f}%')


if __name__ == '__main__':
    main()
