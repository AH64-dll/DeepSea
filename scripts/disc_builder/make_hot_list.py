#!/usr/bin/env python3
"""Write hot-sources.json: the translated REL files the PGO profile saw run.

build.py compiles the game engine (main.dol) and these actor files with the
full optimizer and every other actor file with -O0, which keeps first-launch
setup to minutes. The list holds file names only, no game code.

usage: make_hot_list.py --profile gzle01.profdata --llvm-profdata EXE --generated WORK_DIR
  WORK_DIR is a builder work directory after its generate step (dol/, rel/).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
FUNCTION = re.compile(r"^(?:static )?(?:inline )?void (\w+)\(CPUState\* ctx\) \{", re.M)


def executed_functions(profdata, tool):
    dump = subprocess.run([str(tool), "show", "--all-functions", "--counts", str(profdata)],
                          capture_output=True, text=True, check=True).stdout
    hot, name = set(), None
    for line in dump.splitlines():
        if line.startswith("  ") and not line.startswith("   ") and line.endswith(":"):
            name = line.strip()[:-1]
        elif name and line.strip().startswith("Block counts:"):
            if any(int(n) for n in re.findall(r"\d+", line.split(":", 1)[1])):
                hot.add(name)
    return hot


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", type=Path, required=True)
    ap.add_argument("--llvm-profdata", type=Path, required=True)
    ap.add_argument("--generated", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=HERE / "hot-sources.json")
    a = ap.parse_args()
    hot = executed_functions(a.profile, a.llvm_profdata)
    rels = a.generated / "rel/generated/rels"
    files = []
    for source in sorted(rels.rglob("*.c")):
        if any(fn in hot for fn in FUNCTION.findall(source.read_text(errors="replace"))):
            files.append(source.relative_to(a.generated).as_posix())
    a.out.write_text(json.dumps({"optimize": files}, indent=1) + "\n", encoding="utf-8", newline="\n")
    print(f"make_hot_list: {len(files)} hot actor files -> {a.out}")


if __name__ == "__main__":
    main()
