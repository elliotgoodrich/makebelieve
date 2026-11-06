# Stands in for a compiler: accepts INPUT only if every line is a number,
# printing it back, and otherwise reports the first line it objects to on
# standard error and exits nonzero.
file(STRINGS "${INPUT}" lines)
set(number 0)
foreach(line IN LISTS lines)
  math(EXPR number "${number} + 1")
  if(NOT line MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${INPUT}:${number}: expected a number, found '${line}'")
  endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${INPUT}")
