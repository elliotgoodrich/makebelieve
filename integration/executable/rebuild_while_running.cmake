# Run beside a waiting copy of the helper program started from the mount (see
# hello.m.cpp): once it has started, replaces the input its output is built
# from with a different program, reads the output back until it has been
# rebuilt from that, and only then lets the waiting program carry on.
#
# Expected -D variables:
#   READY        the file the waiting program writes once started
#   RELEASE      the file whose creation lets it carry on
#   REPLACEMENT  the program to replace the input with
#   INPUT        the source file the output is built from
#   OUTPUT       the output, through the mount
#   RESULT       where to write "rebuilt", or why not

foreach(attempt RANGE 1 600)
  if(EXISTS "${READY}")
    break()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
endforeach()

set(result "the program never started")
if(EXISTS "${READY}")
  file(SHA256 "${REPLACEMENT}" wanted)
  file(COPY_FILE "${REPLACEMENT}" "${INPUT}")
  set(result "the output was never rebuilt")
  foreach(attempt RANGE 1 600)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sha256sum "${OUTPUT}"
                    OUTPUT_VARIABLE sum ERROR_QUIET)
    string(REGEX MATCH "^[0-9a-f]+" sum "${sum}")
    if(sum STREQUAL wanted)
      set(result "rebuilt")
      break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.1)
  endforeach()
endif()

file(WRITE "${RESULT}" "${result}")
file(WRITE "${RELEASE}" "")
