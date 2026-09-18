#!/usr/bin/env bash
# Mounts this experiment, runs each tool below against its own unbuilt
# output, and prints a markdown table of the results. See README.md.
#
# Usage: compat.sh [path/to/makebelieve]
set -u

here=$(cd "$(dirname "$0")" && pwd)
exe=${1:-"$here/../../build/src/makebelieve"}
[ -x "$exe" ] || { echo "makebelieve not found at $exe" >&2; exit 1; }

# A native filesystem: FUSE will not mount over a 9P path such as /mnt/c.
work=$(mktemp -d /tmp/makebelieve-compat.XXXXXX)
mkdir "$work/src" "$work/dest"
cp "$here/build.makebelieve" "$here/input.txt" "$work/src/"
src="$work/src/input.txt"
mnt="$work/mnt"

(cd "$work/src" && exec "$exe" mount "$mnt") > "$work/daemon.log" 2>&1 &
trap '"$exe" unmount "$mnt" > /dev/null 2>&1; rm -rf "$work"' EXIT
for _ in $(seq 100); do [ -e "$mnt/00.txt" ] && break; sleep 0.05; done
[ -e "$mnt/00.txt" ] || { cat "$work/daemon.log" >&2; exit 1; }

# Tool | flags tested | command, where {} is the file under test, {src} the
# source file and {dest} a fresh scratch path. The command's output is compared
# with the same command run on the source file.
tests=(
  'awk|`END { print NR, length($0) }`|awk "END { print NR, length(\$0) }" {}'
  'cat|none|cat {}'
  'cmp|none|cmp {src} {} && echo same'
  'cp|none|cp {} {dest} && cat {dest}'
  'dd|`if=FILE of=DEST status=none`|dd if={} of={dest} status=none && cat {dest}'
  'diff|none|diff {src} {} && echo same'
  'diff|`-q`|diff -q {src} {} && echo same'
  'grep|`-c`|grep -c fox {}'
  'head|`-c 500`|head -c 500 {}'
  'less|`-F -X`|less -F -X {}'
  'md5sum|file on standard input|md5sum < {}'
  'od|`-c`|od -c {} | tail -n 3'
  'python3|`open().read()`|python3 -c "import sys; print(open(sys.argv[1], \"rb\").read()[-40:])" {}'
  'python3|`mmap` (read-only)|python3 -c "import mmap, sys; f = open(sys.argv[1], \"rb\"); print(mmap.mmap(f.fileno(), 0, prot=mmap.PROT_READ)[-40:])" {}'
  'python3|`os.open()` then `os.fstat()`|python3 -c "import os, sys; print(os.fstat(os.open(sys.argv[1], os.O_RDONLY)).st_size)" {}'
  'rsync|none|rsync {} {dest} && cat {dest}'
  'sed|`-n 50,52p`|sed -n 50,52p {}'
  'sha256sum|file on standard input|sha256sum < {}'
  'sort|`-r`|sort -r {} | head -n 3'
  'stat|`-c %s`|stat -c %s {}'
  'tail|`-n 3`|tail -n 3 {}'
  'tail|`-c 200`|tail -c 200 {}'
  'tar|`-cf`, then `-xf`|tar -C $(dirname {}) -cf {dest}.tar $(basename {}) && mkdir {dest} && tar -C {dest} -xf {dest}.tar && cat {dest}/*'
  'wc|`-c -l`, file on standard input|wc -c -l < {}'
  'xxd|none|xxd {} | tail -n 2'
)

# Runs one test's command against one file, with a fresh {dest}.
run() {  # command, file
  local cmd=${1//\{src\}/$src}
  cmd=${cmd//\{dest\}/$(mktemp -u "$work/dest/XXXXXX")}
  cmd=${cmd//\{\}/$2}
  timeout 60 bash -c "$cmd" 2>&1
}

echo '| Tool | Flags tested | Compatible |'
echo '| --- | --- | --- |'
i=0
for test in "${tests[@]}"; do
  IFS='|' read -r tool flags cmd <<< "$test"
  if ! command -v "$tool" > /dev/null; then
    echo "| \`$tool\` | $flags | Not installed |"
    continue
  fi
  output=$(printf '%s/%02d.txt' "$mnt" "$i")
  i=$((i + 1))
  if [ "$(run "$cmd" "$output")" == "$(run "$cmd" "$src")" ]; then
    result=Yes
  else
    result=No
  fi
  echo "| \`$tool\` | $flags | $result |"
done
