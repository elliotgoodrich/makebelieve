# Black-box test of running outputs rather than reading them: spawns the
# daemon over integration/executable and runs a generated executable and a
# generated script straight from the mount, first before they have been built,
# so that running them is what asks for the build, and then again once built.
# Then runs many programs the moment they are built, and rebuilds a program
# while a copy of it is still running.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   HELLO_EXE         the built helper executable to generate copies of
#   REPLACEMENT_EXE   a different program to rebuild a running copy into
#   FIXTURE_DIR       integration/executable
#   WORK_DIR          scratch directory this script owns exclusively

set(SRC "${WORK_DIR}/src")
set(MNT "${WORK_DIR}/mnt")

# How long any one run may take before it counts as a hang.
set(RUN_TIMEOUT 60)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${SRC}")
file(COPY "${FIXTURE_DIR}/" DESTINATION "${SRC}")
file(COPY_FILE "${HELLO_EXE}" "${SRC}/hello.exe")
file(COPY_FILE "${HELLO_EXE}" "${SRC}/waiter-input.exe")
# Each run the moment its build finishes, so each is one chance to lose a race
# with whatever the daemon does as a build completes.
set(FRESH_RUNS 30)
foreach(i RANGE 1 ${FRESH_RUNS})
  file(APPEND "${SRC}/build.makebelieve"
    "
@/fresh${i}.exe = run cmake -E copy hello.exe %out
")
endforeach()

# Launched as the main integration test launches it; see the notes there.
if(WIN32)
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
    "$ErrorActionPreference='Stop'; Start-Process -FilePath '${MAKEBELIEVE_EXE}' -ArgumentList 'mount','${MNT}' -WorkingDirectory '${SRC}' -WindowStyle Hidden")
else()
  execute_process(
    COMMAND sh -c
    "cd \"${SRC}\" && \"${MAKEBELIEVE_EXE}\" mount \"${MNT}\" >\"${WORK_DIR}/daemon.log\" 2>&1 &")
endif()

set(FAILURES "")

# Runs `command`, bounded, and records a failure unless it exits 0 and prints
# `expected` (ignoring a trailing newline). `when` says which run this is.
function(run_expecting when expected)
  execute_process(COMMAND ${ARGN}
                  OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE rc
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE
                  TIMEOUT ${RUN_TIMEOUT})
  if(NOT rc EQUAL 0 OR NOT output STREQUAL "${expected}")
    set(FAILURES
        "${FAILURES};running ${ARGN} ${when} exited '${rc}' printing '${output}' (stderr '${error}'), not '${expected}'"
        PARENT_SCOPE)
  endif()
endfunction()

set(MOUNTED FALSE)
foreach(attempt RANGE 1 100)
  if(EXISTS "${MNT}/hello.exe")
    set(MOUNTED TRUE)
    break()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()

if(NOT MOUNTED)
  list(APPEND FAILURES "daemon did not mount within the timeout")
else()
  if(WIN32)
    file(TO_NATIVE_PATH "${MNT}/hello.cmd" script)
    set(SCRIPT_COMMAND cmd /c "${script}")
  else()
    set(SCRIPT_COMMAND "${MNT}/hello.sh")
  endif()
  foreach(when "before it was built" "once built")
    run_expecting("${when}" "hello from a generated executable" "${MNT}/hello.exe")
    run_expecting("${when}" "hello from a generated script" ${SCRIPT_COMMAND})
  endforeach()

  # Run straight after their builds, every run must succeed.
  set(fresh_failures "")
  foreach(i RANGE 1 ${FRESH_RUNS})
    execute_process(COMMAND "${MNT}/fresh${i}.exe"
                    OUTPUT_VARIABLE output RESULT_VARIABLE rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
                    TIMEOUT ${RUN_TIMEOUT})
    if(NOT rc EQUAL 0 OR NOT output STREQUAL "hello from a generated executable")
      list(APPEND fresh_failures "fresh${i}.exe: ${rc}")
    endif()
  endforeach()
  if(fresh_failures)
    list(LENGTH fresh_failures count)
    string(JOIN ", " joined ${fresh_failures})
    list(APPEND FAILURES
         "${count} of ${FRESH_RUNS} programs run as they were built failed: ${joined}")
  endif()

  # A program keeps running the build it was started from when its output is
  # rebuilt underneath it, as it would had the build replaced the file on
  # disk. execute_process runs its commands side by side, as a pipeline: the
  # waiting program, and the script that rebuilds it and then lets it go on.
  execute_process(
    COMMAND "${MNT}/waiter.exe" "${WORK_DIR}/waiter.log" "${WORK_DIR}/release"
    COMMAND "${CMAKE_COMMAND}"
      "-DREADY=${WORK_DIR}/waiter.log"
      "-DRELEASE=${WORK_DIR}/release"
      "-DREPLACEMENT=${REPLACEMENT_EXE}"
      "-DINPUT=${SRC}/waiter-input.exe"
      "-DOUTPUT=${MNT}/waiter.exe"
      "-DRESULT=${WORK_DIR}/rebuild.txt"
      -P "${FIXTURE_DIR}/rebuild_while_running.cmake"
    RESULTS_VARIABLE waiter_results OUTPUT_QUIET ERROR_QUIET
    TIMEOUT ${RUN_TIMEOUT})
  set(waiter_log "")
  if(EXISTS "${WORK_DIR}/waiter.log")
    file(READ "${WORK_DIR}/waiter.log" waiter_log)
  endif()
  set(rebuild "")
  if(EXISTS "${WORK_DIR}/rebuild.txt")
    file(READ "${WORK_DIR}/rebuild.txt" rebuild)
  endif()
  list(GET waiter_results 0 waiter_result)
  if(NOT rebuild STREQUAL "rebuilt")
    list(APPEND FAILURES "rebuilding a running program: ${rebuild}")
  elseif(NOT waiter_result EQUAL 0 OR NOT waiter_log STREQUAL "started
finished
")
    string(REPLACE "
" "|" waiter_log "${waiter_log}")
    list(APPEND FAILURES
         "a program rebuilt while running exited '${waiter_result}' having logged '${waiter_log}', not 'started|finished|'")
  endif()
endif()

execute_process(COMMAND "${MAKEBELIEVE_EXE}" unmount "${MNT}"
                RESULT_VARIABLE unmount_result OUTPUT_QUIET ERROR_QUIET
                TIMEOUT ${RUN_TIMEOUT})
if(NOT unmount_result EQUAL 0)
  list(APPEND FAILURES "unmount did not report success (was ${unmount_result})")
endif()

if(FAILURES)
  string(JOIN "\n  - " joined ${FAILURES})
  message(FATAL_ERROR "makebelieve executable test failed:\n  - ${joined}")
endif()
message(STATUS "makebelieve executable test passed")
