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
# install rules for a subproject
file(STRINGS "${OPENFX_PATH}/cmake/OpenFX.cmake" _ofx_has_api REGEX "^function\\(ofx_target_filter_missing_includes")
if(NOT _ofx_has_api)
  message(FATAL_ERROR "${OPENFX_PATH}/cmake/OpenFX.cmake has no ofx_target_filter_missing_includes(): update the openfx submodule "
                      "(git submodule update --init --recursive)")
endif()
add_subdirectory("${OPENFX_PATH}" "${CMAKE_BINARY_DIR}/openfx")

# supportext_add_sources() (after add_subdirectory(openfx), so that it links openfx::Support)
include("${OFXS_EXT_PATH}/cmake/SupportExt.cmake")
