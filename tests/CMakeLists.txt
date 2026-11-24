# The tests of tests/Makefile, as ctest tests of the CMake build (add_subdirectory(tests) from the top-level
# CMakeLists.txt, with BUILD_TESTING): the same programs, sources, defines and flags, and the same runs, one ctest test
# per run (test-<Makefile target>[-<variant>]). They need the libraries of the plug-ins they link (the groups of
# _io_groups) and use this build's compiler, C++ standard (OFX_CXX_STANDARD) and FFmpeg (FFMPEG_PKG_CONFIG_PATH).
# The programs are built in build/tests, by the default target; they run there (their output files too).
#
# Like tests/Makefile, the build type doesn't apply to the test programs: -O0 -g for the tests that link the plug-ins,
# -O2 -g -Wall for the others, and assert() is enabled (no NDEBUG, no DEBUG).
#
# Options:
#   IO_TESTS_WITH_LIBRAW  the tests that link ReadOIIO also build its RAW support (OFX_IO_USING_LIBRAW), when the bundle
#                         has it (as tests/Makefile's WITH_LIBRAW=1); default ON
#   IO_TESTS_TIMEOUT      time limit of each test, in seconds (test-ffmpeg: twice as much); default 1800
#   IO_TESTS_FUZZ_OPS     operations of each short fuzz run of test-ocio-anim (tests/Makefile's FUZZ_OPS); default 2000
#   IO_TESTS_SANITIZERS   also build and run the sanitizer variants that tests/Makefile's `make test` doesn't run:
#                         test-ofx14-scan-asan, test-exr-tsan, test-exr-ubsan, test-exr-tsan-debug,
#                         test-ocio-processor-cache-tsan, test-oiio-layer-menu-tsan,
#                         test-readffmpeg-views-concurrent-tsan, test-writeffmpeg-codec-edit-during-begin-tsan
#                         (needs a compiler with -fsanitize=thread,undefined; not with MinGW); default OFF
# (tests/Makefile's bench-exr is not a test: run it with make.)

option(IO_TESTS_WITH_LIBRAW "Build ReadOIIO's RAW support in the tests that link it, when the bundle has it" ON)
set(IO_TESTS_FUZZ_OPS 2000 CACHE STRING "Operations of each short fuzz run of test-ocio-anim")
option(IO_TESTS_SANITIZERS "Also build and run the ThreadSanitizer, UBSan and ASan variants that `make test` doesn't run" OFF)

set(S "${PROJECT_SOURCE_DIR}")
set(T "${CMAKE_CURRENT_SOURCE_DIR}")
set(B "${CMAKE_CURRENT_BINARY_DIR}")

macro(_io_has _g _var)
  if(" ${_io_groups} " MATCHES " ${_g} ")
    set(${_var} ON)
  else()
    set(${_var} OFF)
  endif()
endmacro()
_io_has(FFmpeg _io_t_ffmpeg)
_io_has(OIIO _io_t_oiio)
_io_has(OCIO _io_t_ocio)
_io_has(EXR _io_t_exr)
_io_has(PNG _io_t_png)
_io_has(SeExpr _io_t_seexpr)
_io_has(RunScript _io_t_runscript)
set(_io_t_libraw OFF)
if(IO_TESTS_WITH_LIBRAW AND _io_t_oiio AND OFX_IO_WITH_LIBRAW AND TARGET PkgConfig::LIBRAW)
  set(_io_t_libraw ON)
endif()

# The build type's flags (e.g. Release's -O3 -DNDEBUG) don't apply in this directory: each program gets those of
# tests/Makefile
foreach(_c DEBUG RELEASE RELWITHDEBINFO MINSIZEREL ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
  string(TOUPPER "${_c}" _c)
  set(CMAKE_CXX_FLAGS_${_c} "")
endforeach()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# AddressSanitizer, except with MinGW (MS Windows), which doesn't have it (tests/Makefile's OCIO_FT_ASAN)
if(WIN32)
  set(_io_asan "")
else()
  set(_io_asan -fsanitize=address -fno-omit-frame-pointer)
endif()

# the OFX C++ Support library (host-facing plug-in runtime), as tests/Makefile's OFX_SUPPORT: ofxsDrawContextOpenGL.cpp
# (DrawArgs::isOpenGLContextCurrent, used by SupportExt's OverlayDraw) loads OpenGL at run time
set(_io_ofx_support
  ${S}/openfx/Support/Library/ofxsCore.cpp ${S}/openfx/Support/Library/ofxsImageEffect.cpp
  ${S}/openfx/Support/Library/ofxsInteract.cpp ${S}/openfx/Support/Library/ofxsLog.cpp
  ${S}/openfx/Support/Library/ofxsMultiThread.cpp ${S}/openfx/Support/Library/ofxsParams.cpp
  ${S}/openfx/Support/Library/ofxsProperty.cpp ${S}/openfx/Support/Library/ofxsPropertyValidation.cpp
  ${S}/openfx/Support/Library/ofxsDrawContextOpenGL.cpp)

# the libraries of the OFX tests (tests/Makefile's OFX_CXXFLAGS and OFX_LIBS): OpenColorIO, OpenImageIO, FFmpeg (and
# OpenEXR: OIIOGlobal.cpp includes <IlmThreadPool.h>), the plug-ins' defines and in-repo include paths
if(_io_t_ocio AND _io_t_oiio AND _io_t_ffmpeg)
  add_library(io_tests_ofx INTERFACE)
  target_compile_definitions(io_tests_ofx INTERFACE OFX_EXTENSIONS_VEGAS OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_TUTTLE
                             OFX_EXTENSIONS_NATRON OFX_IO_MT_FFMPEG OFX_IO_USING_OCIO)
  # FFmpeg's include directories as -I, before the -isystem of OpenColorIO's or OpenImageIO's prefix, which may have
  # another FFmpeg (as for the IO target)
  target_include_directories(io_tests_ofx INTERFACE
    ${FFMPEG_INCLUDE_DIRS}
    ${S}/FFmpeg ${S}/OIIO ${S}/RunScript ${S}/IOSupport ${S}/IOSupport/SequenceParsing ${S}/SupportExt
    ${S}/openfx/include ${S}/openfx/Support/include)
  target_link_libraries(io_tests_ofx INTERFACE OpenColorIO::OpenColorIO OpenImageIO::OpenImageIO PkgConfig::FFMPEG
                        $<TARGET_NAME_IF_EXISTS:OpenEXR::OpenEXR>)
  if(_io_t_libraw)
    target_compile_definitions(io_tests_ofx INTERFACE OFX_IO_USING_LIBRAW)
    target_link_libraries(io_tests_ofx INTERFACE PkgConfig::LIBRAW)
  endif()
  # Work around a oneTBB 2023.2 exit crash (tests/Makefile's TBBMALLOC_LIBS): libtbb dlopen()s libtbbmalloc and frees
  # memory through it from a static destructor at exit, after it was unloaded, in programs that link TBB (through
  # OpenImageIO) but never use it. Linking libtbbmalloc keeps it loaded. Only if it is installed next to libtbb.
  if(PKG_CONFIG_FOUND)
    pkg_get_variable(_io_tbb_libdir tbb libdir)
    if(_io_tbb_libdir)
      find_library(IO_TESTS_TBBMALLOC_LIBRARY NAMES tbbmalloc PATHS "${_io_tbb_libdir}" NO_DEFAULT_PATH)
      if(IO_TESTS_TBBMALLOC_LIBRARY)
        target_link_libraries(io_tests_ofx INTERFACE "${IO_TESTS_TBBMALLOC_LIBRARY}")
      endif()
    endif()
  endif()
endif()

# io_test_program(<name> [OFX] [THREADS] SOURCES <file>... [DEFINITIONS <def>...] [INCLUDES <dir>...]
#                 [SANITIZE <flag>...] [LIBS <lib>...])
# A test program, built from its sources only (as tests/Makefile, which compiles every source with the flags of the
# program). OFX: one that links the plug-ins (OFX_SUPPORT, io_tests_ofx, -O0 -g); otherwise -O2 -g -Wall.
# THREADS: -pthread. SANITIZE: compile and link flags.
function(io_test_program name)
  cmake_parse_arguments(PARSE_ARGV 1 P "OFX;THREADS" "" "SOURCES;DEFINITIONS;INCLUDES;SANITIZE;LIBS")
  if(P_OFX)
    add_executable(${name} ${P_SOURCES} ${_io_ofx_support})
    target_compile_options(${name} PRIVATE -O0 -g)
    target_link_libraries(${name} PRIVATE io_tests_ofx)
    if(_io_t_libraw AND "${LIBRAW_CFLAGS_OTHER};${LIBRAW_LDFLAGS_OTHER};${LIBRAW_LIBRARIES}" MATCHES "openmp|(^|;)-?l?gomp(;|$)|(^|;)-?l?omp(;|$)")
      # LibRaw's headers include <omp.h> when compiled with OpenMP (as for the IO target)
      ofx_target_openmp(${name})
    endif()
  else()
    add_executable(${name} ${P_SOURCES})
    target_compile_options(${name} PRIVATE -O2 -g -Wall)
  endif()
  set_target_properties(${name} PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  target_compile_definitions(${name} PRIVATE ${P_DEFINITIONS})
  target_include_directories(${name} PRIVATE ${P_INCLUDES})
  if(P_SANITIZE)
    target_compile_options(${name} PRIVATE ${P_SANITIZE})
    target_link_options(${name} PRIVATE ${P_SANITIZE})
  endif()
  if(P_THREADS)
    target_link_libraries(${name} PRIVATE Threads::Threads)
  endif()
  target_link_libraries(${name} PRIVATE ${P_LIBS})
endfunction()

# io_test(<name> [DIR <work directory>] [TIMEOUT <seconds>] [ENV <var=value>...] COMMAND <program> [<arguments>...])
# A ctest test, run in build/tests. DIR: removed and created again before the run (tests/run_test.cmake). TIMEOUT
# (default IO_TESTS_TIMEOUT): a hung test fails instead of running into the CI job's time limit.
set(IO_TESTS_TIMEOUT 1800 CACHE STRING "Time limit of each test-* ctest test, in seconds (test-ffmpeg: twice as much)")
function(io_test name)
  cmake_parse_arguments(PARSE_ARGV 1 P "" "DIR;TIMEOUT" "ENV;COMMAND")
  if(NOT P_TIMEOUT)
    set(P_TIMEOUT ${IO_TESTS_TIMEOUT})
  endif()
  if(P_DIR)
    add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DDIR=${P_DIR}" -P "${T}/run_test.cmake" -- ${P_COMMAND}
             WORKING_DIRECTORY "${B}")
  else()
    add_test(NAME ${name} COMMAND ${P_COMMAND} WORKING_DIRECTORY "${B}")
  endif()
  set_tests_properties(${name} PROPERTIES TIMEOUT ${P_TIMEOUT})
  if(P_ENV)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${P_ENV}")
  endif()
endfunction()

set(_io_ocio_env "OCIO=${T}/test.ocio")
set(_io_swap_env "OCIO=${T}/swap.ocio")
set(_io_asan_env "ASAN_OPTIONS=detect_leaks=0 $ENV{ASAN_OPTIONS}")
find_program(IO_BASH NAMES bash)

# ---- test-ffmpeg: the FFmpeg reader-core harness (FFmpegFile.cpp), on clips that the ffmpeg CLI generates
if(_io_t_ffmpeg)
  io_test_program(ffmpeg_read_test
    SOURCES ${T}/ffmpeg_read_test.cpp ${S}/FFmpeg/FFmpegFile.cpp ${S}/FFmpeg/PixelFormat.cpp ${T}/ofx_stubs.cpp
    INCLUDES ${FFMPEG_INCLUDE_DIRS} ${S}/FFmpeg ${S}/IOSupport ${S}/SupportExt ${S}/openfx/include
             ${S}/openfx/Support/include
    LIBS PkgConfig::FFMPEG)
  if(IO_BASH)
    # (about 290 s on a recent Mac, 2026-11-23, of which about 15 s for the forward-request clips of #311; MS Windows
    # runners are much slower)
    math(EXPR _io_t "${IO_TESTS_TIMEOUT} * 2")
    io_test(test-ffmpeg TIMEOUT ${_io_t} ENV "FFMPEG_READ_TEST_BIN=$<TARGET_FILE:ffmpeg_read_test>"
            COMMAND "${IO_BASH}" "${T}/run_ffmpeg_read_test.sh")
  endif()
endif()

# ---- test-oiio: oiiotool format round-trip (OpenImageIO library sanity only, nothing to build)
if(_io_t_oiio AND IO_BASH)
  io_test(test-oiio COMMAND "${IO_BASH}" "${T}/run_oiio_roundtrip_test.sh")
endif()

if(NOT IO_BASH AND (_io_t_ffmpeg OR _io_t_oiio))
  message(STATUS "bash not found: no test-ffmpeg and test-oiio tests")
endif()

# ---- test-sequence-parsing: SequenceParsing's own test (patterns, views, UTF-8 file names, negative frames, %d,
# padding, int limits, extensions), and test-sequence-parsing-utf8 (UTF-8 cases it doesn't cover) (#357). No library.
# With UBSan, except on MS Windows (MinGW has no UBSan runtime), for SequenceParsing's own arithmetic (INT_MIN,
# negation, stringToInt): UBSan can't detect #357, since the <cctype> preconditions are not core-language UB and the
# glibc and UCRT tables accept -128..255.
if(WIN32)
  set(_io_sp_ubsan "")
else()
  set(_io_sp_ubsan -fsanitize=undefined -fno-sanitize-recover=all)
endif()
io_test_program(sequence_parsing_test SOURCES ${S}/IOSupport/SequenceParsing/tests/SequenceParsingTest.cpp
                ${S}/IOSupport/SequenceParsing/SequenceParsing.cpp INCLUDES ${S}/IOSupport/SequenceParsing
                SANITIZE ${_io_sp_ubsan})
io_test(test-sequence-parsing COMMAND $<TARGET_FILE:sequence_parsing_test>)
io_test_program(sequence_parsing_utf8_test SOURCES ${T}/sequence_parsing_utf8_test.cpp
                ${S}/IOSupport/SequenceParsing/SequenceParsing.cpp INCLUDES ${S}/IOSupport/SequenceParsing
                SANITIZE ${_io_sp_ubsan})
io_test(test-sequence-parsing-utf8 COMMAND $<TARGET_FILE:sequence_parsing_utf8_test>)

# ---- the tests that link the plug-ins (tests/Makefile's OFX_PKGS: OpenColorIO, OpenImageIO, FFmpeg)
if(TARGET io_tests_ofx)
  # the reader/writer plug-ins and their in-repo dependencies (tests/Makefile's PLUGIN_SRCS)
  set(_io_plugin_srcs
    ${S}/FFmpeg/ReadFFmpeg.cpp ${S}/FFmpeg/WriteFFmpeg.cpp ${S}/FFmpeg/FFmpegFile.cpp ${S}/FFmpeg/PixelFormat.cpp
    ${S}/OIIO/ReadOIIO.cpp ${S}/OIIO/WriteOIIO.cpp ${S}/OIIO/OIIOGlobal.cpp
    ${S}/IOSupport/GenericReader.cpp ${S}/IOSupport/ExrThreads.cpp ${S}/IOSupport/GenericWriter.cpp ${S}/IOSupport/GenericOCIO.cpp
    ${S}/IOSupport/SequenceParsing/SequenceParsing.cpp
    ${S}/SupportExt/ofxsMultiPlane.cpp ${S}/SupportExt/ofxsThreadSuite.cpp
    ${S}/SupportExt/ofxsFileOpen.cpp ${S}/SupportExt/ofxsLut.cpp
    ${S}/RunScript/RunScript.cpp ${S}/RunScript/RunScriptWin32.cpp)
  # GenericReader and its in-repo dependencies only (READER_HOOKS_SRCS)
  set(_io_reader_srcs
    ${S}/IOSupport/GenericReader.cpp ${S}/IOSupport/ExrThreads.cpp ${S}/IOSupport/GenericOCIO.cpp ${S}/IOSupport/SequenceParsing/SequenceParsing.cpp
    ${S}/SupportExt/ofxsMultiPlane.cpp ${S}/SupportExt/ofxsThreadSuite.cpp
    ${S}/SupportExt/ofxsFileOpen.cpp ${S}/SupportExt/ofxsLut.cpp)
  # OpenGL render support, as in ../Makefile.master (OCIOPluginBase.cpp needs it): OCIO_CDL_CXXFLAGS and the
  # OpenGL sources of OCIO_CDL_SRCS; OCIO_CDL_LIBS: OpenGL, and libdl (glad's loaders use dlopen(), in libdl before
  # glibc 2.34)
  set(_io_gl_defs OFX_SUPPORTS_OPENGLRENDER)
  set(_io_gl_incs ${S}/OCIO ${S}/SupportExt/glad ${S}/openfx/Support/Plugins/include)
  set(_io_gl_srcs ${S}/IOSupport/GenericOCIOOpenGL.cpp ${S}/IOSupport/glsl.cpp ${S}/SupportExt/glad/glad.cpp
                  ${S}/SupportExt/glad/gladegl.cpp ${S}/SupportExt/ofxsOGLUtilities.cpp)
  set(_io_gl_libs OpenGL::GL ${CMAKE_DL_LIBS})

  # test-ofx: OFX entry points (enumerate) and action dispatch (Load, Describe, DescribeInContext, Unload)
  io_test_program(ofx_entrypoints_test OFX SOURCES ${T}/ofx_entrypoints_test.cpp ${_io_plugin_srcs})
  io_test(test-ofx-entrypoints COMMAND ofx_entrypoints_test fr.inria.openfx.ReadFFmpeg fr.inria.openfx.WriteFFmpeg
          fr.inria.openfx.ReadOIIO fr.inria.openfx.WriteOIIO fr.inria.openfx.RunScript)
  io_test_program(ofx_actions_test OFX SOURCES ${T}/ofx_actions_test.cpp ${_io_plugin_srcs})
  io_test(test-ofx-actions COMMAND ofx_actions_test)

  # test-render: WriteFFmpeg encodes, ReadFFmpeg decodes (round trips)
  # (also WritePNG with libpng, for the disk-full part of test-writer-remove-partial-file)
  set(_io_render_srcs ${T}/ofx_render_test.cpp ${_io_plugin_srcs} ${S}/PFM/ReadPFM.cpp ${S}/PFM/WritePFM.cpp)
  set(_io_render_libs)
  if(_io_t_png)
    list(APPEND _io_render_srcs ${S}/PNG/WritePNG.cpp)
    set(_io_render_libs io_png)
  endif()
  io_test_program(ofx_render_test OFX SOURCES ${_io_render_srcs} LIBS ${_io_render_libs})
  io_test(test-render ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov")
  # test-dnxhr444-noise (#267): DNxHR 444 on binary RGB noise, FFmpeg's dnxhd encoder must not abort
  io_test(test-dnxhr444-noise ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i267.dnxhr444.mov" --dnxhr444-noise)
  # test-writeffmpeg-begin-failure (#289): a beginSequenceRender that fails after a render of the same instance, then
  # endSequenceRender: no crash, and the next render works
  io_test(test-writeffmpeg-begin-failure ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov" --begin-failure)
  # test-readffmpeg-destroy-closes-file (#290): DestroyInstance closes the movie file, before Unload
  io_test(test-readffmpeg-destroy-closes-file ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i290.mov" --destroy-closes-file)
  # test-readffmpeg-views-concurrent (#291): concurrent renders of the views of a movie with one video stream per view
  # (written by the ffmpeg CLI): each render gets its own view
  io_test(test-readffmpeg-views-concurrent ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i291.mov" --views-concurrent)
  if(IO_TESTS_SANITIZERS)
    # test-readffmpeg-views-concurrent-tsan: the same with ThreadSanitizer
    io_test_program(ofx_render_test_tsan OFX THREADS SOURCES ${T}/ofx_render_test.cpp ${_io_plugin_srcs} SANITIZE -fsanitize=thread)
    io_test(test-readffmpeg-views-concurrent-tsan ENV "${_io_ocio_env}" "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}"
            COMMAND ofx_render_test_tsan "${B}/.render_out.mov.i291.tsan.mov" --views-concurrent)
  endif()
  # test-writeffmpeg-dnxhd-profiles (#305): each DNxHD/DNxHR Codec Profile in 1080p and 720p at 23.976 to 60 fps: the
  # expected compression ID, or a clear error where the profile does not exist
  io_test(test-writeffmpeg-dnxhd-profiles TIMEOUT 600 ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov" --dnxhd-profiles)
  # test-writeffmpeg-encode-failure (#288): frame 1 is aborted or fails a check of encode() while other threads wait
  # for their turn: they fail instead of waiting forever (the test exits after 60 s; the ctest time limit is a backstop)
  io_test(test-writeffmpeg-encode-failure TIMEOUT 300 ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i288.mov" --encode-failure)
  # test-writeffmpeg-encode-failure-disk-full (#288): the same, frame 1 fails to be written (RLIMIT_FSIZE); skipped
  # (exit status 77) on MS Windows
  io_test(test-writeffmpeg-encode-failure-disk-full TIMEOUT 300 ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i288-disk-full.mov" --encode-failure-disk-full)
  set_tests_properties(test-writeffmpeg-encode-failure-disk-full PROPERTIES SKIP_RETURN_CODE 77)
  # test-writeoiio-disk-full (#292, #362): WriteOIIO, the file can not grow beyond 16 KB (RLIMIT_FSIZE, as on a full
  # disk): the render fails, and the process survives; skipped (exit status 77) on MS Windows
  io_test(test-writeoiio-disk-full ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.i292" --writeoiio-disk-full)
  set_tests_properties(test-writeoiio-disk-full PROPERTIES SKIP_RETURN_CODE 77)
  # test-writeoiio-files (#362): WriteOIIO, a 2-view multi-part EXR and an htj2k JPEG 2000 (OpenJPH), which OpenImageIO
  # writes by name, are written and read back
  io_test(test-writeoiio-files ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.i362" --writeoiio-files)
  # test-writer-remove-partial-file (#251): a failed or aborted render of an image file leaves no partial file (a test
  # writer; WritePNG and RLIMIT_FSIZE, except on MS Windows and without libpng), and a render that fails before the
  # file is written keeps the previous one
  io_test(test-writer-remove-partial-file ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out" --writer-remove-partial-file)
  # test-swscale-exact (#207, #220, #316): the conversions between Y'CbCr and RGB of ReadFFmpeg and WriteFFmpeg give
  # exact values (rawvideo written and decoded without swscale)
  io_test(test-swscale-exact ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov" --swscale-exact)
  # test-writeffmpeg-param-change (#304): param and clip changes (kOfxActionInstanceChanged) and
  # kOfxActionSyncPrivateData between the frames of a render: every frame is encoded
  io_test(test-writeffmpeg-param-change ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i304.mov" --param-change)
  # test-writeffmpeg-codec-params (#343): the settings that WriteFFmpeg shows and the pixel format it selects for each
  # encoder of its Codec menu, and each encoder encodes with them
  io_test(test-writeffmpeg-codec-params ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.i343" --codec-params)
  # test-writeffmpeg-codec-edit-during-begin (#319): the Codec edited on the main thread while beginSequenceRender runs
  # on a render thread: every frame is encoded
  io_test(test-writeffmpeg-codec-edit-during-begin ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov.i319.mov" --codec-edit-during-begin)
  if(IO_TESTS_SANITIZERS AND NOT WIN32)
    # test-writeffmpeg-codec-edit-during-begin-tsan: the same with ThreadSanitizer (ofx_render_test_tsan, above), no
    # data race in WriteFFmpeg
    io_test(test-writeffmpeg-codec-edit-during-begin-tsan ENV "${_io_ocio_env}" "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}"
            COMMAND ofx_render_test_tsan "${B}/.render_out.mov.i319tsan.mov" --codec-edit-during-begin)
  endif()
  # test-pfm-write-errors (#294): WritePFM fails the render when the file can not grow (RLIMIT_FSIZE); skipped (exit
  # status 77) on MS Windows
  io_test(test-pfm-write-errors ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov" --pfm-write-errors)
  set_tests_properties(test-pfm-write-errors PROPERTIES SKIP_RETURN_CODE 77)
  # test-pfm-read-errors (#296): ReadPFM's getRegionOfDefinition fails on a header with an empty, negative or huge size
  io_test(test-pfm-read-errors ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.mov" --pfm-read-errors)
  # test-writeffmpeg-selected-encoder (#348): WriteFFmpeg encodes with the encoder selected in the Codec menu, not with
  # the default encoder of its codec ID; skipped (exit status 77) if the FFmpeg build has no such encoder to check
  io_test(test-writeffmpeg-selected-encoder ENV "${_io_ocio_env}" COMMAND ofx_render_test "${B}/.render_out.i348" --selected-encoder)
  set_tests_properties(test-writeffmpeg-selected-encoder PROPERTIES SKIP_RETURN_CODE 77)

  # test-layers: ReadOIIO's EXR channel name -> view/layer mapping (the test includes ReadOIIO.cpp)
  set(_io_srcs ${_io_plugin_srcs})
  list(REMOVE_ITEM _io_srcs ${S}/OIIO/ReadOIIO.cpp)
  io_test_program(oiio_layers_test OFX SOURCES ${T}/oiio_layers_test.cpp ${_io_srcs})
  io_test(test-layers COMMAND oiio_layers_test)

  # test-reader-hooks: GenericReader's hooks, on test readers
  io_test_program(reader_hooks_test OFX THREADS SOURCES ${T}/reader_hooks_test.cpp ${_io_reader_srcs})
  io_test(test-reader-hooks ENV "${_io_ocio_env}" COMMAND reader_hooks_test "${B}/.reader_hooks")

  # test-ocio-cdl: OCIOCDLTransform's Export param writes a .cc file that OCIO reads back
  set(_io_ocio_cdl_srcs ${S}/OCIO/OCIOCDLTransform.cpp ${S}/OCIO/OCIOPluginBase.cpp ${S}/IOSupport/GenericOCIO.cpp
      ${_io_gl_srcs} ${S}/SupportExt/ofxsThreadSuite.cpp ${S}/SupportExt/ofxsFileOpen.cpp)
  io_test_program(ocio_cdl_test OFX SOURCES ${T}/ocio_cdl_test.cpp ${_io_ocio_cdl_srcs}
                  DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_gl_incs} LIBS ${_io_gl_libs})
  io_test(test-ocio-cdl ENV "${_io_ocio_env}" COMMAND ocio_cdl_test)

  # test-ocio-filetransform: OCIOFileTransform's CCC Id param, with AddressSanitizer
  set(_io_srcs ${_io_ocio_cdl_srcs})
  list(REMOVE_ITEM _io_srcs ${S}/OCIO/OCIOCDLTransform.cpp)
  io_test_program(ocio_filetransform_test OFX SOURCES ${T}/ocio_filetransform_test.cpp ${S}/OCIO/OCIOFileTransform.cpp
                  ${_io_srcs} DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_gl_incs} SANITIZE ${_io_asan} LIBS ${_io_gl_libs})
  io_test(test-ocio-filetransform ENV "${_io_ocio_env}" "${_io_asan_env}" COMMAND ocio_filetransform_test)

  # test-ocio-processor-cache: the processor caches of the OCIO plug-ins (#282, #283, #284) and GenericOCIO's config
  # (#275), and with ThreadSanitizer
  set(_io_ocio_pc_srcs ${S}/OCIO/OCIOColorSpace.cpp ${S}/OCIO/OCIODisplay.cpp ${S}/OCIO/OCIOFileTransform.cpp
      ${S}/OCIO/OCIOLogConvert.cpp ${S}/OCIO/OCIOLookTransform.cpp ${_io_ocio_cdl_srcs})
  io_test_program(ocio_processor_cache_test OFX THREADS SOURCES ${T}/ocio_processor_cache_test.cpp ${_io_ocio_pc_srcs}
                  DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_gl_incs} LIBS ${_io_gl_libs})
  # (with DIR, run_test.cmake runs the program: give its path, not the target name)
  io_test(test-ocio-processor-cache DIR "${B}/.ocio_processor_cache"
          COMMAND $<TARGET_FILE:ocio_processor_cache_test> "${B}/.ocio_processor_cache")
  if(IO_TESTS_SANITIZERS)
    io_test_program(ocio_processor_cache_test_tsan OFX THREADS SOURCES ${T}/ocio_processor_cache_test.cpp ${_io_ocio_pc_srcs}
                    DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_gl_incs} SANITIZE -fsanitize=thread LIBS ${_io_gl_libs})
    io_test(test-ocio-processor-cache-tsan DIR "${B}/.ocio_processor_cache_tsan"
            ENV "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}"
            COMMAND $<TARGET_FILE:ocio_processor_cache_test_tsan> "${B}/.ocio_processor_cache_tsan")
  endif()

  # test-ocio-anim: the animated colorspaces of GenericOCIO, in a host that animates strings and choices like Natron,
  # and short fuzz runs of each plug-in, with fixed seeds
  set(_io_ocio_anim_srcs ${S}/OCIO/OCIOColorSpace.cpp ${S}/OCIO/OCIOPluginBase.cpp ${_io_gl_srcs} ${_io_plugin_srcs})
  io_test_program(ocio_colorspace_anim_test OFX SOURCES ${T}/ocio_colorspace_anim_test.cpp ${_io_ocio_anim_srcs}
                  DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_gl_incs} LIBS ${_io_gl_libs})
  # (ocio_anim_fuzz.cpp includes ocio_colorspace_anim_test.cpp)
  io_test_program(ocio_anim_fuzz OFX SOURCES ${T}/ocio_anim_fuzz.cpp ${_io_ocio_anim_srcs}
                  DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_gl_incs} LIBS ${_io_gl_libs})
  set(_n ${IO_TESTS_FUZZ_OPS})
  io_test(test-ocio-anim ENV "${_io_swap_env}" COMMAND ocio_colorspace_anim_test)
  io_test(test-ocio-anim-fuzz-cs ENV "${_io_swap_env}" COMMAND ocio_anim_fuzz cs ${_n} 1)
  io_test(test-ocio-anim-fuzz-writer ENV "${_io_swap_env}" COMMAND ocio_anim_fuzz writer ${_n} 2)
  io_test(test-ocio-anim-fuzz-reader ENV "${_io_swap_env}" COMMAND ocio_anim_fuzz reader ${_n} 3)
  io_test(test-ocio-anim-not-natron ENV "${_io_swap_env}" OCIO_ANIM_NOT_NATRON=1 COMMAND ocio_colorspace_anim_test)
  io_test(test-ocio-anim-fuzz-cs-not-natron ENV "${_io_swap_env}" OCIO_ANIM_NOT_NATRON=1 COMMAND ocio_anim_fuzz cs ${_n} 4)
  io_test(test-ocio-anim-fuzz-cs-near ENV "${_io_swap_env}" FUZZ_NEAR=1 COMMAND ocio_anim_fuzz cs ${_n} 5)
  # (seeds that found bugs in the review)
  io_test(test-ocio-anim-fuzz-reader-near-32 ENV "${_io_swap_env}" FUZZ_NEAR=1 COMMAND ocio_anim_fuzz reader 20000 32)
  io_test(test-ocio-anim-fuzz-cs-near-77 ENV "${_io_swap_env}" FUZZ_NEAR=1 COMMAND ocio_anim_fuzz cs 50000 77)
  io_test(test-ocio-anim-fuzz-cs-near-102 ENV "${_io_swap_env}" FUZZ_NEAR=1 COMMAND ocio_anim_fuzz cs 50000 102)

  # test-oiio-threads: OpenImageIO's thread pool is stopped when the last OIIO plug-in is unloaded, not the first one
  io_test_program(oiio_threadpool_test OFX SOURCES ${T}/oiio_threadpool_test.cpp ${_io_plugin_srcs} ${S}/OIIO/OIIOResize.cpp
                  INCLUDES ${S}/openfx/Support/Plugins/include)
  io_test(test-oiio-threads ENV "${_io_ocio_env}" COMMAND oiio_threadpool_test)
  if(IO_TESTS_SANITIZERS)
    # test-oiio-threads-tsan: the same with ThreadSanitizer (the instances are counted from several threads, #250)
    io_test_program(oiio_threadpool_test_tsan OFX THREADS SOURCES ${T}/oiio_threadpool_test.cpp ${_io_plugin_srcs}
                    ${S}/OIIO/OIIOResize.cpp INCLUDES ${S}/openfx/Support/Plugins/include SANITIZE -fsanitize=thread)
    io_test(test-oiio-threads-tsan ENV "${_io_ocio_env}" "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}"
            COMMAND oiio_threadpool_test_tsan)
  endif()

  # test-oiio-edge: OIIOResize and OIIOText with degenerate inputs, with OpenGL (OIIOText's interact)
  io_test_program(oiio_edge_test OFX SOURCES ${T}/oiio_edge_test.cpp ${_io_plugin_srcs} ${S}/OIIO/OIIOResize.cpp
                  ${S}/OIIO/OIIOText.cpp ${S}/SupportExt/ofxsOGLTextRenderer.cpp ${S}/SupportExt/ofxsOGLFontData.cpp
                  INCLUDES ${S}/openfx/Support/Plugins/include LIBS ${_io_gl_libs})
  io_test(test-oiio-edge ENV "${_io_ocio_env}" COMMAND oiio_edge_test)

  # test-reader-window: ReadOIIO, ReadPFM and ReadPNG (with libpng): render windows narrower than the image, render
  # scale 1/2, files rewritten while rendering, with AddressSanitizer
  set(_io_srcs ${S}/OIIO/ReadOIIO.cpp ${S}/OIIO/OIIOGlobal.cpp ${S}/PFM/ReadPFM.cpp)
  set(_io_defs "")
  set(_io_libs "")
  if(_io_t_png)
    list(APPEND _io_srcs ${S}/PNG/ReadPNG.cpp)
    set(_io_defs HAVE_PNG)
    set(_io_libs io_png)
  endif()
  # also UBSan's signed-integer-overflow check, stopping at the first error (PNG offsets near the limits of an int at
  # scales 1/2 and 1/4, #371), except with MinGW, as AddressSanitizer
  set(_io_rw_sanitize ${_io_asan})
  if(NOT WIN32)
    list(APPEND _io_rw_sanitize -fsanitize=signed-integer-overflow -fno-sanitize-recover=signed-integer-overflow)
  endif()
  io_test_program(reader_window_test OFX THREADS SOURCES ${T}/reader_window_test.cpp ${_io_srcs} ${_io_reader_srcs}
                  DEFINITIONS ${_io_defs} INCLUDES ${S}/PFM ${S}/PNG SANITIZE ${_io_rw_sanitize} LIBS ${_io_libs})
  io_test(test-reader-window DIR "${B}/.reader_window" ENV "${_io_ocio_env}" "${_io_asan_env}"
          COMMAND $<TARGET_FILE:reader_window_test> "${B}/.reader_window")
  # the 2-part EXR test, in a Natron-like host (tests/Makefile's second run of reader_window_test)
  io_test(test-reader-window-multiplane DIR "${B}/.reader_window_multiplane" ENV "${_io_ocio_env}" "${_io_asan_env}"
          READER_WINDOW_MULTIPLANE=1 COMMAND $<TARGET_FILE:reader_window_test> "${B}/.reader_window_multiplane")

  # test-oiio-layer-menu: ReadOIIO's output-layer menu rebuilt on the main thread while render threads render the
  # same instance and call isIdentity (#298), in a Natron-like host, with assert() (the menu's size in decodePlane)
  set(_io_layer_menu_srcs ${T}/oiio_layer_menu_test.cpp ${S}/OIIO/ReadOIIO.cpp ${S}/OIIO/OIIOGlobal.cpp ${_io_reader_srcs})
  io_test_program(oiio_layer_menu_test OFX THREADS SOURCES ${_io_layer_menu_srcs})
  io_test(test-oiio-layer-menu DIR "${B}/.oiio_layer_menu" ENV "${_io_ocio_env}"
          COMMAND $<TARGET_FILE:oiio_layer_menu_test> "${B}/.oiio_layer_menu")
  if(IO_TESTS_SANITIZERS AND NOT WIN32)
    # test-oiio-layer-menu-tsan: the same with ThreadSanitizer, without assert() (which would stop it before the report)
    io_test_program(oiio_layer_menu_test_tsan OFX THREADS SOURCES ${_io_layer_menu_srcs} DEFINITIONS NDEBUG
                    SANITIZE -fsanitize=thread)
    io_test(test-oiio-layer-menu-tsan DIR "${B}/.oiio_layer_menu_tsan" ENV "${_io_ocio_env}"
            "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}" COMMAND $<TARGET_FILE:oiio_layer_menu_test_tsan> "${B}/.oiio_layer_menu_tsan")
  endif()

  # test-png-corrupt: ReadPNG and WritePNG on libpng errors (#235): corrupt PNG files fail cleanly (no crash, no
  # AddressSanitizer report, no leak); an image offset out of range and a huge image (#276, #297). With
  # AddressSanitizer and UBSan's signed-integer-overflow check, which aborts (not on MS Windows)
  if(_io_t_png)
    if(WIN32)
      set(_io_png_san "")
    else()
      set(_io_png_san ${_io_asan} -fsanitize=signed-integer-overflow -fno-sanitize-recover=signed-integer-overflow)
    endif()
    io_test_program(png_corrupt_test OFX THREADS SOURCES ${T}/png_corrupt_test.cpp ${S}/PNG/ReadPNG.cpp ${S}/PNG/WritePNG.cpp
                    ${S}/IOSupport/GenericWriter.cpp ${_io_reader_srcs} INCLUDES ${S}/PNG SANITIZE ${_io_png_san} LIBS io_png)
    io_test(test-png-corrupt DIR "${B}/.png_corrupt" ENV "${_io_ocio_env}" "${_io_asan_env}"
            COMMAND $<TARGET_FILE:png_corrupt_test> "${B}/.png_corrupt")
  endif()

  # test-reader-window-host: the same readers and ReadFFmpeg, in a bundle of their own, rendered by reader_window_host
  # (hostdemo/reader_window.cpp), a host built on HostSupport (OfxHost: with IO_HOSTDEMO_TESTS, or
  # OFX_BUILD_HOSTSUPPORT) and hostDemo's host classes; hostdemo/reader_window.sh writes the files with oiiotool and
  # ffmpeg (skipped without oiiotool)
  if(TARGET OfxHost AND IO_BASH)
    set(_io_hd "${S}/openfx/HostSupport/examples")
    add_executable(reader_window_host ${T}/hostdemo/reader_window.cpp "${_io_hd}/hostDemoClipInstance.cpp"
                   "${_io_hd}/hostDemoEffectInstance.cpp" "${_io_hd}/hostDemoHostDescriptor.cpp"
                   "${_io_hd}/hostDemoParamInstance.cpp")
    target_compile_options(reader_window_host PRIVATE -O0 -g)
    target_include_directories(reader_window_host PRIVATE "${_io_hd}")
    target_link_libraries(reader_window_host PRIVATE OfxHost)
    set_target_properties(reader_window_host PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON
                          CXX_EXTENSIONS OFF)
    # the bundle (tests/Makefile's RW_BUNDLE_BINARY), with the flags of the tests that link the plug-ins
    set(_io_rw_bundle "${B}/.reader_window_host/plugins/ReaderWindow.ofx.bundle/Contents/${ARCHDIR}")
    add_library(io_reader_window_plugins MODULE ${_io_srcs} ${S}/FFmpeg/ReadFFmpeg.cpp ${S}/FFmpeg/FFmpegFile.cpp
                ${S}/FFmpeg/PixelFormat.cpp ${_io_reader_srcs} ${_io_ofx_support})
    target_compile_options(io_reader_window_plugins PRIVATE -O0 -g)
    target_compile_definitions(io_reader_window_plugins PRIVATE ${_io_defs})
    target_include_directories(io_reader_window_plugins PRIVATE ${S}/PFM ${S}/PNG)
    target_link_libraries(io_reader_window_plugins PRIVATE io_tests_ofx ${_io_libs})
    if(_io_t_libraw AND "${LIBRAW_CFLAGS_OTHER};${LIBRAW_LDFLAGS_OTHER};${LIBRAW_LIBRARIES}" MATCHES "openmp|(^|;)-?l?gomp(;|$)|(^|;)-?l?omp(;|$)")
      ofx_target_openmp(io_reader_window_plugins)
    endif()
    set_target_properties(io_reader_window_plugins PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON
                          CXX_EXTENSIONS OFF PREFIX "" SUFFIX ".ofx" OUTPUT_NAME ReaderWindow
                          LIBRARY_OUTPUT_DIRECTORY "${_io_rw_bundle}" RUNTIME_OUTPUT_DIRECTORY "${_io_rw_bundle}")
    foreach(_c ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER "${_c}" _c)
      set_target_properties(io_reader_window_plugins PROPERTIES LIBRARY_OUTPUT_DIRECTORY_${_c} "${_io_rw_bundle}"
                            RUNTIME_OUTPUT_DIRECTORY_${_c} "${_io_rw_bundle}")
    endforeach()
    io_test(test-reader-window-host ENV "${_io_ocio_env}"
            COMMAND "${IO_BASH}" "${T}/hostdemo/reader_window.sh" $<TARGET_FILE:reader_window_host>
                    "${B}/.reader_window_host/plugins" "${B}/.reader_window_host/work")
    set_tests_properties(test-reader-window-host PROPERTIES SKIP_RETURN_CODE 77)
    # test-exit-without-unload: a host that renders ReadOIIO, destroys its instance and exits without
    # kOfxActionUnload, 10 times, must exit (#250); linked with OpenImageIO, whose pool it also checks
    add_executable(exit_without_unload ${T}/hostdemo/exit_without_unload.cpp "${_io_hd}/hostDemoClipInstance.cpp"
                   "${_io_hd}/hostDemoEffectInstance.cpp" "${_io_hd}/hostDemoHostDescriptor.cpp"
                   "${_io_hd}/hostDemoParamInstance.cpp")
    target_compile_options(exit_without_unload PRIVATE -O0 -g)
    target_include_directories(exit_without_unload PRIVATE "${_io_hd}")
    target_link_libraries(exit_without_unload PRIVATE OfxHost OpenImageIO::OpenImageIO)
    if(IO_TESTS_TBBMALLOC_LIBRARY)
      # (the oneTBB exit crash of programs that link OpenImageIO, see io_tests_ofx)
      target_link_libraries(exit_without_unload PRIVATE "${IO_TESTS_TBBMALLOC_LIBRARY}")
    endif()
    set_target_properties(exit_without_unload PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON
                          CXX_EXTENSIONS OFF)
    add_dependencies(exit_without_unload io_reader_window_plugins)
    io_test(test-exit-without-unload ENV "${_io_ocio_env}"
            COMMAND "${IO_BASH}" "${T}/hostdemo/exit_without_unload.sh" $<TARGET_FILE:exit_without_unload>
                    "${B}/.reader_window_host/plugins" "${B}/.exit_without_unload")
    set_tests_properties(test-exit-without-unload PROPERTIES SKIP_RETURN_CODE 77)
    # test-raw-unload: a host that renders a DNG file with ReadOIIO (LibRaw: OpenMP on Linux and MS Windows), unloads
    # the plug-ins, keeps running, then exits, 5 times: the OpenMP runtime loaded after the render must still be loaded
    # after the unload, and no run may crash (#239); not linked with OpenImageIO, LibRaw or OpenMP, so that nothing
    # else keeps them loaded
    add_executable(raw_unload ${T}/hostdemo/raw_unload.cpp "${_io_hd}/hostDemoClipInstance.cpp"
                   "${_io_hd}/hostDemoEffectInstance.cpp" "${_io_hd}/hostDemoHostDescriptor.cpp"
                   "${_io_hd}/hostDemoParamInstance.cpp")
    target_compile_options(raw_unload PRIVATE -O0 -g)
    target_include_directories(raw_unload PRIVATE "${_io_hd}")
    target_link_libraries(raw_unload PRIVATE OfxHost)
    set_target_properties(raw_unload PROPERTIES CXX_STANDARD ${OFX_CXX_STANDARD} CXX_STANDARD_REQUIRED ON
                          CXX_EXTENSIONS OFF)
    add_dependencies(raw_unload io_reader_window_plugins)
    io_test(test-raw-unload ENV "${_io_ocio_env}"
            COMMAND "${IO_BASH}" "${T}/hostdemo/raw_unload.sh" $<TARGET_FILE:raw_unload>
                    "${B}/.reader_window_host/plugins" "${B}/.raw_unload")
    set_tests_properties(test-raw-unload PROPERTIES SKIP_RETURN_CODE 77)
  else()
    message(STATUS "tests: no test-reader-window-host (it needs HostSupport, e.g. IO_HOSTDEMO_TESTS=ON, and bash)")
  endif()

  if(_io_t_exr)
    # test-exr: ReadEXR: pixels, data/display windows, UINT, a truncated file, concurrent renders
    # (with OIIOGlobal.cpp: the test creates OIIO plug-in instances' OIIOThreadsUser while ReadEXR renders, #250)
    # (with ReadOIIO: ReadEXR presents the same planes in a multi-planar host, test-exr-multiplanar, #386)
    set(_io_exr_srcs ${T}/exr_read_test.cpp ${S}/EXR/ReadEXR.cpp ${S}/OIIO/ReadOIIO.cpp ${S}/OIIO/OIIOGlobal.cpp ${_io_reader_srcs})
    io_test_program(exr_read_test OFX THREADS SOURCES ${_io_exr_srcs} INCLUDES ${S}/EXR ${S}/OIIO)
    io_test(test-exr DIR "${B}/.exr_read" ENV "${_io_ocio_env}" COMMAND $<TARGET_FILE:exr_read_test> "${B}/.exr_read")
    io_test(test-exr-multiplanar DIR "${B}/.exr_read_multiplanar" ENV "${_io_ocio_env}"
            COMMAND $<TARGET_FILE:exr_read_test> --multiplanar "${B}/.exr_read_multiplanar")

    # test-ofx14 and test-ofx14-scan: every plug-in of IO.ofx (the sources of ../IO/Makefile; the SeExpr and PNG ones
    # if the bundle has them), in the OFX 1.4-only host of ofx_test_host.h, and in the full host, with OpenGL render
    # support
    set(_io_ofx14_srcs ${T}/ofx14_host_test.cpp
      ${S}/EXR/ReadEXR.cpp ${S}/EXR/WriteEXR.cpp
      ${S}/FFmpeg/ReadFFmpeg.cpp ${S}/FFmpeg/WriteFFmpeg.cpp ${S}/FFmpeg/FFmpegFile.cpp ${S}/FFmpeg/PixelFormat.cpp
      ${S}/OIIO/ReadOIIO.cpp ${S}/OIIO/WriteOIIO.cpp ${S}/OIIO/OIIOGlobal.cpp ${S}/OIIO/OIIOText.cpp ${S}/OIIO/OIIOResize.cpp
      ${S}/PFM/ReadPFM.cpp ${S}/PFM/WritePFM.cpp
      ${S}/OCIO/OCIOColorSpace.cpp ${S}/OCIO/OCIODisplay.cpp ${S}/OCIO/OCIOCDLTransform.cpp ${S}/OCIO/OCIOFileTransform.cpp
      ${S}/OCIO/OCIOLogConvert.cpp ${S}/OCIO/OCIOLookTransform.cpp ${S}/OCIO/OCIOPluginBase.cpp
      ${S}/IOSupport/GenericReader.cpp ${S}/IOSupport/ExrThreads.cpp ${S}/IOSupport/GenericWriter.cpp ${S}/IOSupport/GenericOCIO.cpp
      ${S}/IOSupport/GenericOCIOOpenGL.cpp ${S}/IOSupport/glsl.cpp ${S}/IOSupport/SequenceParsing/SequenceParsing.cpp
      ${S}/RunScript/RunScript.cpp ${S}/RunScript/RunScriptWin32.cpp
      ${S}/SupportExt/ofxsMultiPlane.cpp ${S}/SupportExt/ofxsThreadSuite.cpp
      ${S}/SupportExt/ofxsFileOpen.cpp ${S}/SupportExt/ofxsLut.cpp ${S}/SupportExt/ofxsOGLTextRenderer.cpp
      ${S}/SupportExt/ofxsOGLFontData.cpp ${S}/SupportExt/ofxsRamp.cpp ${S}/SupportExt/ofxsGenerator.cpp
      ${S}/SupportExt/ofxsTransformInteract.cpp ${S}/SupportExt/ofxsRectangleInteract.cpp ${S}/SupportExt/ofxsOGLUtilities.cpp
      ${S}/SupportExt/glad/glad.cpp ${S}/SupportExt/glad/gladegl.cpp)
    set(_io_libs ${_io_gl_libs})
    if(_io_t_seexpr)
      list(APPEND _io_ofx14_srcs ${S}/SeExpr/SeExpr.cpp ${S}/SeExpr/SeGrain.cpp ${S}/SeExpr/SeNoise.cpp)
      list(APPEND _io_libs SeExpr::SeExpr)
    endif()
    if(_io_t_png)
      list(APPEND _io_ofx14_srcs ${S}/PNG/ReadPNG.cpp ${S}/PNG/WritePNG.cpp)
      list(APPEND _io_libs io_png)
    endif()
    set(_io_ofx14_incs ${S}/EXR ${S}/OCIO ${S}/PFM ${S}/PNG ${S}/SeExpr ${S}/SupportExt/glad ${S}/openfx/Support/Plugins/include)
    io_test_program(ofx14_host_test OFX THREADS SOURCES ${_io_ofx14_srcs}
                    DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_ofx14_incs} LIBS ${_io_libs})
    set(_p $<TARGET_FILE:ofx14_host_test>)
    # the OFX 1.4-only host; an OFX 1.1 host; OFX 1.4 with the optional OpenGL render suite and a sequential render
    # status of 0; the full host
    io_test(test-ofx14 DIR "${B}/.ofx14" ENV "${_io_ocio_env}" COMMAND ${_p} "${B}/.ofx14")
    io_test(test-ofx14-11 DIR "${B}/.ofx14-11" ENV "${_io_ocio_env}" OFX14_LEVEL=11 COMMAND ${_p} "${B}/.ofx14-11")
    io_test(test-ofx14-gl DIR "${B}/.ofx14-gl" ENV "${_io_ocio_env}" OFX14_GL=1 OFX14_SEQSTATUS=0
            COMMAND ${_p} "${B}/.ofx14-gl")
    io_test(test-ofx14-full DIR "${B}/.ofx14full" ENV "${_io_ocio_env}" COMMAND ${_p} --full "${B}/.ofx14full")
    # after a plug-in scan (Load and Describe every plug-in, then Unload them all), in the OFX 1.4-only and full hosts
    io_test(test-ofx14-scan DIR "${B}/.ofx14scan" ENV "${_io_ocio_env}" COMMAND ${_p} --scan "${B}/.ofx14scan")
    io_test(test-ofx14-scan-full DIR "${B}/.ofx14scanfull" ENV "${_io_ocio_env}"
            COMMAND ${_p} --full --scan "${B}/.ofx14scanfull")

    if(IO_TESTS_SANITIZERS AND NOT WIN32)
      # test-ofx14-scan-asan: the same with AddressSanitizer
      io_test_program(ofx14_host_test_asan OFX THREADS SOURCES ${_io_ofx14_srcs}
                      DEFINITIONS ${_io_gl_defs} INCLUDES ${_io_ofx14_incs} SANITIZE ${_io_asan} LIBS ${_io_libs})
      set(_p $<TARGET_FILE:ofx14_host_test_asan>)
      io_test(test-ofx14-scan-asan DIR "${B}/.ofx14scanasan" ENV "${_io_ocio_env}"
              COMMAND ${_p} --scan "${B}/.ofx14scanasan")
      io_test(test-ofx14-scan-asan-full DIR "${B}/.ofx14scanasanfull" ENV "${_io_ocio_env}"
              COMMAND ${_p} --full --scan "${B}/.ofx14scanasanfull")
      # test-exr-tsan: test-exr with ThreadSanitizer
      io_test_program(exr_read_test_tsan OFX THREADS SOURCES ${_io_exr_srcs} INCLUDES ${S}/EXR ${S}/OIIO
                      SANITIZE -fsanitize=thread)
      io_test(test-exr-tsan DIR "${B}/.exr_read_tsan" ENV "${_io_ocio_env}" "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}"
              COMMAND $<TARGET_FILE:exr_read_test_tsan> "${B}/.exr_read_tsan")
      io_test(test-exr-multiplanar-tsan DIR "${B}/.exr_read_multiplanar_tsan" ENV "${_io_ocio_env}"
              "TSAN_OPTIONS=halt_on_error=1 $ENV{TSAN_OPTIONS}"
              COMMAND $<TARGET_FILE:exr_read_test_tsan> --multiplanar "${B}/.exr_read_multiplanar_tsan")
      # test-exr-ubsan: with UndefinedBehaviorSanitizer, stopping at the first error, with an allocator that fills new
      # memory with a pattern (tests/Makefile's test-exr-ubsan)
      io_test_program(exr_read_test_ubsan OFX THREADS SOURCES ${_io_exr_srcs} INCLUDES ${S}/EXR ${S}/OIIO
                      SANITIZE -fsanitize=undefined,float-cast-overflow -fno-sanitize=pointer-overflow -fno-sanitize-recover=all)
      io_test(test-exr-ubsan DIR "${B}/.exr_read_ubsan" ENV "${_io_ocio_env}" MallocPreScribble=1 MallocScribble=1
              MALLOC_PERTURB_=170 COMMAND $<TARGET_FILE:exr_read_test_ubsan> "${B}/.exr_read_ubsan")
      io_test(test-exr-multiplanar-ubsan DIR "${B}/.exr_read_multiplanar_ubsan" ENV "${_io_ocio_env}" MallocPreScribble=1
              MallocScribble=1 MALLOC_PERTURB_=170
              COMMAND $<TARGET_FILE:exr_read_test_ubsan> --multiplanar "${B}/.exr_read_multiplanar_ubsan")
      # test-exr-tsan-debug: test-exr and test-reader-hooks, DEBUG builds (the Support library's property validation
      # and logging) with ThreadSanitizer
      set(_io_tsan_env "${_io_ocio_env}" "TSAN_OPTIONS=halt_on_error=1 abort_on_error=0 $ENV{TSAN_OPTIONS}")
      io_test_program(exr_read_test_tsan_debug OFX THREADS SOURCES ${_io_exr_srcs} INCLUDES ${S}/EXR ${S}/OIIO
                      DEFINITIONS DEBUG
                      SANITIZE -fsanitize=thread)
      io_test(test-exr-tsan-debug DIR "${B}/.exr_read_tsan_debug" ENV ${_io_tsan_env}
              COMMAND $<TARGET_FILE:exr_read_test_tsan_debug> "${B}/.exr_read_tsan_debug")
      io_test_program(reader_hooks_test_tsan_debug OFX THREADS SOURCES ${T}/reader_hooks_test.cpp ${_io_reader_srcs}
                      DEFINITIONS DEBUG SANITIZE -fsanitize=thread)
      io_test(test-reader-hooks-tsan-debug ENV ${_io_tsan_env}
              COMMAND reader_hooks_test_tsan_debug "${B}/.reader_hooks_tsan_debug")
    endif()
  endif()

  # test-seexpr: SeExpr, SeNoise and SeGrain, with the SeExpr of this build
  if(_io_t_seexpr)
    io_test_program(seexpr_test OFX SOURCES ${T}/seexpr_test.cpp
      ${S}/SeExpr/SeExpr.cpp ${S}/SeExpr/SeNoise.cpp ${S}/SeExpr/SeGrain.cpp
      ${S}/SupportExt/ofxsRectangleInteract.cpp ${S}/SupportExt/ofxsTransformInteract.cpp ${S}/SupportExt/ofxsRamp.cpp
      ${S}/SupportExt/ofxsOGLTextRenderer.cpp ${S}/SupportExt/ofxsOGLFontData.cpp
      ${S}/SupportExt/ofxsThreadSuite.cpp
      INCLUDES ${S}/openfx/Support/Plugins/include LIBS SeExpr::SeExpr OpenGL::GL)
    io_test(test-seexpr COMMAND seexpr_test)
  endif()
elseif(_io_t_ocio OR _io_t_oiio OR _io_t_ffmpeg)
  message(STATUS "tests: no tests that link the plug-ins (they need OpenColorIO, OpenImageIO and FFmpeg)")
endif()

# ---- test-runscript: RunScript on MS Windows (argument quoting, running a program, a batch file and a shebang script);
# on all systems: splitting the script's stderr into lines, with bounded memory; on POSIX: aborting the script
if(_io_t_runscript)
  set(_io_libs "")
  if(WIN32)
    # CommandLineToArgvW is in shell32
    set(_io_libs shell32 advapi32)
  endif()
  io_test_program(runscript_win32_test SOURCES ${T}/runscript_win32_test.cpp ${S}/RunScript/RunScriptWin32.cpp
                  INCLUDES ${S}/RunScript LIBS ${_io_libs})
  io_test(test-runscript-win32 COMMAND runscript_win32_test)
  io_test_program(runscript_output_test SOURCES ${T}/runscript_output_test.cpp INCLUDES ${S}/RunScript)
  io_test(test-runscript-output TIMEOUT 300 COMMAND runscript_output_test)
  if(NOT WIN32)
    # test-runscript-render (POSIX): an aborted render kills the script (#287); only the OFX Support library
    io_test_program(runscript_render_test THREADS
                    SOURCES ${T}/runscript_render_test.cpp ${S}/RunScript/RunScript.cpp ${S}/RunScript/RunScriptWin32.cpp
                            ${S}/SupportExt/ofxsThreadSuite.cpp ${_io_ofx_support}
                    DEFINITIONS OFX_EXTENSIONS_VEGAS OFX_EXTENSIONS_NUKE OFX_EXTENSIONS_TUTTLE OFX_EXTENSIONS_NATRON
                    INCLUDES ${S}/RunScript ${S}/SupportExt ${S}/openfx/include ${S}/openfx/Support/include)
    io_test(test-runscript-render TIMEOUT 300 COMMAND runscript_render_test)
  endif()
endif()
