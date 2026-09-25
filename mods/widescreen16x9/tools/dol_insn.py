"""Minimal DOL reader + PPC decoder for the widescreen Gecko site audit.
Reads retail main.dol, prints the ORIGINAL instruction at each Gecko
04-site plus a window around it. No external deps."""
import os, struct, sys

# Path to sys/main.dol of your own extracted USA disc.
DOL = os.environ.get("GZLE01_MAIN_DOL", "sys/main.dol")

def load_dol(path):
    d = open(path, "rb").read()
    secs = []
    for i in range(7):
        off = struct.unpack(">I", d[0x00 + i*4:0x04 + i*4])[0]
        secs.append(("text%d" % i, off))
    for i in range(11):
        off = struct.unpack(">I", d[0x1C + i*4:0x20 + i*4])[0]
        secs.append(("data%d" % i, off))
    addrs = []
    for i in range(7):
        addrs.append(struct.unpack(">I", d[0x48 + i*4:0x4C + i*4])[0])
    for i in range(11):
        addrs.append(struct.unpack(">I", d[0x64 + i*4:0x68 + i*4])[0])
    sizes = []
    for i in range(7):
        sizes.append(struct.unpack(">I", d[0x90 + i*4:0x94 + i*4])[0])
    for i in range(11):
        sizes.append(struct.unpack(">I", d[0xAC + i*4:0xB0 + i*4])[0])
    table = []
    for (name, off), addr, size in zip(secs, addrs, sizes):
        if size:
            table.append((name, addr, off, size))
    return d, table

D, SECS = load_dol(DOL)

def foff(addr):
    for name, a, off, size in SECS:
        if a <= addr < a + size:
            return off + (addr - a), name
    return None, None

def word(addr):
    fo, _ = foff(addr)
    if fo is None:
        return None
    return struct.unpack(">I", D[fo:fo+4])[0]

GPR = ["r%d" % i for i in range(32)]
SPR = {1:"xer",8:"lr",9:"ctr"}
def sx24(v):  # 24-bit signed
    return v - 0x1000000 if v & 0x800000 else v
def sx16(v):
    return v - 0x10000 if v & 0x8000 else v

def dis(addr, w):
    op = w >> 26
    rD = (w >> 21) & 31; rA = (w >> 16) & 31; rB = (w >> 11) & 31
    simm = sx16(w & 0xFFFF); uimm = w & 0xFFFF
    bo = rD; bi = rA
    if op == 18:  # b
        li = sx24((w >> 2) & 0xFFFFFF) << 2
        tgt = (addr if (w & 2) else addr + li) if (w & 2) else (addr + li)
        tgt = (li if (w & 2) else li + addr)
        return ("bl 0x%08X" if (w & 1) else "b 0x%08X") % tgt
    if op == 19:  # bclr/bcctr
        if (w >> 1) & 0x3FF == 16:
            return "bclrl" if (w & 1) else "bclr"
        if (w >> 1) & 0x3FF == 528:
            return "bcctrl" if (w & 1) else "bcctr"
        if (w >> 1) & 0x3FF == 0x210:
            return "bcctr"
        return "bclr/bcctr xo=%d" % ((w >> 1) & 0x3FF)
    if op == 16:  # bc
        bd = sx16(w & 0xFFFC)
        tgt = bd + addr
        return ("bcl bo=%d bi=%d 0x%08X" if (w & 1) else "bc bo=%d bi=%d 0x%08X") % (bo, bi, tgt)
    if op == 14: return "addi %s,%s,%d" % (GPR[rD], GPR[rA], simm)
    if op == 15: return "addis %s,%s,%d" % (GPR[rD], GPR[rA], simm)
    if op == 24: return "ori %s,%s,0x%X" % (GPR[rA], GPR[rD], uimm)
    if op == 25: return "oris %s,%s,0x%X" % (GPR[rA], GPR[rD], uimm)
    if op == 20: return "rlwimi %s,%s,%d,%d,%d" % (GPR[rA], GPR[rD], rB, (w>>6)&31, (w>>1)&31)
    if op == 21: return "rlwinm %s,%s,%d,%d,%d" % (GPR[rA], GPR[rD], rB, (w>>6)&31, (w>>1)&31)
    if op == 32: return "lwz %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 33: return "lwzu %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 34: return "lbz %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 35: return "lbzu %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 36: return "stw %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 37: return "stwu %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 38: return "stb %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 39: return "stbu %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 40: return "lhz %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 41: return "lhzu %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 42: return "lha %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 43: return "lhau %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 44: return "sth %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 45: return "sthu %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 46: return "lmw %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 47: return "stmw %s,%d(%s)" % (GPR[rD], simm, GPR[rA])
    if op == 48: return "lfs f%d,%d(%s)" % (rD, simm, GPR[rA])
    if op == 49: return "lfd f%d,%d(%s)" % (rD, simm, GPR[rA])
    if op == 50: return "lfdu f%d,%d(%s)" % (rD, simm, GPR[rA])
    if op == 52: return "stfs f%d,%d(%s)" % (rD, simm, GPR[rA])
    if op == 53: return "stfd f%d,%d(%s)" % (rD, simm, GPR[rA])
    if op == 56: return "psq_l f%d,%d(%s)" % (rD, simm & 0xFFF, GPR[rA])
    if op == 57: return "lfdx/psx f%d" % rD
    if op == 60: return "psq_st f%d,%d(%s)" % (rD, simm & 0xFFF, GPR[rA])
    if op == 11: return "cmpwi cr%d,%s,%d" % ((w>>23)&7, GPR[rA], simm)
    if op == 10: return "cmplwi cr%d,%s,0x%X" % ((w>>23)&7, GPR[rA], uimm)
    if op == 12: return "addic %s,%s,%d" % (GPR[rD], GPR[rA], simm)
    if op == 13: return "addic. %s,%s,%d" % (GPR[rD], GPR[rA], simm)
    if op == 7:  return "mulli %s,%s,%d" % (GPR[rD], GPR[rA], simm)
    if op == 8:  return "subfic %s,%s,%d" % (GPR[rD], GPR[rA], simm)
    if op == 28: return "andi. %s,%s,0x%X" % (GPR[rA], GPR[rD], uimm)
    if op == 29: return "andis. %s,%s,0x%X" % (GPR[rA], GPR[rD], uimm)
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 444: return ("or. %s,%s,%s" if w&1 else "or %s,%s,%s") % (GPR[rA], GPR[rD], GPR[rB])
        if xo == 467: return "extsh %s,%s" % (GPR[rA], GPR[rD])
        if xo == 266: return "add %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 10:  return "addc %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 40:  return "subf %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 151: return "stwx %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 23:  return "lwzx %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 339: return "mfspr %s,%s" % (GPR[rD], SPR.get((w>>11)&31, "spr%d"%((w>>11)&31)))
        if xo == 144: return "mtcrf 0x%02X,%s" % ((w>>12)&0xFF, GPR[rD])
        if xo == 19:  return "mfcr %s" % GPR[rD]
        if xo == 26:  return "cntlzw %s,%s" % (GPR[rA], GPR[rD])
        if xo == 0:   return "fcmpu cr%d,f%d,f%d" % ((w>>23)&7, rA, rB)
        if xo == 235: return "mullw %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 87:  return "lbzx %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 215: return "stbx %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 343: return "lhax %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 407: return "sthx %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 311: return "lhzx %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 375: return "lhaux %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 535: return "lfsx f%d,%s,%s" % (rD, GPR[rA], GPR[rB])
        if xo == 663: return "stfsx f%d,%s,%s" % (rD, GPR[rA], GPR[rB])
        if xo == 567: return "lfsux f%d" % rD
        if xo == 595: return "lfdx f%d" % rD
        if xo == 631: return "lfdux f%d" % rD
        if xo == 599: return "lfdx f%d,%s,%s" % (rD, GPR[rA], GPR[rB])
        if xo == 727: return "stfdx f%d,%s,%s" % (rD, GPR[rA], GPR[rB])
        if xo == 983: return "stfiwx f%d,%s,%s" % (rD, GPR[rA], GPR[rB])
        if xo == 247: return "stbux %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 55:  return "lwzux %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 183: return "stwux %s,%s,%s" % (GPR[rD], GPR[rA], GPR[rB])
        if xo == 54:  return "dcbst %s,%s" % (GPR[rA], GPR[rB])
        if xo == 86:  return "dcbf %s,%s" % (GPR[rA], GPR[rB])
        if xo == 470: return "dcbi %s,%s" % (GPR[rA], GPR[rB])
        return "xo31:%d" % xo
    if op == 59:
        xo = w & 0x3E
        if xo == 36: return "fdivs f%d,f%d,f%d" % (rD, rA, rB)
        if xo == 40: return "fsubs f%d,f%d,f%d" % (rD, rA, rB)
        if xo == 42: return "fadds f%d,f%d,f%d" % (rD, rA, rB)
        if xo == 48: return "fres f%d,f%d" % (rD, rB)
        if xo == 50: return "fmuls f%d,f%d,f%d" % (rD, rA, (w>>6)&31)
        if xo == 56: return "fmsubs f%d" % rD
        if xo == 58: return "fmadds f%d,f%d,f%d,f%d" % (rD, rA, (w>>6)&31, rB)
        if xo == 60: return "fnmsubs f%d" % rD
        if xo == 62: return "fnmadds f%d" % rD
        if xo == 24: return "frsp f%d,f%d" % (rD, rB)
        if xo == 30: return "fctiwz f%d,f%d" % (rD, rB)
        if xo == 28: return "fctiw f%d,f%d" % (rD, rB)
        if xo == 14: return "fctiwz f%d,f%d" % (rD, rB)
        if xo == 18: return "fdivs f%d" % rD
        if xo == 20: return "fsubs f%d" % rD
        if xo == 21: return "fadds f%d" % rD
        return "xo59:%02X" % xo
    if op == 63:
        xo10 = (w >> 1) & 0x3FF
        if xo10 == 40: return "fneg f%d,f%d" % (rD, rB)
        if xo10 == 12: return "frsp f%d,f%d" % (rD, rB)
        if xo10 == 14: return "fctiw f%d,f%d" % (rD, rB)
        if xo10 == 15: return "fctiwz f%d,f%d" % (rD, rB)
        if xo10 == 72: return "fmr f%d,f%d" % (rD, rB)
        if xo10 == 264: return "fabs f%d,f%d" % (rD, rB)
        if xo10 == 136: return "fnabs f%d,f%d" % (rD, rB)
        if xo10 == 32: return "fcmpo cr%d,f%d,f%d" % ((w>>23)&7, rA, rB)
        if xo10 == 0: return "fcmpu cr%d,f%d,f%d" % ((w>>23)&7, rA, rB)
        if xo10 == 583: return "mffs f%d" % rD
        if xo10 == 711: return "mtfsf 0x%02X,f%d" % ((w>>17)&0xFF, rB)
        if xo10 == 70: return "mtfsb0 %d" % rD
        if xo10 == 38: return "mtfsb1 %d" % rD
        xo5 = w & 0x3E
        if xo5 == 40: return "fsubs f%d,f%d,f%d" % (rD, rA, rB)
        if xo5 == 42: return "fadds f%d,f%d,f%d" % (rD, rA, rB)
        if xo5 == 36: return "fdiv f%d,f%d,f%d" % (rD, rA, rB)
        if xo5 == 50: return "fmul f%d,f%d,f%d" % (rD, rA, (w>>6)&31)
        if xo5 == 52: return "frsqrte f%d,f%d" % (rD, rB)
        if xo5 == 56: return "fmsub f%d" % rD
        if xo5 == 58: return "fmadd f%d" % rD
        if xo5 == 60: return "fnmsub f%d" % rD
        if xo5 == 62: return "fnmadd f%d" % rD
        return "xo63:%03X/%03X" % (xo10, xo5)
    if op == 4:
        return "paired/vec op4 xo=%d" % ((w>>1)&0x3FF)
    return "op%d w=%08X" % (op, w)

if __name__ == "__main__":
    for arg in sys.argv[1:]:
        a = int(arg, 16)
        w = word(a)
        fo, sec = foff(a)
        if w is None:
            print("%08X  <outside sections>" % a)
        else:
            print("%08X  %-26s  %08X  (%s)" % (a, dis(a, w), w, sec))
