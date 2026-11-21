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
 * raw_unload: a host that renders a RAW (DNG) file with ReadOIIO, destroys the instance, unloads the plug-ins
 * (kOfxActionUnload, then dlclose() or FreeLibrary() of the binary, PluginCache::clearPluginCache(), as hostDemo
 * does at exit and after its plug-in scan), keeps running for a while, then exits (#239).
 *
 *   raw_unload <identifier> <major> [--sleep seconds] [name=value...]
 *   raw_unload --write-dng <file> <width> <height>
 *
 * LibRaw's demosaic uses OpenMP on Linux (Ubuntu, EL9: libgomp) and MS Windows (MSYS2: libgomp-1.dll), whose worker
 * threads stay alive, idle, after the render. If unloading IO.ofx also unloads the OpenMP runtime, those threads
 * run unmapped code, and the process crashes, either while it keeps running or at exit. Run it with OMP_NUM_THREADS=4
 * (and OMP_WAIT_POLICY=active, so that the idle workers keep running the runtime's code). It is not linked with
 * OpenImageIO, LibRaw or OpenMP: nothing else keeps them loaded. Exit status: 0 on success (a crash is a signal, or
 * on MS Windows an exception exit code). If an OpenMP runtime was loaded after the render, it must still be loaded
 * after the unload: a crash is not certain, so this is checked too (it prints the runtime and the thread counts).
 *
 * --write-dng writes a small 16-bit Bayer (RGGB) DNG file, for this test: no RAW file can be committed or
 * downloaded reliably, and LibRaw reads a minimal uncompressed CFA DNG.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
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

#include <chrono>
#include <fstream>
#include <thread>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dlfcn.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#include <dirent.h>
#include <link.h>
#endif
#endif

namespace {

// the number of threads of this process (Linux: /proc/self/task; MS Windows: a ToolHelp snapshot), or -1
int
threadCount()
{
#if defined(__linux__)
    DIR* d = opendir("/proc/self/task");
    if (!d) {
        return -1;
    }
    int n = 0;
    while (struct dirent* e = readdir(d)) {
        n += (e->d_name[0] != '.') ? 1 : 0;
    }
    closedir(d);

    return n;
#elif defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return -1;
    }
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    int n = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        n += (te.th32OwnerProcessID == pid) ? 1 : 0;
    }
    CloseHandle(snap);

    return n;
#else
    return -1;
#endif
}

#if !defined(_WIN32)
// Is this the path of an OpenMP runtime (GNU libgomp, LLVM libomp, Intel libiomp5)?
bool
isOpenMPRuntime(const char* path)
{
    if (!path) {
        return false;
    }
    const char* name = std::strrchr(path, '/');
    name = name ? name + 1 : path;

    return !std::strncmp(name, "libgomp.", 8) || !std::strncmp(name, "libomp.", 7) || !std::strncmp(name, "libiomp5.", 9);
}
#endif
#if defined(__linux__)
int
findOpenMP(struct dl_phdr_info* info, size_t /*size*/, void* data)
{
    if (isOpenMPRuntime(info->dlpi_name)) {
        *(std::string*)data = info->dlpi_name;

        return 1;
    }

    return 0;
}
#endif

// The OpenMP runtime loaded in this process (its file name), or "" if none is
std::string
openMPRuntime()
{
#if defined(_WIN32)
    static const char* const names[] = { "libgomp-1.dll", "libomp.dll", "libiomp5md.dll", "vcomp140.dll" };
    for (const char* name : names) {
        if (GetModuleHandleA(name)) {
            return name;
        }
    }

    return std::string();
#elif defined(__APPLE__)
    const uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; ++i) {
        if (isOpenMPRuntime(_dyld_get_image_name(i))) {
            return _dyld_get_image_name(i);
        }
    }

    return std::string();
#else
    std::string found;
    dl_iterate_phdr(findOpenMP, &found);

    return found;
#endif
}

void
put16(std::vector<unsigned char>& b, unsigned v)
{
    b.push_back((unsigned char)(v & 0xff));
    b.push_back((unsigned char)((v >> 8) & 0xff));
}

void
put32(std::vector<unsigned char>& b, unsigned v)
{
    put16(b, v & 0xffff);
    put16(b, v >> 16);
}

// A minimal uncompressed little-endian DNG: one 16-bit CFA (RGGB) image of width x height, a smooth pattern
int
writeDNG(const char* path, int width, int height)
{
    if (width < 16 || height < 16 || width > 8192 || height > 8192) {
        std::fprintf(stderr, "Error: --write-dng <file> <width> <height>\n");

        return 2;
    }
    struct Entry {
        unsigned tag, type, count, value; // value: the value (<= 4 bytes) or the offset of the data
        std::vector<unsigned char> data; // more than 4 bytes
    };
    std::vector<Entry> e;
    auto add = [&e](unsigned tag, unsigned type, unsigned count, unsigned value) { e.push_back(Entry { tag, type, count, value, {} }); };
    auto addData = [&e](unsigned tag, unsigned type, unsigned count, const std::vector<unsigned char>& data) {
        e.push_back(Entry { tag, type, count, 0, data });
    };
    auto ascii = [](const char* s) {
        std::vector<unsigned char> d(s, s + std::strlen(s) + 1);
        return d;
    };
    const unsigned SHORT = 3, LONG = 4, ASCII = 2, BYTE = 1, SRATIONAL = 10;
    const unsigned imageBytes = (unsigned)width * (unsigned)height * 2;
    add(254, LONG, 1, 0); // NewSubFileType: main image
    add(256, LONG, 1, (unsigned)width);
    add(257, LONG, 1, (unsigned)height);
    add(258, SHORT, 1, 16); // BitsPerSample
    add(259, SHORT, 1, 1); // Compression: none
    add(262, SHORT, 1, 32803); // PhotometricInterpretation: CFA
    addData(271, ASCII, 10, ascii("openfx-io")); // Make
    addData(272, ASCII, 11, ascii("raw_unload")); // Model
    add(273, LONG, 1, 0); // StripOffsets (set below)
    add(274, SHORT, 1, 1); // Orientation
    add(277, SHORT, 1, 1); // SamplesPerPixel
    add(278, LONG, 1, (unsigned)height); // RowsPerStrip
    add(279, LONG, 1, imageBytes); // StripByteCounts
    add(284, SHORT, 1, 1); // PlanarConfiguration
    add(33421, SHORT, 2, 2 | (2u << 16)); // CFARepeatPatternDim: 2x2
    add(33422, BYTE, 4, 0 | (1u << 8) | (1u << 16) | (2u << 24)); // CFAPattern: R G / G B
    add(50706, BYTE, 4, 1 | (4u << 8)); // DNGVersion 1.4.0.0
    add(50707, BYTE, 4, 1 | (1u << 8)); // DNGBackwardVersion 1.1.0.0
    addData(50708, ASCII, 21, ascii("openfx-io raw_unload")); // UniqueCameraModel
    add(50717, LONG, 1, 65535); // WhiteLevel
    {
        std::vector<unsigned char> m; // ColorMatrix1: identity
        for (int i = 0; i < 9; ++i) {
            put32(m, (i % 4 == 0) ? 1u : 0u);
            put32(m, 1u);
        }
        addData(50721, SRATIONAL, 9, m);
    }
    add(50778, SHORT, 1, 21); // CalibrationIlluminant1: D65
    // layout: header (8), IFD, the data of the entries, the image
    const unsigned ifdSize = 2 + 12 * (unsigned)e.size() + 4;
    unsigned offset = 8 + ifdSize;
    for (Entry& x : e) {
        if (!x.data.empty()) {
            x.value = offset;
            offset += (unsigned)x.data.size() + (x.data.size() & 1);
        }
    }
    const unsigned imageOffset = offset;
    for (Entry& x : e) {
        if (x.tag == 273) {
            x.value = imageOffset;
        }
    }
    std::vector<unsigned char> b;
    b.push_back('I');
    b.push_back('I');
    put16(b, 42);
    put32(b, 8);
    put16(b, (unsigned)e.size());
    for (const Entry& x : e) {
        put16(b, x.tag);
        put16(b, x.type);
        put32(b, x.count);
        if ((x.type == SHORT) && (x.count == 1)) {
            put16(b, x.value);
            put16(b, 0);
        } else {
            put32(b, x.value);
        }
    }
    put32(b, 0); // no next IFD
    for (const Entry& x : e) {
        if (!x.data.empty()) {
            b.insert(b.end(), x.data.begin(), x.data.end());
            if (x.data.size() & 1) {
                b.push_back(0);
            }
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned v = 4096 + (unsigned)((x * 37 + y * 23 + ((x / 7) * (y / 5)) * 11) % 50000);
            put16(b, v);
        }
    }
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)b.data(), (std::streamsize)b.size());

    return f.good() ? 0 : 1;
}

} // namespace

int
main(int argc, char** argv)
{
    if (argc == 5 && std::strcmp(argv[1], "--write-dng") == 0) {
        return writeDNG(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    }
    if (argc < 3) {
        std::fprintf(stderr, "Usage: %s <identifier> <major> [--sleep seconds] [name=value...]\n", argv[0]);

        return 2;
    }
    const std::string pluginId = argv[1];
    const int pluginMajor = std::atoi(argv[2]);
    double sleepSeconds = 3.;
    std::vector<std::string> assignments;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--sleep") && i + 1 < argc) {
            sleepSeconds = std::atof(argv[++i]);
        } else {
            assignments.push_back(argv[i]);
        }
    }
    const int threadsAtStart = threadCount();

    OFX::Host::PluginCache::useStdOFXPluginsLocation(false);
    OFX::Host::PluginCache::getPluginCache()->setCacheVersion("ioRawUnloadV1");
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
    int threadsAfterRender = -1;
    std::string runtimeAfterRender;
    {
        OFX::Host::auto_ptr<OFX::Host::ImageEffect::Instance> instance(plugin->createInstance(kOfxImageEffectContextReader, NULL));
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
        for (size_t i = 0; i < assignments.size(); ++i) {
            const std::string& a = assignments[i];
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
        threadsAfterRender = threadCount();
        runtimeAfterRender = openMPRuntime();
    } // the instance is destroyed (kOfxActionDestroyInstance)
    // the instance is destroyed: unload the plug-ins (kOfxActionUnload, and the binary)
    OFX::Host::PluginCache::clearPluginCache();
    const std::string runtimeAfterUnload = openMPRuntime();
    std::printf("threads: %d at start, %d after the render, %d after the unload (-1: not counted); OpenMP runtime: "
                "%s after the render, %s after the unload\n",
                threadsAtStart, threadsAfterRender, threadCount(), runtimeAfterRender.empty() ? "none" : runtimeAfterRender.c_str(),
                runtimeAfterUnload.empty() ? "none" : runtimeAfterUnload.c_str());
    // its idle workers outlive the render: if the runtime was unloaded with IO.ofx, they run unmapped code (#239)
    if (!runtimeAfterRender.empty() && runtimeAfterUnload.empty()) {
        std::printf("FAIL: the OpenMP runtime (%s) was unloaded with the plug-ins\n", runtimeAfterRender.c_str());
        ++failures;
    }
    std::fflush(stdout);
    // keep running (any thread left running the code of an unloaded library crashes the process)
    std::this_thread::sleep_for(std::chrono::milliseconds((long)(sleepSeconds * 1000.)));
    std::printf("exiting after the unload: %s\n", failures ? "FAILED" : "OK");
    std::fflush(stdout);

    return failures ? 1 : 0;
}
