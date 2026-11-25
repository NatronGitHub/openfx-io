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
 */

#ifndef __Io__FFmpegHandler__
#define __Io__FFmpegHandler__

#if (defined(_STDINT_H) || defined(_STDINT_H_) || defined(_MSC_STDINT_H_)) && !defined(UINT64_C)
#warning "__STDC_CONSTANT_MACROS has to be defined before including <stdint.h>, this file will probably not compile."
#endif
#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS // ...or stdint.h wont' define UINT64_C, needed by libavutil
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
extern "C" {
#include <errno.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/ffversion.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

// FFmpeg 8.0.1 or later. First, the versions of the libraries in the n8.0 release (their libavcodec/version.h,
// libavformat/version.h, libavutil/version.h and libswscale/version.h). CMakeLists.txt and Makefile.master check
// the same versions with pkg-config.
#if (LIBAVCODEC_VERSION_INT < AV_VERSION_INT(62, 11, 100)) || (LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(62, 3, 100)) \
    || (LIBAVUTIL_VERSION_INT < AV_VERSION_INT(60, 8, 100)) || (LIBSWSCALE_VERSION_INT < AV_VERSION_INT(9, 1, 100))
#error "The FFmpeg plug-ins require FFmpeg 8.0.1 or later (libavcodec 62.11.100, libavformat 62.3.100, libavutil 60.8.100, libswscale 9.1.100)"
#endif

// Then not 8.0 itself: n8.0's libswscale truncates in its range conversions (solve_range_convert(), n8.0
// libswscale/swscale.c:574-585 has no rounding term), which n8.0.1 fixes (44bfe0da61, n8.0.1 swscale.c:583-585).
// n8.0.1 has the same library versions as n8.0 (all the lib*/version.h and version_major.h are equal), so only the
// release name tells them apart: FFMPEG_VERSION in libavutil/ffversion.h (installed with the headers; from VERSION,
// RELEASE or git describe, plus --extra-version: ffbuild/version.sh) at build time, and av_version_info() at run
// time (ffmpegRuntimeVersionError()). ffmpegIsRelease80() only rejects 8.0: "8.0" and "n8.0", alone or
// followed by '-' (a distribution package of 8.0, or a git snapshot of the release/8.0 branch); other names (8.0.1,
// 8.1, 9.0, git-..., N-...) are accepted. Not detected: unreleased N-... snapshots of master between the n8.0 branch
// point and the master fix (4bee010844), and shallow, tagless clones of release/8.0 from before the fix (git-...).
// Rejected although fixed: a distribution's 8.0 package with the fix backported (still named 8.0-...). Only
// libavutil's release name is checked, while the bug is in libswscale (FFmpeg's libraries are released together).
// FFmpegFile.cpp tests ffmpegIsRelease80() on sample names.

constexpr bool
ffmpegIsRelease80(std::string_view version)
{
    if (!version.empty() && version.front() == 'n') {
        version.remove_prefix(1);
    }

    return version.substr(0, 3) == "8.0" && (version.size() == 3 || version[3] == '-');
}

static_assert(!ffmpegIsRelease80(FFMPEG_VERSION),
              "The FFmpeg plug-ins require FFmpeg 8.0.1 or later: FFmpeg 8.0's libswscale truncates in its range "
              "conversions (fixed in 8.0.1)");

// Why the FFmpeg libraries loaded at run time cannot be used (they are FFmpeg 8.0, av_version_info()), or an empty
// string if they can. Independent of OFX, so that tests/ffmpeg_read_test links FFmpegFile.cpp without the Support
// library.
std::string ffmpegRuntimeVersionError();

#include "IOMutex.h" // FFmpegFile::Mutex
#include "ofxsMultiThread.h"

// For the plug-ins' load(): if ffmpegRuntimeVersionError() is not empty, prints it on stderr and throws
// OFX::Exception::Suite(kOfxStatFailed), so that the host does not list the plug-in (ofxCore.h, kOfxActionLoad:
// "kOfxStatFailed, the load action failed, no further actions will be passed to the plug-in"). Inline, so that only
// the plug-ins, which call it, need the Support library.
inline void
requireSupportedFFmpegRuntime(const char* pluginName)
{
    const std::string error = ffmpegRuntimeVersionError();
    if (!error.empty()) {
        std::cerr << pluginName << ": " << error << std::endl;
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
}

#define CHECKMSG(x, msg)         \
    {                            \
        int error = (x);         \
        if (error < 0) {         \
            setError((msg), ""); \
            close();             \
            return;              \
        }                        \
    }

// Used in ffmpeg metadata dicts to stash values written to and from nclc atom.
#define kNCLCPrimariesKey "fn_primaries"
#define kNCLCTransferKey "fn_transfer_function"
#define kNCLCMatrixKey "fn_matrix"
#define kNCLCUnknownLabel "Unknown"
#define kNCLCReservedLabel "Reserved"

// Avid DNxHD specific. Label for switching between video legal and full range.
#define kACLRYuvRange "fn_aclr_yuv_range"

// metadata keys used by Nuke
#define kMetaKeyApplication "uk.co.thefoundry.Application"
#define kMetaKeyApplicationVersion "uk.co.thefoundry.ApplicationVersion"
#define kMetaKeyYCbCrMatrix "uk.co.thefoundry.YCbCrMatrix"
#define kMetaKeyPixelFormat "uk.co.thefoundry.PixelFormat"
#define kMetaKeyColorspace "uk.co.thefoundry.Colorspace"
#define kMetaKeyWriter "uk.co.thefoundry.Writer"
#define kMetaValueWriter64 "mov64"

#define OFX_FFMPEG_MAX_THREADS 16 // MAX_AUTO_THREADS in libavcodec/pthread_internal.h. 32 in libavcodec/mpegvideo.h, 16 in libavcodec/hevcdec.h, 8 in libavcodec/vp8.h

////////////////////////////////////////////////////////////////////////////////
// Chunksize static names.
////////////////////////////////////////////////////////////////////////////////

#define kChunkSizeKey "fn_log2chunksize"

namespace OFX {
class ImageEffect;
}

class MyAVPacket {
public:
    MyAVPacket()
    {
        _pkt = av_packet_alloc();
    }
    ~MyAVPacket()
    {
        av_packet_free(&_pkt);
    }

    AVPacket* pkt() const
    {
        return _pkt;
    }

    AVPacket* operator->() const
    {
        return _pkt;
    }

private:
    AVPacket* _pkt;
};

class FFmpegFile {
public:
    // Thrown by decode() when the frame is outside of the stream and !loadNearest
    class MissingFrameException
        : public std::runtime_error
    {
    public:
        MissingFrameException()
            : std::runtime_error("Missing frame")
        {
        }
    };

    using Mutex = OFX::IO::IOMutex;
    using AutoMutex = OFX::IO::IOAutoMutex;

private:
    struct Stream {
        int _idx; // stream index
        AVStream* _avstream; // video stream
        AVCodecContext* _codecContext; // video codec context
        const AVCodec* _videoCodec;
        AVFrame* _avFrame; // decoding frame
        AVFrame* _avIntermediateFrame; // decode into this if an image conversion is required
        // Decoded frames kept since the last seek, to serve the frames that fall between two decoded
        // frames. In a variable frame rate movie, there is no frame at some frame times (timestamp
        // gaps), and the frame displayed at that time is the last decoded frame whose PTS is at or
        // before it, as in players. The decoder outputs frames in presentation order, so these are
        // decided from the decoded frames, not from the packets (which are in decoding order, and
        // whose durations are the nominal frame duration in Matroska).
        AVFrame* _avHeldFrame; // the last decoded frame at or before the last requested frame (empty if none)
        AVFrame* _avNextFrame; // the decoded frame that follows _avHeldFrame, if already decoded (empty if not)
        // A packet read from the demuxer that avcodec_send_packet() refused with AVERROR(EAGAIN), and whose drain gave
        // the requested frame: it is sent first by the next sequential request (empty if none, see demuxAndDecode()).
        AVPacket* _pendingPacket;
        // Palette formats (AV_PIX_FMT_FLAG_PAL): the last palette side data (AV_PKT_DATA_PALETTE) read from the stream
        // (empty if none), which is given again to the decoder after it is opened or flushed (see attachPalette()).
        std::vector<uint8_t> _palette;
        bool _paletteNeeded; // the decoder was opened or flushed since it last got _palette
        bool _heldFrameIsLast; // the decoder was drained after _avHeldFrame: it is the last frame of the stream
        bool _decodingFromStart; // the decoder has decoded every frame since the start of the stream (no seek since
        // decode() retried from the start): a corrupt held frame cannot be better, and may be used in a gap
        int _invalidDataSkipped; // the error of the last damaged packet dropped during the current decode() (0 if none)
        std::vector<int64_t> _damagedPts; // the PTS of the damaged packets dropped so far (bounded)
        int _truncatedFrame; // the frame of the partial last packet of a truncated file, from which frames fail (-1 if none)
        int _firstOutputFrame; // the first frame decoded by the last demuxAndDecode() (-1 if none)
        int _firstFrameAfter; // the first frame after the requested one decoded by the last demuxAndDecode() (-1 if none)
        bool _reachedEnd; // the last demuxAndDecode() drained the decoder at the end of the file, without a frame to hold
        bool _endTainted; // the last demuxAndDecode() reached the end of the file while tainted (see demuxAndDecode())
        bool _drainedEarly; // the decoder was drained after too many damaged packets: decoding further needs a seek
        bool _damageSeen; // a damaged packet was dropped in this file (see seekToFrame())
        bool _damagedSinceOpen; // a damaged packet was dropped since the decoder was opened (see flushDecoder())
        bool _singleThreaded; // long-GOP codecs: damage was found with frame threads, the decoder now has 1 thread
        int64_t _keyFrames; // the keyframes decoded without an error
        int64_t _packetsWithoutPts; // the video packets sent without a PTS (see positionTaint())
        bool _awaitRestartFrame; // position taint: the next decoded frame is the frame of the restart keyframe
        int64_t _nonKeyFrames; // the other frames decoded without an error
        bool _restartSingleThread; // the decoder must be opened again with 1 thread, before decoding further
        int64_t _restartPts; // the PTS of the keyframe where the decoder restarted, until it decodes (AV_NOPTS_VALUE if none)
        bool _verifyNextPacket; // the next packet sent is a restart keyframe, to verify (see verifyRestart())
        std::vector<int64_t> _candidatePts; // the PTS of the packets sent since the restart keyframe, until it is verified
        std::vector<int64_t> _suspectKeyPts; // the PTS of the packets with a keyframe flag that did not decode as a clean keyframe
        std::vector<int64_t> _recentPts; // the PTS of the last packets sent (as many as the frame threads)
        bool _tainted; // long-GOP codecs: damage was found since the last restart of the decoder (see taint())
        std::vector<int64_t> _taintedPts; // the PTS of the packets sent since the damage (see taint())
        int _lastDamagedFrame; // the largest frame of the damaged packets found by the current decode() (-1 if none)
        int _requestDroppedPackets; // the packets dropped by all the attempts of the current decode()
        bool _noTimestamps; // the current decode() found a decoded frame without a timestamp, and stopped
        std::vector<std::pair<int, int>> _damagedRanges; // ranges of frames found to fail because of damage (sorted, disjoint)
        int _damagedRangeError;
        bool _sawFrameWithoutTimestamp; // the current decode() decoded a frame without a timestamp
        bool _stoppedOnDamagedPacket; // intra-only: the last demuxAndDecode() found only damaged packets from its start to the requested frame (and stopped)
        bool _tooManyDroppedPackets; // decoding stopped after too many consecutive damaged packets in the current decode()
        int64_t _nextDecodedPts; // the timestamp of the frame after the last decoded frame (AV_NOPTS_VALUE if none since the
        // last seek), for a decoded frame without a timestamp (e.g. the last frame of an AVI with B-frames)
        // For decodeForwardIsCheaper():
        bool _indexReliable; // the format has no timestamp discontinuities (not AVFMT_TS_DISCONT), set at open, and no
        // seek landed on a predicted frame (see demuxAndDecode()). Cleared for the lifetime of the stream by one such
        // seek: the frames are still right, but later backward requests that land after the requested frame (e.g. on
        // a leading picture of an open-GOP keyframe) retry from a fixed number of frames before instead of from the
        // keyframe before (earlierRestartTs()), and decodeForwardIsCheaper() decodes forward within its window without
        // the index.
        // The check of the frame of the first packet sent since the last seek (see demuxAndDecode()): a seek may land
        // on a packet that the demuxer flags as a keyframe, but that is not one.
        enum class SeekFrameCheck {
            eNone, // no check (not since a seek, or that frame was checked, or its packet had no PTS)
            eAwaitPacket, // no packet was sent since the last seek
            eAwaitFrame, // that packet was sent (_seekFramePts), its frame was not decoded yet
            eAwaitIntraFrame, // its frame was a predicted frame: the frames until one that is not are not used
        };
        SeekFrameCheck _seekFrameCheck;
        int64_t _seekFramePts; // eAwaitFrame: the PTS of the first packet sent since the last seek
        int64_t _maxReadTs; // the largest timestamp (PTS, or DTS without one) of the packets of this stream sent to the
        // decoder since the last seek (AV_NOPTS_VALUE if none)
        int64_t _lastReadTs; // the timestamp of the last packet sent with one since the last seek (AV_NOPTS_VALUE if none)
        int _packetsSinceKey; // the packets sent since the last keyframe packet, since the last seek (-1 if none)
        bool _intervalDiscontinuous; // a timestamp discontinuity was found since the last keyframe packet
        int _maxKeyframeInterval; // the largest number of packets from a keyframe packet to the next one (0 if none)
        SwsContext* _convertCtx;
        bool _resetConvertCtx;
        // Source/destination parameters the cached _convertCtx was created for,
        // used to detect when it must be rebuilt (e.g. if the frame dimensions,
        // pixel format or color range change mid-stream).
        AVPixelFormat _convertCtxSrcPixelFormat;
        int _convertCtxSrcWidth;
        int _convertCtxSrcHeight;
        int _convertCtxSrcRange; // the source range of the conversion: 0 = video (16..235), 1 = full (0..255)
        int _convertCtxSwsColorspace; // the Y'CbCr coefficient table of the conversion (SWS_CS_*)
        AVPixelFormat _convertCtxDstPixelFormat;
        int _convertCtxDstWidth;
        int _convertCtxDstHeight;
        // The 16-bit values that _convertCtx gives for nominal white (R', G' and B') and for opaque alpha, which
        // imageConvert() scales to 65535 (see MeasureWhite16() in FFmpegFile.cpp); 65535 if they need no scaling.
        unsigned _convertCtxWhite16;
        unsigned _convertCtxOpaque16;

        int _fpsNum;
        int _fpsDen;

        int64_t _startPTS; // PTS of the first frame in the stream
        int64_t _startDTS; // DTS of the first packet in the stream (_startPTS if unknown)
        bool _startFromDecodedFrame; // the packets have no PTS: _startPTS is the timestamp of the first decoded frame
        bool _ptsOnFirstPacketOnly; // the first packet has a PTS, and the next ones none, even after a seek (see probeStreamStart())
        int64_t _frames; // video duration in frames

        bool _ptsSeen; // True if a read AVPacket has ever contained a valid PTS during this stream's decode,
        // indicating that this stream does contain PTSs.
        int64_t AVPacket::*_timestampField; // Pointer to member of AVPacket from which timestamps are to be retrieved. Enables
        // fallback to using DTSs for a stream if PTSs turn out not to be available.
        int _width;
        int _height;
        double _aspect;
        int _bitDepth;
        int _numberOfComponents;
        enum AVPixelFormat _outputPixelFormat;
        uint8_t _componentPosition[4];
        int _colorMatrixOverride; // the Y'CbCr matrix (eColorMatrixAuto: from the tags of the stream)
        int _colorRangeOverride; // the Y'CbCr range (eColorRangeAuto: from the tags of the stream)
        bool _doNotAttachPrefix;
        bool _matchMetaFormat;
        int _decodeNextFrameIn; // The 0-based index of the next frame to be fed into decode. Negative before any
        // frames have been decoded or when we've just seeked but not yet found a relevant frame. Equal to
        // frames_ when all available frames have been fed into decode.
        int _decodeNextFrameOut; // The 0-based index of the next frame expected out of decode. Negative before
        // any frames have been decoded or when we've just seeked but not yet found a relevant frame. Equal to
        // frames_ when all available frames have been output from decode.
        int _accumDecodeLatency; // The number of frames that have been input without any frame being output so far in this stream
        // since the last seek. This is part of a guard mechanism to detect when decode appears to have
        // stalled and ensure that FFmpegFile::decode() does not loop indefinitely.

        Stream()
            : _idx(0)
            , _avstream(nullptr)
            , _codecContext(nullptr)
            , _videoCodec(nullptr)
            , _avFrame(nullptr)
            , _avIntermediateFrame(nullptr)
            , _avHeldFrame(nullptr)
            , _avNextFrame(nullptr)
            , _pendingPacket(nullptr)
            , _palette()
            , _paletteNeeded(true)
            , _heldFrameIsLast(false)
            , _decodingFromStart(false)
            , _invalidDataSkipped(0)
            , _damagedPts()
            , _truncatedFrame(-1)
            , _firstOutputFrame(-1)
            , _firstFrameAfter(-1)
            , _reachedEnd(false)
            , _endTainted(false)
            , _drainedEarly(false)
            , _damageSeen(false)
            , _damagedSinceOpen(false)
            , _singleThreaded(false)
            , _keyFrames(0)
            , _packetsWithoutPts(0)
            , _awaitRestartFrame(false)
            , _nonKeyFrames(0)
            , _restartSingleThread(false)
            , _restartPts(AV_NOPTS_VALUE)
            , _verifyNextPacket(false)
            , _candidatePts()
            , _suspectKeyPts()
            , _recentPts()
            , _tainted(false)
            , _taintedPts()
            , _lastDamagedFrame(-1)
            , _requestDroppedPackets(0)
            , _noTimestamps(false)
            , _damagedRanges()
            , _damagedRangeError(0)
            , _sawFrameWithoutTimestamp(false)
            , _stoppedOnDamagedPacket(false)
            , _tooManyDroppedPackets(false)
            , _nextDecodedPts(AV_NOPTS_VALUE)
            , _indexReliable(false)
            , _seekFrameCheck(SeekFrameCheck::eNone)
            , _seekFramePts(AV_NOPTS_VALUE)
            , _maxReadTs(AV_NOPTS_VALUE)
            , _lastReadTs(AV_NOPTS_VALUE)
            , _packetsSinceKey(-1)
            , _intervalDiscontinuous(false)
            , _maxKeyframeInterval(0)
            , _convertCtx(nullptr)
            , _resetConvertCtx(true)
            , _convertCtxSrcPixelFormat(AV_PIX_FMT_NONE)
            , _convertCtxSrcWidth(0)
            , _convertCtxSrcHeight(0)
            , _convertCtxSrcRange(-1)
            , _convertCtxSwsColorspace(-1)
            , _convertCtxDstPixelFormat(AV_PIX_FMT_NONE)
            , _convertCtxDstWidth(0)
            , _convertCtxDstHeight(0)
            , _convertCtxWhite16(65535)
            , _convertCtxOpaque16(65535)
            , _fpsNum(1)
            , _fpsDen(1)
            , _startPTS(0)
            , _startDTS(0)
            , _startFromDecodedFrame(false)
            , _ptsOnFirstPacketOnly(false)
            , _frames(0)
            , _ptsSeen(false)
            , _timestampField(&AVPacket::pts)
            , _width(0)
            , _height(0)
            , _aspect(1.0)
            , _bitDepth(8)
            , _numberOfComponents(3)
            , _outputPixelFormat(AV_PIX_FMT_RGB24)
            , _colorMatrixOverride(0)
            , _colorRangeOverride(0)
            , _doNotAttachPrefix(true)
            , _matchMetaFormat(true)
            , _decodeNextFrameIn(-1)
            , _decodeNextFrameOut(-1)
            , _accumDecodeLatency(0)
        {
            // The purpose of this is to avoid an RGB->RGB conversion.
            // This saves memory and improves performance. For example
            // Nuke expects RGBA component order. So for any RGB pixel
            // format which does not have this ordering, the following
            // should be used to remap the components, e.g. ARGB->RGBA,
            // BGRA->RGBA, BGR->RGB, etc. The output of the DNxHD decoder
            // is an example of this in action.
            _componentPosition[0] = 0; // Red position.
            _componentPosition[1] = 1; // Green position.
            _componentPosition[2] = 2; // Blue position.
            _componentPosition[3] = 3; // Alpha position.
        }

        ~Stream()
        {
            if (_avFrame) {
                av_frame_free(&_avFrame);
            }

            if (_avIntermediateFrame) {
                av_frame_free(&_avIntermediateFrame);
            }

            if (_avHeldFrame) {
                av_frame_free(&_avHeldFrame);
            }

            if (_avNextFrame) {
                av_frame_free(&_avNextFrame);
            }

            if (_pendingPacket) {
                av_packet_free(&_pendingPacket);
            }

            if (_codecContext) {
                avcodec_flush_buffers(_codecContext);
                avcodec_free_context(&_codecContext);
            }

            if (_convertCtx) {
                sws_freeContext(_convertCtx);
            }
        }

        static void destroy(Stream* s)
        {
            delete (s);
        }

        bool inDamagedRange(int frame) const
        {
            for (const auto& r : _damagedRanges) {
                if ((frame >= r.first) && (frame <= r.second)) {
                    return true;
                }
            }
            return false;
        }

        // the end of the last damaged range before the frame (-1 if none)
        int damagedRangeEndBefore(int frame) const
        {
            int end = -1;
            for (const auto& r : _damagedRanges) {
                if ((r.second < frame) && (r.second > end)) {
                    end = r.second;
                }
            }
            return end;
        }

        // add a damaged range, merged with the ranges it overlaps or touches
        void addDamagedRange(int lo, int hi)
        {
            std::vector<std::pair<int, int>> ranges;
            for (const auto& r : _damagedRanges) {
                if ((r.second + 1 < lo) || (r.first > hi + 1)) {
                    ranges.push_back(r);
                } else {
                    lo = (std::min)(lo, r.first);
                    hi = (std::max)(hi, r.second);
                }
            }
            ranges.push_back(std::make_pair(lo, hi));
            std::sort(ranges.begin(), ranges.end());
            _damagedRanges.swap(ranges);
        }

        // Long-GOP codecs: after damage, the frames of the packets sent from the damaged one until the decoder
        // restarts at a keyframe (see demuxAndDecode()) are not used, since they may reference a damaged frame.
        // As FFmpeg's own H.264 decoder does after a flush (h264dec.c:471, frame_recovered = 0), where frames are
        // only output once "recovered" by an IDR frame or a recovery point (libavcodec/h264_slice.c:1374-1384,
        // 1665-1681, h264dec.c:927-929 in FFmpeg 9.0.2, with AV_CODEC_FLAG_OUTPUT_CORRUPT and AV_CODEC_FLAG2_SHOW_ALL
        // off, their defaults, libavcodec/options_table.h:56): without a flush, frame_recovered stays set, and
        // the decoder outputs the frames that reference the damage as if they were good. With frame threads, the error of a packet
        // comes with up to thread_count packets sent after it: these are taken as damaged too.
        // Some packets have no PTS (e.g. AVI): the taint is by position, not by timestamp (see demuxAndDecode())
        bool positionTaint() const
        {
            return _packetsWithoutPts > 0;
        }

        void taint()
        {
            _awaitRestartFrame = false; // damage after the restart keyframe
            if (_restartPts != int64_t(AV_NOPTS_VALUE)) {
                verifyRestart(false); // damage before the restart keyframe was verified
            }
            _tainted = true;
            for (int64_t pts : _recentPts) {
                addTaintedPts(pts);
            }
        }

        // whether the frame of the restart keyframe is a valid restart point (see FFmpegFile.cpp)
        bool isCleanRestartFrame(const AVFrame* f) const;

        // The keyframe where the decoder restarted (after damage, or after a seek once damage was found) gave its
        // frame: if it is a clean restart (isCleanRestartFrame()), the frames of the packets from
        // there on are used again (but not the frames displayed before it, e.g. the leading B-frames of an open
        // GOP, which reference the previous GOP); otherwise they stay tainted, and the decoder restarts at the next
        // packet with the keyframe flag.
        void verifyRestart(bool clean)
        {
            if (clean) {
                _tainted = false;
                for (int64_t pts : _candidatePts) {
                    if (pts < _restartPts) {
                        addTaintedPts(pts);
                    }
                }
                _suspectKeyPts.erase(std::remove(_suspectKeyPts.begin(), _suspectKeyPts.end(), _restartPts), _suspectKeyPts.end());
            } else {
                for (int64_t pts : _candidatePts) {
                    addTaintedPts(pts);
                }
            }
            _candidatePts.clear();
            _restartPts = AV_NOPTS_VALUE;
        }

        // damage was found in a stream whose frames decoded without an error are all keyframes (see decode()), at least
        // kMinKeyframesAllIntra of them (a long-GOP stream damaged after its first keyframes may only have decoded
        // keyframes cleanly so far)
        static const int kMinKeyframesAllIntra = 16;
        bool allKeyframesAfterDamage() const
        {
            return _damageSeen && (_nonKeyFrames == 0) && (_keyFrames >= kMinKeyframesAllIntra);
        }

        bool isCandidatePts(int64_t pts) const
        {
            return std::find(_candidatePts.begin(), _candidatePts.end(), pts) != _candidatePts.end();
        }

        bool isSuspectKeyPts(int64_t pts) const
        {
            return std::find(_suspectKeyPts.begin(), _suspectKeyPts.end(), pts) != _suspectKeyPts.end();
        }

        void addTaintedPts(int64_t pts)
        {
            if ((pts != int64_t(AV_NOPTS_VALUE)) && !isTaintedPts(pts)) {
                if (_taintedPts.size() >= 4096) {
                    _taintedPts.erase(_taintedPts.begin());
                }
                _taintedPts.push_back(pts);
            }
        }

        bool isTaintedPts(int64_t pts) const
        {
            return std::find(_taintedPts.begin(), _taintedPts.end(), pts) != _taintedPts.end();
        }

        void clearTaint()
        {
            _tainted = false;
            _taintedPts.clear();
            _recentPts.clear();
            _restartPts = AV_NOPTS_VALUE;
            _verifyNextPacket = false;
            _awaitRestartFrame = false;
            _candidatePts.clear();
        }

        // Flush the decoder: it gets the palette again with the next packet (see attachPalette()). This is defensive: no
        // palette decoder of FFmpeg n9.0.2 clears its palette when flushed (rawdec has no flush callback, and
        // msrle_decode_flush(), libavcodec/msrle.c:142-147, and qtrle_decode_flush(), qtrle.c:566-571, only unref the
        // frame), but opening the decoder again (FFmpegFile::flushDecoder()) does.
        // A decoder may still return frames that it output before the flush: they are dropped. E.g. the HEVC decoder
        // queues the frames it outputs (av_container_fifo_write(s->output_fifo), libavcodec/hevc/refs.c:313 in FFmpeg
        // n9.0.2, refs.c:311 in n8.0.1), returns them before it reads a packet (hevc_receive_frame(),
        // hevcdec.c:3848-3849 in n9.0.2, 3842 in n8.0.1), and does not empty the queue when flushed
        // (hevc_decode_flush(), hevcdec.c:4197-4209 in n9.0.2, 4190-4201 in n8.0.1). With frame threads,
        // ff_thread_flush() already drops the frames decoded by the threads (libavcodec/pthread_frame.c:986-997 in
        // n9.0.2). After a request that stopped at the first of two frames output together, the second one would
        // otherwise come first after a seek, and decode() would take it for a seek that landed after the requested
        // frame. Without a new packet, avcodec_receive_frame() then returns AVERROR(EAGAIN).
        void flushCodec()
        {
            avcodec_flush_buffers(_codecContext);
            AVFrame* stale = av_frame_alloc();
            while (stale && (avcodec_receive_frame(_codecContext, stale) >= 0)) {
            }
            av_frame_free(&stale);
            _paletteNeeded = true;
        }

        // Keep the palette side data of a packet, if any (a palette of the wrong size is ignored, as by
        // ff_copy_palette()).
        static void keepPalette(const AVPacket& pkt, std::vector<uint8_t>* palette)
        {
            size_t size = 0;
            const uint8_t* pal = av_packet_get_side_data(&pkt, AV_PKT_DATA_PALETTE, &size);
            if (pal && (size == AVPALETTE_SIZE)) {
                palette->assign(pal, pal + size);
            }
        }

        // Before a packet of the stream is sent to the decoder. Some demuxers send the palette only once, as side data
        // of the first packet (AVI, MOV: FFmpeg n9.0.2 libavformat/avidec.c:1526-1536, libavformat/mov.c:11902-11910),
        // and the decoders that read it from the packets (ff_copy_palette(), libavcodec/decode.c:2321-2334) keep a
        // zeroed palette until then (e.g. libavcodec/rawdec.c:96-103). The reader seeks and flushes the decoder, so
        // that packet is not the first one it decodes: the last palette read is attached to the first packet sent
        // after the decoder was opened or flushed, unless that packet has its own. A palette change (AVI 'xxpc' chunks,
        // libavformat/avidec.c:1345-1356) also comes as side data: after a backward seek, the last palette read is
        // used, as the decoder itself would (and as in ffplay, which only flushes the decoder after a seek).
        void attachPalette(AVPacket* pkt)
        {
            if (av_packet_get_side_data(pkt, AV_PKT_DATA_PALETTE, nullptr)) {
                keepPalette(*pkt, &_palette);
            } else if (_paletteNeeded && !_palette.empty()) {
                uint8_t* pal = av_packet_new_side_data(pkt, AV_PKT_DATA_PALETTE, _palette.size());
                if (!pal) {
                    return; // out of memory: the next packet gets it
                }
                std::copy(_palette.begin(), _palette.end(), pal);
            }
            _paletteNeeded = false;
        }

        // a packet is sent to the decoder (window: the number of packets that may be decoded at once)
        void packetSent(const AVPacket& pkt, int window)
        {
            const int64_t pts = pkt.pts;
            const bool key = (pkt.flags & AV_PKT_FLAG_KEY) != 0;
            trackReadPosition(pkt);
            if (_seekFrameCheck == SeekFrameCheck::eAwaitPacket) {
                // the first packet since the seek, whatever its keyframe flag (see demuxAndDecode())
                _seekFramePts = pts;
                _seekFrameCheck = (pts == int64_t(AV_NOPTS_VALUE)) ? SeekFrameCheck::eNone : SeekFrameCheck::eAwaitFrame;
            }
            if (pts == int64_t(AV_NOPTS_VALUE)) {
                ++_packetsWithoutPts;
                return;
            }
            while ((int)_recentPts.size() >= window) {
                _recentPts.erase(_recentPts.begin());
            }
            _recentPts.push_back(pts);
            if (_verifyNextPacket) {
                _verifyNextPacket = false;
                _restartPts = pts;
            }
            if (_tainted) {
                if (_restartPts != int64_t(AV_NOPTS_VALUE)) {
                    if (_candidatePts.size() < 4096) {
                        _candidatePts.push_back(pts);
                    }
                } else {
                    addTaintedPts(pts);
                }
                if (key && !isSuspectKeyPts(pts)) {
                    if (_suspectKeyPts.size() >= 1024) {
                        _suspectKeyPts.erase(_suspectKeyPts.begin());
                    }
                    _suspectKeyPts.push_back(pts); // until it decodes as a clean keyframe (verifyRestart())
                }
            }
        }

        bool isDamagedPts(int64_t pts) const
        {
            return std::find(_damagedPts.begin(), _damagedPts.end(), pts) != _damagedPts.end();
        }

        static bool hasFrame(const AVFrame* f)
        {
            return f && f->buf[0];
        }

        static bool hasPacket(const AVPacket* p)
        {
            return p && (p->data || p->side_data_elems);
        }

        // Forget the decoded frames kept since the last seek. Must be called whenever the
        // demuxer or the decoder is repositioned.
        void resetHeldFrames()
        {
            if (_avHeldFrame) {
                av_frame_unref(_avHeldFrame);
            }
            if (_avNextFrame) {
                av_frame_unref(_avNextFrame);
            }
            if (_pendingPacket) {
                av_packet_unref(_pendingPacket);
            }
            _heldFrameIsLast = false;
            _drainedEarly = false;
            _decodingFromStart = false;
            _nextDecodedPts = AV_NOPTS_VALUE;
            _maxReadTs = AV_NOPTS_VALUE;
            _lastReadTs = AV_NOPTS_VALUE;
            _packetsSinceKey = -1;
            _intervalDiscontinuous = false;
            _seekFrameCheck = SeekFrameCheck::eAwaitPacket;
            _seekFramePts = AV_NOPTS_VALUE;
        }

        // The position of the demuxer, and the keyframe interval, for decodeForwardIsCheaper(). The interval is counted
        // in packets, not from the timestamps, so that a timestamp jump does not make it longer, and an interval with a
        // timestamp discontinuity (a jump of more than kMaxReorderFrames frames and 1 s) is not counted.
        static constexpr int kMaxReorderFrames = 16;
        void trackReadPosition(const AVPacket& pkt)
        {
            const int64_t ts = (pkt.pts != int64_t(AV_NOPTS_VALUE)) ? pkt.pts : pkt.dts;
            if (ts != int64_t(AV_NOPTS_VALUE)) {
                if ((_maxReadTs == int64_t(AV_NOPTS_VALUE)) || (ts > _maxReadTs)) {
                    _maxReadTs = ts;
                }
                if (_lastReadTs != int64_t(AV_NOPTS_VALUE)) {
                    const int64_t jump = std::abs(int64_t(ptsToFrame(ts)) - ptsToFrame(_lastReadTs));
                    if (jump > (std::max)(kMaxReorderFrames, oneSecondInFrames())) {
                        _intervalDiscontinuous = true;
                    }
                }
                _lastReadTs = ts;
            }
            if (pkt.flags & AV_PKT_FLAG_KEY) {
                if ((_packetsSinceKey > 0) && !_intervalDiscontinuous) {
                    _maxKeyframeInterval = (std::max)(_maxKeyframeInterval, _packetsSinceKey);
                }
                _packetsSinceKey = 1;
                _intervalDiscontinuous = false;
            } else if (_packetsSinceKey >= 0) {
                ++_packetsSinceKey;
            }
        }

        // The 0-based frame of the start time of the stream (0 if unknown, e.g. in elementary streams)
        int firstFrame() const
        {
            return (_avstream->start_time != int64_t(AV_NOPTS_VALUE)) ? ptsToFrame(_avstream->start_time) : 0;
        }

        // The number of frames in 1 s (at least 1)
        int oneSecondInFrames() const
        {
            const int64_t den = (std::max)(1, _fpsDen);
            return static_cast<int>((std::max)(int64_t(1), (int64_t(_fpsNum) + den - 1) / den));
        }

        // The largest forward request decoded without a seek, in frames: the largest keyframe interval seen, at least
        // 1 s and at most 10 s. The interval is counted in packets, which are frames, except with field-coded video
        // (e.g. MPEG-2 with a packet per field), where it is doubled: the 10 s cap bounds that.
        int forwardWindow() const
        {
            const int second = oneSecondInFrames();
            return (std::min)((std::max)(_maxKeyframeInterval, second), 10 * second);
        }

        // True if a forward request, more than one frame after the held frame, is cheaper to decode from the
        // current position of the decoder than after a seek (see FFmpegFile.cpp).
        bool decodeForwardIsCheaper(int frame, int held) const;

        // The keyframe of the index at or before the timestamp, as a backward seek searches it, or nullptr.
        const AVIndexEntry* indexKeyframeAtOrBefore(int64_t ts) const;

        // The timestamp from which to decode again when a backward seek to the 0-based frame landed after it: the
        // keyframe of the index before the one that the seek finds, or without an index to trust a timestamp earlier by
        // twice forwardWindow(); AV_NOPTS_VALUE if none (see FFmpegFile.cpp).
        int64_t earlierRestartTs(int frame) const;

        // True if the 0-based frame can be obtained without seeking: from the decoded frames kept
        // since the last seek, by decoding the next frame, or by decoding forward to a later frame when this is
        // cheaper than a seek (decodeForwardIsCheaper()).
        bool isSequential(int frame) const
        {
            if (!hasFrame(_avHeldFrame)) {
                return false;
            }
            int held = ptsToFrame(_avHeldFrame->pts);
            if (frame < held) {
                return false;
            }
            if (hasFrame(_avNextFrame)) {
                int next = ptsToFrame(_avNextFrame->pts);
                if (frame < next) {
                    return true;
                }
                held = next; // _avNextFrame will become the held frame
            } else if (_heldFrameIsLast) {
                return true;
            }
            if (_drainedEarly) {
                return frame == held; // the decoder gives no more frames
            }

            return (frame <= held + 1) || decodeForwardIsCheaper(frame, held);
        }

        // Conversions between 0-based frame indices and timestamps round to the nearest value,
        // not towards zero: a container's time base is often not a multiple of the frame
        // duration, so timestamps are rounded when muxed. Matroska/WebM store milliseconds, so at
        // 24 fps the third frame (index 2) is at 83 ms, and 83 * 24 / 1000 = 1.992 must give
        // frame 2, not 1 (truncation made every third frame impossible to find).
        // Timestamps that are an exact number of frames (most MOV/MP4) give the same result.
        int64_t frameToPts(int frame) const
        {
            int64_t numerator = int64_t(_fpsDen) * _avstream->time_base.den;
            int64_t denominator = int64_t(_fpsNum) * _avstream->time_base.num;

            // guard against division by zero
            assert(denominator);

            return _startPTS + (denominator ? av_rescale_rnd(frame, numerator, denominator, AV_ROUND_NEAR_INF) : int64_t(frame) * numerator);
        }

        // The duration of a frame, in AV_TIME_BASE units.
        int64_t frameDuration() const
        {
            return av_rescale(AV_TIME_BASE, _fpsDen, _fpsNum);
        }

        int ptsToFrame(int64_t pts) const
        {
            int64_t numerator = int64_t(_avstream->time_base.num) * _fpsNum;
            int64_t denominator = int64_t(_avstream->time_base.den) * _fpsDen;

            // guard against division by zero
            assert(denominator);

            return static_cast<int>(denominator ? av_rescale_rnd(pts - _startPTS, numerator, denominator, AV_ROUND_NEAR_INF) : (pts - _startPTS) * numerator);
        }

        bool isRec709Format()
        {
            // First check for codecs which require special handling:
            //  * JPEG codecs always use Rec 601.
            AVCodecID codecID = _codecContext->codec_id;

            if (codecID == AV_CODEC_ID_MJPEG) {
                return false;
            }

            // Using method described in step 5 of QuickTimeCodecReader::setPreferredMetadata
            // #cristian: height >= 720 (HD); height < 720 (SD)
            return (_height >= 720);
        }

        bool isYUV() const
        {
            // from swscale_internal.h
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(_codecContext->pix_fmt);

            return !(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components >= 2;
        }

        static double GetStreamAspectRatio(Stream* stream);

        // Generate the conversion context used by SoftWareScaler if not already set.
        // |reset| forces recalculation of cached context.
        SwsContext* getConvertCtx(AVPixelFormat srcPixelFormat, int srcWidth, int srcHeight, int srcColorRange, int srcColorSpace, AVPixelFormat dstPixelFormat, int dstWidth, int dstHeight);

        // The Y'CbCr coefficient table (SWS_CS_*) for a frame with the given matrix (AVFrame.colorspace), see getConvertCtx()
        int getSwsColorspace(int frameColorSpace);

        // The source range for sws_setColorspaceDetails() (0 = video, 1 = full) for a frame with the given pixel
        // format and range (AVFrame.color_range), see getConvertCtx()
        int getSwsSrcRange(AVPixelFormat srcPixelFormat, int frameColorRange) const;

        // Return the number of input frames needed by this stream's codec before it can produce output. We expect to have to
        // wait this many frames to receive output; any more and a decode stall is detected.
        // Frame-threaded decoding delays the output by one frame per thread (n8.0 libavcodec/avcodec.h:1565,
        // pthread_frame.c:959 sets AVCodecContext.delay to thread_count - 1), so the thread count is added too.
        int getCodecDelay() const
        {
            return (((_videoCodec->capabilities & AV_CODEC_CAP_DELAY) ? _codecContext->delay : 0)
                    + _codecContext->has_b_frames
                    + _codecContext->thread_count);
        }
    };

    std::string _filename;

    // AV structure
    AVFormatContext* _context;

    // store all video streams available in the file (the views: they match the first one, see
    // CheckStreamPropertiesMatch()). Set by the constructor only, so it may be read without a lock; the Streams
    // themselves are decoded under _lock.
    std::vector<Stream*> _streams;

    // The stream that the last decode() demuxed (nullptr before the first one), guarded by _lock
    Stream* _demuxedStream;

    // reader error state
    std::string _errorMsg; // internal decoding error string
    bool _invalidState; // true if the reader is in an invalid state
    unsigned long _seekCount; // number of seeks by decode(), for tests
    unsigned long _startSeekCount; // number of those seeks to the start of the file, for tests
    int64_t _bytesReadAtOpen; // bytes read from the file by the constructor, for tests
    bool _readAtOpen; // the constructor read or sought in the file, after avformat_find_stream_info()
    bool _damagedPacketWarned; // a warning was logged for a damaged packet
    int64_t _packetsEnd; // in AV_TIME_BASE units (AV_NOPTS_VALUE if none), guarded by _lock: see notePacketEnd()

#ifdef OFX_IO_MT_FFMPEG
    // _lock guards the decoding state: _context (demuxing, seeking), _demuxedStream, the state of every Stream
    // (codec context, held frames, conversion context, color overrides), _seekCount, _startSeekCount and _packetsEnd. decode()
    // holds it from the choice of the stream (the view) to the end of the decode, so that concurrent renders of
    // different views do not decode each other's stream (#291).
    // _invalidStateLock guards _invalidState. _errorMsg is written with both locks held (decode() holds _lock), and
    // read under either one.
    // Lock order: _lock, then _invalidStateLock (decode() sets errors). FFmpegFileManager's lock is taken before
    // _invalidStateLock (isInvalid()), never while _lock is held.
    mutable Mutex _lock;
    mutable Mutex _invalidStateLock;
#endif

    // set reader error
    void setError(const char* msg, const char* prefix = 0);

    // clear the decode error
    void clearDecodeError();

    // After this many consecutive damaged packets without a decoded frame, decode() stops (see demuxAndDecode())
    static const int kMaxDroppedPacketsPerDecode = 64;

    // set a decode error, which does not make the file invalid
    void setDecodeError(const char* msg, const char* prefix = 0);
    void setDecodeError(const int error, const char* prefix = 0);

    // set FFmpeg library error
    void setInternalError(const int error,
                          const char* prefix = 0)
    {
        char errorBuf[1024];

        av_strerror(error, errorBuf, sizeof(errorBuf));
        setError(errorBuf, prefix);
    }

    // get stream start time
    std::vector<std::vector<uint8_t>> readFirstPalettes();
    int64_t getStreamStartTime(Stream& stream);

    // What probeStreamStart() reads from the first packets of a stream (see FFmpegFile.cpp)
    struct StreamStartProbe
    {
        int64_t firstDTS = AV_NOPTS_VALUE; // the DTS of the first packet
        int64_t firstFramePTS = AV_NOPTS_VALUE; // the first packet has no PTS: the timestamp of the first decoded frame
        bool ptsOnFirstPacketOnly = false; // the first packet has PTS == DTS, and a next one a DTS but no PTS
    };
    StreamStartProbe probeStreamStart(Stream& stream);
    bool nextPacketsLackPts(const Stream& stream, int maxPackets);

    // The number of frames of the stream that are not displayed
    int64_t getDiscardedFrames(Stream& stream);

    // The end of the movie (in AV_TIME_BASE units) and the largest PTS of the video stream, from the last packets
    bool probeEnd(Stream& stream, int64_t& movieEnd, int64_t& lastVideoPts);

    // Get the video stream duration in frames...
    int64_t getStreamFrames(Stream& stream);

public:
    // FFmpegFile();

    // constructor
    FFmpegFile(const std::string& filename);

    // destructor
    ~FFmpegFile();

    const std::string& getFilename() const
    {
        return _filename;
    }

    // The frame of the largest timestamp in the index of the first stream (-1 if none), for tests
    int getLastIndexedFrame() const;

    // The number of bytes read from the file when it was opened (by the constructor), for tests
    int64_t getBytesReadAtOpen() const
    {
        return _bytesReadAtOpen;
    }

    // The number of bytes read from the file so far, for tests (not thread safe: no decode() must run meanwhile)
    int64_t getBytesRead() const
    {
        return (_context && _context->pb) ? _context->pb->bytes_read : 0;
    }

    // get the internal error string
    // The error message (of the file, or of the last frame that failed); by value, since another thread may
    // decode a frame meanwhile (see also the errorMsg argument of decode())
    std::string getError() const;

    // return true if the reader can't decode the frame
    bool isInvalid() const;

    // return the numbers of streams supported by the reader
    unsigned int getNbStreams() const
    {
        return _streams.size();
    }

    // The Y'CbCr matrix and range of the conversion to RGB (the ReadFFmpeg "YCbCr Matrix" and "YCbCr Range" params).
    // Auto takes them from the tags of the stream (see Stream::getConvertCtx()).
    enum ColorMatrixEnum {
        eColorMatrixAuto = 0,
        eColorMatrixRec601,
        eColorMatrixRec709,
        eColorMatrixRec2020,
    };
    enum ColorRangeEnum {
        eColorRangeAuto = 0,
        eColorRangeVideo,
        eColorRangeFull,
    };

    // Set the Y'CbCr matrix and range of all the streams. The conversion context is rebuilt by the next decode() if
    // they changed. Thread safe.
    void setColorOverrides(ColorMatrixEnum matrix, ColorRangeEnum range) const
    {
#ifdef OFX_IO_MT_FFMPEG
        AutoMutex guard(_lock); // decode() reads them
#endif
        for (Stream* stream : _streams) {
            stream->_colorMatrixOverride = (int)matrix;
            stream->_colorRangeOverride = (int)range;
        }
    }

    // The OCIO colorspace name written by WriteFFmpeg (or Nuke) in the kMetaKeyColorspace metadata of the file, or
    // an empty string. Thread safe (takes _lock: decode() may update the metadata).
    std::string getColorspaceMetadata() const;

    void setDoNotAttachPrefix(bool doNotAttachPrefix) const
    {
        if (_streams.empty()) {
            return;
        }

        // mov64Reader::decode always uses stream 0
        Stream* stream = _streams[0];
        stream->_doNotAttachPrefix = doNotAttachPrefix;
    }

    void setMatchMetaFormat(bool matchMetaFormat) const
    {
        if (_streams.empty()) {
            return;
        }

        // mov64Reader::decode always uses stream 0
        Stream* stream = _streams[0];
        stream->_matchMetaFormat = matchMetaFormat;
    }

    bool isRec709Format() const
    {
#ifdef OFX_IO_MT_FFMPEG
        AutoMutex guard(_lock); // decode() may replace the codec context (flushDecoder())
#endif
        if (_streams.empty()) {
            return false;
        }

        // mov64Reader::decode always uses stream 0
        Stream* stream = _streams[0];

        return stream->isRec709Format();
    }

    bool isYUV() const
    {
#ifdef OFX_IO_MT_FFMPEG
        AutoMutex guard(_lock); // decode() may replace the codec context (flushDecoder())
#endif
        if (_streams.empty()) {
            return false;
        }

        // mov64Reader::decode always uses stream 0
        Stream* stream = _streams[0];

        return stream->isYUV();
    }

    int getBitDepth() const
    {
        if (_streams.empty()) {
            return 0;
        }

        // mov64Reader::decode always uses stream 0
        // Pixel formats with less than 8 bits per component (e.g. monow: 1, bgr8: 3) are decoded to 8 bits.
        return (std::max)(8, _streams[0]->_bitDepth);
    }

    int getNumberOfComponents() const
    {
        if (_streams.empty()) {
            return 0;
        }

        // mov64Reader::decode always uses stream 0
        return _streams[0]->_numberOfComponents;
    }

    int getComponentPosition(int componentIndex) const
    {
        if (_streams.empty()) {
            return 0;
        }

        // mov64Reader::decode always uses stream 0
        return _streams[0]->_componentPosition[componentIndex];
    }

    int getWidth() const
    {
        if (_streams.empty()) {
            return 0;
        }

        // mov64Reader::decode always uses stream 0
        return _streams[0]->_width;
    }

    int getHeight() const
    {
        if (_streams.empty()) {
            return 0;
        }

        // mov64Reader::decode always uses stream 0
        return _streams[0]->_height;
    }

    std::size_t getSizeOfData() const
    {
        if (_streams.empty()) {
            return 0;
        }

        return _streams[0]->_bitDepth > 8 ? sizeof(unsigned short) : sizeof(unsigned char);
    }

    // decode a single frame of the stream streamIndex (the view; the first stream if there is no such stream) into
    // the buffer. Thread safe.
    // If it fails and errorMsg is not null, *errorMsg is the error message of this frame.
    bool decode(const OFX::ImageEffect* plugin, int streamIndex, int frame, bool loadNearest, unsigned char* buffer, std::string* errorMsg = nullptr);

    // get stream information
    bool getFPS(double& fps,
                unsigned streamIdx = 0);

    // get the information of the stream streamIndex (the first stream if there is no such stream). Thread safe.
    bool getInfo(int streamIndex,
                 int& width,
                 int& height,
                 double& aspect,
                 int& frames) const;

    const char* getColorspace() const;

    std::size_t getBufferBytesCount() const;

    // Number of seeks done by decode() so far (a debugging and test counter: seeking is costly).
    unsigned long getSeekCount() const
    {
        return _seekCount;
    }

    // Number of those seeks to the start of the file (decoding from the start of a long file is slow).
    unsigned long getStartSeekCount() const
    {
        return _startSeekCount;
    }

    // Whether the file holds a single image, from its extension (see FFmpegFile.cpp).
    static bool isImageFile(const std::string& filename);

    //! Check whether a named container format is allowed
    static bool isFormatAllowedForWriting(const char* name);

    //! Check whether a certain codec name is allowed
    static bool isCodecAllowedForWriting(const char* name);

private:
    // The stream streamIndex, or the first stream if there is no such stream (as a view of a multi-view project
    // beyond the streams of the file), or nullptr if the file has no stream. The non-const one is for decode(), which
    // holds _lock.
    const Stream* getStream(int streamIndex) const;
    Stream* getStream(int streamIndex);

    // flush the decoder, or open it again after damage
    // (these are called by decode(), with _lock held, on the stream that it decodes)
    void flushDecoder(Stream* stream);
    bool seekToFrame(Stream* stream, int64_t frame, int seekFlags);

    // decodeAsIntraOnly: each frame is decoded from its own packet, as for an intra-only codec (IsIntraOnly()), or for a
    // stream found to be all keyframes after damage (Stream::allKeyframesAfterDamage(), e.g. all-intra H.264): no taint,
    // a held frame that conceals damage may be displayed in a timestamp gap, and decoding stops at damaged packets.
    // damagedFrom: intra-only, the first frame of the frames up to the requested one known to be damaged (-1 if none)
    bool demuxAndDecode(Stream* stream, AVFrame* avFrameOut, int64_t frame, bool decodeAsIntraOnly, int damagedFrom = -1);
    // Updates _packetsEnd with a packet read by demuxAndDecode() for stream (with _lock held).
    void notePacketEnd(const Stream& stream, const AVPacket& pkt);

    bool imageConvert(Stream* stream, AVFrame* avFrameIn, AVFrame* avFrameOut);
};

class FFmpegFileManager {
    /// For each plug-in instance, a list of opened files.
    /// Files are held by shared_ptr so a caller that obtained a file via get()/getOrCreate()
    /// keeps it alive for the duration of its use, even if clear() or invalid-eviction
    /// removes it from the map on another thread. This prevents use-after-free during decode.
    typedef std::map<void const*, std::list<std::shared_ptr<FFmpegFile>>> FilesMap;
    mutable FilesMap _files;
    mutable FFmpegFile::Mutex* _lock; // guards _files

public:
    FFmpegFileManager();

    ~FFmpegFileManager();

    void init();

    void clear(void const* plugin);

    std::shared_ptr<FFmpegFile> get(void const* plugin, const std::string& filename) const;
    std::shared_ptr<FFmpegFile> getOrCreate(void const* plugin, const std::string& filename) const;
};

#endif /* defined(__Io__FFmpegHandler__) */
