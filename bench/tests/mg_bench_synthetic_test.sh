#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/runner-dir" "$TMP/fake-bin"
# Windows Git Bash does not ship flock. The benchmark's lock descriptor is
# private to this synthetic harness, so provide a no-op flock shim instead of
# touching the machine-wide lock used by actual runs.
cat > "$TMP/fake-bin/flock" <<'FLOCK'
#!/usr/bin/env bash
exit 0
FLOCK
chmod +x "$TMP/fake-bin/flock"
touch "$TMP/state" "$TMP/module"
cat > "$TMP/runner-dir/fake-runner" <<'RUNNER'
#!/usr/bin/env bash
set -u
state=""
while [ $# -gt 0 ]; do
  [ "$1" = --load-state ] && { state="$2"; shift 2; continue; }
  shift
done
case "${MODE:-good}" in
  prefix_restore)
    echo "[state] loaded '$state-extra'"
    echo '[vi] fields=1 +60.0 Hz'
    sleep 30
    ;;
  silent_restore)
    echo '[vi] fields=1 +60.0 Hz'
    sleep 30
    ;;
  parked)
    echo "[state] loaded '$state'"
    field=0
    while :; do
      field=$((field+1))
      echo "[vi] fields=$field +60.0 Hz"
      echo '[g1tick] wall_ms=1000 tb=0 tb_delta=0 logic_frames=0 wall_delta_ms=1000'
      sleep 0.1
    done
    ;;
  fatal_exit0)
    echo "[state] loaded '$state'"
    trap 'exit 0' TERM
    field=0; tb=0
    while [ "$field" -lt 12 ]; do
      field=$((field+1)); tb=$((tb+1350000))
      echo "[vi] fields=$field +60.0 Hz"
      echo "[g1tick] wall_ms=$((field*1000)) tb=$tb tb_delta=1350000 logic_frames=1 wall_delta_ms=1000"
      [ "$field" -eq 5 ] && echo '[panic] FATAL guest failure'
      sleep 0.1
    done
    exit 0
    ;;
  guest_stall|os_panic|os_exception|load_failed_anywhere)
    echo "[state] loaded '$state'"
    trap 'exit 0' TERM
    field=0; tb=0
    while [ "$field" -lt 12 ]; do
      field=$((field+1)); tb=$((tb+1350000))
      echo "[vi] fields=$field +60.0 Hz"
      echo "[g1tick] wall_ms=$((field*1000)) tb=$tb tb_delta=1350000 logic_frames=1 wall_delta_ms=1000"
      [ "$field" -eq 5 ] && break
      sleep 0.1
    done
    case "$MODE" in
      guest_stall) echo 'guest warning [stall] execution stopped';;
      os_panic) echo '[Core] OSPanic: synthetic fatal guest error';;
      os_exception) echo '[Core] OSDefaultExceptionHandler: synthetic exception';;
      load_failed_anywhere) echo '[Core] state restore load FAILED in guest';;
    esac
    sleep 30
    ;;
  crash_after_vi)
    echo "[state] loaded '$state'"
    echo '[vi] fields=1 +60.0 Hz'
    sleep 1
    echo '[vi] fields=2 +60.0 Hz'
    sleep 1
    kill -SEGV $$
    ;;
  good)
    echo "[state] loaded '$state'"
    echo '[scene] synthetic-checkpoint'
    trap 'exit 0' TERM
    field=0
    tb=0
    while :; do
      field=$((field+1)); tb=$((tb+1350000))
      echo "[vi] fields=$field +60.0 Hz"
      echo "[g1tick] wall_ms=$((field*1000)) tb=$tb tb_delta=1350000 logic_frames=1 wall_delta_ms=1000"
      sleep 0.1
    done
    ;;
  vi_only)
    echo "[state] loaded '$state'"
    trap 'exit 0' TERM
    field=0; tb=0
    while :; do
      field=$((field+1)); tb=$((tb+1350000))
      echo "[vi] fields=$field +60.0 Hz"
      echo "[g1tick] wall_ms=$((field*1000)) tb=$tb tb_delta=1350000 logic_frames=1 wall_delta_ms=1000"
      sleep 0.1
    done
    ;;
esac
RUNNER
chmod +x "$TMP/runner-dir/fake-runner"

run_case() {
  local mode="$1" expectation="$2" runner_mode="${3:-$1}" out name rc
  out="$TMP/out-$mode"; name="case-$mode"
  set +e
  local scene_args=()
  if [ "$expectation" = pass ]; then
    scene_args=(--expect-route synthetic-checkpoint --expect-log '^\[scene\] synthetic-checkpoint')
  elif [ "$mode" = vi_false_expect ]; then
    scene_args=(--expect-route synthetic-checkpoint --expect-log '^\[vi\]')
  fi
  PATH="$TMP/fake-bin:$PATH" MODE="$runner_mode" MG_BENCH_LOCK="$TMP/bench.lock" \
    "$BENCH_DIR/mg_bench.sh" --name "$name" --state "$TMP/state" \
    --module "$TMP/module" --runner "$TMP/runner-dir/fake-runner" \
    --duration 2 --warmup 2 --startup 5 "${scene_args[@]}" --outdir "$out" >"$TMP/$mode.stdout" 2>&1
  rc=$?
  set -e
  local summary
  summary="$(find "$out" -name summary.txt -print -quit)"
  [ -n "$summary" ] || { cat "$TMP/$mode.stdout"; echo "missing summary: $mode" >&2; return 1; }
  if [ "$expectation" = pass ]; then
    [ "$rc" -eq 0 ] && grep -q '^result=PASS$' "$summary" && grep -q '^scene_verification=VERIFIED route=synthetic-checkpoint$' "$summary"
  elif [ "$expectation" = unverified ]; then
    [ "$rc" -ne 0 ] && grep -q '^result=UNVERIFIED$' "$summary" && grep -q '^scene_verification=UNVERIFIED' "$summary" && grep -q '^mean_vi_hz=' "$summary"
  else
    [ "$rc" -ne 0 ] && grep -q '^result=FAIL$' "$summary"
  fi
  [ "$mode" != vi_false_expect ] || grep -q "no \[scene\]/\[checkpoint\] line matched route 'synthetic-checkpoint'" "$summary"
  [ "$mode" != parked ] || grep -q 'no guest timebase/logic progress during warmup' "$summary"
  case "$mode" in
    fatal_exit0|guest_stall|os_panic|os_exception|load_failed_anywhere) grep -q '^failure=fatal guest marker$' "$summary";;
  esac
}

run_case good pass good
run_case good_unverified unverified good
run_case vi_false_expect unverified vi_only
run_case crash_after_vi fail
run_case fatal_exit0 fail
run_case silent_restore fail
run_case prefix_restore fail
run_case parked fail
run_case guest_stall fail
run_case os_panic fail
run_case os_exception fail
run_case load_failed_anywhere fail
echo 'mg_bench synthetic tests: PASS'
