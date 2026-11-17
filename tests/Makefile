# Makefile for the openfx-io command-line test harnesses.
#
# Two families of tests:
#  1. Standalone FFmpeg reader-core harness (ffmpeg_read_test) + OIIO roundtrip.
#  2. Direct-link OFX tests: the plugin translation units are compiled and linked
#     together with the OFX C++ Support library and an in-process minimal host, so
#     the plugins' OFX entry points/actions can be exercised WITHOUT building the
#     .ofx bundle and WITHOUT an external OFX host (no dlopen).
#
# See README.md.

CXX      ?= c++
PKGCONF  ?= pkg-config

FFMPEG_PKGS := libavcodec libavformat libavutil libswscale

# C++17 by default (OpenImageIO 3.x), unless CXX already has a -std option; e.g. `make test CXXSTD=-std=c++20`
# or `make test CXX="clang++ -std=c++20"` to test C++20
CXXSTD   ?= $(if $(findstring -std=,$(CXX)),,-std=c++17)

CXXFLAGS += $(CXXSTD) -O2 -g -Wall

# ---- (1) FFmpeg reader-core harness --------------------------------------------
READ_CXXFLAGS := $(CXXFLAGS) $(shell $(PKGCONF) --cflags $(FFMPEG_PKGS)) \
    -I../FFmpeg -I../IOSupport -I../SupportExt -I../openfx/include -I../openfx/Support/include
READ_LIBS := $(shell $(PKGCONF) --libs $(FFMPEG_PKGS))
READ_SRCS := ffmpeg_read_test.cpp ../FFmpeg/FFmpegFile.cpp ofx_stubs.cpp

# ---- (2) Direct-link OFX plugin/host tests -------------------------------------
# OpenImageIO 3.1 requires C++17; its headers also pull in fmt.
OFX_PKGS  := OpenColorIO OpenImageIO $(FFMPEG_PKGS)
# OIIOGlobal.cpp includes <IlmThreadPool.h> directly: add OpenEXR's include path (include/OpenEXR) when available.
OFX_PKGS  += $(shell $(PKGCONF) --exists OpenEXR 2>/dev/null && echo OpenEXR)
OFX_DEFS  := -DOFX_EXTENSIONS_VEGAS -DOFX_EXTENSIONS_NUKE -DOFX_EXTENSIONS_TUTTLE \
             -DOFX_EXTENSIONS_NATRON -DOFX_IO_MT_FFMPEG -DOFX_IO_USING_OCIO
OFX_INC   := -I../FFmpeg -I../OIIO -I../RunScript -I../IOSupport -I../IOSupport/SequenceParsing -I../SupportExt \
             -I../openfx/include -I../openfx/Support/include
FMT_INC   := $(shell $(PKGCONF) --cflags fmt 2>/dev/null)

# Opt-in RAW support: `make test-ofx WITH_LIBRAW=1`.
# Homebrew's libraw (0.22.x) headers #include <omp.h>, and Apple clang has no
# OpenMP out of the box, so this also wires in libomp (Homebrew or MacPorts). ReadOIIO builds
# against libraw >= 0.19 with some advanced demosaicing algorithms disabled.
WITH_LIBRAW ?= 0
ifeq ($(WITH_LIBRAW),1)
  OFX_PKGS += libraw
  OFX_DEFS += -DOFX_IO_USING_LIBRAW
  # libomp location: Homebrew (brew --prefix libomp), or MacPorts (/opt/local/{include,lib}/libomp)
  LIBOMP_PREFIX := $(shell brew --prefix libomp 2>/dev/null)
  ifneq ($(LIBOMP_PREFIX),)
    LIBOMP_INC := $(LIBOMP_PREFIX)/include
    LIBOMP_LIB := $(LIBOMP_PREFIX)/lib
  else ifneq ($(wildcard /opt/local/lib/libomp/libomp.dylib),)
    LIBOMP_INC := /opt/local/include/libomp
    LIBOMP_LIB := /opt/local/lib/libomp
  endif
  OMP_CFLAGS := -Xpreprocessor -fopenmp $(if $(LIBOMP_INC),-I$(LIBOMP_INC),)
  OMP_LDLIBS := $(if $(LIBOMP_LIB),-L$(LIBOMP_LIB),) -lomp
endif

# Work around a oneTBB 2023.2 exit crash (seen with MacPorts' OpenImageIO, which
# links TBB, on macOS 27): libtbb dlopen()s libtbbmalloc and frees memory through
# it from a static destructor at exit, after dyld has already unloaded it. Any
# program that links TBB but never uses it, such as these tests, then segfaults
# in tbb::detail::r1::governor::release_resources() after main() returns. Linking
# libtbbmalloc directly keeps it loaded until libtbb is finalized. (A host that
# dlopen()s the IO.ofx bundle is not affected.)
# Only if libtbbmalloc is actually installed next to libtbb.
TBB_LIBDIR := $(shell $(PKGCONF) --variable=libdir tbb 2>/dev/null)
TBBMALLOC_LIBS := $(if $(and $(TBB_LIBDIR),$(wildcard $(TBB_LIBDIR)/libtbbmalloc.*)),-L$(TBB_LIBDIR) -ltbbmalloc,)

OFX_CXXFLAGS := $(CXXSTD) -O0 -g $(OFX_DEFS) $(OFX_INC) $(shell $(PKGCONF) --cflags $(OFX_PKGS)) $(FMT_INC) $(OMP_CFLAGS)
OFX_LIBS  := $(shell $(PKGCONF) --libs $(OFX_PKGS)) $(OMP_LDLIBS) $(TBBMALLOC_LIBS)

# OFX C++ Support library (host-facing plugin runtime); ofxsHWNDInteract.cpp is Windows-only.
OFX_SUPPORT := \
    ../openfx/Support/Library/ofxsCore.cpp \
    ../openfx/Support/Library/ofxsImageEffect.cpp \
    ../openfx/Support/Library/ofxsInteract.cpp \
    ../openfx/Support/Library/ofxsLog.cpp \
    ../openfx/Support/Library/ofxsMultiThread.cpp \
    ../openfx/Support/Library/ofxsParams.cpp \
    ../openfx/Support/Library/ofxsProperty.cpp \
    ../openfx/Support/Library/ofxsPropertyValidation.cpp

# The reader/writer plugin translation units + their in-repo dependencies.
# (LibRaw/RAW support is left out of the test build to avoid an OpenMP dependency;
#  it is not needed to exercise the OFX entry points.)
PLUGIN_SRCS := \
    ../FFmpeg/ReadFFmpeg.cpp ../FFmpeg/WriteFFmpeg.cpp ../FFmpeg/FFmpegFile.cpp ../FFmpeg/PixelFormat.cpp \
    ../OIIO/ReadOIIO.cpp ../OIIO/WriteOIIO.cpp ../OIIO/OIIOGlobal.cpp \
    ../IOSupport/GenericReader.cpp ../IOSupport/GenericWriter.cpp ../IOSupport/GenericOCIO.cpp \
    ../IOSupport/SequenceParsing/SequenceParsing.cpp \
    ../SupportExt/ofxsMultiPlane.cpp ../SupportExt/ofxsThreadSuite.cpp ../SupportExt/tinythread.cpp \
    ../SupportExt/ofxsFileOpen.cpp ../SupportExt/ofxsLut.cpp \
    ../RunScript/RunScript.cpp ../RunScript/RunScriptWin32.cpp

EXPECTED_PLUGINS := fr.inria.openfx.ReadFFmpeg fr.inria.openfx.WriteFFmpeg \
                    fr.inria.openfx.ReadOIIO fr.inria.openfx.WriteOIIO \
                    fr.inria.openfx.RunScript

# ---- (3) RunScript's MS Windows process launching (a no-op elsewhere) ------------
# CommandLineToArgvW is in shell32 (MSYS2/MinGW: $(OS) is Windows_NT, from the environment)
RUNSCRIPT_LIBS := $(if $(filter Windows_NT,$(OS)),-lshell32 -ladvapi32,)

.PHONY: all test test-ffmpeg test-oiio test-ofx test-render test-layers test-reader-hooks test-ocio-cdl test-runscript test-oiio-threads clean
all: ffmpeg_read_test ofx_entrypoints_test ofx_actions_test ofx_render_test oiio_layers_test reader_hooks_test ocio_cdl_test runscript_win32_test runscript_output_test oiio_threadpool_test

# ---- build rules ----
ffmpeg_read_test: $(READ_SRCS)
	$(CXX) $(READ_CXXFLAGS) $(READ_SRCS) $(READ_LIBS) -o $@

ofx_entrypoints_test: ofx_entrypoints_test.cpp $(PLUGIN_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $^ $(OFX_LIBS) -o $@

ofx_actions_test: ofx_actions_test.cpp $(PLUGIN_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $^ $(OFX_LIBS) -o $@

ofx_render_test: ofx_render_test.cpp ofx_test_host.h $(PLUGIN_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# includes ../OIIO/ReadOIIO.cpp (to reach its file-static helpers), so it's left out of the sources
oiio_layers_test: oiio_layers_test.cpp $(filter-out ../OIIO/ReadOIIO.cpp,$(PLUGIN_SRCS)) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $^ $(OFX_LIBS) -o $@

# GenericReader and its in-repo dependencies only: the test defines its own readers
READER_HOOKS_SRCS := ../IOSupport/GenericReader.cpp ../IOSupport/GenericOCIO.cpp \
    ../IOSupport/SequenceParsing/SequenceParsing.cpp \
    ../SupportExt/ofxsMultiPlane.cpp ../SupportExt/ofxsThreadSuite.cpp ../SupportExt/tinythread.cpp \
    ../SupportExt/ofxsFileOpen.cpp ../SupportExt/ofxsLut.cpp

# -pthread: the test uses std::thread
reader_hooks_test: reader_hooks_test.cpp ofx_test_host.h $(READER_HOOKS_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -pthread $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# OCIOCDLTransform and its in-repo dependencies only, with OpenGL render support, as in
# ../Makefile.master (OCIOPluginBase.cpp needs it) and its OCIO_OPENGL_OBJS
OCIO_CDL_SRCS := ../OCIO/OCIOCDLTransform.cpp ../OCIO/OCIOPluginBase.cpp \
    ../IOSupport/GenericOCIO.cpp ../IOSupport/GenericOCIOOpenGL.cpp ../IOSupport/glsl.cpp \
    ../SupportExt/glad/glad.cpp ../SupportExt/glad/gladegl.cpp ../SupportExt/ofxsOGLUtilities.cpp \
    ../SupportExt/ofxsThreadSuite.cpp ../SupportExt/tinythread.cpp ../SupportExt/ofxsFileOpen.cpp
OCIO_CDL_CXXFLAGS := -DOFX_SUPPORTS_OPENGLRENDER -I../OCIO -I../SupportExt/glad -I../openfx/Support/Plugins/include
ifeq ($(OS),Windows_NT)
  OCIO_CDL_LIBS := -lopengl32
else ifeq ($(shell uname -s),Darwin)
  OCIO_CDL_LIBS := -framework OpenGL
else
  # glad's loaders use dlopen(), in libdl before glibc 2.34
  OCIO_CDL_LIBS := -lGL -ldl
endif

ocio_cdl_test: ocio_cdl_test.cpp ofx_test_host.h $(OCIO_CDL_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(OCIO_CDL_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) $(OCIO_CDL_LIBS) -o $@

# the OIIO plug-ins, with OIIOResize (which is not in PLUGIN_SRCS: it renders with ImageBufAlgo)
oiio_threadpool_test: oiio_threadpool_test.cpp ofx_test_host.h $(PLUGIN_SRCS) ../OIIO/OIIOResize.cpp $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -I../openfx/Support/Plugins/include $(filter-out %.h,$^) $(OFX_LIBS) -o $@

runscript_win32_test: runscript_win32_test.cpp ../RunScript/RunScriptWin32.cpp ../RunScript/RunScriptWin32.h ../RunScript/RunScriptOutput.h
	$(CXX) $(CXXFLAGS) -I../RunScript runscript_win32_test.cpp ../RunScript/RunScriptWin32.cpp $(RUNSCRIPT_LIBS) -o $@

runscript_output_test: runscript_output_test.cpp ../RunScript/RunScriptOutput.h ../RunScript/pstream.h
	$(CXX) $(CXXFLAGS) -I../RunScript runscript_output_test.cpp -o $@

# ---- run targets ----
test: test-ffmpeg test-oiio test-ofx test-render test-layers test-reader-hooks test-ocio-cdl test-runscript test-oiio-threads

test-ffmpeg: ffmpeg_read_test
	./run_ffmpeg_read_test.sh

test-oiio:
	./run_oiio_roundtrip_test.sh

# Direct-link OFX entry-point (enumerate) + action-dispatch (load/describe/…) tests.
test-ofx: ofx_entrypoints_test ofx_actions_test
	./ofx_entrypoints_test $(EXPECTED_PLUGINS)
	./ofx_actions_test

# Full render integration: WriteFFmpeg encodes a ramp, ReadFFmpeg decodes it back
# (round-trip), driven through the linked plugins by the in-process host.
# GenericOCIO needs a loadable OCIO config, supplied via $OCIO.
test-render: ofx_render_test
	OCIO=$(CURDIR)/test.ocio ./ofx_render_test $(CURDIR)/.render_out.mov

# RunScript on MS Windows: argument quoting, running a program, a batch file and a shebang script (#70);
# on all systems: splitting the script's stderr into lines, with bounded memory (#111)
test-runscript: runscript_win32_test runscript_output_test
	./runscript_win32_test
	./runscript_output_test

# ReadOIIO's EXR channel name -> view/layer mapping (multi-view EXRs, #75)
test-layers: oiio_layers_test
	./oiio_layers_test

# GenericReader's isOutputFrameVarying() and getRenderTime() hooks (#96), on test readers
test-reader-hooks: reader_hooks_test
	OCIO=$(CURDIR)/test.ocio ./reader_hooks_test $(CURDIR)/.reader_hooks

# OCIOCDLTransform's Export param writes a .cc file that OCIO reads back (#105)
test-ocio-cdl: ocio_cdl_test
	OCIO=$(CURDIR)/test.ocio ./ocio_cdl_test

# OpenImageIO's thread pool is stopped when the last OIIO plug-in is unloaded, not the first one (#118)
test-oiio-threads: oiio_threadpool_test
	OCIO=$(CURDIR)/test.ocio ./oiio_threadpool_test

clean:
	rm -rf ffmpeg_read_test ffmpeg_read_test.dSYM \
	       ofx_entrypoints_test ofx_entrypoints_test.dSYM \
	       ofx_actions_test ofx_actions_test.dSYM \
	       ofx_render_test ofx_render_test.dSYM .render_out.mov .render_out.mov.i79.jpg .render_out.mov.i79.png .render_out.mov.i90a.png .render_out.mov.i90b.png .render_out.mov.i90c.mov .render_out.mov.i71.mp4 .render_out.mov.i71.mp4.avi .render_out.mov.i78.mov .render_out.mov.i78.mp4 .render_out.mov.i110.mov .render_out.mov.i110.mp4v.mov .render_out.mov.i110.3gp .render_out.mov.i137.mov .render_out.mov.i137.mov.lnk .render_out.mov.i137.mov.other.mov ofxTestLog.txt \
	       oiio_layers_test oiio_layers_test.dSYM \
	       reader_hooks_test reader_hooks_test.dSYM .reader_hooks.still.tst .reader_hooks.video.tst \
	       ocio_cdl_test ocio_cdl_test.dSYM \
	       oiio_threadpool_test oiio_threadpool_test.dSYM \
	       runscript_win32_test runscript_win32_test.dSYM runscript_output_test runscript_output_test.dSYM
