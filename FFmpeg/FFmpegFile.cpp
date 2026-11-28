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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

extern "C" {
#include <libavutil/intreadwrite.h>
#include <libavutil/pixdesc.h>
}

#include <ofxsImageEffect.h>
#include <ofxsMacros.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(WIN64)
#define strncasecmp _strnicmp
#define strcasecmp _stricmp // strcasecmp is POSIX: not declared by MinGW without -U__STRICT_ANSI__ under a strict -std
#endif

using namespace OFX;

using std::string;

#define CHECK(x)                     \
    {                                \
        int error = x;               \
        if (error < 0) {             \
            setInternalError(error); \
            return;                  \
        }                            \
    }

//#define TRACE_FILE_OPEN 1

static_assert(ffmpegIsRelease80("8.0") && ffmpegIsRelease80("n8.0") && ffmpegIsRelease80("8.0-1ubuntu1")
                  && ffmpegIsRelease80("n8.0-12-g0123456") && !ffmpegIsRelease80("8.0.1") && !ffmpegIsRelease80("n8.0.1")
                  && !ffmpegIsRelease80("8.0.1-3ubuntu2") && !ffmpegIsRelease80("8.1") && !ffmpegIsRelease80("9.0.2")
                  && !ffmpegIsRelease80("80") && !ffmpegIsRelease80("N-121000-g0123456") && !ffmpegIsRelease80("")
                  && !ffmpegIsRelease80("n"),
              "ffmpegIsRelease80");

std::string
ffmpegRuntimeVersionError()
{
    const char* const version = av_version_info();
    if (!version || !ffmpegIsRelease80(version)) {
        return std::string();
    }

    return std::string("the FFmpeg libraries are FFmpeg ") + version
        + ", but this plug-in requires FFmpeg 8.0.1 or later (it was built with " FFMPEG_VERSION
          "): FFmpeg 8.0's libswscale truncates in its range conversions. The plug-in is not loaded.";
}

// A file with an extension of the image2 muxer (FFmpeg n9.1 libavformat/img2enc.c:398-400, n8.0 img2enc.c:294-296) holds
// one image, unless a demuxer declares that extension: it may then hold several frames (the mov demuxer for AVIF, which
// may be an image sequence, n9.1 libavformat/mov.c:13313; jpegxl_anim for animated JPEG XL, jpegxl_anim_dec.c:201).
// av_match_ext() compares the extension case-insensitively (n9.1 libavformat/format.c:41-52, libavutil/avstring.c:344-365).
// The result depends on the demuxers that FFmpeg was built with: e.g. with --enable-libopenmpt, the libopenmpt
// demuxer declares "ppm" (a module format, n9.1 libavformat/libopenmpt.c:295), and .ppm files are not images.
bool
FFmpegFile::isImageFile(const string& filename)
{
    static const AVOutputFormat* const image2 = av_guess_format("image2", nullptr, nullptr);
    if (!image2 || !av_match_ext(filename.c_str(), image2->extensions)) {
        return false;
    }
    void* opaque = nullptr;
    while (const AVInputFormat* iformat = av_demuxer_iterate(&opaque)) {
        if (iformat->extensions && av_match_ext(filename.c_str(), iformat->extensions)) {
            return false;
        }
    }

    return true;
}

namespace {
struct FilterEntry {
    const char* name;
    bool enableReader;
    bool enableWriter;
};

// Bug 11027 - Nuke write: ffmpeg codec fails has details on individual codecs

// Container formats the writer offers (only isFormatAllowedForWriting() uses this table).
// For a full list of formats, define OFX_FFMPEG_PRINT_CODECS in WriteFFmpeg.cpp.
// Not offered for writing:
// - dv: DV (Digital Video), no HD support
// - gif: GIF Animation - write not supported as 8-bit only.
// - h264: raw H.264 video. prefer a proper container (mp4, mov, avi)
// - hevc: raw HEVC video. hevc codec cannot be read in official qt
// - m4v: raw MPEG-4 video. prefer a proper container (mp4, mov, avi)
// - mxf: not readable in Qt but may be used with other software, however MXF has too many constraints to be easyly writable (for H264 it requires avctx.profile = FF_PROFILE_H264_BASELINE, FF_PROFILE_H264_HIGH_10 or FF_PROFILE_H264_HIGH_422). it is better to transcode with an external tool
// - ogg: Ogg, for theora codec (use ogv for writing)
const FilterEntry kFormatAllowlist[] = {
    { "3gp", true, true },
    { "3g2", true, true },
    { "avi", true, true },
    { "flv", true, true }, // FLV (Flash Video), only used with flv codec. cannot be read in official Qt
    { "matroska", true, true }, // not readable in Qt but may be used with other software
    { "mov", true, true },
    { "mp4", true, true },
    { "mpeg", true, true },
    { "mpegts", true, true },
    { "ogv", true, true }, // Ogg Video, for theora codec
    { nullptr, false, false }
};

// For a full list of codecs, define OFX_FFMPEG_PRINT_CODECS in WriteFFmpeg.cpp.
// A range of codecs are omitted for licensing reasons, or because they support obsolete/unnecessary
// formats that confuse the interface.

#define UNSAFEQT0 true // set to true: not really harmful
#define UNSAFEQT false // set to false: we care about QuickTime, because it is used widely - mainly colorshift issues
#define UNSAFEVLC true // set to true: we don't care much about being playable in VLC
#define TERRIBLE false
//#define SHOULDWORK true
#define SHOULDWORK false
// The reader column is unused since #339 (see #344).
const FilterEntry kCodecAllowlist[] = {
    // Video codecs.
    { "aic", true, false }, // Apple Intermediate Codec (no encoder)
    { "av1", true, false }, // Alliance for Open Media AV1 (encoders: libaom-av1 librav1e)
    { "avrp", true, UNSAFEQT0&& UNSAFEVLC }, // Avid 1:1 10-bit RGB Packer - write not supported as not official qt readable with relevant 3rd party codec.
    { "avui", true, false }, // Avid Meridien Uncompressed - write not supported as this is an SD only codec. Only 720x486 and 720x576 are supported. Experimental (AV_CODEC_CAP_EXPERIMENTAL, n8.0 libavcodec/avuienc.c:100).
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
    // writes the 'colr' atom (FFmpeg n9.0 libavformat/movenc.c:2948-2953, n8.0 movenc.c:2847-2852). The muxer also
    // writes the version 2 sample description, lossless spatial quality and 'clap' atom that Apple's TN2162 asks for
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
FFmpegFile::isFormatAllowedForWriting(const char* formatName)
{
    const FilterEntry* allowlistEntry = getEntry(formatName, kFormatAllowlist);

    return (allowlistEntry && allowlistEntry->enableWriter);
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
//   n9.0 libavcodec/decode.c:575-585, n8.0 libavcodec/decode.c:556-566), else the one of the stream
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
    if (FFmpeg::nonJpegPixelFormat(srcPixelFormat) != srcPixelFormat) { // a YUVJ pixel format
        return 1;
    }

    // If the colour range wasn't specified, set the flag according to
    // whether the data is YUV or not.
    return isYUV() ? 0 : 1;
}

bool
FFmpegFile::Stream::isYUV() const
{
    return FFmpeg::pixelFormatIsYUV(_codecContext->pix_fmt);
}

namespace {
// The flags of the conversion to RGB (#207, #220). Measured on FFmpeg 8.0 and 9.0.2 (4:2:0, BT.601 video range, against
// the exact equations of ITU-T H.273 §8.3, in 1/255): to 8-bit RGB, the error is +0.003 on average and 0.514 at most,
// i.e. the result is rounded to the nearest; with SWS_BICUBIC alone, it was -0.22 / 1.46 (aarch64 NEON) or
// -0.94 / 2.88 (C), and adding SWS_ACCURATE_RND alone gave -0.94 / 2.88.
// - SWS_ACCURATE_RND turns off the unscaled yuv420p/yuv422p/yuva420p to RGB path (in FFmpeg n9.0.2,
//   libswscale/swscale_unscaled.c:2426-2430), which never writes the last column at an odd width and even height: its
//   loop writes blocks of 8, 4 and 2 pixels (libswscale/yuv2rgb.c:158, :198, :207-236) (#220).
// - SWS_FULL_CHR_H_INT computes each RGB pixel from interpolated chroma, without the 8-bit lookup tables of
//   ff_yuv2rgb_c_init_tables() (libswscale/yuv2rgb.c:756-787), which round with a bias.
// The 16-bit output is then scaled to white (see MeasureWhite16()).
// Converting a 1080p frame on one thread (arm64) takes 7.7 ms instead of 0.23 ms (4:2:0 8-bit, NEON; 1.2 ms without),
// 2.4 ms instead of 0.30 ms (4:2:2 8-bit), and, with the scaling to white, 10.8 ms instead of 4.8 ms (4:2:0 10-bit)
// and 5.3 ms instead of 2.6 ms (4:2:2 10-bit).
constexpr int kReaderSwsFlags = SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT;

using FFmpeg::AvBuffer;
using FFmpeg::kWhite16;

// Creates the reader's swscale context. yuv: set up the conversion from Y'CbCr with the given matrix (SWS_CS_*) and
// range (0 = video, 1 = full) to full-range RGB.
SwsContext*
CreateReaderSwsContext(AVPixelFormat srcPixelFormat,
                       int srcWidth,
                       int srcHeight,
                       AVPixelFormat dstPixelFormat,
                       int dstWidth,
                       int dstHeight,
                       bool yuv,
                       int srcRange,
                       int swsColorspace)
{
    SwsContext* context = sws_getContext(srcWidth, srcHeight, srcPixelFormat, // src format
                                         dstWidth, dstHeight, dstPixelFormat, // dest format
                                         kReaderSwsFlags, nullptr, nullptr, nullptr);

    // Set up the SoftWareScaler to convert colorspaces correctly.
    // Colorspace conversion makes no sense for RGB->RGB conversions
    if (!context || !yuv) {
        return context;
    }

    int result = sws_setColorspaceDetails(context,
                                          sws_getCoefficients(swsColorspace), // inv_table
                                          srcRange, // srcRange -flag indicating the white-black range of the input (1=jpeg / 0=mpeg) 0 = 16..235, 1 = 0..255
                                          sws_getCoefficients(SWS_CS_DEFAULT), // table
                                          1, // dstRange - 0 = 16..235, 1 = 0..255
                                          0, // brightness fixed point, with 0 meaning no change,
                                          1 << 16, // contrast   fixed point, with 1<<16 meaning no change,
                                          1 << 16); // saturation fixed point, with 1<<16 meaning no change);

    assert(result != -1);
    (void)result;

    return context;
}

// What swscale converts nominal white and opaque alpha to (16-bit values).
struct White16 {
    unsigned colour; // R', G' and B'
    unsigned alpha;
};

// #316: the 16-bit values that the reader's conversion gives for nominal white and for opaque alpha. swscale's
// conversion from Y'CbCr to 16-bit RGB (FFmpeg 8.0 to 9.1) does not map white to 65535, whatever the flags, nor does
// the ffmpeg CLI (upstream: https://code.ffmpeg.org/FFmpeg/FFmpeg/issues/25681). Measured on FFmpeg 8.0 and 9.0.2,
// for the Y'CbCr pixel formats (planar, semi-planar and packed, 4:2:0 to 4:4:4, with alpha), at odd and even widths:
// - video range: Y' = 16·2^(n-8), the nominal white Y' = 235·2^(n-8) and the mid-gray in between give 0, 65283 and
//   32641 (exact: 0, 65535 and 32767.5), whatever the bit depth n: 255/219 is scaled in 8-bit units
//   (libswscale/yuv2rgb.c:756-761 in n9.0.2), and the output of yuv2rgba64_full_X_c_template()
//   (libswscale/output.c:1374-1432) reaches about 255·256 = 65280;
// - full range: Y' = 2^n - 1 gives (2^n - 1)·2^(16-n), e.g. 65472 at 10 bits and 65520 at 12 bits, a shift without
//   bit replication (16 bits: 65535);
// - alpha: A = 2^n - 1 gives (2^n - 1)·2^(16-n) too (output.c:1400-1406, :1430).
// The nominal white must give 1: Y' = 219·2^(n-8)·E'Y + 16·2^(n-8) in video range, (2^n - 1)·E'Y in full range
// (ITU-T H.273 §8.3; the quantization levels of ITU-R BT.709-6 Part 2 and ITU-R BT.2100-3 Table 9: narrow range
// 64-940 and full range 0-1023 at 10 bits).
// Rather than hard-coding these values, the conversion of a small image of nominal white and opaque alpha in the
// source pixel format measures them, with the same context parameters: no scaling once swscale is fixed.
// A value below 254·256 is not a scaling error, and is not corrected (nor are pixel formats that
// av_image_fill_color() can not fill, e.g. hardware or bitstream formats).
White16
MeasureWhite16(AVPixelFormat srcPixelFormat,
               int srcWidth,
               AVPixelFormat dstPixelFormat,
               bool yuv,
               int srcRange,
               int swsColorspace)
{
    const White16 none = { kWhite16, kWhite16 };
    const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(srcPixelFormat);
    const AVPixFmtDescriptor* dstDesc = av_pix_fmt_desc_get(dstPixelFormat);
    // only Y'CbCr to 16-bit RGB: from RGB, the conversion is exact (measured: gbrp10, gbrp12, gbrap12, x2rgb10)
    if (!yuv || !srcDesc || !dstDesc || (dstDesc->comp[0].depth != 16) || (srcDesc->comp[0].depth < 8) || (srcDesc->comp[0].depth > 16)) {
        return none;
    }
    // 4 lines: enough for any vertical chroma subsampling (4:1:0 included)
    const int width = srcWidth, height = 4;
    uint8_t* srcData[4] = {};
    int srcLinesize[4] = {};
    uint8_t* dstData[4] = {};
    int dstLinesize[4] = {};
    if (av_image_alloc(srcData, srcLinesize, width, height, srcPixelFormat, 64) < 0) {
        return none;
    }
    const AvBuffer srcBuffer(srcData[0], &av_free);
    if (av_image_alloc(dstData, dstLinesize, width, height, dstPixelFormat, 64) < 0) {
        return none;
    }
    const AvBuffer dstBuffer(dstData[0], &av_free);

    // the components in the order of srcDesc->comp[] (Y', Cb, Cr, A or Y', A): nominal white, no chroma, opaque
    uint32_t color[4] = {};
    const bool hasChroma = srcDesc->nb_components >= 3;
    for (int c = 0; c < srcDesc->nb_components; ++c) {
        const int depth = srcDesc->comp[c].depth;
        if (c == 0) {
            color[c] = FFmpeg::nominalWhite(depth, srcRange != 0);
        } else if (hasChroma && (c <= 2)) {
            color[c] = 1u << (depth - 1);
        } else {
            color[c] = (1u << depth) - 1; // alpha
        }
    }
    const ptrdiff_t fillLinesize[4] = { srcLinesize[0], srcLinesize[1], srcLinesize[2], srcLinesize[3] };
    if (av_image_fill_color(srcData, fillLinesize, srcPixelFormat, color, width, height, 0) < 0) {
        return none;
    }
    SwsContext* context = CreateReaderSwsContext(srcPixelFormat, width, height, dstPixelFormat, width, height, yuv, srcRange, swsColorspace);
    if (!context) {
        return none;
    }
    const int lines = sws_scale(context, srcData, srcLinesize, 0, height, dstData, dstLinesize);
    sws_freeContext(context);
    if (lines != height) {
        return none;
    }

    // a value is a scaling error to correct if it is at least 254·256 (and not 65535); the pixel in the middle
    constexpr unsigned kMinWhite16 = 254u << 8;
    const int nbComponents = dstDesc->nb_components; // 3 (RGB48LE) or 4 (RGBA64LE)
    const uint8_t* pixel = dstData[0] + (height / 2) * dstLinesize[0] + (width / 2) * nbComponents * 2;
    const unsigned colour = AV_RL16(pixel);
    const unsigned alpha = (nbComponents == 4) ? AV_RL16(pixel + 6) : kWhite16;

    return White16 { (colour >= kMinWhite16) ? colour : kWhite16, (alpha >= kMinWhite16) ? alpha : kWhite16 };
} // MeasureWhite16

// The scaling of a 16-bit value v so that den gives 65535: (v·65535 + den/2) / den, clamped to 65535 (values above
// white), for den in [254·256, 65535]. A multiplication is 1.5 times as fast as the division (1.9 ms instead of 2.9 ms
// for a 1080p RGB48 frame on arm64) and gives the same value for every v < 2^16: with M = ceil(65535·2^40 / den),
// v·M exceeds v·65535·2^40 / den by less than 2^16, so (v·M + 2^39) >> 40 differs from the rounded quotient only if
// the fraction of v·65535 / den + 1/2 were above 1 - 2^-24, but it is a multiple of 1 / (2·den), at most
// 1 - 1/131070 (checked for every v and den).
class ScaleTo16 {
public:
    explicit ScaleTo16(unsigned den)
        : _m(((uint64_t(kWhite16) << 40) + den - 1) / den)
    {
        assert((den >= (254u << 8)) && (den <= kWhite16));
    }

    unsigned operator()(unsigned v) const
    {
        const uint64_t scaled = (v * _m + (uint64_t(1) << 39)) >> 40;

        return (scaled < kWhite16) ? (unsigned)scaled : kWhite16;
    }

private:
    uint64_t _m;
};

// Scales the 16-bit RGB (RGB48LE) or RGBA (RGBA64LE) image so that white (R', G', B') and opaque (A) give 65535 (#316),
// rounded to the nearest.
void
ScaleToWhite16(const AVFrame* frame,
               unsigned white,
               unsigned opaque)
{
    const int nbComponents = av_pix_fmt_desc_get((AVPixelFormat)frame->format)->nb_components;
    const bool sameScale = (nbComponents == 3) || (white == opaque);
    const ScaleTo16 colour(white), alpha(opaque);
    for (int y = 0; y < frame->height; ++y) {
        uint8_t* row = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];
        const int samples = frame->width * nbComponents;
        if (sameScale) { // (the loop of most frames: no alpha)
            for (int i = 0; i < samples; ++i) {
                uint8_t* sample = row + (ptrdiff_t)i * 2;
                AV_WL16(sample, colour(AV_RL16(sample)));
            }
        } else {
            for (int i = 0; i < samples; ++i) {
                uint8_t* sample = row + (ptrdiff_t)i * 2;
                AV_WL16(sample, ((i & 3) == 3) ? alpha(AV_RL16(sample)) : colour(AV_RL16(sample)));
            }
        }
    }
}
} // namespace

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
    if (paramsChanged) {
        sws_freeContext(_convertCtx);
        _convertCtx = nullptr;
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
        srcPixelFormat = FFmpeg::nonJpegPixelFormat(srcPixelFormat);

        const bool yuv = isYUV();
        _convertCtx = CreateReaderSwsContext(srcPixelFormat, srcWidth, srcHeight, dstPixelFormat, dstWidth, dstHeight, yuv, srcRange, swsColorspace);
        const White16 white = _convertCtx ? MeasureWhite16(srcPixelFormat, srcWidth, dstPixelFormat, yuv, srcRange, swsColorspace) : White16 { kWhite16, kWhite16 };
        _convertCtxWhite16 = white.colour;
        _convertCtxOpaque16 = white.alpha;
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

// The most packets that a scan of the file at open reads (e.g. for a stream that has few packets)
static constexpr int kMaxOpenScanPackets = 1024;

// Whether the demuxer may give timestamp discontinuities (MPEG-TS and MPEG-PS). Their demuxers resync after a byte
// seek, and libavformat counts a duration estimated from their timestamps from the start time.
static bool
HasTsDiscontinuities(const AVFormatContext* context)
{
    return (context->iformat->flags & AVFMT_TS_DISCONT) != 0;
}

// Raises the timestamp v to e (v may be AV_NOPTS_VALUE: none yet)
static void
raiseTs(int64_t& v,
        int64_t e)
{
    if ((v == int64_t(AV_NOPTS_VALUE)) || (e > v)) {
        v = e;
    }
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
    return isMov && (avstream->pts_wrap_bits >= 63) && !HasTsDiscontinuities(context);
}

// The palette side data (AV_PKT_DATA_PALETTE) of the first packet of each palette stream (AV_PIX_FMT_FLAG_PAL), by
// stream index (empty if none), for the streams found by avformat_find_stream_info(). Some demuxers attach the
// palette only to the first packet (see Stream::attachPalette()), which avformat_find_stream_info() read and
// buffered: a seek drops it, and it is not read again (ff_read_frame_flush(), FFmpeg n9.0.2 libavformat/seek.c:716-744),
// so the first packets are read before anything seeks (only if the file has a palette stream; the next probes then
// seek, see probeStreamStart()). They come from that buffer, unless the palette stream starts late: then up to
// kMaxPackets packets are read from the file.
std::vector<std::vector<uint8_t>>
FFmpegFile::readFirstPalettes()
{
    std::vector<std::vector<uint8_t>> palettes(_context->nb_streams);
    std::vector<bool> pending(_context->nb_streams, false);
    int pendingStreams = 0;
    for (unsigned i = 0; i < _context->nb_streams; ++i) {
        const AVCodecParameters* par = _context->streams[i]->codecpar;
        const AVPixFmtDescriptor* desc = (par->codec_type == AVMEDIA_TYPE_VIDEO) ? av_pix_fmt_desc_get(AVPixelFormat(par->format)) : nullptr;
        if (desc && (desc->flags & AV_PIX_FMT_FLAG_PAL)) {
            pending[i] = true;
            ++pendingStreams;
        }
    }
    if (pendingStreams == 0) {
        return palettes;
    }
    _readAtOpen = true;
    MyAVPacket avPacket;
    for (int packets = 0; (pendingStreams > 0) && (packets < kMaxOpenScanPackets) && (av_read_frame(_context, avPacket.pkt()) >= 0); ++packets) {
        const int idx = avPacket->stream_index;
        // the streams found while reading (AVFMTCTX_NOHEADER, e.g. MPEG-TS) are not in palettes and pending
        if ((idx >= 0) && (size_t(idx) < pending.size()) && pending[idx]) {
            pending[idx] = false;
            --pendingStreams;
            Stream::keepPalette(*avPacket.pkt(), &palettes[idx]);
        }
        av_packet_unref(avPacket.pkt());
    }

    return palettes;
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
        stream.flushCodec();

        _readAtOpen = true;
        if (av_seek_frame(_context, stream._idx, startPTS, AVSEEK_FLAG_BACKWARD) >= 0) {

            // Read frames until we get one for the video stream that contains a valid PTS.
            while (readPacket(stream, avPacket.pkt()) >= 0) {
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
        StreamStartProbe probe = probeStreamStart(stream);
        if (probe.ptsOnFirstPacketOnly) {
            // The PTS of the first packet was filled in by libavformat before it knew the decoding delay (see
            // probeStreamStart()). Only the packets that avformat_find_stream_info() read and buffered have it: a
            // seek drops them and closes the parser (ff_read_frame_flush(), FFmpeg n9.1 libavformat/seek.c:713-741),
            // and the packets read again are parsed with the delay known (demux.c:1060-1062, 1123-1155). Probe once
            // more (probeStreamStart() now seeks, since _readAtOpen is true); only if the second probe sees the same
            // timestamps is the stream refused (see FFmpegFile::FFmpegFile()).
            probe = probeStreamStart(stream);
            stream._ptsOnFirstPacketOnly = probe.ptsOnFirstPacketOnly;
        }
        const int64_t firstDTS = probe.firstDTS;
        const int64_t firstFramePTS = probe.firstFramePTS;
        if (firstDTS != int64_t(AV_NOPTS_VALUE) && (firstDTS <= startPTS)) {
            stream._startDTS = firstDTS;
        }
        // Only if the first decoded frame is within the reordering delay of the first packet: a later one is
        // after frames that cannot be decoded (e.g. an MPEG-PS cut in the middle of a GOP, whose first packets
        // have no PTS), and whether the decoder outputs it within the packets read depends on its threads.
        const int64_t frameTicks = stream.frameTicks();
        if ((firstFramePTS != int64_t(AV_NOPTS_VALUE)) && (firstDTS != int64_t(AV_NOPTS_VALUE)) &&
            (firstFramePTS - firstDTS <= (stream._codecContext->has_b_frames + 1) * frameTicks)) {
#if TRACE_FILE_OPEN
            std::cout << "        Packets have no PTS, start from the first decoded frame=" << firstFramePTS << " ticks" << std::endl;
#endif
            startPTS = firstFramePTS;
            stream._startFromDecodedFrame = true;
        }
    }

    return startPTS;
} // FFmpegFile::getStreamStartTime

// Whether the packets after the first one, up to maxPackets of the stream, include one with a DTS and no PTS.
bool
FFmpegFile::nextPacketsLackPts(const Stream& stream, int maxPackets)
{
    MyAVPacket avPacket;
    while ((maxPackets > 0) && (readPacket(stream, avPacket.pkt()) >= 0)) {
        const bool inStream = (avPacket->stream_index == stream._idx);
        const bool lacksPts = inStream && (avPacket->pts == int64_t(AV_NOPTS_VALUE)) && (avPacket->dts != int64_t(AV_NOPTS_VALUE));
        av_packet_unref(avPacket.pkt());
        if (lacksPts) {
            return true;
        }
        if (inStream) {
            --maxPackets;
        }
    }

    return false;
}

// Whether the timestamps of the frames of the stream may be those of other frames (Stream::_timestampsMayShift, see
// Stream::armResync() and Stream::placeShiftedFrame()): in a format whose demuxer resumes reading anywhere after a seek
// (AVFMT_TS_DISCONT, e.g. MPEG-PS, MPEG-TS), if a demuxer packet holds several frames. The parser then splits it into
// frames, and only the frame in which it finds the start of the demuxer packet gets the timestamps of that packet
// (ff_fetch_timestamp(), libavcodec/parser.c:89-118 in FFmpeg 9.0.2, 84-113 in 8.0.1), which H.222.0 gives to the first
// frame that starts in it (section 2.4.3.7; a PTS is only needed every 0.7 s, section 2.7.4). Decided at open, from
// the first kShiftScanPackets packets of the stream, read with AVFMT_FLAG_NOFILLIN, so that libavformat leaves out the
// timestamps that it would compute for the other frames (compute_pkt_fields(), libavformat/demux.c:997-998 in 9.0.2,
// 981-982 in 8.0.1; those of frames that are not reordered are interpolated, e.g. MPEG-4 Part 2 without B-frames): a
// frame without one is such a frame. A damaged packet later in the file, e.g. a PES header without a PTS in an MPEG-TS
// whose PES packets each hold one frame, does not make the stream shift.
// Only the first kShiftScanPackets packets of the stream decide (and at most kMaxOpenScanPackets packets of the file are
// read, e.g. for a still picture next to a long audio track): a file whose first pictures each fill a demuxer packet,
// but whose later pictures share them, is not taken as shifting (issue #481).
static constexpr int kShiftScanPackets = 8;
bool
FFmpegFile::timestampsMayShift(Stream& stream)
{
    if (!HasTsDiscontinuities(_context)) {
        return false;
    }
    // A seek drops the packets that avformat_find_stream_info() kept, whose timestamps were filled in.
    _readAtOpen = true;
    if (seekToStreamStart(stream) < 0) {
        return false;
    }
    // (_context->flags restored on every path)
    struct FlagsRestore {
        AVFormatContext* context;
        int flags;
        ~FlagsRestore() { context->flags = flags; }
    } restore { _context, _context->flags };
    _context->flags |= AVFMT_FLAG_NOFILLIN;
    MyAVPacket avPacket;
    int packetsLeft = kShiftScanPackets;
    for (int packets = 0; (packetsLeft > 0) && (packets < kMaxOpenScanPackets) && (av_read_frame(_context, avPacket.pkt()) >= 0); ++packets) {
        const bool inStream = (avPacket->stream_index == stream._idx);
        const bool noTimestamp = (avPacket->pts == int64_t(AV_NOPTS_VALUE)) && (avPacket->dts == int64_t(AV_NOPTS_VALUE));
        av_packet_unref(avPacket.pkt());
        if (inStream) {
            if (noTimestamp) {
                return true;
            }
            --packetsLeft;
        }
    }

    return false;
}

// Seeks to read the first packets of the stream again (see probeStreamStart() and timestampsMayShift()), never with
// AVSEEK_FLAG_FRAME, which makes some demuxers (FLV) read the file from its start. In a format whose demuxer resumes
// reading anywhere (AVFMT_TS_DISCONT, e.g. MPEG-TS, MPEG-PS) and that allows byte seeks (not AVFMT_NO_BYTE_SEEK), to byte
// 0, which needs no binary search on the timestamps (ff_seek_frame_binary(), libavformat/seek.c:290-358 in FFmpeg 9.0.2
// and 8.0.1); otherwise to the start time, or to byte 0 if it is unknown (e.g. in elementary streams). Returns
// av_seek_frame()'s result.
int
FFmpegFile::seekToStreamStart(const Stream& stream)
{
    const bool byBytes = (stream._avstream->start_time == int64_t(AV_NOPTS_VALUE)) ||
                         (HasTsDiscontinuities(_context) && !(_context->iformat->flags & AVFMT_NO_BYTE_SEEK));
    return byBytes ? av_seek_frame(_context, -1, 0, AVSEEK_FLAG_BYTE) : av_seek_frame(_context, stream._idx, stream._avstream->start_time, AVSEEK_FLAG_BACKWARD);
}

// A stream whose packets have no timestamps at all, in a format without timestamps (AVFMT_NOTIMESTAMPS): e.g. raw
// H.264 and HEVC elementary streams, whose missing timestamps libavformat does not compute, unlike those of the other
// raw streams (compute_pkt_fields(), libavformat/demux.c:993-995 and 1101-1102 in FFmpeg n9.0.2, 977-979 and 1085-1086
// in n8.0.1). Neither the duration nor the timestamps then give the number of frames (it was 1), and libavformat cannot
// seek in it: its generic index (demux.c:1670-1673 in n9.0.2, 1640-1643 in n8.0.1) refuses entries without a timestamp
// (ff_add_index_entry(), libavformat/seek.c:76-77 in n9.0.2 and n8.0.1). The ffmpeg CLI gives each packet without a
// DTS the DTS of the previous one plus its duration (ist_dts_update(), fftools/ffmpeg_demux.c:316-372 in n9.0.2,
// 305-361 in n8.0.1), and ffprobe -count_frames counts the decoded frames.
// Every packet of the stream is read once, at open, from the first keyframe (the packets before it cannot be decoded,
// and are skipped, as a stream copy by the ffmpeg CLI does without -copyinkf): its byte position and the rank of its
// frame are recorded (Stream::_untimedPackets), and each keyframe is added to the index at the timestamp of its rank,
// which libavformat's generic seek then uses (seek_frame_generic(), libavformat/seek.c:526-595 in n9.0.2 and n8.0.1).
// readPacket() gives every packet read the DTS of its rank, found from its position: the frames are then placed as
// those of packets with a DTS and no PTS (e.g. AVI, see getStreamStartTime()). A raw parser gives a packet the position
// of the start of its frame (demux.c:1290-1294 in n9.0.2, 1264-1268 in n8.0.1), which is the same after a seek.
// A packet is a frame, or a field of a codec that codes the two fields of a frame separately (AV_CODEC_PROP_FIELDS, e.g.
// H.264 PAFF), whose decoder outputs a pair as one frame: the two fields of a frame have the same rank (see the pairing
// below). The field
// packets are told by a parser of the codec (AVCodecParserContext::picture_structure), run on each packet (a complete
// frame or field): the parser of the demuxer (av_stream_get_parser()) may already have parsed the next packets, which
// it keeps in a queue (parse_packet(), demux.c:1173-1332 in n9.0.2). A codec without that property (e.g. HEVC, whose
// parser gives the picture structure of the SEI, hevc/parser.c:72 in n9.0.2) decodes each field as a frame.
// Not applied if the stream has a start time, a packet has no position, or the first packet has a timestamp (e.g.
// MPEG-1/2, MPEG-4 Part 2 or MJPEG elementary streams, whose timestamps libavformat computes).
void
FFmpegFile::indexUntimedPackets(Stream& stream)
{
    // (a stream with timestamps has a start time: nothing is read for it, so that probeStreamStart() still reads the
    // packets that avformat_find_stream_info() kept)
    // (a byte seek "succeeds" without moving in an input that is not seekable, see seek(): nothing is read from one)
    if (!(_context->iformat->flags & AVFMT_NOTIMESTAMPS) || (stream._avstream->start_time != int64_t(AV_NOPTS_VALUE)) || !_context->pb ||
        !(_context->pb->seekable & AVIO_SEEKABLE_NORMAL)) {
        return;
    }
    _readAtOpen = true;
    if (seekToStreamStart(stream) < 0) {
        return;
    }
    // The parser that tells the fields of a frame (none if the codec does not code fields separately)
    const AVCodecParameters* par = stream._avstream->codecpar;
    const AVCodecDescriptor* desc = avcodec_descriptor_get(par->codec_id);
    std::unique_ptr<AVCodecParserContext, void (*)(AVCodecParserContext*)> parser(nullptr, av_parser_close);
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> parserContext(nullptr, [](AVCodecContext* c) { avcodec_free_context(&c); });
    if (desc && (desc->props & AV_CODEC_PROP_FIELDS)) {
        parser.reset(av_parser_init(par->codec_id));
        parserContext.reset(avcodec_alloc_context3(nullptr));
        if (!parser || !parserContext || (avcodec_parameters_to_context(parserContext.get(), par) < 0)) {
            parser.reset();
        } else {
            parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
        }
    }
    std::vector<Stream::UntimedPacket> packets;
    std::vector<size_t> keyframes; // the packets (indexes in packets) that start the frame of a keyframe
    int64_t frames = 0; // the frames complete so far: the rank of the next one
    // The parity of the first field of the current frame (AV_PICTURE_STRUCTURE_TOP_FIELD or _BOTTOM_FIELD), or
    // AV_PICTURE_STRUCTURE_UNKNOWN if none. As the decoder does (h264_field_start(), libavcodec/h264_slice.c:1573-1611 in
    // n9.0.2, 1572-1610 in n8.0.1), a field is the second field of the pending one only if it has the other parity and
    // the same frame number, which the parser tells by a field order (AVCodecParserContext::field_order, h264_parser.c:
    // 552-561 in n9.0.2, 547-556 in n8.0.1). Otherwise (a frame, or another field) the pending field is unmatched, and
    // not displayed: it is not counted, and has the rank of the next picture. Checked on FATE's CVFI1_Sony_D.jsv
    // without one field, not in the tests (no sample). Not followed: the decoder displays unmatched fields if several
    // follow each other (h264_slice.c:1687 in n9.0.2, 1682 in n8.0.1).
    int pendingField = AV_PICTURE_STRUCTURE_UNKNOWN;
    MyAVPacket avPacket;
    bool untimed = true;
    while (untimed && (av_read_frame(_context, avPacket.pkt()) >= 0)) {
        if (avPacket->stream_index == stream._idx) {
            untimed = (avPacket->pts == int64_t(AV_NOPTS_VALUE)) && (avPacket->dts == int64_t(AV_NOPTS_VALUE)) && (avPacket->pos >= 0) &&
                      (packets.empty() || (avPacket->pos > packets.back().pos));
            if (untimed && (!packets.empty() || (avPacket->flags & AV_PKT_FLAG_KEY))) {
                int structure = AV_PICTURE_STRUCTURE_FRAME;
                if (parser) {
                    uint8_t* out = nullptr;
                    int outSize = 0;
                    av_parser_parse2(parser.get(), parserContext.get(), &out, &outSize, avPacket->data, avPacket->size,
                                     AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    if ((parser->picture_structure == AV_PICTURE_STRUCTURE_TOP_FIELD) ||
                        (parser->picture_structure == AV_PICTURE_STRUCTURE_BOTTOM_FIELD)) {
                        structure = parser->picture_structure;
                    }
                }
                const bool secondField = (pendingField != AV_PICTURE_STRUCTURE_UNKNOWN) && (structure != AV_PICTURE_STRUCTURE_FRAME) &&
                                         (structure != pendingField) &&
                                         ((parser->field_order == AV_FIELD_TT) || (parser->field_order == AV_FIELD_BB));
                const bool startsFrame = !secondField; // (the pending field, if any, is unmatched)
                if (startsFrame && (avPacket->flags & AV_PKT_FLAG_KEY) && (keyframes.empty() || (packets[keyframes.back()].rank < frames))) {
                    keyframes.push_back(packets.size());
                }
                packets.push_back({ avPacket->pos, frames });
                if (structure == AV_PICTURE_STRUCTURE_FRAME) {
                    pendingField = AV_PICTURE_STRUCTURE_UNKNOWN;
                    ++frames;
                } else if (startsFrame) {
                    pendingField = structure; // a first field
                } else {
                    pendingField = AV_PICTURE_STRUCTURE_UNKNOWN; // the second field of the frame
                    ++frames;
                }
            }
        }
        av_packet_unref(avPacket.pkt());
    }
    if (!untimed || (frames == 0)) {
        return;
    }
    for (size_t i : keyframes) {
        if (packets[i].rank < frames) { // (not a last field that is unmatched)
            av_add_index_entry(stream._avstream, packets[i].pos, stream.untimedPacketDts(packets[i].rank), 0, 0, AVINDEX_KEYFRAME);
        }
    }
    stream._untimedPackets = std::move(packets);
    stream._untimedFrames = frames;
    stream._seekFallsBackToStart = true;
}

int
FFmpegFile::readPacket(const Stream& stream,
                       AVPacket* pkt)
{
    for (;;) {
        const int res = av_read_frame(_context, pkt);
        if ((res < 0) || (pkt->stream_index != stream._idx) || stream._untimedPackets.empty() ||
            (pkt->pts != int64_t(AV_NOPTS_VALUE)) || (pkt->dts != int64_t(AV_NOPTS_VALUE))) {
            return res;
        }
        const std::vector<Stream::UntimedPacket>& packets = stream._untimedPackets;
        const auto it = std::lower_bound(packets.begin(), packets.end(), pkt->pos,
                                         [](const Stream::UntimedPacket& p, int64_t pos) { return p.pos < pos; });
        if ((it != packets.end()) && (it->pos == pkt->pos)) {
            pkt->dts = stream.untimedPacketDts(it->rank);
            return res;
        }
        if (pkt->pos >= packets.front().pos) {
            return res; // (not a packet read at open: left without a timestamp)
        }
        av_packet_unref(pkt); // before the first keyframe
    }
}

// Read the first packets of the stream (from the start time, seeking if anything was read since
// avformat_find_stream_info()), for a stream whose decoder delays frames (has_b_frames > 0):
// - firstDTS: the DTS of the first packet;
// - firstFramePTS: if the first packet has a DTS but no PTS (e.g. AVI), the timestamp of the first decoded
//   frame (best_effort_timestamp), else AV_NOPTS_VALUE;
// - ptsOnFirstPacketOnly: the first packet has PTS == DTS, and a next one a DTS but no PTS. The decoding delay
//   is then learned from the decoder, not the parser, in a container without PTS (e.g. MPEG-1 in AVI, whose
//   sequence header has no low_delay flag: the parser only reads it in the MPEG-2 sequence extension, FFmpeg
//   n9.1 libavcodec/mpegvideo_parser.c:164, and the decoder sets has_b_frames, n9.1 libavcodec/mpeg12dec.c:949;
//   or VC-1/WMV3 in AVI). libavformat filled in PTS = DTS on the first packet, as for a frame that is not delayed,
//   before it knew the delay, and gives the next delayed frames none (n9.1 libavformat/demux.c:1123-1170): the
//   first frame would be one frame before the next ones, which are at the DTS of the packet after which they are
//   output, and a frame time would have no frame.
FFmpegFile::StreamStartProbe
FFmpegFile::probeStreamStart(Stream& stream)
{
    StreamStartProbe probe;

    stream.flushCodec();
    // If nothing was read or sought since avformat_find_stream_info(), the first packets are those it kept,
    // from the start of the file: no seek is needed. Otherwise, seek to the start (seekToStreamStart()).
    if (_readAtOpen && (seekToStreamStart(stream) < 0)) {
        return probe;
    }
    _readAtOpen = true;
    MyAVPacket avPacket;
    AVFrame* avFrame = av_frame_alloc();
    bool firstPacket = true;
    bool eof = false;
    // Enough packets for the decoder to output a frame (its delay, with frame threads), but bounded.
    int packetsLeft = stream.getCodecDelay() + 16;
    while (avFrame && !eof && (packetsLeft > 0)) {
        int res = readPacket(stream, avPacket.pkt());
        if (res < 0) {
            eof = true;
        } else if (avPacket->stream_index != stream._idx) {
            av_packet_unref(avPacket.pkt());
            continue;
        } else if (firstPacket) {
            firstPacket = false;
            probe.firstDTS = avPacket->dts;
            if ((avPacket->pts != int64_t(AV_NOPTS_VALUE)) || (avPacket->dts == int64_t(AV_NOPTS_VALUE))) {
                // the packets have a PTS (or no timestamp at all): nothing to decode, unless only the first one has
                // a PTS (see above)
                const bool ptsIsDts = (avPacket->pts != int64_t(AV_NOPTS_VALUE)) && (avPacket->pts == avPacket->dts);
                av_packet_unref(avPacket.pkt());
                probe.ptsOnFirstPacketOnly = ptsIsDts && nextPacketsLackPts(stream, packetsLeft);
                break;
            }
        }
        if (firstPacket) {
            break; // no packet in the stream
        }
        --packetsLeft;
        if (!eof) {
            stream.attachPalette(avPacket.pkt());
        }
        res = avcodec_send_packet(stream._codecContext, eof ? nullptr : avPacket.pkt());
        av_packet_unref(avPacket.pkt());
        if ((res < 0) && (res != AVERROR(EAGAIN)) && (res != AVERROR_INVALIDDATA)) {
            break;
        }
        if (avcodec_receive_frame(stream._codecContext, avFrame) >= 0) {
            probe.firstFramePTS = avFrame->best_effort_timestamp;
            break;
        }
    }
    av_frame_free(&avFrame);
    stream.flushCodec();

    return probe;
} // FFmpegFile::probeStreamStart

// The number of frames of the stream that are not displayed (marked as discarded in the index, e.g. by an
// MOV/MP4 edit list).
int64_t
FFmpegFile::getDiscardedFrames(Stream& stream)
{
    int64_t discarded = 0;
    const int entries = avformat_index_get_entries_count(stream._avstream);
    for (int i = 0; i < entries; ++i) {
        const AVIndexEntry* entry = avformat_index_get_entry(stream._avstream, i);
        if (entry && (entry->flags & AVINDEX_DISCARD_FRAME)) {
            ++discarded;
        }
    }

    return discarded;
}


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

// Whether h is the header of an FLV tag of tagSize bytes (the previous tag size that follows the tag): an audio (8),
// video (9) or script data (18) tag, whose data size is tagSize - 11
static bool
isFlvTagHeader(const unsigned char* h,
               int64_t tagSize)
{
    const int type = h[0] & 0x1f;

    return ((type == 8) || (type == 9) || (type == 18)) && (int64_t(AV_RB24(h + 1)) + 11 == tagSize);
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
            const int64_t previousTagSize = AV_RB32(p);
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
                    if ((avio_seek(pb, tagStart, SEEK_SET) < 0) || (avio_read(pb, header, 4) != 4) || !isFlvTagHeader(header, previousTagSize)) {
                        return -1;
                    }
                }
                break;
            }
            const unsigned char* t = at(tagStart);
            if (!isFlvTagHeader(t, previousTagSize)) {
                if (tag == size) {
                    return -1; // the last tag is not valid
                }
                break;
            }
            const int64_t dataSize = previousTagSize - 11;
            if (((t[0] & 0x1f) == 9) && (dataSize >= 1) && isFlvVideoFrame(t + 11, dataSize)) {
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

// The end of the movie given by a duration that libavformat estimated from the timestamps (AVFMT_DURATION_FROM_PTS),
// in AV_TIME_BASE units. videoStart is the start of the video stream, used when the start time is unknown, and
// frameDuration the duration of a video frame.
// With timestamp discontinuities (MPEG-TS and MPEG-PS), the duration is counted from the start time, up to the end
// of the last packet. Otherwise (NUT), it is the largest PTS, counted from 0: the start of the last packet, which
// lasts one frame. The largest PTS may also be that of an audio packet, and the result may then be one frame long.
// Without an index, the NUT duration is the PTS of the last syncpoint. When the probe fails (a GOP longer than the
// probe), the result may then be short with B-frames: e.g. 58 of 60 frames for a 640x480 noise clip encoded with
// "libx264 -preset veryfast -qp 8 -bf 3 -g 1000 -x264-params scenecut=0" (one keyframe) and -write_index 0.
// A duration that does not reach the start cannot be counted from 0, and is then counted from the start.
static int64_t
TimestampDurationEnd(const AVFormatContext* context,
                     int64_t videoStart,
                     int64_t frameDuration)
{
    const int64_t start = (context->start_time == int64_t(AV_NOPTS_VALUE)) ? videoStart : context->start_time;
    if (HasTsDiscontinuities(context) || (context->duration <= start)) {
        return start + context->duration;
    }

    return context->duration + frameDuration;
}

// Read the last packets of the file, to get the end of the movie (the largest PTS + duration of the packets of all
// streams, in AV_TIME_BASE units) and the largest PTS of the video stream (in its time base). This reads at most
// kMaxEndProbeBytes. The demuxer is positioned in one of these ways:
// - through its index (the MOV/MP4 samples, the Matroska cues);
// - in FLV, at one of the last tags;
// - when the duration was estimated from the timestamps, near the end that this duration gives (see below).
// Returns false if they cannot be read (e.g. the input is not seekable, the index does not reach the end, or the
// last FLV tag is not valid, as in a truncated file), and the caller then uses the duration.
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
    const char* formatName = _context->iformat->name;
    const bool isFlv = (std::strcmp(formatName, "flv") == 0) || (std::strcmp(formatName, "live_flv") == 0);
    const bool isMatroska = std::strstr(formatName, "matroska") != nullptr;
    _readAtOpen = true;
    // A byte seek goes to the last 1 MB. If that has too few video packets (e.g. at a high bit rate), the probe
    // seeks again, to the last 4 MB.
    int64_t byteSeekWindow = int64_t(1) << 20;
    auto seekToLastBytes = [&]() -> bool {
        return av_seek_frame(_context, -1, (std::max)(int64_t(0), size - byteSeekWindow), AVSEEK_FLAG_BYTE) >= 0;
    };
    // When the duration was estimated from the timestamps, the formats with timestamp discontinuities (MPEG-TS and
    // MPEG-PS) resync after a byte seek, so the probe reads their last bytes. Their index, if any, does not reach
    // the end: it only has the entries that the demuxer added while reading.
    // The other formats (NUT) may not resync forward after a byte seek. The NUT demuxer goes back to the last
    // syncpoint that it knows instead. So the probe seeks by timestamp, to the end that the duration gives: the
    // demuxer finds the syncpoint before it through its index, or by a search without one.
    const int64_t frameDuration = stream.frameDuration();
    const bool durationFromPts = _context->duration_estimation_method == AVFMT_DURATION_FROM_PTS;
    const bool seekByBytes = durationFromPts && HasTsDiscontinuities(_context);
    if (isFlv) {
        // Enough video tags for the reordered frames (with B-frames, the last tag is not the last frame).
        const int64_t pos = FlvLastTagsStart(pb, size, kMaxEndProbeBytes);
        if ((pos < 0) || (av_seek_frame(_context, -1, pos, AVSEEK_FLAG_BYTE) < 0)) {
            return false;
        }
    } else if (seekByBytes) {
        if (!seekToLastBytes()) {
            return false;
        }
    } else if (durationFromPts) {
        const int64_t videoStart = av_rescale_q(stream._startPTS, stream._avstream->time_base, AV_TIME_BASE_Q);
        const int64_t target = av_rescale_q(TimestampDurationEnd(_context, videoStart, frameDuration), AV_TIME_BASE_Q, stream._avstream->time_base);
        if (av_seek_frame(_context, stream._idx, target, AVSEEK_FLAG_BACKWARD) < 0) {
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
        const int entries = avformat_index_get_entries_count(stream._avstream);
        const bool isMov = std::strstr(formatName, "mov") != nullptr;
        int entry = entries - 1;
        if (isMov) {
            entry -= stream._codecContext->has_b_frames + 4;
        }
        if (entries < 1) {
            return false;
        }
        entry = (std::max)(0, entry);
        const AVIndexEntry* indexEntry = avformat_index_get_entry(stream._avstream, entry);
        if (!indexEntry || (indexEntry->pos < size - kMaxEndProbeBytes) || (indexEntry->timestamp == int64_t(AV_NOPTS_VALUE))) {
            return false;
        }
        if (av_seek_frame(_context, stream._idx, indexEntry->timestamp, AVSEEK_FLAG_BACKWARD | (isMov ? AVSEEK_FLAG_ANY : 0)) < 0) {
            return false;
        }
    }
    int64_t end = AV_NOPTS_VALUE;
    int64_t videoEnd = AV_NOPTS_VALUE;
    bool eof = false;
    int videoPackets = 0;
    int64_t bytesRead0 = pb->bytes_read;
    bool refPending = false; // a video packet without a PTS was read (MPEG-PS reference frame)
    int unplacedRefs = 0; // the video packets without a PTS since the last one with a PTS
    const int64_t frameTicks = stream.frameTicks();
    // The streams of the program of the video stream (all of them if there are no programs)
    const AVProgram* program = av_find_program_from_stream(_context, nullptr, stream._idx);
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
        const int res = readPacket(stream, avPacket.pkt());
        if ((res == AVERROR_EOF) && seekByBytes && (videoPackets < 8) && (byteSeekWindow < kMaxEndProbeBytes) && (byteSeekWindow < size)) {
            // too few video packets in the last bytes: read more
            byteSeekWindow = kMaxEndProbeBytes;
            end = videoEnd = lastVideoPts = AV_NOPTS_VALUE;
            videoPackets = 0;
            refPending = false;
            unplacedRefs = 0;
            bytesRead0 = pb->bytes_read;
            if (!seekToLastBytes()) {
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
            raiseTs(end, e);
            raiseTs(videoEnd, e);
            raiseTs(lastVideoPts, refPts);
        }
        if (isVideoStream && (avPacket->pts != int64_t(AV_NOPTS_VALUE))) {
            unplacedRefs = 0;
        }
        const int64_t ts = (avPacket->pts != int64_t(AV_NOPTS_VALUE)) ? avPacket->pts : avPacket->dts;
        if ((ts != int64_t(AV_NOPTS_VALUE)) && !(avPacket->flags & AV_PKT_FLAG_DISCARD)) {
            int64_t duration = avPacket->duration;
            // A video packet without a duration lasts one frame of its stream. The frame rate is
            // av_guess_frame_rate()'s, as in FFmpegFile(), not r_frame_rate, which may be a field rate.
            if ((duration <= 0) && (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)) {
                if (isVideoStream) {
                    duration = av_rescale_q(frameDuration, AV_TIME_BASE_Q, st->time_base);
                } else {
                    const AVRational rate = av_guess_frame_rate(_context, _context->streams[avPacket->stream_index], nullptr);
                    if ((rate.num > 0) && (rate.den > 0)) {
                        duration = av_rescale_q(1, av_inv_q(rate), st->time_base);
                    }
                }
            }
            const int64_t e = av_rescale_q(ts + (std::max)(int64_t(0), duration), st->time_base, AV_TIME_BASE_Q);
            raiseTs(end, e);
            if (isVideoStream) {
                ++videoPackets;
                raiseTs(videoEnd, e);
                if (avPacket->pts != int64_t(AV_NOPTS_VALUE)) {
                    raiseTs(lastVideoPts, avPacket->pts);
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
        raiseTs(end, e);
        raiseTs(videoEnd, e);
    }
    if (seekByBytes && (videoPackets <= stream._codecContext->has_b_frames)) {
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
    // (for tests: reads the index without _lock, so no decode() must run meanwhile)
    const Stream* stream = getStream(0);
    if (!stream) {
        return -1;
    }
    AVStream* st = stream->_avstream;
    int64_t last = AV_NOPTS_VALUE;
    const int entries = avformat_index_get_entries_count(st);
    for (int i = 0; i < entries; ++i) {
        const AVIndexEntry* e = avformat_index_get_entry(st, i);
        if (e && (e->timestamp != int64_t(AV_NOPTS_VALUE))) {
            raiseTs(last, e->timestamp);
        }
    }

    return (last == int64_t(AV_NOPTS_VALUE)) ? -1 : stream->ptsToFrame(last);
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
    // A format without timestamps (AVFMT_NOTIMESTAMPS, e.g. a raw elementary stream) whose duration libavformat
    // estimated from the bit rate (FFmpeg n9.0.2 libavformat/demux.c:2054-2057, also the stream duration,
    // demux.c:1845-1858) may be far off (e.g. 1 ms for 40 frames, with the VBR bit rate value of MPEG-1): neither
    // duration is used, and its frames are counted from the timestamps of the packets (below), which the parser
    // gives them.
    const bool durationFromBitRateOnly = (_context->iformat->flags & AVFMT_NOTIMESTAMPS) && (_context->duration_estimation_method == AVFMT_DURATION_FROM_BITRATE);
    if (frames <= 0 && _context->duration > 0 && !durationFromBitRateOnly) {
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
        const char* formatName = _context->iformat->name;
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
        // The number of samples of the stream that are displayed (in MOV/MP4, the frames before the start of the edit
        // list, e.g. to decode the first displayed frame, are counted in AVStream::nb_frames, but the demuxer marks
        // them as discarded in the index), or 0 if unknown
        const int64_t streamFrames = (stream._avstream->nb_frames > 0) ? (stream._avstream->nb_frames - getDiscardedFrames(stream)) : 0;
        // A probed end is not trusted far from the duration (e.g. a damaged last FLV tag, or a timestamp
        // discontinuity): it must be within 10 % of the duration, and at least 2 s.
        const int64_t probeTolerance = (std::max)(_context->duration / 10, int64_t(2) * AV_TIME_BASE);
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
            if (!probeEnd(stream, movieEnd, lastVideoPts) || (movieEnd > durationEnd + probeTolerance)) {
                movieEnd = durationEnd;
            }
        } else if (_context->duration_estimation_method == AVFMT_DURATION_FROM_PTS) {
            // The duration was estimated from the timestamps (#215, #331). For MPEG-TS and MPEG-PS, libavformat
            // reads it from the last packets, and may miss the last video frames (e.g. at a high bit rate). For
            // NUT, it is the largest PTS: the start of the last packet, not its end. Without an index, it is the
            // PTS of the last syncpoint. So the end (PTS + duration) is read from the last packets.
            // With timestamp discontinuities, the probed end is only trusted near the estimate.
            movieEnd = TimestampDurationEnd(_context, videoStart, stream.frameDuration());
            int64_t probedEnd, lastVideoPts;
            if (probeEnd(stream, probedEnd, lastVideoPts)) {
                const bool nearEstimate = std::abs(double(probedEnd - movieEnd)) <= double(probeTolerance);
                if (!HasTsDiscontinuities(_context) || nearEstimate) {
                    movieEnd = probedEnd;
                }
            }
        } else if (_context->start_time == int64_t(AV_NOPTS_VALUE)) {
            // the start is unknown: count the duration from the start of the video stream
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
            // MOV/MP4: the duration is counted from the start. Count it from 0 only if that gives the number of
            // samples of the stream (minus the ones that are not displayed) and counting it from the start does not
            // (with B-frames and no edit list, e.g. in fragmented MP4, the start time is the B-frame delay, and both
            // may).
            const int64_t fromStart = _context->start_time + _context->duration;
            const int64_t fromZero = _context->duration;
            const bool startMatches = (streamFrames > 0) && (std::abs(framesUntil(fromStart) - streamFrames) <= 1);
            const bool zeroMatches = (streamFrames > 0) && (std::abs(framesUntil(fromZero) - streamFrames) <= 1);
            movieEnd = fromStart;
            if (zeroMatches && !startMatches) {
                movieEnd = fromZero;
            }
        } else {
            movieEnd = _context->start_time + _context->duration;
            if (stream._startFromDecodedFrame) {
                // The packets have no PTS (AVI): the duration is counted from the first DTS, and the decoded
                // frames, counted from the first one, are delayed by the decoder (see getStreamStartTime()), by more
                // than one frame with H.264 B-frames (has_b_frames is 2 with B-pyramids).
                movieEnd += av_rescale_q(stream._startPTS - stream._startDTS, stream._avstream->time_base, AV_TIME_BASE_Q);
            }
        }
        frames = framesUntil(movieEnd);

        // The above calculation is not reliable, because it seems in some situations (such as rendering out a mov
        // with 5 frames at 24 fps from Nuke) the duration has been rounded up to the nearest millisecond, which
        // leads to an extra frame being reported.  To attempt to work around this, compare against the number of
        // frames in the stream (streamFrames, without the frames that are not displayed), and if they differ by one,
        // use that value instead.
        // But a stream may have fewer samples than frame times (a gap in the timestamps, e.g. after a stream copy
        // from a non-keyframe, or VFR): when snapping down, the count must still reach the last sample (its PTS,
        // from the last packets), which may also be after the end given by the duration.
        if ((streamFrames > 0) && (std::abs(frames - streamFrames) <= 1)) {
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

    // A stream without timestamps (see indexUntimedPackets())
    if (frames <= 0) {
        frames = stream._untimedFrames;
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
    if (frames <= 0 && stream._avstream->duration > 0 && !durationFromBitRateOnly) {
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

        // Seek last key-frame. If that fails, read from the start, and decode from the start when a seek fails later
        // (Stream::_seekFallsBackToStart). This path is for the streams without a duration or a number of frames:
        // elementary streams whose timestamps libavformat computes (e.g. MPEG-1/2, MPEG-4 Part 2, MJPEG, whose duration
        // estimated from the bit rate is not used, see above) and image pipes. libavformat's generic seek
        // (seek_frame_generic(), FFmpeg n9.0.2 and n8.0.1 libavformat/seek.c:526-595), which their demuxers use, reads
        // the file up to its end to index it, and fails if the index entry it would seek to has no position, which
        // left the demuxer at the end of the file: a PNG image pipe was counted as 1 frame. The image pipe demuxer
        // reads 4096-byte chunks, which the parser assembles into frames (libavformat/img2dec.c:200 and 452-458 in
        // n9.0.2): a frame gets a position only when a new chunk started since the previous frame (the position of that
        // chunk), and small frames, several per chunk, get none (ff_fetch_timestamp(), libavcodec/parser.c:99-111 in
        // n9.0.2, 94-106 in n8.0.1).
        // Counted from the start, a stream whose last reference frame has no PTS (with B-frames) may count one frame
        // more (see below), which is better than one frame.
        _readAtOpen = true;
        stream.flushCodec();
        bool positioned = av_seek_frame(_context, stream._idx, stream.frameToPts(1 << 29), AVSEEK_FLAG_BACKWARD) >= 0;
        if (!positioned) {
            stream._seekFallsBackToStart = true;
            positioned = seekToStreamStart(stream) >= 0;
        }

        // Read up to last frame, extending max PTS for every valid PTS value found for the video stream.
        MyAVPacket avPacket;

        // A packet without a PTS (a reference frame in an MPEG-2 elementary stream with B-frames, or in AVI) is
        // displayed at least at its DTS, delayed as the timestamps of the decoded frames (see
        // getStreamStartTime()), and after the frames that follow it in decoding order until the next reference
        // frame (B-frames): e.g. the last reference frame is the last frame displayed.
        const int64_t frameTicks = stream.frameTicks();
        bool refPending = false; // a packet without a PTS was read
        while (positioned && (readPacket(stream, avPacket.pkt()) >= 0)) {
            if (avPacket->stream_index == stream._idx) {
                int64_t pts = avPacket->pts;
                if ((pts == int64_t(AV_NOPTS_VALUE)) && (avPacket->dts != int64_t(AV_NOPTS_VALUE))) {
                    pts = (std::max)(avPacket->dts + (stream._startPTS - stream._startDTS), maxPts + frameTicks);
                    refPending = true;
                } else if ((pts != int64_t(AV_NOPTS_VALUE)) && refPending && (pts + frameTicks > maxPts)) {
                    // the pending reference frame is displayed after this one
                    maxPts = pts + frameTicks;
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
    const AVCodecParameters* codecA = streamA->codecpar;
    const AVCodecParameters* codecB = streamB->codecpar;

    // Sanity check //
    if (codecA == nullptr || codecB == nullptr) {
        return false;
    }

    AVPixelFormat codecAfmt = (AVPixelFormat)codecA->format;
    AVPixelFormat codecBfmt = (AVPixelFormat)codecB->format;

    // Not matching even if both reads failed
    if (codecAfmt == AV_PIX_FMT_NONE || codecBfmt == AV_PIX_FMT_NONE) {
        return false;
    }

    const AVPixFmtDescriptor* pixFmtDescA = av_pix_fmt_desc_get(codecAfmt);
    const AVPixFmtDescriptor* pixFmtDescB = av_pix_fmt_desc_get(codecBfmt);

    return (codecA->codec_id == codecB->codec_id) && (codecA->bits_per_raw_sample == codecB->bits_per_raw_sample) && (codecA->width == codecB->width) && (codecA->height == codecB->height) && (codecA->sample_aspect_ratio.num == codecB->sample_aspect_ratio.num) && (codecA->sample_aspect_ratio.den == codecB->sample_aspect_ratio.den) && (pixFmtDescA->nb_components == pixFmtDescB->nb_components) && (streamA->sample_aspect_ratio.num == streamB->sample_aspect_ratio.num) && (streamA->sample_aspect_ratio.den == streamB->sample_aspect_ratio.den) && (streamA->time_base.num == streamB->time_base.num) && (streamA->time_base.den == streamB->time_base.den) && (streamA->start_time == streamB->start_time) && (streamA->duration == streamB->duration) && (streamA->nb_frames == streamB->nb_frames) && (streamA->r_frame_rate.num == streamB->r_frame_rate.num) && (streamA->r_frame_rate.den == streamB->r_frame_rate.den);
}

// Whether the codec only has keyframes (see below)
static bool IsIntraOnly(const AVCodecContext* ctx);

// constructor
FFmpegFile::FFmpegFile(const string& filename)
    : _filename(filename)
    , _context(nullptr)
    , _streams()
    , _demuxedStream(nullptr)
    , _errorMsg()
    , _invalidState(false)
    , _seekCount(0)
    , _startSeekCount(0)
    , _bytesReadAtOpen(0)
    , _readAtOpen(false)
    , _damagedPacketWarned(false)
    , _packetsEnd(AV_NOPTS_VALUE)
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
    // Scan all the program map tables, as ffmpeg, ffprobe and ffplay do (FFmpeg n9.0.2 fftools/ffmpeg_demux.c:
    // 2163-2166, fftools/ffprobe.c:2574-2577, fftools/ffplay.c:2916-2919): otherwise avformat_find_stream_info()
    // may leave the average frame rate unknown (e.g. MPEG-1 video in MPEG-TS), and av_guess_frame_rate() (see
    // below) cannot tell a field rate from a frame rate. Like enable_drefs, the option is ignored by the demuxers
    // that do not have it.
    av_dict_set(&demuxerOptions, "scan_all_pmts", "1", 0);
    const int openError = avformat_open_input(&_context, _filename.c_str(), nullptr, &demuxerOptions);
    // On success, demuxerOptions is destroyed and replaced with a dict containing the options that were not found
    // (this is OK: enable_drefs is a valid option only for mov/mp4/3gp, https://ffmpeg.org/ffmpeg-formats.html#Options-1,
    // and scan_all_pmts only for MPEG-TS).
    // On failure, avformat_open_input leaves it as is (libavformat/demux.c, avformat_open_input(), "fail:" frees only
    // its copy). Free it in both cases.
    av_dict_free(&demuxerOptions);
    CHECK(openError);
    assert(_context);
    // Bug 51016 - probesize is the maximum amount of data, in bytes, which is read to determine
    // frame information in avformat_find_stream_info. It's important that when reading
    // stereo quicktimes that the probe size be large enough to read data from both video tracks.
    // Otherwise the streams could be reported as having different properties, when really they're
    // the same but due to an insufficient probesize the second stream didn't have all the relevant data
    // loaded. This defaults to 5meg. 100meg should be enough for large stereo quicktimes.
    _context->probesize = 100000000;

    CHECK(avformat_find_stream_info(_context, nullptr));
    const std::vector<std::vector<uint8_t>> firstPalettes = readFirstPalettes();

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
            codecCtx->thread_count = (std::min)((int)MultiThread::getNumCPUs(), OFX_FFMPEG_MAX_THREADS); // ask for the number of available cores for multithreading
            // The thread type is FFmpeg's default (frame and slice threads, as the codec supports them).
            // Slice threads only were asked here for codecs that support them, but the test was always
            // false (codecCtx->codec is set by avcodec_open2()), and frame threads have been used.
        }

        // skip if the codec can't be open
        if (avcodec_open2(codecCtx, videoCodec, nullptr) < 0) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" failed to open, skipping..." << std::endl;
#endif
            unsupported_codec = true;
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
        std::unique_ptr<Stream> stream = std::make_unique<Stream>(); // (owns the codec context from here)
        stream->_idx = i;
        if (i < firstPalettes.size()) { // not a stream found after readFirstPalettes()
            stream->_palette = firstPalettes[i];
        }
        stream->_avstream = avstream;
        stream->_codecContext = codecCtx;
        stream->_videoCodec = videoCodec;
        stream->_intraOnlyCodec = IsIntraOnly(codecCtx);
        stream->_avFrame = av_frame_alloc(); // avcodec_alloc_frame();
        stream->_avIntermediateFrame = av_frame_alloc();
        stream->_avHeldFrame = av_frame_alloc();
        stream->_avNextFrame = av_frame_alloc();
        stream->_pendingPacket = av_packet_alloc();

        {
            const int bitDepth = FFmpeg::pixelFormatMaxComponentDepth(stream->_codecContext->pix_fmt);
            if (bitDepth == 0) {
                unsupported_codec = true; // no descriptor, or no component (an opened decoder has a known pixel format)
                continue;
            }
            stream->_bitDepth = bitDepth;
            // The output is RGBA if the pixel format has an alpha channel (AV_PIX_FMT_FLAG_ALPHA), else RGB. This is
            // the flag, not the component count: the palette of PAL8 has an alpha channel, but its descriptor has 1
            // component (the index), and gray + alpha (ya8, ya16) has 2 (FFmpeg n9.0.2 libavutil/pixdesc.h:138-147,
            // libavutil/pixdesc.c:427-436 and 1836-1863). Every descriptor with 4 components has the flag.
            stream->_numberOfComponents = FFmpeg::pixelFormatAlpha(stream->_codecContext->pix_fmt) ? 4 : 3;
        }

        if (stream->_bitDepth > 8) {
            // TODO: This will sometimes be sub-optimal for the timeline, which can deal directly with 3 x 10 bit pixel formats.
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGB48LE; // 16-bit.
        } else {
            stream->_outputPixelFormat = (4 == stream->_numberOfComponents) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_RGB24; // 8-bit
        }
        // ReadFFmpeg sizes the decode buffer of every view from the first stream (its bit depth and component count,
        // which give the output pixel format): a stream that decodes to another output pixel format (e.g. a 10-bit
        // stream after an 8-bit one) is not a view.
        if (!_streams.empty() && (stream->_outputPixelFormat != _streams[0]->_outputPixelFormat)) {
#if TRACE_FILE_OPEN
            std::cout << "Output pixel format differs from that of the first video stream, ignoring this stream." << std::endl;
#endif
            continue;
        }
#if TRACE_FILE_OPEN
        std::cout << "      Timebase=" << avstream->time_base.num << "/" << avstream->time_base.den << " s/tick" << std::endl;
        std::cout << "      Duration=" << avstream->duration << " ticks, " << double(avstream->duration) * double(avstream->time_base.num) / double(avstream->time_base.den) << " s" << std::endl;
        std::cout << "      BitDepth=" << stream->_bitDepth << std::endl;
        std::cout << "      NumberOfComponents=" << stream->_numberOfComponents << std::endl;
#endif

        // Indexes of formats with timestamp discontinuities cannot be trusted (see Stream::decodeForwardIsCheaper()).
        stream->_indexReliable = !HasTsDiscontinuities(_context);

        // The frame rate is av_guess_frame_rate()'s (1 fps, the default value, if it is unknown), as in ffmpeg
        // and ffplay (FFmpeg n9.0.2 fftools/ffmpeg_demux.c:1085, fftools/ffplay.c:1910): AVStream::r_frame_rate
        // may be the field rate of a codec that can code fields (AV_CODEC_PROP_FIELDS), e.g. twice the frame rate
        // of progressive MPEG-1 or H.264 (libavformat/demux.c:3061-3068), and av_guess_frame_rate() then gives
        // the codec frame rate (libavformat/avformat.c:807-829). ffprobe shows r_frame_rate as is.
        const AVRational frameRate = av_guess_frame_rate(_context, avstream, nullptr);
        if ((frameRate.num > 0) && (frameRate.den > 0)) {
            stream->_fpsNum = frameRate.num;
            stream->_fpsDen = frameRate.den;
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
        stream->_aspect = Stream::GetStreamAspectRatio(stream.get());

        // set stream start time and numbers of frames
        indexUntimedPackets(*stream);
        stream->_startPTS = getStreamStartTime(*stream);
        if (stream->_ptsOnFirstPacketOnly) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" in format \"" << _context->iformat->name << "\" disallowed because only the first packet has a PTS, skipping..." << std::endl;
#endif
            unsupported_codec = true;
            continue;
        }
        stream->_frames = getStreamFrames(*stream);
        stream->_timestampsMayShift = timestampsMayShift(*stream);

        // save the stream
        _streams.push_back(std::move(stream));
    }
    // A truncated file whose index (e.g. MOV/MP4 with the index at the start) has samples beyond the end of the
    // file: the frames from the first of them on fail, whatever the render order (otherwise, it is only known
    // when the partial last packet is read, see demuxAndDecode()).
    if (_context && _context->pb) {
        const int64_t size = avio_size(_context->pb);
        for (size_t s = 0; (size > 0) && (s < _streams.size()); ++s) {
            Stream* st = _streams[s].get();
            const int entries = avformat_index_get_entries_count(st->_avstream);
            int64_t truncatedTs = AV_NOPTS_VALUE;
            for (int i = 0; i < entries; ++i) {
                const AVIndexEntry* e = avformat_index_get_entry(st->_avstream, i);
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
    }
}

// destructor
FFmpegFile::~FFmpegFile()
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_lock);
#endif

    _streams.clear();

    if (_context) {
        avformat_close_input(&_context);
        av_free(_context);
    }
    _filename.clear();
    _errorMsg.clear();
}

const FFmpegFile::Stream*
FFmpegFile::getStream(int streamIndex) const
{
    assert(streamIndex >= 0);
    if (_streams.empty()) {
        return nullptr;
    }
    // _streams is only set by the constructor: no lock needed
    return ((streamIndex >= 0) && (static_cast<size_t>(streamIndex) < _streams.size())) ? _streams[streamIndex].get() : _streams[0].get();
}

FFmpegFile::Stream*
FFmpegFile::getStream(int streamIndex)
{
    return const_cast<Stream*>(static_cast<const FFmpegFile*>(this)->getStream(streamIndex));
}

std::string
FFmpegFile::getColorspaceMetadata() const
{
#ifdef OFX_IO_MT_FFMPEG
    // av_read_frame() (in decode()) may update _context->metadata (AVFMT_EVENT_FLAG_METADATA_UPDATED, FFmpeg n9.0.2
    // libavformat/avformat.h:1689-1695)
    AutoMutex guard(_lock);
#endif

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
            return t->value;
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
    // the streams match (see CheckStreamPropertiesMatch()): the first one tells
    const Stream* stream = getStream(0);
    if (stream && stream->_codecContext && stream->_codecContext->codec_id) {
        if (stream->_codecContext->codec_id == AV_CODEC_ID_PRORES) {
            if ((_streams[0]->_codecContext->codec_tag == MKTAG('a', 'p', '4', 'h')) || (_streams[0]->_codecContext->codec_tag == MKTAG('a', 'p', '4', 'x'))) {
                return "Gamma1.8";
            } else {
                return "Gamma2.2";
            }
        }
    }
    if (stream && stream->_codecContext) {
        if (stream->_codecContext->color_trc == AVCOL_TRC_BT709 || stream->_codecContext->color_trc == AVCOL_TRC_SMPTE240M) {
            return "rec709";
        } else if (stream->_codecContext->color_trc == AVCOL_TRC_GAMMA22) {
            return "Gamma2.2";
        } else if (stream->_codecContext->color_trc == AVCOL_TRC_SMPTE170M) {
            return "rec601";
        } else if (stream->_codecContext->color_trc == AVCOL_TRC_SMPTE2084) {
            return "st2084";
        } else if (stream->_codecContext->color_trc == AVCOL_TRC_IEC61966_2_1) {
            return "sRGB";
        } else if (stream->_codecContext->color_trc == AVCOL_TRC_LINEAR) {
            return "linear";
        }
    }
    // (Stream::isYUV(): the lock is taken above)
    return (stream && stream->isYUV()) ? "Gamma2.2" : "Gamma1.8";
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
// compression", libavcodec/codec_desc.h:68-72 in FFmpeg 8.0 and 9.0), e.g. ProRes, DNxHD, MJPEG, rawvideo, v210,
// r210, r10k, HuffYUV, FFVHuff, JPEG-LS, JPEG 2000, Hap, EXR (libavcodec/codec_desc.c), or it is in the list
// below, which overrides the descriptor: PNG has no AV_CODEC_PROP_INTRA_ONLY (codec_desc.c:470 in FFmpeg 8.0 and 9.0;
// animated PNG is AV_CODEC_ID_APNG), but each PNG packet is a complete image. The other codecs of the list have
// AV_CODEC_PROP_INTRA_ONLY in FFmpeg 8.0 and 9.0: they are kept as they were before the descriptor was used.
// AVCodecContext::gop_size does not tell: it is "unused" for decoding (libavcodec/avcodec.h:1016-1021 in FFmpeg 9.0,
// 1000-1005 in 8.0), and avcodec_alloc_context3() sets it to 12, its default (the "g" option,
// libavcodec/options_table.h:96 in 9.0, 95 in 8.0, set for video decoders too by init_context_defaults(),
// libavcodec/options.c:90-110 in 8.0 and 9.0), so the former gop_size == 0 test was never true when decoding.
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

// A forward request more than one frame after the held frame (e.g. every other frame, a retime, or a render
// that skips frames) is decoded from the current position of the decoder, without a seek, when that is cheaper:
// demuxAndDecode() skips the frames before the requested one without converting them, whereas a seek restarts
// at a keyframe at or before the requested frame (with an index), or wherever the binary search of the demuxer
// lands (MPEG-TS, MPEG-PS, ASF without an index, seek.c:290 in FFmpeg 9.0.2), and decodes from there. FFmpeg has
// no model for this: ffplay seeks on every user seek, and the ffmpeg CLI never seeks after -ss.
// The forward distance is always bounded by Stream::forwardWindow(): the largest keyframe interval seen (in
// packets, so that a timestamp jump does not make it longer; with field-coded video, a packet per field doubles
// it), at least 1 s and at most 10 s. This bounds the cost of decoding forward when a keyframe comes between
// unseen (e.g. in an index with holes: ASF and RealMedia add the packets found by their binary search,
// asf_read_pts(), asfdec_f.c:1486 in FFmpeg 9.0.2, and rmdec.c:1148). Then:
// - With an index that covers the requested frame (its last entry is at or after it, and the format has no
//   timestamp discontinuities, Stream::_indexReliable; e.g. MOV/MP4, AVI, Matroska with cues), the keyframe that
//   a seek would restart at is the last entry at or before the requested frame (av_index_search_timestamp() with
//   AVSEEK_FLAG_BACKWARD, libavformat/seek.c:245 in FFmpeg 9.0.2 and 8.0). Decode forward if it was already read
//   (its timestamp is at most the largest PTS read, Stream::_maxReadTs). The index is on the DTS (MOV/MP4:
//   pkt->dts = sample->timestamp, mov.c:11679 in FFmpeg 9.0.2, and mov_seek_stream() searches it with the PTS
//   minus the decoding delay, mov.c:12008-12010; the generic index, demux.c:1504 and 1672) or on the PTS
//   (Matroska: the cues and the block timestamps, matroskadec.c:2023 and 4331, pkt->pts = timecode,
//   matroskadec.c:3986). Searching with the PTS of the requested frame finds the keyframe of the seek or a later
//   one, as the DTS is at most the PTS.
//   - With an index on the DTS, the keyframe found may be displayed after the requested frame: a frame decoded
//     before it (or, with open GOPs, e.g. an HEVC CRA, a leading picture, decoded after it but displayed before
//     it). Decode forward if the PTS of the keyframe is after the requested frame. With an index on the DTS, its
//     PTS is at least its DTS plus the decoding delay of the stream: with a constant frame rate, the frames
//     decoded before a keyframe are displayed before it. The delay is _startPTS - _startDTS, at most
//     has_b_frames frame durations (an edit-list pre-roll may make _startPTS - _startDTS larger: this would
//     only cost time, never a wrong frame). With an index on the PTS (Matroska), the keyframe's PTS is its
//     timestamp, and the delay only makes this decode forward a little more often, within the window.
//     The bound does not see the leading pictures: a leading picture whose CRA is not read yet seeks (mov.c then
//     only goes back to the previous keyframe for HEVC with an sgpd sync sample group, mov.c:4718-4742 and
//     can_seek_to_key_sample(), mov.c:11973, which FFmpeg's mov muxer does not write; H.264 open GOPs are never
//     handled there: decode() then decodes from the keyframe before, see earlierRestartTs()).
//   At worst (e.g. a keyframe whose DTS is just below the largest PTS read, not read yet), decoding forward reads a
//   few packets that a seek would not; the decoded frame is the same either way.
// - Without such an index (MPEG-TS and MPEG-PS, which have AVFMT_TS_DISCONT, mpegts.c:3880 and mpeg.c:707 in
//   FFmpeg 9.0.2, and whose index holds any packet with a DTS, mpegts.c:3805 and mpeg.c:472; Matroska without
//   cues, raw streams, whose index only grows as packets are read), where seeks are the expensive part (they
//   search the file), decode forward within the window.
// Not for intra-only codecs (each frame is decoded from its own packet, and a seek is cheap), nor after damage
// (Stream::_tainted: a seek restarts the decoder cleanly).
bool
FFmpegFile::Stream::decodeForwardIsCheaper(int frame,
                                           int held) const
{
    assert(frame > held + 1);
    if (_tainted || decodesAsIntraOnly()) {
        return false;
    }
    if (_maxReadTs == int64_t(AV_NOPTS_VALUE)) {
        return false; // the position of the demuxer is unknown
    }
    const int window = forwardWindow();
    if (frame - held > window) {
        return false;
    }
    const int64_t targetTs = frameToPts(frame);
    const int entries = avformat_index_get_entries_count(_avstream);
    if (!_indexReliable || (entries <= 0) || (avformat_index_get_entry(_avstream, entries - 1)->timestamp < targetTs)) {
        return true; // no index to trust: within the window
    }
    const AVIndexEntry* keyframe = indexKeyframeAtOrBefore(targetTs);
    if (!keyframe) {
        return true; // no keyframe before the requested frame: a seek would start from the first one
    }
    if (keyframe->timestamp <= _maxReadTs) {
        return true; // already read
    }
    const int64_t maxDelay = int64_t((std::max)({ 0, _codecContext->has_b_frames, _avstream->codecpar->video_delay })) * frameTicks();
    const int64_t decodingDelay = (std::min)((std::max)(int64_t(0), _startPTS - _startDTS), maxDelay);

    return keyframe->timestamp + decodingDelay > targetTs; // an open-GOP keyframe displayed after the requested frame
}

// The keyframe of the index at or before the timestamp, as a backward seek searches it (av_index_search_timestamp(),
// libavformat/seek.c:132-172 in FFmpeg 9.0.2; mov_seek_stream() calls it on the DTS, mov.c:12000-12016), or nullptr.
const AVIndexEntry*
FFmpegFile::Stream::indexKeyframeAtOrBefore(int64_t ts) const
{
    const int i = av_index_search_timestamp(_avstream, ts, AVSEEK_FLAG_BACKWARD);
    return (i >= 0) ? avformat_index_get_entry(_avstream, i) : nullptr;
}

// The timestamp from which decode() decodes again, at its retry-th retry, when a backward seek to the 0-based frame
// landed after it, or not on a restart point (Stream::_seekLanding, see checkSeekFrame()), or AV_NOPTS_VALUE if
// there is none (decode() then decodes from the start of the file).
// With an index to trust (Stream::_indexReliable), the keyframe of the index before the one that the seek finds.
// mov_seek_stream() does the same for an open-GOP HEVC key sample marked in an sgpd box (mov.c:12017-12035), but
// FFmpeg's mov muxer only writes sgpd boxes for audio roll groups (movenc.c:3305-3330), and H.264 has none.
// A seek to the timestamp of this keyframe lands on it: mov_seek_stream() first subtracts min_corrected_pts +
// dts_shift (mov.c:12009, 0 in the usual files). A positive offset makes the seek land earlier, which only decodes
// more frames; a negative one may make it land on the same keyframe again, and decode() then decodes from the start of
// the file.
// One keyframe back is enough for HEVC: the reference picture set of a CRA picture cannot hold a picture that precedes
// the previous IRAP picture in decoding or output order (H.265 section 8.3.2), so its leading pictures only reference
// pictures decoded from the previous IRAP picture on. For H.264, this depends on the encoder: the standard allows
// leading B-frames to reference pictures two GOPs back, for which FFmpeg would substitute its default reference without
// flagging the frame (libavcodec/h264_refs.c:392-407); x264 does not do this.
// Only at the first retry (retry 1): a later retry would decode from the start of the file (AV_NOPTS_VALUE).
// Without an index to trust (e.g. in MPEG-TS and MPEG-PS, whose index entries are the points of the timestamp
// searches, flagged as keyframes, mpegts.c:3805 and mpeg.c:472 in FFmpeg 9.0.2, and whose seeks search the file, so
// that a seek to one of them does not land one GOP earlier), the timestamp 2^retry forward windows before the frame
// (Stream::forwardWindow(): the largest keyframe interval seen, at least 1 s and at most 10 s), or AV_NOPTS_VALUE if
// that is at or before the first frame. A backward seek there (a binary search on the timestamps,
// ff_seek_frame_binary(), libavformat/seek.c:290-358) lands at or before it, and the decoder starts at the next
// keyframe, which is then at or before the requested frame, also if the requested frame is a leading picture of the
// keyframe after it: decoding from there gives it, instead of decoding from the start of the file (issue #334). The
// second window covers the decoding delay (the keyframe interval is counted in decoding order) and a seek that lands
// a little after the timestamp. The window doubles at each retry (4, 8, ... windows), for a keyframe interval that
// the stream does not tell: e.g. in a MOV without a sync-sample table, every packet has the keyframe flag
// (libavformat/mov.c:4947 in FFmpeg 9.0.2), so that the interval seen is one packet, and each seek lands on any
// sample (see demuxAndDecode()); a GOP longer than 2 windows then takes more than one retry, rather than decoding from
// the start of the file for each backward request (issue #440).
int64_t
FFmpegFile::Stream::earlierRestartTs(int frame, int retry, bool twoKeyframesBack) const
{
    if (!_indexReliable) {
        const int64_t from = int64_t(frame) - (int64_t(forwardWindow()) << (std::min)(retry, 30));
        return (from > firstFrame()) ? frameToPts(int(from)) : int64_t(AV_NOPTS_VALUE);
    }
    if (retry > 1) {
        return AV_NOPTS_VALUE;
    }
    const AVIndexEntry* keyframe = indexKeyframeAtOrBefore(frameToPts(frame));
    if (!keyframe || (keyframe->timestamp == int64_t(AV_NOPTS_VALUE))) {
        return AV_NOPTS_VALUE;
    }
    const AVIndexEntry* before = indexKeyframeAtOrBefore(keyframe->timestamp - 1);
    // Two keyframes back after a seek whose keyframe frame the decoder held back (see checkSeekFrame()), or the
    // first one. This assumes that a gradual refresh ends before the next keyframe (an H.264 recovery_frame_cnt less
    // than the keyframe interval, as libx264 writes): decoding from the keyframe before the one that the seek found
    // then decodes the whole refresh of that keyframe with its real references.
    if (before && twoKeyframesBack) {
        const AVIndexEntry* earlier = indexKeyframeAtOrBefore(before->timestamp - 1);
        if (earlier) {
            before = earlier;
        }
    }
    return before ? before->timestamp : int64_t(AV_NOPTS_VALUE);
}

// In a MOV without a sync-sample table, every sample is a keyframe (libavformat/mov.c:4947 in FFmpeg 9.0.2), and so is
// every entry of the index: seeks land on any sample. In a stream that is not decoded as intra-only, that index tells
// nothing about the restart points (decode() then does not trust it, Stream::_indexReliable). The frames are
// compared, not the timestamps (e.g. 29.97 fps in a time base of 1/600: the samples are 20 or 21 ticks apart).
bool
FFmpegFile::Stream::indexKeyframesEveryFrame(int64_t ts) const
{
    const AVIndexEntry* keyframe = indexKeyframeAtOrBefore(ts);
    if (!keyframe || (keyframe->timestamp == int64_t(AV_NOPTS_VALUE))) {
        return false;
    }
    const AVIndexEntry* before = indexKeyframeAtOrBefore(keyframe->timestamp - 1);
    return before && (ptsToFrame(keyframe->timestamp) - ptsToFrame(before->timestamp) <= 1);
}

// decode a single frame into the buffer thread safe
bool
FFmpegFile::decode(const ImageEffect* /*plugin*/,
                   int streamIndex,
                   int frame,
                   bool loadNearest,
                   unsigned char* buffer,
                   string* errorMsg)
{
#ifdef OFX_IO_MT_FFMPEG
    AutoMutex guard(_lock);
#endif

    // chosen under _lock, and passed to the functions that decode it (#291)
    Stream* stream = getStream(streamIndex);
    if (!stream) {
        if (errorMsg) {
            *errorMsg = _errorMsg.empty() ? string("FFmpeg Reader: no video stream") : _errorMsg;
        }
        return false;
    }

    // The error message of the previous frame, if any (unless the file is invalid)
    clearDecodeError();
    // Fail with _errorMsg (set by the caller)
    auto failWithError = [this, errorMsg]() -> bool {
        if (errorMsg) {
            *errorMsg = _errorMsg;
        }
        return false;
    };

    // Translate from the 1-based frames expected to 0-based frame offsets for use in the rest of this code.
    frame = frame - 1;

    // Check if the requested frame index is in range (AVStream::start_time is unknown in elementary streams)
    const int firstFrame = stream->firstFrame();
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

    // Each frame is decoded from its own packet: an intra-only codec, or a stream of keyframes after damage (see
    // Stream::decodesAsIntraOnly()).
    const bool decodeAsIntraOnly = stream->decodesAsIntraOnly();

    // The decoder outputs frames in presentation order (libavcodec decoders reorder frames
    // internally, e.g. B-frames in H.264, HEVC and MPEG-4), each with its timestamp: the frame
    // displayed at the requested time is decided from the decoded frames (see demuxAndDecode()).
    // The next frame out of the decoder is not always the next frame index: there may be a
    // timestamp gap in a variable frame rate movie.

    // Only seek and reset for non-sequential frames as this can be very costly: the frame may be
    // the one held since the previous request (e.g. in a timestamp gap of a variable frame rate
    // movie), or the next one out of the decoder.
    // The demuxer is shared by the streams (the views), and drops the packets of the other streams: after a decode of
    // another stream, this one must seek (#291).
    if (!stream->isSequential(frame) || stream->_restartSingleThread || (stream != _demuxedStream)) {
        seekToTimestamp(stream, stream->seekTs(frame));
    }
    _demuxedStream = stream;

    // Setup the output frame struct with the buffer passed in
    avFrameOut->width = stream->_width;
    avFrameOut->height = stream->_height;
    avFrameOut->format = stream->_outputPixelFormat;

    int res = 0;

    if ((res = av_image_fill_linesizes(avFrameOut->linesize, stream->_outputPixelFormat, stream->_width)) < 0) {
        setDecodeError(res, "FFmpeg Reader Failed to fill image linesizes: ");
        return failWithError();
    }

    res = av_image_fill_pointers(
        avFrameOut->data,
        stream->_outputPixelFormat,
        stream->_height,
        buffer,
        avFrameOut->linesize);

    if (res < 0) {
        setDecodeError(res, "FFmpeg Reader Failed to fill image pointers: ");
        return failWithError();
    }

    // Frames known to fail, without reading the file again: from the truncated packet of a truncated file
    // on, and the frames of a persistently damaged range, found by a previous request.
    if ((stream->_truncatedFrame >= 0) && (frame >= stream->_truncatedFrame)) {
        setDecodeError("the file is truncated", "FFmpeg Reader: ");
        return failWithError();
    }
    if (stream->inDamagedRange(frame)) {
        setDecodeError(stream->_damagedRangeError, "FFmpeg Reader Failed to decode packet (damaged range): ");
        return failWithError();
    }

    // The attempts of this request (see Attempt), in that order
    Attempt attempt = Attempt::eAtFrame;
    int64_t retryTs = AV_NOPTS_VALUE; // eEarlier: the timestamp of the retry (earlierRestartTs(), below)
    int earlierRetries = 0; // the retries from an earlier point so far
    // Seek for the current attempt (again, e.g. to decode it again with 1 thread)
    auto seekForAttempt = [&]() -> bool {
        switch (attempt) {
        case Attempt::eAtFrame:
            return seekToTimestamp(stream, stream->seekTs(frame));
        case Attempt::eEarlier:
            return seekToTimestamp(stream, retryTs);
        case Attempt::eFromStart:
            return seekToStart(stream);
        }
        return false;
    };
    bool twoKeyframesBack = false; // the first attempt's seek landed on a keyframe whose frame was held back
    bool fromStartFailed = false; // the attempt from the start of the file gave no frame up to the requested one
    int requestDamageError = 0; // a damaged packet was dropped by an attempt of this request
    stream->_invalidDataSkipped = 0;
    stream->_stoppedOnDamagedPacket = false;
    stream->_tooManyDroppedPackets = false;
    stream->_lastDamagedFrame = -1;
    stream->_requestDroppedPackets = 0;
    stream->_lastDamageConcealed = false;
    stream->_noTimestamps = false;
    stream->_sawFrameWithoutTimestamp = false;

    for (;;) {
        // A frame decoded after a seek may be corrupt (e.g. a reference frame is missing): it is
        // only used for a timestamp gap when decoding from the start (Stream::_decodingFromStart), or for a
        // stream decoded as intra-only that is not of an intra-only codec (all keyframes after damage), see
        // demuxAndDecode(). For an intra-only codec, a frame that conceals damage is a gap.
        hasPicture = demuxAndDecode(stream, avFrameOut, frame, decodeAsIntraOnly, attempt);
        if (stream->_invalidDataSkipped) {
            requestDamageError = stream->_invalidDataSkipped;
        }
        if (!hasPicture && stream->_restartSingleThread) {
            // damage found with frame threads: decode again with 1 thread (see noteDamage in demuxAndDecode()),
            // in the same way (e.g. from the start of the file, after a seek that landed after the requested frame)
            clearDecodeError();
            if (seekForAttempt()) {
                continue; // (this attempt again)
            }
            break;
        }
        if (hasPicture || (attempt == Attempt::eFromStart)) {
            if (!hasPicture && stream->_decodingFromStart && !stream->_tooManyDroppedPackets &&
                ((stream->_firstOutputFrame < 0) || (stream->_firstOutputFrame > frame)) &&
                (stream->_reachedEnd || (stream->_firstFrameAfter > frame))) {
                fromStartFailed = true;
            }
            break;
        }

        // The seek landed after the requested frame: no frame is held, and the first decoded frame is after it (or no
        // frame was decoded up to the end of the file, e.g. after the last keyframe of an MPEG-TS), without damage.
        // Not with a held frame: demuxAndDecode() then refused it as corrupt for a gap after it
        // (canHoldFrame()), and decoding from an earlier keyframe would give the same corrupt frame and refuse it
        // again; only decoding from the start can accept it (Stream::_decodingFromStart). E.g. a
        // leading picture of an open-GOP keyframe (an HEVC CRA, an H.264 I-frame with a recovery point) in MP4/MOV,
        // whose index is on the DTS: the keyframe found has a DTS at or before the requested frame, but a PTS after
        // it, and the decoder drops its leading pictures after a seek. Decode from the keyframe before that one
        // first, which decodes the leading pictures (once), rather than from the start of the file. Without an index
        // to trust (e.g. MPEG-TS, MPEG-PS), decode from an earlier timestamp (see earlierRestartTs()).
        // The same if the seek did not land on a restart point (Stream::_seekLanding, see checkSeekFrame()): the frame
        // of its first packet was a predicted frame, or the decoder did not output it (e.g. an H.264 recovery point with
        // a recovery frame count, issue #442: decode from two keyframes before).
        // An index whose keyframes are one frame apart (every sample, e.g. in a MOV without a sync-sample table, see
        // Stream::indexKeyframesEveryFrame()) is not trusted from then on, as after a seek that lands on a predicted
        // frame (issue #440): whether the decoder outputs that predicted frame (MPEG-4 Part 2) or nothing until the
        // next intra frame (H.264, MPEG-2).
        // Without an index to trust, the retries go back further and further (earlierRestartTs()), until the start of
        // the file.
        if (!decodeAsIntraOnly && !Stream::hasFrame(stream->_avHeldFrame) &&
            ((stream->_seekLanding != Stream::SeekLanding::eRestartPoint) || (stream->_firstOutputFrame > frame) ||
             ((stream->_firstOutputFrame < 0) && stream->_reachedEnd)) &&
            !stream->_invalidDataSkipped && !stream->_tooManyDroppedPackets && !stream->_noTimestamps && _errorMsg.empty()) {
            if (stream->_indexReliable && stream->indexKeyframesEveryFrame(stream->frameToPts(frame))) {
                stream->_indexReliable = false;
            }
            if (attempt == Attempt::eAtFrame) {
                twoKeyframesBack = (stream->_seekLanding == Stream::SeekLanding::eHeldBack);
            }
            retryTs = stream->earlierRestartTs(frame, ++earlierRetries, twoKeyframesBack);
            if (retryTs != int64_t(AV_NOPTS_VALUE)) {
                attempt = Attempt::eEarlier;
                if (seekForAttempt()) {
                    continue;
                }
            }
            clearDecodeError(); // (a failed seek: from the start of the file, below)
        }

        // A last ditch effot to get a frame out for non-intra codecs.
        // This will perform a seek to the start of the file. which is
        // the only reliable way to get frame accurate seeking in a
        // stream with B-frames.
        if (decodeAsIntraOnly || stream->_tooManyDroppedPackets || stream->_noTimestamps) {
            break;
        }
        attempt = Attempt::eFromStart;
        (void)seekForAttempt(); // (if it fails, this attempt decodes from where the demuxer is)
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
    // decoder delay, see noteDamage in demuxAndDecode()): the lowest such start.
    int damagedFrom = (decodeAsIntraOnly && !hasPicture && stream->_stoppedOnDamagedPacket) ? frame : -1;
    // Not into a known damaged range before the requested frame (it cannot give a frame to hold), and with a
    // bound on the packets dropped by all the attempts of this request.
    const int backLimit = stream->damagedRangeEndBefore(frame) + 1;
    for (int back = 1; !hasPicture && decodeAsIntraOnly && !firstAttemptFailedHard && !stream->_tooManyDroppedPackets && (frame > backLimit) && (stream->_requestDroppedPackets <= 2 * kMaxDroppedPacketsPerDecode) && (back < (1 << 30)); back *= 2) {
        const int from = (std::max)(backLimit, frame - back);
        clearDecodeError();
        stream->_stoppedOnDamagedPacket = false;
        stream->_invalidDataSkipped = 0;
        if (!seekToTimestamp(stream, stream->frameToPts(from))) {
            break;
        }
        hasPicture = demuxAndDecode(stream, avFrameOut, frame, decodeAsIntraOnly, Attempt::eEarlier, damagedFrom);
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
        if (!decodeAsIntraOnly && stream->_endTainted && Stream::hasFrame(stream->_avHeldFrame)) {
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
        stream->resetSeekState();
        if (_errorMsg.empty()) {
            if (stream->_sawFrameWithoutTimestamp) {
                setDecodeError("Video frames have no timestamp", "FFmpeg Reader: ");
            } else if (stream->_invalidDataSkipped && stream->_lastDamageConcealed) {
                // No frame could be decoded for the requested frame: the decoder output frames that conceal damage.
                setDecodeError("the decoded frames conceal damage (damaged packets)", "FFmpeg Reader: ");
            } else if (stream->_invalidDataSkipped) {
                // No frame could be decoded for the requested frame, and damaged packets were dropped.
                setDecodeError(stream->_invalidDataSkipped, "FFmpeg Reader Failed to decode packet: ");
            } else {
                setDecodeError("no frame could be decoded at or before this frame", "FFmpeg Reader: ");
            }
        }
        return failWithError();
    }

    return true;
} // FFmpegFile::decode

// A frame that the decoder says conceals damage: AV_FRAME_FLAG_CORRUPT (in H.264 and HEVC, only set with
// AV_CODEC_FLAG_OUTPUT_CORRUPT or AV_CODEC_FLAG2_SHOW_ALL, libavcodec/h264_slice.c:1379-1384 and
// libavcodec/hevc/refs.c:509-519 in FFmpeg 9.0.2) or decode_error_flags (which the H.264 decoder sets on a frame whose
// slices had errors or were concealed, libavcodec/h264dec.c:752-764). Only a few decoders set them (e.g. through
// libavcodec/error_resilience.c, and H.264, ProRes): a frame without them is not known to be good.
static bool
ConcealsDamage(const AVFrame* f)
{
    return (f->flags & AV_FRAME_FLAG_CORRUPT) || f->decode_error_flags;
}

// A frame decoded as a keyframe, without an error: e.g. an H.264 IDR frame (AV_FRAME_FLAG_KEY, libavcodec/h264_slice.c:
// 1664 in FFmpeg 9.0.2), that does not conceal damage (ConcealsDamage())
static bool
IsCleanKeyframe(const AVFrame* f)
{
    return (f->flags & AV_FRAME_FLAG_KEY) && !ConcealsDamage(f);
}

// A predicted frame: its picture type is P, B, S or SP, and the decoder does not flag it as a keyframe. (The H.264
// decoder flags a P-frame with a recovery point as a keyframe, libavcodec/h264dec.c:881-882 in FFmpeg 9.0.2: it is
// not a predicted frame here.) Unlike Stream::isCleanRestartFrame(), which needs evidence that a frame is clean to end
// the damage, this needs evidence that it is wrong: a frame without a picture type is not predicted.
static bool
IsPredictedFrame(const AVFrame* f)
{
    if (f->flags & AV_FRAME_FLAG_KEY) {
        return false;
    }
    switch (f->pict_type) {
    case AV_PICTURE_TYPE_P:
    case AV_PICTURE_TYPE_B:
    case AV_PICTURE_TYPE_S:
    case AV_PICTURE_TYPE_SP:
        return true;
    default:
        return false;
    }
}

// The frame of the packet where the decoder restarted after damage (a packet with AV_PKT_FLAG_KEY, sent to a decoder
// opened again, see demuxAndDecode()) is a valid restart point if it is a clean keyframe (IsCleanKeyframe()).
// Or, if no clean keyframe was decoded yet since the file was opened (the decoder may never flag keyframes), if it
// is an intra frame or has no picture type, and does not conceal damage: the frame flag then says nothing, and what
// is left is the keyframe flag of the container, the picture type, and the decoder errors (a packet that needs the
// previous ones may fail, or give no frame, in a decoder opened at it). E.g. the CineForm decoder sets neither
// AV_FRAME_FLAG_KEY nor pict_type (libavcodec/cfhd.c in FFmpeg 9.0.2), and its descriptor has no
// AV_CODEC_PROP_INTRA_ONLY (libavcodec/codec_desc.c:1530-1535), with which libavcodec would set the flag and
// AV_PICTURE_TYPE_I (libavcodec/decode.c:455-456 and 2034-2039): requiring the flag kept such a stream tainted from
// the first damaged packet to the end of the file (issue #312). The second frame of a CineForm 3D-transform group fails
// in a decoder opened at it ("Invalid dimensions", cfhd.c:928-932). But the picture type is needed: e.g. in a MOV
// without a sync-sample table, every packet has the keyframe flag (libavformat/mov.c:4947), and the MPEG-4 Part 2
// decoder decodes a P-frame without its reference, from a gray frame, without an error
// (ff_mpv_alloc_dummy_frames(), libavcodec/mpegvideo_dec.c:310-333).
bool
FFmpegFile::Stream::isCleanRestartFrame(const AVFrame* f) const
{
    if (IsCleanKeyframe(f)) {
        return true;
    }
    const bool intraOrUnknown = (f->pict_type == AV_PICTURE_TYPE_I) || (f->pict_type == AV_PICTURE_TYPE_NONE);
    return (_keyFrames == 0) && intraOrUnknown && !ConcealsDamage(f);
}

// The restart keyframe (Stream::_restartPts, after damage) is verified by its own frame (isCleanRestartFrame()). Until
// then, the packets flagged as keyframes do not restart the decoder again (see demuxAndDecode()), and two other frames
// settle it:
// - a clean keyframe (IsCleanKeyframe()) displayed after it is a restart point by itself, whatever packet the decoder
//   restarted at: e.g. in a MOV without a sync-sample table, every packet has the keyframe flag (libavformat/mov.c:4947
//   in FFmpeg 9.0.2), the decoder restarted at a predicted frame, and the HEVC decoder outputs nothing until the next
//   IRAP picture (issue #441);
// - another frame displayed after it, that comes before it, shows that the decoder dropped it, or did not output it
//   (e.g. an H.264 recovery point with a recovery frame count, whose frames are only output from the recovery point
//   on, libavcodec/h264_slice.c:1372-1384 and 1649-1681 in FFmpeg 9.0.2, 1373-1385 and 1644-1676 in 8.0.1): no clean
//   restart.
void
FFmpegFile::Stream::settleRestart(const AVFrame* f)
{
    if ((f->pts == AV_NOPTS_VALUE) || (_restartPts == int64_t(AV_NOPTS_VALUE))) {
        return;
    }
    if (f->pts == _restartPts) {
        verifyRestart(isCleanRestartFrame(f));
    } else if (f->pts > _restartPts) {
        if (IsCleanKeyframe(f)) {
            _restartPts = f->pts;
            verifyRestart(true);
        } else {
            verifyRestart(false);
        }
    }
}

// The check of the frame of the first packet sent after a seek (Stream::packetSent(), whatever its keyframe flag),
// for the 0-based requested frame. That packet is normally one that the demuxer flags as a keyframe.
// - If its frame is a predicted frame, the flag was wrong, and the decoder decoded it without its reference frames,
//   often without an error: e.g. in a MOV without a sync-sample table, every packet has the keyframe flag
//   (libavformat/mov.c:4947 in FFmpeg 9.0.2), and the MPEG-4 Part 2 decoder decodes a P-frame from a gray frame
//   (ff_mpv_frame_start() calls ff_mpv_alloc_dummy_frames(), libavcodec/mpegvideo_dec.c:310-346 and 393). The frames
//   up to the next one that is not predicted are not used (eSkip), and decoding stops at the first one after the
//   requested frame (eStop, SeekLanding::ePredicted: decode() then decodes from an earlier point), rather than at the
//   next intra frame or the end of the file; the index of the keyframes is not trusted any more (_indexReliable).
//   Not checked: the frames output before that frame (leading B-frames: the MPEG-4 Part 2 decoder skips a B-frame
//   whose timing is out of order after a seek, libavcodec/mpeg4videodec.c:3138-3142 in FFmpeg 9.0.2, 3280-3284 in
//   8.0.1).
// - If a frame displayed after it comes first, the decoder did not output it, e.g. P-frames that the H.264 or MPEG-2
//   decoder does not output until it recovers (libavcodec/h264_slice.c:1372-1384 in FFmpeg 9.0.2, 1373-1385 in 8.0.1;
//   mpeg12dec.c:2455-2463 in 9.0.2, 2458-2466 in 8.0.1), or an H.264 recovery point
//   with a recovery frame count (gradual decoding refresh, as libx264 writes with -intra-refresh 1, which the MP4
//   muxer flags as a sync sample), whose frames are only output from the recovery point on (h264_slice.c:1649-1681 in
//   9.0.2, 1644-1676 in 8.0.1). That keyframe is not a restart point by itself (eStop, SeekLanding::eHeldBack). The
//   frames from the recovery point on may still differ from those decoded from an earlier keyframe, whatever the
//   reader does (the ffmpeg CLI with -ss gives the same frames): at a recovery point whose frame_num is 0, the H.264
//   decoder sees no frame_num gap (h264_slice.c:1507-1508 in 9.0.2, 1506-1507 in 8.0.1; prev_frame_num is 0 in a new
//   decoder, -1 after a flush, h264dec.c:459 in 9.0.2, 460 in 8.0.1), logs "Missing reference picture"
//   (h264_refs.c:400 in 9.0.2, 395 in 8.0.1) instead of filling the gap, and the frames differ up to the end of the
//   refresh of the next recovery point, also when decoding on from there (issue #442). decode() then decodes from two
//   keyframes before (earlierRestartTs()). Only with an index to trust (otherwise a seek lands anywhere, and the
//   decoder may well skip frames), on a packet flagged as a keyframe, before damage was found (the frame of a damaged
//   packet does not come either: the restart keyframe is then checked instead, settleRestart()), not for streams
//   decoded as intra-only, and only for the attempt at the requested frame (not from an earlier point or from the
//   start, where that is the best restart point). In a MOV without a sync-sample table, a seek may also land on a
//   B-frame, which the MPEG-4 Part 2 decoder skips without a reference frame (libavcodec/h263dec.c:545-547 in 9.0.2,
//   532-534 in 8.0.1); that index is not trusted once decode() finds that its keyframes are one frame apart
//   (indexKeyframesEveryFrame()), so this check does not flag it then.
// - A packet that the demuxer flags with AV_PKT_FLAG_DISCARD (the edit-list pre-roll of a MOV/MP4, mov.c:11680-11681
//   in FFmpeg 9.0.2) is decoded, but libavcodec drops its frame (libavcodec/decode.c:463-466): such a seek is not
//   checked (see packetSent()), as before this check existed.
FFmpegFile::Stream::SeekFrameVerdict
FFmpegFile::Stream::checkSeekFrame(const AVFrame* f, int frame, bool decodeAsIntraOnly, Attempt attempt)
{
    if ((_seekFrameCheck == SeekFrameCheck::eAwaitFrame) && (f->pts != AV_NOPTS_VALUE) && (f->pts > _seekFramePts)) {
        _seekFrameCheck = SeekFrameCheck::eNone;
        if (_seekFrameKey && _indexReliable && !decodeAsIntraOnly && !_damageSeen && !_decodingFromStart && (attempt == Attempt::eAtFrame)) {
            _seekLanding = SeekLanding::eHeldBack;
            return SeekFrameVerdict::eStop;
        }
    }
    if ((_seekFrameCheck == SeekFrameCheck::eAwaitFrame) && (f->pts == _seekFramePts)) {
        _seekFrameCheck = SeekFrameCheck::eNone;
        if (!_decodingFromStart && IsPredictedFrame(f)) {
            _seekFrameCheck = SeekFrameCheck::eAwaitIntraFrame;
            _indexReliable = false;
        }
    }
    if (_seekFrameCheck == SeekFrameCheck::eAwaitIntraFrame) {
        if (IsPredictedFrame(f)) {
            const int64_t ts = f->best_effort_timestamp;
            if ((ts != int64_t(AV_NOPTS_VALUE)) && (ptsToFrame(ts) > frame)) {
                _seekLanding = SeekLanding::ePredicted;
                return SeekFrameVerdict::eStop;
            }
            return SeekFrameVerdict::eSkip;
        }
        _seekFrameCheck = SeekFrameCheck::eNone;
    }
    return SeekFrameVerdict::eUse;
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
                stream->_paletteNeeded = true;
                return;
            }
        }
        avcodec_free_context(&ctx);
    }
    stream->flushCodec();
}

bool
FFmpegFile::seekToTimestamp(Stream* stream, int64_t ts)
{
    return seek(stream, ts, AVSEEK_FLAG_BACKWARD);
}

// The start of the file: frame 0 with AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD. Timestamp 0 is at or before the first
// packet, unless the stream starts at a negative timestamp that is not an edit-list pre-roll (see
// NegativeStartIsPreroll()), e.g. an MPEG-TS whose timestamps wrap: timestamp 0 is then after the wrap, and the MPEG-TS
// and MPEG-PS demuxers, which have no read_seek but a read_timestamp (mpegts.c:3878, mpeg.c:712 in FFmpeg 9.0), seek to
// it by a binary search on the DTS (seek_frame_internal(), libavformat/seek.c:597-639, ff_seek_frame_binary(), with the
// timestamps unwrapped as the packets, seek.c:281-288). Seek to the first DTS of the stream instead
// (Stream::_startDTS, or its first PTS without B-frames), which is at or before its first packet.
// The same if only the first DTS is negative (B-frames whose first frame is at 0): the index of a demuxer without
// read_seek (AVFMT_GENERIC_INDEX, e.g. a raw MPEG-4 elementary stream) is keyed by the DTS of the keyframes (FFmpeg
// n9.0.2 libavformat/demux.c:1670-1673), and seeking to 0 lands on the second keyframe, whose DTS is 0
// (seek_frame_generic(), libavformat/seek.c:526-595): the first frames would never decode.
// On success, the decoder decodes every frame from the start of the stream until the next seek
// (Stream::_decodingFromStart), including for the next sequential requests: otherwise each of them would refuse the
// same corrupt frame and decode from the start again.
bool
FFmpegFile::seekToStart(Stream* stream)
{
    int64_t ts = 0;
    int seekFlags = AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD;
    const int64_t firstTimestamp = (std::min)(stream->_startPTS, stream->_startDTS);
    if ((firstTimestamp < 0) && !NegativeStartIsPreroll(_context, stream->_avstream)) {
        ts = firstTimestamp;
        seekFlags = AVSEEK_FLAG_BACKWARD;
    }
    if (!seek(stream, ts, seekFlags)) {
        ++_startSeekCount;
        return false;
    }
    if (!stream->_decodingFromStart) { // (otherwise seek() fell back to the start, and counted it)
        ++_startSeekCount;
        stream->_decodingFromStart = true;
    }
    return true;
}

bool
FFmpegFile::seek(Stream* stream, int64_t ts, int seekFlags)
{
    stream->resetSeekState(); // also resets _decodingFromStart
    stream->_seekTargetTs = ts; // (seekToStart() then sets _decodingFromStart: see Stream::armResync())
    flushDecoder(stream);
    stream->clearTaint();
    ++_seekCount;
    int res = 0;

    // Once damage was found in the file (long-GOP codecs), a seek may land on a spurious keyframe: the Matroska
    // demuxer adds an index entry for each block whose header has the keyframe flag (libavformat/matroskadec.c:
    // 4319-4320 and 4329-4331 in FFmpeg 9.0.2), also in a damaged block. Skip the index entries of the keyframes
    // that did not decode cleanly (Stream::_suspectKeyPts), and decode as after a restart at a keyframe: the
    // frames are only used once the first packet read decodes as a clean keyframe (Stream::verifyRestart()).
    if (stream->_damageSeen && !stream->decodesAsIntraOnly()) {
        if (seekFlags == AVSEEK_FLAG_BACKWARD) {
            int i = av_index_search_timestamp(stream->_avstream, ts, AVSEEK_FLAG_BACKWARD);
            while (i > 0) {
                const AVIndexEntry* e = avformat_index_get_entry(stream->_avstream, i);
                if (!e || !stream->_suspectKeyPts.contains(e->timestamp)) {
                    break;
                }
                --i;
            }
            const AVIndexEntry* e = (i >= 0) ? avformat_index_get_entry(stream->_avstream, i) : nullptr;
            if (e && (e->timestamp < ts)) {
                ts = e->timestamp;
            }
        }
        stream->_tainted = true;
        if (stream->positionTaint()) {
            stream->_awaitRestartFrame = true;
        } else {
            stream->_verifyNextPacket = true;
        }
    }
    if ((res = av_seek_frame(_context, stream->_idx, ts, seekFlags)) < 0) {
        // A stream whose index cannot be used (Stream::_seekFallsBackToStart, e.g. an image pipe, in which libavformat
        // cannot seek, see getStreamFrames()) decodes from the start of the file. Only in a seekable input: a
        // byte seek "succeeds" without moving in one that is not (seek_frame_byte() ignores the result of avio_seek(),
        // libavformat/seek.c:505-524 in FFmpeg n9.0.2 and n8.0.1).
        if (!stream->_seekFallsBackToStart || !_context->pb || !(_context->pb->seekable & AVIO_SEEKABLE_NORMAL) || (seekToStreamStart(*stream) < 0)) {
            setDecodeError(res, "FFmpeg Reader Failed to seek frame: ");
            return false;
        }
        ++_startSeekCount;
        stream->_decodingFromStart = true;
    }

    return true;
}

// The end of a packet read by demuxAndDecode() for stream: its PTS (or its DTS, plus the decoder delay of stream, if
// it has none, e.g. AVI with B-frames), plus its duration, or one frame of stream without one, so that the frame of
// the last packet is never taken as missing. _packetsEnd is the largest end, over all the streams (_lock held).
void
FFmpegFile::notePacketEnd(const Stream& stream, const AVPacket& pkt)
{
    if ((pkt.stream_index < 0) || (unsigned(pkt.stream_index) >= _context->nb_streams)) {
        return;
    }
    int64_t ts = pkt.pts;
    if (ts == int64_t(AV_NOPTS_VALUE)) {
        ts = pkt.dts;
        if (ts == int64_t(AV_NOPTS_VALUE)) {
            return;
        }
        if (pkt.stream_index == stream._idx) {
            ts += stream._startPTS - stream._startDTS;
        }
    }
    const AVRational timeBase = _context->streams[pkt.stream_index]->time_base;
    const int64_t start = av_rescale_q(ts, timeBase, AV_TIME_BASE_Q);
    const int64_t duration = (pkt.duration > 0) ? av_rescale_q(pkt.duration, timeBase, AV_TIME_BASE_Q) : stream.frameDuration();
    // Saturated: a damaged timestamp must not wrap around (ES.103)
    const int64_t end = ((duration > 0) && (start > std::numeric_limits<int64_t>::max() - duration))
        ? std::numeric_limits<int64_t>::max()
        : (start + duration);
    raiseTs(_packetsEnd, end);
}

// avcodec_send_packet() and avcodec_receive_frame() replace avcodec_decode_video2(), see
// https://github.com/FFmpeg/FFmpeg/blob/9e30859cb60b915f237581e3ce91b0d31592edc0/libavcodec/decode.c#L748
// Doc for the new AVCodec API: https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/

bool
FFmpegFile::demuxAndDecode(Stream* stream, AVFrame* avFrameOut, int64_t frame, bool decodeAsIntraOnly, Attempt attempt, int damagedFrom)
{
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
    // unless it cannot be better (when decoding from the start, or decoding as intra-only, e.g. all-intra
    // H.264): a frame decoded after a seek may lack a reference frame, so decode() will then retry from the
    // start. For an intra-only codec (IsIntraOnly()), a frame that conceals damage is never held (see
    // processDecodedFrame).
    const bool intraOnlyCodec = stream->_intraOnlyCodec;
    auto canHoldFrame = [decodeAsIntraOnly, stream](const AVFrame* f) -> bool {
        return decodeAsIntraOnly || stream->_decodingFromStart || !ConcealsDamage(f);
    };

    if (Stream::hasFrame(held)) {
        int64_t heldFrameIdx = stream->ptsToFrame(held->pts);
        // decode() seeks if the requested frame is before the held frame (Stream::isSequential()).
        assert(heldFrameIdx <= frame);
        if (heldFrameIdx > frame) {
            // Should not happen, but the held frames would give a wrong frame: seek, which resets them.
            seekToTimestamp(stream, stream->seekTs((int)frame));
        } else if (heldFrameIdx == frame) {
            return imageConvert(stream, held, avFrameOut);
        } else if (Stream::hasFrame(next) || stream->_heldFrameIsLast) {
            // The requested frame is in a gap after the held frame.
            if (!canHoldFrame(held)) {
                return false;
            }
            return imageConvert(stream, held, avFrameOut);
        }
    }

    // Process a frame output by the decoder. Returns true if it gives the requested frame (which is
    // then converted into avFrameOut), or if decoding should stop without a picture (hasPicture is
    // then false, and decode() retries from the start).
    int droppedPackets = 0; // consecutive dropped packets, without a decoded frame
    stream->_firstOutputFrame = -1;
    stream->_firstFrameAfter = -1;
    stream->_reachedEnd = false;
    stream->_seekLanding = Stream::SeekLanding::eRestartPoint;
    stream->_endTainted = false;
    bool decodedAny = false; // a frame was decoded since the start of this call
    // Intra-only codecs: this call dropped damaged packets, and neither decoded nor held any frame up to the
    // requested one (a later frame came, or the end of the file, or too many damaged packets): every frame from
    // where it started to the requested one is damaged (decode() then knows the damaged range).
    auto markOnlyDamaged = [&]() {
        if (decodeAsIntraOnly && stream->_invalidDataSkipped && !Stream::hasFrame(held)) {
            stream->_stoppedOnDamagedPacket = true;
            stream->_lastDamagedFrame = (std::max)(stream->_lastDamagedFrame, (int)frame);
        }
    };
    int framesWithoutTimestamp = 0;
    // Intra-only codecs: the frame from which the damaged packets stop decoding (see noteDamage, below).
    const int stopFrame = (damagedFrom >= 0) ? damagedFrom : (int)frame + 1;
    // A damaged packet that the decoder rejected (error r), or a frame of an intra-only codec that conceals damage (see
    // concealedIntraFrame, below): the bookkeeping of both, at the PTS of the packet or of the frame (AV_NOPTS_VALUE if
    // unknown).
    enum class DamageSource {
        ePacket, // a packet that the decoder rejected: its PTS is the one of the damaged frame
        ePacketFrameThreads, // the same with frame threads: the error is that of a packet up to the decoder delay before
        eConcealedFrame, // a frame that conceals damage: its PTS is exact
    };
    enum class DamageAction {
        eSkip, // drop it and go on decoding
        eStop, // intra-only: stop, as every frame from the start of this call to the requested one is damaged
        eTooMany, // stop with this error: too many consecutive damaged packets
    };
    auto noteDamage = [&](int r, int64_t pts, DamageSource source) -> DamageAction {
        const bool exact = source != DamageSource::ePacketFrameThreads;
        const bool concealed = source == DamageSource::eConcealedFrame;
        auto warnOnce = [&]() {
            if (!_damagedPacketWarned) {
                _damagedPacketWarned = true;
                char errorBuf[256];
                av_strerror(r, errorBuf, sizeof(errorBuf));
                av_log(nullptr, AV_LOG_WARNING, "FFmpeg Reader: %s: skipping damaged %s (%s)\n", _filename.c_str(), concealed ? "frames" : "packets", errorBuf);
            }
        };
        ++stream->_requestDroppedPackets;
        stream->_damageSeen = true;
        stream->_damagedSinceOpen = true;
        stream->_lastDamageConcealed = concealed;
        if (!decodeAsIntraOnly) {
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
        if (++droppedPackets > kMaxDroppedPacketsPerDecode) {
            stream->_tooManyDroppedPackets = true;
            if (!decodedAny) {
                stream->_invalidDataSkipped = r;
                markOnlyDamaged();
            }
            return DamageAction::eTooMany;
        }
        stream->_invalidDataSkipped = r;
        if (pts == int64_t(AV_NOPTS_VALUE)) {
            warnOnce();
            return DamageAction::eSkip;
        }
        const int damagedFrame = stream->ptsToFrame(pts) - (exact ? 0 : stream->getCodecDelay());
        if (decodeAsIntraOnly && (damagedFrame >= stopFrame) && !Stream::hasFrame(held)) {
            stream->_stoppedOnDamagedPacket = true;
            // With frame threads, the frames after the requested one are not known to be damaged (the
            // damaged packet is one of the last ones sent).
            stream->_lastDamagedFrame = (std::max)(stream->_lastDamagedFrame, exact ? (std::max)((int)frame, damagedFrame) : (int)frame);
            return DamageAction::eStop; // (decode() then decodes from earlier frames)
        }
        stream->_lastDamagedFrame = (std::max)(stream->_lastDamagedFrame, damagedFrame);
        if (exact) {
            stream->_damagedPts.add(pts);
        }
        warnOnce();
        return DamageAction::eSkip;
    };
    // Intra-only codecs (IsIntraOnly()): a frame that conceals damage is not used (e.g. a ProRes frame with damaged
    // slices, libavcodec/proresdec.c:754 in FFmpeg 9.0.2, 745 in 8.0, whose undecoded slices keep the contents of the
    // decoder buffer, which depends on the read order and the threads): its frame time is a gap, and it counts as a
    // damaged packet that the decoder rejected (noteDamage()), so that a long run of such frames is not decoded again
    // for each request in a random order, and becomes a damaged range (issue #434). Its frame is known exactly, also
    // with frame threads. The ffmpeg CLI keeps such a frame, with a "corrupt decoded frame" warning
    // (fftools/ffmpeg_dec.c:767-772 in FFmpeg 9.0.2). This depends on the codec only, not on decodeAsIntraOnly, which
    // also becomes true once damage was seen in a stream of keyframes (e.g. all-intra H.264, whose concealed frames are
    // used, as before): the result then does not depend on the read order.
    // Returns true if decoding stops (without a picture).
    auto concealedIntraFrame = [&](const AVFrame* f) -> bool {
        const int64_t ts = (f->best_effort_timestamp != int64_t(AV_NOPTS_VALUE)) ? f->best_effort_timestamp : f->pts;
        return noteDamage(AVERROR_INVALIDDATA, ts, DamageSource::eConcealedFrame) != DamageAction::eSkip;
    };
    auto processDecodedFrame = [&](AVFrame* decodedFrame) -> bool {
        // Fallback to using the DTS from the AVPacket that triggered returning this frame if no PTS is found
        // Original code, which doesn't work (AVI/m1v):
        // if (decodedFrame->pts == AV_NOPTS_VALUE) {
        //     decodedFrame->pts = decodedFrame->pkt_dts;
        // }
        if (intraOnlyCodec && ConcealsDamage(decodedFrame)) {
            return concealedIntraFrame(decodedFrame);
        }
        droppedPackets = 0;
        decodedAny = true;
        // The damaged packets are known by their PTS: a frame whose timestamp is from its DTS is not compared.
        // A frame of a packet sent after damage, before the decoder restarted at a keyframe that decoded cleanly
        // (long-GOP codecs), is not used either: it may reference a damaged frame (Stream::taint()).
        // Without PTS (e.g. AVI, where the demuxer only sets the DTS, libavformat/avidec.c:1554 in FFmpeg 9.0.2,
        // and the PTS of the frames that are not reordered, libavformat/demux.c:1131-1137), the frames cannot be
        // matched to their packets: while tainted, every frame is rejected, until the first frame after a
        // restart of the decoder (which only has the packets from the restart keyframe on) is a clean restart
        // (Stream::isCleanRestartFrame()).
        const bool positionTainted = !decodeAsIntraOnly && stream->_tainted && stream->positionTaint();
        if (positionTainted && stream->_awaitRestartFrame) {
            stream->_awaitRestartFrame = false;
            if (stream->isCleanRestartFrame(decodedFrame)) {
                stream->_tainted = false;
            }
        }
        stream->settleRestart(decodedFrame);
        // While tainted, a frame that conceals errors (decode_error_flags, AV_FRAME_FLAG_CORRUPT) is not used either,
        // whatever its packet: e.g. a damaged IDR frame that the H.264 decoder decodes without an error, but with
        // FF_DECODE_ERROR_DECODE_SLICES (libavcodec/h264dec.c:752-764 in FFmpeg 9.0.2), output after the
        // errors of the following packets.
        // The frame is placed at its best_effort_timestamp (see below), which libavcodec takes from the PTS, or
        // from the DTS of the packet that returned the frame once the PTS look less reliable than the DTS
        // (guess_correct_pts(), libavcodec/decode.c:296-320 and 694-696 in FFmpeg 9.0.2): the taint is checked at
        // both, and while tainted, a frame whose two timestamps differ is not used (e.g. a frame decoded before
        // the damage, but placed after it at the DTS of a damaged packet).
        // The frame of the first packet sent after a seek (see Stream::checkSeekFrame())
        switch (stream->checkSeekFrame(decodedFrame, (int)frame, decodeAsIntraOnly, attempt)) {
        case Stream::SeekFrameVerdict::eUse:
            break;
        case Stream::SeekFrameVerdict::eSkip:
            return false;
        case Stream::SeekFrameVerdict::eStop:
            return true; // stop without a picture
        }
        const bool concealsDamage = ConcealsDamage(decodedFrame);
        if (!concealsDamage) {
            // (see decode(): the frames decoded without an error, keyframes or not)
            if (IsCleanKeyframe(decodedFrame)) {
                ++stream->_keyFrames;
            } else {
                ++stream->_nonKeyFrames;
            }
        }
        auto taintedAt = [stream](int64_t t) -> bool {
            return (t != AV_NOPTS_VALUE) && (stream->_damagedPts.contains(t) || stream->_taintedPts.contains(t) || stream->isCandidatePts(t));
        };
        // (Intra-only codecs: a frame that conceals damage was handled above, see concealedIntraFrame.)
        const bool atDamagedTime = taintedAt(decodedFrame->pts) || taintedAt(decodedFrame->best_effort_timestamp);
        const bool taintedLongGop = !decodeAsIntraOnly && stream->_tainted &&
                                    (concealsDamage || stream->positionTaint() || (decodedFrame->best_effort_timestamp != decodedFrame->pts));
        const bool damaged = atDamagedTime || taintedLongGop;
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
            // A few such frames are skipped (e.g. the first one after a seek, in a container without PTS, before
            // the next one has a DTS), but a stream without timestamps stops (decoding further, or from the
            // start, would read the rest of the file for each request), without making the file invalid.
            if (++framesWithoutTimestamp > 32) {
                stream->_noTimestamps = true;
                return true; // stop without a picture
            }
            return false;
        }

        // A stream whose timestamps may shift, e.g. an MPEG-PS from FFmpeg's vob muxer (see FFmpegFile::timestampsMayShift())
        if (stream->_timestampsMayShift) {
            const std::optional<int64_t> placed = stream->placeShiftedFrame(decodedFrame->pts);
            if (!placed) {
                return false;
            }
            decodedFrame->pts = *placed;
        }

        if (damaged) {
            // A frame at the time of a damaged packet, which was dropped (see noteDamage): not
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
            hasPicture = imageConvert(stream, held, avFrameOut);
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
            hasPicture = imageConvert(stream, held, avFrameOut);
            return true;
        }

        // Decoding started after the requested frame (the seek went too far, or the requested
        // frame is before the first frame): decoding further cannot give it, since frames come in
        // presentation order. Stop without a picture: decode() retries from the start.
        // Intra-only codecs: if damaged packets were dropped, every frame from the start of this
        // attempt to the requested one is damaged (each good packet gives a frame, in presentation
        // order), as when stopping on a damaged packet (see noteDamage).
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
    // frame read the rest of the file: after kMaxDroppedPacketsPerDecode packets dropped without any decoded
    // frame, decoding stops with that error (after draining the decoder, which may still hold good frames),
    // and the frames up to the last damaged packet fail without reading again (Stream::_damagedRanges). The bound is per decode(): in a long-GOP stream, the frames
    // after a damaged run of more than kMaxDroppedPacketsPerDecode packets fail until the next keyframe.
    // The PTS of a damaged packet is remembered (without frame threads): the decoder may later output
    // a frame with that PTS anyway (e.g. a damaged first MPEG-2 I-frame, when decoding from the start, gives a picture
    // from a recycled buffer, without any error flag), which must not be used (see
    // processDecodedFrame).
    // A packet that the demuxer flagged as corrupt is decoded (and skipped if the decoder rejects it),
    // unless it is the partial last packet of a truncated file (see below). E.g. the MPEG-TS demuxer flags
    // the packets with continuity counter or transport errors, which are common in broadcast recordings.
    // A frame that the decoder outputs is used, even if it conceals damage (AV_FRAME_FLAG_CORRUPT or
    // decode_error_flags, which concealed H.264 frames have too), except while tainted (long-GOP codecs) and for
    // intra-only codecs (IsIntraOnly(), see processDecodedFrame): e.g. an HEVC frame after a damaged IDR frame and
    // the next packet, or a concealed frame of all-intra H.264.
    // A frame without these flags is used too, e.g. a partially damaged MJPEG or JPEG 2000 frame (neither decoder
    // sets them), whose undecoded area keeps the contents of a recycled buffer: such frames may then differ between
    // read orders.
    // For intra-only codecs, decoding stops at the first damaged packet after the requested frame, without a
    // held frame: the following ones cannot give it (decode() then decodes from earlier frames). Or at the
    // first one in the frames up to the requested one already known to be damaged (damagedFrom), so that the
    // attempts of decode() from earlier frames do not read these again (stopFrame, above).
    auto skipInvalidData = [&](int r, const AVPacket* pkt) -> bool {
        if ((r >= 0) || (r == AVERROR(EAGAIN)) || (r == AVERROR_EOF)) {
            return false;
        }
        // With frame threads (or the decoder's own threads, e.g. libdav1d), the error of a packet is
        // returned later, with another packet: it is unknown.
        const bool frameThreads = stream->frameThreadsActive();
        const int64_t pts = pkt ? pkt->pts : int64_t(AV_NOPTS_VALUE);
        if (noteDamage(r, pts, frameThreads ? DamageSource::ePacketFrameThreads : DamageSource::ePacket) != DamageAction::eSkip) {
            return false; // fail with this error (intra-only: decode() then decodes from earlier frames)
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

    const bool frameThreadsActive = stream->frameThreadsActive();

    // Fail with an error. After too many damaged packets, the decoder may still hold good frames (e.g. the
    // frames waiting for their B-frames, or with frame threads the frames being decoded): drain it first, which
    // may give the requested frame, or the frames around it (a gap). With frame threads, the frames of the last
    // packets sent are not used: decoding with 1 thread would have stopped before them, and the result must not
    // depend on the threads. The decoder is then drained: the next requests that need more frames seek
    // (Stream::_drainedEarly).
    // (The window is thread_count packets. A decoder with its own threads, AV_CODEC_CAP_OTHER_THREADS, e.g.
    // libdav1d, may have a longer delay, e.g. its max_frame_delay: then a few frames sent after the last damaged
    // packet may still be used, and the result may depend on the threads.)
    // Drains the decoder (the end of the stream is sent): its frames, at most 4 * kMaxDroppedPacketsPerDecode, go to
    // processDecodedFrame(), except the damaged ones and, with skipRecent, those that frame threads decoded from the
    // last packets sent (see fail()). Returns true if processDecodedFrame() stopped decoding (it gave the requested
    // frame, or decoding should stop).
    auto drainAll = [&](bool skipRecent) -> bool {
        (void)avcodec_send_packet(stream->_codecContext, nullptr);
        for (int i = 0; i < 4 * kMaxDroppedPacketsPerDecode; ++i) {
            const int rr = avcodec_receive_frame(stream->_codecContext, avFrameDecodeDst);
            if ((rr == AVERROR(EAGAIN)) || (rr == AVERROR_EOF)) {
                break;
            }
            if (rr < 0) {
                continue; // a damaged frame
            }
            if (skipRecent && (avFrameDecodeDst->pts != int64_t(AV_NOPTS_VALUE)) && frameThreadsActive && (std::find(stream->_recentPts.begin(), stream->_recentPts.end(), avFrameDecodeDst->pts) != stream->_recentPts.end())) {
                // With frame threads, the error of the last damaged packet came with up to thread_count packets
                // sent after it, which decoding with 1 thread would not have read: their frames are not used.
                continue;
            }
            if (processDecodedFrame(avFrameDecodeDst)) {
                return true;
            }
        }

        return false;
    };
    auto fail = [&](int r, const char* prefix) -> bool {
        if (stream->_tooManyDroppedPackets && !stream->_drainedEarly) {
            stream->_drainedEarly = true;
            if (drainAll(/*skipRecent=*/true) && hasPicture) {
                return true;
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
    while (resendPending || ((res = readPacket(*stream, avPacket.pkt())) >= 0)) {
        const bool resending = resendPending;
        resendPending = false;
        if (stream->_restartSingleThread) {
            return hasPicture; // decode() decodes again with 1 thread
        }
        if (!resending) {
            notePacketEnd(*stream, *avPacket.pkt());
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
        // Not while the restart keyframe is not verified yet (Stream::_restartPts, see processDecodedFrame): with
        // B-frames, its frame is output after the next packets, which may also have the keyframe flag (e.g. in a MOV
        // without a sync-sample table, libavformat/mov.c:4947 in FFmpeg 9.0.2, or a Matroska block whose header is
        // damaged): the drain would give its frame, and then lose the decoder state that the next frames need, so
        // that the decoder restarted at a leading picture after the real keyframe (issue #441).
        if (!resending && !decodeAsIntraOnly && stream->_tainted && (avPacket->flags & AV_PKT_FLAG_KEY) && !stream->_verifyNextPacket &&
            (stream->_restartPts == int64_t(AV_NOPTS_VALUE))) {
            const bool stop = drainAll(/*skipRecent=*/false);
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
            stream->packetSent(*avPacket.pkt(), frameThreadsActive ? (std::max)(1, stream->_codecContext->thread_count) : 1);
            stream->attachPalette(avPacket.pkt());
        }
        res = avcodec_send_packet(stream->_codecContext, avPacket.pkt());
        if ((res >= 0) && !decodeAsIntraOnly) {
            // Long-GOP codecs (decoded with 1 thread after damage, see noteDamage): an accepted packet ends a
            // run of damaged packets, also if its frame is not output (e.g. the H.264 decoder only outputs the
            // frames after a restart once recovered, libavcodec/h264_slice.c:1374-1384), so that the bound
            // (kMaxDroppedPacketsPerDecode) does not depend on what the decoder outputs, which depends on the read order.
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
                // which sends it before reading the next one (a seek drops it, Stream::resetSeekState()). It was
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
        // A demuxer may also drop the partial last packet of a truncated file without an error, e.g. Matroska (n8.0
        // libavformat/matroskadec.c:1099-1104, 1534-1551, 901-902): if the packets of all the streams end more than one
        // frame (a duration may be rounded up) before the end of the movie, the frames after them fail too. Not with an
        // end estimated from the bit rate, or timestamp discontinuities (MPEG-TS flags the partial packet instead).
        const int64_t lastFrame = stream->_frames - 1;
        if ((_packetsEnd != int64_t(AV_NOPTS_VALUE)) && !HasTsDiscontinuities(_context) &&
            (_context->duration_estimation_method != AVFMT_DURATION_FROM_BITRATE) && (lastFrame > 0) &&
            (lastFrame <= std::numeric_limits<int>::max())) {
            // Compared in int64, in the time base of stream, before narrowing to a frame (ES.46)
            const int64_t endPts = av_rescale_q_rnd(_packetsEnd, AV_TIME_BASE_Q, stream->_avstream->time_base,
                                                    AVRounding(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            if ((endPts >= stream->_startPTS) && (endPts < stream->frameToPts(int(lastFrame)))) {
                const int missingFrom = stream->ptsToFrame(endPts); // in [0, lastFrame]
                if ((missingFrom < lastFrame) && ((stream->_truncatedFrame < 0) || (missingFrom < stream->_truncatedFrame))) {
                    stream->_truncatedFrame = missingFrom;
                }
            }
        }
        if ((stream->_truncatedFrame >= 0) && (frame >= stream->_truncatedFrame)) {
            setDecodeError("the file is truncated", "FFmpeg Reader: ");
            return false;
        }
        if (!decodeAsIntraOnly && stream->_tainted) {
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
            return imageConvert(stream, held, avFrameOut);
        }
        stream->_reachedEnd = true;
        markOnlyDamaged(); // e.g. a file damaged up to its end
    }

    return hasPicture;
}

bool
FFmpegFile::imageConvert(Stream* stream, AVFrame* avFrameIn, AVFrame* avFrameOut)
{
    if (!avFrameIn || !avFrameOut) {
        setDecodeError("mov64Reader no input or output frame provided for conversion");
        return false;
    }

    AVPixelFormat dstPixFmt = (AVPixelFormat)avFrameOut->format;
    AVPixelFormat srcPixFmt = (AVPixelFormat)avFrameIn->format;

    avFrameOut->pts = avFrameIn->pts;
    avFrameOut->pkt_dts = avFrameIn->pkt_dts;
    avFrameOut->duration = avFrameIn->duration;

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
        if ((stream->_convertCtxWhite16 != kWhite16) || (stream->_convertCtxOpaque16 != kWhite16)) {
            ScaleToWhite16(avFrameOut, stream->_convertCtxWhite16, stream->_convertCtxOpaque16);
        }
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
    Stream* stream = _streams[streamIdx].get();
    // guard against division by zero
    assert(stream->_fpsDen);
    fps = stream->_fpsDen ? ((double)stream->_fpsNum / stream->_fpsDen) : stream->_fpsNum;

    return true;
}

// get stream information
bool
FFmpegFile::getInfo(int streamIndex,
                    int& width,
                    int& height,
                    double& aspect,
                    int& frames) const
{
    // set by the constructor only: no lock needed
    const Stream* stream = getStream(streamIndex);
    if (!stream) {
        return false;
    }
    width = stream->_width;
    height = stream->_height;
    aspect = stream->_aspect;
    frames = (int)stream->_frames;

    return true;
}

FFmpegFileManager::FFmpegFileManager()
    : _files()
    , _lock()
{
}

FFmpegFileManager::~FFmpegFileManager()
{
    // shared_ptr entries free their FFmpegFile when the map is cleared.
    _files.clear();
}

void
FFmpegFileManager::init()
{
    _lock = std::make_unique<FFmpegFile::Mutex>();
}

void
FFmpegFileManager::clear(void const* plugin)
{
    // The files are destroyed (avformat_close_input(), decoder threads joined) after the lock is released, so that
    // other instances don't wait for it (CP.43). Any FFmpegFile still referenced by a concurrent caller stays alive
    // until that caller releases its shared_ptr.
    std::list<std::shared_ptr<FFmpegFile>> files;
    {
        assert(_lock);
        FFmpegFile::AutoMutex guard(*_lock);
        FilesMap::iterator found = _files.find(plugin);
        if (found != _files.end()) {
            files.swap(found->second);
            _files.erase(found);
        }
    }
}

std::shared_ptr<FFmpegFile>
FFmpegFileManager::find(void const* plugin,
                        const string& filename,
                        std::shared_ptr<FFmpegFile>& evicted) const
{
    FilesMap::iterator found = _files.find(plugin);
    if (found != _files.end()) {
        for (std::list<std::shared_ptr<FFmpegFile>>::iterator it = found->second.begin(); it != found->second.end(); ++it) {
            if ((*it)->getFilename() == filename) {
                if ((*it)->isInvalid()) {
                    evicted = std::move(*it);
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
FFmpegFileManager::get(void const* plugin,
                       const string& filename) const
{
    if (filename.empty() || !plugin) {
        return nullptr;
    }
    std::shared_ptr<FFmpegFile> evicted; // destroyed after the lock is released (declared before guard)
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);

    return find(plugin, filename, evicted);
}

std::shared_ptr<FFmpegFile>
FFmpegFileManager::getOrCreate(void const* plugin,
                               const string& filename) const
{
    if (filename.empty() || !plugin) {
        return nullptr;
    }
    std::shared_ptr<FFmpegFile> evicted; // destroyed after the lock is released (declared before guard)
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    if (std::shared_ptr<FFmpegFile> file = find(plugin, filename, evicted)) {
        return file;
    }

    std::shared_ptr<FFmpegFile> file = std::make_shared<FFmpegFile>(filename);
    _files[plugin].push_back(file);

    return file;
}
