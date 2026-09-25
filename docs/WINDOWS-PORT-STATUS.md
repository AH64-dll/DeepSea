# Windows port status — GZLE01 (Wind Waker US)

Status as of the `perf/integration` merge candidate. Everything below is
backed by a real run on this machine; unverified items are called out
explicitly.

## Verified

- **Boot -> title screen renders natively.** `moderngekko-run` +
  `build-module/gGZLE01_recomp.dll` + `--game <gzle01-full>` produces the
  real title screen (logo, "PRESS START", King of Red Lions, Link on the
  Outset cliff). (The verification capture is kept out of the tree: it is
  Nintendo artwork.)
- **The solid-green screen is gone on the reference module.** The green
  fill was the YUYV decode of an all-zero XFB (Y=U=V=0 -> RGB 0,135,0).
  With the reference module the guest issues full-frame 640x480 XFB copies
  from frame 1 and presents real pixels.
- **VI pipeline ticks at ~60 Hz.** `[vi] fields` lines hold ~59.9 Hz
  through boot, the title screen, and into the attract intro
  (`Audiores/Stream/1tale.afc` streaming autonomously).
- **The render loop is real.** `MODERNGEKKO_GP_TRACE=1` counted
  ~7.6M draw calls / ~33M vertices over a 50 s headless run.
- **XFB copies are full-frame.** `MODERNGEKKO_XFB_TRACE=1` shows
  `dest=0x004C8EC0/0x0055EEC0 stride=1280 w=640 h=480` alternating —
  correct double-buffered XFB.
- **Tests pass**: `moderngekko_frontend_config_test`,
  `moderngekko_frontend_config_gamecube_test`,
  `moderngekko_dol_patch_test`, `moderngekko_launcher_savestates_test`
  all exit 0 on the llvm-mingw Release preset.
- **Tools run**: `moderngekko-port`, `moderngekko-module-info`,
  `moderngekko-run` all print usage and launch.

## Root cause of the green screen (two layers)

1. **VI shadow-register flush was gated off.** The recompiled guest
   programs the SDK `shdwRegs` file + HorVer framebuffer block and is
   meant to flush them to hardware VI registers from
   `__VIRetraceHandler` each field. In this build that flush is off, so
   the real VI registers stayed at reset and no XFB was ever scanned out.
   Fixed host-side by `VideoInterfaceManager::SyncGuestVI()`
   (`vendor/dolphin/Source/Core/Core/HW/VideoInterface.cpp`), called from
   `OutputField` before scanout. It is gated to
   `IsModuleActive() && GetGameID() == "GZLE01"` because the shadow/HorVer
   addresses come from the GZLE01 `framework.map`.
2. **A green screen still appears when the guest crashes before its
   first frame.** With the experimental `gGZLE01_recomp-dcall.dll` bench
   module the guest takes an unhandled DSI exception during boot
   (memory-card probing; context dump shows a poisoned LR), never submits
   a draw call, and the XFB stays zeroed -> green. That module is a
   bench-out experiment, not the repo's deliverable — but it means any
   "VI/s" numbers recorded against it measured a dead game ticking VI,
   not a running one.

## Build & run (Windows, llvm-mingw + Ninja)

```bat
cmake --preset <your-preset>          # see CMakePresets.json
ninja -C build-windows\build-windows-mingw-release-opt-sourcefix
```

Run a game directory (extracted GZLE01 with `sys/main.dol`):

```bat
moderngekko-run.exe --game D:\path\to\gzle01-full ^
  --module <path>\gGZLE01_recomp.dll ^
  --user-dir <path>\userdir --graphics vulkan
```

Headless smoke (no window):

```bat
moderngekko-run.exe --game ... --module ... --headless
```

Diagnostics (env vars, all off by default):

- `MODERNGEKKO_XFB_TRACE=1` — BP copy-config writes + XFB copy dest/dims.
- `MODERNGEKKO_GP_TRACE=1` — periodic draw/vertex liveness counter.

To silence the invalid-read warning dialog, set
`Config/Dolphin.ini` -> `[Interface] UsePanicHandlers = False` in the
user dir.

## UTF-8 / path handling (this merge)

- `include/moderngekko/utf8_path.hpp` provides `Utf8ToWide`,
  `WideToUtf8`, `Utf8ToPath`, `PathToUtf8`, `GetEnvPath`,
  `CommandLineToUtf8Argv`.
- `moderngekko-port` now builds its command line as UTF-16 and calls
  `CreateProcessW` (was `CreateProcessA` + ANSI `.string()`, which broke
  on non-ANSI-codepage paths). `Quote()` follows `CommandLineToArgvW`
  escaping; `QuotePath()` handles paths. `active-module.txt` is read and
  written as UTF-8. `PortMain` rebuilds argv via `CommandLineToUtf8Argv`.
- `moderngekko-module-info` uses the shared `CommandLineToUtf8Argv`
  (falls back to CRT argv on failure). `shell32` is linked for
  `CommandLineToArgvW`.
- `frontend_config` uses `GetEnvPath` for env paths.

## Known issues / honest gaps

- **Experimental bench modules are unstable.** `gGZLE01_recomp-dcall*.dll`
  (in `bench-out`, outside the repo) crashes the guest at boot — do not
  benchmark or ship against them. Rebuild modules from the in-repo
  pipeline (`build-module` preset output works).
- **Performance is not yet at target everywhere.** Title/attract ran
  ~20-42 FPS present rate on this machine's iGPU at ~1245x747; the plan60
  docs document an Outset-scene gap vs the 60 target. VI holds ~60 Hz,
  but presented FPS and VI rate are different axes.
- **Not yet verified**: in-game input, save/load round-trip, scene
  transitions past the attract demo, fullscreen, real controller input,
  packaged-installer flow, and long soak under load. The title screen
  and attract intro are verified; gameplay beyond that is not.
- **Build warnings** remain: a `fatal: bad revision '^master'` message
  from the vendor `ScmRevGen` step (cosmetic — the checkout has no
  `master` ref), deprecated `volatile` increments in `GXRuntime`, and
  duplicate `WIN32_LEAN_AND_MEAN`/`NOMINMAX` defines. None block the build.
- **`SyncGuestVI` is a GZLE01-specific workaround.** It is gated on the
  game ID and documented as such; a different recompiled game needs its
  own shadow-layout mapping (or the guest's own retrace flush restored).

## Readiness assessment

The runtime + reference module boot GZLE01 to a correctly-rendered title
screen at ~60 VI/s with real geometry and double-buffered XFB — the
headline green-screen defect is fixed and visually verified. The merge is
sane to take as a checkpoint. It is **not** a finished production port:
the perf-target module is currently broken, input/save/packaging are
unverified, and Outset performance still misses 60 FPS. Do not present
this as "done"; present it as "renders correctly, verified on the
reference module".
