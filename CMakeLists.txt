# 3.25 is the floor for standard 23: cxx_std_23 arrived in 3.20, and mapping it
# to MSVC's /std:c++latest (MSVC ships no stable /std:c++23) is reliable from
# 3.25 on.
cmake_minimum_required(VERSION 3.25)
project(makebelieve LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
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

option(BUILD_TESTING "Build the tests." ON)
if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
  )
  # Make GoogleTest link the same dynamic CRT this project builds with
  # (/MDd, /MD). Its default is the static CRT, and mixing the two is a hard
  # link error on MSVC - the single most common way a first gtest build fails.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
endif()

add_compile_options(
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall>
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wextra>
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

add_subdirectory(src)

# Add experiments
if(BUILD_TESTING)
  add_subdirectory(experiments/FUSE)
  add_subdirectory(experiments/FUSE_tracing)
  add_subdirectory(experiments/ProjFS)
  add_subdirectory(experiments/ETW_tracing)
  add_subdirectory(experiments/detours_tracing)
endif()
