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
 * reader_window: render a reader of a bundle with render windows smaller than its image, or partly or entirely out
 * of its RoD (#232), in a real OFX host: openfx's HostSupport, with hostDemo's host classes (hostDemo itself can't do
 * this: its --window renders the whole RoD for a plug-in without tiles, as most readers are). Adapted from
 * openfx-arena's tests/readerwindow/reader_window.cpp (openfx-arena #105).
 *
 *   reader_window <identifier> <major> [--scale s] [--time t] [name=value...] [--window x1,y1,x2,y2]...
 *   reader_window --write-pfm <file> <width> <height>
 *
 * --write-pfm writes an RGB PFM file (a gradient that differs along x and y) for reader_window.sh, and exits: not every
 * oiiotool writes PFM (OpenImageIO 2.4, in Ubuntu 24.04, doesn't).
 *
 * The OFX spec only says that a plug-in without tiles gets full RoD images (kOfxImageEffectPropSupportsTiles: "If a
 * clip or plugin does not support tiled images, then the host should supply full RoD images to the effect whenever
 * it fetches one"), not that the render window is the whole RoD, and that "A host can ask an effect to render an
 * arbitrary window of pixels, generally these should be clipped to an effect's region of definition, however,
 * depending on the host, they may not be. [...] If an effect is asked to render outside of its region of definition,
 * it should fill those pixels in with black transparent pixels." (ofxRendering.rst). So this host does what such a
 * host may do: the output image covers the RoD and the render window, and the render window is any part of it.
 *
 * It creates the plug-in in the reader context (or the general one), sets the parameters (string, integer, choice
 * index, double, boolean), renders time t (default 0) at render scale s (default 1) with the whole RoD as the render
 * window (the reference), then renders each window (by default: windows in the image, at its edges and offset from
 * the origin, and windows partly or entirely out of the RoD) into a new image, and checks that
 *   - the render succeeds,
 *   - the pixels of the window in the RoD are bit-identical to the same pixels of the reference, and those out of
 *     the RoD are 0 (transparent black),
 *   - the pixels outside of the window still hold hostDemo's sentinel (NaN, compared on its bits): the reader wrote
 *     only the render window.
 * A plug-in that supports tiles also renders each window into an image of the window only. The output depth is
 * float. OFX_PLUGIN_PATH must contain the bundle (no standard paths, no plug-in cache) and OCIO must be set, as for
 * hostDemo. Exit status: 0 if every check passes.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h"
#endif

// (HostSupport's headers are not self-contained: in this order, as in hostDemo.cpp)
// clang-format off
#include "ofxhBinary.h"
#include "ofxhPropertySuite.h"
#include "ofxhClip.h"
#include "ofxhParam.h"
#include "ofxhMemory.h"
#include "ofxhImageEffect.h"
#include "ofxhPluginAPICache.h"
#include "ofxhPluginCache.h"
#include "ofxhHost.h"
#include "ofxhImageEffectAPI.h"

#include "hostDemoHostDescriptor.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"
// clang-format on

namespace {

bool
parseInts(const std::string& s, std::vector<int>& v)
{
    v.clear();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        char* end = NULL;
        const long l = std::strtol(item.c_str(), &end, 10);
        if (item.empty() || *end != '\0') {
            return false;
        }
        v.push_back((int)l);
    }

    return true;
}

// set a parameter from "name=value", then send the instance changed actions
bool
setParam(OFX::Host::ImageEffect::Instance* instance, const std::string& assignment, double time)
{
    const std::string::size_type eq = assignment.find('=');
    if (eq == std::string::npos || eq == 0) {
        std::fprintf(stderr, "Error: expected name=value, got \"%s\"\n", assignment.c_str());

        return false;
    }
    const std::string name = assignment.substr(0, eq);
    const std::string value = assignment.substr(eq + 1);
    OFX::Host::Param::Instance* p = instance->getParam(name);
    if (!p) {
        std::fprintf(stderr, "Error: no parameter \"%s\"\n", name.c_str());

        return false;
    }
    OfxStatus st = kOfxStatErrUnsupported;
    std::vector<int> iv;
    if (OFX::Host::Param::StringInstance* s = dynamic_cast<OFX::Host::Param::StringInstance*>(p)) {
        st = s->set(value.c_str());
    } else if (OFX::Host::Param::IntegerInstance* i = dynamic_cast<OFX::Host::Param::IntegerInstance*>(p)) {
        st = (parseInts(value, iv) && iv.size() == 1) ? i->set(iv[0]) : kOfxStatErrValue;
    } else if (OFX::Host::Param::ChoiceInstance* c = dynamic_cast<OFX::Host::Param::ChoiceInstance*>(p)) {
        st = (parseInts(value, iv) && iv.size() == 1) ? c->set(iv[0]) : kOfxStatErrValue;
    } else if (OFX::Host::Param::BooleanInstance* b = dynamic_cast<OFX::Host::Param::BooleanInstance*>(p)) {
        st = (value == "1" || value == "true") ? b->set(true) : (value == "0" || value == "false") ? b->set(false)
                                                                                                   : kOfxStatErrValue;
    } else if (OFX::Host::Param::DoubleInstance* d = dynamic_cast<OFX::Host::Param::DoubleInstance*>(p)) {
        char* end = NULL;
        const double v = std::strtod(value.c_str(), &end);
        st = (!value.empty() && *end == '\0') ? d->set(v) : kOfxStatErrValue;
    }
    if (st != kOfxStatOK) {
        std::fprintf(stderr, "Error: could not set %s (status %d)\n", assignment.c_str(), (int)st);

        return false;
    }
    OfxPointD scale;
    scale.x = scale.y = 1.;
    instance->beginInstanceChangedAction(kOfxChangeUserEdited);
    const OfxStatus st2 = instance->paramInstanceChangedAction(name, kOfxChangeUserEdited, time, scale);
    instance->endInstanceChangedAction(kOfxChangeUserEdited);
    if (st2 != kOfxStatOK && st2 != kOfxStatReplyDefault) {
        std::fprintf(stderr, "Error: instance changed action failed for %s (status %d)\n", name.c_str(), (int)st2);

        return false;
    }

    return true;
}

OfxRectI
boundingBox(const OfxRectI& a, const OfxRectI& b)
{
    OfxRectI r;
    r.x1 = a.x1 < b.x1 ? a.x1 : b.x1;
    r.y1 = a.y1 < b.y1 ? a.y1 : b.y1;
    r.x2 = a.x2 > b.x2 ? a.x2 : b.x2;
    r.y2 = a.y2 > b.y2 ? a.y2 : b.y2;

    return r;
}

bool
contains(const OfxRectI& r, int x, int y)
{
    return x >= r.x1 && x < r.x2 && y >= r.y1 && y < r.y2;
}

// render time t (view 0) at the given render scale and render window, into a new output image of the given bounds.
// Returns the output image (owned by the clip until the next render), or NULL.
MyHost::MyImage*
render(MyHost::MyEffectInstance* instance, MyHost::MyClipInstance* outputClip, double time, const OfxPointD& renderScale,
       const OfxRectI& imageBounds, const OfxRectI& window)
{
    // the output image covers the render state's window, whatever the render window passed to the render action
    std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD> rois;
    instance->setRenderState(renderScale, imageBounds, rois);
    outputClip->releaseOutputImage();
    const int imageErrors = MyHost::gImageErrors;
#ifdef OFX_EXTENSIONS_NUKE
    std::list<std::string> planes;
    planes.push_back(kFnOfxImagePlaneColour);
#endif
    const OfxStatus stat = instance->renderAction(time, kOfxImageFieldNone, window, renderScale,
                                                  /*sequentialRender=*/false, /*interactiveRender=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                                  /*openGLRender=*/false,
#ifdef OFX_EXTENSIONS_NATRON
                                                  /*contextData=*/NULL,
#endif
#endif
                                                  /*draftRender=*/false
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
                                                  ,
                                                  /*view=*/0
#endif
#ifdef OFX_EXTENSIONS_VEGAS
                                                  ,
                                                  /*nViews=*/1
#endif
#ifdef OFX_EXTENSIONS_NUKE
                                                  ,
                                                  planes
#endif
    );
    instance->clearRenderState();
    MyHost::MyImage* image = outputClip->getOutputImage();
    if (stat != kOfxStatOK || !image || MyHost::gImageErrors != imageErrors) {
        std::fprintf(stderr, "Error: render of (%d,%d,%d,%d) failed (status %d%s)\n", window.x1, window.y1, window.x2, window.y2,
                     (int)stat, image ? "" : ", no output image");

        return NULL;
    }

    return image;
}

bool
isSentinel(const unsigned char* p)
{
    const unsigned int quietNaNBits = 0x7fc00000U; // hostDemo's sentinel (hostDemoClipInstance.cpp)

    return std::memcmp(p, &quietNaNBits, sizeof(float)) == 0;
}

// An RGB PFM file of width x height pixels, little-endian (a negative scale), stored bottom-up
int
writePFM(const char* path, int width, int height)
{
    if (width <= 0 || height <= 0) {
        std::fprintf(stderr, "Error: --write-pfm <file> <width> <height>\n");

        return 2;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        std::fprintf(stderr, "Error: cannot write %s\n", path);

        return 1;
    }
    std::fprintf(f, "PF\n%d %d\n-1.0\n", width, height);
    std::vector<unsigned char> line((size_t)width * 3 * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float v[3] = { 0.1f + 0.8f * x / width, 0.9f - 0.8f * y / height, 0.25f + 0.5f * ((x + 2 * y) % 7) / 7.f };
            for (int c = 0; c < 3; ++c) {
                unsigned int bits;
                std::memcpy(&bits, &v[c], 4);
                for (int k = 0; k < 4; ++k) { // little-endian, whatever the host's byte order
                    line[((size_t)x * 3 + c) * 4 + k] = (unsigned char)(bits >> (8 * k));
                }
            }
        }
        std::fwrite(&line[0], 1, line.size(), f);
    }

    return (std::fclose(f) == 0) ? 0 : 1;
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc == 5 && std::strcmp(argv[1], "--write-pfm") == 0) {
        return writePFM(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    }
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <identifier> <major> [--scale s] [--time t] [name=value...] [--window x1,y1,x2,y2]...\n", argv[0]);

        return 2;
    }
    const std::string pluginId = argv[1];
    const int pluginMajor = std::atoi(argv[2]);
    std::vector<std::string> assignments;
    std::vector<OfxRectI> windows;
    OfxPointD renderScale;
    renderScale.x = renderScale.y = 1.;
    double time = 0.;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--window" && i + 1 < argc) {
            std::vector<int> v;
            if (!parseInts(argv[++i], v) || v.size() != 4) {
                std::fprintf(stderr, "Error: --window x1,y1,x2,y2\n");

                return 2;
            }
            OfxRectI w;
            w.x1 = v[0];
            w.y1 = v[1];
            w.x2 = v[2];
            w.y2 = v[3];
            windows.push_back(w);
        } else if (arg == "--scale" && i + 1 < argc) {
            renderScale.x = renderScale.y = std::atof(argv[++i]);
            if (!(renderScale.x > 0. && renderScale.x <= 1.)) {
                std::fprintf(stderr, "Error: --scale s, 0 < s <= 1\n");

                return 2;
            }
        } else if (arg == "--time" && i + 1 < argc) {
            time = std::atof(argv[++i]);
        } else {
            assignments.push_back(arg);
        }
    }
    std::string what = pluginId + " v" + argv[2];
    for (size_t k = 0; k < assignments.size(); ++k) {
        if (assignments[k].compare(0, 9, "filename=") != 0) {
            what += " " + assignments[k];
        }
    }
    if (renderScale.x != 1.) {
        std::ostringstream ss;
        ss << " scale " << renderScale.x;
        what += ss.str();
    }

    OFX::Host::PluginCache::useStdOFXPluginsLocation(false);
    OFX::Host::PluginCache::getPluginCache()->setCacheVersion("ioReaderWindowV1");
    MyHost::Host myHost;
    OFX::Host::ImageEffect::PluginCache imageEffectPluginCache(&myHost);
    imageEffectPluginCache.registerInCache(*OFX::Host::PluginCache::getPluginCache());
    OFX::Host::PluginCache::getPluginCache()->scanPluginFiles();
    OFX::Host::ImageEffect::ImageEffectPlugin* plugin = imageEffectPluginCache.getPluginById(pluginId, pluginMajor);
    if (!plugin) {
        std::fprintf(stderr, "Error: %s not found in OFX_PLUGIN_PATH\n", what.c_str());

        return 1;
    }

    int failures = 0;
    {
        OFX::Host::auto_ptr<OFX::Host::ImageEffect::Instance> instance(plugin->createInstance(kOfxImageEffectContextReader, NULL));
        if (!instance.get()) {
            instance.reset(plugin->createInstance(kOfxImageEffectContextGeneral, NULL));
        }
        MyHost::MyEffectInstance* myInstance = dynamic_cast<MyHost::MyEffectInstance*>(instance.get());
        if (!myInstance) {
            std::fprintf(stderr, "Error: could not create an instance of %s\n", what.c_str());

            return 1;
        }
        myInstance->setDefaultDepth(kOfxBitDepthFloat); // float output: bit-exact comparisons
        OfxStatus stat = instance->createInstanceAction();
        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
            std::fprintf(stderr, "Error: create instance action failed (status %d)\n", (int)stat);

            return 1;
        }
        if (!instance->getClipPreferences()) {
            std::fprintf(stderr, "Error: get clip preferences action failed\n");

            return 1;
        }
        for (size_t k = 0; k < assignments.size(); ++k) {
            if (!setParam(instance.get(), assignments[k], time)) {
                return 1;
            }
        }
        if (instance->areClipPrefsDirty() && !instance->getClipPreferences()) {
            std::fprintf(stderr, "Error: get clip preferences action failed\n");

            return 1;
        }
        MyHost::MyClipInstance* outputClip = dynamic_cast<MyHost::MyClipInstance*>(instance->getClip(kOfxImageEffectOutputClipName));
        if (!outputClip) {
            std::fprintf(stderr, "Error: no output clip\n");

            return 1;
        }
        if (outputClip->getPixelDepth() != kOfxBitDepthFloat) {
            std::fprintf(stderr, "Error: the output depth is %s, not float\n", outputClip->getPixelDepth().c_str());

            return 1;
        }
        instance->timeLineGotoTime(time);
        OfxRectD rodD;
        rodD.x1 = rodD.y1 = rodD.x2 = rodD.y2 = 0.;
        stat = instance->getRegionOfDefinitionAction(time, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                     0,
#endif
                                                     rodD);
        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
            std::fprintf(stderr, "Error: get region of definition action failed (status %d)\n", (int)stat);

            return 1;
        }
        const double par = instance->getProjectPixelAspectRatio();
        const OfxRectI rod = MyHost::canonicalToPixelEnclosing(rodD, renderScale, par);
        const int w = rod.x2 - rod.x1;
        const int h = rod.y2 - rod.y1;
        if (w < 6 || h < 6) {
            std::fprintf(stderr, "Error: the RoD (%d,%d,%d,%d) is too small for the test\n", rod.x1, rod.y1, rod.x2, rod.y2);

            return 1;
        }
        if (windows.empty()) {
            const int def[][4] = {
                // in the RoD: the middle quarter, and windows at the edges of the image, none at the origin
                { w / 4, h / 4, 3 * w / 4, 3 * h / 4 }, // the middle quarter
                { w / 3, h / 2 + 1, w, h }, // the top right part (odd offsets)
                { 1, 0, w / 5 + 1, h / 3 }, // along the bottom edge
                { 0, h / 2, w, h / 2 + 1 }, // one row
                { w - 1, 1, w, h - 1 }, // one column, at the right edge
                { w / 2, h / 2, w / 2 + 1, h / 2 + 1 }, // one pixel
                // partly out of the RoD
                { -3, h / 3, 2, h / 2 }, // across the left edge
                { w - 2, h - 2, w + 5, h + 4 }, // across the top right corner
                { w / 2, -4, w / 2 + 3, h + 3 }, // a few columns, longer than the RoD
                { -4, -4, w + 4, h + 4 }, // the RoD and a border
                // out of the RoD
                { w + 2, 0, w + 6, 3 }, // right of the RoD
                { 0, -5, 4, -1 }, // below the RoD
            };
            for (size_t k = 0; k < sizeof(def) / sizeof(def[0]); ++k) {
                OfxRectI r;
                r.x1 = rod.x1 + def[k][0];
                r.y1 = rod.y1 + def[k][1];
                r.x2 = rod.x1 + def[k][2];
                r.y2 = rod.y1 + def[k][3];
                windows.push_back(r);
            }
        }

        stat = instance->beginRenderAction(time, time, 1., false, renderScale, false, /*interactive=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                           /*openGLRender=*/false,
#ifdef OFX_EXTENSIONS_NATRON
                                           /*contextData=*/NULL,
#endif
#endif
                                           /*draftRender=*/false
#ifdef OFX_EXTENSIONS_NUKE
                                           ,
                                           0 /* view*/
#endif
        );
        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
            std::fprintf(stderr, "Error: begin render action failed (status %d)\n", (int)stat);

            return 1;
        }

        // the reference: the whole RoD
        MyHost::MyImage* full = render(myInstance, outputClip, time, renderScale, rod, rod);
        std::vector<unsigned char> ref;
        int nComps = 0;
        size_t pixelBytes = 0;
        if (!full) {
            ++failures;
        } else {
            nComps = full->getNComponents();
            pixelBytes = (size_t)nComps * full->getBytesPerComponent();
            ref.assign(full->getPixelData(rod.x1, rod.y1), full->getPixelData(rod.x1, rod.y1) + pixelBytes * (size_t)w * (size_t)h);
            long unwritten = 0;
            for (size_t i = 0; i + sizeof(float) <= ref.size(); i += sizeof(float)) {
                unwritten += isSentinel(&ref[i]);
            }
            if (unwritten) {
                std::fprintf(stderr, "FAIL %s: the full render left %ld components unwritten\n", what.c_str(), unwritten);
                ++failures;
            }
        }

        const bool tiles = instance->supportsTiles();
        const float zero = 0.f;
        for (size_t k = 0; full && k < windows.size(); ++k) {
            const OfxRectI& win = windows[k];
            // an image of the RoD and the window, and, with tiles, of the window only
            for (int t = 0; t < (tiles ? 2 : 1); ++t) {
                const OfxRectI bounds = (t == 0) ? boundingBox(rod, win) : win;
                MyHost::MyImage* img = render(myInstance, outputClip, time, renderScale, bounds, win);
                if (!img) {
                    std::printf("FAIL %s window (%d,%d,%d,%d): render failed\n", what.c_str(), win.x1, win.y1, win.x2, win.y2);
                    ++failures;
                    continue;
                }
                long differ = 0;
                long unwritten = 0;
                long outsideWritten = 0;
                for (int y = bounds.y1; y < bounds.y2; ++y) {
                    for (int x = bounds.x1; x < bounds.x2; ++x) {
                        const unsigned char* p = img->getPixelData(x, y);
                        const bool inside = contains(win, x, y);
                        const bool inRoD = contains(rod, x, y);
                        for (int c = 0; c < nComps; ++c) {
                            const unsigned char* pc = p + c * sizeof(float);
                            if (inside) {
                                const unsigned char* expected = inRoD ? &ref[((size_t)(y - rod.y1) * w + (x - rod.x1)) * pixelBytes + c * sizeof(float)]
                                                                      : (const unsigned char*)&zero;
                                if (isSentinel(pc)) {
                                    ++unwritten;
                                } else if (std::memcmp(pc, expected, sizeof(float)) != 0) {
                                    ++differ;
                                }
                            } else if (!isSentinel(pc)) {
                                ++outsideWritten;
                            }
                        }
                    }
                }
                const bool ok = !differ && !unwritten && !outsideWritten;
                std::printf("%s %s window (%d,%d,%d,%d) of (%d,%d,%d,%d), image %s: %ld components differ from the full render, %ld unwritten, %ld written outside\n",
                            ok ? "ok  " : "FAIL", what.c_str(), win.x1, win.y1, win.x2, win.y2, rod.x1, rod.y1, rod.x2, rod.y2,
                            (t == 0) ? "RoD+window" : "window", differ, unwritten, outsideWritten);
                if (!ok) {
                    ++failures;
                }
            }
        }
        outputClip->releaseOutputImage();
        instance->endRenderAction(time, time, 1., false, renderScale, false, /*interactive=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                  /*openGLRender=*/false,
#ifdef OFX_EXTENSIONS_NATRON
                                  /*contextData=*/NULL,
#endif
#endif
                                  /*draftRender=*/false
#ifdef OFX_EXTENSIONS_NUKE
                                  ,
                                  0 /* view*/
#endif
        );
    }
    std::printf("%s: %s\n", what.c_str(), failures ? "FAILED" : "all windows OK");

    return failures ? 1 : 0;
}
