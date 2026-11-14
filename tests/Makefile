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

CXXFLAGS += -std=c++14 -O2 -g -Wall

# ---- (1) FFmpeg reader-core harness --------------------------------------------
READ_CXXFLAGS := $(CXXFLAGS) $(shell $(PKGCONF) --cflags $(FFMPEG_PKGS)) \
    -I../FFmpeg -I../IOSupport -I../SupportExt -I../openfx/include -I../openfx/Support/include
READ_LIBS := $(shell $(PKGCONF) --libs $(FFMPEG_PKGS))
READ_SRCS := ffmpeg_read_test.cpp ../FFmpeg/FFmpegFile.cpp ofx_stubs.cpp

# ---- (2) Direct-link OFX plugin/host tests -------------------------------------
# OpenImageIO 3.1 requires C++17; its headers also pull in fmt.
OFX_PKGS  := OpenColorIO OpenImageIO $(FFMPEG_PKGS)
OFX_DEFS  := -DOFX_EXTENSIONS_VEGAS -DOFX_EXTENSIONS_NUKE -DOFX_EXTENSIONS_TUTTLE \
             -DOFX_EXTENSIONS_NATRON -DOFX_IO_MT_FFMPEG -DOFX_IO_USING_OCIO
OFX_INC   := -I../FFmpeg -I../OIIO -I../IOSupport -I../IOSupport/SequenceParsing -I../SupportExt \
             -I../openfx/include -I../openfx/Support/include
FMT_INC   := $(shell $(PKGCONF) --cflags fmt 2>/dev/null)

# Opt-in RAW support: `make test-ofx WITH_LIBRAW=1`.
# Homebrew's libraw (0.22.x) headers #include <omp.h>, and Apple clang has no
# OpenMP out of the box, so this also wires in Homebrew's libomp. ReadOIIO builds
# against libraw >= 0.19 with some advanced demosaicing algorithms disabled.
WITH_LIBRAW ?= 0
ifeq ($(WITH_LIBRAW),1)
  OFX_PKGS += libraw
  OFX_DEFS += -DOFX_IO_USING_LIBRAW
  LIBOMP_PREFIX := $(shell brew --prefix libomp 2>/dev/null)
  OMP_CFLAGS := -Xpreprocessor -fopenmp $(if $(LIBOMP_PREFIX),-I$(LIBOMP_PREFIX)/include,)
  OMP_LDLIBS := $(if $(LIBOMP_PREFIX),-L$(LIBOMP_PREFIX)/lib,) -lomp
endif

OFX_CXXFLAGS := -std=c++17 -O0 -g $(OFX_DEFS) $(OFX_INC) $(shell $(PKGCONF) --cflags $(OFX_PKGS)) $(FMT_INC) $(OMP_CFLAGS)
OFX_LIBS  := $(shell $(PKGCONF) --libs $(OFX_PKGS)) $(OMP_LDLIBS)

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
    ../SupportExt/ofxsFileOpen.cpp ../SupportExt/ofxsLut.cpp

EXPECTED_PLUGINS := fr.inria.openfx.ReadFFmpeg fr.inria.openfx.WriteFFmpeg \
                    fr.inria.openfx.ReadOIIO fr.inria.openfx.WriteOIIO

.PHONY: all test test-ffmpeg test-oiio test-ofx clean
all: ffmpeg_read_test ofx_entrypoints_test ofx_actions_test ofx_render_test

# ---- build rules ----
ffmpeg_read_test: $(READ_SRCS)
	$(CXX) $(READ_CXXFLAGS) $(READ_SRCS) $(READ_LIBS) -o $@

ofx_entrypoints_test: ofx_entrypoints_test.cpp $(PLUGIN_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $^ $(OFX_LIBS) -o $@

ofx_actions_test: ofx_actions_test.cpp $(PLUGIN_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $^ $(OFX_LIBS) -o $@

ofx_render_test: ofx_render_test.cpp $(PLUGIN_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $^ $(OFX_LIBS) -o $@

# ---- run targets ----
test: test-ffmpeg test-oiio test-ofx test-render

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

clean:
	rm -rf ffmpeg_read_test ffmpeg_read_test.dSYM \
	       ofx_entrypoints_test ofx_entrypoints_test.dSYM \
	       ofx_actions_test ofx_actions_test.dSYM \
	       ofx_render_test ofx_render_test.dSYM .render_out.mov ofxTestLog.txt
