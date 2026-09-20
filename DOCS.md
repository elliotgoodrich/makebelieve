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

An output is assigned an action and the command it applies to. With
`run` the command writes the output itself, to the path `%out` stands
for. With `capture` the command writes nothing, and the bytes it sends
to standard output are the output:

```mb
@/version.txt = capture git describe --tags
```

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

Without the `!`, the right-hand side is a built-in action (`run` or
`capture`) and its command.

## Directory Expansion

A wildcard can create one output per matching input:

```mb
@/out/*.o = !cc src/*.cpp
```

For now, only the simple single-`*` case is defined.

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
