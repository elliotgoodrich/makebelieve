# Makebelieve Manifest

> **Partly implemented:** explicit outputs, reusable commands with `%in` and
> `%out`, and configurable placeholders are supported. User variables and
> directory expansion below still describe planned syntax.
> See [the current manifest reference](docs/manifest.md) for implemented behaviour.

A manifest describes generated files and the commands used to produce them.

## Variables

User variables use `$`; Makebelieve built-ins use `%`:

```mb
cc = clang++
flags = -O2
```

## Generated Files

Generated files live in the mounted output namespace and use `@/`:

```mb
@/output.txt <- cp input.txt %out
```

Paths without `@/` refer to the normal filesystem.

## Rules

Reusable commands can be declared as rules:

```mb
rule cc = clang++ $flags -c %in -o %out
    flags = -O2
```

Invoke them using `:`:

```mb
@/foo.o <- cc: foo.cpp
    flags = -O0 -g
```

An invocation is a bare rule name, a colon, then whitespace. A colon anywhere
else is part of the command, so `C:	oolsuild.exe` and `https://` URLs are
left alone. Invoking an undeclared rule is an error.

`%in` is substituted with the invocation's input, quoted when it contains
whitespace; `%out` with the output path, always quoted. Both are whole-word
tokens, so `%input` and `%output` are left as they stand.

## Directory Expansion

A wildcard can create one output per matching input:

```mb
@/out/*.o <- cc: src/*.cpp
```

For now, only the simple single-`*` case is defined.

## Placeholders

When an output has not yet been generated, Makebelieve can return a placeholder file.

Defaults can be selected by filename:

```mb
[*.html]
    %placeholder = placeholders/page.html

[no extension]
    %placeholder = placeholders/file

[*]
    %placeholder = placeholders/default
```

`[*.]` can match filenames that actually end in `.`.

A rule can override the default:

```mb
rule page = build-page %in %out
    %placeholder = placeholders/page.html
```

And an individual output can override the rule:

```mb
@/index.html <- page: src/index.md
    %placeholder = placeholders/index.html
```

Precedence is **output → rule → filename selector**.

Selectors match the output's basename. Exact names take precedence over
single-`*` patterns and `[no extension]`, which take precedence over `[*]`.
For equally specific matches, the last declaration wins. Defaults and named
rules can appear before or after the outputs that use them. A selector accepts
at most one `*`; more than that is rejected rather than silently never matched.

Settings are indented under the output, rule or selector they apply to. A
setting at the top level is an error. A setting Makebelieve does not implement
yet - a user variable - is ignored without ending its block, so a `%placeholder`
below one still applies.

Placeholder paths refer to non-empty files in the source tree; quote paths
containing spaces if desired. MakeBelieve reads their bytes when the mount is
created and rejects unreadable or empty files. Without a matching setting, the
placeholder remains a single null byte. Restart the mount after editing a
placeholder or its configuration. A completed output replaces the placeholder;
later rebuilds continue serving the last generated content.

For HTML previews, use a minimal document such as
`<!doctype html><html><body></body></html>`. A zero-byte file may never receive
the read needed to start its build. BrowserSync can inject its reload client
into the placeholder's body without any MakeBelieve-specific middleware.

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
