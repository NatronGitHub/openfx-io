#!/bin/sh
# Compile check of the FFmpeg plug-in sources against the FFmpeg found by pkg-config, without linking
# (-fsyntax-only), e.g. to check a given FFmpeg version (8.0.1 or later) with PKG_CONFIG_PATH or in a container: see
# "FFmpeg version compile check" in tests/README.md. Run from the repository root; $1 is the compiler (default: g++).
CXX="${1:-g++}"
FFCFLAGS="$(pkg-config --cflags libavformat libavcodec libavutil libswscale libswresample)" || exit 1
# the release name (FFMPEG_VERSION, e.g. "8.0.1"): FFmpeg/FFmpegFile.h rejects 8.0, which has the same library versions
FFVERSION="$(printf '#include <libavutil/ffversion.h>\nFFMPEG_VERSION\n' | $CXX -E -P -x c++ $FFCFLAGS - 2>/dev/null | tail -n 1 | tr -d '"')"
case "$FFVERSION" in "" | FFMPEG_VERSION) FFVERSION="(unknown: no libavutil/ffversion.h)" ;; esac
echo "FFmpeg $FFVERSION, libavformat $(pkg-config --modversion libavformat), libavcodec $(pkg-config --modversion libavcodec), $($CXX --version | head -n 1)"
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
