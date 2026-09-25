# Disc builder (GZLE01)

Builds `gGZLE01_recomp.dll`, the recompiled Wind Waker module, on the
player's PC from their own USA disc. Release bundles ship this kit instead of
the module, so no translated game code is ever distributed.

```
python build.py <extracted disc> [--output DIR] [--status FILE] [--jobs N]
```

The launcher (`DeepSea.exe`) and `Launch-WindWaker.ps1` run it on the
first launch and show its progress; `--status` receives one JSON state per
step.

## What it does

1. **Check the disc.** Every input file (`sys/main.dol`, `files/RELS.arc`,
   `files/rels/*.rel`) must match the SHA-256 in `gzle01.json`.
2. **Translate.** `dolrecomp.exe` turns `main.dol` and the 415 REL modules
   into C with the settings in `gzle01.json` (host hooks, GPR-stub inlining,
   REL bank offset). `patches/dol-generated-h.patch` adds the batch-dispatch
   channel, and `relcall.py` adds cached cross-module call sites.
3. **Generate tables.** Four `gen_*.py` scripts write the module, REL and
   band tables that `module/rel_loader.c` uses.
4. **Verify.** The translated tree must match `expected-sources.json`, the
   hashes of the build that was tested. Anything else stops the build.
5. **Compile.** Clang (`-O2 -march=native`, ThinLTO, optional PGO profile)
   builds the sources plus `module/` and the GXRuntime core into one DLL.
6. **Cache.** The result goes to `<output>/GZLE01/<recipe hash>/`. The hash
   covers the kit files, the tools, the profile and the CPU, so a changed kit
   or a different processor rebuilds. Intermediates are deleted afterwards.

## Kit layout

`make_kit.py` assembles `disc-builder/` for a release:

```
python make_kit.py --out kit --dolrecomp dolrecomp.exe \
    --python-embed python-3.x-embed-amd64.zip \
    --toolchain <llvm-mingw dir> [--profile gzle01.profdata]
```

It copies these scripts, the headers and GXRuntime sources they compile
against, `dolrecomp.exe`, the Windows embeddable Python, and an x86_64-only
subset of llvm-mingw (including its `*.cfg` target defaults). The PGO profile
comes from training runs of the tested module. It holds only function names
and execution counts, and the build works without it (with slower code).
Then run `scripts/package-release.sh --builder-kit kit ...`.

## Tests

`python -m unittest tests.test_builder` (synthetic data only).
