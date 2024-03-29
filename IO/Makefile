TOP_SRCDIR = ..

PLUGINOBJECTS = ofxsThreadSuite.o tinythread.o \
ofxsFileOpen.o \
ofxsOGLTextRenderer.o \
ofxsOGLFontData.o \
ofxsRamp.o \
ofxsGenerator.o \
ofxsTransformInteract.o \
ofxsMultiPlane.o \
ofxsRectangleInteract.o \
ofxsLut.o \
GenericReader.o GenericWriter.o SequenceParsing.o \
SeExpr.o \
SeGrain.o \
SeNoise.o \
OCIOPluginBase.o GenericOCIO.o $(OCIO_OPENGL_OBJS) \
ReadEXR.o WriteEXR.o \
ReadFFmpeg.o FFmpegFile.o WriteFFmpeg.o PixelFormat.o \
ReadOIIO.o WriteOIIO.o \
OIIOGlobal.o \
OIIOText.o \
OIIOResize.o \
ReadPFM.o WritePFM.o \
ReadPNG.o WritePNG.o \
OCIOColorSpace.o \
OCIODisplay.o \
OCIOCDLTransform.o \
OCIOFileTransform.o \
OCIOLogConvert.o \
OCIOLookTransform.o \

OCIO_OPENGL_OBJS = GenericOCIOOpenGL.o glsl.o glad.o gladegl.o ofxsOGLUtilities.o

ifeq ($(OS),$(filter $(OS),Linux FreeBSD Darwin))
    PLUGINOBJECTS += RunScript.o
endif

PLUGINNAME = IO

# ls */*png */*svg|sed -e 's!^!../!' -e 's!$! \\!'|sort
RESOURCES = \
$(TOP_SRCDIR)/EXR/fr.inria.openfx.ReadEXR.png \
$(TOP_SRCDIR)/EXR/fr.inria.openfx.ReadEXR.svg \
$(TOP_SRCDIR)/EXR/fr.inria.openfx.WriteEXR.png \
$(TOP_SRCDIR)/EXR/fr.inria.openfx.WriteEXR.svg \
$(TOP_SRCDIR)/FFmpeg/fr.inria.openfx.ReadFFmpeg.png \
$(TOP_SRCDIR)/FFmpeg/fr.inria.openfx.ReadFFmpeg.svg \
$(TOP_SRCDIR)/FFmpeg/fr.inria.openfx.WriteFFmpeg.png \
$(TOP_SRCDIR)/FFmpeg/fr.inria.openfx.WriteFFmpeg.svg \
$(TOP_SRCDIR)/OCIO/fr.inria.openfx.OCIOCDLTransform.png \
$(TOP_SRCDIR)/OCIO/fr.inria.openfx.OCIOColorSpace.png \
$(TOP_SRCDIR)/OCIO/fr.inria.openfx.OCIODisplay.png \
$(TOP_SRCDIR)/OCIO/fr.inria.openfx.OCIOFileTransform.png \
$(TOP_SRCDIR)/OCIO/fr.inria.openfx.OCIOLogConvert.png \
$(TOP_SRCDIR)/OCIO/fr.inria.openfx.OCIOLookTransform.png \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.OIIOText.png \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.OIIOText.svg \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.ReadOIIO.png \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.ReadOIIO.svg \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.OIIOResize.png \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.OIIOResize.svg \
$(TOP_SRCDIR)/SeExpr/fr.inria.openfx.SeExpr.png \
$(TOP_SRCDIR)/SeExpr/fr.inria.openfx.SeExpr.svg \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.WriteOIIO.png \
$(TOP_SRCDIR)/OIIO/fr.inria.openfx.WriteOIIO.svg \
$(TOP_SRCDIR)/PFM/fr.inria.openfx.ReadPFM.png \
$(TOP_SRCDIR)/PFM/fr.inria.openfx.ReadPFM.svg \
$(TOP_SRCDIR)/PFM/fr.inria.openfx.WritePFM.png \
$(TOP_SRCDIR)/PFM/fr.inria.openfx.WritePFM.svg \
$(TOP_SRCDIR)/PNG/fr.inria.openfx.ReadPNG.png \
$(TOP_SRCDIR)/PNG/fr.inria.openfx.ReadPNG.svg \
$(TOP_SRCDIR)/PNG/fr.inria.openfx.WritePNG.png \
$(TOP_SRCDIR)/PNG/fr.inria.openfx.WritePNG.svg

ifeq ($(OS),$(filter $(OS),Linux FreeBSD Darwin))
RESOURCES += ../RunScript/fr.inria.openfx.RunScript.png \
             ../RunScript/fr.inria.openfx.RunScript.svg 
endif

VPATH += \
$(TOP_SRCDIR)/EXR \
$(TOP_SRCDIR)/FFmpeg \
$(TOP_SRCDIR)/OCIO \
$(TOP_SRCDIR)/OIIO \
$(TOP_SRCDIR)/PFM \
$(TOP_SRCDIR)/PNG \
$(TOP_SRCDIR)/SeExpr \
$(TOP_SRCDIR)/IOSupport/SequenceParsing

ifeq ($(OS),$(filter $(OS),Linux FreeBSD Darwin))
VPATH += $(TOP_SRCDIR)/RunScript
endif

include $(TOP_SRCDIR)/Makefile.master

CXXFLAGS += -I$(TOP_SRCDIR)/EXR -I$(TOP_SRCDIR)/FFmpeg -I$(TOP_SRCDIR)/OIIO -I$(TOP_SRCDIR)/PFM -I$(TOP_SRCDIR)/OCIO -I$(TOP_SRCDIR)/SeExpr -I$(TOP_SRCDIR)/PNG

ifeq ($(OS),$(filter $(OS),Linux FreeBSD Darwin))
CXXFLAGS += -I$(TOP_SRCDIR)/RunScript
endif

CXXFLAGS  += $(OCIO_CXXFLAGS) $(SEEXPR_CXXFLAGS) $(OPENEXR_CXXFLAGS) $(FFMPEG_CXXFLAGS) $(OIIO_CXXFLAGS) $(PNG_CXXFLAGS)
LINKFLAGS += $(OCIO_LINKFLAGS) $(SEEXPR_LINKFLAGS) $(OPENEXR_LINKFLAGS) $(FFMPEG_LINKFLAGS) $(OIIO_LINKFLAGS) $(PNG_LINKFLAGS)
