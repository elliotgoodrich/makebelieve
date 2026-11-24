# Stops with "NOT BUILT: <path>" for a binary that wasn't built, which ctest
# skips with MAKEBELIEVE_SKIP_UNBUILT_TESTS and fails without it.
function(require_built)
  foreach(path IN LISTS ARGN)
    if(NOT EXISTS "${path}")
      message(FATAL_ERROR "NOT BUILT: ${path}")
    endif()
  endforeach()
endfunction()
