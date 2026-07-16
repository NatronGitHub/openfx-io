/*
 * Minimal OFX Support stubs so the FFmpeg reader core (FFmpegFile.cpp) can be
 * linked into a standalone command-line test harness WITHOUT the full OpenFX
 * Support/HostSupport libraries.
 *
 * FFmpegFile.cpp only references a single symbol from the OFX Support library:
 *   OFX::MultiThread::getNumCPUs()
 * which it uses to pick a decoder thread count. For a single-threaded test
 * harness returning 1 is sufficient and deterministic.
 *
 * If the linker reports additional undefined OFX symbols, add matching stubs here.
 */

namespace OFX {
namespace MultiThread {

unsigned int
getNumCPUs(void)
{
    return 1;
}

} // namespace MultiThread
} // namespace OFX
