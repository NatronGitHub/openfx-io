PLUGINOBJECTS = \
	ReadFFmpeg.o FFmpegFile.o WriteFFmpeg.o PixelFormat.o \
	GenericReader.o GenericWriter.o GenericOCIO.o SequenceParsing.o ofxsMultiPlane.o
PLUGINNAME = FFmpeg

TOP_SRCDIR = ..
include $(TOP_SRCDIR)/Makefile.master

RESOURCES = \
fr.inria.openfx.ReadFFmpeg.png \
fr.inria.openfx.ReadFFmpeg.svg \
fr.inria.openfx.WriteFFmpeg.png \
fr.inria.openfx.WriteFFmpeg.svg 

CXXFLAGS += $(OCIO_CXXFLAGS) $(FFMPEG_CXXFLAGS)
LINKFLAGS += $(OCIO_LINKFLAGS) $(FFMPEG_LINKFLAGS)
