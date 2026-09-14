---
title: Files, when you need them.
pagetitle: Introduction
subtitle: makebelieve turns build commands into a filesystem. Ask for a file, and the work begins.
section: INTRODUCTION
kicker: A DIFFERENT WAY TO BUILD
home: true
---

## Your build is a directory

`makebelieve` is a reactive build system that creates your files on demand when they are read. The outputs you want are declared in `build.makebelieve` alongside the commands that produce them. Then `makebelieve` mounts the result as a directory and will create these files as they are needed.  Though `makebelieve` is unopinionated, it works well as a static-site generator.

For example:

```text
# build.makebelieve
@/index.html <- pandoc index.md --to=html5 --standalone -o %out
@/favicon.ico <- cp favicon.ico %out
@/beach.jpg <- magick beach_large.jpg %out
```

Can be mounted `makebelieve mount /example-site/` so that we see the generated files

```shell
$ ls /example-site/
favicon.ico
beach.jpg
index.html
```

Reading any file will return a placeholder file and kick off the underlying build command.
When that command finishes the file will be updated with the result.  The dependencies
of the command are tracked automatically so changing any dependency will rebuild.

## Read. Build. Refresh.

1. **Declare an output.** Add an entry to `build.makebelieve`.
2. **Mount the directory.** The declared files appear before they are built.
3. **Read a file.** `makebelieve` runs its command in the background.
4. **Edit a source.** `makebelieve` tracks the files read by the command and
   rebuilds affected outputs that have already been read.

The first read returns a configured placeholder while the command
runs. Subsequent reads see the generated content once it is ready. During a
rebuild, readers can see the previous version until the replacement arrives.

## Next Steps

Follow the [getting-started guide](getting-started.html) to run this website.
Then try changing a paragraph, the shared template, or a CSS colour. Each
change exercises a different part of the dependency graph.

> MakeBelieve is experimental. The [manifest reference](manifest.html)
> distinguishes the syntax implemented today from planned additions.
