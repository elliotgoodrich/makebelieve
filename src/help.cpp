// SPDX-License-Identifier: MIT
#include "help.hpp"

#include <string_view>

// Set by src/CMakeLists.txt from the project version and git commit.
#ifndef MAKEBELIEVE_VERSION
#define MAKEBELIEVE_VERSION "unknown"
#endif

namespace makebelieve::help {

std::string_view overview() {
  return R"(makebelieve - a demand-driven build system exposed as a filesystem

makebelieve reads build.makebelieve in the current directory and serves the
outputs it declares as files under a mountpoint. Nothing is built until a file
is opened: opening it runs its command, waits for it, and returns the result.
Every file the command reads is recorded, and the output is rebuilt when one
changes. Builds are kept beside the mountpoint, so the next mount serves
unchanged outputs at once.

usage:
  makebelieve mount [-j <builds>] [--build=requested|all] <mountpoint>
      Serve the outputs at <mountpoint>, which must not exist yet, in the
      foreground until Ctrl+C or `makebelieve unmount`. See
      `makebelieve mount --help`.
  makebelieve unmount <mountpoint>
      Stop the makebelieve serving <mountpoint>, waiting until it has gone.
  makebelieve help manifest
      How to write build.makebelieve, with an example.
  makebelieve help [mount|unmount], makebelieve <command> --help
      Help for one command.
  makebelieve --help, -h, help
      Show this help.
  makebelieve --version
      Show the version.

Start with `makebelieve help manifest`.
)";
}

std::string_view mount() {
  return R"(usage: makebelieve mount [-j <builds>] [--build=requested|all] <mountpoint>

Serves the outputs declared by build.makebelieve in the current directory as
files under <mountpoint>, which must be outside the current directory and must
not exist yet: makebelieve creates it, and removes it on unmount. Runs in the
foreground until Ctrl+C, or until `makebelieve unmount <mountpoint>` from
another terminal; to script it, start it in the background and wait for an
output to appear under <mountpoint>.

Opening an output builds it if it is out of date, and waits. Listing or
stat-ing never waits: an output never built reports a size of 1. A failed build
fails the open with an I/O error; build commands' standard error, and problems
with build.makebelieve, are printed on this command's standard error. Edits to
build.makebelieve take effect while mounted; one that does not parse is ignored
until fixed.

Builds are kept in <mountpoint>.makebelieve, beside the mountpoint, and the
next mount serves every output whose command and inputs are unchanged without
rebuilding it. Deleting that directory while unmounted is always safe.

options (they may also follow <mountpoint>):
  -j <builds>        At most this many builds run at once (default: one per
                     core). A build waiting on another output it reads gives
                     its place up meanwhile.
  --build=requested  Build an output only once it is opened, then rebuild it
                     whenever an input changes. The default.
  --build=all        Build every output as soon as the mount is up, and
                     rebuild any whenever its inputs change. Opening an output
                     still waits for it, and moves it ahead of the rest.
  -h, --help         Show this help.
)";
}

std::string_view unmount() {
  return R"(usage: makebelieve unmount <mountpoint>

Asks the `makebelieve mount` serving <mountpoint> to stop, and waits until it
has torn down. Fails if nothing is mounted there.

options:
  -h, --help  Show this help.
)";
}

std::string_view manifest() {
  return R"(build.makebelieve - the manifest `makebelieve mount` serves

build.makebelieve, in the directory makebelieve is run from, declares each file
the mount serves on a line of its own:

  @/<output> = <action> <argument>

Blank lines, and lines starting with #, are ignored. A line that does not parse
is reported as build.makebelieve:<line>: <problem>.

GENERATED FILES
  Outputs are named with @/, which is the mountpoint: `@/docs/a.html` is served
  as <mountpoint>/docs/a.html. A path without @/ is an ordinary file, relative
  to the manifest's directory. Commands run in the manifest's directory, through
  `sh -c` on Linux and `cmd.exe /s /c` on Windows. The actions are:

  run <command>      The command writes the output to %out, a scratch file with
                     the output's file name (so a tool that picks its format by
                     extension needs no extra flag). Leaving nothing at %out is
                     a failed build.
                       @/paper.pdf = run pandoc paper.md -o %out
  capture <command>  The output is what the command writes to standard output.
                       @/version.txt = capture git describe --tags
  copy <path>        The output is the bytes of <path>, which must be inside the
                     manifest's directory; nothing runs. `copy @/<output>`
                     copies another declared output.
                       @/config.json = copy etc/config.json
  tracing            Takes no argument. The output is a trace, in Chrome's
                     Trace Event Format, of what makebelieve has been doing:
                     each open through the mount, each build with its command
                     and duration. Open it in https://ui.perfetto.dev.
                       @/tracing.json = tracing

  %out is the only built-in variable, and is replaced with a quoted path.

  A command reads another output by naming it with @/. Before the command runs,
  each @/<path> in it becomes that output's quoted path through the mount, and
  opening it there builds it first. Outside quotes the path ends at the next
  space or shell metacharacter; inside quotes ("@/my notes.md") at the closing
  quote. Outputs may be chained like this to any depth:

    @/paper.md  = capture python render.py
    @/paper.pdf = run pandoc @/paper.md -o %out

  An output that would wait on itself, directly or round a cycle, fails.

WHAT IS TRACKED
  Every file a command opens inside the manifest's directory, and every output
  it opens through the mount, becomes an input; when one changes, the output is
  rebuilt. Changing a rule's command rebuilds its output too. Not tracked:
  files outside the manifest's directory (compilers, system headers), files a
  command looked for but did not find, and directory listings - a command that
  lists a directory is not rebuilt when a file is added to it. A copy is
  always rebuilt when its source changes.

DIRECTORY EXPANSION
  A * in the output's file name declares one output per matching source file.
  The sources are the files matching the first word of the command to hold a
  * (for copy, its path), and every * in the command becomes what the * matched:

    @/out/*.o = run clang++ -c src/*.cpp -o %out
    @/site/*.md = copy notes/*.md

  With src/main.cpp present, the first declares @/out/main.o, built by
  `clang++ -c src/main.cpp -o %out`. Outputs appear and disappear as matching
  files do. Each pattern holds a single *, in its file name, matching one or
  more characters; the source pattern is an unquoted path inside the manifest's
  directory. What the * matched goes into the command as it is, so quote the *
  if file names may hold spaces. Without a * in the output, a * in the command
  is left to the shell.

  A source pattern under @/ matches other outputs instead:

    @/gen/*.md    = capture python render.py src/*.txt
    @/site/*.html = run pandoc @/gen/*.md -o %out

  No two rules may ever be able to produce the same path, so with @/out/*.foo
  declared nothing else in @/out may end in .foo: not an output (@/out/a.foo),
  another wildcard (@/out/lib*.foo, @/out/*o) or a directory (@/out/x.foo/y).
  Nor may a wildcard over outputs match its own outputs.

VARIABLES AND RULES
  User variables ($name = value) and reusable rules (`!cc = clang++ -c %in -o
  %out`, invoked as `@/foo.o = !cc foo.cpp`) are planned, but this version
  rejects them: every line that is not blank or a comment must declare an @/
  output.

SERVING AND RESUMING
  See `makebelieve mount --help`: outputs are built when opened (or all up front
  with --build=all), and builds are kept in <mountpoint>.makebelieve so the next
  mount serves unchanged outputs without running anything.

EXAMPLE
  With notes/a.md and notes/b.md beside it:

    # build.makebelieve
    @/greeting.txt = capture echo hello
    @/shout.txt    = capture tr a-z A-Z < @/greeting.txt
    @/html/*.html  = run pandoc notes/*.md -o %out
    @/raw.md       = copy notes/a.md

  then, with `makebelieve mount ../out` running in another terminal:

    $ ls ../out ../out/html
    ../out:
    greeting.txt  html  raw.md  shout.txt

    ../out/html:
    a.html  b.html
    $ cat ../out/shout.txt    # builds greeting.txt, then shout.txt
    HELLO
    $ makebelieve unmount ../out

  Editing notes/a.md rebuilds html/a.html and raw.md, once each has been opened.
)";
}

std::string_view version() {
  return "makebelieve " MAKEBELIEVE_VERSION;
}

}  // namespace makebelieve::help
