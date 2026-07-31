# FUSE is Linux-only; nothing to build here on Windows.
if(WIN32)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

add_executable(fuse_experiment fuse.m.cpp)
target_link_libraries(fuse_experiment PRIVATE PkgConfig::FUSE3 Threads::Threads)
target_compile_definitions(fuse_experiment PRIVATE FUSE_USE_VERSION=35)
makebelieve_apply_clang_tidy(fuse_experiment)

add_executable(inotify_wait inotify_wait.m.cpp)
makebelieve_apply_clang_tidy(inotify_wait)

# Black-box test: mounts the real fuse_experiment binary and drives it
# through smoke_test.sh, which self-skips (rather than failing) if
# /dev/fuse isn't usable, e.g. on a sandboxed CI runner.
add_test(NAME fuse_experiment_smoke
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.sh"
    "$<TARGET_FILE:fuse_experiment>" "$<TARGET_FILE:inotify_wait>"
)
set_tests_properties(fuse_experiment_smoke PROPERTIES
  SKIP_REGULAR_EXPRESSION "SKIP:"
  TIMEOUT 30
)
