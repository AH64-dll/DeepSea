#!/usr/bin/env bash
# make-source-archive.sh — write the complete corresponding source of a
# ModernGekko release as one tarball: the ModernGekko tree at HEAD, the
# vendored Dolphin tree (vendor/dolphin) at its recorded commit, and every
# initialized submodule below it at ITS recorded commit. It archives
# commits, never the working tree, so it refuses to run while any of those
# trees has uncommitted changes to tracked files (the archive would then not
# match what was built).
#
# Excluded: Externals/Qt and Externals/FFmpeg-bin, prebuilt binary
# packages that the Windows mingw build does not use (ENABLE_QT=OFF,
# ENCODE_FRAMEDUMPS=OFF), and Externals/mGBA/mgba (USE_MGBA=OFF), whose
# emulator test suite carries save files from commercial games. Game code and data are never part of the tree.
#
# Usage:
#   scripts/make-source-archive.sh --out <file.tar.gz> [--allow-dirty]
#
# Runs under Git Bash / MSYS2 on Windows or any POSIX shell with git + tar.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT=""
ALLOW_DIRTY=0
EXCLUDE_RE='^vendor/dolphin/Externals/(Qt|FFmpeg-bin|mGBA/mgba)$'

die() { echo "make-source-archive: ERROR: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --out)         OUT="$2"; shift 2 ;;
    --allow-dirty) ALLOW_DIRTY=1; shift ;;
    -h|--help)     sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *)             die "unknown argument: $1" ;;
  esac
done
[ -n "$OUT" ] || die "--out <file.tar.gz> is required"
case "$OUT" in *.tar.gz|*.tgz) ;; *) die "--out must end in .tar.gz" ;; esac

# git refuses repos owned by another account (e.g. submodules cloned from an
# elevated shell). This script only reads commits, so trust exactly the
# trees it archives for its own invocations; the user's config is untouched.
g() { local dir="$1"; shift; git -c safe.directory="$dir" -C "$dir" "$@"; }

commit="$(g "$MG_ROOT" rev-parse HEAD)"
short="${commit:0:12}"
prefix="DeepSea-src-$short"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
# One tar per repository straight from `git archive --prefix`, concatenated
# at the end: nothing is extracted, so symlinks survive even on Windows.
parts=0

dirty_list=""
# archive_repo <abs repo dir> <commit> <relative path in the archive>
archive_repo() {
  local dir="$1" rev="$2" rel="$3"
  if [ -n "$(g "$dir" status --porcelain --untracked-files=no --ignore-submodules=all)" ]; then
    dirty_list="$dirty_list ${rel:-.}"
  fi
  g "$dir" archive --format=tar --prefix="$prefix/${rel:+$rel/}" "$rev" > "$stage/part-$(printf %04d "$parts").tar"
  parts=$((parts + 1))
  # Recurse into initialized submodules at the commit recorded in $rev.
  local line mode sha path
  while IFS= read -r line; do
    [ -n "$line" ] || continue
    mode="${line%% *}"; line="${line#* }"
    line="${line#* }"; sha="${line%%	*}"; path="${line#*	}"
    [ "$mode" = "160000" ] || continue
    local sub_rel="${rel:+$rel/}$path"
    if [[ "$sub_rel" =~ $EXCLUDE_RE ]]; then
      echo "  skip  $sub_rel (unused binary package)"
      continue
    fi
    if [ ! -e "$dir/$path/.git" ]; then
      echo "  skip  $sub_rel (not initialized)"
      continue
    fi
    g "$dir/$path" cat-file -e "$sha^{commit}" 2>/dev/null ||
      die "$sub_rel: recorded commit $sha is missing from the local clone"
    echo "  add   $sub_rel @ ${sha:0:12}"
    archive_repo "$dir/$path" "$sha" "$sub_rel"
  done < <(g "$dir" ls-tree -r "$rev" | grep '^160000 ' || true)
}

echo "make-source-archive: Deep Sea @ $short"
archive_repo "$MG_ROOT" "$commit" ""

if [ -n "$dirty_list" ]; then
  if [ "$ALLOW_DIRTY" -eq 1 ]; then
    echo "make-source-archive: WARN: uncommitted changes NOT included in:$dirty_list" >&2
  else
    die "uncommitted changes in:$dirty_list (commit them, or pass --allow-dirty)"
  fi
fi

mkdir -p "$stage/$prefix"
cat > "$stage/$prefix/SOURCE-MANIFEST.txt" <<EOF
Deep Sea complete corresponding source
commit: $commit
generated: $(date -u +%Y-%m-%dT%H:%M:%SZ)

Build: see README.md and docs/ in this tree. The Windows release is built with
llvm-mingw (clang) via CMake + Ninja. Excluded as unused by that build:
vendor/dolphin/Externals/Qt, vendor/dolphin/Externals/FFmpeg-bin, vendor/dolphin/Externals/mGBA/mgba.
EOF

tar -C "$stage" -cf "$stage/part-manifest.tar" "$prefix/SOURCE-MANIFEST.txt"
combined="$stage/combined.tar"
cp "$stage/part-0000.tar" "$combined"
for part in "$stage"/part-0*.tar "$stage/part-manifest.tar"; do
  [ "$part" = "$stage/part-0000.tar" ] && continue
  tar -A -f "$combined" "$part"
done
mkdir -p "$(dirname "$OUT")"
gzip -9 -c "$combined" > "$OUT"
echo "make-source-archive: wrote $OUT ($(du -h "$OUT" | cut -f1))"
