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

# OFX C++ Support library (host-facing plugin runtime), as in openfx's Makefile.master: ofxsDrawContextOpenGL.cpp
# (DrawArgs::isOpenGLContextCurrent, used by SupportExt's OverlayDraw) loads OpenGL at run time; ofxsHWNDInteract.cpp
# is Windows-only.
OFX_SUPPORT := \
    ../openfx/Support/Library/ofxsCore.cpp \
    ../openfx/Support/Library/ofxsImageEffect.cpp \
    ../openfx/Support/Library/ofxsInteract.cpp \
    ../openfx/Support/Library/ofxsLog.cpp \
    ../openfx/Support/Library/ofxsMultiThread.cpp \
    ../openfx/Support/Library/ofxsParams.cpp \
    ../openfx/Support/Library/ofxsProperty.cpp \
    ../openfx/Support/Library/ofxsPropertyValidation.cpp \
    ../openfx/Support/Library/ofxsDrawContextOpenGL.cpp

# The reader/writer plugin translation units + their in-repo dependencies.
# (LibRaw/RAW support is left out of the test build to avoid an OpenMP dependency;
#  it is not needed to exercise the OFX entry points.)
PLUGIN_SRCS := \
    ../FFmpeg/ReadFFmpeg.cpp ../FFmpeg/WriteFFmpeg.cpp ../FFmpeg/FFmpegFile.cpp ../FFmpeg/PixelFormat.cpp \
    ../OIIO/ReadOIIO.cpp ../OIIO/WriteOIIO.cpp ../OIIO/OIIOGlobal.cpp \
    ../IOSupport/GenericReader.cpp ../IOSupport/ExrThreads.cpp ../IOSupport/GenericWriter.cpp ../IOSupport/GenericOCIO.cpp \
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

.PHONY: all test test-ffmpeg test-oiio test-ofx test-render test-dnxhr444-noise test-writeffmpeg-begin-failure test-writeffmpeg-encode-failure test-writeffmpeg-encode-failure-disk-full test-layers test-reader-hooks test-ocio-cdl test-ocio-anim test-runscript test-oiio-threads test-exr test-exr-tsan test-exr-tsan-debug bench-exr clean
all: ffmpeg_read_test ofx_entrypoints_test ofx_actions_test ofx_render_test oiio_layers_test reader_hooks_test ocio_cdl_test ocio_colorspace_anim_test ocio_anim_fuzz runscript_win32_test runscript_output_test oiio_threadpool_test exr_read_test

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
READER_HOOKS_SRCS := ../IOSupport/GenericReader.cpp ../IOSupport/ExrThreads.cpp ../IOSupport/GenericOCIO.cpp \
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

# OCIOFileTransform's CCC Id param, with AddressSanitizer (an out-of-bounds read of the file name), with the OpenGL
# sources of ocio_cdl_test. No ASan with MinGW (MS Windows), which doesn't have it. (Added to test and clean here.)
OCIO_FT_SRCS := ../OCIO/OCIOFileTransform.cpp $(filter-out ../OCIO/OCIOCDLTransform.cpp,$(OCIO_CDL_SRCS))
OCIO_FT_ASAN := $(if $(filter Windows_NT,$(OS)),,-fsanitize=address -fno-omit-frame-pointer)
ocio_filetransform_test: ocio_filetransform_test.cpp ofx_test_host.h $(OCIO_FT_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(OCIO_CDL_CXXFLAGS) $(OCIO_FT_ASAN) $(filter-out %.h,$^) $(OFX_LIBS) $(OCIO_CDL_LIBS) -o $@

.PHONY: test-ocio-filetransform clean-ocio-filetransform
test: test-ocio-filetransform
clean: clean-ocio-filetransform
test-ocio-filetransform: ocio_filetransform_test
	OCIO=$(CURDIR)/test.ocio ASAN_OPTIONS="detect_leaks=0 $(ASAN_OPTIONS)" ./ocio_filetransform_test
clean-ocio-filetransform:
	rm -rf ocio_filetransform_test ocio_filetransform_test.dSYM

# OCIOColorSpace (with the OpenGL sources of ocio_cdl_test) and the reader/writer plug-ins
OCIO_ANIM_SRCS := ../OCIO/OCIOColorSpace.cpp ../OCIO/OCIOPluginBase.cpp \
    ../IOSupport/GenericOCIOOpenGL.cpp ../IOSupport/glsl.cpp \
    ../SupportExt/glad/glad.cpp ../SupportExt/glad/gladegl.cpp ../SupportExt/ofxsOGLUtilities.cpp \
    $(PLUGIN_SRCS)

ocio_colorspace_anim_test: ocio_colorspace_anim_test.cpp ofx_test_host.h $(OCIO_ANIM_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(OCIO_CDL_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) $(OCIO_CDL_LIBS) -o $@

# random operations on the same host, checking invariants (it includes ocio_colorspace_anim_test.cpp)
ocio_anim_fuzz: ocio_anim_fuzz.cpp ocio_colorspace_anim_test.cpp ofx_test_host.h $(OCIO_ANIM_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(OCIO_CDL_CXXFLAGS) ocio_anim_fuzz.cpp $(OCIO_ANIM_SRCS) $(OFX_SUPPORT) $(OFX_LIBS) $(OCIO_CDL_LIBS) -o $@

# the OIIO plug-ins, with OIIOResize (which is not in PLUGIN_SRCS: it renders with ImageBufAlgo)
oiio_threadpool_test: oiio_threadpool_test.cpp ofx_test_host.h $(PLUGIN_SRCS) ../OIIO/OIIOResize.cpp $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -I../openfx/Support/Plugins/include $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# the same with ThreadSanitizer: the OIIO plug-in instances are counted under a mutex, from several threads (#250)
oiio_threadpool_test_tsan: oiio_threadpool_test.cpp ofx_test_host.h $(PLUGIN_SRCS) ../OIIO/OIIOResize.cpp $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -I../openfx/Support/Plugins/include -fsanitize=thread -pthread $(filter-out %.h,$^) $(OFX_LIBS) -o $@

.PHONY: test-oiio-threads-tsan clean-oiio-threads-tsan
clean: clean-oiio-threads-tsan
test-oiio-threads-tsan: oiio_threadpool_test_tsan
	OCIO=$(CURDIR)/test.ocio TSAN_OPTIONS="halt_on_error=1 $(TSAN_OPTIONS)" ./oiio_threadpool_test_tsan
clean-oiio-threads-tsan:
	rm -rf oiio_threadpool_test_tsan oiio_threadpool_test_tsan.dSYM

# OIIOResize and OIIOText with degenerate inputs (an empty source, huge or infinite RoDs, huge text positions), with
# the OIIO plug-ins (OIIOGlobal), and OpenGL (OIIOText's interact; OCIO_CDL_LIBS). (Added to test and clean here, on
# lines of their own.)
oiio_edge_test: oiio_edge_test.cpp ofx_test_host.h $(PLUGIN_SRCS) ../OIIO/OIIOResize.cpp ../OIIO/OIIOText.cpp \
    ../SupportExt/ofxsOGLTextRenderer.cpp ../SupportExt/ofxsOGLFontData.cpp $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -I../openfx/Support/Plugins/include $(filter-out %.h,$^) $(OFX_LIBS) $(OCIO_CDL_LIBS) -o $@

.PHONY: test-oiio-edge clean-oiio-edge
test: test-oiio-edge
clean: clean-oiio-edge
test-oiio-edge: oiio_edge_test
	OCIO=$(CURDIR)/test.ocio ./oiio_edge_test
clean-oiio-edge:
	rm -rf oiio_edge_test oiio_edge_test.dSYM

# Every plug-in of IO.ofx (the sources of ../IO/Makefile), in the OFX 1.4-only host of ofx_test_host.h (no suite or
# property newer than OFX 1.4, no extension, no OfxSetHost), and in the full host: describe, instantiate and render
# each one. With OpenGL render support, as in ../Makefile.master. The SeExpr plug-ins only if SeExpr 2.11 (the SeExpr 1
# API, SeExpression.h) is in $(SEEXPR_HOME), and the PNG ones only if pkg-config finds libpng.
# SEEXPR_HOME default: MacPorts' prefix on macOS if SeExpr is there (the seexpr211 port), else /usr (also for test-seexpr,
# below)
SEEXPR_HOME ?= $(if $(and $(filter Darwin,$(shell uname -s)),$(wildcard /opt/local/include/SeExpression.h)),/opt/local,/usr)
comma := ,
HAVE_SEEXPR := $(if $(wildcard $(SEEXPR_HOME)/include/SeExpression.h),1,)
HAVE_PNG := $(shell $(PKGCONF) --exists libpng 2>/dev/null && echo 1)
OFX14_SRCS := ../EXR/ReadEXR.cpp ../EXR/WriteEXR.cpp \
    ../FFmpeg/ReadFFmpeg.cpp ../FFmpeg/WriteFFmpeg.cpp ../FFmpeg/FFmpegFile.cpp ../FFmpeg/PixelFormat.cpp \
    ../OIIO/ReadOIIO.cpp ../OIIO/WriteOIIO.cpp ../OIIO/OIIOGlobal.cpp ../OIIO/OIIOText.cpp ../OIIO/OIIOResize.cpp \
    ../PFM/ReadPFM.cpp ../PFM/WritePFM.cpp \
    ../OCIO/OCIOColorSpace.cpp ../OCIO/OCIODisplay.cpp ../OCIO/OCIOCDLTransform.cpp ../OCIO/OCIOFileTransform.cpp \
    ../OCIO/OCIOLogConvert.cpp ../OCIO/OCIOLookTransform.cpp ../OCIO/OCIOPluginBase.cpp \
    ../IOSupport/GenericReader.cpp ../IOSupport/ExrThreads.cpp ../IOSupport/GenericWriter.cpp ../IOSupport/GenericOCIO.cpp \
    ../IOSupport/GenericOCIOOpenGL.cpp ../IOSupport/glsl.cpp ../IOSupport/SequenceParsing/SequenceParsing.cpp \
    ../RunScript/RunScript.cpp ../RunScript/RunScriptWin32.cpp \
    ../SupportExt/ofxsMultiPlane.cpp ../SupportExt/ofxsThreadSuite.cpp ../SupportExt/tinythread.cpp \
    ../SupportExt/ofxsFileOpen.cpp ../SupportExt/ofxsLut.cpp ../SupportExt/ofxsOGLTextRenderer.cpp \
    ../SupportExt/ofxsOGLFontData.cpp ../SupportExt/ofxsRamp.cpp ../SupportExt/ofxsGenerator.cpp \
    ../SupportExt/ofxsTransformInteract.cpp ../SupportExt/ofxsRectangleInteract.cpp ../SupportExt/ofxsOGLUtilities.cpp \
    ../SupportExt/glad/glad.cpp ../SupportExt/glad/gladegl.cpp \
    $(if $(HAVE_SEEXPR),../SeExpr/SeExpr.cpp ../SeExpr/SeGrain.cpp ../SeExpr/SeNoise.cpp,) \
    $(if $(HAVE_PNG),../PNG/ReadPNG.cpp ../PNG/WritePNG.cpp,)
OFX14_CXXFLAGS := -DOFX_SUPPORTS_OPENGLRENDER -I../EXR -I../OCIO -I../PFM -I../PNG -I../SeExpr \
    -I../SupportExt/glad -I../openfx/Support/Plugins/include \
    $(if $(HAVE_SEEXPR),-I$(SEEXPR_HOME)/include,) $(if $(HAVE_PNG),$(shell $(PKGCONF) --cflags libpng),)
OFX14_LIBS := $(if $(HAVE_SEEXPR),-L$(SEEXPR_HOME)/lib -Wl$(comma)-rpath$(comma)$(SEEXPR_HOME)/lib -lSeExpr,) \
    $(if $(HAVE_PNG),$(shell $(PKGCONF) --libs libpng) -lz,)

ofx14_host_test: ofx14_host_test.cpp ofx_test_host.h ofx14_names.h $(OFX14_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(OFX14_CXXFLAGS) -pthread $(filter-out %.h,$^) $(OFX_LIBS) $(OFX14_LIBS) $(OCIO_CDL_LIBS) -o $@

.PHONY: test-ofx14 clean-ofx14
test: test-ofx14
clean: clean-ofx14
# the OFX 1.4-only host; an OFX 1.1 host (no OFX 1.2-1.4 property, e.g. kOfxImageEffectPropSequentialRenderStatus,
# no Message suite V2, no RGB); OFX 1.4 with the optional OpenGL render suite, and a sequential render status of 0;
# then the full host
test-ofx14: ofx14_host_test
	rm -rf .ofx14 .ofx14-11 .ofx14-gl .ofx14full
	OCIO=$(CURDIR)/test.ocio ./ofx14_host_test $(CURDIR)/.ofx14
	OCIO=$(CURDIR)/test.ocio OFX14_LEVEL=11 ./ofx14_host_test $(CURDIR)/.ofx14-11
	OCIO=$(CURDIR)/test.ocio OFX14_GL=1 OFX14_SEQSTATUS=0 ./ofx14_host_test $(CURDIR)/.ofx14-gl
	OCIO=$(CURDIR)/test.ocio ./ofx14_host_test --full $(CURDIR)/.ofx14full
clean-ofx14:
	rm -rf ofx14_host_test ofx14_host_test.dSYM .ofx14 .ofx14-11 .ofx14-gl .ofx14full

# The same tests after a plug-in scan (--scan: Load and Describe every plug-in, then Unload them all, as a host that
# unloads the binary after its scan, HostSupport's default since openfx #174), in the OFX 1.4-only host and in the
# full host; then the same with AddressSanitizer (not on MS Windows).
OFX14_ASAN := -fsanitize=address -fno-omit-frame-pointer
ofx14_host_test_asan: ofx14_host_test.cpp ofx_test_host.h ofx14_names.h $(OFX14_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(OFX14_CXXFLAGS) $(OFX14_ASAN) -pthread $(filter-out %.h,$^) $(OFX_LIBS) $(OFX14_LIBS) $(OCIO_CDL_LIBS) -o $@

.PHONY: test-ofx14-scan test-ofx14-scan-asan clean-ofx14-scan
test: test-ofx14-scan
clean: clean-ofx14-scan
test-ofx14-scan: ofx14_host_test
	rm -rf .ofx14scan .ofx14scanfull
	OCIO=$(CURDIR)/test.ocio ./ofx14_host_test --scan $(CURDIR)/.ofx14scan
	OCIO=$(CURDIR)/test.ocio ./ofx14_host_test --full --scan $(CURDIR)/.ofx14scanfull
test-ofx14-scan-asan: ofx14_host_test_asan
	rm -rf .ofx14scanasan .ofx14scanasanfull
	OCIO=$(CURDIR)/test.ocio ./ofx14_host_test_asan --scan $(CURDIR)/.ofx14scanasan
	OCIO=$(CURDIR)/test.ocio ./ofx14_host_test_asan --full --scan $(CURDIR)/.ofx14scanasanfull
clean-ofx14-scan:
	rm -rf ofx14_host_test_asan ofx14_host_test_asan.dSYM .ofx14scan .ofx14scanfull .ofx14scanasan .ofx14scanasanfull

# ReadEXR and GenericReader with their in-repo dependencies, and OpenEXR (in OFX_PKGS, if pkg-config finds it).
# -pthread: the test uses std::thread
EXR_READ_SRCS := ../EXR/ReadEXR.cpp ../OIIO/OIIOGlobal.cpp $(READER_HOOKS_SRCS)
EXR_READ_CXXFLAGS := -I../EXR -I../OIIO -pthread

exr_read_test: exr_read_test.cpp ofx_test_host.h $(EXR_READ_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(EXR_READ_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# the same, with ThreadSanitizer (data races in the concurrent renders)
exr_read_test_tsan: exr_read_test.cpp ofx_test_host.h $(EXR_READ_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(EXR_READ_CXXFLAGS) -fsanitize=thread $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# the same, with UndefinedBehaviorSanitizer (e.g. GenericReader's components table read past its end, an
# uninitialized enum, in the luminance/chroma case), stopping at the first error. pointer-overflow is left out: the
# test computes the OpenEXR slice bases with pointer arithmetic out of the buffers (fixed by #185). (Added to clean
# here, on lines of their own.)
exr_read_test_ubsan: exr_read_test.cpp ofx_test_host.h $(EXR_READ_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(EXR_READ_CXXFLAGS) -fsanitize=undefined,float-cast-overflow -fno-sanitize=pointer-overflow -fno-sanitize-recover=all $(filter-out %.h,$^) $(OFX_LIBS) -o $@

.PHONY: test-exr-ubsan clean-exr-ubsan
clean: clean-exr-ubsan
# The allocator fills new memory with a pattern, so that a read of uninitialized memory gives an invalid value that
# UBSan reports (e.g. an enum): MallocPreScribble/MallocScribble on macOS (0xAA bytes), MALLOC_PERTURB_=170 with glibc
# (0x55 bytes, 170 ^ 0xff, on allocation; 0xAA on free); ignored elsewhere.
test-exr-ubsan: exr_read_test_ubsan
	rm -rf .exr_read_ubsan && mkdir -p .exr_read_ubsan
	OCIO=$(CURDIR)/test.ocio MallocPreScribble=1 MallocScribble=1 MALLOC_PERTURB_=170 ./exr_read_test_ubsan $(CURDIR)/.exr_read_ubsan
clean-exr-ubsan:
	rm -rf exr_read_test_ubsan exr_read_test_ubsan.dSYM .exr_read_ubsan

# ReadOIIO, ReadPFM and ReadPNG (if pkg-config finds libpng): render windows narrower than the image, render scale
# 1/2, files rewritten while rendering (#160, #163). With AddressSanitizer (not on MS Windows, as OCIO_FT_ASAN), and
# guard pages around the output image. -pthread: the test uses std::thread. (Added to test and clean here, on lines of
# their own.)
READER_WINDOW_SRCS := ../OIIO/ReadOIIO.cpp ../OIIO/OIIOGlobal.cpp ../PFM/ReadPFM.cpp $(if $(HAVE_PNG),../PNG/ReadPNG.cpp,) \
    $(READER_HOOKS_SRCS)
READER_WINDOW_CXXFLAGS := -I../PFM -I../PNG -pthread $(if $(filter Windows_NT,$(OS)),,-fsanitize=address -fno-omit-frame-pointer) \
    $(if $(HAVE_PNG),-DHAVE_PNG $(shell $(PKGCONF) --cflags libpng),)
READER_WINDOW_LIBS := $(if $(HAVE_PNG),$(shell $(PKGCONF) --libs libpng) -lz,)

reader_window_test: reader_window_test.cpp ofx_test_host.h $(READER_WINDOW_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(READER_WINDOW_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) $(READER_WINDOW_LIBS) -o $@

.PHONY: test-reader-window clean-reader-window
test: test-reader-window
clean: clean-reader-window
test-reader-window: reader_window_test
	rm -rf .reader_window && mkdir -p .reader_window
	OCIO=$(CURDIR)/test.ocio ASAN_OPTIONS="detect_leaks=0 $(ASAN_OPTIONS)" ./reader_window_test $(CURDIR)/.reader_window
	OCIO=$(CURDIR)/test.ocio ASAN_OPTIONS="detect_leaks=0 $(ASAN_OPTIONS)" READER_WINDOW_MULTIPLANE=1 ./reader_window_test $(CURDIR)/.reader_window
clean-reader-window:
	rm -rf reader_window_test reader_window_test.dSYM .reader_window

# ReadPNG and WritePNG on libpng errors (#235): corrupt PNG files (made from valid ones that the test writes with
# libpng) must fail cleanly, without a crash, an AddressSanitizer report, or a leak (of memory or of a file
# descriptor). With AddressSanitizer (not on MS Windows, as OCIO_FT_ASAN). Only if pkg-config finds libpng.
ifneq ($(HAVE_PNG),)
PNG_CORRUPT_SRCS := ../PNG/ReadPNG.cpp ../PNG/WritePNG.cpp ../IOSupport/GenericWriter.cpp $(READER_HOOKS_SRCS)
PNG_CORRUPT_CXXFLAGS := -I../PNG -pthread $(if $(filter Windows_NT,$(OS)),,-fsanitize=address -fno-omit-frame-pointer) \
    $(shell $(PKGCONF) --cflags libpng)

png_corrupt_test: png_corrupt_test.cpp ofx_test_host.h ../PNG/PNGError.h $(PNG_CORRUPT_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(PNG_CORRUPT_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) $(READER_WINDOW_LIBS) -o $@

.PHONY: test-png-corrupt clean-png-corrupt
test: test-png-corrupt
clean: clean-png-corrupt
test-png-corrupt: png_corrupt_test
	rm -rf .png_corrupt && mkdir -p .png_corrupt
	OCIO=$(CURDIR)/test.ocio ASAN_OPTIONS="detect_leaks=0 $(ASAN_OPTIONS)" ./png_corrupt_test $(CURDIR)/.png_corrupt
clean-png-corrupt:
	rm -rf png_corrupt_test png_corrupt_test.dSYM .png_corrupt
endif

# The same readers and ReadFFmpeg in a real OFX host (#232): reader_window_host (hostdemo/reader_window.cpp) is built
# on openfx's HostSupport and hostDemo's host classes (with expat, from pkg-config), and loads them from a bundle built
# here (ReaderWindow.ofx.bundle, like IO.ofx but with the readers only). hostdemo/reader_window.sh writes the files
# with oiiotool and ffmpeg, and renders windows in the image and partly or entirely out of its RoD, at scale 1 and
# 1/2, with both filePremult paths. It is skipped (exit status 77) without oiiotool. CMake's io-reader-window runs the
# same script on the installed IO.ofx bundle. (Added to test and clean here, on lines of their own.)
RW_HOST_SRCS := hostdemo/reader_window.cpp $(wildcard ../openfx/HostSupport/src/*.cpp) \
    ../openfx/HostSupport/examples/hostDemoClipInstance.cpp ../openfx/HostSupport/examples/hostDemoEffectInstance.cpp \
    ../openfx/HostSupport/examples/hostDemoHostDescriptor.cpp ../openfx/HostSupport/examples/hostDemoParamInstance.cpp
RW_HOST_CXXFLAGS := $(CXXSTD) -O0 -g -DOFX_EXTENSIONS_VEGAS -DOFX_EXTENSIONS_NUKE -DOFX_EXTENSIONS_TUTTLE -DOFX_EXTENSIONS_NATRON \
    -I../openfx/include -I../openfx/HostSupport/include -I../openfx/HostSupport/examples $(shell $(PKGCONF) --cflags expat 2>/dev/null)
RW_HOST_LIBS := $(shell $(PKGCONF) --libs expat 2>/dev/null || echo -lexpat) $(if $(filter Windows_NT,$(OS)),,-ldl)
# the folder of the binary in the bundle (as in openfx's Support/Plugins/Makefile.master)
ifeq ($(OS),Windows_NT)
  RW_ARCH := Win64
else ifeq ($(shell uname -s),Darwin)
  RW_ARCH := MacOS
else ifeq ($(shell uname -m),x86_64)
  RW_ARCH := Linux-x86-64
else
  RW_ARCH := Linux-$(shell uname -m)
endif
RW_BUNDLE_BINARY := .reader_window_host/plugins/ReaderWindow.ofx.bundle/Contents/$(RW_ARCH)/ReaderWindow.ofx
RW_PLUGIN_SRCS := $(filter-out $(READER_HOOKS_SRCS),$(READER_WINDOW_SRCS)) \
    ../FFmpeg/ReadFFmpeg.cpp ../FFmpeg/FFmpegFile.cpp ../FFmpeg/PixelFormat.cpp $(READER_HOOKS_SRCS)

reader_window_host: $(RW_HOST_SRCS) ../openfx/HostSupport/examples/hostDemoClipInstance.h ../openfx/HostSupport/examples/hostDemoEffectInstance.h
	$(CXX) $(RW_HOST_CXXFLAGS) $(filter %.cpp,$^) $(RW_HOST_LIBS) -o $@

$(RW_BUNDLE_BINARY): $(RW_PLUGIN_SRCS) $(OFX_SUPPORT)
	mkdir -p $(@D)
	$(CXX) $(OFX_CXXFLAGS) -I../PFM -I../PNG $(if $(HAVE_PNG),$(shell $(PKGCONF) --cflags libpng),) -fPIC -shared $^ $(OFX_LIBS) $(READER_WINDOW_LIBS) -o $@

# a host that renders ReadOIIO, destroys its instance and exits without kOfxActionUnload, 10 times: it must exit
# (#250; with the bundle of test-reader-window-host, and OpenImageIO, whose pool it also checks)
EWU_HOST_SRCS := hostdemo/exit_without_unload.cpp $(filter-out hostdemo/reader_window.cpp,$(RW_HOST_SRCS))
exit_without_unload: $(EWU_HOST_SRCS)
	$(CXX) $(RW_HOST_CXXFLAGS) $(shell $(PKGCONF) --cflags OpenImageIO) $(filter %.cpp,$^) $(RW_HOST_LIBS) $(shell $(PKGCONF) --libs OpenImageIO) $(TBBMALLOC_LIBS) -o $@

# a host that renders a DNG file with ReadOIIO (LibRaw: OpenMP on Linux and MS Windows), unloads the plug-ins, keeps
# running, then exits, 5 times: the OpenMP runtime loaded after the render must still be loaded after the unload, and
# no run may crash (#239; with the bundle of test-reader-window-host; not linked with OpenImageIO, LibRaw or OpenMP, so
# that nothing else keeps them loaded)
RAW_UNLOAD_SRCS := hostdemo/raw_unload.cpp $(filter-out hostdemo/reader_window.cpp,$(RW_HOST_SRCS))
raw_unload: $(RAW_UNLOAD_SRCS)
	$(CXX) $(RW_HOST_CXXFLAGS) $(filter %.cpp,$^) $(RW_HOST_LIBS) -o $@

.PHONY: test-raw-unload clean-raw-unload
test: test-raw-unload
clean: clean-raw-unload
test-raw-unload: raw_unload $(RW_BUNDLE_BINARY)
	OCIO=$(CURDIR)/test.ocio bash hostdemo/raw_unload.sh ./raw_unload .reader_window_host/plugins .raw_unload; \
	    st=$$?; if [ $$st -eq 77 ]; then echo "test-raw-unload: SKIPPED"; else exit $$st; fi
clean-raw-unload:
	rm -rf raw_unload raw_unload.dSYM .raw_unload

.PHONY: test-exit-without-unload clean-exit-without-unload
test: test-exit-without-unload
clean: clean-exit-without-unload
test-exit-without-unload: exit_without_unload $(RW_BUNDLE_BINARY)
	OCIO=$(CURDIR)/test.ocio bash hostdemo/exit_without_unload.sh ./exit_without_unload .reader_window_host/plugins .exit_without_unload; \
	    st=$$?; if [ $$st -eq 77 ]; then echo "test-exit-without-unload: SKIPPED"; else exit $$st; fi
clean-exit-without-unload:
	rm -rf exit_without_unload exit_without_unload.dSYM .exit_without_unload

.PHONY: test-reader-window-host clean-reader-window-host
test: test-reader-window-host
clean: clean-reader-window-host
test-reader-window-host: reader_window_host $(RW_BUNDLE_BINARY)
	OCIO=$(CURDIR)/test.ocio bash hostdemo/reader_window.sh ./reader_window_host .reader_window_host/plugins .reader_window_host/work; \
	    st=$$?; if [ $$st -eq 77 ]; then echo "test-reader-window-host: SKIPPED"; else exit $$st; fi
clean-reader-window-host:
	rm -rf reader_window_host reader_window_host.dSYM .reader_window_host

# DEBUG builds of exr_read_test and reader_hooks_test with ThreadSanitizer: -DDEBUG enables the Support
# library's property validation and logging, which run on the threads that describe/create instances while
# other threads render (openfx #144: races on PropertySet::_gPropLogging and PropertyDescription::_exists)
TSAN_DEBUG_CXXFLAGS := -DDEBUG -fsanitize=thread

exr_read_test_tsan_debug: exr_read_test.cpp ofx_test_host.h $(EXR_READ_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(EXR_READ_CXXFLAGS) $(TSAN_DEBUG_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) -o $@

reader_hooks_test_tsan_debug: reader_hooks_test.cpp ofx_test_host.h $(READER_HOOKS_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -pthread $(TSAN_DEBUG_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# the same, optimized, for bench-exr (the plug-in is built with -O2 in a release build)
exr_read_bench: exr_read_test.cpp ofx_test_host.h $(EXR_READ_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) -O2 $(EXR_READ_CXXFLAGS) $(filter-out %.h,$^) $(OFX_LIBS) -o $@

# SeExpr.cpp, SeNoise.cpp and SeGrain.cpp and their in-repo dependencies, with SeExpr 2.11 (the SeExpr 1 API) from SEEXPR_HOME,
# as in ../Makefile.master. test-seexpr is skipped if they are not found there, or fails if SEEXPR_REQUIRED=1.
# SEEXPR_LIBDIR: the directory of libSeExpr (default: the first of lib, lib64 and lib/<multiarch> in SEEXPR_HOME
# that has it).
# SEEXPR_HOME: its default is set above, for test-ofx14.
SEEXPR_REQUIRED ?= 0
SEEXPR_MULTIARCH := $(shell $(CXX) -print-multiarch 2>/dev/null)
SEEXPR_LIBDIR ?= $(firstword $(patsubst %/,%,$(dir $(wildcard $(SEEXPR_HOME)/lib/libSeExpr.* $(SEEXPR_HOME)/lib64/libSeExpr.* \
    $(if $(SEEXPR_MULTIARCH),$(SEEXPR_HOME)/lib/$(SEEXPR_MULTIARCH)/libSeExpr.*)))) $(SEEXPR_HOME)/lib)
# the header and the library (a missing library skips the test instead of failing to link)
SEEXPR_FOUND := $(and $(wildcard $(SEEXPR_HOME)/include/SeExpression.h),$(wildcard $(SEEXPR_LIBDIR)/libSeExpr.*))
SEEXPR_SRCS := ../SeExpr/SeExpr.cpp ../SeExpr/SeNoise.cpp ../SeExpr/SeGrain.cpp \
    ../SupportExt/ofxsRectangleInteract.cpp ../SupportExt/ofxsTransformInteract.cpp ../SupportExt/ofxsRamp.cpp \
    ../SupportExt/ofxsOGLTextRenderer.cpp ../SupportExt/ofxsOGLFontData.cpp \
    ../SupportExt/ofxsThreadSuite.cpp ../SupportExt/tinythread.cpp
SEEXPR_CXXFLAGS := -I$(SEEXPR_HOME)/include -I../openfx/Support/Plugins/include
SEEXPR_LIBS := -L$(SEEXPR_LIBDIR) -lSeExpr
ifeq ($(OS),Windows_NT)
  # the SeExpr DLL is found on PATH ($(SEEXPR_HOME)/bin, e.g. $MINGW_PREFIX/bin)
  SEEXPR_LIBS += -lopengl32
else ifeq ($(shell uname -s),Darwin)
  SEEXPR_LIBS += -Wl,-rpath,$(SEEXPR_LIBDIR) -framework OpenGL
else
  SEEXPR_LIBS += -Wl,-rpath,$(SEEXPR_LIBDIR) -lGL
endif

seexpr_test: seexpr_test.cpp seexpr_test_master.inc ofx_test_host.h $(SEEXPR_SRCS) $(OFX_SUPPORT)
	$(CXX) $(OFX_CXXFLAGS) $(SEEXPR_CXXFLAGS) $(filter-out %.h %.inc,$^) $(OFX_LIBS) $(SEEXPR_LIBS) -o $@

# (added to test and clean here, on lines of their own)
.PHONY: test-seexpr clean-seexpr
test: test-seexpr
clean: clean-seexpr
clean-seexpr:
	rm -rf seexpr_test seexpr_test.dSYM

ifneq ($(SEEXPR_FOUND),)
test-seexpr: seexpr_test
	./seexpr_test
else
test-seexpr:
	@echo "test-seexpr: SKIPPED: SeExpr 2.11 (include/SeExpression.h and libSeExpr) not found in SEEXPR_HOME=$(SEEXPR_HOME) (set SEEXPR_HOME)"
	@test "$(SEEXPR_REQUIRED)" != 1 || { echo "test-seexpr: SEEXPR_REQUIRED=1: failing"; exit 1; }
endif

runscript_win32_test: runscript_win32_test.cpp ../RunScript/RunScriptWin32.cpp ../RunScript/RunScriptWin32.h ../RunScript/RunScriptOutput.h
	$(CXX) $(CXXFLAGS) -I../RunScript runscript_win32_test.cpp ../RunScript/RunScriptWin32.cpp $(RUNSCRIPT_LIBS) -o $@

runscript_output_test: runscript_output_test.cpp ../RunScript/RunScriptOutput.h ../RunScript/pstream.h
	$(CXX) $(CXXFLAGS) -I../RunScript runscript_output_test.cpp -o $@

# ---- run targets ----
test: test-ffmpeg test-oiio test-ofx test-render test-dnxhr444-noise test-writeffmpeg-begin-failure test-writeffmpeg-encode-failure test-writeffmpeg-encode-failure-disk-full test-layers test-reader-hooks test-ocio-cdl test-ocio-anim test-runscript test-oiio-threads test-exr

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

# #267: WriteFFmpeg, DNxHR 444 on binary RGB noise: FFmpeg's dnxhd encoder must not abort (the frames encode, or fail
# cleanly). By itself, so that an abort only fails this test.
test-dnxhr444-noise: ofx_render_test
	OCIO=$(CURDIR)/test.ocio ./ofx_render_test $(CURDIR)/.render_out.mov.i267.dnxhr444.mov --dnxhr444-noise

# #289: WriteFFmpeg, a beginSequenceRender that fails after a render of the same instance, then endSequenceRender: no
# crash, and the next render works. By itself, so that a crash only fails this test.
test-writeffmpeg-begin-failure: ofx_render_test
	OCIO=$(CURDIR)/test.ocio ./ofx_render_test $(CURDIR)/.render_out.mov --begin-failure

# #288: WriteFFmpeg, frame 1 is aborted or fails a check of encode() while other threads wait for their turn: they fail
# instead of waiting forever (the test fails after 60 s). By itself, so that a deadlock only fails this test.
test-writeffmpeg-encode-failure: ofx_render_test
	OCIO=$(CURDIR)/test.ocio ./ofx_render_test $(CURDIR)/.render_out.mov.i288.mov --encode-failure

# #288: the same, frame 1 fails to be written (RLIMIT_FSIZE, as on a full disk). Skipped (exit status 77) on MS Windows.
test-writeffmpeg-encode-failure-disk-full: ofx_render_test
	OCIO=$(CURDIR)/test.ocio ./ofx_render_test $(CURDIR)/.render_out.mov.i288.mov --encode-failure-disk-full; \
	    st=$$?; if [ $$st -eq 77 ]; then echo "test-writeffmpeg-encode-failure-disk-full: SKIPPED"; else exit $$st; fi

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

# the animated colorspaces of GenericOCIO, in a host that animates strings and choices like Natron (#128)
# (and a short fuzz run of each plug-in, with fixed seeds; FUZZ_OPS=n for more operations)
FUZZ_OPS ?= 2000
test-ocio-anim: ocio_colorspace_anim_test ocio_anim_fuzz
	OCIO=$(CURDIR)/swap.ocio ./ocio_colorspace_anim_test
	OCIO=$(CURDIR)/swap.ocio ./ocio_anim_fuzz cs $(FUZZ_OPS) 1
	OCIO=$(CURDIR)/swap.ocio ./ocio_anim_fuzz writer $(FUZZ_OPS) 2
	OCIO=$(CURDIR)/swap.ocio ./ocio_anim_fuzz reader $(FUZZ_OPS) 3
	OCIO=$(CURDIR)/swap.ocio OCIO_ANIM_NOT_NATRON=1 ./ocio_colorspace_anim_test
	OCIO=$(CURDIR)/swap.ocio OCIO_ANIM_NOT_NATRON=1 ./ocio_anim_fuzz cs $(FUZZ_OPS) 4
	OCIO=$(CURDIR)/swap.ocio FUZZ_NEAR=1 ./ocio_anim_fuzz cs $(FUZZ_OPS) 5
	# (seeds that found bugs in the review)
	OCIO=$(CURDIR)/swap.ocio FUZZ_NEAR=1 ./ocio_anim_fuzz reader 20000 32
	OCIO=$(CURDIR)/swap.ocio FUZZ_NEAR=1 ./ocio_anim_fuzz cs 50000 77
	OCIO=$(CURDIR)/swap.ocio FUZZ_NEAR=1 ./ocio_anim_fuzz cs 50000 102

# OpenImageIO's thread pool is stopped when the last OIIO plug-in is unloaded, not the first one (#118)
test-oiio-threads: oiio_threadpool_test
	OCIO=$(CURDIR)/test.ocio ./oiio_threadpool_test

# ReadEXR: pixels, data/display windows, UINT, a truncated file, concurrent renders (#123)
test-exr: exr_read_test
	rm -rf .exr_read && mkdir -p .exr_read
	OCIO=$(CURDIR)/test.ocio ./exr_read_test $(CURDIR)/.exr_read

# the same with ThreadSanitizer (needs a compiler that supports -fsanitize=thread)
test-exr-tsan: exr_read_test_tsan
	rm -rf .exr_read_tsan && mkdir -p .exr_read_tsan
	OCIO=$(CURDIR)/test.ocio TSAN_OPTIONS="halt_on_error=1 $(TSAN_OPTIONS)" ./exr_read_test_tsan $(CURDIR)/.exr_read_tsan

# test-exr and test-reader-hooks, DEBUG builds with ThreadSanitizer (no suppressions)
test-exr-tsan-debug: exr_read_test_tsan_debug reader_hooks_test_tsan_debug
	rm -rf .exr_read_tsan_debug && mkdir -p .exr_read_tsan_debug
	OCIO=$(CURDIR)/test.ocio TSAN_OPTIONS="halt_on_error=1 abort_on_error=0 $(TSAN_OPTIONS)" ./exr_read_test_tsan_debug $(CURDIR)/.exr_read_tsan_debug
	OCIO=$(CURDIR)/test.ocio TSAN_OPTIONS="halt_on_error=1 abort_on_error=0 $(TSAN_OPTIONS)" ./reader_hooks_test_tsan_debug $(CURDIR)/.reader_hooks_tsan_debug

# ReadEXR's render time on 2048x1556 RGBA half files, ZIP, PIZ and DWAA (EXR_THREADS: OpenEXR's thread count)
bench-exr: exr_read_bench
	rm -rf .exr_bench && mkdir -p .exr_bench
	OCIO=$(CURDIR)/test.ocio ./exr_read_bench --bench $(CURDIR)/.exr_bench

clean:
	rm -rf ffmpeg_read_test ffmpeg_read_test.dSYM \
	       ofx_entrypoints_test ofx_entrypoints_test.dSYM \
	       ofx_actions_test ofx_actions_test.dSYM \
	       ofx_render_test ofx_render_test.dSYM .render_out.mov .render_out.mov.i79.jpg .render_out.mov.i79.png .render_out.mov.i90a.png .render_out.mov.i90b.png .render_out.mov.i90c.mov .render_out.mov.i71.mp4 .render_out.mov.i71.mp4.avi .render_out.mov.i78.mov .render_out.mov.i78.mp4 .render_out.mov.i110.mov .render_out.mov.i110.mp4v.mov .render_out.mov.i110.3gp .render_out.mov.i135.prores.mov .render_out.mov.i135.dnxhd.mov .render_out.mov.i267.dnxhr444.mov .render_out.mov.i288.mov .render_out.mov.i288.mov.other.mov .render_out.mov.i289-interlaced.mov .render_out.mov.i289-dnxhd.mov .render_out.mov.i136.mp4 .render_out.mov.i217.mp4 .render_out.mov.i137.mov .render_out.mov.i137.mov.lnk .render_out.mov.i137.mov.other.mov .render_out.mov.i134.mov .render_out.mov.i134trunc.mov .render_out.mov.24fps.mkv ofxTestLog.txt \
	       oiio_layers_test oiio_layers_test.dSYM \
	       reader_hooks_test reader_hooks_test.dSYM .reader_hooks.still.tst .reader_hooks.video.tst \
	       ocio_cdl_test ocio_cdl_test.dSYM \
	       ocio_colorspace_anim_test ocio_colorspace_anim_test.dSYM ocio_anim_fuzz ocio_anim_fuzz.dSYM \
	       oiio_threadpool_test oiio_threadpool_test.dSYM \
	       exr_read_test exr_read_test.dSYM exr_read_test_tsan exr_read_test_tsan.dSYM exr_read_bench exr_read_bench.dSYM .exr_read .exr_read_tsan .exr_bench \
	       exr_read_test_tsan_debug exr_read_test_tsan_debug.dSYM reader_hooks_test_tsan_debug reader_hooks_test_tsan_debug.dSYM \
	       .exr_read_tsan_debug .reader_hooks_tsan_debug.still.tst .reader_hooks_tsan_debug.video.tst \
	       runscript_win32_test runscript_win32_test.dSYM runscript_output_test runscript_output_test.dSYM
