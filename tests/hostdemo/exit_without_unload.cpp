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
 * exit_without_unload: a host that renders a reader, destroys its instance, and exits WITHOUT sending
 * kOfxActionUnload (#250). The spec allows it for a host that never unloads the binary, and Natron does it when a
 * background script ends with sys.exit(). OpenImageIO requires its thread pools to be shut down before exit
 * ("Failing to call `OIIO::shutdown` could lead to a sporadic dead-lock during application shutdown on certain
 * platforms such as Windows", imageio.h, OpenImageIO 3.2.2): the OIIO plug-ins stop them when their last instance is
 * destroyed (OIIOGlobal.h), not only in kOfxActionUnload.
 *
 *   exit_without_unload <identifier> <major> [name=value...]
 *
 * A host built on openfx's HostSupport and hostDemo's host classes, linked with OpenImageIO. It creates the
 * plug-in in the reader context, sets the parameters (string params only, e.g. filename=...), renders the RoD at
 * time 0, destroys the instance, and returns from main() without unloading the plug-ins. The test is that the
 * process exits (tests/hostdemo/exit_without_unload.sh runs it with a timeout).
 * If the plug-in uses the same OpenImageIO library as this program (a bundle that is not deployed with its own
 * copy), and OpenImageIO can stop its pool (2.5.3 or later), it also checks OpenImageIO's default thread pool:
 * hardware_concurrency() - 1 threads while the instance exists, and 0 after it is destroyed, before exit. Exit
 * status: 0 on success.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <memory>
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

#include <OpenImageIO/oiioversion.h>
#include <OpenImageIO/sysutil.h>
#include <OpenImageIO/thread.h>

namespace {

// OpenImageIO >= 2.5.3 (or a patched version) can stop its pool (OIIOGlobal.cpp): with an older one (e.g. 2.4, in
// Ubuntu 24.04), shutdownOIIOThreads() can't, and the pool size after the destruction is not checked
#if defined(NATRON_OIIO_HAS_DEFAULT_THREAD_POOL_SHUTDOWN) || OIIO_VERSION >= OIIO_MAKE_VERSION(2, 5, 3)
const bool kHasPoolShutdown = true;
#else
const bool kHasPoolShutdown = false;
#endif

int
poolSize()
{
    return OIIO::default_thread_pool()->size();
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <identifier> <major> [name=value...]\n", argv[0]);

        return 2;
    }
    const std::string pluginId = argv[1];
    const int pluginMajor = std::atoi(argv[2]);
    const int expected = (int)OIIO::Sysutil::hardware_concurrency() - 1;

    OFX::Host::PluginCache::useStdOFXPluginsLocation(false);
    OFX::Host::PluginCache::getPluginCache()->setCacheVersion("ioExitWithoutUnloadV1");
    MyHost::Host myHost;
    OFX::Host::ImageEffect::PluginCache imageEffectPluginCache(&myHost);
    imageEffectPluginCache.registerInCache(*OFX::Host::PluginCache::getPluginCache());
    OFX::Host::PluginCache::getPluginCache()->scanPluginFiles();
    OFX::Host::ImageEffect::ImageEffectPlugin* plugin = imageEffectPluginCache.getPluginById(pluginId, pluginMajor);
    if (!plugin) {
        std::fprintf(stderr, "Error: %s v%d not found in OFX_PLUGIN_PATH\n", pluginId.c_str(), pluginMajor);

        return 1;
    }

    int failures = 0;
    int sizeWithInstance = -1;
    {
        std::unique_ptr<OFX::Host::ImageEffect::Instance> instance(plugin->createInstance(kOfxImageEffectContextReader, NULL));
        MyHost::MyEffectInstance* myInstance = dynamic_cast<MyHost::MyEffectInstance*>(instance.get());
        if (!myInstance) {
            std::fprintf(stderr, "Error: could not create an instance\n");

            return 1;
        }
        myInstance->setDefaultDepth(kOfxBitDepthFloat);
        OfxStatus stat = instance->createInstanceAction();
        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
            std::fprintf(stderr, "Error: create instance action failed (status %d)\n", (int)stat);

            return 1;
        }
        for (int i = 3; i < argc; ++i) {
            const std::string a = argv[i];
            const std::string::size_type eq = a.find('=');
            OFX::Host::Param::StringInstance* p = (eq == std::string::npos) ? NULL : dynamic_cast<OFX::Host::Param::StringInstance*>(instance->getParam(a.substr(0, eq)));
            if (!p || p->set(a.substr(eq + 1).c_str()) != kOfxStatOK) {
                std::fprintf(stderr, "Error: could not set the string param %s\n", a.c_str());

                return 1;
            }
            OfxPointD scale;
            scale.x = scale.y = 1.;
            instance->beginInstanceChangedAction(kOfxChangeUserEdited);
            instance->paramInstanceChangedAction(a.substr(0, eq), kOfxChangeUserEdited, 0., scale);
            instance->endInstanceChangedAction(kOfxChangeUserEdited);
        }
        if (!instance->getClipPreferences()) {
            std::fprintf(stderr, "Error: get clip preferences action failed\n");

            return 1;
        }
        MyHost::MyClipInstance* outputClip = dynamic_cast<MyHost::MyClipInstance*>(instance->getClip(kOfxImageEffectOutputClipName));
        OfxPointD renderScale;
        renderScale.x = renderScale.y = 1.;
        instance->timeLineGotoTime(0.);
        OfxRectD rodD = { 0., 0., 0., 0. };
        stat = instance->getRegionOfDefinitionAction(0., renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                     0,
#endif
                                                     rodD);
        if (!outputClip || (stat != kOfxStatOK && stat != kOfxStatReplyDefault)) {
            std::fprintf(stderr, "Error: no output clip, or get region of definition action failed (status %d)\n", (int)stat);

            return 1;
        }
        const OfxRectI rod = MyHost::canonicalToPixelEnclosing(rodD, renderScale, instance->getProjectPixelAspectRatio());
        std::map<OFX::Host::ImageEffect::ClipInstance*, OfxRectD> rois;
        instance->beginRenderAction(0., 0., 1., false, renderScale, false, /*interactive=*/false,
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
        myInstance->setRenderState(renderScale, rod, rois);
#ifdef OFX_EXTENSIONS_NUKE
        std::list<std::string> planes;
        planes.push_back(kFnOfxImagePlaneColour);
#endif
        stat = instance->renderAction(0., kOfxImageFieldNone, rod, renderScale, /*sequentialRender=*/false, /*interactiveRender=*/false,
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
        myInstance->clearRenderState();
        instance->endRenderAction(0., 0., 1., false, renderScale, false, /*interactive=*/false,
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
        if (stat != kOfxStatOK || !outputClip->getOutputImage()) {
            std::printf("FAIL: render of (%d,%d,%d,%d) (status %d)\n", rod.x1, rod.y1, rod.x2, rod.y2, (int)stat);
            ++failures;
        } else {
            std::printf("ok   render of (%d,%d,%d,%d)\n", rod.x1, rod.y1, rod.x2, rod.y2);
        }
        outputClip->releaseOutputImage();
        sizeWithInstance = poolSize();
    } // the instance is destroyed (kOfxActionDestroyInstance)
    const int sizeAfter = poolSize();
    if (!kHasPoolShutdown) {
        std::printf("note OpenImageIO %s has no default_thread_pool_shutdown(): the pool size is not checked "
                    "(%d threads with the instance, %d after it was destroyed)\n",
                    OIIO_VERSION_STRING, sizeWithInstance, sizeAfter);
    } else if ((expected > 0) && (sizeWithInstance == expected)) {
        // the plug-in uses this program's OpenImageIO
        std::printf("%s OpenImageIO's pool: %d threads with the instance, %d after it was destroyed (0 expected)\n",
                    sizeAfter == 0 ? "ok  " : "FAIL", sizeWithInstance, sizeAfter);
        failures += (sizeAfter == 0) ? 0 : 1;
    } else {
        std::printf("note OpenImageIO's pool: %d threads with the instance (%d expected): the plug-in has its own "
                    "OpenImageIO, the pool size is not checked\n",
                    sizeWithInstance, expected);
    }
    // no PluginCache::clearPluginCache(): the plug-ins never get kOfxActionUnload
    std::printf("exiting without kOfxActionUnload: %s\n", failures ? "FAILED" : "OK");
    std::fflush(stdout);

    return failures ? 1 : 0;
}
