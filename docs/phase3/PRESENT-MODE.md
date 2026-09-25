# Present mode — PRESENTATION POLICY

This knob selects how the compositor presents an already-produced frame
(MAILBOX / IMMEDIATE / FIFO). It is **never** proof of unlocked FPS;
proof is `fps-gates.md` G2/G4 artifacts (present-mode-validation-protocol.md
§5 binds every knob value to a fresh G2 set).

## Knobs

- CLI: `moderngekko-run --present-mode <fifo|mailbox|immediate>` (case-insensitive, trims).
- `config.ini` (frontend): `[Video] present_mode=fifo|mailbox|immediate` (`present` alias; CLI shadows INI).
- Vendored Dolphin `Config::GFX_PRESENT_MODE` (`Source/Core/Core/Config/GraphicsSettings.*`).

Semantics: `fifo` = `VK_PRESENT_MODE_FIFO_KHR` (vsync queued), `mailbox` = `VK_PRESENT_MODE_MAILBOX_KHR`
(tearing-free latest-wins), `immediate` = `VK_PRESENT_MODE_IMMEDIATE_KHR` (tearing).

Headless (`--headless` → `Null` backend): present mode is `warn-and-ignore` (stderr
`ignored in --headless`), exit ≠2; no swapchain log.

VRR/G-Sync: validated separately; Wayland compositors may collapse MAILBOX to FIFO —
prefer X11 exclusive for faithful measurements (present-mode-validation-protocol.md §4).

Bind: knob flip without a fresh `.org/port/evidence/fps-g2-pacing-*.json` at that
`present_mode` is `INVALID` (not PASS).
