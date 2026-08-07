# ETW is Windows-only; nothing to build here elsewhere.
if(NOT WIN32)
  return()
endif()

add_executable(etw_tracing_experiment etw_tracing.m.cpp)
target_link_libraries(etw_tracing_experiment PRIVATE advapi32 tdh)
target_compile_definitions(etw_tracing_experiment PRIVATE UNICODE _UNICODE)
makebelieve_apply_clang_tidy(etw_tracing_experiment)

# Black-box test: runs the real etw_tracing_experiment binary against a
# scratch directory and checks which files it reports as read, mirroring
# ../FUSE_tracing/smoke_test.sh. Fails (rather than skipping) when not
# running elevated, since consuming NT Kernel Logger events always needs
# Administrator or "Performance Log Users" regardless of what the traced
# command itself needs - the same policy ../FUSE_tracing/smoke_test.sh
# uses for /dev/fuse being unavailable, rather than silently skipping on a
# runner that can't satisfy it.
#
# Drives the traced command via catfiles.ps1 (here and in
# ../detours_tracing, which reuses this same script rather than a second
# copy) - a plain script file, not a CMake target, so it's referenced by
# path rather than $<TARGET_FILE:...>.
add_test(NAME etw_tracing_experiment_smoke
  COMMAND powershell -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.ps1"
    -TracingExe "$<TARGET_FILE:etw_tracing_experiment>"
    -CatfilesScript "${CMAKE_CURRENT_SOURCE_DIR}/catfiles.ps1"
)
set_tests_properties(etw_tracing_experiment_smoke PROPERTIES TIMEOUT 30)
