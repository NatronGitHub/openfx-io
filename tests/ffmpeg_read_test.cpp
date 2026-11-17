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
 *   7. optionally (--positions P0,P1,..., with --ramp) checks a variable frame rate
 *      clip with timestamp gaps: ramp frame i (0-based) is at frame Pi (0-based,
 *      increasing), and a requested frame in a gap must give the frame displayed at
 *      that time, i.e. the last ramp frame at or before it (held until the next
 *      one, or until the end).
 *
 * A wrong seek target (e.g. the old frameToPts operator-precedence bug that
 * dropped the stream start PTS) makes seeked frames land on the wrong frame,
 * which this harness detects as a signature mismatch.
 *
 * Usage: ffmpeg_read_test <file> [--tol N] [--monotonic] [--nearest] [--ramp OFFSET,STEP] [--frames N] [--positions P0,P1,...] [--verbose]
 * Exit status: 0 = all checks passed, non-zero = failure.
 */

#include "FFmpegFile.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

namespace {

double
meanFirstComponent(const unsigned char* buf, int w, int h, int nComps, int sizeOfData)
{
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (n == 0) {
        return 0.0;
    }
    double sum = 0.0;
    if (sizeOfData == 2) {
        const uint16_t* p = reinterpret_cast<const uint16_t*>(buf);
        for (size_t i = 0; i < n; ++i) {
            sum += p[i * nComps];
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            sum += buf[i * nComps];
        }
    }
    // on a 0-255 scale, whatever the bit depth, so that --tol and --ramp do not depend on it
    return sum / static_cast<double>(n) / (sizeOfData == 2 ? 256.0 : 1.0);
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file> [--tol N] [--monotonic] [--nearest] [--ramp OFFSET,STEP] [--frames N] [--positions P0,P1,...] [--verbose]\n", argv[0]);
        return 2;
    }
    const std::string filename = argv[1];
    double tol = 2.0;
    bool checkMonotonic = false;
    bool loadNearest = false;
    double rampOffset = 0.0, rampStep = 0.0; // if rampStep > 0, frame f must have signature rampOffset + rampStep * (f - 1)
    int expectedFrames = -1;
    std::vector<int> positions; // if not empty, ramp frame i (0-based) is at frame positions[i] (0-based)
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--tol" && i + 1 < argc) {
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
    if (!positions.empty() && (rampStep <= 0 || positions[0] != 0)) {
        std::fprintf(stderr, "--positions needs --ramp, and must start at 0\n");
        return 2;
    }

    FFmpegFile file(filename);
    if (file.isInvalid()) {
        std::fprintf(stderr, "FAIL: open failed: %s\n", file.getError().c_str());
        return 1;
    }

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

    std::printf("file=%s\n  %dx%d par=%.4f frames=%d fps=%.4f comps=%d bytesPerSample=%d colorspace=%s\n",
                filename.c_str(), w, h, aspect, frames, fps, nComps, sizeOfData, file.getColorspace());

    if (w <= 0 || h <= 0 || frames <= 0 || (nComps != 3 && nComps != 4)) {
        std::fprintf(stderr, "FAIL: implausible stream info\n");
        return 1;
    }

    if (expectedFrames >= 0 && frames != expectedFrames) {
        std::fprintf(stderr, "FAIL: getInfo reports %d frames, expected %d\n", frames, expectedFrames);
        return 1;
    }

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
    auto rampExpected = [rampOffset, rampStep, &rampDisplayed](int f) -> double { return rampOffset + rampStep * (rampDisplayed(f) - 1); };
    auto rampFrame = [rampOffset, rampStep](double s) -> double { return 1.0 + (s - rampOffset) / rampStep; };
    const double rampTol = 0.4 * rampStep;
    int contentErrors = 0;

    const size_t bufBytes = static_cast<size_t>(w) * h * nComps * sizeOfData;
    std::vector<unsigned char> buf(bufBytes);

    // 1) Sequential decode -> ground-truth signatures (1-based frame indices).
    std::vector<double> seqSig(static_cast<size_t>(frames) + 1, -1.0);
    for (int f = 1; f <= frames; ++f) {
        std::memset(buf.data(), 0, bufBytes);
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
        if (rampStep > 0 && std::fabs(seqSig[f] - rampExpected(f)) > rampTol) {
            std::printf("  CONTENT sequential frame %d: sig=%.3f expected=%.3f (ramp frame %d, looks like ramp frame %.2f)\n",
                        f, seqSig[f], rampExpected(f), rampDisplayed(f), rampFrame(seqSig[f]));
            ++contentErrors;
        }
    }

    // Optional: sequential frames must be strictly increasing (generated ramp clips).
    int monotonicViolations = 0;
    if (checkMonotonic) {
        for (int f = 2; f <= frames; ++f) {
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
        const double s = meanFirstComponent(buf.data(), w, h, nComps, sizeOfData);
        const double diff = std::fabs(s - seqSig[f]);
        if (diff > tol) {
            std::printf("  MISMATCH frame %d: sequential=%.3f seeked=%.3f diff=%.3f (tol=%.3f)\n",
                        f, seqSig[f], s, diff, tol);
            ++mismatches;
        }
        if (rampStep > 0 && std::fabs(s - rampExpected(f)) > rampTol) {
            std::printf("  CONTENT seeked frame %d: sig=%.3f expected=%.3f (ramp frame %d, looks like ramp frame %.2f)\n",
                        f, s, rampExpected(f), rampDisplayed(f), rampFrame(s));
            ++contentErrors;
        }
    }

    const auto seekEnd = std::chrono::steady_clock::now();
    const double seekMs = std::chrono::duration<double, std::milli>(seekEnd - seekStart).count();

    std::printf("Result: seekChecks=%zu mismatches=%d monotonicViolations=%d contentErrors=%d seekPhaseMs=%.1f\n",
                order.size(), mismatches, monotonicViolations, contentErrors, seekMs);

    if (mismatches != 0 || monotonicViolations != 0 || contentErrors != 0) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
