#!/usr/bin/env bash
# Black-box smoke test for the FUSE tracing experiment: reads a handful of
# files (some more than once, some out of alphabetical order, one via ".."
# into the parent directory, one never at all), then diffs the command's
# entire output against an exact expectation. That single diff covers:
#   - files read from the current directory are reported,
#   - files read from a subdirectory are reported,
#   - a file read via ".." into the parent directory is NOT reported -
#     parent directories are never tracked,
#   - a file that is never read is not reported,
#   - a file read multiple times is reported exactly once, not once per
#     read,
#   - the report is sorted alphabetically, not in read order (aaa.txt is
#     read last but must be listed first).
#
# Usage: smoke_test.sh <fuse_tracing_experiment-binary>
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <fuse_tracing_experiment-binary>" >&2
  exit 2
fi
BIN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

if [[ ! -e /dev/fuse ]]; then
  echo "FAIL: /dev/fuse not accessible in this environment" >&2
  exit 1
fi

PARENT="$(mktemp -d)"
WORKDIR="$PARENT/work"
mkdir "$WORKDIR"
mkdir "$WORKDIR/sub"

echo "top-level content" > "$WORKDIR/top.txt"
echo "nested content" > "$WORKDIR/sub/nested.txt"
echo "aaa content" > "$WORKDIR/aaa.txt"
echo "untouched content" > "$WORKDIR/untouched.txt"
echo "outside content" > "$PARENT/outside.txt"

cleanup() {
  rm -rf "$PARENT"
}
trap cleanup EXIT

OUT="$(mktemp)"
EXPECTED="$(mktemp)"

# Read top.txt and sub/nested.txt twice each (dedup), aaa.txt last even
# though it must sort first (sort-order), and ../outside.txt once (must
# not be tracked). cat's own stdout and the tracer's report share one fd,
# so the expected output below interleaves both, in the order they're
# actually written: cat's content as each arg is read, then the tracer's
# report once cat has exited.
if ! ( cd "$WORKDIR" && \
       "$BIN" cat top.txt sub/nested.txt top.txt sub/nested.txt \
         ../outside.txt aaa.txt ) > "$OUT" 2>&1; then
  :
fi

{
  echo "top-level content"
  echo "nested content"
  echo "top-level content"
  echo "nested content"
  echo "outside content"
  echo "aaa content"
  echo "Files read from $WORKDIR:"
  echo "  $WORKDIR/aaa.txt"
  echo "  $WORKDIR/sub/nested.txt"
  echo "  $WORKDIR/top.txt"
} > "$EXPECTED"

if ! diff -u "$EXPECTED" "$OUT"; then
  echo "FAIL: report did not match exactly" >&2
  exit 1
fi
echo "OK: report matches exactly" \
     "(dedup, parent-dir exclusion, and sort order all correct)"

rm -f "$OUT" "$EXPECTED"
