# Black-box test that the daemon picks up where it left off: mounts, reads an
# output whose command reads another output through the mount, unmounts, and
# mounts again - checking that both come back built without their commands
# running again, that an input changed while nothing was mounted rebuilds
# them, and that a changed command rebuilds its output. Every build appends
# its name to a log outside the source directory, which is how the test counts
# them.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   WORK_DIR          scratch directory this script owns exclusively

set(SRC "${WORK_DIR}/src")
set(MNT "${WORK_DIR}/mnt")
set(LOG "${WORK_DIR}/builds.log")

# How long any one read may take before it counts as a hang.
set(READ_TIMEOUT 60)

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${SRC}")
file(WRITE "${SRC}/input.txt" "first input\n")
file(WRITE "${SRC}/build.cmake" [=[
file(APPEND "${LOG}" "${NAME}\n")
file(READ "${IN}" content)
file(WRITE "${OUT}" "${NAME}: ${content}")
]=])

# Writes the manifest, with `top_flag` passed to the top output's command, so
# changing it changes that command alone.
function(write_manifest top_flag)
  file(WRITE "${SRC}/build.makebelieve"
    "@/middle.txt = run cmake -DLOG=${LOG} -DNAME=middle -DIN=input.txt -DOUT=%out -P build.cmake\n"
    "@/top.txt = run cmake -DLOG=${LOG} -DNAME=top -DIN=@/middle.txt -DOUT=%out ${top_flag} -P build.cmake\n")
endfunction()
write_manifest("")

set(FAILURES "")

# Starts the daemon in the background, as the main integration test does (see
# the notes there), and waits for the mount to come up.
function(mount)
  if(WIN32)
    execute_process(
      COMMAND powershell -NoProfile -ExecutionPolicy Bypass -Command
      "$ErrorActionPreference='Stop'; Start-Process -FilePath '${MAKEBELIEVE_EXE}' -ArgumentList 'mount','${MNT}' -WorkingDirectory '${SRC}' -WindowStyle Hidden")
  else()
    execute_process(
      COMMAND sh -c
      "cd \"${SRC}\" && \"${MAKEBELIEVE_EXE}\" mount \"${MNT}\" >>\"${WORK_DIR}/daemon.log\" 2>&1 &")
  endif()
  foreach(attempt RANGE 1 200)
    if(EXISTS "${MNT}/top.txt")
      return()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 0.05)
  endforeach()
  message(FATAL_ERROR "the daemon did not mount within the timeout")
endfunction()

# Asks the daemon to unmount, which blocks until it has torn down.
function(unmount)
  execute_process(COMMAND "${MAKEBELIEVE_EXE}" unmount "${MNT}"
                  RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "unmount did not report success (was ${result})")
  endif()
endfunction()

# Fails, naming `stage`, unless `output` reads back as `expected`.
function(expect_output stage output expected)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E cat "${MNT}/${output}"
                  OUTPUT_VARIABLE value RESULT_VARIABLE rc ERROR_QUIET
                  TIMEOUT ${READ_TIMEOUT})
  if(NOT rc EQUAL 0)
    set(FAILURES "${FAILURES};${stage}: reading ${output} failed or hung (${rc})"
        PARENT_SCOPE)
  elseif(NOT value STREQUAL "${expected}")
    set(FAILURES
        "${FAILURES};${stage}: ${output} read '${value}', expected '${expected}'"
        PARENT_SCOPE)
  endif()
endfunction()

# Fails, naming `stage`, unless the builds so far are `expected`, one name per
# line.
function(expect_builds stage expected)
  set(builds "")
  if(EXISTS "${LOG}")
    file(READ "${LOG}" builds)
  endif()
  if(NOT builds STREQUAL "${expected}")
    set(FAILURES
        "${FAILURES};${stage}: builds were '${builds}', expected '${expected}'"
        PARENT_SCOPE)
  endif()
endfunction()

# Fails, naming `stage`, unless `output` already holds `size` bytes, before
# anything opens it.
function(expect_size stage output size)
  file(SIZE "${MNT}/${output}" actual)
  if(NOT actual EQUAL size)
    set(FAILURES
        "${FAILURES};${stage}: ${output} was ${actual} bytes before any read, expected ${size}"
        PARENT_SCOPE)
  endif()
endfunction()

set(TOP_FIRST "top: middle: first input\n")

# The first run builds both.
mount()
expect_output("first run" top.txt "${TOP_FIRST}")
expect_builds("first run" "top\nmiddle\n")
# Sized on disk rather than from what reading it gave, which on Windows has
# lost its carriage returns.
file(SIZE "${MNT}/top.txt" top_size)
unmount()

# The next run serves both as they were, building nothing.
mount()
expect_size("restart" top.txt ${top_size})
expect_output("restart" top.txt "${TOP_FIRST}")
expect_output("restart" middle.txt "middle: first input\n")
expect_builds("restart" "top\nmiddle\n")
unmount()

# An input changed while nothing was mounted rebuilds what read it, and what
# read that.
file(WRITE "${SRC}/input.txt" "second input\n")
mount()
expect_size("changed input" top.txt 1)
expect_output("changed input" top.txt "top: middle: second input\n")
expect_builds("changed input" "top\nmiddle\ntop\nmiddle\n")
unmount()

# A changed command rebuilds its output alone.
write_manifest("-DUNUSED=1")
mount()
expect_output("changed command" top.txt "top: middle: second input\n")
expect_builds("changed command" "top\nmiddle\ntop\nmiddle\ntop\n")
unmount()

# The cache lives beside the mountpoint, not in the source directory.
if(NOT EXISTS "${MNT}.makebelieve/cache.sqlite")
  list(APPEND FAILURES "no cache was kept at ${MNT}.makebelieve")
endif()
file(GLOB_RECURSE source_files RELATIVE "${SRC}" "${SRC}/*")
list(SORT source_files)
if(NOT source_files STREQUAL "build.cmake;build.makebelieve;input.txt")
  list(APPEND FAILURES "the source directory gained files: ${source_files}")
endif()

if(FAILURES)
  list(REMOVE_ITEM FAILURES "")
  string(JOIN "\n  - " joined ${FAILURES})
  message(FATAL_ERROR "makebelieve resume test failed:\n  - ${joined}")
endif()
message(STATUS "makebelieve resume test passed")
