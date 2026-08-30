# Detours (vendored) as one shared static-library target. Both the product
# (../../src: makebelieve_core launches build commands with a hook DLL
# injected, and that DLL) and ../../experiments/detours_tracing link it, so it
# is defined here and pulled in once from the root CMakeLists rather than each
# consumer rebuilding it. Windows-only - Detours is a Windows technology, and
# the root only add_subdirectory()s this under WIN32.
#
# Detours ships no CMakeLists.txt of its own - its native build is an nmake
# Makefile (src/Makefile) - so the target below is our own translation of that
# Makefile's OBJS list into CMake, not something Detours provides. See
# README.md in this directory for the vendored tag, the one local patch to
# creatwth.cpp, and why _WIN32_WINNT is raised off the Makefile's default.

# Mirrors src/Makefile's OBJS list exactly, all five per-architecture
# disassemblers included: Detours' own official build compiles all of them into
# detours.lib on every target architecture (each disolXXX.cpp exports a
# differently-named entry point, so nothing collides), rather than selecting
# just one to match the host architecture.
#
# uimports.cpp is deliberately not listed as its own source file - src/Makefile's
# dependency line for creatwth.obj ("creatwth.obj : creatwth.cpp uimports.cpp
# detours.h") shows it's textually #included by creatwth.cpp itself, not compiled
# as a separate translation unit; listing it here too would double-define its
# symbols.
add_library(detours STATIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/detours.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/modules.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/disasm.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/image.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/creatwth.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/disolx86.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/disolx64.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/disolia64.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/disolarm.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/disolarm64.cpp"
)
target_include_directories(detours PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Detours' own headers rely on implicit int/pointer conversions and benign
# sign-compare mismatches throughout; this is vendored, unmodified third-party
# source, not code this project owns the warnings for.
target_compile_options(detours PRIVATE
  $<$<CXX_COMPILER_ID:MSVC>:/W0>
)
# WIN32_LEAN_AND_MEAN matches src/Makefile's own CFLAGS. _WIN32_WINNT, though,
# is deliberately raised to 0x0A00 (Windows 10) rather than the Makefile's
# 0x501 (XP): consumers call GetFinalPathNameByHandleW, which windows.h only
# declares at Vista (0x0600) and above, and this project targets Windows 11
# regardless. It is PUBLIC so the library and any translation unit that
# includes detours.h compile against one _WIN32_WINNT - a library and its
# callers disagreeing on _WIN32_WINNT-conditional declarations in
# detours.h/windows.h is exactly the kind of silent ABI mismatch worth ruling
# out explicitly. (See README.md here for the same note from the vendoring
# side.)
target_compile_definitions(detours PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
