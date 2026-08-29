# Makebelieve Manifest

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

Without `:`, the right-hand side is a normal command.

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
