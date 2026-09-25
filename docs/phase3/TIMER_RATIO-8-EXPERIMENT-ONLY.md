# TIMER_RATIO=8 — experiment-only / never-ship

Per arbitration Q3 D11 §1.2 / timer-ratio-spec.md and guest-clock-budget-spec.md:

- TIMER_RATIO (SystemTimers.h:41, default 12) is a guest-time ↔ CPU-cycle
  calibration constant, NOT a throughput lever and NOT a CPU-speed decoupler.
  It appears in exactly 10 use sites across 4 files; VI retrace (59.94 Hz),
  DSP/AI-DMA, CoreTiming and Throttle are independent of it.

- At 254M cycles/s, ratio 8 lifts the 30fps gate from 15.7 -> 23.5 logic FPS
  at 0.78x game speed (slow-motion). It fast-forwards above 324M; with B/A
  fixes (340-400M) it becomes 1.05-1.23x — wrong. A ratio is only correct at
  one throughput (ratio ≈ 12 * 486M / T_sustained; ratio 6 ≈ 1.0x at 254M).

- Build/runtime knob MODERNGEKKO_TIMER_RATIO / --timer-ratio is
  EXPERIMENT-ONLY. Run ONCE at Phase-3 entry (V8+V6, ~2 rebuilds + 6 headless
  runs, serialized with BootChainDrive), produce the FPS/game-speed/audio-drift
  table as coupling-model validation, then discard. Never a release feature,
  never a release knob, never gated by logic in shipping code.

- VI-adjust does NOT re-lock it (VI and TIMER_RATIO are independent clocks).

- This note is part of the Option B accumulator patch: the accumulator mod
  (mods/frame60-accum/mod.c) does NOT use TIMER_RATIO and explicitly refuses
  to gate on it (see on_load guard on MODERNGEKKO_TIMER_RATIO=8).
