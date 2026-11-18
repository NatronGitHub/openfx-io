/*
 * Minimal OFX Support stubs so the FFmpeg reader core (FFmpegFile.cpp) can be
 * linked into a standalone command-line test harness WITHOUT the full OpenFX
 * Support/HostSupport libraries.
 *
 * FFmpegFile.cpp only references a single symbol from the OFX Support library:
 *   OFX::MultiThread::getNumCPUs()
 * which it uses to pick a decoder thread count. For a single-threaded test
 * harness returning 1 is sufficient and deterministic; FFMPEG_READ_TEST_THREADS=N
 * gives N threads (e.g. to test frame threads).
 *
 * If the linker reports additional undefined OFX symbols, add matching stubs here.
 */

#include <cstdlib>

namespace OFX {
namespace MultiThread {

unsigned int
getNumCPUs(void)
{
    const char* threads = std::getenv("FFMPEG_READ_TEST_THREADS");
    const int n = threads ? std::atoi(threads) : 0;
    return (n > 0) ? n : 1;
}

} // namespace MultiThread
} // namespace OFX
