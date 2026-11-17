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

# OFX_CXX_STANDARD follows CMAKE_CXX_STANDARD (default 17), also in an existing build directory, unless it was
# set explicitly
if(DEFINED CMAKE_CXX_STANDARD)
  set(_io_std ${CMAKE_CXX_STANDARD})
else()
  set(_io_std 17)
endif()
if(NOT DEFINED CACHE{OFX_CXX_STANDARD} OR "${OFX_CXX_STANDARD}" STREQUAL "${_OFX_IO_CXX_STANDARD_AUTO}")
  set(OFX_CXX_STANDARD ${_io_std} CACHE STRING "C++ standard of the plug-ins and of openfx::Support" FORCE)
  set(_OFX_IO_CXX_STANDARD_AUTO ${_io_std} CACHE INTERNAL "")
endif()

set(OPENFX_PATH "${PROJECT_SOURCE_DIR}/openfx")
set(OFXS_EXT_PATH "${PROJECT_SOURCE_DIR}/SupportExt")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" "${PROJECT_SOURCE_DIR}/IOSupport/cmake")
# openfx's add_ofx_plugin() & co. (add_subdirectory(openfx)) and SupportExt's supportext_add_sources()
include(OpenFXLocal)
include(IOSupport)

if(OFX_CXX_STANDARD LESS 17 OR OFX_CXX_STANDARD EQUAL 98)
  message(FATAL_ERROR "OFX_CXX_STANDARD=${OFX_CXX_STANDARD}: openfx-io needs C++17 or later (OpenImageIO 3.x, OpenColorIO 2.5)")
endif()

message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}, bundle folder ${ARCHDIR}, C++${OFX_CXX_STANDARD}")

# ---------------------------------------------------------------------------------------------------------------
# Dependencies. Each one gives an imported target; a group of plug-ins is only built if its libraries are found,
# and if its OFX_IO_WITH_<group> option is ON. CMAKE_DISABLE_FIND_PACKAGE_<package> also skips the pkg-config
# fallbacks (packages: OpenColorIO OpenEXR OpenImageIO LibRaw PNG FFmpeg SeExpr).
foreach(_g OCIO EXR FFMPEG OIIO LIBRAW PNG SEEXPR RUNSCRIPT)
  option(OFX_IO_WITH_${_g} "Build the ${_g} plug-ins (LIBRAW: RAW support in ReadOIIO) if the libraries are found" ON)
endforeach()

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
if(OFX_IO_WITH_OCIO)
  find_package(OpenColorIO CONFIG QUIET)
endif()
if(OFX_IO_WITH_OCIO AND NOT CMAKE_DISABLE_FIND_PACKAGE_OpenColorIO AND NOT TARGET OpenColorIO::OpenColorIO AND PKG_CONFIG_FOUND)
  pkg_check_modules(OCIO_PC QUIET IMPORTED_TARGET OpenColorIO)
  if(OCIO_PC_FOUND)
    add_library(OpenColorIO::OpenColorIO INTERFACE IMPORTED)
    target_link_libraries(OpenColorIO::OpenColorIO INTERFACE PkgConfig::OCIO_PC)
  endif()
endif()

# OpenEXR and Imath: OpenEXR::OpenEXR (which links Imath::Imath)
# (also used by OIIOGlobal.cpp)
find_package(OpenEXR CONFIG QUIET)
if(NOT CMAKE_DISABLE_FIND_PACKAGE_OpenEXR AND NOT TARGET OpenEXR::OpenEXR AND PKG_CONFIG_FOUND)
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
if(OFX_IO_WITH_OIIO)
  find_package(OpenImageIO CONFIG QUIET)
endif()
if(OFX_IO_WITH_OIIO AND NOT CMAKE_DISABLE_FIND_PACKAGE_OpenImageIO AND NOT TARGET OpenImageIO::OpenImageIO AND PKG_CONFIG_FOUND)
  pkg_check_modules(OIIO_PC QUIET IMPORTED_TARGET OpenImageIO)
  if(OIIO_PC_FOUND)
    add_library(OpenImageIO::OpenImageIO INTERFACE IMPORTED)
    target_link_libraries(OpenImageIO::OpenImageIO INTERFACE PkgConfig::OIIO_PC)
  endif()
endif()

# LibRaw (thread-safe libraw_r), for ReadOIIO's RAW options: PkgConfig::LIBRAW
if(OFX_IO_WITH_LIBRAW AND NOT CMAKE_DISABLE_FIND_PACKAGE_LibRaw AND PKG_CONFIG_FOUND)
  pkg_check_modules(LIBRAW QUIET IMPORTED_TARGET libraw_r)
endif()

# libpng and zlib (ReadPNG and WritePNG use both): PkgConfig::LIBPNG, or CMake's PNG::PNG
if(OFX_IO_WITH_PNG AND NOT CMAKE_DISABLE_FIND_PACKAGE_PNG AND PKG_CONFIG_FOUND)
  pkg_check_modules(LIBPNG QUIET IMPORTED_TARGET libpng)
  pkg_check_modules(ZLIB_PC QUIET IMPORTED_TARGET zlib)
endif()
if(NOT OFX_IO_WITH_PNG)
  # nothing
elseif(TARGET PkgConfig::LIBPNG)
  add_library(io_png INTERFACE)
  if(TARGET PkgConfig::ZLIB_PC)
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
if(OFX_IO_WITH_FFMPEG AND NOT CMAKE_DISABLE_FIND_PACKAGE_FFmpeg AND PKG_CONFIG_FOUND)
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
if(APPLE AND TARGET PkgConfig::FFMPEG AND TARGET OpenImageIO::OpenImageIO)
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
if(NOT OFX_IO_WITH_SEEXPR OR CMAKE_DISABLE_FIND_PACKAGE_SeExpr)
  # nothing
elseif(SEEXPR_HOME)
  find_path(SEEXPR_INCLUDE_DIR SeExpression.h PATHS "${SEEXPR_HOME}/include" NO_DEFAULT_PATH)
  find_library(SEEXPR_LIBRARY NAMES SeExpr PATHS "${SEEXPR_HOME}/lib" NO_DEFAULT_PATH)
else()
  find_path(SEEXPR_INCLUDE_DIR SeExpression.h)
  find_library(SEEXPR_LIBRARY NAMES SeExpr)
endif()
if(OFX_IO_WITH_SEEXPR AND NOT CMAKE_DISABLE_FIND_PACKAGE_SeExpr AND SEEXPR_INCLUDE_DIR AND SEEXPR_LIBRARY)
  add_library(SeExpr::SeExpr UNKNOWN IMPORTED)
  set_target_properties(SeExpr::SeExpr PROPERTIES
    IMPORTED_LOCATION "${SEEXPR_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SEEXPR_INCLUDE_DIR}")
endif()

# Imported targets may list include directories that don't exist: e.g. Ubuntu 24.04's OpenImageIO config has
# /usr/include/opencv4 without libopencv-dev, which makes generation fail. Drop them, also in the targets they
# link. (openfx will provide ofx_target_filter_missing_includes())
function(_io_filter_missing_includes _t)
  string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" _t "${_t}")
  if(NOT TARGET "${_t}")
    return()
  endif()
  get_target_property(_aliased "${_t}" ALIASED_TARGET)
  if(_aliased)
    set(_t "${_aliased}")
  endif()
  get_property(_seen GLOBAL PROPERTY _IO_FILTERED_TARGETS)
  if("${_t}" IN_LIST _seen)
    return()
  endif()
  set_property(GLOBAL APPEND PROPERTY _IO_FILTERED_TARGETS "${_t}")
  get_target_property(_imported "${_t}" IMPORTED)
  if(_imported)
    foreach(_prop INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES)
      get_target_property(_dirs "${_t}" ${_prop})
      if(_dirs)
        set(_keep "")
        foreach(_d IN LISTS _dirs)
          if(_d MATCHES "\\$<" OR EXISTS "${_d}")
            list(APPEND _keep "${_d}")
          else()
            # (once per directory: several targets may list it)
            get_property(_reported GLOBAL PROPERTY _IO_REPORTED_MISSING_INCLUDES)
            if(NOT _d IN_LIST _reported)
              set_property(GLOBAL APPEND PROPERTY _IO_REPORTED_MISSING_INCLUDES "${_d}")
              message(STATUS "${_t}: ignoring the missing include directory ${_d}")
            endif()
          endif()
        endforeach()
        set_target_properties("${_t}" PROPERTIES ${_prop} "${_keep}")
      endif()
    endforeach()
  endif()
  get_target_property(_libs "${_t}" INTERFACE_LINK_LIBRARIES)
  foreach(_l IN LISTS _libs)
    _io_filter_missing_includes("${_l}")
  endforeach()
endfunction()
foreach(_t OpenColorIO::OpenColorIO OpenEXR::OpenEXR OpenImageIO::OpenImageIO PkgConfig::LIBRAW io_png
           PkgConfig::FFMPEG SeExpr::SeExpr)
  _io_filter_missing_includes(${_t})
endforeach()

# ---------------------------------------------------------------------------------------------------------------
# The IO.ofx bundle (IO/Makefile)

# install IO.ofx executable (0755) like the Makefiles: CMake's Debian/Ubuntu default installs modules 0644
set(CMAKE_INSTALL_SO_NO_EXE OFF)
add_ofx_plugin(IO INFO_PLIST IO/Info.plist)
# (add_ofx_plugin links openfx::Api only)
target_link_libraries(IO PRIVATE openfx::Support)
# no IO_EXPORTS define, which CMake adds to modules (the Makefiles have none; OfxExport does the exporting)
set_target_properties(IO PROPERTIES DEFINE_SYMBOL "")

# Include paths of IO/Makefile and Makefile.master (which also has openfx/Support/Plugins/include, for
# ofxsProcessing.H)
target_include_directories(IO PRIVATE
  EXR FFmpeg OIIO PFM OCIO SeExpr PNG "${OPENFX_PATH}/Support/Plugins/include")

# Always built: SupportExt, IOSupport and PFM (IO/Makefile's PLUGINOBJECTS and OCIO_OPENGL_OBJS).
# Resources: ofx_plugin_resources(), recording the names, so that the resources of groups that are no longer
# built are removed from the bundle in the build tree (below)
set(_io_resource_names "")
macro(io_resources)
  ofx_plugin_resources(IO ${ARGN})
  foreach(_f ${ARGN})
    get_filename_component(_n "${_f}" NAME)
    list(APPEND _io_resource_names "${_n}")
  endforeach()
endmacro()

# The plug-in sources are added at the end, in the order of IO/Makefile's PLUGINOBJECTS, which is the order of the
# plug-ins in the bundle.
# SupportExt: IO/Makefile's objects (ofxsGenerator and the text renderer are in IO/Makefile; the OpenGL ones, in
# its OCIO_OPENGL_OBJS, come with OpenColorIO below)
supportext_add_sources(IO COMPONENTS THREADS FILEOPEN MULTIPLANE GENERATOR TEXT)
# GenericReader, GenericWriter, GenericOCIO, SequenceParsing; with OpenColorIO: OFX_IO_USING_OCIO and GenericOCIO's
# OpenGL rendering (+ SupportExt's OPENGL)
if(OFX_IO_WITH_OCIO AND TARGET OpenColorIO::OpenColorIO)
  ioSupport_add_sources(IO ALL)
else()
  ioSupport_add_sources(IO ALL NO_OCIO)
endif()
foreach(_g SEEXPR OCIO EXR FFMPEG OIIO PNG RUNSCRIPT)
  set(_io_src_${_g} "")
endforeach()
target_link_libraries(IO PRIVATE OpenGL::GL) # as Makefile.master, also without the OpenGL sources
io_resources(
  PFM/fr.inria.openfx.ReadPFM.png PFM/fr.inria.openfx.ReadPFM.svg
  PFM/fr.inria.openfx.WritePFM.png PFM/fr.inria.openfx.WritePFM.svg)

set(_io_groups "PFM")
set(_io_missing "")
set(_io_disabled "")
# a group that is not built: disabled (OFX_IO_WITH_<g>=OFF), or its libraries were not found
macro(_io_not_built _g _what)
  if(OFX_IO_WITH_${_g})
    string(APPEND _io_missing " ${_what}")
  else()
    string(APPEND _io_disabled " ${_g}")
  endif()
endmacro()

if(OFX_IO_WITH_OCIO AND TARGET OpenColorIO::OpenColorIO)
  string(APPEND _io_groups " OCIO")
  # OFX_IO_USING_OCIO, OpenColorIO::OpenColorIO and the OpenGL sources (SupportExt's OPENGL: glad, gladegl,
  # ofxsOGLUtilities, as IO/Makefile's OCIO_OPENGL_OBJS): from ioSupport_add_sources(IO ALL) above
  set(_io_src_OCIO
    OCIO/OCIOPluginBase.cpp OCIO/OCIOColorSpace.cpp OCIO/OCIODisplay.cpp OCIO/OCIOCDLTransform.cpp
    OCIO/OCIOFileTransform.cpp OCIO/OCIOLogConvert.cpp OCIO/OCIOLookTransform.cpp)
  io_resources(
    OCIO/fr.inria.openfx.OCIOCDLTransform.png OCIO/fr.inria.openfx.OCIOColorSpace.png
    OCIO/fr.inria.openfx.OCIODisplay.png OCIO/fr.inria.openfx.OCIOFileTransform.png
    OCIO/fr.inria.openfx.OCIOLogConvert.png OCIO/fr.inria.openfx.OCIOLookTransform.png)
else()
  _io_not_built(OCIO "OCIO(OpenColorIO)")
endif()

if(OFX_IO_WITH_EXR AND TARGET OpenEXR::OpenEXR)
  string(APPEND _io_groups " EXR")
  target_link_libraries(IO PRIVATE OpenEXR::OpenEXR)
  set(_io_src_EXR EXR/ReadEXR.cpp EXR/WriteEXR.cpp)
  io_resources(
    EXR/fr.inria.openfx.ReadEXR.png EXR/fr.inria.openfx.ReadEXR.svg
    EXR/fr.inria.openfx.WriteEXR.png EXR/fr.inria.openfx.WriteEXR.svg)
else()
  _io_not_built(EXR "EXR(OpenEXR)")
endif()

if(OFX_IO_WITH_FFMPEG AND TARGET PkgConfig::FFMPEG)
  string(APPEND _io_groups " FFmpeg")
  target_compile_definitions(IO PRIVATE OFX_IO_MT_FFMPEG)
  target_link_libraries(IO PRIVATE PkgConfig::FFMPEG)
  # FFmpeg's include directories first (-I, before the -isystem of OpenColorIO's or OpenImageIO's prefix, which
  # may have another FFmpeg: MacPorts' /opt/local/include, while ffmpeg8 is in /opt/local/libexec/ffmpeg8/include)
  target_include_directories(IO BEFORE PRIVATE ${FFMPEG_INCLUDE_DIRS})
  set(_io_src_FFMPEG FFmpeg/ReadFFmpeg.cpp FFmpeg/FFmpegFile.cpp FFmpeg/WriteFFmpeg.cpp FFmpeg/PixelFormat.cpp)
  io_resources(
    FFmpeg/fr.inria.openfx.ReadFFmpeg.png FFmpeg/fr.inria.openfx.ReadFFmpeg.svg
    FFmpeg/fr.inria.openfx.WriteFFmpeg.png FFmpeg/fr.inria.openfx.WriteFFmpeg.svg)
else()
  _io_not_built(FFMPEG "FFmpeg(libavformat,libavcodec,libswscale,libavutil)")
endif()

if(OFX_IO_WITH_OIIO AND TARGET OpenImageIO::OpenImageIO)
  string(APPEND _io_groups " OIIO")
  # (OIIOText draws with ofxsPositionInteract.h)
  supportext_add_sources(IO COMPONENTS THREADS MULTIPLANE POSITION_INTERACT)
  # OIIOGlobal.cpp includes <IlmThreadPool.h>: OpenEXR's include paths too, as in Makefile.master
  target_link_libraries(IO PRIVATE OpenImageIO::OpenImageIO $<TARGET_NAME_IF_EXISTS:OpenEXR::OpenEXR>)
  set(_io_src_OIIO OIIO/ReadOIIO.cpp OIIO/WriteOIIO.cpp OIIO/OIIOGlobal.cpp OIIO/OIIOText.cpp OIIO/OIIOResize.cpp)
  io_resources(
    OIIO/fr.inria.openfx.OIIOText.png OIIO/fr.inria.openfx.OIIOText.svg
    OIIO/fr.inria.openfx.ReadOIIO.png OIIO/fr.inria.openfx.ReadOIIO.svg
    OIIO/fr.inria.openfx.OIIOResize.png OIIO/fr.inria.openfx.OIIOResize.svg
    OIIO/fr.inria.openfx.WriteOIIO.png OIIO/fr.inria.openfx.WriteOIIO.svg)
  if(OFX_IO_WITH_LIBRAW AND TARGET PkgConfig::LIBRAW)
    string(APPEND _io_groups " (with LibRaw ${LIBRAW_VERSION})")
    target_compile_definitions(IO PRIVATE OFX_IO_USING_LIBRAW)
    target_link_libraries(IO PRIVATE PkgConfig::LIBRAW)
    # LibRaw's headers include <omp.h> when compiled with OpenMP (_OPENMP): link OpenMP only if libraw_r.pc
    # asks for it (e.g. -fopenmp), as the Makefiles do; MacPorts' LibRaw does not.
    if("${LIBRAW_CFLAGS_OTHER};${LIBRAW_LDFLAGS_OTHER};${LIBRAW_LIBRARIES}" MATCHES "openmp|(^|;)-?l?gomp(;|$)|(^|;)-?l?omp(;|$)")
      ofx_target_openmp(IO)
    endif()
  else()
    _io_not_built(LIBRAW "LibRaw(libraw_r)")
  endif()
else()
  _io_not_built(OIIO "OIIO(OpenImageIO)")
endif()

if(OFX_IO_WITH_PNG AND TARGET io_png)
  string(APPEND _io_groups " PNG")
  supportext_add_sources(IO COMPONENTS FILEOPEN LUT)
  target_link_libraries(IO PRIVATE io_png)
  set(_io_src_PNG PNG/ReadPNG.cpp PNG/WritePNG.cpp)
  io_resources(
    PNG/fr.inria.openfx.ReadPNG.png PNG/fr.inria.openfx.ReadPNG.svg
    PNG/fr.inria.openfx.WritePNG.png PNG/fr.inria.openfx.WritePNG.svg)
else()
  _io_not_built(PNG "PNG(libpng)")
endif()

if(OFX_IO_WITH_SEEXPR AND TARGET SeExpr::SeExpr)
  string(APPEND _io_groups " SeExpr SeGrain SeNoise")
  # SeGrain and SeNoise use libSeExpr's noise functions
  supportext_add_sources(IO COMPONENTS RECTANGLE_INTERACT RAMP TRANSFORM_INTERACT)
  target_link_libraries(IO PRIVATE SeExpr::SeExpr)
  set(_io_src_SEEXPR SeExpr/SeExpr.cpp SeExpr/SeGrain.cpp SeExpr/SeNoise.cpp)
  # IO/Makefile has no SeExprSimple icons
  io_resources( SeExpr/fr.inria.openfx.SeExpr.png SeExpr/fr.inria.openfx.SeExpr.svg)
else()
  _io_not_built(SEEXPR "SeExpr(SeExpr 2.11, SEEXPR_HOME)")
endif()

# RunScript, on every OS, as in IO/Makefile (RunScriptWin32.cpp is empty except on MS Windows)
if(OFX_IO_WITH_RUNSCRIPT)
  string(APPEND _io_groups " RunScript")
  target_include_directories(IO PRIVATE RunScript)
  set(_io_src_RUNSCRIPT RunScript/RunScript.cpp RunScript/RunScriptWin32.cpp)
  io_resources( RunScript/fr.inria.openfx.RunScript.png RunScript/fr.inria.openfx.RunScript.svg)
else()
  _io_not_built(RUNSCRIPT "RunScript")
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
if(_io_disabled)
  message(STATUS "IO.ofx: disabled (OFX_IO_WITH_<group>=OFF):${_io_disabled}")
endif()

# stale resources in the build tree's bundle
get_target_property(_io_bundle IO OFX_BUNDLE_DIR)
file(GLOB _io_existing "${_io_bundle}/Contents/Resources/*")
foreach(_f IN LISTS _io_existing)
  get_filename_component(_n "${_f}" NAME)
  if(NOT _n IN_LIST _io_resource_names)
    file(REMOVE_RECURSE "${_f}")
  endif()
endforeach()

# Linux, FreeBSD: the install rpath of Makefile.master, $ORIGIN/../../Libraries then the libdirs of OpenColorIO,
# SeExpr, OpenImageIO (its prefix's lib), LibRaw and libpng, for the groups built (not
# INSTALL_RPATH_USE_LINK_PATH, which skips the libraries of the build tree, e.g. CI's SeExprBuild)
if(UNIX AND NOT APPLE)
  set(_io_rpath "\$ORIGIN/../../Libraries")
  if(" ${_io_groups} " MATCHES " OCIO " AND PKG_CONFIG_FOUND)
    pkg_get_variable(_d OpenColorIO libdir)
    list(APPEND _io_rpath ${_d})
  endif()
  if(" ${_io_groups} " MATCHES " SeExpr ")
    get_filename_component(_d "${SEEXPR_LIBRARY}" DIRECTORY)
    list(APPEND _io_rpath "${_d}")
  endif()
  if(" ${_io_groups} " MATCHES " OIIO ")
    get_target_property(_type OpenImageIO::OpenImageIO TYPE)
    if(NOT _type STREQUAL "INTERFACE_LIBRARY")
      get_target_property(_loc OpenImageIO::OpenImageIO LOCATION)
      get_filename_component(_d "${_loc}" DIRECTORY)
      list(APPEND _io_rpath "${_d}")
    elseif(PKG_CONFIG_FOUND)
      pkg_get_variable(_d OpenImageIO libdir)
      list(APPEND _io_rpath ${_d})
    endif()
    list(APPEND _io_rpath ${LIBRAW_LIBRARY_DIRS})
  endif()
  if(" ${_io_groups} " MATCHES " PNG ")
    list(APPEND _io_rpath ${LIBPNG_LIBRARY_DIRS})
  endif()
  list(REMOVE_DUPLICATES _io_rpath)
  set_target_properties(IO PROPERTIES INSTALL_RPATH "${_io_rpath}")
endif()

ofx_deploy_bundle(IO)

# ---------------------------------------------------------------------------------------------------------------
# Tests (ctest):
# - io-load: loads the IO.ofx of the build tree with .github/workflows/verify_plugin_loads.cpp (RTLD_NOW, as in
#   CI) and checks the number of plug-ins against the groups built;
# - io-load-installed (macOS, OFX_DEPLOY_BUNDLES): the same on the installed, deployed bundle, without DYLD_*
#   variables: run it after cmake --install;
# - tests-make-*: the tests of tests/Makefile (which builds them in tests/, with its own flags), with this build's
#   compiler and FFmpeg, when their libraries were found. IO_TESTS_MAKE_ARGS: extra make arguments, e.g. WITH_LIBRAW=1.
option(BUILD_TESTING "Build and run the tests with ctest" ON)
if(BUILD_TESTING)
  enable_testing()

  # the number of plug-ins of each group
  set(_io_nplugins 2) # PFM
  foreach(_g "OCIO:6" "EXR:2" "FFmpeg:2" "OIIO:5" "PNG:2" "SeExpr:4" "RunScript:1")
    string(REPLACE ":" ";" _g "${_g}")
    list(GET _g 0 _name)
    list(GET _g 1 _n)
    if(" ${_io_groups} " MATCHES " ${_name} ")
      math(EXPR _io_nplugins "${_io_nplugins} + ${_n}")
    endif()
  endforeach()

  # verify_plugin_loads, with HostSupport's ofxhBinary.cpp using RTLD_NOW instead of RTLD_LAZY
  file(READ "${OPENFX_PATH}/HostSupport/src/ofxhBinary.cpp" _io_src)
  string(REPLACE "RTLD_LAZY" "RTLD_NOW" _io_src "${_io_src}")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/ofxhBinaryStrict.cpp.new" "${_io_src}")
  configure_file("${CMAKE_CURRENT_BINARY_DIR}/ofxhBinaryStrict.cpp.new" "${CMAKE_CURRENT_BINARY_DIR}/ofxhBinaryStrict.cpp" COPYONLY)
  add_executable(io_verify_plugin_loads .github/workflows/verify_plugin_loads.cpp "${CMAKE_CURRENT_BINARY_DIR}/ofxhBinaryStrict.cpp")
  if(WIN32)
    target_sources(io_verify_plugin_loads PRIVATE "${OPENFX_PATH}/HostSupport/src/ofxhUtilities.cpp")
  endif()
  target_include_directories(io_verify_plugin_loads PRIVATE "${OPENFX_PATH}/HostSupport/include" "${OPENFX_PATH}/include")
  target_link_libraries(io_verify_plugin_loads PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(io_verify_plugin_loads PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

  add_test(NAME io-load COMMAND io_verify_plugin_loads "$<TARGET_FILE:IO>")
  set_tests_properties(io-load PROPERTIES PASS_REGULAR_EXPRESSION "Num_plugins: ${_io_nplugins}\n"
                                          FAIL_REGULAR_EXPRESSION "error|Failed")
  if(APPLE)
    # the build tree's IO.ofx has no LC_RPATH, and some libraries have @rpath install names: find them in the
    # directories of the linked libraries
    set(_io_dirs "")
    foreach(_t OpenColorIO::OpenColorIO OpenImageIO::OpenImageIO OpenEXR::OpenEXR SeExpr::SeExpr)
      if(TARGET ${_t})
        get_target_property(_type ${_t} TYPE)
        if(NOT _type STREQUAL "INTERFACE_LIBRARY")
          get_target_property(_loc ${_t} LOCATION)
          get_filename_component(_d "${_loc}" DIRECTORY)
          list(APPEND _io_dirs "${_d}")
        endif()
      endif()
    endforeach()
    list(APPEND _io_dirs ${FFMPEG_LIBRARY_DIRS} ${LIBRAW_LIBRARY_DIRS} ${LIBPNG_LIBRARY_DIRS} /usr/local/lib /usr/lib)
    list(REMOVE_DUPLICATES _io_dirs)
    string(REPLACE ";" ":" _io_dirs "${_io_dirs}")
    set_tests_properties(io-load PROPERTIES ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=${_io_dirs}")
  endif()
  # io-load-installed: the installed bundle (macOS: deployed, OFX_DEPLOY_BUNDLES; Linux, FreeBSD: with its install
  # rpath, before any patchelf) loads without DYLD_*/LD_LIBRARY_PATH; skipped if it was not installed yet
  if((APPLE AND OFX_DEPLOY_BUNDLES) OR (UNIX AND NOT APPLE))
    if(IS_ABSOLUTE "${PLUGIN_INSTALLDIR}")
      set(_io_installed "${PLUGIN_INSTALLDIR}")
    else()
      set(_io_installed "${CMAKE_INSTALL_PREFIX}/${PLUGIN_INSTALLDIR}")
    endif()
    set(_io_installed "${_io_installed}/IO.ofx.bundle/Contents/${ARCHDIR}/IO.ofx")
    add_test(NAME io-load-installed
             COMMAND sh -c "test -f \"$1\" || { echo \"not installed: $1\"; exit 77; }; exec \"$0\" \"$1\""
                     "$<TARGET_FILE:io_verify_plugin_loads>" "${_io_installed}")
    set_tests_properties(io-load-installed PROPERTIES PASS_REGULAR_EXPRESSION "Num_plugins: ${_io_nplugins}\n"
      FAIL_REGULAR_EXPRESSION "error|Failed" SKIP_RETURN_CODE 77
      ENVIRONMENT "DYLD_LIBRARY_PATH=;DYLD_FALLBACK_LIBRARY_PATH=;DYLD_FRAMEWORK_PATH=;LD_LIBRARY_PATH=")
  endif()

  find_program(IO_MAKE_PROGRAM NAMES gmake make mingw32-make)
  set(IO_TESTS_MAKE_ARGS "" CACHE STRING "Extra arguments for tests/Makefile (e.g. WITH_LIBRAW=1)")
  set(IO_TESTS_CXX "${CMAKE_CXX_COMPILER}" CACHE STRING "C++ compiler for tests/Makefile")
  # tests/Makefile's targets and what they need (OCIO+OIIO+FFmpeg: its OFX_PKGS)
  set(_io_make_tests "")
  if(" ${_io_groups} " MATCHES " FFmpeg ")
    list(APPEND _io_make_tests ffmpeg)
  endif()
  if(" ${_io_groups} " MATCHES " OIIO ")
    list(APPEND _io_make_tests oiio)
  endif()
  if(" ${_io_groups} " MATCHES " OCIO " AND " ${_io_groups} " MATCHES " OIIO " AND " ${_io_groups} " MATCHES " FFmpeg ")
    list(APPEND _io_make_tests ofx render layers reader-hooks)
  endif()
  # RunScript's MS Windows process launching (a no-op elsewhere)
  if(" ${_io_groups} " MATCHES " RunScript ")
    list(APPEND _io_make_tests runscript)
  endif()
  if(IO_MAKE_PROGRAM AND _io_make_tests)
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
    foreach(_t ${_io_make_tests})
      add_test(NAME tests-make-${_t}
               COMMAND "${IO_MAKE_PROGRAM}" -C "${PROJECT_SOURCE_DIR}/tests" test-${_t}
                       "CXX=${IO_TESTS_CXX}" ${_io_make_args})
      # the tests are built in the source tree: one at a time
      set_tests_properties(tests-make-${_t} PROPERTIES RESOURCE_LOCK openfx-io-tests-dir)
      if(_io_test_env)
        set_tests_properties(tests-make-${_t} PROPERTIES ENVIRONMENT "${_io_test_env}")
      endif()
    endforeach()
  elseif(NOT IO_MAKE_PROGRAM)
    message(STATUS "make not found: no tests-make-* tests")
  endif()
endif()
