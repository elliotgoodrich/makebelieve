# Basic example

A minimal [`build.makebelieve`](build.makebelieve) showing each action: `run`
writes a file from scratch, `capture` takes a command's standard output, and
`copy` hands back [`input.txt`](input.txt) with no command at all. The `run`
and `capture` commands use `cmake -E` (CMake's cross-platform command shim), so
they work under either shell - `cmake` just needs to be on `PATH`. `copy` needs
nothing.

`makebelieve <mountpoint>` reads the manifest from the **current directory** and
projects the built outputs at `<mountpoint>` (a separate, empty directory that
must not overlap the source). Outputs are built lazily: a file reads back as a
single null byte until the first real read runs its command.
