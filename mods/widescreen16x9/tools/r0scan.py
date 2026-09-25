import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dol_insn import word, dis
lo = int(sys.argv[1], 16); hi = int(sys.argv[2], 16)
a = lo
while a <= hi:
    w = word(a)
    if w is not None:
        s = dis(a, w)
        # flag r0 writes: rD==0 loads/addis or mtspr to lr, or any insn writing r0
        mark = ""
        op = w >> 26
        rD = (w >> 21) & 31
        if op in (14,15,7,8,12,13,32,33,34,35,40,41,42,43,46,24,25,28,29) and rD == 0:
            mark = "   <== r0 written"
        if op == 31:
            xo = (w>>1)&0x3FF
            if xo in (26,467,339,266,10,40,444,235,87,343,311,375,23) and rD == 0:
                mark = "   <== r0 written"
            if xo == 19: mark = "   <== mfcr r0"
            if xo == 467: mark = "   <== mtspr r0 (mtlr/mtctr)"
        if op == 18 and (w & 1): mark = "   <== bl (lr=pc+4)"
        if op == 18 and not (w & 1): mark = "   <== b (tail?)"
        if w == 0x4E800020: mark = "   <== blr"
        print("%08X  %-28s %08X%s" % (a, s, w, mark))
    a += 4
