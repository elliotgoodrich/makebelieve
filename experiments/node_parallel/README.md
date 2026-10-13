# Node parallel loading

Checks whether `node` loads several generated files at once, so that their
builds overlap, or one after another.

`build.makebelieve` declares ten outputs for each way of loading them, each of
which waits a second and then copies `module.mjs` (or `module.js` for
`require`). `parallel.ps1` mounts it and runs `load.mjs` once per way, each on
its own ten unbuilt outputs, timing how long all ten take to load. If their
builds overlap that is about 1s; if they run one at a time, about 10s.
`load.mjs` also checks every file was read in full.

## Running

Build `makebelieve` first, then:

```powershell
experiments\node_parallel\parallel.ps1 [-Makebelieve path\to\makebelieve.exe]
```

It prints a markdown table and takes about 50 seconds. It has only been run
on Windows.

## Results

On Windows with node v24.16.0 and 16 cores, so makebelieve's default build
limit allows all ten at once:

| How the 10 files are loaded | Time |
| --- | --- |
| Static `import` ×10 in one entry module | 12.6s |
| `Promise.all` of 10 `import()` calls | 12.7s |
| `require()` ×10 | 12.4s |
| `Promise.all` of 10 `fs.promises.readFile` calls | 3.8s |
| 10 callback-style `fs.readFile` calls at once | 3.7s |
| `fs.promises.readFile` ×10 with `UV_THREADPOOL_SIZE=16` | 1.5s |
| `fs.promises.readFile` ×10 first, then `import()` them all | 3.8s read + 0.1s import |

- Modules load one at a time, even with every `import()` started at once.
- Asynchronous reads run on libuv's thread pool, 4 threads by default, so ten
  files take three rounds. Raising `UV_THREADPOOL_SIZE` lets all ten build at
  once.
- Reading the files asynchronously before importing them builds them in
  parallel, after which the imports are almost instant.

Each file costs about 1.2s rather than 1s, mostly from starting `cmake` twice
per build.
