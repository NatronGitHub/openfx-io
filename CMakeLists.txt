# ***** BEGIN LICENSE BLOCK *****
# This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
# (C) 2018-2021 The Natron Developers
# (C) 2013-2018 INRIA
#
# openfx-io is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# openfx-io is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
# ***** END LICENSE BLOCK *****

# CMake build of the IO.ofx bundle. It builds what IO/Makefile builds (same plug-ins, sources, defines, include
# paths and resources), except that a group of plug-ins is left out when its libraries are not found.
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release [-DSEEXPR_HOME=...] [-DFFMPEG_PKG_CONFIG_PATH=...]
#   cmake --build build
#   cmake --install build --prefix /usr     # installs /usr/OFX/Plugins/IO.ofx.bundle (PLUGIN_INSTALLDIR)
#
# The bundle is also assembled in build/Plugins/IO.ofx.bundle. Options:
#   SEEXPR_HOME             SeExpr 2.11 prefix (SeExpr 1 API: include/SeExpression.h, lib/libSeExpr)
#   FFMPEG_PKG_CONFIG_PATH  pkg-config directory of the FFmpeg to use (e.g. /opt/local/libexec/ffmpeg8/lib/pkgconfig)
#   OFX_DEPLOY_BUNDLES      install step that makes the bundle self-contained (macOS: osxDeploy.sh, like the
#                           Makefile's release builds)
#   BUILD_TESTING           ctest runs the tests of tests/Makefile (IO_TESTS_MAKE_ARGS: extra make arguments)
# The C++ standard is OFX_CXX_STANDARD (default: CMAKE_CXX_STANDARD, or 17).

cmake_minimum_required(VERSION 3.16...3.31)
project(OpenFXIO CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Prevented in-tree built. This is bad practice.")
endif()

# Check that the submodules have been initialized and updated
foreach(_sub openfx/include SupportExt/ofxsMacros.h IOSupport/SequenceParsing/SequenceParsing.h)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${_sub}")
    message(FATAL_ERROR "\n submodule(s) are missing, please update your repository:\n"
                        "  > git submodule update -i --recursive\n")
  endif()
endforeach()

# Makefile.master: -O2 for release builds (CMake's default is -O3)
if(NOT MSVC)
  string(REPLACE "-O3" "-O2" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

set(OPENFX_PATH "${PROJECT_SOURCE_DIR}/openfx")
set(OFXS_EXT_PATH "${PROJECT_SOURCE_DIR}/SupportExt")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" "${PROJECT_SOURCE_DIR}/IOSupport/cmake")
# openfx's add_ofx_plugin() & co. (or a stand-in until the openfx submodule has them) and SupportExt's helper
include(OpenFXLocal)
include(IOSupport)

if(OFX_CXX_STANDARD LESS 17 OR OFX_CXX_STANDARD EQUAL 98)
  message(FATAL_ERROR "OFX_CXX_STANDARD=${OFX_CXX_STANDARD}: openfx-io needs C++17 or later (OpenImageIO 3.x, OpenColorIO 2.5)")
endif()

message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}, bundle folder ${ARCHDIR}, C++${OFX_CXX_STANDARD}")

# ---------------------------------------------------------------------------------------------------------------
# Dependencies. Each one gives an imported target; a group of plug-ins is only built if its libraries are found.

# Makefile.master links -lGL (Linux, FreeBSD), -framework OpenGL, -lopengl32: the legacy libGL, not GLVND's
# libOpenGL + libGLX
if(NOT DEFINED OpenGL_GL_PREFERENCE)
  set(OpenGL_GL_PREFERENCE LEGACY)
endif()
find_package(OpenGL REQUIRED)
find_package(PkgConfig)

# pkg-config with a different PKG_CONFIG_PATH: forget the cached results when the path changes
macro(_io_pkg_config_path prefix path)
  if(NOT "${path}" STREQUAL "${_IO_PKG_CONFIG_PATH_${prefix}}")
    unset(__pkg_config_checked_${prefix} CACHE)
    get_cmake_property(_io_vars CACHE_VARIABLES)
    foreach(_v ${_io_vars})
      if(_v MATCHES "^pkgcfg_lib_${prefix}_")
        unset(${_v} CACHE)
      endif()
    endforeach()
    set(_IO_PKG_CONFIG_PATH_${prefix} "${path}" CACHE INTERNAL "")
  endif()
endmacro()

# OpenColorIO: OpenColorIO::OpenColorIO
find_package(OpenColorIO CONFIG QUIET)
if(NOT TARGET OpenColorIO::OpenColorIO AND PKG_CONFIG_FOUND)
  pkg_check_modules(OCIO_PC QUIET IMPORTED_TARGET OpenColorIO)
  if(OCIO_PC_FOUND)
    add_library(OpenColorIO::OpenColorIO INTERFACE IMPORTED)
    target_link_libraries(OpenColorIO::OpenColorIO INTERFACE PkgConfig::OCIO_PC)
  endif()
endif()

# OpenEXR and Imath: OpenEXR::OpenEXR (which links Imath::Imath)
find_package(OpenEXR CONFIG QUIET)
if(NOT TARGET OpenEXR::OpenEXR AND PKG_CONFIG_FOUND)
  pkg_check_modules(OPENEXR_PC QUIET IMPORTED_TARGET OpenEXR)
  if(OPENEXR_PC_FOUND)
    # OpenEXR 3 needs Imath, OpenEXR 2 IlmBase
    pkg_check_modules(IMATH_PC QUIET IMPORTED_TARGET Imath)
    if(NOT IMATH_PC_FOUND)
      pkg_check_modules(IMATH_PC QUIET IMPORTED_TARGET IlmBase)
    endif()
    add_library(OpenEXR::OpenEXR INTERFACE IMPORTED)
    target_link_libraries(OpenEXR::OpenEXR INTERFACE PkgConfig::OPENEXR_PC $<TARGET_NAME_IF_EXISTS:PkgConfig::IMATH_PC>)
  endif()
endif()

# OpenImageIO: OpenImageIO::OpenImageIO
find_package(OpenImageIO CONFIG QUIET)
if(NOT TARGET OpenImageIO::OpenImageIO AND PKG_CONFIG_FOUND)
  pkg_check_modules(OIIO_PC QUIET IMPORTED_TARGET OpenImageIO)
  if(OIIO_PC_FOUND)
    add_library(OpenImageIO::OpenImageIO INTERFACE IMPORTED)
    target_link_libraries(OpenImageIO::OpenImageIO INTERFACE PkgConfig::OIIO_PC)
  endif()
endif()

# LibRaw (thread-safe libraw_r), for ReadOIIO's RAW options: PkgConfig::LIBRAW
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw_r)
endif()

# libpng and zlib (ReadPNG and WritePNG use both): PkgConfig::LIBPNG, or CMake's PNG::PNG
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBPNG QUIET IMPORTED_TARGET libpng)
  pkg_check_modules(ZLIB_PC QUIET IMPORTED_TARGET zlib)
endif()
if(LIBPNG_FOUND)
  add_library(io_png INTERFACE)
  if(ZLIB_PC_FOUND)
    target_link_libraries(io_png INTERFACE PkgConfig::LIBPNG PkgConfig::ZLIB_PC)
  else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(io_png INTERFACE PkgConfig::LIBPNG ZLIB::ZLIB)
  endif()
else()
  find_package(PNG QUIET)
  if(TARGET PNG::PNG)
    add_library(io_png INTERFACE)
    target_link_libraries(io_png INTERFACE PNG::PNG)
  endif()
endif()

# FFmpeg: PkgConfig::FFMPEG. FFMPEG_PKG_CONFIG_PATH selects another FFmpeg, as in Makefile.master; the imported
# target links the libraries by full path, so another -L on the link line cannot pick a different FFmpeg.
set(FFMPEG_PKG_CONFIG_PATH "" CACHE PATH
    "pkg-config directory of FFmpeg (e.g. /opt/local/libexec/ffmpeg8/lib/pkgconfig for MacPorts' ffmpeg8); empty: the default search path")
if(PKG_CONFIG_FOUND)
  _io_pkg_config_path(FFMPEG "${FFMPEG_PKG_CONFIG_PATH}")
  set(_io_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
  if(FFMPEG_PKG_CONFIG_PATH)
    if(CMAKE_HOST_WIN32)
      set(ENV{PKG_CONFIG_PATH} "${FFMPEG_PKG_CONFIG_PATH};$ENV{PKG_CONFIG_PATH}")
    else()
      set(ENV{PKG_CONFIG_PATH} "${FFMPEG_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
    endif()
  endif()
  pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
  set(ENV{PKG_CONFIG_PATH} "${_io_saved_pkg_config_path}")
endif()

# macOS guard (Makefile.master): if OpenImageIO and the plug-ins link two different FFmpeg builds with the same
# file name (e.g. MacPorts' ffmpeg8 in /opt/local/libexec/ffmpeg8 and Natron's ffmpeg in /opt/local/lib, both
# libavcodec.62.dylib), osxDeploy.sh, which copies libraries by name, silently bundles only one of them. Fail instead.
if(APPLE AND FFMPEG_FOUND AND TARGET OpenImageIO::OpenImageIO)
  set(_io_oiio_lib "")
  get_target_property(_io_oiio_type OpenImageIO::OpenImageIO TYPE)
  if(NOT _io_oiio_type STREQUAL "INTERFACE_LIBRARY")
    get_target_property(_io_oiio_lib OpenImageIO::OpenImageIO LOCATION)
  elseif(PKG_CONFIG_FOUND)
    pkg_get_variable(_io_oiio_libdir OpenImageIO libdir)
    set(_io_oiio_lib "${_io_oiio_libdir}/libOpenImageIO.dylib")
  endif()
  set(_io_ffmpeg_avcodec "")
  foreach(_l ${FFMPEG_LINK_LIBRARIES})
    if(_l MATCHES "/libavcodec[^/]*\\.dylib$")
      set(_io_ffmpeg_avcodec "${_l}")
    endif()
  endforeach()
  if(_io_oiio_lib AND EXISTS "${_io_oiio_lib}" AND _io_ffmpeg_avcodec)
    execute_process(COMMAND otool -L "${_io_oiio_lib}" OUTPUT_VARIABLE _io_out ERROR_QUIET)
    string(REGEX MATCH "[^ \t\n]*libavcodec[^ \t\n]*" _io_oiio_avcodec "${_io_out}")
    # the plug-ins' libavcodec install name, e.g. /opt/local/lib/libavcodec.62.dylib
    execute_process(COMMAND otool -D "${_io_ffmpeg_avcodec}" OUTPUT_VARIABLE _io_out ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(REGEX REPLACE "^.*\n" "" _io_ffmpeg_avcodec_id "${_io_out}")
    if(_io_oiio_avcodec AND _io_ffmpeg_avcodec_id)
      get_filename_component(_io_n1 "${_io_oiio_avcodec}" NAME)
      get_filename_component(_io_n2 "${_io_ffmpeg_avcodec_id}" NAME)
      if(_io_n1 STREQUAL _io_n2 AND NOT _io_oiio_avcodec STREQUAL _io_ffmpeg_avcodec_id)
        get_filename_component(_io_hint "${_io_oiio_avcodec}" DIRECTORY)
        message(FATAL_ERROR "OpenImageIO links ${_io_oiio_avcodec}, but the FFmpeg plug-ins would link "
          "${_io_ffmpeg_avcodec_id}: both are ${_io_n1}, so the bundle would contain only one of them. Use the "
          "same FFmpeg for both, e.g. -DFFMPEG_PKG_CONFIG_PATH=${_io_hint}/pkgconfig, "
          "or build OpenImageIO without FFmpeg.")
      endif()
    endif()
  endif()
endif()

# SeExpr: the SeExpr 1 API of SeExpr 2.11 (SeExpression.h, libSeExpr), not SeExpr2 (SeExpr2/Expression.h).
# SEEXPR_HOME is its prefix, as in Makefile.master; empty: CMake's default search paths.
set(SEEXPR_HOME "" CACHE PATH "SeExpr 2.11 prefix (include/SeExpression.h, lib/libSeExpr)")
if(NOT "${SEEXPR_HOME}" STREQUAL "${_IO_SEEXPR_HOME}")
  unset(SEEXPR_INCLUDE_DIR CACHE)
  unset(SEEXPR_LIBRARY CACHE)
  set(_IO_SEEXPR_HOME "${SEEXPR_HOME}" CACHE INTERNAL "")
endif()
if(SEEXPR_HOME)
  find_path(SEEXPR_INCLUDE_DIR SeExpression.h PATHS "${SEEXPR_HOME}/include" NO_DEFAULT_PATH)
  find_library(SEEXPR_LIBRARY NAMES SeExpr PATHS "${SEEXPR_HOME}/lib" NO_DEFAULT_PATH)
else()
  find_path(SEEXPR_INCLUDE_DIR SeExpression.h)
  find_library(SEEXPR_LIBRARY NAMES SeExpr)
endif()
if(SEEXPR_INCLUDE_DIR AND SEEXPR_LIBRARY)
  add_library(SeExpr::SeExpr UNKNOWN IMPORTED)
  set_target_properties(SeExpr::SeExpr PROPERTIES
    IMPORTED_LOCATION "${SEEXPR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SEEXPR_INCLUDE_DIR}")
endif()

# ---------------------------------------------------------------------------------------------------------------
# The IO.ofx bundle (IO/Makefile)

add_ofx_plugin(IO INFO_PLIST IO/Info.plist)
target_link_libraries(IO PRIVATE openfx::Support)
# no IO_EXPORTS define (the Makefiles have none; OfxExport does the exporting)
set_target_properties(IO PROPERTIES DEFINE_SYMBOL "")
if(UNIX AND NOT APPLE)
  # as Makefile.master, which adds -rpath for the libdirs of OpenColorIO, OpenImageIO, LibRaw, SeExpr and libpng
  set_target_properties(IO PROPERTIES INSTALL_RPATH_USE_LINK_PATH ON)
endif()

# Include paths of IO/Makefile and Makefile.master (which also has openfx/Support/Plugins/include, for
# ofxsProcessing.H)
target_include_directories(IO PRIVATE
  EXR FFmpeg OIIO PFM OCIO SeExpr PNG "${OPENFX_PATH}/Support/Plugins/include")

# Always built: SupportExt, IOSupport and PFM (IO/Makefile's PLUGINOBJECTS and OCIO_OPENGL_OBJS).
# The plug-in sources are added at the end, in the order of IO/Makefile's PLUGINOBJECTS, which is the order of the
# plug-ins in the bundle.
# SupportExt: IO/Makefile's objects (ofxsGenerator and the text renderer are in IO/Makefile, built on every OS,
# like glad/gladegl.cpp)
supportext_add_sources(IO OPENGL EGL COMPONENTS THREADS FILEOPEN MULTIPLANE GENERATOR TEXT)
# GenericReader, GenericWriter, GenericOCIO (+ OpenGL; OFX_IO_USING_OCIO with OpenColorIO), SequenceParsing
ioSupport_add_sources(IO ALL)
foreach(_g SEEXPR OCIO EXR FFMPEG OIIO PNG RUNSCRIPT)
  set(_io_src_${_g} "")
endforeach()
target_link_libraries(IO PRIVATE OpenGL::GL) # as Makefile.master, also without the OpenGL sources
ofx_plugin_resources(IO
  PFM/fr.inria.openfx.ReadPFM.png PFM/fr.inria.openfx.ReadPFM.svg
  PFM/fr.inria.openfx.WritePFM.png PFM/fr.inria.openfx.WritePFM.svg)

set(_io_groups "PFM")
set(_io_missing "")

if(TARGET OpenColorIO::OpenColorIO)
  string(APPEND _io_groups " OCIO")
  # OFX_IO_USING_OCIO and OpenColorIO::OpenColorIO: from ioSupport_add_sources(IO ALL) above
  set(_io_src_OCIO
    OCIO/OCIOPluginBase.cpp OCIO/OCIOColorSpace.cpp OCIO/OCIODisplay.cpp OCIO/OCIOCDLTransform.cpp
    OCIO/OCIOFileTransform.cpp OCIO/OCIOLogConvert.cpp OCIO/OCIOLookTransform.cpp)
  ofx_plugin_resources(IO
    OCIO/fr.inria.openfx.OCIOCDLTransform.png OCIO/fr.inria.openfx.OCIOColorSpace.png
    OCIO/fr.inria.openfx.OCIODisplay.png OCIO/fr.inria.openfx.OCIOFileTransform.png
    OCIO/fr.inria.openfx.OCIOLogConvert.png OCIO/fr.inria.openfx.OCIOLookTransform.png)
else()
  string(APPEND _io_missing " OCIO(OpenColorIO)")
endif()

if(TARGET OpenEXR::OpenEXR)
  string(APPEND _io_groups " EXR")
  target_link_libraries(IO PRIVATE OpenEXR::OpenEXR)
  set(_io_src_EXR EXR/ReadEXR.cpp EXR/WriteEXR.cpp)
  ofx_plugin_resources(IO
    EXR/fr.inria.openfx.ReadEXR.png EXR/fr.inria.openfx.ReadEXR.svg
    EXR/fr.inria.openfx.WriteEXR.png EXR/fr.inria.openfx.WriteEXR.svg)
else()
  string(APPEND _io_missing " EXR(OpenEXR)")
endif()

if(FFMPEG_FOUND)
  string(APPEND _io_groups " FFmpeg")
  target_compile_definitions(IO PRIVATE OFX_IO_MT_FFMPEG)
  target_link_libraries(IO PRIVATE PkgConfig::FFMPEG)
  set(_io_src_FFMPEG FFmpeg/ReadFFmpeg.cpp FFmpeg/FFmpegFile.cpp FFmpeg/WriteFFmpeg.cpp FFmpeg/PixelFormat.cpp)
  ofx_plugin_resources(IO
    FFmpeg/fr.inria.openfx.ReadFFmpeg.png FFmpeg/fr.inria.openfx.ReadFFmpeg.svg
    FFmpeg/fr.inria.openfx.WriteFFmpeg.png FFmpeg/fr.inria.openfx.WriteFFmpeg.svg)
else()
  string(APPEND _io_missing " FFmpeg(libavformat,libavcodec,libswscale,libavutil)")
endif()

if(TARGET OpenImageIO::OpenImageIO)
  string(APPEND _io_groups " OIIO")
  supportext_add_sources(IO COMPONENTS THREADS MULTIPLANE)
  # OIIOGlobal.cpp includes <IlmThreadPool.h>: OpenEXR's include paths too, as in Makefile.master
  target_link_libraries(IO PRIVATE OpenImageIO::OpenImageIO $<TARGET_NAME_IF_EXISTS:OpenEXR::OpenEXR>)
  set(_io_src_OIIO OIIO/ReadOIIO.cpp OIIO/WriteOIIO.cpp OIIO/OIIOGlobal.cpp OIIO/OIIOText.cpp OIIO/OIIOResize.cpp)
  ofx_plugin_resources(IO
    OIIO/fr.inria.openfx.OIIOText.png OIIO/fr.inria.openfx.OIIOText.svg
    OIIO/fr.inria.openfx.ReadOIIO.png OIIO/fr.inria.openfx.ReadOIIO.svg
    OIIO/fr.inria.openfx.OIIOResize.png OIIO/fr.inria.openfx.OIIOResize.svg
    OIIO/fr.inria.openfx.WriteOIIO.png OIIO/fr.inria.openfx.WriteOIIO.svg)
  if(LIBRAW_FOUND)
    string(APPEND _io_groups " (with LibRaw ${LIBRAW_VERSION})")
    target_compile_definitions(IO PRIVATE OFX_IO_USING_LIBRAW)
    target_link_libraries(IO PRIVATE PkgConfig::LIBRAW)
    # LibRaw's headers include <omp.h> when compiled with OpenMP (_OPENMP): link OpenMP only if libraw_r.pc
    # asks for it (e.g. -fopenmp), as the Makefiles do; MacPorts' LibRaw does not.
    if("${LIBRAW_CFLAGS_OTHER};${LIBRAW_LDFLAGS_OTHER};${LIBRAW_LIBRARIES}" MATCHES "openmp|(^|;)-?l?gomp(;|$)|(^|;)-?l?omp(;|$)")
      ofx_target_openmp(IO)
    endif()
  else()
    string(APPEND _io_missing " LibRaw(libraw_r)")
  endif()
else()
  string(APPEND _io_missing " OIIO(OpenImageIO)")
endif()

if(TARGET io_png)
  string(APPEND _io_groups " PNG")
  supportext_add_sources(IO COMPONENTS FILEOPEN LUT)
  target_link_libraries(IO PRIVATE io_png)
  set(_io_src_PNG PNG/ReadPNG.cpp PNG/WritePNG.cpp)
  ofx_plugin_resources(IO
    PNG/fr.inria.openfx.ReadPNG.png PNG/fr.inria.openfx.ReadPNG.svg
    PNG/fr.inria.openfx.WritePNG.png PNG/fr.inria.openfx.WritePNG.svg)
else()
  string(APPEND _io_missing " PNG(libpng)")
endif()

if(TARGET SeExpr::SeExpr)
  string(APPEND _io_groups " SeExpr SeGrain SeNoise")
  # SeGrain and SeNoise use libSeExpr's noise functions
  supportext_add_sources(IO COMPONENTS RECTANGLE_INTERACT RAMP TRANSFORM_INTERACT)
  target_link_libraries(IO PRIVATE SeExpr::SeExpr)
  set(_io_src_SEEXPR SeExpr/SeExpr.cpp SeExpr/SeGrain.cpp SeExpr/SeNoise.cpp)
  # IO/Makefile has no SeExprSimple icons
  ofx_plugin_resources(IO SeExpr/fr.inria.openfx.SeExpr.png SeExpr/fr.inria.openfx.SeExpr.svg)
else()
  string(APPEND _io_missing " SeExpr(SeExpr 2.11, SEEXPR_HOME)")
endif()

# RunScript: only on Linux, FreeBSD and macOS, as in IO/Makefile
if(APPLE OR CMAKE_SYSTEM_NAME MATCHES "^(Linux|FreeBSD)$")
  string(APPEND _io_groups " RunScript")
  target_include_directories(IO PRIVATE RunScript)
  set(_io_src_RUNSCRIPT RunScript/RunScript.cpp)
  ofx_plugin_resources(IO RunScript/fr.inria.openfx.RunScript.png RunScript/fr.inria.openfx.RunScript.svg)
endif()

# IO/Makefile's PLUGINOBJECTS order
target_sources(IO PRIVATE
  ${_io_src_SEEXPR}
  ${_io_src_EXR} ${_io_src_FFMPEG} ${_io_src_OIIO}
  PFM/ReadPFM.cpp PFM/WritePFM.cpp
  ${_io_src_PNG} ${_io_src_OCIO} ${_io_src_RUNSCRIPT})

message(STATUS "IO.ofx plug-ins: ${_io_groups}")
if(_io_missing)
  message(STATUS "IO.ofx: not building (libraries not found):${_io_missing}")
endif()

ofx_deploy_bundle(IO)

# ---------------------------------------------------------------------------------------------------------------
# Tests: ctest runs the tests of tests/Makefile (which builds them in tests/), with this build's compiler and
# FFmpeg. IO_TESTS_MAKE_ARGS: extra make arguments, e.g. WITH_LIBRAW=1.
option(BUILD_TESTING "ctest: run the tests of tests/Makefile" ON)
if(BUILD_TESTING)
  enable_testing()
  find_program(IO_MAKE_PROGRAM NAMES gmake make mingw32-make)
  set(IO_TESTS_MAKE_ARGS "" CACHE STRING "Extra arguments for tests/Makefile (e.g. WITH_LIBRAW=1)")
  set(IO_TESTS_CXX "${CMAKE_CXX_COMPILER}" CACHE STRING "C++ compiler for tests/Makefile")
  if(IO_MAKE_PROGRAM)
    set(_io_test_env "")
    if(FFMPEG_PKG_CONFIG_PATH)
      if(CMAKE_HOST_WIN32)
        list(APPEND _io_test_env "PKG_CONFIG_PATH=${FFMPEG_PKG_CONFIG_PATH}\;$ENV{PKG_CONFIG_PATH}")
      else()
        list(APPEND _io_test_env "PKG_CONFIG_PATH=${FFMPEG_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
      endif()
    endif()
    if(APPLE AND CMAKE_OSX_SYSROOT)
      # CMake resolves the compiler to the toolchain's clang++, which needs the SDK (/usr/bin/clang++ finds it)
      list(APPEND _io_test_env "SDKROOT=${CMAKE_OSX_SYSROOT}")
    endif()
    separate_arguments(_io_make_args NATIVE_COMMAND "${IO_TESTS_MAKE_ARGS}")
    foreach(_t ffmpeg oiio ofx render layers)
      add_test(NAME tests-make-${_t}
               COMMAND "${IO_MAKE_PROGRAM}" -C "${PROJECT_SOURCE_DIR}/tests" test-${_t}
                       "CXX=${IO_TESTS_CXX}" ${_io_make_args})
      # the tests are built in the source tree: one at a time
      set_tests_properties(tests-make-${_t} PROPERTIES RESOURCE_LOCK openfx-io-tests-dir)
      if(_io_test_env)
        set_tests_properties(tests-make-${_t} PROPERTIES ENVIRONMENT "${_io_test_env}")
      endif()
    endforeach()
  else()
    message(STATUS "make not found: no tests")
  endif()
endif()
