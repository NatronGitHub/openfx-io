/*
 * ffmpeg_read_test — standalone command-line harness for the openfx-io FFmpeg
 * reader core (FFmpeg/FFmpegFile.cpp), exercised WITHOUT an OpenFX host.
 *
 * Purpose: validate the reader fixes (frame->PTS seeking arithmetic and the
 * send/drain decode loop) with real media, in an environment where the full
 * OFX plugin cannot be built.
 *
 * It links FFmpegFile.cpp directly and:
 *   1. opens a file, prints stream info,
 *   2. decodes every frame sequentially, recording a per-frame signature
 *      (mean of the first component, on a 0-255 scale) — the "ground truth",
 *   3. re-decodes frames in a seek-forcing order (reverse + jumps) and checks
 *      each seeked frame's signature matches the sequential one,
 *   4. optionally checks the sequential signatures are strictly monotonic
 *      (for the generated gray-ramp clips), proving sequential frames are distinct,
 *   5. optionally (--ramp OFFSET,STEP) checks that every decoded frame, sequential
 *      or seeked, holds the expected content: the generated clips put the frame
 *      number in the pixels, so frame f (1-based) must have a signature of
 *      OFFSET + STEP * (f - 1), within 0.4 * STEP. This catches a wrong frame as
 *      well as a missing one,
 *   6. optionally (--frames N) checks the number of frames reported by getInfo(),
 *   7. optionally (--positions P0,P1,...) checks a variable frame rate clip with
 *      timestamp gaps: ramp frame i (0-based) is at frame Pi (0-based, increasing),
 *      and a requested frame in a gap must give the frame displayed at that time,
 *      i.e. the last ramp frame at or before it (held until the next one, or until
 *      the end): with --ramp, its content is checked, and in any case a frame in a
 *      gap must be identical to the held frame, decoded at its own frame,
 *   8. optionally (--max-seeks N) checks that decoding every frame sequentially seeks at
 *      most N times (FFmpegFile::getSeekCount()): e.g. a frame held in a long gap must
 *      not make every frame of the gap decode from the start again,
 *   9. optionally (--no-seek-check F1,F2,...) does not require that the 1-based frames
 *      Fi give the same image after a seek as sequentially (they must still decode):
 *      with broken timestamps, the timestamps the decoder gives may depend on what it
 *      decoded before (see run_ffmpeg_read_test.sh),
 *  10. optionally (--max-open-bytes N) checks that opening the file read at most N bytes
 *      (FFmpegFile::getBytesReadAtOpen()), e.g. that finding the end of the movie does not read
 *      the whole file, and (--open-only) stops after the checks of the opened file,
 *  11. optionally (--seek-bytes F,N) checks that decoding the 1-based frame F right after the
 *      open reads at most N bytes (e.g. that the end of the movie found at open does not make
 *      the seeks decode from the start), and (--max-indexed-frame N) that the index of the stream
 *      does not reach beyond the 1-based frame N after the open (e.g. that reading the last packets
 *      to find the end did not add an index entry near the end, which would make the seeks decode
 *      from near the start, in FLV without a keyframe index),
 *  12. optionally (--fail F1,F2,...) checks that the 1-based frames Fi fail to decode, with
 *      an error message, sequentially and after a seek (e.g. a damaged first GOP), and
 *      excludes them from the other checks; every other frame must still decode. As in the
 *      plug-in, a failed frame must not make the file invalid (FFmpegFile::isInvalid()), and it
 *      must have an error message,
 *  13. optionally (--gaps F1,F2,...) checks a clip where the 1-based frames Fi have no frame
 *      (e.g. their packets were damaged and dropped): as --positions, each one must give
 *      the last frame before it that is not in a gap (with --ramp, the ramp content of
 *      that frame, i.e. OFFSET + STEP * (its frame - 1)),
 *  14. optionally (--concealed F1,F2,...) does not check the content of the 1-based frames
 *      Fi, nor that they give the same image after a seek (they must still decode): e.g.
 *      frames that reference a dropped frame, which the decoder conceals,
 *  15. optionally (--fresh F1,F2,...) decodes each 1-based frame Fi first in a newly opened
 *      file (e.g. a seek into a run of damaged frames), and checks it against the sequential
 *      decode (and, with --ramp, its content), or that it fails, for a --fail frame,
 *  16. optionally (--max-seq-bytes N) checks that decoding every frame sequentially reads at most N
 *      bytes (e.g. a file damaged from some point on must not be read again for each frame), and
 *      (--max-reverse-bytes N) that decoding every frame in reverse order, in a newly opened file,
 *      reads at most N bytes,
 *  17. optionally (--order F1,F2,...) decodes the 1-based frames Fi in that order in a newly opened
 *      file, and checks each one against the sequential decode (or that it fails, for a --fail
 *      frame): e.g. a render order that must not make a good frame fail,
 *  18. optionally (--reference CLEAN --reference-frames F1,F2,...) only checks that the 1-based
 *      frames Fi give exactly the image of the same frame of the file CLEAN (e.g. the undamaged
 *      file), whatever the other frames give: every frame is decoded sequentially, in reverse
 *      order, in a random order, and in the --order frames, each in a newly opened file, and each
 *      frame in its own newly opened file
 *      (e.g. the intact GOPs of a damaged file must decode as in the clean file, in any order),
 *  19. optionally (--matrix N, --range N) sets the Y'CbCr matrix (0 = Auto, 1 = Rec. 601, 2 = Rec. 709, 3 = Rec. 2020)
 *      and range (0 = Auto, 1 = Video, 2 = Full) of the conversion to RGB (FFmpegFile::setColorOverrides(), the
 *      ReadFFmpeg "YCbCr Matrix" and "YCbCr Range" params), and (--rgb R,G,B[,TOL]) checks that the mean of each
 *      component of the first and the last frame is R, G and B (on a 0-255 scale) within TOL (default 1.5): e.g. a
 *      clip of a constant Y'CbCr color, whose RGB value is computed from its matrix and range by the script.
 *  20. optionally (--bit-depth N) checks that the reader reports a bit depth of N (FFmpegFile::getBitDepth()), and
 *      (--fine-ramp MAX_STEP,MIN_LEVELS) that the first frame, a horizontal gray ramp with one code value per column
 *      (e.g. 10-bit luma 64 + X), is decoded without the steps of a lower bit depth: the means of the first
 *      component of adjacent columns (on a 0-255 scale) differ by at most MAX_STEP, and the first component takes
 *      at least MIN_LEVELS distinct values (more than 256 needs more than 8 bits; issue #213). With
 *      --skip-without-decoder, the harness exits with status 77 (skipped) instead of failing if the libavcodec it
 *      links has no decoder for the stream (e.g. no libdav1d for AV1: the native av1 decoder needs a hwaccel).
 *
 * A wrong seek target (e.g. the old frameToPts operator-precedence bug that
 * dropped the stream start PTS) makes seeked frames land on the wrong frame,
 * which this harness detects as a signature mismatch.
 *
 * Usage: ffmpeg_read_test <file> [--tol N] [--monotonic] [--nearest] [--ramp OFFSET,STEP] [--frames N] [--positions P0,P1,...] [--max-seeks N] [--no-seek-check F1,F2,...] [--max-open-bytes N] [--open-only] [--seek-bytes F,N] [--max-indexed-frame N] [--fail F1,F2,...] [--gaps F1,F2,...] [--concealed F1,F2,...] [--fresh F1,F2,...] [--max-seq-bytes N] [--max-reverse-bytes N] [--order F1,F2,...] [--reference CLEAN --reference-frames F1,F2,...] [--matrix N] [--range N] [--rgb R,G,B[,TOL]] [--bit-depth N] [--fine-ramp MAX_STEP,MIN_LEVELS] [--skip-without-decoder] [--verbose]
 * Exit status: 0 = all checks passed, 77 = skipped (--skip-without-decoder), other = failure.
 */

#include "FFmpegFile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace {

double
meanComponent(const unsigned char* buf, int w, int h, int nComps, int sizeOfData, int comp, double scale16)
{
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (n == 0) {
        return 0.0;
    }
    double sum = 0.0;
    if (sizeOfData == 2) {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(buf);
        for (size_t i = 0; i < n; ++i) {
            sum += p[i * nComps + comp];
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            sum += buf[i * nComps + comp];
        }
    }
    // on a 0-255 scale, whatever the bit depth, so that --tol, --ramp and --rgb do not depend on it (16 bits are
    // divided by scale16)
    return sum / static_cast<double>(n) / (sizeOfData == 2 ? scale16 : 1.0);
}

double
meanFirstComponent(const unsigned char* buf, int w, int h, int nComps, int sizeOfData)
{
    return meanComponent(buf, w, h, nComps, sizeOfData, 0, 256.0);
}

// Exit status of a skipped check (--skip-without-decoder), as in automake and ctest's SKIP_RETURN_CODE.
constexpr int kExitSkipped = 77;

// --skip-without-decoder, after FFmpegFile failed to open the file: whether the linked libavcodec has a decoder for
// none of its video streams (avcodec_find_decoder() fails, or the decoder could not tell the pixel format, e.g. a
// Bayer FFV1 stream for the FFV1 decoder of FFmpeg 8). False if libavformat cannot open the file (a real failure).
bool
hasNoVideoDecoder(const std::string& filename)
{
    AVFormatContext* ctx = nullptr;
    if (avformat_open_input(&ctx, filename.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    const auto close = [](AVFormatContext* c) { avformat_close_input(&c); };
    const std::unique_ptr<AVFormatContext, decltype(close)> guard(ctx, close);
    if (avformat_find_stream_info(ctx, nullptr) < 0) {
        return false;
    }
    bool video = false;
    for (unsigned i = 0; i < ctx->nb_streams; ++i) {
        const AVCodecParameters* par = ctx->streams[i]->codecpar;
        if (par->codec_type != AVMEDIA_TYPE_VIDEO) {
            continue;
        }
        video = true;
        if ((avcodec_find_decoder(par->codec_id) != nullptr) && (par->format != AV_PIX_FMT_NONE)) {
            return false;
        }
    }

    return video;
}

// --skip-without-decoder, after a decode failed: whether the error is AVERROR(ENOSYS), which a decoder that is not
// implemented returns (e.g. the native av1 decoder without a hwaccel, when libavcodec has no libdav1d).
bool
isNotImplementedError(const std::string& error)
{
    char enosys[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(AVERROR(ENOSYS), enosys, sizeof(enosys));

    return error.find(enosys) != std::string::npos;
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file> [--tol N] [--monotonic] [--nearest] [--ramp OFFSET,STEP] [--frames N] [--positions P0,P1,...] [--max-seeks N] [--no-seek-check F1,F2,...] [--max-open-bytes N] [--open-only] [--seek-bytes F,N] [--max-indexed-frame N] [--fail F1,F2,...] [--gaps F1,F2,...] [--concealed F1,F2,...] [--fresh F1,F2,...] [--max-seq-bytes N] [--max-reverse-bytes N] [--order F1,F2,...] [--reference CLEAN --reference-frames F1,F2,...] [--matrix N] [--range N] [--rgb R,G,B[,TOL]] [--bit-depth N] [--fine-ramp MAX_STEP,MIN_LEVELS] [--skip-without-decoder] [--verbose]\n", argv[0]);
        return 2;
    }
    const std::string filename = argv[1];
    double tol = 2.0;
    bool checkMonotonic = false;
    bool loadNearest = false;
    double rampOffset = 0.0, rampStep = 0.0; // if rampStep > 0, frame f must have signature rampOffset + rampStep * (f - 1)
    int expectedFrames = -1;
    long maxSeeks = -1;
    long long maxOpenBytes = -1;
    bool openOnly = false;
    int seekBytesFrame = -1;
    int maxIndexedFrame = -1;
    long long maxSeekBytes = -1;
    std::vector<int> noSeekCheck; // 1-based frames that may give another image after a seek
    std::vector<int> mustFail; // 1-based frames that must fail to decode
    std::vector<int> gaps; // 1-based frames that have no frame (--gaps)
    std::vector<int> concealed; // 1-based frames whose content and seek consistency are not checked
    std::vector<int> fresh; // 1-based frames to decode first in a newly opened file
    long long maxSeqBytes = -1;
    long long maxReverseBytes = -1;
    std::vector<int> renderOrder; // 1-based frames to decode in that order in a newly opened file
    std::string referenceFile; // --reference: the clean file
    std::vector<int> referenceFrames; // --reference-frames: the 1-based frames that must be those of the clean file
    std::vector<int> positions; // if not empty, ramp frame i (0-based) is at frame positions[i] (0-based)
    bool verbose = false;
    int colorMatrix = 0; // --matrix: FFmpegFile::ColorMatrixEnum
    int colorRange = 0; // --range: FFmpegFile::ColorRangeEnum
    double expectedRgb[3] = { -1., -1., -1. }; // --rgb
    double rgbTol = 1.5;
    int expectedBitDepth = 0; // --bit-depth
    double fineRampMaxStep = -1.; // --fine-ramp
    int fineRampMinLevels = 0;
    bool skipWithoutDecoder = false; // --skip-without-decoder
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--matrix" && i + 1 < argc) {
            colorMatrix = std::atoi(argv[++i]);
            if (colorMatrix < 0 || colorMatrix > 3) {
                std::fprintf(stderr, "bad --matrix value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--range" && i + 1 < argc) {
            colorRange = std::atoi(argv[++i]);
            if (colorRange < 0 || colorRange > 2) {
                std::fprintf(stderr, "bad --range value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--rgb" && i + 1 < argc) {
            const int n = std::sscanf(argv[++i], "%lf,%lf,%lf,%lf", &expectedRgb[0], &expectedRgb[1], &expectedRgb[2], &rgbTol);
            if (n < 3) {
                std::fprintf(stderr, "bad --rgb value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--bit-depth" && i + 1 < argc) {
            expectedBitDepth = std::atoi(argv[++i]);
            if (expectedBitDepth <= 0) {
                std::fprintf(stderr, "bad --bit-depth value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--fine-ramp" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%lf,%d", &fineRampMaxStep, &fineRampMinLevels) != 2 || fineRampMaxStep <= 0) {
                std::fprintf(stderr, "bad --fine-ramp value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--skip-without-decoder") {
            skipWithoutDecoder = true;
        } else if (a == "--tol" && i + 1 < argc) {
            tol = std::atof(argv[++i]);
        } else if (a == "--monotonic") {
            checkMonotonic = true;
        } else if (a == "--nearest") {
            loadNearest = true;
        } else if (a == "--ramp" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%lf,%lf", &rampOffset, &rampStep) != 2 || rampStep <= 0) {
                std::fprintf(stderr, "bad --ramp value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--frames" && i + 1 < argc) {
            expectedFrames = std::atoi(argv[++i]);
        } else if (a == "--reference" && i + 1 < argc) {
            referenceFile = argv[++i];
        } else if (a == "--reference-frames" && i + 1 < argc) {
            std::istringstream is(argv[++i]);
            std::string item;
            while (std::getline(is, item, ',')) {
                if (!item.empty()) {
                    referenceFrames.push_back(std::atoi(item.c_str()));
                }
            }
        } else if (a == "--max-reverse-bytes" && i + 1 < argc) {
            maxReverseBytes = std::atoll(argv[++i]);
        } else if (a == "--order" && i + 1 < argc) {
            std::istringstream is(argv[++i]);
            std::string item;
            while (std::getline(is, item, ',')) {
                renderOrder.push_back(std::atoi(item.c_str()));
            }
        } else if (a == "--max-seq-bytes" && i + 1 < argc) {
            maxSeqBytes = std::atoll(argv[++i]);
        } else if (a == "--max-open-bytes" && i + 1 < argc) {
            maxOpenBytes = std::atoll(argv[++i]);
        } else if (a == "--seek-bytes" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%d,%lld", &seekBytesFrame, &maxSeekBytes) != 2) {
                std::fprintf(stderr, "bad --seek-bytes value: %s\n", argv[i]);
                return 2;
            }
        } else if (a == "--max-indexed-frame" && i + 1 < argc) {
            maxIndexedFrame = std::atoi(argv[++i]);
        } else if (a == "--open-only") {
            openOnly = true;
        } else if (a == "--max-seeks" && i + 1 < argc) {
            maxSeeks = std::atol(argv[++i]);
        } else if (a == "--no-seek-check" && i + 1 < argc) {
            std::istringstream is(argv[++i]);
            std::string item;
            while (std::getline(is, item, ',')) {
                noSeekCheck.push_back(std::atoi(item.c_str()));
            }
        } else if ((a == "--fail" || a == "--gaps" || a == "--concealed" || a == "--fresh") && i + 1 < argc) {
            std::vector<int>& list = (a == "--fail") ? mustFail : (a == "--gaps") ? gaps : (a == "--fresh") ? fresh : concealed;
            std::istringstream is(argv[++i]);
            std::string item;
            while (std::getline(is, item, ',')) {
                list.push_back(std::atoi(item.c_str()));
            }
        } else if (a == "--positions" && i + 1 < argc) {
            std::istringstream is(argv[++i]);
            std::string item;
            while (std::getline(is, item, ',')) {
                const int p = std::atoi(item.c_str());
                if (item.empty() || p < 0 || (!positions.empty() && p <= positions.back())) {
                    std::fprintf(stderr, "bad --positions value: %s\n", argv[i]);
                    return 2;
                }
                positions.push_back(p);
            }
        } else if (a == "--verbose") {
            verbose = true;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
    }
    if (!positions.empty() && (positions[0] != 0)) {
        std::fprintf(stderr, "--positions must start at 0\n");
        return 2;
    }
    if (!gaps.empty() && !positions.empty()) {
        std::fprintf(stderr, "--gaps and --positions are exclusive\n");
        return 2;
    }

    FFmpegFile file(filename);
    if (file.isInvalid()) {
        if (skipWithoutDecoder && hasNoVideoDecoder(filename)) {
            std::printf("SKIPPED: no decoder in the linked libavcodec: %s\n", file.getError().c_str());
            return kExitSkipped;
        }
        std::fprintf(stderr, "FAIL: open failed: %s\n", file.getError().c_str());
        return 1;
    }
    file.setColorOverrides((FFmpegFile::ColorMatrixEnum)colorMatrix, (FFmpegFile::ColorRangeEnum)colorRange);

    int w = 0, h = 0, frames = 0;
    double aspect = 1.0;
    if (!file.getInfo(w, h, aspect, frames)) {
        std::fprintf(stderr, "FAIL: getInfo failed\n");
        return 1;
    }
    double fps = 0.0;
    file.getFPS(fps);
    const int nComps = file.getNumberOfComponents();
    const int sizeOfData = static_cast<int>(file.getSizeOfData());

    std::printf("file=%s\n  %dx%d par=%.4f frames=%d fps=%.4f comps=%d bitDepth=%d bytesPerSample=%d colorspace=%s\n",
                filename.c_str(), w, h, aspect, frames, fps, nComps, file.getBitDepth(), sizeOfData, file.getColorspace());

    if (w <= 0 || h <= 0 || frames <= 0 || (nComps != 3 && nComps != 4)) {
        std::fprintf(stderr, "FAIL: implausible stream info\n");
        return 1;
    }

    if (expectedFrames >= 0 && frames != expectedFrames) {
        std::fprintf(stderr, "FAIL: getInfo reports %d frames, expected %d\n", frames, expectedFrames);
        return 1;
    }
    if (verbose || (maxOpenBytes >= 0)) {
        std::printf("  open read %lld bytes\n", (long long)file.getBytesReadAtOpen());
    }
    if ((maxOpenBytes >= 0) && (file.getBytesReadAtOpen() > maxOpenBytes)) {
        std::fprintf(stderr, "FAIL: opening the file read %lld bytes, expected at most %lld\n", (long long)file.getBytesReadAtOpen(), maxOpenBytes);
        return 1;
    }
    if (maxIndexedFrame >= 0) {
        std::printf("  the index reaches frame %d\n", file.getLastIndexedFrame() + 1);
        if (file.getLastIndexedFrame() + 1 > maxIndexedFrame) {
            std::fprintf(stderr, "FAIL: the index reaches frame %d after the open, expected at most %d\n", file.getLastIndexedFrame() + 1, maxIndexedFrame);
            return 1;
        }
    }
    if (seekBytesFrame >= 1) {
        std::vector<unsigned char> seekBuf(static_cast<size_t>(w) * h * nComps * sizeOfData);
        const long long bytes0 = file.getBytesRead();
        bool ok = false;
        try {
            ok = file.decode(nullptr, seekBytesFrame, loadNearest, seekBuf.data());
        } catch (const std::exception& e) {
            ok = false;
        }
        const long long bytes = file.getBytesRead() - bytes0;
        std::printf("  decoding frame %d after the open read %lld bytes\n", seekBytesFrame, bytes);
        if (!ok || (bytes > maxSeekBytes)) {
            std::fprintf(stderr, "FAIL: decoding frame %d after the open %s, and read %lld bytes, expected at most %lld\n", seekBytesFrame, ok ? "succeeded" : "failed", bytes, maxSeekBytes);
            return 1;
        }
    }
    if (expectedRgb[0] >= 0.) {
        // --rgb: the first frame, then the last one (after a seek)
        std::vector<unsigned char> rgbBuf(static_cast<size_t>(w) * h * nComps * sizeOfData);
        for (int f : { 1, frames }) {
            bool ok = false;
            std::string error;
            try {
                ok = file.decode(nullptr, f, loadNearest, rgbBuf.data(), &error);
            } catch (const std::exception& e) {
                error = e.what();
            }
            if (!ok) {
                std::fprintf(stderr, "FAIL: --rgb: frame %d failed to decode: %s\n", f, error.c_str());
                return 1;
            }
            double rgb[3];
            for (int c = 0; c < 3; ++c) {
                rgb[c] = meanComponent(rgbBuf.data(), w, h, nComps, sizeOfData, c, 257.0); // 65535 is 255
            }
            std::printf("  frame %d: mean RGB %.2f %.2f %.2f (expected %.2f %.2f %.2f +-%.2f)\n", f, rgb[0], rgb[1], rgb[2], expectedRgb[0], expectedRgb[1], expectedRgb[2], rgbTol);
            for (int c = 0; c < 3; ++c) {
                if (std::fabs(rgb[c] - expectedRgb[c]) > rgbTol) {
                    std::fprintf(stderr, "FAIL: --rgb: frame %d: component %d is %.2f, expected %.2f +-%.2f\n", f, c, rgb[c], expectedRgb[c], rgbTol);
                    return 1;
                }
            }
        }
    }
    if ((expectedBitDepth > 0) && (file.getBitDepth() != expectedBitDepth)) {
        std::fprintf(stderr, "FAIL: --bit-depth: the reader reports %d bits, expected %d\n", file.getBitDepth(), expectedBitDepth);
        return 1;
    }
    if (fineRampMaxStep > 0.) {
        const size_t samples = static_cast<size_t>(w) * h * nComps;
        std::vector<unsigned char> rampBuf(samples * sizeOfData);
        bool ok = false;
        std::string error;
        try {
            ok = file.decode(nullptr, 1, loadNearest, rampBuf.data(), &error);
        } catch (const std::exception& e) {
            error = e.what();
        }
        if (!ok) {
            if (skipWithoutDecoder && isNotImplementedError(error)) {
                std::printf("SKIPPED: no decoder in the linked libavcodec: %s\n", error.c_str());
                return kExitSkipped;
            }
            std::fprintf(stderr, "FAIL: --fine-ramp: frame 1 failed to decode: %s\n", error.c_str());
            return 1;
        }
        // the samples, as integers (copied: rampBuf holds bytes)
        std::vector<int> values;
        if (sizeOfData == 2) {
            std::vector<uint16_t> values16(samples);
            std::memcpy(values16.data(), rampBuf.data(), samples * sizeof(uint16_t));
            values.assign(values16.begin(), values16.end());
        } else {
            values.assign(rampBuf.begin(), rampBuf.end());
        }
        // the mean of the first component of each column, on a 0-255 scale, and the distinct values it takes
        const auto sample = [&](int x, int y) -> int {
            return values[(static_cast<size_t>(y) * w + x) * nComps];
        };
        const double scale = sizeOfData == 2 ? 257. : 1.; // 65535 is 255
        std::vector<bool> seen(65536, false);
        int levels = 0;
        double maxStep = 0.;
        int maxStepColumn = 0;
        double previous = 0.;
        for (int x = 0; x < w; ++x) {
            double sum = 0.;
            for (int y = 0; y < h; ++y) {
                const int v = sample(x, y);
                sum += v;
                if (!seen[v]) {
                    seen[v] = true;
                    ++levels;
                }
            }
            const double mean = sum / h / scale;
            if ((x > 0) && (std::fabs(mean - previous) > maxStep)) {
                maxStep = std::fabs(mean - previous);
                maxStepColumn = x;
            }
            previous = mean;
        }
        std::printf("  fine ramp: %d distinct values, largest step %.3f at column %d (expected at least %d values, steps of at most %.3f)\n",
                    levels, maxStep, maxStepColumn, fineRampMinLevels, fineRampMaxStep);
        if ((levels < fineRampMinLevels) || (maxStep > fineRampMaxStep)) {
            std::fprintf(stderr, "FAIL: --fine-ramp: %d distinct values, largest step %.3f, expected at least %d values and steps of at most %.3f\n",
                         levels, maxStep, fineRampMinLevels, fineRampMaxStep);
            return 1;
        }
    }
    if (openOnly) {
        std::printf("PASS\n");
        return 0;
    }

    // --reference: the --reference-frames must be exactly those of the clean file, in every order.
    if (!referenceFile.empty()) {
        if (!mustFail.empty() || !gaps.empty() || !concealed.empty() || !fresh.empty() || !positions.empty() || (rampStep > 0) ||
            (maxSeqBytes >= 0) || (maxReverseBytes >= 0) || checkMonotonic) {
            std::fprintf(stderr, "--reference only checks the --reference-frames (with --order): it cannot be combined with the other checks\n");
            return 2;
        }
        for (int f : renderOrder) {
            if ((f < 1) || (f > frames)) {
                std::fprintf(stderr, "--order: frame %d is not in 1..%d\n", f, frames);
                return 2;
            }
        }
        const size_t refBytes = static_cast<size_t>(w) * h * nComps * sizeOfData;
        std::vector<unsigned char> refBuf(refBytes);
        std::vector<std::vector<unsigned char>> ref(frames + 1);
        {
            FFmpegFile clean(referenceFile.c_str());
            for (int f : referenceFrames) {
                if ((f < 1) || (f > frames) || !clean.decode(nullptr, f, loadNearest, refBuf.data())) {
                    std::fprintf(stderr, "FAIL: --reference: frame %d of %s cannot be decoded\n", f, referenceFile.c_str());
                    return 1;
                }
                ref[f] = refBuf;
            }
        }
        std::vector<std::pair<std::string, std::vector<int>>> orders;
        std::vector<int> all;
        for (int f = 1; f <= frames; ++f) {
            all.push_back(f);
        }
        orders.push_back(std::make_pair(std::string("sequential"), all));
        orders.push_back(std::make_pair(std::string("reverse"), std::vector<int>(all.rbegin(), all.rend())));
        std::vector<int> shuffled = all;
        uint32_t seed = 5;
        for (size_t i = shuffled.size(); i > 1; --i) {
            seed = seed * 1664525u + 1013904223u; // LCG: the same order on every platform
            std::swap(shuffled[i - 1], shuffled[(seed >> 8) % i]);
        }
        orders.push_back(std::make_pair(std::string("random"), shuffled));
        if (!renderOrder.empty()) {
            orders.push_back(std::make_pair(std::string("--order"), renderOrder));
        }
        orders.push_back(std::make_pair(std::string("reopen"), all)); // each frame in its own newly opened file
        int refErrors = 0;
        for (const auto& o : orders) {
            std::unique_ptr<FFmpegFile> file(new FFmpegFile(filename));
            for (int f : o.second) {
                if ((o.first == "reopen") && (f > 1)) {
                    file.reset(new FFmpegFile(filename));
                }
                bool ok = false;
                std::memset(refBuf.data(), 0, refBytes);
                try {
                    ok = file->decode(nullptr, f, loadNearest, refBuf.data());
                } catch (const std::exception& e) {
                    ok = false;
                }
                if (ref[f].empty()) {
                    continue; // not a reference frame: anything
                }
                if (!ok || (refBuf != ref[f])) {
                    if (++refErrors <= 20) {
                        std::printf("  REFERENCE %s order: frame %d %s\n", o.first.c_str(), f, ok ? "differs from the clean file" : "failed");
                    }
                }
            }
        }
        std::printf("Result: referenceFrames=%zu referenceErrors=%d\n", referenceFrames.size(), refErrors);
        if (refErrors) {
            std::printf("FAIL\n");
            return 1;
        }
        std::printf("PASS\n");
        return 0;
    }

    // --gaps: the frames that are not in a gap, whose ramp content is that of their own frame
    const bool rampAtPosition = !gaps.empty();
    if (rampAtPosition) {
        for (int f = 1; f <= frames; ++f) {
            if (std::find(gaps.begin(), gaps.end(), f) == gaps.end()) {
                positions.push_back(f - 1);
            }
        }
        if (positions.empty() || (positions[0] != 0)) {
            std::fprintf(stderr, "--gaps: frame 1 must not be in a gap\n");
            return 2;
        }
    }
    auto isConcealed = [&concealed](int f) -> bool { return std::find(concealed.begin(), concealed.end(), f) != concealed.end(); };

    // the content check: the ramp frame displayed at frame f (1-based), and the ramp frame (1-based)
    // whose content is closest to the signature s
    auto rampDisplayed = [&positions](int f) -> int {
        if (positions.empty()) {
            return f;
        }
        // the last ramp frame at or before frame f
        int i = 0;
        while ((i + 1 < (int)positions.size()) && (positions[i + 1] <= f - 1)) {
            ++i;
        }
        return i + 1;
    };
    auto rampExpected = [rampOffset, rampStep, &rampDisplayed, &positions, rampAtPosition](int f) -> double {
        return rampOffset + rampStep * (rampAtPosition ? positions[rampDisplayed(f) - 1] : (rampDisplayed(f) - 1));
    };
    auto rampFrame = [rampOffset, rampStep](double s) -> double { return 1.0 + (s - rampOffset) / rampStep; };
    const double rampTol = 0.4 * rampStep;
    int contentErrors = 0;

    const size_t bufBytes = static_cast<size_t>(w) * h * nComps * sizeOfData;
    std::vector<unsigned char> buf(bufBytes);

    // A frame that must fail to decode (--fail): returns true if it did fail, with an error message.
    auto isMustFail = [&mustFail](int f) -> bool { return std::find(mustFail.begin(), mustFail.end(), f) != mustFail.end(); };
    int failErrors = 0;
    auto checkFails = [&](int f, const char* phase) {
        bool ok = false;
        try {
            ok = file.decode(nullptr, f, loadNearest, buf.data());
        } catch (const std::exception& e) {
            ok = false;
        }
        if (ok) {
            std::printf("  FAIL-CHECK %s frame %d decoded, but must fail\n", phase, f);
            ++failErrors;
            return;
        }
        // As in the plug-in (ReadFFmpeg refuses an invalid file): a frame that fails must not make the
        // file invalid, and its error message is that of this frame (decode() clears the previous one).
        if (file.isInvalid()) {
            std::printf("  FAIL-CHECK %s frame %d failed and made the file invalid: %s\n", phase, f, file.getError().c_str());
            ++failErrors;
        } else if (file.getError().empty()) {
            std::printf("  FAIL-CHECK %s frame %d failed without an error message\n", phase, f);
            ++failErrors;
        } else if (verbose) {
            std::printf("  %s frame %d failed as expected: %s\n", phase, f, file.getError().c_str());
        }
    };

    const long long seqBytes0 = file.getBytesRead();

    // 1) Sequential decode -> ground-truth signatures (1-based frame indices).
    std::vector<double> seqSig(static_cast<size_t>(frames) + 1, -1.0);
    for (int f = 1; f <= frames; ++f) {
        std::memset(buf.data(), 0, bufBytes);
        if (isMustFail(f)) {
            checkFails(f, "sequential");
            continue;
        }
        bool ok = false;
        try {
            ok = file.decode(nullptr, f, loadNearest, buf.data());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: sequential decode frame %d threw: %s\n", f, e.what());
            return 1;
        }
        if (!ok) {
            std::fprintf(stderr, "FAIL: sequential decode frame %d returned false (%s)\n", f, file.getError().c_str());
            return 1;
        }
        seqSig[f] = meanFirstComponent(buf.data(), w, h, nComps, sizeOfData);
        if (verbose) {
            std::printf("  frame %d sig=%.3f\n", f, seqSig[f]);
        }
        if (rampStep > 0 && !isConcealed(f) && std::fabs(seqSig[f] - rampExpected(f)) > rampTol) {
            std::printf("  CONTENT sequential frame %d: sig=%.3f expected=%.3f (ramp frame %d, looks like ramp frame %.2f)\n",
                        f, seqSig[f], rampExpected(f), rampDisplayed(f), rampFrame(seqSig[f]));
            ++contentErrors;
        }
    }

    // Optional: bounded number of seeks for the sequential decode.
    int seekErrors = 0;
    const long long seqBytes = file.getBytesRead() - seqBytes0;
    if (verbose || (maxSeqBytes >= 0)) {
        std::printf("  sequential decode: read %lld bytes\n", seqBytes);
    }
    if ((maxSeqBytes >= 0) && (seqBytes > maxSeqBytes)) {
        std::printf("  SEQ-BYTES sequential decode read %lld bytes, expected at most %lld\n", seqBytes, maxSeqBytes);
        ++seekErrors;
    }
    const unsigned long seqSeeks = file.getSeekCount();
    if (verbose || maxSeeks >= 0) {
        std::printf("  sequential decode: %lu seeks\n", seqSeeks);
    }
    if ((maxSeeks >= 0) && (seqSeeks > (unsigned long)maxSeeks)) {
        std::printf("  SEEKS sequential decode: %lu seeks, expected at most %ld\n", seqSeeks, maxSeeks);
        ++seekErrors;
    }

    // Optional: a frame in a timestamp gap must be identical to the held frame, decoded at its
    // own frame (whatever its content, e.g. if it is corrupt).
    int holdErrors = 0;
    if (!positions.empty()) {
        for (int f = 1; f <= frames; ++f) {
            const int heldAt = positions[rampDisplayed(f) - 1] + 1; // 1-based frame of the held frame
            if ((heldAt != f) && (heldAt <= frames) && !isMustFail(f) && !isMustFail(heldAt) && (seqSig[f] != seqSig[heldAt])) {
                std::printf("  HOLD frame %d: sig=%.3f, differs from the held frame %d: sig=%.3f\n",
                            f, seqSig[f], heldAt, seqSig[heldAt]);
                ++holdErrors;
            }
        }
    }

    // Optional: sequential frames must be strictly increasing (generated ramp clips).
    int monotonicViolations = 0;
    if (checkMonotonic) {
        for (int f = 2; f <= frames; ++f) {
            if (isMustFail(f) || isMustFail(f - 1) || isConcealed(f) || isConcealed(f - 1)) {
                continue;
            }
            if (!(seqSig[f] > seqSig[f - 1])) {
                std::printf("  MONOTONIC violation: frame %d sig=%.3f <= frame %d sig=%.3f\n",
                            f, seqSig[f], f - 1, seqSig[f - 1]);
                ++monotonicViolations;
            }
        }
    }

    // 2) Seek-forcing order: full reverse, then a set of jumps, then forward jumps of 3 frames.
    std::vector<int> order;
    for (int f = frames; f >= 1; --f) {
        order.push_back(f);
    }
    const int jumps[] = { 1, frames, frames / 2, 2, frames - 1, frames / 3, frames / 2, 1, frames };
    for (int j : jumps) {
        if (j >= 1 && j <= frames) {
            order.push_back(j);
        }
    }
    for (int f = 2; f <= frames; f += 3) {
        order.push_back(f);
    }

    int mismatches = 0;
    const auto seekStart = std::chrono::steady_clock::now();
    for (int f : order) {
        std::memset(buf.data(), 0, bufBytes);
        if (isMustFail(f)) {
            checkFails(f, "seeked");
            continue;
        }
        bool ok = false;
        try {
            ok = file.decode(nullptr, f, loadNearest, buf.data());
        } catch (const std::exception& e) {
            std::printf("  SEEK decode frame %d threw: %s\n", f, e.what());
            ++mismatches;
            continue;
        }
        if (!ok) {
            std::printf("  SEEK decode frame %d returned false (%s)\n", f, file.getError().c_str());
            ++mismatches;
            continue;
        }
        if (file.isInvalid()) {
            // A successful decode must not leave the file invalid: the plug-in would refuse every
            // later render.
            std::printf("  INVALID after seeked frame %d decoded: %s\n", f, file.getError().c_str());
            ++seekErrors;
        }
        const double s = meanFirstComponent(buf.data(), w, h, nComps, sizeOfData);
        const double diff = std::fabs(s - seqSig[f]);
        const bool seekCheck = (std::find(noSeekCheck.begin(), noSeekCheck.end(), f) == noSeekCheck.end()) && !isConcealed(f);
        if (seekCheck && (diff > tol)) {
            std::printf("  MISMATCH frame %d: sequential=%.3f seeked=%.3f diff=%.3f (tol=%.3f)\n",
                        f, seqSig[f], s, diff, tol);
            ++mismatches;
        }
        if (rampStep > 0 && !isConcealed(f) && std::fabs(s - rampExpected(f)) > rampTol) {
            std::printf("  CONTENT seeked frame %d: sig=%.3f expected=%.3f (ramp frame %d, looks like ramp frame %.2f)\n",
                        f, s, rampExpected(f), rampDisplayed(f), rampFrame(s));
            ++contentErrors;
        }
    }

    const auto seekEnd = std::chrono::steady_clock::now();

    // --order: the frames in that order, in a newly opened file.
    if (!renderOrder.empty()) {
        FFmpegFile orderFile(filename);
        for (int f : renderOrder) {
            std::memset(buf.data(), 0, bufBytes);
            bool ok = false;
            try {
                ok = orderFile.decode(nullptr, f, loadNearest, buf.data());
            } catch (const std::exception& e) {
                ok = false;
            }
            if (isMustFail(f)) {
                if (ok || orderFile.isInvalid() || orderFile.getError().empty()) {
                    std::printf("  FAIL-CHECK order frame %d: %s\n", f, ok ? "decoded, but must fail" : "failed without an error message, or made the file invalid");
                    ++failErrors;
                }
                continue;
            }
            if (!ok) {
                std::printf("  ORDER decode frame %d returned false (%s)\n", f, orderFile.getError().c_str());
                ++mismatches;
                continue;
            }
            const double sig = meanFirstComponent(buf.data(), w, h, nComps, sizeOfData);
            if (!isConcealed(f) && (std::fabs(sig - seqSig[f]) > tol)) {
                std::printf("  ORDER MISMATCH frame %d: sequential=%.3f in this order=%.3f\n", f, seqSig[f], sig);
                ++mismatches;
            }
        }
    }
    // --max-reverse-bytes: every frame in reverse order, in a newly opened file.
    if (maxReverseBytes >= 0) {
        FFmpegFile reverseFile(filename);
        const long long bytes0 = reverseFile.getBytesRead();
        for (int f = frames; f >= 1; --f) {
            try {
                (void)reverseFile.decode(nullptr, f, loadNearest, buf.data());
            } catch (const std::exception& e) {
            }
        }
        const long long bytes = reverseFile.getBytesRead() - bytes0;
        std::printf("  reverse decode: read %lld bytes\n", bytes);
        if (bytes > maxReverseBytes) {
            std::printf("  REVERSE-BYTES reverse decode read %lld bytes, expected at most %lld\n", bytes, maxReverseBytes);
            ++seekErrors;
        }
    }

    // Each --fresh frame, decoded first in a newly opened file.
    for (int f : fresh) {
        FFmpegFile freshFile(filename);
        std::memset(buf.data(), 0, bufBytes);
        bool ok = false;
        try {
            ok = freshFile.decode(nullptr, f, loadNearest, buf.data());
        } catch (const std::exception& e) {
            ok = false;
        }
        if (isMustFail(f)) {
            if (ok || freshFile.isInvalid() || freshFile.getError().empty()) {
                std::printf("  FAIL-CHECK fresh frame %d: %s\n", f, ok ? "decoded, but must fail" : "failed without an error message, or made the file invalid");
                ++failErrors;
            }
            continue;
        }
        if (!ok) {
            std::printf("  FRESH decode frame %d returned false (%s)\n", f, freshFile.getError().c_str());
            ++mismatches;
            continue;
        }
        const double sig = meanFirstComponent(buf.data(), w, h, nComps, sizeOfData);
        if (std::fabs(sig - seqSig[f]) > tol) {
            std::printf("  FRESH MISMATCH frame %d: sequential=%.3f fresh=%.3f\n", f, seqSig[f], sig);
            ++mismatches;
        }
        if (rampStep > 0 && !isConcealed(f) && std::fabs(sig - rampExpected(f)) > rampTol) {
            std::printf("  CONTENT fresh frame %d: sig=%.3f expected=%.3f\n", f, sig, rampExpected(f));
            ++contentErrors;
        }
    }
    const double seekMs = std::chrono::duration<double, std::milli>(seekEnd - seekStart).count();

    std::printf("Result: seekChecks=%zu mismatches=%d monotonicViolations=%d contentErrors=%d holdErrors=%d seekErrors=%d failErrors=%d seekPhaseMs=%.1f\n",
                order.size(), mismatches, monotonicViolations, contentErrors, holdErrors, seekErrors, failErrors, seekMs);

    if (mismatches != 0 || monotonicViolations != 0 || contentErrors != 0 || holdErrors != 0 || seekErrors != 0 || failErrors != 0) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
