# Tracing example

A [`build.makebelieve`](build.makebelieve) with a `tracing` rule, for looking at
where makebelieve spends its time. Its outputs take different amounts of time
to build - `slow.txt` sleeps for a second - so they stand apart in a trace
viewer, and `@/tracing.json` serves the trace itself.

Build makebelieve first (see [`CONTRIBUTING.md`](../../CONTRIBUTING.md)), then
run the demo script for your platform from anywhere:

```powershell
.\examples\tracing\demo.ps1
```

```sh
./examples/tracing/demo.sh
```

Each one mounts this directory, reads every output, rewrites `input.txt` so
that `copied.txt` and `nested/direct.txt` are rebuilt, and saves the trace to
`trace.json` here before unmounting. Pass the path to `makebelieve` if it is not
at the default `build/src/Debug/makebelieve.exe` (Windows) or
`build/src/makebelieve` (Linux).

Open `trace.json` at [ui.perfetto.dev](https://ui.perfetto.dev) (**Open trace
file**) or in `chrome://tracing`. You should see:

- a group per process reading the mount, named after its executable, holding
  an `open <file>` span for each file it read. Within a group the spans go on
  rows called `reader` - one row per open that process has in flight at once,
  rather than one per filesystem thread
- a `makebelieve` group holding its own threads and its builds, the latter on
  rows called `build`, each span named after the output it builds -
  `slow.txt` about a second long, with its command and result in the details
  pane. A row is reused once the build on it finishes, so there are only as
  many rows as there were builds running at once
- an arrow from each `open` to the build it asked for, and from a
  `source change` to the rebuilds it caused
- `copied.txt` and `nested/direct.txt` built more than once, since rewriting
  `input.txt` dirtied them

To explore by hand instead, run `makebelieve mount <mountpoint>` in this
directory and read `<mountpoint>/tracing.json` whenever you like: every read
is a fresh snapshot of everything since the mount started.
