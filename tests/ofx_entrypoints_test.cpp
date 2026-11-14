/*
 * ofx_entrypoints_test — unit tests for the OpenFX C-ABI entry points of the
 * openfx-io plugins, by DIRECTLY LINKING the plugin translation units together
 * with the OFX C++ Support library (no bundle, no dlopen, no OFX host binary).
 *
 * Stage 1 (this file): enumerate the plugins the linked object code registers
 *   via OfxGetNumberOfPlugins()/OfxGetPlugin(), and check identifiers/versions.
 * Later stages add a minimal OfxHost and drive Load/Describe/DescribeInContext.
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofxImageEffect.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

// Provided by the OFX C++ Support library (openfx/Support/Library/ofxsImageEffect.cpp).
extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

int
main(int argc, char** argv)
{
    std::set<std::string> expected;
    for (int i = 1; i < argc; ++i) {
        expected.insert(argv[i]);
    }

    const int n = OfxGetNumberOfPlugins();
    std::printf("OfxGetNumberOfPlugins() = %d\n", n);
    if (n <= 0) {
        std::printf("FAIL: no plugins registered by the linked object code\n");
        return 1;
    }

    int failures = 0;
    std::set<std::string> seen;
    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = OfxGetPlugin(i);
        if (!p) {
            std::printf("FAIL: OfxGetPlugin(%d) returned null\n", i);
            ++failures;
            continue;
        }
        std::printf("  [%d] api=%s apiVersion=%d id=%s v%d.%d setHost=%p mainEntry=%p\n",
                    i,
                    p->pluginApi ? p->pluginApi : "(null)",
                    p->apiVersion,
                    p->pluginIdentifier ? p->pluginIdentifier : "(null)",
                    p->pluginVersionMajor, p->pluginVersionMinor,
                    (void*)p->setHost, (void*)p->mainEntry);

        if (!p->pluginApi || std::strcmp(p->pluginApi, kOfxImageEffectPluginApi) != 0) {
            std::printf("    FAIL: unexpected pluginApi\n");
            ++failures;
        }
        if (!p->pluginIdentifier) {
            std::printf("    FAIL: null pluginIdentifier\n");
            ++failures;
        } else {
            seen.insert(p->pluginIdentifier);
        }
        if (!p->setHost || !p->mainEntry) {
            std::printf("    FAIL: null setHost/mainEntry entry point\n");
            ++failures;
        }
    }

    for (const std::string& id : expected) {
        if (seen.find(id) == seen.end()) {
            std::printf("FAIL: expected plugin id not registered: %s\n", id.c_str());
            ++failures;
        }
    }

    std::printf("Result: plugins=%d failures=%d\n", n, failures);
    std::printf(failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? 0 : 1;
}
