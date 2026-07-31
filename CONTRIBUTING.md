# Contributing

## Development environment setup

### FUSE build dependencies

`experiments/FUSE` requires the FUSE 3 headers and `pkg-config` (used to
locate `libfuse3` compiler/linker flags). These are not installed by
default:

```sh
sudo apt install -y libfuse3-dev pkg-config
```

Verify the install:

```sh
pkg-config --modversion fuse3
```

#### WSL2 notes

If you're developing under WSL2, keep the FUSE mount point and the repo
you build from on the native Linux filesystem (e.g. under `/home/...`),
not under a Windows-backed `/mnt/c/...` 9p mount. FUSE mounts on top of
9p paths are unreliable. `/dev/fuse` and kernel FUSE support are present
on modern WSL2 kernels by default, so no kernel-side setup is normally
required; only the userspace packages above.

The reverse direction - accessing a FUSE mount *from* Windows, e.g. via
VS Code or `\\wsl.localhost\...` - needs `-o allow_other` on the mount,
which itself needs `user_allow_other` uncommented in `/etc/fuse.conf`
first (`sudo sed -i 's/^#user_allow_other/user_allow_other/'
/etc/fuse.conf`). Without it, WSL's `\\wsl$`/`\\wsl.localhost` bridge -
which runs as a separate process from the one that mounted the
filesystem - hits the same "not the mounting user" access denial any
other unrelated process would (see
[microsoft/WSL#8498](https://github.com/microsoft/WSL/issues/8498)).
Even with `allow_other`, open VS Code by naming the mount from its parent
directory (`cd /tmp && code fuse-mnt`), not by `cd`-ing into the mount and
running `code .` there - the latter still fails intermittently, most
likely because the `code` wrapper's own current-directory resolution has
to round-trip through the same fragile path.

### ProjFS / ETW build dependencies

No Windows experiment has been written yet. When `experiments/ProjFS` or
`experiments/ETW` show up, this section should grow the equivalent Windows
10 SDK / optional-feature setup notes that were tracked in heinzel's
CONTRIBUTING.md.

## Building

```sh
cmake -S . -B build
cmake --build build
```

Each experiment produces its own executable under `build/experiments/<name>/`.

Pass `-DENABLE_CLANG_TIDY=ON` to run clang-tidy as part of the build.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

`experiments/FUSE` has one black-box test, `fuse_experiment_smoke`
(`experiments/FUSE/smoke_test.sh`), that mounts the real binary and checks
against the actual kernel FUSE path - not the source directly - that
`time.txt`'s content changes between reads, that a read is followed by a
real `inotify` `IN_MODIFY` event, and that writes from anyone but the
daemon itself are rejected. It self-skips (rather than failing) if
`/dev/fuse` isn't usable, e.g. on a sandboxed CI runner.

## Trying the FUSE experiment

```sh
mkdir -p /tmp/fuse-mnt
build/experiments/FUSE/fuse_experiment /tmp/fuse-mnt &

inotifywait -m /tmp/fuse-mnt/time.txt &
cat /tmp/fuse-mnt/time.txt   # -> e.g. "hi, it is 2026-07-29 17:06:27 UTC"

fusermount3 -u /tmp/fuse-mnt
```

The content is regenerated on every `read()`, so a plain poller (re-`open`/
re-`cat`/re-`stat`) always sees the current second. `inotify` works too:
the first `read()` schedules a background thread that performs a real
`write()` against the mount at the next whole-second boundary, purely so
the kernel fires
`IN_MODIFY` for the `inotifywait` above - the daemon recognizes and
discards that specific write (see the comment above `is_self_request` in
`fuse.m.cpp`) rather than treating it as new content. Nobody else can
write to the file - try `echo x > /tmp/fuse-mnt/time.txt`, it fails with
`Permission denied`. If nothing reads the file, no write gets scheduled
and the heartbeat stops on its own.
