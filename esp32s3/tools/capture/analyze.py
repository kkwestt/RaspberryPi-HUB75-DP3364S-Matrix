import sys, struct
from collections import Counter

BITS = dict(R1=11, G1=27, B1=7, R2=8, G2=9, B2=10, A=22, B=23, C=24, D=25, E=15,
            CLK=17, OE=18, LAT=4)

def load(p):
    d = open(p, 'rb').read()
    return struct.unpack('<%dI' % (len(d) // 4), d)

def bit(v, n): return (v >> BITS[n]) & 1
def addr(v): return bit(v,'A') | bit(v,'B') << 1 | bit(v,'C') << 2 | bit(v,'D') << 3 | bit(v,'E') << 4
def data6(v): return ''.join(str(bit(v, n)) for n in ('R1','G1','B1','R2','G2','B2'))

def runs(seq, name):
    """list of (start, length) of runs where signal==1"""
    out, start = [], None
    for i, v in enumerate(seq):
        b = bit(v, name)
        if b and start is None: start = i
        if not b and start is not None: out.append((start, i - start)); start = None
    return out

if __name__ == '__main__':
    s = load(sys.argv[1])
    n = len(s)
    lat = runs(s, 'LAT')
    oe = runs(s, 'OE')
    print('edges', n)
    print('LAT run lengths:', Counter(l for _, l in lat).most_common(10))
    print('OE  run lengths:', Counter(l for _, l in oe).most_common(10))
    # spacing between OE starts
    gaps = [oe[i+1][0] - oe[i][0] for i in range(len(oe)-1)]
    print('OE start gaps:', Counter(gaps).most_common(10))
    # long LAT commands (>1) positions, show first 40
    cmds = [(st, l) for st, l in lat if l > 1]
    print('first commands (pos,len):', cmds[:40])
    # frame boundary = VSYNC (len 3)
    vs = [st for st, l in lat if l == 3]
    fr = [vs[i+1] - vs[i] for i in range(len(vs)-1)]
    print('VSYNC count', len(vs), 'frame lengths:', Counter(fr).most_common(5))
