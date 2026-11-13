"""Which guest functions call the graphics API (NVN) in a call-chain profile, and how often, compared between two runs.

usage: prof_gfx_callers.py <a.prof> <b.prof> <totk.map> [--depth 3] [--top 40]

For every chain whose leaf is a runtime function whose name holds 'nvn' (the guest's NVN calls), the first --depth guest
functions (f_<address>) above it are counted. Prints those that drop most from a to b: in a menu that stops the world
draw, the world draw's callers drop to zero.
"""
import argparse
import bisect
import collections
import re

SYM = re.compile(r'^([0-9a-f]{8}) +[0-9a-f]{8} +\d+ {2,}(\S+)$')


def load_map(path):
    syms = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = SYM.match(line.rstrip('\n'))
            if m and '(' not in m.group(2):
                syms.setdefault(int(m.group(1), 16), m.group(2))
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs]


def load(path, addrs, names, depth):
    base, total = 0, 0
    callers = collections.Counter()
    with open(path) as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            if p[0] == 'base':
                base = int(p[1], 16)
            elif p[0] == 'stack':
                n = int(p[1])
                chain = []
                for rip in p[2:]:
                    rva = int(rip, 16) - base
                    i = bisect.bisect_right(addrs, rva) - 1
                    chain.append(names[i] if rva >= 0 and i >= 0 else '?')
                if not chain or 'nvn' not in chain[0].lower():
                    continue
                total += n
                guests = [c for c in chain if c.startswith('f_')]
                for g in set(guests[:depth]):
                    callers[g] += n
    return total, callers


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('map')
    ap.add_argument('--depth', type=int, default=3)
    ap.add_argument('--top', type=int, default=40)
    args = ap.parse_args()
    addrs, names = load_map(args.map)
    ta, ca = load(args.a, addrs, names, args.depth)
    tb, cb = load(args.b, addrs, names, args.depth)
    print(f'NVN-call samples: {args.a} {ta}, {args.b} {tb}')
    rows = sorted(((cb[g] - ca[g], ca[g], cb[g], g) for g in set(ca) | set(cb)))
    for d, a, b, g in rows[:args.top]:
        print(f'  {g:16} {a:6} -> {b:6}')


if __name__ == '__main__':
    main()
