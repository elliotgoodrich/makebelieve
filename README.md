# makebelieve

**makebelieve** is a demand-driven build system exposed as a filesystem.

Declare `output = action argument` rules in a `build.makebelieve` file,

```text
# build.makebelieve
@/hello.txt = capture printf "hello world\n"
@/built.txt = run node generate.js --output %out
@/copied.txt = copy source.txt
```

With `run` the command writes the output itself, to the path `%out`
stands for; with `capture` the bytes the command writes to standard
output are the output. `copy` takes a path instead of a command and
hands back that file's bytes, running nothing. `tracing` takes nothing
at all and hands back a performance trace of **makebelieve** itself, and
`status`, nothing too, a report of what is building and every output
whose build is failing, with its command's exit status and standard
error - empty once everything is built (see [`DOCS.md`](DOCS.md)).

Then **makebelieve** will instantly manifest this as a folder,

```sh
$ makebelieve mount ../output
$ ls ../output/mount
../output/mount/built.txt
../output/mount/copied.txt
../output/mount/hello.txt
```

`makebelieve mount` reads the `build.makebelieve` in the current
directory and serves the outputs at `../output/mount`. The directory it is
given must be outside the source directory, so it can't live inside it as
`./output`. It runs in the foreground until stopped, so run the commands below from
another terminal (or background it with `&`).

Nothing is built until a file is opened. Opening it runs the
corresponding command, automatically collecting all dependencies, and
waits for it to finish, so the first read already sees the output,

```sh
$ cat ../output/mount/hello.txt
hello world
```

Listing or `stat`-ing a file never waits: until it has been built a
file reports a size of 1, and after that the size of its last build.

To build everything up front instead, in the background, mount with
`makebelieve mount --build=all ../output`; opening a file still waits for it,
and goes ahead of the background builds (see [`DOCS.md`](DOCS.md)).

To stop serving, press Ctrl+C in the terminal running `makebelieve
mount`, or from another terminal run,

```sh
$ makebelieve unmount ../output
```

which asks the running instance to unmount and blocks until it has torn
down.

Reading an output whose build failed fails too. To find out why, without
watching the terminal running the mount, add `@/status.txt = status` to
the manifest and read it,

```sh
$ cat ../output/mount/status.txt
@/hello.txt building: capture printf "hello world\n"
@/built.txt failed (exit 1): run node generate.js --output %out
    Error: Cannot find module 'generate.js'
```

It is empty once everything is built and nothing is failing; while
anything is still building, read it again later.

If one of the dependencies changes, **makebelieve** will rerun the
command and regenerate the output, and opening the file while that is
under way waits for it.

What has been built is kept beside the mount, in `../output/.makebelieve`, so
stopping **makebelieve** - or a crash - loses nothing: the next `mount`
serves every output whose command and dependencies are unchanged without
building it again.

This build system works well in situations where we can listen 
for notifications when a file changes and react accordingly. For example,
a static website generator where the browser can reload when a file is
updated.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for build and test instructions.
