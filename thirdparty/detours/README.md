# Detours (vendored)

Source: [microsoft/Detours](https://github.com/microsoft/Detours), tag
[`v4.0.1`](https://github.com/microsoft/Detours/releases/tag/v4.0.1)
(commit `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`). `LICENSE.md` is
Detours' own, unmodified.

`src/` is that tag's `src/` directory as-is, with one local patch already
applied: four functions in `creatwth.cpp` (`DetourFinishHelperProcess`,
`AllocExeHelper`, `DetourProcessViaHelperDllsA`/`W`) declared a local with
an initializer after an earlier `goto Cleanup;` that jumps past it while
still in scope - legal under the laxer rules Detours v4.0.1 was written
against, but rejected as error C2362 by every MSVC version this project's
CI matrix uses. Each declaration was hoisted (uninitialized) above the
earliest `goto` in its function, turning the original declaration site
into a plain assignment - behavior-preserving, since none of these
variables are read on any path reaching `Cleanup` before their original
assignment would have run.

## Build divergence from Detours' Makefile

The `src/` here is compiled by `../../experiments/detours_tracing/
CMakeLists.txt`, not by Detours' own `src/Makefile`, and one build knob is
intentionally different: `_WIN32_WINNT` is set to `0x0A00` (Windows 10)
rather than the Makefile's `0x501` (Windows XP). The `detours_tracing_hook`
DLL that links this library calls `GetFinalPathNameByHandleW`, declared in
windows.h only from Vista (`0x0600`) up, and the define is `PUBLIC` so the
library and that consumer share one value. `0x501` was a floor inherited
from Detours' age, not a requirement of the library's code, and this
project targets Windows 11 regardless. No Detours source was changed for
this - it's a compile definition on our side.

## Tracking upstream

This project doesn't track Detours upstream automatically - v4.0.1 is 8
years old and effectively unmaintained, so there's no live-fetch benefit
being traded away. To pick up a newer release, replace `src/` with that
release's `src/`, re-apply (or re-derive) the same `creatwth.cpp` fix if
it still applies, and update this file's commit/tag reference.
