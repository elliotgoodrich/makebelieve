# makebelieve Manifest

A manifest describes generated files and the commands used to produce them.

## Variables

User variables use `$`; **makebelieve** built-ins use `%`:

```mb
$cc = clang++
$flags = -O2
```

## Generated Files

Generated files live in the mounted output namespace and use `@/`:

```mb
@/output.txt = run cp input.txt %out
```

Paths without `@/` refer to the normal filesystem.

A command reads another output by naming it with `@/` too. Before the
command runs, each `@/<path>` in it becomes that output's path through the
mount, which builds it on the way:

```mb
@/paper.pdf = run pandoc @/paper.md -o %out
@/paper.md = capture python render.py
```

Outside quotes the path runs to the next space or shell metacharacter and
is quoted, so a mountpoint with spaces in it survives the shell; inside
quotes (`"@/my notes.md"`) it runs to the closing quote. Every output a
command opens through the mount - named with `@/` or not - becomes one of
its inputs, so it is rebuilt whenever that output goes out of date.

An output is assigned an action and what that action applies to. With
`run` the command writes the output itself, to the path `%out` stands
for. That path is a scratch file with the same name as the output, so
a tool that picks its format from the extension needs no extra argument
to say so:

```mb
@/paper.pdf = run pandoc paper.md -o %out
```

With `capture` the command writes nothing, and the bytes it sends
to standard output are the output:

```mb
@/version.txt = capture git describe --tags
```

`copy` takes a path rather than a command: the output is the bytes of
that file, read without running anything.

```mb
@/config.json = copy etc/config.json
```

The path is relative to the manifest's directory and must stay inside
it, so the file is always one **makebelieve** can watch - a copy is
rebuilt when its source changes even where command tracing is
unavailable.

A path under `@/` copies another output, which must be declared in the
manifest. It is built first, and whenever it goes out of date, so does
the copy:

```mb
@/latest.json = copy @/releases/v2.json
```

`tracing` takes no argument: the output is a trace of what
**makebelieve** itself has been doing - every file opened through the
mount, every build with its command and how long it took, and an arrow
from the open that asked for a build to the build itself. Each reading
process gets a group of its own, named after its executable, and
**makebelieve** another for its own threads and builds. Within a group,
work goes on rows (`reader`, `build`) that are reused as it finishes, so
a group holds as many rows as it ever had work running at once. It is in
the
[Trace Event Format](https://docs.google.com/document/d/1CvAClvFfyA5R-PhYUmn5OOQtYMH4h6I0nSsKchNAySU/preview),
ready to open in [Perfetto](https://ui.perfetto.dev) or
`chrome://tracing`.

```mb
@/tracing.json = tracing
```

Recording runs from the moment the mount starts, whether or not a
`tracing` rule exists, keeping the most recent 64 MiB of events. Every
open of the output takes a fresh snapshot. That rewrite is not
announced as a change, so a tool that rereads files when told they
changed does not reread the trace forever.

## Rules

Reusable commands can be declared as rules:

```mb
!cc = clang++ $flags -c %in -o %out
    flags = -O2
```

Invoke them by name, prefixed with `!`:

```mb
@/foo.o = !cc foo.cpp
    flags = -O0 -g
```

Without the `!`, the right-hand side is a built-in action (`run`,
`capture`, `copy` or `tracing`) and its argument, if it takes one.

## Directory Expansion

A `*` in an output creates one output per matching source file:

```mb
@/out/*.o = run clang++ -c src/*.cpp -o %out
@/site/*.md = copy notes/*.md
```

The source files are those matching the first word of the command with a
`*` in it - for `copy`, the path. With `src/main.cpp` present the first
rule declares `@/out/main.o`, built by that command with every `*`
replaced by `main`. Outputs appear and disappear as matching files do.

Each pattern takes a single `*`, in the file name, which matches one or
more characters. The source pattern is an unquoted path inside the
manifest's directory. What the `*` matched goes into the command as it
is, so a file name with spaces or shell metacharacters in it needs the
command to quote the `*`.

Without a `*` in the output, a `*` in a command is left for the shell.

A source pattern under `@/` matches other outputs instead, whichever rule
declares them:

```mb
@/gen/*.md = capture python render.py src/*.txt
@/site/*.html = run pandoc @/gen/*.md -o %out
```

Two rules may never be able to produce the same path, whatever source
files come to exist. So a wildcard bans its suffix - whatever follows the
`*` - from its directory: with `@/out/*.foo` declared, nothing else in
`@/out` may end in `.foo`. The manifest is rejected when it holds, beside
that wildcard:

- an output ending in the suffix - `@/out/a.foo`;
- another wildcard whose suffix ends in it, or that it ends in -
  `@/out/lib*.foo`, `@/out/*.x.foo` or `@/out/*o`;
- an output inside a directory ending in it - `@/out/x.foo/y`.

Nor may a wildcard over outputs be able to produce something its own
pattern matches, directly or through other wildcards, since its outputs
would never end - `@/gen/*.md.md = copy @/gen/*.md`.

## Serving

The manifest in the current directory is served at a mountpoint with:

```sh
makebelieve mount <mountpoint>
```

This runs in the foreground and serves the outputs until stopped, either
by Ctrl+C or, from another terminal, by:

```sh
makebelieve unmount <mountpoint>
```

`unmount` signals the instance serving that mountpoint to shut down and
blocks until it has fully torn down.

By default, at most one build per core holds a place to run at a time;
`-j <builds>` sets another limit:

```sh
makebelieve mount -j 2 <mountpoint>
```

Options may also follow the mountpoint: `makebelieve mount <mountpoint> -j 2`.

Opening an output always builds it if it is not up to date, and waits.
`--build=<mode>` sets which outputs are kept built without being asked:

- `requested`, the default: those that have been opened. Nothing is built
  until it is first opened; after that it is rebuilt whenever one of its
  inputs changes.
- `all`: every output, from the moment the mount is up.

```sh
makebelieve mount --build=all <mountpoint>
```

With `--build=all`, every output is treated as already opened: outputs
added later - by an edit to the manifest, or a new file a wildcard matches -
are built as they appear, and an output is rebuilt whenever its inputs
change, whether or not anything has read it. A `tracing` output is the
exception, since every open takes a fresh trace anyway. Giving `--build`
more than once is an error, even with the same mode.

Builds that something is waiting for go first. Opening an output whose build
has yet to start moves it ahead of every build nobody is waiting for - those
of `--build=all`, or rebuilds started only because an input changed - and so
does every output that build then reads, however deep. A build that already
holds a place keeps it, so an open can still wait for one of those to finish.

A build's command may itself read other outputs through the mount - a PDF
built from generated Markdown, say. While it waits for one to build, its
build gives up its place, so a chain of such outputs longer than the limit
still completes. Its other threads and processes keep running meanwhile, so
`-j` limits how many builds hold a place, not how many processes are
runnable or how much CPU they use; a command that never reads another output
keeps its place until it finishes.

Opening an output that would wait on a cycle of builds - including a command
reading its own output - fails with `EDEADLK`, and every build on the cycle
fails. Pending opens suspend as coroutines and consume no dispatcher
thread, so there is no 64-open limit. Linux uses low-level FUSE replies;
Windows defers WinFsp transactions. Unmount cancels pending requests.

## Resuming

Every successful build is kept in a directory beside the mountpoint, named
after it with `.makebelieve` added - mounting at `../output` keeps builds in
`../output.makebelieve`. With each output it keeps the command that built it
and what each input it read looked like: a source file's modification time
and size, or the content of an output read through the mount.

When `mount` starts, an output is served as already built, without running
anything, if

- its rule is still in the manifest, with the same command;
- every source file it read has the same modification time and size; and
- every output it read is itself served as already built, with the content
  it read.

Every other output is built on its next open, as usual, and its old build is
discarded, as is the build of any output no longer in the manifest. The
same goes for an output that went out of date while served but was not
rebuilt before **makebelieve** stopped - however it stopped, a crash
included. A `tracing` output is never kept.

Two mounts can never share one of these directories: mounting where another
running **makebelieve** keeps its builds fails. Deleting the directory while
nothing is mounted is always safe, and just means building everything afresh.

Files outside the manifest's directory - compilers, system headers - are not
tracked, here as anywhere else, so a toolchain upgrade does not rebuild
anything kept; delete the directory to start afresh.
