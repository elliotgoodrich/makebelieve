# Basic example

A minimal [`build.makebelieve`](build.makebelieve) with two rules: one writes a
file from scratch, the other copies [`input.txt`](input.txt). The commands use
`cmake -E` (CMake's cross-platform command shim), so the same manifest works
under either shell - `cmake` just needs to be on `PATH`.

`makebelieve <mountpoint>` reads the manifest from the **current directory** and
projects the built outputs at `<mountpoint>` (a separate, empty directory that
must not overlap the source). Outputs are built lazily: a file reads back as a
single null byte until the first real read runs its command.
