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
 * OFX ffmpegWriter plugin.
 * Writes a video output file using the libav library.
 * Synced with mov64Writer 12.2v5
 */

// to test writing a codec with ffmpeg (e.g. DNxHD with dnxhr_lb profile):
// ffmpeg -f lavfi  -i testsrc=size=2048x1152 -vframes 2 -vcodec dnxhd -profile:v dnxhr_lb out.mov

// to see private options of a codec:
// ffmpeg -h encoder=mjpeg

#if (defined(_STDINT_H) || defined(_STDINT_H_) || defined(_MSC_STDINT_H_)) && !defined(UINT64_C)
#warning "__STDC_CONSTANT_MACROS has to be defined before including <stdint.h>, this file will probably not compile."
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS // ...or stdint.h wont' define UINT64_C, needed by libavutil
#endif

#include <algorithm>
#include <array>
#include <cctype> // ::tolower
#include <cfloat> // DBL_MAX
#include <chrono> // std::chrono::milliseconds
#include <climits> // INT_MAX
#include <condition_variable>
#include <cmath> // std::ceil, std::fabs
#include <cstdio>
#include <cstring> // strncpy
#include <filesystem> // std::filesystem::weakly_canonical
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility> // std::move
#ifdef DEBUG
#include <cstdio>
#define DBG(x) x
#else
#define DBG(x) (void)0
#endif

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define NOMINMAX 1
// windows - defined for both Win32 and Win64
#include <windows.h> // for GetSystemInfo()
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif
#else
#include <time.h>
#include <unistd.h> // for sysconf()
#endif

extern "C" {
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/timecode.h>
#include <libswscale/swscale.h>
}
#include "IOUtility.h"
#include "ofxsFileOpen.h"
#include "ofxsMacros.h"

#ifdef OFX_IO_USING_OCIO
#include "GenericOCIO.h"
#endif
#include "FFmpegFile.h"
#include "GenericWriter.h"
#include "PixelFormat.h"

#define OFX_FFMPEG_PRINT_CODECS 0 // print list of supported/ignored codecs and formats
#define OFX_FFMPEG_AUDIO 0 // audio support
#define OFX_FFMPEG_MBDECISION 0 // add the macroblock decision parameter

#if OFX_FFMPEG_PRINT_CODECS
#include <iostream>
#endif

#include "ofxsMultiThread.h"
#ifdef OFX_USE_MULTITHREAD_MUTEX
namespace {
typedef OFX::MultiThread::Mutex Mutex;
typedef OFX::MultiThread::AutoMutex AutoMutex;
}
#else
// some OFX hosts do not have mutex handling in the MT-Suite (e.g. Sony Catalyst Edit)
namespace {
typedef std::mutex Mutex;
typedef std::lock_guard<std::mutex> AutoMutex;
}
#endif

using namespace OFX;
using namespace OFX::IO;

using std::map;
using std::max;
using std::string;
using std::stringstream;
using std::vector;

OFXS_NAMESPACE_ANONYMOUS_ENTER

// The pixel formats supported by an encoder, terminated by AV_PIX_FMT_NONE, or nullptr if unknown.
// AVCodec::pix_fmts is deprecated since FFmpeg 7.1 (libavcodec 61.13) and removed in FFmpeg 9:
// use avcodec_get_supported_config().
static const AVPixelFormat*
codecPixelFormats(const AVCodec* codec)
{
    if (!codec) {
        return nullptr;
    }
    const void* configs = nullptr;
    int numConfigs = 0;
    if ((avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &numConfigs) < 0) || (numConfigs <= 0)) {
        return nullptr;
    }

    return static_cast<const AVPixelFormat*>(configs);
}

// The pixel formats that WriteFFmpeg writes rawvideo with in a container, terminated by AV_PIX_FMT_NONE, or nullptr if
// rawvideo is not written in this container. The rawvideo encoder takes any pixel format (it lists none), but a
// container can only store the ones it has a tag for. getPixelFormatsFromList() selects one of them from the Pref.
// Pixel Coding, Bit Depth and Alpha params, as for the other encoders; when two have the same coding, depth and alpha,
// the first one is selected.
// - QuickTime: only the pixel formats of the mov muxer's rawvideo tag table (mov_pix_fmt_tags, FFmpeg n9.0
//   libavformat/movenc.c:2052-2075, n8.0 movenc.c:1955-1978, n4.4 movenc.c:1598-1618), which the demuxer maps back to
//   rawvideo (libavformat/isom_tags.c:32-38 in n9.0): 2vuy (UYVY422, 8-bit 4:2:2), 'raw ' (RGB24, and ARGB for RGBA:
//   it is QuickTime's k32ARGBPixelFormat, the RGBA, BGRA and ABGR tags are not as widely read), and
//   v308 (VYU444), v408 (UYVA) and v410 (V30XLE), which give the same files as the v308, v408 and v410 encoders that
//   FFmpeg 9 removed (the mov muxer tags them from FFmpeg 8.0).
//   There is no RGB with more than 8 bits: b48r (RGB48BE) is not listed, because AVFoundation (QuickTime Player, macOS
//   27.2) reads its 16-bit samples byte-swapped (FFmpeg reads them correctly), and movenc has no b64a tag. RGB with
//   10 to 16 bits gets 8-bit RGB ('raw '), with a warning (rawvideoFallbackMessage()).
//   Any other pixel format would get the 'raw ' tag and be unreadable (movenc.c:2101-2108 in n9.0).
//   'raw ' RGB24 rows are padded to an even number of bytes by the muxer (is_unaligned_qt_rgb, movenc.c:8503-8516 and
//   7686-7692 in n9.0, libavformat/rawutils.c:27-67), but the rawvideo decoder only realigns RGB24 rows to 4 bytes
//   (rawdec.c:172 and 382-392 in n9.0, 178 and 407-417 in n4.4; the mov demuxer only gives it the depth, from which it
//   gets the pixel format, mov.c:2871, rawdec.c:77-80), so that with an odd width FFmpeg reads sheared images when
//   the width is 3 modulo 4, and QuickTime reads none. With an odd width, ARGB (whose rows are never padded) is
//   written instead (see rawvideoPixelFormatForWidth()).
// - NUT: the usual planar Y'CbCr and packed RGB formats, which the rawvideo encoder tags with
//   avcodec_pix_fmt_to_codec_tag() (libavcodec/rawenc.c:42-43, libavcodec/raw_pix_fmt_tags.h in n9.0), which the
//   NUT muxer keeps (its default rawvideo tag is not 0, libavformat/nut.c:44-50, so mux.c:304-313 does not reset
//   them), and which the demuxer gives back to the rawvideo decoder.
//   All the formats of these lists were checked to round-trip exactly with FFmpeg 4.4 and 9.
// - AVI: BI_RGB (BGR24, BGRA, whose rows the muxer writes bottom-up, libavformat/avienc.c:791-797 in n9.0) and the
//   Y'CbCr FourCCs of the RIFF table (I420 and UYVY, libavformat/riff.c:221-255 in n9.0): the muxer resets any other
//   rawvideo tag to BI_RGB (libavformat/mux.c:304-313 in n9.0), which would make the file unreadable. (Y42B, yuv422p,
//   is not listed: it has the same coding, depth and size as UYVY, which comes first, so it would never be selected.)
// - Matroska: the same Y'CbCr FourCCs, since the muxer checks the tags against the RIFF table too (mux.c:304-313),
//   and writes them as the ColourSpace of the V_UNCOMPRESSED codec (libavformat/matroskaenc.c:1922-1925 in n9.0).
//   Raw RGB, whose tag is reset to BI_RGB, is refused ("Raw RGB is not supported Natively in Matroska",
//   matroskaenc.c:2031-2044 in n9.0).
static const AVPixelFormat*
rawvideoPixelFormats(const AVOutputFormat* fmt)
{
    if (!fmt || !fmt->name) {
        return nullptr;
    }
    static const AVPixelFormat movFormats[] = {
        AV_PIX_FMT_UYVY422, // 2vuy
        AV_PIX_FMT_VYU444, // v308
        AV_PIX_FMT_UYVA, // v408
        AV_PIX_FMT_V30XLE, // v410
        AV_PIX_FMT_RGB24, // raw (24 bits)
        AV_PIX_FMT_ARGB, // raw (32 bits)
        AV_PIX_FMT_NONE
    };
    static const AVPixelFormat nutFormats[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_YUV422P10LE,
        AV_PIX_FMT_YUV444P10LE,
        AV_PIX_FMT_YUVA444P10LE,
        AV_PIX_FMT_YUV422P16LE,
        AV_PIX_FMT_YUV444P16LE,
        AV_PIX_FMT_YUVA444P16LE,
        AV_PIX_FMT_RGB24,
        AV_PIX_FMT_RGBA,
        AV_PIX_FMT_RGB48LE,
        AV_PIX_FMT_RGBA64LE,
        AV_PIX_FMT_NONE
    };
    static const AVPixelFormat aviFormats[] = {
        AV_PIX_FMT_YUV420P, // I420
        AV_PIX_FMT_UYVY422, // UYVY
        AV_PIX_FMT_BGR24, // BI_RGB (24 bits)
        AV_PIX_FMT_BGRA, // BI_RGB (32 bits)
        AV_PIX_FMT_NONE
    };
    static const AVPixelFormat mkvFormats[] = {
        AV_PIX_FMT_YUV420P, // I420
        AV_PIX_FMT_UYVY422, // UYVY
        AV_PIX_FMT_NONE
    };
    if (!strcmp(fmt->name, "mov")) {
        return movFormats;
    }
    if (!strcmp(fmt->name, "nut")) {
        return nutFormats;
    }
    if (!strcmp(fmt->name, "matroska")) {
        return mkvFormats;
    }
    if (!strcmp(fmt->name, "avi")) {
        return aviFormats;
    }

    return nullptr;
}

// The pixel formats that an encoder can write in a container (see codecPixelFormats() and rawvideoPixelFormats()).
static const AVPixelFormat*
encoderPixelFormats(const AVCodec* codec,
                    const AVOutputFormat* fmt)
{
    if (codec && (codec->id == AV_CODEC_ID_RAWVIDEO)) {
        return rawvideoPixelFormats(fmt);
    }

    return codecPixelFormats(codec);
}

// The pixel format that rawvideo writes for an image of this width (see rawvideoPixelFormats()): in QuickTime, RGB24
// with an odd width is replaced by ARGB (opaque if there is no alpha), because the muxer pads the 'raw ' RGB24 rows
// to an even number of bytes, which neither FFmpeg nor QuickTime reads correctly.
static AVPixelFormat
rawvideoPixelFormatForWidth(const AVOutputFormat* fmt,
                            const AVCodec* codec,
                            AVPixelFormat pixelFormat,
                            int width)
{
    if (codec && (codec->id == AV_CODEC_ID_RAWVIDEO) && fmt && fmt->name && !strcmp(fmt->name, "mov")
        && (pixelFormat == AV_PIX_FMT_RGB24) && (width % 2 != 0)) {
        return AV_PIX_FMT_ARGB;
    }

    return pixelFormat;
}

static const char*
pixelCodingName(FFmpeg::PixelCodingEnum coding)
{
    switch (coding) {
    case FFmpeg::ePixelCodingYUV420:
        return "Y'CbCr 4:2:0";
    case FFmpeg::ePixelCodingYUV440:
        return "Y'CbCr 4:4:0";
    case FFmpeg::ePixelCodingYUV422:
        return "Y'CbCr 4:2:2";
    case FFmpeg::ePixelCodingYUV444:
        return "Y'CbCr 4:4:4";
    case FFmpeg::ePixelCodingRGB:
        return "RGB";
    case FFmpeg::ePixelCodingXYZ:
        return "XYZ";
    default:
        return "?";
    }
}

// rawvideo: the warning shown when the container has no pixel format with the Pref. Pixel Coding, Bit Depth and
// Alpha (see rawvideoPixelFormats(): e.g. QuickTime has no RGB with more than 8 bits, and Matroska has no 4:4:4 and no
// alpha), or an empty string.
static string
rawvideoFallbackMessage(const AVOutputFormat* fmt,
                        const AVCodec* codec,
                        FFmpeg::PixelCodingEnum prefCoding,
                        int prefBitDepth,
                        bool prefAlpha,
                        AVPixelFormat target)
{
    const char* name = av_get_pix_fmt_name(target);
    if (!fmt || !fmt->name || !codec || (codec->id != AV_CODEC_ID_RAWVIDEO) || !name) {
        return string();
    }
    const int depth = FFmpeg::pixelFormatBitDepth(target);
    const FFmpeg::PixelCodingEnum coding = FFmpeg::pixelFormatCoding(target);
    const bool alpha = FFmpeg::pixelFormatAlpha(target);
    if ((depth >= prefBitDepth) && ((int)coding >= (int)prefCoding) && (alpha || !prefAlpha)) {
        return string();
    }
    string msg = string("rawvideo has no pixel format with ") + pixelCodingName(prefCoding) + ", " + std::to_string(prefBitDepth)
        + " bits" + (prefAlpha ? " and alpha" : "") + " in the " + fmt->name + " container: the " + name + " pixel format ("
        + pixelCodingName(coding) + ", " + std::to_string(depth) + " bits" + (alpha ? ", with alpha" : "") + ") is written.";
    if (!strcmp(fmt->name, "mov") && (prefCoding == FFmpeg::ePixelCodingRGB) && (prefBitDepth > 8)) {
        msg += " (16-bit RGB, b48r, is not written: QuickTime reads it byte-swapped.)";
    }

    return msg;
}

// The warning about a project saved with an encoder that this FFmpeg does not have, which checkCodec() replaced by
// rawvideo (see replaceMissingCodec()), with the pixel format that rawvideo writes in the container fmt (if known).
static string
codecReplacedMessage(const string& oldCodec,
                     const AVOutputFormat* fmt,
                     AVPixelFormat target)
{
    const bool ayuv = (oldCodec == "ayuv");
    const bool alpha = ayuv || (oldCodec == "v408");
    // v308, v408 and v410: FFmpeg n8.0 libavcodec/allcodecs.c:354-359 lists them, n9.0 does not.
    // ayuv: FFmpeg n6.1 libavcodec/allcodecs.c:65-66 lists it, n7.0 does not.
    string msg = string("The ") + oldCodec + " encoder of this project is not available in this version of FFmpeg (FFmpeg "
        + (ayuv ? "7" : "9") + " removed it): the codec was replaced by rawvideo, with Pref. Pixel Coding YUV444, Bit Depth "
        + (oldCodec == "v410" ? "10" : "8") + " and Alpha " + (alpha ? "checked" : "unchecked") + ".";
    const char* name = av_get_pix_fmt_name(target);
    if (fmt && fmt->name && name) {
        const char* tag = nullptr;
        if (!strcmp(fmt->name, "mov")) {
            tag = (target == AV_PIX_FMT_VYU444) ? "v308" : (target == AV_PIX_FMT_UYVA) ? "v408" : (target == AV_PIX_FMT_V30XLE) ? "v410" : nullptr;
        }
        msg += string(" In the ") + fmt->name + " container, rawvideo writes the " + name + " pixel format";
        if (tag) {
            msg += string(" with the ") + tag + " tag";
            if (oldCodec == tag) {
                msg += ", as the " + oldCodec + " encoder did";
            }
        }
        msg += ".";
    }

    return msg;
}


#define kPluginName "WriteFFmpeg"
#define kPluginGrouping "Image/Writers"
#define kPluginDescription                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                \
    "Write a video sequence using FFmpeg.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "This plugin can be used to produce entheir digital intermediates, i.e. videos with very high resolution and quality which can be read frame by frame for further processing, or highly compressed videos to distribute on the web. Note that this plug-in does not support audio, but audi can easily be added to the video using the ffmpeg command-line tool (see note below).  In a VFX context, it is often preferable to save processed images as a sequence of individual frames (using WriteOIIO), if disk space and real-time playing are not an issue.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "The preferred pixel coding (Pref. Pixel Coding) and bit depth (Pref. Bit Depth) can be selected. This is especially useful for codecs that propose multiple pixel formats (e.g. ffv1, ffvhuff, huffyuv, jpeg2000, mjpeg, mpeg2video, vc2, libopenjpeg, png, qtrle, targa, tiff, libschroedinger, libtheora, libvpx, libvpx-vp9, libx264, libx265).\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                \
    "The pixel format is selected from the available choices for the chosen codec using the following rules:\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           \
    "- First, try to find the format with the smallest BPP (bits per pixel) that fits into the preferences.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            \
    "- Second, if the bit depth is not available (e.g. with a libx265 compiled for 8-bit only), get the format that fits the pixel coding and alpha preferences with the highest bit depth.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            \
    "- Third, if no format fits, get the format that has a BPP equal or a bit higher that the one computed from the preferences.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       \
    "- Last, if no such format is found, get the format that has the highest BPP.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      \
    "A format with alpha is only selected if Alpha is checked (or if the codec only has formats with alpha).\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           \
    "The selected pixel coding, bit depth, and BPP are displayed in the Selected Pixel Coding, Bit Depth, and BPP parameters.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "The recommended Codec/Container configurations for encoding digital intermediates are (see also https://trac.ffmpeg.org/wiki/Encode/VFX):\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         \
    "- ProRes inside QuickTime: all ProRes profiles are 10-bit and are intra-frame (each frame is encoded separately). ProRes 4444 and 4444 XQ can also encode the alpha channel.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      \
    "- Avid DNxHR inside QuickTime: the codec is intra-frame. DNxHR profiles are resolution-independent and are available with 8-bit or 10-bit depth. The alpha channel cannot be encoded.\n" \
    "- Uncompressed inside QuickTime: v210 (10-bit 4:2:2, even widths only), or raw (rawvideo), whose pixel format is selected with the Pref. Pixel Coding, Bit Depth and Alpha params among the ones QuickTime has a tag for: 2vuy (8-bit 4:2:2), raw (8-bit RGB, or ARGB with alpha or if the width is odd), v308 (8-bit 4:4:4), v408 (8-bit 4:4:4:4) and v410 (10-bit 4:4:4), which replace the v308, v408 and v410 encoders that FFmpeg 9 removed. QuickTime has no uncompressed RGB with more than 8 bits that it reads correctly (it reads b48r byte-swapped): 8-bit RGB is written, with a warning. QuickTime Player (AVFoundation) only decodes 2vuy, raw and v410 if the width is a multiple of 8, and does not decode v308 and v408. In NUT, rawvideo writes planar Y'CbCr 4:2:0, 4:2:2 or 4:4:4 (8, 10 or 16 bits, with or without alpha) or packed RGB (8 or 16 bits, with or without alpha); in AVI, I420, UYVY or BI_RGB (BGR, BGRA); in Matroska, I420 or UYVY (8-bit Y'CbCr).\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             \
    "- HEVC (hev1/libx265) inside Matroska, MP4, QuickTime or MPEG-TS and Output Quality set to Lossless or Perceptually Lossless. libx265 supports 8-bit, 10-bit and 12-bit depth (if libx265 was compiled with high bit depth support). Lossless may not be playable in real-time for high resolutions. Set the Encoding Speed to Ultra Fast for faster encoding but worse compression, or Very Slow for best compression.\n" /*"- Photo JPEG inside MP4 (or QuickTime) and Global Quality set to 1: intra-frame and 8-bit. qscale=1 ensure almost lossless encoding.\n"*/ /*"- H.264 (avc1/libx264/libx264rgb) inside MP4 or QuickTime and Output Quality set to Lossless or Perceptually Lossless: 10-bit(*), and can be made intra-frame by setting Keyframe Interval to 1 and Max B-Frames to 0. Lossless may not be playable in real-time for high resolutions. Set the Encoding Speed to Ultra Fast for faster encoding but worse compression, or Very Slow for best compression.\n"*/ /*"\n"*/ /*"(*) The H.264 output may be 8-bit if libx264 was not compiled with 10-bit support. Check the information displayed by clicking on the Show Avail. button.\n"*/ \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "To write videos intended for distribution (as media files or for streaming), the most popular codecs are mp4v (mpeg4 or libxvid), avc1 (libx264), H264 (libopenh264), hev1 (libx265), VP80 (libvpx) and VP90 (libvpx-vp9). The quality of mp4v may be set using the Global Quality parameter (between 1 and 31, 1 being the highest quality), and the quality of avc1, hev1, VP80 and VP90 may be set using the Output Quality parameter. More information can be found at https://trac.ffmpeg.org/wiki#Encoding\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "If the output video should be encoded with specific FFmpeg options, such as a given pixel format or encoding option, it is better to write the output as individual frames in an image format that has a sufficient bit depth, and to encode the set of individual frames to a video using the command-line ffmpeg tool.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "The settings for the \"Global Quality\" and \"Quality\" parameters may have different meanings for different codecs. See http://slhck.info/video/2017/02/24/vbr-settings.html for a summary of recommended values. Using these settings should be preferred over constant bitrate-based encoding, as it usually gives a much better result.\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       \
    "\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  \
    "Adding audio\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      \
    "If synchronized audio is available as a separate file, encoded with the right codec, it can be easily added to the video using a command like: ffmpeg -i input.mp4 -i input.mp3 -c copy -map 0:0 -map 1:0 output.mp4 (in this example, input.mp4 contains the video, input.mp3 contains the audio, and output.mp4 co,ntains both tracks).\n"                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         \
    "This command does not re-encode the video or audio, but simply copies the data from each source file and places it in separate streams in the output."

#define kPluginIdentifier "fr.inria.openfx.WriteFFmpeg"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 2 // Increment this when you have fixed a bug or made it faster.
#define kPluginEvaluation 0 // plugin quality from 0 (bad) to 100 (perfect) or -1 if not evaluated

#define kSupportsRGBA true
#define kSupportsRGB true
#define kSupportsAlpha false
#define kSupportsXY false

#define kParamFormat "format"
#define kParamFormatLabel "Container"
#define kParamFormatHint "Output format/container."

#define kParamCodec "codec"
#define kParamCodecName "Codec"
#define kParamCodecHint "Output codec used for encoding. "                                                                     \
                        "The general recommendation is to write either separate frames (using WriteOIIO), "                    \
                        "or an uncompressed video format, or a \"digital intermediate\" format (ProRes, DNxHD), "              \
                        "and to transcode the output and mux with audio with a separate tool (such as the ffmpeg or mencoder " \
                        "command-line tools).\n"                                                                               \
                        "The FFmpeg encoder codec name is given between brackets at the end of each codec description.\n"      \
                        "Uncompressed in QuickTime: v210 needs an even width. QuickTime Player (AVFoundation) only decodes "    \
                        "rawvideo (2vuy, raw, v410) if the width is a multiple of 8 pixels (v210 pads its rows, so any even "  \
                        "width works), and does not decode v308 and v408 (FFmpeg reads all of them).\n"                        \
                        "Please refer to the FFmpeg documentation http://ffmpeg.org/ffmpeg-codecs.html for codec options."

// a string param holding the short name of the codec (used to disambiguiate the codec choice when using different versions of FFmpeg)
#define kParamCodecShortName "codecShortName"
#define kParamCodecShortNameLabel "Codec Name"
#define kParamCodecShortNameHint "The codec used when the writer was configured. If this parameter is visible, this means that this codec may not be supported by this version of the plugin."

#define kParamFPS "fps"
#define kParamFPSLabel "FPS"
#define kParamFPSHint "File frame rate"

#define kParamPrefPixelCoding "prefPixelCoding"
#define kParamPrefPixelCodingLabel "Pref. Pixel Coding", "Preferred pixel coding."
#define kParamPrefPixelCodingOptionYUV420 "YUV420", "1 Cr & Cb sample per 2x2 Y samples.", "yuv420"
#define kParamPrefPixelCodingOptionYUV422 "YUV422", "1 Cr & Cb sample per 2x1 Y samples.", "yuv422"
#define kParamPrefPixelCodingOptionYUV444 "YUV444", "1 Cr & Cb sample per Y sample.", "yuv444"
#define kParamPrefPixelCodingOptionRGB "RGB", "Separate r, g, b.", "rgb"
#define kParamPrefPixelCodingOptionXYZ "XYZ", "CIE XYZ compressed with gamma=2.6, used for Digital Cinema.", "xyz"
enum PrefPixelCodingEnum {
    ePrefPixelCodingYUV420 = 0, // 1 Cr & Cb sample per 2x2 Y samples
    ePrefPixelCodingYUV422, // 1 Cr & Cb sample per 2x1 Y samples
    ePrefPixelCodingYUV444, // 1 Cr & Cb sample per 1x1 Y samples
    ePrefPixelCodingRGB, // RGB
    ePrefPixelCodingXYZ, // XYZ
};

#define kParamPrefBitDepth "prefBitDepth"
#define kParamPrefBitDepthLabel "Bit Depth", "Preferred bit depth (number of bits per component)."
enum PrefBitDepthEnum {
    ePrefBitDepth8 = 0,
    ePrefBitDepth10,
    ePrefBitDepth12,
    ePrefBitDepth16,
};

#define kParamPrefAlpha "enableAlpha" // enableAlpha rather than prefAlpha for backward compat
#define kParamPrefAlphaLabel "Alpha", "If checked, and the input contains alpha, formats with an alpha channel are preferred."

#define kParamPrefShow "prefShow"
#define kParamPrefShowLabel "Show Avail.", "Show available pixel codings for this codec."

#define kParamInfoPixelFormat "infoPixelFormat"
#define kParamInfoPixelFormatLabel "Selected Pixel Coding", "Pixel coding of images passed to the encoder. If several pixel codings are available, the coding which causes less data loss is selected. Other pixel formats may be available by transcoding with ffmpeg on the command-line, as can be seen by executing 'ffmpeg --help encoder=codec_name' on the command-line."

#define kParamInfoBitDepth "infoBitDepth"
#define kParamInfoBitDepthLabel "Bit Depth", "Bit depth (number of bits per component) of the pixel format."

#define kParamInfoBPP "infoBpp"
#define kParamInfoBPPLabel "BPP", "Bits per pixel of the pixel format."

#define kParamResetFPS "resetFps"
#define kParamResetFPSLabel "Reset FPS"
#define kParamResetFPSHint "Reset FPS from the input FPS."

#define kParamFastStart "fastStart"
#define kParamFastStartLabel "Fast Start"
#define kParamFastStartHint                                                     \
    "Write decoding critical metadata (moov atom) at beginning of the file to " \
    "allow playback when streaming."

#define kParamStartTimecode "startTimecode"
#define kParamStartTimecodeLabel "Start Timecode"
#define kParamStartTimecodeHint                                                        \
    "Timecode of the first frame of the movie, as HH:MM:SS:FF (hours, minutes, "       \
    "seconds, frames), or HH:MM:SS;FF for a drop-frame timecode (only at 29.97 fps "   \
    "and its multiples). The frame number FF must be less than the frame rate, "       \
    "rounded to the nearest integer.\n"                                                \
    "It is written as the \"timecode\" metadata of the file and of its video stream: " \
    "QuickTime (mov) and MP4 files get a timecode (tmcd) track (unless the Format "    \
    "Options contain write_tmcd=0), and other formats that support it (e.g. MXF) "     \
    "write it in their own way. A tmcd track can not have a timecode above 255 fps.\n" \
    "Leave empty to write no timecode."

#define kParamAdvanced "advanced"
#define kParamAdvancedLabel "Advanced"

#define kParamCRF "crf"
#define kParamCRFLabel "Output Quality"
#define kParamCRFHint "Constant Rate Factor (CRF); tradeoff between video quality and file size. Used by the encoders with a crf option, e.g. libx264 and libx264rgb (avc1), libx265 (hev1), libvpx (VP80), libvpx-vp9 (VP9), libaom-av1 (AV1) and libxavs (CAVS). libvpx and libaom-av1 use a 0-63 scale. Lossless (CRF 0) is only lossless with the encoders that have a lossless mode, e.g. libx264 and libvpx-vp9.\n" \
                      "Option -crf in ffmpeg."
#define kParamCRFOptionNone "None", "Use constant bit-rate rather than constant output quality", "none"
#define kParamCRFOptionLossless "Lossless", "Corresponds to CRF = 0.", "crf0"
#define kParamCRFOptionPercLossless "Perceptually Lossless", "Corresponds to CRF = 17.", "crf17"
#define kParamCRFOptionHigh "High Quality", "Corresponds to CRF = 20.", "crf20"
#define kParamCRFOptionMedium "Medium Quality", "Corresponds to CRF = 23.", "crf23"
#define kParamCRFOptionLow "Low Quality", "Corresponds to CRF = 26.", "crf26"
#define kParamCRFOptionVeryLow "Very Low Quality", "Corresponds to CRF = 29.", "crf29"
#define kParamCRFOptionLowest "Lowest Quality", "Corresponds to CRF = 32.", "crf32"
enum CRFEnum {
    eCRFNone = 0,
    eCRFLossless,
    eCRFPercLossless,
    eCRFHigh,
    eCRFMedium,
    eCRFLow,
    eCRFVeryLow,
    eCRFLowest,
};

#define kParamX26xSpeed "x26xSpeed"
#define kParamX26xSpeedLabel "Encoding Speed"
#define kParamX26xSpeedHint "Trade off performance for compression efficiency. Available for avc1 and hev1.\n" \
                            "Option -preset in ffmpeg."
#define kParamX26xSpeedOptionUltrafast "Ultra Fast", "Fast encoding, but larger file size.", "ultrafast"
#define kParamX26xSpeedOptionVeryfast "Very Fast", "", "veryfast"
#define kParamX26xSpeedOptionFaster "Faster", "", "faster"
#define kParamX26xSpeedOptionFast "Fast", "", "fast"
#define kParamX26xSpeedOptionMedium "Medium", "", "medium"
#define kParamX26xSpeedOptionSlow "Slow", "", "slow"
#define kParamX26xSpeedOptionSlower "Slower", "", "slower"
#define kParamX26xSpeedOptionVerySlow "Very Slow", "Slow encoding, but smaller file size.", "veryslow"
enum X26xSpeedEnum {
    eX26xSpeedUltrafast = 0,
    eX26xSpeedVeryfast,
    eX26xSpeedFaster,
    eX26xSpeedFast,
    eX26xSpeedMedium,
    eX26xSpeedSlow,
    eX26xSpeedSlower,
    eX26xSpeedVeryslow,
};

#define kParamQScale "qscale"
#define kParamQScaleLabel "Global Quality"
#define kParamQScaleHint "For lossy encoding, this controls image quality, from 0 to 100 (the lower, the better, 0 being near-lossless). For lossless encoding, this controls the effort and time spent at compressing more. -1 or negative value means to use the codec default or CBR (constant bit rate). Used for example by FLV1, mjp2, theo, jpeg, m2v1, mp4v MP42, 3IVD, codecs.\n" \
                         "Option -qscale in ffmpeg."

#define kParamBitrate "bitrateMbps"
#define kParamBitrateLabel "Bitrate"
#define kParamBitrateHint                                                                                                     \
    "The target bitrate the codec will attempt to reach (in Megabits/s), within the confines of the bitrate tolerance and "   \
    "quality min/max settings. Only supported by certain codecs (e.g. hev1, m2v1, MP42, 3IVD, but not mp4v, avc1 or H264).\n" \
    "Option -b in ffmpeg (multiplied by 1000000)."
#define kParamBitrateDefault 185
#define kParamBitrateMax 4000

#define kParamBitrateTolerance "bitrateToleranceMbps"
#define kParamBitrateToleranceLabel "Bitrate Tolerance"
#define kParamBitrateToleranceHint                                                                                                                                     \
    "Set video bitrate tolerance (in Megabits/s). In 1-pass mode, bitrate "                                                                                            \
    "tolerance specifies how far ratecontrol is willing to deviate from "                                                                                              \
    "the target average bitrate value. This is not related to min/max "                                                                                                \
    "bitrate. Lowering tolerance too much has an adverse effect on "                                                                                                   \
    "quality. "                                                                                                                                                        \
    "As a guideline, the minimum slider range of target bitrate/target fps is the lowest advisable setting. Anything below this value may result in failed renders.\n" \
    "Only supported by certain codecs (e.g. MP42, 3IVD, but not avc1, hev1, m2v1, mp4v or H264).\n"                                                                    \
    "A reasonable value is 5 * bitrateMbps / fps.\n"                                                                                                                   \
    "Option -bt in ffmpeg (multiplied by 1000000)."
#define kParamBitrateToleranceMax ((int)(INT_MAX / 1000000)) // tol*1000000 is stored in an int

#define kParamQuality "quality"
#define kParamQualityLabel "Quality"
#define kParamQualityHint                                                                                 \
    "The quality range the codec is allowed to vary the image data quantizer "                            \
    "between to attempt to hit the desired bitrate. The lower, the better: higher values mean increased " \
    "image degradation is possible, but with the upside of lower bit rates. "                             \
    "Only supported by certain codecs (e.g. VP80, VP90, avc1, but not hev1 or mp4v).\n"                   \
    "-1 means to use the codec default.\n"                                                                \
    "Good values are 12-23 for the least quality, 6-15 for low quality, 3-7 for medium quality, "         \
    "1-3 for high quality, and 1-1 for the best quality.\n"                                               \
    "Options -qmin and -qmax in ffmpeg."

#define kParamGopSize "gopSize"
#define kParamGopSizeLabel "Keyframe Interval"
#define kParamGopSizeHint                                                                                                                                                                    \
    "The keyframe intervale, also called GOP size, specifies how many frames may be grouped together by the codec to form a compression GOP. Exercise caution "                              \
    "with this control as it may impact whether the resultant file can be opened in other packages. Only supported by "                                                                      \
    "certain codecs.\n"                                                                                                                                                                      \
    "-1 means to use the codec default if bFrames is not 0, or 1 if bFrames is 0 to ensure only intra (I) frames are produced, producing a video which is easier to scrub frame-by-frame.\n" \
    "Option -g in ffmpeg."

#define kParamBFrames "bFrames"
#define kParamBFramesLabel "Max B-Frames"
#define kParamBFramesHint                                                                                                                                                                                                                                                                        \
    "Set max number of B frames between non-B-frames. Must be an integer between -1 and 16. 0 means that B-frames are disabled. If a value of -1 is used, it will choose an automatic value depending on the encoder. Influences file size and seekability. Only supported by certain codecs.\n" \
    "-1 means to use the codec default if Keyframe Interval is not 1, or 0 if Keyframe Interval is 1 to ensure only intra (I) frames are produced, producing a video which is easier to scrub frame-by-frame.\n"                                                                                 \
    "Option -bf in ffmpeg."

#define kParamWriteNCLC "writeNCLC"
#define kParamWriteNCLCLabel "Write NCLC"
#define kParamWriteNCLCHint \
    "Write nclc data in the colr atom of the video header. QuickTime only."

#define kParamOptionsSyntaxHint                                                                                                              \
    "Syntax: a list of key=value pairs, separated by colons or spaces, e.g. \"g=1:bf=0\" or \"g=1 bf=0\". "                                 \
    "This is FFmpeg's own option string syntax (see av_dict_parse_string), not the ffmpeg command line: write \"g=1\", not \"-g 1\". "     \
    "Every key must be followed by '=' and a value, and values cannot be empty (\"tune preset=slow\" and \"tune= preset=slow\" are errors). " \
    "Spaces around '=' are allowed (\"g = 1\" is \"g=1\"), but a value after a space cannot contain an unquoted '='. "                    \
    "A value that contains a colon, a space or a quote must be enclosed in single quotes (inside which a backslash is literal), "          \
    "or each such character escaped with a backslash, e.g. x264-params='keyint=1:bframes=0'. Double quotes are not supported.\n"          \
    "These options are applied last, so they override the values that the plug-in sets from its own parameters. "                         \
    "Options that the encoder or muxer does not apply (unknown keys, or generic options that it ignores) are reported in a warning "      \
    "message when the render starts (which may show a dialog in interactive hosts), and the file is written without them. "              \
    "An option that has an invalid value, or a syntax error, makes the render fail."

#define kParamCodecOptions "codecOptions"
#define kParamCodecOptionsLabel "Codec Options"
#define kParamCodecOptionsHint                                                                                                                   \
    "Extra options for the video encoder, passed to avcodec_open2(). Both generic options (e.g. g, bf, b, qmin, flags) and the private "       \
    "options of the selected encoder (e.g. preset, tune, crf, x264-params for libx264; vendor for prores_ks) are accepted. "                    \
    "See https://ffmpeg.org/ffmpeg-codecs.html for the list of options. They override the options set by the plug-in, "                        \
    "e.g. g overrides Keyframe Interval, bf overrides Max B-Frames, b overrides Bitrate, and crf overrides Output Quality.\n" kParamOptionsSyntaxHint

#define kParamFormatOptions "formatOptions"
#define kParamFormatOptionsLabel "Format Options"
#define kParamFormatOptionsHint                                                                                                                  \
    "Extra options for the muxer (container format), passed to avformat_init_output()/avformat_write_header(), "                                \
    "e.g. \"movflags=+frag_keyframe\" or \"write_tmcd=0\" for QuickTime/MP4. "                                                                   \
    "See https://ffmpeg.org/ffmpeg-formats.html for the list of options. They override the options set by the plug-in. "                       \
    "The plug-in sets movflags to write_colr (plus faststart if Fast Start is checked): a movflags value that starts with + or - is "          \
    "appended to these flags (e.g. \"movflags=+frag_keyframe\" keeps write_colr), any other value replaces them.\n" kParamOptionsSyntaxHint

#define kParamLibraryInfo "libraryInfo"
#define kParamLibraryInfoLabel "FFmpeg Info...", "Display information about the underlying library."

// Removed from panel - should never have been exposed as very low level control.
#if OFX_FFMPEG_MBDECISION
#define kParamMBDecision "mbDecision"
#endif

#define kProresCodec "prores_ks"
#define kProresProfileProxy 0
#define kProresProfileProxyName "Apple ProRes 422 Proxy"
#define kProresProfileProxyFourCC "apco"
#define kProresProfileLT 1
#define kProresProfileLTName "Apple ProRes 422 LT"
#define kProresProfileLTFourCC "apcs"
#define kProresProfileSQ 2
#define kProresProfileSQName "Apple ProRes 422"
#define kProresProfileSQFourCC "apcn"
#define kProresProfileHQ 3
#define kProresProfileHQName "Apple ProRes 422 HQ"
#define kProresProfileHQFourCC "apch"
#define kProresProfile4444 4
#define kProresProfile4444Name "Apple ProRes 4444"
#define kProresProfile4444FourCC "ap4h"
#define kProresProfile4444XQ 5
#define kProresProfile4444XQName "Apple ProRes 4444 XQ"
#define kProresProfile4444XQFourCC "ap4x"

// Valid DNxHD profiles, as FFmpeg's dnxhd encoder prints them when no compression ID (CID) matches
// (ff_dnxhd_print_profiles(), libavcodec/dnxhddata.c:1157, from dnxhd_cid_table, dnxhddata.c:937; the same list in
// n8.0, n9.0.2 and n9.1). FFmpeg neither checks nor prints the frame rate (ba79a101a2, "lavc/dnxhddata: Do not print
// frame rates with supported profiles"): the frame rates in the profile hints are only Avid's names, see
// dnxhdBitrateMbps(). The yuv444p10/gbrp10 entries are CID 1256 (DNxHD 444), which ff_dnxhd_find_cid() never selects.
// Frame size: 1920x1080p; bitrate: 175Mbps; pixel format: yuv422p10
// Frame size: 1920x1080p; bitrate: 185Mbps; pixel format: yuv422p10
// Frame size: 1920x1080p; bitrate: 365Mbps; pixel format: yuv422p10
// Frame size: 1920x1080p; bitrate: 440Mbps; pixel format: yuv422p10
// Frame size: 1920x1080p; bitrate: 115Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 120Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 145Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 240Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 290Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 175Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 185Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 220Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 365Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 440Mbps; pixel format: yuv422p
// Frame size: 1920x1080i; bitrate: 185Mbps; pixel format: yuv422p10
// Frame size: 1920x1080i; bitrate: 220Mbps; pixel format: yuv422p10
// Frame size: 1920x1080i; bitrate: 120Mbps; pixel format: yuv422p
// Frame size: 1920x1080i; bitrate: 145Mbps; pixel format: yuv422p
// Frame size: 1920x1080i; bitrate: 185Mbps; pixel format: yuv422p
// Frame size: 1920x1080i; bitrate: 220Mbps; pixel format: yuv422p
// Frame size: 1440x1080i; bitrate: 120Mbps; pixel format: yuv422p
// Frame size: 1440x1080i; bitrate: 145Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 90Mbps; pixel format: yuv422p10
// Frame size: 1280x720p; bitrate: 180Mbps; pixel format: yuv422p10
// Frame size: 1280x720p; bitrate: 220Mbps; pixel format: yuv422p10
// Frame size: 1280x720p; bitrate: 90Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 110Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 180Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 220Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 60Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 75Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 120Mbps; pixel format: yuv422p
// Frame size: 1280x720p; bitrate: 145Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 36Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 45Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 75Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 90Mbps; pixel format: yuv422p
// Frame size: 1920x1080p; bitrate: 350Mbps; pixel format: yuv444p10, gbrp10
// Frame size: 1920x1080p; bitrate: 390Mbps; pixel format: yuv444p10, gbrp10
// Frame size: 1920x1080p; bitrate: 440Mbps; pixel format: yuv444p10, gbrp10
// Frame size: 1920x1080p; bitrate: 730Mbps; pixel format: yuv444p10, gbrp10
// Frame size: 1920x1080p; bitrate: 880Mbps; pixel format: yuv444p10, gbrp10
// Frame size: 960x720p; bitrate: 42Mbps; pixel format: yuv422p
// Frame size: 960x720p; bitrate: 60Mbps; pixel format: yuv422p
// Frame size: 960x720p; bitrate: 75Mbps; pixel format: yuv422p
// Frame size: 960x720p; bitrate: 115Mbps; pixel format: yuv422p
// Frame size: 1440x1080p; bitrate: 63Mbps; pixel format: yuv422p
// Frame size: 1440x1080p; bitrate: 84Mbps; pixel format: yuv422p
// Frame size: 1440x1080p; bitrate: 100Mbps; pixel format: yuv422p
// Frame size: 1440x1080p; bitrate: 110Mbps; pixel format: yuv422p
// Frame size: 1440x1080i; bitrate: 80Mbps; pixel format: yuv422p
// Frame size: 1440x1080i; bitrate: 90Mbps; pixel format: yuv422p
// Frame size: 1440x1080i; bitrate: 100Mbps; pixel format: yuv422p
// Frame size: 1440x1080i; bitrate: 110Mbps; pixel format: yuv422p

#ifdef DNXHD_444
#pragma message WARN("This version of FFmpeg seems to support DNxHD 444")
#endif

//#define AVID_DNXHD_444_440X_NAME "DNxHD 444 10-bit 440Mbit"
#define AVID_DNXHD_422_440X_NAME "DNxHD 422 10-bit 440Mbit"
#define AVID_DNXHD_422_220X_NAME "DNxHD 422 10-bit 220Mbit"
#define AVID_DNXHD_422_220_NAME "DNxHD 422 8-bit 220Mbit"
#define AVID_DNXHD_422_145_NAME "DNxHD 422 8-bit 145Mbit"
#define AVID_DNXHD_422_36_NAME "DNxHD 422 8-bit 36Mbit"

#define kParamDNxHDCodecProfile "DNxHDCodecProfile"
#define kParamDNxHDCodecProfileLabel "DNxHD Codec Profile"
#define kParamDNxHDCodecProfileHint "Only for the Avid DNxHD codec, select the target bit rate for the encoded movie. DNxHD only encodes 1920x1080 and 1280x720 (DNxHD 440x and 36: 1920x1080 only), and the stream is resized to 1920x1080 if its size is neither; its bitrate depends on the frame rate. DNxHR is resolution-independent. Writing in thin-raster HDV format (1440x1080) is not supported by this plug-in, although FFmpeg supports it.\nA frame that does not fit in the bit rate (e.g. noise in DNxHR 444) fails to encode. Do not set qmax above 256 in Codec Options: FFmpeg's encoder may crash."
#define kParamDNxHDCodecProfileOptionHR444 "DNxHR 444", "DNxHR 4:4:4 (10 bit, RGB, 4.5:1 compression)", "dnxhr444"
#define kParamDNxHDCodecProfileOptionHRHQX "DNxHR HQX", "DNxHR High Quality (10 bit, 4:2:2 chroma sub-sampling, 5.5:1 compression)", "dnxhrhqx"
#define kParamDNxHDCodecProfileOptionHRHQ "DNxHR HQ", "DNxHR High Quality (8 bit, 4:2:2 chroma sub-sampling, 4.5:1 compression)", "dnxhrhq"
#define kParamDNxHDCodecProfileOptionHRSQ "DNxHR SQ", "DNxHR Standard Quality (8 bit, 4:2:2 chroma sub-sampling, 7:1 compression)", "dnxhrsq"
#define kParamDNxHDCodecProfileOptionHRLB "DNxHR LB", "DNxHR Low Bandwidth (8 bit, 4:2:2 chroma sub-sampling, 22:1 compression)", "dnxhrlb"
#define kParamDNxHDCodecProfileOption440x AVID_DNXHD_422_440X_NAME, "10 bit, 4:2:2, 1920x1080 only: the same stream as DNxHD 220x in 1080p (CID 1235), which is 440x in 1080p/59.94 or 1080p/60. FFmpeg cannot write DNxHD 444 (CID 1256, 880 Mbit/s in 1080p/60): use DNxHR 444.", "dnxhd422_440x"
#define kParamDNxHDCodecProfileOption220x AVID_DNXHD_422_220X_NAME, "10 bit, 4:2:2. 1080p (CID 1235): 175x in 1080p/23.976 or 1080p/24, 185x in 1080p/25, 220x in 1080p/29.97 or 1080p/30, 365x in 1080p/50, 440x in 1080p/59.94 or 1080p/60. 720p (CID 1250): 90x in 720p/23.976 to 720p/25, 110x in 720p/29.97 or 720p/30, 175x in 720p/50, 220x in 720p/59.94 or 720p/60", "dnxhd422_220x"
#define kParamDNxHDCodecProfileOption220 AVID_DNXHD_422_220_NAME, "8 bit, 4:2:2. 1080p (CID 1238): 175 in 1080p/23.976 or 1080p/24, 185 in 1080p/25, 220 in 1080p/29.97 or 1080p/30, 365 in 1080p/50, 440 in 1080p/59.94 or 1080p/60. 720p (CID 1251): 90 in 720p/23.976 to 720p/25, 110 in 720p/29.97 or 720p/30, 175 in 720p/50, 220 in 720p/59.94 or 720p/60", "dnxhd422_220"
#define kParamDNxHDCodecProfileOption145 AVID_DNXHD_422_145_NAME, "8 bit, 4:2:2. 1080p (CID 1237): 115 in 1080p/23.976 or 1080p/24, 120 in 1080p/25, 145 in 1080p/29.97 or 1080p/30, 240 in 1080p/50, 290 in 1080p/59.94 or 1080p/60. 720p (CID 1252): 60 in 720p/23.976 to 720p/25, 75 in 720p/29.97 or 720p/30, 115 in 720p/50, 145 in 720p/59.94 or 720p/60", "dnxhd422_145"
#define kParamDNxHDCodecProfileOption36 AVID_DNXHD_422_36_NAME, "8 bit, 4:2:2, 1920x1080 only (CID 1253): 36 in 1080p/23.976 to 1080p/25, 45 in 1080p/29.97 or 1080p/30, 75 in 1080p/50, 90 in 1080p/59.94 or 1080p/60. In 720p, use DNxHD 145.", "dnxhd422_36"

enum DNxHDCodecProfileEnum {
    eDNxHDCodecProfileHR444,
    eDNxHDCodecProfileHRHQX,
    eDNxHDCodecProfileHRHQ,
    eDNxHDCodecProfileHRSQ,
    eDNxHDCodecProfileHRLB,
    eDNxHDCodecProfile440x,
    eDNxHDCodecProfile220x,
    eDNxHDCodecProfile220,
    eDNxHDCodecProfile145,
    eDNxHDCodecProfile36,
};

// #305: the bitrate (in Mbit/s) that makes FFmpeg's dnxhd encoder use the compression ID (CID) of a DNxHD profile in
// 1920x1080 or 1280x720 (is720p), at the frame rate frameRate (rounded down), or 0 if that profile does not exist in
// that size (DNxHD 440x and 36 are 1080p only).
// FFmpeg's encoder only uses the bitrate to choose the CID: ff_dnxhd_find_cid() (libavcodec/dnxhddata.c:1127, the
// same in n8.0, n9.0.2 and n9.1) returns the first CID of the frame size and bit depth (8 for yuv422p, 10 for
// yuv422p10) whose bit_rates (dnxhd_cid_table, dnxhddata.c:937) contain bit_rate / 1000000, and avcodec_open2()
// fails if there is none. It never checks the frame rate: the bitrate of the stream is the frame size of the CID
// times the frame rate (SMPTE ST 2019-1:2016, section 7.1 and Table C.1). The bitrates below are those of FFmpeg's
// bit_rates, by frame rate; the hints use Avid's names (the table "Avid DNxHD family of mastering resolutions" in
// "Avid DNxHD Technology", 2012, p. 9,
// https://web.archive.org/web/20160304091149/http://www.avid.com/static/resources/US/documents/DNxHD.pdf).
// FFmpeg lists no 220 for CID 1235 and no 110 for CID 1250 (the 29.97 fps bitrates): another bitrate of the same CID
// gives the same stream.
static int
dnxhdBitrateMbps(DNxHDCodecProfileEnum profile,
                 bool is720p,
                 int frameRate)
{
    // the bitrates at 23.976/24, 25, 29.97/30, 50 and 59.94/60 fps
    static const std::array<int, 5> cid1235 = { 175, 185, 185, 365, 440 }; // 1920x1080, 10 bit (220 at 29.97 fps)
    static const std::array<int, 5> cid1238 = { 175, 185, 220, 365, 440 }; // 1920x1080, 8 bit
    static const std::array<int, 5> cid1237 = { 115, 120, 145, 240, 290 }; // 1920x1080, 8 bit
    static const std::array<int, 5> cid1253 = { 36, 36, 45, 75, 90 }; // 1920x1080, 8 bit
    static const std::array<int, 5> cid1250 = { 90, 90, 90, 180, 220 }; // 1280x720, 10 bit (110 at 29.97 fps)
    static const std::array<int, 5> cid1251 = { 90, 90, 110, 180, 220 }; // 1280x720, 8 bit
    static const std::array<int, 5> cid1252 = { 60, 60, 75, 120, 145 }; // 1280x720, 8 bit
    const std::array<int, 5>* mbs = nullptr;
    switch (profile) {
    case eDNxHDCodecProfile440x:
        mbs = is720p ? nullptr : &cid1235;
        break;
    case eDNxHDCodecProfile220x:
        mbs = is720p ? &cid1250 : &cid1235;
        break;
    case eDNxHDCodecProfile220:
        mbs = is720p ? &cid1251 : &cid1238;
        break;
    case eDNxHDCodecProfile145:
        mbs = is720p ? &cid1252 : &cid1237;
        break;
    case eDNxHDCodecProfile36:
        mbs = is720p ? nullptr : &cid1253;
        break;
    case eDNxHDCodecProfileHR444:
    case eDNxHDCodecProfileHRHQX:
    case eDNxHDCodecProfileHRHQ:
    case eDNxHDCodecProfileHRSQ:
    case eDNxHDCodecProfileHRLB:
        // DNxHR is resolution-independent, its CID is chosen by the profile option
        break;
    }
    if (!mbs) {
        return 0;
    }
    size_t i = 0; // 23.976 or 24 fps
    if (frameRate > 50) {
        i = 4;
    } else if (frameRate >= 40) {
        i = 3;
    } else if (frameRate > 25) {
        i = 2;
    } else if (frameRate == 25) {
        i = 1;
    }

    return (*mbs)[i];
}

#define kParamDNxHDEncodeVideoRange "DNxHDEncodeVideoRange"
#define kParamDNxHDEncodeVideoRangeLabel "DNxHD Output Range"
#define kParamDNxHDEncodeVideoRangeHint                                                                              \
    "When encoding using DNxHD this is used to select between full scale data range "                                \
    "and 'video/legal' data range.\nFull scale data range is 0-255 for 8-bit and 0-1023 for 10-bit. "                \
    "'Video/legal' data range is a reduced range, 16-235 (luma) and 16-240 (chroma) for 8-bit, "                     \
    "and 64-940 (luma) and 64-960 (chroma) for 10-bit.\n"                                                            \
    "The pixel values are written in this range, and the stream is tagged with it (in the ACLR atom in QuickTime). " \
    "Full Range writes full-range samples tagged as \"RGB\" in the ACLR atom, which is how FFmpeg reads them back, " \
    "but FFmpeg's DNxHD encoder only declares the legal range, and Avid tools may expect legal-range samples.\n"     \
    "DNxHR 444 is written as RGB, which is always full range."
#define kParamDNxHDEncodeVideoRangeOptionFull "Full Range", "", "full"
#define kParamDNxHDEncodeVideoRangeOptionVideo "Video Range", "", "video"

#define kParamHapFormat "HapFormat"
#define kParamHapFormatLabel "Hap Format", "Only for the Hap codec, select the target format."
#define kParamHapFormatOptionHap "Hap 1", "DXT1 textures (FourCC Hap1)", "hap"
#define kParamHapFormatOptionHapAlpha "Hap Alpha", "DXT5 textures (FourCC Hap5)", "hap_alpha"
#define kParamHapFormatOptionHapQ "Hap Q", "DXT5-YCoCg textures (FourCC HapY)", "hap_q"
enum HapFormatEnum {
    eHapFormatHap = 0,
    eHapFormatHapAlpha,
    eHapFormatHapQ,
};

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

template <typename T>
static inline void
unused(const T&)
{
}

// The fourccs for MJPEGA, MJPEGB, and Photo JPEG
static const char* kJpegCodecs[] = { "jpeg", "mjpa", "mjpb" };
static const int kNumJpegCodecs = sizeof(kJpegCodecs) / sizeof(kJpegCodecs[0]);

// A bitrate in Mbit/s (a param, at least 0) in bit/s, as an int64_t (AVCodecContext::bit_rate, rc_max_rate), clamped to
// [0, INT_MAX kbit/s] before the conversion: some encoders (e.g. libx264) take the bitrate in kbit/s as an int, and fail
// ("Invalid argument") above. (On master, the conversion to int overflowed above 2147 Mbit/s.)
static int64_t
bitrateToInt64(double mbps)
{
    const double bps = mbps * 1000000;
    // OFX::IsNaN() and OFX::IsInfinite() test the bits: they work even with -ffast-math, unlike comparisons
    if (OFX::IsNaN(bps) || (bps <= 0.)) {
        return 0;
    }
    const double maxBps = (double)INT_MAX * 1000.;
    if (OFX::IsInfinite(bps) || (bps >= maxBps)) {
        return (int64_t)maxBps;
    }

    return (int64_t)bps;
}

// Trap fractional frame rates so that they can be specified correctly
// in a QuickTime movie. The rational number representation of fractional
// frame rates is 24000/1001, 30000/1001, 60000/1001, etc.
// WARNING: There are some big assumptions here. The assumption is
//          that by specifying 23.98, 29.97 on the UI the intended
//          frame rate is 24/1.001, 30/1.001, etc. so the frame rate
//          is corrected here.
// The NTSC rates of SMPTE ST 12-1 and ST 12-3 (47.952, 95.904, 119.88 and 239.76, which are 48, 96,
// 120 and 240 divided by 1.001) are corrected too: av_d2q() would give e.g. 2997/25 for 119.88.
// The host gives the frame rate as a double (kOfxImageEffectPropFrameRate), which may be either the
// rounded value (119.88) or the exact one (119.88011988...).
// This is the frame rate of the video stream (AVStream::avg_frame_rate).
static AVRational
frameRateFromFps(double fps)
{
    AVRational frame_rate;
    if (((23.969 < fps) && (fps < 23.981)) || ((29.969 < fps) && (fps < 29.981)) || ((59.939 < fps) && (fps < 59.941)) ||
        ((47.951 < fps) && (fps < 47.953)) || ((95.903 < fps) && (fps < 95.905)) || ((119.879 < fps) && (fps < 119.881)) || ((239.759 < fps) && (fps < 239.761))) {
        frame_rate.num = std::ceil(fps) * 1000;
        frame_rate.den = 1001;
    } else {
        // integers are represented exactly as float, so most of the time the denominator will be 1
        frame_rate = av_d2q(fps, INT_MAX);
    }
    return frame_rate;
}

// The minimum bitrate tolerance (in Mbit/s), bitrate / min(fps, 4) rounded up to 1 bit/s, from ffserver.c
// (bit_rate_tolerance = FFMAX(bit_rate / 4, bit_rate * time_base)). The frame rate param may be 0: bitrate / 0 was inf
// (or NaN for a bitrate of 0), converted to int. A frame rate of 0 or less gives bitrate / 4, and the result is at
// most maxTolerance.
static double
bitrateToleranceMin(double bitrate,
                    double fps,
                    double maxTolerance)
{
    const double divisor = ((0. < fps) && (fps < 4.)) ? fps : 4.;
    const double tolerance = std::ceil((bitrate / divisor) * 1000000) / 1000000.;

    return (tolerance < maxTolerance) ? tolerance : maxTolerance;
}

// The frame rate of the encoded video (the inverse of the codec and stream time bases).
// MPEG-4 part 2 can not have a time base denominator above 65535 (libavcodec/mpeg4videoenc.c:
// "timebase %d/%d not supported by MPEG 4 standard, the maximum admitted value for the timebase
// denominator is 65535"), e.g. 120000/1001 for 119.88 fps. The frame rate is then reduced as the
// ffmpeg CLI does (fftools/ffmpeg_filter.c: "reduce frame rate for mpeg4 to be within the spec
// limits", framerate_clip = 65535, then av_reduce(&fr.num, &fr.den, fr.num, fr.den, framerate_clip)),
// e.g. to 40999/342 for 120000/1001.
// The stream's avg_frame_rate stays the unreduced frameRateFromFps(): the mov muxer derives the
// timecode (tmcd) track from it (see configureVideoStream()).
static AVRational
clipFrameRate(AVRational rate,
              AVCodecID codecId)
{
    if (codecId == AV_CODEC_ID_MPEG4) {
        av_reduce(&rate.num, &rate.den, rate.num, rate.den, 65535);
    }

    return rate;
}

// Read 1 to maxDigits decimal digits at p (and nothing else: no sign or whitespace), and advance p.
static bool
parseDigits(const char*& p,
            int maxDigits,
            int* value)
{
    int n = 0;
    int v = 0;
    while (n < maxDigits && '0' <= *p && *p <= '9') {
        v = v * 10 + (*p - '0');
        ++p;
        ++n;
    }
    if (n == 0 || ('0' <= *p && *p <= '9')) {
        return false;
    }
    *value = v;

    return true;
}

// Is the muxer one of the mov family (libavformat/movenc.c), which may write the timecode as a tmcd track?
static bool
isMovMuxer(const AVOutputFormat* fmt)
{
    if (!fmt || !fmt->name) {
        return false;
    }
    static const char* const movMuxers[] = { "mov", "mp4", "ipod", "psp", "3gp", "3g2", "ismv", "f4v", "avif" };
    for (const char* name : movMuxers) {
        if (!std::strcmp(fmt->name, name)) {
            return true;
        }
    }

    return false;
}

// Parse and validate a start timecode, HH:MM:SS:FF (or HH:MM:SS;FF for drop-frame), at the given
// frame rate, using libavutil's timecode API (which is also what the muxers use to parse it).
// av_timecode_init_from_string() uses sscanf("%d") and only checks the frame rate and the drop-frame
// flag, so the syntax (digits only), the ranges of the fields, and drop-frame are checked here too.
// tmcdMuxer is the name of the output format if it writes a timecode (tmcd) track, which can not
// have a timecode above 255 fps, or null.
// On success, returns true and sets canonical to the normalized timecode (as written by
// av_timecode_make_string()); otherwise returns false and sets error.
static bool
parseTimecode(const string& str,
              AVRational rate,
              const char* tmcdMuxer,
              string* canonical,
              string* error)
{
    int hh = 0, mm = 0, ss = 0, ff = 0;
    const char* p = str.c_str();
    char c = 0;
    if (!parseDigits(p, 2, &hh) || *p++ != ':' || !parseDigits(p, 2, &mm) || *p++ != ':' || !parseDigits(p, 2, &ss) || ((c = *p++) != ':' && c != ';') || !parseDigits(p, 3, &ff) || *p != '\0') {
        *error = "cannot parse \"" + str + "\": expected HH:MM:SS:FF, or HH:MM:SS;FF for drop-frame";

        return false;
    }
    const bool dropFrame = (c == ';');
    char rateStr[64];
    std::snprintf(rateStr, sizeof(rateStr), "%g", av_q2d(rate));
    // the timecode frame rate, as computed by libavutil/timecode.c
    const int64_t fps = (rate.num > 0 && rate.den > 0) ? (rate.num + rate.den / 2LL) / rate.den : 0;
    // FFmpeg only checks that the rounded frame rate is a multiple of 30, but drop-frame timecode is
    // only meaningful at the NTSC rates, 30000/1001 and its multiples (or a close approximation)
    if (dropFrame && ((fps <= 0) || (fps % 30 != 0) || (std::fabs(av_q2d(rate) * 1001. / (fps * 1000.) - 1.) > 1e-6))) {
        *error = "\"" + str + "\" is not valid at " + rateStr + " fps (drop-frame timecodes require 29.97 fps or a multiple of it)";

        return false;
    }
    if (tmcdMuxer && fps > 255) {
        // movenc.c: mov_write_tmcd_tag() fails ("fps is too large") when writing the moov atom
        *error = string("the ") + tmcdMuxer + " format can not write a timecode at more than 255 fps (" + rateStr + " fps)";

        return false;
    }
    AVTimecode tc;
    if (av_timecode_init_from_string(&tc, rate, str.c_str(), nullptr) < 0) {
        *error = "\"" + str + "\" is not valid at " + rateStr + " fps";

        return false;
    }
    if ((hh > 23) || (mm > 59) || (ss > 59) || (ff >= (int)tc.fps)) {
        *error = "\"" + str + "\" is out of range: hours must be 0-23, minutes and seconds 0-59, and frames 0-" + std::to_string(tc.fps - 1);

        return false;
    }
    // in drop-frame timecode, the first frame numbers of each minute are skipped, except every tenth minute
    const int droppedFrames = (int)(tc.fps / 30 * 2);
    if (dropFrame && (ss == 0) && (mm % 10 != 0) && (ff < droppedFrames)) {
        *error = "\"" + str + "\" does not exist in drop-frame timecode: frames 0-" + std::to_string(droppedFrames - 1) + " are skipped at the start of each minute, except every tenth minute";

        return false;
    }
    char buf[AV_TIMECODE_STR_SIZE];
    *canonical = av_timecode_make_string(&tc, buf, 0);

    return true;
}

static bool
IsJpeg(ChoiceParam* codecParam,
       int codecValue)
{
    string strCodec;

    codecParam->getOption(codecValue, strCodec);

    // Check the fourcc for a JPEG codec
    for (int i = 0; i < kNumJpegCodecs; ++i) {
        const char* codec = kJpegCodecs[i];
        // All labels start with the fourcc.
        const bool startsWithCodec = (strCodec.find(codec) == 0);
        if (startsWithCodec) {
            return true;
        }
    }

    return false;
}

// check if codec is compatible with format.
// libavformat may not implement query_codec for all formats
static bool
codecCompatible(const AVOutputFormat* ofmt,
                enum AVCodecID codec_id)
{
    string fmt = string(ofmt->name);

    // rawvideo is only written in the containers where a pixel format can be tagged (see rawvideoPixelFormats())
    if ((codec_id == AV_CODEC_ID_RAWVIDEO) && !rawvideoPixelFormats(ofmt)) {
        return false;
    }
    return (avformat_query_codec(ofmt, codec_id, FF_COMPLIANCE_NORMAL) == 1 || (fmt == "mxf" && (codec_id == AV_CODEC_ID_MPEG2VIDEO || codec_id == AV_CODEC_ID_DNXHD || codec_id == AV_CODEC_ID_DVVIDEO || codec_id == AV_CODEC_ID_H264)) || (fmt == "mpegts" && (codec_id == AV_CODEC_ID_MPEG1VIDEO || codec_id == AV_CODEC_ID_MPEG2VIDEO || codec_id == AV_CODEC_ID_MPEG4 || codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC || codec_id == AV_CODEC_ID_CAVS || codec_id == AV_CODEC_ID_DIRAC)) || (fmt == "mpeg" && (codec_id == AV_CODEC_ID_MPEG1VIDEO || codec_id == AV_CODEC_ID_H264)));
}

typedef map<string, string> CodecMap;
static CodecMap
CreateCodecKnobLabelsMap()
{
    CodecMap m;

    // Video codecs.
    m["avrp"] = "AVrp\tAvid 1:1 10-bit RGB Packer";
    m["ayuv"] = "AYUV\tUncompressed packed MS 4:4:4:4";
    m["cfhd"] = "CFHD\tGoPro Cineform HD";
    m["cinepak"] = "cvid\tCinepak";
    m["dnxhd"] = "AVdn\tAvid DNxHD / DNxHR / SMPTE VC-3";
    m["dpx"] = "dpx \tDPX (Digital Picture Exchange) image";
    m["exr"] = "exr \tEXR image";
    m["ffv1"] = "FFV1\tFFmpeg video codec #1";
    m["ffvhuff"] = "FFVH\tHuffyuv FFmpeg variant";
    m["flv"] = "FLV1\tFLV / Sorenson Spark / Sorenson H.263 (Flash Video)";
    m["gif"] = "gif \tGIF (Graphics Interchange Format)";
    m["h263p"] = "H263\tH.263+ / H.263-1998 / H.263 version 2";
    m["hap"] = "Hap1\tVidvox Hap";
    m["huffyuv"] = "HFYU\tHuffYUV";
    m["jpeg2000"] = "mjp2\tJPEG 2000";
    m["jpegls"] = "MJLS\tJPEG-LS";
    m["libaom-av1"] = "AV1\tlibaom AV1 encoder";
    m["libopenh264"] = "H264\tCisco libopenh264 H.264/MPEG-4 AVC encoder";
    m["libopenjpeg"] = "mjp2\tOpenJPEG JPEG 2000";
    m["librav1e"] = "AV1\trav1e AV1 encoder";
    m["libschroedinger"] = "drac\tSMPTE VC-2 (previously BBC Dirac Pro)";
    m["libtheora"] = "theo\tTheora";
    m["libvpx"] = "VP80\tOn2 VP8"; // write doesn't work yet
    m["libvpx-vp9"] = "VP90\tGoogle VP9";
    m["libx264"] = "avc1\tH.264 / AVC / MPEG-4 AVC / MPEG-4 part 10";
    m["libx264rgb"] = "avc1\tH.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 RGB";
    m["libx265"] = "hev1\tH.265 / HEVC (High Efficiency Video Coding)";
    m["libxavs"] = "CAVS\tChinese AVS (Audio Video Standard)"; // disabled in allowlist

    m["libxvid"] = "mp4v\tMPEG-4 part 2";
    m["ljpeg"] = "LJPG\tLossless JPEG";
    m["mjpeg"] = "jpeg\tPhoto JPEG";
    m["mpeg1video"] = "m1v \tMPEG-1 Video"; // disabled in allowlist (random blocks)
    m["mpeg2video"] = "m2v1\tMPEG-2 Video";
    m["mpeg4"] = "mp4v\tMPEG-4 part 2";
    m["msmpeg4v2"] = "MP42\tMPEG-4 part 2 Microsoft variant version 2";
    m["msmpeg4"] = "3IVD\tMPEG-4 part 2 Microsoft variant version 3";
    m["png"] = "png \tPNG (Portable Network Graphics) image";
    m["qtrle"] = "rle \tQuickTime Animation (RLE) video";
    m["r10k"] = "R10k\tAJA Kona 10-bit RGB Codec"; // disabled in allowlist
    m["r210"] = "r210\tUncompressed RGB 10-bit"; // disabled in allowlist
    m["rawvideo"] = "raw \tUncompressed RGB or Y'CbCr (pixel format from the container and the Pref. params)";
    m["svq1"] = "SVQ1\tSorenson Vector Quantizer 1 / Sorenson Video 1 / SVQ1";
    m["targa"] = "tga \tTruevision Targa image";
    m["tiff"] = "tiff\tTIFF image";
    m["v210"] = "v210\tUncompressed 10-bit 4:2:2";
    m["v308"] = "v308\tUncompressed 8-bit 4:4:4";
    m["v408"] = "v408\tUncompressed 8-bit QT 4:4:4:4";
    m["v410"] = "v410\tUncompressed 4:4:4 10-bit";
    m["vc2"] = "drac\tSMPTE VC-2 (previously BBC Dirac Pro)";

    // add the FFmpeg encoder name at the end of each codec description
    for (CodecMap::iterator it = m.begin(); it != m.end(); ++it) {
        it->second += " [";
        it->second += it->first;
        it->second += ']';
    }

    return m;
} // CreateCodecKnobLabelsMap

// MPEG-2 codecs (non-SD)
#if 0

// HDV
m["hdv1"]          = "MPEG2 HDV 720p30";
m["hdv2"]          = "MPEG2 HDV 1080i60";
m["hdv3"]          = "MPEG2 HDV 1080i50";
m["hdv4"]          = "MPEG2 HDV 720p24";
m["hdv5"]          = "MPEG2 HDV 720p25";
m["hdv6"]          = "MPEG2 HDV 1080p24";
m["hdv7"]          = "MPEG2 HDV 1080p25";
m["hdv8"]          = "MPEG2 HDV 1080p30";
m["hdv9"]          = "MPEG2 HDV 720p60 JVC";
m["hdva"]          = "MPEG2 HDV 720p50";

// XDCAM
m["xdv1"]          = "XDCAM EX 720p30 35Mb/s";
m["xdv2"]          = "XDCAM HD 1080i60 35Mb/s";
m["xdv3"]          = "XDCAM HD 1080i50 35Mb/s";
m["xdv4"]          = "XDCAM EX 720p24 35Mb/s";
m["xdv5"]          = "XDCAM EX 720p25 35Mb/s";
m["xdv6"]          = "XDCAM HD 1080p24 35Mb/s";
m["xdv7"]          = "XDCAM HD 1080p25 35Mb/s";
m["xdv8"]          = "XDCAM HD 1080p30 35Mb/s";
m["xdv9"]          = "XDCAM EX 720p60 35Mb/s";
m["xdva"]          = "XDCAM EX 720p50 35Mb/s";

m["xdvb"]          = "XDCAM EX 1080i60 50Mb/s CBR";
m["xdvc"]          = "XDCAM EX 1080i50 50Mb/s CBR";
m["xdvd"]          = "XDCAM EX 1080p24 50Mb/s CBR";
m["xdve"]          = "XDCAM EX 1080p25 50Mb/s CBR";
m["xdvf"]          = "XDCAM EX 1080p30 50Mb/s CBR";

m["xd51"]          = "XDCAM HD422 720p30 50Mb/s CBR";
m["xd54"]          = "XDCAM HD422 720p24 50Mb/s CBR";
m["xd55"]          = "XDCAM HD422 720p25 50Mb/s CBR";
m["xd59"]          = "XDCAM HD422 720p60 50Mb/s CBR";
m["xd5a"]          = "XDCAM HD422 720p50 50Mb/s CBR";
m["xd5b"]          = "XDCAM HD422 1080i60 50Mb/s CBR";
m["xd5c"]          = "XDCAM HD422 1080i50 50Mb/s CBR";
m["xd5d"]          = "XDCAM HD422 1080p24 50Mb/s CBR";
m["xd5e"]          = "XDCAM HD422 1080p25 50Mb/s CBR";
m["xd5f"]          = "XDCAM HD422 1080p30 50Mb/s CBR";

m["xdhd"]          = "XDCAM HD 540p";
m["xdh2"]          = "XDCAM HD422 540p";

#endif

static const CodecMap kCodecKnobLabels = CreateCodecKnobLabelsMap();
static const char*
getCodecKnobLabel(const char* codecShortName)
{
    CodecMap::const_iterator it = kCodecKnobLabels.find(string(codecShortName));
    if (it != kCodecKnobLabels.end()) {
        return it->second.c_str();
    } else {
        return nullptr;
    }
}

static const char*
getCodecFromShortName(const string& name)
{
    string prefix(kProresCodec);

    if (!name.compare(0, prefix.size(), prefix)) {
        return kProresCodec;
    }

    return name.c_str();
}

static int
getProfileFromShortName(const string& name)
{
    string prefix(kProresCodec);

    if (!name.compare(0, prefix.size(), prefix)) {
        if (!name.compare(prefix.size(), string::npos, kProresProfile4444XQFourCC)) {
            return kProresProfile4444XQ;
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfile4444FourCC)) {
            return kProresProfile4444;
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileHQFourCC)) {
            return kProresProfileHQ;
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileSQFourCC)) {
            return kProresProfileSQ;
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileLTFourCC)) {
            return kProresProfileLT;
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileProxyFourCC)) {
            return kProresProfileProxy;
        }
    }

    return -1;
}

// see libavcodec/proresenc_kostya.c for the list of profiles
static const char*
getProfileStringFromShortName(const string& name)
{
    string prefix(kProresCodec);

    if (!name.compare(0, prefix.size(), prefix)) {
        if (!name.compare(prefix.size(), string::npos, kProresProfile4444XQFourCC)) {
            return "4444xq";
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfile4444FourCC)) {
            return "4444";
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileHQFourCC)) {
            return "hq";
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileSQFourCC)) {
            return "standard";
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileLTFourCC)) {
            return "lt";
        }
        if (!name.compare(prefix.size(), string::npos, kProresProfileProxyFourCC)) {
            return "proxy";
        }
    }

    return "auto";
}

////////////////////////////////////////////////////////////////////////////////
// MyAVFrame
// Wrapper class for the FFmpeg AVFrame structure.
// In order negate the chances of memory leaks when using FFmpeg this class
// provides the initialisation, allocation and release of memory associated
// with an AVFrame structure.
//
// Note that this has been designed to be a drop in replacement for host
// managed memory for colourspace, pixel format and sample format conversions.
// It is not designed to be used with avcodec_decode_video2 as the underlying
// decoder usually manages memory.
//
// Example usage:
//
// Audio
//
//    MyAVFrame avFrame;
//    ret = avFrame.alloc(channels, nb_samples, _avSampleFormat, 1);
//    :
//    ret = swr_convert(_swrContext, avFrame->data, ...);
//    :
//    ret = avcodec_send_frame(avCodecContext, avFrame);
//    ret = avcodec_receive_packet(avCodecContext, pkt);
//    :
//
// Video
//
//    MyAVFrame avFrame;
//    ret = avFrame.alloc(width(), height(), pixelFormatCodec, 1);
//    if (!ret) {
//      :
//      sws_scale(convertCtx, ..., avFrame->data, avFrame->linesize);
//      :
//    }
//
// IMPORTANT
// This class has been purposefully designed NOT to have parameterised
// constructors or assignment operators. The reason for this is due to the
// complexity of managing the lifetime of the structure and its associated
// memory buffers.
//
class MyAVFrame {
public:
    ////////////////////////////////////////////////////////////////////////////////
    // Ctor. Initialise the AVFrame structure.
    MyAVFrame()
        : _avFrame(nullptr)
    {
    }

    ////////////////////////////////////////////////////////////////////////////////
    // Dtor. Release all resources. Release the AVFrame buffers and the
    // AVFrame structure.
    ~MyAVFrame();

    ////////////////////////////////////////////////////////////////////////////////
    // alloc
    // VIDEO SPECIFIC.
    // Allocate a buffer or buffers for the AVFrame structure. How many
    // buffers depends upon |avPixelFormat| which is a VIDEO format.
    //
    // @param width Frame width in pixels.
    // @param height Frame height in pixels.
    // @param avPixelFormat An AVPixelFormat enumeration for the frame pixel format.
    // @param align Buffer byte alignment.
    //
    // @return 0 if successful.
    //         <0 otherwise.
    //
    int alloc(int width, int height, enum AVPixelFormat avPixelFormat, int align);

    ////////////////////////////////////////////////////////////////////////////////
    // alloc
    // AUDIO SPECIFIC.
    // Allocate a buffer or buffers for the AVFrame structure. How many
    // buffers depends upon |avSampleFormat| which is an AUDIO format.
    //
    // @param nbChannels The number of audio channels.
    // @param nbSamples The number of audio samples that the buffer will hold.
    // @param avSampleFormat An AVSampleFormat enumeration for the audio format.
    // @param align Buffer byte alignment.
    //
    // @return 0 if successful.
    //         <0 otherwise.
    //
    int alloc(int nbChannels, int nbSamples, enum AVSampleFormat avSampleFormat, int align);

    // operator dereference overload.
    AVFrame* operator->() const { return _avFrame; }

    // operator type cast overload.
    operator AVFrame*() { return _avFrame; }

private:
    AVFrame* _avFrame;

    // Release any memory allocated to the data member variable of
    // the AVFrame structure.
    void deallocateAVFrameData();

    // Probably do not need the following or the headaches
    // of trying to support the functionality.
    // Hide the copy constructor. Who would manage memory?
    MyAVFrame(MyAVFrame& avFrame);
    // Hide the assignment operator. Who would manage memory?
    MyAVFrame& operator=(const MyAVFrame& /* rhs*/) { return *this; }
};

typedef struct MyAVStream {
    AVStream* stream;
    AVCodecContext* codecContext;
} MyAVStream;

////////////////////////////////////////////////////////////////////////////////
// Dtor. Release all resources. Release the AVFrame buffers and the
// AVFrame structure.
MyAVFrame::~MyAVFrame()
{
    // The most important part of this class.
    // Two deallocations may be required, one from the AVFrame structure
    // and one for any image data, AVFrame::data.
    deallocateAVFrameData();
    av_frame_free(&_avFrame);
}

////////////////////////////////////////////////////////////////////////////////
// alloc
// VIDEO SPECIFIC.
// Allocate a buffer or buffers for the AVFrame structure. How many
// buffers depends upon |avPixelFormat| which is a VIDEO format.
//
// @param width Frame width in pixels.
// @param height Frame height in pixels.
// @param avPixelFormat An AVPixelFormat enumeration for the frame pixel format.
// @param align Buffer byte alignment.
//
// @return 0 if successful.
//         <0 otherwise.
//
int
MyAVFrame::alloc(int width,
                 int height,
                 enum AVPixelFormat avPixelFormat,
                 int align)
{
    int ret = 0;

    if (!_avFrame) {
        _avFrame = av_frame_alloc();
    }
    if (_avFrame) {
        deallocateAVFrameData(); // In case this method is called multiple times on the same object.
        // av_image_alloc will return the size of the buffer in bytes if successful,
        // otherwise it will return <0.
        int bufferSize = av_image_alloc(_avFrame->data, _avFrame->linesize, width, height, avPixelFormat, align);
        if (bufferSize > 0) {
            // Set the frame fields for a video buffer as some
            // encoders rely on them, e.g. Lossless JPEG.
            _avFrame->width = width;
            _avFrame->height = height;
            _avFrame->format = (int)avPixelFormat;
            ret = 0;
        } else {
            ret = -1;
        }
    } else {
        // Failed to allocate an AVFrame.
        ret = -2;
    }

    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// alloc
// AUDIO SPECIFIC.
// Allocate a buffer or buffers for the AVFrame structure. How many
// buffers depends upon |avSampleFormat| which is an AUDIO format.
//
// @param nbChannels The number of audio channels.
// @param nbSamples The number of audio samples that the buffer will hold.
// @param avSampleFormat An AVSampleFormat enumeration for the audio format.
// @param align Buffer byte alignment.
//
// @return 0 if successful.
//         <0 otherwise.
//
int
MyAVFrame::alloc(int nbChannels,
                 int nbSamples,
                 enum AVSampleFormat avSampleFormat,
                 int align)
{
    int ret = 0;

    if (!_avFrame) {
        _avFrame = av_frame_alloc();
    }
    if (_avFrame) {
        deallocateAVFrameData(); // In case this method is called multiple times on the same object.
        // av_samples_alloc will return >= if successful, otherwise it will return <0.
        ret = av_samples_alloc(_avFrame->data, _avFrame->linesize, nbChannels, nbSamples, avSampleFormat, align);
        if (ret >= 0) {
            // Set the frame fields for an audio buffer as some
            // encoders rely on them.
            _avFrame->nb_samples = nbSamples;
            _avFrame->format = (int)avSampleFormat;
            ret = 0;
        } else {
            ret = -1;
        }
    } else {
        // Failed to allocate an AVFrame.
        ret = -2;
    }

    return ret;
}

// Release any memory allocated to the data member variable of
// the AVFrame structure.
void
MyAVFrame::deallocateAVFrameData()
{
    if (_avFrame) {
        av_freep(_avFrame->data);
    }
}

// Owns an AVDictionary, so that it is freed on every path, including exceptions.
class AVDictionaryHolder
{
public:
    AVDictionaryHolder() = default;
    AVDictionaryHolder(const AVDictionaryHolder&) = delete;
    AVDictionaryHolder& operator=(const AVDictionaryHolder&) = delete;
    ~AVDictionaryHolder() { av_dict_free(&_dict); }

    AVDictionary* get() const { return _dict; }
    AVDictionary** ptr() { return &_dict; }

private:
    AVDictionary* _dict = nullptr;
};

// Calls a function when it goes out of scope, unless release() was called: the cleanup of a function that has many
// exits, including exceptions (C++ Core Guidelines E.6, E.19):
// https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-raii
// https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-finally
template <class F>
class ScopeExit {
public:
    explicit ScopeExit(F f)
        : _f(std::move(f))
    {
    }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit()
    {
        if (_active) {
            _f();
        }
    }

    void release() { _active = false; }

private:
    F _f;
    bool _active = true;
};

static bool
isOptionSeparator(char c)
{
    return c == ':' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// " at character N", for the 0-based position pos in a user option string
static string
optionCharPos(size_t pos)
{
    return " at character " + std::to_string(pos + 1);
}

// Normalize a user option string (see kParamOptionsSyntaxHint) to the strict syntax of
// av_dict_parse_string(dict, str, "=", ":", 0), whose keys and values are read by av_get_token(),
// and check its syntax: av_dict_parse_string() gives an unexpected meaning to some strings once
// whitespace also separates pairs (e.g. "tune preset=slow" would be {"tune:preset": "slow"}).
// The tokens are copied verbatim, so that av_get_token() unquotes them: outside single quotes, a
// backslash escapes the next character; inside single quotes, every character (including a
// backslash) is literal.
// - Pairs are separated by unquoted runs of separators (colons and whitespace), which become a
//   single colon. Leading and trailing separators are ignored: "g=1 bf=0" and "g=1::bf=0:" both
//   mean "g=1:bf=0".
// - As in av_get_token(), whitespace around '=' is ignored ("g = 1" is "g=1"), except where it
//   would be ambiguous, since whitespace also separates pairs: a token after "key= " that contains
//   an unquoted '=' is an error ("tune= preset=slow" is not {tune: "preset=slow"}, as it would be
//   for av_dict_parse_string with a ':' separator), and so is a '=' after a complete pair ("g=1 =2"
//   is not {g: "1=2"}).
// - Errors: a key without '=' ("tune preset=slow", "g:=1", "a :=1"), a '=' without a key, an empty
//   value (rejected by av_dict_parse_string), an unterminated single quote (av_get_token() would
//   take the rest of the string), and double quotes (not a quoting character for av_get_token()).
// Returns false and sets *error (which says what is wrong and where) on error.
static bool
normalizeOptionString(const string& str,
                      string* out,
                      string* error)
{
    out->clear();
    const size_t n = str.size();
    enum State {
        eBetweenPairs, // expecting a key
        eKey, // in a key
        eAfterKey, // whitespace after a key, expecting '='
        eAfterEqual, // after '=' (and maybe whitespace), expecting a value
        eValue, // in a value
    };
    State state = eBetweenPairs;
    size_t keyPos = 0; // position in str of the current key
    size_t keyOut = 0; // position in *out of the current key
    bool spaceAfterEqual = false; // whitespace was skipped after the '=' of the current pair
    size_t i = 0; // position in str

    auto fail = [&](const string& msg) {
        *error = msg;

        return false;
    };
    // the token (up to the next unquoted separator) that starts at pos, for error messages
    auto tokenAt = [&](size_t pos) {
        size_t j = pos;
        while (j < n && !isOptionSeparator(str[j])) {
            if (str[j] == '\\' && j + 1 < n) {
                ++j;
            } else if (str[j] == '\'') {
                const size_t end = str.find('\'', j + 1);
                if (end == string::npos) {
                    return str.substr(pos);
                }
                j = end;
            }
            ++j;
        }

        return str.substr(pos, j - pos);
    };
    // does the token that starts at pos contain an unquoted, unescaped '='?
    auto tokenHasEqual = [&](size_t pos) {
        const string tok = tokenAt(pos);
        for (size_t j = 0; j < tok.size(); ++j) {
            if (tok[j] == '\\') {
                ++j;
            } else if (tok[j] == '\'') {
                const size_t end = tok.find('\'', j + 1);
                if (end == string::npos) {
                    return false;
                }
                j = end;
            } else if (tok[j] == '=') {
                return true;
            }
        }

        return false;
    };
    auto missingEqual = [&]() {
        const string found = (i >= n) ? string("the end of the string")
            : (str[i] == ':')         ? "':'" + optionCharPos(i)
                                      : "\"" + tokenAt(i) + "\"" + optionCharPos(i);

        return fail("missing '=' after the key \"" + out->substr(keyOut) + "\"" + optionCharPos(keyPos) + ", found " + found
                    + (i < n ? " (a key cannot contain a colon or a space, unless it is quoted or escaped)" : ""));
    };
    auto missingValue = [&]() {
        string msg = "missing value after \"" + out->substr(keyOut) + "\"" + optionCharPos(keyPos);
        if (i < n && str[i] != ':') {
            msg += ", found the pair \"" + tokenAt(i) + "\"" + optionCharPos(i) + " (values cannot be empty, and a value after a space cannot contain an unquoted '=')";
        } else {
            msg += " (values cannot be empty)";
        }

        return fail(msg);
    };
    // copy the token character (or escape sequence, or single-quoted string) at i to *out
    auto copyTokenChar = [&]() {
        const char c = str[i];
        if (c == '"') {
            return fail("double quote" + optionCharPos(i) + ": double quotes are not supported, use single quotes, e.g. x264-params='keyint=1:bframes=0'");
        }
        if (c == '\'') {
            const size_t end = str.find('\'', i + 1);
            if (end == string::npos) {
                return fail("unterminated single quote" + optionCharPos(i));
            }
            *out += str.substr(i, end + 1 - i);
            i = end + 1;

            return true;
        }
        *out += c;
        ++i;
        if (c == '\\' && i < n) {
            *out += str[i];
            ++i;
        }

        return true;
    };

    while (i < n) {
        const char c = str[i];
        const bool sep = isOptionSeparator(c);
        switch (state) {
        case eBetweenPairs:
            if (sep) {
                ++i;
                break;
            }
            if (c == '=') {
                return fail("missing key before '='" + optionCharPos(i));
            }
            if (!out->empty()) {
                *out += ':';
            }
            keyPos = i;
            keyOut = out->size();
            state = eKey;
            if (!copyTokenChar()) {
                return false;
            }
            break;
        case eKey:
        case eAfterKey:
            if (c == '=') {
                *out += '=';
                ++i;
                state = eAfterEqual;
                spaceAfterEqual = false;
            } else if (c == ':' || (state == eAfterKey && !sep)) {
                return missingEqual();
            } else if (sep) { // whitespace
                ++i;
                state = eAfterKey;
            } else if (!copyTokenChar()) {
                return false;
            }
            break;
        case eAfterEqual:
            if (c == ':') {
                return missingValue();
            }
            if (sep) { // whitespace
                ++i;
                spaceAfterEqual = true;
                break;
            }
            if (spaceAfterEqual && tokenHasEqual(i)) {
                return missingValue();
            }
            state = eValue;
            if (!copyTokenChar()) {
                return false;
            }
            break;
        case eValue:
            if (sep) {
                ++i;
                state = eBetweenPairs;
            } else if (!copyTokenChar()) {
                return false;
            }
            break;
        }
    }
    if (state == eKey || state == eAfterKey) {
        return missingEqual();
    }
    if (state == eAfterEqual) {
        return missingValue();
    }

    return true;
}

// Parse a user option string (see kParamOptionsSyntaxHint) into dict.
// Returns false and sets *error on a syntax error.
static bool
parseOptionString(const string& str,
                  AVDictionary** dict,
                  string* error)
{
    string normalized;
    if (!normalizeOptionString(str, &normalized, error)) {
        return false;
    }
    if (normalized.empty()) {
        return true;
    }
    int ret = av_dict_parse_string(dict, normalized.c_str(), "=", ":", 0);
    if (ret < 0) {
        char szError[1024] = { 0 };
        av_strerror(ret, szError, sizeof(szError));
        *error = string("expected key=value pairs separated by colons or spaces (") + szError + ")";

        return false;
    }

    return true;
}

// "key=value, key=value" for the entries of dict.
// If userOptions is not NULL, only the keys of dict that are also in userOptions are listed, with
// the value from userOptions (the plug-in may have merged its own value, e.g. for movflags).
static string
dictToString(const AVDictionary* dict,
             const AVDictionary* userOptions = nullptr)
{
    string ret;
    for (const AVDictionaryEntry* e = av_dict_iterate(dict, nullptr); e; e = av_dict_iterate(dict, e)) {
        const char* value = e->value;
        if (userOptions) {
            const AVDictionaryEntry* u = av_dict_get(userOptions, e->key, nullptr, 0);
            if (!u) {
                continue;
            }
            value = u->value;
        }
        if (!ret.empty()) {
            ret += ", ";
        }
        ret += e->key;
        ret += '=';
        ret += value;
    }

    return ret;
}

class WriteFFmpegPlugin
    : public GenericWriterPlugin {
private:
    enum WriterError { SUCCESS = 0,
                       IGNORE_FINISH,
                       CLEANUP };

    struct CodecParams {
        CodecParams()
            : crf(false)
            , x26xSpeed(false)
            , bitrate(false)
            , bitrateTol(false)
            , qscale(false)
            , qrange(false)
            , interGOP(false)
            , interB(false)
            , crfLossless(false)
        {
        }

        // crf, bitrate, and qscale are exclusive: only one of the three may be used, even if the codec supports several
        bool crf; // "crf" option
        bool x26xSpeed; // "preset" option, specific to x264 and x265
        bool bitrate; // bit_rate
        bool bitrateTol; // bit_rate_tolerance
        bool qscale; // "global_quality" option
        bool qrange; // qmin, qmax
        bool interGOP; // gop_size
        bool interB; // "bf" option or max_b_frames
        bool crfLossless; // the Output Quality Lossless (CRF 0) may be lossless
    };

public:
    WriteFFmpegPlugin(OfxImageEffectHandle handle, const vector<string>& extensions);

    virtual ~WriteFFmpegPlugin();

    // Called by createInstance(). GenericWriterPlugin::restoreStateFromParams() guesses the format and
    // the codec from the filename (onOutputFileChanged()): the pixel format and the persistent message
    // are only updated then, or e.g. a new writer for a .mp4 file would check the default codec (ProRes)
    // in mp4, and fail. An error in the settings (e.g. a codec that the container guessed from a .m4v
    // file does not support) is shown in the persistent message, and must not make the creation fail.
    virtual void restoreStateFromParams() OVERRIDE FINAL
    {
        // (an exception would escape from the factory's createInstance(), which would leak the instance)
        try {
            GenericWriterPlugin::restoreStateFromParams(); // may throw, e.g. checkCodec() for an unavailable codec
            syncPrivateData();
        } catch (const OFX::Exception::Suite&) {
            // already reported (by checkCodec() or updatePixelFormat())
        }
    }

private:
    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;
    virtual void changedClip(const InstanceChangedArgs& args, const string& clipName) OVERRIDE FINAL;
    virtual void onOutputFileChanged(const string& filename, bool setColorSpace) OVERRIDE FINAL;
    /** @brief The sync private data action, called when the effect needs to sync any private data to persistent parameters */
    virtual void syncPrivateData(void) OVERRIDE FINAL
    {
        updateVisibility();
        updatePixelFormat();
    }

    /**
     * @brief Does the given filename support alpha channel.
     **/
    virtual bool supportsAlpha(const std::string&) const OVERRIDE FINAL;

    /** @brief the effect is about to be actively edited by a user, called when the first user interface is opened on an instance */
    virtual void beginEdit(void) OVERRIDE FINAL;
    virtual void beginEncode(const string& filename, const OfxRectI& rodPixel, float pixelAspectRatio, const BeginSequenceRenderArguments& args) OVERRIDE FINAL;
    virtual void endEncode(const EndSequenceRenderArguments& args) OVERRIDE FINAL;
    virtual void encode(const string& filename,
                        const OfxTime time,
                        const string& viewName,
                        const float* pixelData,
                        const OfxRectI& bounds,
                        const float pixelAspectRatio,
                        const int pixelDataNComps,
                        const int dstNCompsStartIndex,
                        const int dstNComps,
                        const int rowBytes) OVERRIDE FINAL;
    virtual bool isImageFile(const string& fileExtension) const OVERRIDE FINAL;
    // a movie has a single video stream: only view 0 is encoded (#116)
    virtual bool encodesMainViewOnly() const OVERRIDE FINAL { return true; }
    virtual void setOutputFrameRate(double fps) OVERRIDE FINAL;
    virtual PreMultiplicationEnum getExpectedInputPremultiplication() const OVERRIDE FINAL { return eImageUnPreMultiplied; }

private:
    void updateVisibility();
    void checkCodec();
    bool replaceMissingCodec(const string& codecShortName);
    string codecReplaced() const;
    void setCodecReplaced(const string& codecShortName);
    int freeFormat();
    void getColorInfo(AVColorPrimaries* color_primaries, AVColorTransferCharacteristic* color_trc) const;
    const AVOutputFormat* initFormat(bool reportErrors, const string* filename = nullptr) const;
    bool writesTimecodeTrack(const AVOutputFormat* fmt) const;
    bool getStartTimecode(const AVOutputFormat* fmt, string* canonical, string* error) const;
    string getSettingsErrors() const;
    string getSettingsWarnings() const;
    string encodeWarnings(const AVOutputFormat* fmt,
                          const AVCodec* videoCodec,
                          FFmpeg::PixelCodingEnum prefPixelCoding,
                          int prefBitDepth,
                          bool prefAlpha,
                          AVPixelFormat target,
                          AVPixelFormat alphaFormat) const;
    void updatePersistentMessage();
    bool initCodec(const AVOutputFormat* fmt, AVCodecID& outCodecId, const AVCodec*& outCodec) const;

    void getPixelPreferences(FFmpeg::PixelCodingEnum* pixelCoding, int* bitdepth) const;
    void getPixelFormats(const AVOutputFormat* fmt,
                         const AVCodec* videoCodec,
                         FFmpeg::PixelCodingEnum pixelCoding,
                         int bitdepth,
                         bool alpha,
                         AVPixelFormat& outRgbBufferPixelFormat,
                         AVPixelFormat& outTargetPixelFormat,
                         bool* outAlphaDropped = nullptr) const;
    void getPixelFormatsFromList(const AVOutputFormat* fmt,
                                 const AVCodec* videoCodec,
                                 FFmpeg::PixelCodingEnum pixelCoding,
                                 int bitdepth,
                                 bool alpha,
                                 AVPixelFormat& outRgbBufferPixelFormat,
                                 AVPixelFormat& outTargetPixelFormat) const;

    void updateBitrateToleranceRange();
    bool isRec709Format(const int height) const;
    bool isPalFormat(const AVRational& displayRatio) const;
    void setColorTags(AVCodecContext* avCodecContext, AVStream* avStream, AVPixelFormat pixelFormat) const;
    static bool IsYUVFromShortName(const char* shortName, int codecProfile);
    static AVPixelFormat GetRGBPixelFormatFromBitDepth(const int bitDepth, const bool hasAlpha);
    static CodecParams GetCodecSupportedParams(const AVCodec* codec);

    void configureAudioStream(const AVCodec* avCodec, AVStream* avStream);
    void configureVideoStream(const AVCodec* avCodec, MyAVStream* avStream);
    void addStream(AVFormatContext* avFormatContext, const AVCodec* avCodec, MyAVStream* myStreamOut);
    int openCodec(AVFormatContext* avFormatContext, const AVCodec* avCodec, MyAVStream* myAVStream, AVDictionary** options = nullptr);
    int writeAudio(AVFormatContext* avFormatContext, AVStream* avStream, bool flush);
    int writeVideo(AVFormatContext* avFormatContext, MyAVStream* myAVStream, bool flush, double time, const float* pixelData = nullptr, const OfxRectI* bounds = nullptr, int pixelDataNComps = 0, int dstNComps = 0, int rowBytes = 0);
    int writeToFile(AVFormatContext* avFormatContext, bool finalise, double time, const float* pixelData = nullptr, const OfxRectI* bounds = nullptr, int pixelDataNComps = 0, int dstNComps = 0, int rowBytes = 0);

    // Creates _convertCtx, which converts the images (srcPixelFormat) to the frames of the encoder (dstPixelFormat),
    // and sets _rgb16White. Returns a negative value on error (then _convertCtx is null).
    int initConvertCtx(AVPixelFormat srcPixelFormat, AVPixelFormat dstPixelFormat, AVCodecContext* avCodecContext);
    int colourSpaceConvert(AVFrame* avFrameIn, AVFrame* avFrameOut, AVPixelFormat srcPixelFormat, AVPixelFormat dstPixelFormat, AVCodecContext* avCodecContext);

    // Returns true if the selected channels contain alpha and that the channel is valid
    bool alphaEnabled() const;

    // Returns the nmber of destination channels this will write into.
    int numberOfDestChannels() const;

    bool codecIndexIsInRange(unsigned int codecIndex) const;
    bool codecIsDisallowed(const string& codecShortName, string& reason) const;

    void updatePixelFormat(); // update info for output pixel format
    void availPixelFormats();

    /// These members are not protected and only read/written by/to by the same thread.
    OfxRectI _rodPixel;
    float _pixelAspectRatio;
    bool _isOpen; // Flag for the configuration state of the FFmpeg components.
    uint64_t _pts_counter;
    int64_t _lastDts; // the dts of the last video packet written, in the codec time base (AV_NOPTS_VALUE if none)
    int64_t _packetsWritten; // the number of video packets written since beginEncode()
    WriterError _error;
    AVFormatContext* _formatContext;
    SwsContext* _convertCtx;
    // the value of the 16-bit R', G' and B' of the images that _convertCtx converts to white (see MeasureRgb16White()):
    // like _convertCtx, set by the encoding of a frame (writeToFile(), under _nextFrameToEncodeMutex) and endEncode()
    unsigned _rgb16White;
    MyAVStream _streamVideo;
    MyAVStream _streamAudio;
    // _nextFrameToEncodeMutex guards _nextFrameToEncode, _firstFrameToEncode, _lastFrameToEncode, _frameStep, and,
    // in encode(), _error and the encoding of a frame (writeToFile()), which the render threads do one at a time and in
    // order; _nextFrameToEncodeCond is notified when _nextFrameToEncode changes. beginEncode() and endEncode()
    // (beginSequenceRender and endSequenceRender) are not concurrent with the renders (C++ Core Guidelines CP.50
    // https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconc-mutex).
    std::mutex _nextFrameToEncodeMutex;
    std::condition_variable _nextFrameToEncodeCond;
    int _nextFrameToEncode; //< the frame index we need to encode next, INT_MIN means uninitialized
    int _firstFrameToEncode;
    int _lastFrameToEncode;
    int _frameStep;
    ChoiceParam* _format;
    DoubleParam* _fps;
    ChoiceParam* _prefPixelCoding;
    ChoiceParam* _prefBitDepth;
    BooleanParam* _prefAlpha;
    StringParam* _infoPixelFormat;
    IntParam* _infoBitDepth;
    IntParam* _infoBPP;
    ChoiceParam* _dnxhdCodecProfile;
    ChoiceParam* _encodeVideoRange;
    ChoiceParam* _hapFormat;

    ChoiceParam* _codec;
    StringParam* _codecShortName;
    // if not empty, the codec of the project that this FFmpeg does not have, which was replaced by rawvideo (see
    // checkCodec()): reported in the persistent message until the user chooses a codec. Read and written only through
    // codecReplaced() and setCodecReplaced(): the main thread writes it (changedParam(), checkCodec()) while a render
    // thread may read or write it in beginEncode() (encodeWarnings(), checkCodec()), #319 (C++ Core Guidelines CP.2
    // https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconc-races). It can not be recomputed from the
    // params: replaceMissingCodec() sets the codecShortName param to rawvideo.
    string _codecReplaced;
    // guards _codecReplaced only; a leaf lock, held only to copy or assign the string (no other lock is taken and the
    // host is not called while it is held)
    mutable std::mutex _codecReplacedMutex;
    // the warnings about the file being written (see encodeWarnings()), shown again at the end of the render, since
    // GenericWriterPlugin::render() clears the persistent message after each frame
    string _encodeWarnings;
    ChoiceParam* _crf;
    ChoiceParam* _x26xSpeed;
    DoubleParam* _qscale;
    DoubleParam* _bitrate;
    DoubleParam* _bitrateTolerance;
    Int2DParam* _quality;
    IntParam* _gopSize;
    IntParam* _bFrames;
    BooleanParam* _writeNCLC;
#if OFX_FFMPEG_MBDECISION
    ChoiceParam* _mbDecision;
#endif
    BooleanParam* _fastStart;
    StringParam* _codecOptions;
    StringParam* _formatOptions;
    StringParam* _startTimecode;

    // we use shared_ptr here as both frames can point to the same AVFrame (ProRes, DNxHD case)
    std::shared_ptr<AVFrame> inputFrame_;
    std::shared_ptr<AVFrame> outputFrame_;
};

class FFmpegSingleton {
public:
    static FFmpegSingleton& Instance()
    {
        return m_instance;
    };
    const vector<string>& getFormatsShortNames() const { return _formatsShortNames; }

    const vector<string>& getFormatsLongNames() const { return _formatsLongNames; }

    const vector<vector<size_t>>& getFormatsCodecs() const { return _formatsCodecs; }

    const vector<string>& getCodecsShortNames() const { return _codecsShortNames; }

    const vector<string>& getCodecsLongNames() const { return _codecsLongNames; }

    const vector<string>& getCodecsKnobLabels() const { return _codecsKnobLabels; }

    const vector<AVCodecID>& getCodecsIds() const { return _codecsIds; }

    const vector<vector<size_t>>& getCodecsFormats() const { return _codecsFormats; }

private:
    FFmpegSingleton& operator=(const FFmpegSingleton&)
    {
        return *this;
    }

    FFmpegSingleton(const FFmpegSingleton&) { }

    static FFmpegSingleton m_instance;

    FFmpegSingleton();

    ~FFmpegSingleton();

    vector<string> _formatsLongNames;
    vector<string> _formatsShortNames;
    vector<vector<size_t>> _formatsCodecs; // for each format, give the list of compatible codecs (indices in the codecs list)
    vector<string> _codecsLongNames;
    vector<string> _codecsShortNames;
    vector<string> _codecsKnobLabels;
    vector<AVCodecID> _codecsIds;
    vector<vector<size_t>> _codecsFormats; // for each codec, give the list of compatible formats (indices in the formats list)
};

FFmpegSingleton FFmpegSingleton::m_instance = FFmpegSingleton();

FFmpegSingleton::FFmpegSingleton()
{
    // TODO: add a log buffer and a way to display it / clear it.
    av_log_set_level(AV_LOG_WARNING);
    // av_log_set_level(AV_LOG_DEBUG);

    _formatsLongNames.push_back("guess from filename");
    _formatsShortNames.push_back("default");
    void* opaqueMuxerIter = nullptr;
    const AVOutputFormat* fmt = nullptr;
    while ((fmt = av_muxer_iterate(&opaqueMuxerIter))) {
        if (fmt->video_codec != AV_CODEC_ID_NONE) { // if this is a video format, it should have a default video codec
            if (FFmpegFile::isFormatAllowedForWriting(fmt->name)) {
                if (fmt->long_name) {
                    _formatsLongNames.push_back(string(fmt->long_name) + string(" [") + string(fmt->name) + string("]"));
                } else {
                    _formatsLongNames.push_back(fmt->name);
                }
                _formatsShortNames.push_back(fmt->name);
#if OFX_FFMPEG_PRINT_CODECS
                std::cout << "Format: " << fmt->name << " = " << fmt->long_name << std::endl;
#endif //  FFMPEG_PRINT_CODECS
            }
#if OFX_FFMPEG_PRINT_CODECS
            else {
                std::cout << "Disallowed Format: " << fmt->name << " = " << fmt->long_name << std::endl;
            }
#endif //  FFMPEG_PRINT_CODECS
        }
    }
    assert(_formatsLongNames.size() == _formatsShortNames.size());

    // Apple ProRes support.
    // short name must start with prores_ap
    // knoblabel must start with FourCC
    _codecsShortNames.push_back(kProresCodec kProresProfile4444FourCC);
    _codecsLongNames.push_back(kProresProfile4444Name);
    _codecsKnobLabels.push_back(kProresProfile4444FourCC "\t" kProresProfile4444Name);
    _codecsIds.push_back(AV_CODEC_ID_PRORES);
    _codecsShortNames.push_back(kProresCodec kProresProfileHQFourCC);
    _codecsLongNames.push_back(kProresProfileHQName);
    _codecsKnobLabels.push_back(kProresProfileHQFourCC "\t" kProresProfileHQName);
    _codecsIds.push_back(AV_CODEC_ID_PRORES);

    _codecsShortNames.push_back(kProresCodec kProresProfileSQFourCC);
    _codecsLongNames.push_back(kProresProfileSQName);
    _codecsKnobLabels.push_back(kProresProfileSQFourCC "\t" kProresProfileSQName);
    _codecsIds.push_back(AV_CODEC_ID_PRORES);

    _codecsShortNames.push_back(kProresCodec kProresProfileLTFourCC);
    _codecsLongNames.push_back(kProresProfileLTName);
    _codecsKnobLabels.push_back(kProresProfileLTFourCC "\t" kProresProfileLTName);
    _codecsIds.push_back(AV_CODEC_ID_PRORES);

    _codecsShortNames.push_back(kProresCodec kProresProfileProxyFourCC);
    _codecsLongNames.push_back(kProresProfileProxyName);
    _codecsKnobLabels.push_back(kProresProfileProxyFourCC "\t" kProresProfileProxyName);
    _codecsIds.push_back(AV_CODEC_ID_PRORES);

    // ProRes 4444 XQ comes after the other ProRes profiles, so that their indices, including the default codec
    // (ProRes 4444), do not change. The index of every codec after it increases by one: the codec of a saved
    // project is restored by its option ID (in Natron), or by checkCodec() from the codecShortName param.
    _codecsShortNames.push_back(kProresCodec kProresProfile4444XQFourCC);
    _codecsLongNames.push_back(kProresProfile4444XQName);
    _codecsKnobLabels.push_back(kProresProfile4444XQFourCC "\t" kProresProfile4444XQName);
    _codecsIds.push_back(AV_CODEC_ID_PRORES);

    void* opaqueCodecIter = nullptr;
    const AVCodec* c = nullptr;
    while ((c = av_codec_iterate(&opaqueCodecIter))) {
        if ((c->type == AVMEDIA_TYPE_VIDEO) && av_codec_is_encoder(c)) {
            if (FFmpegFile::isCodecAllowedForWriting(c->name) && (c->long_name)) {
                const char* knobLabel = getCodecKnobLabel(c->name);
                if (knobLabel == nullptr) {
#if OFX_FFMPEG_PRINT_CODECS
                    std::cout << "Codec allowlisted but unknown: " << c->name << " = " << c->long_name << std::endl;
#endif //  FFMPEG_PRINT_CODECS
                } else {
#if OFX_FFMPEG_PRINT_CODECS
                    std::cout << "Codec[" << _codecsLongNames.size() << "]: " << c->name << " = " << c->long_name << std::endl;
#endif //  FFMPEG_PRINT_CODECS
                    _codecsLongNames.push_back(c->long_name);
                    _codecsShortNames.push_back(c->name);
                    _codecsKnobLabels.push_back(knobLabel);
                    _codecsIds.push_back(c->id);
                    assert(_codecsLongNames.size() == _codecsShortNames.size());
                    assert(_codecsLongNames.size() == _codecsKnobLabels.size());
                    assert(_codecsLongNames.size() == _codecsIds.size());
                }
            }
#if OFX_FFMPEG_PRINT_CODECS
            else {
                std::cout << "Disallowed Codec: " << c->name << " = " << c->long_name << std::endl;
            }
#endif //  FFMPEG_PRINT_CODECS
        }
    }

    // find out compatible formats/codecs
    vector<vector<string>> codecsFormatStrings(_codecsIds.size());
    vector<vector<AVCodecID>> formatsCodecIDs(_formatsShortNames.size());
    for (size_t f = 1; f < _formatsShortNames.size(); ++f) { // format 0 is "default"
        fmt = av_guess_format(_formatsShortNames[f].c_str(), nullptr, nullptr);
        if (fmt) {
            for (size_t c = 0; c < _codecsIds.size(); ++c) {
                if (codecCompatible(fmt, _codecsIds[c])) {
                    codecsFormatStrings[c].push_back(_formatsShortNames[f]);
                    formatsCodecIDs[f].push_back(_codecsIds[c]);
                }
            }
        }
    }

    // remove formats that have no codecs
    const vector<string> formatsLongNamesOrig = _formatsLongNames;
    const vector<string> formatsShortNamesOrig = _formatsShortNames;
    const vector<vector<AVCodecID>> formatsCodecIDsOrig = formatsCodecIDs;
    _formatsLongNames.resize(1);
    _formatsShortNames.resize(1);
    formatsCodecIDs.resize(1);
    for (size_t f = 1; f < formatsShortNamesOrig.size(); ++f) { // format 0 is "default"
        if (!formatsCodecIDsOrig[f].empty()) {
            _formatsLongNames.push_back(formatsLongNamesOrig[f]);
            _formatsShortNames.push_back(formatsShortNamesOrig[f]);
            formatsCodecIDs.push_back(formatsCodecIDsOrig[f]);
        }
    }
    // remove codecs that have no format
    const vector<string> codecsLongNamesOrig = _codecsLongNames;
    const vector<string> codecsShortNamesOrig = _codecsShortNames;
    const vector<string> codecsKnobLabelsOrig = _codecsKnobLabels;
    const vector<AVCodecID> codecsIdsOrig = _codecsIds;
    const vector<vector<string>> codecsFormatStringsOrig = codecsFormatStrings;
    _codecsLongNames.clear();
    _codecsShortNames.clear();
    _codecsKnobLabels.clear();
    _codecsIds.clear();
    codecsFormatStrings.clear();
    for (size_t c = 0; c < codecsIdsOrig.size(); ++c) {
        if (!codecsFormatStringsOrig[c].empty()) {
            _codecsLongNames.push_back(codecsLongNamesOrig[c]);
            _codecsShortNames.push_back(codecsShortNamesOrig[c]);
            _codecsKnobLabels.push_back(codecsKnobLabelsOrig[c]);
            _codecsIds.push_back(codecsIdsOrig[c]);
            codecsFormatStrings.push_back(codecsFormatStringsOrig[c]);
        }
    }

    // fill the entries in _codecsFormats and _formatsCodecs
    _codecsFormats.resize(_codecsIds.size());
    _formatsCodecs.resize(_formatsShortNames.size());
    for (size_t f = 1; f < _formatsShortNames.size(); ++f) { // format 0 is "default"
        fmt = av_guess_format(_formatsShortNames[f].c_str(), nullptr, nullptr);
        if (fmt) {
            for (size_t c = 0; c < _codecsIds.size(); ++c) {
                if (std::find(formatsCodecIDs[f].begin(), formatsCodecIDs[f].end(), _codecsIds[c]) != formatsCodecIDs[f].end()) {
                    _codecsFormats[c].push_back(f);
                    _formatsCodecs[f].push_back(c);
                }
            }
        }
    }
}

FFmpegSingleton::~FFmpegSingleton()
{
}

WriteFFmpegPlugin::WriteFFmpegPlugin(OfxImageEffectHandle handle,
                                     const vector<string>& extensions)
    : GenericWriterPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha)
    , _pixelAspectRatio(1.)
    , _isOpen(false)
    , _pts_counter(0)
    , _lastDts(AV_NOPTS_VALUE)
    , _packetsWritten(0)
    , _error(IGNORE_FINISH)
    , _formatContext(nullptr)
    , _convertCtx(nullptr)
    , _rgb16White(FFmpeg::kWhite16)
    , _streamVideo({ nullptr, nullptr })
    , _streamAudio({ nullptr, nullptr })
    , _nextFrameToEncodeMutex()
    , _nextFrameToEncodeCond()
    , _nextFrameToEncode(INT_MIN)
    , _firstFrameToEncode(1)
    , _lastFrameToEncode(1)
    , _frameStep(1)
    , _format(nullptr)
    , _fps(nullptr)
    , _prefPixelCoding(nullptr)
    , _prefBitDepth(nullptr)
    , _prefAlpha(nullptr)
    , _infoPixelFormat(nullptr)
    , _infoBitDepth(nullptr)
    , _infoBPP(nullptr)
    , _dnxhdCodecProfile(nullptr)
    , _encodeVideoRange(nullptr)
    , _hapFormat(nullptr)
    , _codec(nullptr)
    , _codecShortName(nullptr)
    , _crf(nullptr)
    , _x26xSpeed(nullptr)
    , _qscale(nullptr)
    , _bitrate(nullptr)
    , _bitrateTolerance(nullptr)
    , _quality(nullptr)
    , _gopSize(nullptr)
    , _bFrames(nullptr)
    , _writeNCLC(nullptr)
#if OFX_FFMPEG_MBDECISION
    , _mbDecision(nullptr)
#endif
    , _codecOptions(nullptr)
    , _formatOptions(nullptr)
{
    _rodPixel.x1 = _rodPixel.y1 = 0;
    _rodPixel.x2 = _rodPixel.y2 = -1;
    _format = fetchChoiceParam(kParamFormat);
    _fps = fetchDoubleParam(kParamFPS);
    _prefPixelCoding = fetchChoiceParam(kParamPrefPixelCoding);
    _prefBitDepth = fetchChoiceParam(kParamPrefBitDepth);
    _prefAlpha = fetchBooleanParam(kParamPrefAlpha);
    _infoPixelFormat = fetchStringParam(kParamInfoPixelFormat);
    _infoBitDepth = fetchIntParam(kParamInfoBitDepth);
    _infoBPP = fetchIntParam(kParamInfoBPP);
    _dnxhdCodecProfile = fetchChoiceParam(kParamDNxHDCodecProfile);
    _encodeVideoRange = fetchChoiceParam(kParamDNxHDEncodeVideoRange);
    _hapFormat = fetchChoiceParam(kParamHapFormat);
    _codec = fetchChoiceParam(kParamCodec);
    _codecShortName = fetchStringParam(kParamCodecShortName);
    _crf = fetchChoiceParam(kParamCRF);
    _x26xSpeed = fetchChoiceParam(kParamX26xSpeed);
    _qscale = fetchDoubleParam(kParamQScale);
    _bitrate = fetchDoubleParam(kParamBitrate);
    _bitrateTolerance = fetchDoubleParam(kParamBitrateTolerance);
    _quality = fetchInt2DParam(kParamQuality);
    _gopSize = fetchIntParam(kParamGopSize);
    _bFrames = fetchIntParam(kParamBFrames);
    _writeNCLC = fetchBooleanParam(kParamWriteNCLC);
#if OFX_FFMPEG_MBDECISION
    _mbDecision = fetchChoiceParam(kParamMBDecision);
#endif
    _fastStart = fetchBooleanParam(kParamFastStart);
    _codecOptions = fetchStringParam(kParamCodecOptions);
    _formatOptions = fetchStringParam(kParamFormatOptions);
    _startTimecode = fetchStringParam(kParamStartTimecode);

    // syncPrivateData() is called by restoreStateFromParams()
}

WriteFFmpegPlugin::~WriteFFmpegPlugin()
{
    // a host may destroy the instance without endSequenceRender, e.g. after a failed render (#289)
    freeFormat();
}

bool
WriteFFmpegPlugin::isImageFile(const string& ext) const
{
    return FFmpegFile::isImageFile("." + ext);
}

bool
WriteFFmpegPlugin::isRec709Format(const int height) const
{
    // First check for codecs which require special handling:
    //  * JPEG codecs always use Rec 601.
    assert(_codec);
    int codec = _codec->getValue();
    const bool isJpeg = IsJpeg(_codec, codec);
    if (isJpeg) {
        return false;
    }

    // Using method described in step 5 of QuickTimeCodecReader::setPreferredMetadata
    // #cristian: height >= 720 (HD); height < 720 (SD)
    return (height >= 720);
}

bool
WriteFFmpegPlugin::isPalFormat(const AVRational& displayRatio) const
{
    const AVRational palRatio = { 5, 4 };
    const bool isPAL = (displayRatio.num == 576) || (av_cmp_q(palRatio, displayRatio) == 0);

    return isPAL;
}

// Set the color tags of the video stream, once its pixel format is known. configureVideoStream() set the primaries
// and transfer function of the OCIO output colorspace (getColorInfo()), which are kept.
// - For the usual video codecs (IsYUVFromShortName()), the primaries and transfer function that getColorInfo() does not
//   give are those of the usual video formats: BT.709 primaries for HD, EBU 3213 (BT.470BG) for PAL and SMPTE C (SMPTE
//   170M) for other SD formats, and the BT.709 transfer function.
// - ProRes: the frame header (where prores_ks writes the transfer function of the frame as is, FFmpeg n9.0
//   libavcodec/proresenc_kostya.c:786-788) only allows the transfer_characteristic codes 0, 1 (BT.709), 2
//   (unspecified), 16 (PQ) and 18 (HLG) (SMPTE RDD 36): the BT.601 and BT.2020 transfer functions are the BT.709 one
//   (ITU-T H.273 codes 6, 14 and 15 are "functionally the same" as code 1), the others (sRGB, linear, log, gamma 2.2)
//   are unspecified.
// - The Y'CbCr matrix (AVCodecContext.colorspace, which the encoders and muxers write, e.g. libx264 in the VUI, FFmpeg
//   n9.0 libavcodec/libx264.c:1340-1341, and the mov muxer in the 'colr' atom, libavformat/movenc.c:2648-2650) is the
//   one that colourSpaceConvert() converts with: for a Y'CbCr pixel format, BT.2020 non-constant luminance with the
//   BT.2020 primaries (except for JPEG, which is always BT.601, and DNxHD/DNxHR, which is always BT.709, see
//   below), else BT.709 for HD and BT.601 (SMPTE 170M) for SD, and for an RGB pixel format, AVCOL_SPC_RGB
//   (libavutil/pixfmt.h: "order of coefficients is actually GBR").
//   Before, the matrix was only set for the codecs of IsYUVFromShortName(), so the other Y'CbCr streams (e.g.
//   H.264) were untagged, and the RGB ones too.
void
WriteFFmpegPlugin::setColorTags(AVCodecContext* avCodecContext,
                                AVStream* avStream,
                                AVPixelFormat pixelFormat) const
{
    const int codec = _codec->getValue();
    const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
    int dnxhdCodecProfile_i = 0;
    _dnxhdCodecProfile->getValue(dnxhdCodecProfile_i);
    const bool isYUVCodec = (codec >= 0) && (codec < (int)codecsShortNames.size()) && IsYUVFromShortName(codecsShortNames[codec].c_str(), dnxhdCodecProfile_i);
    const bool isHD = isRec709Format(avCodecContext->height); // false for JPEG

    if (isYUVCodec) {
        if (avCodecContext->color_primaries == AVCOL_PRI_UNSPECIFIED) {
            const AVRational displayRatio = { avCodecContext->width, avCodecContext->height };
            if (isHD) {
                avCodecContext->color_primaries = AVCOL_PRI_BT709; // kQTPrimaries_ITU_R709_2
            } else if (isPalFormat(displayRatio)) {
                avCodecContext->color_primaries = AVCOL_PRI_BT470BG; // kQTPrimaries_EBU_3213
            } else {
                avCodecContext->color_primaries = AVCOL_PRI_SMPTE170M; // kQTPrimaries_SMPTE_C
            }
        }
        if (avCodecContext->color_trc == AVCOL_TRC_UNSPECIFIED) {
            avCodecContext->color_trc = AVCOL_TRC_BT709; // kQTTransferFunction_ITU_R709_2
        }
    }
    if (avCodecContext->codec_id == AV_CODEC_ID_PRORES) {
        switch (avCodecContext->color_trc) {
        case AVCOL_TRC_BT709:
        case AVCOL_TRC_UNSPECIFIED:
        case AVCOL_TRC_SMPTE2084:
        case AVCOL_TRC_ARIB_STD_B67:
            break;
        case AVCOL_TRC_SMPTE170M:
        case AVCOL_TRC_BT2020_10:
        case AVCOL_TRC_BT2020_12:
            avCodecContext->color_trc = AVCOL_TRC_BT709;
            break;
        default:
            avCodecContext->color_trc = AVCOL_TRC_UNSPECIFIED;
            break;
        }
    }

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixelFormat);
    if (FFmpeg::pixelFormatIsYUV(pixelFormat)) {
        const bool isJpeg = IsJpeg(_codec, codec);
        if (avCodecContext->codec_id == AV_CODEC_ID_DNXHD) {
            // DNxHD/DNxHR: the matrix is in the frame header, which FFmpeg's encoder always writes as BT.709 (bits 1-2
            // of byte 0x2c are 0, FFmpeg n8.0 libavcodec/dnxhdenc.c:545, n9.0.2 :544), and FFmpeg's decoder sets
            // the frame's matrix from it (n8.0 and n9.0.2 libavcodec/dnxhddec.c:237-242), so the stream's one (the
            // MOV 'colr' atom) is ignored: fill_frame_props() only uses it if the frame's is unspecified (n8.0
            // libavcodec/decode.c:564-565, n9.0.2 :582-583). So the BT.709 matrix, at all sizes and with the
            // BT.2020 primaries too: the one every decoder assumes for this file.
            avCodecContext->colorspace = AVCOL_SPC_BT709;
        } else if ((avCodecContext->color_primaries == AVCOL_PRI_BT2020) && !isJpeg) {
            avCodecContext->colorspace = AVCOL_SPC_BT2020_NCL;
        } else {
            avCodecContext->colorspace = isHD ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        }
    } else if (desc && (desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        avCodecContext->colorspace = AVCOL_SPC_RGB;
    }

    if (isYUVCodec && _writeNCLC->getValue()) {
        // the codes as decimal numbers (they may have two digits, e.g. 13 for the sRGB transfer function)
        av_dict_set_int(&avStream->metadata, kNCLCPrimariesKey, (int64_t)avCodecContext->color_primaries, 0);
        av_dict_set_int(&avStream->metadata, kNCLCTransferKey, (int64_t)avCodecContext->color_trc, 0);
        av_dict_set_int(&avStream->metadata, kNCLCMatrixKey, (int64_t)avCodecContext->colorspace, 0);
    }
} // WriteFFmpegPlugin::setColorTags

// Figure out if a codec is definitely YUV based from its shortname.
// This decides whether the primaries and transfer function of the stream, if getColorInfo() does not give them,
// are those of the usual video formats (BT.709 for HD, BT.601 for SD, see setColorTags()), which also gives the
// QuickTime 'colr' (nclc) atom.
// All the ProRes profiles are Y'CbCr: 4:2:2, or 4:4:4 for ProRes 4444 and 4444 XQ (SMPTE RDD 36), and
// FFmpeg's prores_ks encoder only accepts yuv422p10, yuv444p10 and yuva444p10.
/*static*/
bool
WriteFFmpegPlugin::IsYUVFromShortName(const char* shortName,
                                      int codecProfile)
{
    return (!strcmp(shortName, kProresCodec kProresProfile4444XQFourCC) || !strcmp(shortName, kProresCodec kProresProfile4444FourCC)
            || !strcmp(shortName, kProresCodec kProresProfileHQFourCC) || !strcmp(shortName, kProresCodec kProresProfileSQFourCC)
            || !strcmp(shortName, kProresCodec kProresProfileLTFourCC) || !strcmp(shortName, kProresCodec kProresProfileProxyFourCC)
            || ((codecProfile != (int)eDNxHDCodecProfileHR444) && !strcmp(shortName, "dnxhd")) // DNxHR 444 is RGB
            || !strcmp(shortName, "mjpeg") || !strcmp(shortName, "mpeg1video") || !strcmp(shortName, "mpeg4") || !strcmp(shortName, "v210"));
}

void
WriteFFmpegPlugin::getColorInfo(AVColorPrimaries* color_primaries,
                                AVColorTransferCharacteristic* color_trc) const
{
    // AVCOL_PRI_RESERVED0   = 0,
    // AVCOL_PRI_BT709       = 1, ///< also ITU-R BT1361 / IEC 61966-2-4 / SMPTE RP177 Annex B
    // AVCOL_PRI_UNSPECIFIED = 2,
    // AVCOL_PRI_RESERVED    = 3,
    // AVCOL_PRI_BT470M      = 4, ///< also FCC Title 47 Code of Federal Regulations 73.682 (a)(20)
    //
    // AVCOL_PRI_BT470BG     = 5, ///< also ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM
    // AVCOL_PRI_SMPTE170M   = 6, ///< also ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC
    // AVCOL_PRI_SMPTE240M   = 7, ///< functionally identical to above
    // AVCOL_PRI_FILM        = 8, ///< colour filters using Illuminant C
    // AVCOL_PRI_BT2020      = 9, ///< ITU-R BT2020
    // AVCOL_PRI_NB,              ///< Not part of ABI

    // AVCOL_TRC_RESERVED0    = 0,
    // AVCOL_TRC_BT709        = 1,  ///< also ITU-R BT1361
    // AVCOL_TRC_UNSPECIFIED  = 2,
    // AVCOL_TRC_RESERVED     = 3,
    // AVCOL_TRC_GAMMA22      = 4,  ///< also ITU-R BT470M / ITU-R BT1700 625 PAL & SECAM
    // AVCOL_TRC_GAMMA28      = 5,  ///< also ITU-R BT470BG
    // AVCOL_TRC_SMPTE170M    = 6,  ///< also ITU-R BT601-6 525 or 625 / ITU-R BT1358 525 or 625 / ITU-R BT1700 NTSC
    // AVCOL_TRC_SMPTE240M    = 7,
    // AVCOL_TRC_LINEAR       = 8,  ///< "Linear transfer characteristics"
    // AVCOL_TRC_LOG          = 9,  ///< "Logarithmic transfer characteristic (100:1 range)"
    // AVCOL_TRC_LOG_SQRT     = 10, ///< "Logarithmic transfer characteristic (100 * Sqrt(10) : 1 range)"
    // AVCOL_TRC_IEC61966_2_4 = 11, ///< IEC 61966-2-4
    // AVCOL_TRC_BT1361_ECG   = 12, ///< ITU-R BT1361 Extended Colour Gamut
    // AVCOL_TRC_IEC61966_2_1 = 13, ///< IEC 61966-2-1 (sRGB or sYCC)
    // AVCOL_TRC_BT2020_10    = 14, ///< ITU-R BT2020 for 10 bit system
    // AVCOL_TRC_BT2020_12    = 15, ///< ITU-R BT2020 for 12 bit system

    *color_primaries = AVCOL_PRI_UNSPECIFIED;
    *color_trc = AVCOL_TRC_UNSPECIFIED;

#ifdef OFX_IO_USING_OCIO
    string selection;
    assert(_ocio.get());
    _ocio->getOutputColorspace(selection);
    if ((selection.find("sRGB") != string::npos) || // sRGB in nuke-default and blender
        (selection.find("srgb") != string::npos) || (selection == "sRGB Curve") || // natron
        (selection == "sRGB D65") || // blender-cycles
        (selection == "sRGB (D60 sim.)") || // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
        (selection == "out_srgbd60sim") || (selection == "rrt_srgb") || // rrt_srgb in aces
        (selection == "srgb8")) { // srgb8 in spi-vfx
        *color_primaries = AVCOL_PRI_BT709;
        *color_trc = AVCOL_TRC_IEC61966_2_1; ///< IEC 61966-2-1 (sRGB or sYCC)
    } else if ((selection.find("Rec709") != string::npos) || // Rec709 in nuke-default
               (selection.find("rec709") != string::npos) || (selection == "Rec 709 Curve") || // natron
               (selection == "nuke_rec709") || // nuke_rec709 in blender
               (selection == "Rec.709 - Full") || // aces 1.0.0
               (selection == "out_rec709full") || // aces 1.0.0
               (selection == "rrt_rec709_full_100nits") || // aces 0.7.1
               (selection == "rrt_rec709") || // rrt_rec709 in aces
               (selection == "hd10")) { // hd10 in spi-anim and spi-vfx
        *color_primaries = AVCOL_PRI_BT709;
        *color_trc = AVCOL_TRC_BT709; ///< also ITU-R BT1361
    } else if ((selection.find("KodakLog") != string::npos) || (selection.find("kodaklog") != string::npos) || (selection.find("Cineon") != string::npos) || // Cineon in nuke-default, blender
               (selection.find("cineon") != string::npos) || (selection == "Cineon Log Curve") || // natron
               (selection == "REDlogFilm") || // REDlogFilm in aces 1.0.0
               (selection == "adx10") || (selection == "lg10") || // lg10 in spi-vfx and blender
               (selection == "lm10") || (selection == "lgf")) {
        *color_primaries = AVCOL_PRI_BT709;
        *color_trc = AVCOL_TRC_LOG; ///< "Logarithmic transfer characteristic (100:1 range)"
    } else if ((selection.find("Gamma2.2") != string::npos) || (selection == "rrt_Gamma2.2") || (selection == "vd8") || // vd8, vd10, vd16 in spi-anim and spi-vfx
               (selection == "vd10") || (selection == "vd16") || (selection == "VD16")) { // VD16 in blender
        *color_primaries = AVCOL_PRI_BT709;
        *color_trc = AVCOL_TRC_GAMMA22; ///< also ITU-R BT470M / ITU-R BT1700 625 PAL & SECAM
    } else if ((selection.find("linear") != string::npos) || (selection.find("Linear") != string::npos) || (selection == "Linear sRGB / REC.709 D65") || // natron
               (selection == "ACES2065-1") || // ACES2065-1 in aces 1.0.0
               (selection == "aces") || // aces in aces
               (selection == "lnf") || // lnf, ln16 in spi-anim and spi-vfx
               (selection == "ln16")) {
        *color_primaries = AVCOL_PRI_BT709;
        *color_trc = AVCOL_TRC_LINEAR;
    } else if ((selection.find("Rec2020") != string::npos) || (selection == "Rec 2020 12 Bit Curve") || // natron
               (selection == "aces") || // aces in aces
               (selection == "lnf") || // lnf, ln16 in spi-anim and spi-vfx
               (selection == "ln16")) {
        *color_primaries = AVCOL_PRI_BT2020;
        *color_trc = AVCOL_TRC_BT2020_12;
    }
#endif // ifdef OFX_IO_USING_OCIO
} // WriteFFmpegPlugin::getColorInfo

// Does the output format write the start timecode as a timecode (tmcd) track?
// In libavformat/movenc.c (mov_init(), from FFmpeg 3.4 to 8.1), the write_tmcd muxer option is -1
// (auto) by default, which writes a tmcd track only in the mov and mp4 formats ("mov->write_tmcd == -1
// && (mov->mode == MODE_MOV || mov->mode == MODE_MP4) || mov->write_tmcd == 1"), and the user may set
// it in the Format Options: write_tmcd=0 writes no tmcd track, and write_tmcd=1 writes one in every
// format of the mov family (e.g. 3gp).
bool
WriteFFmpegPlugin::writesTimecodeTrack(const AVOutputFormat* fmt) const
{
    if (!isMovMuxer(fmt)) {
        return false;
    }
    int64_t writeTmcd = -1; // auto
    string str;
    _formatOptions->getValue(str);
    AVDictionaryHolder options;
    string error;
    const AVDictionaryEntry* e = nullptr;
    // a syntax error in the Format Options is reported when rendering
    if (parseOptionString(str, options.ptr(), &error) && (e = av_dict_get(options.get(), "write_tmcd", nullptr, 0))) {
        // let the muxer parse the value, as avformat_init_output() does (an invalid value makes it fail)
        AVFormatContext* ctx = nullptr;
        if ((avformat_alloc_output_context2(&ctx, fmt, nullptr, nullptr) >= 0) && ctx) {
            if (ctx->priv_data && (av_opt_set(ctx->priv_data, "write_tmcd", e->value, 0) >= 0)) {
                av_opt_get_int(ctx->priv_data, "write_tmcd", 0, &writeTmcd);
            }
            avformat_free_context(ctx);
        }
    }

    return (writeTmcd == 1) || ((writeTmcd == -1) && (!std::strcmp(fmt->name, "mov") || !std::strcmp(fmt->name, "mp4")));
}

// The validated and normalized Start Timecode, or an empty string if it is empty.
// fmt is the output format (may be null if unknown).
// Returns false and sets error if it is invalid.
bool
WriteFFmpegPlugin::getStartTimecode(const AVOutputFormat* fmt,
                                    string* canonical,
                                    string* error) const
{
    canonical->clear();
    string str;
    _startTimecode->getValue(str);
    const char* const whitespace = " \t\r\n";
    const size_t first = str.find_first_not_of(whitespace);
    if (first == string::npos) {
        return true;
    }
    str = str.substr(first, str.find_last_not_of(whitespace) - first + 1);
    // the avg_frame_rate of the video stream (see configureVideoStream()), which the muxer uses for the timecode
    const AVRational rate = frameRateFromFps(_fps->getValue());
    if (!parseTimecode(str, rate, writesTimecodeTrack(fmt) ? fmt->name : nullptr, canonical, error)) {
        *error = string("Invalid ") + kParamStartTimecodeLabel + ": " + *error;

        return false;
    }

    return true;
}

namespace {
// The AVOption of an encoder's private class (not an AV_OPT_TYPE_CONST), or nullptr. With AV_OPT_SEARCH_FAKE_OBJ,
// av_opt_find() searches the class without an encoder instance: its first argument is then a pointer to a pointer
// to the AVClass, which it only reads, but whose type is void* (FFmpeg n9.1 libavutil/opt.h), hence the const_cast.
const AVOption*
encoderOption(const AVCodec* codec,
              const char* name)
{
    if (!codec->priv_class) {
        return nullptr;
    }

    return av_opt_find(const_cast<const AVClass**>(&codec->priv_class), name, nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ);
}

// The rate control settings that a lossy encoder reads from AVCodecContext: bit_rate, bit_rate_tolerance,
// global_quality (with AV_CODEC_FLAG_QSCALE), and qmin and qmax. FFmpeg exposes no property for which of these generic
// fields an encoder reads: they are AVCodecContext fields, which every encoder has, and the encoder defaults that would
// tell (FFCodecDefault) are internal to libavcodec. The mpegvideo encoders are detected by their shared options (see
// GetCodecSupportedParams()); the others are in kEncoderRateControl.
enum class RateControl : unsigned
{
    eNone = 0,
    eBitrate = 1u << 0,
    eBitrateTol = 1u << 1,
    eQScale = 1u << 2,
    eQRange = 1u << 3,
};

constexpr RateControl
operator|(RateControl a,
          RateControl b)
{
    return static_cast<RateControl>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool
hasRateControl(RateControl set,
               RateControl flag)
{
    return (static_cast<unsigned>(set) & static_cast<unsigned>(flag)) != 0;
}

struct EncoderRateControl {
    const char* encoder; // AVCodec::name
    RateControl flags;
};

// The lossy encoders that are not in this table and not mpegvideo encoders get RateControl::eBitrate |
// RateControl::eQRange. This is wrong for snow (mpegvideo rate control without mpv_flags, FFmpeg n9.1
// libavcodec/snowenc.c:236 and :1833), asv1/asv2 (global_quality, asvenc.c:384-388) and a64multi (a64multienc.c:217-220),
// which are not in the Codec menu (FFmpegFile.cpp's allowlist). Only the mpegvideo encoders (ratecontrol.c:994) and
// libxavs (libxavs.c:320) read bit_rate_tolerance; libopenh264 reads bit_rate, qmax and qmin (libopenh264enc.c:142-148).
// svq1, jpeg2000, cinepak and libxvid read Global Quality from the quality of each frame (AVFrame.quality), which
// encode() copies from global_quality, as fftools does (fftools/ffmpeg_enc.c:887).
const EncoderRateControl kEncoderRateControl[] = {
    { "svq1", RateControl::eQScale }, // svq1enc.c:641 (svq1 has its own AVClass, without mpv_flags, svq1enc.c:700)
    { "jpeg2000", RateControl::eQScale }, // j2kenc.c:1487
    { "cinepak", RateControl::eQScale }, // cinepakenc.c:1173
    { "libxvid", RateControl::eBitrate | RateControl::eQScale | RateControl::eQRange }, // libxvid.c:548-551, 607, 799
    { "libtheora", RateControl::eBitrate | RateControl::eQScale }, // libtheoraenc.c:232-241
    { "libxavs", RateControl::eBitrate | RateControl::eBitrateTol | RateControl::eQRange }, // libxavs.c:232, 286-287, 320
    { "vc2", RateControl::eBitrate }, // vc2enc.c:969
    // no rate control field: options of their own, or none
    { "libopenjpeg", RateControl::eNone }, // its rate is compression_level (libopenjpegenc.c:339)
    { "hap", RateControl::eNone }, // its "format", "chunks" and "compressor" options (hapenc.c:334)
    { "jpegls", RateControl::eNone }, // its "pred" option (jpeglsenc.c:460)
    { "cfhd", RateControl::eNone }, // its "quality" option (cfhdenc.c:831)
    { "exr", RateControl::eNone }, // its "compression" option (exrenc.c:526)
};
} // namespace

// The errors in the current settings that make beginEncode() fail, one per line, or an empty string.
string
WriteFFmpegPlugin::getSettingsErrors() const
{
    string errors;
    auto addError = [&errors](const string& error) {
        if (!errors.empty()) {
            errors += '\n';
        }
        errors += error;
    };
    // the same message as in beginEncode()
    {
        StringParam* params[2] = { _codecOptions, _formatOptions };
        const char* labels[2] = { kParamCodecOptionsLabel, kParamFormatOptionsLabel };
        for (int i = 0; i < 2; ++i) {
            string str;
            string error;
            AVDictionaryHolder options;
            params[i]->getValue(str);
            if (!parseOptionString(str, options.ptr(), &error)) {
                addError(string("Syntax error in ") + labels[i] + " \"" + str + "\": " + error);
            }
        }
    }
    const AVOutputFormat* fmt = initFormat(/* reportErrors = */ false);
    if (fmt) {
        AVCodecID codecId = AV_CODEC_ID_NONE;
        const AVCodec* videoCodec = nullptr;
        if (!initCodec(fmt, codecId, videoCodec)) {
            addError("Unable to find codec");
        } else if (!codecCompatible(fmt, codecId)) {
            addError(string("The codec ") + videoCodec->name + " is not supported in container " + fmt->name);
        }
    }
    string canonical, error;
    if (!getStartTimecode(fmt, &canonical, &error)) {
        addError(error);
    }

    return errors;
}

// Show the errors in the current settings in the persistent message, or clear it if there are none.
// A persistent message stays "until explicitly cleared by an effect" (ofxMessage.h), and each new one
// replaces the previous one, so all the current errors are checked and shown together, whatever
// param changed. The persistent message must also be cleared when there is no error, "or render() is
// not called by Nuke after an error" (GenericWriterPlugin::changedParam()). beginEncode() checks the
// same settings again.
void
WriteFFmpegPlugin::updatePersistentMessage()
{
    const string errors = getSettingsErrors();
    if (errors.empty()) {
        const string warnings = getSettingsWarnings();
        if (warnings.empty()) {
            clearPersistentMessage();
        } else {
            setPersistentMessage(Message::eMessageWarning, "", warnings);
        }
    } else {
        setPersistentMessage(Message::eMessageError, "", errors);
    }
}

// The message shown when the encoder can not write alpha (see getPixelFormats()).
static string
alphaDroppedMessage(const AVCodec* videoCodec,
                    AVPixelFormat alphaFormat,
                    AVPixelFormat targetFormat)
{
    const char* alphaName = av_get_pix_fmt_name(alphaFormat);
    const char* targetName = av_get_pix_fmt_name(targetFormat);

    return string("The ") + videoCodec->name + " encoder can not write alpha: it can not be opened with the pixel format "
        + (alphaName ? alphaName : "?") + ", although it lists it. The alpha channel is dropped, and the pixel format "
        + (targetName ? targetName : "?") + " is written.";
}

// The warnings about the file that is written with the pixel format target (one per line, or an empty string): the
// codec of the project was replaced (codecReplacedMessage()), rawvideo has no pixel format with the Pref. params in
// this container (rawvideoFallbackMessage()), or the encoder can not write alpha (alphaDroppedMessage(), if
// alphaFormat, the format with alpha that it lists, is not AV_PIX_FMT_NONE).
string
WriteFFmpegPlugin::encodeWarnings(const AVOutputFormat* fmt,
                                  const AVCodec* videoCodec,
                                  FFmpeg::PixelCodingEnum prefPixelCoding,
                                  int prefBitDepth,
                                  bool prefAlpha,
                                  AVPixelFormat target,
                                  AVPixelFormat alphaFormat) const
{
    string warnings;
    const string replaced = codecReplaced();
    if (!replaced.empty()) {
        warnings += codecReplacedMessage(replaced, fmt, target) + "\n";
    }
    warnings += rawvideoFallbackMessage(fmt, videoCodec, prefPixelCoding, prefBitDepth, prefAlpha, target);
    if (!warnings.empty() && (warnings.back() != '\n')) {
        warnings += "\n";
    }
    if (videoCodec && (alphaFormat != AV_PIX_FMT_NONE)) {
        warnings += alphaDroppedMessage(videoCodec, alphaFormat, target);
    }
    if (!warnings.empty() && (warnings.back() == '\n')) {
        warnings.pop_back();
    }

    return warnings;
}

// The warnings about the current settings, shown in the persistent message if there is no error (see
// encodeWarnings()). They do not depend on the size of the image (beginEncode() knows it).
string
WriteFFmpegPlugin::getSettingsWarnings() const
{
    const AVOutputFormat* fmt = initFormat(/* reportErrors = */ false);
    AVCodecID codecId = AV_CODEC_ID_NONE;
    const AVCodec* videoCodec = nullptr;
    if (!fmt || !initCodec(fmt, codecId, videoCodec) || !videoCodec) {
        return encodeWarnings(nullptr, nullptr, FFmpeg::ePixelCodingNone, 0, false, AV_PIX_FMT_NONE, AV_PIX_FMT_NONE);
    }
    const bool alpha = alphaEnabled();
    if (!alpha && codecReplaced().empty() && (videoCodec->id != AV_CODEC_ID_RAWVIDEO)) {
        // (no warning)
        return string();
    }
    FFmpeg::PixelCodingEnum pixelCoding;
    int bitdepth;
    getPixelPreferences(&pixelCoding, &bitdepth);
    AVPixelFormat alphaFormat = AV_PIX_FMT_NONE;
    AVPixelFormat targetPixelFormat = AV_PIX_FMT_NONE;
    AVPixelFormat rgbBufferPixelFormat = AV_PIX_FMT_NONE;
    bool alphaDropped = false;
    getPixelFormats(fmt, videoCodec, pixelCoding, bitdepth, alpha, rgbBufferPixelFormat, targetPixelFormat, &alphaDropped);
    if (alphaDropped) {
        getPixelFormatsFromList(fmt, videoCodec, pixelCoding, bitdepth, true, rgbBufferPixelFormat, alphaFormat);
    }

    return encodeWarnings(fmt, videoCodec, pixelCoding, bitdepth, alpha, targetPixelFormat, alphaFormat);
}

// The output format. If the Format is "Default", it is guessed from filename, which is the file being
// rendered (beginEncode()), or by default the value of the filename param (e.g. when a param changes).
const AVOutputFormat*
WriteFFmpegPlugin::initFormat(bool reportErrors,
                              const string* filename) const
{
    assert(_format);
    int format = _format->getValue();
    const AVOutputFormat* fmt = nullptr;

    if (!format) { // first item is "Default"
        string fileParam;
        if (!filename) {
            _fileParam->getValue(fileParam);
            filename = &fileParam;
        }
        const char* file = filename->c_str();
        const bool fileSpecified = file && strlen(file);
        // If a file name has not yet been set, assume mov format by default
        fmt = av_guess_format(fileSpecified ? nullptr : "mov", file, nullptr);
        if (!fmt && reportErrors) {
            return nullptr;
        }
    } else {
        const vector<string>& formatsShortNames = FFmpegSingleton::Instance().getFormatsShortNames();
        assert(format < (int)formatsShortNames.size());

        fmt = av_guess_format(formatsShortNames[format].c_str(), nullptr, nullptr);
        if (!fmt && reportErrors) {
            return nullptr;
        }
    }
    // printf("avOutputFormat(%d): %s\n", format, fmt->name);

    return fmt;
}

bool
WriteFFmpegPlugin::initCodec(const AVOutputFormat* fmt,
                             AVCodecID& outCodecId,
                             const AVCodec*& outVideoCodec) const
{
    if (!fmt) {
        return false;
    }
    outCodecId = fmt->video_codec;
    const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();

    assert(_codec);
    int codec = _codec->getValue();
    assert(codec >= 0 && codec < (int)codecsShortNames.size());

    const AVCodec* userCodec = avcodec_find_encoder_by_name(getCodecFromShortName(codecsShortNames[codec]));
    if (userCodec) {
        outCodecId = userCodec->id;
    }
    if (outCodecId == AV_CODEC_ID_PRORES) {
        // use prores_ks instead of prores
        outVideoCodec = userCodec;

        return true;
    }
    if (userCodec) {
        outVideoCodec = userCodec;
    } else {
        outVideoCodec = avcodec_find_encoder(outCodecId);
    }
    if (!outVideoCodec) {
        return false;
    }

    return true;
}

// The Pref. Pixel Coding and Bit Depth params.
void
WriteFFmpegPlugin::getPixelPreferences(FFmpeg::PixelCodingEnum* pixelCoding,
                                       int* bitdepth) const
{
    switch ((PrefPixelCodingEnum)_prefPixelCoding->getValue()) {
    case ePrefPixelCodingYUV420:
        *pixelCoding = FFmpeg::ePixelCodingYUV420;
        break;
    case ePrefPixelCodingYUV422:
    default:
        *pixelCoding = FFmpeg::ePixelCodingYUV422;
        break;
    case ePrefPixelCodingYUV444:
        *pixelCoding = FFmpeg::ePixelCodingYUV444;
        break;
    case ePrefPixelCodingRGB:
        *pixelCoding = FFmpeg::ePixelCodingRGB;
        break;
    case ePrefPixelCodingXYZ:
        *pixelCoding = FFmpeg::ePixelCodingXYZ;
        break;
    }
    switch ((PrefBitDepthEnum)_prefBitDepth->getValue()) {
    case ePrefBitDepth8:
    default:
        *bitdepth = 8;
        break;
    case ePrefBitDepth10:
        *bitdepth = 10;
        break;
    case ePrefBitDepth12:
        *bitdepth = 12;
        break;
    case ePrefBitDepth16:
        *bitdepth = 16;
        break;
    }
}

// Whether the encoder can be opened with this pixel format (on a small image, with the default options).
// Encoders may list pixel formats that they can not encode, e.g. libx265 lists yuva420p if FFmpeg was built
// with a libx265 that has alpha support, but fails in avcodec_open2() with "Loaded libx265 does not support
// alpha layer encoding" if the libx265 loaded at run time does not have it. The result is cached.
static bool
encoderOpensPixelFormat(const AVCodec* codec,
                        AVPixelFormat pixelFormat)
{
    static Mutex mutex;
    static std::map<std::pair<const AVCodec*, int>, bool> cache;
    AutoMutex lock(mutex);
    const std::pair<const AVCodec*, int> key(codec, (int)pixelFormat);
    std::map<std::pair<const AVCodec*, int>, bool>::const_iterator it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    bool opens = false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (ctx) {
        ctx->width = 64;
        ctx->height = 64;
        ctx->pix_fmt = pixelFormat;
        ctx->time_base = av_make_q(1, 25);
        ctx->framerate = av_make_q(25, 1);
        ctx->bits_per_raw_sample = FFmpeg::pixelFormatBitDepth(pixelFormat);
        opens = avcodec_open2(ctx, codec, nullptr) >= 0;
        avcodec_free_context(&ctx);
    }
    cache[key] = opens;

    return opens;
}

// The pixel format to encode with, and the RGB pixel format to convert from.
// If alpha is wanted, but the encoder can not be opened with the selected format with alpha, whereas it can
// with the format selected without alpha, the latter is selected, and *outAlphaDropped is set.
void
WriteFFmpegPlugin::getPixelFormats(const AVOutputFormat* fmt,
                                   const AVCodec* videoCodec,
                                   FFmpeg::PixelCodingEnum prefPixelCoding,
                                   int prefBitDepth,
                                   bool prefAlpha,
                                   AVPixelFormat& outRgbBufferPixelFormat,
                                   AVPixelFormat& outTargetPixelFormat,
                                   bool* outAlphaDropped) const
{
    if (outAlphaDropped) {
        *outAlphaDropped = false;
    }
    getPixelFormatsFromList(fmt, videoCodec, prefPixelCoding, prefBitDepth, prefAlpha, outRgbBufferPixelFormat, outTargetPixelFormat);
    if (!videoCodec || !prefAlpha || !FFmpeg::pixelFormatAlpha(outTargetPixelFormat) || (videoCodec->id == AV_CODEC_ID_PRORES)) {
        return;
    }
    AVPixelFormat rgbBufferPixelFormat = AV_PIX_FMT_NONE;
    AVPixelFormat targetPixelFormat = AV_PIX_FMT_NONE;
    getPixelFormatsFromList(fmt, videoCodec, prefPixelCoding, prefBitDepth, false, rgbBufferPixelFormat, targetPixelFormat);
    if (FFmpeg::pixelFormatAlpha(targetPixelFormat) || encoderOpensPixelFormat(videoCodec, outTargetPixelFormat)
        || !encoderOpensPixelFormat(videoCodec, targetPixelFormat)) {
        // keep the format with alpha if it opens, or if the one without alpha does not open either (the
        // error is then reported when rendering)
        return;
    }
    outRgbBufferPixelFormat = rgbBufferPixelFormat;
    outTargetPixelFormat = targetPixelFormat;
    if (outAlphaDropped) {
        *outAlphaDropped = true;
    }
}

// The pixel format selected from the formats that the encoder lists (or the ones it is known to use), in the container
// fmt (which gives the pixel formats of rawvideo, see encoderPixelFormats()).
void
WriteFFmpegPlugin::getPixelFormatsFromList(const AVOutputFormat* fmt,
                                           const AVCodec* videoCodec,
                                           FFmpeg::PixelCodingEnum prefPixelCoding,
                                           int prefBitDepth,
                                           bool prefAlpha,
                                           AVPixelFormat& outRgbBufferPixelFormat,
                                           AVPixelFormat& outTargetPixelFormat) const
{
    assert(videoCodec);
    if (!videoCodec) {
        outRgbBufferPixelFormat = AV_PIX_FMT_NONE;
        outTargetPixelFormat = AV_PIX_FMT_NONE;

        return;
    }
    if (AV_CODEC_ID_PRORES == videoCodec->id) {
        int index = _codec->getValue();
        const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
        assert(index < (int)codecsShortNames.size());
        int profile = getProfileFromShortName(codecsShortNames[index]);
        if (profile == kProresProfile4444 || profile == kProresProfile4444XQ) {
            // ProRes 4444 and 4444 XQ are Y'CbCr 4:4:4, with an optional alpha channel
            if (prefAlpha) {
                outTargetPixelFormat = AV_PIX_FMT_YUVA444P10;
            } else {
                outTargetPixelFormat = AV_PIX_FMT_YUV444P10;
            }
        } else {
            // in prores_ks, all other profiles use AV_PIX_FMT_YUV422P10
            outTargetPixelFormat = AV_PIX_FMT_YUV422P10;
        }
    } else if (AV_CODEC_ID_DNXHD == videoCodec->id) {
        DNxHDCodecProfileEnum dnxhdCodecProfile = (DNxHDCodecProfileEnum)_dnxhdCodecProfile->getValue();
        // FFmpeg's dnxhd encoder takes yuv422p (8 bit), and yuv422p10, yuv444p10 and gbrp10 (10 bit), and
        // dnxhd_encode_init() checks the pixel format of each DNxHR profile (libavcodec/dnxhdenc.c:358-390 and :1355
        // in n9.1, the same one line further in n8.0)
        if (dnxhdCodecProfile == eDNxHDCodecProfileHR444) {
            // yuv444p10 or gbrp10 (dnxhdenc.c:369-376)
            outTargetPixelFormat = AV_PIX_FMT_GBRP10;
        } else if (dnxhdCodecProfile == eDNxHDCodecProfileHRHQX) {
            // yuv422p10 only (dnxhdenc.c:378-382)
            outTargetPixelFormat = AV_PIX_FMT_YUV422P10;
        } else if ((dnxhdCodecProfile == eDNxHDCodecProfileHRHQ) || (dnxhdCodecProfile == eDNxHDCodecProfileHRSQ) || (dnxhdCodecProfile == eDNxHDCodecProfileHRLB)) {
            // yuv422p only (dnxhdenc.c:384-390)
            outTargetPixelFormat = AV_PIX_FMT_YUV422P;
        } else if ((dnxhdCodecProfile == eDNxHDCodecProfile220x) || (dnxhdCodecProfile == eDNxHDCodecProfile440x)) {
            // DNxHD: the bit depth of the pixel format selects the CID (ff_dnxhd_find_cid()): 10 bit for CIDs 1235
            // and 1250
            outTargetPixelFormat = AV_PIX_FMT_YUV422P10;
        } else {
            // 8 bit for CIDs 1237, 1238, 1251, 1252 and 1253
            outTargetPixelFormat = AV_PIX_FMT_YUV422P;
        }
    } else if (encoderPixelFormats(videoCodec, fmt) != nullptr) {
        // This is the most frequent path, where we can guess best pix format using ffmpeg.
        // find highest bit depth pix fmt.
        const AVPixelFormat* const pixFormats = encoderPixelFormats(videoCodec, fmt);
        const AVPixelFormat* currPixFormat = pixFormats;

        int prefBPP = FFmpeg::pixelFormatBPPFromSpec(prefPixelCoding, prefBitDepth, prefAlpha);

        // A format with alpha is never selected if alpha is not wanted (unless the codec only has formats with
        // alpha): the encoder may not support it (e.g. libx265 compiled without alpha support, whose only 10-bit
        // or 4:2:2-compatible format with BPP >= 20 is yuva420p if it is 8-bit only), and it is useless.
        bool hasFormatWithoutAlpha = false;
        for (const AVPixelFormat* f = currPixFormat; *f != -1; ++f) {
            if (!FFmpeg::pixelFormatAlpha(*f)) {
                hasFormatWithoutAlpha = true;
                break;
            }
        }
        const bool excludeAlpha = !prefAlpha && hasFormatWithoutAlpha;

        // First, try to find the format with the smallest BPP that fits into the preferences
        bool prefFound = false;
        int prefFoundBPP = 1024; // large number
        int prefFoundBitDepth = 1024; // large number
        AVPixelFormat prefFoundFormat = AV_PIX_FMT_NONE;

        // Then, if the bit depth is not available (e.g. a libx265 compiled for 8-bit only), get the format that fits
        // the pixel coding and alpha preferences with the highest bit depth, and the smallest BPP.
        bool depthFound = false;
        int depthFoundBPP = 1024; // large number
        int depthFoundBitDepth = 0;
        AVPixelFormat depthFoundFormat = AV_PIX_FMT_NONE;

        // Second, If no format is found that fits the prefs, get the format that has a BPP equal or a bit higher.
        bool higherFound = false;
        int higherFoundBPP = 1024; // large number
        int higherFoundBitDepth = 1024; // large number
        AVPixelFormat higherFoundFormat = AV_PIX_FMT_NONE;

        // Third, if no such format is found, get the format that has the highest BPP
        int highestBPP = 0;
        int highestBPPBitDepth = 0;
        AVPixelFormat highestBPPFormat = AV_PIX_FMT_NONE;

        while (*currPixFormat != -1) {
            FFmpeg::PixelCodingEnum currCoding = FFmpeg::pixelFormatCoding(*currPixFormat);
            int currBitDepth = FFmpeg::pixelFormatBitDepth(*currPixFormat);
            int currBPP = FFmpeg::pixelFormatBPP(*currPixFormat);
            bool currAlpha = FFmpeg::pixelFormatAlpha(*currPixFormat);
            if (excludeAlpha && currAlpha) {
                currPixFormat++;
                continue;
            }

            // First, try to find the format with the smallest BPP that fits into the preferences
            if ((int)currCoding >= (int)prefPixelCoding && currBitDepth >= prefBitDepth && currAlpha == prefAlpha && currBPP < prefFoundBPP) {
                prefFound = true;
                prefFoundBPP = currBPP;
                prefFoundBitDepth = currBitDepth;
                prefFoundFormat = *currPixFormat;
            }

            // Then, the format that fits the pixel coding and alpha preferences, with the highest bit depth
            if ((int)currCoding >= (int)prefPixelCoding && currAlpha == prefAlpha
                && (currBitDepth > depthFoundBitDepth || (currBitDepth == depthFoundBitDepth && currBPP < depthFoundBPP))) {
                depthFound = true;
                depthFoundBPP = currBPP;
                depthFoundBitDepth = currBitDepth;
                depthFoundFormat = *currPixFormat;
            }

            // Second, If no format is found that fits the prefs, get the format that has a BPP equal or a bit higher.
            if (currBPP >= prefBPP && currBPP < higherFoundBPP) {
                higherFound = true;
                higherFoundBPP = currBPP;
                higherFoundBitDepth = currBitDepth;
                higherFoundFormat = *currPixFormat;
            }

            // Third, if no such format is found, get the format that has the highest BPP
            if (currBPP > highestBPP) {
                highestBPP = currBPP;
                highestBPPBitDepth = currBitDepth;
                highestBPPFormat = *currPixFormat;
            }
            currPixFormat++;
        }

        // get the pixel depth for the selected format
        int selectedBitDepth;
        int selectedBPP;
        AVPixelFormat selectedFormat;

        if (prefFound) {
            selectedBitDepth = prefFoundBitDepth;
            selectedBPP = prefFoundBPP;
            selectedFormat = prefFoundFormat;
        } else if (depthFound) {
            selectedBitDepth = depthFoundBitDepth;
            selectedBPP = depthFoundBPP;
            selectedFormat = depthFoundFormat;
        } else if (higherFound) {
            selectedBitDepth = higherFoundBitDepth;
            selectedBPP = higherFoundBPP;
            selectedFormat = higherFoundFormat;
        } else {
            selectedBitDepth = highestBPPBitDepth;
            selectedBPP = highestBPP;
            selectedFormat = highestBPPFormat;
        }

        // figure out rgbBufferPixelFormat from this.
        outRgbBufferPixelFormat = GetRGBPixelFormatFromBitDepth(selectedBitDepth, prefAlpha);

        // call best_pix_fmt using the full list.
        const int hasAlphaInt = prefAlpha ? 1 : 0;

        if (prefFound || depthFound) {
            outTargetPixelFormat = selectedFormat;
        } else {
            // gather the formats that have the target BPP (avcodec_find_best_pix_fmt_of_list doesn't do the best job: it prefers yuv422p over yuv422p10)
            vector<AVPixelFormat> bestFormats;
            currPixFormat = pixFormats;
            while (*currPixFormat != -1) {
                int currBPP = FFmpeg::pixelFormatBPP(*currPixFormat);
                if (currBPP == selectedBPP && !(excludeAlpha && FFmpeg::pixelFormatAlpha(*currPixFormat))) {
                    bestFormats.push_back(*currPixFormat);
                }
                currPixFormat++;
            }
            bestFormats.push_back(AV_PIX_FMT_NONE);

            // FFmpeg bug https://trac.ffmpeg.org/ticket/5223 : do not pass &loss as the last parameter:
            // there is a bug in avcodec_find_best_pix_fmt_of_list: the loss mask is not the same for all pixel formats
            // (the loss of first format is used a a mask for the second, etc) so that if the first pixel format
            // loses resolution, resolution loss is not considered for other formats.
            // If we pass NULL, the considered loss is ~0 for all formats
            outTargetPixelFormat = avcodec_find_best_pix_fmt_of_list(/*codecPixelFormats(videoCodec)*/ &bestFormats[0], outRgbBufferPixelFormat, hasAlphaInt, nullptr);
        }

#ifdef DEBUG
        int loss = av_get_pix_fmt_loss(outTargetPixelFormat, outRgbBufferPixelFormat, hasAlphaInt);
        // loss is a combination of
        // FF_LOSS_RESOLUTION
        // FF_LOSS_DEPTH
        // FF_LOSS_COLORSPACE
        // FF_LOSS_ALPHA
        // FF_LOSS_COLORQUANT
        // FF_LOSS_CHROMA
        std::printf("WriteFFmpeg: pixel format selected: %s->%s\n", av_get_pix_fmt_name(outRgbBufferPixelFormat),
                    av_get_pix_fmt_name(outTargetPixelFormat));
        if (loss & FF_LOSS_RESOLUTION) {
            std::printf("WriteFFmpeg: pixel format loses RESOLUTION\n");
        }
        if (loss & FF_LOSS_DEPTH) {
            std::printf("WriteFFmpeg: pixel format loses DEPTH\n");
        }
        if (loss & FF_LOSS_COLORSPACE) {
            std::printf("WriteFFmpeg: pixel format loses COLORSPACE\n");
        }
        if (loss & FF_LOSS_ALPHA) {
            std::printf("WriteFFmpeg: pixel format loses ALPHA\n");
        }
        if (loss & FF_LOSS_COLORQUANT) {
            std::printf("WriteFFmpeg: pixel format loses COLORQUANT\n");
        }
        if (loss & FF_LOSS_CHROMA) {
            std::printf("WriteFFmpeg: pixel format loses CHROMA\n");
        }
#endif

        // Alpha is wanted, and the format selected above has none, although the encoder has formats with alpha:
        // select the best one of those that fit the pixel coding and bit depth preferences, or else of all of them
        // (avcodec_find_best_pix_fmt_of_list() above may prefer a format without alpha, e.g. rgb24 for qtrle, which
        // has rgb24, rgb555be, argb and gray).
        if (hasAlphaInt && !FFmpeg::pixelFormatAlpha(outTargetPixelFormat)) {
            vector<AVPixelFormat> alphaFormats;
            vector<AVPixelFormat> prefAlphaFormats;
            for (currPixFormat = pixFormats; *currPixFormat != AV_PIX_FMT_NONE; ++currPixFormat) {
                if (FFmpeg::pixelFormatAlpha(*currPixFormat)) {
                    alphaFormats.push_back(*currPixFormat);
                    if (((int)FFmpeg::pixelFormatCoding(*currPixFormat) >= (int)prefPixelCoding) && (FFmpeg::pixelFormatBitDepth(*currPixFormat) >= prefBitDepth)) {
                        prefAlphaFormats.push_back(*currPixFormat);
                    }
                }
            }
            vector<AVPixelFormat>& candidates = prefAlphaFormats.empty() ? alphaFormats : prefAlphaFormats;
            if (!candidates.empty()) {
                candidates.push_back(AV_PIX_FMT_NONE);
                outTargetPixelFormat = avcodec_find_best_pix_fmt_of_list(candidates.data(), outRgbBufferPixelFormat, hasAlphaInt, nullptr);
            }
        }

        // Unlike the other cases, we're now done figuring out all aspects, so return.
        // return; // don't return, avcodec_find_best_pix_fmt_of_list may have returned a lower bitdepth than infoBitDepth
    } else {
        // Lowest common denominator defaults.
        outTargetPixelFormat = AV_PIX_FMT_YUV420P;
    }

    // update outRGBPixelFormat from the selected format (do not need alpha if format does not have it)
    outRgbBufferPixelFormat = GetRGBPixelFormatFromBitDepth(FFmpeg::pixelFormatBitDepth(outTargetPixelFormat),
                                                            FFmpeg::pixelFormatAlpha(outTargetPixelFormat));
} // WriteFFmpegPlugin::getPixelFormatsFromList

/*static*/
AVPixelFormat
WriteFFmpegPlugin::GetRGBPixelFormatFromBitDepth(const int bitDepth,
                                                 const bool hasAlpha)
{
    if (bitDepth == 0) {
        return AV_PIX_FMT_NONE;
    }
    AVPixelFormat pixelFormat;
    if (hasAlpha) {
        pixelFormat = (bitDepth > 8) ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_RGBA;
    } else {
        pixelFormat = (bitDepth > 8) ? AV_PIX_FMT_RGB48 : AV_PIX_FMT_RGB24;
    }

    return pixelFormat;
}

/*static*/
WriteFFmpegPlugin::CodecParams
WriteFFmpegPlugin::GetCodecSupportedParams(const AVCodec* codec)
{
    assert(codec);
    CodecParams p;
    const AVCodecDescriptor* codecDesc = codec ? avcodec_descriptor_get(codec->id) : nullptr;
    if (!codecDesc) {
        return p;
    }

    // ProRes and DNxHD are profile-based: they do not use the rate control params (https://trac.ffmpeg.org/wiki/Encode/VFX)
    const bool profileBased = (codec->id == AV_CODEC_ID_PRORES) || (codec->id == AV_CODEC_ID_DNXHD);
    const bool lossy = !profileBased && (codecDesc->props & AV_CODEC_PROP_LOSSY);
    const string codecShortName = codec->name;

    p.interGOP = !(codecDesc->props & AV_CODEC_PROP_INTRA_ONLY);
    // The png, vc2 (Dirac) and cfhd encoders only write intra frames (they do not read gop_size), but their codecs
    // are not AV_CODEC_PROP_INTRA_ONLY (their decoders read inter frames), and FFmpeg has no encoder property for it.
    if ((codec->id == AV_CODEC_ID_PNG) || (codec->id == AV_CODEC_ID_DIRAC) || (codec->id == AV_CODEC_ID_CFHD)) {
        p.interGOP = false;
    }
    // B-frames: the codec reorders frames (AV_CODEC_PROP_REORDER), and the encoder can delay its output
    // (AV_CODEC_CAP_DELAY), which is what the mpegvideo encoders check (FFmpeg n9.1 libavcodec/mpegvideo_enc.c:605:
    // "B-frames not supported by codec"). This is necessary, not sufficient: libkvazaar and libvvenc have both and
    // ignore max_b_frames, and the AV1 hardware encoders write B-frames without AV_CODEC_PROP_REORDER (none of them
    // is in the Codec menu). libxvid writes B-frames (libxvid.c:675) without AV_CODEC_CAP_DELAY (:912), so the frames
    // that it holds back are never flushed, and lost at the end of the movie: it gets no B-frames.
    p.interB = (codecDesc->props & AV_CODEC_PROP_REORDER) && (codec->capabilities & AV_CODEC_CAP_DELAY);

    if (!lossy) {
        return p;
    }
    // the encoders that have a "crf" option, e.g. libx264.c:1544, libx265.c:1047, libvpxenc.c:2011, libaomenc.c:1599
    p.crf = (encoderOption(codec, "crf") != nullptr);
    // CRF 0 may be lossless if the encoder has a "lossless" option (libvpxenc.c:2049, which configureVideoStream() sets),
    // or if the codec has a lossless mode (e.g. H.264; HEVC and AV1 since FFmpeg 9); not with libvpx (VP8) or libxavs,
    // where it may fail (openCodec() then says it)
    p.crfLossless = p.crf && (encoderOption(codec, "lossless") || (codecDesc->props & AV_CODEC_PROP_LOSSLESS));
    // the encoders that have a "preset" string option take the names of the x264 and x265 presets (libx264.c:1535,
    // libx265.c:1050), which the Encoding Speed sets; the other "preset" options are integers, with other names
    // (e.g. libsvtav1.c:793, nvenc_h264.c:29)
    const AVOption* preset = encoderOption(codec, "preset");
    p.x26xSpeed = preset && (preset->type == AV_OPT_TYPE_STRING);

    RateControl rateControl = RateControl::eBitrate | RateControl::eQRange;
    if (encoderOption(codec, "mpv_flags")) {
        // An mpegvideo encoder (its options include FF_MPV_COMMON_OPTS, FFmpeg n9.1 libavcodec/mpegvideoenc.h:339,
        // e.g. mpeg4, mpeg1video, mpeg2video, flv, h263p, msmpeg4, msmpeg4v2 and mjpeg): Global Quality
        // (AV_CODEC_FLAG_QSCALE, mpegvideo_enc.c:622) and qmin and qmax (mpegvideo_enc.c:219), rather than bit_rate,
        // which is meant for two-pass encoding, and gives irreproducible low-quality results
        // (https://trac.ffmpeg.org/wiki/Encode/MPEG-4)
        rateControl = RateControl::eQScale | RateControl::eQRange;
    } else {
        for (const EncoderRateControl& rc : kEncoderRateControl) {
            if (codecShortName == rc.encoder) {
                rateControl = rc.flags;
                break;
            }
        }
    }
    p.bitrate = hasRateControl(rateControl, RateControl::eBitrate);
    p.bitrateTol = hasRateControl(rateControl, RateControl::eBitrateTol);
    p.qscale = hasRateControl(rateControl, RateControl::eQScale);
    // qmin and qmax are confusing next to the Output Quality (CRF), although e.g. libx264 reads them
    p.qrange = !p.crf && hasRateControl(rateControl, RateControl::eQRange);

    return p;
} // WriteFFmpegPlugin::GetCodecSupportedParams

#if OFX_FFMPEG_AUDIO
////////////////////////////////////////////////////////////////////////////////
// configureAudioStream
// Set audio parameters of the audio stream.
//
// @param avCodec A pointer reference to an AVCodec to receive the AVCodec if
//                the codec can be located and is on the codec white list.
// @param avStream A reference to an AVStream of a audio stream.
//
void
WriteFFmpegPlugin::configureAudioStream(AVCodec* avCodec,
                                        AVStream* avStream)
{
    assert(avCodec && avStream);
    if (!avCodec || !avStream) {
        return;
    }
    AVCodecContext* avCodecContext = avStream->codec;
    assert(avCodecContext);
    if (!avCodecContext) {
        return;
    }
    avcodec_get_context_defaults3(avCodecContext, avCodec);
    avCodecContext->sample_fmt = audioReader_->getSampleFormat();
    // avCodecContext->bit_rate    = 64000; // Calculate...
    avCodecContext->sample_rate = audioReader_->getSampleRate();
    avCodecContext->channels = audioReader_->getNumberOfChannels();
}

#endif

////////////////////////////////////////////////////////////////////////////////
// configureVideoStream
// Set video parameters of the video stream.
//
// @param avCodec A pointer reference to an AVCodec to receive the AVCodec if
//                the codec can be located and is on the codec white list.
// @param avStream A reference to an AVStream of a audio stream.
//
void
WriteFFmpegPlugin::configureVideoStream(const AVCodec* avCodec,
                                        MyAVStream* myAVStream)
{
    assert(avCodec && myAVStream && myAVStream->stream && _formatContext);
    if (!avCodec || !myAVStream || !myAVStream->stream || !_formatContext) {
        return;
    }

    AVCodecContext* avCodecContext = myAVStream->codecContext;
    AVStream* avStream = myAVStream->stream;
    assert(avCodecContext);
    if (!avCodecContext) {
        return;
    }
    // avCodecContext->strict_std_compliance = FF_COMPLIANCE_STRICT;

    // Only update the relevant context variables where the user is able to set them.
    // This deals with cases where values are left on an old value when knob disabled.
    const CodecParams p = avCodec ? GetCodecSupportedParams(avCodec) : CodecParams();

    assert(_crf && _x26xSpeed && _qscale && _bitrate && _bitrateTolerance && _quality);
    bool setqscale = p.qscale;
    bool setbitrate = p.bitrate;
    if (p.crf) {
        /* Mapping from easily-understandable descriptions to CRF values.
         * Assumes we output 8-bit video. Needs to be remapped if 10-bit
         * is output.
         * We use a slightly wider than "subjectively sane range" according
         * to https://trac.ffmpeg.org/wiki/Encode/H.264#a1.ChooseaCRFvalue
         */
        // default is CRF=23 (see http://git.videolan.org/?p=x264.git;a=blob;f=common/common.c;h=14d4670e0e095f161e7a690b40b035eca8c01fc5;hb=HEAD#l105)

        int crf = -1;
        CRFEnum e = (CRFEnum)_crf->getValue();
        switch (e) {
        case eCRFNone:
            crf = -1;
            break;

        case eCRFLossless:
            crf = 0;
            break;

        case eCRFPercLossless:
            crf = 17;
            break;

        case eCRFHigh:
            crf = 20;
            break;

        case eCRFMedium:
            crf = 23;
            break;

        case eCRFLow:
            crf = 26;
            break;

        case eCRFVeryLow:
            crf = 29;
            break;

        case eCRFLowest:
            crf = 32;
            break;
        }
        if (crf >= 0) {
            setqscale = setbitrate = false;
            const int error = av_opt_set_int(avCodecContext->priv_data, "crf", crf, 0);
            if (error < 0) {
                // e.g. out of the range of the option (libvpx and libaom use a 0-63 scale)
                char szError[AV_ERROR_MAX_STRING_SIZE] = { 0 };
                av_strerror(error, szError, sizeof(szError));
                av_log(avCodecContext, AV_LOG_WARNING, "Output Quality: cannot set crf=%d: %s\n", crf, szError);
            }
            if ((crf == 0) && encoderOption(avCodec, "lossless")) {
                // CRF 0 is not lossless in the encoders that have a "lossless" option, e.g. libvpx-vp9 (FFmpeg n9.1
                // libavcodec/libvpxenc.c:2049, https://trac.ffmpeg.org/wiki/Encode/VP9)
                av_opt_set_int(avCodecContext->priv_data, "lossless", 1, 0);
            }
            if (p.x26xSpeed) {
                X26xSpeedEnum e = (X26xSpeedEnum)_x26xSpeed->getValue();
                const char* preset = nullptr;
                switch (e) {
                case eX26xSpeedUltrafast:
                    preset = "ultrafast";
                    break;

                case eX26xSpeedVeryfast:
                    preset = "veryfast";
                    break;

                case eX26xSpeedFaster:
                    preset = "faster";
                    break;

                case eX26xSpeedFast:
                    preset = "fast";
                    break;

                case eX26xSpeedMedium:
                    preset = "medium";
                    break;

                case eX26xSpeedSlow:
                    preset = "slow";
                    break;

                case eX26xSpeedSlower:
                    preset = "slower";
                    break;

                case eX26xSpeedVeryslow:
                    preset = "veryslow";
                    break;
                }
                if (preset != nullptr) {
                    av_opt_set(avCodecContext->priv_data, "preset", preset, 0);
                }
            }
        }
    }
    if (setqscale) {
        double qscale = _qscale->getValue();
        if (qscale >= 0.) {
            setbitrate = false;
            avCodecContext->flags |= AV_CODEC_FLAG_QSCALE;
            avCodecContext->global_quality = qscale * FF_QP2LAMBDA;
        }
    }
    if (setbitrate) {
        double bitrate = _bitrate->getValue();
        // bit_rate and rc_max_rate are int64_t: the bitrate in bit/s was converted to int, which overflowed (undefined
        // behaviour) above 2147 Mbit/s. It is now clamped to [0, INT_MAX kbit/s] (bitrateToInt64()).
        const int64_t bitRate = bitrateToInt64(bitrate);
        if (bitrate >= 0) {
            avCodecContext->bit_rate = bitRate;
        }
        if (p.bitrateTol) {
            double bitrateTolerance = _bitrateTolerance->getValue();
            if (bitrateTolerance == 0.) {
                avCodecContext->rc_max_rate = bitRate;
                avCodecContext->rc_max_available_vbv_use = 2.0f;
                avCodecContext->bit_rate_tolerance = 0;
            } else {
                double fps = _fps->getValue();
                const double toleranceMin = bitrateToleranceMin(bitrate, fps, kParamBitrateToleranceMax);
                bitrateTolerance = (std::max)(toleranceMin, bitrateTolerance);
                // the host may give a value out of the declared range of the param (or NaN): tol * 1000000 must fit
                // in an int, and be at least 0
                if (OFX::IsNaN(bitrateTolerance) || (bitrateTolerance >= (double)kParamBitrateToleranceMax)) {
                    bitrateTolerance = kParamBitrateToleranceMax;
                } else if (bitrateTolerance <= 0.) {
                    bitrateTolerance = 0.;
                }

                avCodecContext->bit_rate_tolerance = (int)(bitrateTolerance * 1000000);
            }
        } else {
            // avoid warning from mpegvideo_enc.c
            // The time base of the codec context is not set yet here (it is set below, from the frame rate), so it is
            // still the default {0, 1}, and this is always 0. The guard only prevents an infinite tolerance (or
            // 0 * inf = NaN, converted to int) if the time base is ever set before, for a frame rate of 0 (1/0).
            const AVRational timeBase = avCodecContext->time_base;
            avCodecContext->bit_rate_tolerance = ((timeBase.num > 0) && (timeBase.den > 0)) ? avCodecContext->bit_rate * av_q2d(timeBase) : 0;
        }
    }
    if (p.qrange) {
        int qMin, qMax;
        _quality->getValue(qMin, qMax);

        if (qMin >= 0) {
            avCodecContext->qmin = qMin;
            av_opt_set_int(avCodecContext->priv_data, "qmin", qMin, 0);
        }
        if (qMax >= 0) {
            avCodecContext->qmax = qMax;
            av_opt_set_int(avCodecContext->priv_data, "qmax", qMax, 0);
        }
    }

    avCodecContext->width = (_rodPixel.x2 - _rodPixel.x1);
    avCodecContext->height = (_rodPixel.y2 - _rodPixel.y1);

    // the primaries and transfer function of the OCIO output colorspace (if it is one that getColorInfo() knows),
    // which setColorTags() completes once the pixel format is known
    getColorInfo(&avCodecContext->color_primaries, &avCodecContext->color_trc);

    av_dict_set(&_formatContext->metadata, kMetaKeyApplication, kPluginIdentifier, 0);

    av_dict_set(&_formatContext->metadata, kMetaKeyApplicationVersion, STR(kPluginVersionMajor) "." STR(kPluginVersionMinor), 0);

#ifdef OFX_IO_USING_OCIO
    // The OCIO output colorspace, which ReadFFmpeg uses as its File Colorspace if its OCIO config has it
    // (FFmpegFile::getColorspaceMetadata()). The muxers that store any metadata key keep it (e.g. Matroska and
    // NUT); MOV and MP4 only store the keys they know (libavformat/movenc.c mov_write_udta_tag()), unless the
    // movflags option use_metadata_tags is set.
    {
        string outputColorspace;
        _ocio->getOutputColorspace(outputColorspace);
        if (_ocio->getConfig() && !outputColorspace.empty() && _ocio->hasColorspace(outputColorspace.c_str())) {
            av_dict_set(&_formatContext->metadata, kMetaKeyColorspace, outputColorspace.c_str(), 0);
        }
    }
#endif

    av_dict_set(&_formatContext->metadata, kMetaKeyWriter, kMetaValueWriter64, 0);

    int dnxhdCodecProfile_i = 0;
    _dnxhdCodecProfile->getValue(dnxhdCodecProfile_i);

    avStream->sample_aspect_ratio.num = 1;
    avStream->sample_aspect_ratio.den = 1;

    // From the Apple QuickTime movie guidelines. Set the
    // appropriate pixel aspect ratio for the movie.
    // Scale by 100 and convert to int for a reliable comparison, e.g.
    // 0.9100000000 != 0.9100002344. This is done as the pixel aspect
    // ratio is provided as a double and the author cannot find a
    // rational representation (num/den) of par.
    // the PAR comes from the host: it is clamped before the conversion to int (a PAR out of [0, 100] is not one of
    // those below)
    const double hostPar = _inputClip->getPixelAspectRatio();
    int32_t par = (!OFX::IsNaN(hostPar) && (0. <= hostPar) && (hostPar <= 100.)) ? (int32_t)(hostPar * 100.0) : 0;
    if (200 == par) {
        avStream->sample_aspect_ratio.num = 2;
        avStream->sample_aspect_ratio.den = 1;
    } else if (150 == par) {
        avStream->sample_aspect_ratio.num = 3;
        avStream->sample_aspect_ratio.den = 2;
    } else if (146 == par) {
        // PAL 16:9
        avStream->sample_aspect_ratio.num = 118;
        avStream->sample_aspect_ratio.den = 81;
    } else if (133 == par) {
        avStream->sample_aspect_ratio.num = 4;
        avStream->sample_aspect_ratio.den = 3;
    } else if (121 == par) {
        // NTSC 16:9
        avStream->sample_aspect_ratio.num = 40;
        avStream->sample_aspect_ratio.den = 33;
    } else if (109 == par) {
        // PAL 4:3
        avStream->sample_aspect_ratio.num = 59;
        avStream->sample_aspect_ratio.den = 54;
    } else if (100 == par) {
        // Typically HD
        avStream->sample_aspect_ratio.num = 1;
        avStream->sample_aspect_ratio.den = 1;
    } else if (91 == par) {
        // NTSC 4:3
        avStream->sample_aspect_ratio.num = 10;
        avStream->sample_aspect_ratio.den = 11;
    }

    avCodecContext->sample_aspect_ratio = avStream->sample_aspect_ratio;

    double fps = _fps->getValue();
    // timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1.
    //
    // Bug 23953
    // ffmpeg does a horrible job of converting floats to AVRationals
    // It adds 0.5 randomly and does some other stuff
    // To work around that, we just multiply the fps by what I think is a reasonable number to make it an int
    // and use the reasonable number as the numerator for the timebase.
    // Timebase is not the frame rate; it's the inverse of the framerate
    // So instead of doing 1/fps, we just set the numerator and denominator of the timebase directly.
    // The upshot is that this allows ffmpeg to properly do framerates of 23.78 (or 23.796, which is what the user really wants when they put that in).
    //
    // The code was this:
    //_streamVideo.codec->time_base = av_d2q(1.0 / fps_, 100);
    // const float CONVERSION_FACTOR = 1000.0f;
    // avCodecContext->time_base.num = (int)CONVERSION_FACTOR;
    // avCodecContext->time_base.den = (int)(fps * CONVERSION_FACTOR);

    int frameRate = (0.0 < fps) ? (int)fps : 0;
    const AVRational frame_rate = frameRateFromFps(fps);
    AVRational time_base = av_inv_q(clipFrameRate(frame_rate, avCodec->id));
    // copy timebase while removing common factors
    const AVRational zero = { 0, 1 };
    avStream->time_base = av_add_q(time_base, zero);
    // The requested frame rate, even if the codec's time base is reduced (MPEG-4 part 2, see clipFrameRate()):
    // the mov muxer initializes the timecode from it (libavformat/movenc.c: mov_check_timecode_track() calls
    // av_timecode_init_from_string() with src_st->avg_frame_rate), and since FFmpeg 9.1 (commit 30359442a7,
    // "avformat/movenc: always derive timecode timescale from timecode rate") the tmcd track's timescale
    // is fps * 1000 for drop-frame, rate.num if rate.den is 1001, else fps (mov_create_timecode_track()),
    // and its frame duration is that timescale divided by avg_frame_rate, rounded (mov_write_tmcd_tag()).
    // With the reduced 40999/342, a 119.88 fps non-drop-frame timecode would be written at 120/1 fps.
    // FFmpeg < 9.1 copies the video track's timescale (c5f7919a2c, also new in 9.1, had kept that for video
    // timescales up to 100000), which gives the video frame rate (40999/342) in both cases.
    avStream->avg_frame_rate = frame_rate; // see ffmpeg.c:2894 from ffmpeg 3.2.2 - may be set before avformat_write_header

    // Although it is marked as deprecated, if we don't don't set time_base in the codec context if fails
    // with message "The encoder timebase is not set". See ffmpeg-4.0/libavcodec/utils.c:758
    // [mov @ 0x1042d7600] Using AVStream.codec.time_base as a timebase hint to the muxer is deprecated. Set AVStream.time_base instead.
    avCodecContext->time_base = avStream->time_base;

    int gopSize = -1;
    if (p.interGOP) {
        gopSize = _gopSize->getValue();
        if (gopSize >= 0) {
            avCodecContext->gop_size = gopSize; // set the group of picture (GOP) size
            av_opt_set_int(avCodecContext->priv_data, "g", gopSize, 0);
        }
    }

    int bFrames = _bFrames->getValue();
    // NOTE: in new ffmpeg, bframes don't seem to work correctly - ffmpeg crashes...
    if (p.interB) {
        if (bFrames == -1 && p.interGOP && gopSize == 1) {
            bFrames = 0;
        }
        if (bFrames != -1) {
            avCodecContext->max_b_frames = bFrames; // set maximum number of B-frames between non-B-frames
            av_opt_set_int(avCodecContext->priv_data, "bf", bFrames, 0);
            avCodecContext->b_quant_factor = 2.0f; // QP factor between P- and B-frames
            av_opt_set_double(avCodecContext->priv_data, "b_qfactor", 2., 0);

            if (bFrames == 0 && p.interGOP && gopSize < 0) {
                // also set keyframe interval to 1, see https://trac.ffmpeg.org/wiki/Encode/VFX#Frame-by-Framescrubbing
                avCodecContext->gop_size = 1; // set the group of picture (GOP) size
                av_opt_set_int(avCodecContext->priv_data, "g", 1, 0);
            }
        }
    }

    FieldEnum fieldOrder = _inputClip->getFieldOrder();
    bool progressive = (fieldOrder == eFieldNone);
    // for the time being, we only support progressive encoding
    if (!progressive) {
        setPersistentMessage(Message::eMessageError, "", "only progressive video is supported");
        throwSuiteStatusException(kOfxStatFailed);
    }
    // Set this field so that an 'fiel' atom is inserted
    // into the QuickTime 'moov' atom.
    // [note: interlaced flags, see ffmpeg.c:1191 in ffmpeg 3.2.2
    // if (in_picture->interlaced_frame) {
    //    if (enc->codec->id == AV_CODEC_ID_MJPEG)
    //        mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TT:AV_FIELD_BB;
    //    else
    //        mux_par->field_order = in_picture->top_field_first ? AV_FIELD_TB:AV_FIELD_BT;
    //} else
    avCodecContext->field_order = AV_FIELD_PROGRESSIVE;

    // the following was moved here from openCodec()
    if (AV_CODEC_ID_DNXHD == avCodecContext->codec_id) {
        DNxHDCodecProfileEnum dnxhdCodecProfile = (DNxHDCodecProfileEnum)dnxhdCodecProfile_i;
        int srcWidth = avCodecContext->width;
        int srcHeight = avCodecContext->height;
        switch (dnxhdCodecProfile) {
        case eDNxHDCodecProfileHR444:
        case eDNxHDCodecProfileHRHQX:
        case eDNxHDCodecProfileHRHQ:
        case eDNxHDCodecProfileHRSQ:
        case eDNxHDCodecProfileHRLB:
            // DNxHR is resolution-independent
            // No conversion necessary.
            break;
        case eDNxHDCodecProfile440x:
        case eDNxHDCodecProfile220x:
        case eDNxHDCodecProfile220:
        case eDNxHDCodecProfile145:
        case eDNxHDCodecProfile36:
            // This writer will rescale for any source format
            // that is not natively supported by DNxHD. Check
            // that the source format matches a native DNxHD
            // format. If it's not supported, then force to
            // 1920x1080 (the most flexible format for DNxHD).
            // This will also mean that avcodec_open2 will not
            // fail as the format matches a native DNxHD format.
            // Any other frame dimensions result in error.
            if ((((1920 == srcWidth) /*|| (1440 == srcWidth)*/) && (1080 == srcHeight)) || (((1280 == srcWidth) /*|| (960 == srcWidth)*/) && (720 == srcHeight))) {
                // No conversion necessary.
            } else {
                avCodecContext->width = 1920;
                avCodecContext->height = 1080;
            }
            break;
        } // switch

        // If alpha is to be encoded, then the following must
        // be set to 32. This will ensure the correct bit depth
        // information will be embedded in the QuickTime movie.
        // Otherwise the following must be set to 24.
        const bool hasAlpha = alphaEnabled();
        avCodecContext->bits_per_coded_sample = (hasAlpha) ? 32 : 24;
        int mbs = 0;
        switch (dnxhdCodecProfile) {
        case eDNxHDCodecProfileHR444:
            av_opt_set(avCodecContext->priv_data, "profile", "dnxhr_444", 0);
            mbs = -1;
            break;
        case eDNxHDCodecProfileHRHQX:
            av_opt_set(avCodecContext->priv_data, "profile", "dnxhr_hqx", 0);
            mbs = -1;
            break;
        case eDNxHDCodecProfileHRHQ:
            av_opt_set(avCodecContext->priv_data, "profile", "dnxhr_hq", 0);
            mbs = -1;
            break;
        case eDNxHDCodecProfileHRSQ:
            av_opt_set(avCodecContext->priv_data, "profile", "dnxhr_sq", 0);
            mbs = -1;
            break;
        case eDNxHDCodecProfileHRLB:
            av_opt_set(avCodecContext->priv_data, "profile", "dnxhr_lb", 0);
            mbs = -1;
            break;
        case eDNxHDCodecProfile440x:
        case eDNxHDCodecProfile220x:
        case eDNxHDCodecProfile220:
        case eDNxHDCodecProfile145:
        case eDNxHDCodecProfile36:
            // the size is 1920x1080 or 1280x720 (see above), and the video is progressive
            mbs = dnxhdBitrateMbps(dnxhdCodecProfile, avCodecContext->height == 720, frameRate);
            if (mbs == 0) {
                const bool is36 = (dnxhdCodecProfile == eDNxHDCodecProfile36);
                setPersistentMessage(Message::eMessageError, "",
                                     string(is36 ? AVID_DNXHD_422_36_NAME : AVID_DNXHD_422_440X_NAME) + " is only available in 1920x1080: use "
                                         + (is36 ? AVID_DNXHD_422_145_NAME : AVID_DNXHD_422_220X_NAME) + " in 1280x720");
                throwSuiteStatusException(kOfxStatFailed);
            }
            break;
        } // switch
        if (mbs > 0) {
            avCodecContext->bit_rate = mbs * 1000000;
        }
        // #267: FFmpeg's dnxhd encoder stores the qscale of each macroblock in a uint8_t (libavcodec/dnxhdenc.h:101,
        // mb_qscale), but its default qmax is 1024: a qscale above 255 wraps, the slices overflow, and libavcodec
        // aborts (put_bits.h assertion) on high-entropy images (e.g. binary noise in DNxHR 444). 256 is the largest
        // safe value. A frame that needs a qscale near 255 or more then fails cleanly (a few that FFmpeg could encode
        // at 250-254 fail too); a frame that encodes is encoded exactly as before.
        // A qmax in Codec Options still overrides this (it is applied by avcodec_open2()).
        // There is no fixed FFmpeg version yet: once upstream fixes it, apply this only below that LIBAVCODEC_VERSION.
        if (avCodecContext->qmax > 256) {
            avCodecContext->qmax = 256;
        }
        // macroblock decision algorithm (high quality mode) = use best rate distortion
        // if (rd->ffcodecdata.flags & FFMPEG_LOSSLESS_OUTPUT)
        //  av_opt_set(avCodecContext->priv_data, "mbd", "rd", 0);
    }

    if (AV_CODEC_ID_HAP == avCodecContext->codec_id) {
        HapFormatEnum hapFormat = (HapFormatEnum)_hapFormat->getValue();
        switch (hapFormat) {
        case eHapFormatHap:
            av_opt_set(avCodecContext->priv_data, "format", "hap", 0);
            break;
        case eHapFormatHapAlpha:
            av_opt_set(avCodecContext->priv_data, "format", "hap_alpha", 0);
            break;
        case eHapFormatHapQ:
            av_opt_set(avCodecContext->priv_data, "format", "hap_q", 0);
            break;
        }
    }

    // Currently not set - the main problem being that the mov32 reader will use it to set its defaults.
    // TODO: investigate using the writer key in mov32 to ignore this value when set to mov64.
    // av_dict_set(&_formatContext->metadata, kMetaKeyPixelFormat, "YCbCr  8-bit 422 (2vuy)", 0);

    const char* ycbcrmetavalue = isRec709Format(avCodecContext->height) ? "Rec 709" : "Rec 601";
    av_dict_set(&_formatContext->metadata, kMetaKeyYCbCrMatrix, ycbcrmetavalue, 0);

#if OFX_FFMPEG_MBDECISION
    int mbDecision = _mbDecision->getValue();
    avCodecContext->mb_decision = mbDecision;
#else
    avCodecContext->mb_decision = FF_MB_DECISION_SIMPLE;
#endif
} // WriteFFmpegPlugin::configureVideoStream

////////////////////////////////////////////////////////////////////////////////
// addStream
// Add a new stream to the AVFormatContext of the file, with a codec context
// for the encoder avCodec, and configure it.
//
// @param avFormatContext A reference to an AVFormatContext of the file.
// @param avCodec The encoder, e.g. the one selected in the Codec menu
//                (initCodec()). It is not replaced by the default encoder of its
//                codec ID (avcodec_find_encoder()), which may be another one
//                (e.g. libx264 for libx264rgb or libopenh264, mpeg4 for libxvid).
// @param myStreamOut Receives the stream and its codec context if successful;
//                    unchanged otherwise.
//
void
WriteFFmpegPlugin::addStream(AVFormatContext* avFormatContext,
                             const AVCodec* avCodec,
                             MyAVStream* myStreamOut)
{
    AVCodecContext* avCodecContext = nullptr;
    AVStream* avStream = nullptr;

    if (!avCodec) {
        setPersistentMessage(Message::eMessageError, "", "could not find codec");

        return;
    }

    avCodecContext = avcodec_alloc_context3(avCodec);
    if (!avCodecContext) {
        setPersistentMessage(Message::eMessageError, "", "could not allocate codec context");
        return;
    }

    avStream = avformat_new_stream(avFormatContext, avCodecContext->codec);
    if (!avStream) {
        avcodec_free_context(&avCodecContext);
        setPersistentMessage(Message::eMessageError, "", "could not allocate stream");

        return;
    }

    avStream->id = avFormatContext->nb_streams - 1;

    myStreamOut->stream = avStream;
    myStreamOut->codecContext = avCodecContext;

    switch (avCodec->type) {
    case AVMEDIA_TYPE_AUDIO:
#if OFX_FFMPEG_AUDIO
        configureAudioStream(avCodec, myStreamOut);
#endif
        break;

    case AVMEDIA_TYPE_VIDEO:
        configureVideoStream(avCodec, myStreamOut);
        break;

    default:
        break;
    }
}

////////////////////////////////////////////////////////////////////////////////
// openCodec
// Open a codec.
//
// @param avFormatContext A reference to an AVFormatContext of the file.
// @param avCodec A reference to an AVCodec of the video codec.
// @param avStream A reference to an AVStream of a video stream.
//
// @return 0 if successful,
//         <0 otherwise.
//
int
WriteFFmpegPlugin::openCodec(AVFormatContext* avFormatContext,
                             const AVCodec* avCodec,
                             MyAVStream* myAVStream,
                             AVDictionary** options)
{
    assert(avFormatContext && avCodec && myAVStream && myAVStream->stream);
    if (!avFormatContext || !avCodec || !myAVStream || !myAVStream->stream) {
        return -1;
    }
    AVCodecContext* avCodecContext = myAVStream->codecContext;
    assert(avCodecContext);
    if (!avCodecContext) {
        return -1;
    }
    if (AVMEDIA_TYPE_AUDIO == avCodecContext->codec_type) {
        // Audio codecs.
        int error = avcodec_open2(avCodecContext, avCodec, nullptr);
        if (error < 0) {
            // Report the error.
            char szError[1024] = { 0 };
            av_strerror(error, szError, sizeof(szError));
            setPersistentMessage(Message::eMessageError, "", string("Could not open audio codec: ") + szError);

            return -1;
        }
    } else if (AVMEDIA_TYPE_VIDEO == avCodecContext->codec_type) {
        // The user options (if any) are applied by avcodec_open2() after all the options that were set
        // on avCodecContext and its priv_data, so they take precedence. Recognized options are removed
        // from *options.
        const bool hasOptions = options && av_dict_count(*options) > 0;
        int error = avcodec_open2(avCodecContext, avCodec, options);
        if (error < 0) {
            // Report the error.
            char szError[1024] = { 0 };
            av_strerror(error, szError, sizeof(szError));
            string msg = string("Could not open video codec: ") + szError;
            const CodecParams p = GetCodecSupportedParams(avCodec);
            if (p.crf && !p.crfLossless && ((CRFEnum)_crf->getValue() == eCRFLossless)) {
                // e.g. libvpx (VP8), whose CRF 0 is out of its quantizer range (FFmpeg n9.1 libavcodec/libvpxenc.c:1145)
                msg += string("\nThe ") + kParamCRFLabel + " Lossless (CRF 0) may not be available with the " + avCodec->name
                       + " encoder: it has no lossless mode. Choose another " + kParamCRFLabel + ".";
            }
            if (hasOptions) {
                msg += string("\nCheck the values in ") + kParamCodecOptionsLabel + ".";
            }
            setPersistentMessage(Message::eMessageError, "", msg);

            return -4;
        }
    } else if (AVMEDIA_TYPE_DATA == avCodecContext->codec_type) {
        // Timecode codecs.
    }

    // see ffmpeg.c:3042 from ffmpeg 3.2.2
    int ret = avcodec_parameters_from_context(myAVStream->stream->codecpar, avCodecContext);
    if (ret < 0) {
        setPersistentMessage(Message::eMessageError, "", string("Error initializing the output stream codec context."));

        return -5;
    }

    return 0;
}

// avcodec_send_frame() and avcodec_receive_packet() replace avcodec_encode_video2(), see
// https://github.com/FFmpeg/FFmpeg/blob/9e30859cb60b915f237581e3ce91b0d31592edc0/libavcodec/encode.c#L360
// Doc for the new AVCodec API: https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/
// Only used by the (currently disabled) audio path; the video path drains the encoder
// directly in writeVideo().
#if OFX_FFMPEG_AUDIO
static int
mov64_av_encode(AVCodecContext* avctx,
                AVPacket* avpkt,
                const AVFrame* frame,
                int* got_packet_ptr)
{
    int ret;

    ret = avcodec_send_frame(avctx, frame);
    if (ret < 0) {
        *got_packet_ptr = 0;
        if (ret != AVERROR_EOF)
            return ret;
    }

    ret = avcodec_receive_packet(avctx, avpkt);
    if (avpkt->data && avpkt->size > 0) {
        *got_packet_ptr = 1;
    }
    if (ret < 0) {
        *got_packet_ptr = 0;
        if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            return ret;
    }

    return 0;
}
#endif // OFX_FFMPEG_AUDIO

#if OFX_FFMPEG_AUDIO
////////////////////////////////////////////////////////////////////////////////
// writeAudio
//
// * Read audio from the source file (this will also convert to the desired
//   sample format for the file).
// * Encode
// * Write to file.
//
// NOTE: Writing compressed audio is not yet implemented. E.g. 'flushing' the
//       encoder when the writer has finished writing all the video frames.
//
// @param avFormatContext A reference to an AVFormatContext of the file.
// @param avStream A reference to an AVStream of an audio stream.
// @param flush A boolean value to flag that any remaining frames in the internal
//              queue of the encoder should be written to the file. No new
//              frames will be queued for encoding.
//
// @return 0 if successful,
//         <0 otherwise for any failure to read (and convert) the audio format,
//         encode the audio or write to the file.
//
int
WriteFFmpegPlugin::writeAudio(AVFormatContext* avFormatContext,
                              MyAVStream* myAVStream,
                              bool flush)
{
    int ret = 0;
    MyAVFrame avFrame;
    const int audioReaderResult = audioReader_->read(avFrame);
    if (audioReaderResult > 0) { // read successful
        MyAVPacket pkt;
        if (flush) {
            // A slight hack.
            // So that the durations of the video track and audio track will be
            // as close as possible, when flushing (finishing) calculate how many
            // remaining audio samples to write using the difference between the
            // the duration of the video track and the duration of the audio track.
            const double videoTime = (av_stream_get_end_pts(_streamVideo.stream) + av_rescale_q(1, _streamVideo.codecContext->time_base, _streamVideo.stream->time_base)) * av_q2d(_streamVideo.stream->time_base);
            const double audioPts = static_cast<int>(av_stream_get_end_pts(_streamAudio.stream));
            const int samplesToFinish = static_cast<int>((videoTime / av_q2d(_streamAudio.stream->time_base)) - audioPts);
            if (0 < samplesToFinish) {
                nbSamples = delta / av_q2d(_streamAudio->time_base);
                // Add one sample to the count to guarantee that the audio track
                // will be the same length or very slightly longer than the video
                // track. This will then end the final loop that writes audio up
                // to the duration of the video track.
                if (avFrame->nb_samples > samplesToFinish) {
                    avFrame->nb_samples = samplesToFinish;
                }
            }
        }

        AVCodecContext* avCodecContext = myAVStream->codecContext;
        int gotPacket;
        ret = mov64_av_encode(avCodecContext, pkt.pkt(), avFrame, &gotPacket);
        if (!ret && gotPacket) {
            pkt->stream_index = myAVStream->stream->index;
            ret = av_write_frame(avFormatContext, pkt.pkt());
        }

        if (ret < 0) {
            // Report the error.
            char szError[1024] = { 0 };
            av_strerror(ret, szError, sizeof(szError));
            iop->error(szError);
        }
    } else if (audioReaderResult < 0) {
        // error in read from audio reader
        ret = audioReaderResult;
    }

    return ret;
}

#endif // if OFX_FFMPEG_AUDIO

// The flags of the conversion to Y'CbCr when the size does not change (#207): SWS_AREA gives each chroma sample of
// 4:2:0 and 4:2:2 the average of the pixels it covers, the 2x2 box (2x1 in 4:2:2). SWS_POINT point-sampled the chroma:
// one of the two lines in 4:2:0, and one of the two columns at odd widths (at even widths, swscale averages pairs of
// pixels anyway: the *ToUV_half input functions, libswscale/utils.c:1380-1401 and libswscale/input.c:1155-1170 in
// n9.0.2). Measured on FFmpeg 8.0 and 9.0.2 (BT.601), with alternating red and blue lines in 4:2:0: Cb 240 (the blue
// line) with SWS_POINT, 165 with SWS_AREA (the average, 165.1); with alternating columns at an odd width (49): Cb 90
// (red) with SWS_POINT, 164 with SWS_AREA, which resamples the 49 columns to 25 samples. Luma, and the rounding of RGB
// to Y'CbCr (at most 0.51 code value at 8 bits), do not change. SWS_BICUBIC and SWS_BILINEAR would blur flat 2x2
// blocks, and SWS_ACCURATE_RND changes nothing here.
constexpr int kWriterSwsFlagsSameSize = SWS_AREA;

// #316, writer side: the value of the 16-bit R', G' and B' (RGB48 or RGBA64) that the conversion to Y'CbCr takes for
// white. swscale's conversion from 16-bit RGB to Y'CbCr (FFmpeg 8.0 to 9.1) takes 255·256 = 65280, not 65535, as
// white (upstream: https://code.ffmpeg.org/FFmpeg/FFmpeg/issues/25681, the same scaling as for the reader).
// Measured on FFmpeg 8.0 and 9.0.2 (BT.709): in video range, R' = G' = B' = 65535 gives Y' = 943 at 10 bits (exact:
// 940), 3774 at 12 bits (3760) and 236 at 8 bits (235), and 32768 gives 504 at 10 bits (502.0); in full range, 32768
// gives 514 at 10 bits (511.5), and the values above 65280 are clipped. Alpha is not scaled (65535 gives 1023 at 10
// bits). The RGB48 input functions (rgb48ToY_c_template(), libswscale/input.c:134-148 in n9.0.2) use the table that
// fill_rgb2yuv_table() (libswscale/utils.c:625-700) computes for 8-bit input.
// The conversion of a small image whose R', G' and B' are 65280 measures it: if it gives the nominal white Y'
// (235·2^(n-8) in video range, 2^n - 1 in full range, ITU-T H.273 §8.3) within half an 8-bit step, white is 65280;
// else it is 65535 (a conversion that takes 65535 as white gives 0.85 8-bit step less, e.g. 937 at 10 bits): no
// scaling once swscale is fixed.
unsigned
MeasureRgb16White(AVPixelFormat srcPixelFormat,
                  AVPixelFormat dstPixelFormat,
                  int swsFlags,
                  int colorspace,
                  int dstRange)
{
    using FFmpeg::kWhite16;
    constexpr unsigned kLegacyWhite16 = 255u << 8;
    const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(srcPixelFormat);
    const AVPixFmtDescriptor* dstDesc = av_pix_fmt_desc_get(dstPixelFormat);
    if (!srcDesc || !dstDesc || (srcDesc->comp[0].depth != 16) || !FFmpeg::pixelFormatIsYUV(dstPixelFormat)
        || (dstDesc->comp[0].depth < 8) || (dstDesc->comp[0].depth > 16)) {
        return kWhite16;
    }
    // 16x4: whole blocks of any chroma subsampling
    const int width = 16, height = 4;
    uint8_t* srcData[4] = {};
    int srcLinesize[4] = {};
    if (av_image_alloc(srcData, srcLinesize, width, height, srcPixelFormat, 64) < 0) {
        return kWhite16;
    }
    const FFmpeg::AvBuffer srcBuffer(srcData[0], &av_free);
    uint8_t* dstData[4] = {};
    int dstLinesize[4] = {};
    if (av_image_alloc(dstData, dstLinesize, width, height, dstPixelFormat, 64) < 0) {
        return kWhite16;
    }
    const FFmpeg::AvBuffer dstBuffer(dstData[0], &av_free);
    const int nbComponents = srcDesc->nb_components; // 3 (RGB48) or 4 (RGBA64), in this order
    for (int y = 0; y < height; ++y) {
        uint16_t* row = reinterpret_cast<uint16_t*>(srcData[0] + (ptrdiff_t)y * srcLinesize[0]);
        for (int i = 0; i < width * nbComponents; ++i) {
            row[i] = ((i % nbComponents) == 3) ? kWhite16 : kLegacyWhite16; // (native endianness: AV_PIX_FMT_RGB48)
        }
    }
    SwsContext* context = sws_getContext(width, height, srcPixelFormat, width, height, dstPixelFormat, swsFlags, nullptr, nullptr, nullptr);
    if (!context) {
        return kWhite16;
    }
    const int details = sws_setColorspaceDetails(context, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(colorspace), dstRange, 0, 1 << 16, 1 << 16);
    const int lines = (details < 0) ? 0 : sws_scale(context, srcData, srcLinesize, 0, height, dstData, dstLinesize);
    sws_freeContext(context);
    if (lines != height) { // (no correction)
        return kWhite16;
    }
    uint32_t luma = 0; // Y' in the middle of the image
    av_read_image_line2(&luma, const_cast<const uint8_t**>(dstData), dstLinesize, dstDesc, width / 2, height / 2, 0, 1, 0, 4);
    const int depth = dstDesc->comp[0].depth;
    const int nominalWhite = dstRange ? ((1 << depth) - 1) : (235 << (depth - 8));
    const int halfStep8 = (1 << (depth - 8)) / 2; // half an 8-bit step (0 at 8 bits: equal)

    return (std::abs((int)luma - nominalWhite) <= halfStep8) ? kLegacyWhite16 : kWhite16;
} // MeasureRgb16White

// Maps 0.-1. to 0-white (see MeasureRgb16White()), rounded to the nearest and clamped to 0-65535: the values above 1
// keep the headroom that the conversion to Y'CbCr has above white. With white = 65535, this is floatToInt<65536>(); a
// NaN gives 0, as there.
inline unsigned short
floatToRgb16(float value,
             unsigned white)
{
    if (!(value > 0.f)) {
        return 0;
    }
    const float v = value * (float)white + 0.5f;

    return (v < 65535.f) ? (unsigned short)v : (unsigned short)65535;
}

int
WriteFFmpegPlugin::initConvertCtx(AVPixelFormat srcPixelFormat,
                                  AVPixelFormat dstPixelFormat,
                                  AVCodecContext* avCodecContext)
{
    assert(!_convertCtx);
    int ret = 0;
    const int width = (_rodPixel.x2 - _rodPixel.x1);
    const int height = (_rodPixel.y2 - _rodPixel.y1);
    const int swsFlags = (width == avCodecContext->width && height == avCodecContext->height) ? kWriterSwsFlagsSameSize : SWS_BICUBIC;
    _convertCtx = sws_getContext(width, height, srcPixelFormat, // from
                                 avCodecContext->width, avCodecContext->height, dstPixelFormat, // to
                                 swsFlags, nullptr, nullptr, nullptr);
    if (!_convertCtx) {
        return -1;
    }

    // Only apply colorspace conversions for YUV.
    if (FFmpeg::pixelFormatIsYUV(dstPixelFormat)) {
        // Set up the sws (SoftWareScaler) to convert colourspaces correctly, in the sws_scale function below
        // The Y'CbCr matrix the stream is tagged with (setColorTags())
        int colorspace = FFmpeg::swsColorspaceFromAVColorSpace(avCodecContext->colorspace);
        if (colorspace < 0) {
            colorspace = isRec709Format(height) ? SWS_CS_ITU709 : SWS_CS_ITU601;
        }
        // The range of the stream (e.g. the DNxHD Output Range, or full range for the encoders that only write it, such
        // as mjpeg: see beginEncode())
        const int dstRange = (avCodecContext->color_range == AVCOL_RANGE_JPEG) ? 1 : 0; // 0 = 16..235, 1 = 0..255

        ret = sws_setColorspaceDetails(_convertCtx,
                                       sws_getCoefficients(SWS_CS_DEFAULT), // inv_table
                                       1, // srcRange - 0 = 16..235, 1 = 0..255
                                       sws_getCoefficients(colorspace), // table
                                       dstRange, // dstRange - 0 = 16..235, 1 = 0..255
                                       0, // brightness fixed point, with 0 meaning no change,
                                       1 << 16, // contrast   fixed point, with 1<<16 meaning no change,
                                       1 << 16); // saturation fixed point, with 1<<16 meaning no change);
        _rgb16White = MeasureRgb16White(srcPixelFormat, dstPixelFormat, swsFlags, colorspace, dstRange);
    } else {
        _rgb16White = FFmpeg::kWhite16;
    }
    if (ret < 0) { // every frame fails
        sws_freeContext(_convertCtx);
        _convertCtx = nullptr;
    }

    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// More of a utility function added since supported DNxHD.
// This codec requires colour space conversions, and more to the point, requires
// VIDEO levels for all formats. Specifically, 4:4:4 requires video levels on
// the input RGB component data!
//
int
WriteFFmpegPlugin::colourSpaceConvert(AVFrame* avFrameIn,
                                      AVFrame* avFrameOut,
                                      AVPixelFormat srcPixelFormat,
                                      AVPixelFormat dstPixelFormat,
                                      AVCodecContext* avCodecContext)
{
    if (!avFrameIn || !avFrameOut || !avCodecContext) {
        return -1;
    }
    (void)srcPixelFormat;
    (void)dstPixelFormat;
    assert(_convertCtx); // (writeVideo(): initConvertCtx())
    if (!_convertCtx) {
        return -1;
    }

    const int height = (_rodPixel.y2 - _rodPixel.y1);
    sws_scale(_convertCtx,
              avFrameIn->data, // src
              avFrameIn->linesize, // src rowbytes
              0,
              height,
              avFrameOut->data, // dst
              avFrameOut->linesize); // dst rowbytes

    return 0;
}

bool
WriteFFmpegPlugin::alphaEnabled() const
{
    // is the writer configured to write alpha channel to file ?
    bool enableAlpha = _prefAlpha->getValue();

    return enableAlpha && _inputClip->getPixelComponents() == ePixelComponentRGBA;
}

int
WriteFFmpegPlugin::numberOfDestChannels() const
{
    // 4 channels (RGBA) if alpha is enabled, else 3 (RGB)
    return alphaEnabled() ? 4 : 3;
}

////////////////////////////////////////////////////////////////////////////////
// writeVideo
//
// * Convert Nuke float RGB values to the ffmpeg pixel format of the encoder.
// * Encode.
// * Write to file.
//
// @param avFormatContext A reference to an AVFormatContext of the file.
// @param avStream A reference to an AVStream of a video stream.
// @param flush A boolean value to flag that any remaining frames in the internal
//              queue of the encoder should be written to the file. No new
//              frames will be queued for encoding.
//
// @return 0 if successful,
//         <0 otherwise for any failure to convert the pixel format, encode the
//         video or write to the file.
//
int
WriteFFmpegPlugin::writeVideo(AVFormatContext* avFormatContext,
                              MyAVStream* myAVStream,
                              bool flush,
                              double /*time*/,
                              const float* pixelData,
                              const OfxRectI* bounds,
                              int pixelDataNComps,
                              int dstNComps,
                              int rowBytes)
{
    assert(dstNComps == 3 || dstNComps == 4 || dstNComps == 0);
    // FIXME enum needed for error codes.
    if (!_isOpen) {
        return -5; // writer is not open!
    }
    AVStream* avStream = myAVStream->stream;
    if (!avStream) {
        return -6;
    }
    assert(avFormatContext);
    if (!avFormatContext || (!flush && (!pixelData || !bounds))) {
        return -7;
    }
    int ret = 0;
    // First convert from Nuke floating point RGB to either 16-bit or 8-bit RGB.
    // Create a buffer to hold either  16-bit or 8-bit RGB.
    AVCodecContext* avCodecContext = myAVStream->codecContext;
    assert(avCodecContext);
    if (!avCodecContext) {
        return -8;
    }
    // Create another buffer to convert from either 16-bit or 8-bit RGB
    // to the input pixel format required by the encoder.
    AVPixelFormat pixelFormatCodec = avCodecContext->pix_fmt;
    int width = _rodPixel.x2 - _rodPixel.x1;
    int height = _rodPixel.y2 - _rodPixel.y1;

    if (!flush) {
        assert(pixelData && bounds);

        const bool hasAlpha = alphaEnabled();
        AVPixelFormat pixelFormatNuke;
        if (hasAlpha) {
            pixelFormatNuke = (avCodecContext->bits_per_raw_sample > 8) ? AV_PIX_FMT_RGBA64 : AV_PIX_FMT_RGBA;
        } else {
            pixelFormatNuke = (avCodecContext->bits_per_raw_sample > 8) ? AV_PIX_FMT_RGB48 : AV_PIX_FMT_RGB24;
        }

        // Reuse the input conversion buffer across frames; only (re)allocate when it
        // does not yet exist or its geometry/pixel format changed. This avoids an
        // av_image_alloc + av_freep on every frame during a sequence render (the
        // output buffer, outputFrame_, is already reused the same way below).
        if (!inputFrame_
            || inputFrame_->width != width
            || inputFrame_->height != height
            || inputFrame_->format != (int)pixelFormatNuke) {
            inputFrame_.reset(av_frame_alloc(), [](AVFrame* frame) { av_freep(&frame->data[0]); av_frame_free(&frame); });
            const int allocRet = inputFrame_ ? av_image_alloc(inputFrame_->data, inputFrame_->linesize, width, height, pixelFormatNuke, 32) : -1;
            if (allocRet < 0) {
                inputFrame_.reset();
            } else {
                inputFrame_->width = width;
                inputFrame_->height = height;
                inputFrame_->format = (int)pixelFormatNuke;
            }
        }
        ret = inputFrame_ ? 0 : -1;
        // the conversion to the pixel format of the encoder, and the 16-bit value it takes for white (_rgb16White,
        // used below): the frame fails without it
        if (inputFrame_ && !_convertCtx && (initConvertCtx(pixelFormatNuke, pixelFormatCodec, avCodecContext) < 0)) {
            ret = -1;
        }

        if (!ret) {
            // Convert floating point values to unsigned values.
            // The frames of the movie have the size it was opened with (_rodPixel, the output RoD at
            // beginSequenceRender), but the image may have other bounds: the output RoD may change during
            // the render (Natron sets the project format from the first Reader once the render has started).
            // Only read the part of the image that is in the frame, the rest of the frame is black and
            // transparent: reading the image as if it had the size of the movie reads out of its buffer.
            assert(rowBytes && rowBytes >= (int)sizeof(float) * (bounds->x2 - bounds->x1) * pixelDataNComps);
            const int numDestChannels = hasAlpha ? 4 : 3;
            // the columns of the frame that are in the image
            const int xBegin = (std::max)(bounds->x1, _rodPixel.x1) - _rodPixel.x1;
            const int xEnd = (std::min)(bounds->x2, _rodPixel.x2) - _rodPixel.x1;
            const bool highBitDepth = (avCodecContext->bits_per_raw_sample > 8);
            const int bytesPerComponent = highBitDepth ? 2 : 1;
            assert(inputFrame_->linesize[0] >= width * numDestChannels * bytesPerComponent);

            for (int y = 0; y < height; ++y) {
                // the frame is top-down, the image bottom-up
                const int imageY = _rodPixel.y1 + (height - 1 - y);
                unsigned char* dst_row = inputFrame_->data[0] + (ptrdiff_t)y * inputFrame_->linesize[0];
                if ((imageY < bounds->y1) || (imageY >= bounds->y2) || (xBegin >= xEnd)) {
                    std::memset(dst_row, 0, (size_t)width * numDestChannels * bytesPerComponent);
                    continue;
                }
                if ((xBegin > 0) || (xEnd < width)) {
                    std::memset(dst_row, 0, (size_t)width * numDestChannels * bytesPerComponent);
                }
                // the pixel of the image at column _rodPixel.x1 + x is src_pixels[(x - xBegin) * pixelDataNComps]
                const float* src_pixels = (const float*)((const char*)pixelData + (ptrdiff_t)(imageY - bounds->y1) * (ptrdiff_t)rowBytes) +
                                          (ptrdiff_t)(_rodPixel.x1 + xBegin - bounds->x1) * pixelDataNComps;

                if (highBitDepth) {
                    assert(pixelFormatNuke == AV_PIX_FMT_RGBA64 || pixelFormatNuke == AV_PIX_FMT_RGB48);

                    unsigned short* dst_pixels = reinterpret_cast<unsigned short*>(dst_row);
                    // the value that the conversion to the encoder's pixel format takes for white (#316)
                    const unsigned white = _rgb16White;

                    for (int x = xBegin; x < xEnd; ++x) {
                        int srcCol = (x - xBegin) * pixelDataNComps;
                        int dstCol = x * numDestChannels;
                        dst_pixels[dstCol + 0] = floatToRgb16(src_pixels[srcCol + 0], white);
                        dst_pixels[dstCol + 1] = floatToRgb16(src_pixels[srcCol + 1], white);
                        dst_pixels[dstCol + 2] = floatToRgb16(src_pixels[srcCol + 2], white);
                        if (hasAlpha) {
                            dst_pixels[dstCol + 3] = floatToInt<65536>((pixelDataNComps == 4) ? src_pixels[srcCol + 3] : 1.);
                        }
                    }
                } else {
                    assert(pixelFormatNuke == AV_PIX_FMT_RGBA || pixelFormatNuke == AV_PIX_FMT_RGB24);

                    unsigned char* dst_pixels = dst_row;

                    for (int x = xBegin; x < xEnd; ++x) {
                        int srcCol = (x - xBegin) * pixelDataNComps;
                        int dstCol = x * numDestChannels;
                        dst_pixels[dstCol + 0] = floatToInt<256>(src_pixels[srcCol + 0]);
                        dst_pixels[dstCol + 1] = floatToInt<256>(src_pixels[srcCol + 1]);
                        dst_pixels[dstCol + 2] = floatToInt<256>(src_pixels[srcCol + 2]);
                        if (hasAlpha) {
                            dst_pixels[dstCol + 3] = floatToInt<256>((pixelDataNComps == 4) ? src_pixels[srcCol + 3] : 1.);
                        }
                    }
                }
            }

            assert(inputFrame_);
            if (!inputFrame_) {
                ret = -1;
            } else {
                // For any codec an
                // intermediate buffer is allocated for the
                // colour space conversion.

                if (!outputFrame_) {
                    outputFrame_.reset(av_frame_alloc(), [](AVFrame* frame) { av_freep(&frame->data[0]); av_frame_free(&frame); });

                    outputFrame_->width = avCodecContext->width;
                    outputFrame_->height = avCodecContext->height;
                    outputFrame_->format = avCodecContext->pix_fmt;
                    // Encoders that write the color properties in each frame (e.g. ProRes, in its frame header) take
                    // them from the frame, not from the codec context. Only for the Y'CbCr pixel formats, which get a
                    // matrix (setColorTags()): the encoders of RGB formats that read them, e.g. png (sRGB, cICP, cHRM
                    // and gAMA chunks, FFmpeg n9.0 libavcodec/pngenc.c:433-481), keep writing what they wrote before.
                    if ((avCodecContext->colorspace != AVCOL_SPC_UNSPECIFIED) && (avCodecContext->colorspace != AVCOL_SPC_RGB)) {
                        outputFrame_->color_primaries = avCodecContext->color_primaries;
                        outputFrame_->color_trc = avCodecContext->color_trc;
                        outputFrame_->colorspace = avCodecContext->colorspace;
                        outputFrame_->color_range = avCodecContext->color_range;
                    }

                    int bufferSize = av_image_alloc(outputFrame_->data, outputFrame_->linesize, outputFrame_->width, outputFrame_->height, pixelFormatCodec, 32);

                    if (bufferSize < 0) {
                        outputFrame_.reset();
                    }
                }

                if (outputFrame_) {
                    colourSpaceConvert(inputFrame_.get(), outputFrame_.get(), pixelFormatNuke, pixelFormatCodec, avCodecContext);

                    // as fftools does (FFmpeg n9.1 fftools/ffmpeg_enc.c:887): the encoders that read Global Quality
                    // from the quality of each frame (e.g. mjpeg, svq1, jpeg2000, cinepak and libxvid) need it
                    outputFrame_->quality = avCodecContext->global_quality;
                    outputFrame_->pict_type = AV_PICTURE_TYPE_NONE;
                    outputFrame_->pts = _pts_counter;
                } else {
                    // av_image_alloc failed.
                    ret = -1;
                }
            }
        }
    } else {
        inputFrame_.reset();
        outputFrame_.reset();
    }

    if (!ret) {
        // Send the frame to the encoder (a NULL frame on flush enters draining mode),
        // then write out *every* packet the encoder produces. This matches FFmpeg's
        // canonical encode/mux loop: a single send may yield zero, one, or several
        // packets, and lookahead encoders (e.g. H.264/H.265) buffer many frames before
        // emitting output. Draining in a loop ensures frames are never dropped when the
        // encoder buffers input (the previous one-packet-per-send code dropped a frame
        // whenever avcodec_send_frame returned EAGAIN).
        // NOTE: If |flush| is true, then outputFrame_ will be NULL at this point as
        //       alloc will not have been called.

        _pts_counter++;
        const int sendRet = avcodec_send_frame(avCodecContext, flush ? nullptr : outputFrame_.get());
        // AVERROR_EOF is expected when a flush (NULL) packet is re-sent after draining.
        // EAGAIN should not occur because we fully drain the encoder below after every send.
        if ((sendRet < 0) && (sendRet != AVERROR_EOF)) {
            char szError[1024];
            av_strerror(sendRet, szError, sizeof(szError));
            setPersistentMessage(Message::eMessageError, "", string("Cannot encode frame: ") + szError);
            av_log(avCodecContext, AV_LOG_ERROR, "error writing frame to file\n");
            ret = -2;
        } else {
            bool wrotePacket = false;
            bool error = false;

            for (;;) {
                MyAVPacket pkt;
                const int recvRet = avcodec_receive_packet(avCodecContext, pkt.pkt());
                if ((recvRet == AVERROR(EAGAIN)) || (recvRet == AVERROR_EOF)) {
                    break;
                }
                if (recvRet < 0) {
                    // Report the error.
                    char szError[1024];
                    av_strerror(recvRet, szError, sizeof(szError));
                    setPersistentMessage(Message::eMessageError, "", string("Cannot encode frame: ") + szError);
                    error = true;
                    break;
                }

                // libx265 leaves the dts of the first packets uninitialized if fewer frames were encoded than its
                // B-frame delay (bframes ? (b-pyramid ? 2 : 1) : 0, so 2 by default): x265's Encoder sets
                // m_bframeDelayTime only when frame number m_bframeDelay is received (x265 4.2,
                // source/encoder/encoder.cpp:1791-1792), but the dts of the first m_bframeDelay packets is
                // m_reorderedPts - m_bframeDelayTime (encoder.cpp:2444-2446), and m_bframeDelayTime is not
                // initialized (encoder.h:186). libavcodec/libx265.c copies it (pkt->dts = x265pic_out->dts). With
                // 1 or 2 frames, the dts is then garbage, which the mov muxer rejects ("pts/dts pair unsupported",
                // libavformat/movenc.c: ff_mov_write_packet()), or that would delay the start of the video.
                // A dts can not be after the pts, nor more frames before it than frames were sent to the encoder
                // (the input pts are 0, 1, 2... in the codec time base): use the pts (there are no B-frames with
                // fewer than 3 frames), or the next tick after the previous dts.
                if ((pkt->pts != AV_NOPTS_VALUE) && (pkt->dts != AV_NOPTS_VALUE) && (pkt->pts >= 0) &&
                    ((pkt->dts > pkt->pts) || (pkt->dts < pkt->pts - (int64_t)_pts_counter))) {
                    const int64_t dts = (_lastDts == AV_NOPTS_VALUE) ? pkt->pts : (std::min)(pkt->pts, _lastDts + 1);
                    av_log(avCodecContext, AV_LOG_WARNING, "invalid dts %" PRId64 " for pts %" PRId64 ", using %" PRId64 "\n", (int64_t)pkt->dts, (int64_t)pkt->pts, dts);
                    pkt->dts = dts;
                }
                if ((int64_t)(pkt->dts) != AV_NOPTS_VALUE) {
                    _lastDts = pkt->dts;
                }

                // Each of these packets should consist of a single frame therefore each one
                // must have a PTS in order to correctly set the frame-rate
                if ((int64_t)(pkt->dts) != AV_NOPTS_VALUE) {
                    pkt->dts = av_rescale_q(pkt->dts, avCodecContext->time_base, avStream->time_base);
                } else {
                    pkt->dts = av_rescale_q(_pts_counter, avCodecContext->time_base, avStream->time_base);
                }

                if ((int64_t)(pkt->pts) != AV_NOPTS_VALUE) {
                    pkt->pts = av_rescale_q(pkt->pts, avCodecContext->time_base, avStream->time_base);
                } else {
                    pkt->pts = av_rescale_q(_pts_counter, avCodecContext->time_base, avStream->time_base);
                }

                if (!pkt->duration) {
                    pkt->duration = av_rescale_q(1, avCodecContext->time_base, avStream->time_base);
                }

                pkt->stream_index = avStream->index;

                const int writeResult = av_interleaved_write_frame(avFormatContext, pkt.pkt());
                if (writeResult < 0) {
                    // Report the error.
                    char szError[1024];
                    av_strerror(writeResult, szError, sizeof(szError));
                    setPersistentMessage(Message::eMessageError, "", string("Cannot write frame: ") + szError);
                    error = true;
                    break;
                }
                wrotePacket = true;
                ++_packetsWritten;
            }

            if (error) {
                av_log(avCodecContext, AV_LOG_ERROR, "error writing frame to file\n");
                ret = -2;
            } else if (flush && !wrotePacket) {
                // The encoder has been fully drained: signal completion to
                // endEncode()'s flush loop.
                ret = -10;
            }
        }
    }

    return ret;
} // WriteFFmpegPlugin::writeVideo

////////////////////////////////////////////////////////////////////////////////
// writeToFile
// Write video and if specified audio to the movie. Interleave the audio and
// video as specified in the QuickTime movie recommendation. This is to have
// ~0.5s of audio interleaved with the video.
//
// @param avFormatContext A reference to the AVFormatContext of the file to
//                        write.
// @param finalise A flag to indicate that the streams should be flushed as
//                 no further frames are to be encoded.
//
// @return 0 if successful.
//         <0 otherwise.
//
int
WriteFFmpegPlugin::writeToFile(AVFormatContext* avFormatContext,
                               bool finalise,
                               double time,
                               const float* pixelData,
                               const OfxRectI* bounds,
                               int pixelDataNComps,
                               int dstNComps,
                               int rowBytes)
{
#if OFX_FFMPEG_AUDIO
    // Write interleaved audio and video if an audio file has
    // been specified. Otherwise just write video.
    //
    // If an audio file has been specified, calculate the
    // target stream time of the audio stream. For a QuickTime
    // movie write the audio in ~0.5s chunks so that there is
    // an approximate 0.5s interleave of audio and video.
    if (_streamAudio.stream && _streamVideo.stream) {
        const double videoTime = av_stream_get_end_pts(_streamVideo.stream) * av_q2d(_streamVideo.stream->time_base);
        // Determine the current audio stream time.
        double audioTime = av_stream_get_end_pts(_streamAudio.stream) * av_q2d(_streamAudio.stream->time_base);
        // Determine the target audio stream time. This is
        // the current video time rounded to the nearest
        // 0.5s boundary.
        const double targetTime = ((int)(videoTime / 0.5)) * 0.5;
        if (audioTime < targetTime) {
            // If audio stream is more than 0.5s behind, write
            // another ~0.5s of audio.
            const double sourceDuration = audioReader_->getDuration();
            int writeResult = 0;
            while ((audioTime < targetTime) && (audioTime < sourceDuration) && (writeResult >= 0)) {
                writeResult = writeAudio(avFormatContext, &_streamAudio, finalise);
                audioTime = av_stream_get_end_pts(_streamAudio.stream) * av_q2d(_streamAudio.stream->time_base);
            }
        }
    }
#endif
    if (!_streamVideo.stream) {
        return -6;
    }
    assert(avFormatContext);
    if (!avFormatContext || (!finalise && (!pixelData || !bounds))) {
        return -7;
    }

    return writeVideo(avFormatContext, &_streamVideo, finalise, time, pixelData, bounds, pixelDataNComps, dstNComps, rowBytes);
}

////////////////////////////////////////////////////////////////////////////////
// open
// Internal function to create all the required streams for writing a QuickTime
// movie.
//
// @return true if successful,
//         false otherwise.
//
void
WriteFFmpegPlugin::beginEncode(const string& filename,
                               const OfxRectI& rodPixel,
                               float pixelAspectRatio,
                               const BeginSequenceRenderArguments& args)
{
    // (not args.sequentialRenderStatus: kOfxImageEffectPropSequentialRenderStatus is OFX 1.2, a 1.0 or 1.1 host
    // doesn't set it, and a host may set it to 0 for a render that is sequential anyway, as WriteFFmpeg requires
    // with kOfxImageEffectInstancePropSequentialRender)
    if (_formatContext || _streamVideo.stream) {
        setPersistentMessage(Message::eMessageError, "", "Another render is currently active");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // the state of the previous render does not apply to this one: until beginEncode() succeeds, endEncode() has
    // nothing to finish (#289)
    _error = IGNORE_FINISH;

    if (args.isInteractive) {
        setPersistentMessage(Message::eMessageError, "", "can only write files when in non-interactive mode.");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    assert(!_convertCtx);

    // first, check that the codec setting is OK
    checkCodec();

    // Parse the user codec and format options before allocating anything, so that a syntax error
    // is reported immediately. The holders free the dictionaries on every path.
    AVDictionaryHolder codecOptions;
    AVDictionaryHolder formatOptions;
    {
        StringParam* params[2] = { _codecOptions, _formatOptions };
        const char* labels[2] = { kParamCodecOptionsLabel, kParamFormatOptionsLabel };
        AVDictionary** dicts[2] = { codecOptions.ptr(), formatOptions.ptr() };
        for (int i = 0; i < 2; ++i) {
            string str;
            string error;
            params[i]->getValue(str);
            if (!parseOptionString(str, dicts[i], &error)) {
                setPersistentMessage(Message::eMessageError, "", string("Syntax error in ") + labels[i] + " \"" + str + "\": " + error);
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
        }
    }

    // Options that were not applied by the encoder or the muxer, reported once the file is open.
    string unrecognizedOptions;

    ////////////////////                        ////////////////////
    //////////////////// INITIALIZE FORMAT       ////////////////////

    _rodPixel = rodPixel;
    _pixelAspectRatio = pixelAspectRatio;

    const AVOutputFormat* avOutputFormat = initFormat(/* reportErrors = */ true, &filename);
    if (!avOutputFormat) {
        setPersistentMessage(Message::eMessageError, "", "Invalid file extension");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // check the start timecode before touching the output file
    string startTimecode;
    {
        string error;
        if (!getStartTimecode(avOutputFormat, &startTimecode, &error)) {
            setPersistentMessage(Message::eMessageError, "", error);
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
    }

    // If the file exists (which means "overwrite" was checked), remove it first.
    // See https://github.com/NatronGitHub/Natron/issues/666
    if (OFX::exists_utf8(filename.c_str())) {
        OFX::remove_utf8(filename.c_str());
    }

    // From here on, any failure (also an exception, e.g. from configureVideoStream()) frees the format and codec
    // contexts and closes the AVIOContext (a partially written file stays), so that endEncode() has nothing to finish
    // and the next render can begin (#289).
    ScopeExit freeFormatOnFailure([this] { freeFormat(); });

    assert(!_formatContext);
    if (avformat_alloc_output_context2(&_formatContext, avOutputFormat, nullptr, filename.c_str()) < 0) {
        setPersistentMessage(Message::eMessageError, "", "Unable to allocate the output format context");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    /////////////////////                            ////////////////////
    ////////////////////    INITIALISE STREAM     ////////////////////

#if OFX_FFMPEG_AUDIO
    // Create an audio stream if a file has been provided.
    if (_audioFile && (strlen(_audioFile) > 0)) {
        if (!_streamAudio.stream) {
            // Attempt to create an audio reader.
            audioReader_.reset(new AudioReader());

            // TODO: If the sample format is configurable via a knob, set
            //       the desired format here, e.g.:
            // audioReader_->setSampleFormat(_avSampleFormat);

            if (!audioReader_->open(_audioFile)) {
                const AVCodec* audioCodec = avcodec_find_encoder(audioReader_->getCodecID());
                addStream(_formatContext, audioCodec, &_streamAudio);
                if (!_streamAudio.stream) {
                    return false;
                }

                // Bug 45010 The following flags must be set BEFORE calling
                // openCodec (avcodec_open2). This will ensure that codec
                // specific data is created and initialized. (E.g. for MPEG4
                // the AVCodecContext::extradata will contain Elementary Stream
                // Descriptor which is required for QuickTime to decode the
                // stream.)
                AVCodecContext* avCodecContext = _streamAudio.codecContext;
                // Some formats want stream headers to be separate.
                if (_formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
                    // see fftools/ffmpeg_mux_init.c:1593 (n9.1), :1521 (n8.0)
                    avCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                }
                // Activate multithreaded decoding. This must be done before opening the codec; see
                // http://lists.gnu.org/archive/html/bino-list/2011-08/msg00019.html
                avCodecContext->thread_count = (std::min)((int)MultiThread::getNumCPUs(), OFX_FFMPEG_MAX_THREADS);

                avcodec_parameters_from_context(_streamAudio.stream->codecpar, _streamAudio.codecContext);

                if (openCodec(_formatContext, audioCodec, &_streamAudio) < 0) {
                    return false;
                }

                // If audio has been specified, set the start position (in seconds).
                // The values are negated to convert them from a video time to an
                // audio time.
                // So for a knob value of -10s, this means that the audio starts at
                // a video time of -10s. So this is converted to +10s audio time. I.e.
                // The video starts +10s into the audio. Vice-versa for a knob value
                // of +10s. The video starts -10s into the audio. In this case the
                // audio reader will provide 10s of silence for the first 10s of
                // video.
                audioReader_->setStartPosition(-((_audioOffsetUnit == 0) ? _audioOffset : (_audioOffset / fps_)));
            } else {
                setPersistentMessage(Message::eMessageError, "", "failed to open the audio file\nIt does not contain audio or is an invalid file");
                throwSuiteStatusException(kOfxStatFailed);

                return false;
            }
        }
    }
#endif // if OFX_FFMPEG_AUDIO

    // Create a video stream.
    AVCodecID codecId = AV_CODEC_ID_NONE;
    const AVCodec* videoCodec = nullptr;
    if (!initCodec(avOutputFormat, codecId, videoCodec)) {
        setPersistentMessage(Message::eMessageError, "", "Unable to find codec");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // Test if the container recognises the codec type.
    bool isCodecSupportedInContainer = codecCompatible(avOutputFormat, codecId);
    if (!isCodecSupportedInContainer) {
        setPersistentMessage(Message::eMessageError, "", string("The codec ") + videoCodec->name + " is not supported in container " + avOutputFormat->name);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // The v210 encoder needs an even width (FFmpeg n9.1 libavcodec/v210enc.c:56-59), and fails in avcodec_open2() with
    // "Invalid argument" otherwise. Its own message ("v210 needs even width") only goes to av_log(): reporting it
    // would need av_log_set_callback(), which is process-wide (it would replace the host's callback, and get the
    // messages of every thread), so this special case stays.
    const int width = rodPixel.x2 - rodPixel.x1;
    if ((codecId == AV_CODEC_ID_V210) && (width % 2 != 0)) {
        setPersistentMessage(Message::eMessageError, "", string("The v210 codec needs an even width, and the image is ") + std::to_string(width)
                             + " pixels wide: crop or pad the image to an even width, or choose another codec.");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    clearPersistentMessage();
    _encodeWarnings.clear();

    FFmpeg::PixelCodingEnum pixelCoding;
    int bitdepth;
    getPixelPreferences(&pixelCoding, &bitdepth);
    bool alpha = alphaEnabled();

    AVPixelFormat targetPixelFormat = AV_PIX_FMT_YUV422P;
    AVPixelFormat rgbBufferPixelFormat = AV_PIX_FMT_RGB24;
    bool alphaDropped = false;
    getPixelFormats(avOutputFormat, videoCodec, pixelCoding, bitdepth, alpha, rgbBufferPixelFormat, targetPixelFormat, &alphaDropped);
    // rawvideo RGB24 in QuickTime with an odd width: ARGB (see rawvideoPixelFormatForWidth())
    targetPixelFormat = rawvideoPixelFormatForWidth(avOutputFormat, videoCodec, targetPixelFormat, width);
    assert(!_streamVideo.stream);
    if (!_streamVideo.stream) {
        // the encoder selected in the Codec menu, as "ffmpeg -c:v name" (FFmpeg n9.1 fftools/ffmpeg_mux_init.c:105,
        // fftools/ffmpeg_opt.c:791), not the default encoder of its codec ID (libavcodec/allcodecs.c:993)
        addStream(_formatContext, videoCodec, &_streamVideo);
        if (!_streamVideo.stream) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }

        // The muxers read the "timecode" metadata when the output is initialized (avformat_init_output):
        // mov and mp4 write a timecode (tmcd) track for each video stream that has one (or for all
        // video streams, if it is set on the format context, as "ffmpeg -timecode" does).
        if (!startTimecode.empty()) {
            av_dict_set(&_formatContext->metadata, "timecode", startTimecode.c_str(), 0);
            av_dict_set(&_streamVideo.stream->metadata, "timecode", startTimecode.c_str(), 0);
        }

        AVCodecContext* avCodecContext = _streamVideo.codecContext;
        avCodecContext->pix_fmt = targetPixelFormat;
        setColorTags(avCodecContext, _streamVideo.stream, targetPixelFormat);

        avCodecContext->bits_per_raw_sample = FFmpeg::pixelFormatBitDepth(targetPixelFormat);
        avCodecContext->sample_aspect_ratio = av_d2q(pixelAspectRatio, 255);
        // configureVideoStream() set the stream's sample aspect ratio to QuickTime's value for the usual pixel aspect
        // ratios, or else to 1:1. avformat_write_header() fails ("Aspect ratio mismatch between muxer and encoder
        // layer") if the stream's and the codec's differ (by more than 0.4%), e.g. for a PAR of 16/15 (PAL SD,
        // 768/720): the stream gets the codec's, so that they always match.
        _streamVideo.stream->sample_aspect_ratio = avCodecContext->sample_aspect_ratio;

        // Now that the stream has been created, and the pixel format
        // is known, for DNxHD, set the YUV range.
        if (AV_CODEC_ID_DNXHD == avCodecContext->codec_id) {
            int encodeVideoRange = _encodeVideoRange->getValue();
            // Set the metadata for the YUV range. The mov muxer writes it in the 'ACLR' atom of the
            // video sample description: 1 for legal range, 2 for full range (libavformat/movenc.c says that they
            // correspond to "709" and "RGB" in the official encoder), and colourSpaceConvert() converts to
            // this range. FFmpeg's dnxhd encoder only declares the legal range (color_ranges), but it encodes
            // the samples as they are.
            // RGB (DNxHR 444, gbrp10) is tagged full range, since swscale writes RGB in full range (it does not
            // scale RGB to video levels).
            if (!FFmpeg::pixelFormatIsYUV(targetPixelFormat)) {
                avCodecContext->color_range = AVColorRange::AVCOL_RANGE_JPEG;
            } else {
                avCodecContext->color_range = encodeVideoRange ? AVColorRange::AVCOL_RANGE_MPEG : AVColorRange::AVCOL_RANGE_JPEG;
            }
        }
        // Uncompressed Y'CbCr (v210, and rawvideo in a Y'CbCr pixel format, e.g. v308, v408 and v410 in QuickTime):
        // colourSpaceConvert() converts to video range (the range is not full): tag the stream with it, so that
        // Matroska and NUT readers get it. The matrix is the one setColorTags() set above, as for every Y'CbCr
        // stream (colourSpaceConvert() converts with it), which the mov muxer writes in the 'colr' atom (FFmpeg
        // n9.0 libavformat/movenc.c:2648-2650).
        if (((AV_CODEC_ID_V210 == avCodecContext->codec_id) || (AV_CODEC_ID_RAWVIDEO == avCodecContext->codec_id))
            && FFmpeg::pixelFormatIsYUV(targetPixelFormat)) {
            avCodecContext->color_range = AVColorRange::AVCOL_RANGE_MPEG;
        }
        // An encoder that only writes full range Y'CbCr, e.g. mjpeg at the default strictness (FFmpeg n9.1
        // libavcodec/mjpegenc.c:666-680): colourSpaceConvert() converts to full range.
        if ((avCodecContext->color_range == AVCOL_RANGE_UNSPECIFIED) && FFmpeg::pixelFormatIsYUV(targetPixelFormat)) {
            const void* ranges = nullptr;
            int nRanges = 0;
            if ((avcodec_get_supported_config(avCodecContext, nullptr, AV_CODEC_CONFIG_COLOR_RANGE, 0, &ranges, &nRanges) >= 0)
                && (nRanges == 1) && (static_cast<const AVColorRange*>(ranges)[0] == AVCOL_RANGE_JPEG)) {
                avCodecContext->color_range = AVCOL_RANGE_JPEG;
            }
        }

        // Bug 45010 The following flags must be set BEFORE calling
        // openCodec (avcodec_open2). This will ensure that codec
        // specific data is created and initialized. (E.g. for MPEG4
        // the AVCodecContext::extradata will contain Elementary Stream
        // Descriptor which is required for QuickTime to decode the
        // stream.)
        // Some formats want stream headers to be separate.
        // see fftools/ffmpeg_mux_init.c:1593 (n9.1), :1521 (n8.0)
        if (_formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
            avCodecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        if (codecId == AV_CODEC_ID_PRORES) {
            int index = _codec->getValue();
            const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
            assert(index < (int)codecsShortNames.size());
            // avCodecContext->profile = getProfileFromShortName(codecsShortNames[index]);
            av_opt_set(avCodecContext->priv_data, "profile", getProfileStringFromShortName(codecsShortNames[index]), 0);
            av_opt_set(avCodecContext->priv_data, "bits_per_mb", "8000", 0);
            // ProRes vendor should be apl0
            // ref: https://ffmpeg.org/ffmpeg-all.html#toc-Private-Options-for-prores_002dks
            av_opt_set(avCodecContext->priv_data, "vendor", "apl0", 0);
        }
        // Activate multithreaded decoding. This must be done before opening the codec; see
        // http://lists.gnu.org/archive/html/bino-list/2011-08/msg00019.html
        avCodecContext->thread_count = (std::min)((int)MultiThread::getNumCPUs(), OFX_FFMPEG_MAX_THREADS);
        if (avCodecContext->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
            // multiple threads are used to decode a single frame. Reduces delay
            // also, mjpeg prefers this, see libavfocodec/frame_thread_encoder.c:ff_frame_thread_encoder_init()
            avCodecContext->thread_type = FF_THREAD_SLICE;
        }

#if OFX_FFMPEG_PRINT_CODECS
        std::cout << "Format: " << _formatContext->oformat->name << " Codec: " << videoCodec->name << " rgbBufferPixelFormat: " << av_get_pix_fmt_name(rgbBufferPixelFormat) << " targetPixelFormat: " << av_get_pix_fmt_name(targetPixelFormat) << " infoBitDepth: " << _infoBitDepth->getValue() << " Profile: " << _streamVideo->codec->profile << std::endl;
#endif //  FFMPEG_PRINT_CODECS
        // avcodec_open2() removes the options that it recognizes from the dictionary.
        AVDictionaryHolder codecOptionsLeft;
        av_dict_copy(codecOptionsLeft.ptr(), codecOptions.get(), 0);
        if (openCodec(_formatContext, videoCodec, &_streamVideo, codecOptionsLeft.ptr()) < 0) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        if (av_dict_count(codecOptionsLeft.get()) > 0) {
            unrecognizedOptions += string(kParamCodecOptionsLabel) + " not applied by the " + videoCodec->name + " encoder: " + dictToString(codecOptionsLeft.get()) + "\n";
        }
        {
            AVPixelFormat alphaFormat = AV_PIX_FMT_NONE;
            if (alphaDropped) {
                AVPixelFormat rgbFormat = AV_PIX_FMT_NONE;
                getPixelFormatsFromList(avOutputFormat, videoCodec, pixelCoding, bitdepth, true, rgbFormat, alphaFormat);
            }
            // shown again by endEncode(), since GenericWriterPlugin::render() clears the persistent message
            _encodeWarnings = encodeWarnings(avOutputFormat, videoCodec, pixelCoding, bitdepth, alpha, targetPixelFormat, alphaFormat);
            if (!_encodeWarnings.empty()) {
                setPersistentMessage(Message::eMessageWarning, "", _encodeWarnings);
            }
        }

        avcodec_parameters_from_context(_streamVideo.stream->codecpar, avCodecContext);

        if (!(avOutputFormat->flags & AVFMT_NOFILE)) {
            int error = avio_open(&_formatContext->pb, filename.c_str(), AVIO_FLAG_WRITE);
            if (error < 0) {
                // Report the error.
                char szError[1024] = { 0 };
                av_strerror(error, szError, sizeof(szError));
                setPersistentMessage(Message::eMessageError, "", string("Unable to open file: ") + szError);
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
        }

        std::string movflags = "write_colr";
        if (_fastStart->getValue()) {
            movflags += "+faststart";
        }

        // Muxer options (movflags: write_colr / faststart) must be passed to the muxer
        // initialization so they are actually applied; they are consumed during init.
        AVDictionaryHolder header_params;
        av_dict_set(header_params.ptr(), "movflags", movflags.c_str(), 0);

        // The user format options override the plug-in options, except that a relative movflags
        // value ("+flag" or "-flag") is appended to the plug-in movflags.
        for (const AVDictionaryEntry* e = av_dict_iterate(formatOptions.get(), nullptr); e; e = av_dict_iterate(formatOptions.get(), e)) {
            if (!strcmp(e->key, "movflags") && (e->value[0] == '+' || e->value[0] == '-')) {
                av_dict_set(header_params.ptr(), e->key, (movflags + e->value).c_str(), 0);
            } else {
                av_dict_set(header_params.ptr(), e->key, e->value, 0);
            }
        }
        const bool hasFormatOptions = av_dict_count(formatOptions.get()) > 0;

        // avformat_init_output may set the "encoder" metadata (see libavformat/muc.c:init_muxer)
        // It applies the options and removes the recognized ones from header_params
        // (avformat_write_header does not initialize the muxer again).
        int error = avformat_init_output(_formatContext, header_params.ptr());
        if (error < 0) {
            // Report the error.
            char szError[1024] = { 0 };
            av_strerror(error, szError, sizeof(szError));
            setPersistentMessage(Message::eMessageError, "", string("Unable to initialize output: ") + szError + (hasFormatOptions ? string("\nCheck the values in ") + kParamFormatOptionsLabel + "." : string()));
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        // Only report the user options: non-QuickTime muxers ignore the plug-in movflags.
        if (hasFormatOptions) {
            string left = dictToString(header_params.get(), formatOptions.get());
            if (!left.empty()) {
                unrecognizedOptions += string(kParamFormatOptionsLabel) + " not applied by the " + _formatContext->oformat->name + " muxer: " + left + "\n";
            }
        }

        error = avformat_write_header(_formatContext, header_params.ptr());
        if (error < 0) {
            // Report the error.
            char szError[1024] = { 0 };
            av_strerror(error, szError, sizeof(szError));
            setPersistentMessage(Message::eMessageError, "", string("Unable to write file header: ") + szError);
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }

        if (!unrecognizedOptions.empty()) {
            // Not a persistent message: GenericWriterPlugin::render() clears it after each frame.
            // This is sent during beginSequenceRender, and may pop up a dialog in interactive hosts.
            sendMessage(Message::eMessageWarning, "", unrecognizedOptions + "These options were ignored.");
        }

        // Special behaviour.
        // Valid on Aug 2014 for ffmpeg v2.1.4
        //
        // R.e. libavformat/movenc.c::mov_write_udta_tag
        // R.e. libavformat/movenc.c::mov_write_string_metadata
        //
        // Remove all ffmpeg references from the QuickTime movie.
        // The 'encoder' key in the AVFormatContext metadata will
        // result in the writer adding @swr with libavformat
        // version information in the 'udta' atom.
        //
        // Prevent the @swr libavformat reference from appearing
        // in the 'udta' atom by setting the 'encoder' key in the
        // metadata to null. From movenc.c a zero length value
        // will not be written to the 'udta' atom.
        //
        AVDictionaryEntry* tag = av_dict_get(_formatContext->metadata, "encoder", nullptr, AV_DICT_IGNORE_SUFFIX);
        if (tag) {
            av_dict_set(&_formatContext->metadata, "encoder", videoCodec->name, 0);
        }
    }

    // Set the stream encoder to proper values for ProRes, DNxHD/DNxHR, and others (used in MOV)
    // see:
    // - https://trac.ffmpeg.org/ticket/6465
    // - https://lists.ffmpeg.org/pipermail/ffmpeg-user/2015-May/026742.html
    // - https://forum.blackmagicdesign.com/viewtopic.php?f=21&t=51457#p355458
    const char* encoder = nullptr;
    if (codecId == AV_CODEC_ID_PRORES) {
        int index = _codec->getValue();
        const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
        assert(index < (int)codecsShortNames.size());
        switch (getProfileFromShortName(codecsShortNames[index])) {
        case kProresProfile4444XQ:
            encoder = "Apple ProRes 4444 (XQ)";
            break;
        case kProresProfile4444:
            encoder = "Apple ProRes 4444";
            break;
        case kProresProfileHQ:
            encoder = "Apple ProRes 422 (HQ)";
            break;
        case kProresProfileSQ:
            encoder = "Apple ProRes 422";
            break;
        case kProresProfileLT:
            encoder = "Apple ProRes 422 (LT)";
            break;
        case kProresProfileProxy:
            encoder = "Apple ProRes 422 (Proxy)";
            break;
        }
    }
    if (AV_CODEC_ID_DNXHD == videoCodec->id) {
        DNxHDCodecProfileEnum dnxhdCodecProfile = (DNxHDCodecProfileEnum)_dnxhdCodecProfile->getValue();
        switch (dnxhdCodecProfile) {
        case eDNxHDCodecProfileHR444:
            // http://users.mur.at/ms/attachments/dnxhr-444.mov
            encoder = "DNxHR 444";
            break;
        case eDNxHDCodecProfileHRHQX:
            // http://users.mur.at/ms/attachments/dnxhr-hqx.mov
            encoder = "DNxHR HQX";
            break;
        case eDNxHDCodecProfileHRHQ:
            encoder = "DNxHR HQ";
            break;
        case eDNxHDCodecProfileHRSQ:
            encoder = "DNxHR SQ";
            break;
        case eDNxHDCodecProfileHRLB:
            encoder = "DNxHR LB";
            break;
        case eDNxHDCodecProfile440x:
        case eDNxHDCodecProfile220x:
        case eDNxHDCodecProfile220:
        case eDNxHDCodecProfile145:
        case eDNxHDCodecProfile36:
            encoder = "DNxHD";
            break;
        }
    }
    if (AV_CODEC_ID_CINEPAK == videoCodec->id) {
        encoder = "Cinepak";
    }
    if (AV_CODEC_ID_SVQ1 == videoCodec->id) {
        encoder = "Sorenson Video";
    }
    if (AV_CODEC_ID_SVQ3 == videoCodec->id) {
        encoder = "Sorenson Video 3";
    }
    if (AV_CODEC_ID_MJPEG == videoCodec->id) {
        encoder = "Photo - JPEG";
    }
    if (AV_CODEC_ID_JPEG2000 == videoCodec->id) {
        encoder = "JPEG 2000";
    }
    if (AV_CODEC_ID_H263 == videoCodec->id) {
        encoder = "H.263";
    }
    if (AV_CODEC_ID_H264 == videoCodec->id) {
        encoder = "H.264";
    }
    if (AV_CODEC_ID_GIF == videoCodec->id) {
        encoder = "GIF";
    }
    if (AV_CODEC_ID_PNG == videoCodec->id) {
        encoder = "PNG";
    }
    if (AV_CODEC_ID_DPX == videoCodec->id) {
        encoder = "DPX";
    }
    if (AV_CODEC_ID_TARGA == videoCodec->id) {
        encoder = "TGA";
    }
    if (AV_CODEC_ID_TIFF == videoCodec->id) {
        encoder = "TIFF";
    }
    if (AV_CODEC_ID_QTRLE == videoCodec->id) {
        encoder = "Animation";
    }
    if (AV_CODEC_ID_8BPS == videoCodec->id) {
        encoder = "Planar RGB";
    }
    if (AV_CODEC_ID_SMC == videoCodec->id) {
        encoder = "Graphics";
    }
    if (AV_CODEC_ID_MSRLE == videoCodec->id) {
        encoder = "BMP";
    }
    if (AV_CODEC_ID_RPZA == videoCodec->id) {
        encoder = "Video";
    }
    // FFmpeg sets the encoder for XDCAM (see libavformat/movenc.c:find_compressor)
    if (encoder != nullptr) {
        av_dict_set(&_formatContext->metadata, "encoder", encoder, 0);
    }

    // Flag that we didn't encode any frame yet
    {
        std::lock_guard<std::mutex> guard(_nextFrameToEncodeMutex);
        _nextFrameToEncode = (int)args.frameRange.min;
        _firstFrameToEncode = (int)args.frameRange.min;
        _lastFrameToEncode = (int)args.frameRange.max;
        _frameStep = (int)args.frameStep;
        _nextFrameToEncodeCond.notify_all();
    }

    _isOpen = true;
    _lastDts = AV_NOPTS_VALUE;
    _packetsWritten = 0;
    _error = CLEANUP;
    freeFormatOnFailure.release();
} // WriteFFmpegPlugin::beginEncode

// If url is a local file for libavformat, set *path to it and return true, else return false
// (e.g. "pipe:1" or "udp://..."). As url_find_protocol() in libavformat/avio.c: the protocol is the
// leading URL_SCHEME_CHARS before a ':', "file" if there is none (or, on Windows, for "C:..."), and
// the file protocol strips the "file:" prefix (file_open() in libavformat/file.c).
static bool
urlToLocalPath(const string& url,
               string* path)
{
    const size_t n = url.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+-.");
    if ((n == string::npos) || (url[n] != ':')
#ifdef _WIN32
        || (n == 1) // DOS path
#endif
    ) {
        *path = url;

        return true;
    }
    if (url.compare(0, n, "file") == 0) {
        *path = url.substr(n + 1);

        return true;
    }

    return false;
}

// The canonical form of a path given in UTF-8 (as are OFX strings): absolute, with the symbolic links,
// "." and ".." resolved, even if the file (or its directory) does not exist yet.
static std::filesystem::path
canonicalPathUtf8(const string& utf8)
{
    namespace fs = std::filesystem;
#ifdef _WIN32
    const fs::path p(OFX::utf8_to_utf16(utf8));
#else
    const fs::path p(utf8);
#endif
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    if (!ec) {
        return c;
    }
    // e.g. a directory that can not be read: at least an absolute, lexically normal path
    c = fs::absolute(p, ec);

    return (ec ? p : c).lexically_normal();
}

// Whether the urls a and b (as given to avio_open()) are the same output: a file may be reached through
// different paths, e.g. /tmp/x.mov and /private/tmp/x.mov on macOS, where /tmp is a symbolic link.
static bool
isSameOutputUrl(const string& a,
                const string& b)
{
    if (a == b) {
        return true;
    }
    string pa, pb;
    if (!urlToLocalPath(a, &pa) || !urlToLocalPath(b, &pb)) {
        return false; // other protocols: only the same url
    }
    const std::filesystem::path ca = canonicalPathUtf8(pa);
    const std::filesystem::path cb = canonicalPathUtf8(pb);
#ifdef _WIN32
    // Windows file systems are case-insensitive
    const std::wstring& wa = ca.native();
    const std::wstring& wb = cb.native();

    return CompareStringOrdinal(wa.c_str(), (int)wa.size(), wb.c_str(), (int)wb.size(), TRUE) == CSTR_EQUAL;
#else
    if (ca.native() == cb.native()) {
        return true;
    }
    // the same existing file, e.g. through a hard link or on a case-insensitive file system (macOS)
    std::error_code ec;
    const bool same = std::filesystem::equivalent(ca, cb, ec);

    return !ec && same;
#endif
}

#define checkAvError()                                              \
    if (error < 0) {                                                \
        char errorBuf[1024];                                        \
        av_strerror(error, errorBuf, sizeof(errorBuf));             \
        setPersistentMessage(Message::eMessageError, "", errorBuf); \
        throwSuiteStatusException(kOfxStatFailed);                  \
        return;                                                     \
    }

void
WriteFFmpegPlugin::encode(const string& filename,
                          const OfxTime time,
                          const string& /*viewName*/,
                          const float* pixelData,
                          const OfxRectI& bounds,
                          const float pixelAspectRatio,
                          const int pixelDataNComps,
                          const int dstNCompsStartIndex,
                          const int dstNComps,
                          const int rowBytes)
{
    assert(dstNCompsStartIndex == 0);
    // the frame number, which _nextFrameToEncode is compared with
    const int frame = (int)std::lround(time);

    /// Check that we're really encoding in sequential order
    {
        std::unique_lock<std::mutex> guard(_nextFrameToEncodeMutex); // a unique_lock: _nextFrameToEncodeCond waits on it
        // However encode() ends, wake up the threads that wait for their turn below: they check _nextFrameToEncode
        // again, which is INT_MIN if this frame failed (#288).
        ScopeExit notifyWaitingThreads([this] { _nextFrameToEncodeCond.notify_all(); });

        // A frame before the next one was already encoded, or will never be: waiting for it would never end.
        // The frame that was just encoded is rendered again (only view 0 gets here, see encodesMainViewOnly()):
        // nothing to do; any other one is an error. A host doesn't do that: Natron calls beginSequenceRender
        // before, and endSequenceRender after, each render of a Write node, which renders its frames in order.
        // No other thread waits for such a frame, so the render goes on.
        if ((_nextFrameToEncode != INT_MIN) && (frame < _nextFrameToEncode)) {
            if (frame == _nextFrameToEncode - _frameStep) {
                return;
            }
            stringstream ss;
            ss << "Frame " << frame << " was rendered after frame " << (_nextFrameToEncode - _frameStep)
               << ": the frames of a movie must be rendered in increasing order";
            setPersistentMessage(Message::eMessageError, "", ss.str());
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }

        try {
            if ((dstNComps != 4) && (dstNComps != 3)) {
                setPersistentMessage(Message::eMessageError, "", "can only write RGBA or RGB components images");
                throwSuiteStatusException(kOfxStatErrFormat);

                return;
            }

            if (!_isOpen || !_formatContext) {
                setPersistentMessage(Message::eMessageError, "", "file is not open");
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            if (!pixelData || !_streamVideo.stream) {
                throwSuiteStatusException(kOfxStatErrBadHandle);

                return;
            }
            // beginEncode() opened the file param at beginSequenceRender, and the frames of a movie can only go
            // to that file: the param at this frame (if it is animated, or was changed during the render) must
            // name the same file, maybe through another path (#137).
            const string openUrl = _formatContext->url ? _formatContext->url : "";
            if (!isSameOutputUrl(filename, openUrl)) {
                stringstream ss;
                ss << "Trying to render " << filename << " but another active render is rendering " << openUrl;
                setPersistentMessage(Message::eMessageError, "", ss.str());
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }

            if (pixelAspectRatio != _pixelAspectRatio) {
                setPersistentMessage(Message::eMessageError, "", "all images in the sequence do not have the same pixel aspect ratio");
                throwSuiteStatusException(kOfxStatErrFormat);

                return;
            }

            // The frame this thread waits for may never come to encode(): GenericWriterPlugin::render() returns before
            // it if the render is aborted. Check abort() while waiting, with a predicate (C++ Core Guidelines CP.42,
            // https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconc-wait).
            const auto myTurnOrFailed = [this, frame] { return (_nextFrameToEncode == frame) || (_nextFrameToEncode == INT_MIN); };
            while (!_nextFrameToEncodeCond.wait_for(guard, std::chrono::milliseconds(100), myTurnOrFailed)) {
                if (abort()) {
                    _nextFrameToEncode = INT_MIN;
                }
            }

            if (_nextFrameToEncode == INT_MIN) {
                // Another thread aborted
                if (abort()) {
                    setPersistentMessage(Message::eMessageError, "", "Render aborted");
                }
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            _error = IGNORE_FINISH;

            if (_isOpen) {
                _error = CLEANUP;

                if (!_streamVideo.stream) {
                    throwSuiteStatusException(kOfxStatErrBadHandle);

                    return;
                }
                assert(_formatContext);
                if (!writeToFile(_formatContext, false, time, pixelData, &bounds, pixelDataNComps, dstNComps, rowBytes)) {
                    _error = SUCCESS;
                    _nextFrameToEncode = frame + _frameStep;
                    if (abort()) {
                        _nextFrameToEncode = INT_MIN;
                    }
                } else {
                    throwSuiteStatusException(kOfxStatFailed);

                    return;
                }
            }
        } catch (...) {
            // this frame will never be encoded: the threads that wait for the next ones fail (#288)
            _nextFrameToEncode = INT_MIN;
            // the exception itself, not a copy (C++ Core Guidelines E.15, https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#re-exception-ref)
            throw;
        }
    } // MultiThread::AutoMutex lock(_nextFrameToEncodeMutex);
} // WriteFFmpegPlugin::encode

////////////////////////////////////////////////////////////////////////////////
// finish
// Complete the encoding, finalise and close the file.
// Some video codecs may have data remaining in their encoding buffers. This
// must be 'flushed' to the file.
// This method flags the 'flush' to get the remaining encoded data out of the
// encoder(s) and into the file.
// Audio is written after video so it may require more writes from the audio
// stream in order to ensure that the duration of the audio and video tracks
// are the same.
//
void
WriteFFmpegPlugin::endEncode(const EndSequenceRenderArguments& /*args*/)
{
    if (!_formatContext) {
        return;
    }

    if (_error == IGNORE_FINISH) {
        freeFormat();

        return;
    }
    // Whether all the frames were encoded: if a frame failed (in GenericWriterPlugin::render(), or in encode()'s
    // checks, which do not change _error), its error is the persistent message, and must stay.
    bool allEncoded;
    {
        std::lock_guard<std::mutex> guard(_nextFrameToEncodeMutex);
        allEncoded = (_nextFrameToEncode != INT_MIN) && (_nextFrameToEncode > _lastFrameToEncode);
    }

    // Continue to write the audio/video interleave while there are still
    // frames in the video and/or audio encoder queues, without queuing any
    // new data to encode. This is ffmpeg specific.
    // writeToFile() returns 0 while it writes packets, -10 once the encoder is drained, and another negative value
    // if encoding or writing a packet failed (writeVideo() then set the persistent message, e.g. "Cannot write
    // frame: ..."): the file is still finalized and closed, but the render fails.
    int flushResult = 0;
    while (flushResult == 0) {
        flushResult = writeToFile(_formatContext, true, -1);
    }
    const bool flushFailed = (flushResult != -10);
    if (flushFailed && (_error == SUCCESS)) {
        _error = CLEANUP;
    }
#if OFX_FFMPEG_AUDIO
    // The audio is written in ~0.5s chunks only when the video stream position
    // passes a multiple of 0.5s interval. Flushing the video encoder may result
    // in less than 0.5s of video being written to the file so at this point the
    // video duration may be longer than the audio duration. This final stage
    // writes enough audio samples to the file so that the duration of both
    // audio and video are equal.
    if (_streamAudio.stream && _streamVideo.stream) {
        // add frame duration to end pts so we dont discard last audio frame
        const double videoTime = (av_stream_get_end_pts(_streamVideo.stream) + av_rescale_q(1, _streamVideo.codecContext->time_base, _streamVideo.stream->time_base)) * av_q2d(_streamVideo.stream->time_base);
        // Determine the current audio stream time.
        double audioTime = _streamAudio->pts.val * av_q2d(_streamAudio->time_base);
        if (audioTime < videoTime) {
            int ret = 0;
            double sourceDuration = audioReader_->getDuration();
            while ((audioTime < videoTime) && (audioTime < sourceDuration) && !ret) {
                ret = writeAudio(_formatContext, &_streamAudio, true);
                audioTime = av_stream_get_end_pts(_streamAudio.stream) * av_q2d(_streamAudio.stream->time_base);
            }
        }
    }
#endif

    // Finalise the movie.
    int error = av_write_trailer(_formatContext);

    const int closeError = freeFormat();

    _pts_counter = 0;

    if (flushFailed) {
        // the error of the encoder or of the muxer is the persistent message
        throwSuiteStatusException(kOfxStatFailed);
    }
    if ((error >= 0) && (_error == SUCCESS) && (closeError < 0)) {
        // The file is closed after the trailer is written, and the last data may only be written now
        // (e.g. on a network share). Not reported if writing a frame failed (_error is CLEANUP), which
        // was reported already.
        char szError[1024] = { 0 };
        av_strerror(closeError, szError, sizeof(szError));
        setPersistentMessage(Message::eMessageError, "", string("Unable to close the file: ") + szError);
        throwSuiteStatusException(kOfxStatFailed);
    }
    if (error < 0) {
        // e.g. the moov atom of a mov/mp4 file could not be written: the file is unreadable
        char szError[1024] = { 0 };
        av_strerror(error, szError, sizeof(szError));
        setPersistentMessage(Message::eMessageError, "", string("Unable to finalize the file: ") + szError);
        throwSuiteStatusException(kOfxStatFailed);
    }
    if ((_error == SUCCESS) && allEncoded && (_packetsWritten == 0)) {
        // issue #129: every frame was rendered, but nothing was written to the file
        setPersistentMessage(Message::eMessageError, "", "No frame was written to the file");
        throwSuiteStatusException(kOfxStatFailed);
    }
    if ((closeError >= 0) && (_error == SUCCESS) && allEncoded && !_encodeWarnings.empty()) {
        // the warnings of beginEncode(), which GenericWriterPlugin::render() cleared after each frame
        setPersistentMessage(Message::eMessageWarning, "", _encodeWarnings);
    }
}

void
WriteFFmpegPlugin::setOutputFrameRate(double fps)
{
    _fps->setValue(fps);
}

void
WriteFFmpegPlugin::updateVisibility()
{
    // The advanced params are enabled/disabled based on the codec chosen and its capabilities.
    // We also investigated setting the defaults based on the codec defaults, however all current
    // codecs defaulted the same, and as a user experience it was pretty counter intuitive.
    // Check knob exists, to deal with cases where Nuke might not have updated underlying writer (#44774)
    //(we still want to use showPanel to update when loading from script and the like).
    int index = _codec->getValue();
    // assert(index < _codec->getNOptions());
    const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
    string codecShortName;

    _codecShortName->getValue(codecShortName);
    // assert(index < (int)codecsShortNames.size());
    //  codecShortName may be empty if this was configured in an old version
    if (!codecShortName.empty() && (((int)codecsShortNames.size() <= index) || (codecShortName != codecsShortNames[index]))) {
        _codecShortName->setIsSecret(false); // something may be wrong. Make it visible, at least
    } else {
        _codecShortName->setIsSecret(true);
        if (0 <= index && index < (int)codecsShortNames.size()) {
            codecShortName = codecsShortNames[index];
        }
    }

    const AVCodec* codec = avcodec_find_encoder_by_name(getCodecFromShortName(codecShortName));
    const CodecParams p = codec ? GetCodecSupportedParams(codec) : CodecParams();

    _crf->setIsSecretAndDisabled(!p.crf);
    _x26xSpeed->setIsSecretAndDisabled(!p.x26xSpeed);
    _qscale->setIsSecretAndDisabled(!p.qscale);
    _bitrate->setIsSecretAndDisabled(!p.bitrate);
    _bitrateTolerance->setIsSecretAndDisabled(!p.bitrateTol);
    _quality->setIsSecretAndDisabled(!p.qrange);

    bool setqscale = p.qscale;
    bool setbitrate = p.bitrate;

    if (p.crf && (CRFEnum)_crf->getValue() != eCRFNone) {
        setqscale = setbitrate = false;
    }

    if (setqscale && _qscale->getValue() >= 0.) {
        setbitrate = false;
    }

    if (p.qscale && !setqscale) {
        _qscale->setEnabled(false);
    }

    if (p.bitrate && !setbitrate) {
        _bitrate->setEnabled(false);
        if (p.bitrateTol) {
            _bitrateTolerance->setEnabled(false);
        }
    }
    _gopSize->setIsSecretAndDisabled(!p.interGOP);
    _bFrames->setIsSecretAndDisabled(!p.interB);

    // We use the bitrate to set the min range for bitrate tolerance.
    updateBitrateToleranceRange();

    // Only enable the DNxHD codec profile knob if the Avid
    // DNxHD codec has been selected.
    // Only enable the video range knob if the Avid DNxHD codec
    // has been selected.
    bool isdnxhd = (!strcmp(codecShortName.c_str(), "dnxhd"));
    _dnxhdCodecProfile->setIsSecretAndDisabled(!isdnxhd);
    _encodeVideoRange->setIsSecretAndDisabled(!isdnxhd);

    bool ishap = (!strcmp(codecShortName.c_str(), "hap"));
    _hapFormat->setIsSecretAndDisabled(!ishap);

    /// Do not allow custom channel shuffling for the user, it's either RGB or RGBA
    for (int i = 0; i < 4; ++i) {
        _processChannels[i]->setIsSecretAndDisabled(true);
    }
    _outputComponents->setIsSecretAndDisabled(true);
} // WriteFFmpegPlugin::updateVisibility

////////////////////////////////////////////////////////////////////////////////
// updateBitrateToleranceRange
// Utility to update tolerance knob's slider range.
// Employed in place of chained knob_changed calls.
// Only valid for use from knob_changed.
//
void
WriteFFmpegPlugin::updateBitrateToleranceRange()
{
    // Bitrate tolerance should in theory be allowed down to target bitrate/target framerate.
    // We're not force limiting the range since the upper range is not bounded.
    double bitrate = _bitrate->getValue();
    double fps = _fps->getValue();
    // the 4 comes from the ffserver.c code
    double minRange = bitrateToleranceMin(bitrate, fps, kParamBitrateToleranceMax);

    _bitrateTolerance->setRange(minRange, kParamBitrateToleranceMax);
    _bitrateTolerance->setDisplayRange(minRange, kParamBitrateToleranceMax);
}

// The codec of the project that replaceMissingCodec() replaced (_codecReplaced), or an empty string.
string
WriteFFmpegPlugin::codecReplaced() const
{
    std::lock_guard<std::mutex> guard(_codecReplacedMutex);

    return _codecReplaced;
}

void
WriteFFmpegPlugin::setCodecReplaced(const string& codecShortName)
{
    std::lock_guard<std::mutex> guard(_codecReplacedMutex);
    _codecReplaced = codecShortName;
}

// A project saved with the v308, v408, v410 or ayuv encoder, which this FFmpeg does not have (FFmpeg 9 removed them:
// n8.0 libavcodec/allcodecs.c:354-359 lists v308, v408 and v410, n9.0 does not; FFmpeg 7 removed ayuv, which n6.1
// allcodecs.c:65-66 lists): select rawvideo, with the Pref. params that give the same pixel layout
// (rawvideoPixelFormats()): in QuickTime, the mov muxer writes it with the same v308, v408 or v410 tag
// (n8.0 libavformat/movenc.c:1963-1965), which the decoders of all versions read. In the other containers, rawvideo
// writes the closest pixel format that the container has (e.g. UYVY for v308 in Matroska, BGRA for v408 in AVI).
// These encoders only took one pixel format (yuv444p, yuva444p, yuv444p10), so the Pref. params of the project had no
// effect on them. The replacement is reported as a warning in the persistent message (codecReplacedMessage(), with the
// pixel format of the current container) until the user chooses a codec. Returns false if rawvideo is not available
// either.
bool
WriteFFmpegPlugin::replaceMissingCodec(const string& codecShortName)
{
    PrefBitDepthEnum bitDepth;
    bool alpha;
    if (codecShortName == "v308") {
        bitDepth = ePrefBitDepth8;
        alpha = false;
    } else if ((codecShortName == "v408") || (codecShortName == "ayuv")) {
        bitDepth = ePrefBitDepth8;
        alpha = true;
    } else if (codecShortName == "v410") {
        bitDepth = ePrefBitDepth10;
        alpha = false;
    } else {
        return false;
    }
    const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
    vector<string>::const_iterator it = find(codecsShortNames.begin(), codecsShortNames.end(), string("rawvideo"));
    if (it == codecsShortNames.end()) {
        return false;
    }
    setCodecReplaced(codecShortName);
    _codecShortName->setValue(*it);
    _prefPixelCoding->setValue((int)ePrefPixelCodingYUV444);
    _prefBitDepth->setValue((int)bitDepth);
    _prefAlpha->setValue(alpha);
    _codec->setValue((int)(it - codecsShortNames.begin()));
    updateVisibility();

    return true;
}

// Check that the secret codecShortName corresponds to the selected codec.
// It may change because different versions of FFmpeg support different codecs, so the choice menu may
// be different in different instances.
// This is also done in beginEdit() because setValue() cannot be called in the plugin constructor.
void
WriteFFmpegPlugin::checkCodec()
{
    int codec = _codec->getValue();
    const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();

    // assert(codec < (int)codecsShortNames.size());
    string codecShortName;

    _codecShortName->getValue(codecShortName);
    // codecShortName may be empty if this was configured in an old version
    if (!codecShortName.empty() && (((int)codecsShortNames.size() <= codec) || (codecShortName != codecsShortNames[codec]))) {
        // maybe it's another one but the label changed, if yes select it
        vector<string>::const_iterator it;

        it = find(codecsShortNames.begin(), codecsShortNames.end(), codecShortName);
        if (it != codecsShortNames.end()) {
            // found it! update the choice param
            codec = it - codecsShortNames.begin();
            _codec->setValue(codec);
            updateVisibility();
            // hide the codec name
            _codecShortName->setIsSecret(true);
        } else if (replaceMissingCodec(codecShortName)) {
            // the codec was replaced, see _codecReplaced
            _codecShortName->setIsSecret(true);
        } else {
            _codecShortName->setIsSecret(false);
            setPersistentMessage(Message::eMessageError, "", string("writer was configured for unavailable codec \"") + codecShortName + "\".");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
    } else if ((int)codecsShortNames.size() <= codec) {
        setPersistentMessage(Message::eMessageError, "", "writer was configured for unavailable codec.");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
}

// http://stackoverflow.com/questions/3418231/replace-part-of-a-string-with-another-string
static void
replaceAll(string& str, const string& from, const string& to)
{
    if (from.empty()) {
        return;
    }
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

// chop "le"/"be" at the end, because we don't care about endianness,
// and remove the "p" because we do't care about the format being planar or not
static string
pix_fmt_name_canonical(const char* name)
{
    if (name == nullptr) {
        return "unknown";
    }
    string ret = name;
    std::size_t len = ret.size();
    if (len > 2 && ret[len - 1] == 'e' && (ret[len - 2] == 'b' || ret[len - 2] == 'l')) {
        ret.resize(len - 2);
    }
    std::size_t found = ret.find('p');
    if (found != string::npos) {
        if (found == ret.size() - 1) {
            ret.erase(found, 1);
        } else if ('0' <= ret[found + 1] && ret[found + 1] <= '9') { // p10 p12 ...
            ret[found] = '/';
        }
    }
    for (string::iterator it = ret.begin(); it != ret.end(); ++it) {
        // avoid using toupper and the whole locale stuff
        if (*it >= 'a' && *it <= 'z') {
            *it = (*it - 'a') + 'A';
        }
    }
    // replace BGR by RGB
    replaceAll(ret, "BGR", "RGB");
    // replace GBR by RGB
    replaceAll(ret, "GBR", "RGB");
    // replace ARGB by RGBA
    replaceAll(ret, "ARGB", "RGBA");
    // replace AYUV by YUVA
    replaceAll(ret, "AYUV", "YUVA");

    return ret;
}

// Called from changedParam(), changedClip() or syncPrivateData(), maybe during a render: it owns nothing that outlives
// the call, and must not call freeFormat(), which would free the format and codec contexts of the render (#304).
void
WriteFFmpegPlugin::updatePixelFormat()
{
    // first, check that the codec setting is OK
    checkCodec();

    ////////////////////                        ////////////////////
    //////////////////// INITIALIZE FORMAT       ////////////////////

    const AVOutputFormat* avOutputFormat = initFormat(/* reportErrors = */ true);
    if (!avOutputFormat) {
        _infoPixelFormat->setValue("none");
        _infoBitDepth->setValue(0);
        _infoBPP->setValue(0);
        // setPersistentMessage(Message::eMessageError, "", "Invalid file extension");
        // throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    /////////////////////                            ////////////////////
    ////////////////////    INITIALISE STREAM     ////////////////////

    // Create a video stream.
    AVCodecID codecId = AV_CODEC_ID_NONE;
    const AVCodec* videoCodec = nullptr;
    if (!initCodec(avOutputFormat, codecId, videoCodec)) {
        updatePersistentMessage(); // "Unable to find codec", and the other errors
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // Test if the container recognises the codec type.
    bool isCodecSupportedInContainer = codecCompatible(avOutputFormat, codecId);
    if (!isCodecSupportedInContainer) {
        _infoPixelFormat->setValue("none");
        _infoBitDepth->setValue(0);
        _infoBPP->setValue(0);
        updatePersistentMessage(); // "The codec X is not supported in container Y", and the other errors
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    updatePersistentMessage(); // e.g. an invalid Start Timecode

    FFmpeg::PixelCodingEnum pixelCoding;
    int bitdepth;
    getPixelPreferences(&pixelCoding, &bitdepth);
    bool alpha = alphaEnabled();

    AVPixelFormat targetPixelFormat = AV_PIX_FMT_YUV422P;
    AVPixelFormat rgbBufferPixelFormat = AV_PIX_FMT_RGB24;
    getPixelFormats(avOutputFormat, videoCodec, pixelCoding, bitdepth, alpha, rgbBufferPixelFormat, targetPixelFormat);

    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(targetPixelFormat);
    _infoPixelFormat->setValue(desc ? pix_fmt_name_canonical(desc->name) : "unknown");
    _infoBitDepth->setValue(FFmpeg::pixelFormatBitDepth(targetPixelFormat));
    _infoBPP->setValue(FFmpeg::pixelFormatBPP(targetPixelFormat));
}

// Called from changedParam(), changedClip() or syncPrivateData(), maybe during a render: it owns nothing that outlives
// the call, and must not call freeFormat(), which would free the format and codec contexts of the render (#304).
void
WriteFFmpegPlugin::availPixelFormats()
{
    // first, check that the codec setting is OK
    checkCodec();

    ////////////////////                        ////////////////////
    //////////////////// INITIALIZE FORMAT       ////////////////////

    const AVOutputFormat* avOutputFormat = initFormat(/* reportErrors = */ true);
    if (!avOutputFormat) {
        sendMessage(Message::eMessageError, "", "Supported pixel codings:\nnone (cannot initialize container/format)", false);

        return;
    }

    /////////////////////                            ////////////////////
    ////////////////////    INITIALISE STREAM     ////////////////////

    // Create a video stream.
    AVCodecID codecId = AV_CODEC_ID_NONE;
    const AVCodec* videoCodec = nullptr;
    if (!initCodec(avOutputFormat, codecId, videoCodec)) {
        sendMessage(Message::eMessageError, "", "Supported pixel codings:\nnone (cannot initialize codec)", false);

        return;
    }

    // Test if the container recognises the codec type.
    bool isCodecSupportedInContainer = codecCompatible(avOutputFormat, codecId);
    if (!isCodecSupportedInContainer) {
        string ret = string("Supported pixel codings:\nnone (codec ") + videoCodec->name + " is not supported in container " + avOutputFormat->name + ")";
        sendMessage(Message::eMessageError, "", ret);

        return;
    }

    string ret("Supported pixel codings for codec ");
    ret += videoCodec->name;
    ret += " in container ";
    ret += avOutputFormat->name;
    ret += ":\n";

    const AVPixelFormat* const pixFormats = encoderPixelFormats(videoCodec, avOutputFormat);
    if (pixFormats == nullptr || *pixFormats == -1) {
        ret += "none";
    } else {
        const AVPixelFormat* currPixFormat = pixFormats;
        bool comma = false;
        while (*currPixFormat != -1) {
            if (comma) {
                ret += ", ";
            } else {
                comma = true;
            }

            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*currPixFormat);
            ret += (desc ? pix_fmt_name_canonical(desc->name) : "unknown");
            currPixFormat++;
        }
        ret += '.';
    }

    sendMessage(Message::eMessageMessage, "", ret, false);
}

// Check that the secret codecShortName corresponds to the selected codec.
// This is also done in beginEdit() because setValue() cannot be called in the plugin constructor.
void
WriteFFmpegPlugin::beginEdit()
{
    checkCodec();
}

void
WriteFFmpegPlugin::onOutputFileChanged(const string& filename,
                                       bool setColorSpace)
{
    if (setColorSpace) {
#ifdef OFX_IO_USING_OCIO
        // Unless otherwise specified, video files are assumed to be rec709.
        if (_ocio->hasColorspace("Rec709")) {
            // nuke-default
            _ocio->setOutputColorspace("Rec709");
        } else if (_ocio->hasColorspace("nuke_rec709")) {
            // blender
            _ocio->setOutputColorspace("nuke_rec709");
        } else if (_ocio->hasColorspace("Rec.709 - Full")) {
            // out_rec709full or "Rec.709 - Full" in aces 1.0.0
            _ocio->setOutputColorspace("Rec.709 - Full");
        } else if (_ocio->hasColorspace("out_rec709full")) {
            // out_rec709full or "Rec.709 - Full" in aces 1.0.0
            _ocio->setOutputColorspace("out_rec709full");
        } else if (_ocio->hasColorspace("rrt_rec709_full_100nits")) {
            // rrt_rec709_full_100nits in aces 0.7.1
            _ocio->setOutputColorspace("rrt_rec709_full_100nits");
        } else if (_ocio->hasColorspace("rrt_rec709")) {
            // rrt_rec709 in aces 0.1.1
            _ocio->setOutputColorspace("rrt_rec709");
        } else if (_ocio->hasColorspace("hd10")) {
            // hd10 in spi-anim and spi-vfx
            _ocio->setOutputColorspace("hd10");
        }
#endif
    }
    // Switch the 'format' knob based on the new filename suffix
    string suffix = filename.substr(filename.find_last_of(".") + 1);
    if (!suffix.empty()) {
        // downcase the suffix
        std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
        // Compare found suffix to known formats
        const vector<string>& formatsShortNames = FFmpegSingleton::Instance().getFormatsShortNames();
        for (size_t i = 0; i < formatsShortNames.size(); ++i) {
            bool setFormat = false;
            if (suffix.compare(formatsShortNames[i]) == 0) {
                setFormat = true;
                ;
            } else if (formatsShortNames[i] == "matroska") {
                if ((suffix.compare("mkv") == 0) || (suffix.compare("mk3d") == 0) || (suffix.compare("webm") == 0)) {
                    setFormat = true;
                }
            } else if (formatsShortNames[i] == "mpeg") {
                if (suffix.compare("mpg") == 0) {
                    setFormat = true;
                }
            } else if (formatsShortNames[i] == "mpegts") {
                if ((suffix.compare("m2ts") == 0) || (suffix.compare("mts") == 0) || (suffix.compare("ts") == 0)) {
                    setFormat = true;
                }
            } else if (formatsShortNames[i] == "mp4") {
                if ((suffix.compare("mov") == 0) || (suffix.compare("3gp") == 0) || (suffix.compare("3g2") == 0) || (suffix.compare("mj2") == 0)) {
                    setFormat = true;
                }
            }
            if (setFormat) {
                _format->setValue(i);
                const AVOutputFormat* fmt = av_guess_format(FFmpegSingleton::Instance().getFormatsShortNames()[i].c_str(), nullptr, nullptr);
                const vector<AVCodecID>& codecs = FFmpegSingleton::Instance().getCodecsIds();
                // is the current codec compatible with this format ?
                if (!codecCompatible(fmt, codecs[_codec->getValue()])) {
                    // set the default codec for this format, or the first compatible codec
                    enum AVCodecID default_video_codec = fmt->video_codec;
                    bool codecSet = false;
                    int compatible_codec = -1;
                    for (size_t c = 0; !codecSet && c < codecs.size(); ++c) {
                        if (codecs[c] == default_video_codec) {
                            _codec->setValue(c);
                            // exit loop
                            codecSet = true;
                        } else if ((compatible_codec == -1) && codecCompatible(fmt, codecs[c])) {
                            compatible_codec = c;
                        }
                    }
                    // if the default codec was not available, use the first compatible codec
                    if (!codecSet && (compatible_codec != -1)) {
                        _codec->setValue(compatible_codec);
                    }
                }
                // exit loop
                break;
                ;
            }
        }
    }
    // also check that the codec setting is OK
    checkCodec();
    updateVisibility();
} // WriteFFmpegPlugin::onOutputFileChanged

bool
WriteFFmpegPlugin::supportsAlpha(const std::string&) const
{
    string pf;
    _infoPixelFormat->getValue(pf);
    return kSupportsRGBA && (pf.find('A') != string::npos);
}

static string
ffmpeg_versions()
{
    std::ostringstream oss;
    oss << "FFmpeg ";
    oss << " versions (compiled with / running with):" << std::endl;
    oss << "libavformat ";
    oss << LIBAVFORMAT_VERSION_MAJOR << '.' << LIBAVFORMAT_VERSION_MINOR << '.' << LIBAVFORMAT_VERSION_MICRO << " / ";
    oss << (avformat_version() >> 16) << '.' << (avformat_version() >> 8 & 0xff) << '.' << (avformat_version() & 0xff) << std::endl;
    // oss << "libavdevice ";
    // oss << LIBAVDEVICE_VERSION_MAJOR << '.' << LIBAVDEVICE_VERSION_MINOR << '.' << LIBAVDEVICE_VERSION_MICRO << " / ";
    // oss << avdevice_version() >> 16 << '.' << avdevice_version() >> 8 & 0xff << '.' << avdevice_version() & 0xff << std::endl;
    oss << "libavcodec ";
    oss << LIBAVCODEC_VERSION_MAJOR << '.' << LIBAVCODEC_VERSION_MINOR << '.' << LIBAVCODEC_VERSION_MICRO << " / ";
    oss << (avcodec_version() >> 16) << '.' << (avcodec_version() >> 8 & 0xff) << '.' << (avcodec_version() & 0xff) << std::endl;
    oss << "libavutil ";
    oss << LIBAVUTIL_VERSION_MAJOR << '.' << LIBAVUTIL_VERSION_MINOR << '.' << LIBAVUTIL_VERSION_MICRO << " / ";
    oss << (avutil_version() >> 16) << '.' << (avutil_version() >> 8 & 0xff) << '.' << (avutil_version() & 0xff) << std::endl;
    oss << "libswscale ";
    oss << LIBSWSCALE_VERSION_MAJOR << '.' << LIBSWSCALE_VERSION_MINOR << '.' << LIBSWSCALE_VERSION_MICRO << " / ";
    oss << (swscale_version() >> 16) << '.' << (swscale_version() >> 8 & 0xff) << '.' << (swscale_version() & 0xff) << std::endl;

    return oss.str();
}

void
WriteFFmpegPlugin::changedParam(const InstanceChangedArgs& args,
                                const string& paramName)
{
    if (paramName == kParamLibraryInfo) {
        sendMessage(Message::eMessageMessage, "", ffmpeg_versions());
    } else if (paramName == kParamCodec) {
        // update the secret parameter
        int codec = _codec->getValue();
        const vector<string>& codecsShortNames = FFmpegSingleton::Instance().getCodecsShortNames();
        assert(codec < (int)codecsShortNames.size());
        _codecShortName->setValue(codecsShortNames[codec]);
        if (args.reason == eChangeUserEdit) {
            // the user chose a codec: the replacement of a missing codec (checkCodec()) is no longer reported
            setCodecReplaced(string());
        }
        updateVisibility();
        updatePixelFormat(); // also reports a codec that the container does not support
    } else if (paramName == kParamFormat) {
        updatePixelFormat(); // also reports a codec that the container does not support
    } else if ((paramName == kParamFPS) || (paramName == kParamBitrate)) {
        updateBitrateToleranceRange();
    } else if (paramName == kParamStartTimecode) {
        // checked below
    } else if ((paramName == kParamResetFPS) && (args.reason == eChangeUserEdit)) {
        double fps = _inputClip->getFrameRate();
        _fps->setValue(fps);
        updateBitrateToleranceRange();
    } else if ((paramName == kParamQuality) && (args.reason == eChangeUserEdit)) {
        int qMin, qMax;
        _quality->getValue(qMin, qMax);
        if (qMax >= 0 && qMin >= 0 && qMax < qMin) {
            // reorder
            _quality->setValue(qMax, qMin);
        }
    } else if (paramName == kParamDNxHDCodecProfile || paramName == kParamPrefPixelCoding || paramName == kParamPrefBitDepth || paramName == kParamPrefAlpha || paramName == kParamHapFormat) {
        updatePixelFormat();
    } else if (paramName == kParamPrefShow) {
        availPixelFormats();
    } else {
        GenericWriterPlugin::changedParam(args, paramName);
    }

    if (args.reason == eChangeUserEdit && (paramName == kParamCodec || paramName == kParamCodecShortName || paramName == kParamCRF || paramName == kParamX26xSpeed || paramName == kParamQScale || paramName == kParamBitrate || paramName == kParamBitrateTolerance || paramName == kParamQuality || paramName == kParamGopSize || paramName == kParamBFrames || paramName == kParamWriteNCLC || paramName == kParamDNxHDCodecProfile || paramName == kParamHapFormat)) {
        // for example, bitrate must be enabled when CRF goes to None or QScale goes to -1
        updateVisibility();
    }
    // also check that the codec setting is OK
    checkCodec();

    // Report the errors in the settings (e.g. an invalid Start Timecode) right away, rather than only
    // when rendering. Any param may change them (e.g. the FPS, the Format, the Codec, or write_tmcd
    // in the Format Options), and the branches above may have cleared or replaced the message
    // (GenericWriterPlugin::changedParam() clears it).
    updatePersistentMessage();
}

void
WriteFFmpegPlugin::changedClip(const InstanceChangedArgs& /*args*/,
                               const string& clipName)
{
    if (clipName == kOfxImageEffectSimpleSourceClipName) {
        updatePixelFormat(); // alphaEnabled() checks the source clip
    }
}

// Returns the error of avio_close() (e.g. when the last data could not be written to a network
// share), or 0.
int
WriteFFmpegPlugin::freeFormat()
{
    int closeError = 0;
    if (_streamVideo.stream) {
        avcodec_free_context(&_streamVideo.codecContext);
        _streamVideo.codecContext = nullptr;
        _streamVideo.stream = nullptr;
        // free(_streamVideo);
        // _streamVideo = nullptr;
    }
#if OFX_FFMPEG_AUDIO
    if (_streamAudio.stream) {
        avcodec_free_context(&_streamAudio.codecContext);
        _streamAudio.codecContext = nullptr;
        _streamAudio.stream = nullptr;
        // free(_streamAudio);
        // _streamAudio = nullptr;
    }
#endif
    if (_formatContext) {
        if (!(_formatContext->oformat->flags & AVFMT_NOFILE)) {
            closeError = avio_close(_formatContext->pb);
        }
        avformat_free_context(_formatContext); // also cleans up the allocation by avformat_new_stream()
        _formatContext = nullptr;
    }
    if (_convertCtx) {
        sws_freeContext(_convertCtx);
        _convertCtx = nullptr;
    }
    _rgb16White = FFmpeg::kWhite16;
    {
        std::lock_guard<std::mutex> guard(_nextFrameToEncodeMutex);
        _nextFrameToEncode = INT_MIN;
        _firstFrameToEncode = 1;
        _lastFrameToEncode = 1;
        _frameStep = 1;
        _nextFrameToEncodeCond.notify_all();
    }
    _isOpen = false;

    return closeError;
}

mDeclareWriterPluginFactory(WriteFFmpegPluginFactory, {}, true);
static std::list<string>&
split(const string& s,
      char delim,
      std::list<string>& elems)
{
    stringstream ss(s);
    string item;

    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }

    return elems;
}

void
WriteFFmpegPluginFactory::load()
{
    _extensions.clear();
    {
        std::list<string> extensionsl;
        void* opaqueMuxerIter = nullptr;
        const AVOutputFormat* oFormat = nullptr;
        while ((oFormat = av_muxer_iterate(&opaqueMuxerIter))) { // DBG(std::printf("WriteFFmpeg: \"%s\", // %s (%s)\n", oFormat->extensions ? oFormat->extensions : oFormat->name, oFormat->name, oFormat->long_name));
            if (oFormat->extensions != nullptr) {
                string extStr(oFormat->extensions);
                split(extStr, ',', extensionsl);
            }
            {
                // name's format defines (in general) extensions
                // require to fix extension in LibAV/FFMpeg to don't use it.
                string extStr(oFormat->name);
                split(extStr, ',', extensionsl);
            }
        }

        // Hack: Add basic video container extensions
        // as some versions of LibAV doesn't declare properly all extensions...
        extensionsl.push_back("avi"); // AVI (Audio Video Interleaved)
        extensionsl.push_back("flv"); // flv (FLV (Flash Video))
        extensionsl.push_back("mkv"); // matroska,webm (Matroska / WebM)
        extensionsl.push_back("webm"); // matroska,webm (Matroska / WebM)
        extensionsl.push_back("mov"); // QuickTime / MOV
        extensionsl.push_back("mp4"); // MP4 (MPEG-4 Part 14)
        extensionsl.push_back("mpg"); // MPEG-1 Systems / MPEG program stream
        extensionsl.push_back("m2ts"); // mpegts (MPEG-TS (MPEG-2 Transport Stream))
        extensionsl.push_back("mts"); // mpegts (MPEG-TS (MPEG-2 Transport Stream))
        extensionsl.push_back("ts"); // mpegts (MPEG-TS (MPEG-2 Transport Stream))
        extensionsl.push_back("mxf"); // mxf (MXF (Material eXchange Format))

        // remove audio and subtitle-only formats
        const char* extensions_denylist[] = {
            "aa", // aa (Audible AA format files)
            "aac", // aac (raw ADTS AAC (Advanced Audio Coding))
            "ac3", // ac3 (raw AC-3)
            "act", // act (ACT Voice file format)
            // "adf", // adf (Artworx Data Format)
            "adp", "dtk", // adp (ADP) Audio format used on the Nintendo Gamecube.
            "adx", // adx (CRI ADX) Audio-only format used in console video games.
            "aea", // aea (MD STUDIO audio)
            "afc", // afc (AFC) Audio format used on the Nintendo Gamecube.
            "aiff", // aiff (Audio IFF)
            "amr", // amr (3GPP AMR)
            // "anm", // anm (Deluxe Paint Animation)
            "apc", // apc (CRYO APC)
            "ape", "apl", "mac", // ape (Monkey's Audio)
            // "apng", // apng (Animated Portable Network Graphics)
            "aqt", "aqtitle", // aqtitle (AQTitle subtitles)
            // "asf", // asf (ASF (Advanced / Active Streaming Format))
            // "asf_o", // asf_o (ASF (Advanced / Active Streaming Format))
            "ass", // ass (SSA (SubStation Alpha) subtitle)
            "ast", // ast (AST (Audio Stream))
            "au", // au (Sun AU)
            // "avi", // avi (AVI (Audio Video Interleaved))
            "avr", // avr (AVR (Audio Visual Research)) Audio format used on Mac.
            // "avs", // avs (AVS)
            // "bethsoftvid", // bethsoftvid (Bethesda Softworks VID)
            // "bfi", // bfi (Brute Force & Ignorance)
            "bin", // bin (Binary text)
            // "bink", // bink (Bink)
            "bit", // bit (G.729 BIT file format)
            // "bmv", // bmv (Discworld II BMV)
            "bfstm", "bcstm", // bfstm (BFSTM (Binary Cafe Stream)) Audio format used on the Nintendo WiiU (based on BRSTM).
            "brstm", // brstm (BRSTM (Binary Revolution Stream)) Audio format used on the Nintendo Wii.
            "boa", // boa (Black Ops Audio)
            // "c93", // c93 (Interplay C93)
            "caf", // caf (Apple CAF (Core Audio Format))
            // "cavsvideo", // cavsvideo (raw Chinese AVS (Audio Video Standard))
            // "cdg", // cdg (CD Graphics)
            // "cdxl,xl", // cdxl (Commodore CDXL video)
            // "cine", // cine (Phantom Cine)
            // "concat", // concat (Virtual concatenation script)
            // "data", // data (raw data)
            "302", "daud", // daud (D-Cinema audio)
            // "dfa", // dfa (Chronomaster DFA)
            // "dirac", // dirac (raw Dirac)
            // "dnxhd", // dnxhd (raw DNxHD (SMPTE VC-3))
            "dsf", // dsf (DSD Stream File (DSF))
            // "dsicin", // dsicin (Delphine Software International CIN)
            "dss", // dss (Digital Speech Standard (DSS))
            "dts", // dts (raw DTS)
            "dtshd", // dtshd (raw DTS-HD)
            // "dv,dif", // dv (DV (Digital Video))
            "dvbsub", // dvbsub (raw dvbsub)
            // "dxa", // dxa (DXA)
            // "ea", // ea (Electronic Arts Multimedia)
            // "cdata", // ea_cdata (Electronic Arts cdata)
            "eac3", // eac3 (raw E-AC-3)
            "paf", "fap", "epaf", // epaf (Ensoniq Paris Audio File)
            "ffm", // ffm (FFM (FFserver live feed))
            "ffmetadata", // ffmetadata (FFmpeg metadata in text)
            // "flm", // filmstrip (Adobe Filmstrip)
            "flac", // flac (raw FLAC)
            // "flic", // flic (FLI/FLC/FLX animation)
            // "flv", // flv (FLV (Flash Video))
            // "flv", // live_flv (live RTMP FLV (Flash Video))
            // "4xm", // 4xm (4X Technologies)
            // "frm", // frm (Megalux Frame)
            "g722", "722", // g722 (raw G.722)
            "tco", "rco", "g723_1", // g723_1 (G.723.1)
            "g729", // g729 (G.729 raw format demuxer)
            // "gif", // gif (CompuServe Graphics Interchange Format (GIF))
            "gsm", // gsm (raw GSM)
            // "gxf", // gxf (GXF (General eXchange Format))
            // "h261", // h261 (raw H.261)
            // "h263", // h263 (raw H.263)
            // "h26l,h264,264,avc", // h264 (raw H.264 video)
            // "hevc,h265,265", // hevc (raw HEVC video)
            // "hls,applehttp", // hls,applehttp (Apple HTTP Live Streaming)
            // "hnm", // hnm (Cryo HNM v4)
            // "ico", // ico (Microsoft Windows ICO)
            // "idcin", // idcin (id Cinematic)
            // "idf", // idf (iCE Draw File)
            // "iff", // iff (IFF (Interchange File Format))
            "ilbc", // ilbc (iLBC storage)
            // "image2", // image2 (image2 sequence)
            "image2pipe", // image2pipe (piped image2 sequence)
            "alias_pix", // alias_pix (Alias/Wavefront PIX image)
            "brender_pix", // brender_pix (BRender PIX image)
            // "cgi", // ingenient (raw Ingenient MJPEG)
            "ipmovie", // ipmovie (Interplay MVE)
            "sf", "ircam", // ircam (Berkeley/IRCAM/CARL Sound Format)
            "iss", // iss (Funcom ISS)
            // "iv8", // iv8 (IndigoVision 8000 video)
            // "ivf", // ivf (On2 IVF)
            "jacosub", // jacosub (JACOsub subtitle format)
            "jv", // jv (Bitmap Brothers JV)
            "latm", // latm (raw LOAS/LATM)
            "lmlm4", // lmlm4 (raw lmlm4)
            "loas", // loas (LOAS AudioSyncStream)
            "lrc", // lrc (LRC lyrics)
            // "lvf", // lvf (LVF)
            // "lxf", // lxf (VR native stream (LXF))
            // "m4v", // m4v (raw MPEG-4 video)
            "mka", "mks", // "mkv,mk3d,mka,mks", // matroska,webm (Matroska / WebM)
            "mgsts", // mgsts (Metal Gear Solid: The Twin Snakes)
            "microdvd", // microdvd (MicroDVD subtitle format)
            // "mjpg,mjpeg,mpo", // mjpeg (raw MJPEG video)
            "mlp", // mlp (raw MLP)
            // "mlv", // mlv (Magic Lantern Video (MLV))
            "mm", // mm (American Laser Games MM)
            "mmf", // mmf (Yamaha SMAF)
            "m4a", // "mov,mp4,m4a,3gp,3g2,mj2", // mov,mp4,m4a,3gp,3g2,mj2 (QuickTime / MOV)
            "mp2", "mp3", "m2a", "mpa", // mp3 (MP2/3 (MPEG audio layer 2/3))
            "mpc", // mpc (Musepack)
            "mpc8", // mpc8 (Musepack SV8)
            // "mpeg", // mpeg (MPEG-PS (MPEG-2 Program Stream))
            "mpegts", // mpegts (MPEG-TS (MPEG-2 Transport Stream))
            "mpegtsraw", // mpegtsraw (raw MPEG-TS (MPEG-2 Transport Stream))
            "mpegvideo", // mpegvideo (raw MPEG video)
            // "mjpg", // mpjpeg (MIME multipart JPEG)
            "txt", "mpl2", // mpl2 (MPL2 subtitles)
            "sub", "mpsub", // mpsub (MPlayer subtitles)
            "msnwctcp", // msnwctcp (MSN TCP Webcam stream)
            // "mtv", // mtv (MTV)
            // "mv", // mv (Silicon Graphics Movie)
            // "mvi", // mvi (Motion Pixels MVI)
            // "mxf", // mxf (MXF (Material eXchange Format))
            // "mxg", // mxg (MxPEG clip)
            // "v", // nc (NC camera feed)
            "nist", "sph", "nistsphere", // nistsphere (NIST SPeech HEader REsources)
            // "nsv", // nsv (Nullsoft Streaming Video)
            // "nut", // nut (NUT)
            // "nuv", // nuv (NuppelVideo)
            // "ogg", // ogg (Ogg)
            "oma", "omg", "aa3", // oma (Sony OpenMG audio)
            // "paf", // paf (Amazing Studio Packed Animation File)
            "al", "alaw", // alaw (PCM A-law)
            "ul", "mulaw", // mulaw (PCM mu-law)
            "f64be", // f64be (PCM 64-bit floating-point big-endian)
            "f64le", // f64le (PCM 64-bit floating-point little-endian)
            "f32be", // f32be (PCM 32-bit floating-point big-endian)
            "f32le", // f32le (PCM 32-bit floating-point little-endian)
            "s32be", // s32be (PCM signed 32-bit big-endian)
            "s32le", // s32le (PCM signed 32-bit little-endian)
            "s24be", // s24be (PCM signed 24-bit big-endian)
            "s24le", // s24le (PCM signed 24-bit little-endian)
            "s16be", // s16be (PCM signed 16-bit big-endian)
            "sw", "s16le", // s16le (PCM signed 16-bit little-endian)
            "sb", "s8", // s8 (PCM signed 8-bit)
            "u32be", // u32be (PCM unsigned 32-bit big-endian)
            "u32le", // u32le (PCM unsigned 32-bit little-endian)
            "u24be", // u24be (PCM unsigned 24-bit big-endian)
            "u24le", // u24le (PCM unsigned 24-bit little-endian)
            "u16be", // u16be (PCM unsigned 16-bit big-endian)
            "uw", "u16le", // u16le (PCM unsigned 16-bit little-endian)
            "ub", "u8", // u8 (PCM unsigned 8-bit)
            "pjs", // pjs (PJS (Phoenix Japanimation Society) subtitles)
            "pmp", // pmp (Playstation Portable PMP)
            "pva", // pva (TechnoTrend PVA)
            "pvf", // pvf (PVF (Portable Voice Format))
            "qcp", // qcp (QCP)
            // "r3d", // r3d (REDCODE R3D)
            // "yuv,cif,qcif,rgb", // rawvideo (raw video)
            "rt", "realtext", // realtext (RealText subtitle format)
            "rsd", "redspark", // redspark (RedSpark)
            // "rl2", // rl2 (RL2)
            // "rm", // rm (RealMedia)
            // "roq", // roq (id RoQ)
            // "rpl", // rpl (RPL / ARMovie)
            "rsd", // rsd (GameCube RSD)
            "rso", // rso (Lego Mindstorms RSO)
            "rtp", // rtp (RTP input)
            "rtsp", // rtsp (RTSP input)
            "smi", "sami", // sami (SAMI subtitle format)
            "sap", // sap (SAP input)
            "sbg", // sbg (SBaGen binaural beats script)
            "sdp", // sdp (SDP)
            // "sdr2", // sdr2 (SDR2)
            "film_cpk", // film_cpk (Sega FILM / CPK)
            "shn", // shn (raw Shorten)
            // "vb,son", // siff (Beam Software SIFF)
            // "sln", // sln (Asterisk raw pcm)
            "smk", // smk (Smacker)
            // "mjpg", // smjpeg (Loki SDL MJPEG)
            "smush", // smush (LucasArts Smush)
            "sol", // sol (Sierra SOL)
            "sox", // sox (SoX native)
            "spdif", // spdif (IEC 61937 (compressed data in S/PDIF))
            "srt", // srt (SubRip subtitle)
            "psxstr", // psxstr (Sony Playstation STR)
            "stl", // stl (Spruce subtitle format)
            "sub", "subviewer1", // subviewer1 (SubViewer v1 subtitle format)
            "sub", "subviewer", // subviewer (SubViewer subtitle format)
            "sup", // sup (raw HDMV Presentation Graphic Stream subtitles)
            // "swf", // swf (SWF (ShockWave Flash))
            "tak", // tak (raw TAK)
            "tedcaptions", // tedcaptions (TED Talks captions)
            "thp", // thp (THP)
            "tiertexseq", // tiertexseq (Tiertex Limited SEQ)
            "tmv", // tmv (8088flex TMV)
            "thd", "truehd", // truehd (raw TrueHD)
            "tta", // tta (TTA (True Audio))
            // "txd", // txd (Renderware TeXture Dictionary)
            "ans", "art", "asc", "diz", "ice", "nfo", "txt", "vt", // tty (Tele-typewriter)
            // "vc1", // vc1 (raw VC-1)
            "vc1test", // vc1test (VC-1 test bitstream)
            // "viv", // vivo (Vivo)
            "vmd", // vmd (Sierra VMD)
            "idx", "vobsub", // vobsub (VobSub subtitle format)
            "voc", // voc (Creative Voice)
            "txt", "vplayer", // vplayer (VPlayer subtitles)
            "vqf", "vql", "vqe", // vqf (Nippon Telegraph and Telephone Corporation (NTT) TwinVQ)
            "w64", // w64 (Sony Wave64)
            "wav", // wav (WAV / WAVE (Waveform Audio))
            "wc3movie", // wc3movie (Wing Commander III movie)
            "webm_dash_manifest", // webm_dash_manifest (WebM DASH Manifest)
            "vtt", "webvtt", // webvtt (WebVTT subtitle)
            "wsaud", // wsaud (Westwood Studios audio)
            "wsvqa", // wsvqa (Westwood Studios VQA)
            "wtv", // wtv (Windows Television (WTV))
            "wv", // wv (WavPack)
            "xa", // xa (Maxis XA)
            "xbin", // xbin (eXtended BINary text (XBIN))
            "xmv", // xmv (Microsoft XMV)
            "xwma", // xwma (Microsoft xWMA)
            // "yop", // yop (Psygnosis YOP)
            // "y4m", // yuv4mpegpipe (YUV4MPEG pipe)
            "bmp_pipe", // bmp_pipe (piped bmp sequence)
            "dds_pipe", // dds_pipe (piped dds sequence)
            "dpx_pipe", // dpx_pipe (piped dpx sequence)
            "exr_pipe", // exr_pipe (piped exr sequence)
            "j2k_pipe", // j2k_pipe (piped j2k sequence)
            "jpeg_pipe", // jpeg_pipe (piped jpeg sequence)
            "jpegls_pipe", // jpegls_pipe (piped jpegls sequence)
            "pictor_pipe", // pictor_pipe (piped pictor sequence)
            "png_pipe", // png_pipe (piped png sequence)
            "qdraw_pipe", // qdraw_pipe (piped qdraw sequence)
            "sgi_pipe", // sgi_pipe (piped sgi sequence)
            "sunrast_pipe", // sunrast_pipe (piped sunrast sequence)
            "tiff_pipe", // tiff_pipe (piped tiff sequence)
            "webp_pipe", // webp_pipe (piped webp sequence)

            // OIIO and PFM extensions:
            "bmp", "cin", /*"dds",*/ "dpx", /*"f3d",*/ "fits", "hdr", "ico",
            "iff", "jpg", "jpe", "jpeg", "jif", "jfif", "jfi", "jp2", "j2k", "exr", "png",
            "pbm", "pgm", "ppm",
            "pfm",
            "psd", "pdd", "psb", /*"ptex",*/ "rla", "sgi", "rgb", "rgba", "bw", "int", "inta", "pic", "tga", "tpic", "tif", "tiff", "tx", "env", "sm", "vsm", "zfile",

            nullptr
        };
        for (const char* const* e = extensions_denylist; *e != nullptr; ++e) {
            extensionsl.remove(*e);
        }

        _extensions.assign(extensionsl.begin(), extensionsl.end());
        // sort / unique
        std::sort(_extensions.begin(), _extensions.end());
        _extensions.erase(std::unique(_extensions.begin(), _extensions.end()), _extensions.end());
    }
} // WriteFFmpegPluginFactory::load

/** @brief The basic describe function, passed a plugin descriptor */
void
WriteFFmpegPluginFactory::describe(ImageEffectDescriptor& desc)
{
    GenericWriterDescribe(desc, eRenderFullySafe, _extensions, kPluginEvaluation, false, false);
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginDescription(kPluginDescription);

    /// This plug-in only supports sequential render
    desc.setSequentialRender(true);
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
WriteFFmpegPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                            ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor* page = GenericWriterDescribeInContextBegin(desc, context,
                                                                    kSupportsRGBA, kSupportsRGB, kSupportsAlpha, kSupportsXY,
                                                                    "scene_linear", "rec709", false);

    /// If the host doesn't support sequential render, fail.
    int hostSequentialRender = getImageEffectHostDescription()->sequentialRender;

    if (hostSequentialRender == 0) {
        // throwSuiteStatusException(kOfxStatErrMissingHostFeature);
    }

    ///////////Output format
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamFormat);
        const vector<string>& formatsV = FFmpegSingleton::Instance().getFormatsLongNames();
        const vector<string>& formatsVs = FFmpegSingleton::Instance().getFormatsShortNames();
        const vector<vector<size_t>>& formatsCodecs = FFmpegSingleton::Instance().getFormatsCodecs();
        const vector<string>& codecsV = FFmpegSingleton::Instance().getCodecsShortNames();
        param->setLabel(kParamFormatLabel);
        param->setHint(kParamFormatHint);
        for (unsigned int i = 0; i < formatsV.size(); ++i) {
            if (formatsCodecs[i].empty()) {
                param->appendOption(formatsV[i], "", formatsVs[i]);
            } else {
                string hint = "Compatible with ";
                for (unsigned int j = 0; j < formatsCodecs[i].size(); ++j) {
                    if (j != 0) {
                        hint.append(", ");
                    }
                    hint.append(codecsV[formatsCodecs[i][j]]);
                }
                hint.append(".");
                param->appendOption(formatsV[i], hint, formatsVs[i]);
            }
        }
        param->setAnimates(false);
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Codec
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamCodec);
        param->setLabel(kParamCodecName);
        param->setHint(kParamCodecHint);
        const vector<string>& codecsV = FFmpegSingleton::Instance().getCodecsKnobLabels(); // getCodecsLongNames();
        const vector<string>& codecsVs = FFmpegSingleton::Instance().getCodecsShortNames();
        const vector<vector<size_t>>& codecsFormats = FFmpegSingleton::Instance().getCodecsFormats();
        const vector<string>& formatsV = FFmpegSingleton::Instance().getFormatsShortNames();
        for (unsigned int i = 0; i < codecsV.size(); ++i) {
            if (codecsFormats[i].empty()) {
                param->appendOption(codecsV[i], "", codecsVs[i]);
            } else {
                string hint = "Compatible with ";
                for (unsigned int j = 0; j < codecsFormats[i].size(); ++j) {
                    if (j != 0) {
                        hint.append(", ");
                    }
                    hint.append(formatsV[codecsFormats[i][j]]);
                }
                hint.append(".");
                param->appendOption(codecsV[i], hint, codecsVs[i]);
            }
        }
        param->setAnimates(false);
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }

    // codecShortName: a secret parameter that holds the real codec name
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamCodecShortName);
        param->setLabel(kParamCodecShortNameLabel);
        param->setHint(kParamCodecShortNameHint);
        param->setAnimates(false);
        param->setEnabled(false); // non-editable
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////FPS
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kParamFPS);
        param->setLabel(kParamFPSLabel);
        param->setHint(kParamFPSHint);
        param->setRange(0., INT_MAX);
        param->setDisplayRange(0., 120.);
        param->setDefault(24.); // should be set from the input FPS
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamResetFPS);
        param->setLabel(kParamResetFPSLabel);
        param->setHint(kParamResetFPSHint);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamPrefPixelCoding);
        param->setLabelAndHint(kParamPrefPixelCodingLabel);
        assert(param->getNOptions() == (int)ePrefPixelCodingYUV420);
        param->appendOption(kParamPrefPixelCodingOptionYUV420);
        assert(param->getNOptions() == (int)ePrefPixelCodingYUV422);
        param->appendOption(kParamPrefPixelCodingOptionYUV422);
        assert(param->getNOptions() == (int)ePrefPixelCodingYUV444);
        param->appendOption(kParamPrefPixelCodingOptionYUV444);
        assert(param->getNOptions() == (int)ePrefPixelCodingRGB);
        param->appendOption(kParamPrefPixelCodingOptionRGB);
        assert(param->getNOptions() == (int)ePrefPixelCodingXYZ);
        param->appendOption(kParamPrefPixelCodingOptionXYZ);

        param->setAnimates(false);
        param->setDefault((int)ePrefPixelCodingYUV422);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamPrefBitDepth);
        param->setLabelAndHint(kParamPrefBitDepthLabel);
        assert(param->getNOptions() == (int)ePrefBitDepth8);
        param->appendOption("8", "", "8");
        assert(param->getNOptions() == (int)ePrefBitDepth10);
        param->appendOption("10", "", "10");
        assert(param->getNOptions() == (int)ePrefBitDepth12);
        param->appendOption("12", "", "12");
        assert(param->getNOptions() == (int)ePrefBitDepth16);
        param->appendOption("16", "", "16");
        param->setAnimates(false);
        param->setDefault((int)ePrefBitDepth8);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamPrefAlpha);
        param->setLabelAndHint(kParamPrefAlphaLabel);
        param->setAnimates(false);
        param->setDefault(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamPrefShow);
        param->setLabelAndHint(kParamPrefShowLabel);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamDNxHDCodecProfile);
        param->setLabel(kParamDNxHDCodecProfileLabel);
        param->setHint(kParamDNxHDCodecProfileHint);
        // DNxHR
        assert(param->getNOptions() == (int)eDNxHDCodecProfileHR444);
        param->appendOption(kParamDNxHDCodecProfileOptionHR444);
        assert(param->getNOptions() == (int)eDNxHDCodecProfileHRHQX);
        param->appendOption(kParamDNxHDCodecProfileOptionHRHQX);
        assert(param->getNOptions() == (int)eDNxHDCodecProfileHRHQ);
        param->appendOption(kParamDNxHDCodecProfileOptionHRHQ);
        assert(param->getNOptions() == (int)eDNxHDCodecProfileHRSQ);
        param->appendOption(kParamDNxHDCodecProfileOptionHRSQ);
        assert(param->getNOptions() == (int)eDNxHDCodecProfileHRLB);
        param->appendOption(kParamDNxHDCodecProfileOptionHRLB);
        // DNxHD
        assert(param->getNOptions() == (int)eDNxHDCodecProfile440x);
        param->appendOption(kParamDNxHDCodecProfileOption440x);
        assert(param->getNOptions() == (int)eDNxHDCodecProfile220x);
        param->appendOption(kParamDNxHDCodecProfileOption220x);
        assert(param->getNOptions() == (int)eDNxHDCodecProfile220);
        param->appendOption(kParamDNxHDCodecProfileOption220);
        assert(param->getNOptions() == (int)eDNxHDCodecProfile145);
        param->appendOption(kParamDNxHDCodecProfileOption145);
        assert(param->getNOptions() == (int)eDNxHDCodecProfile36);
        param->appendOption(kParamDNxHDCodecProfileOption36);

        param->setAnimates(false);
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamHapFormat);
        param->setLabelAndHint(kParamHapFormatLabel);
        // Hap
        assert(param->getNOptions() == (int)eHapFormatHap);
        param->appendOption(kParamHapFormatOptionHap);
        assert(param->getNOptions() == (int)eHapFormatHapAlpha);
        param->appendOption(kParamHapFormatOptionHapAlpha);
        assert(param->getNOptions() == (int)eHapFormatHapQ);
        param->appendOption(kParamHapFormatOptionHapQ);

        param->setAnimates(false);
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        StringParamDescriptor* param = desc.defineStringParam(kParamInfoPixelFormat);
        param->setLabelAndHint(kParamInfoPixelFormatLabel);
        param->setAnimates(false);
        param->setEnabled(false);
        param->setEvaluateOnChange(false);
        param->setIsPersistent(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamInfoBitDepth);
        param->setLabelAndHint(kParamInfoBitDepthLabel);
        param->setAnimates(false);
        param->setEnabled(false);
        param->setEvaluateOnChange(false);
        param->setIsPersistent(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamInfoBPP);
        param->setLabelAndHint(kParamInfoBPPLabel);
        param->setAnimates(false);
        param->setEnabled(false);
        param->setEvaluateOnChange(false);
        param->setIsPersistent(false);
        // param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Fast Start
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamFastStart);
        param->setLabel(kParamFastStartLabel);
        param->setHint(kParamFastStartHint);
        param->setDefault(false);
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Start Timecode
    // Defined after the existing parameters of the main page, so that they keep their order.
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamStartTimecode);
        param->setLabel(kParamStartTimecodeLabel);
        param->setHint(kParamStartTimecodeHint);
        param->setDefault("");
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }

    /////////// Advanced group
    {
        GroupParamDescriptor* group = desc.defineGroupParam(kParamAdvanced);
        group->setLabel(kParamAdvancedLabel);
        group->setOpen(false);
        if (page) {
            page->addChild(*group);
        }

        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamDNxHDEncodeVideoRange);
            param->setLabel(kParamDNxHDEncodeVideoRangeLabel);
            param->setHint(kParamDNxHDEncodeVideoRangeHint);
            param->appendOption(kParamDNxHDEncodeVideoRangeOptionFull);
            param->appendOption(kParamDNxHDEncodeVideoRangeOptionVideo);
            param->setAnimates(false);
            param->setDefault(1);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamCRF);
            param->setLabel(kParamCRFLabel);
            param->setHint(kParamCRFHint);
            assert(param->getNOptions() == eCRFNone);
            param->appendOption(kParamCRFOptionNone);
            assert(param->getNOptions() == eCRFLossless);
            param->appendOption(kParamCRFOptionLossless);
            assert(param->getNOptions() == eCRFPercLossless);
            param->appendOption(kParamCRFOptionPercLossless);
            assert(param->getNOptions() == eCRFHigh);
            param->appendOption(kParamCRFOptionHigh);
            assert(param->getNOptions() == eCRFMedium);
            param->appendOption(kParamCRFOptionMedium);
            assert(param->getNOptions() == eCRFLow);
            param->appendOption(kParamCRFOptionLow);
            assert(param->getNOptions() == eCRFVeryLow);
            param->appendOption(kParamCRFOptionVeryLow);

            param->setAnimates(false);
            param->setDefault((int)eCRFMedium);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamX26xSpeed);
            param->setLabel(kParamX26xSpeedLabel);
            param->setHint(kParamX26xSpeedHint);
            assert(param->getNOptions() == eX26xSpeedUltrafast);
            param->appendOption(kParamX26xSpeedOptionUltrafast);
            assert(param->getNOptions() == eX26xSpeedVeryfast);
            param->appendOption(kParamX26xSpeedOptionVeryfast);
            assert(param->getNOptions() == eX26xSpeedFaster);
            param->appendOption(kParamX26xSpeedOptionFaster);
            assert(param->getNOptions() == eX26xSpeedFast);
            param->appendOption(kParamX26xSpeedOptionFast);
            assert(param->getNOptions() == eX26xSpeedMedium);
            param->appendOption(kParamX26xSpeedOptionMedium);
            assert(param->getNOptions() == eX26xSpeedSlow);
            param->appendOption(kParamX26xSpeedOptionSlow);
            assert(param->getNOptions() == eX26xSpeedSlower);
            param->appendOption(kParamX26xSpeedOptionSlower);
            assert(param->getNOptions() == eX26xSpeedVeryslow);
            param->appendOption(kParamX26xSpeedOptionVerySlow);

            param->setAnimates(false);
            param->setDefault((int)eX26xSpeedMedium);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        // QScale
        {
            DoubleParamDescriptor* param = desc.defineDoubleParam(kParamQScale);
            param->setLabel(kParamQScaleLabel);
            param->setHint(kParamQScaleHint);
            param->setAnimates(false);
            param->setRange(-1, 100);
            param->setDefault(-1);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        ////////////Quality
        {
            Int2DParamDescriptor* param = desc.defineInt2DParam(kParamQuality);
            param->setLabel(kParamQualityLabel);
            param->setHint(kParamQualityHint);
            param->setRange(-1, -1, 100, 100);
            param->setDefault(-1, -1); // was (2,32), but (0,69) is the x264 default
            param->setDimensionLabels("min", "max");
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        ///////////bit-rate
        {
            DoubleParamDescriptor* param = desc.defineDoubleParam(kParamBitrate);
            param->setLabel(kParamBitrateLabel);
            param->setHint(kParamBitrateHint);
            param->setRange(0, DBL_MAX);
            param->setDisplayRange(0, kParamBitrateMax);
            param->setDefault(kParamBitrateDefault);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        ///////////bit-rate tolerance
        {
            DoubleParamDescriptor* param = desc.defineDoubleParam(kParamBitrateTolerance);
            param->setLabel(kParamBitrateToleranceLabel);
            param->setHint(kParamBitrateToleranceHint);
            param->setRange(0, kParamBitrateToleranceMax);
            param->setDefault(0);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        ///////////Gop size
        {
            IntParamDescriptor* param = desc.defineIntParam(kParamGopSize);
            param->setLabel(kParamGopSizeLabel);
            param->setHint(kParamGopSizeHint);
            param->setRange(-1, 30);
            param->setDefault(-1);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        ////////////B Frames
        {
            // @deprecated there is no libavcodec-wide limit on the number of B-frames
            // left for backward compatibility.
            IntParamDescriptor* param = desc.defineIntParam(kParamBFrames);
            param->setLabel(kParamBFramesLabel);
            param->setHint(kParamBFramesHint);
            // #define FF_MAX_B_FRAMES 16
            // @deprecated there is no libavcodec-wide limit on the number of B-frames
            param->setRange(-1, 16);
            param->setDefault(-1); // default is -1, see x264_defaults in libavcodec/libx264.c
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
        ///////////Write NCLC
        {
            BooleanParamDescriptor* param = desc.defineBooleanParam(kParamWriteNCLC);
            param->setLabel(kParamWriteNCLCLabel);
            param->setHint(kParamWriteNCLCHint);
            param->setDefault(true);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

#if OFX_FFMPEG_MBDECISION
        ////////////Macro block decision
        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamMBDecision);
            param->setLabel("Macro block decision mode");
            assert(param->getNOptions() == FF_MB_DECISION_SIMPLE);
            param->appendOption("FF_MB_DECISION_SIMPLE", "", "simple");
            assert(param->getNOptions() == FF_MB_DECISION_BITS);
            param->appendOption("FF_MB_DECISION_BITS", "", "bits");
            assert(param->getNOptions() == FF_MB_DECISION_RD);
            param->appendOption("FF_MB_DECISION_RD", "", "rd");
            param->setDefault(0);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
#endif
        ///////////Codec Options
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamCodecOptions);
            param->setLabel(kParamCodecOptionsLabel);
            param->setHint(kParamCodecOptionsHint);
            param->setStringType(eStringTypeSingleLine);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
        ///////////Format Options
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamFormatOptions);
            param->setLabel(kParamFormatOptionsLabel);
            param->setHint(kParamFormatOptionsHint);
            param->setStringType(eStringTypeSingleLine);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamLibraryInfo);
        param->setLabelAndHint(kParamLibraryInfoLabel);
        if (page) {
            page->addChild(*param);
        }
    }

    GenericWriterDescribeInContextEnd(desc, context, page);
} // WriteFFmpegPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
WriteFFmpegPluginFactory::createInstance(OfxImageEffectHandle handle,
                                         ContextEnum /*context*/)
{
    WriteFFmpegPlugin* ret = new WriteFFmpegPlugin(handle, _extensions);

    ret->restoreStateFromParams();

    return ret;
}

static WriteFFmpegPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

    OFXS_NAMESPACE_ANONYMOUS_EXIT
