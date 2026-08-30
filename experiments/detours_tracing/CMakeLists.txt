# Detours-based hooking is Windows-only; nothing to build here elsewhere.
if(NOT WIN32)
  return()
endif()

# The `detours` static library is defined once at ../../thirdparty/detours and
# pulled in from the root CMakeLists (before src), since the product links it
# too now; this experiment just consumes that shared target. See that
# directory's README.md and CMakeLists.txt for the vendored tag, the local
# creatwth.cpp patch, and the raised _WIN32_WINNT floor.
target_compile_definitions(detours PUBLIC WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)

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
