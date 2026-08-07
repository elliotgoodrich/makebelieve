# Detours-based hooking is Windows-only; nothing to build here elsewhere.
if(NOT WIN32)
  return()
endif()

# Detours ships no CMakeLists.txt of its own - its native build is an
# nmake Makefile (src/Makefile) - so the static library below is our own
# translation of that Makefile's OBJS list into CMake, not something
# Detours provides.
#
# Vendored (not FetchContent'd) under ../../thirdparty/detours - see that
# directory's README.md for exactly which tag this is and the one local
# patch already applied to creatwth.cpp (a real MSVC error C2362 in
# upstream v4.0.1, fixed by hoisting a few declarations past some
# `goto Cleanup;` jumps that leave them in scope but skip their
# initializer). Detours is 8 years unmaintained at this tag, so a live
# fetch bought nothing but a network dependency and a patch step that
# could silently stop matching; a vendored copy is strictly simpler.
set(detours_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/detours")

# Mirrors src/Makefile's OBJS list exactly, all five per-architecture
# disassemblers included: Detours' own official build compiles all of
# them into detours.lib on every target architecture (each disolXXX.cpp
# exports a differently-named entry point, so nothing collides), rather
# than selecting just one to match the host architecture.
#
# uimports.cpp is deliberately not listed as its own source file -
# src/Makefile's dependency line for creatwth.obj
# ("creatwth.obj : creatwth.cpp uimports.cpp detours.h") shows it's
# textually #included by creatwth.cpp itself, not compiled as a separate
# translation unit; listing it here too would double-define its symbols.
add_library(detours STATIC
  "${detours_SOURCE_DIR}/src/detours.cpp"
  "${detours_SOURCE_DIR}/src/modules.cpp"
  "${detours_SOURCE_DIR}/src/disasm.cpp"
  "${detours_SOURCE_DIR}/src/image.cpp"
  "${detours_SOURCE_DIR}/src/creatwth.cpp"
  "${detours_SOURCE_DIR}/src/disolx86.cpp"
  "${detours_SOURCE_DIR}/src/disolx64.cpp"
  "${detours_SOURCE_DIR}/src/disolia64.cpp"
  "${detours_SOURCE_DIR}/src/disolarm.cpp"
  "${detours_SOURCE_DIR}/src/disolarm64.cpp"
)
target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
# Detours' own headers rely on implicit int/pointer conversions and benign
# sign-compare mismatches throughout; this is vendored, unmodified
# third-party source, not code this project owns the warnings for.
target_compile_options(detours PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W0>
)
# Matches src/Makefile's own CFLAGS (WIN32_LEAN_AND_MEAN, _WIN32_WINNT=0x501)
# on both the library and its consumers below - a library and its callers
# disagreeing on _WIN32_WINNT-conditional declarations in detours.h/windows.h
# is exactly the kind of silent ABI mismatch worth ruling out explicitly
# rather than leaving to whatever windows.h's own unversioned default is.
target_compile_definitions(detours PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x501)

# The payload DLL injected into the traced command (and, transitively,
# every child process it spawns - see detours_tracing_hook.cpp).
#
# The /export flag is required, not optional, and its absence fails
# *every* injection target with STATUS_INVALID_IMAGE_FORMAT, confirmed by
# actually hitting that failure first: DetourCreateProcessWithDllEx works
# by rewriting the target's in-memory import table to reference ordinal
# #1 of the injected DLL, and the Windows loader rejects the whole image
# outright if nothing is actually exported there. DetourFinishHelperProcess
# is a real, callable function Detours' own creatwth.cpp defines - part of
# the `detours` static lib linked in below - so exporting it here just
# forwards to code that's already been linked in, not something this
# project implements. Matches Detours' own samples/simple/Makefile
# (`/export:DetourFinishHelperProcess,@1,NONAME`) and is explicitly
# documented as mandatory in the DetourCreateProcessWithDll wiki page's
# Remarks section - easy to miss since it isn't mentioned on the function
# declaration or in detours.h itself.
add_library(detours_tracing_hook SHARED detours_tracing_hook.cpp)
target_link_libraries(detours_tracing_hook PRIVATE detours)
target_link_options(detours_tracing_hook PRIVATE
  "/export:DetourFinishHelperProcess,@1,NONAME"
)
target_compile_definitions(detours_tracing_hook PRIVATE UNICODE _UNICODE)
makebelieve_apply_clang_tidy(detours_tracing_hook)

# The launcher, which locates detours_tracing_hook.dll next to its own
# executable at runtime (see find_hook_dll_path() in detours_tracing.m.cpp) -
# add_dependencies just ensures build ordering, since there's no
# compile-time link between the two.
add_executable(detours_tracing_experiment detours_tracing.m.cpp)
target_link_libraries(detours_tracing_experiment PRIVATE detours)
target_compile_definitions(detours_tracing_experiment PRIVATE UNICODE _UNICODE)
add_dependencies(detours_tracing_experiment detours_tracing_hook)
makebelieve_apply_clang_tidy(detours_tracing_experiment)

# Black-box test: runs the real detours_tracing_experiment binary against
# a scratch directory and checks which files it reports as read,
# mirroring ../FUSE_tracing/smoke_test.sh. Reuses ../ETW_tracing's
# catfiles.ps1 rather than a second copy - it's a plain script file, not
# a CMake target, so it's referenced by path.
add_test(NAME detours_tracing_experiment_smoke
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.ps1"
    -TracingExe "$<TARGET_FILE:detours_tracing_experiment>"
    -CatfilesScript "${CMAKE_CURRENT_SOURCE_DIR}/../ETW_tracing/catfiles.ps1"
)
set_tests_properties(detours_tracing_experiment_smoke PROPERTIES TIMEOUT 30)
