#!/bin/sh
# Mounts this example, reads every output, rewrites input.txt to cause a
# rebuild, and saves what makebelieve recorded to trace.json beside this script.
#
#   ./demo.sh [path to makebelieve]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
exe=$(realpath "${1:-$here/../../build/src/makebelieve}")
mount=${TMPDIR:-/tmp}/makebelieve-tracing-demo
saved=$here/trace.json

(cd "$here" && "$exe" mount "$mount" >/dev/null 2>&1 &)
trap '"$exe" unmount "$mount"' EXIT

tries=0
until [ -e "$mount/tracing.json" ]; do
  tries=$((tries + 1))
  if [ "$tries" -gt 100 ]; then
    echo "makebelieve did not mount $mount" >&2
    exit 1
  fi
  sleep 0.1
done

echo 'Reading every output (the first read of each builds it)...'
for output in fast.txt slow.txt copied.txt nested/direct.txt; do
  echo "  $output -> $(cat "$mount/$output")"
done

echo 'Rewriting input.txt, which rebuilds the outputs that read it...'
contents=$(cat "$here/input.txt")
printf '%s\n' "$contents" > "$here/input.txt"
sleep 1
cat "$mount/copied.txt" >/dev/null

# Read before unmounting, as the trace only exists while mounted.
cp "$mount/tracing.json" "$saved"

echo
echo "Saved the trace to $saved"
echo 'Open it at https://ui.perfetto.dev (Open trace file) or chrome://tracing.'
