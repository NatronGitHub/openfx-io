/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2020 The Natron Developers
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

#if (defined(_STDINT_H) || defined(_STDINT_H_) || defined(_MSC_STDINT_H_ ) ) && !defined(UINT64_C)
#warning "__STDC_CONSTANT_MACROS has to be defined before including <stdint.h>, this file will probably not compile."
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS // ...or stdint.h wont' define UINT64_C, needed by libavutil
#endif
#include "FFmpegFile.h"

#include <cmath>
#include <iostream>
#include <algorithm>

#include <ofxsImageEffect.h>
#include <ofxsMacros.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(WIN64)
#  include <windows.h> // for GetSystemInfo()
#define strncasecmp _strnicmp
#else
#  include <unistd.h> // for sysconf()
#endif

using namespace OFX;

using std::string;
using std::make_pair;

#define CHECK(x) \
    { \
        int error = x; \
        if (error < 0) { \
            setInternalError(error); \
            return; \
        } \
    } \

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
#if defined(WIN32) || defined(WIN64)
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
extensionCorrespondToImageFile(const string & ext)
{
    return (ext == "bmp" ||
            ext == "cin" ||
            ext == "dpx" ||
            ext == "exr" ||
            /*ext == "gif" ||*/
            ext == "jpeg" ||
            ext == "jpg" ||
            ext == "pix" ||
            ext == "png" ||
            ext == "ppm" ||
            ext == "ptx" ||
            ext == "rgb" ||
            ext == "rgba" ||
            ext == "tga" ||
            ext == "tiff" ||
            ext == "webp");
}

bool
FFmpegFile::isImageFile(const string & filename)
{
    ///find the last index of the '.' character
    size_t lastDot = filename.find_last_of('.');

    if (lastDot == string::npos) { //we reached the start of the file, return false because we can't determine from the extension
        return false;
    }
    ++lastDot;//< bypass the '.' character
    string ext;
    std::locale loc;
    while ( lastDot < filename.size() ) {
        ext.append( 1, std::tolower(filename.at(lastDot), loc) );
        ++lastDot;
    }

    return extensionCorrespondToImageFile(ext);
}

namespace {
struct FilterEntry
{
    const char* name;
    bool enableReader;
    bool enableWriter;
};

// Bug 11027 - Nuke write: ffmpeg codec fails has details on individual codecs

// For a full list of formats, define FN_FFMPEGWRITER_PRINT_CODECS in ffmpegWriter.cpp
const FilterEntry kFormatWhitelist[] =
{
    { "3gp",            true,  true },
    { "3g2",            true,  true },
    { "avi",            true,  true },
    { "dv",             true,  false },    // DV (Digital Video), no HD support
    { "flv",            true,  true },     // FLV (Flash Video), only used with flv codec. cannot be read in official Qt
    { "gif",            true,  true },     // GIF Animation
    { "h264",           true,  false },     // raw H.264 video. prefer a proper container (mp4, mov, avi)
    { "hevc",           true,  false },     // raw HEVC video. hevc codec cannot be read in official qt
    { "m4v",            true,  false },     // raw MPEG-4 video. prefer a proper container (mp4, mov, avi)
    { "matroska",       true,  true },     // not readable in Qt but may be used with other software
    { "mov",            true,  true },
    { "mp4",            true,  true },
    { "mpeg",           true,  true },
    { "mpegts",         true,  true },
    { "mxf",            true,  false },     // not readable in Qt but may be used with other software, however MXF has too many constraints to be easyly writable (for H264 it requires avctx.profile = FF_PROFILE_H264_BASELINE, FF_PROFILE_H264_HIGH_10 or FF_PROFILE_H264_HIGH_422). it is better to transcode with an external tool
    { "ogg",            true,  false },    // Ogg, for theora codec (use ogv for writing)
    { "ogv",            true,  true },    // Ogg Video, for theora codec
    { nullptr, false, false}
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
const FilterEntry kCodecWhitelist[] =
{
    // Video codecs.
    { "aic",            true,  false },     // Apple Intermediate Codec (no encoder)
    { "avrp",           true,  UNSAFEQT0 && UNSAFEVLC },     // Avid 1:1 10-bit RGB Packer - write not supported as not official qt readable with relevant 3rd party codec.
    { "avui",           true,  false },     // Avid Meridien Uncompressed - write not supported as this is an SD only codec. Only 720x486 and 720x576 are supported. experimental in ffmpeg 2.6.1.
    { "ayuv",           true,  UNSAFEQT0 && UNSAFEVLC },     // Uncompressed packed MS 4:4:4:4 - write not supported as not official qt readable.
    { "cfhd",           true,  false },     // Cineform HD.
    { "cinepak",        true,  true },     // Cinepak.
    { "dxv",            true,  false },     // Resolume DXV
    { "dnxhd",          true,  true },     // VC3/DNxHD
    { "ffv1",           true,  UNSAFEQT0 && UNSAFEVLC },     // FFmpeg video codec #1 - write not supported as not official qt readable.
    { "ffvhuff",        true,  UNSAFEQT0 && UNSAFEVLC },     // Huffyuv FFmpeg variant - write not supported as not official qt readable.
    { "flv",            true,  UNSAFEQT0 },     // FLV / Sorenson Spark / Sorenson H.263 (Flash Video) - write not supported as not official qt readable.
    { "gif",            true,  true },     // GIF (Graphics Interchange Format) - write not supported as 8-bit only.
    { "h263p",          true,  true },     // H.263+ / H.263-1998 / H.263 version 2
    { "h264",           true,  false },     // H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (the encoder is libx264)
    { "hap",            true,  true },     // Vidvox Hap
    { "hevc",           true,  false },     // H.265 / HEVC (High Efficiency Video Coding) (the encoder is libx265)
    { "huffyuv",        true,  UNSAFEQT0 && UNSAFEVLC },     // HuffYUV - write not supported as not official qt readable.
    { "jpeg2000",       true,  UNSAFEQT0 },     // JPEG 2000 - write not supported as not official qt readable.
    { "jpegls",         true,  UNSAFEQT0 },     // JPEG-LS - write not supported as can't be read in in official qt.
    { "libopenh264",    true,  true },     // Cisco libopenh264 H.264/MPEG-4 AVC encoder
    { "libopenjpeg",    true,  true },     // OpenJPEG JPEG 2000
    { "libschroedinger", true,  UNSAFEQT0 && UNSAFEVLC },     // libschroedinger Dirac - write untested. VLC plays with a wrong format
    { "libtheora",      true,  UNSAFEQT0 },     // libtheora Theora - write untested.
    { "libvpx",         true,  UNSAFEQT0 },     // On2 VP8
    { "libvpx-vp9",     true,  UNSAFEQT0 },     // Google VP9
    { "libx264",        true,  UNSAFEQT0 },     // H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 (encoder)
    { "libx264rgb",     true,  UNSAFEQT0 },     // H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10 RGB (encoder)
    { "libx265",        true,  UNSAFEQT0 },     // H.265 / HEVC (High Efficiency Video Coding) (encoder) - resizes the image
    { "libxavs",        true,  false },         // Chinese AVS (Audio Video Standard) (encoder) - untested
    { "libxvid",        true,  true },     // MPEG-4 part 2
    { "ljpeg",          true,  UNSAFEQT0 },     // Lossless JPEG - write not supported as can't be read in in official qt.
    { "mjpeg",          true,  true },     // MJPEG (Motion JPEG) - this looks to be MJPEG-A. MJPEG-B encoding is not supported by FFmpeg so is not included here. To avoid confusion over the MJPEG-A and MJPEG-B variants, this codec is displayed as 'Photo JPEG'. This is done to i) avoid the confusion of the naming, ii) be consistent with Apple QuickTime, iii) the term 'Photo JPEG' is recommend for progressive frames which is appropriate to Nuke/NukeStudio as it does not have interlaced support.
    { "mpeg1video",     true,  TERRIBLE },     // MPEG-1 video - write not supported as it gives random 8x8 blocky errors
    { "mpeg2video",     true,  true },     // MPEG-2 video
    { "mpeg4",          true,  true },     // MPEG-4 part 2
    { "msmpeg4v2",      true,  UNSAFEQT0 },     // MPEG-4 part 2 Microsoft variant version 2 - write not supported as doesn't read in official qt.
    { "msmpeg4",        true,  UNSAFEQT0 },     // MPEG-4 part 2 Microsoft variant version 3 - write not supported as doesn't read in official qt.
    { "png",            true,  true },     // PNG (Portable Network Graphics) image
    { "prores",         true,  false },     // Apple ProRes (the encoder is prores_ks)
    { "qtrle",          true,  true },     // QuickTime Animation (RLE) video
    { "r10k",           true,  UNSAFEQT && UNSAFEVLC },     // AJA Kono 10-bit RGB - write not supported as not official qt readable without colourshifts.
    { "r210",           true,  UNSAFEQT && UNSAFEVLC },     // Uncompressed RGB 10-bit - write not supported as not official qt readable with relevant 3rd party codec without colourshifts.
    { "rawvideo",       true,  UNSAFEQT && UNSAFEVLC },     // raw video - write not supported as not official qt readable.
    { "svq1",           true,  true },     // Sorenson Vector Quantizer 1 / Sorenson Video 1 / SVQ1
    { "targa",          true,  true },     // Truevision Targa image.
    { "theora",         true,  false },     // Theora (decoder).
    { "tiff",           true,  true },     // TIFF Image
    { "v210",           true,  UNSAFEQT },     // Uncompressed 4:2:2 10-bit- write not supported as not official qt readable without colourshifts.
    { "v308",           true,  UNSAFEQT0 && UNSAFEVLC },     // Uncompressed packed 4:4:4 - write not supported as not official qt readable and 8-bit only.
    { "v408",           true,  UNSAFEQT0 && UNSAFEVLC },     // Uncompressed packed QT 4:4:4:4 - write not supported as official qt can't write, so bad round trip choice and 8-bit only.
    { "v410",           true,  UNSAFEQT0 && UNSAFEVLC },     // Uncompressed 4:4:4 10-bit - write not supported as not official qt readable with standard codecs.
    { "vc2",            true,  UNSAFEQT0 && UNSAFEVLC },     // SMPTE VC-2 (previously BBC Dirac Pro).
    { "vp8",            true,  false },     // On2 VP8 (decoder)
    { "vp9",            true,  false },     // Google VP9 (decoder)

    // Audio codecs.
    { "pcm_alaw",       true,  true },     // PCM A-law / G.711 A-law
    { "pcm_f32be",      true,  true },     // PCM 32-bit floating point big-endian
    { "pcm_f32le",      true,  true },     // PCM 32-bit floating point little-endian
    { "pcm_f64be",      true,  true },     // PCM 64-bit floating point big-endian
    { "pcm_f64le",      true,  true },     // PCM 64-bit floating point little-endian
    { "pcm_mulaw",      true,  true },     // PCM mu-law / G.711 mu-law
    { "pcm_s16be",      true,  true },     // PCM signed 16-bit big-endian
    { "pcm_s16le",      true,  true },     // PCM signed 16-bit little-endian
    { "pcm_s24be",      true,  true },     // PCM signed 24-bit big-endian
    { "pcm_s24le",      true,  true },     // PCM signed 24-bit little-endian
    { "pcm_s32be",      true,  true },     // PCM signed 32-bit big-endian
    { "pcm_s32le",      true,  true },     // PCM signed 32-bit little-endian
    { "pcm_s8",         true,  true },     // PCM signed 8-bit
    { "pcm_u16be",      true,  true },     // PCM unsigned 16-bit big-endian
    { "pcm_u16le",      true,  true },     // PCM unsigned 16-bit little-endian
    { "pcm_u24be",      true,  true },     // PCM unsigned 24-bit big-endian
    { "pcm_u24le",      true,  true },     // PCM unsigned 24-bit little-endian
    { "pcm_u32be",      true,  true },     // PCM unsigned 32-bit big-endian
    { "pcm_u32le",      true,  true },     // PCM unsigned 32-bit little-endian
    { "pcm_u8",         true,  true },     // PCM unsigned 8-bit
    { nullptr, false, false}
};

const FilterEntry*
getEntry(const char* name,
         const FilterEntry* whitelist,
         const FilterEntry* blacklist = nullptr)
{
    const FilterEntry* iterWhitelist = whitelist;
    const size_t nameLength = strlen(name);

    // check for normal mode
    while (iterWhitelist->name != nullptr) {
        size_t iteNameLength = strlen(iterWhitelist->name);
        size_t maxLength = (nameLength > iteNameLength) ? nameLength : iteNameLength;
        if (strncmp(name, iterWhitelist->name, maxLength) == 0) {
            // Found in whitelist, now check blacklist
            if (blacklist) {
                const FilterEntry* iterBlacklist = blacklist;

                while (iterBlacklist->name != nullptr) {
                    iteNameLength = strlen(iterBlacklist->name);
                    maxLength = (nameLength > iteNameLength) ? nameLength : iteNameLength;
                    if (strncmp(name, iterBlacklist->name, maxLength) == 0) {
                        // Found in codec whitelist but blacklisted too
                        return nullptr;
                    }

                    ++iterBlacklist;
                }
            }

            // Found in whitelist and not in blacklist
            return iterWhitelist;
        }

        ++iterWhitelist;
    }

    return nullptr;
}
} // namespace {

bool
FFmpegFile::isFormatWhitelistedForReading(const char* formatName)
{
    const FilterEntry* whitelistEntry = getEntry(formatName, kFormatWhitelist);

    return (whitelistEntry && whitelistEntry->enableReader);
}

bool
FFmpegFile::isFormatWhitelistedForWriting(const char* formatName)
{
    const FilterEntry* whitelistEntry = getEntry(formatName, kFormatWhitelist);

    return (whitelistEntry && whitelistEntry->enableWriter);
}

bool
FFmpegFile::isCodecWhitelistedForReading(const char* codecName)
{
    const FilterEntry* whitelistEntry = getEntry(codecName, kCodecWhitelist);

    return (whitelistEntry && whitelistEntry->enableReader);
}

bool
FFmpegFile::isCodecWhitelistedForWriting(const char* codecName)
{
    const FilterEntry* whitelistEntry = getEntry(codecName, kCodecWhitelist);

    return (whitelistEntry && whitelistEntry->enableWriter);
}

SwsContext*
FFmpegFile::Stream::getConvertCtx(AVPixelFormat srcPixelFormat,
                                  int srcWidth,
                                  int srcHeight,
                                  int srcColorRange,
                                  AVPixelFormat dstPixelFormat,
                                  int dstWidth,
                                  int dstHeight)
{
    // Reset is flagged when the UI colour matrix selection is
    // modified. This causes a new convert context to be created
    // that reflects the UI selection.
    if (_resetConvertCtx) {
        _resetConvertCtx = false;
        if (_convertCtx) {
            sws_freeContext(_convertCtx);
            _convertCtx = nullptr;
        }
    }

    if (!_convertCtx) {
        //Preventing deprecated pixel format used error messages, see:
        //https://libav.org/doxygen/master/pixfmt_8h.html#a9a8e335cf3be472042bc9f0cf80cd4c5
        //This manually sets them to the new versions of equivalent types.
        switch (srcPixelFormat) {
        case AV_PIX_FMT_YUVJ420P:
            srcPixelFormat = AV_PIX_FMT_YUV420P;
            if (srcColorRange == AVCOL_RANGE_UNSPECIFIED) {
                srcColorRange = AVCOL_RANGE_JPEG;
            }
            break;
        case AV_PIX_FMT_YUVJ422P:
            srcPixelFormat = AV_PIX_FMT_YUV422P;
            if (srcColorRange == AVCOL_RANGE_UNSPECIFIED) {
                srcColorRange = AVCOL_RANGE_JPEG;
            }
            break;
        case AV_PIX_FMT_YUVJ444P:
            srcPixelFormat = AV_PIX_FMT_YUV444P;
            if (srcColorRange == AVCOL_RANGE_UNSPECIFIED) {
                srcColorRange = AVCOL_RANGE_JPEG;
            }
            break;
        case AV_PIX_FMT_YUVJ440P:
            srcPixelFormat = AV_PIX_FMT_YUV440P;
            if (srcColorRange == AVCOL_RANGE_UNSPECIFIED) {
                srcColorRange = AVCOL_RANGE_JPEG;
            }
        default:
            break;
        }

        _convertCtx = sws_getContext(srcWidth, srcHeight, srcPixelFormat, // src format
                                     dstWidth, dstHeight, dstPixelFormat,        // dest format
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);

        // Set up the SoftWareScaler to convert colorspaces correctly.
        // Colorspace conversion makes no sense for RGB->RGB conversions
        if ( !isYUV() ) {
            return _convertCtx;
        }

        int colorspace = isRec709Format() ? SWS_CS_ITU709 : SWS_CS_ITU601;
        // Optional color space override
        if (_colorMatrixTypeOverride > 0) {
            if (_colorMatrixTypeOverride == 1) {
                colorspace = SWS_CS_ITU709;
            } else {
                colorspace = SWS_CS_ITU601;
            }
        }

        // sws_setColorspaceDetails takes a flag indicating the white-black range of the input:
        //     0  -  mpeg, 16..235
        //     1  -  jpeg,  0..255
        int srcRange;
        // Set this flag according to the color_range reported by the codec context.
        switch (srcColorRange) {
        case AVCOL_RANGE_MPEG:
            srcRange = 0;
            break;
        case AVCOL_RANGE_JPEG:
            srcRange = 1;
            break;
        case AVCOL_RANGE_UNSPECIFIED:
        default:
            // If the colour range wasn't specified, set the flag according to
            // whether the data is YUV or not.
            srcRange = isYUV() ? 0 : 1;
            break;
        }

        int result = sws_setColorspaceDetails(_convertCtx,
                                              sws_getCoefficients(colorspace), // inv_table
                                              srcRange, // srcRange -flag indicating the white-black range of the input (1=jpeg / 0=mpeg) 0 = 16..235, 1 = 0..255
                                              sws_getCoefficients(SWS_CS_DEFAULT), // table
                                              1, // dstRange - 0 = 16..235, 1 = 0..255
                                              0, // brightness fixed point, with 0 meaning no change,
                                              1 << 16, // contrast   fixed point, with 1<<16 meaning no change,
                                              1 << 16); // saturation fixed point, with 1<<16 meaning no change);

        assert(result != -1);
    }

    return _convertCtx;
} // FFmpegFile::Stream::getConvertCtx

/*static*/ double
FFmpegFile::Stream::GetStreamAspectRatio(Stream* stream)
{
    if (stream->_avstream->sample_aspect_ratio.num) {
#if TRACE_FILE_OPEN
        std::cout << "      Aspect ratio (from stream)=" << av_q2d(stream->_avstream->sample_aspect_ratio) << std::endl;
#endif

        return av_q2d(stream->_avstream->sample_aspect_ratio);
    } else if (stream->_codecContext->sample_aspect_ratio.num) {
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

// get stream start time
int64_t
FFmpegFile::getStreamStartTime(Stream & stream)
{
#if TRACE_FILE_OPEN
    std::cout << "      Determining stream start PTS:" << std::endl;
#endif

    AVPacket avPacket;
    av_init_packet(&avPacket);

    // Read from stream. If the value read isn't valid, get it from the first frame in the stream that provides such a
    // value.
    int64_t startPTS = stream._avstream->start_time;
    int64_t startDTS = stream._avstream->start_time;
#if TRACE_FILE_OPEN
    if ( startPTS != int64_t(AV_NOPTS_VALUE) ) {
        std::cout << "        Obtained from AVStream::start_time=";
    }
#endif

    if (startPTS < 0) {
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
        if (av_seek_frame(_context, stream._idx, startPTS, AVSEEK_FLAG_BACKWARD) >= 0) {

            // Read frames until we get one for the video stream that contains a valid PTS.
            while (av_read_frame(_context, &avPacket) >= 0) {
                if (avPacket.stream_index != stream._idx) {
                    continue;
                }
                // Packet read for video stream. Get its PTS.
                startPTS = avPacket.pts;
                startDTS = avPacket.dts;

                // Loop will continue if the current packet doesn't end after 0
                if (startPTS + avPacket.duration > 0) {
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
        if ( startPTS != int64_t(AV_NOPTS_VALUE) ) {
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
    const bool isStartPTSValid = (startPTS + avPacket.duration > 0);
    if (!isStartPTSValid) {
#if TRACE_FILE_OPEN
        std::cout << "        Not found by searching frames, assuming ";
#endif
        startPTS = 0;
        startDTS = 0;
    }

#if TRACE_FILE_OPEN
    std::cout << startPTS << " ticks, " << double(startPTS) * double(stream._avstream->time_base.num) /
        double(stream._avstream->time_base.den) << " s" << std::endl;
#endif

    stream._startDTS = startDTS;
    av_packet_unref(&avPacket);

    return startPTS;
} // FFmpegFile::getStreamStartTime

// Get the video stream duration in frames...
int64_t
FFmpegFile::getStreamFrames(Stream & stream)
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
        int64_t divisor = int64_t(AV_TIME_BASE) * stream._fpsDen;
        frames = ( (_context->duration - 1) * stream._fpsNum + divisor - 1 ) / divisor;

        // The above calculation is not reliable, because it seems in some situations (such as rendering out a mov
        // with 5 frames at 24 fps from Nuke) the duration has been rounded up to the nearest millisecond, which
        // leads to an extra frame being reported.  To attempt to work around this, compare against the number of
        // frames in the stream, and if they differ by one, use that value instead.
        int64_t streamFrames = stream._avstream->nb_frames;
        if ( (streamFrames > 0) && (std::abs( (double)(frames - streamFrames) ) <= 1) ) {
            frames = streamFrames;
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
        frames = (int64_t(stream._avstream->duration) * stream._avstream->time_base.num  * stream._fpsNum) /
                 (int64_t(stream._avstream->time_base.den) * stream._fpsDen);
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

        while (av_read_frame(_context, &avPacket) >= 0) {
            if (avPacket.stream_index == stream._idx && avPacket.pts != int64_t(AV_NOPTS_VALUE) && avPacket.pts > maxPts)
                maxPts = avPacket.pts;
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
static
bool
CheckStreamPropertiesMatch(const AVStream* streamA,
                           const AVStream* streamB)
{
#if 0//FF_API_LAVF_AVCTX
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

#if 0//FF_API_LAVF_AVCTX
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

    return
    (codecA->codec_id             == codecB->codec_id) &&
    (codecA->bits_per_raw_sample  == codecB->bits_per_raw_sample) &&
    (codecA->width                == codecB->width) &&
    (codecA->height               == codecB->height) &&
    (codecA->sample_aspect_ratio.num  == codecB->sample_aspect_ratio.num) &&
    (codecA->sample_aspect_ratio.den  == codecB->sample_aspect_ratio.den) &&
    (pixFmtDescA->nb_components       == pixFmtDescB->nb_components) &&
    (streamA->sample_aspect_ratio.num == streamB->sample_aspect_ratio.num) &&
    (streamA->sample_aspect_ratio.den == streamB->sample_aspect_ratio.den) &&
    (streamA->time_base.num     == streamB->time_base.num) &&
    (streamA->time_base.den     == streamB->time_base.den) &&
    (streamA->start_time        == streamB->start_time) &&
    (streamA->duration          == streamB->duration) &&
    (streamA->nb_frames         == streamB->nb_frames) &&
    (streamA->r_frame_rate.num  == streamB->r_frame_rate.num) &&
    (streamA->r_frame_rate.den  == streamB->r_frame_rate.den);
}

// constructor
FFmpegFile::FFmpegFile(const string & filename)
    : _filename(filename)
    , _context(nullptr)
    , _format(nullptr)
    , _streams()
    , _selectedStream(nullptr)
    , _errorMsg()
    , _invalidState(false)
    , _avPacket()
#ifdef OFX_IO_MT_FFMPEG
    , _lock()
    , _invalidStateLock()
#endif
{
#ifdef OFX_IO_MT_FFMPEG
    //MultiThread::AutoMutex guard(_lock); // not needed in a constructor: we are the only owner
#endif

#if TRACE_FILE_OPEN
    std::cout << "FFmpeg Reader=" << this << "::c'tor(): filename=" << filename << std::endl;
#endif

    assert( !_filename.empty() );
    AVDictionary* demuxerOptions = nullptr;
    // enabling drefs to allow reading from external tracks
    // this enables quicktime reference files demuxing
    av_dict_set(&demuxerOptions, "enable_drefs", "1", 0);
    CHECK( avformat_open_input(&_context, _filename.c_str(), _format, &demuxerOptions) );
    if (demuxerOptions != nullptr) {
        // demuxerOptions is destroyed and replaced, on avformat_open_input return,
        // with a dict containing the options that were not found
        //assert(false && "invalid options passed to demuxer");
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

    CHECK( avformat_find_stream_info(_context, nullptr) );

#if TRACE_FILE_OPEN
    std::cout << "  " << _context->nb_streams << " streams:" << std::endl;
#endif

    // fill the array with all available video streams
    bool unsuported_codec = false;

    // find all streams that the library is able to decode
    for (unsigned i = 0; i < _context->nb_streams; ++i) {
#if TRACE_FILE_OPEN
        std::cout << "    FFmpeg stream index " << i << ": ";
#endif
        AVStream* avstream = _context->streams[i];
        AVCodecContext* codecCtx = avcodec_alloc_context3(nullptr);
        avcodec_parameters_to_context(codecCtx, avstream->codecpar);

        // be sure to have a valid stream
        if (!avstream || !codecCtx) {
#if TRACE_FILE_OPEN
            std::cout << "No valid stream or codec, skipping..." << std::endl;
#endif
            continue;
        }

        int ret = avcodec_parameters_to_context(codecCtx, avstream->codecpar);
        if (ret < 0) {
#if TRACE_FILE_OPEN
            std::cout << "Could not convert to context, skipping..." << std::endl;
#endif
            continue;
        }

        // considering only video streams, skipping audio
        if (codecCtx->codec_type != AVMEDIA_TYPE_VIDEO) {
#if TRACE_FILE_OPEN
            std::cout << "Not a video stream, skipping..." << std::endl;
#endif
            continue;
        }
        if (codecCtx->pix_fmt == AV_PIX_FMT_NONE) {
#         if TRACE_FILE_OPEN
            std::cout << "Unknown pixel format, skipping..." << std::endl;
#         endif
            continue;
        }

        // find the codec
        AVCodec* videoCodec = avcodec_find_decoder(codecCtx->codec_id);
        if (videoCodec == nullptr) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder not found, skipping..." << std::endl;
#endif
            continue;
        }

        // skip codecs not in the white list
        //string reason;
        if ( !isCodecWhitelistedForReading(videoCodec->name) ) {
# if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" disallowed, skipping..." << std::endl;
# endif
            unsuported_codec = true;
            continue;
        }

        if (codecCtx->codec_type == AVMEDIA_TYPE_VIDEO) {
            // source: http://git.savannah.gnu.org/cgit/bino.git/tree/src/media_object.cpp

            // Some codecs support multi-threaded decoding (eg mpeg). Its fast but causes problems when opening many readers
            // simultaneously since each opens as many threads as you have cores. This leads to resource starvation and failed reads.
            // Hopefully, getNumCPUs() will give us the right number of usable cores

            // Activate multithreaded decoding. This must be done before opening the codec; see
            // http://lists.gnu.org/archive/html/bino-list/2011-08/msg00019.html
#          ifdef AV_CODEC_CAP_AUTO_THREADS
            // Do not use AV_CODEC_CAP_AUTO_THREADS, since it may create too many threads
            //if (avstream->codec->codec && (avstream->codec->codec->capabilities & AV_CODEC_CAP_AUTO_THREADS)) {
            //    avstream->codec->thread_count = 0;
            //} else
#          endif
            {
                codecCtx->thread_count = (std::min)( (int)MultiThread::getNumCPUs(), OFX_FFMPEG_MAX_THREADS ); // ask for the number of available cores for multithreading
#             ifdef AV_CODEC_CAP_SLICE_THREADS
                if ( codecCtx->codec && (codecCtx->codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) ) {
                    // multiple threads are used to decode a single frame. Reduces delay
                    codecCtx->thread_type = FF_THREAD_SLICE;
                }
#             endif
                //avstream->codec->thread_count = video_decoding_threads(); // bino's strategy (disabled)
            }
            // Set CODEC_FLAG_EMU_EDGE in the same situations in which ffplay sets it.
            // I don't know what exactly this does, but it is necessary to fix the problem
            // described in this thread: http://lists.nongnu.org/archive/html/bino-list/2012-02/msg00039.html
#ifdef CODEC_FLAG_EMU_EDGE // removed from ffmpeg 4.0
            int lowres = 0;
#ifdef FF_API_LOWRES
            lowres = avctx->lowres;
#endif
            if ( lowres || ( videoCodec && (videoCodec->capabilities & CODEC_CAP_DR1) ) ) {
                avctx->flags |= CODEC_FLAG_EMU_EDGE;
            }
#endif
        }

        // skip if the codec can't be open
        if (avcodec_open2(codecCtx, videoCodec, nullptr) < 0) {
#if TRACE_FILE_OPEN
            std::cout << "Decoder \"" << videoCodec->name << "\" failed to open, skipping..." << std::endl;
#endif
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
            //stream->_bitDepth = 16; // enabled in Nuke's reader

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
        std::cout << "      Duration=" << avstream->duration << " ticks, " <<
            double(avstream->duration) * double(avstream->time_base.num) /
            double(avstream->time_base.den) << " s" << std::endl;
        std::cout << "      BitDepth=" << stream->_bitDepth << std::endl;
        std::cout << "      NumberOfComponents=" << stream->_numberOfComponents << std::endl;
#endif

        // If FPS is specified, record it.
        // Otherwise assume 1 fps (default value).
        if ( (avstream->r_frame_rate.num != 0) && (avstream->r_frame_rate.den != 0) ) {
            stream->_fpsNum = avstream->r_frame_rate.num;
            stream->_fpsDen = avstream->r_frame_rate.den;
#if TRACE_FILE_OPEN
            std::cout << "      Framerate=" << stream->_fpsNum << "/" << stream->_fpsDen << ", " <<
                double(stream->_fpsNum) / double(stream->_fpsDen) << " fps" << std::endl;
#endif
        }
#if TRACE_FILE_OPEN
        else {
            std::cout << "      Framerate unspecified, assuming 1 fps" << std::endl;
        }
#endif

        stream->_width  = codecCtx->width;
        stream->_height = codecCtx->height;
#if TRACE_FILE_OPEN
        std::cout << "      Image size=" << stream->_width << "x" << stream->_height << std::endl;
#endif

        // set aspect ratio
        stream->_aspect = Stream::GetStreamAspectRatio(stream);

        // set stream start time and numbers of frames
        stream->_startPTS = getStreamStartTime(*stream);
        stream->_frames   = getStreamFrames(*stream);

        // save the stream
        _streams.push_back(stream);
    }
    if ( _streams.empty() ) {
        setError( unsuported_codec ? "unsupported codec..." : "unable to find video stream" );
        _selectedStream = nullptr;
    } else {
#pragma message WARN("should we build a separate FFmpegfile for each view? see also FFmpegFileManager")
        const int viewIndex = 0; // TODO: pass as parameter?
        assert(viewIndex >= 0 && "Negative view index specified.");

        if (static_cast<size_t>(viewIndex) < _streams.size()) {
            _selectedStream = _streams[viewIndex];
        }
        else {
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

void FFmpegFile::setSelectedStream(int streamIndex)
{
    if ((streamIndex >= 0) && (streamIndex < static_cast<int>(_streams.size()))) {
        _selectedStream = _streams[streamIndex];
    }
    else {
        assert(false && "setSelectedStream: Invalid streamIndex");
        _selectedStream = !_streams.empty() ? _streams[0] : nullptr;
    }
}

const char*
FFmpegFile::getColorspace() const
{
    //The preferred colorspace is figured out from a number of sources - initially we look for a number
    //of different metadata sources that may be present in the file. If these fail we then fall back
    //to using the codec's underlying storage mechanism - if RGB we default to gamma 1.8, if YCbCr we
    //default to gamma 2.2 (note prores special case). Note we also ignore the NCLC atom for reading
    //purposes, as in practise it tends to be incorrect.

    //First look for the meta keys that (recent) Nukes would've written, or special cases in Arri meta.
    //Doubles up searching for lower case keys as the ffmpeg searches are case sensitive, and the keys
    //have been seen to be lower cased (particularly in old Arri movs).
    if (_context && _context->metadata) {
        AVDictionaryEntry* t;

        t = av_dict_get(_context->metadata, "uk.co.thefoundry.Colorspace", nullptr, AV_DICT_IGNORE_SUFFIX);
        if (!t) {
            av_dict_get(_context->metadata, "uk.co.thefoundry.colorspace", nullptr, AV_DICT_IGNORE_SUFFIX);
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
            av_dict_get(_context->metadata, "com.arri.camera.colorgammasxs", nullptr, AV_DICT_IGNORE_SUFFIX);
        }
        if ( t && !strncasecmp(t->value, "LOG-C", 5) ) {
            return "AlexaV3LogC";
        }
        if ( t && !strncasecmp(t->value, "REC-709", 7) ) {
            return "rec709";
        }
    }

    //Special case for prores - the util YUV will report RGB, due to pixel format support, but for
    //compatibility and consistency with official quicktime, we need to be using 2.2 for 422 material
    //and 1.8 for 4444. Protected to deal with ffmpeg vagaries.
    assert((_streams.empty() || _selectedStream) && "_streams not empty but null _selectedStream");
    if (!_streams.empty() && _selectedStream->_codecContext && _selectedStream->_codecContext->codec_id) {
        if (_selectedStream->_codecContext->codec_id == AV_CODEC_ID_PRORES) {
            if ( ( _streams[0]->_codecContext->codec_tag == MKTAG('a', 'p', '4', 'h') ) ||
                 ( _streams[0]->_codecContext->codec_tag == MKTAG('a', 'p', '4', 'x') ) ) {
                return "Gamma1.8";
            } else {
                return "Gamma2.2";
            }
        }
    }
    if (!_streams.empty() && _selectedStream->_codecContext) {
        if (_selectedStream->_codecContext->color_trc == AVCOL_TRC_BT709 ||
            _selectedStream->_codecContext->color_trc == AVCOL_TRC_SMPTE240M) {
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
    return isYUV() ? "Gamma2.2" : "Gamma1.8";
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

const string &
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

bool
FFmpegFile::seekFrame(int frame,
                      Stream* stream)
{
    ///Private should not lock

    avcodec_flush_buffers(stream->_codecContext);
    int64_t timestamp = stream->frameToDts(frame);
    int error = av_seek_frame(_context, stream->_idx, timestamp, AVSEEK_FLAG_BACKWARD);
    if (error < 0) {
        // Seek error. Abort attempt to read and decode frames.
        setInternalError(error, "FFmpeg Reader failed to seek frame: ");

        return false;
    }

    // We can't be re-using the existing _avPacket data because we've just had to seek
    _avPacket.FreePacket();

    return true;
}

// decode a single frame into the buffer thread safe
bool
FFmpegFile::decode(const ImageEffect* /*plugin*/,
                   int frame,
                   bool loadNearest,
                   unsigned char* buffer)
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
    Stream* stream = _selectedStream;

    // Translate from the 1-based frames expected to 0-based frame offsets for use in the rest of this code.
    frame = frame - 1;

    // Check if the requested frame index is in range
    if (frame < stream->ptsToFrame(stream->_avstream->start_time)) {
        if (loadNearest) {
            frame = stream->ptsToFrame(stream->_avstream->start_time);
        } else {
            throw std::runtime_error("Missing frame");
        }
    } else if (frame >= stream->_frames) {
        if (loadNearest) {
            frame = (int)stream->_frames - 1;
        } else {
            throw std::runtime_error("Missing frame");
        }
    }

#if TRACE_DECODE_PROCESS
    std::cout << "FFmpeg Reader=" << this << "::decode(): frame=" << frame << /*", _viewIndex = " << _viewIndex <<*/ ", stream->_idx=" << stream->_idx << std::endl;
#endif

    bool hasPicture = false;
    AVFrame* avFrameOut = stream->_avFrame;

    // If gop_size == 0 then this is a intra-only encode and we can
    // assume sequential frame output from the decoder
    bool isIntraOnly = stream->_codecContext->gop_size ? false: true;

    // These codecs may still report a gop_size if incorrectly muxed.
    if ((stream->_codecContext->codec_id == AV_CODEC_ID_PRORES) ||
        (stream->_codecContext->codec_id == AV_CODEC_ID_DNXHD) ||
        (stream->_codecContext->codec_id == AV_CODEC_ID_MJPEG) ||
        (stream->_codecContext->codec_id == AV_CODEC_ID_MJPEGB) ||
        (stream->_codecContext->codec_id == AV_CODEC_ID_PNG)) {
        isIntraOnly = true;
    }

    // Old behaviour of expecting the next frame out of the decoder to be current frame + 1 is wrong.
    // For some codecs i.e H.264 and HEVC the decoder can output frames in decode order.
    // This is here to prevent any performace regression where we assume the decoder will always
    // output the frames in presentation order (true for the SW H.264 and MPEG4 decoders in libavcodec).
    // This may come back to haunt us one day.

    // Only seek and reset for non-sequential frames as this can be very costly.
    if (stream->ptsToFrame(avFrameOut->pts) + 1 != frame) {
        seekToFrame(stream->frameToPts(frame), AVSEEK_FLAG_BACKWARD);
    }

    // Setup the output frame struct with the buffer passed in
    avFrameOut->width   = stream->_width;
    avFrameOut->height  = stream->_height;
    avFrameOut->format  = stream->_outputPixelFormat;

    int res = 0;

    if ((res = av_image_fill_linesizes(avFrameOut->linesize, stream->_outputPixelFormat, stream->_width)) < 0) {
        setInternalError(res, "FFmpeg Reader Failed to fill image linesizes: ");
        return false;
    }

    res = av_image_fill_pointers(
                                 avFrameOut->data,
                                 stream->_outputPixelFormat,
                                 stream->_height,
                                 buffer,
                                 avFrameOut->linesize
                                 );

    if (res < 0) {
        setInternalError(res, "FFmpeg Reader Failed to fill image pointers: ");
        return false;
    }


    bool retriedSeek = false;
    bool retriedDecode = false;

    while (!retriedDecode) {

        retriedDecode = retriedSeek ? true: false;

        hasPicture = demuxAndDecode(avFrameOut, frame);

        // A last ditch effot to get a frame out for non-intra codecs.
        // This will perform a seek to the start of the file. which is
        // the only reliable way to get frame accurate seeking in a
        // stream with B-frames.
        if (!hasPicture && !isIntraOnly && !retriedSeek) {
            retriedSeek = true;
            seekToFrame(0, AVSEEK_FLAG_FRAME | AVSEEK_FLAG_BACKWARD);
        }
        else {
            break;
        }
    }

    return hasPicture;
} // FFmpegFile::decode


bool
FFmpegFile::seekToFrame(int64_t frame, int seekFlags)
{
    Stream* stream = _selectedStream;

    avcodec_flush_buffers(stream->_codecContext);
    int res = 0;

    if ((res = av_seek_frame(_context, stream->_idx, frame, seekFlags)) < 0) {
        setInternalError(res, "FFmpeg Reader Failed to seek frame: ");
        return false;
    }

    return true;
}

// compat_decode() replaces avcodec_decode_video2(), see
// https://github.com/FFmpeg/FFmpeg/blob/9e30859cb60b915f237581e3ce91b0d31592edc0/libavcodec/decode.c#L748
// Doc for the new AVCodec API: https://blogs.gentoo.org/lu_zero/2016/03/29/new-avcodec-api/
static int
mov64_av_decode(AVCodecContext *avctx, AVFrame *frame,
                int *got_frame, const AVPacket &pkt)
{
    return avcodec_decode_video2(avctx, frame, got_frame, &pkt);
}

bool FFmpegFile::demuxAndDecode(AVFrame* avFrameOut, int64_t frame)
{
    Stream* stream = _selectedStream;
    MyAVPacket avPacket;

    av_frame_unref(stream->_avIntermediateFrame);

    int res                   = 0;
    bool hasPicture           = false;
    int frameDecoded          = 0;
    AVFrame* avFrameDecodeDst = stream->_avIntermediateFrame;

    auto foundCorrectFrame = [stream](AVFrame* decodedFrame, int64_t targetFrameIdx) -> bool {
        bool found = false;

        // Fallback to using the DTS from the AVPacket that triggered returning this frame if no PTS is found
        if (decodedFrame->pts == AV_NOPTS_VALUE) {
            decodedFrame->pts = decodedFrame->pkt_dts;
        }

        int64_t decodedFrameIdx = stream->ptsToFrame(decodedFrame->pts);

        if (decodedFrameIdx == targetFrameIdx) {
            found = true;
        }
        // If the current frame needs to be displayed longer than current_pts - prev_pts
        else if ((decodedFrameIdx < targetFrameIdx) && (stream->ptsToFrame(decodedFrame->pts + decodedFrame->pkt_duration) > targetFrameIdx)) {
            found = true;
        }

        return found;
    };

    // Begin reading from the newly seeked position
    while ((res = av_read_frame(_context, &avPacket)) >= 0) {

        if (avPacket.stream_index == stream->_idx) {

            if ((res = mov64_av_decode(stream->_codecContext, avFrameDecodeDst, &frameDecoded, avPacket)) < 0) {
                setInternalError(res, "FFmpeg Reader Failed to decode packet: ");
                return false;
            }

            if (frameDecoded) {
                if (foundCorrectFrame(avFrameDecodeDst, frame)) {
                    hasPicture = imageConvert(avFrameDecodeDst, avFrameOut);
                    return hasPicture;
                }
            }
        }
    }


    // Check for any read failures
    if (res < 0 && res != AVERROR_EOF) {
        setInternalError(res, "FFmpeg Reader Failed to read frame: ");
        return false;
    }


    // Flush the decoder of remaining frames
    if (res == AVERROR_EOF) {
        AVPacket emptyPkt = {};
        while (1) {

            if ((res = mov64_av_decode(stream->_codecContext, avFrameDecodeDst, &frameDecoded, emptyPkt)) < 0) {
                setInternalError(res, "FFmpeg Reader Failed to flush the decoder of remaing frames: ");
                return false;
            }

            if (frameDecoded) {
                if (foundCorrectFrame(avFrameDecodeDst, frame)) {
                    return imageConvert(avFrameDecodeDst, avFrameOut);
                }
            }
            else {
                break;
            }
        }
    }

    return hasPicture;
}

bool
FFmpegFile::imageConvert(AVFrame* avFrameIn, AVFrame* avFrameOut)
{
    if (!avFrameIn || !avFrameOut) {
        setError("mov64Reader no input or output frame provided for conversion");
        return false;
    }

    Stream* stream = _selectedStream;

    AVPixelFormat dstPixFmt = (AVPixelFormat)avFrameOut->format;
    AVPixelFormat srcPixFmt = (AVPixelFormat)avFrameIn->format;

    avFrameOut->pts          = avFrameIn->pts;
    avFrameOut->pkt_dts      = avFrameIn->pkt_dts;
    avFrameOut->pkt_duration = avFrameIn->pkt_duration;

    if (!avFrameOut->data[0]) {
        int res = av_image_alloc(avFrameOut->data, avFrameOut->linesize, avFrameOut->width, avFrameOut->height, dstPixFmt, 32);
        if (res <= 0) {
            setInternalError(res, "FFmpeg Reader Output frame allocation failed during format conversion");
            return false;
        }
    }

    SwsContext* context = stream->getConvertCtx(
                                                srcPixFmt,
                                                avFrameIn->width,
                                                avFrameIn->height,
                                                avFrameIn->color_range,
                                                dstPixFmt,
                                                avFrameOut->width,
                                                avFrameOut->height
                                                );

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
                  avFrameOut->linesize
                  );
    } else {
        return false;
    }

    return true;
}

bool
FFmpegFile::getFPS(double & fps,
                   unsigned streamIdx)
{
    if ( streamIdx >= _streams.size() ) {
        return false;
    }

    // get the stream
    Stream* stream = _streams[streamIdx];
    // guard against division by zero
    assert(stream->_fpsDen);
    fps = stream->_fpsDen ? ( (double)stream->_fpsNum / stream->_fpsDen ) : stream->_fpsNum;

    return true;
}

// get stream information
bool
FFmpegFile::getInfo(int & width,
                    int & height,
                    double & aspect,
                    int & frames)
{
    AutoMutex guard(_lock);

    if (_streams.empty()) {
        return false;
    }

    assert(_selectedStream && "Null _selectedStream");
    if (!_selectedStream) {
        return false;
    }
    width  = _selectedStream->_width;
    height = _selectedStream->_height;
    aspect = _selectedStream->_aspect;
    frames = (int)_selectedStream->_frames;

    return true;
}

std::size_t
FFmpegFile::getBufferBytesCount() const
{
    if ( _streams.empty() ) {
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
    for (FilesMap::iterator it = _files.begin(); it != _files.end(); ++it) {
        for (std::list<FFmpegFile*>::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            delete *it2;
        }
    }
    _files.clear();
    delete _lock;
}

void
FFmpegFileManager::init()
{
    _lock = new FFmpegFile::Mutex;
}

void
FFmpegFileManager::clear(void const * plugin)
{
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    FilesMap::iterator found = _files.find(plugin);
    if ( found != _files.end() ) {
        for (std::list<FFmpegFile*>::iterator it = found->second.begin(); it != found->second.end(); ++it) {
            delete *it;
        }
        _files.erase(found);
    }
}

FFmpegFile*
FFmpegFileManager::get(void const * plugin,
                       const string &filename) const
{
    if (filename.empty() || !plugin) {
        return 0;
    }
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    FilesMap::iterator found = _files.find(plugin);
    if ( found != _files.end() ) {
        for (std::list<FFmpegFile*>::iterator it = found->second.begin(); it != found->second.end(); ++it) {
            if ( (*it)->getFilename() == filename ) {
                if ( (*it)->isInvalid() ) {
                    delete *it;
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

FFmpegFile*
FFmpegFileManager::getOrCreate(void const * plugin,
                               const string &filename) const
{
    if (filename.empty() || !plugin) {
        return 0;
    }
    assert(_lock);
    FFmpegFile::AutoMutex guard(*_lock);
    FilesMap::iterator found = _files.find(plugin);
    if ( found != _files.end() ) {
        for (std::list<FFmpegFile*>::iterator it = found->second.begin(); it != found->second.end(); ++it) {
            if ( (*it)->getFilename() == filename ) {
                if ( (*it)->isInvalid() ) {
                    delete *it;
                    found->second.erase(it);
                    break;
                } else {
                    return *it;
                }
            }
        }
    }

    FFmpegFile* file = new FFmpegFile(filename);
    if ( found == _files.end() ) {
        std::list<FFmpegFile*> fileList;
        fileList.push_back(file);
        _files.insert( make_pair(plugin, fileList) );
    } else {
        found->second.push_back(file);
    }

    return file;
}
