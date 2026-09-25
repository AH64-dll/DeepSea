"""Strict unified-diff application for the builder's own fixed patches.

Every hunk must match its context exactly at the stated position (after
line-ending normalization); anything else is an error, never a fuzzy apply.
"""
from __future__ import annotations

import re
from pathlib import Path

_HUNK = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


def apply_unified_patch(target: Path, patch: Path) -> None:
    lines = target.read_text(encoding="utf-8").splitlines()
    diff = patch.read_text(encoding="utf-8").splitlines()
    out: list[str] = []
    pos = 0  # next unconsumed line of `lines`
    i = 0
    while i < len(diff) and not diff[i].startswith("@@"):
        i += 1
    if i == len(diff):
        raise ValueError(f"{patch}: no hunks")
    while i < len(diff):
        m = _HUNK.match(diff[i])
        if not m:
            raise ValueError(f"{patch}:{i + 1}: expected a hunk header")
        start = int(m.group(1)) - 1 if m.group(2) != "0" else int(m.group(1))
        if start < pos:
            raise ValueError(f"{patch}: overlapping hunks")
        out.extend(lines[pos:start])
        pos = start
        i += 1
        while i < len(diff) and not diff[i].startswith("@@"):
            tag, text = (diff[i][:1], diff[i][1:]) if diff[i] else (" ", "")
            if tag in (" ", "-"):
                if pos >= len(lines) or lines[pos] != text:
                    raise ValueError(f"{target}: patch {patch.name} does not match at line {pos + 1}")
                if tag == " ":
                    out.append(text)
                pos += 1
            elif tag == "+":
                out.append(text)
            elif tag != "\\":  # "\ No newline at end of file"
                raise ValueError(f"{patch}:{i + 1}: unexpected line {diff[i]!r}")
            i += 1
    out.extend(lines[pos:])
    target.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
