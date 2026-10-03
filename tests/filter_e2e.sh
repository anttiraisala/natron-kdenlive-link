#!/usr/bin/env bash
# End-to-end tests of the MLT filter: real daemon + mock worker + the filter loaded
# by a real MLT library. Usage: filter_e2e.sh <bin dir> <module .so> <MLT_ROOT>
set -u
BIN="${1:?bin dir}"; MODULE="${2:?module}"; MLT_ROOT="${3:?MLT root}"
ROOT="$(mktemp -d /tmp/nkb-fe2e.XXXXXX)"
export NKB_HOME="$ROOT/home"; mkdir -p "$NKB_HOME"
# Test module repository: the MLT core/xml modules plus our module.
REPO="$ROOT/repo"; mkdir -p "$REPO"
for f in "$MLT_ROOT"/lib/mlt-7/*.so; do ln -s "$f" "$REPO/"; done
ln -s "$MODULE" "$REPO/libmltnatron.so"
export MLT_REPOSITORY="$REPO" MLT_DATA="$MLT_ROOT/share/mlt-7" LD_LIBRARY_PATH="$MLT_ROOT/lib"
CHECK="$BIN/nkb-filter-check"
PIDS=(); FAILS=0
cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT
pass() { echo "PASS  $1"; }
fail() { echo "FAIL  $1"; FAILS=$((FAILS+1)); }

cat > "$NKB_HOME/config.ini" <<EOF
[daemon]
filter_address = unix:$ROOT/f.sock
worker_address = unix:$ROOT/w.sock
natron_timeout_seconds = 10
stats_log_interval_seconds = 0
[logging]
level = debug
log_file = $ROOT/log.txt
console = false
EOF
start_daemon() {
  "$BIN/natron-kdenlive-daemon" > "$ROOT/daemon.out" 2>&1 & DPID=$!; PIDS+=($DPID)
  for _ in $(seq 1 50); do "$BIN/natron-kdenlive-cache" --ping >/dev/null 2>&1 && return 0; sleep 0.1; done
  echo "daemon did not start"; return 1
}
stop_daemon() { kill "$DPID" 2>/dev/null; wait "$DPID" 2>/dev/null; }
start_worker() { "$BIN/nkb-mock-worker" "$@" > "$ROOT/worker.out" 2>&1 & WPID=$!; PIDS+=($WPID); sleep 0.3; }
field() { sed -n "s/.*[ ]$1=\([0-9]*\).*/\1/p"; }   # field NAME < line
stat_of() { "$BIN/natron-kdenlive-cache" --status | grep "^$1=" | cut -d= -f2; }

echo "== metadata: Kdenlive needs it to list the effect"
if [ -x "$MLT_ROOT/bin/melt" ]; then
  Q=$("$MLT_ROOT/bin/melt" -query filter=natron_link 2>/dev/null)
  echo "$Q" | grep -q "identifier: natron_link" && echo "$Q" | grep -q -- "- Video" && echo "$Q" | grep -q "identifier: ntp" \
     && pass "melt -query filter=natron_link returns identifier, Video tag and parameters" || fail "metadata missing: $Q"
fi

echo "== melt command line loads the module and runs frames through it"
start_daemon; start_worker --mode invert
: > "$ROOT/log.txt"
if [ -x "$MLT_ROOT/bin/melt" ]; then
  "$MLT_ROOT/bin/melt" color:red out=2 -filter natron_link mode=export comp=melt_test -consumer null real_time=-1 >/dev/null 2>&1
  grep -q "event=filter_created" "$ROOT/log.txt" && grep -q "comp=melt_test.*result=rendered" "$ROOT/log.txt" \
     && pass "melt: module loaded, frames rendered through daemon and worker" || fail "melt: module did not process frames"
else
  echo "SKIP  melt binary not found in $MLT_ROOT/bin"
fi
kill "$WPID" 2>/dev/null; stop_daemon

echo "== no daemon: frames pass through unchanged, no hang"
OUT=$(timeout 30 "$CHECK" --frames 5 --playback-timeout-ms 100)
echo "$OUT" | grep -q "unchanged=5 other=0" && pass "no daemon: 5 frames unchanged" || fail "no daemon: $OUT"
MS=$(echo "$OUT" | field elapsed_ms)
[ "${MS:-99999}" -lt 3000 ] && pass "no daemon: fast (${MS} ms, back-off works)" || fail "no daemon too slow: $MS ms"

echo "== daemon + worker (invert), export mode waits for the render"
start_daemon; start_worker --mode invert
OUT=$("$CHECK" --frames 8 --mode export)
echo "$OUT" | grep -q "inverted=8 unchanged=0 other=0" && pass "export: 8 frames processed" || fail "export: $OUT"
OUT=$("$CHECK" --frames 8 --mode playback --playback-timeout-ms 0)
echo "$OUT" | grep -q "inverted=8" && pass "playback: second pass served from cache" || fail "playback cache pass: $OUT"
[ "$(stat_of jobs_completed)" = "8" ] && pass "worker rendered each of the 8 frames exactly once" || fail "jobs_completed=$(stat_of jobs_completed)"

echo "== mode=auto follows the consumer real_time"
OUT=$("$CHECK" --frames 3 --start 20 --mode auto --real-time -1 --export-timeout-ms 5000)
echo "$OUT" | grep -q "inverted=3" && pass "auto + real_time=-1 acts as export (waited and processed)" || fail "auto export: $OUT"
grep -q "mode=export export_reason=real_time consumer=unnamed real_time=-1" "$ROOT/log.txt" \
   && pass "log shows mode=export, reason real_time, real_time=-1" || fail "log mode line"
: > "$ROOT/log.txt"
"$CHECK" --frames 2 --start 30 --mode auto --real-time 1 --consumer-service avformat --export-timeout-ms 5000 >/dev/null
grep -q "mode=export export_reason=consumer_avformat consumer=avformat real_time=1" "$ROOT/log.txt" \
   && pass "a file-writing consumer (avformat) is detected as export even with real_time=1" || fail "avformat detection"
: > "$ROOT/log.txt"
"$CHECK" --frames 2 --start 32 --mode auto --real-time 1 --consumer-service sdl2_audio >/dev/null
grep -q "mode=playback export_reason=none consumer=sdl2_audio real_time=1" "$ROOT/log.txt" \
   && pass "a display consumer (sdl2_audio) with real_time=1 stays playback" || fail "sdl detection"
: > "$ROOT/log.txt"
"$MLT_ROOT/bin/melt" color:red out=1 -filter natron_link comp=melt_auto -consumer null real_time=1 >/dev/null 2>&1
grep -q "comp=melt_auto.*mode=export export_reason=process_melt" "$ROOT/log.txt" \
   && pass "melt process with real_time=1 is detected as export (what Kdenlive's render does)" || fail "process_melt detection: $(grep melt_auto "$ROOT/log.txt" | head -1 | cut -c1-220)"

echo "== changing the .ntp file changes the keys (no stale frames)"
echo "v1" > "$ROOT/comp.ntp"
"$CHECK" --frames 3 --start 40 --ntp "$ROOT/comp.ntp" --mode export >/dev/null
BEFORE=$(stat_of jobs_completed)
"$CHECK" --frames 3 --start 40 --ntp "$ROOT/comp.ntp" --mode export >/dev/null
SAME=$(stat_of jobs_completed)
echo "v2" > "$ROOT/comp.ntp"
"$CHECK" --frames 3 --start 40 --ntp "$ROOT/comp.ntp" --mode export >/dev/null
AFTER=$(stat_of jobs_completed)
[ "$BEFORE" = "$SAME" ] && [ "$AFTER" = "$((SAME+3))" ] && pass "unchanged .ntp reuses cache, edited .ntp re-renders" || fail ".ntp keys: $BEFORE $SAME $AFTER"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== slow worker: playback passes through, later pull is processed"
start_daemon; start_worker --mode invert --delay-ms 400
OUT=$("$CHECK" --frames 3 --start 60 --mode playback --playback-timeout-ms 50)
echo "$OUT" | grep -q "unchanged=3" && pass "slow: frames passed through during playback" || fail "slow first pass: $OUT"
sleep 2.5
OUT=$("$CHECK" --frames 3 --start 60 --mode playback --playback-timeout-ms 0)
echo "$OUT" | grep -q "inverted=3" && pass "slow: renders finished and were cached" || fail "slow second pass: $OUT"
kill "$WPID" 2>/dev/null; stop_daemon

echo "== wrong token: pass-through, no crash"
start_daemon
OLD="$NKB_HOME/token"; cp "$OLD" "$OLD.bak"; echo "deadbeef" > "$OLD"
OUT=$("$CHECK" --frames 2 --mode playback --playback-timeout-ms 100)
cp "$OLD.bak" "$OLD"
echo "$OUT" | grep -q "unchanged=2" && pass "wrong token: frames unchanged" || fail "wrong token: $OUT"
stop_daemon

echo "== log has filter lines in the shared format"
grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z \[debug\] \[filter\] \[t=[0-9]+\] event=filter_frame' "$ROOT/log.txt" \
   && pass "filter_frame lines present" || fail "filter log lines"

echo
if [ "$FAILS" -eq 0 ]; then echo "ALL FILTER TESTS PASSED"; rm -rf "$ROOT"; exit 0; fi
echo "$FAILS FILTER TEST(S) FAILED (artifacts in $ROOT)"; exit 1
