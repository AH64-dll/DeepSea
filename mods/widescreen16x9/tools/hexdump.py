import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol_insn import foff, D
a = int(sys.argv[1], 16); n = int(sys.argv[2], 16)
fo, sec = foff(a)
print("file off 0x%X sec %s" % (fo, sec))
for i in range(0, n, 16):
    print("%08X  %s" % (a + i, " ".join("%02X" % b for b in D[fo+i:fo+i+16])))
