# The daemon needs a per-platform DirectoryTree, console-interrupt and virtual
# filesystem backend. Each has a -windows.cpp and a -linux.cpp; the source name
# is selected by the system name so the set stays in lock-step.
string(TOLOWER "${CMAKE_SYSTEM_NAME}" MAKEBELIEVE_PLATFORM)

# Split from the executable so that tests can link the daemon's guts directly
# rather than driving it through the binary.
add_library(makebelieve_core STATIC
  consoleinterrupthandler-${MAKEBELIEVE_PLATFORM}.cpp
  filesystemutil.cpp
  realdirectorytree-${MAKEBELIEVE_PLATFORM}.cpp
  virtualfilesystem-${MAKEBELIEVE_PLATFORM}.cpp
)
target_include_directories(makebelieve_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Platform libraries and definitions as generator expressions, in the same
# spirit as the root CMakeLists selecting compiler flags: the Windows pieces
# fold in under PLATFORM_ID and evaluate to nothing elsewhere.
target_link_libraries(makebelieve_core PUBLIC
  Threads::Threads
  $<$<PLATFORM_ID:Windows>:ProjectedFSLib>   # PrjStartVirtualizing and friends
  $<$<PLATFORM_ID:Windows>:Ole32>            # CoCreateGuid, for the instance ID
)
# WIN32_LEAN_AND_MEAN trims <windows.h>; NOMINMAX drops its min/max macros,
# which otherwise clash with std::min/std::max. Set here so every TU reaching
# <windows.h> has them before the first include, without repeating the guards.
target_compile_definitions(makebelieve_core PUBLIC
  $<$<PLATFORM_ID:Windows>:UNICODE>
  $<$<PLATFORM_ID:Windows>:_UNICODE>
  $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

# The libfuse3 lookup is a configure-time command, not a value, so it cannot be
# a generator expression and stays behind a plain guard. It backs the Linux
# virtual filesystem and advertises, via pkg-config, both its versioned include
# dir (<fuse.h> lives under fuse3/) and the _FILE_OFFSET_BITS=64 it requires;
# linking the imported target PUBLIC propagates those to every TU sharing
# directorytree.hpp's off_t.
if(NOT WIN32)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
  target_link_libraries(makebelieve_core PUBLIC PkgConfig::FUSE3)
  target_compile_definitions(makebelieve_core PRIVATE FUSE_USE_VERSION=35)
endif()

makebelieve_apply_clang_tidy(makebelieve_core)

add_executable(makebelieve makebelieve.m.cpp)
target_link_libraries(makebelieve PRIVATE makebelieve_core)
makebelieve_apply_clang_tidy(makebelieve)

if(BUILD_TESTING)
  # Built on both platforms: consoleinterrupthandler.t.cpp carries a shared set
  # of in-process cases plus one #ifdef'd integration test per backend (a Win32
  # console-control event re-exec'd through CreateProcessW; a raised SIGINT on
  # POSIX). The Windows binary doubles as its own --serve subject, which
  # gtest_discover_tests never triggers (listing does not pass --serve). No
  # clang-tidy here - GoogleTest's macros are not worth linting.
  add_executable(consoleinterrupthandler_test consoleinterrupthandler.t.cpp)
  target_link_libraries(consoleinterrupthandler_test PRIVATE
    makebelieve_core
    GTest::gtest
  )
  # The Windows interrupt case spawns a child and drives a real console event,
  # so it needs the runner to have a console; kept generous so a hang is capped
  # rather than left to stall CI.
  gtest_discover_tests(consoleinterrupthandler_test
    PROPERTIES TIMEOUT 60
  )

  # Pure logic, so it links makebelieve_core just for the is_within symbol and
  # its include path, plus gtest_main's default entry point.
  add_executable(filesystemutil_test filesystemutil.t.cpp)
  target_link_libraries(filesystemutil_test PRIVATE
    makebelieve_core
    GTest::gtest_main
  )
  gtest_discover_tests(filesystemutil_test)

  # Drives the real backend against a real directory: the fixture builds a known
  # tree under temp_directory_path() and tears it down again. gtest_main's entry
  # point is enough - no re-exec'd subject like consoleinterrupthandler_test.
  add_executable(realdirectorytree_test realdirectorytree.t.cpp)
  target_link_libraries(realdirectorytree_test PRIVATE
    makebelieve_core
    GTest::gtest_main
  )
  # Every case touches the filesystem, so it gets a timeout the way the other
  # OS-touching test does. Nothing here blocks, so the cap only has to cover a
  # slow create and remove on a busy runner.
  gtest_discover_tests(realdirectorytree_test
    PROPERTIES TIMEOUT 30
  )
endif()
