# SPDX-License-Identifier: BSD-3-Clause
#
# The shared OFX plug-in CMake API (NatronGitHub/Natron#1181) for openfx-io:
#
#   add_ofx_plugin() (with RPATH), ofx_plugin_resources(), ofx_target_openmp(), ofx_deploy_bundle(),
#   ofx_target_filter_missing_includes(), OFX_PLUGIN_RPATH, ARCHDIR, the targets
#   openfx::Api and openfx::Support, and the cache variables OFX_CXX_STANDARD, OFX_USE_OPENMP, OFX_DEPLOY_BUNDLES,
#   PLUGIN_INSTALLDIR: from add_subdirectory(openfx) (openfx/cmake/OpenFX.cmake).
#
#   supportext_add_sources(): from SupportExt/cmake/SupportExt.cmake.
#
# Input variables: OPENFX_PATH (the openfx source tree, default ${CMAKE_SOURCE_DIR}/openfx) and OFXS_EXT_PATH
# (openfx-supportext, default ${CMAKE_SOURCE_DIR}/SupportExt).

include_guard(GLOBAL)

if(NOT OPENFX_PATH)
  set(OPENFX_PATH "${CMAKE_SOURCE_DIR}/openfx")
endif()
if(NOT OFXS_EXT_PATH)
  set(OFXS_EXT_PATH "${CMAKE_SOURCE_DIR}/SupportExt")
endif()

# openfx's plug-in API (NatronGitHub/openfx#125, and #127 for add_ofx_plugin(RPATH) and
# ofx_target_filter_missing_includes(), which this checks): openfx::Api, openfx::Support and OpenFX.cmake; no HostSupport and no
# install rules for a subproject (they are gated on OFX_INSTALL, which is OFF under add_subdirectory(), openfx#128).
# EXCLUDE_FROM_ALL: openfx's targets are built only when something needs them, e.g. not the example hosts (hostDemo,
# cacheDemo, propsetDump, suiteHandleChecks) that BUILD_EXAMPLE_HOSTS adds, nor the ofxh module of OFX_PYTHON_BINDINGS
# unless IO_HOSTDEMO_TESTS asks for it.
file(STRINGS "${OPENFX_PATH}/cmake/OpenFX.cmake" _ofx_has_api REGEX "^function\\(ofx_target_filter_missing_includes")
if(NOT _ofx_has_api)
  message(FATAL_ERROR "${OPENFX_PATH}/cmake/OpenFX.cmake has no ofx_target_filter_missing_includes(): update the openfx submodule "
                      "(git submodule update --init --recursive)")
endif()
# openfx's targets that don't set their C++ standard (HostSupport's OfxHost, the ofxh module of OFX_PYTHON_BINDINGS and
# nanobind, which only require C++17 as a minimum) are built as C++17 without GNU extensions: openfx's own MSYS2 CI
# builds the ofxh module that way (-DCMAKE_CXX_STANDARD=17 -DCMAKE_CXX_STANDARD_REQUIRED=ON -DCMAKE_CXX_EXTENSIONS=OFF),
# while at the compiler's default standard (gnu++20 with GCC 16), linking the module failed with MSYS2's GCC 16.2
# (undefined std::__cxx11::basic_string move constructor, from ofxhPythonAdapters.cpp). io_reader_window, which links
# OfxHost, built at gnu++20 there, so the failure is specific to the module. Not OFX_CXX_STANDARD: the plug-ins and
# openfx::Support set their own CXX_STANDARD to it (add_ofx_plugin()), and C++17 is the tested configuration of these.
# CMAKE_CXX_STANDARD* are set for add_subdirectory(openfx) only: the targets of openfx-io set their own standard (the
# plug-ins through add_ofx_plugin(), the tests in tests/CMakeLists.txt, io_reader_window and io_verify_plugin_loads in
# CMakeLists.txt; C++17 for those that link OfxHost).
foreach(_v CMAKE_CXX_STANDARD CMAKE_CXX_STANDARD_REQUIRED CMAKE_CXX_EXTENSIONS)
  if(DEFINED ${_v})
    set(_io_saved_${_v} "${${_v}}")
  else()
    unset(_io_saved_${_v})
  endif()
endforeach()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_subdirectory("${OPENFX_PATH}" "${CMAKE_BINARY_DIR}/openfx" EXCLUDE_FROM_ALL)
foreach(_v CMAKE_CXX_STANDARD CMAKE_CXX_STANDARD_REQUIRED CMAKE_CXX_EXTENSIONS)
  if(DEFINED _io_saved_${_v})
    set(${_v} "${_io_saved_${_v}}")
  else()
    unset(${_v})
  endif()
endforeach()
# OpenFX.cmake names the bundle folder of macOS, MS Windows, Linux and FreeBSD only: elsewhere, its ARCHDIR is
# "unknown-arch", in which no host looks for the plug-in binary (#106)
if(ARCHDIR STREQUAL "unknown-arch")
  message(FATAL_ERROR "No OFX bundle architecture folder (ARCHDIR) for ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR}): "
                      "openfx/cmake/OpenFX.cmake supports macOS, MS Windows, Linux and FreeBSD")
endif()

# supportext_add_sources() (after add_subdirectory(openfx), so that it links openfx::Support)
include("${OFXS_EXT_PATH}/cmake/SupportExt.cmake")
