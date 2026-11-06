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

A build that fails makes opening its output fail with an I/O error (`EIO`
on Linux, `STATUS_IO_DEVICE_ERROR` on Windows) - never with
`ENOENT`, which would suggest the output does not exist. That covers a
command exiting nonzero, a `run` command that exits successfully but
writes nothing to `%out`, and a `copy` whose source cannot be read. Why
it failed - the reason for the last two, and whatever the command wrote
to standard error (or, for a `run` command, standard output) - is in an
`errors` output (see below), and on **makebelieve**'s standard error
under a line naming the output. A failed output is retried the next
time it is opened, or when a file its build read changes - the fix for
what it failed on, say, or the missing source of a `copy` appearing - just
as a built output is rebuilt.

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

`errors` takes no argument either: the output is what is failing right
now, so whoever reads the outputs - an AI agent, say, with no view of
the terminal running the mount - can find out why. It is empty while
nothing is, so an empty `errors` output is the check that everything
built.

```mb
@/errors.txt = errors
```

It holds one entry per output whose most recent build failed, in path
order: a line naming the output, why it failed - its command's exit
status, or the error that stopped it - and its command, followed by
whatever the command wrote to standard error, indented. An output that
failed only because an output it read had failed gets a single line
naming that output instead. Before them come the problems that kept the
latest edit to the manifest from being applied, one per line, as the
mount reports them.

```text
build.makebelieve:4: expected `@/<output> = <action> <command>`
@/site/index.html failed because @/site/index.md failed
@/site/index.md failed (exit 1): run pandoc --from markdown index.txt -o %out
    pandoc: index.txt: withBinaryFile: does not exist (No such file or directory)
```

An output leaves the report once it next builds, or its rule leaves the
manifest, and the manifest's problems once an edit to it is applied.
Reading the report builds nothing - an output is retried when it is
next opened, or when its inputs change - and it is always current, even
to a tool that only looks up its size; it is rewritten, and that change
announced, only when what is failing changes.

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
`capture`, `copy`, `tracing` or `errors`) and its argument, if it takes
one.

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

The manifest in the current directory is served from a build directory
with:

```sh
makebelieve mount <directory>
```

The outputs are served at `<directory>/mount`, and what has been built is
kept in the hidden `<directory>/.makebelieve` (see [Resuming](#resuming)).
`<directory>` must not overlap the source directory.

This runs in the foreground and serves the outputs until stopped, either
by Ctrl+C or, from another terminal, by:

```sh
makebelieve unmount <directory>
```

`unmount` signals the instance serving from that directory to shut down
and blocks until it has fully torn down.

A build's standard error - and a `run` command's standard output, which
is not its output but may say why it failed - goes to the terminal
running the mount, each build's under a line naming its output, so
builds running at once do not interleave:

```text
makebelieve: @/index.md failed (exit 1): run pandoc --from markdown index.txt -o %out
    pandoc: index.txt: withBinaryFile: does not exist (No such file or directory)
```

From anywhere else, an `errors` output holds the same report for every
output that is failing now.

By default, at most one build per core holds a place to run at a time;
`-j <builds>` sets another limit:

```sh
makebelieve mount -j 2 <directory>
```

Options may also follow the directory: `makebelieve mount <directory> -j 2`.

Opening an output always builds it if it is not up to date, and waits.
`--build=<mode>` sets which outputs are kept built without being asked:

- `requested`, the default: those that have been opened. Nothing is built
  until it is first opened; after that it is rebuilt whenever one of its
  inputs changes.
- `all`: every output, from the moment the mount is up.

```sh
makebelieve mount --build=all <directory>
```

With `--build=all`, every output is treated as already opened: outputs
added later - by an edit to the manifest, or a new file a wildcard matches -
are built as they appear, and an output is rebuilt whenever its inputs
change, whether or not anything has read it. `tracing` and `errors`
outputs are the exception, as each is always as fresh as opening it
makes it. Giving `--build`
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

Every successful build is kept in the build directory's `.makebelieve`,
beside its `mount` - `makebelieve mount ../output` keeps builds in
`../output/.makebelieve`. With each output it keeps the command that built
it and what each input it read looked like: a source file's modification
time and size, or the content of an output read through the mount.

`.makebelieve` is hidden (on Windows it also has the hidden attribute) and
holds a `.gitignore` that ignores everything in it, so git never sees it,
and a [`CACHEDIR.TAG`](https://bford.info/cachedir/), so backup tools that
honour it skip it. Deleting it is always safe: it only costs rebuilding.

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
included. A `tracing` or `errors` output is never kept.

Two mounts can never share one of these directories: mounting where another
running **makebelieve** keeps its builds fails. Deleting the directory while
nothing is mounted is always safe, and just means building everything afresh.

Files outside the manifest's directory - compilers, system headers - are not
tracked, here as anywhere else, so a toolchain upgrade does not rebuild
anything kept; delete the directory to start afresh.
