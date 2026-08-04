# FUSE is Linux-only; nothing to build here on Windows.
if(WIN32)
  return()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)

add_executable(fuse_tracing_experiment fuse_tracing.m.cpp)
target_link_libraries(fuse_tracing_experiment
  PRIVATE PkgConfig::FUSE3 Threads::Threads)
target_compile_definitions(fuse_tracing_experiment PRIVATE FUSE_USE_VERSION=35)
makebelieve_apply_clang_tidy(fuse_tracing_experiment)

# Black-box test: runs the real fuse_tracing_experiment binary against a
# scratch directory and checks which files it reports as read, which fails
# (rather than skipping) if /dev/fuse isn't usable, e.g. on a sandboxed CI
# runner.
add_test(NAME fuse_tracing_experiment_smoke
  COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.sh"
    "$<TARGET_FILE:fuse_tracing_experiment>"
)
set_tests_properties(fuse_tracing_experiment_smoke PROPERTIES TIMEOUT 30)
