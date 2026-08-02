cmake_minimum_required(VERSION 3.16)
project(makebelieve LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(ENABLE_CLANG_TIDY "Enable clang-tidy static analysis" OFF)
if(ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
  if(NOT CLANG_TIDY_EXE)
    message(WARNING "clang-tidy not found. Static analysis will be skipped.")
  endif()
endif()

find_package(Threads REQUIRED)

enable_testing()

add_compile_options(
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
  # These experiments designated-initialize large C callback tables (e.g.
  # fuse_operations) and rely on the rest being zero-initialized - that's
  # the whole point of the idiom, not an oversight, so don't warn on it.
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wno-missing-field-initializers>
)

# ASan/UBSan flags below are GCC/Clang syntax; skip on MSVC for now rather
# than wiring up MSVC's separate (and UBSan-less) /fsanitize=address. CONFIG
# (not CMAKE_BUILD_TYPE) is required here since Debug/Release is chosen at
# build time, not configure time, on multi-config generators like the
# Visual Studio one used on Windows.
set(ENABLE_SANITIZERS $<AND:$<CONFIG:Debug>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>)
add_compile_options($<${ENABLE_SANITIZERS}:-fsanitize=address,undefined>)
add_link_options(
  $<${ENABLE_SANITIZERS}:-fsanitize=address,undefined>
  $<${ENABLE_SANITIZERS}:-fno-omit-frame-pointer>
)

# Applied per-target by each experiment via this helper, not the global
# CMAKE_CXX_CLANG_TIDY variable, so it can't leak into fetched third-party
# sources once any experiment starts pulling those in.
function(makebelieve_apply_clang_tidy target)
  if(ENABLE_CLANG_TIDY AND CLANG_TIDY_EXE)
    set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
  endif()
endfunction()

# Each subdirectory under experiments/ is a standalone example application
# exercising one OS filesystem/tracing mechanism (FUSE, ProjFS, ETW, ...);
# add new ones here as they're written.
add_subdirectory(experiments/FUSE)
add_subdirectory(experiments/ProjFS)
