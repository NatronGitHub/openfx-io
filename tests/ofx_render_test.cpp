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
 *   WriteFFmpeg color tags and Y'CbCr matrix, and the ReadFFmpeg round trip:
 *     - encode a constant color with H.264 (libx264), ProRes 4444 and QuickTime Animation (qtrle, RGB), with the OCIO
 *       output colorspaces rec709 and Rec2020 of test.ocio: the stream keeps the primaries and transfer function of
 *       the OCIO colorspace (ProRes: only the transfer functions of SMPTE RDD 36), its Y'CbCr matrix is BT.2020 with the
 *       BT.2020 primaries, else BT.709 in HD and SMPTE 170M in SD, and RGB (gbr) for RGB codecs; the Y' that FFmpeg
 *       decodes is the one of that matrix; ReadFFmpeg (Auto matrix and range) decodes the color back
 *     - Matroska: the OCIO output colorspace is written in the uk.co.thefoundry.Colorspace metadata, and ReadFFmpeg
 *       uses it as its File Colorspace
 *     - DNxHR HQX, HQ and 444 with Rec2020, and DNxHR HQ in SD: the Y'CbCr matrix is always BT.709 (the one of the
 *       DNxHD frame header that FFmpeg's encoder writes, which FFmpeg's decoder uses), 444 is RGB (gbr)
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
 *   WriteFFmpeg libx265 MP4 with 1 or 2 frames (issue #217):
 *     - libx265 gives the first packets an uninitialized dts: each encode must have all its frames (or fail),
 *       and an error of the muxer in the encoder flush must fail endSequenceRender
 *
 *   WriteOIIO test (issue #79):
 *     - render a JPEG while the host has no source image for the render window
 *       (clipGetImage fails with kOfxStatFailed): a black image must be written
 *     - same, with a source image whose bounds do not intersect the render window:
 *       a black image must be written, and the output image must be black
 *
 *   WriteFFmpeg uncompressed codecs (issue #170):
 *     - write a color pattern (with an alpha ramp if the format has alpha) with v210 and with rawvideo in QuickTime,
 *       NUT, Matroska and AVI, check the codec, tag, pixel format and range that libavformat reads back, and that
 *       ReadFFmpeg decodes the pattern exactly (RGB formats) or within the Y'CbCr conversion tolerance
 *     - same, with values on no grid (float, 10-bit and 16-bit), and with an odd width (63x47): RGB in QuickTime is
 *       written as ARGB, and v210 fails with an error
 *     - a project saved with the v308 or v410 encoder, which FFmpeg 9 removed, writes rawvideo with the same tag
 *
 *   WriteFFmpeg DNxHR 444 on binary RGB noise (issue #267), with the option --dnxhr444-noise (only this test):
 *     - FFmpeg's dnxhd encoder must not abort (qscale above 255 in its 8-bit mb_qscale): the frames encode, or
 *       fail with "Cannot encode frame"
 *
 *   WriteFFmpeg, beginSequenceRender fails after a render (issue #289), with the option --begin-failure (only this test):
 *     - render a movie, then on the same instance with an interlaced input, DNxHD 36 in 720p (1080p only), or Codec
 *       Options that FFmpeg's encoder rejects: beginSequenceRender fails, endSequenceRender must not crash, and the
 *       next render must work
 *
 *   WriteFFmpeg DNxHD/DNxHR Codec Profiles (issue #305), with the option --dnxhd-profiles (only this test):
 *     - each DNxHD profile in 1920x1080 and 1280x720 at 23.976, 24, 25, 29.97, 30, 50, 59.94 and 60 fps (DNxHR profiles,
 *       which do not depend on the frame rate, at 23.976 fps only): the movie is encoded
 *       with the compression ID (CID) of the profile (read from the frame header), or beginSequenceRender fails with a
 *       clear message where the profile does not exist (DNxHD 440x and 36 in 720p)
 *     - each DNxHR profile below 256x120 (FFmpeg's encoder limit, issue #248): beginSequenceRender fails with a message
 *       that gives the limit and the image size; at 256x120 and 257x121, the movie is encoded with the CID of the profile
 *
 *   WriteFFmpeg, a frame is not encoded while other threads wait (issue #288), with the option --encode-failure (only
 *   this test):
 *     - frames 2..4 are rendered on other threads, which wait for frame 1; frame 1 is aborted, or rendered to another
 *       file: the waiting renders must fail within 60 s instead of waiting forever, and the next render must work
 *     - with --encode-failure-disk-full (not on MS Windows: exit status 77), frame 1 fails to be written (RLIMIT_FSIZE)
 *
 *   WriteOIIO, the disk is full (issues #292, #362), with the option --writeoiio-disk-full (only this test; not on MS
 *   Windows: exit status 77):
 *     - EXR (also a 2-view multi-part EXR), TIFF, DPX, PNG and JPEG files can not grow beyond 16 KB (RLIMIT_FSIZE):
 *       the render must fail with an error that names the file, the process must survive (OpenImageIO's JPEG writer
 *       can exit it), and the same instance must then write a file that OpenImageIO reads back, without the limit
 *
 *   WriteOIIO, the files that OpenImageIO writes by name (issue #362), with the option --writeoiio-files (only this
 *   test):
 *     - a 2-view EXR, with Parts "Split Views" and "Split Views,Layers" (multi-part: OpenImageIO's multi-part EXR
 *       writer ignores the oiio:ioproxy attribute) and "Single Part"; a JPEG 2000 file with the htj2k256 compression
 *       (OpenJPH, which writes the file by name; skipped without OpenJPH): each file must be read back, with its views
 *     - an EXR file with the zstd compression (issue #171): if the file read back is not zstd (OpenImageIO < 3.2.1, or
 *       its OpenEXR < 3.5, writes zip), the render must end with a persistent warning that names zstd; else no warning;
 *       a zip render of the same instance then ends without a warning
 *
 *   A writer of image files removes the partial file of a failed or aborted render (issue #251), with the option
 *   --writer-remove-partial-file (only this test):
 *     - a test writer (a non-ASCII file name) writes part of the file, then fails or the render is aborted: no file
 *       must be left, with no previous file and over a previous file
 *     - WritePNG (not on MS Windows, nor without libpng), the file can not grow beyond 16 KB (RLIMIT_FSIZE, as on a
 *       full disk), with no previous file and over a previous file: the render must fail and leave no file
 *     - a render that fails before the file is written (no R/G/B/A checkbox checked) leaves the previous file as it is
 *     - after each failure, a render with "Overwrite" unchecked must write the whole file
 *
 *   The conversions between Y'CbCr and RGB give exact values (issues #207, #220, #316), with the option --swscale-exact
 *   (only this test):
 *     - ReadFFmpeg, rawvideo and FFV1 written without swscale (4:2:0, 4:2:2, 4:4:4, 8, 10 and 12 bits, video and full range,
 *       alpha, odd widths): the exact R'G'B' of ITU-T H.273 §8.3 in every pixel, the last column included, rounded to
 *       the nearest at 8 bits, and white is 1 at more than 8 bits; an MPEG-4 movie at an odd width: its last column
 *     - WriteFFmpeg, decoded without swscale: 4:2:0 and 4:2:2 chroma is the average of the pixels (red and blue lines
 *       and columns), and the Y'CbCr at 10 and 16 bits is the exact one, rounded to the nearest
 *
 *   WriteFFmpeg, param and clip changes during a render (issue #304), with the option --param-change (only this test):
 *     - between the frames of a render, kOfxActionInstanceChanged on the params and the clip that update the pixel
 *       format info (Codec, Format, the Pref. params, Show Avail., DNxHD Codec Profile, Hap Format, the Source clip),
 *       and kOfxActionSyncPrivateData: each action must succeed, and every frame must be encoded
 *
 *   WriteFFmpeg, the Codec edited during beginSequenceRender (issue #319), with the option --codec-edit-during-begin
 *   (only this test):
 *     - beginSequenceRender on a render thread is stopped where it sends its warning, while the main thread calls
 *       kOfxActionInstanceChanged on the Codec (a user edit); then the frames render, and the file must be complete.
 *       With ThreadSanitizer (ofx_render_test_tsan), WriteFFmpeg must have no data race between the two actions
 *
 *   WritePFM and ReadPFM fail on errors (issues #294, #296), each with its option (only this test):
 *     - --pfm-write-errors (not on MS Windows: exit status 77): WritePFM, the file can not grow beyond 16 KB
 *       (RLIMIT_FSIZE, as on a full disk): the render must fail with an error that names the file, and the same
 *       instance must then write the file without the limit; then, with "Overwrite" unchecked, a file that exists but
 *       can not be read (mode 000) must be left as it is (skipped as root)
 *     - --pfm-read-errors: ReadPFM, a header with an empty, negative or huge size: getRegionOfDefinition must fail
 *   WriteFFmpeg selects a pixel format that fits the Pref. Pixel Coding (issue #218), with the option --pixfmt-choice
 *   (only this test):
 *     - FFV1 at 720x576 and 720x480, with each Pref. Pixel Coding (4:2:0, 4:2:2, 4:4:4) and Bit Depth: the
 *       stream has the format of that coding and depth (yuv422p with the defaults, not yuv440p); with Alpha, a format
 *       with alpha and that chroma subsampling, or RGB
 *     - every encoder of the Codec menu that has a 4:2:2, 4:4:4 or RGB format: never 4:4:0 for a 4:2:2 or 4:4:4
 *       preference
 *   WriteFFmpeg encodes with the encoder selected in the Codec menu (issue #348), with the option --selected-encoder
 *   (only this test):
 *     - for each encoder of the menu that is not the default encoder of its codec ID (avcodec_find_encoder()), e.g.
 *       libx264rgb, libopenh264, libxvid, libopenjpeg: a short movie must be encoded, and only the codec context of
 *       that encoder may log (the default encoder of the codec ID must not be opened); a pair is not checkable unless
 *       the default encoder is in the menu and logs when it is selected (positive control); fails if the menu is
 *       empty, skipped (exit status 77) if no pair is checkable
 *
 *   WriteFFmpeg, a Codec menu encoder missing from the FFmpeg build (issue #388), with the option --missing-encoder
 *   (only this test), run with OFX_IO_TEST_HIDE_ENCODERS=prores_ks (see avcodec_find_encoder_by_name(), below):
 *     - the Codec menu must still list the six ProRes entries (the indices of a saved project do not change)
 *     - a writer on the default Codec (index 0, ProRes 4444, with an empty codecShortName, as in an old project) and
 *       one configured for ProRes 422 HQ must fail the render with an error that names prores_ks: they must not
 *       encode with the default encoder of the container
 *     - positive control: an encoder of the menu that is not hidden must render
 *
 *   ReadFFmpeg, DestroyInstance closes the movie (issue #290), with the option --destroy-closes-file (only this test):
 *     - render a frame, destroy the instance: the movie is no longer open, before Unload; twice, with the movie
 *       rewritten at another size in between: the second instance (maybe at the address of the first one) reads it
 *
 *   ReadFFmpeg, concurrent renders of the views of a movie with one video stream per view (issue #291), with the
 *   option --views-concurrent (only this test; the ffmpeg CLI writes the movie):
 *     - every frame of view 0, then of view 1, rendered on one thread (view 1 must be the negative of view 0), then
 *       both views of each frame and a random order on one thread, then a random order on 8 threads at once: each
 *       render must give the image of its view and frame
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"
#include "GenericWriter.h" // the test writer of #251
#include "ofxsFileOpen.h" // fopen_utf8()

#include <OpenImageIO/imageio.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <climits> // INT_MAX
#include <optional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // CreateFileW()
#else
#include <dirent.h> // opendir()
#include <stdlib.h> // mkstemps()
#include <sys/resource.h> // setrlimit()
#include <sys/stat.h> // stat()
#include <unistd.h> // close()
#endif

// #388: the encoders named in $OFX_IO_TEST_HIDE_ENCODERS (comma-separated) are missing from this FFmpeg build for
// avcodec_find_encoder_by_name(), which WriteFFmpeg uses to find the encoder of a Codec menu entry. The plug-in sources
// are linked into this program, so they call this definition, not libavcodec's. Without the variable, it is FFmpeg's
// (n8.0 libavcodec/allcodecs.c:1032-1053). av_codec_iterate() still lists the hidden encoders.
// This needs a shared libavcodec: with a static one (libavcodec.a), which also defines this function, ofx_render_test
// fails to link (multiple definition).
extern "C" const AVCodec*
avcodec_find_encoder_by_name(const char* name)
{
    if (!name) return nullptr;
    if (const char* hidden = std::getenv("OFX_IO_TEST_HIDE_ENCODERS")) {
        const std::string list = std::string(",") + hidden + ",";
        if (list.find(std::string(",") + name + ",") != std::string::npos) return nullptr;
    }
    void* it = nullptr;
    const AVCodec* c = nullptr;
    while ((c = av_codec_iterate(&it))) {
        if (av_codec_is_encoder(c) && !std::strcmp(name, c->name)) return c;
    }
    return nullptr;
}

namespace {

// Extra param values for a WriteFFmpeg test.
struct WriterParams {
    std::vector<std::pair<std::string, std::string>> strings;
    std::vector<std::pair<std::string, int>> ints;
    std::vector<std::pair<std::string, double>> doubles; // double params
    double inputPar = 1.;            // the PAR of the source clip
    bool expectBeginFailure = false; // beginSequenceRender must fail (and nothing is rendered)
    bool constantImage = false;      // render the same mid-gray image for every frame instead of a ramp
    std::vector<float> color;        // if it has 3 values, render this constant RGB color for every frame (view 0)
    std::vector<float> frameValues;  // if not empty, the gray level of frame t is frameValues[t - 1] (instead of a ramp)
    bool expectNoFile = false;       // with expectBeginFailure: the output file must not be created
    double fps = 0.;                 // if > 0, the value of the "fps" param
    std::vector<int> views = { 0 };  // the views rendered for each frame, in this order: view 0 is the ramp, the
                                     // others its negative (1 - v)
    // choice params set after the instance is created (which sets e.g. the format from the file
    // extension), as the param and the text of the label of the option
    std::vector<std::pair<std::string, std::string>> choicesAfterCreate;
    // string params set after the instance is created (e.g. the OCIO output colorspace, which the writer sets from
    // the file extension)
    std::vector<std::pair<std::string, std::string>> stringsAfterCreate;
    std::string renderFile;          // if not empty, the "filename" param from the second frame on (#137)
    bool expectRenderFailure = false; // with renderFile or outputFetchFails: frames 2..N must fail to render
    // with expectRenderFailure: the persistent error after endSequenceRender must contain this
    std::string renderErrorText = "another active render";
    bool outputFetchFails = false;   // from the second frame on, the Output clip is connected and its image can not
                                     // be fetched (an error of GenericWriterPlugin::render(), before encode())
    // if renderW > 0: from the second frame on, the render window and the source image are renderW x renderH,
    // not W x H (the size of the movie, the output RoD at beginSequenceRender), as when Natron changes the
    // project format during the render
    int renderW = 0, renderH = 0;
    // if > 0, render colorPattern() instead of the gray ramp; 2: with its alpha ramp (else opaque); 3: binaryNoise();
    // 4 and 5: redBlue() lines and columns
    int pattern = 0;
    // the quantization of colorPattern() (see there)
    int patternQuant = 255;
    // the frames may fail to encode (and endSequenceRender may fail), with the error "Cannot encode frame"; if they
    // all encode, the file must be written (#267: what matters is that the encoder does not abort)
    bool allowEncodeFailure = false;
};

// The persistent warning of WriteFFmpeg after endSequenceRender (see testWriteFFmpeg()).
std::string gWarningAfterRender;

// The color pattern of frame t (issue #170), at column x and row r of the image buffer: smooth ramps (so that 4:2:2
// chroma subsampling only adds a small error), black and white included. Unpremultiplied (alpha < 1 if alpha).
// quant > 0: the values are on the grid k/quant, e.g. 255 (the 8-bit grid, which is also exact in 16 bits:
// k * 257 / 65535, whose two bytes are equal, so that it does not show byte-swapped 16-bit samples) or 65535.
// quant = 0: values that are on no grid (as float images, or 10-bit and 16-bit images, have), in [0.00617, 0.98765].
void colorPattern(int x, int r, int t, int W, int H, bool alpha, float* rgba, int quant = 255)
{
    auto q = [quant](double v) { return quant > 0 ? (float)((double)std::lround(v * quant) / quant) : (float)(0.00617 + 0.98148 * v); };
    rgba[0] = q((double)x / (W - 1));
    rgba[1] = q(1. - (double)r / (H - 1));
    rgba[2] = q(0.2 + 0.5 * (double)(x + r) / (W + H - 2) + 0.05 * t);
    rgba[3] = alpha ? q(1. - 0.75 * (double)x / (W - 1)) : 1.f;
}

// Binary RGB noise (#267): each of R, G and B of each pixel is 0 or 1, from a hash of (x, r, t, channel), so
// that the image is the same on every run. Opaque.
void binaryNoise(int x, int r, int t, float* rgba)
{
    for (int k = 0; k < 3; ++k) {
        uint32_t h = (uint32_t)x * 0x9E3779B1u ^ (uint32_t)r * 0x85EBCA77u ^ (uint32_t)t * 0xC2B2AE3Du ^ (uint32_t)k * 0x27D4EB2Fu;
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        rgba[k] = (h & 1u) ? 1.f : 0.f;
    }
    rgba[3] = 1.f;
}

// #207: opaque red (R' = 1) and blue (B' = 1) pixels, in alternating lines (red on the even rows of the image buffer)
// or columns (red on the even columns).
void redBlue(int x, int r, bool columns, float* rgba)
{
    const bool red = ((columns ? x : r) % 2) == 0;
    rgba[0] = red ? 1.f : 0.f;
    rgba[1] = 0.f;
    rgba[2] = red ? 0.f : 1.f;
    rgba[3] = 1.f;
}

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
    for (const auto& sp : wp.stringsAfterCreate) {
        setOverrideString(&ctx, sp.first.c_str(), sp.second);
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
        if ((t == 2) && wp.outputFetchFails && ctx.clips.count(kOfxImageEffectOutputClipName)) {
            Clip* o = ctx.clips[kOfxImageEffectOutputClipName];
            o->connected = true;
            pSetInt(PH(&o->props), kOfxImageClipPropConnected, 0, 1);
            o->noImage = true;
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
            if (wp.pattern == 3) {
                for (int r = 0; r < rh; ++r) {
                    for (int x = 0; x < rw; ++x) binaryNoise(x, r, t, &fp[((size_t)r * rw + x) * 4]);
                }
            } else if ((wp.pattern == 4) || (wp.pattern == 5)) {
                for (int r = 0; r < rh; ++r) {
                    for (int x = 0; x < rw; ++x) redBlue(x, r, wp.pattern == 5, &fp[((size_t)r * rw + x) * 4]);
                }
            } else if (wp.pattern > 0) {
                for (int r = 0; r < rh; ++r) {
                    for (int x = 0; x < rw; ++x) colorPattern(x, r, t, rw, rh, wp.pattern > 1, &fp[((size_t)r * rw + x) * 4], wp.patternQuant);
                }
            } else {
                const bool colored = (wp.color.size() == 3) && (view == 0);
                for (size_t i = 0; i < npix; ++i) {
                    fp[i * 4 + 0] = colored ? wp.color[0] : v;
                    fp[i * 4 + 1] = colored ? wp.color[1] : v;
                    fp[i * 4 + 2] = colored ? wp.color[2] : v;
                    fp[i * 4 + 3] = 1.f;
                }
            }
            PropSet a = makeRenderInArgs((double)t, rw, rh);
            pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, view);
            OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
            if (!ok(st)) {
                std::printf(wp.allowEncodeFailure ? "  render frame %d view %d = %d (a failure is allowed)\n" : "  FAIL: render frame %d view %d = %d\n", t, view, st);
                ++rfail;
            }
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
        if (!ok(st) && !(wp.allowEncodeFailure && rfail)) { std::printf("  FAIL: endSequenceRender = %d\n", st); return 1; }
        std::lock_guard<std::mutex> lock(gMessagesMutex);
        gWarningAfterRender = gPersistentWarning;
    }

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    std::FILE* f = std::fopen(outFile.c_str(), "rb");
    long sz = 0;
    if (f) { std::fseek(f, 0, SEEK_END); sz = std::ftell(f); std::fclose(f); }
    std::printf("  encoded %s (%ld bytes), render failures=%d\n", outFile.c_str(), sz, rfail);
    if (wp.expectRenderFailure) {
        const int expected = (nFrames - 1) * (int)wp.views.size();
        if ((rfail != expected) || (gPersistentError.find(wp.renderErrorText) == std::string::npos)) {
            std::printf("  FAIL: expected %d render failures, \"%s\" (error: \"%s\")\n", expected, wp.renderErrorText.c_str(), gPersistentError.c_str());
            return 1;
        }
        std::printf("  PASS (render failed as expected: %s)\n", gPersistentError.c_str());
        return 0;
    }
    if (wp.allowEncodeFailure && rfail) {
        if (gPersistentError.find("Cannot encode frame") == std::string::npos) {
            std::printf("  FAIL: %d frames failed to render, without the error \"Cannot encode frame\" (error: \"%s\")\n", rfail, gPersistentError.c_str());
            return 1;
        }
        std::printf("  PASS (the encoder failed cleanly: %s)\n", gPersistentError.c_str());
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
// If tolerance > 0, the video frame rate must only be within this relative tolerance of num/den (a
// codec whose time base is limited, such as MPEG-4 part 2, has the closest representable rate), and the
// tmcd track must be at num/den (FFmpeg >= 9.1, which derives the tmcd timescale from the timecode rate,
// libavformat/movenc.c: mov_create_timecode_track()) or at the video frame rate (FFmpeg < 9.1, which
// copies the video track's timescale).
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
    const bool close = tolerance > 0. && video.den > 0 && std::fabs(av_q2d(video) / av_q2d(expected) - 1.) <= tolerance && (av_cmp_q(tmcd, expected) == 0 || av_cmp_q(tmcd, video) == 0);
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

    // Render windows narrower than the frame (#163), at frame 1, at render scale 1 and 1/2: the pixels of the whole
    // frame in the window, nothing written out of it (the rest of the image keeps a sentinel value, compared on its
    // bits). At scale 1/2, GenericReader decodes into its buffer, of the size of the window, which used to fail ("The
    // host provided an image of wrong size").
    int wfail = 0;
    for (double scale : { 1., 0.5 }) {
        const int OW = (int)(W * scale), OH = (int)(H * scale); // the output image, at the render scale
        setupClipImage(out, OW, OH, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
        double rs[2] = { scale, scale };
        pSetDoubleN(PH(&out->image), kOfxImageEffectPropRenderScale, 2, rs);
        // the clip's RoD is in canonical coordinates (independent of the render scale)
        double clipRod[4] = { 0., 0., (double)W, (double)H };
        pSetDoubleN(PH(&out->props), kOfxImageEffectPropRegionOfDefinition, 4, clipRod);
        float* op = reinterpret_cast<float*>(out->buffer.data());
        const uint32_t sentinelBits = 0xc49a5a5au;
        const size_t nValues = out->buffer.size() / sizeof(float);
        std::vector<float> ref;
        const OfxRectI wins[] = { { 0, 0, OW, OH }, { OW / 2, 0, OW / 2 + 3, OH }, { 0, OH / 2, OW, OH / 2 + 2 }, { OW / 4, OH / 4, OW / 2, OH / 2 }, { OW - 1, OH - 1, OW, OH } };
        for (const OfxRectI& w : wins) {
            for (size_t i = 0; i < nValues; ++i) std::memcpy(op + i, &sentinelBits, sizeof(sentinelBits));
            PropSet a = makeRenderInArgs(1., OW, OH);
            int win[4] = { w.x1, w.y1, w.x2, w.y2 };
            pSetIntN(PH(&a), kOfxImageEffectPropRenderWindow, 4, win);
            pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
            gPersistentError.clear();
            OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
            if (!ok(st)) { std::printf("  FAIL: scale %g, render window %d,%d,%d,%d = %d %s\n", scale, w.x1, w.y1, w.x2, w.y2, st, gPersistentError.c_str()); ++wfail; continue; }
            if (ref.empty()) { ref.assign(op, op + nValues); continue; }
            int bad = 0;
            for (int y = 0; y < OH; ++y) {
                for (int x = 0; x < OW; ++x) {
                    const bool in = x >= w.x1 && x < w.x2 && y >= w.y1 && y < w.y2;
                    for (int c = 0; c < outComps; ++c) {
                        const size_t i = ((size_t)y * OW + x) * outComps + c;
                        bad += in ? (std::memcmp(op + i, &ref[i], sizeof(float)) != 0) : (std::memcmp(op + i, &sentinelBits, sizeof(float)) != 0);
                    }
                }
            }
            if (bad) { std::printf("  FAIL: scale %g, render window %d,%d,%d,%d: %d wrong values\n", scale, w.x1, w.y1, w.x2, w.y2, bad); ++wfail; }
        }
    }
    if (!wfail) std::printf("  PASS: render windows narrower than the frame, at scale 1 and 1/2\n");

    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);

    if (rfail || vfail || wfail) { std::printf("  FAIL (decode: renderFails=%d monotonicViolations=%d windowFails=%d)\n", rfail, vfail, wfail); return 1; }
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

// Issue #217: libx265 into MP4 with 1 or 2 frames, fewer than its B-frame delay (2 by default). libx265 then gives
// the first packets an uninitialized dts (x265 source/encoder/encoder.cpp: m_bframeDelayTime is only set when frame
// number m_bframeDelay is received), which the mov muxer rejects in the flush ("pts/dts pair unsupported", then
// "Cannot write frame"), and WriteFFmpeg ignored the error of the flush: endSequenceRender succeeded and the file
// had no frame (257 bytes). As the dts is uninitialized memory, this happened in 5 to 10 runs out of 40 (when run
// first, on macOS): each size is encoded 20 times. Each file must succeed and have all its frames.
int testWriteFFmpegX265FewFrames(const std::string& outFile)
{
    if (!avcodec_find_encoder_by_name("libx265")) { std::printf("  SKIP: no libx265 encoder\n"); return 0; }
    int failures = 0;
    for (int nFrames = 1; nFrames <= 2; ++nFrames) {
        for (int run = 0; run < 20; ++run) {
            WriterParams wp;
            wp.strings = { { "codecShortName", "libx265" } };
            int f = testWriteFFmpeg(outFile, 128, 96, nFrames, wp);
            int n = 0, nI = 0;
            if (!f) f = probeVideoFrames(outFile, &n, &nI);
            if (!f && (n != nFrames)) { std::printf("  FAIL: decoded %d frames, expected %d\n", n, nFrames); f = 1; }
            failures += f;
        }
    }
    std::remove(outFile.c_str());
    if (failures) std::printf("  FAIL (libx265 with 1 or 2 frames, %d failures)\n", failures);
    else std::printf("  PASS (libx265 with 1 or 2 frames)\n");
    return failures;
}

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

// Decode frame 1 of a movie with ReadFFmpeg (default params: Auto Y'CbCr matrix and range): the mean of each
// component, and the File Colorspace that ReadFFmpeg guessed (its ocioInputSpace param).
int readFFmpegMeanRGB(const std::string& inFile, int W, int H, double rgb[3], std::string* fileColorspace)
{
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, inFile);
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, 1, kOfxImageEffectContextReader);
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
    if (fileColorspace) {
        auto it = ctx.params.find("ocioInputSpace");
        *fileColorspace = (it != ctx.params.end() && it->second.hasOv && !it->second.ov.s.empty()) ? it->second.ov.s[0] : std::string();
    }
    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) { std::printf("  FAIL: no Output clip\n"); return 1; }
    out->connected = true;
    setupClipImage(out, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); if (!ok(cp)) { std::printf("  FAIL: getClipPreferences = %d\n", cp); return 1; } }
    PropSet a = makeRenderInArgs(1., W, H);
    OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
    const float* fp = reinterpret_cast<const float*>(out->buffer.data());
    const size_t npix = (size_t)W * H;
    for (int c = 0; c < 3; ++c) {
        double sum = 0.;
        for (size_t i = 0; i < npix; ++i) sum += fp[i * 3 + c];
        rgb[c] = sum / (double)npix;
    }
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (!ok(st)) { std::printf("  FAIL: ReadFFmpeg render = %d\n", st); return 1; }
    return 0;
}

// The color metadata of the format of a movie (as ffprobe -show_format shows it), or an empty string.
std::string formatColorspaceMetadata(const std::string& file)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) return std::string();
    const AVDictionaryEntry* t = av_dict_get(fmt->metadata, "uk.co.thefoundry.Colorspace", nullptr, 0);
    std::string v = (t && t->value) ? t->value : "";
    avformat_close_input(&fmt);
    return v;
}

// Encode a constant color, check the color tags of the stream and the Y' it decodes to (computed from the Y'CbCr
// matrix of ITU-T H.273: Y' = Kr R + (1 - Kr - Kb) G + Kb B, in video range), and decode it back with ReadFFmpeg.
struct ColorRoundTrip {
    const char* label;
    const char* codec;    // codecShortName
    const char* ext;
    const char* ocioOut;  // OCIO output colorspace of the writer
    int W, H;
    int space, primaries, trc; // expected tags (-1: not checked)
    bool proResHeader;    // the ProRes frame header must have the same tags
    double kr, kb;        // the matrix of the decoded Y' (kr < 0: RGB, not checked)
    int bits;             // the bit depth of the decoded Y'
    const char* fileColorspace; // if not null, the File Colorspace that ReadFFmpeg must guess
    int dnxhdProfile = -1; // if >= 0, the DNxHDCodecProfile (written with the Video Range)
};

int testWriteFFmpegColorRoundTrip(const std::string& outFile, const ColorRoundTrip& c)
{
    const float color[3] = { 0.8f, 0.3f, 0.1f };
    WriterParams wp;
    wp.strings = { { "codecShortName", c.codec } };
    wp.stringsAfterCreate = { { "ocioOutputSpace", c.ocioOut } };
    if (c.dnxhdProfile >= 0) {
        wp.ints = { { "DNxHDCodecProfile", c.dnxhdProfile }, { "DNxHDEncodeVideoRange", 1 } };
    }
    wp.color.assign(color, color + 3);
    const std::string file = outFile + c.ext;
    if (testWriteFFmpeg(file, c.W, c.H, 2, wp)) return 1;
    int failures = 0;
    auto check = [&](bool cond, const std::string& what) {
        if (!cond) { std::printf("  FAIL: %s\n", what.c_str()); ++failures; }
    };
    StreamInfo s;
    if (probeStream(file, &s)) return 1;
    if (c.space >= 0) check(s.space == c.space, std::string("color_space must be ") + av_color_space_name((AVColorSpace)c.space));
    if (c.primaries >= 0) check(s.primaries == c.primaries, std::string("color_primaries must be ") + av_color_primaries_name((AVColorPrimaries)c.primaries));
    if (c.trc >= 0) check(s.trc == c.trc, std::string("color_transfer must be ") + av_color_transfer_name((AVColorTransferCharacteristic)c.trc));
    if (c.proResHeader) {
        int pr = -1, tr = -1, mx = -1;
        const bool found = proResFrameHeaderColors(file, &pr, &tr, &mx);
        std::printf("  ProRes frame header: color_primaries %d, transfer_characteristic %d, matrix_coefficients %d\n", pr, tr, mx);
        check(found && pr == c.primaries && tr == c.trc && mx == c.space, "the ProRes frame header must have the tags of the stream");
    }
    if (c.kr >= 0.) {
        const double y = c.kr * color[0] + (1. - c.kr - c.kb) * color[1] + c.kb * color[2];
        const int expected = (int)std::lround((16. + 219. * y) * (double)(1 << (c.bits - 8)));
        const int tolerance = 1 << (c.bits - 8);
        std::printf("  decoded Y' %d, expected %d +-%d (Kr %.4f, Kb %.4f)\n", s.center.empty() ? -1 : s.center[0], expected, tolerance, c.kr, c.kb);
        check(!s.center.empty() && std::abs(s.center[0] - expected) <= tolerance, "the decoded Y' must be the one of the Y'CbCr matrix");
    }
    double rgb[3] = { -1., -1., -1. };
    std::string fileColorspace;
    if (readFFmpegMeanRGB(file, c.W, c.H, rgb, &fileColorspace)) return failures + 1;
    std::printf("  ReadFFmpeg: mean RGB %.4f %.4f %.4f (encoded %.4f %.4f %.4f), File Colorspace \"%s\", metadata \"%s\"\n", rgb[0], rgb[1], rgb[2],
                color[0], color[1], color[2], fileColorspace.c_str(), formatColorspaceMetadata(file).c_str());
    for (int i = 0; i < 3; ++i) check(std::fabs(rgb[i] - color[i]) <= 0.01, "ReadFFmpeg must decode the encoded color");
    if (c.fileColorspace) {
        check(formatColorspaceMetadata(file) == c.fileColorspace, std::string("the uk.co.thefoundry.Colorspace metadata must be ") + c.fileColorspace);
        check(fileColorspace == c.fileColorspace, std::string("the File Colorspace of ReadFFmpeg must be ") + c.fileColorspace);
    }
    std::printf(failures ? "  FAIL (color round trip)\n" : "  PASS (color round trip)\n");
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
    const AVPixelFormat* pixFmts = nullptr;
    const void* configs = nullptr;
    int nConfigs = 0;
    if (avcodec_get_supported_config(nullptr, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &nConfigs) >= 0 && nConfigs > 0) {
        pixFmts = static_cast<const AVPixelFormat*>(configs); // terminated by AV_PIX_FMT_NONE
    }
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

// Issue #170: an uncompressed codec, written with the given settings, and read back.
struct UncompressedCase {
    const char* label;
    const char* ext;          // the file extension, which gives the container
    const char* codec;        // the codecShortName param (as in a saved project)
    int prefCoding;           // Pref. Pixel Coding: 0 YUV420, 1 YUV422, 2 YUV444, 3 RGB
    int prefDepth;            // Pref. Bit Depth: 0 8, 1 10, 2 12, 3 16
    bool alpha;               // Alpha checked, and the pattern has an alpha ramp
    const char* expectCodec;  // the codec that libavformat reads
    const char* expectTag;    // the codec tag, as av_fourcc_make_string() prints it
    const char* expectPixFmt; // the pixel format
    double tolerance;         // the largest difference of R, G, B, A with the pattern (ReadFFmpeg output)
    const char* expectMessage = nullptr; // a message that the plug-in must send (e.g. the codec was replaced)
    // a text that the persistent warning must contain after the render (e.g. the codec was replaced)
    const char* expectWarningAfterRender = nullptr;
    int quant = 255;          // the quantization of colorPattern(): 255 (the 8-bit grid), or 0 (values on no grid)
    int W = 64, H = 48;       // the size of the images
    bool expectBeginFailure = false; // beginSequenceRender must fail, with the error expectMessage, and write no file
};

int testUncompressed(const std::string& outFile, const UncompressedCase& c)
{
    const int N = 3;
    const int W = c.W, H = c.H;
    WriterParams wp;
    wp.strings = { { "codecShortName", c.codec } };
    wp.ints = { { "prefPixelCoding", c.prefCoding }, { "prefBitDepth", c.prefDepth }, { "enableAlpha", c.alpha ? 1 : 0 },
                { "inputPremult", 2 } }; // unpremultiplied: the pattern is written as is
    wp.pattern = c.alpha ? 2 : 1;
    wp.patternQuant = c.quant;
    wp.expectBeginFailure = c.expectBeginFailure;
    wp.expectNoFile = c.expectBeginFailure;
    gMessages.clear();
    gWarningAfterRender.clear();
    if (testWriteFFmpeg(outFile, W, H, N, wp)) {
        std::printf("  messages:\n%s", gMessages.c_str());
        return 1;
    }
    int failures = 0;
    if (c.expectMessage && (gMessages.find(c.expectMessage) == std::string::npos)) {
        std::printf("  FAIL: no message \"%s\" in:\n%s", c.expectMessage, gMessages.c_str());
        ++failures;
    }
    if (c.expectBeginFailure) {
        std::printf(failures ? "  FAIL (%s)\n" : "  PASS (%s)\n", c.label);
        return failures;
    }
    if (c.expectWarningAfterRender && (gWarningAfterRender.find(c.expectWarningAfterRender) == std::string::npos)) {
        std::printf("  FAIL: the persistent warning after the render is \"%s\", it does not contain \"%s\"\n", gWarningAfterRender.c_str(),
                    c.expectWarningAfterRender);
        ++failures;
    }
    StreamInfo s;
    if (probeStream(outFile, &s)) return 1;
    if (s.codec != c.expectCodec) { std::printf("  FAIL: codec %s, expected %s\n", s.codec.c_str(), c.expectCodec); ++failures; }
    if (s.tag != c.expectTag) { std::printf("  FAIL: codec tag \"%s\", expected \"%s\"\n", s.tag.c_str(), c.expectTag); ++failures; }
    if (s.pixFmt != c.expectPixFmt) { std::printf("  FAIL: pix_fmt %s, expected %s\n", s.pixFmt.c_str(), c.expectPixFmt); ++failures; }
    if ((int)s.center.size() != N) { std::printf("  FAIL: %d frames decoded, expected %d\n", (int)s.center.size(), N); ++failures; }
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(av_get_pix_fmt(s.pixFmt.c_str()));
    const bool yuv = d && !(d->flags & AV_PIX_FMT_FLAG_RGB) && d->nb_components >= 3;
    // the file has alpha (e.g. ARGB for RGB in QuickTime with an odd width): ReadFFmpeg gives RGBA, whose alpha must
    // be the pattern's (1 if the pattern is opaque)
    const bool readAlpha = c.alpha || (d && (d->flags & AV_PIX_FMT_FLAG_ALPHA));
    // (of these containers, only Matroska stores the range: the QuickTime 'colr' nclc atom has none, and NUT and AVI
    // have no color tags)
    if (yuv && (s.range != AVCOL_RANGE_MPEG) && (std::string(c.ext) == "mkv")) {
        std::printf("  FAIL: Y'CbCr is not tagged video range\n");
        ++failures;
    }
    // the matrix that the writer converts with (BT.601 in SD), in the 'colr' atom or the Matroska Colour element:
    // setColorTags() tags every Y'CbCr stream (#210), the v308, v408 and v410 encoders of FFmpeg < 9 included
    if (yuv && (s.space != AVCOL_SPC_SMPTE170M) && ((std::string(c.ext) == "mkv") || (std::string(c.ext) == "mov"))) {
        std::printf("  FAIL: Y'CbCr is not tagged with the BT.601 (SMPTE 170M) matrix\n");
        ++failures;
    }

    // ReadFFmpeg: RGBA output if the file has alpha, with no premultiplication
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    const int optRGBA = choiceIndex(ctx, "outputComponents", "RGBA");
    if (optRGBA < 0) { std::printf("  FAIL: no RGBA option in outputComponents\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    Effect inst;
    inst.delegate = &ctx;
    setInstanceProps(inst, W, H, N, kOfxImageEffectContextReader);
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
    if (readAlpha) {
        setOverrideInt(&ctx, "outputComponents", optRGBA);
        setOverrideInt(&ctx, "filePremult", 2); // unpremultiplied
        setOverrideInt(&ctx, "outputPremult", 2);
    }
    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) { std::printf("  FAIL: no Output clip\n"); return 1; }
    out->connected = true;
    const int outComps = readAlpha ? 4 : 3;
    setupClipImage(out, W, H, readAlpha ? kOfxImageComponentRGBA : kOfxImageComponentRGB, kOfxBitDepthFloat, true);
    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); if (!ok(cp)) { std::printf("  FAIL: getClipPreferences = %d\n", cp); return 1; } }
    float* fp = reinterpret_cast<float*>(out->buffer.data());
    double maxErr[4] = { 0., 0., 0., 0. };
    int worst[4][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    for (int t = 1; t <= N; ++t) {
        for (size_t i = 0; i < out->buffer.size() / sizeof(float); ++i) fp[i] = -1.f; // sentinel
        PropSet a = makeRenderInArgs((double)t, W, H);
        OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        if (!ok(st)) { std::printf("  FAIL: read render frame %d = %d\n", t, st); ++failures; continue; }
        double frameErr = 0.;
        for (int r = 0; r < H; ++r) {
            for (int x = 0; x < W; ++x) {
                float e[4];
                colorPattern(x, r, t, W, H, c.alpha, e, c.quant);
                const float* v = &fp[((size_t)r * W + x) * outComps];
                for (int k = 0; k < outComps; ++k) {
                    const double err = std::fabs((double)v[k] - (double)e[k]);
                    if (!(err <= maxErr[k])) { maxErr[k] = err; worst[k][0] = x; worst[k][1] = r; } // (NaN is the largest)
                    if (!(err <= frameErr)) frameErr = err;
                }
            }
        }
        std::printf("  frame %d: largest difference (x 255) %.3f\n", t, frameErr * 255);
    }
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::printf("  ReadFFmpeg: largest difference with the pattern (x 255): R %.3f at (%d,%d), G %.3f at (%d,%d), B %.3f at (%d,%d)",
                maxErr[0] * 255, worst[0][0], worst[0][1], maxErr[1] * 255, worst[1][0], worst[1][1], maxErr[2] * 255, worst[2][0], worst[2][1]);
    if (readAlpha) std::printf(", A %.3f at (%d,%d)", maxErr[3] * 255, worst[3][0], worst[3][1]);
    std::printf(" (tolerance %.3f)\n", c.tolerance * 255);
    for (int k = 0; k < outComps; ++k) {
        if (!(maxErr[k] <= c.tolerance)) { std::printf("  FAIL: component %d differs by %g\n", k, maxErr[k]); ++failures; }
    }
    std::remove(outFile.c_str());
    std::printf(failures ? "  FAIL (%s)\n" : "  PASS (%s)\n", c.label);
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

// #267: FFmpeg's dnxhd encoder stores the qscale of each macroblock in a uint8_t (libavcodec/dnxhdenc.h:101), and its
// default qmax is 1024: on binary RGB noise in DNxHR 444, rate control picks a qscale above 255, which wraps, and
// libavcodec aborts (put_bits.h assertion), which kills the host (this test). WriteFFmpeg limits qmax to 256: the
// frames must encode, or fail with "Cannot encode frame". Run by itself (--dnxhr444-noise), so that an abort only
// fails this test.
int testDNxHR444Noise(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg DNxHR 444, binary RGB noise: no abort in FFmpeg's encoder (issue #267) ====\n");
    WriterParams wp;
    wp.strings = { { "codecShortName", "dnxhd" } };
    wp.ints = { { "DNxHDCodecProfile", 0 } }; // DNxHR 444
    // relies on WriteFFmpeg writing DNxHR 444 as gbrp10: in yuv444p10, this noise only needs a qscale of about 35
    wp.pattern = 3;
    wp.allowEncodeFailure = true;
    const int failures = testWriteFFmpeg(outFile, 256, 120, 2, wp); // 256x120: the smallest DNxHR size
    std::remove(outFile.c_str());
    return failures;
}

// Gives the R/G/B/A checkboxes whose kOfxParamPropSecret the plug-in did not set the default of the spec, 0 (visible:
// ofxParam.h, kOfxParamPropSecret, "Default - 0"). The test host fails when a plug-in reads an unset property.
static void setDefaultCheckboxesSecret(Effect& ctx)
{
    for (const char* name : { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA }) {
        auto it = ctx.params.find(name);
        if ((it != ctx.params.end()) && !it->second.props->props.count(kOfxParamPropSecret)) {
            pSetInt(PH(it->second.props), kOfxParamPropSecret, 0, 0);
        }
    }
}

// A writer instance (WriteFFmpeg by default) that the tests of #288, #289, #292, #294 and #251 render several times, as
// Natron renders a Write node: beginSequenceRender, the frames (maybe on several threads), endSequenceRender. The
// source image is a constant mid-gray, written once, so that concurrent renders only read it.
class WriterSession {
public:
    WriterSession() = default;
    WriterSession(const WriterSession&) = delete;
    WriterSession& operator=(const WriterSession&) = delete;
    ~WriterSession()
    {
        if (_created) {
            _p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&_inst), nullptr, nullptr);
        }
        if (_p) {
            _p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        }
    }

    // The instance (Writer context) of the plug-in pluginId, with the params of wp (strings, ints and fps only); 0 if
    // it was created.
    int create(const std::string& outFile, int W, int H, int nFrames, const WriterParams& wp,
               const char* pluginId = "fr.inria.openfx.WriteFFmpeg")
    {
        _W = W;
        _H = H;
        OfxPlugin* p = findPlugin(pluginId);
        if (!p) { std::printf("  FAIL: %s not found\n", pluginId); return 1; }
        if (loadAndDescribe(p, _ctx, _ctx, kOfxImageEffectContextWriter) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
        _p = p;
        setOverrideString(&_ctx, kOfxImageEffectFileParamName, outFile);
        for (const auto& sp : wp.strings) {
            if (!_ctx.params.count(sp.first)) { std::printf("  FAIL: no param %s\n", sp.first.c_str()); return 1; }
            setOverrideString(&_ctx, sp.first.c_str(), sp.second);
        }
        for (const auto& ip : wp.ints) {
            if (!_ctx.params.count(ip.first)) { std::printf("  FAIL: no param %s\n", ip.first.c_str()); return 1; }
            setOverrideInt(&_ctx, ip.first.c_str(), ip.second);
        }
        if (wp.fps > 0.) {
            setFps(wp.fps);
        }
        _inst.delegate = &_ctx;
        setInstanceProps(_inst, W, H, nFrames, kOfxImageEffectContextWriter);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(&_inst), nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
        _created = true;
        setDefaultCheckboxesSecret(_ctx);
        _src = _ctx.clips.count(kOfxImageEffectSimpleSourceClipName) ? _ctx.clips[kOfxImageEffectSimpleSourceClipName] : nullptr;
        if (!_src) { std::printf("  FAIL: no Source clip\n"); return 1; }
        _src->connected = true;
        setupClipImage(_src, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
        std::fill(reinterpret_cast<float*>(_src->buffer.data()), reinterpret_cast<float*>(_src->buffer.data()) + (size_t)W * H * 4, 0.5f);
        if (_ctx.clips.count(kOfxImageEffectOutputClipName)) {
            Clip* o = _ctx.clips[kOfxImageEffectOutputClipName];
            o->connected = false;
            setupClipImage(o, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
            pSetInt(PH(&o->props), kOfxImageClipPropConnected, 0, 0);
        }
        PropSet outArgs;
        p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&_inst), nullptr, PH(&outArgs));
        return 0;
    }

    void setFps(double fps)
    {
        _ctx.params["fps"].hasOv = true;
        _ctx.params["fps"].ov.d = { fps };
    }

    void setInt(const char* name, int value) { setOverrideInt(&_ctx, name, value); }

    void setString(const char* name, const std::string& value) { setOverrideString(&_ctx, name, value); }

    void setFilename(const std::string& file) { setOverrideString(&_ctx, kOfxImageEffectFileParamName, file); }

    // the params (and clips) of the instance
    Effect& context() { return _ctx; }

    // Sets the choice param name to the option whose label is label; false if there is no such option.
    bool setChoice(const char* name, const char* label)
    {
        const int i = choiceIndex(_ctx, name, label);
        if (i < 0) { std::printf("  FAIL: no option \"%s\" in param %s\n", label, name); return false; }
        setOverrideInt(&_ctx, name, i);
        return true;
    }

    // the RGBA float pixels of the source image (W x H)
    float* sourcePixels() { return reinterpret_cast<float*>(_src->buffer.data()); }

    // kOfxActionInstanceChanged on the param or clip name (a user edit), as the host calls it on its main thread
    OfxStatus instanceChanged(const char* type, const char* name)
    {
        PropSet a;
        OfxPropertySetHandle ap = PH(&a);
        pSetString(ap, kOfxPropType, 0, type);
        pSetString(ap, kOfxPropName, 0, name);
        pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
        pSetDouble(ap, kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. };
        pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        return _p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(&_inst), ap, nullptr);
    }

    // kOfxActionSyncPrivateData, as the host calls it before it saves the project
    OfxStatus syncPrivateData() { return _p->mainEntry(kOfxActionSyncPrivateData, reinterpret_cast<OfxImageEffectHandle>(&_inst), nullptr, nullptr); }

    // the field order of the source clip, e.g. kOfxImageFieldLower for an interlaced input
    void setSourceFieldOrder(const char* fieldOrder) { pSetString(PH(&_src->props), kOfxImageClipPropFieldOrder, 0, fieldOrder); }

    OfxStatus begin(int nFrames) { return sequenceAction(kOfxImageEffectActionBeginSequenceRender, nFrames); }
    OfxStatus end(int nFrames) { return sequenceAction(kOfxImageEffectActionEndSequenceRender, nFrames); }

    // May be called concurrently.
    OfxStatus render(int t)
    {
        PropSet a = makeRenderInArgs((double)t, _W, _H);
        return _p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&_inst), PH(&a), nullptr);
    }

    // begin, render each frame in order, end: 0 if all succeeded
    int renderAll(int nFrames)
    {
        OfxStatus st = begin(nFrames);
        if (!ok(st)) { std::printf("  FAIL: beginSequenceRender = %d (error: \"%s\")\n", st, persistentError().c_str()); return 1; }
        int failures = 0;
        for (int t = 1; t <= nFrames; ++t) {
            st = render(t);
            if (!ok(st)) { std::printf("  FAIL: render frame %d = %d (error: \"%s\")\n", t, st, persistentError().c_str()); ++failures; }
        }
        st = end(nFrames);
        if (!ok(st)) { std::printf("  FAIL: endSequenceRender = %d (error: \"%s\")\n", st, persistentError().c_str()); ++failures; }
        return failures ? 1 : 0;
    }

    static std::string persistentError()
    {
        std::lock_guard<std::mutex> lock(gMessagesMutex);
        return gPersistentError;
    }

private:
    OfxStatus sequenceAction(const char* action, int nFrames)
    {
        PropSet a;
        OfxPropertySetHandle ap = PH(&a);
        double fr[2] = { 1., (double)nFrames };
        pSetDoubleN(ap, kOfxImageEffectPropFrameRange, 2, fr);
        pSetDouble(ap, kOfxImageEffectPropFrameStep, 0, 1.);
        pSetInt(ap, kOfxPropIsInteractive, 0, 0);
        double rs[2] = { 1., 1. };
        pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
        pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
        pSetInt(ap, kFnOfxImageEffectPropView, 0, 0);
        return _p->mainEntry(action, reinterpret_cast<OfxImageEffectHandle>(&_inst), ap, nullptr);
    }

    OfxPlugin* _p = nullptr;
    Effect _ctx;
    Effect _inst;
    Clip* _src = nullptr;
    int _W = 0;
    int _H = 0;
    bool _created = false;
};

// #289: render a movie, then on the same instance a beginSequenceRender that fails in the settings of the video
// stream (setFailure(true)), then endSequenceRender, which Natron calls after a failed beginSequenceRender: it used to
// write the trailer of a file whose header was never written (av_write_trailer() with a NULL AVIOContext), which
// crashed. Then a render with setFailure(false) must work again (not "Another render is currently active").
int testWriteFFmpegBeginFailure(const std::string& outFile, int W, int H, const WriterParams& wp,
                                const std::function<void(WriterSession&, bool)>& setFailure, const char* expectedError)
{
    const int N = 2;
    std::remove(outFile.c_str());
    WriterSession s;
    if (s.create(outFile, W, H, N, wp)) return 1;
    int failures = 0;
    int nFrames = 0, nIFrames = 0;
    setFailure(s, false);
    if (s.renderAll(N) || probeVideoFrames(outFile, &nFrames, &nIFrames) || (nFrames != N)) {
        std::printf("  FAIL: the first render (%d frames written)\n", nFrames);
        return 1;
    }
    setFailure(s, true);
    OfxStatus st = s.begin(N);
    const std::string error = WriterSession::persistentError();
    if (ok(st) || (error.find(expectedError) == std::string::npos)) {
        std::printf("  FAIL: beginSequenceRender = %d, expected a failure with \"%s\" (error: \"%s\")\n", st, expectedError, error.c_str());
        ++failures;
    }
    st = s.end(N); // crashed in av_write_trailer() (#289)
    std::printf("  failed beginSequenceRender (\"%s\"), then endSequenceRender = %d: no crash\n", error.c_str(), st);
    setFailure(s, false);
    nFrames = 0;
    if (s.renderAll(N) || probeVideoFrames(outFile, &nFrames, &nIFrames) || (nFrames != N)) {
        std::printf("  FAIL: the render after the failed one (%d frames written)\n", nFrames);
        ++failures;
    }
    std::remove(outFile.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
}

int testWriteFFmpegBeginFailures(const std::string& outFile)
{
    int failures = 0;
    std::printf("==== WriteFFmpeg, render, then interlaced input: beginSequenceRender fails, endSequenceRender does not crash (issue #289) ====\n");
    failures += testWriteFFmpegBeginFailure(outFile + ".i289-interlaced.mov", 128, 128, WriterParams(),
                                            [](WriterSession& s, bool fail) { s.setSourceFieldOrder(fail ? kOfxImageFieldLower : kOfxImageFieldNone); },
                                            "only progressive video is supported");
    std::printf("==== WriteFFmpeg DNxHD 720p, render in 220x at 60 fps, then 36 (1080p only) at 25 fps: beginSequenceRender fails, endSequenceRender does not crash (issue #289) ====\n");
    WriterParams dnxhd;
    dnxhd.strings = { { "codecShortName", "dnxhd" } };
    // DNxHD 220x (CID 1250 in 720p), then DNxHD 36 (CID 1253 is 1920x1080 only, #305)
    failures += testWriteFFmpegBeginFailure(outFile + ".i289-dnxhd.mov", 1280, 720, dnxhd,
                                            [](WriterSession& s, bool fail) {
                                                s.setInt("DNxHDCodecProfile", fail ? 9 : 6);
                                                s.setFps(fail ? 25. : 60.);
                                            },
                                            "is only available in 1920x1080");
    // the failure is in avcodec_open2(), after the format context and the stream were allocated, which the cleanup of
    // beginSequenceRender frees
    std::printf("==== WriteFFmpeg DNxHD 720p, render in 220x at 60 fps, then with the Codec Options nitris_compat=2 (out of range): beginSequenceRender fails in avcodec_open2(), endSequenceRender does not crash (issue #289) ====\n");
    failures += testWriteFFmpegBeginFailure(outFile + ".i289-dnxhd.mov", 1280, 720, dnxhd,
                                            [](WriterSession& s, bool fail) {
                                                s.setInt("DNxHDCodecProfile", 6);
                                                s.setFps(60.);
                                                s.setString("codecOptions", fail ? "nitris_compat=2" : "");
                                            },
                                            "Could not open video codec");
    return failures;
}

// The compression ID (CID) of the first video packet of a DNxHD/DNxHR movie: the 4 bytes at offset 0x28 of its frame
// header (SMPTE ST 2019-1:2016, Table 8 and section 7.2.4), or -1.
int dnxhdCid(const std::string& file)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, file.c_str(), nullptr, nullptr) < 0) return -1;
    int cid = -1;
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    AVPacket* pkt = av_packet_alloc();
    while ((si >= 0) && pkt && (av_read_frame(fmt, pkt) >= 0)) {
        const bool found = (pkt->stream_index == si);
        if (found && (pkt->size >= 0x2c)) {
            const uint8_t* d = pkt->data + 0x28;
            cid = (int)(((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3]);
        }
        av_packet_unref(pkt);
        if (found) break;
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return cid;
}

// #305: each DNxHD Codec Profile of WriteFFmpeg, in 1920x1080 and 1280x720 at the usual frame rates: the movie is
// encoded with the CID of the profile (SMPTE ST 2019-1:2016, Tables C.1 and C.2), or, where the profile does not exist
// (DNxHD 440x and 36 are 1080p only), beginSequenceRender fails with a message that says so. FFmpeg's dnxhd encoder
// only uses the bitrate that WriteFFmpeg sets to choose the CID (ff_dnxhd_find_cid() in libavcodec/dnxhddata.c), and
// avcodec_open2() fails if no CID of the size and bit depth has that bitrate (DNxHD 36 in 720p, before #305).
struct DNxHDProfileCase {
    const char* name;
    int profile; // the index of the option of DNxHDCodecProfile
    int cid1080; // the expected CID in 1920x1080
    int cid720;  // the expected CID in 1280x720, 0 if beginSequenceRender must fail
};
const DNxHDProfileCase kDNxHDProfileCases[] = {
    { "DNxHR 444", 0, 1270, 1270 },
    { "DNxHR HQX", 1, 1271, 1271 },
    { "DNxHR HQ", 2, 1272, 1272 },
    { "DNxHR SQ", 3, 1273, 1273 },
    { "DNxHR LB", 4, 1274, 1274 },
    { "DNxHD 440x", 5, 1235, 0 },
    { "DNxHD 220x", 6, 1235, 1250 },
    { "DNxHD 220", 7, 1238, 1251 },
    { "DNxHD 145", 8, 1237, 1252 },
    { "DNxHD 36", 9, 1253, 0 },
};

// DNxHR (CIDs 1270 to 1274) is resolution-independent and does not depend on the frame rate
bool isDNxHR(const DNxHDProfileCase& c)
{
    return c.cid1080 >= 1270;
}

int testDNxHDProfiles(const std::string& outFile)
{
    const double fpsList[] = { 23.976, 24., 25., 29.97, 30., 50., 59.94, 60. };
    const char* const rejected = "is only available in 1920x1080";
    const std::string file = outFile + ".i305.mov";
    const int N = 1;
    int failures = 0;
    int nCases = 0;
    for (const DNxHDProfileCase& c : kDNxHDProfileCases) {
        for (const bool is720p : { false, true }) {
            const int W = is720p ? 1280 : 1920;
            const int H = is720p ? 720 : 1080;
            const int expectedCid = is720p ? c.cid720 : c.cid1080;
            for (const double fps : fpsList) {
                if (isDNxHR(c) && (fps != fpsList[0])) continue;
                ++nCases;
                std::remove(file.c_str());
                WriterParams wp;
                wp.strings = { { "codecShortName", "dnxhd" } };
                wp.ints = { { "DNxHDCodecProfile", c.profile } };
                wp.fps = fps;
                WriterSession s;
                if (s.create(file, W, H, N, wp)) {
                    std::printf("  FAIL: %s %dx%d at %g fps: the instance\n", c.name, W, H, fps);
                    ++failures;
                    continue;
                }
                if (expectedCid == 0) {
                    const OfxStatus st = s.begin(N);
                    const std::string error = WriterSession::persistentError();
                    s.end(N);
                    if (ok(st) || (error.find(rejected) == std::string::npos)) {
                        std::printf("  FAIL: %s %dx%d at %g fps: beginSequenceRender = %d, expected a failure with \"%s\" (error: \"%s\")\n",
                                    c.name, W, H, fps, st, rejected, error.c_str());
                        ++failures;
                    } else {
                        std::printf("  %s %dx%d at %g fps: rejected (\"%s\")\n", c.name, W, H, fps, error.c_str());
                    }
                    continue;
                }
                if (s.renderAll(N)) {
                    std::printf("  FAIL: %s %dx%d at %g fps: the encode failed (expected CID %d)\n", c.name, W, H, fps, expectedCid);
                    ++failures;
                    continue;
                }
                const int cid = dnxhdCid(file);
                if (cid != expectedCid) {
                    std::printf("  FAIL: %s %dx%d at %g fps: CID %d, expected %d\n", c.name, W, H, fps, cid, expectedCid);
                    ++failures;
                } else {
                    std::printf("  %s %dx%d at %g fps: CID %d\n", c.name, W, H, fps, cid);
                }
            }
        }
    }
    std::remove(file.c_str());
    std::printf("  %d cases, %d failures\n", nCases, failures);
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// #248: FFmpeg's dnxhd encoder refuses images smaller than 256x120 (libavcodec/dnxhdenc.c:406-410 at n9.1), which
// WriteFFmpeg sends unchanged with the DNxHR profiles: beginSequenceRender must fail with a message that gives the
// limit and the image size (it was "Could not open video codec: Invalid argument"), and 256x120 and 257x121 (there is
// no alignment constraint) must encode with the CID of the profile.
int testDNxHRMinSize(const std::string& outFile)
{
    struct Size {
        int W;
        int H;
        bool encodes;
    };
    const Size sizes[] = { { 128, 96, false }, { 255, 120, false }, { 256, 119, false }, { 256, 120, true }, { 257, 121, true } };
    const char* const rejected = "at least 256x120";
    const std::string file = outFile + ".i248.mov";
    const int N = 1;
    int failures = 0;
    for (const DNxHDProfileCase& c : kDNxHDProfileCases) {
        if (!isDNxHR(c)) continue;
        for (const Size& sz : sizes) {
            std::remove(file.c_str());
            WriterParams wp;
            wp.strings = { { "codecShortName", "dnxhd" } };
            wp.ints = { { "DNxHDCodecProfile", c.profile } };
            WriterSession s;
            if (s.create(file, sz.W, sz.H, N, wp)) {
                std::printf("  FAIL: %s %dx%d: the instance\n", c.name, sz.W, sz.H);
                ++failures;
                continue;
            }
            if (!sz.encodes) {
                const OfxStatus st = s.begin(N);
                const std::string error = WriterSession::persistentError();
                s.end(N);
                const std::string size = std::to_string(sz.W) + "x" + std::to_string(sz.H);
                if (ok(st) || (error.find(rejected) == std::string::npos) || (error.find(size) == std::string::npos)) {
                    std::printf("  FAIL: %s %s: beginSequenceRender = %d, expected a failure with \"%s\" and \"%s\" (error: \"%s\")\n",
                                c.name, size.c_str(), st, rejected, size.c_str(), error.c_str());
                    ++failures;
                } else {
                    std::printf("  %s %s: rejected (\"%s\")\n", c.name, size.c_str(), error.c_str());
                }
                continue;
            }
            const int cid = s.renderAll(N) ? -1 : dnxhdCid(file);
            if (cid != c.cid1080) {
                std::printf("  FAIL: %s %dx%d: CID %d, expected %d\n", c.name, sz.W, sz.H, cid, c.cid1080);
                ++failures;
            } else {
                std::printf("  %s %dx%d: CID %d\n", c.name, sz.W, sz.H, cid);
            }
        }
    }
    std::remove(file.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// #288: frame 1 is never encoded while other render threads wait in encode() for their turn: they must be woken up
// and fail, instead of waiting forever (which deadlocked Natron, which waits for its render threads before
// endSequenceRender). Frames 2..4 are rendered on other threads, which wait for frame 1; then inject() makes frame 1
// fail (or not reach encode()), afterFrame1() runs once frame 1 returned, and afterWaiting() once the other threads
// returned (or the test exits after 60 s). The next render of the same instance must then work.
int testWriteFFmpegEncodeFailure(const std::string& outFile, const char* label, const std::function<void(WriterSession&)>& inject,
                                 const std::function<void(WriterSession&)>& afterFrame1, const std::function<void(WriterSession&)>& afterWaiting,
                                 bool frame1Fails, const WriterParams& wp = WriterParams())
{
    std::printf("==== WriteFFmpeg, %s while other threads wait for their turn: they fail, no deadlock (issue #288) ====\n", label);
    const int N = 4;
    const int timeoutSeconds = 60;
    std::remove(outFile.c_str());
    int failures = 0;
    {
        WriterSession s;
        if (s.create(outFile, 128, 128, N, wp)) return 1;
        OfxStatus st = s.begin(N);
        if (!ok(st)) { std::printf("  FAIL: beginSequenceRender = %d\n", st); return 1; }

        // Frames 2..N wait in encode() for frame 1.
        std::vector<std::future<OfxStatus>> waiting;
        for (int t = 2; t <= N; ++t) {
            waiting.push_back(std::async(std::launch::async, [&s, t]() { return s.render(t); }));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        inject(s);
        const OfxStatus st1 = s.render(1);
        afterFrame1(s);
        std::printf("  render frame 1 = %d (error: \"%s\")\n", st1, WriterSession::persistentError().c_str());
        if (frame1Fails && ok(st1)) { std::printf("  FAIL: frame 1 was rendered: the failure injection did not work\n"); ++failures; }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
        for (size_t i = 0; i < waiting.size(); ++i) {
            if (waiting[i].wait_until(deadline) != std::future_status::ready) {
                // the waiting threads can not be joined: exit now
                std::printf("  FAIL: the render of frame %d still waits after %d s: deadlock (issue #288)\n", (int)i + 2, timeoutSeconds);
                std::printf("FAIL\n");
                std::_Exit(1);
            }
            const OfxStatus sti = waiting[i].get();
            std::printf("  render frame %d = %d\n", (int)i + 2, sti);
            if (ok(sti)) { std::printf("  FAIL: frame %d was rendered after frame 1 failed\n", (int)i + 2); ++failures; }
        }
        afterWaiting(s);
        st = s.end(N);
        std::printf("  endSequenceRender = %d\n", st);

        // the next render of the same instance works
        int nFrames = 0, nIFrames = 0;
        if (s.renderAll(N) || probeVideoFrames(outFile, &nFrames, &nIFrames) || (nFrames != N)) {
            std::printf("  FAIL: the render after the failed one (%d frames written)\n", nFrames);
            ++failures;
        }
    }
    std::remove(outFile.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// #288, on every system: frame 1 is aborted (GenericWriterPlugin::render() returns before encode(), as when the user
// cancels a render in Natron: abort() stays true for all the render threads), or frame 1 fails one of the checks of
// encode() (here, it is rendered to another file).
int testWriteFFmpegEncodeFailures(const std::string& outFile)
{
    auto nothing = [](WriterSession&) {};
    int failures = 0;
    failures += testWriteFFmpegEncodeFailure(outFile, "frame 1 is aborted", [](WriterSession&) { gAbort = 1; }, nothing,
                                             [](WriterSession&) { gAbort = 0; }, false);
    failures += testWriteFFmpegEncodeFailure(outFile, "frame 1 is rendered to another file",
                                             [&outFile](WriterSession& s) { s.setFilename(outFile + ".other.mov"); },
                                             [&outFile](WriterSession& s) { s.setFilename(outFile); }, nothing, true);
    std::remove((outFile + ".other.mov").c_str());
    return failures;
}

// #288, POSIX only: frame 1 fails to be written because the file can not grow, as on a full disk. The failure is
// injected with RLIMIT_FSIZE, so that the real write error path (avio, the muxer, writeToFile()) fails, without a test
// hook in the plug-in; flush_packets=1 makes the muxer write each packet to the file when it is muxed. Exits with 77
// (skipped) on MS Windows, which has no RLIMIT_FSIZE.
int testWriteFFmpegEncodeFailureDiskFull(const std::string& outFile)
{
#ifdef _WIN32
    (void)outFile;
    std::printf("SKIP: no RLIMIT_FSIZE on MS Windows\n");
    std::exit(77);
#else
    // a write beyond RLIMIT_FSIZE fails with EFBIG, instead of killing the process with SIGXFSZ
    std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit oldLimit;
    ::getrlimit(RLIMIT_FSIZE, &oldLimit);
    WriterParams wp;
    wp.strings = { { "formatOptions", "flush_packets=1" } };
    return testWriteFFmpegEncodeFailure(outFile, "frame 1 fails to be written (the disk is full)",
                                        [&outFile, &oldLimit](WriterSession&) {
                                            struct stat sb;
                                            if (::stat(outFile.c_str(), &sb) != 0) { std::printf("  FAIL: no file %s\n", outFile.c_str()); std::_Exit(1); }
                                            struct rlimit limit = oldLimit;
                                            limit.rlim_cur = (rlim_t)sb.st_size; // the file can not grow any more
                                            ::setrlimit(RLIMIT_FSIZE, &limit);
                                        },
                                        [&oldLimit](WriterSession&) { ::setrlimit(RLIMIT_FSIZE, &oldLimit); },
                                        [](WriterSession&) {}, true, wp);
#endif
}

#ifndef _WIN32
// For a test, limits the size of the files that the process writes (RLIMIT_FSIZE), as on a full disk, and ignores
// SIGXFSZ, so that a write beyond the limit fails with EFBIG instead of killing the process. The destructor restores
// both and removes the file, on every path (C++ Core Guidelines R.1, C.21).
class FileSizeLimit {
public:
    explicit FileSizeLimit(std::string file)
        : _file(std::move(file))
        , _oldHandler(std::signal(SIGXFSZ, SIG_IGN))
    {
        _ok = (::getrlimit(RLIMIT_FSIZE, &_old) == 0);
        if (!_ok) std::printf("  FAIL: getrlimit: %s\n", std::strerror(errno));
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;
    ~FileSizeLimit()
    {
        (void)restore();
        if (_oldHandler != SIG_ERR) std::signal(SIGXFSZ, _oldHandler);
        std::remove(_file.c_str());
    }

    // true if the files can not grow beyond maxSize bytes
    bool limit(rlim_t maxSize)
    {
        std::fflush(nullptr); // write the buffered output (e.g. stdout redirected to a file) while it still can grow
        return setCurrent(maxSize);
    }
    // true if the limit is the original one
    bool restore()
    {
        const bool restored = setCurrent(_old.rlim_cur);
        // clear the error state that a write beyond the limit left on stdout and stderr
        std::clearerr(stdout);
        std::clearerr(stderr);
        return restored;
    }

private:
    bool setCurrent(rlim_t cur)
    {
        if (!_ok) return false;
        struct rlimit l = _old;
        l.rlim_cur = cur;
        if (::setrlimit(RLIMIT_FSIZE, &l) != 0) {
            std::printf("  FAIL: setrlimit: %s\n", std::strerror(errno));
            return false;
        }
        return true;
    }

    std::string _file;
    void (*_oldHandler)(int);
    struct rlimit _old = {};
    bool _ok = false;
};
#endif

#ifndef _WIN32
// #362: the file that WriteOIIO writes with the file size limit, and the limit to restore, for onExitDuringRender()
static const char* gFileWithLimit = nullptr;
static struct rlimit gNoFileSizeLimit = {};

// atexit() handler: on a write error, OpenImageIO 3.1's JPEG writer, when it writes through a "file" proxy, exits the
// process (libjpeg's default error_exit()). exit() does not run the destructor of FileSizeLimit, so the limit is
// restored here (or this message could not be written to a log larger than the limit), and the file is removed.
static void onExitDuringRender()
{
    if (!gFileWithLimit) {
        return;
    }
    (void)::setrlimit(RLIMIT_FSIZE, &gNoFileSizeLimit);
    std::clearerr(stdout);
    std::printf("  FAIL: the process exited while WriteOIIO wrote %s to a full disk\n", gFileWithLimit);
    std::fflush(stdout);
    std::remove(gFileWithLimit);
    std::_Exit(3);
}
#endif

// Reads file with OpenImageIO: true if it has one subimage per element of nChannels, of W x H pixels and with that
// number of channels, which can all be read. If views is not empty, these are the views of the file: the "view" of
// each subimage (a multi-part EXR), or the "multiView" of the only subimage (a single-part EXR).
static bool readBackOIIO(const std::string& file, int W, int H, const std::vector<int>& nChannels,
                         const std::vector<std::string>& views = {})
{
    auto in = OIIO::ImageInput::open(file);
    if (!in) { std::printf("  FAIL: cannot read back %s: %s\n", file.c_str(), OIIO::geterror().c_str()); return false; }
    int n = 0;
    bool good = true;
    std::vector<std::string> fileViews;
    for (; in->seek_subimage(n, 0); ++n) {
        const OIIO::ImageSpec& spec = in->spec();
        std::vector<unsigned char> buf((size_t)spec.image_bytes(true));
        const bool read = in->read_image(n, 0, 0, spec.nchannels, OIIO::TypeDesc::UINT8, buf.data());
        const std::string view = spec.get_string_attribute("view");
        std::printf("  read back subimage %d: %dx%d, %d channels, view \"%s\", %s\n", n, spec.width, spec.height, spec.nchannels, view.c_str(), read ? "ok" : in->geterror().c_str());
        good = good && read && (spec.width == W) && (spec.height == H) && (n < (int)nChannels.size()) && (spec.nchannels == nChannels[n]);
        if (const OIIO::ParamValue* mv = spec.find_attribute("multiView")) {
            for (int i = 0; i < mv->nvalues() * (int)mv->type().basevalues(); ++i) fileViews.push_back(mv->get_string_indexed(i));
        } else if (!view.empty()) {
            fileViews.push_back(view);
        }
    }
    (void)in->geterror(); // seek_subimage() past the last one sets an error
    if (n != (int)nChannels.size()) { std::printf("  FAIL: %d subimages instead of %d\n", n, (int)nChannels.size()); return false; }
    if (!good) { std::printf("  FAIL: %s can not be read back, or its size or channels are wrong\n", file.c_str()); return false; }
    if (!views.empty() && (fileViews != views)) { std::printf("  FAIL: the views of the file are not those of the project\n"); return false; }
    return true;
}

// #292, #362, POSIX only: WriteOIIO, the file can not grow beyond a few KB (RLIMIT_FSIZE, as on a full disk), so that
// a write fails, after open() succeeded: the render must fail with an error that names the file, and the process
// must survive. OpenImageIO 3.1 does not report the PNG write errors, and exits the process on JPEG write errors (when
// its JPEG writer writes through a "file" proxy). Each format is then written without the limit, and read back, to
// check that the failure came from the limit. GenericWriter removes the partial file (#251). Exits with 77 (skipped)
// on MS Windows, which has no RLIMIT_FSIZE.
int testWriteOIIODiskFull(const std::string& outFile)
{
#ifdef _WIN32
    (void)outFile;
    std::printf("SKIP: no RLIMIT_FSIZE on MS Windows\n");
    std::exit(77);
#else
    const int W = 512, H = 512;
    const rlim_t kMaxFileSize = 16384; // more than any header, much less than any of these files
    int failures = 0;
    if (::getrlimit(RLIMIT_FSIZE, &gNoFileSizeLimit) != 0) { std::printf("  FAIL: getrlimit: %s\n", std::strerror(errno)); return 1; }
    std::atexit(onExitDuringRender);
    // a Natron-like host (this test runs by itself), for the Parts and Views params of WriteOIIO: dynamic choices and
    // multi-planar
    pSetInt(PH(&gHostProps), kNatronOfxParamHostPropSupportsDynamicChoices, 0, 1);
    pSetInt(PH(&gHostProps), kFnOfxImageEffectPropMultiPlanar, 0, 1);
    struct Case {
        const char* label;
        const char* ext;
        int nViews;                // the views of the project (all written)
        std::vector<int> nChannels; // the channels of each subimage of the file
    };
    const Case cases[] = {
        { "exr", "exr", 1, { 4 } },
        // multi-part, through the proxy: OpenEXR writes the offset tables when the file is closed, after seeking back
        { "2-view EXR, Parts Split Views", "exr", 2, { 4, 4 } },
        { "tif", "tif", 1, { 4 } },
        { "dpx", "dpx", 1, { 4 } },
        { "png", "png", 1, { 4 } },
        { "jpg", "jpg", 1, { 3 } }, // JPEG has no alpha
    };
    for (const Case& c : cases) {
        const char* ext = c.ext;
        const std::string file = outFile + "." + ext;
        std::printf("==== WriteOIIO %s, the disk is full after %d bytes (issues #292, #362) ====\n", c.label, (int)kMaxFileSize);
        std::remove(file.c_str());
        FileSizeLimit fileSizeLimit(file);
        gViewCount = c.nViews;
        WriterParams wp;
        if (c.nViews > 1) wp.ints.emplace_back("partSplitting", 1); // Split Views
        WriterSession s;
        if (s.create(file, W, H, 1, wp, "fr.inria.openfx.WriteOIIO")) { ++failures; gViewCount = 1; continue; }
        // noise, which no codec compresses into a few KB
        std::mt19937 rng(292);
        std::uniform_real_distribution<float> dist(0.f, 1.f);
        float* pix = s.sourcePixels();
        for (size_t i = 0; i < (size_t)W * H * 4; ++i) pix[i] = dist(rng);

        OfxStatus st = s.begin(1);
        if (!ok(st)) { std::printf("  FAIL: beginSequenceRender = %d\n", st); ++failures; continue; }
        if (!fileSizeLimit.limit(kMaxFileSize)) { ++failures; continue; }
        gFileWithLimit = file.c_str();
        st = s.render(1);
        gFileWithLimit = nullptr;
        if (!fileSizeLimit.restore()) { ++failures; continue; }
        const std::string error = WriterSession::persistentError();
        struct stat sb;
        const long size = (::stat(file.c_str(), &sb) == 0) ? (long)sb.st_size : -1L;
        std::printf("  render = %d (error: \"%s\"), file size %ld\n", st, error.c_str(), size);
        if (ok(st)) { std::printf("  FAIL: the render succeeded, the file is truncated\n"); ++failures; }
        else if (error.find(file) == std::string::npos) { std::printf("  FAIL: the error does not name the file\n"); ++failures; }
        s.end(1);

        // without the limit, the same instance writes the file
        if (s.renderAll(1)) { std::printf("  FAIL: the render without the limit\n"); ++failures; }
        else if (!readBackOIIO(file, W, H, c.nChannels, (c.nViews > 1) ? std::vector<std::string> { "main", "right" } : std::vector<std::string> {})) { ++failures; }
        gViewCount = 1;
    }
    if (!failures) std::printf("  PASS\n");
    return failures;
#endif
}

// true if OpenImageIO was built with OpenJPH (its "build:dependencies" attribute, e.g. "..., openjph 0.33.0, ...")
static bool hasOpenJPH()
{
    std::string libs = OIIO::get_string_attribute("build:dependencies");
    std::transform(libs.begin(), libs.end(), libs.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return libs.find("openjph") != std::string::npos;
}

// #362: WriteOIIO, files that OpenImageIO writes by name although the plug-in gives it a proxy: a 2-view EXR in
// several parts (OpenImageIO 3.1's multi-part EXR writer ignores the oiio:ioproxy attribute), and a JPEG 2000 file
// with the htj2k256 compression (OpenJPH, which ignores the proxy). Each file must be written and read back, with its
// views.
int testWriteOIIOFiles(const std::string& outFile)
{
    const int W = 64, H = 48;
    int failures = 0;
    int skipped = 0;
    // a Natron-like host (this test runs by itself): WriteOIIO has the Parts and Views params if the host supports
    // dynamic choices and is multi-planar
    pSetInt(PH(&gHostProps), kNatronOfxParamHostPropSupportsDynamicChoices, 0, 1);
    pSetInt(PH(&gHostProps), kFnOfxImageEffectPropMultiPlanar, 0, 1);
    struct Case {
        const char* label;
        const char* ext;
        const char* compression; // the label of the Compression option (nullptr: the default)
        int parts;       // the index of the Parts option (-1: the default)
        int nViews;      // the views of the project
        std::vector<int> nChannels; // the channels of each subimage of the file
    };
    // Parts: 0 "Single Part", 1 "Split Views", 2 "Split Views,Layers" (the default); the views: "All" (the default)
    const Case cases[] = {
        { "EXR, 2 views, Split Views,Layers (multi-part)", "exr", nullptr, -1, 2, { 4, 4 } },
        { "EXR, 2 views, Split Views (multi-part)", "exr", nullptr, 1, 2, { 4, 4 } },
        { "EXR, 2 views, Single Part", "exr", nullptr, 0, 2, { 8 } }, // RGBA and right.RGBA
        // OpenImageIO's JPEG 2000 writer uses OpenJPH for a "htj2k..." compression (jpeg2000output.cpp:224-226)
        { "JPEG 2000, compression htj2k256 (OpenJPH)", "jp2", "htj2k256", -1, 1, { 4 } },
    };
    for (const Case& c : cases) {
        const std::string file = outFile + "." + c.ext;
        std::printf("==== WriteOIIO %s (issue #362) ====\n", c.label);
        if (c.compression && !std::strncmp(c.compression, "htj2k", 5) && !hasOpenJPH()) {
            std::printf("  SKIP: OpenImageIO was built without OpenJPH\n");
            ++skipped;
            continue;
        }
        std::remove(file.c_str());
        WriterParams wp;
        if (c.parts >= 0) wp.ints.emplace_back("partSplitting", c.parts);
        gViewCount = c.nViews;
        WriterSession s;
        if (s.create(file, W, H, 1, wp, "fr.inria.openfx.WriteOIIO") || (c.compression && !s.setChoice("compression", c.compression))) { ++failures; }
        else if (s.renderAll(1)) { std::printf("  FAIL: the render: \"%s\"\n", WriterSession::persistentError().c_str()); ++failures; }
        else if (!readBackOIIO(file, W, H, c.nChannels, (c.nViews > 1) ? std::vector<std::string> { "main", "right" } : std::vector<std::string> {})) { ++failures; }
        gViewCount = 1;
        std::remove(file.c_str());
    }
    if (!failures && skipped) std::printf("  PASS (%d case skipped)\n", skipped);
    else if (!failures) std::printf("  PASS\n");
    return failures;
}

// #171: WriteOIIO, an EXR file with the zstd compression. OpenImageIO writes zip instead, without an error, if it or
// its OpenEXR does not support zstd: the render must then end with a persistent warning that names zstd, and without
// it if the file is zstd. The compression of the file, read back, tells which case it is, with any OpenImageIO. The
// next sequence render of the same instance, with the zip compression, must end without the warning.
int testWriteOIIOEXRZstd(const std::string& outFile)
{
    const int W = 64, H = 48;
    const std::string file = outFile + ".i171.exr";
    std::printf("==== WriteOIIO EXR, compression zstd: a warning if the file is not zstd (issue #171) ====\n");
    const int ver = OIIO::openimageio_version();
    std::printf("  OpenImageIO %d.%d.%d, libraries: %s\n", ver / 10000, (ver / 100) % 100, ver % 100,
                OIIO::get_string_attribute("library_list").c_str());
    std::remove(file.c_str());
    {
        std::lock_guard<std::mutex> lock(gMessagesMutex);
        gPersistentWarning.clear();
    }
    WriterSession s;
    if (s.create(file, W, H, 1, WriterParams(), "fr.inria.openfx.WriteOIIO") || !s.setChoice("compression", "zstd")) return 1;
    if (s.renderAll(1)) return 1;
    std::string warning;
    {
        std::lock_guard<std::mutex> lock(gMessagesMutex);
        warning = gPersistentWarning;
    }
    std::string compression;
    {
        auto in = OIIO::ImageInput::open(file);
        if (!in) { std::printf("  FAIL: cannot read back %s: %s\n", file.c_str(), OIIO::geterror().c_str()); return 1; }
        compression = in->spec().get_string_attribute("compression");
    }
    std::remove(file.c_str());
    const bool isZstd = !compression.compare(0, 4, "zstd");
    std::printf("  file compression \"%s\", persistent warning \"%s\"\n", compression.c_str(), warning.c_str());
    if (!isZstd && (warning.find("zstd") == std::string::npos)) {
        std::printf("  FAIL: the file is not zstd, and there is no warning\n");
        return 1;
    }
    if (isZstd && !warning.empty()) {
        std::printf("  FAIL: the file is zstd, but there is a warning\n");
        return 1;
    }

    // the next sequence render of the same instance, with the zip compression: no warning
    {
        std::lock_guard<std::mutex> lock(gMessagesMutex);
        gPersistentWarning.clear();
    }
    if (!s.setChoice("compression", "zip") || s.renderAll(1)) return 1;
    std::remove(file.c_str());
    {
        std::lock_guard<std::mutex> lock(gMessagesMutex);
        warning = gPersistentWarning;
    }
    std::printf("  then compression zip: persistent warning \"%s\"\n", warning.c_str());
    if (!warning.empty()) {
        std::printf("  FAIL: the zip render of the same instance has a warning\n");
        return 1;
    }
    std::printf("  PASS\n");
    return 0;
}

// ---- #294, #296: the PFM plug-ins fail on write errors and on invalid headers (options --pfm-write-errors, --pfm-read-errors) ----

// #294, POSIX only: WritePFM, the file can not grow beyond 16 KB (RLIMIT_FSIZE, as on a full disk), so that fwrite()
// or fclose() fails: the render must fail with an error that names the file. The same instance then writes the whole
// file without the limit, to check that the failure came from the limit. Exits with 77 (skipped) on MS Windows, which
// has no RLIMIT_FSIZE.
int testWritePFMDiskFull(const std::string& outFile)
{
#ifdef _WIN32
    (void)outFile;
    std::printf("SKIP: no RLIMIT_FSIZE on MS Windows\n");
    std::exit(77);
#else
    const int W = 512, H = 512;
    const rlim_t kMaxFileSize = 16384; // more than the header, much less than the file (3 MB)
    const std::string file = outFile + ".i294.pfm";
    std::printf("==== WritePFM, the disk is full after %d bytes (issue #294) ====\n", (int)kMaxFileSize);
    std::remove(file.c_str());
    int failures = 0;
    FileSizeLimit fileSizeLimit(file);
    WriterSession s;
    if (s.create(file, W, H, 1, WriterParams(), "fr.inria.openfx.WritePFM")) return 1;
    OfxStatus st = s.begin(1);
    if (!ok(st)) { std::printf("  FAIL: beginSequenceRender = %d\n", st); return 1; }
    if (!fileSizeLimit.limit(kMaxFileSize)) return 1;
    st = s.render(1);
    if (!fileSizeLimit.restore()) return 1;
    const std::string error = WriterSession::persistentError();
    struct stat sb;
    const long size = (::stat(file.c_str(), &sb) == 0) ? (long)sb.st_size : -1L;
    std::printf("  render = %d (error: \"%s\"), file size %ld\n", st, error.c_str(), size);
    if (ok(st)) { std::printf("  FAIL: the render succeeded, the file is truncated\n"); ++failures; }
    else if (error.find(file) == std::string::npos) { std::printf("  FAIL: the error does not name the file\n"); ++failures; }
    s.end(1);

    // without the limit, the same instance writes the whole file: the header, then W x H RGB floats
    if (s.renderAll(1)) { std::printf("  FAIL: the render without the limit\n"); ++failures; }
    else if ((::stat(file.c_str(), &sb) != 0) || (sb.st_size <= (off_t)W * H * 3 * 4)) {
        std::printf("  FAIL: the file without the limit is not larger than %d bytes\n", W * H * 3 * 4);
        ++failures;
    }

    // With "Overwrite" unchecked, an existing file that the user can not read (mode 000, in a writable directory)
    // exists: the render must leave it as it is. GenericWriter took a file that fopen(..., "rb") can not open for
    // absent, and WritePFM, as the other writers, removes an existing file (OFX::exists_utf8()) before writing it.
    std::printf("==== WritePFM, \"Overwrite\" unchecked, the file exists but can not be read ====\n");
    if (::geteuid() == 0) {
        std::printf("  SKIP: running as root, which can read a file with mode 000\n");
    } else if (::chmod(file.c_str(), 0) != 0) {
        std::printf("  FAIL: chmod: %s\n", std::strerror(errno));
        ++failures;
    } else {
        struct stat before;
        const bool statted = (::stat(file.c_str(), &before) == 0);
        s.setInt("overwrite", 0);
        const int renderFailed = s.renderAll(1);
        s.setInt("overwrite", 1);
        struct stat after;
        const bool exists = (::stat(file.c_str(), &after) == 0);
        std::printf("  render %s, file %s\n", renderFailed ? "failed" : "succeeded", exists ? "exists" : "removed");
        if (!statted || !exists) {
            std::printf("  FAIL: the file was removed\n");
            ++failures;
        } else if ((after.st_ino != before.st_ino) || (after.st_size != before.st_size) ||
                   ((after.st_mode & 07777) != 0) || (after.st_mtime != before.st_mtime)) {
            std::printf("  FAIL: the file was replaced or changed\n");
            ++failures;
        }
        (void)::chmod(file.c_str(), 0644);
    }
    std::remove(file.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
#endif
}

// #296: ReadPFM, a header with an empty, negative or huge size: getRegionOfDefinition must fail (as decode() does),
// instead of giving the host that RoD. A valid header gives the RoD of the image.
int testReadPFMHeaderSize(const std::string& outFile)
{
    std::printf("==== ReadPFM, a header with an empty, negative or huge size: the RoD fails (issue #296) ====\n");
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadPFM");
    if (!p) { std::printf("  FAIL: ReadPFM not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    struct Header {
        const char* size; // "W H" in the header
        bool valid;
    };
    const Header headers[] = { { "3 2", true }, { "-5 3", false }, { "3 -5", false }, { "0 2", false }, { "3 0", false },
                               { "65536 2", false }, { "3 65536", false }, { "2000000000 2000000000", false } };
    const std::string file = outFile + ".i296.pfm";
    int failures = 0;
    for (const Header& h : headers) {
        {
            // a little-endian RGB file, with the pixels of a 3x2 image (the invalid sizes fail before the pixels)
            std::FILE* f = std::fopen(file.c_str(), "wb");
            const float pixels[3 * 2 * 3] = {};
            const bool written = f && (std::fprintf(f, "PF\n%s\n-1.0\n", h.size) > 0) && (std::fwrite(pixels, sizeof(pixels), 1, f) == 1);
            if (!f || (std::fclose(f) != 0) || !written) { std::printf("  FAIL: cannot write %s\n", file.c_str()); ++failures; continue; }
        }
        // (cleared, so that an error can only come from this header)
        {
            std::lock_guard<std::mutex> lock(gMessagesMutex);
            gPersistentError.clear();
        }
        setOverrideString(&ctx, kOfxImageEffectFileParamName, file);
        Effect inst;
        inst.delegate = &ctx;
        setInstanceProps(inst, 3, 2, 1, kOfxImageEffectContextReader);
        const OfxImageEffectHandle handle = reinterpret_cast<OfxImageEffectHandle>(&inst);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, handle, nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); ++failures; continue; }
        {
            PropSet a; OfxPropertySetHandle ap = PH(&a);
            pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
            pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
            pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
            pSetDouble(ap, kOfxPropTime, 0, 1.);
            double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
            p->mainEntry(kOfxActionInstanceChanged, handle, PH(&a), nullptr);
        }
        PropSet a; pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
        double rs[2] = { 1., 1. }; pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
        pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
        PropSet o;
        const OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, handle, PH(&a), PH(&o));
        double rod[4] = { -1, -1, -1, -1 }; pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, rod);
        const std::string error = WriterSession::persistentError();
        std::printf("  \"%s\": getRegionOfDefinition = %d, [%.0f %.0f %.0f %.0f] (error: \"%s\")\n", h.size, st, rod[0], rod[1], rod[2], rod[3], error.c_str());
        if (h.valid && (!ok(st) || (rod[0] != 0) || (rod[1] != 0) || (rod[2] != 3) || (rod[3] != 2))) {
            std::printf("  FAIL: the RoD must be [0 0 3 2]\n");
            ++failures;
        } else if (!h.valid && (st != kOfxStatFailed)) {
            std::printf("  FAIL: getRegionOfDefinition must fail\n");
            ++failures;
        } else if (!h.valid && (error.find(file) == std::string::npos)) {
            std::printf("  FAIL: the error does not name the file\n");
            ++failures;
        }
        p->mainEntry(kOfxActionDestroyInstance, handle, nullptr, nullptr);
    }
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::remove(file.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// #251: a test writer of image files (".partial"), whose encode() writes kTestWriterFileSize bytes, or writes part of
// the file and then fails or sets the host's abort flag (as a writer that stops when the render is aborted), as
// gTestWriterMode says.
namespace TestWriter251 {
using namespace OFX;
enum class Mode { eWrite, eFail, eAbort };
std::atomic<Mode> gTestWriterMode { Mode::eWrite };
constexpr long kTestWriterFileSize = 1000;

class TestWriterPlugin : public IO::GenericWriterPlugin {
public:
    TestWriterPlugin(OfxImageEffectHandle handle, const std::vector<std::string>& extensions)
        : IO::GenericWriterPlugin(handle, extensions, true, true, false, true) {}

private:
    void encode(const std::string& filename, OfxTime, const std::string&, const float*, const OfxRectI&, float, int, int, int, int) override
    {
        const Mode mode = gTestWriterMode;
        const std::vector<char> data((mode == Mode::eWrite) ? kTestWriterFileSize : 10, 'x');
        std::FILE* f = fopen_utf8(filename.c_str(), "wb");
        const bool written = f && (std::fwrite(data.data(), 1, data.size(), f) == data.size());
        if (!f || (std::fclose(f) != 0) || !written) { setPersistentMessage(Message::eMessageError, "", "cannot write " + filename); throwSuiteStatusException(kOfxStatFailed); }
        if (mode == Mode::eFail) { setPersistentMessage(Message::eMessageError, "", "test failure"); throwSuiteStatusException(kOfxStatFailed); }
        if (mode == Mode::eAbort) gAbort = 1; // the host aborts the render: encode() returns without an error
    }
    bool isImageFile(const std::string&) const override { return true; }
    PreMultiplicationEnum getExpectedInputPremultiplication() const override { return eImageUnPreMultiplied; }
    void onOutputFileChanged(const std::string&, bool) override {}
    bool supportsAlpha(const std::string&) const override { return true; }
};

mDeclareWriterPluginFactory(TestWriterPluginFactory, {}, false);
void TestWriterPluginFactory::load() { _extensions = { "partial" }; }
void TestWriterPluginFactory::describe(ImageEffectDescriptor& desc)
{
    IO::GenericWriterDescribe(desc, eRenderFullySafe, _extensions, -1, false, false);
    desc.setLabel("TestWriter251");
}
void TestWriterPluginFactory::describeInContext(ImageEffectDescriptor& desc, ContextEnum context)
{
    PageParamDescriptor* page = IO::GenericWriterDescribeInContextBegin(desc, context, true, true, false, true, "scene_linear", "scene_linear", false);
    IO::GenericWriterDescribeInContextEnd(desc, context, page);
}
ImageEffect* TestWriterPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum)
{
    TestWriterPlugin* ret = new TestWriterPlugin(handle, _extensions);
    ret->restoreStateFromParams();
    return ret;
}
TestWriterPluginFactory gTestWriterFactory("fr.inria.openfx.tests.TestWriter251", 1, 0);
mRegisterPluginFactoryInstance(gTestWriterFactory)
} // namespace TestWriter251

// #251: a failed or aborted render of an image file must not leave a partial file, which a render with "Overwrite"
// unchecked would take for a written frame (the test then does that render, which must write the whole file):
// - the test writer (on every system, with a non-ASCII file name) writes part of the file, then fails or the render is
//   aborted, with no previous file and over a previous file: no file must be left;
// - POSIX only, with WritePNG (built with libpng): the file can not grow beyond 16 KB (RLIMIT_FSIZE, as on a full
//   disk), with no previous file and over a previous file: the render must fail and leave no file. A render that fails
//   before the file is written (no R/G/B/A checkbox checked) must leave the previous file as it is.
int testWriterRemovePartialFile(const std::string& outFile)
{
    using TestWriter251::Mode;
    int failures = 0;
    // the contents of a file, or nothing if there is none
    auto contents = [](const std::string& file) -> std::optional<std::vector<char>> {
        std::FILE* f = OFX::fopen_utf8(file.c_str(), "rb");
        if (!f) return std::nullopt;
        std::vector<char> data;
        char buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) data.insert(data.end(), buf, buf + n);
        std::fclose(f);
        return data;
    };
    // a render of frame 1 that must fail (or be aborted) and leave no file
    auto expectNoFile = [&](WriterSession& s, const std::string& file, const std::function<OfxStatus()>& render, bool aborted) {
        OfxStatus st = s.begin(1);
        if (!ok(st)) { std::printf("  FAIL: beginSequenceRender = %d\n", st); ++failures; return; }
        st = render();
        s.end(1);
        const auto data = contents(file);
        std::printf("  render = %d (error: \"%s\"), file size %ld\n", st, WriterSession::persistentError().c_str(), data ? (long)data->size() : -1L);
        if (!aborted && ok(st)) { std::printf("  FAIL: the render succeeded\n"); ++failures; }
        else if (data) { std::printf("  FAIL: the render left a partial file\n"); ++failures; }
    };
    // with "Overwrite" unchecked, a render must write the whole file (minSize bytes or more), as after a failure
    auto expectWholeFile = [&](WriterSession& s, const std::string& file, size_t minSize) -> std::optional<std::vector<char>> {
        s.setInt("overwrite", 0);
        const int renderFailed = s.renderAll(1);
        s.setInt("overwrite", 1);
        const auto data = contents(file);
        if (renderFailed || !data || (data->size() < minSize)) {
            std::printf("  FAIL: with \"Overwrite\" unchecked, the file is not written (size %ld)\n", data ? (long)data->size() : -1L);
            ++failures;
            return std::nullopt;
        }
        return data;
    };

    {
        const std::string file = outFile + ".i251-\xc3\xa9t\xc3\xa9.partial"; // "-été": UTF-8 (MS Windows: _wstat64())
        OFX::remove_utf8(file.c_str());
        WriterSession s;
        if (s.create(file, 16, 16, 1, WriterParams(), "fr.inria.openfx.tests.TestWriter251")) return failures + 1;
        for (const Mode mode : { Mode::eFail, Mode::eAbort }) {
            const char* what = (mode == Mode::eFail) ? "fails" : "is aborted";
            for (const bool previous : { false, true }) {
                std::printf("==== test writer, the render %s after writing part of the file, %s (issue #251) ====\n", what, previous ? "over a previous file" : "no previous file");
                TestWriter251::gTestWriterMode = Mode::eWrite;
                OFX::remove_utf8(file.c_str());
                if (previous && s.renderAll(1)) ++failures;
                TestWriter251::gTestWriterMode = mode;
                expectNoFile(s, file, [&s]() { return s.render(1); }, mode == Mode::eAbort);
                gAbort = 0;
                TestWriter251::gTestWriterMode = Mode::eWrite;
                (void)expectWholeFile(s, file, TestWriter251::kTestWriterFileSize);
            }
        }
        OFX::remove_utf8(file.c_str());
    }

#ifdef _WIN32
    std::printf("SKIP: WritePNG on a full disk: no RLIMIT_FSIZE on MS Windows\n");
#else
    if (!findPlugin("fr.inria.openfx.WritePNG")) {
        std::printf("SKIP: WritePNG on a full disk: no WritePNG (libpng)\n");
    } else {
        const int W = 512, H = 512;
        const rlim_t kMaxFileSize = 16384; // more than the header, much less than the file (about 1 MB)
        const std::string file = outFile + ".i251.png";
        std::remove(file.c_str());
        FileSizeLimit fileSizeLimit(file);
        WriterSession s;
        if (s.create(file, W, H, 1, WriterParams(), "fr.inria.openfx.WritePNG")) return failures + 1;
        // noise, which zlib does not compress into a few KB
        std::mt19937 rng(251);
        std::uniform_real_distribution<float> dist(0.f, 1.f);
        float* pix = s.sourcePixels();
        for (size_t i = 0; i < (size_t)W * H * 4; ++i) pix[i] = dist(rng);
        // frame 1 can not be written
        auto renderDiskFull = [&]() -> OfxStatus {
            if (!fileSizeLimit.limit(kMaxFileSize)) return kOfxStatErrFatal;
            const OfxStatus st = s.render(1);
            if (!fileSizeLimit.restore()) return kOfxStatErrFatal;
            return st;
        };

        std::printf("==== WritePNG, the disk is full after %d bytes, no previous file (issue #251) ====\n", (int)kMaxFileSize);
        expectNoFile(s, file, renderDiskFull, false);
        const auto written = expectWholeFile(s, file, (size_t)kMaxFileSize + 1);
        if (!written) return failures;
        std::printf("  then with \"Overwrite\" unchecked: file size %ld\n", (long)written->size());

        std::printf("==== WritePNG, a render that fails before the file is written keeps the previous file (issue #251) ====\n");
        const char* const checkboxes[] = { kNatronOfxParamProcessR, kNatronOfxParamProcessG, kNatronOfxParamProcessB, kNatronOfxParamProcessA };
        for (const char* name : checkboxes) s.setInt(name, 0);
        {
            const OfxStatus st = s.begin(1);
            const OfxStatus rst = s.render(1);
            if (ok(st)) s.end(1);
            const auto data = contents(file);
            std::printf("  render = %d (error: \"%s\"), file size %ld\n", rst, WriterSession::persistentError().c_str(), data ? (long)data->size() : -1L);
            if (ok(rst)) { std::printf("  FAIL: the render succeeded\n"); ++failures; }
            else if (data != written) { std::printf("  FAIL: the previous file was changed\n"); ++failures; }
        }
        for (const char* name : checkboxes) s.setInt(name, 1);

        std::printf("==== WritePNG, the disk is full after %d bytes, over a previous file (issue #251) ====\n", (int)kMaxFileSize);
        expectNoFile(s, file, renderDiskFull, false);
        if (const auto again = expectWholeFile(s, file, (size_t)kMaxFileSize + 1)) {
            std::printf("  then with \"Overwrite\" unchecked: file size %ld\n", (long)again->size());
        }
    }
#endif
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// ---- #207, #220, #316: exact values of the conversions between Y'CbCr and RGB (option --swscale-exact) ----

namespace {

// The BT.601 matrix (ITU-T H.273 §8.3, MatrixCoefficients 6), which ReadFFmpeg (Auto, untagged or SMPTE 170M) and
// WriteFFmpeg use in SD.
constexpr double kKr601 = 0.299, kKb601 = 0.114;

// The exact R', G', B' in [0, 1] (clamped) of a Y'CbCr sample with n bits, in video or full range: ITU-T H.273 §8.3
// (video range: Y' = 219·2^(n-8)·E'Y + 16·2^(n-8), Cb = 224·2^(n-8)·E'PB + 2^(n-1); full range: Y' = (2^n - 1)·E'Y,
// Cb = (2^n - 1)·E'PB + 2^(n-1)).
void exactRgb(int y, int cb, int cr, int bits, bool fullRange, double rgb[3])
{
    const double scale = (double)(1 << (bits - 8)), half = (double)(1 << (bits - 1)), max = (double)((1 << bits) - 1);
    const double ey = fullRange ? y / max : (y - 16. * scale) / (219. * scale);
    const double pb = fullRange ? (cb - half) / max : (cb - half) / (224. * scale);
    const double pr = fullRange ? (cr - half) / max : (cr - half) / (224. * scale);
    const double r = ey + 2. * (1. - kKr601) * pr;
    const double b = ey + 2. * (1. - kKb601) * pb;
    const double g = (ey - kKr601 * r - kKb601 * b) / (1. - kKr601 - kKb601);
    const double v[3] = { r, g, b };
    for (int k = 0; k < 3; ++k) rgb[k] = std::min(1., std::max(0., v[k]));
}

// The exact Y', Cb, Cr (not rounded) of R', G', B' in [0, 1], n bits, video range (ITU-T H.273 §8.3, BT.601 matrix).
void exactYCbCr(const float rgb[3], int bits, double ycbcr[3])
{
    const double scale = (double)(1 << (bits - 8));
    const double ey = kKr601 * rgb[0] + (1. - kKr601 - kKb601) * rgb[1] + kKb601 * rgb[2];
    ycbcr[0] = scale * (219. * ey + 16.);
    ycbcr[1] = scale * (224. * (rgb[2] - ey) / (2. * (1. - kKb601)) + 128.);
    ycbcr[2] = scale * (224. * (rgb[0] - ey) / (2. * (1. - kKr601)) + 128.);
}

// Frees what an FFmpeg muxing or decoding session allocated (C++ Core Guidelines R.1
// https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-raii); not copyable (C.21
// https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rc-five).
struct AvSession {
    AVFormatContext* fmt = nullptr;
    bool output = false;
    AVCodecContext* codec = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    AvSession() = default;
    AvSession(const AvSession&) = delete;
    AvSession& operator=(const AvSession&) = delete;
    ~AvSession()
    {
        av_packet_free(&pkt);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        if (output) {
            if (fmt && !(fmt->oformat->flags & AVFMT_NOFILE)) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
        } else {
            avformat_close_input(&fmt);
        }
    }
};

// Writes the packets that the encoder of s gives into stream st.
int writePackets(AvSession& s, AVStream* st)
{
    int ret;
    while ((ret = avcodec_receive_packet(s.codec, s.pkt)) >= 0) {
        av_packet_rescale_ts(s.pkt, s.codec->time_base, st->time_base);
        s.pkt->stream_index = st->index;
        if (av_interleaved_write_frame(s.fmt, s.pkt) < 0) { std::printf("  FAIL: write packet\n"); return 1; }
    }
    return ((ret == AVERROR(EAGAIN)) || (ret == AVERROR_EOF)) ? 0 : 1;
}

// The value of component c (in the order of the pixel format's descriptor: Y', Cb, Cr, A) at sample (x, y) of the
// component's plane (chroma coordinates for Cb and Cr), in frame t.
using SampleFunction = std::function<uint32_t(int c, int x, int y, int t)>;

// Writes nFrames frames in pixFmt with a lossless codec (rawvideo or FFV1), tagged with the BT.601 (SMPTE 170M) matrix
// and the range, into file (the container is given by its extension), without swscale: the decoded samples are the
// ones of sample().
int writeLosslessVideo(const std::string& file, AVCodecID codecId, AVPixelFormat pixFmt, int W, int H, int nFrames, bool fullRange, const SampleFunction& sample)
{
    AvSession s;
    s.output = true;
    if ((avformat_alloc_output_context2(&s.fmt, nullptr, nullptr, file.c_str()) < 0) || !s.fmt) { std::printf("  FAIL: no muxer for %s\n", file.c_str()); return 1; }
    const AVCodec* enc = avcodec_find_encoder(codecId);
    AVStream* st = avformat_new_stream(s.fmt, nullptr);
    if (!enc || !st || !(s.codec = avcodec_alloc_context3(enc)) || !s.frame || !s.pkt) { std::printf("  FAIL: no %s encoder\n", avcodec_get_name(codecId)); return 1; }
    s.codec->width = W;
    s.codec->height = H;
    s.codec->pix_fmt = pixFmt;
    s.codec->time_base = AVRational { 1, 24 };
    s.codec->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    s.codec->colorspace = AVCOL_SPC_SMPTE170M;
    if (s.fmt->oformat->flags & AVFMT_GLOBALHEADER) s.codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // (FFV1 in Matroska)
    if ((avcodec_open2(s.codec, enc, nullptr) < 0) || (avcodec_parameters_from_context(st->codecpar, s.codec) < 0)) {
        std::printf("  FAIL: cannot open the %s encoder for %s\n", enc->name, av_get_pix_fmt_name(pixFmt));
        return 1;
    }
    st->time_base = s.codec->time_base;
    if (avio_open(&s.fmt->pb, file.c_str(), AVIO_FLAG_WRITE) < 0 || avformat_write_header(s.fmt, nullptr) < 0) { std::printf("  FAIL: cannot write %s\n", file.c_str()); return 1; }
    s.frame->format = pixFmt;
    s.frame->width = W;
    s.frame->height = H;
    if (av_frame_get_buffer(s.frame, 0) < 0) { std::printf("  FAIL: av_frame_get_buffer\n"); return 1; }
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(pixFmt);
    std::vector<uint32_t> line((size_t)W);
    for (int t = 1; t <= nFrames; ++t) {
        if (av_frame_make_writable(s.frame) < 0) { std::printf("  FAIL: av_frame_make_writable\n"); return 1; }
        // av_write_image_line2() ORs the samples into the planes (libavutil/pixdesc.c:163-190 in FFmpeg n9.0.2):
        // clear them
        size_t planeSizes[4] = {};
        const ptrdiff_t linesizes[4] = { s.frame->linesize[0], s.frame->linesize[1], s.frame->linesize[2], s.frame->linesize[3] };
        if (av_image_fill_plane_sizes(planeSizes, pixFmt, H, linesizes) < 0) { std::printf("  FAIL: av_image_fill_plane_sizes\n"); return 1; }
        for (int i = 0; i < 4; ++i) {
            if (s.frame->data[i]) std::memset(s.frame->data[i], 0, planeSizes[i]);
        }
        for (int c = 0; c < d->nb_components; ++c) {
            const bool chroma = (c == 1 || c == 2) && (d->nb_components >= 3);
            const int cw = chroma ? AV_CEIL_RSHIFT(W, d->log2_chroma_w) : W, ch = chroma ? AV_CEIL_RSHIFT(H, d->log2_chroma_h) : H;
            for (int y = 0; y < ch; ++y) {
                for (int x = 0; x < cw; ++x) line[(size_t)x] = sample(c, x, y, t);
                av_write_image_line2(line.data(), s.frame->data, s.frame->linesize, d, 0, y, c, cw, 4);
            }
        }
        s.frame->pts = t - 1;
        if (avcodec_send_frame(s.codec, s.frame) < 0) { std::printf("  FAIL: %s encode\n", enc->name); return 1; }
        if (writePackets(s, st)) return 1;
    }
    if ((avcodec_send_frame(s.codec, nullptr) < 0) || writePackets(s, st) || (av_write_trailer(s.fmt) < 0)) { std::printf("  FAIL: cannot finish %s\n", file.c_str()); return 1; }
    return 0;
}

// Decodes the video stream of file with libavcodec (no swscale), and calls check(frame, index) for each frame.
int decodeFrames(const std::string& file, const std::function<void(const AVFrame*, int)>& check)
{
    AvSession s;
    if (avformat_open_input(&s.fmt, file.c_str(), nullptr, nullptr) < 0) { std::printf("  FAIL: cannot open %s\n", file.c_str()); return 1; }
    const AVCodec* codec = nullptr;
    const int si = (avformat_find_stream_info(s.fmt, nullptr) >= 0) ? av_find_best_stream(s.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0) : -1;
    if ((si < 0) || !codec || !(s.codec = avcodec_alloc_context3(codec)) || (avcodec_parameters_to_context(s.codec, s.fmt->streams[si]->codecpar) < 0)
        || (avcodec_open2(s.codec, codec, nullptr) < 0) || !s.frame || !s.pkt) {
        std::printf("  FAIL: cannot decode the video stream of %s\n", file.c_str());
        return 1;
    }
    int index = 0;
    auto drain = [&]() {
        while (avcodec_receive_frame(s.codec, s.frame) >= 0) {
            check(s.frame, index++);
            av_frame_unref(s.frame);
        }
    };
    while (av_read_frame(s.fmt, s.pkt) >= 0) {
        if ((s.pkt->stream_index == si) && (avcodec_send_packet(s.codec, s.pkt) >= 0)) drain();
        av_packet_unref(s.pkt);
    }
    avcodec_send_packet(s.codec, nullptr);
    drain();
    return index ? 0 : 1;
}

// Renders frames 1..nFrames of file with ReadFFmpeg (Auto Y'CbCr matrix, the YCbCr Range option whose label is range,
// RGBA unpremultiplied output if rgba): frames[t - 1] is frame t, top-down (row 0 is the top of the movie), 3 or 4
// components per pixel.
int readFFmpegFrames(const std::string& file, int W, int H, int nFrames, bool rgba, const char* range, std::vector<std::vector<float>>* frames)
{
    frames->clear();
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, file);
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
    const int optRange = choiceIndex(ctx, "ycbcrRange", range);
    if (optRange < 0) { std::printf("  FAIL: no %s option in ycbcrRange\n", range); return 1; }
    setOverrideInt(&ctx, "ycbcrRange", optRange);
    if (rgba) {
        const int optRGBA = choiceIndex(ctx, "outputComponents", "RGBA");
        if (optRGBA < 0) { std::printf("  FAIL: no RGBA option in outputComponents\n"); return 1; }
        setOverrideInt(&ctx, "outputComponents", optRGBA);
        setOverrideInt(&ctx, "filePremult", 2); // unpremultiplied
        setOverrideInt(&ctx, "outputPremult", 2);
    }
    Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
    if (!out) { std::printf("  FAIL: no Output clip\n"); return 1; }
    out->connected = true;
    const int comps = rgba ? 4 : 3;
    setupClipImage(out, W, H, rgba ? kOfxImageComponentRGBA : kOfxImageComponentRGB, kOfxBitDepthFloat, true);
    { PropSet outArgs; OfxStatus cp = p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, PH(&outArgs)); if (!ok(cp)) { std::printf("  FAIL: getClipPreferences = %d\n", cp); return 1; } }
    float* fp = reinterpret_cast<float*>(out->buffer.data());
    int failures = 0;
    for (int t = 1; t <= nFrames; ++t) {
        for (size_t i = 0; i < out->buffer.size() / sizeof(float); ++i) fp[i] = -1.f; // sentinel
        PropSet a = makeRenderInArgs((double)t, W, H);
        const OfxStatus st = p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(&inst), PH(&a), nullptr);
        if (!ok(st)) { std::printf("  FAIL: ReadFFmpeg render frame %d = %d\n", t, st); ++failures; break; }
        std::vector<float> f((size_t)W * H * comps);
        for (int r = 0; r < H; ++r) { // the OFX image is bottom-up
            std::copy(fp + (size_t)(H - 1 - r) * W * comps, fp + (size_t)(H - r) * W * comps, f.begin() + (ptrdiff_t)r * W * comps);
        }
        frames->push_back(std::move(f));
    }
    p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(&inst), nullptr, nullptr);
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    return failures;
}

// A lossless Y'CbCr file that ReadFFmpeg must decode to the exact R'G'B' (ITU-T H.273 §8.3), within tolerance.
struct ReadExactCase {
    const char* label;
    const char* ext;
    AVCodecID codec;
    AVPixelFormat pixFmt;
    int W, H;
    bool fullRange;
    double tolerance;     // the largest error of R', G', B', A (x 255)
    double biasTolerance; // the largest mean error of R', G', B' (x 255)
};

// #207, #220, #316: Y' is a ramp over every code value of the range (frame t starts at another value), Cb and Cr are
// flat (so that chroma interpolation and siting do not matter): neutral in frame 1 (whose first pixels are black and
// white: R' = G' = B' = 0 and 1), saturated in frames 2 and 3. Alpha, if any, is a ramp over every code value.
// ReadFFmpeg must give the exact R'G'B' in every pixel, the last column included (#220: it was not written at odd
// widths), rounded to the nearest at 8 bits (#207: the error was up to 1.5/255 with NEON, 2.9/255 without), and white
// must be 1 at more than 8 bits (#316: it was 65283/65535, 0.99615).
int testReadExact(const std::string& outFile, const ReadExactCase& c)
{
    const int N = 3;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(c.pixFmt);
    const int bits = d->comp[0].depth;
    const bool alpha = (d->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    const int scale = 1 << (bits - 8);
    const int lo = c.fullRange ? 0 : 16 * scale, hi = c.fullRange ? (1 << bits) - 1 : 235 * scale; // nominal black and white
    const int chroma[N][2] = { { 128 * scale, 128 * scale }, { 60 * scale, 200 * scale }, { 210 * scale, 40 * scale } };
    auto lumaAt = [&](int x, int y, int t) -> int {
        const int i = x + y * c.W;
        if (t == 1 && i < 2) return i ? hi : lo; // black and white
        return lo + (i * 7 + t * 101) % (hi - lo + 1);
    };
    auto alphaAt = [&](int x, int y) -> int { return (x * 37 + y * c.W * 37) % (1 << bits); };
    const SampleFunction sample = [&](int comp, int x, int y, int t) -> uint32_t {
        if (comp == 0) return (uint32_t)lumaAt(x, y, t);
        if (comp == 3) return (uint32_t)alphaAt(x, y);
        return (uint32_t)chroma[(t - 1) % N][comp - 1];
    };
    const std::string file = outFile + "." + c.ext;
    // one frame more than read: ReadFFmpeg reads one frame less from NUT (see testUncompressed())
    if (writeLosslessVideo(file, c.codec, c.pixFmt, c.W, c.H, N + 1, c.fullRange, sample)) return 1;
    std::vector<std::vector<float>> frames;
    // Matroska stores the range (Auto); NUT does not: the YCbCr Range param
    if (readFFmpegFrames(file, c.W, c.H, N, alpha, std::string(c.ext) == "nut" ? (c.fullRange ? "Full" : "Video") : "Auto", &frames)) return 1;
    std::remove(file.c_str());
    const int comps = alpha ? 4 : 3;
    double maxErr = 0., maxAlphaErr = 0., sum = 0., lastColumnErr = 0.;
    long n = 0;
    int worst[3] = { 0, 0, 0 };
    double worstGot = 0., worstRef = 0.;
    for (int t = 1; t <= N; ++t) {
        const std::vector<float>& f = frames[(size_t)t - 1];
        for (int y = 0; y < c.H; ++y) {
            for (int x = 0; x < c.W; ++x) {
                double ref[3];
                exactRgb(lumaAt(x, y, t), chroma[t - 1][0], chroma[t - 1][1], bits, c.fullRange, ref);
                const float* v = &f[((size_t)y * c.W + x) * comps];
                for (int k = 0; k < 3; ++k) {
                    const double e = ((double)v[k] - ref[k]) * 255.;
                    sum += e;
                    ++n;
                    if (!(std::fabs(e) <= maxErr)) { // (NaN is the largest)
                        maxErr = std::fabs(e);
                        worst[0] = x; worst[1] = y; worst[2] = t;
                        worstGot = v[k]; worstRef = ref[k];
                    }
                    if ((x == c.W - 1) && !(std::fabs(e) <= lastColumnErr)) lastColumnErr = std::fabs(e);
                }
                if (alpha) {
                    const double e = ((double)v[3] - alphaAt(x, y) / (double)((1 << bits) - 1)) * 255.;
                    if (!(std::fabs(e) <= maxAlphaErr)) maxAlphaErr = std::fabs(e);
                }
            }
        }
    }
    const float* first = frames[0].data();
    std::printf("  frame 1: black %.6f %.6f %.6f, white %.6f %.6f %.6f (x 65535: %.1f)\n", first[0], first[1], first[2], first[comps], first[comps + 1],
                first[comps + 2], first[comps] * 65535.);
    const double bias = sum / (double)n;
    std::printf("  R'G'B' error (x 255): mean %+.4f, max %.4f at (%d,%d) frame %d (%.6f, exact %.6f; Y' %d), last column %.4f (tolerance %.3f, mean %.3f)", bias,
                maxErr, worst[0], worst[1], worst[2], worstGot, worstRef, lumaAt(worst[0], worst[1], worst[2]), lastColumnErr, c.tolerance, c.biasTolerance);
    if (alpha) std::printf(", alpha max %.4f", maxAlphaErr);
    std::printf("\n");
    int failures = 0;
    if (!(maxErr <= c.tolerance)) { std::printf("  FAIL: an R'G'B' error is %.4f/255\n", maxErr); ++failures; }
    if (!(std::fabs(bias) <= c.biasTolerance)) { std::printf("  FAIL: the mean R'G'B' error is %+.4f/255\n", bias); ++failures; }
    if (alpha && !(maxAlphaErr <= c.tolerance)) { std::printf("  FAIL: an alpha error is %.4f/255\n", maxAlphaErr); ++failures; }
    std::printf(failures ? "  FAIL (%s)\n" : "  PASS (%s)\n", c.label);
    return failures;
}

// #220: an odd-width MPEG-4 movie (4:2:0) written by WriteFFmpeg, gray frames of three levels: the last column that
// ReadFFmpeg gives must be the same gray as the others (it was not written, and kept the values of another frame).
int testReadOddWidthMpeg4(const std::string& outFile)
{
    const int W = 49, H = 48, N = 3;
    WriterParams wp;
    wp.strings = { { "codecShortName", "mpeg4" } };
    wp.frameValues = { 0.25f, 0.75f, 0.5f };
    const std::string file = outFile + ".i220.mov";
    if (testWriteFFmpeg(file, W, H, N, wp)) return 1;
    std::vector<std::vector<float>> frames;
    if (readFFmpegFrames(file, W, H, N, false, "Auto", &frames)) return 1;
    std::remove(file.c_str());
    double maxErr = 0.; // the largest difference of the last column with the mean of the other columns of its row
    for (const std::vector<float>& f : frames) {
        for (int y = 0; y < H; ++y) {
            for (int k = 0; k < 3; ++k) {
                double mean = 0.;
                for (int x = 0; x < W - 1; ++x) mean += f[((size_t)y * W + x) * 3 + k];
                mean /= (W - 1);
                const double e = std::fabs(f[((size_t)y * W + W - 1) * 3 + k] - mean) * 255.;
                if (!(e <= maxErr)) maxErr = e;
            }
        }
    }
    const double tolerance = 2.;
    std::printf("  last column: largest difference with its row (x 255) %.3f (tolerance %.1f)\n", maxErr, tolerance);
    const bool pass = maxErr <= tolerance;
    std::printf(pass ? "  PASS (MPEG-4 at 49x48)\n" : "  FAIL (MPEG-4 at 49x48)\n");
    return pass ? 0 : 1;
}

// #207: WriteFFmpeg's 4:2:0 and 4:2:2 chroma of alternating red and blue lines (pattern 4) or columns (pattern 5),
// decoded without swscale: each chroma sample must be the average of the two pixels it covers (it was one of them
// with SWS_POINT: Cb 240 or 90, instead of 165). At an odd width, the last chroma sample covers one column, and must
// be between red and blue.
int testWriteChroma(const std::string& outFile, const char* label, const char* ext, int prefCoding, int pattern, int W, int H, const char* expectPixFmt)
{
    const int N = 2;
    WriterParams wp;
    wp.strings = { { "codecShortName", "rawvideo" } };
    wp.ints = { { "prefPixelCoding", prefCoding }, { "prefBitDepth", 0 }, { "enableAlpha", 0 }, { "inputPremult", 2 } };
    wp.pattern = pattern;
    const std::string file = outFile + ".i207chroma." + ext;
    if (testWriteFFmpeg(file, W, H, N, wp)) return 1;
    const float red[3] = { 1.f, 0.f, 0.f }, blue[3] = { 0.f, 0.f, 1.f };
    double yRed[3], yBlue[3];
    exactYCbCr(red, 8, yRed);
    exactYCbCr(blue, 8, yBlue);
    double maxErr = 0.;
    int failures = 0;
    std::string pixFmt;
    const int dec = decodeFrames(file, [&](const AVFrame* frame, int) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        pixFmt = av_get_pix_fmt_name((AVPixelFormat)frame->format);
        const int cw = AV_CEIL_RSHIFT(frame->width, d->log2_chroma_w), ch = AV_CEIL_RSHIFT(frame->height, d->log2_chroma_h);
        for (int c = 1; c <= 2; ++c) {
            for (int y = 0; y < ch; ++y) {
                const uint8_t* row = frame->data[c] + (ptrdiff_t)y * frame->linesize[c];
                for (int x = 0; x < cw; ++x) {
                    // the pixels the sample covers: two lines (pattern 4, 4:2:0) or two columns (pattern 5)
                    const bool covers2 = (pattern == 4) ? (d->log2_chroma_h > 0) : ((2 * x + 1) < frame->width);
                    const double lo = std::min(yRed[c], yBlue[c]), hi = std::max(yRed[c], yBlue[c]);
                    const double e = covers2 ? std::fabs(row[x] - 0.5 * (yRed[c] + yBlue[c])) : ((row[x] < lo - 0.5) || (row[x] > hi + 0.5) ? 255. : 0.);
                    if (!(e <= maxErr)) maxErr = e;
                }
            }
        }
    });
    std::remove(file.c_str());
    if (dec) return 1;
    // the average is 165.1 for Cb, 174.9 for Cr; SWS_AREA gives 164 and 176 at odd widths (it resamples 49 to 25)
    const double tolerance = 1.5;
    std::printf("  %s: chroma error (code values) %.2f (tolerance %.1f)\n", pixFmt.c_str(), maxErr, tolerance);
    if (pixFmt != expectPixFmt) { std::printf("  FAIL: pixel format %s, expected %s\n", pixFmt.c_str(), expectPixFmt); ++failures; }
    if (!(maxErr <= tolerance)) { std::printf("  FAIL: a chroma sample is not the average of its pixels\n"); ++failures; }
    std::printf(failures ? "  FAIL (%s)\n" : "  PASS (%s)\n", label);
    return failures;
}

// #316, writer side: WriteFFmpeg's Y'CbCr at more than 8 bits, decoded without swscale, must be the exact one of the
// image (colorPattern() on no grid), rounded to the nearest (it was too bright by 0.4%: Y' 943 for white at 10 bits).
int testWriteExact(const std::string& outFile, const char* label, int prefCoding, int prefDepth, const char* expectPixFmt, double tolerance)
{
    const int W = 64, H = 48, N = 2;
    WriterParams wp;
    wp.strings = { { "codecShortName", "rawvideo" } };
    wp.ints = { { "prefPixelCoding", prefCoding }, { "prefBitDepth", prefDepth }, { "enableAlpha", 0 }, { "inputPremult", 2 } };
    wp.pattern = 1;
    wp.patternQuant = 0;
    const std::string file = outFile + ".i316w.nut";
    if (testWriteFFmpeg(file, W, H, N, wp)) return 1;
    double maxErr[3] = { 0., 0., 0. }, sum = 0.;
    long n = 0;
    std::string pixFmt;
    const int dec = decodeFrames(file, [&](const AVFrame* frame, int index) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        pixFmt = av_get_pix_fmt_name((AVPixelFormat)frame->format);
        const int bits = d->comp[0].depth;
        for (int y = 0; y < frame->height; ++y) {
            for (int x = 0; x < frame->width; ++x) {
                // row y of the frame (top-down) is row H - 1 - y of the image (bottom-up)
                float rgba[4];
                colorPattern(x, H - 1 - y, index + 1, W, H, false, rgba, 0);
                double ref[3];
                exactYCbCr(rgba, bits, ref);
                for (int c = 0; c < 3; ++c) {
                    // 4:2:2: only the chroma samples of the even columns (the odd ones are averaged)
                    if ((c > 0) && d->log2_chroma_w && (x & 1)) continue;
                    if ((c > 0) && d->log2_chroma_w) {
                        float next[4];
                        colorPattern(x + 1, H - 1 - y, index + 1, W, H, false, next, 0);
                        double refNext[3];
                        exactYCbCr(next, bits, refNext);
                        ref[c] = 0.5 * (ref[c] + refNext[c]);
                    }
                    const uint16_t* row = reinterpret_cast<const uint16_t*>(frame->data[c] + (ptrdiff_t)y * frame->linesize[c]);
                    const double e = row[c ? (x >> d->log2_chroma_w) : x] - ref[c];
                    if (c == 0) { sum += e; ++n; }
                    if (!(std::fabs(e) <= maxErr[c])) maxErr[c] = std::fabs(e);
                }
            }
        }
    });
    std::remove(file.c_str());
    if (dec) return 1;
    int failures = 0;
    const double bias = sum / (double)n;
    std::printf("  %s: error (code values) Y' mean %+.3f max %.3f, Cb max %.3f, Cr max %.3f (tolerance %.2f)\n", pixFmt.c_str(), bias, maxErr[0], maxErr[1], maxErr[2], tolerance);
    if (pixFmt != expectPixFmt) { std::printf("  FAIL: pixel format %s, expected %s\n", pixFmt.c_str(), expectPixFmt); ++failures; }
    for (int c = 0; c < 3; ++c) {
        if (!(maxErr[c] <= tolerance)) { std::printf("  FAIL: component %d differs by %.3f\n", c, maxErr[c]); ++failures; }
    }
    std::printf(failures ? "  FAIL (%s)\n" : "  PASS (%s)\n", label);
    return failures;
}

int testSwscaleExact(const std::string& outFile)
{
    int failures = 0;
    // Tolerances, x 255. 8 bits: rounded to the nearest, plus swscale's fixed-point coefficients (measured: 0.514 at
    // most, +0.003 on average, on FFmpeg 8.0 and 9.0.2). More than 8 bits: the 16-bit rounding and the scaling to white
    // (measured: 0.019 at most).
    // The mean error at 8 bits: +0.006 in video range; up to +0.13 in full range with saturated chroma (measured with a
    // flat Cb, Cr of 60, 200 and 210, 40: the full-range chroma of swscale is a little strong), where every value is
    // still within 0.5 (before: -0.22 to -0.94, and 1.5 to 2.9 at most).
    const double tol8 = 0.52, bias8 = 0.15, tol16 = 0.03, bias16 = 0.01;
    // 8 bits: rawvideo (Matroska has no rawvideo tag for planar 4:2:2: NUT); more than 8 bits: FFV1, whose decoder
    // gives the bit depth (ReadFFmpeg takes it from bits_per_raw_sample, which rawvideo does not set)
    const AVCodecID raw = AV_CODEC_ID_RAWVIDEO, ffv1 = AV_CODEC_ID_FFV1;
    const ReadExactCase reads[] = {
        { "ReadFFmpeg 4:2:0 8-bit, video range, odd width (rawvideo I420 in Matroska)", "mkv", raw, AV_PIX_FMT_YUV420P, 49, 48, false, tol8, bias8 },
        { "ReadFFmpeg 4:2:2 8-bit, video range, odd width (rawvideo Y42B in NUT)", "nut", raw, AV_PIX_FMT_YUV422P, 49, 48, false, tol8, bias8 },
        { "ReadFFmpeg 4:2:0 8-bit, full range (rawvideo in Matroska)", "mkv", raw, AV_PIX_FMT_YUV420P, 64, 48, true, tol8, bias8 },
        { "ReadFFmpeg 4:2:0 8-bit, video range (rawvideo I420 in Matroska)", "mkv", raw, AV_PIX_FMT_YUV420P, 64, 48, false, tol8, bias8 },
        { "ReadFFmpeg 4:2:0 10-bit, video range, odd width (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUV420P10LE, 49, 48, false, tol16, bias16 },
        { "ReadFFmpeg 4:2:2 10-bit, video range (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUV422P10LE, 64, 48, false, tol16, bias16 },
        { "ReadFFmpeg 4:2:0 12-bit, video range (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUV420P12LE, 64, 48, false, tol16, bias16 },
        { "ReadFFmpeg 4:4:4 12-bit, video range (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUV444P12LE, 64, 48, false, tol16, bias16 },
        { "ReadFFmpeg 4:2:0 10-bit, full range (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUV420P10LE, 64, 48, true, tol16, bias16 },
        { "ReadFFmpeg 4:2:0 12-bit, full range (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUV420P12LE, 64, 48, true, tol16, bias16 },
        { "ReadFFmpeg 4:4:4 10-bit with alpha, video range (FFV1)", "mkv", ffv1, AV_PIX_FMT_YUVA444P10LE, 64, 48, false, tol16, bias16 },
    };
    for (size_t i = 0; i < sizeof(reads) / sizeof(reads[0]); ++i) {
        std::printf("==== %s: exact R'G'B' (issues #207, #220, #316) ====\n", reads[i].label);
        failures += testReadExact(outFile + ".i207-" + std::to_string(i), reads[i]);
    }
    std::printf("==== ReadFFmpeg MPEG-4 at an odd width: the last column (issue #220) ====\n");
    failures += testReadOddWidthMpeg4(outFile);
    struct ChromaCase { const char* label; const char* ext; int prefCoding; int pattern; int W, H; const char* pixFmt; };
    const ChromaCase chromas[] = {
        { "WriteFFmpeg 4:2:0, red and blue lines", "mkv", 0, 4, 64, 48, "yuv420p" },
        { "WriteFFmpeg 4:2:0, red and blue lines, odd width", "mkv", 0, 4, 49, 48, "yuv420p" },
        { "WriteFFmpeg 4:2:0, red and blue columns, odd width", "mkv", 0, 5, 49, 48, "yuv420p" },
        { "WriteFFmpeg 4:2:2, red and blue columns, odd width (NUT)", "nut", 1, 5, 49, 48, "yuv422p" },
    };
    for (const ChromaCase& c : chromas) {
        std::printf("==== %s: the chroma is the average (issue #207) ====\n", c.label);
        failures += testWriteChroma(outFile, c.label, c.ext, c.prefCoding, c.pattern, c.W, c.H, c.pixFmt);
    }
    // the exact Y'CbCr rounded to the nearest (0.5 code value), plus the error of swscale's BT.601 coefficients, which
    // only shows in Cb and Cr (measured on FFmpeg 8.0 and 9.0.2, from 16-bit RGB with the 65280 white: Y' 0.52, Cb and
    // Cr 0.75 code value at 10 bits; Y' 2.2, Cb and Cr 16.5 at 16 bits, i.e. 0.065/255; before, Y' was up to 3.9 code
    // values too high at 10 bits, 205 at 16 bits)
    std::printf("==== WriteFFmpeg 4:4:4 10-bit: exact Y'CbCr (issue #316) ====\n");
    failures += testWriteExact(outFile, "WriteFFmpeg 4:4:4 10-bit", 2, 1, "yuv444p10le", 0.8);
    std::printf("==== WriteFFmpeg 4:2:2 10-bit: exact Y'CbCr (issue #316) ====\n");
    failures += testWriteExact(outFile, "WriteFFmpeg 4:2:2 10-bit", 1, 1, "yuv422p10le", 0.8);
    std::printf("==== WriteFFmpeg 4:4:4 16-bit: exact Y'CbCr (issue #316) ====\n");
    failures += testWriteExact(outFile, "WriteFFmpeg 4:4:4 16-bit", 2, 3, "yuv444p16le", 24.);
    return failures;
}

} // namespace

// #304: while a movie is rendered (between beginSequenceRender and endSequenceRender, the frames on a render thread),
// the host calls, on its main thread, kOfxActionInstanceChanged for the params and the clip whose change updates the
// pixel format info (updatePixelFormat(), availPixelFormats()), and kOfxActionSyncPrivateData (when it saves the
// project), which updates it too. They must not free the format context of the render: every frame must be encoded,
// and the file must be complete. The changes are between frames, not during one, so that the test host, whose params
// are not thread-safe, has no data race.
int testWriteFFmpegParamChangeDuringRender(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg, param and clip changes, and sync private data, during a render: every frame is encoded (issue #304) ====\n");
    struct Change {
        const char* type; // nullptr: kOfxActionSyncPrivateData
        const char* name;
    };
    const Change changes[] = { { kOfxTypeParameter, "codec" }, { kOfxTypeParameter, "format" },
                               { kOfxTypeParameter, "prefPixelCoding" }, { kOfxTypeParameter, "prefBitDepth" },
                               { kOfxTypeParameter, "enableAlpha" }, { kOfxTypeParameter, "prefShow" },
                               { kOfxTypeParameter, "DNxHDCodecProfile" }, { kOfxTypeParameter, "HapFormat" },
                               { kOfxTypeClip, kOfxImageEffectSimpleSourceClipName }, { nullptr, "syncPrivateData" } };
    const int nChanges = (int)(sizeof(changes) / sizeof(changes[0]));
    const int N = nChanges + 1;
    std::remove(outFile.c_str());
    int failures = 0;
    {
        WriterSession s;
        if (s.create(outFile, 128, 128, N, WriterParams())) return 1;
        OfxStatus st = s.begin(N);
        if (!ok(st)) { std::printf("  FAIL: beginSequenceRender = %d\n", st); return 1; }
        for (int t = 1; t <= N; ++t) {
            st = std::async(std::launch::async, [&s, t]() { return s.render(t); }).get();
            if (!ok(st)) {
                std::printf("  FAIL: render frame %d = %d (error: \"%s\")\n", t, st, WriterSession::persistentError().c_str());
                ++failures;
            }
            if (t <= nChanges) {
                const Change& c = changes[t - 1];
                st = c.type ? s.instanceChanged(c.type, c.name) : s.syncPrivateData();
                std::printf("  after frame %d: %s(%s %s) = %d\n", t, c.type ? "instanceChanged" : "syncPrivateData", c.type ? c.type : "", c.name, st);
                if ((st != kOfxStatOK) && (st != kOfxStatReplyDefault)) {
                    std::printf("  FAIL: %s = %d (error: \"%s\")\n", c.name, st, WriterSession::persistentError().c_str());
                    ++failures;
                }
            }
        }
        st = s.end(N);
        if (!ok(st)) { std::printf("  FAIL: endSequenceRender = %d (error: \"%s\")\n", st, WriterSession::persistentError().c_str()); ++failures; }
    }
    int nFrames = 0, nIFrames = 0;
    if (probeVideoFrames(outFile, &nFrames, &nIFrames) || (nFrames != N)) {
        std::printf("  FAIL: %d frames in the file, expected %d\n", nFrames, N);
        ++failures;
    }
    std::remove(outFile.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// #319: the host calls kOfxImageEffectActionBeginSequenceRender on a render thread while its main thread calls
// kOfxActionInstanceChanged ("An instance must be able to handle simultaneous actions called on the main and render
// threads", OFX Thread and Recursion Safety). The user edits the Codec while beginSequenceRender sends its warning
// (rawvideo in QuickTime with Pref. RGB, 16 bits, which rawvideo does not write there): the render thread stops in
// setPersistentMessage, without synchronizing with the main thread (relaxed atomics, which ThreadSanitizer does not
// count as synchronization), until the instanceChanged returns. The state of WriteFFmpeg that both actions read or
// write must be guarded: with ThreadSanitizer (test-writeffmpeg-codec-edit-during-begin-tsan), the data race on the
// replaced codec, which beginSequenceRender reads and the Codec edit clears, is reported without the fix. (This works
// only because the render thread makes no host call between that read, in WriteFFmpeg's encodeWarnings(), and this
// hook: a param read added there would take gHostDataMutex (ofx_test_host.h), a happens-before that would hide a
// regression.) Then the frames are rendered, and the file must be complete.
static std::atomic<int> gCodecRaceStep { 0 }; // 1: the render thread is stopped in setPersistentMessage, 2: it may go on
static thread_local bool tCodecRaceRenderThread = false;

static void codecRaceHook(const char* type)
{
    if (!tCodecRaceRenderThread || !type || std::strcmp(type, kOfxMessageWarning) || (gCodecRaceStep.load(std::memory_order_relaxed) != 0)) {
        return;
    }
    gCodecRaceStep.store(1, std::memory_order_relaxed);
    while (gCodecRaceStep.load(std::memory_order_relaxed) != 2) {
        std::this_thread::yield();
    }
}

int testWriteFFmpegCodecEditDuringBegin(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg, the Codec edited on the main thread while beginSequenceRender runs on a render thread (issue #319) ====\n");
    const int N = 3;
    std::remove(outFile.c_str());
    int failures = 0;
    {
        WriterSession s;
        WriterParams wp;
        wp.strings = { { "codecShortName", "rawvideo" } };
        wp.ints = { { "prefPixelCoding", 3 }, { "prefBitDepth", 3 }, { "enableAlpha", 0 } }; // RGB, 16 bits
        if (s.create(outFile, 64, 48, N, wp)) return 1;
        gCodecRaceStep.store(0, std::memory_order_relaxed);
        gPersistentMessageHook = codecRaceHook;
        OfxStatus beginStatus = kOfxStatFailed;
        std::atomic<bool> beginDone { false };
        std::thread renderThread([&]() {
            tCodecRaceRenderThread = true;
            beginStatus = s.begin(N);
            beginDone.store(true);
        });
        while ((gCodecRaceStep.load(std::memory_order_relaxed) == 0) && !beginDone.load()) {
            std::this_thread::yield();
        }
        const bool stopped = (gCodecRaceStep.load(std::memory_order_relaxed) == 1);
        OfxStatus st = kOfxStatOK;
        if (stopped) {
            st = s.instanceChanged(kOfxTypeParameter, "codec");
            gCodecRaceStep.store(2, std::memory_order_relaxed);
        }
        renderThread.join();
        gPersistentMessageHook = nullptr;
        if (!stopped) {
            std::printf("  FAIL: beginSequenceRender sent no persistent warning\n");
            ++failures;
        }
        if ((st != kOfxStatOK) && (st != kOfxStatReplyDefault)) {
            std::printf("  FAIL: instanceChanged(codec) = %d (error: \"%s\")\n", st, WriterSession::persistentError().c_str());
            ++failures;
        }
        if (!ok(beginStatus)) {
            std::printf("  FAIL: beginSequenceRender = %d (error: \"%s\")\n", beginStatus, WriterSession::persistentError().c_str());
            return failures + 1;
        }
        for (int t = 1; t <= N; ++t) {
            st = s.render(t);
            if (!ok(st)) {
                std::printf("  FAIL: render frame %d = %d (error: \"%s\")\n", t, st, WriterSession::persistentError().c_str());
                ++failures;
            }
        }
        st = s.end(N);
        if (!ok(st)) { std::printf("  FAIL: endSequenceRender = %d (error: \"%s\")\n", st, WriterSession::persistentError().c_str()); ++failures; }
    }
    int nFrames = 0, nIFrames = 0;
    if (probeVideoFrames(outFile, &nFrames, &nIFrames) || (nFrames != N)) {
        std::printf("  FAIL: %d frames in the file, expected %d\n", nFrames, N);
        ++failures;
    }
    std::remove(outFile.c_str());
    if (!failures) std::printf("  PASS\n");
    return failures;
}

// Whether this process has the file open, or nothing if that can't be told.
// POSIX: an open file descriptor with the same device and inode (the descriptors are listed in /proc/self/fd on
// Linux, /dev/fd on macOS and the BSDs). MS Windows: FFmpeg's file protocol opens files without FILE_SHARE_DELETE, so
// opening the file with DELETE access fails with a sharing violation while it is open (the file is not changed).
std::optional<bool> fileIsOpen(const std::string& file)
{
#ifdef _WIN32
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, nullptr, 0);
    if (n <= 0) { std::printf("  FAIL: MultiByteToWideChar(%s)\n", file.c_str()); return std::nullopt; }
    std::wstring wfile(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, file.c_str(), -1, &wfile[0], n);
    const HANDLE h = ::CreateFileW(wfile.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        ::CloseHandle(h);
        return false;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_SHARING_VIOLATION) {
        return true;
    }
    std::printf("  FAIL: CreateFileW(%s) error %lu\n", file.c_str(), (unsigned long)error);
    return std::nullopt;
#else
    struct stat fileStat;
    if (::stat(file.c_str(), &fileStat) != 0) {
        std::printf("  FAIL: stat(%s): %s\n", file.c_str(), std::strerror(errno));
        return std::nullopt;
    }
#ifdef __linux__
    const char* const fdDir = "/proc/self/fd";
#else
    const char* const fdDir = "/dev/fd";
#endif
    DIR* dir = ::opendir(fdDir);
    if (!dir) {
        std::printf("  FAIL: opendir(%s): %s\n", fdDir, std::strerror(errno));
        return std::nullopt;
    }
    bool open = false;
    while (const struct dirent* entry = ::readdir(dir)) {
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if ((end == entry->d_name) || (*end != '\0') || (fd < 0) || (fd > INT_MAX)) {
            continue; // "." and ".."
        }
        struct stat fdStat;
        if ((::fstat(static_cast<int>(fd), &fdStat) == 0) && (fdStat.st_dev == fileStat.st_dev) && (fdStat.st_ino == fileStat.st_ino)) {
            open = true;
            break;
        }
    }
    ::closedir(dir);
    return open;
#endif
}

// Issue #290: destroying a ReadFFmpeg instance must close its movie file (and free its decoder), while the plug-in is
// still loaded (other instances may still exist), not only at kOfxActionUnload.
// Two instances, one after the other; between them, the movie is rewritten at another size: the second instance must
// read the new movie, even if it gets the address of the first one (the open files of the plug-in are keyed by the
// address of the instance).
int testReadFFmpegDestroyClosesFile(const std::string& outFile)
{
    const int N = 3;
    const int sizes[2][2] = { { 64, 64 }, { 32, 48 } };
    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    int failures = 0;
    void* firstInstance = nullptr;
    // one instance: returns the number of failures
    auto runPass = [&](int pass) -> int {
        const int W = sizes[pass][0], H = sizes[pass][1];
        if (testWriteFFmpeg(outFile, W, H, N) != 0) {
            return 1;
        }
        int fails = 0;
        Effect inst;
        inst.delegate = &ctx;
        setInstanceProps(inst, W, H, N, kOfxImageEffectContextReader);
        const OfxImageEffectHandle handle = reinterpret_cast<OfxImageEffectHandle>(&inst);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, handle, nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
        void* instance = nullptr;
        pGetPointer(PH(&inst.props), kOfxPropInstanceData, 0, &instance);
        if (pass == 0) {
            firstInstance = instance;
        } else {
            std::printf("  pass %d: the instance %s the address of the first one\n", pass + 1, (instance == firstInstance) ? "has" : "does not have");
        }
        {
            PropSet a; OfxPropertySetHandle ap = PH(&a);
            pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
            pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
            pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
            pSetDouble(ap, kOfxPropTime, 0, 1.);
            double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
            const OfxStatus st = p->mainEntry(kOfxActionInstanceChanged, handle, PH(&a), nullptr);
            if (!ok(st)) { std::printf("  FAIL: InstanceChanged = %d\n", st); ++fails; }
        }
        {
            PropSet a; pSetDouble(PH(&a), kOfxPropTime, 0, 1.);
            double rs[2] = { 1., 1. }; pSetDoubleN(PH(&a), kOfxImageEffectPropRenderScale, 2, rs);
            pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, 0);
            PropSet o;
            const OfxStatus st = p->mainEntry(kOfxImageEffectActionGetRegionOfDefinition, handle, PH(&a), PH(&o));
            double rod[4] = { -1, -1, -1, -1 }; pGetDoubleN(PH(&o), kOfxImageEffectPropRegionOfDefinition, 4, rod);
            std::printf("  pass %d: region of definition [%.0f %.0f %.0f %.0f] (the movie is %dx%d)\n", pass + 1, rod[0], rod[1], rod[2], rod[3], W, H);
            if (!ok(st) || (rod[0] != 0) || (rod[1] != 0) || (rod[2] != W) || (rod[3] != H)) {
                std::printf("  FAIL: getRegionOfDefinition = %d, must be the size of the movie\n", st);
                ++fails;
            }
        }
        Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
        if (!out) {
            std::printf("  FAIL: no Output clip\n");
            ++fails;
        } else {
            out->connected = true;
            setupClipImage(out, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
            PropSet outArgs;
            OfxStatus st = p->mainEntry(kOfxImageEffectActionGetClipPreferences, handle, nullptr, PH(&outArgs));
            if (!ok(st)) { std::printf("  FAIL: GetClipPreferences = %d\n", st); ++fails; }
            PropSet a = makeRenderInArgs(1., W, H);
            st = p->mainEntry(kOfxImageEffectActionRender, handle, PH(&a), nullptr);
            if (!ok(st)) { std::printf("  FAIL: ReadFFmpeg render = %d\n", st); ++fails; }
        }
        // (also checks that fileIsOpen() sees the files that FFmpeg opens)
        const std::optional<bool> openWhileAlive = fileIsOpen(outFile);
        std::printf("  pass %d: the movie is %s while the instance exists\n", pass + 1, !openWhileAlive ? "?" : (*openWhileAlive ? "open" : "closed"));
        if (openWhileAlive != true) { std::printf("  FAIL: the movie must be open while the instance exists\n"); ++fails; }
        const OfxStatus st = p->mainEntry(kOfxActionDestroyInstance, handle, nullptr, nullptr);
        if (!ok(st)) { std::printf("  FAIL: DestroyInstance = %d\n", st); ++fails; }
        const std::optional<bool> openAfterDestroy = fileIsOpen(outFile);
        std::printf("  pass %d: the movie is %s after DestroyInstance\n", pass + 1, !openAfterDestroy ? "?" : (*openAfterDestroy ? "open" : "closed"));
        if (openAfterDestroy != false) { std::printf("  FAIL: DestroyInstance must close the movie (issue #290)\n"); ++fails; }
        return fails;
    };
    for (int pass = 0; pass < 2; ++pass) {
        failures += runPass(pass);
    }
    // (also after a failure)
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    if (fileIsOpen(outFile) != false) { std::printf("  FAIL: the movie must be closed after Unload\n"); ++failures; }
    return failures;
}

// Whether the muxer of a container takes a video stream of an encoder's codec: avformat_query_codec(), and
// avformat_init_output(), which refuses some of them (e.g. VP8 and AV1 in mov), with the first pixel format of the
// encoder (rawvideo, which lists none: uyvy422, which mov and nut tag).
bool muxerTakesEncoder(const char* ext, const AVCodec* c)
{
    const std::string name = std::string("x.") + ext;
    const AVOutputFormat* f = av_guess_format(nullptr, name.c_str(), nullptr);
    if (!f || (avformat_query_codec(f, c->id, FF_COMPLIANCE_NORMAL) != 1)) return false;
    AvSession s;
    s.output = true;
    if ((avformat_alloc_output_context2(&s.fmt, f, nullptr, name.c_str()) < 0) || !s.fmt) return false;
    AVStream* st = avformat_new_stream(s.fmt, nullptr);
    if (!st) return false;
    const void* pixFmts = nullptr;
    int nPixFmts = 0;
    avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &pixFmts, &nPixFmts);
    st->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    st->codecpar->codec_id = c->id;
    st->codecpar->width = 256;
    st->codecpar->height = 144;
    st->codecpar->format = (pixFmts && (nPixFmts > 0)) ? static_cast<const AVPixelFormat*>(pixFmts)[0] : AV_PIX_FMT_UYVY422;
    st->time_base = AVRational { 1, 25 };
    if (avio_open_dyn_buf(&s.fmt->pb) < 0) return false;
    const bool initialized = (avformat_init_output(s.fmt, nullptr) >= 0);
    uint8_t* buf = nullptr;
    avio_close_dyn_buf(s.fmt->pb, &buf);
    av_free(buf);
    s.fmt->pb = nullptr;
    return initialized;
}

// The container of the movies of an encoder: the first of mov, mkv, nut and avi that takes it, or an empty string.
std::string testContainerFor(const std::string& encoderName)
{
    const AVCodec* c = avcodec_find_encoder_by_name(encoderName.c_str());
    if (!c) return std::string();
    const int level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET); // the muxers that do not take the codec say it
    std::string container;
    for (const char* ext : { "mov", "mkv", "nut", "avi" }) {
        if (muxerTakesEncoder(ext, c)) {
            container = ext;
            break;
        }
    }
    av_log_set_level(level);
    return container;
}

// The video stream of a movie: its codec, profile, pixel format and color tags, its number of packets and of key
// packets, and a hash (FNV-1a) of their data, which do not depend on the container (Matroska writes a random segment
// UID).
std::string videoStreamSignature(const std::string& file)
{
    AvSession s;
    if (avformat_open_input(&s.fmt, file.c_str(), nullptr, nullptr) < 0) return "cannot open";
    const int si = (avformat_find_stream_info(s.fmt, nullptr) >= 0) ? av_find_best_stream(s.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    if ((si < 0) || !s.pkt) return "no video stream";
    const AVCodecParameters* par = s.fmt->streams[si]->codecpar;
    uint64_t h = 14695981039346656037ull;
    int nPackets = 0;
    int nKeyPackets = 0;
    while (av_read_frame(s.fmt, s.pkt) >= 0) {
        if (s.pkt->stream_index == si) {
            ++nPackets;
            if (s.pkt->flags & AV_PKT_FLAG_KEY) ++nKeyPackets;
            for (int i = 0; i < s.pkt->size; ++i) {
                h = (h ^ (uint64_t)s.pkt->data[i]) * 1099511628211ull;
            }
        }
        av_packet_unref(s.pkt);
    }
    const char* pixFmt = av_get_pix_fmt_name((AVPixelFormat)par->format);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s profile %d %s range %d matrix %d primaries %d trc %d, %d packets (%d key), hash %016llx",
                  avcodec_get_name(par->codec_id), par->profile, pixFmt ? pixFmt : "none", (int)par->color_range,
                  (int)par->color_space, (int)par->color_primaries, (int)par->color_trc, nPackets, nKeyPackets, (unsigned long long)h);
    return buf;
}

// Encode 3 frames (W x H, colorPattern() with its alpha ramp, which changes with the frame) with an encoder of the
// Codec menu and extra int params. Returns 0 on success, else 1 and the persistent error in *error.
int encodeCodecVariant(const std::string& file, const std::string& shortName, const std::vector<std::pair<std::string, int>>& ints,
                       std::string* error, int W = 256, int H = 144)
{
    const int nFrames = 3;
    std::remove(file.c_str());
    WriterParams wp;
    wp.strings = { { "codecShortName", shortName } };
    wp.ints = ints;
    WriterSession s;
    if (s.create(file, W, H, nFrames, wp)) {
        *error = "cannot create the instance";
        return 1;
    }
    bool succeeded = ok(s.begin(nFrames));
    for (int t = 1; succeeded && (t <= nFrames); ++t) {
        float* rgba = s.sourcePixels();
        for (int r = 0; r < H; ++r) {
            for (int x = 0; x < W; ++x) colorPattern(x, r, t, W, H, true, &rgba[((size_t)r * W + x) * 4]);
        }
        succeeded = ok(s.render(t));
    }
    succeeded = ok(s.end(nFrames)) && succeeded;
    if (!succeeded) {
        *error = WriterSession::persistentError();
        std::replace(error->begin(), error->end(), '\n', ' '); // one line per movie
    }
    return succeeded ? 0 : 1;
}

// The value of a string param that the plug-in set, or "?".
std::string stringParamValue(const Effect& ctx, const char* name)
{
    const auto it = ctx.params.find(name);
    return ((it != ctx.params.end()) && it->second.hasOv && !it->second.ov.s.empty()) ? it->second.ov.s[0] : std::string("?");
}

namespace {
// The encoder of a Codec menu entry (its codecShortName): the ProRes entries are prores_ks with a profile suffix
// (e.g. prores_ksapch), the others are the name of the encoder.
std::string encoderOfShortName(const std::string& shortName)
{
    return (shortName.rfind("prores_ks", 0) == 0) ? std::string("prores_ks") : shortName;
}

// The codecShortName of each entry of WriteFFmpeg's Codec menu, in menu order (empty if there is none, or on error).
std::vector<std::string> codecMenuShortNames(const std::string& outFile)
{
    std::vector<std::string> shortNames;
    WriterSession s;
    if (s.create(outFile + ".mov", 64, 48, 1, WriterParams())) return shortNames;
    const Effect& ctx = s.context();
    const auto codec = ctx.params.find("codec");
    if (codec == ctx.params.end()) return shortNames;
    const auto opt = codec->second.props->props.find(kOfxParamPropChoiceOption);
    const size_t nCodecs = (opt == codec->second.props->props.end()) ? 0 : opt->second.s.size();
    for (size_t i = 0; i < nCodecs; ++i) {
        s.setInt("codec", (int)i);
        s.instanceChanged(kOfxTypeParameter, "codec");
        shortNames.push_back(stringParamValue(ctx, "codecShortName"));
    }
    return shortNames;
}
} // namespace

// #343: for each encoder of the Codec menu, the encoder settings that WriteFFmpeg shows (- hidden, d disabled, E
// enabled) and the pixel format it selects for each Pref. Pixel Coding, Bit Depth and Alpha, and movies encoded with
// the default settings (in the container given by testContainerFor()), with Keyframe Interval = 1, Max B-Frames = 2
// and Output Quality = Lossless if they are shown, and with Alpha checked: each one must encode, or, for Lossless with
// an encoder that has no lossless mode (e.g. libvpx), fail with an error that says it. The output (with the signature of the video
// stream of each movie) is the same with two builds if the settings and the movies are the same: compare it with diff.
int testWriteFFmpegCodecParams(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg: the settings of each encoder, and its movies (issue #343) ====\n");
    av_log_set_level(AV_LOG_ERROR);
    const std::vector<std::string> shortNames = codecMenuShortNames(outFile);
    if (shortNames.empty()) { std::printf("  FAIL: no Codec menu entry (or no Codec param)\n"); return 1; }
    const char* const settings[] = { "crf", "x26xSpeed", "qscale", "bitrateMbps", "bitrateToleranceMbps", "quality", "gopSize", "bFrames" };
    int failures = 0;
    for (const std::string& shortName : shortNames) {
        const std::string encoder = encoderOfShortName(shortName);
        const AVCodec* c = avcodec_find_encoder_by_name(encoder.c_str());
        const std::string ext = testContainerFor(encoder);
        if (!c || ext.empty()) { std::printf("  FAIL: %s: no container takes the encoder %s\n", shortName.c_str(), encoder.c_str()); ++failures; continue; }
        const std::string file = outFile + "." + ext;
        bool bFramesShown = false;
        bool crfShown = false;
        bool gopShown = false;
        {
            WriterParams wp;
            wp.strings = { { "codecShortName", shortName } };
            WriterSession s;
            if (s.create(file, 64, 48, 1, wp)) { ++failures; continue; }
            const Effect& ctx = s.context();
            std::string line = "  " + shortName + " (" + ext + "):";
            for (const char* name : settings) {
                const auto it = ctx.params.find(name);
                int secret = 0;
                int enabled = 0;
                if ((it == ctx.params.end()) || (pGetInt(PH(it->second.props), kOfxParamPropSecret, 0, &secret) != kOfxStatOK)
                    || (pGetInt(PH(it->second.props), kOfxParamPropEnabled, 0, &enabled) != kOfxStatOK)) {
                    std::printf("  FAIL: %s: no param %s, or its secret or enabled property is not set\n", shortName.c_str(), name);
                    ++failures;
                    continue;
                }
                line += std::string(" ") + name + "=" + (secret ? "-" : (enabled ? "E" : "d"));
                if (!secret && !std::strcmp(name, "bFrames")) bFramesShown = true;
                if (!secret && !std::strcmp(name, "crf")) crfShown = true;
                if (!secret && !std::strcmp(name, "gopSize")) gopShown = true;
            }
            std::printf("%s\n", line.c_str());
            for (int alpha = 0; alpha <= 1; ++alpha) {
                line = std::string("    pixel formats, alpha ") + (alpha ? "1" : "0") + ":";
                for (int coding = 0; coding <= 4; ++coding) {
                    for (int depth = 0; depth <= 3; ++depth) {
                        s.setInt("enableAlpha", alpha);
                        s.setInt("prefPixelCoding", coding);
                        s.setInt("prefBitDepth", depth);
                        s.instanceChanged(kOfxTypeParameter, "enableAlpha");
                        line += " " + stringParamValue(ctx, "infoPixelFormat");
                    }
                }
                std::printf("%s\n", line.c_str());
            }
        }
        // CRF 0 may be lossless if the encoder has a lossless option or its codec a lossless mode (else it may fail)
        const AVCodecDescriptor* desc = avcodec_descriptor_get(c->id);
        const bool lossless = (c->priv_class && av_opt_find(const_cast<const AVClass**>(&c->priv_class), "lossless", nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ))
                              || (desc && (desc->props & AV_CODEC_PROP_LOSSLESS));
        struct Variant {
            const char* label;
            std::vector<std::pair<std::string, int>> ints;
            bool enabled;
            const char* failureHint; // if not nullptr, the encode may fail, with an error that contains it
        };
        const Variant variants[] = { { "default", {}, true, nullptr },
                                     { "gopSize=1", { { "gopSize", 1 } }, gopShown, nullptr },
                                     { "bFrames=2", { { "bFrames", 2 } }, bFramesShown, nullptr },
                                     { "crf=Lossless", { { "crf", 1 } }, crfShown, lossless ? nullptr : "Lossless (CRF 0) may not be available" },
                                     { "alpha", { { "enableAlpha", 1 } }, true, nullptr } };
        for (const Variant& v : variants) {
            if (!v.enabled) continue;
            std::string error;
            const int fail = encodeCodecVariant(file, shortName, v.ints, &error);
            if (fail && v.failureHint) {
                const bool hinted = (error.find(v.failureHint) != std::string::npos);
                std::printf("    %s: %s: %s\n", v.label, hinted ? "failed with the expected error" : "FAIL, the error must contain", hinted ? error.c_str() : v.failureHint);
                failures += hinted ? 0 : 1;
            } else if (fail) {
                std::printf("    %s: FAIL: %s\n", v.label, error.c_str());
                ++failures;
            } else {
                std::printf("    %s: encoded, %s\n", v.label, videoStreamSignature(file).c_str());
            }
        }
        std::remove(file.c_str());
    }
    if (!failures) std::printf("  PASS\n");
    return failures;
}

namespace {
// The pixel format of the video stream of a movie, or "none".
std::string videoStreamPixelFormat(const std::string& file)
{
    AvSession s;
    if (avformat_open_input(&s.fmt, file.c_str(), nullptr, nullptr) < 0) return "none";
    const int si = (avformat_find_stream_info(s.fmt, nullptr) >= 0) ? av_find_best_stream(s.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    const char* name = (si >= 0) ? av_get_pix_fmt_name(static_cast<AVPixelFormat>(s.fmt->streams[si]->codecpar->format)) : nullptr;
    return name ? name : "none";
}

// The duration in seconds of the video stream of a movie (its duration, else the container's), or -1.
double videoStreamDuration(const std::string& file)
{
    AvSession s;
    if (avformat_open_input(&s.fmt, file.c_str(), nullptr, nullptr) < 0) return -1.;
    const int si = (avformat_find_stream_info(s.fmt, nullptr) >= 0) ? av_find_best_stream(s.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0) : -1;
    if (si < 0) return -1.;
    const AVStream* st = s.fmt->streams[si];
    if (st->duration != AV_NOPTS_VALUE) return st->duration * av_q2d(st->time_base);
    return (s.fmt->duration != AV_NOPTS_VALUE) ? s.fmt->duration / (double)AV_TIME_BASE : -1.;
}

// Whether the encoder lists a pixel format that fits a Pref. Pixel Coding of Y'CbCr 4:2:2 or 4:4:4: a color format
// without vertical chroma subsampling, and at most 2 to 1 horizontally (4:2:2, 4:4:4, RGB). False if it lists none.
bool encoderHasFormatFor422(const AVCodec* c)
{
    const void* configs = nullptr;
    int n = 0;
    if ((avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &n) < 0) || !configs) return false;
    const AVPixelFormat* fmts = static_cast<const AVPixelFormat*>(configs);
    for (int i = 0; i < n; ++i) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmts[i]);
        if (d && (d->nb_components >= 3) && !(d->flags & (AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BAYER))
            && (d->log2_chroma_h == 0) && (d->log2_chroma_w <= 1)) {
            return true;
        }
    }
    return false;
}
} // namespace

// #218: the pixel format that WriteFFmpeg selects must fit the Pref. Pixel Coding. 4:4:0 (one chroma sample per 1x2
// luma samples) has as many chroma samples as 4:2:2, and was taken as 4:2:2: FFV1, which lists yuv440p before yuv422p,
// was encoded in yuv440p with the default preferences (4:2:2, 8 bits), at every size.
//  - FFV1 at 720x576 and 720x480, Pref. Pixel Coding 4:2:0, 4:2:2 and 4:4:4, Bit Depth 8, 10, 12 and 16 (the source
//    clip is float, so the Bit Depth is what selects the depth): the stream must have the format of that coding and
//    depth (FFV1 has them all); with Alpha (at 720x576), the format must have alpha, at least that depth, and the
//    chroma subsampling of the coding (or be RGB);
//  - every encoder of the Codec menu that lists a format for 4:2:2 (see encoderHasFormatFor422()): the selected
//    format must not be 4:4:0 with a 4:2:2 or 4:4:4 preference, with or without alpha, at every bit depth.
int testWriteFFmpegPixelFormatChoice(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg: the selected pixel format fits the Pref. Pixel Coding, not 4:4:0 for 4:2:2 (issue #218) ====\n");
    av_log_set_level(AV_LOG_ERROR);
    int failures = 0;
    const char* const codings[] = { "420", "422", "444" }; // Pref. Pixel Coding 0 to 2
    const int depths[] = { 8, 10, 12, 16 }; // Pref. Bit Depth 0 to 3
    if (!avcodec_find_encoder_by_name("ffv1")) {
        std::printf("  FAIL: no ffv1 encoder\n");
        return 1;
    }
    const std::string file = outFile + ".mkv";
    struct Size {
        int w, h;
        bool alpha; // also with Alpha
    };
    for (const Size& size : { Size { 720, 576, true }, Size { 720, 480, false } }) {
        for (int alpha = 0; alpha <= (size.alpha ? 1 : 0); ++alpha) {
            for (int coding = 0; coding < 3; ++coding) {
                for (int depth = 0; depth < 4; ++depth) {
                    std::string error;
                    if (encodeCodecVariant(file, "ffv1", { { "prefPixelCoding", coding }, { "prefBitDepth", depth }, { "enableAlpha", alpha } }, &error, size.w, size.h)) {
                        std::printf("  FAIL: ffv1 %dx%d %s/%d alpha %d: %s\n", size.w, size.h, codings[coding], depths[depth], alpha, error.c_str());
                        ++failures;
                        continue;
                    }
                    const std::string got = videoStreamPixelFormat(file);
                    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(av_get_pix_fmt(got.c_str()));
                    bool pass;
                    std::string expected;
                    if (!alpha) {
                        // e.g. yuv422p10, the native-endian format (av_get_pix_fmt())
                        expected = std::string("yuv") + codings[coding] + "p" + ((depths[depth] == 8) ? std::string() : std::to_string(depths[depth]));
                        pass = (av_get_pix_fmt(expected.c_str()) == av_get_pix_fmt(got.c_str()));
                    } else {
                        // the log2 chroma subsampling of each coding (width, height), or RGB
                        const int log2w[] = { 1, 1, 0 };
                        const int log2h[] = { 1, 0, 0 };
                        expected = std::string("with alpha, ") + codings[coding] + " or RGB, at least " + std::to_string(depths[depth]) + " bits";
                        pass = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA) && (d->comp[0].depth >= depths[depth])
                               && ((d->flags & AV_PIX_FMT_FLAG_RGB) || ((d->log2_chroma_w == log2w[coding]) && (d->log2_chroma_h == log2h[coding])));
                    }
                    std::printf("  %sffv1 %dx%d %s/%d alpha %d: %s%s%s\n", pass ? "" : "FAIL: ", size.w, size.h, codings[coding], depths[depth], alpha,
                                got.c_str(), pass ? "" : ", expected ", pass ? "" : expected.c_str());
                    failures += pass ? 0 : 1;
                }
            }
        }
    }
    std::remove(file.c_str());

    // every encoder of the Codec menu: the Selected Pixel Coding (infoPixelFormat, e.g. YUV440 or YUVA440/10) is not
    // 4:4:0 with a 4:2:2 or 4:4:4 preference
    const std::vector<std::string> shortNames = codecMenuShortNames(outFile);
    if (shortNames.empty()) { std::printf("  FAIL: no Codec menu entry (or no Codec param)\n"); return failures + 1; }
    int nChecked = 0;
    for (const std::string& shortName : shortNames) {
        const std::string encoder = encoderOfShortName(shortName);
        const AVCodec* c = avcodec_find_encoder_by_name(encoder.c_str());
        const std::string ext = testContainerFor(encoder);
        if (!c || ext.empty() || !encoderHasFormatFor422(c)) continue;
        WriterParams wp;
        wp.strings = { { "codecShortName", shortName } };
        WriterSession s;
        if (s.create(outFile + "." + ext, 64, 48, 1, wp)) { std::printf("  FAIL: %s: cannot create the instance\n", shortName.c_str()); ++failures; continue; }
        ++nChecked;
        const Effect& ctx = s.context();
        for (int alpha = 0; alpha <= 1; ++alpha) {
            for (int coding = 1; coding < 3; ++coding) {
                for (int depth = 0; depth < 4; ++depth) {
                    s.setInt("enableAlpha", alpha);
                    s.setInt("prefPixelCoding", coding);
                    s.setInt("prefBitDepth", depth);
                    // a change of any of these params calls updatePixelFormat() (WriteFFmpegPlugin::changedParam()), which
                    // reads all of them: one instanceChanged is enough
                    s.instanceChanged(kOfxTypeParameter, "enableAlpha");
                    const std::string label = stringParamValue(ctx, "infoPixelFormat");
                    if (label.find("440") != std::string::npos) {
                        std::printf("  FAIL: %s %s/%d alpha %d: %s\n", shortName.c_str(), codings[coding], depths[depth], alpha, label.c_str());
                        ++failures;
                    }
                }
            }
        }
    }
    std::printf("  %d encoders of the Codec menu checked for 4:4:0 with a 4:2:2 or 4:4:4 preference\n", nChecked);
    if (!failures) std::printf("  PASS\n");
    return failures;
}

namespace {
// The names of the encoders whose codec context logged (AVCodecContext's item_name() is the name of its codec,
// libavcodec/options.c), while logEncoderNames() is the av_log callback. The encoders may log from their threads:
// the mutex guards names.
struct EncoderLog {
    std::mutex mutex;
    std::set<std::string> names;
};
EncoderLog gEncoderLog;

void logEncoderNames(void* avcl, int level, const char* fmt, va_list vl)
{
    const AVClass* cls = avcl ? *static_cast<const AVClass* const*>(avcl) : nullptr;
    if (cls && cls->item_name && !std::strcmp(cls->class_name, "AVCodecContext")) {
        const AVClassCategory category = cls->get_category ? cls->get_category(avcl) : cls->category;
        if (category == AV_CLASS_CATEGORY_ENCODER) {
            const std::lock_guard<std::mutex> lock(gEncoderLog.mutex);
            gEncoderLog.names.insert(cls->item_name(avcl));
        }
    }
    if (level <= AV_LOG_ERROR) {
        av_log_default_callback(avcl, level, fmt, vl);
    }
}

// While it exists, logEncoderNames() records the encoders that log at the debug level (gEncoderLog.names is cleared
// first); the default callback and the AV_LOG_ERROR level are restored on destruction.
class EncoderLogScope {
public:
    EncoderLogScope()
    {
        {
            const std::lock_guard<std::mutex> lock(gEncoderLog.mutex);
            gEncoderLog.names.clear();
        }
        av_log_set_callback(logEncoderNames);
        av_log_set_level(AV_LOG_DEBUG);
    }
    ~EncoderLogScope()
    {
        av_log_set_level(AV_LOG_ERROR);
        av_log_set_callback(av_log_default_callback);
    }
    EncoderLogScope(const EncoderLogScope&) = delete;
    EncoderLogScope& operator=(const EncoderLogScope&) = delete;
};

// Encode 3 frames with a Codec menu entry and its default settings. Returns 0 on success (else 1 and the error), and
// the names of the encoders that logged.
int encodeLoggingEncoders(const std::string& file, const std::string& shortName, std::string* error, std::set<std::string>* names)
{
    int fail;
    {
        const EncoderLogScope scope;
        fail = encodeCodecVariant(file, shortName, {}, error);
    }
    const std::lock_guard<std::mutex> lock(gEncoderLog.mutex);
    *names = gEncoderLog.names;
    return fail;
}

std::string joinNames(const std::set<std::string>& names)
{
    std::string list;
    for (const std::string& n : names) list += (list.empty() ? "" : ", ") + n;
    return list;
}
} // namespace

// #348: WriteFFmpeg must encode with the encoder selected in the Codec menu, not with the default encoder of its codec
// ID (avcodec_find_encoder(), the first one registered in libavcodec/allcodecs.c), as "ffmpeg -c:v name" does. For
// each encoder S of the menu that is not that default D (e.g. libx264rgb and libopenh264 for H.264, libxvid for
// MPEG-4, libopenjpeg for JPEG 2000, if this FFmpeg build has them), encode 3 frames with the default settings, and
// record which encoder contexts log at the debug level: only S may log. Positive control: D must be in the menu, and
// log when it is selected; otherwise the pair is not checkable. An encoder that logs nothing itself (e.g. libxvid) is
// not confirmed, but D must not log.
int testWriteFFmpegSelectedEncoder(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg encodes with the encoder selected in the Codec menu (issue #348) ====\n");
    av_log_set_level(AV_LOG_ERROR);
    const std::vector<std::string> shortNames = codecMenuShortNames(outFile);
    if (shortNames.empty()) { std::printf("  FAIL: no Codec menu entry (or no Codec param)\n"); return 1; }
    int failures = 0;
    int tested = 0;
    int notCheckable = 0;
    for (const std::string& shortName : shortNames) {
        const std::string encoder = encoderOfShortName(shortName);
        const AVCodec* c = avcodec_find_encoder_by_name(encoder.c_str());
        const AVCodec* d = c ? avcodec_find_encoder(c->id) : nullptr;
        if (!c || !d || (d == c)) continue;
        const std::string label = "  " + shortName + " (default encoder of " + avcodec_get_name(c->id) + ": " + d->name + ")";
        const std::string ext = testContainerFor(encoder);
        if (ext.empty()) { std::printf("%s: FAIL: no container takes the encoder %s\n", label.c_str(), encoder.c_str()); ++failures; continue; }
        const std::string file = outFile + "." + ext;
        // positive control: the default encoder, selected in the menu, logs its name
        std::string error;
        std::set<std::string> names;
        const bool dInMenu = (std::find(shortNames.begin(), shortNames.end(), std::string(d->name)) != shortNames.end());
        if (!dInMenu || encodeLoggingEncoders(file, d->name, &error, &names) || !names.count(d->name)) {
            std::printf("%s: not checkable: %s\n", label.c_str(), dInMenu ? (std::string(d->name) + " does not log when it is selected").c_str() : (std::string(d->name) + " is not in the Codec menu").c_str());
            ++notCheckable;
            std::remove(file.c_str());
            continue;
        }
        ++tested;
        error.clear();
        const int fail = encodeLoggingEncoders(file, shortName, &error, &names);
        const bool confirmed = (names.erase(c->name) > 0);
        const std::string others = joinNames(names);
        if (fail) {
            std::printf("%s, %s: FAIL: %s%s%s\n", label.c_str(), ext.c_str(), error.c_str(), others.empty() ? "" : "; encoders opened: ", others.c_str());
            ++failures;
        } else if (!others.empty()) {
            std::printf("%s, %s: FAIL: encoded by %s\n", label.c_str(), ext.c_str(), others.c_str());
            ++failures;
        } else {
            std::printf("%s, %s: %s, %s\n", label.c_str(), ext.c_str(),
                        confirmed ? "encoded by the selected encoder" : "encoded, not by the default encoder (the selected one logs nothing to confirm it)",
                        videoStreamSignature(file).c_str());
        }
        std::remove(file.c_str());
    }
    if (!tested) {
        std::printf("  SKIP: no checkable encoder (%d not checkable): in this FFmpeg build, the Codec menu has no encoder that is not the default encoder of its codec ID, with that default in the menu\n", notCheckable);
        std::exit(77);
    }
    if (!failures) {
        std::printf("  PASS (%d encoders, %d not checkable)\n", tested, notCheckable);
    }
    return failures;
}

// #388: WriteFFmpeg must fail the render when the encoder of the selected Codec menu entry is missing from the FFmpeg
// build, instead of encoding with the default encoder of the container (avcodec_find_encoder(fmt->video_codec)). Run
// with OFX_IO_TEST_HIDE_ENCODERS=prores_ks: avcodec_find_encoder_by_name() (above) does not find it. The ProRes entries
// stay in the menu, so that the menu indices of a saved project do not change: index 0 (ProRes 4444) with an empty
// codecShortName (as in a project saved by an old version, which checkCodec() does not check) and ProRes 422 HQ must
// fail, with an error that names the encoder. Positive control: an encoder of the menu that is not hidden renders.
int testWriteFFmpegMissingEncoder(const std::string& outFile)
{
    std::printf("==== WriteFFmpeg, a Codec menu encoder missing from the FFmpeg build fails the render (issue #388) ====\n");
    av_log_set_level(AV_LOG_ERROR);
    if (avcodec_find_encoder_by_name("prores_ks")) {
        std::printf("  FAIL: run with OFX_IO_TEST_HIDE_ENCODERS=prores_ks\n");
        return 1;
    }
    int failures = 0;
    const std::vector<std::string> shortNames = codecMenuShortNames(outFile);
    if (shortNames.empty()) { std::printf("  FAIL: no Codec menu entry (or no Codec param)\n"); return 1; }
    const long nProRes = std::count_if(shortNames.begin(), shortNames.end(), [](const std::string& n) { return encoderOfShortName(n) == "prores_ks"; });
    if ((nProRes != 6) || (encoderOfShortName(shortNames[0]) != "prores_ks")) {
        std::printf("  FAIL: the Codec menu must start with the 6 ProRes entries (it has %ld, and starts with %s)\n", nProRes, shortNames[0].c_str());
        ++failures;
    }
    // the default Codec (index 0) of a project saved by an old version (empty codecShortName), and ProRes 422 HQ
    for (const char* shortName : { "", "prores_ksapch" }) {
        const std::string label = *shortName ? shortName : "index 0, empty codecShortName";
        const std::string file = outFile + "." + (*shortName ? shortName : "index0") + ".mov";
        std::string error;
        if (!encodeCodecVariant(file, shortName, {}, &error)) {
            std::printf("  %s: FAIL: the render succeeded, encoded with %s\n", label.c_str(), videoStreamSignature(file).c_str());
            ++failures;
        } else if (((error.find("Unable to find codec") == std::string::npos) && (error.find("writer was configured for unavailable codec") == std::string::npos))
                   || (error.find("prores_ks") == std::string::npos)) {
            std::printf("  %s: FAIL: the render failed, but the error does not say that prores_ks is missing: \"%s\"\n", label.c_str(), error.c_str());
            ++failures;
        } else {
            std::printf("  %s: the render failed: %s\n", label.c_str(), error.c_str());
        }
        std::remove(file.c_str());
    }
    // positive control: the first encoder of the menu that is not hidden, in a container that takes it
    std::string control;
    for (const std::string& shortName : shortNames) {
        if ((encoderOfShortName(shortName) != "prores_ks") && !testContainerFor(shortName).empty()) {
            control = shortName;
            break;
        }
    }
    if (control.empty()) {
        std::printf("  FAIL: positive control: no encoder of the Codec menu that is not hidden\n");
        ++failures;
    } else {
        const std::string file = outFile + "." + control + "." + testContainerFor(control);
        std::string error;
        if (encodeCodecVariant(file, control, {}, &error)) {
            std::printf("  %s (positive control): FAIL: %s\n", control.c_str(), error.c_str());
            ++failures;
        } else {
            std::printf("  %s (positive control): rendered, %s\n", control.c_str(), videoStreamSignature(file).c_str());
        }
        std::remove(file.c_str());
    }
    if (!failures) {
        std::printf("  PASS\n");
    }
    return failures;
}

// Issue #291: the renders of the views of a stereo movie (one video stream per view) share the FFmpegFile of the
// instance, and ReadFFmpeg is kOfxImageEffectRenderFullySafe. ReadFFmpeg selected the stream of the view, then decoded
// it in a separate call: a concurrent render of the other view could select its own stream in between, and the render
// got the other view (or a mix of both streams). Also, the streams share the demuxer, which drops the packets of the
// other streams: a decode of a view after the other one, without a seek, missed packets (even on one thread). The
// ffmpeg CLI writes a movie with two matching video streams (MPEG-4, with B-frames), the second one the negative of the
// first; one instance renders every frame of each view on one thread (the references), then both views of each frame
// and a random order of frames and views on one thread, then on kThreads threads at once, in a random order of frames
// and views (with seeks): each render must give the reference of its view and frame, exactly. ThreadSanitizer
// (ofx_render_test_tsan) also reports the race.
int testReadFFmpegViewsConcurrent(const std::string& outFile)
{
    const int W = 64, H = 48, N = 20, kViews = 2, kThreads = 8, kRenders = 40;
    std::printf("==== ReadFFmpeg, a movie with one video stream per view: concurrent renders of both views get their own view (issue #291) ====\n");
    // (double quotes: also for cmd.exe, which std::system() uses on MS Windows)
    const std::string src = "testsrc2=s=" + std::to_string(W) + "x" + std::to_string(H) + ":r=25";
    const std::string cmd = "ffmpeg -y -loglevel error -f lavfi -i \"" + src + "\" -f lavfi -i \"" + src + ",negate\" -map 0:v -map 1:v -frames:v "
        + std::to_string(N) + " -c:v mpeg4 -q:v 2 -g 8 -bf 2 -pix_fmt yuv420p \"" + outFile + "\"";
    if (std::system(cmd.c_str()) != 0) { std::printf("  FAIL: %s\n", cmd.c_str()); return 1; }

    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadFFmpeg");
    if (!p) { std::printf("  FAIL: ReadFFmpeg not found\n"); return 1; }
    // a stereo project (restored on return)
    struct ViewCount {
        ViewCount() { gViewCount = 2; }
        ~ViewCount() { gViewCount = 1; }
    } viewCount;
    Effect ctx;
    if (loadAndDescribe(p, ctx, ctx, kOfxImageEffectContextReader) != 0) { std::printf("  FAIL: load/describe\n"); return 1; }
    setOverrideString(&ctx, kOfxImageEffectFileParamName, outFile);
    int failures = 0;
    {
        Effect inst;
        inst.delegate = &ctx;
        setInstanceProps(inst, W, H, N, kOfxImageEffectContextReader);
        const OfxImageEffectHandle handle = reinterpret_cast<OfxImageEffectHandle>(&inst);
        if (!ok(p->mainEntry(kOfxActionCreateInstance, handle, nullptr, nullptr))) { std::printf("  FAIL: CreateInstance\n"); return 1; }
        {
            PropSet a; OfxPropertySetHandle ap = PH(&a);
            pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
            pSetString(ap, kOfxPropName, 0, kOfxImageEffectFileParamName);
            pSetString(ap, kOfxPropChangeReason, 0, kOfxChangeUserEdited);
            pSetDouble(ap, kOfxPropTime, 0, 1.);
            double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
            p->mainEntry(kOfxActionInstanceChanged, handle, PH(&a), nullptr);
        }
        Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
        if (!out) { std::printf("  FAIL: no Output clip\n"); return 1; }
        out->connected = true;
        setupClipImage(out, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
        { PropSet outArgs; if (!ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, handle, nullptr, PH(&outArgs)))) { std::printf("  FAIL: getClipPreferences\n"); return 1; } }
        const size_t nValues = (size_t)W * H * 3;
        // renders view v of frame t into img (an image of its own, see tThreadImage); kOfxStatOK if it succeeded
        auto render = [&](Clip& img, int t, int v) -> OfxStatus {
            float* fp = reinterpret_cast<float*>(img.buffer.data());
            fillUnwritten(fp, nValues);
            const ThreadImageGuard imageGuard(&img.image);
            PropSet a = makeRenderInArgs((double)t, W, H);
            pSetInt(PH(&a), kFnOfxImageEffectPropView, 0, v);
            return p->mainEntry(kOfxImageEffectActionRender, handle, PH(&a), nullptr);
        };
        // the references, rendered on this thread: every frame of view 0, then of view 1
        std::vector<std::vector<uint8_t>> refs((size_t)kViews * N);
        Clip img;
        setupClipImage(&img, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
        for (int v = 0; v < kViews; ++v) {
            for (int t = 1; t <= N; ++t) {
                const OfxStatus st = render(img, t, v);
                if (!ok(st)) { std::printf("  FAIL: render of view %d, frame %d = %d\n", v, t, st); ++failures; }
                refs[(size_t)(t - 1) * kViews + v] = img.buffer;
            }
        }
        // view 1 is the negative of view 0 (within the Y'CbCr rounding): the views are their own streams
        int notNegative = 0, unwritten = 0;
        for (int t = 1; (t <= N) && !failures; ++t) {
            const float* v0 = reinterpret_cast<const float*>(refs[(size_t)(t - 1) * kViews].data());
            const float* v1 = reinterpret_cast<const float*>(refs[(size_t)(t - 1) * kViews + 1].data());
            double sum = 0., diff = 0.;
            for (size_t i = 0; i < nValues; ++i) {
                if (isUnwritten(v0 + i) || isUnwritten(v1 + i)) { ++unwritten; continue; }
                sum += std::fabs(v0[i] + v1[i] - 1.);
                diff += std::fabs(v0[i] - v1[i]);
            }
            if ((sum / nValues > 0.05) || (diff / nValues < 0.2)) {
                std::printf("  FAIL: frame %d: view 1 must be the negative of view 0 (mean |v0 + v1 - 1| = %.3f, mean |v0 - v1| = %.3f)\n", t, sum / nValues, diff / nValues);
                ++notNegative;
            }
        }
        if (unwritten) { std::printf("  FAIL: %d values not written by the references\n", unwritten); ++failures; }
        failures += notNegative;
        const bool refsOk = !failures; // (the other passes compare with the references)
        if (refsOk) {
            std::printf("  PASS: views 0 and 1 of %d frames, rendered on one thread: view 1 is the negative of view 0\n", N);
            // on one thread, both views of each frame, as a host renders a stereo movie, then a random order of frames
            // and views: the demuxer is shared by the streams, so a decode of a view after the other one (without a
            // seek) used to miss the packets that the other one read
            std::vector<std::pair<int, int>> order; // (frame, view)
            for (int t = 1; t <= N; ++t) {
                for (int v = 0; v < kViews; ++v) {
                    order.emplace_back(t, v);
                }
            }
            uint32_t seed = 4321u;
            auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }; // (a fixed sequence)
            for (int n = 0; n < 4 * kRenders; ++n) {
                order.emplace_back(1 + (int)(next() % N), (int)(next() % kViews));
            }
            int wrong = 0;
            for (size_t n = 0; n < order.size(); ++n) {
                const int t = order[n].first, v = order[n].second;
                if (!ok(render(img, t, v)) || (img.buffer != refs[(size_t)(t - 1) * kViews + v])) {
                    std::printf("  FAIL: one thread, render %zu: view %d of frame %d is wrong\n", n + 1, v, t);
                    ++wrong;
                }
            }
            if (wrong) {
                ++failures;
            } else {
                std::printf("  PASS: one thread, both views of each frame, then %d renders in a random order\n", 4 * kRenders);
            }
        }
        // the concurrent renders
        std::atomic<int> renderFailures(0), wrongView(0), wrongImage(0), started(0);
        std::vector<std::thread> threads;
        for (int k = 0; (k < kThreads) && refsOk; ++k) {
            threads.emplace_back([&, k]() {
                Clip img;
                setupClipImage(&img, W, H, kOfxImageComponentRGB, kOfxBitDepthFloat, true);
                uint32_t seed = 12345u + 977u * (uint32_t)k;
                auto next = [&seed]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; }; // (a fixed sequence)
                ++started;
                while (started < kThreads) {
                    std::this_thread::yield();
                }
                for (int n = 0; n < kRenders; ++n) {
                    const int t = 1 + (int)(next() % N), v = (int)(next() % kViews);
                    if (!ok(render(img, t, v))) {
                        ++renderFailures;
                    } else if (img.buffer != refs[(size_t)(t - 1) * kViews + v]) {
                        ++(img.buffer == refs[(size_t)(t - 1) * kViews + (1 - v)] ? wrongView : wrongImage);
                    }
                }
            });
        }
        for (std::thread& th : threads) {
            th.join();
        }
        if (!threads.empty()) {
            std::printf("  %d threads x %d renders: %d failed, %d got the other view, %d another image\n", kThreads, kRenders, (int)renderFailures,
                        (int)wrongView, (int)wrongImage);
            if (renderFailures || wrongView || wrongImage) {
                std::printf("  FAIL: each concurrent render must give the image of its view and frame (issue #291)\n");
                ++failures;
            } else {
                std::printf("  PASS: each concurrent render gave the image of its view and frame\n");
            }
        }
        p->mainEntry(kOfxActionDestroyInstance, handle, nullptr, nullptr);
    }
    p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    return failures;
}

int
main(int argc, char** argv)
{
    initHost();
    const std::string outFile = (argc > 1) ? argv[1] : "/tmp/iotest/ofx_render_out.mov";
    if ((argc > 2) && (std::string(argv[2]) == "--dnxhr444-noise")) {
        const int failures = testDNxHR444Noise(outFile);
        std::printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
        return failures ? 1 : 0;
    }
    if ((argc > 2) && (std::string(argv[2]).compare(0, 2, "--") == 0) && (std::string(argv[2]) != "--dnxhr444-noise")) {
        // a crash or a deadlock must not lose the output
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        const std::string mode = argv[2];
        int failures = 0;
        if (mode == "--begin-failure") {
            failures = testWriteFFmpegBeginFailures(outFile);
        } else if (mode == "--dnxhd-profiles") {
            std::printf("==== WriteFFmpeg DNxHD/DNxHR Codec Profiles, 1080p and 720p at 23.976 to 60 fps: the expected CID, or a clear error (issue #305) ====\n");
            failures = testDNxHDProfiles(outFile);
            std::printf("==== WriteFFmpeg DNxHR below 256x120 (FFmpeg's limit): a clear error; 256x120 encodes (issue #248) ====\n");
            failures += testDNxHRMinSize(outFile);
        } else if (mode == "--encode-failure") {
            failures = testWriteFFmpegEncodeFailures(outFile);
        } else if (mode == "--encode-failure-disk-full") {
            failures = testWriteFFmpegEncodeFailureDiskFull(outFile);
        } else if (mode == "--writeoiio-disk-full") {
            failures = testWriteOIIODiskFull(outFile);
        } else if (mode == "--writeoiio-files") {
            failures = testWriteOIIOFiles(outFile) + testWriteOIIOEXRZstd(outFile);
        } else if (mode == "--writer-remove-partial-file") {
            failures = testWriterRemovePartialFile(outFile);
        } else if (mode == "--swscale-exact") {
            failures = testSwscaleExact(outFile);
        } else if (mode == "--param-change") {
            failures = testWriteFFmpegParamChangeDuringRender(outFile);
        } else if (mode == "--codec-edit-during-begin") {
            failures = testWriteFFmpegCodecEditDuringBegin(outFile);
        } else if (mode == "--codec-params") {
            failures = testWriteFFmpegCodecParams(outFile);
        } else if (mode == "--pixfmt-choice") {
            failures = testWriteFFmpegPixelFormatChoice(outFile);
        } else if (mode == "--selected-encoder") {
            failures = testWriteFFmpegSelectedEncoder(outFile);
        } else if (mode == "--missing-encoder") {
            failures = testWriteFFmpegMissingEncoder(outFile);
        } else if (mode == "--destroy-closes-file") {
            failures = testReadFFmpegDestroyClosesFile(outFile);
        } else if (mode == "--pfm-write-errors") {
            failures = testWritePFMDiskFull(outFile);
        } else if (mode == "--pfm-read-errors") {
            failures = testReadPFMHeaderSize(outFile);
        } else if (mode == "--views-concurrent") {
            failures = testReadFFmpegViewsConcurrent(outFile);
        } else {
            std::printf("unknown option %s\n", mode.c_str());
            return 2;
        }
        std::printf(failures ? "FAILED: %d\n" : "ALL PASSED\n", failures);
        return failures ? 1 : 0;
    }
    const int W = 128, H = 128, N = 10;

    int failures = 0;
    // first, while the heap is fresh: the uninitialized dts of libx265 was more often garbage then (issue #217)
    std::printf("==== WriteFFmpeg libx265 MP4 at 128x96 with 1 or 2 frames, 20 times each: encodes (issue #217) ====\n");
    failures += testWriteFFmpegX265FewFrames(outFile + ".i217.mp4");
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
    // issue #339: H.264 and MPEG-2 with B-frames in AVI, which has no PTS, are written and read back
    for (const char* codec : { "libx264", "mpeg2video" }) {
        if (!avcodec_find_encoder_by_name(codec)) {
            std::printf("==== WriteFFmpeg avi, %s: SKIP, no encoder (issue #339) ====\n", codec);
            continue;
        }
        WriterParams wp;
        wp.strings = { { "codecShortName", codec } };
        wp.ints = { { "bFrames", 3 } };
        std::printf("==== WriteFFmpeg avi, %s with B-frames, read back by ReadFFmpeg (issue #339) ====\n", codec);
        const std::string aviFile = outFile + ".i339.avi";
        failures += testWriteFFmpeg(aviFile, W, H, N, wp) || testReadFFmpeg(aviFile, W, H, N);
        std::remove(aviFile.c_str());
    }
    {
        // Matroska stores timestamps in milliseconds: at 24 fps, every frame of the movie must be decoded
        WriterParams wp;
        wp.fps = 24.;
        std::printf("==== WriteFFmpeg + ReadFFmpeg, Matroska at 24 fps: every frame is decoded ====\n");
        failures += testWriteFFmpeg(outFile + ".24fps.mkv", W, H, N, wp) || testReadFFmpeg(outFile + ".24fps.mkv", W, H, N);
    }
    // issue #247: libaom-av1 gives each packet a duration of one frame in the codec time base, which was written as
    // is: the last frame lasted one tick of the stream time base (1 ms in Matroska, where ReadFFmpeg then lost it).
    // The movie must last N frames, and ReadFFmpeg must read all of them.
    for (const char* ext : { "mkv", "mp4" }) {
        const AVCodec* c = avcodec_find_encoder_by_name("libaom-av1");
        if (!c || !muxerTakesEncoder(ext, c)) {
            std::printf("==== WriteFFmpeg + ReadFFmpeg, libaom-av1 in %s at 24 fps: SKIP, no encoder or not in this container (issue #247) ====\n", ext);
            continue;
        }
        WriterParams wp;
        wp.fps = 24.;
        wp.strings = { { "codecShortName", "libaom-av1" } };
        std::printf("==== WriteFFmpeg + ReadFFmpeg, libaom-av1 in %s at 24 fps: the movie lasts %d frames, and every frame is decoded (issue #247) ====\n", ext, N);
        const std::string file = outFile + ".i247." + ext;
        if (testWriteFFmpeg(file, W, H, N, wp)) {
            ++failures;
        } else {
            const double duration = videoStreamDuration(file);
            const double expected = N / wp.fps;
            if (std::fabs(duration - expected) > 0.5 / wp.fps) {
                std::printf("  FAIL: the movie lasts %g s, expected %g s\n", duration, expected);
                ++failures;
            } else {
                std::printf("  PASS (the movie lasts %g s)\n", duration);
            }
            failures += testReadFFmpeg(file, W, H, N);
        }
        std::remove(file.c_str());
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
        // MPEG-4 part 2 can not have a time base denominator above 65535: the frame rate of the video is
        // reduced, as the ffmpeg CLI does (40999/342 for 120000/1001), drop-frame is still valid, and the
        // tmcd track is at 120000/1001 with FFmpeg >= 9.1 (also non-drop-frame: with the reduced rate as the
        // stream's avg_frame_rate, FFmpeg 9.1 would write it at 120/1), or at 40999/342 with FFmpeg < 9.1
        const struct { const char* tc; const char* expected; } mp4vCases[] = {
            { "01:00:00;00", "01:00:00;000" },
            { "01:00:00:00", "01:00:00:000" },
        };
        for (const auto& c : mp4vCases) {
            std::printf("==== WriteFFmpeg mov, MPEG-4 part 2 at 119.88 fps, start timecode \"%s\": close to 120000/1001 fps (issue #110) ====\n", c.tc);
            WriterParams wp;
            wp.strings = { { "codecShortName", "mpeg4" } };
            const int f = testStartTimecode(outFile + ".i110.mp4v.mov", c.tc, 119.88, c.expected, wp);
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
        // The primaries are those of the OCIO output colorspace (the default, rec709 in test.ocio), not those of
        // SD video (SMPTE C).
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
            e.primaries = AVCOL_PRI_BT709;
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

    {
        // The color tags and Y'CbCr matrix of WriteFFmpeg, and the ReadFFmpeg round trip (Auto matrix and range)
        const double k709[2] = { 0.2126, 0.0722 }, k601[2] = { 0.299, 0.114 }, k2020[2] = { 0.2627, 0.0593 };
        const ColorRoundTrip cases[] = {
            { "H.264, SD, rec709", "libx264", ".mp4", "rec709", 128, 96, AVCOL_SPC_SMPTE170M, AVCOL_PRI_BT709, AVCOL_TRC_BT709, false, k601[0], k601[1], 8, nullptr },
            { "H.264, 720p, rec709", "libx264", ".mp4", "rec709", 1280, 720, AVCOL_SPC_BT709, AVCOL_PRI_BT709, AVCOL_TRC_BT709, false, k709[0], k709[1], 8, nullptr },
            { "H.264, SD, Rec2020", "libx264", ".mov", "Rec2020", 128, 96, AVCOL_SPC_BT2020_NCL, AVCOL_PRI_BT2020, AVCOL_TRC_BT2020_12, false, k2020[0], k2020[1], 8, nullptr },
            { "H.264, SD, sRGB", "libx264", ".mp4", "sRGB", 128, 96, AVCOL_SPC_SMPTE170M, AVCOL_PRI_BT709, AVCOL_TRC_IEC61966_2_1, false, k601[0], k601[1], 8, nullptr },
            { "ProRes 4444, SD, Rec2020", "prores_ksap4h", ".mov", "Rec2020", 128, 96, AVCOL_SPC_BT2020_NCL, AVCOL_PRI_BT2020, AVCOL_TRC_BT709, true, k2020[0], k2020[1], 12, nullptr },
            { "ProRes 422 HQ, 720p, rec709", "prores_ksapch", ".mov", "rec709", 1280, 720, AVCOL_SPC_BT709, AVCOL_PRI_BT709, AVCOL_TRC_BT709, true, k709[0], k709[1], 10, nullptr },
            { "ProRes 422 HQ, SD, sRGB (unspecified transfer function in ProRes)", "prores_ksapch", ".mov", "sRGB", 128, 96, AVCOL_SPC_SMPTE170M, AVCOL_PRI_BT709, AVCOL_TRC_UNSPECIFIED, true, k601[0], k601[1], 10, nullptr },
            { "QuickTime Animation (RGB), sRGB", "qtrle", ".mov", "sRGB", 128, 96, AVCOL_SPC_RGB, AVCOL_PRI_BT709, AVCOL_TRC_IEC61966_2_1, false, -1., -1., 8, nullptr },
            { "H.264 in Matroska, SD, sRGB: colorspace metadata", "libx264", ".mkv", "sRGB", 128, 96, AVCOL_SPC_SMPTE170M, AVCOL_PRI_BT709, AVCOL_TRC_IEC61966_2_1, false, k601[0], k601[1], 8, "sRGB" },
            { "H.264 in Matroska, SD, Gamma2.2: colorspace metadata", "libx264", ".mkv", "Gamma2.2", 128, 96, AVCOL_SPC_SMPTE170M, AVCOL_PRI_BT709, AVCOL_TRC_GAMMA22, false, k601[0], k601[1], 8, "Gamma2.2" },
            // DNxHD/DNxHR: FFmpeg's decoder takes the matrix of every frame from the frame header, where FFmpeg's
            // encoder always writes BT.709, so the BT.709 matrix, with the BT.2020 primaries and in SD too
            { "DNxHR HQX, 720p, Rec2020: BT.709 matrix", "dnxhd", ".mov", "Rec2020", 1280, 720, AVCOL_SPC_BT709, AVCOL_PRI_BT2020, -1, false, k709[0], k709[1], 10, nullptr, 1 },
            { "DNxHR HQ, 720p, Rec2020: BT.709 matrix", "dnxhd", ".mov", "Rec2020", 1280, 720, AVCOL_SPC_BT709, AVCOL_PRI_BT2020, -1, false, k709[0], k709[1], 8, nullptr, 2 },
            { "DNxHR HQ, SD, rec709: BT.709 matrix", "dnxhd", ".mov", "rec709", 256, 144, AVCOL_SPC_BT709, AVCOL_PRI_BT709, AVCOL_TRC_BT709, false, k709[0], k709[1], 8, nullptr, 2 },
            { "DNxHR 444 (RGB), 720p, Rec2020", "dnxhd", ".mov", "Rec2020", 1280, 720, AVCOL_SPC_RGB, AVCOL_PRI_BT2020, -1, false, -1., -1., 10, nullptr, 0 },
        };
        for (const auto& c : cases) {
            std::printf("==== WriteFFmpeg + ReadFFmpeg %s: color tags and Y'CbCr matrix ====\n", c.label);
            const std::string encoder = encoderOfShortName(c.codec);
            if (!avcodec_find_encoder_by_name(encoder.c_str())) {
                std::printf("  SKIP: no %s encoder\n", c.codec);
                continue;
            }
            failures += testWriteFFmpegColorRoundTrip(outFile + ".color", c);
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
        // #170: uncompressed codecs. RGB is exact (the pattern is on the 8-bit grid). Y'CbCr: the rounding of 8-bit or
        // 10-bit video range (8 bits: Y' has 219 levels, Cb and Cr 224), plus the error of the 4:2:2 or 4:2:0 chroma
        // interpolation (measured on FFmpeg 8.0 and 9.0.2: 4:4:4 at most 2/255 in 8 bits and 0.5/255 in 10 bits, 4:2:2
        // 2/255 in 8 bits and 1.3/255 in 10 bits, 4:2:0 3/255, 4/255 at an odd width). The tolerances of 4:2:2 and
        // 4:2:0 were 6/255, 3/255 (10 bits) and 8/255 when ReadFFmpeg's conversion to 8-bit RGB had a bias, and
        // WriteFFmpeg point-sampled the chroma (#207: the errors were 5/255, 2.1/255 and 7/255). A wrong range would
        // give about 16/255 in black and white, and a wrong matrix more than 10/255 in the saturated corners of the
        // pattern.
        // With values on no grid (quant 0): 8-bit RGB rounds to the nearest of 255 levels (0.5/255), 16-bit RGB is exact
        // within 1e-4 (6.5/65535, much less than the 8-bit rounding).
        const double rgbTol = 1e-4, rgb8Tol = 0.5 / 255. + 1e-5, yuv8Tol = 2.5 / 255., yuv10Tol = 1. / 255., yuv422Tol = 3. / 255.,
                     yuv422p10Tol = 2. / 255., yuv420Tol = 5. / 255.;
        std::vector<UncompressedCase> cases = {
            { "v210 in QuickTime", "mov", "v210", 1, 1, false, "v210", "v210", "yuv422p10le", yuv422p10Tol },
            { "rawvideo 2vuy in QuickTime", "mov", "rawvideo", 1, 0, false, "rawvideo", "2vuy", "uyvy422", yuv422Tol },
            { "rawvideo raw RGB in QuickTime", "mov", "rawvideo", 3, 0, false, "rawvideo", "raw ", "rgb24", rgbTol },
            { "rawvideo raw ARGB in QuickTime", "mov", "rawvideo", 3, 0, true, "rawvideo", "raw ", "argb", rgbTol },
            // 16-bit RGB in QuickTime: b48r (rgb48be) is not written, since QuickTime reads it byte-swapped: 8-bit RGB,
            // with a warning (values on no grid)
            { "rawvideo RGB 16-bit in QuickTime: 8-bit raw RGB", "mov", "rawvideo", 3, 3, false, "rawvideo", "raw ", "rgb24", rgb8Tol,
              "rawvideo has no pixel format with RGB, 16 bits in the mov container", nullptr, 0 },
            { "rawvideo YUV420 8-bit in Matroska", "mkv", "rawvideo", 0, 0, false, "rawvideo", "I420", "yuv420p", yuv420Tol },
            { "rawvideo YUV422 8-bit in Matroska", "mkv", "rawvideo", 1, 0, false, "rawvideo", "UYVY", "uyvy422", yuv422Tol },
            { "rawvideo RGB 16-bit in NUT", "nut", "rawvideo", 3, 3, false, "rawvideo", "RGB0", "rgb48le", rgbTol }, // (48 is '0')
            { "rawvideo YUV422 8-bit in NUT", "nut", "rawvideo", 1, 0, false, "rawvideo", "Y42B", "yuv422p", yuv422Tol },
            { "rawvideo YUVA444 10-bit in NUT", "nut", "rawvideo", 2, 1, true, "rawvideo", "Y4[0][10]", "yuva444p10le", yuv10Tol },
            { "rawvideo RGBA 8-bit in NUT", "nut", "rawvideo", 3, 0, true, "rawvideo", "RGBA", "rgba", rgbTol },
            { "rawvideo BI_RGB in AVI", "avi", "rawvideo", 3, 0, false, "rawvideo", "[0][0][0][0]", "bgr24", rgbTol },
            { "rawvideo UYVY in AVI", "avi", "rawvideo", 1, 0, false, "rawvideo", "UYVY", "uyvy422", yuv422Tol },
        };
        // the mov muxer writes rawvideo with the v308, v408 and v410 tags
        cases.push_back({ "rawvideo v308 in QuickTime", "mov", "rawvideo", 2, 0, false, "rawvideo", "v308", "vyu444", yuv8Tol });
        cases.push_back({ "rawvideo v408 in QuickTime", "mov", "rawvideo", 2, 0, true, "rawvideo", "v408", "uyva", yuv8Tol });
        cases.push_back({ "rawvideo v410 in QuickTime", "mov", "rawvideo", 2, 1, false, "rawvideo", "v410", "v30xle", yuv10Tol });
        // a project saved with the v308 or v410 encoder: with FFmpeg 9, which removed them, rawvideo writes the same tag
        if (avcodec_find_encoder_by_name("v308")) {
            // the real encoders (FFmpeg 8), whose files the mov demuxer gives to rawvideo (libavformat/isom_tags.c:36-38
            // in n8.0)
            cases.push_back({ "v308 encoder in QuickTime", "mov", "v308", 1, 0, false, "rawvideo", "v308", "vyu444", yuv8Tol });
            cases.push_back({ "v410 encoder in QuickTime", "mov", "v410", 1, 0, false, "rawvideo", "v410", "v30xle", yuv10Tol });
        } else {
            // the warning stays after the render (GenericWriterPlugin::render() clears the persistent message)
            cases.push_back({ "saved v308 project, rawvideo in QuickTime", "mov", "v308", 1, 0, false, "rawvideo", "v308", "vyu444", yuv8Tol,
                              "the codec was replaced by rawvideo",
                              "with the v308 tag, as the v308 encoder did" });
            cases.push_back({ "saved v410 project, rawvideo in QuickTime", "mov", "v410", 1, 0, false, "rawvideo", "v410", "v30xle", yuv10Tol,
                              "the codec was replaced by rawvideo",
                              "In the mov container, rawvideo writes the v30xle pixel format with the v410 tag, as the v410 encoder did" });
            // Matroska has no 4:4:4: UYVY, and the warning says so
            cases.push_back({ "saved v308 project, rawvideo in Matroska", "mkv", "v308", 1, 0, false, "rawvideo", "UYVY", "uyvy422", yuv422Tol,
                              "In the matroska container, rawvideo writes the uyvy422 pixel format.",
                              "rawvideo has no pixel format with Y'CbCr 4:4:4, 8 bits in the matroska container" });
        }

        // Values on no grid (as in float, 10-bit and 16-bit images), which show a lower depth than expected (e.g.
        // 16-bit samples read byte-swapped, or written in 8 bits)
        cases.push_back({ "rawvideo RGB 16-bit in NUT, values on no grid", "nut", "rawvideo", 3, 3, false, "rawvideo", "RGB0", "rgb48le", rgbTol,
                          nullptr, nullptr, 0 });
        cases.push_back({ "rawvideo RGBA 16-bit in NUT, values on no grid", "nut", "rawvideo", 3, 3, true, "rawvideo", "RBA[64]", "rgba64le", rgbTol,
                          nullptr, nullptr, 0 });
        cases.push_back({ "rawvideo RGB 10-bit in QuickTime: 8-bit raw RGB", "mov", "rawvideo", 3, 1, false, "rawvideo", "raw ", "rgb24", rgb8Tol,
                          "rawvideo has no pixel format with RGB, 10 bits in the mov container", nullptr, 0 });
        cases.push_back({ "rawvideo raw ARGB 16-bit in QuickTime: 8-bit ARGB", "mov", "rawvideo", 3, 3, true, "rawvideo", "raw ", "argb", rgb8Tol,
                          "rawvideo has no pixel format with RGB, 16 bits and alpha in the mov container", nullptr, 0 });
        cases.push_back({ "v210 in QuickTime, values on no grid", "mov", "v210", 1, 1, false, "v210", "v210", "yuv422p10le", yuv422p10Tol,
                          nullptr, nullptr, 0 });
        cases.push_back({ "rawvideo YUVA444 10-bit in NUT, values on no grid", "nut", "rawvideo", 2, 1, true, "rawvideo", "Y4[0][10]", "yuva444p10le",
                          yuv10Tol, nullptr, nullptr, 0 });
        cases.push_back({ "rawvideo v410 in QuickTime, values on no grid", "mov", "rawvideo", 2, 1, false, "rawvideo", "v410", "v30xle", yuv10Tol,
                          nullptr, nullptr, 0 });
        // Y'CbCr with more than 10 bits in QuickTime: v410 (10-bit 4:4:4), with a warning
        cases.push_back({ "rawvideo YUV444 16-bit in QuickTime: v410", "mov", "rawvideo", 2, 3, false, "rawvideo", "v410", "v30xle", yuv10Tol,
                          "rawvideo has no pixel format with Y'CbCr 4:4:4, 16 bits in the mov container", nullptr, 0 });
        cases.push_back({ "rawvideo YUV422 12-bit in QuickTime: v410", "mov", "rawvideo", 1, 2, false, "rawvideo", "v410", "v30xle", yuv10Tol,
                          "rawvideo has no pixel format with Y'CbCr 4:2:2, 12 bits in the mov container", nullptr, 0 });

        // Odd widths (63x47)
        {
            std::vector<UncompressedCase> odd = {
                // the mov muxer pads the RGB24 rows to an even number of bytes, which FFmpeg and QuickTime do not read:
                // ARGB (opaque)
                { "rawvideo RGB in QuickTime, odd width: ARGB", "mov", "rawvideo", 3, 0, false, "rawvideo", "raw ", "argb", rgbTol },
                { "rawvideo raw ARGB in QuickTime, odd width", "mov", "rawvideo", 3, 0, true, "rawvideo", "raw ", "argb", rgbTol },
                { "rawvideo 2vuy in QuickTime, odd width", "mov", "rawvideo", 1, 0, false, "rawvideo", "2vuy", "uyvy422", yuv422Tol },
                // the v210 encoder needs an even width
                { "v210 in QuickTime, odd width: error", "mov", "v210", 1, 1, false, "v210", "v210", "yuv422p10le", yuv422p10Tol,
                  "The v210 codec needs an even width, and the image is 63 pixels wide" },
                { "rawvideo RGB 16-bit in NUT, odd width", "nut", "rawvideo", 3, 3, false, "rawvideo", "RGB0", "rgb48le", rgbTol },
                { "rawvideo YUV422 8-bit in NUT, odd width", "nut", "rawvideo", 1, 0, false, "rawvideo", "Y42B", "yuv422p", yuv422Tol },
                { "rawvideo YUVA444 10-bit in NUT, odd width", "nut", "rawvideo", 2, 1, true, "rawvideo", "Y4[0][10]", "yuva444p10le", yuv10Tol },
                { "rawvideo BI_RGB in AVI, odd width", "avi", "rawvideo", 3, 0, false, "rawvideo", "[0][0][0][0]", "bgr24", rgbTol },
                { "rawvideo BI_RGB BGRA in AVI, odd width", "avi", "rawvideo", 3, 0, true, "rawvideo", "[0][0][0][0]", "bgra", rgbTol },
                { "rawvideo UYVY in AVI, odd width", "avi", "rawvideo", 1, 0, false, "rawvideo", "UYVY", "uyvy422", yuv422Tol },
                { "rawvideo YUV420 8-bit in Matroska, odd width", "mkv", "rawvideo", 0, 0, false, "rawvideo", "I420", "yuv420p", yuv420Tol },
                { "rawvideo v410 in QuickTime, odd width", "mov", "rawvideo", 2, 1, false, "rawvideo", "v410", "v30xle", yuv10Tol },
                { "rawvideo v308 in QuickTime, odd width", "mov", "rawvideo", 2, 0, false, "rawvideo", "v308", "vyu444", yuv8Tol },
                { "rawvideo v408 in QuickTime, odd width", "mov", "rawvideo", 2, 0, true, "rawvideo", "v408", "uyva", yuv8Tol },
            };
            for (UncompressedCase c : odd) {
                c.W = 63;
                c.H = 47;
                c.expectBeginFailure = (std::string(c.codec) == "v210");
                cases.push_back(c);
            }
        }
        for (size_t i = 0; i < cases.size(); ++i) {
            std::printf("==== WriteFFmpeg %s, read back by ReadFFmpeg (issue #170) ====\n", cases[i].label);
            failures += testUncompressed(outFile + ".i170-" + std::to_string(i) + "." + cases[i].ext, cases[i]);
        }

        // A frame fails after a good one, while a warning is set (rawvideo RGB 16-bit in QuickTime writes 8-bit RGB):
        // endSequenceRender must not replace the error by the warning.
        const std::pair<const char*, bool> lateFailures[] = { { "another file from frame 2 (encode())", true },
                                                              { "the output image can not be fetched from frame 2 (render())", false } };
        for (const auto& lf : lateFailures) {
            std::printf("==== WriteFFmpeg rawvideo RGB 16-bit in QuickTime (warning), %s: the error stays (issue #170) ====\n", lf.first);
            WriterParams wp;
            wp.strings = { { "codecShortName", "rawvideo" } };
            wp.ints = { { "prefPixelCoding", 3 }, { "prefBitDepth", 3 } };
            wp.expectRenderFailure = true;
            if (lf.second) {
                wp.renderFile = outFile + ".i170-late-other.mov";
            } else {
                wp.outputFetchFails = true;
                wp.renderErrorText = "Output image could not be fetched";
            }
            gMessages.clear();
            const int f = testWriteFFmpeg(outFile + ".i170-late.mov", 64, 48, 3, wp);
            if (!f && (gMessages.find("rawvideo has no pixel format with RGB, 16 bits") == std::string::npos)) {
                std::printf("  FAIL: no fallback warning in:\n%s", gMessages.c_str());
                ++failures;
            }
            failures += f;
            std::remove((outFile + ".i170-late.mov").c_str());
            std::remove((outFile + ".i170-late-other.mov").c_str());
        }
    }

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
