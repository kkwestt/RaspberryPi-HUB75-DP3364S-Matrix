import sys
from analyze import load, bit, runs

s = load(sys.argv[1])
lat = runs(s, 'LAT')
vs = [st for st, l in lat if l == 3]
f0, f1 = vs[1], vs[2]
fr = s[f0:f1]
dl = [st for st, l in runs(fr, 'LAT') if l == 1]
print('latches', len(dl))

# For each latch: the 256 data clocks that end at the latch clock (inclusive?).
# Determine whether the latch clock itself carries data: try window [st-255, st] and [st-256, st-1]
def window(st, incl):
    return fr[st - 255:st + 1] if incl else fr[st - 256:st]

def ones(line, incl):
    res = []
    for k, st in enumerate(dl):
        w = window(st, incl)
        for i, v in enumerate(w):
            if bit(v, line): res.append((k, i))
    return res

for incl in (True, False):
    print('--- window includes latch clock:', incl)
    # R1 in latches of line 3 (if line = k//16): expect only pixel (5,3)
    r1 = ones('R1', incl)
    for L in (3,):
        sel = [(k, i) for k, i in r1 if k // 16 == L]
        print('R1 line', L, sel[:20])
    g2 = [(k, i) for k, i in ones('G2', incl) if k // 16 == 8]
    print('G2 line 8', g2[:20])
    b2 = [(k, i) for k, i in ones('B2', incl) if k // 16 == 1]
    print('B2 line 1', b2[:40])
