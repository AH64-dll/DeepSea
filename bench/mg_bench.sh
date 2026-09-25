#!/usr/bin/env bash
# mg_bench.sh — reproducible ModernGekko benchmark run.
#
# Replicates the windows-optimization Invoke-WindWakerBenchmark semantics:
# isolated copied user-dir, [vi] readiness marker, warmup window, measured
# window, graceful SIGTERM stop, evidence files under $MG_BENCH_OUT/<name>-<ts>/.
#
# Usage:
#   mg_bench.sh --name NAME --state FILE [options]
#
# Options:
#   --runner PATH      moderngekko-run binary   (default: <repo>/build/moderngekko-run)
#   --module PATH      recomp module .so        (default: $MG_MODULE_INLINE)
#   --duration SEC     measured window          (default: 60)
#   --warmup SEC       warmup after first [vi]  (default: 15)
#   --startup SEC      max wait for first [vi]  (default: 240)
#   --uncapped         pass --uncapped          (default: none)
#   --capped           pass --capped
#   --max-fps N        pass --max-fps N
#   --no-mods          pass --no-mods           (default: mods auto-load)
#   --perf             MODERNGEKKO_PERF_COUNTERS=1
#   --sampler          LD_PRELOAD mg_sampler.so (MG_SAMPLE_HZ to tune)
#   --env "K=V ..."    extra environment (repeatable)
#   --userdir PATH     user-dir template        (default: $MG_USERDIR_TEMPLATE)
#   --outdir PATH      override output root     (default: $MG_BENCH_OUT)
#   --expect-route ID require a matching [scene]/[checkpoint] route record
#   --expect-log REGEX optional additional regex on that same route record
#
# Outputs: runner.log vi.csv thr.csv f60.csv perf.log threads.csv maps.txt
#          samples.txt (if --sampler) metadata.env summary.txt exit.state
#
# Determinism note: the demo is retrace-driven; under CPU-bound slowdown the
# scene advances slower in wall time. Compare arms with equal wall windows
# and prefer interleaved A/B runs plus >=3 repetitions per arm.
set -u

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$BENCH_DIR/env.sh" ] && . "$BENCH_DIR/env.sh"

NAME=""; STATE=""; RUNNER=""; MODULE="${MG_MODULE_INLINE:-}"
DURATION=60; WARMUP=15; STARTUP=240
RATE_ARGS=(); MODS=1; PERF=0; SAMPLER=0
EXTRA_ENV=(); USERDIR="${MG_USERDIR_TEMPLATE:-}"; OUTROOT="${MG_BENCH_OUT:-/tmp/mg-bench-out}"
EXPECT_LOG=""; EXPECT_ROUTE=""; SCENE_VERIFIED=0

while [ $# -gt 0 ]; do
  case "$1" in
    --name) NAME="$2"; shift 2;;
    --state) STATE="$2"; shift 2;;
    --runner) RUNNER="$2"; shift 2;;
    --module) MODULE="$2"; shift 2;;
    --duration) DURATION="$2"; shift 2;;
    --warmup) WARMUP="$2"; shift 2;;
    --startup) STARTUP="$2"; shift 2;;
    --uncapped) RATE_ARGS+=(--uncapped); shift;;
    --capped) RATE_ARGS+=(--capped); shift;;
    --max-fps) RATE_ARGS+=(--max-fps "$2"); shift 2;;
    --no-mods) MODS=0; shift;;
    --perf) PERF=1; shift;;
    --sampler) SAMPLER=1; shift;;
    --env) EXTRA_ENV+=("$2"); shift 2;;
    --userdir) USERDIR="$2"; shift 2;;
    --outdir) OUTROOT="$2"; shift 2;;
    --expect-log) EXPECT_LOG="$2"; shift 2;;
    --expect-route) EXPECT_ROUTE="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

[ -n "$NAME" ] || { echo "--name required" >&2; exit 2; }
[ -n "$STATE" ] || { echo "--state required" >&2; exit 2; }
[ -f "$STATE" ] || { echo "state not found: $STATE" >&2; exit 2; }
[ -n "$MODULE" ] || { echo "--module required (or MG_MODULE_INLINE)" >&2; exit 2; }
[ -f "$MODULE" ] || { echo "module not found: $MODULE" >&2; exit 2; }
if [ -z "$RUNNER" ]; then
  RUNNER="$(cd "$BENCH_DIR/.." && pwd)/build/moderngekko-run"
fi
[ -x "$RUNNER" ] || { echo "runner not found/executable: $RUNNER" >&2; exit 2; }

for v in "$DURATION" "$WARMUP" "$STARTUP"; do
  case "$v" in ''|*[!0-9]*) echo "--duration/--warmup/--startup must be non-negative integers" >&2; exit 2;; esac
done

TS="$(date +%Y%m%d-%H%M%S)"
OUT="$OUTROOT/$NAME-$TS"
mkdir -p "$OUT/userdir"
# The script later cd's to the runner directory; keep $OUT absolute so a
# relative --outdir does not silently relocate every artifact.
OUT="$(cd "$OUT" && pwd)"

# Benchmarks are timing-sensitive: serialize against other bench runs on this
# machine (agents share the host). Waiting here is mandatory, not optional.
BENCH_LOCK="${MG_BENCH_LOCK:-/tmp/mg-bench.lock}"
exec 9>"$BENCH_LOCK"
echo "[mg_bench] waiting for bench lock $BENCH_LOCK ..."
flock -w 7200 9 || { echo "bench lock timeout" >&2; exit 3; }
echo "[mg_bench] lock acquired"
if [ -n "$USERDIR" ] && [ -d "$USERDIR" ]; then
  cp -a "$USERDIR/." "$OUT/userdir/" 2>/dev/null || true
fi

LOG="$OUT/runner.log"
: > "$LOG"

ENV_ARGS=()
if [ "$SAMPLER" = 1 ]; then
  # LD_PRELOAD cannot contain spaces; stage the sampler under a
  # space-free cache dir keyed by source mtime.
  SCACHE="${XDG_CACHE_HOME:-$HOME/.cache}/mg-bench"
  mkdir -p "$SCACHE"
  SLIB="$SCACHE/mg_sampler.so"
  if [ ! -f "$SLIB" ] || [ "$BENCH_DIR/mg_sampler.c" -nt "$SLIB" ]; then
    cc -O2 -fPIC -shared -o "$SLIB" "$BENCH_DIR/mg_sampler.c" || exit 1
  fi
  ENV_ARGS+=("LD_PRELOAD=$SLIB" "MG_SAMPLE_OUT=$OUT/samples.txt")
fi
if [ "$PERF" = 1 ]; then
  ENV_ARGS+=("MODERNGEKKO_PERF_COUNTERS=1")
fi
for kv in ${EXTRA_ENV[@]+"${EXTRA_ENV[@]}"}; do [ -n "$kv" ] && ENV_ARGS+=("$kv"); done
# G1 tick is the guest-side progress signal; VI cadence alone can continue
# while the guest is parked. Keep it enabled even if an extra env overrides it.
ENV_ARGS+=("MODERNGEKKO_G1TICK=1")

MODS_ARGS=()
[ "$MODS" = 0 ] && MODS_ARGS+=(--no-mods)

scene_verification_line() {
  if [ "$SCENE_VERIFIED" = 1 ]; then echo "scene_verification=VERIFIED route=$EXPECT_ROUTE"; else echo "scene_verification=UNVERIFIED (${SCENE_FAILURE_REASON:-route record not verified})"; fi
}
guest_progress_note() {
  echo "guest_progress_signal=G1 timebase/logic_frames advanced; this does not prove 60 simulation updates"
}

cd "$(dirname "$RUNNER")"
env ${ENV_ARGS[@]+"${ENV_ARGS[@]}"} \
  "$RUNNER" \
    --game "$MG_GAME_ROOT" \
    --module "$MODULE" \
    --user-dir "$OUT/userdir" \
    --load-state "$STATE" \
    --save-state-on-exit "$OUT/exit.state" \
    --headless ${MODS_ARGS[@]+"${MODS_ARGS[@]}"} ${RATE_ARGS[@]+"${RATE_ARGS[@]}"} \
    >>"$LOG" 2>&1 &
PID=$!
LAUNCH_MS=$(date +%s%3N)
echo "$PID" > "$OUT/pid"
echo "$LAUNCH_MS" > "$OUT/launch_ms"

# Per-thread CPU monitor: epoch_ms,tid,comm,utime+stime(jiffies)
(
  CLK=$(getconf CLK_TCK); echo "$CLK" > "$OUT/clk_tck"
  while kill -0 "$PID" 2>/dev/null; do
    now=$(date +%s%3N)
    for t in /proc/"$PID"/task/*/stat; do
      tid=${t%/stat}; tid=${tid##*/task/}
      read -r line < "$t" 2>/dev/null || continue
      comm="${line#*(}"; comm="${comm%%)*}"
      rest="${line##*) }"
      # $rest starts at stat field 3 (state): $12=utime(14), $13=stime(15).
      # (The previous ${13}+${14} summed stime+cutime and dropped utime.)
      set -- $rest
      echo "$now,$tid,$comm,$(( ${12:-0} + ${13:-0} ))" >> "$OUT/threads.csv"
    done
    sleep 1
  done
) &
MONPID=$!

# Heartbeat/watchdog: every MG_HEARTBEAT_S (default 600) seconds verify the
# runner is (a) alive and (b) making progress (log bytes or [vi] count grew).
# Two consecutive stalled beats -> SIGTERM; still alive after 15 s -> SIGKILL.
HB_S="${MG_HEARTBEAT_S:-600}"
last_bytes=0; last_vi=0; stall_beats=0
heartbeat() { # returns 0 if runner should keep going, 1 if we killed it
  kill -0 "$PID" 2>/dev/null || return 1
  local bytes vi
  bytes=$(stat -c %s "$LOG" 2>/dev/null); bytes=${bytes:-0}
  # grep -c already prints "0" (exit 1) on no match; appending "|| echo 0"
  # yielded "0\n0" and broke the -le test, permanently disarming the watchdog.
  vi=$(grep -c '^\[vi\]' "$LOG" 2>/dev/null); vi=${vi:-0}
  echo "[hb] $(date +%H:%M:%S) pid_alive=1 log_bytes=$bytes vi=$vi" | tee -a "$LOG"
  if [ "$bytes" -le "$last_bytes" ] && [ "$vi" -le "$last_vi" ]; then
    stall_beats=$((stall_beats + 1))
  else
    stall_beats=0
  fi
  last_bytes=$bytes; last_vi=$vi
  if [ "$stall_beats" -ge 2 ]; then
    echo "[hb] STALLED: no log progress for $((2 * HB_S))s; sending SIGTERM" | tee -a "$LOG"
    kill -TERM "$PID" 2>/dev/null
    sleep 15
    if kill -0 "$PID" 2>/dev/null; then
      echo "[hb] SIGTERM ignored; sending SIGKILL" | tee -a "$LOG"
      kill -KILL "$PID" 2>/dev/null
    fi
    return 1
  fi
  return 0
}

# Wait for first [vi] record (readiness marker), up to --startup seconds.
deadline=$(( $(date +%s) + STARTUP ))
last_hb=$(date +%s)
vi_seen=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  if grep -q '^\[vi\]' "$LOG" 2>/dev/null; then vi_seen=1; break; fi
  kill -0 "$PID" 2>/dev/null || break
  now=$(date +%s)
  if [ $((now - last_hb)) -ge "$HB_S" ]; then last_hb=$now; heartbeat || break; fi
  sleep 1
done

# Common gate helper used after readiness, during warmup, and through the
# measured window.  The runner can stay alive while emulation has crashed.
runner_alive() {
  kill -0 "$PID" 2>/dev/null || return 1
  # kill -0 succeeds for zombies on Linux; they have exited and cannot make
  # progress. Git Bash lacks /proc, so use ps as its equivalent fallback.
  if [ -r "/proc/$PID/stat" ]; then
    local state
    state=$(sed 's/^.*) //' "/proc/$PID/stat" | awk '{print $1}')
    [ "$state" != Z ] && [ "$state" != X ]
  else
    local state
    state=$(ps -p "$PID" -o stat= 2>/dev/null | tr -d ' ')
    [ -n "$state" ] && [[ "$state" != Z* && "$state" != X* ]]
  fi
}
fatal_guest_marker() {
  grep -Eiq '\[(stall|panic|fatal|assert)\]|\bOSPanic\b|\bOSDefaultExceptionHandler\b|load FAILED|FATAL|PANIC|Assertion failed:' "$LOG"
}
vi_count() { grep -c '^\[vi\]' "$LOG" 2>/dev/null || true; }
g1_count() { grep -c '^\[g1tick\]' "$LOG" 2>/dev/null || true; }
g1_latest() { grep '^\[g1tick\]' "$LOG" | tail -1; }
g1_value() {
  local line="$1" key="$2"
  echo "$line" | sed -n "s/.*${key}=\\([0-9][0-9]*\\).*/\\1/p"
}
g1_progress_since() {
  local before_count="$1" before_tb="$2" lines latest tb progress=0
  lines=$(grep '^\[g1tick\]' "$LOG" | tail -n +$((before_count + 1)))
  [ -n "$lines" ] || return 1
  latest=$(echo "$lines" | tail -1)
  tb=$(g1_value "$latest" tb)
  [ -n "$tb" ] && [ "$tb" -gt "$before_tb" ] || return 1
  echo "$lines" | awk '
    /tb_delta=[1-9][0-9]*/ && /logic_frames=[1-9][0-9]*/ { found=1 }
    END { exit !found }
  '
}
gate_failure=""
check_guest_gate() {
  if ! runner_alive; then gate_failure="runner exited/crashed"; return 1; fi
  if fatal_guest_marker; then gate_failure="fatal guest marker"; return 1; fi
  if grep -Eq '^\[state\] load FAILED' "$LOG"; then gate_failure="state load failed"; return 1; fi
  return 0
}

if [ "$vi_seen" != 1 ]; then
  echo "BENCH-FAIL: no [vi] within ${STARTUP}s (state/module mismatch?)" >> "$LOG"
  kill -TERM "$PID" 2>/dev/null; sleep 5; kill -KILL "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null; kill "$MONPID" 2>/dev/null
  echo "result=NO_VI" > "$OUT/summary.txt"; scene_verification_line >> "$OUT/summary.txt"; guest_progress_note >> "$OUT/summary.txt"; echo "outdir=$OUT"
  exit 1
fi

# [vi] means the guest is producing video interrupts, but not that the
# requested checkpoint restored. Dolphin emits this marker only at commit.
if ! grep -Fxq "[state] loaded '$STATE'" "$LOG"; then
  echo "BENCH-FAIL: no successful state-load marker for $STATE" >> "$LOG"
  gate_failure="state load success marker missing"
fi
if [ -n "$gate_failure" ] || ! check_guest_gate; then
  [ -n "$gate_failure" ] || gate_failure="guest gate failed"
  echo "BENCH-FAIL: $gate_failure" >> "$LOG"
  kill -TERM "$PID" 2>/dev/null; sleep 1; kill -KILL "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null; RC=$?; kill "$MONPID" 2>/dev/null
  echo "result=FAIL" > "$OUT/summary.txt"; echo "failure=$gate_failure" >> "$OUT/summary.txt"
  echo "exit_code=$RC" >> "$OUT/summary.txt"; scene_verification_line >> "$OUT/summary.txt"; guest_progress_note >> "$OUT/summary.txt"; echo "outdir=$OUT"
  exit 1
fi

warmup_remaining=$WARMUP
warmup_g1_count=$(g1_count)
warmup_g1_tb=$(g1_value "$(g1_latest)" tb)
warmup_g1_tb=${warmup_g1_tb:-0}
while [ "$warmup_remaining" -gt 0 ]; do
  check_guest_gate || break
  sleep 1; warmup_remaining=$((warmup_remaining-1))
done
if [ -z "$gate_failure" ]; then
  # At least one new VI must arrive after readiness; otherwise the guest
  # stopped making useful progress while the process remained alive.
  [ "$(vi_count)" -gt 1 ] || gate_failure="no guest VI progress during warmup"
fi
if [ -z "$gate_failure" ] && ! g1_progress_since "$warmup_g1_count" "$warmup_g1_tb"; then
  gate_failure="no guest timebase/logic progress during warmup"
fi
if [ -n "$EXPECT_ROUTE" ]; then
  scene_lines=$(grep -E '^\[(scene|checkpoint)\]' "$LOG" || true)
  if [ -n "$EXPECT_LOG" ]; then scene_lines=$(echo "$scene_lines" | grep -E "$EXPECT_LOG" || true); fi
  scene_lines=$(echo "$scene_lines" | grep -F -- "$EXPECT_ROUTE" || true)
  if [ -n "$scene_lines" ]; then
    SCENE_VERIFIED=1
  else
    SCENE_FAILURE_REASON="no [scene]/[checkpoint] line matched route '$EXPECT_ROUTE' and --expect-log"
  fi
else
  SCENE_FAILURE_REASON="missing --expect-route"
fi
if [ -n "$gate_failure" ] || ! check_guest_gate; then
  [ -n "$gate_failure" ] || gate_failure="guest gate failed"
  echo "BENCH-FAIL: $gate_failure" >> "$LOG"
  kill -TERM "$PID" 2>/dev/null; sleep 1; kill -KILL "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null; RC=$?; kill "$MONPID" 2>/dev/null
  echo "result=FAIL" > "$OUT/summary.txt"; echo "failure=$gate_failure" >> "$OUT/summary.txt"
  echo "exit_code=$RC" >> "$OUT/summary.txt"; scene_verification_line >> "$OUT/summary.txt"; guest_progress_note >> "$OUT/summary.txt"; echo "outdir=$OUT"
  exit 1
fi
MEASURE_START=$(date +%s%3N)
# Measured window with periodic heartbeat (sleeps in HB chunks so a stall
# during measurement is caught too).
remaining=$DURATION
measure_vi_start=$(vi_count)
measure_g1_count=$(g1_count)
measure_g1_tb=$(g1_value "$(g1_latest)" tb)
measure_g1_tb=${measure_g1_tb:-0}
last_hb=$(date +%s)
STALLED=0
while [ "$remaining" -gt 0 ]; do
  # Poll at most once per second so crashes and fatal guest markers are not
  # hidden inside a long measurement sleep.
  sleep 1; remaining=$((remaining - 1))
  check_guest_gate || { STALLED=1; break; }
  now=$(date +%s)
  if [ $((now - last_hb)) -ge "$HB_S" ]; then
    last_hb=$now
    heartbeat || { STALLED=1; break; }
  fi
done
if [ "$STALLED" = 0 ] && [ "$(vi_count)" -le "$measure_vi_start" ]; then
  gate_failure="no guest VI progress during measurement"
  STALLED=1
fi
if [ "$STALLED" = 0 ] && ! g1_progress_since "$measure_g1_count" "$measure_g1_tb"; then
  gate_failure="no guest timebase/logic progress during measurement"
  STALLED=1
fi
MEASURE_END=$(date +%s%3N)

cat /proc/"$PID"/maps > "$OUT/maps.txt" 2>/dev/null || true
kill -TERM "$PID" 2>/dev/null
for i in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
kill -KILL "$PID" 2>/dev/null
wait "$PID" 2>/dev/null; RC=$?
kill "$MONPID" 2>/dev/null; wait "$MONPID" 2>/dev/null

# ---- parse ----
grep '^\[vi\]' "$LOG" | sed 's/\[vi\] fields=\([0-9]*\) +\([0-9.]*\) Hz/\1 \2/' > "$OUT/vi.raw"
grep '^\[thr\]' "$LOG" | sed 's/.*calls=\([0-9]*\) slept_ms=\([0-9]*\).*/\1 \2/' > "$OUT/thr.csv"
grep '^\[f60\]' "$LOG" > "$OUT/f60.csv"
grep -E '^\[(vi-perf|perf-|fb|staticrecomp)' "$LOG" > "$OUT/perf.log"

# measured-window vi_hz: [vi] lines print at ~1 s wall cadence; take the last
# floor(duration) lines preceding stop. Approximation: last DURATION records.
tail -n "$DURATION" "$OUT/vi.raw" > "$OUT/vi.measured" || true
awk '{s+=$2; if($2<min||NR==1)min=$2; if($2>max)max=$2; n++}
     END{if(n)printf "mean_vi_hz=%.2f\nmin_vi_hz=%.2f\nmax_vi_hz=%.2f\nvi_samples=%d\n",s/n,min,max,n;
         else print "mean_vi_hz=NA"}' "$OUT/vi.measured" > "$OUT/vi.stats"

# realtime ratio from shutdown cycles / wall seconds
SHUT=$(grep 'staticrecomp\] shutdown' "$LOG" | tail -1)
echo "$SHUT" > "$OUT/shutdown.txt"
CYC=$(echo "$SHUT" | sed -n 's/.* cycles=\([0-9]*\).*/\1/p')
EXIT_MS=$(date +%s%3N)
WALL_MS=$(( MEASURE_END - MEASURE_START ))
TOTAL_WALL_MS=$(( EXIT_MS - LAUNCH_MS ))
{
  echo "name=$NAME"; echo "outdir=$OUT"; echo "runner=$RUNNER"; echo "module=$MODULE"
  echo "state=$STATE"; echo "duration=$DURATION"; echo "warmup=$WARMUP"
  echo "rate_args=${RATE_ARGS[*]:-none}"; echo "mods=$MODS"; echo "perf=$PERF"; echo "sampler=$SAMPLER"
  echo "extra_env=${EXTRA_ENV[*]:-none}"
  scene_verification_line
  guest_progress_note
  echo "exit_code=$RC"; echo "measure_ms=$WALL_MS"; echo "total_wall_ms=$TOTAL_WALL_MS"
  echo "measure_start_ms=$MEASURE_START"; echo "measure_end_ms=$MEASURE_END"; echo "launch_ms=$LAUNCH_MS"
  [ -n "$CYC" ] && echo "guest_cycles=$CYC"
  sha256sum "$RUNNER" "$MODULE" "$STATE" 2>/dev/null | awk '{print "sha256="$1"  file="$2}'
} > "$OUT/metadata.env"

cat "$OUT/vi.stats" >> "$OUT/summary.txt" 2>/dev/null || true
[ -n "$CYC" ] && awk -v c="$CYC" -v ms="$TOTAL_WALL_MS" 'BEGIN{printf "rt_ratio_run=%.3f\n", c/486000000.0/(ms/1000.0)}' >> "$OUT/summary.txt"
awk '{s+=$2;n++} END{if(n)printf "field_ratio=%.3f\n", s/n/60.0}' "$OUT/vi.measured" >> "$OUT/summary.txt" 2>/dev/null
grep -o 'fallback=[0-9]*' "$OUT/shutdown.txt" | head -1 | sed 's/^/shutdown_/' >> "$OUT/summary.txt" 2>/dev/null || true
grep -o 'native=[0-9]*' "$OUT/shutdown.txt" | head -1 | sed 's/^/shutdown_/' >> "$OUT/summary.txt" 2>/dev/null || true
echo "exit_code=$RC" >> "$OUT/summary.txt"
[ "${STALLED:-0}" = 1 ] && echo "stalled=1 (heartbeat kill)" >> "$OUT/summary.txt"
scene_verification_line >> "$OUT/summary.txt"
guest_progress_note >> "$OUT/summary.txt"

# The benchmark driver terminates a healthy runner itself. Any exit observed
# during the sampling window is already captured by STALLED/gate_failure.
if [ -n "$gate_failure" ] || [ "${STALLED:-0}" = 1 ] || fatal_guest_marker || [ "$RC" -ne 0 ]; then
  [ -n "$gate_failure" ] || gate_failure="runner exit_code=$RC or fatal guest marker"
  echo "result=FAIL" >> "$OUT/summary.txt"; echo "failure=$gate_failure" >> "$OUT/summary.txt"
  echo "outdir=$OUT"; exit 1
fi
if [ "$SCENE_VERIFIED" != 1 ]; then
  echo "result=UNVERIFIED" >> "$OUT/summary.txt"
  echo "failure=scene/checkpoint acceptance requires a matching --expect-route record" >> "$OUT/summary.txt"
  echo "outdir=$OUT"; exit 1
fi
echo "result=PASS" >> "$OUT/summary.txt"

echo "=== $NAME ==="; cat "$OUT/summary.txt"; echo "outdir=$OUT"
