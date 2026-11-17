# SPDX-License-Identifier: BSD-3-Clause
#
# The shared OFX plug-in CMake API (NatronGitHub/Natron#1181) for openfx-io:
#
#   add_ofx_plugin(), ofx_plugin_resources(), ofx_target_openmp(), ofx_deploy_bundle(), ARCHDIR, the targets
#   openfx::Api and openfx::Support, and the cache variables OFX_CXX_STANDARD, OFX_USE_OPENMP, OFX_DEPLOY_BUNDLES,
#   PLUGIN_INSTALLDIR: from add_subdirectory(openfx) if the openfx submodule has them (NatronGitHub/openfx#125);
#   otherwise from cmake/OpenFXStandin.cmake, an unchanged copy of #125's OpenFX.cmake, with local openfx::Api and
#   openfx::Support targets like #125's. Drop the stand-in once the openfx submodule is updated.
#
#   supportext_add_sources(): a local stand-in, see below.
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

# supportext_add_sources(<target> [ALL] [OPENGL] [EGL] [COMPONENTS ...]): a local stand-in, with the names of
# SupportExt/cmake/SupportExt.cmake (openfx-supportext#45) and the narrower OPENGL of its upcoming correction
# (glad, ofxsOGLUtilities, and glad/gladegl.cpp on Linux), because the merged helper has known bugs. Set
# OFX_IO_USE_SUPPORTEXT_HELPER=ON to use SupportExt's own helper once the corrected one is in the submodule.
option(OFX_IO_USE_SUPPORTEXT_HELPER "Use SupportExt/cmake/SupportExt.cmake instead of the stand-in in cmake/OpenFXLocal.cmake" OFF)
if(OFX_IO_USE_SUPPORTEXT_HELPER)
  include("${OFXS_EXT_PATH}/cmake/SupportExt.cmake")
endif()
if(NOT COMMAND supportext_add_sources)
  set_property(GLOBAL PROPERTY SUPPORTEXT_DIR "${OFXS_EXT_PATH}")
  # component: sources, then dependencies
  set(_SX_THREADS            tinythread.cpp ofxsThreadSuite.cpp)
  set(_SX_FILEOPEN           ofxsFileOpen.cpp)
  set(_SX_LUT                ofxsLut.cpp)
  set(_SX_MULTIPLANE         ofxsMultiPlane.cpp)
  set(_SX_SHUTTER            ofxsShutter.cpp)
  set(_SX_TRANSFORM3X3       ofxsTransform3x3.cpp)
  set(_SX_TRANSFORM3X3_DEPS  SHUTTER)
  set(_SX_TRANSFORM_INTERACT ofxsTransformInteract.cpp)
  set(_SX_RECTANGLE_INTERACT ofxsRectangleInteract.cpp)
  set(_SX_GENERATOR          ofxsGenerator.cpp)
  set(_SX_GENERATOR_DEPS     RECTANGLE_INTERACT)
  set(_SX_TEXT               ofxsOGLTextRenderer.cpp ofxsOGLFontData.cpp)
  set(_SX_RAMP               ofxsRamp.cpp)
  set(_SX_RAMP_DEPS          TEXT)
  set(_SX_TRACKING           ofxsTracking.cpp)
  set(_SX_TRACKING_DEPS      TEXT)
  set(_SX_OGL_FUNCTIONS      ofxsOGLFunctions_gl.cpp glad/glad.cpp)
  set(_SX_EGL                glad/gladegl.cpp)
  set(_SX_OPENGL             glad/glad.cpp ofxsOGLUtilities.cpp)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_SX_OPENGL_DEPS      EGL)
  endif()
  set(_SX_COMPONENTS THREADS FILEOPEN LUT MULTIPLANE SHUTTER TRANSFORM3X3 TRANSFORM_INTERACT RECTANGLE_INTERACT
      GENERATOR TEXT RAMP TRACKING OGL_FUNCTIONS EGL OPENGL)

  function(supportext_add_sources TARGET)
    cmake_parse_arguments(SX "ALL;OPENGL;OSMESA;EGL" "" "COMPONENTS" ${ARGN})
    get_property(_dir GLOBAL PROPERTY SUPPORTEXT_DIR)
    if(SX_UNPARSED_ARGUMENTS)
      message(FATAL_ERROR "supportext_add_sources: unknown arguments: ${SX_UNPARSED_ARGUMENTS}")
    endif()
    if(SX_OSMESA)
      message(FATAL_ERROR "supportext_add_sources: OSMESA is not supported by the stand-in in cmake/OpenFXLocal.cmake")
    endif()
    set(_todo ${SX_COMPONENTS})
    if(SX_ALL)
      list(APPEND _todo ${_SX_COMPONENTS})
    endif()
    foreach(_opt OPENGL EGL)
      if(SX_${_opt})
        list(APPEND _todo ${_opt})
      endif()
    endforeach()
    set(_done "")
    while(_todo)
      list(GET _todo 0 _c)
      list(REMOVE_AT _todo 0)
      string(TOUPPER "${_c}" _c)
      if(NOT _c IN_LIST _SX_COMPONENTS)
        message(FATAL_ERROR "supportext_add_sources: unknown component ${_c} (known: ${_SX_COMPONENTS})")
      endif()
      if(NOT _c IN_LIST _done)
        list(APPEND _done ${_c})
        list(APPEND _todo ${_SX_${_c}_DEPS})
      endif()
    endwhile()
    # each source once per target, whatever the calls and the directory they come from
    get_property(_added TARGET ${TARGET} PROPERTY SUPPORTEXT_SOURCES)
    set(_sources "")
    foreach(_c ${_done})
      foreach(_s ${_SX_${_c}})
        if(NOT "${_dir}/${_s}" IN_LIST _added AND NOT "${_dir}/${_s}" IN_LIST _sources)
          list(APPEND _sources "${_dir}/${_s}")
        endif()
      endforeach()
    endforeach()
    if(_sources)
      set_property(TARGET ${TARGET} APPEND PROPERTY SUPPORTEXT_SOURCES ${_sources})
      target_sources(${TARGET} PRIVATE ${_sources})
    endif()
    get_property(_setup TARGET ${TARGET} PROPERTY SUPPORTEXT_SETUP)
    if(NOT _setup)
      set_property(TARGET ${TARGET} PROPERTY SUPPORTEXT_SETUP ON)
      target_include_directories(${TARGET} PRIVATE "${_dir}" "${_dir}/glad")
    endif()
    # OpenGL::GL and openfx::Support are linked by the caller (CMakeLists.txt)
  endfunction()
endif()


# openfx's own API, if it has it
file(STRINGS "${OPENFX_PATH}/cmake/OpenFX.cmake" _ofx_has_api REGEX "^function\\(ofx_plugin_resources")
if(_ofx_has_api)
  # openfx::Api, openfx::Support and OpenFX.cmake; no HostSupport and no install rules for a subproject
  add_subdirectory("${OPENFX_PATH}" "${CMAKE_BINARY_DIR}/openfx")
  return()
endif()
message(STATUS "openfx/cmake/OpenFX.cmake has no ofx_plugin_resources(): using cmake/OpenFXStandin.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/OpenFXStandin.cmake")
# it finds Info.plist.in and osxDeploy.sh in the openfx tree
set(_OFX_CMAKE_MODULE_DIR "${OPENFX_PATH}/cmake" CACHE INTERNAL "")
if(ARCHDIR STREQUAL "unknown-arch")
  message(FATAL_ERROR "OFX bundle architecture folder not detected, see https://openfx.readthedocs.io/en/main/Reference/ofxPackaging.html")
endif()

# openfx::Api and openfx::Support, as openfx#125 defines them (with openfx's default options)
if(NOT TARGET openfx::Api)
  add_library(ofxlocal_Api INTERFACE)
  target_include_directories(ofxlocal_Api INTERFACE "${OPENFX_PATH}/include")
  target_compile_definitions(ofxlocal_Api INTERFACE
    OFX_SUPPORTS_OPENGLRENDER OFX_EXTENSIONS_NATRON OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_VEGAS OFX_EXTENSIONS_TUTTLE
    $<$<PLATFORM_ID:Darwin>:GL_SILENCE_DEPRECATION>
    "$<$<AND:$<PLATFORM_ID:Windows>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:_USE_MATH_DEFINES;__USE_MINGW_ANSI_STDIO=1>")
  target_compile_options(ofxlocal_Api INTERFACE "$<$<CXX_COMPILER_ID:MSVC>:/Zc:__cplusplus>")
  add_library(openfx::Api ALIAS ofxlocal_Api)
endif()
if(NOT TARGET openfx::Support)
  # Makefile.master's SUPPORTOBJECTS (#125 also compiles ofxsHWNDInteract.cpp, empty except on Windows)
  set(_ofx_support_sources)
  foreach(_f ofxsMultiThread ofxsInteract ofxsProperty ofxsLog ofxsCore ofxsPropertyValidation ofxsImageEffect ofxsParams)
    list(APPEND _ofx_support_sources "${OPENFX_PATH}/Support/Library/${_f}.cpp")
  endforeach()
  add_library(ofxlocal_Support STATIC ${_ofx_support_sources})
  set_target_properties(ofxlocal_Support PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    CXX_STANDARD ${OFX_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  target_compile_definitions(ofxlocal_Support PRIVATE $<$<CONFIG:Debug>:DEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:_HAS_AUTO_PTR_ETC> $<$<CXX_COMPILER_ID:MSVC>:NOMINMAX>)
  target_include_directories(ofxlocal_Support PUBLIC
    "${OPENFX_PATH}/Support/include" "${OPENFX_PATH}/Support/Plugins/include")
  target_link_libraries(ofxlocal_Support PUBLIC openfx::Api)
  add_library(openfx::Support ALIAS ofxlocal_Support)
endif()
