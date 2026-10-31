# Black-box test of the help makebelieve prints about itself: which command
# lines print help or the version (to stdout, succeeding) and which are
# mistakes (to stderr, failing, with a pointer to --help). Then checks that
# `makebelieve help manifest` keeps up with DOCS.md, the manifest's source of
# truth: it covers every section, action, `%` variable and option DOCS.md
# names, and shows no action DOCS.md does not.
#
# Expected -D variables:
#   MAKEBELIEVE_EXE   the built makebelieve binary
#   DOCS_FILE         DOCS.md
#   EXPECTED_VERSION  the project version from CMakeLists.txt

# For if(IN_LIST), which a script run with -P lacks without a policy version.
cmake_minimum_required(VERSION 3.25)

set(FAILURES "")

macro(fail message)
  string(APPEND FAILURES "  ${message}\n")
endmacro()

# Runs makebelieve with the arguments after @a name, leaving its exit code,
# stdout and stderr in ${name}_CODE, ${name}_OUT and ${name}_ERR.
function(run name)
  execute_process(
    COMMAND "${MAKEBELIEVE_EXE}" ${ARGN}
    RESULT_VARIABLE code
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    TIMEOUT 30)
  set(${name}_CODE "${code}" PARENT_SCOPE)
  set(${name}_OUT "${out}" PARENT_SCOPE)
  set(${name}_ERR "${err}" PARENT_SCOPE)
endfunction()

# Expects `makebelieve ARGN` to print help containing every word in WORDS to
# stdout, nothing to stderr, and succeed. Leaves what it printed in
# ${result}.
function(expect_help result)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "ARGS;WORDS")
  run(r ${arg_ARGS})
  if(NOT r_CODE EQUAL 0)
    fail("`makebelieve ${arg_ARGS}` exited ${r_CODE}, not 0")
  endif()
  if(NOT r_ERR STREQUAL "")
    fail("`makebelieve ${arg_ARGS}` wrote to stderr: ${r_ERR}")
  endif()
  foreach(word IN LISTS arg_WORDS)
    string(FIND "${r_OUT}" "${word}" found)
    if(found EQUAL -1)
      fail("`makebelieve ${arg_ARGS}` does not mention `${word}`")
    endif()
  endforeach()
  set(FAILURES "${FAILURES}" PARENT_SCOPE)
  set(${result} "${r_OUT}" PARENT_SCOPE)
endfunction()

# Expects `makebelieve ARGN` to fail without running anything, saying on
# stderr what was wrong - including PROBLEM - and where to look.
function(expect_usage_error problem)
  run(r ${ARGN})
  if(r_CODE EQUAL 0 OR NOT r_CODE MATCHES "^[0-9]+$")
    fail("`makebelieve ${ARGN}` exited ${r_CODE}, not a failure")
  endif()
  if(NOT r_OUT STREQUAL "")
    fail("`makebelieve ${ARGN}` wrote to stdout: ${r_OUT}")
  endif()
  foreach(word IN ITEMS "${problem}" "makebelieve --help")
    string(FIND "${r_ERR}" "${word}" found)
    if(found EQUAL -1)
      fail("`makebelieve ${ARGN}` does not say `${word}`: ${r_ERR}")
    endif()
  endforeach()
  set(FAILURES "${FAILURES}" PARENT_SCOPE)
endfunction()

# --- Command lines -----------------------------------------------------------

set(commands "mount" "unmount" "help manifest" "--version" "-j <builds>"
  "--build=requested|all")
expect_help(overview ARGS --help WORDS ${commands})
expect_help(overview_h ARGS -h WORDS ${commands})
expect_help(overview_help ARGS help WORDS ${commands})
foreach(other IN ITEMS overview_h overview_help)
  if(NOT ${other} STREQUAL overview)
    fail("`--help`, `-h` and `help` print different help")
  endif()
endforeach()
# The program is named as such, not by the path it was run from.
string(FIND "${overview}" "${MAKEBELIEVE_EXE}" found)
if(NOT found EQUAL -1)
  fail("`makebelieve --help` names the program by its path")
endif()

expect_help(mount_help ARGS mount --help WORDS "-j <builds>" "--build=all"
  "--build=requested" "<mountpoint>")
expect_help(mount_h ARGS mount ../somewhere -h)
expect_help(mount_topic ARGS help mount)
if(NOT mount_h STREQUAL mount_help OR NOT mount_topic STREQUAL mount_help)
  fail("`mount --help`, `mount <mountpoint> -h` and `help mount` differ")
endif()
expect_help(unmount_help ARGS unmount --help WORDS "<mountpoint>")
expect_help(unmount_topic ARGS help unmount)
if(NOT unmount_topic STREQUAL unmount_help)
  fail("`unmount --help` and `help unmount` differ")
endif()

expect_help(manifest ARGS help manifest WORDS "build.makebelieve" "%out" "@/"
  "*" "EXAMPLE")

expect_help(version ARGS --version)
if(NOT version MATCHES "^makebelieve ([0-9]+\\.[0-9]+\\.[0-9]+)")
  fail("`makebelieve --version` printed `${version}`")
elseif(NOT CMAKE_MATCH_1 STREQUAL EXPECTED_VERSION)
  fail("`makebelieve --version` says ${CMAKE_MATCH_1}, not ${EXPECTED_VERSION}")
endif()

expect_usage_error("no command given")
expect_usage_error("unknown command `frobnicate`" frobnicate)
expect_usage_error("no help on `frobnicate`" help frobnicate)
expect_usage_error("`mount` needs a mountpoint" mount)
expect_usage_error("`mount` takes one mountpoint" mount a b)
expect_usage_error("`mount` has no option `--frobnicate`" mount --frobnicate a)
expect_usage_error("`-j` needs a number" mount a -j)
expect_usage_error("`-j` takes a positive number of builds, not `0`"
  mount -j 0 a)
expect_usage_error("`-j` was given more than once" mount -j 1 -j 2 a)
expect_usage_error("`--build=` takes `requested` or `all`, not `some`"
  mount --build=some a)
expect_usage_error("`--build` was given more than once"
  mount --build=all --build=all a)
expect_usage_error("`unmount` takes just a mountpoint" unmount)
expect_usage_error("`unmount` takes just a mountpoint" unmount a b)

# --- In step with DOCS.md ----------------------------------------------------

file(READ "${DOCS_FILE}" docs)
string(TOLOWER "${manifest}" manifest_lower)
set(all_help "${overview}${mount_help}${unmount_help}${manifest}")

# Every section of DOCS.md has its counterpart.
string(REGEX MATCHALL "\n## [^\n]+" headings "${docs}")
foreach(heading IN LISTS headings)
  string(REGEX REPLACE "^\n## " "" heading "${heading}")
  string(TOLOWER "${heading}" heading)
  string(FIND "${manifest_lower}" "${heading}" found)
  if(found EQUAL -1)
    fail("`help manifest` has no section on DOCS.md's `${heading}`")
  endif()
endforeach()

# Each action DOCS.md assigns an output - `@/x = <action> ...` - is shown, and
# the help shows none DOCS.md does not.
function(actions_in text result)
  string(REGEX MATCHALL "@/[^ \n`]* *= *[a-z]+" assignments "${text}")
  set(actions "")
  foreach(assignment IN LISTS assignments)
    string(REGEX REPLACE ".*= *" "" action "${assignment}")
    list(APPEND actions "${action}")
  endforeach()
  list(REMOVE_DUPLICATES actions)
  set(${result} "${actions}" PARENT_SCOPE)
endfunction()
actions_in("${docs}" docs_actions)
actions_in("${manifest}" help_actions)
if(NOT docs_actions)
  fail("found no actions in DOCS.md")
endif()
foreach(action IN LISTS docs_actions)
  if(NOT action IN_LIST help_actions)
    fail("`help manifest` shows no `@/... = ${action}` from DOCS.md")
  endif()
endforeach()
foreach(action IN LISTS help_actions)
  if(NOT action IN_LIST docs_actions)
    fail("`help manifest` shows `= ${action}`, which DOCS.md never does")
  endif()
endforeach()

# Every `%` variable DOCS.md uses, and every option it names, is mentioned.
string(REGEX MATCHALL "%[a-z]+|--[a-z]+(=[a-z]+)?| -j " names "${docs}")
list(REMOVE_DUPLICATES names)
foreach(name IN LISTS names)
  string(STRIP "${name}" name)
  string(FIND "${all_help}" "${name}" found)
  if(found EQUAL -1)
    fail("no help mentions `${name}`, which DOCS.md does")
  endif()
endforeach()

if(FAILURES)
  message(FATAL_ERROR "makebelieve's help is wrong:\n${FAILURES}")
endif()
message(STATUS "makebelieve's help is complete")
