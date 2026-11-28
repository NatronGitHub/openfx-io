# ctest helper for the tests of tests/CMakeLists.txt that need an empty work directory (`rm -rf <dir> && mkdir -p
# <dir>`):
#
#   cmake -DDIR=<dir> -P run_test.cmake -- <program> [<arguments>...]
#
# removes DIR, creates it, then runs the program, and fails if the program fails (or doesn't exit). Every non-zero exit
# status fails, also 77: cmake -P can't exit with the program's status (cmake_language(EXIT) needs CMake 3.29), so a
# test run by this script can't use ctest's SKIP_RETURN_CODE.

set(_cmd "")
set(_found OFF)
math(EXPR _last "${CMAKE_ARGC} - 1")
foreach(_i RANGE ${_last})
  if(_found)
    list(APPEND _cmd "${CMAKE_ARGV${_i}}")
  elseif("${CMAKE_ARGV${_i}}" STREQUAL "--")
    set(_found ON)
  endif()
endforeach()
if(NOT _cmd)
  message(FATAL_ERROR "usage: cmake -DDIR=<dir> -P run_test.cmake -- <program> [<arguments>...]")
endif()
if(DIR)
  file(REMOVE_RECURSE "${DIR}")
  file(MAKE_DIRECTORY "${DIR}")
endif()
execute_process(COMMAND ${_cmd} RESULT_VARIABLE _status)
if(_status EQUAL 77)
  message(FATAL_ERROR "${_cmd}: exit status 77 (skipped), which run_test.cmake can't pass to ctest: a failure")
elseif(NOT _status EQUAL 0)
  message(FATAL_ERROR "${_cmd}: ${_status}")
endif()
