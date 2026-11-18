/*
 * ofx_render_test — full render/encode/decode integration tests for the openfx-io
 * FFmpeg plugins, DIRECTLY LINKED with the OFX C++ Support library and driven by a
 * minimal in-process OFX host (no bundle / no dlopen / no external host).
 *
 * Unlike the describe-only tests, this creates plugin instances and drives the
 * full render pipeline through the plugin code.
 *
 *   WriteFFmpeg test:
 *     - CreateInstance (Writer context), set the "filename" param to an output file
 *     - beginSequenceRender -> render each frame (host supplies a source image whose
 *       gray level ramps with frame number) -> endSequenceRender
 *     - the plugin encodes a video file on disk
 *     - verify the file exists and is non-empty (the ReadFFmpeg test below then
 *       decodes it back, which validates the encoded content)
 *
 *   ReadFFmpeg test:
 *     - CreateInstance (Reader context), set "filename" to the file just written
 *     - getClipPreferences + getRegionOfDefinition + render each frame into a host
 *       output image, then verify the decoded gray level ramps with frame number
 *       (i.e. the decode round-trips the encode)
 *
 *   WriteFFmpeg codec/format options (issue #71):
 *     - encode MPEG-4 part 2 with Keyframe Interval = 5 and the Codec Options "g=1": every decoded
 *       frame must be an I frame (the user option overrides the plug-in param), whereas without it
 *       there are P frames
 *     - Format Options "movflags=+faststart": the moov atom must precede the mdat atom
 *     - an unknown key must produce a warning naming it, and the file must still be written
 *     - a syntax error must make beginSequenceRender fail with an error message
 *
 *   WriteFFmpeg views (issue #116):
 *     - render views 1 and 0 of each frame (view 1 first, then view 0 first): the movie must be view 0
 *
 *   WriteFFmpeg start timecode test (issue #78):
 *     - write a short .mov/.mp4 with the "startTimecode" param set, and read the
 *       timecode back with libavformat (the timecode track and the video stream
 *       metadata, as ffprobe shows them)
 *     - an invalid timecode must make beginSequenceRender fail, and write nothing
 *     - issue #110: 119.88 fps drop-frame is written at 120000/1001 fps; no 255 fps limit when no
 *       timecode (tmcd) track is written (3gp, write_tmcd=0); the persistent message shows all the
 *       errors in the settings (timecode and codec/container), whatever param is edited
 *
 *   WriteFFmpeg color tags and levels (issue #135):
 *     - encode a black and a white frame, and read the stream back with libavformat/libavcodec (the
 *       properties that ffprobe -show_streams shows, and the decoded levels)
 *     - ProRes 4444, 4444 XQ and 422 HQ are tagged with their Y'CbCr matrix (SMPTE 170M in SD)
 *     - DNxHD/DNxHR with Output Range = Full/Video: the decoded levels match the range the stream is tagged
 *       with; DNxHR 444 (RGB) is full range; HQX and 444 are 10-bit, and their hints do not say 12 bit
 *
 *   WriteFFmpeg, output RoD changed during the render:
 *     - from the second frame on, the image is smaller (or larger) than the movie, as when Natron sets the project
 *       format from the first Reader once the render has started: the part of the image in the frame is written,
 *       the rest is black (the image was read as if it had the size of the movie, out of its buffer)
 *
 *   WriteFFmpeg libx265 pixel format (issue #136):
 *     - with Alpha unchecked and Bit Depth 10 or 12, the pixel format has no alpha, at least the requested
 *       chroma, and the highest bit depth that libx265 supports up to the requested one
 *
 *   WriteOIIO test (issue #79):
 *     - render a JPEG while the host has no source image for the render window
 *       (clipGetImage fails with kOfxStatFailed): a black image must be written
 *     - same, with a source image whose bounds do not intersect the render window:
 *       a black image must be written, and the output image must be black
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <OpenImageIO/imageio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#ifndef _WIN32
#include <stdlib.h> // mkstemps()
#include <unistd.h> // close()
#endif

namespace {

// Extra param values for a WriteFFmpeg test.
struct WriterParams {
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::pair<std::string, int>> ints;
    std::vector<std::pair<std::string, double>> doubles; // double params
    double inputPar = 1.;            // the PAR of the source clip
    bool expectBeginFailure = false; // beginSequenceRender must fail (and nothing is rendered)
    bool constantImage = false;      // render the same mid-gray image for every frame instead of a ramp
    std::vector<float> frameValues;  // if not empty, the gray level of frame t is frameValues[t - 1] (instead of a ramp)
    bool expectNoFile = false;       // with expectBeginFailure: the output file must not be created
    double fps = 0.;                 // if > 0, the value of the "fps" param
    std::vector<int> views = { 0 };  // the views rendered for each frame, in this order: view 0 is the ramp, the
                                     // others its negative (1 - v)
    // choice params set after the instance is created (which sets e.g. the format from the file
    // extension), as the param and the text of the label of the option
    std::vector<std::pair<std::string, std::string>> choicesAfterCreate;
    std::string renderFile;          // if not empty, the "filename" param from the second frame on (#137)
    bool expectRenderFailure = false; // with renderFile: frames 2..N must fail to render
    // if renderW > 0: from the second frame on, the render window and the source image are renderW x renderH,
    // not W x H (the size of the movie, the output RoD at beginSequenceRender), as when Natron changes the
    // project format during the render
    int renderW = 0, renderH = 0;
};

// The index of the first option of a choice param whose label contains the given text, or -1.
int choiceIndex(Effect& ctx, const char* param, const char* text)
{
    auto it = ctx.params.find(param);
    if (it == ctx.params.end()) return -1;
    auto opt = it->second.props->props.find(kOfxParamPropChoiceOption);
    if (opt == it->second.props->props.end()) return -1;
    for (size_t i = 0; i < opt->second.s.size(); ++i) {
        if (opt->second.s[i].find(text) != std::string::npos) return (int)i;
    }
    return -1;
}

int testWriteFFmpeg(const std::string& outFile, int W, int H, int nFrames, const WriterParams& wp = WriterParams())
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    std::remove(outFile.c_str());

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }

    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    for (const auto& sp : wp.strings) {
        if (!ctx.params.count(sp.first)) { std::printf("  FAIL: no param %s\n", sp.first.c_str()); return 1; }
        setOverrideString(&ctx, sp.first.c_str(), sp.second);
    }
    for (const auto& ip : wp.ints) {
        if (!ctx.params.count(ip.first)) { std::printf("  FAIL: no param %s\n", ip.first.c_str()); return 1; }
        setOverrideInt(&ctx, ip.first.c_str(), ip.second);
    }
    for (const auto& dp : wp.doubles) {
        if (!ctx.params.count(dp.first)) { std::printf("  FAIL: no param %s\n", dp.first.c_str()); return 1; }
        ctx.params[dp.first].hasOv = true;
        ctx.params[dp.first].ov.d = { dp.second };
    }
    if (wp.fps > 0.) {
        if (!ctx.params.count("fps")) { std::printf("  FAIL: no param fps\n"); return 1; }
        ctx.params["fps"].hasOv = true;
        ctx.params["fps"].ov.d = { wp.fps };
    }

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, nFrames, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    for (const auto& cp : wp.choicesAfterCreate) {
        const int i = choiceIndex(ctx, cp.first.c_str(), cp.second.c_str());
        if (i < 0) { std::printf("  FAIL: no option \"%s\" in param %s\n", cp.second.c_str(), cp.first.c_str()); return 1; }
        setOverrideInt(&ctx, cp.first.c_str(), i);
    }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    if (!src) { std::printf("  FAIL: no Source clip\n"); return 1; }
    src->connected = true;
    setupClipImage(src, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    if (wp.inputPar != 1.) {
        pSetDouble(PH(&src->props), kOfxImagePropPixelAspectRatio, 0, wp.inputPar);
        pSetDouble(PH(&src->image), kOfxImagePropPixelAspectRatio, 0, wp.inputPar);
    }
    if (ctx.clips.count(kOfxImageEffectOutputClipName)) {
        Clip* o = ctx.clips[kOfxImageEffectOutputClipName];
        o->connected = false;
        setupClipImage(o, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
        pSetInt(PH(&o->props), kOfxImageClipPropConnected, 0, 0);
    }

    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); std::printf("  getClipPreferences = %d\n", cp); }

    {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        double fr[2] = { 1., (double)nFrames }; pSetDoubleN(ap, kOfxImageEffectPropFrameRange, 2, fr);
        pSetDouble(ap, kOfxImageEffectPropFrameStep, 0, 1.);
        pSetInt(ap, kOfxPropIsInteractive, 0, 0);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
        pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
        pSetInt(ap, kFnOfxImageEffectPropView, 0, 0);
        const bool began = ok(p->mainEntry(kOfxImageEffectActionBeginSequenceRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr));
        if (wp.expectBeginFailure) {
            p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
            p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
            if (began) { std::printf("  FAIL: beginSequenceRender succeeded, a failure was expected\n"); return 1; }
            if (wp.expectNoFile) {
                std::FILE* f = std::fopen(outFile.c_str(), "rb");
                if (f) { std::fclose(f); std::printf("  FAIL: the output file was created\n"); return 1; }
            }
            std::printf("  PASS (beginSequenceRender failed as expected)\n");
            return 0;
        }
        if (!began) { std::printf("  FAIL: beginSequenceRender\n"); return 1; }
    }

    float* fp = reinterpret_cast<float*>(src->buffer.data());
    size_t npix = (size_t)W * H;
    int rw = W, rh = H; // the render window
    int rfail = 0;
    for (int t = 1; t <= nFrames; ++t) {
        if ((t == 2) && !wp.renderFile.empty()) {
            setOverrideString(&ctx, kOfxImageEffectFileParamName, wp.renderFile);
        }
        if ((t == 2) && (wp.renderW > 0)) {
            rw = wp.renderW;
            rh = wp.renderH;
            setupClipImage(src, rw, rh, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
            fp = reinterpret_cast<float*>(src->buffer.data());
            npix = (size_t)rw * rh;
        }
        for (int view : wp.views) {
            float v = wp.constantImage ? 0.5f : (float)t / (float)(nFrames + 1); // gray ramp, monotonic in t
            if (!wp.frameValues.empty()) v = wp.frameValues[(size_t)(t - 1) % wp.frameValues.size()];
            if (view != 0) v = 1.f - v;
            for (size_t i = 0; i < npix; ++i) { fp[i * 4 + 0] = v; fp[i * 4 + 1] = v; fp[i * 4 + 2] = v; fp[i * 4 + 3] = 1.f; }
            PropSet a = makeRenderInArgs((double)t, rw, rh);
            pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, view);
            OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
            if (!ok(st)) { std::printf("  FAIL: render frame %d view %d = %d\n", t, view, st); ++rfail; }
        }
    }

    {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        double fr[2] = { 1., (double)nFrames }; pSetDoubleN(ap, kOfxImageEffectPropFrameRange, 2, fr);
        pSetDouble(ap, kOfxImageEffectPropFrameStep, 0, 1.);
        pSetInt(ap, kOfxPropIsInteractive, 0, 0);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
        pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionEndSequenceRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        if (!ok(st)) { std::printf("  FAIL: endSequenceRender = %d\n", st); return 1; }
    }

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    std::FILE* f = std::fopen(outFile.c_str(), "rb");
    long sz = 0;
    if (f) { std::fseek(f, 0, SEEK_END); sz = std::ftell(f); std::fclose(f); }
    std::printf("  encoded %s (%ld bytes), render failures=%d\n", outFile.c_str(), sz, rfail);
    if (wp.expectRenderFailure) {
        const int expected = (nFrames - 1) * (int)wp.views.size();
        if ((rfail != expected) || (gPersistentError.find("another active render") == std::string::npos)) {
            std::printf("  FAIL: expected %d render failures, \"another active render\" (error: \"%s\")\n", expected, gPersistentError.c_str());
            return 1;
        }
        std::printf("  PASS (render failed as expected: %s)\n", gPersistentError.c_str());
        return 0;
    }
    if (rfail || sz <= 0) { std::printf("  FAIL (encode)\n"); return 1; }
    std::printf("  PASS (encode)\n");
    return 0;
}

// Decode the video stream of a file with libavformat/libavcodec: number of frames, and number of I frames.
int probeVideoFrames(const std::string& file, int* nFrames, int* nIFrames)
{
    *nFrames = *nIFrames = 0;
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) { std::printf("  FAIL: cannot open %s\n", file.c_str()); return 1; }
    int ret = 1;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const AVCodec* codec = nullptr;
    int si = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    }
    if (si >= 0 && codec && (dec = avcodec_alloc_context3(codec)) &&
        avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0) {
        auto drain = [&]() {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                ++*nFrames;
                if (frame->pict_type == AV_PICTURE_TYPE_I) ++*nIFrames;
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) drain();
            av_packet_unref(pkt);
        }
        avcodec_send_packet(dec, nullptr);
        drain();
        ret = 0;
    } else {
        std::printf("  FAIL: cannot decode the video stream of %s\n", file.c_str());
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ret;
}

// Decode the video stream of a file: its size, the bit depth of the decoded pixel format, and the first component
// (Y', or G for RGB formats) of each decoded frame at the given points (top-down pixel coordinates of the frame).
int probePixels(const std::string& file, const std::vector<std::pair<int, int>>& points, int* width, int* height,
                int* depth, std::vector<std::vector<int>>* values)
{
    *width = *height = *depth = 0;
    values->clear();
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) { std::printf("  FAIL: cannot open %s\n", file.c_str()); return 1; }
    int ret = 1;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const AVCodec* codec = nullptr;
    int si = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    }
    if (si >= 0 && codec && (dec = avcodec_alloc_context3(codec)) &&
        avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0) {
        auto drain = [&]() {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
                *width = frame->width;
                *height = frame->height;
                *depth = d ? d->comp[0].depth : 0;
                std::vector<int> v;
                for (const auto& pt : points) {
                    const uint8_t* row = frame->data[0] + (ptrdiff_t)pt.second * frame->linesize[0];
                    v.push_back(d && d->comp[0].depth > 8 ? (int)reinterpret_cast<const uint16_t*>(row)[pt.first] : (int)row[pt.first]);
                }
                values->push_back(v);
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) drain();
            av_packet_unref(pkt);
        }
        avcodec_send_packet(dec, nullptr);
        drain();
        ret = 0;
    } else {
        std::printf("  FAIL: cannot decode the video stream of %s\n", file.c_str());
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    return ret;
}

// The output RoD changes during the render (Natron sets the project format from the first Reader once the render
// has started): from the second frame on, the image is renderW x renderH, the movie W x H (the RoD at
// beginSequenceRender). WriteFFmpeg read the image as if it were W x H, out of its buffer if it is smaller
// (a crash in NatronRenderer). The part of the image in the frame must be written, the rest of the frame black.
int testWriteFFmpegRoDChanged(const std::string& outFile, int W, int H, int nFrames, int renderW, int renderH,
                              WriterParams wp = WriterParams())
{
    wp.renderW = renderW;
    wp.renderH = renderH;
    if (testWriteFFmpeg(outFile, W, H, nFrames, wp)) return 1;
    // a point in the image (bottom-left, the frame is top-down) and, if the image is smaller, one out of it (top-right)
    const int inX = (std::min)(W, renderW) / 2, inY = H - 1 - (std::min)(H, renderH) / 2;
    const bool smaller = (renderW < W) && (renderH < H);
    const int outX = (W + renderW) / 2, outY = (H - renderH) / 2;
    std::vector<std::pair<int, int>> points = { { inX, inY } };
    if (smaller) points.push_back({ outX, outY });
    int w = 0, h = 0, depth = 0;
    std::vector<std::vector<int>> v;
    if (probePixels(outFile, points, &w, &h, &depth, &v)) return 1;
    std::printf("  %dx%d, %d frames, %d bit, Y' in the image / out of it:", w, h, (int)v.size(), depth);
    for (const auto& f : v) std::printf(" %d/%d", f[0], smaller ? f[1] : -1);
    std::printf("\n");
    if ((w != W) || (h != H) || ((int)v.size() != nFrames) || (depth < 8)) { std::printf("  FAIL: expected %d frames of %dx%d\n", nFrames, W, H); return 1; }
    // ProRes and MPEG-4 are video range: black is 16 in 8 bit; the gray ramp of the source is above it, and increases
    const int black = 16 << (depth - 8), tolerance = 2 << (depth - 8);
    int failures = 0;
    for (int t = 1; t <= nFrames; ++t) {
        const std::vector<int>& f = v[(size_t)t - 1];
        if ((f[0] <= black + tolerance) || ((t > 1) && (f[0] <= v[(size_t)t - 2][0]))) {
            std::printf("  FAIL: frame %d: the image is not the gray ramp\n", t);
            ++failures;
        }
        if (smaller && (t > 1) && std::abs(f[1] - black) > tolerance) {
            std::printf("  FAIL: frame %d: the frame out of the image is not black\n", t);
            ++failures;
        }
        if (smaller && (t == 1) && std::abs(f[1] - f[0]) > tolerance) {
            std::printf("  FAIL: frame 1: the image covers the whole frame\n");
            ++failures;
        }
    }
    std::remove(outFile.c_str());
    if (failures) return 1;
    std::printf("  PASS\n");
    return 0;
}

// Offset and size of the first top-level QuickTime atom of the given type; offset -1 if not found.
long findTopLevelAtom(const std::string& file, const char* type, uint64_t* atomSize = nullptr)
{
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return -1;
    long pos = 0, found = -1;
    unsigned char h[16];
    while (std::fseek(f, pos, SEEK_SET) == 0 && std::fread(h, 1, 8, f) == 8) {
        uint64_t size = ((uint64_t)h[0] << 24) | ((uint64_t)h[1] << 16) | ((uint64_t)h[2] << 8) | h[3];
        if (size == 1) { // 64-bit size
            if (std::fread(h + 8, 1, 8, f) != 8) break;
            size = 0; for (int k = 8; k < 16; ++k) size = (size << 8) | h[k];
        }
        if (!std::memcmp(h + 4, type, 4)) { found = pos; if (atomSize) *atomSize = size; break; }
        if (size < 8) break;
        pos += (long)size;
    }
    std::fclose(f);
    return found;
}

// Whether the top-level moov atom contains the given 4-character code (e.g. the mdta handler of the
// metadata keys written with movflags use_metadata_tags). A byte search is enough for these small files.
bool moovContains(const std::string& file, const char* fourcc)
{
    uint64_t size = 0;
    const long moov = findTopLevelAtom(file, "moov", &size);
    if (moov < 0 || size < 8 || size > (1 << 24)) return false;
    std::vector<char> buf((size_t)size);
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    const bool read = std::fseek(f, moov, SEEK_SET) == 0 && std::fread(buf.data(), 1, buf.size(), f) == buf.size();
    std::fclose(f);
    return read && std::search(buf.begin(), buf.end(), fourcc, fourcc + 4) != buf.end();
}

// Issue #78: read the start timecode of a movie back with libavformat, as ffprobe shows it.
// The mov demuxer exposes the timecode (tmcd) track as a data stream with a "timecode" tag, and
// copies that tag to the video stream it refers to.
int checkTimecode(const std::string& file, const char* expected)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, file.c_str(), nullptr, nullptr) < 0) { std::printf("  FAIL: cannot open %s\n", file.c_str()); return 1; }
    std::string videoTc = "(none)", tmcdTc = "(none)";
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        const AVDictionaryEntry* e = av_dict_get(st->metadata, "timecode", nullptr, 0);
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && e) videoTc = e->value;
        if (st->codecpar->codec_tag == MKTAG('t', 'm', 'c', 'd') && e) tmcdTc = e->value;
    }
    avformat_close_input(&fc);
    std::printf("  %s: tmcd track timecode %s, video stream timecode %s (expected %s)\n", file.c_str(), tmcdTc.c_str(), videoTc.c_str(), expected);
    if (tmcdTc != expected || videoTc != expected) { std::printf("  FAIL (timecode)\n"); return 1; }
    std::printf("  PASS (timecode)\n");
    return 0;
}

// Issue #110: the frame rate of the video stream and of the timecode (tmcd) track of a movie, as
// ffprobe shows them (r_frame_rate of the video stream, and the timecode track's frame duration in
// its time base, which is the tmcd sample description's timescale and frame duration).
// If tolerance > 0, the frame rates must only be within this relative tolerance of num/den, and equal.
int checkFrameRate(const std::string& file, int num, int den, double tolerance = 0.)
{
    AVFormatContext* fc = nullptr;
    if (avformat_open_input(&fc, file.c_str(), nullptr, nullptr) < 0) { std::printf("  FAIL: cannot open %s\n", file.c_str()); return 1; }
    avformat_find_stream_info(fc, nullptr);
    AVRational video = { 0, 1 }, tmcd = { 0, 1 };
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) video = st->r_frame_rate;
        // the mov demuxer gives the tmcd track one packet per sample (the frame number of the first
        // frame), whose duration is the whole track: use its avg_frame_rate if set, else the time base
        if (st->codecpar->codec_tag == MKTAG('t', 'm', 'c', 'd')) tmcd = st->avg_frame_rate.num ? st->avg_frame_rate : av_inv_q(st->time_base);
    }
    avformat_close_input(&fc);
    std::printf("  %s: video stream at %d/%d fps, tmcd track at %d/%d (expected %d/%d)\n", file.c_str(), video.num, video.den, tmcd.num, tmcd.den, num, den);
    const AVRational expected = { num, den };
    const bool close = tolerance > 0. && video.den > 0 && std::fabs(av_q2d(video) / av_q2d(expected) - 1.) <= tolerance && av_cmp_q(tmcd, video) == 0;
    if (!close && (av_cmp_q(video, expected) != 0 || av_cmp_q(tmcd, expected) != 0)) { std::printf("  FAIL (frame rate)\n"); return 1; }
    std::printf("  PASS (frame rate)\n");
    return 0;
}

// Issue #78: write a movie with the given Start Timecode, at the given frame rate (0: the default).
// If expected is null, the timecode is invalid: beginSequenceRender must fail and write nothing.
// Otherwise, the timecode read back from the file must be expected ("(none)" for no timecode).
// base holds the other params (e.g. the codec and the Format Options).
int testStartTimecode(const std::string& outFile, const char* timecode, double fps, const char* expected,
                      const WriterParams& base = WriterParams())
{
    WriterParams wp = base;
    wp.strings.push_back({ "startTimecode", timecode });
    wp.fps = fps;
    if (!expected) {
        wp.expectBeginFailure = true;
        wp.expectNoFile = true;
        gMessages.clear();
        if (testWriteFFmpeg(outFile, 64, 48, 3, wp)) return 1;
        if (gMessages.find("Start Timecode") == std::string::npos) { std::printf("  FAIL: expected an error about the Start Timecode, got:\n%s", gMessages.c_str()); return 1; }
        return 0;
    }
    if (testWriteFFmpeg(outFile, 64, 48, 3, wp)) return 1;
    return checkTimecode(outFile, expected);
}

// Issue #78: an invalid Start Timecode is reported as soon as it is edited (changedParam), and
// the error is cleared when it is fixed.
int testStartTimecodeChangedParam(const std::string& outFile)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    if (!ctx.params.count("startTimecode")) { std::printf("  FAIL: WriteFFmpeg has no \"startTimecode\" param\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, 64, 48, 3, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    int fail = 0;
    const struct { const char* tc; bool valid; } cases[] = { { "01:02:03:99", false }, { "01:02:03:04", true }, { "+1:02:03:04", false }, { "", true } };
    for (const auto& c : cases) {
        setOverrideString(&ctx, "startTimecode", c.tc);
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, "startTimecode");
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        const bool reported = gPersistentError.find("Start Timecode") != std::string::npos;
        std::printf("  \"%s\": %s\n", c.tc, reported ? "error reported" : "no error");
        if (reported == c.valid) { std::printf("  FAIL (expected %s)\n", c.valid ? "no error" : "an error"); ++fail; }
    }
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (fail) return 1;
    std::printf("  PASS\n");
    return 0;
}

// A frame rate of 0 (the range of the FPS param starts at 0): the minimum bitrate tolerance, bitrate / min(fps, 4),
// was bitrate / 0 = inf, or 0 / 0 = NaN for a bitrate of 0, set as the range of the Bitrate Tolerance param (and
// converted to int when encoding). It is now bitrate / 4.
int testBitrateToleranceFps0()
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    if (!ctx.params.count("fps") || !ctx.params.count("bitrateMbps") || !ctx.params.count("bitrateToleranceMbps")) {
        std::printf("  FAIL: no fps, bitrateMbps or bitrateToleranceMbps param\n");
        return 1;
    }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, "/tmp/ofx_render_test_fps0.mov");
    ctx.params["fps"].hasOv = true;
    ctx.params["fps"].ov.d = { 0. };
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, 64, 48, 3, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    int fail = 0;
    const struct { double bitrate; double expected; } cases[] = { { 0., 0. }, { 10., 2.5 } };
    for (const auto& c : cases) {
        ctx.params["bitrateMbps"].hasOv = true;
        ctx.params["bitrateMbps"].ov.d = { c.bitrate };
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, "fps");
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        double vmin = -1.;
        pGetDouble(PH(ctx.params["bitrateToleranceMbps"].props), kOfxParamPropMin, 0, &vmin);
        std::printf("  fps 0, bitrate %g: minimum bitrate tolerance %g (expected %g)\n", c.bitrate, vmin, c.expected);
        if (!(vmin == c.expected)) { std::printf("  FAIL\n"); ++fail; }
    }
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (fail) return 1;
    std::printf("  PASS\n");
    return 0;
}

// Issue #110: with the Format "Default", the format is guessed from the filename param before the first
// render, e.g. ipod or m4v for a .m4v file, where no tmcd track is written: no 255 fps limit.
int testStartTimecodeDefaultFormat(const std::string& outFile, bool expectError)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    ctx.params["fps"].hasOv = true;
    ctx.params["fps"].ov.d = { 300. };
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, 64, 48, 3, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    setOverrideString(&ctx, "startTimecode", "01:00:00:00");
    PropSet a; OfxPropertySetHandle ap = PH(&a);
    pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
    pSetString(ap, kOfxPropName, 0, "startTimecode");
    pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
    pSetDouble(ap, kOfxPropTime, 0, 1.);
    double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
    const bool reported = gPersistentError.find("Start Timecode") != std::string::npos;
    std::printf("  %s at 300 fps: \"%s\"\n", outFile.c_str(), gPersistentError.c_str());
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (reported != expectError) { std::printf("  FAIL: expected %s\n", expectError ? "a Start Timecode error" : "no Start Timecode error"); return 1; }
    std::printf("  PASS\n");
    return 0;
}

// Issue #110: the persistent message shows all the errors in the settings (an invalid Start Timecode,
// a codec that the container does not support), whatever param is edited, and only these.
int testSettingsErrorsChangedParam(const std::string& outFile)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    const int mp4 = choiceIndex(ctx, "format", "[mp4]");
    const int prores = choiceIndex(ctx, "codec", "apch"); // ProRes 422 HQ: QuickTime only
    const int mpeg4 = choiceIndex(ctx, "codec", "mp4v");  // MPEG-4 part 2: QuickTime and MP4
    if (mp4 < 0 || prores < 0 || mpeg4 < 0) { std::printf("  FAIL: no mp4 format (%d), ProRes (%d) or MPEG-4 (%d) codec\n", mp4, prores, mpeg4); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    setOverrideInt(&ctx, "codec", prores);
    ctx.params["fps"].hasOv = true;
    ctx.params["fps"].ov.d = { 300. }; // a tmcd track can not have a timecode above 255 fps
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, 64, 48, 3, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    int fail = 0;
    // edit param, then check which errors the persistent message shows
    auto edit = [&](const char* what, const char* param, bool timecodeError, bool containerError) {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, param);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        const bool tc = gPersistentError.find("Start Timecode") != std::string::npos;
        const bool container = gPersistentError.find("is not supported in container") != std::string::npos;
        std::printf("  %s: \"%s\"\n", what, gPersistentError.c_str());
        if (tc != timecodeError || container != containerError || (gPersistentError.empty() != (!timecodeError && !containerError))) {
            std::printf("  FAIL: expected %s%s%s\n", timecodeError ? "the timecode error" : "", (timecodeError && containerError) ? " and " : "", containerError ? "the container error" : (timecodeError ? "" : "no error"));
            ++fail;
        }
    };
    setOverrideString(&ctx, "startTimecode", "01:00:00:00");
    edit("timecode at 300 fps in mov", "startTimecode", true, false);
    setOverrideInt(&ctx, "format", mp4);
    edit("Format mp4, which does not support ProRes", "format", true, true);
    ctx.params["fps"].ov.d = { 25. };
    edit("timecode fixed (FPS 25), the container error stays", "fps", false, true);
    ctx.params["fps"].ov.d = { 300. };
    edit("FPS 300 again: both errors", "fps", true, true);
    setOverrideInt(&ctx, "codec", mpeg4);
    edit("container fixed (Codec MPEG-4), the timecode error stays", "codec", true, false);
    edit("Pixel Coding edited, the timecode error stays", "prefPixelCoding", true, false);
    setOverrideString(&ctx, "codecOptions", "g=1");
    edit("Codec Options edited (a GenericWriter param), the timecode error stays", "codecOptions", true, false);
    setOverrideString(&ctx, "codecOptions", "g");
    edit("Codec Options syntax error: shown with the timecode error", "codecOptions", true, false);
    if (gPersistentError.find("Syntax error in Codec Options \"g\"") == std::string::npos) { std::printf("  FAIL: expected the Codec Options syntax error\n"); ++fail; }
    setOverrideString(&ctx, "codecOptions", "g=1");
    edit("Codec Options fixed", "codecOptions", true, false);
    if (gPersistentError.find("Syntax error") != std::string::npos) { std::printf("  FAIL: unexpected syntax error\n"); ++fail; }
    setOverrideString(&ctx, "formatOptions", "write_tmcd=0");
    edit("Format Options write_tmcd=0: no tmcd track, no error", "formatOptions", false, false);
    setOverrideString(&ctx, "formatOptions", "");
    edit("Format Options cleared: the timecode error is back", "formatOptions", true, false);
    setOverrideString(&ctx, "startTimecode", "");
    edit("timecode cleared: no error", "startTimecode", false, false);
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (fail) return 1;
    std::printf("  PASS\n");
    return 0;
}

// Issue #71: the Codec Options and Format Options params of WriteFFmpeg.
int testWriteFFmpegOptions(const std::string& outFile, int W, int H, int nFrames)
{
    int failures = 0;
    int n = 0, nI = 0;
    // MPEG-4 part 2 uses P frames; Keyframe Interval = 5 gives two I frames in 10 frames.
    WriterParams base;
    base.strings = { { "codecShortName", "mpeg4" } };
    base.ints = { { "gopSize", 5 } };
    base.constantImage = true; // with the ramp, each frame is a scene change, which gives an I frame

    auto check = [&](bool cond, const char* what) {
        if (!cond) { std::printf("  FAIL: %s\n", what); ++failures; }
    };
    // faststart: moov must precede mdat; mdta: the metadata is written as mdta keys (use_metadata_tags)
    auto checkAtoms = [&](bool faststart, bool mdta) {
        const long moov = findTopLevelAtom(outFile, "moov"), mdat = findTopLevelAtom(outFile, "mdat");
        const bool hasMdta = moovContains(outFile, "mdta");
        std::printf("  moov at %ld, mdat at %ld, mdta in moov: %s\n", moov, mdat, hasMdta ? "yes" : "no");
        check(moov >= 0 && mdat >= 0, "no moov or mdat atom");
        check(faststart ? moov < mdat : mdat < moov, faststart ? "moov must precede mdat (faststart)" : "mdat must precede moov (no faststart)");
        check(hasMdta == mdta, mdta ? "no mdta keys (use_metadata_tags)" : "unexpected mdta keys");
    };
    // encode with the given options; returns false if the encode or decode failed
    auto encode = [&](const char* codecOptions, const char* formatOptions, bool fastStartParam = false) {
        WriterParams wp = base;
        if (fastStartParam) wp.ints.push_back({ "fastStart", 1 });
        if (codecOptions) wp.strings.push_back({ "codecOptions", codecOptions });
        if (formatOptions) wp.strings.push_back({ "formatOptions", formatOptions });
        gMessages.clear();
        if (testWriteFFmpeg(outFile, W, H, nFrames, wp) || probeVideoFrames(outFile, &n, &nI)) return false;
        std::printf("  decoded %d frames, %d I frames\n", n, nI);
        return true;
    };
    // the render must fail in beginSequenceRender, with an error that mentions label
    auto expectFailure = [&](const char* codecOptions, const char* formatOptions, const char* label) {
        WriterParams wp = base;
        if (codecOptions) wp.strings.push_back({ "codecOptions", codecOptions });
        if (formatOptions) wp.strings.push_back({ "formatOptions", formatOptions });
        wp.expectBeginFailure = true;
        gMessages.clear();
        if (testWriteFFmpeg(outFile, W, H, nFrames, wp)) ++failures;
        if (gMessages.find(kOfxMessageError) == std::string::npos || gMessages.find(label) == std::string::npos) {
            std::printf("  FAIL: expected an error about %s, got:\n%s", label, gMessages.c_str());
            ++failures;
        }
    };

    std::printf("  -- reference: mpeg4, Keyframe Interval = 5, no options\n");
    if (!encode(nullptr, nullptr)) return 1;
    check(n == nFrames && nI < n, "expected P frames");
    checkAtoms(/*faststart=*/false, /*mdta=*/false);

    std::printf("  -- Codec Options \"g=1\" (overrides Keyframe Interval), Format Options \"movflags=+faststart\"\n");
    if (!encode("g=1", "movflags=+faststart")) return 1;
    check(n == nFrames && nI == n, "expected all I frames");
    checkAtoms(/*faststart=*/true, /*mdta=*/false);
    check(gMessages.find("not applied") == std::string::npos, "unexpected warning");

    // The plug-in movflags are write_colr+faststart when Fast Start is checked. (write_colr cannot be
    // checked: FFmpeg 8 writes a colr atom in MP4 whenever the color properties are known.)
    std::printf("  -- Fast Start checked, \"movflags=+use_metadata_tags\" is appended to the plug-in movflags\n");
    if (!encode(nullptr, "movflags=+use_metadata_tags", /*fastStartParam=*/true)) return 1;
    checkAtoms(/*faststart=*/true, /*mdta=*/true);

    std::printf("  -- Fast Start checked, \"movflags=use_metadata_tags\" replaces the plug-in movflags\n");
    if (!encode(nullptr, "movflags=use_metadata_tags", /*fastStartParam=*/true)) return 1;
    checkAtoms(/*faststart=*/false, /*mdta=*/true);

    std::printf("  -- several options, spaces as separators, and unknown keys\n");
    if (!encode(" g=1  bogus_codec_key=3 ", "bogus_format_key=4")) return 1;
    check(n == nFrames && nI == n, "expected all I frames");
    if (gMessages.find(kOfxMessageWarning) == std::string::npos ||
        gMessages.find("not applied by the mpeg4 encoder: bogus_codec_key=3") == std::string::npos ||
        gMessages.find("not applied by the mp4 muxer: bogus_format_key=4") == std::string::npos) {
        std::printf("  FAIL: expected a warning about bogus_codec_key and bogus_format_key, got:\n%s", gMessages.c_str());
        ++failures;
    }

    std::printf("  -- a backslash is literal in single quotes, an '=' may end a value\n");
    if (!encode("bogus_path='C:\\' g=1 bogus_eq=a=", nullptr)) return 1;
    check(n == nFrames && nI == n, "expected all I frames (g=1 must not be swallowed)");
    if (gMessages.find("bogus_path=C:\\") == std::string::npos || gMessages.find("bogus_eq=a=") == std::string::npos) {
        std::printf("  FAIL: expected a warning about bogus_path=C:\\ and bogus_eq=a=, got:\n%s", gMessages.c_str());
        ++failures;
    }

    // issue #109: as in av_get_token(), whitespace around '=' is ignored, where it is not ambiguous
    std::printf("  -- spaces around '=', a quoted value with '=' after a space\n");
    if (!encode("g = 1 bogus_sp= 'x=y:z'", "bogus_fmt =3")) return 1;
    check(n == nFrames && nI == n, "expected all I frames (g = 1)");
    if (gMessages.find("not applied by the mpeg4 encoder: bogus_sp=x=y:z\n") == std::string::npos ||
        gMessages.find("not applied by the mp4 muxer: bogus_fmt=3\n") == std::string::npos) {
        std::printf("  FAIL: expected a warning about bogus_sp=x=y:z and bogus_fmt=3, got:\n%s", gMessages.c_str());
        ++failures;
    }

    std::printf("  -- a leftover relative movflags is reported with the user value\n");
    {
        WriterParams wp = base;
        wp.strings.push_back({ "formatOptions", "movflags=+faststart" });
        // the avi muxer has no movflags option: the warning must not show "write_colr+faststart"
        const std::string aviFile = outFile + ".avi";
        gMessages.clear();
        if (testWriteFFmpeg(aviFile, W, H, nFrames, wp)) return 1;
        std::remove(aviFile.c_str());
        if (gMessages.find("not applied by the avi muxer: movflags=+faststart\n") == std::string::npos) {
            std::printf("  FAIL: expected a warning about movflags=+faststart, got:\n%s", gMessages.c_str());
            ++failures;
        }
    }

    std::printf("  -- syntax errors\n");
    expectFailure("g", nullptr, "Syntax error in Codec Options");
    // an empty value must not swallow the next pair (tune="preset=slow")
    expectFailure("bogus_empty= g=1", nullptr, "missing value after \"bogus_empty=\"");
    expectFailure("g=1 bogus_empty=", nullptr, "missing value after \"bogus_empty=\"");
    expectFailure(nullptr, "x264-params=\"keyint=1:bframes=0\"", "double quotes");
    // issue #109: a key without '=' must not be glued to the next pair ("tune:preset" = "slow")
    expectFailure("tune preset=slow", nullptr, "missing '=' after the key \"tune\" at character 1, found \"preset=slow\" at character 6");
    expectFailure(nullptr, "movflags +faststart", "Syntax error in Format Options");
    // no unquoted ':' or whitespace in a key (the key would be "g:" or "bogus_a:")
    expectFailure("g:=1", nullptr, "missing '=' after the key \"g\" at character 1, found ':' at character 2");
    expectFailure("bogus_a :=1", nullptr, "missing '=' after the key \"bogus_a\" at character 1, found ':' at character 9");
    // a '=' after a complete pair is not part of its value ({g: "1=2"} or {bogus_a: "1=2"})
    expectFailure("g=1 =2", nullptr, "missing key before '=' at character 5");
    expectFailure("bogus_a=1 =2", nullptr, "missing key before '=' at character 11");
    // an unterminated single quote must not take the rest of the string
    expectFailure("bogus_q='x g=1", nullptr, "unterminated single quote at character 9");
    std::printf("  -- invalid values: avcodec_open2 and avformat_init_output fail\n");
    expectFailure("g=abc", nullptr, "Could not open video codec");
    expectFailure(nullptr, "write_tmcd=abc", "Unable to initialize output");

    std::printf(failures ? "  FAIL (codec/format options)\n" : "  PASS (codec/format options)\n");
    return failures;
}

// Regression test for issue #90 with a writer that uses the start index: an Alpha-only source with only
// the A checkbox checked, written by WriteFFmpeg (which can't write alpha, so the channel is padded to RGB).
// render() passed packingMapping[0] = 3 as the start index, and WriteFFmpeg asserts that it is 0.
int testWriteFFmpegAlphaSource(const std::string& outFile, int W, int H, int nFrames)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    std::remove(outFile.c_str());

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }

    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, nFrames, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    {
        const char* const boxes[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
        for (int k = 0; k < 4; ++k) {
            auto it = ctx.params.find(boxes[k]);
            if (it != ctx.params.end()) {
                pSetInt(PH(it->second.props), kOfxParamPropSecret, 0, 0);
                it->second.hasOv = true;
                it->second.ov.i = { k == 3 ? 1 : 0 };
            }
        }
    }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    if (!src) { std::printf("  FAIL: no Source clip\n"); return 1; }
    src->connected = true;
    setupClipImage(src, W, H, kOfxImageComponentAlpha, kOfxBitDepthFloat, true);
    pSetString(PH(&src->props), kOfxImageEffectPropComponents, 0, kOfxImageComponentAlpha);
    pSetString(PH(&src->props), kOfxImageClipPropUnmappedComponents, 0, kOfxImageComponentAlpha);
    if (ctx.clips.count(kOfxImageEffectOutputClipName)) {
        Clip* o = ctx.clips[kOfxImageEffectOutputClipName];
        o->connected = false;
        setupClipImage(o, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
        pSetInt(PH(&o->props), kOfxImageClipPropConnected, 0, 0);
    }

    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); std::printf("  getClipPreferences = %d\n", cp); }

    {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        double fr[2] = { 1., (double)nFrames }; pSetDoubleN(ap, kOfxImageEffectPropFrameRange, 2, fr);
        pSetDouble(ap, kOfxImageEffectPropFrameStep, 0, 1.);
        pSetInt(ap, kOfxPropIsInteractive, 0, 0);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
        pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
        pSetInt(ap, kFnOfxImageEffectPropView, 0, 0);
        if (!ok(p->mainEntry(kOfxImageEffectActionBeginSequenceRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr))) { std::printf("  FAIL: beginSequenceRender\n"); return 1; }
    }

    float* fp = reinterpret_cast<float*>(src->buffer.data());
    const size_t npix = (size_t)W * H;
    int rfail = 0;
    for (int t = 1; t <= nFrames; ++t) {
        const float v = (float)t / (float)(nFrames + 1); // gray ramp, monotonic in t
        for (size_t i = 0; i < npix; ++i) { fp[i] = v; }
        PropSet a = makeRenderInArgs((double)t, W, H);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        if (!ok(st)) { std::printf("  FAIL: render frame %d = %d\n", t, st); ++rfail; }
    }

    {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        double fr[2] = { 1., (double)nFrames }; pSetDoubleN(ap, kOfxImageEffectPropFrameRange, 2, fr);
        pSetDouble(ap, kOfxImageEffectPropFrameStep, 0, 1.);
        pSetInt(ap, kOfxPropIsInteractive, 0, 0);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
        pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionEndSequenceRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        if (!ok(st)) { std::printf("  FAIL: endSequenceRender = %d\n", st); return 1; }
    }

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    std::FILE* f = std::fopen(outFile.c_str(), "rb");
    long sz = 0;
    if (f) { std::fseek(f, 0, SEEK_END); sz = std::ftell(f); std::fclose(f); }
    std::printf("  encoded %s (%ld bytes), render failures=%d\n", outFile.c_str(), sz, rfail);
    if (rfail || sz <= 0) { std::printf("  FAIL (encode)\n"); return 1; }
    std::printf("  PASS (encode)\n");
    return 0;
}

int testReadFFmpeg(const std::string& inFile, int W, int H, int nFrames)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }

    setOverrideString(&ctx, kOfxImageEffectFileParamName, inFile);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, nFrames, kOfxImageEffectContextReader);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }

    // Notify the plugin that the filename changed, so it runs guessParamsFromFilename and
    // initializes its frame-range / starting-time params (otherwise getSequenceTime maps
    // time 1 -> frame 0, which is out of the 1-based decode range).
    {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
    }

    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) { std::printf("  FAIL: no Output clip\n"); return 1; }
    out->connected = true;
    const int outComps = 3; // reader's getClipPreferences requests RGB for opaque video
    setupClipImage(out, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);

    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); if (!ok(cp)) { std::printf("  FAIL: getClipPreferences = %d\n", cp); return 1; } }
    float* fp = reinterpret_cast<float*>(out->buffer.data());
    const size_t npix = (size_t)W * H;
    double prev = -1.0;
    int vfail = 0, rfail = 0;
    for (int t = 1; t <= nFrames; ++t) {
        for (size_t i = 0; i < out->buffer.size() / 4; ++i) fp[i] = 0.5f; // sentinel
        {
            PropSet a; pSetDouble(PH(&a), kOfxPropTime, 0, (double)t);
            double rs[2] = { 1., 1. }; pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
            pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
            PropSet o; OfxStatus rst = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), PH(&o));
            double rod[4] = { -1, -1, -1, -1 }; pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, rod);
            if (t == 1 && (!ok(rst) || rod[2] != W || rod[3] != H)) {
                std::printf("  FAIL: getRegionOfDefinition=%d rod=[%.0f %.0f %.0f %.0f] (expected 0 0 %d %d)\n", rst, rod[0], rod[1], rod[2], rod[3], W, H);
                ++rfail;
            }
        }
        PropSet a = makeRenderInArgs((double)t, W, H);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        if (!ok(st)) { std::printf("  FAIL: read render frame %d = %d\n", t, st); ++rfail; continue; }
        double sum = 0;
        for (size_t i = 0; i < npix; ++i) sum += fp[i * outComps + 0];
        double mean = sum / (double)npix;
        const double expected = (double)t / (double)(nFrames + 1);
        std::printf("  frame %d decoded mean(R)=%.4f (expected ~%.4f)\n", t, mean, expected);
        // The decoded gray level must both ramp up and match the encoded source value
        // (N/(N+1)) within tolerance, so a monotonic-but-wrong transform can't pass.
        if (mean <= prev || std::fabs(mean - expected) > 0.02) ++vfail;
        prev = mean;
    }

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    if (rfail || vfail) { std::printf("  FAIL (decode: renderFails=%d monotonicViolations=%d)\n", rfail, vfail); return 1; }
    std::printf("  PASS (decode round-trip)\n");
    return 0;
}

// Regression test for https://github.com/NatronGitHub/openfx-io/issues/134:
// ReadFFmpeg read GenericReader's "onMissingFrame" choice with an old numbering, so with the
// default (Error) a frame that is not in the movie returned kOfxStatOK and left the image
// unwritten, and Black did the same. The frame range is extended to [0, nFrames+3], and
// frames 0 (before the first frame of the movie) and nFrames+3 (after the last one) are
// rendered with each option:
// - Hold previous, Load next, Load nearest: the nearest frame of the movie (FFmpegFile can
//   only clamp to the first or last frame) is rendered,
// - Error: the render fails with a persistent error message,
// - Black: the render succeeds and the image is transparent black (0,0,0,0), as for a missing
//   file in an image sequence (with RGBA output, the alpha of the opaque movie is not set to 1).
// With truncated (the movie file is truncated, so that its last frame can not be decoded),
// frame 1 and frame nFrames, which are inside of the movie, are rendered instead: frame 1 must
// be decoded, and frame nFrames must fail with a persistent error message whatever the option
// (a frame that is in the movie but fails to decode is not a missing frame).
// The output image is filled with NaN before each render, and no NaN may remain on success.
// With halfScale, the frames are rendered at render scale 0.5: GenericReader::render() decodes into a
// temporary full-resolution buffer and downscales it (the Black option must also give transparent black there).
int testReadFFmpegMissingFrame(const std::string& inFile, int W, int H, int nFrames, bool rgba, bool truncated = false, bool halfScale = false)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    const int optPrevious = choiceIndex(ctx, "onMissingFrame", "previous");
    const int optNext = choiceIndex(ctx, "onMissingFrame", "next");
    const int optNearest = choiceIndex(ctx, "onMissingFrame", "nearest");
    const int optError = choiceIndex(ctx, "onMissingFrame", "Error");
    const int optBlack = choiceIndex(ctx, "onMissingFrame", "Black");
    const int optRGBA = choiceIndex(ctx, "outputComponents", "RGBA");
    if (optPrevious < 0 || optNext < 0 || optNearest < 0 || optError < 0 || optBlack < 0 || optRGBA < 0) {
        std::printf("  FAIL: onMissingFrame or outputComponents options not found (%d %d %d %d %d %d)\n", optPrevious, optNext, optNearest, optError, optBlack, optRGBA);
        return 1;
    }

    setOverrideString(&ctx, kOfxImageEffectFileParamName, inFile);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, nFrames, kOfxImageEffectContextReader);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    {
        PropSet a; OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
        pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
    }
    // read frames outside of the movie (the first frame of a movie is 1), at the same time
    setOverrideInt(&ctx, "firstFrame", 0);
    setOverrideInt(&ctx, "lastFrame", nFrames + 3);
    setOverrideInt(&ctx, "timeOffset", 0);
    if (rgba) {
        setOverrideInt(&ctx, "outputComponents", optRGBA);
    }

    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) { std::printf("  FAIL: no Output clip\n"); return 1; }
    out->connected = true;
    const int outComps = rgba ? 4 : 3;
    const int OW = halfScale ? W / 2 : W, OH = halfScale ? H / 2 : H; // the output image, at the render scale
    const double scale = halfScale ? 0.5 : 1.;
    setupClipImage(out, OW, OH, rgba ? kOfxImageComponentRGBA : kOfxImageComponentRGB, kOfxBitDepthFloat, true);
    // the clip's RoD is in canonical coordinates (independent of the render scale)
    double clipRod[4] = { 0., 0., (double)W, (double)H };
    pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, clipRod);
    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); if (!ok(cp)) { std::printf("  FAIL: getClipPreferences = %d\n", cp); return 1; } }
    float* fp = reinterpret_cast<float*>(out->buffer.data());
    const size_t npix = (size_t)OW * OH;
    const size_t nval = npix * outComps;

    const struct { const char* name; int option; } options[] = {
        { "Hold previous", optPrevious }, { "Load next", optNext }, { "Load nearest", optNearest },
        { "Error", optError }, { "Black", optBlack },
    };
    const std::vector<int> frames = truncated ? std::vector<int>{ 1, nFrames } : std::vector<int>{ 0, nFrames + 3 };
    int fails = 0;
    for (const auto& o : options) {
        setOverrideInt(&ctx, "onMissingFrame", o.option);
        for (int t : frames) {
            fillUnwritten(fp, nval);
            gPersistentError.clear();
            PropSet a = makeRenderInArgs((double)t, OW, OH);
            double rs[2] = { scale, scale };
            pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
            pSetDoubleN(PH(&out->image), kOfxImageEffectPropRenderScale, 2, rs);
            OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
            size_t nNaN = 0;
            double sum = 0, alphaMin = 1e30, alphaMax = -1e30;
            for (size_t i = 0; i < npix; ++i) {
                for (int c = 0; c < outComps; ++c) {
                    const float v = fp[i * outComps + c];
                    if (isUnwritten(&fp[i * outComps + c])) {
                        ++nNaN;
                    } else if (c < 3) {
                        sum += v;
                    } else {
                        alphaMin = (std::min)(alphaMin, (double)v);
                        alphaMax = (std::max)(alphaMax, (double)v);
                    }
                }
            }
            const double mean = (nNaN == nval) ? 0. : sum / (double)(npix * 3);
            std::printf("  %s, frame %d: status=%d NaN=%zu mean(RGB)=%.4f", o.name, t, st, nNaN, mean);
            if (rgba) std::printf(" alpha=[%g, %g]", alphaMin, alphaMax);
            std::printf(" error=\"%s\"\n", gPersistentError.c_str());
            // the frame that is rendered: -1 if the render must fail, 0 if black, else the frame of the movie
            int expectedFrame;
            if (truncated) {
                expectedFrame = (t == 1) ? 1 : -1;
            } else if (o.option == optError) {
                expectedFrame = -1;
            } else if (o.option == optBlack) {
                expectedFrame = 0;
            } else {
                expectedFrame = (t <= 0) ? 1 : nFrames; // the nearest frame
            }
            if (expectedFrame < 0) {
                if (st != kOfxStatFailed || gPersistentError.empty()) { std::printf("  FAIL: expected kOfxStatFailed and an error message\n"); ++fails; }
                continue;
            }
            if (st != kOfxStatOK) { std::printf("  FAIL: expected kOfxStatOK\n"); ++fails; continue; }
            if (nNaN) { std::printf("  FAIL: %zu of %zu values were not written\n", nNaN, nval); ++fails; continue; }
            // frame f is gray f/(N+1)
            const double expected = (double)expectedFrame / (double)(nFrames + 1);
            if (std::fabs(mean - expected) > 0.02) { std::printf("  FAIL: mean %.4f, expected ~%.4f\n", mean, expected); ++fails; }
            // black is transparent, a frame of the (opaque) movie has alpha 1
            const double expectedAlpha = (expectedFrame == 0) ? 0. : 1.;
            if (rgba && (alphaMin != expectedAlpha || alphaMax != expectedAlpha)) { std::printf("  FAIL: alpha must be %g\n", expectedAlpha); ++fails; }
        }
    }

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    gPersistentError.clear();

    if (fails) { std::printf("  FAIL (%d)\n", fails); return 1; }
    std::printf("  PASS\n");
    return 0;
}

// Writes a copy of the movie inFile (with the moov atom before the mdat atom) to outFile, truncated in the middle
// of its mdat atom: its last frames are listed in the moov atom, but can not be decoded.
int truncateMovie(const std::string& inFile, const std::string& outFile)
{
    const long moov = findTopLevelAtom(inFile, "moov"), mdat = findTopLevelAtom(inFile, "mdat");
    if (moov < 0 || mdat < 0 || moov > mdat) { std::printf("  FAIL: %s: expected a moov atom before the mdat atom (moov at %ld, mdat at %ld)\n", inFile.c_str(), moov, mdat); return 1; }
    FILE* f = std::fopen(inFile.c_str(), "rb");
    if (!f) { std::printf("  FAIL: cannot open %s\n", inFile.c_str()); return 1; }
    std::vector<char> data;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    const size_t size = (size_t)mdat + ((data.size() - (size_t)mdat) / 2); // keep half of the mdat atom
    FILE* g = std::fopen(outFile.c_str(), "wb");
    if (!g || std::fwrite(data.data(), 1, size, g) != size) { std::printf("  FAIL: cannot write %s\n", outFile.c_str()); if (g) std::fclose(g); return 1; }
    std::fclose(g);
    std::printf("  truncated %s (%zu bytes, mdat at %ld) to %zu bytes: %s\n", inFile.c_str(), data.size(), mdat, size, outFile.c_str());
    return 0;
}

// Regression test for https://github.com/NatronGitHub/openfx-io/issues/79:
// WriteOIIO segfaulted in OIIO's contiguize() when writing a JPEG whose render window
// (the output format) does not intersect the input RoD (e.g. Crop outside of the format).
// The host then fails clipGetImage with kOfxStatFailed, and the OFX spec says the plugin
// "should continue operation, but assume the image was black and transparent".
// GenericWriter used to call encode() with a NULL pixel pointer and an uninitialized rowBytes.
// Expected: render succeeds and writes a black W x H image.
// With channelCheckboxes, the R/G/B/A "process" checkboxes are visible as in Natron, so
// GenericWriter packs the channels (doAnyPacking); otherwise they are hidden.
// overlappingRoD: the input RoD intersects the render window (e.g. a time outside of the input's frame range,
// or a gap in a timeline, with the project's RoD): per the spec, a black image is written too.
// outputConnected: the output image must be filled with black too.
// aborted: the host is aborting the render: nothing is written (the previous file is kept).
int testWriteOIIONoInputImage(const std::string& outFile, int W, int H, bool channelCheckboxes,
                              bool overlappingRoD = false, bool outputConnected = false, bool aborted = false)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteOIIO");
    if (!p) { std::printf("  FAIL: WriteOIIO not found\n"); return 1; }
    std::remove(outFile.c_str());

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, 1, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    for (const char* name : { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA }) {
        auto it = ctx.params.find(name);
        if (it != ctx.params.end()) {
            pSetInt(PH(it->second.props), kOfxParamPropSecret, 0, channelCheckboxes ? 0 : 1);
        }
    }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    if (!src) { std::printf("  FAIL: no Source clip\n"); return 1; }
    src->connected = true;
    setupClipImage(src, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    // Input RoD entirely to the right of the render window, as in the issue's project
    // (format 720x1280, Crop RoD 727,19 -> 1450,1277): the host has no image to give.
    double rod[4] = { W + 7., 19., 2. * W + 10., H - 3. };
    if (overlappingRoD) {
        rod[0] = W / 2.; // overlaps the right half of the render window
    }
    pSetDoubleN(PH(&src->props), kOfxImageEffectPropRegionOfDefinition, 4, rod);
    src->noImage = true;
    Clip* o = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (o) {
        o->connected = outputConnected;
        setupClipImage(o, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
        std::fill((float*)o->buffer.data(), (float*)o->buffer.data() + (size_t)W * H * 4, 0.25f);
    }
    if (aborted) {
        // a file from a previous render, which must not be replaced by a black image
        std::FILE* f = std::fopen(outFile.c_str(), "wb");
        if (f) { std::fputs("previous", f); std::fclose(f); }
    }

    { PropSet outArgs; p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); }

    PropSet a = makeRenderInArgs(1., W, H);
    gAbort = aborted ? 1 : 0;
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
    gAbort = 0;

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    if (aborted) {
        std::FILE* f = std::fopen(outFile.c_str(), "rb");
        char buf[16] = { 0 };
        const bool kept = f && std::fread(buf, 1, 8, f) == 8 && std::strncmp(buf, "previous", 8) == 0;
        if (f) std::fclose(f);
        std::printf("  render = %d, previous file %s\n", st, kept ? "kept" : "REPLACED");
        if (!kept) { std::printf("  FAIL (expected the previous file kept)\n"); return 1; }
        std::printf("  PASS\n");
        return 0;
    }
    if (!ok(st)) { std::printf("  FAIL: render = %d\n", st); return 1; }
    if (o && outputConnected) {
        float outMax = 0.f;
        for (size_t i = 0; i < (size_t)W * H * 4; ++i) outMax = std::max(outMax, ((const float*)o->buffer.data())[i]);
        std::printf("  output image max value %g\n", outMax);
        if (outMax != 0.f) { std::printf("  FAIL (expected a black output image)\n"); return 1; }
    }

    auto in = OIIO::ImageInput::open(outFile);
    if (!in) { std::printf("  FAIL: cannot read back %s: %s\n", outFile.c_str(), OIIO::geterror().c_str()); return 1; }
    const OIIO::ImageSpec& spec = in->spec();
    std::vector<unsigned char> pix((size_t)spec.width * spec.height * spec.nchannels);
    bool rd = in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, pix.data());
    in->close();
    int maxv = 0;
    for (unsigned char v : pix) maxv = std::max(maxv, (int)v);
    std::printf("  wrote %s: %dx%d, %d channels, max value %d\n", outFile.c_str(), spec.width, spec.height, spec.nchannels, maxv);
    if (!rd || spec.width != W || spec.height != H || maxv > 2) { std::printf("  FAIL (expected a black %dx%d image)\n", W, H); return 1; }
    std::printf("  PASS\n");
    return 0;
}

// Regression test for issue #90: the R/G/B/A checkboxes select a subset of the source channels.
// After fetchPlaneConvertAndCopy() the buffer always starts at component 0, but render()
// passed packingMapping[0] as the start index, and a contiguous selection with as many
// channels as the source (e.g. G,B,A of an RGB source) was not packed at all, so the writer
// read past the end of each pixel. checked[] are the R/G/B/A checkbox values, expected[] the
// expected 8-bit values of the written channels.
int testWriteOIIOChannelSelection(const std::string& outFile, int W, int H, const char* srcComps,
                                  const float srcVals[4], const bool checked[4],
                                  int expectedNChannels, const int expected[4])
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteOIIO");
    if (!p) { std::printf("  FAIL: WriteOIIO not found\n"); return 1; }
    std::remove(outFile.c_str());

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, 1, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    const char* const boxes[4] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
    for (int k = 0; k < 4; ++k) {
        auto it = ctx.params.find(boxes[k]);
        if (it != ctx.params.end()) {
            pSetInt(PH(it->second.props), kOfxParamPropSecret, 0, 0);
            it->second.hasOv = true;
            it->second.ov.i = { checked[k] ? 1 : 0 };
        }
    }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    if (!src) { std::printf("  FAIL: no Source clip\n"); return 1; }
    src->connected = true;
    setupClipImage(src, W, H, srcComps, kOfxBitDepthFloat, true);
    const int nc = (!std::strcmp(srcComps, kOfxImageComponentRGBA)) ? 4 : (!std::strcmp(srcComps, kOfxImageComponentAlpha) ? 1 : 3);
    float* sp = (float*)src->buffer.data();
    for (size_t i = 0; i < (size_t)W * H; ++i) for (int c = 0; c < nc; ++c) sp[i * nc + c] = srcVals[c];
    pSetString(PH(&src->props), kOfxImageEffectPropComponents, 0, srcComps);
    pSetString(PH(&src->props), kOfxImageClipPropUnmappedComponents, 0, srcComps);
    if (ctx.clips.count(kOfxImageEffectOutputClipName)) {
        ctx.clips[kOfxImageEffectOutputClipName]->connected = false;
    }

    { PropSet outArgs; p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); }

    PropSet a = makeRenderInArgs(1., W, H);
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    if (!ok(st)) { std::printf("  FAIL: render = %d\n", st); return 1; }

    auto in = OIIO::ImageInput::open(outFile);
    if (!in) { std::printf("  FAIL: cannot read back %s: %s\n", outFile.c_str(), OIIO::geterror().c_str()); return 1; }
    const OIIO::ImageSpec& spec = in->spec();
    std::vector<unsigned char> pix((size_t)spec.width * spec.height * spec.nchannels);
    bool rd = in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, pix.data());
    in->close();
    int fail = (!rd || spec.width != W || spec.height != H || spec.nchannels != expectedNChannels) ? 1 : 0;
    int maxErr = 0;
    for (size_t i = 0; !fail && i < pix.size(); ++i) {
        maxErr = std::max(maxErr, std::abs((int)pix[i] - expected[i % spec.nchannels]));
    }
    std::printf("  wrote %s: %dx%d, %d channels, first pixel", outFile.c_str(), spec.width, spec.height, spec.nchannels);
    for (int c = 0; c < spec.nchannels && !pix.empty(); ++c) std::printf(" %d", (int)pix[c]);
    std::printf(", max error %d\n", maxErr);
    if (fail || maxErr > 1) { std::printf("  FAIL (expected %d channels:", expectedNChannels); for (int c = 0; c < expectedNChannels; ++c) std::printf(" %d", expected[c]); std::printf(")\n"); return 1; }
    std::printf("  PASS\n");
    return 0;
}

// Second regression test for issue #79: the host returns a valid source image, but its
// bounds do not intersect the render window. GenericWriter returned early from
// fetchPlaneConvertAndCopy(): the output image was not filled, and the channel packing was
// skipped, so encode() got a buffer with fewer components than the channels to write.
// Here the source is RGB and the four R/G/B/A checkboxes are visible and checked, so 4
// channels are written from a 3-component buffer (a heap overflow, caught by ASan). PNG
// is used because it writes all 4 channels (the JPEG writer only reads 3 of them).
// Expected: a black W x H image (alpha is opaque, since the RGB source is opaque), and a
// black output image over the render window.
// outputMissing: the host gives no output image: the render fails, and nothing is written (the previous file is
// kept). aborted: same, while the host is aborting the render (no error, nothing written).
int testWriteOIIODisjointInputImage(const std::string& outFile, int W, int H, bool aborted = false, bool outputMissing = false)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteOIIO");
    if (!p) { std::printf("  FAIL: WriteOIIO not found\n"); return 1; }
    std::remove(outFile.c_str());

    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, 1, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    for (const char* name : { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA }) {
        auto it = ctx.params.find(name);
        if (it != ctx.params.end()) {
            pSetInt(PH(it->second.props), kOfxParamPropSecret, 0, 0);
        }
    }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    if (!src) { std::printf("  FAIL: no Source clip\n"); return 1; }
    src->connected = true;
    // A gray RGB source image whose bounds are entirely to the right of the render window.
    const int sw = W / 2, sh = H / 2;
    setupClipImage(src, sw, sh, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
    std::fill((float*)src->buffer.data(), (float*)src->buffer.data() + (size_t)sw * sh * 3, 0.5f);
    const int sb[4] = { W + 7, 19, W + 7 + sw, 19 + sh };
    pSetIntN(PH(&src->image), kOfxImagePropBounds, 4, sb);
    pSetIntN(PH(&src->image), kOfxImagePropRegionOfDefinition, 4, sb);
    const double rod[4] = { (double)sb[0], (double)sb[1], (double)sb[2], (double)sb[3] };
    pSetDoubleN(PH(&src->props), kOfxImageEffectPropRegionOfDefinition, 4, rod);

    Clip* o = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!o) { std::printf("  FAIL: no Output clip\n"); return 1; }
    o->connected = true;
    setupClipImage(o, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
    std::fill((float*)o->buffer.data(), (float*)o->buffer.data() + (size_t)W * H * 3, 0.25f);
    if (aborted || outputMissing) {
        o->noImage = true; // e.g. as Natron does for an aborted render
        std::FILE* f = std::fopen(outFile.c_str(), "wb");
        if (f) { std::fputs("previous", f); std::fclose(f); }
    }

    { PropSet outArgs; p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); }

    PropSet a = makeRenderInArgs(1., W, H);
    gAbort = aborted ? 1 : 0;
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
    gAbort = 0;

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    if (aborted || outputMissing) {
        std::FILE* f = std::fopen(outFile.c_str(), "rb");
        char buf[16] = { 0 };
        const bool kept = f && std::fread(buf, 1, 8, f) == 8 && std::strncmp(buf, "previous", 8) == 0;
        if (f) std::fclose(f);
        std::printf("  render = %d, previous file %s\n", st, kept ? "kept" : "REPLACED");
        if (!kept) { std::printf("  FAIL (expected the previous file kept)\n"); return 1; }
        if (!aborted && ok(st)) { std::printf("  FAIL (expected a failed render: no output image)\n"); return 1; }
        std::printf("  PASS\n");
        return 0;
    }

    if (!ok(st)) { std::printf("  FAIL: render = %d\n", st); return 1; }

    int fail = 0;
    float outMax = 0.f;
    for (size_t i = 0; i < (size_t)W * H * 3; ++i) outMax = std::max(outMax, ((const float*)o->buffer.data())[i]);
    std::printf("  output image max value %g\n", outMax);
    if (outMax != 0.f) { std::printf("  FAIL (expected a black output image)\n"); ++fail; }

    auto in = OIIO::ImageInput::open(outFile);
    if (!in) { std::printf("  FAIL: cannot read back %s: %s\n", outFile.c_str(), OIIO::geterror().c_str()); return 1; }
    const OIIO::ImageSpec& spec = in->spec();
    std::vector<unsigned char> pix((size_t)spec.width * spec.height * spec.nchannels);
    bool rd = in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, pix.data());
    in->close();
    int maxv = 0;
    for (size_t i = 0; i < pix.size(); ++i) if ((int)(i % spec.nchannels) < 3) maxv = std::max(maxv, (int)pix[i]);
    std::printf("  wrote %s: %dx%d, %d channels, max RGB value %d\n", outFile.c_str(), spec.width, spec.height, spec.nchannels, maxv);
    if (!rd || spec.width != W || spec.height != H || spec.nchannels != 4 || maxv > 2) { std::printf("  FAIL (expected a black %dx%d RGBA image)\n", W, H); ++fail; }
    if (fail) return 1;
    std::printf("  PASS\n");
    return 0;
}

// Issue #135: the properties of the video stream of a movie, as ffprobe -show_streams shows them (after
// avformat_find_stream_info, which fills bits_per_raw_sample and pix_fmt from the decoder), and the first
// component (Y', or G for RGB formats) at the center of each decoded frame, in the decoded pixel format.
struct StreamInfo {
    std::string codec = "none", pixFmt = "none", tag;
    int bitsPerRawSample = 0;
    AVColorSpace space = AVCOL_SPC_UNSPECIFIED;
    AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
    AVColorPrimaries primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorTransferCharacteristic trc = AVCOL_TRC_UNSPECIFIED;
    std::vector<int> center; // per decoded frame
};

int probeStream(const std::string& file, StreamInfo* info)
{
    *info = StreamInfo();
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) { std::printf("  FAIL: cannot open %s\n", file.c_str()); return 1; }
    int ret = 1;
    AVCodecContext* dec = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const AVCodec* codec = nullptr;
    int si = -1;
    if (avformat_find_stream_info(fmt, nullptr) >= 0) {
        si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    }
    if (si >= 0) {
        const AVCodecParameters* par = fmt->streams[si]->codecpar;
        info->codec = avcodec_get_name(par->codec_id);
        const char* name = av_get_pix_fmt_name((AVPixelFormat)par->format);
        info->pixFmt = name ? name : "none";
        char tag[AV_FOURCC_MAX_STRING_SIZE] = { 0 };
        info->tag = av_fourcc_make_string(tag, par->codec_tag);
        info->bitsPerRawSample = par->bits_per_raw_sample;
        info->space = par->color_space;
        info->range = par->color_range;
        info->primaries = par->color_primaries;
        info->trc = par->color_trc;
    }
    if (si >= 0 && codec && (dec = avcodec_alloc_context3(codec)) &&
        avcodec_parameters_to_context(dec, fmt->streams[si]->codecpar) >= 0 &&
        avcodec_open2(dec, codec, nullptr) >= 0) {
        auto drain = [&]() {
            while (avcodec_receive_frame(dec, frame) >= 0) {
                const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
                const int x = frame->width / 2, y = frame->height / 2;
                const uint8_t* row = frame->data[0] + (ptrdiff_t)y * frame->linesize[0];
                info->center.push_back(d && d->comp[0].depth > 8 ? (int)reinterpret_cast<const uint16_t*>(row)[x] : (int)row[x]);
                av_frame_unref(frame);
            }
        };
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index == si && avcodec_send_packet(dec, pkt) >= 0) drain();
            av_packet_unref(pkt);
        }
        avcodec_send_packet(dec, nullptr);
        drain();
        ret = 0;
    } else {
        std::printf("  FAIL: cannot decode the video stream of %s\n", file.c_str());
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    const char* sp = av_color_space_name(info->space);
    const char* rg = av_color_range_name(info->range);
    const char* pr = av_color_primaries_name(info->primaries);
    const char* tr = av_color_transfer_name(info->trc);
    std::printf("  %s: codec %s (%s), pix_fmt %s, bits_per_raw_sample %d, color_space %s, color_range %s, color_primaries %s, color_transfer %s, center",
                file.c_str(), info->codec.c_str(), info->tag.c_str(), info->pixFmt.c_str(), info->bitsPerRawSample,
                sp ? sp : "?", rg ? rg : "?", pr ? pr : "?", tr ? tr : "?");
    for (int c : info->center) std::printf(" %d", c);
    std::printf("\n");
    return ret;
}

// Issue #135: the color codes of the first ProRes frame header (SMPTE RDD 36): the frame header follows the
// 'icpf' frame identifier; its color_primaries, transfer_characteristic and matrix_coefficients bytes are at
// offsets 14-16 (after the header size, version, creator, width, height, flags and a reserved byte).
// Returns false if there is no ProRes frame.
bool proResFrameHeaderColors(const std::string& file, int* primaries, int* trc, int* matrix)
{
    std::FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) return false;
    std::vector<char> buf;
    char chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(f);
    const char id[4] = { 'i', 'c', 'p', 'f' };
    auto it = std::search(buf.begin(), buf.end(), id, id + 4);
    if (it == buf.end() || buf.end() - it < 4 + 17) return false;
    const unsigned char* h = reinterpret_cast<const unsigned char*>(&*(it + 4));
    *primaries = h[14];
    *trc = h[15];
    *matrix = h[16];
    return true;
}

// Expected stream properties for testWriteFFmpegColor(); an empty string or a negative value is not checked.
struct ExpectedStream {
    std::string tag, pixFmt;
    int bitsPerRawSample = -1;
    int space = -1, range = -1, primaries = -1, trc = -1;
    bool rangeNotFull = false; // the range must not be full (it may be unspecified or limited)
    bool proResHeader = false; // the ProRes frame header has the same primaries, transfer and matrix
    std::vector<int> center;   // the first component at the center of each decoded frame, +-tolerance
    int tolerance = 0;
};

// Issue #135: encode a black frame then a white frame, and check the stream properties and the decoded levels.
int testWriteFFmpegColor(const std::string& outFile, int W, int H, WriterParams wp, const ExpectedStream& e)
{
    wp.frameValues = { 0.f, 1.f };
    if (testWriteFFmpeg(outFile, W, H, 2, wp)) return 1;
    StreamInfo s;
    if (probeStream(outFile, &s)) return 1;
    int failures = 0;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); ++failures; }
    };
    if (!e.tag.empty()) check(s.tag == e.tag, "the codec tag must be " + e.tag);
    if (!e.pixFmt.empty()) check(s.pixFmt == e.pixFmt, "pix_fmt must be " + e.pixFmt);
    if (e.bitsPerRawSample >= 0) check(s.bitsPerRawSample == e.bitsPerRawSample, "bits_per_raw_sample must be " + std::to_string(e.bitsPerRawSample));
    if (e.space >= 0) check(s.space == e.space, std::string("color_space must be ") + av_color_space_name((AVColorSpace)e.space));
    if (e.range >= 0) check(s.range == e.range, std::string("color_range must be ") + av_color_range_name((AVColorRange)e.range));
    if (e.rangeNotFull) check(s.range != AVCOL_RANGE_JPEG, "color_range must not be pc (full range)");
    if (e.proResHeader) {
        int pr = -1, tr = -1, mx = -1;
        const bool found = proResFrameHeaderColors(outFile, &pr, &tr, &mx);
        std::printf("  ProRes frame header: color_primaries %d, transfer_characteristic %d, matrix_coefficients %d\n", pr, tr, mx);
        check(found && pr == e.primaries && tr == e.trc && mx == e.space,
              "the ProRes frame header must have color_primaries " + std::to_string(e.primaries) + ", transfer_characteristic "
                  + std::to_string(e.trc) + " and matrix_coefficients " + std::to_string(e.space));
    }
    if (e.primaries >= 0) check(s.primaries == e.primaries, std::string("color_primaries must be ") + av_color_primaries_name((AVColorPrimaries)e.primaries));
    if (e.trc >= 0) check(s.trc == e.trc, std::string("color_transfer must be ") + av_color_transfer_name((AVColorTransferCharacteristic)e.trc));
    if (!e.center.empty()) {
        bool levels = s.center.size() == e.center.size();
        for (size_t i = 0; levels && i < e.center.size(); ++i) levels = std::abs(s.center[i] - e.center[i]) <= e.tolerance;
        std::string what = "the decoded black and white levels must be";
        for (int c : e.center) what += " " + std::to_string(c);
        check(levels, what);
    }
    std::printf(failures ? "  FAIL (color)\n" : "  PASS (color)\n");
    return failures;
}

// Issue #135: FFmpeg's dnxhd encoder writes the DNxHR HQX and 444 profiles in 10 bit (yuv422p10, and
// yuv444p10 or gbrp10), so their option labels and hints must not say 12 bit.
int testDNxHDProfileHints()
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    int failures = 0;
    auto it = ctx.params.find("DNxHDCodecProfile");
    if (it == ctx.params.end()) {
        std::printf("  FAIL: no param DNxHDCodecProfile\n");
        ++failures;
    } else {
        int found = 0;
        for (const auto& prop : it->second.props->props) {
            if (prop.first != kOfxParamPropChoiceOption && prop.first != kOfxParamPropChoiceLabelOption && prop.first != kOfxParamPropHint) continue;
            for (const std::string& v : prop.second.s) {
                if (v.find("DNxHR 4:4:4") != std::string::npos || v.find("DNxHR High Quality (1") != std::string::npos) {
                    std::printf("  %s: %s\n", prop.first.c_str(), v.c_str());
                    ++found;
                }
                if (v.find("12 bit") != std::string::npos || v.find("12-bit") != std::string::npos) {
                    std::printf("  FAIL: %s says 12 bit: %s\n", prop.first.c_str(), v.c_str());
                    ++failures;
                }
            }
        }
        if (found < 2) { std::printf("  FAIL: the hints of DNxHR 444 and HQX were not found\n"); ++failures; }
    }
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf(failures ? "  FAIL (DNxHR hints)\n" : "  PASS (DNxHR hints)\n");
    return failures;
}

// Issue #136: libx265 with Alpha unchecked must never get a pixel format with alpha (yuva420p, which an 8-bit-only
// libx265 offers, fails with "Loaded libx265 does not support alpha layer encoding"). If the encoder has no format
// with the requested chroma and bit depth (e.g. MacPorts' x265 on arm64 is 8-bit only), the closest one is the
// format with the requested chroma (or more) and the highest bit depth.
// With Alpha checked, the encode must succeed: if libx265 lists yuva420p but can not open it (a libx265 without
// alpha support), alpha is dropped, with a warning in the persistent message.
int testWriteFFmpegX265(const std::string& outFile, int W, int H, int prefCoding, int prefDepthIndex, bool alpha = false)
{
    static const int depths[] = { 8, 10, 12, 16 };
    static const int log2ChromaW[] = { 1, 1, 0 }; // YUV420, YUV422, YUV444
    static const int log2ChromaH[] = { 1, 0, 0 };
    const AVCodec* enc = avcodec_find_encoder_by_name("libx265");
    if (!enc) { std::printf("  SKIP: no libx265 encoder\n"); return 0; }
    // the expected bit depth: the highest one up to the requested one, among the Y'CbCr formats without alpha
    // that have at least the requested chroma resolution
    // (avcodec_get_supported_config() is in libavcodec 61.13 and later, AVCodec::pix_fmts is deprecated since then)
    const AVPixelFormat* pixFmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const void* configs = nullptr;
    int nConfigs = 0;
    if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &nConfigs) >= 0 && nConfigs > 0) {
        pixFmts = static_cast<const AVPixelFormat*>(configs); // terminated by AV_PIX_FMT_NONE
    }
#else
    pixFmts = enc->pix_fmts;
#endif
    if (!pixFmts) { std::printf("  FAIL: no pixel formats for libx265\n"); return 1; }
    int expectedDepth = 0;
    bool listsAlpha = false; // libx265 lists a format with alpha and at least the requested chroma
    std::string formats;
    for (const AVPixelFormat* p = pixFmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixelFormat f = *p;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(f);
        formats += std::string(" ") + av_get_pix_fmt_name(f);
        if (d && (d->flags & AV_PIX_FMT_FLAG_ALPHA) && d->log2_chroma_w <= log2ChromaW[prefCoding] && d->log2_chroma_h <= log2ChromaH[prefCoding]) listsAlpha = true;
        if (!d || (d->flags & (AV_PIX_FMT_FLAG_ALPHA | AV_PIX_FMT_FLAG_RGB)) || d->nb_components < 3) continue;
        if (d->log2_chroma_w <= log2ChromaW[prefCoding] && d->log2_chroma_h <= log2ChromaH[prefCoding]) {
            expectedDepth = std::max(expectedDepth, std::min((int)d->comp[0].depth, depths[prefDepthIndex]));
        }
    }
    std::printf("  libx265 pixel formats:%s; expected bit depth %d\n", formats.c_str(), expectedDepth);

    WriterParams wp;
    wp.strings = { { "codecShortName", "libx265" } };
    wp.ints = { { "prefPixelCoding", prefCoding }, { "prefBitDepth", prefDepthIndex }, { "enableAlpha", alpha ? 1 : 0 } };
    gMessages.clear();
    if (testWriteFFmpeg(outFile, W, H, 3, wp)) {
        std::printf("  messages:\n%s", gMessages.c_str());
        return 1;
    }
    StreamInfo s;
    if (probeStream(outFile, &s)) return 1;
    int failures = 0;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(av_get_pix_fmt(s.pixFmt.c_str()));
    if (!d) { std::printf("  FAIL: unknown pix_fmt %s\n", s.pixFmt.c_str()); return 1; }
    if (alpha) {
        // the file was written; if alpha was dropped although libx265 lists a format with alpha, a warning says so
        const bool warned = gMessages.find("can not write alpha") != std::string::npos;
        std::printf("  alpha %s, warning: %s\n", (d->flags & AV_PIX_FMT_FLAG_ALPHA) ? "written" : "dropped", warned ? "yes" : "no");
        if (!(d->flags & AV_PIX_FMT_FLAG_ALPHA) && listsAlpha && !warned) { std::printf("  FAIL: alpha was dropped without a warning:\n%s", gMessages.c_str()); ++failures; }
        if ((d->flags & AV_PIX_FMT_FLAG_ALPHA) && warned) { std::printf("  FAIL: alpha was written, but a warning says that it was dropped\n"); ++failures; }
        std::printf(failures ? "  FAIL (libx265 with alpha)\n" : "  PASS (libx265 with alpha)\n");
        return failures;
    }
    if (d->flags & AV_PIX_FMT_FLAG_ALPHA) { std::printf("  FAIL: pix_fmt %s has alpha, Alpha is unchecked\n", s.pixFmt.c_str()); ++failures; }
    if (d->log2_chroma_w > log2ChromaW[prefCoding] || d->log2_chroma_h > log2ChromaH[prefCoding]) { std::printf("  FAIL: pix_fmt %s has less chroma than requested\n", s.pixFmt.c_str()); ++failures; }
    if ((int)d->comp[0].depth != expectedDepth) { std::printf("  FAIL: pix_fmt %s is not %d-bit\n", s.pixFmt.c_str(), expectedDepth); ++failures; }
    std::printf(failures ? "  FAIL (libx265 pixel format)\n" : "  PASS (libx265 pixel format)\n");
    return failures;
}

} // namespace

// A Fixed output format whose PAR is 0 (the range of the PAR param starts at 0): the PAR is invalid, and is
// replaced by 1. Before, the RoD had a width of 0 (the format was multiplied by the PAR), and beginSequenceRender
// divided by the PAR (0/0 = NaN, converted to int).
int testWriterFormatPar0()
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.WriteFFmpeg");
    if (!p) { std::printf("  FAIL: WriteFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    if (!ctx.params.count("formatType") || !ctx.params.count(kNatronParamFormatSize) || !ctx.params.count(kNatronParamFormatPar)) {
        std::printf("  FAIL: no formatType, %s or %s param\n", kNatronParamFormatSize, kNatronParamFormatPar);
        return 1;
    }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, "/tmp/ofx_render_test_par0.mov");
    setOverrideInt(&ctx, "formatType", 2); // Fixed
    if (ctx.params.count("clipToRoD")) setOverrideInt(&ctx, "clipToRoD", 0);
    ctx.params[kNatronParamFormatSize].hasOv = true;
    ctx.params[kNatronParamFormatSize].ov.i = { 64, 48 };
    ctx.params[kNatronParamFormatPar].hasOv = true;
    ctx.params[kNatronParamFormatPar].ov.d = { 0. };
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, 128, 128, 3, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
    PropSet a; OfxPropertySetHandle ap = PH(&a);
    pSetDouble(ap, kOfxPropTime, 0, 1.);
    double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    PropSet o;
    OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), PH(&o));
    double rod[4] = { -1., -1., -1., -1. };
    pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, rod);
    std::printf("  status=%d RoD=%g,%g,%g,%g\n", st, rod[0], rod[1], rod[2], rod[3]);
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (!ok(st) || rod[0] != 0. || rod[1] != 0. || rod[2] != 64. || rod[3] != 48.) {
        std::printf("  FAIL: expected the RoD 0,0,64,48 (the PAR of 0 is replaced by 1)\n");
        return 1;
    }
    std::printf("  PASS\n");
    return 0;
}

// #137: the file param at a frame may name the open file through another path (a symbolic link, as /tmp
// and /private/tmp on macOS, "." components, a "file:" url, or another case on Windows): it is encoded.
// Another file is still an error.
int testWriteFFmpegSameFile(const std::string& outFile, int W, int H, int nFrames)
{
    namespace fs = std::filesystem;
    const fs::path out = fs::absolute(fs::path(outFile));
    const std::string dir = out.parent_path().string();
    const std::string base = out.filename().string();
    int failures = 0;
    std::vector<std::pair<std::string, std::string>> aliases; // (description, path of outFile)
    aliases.push_back({ "\".\" component", dir + "/./" + base });
    aliases.push_back({ "\"..\" component", dir + "/../" + out.parent_path().filename().string() + "/" + base });
    aliases.push_back({ "\"file:\" url", "file:" + out.string() });
#ifdef _WIN32
    {
        std::string upper = out.string();
        for (char& c : upper) c = (char)std::toupper((unsigned char)c);
        aliases.push_back({ "other case", upper });
    }
#else
    // a symbolic link to the directory, as /tmp -> /private/tmp on macOS
    const std::string link = out.string() + ".lnk";
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_directory_symlink(".", link, ec);
    if (ec) { std::printf("  FAIL: symlink %s: %s\n", link.c_str(), ec.message().c_str()); return 1; }
    aliases.push_back({ "symbolic link to the directory", link + "/" + base });
    if (fs::is_symlink("/tmp", ec) && (fs::canonical("/tmp", ec) == fs::path("/private/tmp"))) {
        // macOS: the file is written through /tmp, and rendered through /private/tmp, and vice versa.
        // A unique name, since several test runs may share the machine.
        char tmpTemplate[] = "/tmp/ofx_render_test_i137_XXXXXX.mov";
        const int tmpFd = mkstemps(tmpTemplate, 4);
        if (tmpFd < 0) {
            std::printf("  FAIL: mkstemps %s: %s\n", tmpTemplate, std::strerror(errno));
            fs::remove(link, ec);
            return 1;
        }
        close(tmpFd);
        const std::string tmp = tmpTemplate;
        WriterParams wp;
        wp.renderFile = "/private" + tmp;
        std::printf("  -- %s, then %s\n", tmp.c_str(), wp.renderFile.c_str());
        failures += testWriteFFmpeg(tmp, W, H, nFrames, wp) || testReadFFmpeg(tmp, W, H, nFrames);
        wp.renderFile = tmp;
        std::printf("  -- %s, then %s\n", ("/private" + tmp).c_str(), tmp.c_str());
        failures += testWriteFFmpeg("/private" + tmp, W, H, nFrames, wp) || testReadFFmpeg(tmp, W, H, nFrames);
        std::remove(tmp.c_str());
    }
#endif
    for (const auto& a : aliases) {
        WriterParams wp;
        wp.renderFile = a.second;
        std::printf("  -- %s: %s\n", a.first.c_str(), a.second.c_str());
        failures += testWriteFFmpeg(outFile, W, H, nFrames, wp) || testReadFFmpeg(outFile, W, H, nFrames);
    }
    {
        WriterParams wp;
        wp.renderFile = out.string() + ".other.mov";
        wp.expectRenderFailure = true;
        std::printf("  -- another file: %s\n", wp.renderFile.c_str());
        failures += testWriteFFmpeg(outFile, W, H, nFrames, wp);
        std::remove(wp.renderFile.c_str());
    }
#ifndef _WIN32
    fs::remove(link, ec);
#endif
    std::remove(outFile.c_str());
    return failures ? 1 : 0;
}

int
main(int argc, char** argv)
{
    initHost();
    const std::string outFile = (argc > 1) ? argv[1] : "/tmp/iotest/ofx_render_out.mov";
    const int W = 128, H = 128, N = 10;

    int failures = 0;
    std::printf("==== WriteFFmpeg render integration ====\n");
    failures += testWriteFFmpeg(outFile, W, H, N);
    std::printf("==== ReadFFmpeg render integration ====\n");
    failures += testReadFFmpeg(outFile, W, H, N);
    std::printf("==== ReadFFmpeg, frames outside of the movie, with each onMissingFrame option (issue #134) ====\n");
    failures += testReadFFmpegMissingFrame(outFile, W, H, N, false);
    std::printf("==== same, RGBA output: Black is transparent black (issue #134) ====\n");
    failures += testReadFFmpegMissingFrame(outFile, W, H, N, true);
    std::printf("==== same, RGBA output at render scale 0.5 (decoded into a temporary buffer): Black is transparent black (issue #147) ====\n");
    failures += testReadFFmpegMissingFrame(outFile, W, H, N, true, false, true);
    {
        // a frame inside of the movie that can not be decoded is an error, whatever onMissingFrame
        WriterParams wp;
        wp.ints = { { "fastStart", 1 } };
        std::printf("==== ReadFFmpeg, truncated movie: a frame that fails to decode is an error with each onMissingFrame option (issue #134) ====\n");
        failures += testWriteFFmpeg(outFile + ".i134.mov", W, H, N, wp) || truncateMovie(outFile + ".i134.mov", outFile + ".i134trunc.mov") || testReadFFmpegMissingFrame(outFile + ".i134trunc.mov", W, H, N, true, true);
    }
    {
        // Matroska stores timestamps in milliseconds: at 24 fps, every frame of the movie must be decoded
        WriterParams wp;
        wp.fps = 24.;
        std::printf("==== WriteFFmpeg + ReadFFmpeg, Matroska at 24 fps: every frame is decoded ====\n");
        failures += testWriteFFmpeg(outFile + ".24fps.mkv", W, H, N, wp) || testReadFFmpeg(outFile + ".24fps.mkv", W, H, N);
    }
    {
        // #116: a movie has a single view, view 0, whatever the order in which the host renders the views
        WriterParams wp;
        wp.views = { 1, 0 };
        std::printf("==== WriteFFmpeg, views rendered in reverse order: view 0 is encoded (issue #116) ====\n");
        failures += testWriteFFmpeg(outFile + ".i116.mov", W, H, N, wp) || testReadFFmpeg(outFile + ".i116.mov", W, H, N);
        wp.views = { 0, 1 };
        std::printf("==== WriteFFmpeg, views rendered in order: view 0 is encoded (issue #116) ====\n");
        failures += testWriteFFmpeg(outFile + ".i116.mov", W, H, N, wp) || testReadFFmpeg(outFile + ".i116.mov", W, H, N);
    }
    std::printf("==== WriteFFmpeg mov, start timecode (issue #78) ====\n");
    failures += testStartTimecode(outFile + ".i78.mov", "1:02:03:04", 0., "01:02:03:04");
    std::printf("==== WriteFFmpeg mp4, start timecode (issue #78) ====\n");
    failures += testStartTimecode(outFile + ".i78.mp4", " 10:00:00:00 ", 0., "10:00:00:00");
    std::printf("==== WriteFFmpeg, invalid start timecode: error, nothing written (issue #78) ====\n");
    failures += testStartTimecode(outFile + ".i78.mov", "01:02:03:99", 0., nullptr);
    failures += testStartTimecode(outFile + ".i78.mov", "01:02:03", 0., nullptr);
    std::printf("==== WriteFFmpeg mov, drop-frame start timecode at 29.97 fps (issue #78) ====\n");
    failures += testStartTimecode(outFile + ".i78.mov", "01:00:00;02", 29.97, "01:00:00;02");
    std::printf("==== WriteFFmpeg, invalid drop-frame start timecodes: error, nothing written (issue #78) ====\n");
    failures += testStartTimecode(outFile + ".i78.mov", "01:01:00;00", 29.97, nullptr); // dropped frame number
    failures += testStartTimecode(outFile + ".i78.mov", "01:00:00;02", 25., nullptr); // not at 29.97 fps
    {
        // start timecode validation at various frame rates: valid ones are written and read back
        const struct { const char* tc; double fps; const char* expected; } cases[] = {
            { "01:00:00;00", 30., nullptr },         // drop-frame needs 30000/1001, not 30/1
            { "01:00:00;04", 60., nullptr },         // nor 60/1
            { "01:01:00;01", 29.97, nullptr },       // frames 0-1 are skipped at minute 1
            { "01:10:00;00", 29.97, "01:10:00;00" }, // but not at minute 10
            { "01:01:00;03", 59.94, nullptr },       // frames 0-3 are skipped at 59.94 fps
            { "01:01:00;04", 59.94, "01:01:00;04" },
            { "01:00:00:24", 23.976, nullptr },      // frames 0-23 at 23.976 fps
            { "01:00:00:23", 23.976, "01:00:00:23" },
            { "01:00:00:00", 300., nullptr },        // mov can not write a timecode above 255 fps
            { "+1:02:03:04", 24., nullptr },         // digits only
            { "1: 2: 3: 4", 24., nullptr },
            { "01:02:03:04x", 24., nullptr },
            { "99999999999:00:00:00", 24., nullptr },
            { "100:00:00:00", 24., nullptr },
            { "24:00:00:00", 24., nullptr },
        };
        for (const auto& c : cases) {
            std::printf("==== WriteFFmpeg mov, start timecode \"%s\" at %g fps: %s (issue #78) ====\n", c.tc, c.fps, c.expected ? "valid" : "invalid");
            failures += testStartTimecode(outFile + ".i78.mov", c.tc, c.fps, c.expected);
        }
    }
    {
        // issue #110: the 255 fps limit only applies when a tmcd track is written (mov and mp4 by
        // default, or write_tmcd=1 in the Format Options)
        // (onOutputFileChanged() selects the mp4 format for a .3gp file: select the 3gp format)
        WriterParams mpeg4, mp4, noTmcd, gp3, gp3Tmcd;
        mpeg4.strings = { { "codecShortName", "mpeg4" } };
        mp4 = mpeg4;
        mp4.choicesAfterCreate = { { "format", "[mp4]" } };
        noTmcd = mpeg4;
        noTmcd.strings.push_back({ "formatOptions", "write_tmcd=0" });
        gp3 = mpeg4;
        gp3.choicesAfterCreate = { { "format", "[3gp]" } };
        gp3Tmcd = gp3;
        gp3Tmcd.strings.push_back({ "formatOptions", "write_tmcd=1" });
        std::printf("==== WriteFFmpeg mov, start timecode at 300 fps with write_tmcd=0: no tmcd track, valid (issue #110) ====\n");
        failures += testStartTimecode(outFile + ".i78.mov", "01:00:00:00", 300., "(none)", noTmcd);
        std::printf("==== WriteFFmpeg 3gp, start timecode at 300 fps: no tmcd track, valid (issue #110) ====\n");
        failures += testStartTimecode(outFile + ".i110.3gp", "01:00:00:00", 300., "(none)", gp3);
        std::printf("==== WriteFFmpeg 3gp, start timecode at 300 fps with write_tmcd=1: invalid (issue #110) ====\n");
        failures += testStartTimecode(outFile + ".i110.3gp", "01:00:00:00", 300., nullptr, gp3Tmcd);
        std::printf("==== WriteFFmpeg mp4, start timecode at 300 fps: invalid (issue #110) ====\n");
        failures += testStartTimecode(outFile + ".i78.mp4", "01:00:00:00", 300., nullptr, mp4);
        std::printf("==== WriteFFmpeg 3gp, start timecode at 24 fps with write_tmcd=1: tmcd track (issue #110) ====\n");
        failures += testStartTimecode(outFile + ".i110.3gp", "01:02:03:04", 24., "01:02:03:04", gp3Tmcd);
        std::remove((outFile + ".i110.3gp").c_str());
    }
    {
        // issue #110: drop-frame at 119.88 fps (SMPTE ST 12-3), 47.952 and 95.904 fps are written at x/1001 fps
        // (libavutil writes the frame number with 3 digits above 100 fps)
        const struct { const char* tc; double fps; const char* expected; int num; } cases[] = {
            { "01:00:00:00", 47.952, "01:00:00:00", 48000 },
            { "01:00:00:00", 95.904, "01:00:00:00", 96000 },
            { "01:01:00;08", 119.88011988011988, "01:01:00;008", 120000 }, // frames 0-7 are skipped at 119.88 fps
            { "01:00:00;00", 119.88, "01:00:00;000", 120000 }, // the last one: the file is kept, e.g. for ffprobe
        };
        for (const auto& c : cases) {
            std::printf("==== WriteFFmpeg mov, start timecode \"%s\" at %.17g fps: %d/1001 fps (issue #110) ====\n", c.tc, c.fps, c.num);
            // the default codec (ProRes): MPEG-4 part 2 can not have a time base denominator above 65535
            const int f = testStartTimecode(outFile + ".i110.mov", c.tc, c.fps, c.expected);
            failures += f;
            if (!f) failures += checkFrameRate(outFile + ".i110.mov", c.num, 1001);
        }
        // MPEG-4 part 2 can not have a time base denominator above 65535: the frame rate is reduced, as
        // the ffmpeg CLI does (40999/342 for 120000/1001), and drop-frame is still valid
        std::printf("==== WriteFFmpeg mov, MPEG-4 part 2 at 119.88 fps, drop-frame: close to 120000/1001 fps (issue #110) ====\n");
        {
            WriterParams wp;
            wp.strings = { { "codecShortName", "mpeg4" } };
            const int f = testStartTimecode(outFile + ".i110.mp4v.mov", "01:00:00;00", 119.88, "01:00:00;000", wp);
            failures += f;
            if (!f) failures += checkFrameRate(outFile + ".i110.mp4v.mov", 120000, 1001, 1e-6);
            int n = 0, nI = 0;
            if (!f) {
                failures += probeVideoFrames(outFile + ".i110.mp4v.mov", &n, &nI);
                std::printf("  decoded %d frames\n", n);
                if (n != 3) { std::printf("  FAIL: expected 3 frames\n"); ++failures; }
            }
        }
        std::printf("==== WriteFFmpeg mov, \"01:01:00;07\" at 119.88 fps: dropped frame number, invalid (issue #110) ====\n");
        failures += testStartTimecode(outFile + ".i78.mov", "01:01:00;07", 119.88, nullptr);
    }
    std::printf("==== WriteFFmpeg, invalid start timecode reported when edited (issue #78) ====\n");
    failures += testStartTimecodeChangedParam(outFile + ".i78.mov");
    std::printf("==== WriteFFmpeg, Format \"Default\" guessed from the filename param before rendering (issue #110) ====\n");
    failures += testStartTimecodeDefaultFormat(outFile + ".i110.m4v", /*expectError=*/false);
    failures += testStartTimecodeDefaultFormat(outFile + ".i78.mov", /*expectError=*/true);
    std::printf("==== WriteFFmpeg, the persistent message shows all the errors in the settings (issue #110) ====\n");
    failures += testSettingsErrorsChangedParam(outFile + ".i78.mov");
    std::printf("==== WriteFFmpeg, empty start timecode: no timecode track (issue #78) ====\n");
    failures += testStartTimecode(outFile + ".i78.mov", "", 0., "(none)");
    std::printf("==== WriteOIIO JPEG, input image outside of the render window (issue #79) ====\n");
    failures += testWriteOIIONoInputImage(outFile + ".i79.jpg", 72, 128, /*channelCheckboxes=*/true);
    std::printf("==== same, with hidden R/G/B/A checkboxes ====\n");
    failures += testWriteOIIONoInputImage(outFile + ".i79.jpg", 72, 128, /*channelCheckboxes=*/false);
    std::printf("==== WriteOIIO PNG, input image bounds outside of the render window (issue #79) ====\n");
    failures += testWriteOIIODisjointInputImage(outFile + ".i79.png", 72, 128);
    std::printf("==== same, render aborted (no output image): nothing written ====\n");
    failures += testWriteOIIODisjointInputImage(outFile + ".i79.png", 72, 128, /*aborted=*/true);
    std::printf("==== same, no output image: the render fails, nothing written ====\n");
    failures += testWriteOIIODisjointInputImage(outFile + ".i79.png", 72, 128, /*aborted=*/false, /*outputMissing=*/true);
    std::printf("==== WriteOIIO, no input image, output clip connected (#79) ====\n");
    failures += testWriteOIIONoInputImage(outFile + ".i79.jpg", 72, 128, /*channelCheckboxes=*/true, /*overlappingRoD=*/false, /*outputConnected=*/true);
    std::printf("==== WriteOIIO, no input image although its RoD overlaps: black, per the spec ====\n");
    failures += testWriteOIIONoInputImage(outFile + ".i79.jpg", 72, 128, /*channelCheckboxes=*/true, /*overlappingRoD=*/true);
    std::printf("==== WriteOIIO, no input image, render aborted: nothing written ====\n");
    failures += testWriteOIIONoInputImage(outFile + ".i79.jpg", 72, 128, /*channelCheckboxes=*/true, /*overlappingRoD=*/false, /*outputConnected=*/false, /*aborted=*/true);

    {
        // #90: G,B,A of an RGB source: the output is G, B, and the missing alpha, which is opaque
        const float v[4] = { 0.2f, 0.5f, 0.8f, 0.f };
        const bool chk[4] = { false, true, true, true };
        const int exp[4] = { 128, 204, 255, 0 };
        std::printf("==== WriteOIIO PNG, RGB source, G/B/A checked (issue #90) ====\n");
        failures += testWriteOIIOChannelSelection(outFile + ".i90a.png", 32, 16, kOfxImageComponentRGB, v, chk, 3, exp);
    }
    {
        // #90: an Alpha-only source with A checked: a 1-channel image of the alpha
        const float v[4] = { 0.7f, 0.f, 0.f, 0.f };
        const bool chk[4] = { false, false, false, true };
        const int exp[4] = { 179, 0, 0, 0 };
        std::printf("==== WriteOIIO PNG, Alpha source, A checked (issue #90) ====\n");
        failures += testWriteOIIOChannelSelection(outFile + ".i90b.png", 32, 16, kOfxImageComponentAlpha, v, chk, 1, exp);
    }
    std::printf("==== WriteFFmpeg Codec Options and Format Options (issue #71) ====\n");
    failures += testWriteFFmpegOptions(outFile + ".i71.mp4", 64, 48, N);
    std::printf("==== WriteFFmpeg, Alpha source, A checked (issue #90) ====\n");
    failures += testWriteFFmpegAlphaSource(outFile + ".i90c.mov", 64, 48, 3);
    {
        // #135: ProRes 4444 and 4444 XQ are Y'CbCr 4:4:4 (SMPTE RDD 36), like the 422 profiles: they must be
        // tagged with the Y'CbCr matrix they were converted with (SD: SMPTE 170M), and are in limited range.
        // FFmpeg's prores decoder decodes the 4444 profiles to 12 bit, and the 422 profiles to 10 bit.
        const struct { const char* fourcc; const char* pixFmt; int bits; } cases[] = {
            { "ap4h", "yuv444p12le", 12 }, { "ap4x", "yuv444p12le", 12 }, { "apch", "yuv422p10le", 10 },
        };
        for (const auto& c : cases) {
            std::printf("==== WriteFFmpeg ProRes %s: Y'CbCr matrix and color tags (issue #135) ====\n", c.fourcc);
            WriterParams wp;
            wp.strings = { { "codecShortName", std::string("prores_ks") + c.fourcc } };
            ExpectedStream e;
            e.tag = c.fourcc;
            e.pixFmt = c.pixFmt;
            e.bitsPerRawSample = c.bits;
            e.space = AVCOL_SPC_SMPTE170M;
            e.primaries = AVCOL_PRI_SMPTE170M;
            e.trc = AVCOL_TRC_BT709;
            e.rangeNotFull = true;
            e.proResHeader = true;
            e.center = { 16 << (c.bits - 8), 235 << (c.bits - 8) };
            e.tolerance = 1 << (c.bits - 8);
            failures += testWriteFFmpegColor(outFile + ".i135.prores.mov", W, H, wp, e);
        }
    }
    {
        // #135: DNxHD/DNxHR Output Range: the levels written must be the ones the stream is tagged with
        // (full: 0-255 or 0-1023, video: 16-235 or 64-940), and HQX/444 are 10-bit. DNxHR 444 is written as
        // RGB (gbrp10), which is always full range.
        const struct { const char* label; int profile; bool full; int w, h; const char* pixFmt; int bits; int range; int black, white; } cases[] = {
            { "DNxHR HQ, full range", 2, true, 256, 144, "yuv422p", 8, AVCOL_RANGE_JPEG, 0, 255 },
            { "DNxHR HQ, video range", 2, false, 256, 144, "yuv422p", 8, AVCOL_RANGE_MPEG, 16, 235 },
            { "DNxHR HQX, full range", 1, true, 256, 144, "yuv422p10le", 10, AVCOL_RANGE_JPEG, 0, 1023 },
            { "DNxHR HQX, video range", 1, false, 256, 144, "yuv422p10le", 10, AVCOL_RANGE_MPEG, 64, 940 },
            { "DNxHR 444, full range", 0, true, 256, 144, "gbrp10le", 10, AVCOL_RANGE_JPEG, 0, 1023 },
            { "DNxHR 444, video range (RGB is full range)", 0, false, 256, 144, "gbrp10le", 10, AVCOL_RANGE_JPEG, 0, 1023 },
            { "DNxHD 220x 1080p, full range", 6, true, 1920, 1080, "yuv422p10le", 10, AVCOL_RANGE_JPEG, 0, 1023 },
        };
        for (const auto& c : cases) {
            std::printf("==== WriteFFmpeg %s: levels, range and bit depth (issue #135) ====\n", c.label);
            WriterParams wp;
            wp.strings = { { "codecShortName", "dnxhd" } };
            wp.ints = { { "DNxHDCodecProfile", c.profile }, { "DNxHDEncodeVideoRange", c.full ? 0 : 1 } };
            ExpectedStream e;
            e.pixFmt = c.pixFmt;
            e.bitsPerRawSample = c.bits;
            e.range = c.range;
            e.center = { c.black, c.white };
            e.tolerance = c.bits > 8 ? 4 : 1;
            failures += testWriteFFmpegColor(outFile + ".i135.dnxhd.mov", c.w, c.h, wp, e);
        }
        std::printf("==== WriteFFmpeg DNxHR HQX and 444 hints: 10 bit (issue #135) ====\n");
        failures += testDNxHDProfileHints();
    }
    {
        // #136: libx265, Alpha unchecked, Bit Depth 10 or 12: never a format with alpha, even with an 8-bit-only libx265
        const struct { const char* label; int coding; int depth; } cases[] = {
            { "YUV422, 10 bit", 1, 1 }, { "YUV422, 12 bit", 1, 2 }, { "YUV420, 10 bit", 0, 1 }, { "YUV444, 10 bit", 2, 1 },
        };
        for (const auto& c : cases) {
            std::printf("==== WriteFFmpeg libx265 %s, no alpha: closest pixel format without alpha (issue #136) ====\n", c.label);
            failures += testWriteFFmpegX265(outFile + ".i136.mp4", W, H, c.coding, c.depth);
        }
        // #136: libx265, Alpha checked, YUV420: the encode must succeed, even if libx265 lists yuva420p but was
        // built without alpha support (then alpha is dropped, with a warning)
        for (int depth = 0; depth < 3; ++depth) {
            std::printf("==== WriteFFmpeg libx265 YUV420, %d bit, alpha: encodes (issue #136) ====\n", depth == 0 ? 8 : (depth == 1 ? 10 : 12));
            failures += testWriteFFmpegX265(outFile + ".i136.mp4", W, H, 0, depth, /*alpha=*/true);
        }
    }

    std::printf("==== WriteFFmpeg, frame rate of 0: the minimum bitrate tolerance is bitrate / 4 ====\n");
    failures += testBitrateToleranceFps0();

    std::printf("==== WriteFFmpeg, the file param names the open file through another path (issue #137) ====\n");
    failures += testWriteFFmpegSameFile(outFile + ".i137.mov", W, H, N);

    std::printf("==== WriteFFmpeg, the image gets smaller than the movie during the render: the rest of the frame is black ====\n");
    failures += testWriteFFmpegRoDChanged(outFile + ".rod.mov", W, H, N, 64, 32);
    {
        // 8-bit RGB intermediate buffer (the one above is 16-bit)
        WriterParams wp;
        wp.strings = { { "codecShortName", "mpeg4" } };
        std::printf("==== same, MPEG-4 (8 bit) ====\n");
        failures += testWriteFFmpegRoDChanged(outFile + ".rod.mov", W, H, N, 64, 32, wp);
    }
    std::printf("==== WriteFFmpeg, the image gets larger than the movie during the render: it is cropped ====\n");
    failures += testWriteFFmpegRoDChanged(outFile + ".rod.mov", W, H, N, 192, 160);

    std::printf("==== WriteFFmpeg, Fixed output format with a PAR of 0: the PAR is 1 ====\n");
    failures += testWriterFormatPar0();

    {
        // a bitrate above 2147 Mbit/s (its conversion to int overflowed) and a PAR out of range from the host (its
        // conversion to int overflowed): found by -fsanitize=float-cast-overflow; the movie must be written
        WriterParams wp;
        // libx264 with CRF None uses the Bitrate param (FFmpeg's own encoders here use qscale instead); skipped without
        // libx264
        std::printf("==== WriteFFmpeg libx264, bitrate 3000 Mbit/s, source PAR 1e300: no out-of-range conversion ====\n");
        if (!avcodec_find_encoder_by_name("libx264")) {
            std::printf("  SKIP: no libx264 encoder\n");
        } else {
            wp.strings = { { "codecShortName", "libx264" } };
            wp.ints = { { "crf", 0 } }; // CRF None: the bitrate is used
            wp.doubles = { { "bitrateMbps", 3000. } };
            wp.inputPar = 1e300;
            failures += testWriteFFmpeg(outFile + ".bitrate.mp4", W, H, 3, wp);
            std::remove((outFile + ".bitrate.mp4").c_str());
        }
    }

    std::printf("Result: failures=%d\n", failures);
    std::printf(failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? 0 : 1;
}
