#!/usr/bin/env bash
# End-to-end test of the real Natron worker: daemon + nkb_natron_worker.py (inside Natron) + mock filter.
# Usage: natron_e2e.sh <bin dir> <natron command...>
#   e.g. natron_e2e.sh build /path/to/NatronRenderer          or:  natron_e2e.sh build snap run natron
# Exits 0 and prints SKIP if no Natron command is given.
set -u
BIN="${1:?usage: natron_e2e.sh <bin dir> <natron command...>}"; shift
if [ $# -eq 0 ]; then echo "SKIP  no Natron command given"; exit 0; fi
NATRON=("$@")
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# For the snap: NKB_TEST_BASE=$HOME/nkb-test (exchange folder must be in your real home, not hidden; /tmp is private
# to a snap) and NKB_TEST_TCP=1 (a snap cannot reach Unix sockets outside its sandbox, loopback TCP works).
BASE="${NKB_TEST_BASE:-/tmp}"
ROOT="$(mktemp -d "$BASE/nkb-natron.XXXXXX")"
export NKB_HOME="$ROOT/home" NKB_COMPS_DIR="$ROOT/comps" NKB_EXCHANGE_DIR="$ROOT/exchange"
mkdir -p "$NKB_HOME"
PIDS=(); FAILS=0
# Ctrl-C or SIGTERM stops everything at once: ask nicely, then force. Natron and "snap run" can ignore SIGINT.
cleanup() {
  trap '' INT TERM
  for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
  pkill -f "$ROOT" 2>/dev/null          # anything started with this run's folder (e.g. the worker inside the snap)
  sleep 0.5
  for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null; done
  pkill -9 -f "$ROOT" 2>/dev/null
  wait 2>/dev/null
}
trap cleanup EXIT
trap 'echo; echo "interrupted, stopping everything"; exit 130' INT TERM
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILS=$((FAILS+1)); }

mkdir -p "$BASE"
FADDR="unix:$ROOT/f.sock"; WADDR="unix:$ROOT/w.sock"
if [ -n "${NKB_TEST_TCP:-}" ]; then P=$((30000 + RANDOM % 20000)); FADDR="tcp:127.0.0.1:$P"; WADDR="tcp:127.0.0.1:$((P+1))"; fi
cat > "$NKB_HOME/config.ini" <<EOF
[daemon]
filter_address = $FADDR
worker_address = $WADDR
natron_timeout_seconds = 30
stats_log_interval_seconds = 0
[natron]
start_worker = false
[logging]
level = debug
log_file = $ROOT/log.txt
console = false
EOF
"$BIN/natron-kdenlive-daemon" > "$ROOT/daemon.out" 2>&1 & DPID=$!; PIDS+=($DPID)
for _ in $(seq 1 50); do "$BIN/natron-kdenlive-cache" --ping >/dev/null 2>&1 && break; sleep 0.1; done

stat_of() { "$BIN/natron-kdenlive-cache" --status 2>/dev/null | sed -n "s/^$1=//p"; }
# The worker also exits by itself when this script is gone (NKB_EXIT_WITH_PID), so it can never be left behind.
export NKB_EXIT_WITH_PID=$$
start_worker() {  # env NKB_COLOR etc. are inherited. Returns once a NEW worker connection exists
  echo "      starting the worker inside Natron..."
  local before; before=$(stat_of worker_connections); before=${before:-0}
  "${NATRON[@]}" -t "$HERE/natron/nkb_natron_worker.py" > "$ROOT/worker.out" 2>&1 < /dev/null & WPID=$!; PIDS+=($WPID)
  for _ in $(seq 1 300); do
    [ "$(stat_of workers)" = "1" ] && [ "$(stat_of worker_connections)" -gt "$before" ] 2>/dev/null && return 0; sleep 0.2
  done
  echo "worker did not connect within 60 s"; grep -v -E "OpenFX: loading|OpenGL|libGL" "$ROOT/worker.out" | tail -20; return 1
}
# Asks the worker to stop (stop file), waits up to 10 s, then forces it. Never blocks on a process that ignores SIGTERM.
stop_worker() {
  touch "$NKB_HOME/worker.stop"
  for _ in $(seq 1 50); do kill -0 "$WPID" 2>/dev/null || break; sleep 0.2; done
  if kill -0 "$WPID" 2>/dev/null; then echo "      worker ignored the stop file, forcing it"; kill -9 "$WPID" 2>/dev/null; fi
  for _ in $(seq 1 25); do [ "$(stat_of workers)" = "0" ] && break; sleep 0.2; done   # the daemon has noticed it is gone
}
field() { sed -n "s/.*[ ]$1=\([0-9]*\).*/\1/p"; }

echo "== test comp (starts Natron once, can take 10-20 s)"
timeout 120 "${NATRON[@]}" -t "$HERE/natron/make_test_comp.py" < /dev/null 2>&1 | grep -q "made invert_rgb.ntp" \
   && pass "created invert_rgb.ntp inside Natron" || { fail "could not create test comp"; exit 1; }

echo "== default pass-through graph, sRGB mode (graph works in linear light)"
export NKB_COLOR=srgb
start_worker && pass "worker connected to daemon" || { fail "worker start"; exit 1; }
OUT=$("$BIN/nkb-mock-filter" --comp fresh_comp --frames 6 --width 640 --height 360 --alpha-min 64 --tolerance 6 --expect-transform passthrough --expect ok=6 --timeout-ms 60000)
echo "$OUT" | grep -q "ok=6" && echo "$OUT" | grep -q "mismatches=0" && pass "6 frames through Natron within +-6 RGB levels, alpha exact: $(echo "$OUT" | grep -o 'max_rgb_err=[0-9]* max_alpha_err=[0-9]*')" || fail "pass-through: $OUT"
[ -f "$NKB_COMPS_DIR/fresh_comp.ntp" ] && pass "default comp fresh_comp.ntp was created automatically" || fail "default comp missing"
OUT=$("$BIN/nkb-mock-filter" --comp fresh_comp --frames 6 --width 640 --height 360 --alpha-min 64 --tolerance 6 --expect-transform passthrough --expect cache_hit=6)
echo "$OUT" | grep -q "cache_hit=6" && pass "second pass served from the daemon cache" || fail "cache: $OUT"
stop_worker

echo "== user graph with an Invert node, raw colour mode (opaque pixels: Invert acts on premultiplied data)"
export NKB_COLOR=raw
start_worker || { fail "worker restart"; exit 1; }
OUT=$("$BIN/nkb-mock-filter" --comp invert_rgb --frames 4 --width 640 --height 360 --alpha-min 255 --tolerance 2 --expect-transform invert --expect ok=4 --timeout-ms 60000)
echo "$OUT" | grep -q "ok=4" && echo "$OUT" | grep -q "mismatches=0" && pass "Invert graph from a saved .ntp inverts RGB of opaque pixels: $(echo "$OUT" | grep -o 'max_rgb_err=[0-9]* max_alpha_err=[0-9]*')" || fail "invert comp: $OUT"

echo "== editing the .ntp on disk is picked up without restarting the worker"
cp "$NKB_COMPS_DIR/fresh_comp.ntp" "$NKB_COMPS_DIR/invert_rgb.ntp"   # replace the Invert graph by pass-through
OUT=$("$BIN/nkb-mock-filter" --comp invert_rgb --seed 9 --frames 3 --width 640 --height 360 --alpha-min 64 --tolerance 2 --expect-transform passthrough --expect ok=3 --timeout-ms 60000)
echo "$OUT" | grep -q "ok=3" && echo "$OUT" | grep -q "mismatches=0" && pass "worker reloaded the changed .ntp (now pass-through)" || fail "reload: $OUT"
grep -q "event=comp_loaded.*reason=file_changed" "$ROOT/log.txt" && pass "log shows comp_loaded reason=file_changed" || fail "reload log line"

echo "== the first frame after a load is correct (Natron 2.5.0 warm-up workaround)"
grep -q "event=warm_up_done" "$ROOT/log.txt" && pass "warm-up render ran after each load" || fail "no warm-up"
echo "== broken comp: error is reported, worker keeps serving"
echo "not a project" > "$NKB_COMPS_DIR/broken.ntp"
OUT=$("$BIN/nkb-mock-filter" --comp broken --frames 1 --width 64 --height 36 --timeout-ms 60000 --expect-transform passthrough)
echo "$OUT" | grep -q "error=1" && pass "broken comp reported as error status" || fail "broken comp: $OUT"
OUT=$("$BIN/nkb-mock-filter" --comp invert_rgb --seed 10 --frames 2 --width 64 --height 36 --alpha-min 64 --tolerance 2 --expect-transform passthrough --expect ok=2 --timeout-ms 60000)
echo "$OUT" | grep -q "ok=2" && echo "$OUT" | grep -q "mismatches=0" && pass "worker still serves correctly after a failed job (first frame after the reload is right)" || fail "after broken: $OUT"

echo "== 1080p timing (informational)"
OUT=$("$BIN/nkb-mock-filter" --comp invert_rgb --seed 20 --frames 8 --width 1920 --height 1080 --alpha-min 64 --tolerance 2 --expect-transform passthrough --timeout-ms 60000)
echo "$OUT"
grep "event=job_rendered" "$ROOT/log.txt" | grep "size=1920x1080" | tail -8 | sed 's/.*event=job_rendered //' | cut -c1-200
echo "== colour mode is applied: the same Invert graph in sRGB mode inverts in linear light"
stop_worker
"${NATRON[@]}" -t "$HERE/natron/make_test_comp.py" < /dev/null >/dev/null 2>&1   # restore the Invert graph
export NKB_COLOR=srgb
start_worker || { fail "worker restart (srgb)"; exit 1; }
OUT=$("$BIN/nkb-mock-filter" --comp invert_rgb --seed 5 --frames 3 --width 640 --height 360 --alpha-min 255 --tolerance 3 --expect-transform srgb-invert --expect ok=3 --timeout-ms 60000)
echo "$OUT" | grep -q "ok=3" && echo "$OUT" | grep -q "mismatches=0" && pass "srgb mode: Invert acts in linear light: $(echo "$OUT" | grep -o 'max_rgb_err=[0-9]*')" || fail "srgb invert: $OUT"

stop_worker

echo
if [ "$FAILS" -eq 0 ]; then echo "ALL NATRON WORKER TESTS PASSED"; rm -rf "$ROOT"; exit 0; fi
echo "$FAILS NATRON WORKER TEST(S) FAILED (artifacts in $ROOT)"; exit 1
