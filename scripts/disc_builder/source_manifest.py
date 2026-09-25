"""Hash manifest of the generated game sources.

The builder regenerates the game code from the player's disc. Comparing each
generated file with the hash recorded from the tested build proves the local
DLL is compiled from exactly the code that was played and measured. Only
hashes are stored: the manifest contains no game code.

Line endings are normalized, and table comment lines are ignored because they
embed local paths.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

CODE_SUFFIXES = (".c", ".h")


def _normalized(path: Path, drop_comments: bool) -> bytes:
    data = path.read_bytes().replace(b"\r\n", b"\n")
    if drop_comments:
        data = b"\n".join(l for l in data.split(b"\n") if not l.lstrip().startswith(b"//"))
    return data


def collect(work: Path) -> dict[str, str]:
    """Map 'dol/…', 'rel/…', 'tables/…' relative paths to SHA-256."""
    out = {}
    for root, label in ((work / "dol/generated", "dol"), (work / "rel/generated/rels", "rel")):
        for path in sorted(root.rglob("*")):
            if path.is_file() and path.suffix in CODE_SUFFIXES:
                rel = path.relative_to(root).as_posix()
                out[f"{label}/{rel}"] = hashlib.sha256(_normalized(path, False)).hexdigest()
    for path in sorted((work / "tables").glob("*")):
        if path.is_file() and path.suffix in (".h", ".inc"):
            out[f"tables/{path.name}"] = hashlib.sha256(_normalized(path, True)).hexdigest()
    return out


def verify(work: Path, manifest: Path) -> list[str]:
    """Return a sorted list of mismatching / missing / unexpected paths."""
    expected = json.loads(manifest.read_text(encoding="utf-8"))["files"]
    actual = collect(work)
    bad = {p for p in expected if actual.get(p) != expected[p]}
    bad |= set(actual) - set(expected)
    return sorted(bad)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: source_manifest.py <build-work-dir> <out.json>")
    files = collect(Path(sys.argv[1]))
    Path(sys.argv[2]).write_text(json.dumps({"game": "GZLE01", "files": files}, indent=1) + "\n",
                                 encoding="utf-8", newline="\n")
    print(f"{len(files)} files")
