# CMake helper to compile openfx-io's IOSupport (also used by openfx-arena) into an OpenFX plug-in target.
#
#   include(<IOSupport dir>/cmake/IOSupport.cmake)
#   ioSupport_add_sources(<target> [ALL] [COMPONENTS <name>...])
#
# The sources are added to <target> (target_sources PRIVATE), so that they compile with the target's own defines
# (OFX_EXTENSIONS_*, OFX_SUPPORTS_OPENGLRENDER and OFX_IO_USING_OCIO change class layouts), with the IOSupport
# include directory. Components (dependencies are added automatically):
#   SequenceParsing  SequenceParsing/SequenceParsing.cpp
#   GenericOCIO      GenericOCIO.cpp, and its OpenGL rendering GenericOCIOOpenGL.cpp glsl.cpp
#                    (+ SupportExt OPENGL). With the target OpenColorIO::OpenColorIO, <target> is linked with it and
#                    gets OFX_IO_USING_OCIO.
#   GenericReader    GenericReader.cpp (+ SequenceParsing GenericOCIO, SupportExt THREADS MULTIPLANE)
#   GenericWriter    GenericWriter.cpp (+ SequenceParsing GenericOCIO, SupportExt FILEOPEN MULTIPLANE)
#   ALL              all of them
# SupportExt sources are added with supportext_add_sources() (SupportExt/cmake/SupportExt.cmake), which must be
# defined. Calling ioSupport_add_sources() several times for the same target is fine: each source is added once.
include_guard(GLOBAL)

get_filename_component(_iosupport_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set_property(GLOBAL PROPERTY IOSUPPORT_DIR "${_iosupport_dir}")
unset(_iosupport_dir)

set(_IOSUPPORT_SequenceParsing_SOURCES SequenceParsing/SequenceParsing.cpp)
set(_IOSUPPORT_GenericOCIO_SOURCES     GenericOCIO.cpp GenericOCIOOpenGL.cpp glsl.cpp)
set(_IOSUPPORT_GenericOCIO_SUPPORTEXT  OPENGL)
set(_IOSUPPORT_GenericReader_SOURCES   GenericReader.cpp)
set(_IOSUPPORT_GenericReader_DEPENDS   SequenceParsing GenericOCIO)
set(_IOSUPPORT_GenericReader_SUPPORTEXT THREADS MULTIPLANE)
set(_IOSUPPORT_GenericWriter_SOURCES   GenericWriter.cpp)
set(_IOSUPPORT_GenericWriter_DEPENDS   SequenceParsing GenericOCIO)
set(_IOSUPPORT_GenericWriter_SUPPORTEXT FILEOPEN MULTIPLANE)
set(_IOSUPPORT_COMPONENTS SequenceParsing GenericOCIO GenericReader GenericWriter)

function(ioSupport_add_sources TARGET)
  cmake_parse_arguments(IS "ALL" "" "COMPONENTS" ${ARGN})
  get_property(IOSUPPORT_DIR GLOBAL PROPERTY IOSUPPORT_DIR)
  if(NOT TARGET ${TARGET})
    message(FATAL_ERROR "ioSupport_add_sources: ${TARGET} is not a target")
  endif()
  if(IS_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "ioSupport_add_sources: unknown arguments: ${IS_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT COMMAND supportext_add_sources)
    message(FATAL_ERROR "ioSupport_add_sources: include SupportExt/cmake/SupportExt.cmake first (supportext_add_sources)")
  endif()

  set(_todo ${IS_COMPONENTS})
  if(IS_ALL)
    list(APPEND _todo ${_IOSUPPORT_COMPONENTS})
  endif()
  # resolve the dependencies
  set(_done "")
  while(_todo)
    list(GET _todo 0 _c)
    list(REMOVE_AT _todo 0)
    if(NOT DEFINED _IOSUPPORT_${_c}_SOURCES)
      message(FATAL_ERROR "ioSupport_add_sources: unknown component ${_c} (known: ${_IOSUPPORT_COMPONENTS})")
    endif()
    if(NOT _c IN_LIST _done)
      list(APPEND _done ${_c})
      list(APPEND _todo ${_IOSUPPORT_${_c}_DEPENDS})
    endif()
  endwhile()

  # sources, once per target
  get_property(_added TARGET ${TARGET} PROPERTY IOSUPPORT_COMPONENTS)
  set(_sources "")
  set(_supportext "")
  foreach(_c ${_done})
    if(NOT _c IN_LIST _added)
      foreach(_s ${_IOSUPPORT_${_c}_SOURCES})
        list(APPEND _sources "${IOSUPPORT_DIR}/${_s}")
      endforeach()
      list(APPEND _supportext ${_IOSUPPORT_${_c}_SUPPORTEXT})
      set_property(TARGET ${TARGET} APPEND PROPERTY IOSUPPORT_COMPONENTS ${_c})
    endif()
  endforeach()
  if(_sources)
    target_sources(${TARGET} PRIVATE ${_sources})
  endif()
  if(_supportext)
    # OPENGL is an option of supportext_add_sources(), not a component
    set(_opts "")
    if("OPENGL" IN_LIST _supportext)
      list(REMOVE_ITEM _supportext OPENGL)
      set(_opts OPENGL)
    endif()
    list(REMOVE_DUPLICATES _supportext)
    supportext_add_sources(${TARGET} ${_opts} COMPONENTS ${_supportext})
  endif()

  # include directory and OpenColorIO, once per target
  get_property(_setup TARGET ${TARGET} PROPERTY IOSUPPORT_SETUP)
  if(NOT _setup)
    set_property(TARGET ${TARGET} PROPERTY IOSUPPORT_SETUP ON)
    target_include_directories(${TARGET} PRIVATE "${IOSUPPORT_DIR}")
  endif()
  get_property(_ocio TARGET ${TARGET} PROPERTY IOSUPPORT_OCIO_LINKED)
  if("GenericOCIO" IN_LIST _done AND TARGET OpenColorIO::OpenColorIO AND NOT _ocio)
    set_property(TARGET ${TARGET} PROPERTY IOSUPPORT_OCIO_LINKED ON)
    set_property(TARGET ${TARGET} APPEND PROPERTY COMPILE_DEFINITIONS OFX_IO_USING_OCIO)
    set_property(TARGET ${TARGET} APPEND PROPERTY LINK_LIBRARIES OpenColorIO::OpenColorIO)
  endif()
endfunction()
