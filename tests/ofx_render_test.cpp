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
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Extra param values for a WriteFFmpeg test.
struct WriterParams {
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::pair<std::string, int>> ints;
    bool expectBeginFailure = false; // beginSequenceRender must fail (and nothing is rendered)
    bool constantImage = false;      // render the same mid-gray image for every frame instead of a ramp
};

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

    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, nFrames, kOfxImageEffectContextWriter);
    if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }

    Clip* src = ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
    if (!src) { std::printf("  FAIL: no Source clip\n"); return 1; }
    src->connected = true;
    setupClipImage(src, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
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
            std::printf("  PASS (beginSequenceRender failed as expected)\n");
            return 0;
        }
        if (!began) { std::printf("  FAIL: beginSequenceRender\n"); return 1; }
    }

    float* fp = reinterpret_cast<float*>(src->buffer.data());
    const size_t npix = (size_t)W * H;
    int rfail = 0;
    for (int t = 1; t <= nFrames; ++t) {
        const float v = wp.constantImage ? 0.5f : (float)t / (float)(nFrames + 1); // gray ramp, monotonic in t
        for (size_t i = 0; i < npix; ++i) { fp[i * 4 + 0] = v; fp[i * 4 + 1] = v; fp[i * 4 + 2] = v; fp[i * 4 + 3] = 1.f; }
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

// Offset of the first top-level QuickTime atom of the given type, or -1.
long findTopLevelAtom(const std::string& file, const char* type)
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
        if (!std::memcmp(h + 4, type, 4)) { found = pos; break; }
        if (size < 8) break;
        pos += (long)size;
    }
    std::fclose(f);
    return found;
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

    std::printf("  -- reference: mpeg4, Keyframe Interval = 5, no options\n");
    if (testWriteFFmpeg(outFile, W, H, nFrames, base) || probeVideoFrames(outFile, &n, &nI)) return 1;
    std::printf("  decoded %d frames, %d I frames\n", n, nI);
    if (n != nFrames || nI >= n) { std::printf("  FAIL: expected %d frames, including P frames\n", nFrames); ++failures; }
    if (findTopLevelAtom(outFile, "moov") < findTopLevelAtom(outFile, "mdat")) { std::printf("  FAIL: without faststart, mdat must precede moov\n"); ++failures; }

    std::printf("  -- Codec Options \"g=1\" (overrides Keyframe Interval), Format Options \"movflags=+faststart\"\n");
    {
        WriterParams wp = base;
        wp.strings.push_back({ "codecOptions", "g=1" });
        wp.strings.push_back({ "formatOptions", "movflags=+faststart" });
        gMessages.clear();
        if (testWriteFFmpeg(outFile, W, H, nFrames, wp) || probeVideoFrames(outFile, &n, &nI)) return 1;
        std::printf("  decoded %d frames, %d I frames\n", n, nI);
        if (n != nFrames || nI != n) { std::printf("  FAIL: expected %d frames, all I frames\n", nFrames); ++failures; }
        const long moov = findTopLevelAtom(outFile, "moov"), mdat = findTopLevelAtom(outFile, "mdat");
        std::printf("  moov at %ld, mdat at %ld\n", moov, mdat);
        if (moov < 0 || mdat < 0 || moov > mdat) { std::printf("  FAIL: movflags=+faststart: moov must precede mdat\n"); ++failures; }
        if (gMessages.find("not recognized") != std::string::npos) { std::printf("  FAIL: unexpected warning:\n%s", gMessages.c_str()); ++failures; }
    }

    std::printf("  -- several options, spaces as separators, and unknown keys\n");
    {
        WriterParams wp = base;
        wp.strings.push_back({ "codecOptions", " g = 1  bogus_codec_key=3 " });
        wp.strings.push_back({ "formatOptions", "bogus_format_key=4" });
        gMessages.clear();
        if (testWriteFFmpeg(outFile, W, H, nFrames, wp) || probeVideoFrames(outFile, &n, &nI)) return 1;
        std::printf("  decoded %d frames, %d I frames\n", n, nI);
        if (n != nFrames || nI != n) { std::printf("  FAIL: expected %d frames, all I frames\n", nFrames); ++failures; }
        if (gMessages.find(kOfxMessageWarning) == std::string::npos ||
            gMessages.find("bogus_codec_key=3") == std::string::npos ||
            gMessages.find("bogus_format_key=4") == std::string::npos) {
            std::printf("  FAIL: expected a warning about bogus_codec_key and bogus_format_key, got:\n%s", gMessages.c_str());
            ++failures;
        } else {
            std::printf("  PASS (unknown keys reported in a warning)\n");
        }
    }

    std::printf("  -- syntax error in Codec Options\n");
    {
        WriterParams wp = base;
        wp.strings.push_back({ "codecOptions", "g" });
        wp.expectBeginFailure = true;
        gMessages.clear();
        if (testWriteFFmpeg(outFile, W, H, nFrames, wp)) ++failures;
        if (gMessages.find(kOfxMessageError) == std::string::npos || gMessages.find("Codec Options") == std::string::npos) {
            std::printf("  FAIL: expected an error about Codec Options, got:\n%s", gMessages.c_str());
            ++failures;
        }
    }

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

} // namespace

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
    failures += testWriteFFmpegOptions(outFile + ".i71.mov", 64, 48, N);
    std::printf("==== WriteFFmpeg, Alpha source, A checked (issue #90) ====\n");
    failures += testWriteFFmpegAlphaSource(outFile + ".i90c.mov", 64, 48, 3);

    std::printf("Result: failures=%d\n", failures);
    std::printf(failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? 0 : 1;
}
