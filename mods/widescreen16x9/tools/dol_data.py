import os
import struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol_insn import foff, D, word
for arg in sys.argv[1:]:
    a = int(arg, 16)
    w = word(a)
    fo, sec = foff(a)
    if w is None:
        print("%08X  <outside>" % a); continue
    f = struct.unpack(">f", struct.pack(">I", w))[0]
    print("%08X  %08X  f32=%-14g  s32=%d  (%s)" % (a, w, f, w - 0x100000000 if w & 0x80000000 else w, sec))
