# Black-box test of finding out why builds fail without watching the mount:
# mounts a manifest with an `errors` rule, fails a build in each way an agent
# might - a command that exits nonzero, a tool that does not exist, an output
# reading one that failed, and a broken edit to the manifest - and checks that
# both `errors.txt` and the mount's own stderr say so,
# then that fixing each clears it.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   WORK_DIR          scratch directory this script owns exclusively

set(SRC "${WORK_DIR}/src")
set(BUILD_DIR "${WORK_DIR}/out")
set(MNT "${BUILD_DIR}/mount")
set(LOG "${WORK_DIR}/daemon.log")

# How long any one read, or wait for a change to land, may take.
set(TIMEOUT 60)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${SRC}")

set(RULES
  "@/errors.txt = errors\n"
  "@/good.txt = capture cmake -E echo good\n"
  "@/downstream.txt = run cmake -E copy @/bad.txt %out\n")
file(WRITE "${SRC}/build.makebelieve" ${RULES}
  "@/bad.txt = capture cmake -E echo broken 1>&2 && exit 3\n"
  "@/missing.txt = capture makebelieve-no-such-tool\n")

set(FAILURES "")

# Starts the daemon in the background, with its stderr kept in LOG, and waits
# for the mount to come up. Redirecting makes Start-Process hand the daemon
# every handle PowerShell can pass on, so PowerShell's own output goes to files
# rather than to pipes this script would wait on for as long as the daemon ran.
if(WIN32)
  execute_process(
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
    "$ErrorActionPreference='Stop'; Start-Process -FilePath '${MAKEBELIEVE_EXE}' -ArgumentList 'mount','${BUILD_DIR}' -WorkingDirectory '${SRC}' -WindowStyle Hidden -RedirectStandardError '${LOG}' -RedirectStandardOutput '${WORK_DIR}/daemon.out'"
    OUTPUT_FILE "${WORK_DIR}/launch.out"
    ERROR_FILE "${WORK_DIR}/launch.err")
else()
  execute_process(
    COMMAND sh -c
    "cd \"${SRC}\" && \"${MAKEBELIEVE_EXE}\" mount \"${BUILD_DIR}\" >\"${LOG}\" 2>&1 &")
endif()
set(MOUNTED FALSE)
foreach(attempt RANGE 1 200)
  if(EXISTS "${MNT}/errors.txt")
    set(MOUNTED TRUE)
    break()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
if(NOT MOUNTED)
  message(FATAL_ERROR "the daemon did not mount within the timeout")
endif()

# Reads `output` through the mount into `variable`, and whether that worked
# into `variable_OK`.
#
# Copied out rather than read with `cmake -E cat`, which on Windows succeeds,
# printing nothing, when the file cannot be opened.
function(read_output output variable)
  set(copy "${WORK_DIR}/read.txt")
  file(REMOVE "${copy}")
  execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${MNT}/${output}" "${copy}"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET
                  TIMEOUT ${TIMEOUT})
  if(rc EQUAL 0)
    file(READ "${copy}" value)
    set(${variable} "${value}" PARENT_SCOPE)
    set(${variable}_OK TRUE PARENT_SCOPE)
  else()
    set(${variable} "" PARENT_SCOPE)
    set(${variable}_OK FALSE PARENT_SCOPE)
  endif()
endfunction()

# Fails, naming `stage`, unless `text` contains `expected`.
function(expect_contains stage text expected)
  string(FIND "${text}" "${expected}" at)
  if(at EQUAL -1)
    set(FAILURES "${FAILURES};${stage}: '${text}' does not contain '${expected}'"
        PARENT_SCOPE)
  endif()
endfunction()

# Waits until errors.txt, read afresh, contains `expected` - a change to the
# manifest lands in its own time - and fails, naming `stage`, if it never does.
function(wait_for_errors stage expected)
  foreach(attempt RANGE 1 200)
    read_output(errors.txt errors)
    string(FIND "${errors}" "${expected}" at)
    if(NOT at EQUAL -1)
      return()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
  endforeach()
  set(FAILURES "${FAILURES};${stage}: errors.txt never held '${expected}', last '${errors}'"
      PARENT_SCOPE)
endfunction()

# Nothing has failed yet.
read_output(errors.txt errors)
if(NOT errors_OK OR NOT errors STREQUAL "")
  list(APPEND FAILURES "at first: errors.txt read '${errors}' (ok: ${errors_OK}), expected it empty")
endif()

# Fail every way but the manifest.
read_output(good.txt good)
if(NOT good_OK)
  list(APPEND FAILURES "good.txt could not be read")
endif()
foreach(output bad.txt downstream.txt missing.txt)
  read_output(${output} value)
  if(value_OK)
    list(APPEND FAILURES "${output} read as '${value}' though its build fails")
  endif()
endforeach()

read_output(errors.txt errors)
expect_contains("after failing" "${errors}"
  "@/bad.txt failed (exit 3): capture cmake -E echo broken 1>&2 && exit 3\n    broken\n")
expect_contains("after failing" "${errors}"
  "@/downstream.txt failed because @/bad.txt failed\n")
expect_contains("after failing" "${errors}"
  "@/missing.txt failed (exit ")
expect_contains("after failing" "${errors}" "makebelieve-no-such-tool")
string(FIND "${errors}" "good.txt" at)
if(NOT at EQUAL -1)
  list(APPEND FAILURES "after failing: errors.txt names good.txt: '${errors}'")
endif()

# A broken edit to the manifest is reported in its own terms.
file(APPEND "${SRC}/build.makebelieve" "not a rule\n")
wait_for_errors("broken manifest" "build.makebelieve:6: ")

# Fixing everything clears it all, once each output builds again.
file(WRITE "${SRC}/build.makebelieve" ${RULES}
  "@/bad.txt = capture cmake -E echo fixed\n")
foreach(attempt RANGE 1 200)
  read_output(errors.txt errors)
  string(FIND "${errors}" "build.makebelieve:" at)
  string(FIND "${errors}" "missing.txt" missing_at)
  if(at EQUAL -1 AND missing_at EQUAL -1)
    break()
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
endforeach()
read_output(downstream.txt downstream)
if(NOT downstream_OK)
  list(APPEND FAILURES "downstream.txt still fails once bad.txt is fixed")
endif()
read_output(errors.txt errors)
if(NOT errors STREQUAL "")
  list(APPEND FAILURES "once fixed: errors.txt read '${errors}', expected it empty")
endif()

execute_process(COMMAND "${MAKEBELIEVE_EXE}" unmount "${BUILD_DIR}"
                RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET TIMEOUT ${TIMEOUT})
if(NOT rc EQUAL 0)
  list(APPEND FAILURES "unmount did not report success (was ${rc})")
endif()

# The mount's stderr said which output each failure belonged to.
file(READ "${LOG}" log)
expect_contains("the mount's stderr" "${log}"
  "makebelieve: @/bad.txt failed (exit 3): capture cmake -E echo broken 1>&2 && exit 3")
expect_contains("the mount's stderr" "${log}" "    broken")

if(FAILURES)
  list(REMOVE_ITEM FAILURES "")
  string(JOIN "\n  - " joined ${FAILURES})
  message(FATAL_ERROR "makebelieve errors test failed:\n  - ${joined}")
endif()
message(STATUS "makebelieve errors test passed")
