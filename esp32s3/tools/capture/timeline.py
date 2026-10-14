import sys
from analyze import load, bit, addr, runs, data6

s = load(sys.argv[1])
lat = runs(s, 'LAT')
vs = [st for st, l in lat if l == 3]
f0, f1 = vs[1], vs[2]          # one full frame
print('frame', f0, '->', f1, 'len', f1 - f0)
fr = s[f0 - 20:f1 + 5]
base = f0 - 20
# event list: LAT runs, OE runs, address changes
ev = []
for st, l in runs(fr, 'LAT'): ev.append((st + base - f0, 'LAT', l))
for st, l in runs(fr, 'OE'): ev.append((st + base - f0, 'ROW', l, addr(fr[st])))
prev = None
for i, v in enumerate(fr):
    a = addr(v)
    if a != prev: ev.append((i + base - f0, 'ADDR', a)); prev = a
ev.sort()
for e in ev[:80]: print(e)
print('...')
for e in ev[-40:]: print(e)
# data clocks per latch: distance between consecutive data latches
dl = [st for st, l in runs(fr, 'LAT') if l == 1]
gaps = [dl[i+1] - dl[i] for i in range(len(dl) - 1)]
from collections import Counter
print('data-latch spacing:', Counter(gaps).most_common(8))
print('first data latch at', dl[0] + base - f0, 'last at', dl[-1] + base - f0)
# ROW address sequence (first 70)
rows = [(st + base - f0, addr(fr[st]), l) for st, l in runs(fr, 'OE')]
print('ROW count in frame', len(rows))
print('ROW addr seq head:', [r[1] for r in rows[:70]])
print('W12 rows:', [r for r in rows if r[2] == 12])
