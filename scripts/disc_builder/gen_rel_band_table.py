#!/usr/bin/env python3
"""Generate the REL L-window reservation-band table for rel_loader.c
(MODERNGEKKO_BAND_PROTECT, pulse12 / B28 allocator-level carve fix).

For every one of the 415 folder-mode modules this tool emits the EXACT guest
address window the port reserves for that module:

    [base .. base+len(rel_file))                 file image mirrored at L
    [align_up(end, bssAlign) .. +bssSize)        linked BSS window

Inputs:
  --basemap   .org/port/artifacts/rel-basemap.txt (gate-E1 ground truth;
              NEVER regenerated ad hoc) - per-module linked base addresses.
  --disc-root directory the basemap <path>s resolve under (default: repo
              root; also accepts its parent).
  --out       C include emitted beside rel_loader.c.

Output: `RelBand {id, lo, hi}` rows sorted ascending by lo; touching or
overlapping windows merged (merged rows carry id | 0x10000). Runtime carve
reads them only - never regenerate this file by hand.

Run from repo root:  python3 scripts/port/gen_rel_band_table.py
                                   [--descriptor inc | default --basemap mode]

Modes:
  basemap    .org/port/artifacts/rel-basemap.txt retail MEM1 bases
             (pre-LRELOC; kept GEN-STABLE, reproduces MEM1 table byte-exact).
  descriptor port/descriptor/rel-modules-data.inc `// N: <path> base 0xBASE`
             comment rows — the L-reloc descriptor-fixed bases (P1 EXRAM regen).
             P1 finding 1 law: downstream consumers of batch bases MUST read the
             regenerated descriptor, never "retail base + const".
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

BASEMAP_RE = re.compile(r"^\s*module\s+(\d+):\s+(\S+)\s+->\s+base\s+0x([0-9A-Fa-f]+)\s*$")


def be32(data: bytes, off: int) -> int:
    return struct.unpack(">I", data[off : off + 4])[0]


class InputError(Exception):
    pass


def parse_basemap(path: Path) -> dict[int, tuple[str, int]]:
    entries: dict[int, tuple[str, int]] = {}
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        m = BASEMAP_RE.match(line)
        if not m:
            continue
        mod = int(m.group(1))
        if mod in entries:
            raise InputError(f"{path}:{lineno}: duplicate module {mod}")
        entries[mod] = (m.group(2), int(m.group(3), 16))
    if not entries:
        raise InputError(f"{path}: no 'module N: path -> base' rows parsed")
    return entries


DESCRIPTOR_RE = re.compile(r"^// (\d+): (\S+) base 0x([0-9A-Fa-f]+)\s*$")


def parse_descriptor(path: Path) -> dict[int, tuple[str, int]]:
    """Parse the L-reloc descriptor's per-module base comment rows
    (`// N: disc/...rel base 0x912066A4`) into the same (path, base) shape
    parse_basemap returns, so build_bands is shared."""
    entries: dict[int, tuple[str, int]] = {}
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        m = DESCRIPTOR_RE.match(line)
        if not m:
            continue
        mod = int(m.group(1))
        if mod in entries:
            raise InputError(f"{path}:{lineno}: duplicate module {mod}")
        entries[mod] = (m.group(2), int(m.group(3), 16))
    if not entries:
        raise InputError(f"{path}: no '// N: <path> base 0xX' rows parsed")
    return entries


def build_bands(basemap: dict[int, tuple[str, int]], roots: list[Path]) -> list[tuple[int, int, int]]:
    bands: list[tuple[int, int, int]] = []
    for mod in sorted(basemap):
        rel_path_str, base = basemap[mod]
        rel_path = None
        for root in roots:
            for cand in (root / rel_path_str, root / "port" / rel_path_str):
                if cand.exists():
                    rel_path = cand
                    break
            if rel_path is not None:
                break
        if rel_path is None:
            raise InputError(f"{rel_path_str}: not found under {roots}")
        data = rel_path.read_bytes()
        if len(data) < 0x48:
            raise InputError(f"{rel_path}: too small ({len(data)} bytes)")
        rid = be32(data, 0x00)
        if rid != mod:
            raise InputError(f"{rel_path}: REL id {rid} != basemap module {mod}")
        bss_size = be32(data, 0x20)
        bss_align = be32(data, 0x44) or 4
        lo = base
        hi = base + len(data)
        if bss_size:
            lbss = (hi + bss_align - 1) & ~(bss_align - 1)
            hi = lbss + bss_size
        bands.append((mod, lo, hi))
    if not bands:
        raise InputError("no bands produced")
    return bands


def merge_bands(bands: list[tuple[int, int, int]]) -> list[tuple[int, int, int]]:
    """Merge windows that touch or overlap. Merged rows keep the lowest id,
    flagged with bit 0x10000."""
    merged: list[list[int]] = []
    for mod, lo, hi in sorted(bands, key=lambda t: (t[1], t[2])):
        if merged and lo <= merged[-1][2]:
            merged[-1][2] = max(merged[-1][2], hi)
            merged[-1][3] |= 1 if merged[-1][0] != mod else 0
        else:
            merged.append([mod, lo, hi, 0])
    return [(m[0] | (0x10000 if m[3] else 0), m[1], m[2]) for m in merged]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--basemap", type=Path, default=Path(".org/port/artifacts/rel-basemap.txt"),
                    help="retail-truth basemap (basemap mode input)")
    ap.add_argument("--descriptor", type=Path, nargs="?",
                    default=None, const=Path("port/descriptor/rel-modules-data.inc"),
                    help="L-reloc descriptor inc: use its per-module bases "
                         "instead of the retail basemap (EXRAM regen)")
    ap.add_argument("--disc-root", type=Path, default=Path("."))
    ap.add_argument("--out", type=Path, default=Path("port/loader/rel_bands.inc"))
    args = ap.parse_args()

    if args.descriptor is not None:
        sources = parse_descriptor(args.descriptor)
        mode = f"descriptor:{args.descriptor}"
    else:
        sources = parse_basemap(args.basemap)
        mode = f"basemap:{args.basemap}"

    raw = build_bands(sources, [args.disc_root, args.disc_root.parent])
    bands = merge_bands(raw)

    covered = sum(hi - lo for _, lo, hi in bands)
    gaps = 0
    prev_hi: int | None = None
    for _m, lo, hi in bands:
        if prev_hi is not None and lo > prev_hi:
            gaps += lo - prev_hi
        prev_hi = hi
    span_lo = min(lo for _, lo, _ in bands)
    span_hi = max(hi for _, _, hi in bands)

    lines: list[str] = []
    lines.append("// Generated by scripts/port/gen_rel_band_table.py - do not edit.")
    lines.append("// Pulsed by phase3-pulse12 (MODERNGEKKO_BAND_PROTECT): every module's")
    lines.append("// descriptor-fixed L reservation as [lo..hi) guest-RAM bands (file image")
    lines.append("// + aligned linked bss). Sorted ascending, disjoint (touching merged;")
    lines.append("// merged ids carry bit 0x10000).")
    lines.append(f"// Mode: {mode}")
    lines.append(f"// Modules: {len(raw)}  Bands: {len(bands)}  Reserved: {covered} (0x{covered:X}) bytes")
    lines.append(f"// Span: [0x{span_lo:08X}..0x{span_hi:08X})  inter-band gaps: {gaps}")
    lines.append("")
    lines.append("static const RelBand s_rel_bands[] = {")
    for mod, lo, hi in bands:
        tag = f"{mod:#07x}" if mod & 0x10000 else f"{mod}u"
        lines.append(f"    {{{tag}, 0x{lo:08X}u, 0x{hi:08X}u}},")
    lines.append("};")
    lines.append("")
    lines.append("#define REL_BAND_COUNT (sizeof(s_rel_bands) / sizeof(s_rel_bands[0]))")
    lines.append("")
    args.out.write_text("\n".join(lines) + "\n")
    print(f"wrote {args.out}: {len(raw)} modules -> {len(bands)} bands, "
          f"reserved {covered} (0x{covered:X}) bytes, span "
          f"[0x{span_lo:08X}..0x{span_hi:08X}), inter-band gaps {gaps} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
