import os
import struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol_insn import word, dis
# find enclosing function: scan backwards for stwu r1,-N(r1) prologue
a = int(sys.argv[1], 16)
for back in range(0, 0x800, 4):
    w = word(a - back)
    if w is None: continue
    if (w & 0xFFFF0000) == 0x94210000:  # stwu r1,-N(r1)
        print("prologue ~0x%08X: %s" % (a - back, dis(a - back, w)))
        break
for back in range(4, 0x800, 4):
    w = word(a - back)
    if w is None: continue
    if w == 0x4E800020:
        print("prev blr at 0x%08X" % (a - back))
        break
