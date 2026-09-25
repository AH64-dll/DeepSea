Deep Sea - The Legend of Zelda: The Wind Waker (USA) - Windows release
============================================================================

Double-click "Play Deep Sea.cmd". It opens the Deep Sea launcher:
pick your game folder once (the extracted retail USA disc: must contain
sys\main.dol and files\), choose a controller and graphics options, and
press Play. Your choices are saved in assets\user-dir\.

Controllers: an Xbox-style (XInput) pad on port 1 is the default. Open the
launcher's Controllers section to pick the device for each of ports 1-4
(XInput pads, DirectInput pads, keyboard), add a second controller, remap
any GameCube button/stick/trigger/D-pad by clicking it and pressing the
control you want, or reset to defaults. Default Xbox layout: A=A, B=B, X=X,
Y=Y, RB=Z, Start=Start, LT/RT=L/R, left stick=Main, right stick=C-stick.
The profile is saved to assets\user-dir\Config\GCPadNew.ini.

Cutscene skip: hold Z + START together (Xbox pad: RB + Start) during a
cutscene. Works for the new-game storybook prologue and for in-game
story/dialogue cutscenes. A plain Start press still pauses as normal.
Skipping an event early skips whatever that scene would have done at its
end, like any abort. Set MODERNGEKKO_CUTSCENE_SKIP=0 to disable it.

New game: after naming Link, the storybook prologue plays (about 3.5
minutes, skippable as above), then the game continues on Outset Island.

Performance defaults: Vulkan backend, auto-selects the discrete GPU on
hybrid laptops (override in the launcher), 60 fps frame interpolation on,
multi-threaded shader compile, disc reads cached. Config files are in
assets\user-dir\Config\ (Dolphin.ini, GFX.ini, Logger.ini).

Graphics: the launcher exposes Window mode (Fullscreen / Maximized /
Windowed), Aspect ratio, Internal resolution, Anti-aliasing (Off / FXAA /
MSAA 2x / 4x / 8x) and Anisotropic filtering; they persist to
assets\user-dir\config.ini. Shipped defaults are fullscreen, 16:9,
1920x1080 internal resolution and FXAA. With Aspect ratio = 16:9 the
bundled widescreen16x9 mod (a native port of Dolphin's official Wind Waker
widescreen Gecko code) widens the game's camera and relocates the HUD -
true widescreen rather than a stretched 4:3 image; the generic
"widescreen hack" stays off because stacking the two would widen twice.
Set MODERNGEKKO_WIDESCREEN=0 to disable the mod (the picture then falls
back to a 16:9 stretch of the 4:3 frame), or pick 4:3 / Auto to play
exactly as the original. MODERNGEKKO_AA=off|fxaa|2x|4x|8x overrides the
anti-aliasing choice for one run.

Logs: launcher writes assets\user-dir\Logs\DeepSea.log; the game writes
logs\ and (with file logging enabled) assets\user-dir\Logs\dolphin.log.
A native crash writes logs\crash-*.dmp + crash-*.txt next to the exe.

Troubleshooting (2026-09-22 repair pass):
- Controller not read: the pad profile is assets\user-dir\Config\GCPadNew.ini
  and must bind Device = XInput/0/Gamepad (the known-good template is
  GCPadNew.ini.xinput). Re-picking the device in the launcher regenerates it.
  If an old session crashed during QA input injection, delete any leftover
  file in assets\user-dir\Pipes\ - a stale pad0 can replay a stuck button.
- "It doesn't begin again": a leftover moderngekko-run.exe from a crashed
  session blocks every new start (runtime singleton). The launcher .cmd and
  Launch-WindWaker.ps1 now detect leftover runners and offer to end them
  (scripted: powershell -File Launch-WindWaker.ps1 -KillStaleRunners).
- Screen flickers / flashes black-and-white: caused by a stale
  Vulkan-Pipeline-*.cache (quarantined; driver pipeline disk cache stays
  DISABLED - never set MODERNGEKKO_VK_PIPELINE_CACHE=1) and by immediate
  present mode (now VSync + --present-mode fifo).
- Boot crashes were traced to the in-game analytics report builder; analytics
  is disabled in assets\user-dir\Config\Dolphin.ini ([Analytics] Enabled =
  False). Keep it off.
- Savestates: never load captures from the playsea-p14 family (deterministic
  crash on restore) or the ovlphang-d7 / pulse3-d0 dev captures (load fails);
  watch logs for "[state] load FAILED".

Advanced: powershell -File Launch-WindWaker.ps1 launches the runner
directly (-ChooseGame reopens the folder picker, -ValidateOnly checks a
path, -GameRoot <dir> sets it explicitly). moderngekko-run.exe accepts
--adapter/--graphics/--mods/--load-state/--uncapped and friends; see
--help. Debug env knobs: MODERNGEKKO_PANIC_ALERTS=1 re-enables modal panic
dialogs; MODERNGEKKO_FRAME60_ACCUM=0 disables the 60 fps mod;
MODERNGEKKO_WIDESCREEN=0 disables the widescreen mod;
MODERNGEKKO_VK_PIPELINE_CACHE=1 re-enables the Vulkan driver pipeline disk
cache (off by default - a stale blob could persist black-screen-causing
pipeline entries across boots; shader caches are unaffected).
