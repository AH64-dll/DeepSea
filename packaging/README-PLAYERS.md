# Deep Sea — The Legend of Zelda: The Wind Waker (USA) — players' guide

A Windows x64 native port: the game's PowerPC code is recompiled to x64
(`gGZLE01_recomp.dll`) and runs on a Dolphin-derived runtime. No game data is
included — you must supply your own legally dumped copy.

## Requirements

- Windows 10/11, 64-bit.
- A GPU with Vulkan 1.1+ and a working Vulkan runtime (`vulkan-1.dll` is
  provided by your GPU driver, or install the LunarG Vulkan runtime).
- **Your own retail USA disc (GZLE01)**, dumped by you. European (GZLP01)
  and Japanese (GZLJ01) dumps are not supported by the recompiled module.

## Game data — bring your own disc

The port runs an *extracted* disc, not an ISO container. Two ways to get one:

- Extract your disc with Dolphin ("Extract Disc") or `dolphin-tool` so the
  folder contains `sys\main.dol`, `sys\boot.bin`, and `files\`.
- Or let the launcher do it: `DeepSea.exe` → "Browse for ISO / WBFS /
  RVZ" → "Extract and Play" (extracts a `.iso`, `.wbfs`, or `.rvz` you own
  into the user directory).

Then double-click **`Play Deep Sea.cmd`** and pick the extracted folder
once; the path is saved in `assets\user-dir\default-game.txt`. Direct
launch: `powershell -File Launch-WindWaker.ps1` (`-ChooseGame` re-picks,
`-ValidateOnly` checks a path, `-GameRoot <dir>` sets it explicitly).

## First launch: game setup (bundles with a `disc-builder` folder)

Some bundles ship without `gGZLE01_recomp.dll`. They carry a
`disc-builder\` folder instead, which builds that file on your PC from your
own disc the first time you press Play. It checks that the disc files are
the USA release, translates the game's code, and compiles it with the
bundled compiler. Nothing is downloaded.

- It takes about 10 minutes, depending on your CPU (7 to 9 minutes on a
  6-core laptop). A progress bar
  shows in `DeepSea.exe`; `Play Deep Sea.cmd` shows it in the
  console. Cancel is safe, and the next Play starts over.
- It needs about 2 GB free while it runs. Afterwards it keeps about
  500 MB in `assets\user-dir\modules\`.
- Later launches check the saved build in a few seconds and start right
  away. A new version of the kit builds again.
- The build is tuned for this PC's processor. On a PC with a different
  processor, the bundle builds again by itself.
- If setup fails, the message names the step. Its logs stay in the newest
  `assets\user-dir\modules\GZLE01\build-*` folder until the next attempt.
  A "differs from the tested build" error means the translated code did not
  match the release this bundle was tested with, so it refuses to run it.

## Where settings and saves live

By default everything mutable is under `assets\user-dir\` inside the bundle —
config (`Config\Dolphin.ini`, `GFX.ini`, `Logger.ini`, `GCPadNew.ini`),
savestates, memory cards, logs. If you install somewhere read-only (e.g.
`Program Files`), relocate it:

    moderngekko-run.exe --user-dir "%LOCALAPPDATA%\DeepSea\user-dir"

`DeepSea.exe` accepts the same `--user-dir` switch. Without the bundled
`assets\user-dir`, the runner falls back to `%LOCALAPPDATA%\moderngekko`.

## Controls

Default: an Xbox-style (XInput) pad on port 1 — A=A, B=B, X=X, Y=Y, RB=Z,
Start=Start, LT/RT=L/R, left stick=Main Stick, right stick=C-Stick, D-Pad
matches, rumble on both motors. Open the launcher's **Controllers** section
to pick the device for each port (XInput, DirectInput, keyboard), remap any
GameCube control by clicking it and pressing a control, or reset to defaults.
The profile is saved to `assets\user-dir\Config\GCPadNew.ini` (the
`.xinput` file beside it is a known-good template). With no pad profile,
keyboard defaults apply.

**Cutscene skip:** hold **Z + START** (Xbox: **RB + Start**) — works on the
new-game storybook and in-game events. `MODERNGEKKO_CUTSCENE_SKIP=0` disables.

## Hotkeys (in-game window)

| Key | Action |
|-----|--------|
| F1 | Save state (File menu; load via File → Load State, the launcher picker, or `--load-state`) |
| F11 / Alt+Enter | Toggle fullscreen |
| Esc | Leave fullscreen (windowed: quit) |
| Space (hold) | Fast-forward 2x while held |
| File → Pause, View → Mute | menu equivalents |

Savestates are build-specific — only load captures made by this build; a
failed load falls back to a fresh boot (watch for `[state] load FAILED` in
`logs\`).

## Graphics, widescreen, 60 fps

Shipped defaults: Vulkan, fullscreen, 16:9, 1920×1080 internal resolution,
FXAA. The launcher exposes window mode, aspect ratio, internal resolution,
anti-aliasing (Off/FXAA/MSAA), and anisotropic filtering; they persist to
`assets\user-dir\config.ini` (`[Video] aspect_ratio`, `aa=`, ...).

With `aspect_ratio=16:9` the bundled **widescreen16x9** mod (a native port of
Dolphin's GZLE01 widescreen Gecko code) widens the camera and relocates the
HUD — true widescreen, not a stretch. The generic `wideScreenHack` is forced
off so the two never stack. `MODERNGEKKO_WIDESCREEN=0` disables the mod
(falls back to a 16:9 stretch); `aspect_ratio=4:3` plays exactly as retail.

The **frame60-accum** mod renders at 60 fps with interpolated frames
(default on). Kill switches if you hit a rendering artifact:

| Env var | Default | Effect when `=0` |
|---------|---------|------------------|
| `MODERNGEKKO_FRAME60_ACCUM` | 1 | disable the 60 fps mod entirely |
| `MODERNGEKKO_J3D_INTERP` | 1 | disable model interpolation |
| `MODERNGEKKO_RFRAME_RENDER` | 1 | disable interpolated-frame rendering |
| `MODERNGEKKO_F60_FADER_FIX` | 1 | restore pre-fix fader handling |
| `MODERNGEKKO_F60_SEA_FIX` | 1 | restore pre-fix sea animation cadence |
| `MODERNGEKKO_F60_LIGHT_FIX` | 1 | restore pre-fix interpolated relighting |
| `MODERNGEKKO_F60_VI_LOCK` | 1 | restore tick-only frame pacing (not locked to the display refresh) |
| `MODERNGEKKO_F60_DISPLAY_ALPHA` | 1 | restore accumulator-based interpolation weights |
| `MODERNGEKKO_F60_AUTO_DEGRADE` | 1 | keep 60 fps interpolation even when the PC is too slow (the game then runs in slow motion instead of dropping to 30 fps) |
| `MODERNGEKKO_WIDESCREEN` | auto (16:9) | disable the widescreen mod |
| `MODERNGEKKO_CUTSCENE_SKIP` | 1 | disable Z+Start cutscene skip |
| `MODERNGEKKO_AA` | unset | `off\|fxaa\|2x\|4x\|8x` for one run |
| `MODERNGEKKO_PANIC_ALERTS` | 0 | `=1` re-enables modal panic dialogs |

Do **not** set `MODERNGEKKO_VK_PIPELINE_CACHE=1` — the Vulkan driver pipeline
disk cache is deliberately off (a stale blob caused black screens).

## Logs, licenses, source

Logs: `logs\<ts>.stdout/stderr.log` beside the exe plus
`assets\user-dir\Logs\{DeepSea.log,dolphin.log}`; crashes write
`logs\crash-*.dmp/.txt`. License texts are in `licenses\`; `SOURCE-OFFER.txt`
explains how to obtain the complete corresponding source (GPLv3/GPLv2+).
`provenance.json` records the exact commits and `SHA256SUMS` lets you verify
every file (`sha256sum -c SHA256SUMS`).

## Credits

Deep Sea is built on the ModernGekko runtime by Hyperway (ExpansionPak) and
contributors, RecompCore, DolRecomp and the Dolphin emulator. The Wind Waker
port draws on the zeldaret decompilation of the game
(https://github.com/zeldaret/tww) for its understanding of the game's code.
