# Detours Tracing Experiment

`detours_tracing_experiment <command> [args...]` runs `<command>` with
[Microsoft Detours](https://github.com/microsoft/Detours) hooking
`CreateFileW`/`CreateFileA` inside its own process, and prints every file
under the current directory that the command - or a descendant process it
spawns - opened for reading. It's the Detours counterpart to
[`../FUSE_tracing`](../FUSE_tracing) and [`../ETW_tracing`](../ETW_tracing),
and aims for the same report by a third structurally different mechanism:
API interposition inside the traced process itself, rather than a virtual
filesystem or a system-wide observer.

## How it works

1. The launcher locates `detours_tracing_hook.dll` next to its own
   executable and starts `<command>` via `DetourCreateProcessWithDllExW`,
   which creates the process suspended, injects the DLL, and only then
   resumes it - so the hooks are always installed before `<command>`'s own
   code runs.
2. The injected DLL detours `CreateFileW`/`CreateFileA` in `<command>`'s
   own address space. Every successful, read-access, non-directory open
   under the current directory is appended to a log file, whose path (and
   the current directory itself) the launcher passed in via environment
   variables before spawning.
3. The DLL also detours `CreateProcessW`/`CreateProcessA`, re-injecting
   itself into every child process `<command>` spawns via the same
   `DetourCreateProcessWithDllEx` mechanism - so a build action that
   shells out to other tools is still fully traced, not just its own
   direct opens.
4. Once `<command>` exits, the launcher reads the log back (every write
   completed synchronously inside the hooked call, so nothing further
   needs to be flushed or awaited) and prints the deduplicated, sorted
   list of files read.

## Differences from FUSE_tracing/ETW_tracing

- **Scope: two Win32 entry points, not the filesystem stack.** This
  tracer only sees opens made through `CreateFileW`/`CreateFileA`. A
  statically-linked binary, or one that calls `NtCreateFile` directly, is
  structurally invisible to it - unlike FUSE_tracing (which sits below all
  of that, at the filesystem-driver boundary) or ETW_tracing (which
  observes the same kernel-level `NtCreateFile` activity ETW_tracing's own
  `FileIo_Create` events are drawn from).
- **Bitness-locked.** The launcher and `<command>` must both be the same
  bitness (32-bit or 64-bit) - `DetourCreateProcessWithDllEx` cannot
  inject across that boundary.
- **Read-only in spirit, not by construction.** Like ETW_tracing, this one
  never denies a write - both are purely observational. There's nothing
  here playing the role of FUSE_tracing's `EROFS`.

## Build dependency

Detours has no CMakeLists.txt of its own (its native build is an `nmake`
Makefile); `CMakeLists.txt` here fetches its source via `FetchContent` and
compiles the handful of files its own `src/Makefile` lists into a static
library, pinned to release
[`v4.0.1`](https://github.com/microsoft/Detours/releases/tag/v4.0.1).

## A non-obvious requirement worth calling out

`detours_tracing_hook.dll` must export `DetourFinishHelperProcess` at
ordinal `#1` (`target_link_options(detours_tracing_hook PRIVATE
"/export:DetourFinishHelperProcess,@1,NONAME")` in `CMakeLists.txt`), or
`DetourCreateProcessWithDllEx` fails *every* target it launches with
`STATUS_INVALID_IMAGE_FORMAT`. `DetourCreateProcessWithDllEx` rewrites the
target's in-memory import table to reference ordinal 1 of the injected
DLL, and the Windows loader rejects the whole image outright if nothing
is actually exported there - `DetourFinishHelperProcess` itself is a real
function already defined in Detours' own `creatwth.cpp` (part of the
`detours` static lib linked into the hook DLL), so exporting it is just
forwarding to code that's already there, not something this project
implements. This is documented in the
[`DetourCreateProcessWithDll` wiki page](https://github.com/microsoft/Detours/wiki/DetourCreateProcessWithDll)'s
Remarks section and demonstrated in Detours' own `samples/simple/Makefile`,
but easy to miss since it isn't mentioned anywhere in `detours.h` itself
or on the function declarations - this was found by hitting the failure
first, then confirming against [a maintainer's answer on the same
question](https://github.com/microsoft/Detours/issues/120).
