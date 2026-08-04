# FUSE Tracing Experiment

`fuse_tracing_experiment <command> [args...]` runs `<command>` with a FUSE
filesystem mounted directly on top of the current directory, and prints
every file under the current directory that the command opened for
reading. This is the same technique
[Tup](https://gittup.org/tup/ex_a_first_tupfile.html) uses to discover a
build command's real dependencies without requiring them to be declared up
front.

## How it works

1. Fork a child process into a fresh user + mount namespace
   (`unshare(CLONE_NEWUSER | CLONE_NEWNS)`), so its mount changes are
   invisible to everyone else.
2. The child opens `/dev/fuse` and mounts a passthrough filesystem
   directly *on top of* the current directory (not on some ancestor of
   it), then execs `<command>`.
3. The parent services that mount on a background thread, forwarding
   every `open()` through to the real underlying file (via
   `/proc/self/fd/N` reopen tricks) and recording its path.
4. Once `<command>` exits, its mount namespace is torn down by the
   kernel, the servicing thread's read loop ends, and the parent prints
   the deduplicated, sorted list of files that were opened for reading.
