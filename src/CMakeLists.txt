# The daemon needs a per-platform DirectoryTree, console-interrupt and virtual
# filesystem backend. Each has a -windows.cpp and a -linux.cpp; the source name
# is selected by the system name so the set stays in lock-step.
string(TOLOWER "${CMAKE_SYSTEM_NAME}" MAKEBELIEVE_PLATFORM)

# Split from the executable so that tests can link the daemon's guts directly
# rather than driving it through the binary.
add_library(makebelieve_core STATIC
  builddirectorytree.cpp
  consoleinterrupthandler-${MAKEBELIEVE_PLATFORM}.cpp
  directorytreeutil.cpp
  filesystemutil.cpp
  inmemorydirectorytree.cpp
  manifest.cpp
  processutil-${MAKEBELIEVE_PLATFORM}.cpp
  realdirectorytree-${MAKEBELIEVE_PLATFORM}.cpp
  stringutil.cpp
  unmountchannel-${MAKEBELIEVE_PLATFORM}.cpp
  unmountchannel-common.cpp
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

# Detours backs the Windows dependency tracer. Linked PRIVATE so its usage
# requirements - notably the raised _WIN32_WINNT detours.h needs - reach core's
# own translation units without leaking onto every test that links core; a
# static lib still forwards the .lib to the final link line.
target_link_libraries(makebelieve_core PRIVATE
  $<$<PLATFORM_ID:Windows>:detours>
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

# The payload DLL makebelieve injects into a build command to trace its file
# reads. Windows-only. It lands beside makebelieve and the test binaries, where
# processutil looks for it at runtime.
if(WIN32)
  # stringutil.cpp is compiled in directly so the hook stays a self-contained
  # payload without pulling in makebelieve_core.
  add_library(makebelievehook SHARED makebelievehook.m.cpp stringutil.cpp)
  target_link_libraries(makebelievehook PRIVATE detours)
  # Required: DetourCreateProcessWithDllEx rewrites the target's import table to
  # reference ordinal #1 of the injected DLL, and the loader rejects the image if
  # nothing is exported there. DetourFinishHelperProcess is a real detours
  # function, so this just forwards to existing code.
  target_link_options(makebelievehook PRIVATE
    "/export:DetourFinishHelperProcess,@1,NONAME"
  )
  target_compile_definitions(makebelievehook PRIVATE UNICODE _UNICODE)
  makebelieve_apply_clang_tidy(makebelievehook)

  # Build the hook whenever core - and any binary linking it - is built, so the
  # DLL is beside the executable at runtime.
  add_dependencies(makebelieve_core makebelievehook)
endif()

add_executable(makebelieve makebelieve.m.cpp)
target_link_libraries(makebelieve PRIVATE makebelieve_core)
makebelieve_apply_clang_tidy(makebelieve)

if(BUILD_TESTING)
  # Waits for a filesystem change notification naming a file.
  add_executable(notifyprobe notifyprobe-${MAKEBELIEVE_PLATFORM}.m.cpp)
  makebelieve_apply_clang_tidy(notifyprobe)

  function(makebelieve_add_test name)
    add_executable(${name}_test ${name}.t.cpp)
    target_link_libraries(${name}_test PRIVATE
      makebelieve_core
      GTest::gtest_main
    )
    gtest_discover_tests(${name}_test PROPERTIES TIMEOUT 30)
  endfunction()

  makebelieve_add_test(builddirectorytree)
  makebelieve_add_test(filesystemutil)
  makebelieve_add_test(manifest)
  makebelieve_add_test(processutil)
  makebelieve_add_test(realdirectorytree)
  makebelieve_add_test(inmemorydirectorytree)
  makebelieve_add_test(directorytreeutil)

  add_executable(consoleinterrupthandler_test consoleinterrupthandler.t.cpp)
  target_link_libraries(consoleinterrupthandler_test PRIVATE
    makebelieve_core
    GTest::gtest
  )
  gtest_discover_tests(consoleinterrupthandler_test PROPERTIES TIMEOUT 30)
endif()
