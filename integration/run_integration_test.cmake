# Black-box integration test for the makebelieve daemon: spawns it, mounts a
# build.makebelieve, and checks outputs appear as 1-byte placeholders, emit a
# change notification when built or rebuilt, and hold the right value.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   NOTIFY_PROBE_EXE  the built inotify probe helper
#   FIXTURE_DIR       integration/fixture (build.makebelieve + inputs)
#   WORK_DIR          scratch directory this script owns exclusively

if(WIN32)
  # TODO: a ProjFS-mounted equivalent is follow-up work.
  message("SKIP: makebelieve_integration is not yet implemented for ProjFS")
  return()
endif()

set(SRC "${WORK_DIR}/src")
set(MNT "${WORK_DIR}/mnt")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${SRC}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${SRC}")

# The updated value is copied over the input mid-run.
file(READ "${SRC}/input.txt" EXPECTED_INITIAL)
set(EXPECTED_UPDATED "updated makebelieve input\n")
file(WRITE "${WORK_DIR}/updated.txt" "${EXPECTED_UPDATED}")

# Launch the daemon in the background, recording its pid for teardown.
execute_process(
  COMMAND sh -c
  "cd \"${SRC}\" && \"${MAKEBELIEVE_EXE}\" \"${MNT}\" >\"${WORK_DIR}/daemon.log\" 2>&1 & echo $! >\"${WORK_DIR}/daemon.pid\"")

set(FAILURES "")

set(MOUNTED FALSE)
foreach(attempt RANGE 1 100)
  if(EXISTS "${MNT}/copy.txt")
    set(MOUNTED TRUE)
    break()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()

if(NOT MOUNTED)
  list(APPEND FAILURES
       "daemon did not mount within the timeout; see ${WORK_DIR}/daemon.log")
else()
  # Before any read, each output is a 1-byte placeholder.
  foreach(output copy.txt second.txt)
    if(EXISTS "${MNT}/${output}")
      file(SIZE "${MNT}/${output}" size)
      if(NOT size EQUAL 1)
        list(APPEND FAILURES
             "${output} should be a 1-byte placeholder before first read, was ${size}")
      endif()
    else()
      list(APPEND FAILURES "${output} did not appear in the mount")
    endif()
  endforeach()

  # First read builds copy.txt: expect a notification and the built value.
  execute_process(
    COMMAND "${NOTIFY_PROBE_EXE}" "${MNT}" copy.txt 5000
            -- sh -c "cat \"${MNT}/copy.txt\" >/dev/null"
    RESULT_VARIABLE probe_build OUTPUT_QUIET ERROR_QUIET)
  if(NOT probe_build EQUAL 0)
    list(APPEND FAILURES "no filesystem notification when copy.txt was first built")
  endif()
  file(READ "${MNT}/copy.txt" built_value)
  if(NOT built_value STREQUAL "${EXPECTED_INITIAL}")
    list(APPEND FAILURES
         "copy.txt after first build was '${built_value}', expected '${EXPECTED_INITIAL}'")
  endif()

  # Changing the traced input rebuilds copy.txt: expect a notification and the
  # new value.
  execute_process(
    COMMAND "${NOTIFY_PROBE_EXE}" "${MNT}" copy.txt 5000
            -- sh -c "cp \"${WORK_DIR}/updated.txt\" \"${SRC}/input.txt\""
    RESULT_VARIABLE probe_rebuild OUTPUT_QUIET ERROR_QUIET)
  if(NOT probe_rebuild EQUAL 0)
    list(APPEND FAILURES "no filesystem notification after the input changed")
  endif()
  file(READ "${MNT}/copy.txt" rebuilt_value)
  if(NOT rebuilt_value STREQUAL "${EXPECTED_UPDATED}")
    list(APPEND FAILURES
         "copy.txt after the input changed was '${rebuilt_value}', expected '${EXPECTED_UPDATED}'")
  endif()
endif()

# Signal the daemon so it unmounts and exits cleanly, then wait for it to go.
if(EXISTS "${WORK_DIR}/daemon.pid")
  file(READ "${WORK_DIR}/daemon.pid" pid)
  string(STRIP "${pid}" pid)
  if(pid)
    execute_process(COMMAND kill -TERM "${pid}" ERROR_QUIET)
    foreach(attempt RANGE 1 100)
      execute_process(COMMAND kill -0 "${pid}"
                      RESULT_VARIABLE alive OUTPUT_QUIET ERROR_QUIET)
      if(NOT alive EQUAL 0)
        break()
      endif()
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
    endforeach()
  endif()
endif()

if(FAILURES)
  string(JOIN "\n  - " joined ${FAILURES})
  message(FATAL_ERROR "makebelieve integration test failed:\n  - ${joined}")
endif()
message(STATUS "makebelieve integration test passed")
