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
 * oiio_layer_menu_test — ReadOIIO's output-layer menu, rebuilt on the main thread while render threads render the
 * same instance and call isIdentity (#298), in a Natron-like host of ofx_test_host.h (dynamic choices, plane suite).
 *
 * The OFX spec (Thread and Recursion Safety) allows it: "An instance must be able to handle simultaneous actions
 * called on the main and render threads", and render and isIdentity may run on render threads. ReadOIIO is
 * kOfxImageEffectRenderFullySafe: 4 threads render the bands of one output image, and call isIdentity on the color
 * plane, while the main thread switches the file param between two EXR files with 2 and 3 layers and calls
 * instanceChanged, which rebuilds the menu (buildOutputLayerMenu). Checks: every render and isIdentity succeeds, the
 * image has the files' pixels, and the menu has the layers of the last file. Then the same with the diffuse layer
 * selected, and isIdentity only: an identity on the diffuse plane. Before the fix, decodePlane() and
 * isIdentity() read the menu without its lock: the assert on the menu's size in decodePlane() may fail, and
 * ThreadSanitizer (make test-oiio-layer-menu-tsan) reports the data race.
 *
 * Usage: oiio_layer_menu_test <scratch dir>   (needs $OCIO, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include <OpenImageIO/imageio.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

int gFailures = 0;

void
check(bool cond, const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    std::printf("  %s: ", cond ? "PASS" : "FAIL");
    std::vprintf(fmt, a);
    std::printf("\n");
    va_end(a);
    if (!cond) {
        ++gFailures;
    }
}

inline OfxImageEffectHandle
H(Effect& e)
{
    return reinterpret_cast<OfxImageEffectHandle>(&e);
}

const int kW = 32;
const int kH = 32;

// the files' R, G, B, A at pixel (x, y) of the file (y down from the top line); the other layers' channels are 0.25
float
fileValue(int x, int y, int c)
{
    switch (c) {
    case 0:
        return (x + 1) / 64.f;
    case 1:
        return (y + 1) / 64.f;
    case 2:
        return 0.5f;
    default:
        return 1.f;
    }
}

// an EXR file with R, G, B, A and the other layers' channels (e.g. "diffuse.R")
bool
writeFile(const std::string& path, const std::vector<std::string>& layers)
{
    std::vector<std::string> names = { "R", "G", "B", "A" };
    for (const std::string& l : layers) {
        for (const char* c : { "R", "G", "B" }) {
            names.push_back(l + "." + c);
        }
    }
    OIIO::ImageSpec spec(kW, kH, (int)names.size(), OIIO::TypeDesc::FLOAT);
    spec.channelnames = names;
    spec.alpha_channel = 3;
    std::vector<float> pix;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            for (int c = 0; c < (int)names.size(); ++c) {
                pix.push_back((c < 4) ? fileValue(x, y, c) : 0.25f);
            }
        }
    }
    auto out = OIIO::ImageOutput::create(path);
    const bool written = out && out->open(path, spec) && out->write_image(OIIO::TypeDesc::FLOAT, pix.data());
    if (!out || !out->close() || !written) {
        std::printf("  cannot write %s: %s\n", path.c_str(), out ? out->geterror().c_str() : OIIO::geterror().c_str());

        return false;
    }

    return true;
}

OfxStatus
instanceChanged(Effect& inst, OfxPlugin* p, const char* param, const char* reason)
{
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
    pSetString(ap, kOfxPropName, 0, param);
    pSetString(ap, kOfxPropChangeReason, 0, reason);
    pSetDouble(ap, kOfxPropTime, 0, 1.);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);

    return p->mainEntry(kOfxActionInstanceChanged, H(inst), ap, nullptr);
}

// the number of options of the output-layer menu
int
nLayerOptions(Effect& ctx)
{
    int n = -1;
    pGetDimension(PH(ctx.params.at("outputLayer").props), kOfxParamPropChoiceOption, &n);

    return n;
}

} // namespace

int
main(int argc,
     char** argv)
{
    if (argc != 2) {
        std::printf("usage: %s <scratch dir>\n", argv[0]);

        return 2;
    }
    const std::string dir = argv[1];
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // (the output before a crash)
    initHost();
    // a Natron-like host: ReadOIIO has the output-layer menu if the host supports dynamic choices and the plane suite
    pSetInt(PH(&gHostProps), kNatronOfxParamHostPropSupportsDynamicChoices, 0, 1);
    pSetInt(PH(&gHostProps), kFnOfxImageEffectPropMultiPlanar, 0, 1);

    const std::string files[2] = { dir + "/layers2.exr", dir + "/layers3.exr" };
    const int nLayers[2] = { 2, 3 }; // Color and the other layers
    if (!writeFile(files[0], { "diffuse" }) || !writeFile(files[1], { "diffuse", "specular" })) {
        std::printf("FAIL: files\n");

        return 1;
    }

    OfxPlugin* p = findPlugin("fr.inria.openfx.ReadOIIO");
    if (!p) {
        std::printf("FAIL: fr.inria.openfx.ReadOIIO not found\n");

        return 1;
    }
    int status = 0;
    {
        Effect desc, ctx, inst;
        if (loadAndDescribe(p, desc, ctx, kOfxImageEffectContextReader) != 0 || !ctx.params.count("outputLayer")) {
            std::printf("FAIL: load/describe (or no output-layer menu)\n");

            return 1;
        }
        std::printf("==== ReadOIIO: the output-layer menu rebuilt during renders and isIdentity (#298) ====\n");
        setOverrideString(&ctx, kOfxImageEffectFileParamName, files[0]);
        inst.delegate = &ctx;
        setInstanceProps(inst, kW, kH, 1, kOfxImageEffectContextReader);
        bool created = ok(p->mainEntry(kOfxActionCreateInstance, H(inst), nullptr, nullptr));
        Clip* out = ctx.clips.count(kOfxImageEffectOutputClipName) ? ctx.clips[kOfxImageEffectOutputClipName] : nullptr;
        bool ready = created && out && ok(instanceChanged(inst, p, kOfxImageEffectFileParamName, kOfxChangeUserEdited));
        if (ready) {
            setOverrideInt(&ctx, "outputComponents", 0); // RGBA
            out->connected = true;
            setupClipImage(out, kW, kH, kOfxImageComponentRGBA, kOfxBitDepthFloat, false);
            PropSet o;
            ready = ok(p->mainEntry(kOfxImageEffectActionGetClipPreferences, H(inst), nullptr, PH(&o)));
        }
        check(ready, "create the instance, read %s", files[0].c_str());
        if (ready) {
            check(nLayerOptions(ctx) == nLayers[0], "%d layers in the menu (expected %d)", nLayerOptions(ctx), nLayers[0]);
            // a proxy threshold of 1/2: at scale 1, GenericReader::render() doesn't read the file param a second time
            // (for a proxy file), and doesn't assert that it is still the same (another race with a file change, #373)
            {
                ParamRec& r = ctx.params.at("proxyThreshold");
                r.hasOv = true;
                r.ov.d = { 0.5, 0.5 };
            }
            float* const data = reinterpret_cast<float*>(out->buffer.data());
            fillUnwritten(data, (size_t)kW * kH * 4);

            const int kThreads = 4;
            const int kRenders = 100; // per thread
            // isIdentity on the color plane, for the rows [y1, y2)
            auto isIdentity = [&](int y1, int y2, PropSet& io) {
                PropSet ia;
                OfxPropertySetHandle iap = PH(&ia);
                pSetDouble(iap, kOfxPropTime, 0, 1.);
                double rs[2] = { 1., 1. };
                pSetDoubleN(iap, kOfxImageEffectPropRenderScale, 2, rs);
                int win[4] = { 0, y1, kW, y2 };
                pSetIntN(iap, kOfxImageEffectPropRenderWindow, 4, win);
                pSetString(iap, kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
                pSetInt(iap, kFnOfxImageEffectPropView, 0, 0);
                pSetString(iap, kOfxImageEffectPropIdentityPlane, 0, kFnOfxImagePlaneColour);

                return p->mainEntry(kOfxImageEffectActionIsIdentity, H(inst), iap, PH(&io));
            };
            // the main thread: the user switches files while the renders run (instanceChanged rebuilds the menu)
            ParamRec& fileParam = ctx.params.at(kOfxImageEffectFileParamName);
            int rebuilds = 0, wrongMenus = 0;
            auto switchFilesWhile = [&](const std::atomic<int>& running) {
                do {
                    const int next = (rebuilds + 1) % 2;
                    {
                        HostWrite lock;
                        fileParam.ov.s = { files[next] };
                    }
                    if (!ok(instanceChanged(inst, p, kOfxImageEffectFileParamName, kOfxChangeUserEdited)) || (nLayerOptions(ctx) != nLayers[next])) {
                        ++wrongMenus;
                    }
                    ++rebuilds;
                } while (running > 0);
            };
            std::atomic<int> running(kThreads), renders(0), failedRenders(0), failedIdentities(0);
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t]() {
                    const int y1 = t * kH / kThreads;
                    const int y2 = (t + 1) * kH / kThreads;
                    for (int i = 0; i < kRenders; ++i) {
                        // the color plane is the Color layer (the menu's first option): not an identity
                        PropSet io;
                        if (isIdentity(y1, y2, io) != kOfxStatReplyDefault) {
                            ++failedIdentities;
                        }
                        PropSet ra = makeRenderInArgs(1., kW, kH);
                        int win[4] = { 0, y1, kW, y2 };
                        pSetIntN(PH(&ra), kOfxImageEffectPropRenderWindow, 4, win);
                        if (!ok(p->mainEntry(kOfxImageEffectActionRender, H(inst), PH(&ra), nullptr))) {
                            ++failedRenders;
                        }
                        ++renders;
                    }
                    --running;
                });
            }
            switchFilesWhile(running);
            for (std::thread& t : threads) {
                t.join();
            }
            check(failedRenders == 0 && failedIdentities == 0, "%d renders and isIdentity (%d failed renders, %d isIdentity not on the color plane)",
                  (int)renders, (int)failedRenders, (int)failedIdentities);
            check(wrongMenus == 0, "%d menus rebuilt, alternately %d and %d layers (%d wrong)", rebuilds, nLayers[0], nLayers[1], wrongMenus);
            int bad = 0;
            for (int y = 0; y < kH; ++y) {
                for (int x = 0; x < kW; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const float v = fileValue(x, kH - 1 - y, c);
                        const float* px = data + ((size_t)y * kW + x) * 4 + c;
                        if (isUnwritten(px) || !(std::fabs(*px - v) <= 1e-6f)) {
                            if (bad < 3) {
                                std::printf("  pixel (%d,%d) channel %d: %.9g, expected %.9g\n", x, y, c, *px, v);
                            }
                            ++bad;
                        }
                    }
                }
            }
            check(bad == 0, "the image is the files' color layer (%d wrong values)", bad);
            check(gPersistentError.empty(), "no error message%s%s", gPersistentError.empty() ? "" : ": ", gPersistentError.c_str());

            // the user selects the diffuse layer (in both files): isIdentity on the color plane is an identity on the
            // diffuse plane of the output clip, while the main thread switches files
            // (the menu, rebuilt before the threads start, selects it)
            setOverrideString(&ctx, "outputLayerChoice", "diffuse.RGB");
            const int before = rebuilds;
            std::atomic<int> none(0);
            switchFilesWhile(none);
            std::atomic<int> runningIdentities(kThreads), identities(0), wrongIdentities(0);
            threads.clear();
            for (int t = 0; t < kThreads; ++t) {
                threads.emplace_back([&, t]() {
                    for (int i = 0; i < kRenders; ++i) {
                        PropSet io;
                        const char* plane = "";
                        if (isIdentity(t * kH / kThreads, (t + 1) * kH / kThreads, io) != kOfxStatOK
                            || pGetString(PH(&io), kOfxImageEffectPropIdentityPlane, 0, &plane) != kOfxStatOK
                            || std::string(plane).find("diffuse") == std::string::npos) {
                            ++wrongIdentities;
                        }
                        ++identities;
                    }
                    --runningIdentities;
                });
            }
            switchFilesWhile(runningIdentities);
            for (std::thread& t : threads) {
                t.join();
            }
            check(wrongIdentities == 0 && wrongMenus == 0, "diffuse layer: %d isIdentity, %d menus rebuilt (%d not an identity on the diffuse plane, %d wrong menus)",
                  (int)identities, rebuilds - before, (int)wrongIdentities, wrongMenus);
        }
        if (created) {
            p->mainEntry(kOfxActionDestroyInstance, H(inst), nullptr, nullptr);
        }
        p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    }
    status = gFailures ? 1 : 0;
    std::printf("%s (%d failure%s)\n", gFailures ? "FAIL" : "PASS", gFailures, gFailures == 1 ? "" : "s");

    return status;
}
