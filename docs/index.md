---
title: Files, when you need them.
pagetitle: Introduction
subtitle: MakeBelieve turns build commands into a filesystem. Ask for a file, and the work begins.
section: INTRODUCTION
kicker: A DIFFERENT WAY TO BUILD
home: true
---

## Your build is a directory

MakeBelieve is a demand-driven build system exposed as a filesystem. Declare
the files you want and the commands that produce them. Mount the result as a
directory, then use the tools you already know to read it.

```text
@/index.html <- pandoc index.md --to=html5 --standalone -o %out
```

Here, `index.html` is a generated file. Reading it starts Pandoc, which reads
`index.md` and writes HTML to the path substituted for `%out`.

## Read. Build. Refresh.

1. **Declare an output.** Add an entry to `build.makebelieve`.
2. **Mount the directory.** The declared files appear before they are built.
3. **Read a file.** MakeBelieve runs its command in the background.
4. **Edit a source.** MakeBelieve tracks the files read by the command and
   rebuilds affected outputs that have already been read.

The first read returns a configured placeholder while the command
runs. Subsequent reads see the generated content once it is ready. During a
rebuild, readers can see the previous version until the replacement arrives.

## This website is the example

Each page starts as Markdown. Pandoc combines it with a shared HTML template.
The stylesheet has its own copy rule. MakeBelieve manages when those commands
run and which source changes require another build.

The local preview serves `docs_output/`, a mounted directory beside `docs/`.
MakeBelieve supplies a small HTML loading page on first access. BrowserSync
watches generated files and refreshes the browser after an HTML update, or
injects an updated stylesheet without reloading the page.

## Start small, see what happens

Follow the [getting-started guide](getting-started.html) to run this website.
Then try changing a paragraph, the shared template, or a CSS colour. Each
change exercises a different part of the dependency graph.

> MakeBelieve is experimental. The [manifest reference](manifest.html)
> distinguishes the syntax implemented today from planned additions.
