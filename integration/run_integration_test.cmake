# Black-box integration test for the makebelieve daemon: spawns it, mounts a
# build.makebelieve, and checks outputs appear as 1-byte placeholders, emit a
# change notification when built or rebuilt, and hold the right value.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   NOTIFY_PROBE_EXE  the built change-notification probe helper
#   FIXTURE_DIR       integration/fixture (build.makebelieve + inputs)
#   WORK_DIR          scratch directory this script owns exclusively

set(SRC "${WORK_DIR}/src")
set(MNT "${WORK_DIR}/mnt")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${SRC}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${SRC}")

# The updated value is copied over the input mid-run.
file(READ "${SRC}/input.txt" EXPECTED_INITIAL)
set(EXPECTED_UPDATED "updated makebelieve input\n")
file(WRITE "${WORK_DIR}/updated.txt" "${EXPECTED_UPDATED}")

# Re-reads `path` until it holds `expected`, or a ~5s budget runs out. A change
# notification is not a content barrier on ProjFS: it can arrive while the
# placeholder update is still in flight, so a read racing it may see the old
# value or briefly fail to open. Retrying is the supported way to consume the
# notification; `cmake -E cat` (rather than file(READ)) so an in-flight open
# error is just a miss to retry, not a fatal script error. On Linux the read is
# served synchronously and this matches on the first pass.
function(read_until_equal path expected out_matched out_last)
  set(matched FALSE)
  set(actual "")
  foreach(attempt RANGE 1 100)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${path}"
                    OUTPUT_VARIABLE actual RESULT_VARIABLE rc ERROR_QUIET)
    if(rc EQUAL 0 AND actual STREQUAL "${expected}")
      set(matched TRUE)
      break()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
  endforeach()
  set(${out_matched} "${matched}" PARENT_SCOPE)
  set(${out_last} "${actual}" PARENT_SCOPE)
endfunction()

# The probe triggers a read (to build) and an input change (to rebuild). These
# differ per platform only because Windows has no sh/cat/cp; the Linux commands
# are the ones proven on that runner, and cmake -E stands in on Windows.
if(WIN32)
  set(BUILD_TRIGGER "${CMAKE_COMMAND}" -E cat "${MNT}/copy.txt")
  set(REBUILD_TRIGGER
      "${CMAKE_COMMAND}" -E copy "${WORK_DIR}/updated.txt" "${SRC}/input.txt")
else()
  set(BUILD_TRIGGER sh -c "cat \"${MNT}/copy.txt\" >/dev/null")
  set(REBUILD_TRIGGER sh -c "cp \"${WORK_DIR}/updated.txt\" \"${SRC}/input.txt\"")
endif()

# Launch the daemon in the background. Both backends serve from cwd=SRC with
# `mount <mountpoint>`; only the way the shell backgrounds them differs, and the
# unmount below stops either one by name.
if(WIN32)
  # Start-Process backgrounds the daemon in its own hidden window. Deliberately
  # no -RedirectStandardError: that switch makes Start-Process launch with
  # bInheritHandles=TRUE, and the daemon would then inherit cmake's capture pipes
  # and hand them to every build subprocess it spawns, whose stdout ProcessUtil
  # waits on - deadlocking the first read. The daemon's own stderr is therefore
  # not captured here; a mount failure is diagnosed from the assertions below
  # rather than a log.
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
    "$ErrorActionPreference='Stop'; Start-Process -FilePath '${MAKEBELIEVE_EXE}' -ArgumentList 'mount','${MNT}' -WorkingDirectory '${SRC}' -WindowStyle Hidden")
else()
  execute_process(
    COMMAND sh -c
    "cd \"${SRC}\" && \"${MAKEBELIEVE_EXE}\" mount \"${MNT}\" >\"${WORK_DIR}/daemon.log\" 2>&1 &")
endif()

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
  # The Linux launch captures the daemon's output; the Windows one cannot (see
  # the launch note above), so only point at the log where there is one.
  if(WIN32)
    list(APPEND FAILURES "daemon did not mount within the timeout")
  else()
    list(APPEND FAILURES
         "daemon did not mount within the timeout; see ${WORK_DIR}/daemon.log")
  endif()
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

  # First read builds copy.txt: expect a change notification, then the built
  # value once it settles.
  execute_process(
    COMMAND "${NOTIFY_PROBE_EXE}" "${MNT}" copy.txt 5000 -- ${BUILD_TRIGGER}
    RESULT_VARIABLE probe_build OUTPUT_QUIET ERROR_QUIET)
  if(NOT probe_build EQUAL 0)
    list(APPEND FAILURES "no filesystem notification when copy.txt was first built")
  endif()
  read_until_equal("${MNT}/copy.txt" "${EXPECTED_INITIAL}" built_ok built_value)
  if(NOT built_ok)
    list(APPEND FAILURES
         "copy.txt after first build never settled to '${EXPECTED_INITIAL}' (last read '${built_value}')")
  endif()

  # Changing the traced input rebuilds copy.txt: expect a notification, then the
  # new value once it settles.
  execute_process(
    COMMAND "${NOTIFY_PROBE_EXE}" "${MNT}" copy.txt 5000 -- ${REBUILD_TRIGGER}
    RESULT_VARIABLE probe_rebuild OUTPUT_QUIET ERROR_QUIET)
  if(NOT probe_rebuild EQUAL 0)
    list(APPEND FAILURES "no filesystem notification after the input changed")
  endif()
  read_until_equal("${MNT}/copy.txt" "${EXPECTED_UPDATED}" rebuilt_ok rebuilt_value)
  if(NOT rebuilt_ok)
    list(APPEND FAILURES
         "copy.txt after the input changed never settled to '${EXPECTED_UPDATED}' (last read '${rebuilt_value}')")
  endif()
endif()

# Ask the daemon to unmount, which blocks until it has torn down. A clean stop
# matters on Windows: a hard kill would orphan the ProjFS placeholders, which
# then resist the next run's file(REMOVE_RECURSE).
execute_process(COMMAND "${MAKEBELIEVE_EXE}" unmount "${MNT}"
                RESULT_VARIABLE unmount_result OUTPUT_QUIET ERROR_QUIET)
if(NOT unmount_result EQUAL 0)
  list(APPEND FAILURES "unmount did not report success (was ${unmount_result})")
endif()

# unmount only returns once teardown is complete, so the mountpoint the daemon
# created must be gone.
if(EXISTS "${MNT}")
  list(APPEND FAILURES "mountpoint still exists after unmount returned")
endif()

if(FAILURES)
  string(JOIN "\n  - " joined ${FAILURES})
  message(FATAL_ERROR "makebelieve integration test failed:\n  - ${joined}")
endif()
message(STATUS "makebelieve integration test passed")
