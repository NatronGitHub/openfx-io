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
# paths and resources), except that a group of plug-ins is left out when its libraries are not found (unless
# OFX_IO_REQUIRE_ALL is ON).
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
#   BUILD_TESTING           build the tests of tests/ (tests/CMakeLists.txt, the programs of tests/Makefile), and run
#                           them with ctest; IO_TESTS_WITH_LIBRAW, IO_TESTS_FUZZ_OPS, IO_TESTS_SANITIZERS: see there
#   OFX_IO_WITH_<group>     build the group if its libraries are found (default ON): OCIO EXR FFMPEG OIIO LIBRAW
#                           PNG SEEXPR RUNSCRIPT
#   OFX_IO_REQUIRE_ALL      fail if a group that is not disabled can't be built (default OFF; for packagers)
#   IO_HOSTDEMO_TESTS       build openfx's Python host (the ofxh module, OFX_PYTHON_BINDINGS: needs nanobind and
#                           NumPy); ctest renders every plug-in of the installed bundle with its hostDemo command line,
#                           ofxhost.hostdemo (io-hostdemo, tests/hostdemo/run.sh), and again with --ofx14
#                           (io-hostdemo-ofx14); and renders its readers with narrow render windows, and windows out
#                           of their RoD, in a host built on HostSupport (io-reader-window, tests/hostdemo/reader_window.sh)
# The C++ standard is OFX_CXX_STANDARD (default: CMAKE_CXX_STANDARD, or 17).

cmake_minimum_required(VERSION 3.21...3.31)
# Minimum macOS version of the plug-ins (Natron 2.6 supports macOS 14 and later); must be set before project().
# Override it with -DCMAKE_OSX_DEPLOYMENT_TARGET=... or the MACOSX_DEPLOYMENT_TARGET environment variable; not set when cross-compiling (same guard as openfx).
if(CMAKE_HOST_APPLE AND NOT CMAKE_TOOLCHAIN_FILE AND (NOT CMAKE_SYSTEM_NAME OR CMAKE_SYSTEM_NAME STREQUAL "Darwin") AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "Minimum macOS version of the plug-ins")
endif()
project(OpenFXIO CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Prevented in-tree built. This is bad practice.")
endif()

# Check that the submodules have been initialized and updated. A source tarball (GitHub's archive/<ref>.tar.gz, or a
# release tarball) has no .git and empty submodule directories: git submodule update can't fetch them there.
foreach(_sub openfx/include SupportExt/ofxsMacros.h IOSupport/SequenceParsing/SequenceParsing.h
             IOSupport/SequenceParsing/tinydir/tinydir.h)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${_sub}")
    if(EXISTS "${PROJECT_SOURCE_DIR}/.git")
      message(FATAL_ERROR "\n submodule(s) are missing, please update your repository:\n"
                          "  > git submodule update --init --recursive\n")
    else()
      message(FATAL_ERROR "\n submodule(s) are missing, and this source tree has no .git (a source tarball?).\n"
                          " Either clone the repository with its submodules:\n"
                          "  > git clone --recursive --branch <tag> https://github.com/NatronGitHub/openfx-io.git\n"
                          " or extract the archive of each submodule, at the commit this tree pins, into its path:\n"
                          "  openfx                              https://github.com/NatronGitHub/openfx\n"
                          "  SupportExt                          https://github.com/NatronGitHub/openfx-supportext\n"
                          "  IOSupport/SequenceParsing           https://github.com/NatronGitHub/SequenceParsing\n"
                          "  IOSupport/SequenceParsing/tinydir   https://github.com/NatronGitHub/tinydir\n"
                          " (each pinned commit is in the parent repository's tree, at the tarball's tag or commit for openfx-io:\n"
                          " GitHub shows it next to the submodule's directory, e.g. \"openfx @ <sha>\").\n")
    endif()
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

# IO_HOSTDEMO_TESTS: build openfx's Python host, the ofxh module (HostSupport/python, which needs HostSupport, the system
# expat, Python >= 3.10, nanobind and NumPy, found by openfx; Python_EXECUTABLE chooses the Python), for the io-hostdemo
# tests (below), which run its hostDemo command line, ofxhost.hostdemo (#261)
option(IO_HOSTDEMO_TESTS "Build openfx's Python host (ofxh) and test rendering every plug-in of the installed bundle with it" OFF)
if(IO_HOSTDEMO_TESTS)
  set(OFX_PYTHON_BINDINGS ON CACHE BOOL "HostSupport Python bindings (nanobind)" FORCE)
endif()

set(OPENFX_PATH "${PROJECT_SOURCE_DIR}/openfx")
set(OFXS_EXT_PATH "${PROJECT_SOURCE_DIR}/SupportExt")
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake" "${PROJECT_SOURCE_DIR}/IOSupport/cmake")
# openfx's add_ofx_plugin() & co. (add_subdirectory(openfx)) and SupportExt's supportext_add_sources()
include(OpenFXLocal)
include(IOSupport)

if(OFX_CXX_STANDARD LESS 17)
  message(FATAL_ERROR "OFX_CXX_STANDARD=${OFX_CXX_STANDARD}: openfx-io needs C++17 or later (OpenImageIO 3.x, OpenColorIO 2.5)")
endif()

message(STATUS "Platform: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}, bundle folder ${ARCHDIR}, C++${OFX_CXX_STANDARD}")

# ---------------------------------------------------------------------------------------------------------------
# Dependencies. Each one gives an imported target; a group of plug-ins is only built if its libraries are found,
# and if its OFX_IO_WITH_<group> option is ON. CMAKE_DISABLE_FIND_PACKAGE_<package> also skips the pkg-config
# fallbacks (packages: OpenColorIO OpenEXR OpenImageIO LibRaw PNG FFmpeg SeExpr); with OFX_IO_REQUIRE_ALL, that
# counts as "not found": disable the group with OFX_IO_WITH_<group>=OFF instead.
foreach(_g OCIO EXR FFMPEG OIIO LIBRAW PNG SEEXPR RUNSCRIPT)
  option(OFX_IO_WITH_${_g} "Build the ${_g} plug-ins (LIBRAW: RAW support in ReadOIIO) if the libraries are found" ON)
endforeach()
# Packagers (e.g. Natron's build-plugins.sh): fail if a group that is not disabled can't be built, rather than
# silently building a bundle without it
option(OFX_IO_REQUIRE_ALL "Fail if the libraries of a plug-in group that is not disabled (OFX_IO_WITH_<group>) are not found" OFF)

# Makefile.master links -lGL (Linux, FreeBSD), -weak_framework OpenGL, -lopengl32: the legacy libGL, not GLVND's
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
# (OpenColorIO 2.0 or later, as checked in IOSupport/GenericOCIO.h)
if(OFX_IO_WITH_OCIO)
  find_package(OpenColorIO 2.0 CONFIG QUIET) # (OCIO 2.x: its version file accepts only its own major)
endif()
if(OFX_IO_WITH_OCIO AND NOT CMAKE_DISABLE_FIND_PACKAGE_OpenColorIO AND NOT TARGET OpenColorIO::OpenColorIO AND PKG_CONFIG_FOUND)
  pkg_check_modules(OCIO_PC QUIET IMPORTED_TARGET "OpenColorIO>=2.0")
  if(OCIO_PC_FOUND)
    add_library(OpenColorIO::OpenColorIO INTERFACE IMPORTED)
    target_link_libraries(OpenColorIO::OpenColorIO INTERFACE PkgConfig::OCIO_PC)
  else()
    # an older OpenColorIO: fail, rather than silently building without the OpenColorIO plug-ins
    pkg_check_modules(_IO_OCIO_ANY QUIET OpenColorIO)
    if(_IO_OCIO_ANY_FOUND)
      message(FATAL_ERROR "The OpenColorIO plug-ins require OpenColorIO 2.0 or later, but pkg-config found "
              "OpenColorIO ${_IO_OCIO_ANY_VERSION} (in ${_IO_OCIO_ANY_PREFIX}). Build without them with "
              "-DOFX_IO_WITH_OCIO=OFF.")
    endif()
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
# (OpenImageIO 2.4 or later, as checked in OIIO/OIIOGlobal.cpp; an older one fails, rather than silently building
# without the OpenImageIO plug-ins)
if(OFX_IO_WITH_OIIO)
  find_package(OpenImageIO CONFIG QUIET) # (no version: its version file accepts only its own major)
  if(OpenImageIO_FOUND AND OpenImageIO_VERSION VERSION_LESS 2.4)
    message(FATAL_ERROR "The OpenImageIO plug-ins require OpenImageIO 2.4 or later, but CMake found OpenImageIO "
            "${OpenImageIO_VERSION} (in ${OpenImageIO_DIR}). Build without them with -DOFX_IO_WITH_OIIO=OFF.")
  endif()
endif()
if(OFX_IO_WITH_OIIO AND NOT CMAKE_DISABLE_FIND_PACKAGE_OpenImageIO AND NOT TARGET OpenImageIO::OpenImageIO AND PKG_CONFIG_FOUND)
  pkg_check_modules(OIIO_PC QUIET IMPORTED_TARGET "OpenImageIO>=2.4")
  if(OIIO_PC_FOUND)
    add_library(OpenImageIO::OpenImageIO INTERFACE IMPORTED)
    target_link_libraries(OpenImageIO::OpenImageIO INTERFACE PkgConfig::OIIO_PC)
  else()
    pkg_check_modules(_IO_OIIO_ANY QUIET OpenImageIO)
    if(_IO_OIIO_ANY_FOUND)
      message(FATAL_ERROR "The OpenImageIO plug-ins require OpenImageIO 2.4 or later, but pkg-config found "
              "OpenImageIO ${_IO_OIIO_ANY_VERSION} (in ${_IO_OIIO_ANY_PREFIX}). Build without them with "
              "-DOFX_IO_WITH_OIIO=OFF.")
    endif()
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
  # FFmpeg 8.0.1 or later: the versions of the libraries in the n8.0 release, which n8.0.1 keeps (as checked in
  # FFmpeg/FFmpegFile.h, which also rejects 8.0 itself by its FFMPEG_VERSION: pkg-config only has library versions)
  set(_io_ffmpeg_modules libavformat>=62.3.100 libavcodec>=62.11.100 libswscale>=9.1.100 libavutil>=60.8.100)
  pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET ${_io_ffmpeg_modules})
  if(NOT FFMPEG_FOUND)
    # an older FFmpeg: fail, rather than silently building the bundle without the FFmpeg plug-ins
    _io_pkg_config_path(_IO_FFMPEG_ANY "${FFMPEG_PKG_CONFIG_PATH}")
    pkg_check_modules(_IO_FFMPEG_ANY QUIET libavformat libavcodec libswscale libavutil)
    if(_IO_FFMPEG_ANY_FOUND)
      set(_io_ffmpeg_found "")
      foreach(_m libavformat libavcodec libswscale libavutil)
        string(APPEND _io_ffmpeg_found " ${_m} ${_IO_FFMPEG_ANY_${_m}_VERSION}")
      endforeach()
      string(REPLACE ";" ", " _io_ffmpeg_min "${_io_ffmpeg_modules}")
      message(FATAL_ERROR "The FFmpeg plug-ins require FFmpeg 8.0.1 or later (${_io_ffmpeg_min}), but pkg-config "
              "found${_io_ffmpeg_found} (in ${_IO_FFMPEG_ANY_libavcodec_PREFIX}). Select another FFmpeg with "
              "-DFFMPEG_PKG_CONFIG_PATH=<prefix>/lib/pkgconfig, or build without the FFmpeg plug-ins with "
              "-DOFX_IO_WITH_FFMPEG=OFF.")
    endif()
  endif()
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
# link.
ofx_target_filter_missing_includes(OpenColorIO::OpenColorIO OpenEXR::OpenEXR OpenImageIO::OpenImageIO
                                   PkgConfig::LIBRAW io_png PkgConfig::FFMPEG SeExpr::SeExpr)

# ---------------------------------------------------------------------------------------------------------------
# The IO.ofx bundle (IO/Makefile)

# Install rpath (Linux, FreeBSD), which the build-tree IO.ofx has too (OFX_PLUGIN_BUILD_WITH_INSTALL_RPATH):
# add_ofx_plugin's $ORIGIN/../../Libraries, then the directories of the linked libraries
# (INSTALL_RPATH_USE_LINK_PATH), which skips system directories and the libraries of the source and build trees, e.g.
# CI's SeExprBuild and linux-natron's FFmpegBuild: add SeExpr's and FFmpeg's directories explicitly when they are in
# one of those trees. (Other dependencies installed in the source or build tree need OFX_PLUGIN_RPATH.) Not on
# macOS, where the libraries have absolute install names (and ofx_deploy_bundle rewrites them).
set(_io_extra_rpath "")
if(UNIX AND NOT APPLE)
  set(_io_lib_dirs "")
  if(OFX_IO_WITH_SEEXPR AND TARGET SeExpr::SeExpr)
    get_filename_component(_d "${SEEXPR_LIBRARY}" DIRECTORY)
    list(APPEND _io_lib_dirs "${_d}")
  endif()
  if(OFX_IO_WITH_FFMPEG AND TARGET PkgConfig::FFMPEG)
    # the -L directories of the FFmpeg modules (pkg-config leaves out the system ones)
    list(APPEND _io_lib_dirs ${FFMPEG_LIBRARY_DIRS})
  endif()
  foreach(_d IN LISTS _io_lib_dirs)
    string(FIND "${_d}/" "${PROJECT_SOURCE_DIR}/" _in_src)
    string(FIND "${_d}/" "${PROJECT_BINARY_DIR}/" _in_bin)
    if(_in_src EQUAL 0 OR _in_bin EQUAL 0)
      list(APPEND _io_extra_rpath "${_d}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _io_extra_rpath)
endif()
add_ofx_plugin(IO INFO_PLIST IO/Info.plist RPATH ${_io_extra_rpath})
if(_io_extra_rpath)
  # DT_RPATH instead of DT_RUNPATH: a DT_RUNPATH only applies to IO.ofx's own (direct) dependencies, not to theirs
  # (ld.so(8)), and an in-tree FFmpeg's libraries have no rpath of their own, e.g. libavformat needs
  # libswresample, which is in FFmpegBuild/lib too. The DT_RPATH of IO.ofx also applies to them, as in the testing
  # bundle (patchelf --force-rpath). The side effect, a DT_RPATH taking precedence over LD_LIBRARY_PATH, only
  # applies to these builds with an in-tree SeExpr or FFmpeg.
  target_link_options(IO PRIVATE "LINKER:--disable-new-dtags")
endif()
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
# OpenGL, also without the OpenGL sources, linked as openfx's Makefile.master does (openfx#171): weak-linked on
# macOS (-weak_framework OpenGL; also weak if OpenGL::GL adds -framework OpenGL), and --as-needed on ELF
# platforms (-Wl,--as-needed -lGL), so that the bundle loads on a machine without OpenGL when it makes no OpenGL
# call. LINK_OPTIONS come before the libraries on the link line: --as-needed applies to all of the IO target's
# shared libraries, as with the default of many Linux distributions' toolchains.
target_link_libraries(IO PRIVATE OpenGL::GL)
if(APPLE)
  target_link_options(IO PRIVATE "LINKER:-weak_framework,OpenGL")
elseif(UNIX)
  target_link_options(IO PRIVATE "LINKER:--as-needed")
endif()
io_resources(
  PFM/fr.inria.openfx.ReadPFM.png PFM/fr.inria.openfx.ReadPFM.svg
  PFM/fr.inria.openfx.WritePFM.png PFM/fr.inria.openfx.WritePFM.svg)

set(_io_groups "PFM")
set(_io_missing "")
set(_io_missing_options "")
set(_io_disabled "")
# a group that is not built: disabled (OFX_IO_WITH_<g>=OFF), or its libraries were not found
macro(_io_not_built _g _what)
  if(OFX_IO_WITH_${_g})
    string(APPEND _io_missing " ${_what}")
    string(APPEND _io_missing_options " -DOFX_IO_WITH_${_g}=OFF")
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
  io_resources(
    SeExpr/fr.inria.openfx.SeExpr.png SeExpr/fr.inria.openfx.SeExpr.svg
    SeExpr/fr.inria.openfx.SeExprSimple.png SeExpr/fr.inria.openfx.SeExprSimple.svg)
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

# OpenEXR's global thread pool, used by the EXR and OIIO plug-ins (IOSupport/ExrThreads.h)
if(_io_src_EXR OR _io_src_OIIO)
  target_sources(IO PRIVATE IOSupport/ExrThreads.cpp)
  if(NOT TARGET OpenEXR::OpenEXR)
    target_compile_definitions(IO PRIVATE OFX_IO_NO_ILMTHREAD)
  endif()
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
if(OFX_IO_REQUIRE_ALL AND _io_missing)
  message(FATAL_ERROR "IO.ofx: OFX_IO_REQUIRE_ALL is ON, but the libraries of these plug-in groups were not found:"
                      "${_io_missing}. Install them (or point CMake to them), or disable the groups:${_io_missing_options}")
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

ofx_deploy_bundle(IO)

# ---------------------------------------------------------------------------------------------------------------
# Tests (ctest):
# - io-load: loads the IO.ofx of the build tree with io_verify_plugin_loads (tests/verify_plugin_loads.cpp, with
#   RTLD_NOW; CI's load checks of the bundles use it too) and checks the number of plug-ins against the groups built;
# - io-icons: each plug-in of the build tree's bundle has its icon, Contents/Resources/<identifier>.png
#   (tests/check_icons.cmake);
# - io-load-installed (macOS, OFX_DEPLOY_BUNDLES): the same on the installed, deployed bundle, without DYLD_*
#   variables: run it after cmake --install;
# - io-cmake-unknown-arch: configuring for a system without an OFX bundle folder (CMAKE_SYSTEM_NAME Generic) fails
#   (cmake/OpenFXLocal.cmake, #106);
# - test-*: the tests of tests/Makefile, built by CMake in build/tests with this build's compiler, C++ standard and
#   FFmpeg, when the libraries they link were found (tests/CMakeLists.txt: the same programs, sources and flags as
#   tests/Makefile, one ctest test per run of `make test`).
option(BUILD_TESTING "Build and run the tests with ctest" ON)
if(IO_HOSTDEMO_TESTS AND NOT BUILD_TESTING)
  message(FATAL_ERROR "IO_HOSTDEMO_TESTS needs BUILD_TESTING")
endif()
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
  add_executable(io_verify_plugin_loads tests/verify_plugin_loads.cpp "${CMAKE_CURRENT_BINARY_DIR}/ofxhBinaryStrict.cpp")
  if(WIN32)
    target_sources(io_verify_plugin_loads PRIVATE "${OPENFX_PATH}/HostSupport/src/ofxhUtilities.cpp")
  endif()
  target_include_directories(io_verify_plugin_loads PRIVATE "${OPENFX_PATH}/HostSupport/include" "${OPENFX_PATH}/include")
  target_link_libraries(io_verify_plugin_loads PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(io_verify_plugin_loads PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

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
  # io-icons: every plug-in of the build tree's bundle has its icon, Contents/Resources/<identifier>.png
  add_test(NAME io-icons
           COMMAND "${CMAKE_COMMAND}" "-DVERIFY=$<TARGET_FILE:io_verify_plugin_loads>" "-DBINARY=$<TARGET_FILE:IO>"
                   "-DBUNDLE=${_io_bundle}" -P "${PROJECT_SOURCE_DIR}/tests/check_icons.cmake")
  if(APPLE)
    set_tests_properties(io-icons PROPERTIES ENVIRONMENT "DYLD_FALLBACK_LIBRARY_PATH=${_io_dirs}")
  endif()
  # the installed bundle (io-load-installed, io-hostdemo...)
  if(IS_ABSOLUTE "${PLUGIN_INSTALLDIR}")
    set(_io_installed_bundle "${PLUGIN_INSTALLDIR}/IO.ofx.bundle")
  else()
    set(_io_installed_bundle "${CMAKE_INSTALL_PREFIX}/${PLUGIN_INSTALLDIR}/IO.ofx.bundle")
  endif()
  # io-load-installed: the installed bundle (macOS: deployed, OFX_DEPLOY_BUNDLES; Linux, FreeBSD: with its install
  # rpath, before any patchelf) loads without DYLD_*/LD_LIBRARY_PATH; skipped if it was not installed yet
  if((APPLE AND OFX_DEPLOY_BUNDLES) OR (UNIX AND NOT APPLE))
    add_test(NAME io-load-installed
             COMMAND sh -c "test -f \"$1\" || { echo \"not installed: $1\"; exit 77; }; exec \"$0\" \"$1\""
                     "$<TARGET_FILE:io_verify_plugin_loads>" "${_io_installed_bundle}/Contents/${ARCHDIR}/IO.ofx")
    set_tests_properties(io-load-installed PROPERTIES PASS_REGULAR_EXPRESSION "Num_plugins: ${_io_nplugins}\n"
      FAIL_REGULAR_EXPRESSION "error|Failed" SKIP_RETURN_CODE 77
      ENVIRONMENT "DYLD_LIBRARY_PATH=;DYLD_FALLBACK_LIBRARY_PATH=;DYLD_FRAMEWORK_PATH=;LD_LIBRARY_PATH=")
  endif()

  # io-hostdemo (IO_HOSTDEMO_TESTS): openfx's Python host, with hostDemo's command line (ofxhost.hostdemo, run by
  # openfx's HostSupport/examples/python/hostdemo-inprocess), loads the installed bundle and renders each of its
  # plug-ins (tests/hostdemo/run.sh, which lists what is tested and what is skipped); skipped if it was not installed
  # yet (FATAL_ERROR if it can't be: a CI build with IO_HOSTDEMO_TESTS=ON must not pass without the test)
  if(IO_HOSTDEMO_TESTS)
    find_program(IO_BASH NAMES bash)
    if(NOT TARGET ofxh)
      message(FATAL_ERROR "IO_HOSTDEMO_TESTS: openfx's ofxh module is not built (OFX_PYTHON_BINDINGS)")
    elseif(NOT IO_BASH)
      message(FATAL_ERROR "IO_HOSTDEMO_TESTS: bash not found")
    else()
      # openfx is EXCLUDE_FROM_ALL: build the ofxh module with the default target
      add_custom_target(io_hostdemo ALL)
      add_dependencies(io_hostdemo ofxh)
      # the host: the Python that openfx built the ofxh module for (OFXHOST_PYTHON), with the module on PYTHONPATH
      # (hostdemo-inprocess adds the ofxhost package next to it)
      get_directory_property(_io_python DIRECTORY "${OPENFX_PATH}/HostSupport/python" DEFINITION Python_EXECUTABLE)
      if(NOT _io_python)
        # (hostdemo-inprocess would run python3 from PATH, which may not be the one of the module)
        message(FATAL_ERROR "IO_HOSTDEMO_TESTS: the Python of openfx's ofxh module is unknown (no Python_EXECUTABLE in "
                            "${OPENFX_PATH}/HostSupport/python): set Python_EXECUTABLE")
      endif()
      set(_io_hd_host "${OPENFX_PATH}/HostSupport/examples/python/hostdemo-inprocess")
      set(_io_hd_env "OFXHOST_PYTHON=${_io_python}" "PYTHONPATH=$<TARGET_FILE_DIR:ofxh>")
      add_test(NAME io-hostdemo
               COMMAND "${IO_BASH}" "${PROJECT_SOURCE_DIR}/tests/hostdemo/run.sh" "${_io_hd_host}"
                       "${_io_installed_bundle}" "$<TARGET_FILE:io_verify_plugin_loads>"
                       "${CMAKE_CURRENT_BINARY_DIR}/hostdemo")
      set(_io_env "OCIO=${PROJECT_SOURCE_DIR}/tests/test.ocio")
      if(APPLE AND NOT OFX_DEPLOY_BUNDLES)
        list(APPEND _io_env "DYLD_FALLBACK_LIBRARY_PATH=${_io_dirs}")
      elseif(UNIX)
        # the installed bundle (deployed on macOS) loads by itself
        list(APPEND _io_env "DYLD_LIBRARY_PATH=" "DYLD_FALLBACK_LIBRARY_PATH=" "DYLD_FRAMEWORK_PATH=" "LD_LIBRARY_PATH=")
      endif()
      # TIMEOUT: more than run.sh's HOSTDEMO_TOTAL_TIMEOUT (600 s), so that it reports what hung
      # RESOURCE_LOCK: io-hostdemo and io-hostdemo-ofx14 render every plug-in of the same bundle, each of them
      # loads the machine: not at the same time under ctest -j
      set_tests_properties(io-hostdemo PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "${_io_env};${_io_hd_env}"
                           RESOURCE_LOCK openfx-io-hostdemo)
      # io-hostdemo-ofx14: the same tests with --ofx14, a host that implements only OFX 1.4 (its host properties and
      # suites, no OfxSetHost, no StrChoice or Bytes params, one view)
      add_test(NAME io-hostdemo-ofx14
               COMMAND "${IO_BASH}" "${PROJECT_SOURCE_DIR}/tests/hostdemo/run.sh" "${_io_hd_host}"
                       "${_io_installed_bundle}" "$<TARGET_FILE:io_verify_plugin_loads>"
                       "${CMAKE_CURRENT_BINARY_DIR}/hostdemo-ofx14")
      set_tests_properties(io-hostdemo-ofx14 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900
                           ENVIRONMENT "${_io_env};${_io_hd_env};HOSTDEMO_ARGS=--ofx14" RESOURCE_LOCK openfx-io-hostdemo)
      # io-reader-window: the readers of the installed bundle, with render windows in the image and partly or
      # entirely out of the RoD, in reader_window (tests/hostdemo/reader_window.cpp), a host built on HostSupport
      # and hostDemo's host classes (hostDemo renders the whole RoD of a plug-in without tiles, whatever --window)
      set(_io_hd "${OPENFX_PATH}/HostSupport/examples")
      add_executable(io_reader_window tests/hostdemo/reader_window.cpp
                     "${_io_hd}/hostDemoClipInstance.cpp" "${_io_hd}/hostDemoEffectInstance.cpp"
                     "${_io_hd}/hostDemoHostDescriptor.cpp" "${_io_hd}/hostDemoParamInstance.cpp")
      target_include_directories(io_reader_window PRIVATE "${_io_hd}")
      target_link_libraries(io_reader_window PRIVATE OfxHost)
      # the C++ standard of OfxHost (C++17, cmake/OpenFXLocal.cmake), not the compiler's default (gnu++20 with GCC 16)
      set_target_properties(io_reader_window PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
      get_filename_component(_io_installed_dir "${_io_installed_bundle}" DIRECTORY)
      add_test(NAME io-reader-window
               COMMAND "${IO_BASH}" "${PROJECT_SOURCE_DIR}/tests/hostdemo/reader_window.sh" "$<TARGET_FILE:io_reader_window>"
                       "${_io_installed_dir}" "${CMAKE_CURRENT_BINARY_DIR}/reader-window")
      set_tests_properties(io-reader-window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "${_io_env}")
    endif()
  endif()

  # io-cmake-unknown-arch: a configure for a system that OpenFX.cmake has no ARCHDIR for (Generic: "unknown-arch") stops
  # with an error, instead of installing the plug-ins into Contents/unknown-arch, where no host looks (#106). In an
  # empty build directory each time (tests/run_test.cmake, whose failure doesn't matter: the error message passes).
  # Single-config generators only (Makefiles, Ninja): Xcode and Visual Studio have no Generic platform.
  get_property(_io_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(NOT _io_multi_config)
    set(_io_ua "${CMAKE_CURRENT_BINARY_DIR}/cmake-unknown-arch")
    add_test(NAME io-cmake-unknown-arch
             COMMAND "${CMAKE_COMMAND}" "-DDIR=${_io_ua}" -P "${PROJECT_SOURCE_DIR}/tests/run_test.cmake" --
                     "${CMAKE_COMMAND}" -S "${PROJECT_SOURCE_DIR}" -B "${_io_ua}" -G "${CMAKE_GENERATOR}"
                     "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}" -DCMAKE_SYSTEM_NAME=Generic
                     -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
                     "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    set_tests_properties(io-cmake-unknown-arch PROPERTIES
                         PASS_REGULAR_EXPRESSION "No OFX bundle architecture folder \\(ARCHDIR\\) for Generic")
  endif()

  # (a few seconds each: a hung load fails)
  foreach(_t io-load io-icons io-load-installed io-cmake-unknown-arch)
    if(TEST ${_t})
      set_tests_properties(${_t} PROPERTIES TIMEOUT 300)
    endif()
  endforeach()

  # the tests of tests/Makefile, built and run by CMake and ctest (tests/CMakeLists.txt)
  add_subdirectory(tests)
endif()
