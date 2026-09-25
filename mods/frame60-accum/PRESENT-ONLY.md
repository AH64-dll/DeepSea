# PRESENT-ONLY — making render-only frames nearly free

> **Status: implemented and verified working.** The shipped design differs
> from §4's original D1 (a mid-function hook at `0x8000AFB4`) — that approach
> livelocks: `0x8000AFB4` is itself a `bl` call *site*, and hooking a call-site
> instruction makes the chassis↔JIT yield/passthrough loop forever at that
> address. The implemented D1 is a **Painter-entry tail-call** — see §10.

Investigation of the GZLE01 draw path for the `frame60-accum` mod. Goal: on
render-only (R) iterations, skip scene draw submission entirely and let VI
re-present the last completed XFB, while logic (L) frames keep working normally.

All addresses are GZLE01 DOL virtual addresses (`config/GZLE01/symbols.txt` +
disassembly of `orig/GZLE01/sys/main.dol`).

---

## 1. Frame pipeline (verified against DOL disassembly)

`fapGm_Execute` → `fpcM_Management(0, fapGm_After)` + `cCt_Counter(0)`
(`src/f_ap/f_ap_game.cpp:76-80`). `fpcM_Management` body
(`src/f_pc/f_pc_manager.cpp:266-295`, fn 0x8003EC84), disassembled order:

| addr        | call                             | role |
|-------------|----------------------------------|------|
| 0x8003ECA0  | MtxInit (0x802539A4)             |      |
| 0x8003ECA4  | checkDvdCondition (0x8003EBD4)   | DVD-error path calls `JFWDisplay::beginRender`/`endRender` itself (`f_pc_manager.cpp:186-237`) |
| 0x8003ECB0  | **cAPIGph_Painter** (0x8024135C) | → `mDoGph_Painter` **0x8000AF2C** — the whole renderer |
| 0x8003ECB4  | fpcDt_Handler (0x8003D314)       | delete queue (void) — already gated H4 |
| 0x8003ECB8  | fpcPi_Handler (0x8003FF00)       | s32, asserted non-zero — already gated H5 |
| 0x8003ECF4  | fpcCt_Handler (0x8003D150)       | BOOL, asserted non-zero — already gated H6 |
| 0x8003ED40  | callBack1 `bctrl` (r30=0)        | unused |
| 0x8003ED4C  | fpcEx_Handler (0x8003D7E0)       | iterates `fpcM_Execute` 0x8003E370 per process — gated per-process H2 |
| 0x8003ED60  | **fpcDw_Handler** (0x800404CC)   | `(fpcM_DrawIterater 0x8003E338, fpcM_Draw 0x8003E318)` — NOT gated today |
| 0x8003ED74  | callBack2 `bctrl` (r31)          | = fapGm_After 0x800231BC — already gated H3 |

Then `cCt_Counter` (0x802449AC, gated H7), `mDoAud_Execute`/`mDoCPd_Read`
(gated H8/H9 in `main01`, `src/m_Do/m_Do_main.cpp`).

**Ordering fact that matters most:** the renderer runs at the *top* of the
management call, the draw-iterate at the *bottom*. The two are pipelined:

- `fpcDw_Handler` → `cAPIGph_BeforeOfDraw` (0x8024138C) →
  `mDoGph_BeforeOfDraw` (0x800083EC) → `dScnPly_BeforeOfPaint` (0x800083C0)
  → `dComIfGd_reset()` → `dDlst_list_c::reset` **0x80086368**
  (`src/m_Do/m_Do_graphic.cpp:263-272`, `src/d/d_drawlist.cpp:1959-1988`).
  `reset()` calls `J3DDrawBuffer::frameInit` (0x802EC8AC,
  `src/JSystem/J3DGraphBase/J3DDrawBuffer.cpp:49-55`) on all 15 draw buffers,
  rewinds the 2D pointer arrays, resets alpha/spot/light models and the shadow
  control.
- The Draw methods (via `fpcDw_Execute` 0x8004042C → each process's
  `mpDrawFunc`, `src/f_pc/f_pc_draw.cpp:12-31`) then *enqueue* packets into
  those buffers (`J3DDrawBuffer::entry`, `entryMatSort`, `entryImm`) and run
  `J3DModel::calc`/`viewCalc`/DL diffs.
- `mDoGph_Painter` **next iteration** walks the buffers
  (`J3DDrawBuffer::draw` 0x802ECCE4 / `drawTail` 0x802ECDB0 →
  `J3DPacket::draw` → `J3DDisplayListObj::callDL` → `GXCallDisplayList`
  0x80326B80) — i.e. **packet lists are produced by fpcDw at iteration N and
  consumed by Painter at iteration N+1.** One full iteration of latency is
  retail behaviour.

`fpcDw_Handler` itself (`src/f_pc/f_pc_draw.cpp:33-40`):

```cpp
BOOL fpcDw_Handler(fpcDw_HandlerFuncFunc i_iterHandler, fpcDw_HandlerFunc i_func) {
    cAPIGph_BeforeOfDraw();            // dComIfGd_reset — clears all lists
    ret = i_iterHandler(i_func);       // fpcLyIt_OnlyHere(root, fpcM_Draw)
    cAPIGph_AfterOfDraw();             // mDoGph_AfterOfDraw — see below
    return ret;                        // caller ignores it (no assert at 0x8003ED60)
}
```

`mDoGph_AfterOfDraw` (0x80008410, `src/m_Do/m_Do_graphic.cpp:274-320`):
ProcBar/DbPrint visibility, GX state restore (ZCompLoc/ZMode/Blend/AlphaCompare/
Fog/ZTexture/Dither/ClipMode/CullMode), render-mode update,
`dComIfGd_peekZdata()` (`dDlst_peekZ_c::peekData`, 0x80085BFC — `GXPeekZ` per
registered entry), `mDoGph_gInf_c::endFrame()` → `JFWDisplay::endFrame`
(0x80255B58 → for double: `JFWGXDrawDoneAutoAbort` + `GXFlush` + costFrame).

## 2. Why today's R-frames still cost a full frame

With current gates (logic/process/handlers skipped, draw NOT gated), an
R-iteration still runs:

1. `mDoGph_Painter` in full: `beginRender` (XFB exchange copy), all
   `J3DDrawBuffer::draw`/`dDlst_list_c::draw` packet submission, particle
   draws, post-FX (drawDepth `GXCopyTex`, motionBlur, alphaBuffer, spot),
   `calcWipe`, 2D lists, `endRender`/`endGX` (fader state advance + overlay
   draws + `GXFlush`).
2. `fpcDw_Handler` in full: list reset + every process Draw → J3D
   calc/viewCalc/DL rebuild + `dScnPly_Draw` side-effects (§5).

That is ≈ a complete retail frame, hence the ~40 VI Hz cap.

## 3. XFB/EFB mechanics (GZLE01 = double buffer, async draw-done)

- `JFWDisplay::createManager(heap, JUTXfb::Double, true)` and
  `setDrawDoneMethod(JFWDisplay::Async)` (`src/m_Do/m_Do_graphic.cpp:79-86`).
  `getBufferNum()==2` → live paths are `exchangeXfb_double` and the
  double/triple branch of `JUTVideo::preRetraceProc`.
  `drawendXfb_single`, `copyXfb_triple`, `exchangeXfb_triple`, and both
  `clearEfb` sites in `beginRender` case 1 are **dead code for this game**.
- `JFWDisplay::beginRender` (0x802558CC, `JFWDisplay.cpp:247-289`):
  ProcBar ticks → `waitForTick(mTickRate, mFrameRate)` (0x80255D34 — the H10
  retime point) → `OSGetTick` bookkeeping into `field_0x30/0x34/0x38`
  (field_0x34 is the H1 accumulator input) → `exchangeXfb_double` → `preGX`
  (0x80255730: `GXInvalidateTexAll/VtxCache`/`GXSetPixelFmt`/dither — cheap
  register pokes).
- `exchangeXfb_double` (0x80255570, `JFWDisplay.cpp:139-168`), sole caller
  `bl 0x80255570` @ 0x80255A88 inside beginRender:
  - `drawn == displaying` (steady state): `prepareCopyDisp` (0x80255444 —
    `GXSetCopySrc/Clear/DispCopyGamma/ZMode`) then
    **`GXCopyDisp(drawingXfb, GX_TRUE)`** — copies EFB→XFB *and clears the EFB
    to the copy-clear color/Z* (`GXFrameBuf.c:519-569`; clear values set by
    `GXSetCopyClear` in prepareCopyDisp, `GXFrameBuf.c:408`). Then Async →
    `JUTVideo::drawDoneStart` (0x802C8054: `sDrawWaiting=true` +
    `GXSetDrawDone`). Then `drawn=drawing; drawing^=1`.
  - `drawn != displaying` (VI hasn't consumed the last drawn XFB — the
    GP-is-behind drop path): **`clearEfb(mClearColor)`** (0x80255F60 →
    0x80255FA0 — draws a fullscreen `GXSetZTexture(GX_ZT_REPLACE)` quad,
    `JFWDisplay.cpp:401-462`), no copy.
- `JUTVideo::preRetraceProc` (0x802C7E30, `JUTVideo.cpp:108-121`): for
  bufferNum 2/3, iff `!sDrawWaiting`: `displaying = drawn; VISetNextFrameBuffer(
  displayingXfb); VIFlush`. `drawDoneCallback` (0x802C8088) clears
  `sDrawWaiting` when the GP drains. **If no new draw is pending, every
  retrace re-issues the same XFB — a duplicate present is already free in
  retail.**
- `JFWDisplay::endRender` (0x80255AB8, `JFWDisplay.cpp:291-308`): `endGX`
  (0x802557C0 → `JUTFader::control` (advance+draw!), `JUTDbPrint::flush`,
  console, ProcBar, `GXFlush`) → bufferNum case 2: nothing → `cpuStart` →
  `calcCombinationRatio`.
- Single-buffer case 1 in beginRender (`JFWDisplay.cpp:268-276`) calls
  `clearEfb` unconditionally when `sDrawingFlag!=2` — the path the earlier
  analysis worried about. **Not live** (bufferNum==2). `preRetraceProc`'s
  single-buffer `GXCopyDisp` (`JUTVideo.cpp:122-137`) likewise dead.

## 4. Recommended design A — 4 gates, true present-only

On R-frames (`s_enabled && s_split_mode && !s_logic_this_frame`):

| # | address | site | action | why |
|---|---------|------|--------|-----|
| D1 | `0x8000AFB4` | inside `mDoGph_Painter`, first insn after the `bctrl`→`beginRender` (vtable+8 @0x8000AFB0) | `state->pc = 0x8000BBF0` | skips the entire draw body; resumes at the `lwz sManager`/`bctrl`→`endRender` (vtable+0xC @0x8000BC00) sequence → Painter still runs beginRender (pacing + tick bookkeeping + exchange + preGX) and endRender (endGX + ratio) then returns true |
| D2 | `0x800404CC` | `fpcDw_Handler` entry | `moderngekko_mod_return_u32(state,1)` (return value ignored anyway) | skips `dComIfGd_reset` (lists stay), the whole process Draw iterate (J3D calc/viewCalc, packet enqueue, `dScnPly_Draw` side-effects) and `AfterOfDraw` (GX restore, peekZ, endFrame/`GXDrawDone`) |
| D3 | `0x80255570` | `exchangeXfb_double` entry | `state->pc = state->lr` | no `GXCopyDisp` → no EFB→XFB copy, no EFB clear, no index rotation, no `drawDoneStart`. EFB retains the last rendered frame; VI keeps scanning the displayed XFB → free dup |
| D4 | `0x802557C0` | `endGX` entry | `state->pc = state->lr` | skips `JUTFader::control` (would otherwise advance mTimer at loop rate AND re-blend the quad into the retained EFB), DbPrint/console/ProcBar draws, `GXFlush` |

Net R-iteration work: main01 overhead + Painter's beginRender head
(`waitForTick` pacing — still retimed by H10 to `s_render_hz` — +
`field_0x34` bookkeeping feeding the H1 accumulator + preGX reg pokes) +
endRender tail (`cpuStart` + `calcCombinationRatio`). Everything else is
already gated. This is the "nearly free" present-only frame.

### Why it is correct

- **Duplicate present:** with D3, `drawn`/`displaying`/`drawing` indices and
  `sDrawWaiting` are untouched on R-frames. `preRetraceProc` keeps re-issuing
  `VISetNextFrameBuffer(same XFB)` every retrace. No GPU work at all.
- **EFB correctness:** on the next L-frame, `exchangeXfb_double` runs its
  steady-state branch and `GXCopyDisp` ships the *retained* EFB (last full
  frame + overlays — identical to what's on screen) to the other XFB, then
  clears it. Painter repaints over a cleared EFB exactly like retail. If VI
  lags and the else-branch runs instead, `clearEfb` wipes the EFB — also fine,
  Painter repaints. Either way no stale/torn/blank frame can be presented.
- **Packet lists:** `dComIfGd_reset` only runs inside the gated fpcDw window
  → lists are not cleared on R-frames and are consumed *exactly once* by the
  next L-frame Painter (the normal N→N+1 pipeline, just stretched). Packet
  objects live in actor/model/struct storage, and process deletes are gated
  (H4) — plus retail order already consumes packets before deletes — so no
  dangling pointers.
- **J3D animation:** `calc`/`update`/`viewCalc`/DL diffs run only inside Draw
  funcs → gated → the retained packet DLs reproduce the identical image. This
  is precisely the desired freeze; the B1–B4 interp hooks will need adjusting
  since `viewCalc` no longer runs on R-frames.
- **Fences/sync:** `GXSetDrawDone` (drawDoneStart) and `GXDrawDone`
  (endFrame) only run on L-frames; on R-frames nothing is submitted and
  nothing waits — no half-flushed state (the few `preGX` register writes just
  sit in the FIFO until the next `GXFlush`, harmless).
- **Frame-2 cadence side-effects** (`dScnPly_Draw`'s vibration, particle calc,
  collision Move, stage-change checks, dSnap, GBA map send, `cCt_execCounter`)
  freeze at the 30 Hz cadence — consistent with the logic gates, not a bug.

### D1 caveat (mechanism, not correctness)

`0x8000AFB4` is a **mid-function** hook. Verified viable: entry hooks are
mutating pre-hooks (`ModManager::Dispatch`, `src/runtime/mod_loader.cpp:586-588`),
hooks key on exact 4-aligned addresses, every guest instruction inside a
recompiled function is a `case` in its entry `switch` (DolRecomp
`backend/emitter.c` `emit_function`), and the run loop checks
`IsHostCallAddress(pc)` on every chassis re-entry — the `bctrl` return to
`0x8000AFB4` is such a re-entry. Side effect: `ChunkContainsHostCall` demotes
the *whole Painter chunk* to the fallback JIT
(`StaticRecompCore_SMC.cpp:563-588`, comment at 572-574) — Painter then runs
under the Dolphin JIT on every frame including L-frames. That is the one perf
cost of Design A; it is bounded (one ~3.3 KB function) and still leaves the
R-frame at ~zero GPU work. If a build lacks the fallback JIT or the demotion
is undesirable, use Design B.

## 5. Design B — fallback without mid-function hooks

Keep D2/D3/D4, drop D1, and instead gate every submission chokepoint Painter
calls (all plain `bl` targets, all `void` → `pc=lr`; verified by full-DOL
call-site scan — these are the *only* draw submitters inside Painter, the
rest of its `bl`s are cheap CPU/state pokes):

| address | function | Painter call sites |
|---------|----------|--------------------|
| 0x802ECCE4 | `J3DDrawBuffer::draw` | many (all `draw*List` helpers funnel here) |
| 0x802ECDB0 | `J3DDrawBuffer::drawTail` | 0x8000B3A0, 0x8000B6E4 (OpaListP0/XluListP1 paths) |
| 0x80086570 | `dDlst_list_c::draw` | 0x8000B098 (copy2D), 0x8000BB80/0x8000BB98/0x8000BBB0 (2DOpa/2DOpaTop/2DXlu) |
| 0x8007D16C | `dPa_control_c::draw` | ~11 sites — all `dComIfGp_particle_draw*` wrappers funnel here |
| 0x80084DEC | `dDlst_shadowControl_c::imageDraw` | 0x8000B0DC |
| 0x80084EF0 | `dDlst_shadowControl_c::draw` | 0x8000B308 |
| 0x80082F9C | `dDlst_alphaModel_c::draw` | alpha/spot/light model sites (returns BOOL, discarded via void wrappers — `pc=lr` is fine; `return_u32(1)` also safe) |
| 0x80008880 | `drawAlphaBuffer` | 0x8000B334/0x8000B37C |
| 0x80008600 | `clearAlphaBuffer` | 0x8000B358/0x8000B40C/0x8000B6CC |
| 0x80008B0C | `drawSpot` | 0x8000B5B4 |
| 0x80009914 | `motionBlure` | 0x8000B600 |
| 0x80008F34 | `drawDepth` (`GXCopyTex`) | 0x8000B628 |
| 0x80007FE8 | `calcFade` | 0x8000B748 |
| 0x800866F0 | `calcWipe` | 0x8000B9E8 (gate to keep wipe scroll at 30 Hz; the existing F3 snapshot hook becomes redundant) |
| 0x8000A8B8 | `mDoGph_screenCaptureDraw` | 0x8000B990 |

Design B still runs Painter's body bookkeeping (window loop, JPADrawInfo,
viewport/scissor/projection pokes, `j3dSys.drawInit`/`reinitGX`, `dMenu_flag`,
`dKy_setLight`, capture state machine) — negligible CPU, zero submitted
primitives. Painter stays compiled (no mid-function hook), but each gated
callee's chunk demotes (only entered on L-frames → fine).

## 6. What stops ticking on R-frames (all consistent with 30 Hz semantics)

- All process Draw methods (`fpcDw_Execute` iterate): `J3DModel::calc`
  (0x802EE8C0)/`update`/`entry`/`viewCalc` (0x802EEE30), `mDoExt_modelUpdate*`,
  DL diffs, every `dComIfGd_*` enqueue.
- `dScnPly_Draw` non-draw work (`src/d/d_s_play.cpp:285-393`): Ccsp Move/
  ClrMoveFlag, next-stage/wipe selection + `mDoAud_setSceneName`, vibration
  Run/Pause, `dMat_control_c::icePlay`, magma/grass/tree/wood/flower executes,
  `dKyr_poison_light_colision`, `dSnap_Execute`, `particle_calc3D/2D/menu`
  (0x8007D094/0x8007D0DC/0x8007D124), `cCt_execCounter`,
  `roomControl_checkDrawArea`, msg HIO proc, `map_mapBufferSendAGB` (GBA),
  plus the direct draw tail (`drawModelParticle`, attention, Ccsp Draw).
- `mDoGph_AfterOfDraw`: ProcBar/DbPrint visibility toggles, GX state restore,
  `peekZdata` (kantera-style `GXPeekZ` results simply hold their last value),
  `endFrame` (`GXDrawDone` fence + `GXFlush` + costFrame).
- Under D1 additionally: `mDoGph_gInf_c::free` heap ping-pong (halved cadence —
  alloc side is equally gated, consistent), `j3dSys.drawInit`, 2D graf setup,
  copy2D list, capture state machine, `calcWipe`/`calcFade`/particles/post-FX.
- Under D4: `JUTFader::control` (mTimer/mStatus advance + quad draw),
  DbPrint/console/ProcBar draws, `JUTAssertion::flushMessage_dbPrint`,
  `GXFlush`.

## 7. Risks / edge cases

- **DVD-error path** (`drawDvdCondition`, `f_pc_manager.cpp:186-237`) calls
  `beginRender`/`endRender` directly when a DVD error is active: on R-frames
  D3/D4 mean its screen isn't presented and its J2D writes land in the
  retained EFB → one mixed frame possible on the next L-frame. Cosmetic,
  unreachable in normal operation.
- **Single-buffer configs:** dead for GZLE01 (Double is hardcoded). If ever
  used, beginRender case 1 calls `clearEfb` unconditionally and would wipe the
  retained EFB — would need an extra gate; check `JUTXfb::sManager`
  (0x803F78F0) `getBufferNum()==2` defensively if reused elsewhere.
- **ProcBar imbalance:** `cpuStart` runs (endRender tail) while
  `gpWaitStart/gpEnd/cpuEnd` live in endFrame (gated) → debug meter numbers
  wrong on R-frames; invisible in retail (procVisible forced false).
- **Screen capture / photobox:** `mCaptureStep` machine pauses on R-frames,
  completes on L-frames — just slower.
- **Fades/wipes/monotone:** freeze at 30 Hz cadence (desired); existing F1–F3
  hooks become no-ops under Design A but remain harmless.
- **Asserts:** `fpcDw_Handler`'s return is unasserted (disasm @0x8003ED60 has
  no post-call check) — `pc=lr` or `return_u32(1)` both safe; prefer
  `return_u32(1)` for parity with H5/H6.
- **Menu/logo/movie scenes:** same gates apply whenever `s_split_mode` is on;
  the adaptive rule (mTickRate==1350000) already limits split to the 30 fps
  play-scene tick. Other scenes' draws go through the same fpcDw/dDlst path —
  consistent either way.

## 8. Suggested `mod.c` additions (orchestrator — do NOT merge blindly)

```c
/* Present-only gates. On R-frames: skip Painter's draw body, the whole
 * draw-iterate, the XFB exchange copy and the overlay tail. VI keeps
 * scanning the displayed XFB (preRetraceProc re-issues it while
 * !sDrawWaiting), so a duplicate present costs ~nothing. */

#define MDGPH_POST_BEGINRENDER 0x8000AFB4u  /* mid mDoGph_Painter: insn after bctrl→beginRender */
#define MDGPH_ENDRENDER_SEQ    0x8000BBF0u  /* resume: lwz sManager; bctrl→endRender */

static void on_painter_body_gate(CPUState* state)
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = MDGPH_ENDRENDER_SEQ;      /* skip draw body, still run endRender */
}

static void on_draw_handler_gate(CPUState* state)                 /* fpcDw_Handler 0x800404CC */
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        moderngekko_mod_return_u32(state, 1u);
}

static void on_exchange_gate(CPUState* state)                     /* exchangeXfb_double 0x80255570 */
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

static void on_endgx_gate(CPUState* state)                        /* endGX 0x802557C0 */
{
    if (s_enabled && s_split_mode && !s_logic_this_frame)
        state->pc = state->lr;
}

/* hooks[] additions: */
    RECOMP_HOOK(0x8000AFB4u, on_painter_body_gate),   /* D1 — demotes Painter chunk to fallback JIT */
    RECOMP_HOOK(0x800404CCu, on_draw_handler_gate),   /* D2 */
    RECOMP_HOOK(0x80255570u, on_exchange_gate),       /* D3 */
    RECOMP_HOOK(0x802557C0u, on_endgx_gate),          /* D4 */
```

Notes for the integrator:
- `hooks_active`/`num_hooks` sizing must grow (currently fixed 14) and the
  `s_enabled` copy loop at the bottom of `moderngekko_get_mod` must include
  these.
- Keep H1 (0x802558CC) and H10 (0x80255D34) exactly as-is — D1 depends on
  beginRender still running (accumulator input `field_0x34` and the retimed
  `waitForTick` pacing).
- If Design B is preferred, drop the D1 hook and add the §5 chokepoints
  instead; D2/D3/D4 are shared.

## 9. Source index

- `src/f_pc/f_pc_manager.cpp:266-295` — fpcM_Management order
- `src/f_pc/f_pc_draw.cpp:12-40` — fpcDw_Execute / fpcDw_Handler (BOOL)
- `src/f_pc/f_pc_layer_iter.cpp:11-16` — fpcLyIt_OnlyHere
- `src/m_Do/m_Do_graphic.cpp:1584-1944` — mDoGph_Painter (beginRender 1590,
  endRender 1941); `:263-320` BeforeOfDraw/AfterOfDraw; `:79-86` Double+Async
- `src/d/d_s_play.cpp:285-393` — dScnPly_Draw (mixed calc+draw)
- `src/d/d_drawlist.cpp:1959-1988` — dDlst reset → J3DDrawBuffer::frameInit
- `src/JSystem/J3DGraphBase/J3DDrawBuffer.cpp:214-241` — draw/drawHead/drawTail
- `src/JSystem/JFramework/JFWDisplay.cpp:127-168,200-245,247-308,346-368,387-462`
  — drawendXfb_single / exchangeXfb_double / preGX / endGX / beginRender /
  endRender / waitForTick / clearEfb(+init)
- `src/JSystem/JUtility/JUTVideo.cpp:63-170` — preRetraceProc (double/triple
  re-present while !sDrawWaiting), drawDoneStart/Callback
- `src/dolphin/gx/GXFrameBuf.c:408-430,519-569` — GXSetCopyClear / GXCopyDisp
  (doClear clears EFB post-copy)

---

## 10. As-implemented design (supersedes §4's D1 mechanism)

### D1 = Painter-entry tail-call (works, verified)

`on_painter_skip` is hooked at **`0x8000AF2C`** (Painter *entry* — a safe
function-entry site, not a call-site). On every frame-loop iteration it:

1. Runs `frame60_accum_decide` — reads the JFW singleton, `mTickRate`
   (split detect) and `field_0x34` (per-iteration tick delta), accumulates
   `s_acc`, and sets `s_logic_this_frame` (~30 Hz logic cadence).
2. On R-frames, sets `state->gpr[3] = s_jfw_display` and
   `state->pc = 0x802558CC` (JFWDisplay::beginRender). `lr` still holds
   Painter's return address (`cAPIGph_Painter`'s continuation), so beginRender
   returns straight to `fpcM_Management` — Painter's entire draw body AND
   `endRender` are skipped.

On L-frames there is no redirect — Painter runs fully (setup + all draws +
endRender). `cAPIGph_Painter` discards Painter's bool return, so beginRender's
void return is harmless.

### Why the accumulator lives in on_painter_skip, not beginRender

`beginRender` reached via the redirect goes through the **chassis
passthrough → fallback-JIT** path: `Dispatch` returns false for hook-only
addresses, so the JIT runs beginRender's block directly **without re-dispatching
its entry hook**. An accumulator keyed on beginRender's hook never ticks on
R-frames → `s_logic_this_frame` froze at 0 → the game logic stalled (the
earlier "starvation" was frozen logic, not a handshake deadlock). Painter
entry *is* reached via normal inter-chunk dispatch every iteration, so it is
the only correct owner. `0x802558CC` is deliberately **not** hooked — hooking
it too would double-accumulate on L-frames.

### Gates retained on R-frames

- `exchangeXfb_double` (D3, `pc=lr`): no `GXCopyDisp`, no index rotation, no
  `drawDoneStart` → VI keeps re-issuing the displayed XFB (free dup-present).
- `endGX` (D4, `pc=lr`): skips the fader/console/`GXFlush` tail.
- `fpcDw_Handler` (D2) + leaf draw funnels D5–D11 + the F1–F4 fade/wipe gates:
  defense-in-depth — they also cover any Painter call that reaches the body
  without hitting the entry hook.

### Verified

- 46/46 tests pass; `frame60_accum_test` drives the accumulator via
  `on_painter_skip`.
- Soak: `[f60] L=1969 R=8783 (18.3% L)` — ~30 Hz logic at ~150 iter/s through
  the `sea_T` scene load (progressed past Room44→Room0) with no crash/stall.
- Capped `--capped` path unharmed.

### Not yet empirically verified

- Duplicate-present *visuals* on a real display (headless can't confirm).
- Sustained ~200 Hz — only reachable in light scenes; heavy attract scenes are
  host-CPU-bound (~0.5× realtime), the honest hardware limit.
- That the skipped `endRender`/virtual UI call has no subtle side-effect
  across all menus/cutscenes/gameplay states.

### Source index (implementation details)

- `include/JSystem/JUtility/JUTXfb.h:9-15` — EXfbNumber (Double=2 used)
- Runtime hook semantics: `src/runtime/mod_loader.cpp:560-601`,
  `vendor/dolphin/.../StaticRecompCore_SMC.cpp:563-640`,
  `vendor/dolphin/DolRecomp/src/backend/emitter.c` (per-instruction switch +
  `return_dispatch`), `StaticRecompCore_Run.cpp:400-450` (host-call fast path)

---

## 11. Presentation pacing and interpolation weight (render mode)

Measured with `MODERNGEKKO_F60_VILOG=1`, which logs each XFB copy (`[vcp]`),
each `clearEfb` drop (`[vce]`) and each `JUTVideo::preRetraceProc` pick
(`[vir]`). The copy log includes the view matrix of the image being copied,
so offline analysis can reconstruct exactly which image every retrace
scanned out.

### VI phase lock (`MODERNGEKKO_F60_VI_LOCK`, default on)

Tick-exact pacing runs the L+R pair on a 1,350,000-tick period. NTSC VI
retraces every ~675,675 ticks, so the copy phase drifts ~1,351 ticks per
pair against the retrace grid. When the L-slot is work-bound past one field
(sailing: ~760K ticks), the L-image is the newest XFB for less than one
field. While the drift parks that window between two retraces, no retrace
scans it out, and `exchangeXfb_double`'s else-branch discards the R-image
too. Result: 6-10 s bursts of every-other-pair drops about every 40 s
(playsea: 93.5% of fields showed a new image).

`on_wait_for_tick` now picks the pad so that the NEXT wait exits `field/8`
after a retrace. It reads `nextTick$2569` (0x803F73A0),
`JUTVideo::sVideoLastTick` (0x803F78DC) and `sVideoInterval` (0x803F78E0).
Every image then spans exactly one retrace whenever L+R work fits in two
fields (playsea: 100.0% new images, 0 drops, copy phase pinned at 0.13-0.16
field). Logic cadence is unchanged: `decide()` still accumulates real tick
deltas, so logic stays at 30.000 Hz. The 0.1% VI/logic mismatch shows up as
one L,L pair every ~16.6 s, the same slip retail shows as one 3-field frame
every ~16.7 s.

### Display-time alpha (`MODERNGEKKO_F60_DISPLAY_ALPHA`, default on)

L-frames draw their pose exactly, so an R-frame must sit at the fraction of
display time between its neighbouring L-frames. `s_acc / TICK_30FPS` is the
logic schedule's residual phase instead, and it parks anywhere in
[0.5, 1). Under the VI lock the pattern is strictly L,R,L,R (slips are L,L,
never R,R), so an R right after an L is exactly 0.5.

Measurement: project a fixed world point through each displayed field's view
and compare per-field screen steps. In the attract flyover's interpolated
regime:

| | alpha fix off | alpha fix on |
|---|---|---|
| frozen fields | 29.5% | 0.0% |
| uneven fields (>50% off the local median) | 52.3% | 0.2% |

Other render rates, or unlocked pacing, keep the residual alpha.

### Known 30 Hz presentation (by design)

`windowNum == 0` scenes (title/`dScnTitle`, logos, storybook/opening) draw
inline inside fpcDw, so they have nothing an R-frame can repaint. They stay
on the duplicate-present path at 30 unique images/s.

## 12. Headroom governor (`MODERNGEKKO_F60_AUTO_DEGRADE`, default on)

An interpolated R-frame re-runs Painter and costs real host time: in the
heaviest scene measured, about 7 ms on top of about 27 ms of L-frame work per
33.3 ms pair. A host that cannot fit both never reaches the throttle's sleep,
so guest time falls behind wall time. The whole game (logic, audio, VI) then
runs in slow motion at 50-56 fps instead of full speed.

The governor (`gov_on_lframe`) measures guest seconds (timebase / 40.5 MHz)
per host wall second for every L->L pair and judges the median pair of each
60-pair window (about 2 s). R-frame cost is uniform: every pair runs long.
Shader compiles and disc reads are spikes, a few long pairs followed by a
throttle catch-up above 1.0x. Dropping R-frames cures only the first, and
the median ignores the second. (The first live run averaged instead and
tripped on a cold shader cache.)

- It degrades after two consecutive windows whose median is below 0.95x. R-frames then take
  the duplicate-present path, the same one the overlap guard uses, and
  logic cadence is untouched. The game shows 30 fps at full speed.
- It retries interpolation after a hold of 15 s. Each failed retry doubles
  the hold, up to 4 min. After about 60 s at full speed the hold goes back
  to 15 s.
- It does not retry while even the duplicate cadence is below 0.97x, since
  interpolating can only be slower.
- It drops any L->L gap longer than 150 ms (pause, window drag, disc or
  shader stall) as a sample. It also ignores fast-forward, where speed
  is above 1.

Transitions are logged to stderr as `[f60] headroom: ...`. Unit coverage is
in `tests/frame60_accum_test.c` (checks 410-425).
