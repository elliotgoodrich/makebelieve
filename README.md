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
hands back that file's bytes, running nothing.

Then **makebelieve** will instantly manifest this as a folder,

```sh
$ makebelieve mount ../output
$ ls ../output
../output/built.txt
../output/copied.txt
../output/hello.txt
```

`makebelieve mount` reads the `build.makebelieve` in the current
directory and serves the outputs at `../output`; the mountpoint must be
outside the source directory, so it can't live inside it as `./output`.
It runs in the foreground until stopped, so run the commands below from
another terminal (or background it with `&`).

Nothing is built until a file is opened. Opening it runs the
corresponding command, automatically collecting all dependencies, and
waits for it to finish, so the first read already sees the output,

```sh
$ cat ../output/hello.txt
hello world
```

Listing or `stat`-ing a file never waits: until it has been built a
file reports a size of 1, and after that the size of its last build.

To stop serving, press Ctrl+C in the terminal running `makebelieve
mount`, or from another terminal run,

```sh
$ makebelieve unmount ../output
```

which asks the running instance to unmount and blocks until it has torn
down.

If one of the dependencies changes, **makebelieve** will rerun the
command and regenerate the output, and opening the file while that is
under way waits for it.

This build system works well in situations where we can listen 
for notifications when a file changes and react accordingly. For example,
a static website generator where the browser can reload when a file is
updated.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for build and test instructions.
