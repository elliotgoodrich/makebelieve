# Black-box test of builds whose commands open other generated outputs through
# the mount: spawns the daemon capped at one build at a time (`-j 1`) over
# integration/dependencies, whose commands are real `cmake -E cat`s of other
# outputs, and checks that a chain longer than the cap, a fan-in opened all at
# once, and a command that opens a generated output as its very first action
# all build and read back correctly. At a cap of one, a command's open that
# was not attributed to its build - one registered too late, say - would hold
# the only permit while waiting on a build that needs it, and hang; every read
# here is bounded, so that fails rather than hangs.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   FIXTURE_DIR       integration/dependencies
#   WORK_DIR          scratch directory this script owns exclusively
#   JOBS_AFTER_MOUNTPOINT  optional: exercise `mount <mountpoint> -j 1`

set(SRC "${WORK_DIR}/src")
set(MNT "${WORK_DIR}/mnt")

# How long any one read may take before it counts as a hang.
set(READ_TIMEOUT 60)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${SRC}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${SRC}")
# Exceed the previous 64-parked-open limit with real traced child commands.
foreach(link RANGE 1 79)
  math(EXPR next "${link} + 1")
  file(APPEND "${SRC}/build.makebelieve"
    "\n@/deep${link}.txt = capture cmake -E cat ../mnt/deep${next}.txt\n")
endforeach()
file(APPEND "${SRC}/build.makebelieve" "\n@/deep80.txt = capture cmake -E cat leaf.txt\n")

file(READ "${SRC}/leaf.txt" LEAF)
file(READ "${SRC}/shared-leaf.txt" SHARED_LEAF)

# Launched as the main integration test launches it; see the notes there.
if(WIN32)
  if(JOBS_AFTER_MOUNTPOINT)
    set(mount_arguments "'mount','${MNT}','-j','1'")
  else()
    set(mount_arguments "'mount','-j','1','${MNT}'")
  endif()
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
    "$ErrorActionPreference='Stop'; Start-Process -FilePath '${MAKEBELIEVE_EXE}' -ArgumentList ${mount_arguments} -WorkingDirectory '${SRC}' -WindowStyle Hidden")
else()
  if(JOBS_AFTER_MOUNTPOINT)
    set(mount_arguments "mount \"${MNT}\" -j 1")
  else()
    set(mount_arguments "mount -j 1 \"${MNT}\"")
  endif()
  execute_process(
    COMMAND sh -c
    "cd \"${SRC}\" && \"${MAKEBELIEVE_EXE}\" ${mount_arguments} >\"${WORK_DIR}/daemon.log\" 2>&1 &")
endif()

set(FAILURES "")

# Reads `output` from the mount, bounded, into `out_value`; records a failure
# if the read failed or timed out.
function(read_output output out_value)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${MNT}/${output}"
                  OUTPUT_VARIABLE value RESULT_VARIABLE rc ERROR_QUIET
                  TIMEOUT ${READ_TIMEOUT})
  if(NOT rc EQUAL 0)
    set(FAILURES "${FAILURES};reading ${output} failed or hung (${rc})"
        PARENT_SCOPE)
  endif()
  set(${out_value} "${value}" PARENT_SCOPE)
endfunction()

set(MOUNTED FALSE)
foreach(attempt RANGE 1 100)
  if(EXISTS "${MNT}/chain1.txt")
    set(MOUNTED TRUE)
    break()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()

if(NOT MOUNTED)
  list(APPEND FAILURES "daemon did not mount within the timeout")
else()
  # The chain: opening its head builds every link, one waiting on the next,
  # each giving its permit back while it waits.
  read_output(chain1.txt chain)
  if(NOT chain STREQUAL "${LEAF}")
    list(APPEND FAILURES "chain1.txt read '${chain}', not '${LEAF}'")
  endif()
  foreach(link RANGE 2 6)
    read_output(chain${link}.txt value)
    if(NOT value STREQUAL "${LEAF}")
      list(APPEND FAILURES "chain${link}.txt read '${value}', not '${LEAF}'")
    endif()
  endforeach()

  read_output(deep1.txt deep)
  if(NOT deep STREQUAL "${LEAF}")
    list(APPEND FAILURES "80-command dependency chain failed")
  endif()

  # The fan-in, opened at once: execute_process runs its commands side by
  # side, as a pipeline. Each copies its output to a file rather than printing
  # it, so no stage writes into a pipe a later stage may already have closed.
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy "${MNT}/fan1.txt" "${WORK_DIR}/fan1.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${MNT}/fan2.txt" "${WORK_DIR}/fan2.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${MNT}/fan3.txt" "${WORK_DIR}/fan3.txt"
    COMMAND "${CMAKE_COMMAND}" -E copy "${MNT}/fan4.txt" "${WORK_DIR}/fan4.txt"
    RESULTS_VARIABLE fan_results OUTPUT_QUIET ERROR_QUIET
    TIMEOUT ${READ_TIMEOUT})
  foreach(fan_result IN LISTS fan_results)
    if(NOT fan_result EQUAL 0)
      list(APPEND FAILURES "opening the fan-in at once failed or hung (${fan_results})")
      break()
    endif()
  endforeach()
  foreach(fan RANGE 1 4)
    if(EXISTS "${WORK_DIR}/fan${fan}.txt")
      file(READ "${WORK_DIR}/fan${fan}.txt" value)
    else()
      set(value "")
    endif()
    if(NOT value STREQUAL "${SHARED_LEAF}")
      list(APPEND FAILURES "fan${fan}.txt read '${value}', not '${SHARED_LEAF}'")
    endif()
  endforeach()

  # A command whose first action is to open a generated output.
  read_output(first.txt first)
  if(NOT first STREQUAL "${LEAF}")
    list(APPEND FAILURES "first.txt read '${first}', not '${LEAF}'")
  endif()
endif()

execute_process(COMMAND "${MAKEBELIEVE_EXE}" unmount "${MNT}"
                RESULT_VARIABLE unmount_result OUTPUT_QUIET ERROR_QUIET
                TIMEOUT ${READ_TIMEOUT})
if(NOT unmount_result EQUAL 0)
  list(APPEND FAILURES "unmount did not report success (was ${unmount_result})")
endif()

if(FAILURES)
  string(JOIN "\n  - " joined ${FAILURES})
  message(FATAL_ERROR "makebelieve dependency test failed:\n  - ${joined}")
endif()
message(STATUS "makebelieve dependency test passed")
