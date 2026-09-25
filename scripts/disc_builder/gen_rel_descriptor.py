#!/usr/bin/env python3
"""Generate the ModernGekko REL-module descriptor tables for the Wind Waker
(GZLE01) Phase 2 recomp module.

Inputs (all defaults verified 2026-08-15):
  --basemap   .org/port/artifacts/rel-basemap.txt
              one "  module <id>: <path> -> base 0x<ADDR>" line per REL —
              the address-space ground truth produced by the folder-mode batch
              (gate E1; NEVER regenerated ad hoc).
  --rels-dir  /tmp/port-rel-out/generated/rels
              DolRecomp folder-mode per-module trees (<module>_<id>/
              {generated.h, chunks/chunk_NNNN_rel<idx>_<addr>.c}).
  --disc-root port/
              directory the basemap <path>s are relative to; holds the retail
              disc extraction (disc/GZLE01-full/files/...) whose .rel files
              provide the REL headers (module id, version, section table) and
              the original chunk bytes for the FNV-1a chunk hashes.
  --batch-log /tmp/port-rel-batch.log
              folder-mode batch log; module ids cross-checked against the
              basemap.

Outputs:
  port/descriptor/rel-modules-data.inc — static C arrays:
      s_rel_code_ranges[]   415 ranges, one per executable section (linked
                            addresses; sorted, non-overlapping)
      s_rel_chunk_ranges[]  542 ranges, tile s_rel_code_ranges exactly
      s_rel_chunk_hashes[]  FNV-1a 64 over each chunk's original retail bytes
                            (matches gen_module_tables.py / StaticRecompCore_SMC);
                            emitted in the same address order as
                            s_rel_chunk_ranges[] so index i pairs ranges[i]
                            with the hash of the retail bytes at that span)
      s_rel_module_sections_<id>[]  per-module executable-section tables
      s_rel_modules[]       415 ModernGekkoRelModule entries
  port/descriptor/rel-modules-data.h — extern decls + counts (415u etc.).

The runtime (src/runtime/module.cpp moderngekko_validate_module) enforces the
descriptor contract: module_id != 0, section_count != 0, section_info_offset
>= 0x40, file_size >= 0x40, every section's module_id matches, section_index
< section_count, size != 0, linked_start covered by code_ranges, and
chunk_ranges tile code_ranges exactly. Every check below mirrors that
contract; the tool refuses to emit tables that the runtime would reject.

Never hand-edit the outputs — regenerate with this tool.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from pathlib import Path

FNV1A_OFFSET = 0xCBF29CE484222325
FNV1A_PRIME = 0x100000001B3

REL_SECTION_ENTRY_SIZE = 8
REL_HEADER_MIN = 0x40
CHUNK_STRIDE = 0x4000  # DOLRECOMP_C_DEFAULT_CHUNK_INSTRUCTIONS 4096 * 4


class ValidationError(Exception):
    """Raised on any descriptor-contract violation; nothing is written."""


def be32(data: bytes, off: int) -> int:
    return struct.unpack(">I", data[off : off + 4])[0]


def fnv1a(data: bytes) -> int:
    h = FNV1A_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV1A_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def parse_basemap(path: Path) -> dict[int, tuple[str, int]]:
    entries: dict[int, tuple[str, int]] = {}
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        m = re.match(r"\s*module (\d+): (.+?) -> base 0x([0-9A-Fa-f]+)", line)
        if not m:
            raise ValidationError(f"{path}:{lineno}: unparsable basemap line: {line!r}")
        mid = int(m.group(1))
        if mid in entries:
            raise ValidationError(f"{path}:{lineno}: duplicate module id {mid}")
        entries[mid] = (m.group(2), int(m.group(3), 16))
    if not entries:
        raise ValidationError(f"{path}: no module entries")
    return entries


def parse_batch_ids(path: Path) -> set[int]:
    ids = set()
    for lineno, line in enumerate(path.read_text().splitlines(), 1):
        m = re.match(r"\s*module (\d+):", line)
        if m:
            ids.add(int(m.group(1)))
    return ids


class RelModule:
    __slots__ = (
        "mid", "relpath", "base", "version", "section_count",
        "section_info_offset", "file_size", "sections", "chunks",
        "entry_point", "find_range",
    )

    def __init__(self, mid: int, relpath: str, base: int):
        self.mid = mid
        self.relpath = relpath
        self.base = base
        self.version = 0
        self.section_count = 0
        self.section_info_offset = 0
        self.file_size = 0
        self.sections: list[tuple[int, int, int]] = []  # (index, file_offset, size)
        self.chunks: list[tuple[int, int, int]] = []  # (section_index, start, end)
        self.entry_point = 0
        self.find_range: tuple[int, int] | None = None


def parse_generated_h(rel: RelModule, tree: Path) -> None:
    hdr = (tree / "generated.h").read_text()
    m = re.search(r"#define DOLRECOMP_ENTRY_POINT (0x[0-9A-Fa-f]+)u", hdr)
    if not m:
        raise ValidationError(f"module {rel.mid}: no DOLRECOMP_ENTRY_POINT in generated.h")
    rel.entry_point = int(m.group(1), 16)

    fo = re.search(r"dolrecomp_find_original\(u32 address\) \{(.*?)\n\}", hdr, re.S)
    if not fo:
        raise ValidationError(f"module {rel.mid}: no dolrecomp_find_original in generated.h")
    body = fo.group(1)
    rng = None
    m = re.search(
        r"address >= 0x([0-9A-Fa-f]+)u && address < 0x([0-9A-Fa-f]+)u", body
    )
    if m:
        rng = (int(m.group(1), 16), int(m.group(2), 16))
    else:
        m = re.search(
            r"u32 offset = address - 0x([0-9A-Fa-f]+)u;\s*if \(offset < 0x([0-9A-Fa-f]+)u",
            body,
        )
        if m:
            start = int(m.group(1), 16)
            rng = (start, start + int(m.group(2), 16))
    if rng is None:
        raise ValidationError(
            f"module {rel.mid}: unsupported dolrecomp_find_original shape"
        )
    rel.find_range = rng


def parse_chunks(rel: RelModule, tree: Path, exec_idx: int) -> None:
    chunk_dir = tree / "chunks"
    if not chunk_dir.is_dir():
        raise ValidationError(f"module {rel.mid}: no chunks/ directory in {tree}")
    by_ordinal: dict[int, tuple[int, int]] = {}
    for f in chunk_dir.glob("chunk_*.c"):
        m = re.fullmatch(r"chunk_(\d+)_rel(\d+)_([0-9A-Fa-f]+)\.c", f.name)
        if not m:
            raise ValidationError(f"module {rel.mid}: unparsable chunk name {f.name}")
        ordinal, sec_idx, start = int(m.group(1)), int(m.group(2)), int(m.group(3), 16)
        if sec_idx != exec_idx:
            raise ValidationError(
                f"module {rel.mid}: chunk {f.name} section {sec_idx} != "
                f"executable section {exec_idx}"
            )
        if ordinal in by_ordinal:
            raise ValidationError(f"module {rel.mid}: duplicate chunk ordinal {ordinal}")
        by_ordinal[ordinal] = (sec_idx, start)
    if not by_ordinal:
        raise ValidationError(f"module {rel.mid}: no chunk files")
    ordinals = sorted(by_ordinal)
    if ordinals != list(range(len(ordinals))):
        raise ValidationError(f"module {rel.mid}: chunk ordinals not contiguous")
    starts = [by_ordinal[k] for k in ordinals]
    rel.chunks = [(sec, s, 0) for sec, s in starts]


def load_module(mid: int, relpath: str, base: int | None, disc_root: Path,
                rels_dir: Path) -> RelModule:
    rel = RelModule(mid, relpath, base or 0)
    src = disc_root / relpath
    if not src.is_file():
        raise ValidationError(f"module {mid}: retail REL not found: {src}")
    data = src.read_bytes()
    rel.file_size = len(data)
    if rel.file_size < REL_HEADER_MIN:
        raise ValidationError(f"module {mid}: file too small ({rel.file_size})")

    hdr_id = be32(data, 0x00)
    if hdr_id != mid:
        raise ValidationError(
            f"module {mid}: REL header id {hdr_id} != basemap id {mid} ({relpath})"
        )
    rel.version = be32(data, 0x1C)
    rel.section_count = be32(data, 0x0C)
    rel.section_info_offset = be32(data, 0x10)
    if rel.section_info_offset < REL_HEADER_MIN:
        raise ValidationError(f"module {mid}: section_info_offset < 0x40")
    if rel.section_count == 0:
        raise ValidationError(f"module {mid}: zero sections")
    table_end = rel.section_info_offset + rel.section_count * REL_SECTION_ENTRY_SIZE
    if table_end > rel.file_size:
        raise ValidationError(f"module {mid}: section table out of file bounds")

    execs = []
    for i in range(rel.section_count):
        e = rel.section_info_offset + i * REL_SECTION_ENTRY_SIZE
        raw = be32(data, e)
        size = be32(data, e + 4)
        if raw & 1:  # executable (OS_SECTIONINFO_EXEC)
            off = raw & ~1
            if (size & 3) or (off & 3):
                raise ValidationError(
                    f"module {mid}: exec section {i} not instruction-aligned"
                )
            if off + size > rel.file_size:
                raise ValidationError(f"module {mid}: exec section {i} out of file")
            execs.append((i, off, size))
    if not execs:
        raise ValidationError(f"module {mid}: no executable sections")
    rel.sections = execs

    tree = rels_dir / f"{Path(relpath).stem}_{mid}"
    if not tree.is_dir():
        raise ValidationError(f"module {mid}: missing folder-mode tree {tree}")
    parse_generated_h(rel, tree)

    # One executable section per module on this tree (415/415); the loop below
    # would generalize to several, but chunk files carry rel<idx> so keep the
    # single-section contract explicit.
    if len(execs) != 1:
        raise ValidationError(
            f"module {mid}: expected exactly 1 executable section, found {len(execs)}"
        )
    sec_idx, sec_off, sec_size = execs[0]
    if base is None:
        # Rebased regen: the new batch's packing order is a DolRecomp artifact
        # (path-sorted collection), NOT a flat translate of the E1 basemap. The
        # TREE is the truth for the new layout: the first chunk-file start
        # names the linked section start; entry/offsets come from the retail
        # header. The basemap stays the untouched retail-truth record.
        first_chunk = sorted((tree / "chunks").glob("chunk_*.c"))[0].name
        m = re.search(r"chunk_(\d+)_rel\d+_([0-9A-Fa-f]+)\.c", first_chunk)
        if not m:
            raise ValidationError(f"module {mid}: no parsable chunk name")
        base = int(m.group(2), 16) - sec_off
        rel.base = base
        print(f"  module {mid}: inferred base 0x{base:08X} from tree")
    linked_start = base + sec_off
    linked_end = linked_start + sec_size

    expected = (rel.entry_point, linked_start + sec_size)
    if rel.find_range != expected:
        raise ValidationError(
            f"module {mid}: generated.h range {tuple(hex(x) for x in rel.find_range)} "
            f"!= header-derived {tuple(hex(x) for x in expected)}"
        )
    if not (linked_start <= rel.entry_point < linked_end):
        raise ValidationError(
            f"module {mid}: entry 0x{rel.entry_point:X} outside "
            f"[0x{linked_start:X}, 0x{linked_end:X})"
        )

    parse_chunks(rel, tree, sec_idx)

    # Chunk ranges: starts must stride from the section start; the last chunk
    # ends exactly at the section end.
    starts = [s for _, s, _ in rel.chunks]
    ranges = []
    for k, start in enumerate(starts):
        if start != linked_start + k * CHUNK_STRIDE:
            raise ValidationError(
                f"module {mid}: chunk {k} start 0x{start:X} != "
                f"section start + {k}*0x4000 (0x{linked_start + k * CHUNK_STRIDE:X})"
            )
        end = starts[k + 1] if k + 1 < len(starts) else linked_end
        ranges.append((sec_idx, start, end))
    rel.chunks = ranges
    return rel


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--basemap", type=Path,
                    default=Path("/home/amr/ZeldaDecompile/.org/port/artifacts/rel-basemap.txt"))
    ap.add_argument("--rels-dir", type=Path,
                    default=Path("/tmp/port-rel-out/generated/rels"))
    ap.add_argument("--disc-root", type=Path,
                    default=Path("/home/amr/ZeldaDecompile/port"))
    ap.add_argument("--batch-log", type=Path,
                    default=Path("/tmp/port-rel-batch.log"))
    ap.add_argument("--out-h", type=Path,
                    default=Path("/home/amr/ZeldaDecompile/port/descriptor/rel-modules-data.h"))
    ap.add_argument("--out-inc", type=Path,
                    default=Path("/home/amr/ZeldaDecompile/port/descriptor/rel-modules-data.inc"))
    ap.add_argument("--l-base-rebase", type=lambda v: int(v, 0), default=0,
                    metavar="BASE",
                    help="flat-rebase every REL L-image to a new first-module "
                         "base (e.g. 0x90100000 for the EXRAM second bank); "
                         "0 keeps retail E1 bases. The basemap stays the "
                         "retail-truth record: relocation is applied ONLY to "
                         "the derived descriptor + regenerated chunk tree.")
    args = ap.parse_args(argv)

    basemap = parse_basemap(args.basemap)
    print(f"basemap: {len(basemap)} modules from {args.basemap}")

    if args.batch_log.is_file():
        batch_ids = parse_batch_ids(args.batch_log)
        if batch_ids != set(basemap):
            raise ValidationError(
                f"batch log ids ({len(batch_ids)}) != basemap ids ({len(basemap)})"
            )
        print(f"batch log cross-check: {len(batch_ids)} module ids match basemap")
    else:
        print(f"note: batch log {args.batch_log} not found; id cross-check skipped")

    # Rebased regen: with --l-base-rebase the per-module new bases are INFERRED
    # from the regenerated trees (see load_module); --l-base-rebase then only
    # asserts the second-bank contract and rewrites provenance notes. Without
    # it, behavior is byte-identical to the historical basemap-driven path.
    rebased = bool(args.l_base_rebase)
    if rebased and args.l_base_rebase < 0x90000000:
        raise ValidationError(
            f"--l-base-rebase 0x{args.l_base_rebase:X} is inside/near MEM1; "
            "the L second bank must start at >= 0x90000000 (EXRAM window)"
        )

    rels = []
    for mid in sorted(basemap):
        relpath, _base = basemap[mid]
        rels.append(load_module(mid, relpath,
                                None if rebased else _base,
                                args.disc_root, args.rels_dir))

    # ---- Aggregate validation (mirrors src/runtime/module.cpp) ----
    code_ranges = sorted((r.base + off, r.base + off + size)
                         for r in rels for (_, off, size) in r.sections)
    for i in range(1, len(code_ranges)):
        if code_ranges[i - 1][1] > code_ranges[i][0]:
            raise ValidationError("code ranges overlap or out of order")

    if rebased:
        span_end = max(e for _, e in code_ranges)
        if span_end >= 0x94000000:
            raise ValidationError(
                f"rebased span end 0x{span_end:X} exceeds the 64 MB EXRAM "
                "window [0x90000000..0x94000000)"
            )
        print(f"L-base rebase: tree-inferred bases "
              f"(first base 0x{code_ranges[0][0]:08X}, "
              f"span end 0x{span_end:08X})")

    chunk_ranges = sorted(((r.mid, sec, s, e) for r in rels for (sec, s, e) in r.chunks),
                          key=lambda c: c[2])
    for i in range(1, len(chunk_ranges)):
        if chunk_ranges[i - 1][3] > chunk_ranges[i][2]:
            raise ValidationError("chunk ranges overlap or out of order")

    # chunks_tile_code replica
    ci = 0
    for cstart, cend in code_ranges:
        cursor = cstart
        while ci < len(chunk_ranges) and chunk_ranges[ci][2] < cend:
            _, _, s, e = chunk_ranges[ci]
            if s != cursor or e > cend:
                raise ValidationError(
                    f"chunk [{s:#x},{e:#x}) does not tile code range "
                    f"[{cstart:#x},{cend:#x}) at cursor {cursor:#x}"
                )
            cursor = e
            ci += 1
        if cursor != cend:
            raise ValidationError(
                f"code range [{cstart:#x},{cend:#x}) not fully tiled (cursor {cursor:#x})"
            )
    if ci != len(chunk_ranges):
        raise ValidationError("chunk ranges exceed code ranges")

    ids = [r.mid for r in rels]
    if len(set(ids)) != len(ids):
        raise ValidationError("duplicate module ids in descriptor")
    if any(r.mid == 0 for r in rels):
        raise ValidationError("module id 0 present")

    # Runtime field contract + section coverage per module
    for r in rels:
        if r.section_count == 0 or r.file_size < REL_HEADER_MIN:
            raise ValidationError(f"module {r.mid}: violates size/section contract")
        for sec, off, size in r.sections:
            linked = r.base + off
            if size == 0 or linked + size > 0x100000000:
                raise ValidationError(f"module {r.mid}: section {sec} out of range")
            if not any(linked >= cs and linked < ce for cs, ce in code_ranges):
                raise ValidationError(
                    f"module {r.mid}: section {sec} start 0x{linked:X} not covered"
                )

    # Chunk hashes over the retail bytes — one per chunk, emitted in the SAME
    # order as s_rel_chunk_ranges[] (address order), exactly like
    # gen_module_tables.py / StaticRecompCore_SMC. The runtime
    # (modernegekko_validate_module) walks chunk_ranges in address order and
    # pairs chunk_hashes[chunk_index] with chunk_ranges[chunk_index] by index.
    # Bugfix 2026-08-15: hashes were previously emitted by iterating rels in
    # module-id order, so hashes[i] did NOT correspond to ranges[i] (ranges[0]
    # belonged to module 120 at 0x80500000 while hashes[0] was module 1's
    # f_pc_profile_lst hash at 0x80701DA0).
    rel_by_mid = {r.mid: r for r in rels}
    rel_data = {r.mid: (args.disc_root / r.relpath).read_bytes() for r in rels}
    hashes = []
    for mid, _sec, s, e in chunk_ranges:
        r = rel_by_mid[mid]
        sec_off = r.sections[0][1]
        off = sec_off + (s - (r.base + sec_off))
        hashes.append(fnv1a(rel_data[mid][off : off + (e - s)]))

    total_chunks = sum(len(r.chunks) for r in rels)
    total_sections = sum(len(r.sections) for r in rels)

    # ---- Emit .h ----
    args.out_h.parent.mkdir(parents=True, exist_ok=True)
    batch_note = "(folder-mode batch, E1)."
    if rebased:
        batch_note = (f"(folder-mode batch, L-reloc regen to "
                      f"--l-base-rebase 0x{args.l_base_rebase:X}).")
    span_line = (
        f"// {len(rels)} retail RELs linked at "
        f"0x{min(r.base for r in rels):08X}..0x{max(r.base for r in rels):08X} "
        f"{batch_note}"
    )
    h_lines = [
        "// Generated by scripts/port/gen_rel_descriptor.py — do not edit.",
        "// REL module descriptor declarations for the Wind Waker (GZLE01) recomp module.",
        span_line,
        "#ifndef PORT_REL_MODULES_DATA_H",
        "#define PORT_REL_MODULES_DATA_H",
        "",
        "#include <stdint.h>",
        '#include "moderngekko/module_abi.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
        "// Linked code ranges, one per executable section of each retail REL.",
        "// Sorted ascending, non-overlapping; covered by chunk_ranges exactly.",
        "extern const ModernGekkoRange s_rel_code_ranges[];",
        f"#define REL_MODULE_CODE_RANGE_COUNT {len(code_ranges)}u",
        "",
        "// Chunk ranges tile s_rel_code_ranges exactly.",
        "extern const ModernGekkoRange s_rel_chunk_ranges[];",
        f"#define REL_MODULE_CHUNK_RANGE_COUNT {total_chunks}u",
        "",
        "// FNV-1a 64 over each chunk's original retail bytes (matches",
        "// gen_module_tables.py / StaticRecompCore::VerifyChunk).",
        "extern const uint64_t s_rel_chunk_hashes[];",
        "",
        "// Per-module REL registration (415): module_id/version/section_count/",
        "// section_info_offset/file_size + executable-section tables.",
        "extern const ModernGekkoRelModule s_rel_modules[];",
        f"#define MODULE_REL_MODULE_COUNT {len(rels)}u",
        "",
        "#ifdef __cplusplus",
        "}",
        "#endif",
        "",
        "#endif /* PORT_REL_MODULES_DATA_H */",
        "",
    ]
    args.out_h.write_text("\n".join(h_lines))

    # ---- Emit .inc ----
    inc = []
    inc.append("// Generated by scripts/port/gen_rel_descriptor.py — do not edit.")
    inc.append('// Include after "rel-modules-data.h" (count macros) and after the DOL')
    inc.append("// module_tables.inc; wire the arrays into the descriptor in module_glue.c.")
    inc.append('#include "rel-modules-data.h"')
    inc.append("")
    inc.append("// REL code ranges: one per executable section, linked addresses.")
    inc.append("const ModernGekkoRange s_rel_code_ranges[] = {")
    for s, e in code_ranges:
        inc.append(f"    {{0x{s:08X}u, 0x{e:08X}u}},")
    inc.append("};")
    inc.append("")
    inc.append("// REL chunk ranges: tile s_rel_code_ranges exactly.")
    inc.append("const ModernGekkoRange s_rel_chunk_ranges[] = {")
    for _, _, s, e in chunk_ranges:
        inc.append(f"    {{0x{s:08X}u, 0x{e:08X}u}},")
    inc.append("};")
    inc.append("")
    inc.append("// FNV-1a 64 over each chunk's original retail bytes.")
    inc.append("const uint64_t s_rel_chunk_hashes[] = {")
    for i in range(0, len(hashes), 4):
        row = ", ".join(f"0x{h:X}u" for h in hashes[i : i + 4])
        inc.append(f"    {row},")
    inc.append("};")
    inc.append("")
    for r in rels:
        name = f"s_rel_module_sections_{r.mid}"
        inc.append(f"// {r.mid}: {r.relpath} base 0x{r.base:08X}")
        inc.append(f"static const ModernGekkoRelSection {name}[] = {{")
        for sec, off, size in r.sections:
            linked = r.base + off
            inc.append(
                f"    {{{r.mid}u, {sec}u, 0x{linked:08X}u, 0x{size:08X}u}},"
            )
        inc.append("};")
        inc.append("")
    inc.append("// Per-module REL registration, sorted by module id.")
    inc.append("const ModernGekkoRelModule s_rel_modules[] = {")
    for r in rels:
        inc.append(
            f"    {{{r.mid}u, {r.version}u, {r.section_count}u, "
            f"0x{r.section_info_offset:X}u, 0x{r.file_size:X}u, "
            f"s_rel_module_sections_{r.mid}, {len(r.sections)}u}},"
        )
    inc.append("};")
    inc.append("")
    args.out_inc.write_text("\n".join(inc))

    # ---- Summary ----
    print(f"modules: {len(rels)}")
    print(f"executable sections: {total_sections} (1 per module)")
    print(f"chunks: {total_chunks}")
    print(f"code range span: 0x{code_ranges[0][0]:08X}..0x{code_ranges[-1][1]:08X}")
    print(f"entry points: {sum(1 for r in rels if r.entry_point)} / {len(rels)} within range")
    print(f"written: {args.out_h} ({args.out_h.stat().st_size} bytes)")
    print(f"written: {args.out_inc} ({args.out_inc.stat().st_size} bytes)")
    print("VALIDATION: OK (415 modules, unique ids, sections in code ranges, "
          "chunks tile exactly)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValidationError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
