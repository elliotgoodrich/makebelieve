#!/usr/bin/env bash
# Black-box smoke test for the FUSE experiment:
#   1. Nothing is notified while nobody reads the file.
#   2. A read is followed by exactly one inotify IN_MODIFY event.
#   3. Reading again afterwards shows the content actually changed.
#   4. That second read schedules exactly one more notification
# It also checks that writes from anyone but the daemon itself are
# rejected.
#
# A single log file records both streams - "EVENT" lines from
# inotify_wait's continuous background watch, and "READ <time>" lines
# from this script's own reads - so a failure has one interleaved
# transcript to inspect instead of two separate, unsynchronized ones.
#
# Usage: smoke_test.sh <fuse_experiment-binary> <inotify_wait-binary>
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <fuse_experiment-binary> <inotify_wait-binary>" >&2
  exit 2
fi
FUSE_BIN="$1"
INOTIFY_WAIT_BIN="$2"

if [[ ! -e /dev/fuse ]]; then
  echo "FAIL: /dev/fuse not accessible in this environment" >&2
  exit 1
fi

# -u: a unique path that doesn't exist yet, not an already-created
# directory - fuse_experiment now creates (and fails if it can't create)
# its own mountpoint, matching how it must always be freshly created.
MNT="$(mktemp -u -d)"
LOG="$(mktemp)"
DAEMON_PID=""
WATCH_PID=""

# Wait up to ~3s for the daemon in $1 to exit, then SIGKILL and reap it.
# Bounded so a daemon whose mount didn't come down can't wedge `wait` until
# ctest's TIMEOUT. Teardown only - phase 6 asserts a clean exit.
reap_daemon() {
  local pid="$1" _
  for _ in $(seq 1 30); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

cleanup() {
  if [[ -n "$WATCH_PID" ]] && kill -0 "$WATCH_PID" 2>/dev/null; then
    kill "$WATCH_PID" 2>/dev/null || true
    wait "$WATCH_PID" 2>/dev/null || true
  fi
  if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
    # SIGTERM tells libfuse to unmount and exit; reap_daemon then guarantees
    # the process is gone. We avoid `fusermount3 -u` - its unprivileged
    # unmount is denied on some sandboxed runners.
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    reap_daemon "$DAEMON_PID"
  fi
  rm -f "$LOG"
  # fuse_experiment removes $MNT itself after a clean unmount; this is
  # just a safety net for paths where the daemon was killed instead
  # (e.g. an earlier `fail`), so $MNT may already be gone.
  rmdir "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

fail() {
  echo "FAIL: $1" >&2
  echo "--- combined event/read log ---" >&2
  cat "$LOG" >&2
  exit 1
}

"$FUSE_BIN" -f "$MNT" &
DAEMON_PID=$!

for _ in $(seq 1 50); do
  mountpoint -q "$MNT" && break
  sleep 0.1
done
if ! mountpoint -q "$MNT"; then
  fail "mount did not become ready within 5s"
fi

# Reads its own timestamp out of the file, records "READ <time>" in the
# shared log (so it lines up with the EVENT lines below), and echoes the
# timestamp for the caller to compare across reads.
record_read() {
  local t
  t="$(grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}' \
       "$MNT/time.txt")"
  echo "READ $t" >> "$LOG"
  echo "$t"
}

event_count() {
  # grep -c exits 1 on zero matches; zero is a valid count, so don't let
  # set -e treat it as an error.
  grep -c '^EVENT$' "$LOG" || true
}

# Polls event_count in 0.1s steps, returning as soon as it reaches
# $1, rather than always waiting the full $2-second ceiling - the write
# a read schedules lands at the next whole-second boundary, so it's
# typically well under 1s away, not a fixed worst case. $2 is only a
# safety ceiling for a slow/loaded machine, not the expected duration.
wait_for_count() {
  local target="$1" timeout_s="$2" count=0
  local -i max_iters=$((timeout_s * 10))
  for ((i = 0; i < max_iters; i++)); do
    count="$(event_count)"
    [[ "$count" -ge "$target" ]] && break
    sleep 0.1
  done
  echo "$count"
}

# One continuous watch spanning the whole scenario below, rather than
# re-arming per phase - a fresh watch started right before a read could
# race the write it's trying to observe.
"$INOTIFY_WAIT_BIN" "$MNT/time.txt" 10 >> "$LOG" &
WATCH_PID=$!
sleep 0.2  # give the watch time to arm

# --- 1. nobody has read the file yet: no notification should arrive ---
# Unlike the phases below we just need to wait a bit to check that no
# notifications arrive.
sleep 1.5
count="$(event_count)"
if [[ "$count" -ne 0 ]]; then
  fail "got $count notification(s) with nobody reading (expected 0)"
fi
echo "OK: no notification while idle"

# --- 2. a read is followed by exactly one notification ---
t1="$(record_read)"
count="$(wait_for_count 1 3)"
if [[ "$count" -ne 1 ]]; then
  fail "expected exactly 1 notification after a read, got $count"
fi
echo "OK: exactly one notification after a read"

# --- 3. reading again shows the content actually changed ---
t2="$(record_read)"
if [[ "$t1" == "$t2" ]]; then
  fail "content did not change after the notification (still '$t1')"
fi
echo "OK: content changed ('$t1' -> '$t2')"

# --- 4. that second read schedules exactly one more notification ---
count="$(wait_for_count 2 3)"
if [[ "$count" -ne 2 ]]; then
  fail "expected exactly 2 total notifications after the second read, got $count"
fi
# A short settle window: catches a runaway heartbeat immediately firing a
# third event, without paying a full extra second on every run just to
# rule that out.
sleep 0.5
count="$(event_count)"
if [[ "$count" -ne 2 ]]; then
  fail "expected exactly 2 total notifications after the second read, got $count"
fi
echo "OK: exactly one more notification after the second read"

# --- 5. writes from anyone else are rejected ---
if echo -n x > "$MNT/time.txt" 2>/dev/null; then
  fail "external write to time.txt unexpectedly succeeded"
fi
echo "OK: external write rejected"

# --- 6. a signaled shutdown unmounts and removes the mountpoint ---
# Stop the daemon the way it's meant to stop - SIGTERM, which libfuse turns
# into its own unmount - and assert it exits and clears its mountpoint. We
# avoid `fusermount3 -u`, whose unprivileged unmount is denied on some
# sandboxed runners; the daemon's own unmount takes a different path.
kill -TERM "$DAEMON_PID"
daemon_exited=0
for _ in $(seq 1 50); do
  kill -0 "$DAEMON_PID" 2>/dev/null || { daemon_exited=1; break; }
  sleep 0.1
done
if [[ "$daemon_exited" -ne 1 ]]; then
  fail "daemon still running 5s after SIGTERM"
fi
wait "$DAEMON_PID" 2>/dev/null || true  # reap; a signaled exit is non-zero
DAEMON_PID=""  # reaped; cleanup() shouldn't touch it again
if [[ -e "$MNT" ]]; then
  fail "mountpoint still exists after shutdown"
fi
echo "OK: shutdown unmounted and removed the mountpoint"
