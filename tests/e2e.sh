#!/usr/bin/env bash
# End-to-end tests: real daemon + mock worker + mock filter over Unix sockets
# and over loopback TCP. Usage: e2e.sh <directory containing the binaries>
set -u
BIN="${1:?usage: e2e.sh <bin dir>}"
ROOT="$(mktemp -d /tmp/nkb-e2e.XXXXXX)"
export NKB_HOME="$ROOT/home"
mkdir -p "$NKB_HOME"
PIDS=()
FAILS=0

cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILS=$((FAILS+1)); }

write_config() {  # $1=filter addr $2=worker addr $3=behavior $4=queue_mb $5=worker timeout s
cat > "$NKB_HOME/config.ini" <<EOF
[daemon]
filter_address = $1
worker_address = $2
buffer_behavior = $3
input_queue_memory_mb = $4
natron_timeout_seconds = $5
cache_memory_mb = 256
cache_memory_min_mb = 1
stats_log_interval_seconds = 0
[natron]
start_worker = ${START_WORKER:-false}
worker_command = ${WORKER_COMMAND:-snap run natron}
[logging]
level = debug
log_file = $ROOT/log.txt
console = false
EOF
}

start_daemon() {
  "$BIN/natron-kdenlive-daemon" > "$ROOT/daemon.out" 2>&1 &
  DPID=$!; PIDS+=($DPID)
  for _ in $(seq 1 50); do "$BIN/natron-kdenlive-cache" --ping >/dev/null 2>&1 && return 0; sleep 0.1; done
  echo "daemon did not start"; cat "$ROOT/daemon.out"; return 1
}
stop_daemon() { kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null; }
start_worker() { "$BIN/nkb-mock-worker" "$@" > "$ROOT/worker.out" 2>&1 & WPID=$!; PIDS+=($WPID); sleep 0.3; }
stat_of() { "$BIN/natron-kdenlive-cache" --status | grep "^$1=" | cut -d= -f2; }

run_scenario() {  # $1=name $2=filter addr $3=worker addr
  local name="$1"
  write_config "$2" "$3" buffer_skip 512 10
  start_daemon || { fail "$name: daemon start"; return; }
  start_worker --mode invert

  "$BIN/nkb-mock-filter" --frames 10 --expect ok=10,cache_hit=0 >/dev/null \
     && pass "$name: 10 frames rendered and pixel-verified" || fail "$name: first pass"
  "$BIN/nkb-mock-filter" --frames 10 --expect cache_hit=10,ok=0 >/dev/null \
     && pass "$name: second pass served entirely from cache" || fail "$name: cache pass"
  "$BIN/nkb-mock-filter" --frames 10 --seed 2 --expect ok=10,cache_hit=0 >/dev/null \
     && pass "$name: changed input produces new keys (no stale frames)" || fail "$name: seed change"
  [ "$(stat_of cache_entries)" = "20" ] && pass "$name: cache holds 20 entries" || fail "$name: cache entries=$(stat_of cache_entries)"
  "$BIN/natron-kdenlive-cache" --clear | grep -q "cleared_entries=20" \
     && pass "$name: cache clear" || fail "$name: clear"
  [ "$(stat_of cache_entries)" = "0" ] && pass "$name: cache empty after clear" || fail "$name: not empty"
  "$BIN/nkb-mock-filter" --frames 1 --token wrong >/dev/null 2>&1 \
     && fail "$name: wrong token was accepted" || pass "$name: wrong token rejected"
  "$BIN/natron-kdenlive-doctor" | grep -q "check=daemon result=ok" \
     && pass "$name: doctor sees daemon" || fail "$name: doctor"
  stop_daemon; kill "$WPID" 2>/dev/null
}

echo "== unix sockets"
run_scenario unix "unix:$ROOT/f.sock" "unix:$ROOT/w.sock"
echo "== loopback tcp"
PORT=$((20000 + RANDOM % 20000))
run_scenario tcp "tcp:127.0.0.1:$PORT" "tcp:127.0.0.1:$((PORT+1))"

echo "== no worker: requests pass through immediately instead of waiting"
write_config "unix:$ROOT/f.sock" "unix:$ROOT/w.sock" buffer_skip 512 10
start_daemon
OUT=$("$BIN/nkb-mock-filter" --frames 4 --timeout-ms 3000 --expect passthrough=4)
echo "$OUT" | grep -q "passthrough=4" && pass "no worker: passthrough status, filter keeps its own frame" || fail "no worker: $OUT"
MS=$(echo "$OUT" | sed -n 's/.*elapsed_ms=\([0-9]*\).*/\1/p')
[ "${MS:-99999}" -lt 1000 ] && pass "no worker: 4 requests with a 3 s timeout answered in ${MS} ms" || fail "no worker waited: ${MS} ms"
start_worker --mode invert
sleep 1
"$BIN/nkb-mock-filter" --frames 4 --expect cache_hit=4 >/dev/null \
   && pass "no worker: queued frames were rendered once a worker connected" || fail "queued frames after worker start"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== slow worker: result is cached after the waiter timed out"
start_daemon
start_worker --mode invert --delay-ms 500
"$BIN/nkb-mock-filter" --frames 3 --start 100 --timeout-ms 100 --expect timeout=3 >/dev/null \
   && pass "slow worker: filter passed through" || fail "slow worker: timeout pass"
sleep 2.5
"$BIN/nkb-mock-filter" --frames 3 --start 100 --expect cache_hit=3 >/dev/null \
   && pass "slow worker: later pull returns the rendered frames" || fail "slow worker: later pull"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== queue-only playback mode with buffer_skip"
write_config "unix:$ROOT/f.sock" "unix:$ROOT/w.sock" buffer_skip 1 10
start_daemon
# 320x180 RGBA8 = 230400 bytes; a 1 MB queue holds 4 frames, so 10 queued frames drop 6.
"$BIN/nkb-mock-filter" --frames 10 --timeout-ms 0 --expect passthrough=10 >/dev/null \
   && pass "queue-only: 10 requests queued without waiting" || fail "queue-only"
[ "$(stat_of jobs_skipped)" = "6" ] && pass "buffer_skip dropped the 6 oldest" || fail "skipped=$(stat_of jobs_skipped)"
start_worker --mode invert
sleep 1.5
"$BIN/nkb-mock-filter" --frames 6 --start 0 --timeout-ms 0 --expect passthrough=6 >/dev/null  # frames 0-5: dropped earlier
"$BIN/nkb-mock-filter" --frames 4 --start 6 --expect cache_hit=4 >/dev/null \
   && pass "buffer_skip kept and rendered the 4 newest" || fail "newest 4"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== show_cached policy rejects when the queue is full"
write_config "unix:$ROOT/f.sock" "unix:$ROOT/w.sock" show_cached 1 10
start_daemon
"$BIN/nkb-mock-filter" --frames 10 --timeout-ms 0 --expect passthrough=4,rejected=6 >/dev/null \
   && pass "show_cached: 4 queued, 6 rejected" || fail "show_cached"
stop_daemon

echo "== pause policy: overflowing requests wait for space, then time out"
write_config "unix:$ROOT/f.sock" "unix:$ROOT/w.sock" pause 1 10
start_daemon
"$BIN/nkb-mock-filter" --frames 10 --timeout-ms 0 --expect passthrough=4,timeout=6 >/dev/null \
   && pass "pause: 4 queued, 6 timed out waiting for queue space" || fail "pause"
stop_daemon

echo "== worker failure handling"
write_config "unix:$ROOT/f.sock" "unix:$ROOT/w.sock" buffer_skip 512 10
start_daemon
start_worker --mode invert --fail-frame 3
"$BIN/nkb-mock-filter" --frames 6 --expect ok=5,error=1 >/dev/null \
   && pass "worker error on one frame does not affect the others" || fail "fail-frame"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== stuck worker is dropped after natron_timeout_seconds"
write_config "unix:$ROOT/f.sock" "unix:$ROOT/w.sock" buffer_skip 512 2
start_daemon
start_worker --mode invert --hang-frame 1
"$BIN/nkb-mock-filter" --frames 2 --timeout-ms 4000 --expect ok=1,timeout=1 >/dev/null \
   && pass "stuck worker: timeout reported" || fail "hang"
sleep 0.5
[ "$(stat_of workers)" = "0" ] && pass "stuck worker connection dropped" || fail "workers=$(stat_of workers)"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== the daemon starts the worker and restarts it when it dies"
# Stand-in for "natron -t nkb_natron_worker.py": runs the mock worker and records its pid.
cat > "$ROOT/fake_natron.sh" <<EOS
#!/bin/sh
echo "\$\$ \$*" >> "$ROOT/worker_starts.txt"
exec "$BIN/nkb-mock-worker" --mode invert
EOS
chmod +x "$ROOT/fake_natron.sh"
START_WORKER=true WORKER_COMMAND="$ROOT/fake_natron.sh" write_config "unix:$ROOT/s6.sock" "unix:$ROOT/w6.sock" buffer_skip 512 10
: > "$ROOT/log.txt"
start_daemon
for _ in $(seq 1 30); do [ "$(stat_of workers)" = "1" ] && break; sleep 0.1; done
[ "$(stat_of workers)" = "1" ] && grep -q "nkb_natron_worker.py" "$ROOT/worker_starts.txt" \
   && pass "worker started by the daemon (-t .../nkb_natron_worker.py)" || fail "supervised start: workers=$(stat_of workers)"
"$BIN/nkb-mock-filter" --frames 2 --expect ok=2 >/dev/null && pass "supervised worker renders" || fail "supervised render"
W1=$(tail -1 "$ROOT/worker_starts.txt" | cut -d' ' -f1)
kill -9 "$W1" 2>/dev/null
for _ in $(seq 1 60); do [ "$(wc -l < "$ROOT/worker_starts.txt")" = "2" ] && [ "$(stat_of workers)" = "1" ] && break; sleep 0.1; done
grep -q "event=worker_exited pid=$W1 signal=9 .*restart=yes" "$ROOT/log.txt" && [ "$(stat_of workers)" = "1" ] \
   && pass "a killed worker is restarted (signal 9 logged, workers=1 again)" || fail "restart: $(grep -E 'worker_(exited|launched|restarting)' "$ROOT/log.txt" | cut -c1-160)"
"$BIN/nkb-mock-filter" --frames 2 --seed 7 --expect ok=2 >/dev/null && pass "the restarted worker renders" || fail "render after restart"
W2=$(tail -1 "$ROOT/worker_starts.txt" | cut -d' ' -f1)
stop_daemon
sleep 0.3
kill -0 "$W2" 2>/dev/null && fail "worker $W2 still running after the daemon stopped" || pass "stopping the daemon stops its worker"

echo "== log is structured"
grep -q "event=job_done" "$ROOT/log.txt" && grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z \[' "$ROOT/log.txt" \
   && pass "log lines have ISO timestamp, level and event=" || fail "log format"

echo
if [ "$FAILS" -eq 0 ]; then echo "ALL E2E TESTS PASSED"; rm -rf "$ROOT"; exit 0; fi
echo "$FAILS E2E TEST(S) FAILED (artifacts in $ROOT)"; exit 1
