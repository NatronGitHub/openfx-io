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
 *      (mean of the first component) — the "ground truth",
 *   3. re-decodes frames in a seek-forcing order (reverse + jumps) and checks
 *      each seeked frame's signature matches the sequential one,
 *   4. optionally checks the sequential signatures are strictly monotonic
 *      (for the generated gray-ramp clips), proving sequential frames are distinct.
 *
 * A wrong seek target (e.g. the old frameToPts operator-precedence bug that
 * dropped the stream start PTS) makes seeked frames land on the wrong frame,
 * which this harness detects as a signature mismatch.
 *
 * Usage: ffmpeg_read_test <file> [--tol N] [--monotonic] [--nearest]
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
    return sum / static_cast<double>(n);
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <file> [--tol N] [--monotonic] [--nearest]\n", argv[0]);
        return 2;
    }
    const std::string filename = argv[1];
    double tol = 2.0;
    bool checkMonotonic = false;
    bool loadNearest = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--tol" && i + 1 < argc) {
            tol = std::atof(argv[++i]);
        } else if (a == "--monotonic") {
            checkMonotonic = true;
        } else if (a == "--nearest") {
            loadNearest = true;
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", a.c_str());
            return 2;
        }
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

    // 2) Seek-forcing order: full reverse, then a set of jumps.
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
    }

    const auto seekEnd = std::chrono::steady_clock::now();
    const double seekMs = std::chrono::duration<double, std::milli>(seekEnd - seekStart).count();

    std::printf("Result: seekChecks=%zu mismatches=%d monotonicViolations=%d seekPhaseMs=%.1f\n",
                order.size(), mismatches, monotonicViolations, seekMs);

    if (mismatches != 0 || monotonicViolations != 0) {
        std::printf("FAIL\n");
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
