# SPDX-License-Identifier: BSD-3-Clause
#
# The shared OFX plug-in CMake API (NatronGitHub/Natron#1181) for openfx-io:
#
#   add_ofx_plugin(<target> [DIR d] [BUNDLE_NAME n] [INFO_PLIST file] [IDENTIFIER id] [VERSION v]
#                  [EXPORT_SYMBOLS sym...] [SOURCES ...])
#   ofx_plugin_resources(<target> [DESTINATION subdir] files...)
#   ofx_target_openmp(<target> [REQUIRED])
#   ofx_deploy_bundle(<target>)
#   targets openfx::Api and openfx::Support
#   cache variables OFX_CXX_STANDARD, OFX_USE_OPENMP, OFX_DEPLOY_BUNDLES, PLUGIN_INSTALLDIR
#   supportext_add_sources(<target> [ALL] [OPENGL] [EGL] [COMPONENTS ...]) (a stand-in for SupportExt's, see below)
#
# If the openfx submodule provides them (openfx/cmake/OpenFX.cmake with ofx_plugin_resources(),
# NatronGitHub/openfx#125), they come from add_subdirectory(openfx). Otherwise, this file defines a stand-in with
# the same names and arguments, which follows openfx/Support/Plugins/Makefile.master. Drop the stand-in once the
# openfx submodule has them.
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
message(STATUS "openfx/cmake/OpenFX.cmake has no ofx_plugin_resources(): using the stand-in in cmake/OpenFXLocal.cmake")

# ---------------------------------------------------------------------------------------------------------------
# Stand-in. Cache variables

if(DEFINED CMAKE_CXX_STANDARD)
  set(_ofx_default_std ${CMAKE_CXX_STANDARD})
else()
  set(_ofx_default_std 17)
endif()
set(OFX_CXX_STANDARD ${_ofx_default_std} CACHE STRING "C++ standard of the plug-ins and of openfx::Support")
option(OFX_USE_OPENMP "ofx_target_openmp(): link OpenMP" ON)
option(OFX_DEPLOY_BUNDLES "Install step that makes bundles self-contained (macOS: osxDeploy.sh, Windows: runtime DLLs)" OFF)
set(PLUGIN_INSTALLDIR "OFX/Plugins" CACHE PATH
    "Where the .ofx.bundle directories are installed (relative: to CMAKE_INSTALL_PREFIX)")

# The bundle architecture folder (ARCHDIR) and OFX_PLUGIN_BUILD_DIR come from openfx's module, whose folder names
# match HostSupport and the Makefiles. It also sets a default PLUGIN_INSTALLDIR for openfx's examples: keep ours.
set(_ofx_plugin_installdir "${PLUGIN_INSTALLDIR}")
include("${OPENFX_PATH}/cmake/OpenFX.cmake")
set(PLUGIN_INSTALLDIR "${_ofx_plugin_installdir}")
if(ARCHDIR STREQUAL "unknown-arch")
  message(FATAL_ERROR "OFX bundle architecture folder not detected, see https://openfx.readthedocs.io/en/main/Reference/ofxPackaging.html")
endif()

# ---------------------------------------------------------------------------------------------------------------
# openfx::Api (C API headers) and openfx::Support (the C++ support library), with the usage requirements that
# Makefile.master puts in CXXFLAGS. The defines change class layouts in Support, so they are on openfx::Api.

if(NOT TARGET openfx::Api)
  add_library(ofxlocal_Api INTERFACE)
  target_include_directories(ofxlocal_Api INTERFACE "${OPENFX_PATH}/include")
  target_compile_definitions(ofxlocal_Api INTERFACE
    OFX_EXTENSIONS_VEGAS OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_NATRON OFX_SUPPORTS_OPENGLRENDER)
  if(APPLE)
    target_compile_definitions(ofxlocal_Api INTERFACE GL_SILENCE_DEPRECATION)
  endif()
  if(MINGW)
    # Makefile.master: proper snprintf, and the math constants in every C++ dialect
    target_compile_definitions(ofxlocal_Api INTERFACE _USE_MATH_DEFINES __USE_MINGW_ANSI_STDIO=1)
  endif()
  if(MSVC)
    target_compile_definitions(ofxlocal_Api INTERFACE NOMINMAX _HAS_AUTO_PTR_ETC)
    target_compile_options(ofxlocal_Api INTERFACE /Zc:__cplusplus)
  endif()
  add_library(openfx::Api ALIAS ofxlocal_Api)
endif()

if(NOT TARGET openfx::Support)
  # The objects of Makefile.master's SUPPORTOBJECTS (ofxsHWNDInteract.cpp is Windows/Vegas-only)
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
  target_compile_definitions(ofxlocal_Support PRIVATE $<$<CONFIG:Debug>:DEBUG>)
  target_include_directories(ofxlocal_Support PUBLIC
    "${OPENFX_PATH}/Support/include" "${OPENFX_PATH}/Support/Plugins/include")
  target_link_libraries(ofxlocal_Support PUBLIC openfx::Api)
  add_library(openfx::Support ALIAS ofxlocal_Support)
endif()

# ---------------------------------------------------------------------------------------------------------------
# add_ofx_plugin(<target> [DIR d] [BUNDLE_NAME n] [INFO_PLIST file] [IDENTIFIER id] [VERSION v] [SOURCES ...])
#
# Builds <BUNDLE_NAME>.ofx (default: <target>.ofx) as a loadable module, assembles
# ${OFX_PLUGIN_BUILD_DIR}/<BUNDLE_NAME>.ofx.bundle/Contents/{Info.plist,<ARCHDIR>/} in the build tree, and installs
# the bundle to PLUGIN_INSTALLDIR.
# INFO_PLIST: the bundle's Info.plist, copied through configure_file(@ONLY); default: <DIR>/Info.plist if it
# exists (DIR defaults to ${CMAKE_CURRENT_SOURCE_DIR}/<target>), else openfx's Info.plist.in template filled with
# IDENTIFIER (default org.openeffects.plugins.<name>) and VERSION (default 1.0.0).
function(add_ofx_plugin TARGET)
  cmake_parse_arguments(P "" "DIR;BUNDLE_NAME;INFO_PLIST;IDENTIFIER;VERSION" "EXPORT_SYMBOLS;SOURCES" ${ARGN})
  if(P_UNPARSED_ARGUMENTS)
    # old signature: add_ofx_plugin(<target> [<dir>])
    list(GET P_UNPARSED_ARGUMENTS 0 P_DIR)
  endif()
  if(NOT P_DIR)
    set(P_DIR "${TARGET}")
  endif()
  if(NOT IS_ABSOLUTE "${P_DIR}")
    set(P_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${P_DIR}")
  endif()
  if(NOT P_BUNDLE_NAME)
    set(P_BUNDLE_NAME "${TARGET}")
  endif()

  # A loadable module, like Makefile.master's -bundle (macOS) / -shared without soname or import library
  add_library(${TARGET} MODULE ${P_SOURCES})
  target_link_libraries(${TARGET} PRIVATE openfx::Support)

  set(_bundle "${OFX_PLUGIN_BUILD_DIR}/${P_BUNDLE_NAME}.ofx.bundle")
  set(_bindir "${_bundle}/Contents/${ARCHDIR}")
  set_target_properties(${TARGET} PROPERTIES
    OUTPUT_NAME "${P_BUNDLE_NAME}"
    PREFIX ""
    SUFFIX ".ofx"
    LIBRARY_OUTPUT_DIRECTORY "${_bindir}"
    CXX_STANDARD ${OFX_CXX_STANDARD}
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # no <target>_EXPORTS define (OfxExport does the exporting)
    DEFINE_SYMBOL ""
    # hidden visibility; only the entry points (OfxExport) are exported, see the export lists below
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    # the bundle in the build tree is usable as is (OFX_PLUGIN_PATH), with the rpath of the installed one
    BUILD_WITH_INSTALL_RPATH ON)
  foreach(_cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER "${_cfg}" _CFG)
    set_target_properties(${TARGET} PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${_bindir}")
  endforeach()

  # Exported symbols: OfxGetPlugin, OfxGetNumberOfPlugins and EXPORT_SYMBOLS, with an export list (macOS) or a
  # version script (Linux, FreeBSD, MinGW), as Makefile.master does with Support/include/{osx,linux}Symbols
  set(_symbols OfxGetPlugin OfxGetNumberOfPlugins ${P_EXPORT_SYMBOLS})
  set(_exports "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.exports")
  if(APPLE)
    list(TRANSFORM _symbols PREPEND "_")
    string(JOIN "\n" _content ${_symbols})
    file(WRITE "${_exports}" "${_content}\n")
    target_link_options(${TARGET} PRIVATE "LINKER:-exported_symbols_list,${_exports}")
  elseif(NOT MSVC)
    string(JOIN ";\n    " _content ${_symbols})
    file(WRITE "${_exports}" "OFX_1.0 {\n  global:\n    ${_content};\n  local:\n    *;\n};\n")
    target_link_options(${TARGET} PRIVATE "LINKER:--version-script=${_exports}")
  endif()

  # rpath (Makefile.master): bundled libraries first
  if(APPLE)
    set_target_properties(${TARGET} PROPERTIES INSTALL_RPATH "@loader_path/../Frameworks;@loader_path/../Libraries")
  elseif(UNIX)
    # Makefile.master: $ORIGIN/../../Libraries; the repositories' Makefiles add -rpath for the dependencies' libdirs
    set_target_properties(${TARGET} PROPERTIES
      INSTALL_RPATH "\$ORIGIN/../../Libraries"
      INSTALL_RPATH_USE_LINK_PATH ON)
  endif()

  # DEBUG for Debug builds (NDEBUG comes from CMake's release flags)
  target_compile_definitions(${TARGET} PRIVATE $<$<CONFIG:Debug>:DEBUG>)
  if(MSVC)
    target_compile_options(${TARGET} PRIVATE /bigobj)
  elseif(MINGW)
    target_compile_options(${TARGET} PRIVATE -Wa,-mbig-obj)
  endif()

  # Info.plist
  if(NOT P_INFO_PLIST AND EXISTS "${P_DIR}/Info.plist")
    set(P_INFO_PLIST "${P_DIR}/Info.plist")
  endif()
  if(P_INFO_PLIST)
    if(NOT IS_ABSOLUTE "${P_INFO_PLIST}")
      set(P_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/${P_INFO_PLIST}")
    endif()
    configure_file("${P_INFO_PLIST}" "${_bundle}/Contents/Info.plist" @ONLY)
  else()
    set(PLUGIN_NAME "${P_BUNDLE_NAME}")
    set(PLUGIN_EXE "${P_BUNDLE_NAME}.ofx")
    if(P_IDENTIFIER)
      set(BUNDLE_IDENTIFIER "${P_IDENTIFIER}")
    else()
      set(BUNDLE_IDENTIFIER "org.openeffects.plugins.${P_BUNDLE_NAME}")
    endif()
    set(BUNDLE_SIGNATURE "????")
    if(P_VERSION)
      set(PLUGIN_VERSION "${P_VERSION}")
    else()
      set(PLUGIN_VERSION "1.0.0")
    endif()
    configure_file("${OPENFX_PATH}/Examples/Info.plist.in" "${_bundle}/Contents/Info.plist")
  endif()

  set(_dest "${PLUGIN_INSTALLDIR}/${P_BUNDLE_NAME}.ofx.bundle")
  set_target_properties(${TARGET} PROPERTIES
    OFX_BUNDLE_NAME "${P_BUNDLE_NAME}"
    OFX_BUNDLE_DIR "${_bundle}"
    OFX_BUNDLE_INSTALL_DIR "${_dest}")

  install(TARGETS ${TARGET} LIBRARY DESTINATION "${_dest}/Contents/${ARCHDIR}")
  install(FILES "${_bundle}/Contents/Info.plist" DESTINATION "${_dest}/Contents")
endfunction()

# ---------------------------------------------------------------------------------------------------------------
# ofx_plugin_resources(<target> [DESTINATION subdir] files...): copy files into the bundle's Contents/Resources
# (or Contents/Resources/<subdir>), in the build tree and in the installed bundle. Can be called repeatedly.
function(ofx_plugin_resources TARGET)
  cmake_parse_arguments(R "" "DESTINATION" "" ${ARGN})
  get_target_property(_bundle ${TARGET} OFX_BUNDLE_DIR)
  get_target_property(_dest ${TARGET} OFX_BUNDLE_INSTALL_DIR)
  set(_sub "Contents/Resources")
  if(R_DESTINATION)
    set(_sub "${_sub}/${R_DESTINATION}")
  endif()
  set(_files)
  foreach(_f ${R_UNPARSED_ARGUMENTS})
    if(NOT IS_ABSOLUTE "${_f}")
      set(_f "${CMAKE_CURRENT_SOURCE_DIR}/${_f}")
    endif()
    get_filename_component(_name "${_f}" NAME)
    # configure-time copy, redone when the file changes
    configure_file("${_f}" "${_bundle}/${_sub}/${_name}" COPYONLY)
    list(APPEND _files "${_f}")
  endforeach()
  if(_files)
    install(FILES ${_files} DESTINATION "${_dest}/${_sub}")
  endif()
endfunction()

# ---------------------------------------------------------------------------------------------------------------
# ofx_target_openmp(<target> [REQUIRED]): link OpenMP::OpenMP_CXX (if OFX_USE_OPENMP). Apple clang has no OpenMP
# runtime of its own: use MacPorts' libomp (/opt/local/{include,lib}/libomp), else Homebrew's.
function(ofx_target_openmp TARGET)
  cmake_parse_arguments(O "REQUIRED" "" "" ${ARGN})
  if(NOT OFX_USE_OPENMP)
    if(O_REQUIRED)
      message(FATAL_ERROR "${TARGET} requires OpenMP, but OFX_USE_OPENMP is OFF")
    endif()
    return()
  endif()
  if(NOT TARGET OpenMP::OpenMP_CXX)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND NOT OpenMP_CXX_FLAGS)
      find_path(OFX_LIBOMP_INCLUDE_DIR omp.h NO_DEFAULT_PATH PATHS
        /opt/local/include/libomp /opt/homebrew/opt/libomp/include /usr/local/opt/libomp/include)
      find_library(OFX_LIBOMP_LIBRARY omp NO_DEFAULT_PATH PATHS
        /opt/local/lib/libomp /opt/homebrew/opt/libomp/lib /usr/local/opt/libomp/lib)
      if(OFX_LIBOMP_INCLUDE_DIR AND OFX_LIBOMP_LIBRARY)
        set(OpenMP_CXX_FLAGS "-Xpreprocessor -fopenmp -I${OFX_LIBOMP_INCLUDE_DIR}" CACHE STRING "")
        set(OpenMP_CXX_LIB_NAMES "omp" CACHE STRING "")
        set(OpenMP_omp_LIBRARY "${OFX_LIBOMP_LIBRARY}" CACHE FILEPATH "")
      endif()
    endif()
    find_package(OpenMP COMPONENTS CXX)
  endif()
  if(TARGET OpenMP::OpenMP_CXX)
    target_link_libraries(${TARGET} PRIVATE OpenMP::OpenMP_CXX)
  elseif(O_REQUIRED)
    message(FATAL_ERROR "${TARGET} requires OpenMP, which was not found")
  else()
    message(WARNING "${TARGET}: OpenMP not found")
  endif()
endfunction()

# ---------------------------------------------------------------------------------------------------------------
# ofx_deploy_bundle(<target>): if OFX_DEPLOY_BUNDLES, an install step that makes the installed bundle
# self-contained. macOS: osxDeploy.sh, as Makefile.master does for release builds (copies the non-system
# libraries into Contents/Libraries and rewrites their install names). Windows: copies the runtime DLLs next to
# the plug-in. Other systems: nothing.
function(ofx_deploy_bundle TARGET)
  if(NOT OFX_DEPLOY_BUNDLES)
    return()
  endif()
  get_target_property(_dest ${TARGET} OFX_BUNDLE_INSTALL_DIR)
  get_target_property(_name ${TARGET} OFX_BUNDLE_NAME)
  set(_code [=[
set(_ofx_bundle "@_dest@")
if(NOT IS_ABSOLUTE "${_ofx_bundle}")
  set(_ofx_bundle "${CMAKE_INSTALL_PREFIX}/${_ofx_bundle}")
endif()
set(_ofx_bundle "$ENV{DESTDIR}${_ofx_bundle}")
]=])
  if(APPLE)
    string(APPEND _code [=[
message(STATUS "osxDeploy.sh: ${_ofx_bundle}")
execute_process(COMMAND bash "@OPENFX_PATH@/Support/include/osxDeploy.sh" "${_ofx_bundle}" "@_name@.ofx"
                RESULT_VARIABLE _ofx_res)
if(NOT _ofx_res EQUAL 0)
  message(FATAL_ERROR "osxDeploy.sh failed on ${_ofx_bundle}")
endif()
]=])
  elseif(WIN32)
    # directories searched for DLLs: the compiler's (MinGW runtime) and the prefixes' bin directories
    get_filename_component(_ofx_compiler_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_dirs "${_ofx_compiler_dir}")
    foreach(_p ${CMAKE_PREFIX_PATH} ${CMAKE_SYSTEM_PREFIX_PATH})
      list(APPEND _dirs "${_p}/bin")
    endforeach()
    string(APPEND _code [=[
set(_ofx_bin "${_ofx_bundle}/Contents/@ARCHDIR@")
file(GET_RUNTIME_DEPENDENCIES
  LIBRARIES "${_ofx_bin}/@_name@.ofx"
  RESOLVED_DEPENDENCIES_VAR _ofx_deps
  UNRESOLVED_DEPENDENCIES_VAR _ofx_unresolved
  DIRECTORIES @_dirs@
  PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-"
  POST_EXCLUDE_REGEXES "[Ss][Yy][Ss][Tt][Ee][Mm]32")
foreach(_ofx_dep ${_ofx_deps})
  file(INSTALL "${_ofx_dep}" DESTINATION "${_ofx_bin}")
endforeach()
if(_ofx_unresolved)
  message(WARNING "unresolved DLLs: ${_ofx_unresolved}")
endif()
]=])
    string(REPLACE ";" "\" \"" _dirs "${_dirs}")
    set(_dirs "\"${_dirs}\"")
  else()
    return()
  endif()
  string(CONFIGURE "${_code}" _code @ONLY)
  install(CODE "${_code}")
endfunction()
