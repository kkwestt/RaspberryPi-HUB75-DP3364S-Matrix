import sys
from analyze import load, bit, addr, runs

s = load(sys.argv[1])
lat = runs(s, 'LAT')
cmds = [(st, l) for st, l in lat if l > 1]
print('commands (first 60):', [(st, l) for st, l in cmds[:60]])
# find first PRE_ACT (14) and decode following WR_CFG (5) words:
# a register write = 16 data bits per chip * chips, LAT high during last 5 clocks.
for st, l in lat[:400]:
    if l == 5:
        # data bits of the 16*16 = 256 clocks ending at the LAT end, R1 line
        end = st + l
        w = s[end - 256:end]
        word0 = 0
        for i in range(16):
            word0 = (word0 << 1) | bit(w[i], 'R1')
        # all chips same?
        words = []
        for c in range(16):
            v = 0
            for i in range(16):
                v = (v << 1) | bit(w[c * 16 + i], 'R1')
            words.append(v)
        same_lines = all(bit(x, 'R1') == bit(x, 'G1') == bit(x, 'B1') == bit(x, 'R2') == bit(x, 'G2') == bit(x, 'B2') for x in w)
        print('WR_CFG at', st, 'word0=0x%04x' % word0, 'all chips same:', len(set(words)) == 1, 'all 6 lines same:', same_lines, 'ROW/addr', bit(s[st], 'OE'), addr(s[st]))
# what is between commands: gaps and clocks
print('gaps between first 12 commands:', [cmds[i + 1][0] - (cmds[i][0] + cmds[i][1]) for i in range(12)])
