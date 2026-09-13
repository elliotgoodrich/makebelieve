---
title: A file. A command.
pagetitle: Manifest reference
subtitle: The syntax supported by MakeBelieve today, with the documentation site as a working example.
section: MANIFEST REFERENCE
kicker: DECLARE THE OUTPUT
manifest: true
---

## One output per line

Create a file named `build.makebelieve` in the source directory:

```text
@/output-path <- command
```

The left side names a file inside the mounted filesystem. The right side is
a command executed through the system shell, with the source directory as its
working directory. Blank lines and lines starting with `#` are ignored.

## Output paths and source paths

`@/` marks the generated output namespace on the left side of a declaration.
Normal paths in a command refer to files on the real filesystem, relative to
the source directory unless absolute.

```text
@/style.css <- cmake -E copy style.css %out
```

This reads `style.css` in the source directory and exposes its copy as
`style.css` in the mounted directory.

## The output placeholder

`%out` is replaced with a quoted path to a temporary output file. Write the
result there. MakeBelieve reads that file after a successful command and
updates the mounted output. Do not add another pair of quotes around `%out`.

```text
@/hello.txt <- cmake -E echo "hello, makebelieve" > %out
```

The mounted filename is not the temporary filename. When a converter infers
its output format from the filename, specify the format explicitly. These
docs use Pandoc's `--to=html5` because the temporary path has no `.html` suffix.

## The documentation rules

```text
[*.html]
    %placeholder = placeholders/page.html

@/index.html <- pandoc index.md --from=markdown --to=html5 --standalone --template=template.html --css=style.css -o %out
@/getting-started.html <- pandoc getting-started.md --from=markdown --to=html5 --standalone --template=template.html --css=style.css -o %out
@/manifest.html <- pandoc manifest.md --from=markdown --to=html5 --standalone --template=template.html --css=style.css -o %out
@/style.css <- cmake -E copy style.css %out
```

Each page command reads its Markdown source and the shared template. Pandoc
emits a link to the stylesheet; the separate copy command reads the CSS file.
MakeBelieve discovers dependencies from command file reads rather than from
an explicit input list in the manifest.

Commands must work in the platform's shell. These examples use tools with
cross-platform command-line interfaces; shell-specific scripts may need
different commands on Windows and Linux.

## Current boundaries

- Outputs are generated on demand. Initial reads return the configured
  placeholder (a single null byte by default) until generation completes.
- Changes to tracked source files rebuild affected outputs already read.
- Restart the mount after adding outputs or editing manifest commands.
- Use explicit declarations for each file.

## Planned syntax

User variables and wildcard output expansion in the repository's `DOCS.md`
are **not implemented by the current parser**. Use explicit output paths.

## Configurable placeholders

An indented `%placeholder` setting selects a non-empty file in the source tree.
Its bytes appear in the mounted output until the first build completes.

```text
[*.html]
    %placeholder = placeholders/page.html

[no extension]
    %placeholder = placeholders/file

[*]
    %placeholder = placeholders/default

rule page = pandoc %in --to=html5 --standalone -o %out
    %placeholder = placeholders/page.html

@/index.html <- page: index.md
    %placeholder = placeholders/index.html
```

Precedence is **output → named rule → filename selector**. Selectors match
basenames: exact names beat single-`*` patterns and `[no extension]`, which
beat `[*]`. The last declaration wins between equally specific selectors.
`[*.]` matches a trailing dot, while `[no extension]` matches names such as
`LICENSE` and `.hidden`. Declarations may appear in any order.

An invocation is a bare rule name, a colon, then whitespace: `page: index.md`.
A colon anywhere else in a command is left alone, so `C:	oolsuild.exe` and
`curl https://example.com` stay ordinary commands. Invoking a rule the manifest
does not declare is an error rather than a command the shell fails on.

Named rules substitute `%in` with the invocation's input, quoting it when it
contains whitespace so a path with spaces survives the shell; input that is
already quoted, or that holds a wildcard the shell should expand, is passed
through untouched. `%out` is substituted by MakeBelieve when the command runs.
Both are whole-word tokens: `%input` and `%output` are left as they stand.

A selector accepts at most one `*`, and a setting must be indented under the
output, rule or selector it applies to; either mistake is reported rather than
silently ignored. A setting MakeBelieve does not implement yet - a user
variable - is ignored without ending the block it sits in, so a `%placeholder`
written below one still applies.

Placeholder files must be readable and non-empty. They are loaded at mount
startup; restart after changing their bytes or configuration. Use a minimal
HTML document with a `<body></body>` so BrowserSync can inject its reload
client. A zero-byte file may never receive the read that starts a build.
