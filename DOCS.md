# makebelieve Manifest

A manifest describes generated files and the commands used to produce them.

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
quotes (`"@/my notes.md"`) it runs to the closing quote. Whatever a
command looks at through the mount - named with `@/` or not - becomes one
of its inputs, as anything it looks at in the source does (see
[Inputs](#inputs)): it is rebuilt whenever an output it opened goes out of
date, an output it found missing is declared, a directory of outputs it
listed gains or loses one, or a directory of outputs it looked at goes.

An output is assigned an action and what that action applies to. With
`run` the command writes the output itself, to the path `%out` stands
for. That path is a scratch file with the same name as the output, in a
directory of its own under the build directory's `.makebelieve` (see
[Resuming](#resuming)), so a tool that picks its format from the extension
needs no extra argument to say so:

```mb
@/paper.pdf = run pandoc paper.md -o %out
```

With `capture` the command writes nothing, and the bytes it sends
to standard output are the output. Its standard output is that scratch
file rather than a pipe, as if run with `> %out`:

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
to standard error (or, for a `run` command, standard output) - is in a
`status` output (see below), and on **makebelieve**'s standard error
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

`status` takes no argument either: the output is what **makebelieve**
is doing and what went wrong, so whoever reads the outputs - an AI
agent, say, with no view of the terminal running the mount - can tell
whether to wait, and why something failed. It is empty once everything
asked for is built and nothing is failing, so an empty `status` output
is the check that everything built.

```mb
@/status.txt = status
```

It holds, each part in path order:

- the problems that kept the latest edit to the manifest from being
  applied, one per line, as the mount reports them;
- each path that more than one rule produces, with each of those rules
  and what it matched, indented (see
  [Directory Expansion](#directory-expansion));
- each build under way, as `building`, or `waiting to build` while it
  waits its turn for a place to run, with its command - and
  `(its last build failed)` after one being retried;
- each output whose most recent build failed: a line naming the output,
  why it failed - its command's exit status, or the error that stopped
  it - and its command, followed by whatever the command wrote to
  standard error, indented: its first 16 KiB, and a line saying how much
  more there was. One that read an output that is failing still names
  that output beside its own reason, as its command may have failed for
  it - or for something else. A `copy` of an output that is failing gets
  a single line naming that output instead, as that is its only reason.

```text
build.makebelieve:4: expected `@/<output> = <action> <command>`
@/site/about.html waiting to build: run pandoc about.md -o %out
@/site/style.css building: run sass style.scss %out (its last build failed)
@/site/index.html failed because @/site/index.md failed
@/site/sitemap.xml failed (exit 1; it read @/site/index.md, which failed): capture ./sitemap.sh
    sitemap.sh: cannot open @/site/index.md
@/site/index.md failed (exit 1): run pandoc --from markdown index.txt -o %out
    pandoc: index.txt: withBinaryFile: does not exist (No such file or directory)
```

A build leaves the report once it finishes, a failure once the output
next builds or its rule leaves the manifest, and a conflict once only one
rule produces the path; the manifest's
problems leave it once an edit to it is applied. While anything is
building or waiting, the report will change again, so read it again
later for the answer. Reading it builds nothing.

What the report shows changes as builds start and finish, so it is made
when read. A tool that rereads a file when told it changed hears once,
the first time anything changes after it read the report, and then
nothing until it reads it again - however many builds come and go
meanwhile - so it can reread as often as suits it. Between reads, the
size it reports before being opened may be out of date. A command that
reads a `status` output gets what it showed then, and its output is not
rebuilt as the report changes.

## Inputs

Every command runs traced, and everything it looks at in the manifest's
directory becomes one of its inputs - whatever tool it runs, and whatever
that starts in turn:

- each file it reads, or only looks at the size or modification time of;
- each directory it lists, looks at the attributes of, or passes through on
  the way to something beneath it; and
- each path it looks for and finds nothing at.

An output is rebuilt whenever what it saw of an input changes:

- a file's content, which its modification time and size stand in for, so
  touching a file rebuilds what read it, even if its content is the same;
- a directory's entries, if it listed it: the names of the files and
  directories directly in it, so adding, removing or renaming one rebuilds
  what listed it, but writing a file in it, or touching the directory
  itself, does not;
- a directory going, or becoming a file, if it only looked at it - which is
  all that looking at a directory without listing it shows;
- a path found missing appearing - as a file or a directory. A directory
  appearing also rebuilds whatever found a path beneath it missing, even if
  that path is still missing, as it may have arrived full.

On Windows, a listing also shows each file's size and modification time.
They are not part of a directory's value there either, so a tool that
decides what is out of date from a listing alone, without looking at the
files, is not rebuilt when one of them is rewritten.

Commands are expected to run and exit. A long-lived service that learns of
changes without looking at the files again - a file watcher or a compile
server, say - is not supported, as what it learns is not traced.

## Rules

Reusable commands can be declared as rules: a name prefixed with `!`,
assigned an action - `run`, `capture` or `copy` - and its argument.

```mb
!cc = run clang++ $flags -c %in -o %out
    flags = -O2
```

An output below the rule uses it by name, in place of an action:

```mb
@/foo.o = !cc foo.cpp
@/bar.o = !cc bar.cpp
    flags = -O0 -g
```

`%in` in the rule becomes whatever follows the rule's name, and each
`$variable` its value, so these are `run clang++ -O2 -c foo.cpp -o %out`
and `run clang++ -O0 -g -c bar.cpp -o %out`. From there the output is
treated as if it had been written out in full, so an output with a
capture takes its sources from the expanded command (see
[Directory Expansion](#directory-expansion)):

```mb
@/out/$name.o = !cc src/*{name}.cpp
```

The indented `variable = value` lines under a rule give its variables
default values, and those under an output set them for that output. A
variable with no default must be set, or captured (see
[Directory Expansion](#directory-expansion)), by every output using the rule. A
value runs to the end of its line and may be empty. Setting a variable
that neither the rule nor the output's own line names is an error, as is
giving something for `%in` to a rule without one, or nothing to a rule
with one.

An output that uses no rule may set variables too, for its own line:

```mb
@/$lang/index.html = run pandoc -M lang=$lang index.md -o %out
    lang = en
```

A variable is used as `$name`, or as `${name}` where what follows would
otherwise run on into the name, as in `${name}_draft`. A name is
letters, digits and `_`, not starting with a digit. `$$` is a single
`$`, and a `$` before anything else that cannot start a name, as in
`$(date)` or `$1`, is left for the shell. What a variable or `%in` is
replaced with is not expanded again, and a value in a path is taken as
it is, `/` included.

In a rule, every `$name` and `${name}` is one of its variables, so a
shell `${HOME}` in a rule is written `$${HOME}`. An output's own line -
its path, and its command or the argument it gives a rule - expands only
the variables it sets and the names it captures, before any rule is
expanded. Anything else on it is the shell's: `$HOME` stays `$HOME`, and
an output that sets and captures nothing expands nothing, `$$` included.
A rule's defaults are for its body alone, so naming one on the line of
an output that does not set it is an error.

A rule must be declared above the outputs that use it, and only once.

Without the `!`, the right-hand side is a built-in action (`run`,
`capture`, `copy`, `tracing` or `status`) and its argument, if it takes
one.

Lines starting with `#` are comments. Any number of spaces or tabs at
the start of a line, at least one, indents it. Only variables are
indented: every other line starts at the left margin.

## Directory Expansion

A capture in a command creates one output per matching source file.
`*{name}` captures part of each matching file's path, and the output
uses what it captured as the variable `$name`:

```mb
@/out/$name.o = run clang++ -c src/*{name}.cpp -o %out
@/site/$name.md = copy notes/*{name}.md
```

The source files are those matching the source pattern: the first word
with a capture in it, once any rule is expanded - for `copy`, the path.
With `src/main.cpp` present the first rule declares `@/out/main.o`,
built by `run clang++ -c src/main.cpp -o %out`. Outputs, and the
directories they are in, appear and disappear as matching files do.

Captures can take a path apart and lay it out differently:

```mb
@/$year/$month/$day/$name.html = run pandoc posts/*{year}-*{month}-*{day}-*{name}.md -o %out
```

With `posts/2024-01-02-my-post.md` present, this declares
`@/2024/01/02/my-post.html`.

The source pattern is an unquoted path inside the manifest's directory,
and captures go nowhere else: a capture elsewhere is an error. Each name
is captured once and used as `$name` (or `${name}`) wherever its value
is wanted - the output's path, its command, or the body of the rule it
uses:

```mb
!post = run pandoc -M date=$year-$month-$day %in -o %out

@/$year/$month/$day/$name.html = !post posts/*{year}-*{month}-*{day}-*{name}.md
```

A capture sets its variable for that file, so it gives a rule's variable
with no default its value, and an output may not also set a variable it
captures. Every capture must be used in the output's path, so no two
files make the same output, and every `$name` in the path must be one
the output captures or sets. What a capture matched goes into the
command as it is, so a file name with spaces or shell metacharacters in
it needs the command to quote where it goes.

A `*` that is not a capture - one not followed by `{`, a name and then
`}` or `:`, as in `-I inc/*` or `cp *{.c,.h} dir` - is left for the
shell.

### Matching

`*{name}` matches one or more characters, never `/`. `**{name}`
matches one or more characters, `/` included, so a pattern can reach
into subdirectories:

```mb
@/$path.html = run pandoc content/**{path}.md -o %out
@/$dir/index/$name.html = copy content/**{dir}/*{name}.md
```

The first lays out `content/blog/2024/intro.md` as
`@/blog/2024/intro.html`. A pattern with a `**` capture, or a capture
before its last `/`, lists every directory beneath the part before its
first capture, so a file added anywhere beneath `content/` declares its
output.

Where a path can be split more than one way, each capture takes as
little as it can, from the left. In `2024-01-02-my-post.md`, `year`,
`month` and `day` take one piece each and `name` takes `my-post`. In
`content/a/b/c.md`, the second pattern above gives `dir` as `a/b` and
`name` as `c`, as `name` cannot hold a `/`.

`*{name:regex}` and `**{name:regex}` capture only text the regular
expression matches in full. It is an ECMAScript regular expression, as
in JavaScript, and may hold balanced braces. A file that no split
matches is skipped:

```mb
@/$year/$month/$day/$name.html = run pandoc posts/*{year:\d{4}}-*{month:\d\d}-*{day:\d\d}-*{name}.md -o %out
```

Without them, `posts/about-us-page-v2.md` would become
`@/about/us/page/v2.html`; with them it declares nothing. A
`*{name:regex}` still never holds a `/`, whatever the expression allows.

Precisely, a path is matched as by the first match of one regular
expression built from the pattern, left to right: its text matched
literally, `*{name}` as `([^/]+?)`, `**{name}` as `(.+?)`, and a
constraint as its own expression in a group. An expression's own
quantifiers decide how much it takes, so `**{dir:.+}` takes as much as
it can.

A path is an output only if a file maps to it. Opening
`@/2024/01/02-x/post.html` finds nothing, even with
`posts/2024-01-02-x-post.md` present, as that file is
`@/2024/01/02/x-post.html`.

### Outputs as Sources

A source pattern under `@/` matches other outputs instead, whichever rule
declares them:

```mb
@/gen/$name.md = capture python render.py src/*{name}.txt
@/site/$name.html = run pandoc @/gen/*{name}.md -o %out
```

Generated and hand-written files can share a directory, and rules may
overlap:

```mb
@/pkg/$name.py = copy pkg/*{name}.py
@/pkg/${name}_pb2.py = run ./gen_pb2.sh pkg/*{name}.proto %out
```

A path that two rules actually produce, given the files that exist, is a
conflict. It still appears, but nothing builds it and opening it fails
with an I/O error (`EIO`, or `STATUS_IO_DEVICE_ERROR` on Windows), like a
failed build. No rule takes precedence - not even a rule naming the path
outright over a pattern - and every other path is unaffected. A
`status` output (see [Generated Files](#generated-files)) names the path,
both rules and the files they matched:

```text
@/site/index.html is claimed by more than one rule:
    build.makebelieve:1: @/site/$name.html = run pandoc src/*{name}.md -o %out (matching src/index.md)
    build.makebelieve:2: @/site/index.html = run ./gen.sh
```

Conflicts come and go with the files that cause them. With the two rules
above, `@/site/index.html` builds until `src/index.md` is created, fails
while it exists, and builds again once it is removed. Whatever read the
path is rebuilt when it becomes a conflict or stops being one, as it is
for any other change. A rule that would make a file of a directory that
another rule's output is in, such as `@/out/$name.foo` making
`@/out/x.foo` beside `@/out/x.foo/y`, conflicts with every output in
that directory.

The manifest itself is still rejected when two rules name the same path
outright, or when a rule names a path inside another rule's output.

Nor may a pattern over outputs be able to produce something it matches
itself, directly or through other patterns, since its outputs would
never end - `@/gen/$name.md.md = copy @/gen/*{name}.md`. This is
decided from the patterns' shapes alone, each capture standing for any
text it could hold, so a pattern is rejected even if its captures'
expressions would keep it from matching what it produces.

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

A failed build's standard error - and a `run` command's standard output,
which is not its output but may say why it failed - goes to the terminal
running the mount, under a line naming its output, so builds failing at
once do not interleave. What a build that succeeds writes there is
dropped:

```text
makebelieve: @/index.md failed (exit 1): run pandoc --from markdown index.txt -o %out
    pandoc: index.txt: withBinaryFile: does not exist (No such file or directory)
```

So does each path that comes to be claimed by more than one rule (see
[Directory Expansion](#directory-expansion)), as `status` shows it, and
again if the rules claiming it change:

```text
makebelieve: @/site/index.html is claimed by more than one rule:
    build.makebelieve:1: @/site/$name.html = run pandoc src/*{name}.md -o %out (matching src/index.md)
    build.makebelieve:2: @/site/index.html = run ./gen.sh
```

From anywhere else, a `status` output holds the same report for every
output that is failing now, along with what is still building.

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
change, whether or not anything has read it. `tracing` and `status`
outputs are the exception, as nobody builds one. Giving `--build`
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

## Dependencies

`deps` prints what an output was last built from, as a tree, in the style of
`cargo tree`. It reads what is kept in the build directory, so it works
whether or not that directory is mounted, and never builds anything:

```text
$ makebelieve deps ../output @/site/index.html
@/site/index.html
├── @/site/index.md
│   └── index.txt
└── template.html
```

Outputs are named `@/path`, and anything else by its path from the manifest's
directory. Only what has been built is known: an output that has not been
built since it last went out of date has no dependencies to print, and is an
error to ask about.

- `-i`, `--invert`: print what depends on each path instead, which may be a
  source file, a directory, a missing path or an output.
- `--depth <n>`: go at most `n` steps, so `--depth 1` is direct dependencies.
- `--prefix <style>`: `indent`, the default, draws the tree; `depth` puts each
  path's distance before it; `none` prints a sorted list of every path once,
  for scripts.
- `--charset <charset>`: `utf8`, the default, draws with box-drawing
  characters; `ascii` uses `|`, `-` and a backtick, for terminals or pipes that
  mangle them.
- `--no-dedupe`: repeat what is beneath a path each time it appears. Otherwise
  it is printed once, and later appearances are marked `(*)`.
- `--all`: include directories that were only looked at, and paths found
  missing, marked `(directory)` and `(missing)`. A directory that was listed
  is always included, marked `(listed)`.

```sh
makebelieve deps --prefix none ../output @/site/index.html  # every input
makebelieve deps -i ../output index.txt                     # what it affects
```

## Resuming

Every successful build is kept in the build directory's `.makebelieve`,
beside its `mount` - `makebelieve mount ../output` keeps builds in
`../output/.makebelieve`. With each output it keeps the command that built
it and what each input looked like: a source file's modification time and
size, a directory's entries if it was listed, or that one was there if it
was only looked at, that nothing was there, or the content of an output
read through the mount. The
mount serves each output from the file kept there, rather than holding it
in memory, so outputs larger than memory are fine; a reader that opened an
output goes on reading the build it opened even once a rebuild replaces
it.

`.makebelieve` is hidden (on Windows it also has the hidden attribute) and
holds a `.gitignore` that ignores everything in it, so git never sees it,
and a [`CACHEDIR.TAG`](https://bford.info/cachedir/), so backup tools that
honour it skip it. Deleting it is always safe: it only costs rebuilding.

When `mount` starts, an output is served as already built, without running
anything, if

- its rule is still in the manifest, with the same command;
- every source file it looked at has the same modification time and size,
  every directory it looked at has the same entries, and every path it
  found missing is still missing; and
- every output it read is itself served as already built, with the content
  it read, and every output path it looked at is still what it was: a
  directory of the same outputs, or not declared.

Every other output is built on its next open, as usual, and its old build is
discarded, as is the build of any output no longer in the manifest. The
same goes for an output that went out of date while served but was not
rebuilt before **makebelieve** stopped - however it stopped, a crash
included. A `tracing` or `status` output is never kept.

Two mounts can never share one of these directories: mounting where another
running **makebelieve** keeps its builds fails. Deleting the directory while
nothing is mounted is always safe, and just means building everything afresh.

Files outside the manifest's directory - compilers, system headers - are not
tracked, here as anywhere else, so a toolchain upgrade does not rebuild
anything kept; delete the directory to start afresh.
