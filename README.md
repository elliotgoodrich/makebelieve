# MakeBelieve

**MakeBelieve** is a demand-driven build system exposed as a filesystem.

Declare `output <- command` rules in a `build.makebelieve` file,

```text
# build.makebelieve
@/hello.txt <- /bin/sh -c 'printf "hello world\n" > %out'
@/copied.txt <- cp source.txt %out
```

Then **Makebelieve** will instantly manifest this as a folder,

```sh
$ makebelieve mount ../output
$ ls ../output
../output/copied.txt
../output/hello.txt
```

`makebelieve mount` reads the `build.makebelieve` in the current
directory and serves the outputs at `../output`; the mountpoint must be
outside the source directory, so it can't live inside it as `./output`.
It runs in the foreground until stopped, so run the commands below from
another terminal (or background it with `&`).

On first access, files will be empty,

```sh
$ cat ../output/hello.txt
```

But **MakeBelieve** will run the corresponding command behind the
scenes, automatically collecting all dependencies, and then will
update the output,

```sh
$ cat ../output/hello.txt
Hello World
```

To stop serving, press Ctrl+C in the terminal running `makebelieve
mount`, or from another terminal run,

```sh
$ makebelieve unmount ../output
```

which asks the running instance to unmount and blocks until it has torn
down.

If the command in `build.makebelieve` is updated, or one of the
dependencies changes, **MakeBelieve** will rerun the command and
regenerate the output.

This build system works well in situations where we can listen 
for notifications when a file changes and react accordingly. For example,
a static website generator where the browser can reload when a file is
updated.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for build and test instructions.
