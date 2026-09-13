---
title: Build these docs.
pagetitle: Getting started
subtitle: A real project, four generated files, and a browser pointed at the result.
section: GETTING STARTED
kicker: FROM SOURCE TO BROWSER
start: true
---

## Prerequisites

You need a built makebelieve executable, [Pandoc](https://pandoc.org/installing.html),
CMake, and Node.js with npm on your machine. The commands below assume
`pandoc`, `cmake`, and `npm` are on `PATH`.

Build MakeBelieve from the repository root using your configured C++ toolchain
(on Windows, use a Visual Studio developer shell):

```sh
cmake -S . -B build
cmake --build build --target makebelieve
```

Windows requires the Windows Projected File System (ProjFS) optional feature.
Linux requires FUSE 3 development libraries to build, and a usable `/dev/fuse`
to mount. Under WSL, keep both source and output on the native Linux filesystem.

## Mount the generated files

Run MakeBelieve **from `docs/`**. Its working directory determines the source
root and the location of `build.makebelieve`.

On Windows, in PowerShell:

```powershell
cd docs
../build/src/makebelieve.exe mount ../docs_output
```

With a multi-configuration generator, the executable may instead be
`../build/src/Debug/makebelieve.exe` or `../build/src/Release/makebelieve.exe`.

On Linux:

```sh
cd docs
../build/src/makebelieve mount ../docs_output
```

Leave this terminal running; `mount` serves in the foreground until you
stop it with Ctrl+C, or with `makebelieve unmount ../docs_output` from another
terminal. The mount directory must not exist:
MakeBelieve creates it and refuses to reuse an existing directory.
The source and output must not overlap: `docs/` and `docs_output/` are siblings.
Mounting from the repository root would make `docs_output/` overlap the source.

## Open the local preview

In a second terminal, from the repository root:

```sh
npm install
npm run docs:serve
```

Open [localhost:8000](http://localhost:8000). The preview binds to the loopback
interface and serves the mounted output using BrowserSync. It does not run
Pandoc itself. Configure the port and watching in `docs/browser-sync.cjs`.

The first request may show **Building this page…**. When the generated page
arrives, BrowserSync refreshes the browser. HTML changes trigger refreshes;
CSS changes are injected without reloading the page. MakeBelieve reads the
loading document from `docs/placeholders/page.html`, selected by `[*.html]`
in the manifest. BrowserSync injects its client into both this placeholder and
generated HTML; no custom preview middleware is needed. The configuration
polls output metadata so it also works with virtual filesystem notifications.

## Try the dependency graph

- Edit a paragraph in `docs/index.md`. The introduction should update.
- Edit `docs/template.html`. Every page already read should rebuild.
- Edit `docs/style.css`. The stylesheet should update independently.
- Visit a page you have not opened yet. Its first read should start its build.

Outputs are read-only. Make changes in `docs/`; `docs_output/` is ignored by Git.
When changing `build.makebelieve`, restart MakeBelieve: manifest hot reload
is not implemented yet.

## Stop or troubleshoot

Press **Ctrl+C** in the preview terminal and in the MakeBelieve terminal to
stop each process. On Linux, if the mount remains attached, unmount it with
`fusermount3 -u docs_output` from the repository root.

If a page stays on the loading screen, check the MakeBelieve terminal and
verify `pandoc --version` works in the environment used to launch MakeBelieve.
You can isolate the converter by running this from `docs/`:

```sh
pandoc index.md --from=markdown --to=html5 --standalone --template=template.html --css=style.css -o ../build/docs-check.html
```

That diagnostic writes outside the mount. Command exit codes are not currently checked by MakeBelieve,
so a failed converter may leave an empty output; fix the command or tool setup
and restart the mount if it does not recover.

Restart MakeBelieve after changing placeholder files or their settings.
Placeholder files must contain at least one byte so a filesystem read can
trigger generation; the HTML placeholder includes a body for BrowserSync.
