# Detours Tracing Experiment

`detours_tracing_experiment <command> [args...]` runs `<command>` with
[Microsoft Detours](https://github.com/microsoft/Detours) hooking ntdll's
`NtCreateFile`/`NtOpenFile` inside its own process, and prints every file
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
2. The injected DLL detours `NtCreateFile`/`NtOpenFile` in `<command>`'s
   own address space - the two ntdll stubs every user-mode file open
   funnels through, whether the caller reached them via `CreateFileW`/`A`,
   the C runtime, or `NtCreateFile` directly. Every successful,
   read-access, non-directory open under the current directory is appended
   to a log file, whose path (and the current directory itself) the
   launcher passed in via environment variables before spawning. The
   opened file's canonical path is read back from the returned handle with
   `GetFinalPathNameByHandleW`, so a relative or `RootDirectory`-based
   (`openat`-style) open is resolved for free rather than parsed out of
   ntdll's `\??\` NT path. (That API is Vista+, so the build raises
   `_WIN32_WINNT` above Detours' Makefile default - see `CMakeLists.txt`.)
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

- **Scope: the ntdll open stubs, not the filesystem stack.** This tracer
  sees opens made through `NtCreateFile`/`NtOpenFile` - which is every
  user-mode open, since `CreateFileW`/`A`, the C runtime, and direct
  `NtCreateFile` callers all funnel through them (this is the same
  kernel-level `NtCreateFile` activity ETW_tracing observes via its
  `FileIo_Create` events, just caught in-process instead of system-wide).
  What remains invisible is only an open that bypasses those stubs by
  issuing the raw syscall itself, and a child launched other than through
  the hooked `CreateProcessW`/`A` (e.g. `NtCreateUserProcess` directly) -
  neither of which FUSE_tracing can miss, since it sits below all of that
  at the filesystem-driver boundary and is scoped by the mount rather than
  by which process or API made the call.
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
