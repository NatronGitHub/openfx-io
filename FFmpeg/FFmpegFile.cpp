/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2021 The Natron Developers
 * (C) 2013-2018 INRIA
 *
 * openfx-io is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * openfx-io is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with openfx-io.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

/*
 * OFX ffmpeg Reader plugin.
 * Reads a video input file using the libav library.
 * Synced with mov64Reader 11.1v3
 *
 * BUGS:
 * - The last frames from long-GOP mp4 don't read, see:
 *   - https://github.com/NatronGitHub/Natron/issues/241
 *   - https://github.com/NatronGitHub/Natron/issues/231
 * - MPEG1 files cannot be read, for example
 *   - https://github.com/NatronGitHub/Natron-Tests/raw/master/TestReadMPEG1/input.mpg
 *   - http://devernay.free.fr/vision/diffprop/herve3d.mpg
 *   This was already true before the 11.1v3 sync (e.g. at commit 4d0d3a5).
 */
//#define TRACE_DECODE_PROCESS 1

#if (defined(_STDINT_H) || defined(_STDINT_H_) || defined(_MSC_STDINT_H_)) && !defined(UINT64_C)
#warning "__STDC_CONSTANT_MACROS has to be defined before including <stdint.h>, this file will probably not compile."
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS // ...or stdint.h wont' define UINT64_C, needed by libavutil
#endif
#include "FFmpegFile.h"
#include "PixelFormat.h"
// Compat: AVFrame::pkt_duration was renamed to duration in FFmpeg 6 (libavutil 58.2)
// and removed in FFmpeg 8.
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(58, 2, 100)
#  define OFX_AVFRAME_DURATION(f) ((f)->duration)
#else
#  define OFX_AVFRAME_DURATION(f) ((f)->pkt_duration)
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#include <ofxsImageEffect.h>
#include <ofxsMacros.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(WIN64)
#include <windows.h> // for GetSystemInfo()
#define strncasecmp _strnicmp
#define strcasecmp _stricmp // strcasecmp is POSIX: not declared by MinGW without -U__STRICT_ANSI__ under a strict -std
#else
#include <unistd.h> // for sysconf()
#endif

using namespace OFX;

using std::make_pair;
using std::string;

#define CHECK(x)                     \
    {                                \
        int error = x;               \
        if (error < 0) {             \
            setInternalError(error); \
            return;                  \
        }                            \
    }

//#define TRACE_DECODE_PROCESS 1
//#define TRACE_FILE_OPEN 1

// Use one decoding thread per processor for video decoding.
// source: http://git.savannah.gnu.org/cgit/bino.git/tree/src/media_object.cpp
#if 0
static int
video_decoding_threads()
{
    static long n = -1;

    if (n < 0) {
#if defined(_WIN32) // also defined for 64-bit Windows; WIN32 is not defined by MinGW under a strict -std (e.g. -std=c++17)
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        n = si.dwNumberOfProcessors;
#else
        n = sysconf(_SC_NPROCESSORS_ONLN);
#endif
        if (n < 1) {
            n = 1;
        } else if (n > 16) {
            n = 16;
        }
    }

    return n;
}

#endif

static bool
extensionCorrespondToImageFile(const string& ext)
{
    return (ext == "bmp" || ext == "cin" || ext == "dpx" || ext == "exr" ||
            /*ext == "gif" ||*/
            ext == "jpeg" || ext == "jpg" || ext == "pix" || ext == "png" || ext == "ppm" || ext == "ptx" || ext == "rgb" || ext == "rgba" || ext == "tga" || ext == "tiff" || ext == "webp");
}

bool
FFmpegFile::isImageFile(const string& filename)
{
    /// find the last index of the '.' character
    size_t lastDot = filename.find_last_of('.');

    if (lastDot == string::npos) { // we reached the start of the file, return false because we can't determine from the extension
        return false;
    }
    ++lastDot; //< bypass the '.' character
    string ext;
    std::locale loc;
    while (lastDot < filename.size()) {
        ext.append(1, std::tolower(filename.at(lastDot), loc));
        ++lastDot;
    }

    return extensionCorrespondToImageFile(ext);
}

namespace {
struct FilterEntry {
    const char* name;
    bool enableReader;
    bool enableWriter;
};

// Bug 11027 - Nuke write: ffmpeg codec fails has details on individual codecs

// For a full list of formats, define FN_FFMPEGWRITER_PRINT_CODECS in ffmpegWriter.cpp
const FilterEntry kFormatAllowlist[] = {
    { "3gp", true, true },
    { "3g2", true, true },
    { "avi", true, true },
    { "dv", true, false }, // DV (Digital Video), no HD support
    { "flv", true, true }, // FLV (Flash Video), only used with flv codec. cannot be read in official Qt
    { "gif", true, false }, // GIF Animation - write not supported as 8-bit only.
    { "h264", true, false }, // raw H.264 video. prefer a proper container (mp4, mov, avi)
    { "hevc", true, false }, // raw HEVC video. hevc codec cannot be read in official qt
    { "m4v", true, false }, // raw MPEG-4 video. prefer a proper container (mp4, mov, avi)
    { "matroska", true, true }, // not readable in Qt but may be used with other software
    { "mov", true, true },
    { "mp4", true, true },
    { "mpeg", true, true },
    { "mpegts", true, true },
    { "mxf", true, false }, // not readable in Qt but may be used with other software, however MXF has too many constraints to be easyly writable (for H264 it requires avctx.profile = FF_PROFILE_H264_BASELINE, FF_PROFILE_H264_HIGH_10 or FF_PROFILE_H264_HIGH_422). it is better to transcode with an external tool
    { "ogg", true, false }, // Ogg, for theora codec (use ogv for writing)
    { "ogv", true, true }, // Ogg Video, for theora codec
    { nullptr, false, false }
};

// For a full list of formats, define FN_FFMPEGWRITER_PRINT_CODECS in ffmpegWriter.cpp
// A range of codecs are omitted for licensing reasons, or because they support obsolete/unnecessary
// formats that confuse the interface.

#define UNSAFEQT0 true // set to true: not really harmful
#define UNSAFEQT false // set to false: we care about QuickTime, because it is used widely - mainly colorshift issues
#define UNSAFEVLC true // set to true: we don't care much about being playable in VLC
#define TERRIBLE false
//#define SHOULDWORK true
#define SHOULDWORK false
const FilterEntry kCodecAllowlist[] = {
    // Video codecs.
    { "aic", true, false }, // Apple Intermediate Codec (no encoder)
    { "av1", true, false }, // Alliance for Open Media AV1 (encoders: libaom-av1 librav1e)
    { "avrp", true, UNSAFEQT0&& UNSAFEVLC }, // Avid 1:1 10-bit RGB Packer - write not supported as not official qt readable with relevant 3rd party codec.
    { "avui", true, false }, // Avid Meridien Uncompressed - write not supported as this is an SD only codec. Only 720x486 and 720x576 are supported. experimental in ffmpeg 2.6.1.
    { "ayuv", true, UNSAFEQT0&& UNSAFEVLC }, // Uncompressed packed MS 4:4:4:4 - write not supported as not official qt readable.
    { "cfhd", true, true }, // GoPro Cineform HD.
    { "cinepak", true, true }, // Cinepak.
    { "cllc", true, false }, // Canopus Lossless Codec
    { "dnxhd", true, true }, // VC3/DNxHD
    { "dpx", true, true }, // DPX (Digital Picture Exchange) image
    { "dxv", true, false }, // Resolume DXV
    { "exr", true, true }, // EXR image
    { "ffv1", true, UNSAFEQT0&& UNSAFEVLC }, // FFmpeg video codec #1 - write not supported as not official qt readable.
    { "ffvhuff", true, UNSAFEQT0&& UNSAFEVLC }, // Huffyuv FFmpeg variant - write not supported as not official qt readable.
    { "flv", true, UNSAFEQT0 }, // FLV / Sorenson Spark / Sorenson H.263 (Flash Video) - write not supported as not official qt readable.
    { "gif", true, false }, // GIF (Graphics Interchange Format) - write not supported as 8-bit only.
    { "h263p", true, true }, // H.263+ / H.263-1998 / H.263 version 2
    { "h264", true, false }, // H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (the encoder is libx264)
    { "hap", true, true }, // Vidvox Hap
    { "hevc", true, false }, // H.265 / HEVC (High Efficiency Video Coding) (the encoder is libx265)
    { "hq_hqa", true, false }, // Canopus HQ/HQA
    { "hqx", true, false }, // Canopus HQX
    { "huffyuv", true, UNSAFEQT0&& UNSAFEVLC }, // HuffYUV - write not supported as not official qt readable.
    { "jpeg2000", true, UNSAFEQT0 }, // JPEG 2000 - write not supported as not official qt readable.
    { "jpegls", true, UNSAFEQT0 }, // JPEG-LS - write not supported as can't be read in in official qt.
    { "libaom-av1", true, true }, // Alliance for Open Media AV1 encoder
    { "libdav1d", true, false }, // Alliance for Open Media AV1 (encoder: libaom-av1)
    { "libopenh264", true, true }, // Cisco libopenh264 H.264/MPEG-4 AVC encoder
    { "libopenjpeg", true, true }, // OpenJPEG JPEG 2000
    { "librav1e", true, true }, // rav1e AV1 encoder
    { "libschroedinger", true, UNSAFEQT0&& UNSAFEVLC }, // libschroedinger Dirac - write untested. VLC plays with a wrong format
    { "libtheora", true, UNSAFEQT0 }, // libtheora Theora - write untested.
    { "libvpx", true, UNSAFEQT0 }, // On2 VP8
    { "libvpx-vp9", true, UNSAFEQT0 }, // Google VP9
    { "libx264", true, UNSAFEQT0 }, // H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (encoder)
    { "libx264rgb", true, UNSAFEQT0 }, // H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 RGB (encoder)
    { "libx265", true, UNSAFEQT0 }, // H.265 / HEVC (High Efficiency Video Coding) (encoder) - resizes the image
    { "libxavs", true, false }, // Chinese AVS (Audio Video Standard) (encoder) - untested
    { "libxvid", true, true }, // MPEG-4 part 2
    { "ljpeg", true, UNSAFEQT0 }, // Lossless JPEG - write not supported as can't be read in in official qt.
    { "mjpeg", true, true }, // MJPEG (Motion JPEG) - this looks to be MJPEG-A. MJPEG-B encoding is not supported by FFmpeg so is not included here. To avoid confusion over the MJPEG-A and MJPEG-B variants, this codec is displayed as 'Photo JPEG'. This is done to i) avoid the confusion of the naming, ii) be consistent with Apple QuickTime, iii) the term 'Photo JPEG' is recommend for progressive frames which is appropriate to Nuke/NukeStudio as it does not have interlaced support.
    { "mpeg1video", true, TERRIBLE }, // MPEG-1 video - write not supported as it gives random 8x8 blocky errors
    { "mpeg2video", true, true }, // MPEG-2 video
    { "mpeg4", true, true }, // MPEG-4 part 2
    { "msmpeg4v2", true, UNSAFEQT0 }, // MPEG-4 part 2 Microsoft variant version 2 - write not supported as doesn't read in official qt.
    { "msmpeg4", true, UNSAFEQT0 }, // MPEG-4 part 2 Microsoft variant version 3 - write not supported as doesn't read in official qt.
    { "png", true, true }, // PNG (Portable Network Graphics) image
    { "prores", true, false }, // Apple ProRes (the encoder is prores_ks)
    { "qtrle", true, true }, // QuickTime Animation (RLE) video
    { "r10k", true, UNSAFEQT&& UNSAFEVLC }, // AJA Kono 10-bit RGB - write not supported as not official qt readable without colourshifts.
    { "r210", true, UNSAFEQT&& UNSAFEVLC }, // Uncompressed RGB 10-bit - write not supported as not official qt readable with relevant 3rd party codec without colourshifts.
    // rawvideo: WriteFFmpeg only writes it in a pixel format that the container has a tag for (rawvideoPixelFormats() in
    // WriteFFmpeg.cpp), e.g. in QuickTime 2vuy and raw (RGB24, ARGB), and from FFmpeg 8 v308, v408 and v410.
    { "rawvideo", true, true }, // raw video
    { "svq1", true, true }, // Sorenson Vector Quantizer 1 / Sorenson Video 1 / SVQ1
    { "targa", true, true }, // Truevision Targa image.
    { "theora", true, false }, // Theora (decoder).
    { "tiff", true, true }, // TIFF Image
    // v210: Uncompressed 4:2:2 10-bit. It was disabled because of QuickTime "colourshifts", which came from the missing
    // colour tags: WriteFFmpeg tags v210 with the Y'CbCr matrix, primaries and transfer function of the usual video
    // formats (IsYUVFromShortName()) and with video range, and always sets the write_colr movflag, so that the mov muxer
    // writes the 'colr' atom (FFmpeg n9.0 libavformat/movenc.c:2948-2953, n4.4 movenc.c:2247). The muxer also writes
    // the version 2 sample description, lossless spatial quality and 'clap' atom that Apple's TN2162 asks for
    // uncompressed Y'CbCr (n9.0 movenc.c:2804-2831, 3040-3041), and the encoder pads each row to 48 pixels (128 bytes),
    // as TN2162 requires (n9.0 libavcodec/v210enc.c:72-73).
    { "v210", true, true },
    // v308, v408 and v410: FFmpeg 9 removed these encoders (FFmpeg n8.0 libavcodec/allcodecs.c:354-358 lists them, n9.0
    // does not), and from FFmpeg 8 the mov muxer writes rawvideo in the VYU444, UYVA and V30XLE pixel formats with the
    // same tags (n8.0 libavformat/movenc.c:1963-1965): WriteFFmpeg uses rawvideo when these encoders are missing.
    { "v308", true, UNSAFEQT0&& UNSAFEVLC }, // Uncompressed packed 4:4:4 - 8-bit only.
    { "v408", true, UNSAFEQT0&& UNSAFEVLC }, // Uncompressed packed QT 4:4:4:4 - 8-bit only.
    { "v410", true, UNSAFEQT0&& UNSAFEVLC }, // Uncompressed 4:4:4 10-bit.
    { "vc2", true, UNSAFEQT0&& UNSAFEVLC }, // SMPTE VC-2 (previously BBC Dirac Pro).
    { "vp8", true, false }, // On2 VP8 (decoder)
    { "vp9", true, false }, // Google VP9 (decoder)

    // Audio codecs.
    { "pcm_alaw", true, true }, // PCM A-law / G.711 A-law
    { "pcm_f32be", true, true }, // PCM 32-bit floating point big-endian
    { "pcm_f32le", true, true }, // PCM 32-bit floating point little-endian
    { "pcm_f64be", true, true }, // PCM 64-bit floating point big-endian
    { "pcm_f64le", true, true }, // PCM 64-bit floating point little-endian
    { "pcm_mulaw", true, true }, // PCM mu-law / G.711 mu-law
    { "pcm_s16be", true, true }, // PCM signed 16-bit big-endian
    { "pcm_s16le", true, true }, // PCM signed 16-bit little-endian
    { "pcm_s24be", true, true }, // PCM signed 24-bit big-endian
    { "pcm_s24le", true, true }, // PCM signed 24-bit little-endian
    { "pcm_s32be", true, true }, // PCM signed 32-bit big-endian
    { "pcm_s32le", true, true }, // PCM signed 32-bit little-endian
    { "pcm_s8", true, true }, // PCM signed 8-bit
    { "pcm_u16be", true, true }, // PCM unsigned 16-bit big-endian
    { "pcm_u16le", true, true }, // PCM unsigned 16-bit little-endian
    { "pcm_u24be", true, true }, // PCM unsigned 24-bit big-endian
    { "pcm_u24le", true, true }, // PCM unsigned 24-bit little-endian
    { "pcm_u32be", true, true }, // PCM unsigned 32-bit big-endian
    { "pcm_u32le", true, true }, // PCM unsigned 32-bit little-endian
    { "pcm_u8", true, true }, // PCM unsigned 8-bit
    { nullptr, false, false }
};

const FilterEntry*
getEntry(const char* name,
         const FilterEntry* allowlist,
         const FilterEntry* denylist = nullptr)
{
    const FilterEntry* iterAllowlist = allowlist;
    const size_t nameLength = strlen(name);

    // check for normal mode
    while (iterAllowlist->name != nullptr) {
        size_t iteNameLength = strlen(iterAllowlist->name);
        size_t maxLength = (nameLength > iteNameLength) ? nameLength : iteNameLength;
        if (strncmp(name, iterAllowlist->name, maxLength) == 0) {
            // Found in allowlist, now check denylist
            if (denylist) {
                const FilterEntry* iterDenylist = denylist;

                while (iterDenylist->name != nullptr) {
                    iteNameLength = strlen(iterDenylist->name);
                    maxLength = (nameLength > iteNameLength) ? nameLength : iteNameLength;
                    if (strncmp(name, iterDenylist->name, maxLength) == 0) {
                        // Found in codec allowlist but denylisted too
                        return nullptr;
                    }

                    ++iterDenylist;
                }
            }

            // Found in allowlist and not in denylist
            return iterAllowlist;
        }

        ++iterAllowlist;
    }

    return nullptr;
}
} // namespace {

bool
FFmpegFile::isFormatAllowedForReading(const char* formatName)
{
    const FilterEntry* allowlistEntry = getEntry(formatName, kFormatAllowlist);

    return (allowlistEntry && allowlistEntry->enableReader);
}

bool
FFmpegFile::isFormatAllowedForWriting(const char* formatName)
{
    const FilterEntry* allowlistEntry = getEntry(formatName, kFormatAllowlist);

    return (allowlistEntry && allowlistEntry->enableWriter);
}

bool
FFmpegFile::isCodecAllowedForReading(const char* codecName)
{
    const FilterEntry* allowlistEntry = getEntry(codecName, kCodecAllowlist);

    return (allowlistEntry && allowlistEntry->enableReader);
}

bool
FFmpegFile::isCodecAllowedForWriting(const char* codecName)
{
    const FilterEntry* allowlistEntry = getEntry(codecName, kCodecAllowlist);

    return (allowlistEntry && allowlistEntry->enableWriter);
}

// The Y'CbCr matrix of the conversion to RGB (the "YCbCr Matrix" param of ReadFFmpeg):
// - Rec. 601, Rec. 709 or Rec. 2020 if the user chose one,
// - Auto: the matrix of the frame (AVFrame.colorspace: the decoders give the one of the bitstream, e.g. the H.264/HEVC
//   VUI, the ProRes frame header or the DNxHD frame header, which is BT.709 when FFmpeg wrote it, FFmpeg n9.0.2
//   libavcodec/dnxhddec.c:237-242 and libavcodec/dnxhdenc.c:544, whatever the container says), or else the one of the codec context, which avcodec_parameters_to_context()
//   copied from the container, e.g. the MOV 'colr' atom or the Matroska Colour element: fill_frame_props() in FFmpeg
//   n9.0 libavcodec/decode.c:575-585, n4.0 libavcodec/decode.c:1717-1724), else the one of the stream
//   (AVCodecParameters.color_space), else, if neither is a matrix that swscale has (see
//   FFmpeg::swsColorspaceFromAVColorSpace()), the usual one for the picture size (Rec. 709 from 720 lines, else
//   Rec. 601; JPEG is always Rec. 601: isRec709Format()), as before the tags were used.
int
FFmpegFile::Stream::getSwsColorspace(int frameColorSpace)
{
    switch (_colorMatrixOverride) {
    case FFmpegFile::eColorMatrixRec601:
        return SWS_CS_ITU601;
    case FFmpegFile::eColorMatrixRec709:
        return SWS_CS_ITU709;
    case FFmpegFile::eColorMatrixRec2020:
        return SWS_CS_BT2020;
    default:
        break;
    }
    int colorspace = FFmpeg::swsColorspaceFromAVColorSpace(frameColorSpace);
    if ((colorspace < 0) && _avstream && _avstream->codecpar) {
        colorspace = FFmpeg::swsColorspaceFromAVColorSpace(_avstream->codecpar->color_space);
    }
    if (colorspace < 0) {
        colorspace = isRec709Format() ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }

    return colorspace;
}

// The source range of the conversion to RGB, as the srcRange argument of sws_setColorspaceDetails() (0 = video
// range, 16..235 for 8 bits, 1 = full range, 0..255, libswscale/swscale.h): the "YCbCr Range" param of ReadFFmpeg,
// or Auto: the range of the frame (AVFrame.color_range), or, if it is unspecified, full range for the deprecated
// YUVJ pixel formats (which are full range, libavutil/pixfmt.h), else video range for Y'CbCr and full range for RGB.
int
FFmpegFile::Stream::getSwsSrcRange(AVPixelFormat srcPixelFormat,
                                   int frameColorRange) const
{
    switch (_colorRangeOverride) {
    case FFmpegFile::eColorRangeVideo:
        return 0;
    case FFmpegFile::eColorRangeFull:
        return 1;
    default:
        break;
    }
    switch (frameColorRange) {
    case AVCOL_RANGE_MPEG:
        return 0;
    case AVCOL_RANGE_JPEG:
        return 1;
    default:
        break;
    }
    switch (srcPixelFormat) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
        return 1;
    default:
        break;
    }

    // If the colour range wasn't specified, set the flag according to
    // whether the data is YUV or not.
    return isYUV() ? 0 : 1;
}

SwsContext*
FFmpegFile::Stream::getConvertCtx(AVPixelFormat srcPixelFormat,
                                  int srcWidth,
                                  int srcHeight,
                                  int srcColorRange,
                                  int srcColorSpace,
                                  AVPixelFormat dstPixelFormat,
                                  int dstWidth,
                                  int dstHeight)
{
    // The Y'CbCr matrix and range of the conversion, from the tags of the frame and the params: if they change, the
    // context is rebuilt
    const int swsColorspace = getSwsColorspace(srcColorSpace);
    const int srcRange = getSwsSrcRange(srcPixelFormat, srcColorRange);
    // The context is also rebuilt if any of the source/destination parameters it was created for have changed since.
    const bool paramsChanged = _convertCtx
        && (_convertCtxSrcPixelFormat != srcPixelFormat
            || _convertCtxSrcWidth != srcWidth
            || _convertCtxSrcHeight != srcHeight
            || _convertCtxSrcRange != srcRange
            || _convertCtxSwsColorspace != swsColorspace
            || _convertCtxDstPixelFormat != dstPixelFormat
            || _convertCtxDstWidth != dstWidth
            || _convertCtxDstHeight != dstHeight);
    if (_resetConvertCtx || paramsChanged) {
        _resetConvertCtx = false;
        if (_convertCtx) {
            sws_freeContext(_convertCtx);
            _convertCtx = nullptr;
        }
    }

    if (!_convertCtx) {
        // Remember the (original) parameters this context is being built for, so a
        // later call with different parameters triggers a rebuild.
        _convertCtxSrcPixelFormat = srcPixelFormat;
        _convertCtxSrcWidth = srcWidth;
        _convertCtxSrcHeight = srcHeight;
        _convertCtxSrcRange = srcRange;
        _convertCtxSwsColorspace = swsColorspace;
        _convertCtxDstPixelFormat = dstPixelFormat;
        _convertCtxDstWidth = dstWidth;
        _convertCtxDstHeight = dstHeight;

        // Preventing deprecated pixel format used error messages, see:
        // https://libav.org/doxygen/master/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5
        // This manually sets them to the new versions of equivalent types (their full range is in srcRange).
        switch (srcPixelFormat) {
        case AV_PIX_FMT_YUVJ420P:
            srcPixelFormat = AV_PIX_FMT_YUV420P;
            break;
        case AV_PIX_FMT_YUVJ422P:
            srcPixelFormat = AV_PIX_FMT_YUV422P;
            break;
        case AV_PIX_FMT_YUVJ444P:
            srcPixelFormat = AV_PIX_FMT_YUV444P;
            break;
        case AV_PIX_FMT_YUVJ440P:
            srcPixelFormat = AV_PIX_FMT_YUV440P;
            break;
        default:
            break;
        }

        _convertCtx = sws_getContext(srcWidth, srcHeight, srcPixelFormat, // src format
                                     dstWidth, dstHeight, dstPixelFormat, // dest format
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);

        // Set up the SoftWareScaler to convert colorspaces correctly.
        // Colorspace conversion makes no sense for RGB->RGB conversions
        if (!_convertCtx || !isYUV()) {
            return _convertCtx;
        }

        int result = sws_setColorspaceDetails(_convertCtx,
                                              sws_getCoefficients(swsColorspace), // inv_table
                                              srcRange, // srcRange -flag indicating the white-black range of the input (1=jpeg / 0=mpeg) 0 = 16..235, 1 = 0..255
                                              sws_getCoefficients(SWS_CS_DEFAULT), // table
                                              1, // dstRange - 0 = 16..235, 1 = 0..255
                                              0, // brightness fixed point, with 0 meaning no change,
                                              1 << 16, // contrast   fixed point, with 1<<16 meaning no change,
                                              1 << 16); // saturation fixed point, with 1<<16 meaning no change);

        assert(result != -1);
        (void)result;
    }

    return _convertCtx;
} // FFmpegFile::Stream::getConvertCtx

/*static*/ double
FFmpegFile::Stream::GetStreamAspectRatio(Stream* stream)
{
    // a sample aspect ratio with a numerator or denominator of 0 or less is unknown or invalid (av_q2d() would give
    // 0, inf, NaN or a negative PAR)
    if ((stream->_avstream->sample_aspect_ratio.num > 0) && (stream->_avstream->sample_aspect_ratio.den > 0)) {
#if TRACE_FILE_OPEN
        std::cout << "      Aspect ratio (from stream)=" << av_q2d(stream->_avstream->sample_aspect_ratio) << std::endl;
#endif

        return av_q2d(stream->_avstream->sample_aspect_ratio);
    } else if ((stream->_codecContext->sample_aspect_ratio.num > 0) && (stream->_codecContext->sample_aspect_ratio.den > 0)) {
#if TRACE_FILE_OPEN
        std::cout << "      Aspect ratio (from codec)=" << av_q2d(stream->_codecContext->sample_aspect_ratio) << std::endl;
#endif

        return av_q2d(stream->_codecContext->sample_aspect_ratio);
    }
#if TRACE_FILE_OPEN
    else {
        std::cout << "      Aspect ratio unspecified, assuming " << stream->_aspect << std::endl;
    }
#endif

    return stream->_aspect;
}

// Whether the frames with a negative timestamp at the start of the stream are an edit-list pre-roll, which is not
// displayed: only in MOV/MP4 (the "mov,mp4,m4a,3gp,3g2,mj2" demuxer, libavformat/mov.c:12205 in FFmpeg 9.0), whose
// edit list starts after the first samples (mov_fix_index(), mov.c:4374, gives them negative timestamps and marks
// those before the edit as AVINDEX_DISCARD_FRAME, mov.c:4567). Its timestamps have 64 bits (avpriv_set_pts_info(st,
// 64, ...), mov.c:5353), and the demuxer does not have AVFMT_TS_DISCONT (mov.c:12209).
// Elsewhere, negative timestamps are frames to display. In particular, libavformat unwraps the timestamps of the
// streams with pts_wrap_bits < 63 (MPEG-TS and MPEG-PS: 33 bits, the default of avformat_new_stream(),
// libavformat/options.c:295, and mpegts.c:954; FLV: 32 bits, flvdec.c:208), and when the first timestamp is less
// than 60 s (or 1/8 of the range) before the wrap, it subtracts the wrap offset from the timestamps before the wrap,
// which become negative, as does AVStream::start_time (update_wrap_reference() and update_timestamps(),
// libavformat/demux.c:477-560 in FFmpeg 9.0 and 467-550 in 8.0, utils.c:745-898 in 4.4; wrap_timestamp(),
// demux.c:53-66; with the correct_ts_overflow option, on by default, libavformat/options_table.h:83). The
// demuxers with AVFMT_TS_DISCONT (MPEG-TS, MPEG-PS, live FLV: mpegts.c:3871, mpeg.c:707, flvdec.c:2009) may also
// have timestamp discontinuities. The index entries and the timestamps read by the binary search of a seek are
// unwrapped in the same way (ff_wrap_timestamp(), libavformat/seek.c:126 and 286).
static bool
NegativeStartIsPreroll(const AVFormatContext* context,
                       const AVStream* avstream)
{
    const char* formatName = (context->iformat && context->iformat->name) ? context->iformat->name : "";
    const bool isMov = std::strstr(formatName, "mov") != nullptr;
    return isMov && (avstream->pts_wrap_bits >= 63) && !(context->iformat->flags & AVFMT_TS_DISCONT);
}

// get stream start time
// The frames are counted from it (AVStream::start_time: the first PTS of the stream). In a stream cut on an
// I-frame, the leading B-frames of an open GOP, which reference a frame before the cut, have a PTS before it:
// they are not counted (they cannot be decoded). In a stream cut in the middle of a GOP, the frames after it
// whose reference frames are cut are counted, and fail: they are on the time line of the stream (e.g. its
// audio), which a count from the first decodable frame would shift.
int64_t
FFmpegFile::getStreamStartTime(Stream& stream)
{
#if TRACE_FILE_OPEN
    std::cout << "      Determining stream start PTS:" << std::endl;
#endif

    MyAVPacket avPacket;

    // Read from stream. If the value read isn't valid, get it from the first frame in the stream that provides such a
    // value.
    int64_t startPTS = stream._avstream->start_time;
#if TRACE_FILE_OPEN
    if (startPTS != int64_t(AV_NOPTS_VALUE)) {
        std::cout << "        Obtained from AVStream::start_time=";
    }
#endif

    // A negative start time is only an edit-list pre-roll, whose frames are not displayed, in MOV/MP4 (see
    // NegativeStartIsPreroll()): elsewhere it is the real start of the stream (e.g. an MPEG-TS whose timestamps
    // wrap, which libavformat unwraps to negative timestamps), and its frames are counted from it.
    const bool negativeStart = (startPTS != int64_t(AV_NOPTS_VALUE)) && (startPTS < 0) && !NegativeStartIsPreroll(_context, stream._avstream);
    if ((startPTS < 0) && !negativeStart) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVStream::start_time, searching frames..." << std::endl;
#endif

        // Seek 1st key-frame in video stream.
        avcodec_flush_buffers(stream._codecContext);

        // Here, avPacket needs to be local as we don't need to keep any information outside this function context.
        // Do not replace this with _avPacket, which is global, because _avPacket is associated with the playback process
        // and that may produces inconsistencies. _avPacket is now used only in the |decode| method and we need to ensure
        // that it remains valid even after we get out of the |decode| method context, because the information stored by
        // _avPacket may be displayed on the screen for multiple frames. Please have a look at TP 162892 for more information
        // https://foundry.tpondemand.com/entity/162892
        _readAtOpen = true;
        if (av_seek_frame(_context, stream._idx, startPTS, AVSEEK_FLAG_BACKWARD) >= 0) {

            // Read frames until we get one for the video stream that contains a valid PTS.
            while (av_read_frame(_context, avPacket.pkt()) >= 0) {
                if (avPacket->stream_index != stream._idx) {
                    continue;
                }
                // Packet read for video stream. Get its PTS.
                startPTS = avPacket->pts;

                // Loop will continue if the current packet doesn't end after 0
                if (startPTS + avPacket->duration > 0) {
                    break;
                }
            }
        }
#if TRACE_FILE_OPEN
        else {
            std::cout << "          Seek error, aborted search" << std::endl;
        }
#endif

#if TRACE_FILE_OPEN
        if (startPTS != int64_t(AV_NOPTS_VALUE)) {
            std::cout << "        Found by searching frames=";
        }
#endif
    }

    // If we still don't have a valid initial PTS, assume 0. (This really shouldn't happen for any real media file, as
    // it would make meaningful playback presentation timing and seeking impossible.);
    // TP 162519 - We discard the samples with a negative timestamp to make mov64 match the Quick Time Player;
    // Video streams are usually inter-coded (most frames rely on other frames to be decoded). So, when such a stream
    // is trimmed those reference frames, even if not within the trimmed time range, have to be included in the output
    // in order for the video to be playable. These frames are assigned to a negative time-stamp;
    // Based on the experimental work we concluded that for the streams that have included frames which are assigned
    // to negative timestamps it doesn't make sense to pick a value grater than 0 for the first frame timestamp. The first
    // frame timestamp is going to match the packet which starts just before 0 and ends after 0 if that exists. Otherwise
    // it will be 0.
    // For more information please have a look at TP 162519
    // This only applies to MOV/MP4 edit lists (see NegativeStartIsPreroll()): a negative start time of another
    // container (e.g. an unwrapped MPEG-TS) is kept, and its frames are counted from it.
    const bool isStartPTSValid = negativeStart || (startPTS + avPacket->duration > 0);
    if (!isStartPTSValid) {
#if TRACE_FILE_OPEN
        std::cout << "        Not found by searching frames, assuming ";
#endif
        startPTS = 0;
    }

#if TRACE_FILE_OPEN
    std::cout << startPTS << " ticks, " << double(startPTS) * double(stream._avstream->time_base.num) / double(stream._avstream->time_base.den) << " s" << std::endl;
#endif

    // The DTS of the first packet, for the FLV duration (see getStreamFrames()). With B-frames, read it
    // from the first packet. Some containers (AVI) have a DTS but no PTS: libavformat then gives the
    // packets of the frames that are not reordered (B-frames) a PTS equal to their DTS, and the
    // decoder gives the other frames the DTS of the packet after which they are output
    // (best_effort_timestamp, see demuxAndDecode()). With B-frames, the timestamps of the decoded
    // frames are thus delayed by the reordering delay, and the first decoded frame is not at the DTS
    // of the first packet: use its timestamp as the start.
    stream._startDTS = startPTS;
    if (stream._codecContext->has_b_frames > 0) {
        int64_t firstDTS = AV_NOPTS_VALUE;
        const int64_t firstFramePTS = probeStreamStart(stream, firstDTS);
        if (firstDTS != int64_t(AV_NOPTS_VALUE) && (firstDTS <= startPTS)) {
            stream._startDTS = firstDTS;
        }
        // Only if the first decoded frame is within the reordering delay of the first packet: a later one is
        // after frames that cannot be decoded (e.g. an MPEG-PS cut in the middle of a GOP, whose first packets
        // have no PTS), and whether the decoder outputs it within the packets read depends on its threads.
        const int64_t frameTicks = stream.frameToPts(1) - stream.frameToPts(0);
        if ((firstFramePTS != int64_t(AV_NOPTS_VALUE)) && (firstDTS != int64_t(AV_NOPTS_VALUE)) &&
            (firstFramePTS - firstDTS <= (stream._codecContext->has_b_frames + 1) * frameTicks)) {
#if TRACE_FILE_OPEN
            std::cout << "        Packets have no PTS, start from the first decoded frame=" << firstFramePTS << " ticks" << std::endl;
#endif
            startPTS = firstFramePTS;
        }
    }

    return startPTS;
} // FFmpegFile::getStreamStartTime

// Read the first packet of the stream, and set firstDTS to its DTS. If it has a DTS but no PTS,
// return the timestamp of the first decoded frame (best_effort_timestamp), else AV_NOPTS_VALUE.
int64_t
FFmpegFile::probeStreamStart(Stream& stream, int64_t& firstDTS)
{
    int64_t firstFramePTS = AV_NOPTS_VALUE;

    firstDTS = AV_NOPTS_VALUE;
    avcodec_flush_buffers(stream._codecContext);
    // If nothing was read or sought since avformat_find_stream_info(), the first packets are those it kept,
    // from the start of the file: no seek is needed. Otherwise, seek to the start time (by bytes if it is
    // unknown, e.g. in elementary streams), never with AVSEEK_FLAG_FRAME, which makes some demuxers (FLV)
    // read the file from its start.
    if (_readAtOpen) {
        const int res = (stream._avstream->start_time != int64_t(AV_NOPTS_VALUE)) ? av_seek_frame(_context, stream._idx, stream._avstream->start_time, AVSEEK_FLAG_BACKWARD) : av_seek_frame(_context, -1, 0, AVSEEK_FLAG_BYTE);
        if (res < 0) {
            return firstFramePTS;
        }
    }
    _readAtOpen = true;
    MyAVPacket avPacket;
    AVFrame* avFrame = av_frame_alloc();
    bool firstPacket = true;
    bool eof = false;
    // Enough packets for the decoder to output a frame (its delay, with frame threads), but bounded.
    int packetsLeft = stream.getCodecDelay() + 16;
    while (avFrame && !eof && (packetsLeft > 0)) {
        int res = av_read_frame(_context, avPacket.pkt());
        if (res < 0) {
            eof = true;
        } else if (avPacket->stream_index != stream._idx) {
            av_packet_unref(avPacket.pkt());
            continue;
        } else if (firstPacket) {
            firstPacket = false;
            firstDTS = avPacket->dts;
            if ((avPacket->pts != int64_t(AV_NOPTS_VALUE)) || (avPacket->dts == int64_t(AV_NOPTS_VALUE))) {
                // the packets have a PTS (or no timestamp at all): nothing to decode
                av_packet_unref(avPacket.pkt());
                break;
            }
        }
        if (firstPacket) {
            break; // no packet in the stream
        }
        --packetsLeft;
        res = avcodec_send_packet(stream._codecContext, eof ? nullptr : avPacket.pkt());
        av_packet_unref(avPacket.pkt());
        if ((res < 0) && (res != AVERROR(EAGAIN)) && (res != AVERROR_INVALIDDATA)) {
            break;
        }
        if (avcodec_receive_frame(stream._codecContext, avFrame) >= 0) {
            firstFramePTS = avFrame->best_effort_timestamp;
            break;
        }
    }
    av_frame_free(&avFrame);
    avcodec_flush_buffers(stream._codecContext);

    return firstFramePTS;
} // FFmpegFile::probeStreamStart

// The index entries of a stream (avformat_index_get_entry() appeared in lavf 58.78, FFmpeg 4.4 has 58.76).
static int
IndexEntriesCount(AVStream* st)
{
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 78, 100)
    return avformat_index_get_entries_count(st);
#else
    return st->nb_index_entries;
#endif
}

static const AVIndexEntry*
IndexEntry(AVStream* st, int i)
{
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 78, 100)
    return avformat_index_get_entry(st, i);
#else
    return &st->index_entries[i];
#endif
}

// The number of frames of the stream that are not displayed (marked as discarded in the index, e.g. by an
// MOV/MP4 edit list).
int64_t
FFmpegFile::getDiscardedFrames(Stream& stream)
{
    int64_t discarded = 0;
    const int entries = IndexEntriesCount(stream._avstream);
    for (int i = 0; i < entries; ++i) {
        const AVIndexEntry* entry = IndexEntry(stream._avstream, i);
        if (entry && (entry->flags & AVINDEX_DISCARD_FRAME)) {
            ++discarded;
        }
    }

    return discarded;
}

#if LIBAVFORMAT_VERSION_MAJOR < 59
// The largest timestamp in the index of the stream (e.g. the MOV/MP4 sample table, the Matroska cues), in
// AV_TIME_BASE units, or AV_NOPTS_VALUE if the index is empty.
int64_t
FFmpegFile::getLastIndexTimestamp(Stream& stream)
{
    int64_t last = AV_NOPTS_VALUE;
    const int entries = IndexEntriesCount(stream._avstream);
    for (int i = 0; i < entries; ++i) {
        const AVIndexEntry* entry = IndexEntry(stream._avstream, i);
        if (entry && !(entry->flags & AVINDEX_DISCARD_FRAME) && (entry->timestamp != int64_t(AV_NOPTS_VALUE)) && ((last == int64_t(AV_NOPTS_VALUE)) || (entry->timestamp > last))) {
            last = entry->timestamp;
        }
    }

    return (last == int64_t(AV_NOPTS_VALUE)) ? last : av_rescale_q(last, stream._avstream->time_base, AV_TIME_BASE_Q);
}
#endif

// Whether the data of an FLV video tag (dataSize >= 1 bytes) is a video frame, not a sequence header, an end of
// sequence or a command (legacy and enhanced FLV).
static bool
isFlvVideoFrame(const unsigned char* d,
                int64_t dataSize)
{
    if (((d[0] >> 4) & 7) == 5) {
        return false; // video info/command frame
    }
    if (d[0] & 0x80) {
        // enhanced FLV: the packet type is in the low 4 bits (1: coded frames, 3: coded frames without composition time)
        return ((d[0] & 0x0f) == 1) || ((d[0] & 0x0f) == 3);
    }
    const int codecId = d[0] & 0x0f;
    if ((codecId == 7) || (codecId == 12)) { // AVC, HEVC: the AVC packet type (1: NAL units)
        return (dataSize >= 2) && (d[1] == 1);
    }

    return true;
}

// The start of the last tags of an FLV file (at most kMaxTags tags, and at least kMinVideoTags video tags if
// possible), found by walking back from the end with the size of the previous tag, which follows each tag, so
// that the demuxer does not have to resync. The last bytes of the file are read at once (256 kB), and more if
// needed (only the extra bytes), up to maxBytes in all. Each tag header is checked (type, data size): -1 if the
// last tag is not valid (e.g. a truncated file).
static int64_t
FlvLastTagsStart(AVIOContext* pb,
                 int64_t size,
                 int64_t maxBytes)
{
    const int kMaxTags = 64;
    const int kMinVideoTags = 8;
    const int kMinFrameTags = 2;
    const int64_t firstTag = 13; // after the 9-byte header and the first previous tag size
    std::vector<unsigned char> data; // the bytes [begin, size) of the file
    int64_t begin = size;
    int64_t start = -1;
    for (int64_t window = int64_t(1) << 18; begin > firstTag; window *= 4) {
        const int64_t newBegin = (std::max)(firstTag, size - (std::min)(window, maxBytes));
        if (newBegin >= begin) {
            break; // maxBytes reached
        }
        std::vector<unsigned char> more(size_t(begin - newBegin));
        if ((avio_seek(pb, newBegin, SEEK_SET) < 0) || (avio_read(pb, more.data(), (int)more.size()) != (int)more.size())) {
            return -1;
        }
        data.insert(data.begin(), more.begin(), more.end());
        begin = newBegin;
        auto at = [&data, begin](int64_t pos) -> const unsigned char* { return &data[size_t(pos - begin)]; };
        int64_t tag = size;
        int videoTags = 0;
        int tags = 0;
        start = -1;
        // At most kMaxTags tags, unless fewer than kMinFrameTags video frames were found (e.g. a low frame
        // rate with audio: the last tags are audio, and the AVC end of sequence).
        for (; ((tags < kMaxTags) || (videoTags < kMinFrameTags)) && (tag - 4 >= begin); ++tags) {
            const unsigned char* p = at(tag - 4);
            const int64_t previousTagSize = (int64_t(p[0]) << 24) | (int64_t(p[1]) << 16) | (int64_t(p[2]) << 8) | int64_t(p[3]);
            const int64_t tagStart = tag - 4 - previousTagSize;
            if ((previousTagSize < 11) || (tagStart < firstTag)) {
                if (tag == size) {
                    return -1; // the last tag is not valid
                }
                break;
            }
            if (tagStart < begin) {
                if (tag == size) {
                    // A last tag larger than the bytes read: check its header before reading more (the previous
                    // tag size of a truncated file may be anything).
                    unsigned char header[4];
                    if ((avio_seek(pb, tagStart, SEEK_SET) < 0) || (avio_read(pb, header, 4) != 4) ||
                        (((header[0] & 0x1f) != 8) && ((header[0] & 0x1f) != 9) && ((header[0] & 0x1f) != 18)) ||
                        ((((int64_t(header[1]) << 16) | (int64_t(header[2]) << 8) | int64_t(header[3])) + 11) != previousTagSize)) {
                        return -1;
                    }
                }
                break;
            }
            const unsigned char* t = at(tagStart);
            const int type = t[0] & 0x1f;
            const int64_t dataSize = (int64_t(t[1]) << 16) | (int64_t(t[2]) << 8) | int64_t(t[3]);
            if (((type != 8) && (type != 9) && (type != 18)) || (dataSize + 11 != previousTagSize)) {
                if (tag == size) {
                    return -1; // the last tag is not valid
                }
                break;
            }
            if ((type == 9) && (dataSize >= 1) && isFlvVideoFrame(t + 11, dataSize)) {
                ++videoTags;
            }
            start = tagStart;
            tag = tagStart;
        }
        if ((start < 0) && (tag == size) && (tag - 4 >= begin)) {
            // the last tag starts before the bytes read: read more
            continue;
        }
        if ((videoTags >= kMinVideoTags) || (tag - 4 < firstTag) || (start < 0) || ((tags >= kMaxTags) && (videoTags >= kMinFrameTags))) {
            break;
        }
    }

    return start;
}

// Read the last packets of the file, to get the end of the movie (the largest PTS + duration of the packets of all
// streams, in AV_TIME_BASE units) and the largest PTS of the video stream (in its time base). This reads at most
// kMaxEndProbeBytes: the demuxer is positioned through its index (the MOV/MP4 samples, the Matroska cues) or, in FLV,
// at one of the last tags. Returns false if they cannot be read (e.g. the input is not seekable, there is no
// index, or the last FLV tag is not valid, as in a truncated file), and the caller then uses the duration.
// A video packet without a duration lasts one frame. Another stream that ends after the last frame of the video
// stream (e.g. the last audio packet) only adds the whole frames it covers (the last frame is held until the end
// of the movie, see demuxAndDecode()): audio that ends a few milliseconds after the video does not add a frame.
bool
FFmpegFile::probeEnd(Stream& stream,
                     int64_t& movieEnd,
                     int64_t& lastVideoPts)
{
    movieEnd = AV_NOPTS_VALUE;
    lastVideoPts = AV_NOPTS_VALUE;
    AVIOContext* pb = _context->pb;
    if (!pb || !(pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        return false;
    }
    const int64_t size = avio_size(pb);
    if (size <= 0) {
        return false;
    }
    // The FLV tag walk and the reading of the packets each read at most kMaxEndProbeBytes (the Matroska and MOV/MP4
    // seeks only read the index: the cues, read at the first seek, or nothing). This is per video stream: with
    // several matching video streams (views), each one is probed.
    const int64_t kMaxEndProbeBytes = int64_t(4) << 20;
    const char* formatName = _context->iformat ? _context->iformat->name : "";
    const bool isFlv = (std::strcmp(formatName, "flv") == 0) || (std::strcmp(formatName, "live_flv") == 0);
    const bool isMatroska = std::strstr(formatName, "matroska") != nullptr;
    const bool isMpegTsPs = (std::strcmp(formatName, "mpegts") == 0) || (std::strcmp(formatName, "mpeg") == 0);
    _readAtOpen = true;
    // MPEG-TS/PS have no index, but their demuxers resync after a byte seek: read the last 1 MB (or 4 MB if it
    // has too few video packets, e.g. a high bit rate), to get the end that FFmpeg's estimate may miss.
    int64_t tsWindow = int64_t(1) << 20;
    if (isFlv) {
        // Enough video tags for the reordered frames (with B-frames, the last tag is not the last frame).
        const int64_t pos = FlvLastTagsStart(pb, size, kMaxEndProbeBytes);
        if ((pos < 0) || (av_seek_frame(_context, -1, pos, AVSEEK_FLAG_BYTE) < 0)) {
            return false;
        }
    } else if (isMpegTsPs) {
        if (av_seek_frame(_context, -1, (std::max)(int64_t(0), size - tsWindow), AVSEEK_FLAG_BYTE) < 0) {
            return false;
        }
    } else {
        if (isMatroska) {
            // the Matroska demuxer reads the cues at the first seek
            av_seek_frame(_context, stream._idx, stream._startPTS, AVSEEK_FLAG_BACKWARD);
        }
        // The index entry to seek to: in MOV/MP4, where every sample is indexed, one of the last samples
        // (with AVSEEK_FLAG_ANY, so that only the last samples are read, whatever the GOP), enough before
        // the last one to read the reordered ones; in Matroska, the last cue (a keyframe). It must be within
        // the last kMaxEndProbeBytes of the file: otherwise (e.g. a long GOP in Matroska, or no cues near the
        // end, e.g. written to a pipe, or truncated), the demuxer would read too much.
        const int entries = IndexEntriesCount(stream._avstream);
        const bool isMov = std::strstr(formatName, "mov") != nullptr;
        int entry = entries - 1;
        if (isMov) {
            entry -= stream._codecContext->has_b_frames + 4;
        }
        if (entries < 1) {
            return false;
        }
        entry = (std::max)(0, entry);
        const AVIndexEntry* indexEntry = IndexEntry(stream._avstream, entry);
        if (!indexEntry || (indexEntry->pos < size - kMaxEndProbeBytes) || (indexEntry->timestamp == int64_t(AV_NOPTS_VALUE))) {
            return false;
        }
        if (av_seek_frame(_context, stream._idx, indexEntry->timestamp, AVSEEK_FLAG_BACKWARD | (isMov ? AVSEEK_FLAG_ANY : 0)) < 0) {
            return false;
        }
    }
    const int64_t frameDuration = av_rescale(AV_TIME_BASE, stream._fpsDen, stream._fpsNum);
    int64_t end = AV_NOPTS_VALUE;
    int64_t videoEnd = AV_NOPTS_VALUE;
    bool eof = false;
    int videoPackets = 0;
    int64_t bytesRead0 = pb->bytes_read;
    bool refPending = false; // a video packet without a PTS was read (MPEG-PS reference frame)
    int unplacedRefs = 0; // the video packets without a PTS since the last one with a PTS
    const int64_t frameTicks = stream.frameToPts(1) - stream.frameToPts(0);
    // The streams of the program of the video stream (all of them if there are no programs)
    const AVProgram* program = nullptr;
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
    program = av_find_program_from_stream(_context, nullptr, stream._idx);
#endif
    auto sameProgram = [program](int index) -> bool {
        if (!program) {
            return true;
        }
        for (unsigned i = 0; i < program->nb_stream_indexes; ++i) {
            if ((int)program->stream_index[i] == index) {
                return true;
            }
        }
        return false;
    };
    // The FLV demuxer adds an index entry for each keyframe it reads from a seekable input: an entry near
    // the end would make later seeks in a file without a keyframe index decode from near its start.
    const int seekable = pb->seekable;
    if (isFlv) {
        pb->seekable &= ~AVIO_SEEKABLE_NORMAL;
    }
    MyAVPacket avPacket;
    while (pb->bytes_read - bytesRead0 <= kMaxEndProbeBytes) {
        const int res = av_read_frame(_context, avPacket.pkt());
        if ((res == AVERROR_EOF) && isMpegTsPs && (videoPackets < 8) && (tsWindow < kMaxEndProbeBytes) && (tsWindow < size)) {
            // too few video packets in the last bytes: read more
            tsWindow = kMaxEndProbeBytes;
            end = videoEnd = lastVideoPts = AV_NOPTS_VALUE;
            videoPackets = 0;
            refPending = false;
            unplacedRefs = 0;
            bytesRead0 = pb->bytes_read;
            if (av_seek_frame(_context, -1, (std::max)(int64_t(0), size - tsWindow), AVSEEK_FLAG_BYTE) < 0) {
                break;
            }
            continue;
        }
        if (res == AVERROR_EOF) {
            eof = true;
            break;
        }
        if (res < 0) {
            break; // an error: use the duration
        }
        const AVStream* st = _context->streams[avPacket->stream_index];
        const bool isVideoStream = avPacket->stream_index == stream._idx;
        if (!isVideoStream && (!sameProgram(avPacket->stream_index) || ((st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) && (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)))) {
            // another program (e.g. a DVB multiplex), or subtitles or data (e.g. SCTE-35, teletext, KLV)
            av_packet_unref(avPacket.pkt());
            continue;
        }
        if (isVideoStream && (avPacket->pts == int64_t(AV_NOPTS_VALUE)) && (avPacket->dts != int64_t(AV_NOPTS_VALUE))) {
            // A reference frame without a PTS (MPEG-PS): displayed after the B-frames that follow it in decoding
            // order (see the last-resort count in getStreamFrames()).
            refPending = true;
            ++unplacedRefs;
        } else if (isVideoStream && refPending && (avPacket->pts != int64_t(AV_NOPTS_VALUE)) && (avPacket->dts != int64_t(AV_NOPTS_VALUE)) &&
                   (avPacket->pts > avPacket->dts)) {
            // a reference frame with a PTS (displayed after its DTS, unlike a B-frame): the pending one is
            // displayed before it
            refPending = false;
        } else if (isVideoStream && refPending && (avPacket->pts != int64_t(AV_NOPTS_VALUE)) && (frameTicks > 0)) {
            const int64_t refPts = avPacket->pts + frameTicks;
            const int64_t e = av_rescale_q(refPts + frameTicks, st->time_base, AV_TIME_BASE_Q);
            end = (end == int64_t(AV_NOPTS_VALUE)) ? e : (std::max)(end, e);
            videoEnd = (videoEnd == int64_t(AV_NOPTS_VALUE)) ? e : (std::max)(videoEnd, e);
            lastVideoPts = (lastVideoPts == int64_t(AV_NOPTS_VALUE)) ? refPts : (std::max)(lastVideoPts, refPts);
        }
        if (isVideoStream && (avPacket->pts != int64_t(AV_NOPTS_VALUE))) {
            unplacedRefs = 0;
        }
        const int64_t ts = (avPacket->pts != int64_t(AV_NOPTS_VALUE)) ? avPacket->pts : avPacket->dts;
        if ((ts != int64_t(AV_NOPTS_VALUE)) && !(avPacket->flags & AV_PKT_FLAG_DISCARD)) {
            int64_t duration = avPacket->duration;
            if ((duration <= 0) && (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) && (st->r_frame_rate.num > 0) && (st->r_frame_rate.den > 0)) {
                duration = av_rescale_q(1, av_inv_q(st->r_frame_rate), st->time_base);
            }
            const int64_t e = av_rescale_q(ts + (std::max)(int64_t(0), duration), st->time_base, AV_TIME_BASE_Q);
            if ((end == int64_t(AV_NOPTS_VALUE)) || (e > end)) {
                end = e;
            }
            if (avPacket->stream_index == stream._idx) {
                ++videoPackets;
                if ((videoEnd == int64_t(AV_NOPTS_VALUE)) || (e > videoEnd)) {
                    videoEnd = e;
                }
                if ((avPacket->pts != int64_t(AV_NOPTS_VALUE)) && ((lastVideoPts == int64_t(AV_NOPTS_VALUE)) || (avPacket->pts > lastVideoPts))) {
                    lastVideoPts = avPacket->pts;
                }
            }
        }
        av_packet_unref(avPacket.pkt());
    }
    pb->seekable = seekable;
    if (eof && (unplacedRefs > 0) && (lastVideoPts != int64_t(AV_NOPTS_VALUE)) && (frameTicks > 0)) {
        // The last reference frames have no PTS, and no B-frame follows them: each is displayed after the
        // previous frame (e.g. the last frame of a VOB).
        lastVideoPts += unplacedRefs * frameTicks;
        const int64_t e = av_rescale_q(lastVideoPts + frameTicks, stream._avstream->time_base, AV_TIME_BASE_Q);
        end = (end == int64_t(AV_NOPTS_VALUE)) ? e : (std::max)(end, e);
        videoEnd = (videoEnd == int64_t(AV_NOPTS_VALUE)) ? e : (std::max)(videoEnd, e);
    }
    if (isMpegTsPs && (videoPackets <= stream._codecContext->has_b_frames)) {
        eof = false; // too few video packets to know the last frame (with B-frames): use the duration
    }
    if (!eof || (end == int64_t(AV_NOPTS_VALUE)) || (videoEnd == int64_t(AV_NOPTS_VALUE))) {
        // gave up before the end of the file, or no video packet near the end
        lastVideoPts = AV_NOPTS_VALUE;
        return false;
    }
    if ((end > videoEnd) && (frameDuration > 0)) {
        // whole frames only (within 1 ms, for the rounding of the timestamps)
        end = videoEnd + ((end - videoEnd + 1000) / frameDuration) * frameDuration;
    }
    movieEnd = end;

    return true;
}

int
FFmpegFile::getLastIndexedFrame() const
{
    if (!_selectedStream) {
        return -1;
    }
    AVStream* st = _selectedStream->_avstream;
    int64_t last = AV_NOPTS_VALUE;
    const int entries = IndexEntriesCount(st);
    for (int i = 0; i < entries; ++i) {
        const AVIndexEntry* e = IndexEntry(st, i);
        if (e && (e->timestamp != int64_t(AV_NOPTS_VALUE)) && ((last == int64_t(AV_NOPTS_VALUE)) || (e->timestamp > last))) {
            last = e->timestamp;
        }
    }

    return (last == int64_t(AV_NOPTS_VALUE)) ? -1 : _selectedStream->ptsToFrame(last);
}

// Get the video stream duration in frames...
int64_t
FFmpegFile::getStreamFrames(Stream& stream)
{
#if TRACE_FILE_OPEN
    std::cout << "      Determining stream frame count:" << std::endl;
#endif

    int64_t frames = 0;

    // Obtain from movie duration if specified. This is preferred since mov/mp4 formats allow the media in
    // tracks (=streams) to be remapped in time to the final movie presentation without needing to recode the
    // underlying tracks content; the movie duration thus correctly describes the final presentation.
    if (frames <= 0 && _context->duration > 0) {
        // Annoyingly, FFmpeg exposes the movie duration converted (with round-to-nearest semantics) to units of
        // AV_TIME_BASE (microseconds in practice) and does not expose the original rational number duration
        // from a mov/mp4 file's "mvhd" atom/box. Accuracy may be lost in this conversion; a duration that was
        // an exact number of frames as a rational may end up as a duration slightly over or under that number
        // of frames in units of AV_TIME_BASE.
        // Conversion to whole frames rounds up the resulting number of frames because a partial frame is still
        // a frame. However, in an attempt to compensate for AVFormatContext's inaccurate representation of
        // duration, with unknown rounding direction, the conversion to frames subtracts 1 unit (microsecond)
        // from that duration. The rationale for this is thus:
        // * If the stored duration exactly represents an exact number of frames, then that duration minus 1
        //   will result in that same number of frames once rounded up.
        // * If the stored duration is for an exact number of frames that was rounded down, then that duration
        //   minus 1 will result in that number of frames once rounded up.
        // * If the stored duration is for an exact number of frames that was rounded up, then that duration
        //   minus 1 will result in that number of frames once rounded up, while that duration unchanged would
        //   result in 1 more frame being counted after rounding up.
        // * If the original duration in the file was not for an exact number of frames, then the movie timebase
        //   would have to be >= 10^6 for there to be any chance of this calculation resulting in the wrong
        //   number of frames. This isn't a case that I've seen. Even if that were to be the case, the original
        //   duration would have to be <= 1 microsecond greater than an exact number of frames in order to
        //   result in the wrong number of frames, which is highly improbable.
        // The same applies to the stream's time base: the duration is a number of ticks of the time base, and
        // with a coarse time base it may have been rounded up by up to a tick. Matroska/WebM use 1 ms ticks,
        // so a 40-frame 24 fps movie lasts 1667 ms, i.e. 40.008 frames, and would give 41 frames. Subtract one
        // tick of the time base (at least 1 microsecond, at most half a frame) instead of 1 microsecond.
        int64_t tick = av_rescale_q_rnd(1, stream._avstream->time_base, AV_TIME_BASE_Q, AV_ROUND_UP);
        const int64_t halfFrame = (int64_t(AV_TIME_BASE) * stream._fpsDen) / (2 * int64_t(stream._fpsNum));
        if (tick > halfFrame) {
            tick = halfFrame;
        }
        if (tick < 1) {
            tick = 1;
        }
        // The movie lasts from the start of the video stream (the first frame) to the end of the movie, which may
        // be later than the end of the video stream (another stream may be longer: the last frame is then held,
        // see demuxAndDecode()).
        // AVFormatContext::duration is counted from AVFormatContext::start_time, the start of the earliest stream,
        // which may be before the video stream (e.g. audio in MPEG-TS or M2TS: an AVCHD M2TS counted an extra
        // frame). But FFmpeg reports the duration stored by some containers as is, and it is not counted from the
        // start of the movie: Matroska/WebM store the duration of their timeline, which begins at timestamp 0 (e.g.
        // with a start time of 10 s, a 40-frame 24 fps movie has a duration of 11.667 s), and FLV see below.
        const char* formatName = _context->iformat ? _context->iformat->name : "";
        const bool isMatroska = std::strstr(formatName, "matroska") != nullptr;
        const bool isMov = std::strstr(formatName, "mov") != nullptr;
        const bool isFlv = (std::strcmp(formatName, "flv") == 0) || (std::strcmp(formatName, "live_flv") == 0);
        const int64_t videoStart = av_rescale_q(stream._startPTS, stream._avstream->time_base, AV_TIME_BASE_Q);
        // the number of frames from the start of the video stream until a movie end (in AV_TIME_BASE units)
        auto framesUntil = [&](int64_t end) -> int64_t {
            const int64_t duration = end - videoStart;
            const int64_t divisor = int64_t(AV_TIME_BASE) * stream._fpsDen;
            return (duration > tick) ? ((duration - tick) * stream._fpsNum + divisor - 1) / divisor : 0;
        };
        int64_t movieEnd = AV_NOPTS_VALUE;
        if (isFlv) {
            // FLV timestamps are DTS (with a composition time offset for the PTS). FFmpeg's FLV muxer stores the
            // duration from the first DTS of any stream (before the first frame with B-frames, or the start of an
            // audio track that starts first), and without that metadata (e.g. written to a pipe) the demuxer gives
            // the DTS of the last tag, counted from 0: read the end of the movie from the last packets instead.
            // The duration, if the last packets cannot be read (e.g. a truncated file): it may be counted from the
            // first DTS (FFmpeg's FLV muxer), or be the DTS of the last tag, counted from 0 (without the duration
            // metadata), which does not tell; count it from the start of the video stream, as before, which
            // includes the B-frame delay: never fewer frames than the duration.
            const int64_t durationEnd = _context->duration + videoStart;
            int64_t lastVideoPts;
            // A probed end far beyond the duration is not trusted (e.g. a damaged last tag).
            if (!probeEnd(stream, movieEnd, lastVideoPts) || (movieEnd > durationEnd + (std::max)(_context->duration / 10, int64_t(2) * AV_TIME_BASE))) {
                movieEnd = durationEnd;
            }
        } else if (_context->start_time == int64_t(AV_NOPTS_VALUE)) {
            // the start is unknown (e.g. NUT with B-frames, whose duration is counted from the first PTS)
            movieEnd = _context->duration + videoStart;
        } else if (isMatroska) {
            // Matroska: the duration is the end of the movie (from timestamp 0). But a muxer may write a length
            // while the timestamps do not start at 0 (e.g. mkvmerge with split parts or --sync): with a start
            // time, read the end of the movie from the last packets, as for FLV.
            // Without a duration in the file (e.g. written to a pipe), FFmpeg estimates it from the bit rate, as a
            // length from the start.
            int64_t lastVideoPts;
            if ((_context->start_time <= 0) || !probeEnd(stream, movieEnd, lastVideoPts)) {
                movieEnd = _context->duration;
                if ((_context->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE) && (_context->start_time > 0)) {
                    movieEnd += _context->start_time;
                }
            }
        } else if (isMov) {
            // MOV/MP4: the duration is counted from the start. But in MOV/MP4 with an initial empty edit, lavf 58
            // (FFmpeg 4) counts it from 0. Use the one that gives the number of samples of the stream (minus the
            // ones that are not displayed), if only one of them does (with B-frames and no edit list, e.g. in
            // fragmented MP4, the start time is the B-frame delay, and both may).
            const int64_t streamFrames = (stream._avstream->nb_frames > 0) ? (stream._avstream->nb_frames - getDiscardedFrames(stream)) : 0;
            const int64_t fromStart = _context->start_time + _context->duration;
            const int64_t fromZero = _context->duration;
            const bool startMatches = (streamFrames > 0) && (std::abs(double(framesUntil(fromStart) - streamFrames)) <= 1);
            const bool zeroMatches = (streamFrames > 0) && (std::abs(double(framesUntil(fromZero) - streamFrames)) <= 1);
            movieEnd = fromStart;
            if (zeroMatches && !startMatches) {
                movieEnd = fromZero;
            }
#if LIBAVFORMAT_VERSION_MAJOR < 59
            else if (!startMatches) {
                // without the number of samples (fragmented MP4): a duration that does not reach the last
                // indexed sample is not an end
                const int64_t lastIndexed = getLastIndexTimestamp(stream);
                if ((lastIndexed != int64_t(AV_NOPTS_VALUE)) && (fromZero > lastIndexed + av_rescale(AV_TIME_BASE, stream._fpsDen, stream._fpsNum))) {
                    movieEnd = fromZero;
                }
            }
#endif
        } else {
            movieEnd = _context->start_time + _context->duration;
            // MPEG-TS/PS: FFmpeg estimates the duration from the timestamps of packets near the end, and may miss
            // the last frames of the video (e.g. with a high bit rate): read the last packets.
            int64_t probedEnd, lastVideoPts;
            // (only near FFmpeg's estimate: within 10 % of the duration, at least 2 s)
            if (((std::strcmp(formatName, "mpegts") == 0) || (std::strcmp(formatName, "mpeg") == 0)) && probeEnd(stream, probedEnd, lastVideoPts) &&
                (std::abs(double(probedEnd - movieEnd)) <= double((std::max)(_context->duration / 10, int64_t(2) * AV_TIME_BASE)))) {
                movieEnd = probedEnd;
            }
        }
        frames = framesUntil(movieEnd);

        // The above calculation is not reliable, because it seems in some situations (such as rendering out a mov
        // with 5 frames at 24 fps from Nuke) the duration has been rounded up to the nearest millisecond, which
        // leads to an extra frame being reported.  To attempt to work around this, compare against the number of
        // frames in the stream, and if they differ by one, use that value instead.
        // In MOV/MP4, the frames before the start of the edit list (e.g. to decode the first displayed frame, if
        // it is not a keyframe, or with negative timestamps) are counted in AVStream::nb_frames, but they are not
        // displayed: the demuxer marks them as discarded in the index.
        // But a stream may have fewer samples than frame times (a gap in the timestamps, e.g. after a stream copy
        // from a non-keyframe, or VFR): when snapping down, the count must still reach the last sample (its PTS,
        // from the last packets), which may also be after the end given by the duration.
        int64_t streamFrames = stream._avstream->nb_frames;
        if (streamFrames > 0) {
            streamFrames -= getDiscardedFrames(stream);
        }
        if ((streamFrames > 0) && (std::abs((double)(frames - streamFrames)) <= 1)) {
            frames = streamFrames;
        }
        if (isMov && (streamFrames > 0)) {
            int64_t probedEnd, lastVideoPts;
            if (probeEnd(stream, probedEnd, lastVideoPts) && (lastVideoPts != int64_t(AV_NOPTS_VALUE))) {
                frames = (std::max)(frames, int64_t(stream.ptsToFrame(lastVideoPts)) + 1);
            }
        }
#if TRACE_FILE_OPEN
        std::cout << "        Obtained from AVFormatContext::duration & framerate=";
#endif
    }

    // If number of frames still unknown, obtain from stream's number of frames if specified. Will be 0 if
    // unknown.
    if (frames <= 0 && stream._avstream->nb_frames > 0) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVFormatContext::duration, obtaining from AVStream::nb_frames..." << std::endl;
#endif
        frames = stream._avstream->nb_frames;
#if TRACE_FILE_OPEN
        if (frames) {
            std::cout << "        Obtained from AVStream::nb_frames=";
        }
#endif
    }

    // If number of frames still unknown, attempt to calculate from stream's duration, fps and timebase.
    if (frames <= 0 && stream._avstream->duration > 0) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by AVStream::nb_frames, calculating from duration & framerate..." << std::endl;
#endif
        // round to the nearest frame, as Stream::ptsToFrame()
        frames = av_rescale_rnd(stream._avstream->duration, int64_t(stream._avstream->time_base.num) * stream._fpsNum, int64_t(stream._avstream->time_base.den) * stream._fpsDen, AV_ROUND_NEAR_INF);
#if TRACE_FILE_OPEN
        if (frames > 0) {
            std::cout << "        Calculated from duration & framerate=";
        }
#endif
    }

    // If the number of frames is still unknown, attempt to measure it from the last frame PTS for the stream in the
    // file relative to first (which we know from earlier).
    if (frames <= 0) {
#if TRACE_FILE_OPEN
        std::cout << "        Not specified by duration & framerate, searching frames for last PTS..." << std::endl;
#endif

        int64_t maxPts = stream._startPTS;

        // Seek last key-frame.
        _readAtOpen = true;
        avcodec_flush_buffers(stream._codecContext);
        av_seek_frame(_context, stream._idx, stream.frameToPts(1 << 29), AVSEEK_FLAG_BACKWARD);

        // Read up to last frame, extending max PTS for every valid PTS value found for the video stream.
        MyAVPacket avPacket;
        // Here, avPacket needs to be local as we don't need to keep any information outside this function context.
        // Do not replace this with _avPacket, which is global, because _avPacket is associated with the playback process
        // and that may produces inconsistencies. _avPacket is now used only in the |decode| method and we need to ensure
        // that it remains valid even after we get out of the |decode| method context, because the information stored by
        // _avPacket may be displayed on the screen for multiple frames. Please have a look at TP 162892 for more information
        // https://foundry.tpondemand.com/entity/162892

        // A packet without a PTS (a reference frame in an MPEG-2 elementary stream with B-frames, or in AVI) is
        // displayed at least at its DTS, delayed as the timestamps of the decoded frames (see
        // getStreamStartTime()), and after the frames that follow it in decoding order until the next reference
        // frame (B-frames): e.g. the last reference frame is the last frame displayed.
        const int64_t frameDuration = stream.frameToPts(1) - stream.frameToPts(0);
        bool refPending = false; // a packet without a PTS was read
        while (av_read_frame(_context, avPacket.pkt()) >= 0) {
            if (avPacket->stream_index == stream._idx) {
                int64_t pts = avPacket->pts;
                if ((pts == int64_t(AV_NOPTS_VALUE)) && (avPacket->dts != int64_t(AV_NOPTS_VALUE))) {
                    pts = (std::max)(avPacket->dts + (stream._startPTS - stream._startDTS), maxPts + frameDuration);
                    refPending = true;
                } else if ((pts != int64_t(AV_NOPTS_VALUE)) && refPending && (pts + frameDuration > maxPts)) {
                    // the pending reference frame is displayed after this one
                    maxPts = pts + frameDuration;
                }
                if ((pts != int64_t(AV_NOPTS_VALUE)) && (pts > maxPts)) {
                    maxPts = pts;
                }
            }
            av_packet_unref(avPacket.pkt());
        }
#if TRACE_FILE_OPEN
        std::cout << "          Start PTS=" << stream._startPTS << ", Max PTS found=" << maxPts << std::endl;
#endif

        // Compute frame range from min to max PTS. Need to add 1 as both min and max are at starts of frames, so stream
        // extends for 1 frame beyond this.
        frames = 1 + stream.ptsToFrame(maxPts);
#if TRACE_FILE_OPEN
        std::cout << "        Calculated from frame PTS range=";
#endif
    }

#if TRACE_FILE_OPEN
    std::cout << frames << std::endl;
#endif

    return frames;
} // FFmpegFile::getStreamFrames

// Returns true if the properties of the two streams are considered to match in terms of
// codec, resolution, frame rate, time base, etc. The motivation for this is that streams
// that match in this way are more likely to contain multiple views rather then unrelated
// content. This is somewhat arbitrary but seems like a reasonable starting point until
// users tell us otherwise.
static bool
CheckStreamPropertiesMatch(const AVStream* streamA,
                           const AVStream* streamB)
{
#if 0 // FF_API_LAVF_AVCTX
    const AVCodecContext* codecA = streamA->codec;
    const AVCodecContext* codecB = streamB->codec;
#else
    const AVCodecParameters* codecA = streamA->codecpar;
    const AVCodecParameters* codecB = streamB->codecpar;
#endif

    // Sanity check //
    if (codecA == nullptr || codecB == nullptr) {
        return false;
    }

#if 0 // FF_API_LAVF_AVCTX
    AVPixelFormat codecAfmt = codecA->pix_fmt;
    AVPixelFormat codecBfmt = codecB->pix_fmt;
#else
    AVPixelFormat codecAfmt = (AVPixelFormat)codecA->format;
    AVPixelFormat codecBfmt = (AVPixelFormat)codecB->format;
#endif

    // Not matching even if both reads failed
    if (codecAfmt == AV_PIX_FMT_NONE || codecBfmt == AV_PIX_FMT_NONE) {
        return false;
    }

    const AVPixFmtDescriptor* pixFmtDescA = av_pix_fmt_desc_get(codecAfmt);
    const AVPixFmtDescriptor* pixFmtDescB = av_pix_fmt_desc_get(codecBfmt);

    return (codecA->codec_id == codecB->codec_id) && (codecA->bits_per_raw_sample == codecB->bits_per_raw_sample) && (codecA->width == codecB->width) && (codecA->height == codecB->height) && (codecA->sample_aspect_ratio.num == codecB->sample_aspect_ratio.num) && (codecA->sample_aspect_ratio.den == codecB->sample_aspect_ratio.den) && (pixFmtDescA->nb_components == pixFmtDescB->nb_components) && (streamA->sample_aspect_ratio.num == streamB->sample_aspect_ratio.num) && (streamA->sample_aspect_ratio.den == streamB->sample_aspect_ratio.den) && (streamA->time_base.num == streamB->time_base.num) && (streamA->time_base.den == streamB->time_base.den) && (streamA->start_time == streamB->start_time) && (streamA->duration == streamB->duration) && (streamA->nb_frames == streamB->nb_frames) && (streamA->r_frame_rate.num == streamB->r_frame_rate.num) && (streamA->r_frame_rate.den == streamB->r_frame_rate.den);
}

// constructor
FFmpegFile::FFmpegFile(const string& filename)
    : _filename(filename)
    , _context(nullptr)
    , _streams()
    , _selectedStream(nullptr)
    , _errorMsg()
    , _invalidState(false)
    , _seekCount(0)
    , _bytesReadAtOpen(0)
    , _readAtOpen(false)
    , _damagedPacketWarned(false)
#ifdef OFX_IO_MT_FFMPEG
    , _lock()
    , _invalidStateLock()
#endif
{
#ifdef OFX_IO_MT_FFMPEG
    // MultiThread::AutoMutex guard(_lock); // not needed in a constructor: we are the only owner
#endif

#if TRACE_FILE_OPEN
    std::cout << "FFmpeg Reader=" << this << "::c'tor(): filename=" << filename << std::endl;
#endif

    assert(!_filename.empty());
    AVDictionary* demuxerOptions = nullptr;
    // enabling drefs to allow reading from external tracks
    // this enables quicktime reference files demuxing
    av_dict_set(&demuxerOptions, "enable_drefs", "1", 0);
    CHECK(avformat_open_input(&_context, _filename.c_str(), nullptr, &demuxerOptions));
    if (demuxerOptions != nullptr) {
        // demuxerOptions is destroyed and replaced, on avformat_open_input return,
        // with a dict containing the options that were not found
        // assert(false && "invalid options passed to demuxer");
        // Actually, this is OK since enable_drefs is a valid option only for mov/mp4/3gp
        // https://ffmpeg.org/ffmpeg-formats.html#Options-1
        av_dict_free(&demuxerOptions);
    }
    assert(_context);
    // Bug 51016 - probesize is the maximum amount of data, in bytes, which is read to determine
    // frame information in avformat_find_stream_info. It's important that when reading
    // stereo quicktimes that the probe size be large enough to read data from both video tracks.
    // Otherwise the streams could be reported as having different properties, when really they're
    // the same but due to an insufficient probesize the second stream didn't have all the relevant data
    // loaded. This defaults to 5meg. 100meg should be enough for large stereo quicktimes.
    _context->probesize = 100000000;

    CHECK(avformat_find_stream_info(_context, nullptr));

#if TRACE_FILE_OPEN
    std::cout << "  " << _context->nb_streams << " streams:" << std::endl;
#endif

    // fill the array with all available video streams
    bool unsupported_codec = false;

    // find all streams that the library is able to decode
    for (unsigned i = 0; i < _context->nb_streams; ++i) {
#if TRACE_FILE_OPEN
        std::cout << "    FFmpeg stream index " << i << ": ";
#endif
        AVStream* avstream = _context->streams[i];

        // be sure to have a valid stream
        if (!avstream) {
#if TRACE_FILE_OPEN
            std::cout << "No valid stream, skipping..." << std::endl;
#endif
            continue;
        }

        AVCodecContext* codecCtx = avcodec_alloc_context3(nullptr);
        if (!codecCtx) {
#if TRACE_FILE_OPEN
            std::cout << "No valid codec context, skipping..." << std::endl;
#endif
            continue;
        }

        int ret = avcodec_parameters_to_context(codecCtx, avstream->codecpar);
        if (ret < 0) {
#if TRACE_FILE_OPEN
            std::cout << "Could not convert to context, skipping..." << std::endl;
#endif
            avcodec_free_context(&codecCtx);
            continue;
        }

        // considering only video streams, skipping audio
        if (codecCtx->codec_type != AVMEDIA_TYPE_VIDEO) {
#if TRACE_FILE_OPEN
            std::cout << "Not a video stream, skipping..." << std::endl;
#endif
            avcodec_free_context(&codecCtx);
            continue;
        }
        if (codecCtx->pix_fmt == AV_PIX_FMT_NONE) {
#if TRACE_FILE_OPEN
            std::cout << "Unknown pixel format, skipping..." << std::endl;
#endif
            avcodec_free_context(&codecCtx);
            continue;
        }

        // find the codec
        const AVCodec* videoCodec = avcodec_find_decoder(codecCtx->codec_id);
        if (videoCodec == nullptr) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder not found, skipping..." << std::endl;
#endif
            avcodec_free_context(&codecCtx);
            continue;
        }

        // skip codecs not in the white list
        // string reason;
        if (!isCodecAllowedForReading(videoCodec->name)) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" disallowed, skipping..." << std::endl;
#endif
            unsupported_codec = true;
            avcodec_free_context(&codecCtx);
            continue;
        }
        // Some format/codec combinations, even if readable, do not have timestamps.
        // See also codecCompatible() in WriteFFmpeg.cpp which contains a similar check.
        if ((std::string(_context->iformat->name) == "avi") && (codecCtx->codec_id == AV_CODEC_ID_H264 || codecCtx->codec_id == AV_CODEC_ID_MPEG1VIDEO || codecCtx->codec_id == AV_CODEC_ID_MPEG2VIDEO)) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" in format \"" << _context->iformat->name << "\" disallowed because it lacks timestamps, skipping..." << std::endl;
#endif
            unsupported_codec = true;
            avcodec_free_context(&codecCtx);
            continue;
        }

        if (codecCtx->codec_type == AVMEDIA_TYPE_VIDEO) {
            // source: http://git.savannah.gnu.org/cgit/bino.git/tree/src/media_object.cpp

            // Some codecs support multi-threaded decoding (eg mpeg). Its fast but causes problems when opening many readers
            // simultaneously since each opens as many threads as you have cores. This leads to resource starvation and failed reads.
            // Hopefully, getNumCPUs() will give us the right number of usable cores

            // Activate multithreaded decoding. This must be done before opening the codec; see
            // http://lists.gnu.org/archive/html/bino-list/2011-08/msg00019.html
#ifdef AV_CODEC_CAP_AUTO_THREADS
            // Do not use AV_CODEC_CAP_AUTO_THREADS, since it may create too many threads
            // if (avstream->codec->codec && (avstream->codec->codec->capabilities & AV_CODEC_CAP_AUTO_THREADS)) {
            //    avstream->codec->thread_count = 0;
            //} else
#endif
            {
                codecCtx->thread_count = (std::min)((int)MultiThread::getNumCPUs(), OFX_FFMPEG_MAX_THREADS); // ask for the number of available cores for multithreading
                // The thread type is FFmpeg's default (frame and slice threads, as the codec supports them).
                // Slice threads only were asked here for codecs that support them, but the test was always
                // false (codecCtx->codec is set by avcodec_open2()), and frame threads have been used.
                // avstream->codec->thread_count = video_decoding_threads(); // bino's strategy (disabled)
            }
            // Set CODEC_FLAG_EMU_EDGE in the same situations in which ffplay sets it.
            // I don't know what exactly this does, but it is necessary to fix the problem
            // described in this thread: http://lists.nongnu.org/archive/html/bino-list/2012-02/msg00039.html
#ifdef CODEC_FLAG_EMU_EDGE // removed from ffmpeg 4.0
            int lowres = 0;
#ifdef FF_API_LOWRES
            lowres = codecCtx->lowres;
#endif
            if (lowres || (videoCodec && (videoCodec->capabilities & CODEC_CAP_DR1))) {
                codecCtx->flags |= CODEC_FLAG_EMU_EDGE;
            }
#endif
        }

        // skip if the codec can't be open
        if (avcodec_open2(codecCtx, videoCodec, nullptr) < 0) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" failed to open, skipping..." << std::endl;
#endif
            avcodec_free_context(&codecCtx);
            continue;
        }

#if TRACE_FILE_OPEN
        std::cout << "Video decoder \"" << videoCodec->name << "\" opened ok, getting stream properties:" << std::endl;
#endif

        if (!_streams.empty()) {
            // Assume that if this stream's properties don't match those of the zeroth stream then it doesn't
            // correspond to an alternative view.
            // This may turn out to be either too stringent or pointlessly lax, we'll have to see what users
            // make of it. The way to handle this properly is to provide a knob allowing the user to map views
            // to streams.
            if (!CheckStreamPropertiesMatch(_streams[0]->_avstream, avstream)) {
#if TRACE_FILE_OPEN
                std::cout << "Stream properties do not match those of first video stream, ignoring this stream." << std::endl;
#endif
                avcodec_free_context(&codecCtx);
                continue;
            }
        }
        Stream* stream = new Stream();
        stream->_idx = i;
        stream->_avstream = avstream;
        stream->_codecContext = codecCtx;
        stream->_videoCodec = videoCodec;
        stream->_avFrame = av_frame_alloc(); // avcodec_alloc_frame();
        stream->_avIntermediateFrame = av_frame_alloc();
        stream->_avHeldFrame = av_frame_alloc();
        stream->_avNextFrame = av_frame_alloc();
        stream->_pendingPacket = av_packet_alloc();

        {
            // In |engine| the output bit depth was hard coded to 16-bits.
            // Now it will use the bit depth reported by the decoder so
            // that if a decoder outputs 10-bits then |engine| will convert
            // this correctly. This means that the following change is
            // requireded for FFmpeg decoders. Currently |_bitDepth| is used
            // internally so this change has no side effects.
            // [openfx-io note] when using insternal ffmpeg 8bits->16 bits conversion,
            // (255 = 100%) becomes (65280 =99.6%)
            stream->_bitDepth = codecCtx->bits_per_raw_sample; // disabled in Nuke's reader
            // stream->_bitDepth = 16; // enabled in Nuke's reader

            const AVPixFmtDescriptor* avPixFmtDescriptor = av_pix_fmt_desc_get(stream->_codecContext->pix_fmt);
            if (avPixFmtDescriptor == nullptr) {
                throw std::runtime_error("av_pix_fmt_desc_get() failed");
            }
            // Sanity check the number of components.
            // Only 3 or 4 components are supported by |engine|, that is
            // Nuke/NukeStudio will only accept 3 or 4 component data.
            // For a monochrome image (single channel) promote to 3
            // channels. This is in keeping with all the assumptions
            // throughout the code that if it is not 4 channels data
            // then it must be three channel data. This ensures that
            // all the buffer size calculations are correct.
            stream->_numberOfComponents = avPixFmtDescriptor->nb_components;
            if (3 > stream->_numberOfComponents) {
                stream->_numberOfComponents = 3;
            }
            // AVCodecContext::bits_pre_raw_sample may not be set, if
            // it's not set, try with the following utility function.
            if (0 == stream->_bitDepth) {
                stream->_bitDepth = av_get_bits_per_pixel(avPixFmtDescriptor) / stream->_numberOfComponents;
            }
        }

        if (stream->_bitDepth > 8) {
            // TODO: This will sometimes be sub-optimal for the timeline, which can deal directly with 3 x 10 bit pixel formats.
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE; // 16-bit.
        } else {
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24; // 8-bit
        }
#if TRACE_FILE_OPEN
        std::cout << "      Timebase=" << avstream->time_base.num << "/" << avstream->time_base.den << " s/tick" << std::endl;
        std::cout << "      Duration=" << avstream->duration << " ticks, " << double(avstream->duration) * double(avstream->time_base.num) / double(avstream->time_base.den) << " s" << std::endl;
        std::cout << "      BitDepth=" << stream->_bitDepth << std::endl;
        std::cout << "      NumberOfComponents=" << stream->_numberOfComponents << std::endl;
#endif

        // If FPS is specified, record it.
        // Otherwise assume 1 fps (default value).
        if ((avstream->r_frame_rate.num != 0) && (avstream->r_frame_rate.den != 0)) {
            stream->_fpsNum = avstream->r_frame_rate.num;
            stream->_fpsDen = avstream->r_frame_rate.den;
#if TRACE_FILE_OPEN
            std::cout << "      Framerate=" << stream->_fpsNum << "/" << stream->_fpsDen << ", " << double(stream->_fpsNum) / double(stream->_fpsDen) << " fps" << std::endl;
#endif
        }
#if TRACE_FILE_OPEN
        else {
            std::cout << "      Framerate unspecified, assuming 1 fps" << std::endl;
        }
#endif

        stream->_width = codecCtx->width;
        stream->_height = codecCtx->height;
#if TRACE_FILE_OPEN
        std::cout << "      Image size=" << stream->_width << "x" << stream->_height << std::endl;
#endif

        // set aspect ratio
        stream->_aspect = Stream::GetStreamAspectRatio(stream);

        // set stream start time and numbers of frames
        stream->_startPTS = getStreamStartTime(*stream);
        stream->_frames = getStreamFrames(*stream);

        // save the stream
        _streams.push_back(stream);
    }
    // A truncated file whose index (e.g. MOV/MP4 with the index at the start) has samples beyond the end of the
    // file: the frames from the first of them on fail, whatever the render order (otherwise, it is only known
    // when the partial last packet is read, see demuxAndDecode()).
    if (_context && _context->pb) {
        const int64_t size = avio_size(_context->pb);
        for (size_t s = 0; (size > 0) && (s < _streams.size()); ++s) {
            Stream* st = _streams[s];
            const int entries = IndexEntriesCount(st->_avstream);
            int64_t truncatedTs = AV_NOPTS_VALUE;
            for (int i = 0; i < entries; ++i) {
                const AVIndexEntry* e = IndexEntry(st->_avstream, i);
                if (e && (e->pos >= 0) && (e->size > 0) && (e->pos + e->size > size) && (e->timestamp != int64_t(AV_NOPTS_VALUE)) &&
                    ((truncatedTs == int64_t(AV_NOPTS_VALUE)) || (e->timestamp < truncatedTs))) {
                    truncatedTs = e->timestamp;
                }
            }
            if (truncatedTs != int64_t(AV_NOPTS_VALUE)) {
                st->_truncatedFrame = (std::max)(0, st->ptsToFrame(truncatedTs)); // a DTS: at or before the frame
            }
        }
    }
    _bytesReadAtOpen = (_context && _context->pb) ? _context->pb->bytes_read : 0;
    if (_streams.empty()) {
        setError(unsupported_codec ? "unsupported codec or format/codec combination" : "unable to find video stream");
        _selectedStream = nullptr;
    } else {
#pragma message WARN("should we build a separate FFmpegfile for each view? see also FFmpegFileManager")
        const int viewIndex = 0; // TODO: pass as parameter?
        assert(viewIndex >= 0 && "Negative view index specified.");

        if (static_cast<size_t>(viewIndex) < _streams.size()) {
            _selectedStream = _streams[viewIndex];
        } else {
            _selectedStream = _streams[0];
        }
    }
}

// destructor
FFmpegFile::~FFmpegFile()
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_lock);
#endif

    for (unsigned int i = 0; i < _streams.size(); ++i) {
        delete _streams[i];
    }
    _streams.clear();

    if (_context) {
        avformat_close_input(&_context);
        av_free(_context);
    }
    _filename.clear();
    _errorMsg.clear();
}

void
FFmpegFile::setSelectedStream(int streamIndex)
{
    if ((streamIndex >= 0) && (streamIndex < static_cast<int>(_streams.size()))) {
        _selectedStream = _streams[streamIndex];
    } else {
        assert(false && "setSelectedStream: Invalid streamIndex");
        _selectedStream = !_streams.empty() ? _streams[0] : nullptr;
    }
}

std::string
FFmpegFile::getColorspaceMetadata() const
{
    // the metadata of the format (WriteFFmpeg writes it there): its keys are not case sensitive (av_dict_get()
    // without AV_DICT_MATCH_CASE), e.g. Matroska tags are upper case
    const AVDictionaryEntry* t = (_context && _context->metadata) ? av_dict_get(_context->metadata, kMetaKeyColorspace, nullptr, 0) : nullptr;

    return (t && t->value) ? std::string(t->value) : std::string();
}

const char*
FFmpegFile::getColorspace() const
{
#ifdef OFX_IO_MT_FFMPEG
    // decode() may open the decoder again (flushDecoder()), which replaces the codec context
    AutoMutex guard(_lock);
#endif

    // The preferred colorspace is figured out from a number of sources - initially we look for a number
    // of different metadata sources that may be present in the file. If these fail we then fall back
    // to using the codec's underlying storage mechanism - if RGB we default to gamma 1.8, if YCbCr we
    // default to gamma 2.2 (note prores special case). Note we also ignore the NCLC atom for reading
    // purposes, as in practise it tends to be incorrect.

    // First look for the meta keys that (recent) Nukes would've written, or special cases in Arri meta.
    // Doubles up searching for lower case keys as the ffmpeg searches are case sensitive, and the keys
    // have been seen to be lower cased (particularly in old Arri movs).
    if (_context && _context->metadata) {
        AVDictionaryEntry* t;

        t = av_dict_get(_context->metadata, "uk.co.thefoundry.Colorspace", nullptr, AV_DICT_IGNORE_SUFFIX);
        if (!t) {
            t = av_dict_get(_context->metadata, "uk.co.thefoundry.colorspace", nullptr, AV_DICT_IGNORE_SUFFIX);
        }
        if (t) {
#if 0
            //Validate t->value against root list, to make sure it's been written with a LUT
            //we have a matching conversion for.
            bool found = false;
            int i     = 0;
            while (!found && LUT::builtin_names[i] != nullptr) {
                found = !strcasecmp(t->value, LUT::builtin_names[i++]);
            }
#else
            bool found = true;
#endif
            if (found) {
                return t->value;
            }
        }

        t = av_dict_get(_context->metadata, "com.arri.camera.ColorGammaSxS", nullptr, AV_DICT_IGNORE_SUFFIX);
        if (!t) {
            t = av_dict_get(_context->metadata, "com.arri.camera.colorgammasxs", nullptr, AV_DICT_IGNORE_SUFFIX);
        }
        if (t && !strncasecmp(t->value, "LOG-C", 5)) {
            return "AlexaV3LogC";
        }
        if (t && !strncasecmp(t->value, "REC-709", 7)) {
            return "rec709";
        }
    }

    // Special case for prores - the util YUV will report RGB, due to pixel format support, but for
    // compatibility and consistency with official quicktime, we need to be using 2.2 for 422 material
    // and 1.8 for 4444. Protected to deal with ffmpeg vagaries.
    assert((_streams.empty() || _selectedStream) && "_streams not empty but null _selectedStream");
    if (!_streams.empty() && _selectedStream->_codecContext && _selectedStream->_codecContext->codec_id) {
        if (_selectedStream->_codecContext->codec_id == AV_CODEC_ID_PRORES) {
            if ((_streams[0]->_codecContext->codec_tag == MKTAG('a', 'p', '4', 'h')) || (_streams[0]->_codecContext->codec_tag == MKTAG('a', 'p', '4', 'x'))) {
                return "Gamma1.8";
            } else {
                return "Gamma2.2";
            }
        }
    }
    if (!_streams.empty() && _selectedStream->_codecContext) {
        if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_BT709 || _selectedStream->_codecContext->color_trc == AVCOL_TRC_SMPTE240M) {
            return "rec709";
        } else if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_GAMMA22) {
            return "Gamma2.2";
        } else if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_SMPTE170M) {
            return "rec601";
        } else if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_SMPTE2084) {
            return "st2084";
        } else if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_IEC61966_2_1) {
            return "sRGB";
        } else if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_LINEAR) {
            return "linear";
        }
    }
    return (!_streams.empty() && _streams[0]->isYUV()) ? "Gamma2.2" : "Gamma1.8"; // (FFmpegFile::isYUV() takes the lock)
} // FFmpegFile::getColorspace

void
FFmpegFile::setError(const char* msg,
                     const char* prefix)
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_invalidStateLock);
#endif
    if (prefix) {
        _errorMsg = prefix;
        _errorMsg += msg;
#if TRACE_DECODE_PROCESS
        std::cout << "!!ERROR: " << prefix << msg << std::endl;
#endif
    } else {
        _errorMsg = msg;
#if TRACE_DECODE_PROCESS
        std::cout << "!!ERROR: " << msg << std::endl;
#endif
    }
    _invalidState = true;
}

// Clear the decode error (the error message of the last frame), unless the file is invalid
void
FFmpegFile::clearDecodeError()
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_invalidStateLock);
#endif
    if (!_invalidState) {
        _errorMsg.clear();
    }
}

// A decode error: the error message of a frame, which does not make the file invalid (other frames may decode)
void
FFmpegFile::setDecodeError(const char* msg,
                           const char* prefix)
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_invalidStateLock);
#endif
    if (_invalidState) {
        return; // keep the error that made the file invalid
    }
    _errorMsg = prefix ? (string(prefix) + msg) : string(msg);
#if TRACE_DECODE_PROCESS
    std::cout << "!!DECODE ERROR: " << _errorMsg << std::endl;
#endif
}

void
FFmpegFile::setDecodeError(const int error,
                           const char* prefix)
{
    char errorBuf[1024];

    av_strerror(error, errorBuf, sizeof(errorBuf));
    setDecodeError(errorBuf, prefix);
}

string
FFmpegFile::getError() const
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_lock);
#endif

    return _errorMsg;
}

// return true if the reader can't decode the frame
bool
FFmpegFile::isInvalid() const
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_invalidStateLock);
#endif

    return _invalidState;
}

// Whether the codec only has keyframes: its descriptor has AV_CODEC_PROP_INTRA_ONLY ("Codec uses only intra
// compression", libavcodec/codec_desc.h:68-72 in FFmpeg 4.4, 8.0 and 9.0), e.g. ProRes, DNxHD, MJPEG, rawvideo, v210,
// r210, r10k, HuffYUV, FFVHuff, JPEG-LS, JPEG 2000, Hap, EXR (libavcodec/codec_desc.c), or it is in the list
// below, which overrides the descriptor: PNG has no AV_CODEC_PROP_INTRA_ONLY (codec_desc.c:470 in FFmpeg 8.0 and 9.0,
// 463 in 4.4; animated PNG is AV_CODEC_ID_APNG), but each PNG packet is a complete image. The other codecs of the list
// have AV_CODEC_PROP_INTRA_ONLY in FFmpeg 4.4 to 9.0: they are kept as they were before the descriptor was used.
// AVCodecContext::gop_size does not tell: it is "unused" for decoding (libavcodec/avcodec.h:1016-1021 in FFmpeg 9.0,
// 1000-1005 in 8.0, 726-731 in 4.4), and avcodec_alloc_context3() sets it to 12, its default (the "g" option,
// libavcodec/options_table.h:96 in 9.0, 95 in 8.0, 88 in 4.4, set for video decoders too by init_context_defaults(),
// libavcodec/options.c:90-110 in 8.0 and 9.0, 107-126 in 4.4), so the former gop_size == 0 test was never true when
// decoding.
// Not CineForm (AV_CODEC_ID_CFHD), whose descriptor has no AV_CODEC_PROP_INTRA_ONLY (codec_desc.c:1530 in 9.0): it may
// have inter frames.
static bool
IsIntraOnly(const AVCodecContext* ctx)
{
    const AVCodecID id = ctx->codec_id;
    if ((id == AV_CODEC_ID_PRORES) || (id == AV_CODEC_ID_DNXHD) || (id == AV_CODEC_ID_MJPEG) || (id == AV_CODEC_ID_MJPEGB) ||
        (id == AV_CODEC_ID_PNG) || (id == AV_CODEC_ID_DPX) || (id == AV_CODEC_ID_TARGA) || (id == AV_CODEC_ID_TIFF)) {
        return true;
    }
    const AVCodecDescriptor* desc = avcodec_descriptor_get(id);

    return desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
}

// decode a single frame into the buffer thread safe
bool
FFmpegFile::decode(const ImageEffect* /*plugin*/,
                   int frame,
                   bool loadNearest,
                   unsigned char* buffer,
                   string* errorMsg)
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_lock);
#endif

    if (_streams.empty() || !_selectedStream) {
        if (errorMsg) {
            *errorMsg = _errorMsg.empty() ? string("FFmpeg Reader: no video stream") : _errorMsg;
        }
        return false;
    }
    Stream* stream = _selectedStream;

    // The error message of the previous frame, if any (unless the file is invalid)
    clearDecodeError();

    // Translate from the 1-based frames expected to 0-based frame offsets for use in the rest of this code.
    frame = frame - 1;

    // Check if the requested frame index is in range (AVStream::start_time is unknown in elementary streams)
    const int firstFrame = (stream->_avstream->start_time != int64_t(AV_NOPTS_VALUE)) ? stream->ptsToFrame(stream->_avstream->start_time) : 0;
    if (frame < firstFrame) {
        if (loadNearest) {
            frame = firstFrame;
        } else {
            throw MissingFrameException();
        }
    } else if (frame >= stream->_frames) {
        if (loadNearest) {
            frame = (int)stream->_frames - 1;
        } else {
            throw MissingFrameException();
        }
    }

#if TRACE_DECODE_PROCESS
    std::cout << "FFmpeg Reader=" << this << "::decode(): frame=" << frame << /*", _viewIndex = " << _viewIndex <<*/ ", stream->_idx=" << stream->_idx << std::endl;
#endif

    bool hasPicture = false;
    AVFrame* avFrameOut = stream->_avFrame;

    // An intra-only codec (see IsIntraOnly()): each frame is decoded from its own packet.
    bool isIntraOnly = IsIntraOnly(stream->_codecContext);

    // A stream whose frames decoded so far without an error are all keyframes (e.g. all-intra H.264, -g 1) is decoded
    // as an intra-only one once damage was found: each frame is then decoded from the previous keyframes (the back-off below,
    // bounded), instead of restarting the decoder at each keyframe and decoding from the start of the file.
    if (stream->allKeyframesAfterDamage()) {
        isIntraOnly = true;
    }

    // The decoder outputs frames in presentation order (libavcodec decoders reorder frames
    // internally, e.g. B-frames in H.264, HEVC and MPEG-4), each with its timestamp: the frame
    // displayed at the requested time is decided from the decoded frames (see demuxAndDecode()).
    // The next frame out of the decoder is not always the next frame index: there may be a
    // timestamp gap in a variable frame rate movie.

    // Only seek and reset for non-sequential frames as this can be very costly: the frame may be
    // the one held since the previous request (e.g. in a timestamp gap of a variable frame rate
    // movie), or the next one out of the decoder.
    if (!stream->isSequential(frame) || stream->_restartSingleThread) {
        seekToFrame(stream->frameToPts(frame), AVSEEK_FLAG_BACKWARD);
    }

    // Setup the output frame struct with the buffer passed in
    avFrameOut->width = stream->_width;
    avFrameOut->height = stream->_height;
    avFrameOut->format = stream->_outputPixelFormat;

    int res = 0;

    if ((res = av_image_fill_linesizes(avFrameOut->linesize, stream->_outputPixelFormat, stream->_width)) < 0) {
        setDecodeError(res, "FFmpeg Reader Failed to fill image linesizes: ");
        if (errorMsg) {
            *errorMsg = _errorMsg;
        }
        return false;
    }

    res = av_image_fill_pointers(
        avFrameOut->data,
        stream->_outputPixelFormat,
        stream->_height,
        buffer,
        avFrameOut->linesize);

    if (res < 0) {
        setDecodeError(res, "FFmpeg Reader Failed to fill image pointers: ");
        if (errorMsg) {
            *errorMsg = _errorMsg;
        }
        return false;
    }

    // Frames known to fail, without reading the file again: from the truncated packet of a truncated file
    // on, and the frames of a persistently damaged range, found by a previous request.
    if ((stream->_truncatedFrame >= 0) && (frame >= stream->_truncatedFrame)) {
        setDecodeError("the file is truncated", "FFmpeg Reader: ");
        if (errorMsg) {
            *errorMsg = _errorMsg;
        }
        return false;
    }
    if (stream->inDamagedRange(frame)) {
        setDecodeError(stream->_damagedRangeError, "FFmpeg Reader Failed to decode packet (damaged range): ");
        if (errorMsg) {
            *errorMsg = _errorMsg;
        }
        return false;
    }

    bool retriedSeek = false;
    bool retriedDecode = false;
    bool fromStartFailed = false; // the attempt from the start of the file gave no frame up to the requested one
    int requestDamageError = 0; // a damaged packet was dropped by an attempt of this request
    stream->_invalidDataSkipped = 0;
    stream->_stoppedOnDamagedPacket = false;
    stream->_tooManyDroppedPackets = false;
    stream->_lastDamagedFrame = -1;
    stream->_requestDroppedPackets = 0;
    stream->_noTimestamps = false;
    stream->_sawFrameWithoutTimestamp = false;

    while (!retriedDecode) {

        retriedDecode = retriedSeek ? true : false;

        // A frame decoded after a seek may be corrupt (e.g. a reference frame is missing): it is
        // only used for a timestamp gap when decoding from the start (Stream::_decodingFromStart),
        // or for intra-only codecs, see demuxAndDecode().
        hasPicture = demuxAndDecode(avFrameOut, frame, /*acceptCorruptHeldFrame=*/isIntraOnly);
        if (stream->_invalidDataSkipped) {
            requestDamageError = stream->_invalidDataSkipped;
        }
        if (!hasPicture && stream->_restartSingleThread) {
            // damage found with frame threads: decode again with 1 thread (see skipInvalidData in demuxAndDecode()),
            // in the same way (from the start of the file if this attempt was, e.g. after a seek that landed after
            // the requested frame)
            clearDecodeError();
            const bool fromStart = retriedSeek && stream->_decodingFromStart;
            if (fromStart ? seekToFrame(0, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD) : seekToFrame(stream->frameToPts(frame), AVSEEK_FLAG_BACKWARD)) {
                stream->_decodingFromStart = fromStart;
                retriedDecode = false; // (this attempt again)
                continue;
            }
            break;
        }
        if (!hasPicture && retriedSeek && stream->_decodingFromStart && !stream->_tooManyDroppedPackets &&
            ((stream->_firstOutputFrame < 0) || (stream->_firstOutputFrame > frame)) &&
            (stream->_reachedEnd || (stream->_firstFrameAfter > frame))) {
            fromStartFailed = true;
        }

        // A last ditch effot to get a frame out for non-intra codecs.
        // This will perform a seek to the start of the file. which is
        // the only reliable way to get frame accurate seeking in a
        // stream with B-frames.
        if (!hasPicture && !isIntraOnly && !retriedSeek && !stream->_tooManyDroppedPackets && !stream->_noTimestamps) {
            retriedSeek = true;
            if (seekToFrame(0, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD)) {
                // Until the next seek (which resets it), including the next sequential requests:
                // otherwise each of them would refuse the same corrupt frame and decode from the
                // start again.
                stream->_decodingFromStart = true;
            }
        } else {
            break;
        }
    }

    // Intra-only codecs: if decoding started after the requested frame and no frame is held, the
    // packet of the requested frame is damaged (it was dropped), or missing (a truncated file, where
    // the seek reads no packet before the end): decode from earlier frames, each a keyframe, 1, 2, 4,
    // ... frames before, to hold the last good one, as when decoding sequentially (whatever the
    // render order). Not if the damage is persistent (see demuxAndDecode).
    // An attempt that stopped on a damaged packet (several consecutive damaged frames) goes on further back;
    // an attempt that failed with another error stops.
    const bool firstAttemptFailedHard = !hasPicture && !stream->_stoppedOnDamagedPacket && !stream->_invalidDataSkipped && !_errorMsg.empty();
    int lastDamageError = stream->_invalidDataSkipped;
    // For intra-only codecs, an attempt that stopped on a damaged packet without decoding any frame shows that
    // every frame from its start to the requested one is damaged (with frame threads, the stop waits for the
    // decoder delay, see skipInvalidData): the lowest such start.
    int damagedFrom = (isIntraOnly && !hasPicture && stream->_stoppedOnDamagedPacket) ? frame : -1;
    // Not into a known damaged range before the requested frame (it cannot give a frame to hold), and with a
    // bound on the packets dropped by all the attempts of this request.
    const int backLimit = stream->damagedRangeEndBefore(frame) + 1;
    for (int back = 1; !hasPicture && isIntraOnly && !firstAttemptFailedHard && !stream->_tooManyDroppedPackets && (frame > backLimit) && (stream->_requestDroppedPackets <= 2 * kMaxDroppedPacketsPerDecode) && (back < (1 << 30)); back *= 2) {
        const int from = (std::max)(backLimit, frame - back);
        clearDecodeError();
        stream->_stoppedOnDamagedPacket = false;
        stream->_invalidDataSkipped = 0;
        if (!seekToFrame(stream->frameToPts(from), AVSEEK_FLAG_BACKWARD)) {
            break;
        }
        hasPicture = demuxAndDecode(avFrameOut, frame, /*acceptCorruptHeldFrame=*/isIntraOnly, damagedFrom);
        if (stream->_invalidDataSkipped) {
            lastDamageError = stream->_invalidDataSkipped;
        }
        if (!hasPicture && stream->_stoppedOnDamagedPacket) {
            damagedFrom = from;
        }
        if ((from == backLimit) || (!hasPicture && !_errorMsg.empty() && !stream->_stoppedOnDamagedPacket && !stream->_invalidDataSkipped)) {
            break; // e.g. the truncated packet, which fails (see demuxAndDecode)
        }
    }
    if (!hasPicture && !stream->_invalidDataSkipped) {
        stream->_invalidDataSkipped = lastDamageError;
    }

    // A damaged range: the following requests in it fail without reading it again (until the file is opened
    // again, also if the error was transient, e.g. a lack of memory), in any order. For intra-only codecs, from
    // the lowest start of an attempt that found only damaged packets; otherwise (a persistent damage, after too
    // many damaged packets), from the requested frame; up to the last damaged packet. The ranges are merged
    // when they overlap or touch (e.g. a file damaged from some point on, read sequentially or backward).
    // Errors returned by avcodec_receive_frame() do not extend a range (their packet is unknown): conservative.
    if (!hasPicture) {
        int lo = -1;
        int hi = -1;
        if (damagedFrom >= 0) {
            lo = damagedFrom;
            hi = (std::max)((int)frame, stream->_lastDamagedFrame);
        } else if ((stream->_tooManyDroppedPackets || (stream->_requestDroppedPackets > 2 * kMaxDroppedPacketsPerDecode)) && (stream->_lastDamagedFrame >= frame)) {
            lo = frame;
            hi = stream->_lastDamagedFrame;
            if ((stream->_firstFrameAfter > frame) && (stream->_firstFrameAfter - 1 < hi)) {
                hi = stream->_firstFrameAfter - 1; // a later frame was decoded (e.g. when draining the decoder)
            }
        } else if (fromStartFailed && requestDamageError) {
            // Decoding from the start of the file, with damaged packets, gave no frame before a later one (e.g. a
            // damaged first GOP), or up to the end of the file (e.g. a damaged first keyframe in a long GOP): the
            // frames before it fail in any order.
            lo = 0;
            hi = (stream->_firstFrameAfter > frame) ? (stream->_firstFrameAfter - 1) : (std::numeric_limits<int>::max() - 1);
            stream->_invalidDataSkipped = requestDamageError;
        }
        // After too many damaged packets that followed a held frame (and once the decoder was drained, see
        // demuxAndDecode): every frame after it, up to the last damaged packet (and before a later frame that
        // was decoded), is in a damaged run longer than kMaxDroppedPacketsPerDecode packets, which fails in any
        // order (also for long-GOP codecs: a keyframe in the run was damaged too).
        if (stream->_tooManyDroppedPackets && Stream::hasFrame(stream->_avHeldFrame) && (stream->_lastDamagedFrame >= frame)) {
            const int heldFrame = stream->ptsToFrame(stream->_avHeldFrame->pts);
            if ((heldFrame < frame) && ((lo < 0) || (heldFrame + 1 < lo))) {
                lo = heldFrame + 1;
                hi = (std::max)(hi, stream->_lastDamagedFrame);
                if ((stream->_firstFrameAfter > frame) && (stream->_firstFrameAfter - 1 < hi)) {
                    hi = stream->_firstFrameAfter - 1;
                }
            }
        }
        if (!isIntraOnly && stream->_endTainted && Stream::hasFrame(stream->_avHeldFrame)) {
            // the stream ends in damage after the held frame (see demuxAndDecode())
            const int heldFrame = stream->ptsToFrame(stream->_avHeldFrame->pts);
            if (heldFrame < frame) {
                lo = (lo < 0) ? (heldFrame + 1) : (std::min)(lo, heldFrame + 1);
                hi = std::numeric_limits<int>::max() - 1;
            }
        }
        if (lo >= 0) {
            stream->addDamagedRange(lo, hi);
            stream->_damagedRangeError = stream->_invalidDataSkipped ? stream->_invalidDataSkipped : AVERROR_INVALIDDATA;
        }
    }

    if (!hasPicture) {
        // The decoder position is unknown: seek before the next decode.
        stream->resetHeldFrames();
        if (_errorMsg.empty()) {
            if (stream->_sawFrameWithoutTimestamp) {
                setDecodeError("Video frames have no timestamp", "FFmpeg Reader: ");
            } else if (stream->_invalidDataSkipped) {
                // No frame could be decoded for the requested frame, and damaged packets were dropped.
                setDecodeError(stream->_invalidDataSkipped, "FFmpeg Reader Failed to decode packet: ");
            } else {
                setDecodeError("no frame could be decoded at or before this frame", "FFmpeg Reader: ");
            }
        }
        if (errorMsg) {
            *errorMsg = _errorMsg;
        }
    }

    return hasPicture;
} // FFmpegFile::decode

// A frame decoded as a keyframe, without an error: e.g. an H.264 IDR frame (AV_FRAME_FLAG_KEY, libavcodec/h264_slice.c:
// 1664 in FFmpeg 9.0.2), without decode_error_flags (which the H.264 decoder sets on a frame whose slices had errors or
// were concealed, libavcodec/h264dec.c:752-764) or AV_FRAME_FLAG_CORRUPT (only set with AV_CODEC_FLAG_OUTPUT_CORRUPT or
// AV_CODEC_FLAG2_SHOW_ALL, h264_slice.c:1379-1384)
static bool
IsCleanKeyframe(const AVFrame* f)
{
#if defined(AV_FRAME_FLAG_KEY)
    const bool key = (f->flags & AV_FRAME_FLAG_KEY) != 0;
#else
    const bool key = f->key_frame != 0;
#endif
    return key && !(f->flags & AV_FRAME_FLAG_CORRUPT) && !f->decode_error_flags;
}

// After damage, the decoder is opened again instead of flushed: avcodec_flush_buffers() keeps the parameter sets
// that the decoder parsed (in FFmpeg 9.0.2, h264_decode_flush(), libavcodec/h264dec.c:476-497, only resets the
// pictures and the DPB; the H.264 SPS and PPS are only freed when the decoder is closed, h264dec.c:372, and parsed
// from the extradata when it is opened, h264dec.c:402). A damaged packet parsed as a parameter set with the id of a
// real one (e.g. "sps_id out of range", "non-existing PPS referenced" in the log) then makes the good packets fail,
// also after a seek: e.g. every packet after the first keyframe of a damaged Matroska file, whose parameter sets
// are only in the extradata. Opening the decoder again takes them from the extradata again. The ffmpeg CLI never
// needs this: it decodes the file once, sequentially, logs a decoding error and goes on
// (fftools/ffmpeg_dec.c:719-736 and 755-772, packet_decode()), and it only flushes the decoder at the end of a
// stream (ffmpeg_dec.c:976); ffplay flushes it after a seek, when the packet serial changes (fftools/ffplay.c:632-637).
// After damage in a long-GOP stream decoded with frame threads, it is opened again with 1 thread (see
// demuxAndDecode()).
void
FFmpegFile::flushDecoder(Stream* stream)
{
    if ((stream->_damagedSinceOpen || stream->_restartSingleThread) && stream->_videoCodec) {
        stream->_restartSingleThread = false;
        AVCodecContext* ctx = avcodec_alloc_context3(nullptr);
        if (ctx && (avcodec_parameters_to_context(ctx, stream->_avstream->codecpar) >= 0)) {
            ctx->thread_count = stream->_singleThreaded ? 1 : stream->_codecContext->thread_count;
            ctx->pkt_timebase = stream->_codecContext->pkt_timebase;
            if (avcodec_open2(ctx, stream->_videoCodec, nullptr) >= 0) {
                avcodec_free_context(&stream->_codecContext);
                stream->_codecContext = ctx;
                stream->_damagedSinceOpen = false;
                return;
            }
        }
        avcodec_free_context(&ctx);
    }
    avcodec_flush_buffers(stream->_codecContext);
}

bool
FFmpegFile::seekToFrame(int64_t frame, int seekFlags)
{
    Stream* stream = _selectedStream;

    stream->resetHeldFrames(); // also resets _decodingFromStart
    flushDecoder(stream);
    stream->clearTaint();
    ++_seekCount;
    int res = 0;

    // The start of the file (seekToFrame(0, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD)): timestamp 0 is at or before
    // the first packet, unless the stream starts at a negative timestamp that is not an edit-list pre-roll (see
    // NegativeStartIsPreroll()), e.g. an MPEG-TS whose timestamps wrap: timestamp 0 is then after the wrap, and the
    // MPEG-TS and MPEG-PS demuxers, which have no read_seek but a read_timestamp (mpegts.c:3878, mpeg.c:712 in
    // FFmpeg 9.0), seek to it by a binary search on the DTS (seek_frame_internal(), libavformat/seek.c:597-639,
    // ff_seek_frame_binary(), with the timestamps unwrapped as the packets, seek.c:281-288). Seek to the first DTS
    // of the stream instead (Stream::_startDTS, or its first PTS without B-frames), which is at or before its
    // first packet.
    if ((seekFlags & AVSEEK_FLAG_FRAME) && (frame == 0) && (stream->_startPTS < 0) && !NegativeStartIsPreroll(_context, stream->_avstream)) {
        frame = (std::min)(stream->_startPTS, stream->_startDTS);
        seekFlags = AVSEEK_FLAG_BACKWARD;
    }

    // Once damage was found in the file (long-GOP codecs), a seek may land on a spurious keyframe: the Matroska
    // demuxer adds an index entry for each block whose header has the keyframe flag (libavformat/matroskadec.c:
    // 4319-4320 and 4329-4331 in FFmpeg 9.0.2), also in a damaged block. Skip the index entries of the keyframes
    // that did not decode cleanly (Stream::_suspectKeyPts), and decode as after a restart at a keyframe: the
    // frames are only used once the first packet read decodes as a clean keyframe (Stream::verifyRestart()).
    if (stream->_damageSeen && !IsIntraOnly(stream->_codecContext) && !stream->allKeyframesAfterDamage()) {
        if (seekFlags == AVSEEK_FLAG_BACKWARD) {
            int i = av_index_search_timestamp(stream->_avstream, frame, AVSEEK_FLAG_BACKWARD);
            while (i > 0) {
                const AVIndexEntry* e = IndexEntry(stream->_avstream, i);
                if (!e || !stream->isSuspectKeyPts(e->timestamp)) {
                    break;
                }
                --i;
            }
            const AVIndexEntry* e = (i >= 0) ? IndexEntry(stream->_avstream, i) : nullptr;
            if (e && (e->timestamp < frame)) {
                frame = e->timestamp;
            }
        }
        stream->_tainted = true;
        if (stream->positionTaint()) {
            stream->_awaitRestartFrame = true;
        } else {
            stream->_verifyNextPacket = true;
        }
    }
    if ((res = av_seek_frame(_context, stream->_idx, frame, seekFlags)) < 0) {
        setDecodeError(res, "FFmpeg Reader Failed to seek frame: ");
        return false;
    }

    return true;
}

// avcodec_send_packet() and avcodec_receive_frame() replace avcodec_decode_video2(), see
// https://github.com/FFmpeg/FFmpeg/blob/9e30859cb60b915f237581e3ce91b0d31592edc0/libavcodec/decode.c#L748
// Doc for the new AVCodec API: https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/

bool
FFmpegFile::demuxAndDecode(AVFrame* avFrameOut, int64_t frame, bool acceptCorruptHeldFrame, int damagedFrom)
{
    Stream* stream = _selectedStream;
    MyAVPacket avPacket;

    av_frame_unref(stream->_avIntermediateFrame);

    int res = 0;
    bool hasPicture = false;
    AVFrame* avFrameDecodeDst = stream->_avIntermediateFrame;

    AVFrame* held = stream->_avHeldFrame;
    AVFrame* next = stream->_avNextFrame;

    // The frame displayed at the requested frame time is the last decoded frame whose PTS is at
    // or before it. In a constant frame rate movie, that is the frame with that PTS. In a variable
    // frame rate movie, the requested frame may fall into a timestamp gap between two frames: the
    // previous frame is then displayed until the next one, which is only known once the next frame
    // is decoded. The decoder outputs frames in presentation order, even with B-frames, so this is
    // decided from the decoded frames: the packets are in decoding order, and their durations may be
    // the nominal frame duration (Matroska) or the DTS difference (MP4/MOV), which do not give the gaps.

    // Use the frames kept from the previous requests (since the last seek) if possible.
    if (Stream::hasFrame(next) && (stream->ptsToFrame(next->pts) <= frame)) {
        av_frame_unref(held);
        av_frame_move_ref(held, next);
    }
    // A held frame that is displayed for a later frame (in a timestamp gap) must not be corrupt,
    // unless it cannot be better (when decoding from the start, or for intra-only codecs): a frame
    // decoded after a seek may lack a reference frame, so decode() will then retry from the start.
    auto canHoldFrame = [acceptCorruptHeldFrame, stream](const AVFrame* f) -> bool {
        return acceptCorruptHeldFrame || stream->_decodingFromStart || !((f->flags & AV_FRAME_FLAG_CORRUPT) || f->decode_error_flags);
    };

    if (Stream::hasFrame(held)) {
        int64_t heldFrameIdx = stream->ptsToFrame(held->pts);
        // decode() seeks if the requested frame is before the held frame (Stream::isSequential()).
        assert(heldFrameIdx <= frame);
        if (heldFrameIdx > frame) {
            // Should not happen, but the held frames would give a wrong frame: seek, which resets them.
            seekToFrame(stream->frameToPts((int)frame), AVSEEK_FLAG_BACKWARD);
        } else if (heldFrameIdx == frame) {
            return imageConvert(held, avFrameOut);
        } else if (Stream::hasFrame(next) || stream->_heldFrameIsLast) {
            // The requested frame is in a gap after the held frame.
            if (!canHoldFrame(held)) {
                return false;
            }
            return imageConvert(held, avFrameOut);
        }
    }

    bool draining = false; // the decoder is drained (end of the stream)

    // Process a frame output by the decoder. Returns true if it gives the requested frame (which is
    // then converted into avFrameOut), or if decoding should stop without a picture (hasPicture is
    // then false, and decode() retries from the start).
    const int kMaxDroppedPackets = kMaxDroppedPacketsPerDecode;
    int droppedPackets = 0; // consecutive dropped packets, without a decoded frame
    stream->_firstOutputFrame = -1;
    stream->_firstFrameAfter = -1;
    stream->_reachedEnd = false;
    stream->_endTainted = false;
    bool decodedAny = false; // a frame was decoded since the start of this call
    // Intra-only codecs: this call dropped damaged packets, and neither decoded nor held any frame up to the
    // requested one (a later frame came, or the end of the file, or too many damaged packets): every frame from
    // where it started to the requested one is damaged (decode() then knows the damaged range).
    auto markOnlyDamaged = [&]() {
        if (acceptCorruptHeldFrame && stream->_invalidDataSkipped && !Stream::hasFrame(held)) {
            stream->_stoppedOnDamagedPacket = true;
            stream->_lastDamagedFrame = (std::max)(stream->_lastDamagedFrame, (int)frame);
        }
    };
    int framesWithoutTimestamp = 0;
    auto processDecodedFrame = [&](AVFrame* decodedFrame) -> bool {
        // Fallback to using the DTS from the AVPacket that triggered returning this frame if no PTS is found
        // Original code, which doesn't work (AVI/m1v):
        // if (decodedFrame->pts == AV_NOPTS_VALUE) {
        //     decodedFrame->pts = decodedFrame->pkt_dts;
        // }
        droppedPackets = 0;
        decodedAny = true;
        // The damaged packets are known by their PTS: a frame whose timestamp is from its DTS is not compared.
        // A frame of a packet sent after damage, before the decoder restarted at a keyframe that decoded cleanly
        // (long-GOP codecs), is not used either: it may reference a damaged frame (Stream::taint()).
        // Without PTS (e.g. AVI, where the demuxer only sets the DTS, libavformat/avidec.c:1554 in FFmpeg 9.0.2,
        // and the PTS of the frames that are not reordered, libavformat/demux.c:1131-1137), the frames cannot be
        // matched to their packets: while tainted, every frame is rejected, until the first frame after a
        // restart of the decoder (which only has the packets from the restart keyframe on) is a clean keyframe.
        const bool positionTainted = !acceptCorruptHeldFrame && stream->_tainted && stream->positionTaint();
        if (positionTainted && stream->_awaitRestartFrame) {
            stream->_awaitRestartFrame = false;
            if (IsCleanKeyframe(decodedFrame)) {
                stream->_tainted = false;
            }
        }
        if ((decodedFrame->pts != AV_NOPTS_VALUE) && (decodedFrame->pts == stream->_restartPts)) {
            stream->verifyRestart(IsCleanKeyframe(decodedFrame));
        }
        // While tainted, a frame that conceals errors (decode_error_flags, AV_FRAME_FLAG_CORRUPT) is not used either,
        // whatever its packet: e.g. a damaged IDR frame that the H.264 decoder decodes without an error, but with
        // FF_DECODE_ERROR_DECODE_SLICES (libavcodec/h264dec.c:752-764 in FFmpeg 9.0.2), output after the
        // errors of the following packets.
        // The frame is placed at its best_effort_timestamp (see below), which libavcodec takes from the PTS, or
        // from the DTS of the packet that returned the frame once the PTS look less reliable than the DTS
        // (guess_correct_pts(), libavcodec/decode.c:296-320 and 694-696 in FFmpeg 9.0.2): the taint is checked at
        // both, and while tainted, a frame whose two timestamps differ is not used (e.g. a frame decoded before
        // the damage, but placed after it at the DTS of a damaged packet).
        const bool concealsDamage = (decodedFrame->flags & AV_FRAME_FLAG_CORRUPT) || decodedFrame->decode_error_flags;
        if (!concealsDamage) {
            // (see decode(): the frames decoded without an error, keyframes or not)
            if (IsCleanKeyframe(decodedFrame)) {
                ++stream->_keyFrames;
            } else {
                ++stream->_nonKeyFrames;
            }
        }
        auto taintedAt = [stream](int64_t t) -> bool {
            return (t != AV_NOPTS_VALUE) && (stream->isDamagedPts(t) || stream->isTaintedPts(t) || stream->isCandidatePts(t));
        };
        const bool damaged = taintedAt(decodedFrame->pts) || taintedAt(decodedFrame->best_effort_timestamp) ||
                             (!acceptCorruptHeldFrame && stream->_tainted &&
                              (concealsDamage || stream->positionTaint() || (decodedFrame->best_effort_timestamp != decodedFrame->pts)));
        decodedFrame->pts = decodedFrame->best_effort_timestamp;
        if (decodedFrame->pts == AV_NOPTS_VALUE) {
            // A frame without a PTS, output after a packet without a DTS, e.g. a frame output when
            // the decoder is drained, in a container that has no PTS (AVI with B-frames): it is
            // the frame after the previous decoded frame.
            decodedFrame->pts = stream->_nextDecodedPts;
        }
        if (decodedFrame->pts == AV_NOPTS_VALUE) {
            // A given framme cannot be read from a video with no timestamps.
            // H.264 elementary packets have no timestamp.
            // See https://libav-user.ffmpeg.narkive.com/HhouRdWQ/pts-from-best-effort-timestamp
            // Only an error if decode() gets no picture (Stream::_sawFrameWithoutTimestamp, see the end of
            // decode()): the retry from the start may still give the frame, and the file is never made invalid.
            stream->_sawFrameWithoutTimestamp = true;
            // When the decoder is drained, stop: a frame without a timestamp then cannot be
            // placed (no frame was decoded since the seek), and neither can the frames after it.
            // E.g. in AVI with MPEG-4 B-frames (no PTS), after a seek to the last reference frame,
            // the B-frames after it are skipped, since their other reference frame is missing
            // ("messed up order, maybe after seeking?", libavcodec/mpeg4videodec.c:3169-3173 in
            // FFmpeg 6.1), and FFmpeg 6.1 then outputs the last reference frame twice when drained:
            // first without a timestamp, then again with the timestamp of the last packet, that of
            // the frame before it (libavcodec/h263dec.c:413-431 and :468, which sets
            // skipped_last_frame for any skipped frame; FFmpeg 8 sets it only for a not coded VOP,
            // libavcodec/mpeg4videodec.c:3316 and h263dec.c:442-460 in n8.0). Taking the second
            // one gave the last frame for the one before it. decode() then retries from the start.
            // (fftools/ffmpeg_dec.c:350-353 in n6.1 also extrapolates a missing timestamp from the
            // previous frame, and has none after a seek either.)
            if (draining) {
                return true; // (without setting _noTimestamps, which would prevent the retry from the start)
            }
            // A few such frames are skipped (e.g. the first one after a seek, in a container without PTS, before
            // the next one has a DTS), but a stream without timestamps stops (decoding further, or from the
            // start, would read the rest of the file for each request), without making the file invalid.
            if (++framesWithoutTimestamp > 32) {
                stream->_noTimestamps = true;
                return true; // stop without a picture
            }
            return false;
        }

        if (damaged) {
            // A frame at the time of a damaged packet, which was dropped (see skipInvalidData): not
            // a valid frame, the frame time is a gap.
            return false;
        }

        int64_t decodedFrameIdx = stream->ptsToFrame(decodedFrame->pts);
        stream->_nextDecodedPts = stream->frameToPts((int)decodedFrameIdx + 1);
        if (stream->_firstOutputFrame < 0) {
            stream->_firstOutputFrame = (int)decodedFrameIdx;
        }

        if (decodedFrameIdx == frame) {
            // The requested frame: return it, whatever the frames decoded before it (with broken
            // timestamps, e.g. if best_effort_timestamp switches between the PTS and the DTS, they
            // may not be in presentation order).
            av_frame_unref(held);
            av_frame_move_ref(held, decodedFrame);
            hasPicture = imageConvert(held, avFrameOut);
            return true;
        }

        if (decodedFrameIdx < frame) {
            // This frame, or a later one, is displayed at the requested frame: hold it, unless it is
            // before the held frame, i.e. not in presentation order (broken timestamps).
            if (!Stream::hasFrame(held) || (decodedFrameIdx >= stream->ptsToFrame(held->pts))) {
                av_frame_unref(held);
                av_frame_move_ref(held, decodedFrame);
            }
            return false;
        }

        // This frame is after the requested frame.
        if (stream->_firstFrameAfter < 0) {
            stream->_firstFrameAfter = (int)decodedFrameIdx;
        }
        if (Stream::hasFrame(held)) {
            // The requested frame is in a gap: the held frame is displayed until this one, which is
            // kept for the next requests.
            av_frame_unref(next);
            av_frame_move_ref(next, decodedFrame);
            if (!canHoldFrame(held)) {
                return true; // stop without a picture
            }
            hasPicture = imageConvert(held, avFrameOut);
            return true;
        }

        // Decoding started after the requested frame (the seek went too far, or the requested
        // frame is before the first frame): decoding further cannot give it, since frames come in
        // presentation order. Stop without a picture: decode() retries from the start.
        // Intra-only codecs: if damaged packets were dropped, every frame from the start of this
        // attempt to the requested one is damaged (each good packet gives a frame, in presentation
        // order), as when stopping on a damaged packet (see skipInvalidData).
        markOnlyDamaged();
        return true;
    };

    // A damaged packet (AVERROR_INVALIDDATA) is dropped, and decoding goes on, as in players: the
    // frame time it covered becomes a gap, which shows the previous frame (see processDecodedFrame),
    // and the following frames decode normally once the next keyframe or reference frame arrives
    // (or with errors, concealed by the decoder). With frame threads, the error may be returned by
    // avcodec_receive_frame() instead of avcodec_send_packet(). If no frame can be produced at all
    // (e.g. a damaged first GOP), decode() fails with that error.
    // Other decoder errors on a damaged packet are skipped too: e.g. AVERROR(ENOSYS) from the DNxHD
    // decoder for an unsupported header, or AVERROR(ENOMEM) from libdav1d (AV1) with threads. But a
    // persistent error (e.g. a lack of memory, or a file damaged from some point on) must not make each
    // frame read the rest of the file: after kMaxDroppedPackets packets dropped without any decoded
    // frame, decoding stops with that error (after draining the decoder, which may still hold good frames),
    // and the frames up to the last damaged packet fail without reading again (Stream::_damagedRanges). The bound is per decode(): in a long-GOP stream, the frames
    // after a damaged run of more than kMaxDroppedPackets packets fail until the next keyframe.
    // The PTS of a damaged packet is remembered (without frame threads): the decoder may later output
    // a frame with that PTS anyway (e.g. a damaged first MPEG-2 I-frame, when decoding from the start, gives a picture
    // from a recycled buffer, without any error flag), which must not be used (see
    // processDecodedFrame).
    // A packet that the demuxer flagged as corrupt is decoded (and skipped if the decoder rejects it),
    // unless it is the partial last packet of a truncated file (see below). E.g. the MPEG-TS demuxer flags
    // the packets with continuity counter or transport errors, which are common in broadcast recordings.
    // A frame that the decoder outputs is used, even if it conceals damage (AV_FRAME_FLAG_CORRUPT or
    // decode_error_flags, which concealed H.264 frames have too): e.g. an HEVC frame after a damaged IDR frame
    // and the next packet, or a partially damaged MJPEG frame, whose undecoded area keeps the contents of a
    // recycled buffer. Such frames may then differ between read orders.
    // For intra-only codecs, decoding stops at the first damaged packet after the requested frame, without a
    // held frame: the following ones cannot give it (decode() then decodes from earlier frames). Or at the
    // first one in the frames up to the requested one already known to be damaged (damagedFrom), so that the
    // attempts of decode() from earlier frames do not read these again.
    const int stopFrame = (damagedFrom >= 0) ? damagedFrom : (int)frame + 1;
    auto skipInvalidData = [this, stream, &droppedPackets, &decodedAny, &markOnlyDamaged, frame, stopFrame, acceptCorruptHeldFrame](int r, const AVPacket* pkt) -> bool {
        if ((r >= 0) || (r == AVERROR(EAGAIN)) || (r == AVERROR_EOF)) {
            return false;
        }
        ++stream->_requestDroppedPackets;
        stream->_damageSeen = true;
        stream->_damagedSinceOpen = true;
        if (!acceptCorruptHeldFrame) {
            stream->taint();
            // Long-GOP codecs with frame threads: the error of a damaged packet comes with up to thread_count
            // packets sent after it (frame threads decode several frames at once, libavcodec/pthread_frame.c, and
            // a decoding error is returned when the frame of that thread is collected, which is the oldest one in
            // flight), so the packet is unknown, and a keyframe sent after it may already be decoded by a decoder
            // that saw the damage. Decode the rest of this file with 1 thread, from the keyframe before the
            // requested frame (decode()): the packet of each error is then known, and the result does not
            // depend on the threads.
            if (((stream->_codecContext->active_thread_type & FF_THREAD_FRAME) || (stream->_codecContext->thread_count != 1)) && !stream->_singleThreaded) {
                stream->_singleThreaded = true;
                stream->_restartSingleThread = true;
            }
        }
        if (++droppedPackets > kMaxDroppedPackets) {
            stream->_tooManyDroppedPackets = true;
            if (!decodedAny) {
                stream->_invalidDataSkipped = r;
                markOnlyDamaged();
            }
            return false; // fail with this error
        }
        // With frame threads (or the decoder's own threads, e.g. libdav1d), the error of a packet is
        // returned later, with another packet: it is unknown.
#if defined(AV_CODEC_CAP_OTHER_THREADS)
        const bool otherThreads = stream->_videoCodec && (stream->_videoCodec->capabilities & AV_CODEC_CAP_OTHER_THREADS) && (stream->_codecContext->thread_count != 1);
#elif defined(AV_CODEC_CAP_AUTO_THREADS) // FFmpeg 4.0-4.3 (e.g. libdav1d)
        const bool otherThreads = stream->_videoCodec && (stream->_videoCodec->capabilities & AV_CODEC_CAP_AUTO_THREADS) && (stream->_codecContext->thread_count != 1);
#else
        const bool otherThreads = false;
#endif
        const bool frameThreads = ((stream->_codecContext->active_thread_type & FF_THREAD_FRAME) != 0) || otherThreads;
        // (With frame threads, the error is that of a packet up to the decoder delay before this one.)
        if (acceptCorruptHeldFrame && pkt && (pkt->pts != int64_t(AV_NOPTS_VALUE)) && (stream->ptsToFrame(pkt->pts) - (frameThreads ? stream->getCodecDelay() : 0) >= stopFrame) && !Stream::hasFrame(stream->_avHeldFrame)) {
            stream->_invalidDataSkipped = r;
            stream->_stoppedOnDamagedPacket = true;
            // With frame threads, the frames after the requested one are not known to be damaged (the
            // damaged packet is one of the last ones sent).
            stream->_lastDamagedFrame = (std::max)(stream->_lastDamagedFrame, frameThreads ? (int)frame : (std::max)((int)frame, stream->ptsToFrame(pkt->pts)));
            return false; // intra-only: stop, with this error (decode() then decodes from earlier frames)
        }
        stream->_invalidDataSkipped = r;
        if (pkt && (pkt->pts != int64_t(AV_NOPTS_VALUE))) {
            const int damagedFrame = stream->ptsToFrame(pkt->pts) - (frameThreads ? stream->getCodecDelay() : 0);
            stream->_lastDamagedFrame = (std::max)(stream->_lastDamagedFrame, damagedFrame);
        }
        if (pkt && !frameThreads && (pkt->pts != int64_t(AV_NOPTS_VALUE)) && !stream->isDamagedPts(pkt->pts)) {
            if (stream->_damagedPts.size() >= 1024) {
                stream->_damagedPts.erase(stream->_damagedPts.begin());
            }
            stream->_damagedPts.push_back(pkt->pts);
        }
        if (!_damagedPacketWarned) {
            _damagedPacketWarned = true;
            char errorBuf[256];
            av_strerror(r, errorBuf, sizeof(errorBuf));
            av_log(nullptr, AV_LOG_WARNING, "FFmpeg Reader: %s: skipping damaged packets (%s)\n", _filename.c_str(), errorBuf);
        }
#if TRACE_DECODE_PROCESS
        std::cout << "FFmpeg Reader: skipping a damaged packet" << std::endl;
#endif
        return true;
    };

    // Drain all frames currently available from the decoder, checking each against the
    // target frame. Returns:
    //   1  if the target frame was found (and converted into avFrameOut),
    //   0  if the decoder needs more input (EAGAIN) or has been fully drained (EOF),
    //  <0  an AVERROR on a real decode error.
    // avcodec_receive_frame() unrefs the destination frame before writing, so there is
    // no need to unref between iterations.
    auto drainDecoder = [&]() -> int {
        for (;;) {
            int r = avcodec_receive_frame(stream->_codecContext, avFrameDecodeDst);
            if ((r == AVERROR(EAGAIN)) || (r == AVERROR_EOF)) {
                return 0;
            }
            if (skipInvalidData(r, nullptr)) {
                if (stream->_restartSingleThread) {
                    return 0; // see decode()
                }
                continue;
            }
            if (r < 0) {
                return r;
            }
            if (processDecodedFrame(avFrameDecodeDst)) {
                return 1;
            }
        }
    };

#if defined(AV_CODEC_CAP_OTHER_THREADS)
    const bool otherThreadsActive = stream->_videoCodec && (stream->_videoCodec->capabilities & AV_CODEC_CAP_OTHER_THREADS) && (stream->_codecContext->thread_count != 1);
#elif defined(AV_CODEC_CAP_AUTO_THREADS)
    const bool otherThreadsActive = stream->_videoCodec && (stream->_videoCodec->capabilities & AV_CODEC_CAP_AUTO_THREADS) && (stream->_codecContext->thread_count != 1);
#else
    const bool otherThreadsActive = false;
#endif
    const bool frameThreadsActive = ((stream->_codecContext->active_thread_type & FF_THREAD_FRAME) != 0) || otherThreadsActive;

    // Fail with an error. After too many damaged packets, the decoder may still hold good frames (e.g. the
    // frames waiting for their B-frames, or with frame threads the frames being decoded): drain it first, which
    // may give the requested frame, or the frames around it (a gap). With frame threads, the frames of the last
    // packets sent are not used: decoding with 1 thread would have stopped before them, and the result must not
    // depend on the threads. The decoder is then drained: the next requests that need more frames seek
    // (Stream::_drainedEarly).
    // (The window is thread_count packets. A decoder with its own threads, AV_CODEC_CAP_OTHER_THREADS, e.g.
    // libdav1d, may have a longer delay, e.g. its max_frame_delay: then a few frames sent after the last damaged
    // packet may still be used, and the result may depend on the threads.)
    auto fail = [&](int r, const char* prefix) -> bool {
        if (stream->_tooManyDroppedPackets && !stream->_drainedEarly) {
            stream->_drainedEarly = true;
            (void)avcodec_send_packet(stream->_codecContext, nullptr);
            for (int i = 0; i < 4 * kMaxDroppedPackets; ++i) {
                const int rr = avcodec_receive_frame(stream->_codecContext, avFrameDecodeDst);
                if ((rr == AVERROR(EAGAIN)) || (rr == AVERROR_EOF)) {
                    break;
                }
                if (rr < 0) {
                    continue; // another damaged frame
                }
                if ((avFrameDecodeDst->pts != int64_t(AV_NOPTS_VALUE)) && frameThreadsActive && (std::find(stream->_recentPts.begin(), stream->_recentPts.end(), avFrameDecodeDst->pts) != stream->_recentPts.end())) {
                    // With frame threads, the error of the last damaged packet came with up to thread_count packets
                    // sent after it, which decoding with 1 thread would not have read: their frames are not used.
                    continue;
                }
                if (processDecodedFrame(avFrameDecodeDst)) {
                    if (hasPicture) {
                        return true;
                    }
                    break;
                }
            }
        }
        setDecodeError(r, prefix);
        return false;
    };

    // Begin reading from the newly seeked position, or from the packet that the decoder refused at the previous
    // request (Stream::_pendingPacket, see below): it is sent first, as the next packet of the stream (its checks
    // below were made when it was read, and it was counted by Stream::packetSent()).
    bool resendPending = Stream::hasPacket(stream->_pendingPacket);
    if (resendPending) {
        av_packet_move_ref(avPacket.pkt(), stream->_pendingPacket);
    }
    while (resendPending || ((res = av_read_frame(_context, avPacket.pkt())) >= 0)) {
        const bool resending = resendPending;
        resendPending = false;
        if (stream->_restartSingleThread) {
            return hasPicture; // decode() decodes again with 1 thread
        }

        if (avPacket->stream_index != stream->_idx) {
            continue;
        }

        // Send the packet, then fully drain any frames it produced. If the decoder's input
        // buffer is full it returns EAGAIN, which per the API contract means we must drain
        // output first and then resend the *same* packet (rather than dropping it).
        // The partial last packet of a truncated file (flagged as corrupt by the demuxer, which read it up to
        // the end of the file) is missing data: it is not decoded (an intra-only frame would decode from a
        // part of its data, and give different images in different orders), and the frames from there on
        // fail, sequentially and after a seek, as before (issue #134).
        // (A packet with a transport error that happens to be the last one of the file, e.g. a TS continuity
        // counter error in the last packet, is also taken as truncated: rare.)
        if (!resending && (avPacket->flags & AV_PKT_FLAG_CORRUPT) && _context->pb && (avio_size(_context->pb) > 0) && (avio_feof(_context->pb) || (avio_tell(_context->pb) >= avio_size(_context->pb)))) {
            const int64_t ts = (avPacket->pts != int64_t(AV_NOPTS_VALUE)) ? avPacket->pts : avPacket->dts;
            if (ts != int64_t(AV_NOPTS_VALUE)) {
                const int truncatedFrame = stream->ptsToFrame(ts);
                if ((stream->_truncatedFrame < 0) || (truncatedFrame < stream->_truncatedFrame)) {
                    stream->_truncatedFrame = truncatedFrame;
                }
            }
            continue;
        }
        // A keyframe after damage (long-GOP codecs): the damaged packets may have changed the state of the decoder
        // (e.g. a damaged packet parsed as a parameter set, or broken reordering), so that the frames after the
        // keyframe would not be output, or would differ from the frames decoded after a seek to it. Drain the
        // decoder (its frames before the keyframe are still used), and restart it at the keyframe, as after a
        // seek. A packet with a keyframe flag in damaged data (e.g. from a broken Matroska block header,
        // matroskadec.c:4319-4320) is not a real keyframe: the frames are only used again once the keyframe
        // decodes as a keyframe, without an error (Stream::verifyRestart()); otherwise the decoder restarts at the
        // next packet with the keyframe flag. If draining gives
        // the requested frame, or stops decoding, the keyframe is not sent (nor the other frames of the drain
        // used): the next requests that need more frames seek (Stream::_drainedEarly).
        if (!resending && !acceptCorruptHeldFrame && stream->_tainted && (avPacket->flags & AV_PKT_FLAG_KEY) && !stream->_verifyNextPacket) {
            bool stop = false;
            (void)avcodec_send_packet(stream->_codecContext, nullptr);
            for (int i = 0; i < 4 * kMaxDroppedPackets; ++i) {
                const int rr = avcodec_receive_frame(stream->_codecContext, avFrameDecodeDst);
                if ((rr == AVERROR(EAGAIN)) || (rr == AVERROR_EOF)) {
                    break;
                }
                if (rr < 0) {
                    continue; // a damaged frame
                }
                if (processDecodedFrame(avFrameDecodeDst)) {
                    stop = true;
                    break;
                }
            }
            // The drain gave the frame of the previous restart keyframe, if any (with B-frames, a keyframe is
            // output after the next packets, which may be another keyframe, e.g. when damaged blocks are lost):
            // if it did not come, that restart gave no clean keyframe.
            if (stream->_restartPts != int64_t(AV_NOPTS_VALUE)) {
                stream->verifyRestart(false);
            }
            flushDecoder(stream);
            // The frames of the packets from here on are used once this keyframe decodes cleanly (see
            // Stream::verifyRestart(); the tainted PTS are kept until the next seek), unless the drain verified
            // the previous restart.
            if (stream->positionTaint()) {
                stream->_awaitRestartFrame = stream->_tainted;
            } else {
                stream->_verifyNextPacket = stream->_tainted;
            }
            stream->_recentPts.clear();
            if (stop) {
                stream->_drainedEarly = true;
                return hasPicture;
            }
        }
        if (!resending) {
            stream->packetSent(avPacket->pts, frameThreadsActive ? (std::max)(1, stream->_codecContext->thread_count) : 1, (avPacket->flags & AV_PKT_FLAG_KEY) != 0);
        }
        res = avcodec_send_packet(stream->_codecContext, avPacket.pkt());
        if ((res >= 0) && !acceptCorruptHeldFrame) {
            // Long-GOP codecs (decoded with 1 thread after damage, see skipInvalidData): an accepted packet ends a
            // run of damaged packets, also if its frame is not output (e.g. the H.264 decoder only outputs the
            // frames after a restart once recovered, libavcodec/h264_slice.c:1374-1384), so that the bound
            // (kMaxDroppedPackets) does not depend on what the decoder outputs, which depends on the read order.
            droppedPackets = 0;
        }
        if (skipInvalidData(res, avPacket.pkt())) {
            res = 0; // the packet is dropped: drain the frames of the previous packets
        } else if ((res < 0) && (res != AVERROR(EAGAIN))) {
            return fail(res, "FFmpeg Reader Failed to decode packet: ");
        }

        int drained = drainDecoder();
        if (drained < 0) {
            return fail(drained, "FFmpeg Reader Failed to decode packet: ");
        }
        if (drained == 1) {
            if (res == AVERROR(EAGAIN)) {
                // The drain gave the requested frame, but the packet was not accepted: keep it for the next request,
                // which sends it before reading the next one (a seek drops it, Stream::resetHeldFrames()). It was
                // lost before, and the frames that reference it decoded wrong until the next keyframe (issue #159).
                // E.g. VP9 with frame threads: the decoder splits a superframe (a hidden alt-ref frame and a shown
                // frame) into two packets with its vp9_superframe_split bitstream filter (FFCodec.bsfs,
                // libavcodec/vp9.c:1934 in FFmpeg 9.0.2, 1927 in 8.0). Its frame threads take one packet per thread
                // and return a frame once every thread has one (ff_thread_receive_frame(), libavcodec/
                // pthread_frame.c:565-603), so the bitstream filter may still hold the shown frame when the next
                // packet is sent: the decoder takes that one first and keeps the sent packet in its input buffer
                // (ff_decode_get_packet(), libavcodec/decode.c:268-283), until a later avcodec_receive_frame().
                // A packet sent before that is refused with AVERROR(EAGAIN) (avcodec_send_packet(), decode.c:
                // 745-747), and the frame of the drain may then be the requested one.
                av_packet_move_ref(stream->_pendingPacket, avPacket.pkt());
            }
            return hasPicture;
        }

        if (res == AVERROR(EAGAIN)) {
            // The packet was not accepted before; resend it now that output has been drained.
            res = avcodec_send_packet(stream->_codecContext, avPacket.pkt());
            if (skipInvalidData(res, avPacket.pkt())) {
                res = 0;
            } else if (res < 0) {
                return fail(res, "FFmpeg Reader Failed to decode packet: ");
            }
            drained = drainDecoder();
            if (drained < 0) {
                return fail(drained, "FFmpeg Reader Failed to decode packet: ");
            }
            if (drained == 1) {
                return hasPicture;
            }
        }
    }

    // Check for any read failures
    if (res < 0 && res != AVERROR_EOF) {
        setDecodeError(res, "FFmpeg Reader Failed to read frame: ");
        return false;
    }

    // Flush the decoder of remaining frames by sending a NULL packet (enter draining mode).
    if (res == AVERROR_EOF) {
        draining = true;
        // (With frame threads, the error of a damaged packet may come here: the decoder is then draining.)
        res = avcodec_send_packet(stream->_codecContext, nullptr);
        if ((res < 0) && (res != AVERROR_EOF) && !skipInvalidData(res, nullptr)) {
            return fail(res, "FFmpeg Reader Failed to flush the decoder of remaining frames: ");
        }

        int drained = drainDecoder();
        if (drained < 0) {
            return fail(drained, "FFmpeg Reader Failed to flush the decoder of remaining frames: ");
        }
        if (drained == 1) {
            return hasPicture;
        }

        // The decoder is drained: the last decoded frame is displayed until the end of the
        // stream (which may be later than that frame, e.g. if the stream duration is longer).
        // But not at or after the truncated packet of a truncated file: the frames from there on fail.
        if ((stream->_truncatedFrame >= 0) && (frame >= stream->_truncatedFrame)) {
            setDecodeError("the file is truncated", "FFmpeg Reader: ");
            return false;
        }
        if (!acceptCorruptHeldFrame && stream->_tainted) {
            // Long-GOP codecs: the stream ends in damage (no clean keyframe after it): the frames after the last
            // clean one fail, as the reader did before (it failed at the first damaged packet), whatever the read
            // order (holding the last clean frame would depend on whether it was decoded before the damage).
            stream->_endTainted = true;
            stream->_reachedEnd = !Stream::hasFrame(held); // (e.g. a damaged first keyframe, see decode())
            setDecodeError(stream->_invalidDataSkipped ? stream->_invalidDataSkipped : AVERROR_INVALIDDATA, "FFmpeg Reader Failed to decode packet: ");
            return false;
        }
        if (Stream::hasFrame(held)) {
            stream->_heldFrameIsLast = true;
            if (!canHoldFrame(held)) {
                return false;
            }
            return imageConvert(held, avFrameOut);
        }
        stream->_reachedEnd = true;
        markOnlyDamaged(); // e.g. a file damaged up to its end
    }

    return hasPicture;
}

bool
FFmpegFile::imageConvert(AVFrame* avFrameIn, AVFrame* avFrameOut)
{
    if (!avFrameIn || !avFrameOut) {
        setDecodeError("mov64Reader no input or output frame provided for conversion");
        return false;
    }

    Stream* stream = _selectedStream;

    AVPixelFormat dstPixFmt = (AVPixelFormat)avFrameOut->format;
    AVPixelFormat srcPixFmt = (AVPixelFormat)avFrameIn->format;

    avFrameOut->pts = avFrameIn->pts;
    avFrameOut->pkt_dts = avFrameIn->pkt_dts;
    OFX_AVFRAME_DURATION(avFrameOut) = OFX_AVFRAME_DURATION(avFrameIn);

    if (!avFrameOut->data[0]) {
        int res = av_image_alloc(avFrameOut->data, avFrameOut->linesize, avFrameOut->width, avFrameOut->height, dstPixFmt, 32);
        if (res <= 0) {
            setDecodeError(res, "FFmpeg Reader Output frame allocation failed during format conversion");
            return false;
        }
    }

    SwsContext* context = stream->getConvertCtx(
        srcPixFmt,
        avFrameIn->width,
        avFrameIn->height,
        avFrameIn->color_range,
        avFrameIn->colorspace,
        dstPixFmt,
        avFrameOut->width,
        avFrameOut->height);

    // Scale if any of the decoding path has provided a convert
    // context. Otherwise, no scaling/conversion is required after
    // decoding the frame.
    if (context) {
        sws_scale(
            context,
            avFrameIn->data,
            avFrameIn->linesize,
            0,
            avFrameIn->height,
            avFrameOut->data,
            avFrameOut->linesize);
    } else {
        return false;
    }

    return true;
}

bool
FFmpegFile::getFPS(double& fps,
                   unsigned streamIdx)
{
    if (streamIdx >= _streams.size()) {
        return false;
    }

    // get the stream
    Stream* stream = _streams[streamIdx];
    // guard against division by zero
    assert(stream->_fpsDen);
    fps = stream->_fpsDen ? ((double)stream->_fpsNum / stream->_fpsDen) : stream->_fpsNum;

    return true;
}

// get stream information
bool
FFmpegFile::getInfo(int& width,
                    int& height,
                    double& aspect,
                    int& frames)
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_lock);
#endif

    if (_streams.empty()) {
        return false;
    }

    assert(_selectedStream && "Null _selectedStream");
    if (!_selectedStream) {
        return false;
    }
    width = _selectedStream->_width;
    height = _selectedStream->_height;
    aspect = _selectedStream->_aspect;
    frames = (int)_selectedStream->_frames;

    return true;
}

std::size_t
FFmpegFile::getBufferBytesCount() const
{
    if (_streams.empty()) {
        return 0;
    }

    Stream* stream = _streams[0];
    std::size_t pixelDepth = stream->_bitDepth > 8 ? sizeof(unsigned short) : sizeof(unsigned char);

    // this is the first stream (in fact the only one we consider for now), allocate the output buffer according to the bitdepth
    return stream->_width * stream->_height * stream->_numberOfComponents * pixelDepth;
}

FFmpegFileManager::FFmpegFileManager()
    : _files()
    , _lock(nullptr)
{
}

FFmpegFileManager::~FFmpegFileManager()
{
    // shared_ptr entries free their FFmpegFile when the map is cleared.
    _files.clear();
    delete _lock;
}

void
FFmpegFileManager::init()
{
    _lock = new FFmpegFile::Mutex;
}

void
FFmpegFileManager::clear(void const* plugin)
{
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    FilesMap::iterator found = _files.find(plugin);
    if (found != _files.end()) {
        // Erasing drops the map's references; any FFmpegFile still referenced by a
        // concurrent caller stays alive until that caller releases its shared_ptr.
        _files.erase(found);
    }
}

std::shared_ptr<FFmpegFile>
FFmpegFileManager::get(void const* plugin,
                       const string& filename) const
{
    if (filename.empty() || !plugin) {
        return nullptr;
    }
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    FilesMap::iterator found = _files.find(plugin);
    if (found != _files.end()) {
        for (std::list<std::shared_ptr<FFmpegFile>>::iterator it = found->second.begin(); it != found->second.end(); ++it) {
            if ((*it)->getFilename() == filename) {
                if ((*it)->isInvalid()) {
                    found->second.erase(it);
                    break;
                } else {
                    return *it;
                }
            }
        }
    }

    return nullptr;
}

std::shared_ptr<FFmpegFile>
FFmpegFileManager::getOrCreate(void const* plugin,
                               const string& filename) const
{
    if (filename.empty() || !plugin) {
        return nullptr;
    }
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    FilesMap::iterator found = _files.find(plugin);
    if (found != _files.end()) {
        for (std::list<std::shared_ptr<FFmpegFile>>::iterator it = found->second.begin(); it != found->second.end(); ++it) {
            if ((*it)->getFilename() == filename) {
                if ((*it)->isInvalid()) {
                    found->second.erase(it);
                    break;
                } else {
                    return *it;
                }
            }
        }
    }

    std::shared_ptr<FFmpegFile> file = std::make_shared<FFmpegFile>(filename);
    if (found == _files.end()) {
        std::list<std::shared_ptr<FFmpegFile>> fileList;
        fileList.push_back(file);
        _files.insert(make_pair(plugin, fileList));
    } else {
        found->second.push_back(file);
    }

    return file;
}
