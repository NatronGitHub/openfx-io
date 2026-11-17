#!/bin/sh
# Compile check of the FFmpeg plug-in sources against the FFmpeg found by pkg-config, without linking
# (-fsyntax-only), e.g. to check older FFmpeg versions in a container: see "FFmpeg version compile check" in
# tests/README.md. Run from the repository root; $1 is the compiler (default: g++).
CXX="${1:-g++}"
FFCFLAGS="$(pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample)" || exit 1
echo "libavformat $(pkg-config --modversion libavformat), libavcodec $(pkg-config --modversion libavcodec), $($CXX --version | head -n 1)"
st=0
for f in FFmpeg/WriteFFmpeg.cpp FFmpeg/ReadFFmpeg.cpp FFmpeg/FFmpegFile.cpp FFmpeg/PixelFormat.cpp; do
    if $CXX -std=c++17 -fsyntax-only -Wall \
        -DOFX_EXTENSIONS_VEGAS -DOFX_EXTENSIONS_NUKE -DOFX_EXTENSIONS_TUTTLE -DOFX_EXTENSIONS_NATRON -DOFX_IO_MT_FFMPEG \
        -IFFmpeg -IIOSupport -IIOSupport/SequenceParsing -ISupportExt -Iopenfx/include -Iopenfx/Support/include \
        -Iopenfx/Support/Plugins/include $FFCFLAGS "$f"; then
        echo "OK $f"
    else
        echo "FAILED $f"
        st=1
    fi
done
exit $st
