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

########################################################################
if(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
message(FATAL_ERROR "Prevented in-tree built. This is bad practice.")
endif(${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})

########################################################################
# Project setup
########################################################################
cmake_minimum_required(VERSION 3.1)
project(OpenFXIO CXX)
enable_testing()
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake/Modules/")

# ========================== build platform ==========================

message(STATUS "  Platform:")
message(STATUS "    Host:"             ${CMAKE_HOST_SYSTEM_NAME} ${CMAKE_HOST_SYSTEM_VERSION} ${CMAKE_HOST_SYSTEM_PROCESSOR})
if(CMAKE_CROSSCOMPILING)
  message(STATUS "    Target:"         ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_VERSION} ${CMAKE_SYSTEM_PROCESSOR})
endif()
message(STATUS "    CMake:"            ${CMAKE_VERSION})
message(STATUS "    CMake generator:"  ${CMAKE_GENERATOR})
message(STATUS "    CMake build tool:" ${CMAKE_BUILD_TOOL})
if(MSVC)
  message(STATUS "    MSVC:"           ${MSVC_VERSION})
endif()
if(CMAKE_GENERATOR MATCHES Xcode)
  message(STATUS "    Xcode:"          ${XCODE_VERSION})
endif()
if(NOT CMAKE_GENERATOR MATCHES "Xcode|Visual Studio")
  message(STATUS "    Configuration:"  ${CMAKE_BUILD_TYPE})
endif()

include(FindOpenGL REQUIRED)
find_package(OpenImageIO)
find_package(PNG)
find_package(OpenColorIO)
find_package(FFmpeg)
find_package(OpenEXR)
find_package(LibRaw)
find_package(SeExpr2)


set(CMAKE_CXX_STANDARD 11)

# Use -Ofast when applicable (implies -ffast-math)
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Wextra -Ofast -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE   "-Wall -Wextra -Ofast -DNDEBUG")
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel")
    set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Ofast -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE   "-Wall -Ofast -DNDEBUG")
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS_RELEASE "-Wall -Wextra -Ofast -DNDEBUG")
    set(CMAKE_C_FLAGS_RELEASE   "-Wall -Wextra -Ofast -DNDEBUG")
endif()
set(OPENFX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/openfx)

# Check that submodule have been initialized and updated
if(NOT EXISTS ${PROJECT_SOURCE_DIR}/openfx/include)
  message(FATAL_ERROR
    "\n submodule(s) are missing, please update your repository:\n"
    "  > git submodule update -i\n")
endif()

# C++ Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SupportExt)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/SupportExt/glad)
include_directories(${OPENFX_PATH}/include)
include_directories(${OPENFX_PATH}/Support/include)
include_directories(${OPENFX_PATH}/Support/Plugins/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/IOSupport)

# Define "DEBUG" on debug builds
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")

# /MP on MSVC
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)

set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")

set(OFX_SUPPORT_HEADER_DIR "${OPENFX_PATH}/Support/include")
set(OFX_SUPPORT_LIBRARY_DIR "${OPENFX_PATH}/Support/Library")

file(GLOB SUPPORT_SOURCES
    "${OFX_SUPPORT_HEADER_DIR}/*.h"

    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsCore.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsImageEffect.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsInteract.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsLog.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsMultiThread.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsParams.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsProperty.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/ofxsPropertyValidation.cpp"
    "${OFX_SUPPORT_LIBRARY_DIR}/*.h"
)

#add_library(Support STATIC )
#target_compile_definitions(Support PRIVATE OFX_SUPPORTS_OPENGLRENDER OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_NATRON OFX_EXTENSIONS_VEGAS NOMINMAX)

file(GLOB IO_SOURCES_EXR
  "EXR/*.cpp"
)
file(GLOB IO_SOURCES_FFMPEG
  "FFmpeg/*.cpp"
)
file(GLOB IO_SOURCES_OPENCOLORIO
  "OCIO/*.cpp"
)
file(GLOB IO_SOURCES_OPENIMAGEIO
  "OIIO/*.cpp"
  "SupportExt/ofxsOGLTextRenderer.cpp"
  "SupportExt/ofxsOGLFontData.cpp"
)
file(GLOB IO_SOURCES_PNG
  "PNG/*.cpp"
  "SupportExt/ofxsLut.cpp"
)
file(GLOB IO_SOURCES_SEEXPR
  "SeExpr/*.cpp"
  "SupportExt/ofxsOGLFontData.cpp"
  "SupportExt/ofxsOGLTextRenderer.cpp"
  "SupportExt/ofxsOGLUtilities.cpp"
  "SupportExt/ofxsRamp.cpp"
  "SupportExt/ofxsRectangleInteract.cpp"
  "SupportExt/ofxsShutter.cpp"
  "SupportExt/ofxsTransform3x3.cpp"
  "SupportExt/ofxsTransformInteract.cpp"
)
file(GLOB IO_SOURCES
  "PFM/*.cpp"
  "IOSupport/*.cpp"
  "IOSupport/*.h"
  "IOSupport/SequenceParsing/*.cpp"
  "IOSupport/SequenceParsing/*.h"  
  "SupportExt/tinythread.cpp"
  "SupportExt/ofxsThreadSuite.cpp"
  "SupportExt/ofxsFileOpen.cpp"
#  "SupportExt/ofxsGenerator.cpp"
  "SupportExt/ofxsMultiPlane.cpp"
  "SupportExt/ofxsOGLUtilities.cpp"
#  "SupportExt/ofxsTracking.cpp"
  "SupportExt/*.h"
  "SupportExt/glad/*.cpp"
  "SupportExt/glad/*.h"
)

file(GLOB IO_RESOURCES_EXR
  "EXR/fr.inria.openfx.ReadEXR.png"
  "EXR/fr.inria.openfx.ReadEXR.svg"
  "EXR/fr.inria.openfx.WriteEXR.png"
  "EXR/fr.inria.openfx.WriteEXR.svg"
)
file(GLOB IO_RESOURCES_FFMPEG
  "FFmpeg/fr.inria.openfx.ReadFFmpeg.png"
  "FFmpeg/fr.inria.openfx.ReadFFmpeg.svg"
  "FFmpeg/fr.inria.openfx.WriteFFmpeg.png"
  "FFmpeg/fr.inria.openfx.WriteFFmpeg.svg"
)
file(GLOB IO_RESOURCES_OPENCOLORIO
  "OCIO/fr.inria.openfx.OCIOCDLTransform.png"
  "OCIO/fr.inria.openfx.OCIOColorSpace.png"
  "OCIO/fr.inria.openfx.OCIODisplay.png"
  "OCIO/fr.inria.openfx.OCIOFileTransform.png"
  "OCIO/fr.inria.openfx.OCIOLogConvert.png"
  "OCIO/fr.inria.openfx.OCIOLookTransform.png"
)
file(GLOB IO_RESOURCES_OPENIMAGEIO
  "OIIO/fr.inria.openfx.OIIOResize.png"
  "OIIO/fr.inria.openfx.OIIOResize.svg"
  "OIIO/fr.inria.openfx.OIIOText.png"
  "OIIO/fr.inria.openfx.OIIOText.svg"
  "OIIO/fr.inria.openfx.ReadOIIO.png"
  "OIIO/fr.inria.openfx.ReadOIIO.svg"
  "OIIO/fr.inria.openfx.WriteOIIO.png"
  "OIIO/fr.inria.openfx.WriteOIIO.svg"
)
file(GLOB IO_RESOURCES_PNG
  "PNG/fr.inria.openfx.ReadPNG.png"
  "PNG/fr.inria.openfx.ReadPNG.svg"
  "PNG/fr.inria.openfx.WritePNG.png"
  "PNG/fr.inria.openfx.WritePNG.svg"
)
file(GLOB IO_RESOURCES_SEEXPR
  "SeExpr/fr.inria.openfx.SeExpr.png"
  "SeExpr/fr.inria.openfx.SeExpr.svg"
  "SeExpr/fr.inria.openfx.SeExprSimple.png"
  "SeExpr/fr.inria.openfx.SeExprSimple.svg"
)
file(GLOB IO_RESOURCES
  "PFM/fr.inria.openfx.ReadPFM.png"
  "PFM/fr.inria.openfx.ReadPFM.svg"
  "PFM/fr.inria.openfx.WritePFM.png"
  "PFM/fr.inria.openfx.WritePFM.svg"
  "RunScript/fr.inria.openfx.RunScript.png"
  "RunScript/fr.inria.openfx.RunScript.svg"
)

if(${OPENEXR_FOUND})
  list(APPEND IO_RESOURCES ${IO_RESOURCES_EXR})
  list(APPEND IO_SOURCES ${IO_SOURCES_EXR})
endif()

if(${OPENCOLORIO_FOUND})
  list(APPEND IO_RESOURCES ${IO_RESOURCES_OPENCOLORIO})
  list(APPEND IO_SOURCES ${IO_SOURCES_OPENCOLORIO})
endif()

if(${OPENIMAGEIO_FOUND})
  list(APPEND IO_RESOURCES ${IO_RESOURCES_OPENIMAGEIO})
  list(APPEND IO_SOURCES ${IO_SOURCES_OPENIMAGEIO})
endif()

if(${FFMPEG_FOUND})
  list(APPEND IO_RESOURCES ${IO_RESOURCES_FFMPEG})
  list(APPEND IO_SOURCES ${IO_SOURCES_FFMPEG})
endif()

if(${PNG_FOUND})
  list(APPEND IO_RESOURCES ${IO_RESOURCES_PNG})
  list(APPEND IO_SOURCES ${IO_SOURCES_PNG})
endif()

if(${SEEXPR2_FOUND})
  list(APPEND IO_RESOURCES ${IO_RESOURCES_SEEXPR})
  list(APPEND IO_SOURCES ${IO_SOURCES_SEEXPR})
endif()

add_library(IO SHARED ${IO_SOURCES} ${SUPPORT_SOURCES})
SET_TARGET_PROPERTIES(IO PROPERTIES PREFIX "")
SET_TARGET_PROPERTIES(IO PROPERTIES SUFFIX ".ofx")

# Add extra flags to the link step of the plugin
if(APPLE)
    set_target_properties(IO PROPERTIES LINK_FLAGS "-fvisibility=hidden -exported_symbols_list,${OFX_SUPPORT_HEADER_DIR}/osxSymbols")
    set_target_properties(IO PROPERTIES INSTALL_RPATH "@loader_path/../Frameworks;@loader_path/../Libraries")
elseif(MINGW)
    set_target_properties(IO PROPERTIES LINK_FLAGS "-shared -fvisibility=hidden -Xlinker --version-script=${OFX_SUPPORT_HEADER_DIR}/linuxSymbols")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD" OR ${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
# Linux & FreeBSD
    set_target_properties(IO PROPERTIES LINK_FLAGS "-Wl,-fvisibility=hidden,--version-script=${OFX_SUPPORT_HEADER_DIR}/linuxSymbols")
    set_target_properties(IO PROPERTIES INSTALL_RPATH "$ORIGIN/../../Libraries")
endif()

target_compile_definitions(IO
  PRIVATE
    OFX_EXTENSIONS_VEGAS
    OFX_EXTENSIONS_NUKE
    OFX_EXTENSIONS_NATRON
    OFX_EXTENSIONS_TUTTLE
    OFX_SUPPORTS_OPENGLRENDER
    NOMINMAX)
target_include_directories(IO
  PUBLIC
    ${CMAKE_INSTALL_FULL_INCLUDEDIR}
)
target_link_libraries(IO
  PUBLIC
    ${OPENGL_gl_LIBRARY}
)

message(STATUS "External library support:")

if(${OPENEXR_FOUND})
  message(STATUS "  Adding OpenEXR nodes")
  target_include_directories(IO
    PUBLIC
      ${OPENEXR_INCLUDES} ${ILMBASE_INCLUDES}
  )
  target_link_libraries(IO
    PUBLIC
      # For OpenEXR/Imath 3.x:
      $<$<TARGET_EXISTS:OpenEXR::OpenEXR>:OpenEXR::OpenEXR>
      $<$<TARGET_EXISTS:Imath::Imath>:Imath::Imath>
      $<$<TARGET_EXISTS:Imath::Half>:Imath::Half>
      # For OpenEXR >= 2.4/2.5 with reliable exported targets
      $<$<TARGET_EXISTS:OpenEXR::IlmImf>:OpenEXR::IlmImf>
      $<$<TARGET_EXISTS:IlmBase::Imath>:IlmBase::Imath>
      $<$<TARGET_EXISTS:IlmBase::Half>:IlmBase::Half>
      $<$<TARGET_EXISTS:IlmBase::IlmThread>:IlmBase::IlmThread>
      $<$<TARGET_EXISTS:IlmBase::Iex>:IlmBase::Iex>
      # These two are for OpenEXR <= 2.3:    ${OPENEXR_LIBRARIES} ${ILMBASE_LIBRARIES}
  )
else()
  message(STATUS "  Not adding OpenEXR nodes")
endif()

if(${OPENCOLORIO_FOUND})
  message(STATUS "  Adding OpenColorIO nodes")
  target_compile_definitions(IO PRIVATE OFX_IO_USING_OCIO ${OPENCOLORIO_DEFINITIONS})
  target_include_directories(IO
    PUBLIC
      ${OPENCOLORIO_INCLUDES}
  )
  target_link_libraries(IO
    PUBLIC
      ${OPENCOLORIO_LIBRARIES}
  )
else()
  message(STATUS "  Not adding OpenColorIO nodes")
endif()

if(${OPENIMAGEIO_FOUND})
  message(STATUS "  Adding OpenImageIO nodes")
  target_compile_definitions(IO PRIVATE ${LibRaw_r_DEFINITIONS})
  target_include_directories(IO
    PUBLIC
      ${OPENIMAGEIO_INCLUDES} ${LibRaw_r_INCLUDES}/libraw
  )
  target_link_libraries(IO
    PUBLIC
      ${OPENIMAGEIO_LIBRARIES} ${LibRaw_r_LIBRARIES}
  )
else()
  message(STATUS "  Not adding OpenImageIO nodes")
endif()

if(${FFMPEG_FOUND})
  message(STATUS "  Adding FFmpeg nodes")
  target_compile_definitions(IO PRIVATE OFX_IO_MT_FFMPEG)
  target_include_directories(IO
    PUBLIC
      ${FFMPEG_INCLUDES}
  )
  target_link_libraries(IO
    PUBLIC
      ${FFMPEG_LIBRARIES}
  )
else()
  message(STATUS "  Not adding FFmpeg nodes")
endif()

if(${PNG_FOUND})
  message(STATUS "  Adding PNG nodes")
  target_compile_definitions(IO PRIVATE ${PNG_DEFINITIONS})
  target_include_directories(IO
    PUBLIC
      ${PNG_INCLUDE_DIRS}
  )
  target_link_libraries(IO
    PUBLIC
      ${PNG_LIBRARIES}
  )
else()
  message(STATUS "  Not adding PNG nodes")
endif()

if(${SEEXPR2_FOUND})
  message(STATUS "  Adding SeExpr nodes")
  target_include_directories(IO
    PUBLIC
      ${SEEXPR2_INCLUDES}
  )
  target_link_libraries(IO
    PUBLIC
      ${SEEXPR2_LIBRARIES}
  )
else()
  message(STATUS "  Not adding SeExpr nodes")
endif()


# Find and set the arch name.
# http://openeffects.org/documentation/reference/ch02s02.html
set(OFX_ARCH UNKNOWN)
# Win
if(MINGW)
    set(OFX_ARCH "Win32")
    if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(OFX_ARCH "Win64")
    endif()
# Visual Studio    
elseif(MSVC)
    set(OFX_ARCH "Win32")
    if((${CMAKE_SYSTEM_PROCESSOR} STREQUAL "IA64") OR (${CMAKE_SYSTEM_PROCESSOR} STREQUAL "AMD64"))
        set(OFX_ARCH "Win64")
    endif()
# FreeBSD
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "FreeBSD")
    set(OFX_ARCH "FreeBSD-x86")
    if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(OFX_ARCH "FreeBSD-x86-64")
    endif()
    if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
        if("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
            set(OFX_ARCH FreeBSD-arm-64)
        else()
            set(OFX_ARCH FreeBSD-arm)
        endif()
    endif()
# MacOS
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Darwin")
    set(OFX_ARCH "MacOS")
# Linux
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(OFX_ARCH "Linux-x86")
    if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
        set(OFX_ARCH "Linux-x86-64")
    endif()
    if(${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm")
        if("${CMAKE_SIZEOF_VOID_P}" EQUAL "8")
            set(OFX_ARCH Linux-arm-64)
        else()
            set(OFX_ARCH Linux-arm)
        endif()
    endif()
endif()

set(OFX_ARCH_NAME ${OFX_ARCH} CACHE STRING "OpenFX target OS and architecture")
if("${OFX_ARCH_NAME}" EQUAL "UNKNOWN")
  message(SEND_ERROR "OFX_ARCH_NAME was not detected. Install targets will be unavailable. See http://openeffects.org/documentation/reference/ch02s02.html to pick an arch name.")
else()
  install(TARGETS IO
    RUNTIME DESTINATION IO.ofx.bundle/Contents/${OFX_ARCH_NAME}
    LIBRARY DESTINATION IO.ofx.bundle/Contents/${OFX_ARCH_NAME}
  )

  install(FILES ${IO_RESOURCES}
    DESTINATION IO.ofx.bundle/Contents/Resources
  )

  install(FILES Info.plist
    DESTINATION IO.ofx.bundle/Contents
  )
endif()
