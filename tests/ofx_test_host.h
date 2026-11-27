/*
 * ofx_test_host.h — a minimal in-process OFX host for the test programs that link the plug-in sources
 * directly (ofx_render_test.cpp, reader_hooks_test.cpp, ofx14_host_test.cpp, ...).
 *
 * Implements the Property, ImageEffect, Parameter, Memory, MultiThread, Message and
 * Interact suites, plus FnOfxImageEffectPlaneSuiteV2, over a generic property bag:
 * param values (definition defaults or host overrides), image clips backed by a pixel
 * buffer, and a few helpers to load/describe a plugin and build action arguments.
 * The plug-ins get the host through OfxPlugin::setHost (loadAndDescribe), never OfxSetHost.
 *
 * OFX 1.4-only mode (gOfx14Only = true, before initHost()): the host is one that supports
 * OFX 1.4 and nothing newer, and no extension. fetchSuite returns NULL for every suite but
 * the mandatory ones of OFX 1.4 (Property, ImageEffect, Parameter, Memory, MultiThread V1,
 * Message V1 and V2, Interact V1; Message V2 only from OFX 1.2, see OFX14_LEVEL), and every property suite function
 * fails with
 * kOfxStatErrUnknown on a property that is not defined in the OFX 1.4 headers
 * (ofx14_names.h), on any property set (host, effect, clip, param, image, action
 * arguments): reading it fails, and setting it stores nothing. The host has no Natron,
 * Nuke, Vegas or other extension property, and no OpenGL render suite (unless OFX14_GL is set: it is optional in
 * OFX 1.4).
 * Param values are not animated: getValueAtTime() ignores the time, except for a string
 * param whose stringAtTime is set (e.g. a file param naming each frame of an image sequence), and a one-dimensional
 * double, integer, boolean or choice param whose numberAtTime is set.
 *
 * Everything is in an anonymous namespace: include it in a single translation unit
 * of each test program.
 */
#ifndef IO_TESTS_OFX_TEST_HOST_H
#define IO_TESTS_OFX_TEST_HOST_H

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "ofxGPURender.h" // OfxImageEffectOpenGLRenderSuiteV1 (OFX14_GL)
#include "nuke/fnOfxExtensions.h" // kOfxImageEffectPropRenderPlanes, kFnOfxImagePlaneColour, kFnOfxImageEffectPropView
#include "ofxNatron.h" // kNatronOfxParamProcessR/G/B/A

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "ofx14_names.h"

extern "C" {
int OfxGetNumberOfPlugins(void);
OfxPlugin* OfxGetPlugin(int nth);
}

namespace {

// ------------------------------------------------------------------ property bag
struct Value {
    std::vector<int> i;
    std::vector<double> d;
    std::vector<std::string> s;
    std::vector<void*> p;
};
struct PropSet {
    // the object that the property set belongs to (for gDefaultUnsetProps): action arguments by default
    enum Kind { eArgs, eHost, eEffect, eClip, eImage, eParam, eInteract };
    explicit PropSet(Kind k = eArgs) : kind(k) {}
    std::map<std::string, Value> props;
    Kind kind;
};

struct ParamRec {
    std::string type;
    std::string name;
    PropSet* props = nullptr; // definition props (holds kOfxParamPropDefault)
    Value ov;                 // host override value (optional)
    bool hasOv = false;
    std::function<std::string(double)> stringAtTime; // string param: the value returned by getValueAtTime() (optional)
    std::function<double(double)> numberAtTime; // 1-D double, integer, boolean or choice param: likewise (optional)
    int nSet = 0;             // number of paramSetValue/paramSetValueAtTime calls by the plug-in
};

struct Clip {
    std::string name;
    PropSet props{PropSet::eClip};   // clip props (components, depth, connected, ...)
    PropSet image{PropSet::eImage};  // image props returned by clipGetImage
    std::vector<uint8_t> buffer; // pixel buffer backing the image
    bool connected = false;
    bool noImage = false;     // clipGetImage fails with kOfxStatFailed ("assume black and transparent")
};

struct Effect {
    PropSet props{PropSet::eEffect};        // effect props (context, instance data, ...)
    std::map<std::string, ParamRec> params; // by name
    std::map<std::string, Clip*> clips;     // by name
    std::vector<PropSet*> owned;
    Effect* delegate = nullptr;             // instance -> context descriptor for params/clips
    ~Effect() { for (PropSet* ps : owned) delete ps; for (auto& kv : clips) delete kv.second; }
    PropSet* newChild() { PropSet* ps = new PropSet(PropSet::eParam); owned.push_back(ps); return ps; } // (params)
    Effect* src() { return delegate ? delegate : this; }
};

PropSet gHostProps(PropSet::eHost);
OfxHost gOfxHost;

// OFX 1.4-only host mode (see the top of this file). Set it before initHost().
bool gOfx14Only = false;

// Older hosts and other host variants, from the environment (read by initHost(), see tests/README.md):
int gOfxLevel = 14;                  // OFX14_LEVEL: 10, 11, 12, 13 or 14
std::set<std::string> gNoSuites;     // OFX14_NOSUITE: "name Vn" or "name", comma-separated
std::set<std::string> gExtraUnknown; // names added after gOfxLevel
// Honour kOfxParamPropDefaultCoordinateSystem: the value of a param with a normalised default, never set, is its
// default in canonical coordinates (project size * default, plus the project offset for an absolute position), as
// in Natron and Nuke. Off by default: the other tests read the defaults as described.
bool gHonourNormalisedDefaults = false;
double gProjectSize[2] = { 0., 0. };   // of the last setInstanceProps()
double gProjectOffset[2] = { 0., 0. };
double gRS = 1.;                     // OFX14_SCALE
bool gOpenGLRender = false;          // OFX14_GL: the host has the OpenGL render suite (optional in OFX 1.4)
// Is the property name defined by OFX 1.4 (or by the older version of OFX14_LEVEL)?
bool isOfx14Name(const char* n)
{
    if (gExtraUnknown.count(n)) return false;
    // the names of OFX 1.4's ofxOpenGLRender.h, which ofx14_names.h leaves out
    static const std::set<std::string> glNames = {
        "OfxImageEffectOpenGLRenderSuite", "OfxBitDepthHalf", "OfxImageEffectPropOpenGLRenderSupported",
        "OfxOpenGLPropPixelDepth", "OfxImageEffectPropOpenGLEnabled", "OfxImageEffectPropOpenGLTextureIndex",
        "OfxImageEffectPropOpenGLTextureTarget", "OfxActionOpenGLContextAttached", "kOfxActionOpenGLContextDetached"
    };
    if (gOpenGLRender && glNames.count(n)) return true;
    static const std::set<std::string> names(std::begin(kOfx14Names), std::end(kOfx14Names));
    if (names.count(n)) return true;
    for (const char* prefix : kOfx14ClipPrefixes) {
        if (!std::strncmp(n, prefix, std::strlen(prefix))) return true;
    }
    return false;
}

// Every property access to a name that the host doesn't know fails with this status (kOfxStatOK: known).
// With gDefaultUnsetProps, a known property that was never set has a default value (0, "" or NULL) at index 0, as in
// a real host, which defines every property of a property set: e.g. kOfxParamPropHint, which
// ChoiceParamDescriptor::appendOption() reads when the host has no kOfxParamPropChoiceLabelOption (an extension), or
// kOfxParamPropSecret. (Otherwise, the default, reading it fails.)
// The default is only on the property sets that a property belongs to (OFX 1.4 ofxImageEffect.h, ofxParam.h,
// ofxInteract.h): e.g. kOfxParamProp* on a param, kOfxImageClipProp* on a clip. On the host and on the action
// arguments, which the host sets entirely, and for a property of another property set, reading it fails with
// kOfxStatErrUnknown, and so does propGetDimension. The dimension of a defaulted property is 1, or 0 for the
// properties of variable dimension (e.g. kOfxParamPropChoiceOption).
bool gDefaultUnsetProps = false;

bool startsWith(const char* n, const char* prefix) { return !std::strncmp(n, prefix, std::strlen(prefix)); }

// Does the property n belong to the property set ps (gDefaultUnsetProps)?
bool belongsTo(const PropSet* ps, const char* n)
{
    static const std::set<std::string> labels = { kOfxPropLabel, kOfxPropShortLabel, kOfxPropLongLabel, kOfxPropName, kOfxPropType };
    static const std::set<std::string> clipEffect = {
        kOfxImageEffectPropComponents, kOfxImageEffectPropPixelDepth, kOfxImageEffectPropPreMultiplication,
        kOfxImageEffectPropFrameRate, kOfxImageEffectPropFrameRange, kOfxImageEffectPropUnmappedFrameRate,
        kOfxImageEffectPropUnmappedFrameRange, kOfxImageEffectPropSupportedComponents,
        kOfxImageEffectPropTemporalClipAccess, kOfxImageEffectPropSupportsTiles, kOfxImagePropPixelAspectRatio
    };
    switch (ps->kind) {
    case PropSet::eParam:
        return startsWith(n, "OfxParamProp") || labels.count(n) || !std::strcmp(n, kOfxPropIcon) ||
               !std::strcmp(n, kOfxPropInstanceData) || !std::strcmp(n, kOfxParamPropUseHostOverlayHandle);
    case PropSet::eEffect:
        return startsWith(n, "OfxImageEffectPluginProp") || startsWith(n, "OfxImageEffectProp") ||
               startsWith(n, "OfxImageEffectInstanceProp") || startsWith(n, "OfxPluginProp") || labels.count(n) ||
               !std::strcmp(n, kOfxImageEffectPluginRenderThreadSafety) || !std::strcmp(n, kOfxPropVersion) ||
               !std::strcmp(n, kOfxPropVersionLabel) || !std::strcmp(n, kOfxPropPluginDescription) ||
               !std::strcmp(n, kOfxPropInstanceData) || !std::strcmp(n, kOfxPropIsInteractive) ||
               !std::strcmp(n, kOfxPropParamSetNeedsSyncing);
    case PropSet::eClip:
        return startsWith(n, "OfxImageClipProp") || clipEffect.count(n) || labels.count(n);
    case PropSet::eImage:
        return startsWith(n, "OfxImageProp") || clipEffect.count(n) || !std::strcmp(n, kOfxImageEffectPropRenderScale) ||
               !std::strcmp(n, kOfxPropType);
    case PropSet::eInteract:
        return startsWith(n, "OfxInteractProp") || !std::strcmp(n, kOfxPropEffectInstance) || !std::strcmp(n, kOfxPropInstanceData);
    case PropSet::eHost:
    case PropSet::eArgs:
        break;
    }
    return false;
}

// the defaulted properties of variable dimension: their default dimension is 0
bool variableDimension(const char* n)
{
    static const std::set<std::string> names = {
        kOfxParamPropChoiceOption, kOfxParamPropPageChild, kOfxImageEffectPropSupportedContexts,
        kOfxImageEffectPropSupportedPixelDepths, kOfxImageEffectPropSupportedComponents,
        kOfxImageEffectPropClipPreferencesSlaveParam, kOfxPluginPropParamPageOrder, "OfxOpenGLPropPixelDepth"
    };
    return names.count(n) != 0;
}

// gDefaultUnsetProps: is the unset property n of ps defaulted?
bool defaulted(OfxPropertySetHandle h, const char* n) { return gDefaultUnsetProps && belongsTo(reinterpret_cast<const PropSet*>(h), n) && !variableDimension(n); }
OfxStatus unknownProp(const char* n) { return (gOfx14Only && !isOfx14Name(n)) ? kOfxStatErrUnknown : kOfxStatOK; }
#define CHECK_PROP_KNOWN(n) do { if (unknownProp(n) != kOfxStatOK) return kOfxStatErrUnknown; } while (0)

// The host's data (the property sets and the param values) is read and written under gHostDataMutex, for the duration
// of each property or param suite call, as the OFX spec asks of a host whose main and render threads may access a
// parameter simultaneously ("block write/read access, not block on read/read access, be fine grained at the level of
// individual function calls", Thread and Recursion Safety): e.g. render threads read the params while the main thread
// sets them in instanceChanged (oiio_layer_menu_test). A thread takes it once: the calls that the suite functions make
// to each other (e.g. pSetStringN to pSetString) don't lock it again. A string that a property getter returns points
// into the property set: it is valid until that property is set or reset (a string param's value is a copy, see
// prmGetV). The tests that write the host's data directly (setOverrideString...) do it while no render runs.
// (A single global lock synchronizes more than a real host does: it could hide a plug-in's data race from TSan in
// other tests.)
inline std::shared_mutex gHostDataMutex;
inline thread_local int gHostDataDepth = 0;            // the calls of this thread that hold gHostDataMutex
inline thread_local bool gHostDataExclusive = false;   // the mode of the outermost one, which locked it
template <bool Exclusive>
class HostDataLock {
public:
    HostDataLock()
    {
        if (gHostDataDepth == 0) {
            if constexpr (Exclusive) gHostDataMutex.lock(); else gHostDataMutex.lock_shared();
            gHostDataExclusive = Exclusive;
        } else {
            assert(!Exclusive || gHostDataExclusive); // never a write in a call that holds the lock shared
        }
        ++gHostDataDepth;
    }
    ~HostDataLock()
    {
        if (--gHostDataDepth == 0) {
            if constexpr (Exclusive) gHostDataMutex.unlock(); else gHostDataMutex.unlock_shared();
        }
    }
    HostDataLock(const HostDataLock&) = delete;
    HostDataLock& operator=(const HostDataLock&) = delete;
};
using HostRead = HostDataLock<false>;
using HostWrite = HostDataLock<true>;

inline PropSet* PS(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
inline Effect* EF(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
inline OfxPropertySetHandle PH(PropSet* ps) { return reinterpret_cast<OfxPropertySetHandle>(ps); }
inline OfxImageEffectHandle EH(Effect& e) { return reinterpret_cast<OfxImageEffectHandle>(&e); }

// ------------------------------------------------------------------ property suite
OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int idx, void* v)
{ HostWrite lock; CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.p.size() <= idx) a.p.resize(idx + 1); a.p[idx] = v; return kOfxStatOK; }
OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int idx, const char* v)
{ HostWrite lock; CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.s.size() <= idx) a.s.resize(idx + 1); a.s[idx] = v ? v : ""; return kOfxStatOK; }
OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int idx, double v)
{ HostWrite lock; CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.d.size() <= idx) a.d.resize(idx + 1); a.d[idx] = v; return kOfxStatOK; }
OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int idx, int v)
{ HostWrite lock; CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.i.size() <= idx) a.i.resize(idx + 1); a.i[idx] = v; return kOfxStatOK; }
// (setting N values sets the dimension of the property to N, e.g. 0 to empty a choice param's options)
OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { HostWrite lock; CHECK_PROP_KNOWN(n); PS(h)->props[n].p.resize(c); for (int k = 0; k < c; ++k) pSetPointer(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { HostWrite lock; CHECK_PROP_KNOWN(n); PS(h)->props[n].s.resize(c); for (int k = 0; k < c; ++k) pSetString(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) { HostWrite lock; CHECK_PROP_KNOWN(n); PS(h)->props[n].d.resize(c); for (int k = 0; k < c; ++k) pSetDouble(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { HostWrite lock; CHECK_PROP_KNOWN(n); PS(h)->props[n].i.resize(c); for (int k = 0; k < c; ++k) pSetInt(h, n, k, v[k]); return kOfxStatOK; }

OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int idx, void** v)
{ HostRead lock; CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() && idx == 0 && defaulted(h, n)) { *v = nullptr; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.p.size() <= idx) return kOfxStatErrUnknown; *v = it->second.p[idx]; return kOfxStatOK; }
OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int idx, const char** v)
{ HostRead lock; CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() && idx == 0 && defaulted(h, n)) { *v = ""; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.s.size() <= idx) return kOfxStatErrUnknown; *v = it->second.s[idx].c_str(); return kOfxStatOK; }
OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int idx, double* v)
{ HostRead lock; CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() && idx == 0 && defaulted(h, n)) { *v = 0.; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.d.size() <= idx) return kOfxStatErrUnknown; *v = it->second.d[idx]; return kOfxStatOK; }
OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int idx, int* v)
{ HostRead lock; CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() && idx == 0 && defaulted(h, n)) { *v = 0; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.i.size() <= idx) return kOfxStatErrUnknown; *v = it->second.i[idx]; return kOfxStatOK; }
OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) { HostRead lock; for (int k = 0; k < c; ++k) if (pGetPointer(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, const char** v) { HostRead lock; for (int k = 0; k < c; ++k) if (pGetString(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) { HostRead lock; for (int k = 0; k < c; ++k) if (pGetDouble(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { HostRead lock; for (int k = 0; k < c; ++k) if (pGetInt(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pReset(OfxPropertySetHandle h, const char* n) { HostWrite lock; CHECK_PROP_KNOWN(n); PS(h)->props.erase(n); return kOfxStatOK; }
OfxStatus pGetDimension(OfxPropertySetHandle h, const char* n, int* c)
{ HostRead lock; CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n);
  if (it == PS(h)->props.end()) {
      if (!gDefaultUnsetProps) { *c = 0; return kOfxStatOK; }
      if (!belongsTo(PS(h), n)) return kOfxStatErrUnknown; // (as its getters)
      *c = variableDimension(n) ? 0 : 1; return kOfxStatOK;
  }
  const Value& v = it->second; size_t m = v.i.size(); if (v.d.size() > m) m = v.d.size(); if (v.s.size() > m) m = v.s.size(); if (v.p.size() > m) m = v.p.size(); *c = (int)m; return kOfxStatOK; }

OfxPropertySuiteV1 gPropSuite = {
    pSetPointer, pSetString, pSetDouble, pSetInt, pSetPointerN, pSetStringN, pSetDoubleN, pSetIntN,
    pGetPointer, pGetString, pGetDouble, pGetInt, pGetPointerN, pGetStringN, pGetDoubleN, pGetIntN,
    pReset, pGetDimension
};

// ------------------------------------------------------------------ image effect suite
OfxStatus ieGetPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* out) { *out = PH(&EF(e)->props); return kOfxStatOK; }
OfxStatus ieGetParamSet(OfxImageEffectHandle e, OfxParamSetHandle* out) { *out = reinterpret_cast<OfxParamSetHandle>(EF(e)->src()); return kOfxStatOK; }
OfxStatus ieClipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* out)
{
    Effect* eff = EF(e);
    Clip* c = new Clip();
    c->name = name;
    eff->clips[name] = c;
    // Sensible defaults so the plugin never reads an unknown clip property before the
    // host has configured the clip image (setupClipImage may override these later).
    OfxPropertySetHandle cp = PH(&c->props);
    pSetInt(cp, kOfxImageClipPropConnected, 0, 0);
    pSetString(cp, kOfxImageEffectPropComponents, 0, kOfxImageComponentRGBA);
    pSetString(cp, kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
    pSetString(cp, kOfxImageEffectPropPreMultiplication, 0, kOfxImageOpaque);
    pSetDouble(cp, kOfxImagePropPixelAspectRatio, 0, 1.);
    pSetString(cp, kOfxImageClipPropFieldOrder, 0, kOfxImageFieldNone);
    pSetInt(cp, kOfxImageClipPropContinuousSamples, 0, 0);
    double rod[4] = { 0., 0., 1., 1. }; pSetDoubleN(cp, kOfxImageEffectPropRegionOfDefinition, 4, rod);
    double fr[2] = { 1., 1000. }; pSetDoubleN(cp, kOfxImageEffectPropFrameRange, 2, fr);
    if (out) *out = PH(&c->props);
    return kOfxStatOK;
}
OfxStatus ieClipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* clip, OfxPropertySetHandle* props)
{ Effect* eff = EF(e)->src(); auto it = eff->clips.find(name); if (it == eff->clips.end()) return kOfxStatErrBadIndex; if (clip) *clip = reinterpret_cast<OfxImageClipHandle>(it->second); if (props) *props = PH(&it->second->props); return kOfxStatOK; }
OfxStatus ieClipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* out) { *out = PH(&reinterpret_cast<Clip*>(c)->props); return kOfxStatOK; }
// If set, the image that clipGetImage returns on this thread, whatever the clip: concurrent renders of an instance
// (each thread renders into its own image)
thread_local PropSet* tThreadImage = nullptr;
// Sets tThreadImage for its lifetime (on this thread)
struct ThreadImageGuard {
    explicit ThreadImageGuard(PropSet* image) { tThreadImage = image; }
    ~ThreadImageGuard() { tThreadImage = nullptr; }
    ThreadImageGuard(const ThreadImageGuard&) = delete;
    ThreadImageGuard& operator=(const ThreadImageGuard&) = delete;
};
OfxStatus ieClipGetImage(OfxImageClipHandle c, OfxTime, const OfxRectD*, OfxPropertySetHandle* out) { if (reinterpret_cast<Clip*>(c)->noImage) return kOfxStatFailed; if (out) *out = PH(tThreadImage ? tThreadImage : &reinterpret_cast<Clip*>(c)->image); return kOfxStatOK; }
OfxStatus ieClipReleaseImage(OfxPropertySetHandle) { return kOfxStatOK; }
OfxStatus ieClipGetRoD(OfxImageClipHandle c, OfxTime, OfxRectD* rod)
{
    double r[4];
    if (pGetDoubleN(PH(&reinterpret_cast<Clip*>(c)->props), kOfxImageEffectPropRegionOfDefinition, 4, r) != kOfxStatOK) return kOfxStatFailed;
    *rod = { r[0], r[1], r[2], r[3] };
    return kOfxStatOK;
}
std::atomic<int> gAbort { 0 }; // what ieAbort() returns (a render the host is aborting); read by the render threads
int ieAbort(OfxImageEffectHandle) { return gAbort; }
OfxStatus ieMemAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h) { *h = reinterpret_cast<OfxImageMemoryHandle>(malloc(n)); return *h ? kOfxStatOK : kOfxStatErrMemory; }
OfxStatus ieMemFree(OfxImageMemoryHandle h) { free(reinterpret_cast<void*>(h)); return kOfxStatOK; }
OfxStatus ieMemLock(OfxImageMemoryHandle h, void** p) { *p = reinterpret_cast<void*>(h); return kOfxStatOK; }
OfxStatus ieMemUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

OfxImageEffectSuiteV1 gEffectSuite = {
    ieGetPropertySet, ieGetParamSet, ieClipDefine, ieClipGetHandle, ieClipGetPropertySet,
    ieClipGetImage, ieClipReleaseImage, ieClipGetRoD, ieAbort, ieMemAlloc, ieMemFree, ieMemLock, ieMemUnlock
};

// ------------------------------------------------------------------ parameter suite
OfxStatus prmDefine(OfxParamSetHandle set, const char* type, const char* name, OfxPropertySetHandle* out)
{ Effect* e = reinterpret_cast<Effect*>(set); PropSet* ps = e->newChild(); ParamRec& r = e->params[name]; r.type = type; r.name = name; r.props = ps; pSetString(PH(ps), kOfxParamPropType, 0, type); if (out) *out = PH(ps); return kOfxStatOK; }
OfxStatus prmGetHandle(OfxParamSetHandle set, const char* name, OfxParamHandle* h, OfxPropertySetHandle* ps)
{ Effect* e = reinterpret_cast<Effect*>(set); auto it = e->params.find(name); if (it == e->params.end()) return kOfxStatErrUnknown; if (h) *h = reinterpret_cast<OfxParamHandle>(&it->second); if (ps) *ps = PH(it->second.props); return kOfxStatOK; }
OfxStatus prmSetGetPropertySet(OfxParamSetHandle set, OfxPropertySetHandle* ps) { *ps = PH(&reinterpret_cast<Effect*>(set)->props); return kOfxStatOK; }
OfxStatus prmGetPropertySet(OfxParamHandle p, OfxPropertySetHandle* ps) { *ps = PH(reinterpret_cast<ParamRec*>(p)->props); return kOfxStatOK; }

double paramDouble(ParamRec* r, int idx)
{ if (r->hasOv && (int)r->ov.d.size() > idx) return r->ov.d[idx];
  double v = 0; pGetDouble(PH(r->props), kOfxParamPropDefault, idx, &v);
  const char* cs = nullptr; const char* dt = nullptr;
  if (gHonourNormalisedDefaults && (pGetString(PH(r->props), kOfxParamPropDefaultCoordinateSystem, 0, &cs) == kOfxStatOK) && cs
      && !std::strcmp(cs, kOfxParamCoordinatesNormalised) && (pGetString(PH(r->props), kOfxParamPropDoubleType, 0, &dt) == kOfxStatOK) && dt) {
      // "you use the project size and offset" (ofxParam.h, spatial parameters)
      const std::string t = dt;
      const int axis = (t == kOfxParamDoubleTypeY || t == kOfxParamDoubleTypeYAbsolute) ? 1 : idx;
      const bool absolute = (t == kOfxParamDoubleTypeXAbsolute || t == kOfxParamDoubleTypeYAbsolute || t == kOfxParamDoubleTypeXYAbsolute);
      if (axis < 2 && (t == kOfxParamDoubleTypeX || t == kOfxParamDoubleTypeY || t == kOfxParamDoubleTypeXY || absolute)) {
          v = v * gProjectSize[axis] + (absolute ? gProjectOffset[axis] : 0.);
      }
  }
  return v; }
int paramInt(ParamRec* r, int idx)
{ if (r->hasOv && (int)r->ov.i.size() > idx) return r->ov.i[idx];
  int v = 0; pGetInt(PH(r->props), kOfxParamPropDefault, idx, &v); return v; }
const char* paramString(ParamRec* r)
{ if (r->hasOv && !r->ov.s.empty()) return r->ov.s[0].c_str();
  const char* v = ""; pGetString(PH(r->props), kOfxParamPropDefault, 0, &v); return v; }

OfxStatus prmGetV(OfxParamHandle ph, va_list ap)
{
    HostRead lock;
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    const std::string& t = r->type;
    // a string: a copy, valid until the next call in this thread (another thread may set the param meanwhile)
    if (t == kOfxParamTypeString) { thread_local std::string value; value = paramString(r); const char** o = va_arg(ap, const char**); *o = value.c_str(); }
    else if (t == kOfxParamTypeDouble) { double* o = va_arg(ap, double*); *o = paramDouble(r, 0); }
    else if (t == kOfxParamTypeDouble2D) { double* a = va_arg(ap, double*); double* b = va_arg(ap, double*); *a = paramDouble(r, 0); *b = paramDouble(r, 1); }
    else if (t == kOfxParamTypeDouble3D) { double* a = va_arg(ap, double*); double* b = va_arg(ap, double*); double* c = va_arg(ap, double*); *a = paramDouble(r, 0); *b = paramDouble(r, 1); *c = paramDouble(r, 2); }
    else if (t == kOfxParamTypeRGBA) { double* a = va_arg(ap, double*); double* b = va_arg(ap, double*); double* c = va_arg(ap, double*); double* dd = va_arg(ap, double*); *a = paramDouble(r, 0); *b = paramDouble(r, 1); *c = paramDouble(r, 2); *dd = paramDouble(r, 3); }
    else if (t == kOfxParamTypeRGB) { double* a = va_arg(ap, double*); double* b = va_arg(ap, double*); double* c = va_arg(ap, double*); *a = paramDouble(r, 0); *b = paramDouble(r, 1); *c = paramDouble(r, 2); }
    else if (t == kOfxParamTypeInteger2D) { int* a = va_arg(ap, int*); int* b = va_arg(ap, int*); *a = paramInt(r, 0); *b = paramInt(r, 1); }
    else if (t == kOfxParamTypeInteger3D) { int* a = va_arg(ap, int*); int* b = va_arg(ap, int*); int* c = va_arg(ap, int*); *a = paramInt(r, 0); *b = paramInt(r, 1); *c = paramInt(r, 2); }
    else { int* o = va_arg(ap, int*); *o = paramInt(r, 0); } // integer, boolean, choice, pushbutton
    return kOfxStatOK;
}
OfxStatus prmGetValue(OfxParamHandle ph, ...) { va_list ap; va_start(ap, ph); OfxStatus s = prmGetV(ph, ap); va_end(ap); return s; }
OfxStatus prmGetValueAtTime(OfxParamHandle ph, OfxTime time, ...)
{
    HostRead lock;
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    va_list ap;
    va_start(ap, time);
    OfxStatus s = kOfxStatOK;
    if (r->type == kOfxParamTypeString && r->stringAtTime) {
        thread_local std::string value; // valid until the next call in this thread
        value = r->stringAtTime(time);
        const char** o = va_arg(ap, const char**);
        *o = value.c_str();
    } else if (r->numberAtTime && r->type == kOfxParamTypeDouble) {
        *va_arg(ap, double*) = r->numberAtTime(time);
    } else if (r->numberAtTime && (r->type == kOfxParamTypeInteger || r->type == kOfxParamTypeBoolean || r->type == kOfxParamTypeChoice)) {
        *va_arg(ap, int*) = static_cast<int>(r->numberAtTime(time));
    } else {
        s = prmGetV(ph, ap);
    }
    va_end(ap);
    return s;
}
OfxStatus prmGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus prmGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus prmSetV(OfxParamHandle ph, va_list ap)
{
    HostWrite lock;
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    const std::string& t = r->type;
    r->hasOv = true;
    ++r->nSet;
    if (t == kOfxParamTypeString) { const char* v = va_arg(ap, const char*); r->ov.s = { v ? v : "" }; }
    else if (t == kOfxParamTypeDouble) { r->ov.d = { va_arg(ap, double) }; }
    else if (t == kOfxParamTypeDouble2D) { double a = va_arg(ap, double); double b = va_arg(ap, double); r->ov.d = { a, b }; }
    else if (t == kOfxParamTypeDouble3D) { double a = va_arg(ap, double); double b = va_arg(ap, double); double c = va_arg(ap, double); r->ov.d = { a, b, c }; }
    else if (t == kOfxParamTypeRGBA) { double a = va_arg(ap, double); double b = va_arg(ap, double); double c = va_arg(ap, double); double d = va_arg(ap, double); r->ov.d = { a, b, c, d }; }
    else if (t == kOfxParamTypeRGB) { double a = va_arg(ap, double); double b = va_arg(ap, double); double c = va_arg(ap, double); r->ov.d = { a, b, c }; }
    else if (t == kOfxParamTypeInteger2D) { int a = va_arg(ap, int); int b = va_arg(ap, int); r->ov.i = { a, b }; }
    else if (t == kOfxParamTypeInteger3D) { int a = va_arg(ap, int); int b = va_arg(ap, int); int c = va_arg(ap, int); r->ov.i = { a, b, c }; }
    else { r->ov.i = { va_arg(ap, int) }; } // integer, boolean, choice
    return kOfxStatOK;
}
OfxStatus prmSetValue(OfxParamHandle ph, ...) { va_list ap; va_start(ap, ph); OfxStatus s = prmSetV(ph, ap); va_end(ap); return s; }
OfxStatus prmSetValueAtTime(OfxParamHandle ph, OfxTime time, ...) { va_list ap; va_start(ap, time); OfxStatus s = prmSetV(ph, ap); va_end(ap); return s; }
OfxStatus prmGetNumKeys(OfxParamHandle, unsigned int* n) { if (n) *n = 0; return kOfxStatOK; }
OfxStatus prmGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrBadIndex; }
OfxStatus prmGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
OfxStatus prmDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatOK; }
OfxStatus prmDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
OfxStatus prmCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatOK; }
OfxStatus prmEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus prmEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

OfxParameterSuiteV1 gParamSuite = {
    prmDefine, prmGetHandle, prmSetGetPropertySet, prmGetPropertySet, prmGetValue, prmGetValueAtTime,
    prmGetDerivative, prmGetIntegral, prmSetValue, prmSetValueAtTime, prmGetNumKeys, prmGetKeyTime,
    prmGetKeyIndex, prmDeleteKey, prmDeleteAllKeys, prmCopy, prmEditBegin, prmEditEnd
};

// ------------------------------------------------------------------ memory / multithread / message / interact
OfxStatus memAlloc(void*, size_t n, void** p) { *p = malloc(n); return *p ? kOfxStatOK : kOfxStatErrMemory; }
OfxStatus memFree(void* p) { free(p); return kOfxStatOK; }
OfxMemorySuiteV1 gMemSuite = { memAlloc, memFree };

OfxStatus mtRun(OfxThreadFunctionV1 f, unsigned int n, void* a) { unsigned int t = n ? n : 1; for (unsigned int k = 0; k < t; ++k) f(k, t, a); return kOfxStatOK; }
OfxStatus mtNum(unsigned int* n) { *n = 1; return kOfxStatOK; }
OfxStatus mtIdx(unsigned int* n) { *n = 0; return kOfxStatOK; }
int mtSpawned(void) { return 0; }
OfxStatus mxC(OfxMutexHandle* m, int) { *m = reinterpret_cast<OfxMutexHandle>(new std::mutex()); return kOfxStatOK; }
OfxStatus mxD(const OfxMutexHandle m) { delete reinterpret_cast<std::mutex*>(m); return kOfxStatOK; }
OfxStatus mxL(const OfxMutexHandle m) { reinterpret_cast<std::mutex*>(m)->lock(); return kOfxStatOK; }
OfxStatus mxU(const OfxMutexHandle m) { reinterpret_cast<std::mutex*>(m)->unlock(); return kOfxStatOK; }
OfxStatus mxT(const OfxMutexHandle m) { return reinterpret_cast<std::mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed; }
OfxMultiThreadSuiteV1 gMTSuite = { mtRun, mtNum, mtIdx, mtSpawned, mxC, mxD, mxL, mxU, mxT };

// The messages of the plug-ins (gMessages, gPersistentError) are written under this lock: concurrent renders send
// them from several threads (e.g. GenericReader's render clears the persistent message). The tests read and clear
// them while no render runs.
std::mutex gMessagesMutex;

// All the messages sent by the plug-ins, as "type: text\n" lines (cleared by the tests that check them).
std::string gMessages;
// The current persistent error message of the plug-in, if any (cleared by clearPersistentMessage).
std::string gPersistentError;
// The current persistent warning message of the plug-in, if any (cleared by clearPersistentMessage).
std::string gPersistentWarning;

// Prints the message on stderr, with its kind ("msg" or "persist") and type, and appends it to gMessages (called
// with gMessagesMutex locked)
void recordMessage(const char* kind, const char* type, const char* text)
{
    std::fprintf(stderr, "[ofx %s/%s] %s\n", kind, type ? type : "?", text);
    gMessages += std::string(type ? type : "?") + ": " + text + "\n";
}

OfxStatus msg(void*, const char* type, const char*, const char* fmt, ...)
{ char buf[4096]; va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  std::lock_guard<std::mutex> lock(gMessagesMutex);
  recordMessage("msg", type, buf); return kOfxStatOK; }
OfxMessageSuiteV1 gMsgSuite = { msg };

// A test's hook, called first by setPersistentMessage with its type, before any lock is taken (nullptr: none). Set and
// reset while no other thread calls the host. (e.g. it stops a render thread there, without synchronizing with the
// main thread, so that ThreadSanitizer sees a data race of the plug-in between them: #319)
inline void (*gPersistentMessageHook)(const char* type) = nullptr;
OfxStatus msgPersist(void*, const char* type, const char*, const char* fmt, ...)
{ if (gPersistentMessageHook) gPersistentMessageHook(type);
  char buf[4096]; va_list a; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
  std::lock_guard<std::mutex> lock(gMessagesMutex);
  recordMessage("persist", type, buf);
  gPersistentError = (type && !std::strcmp(type, kOfxMessageError)) ? buf : "";
  gPersistentWarning = (type && !std::strcmp(type, kOfxMessageWarning)) ? buf : "";
  return kOfxStatOK; }
OfxStatus msgClear(void*) { std::lock_guard<std::mutex> lock(gMessagesMutex); gPersistentError.clear(); gPersistentWarning.clear(); return kOfxStatOK; }
// gPersistentError, read under gMessagesMutex (e.g. while a render thread may set it)
inline std::string persistentError() { std::lock_guard<std::mutex> lock(gMessagesMutex); return gPersistentError; }
OfxMessageSuiteV2 gMsgSuiteV2 = { msg, msgPersist, msgClear };

OfxStatus itSwap(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus itRedraw(OfxInteractHandle) { return kOfxStatOK; }
// an interact handle is its PropSet
OfxStatus itGetPropSet(OfxInteractHandle h, OfxPropertySetHandle* ps) { if (!h || !ps) return kOfxStatErrBadHandle; *ps = reinterpret_cast<OfxPropertySetHandle>(h); return kOfxStatOK; }
OfxInteractSuiteV1 gInteractSuite = { itSwap, itRedraw, itGetPropSet };

// ------------------------------------------------------------------ FnOfx multi-plane suite v2
OfxStatus fnClipGetImagePlaneV2(OfxImageClipHandle c, OfxTime t, int, const char*, const OfxRectD*, OfxPropertySetHandle* out)
{ return ieClipGetImage(c, t, nullptr, out); }
OfxStatus fnClipGetRoDV2(OfxImageClipHandle c, OfxTime t, int, OfxRectD* b) { return ieClipGetRoD(c, t, b); }
// the views of the project: getViewCount returns gViewCount (1 or 2), and getViewName the name of a view in
// gViewNames, "main" for an index out of it. Tests that set other names (e.g. { "left", "right" }) restore them.
int gViewCount = 1;
std::vector<std::string> gViewNames = { "main", "right" };
// Sets gViewCount for its lifetime (e.g. a stereo project), then restores 1
struct ViewCountGuard {
    explicit ViewCountGuard(int n) { gViewCount = n; }
    ~ViewCountGuard() { gViewCount = 1; }
    ViewCountGuard(const ViewCountGuard&) = delete;
    ViewCountGuard& operator=(const ViewCountGuard&) = delete;
};
OfxStatus fnGetViewName(OfxImageEffectHandle, int v, const char** n) { if (n) *n = (v >= 0 && v < (int)gViewNames.size()) ? gViewNames[v].c_str() : "main"; return kOfxStatOK; }
OfxStatus fnGetViewCount(OfxImageEffectHandle, int* n) { if (n) *n = gViewCount; return kOfxStatOK; }
FnOfxImageEffectPlaneSuiteV2 gPlaneSuiteV2 = { fnClipGetImagePlaneV2, fnClipGetRoDV2, fnGetViewName, fnGetViewCount };

// OFX14_GL: an OpenGL render suite, for a host that has it but renders on the CPU: the render actions never have
// kOfxImageEffectPropOpenGLEnabled, so the textures are never loaded
OfxStatus glLoadTexture(OfxImageClipHandle, OfxTime, const char*, const OfxRectD*, OfxPropertySetHandle*) { return kOfxStatFailed; }
OfxStatus glFreeTexture(OfxPropertySetHandle) { return kOfxStatErrBadHandle; }
OfxStatus glFlushResources() { return kOfxStatReplyDefault; }
OfxImageEffectOpenGLRenderSuiteV1 gOpenGLSuite = { glLoadTexture, glFreeTexture, glFlushResources };

// OFX14_KEEPMSG2: the Message suite V2 even with OFX14_LEVEL < 12 (read by initHost())
bool gKeepMessageSuiteV2 = false;

// The names of the suites that the plug-ins fetched (ofx14_host_test checks them in the OFX 1.4-only mode)
std::set<std::string> gFetchedSuites;

// Only the versions that the host implements (e.g. no OfxMultiThreadSuiteV2, OFX 1.5): NULL for any other suite or
// version, as the spec allows ("returns a pointer to the suite, or NULL if the suite is not supported").
const void* fetchSuite(OfxPropertySetHandle, const char* name, int v)
{
    gFetchedSuites.insert(std::string(name) + " V" + std::to_string(v));
    if (gOfx14Only && (gNoSuites.count(name) || gNoSuites.count(std::string(name) + " V" + std::to_string(v)))) return nullptr;
    if (gOfx14Only && gOfxLevel < 12 && !gKeepMessageSuiteV2 && !std::strcmp(name, kOfxMessageSuite) && v == 2) return nullptr; // V2: OFX 1.2
    if (!std::strcmp(name, kOfxPropertySuite)) return v == 1 ? &gPropSuite : nullptr;
    if (!std::strcmp(name, kOfxImageEffectSuite)) return v == 1 ? &gEffectSuite : nullptr;
    if (!std::strcmp(name, kOfxParameterSuite)) return v == 1 ? &gParamSuite : nullptr;
    if (!std::strcmp(name, kOfxMemorySuite)) return v == 1 ? &gMemSuite : nullptr;
    if (!std::strcmp(name, kOfxMultiThreadSuite)) return v == 1 ? &gMTSuite : nullptr;
    if (!std::strcmp(name, kOfxMessageSuite)) return v == 2 ? (const void*)&gMsgSuiteV2 : (v == 1 ? (const void*)&gMsgSuite : nullptr);
    if (!std::strcmp(name, kOfxInteractSuite)) return v == 1 ? &gInteractSuite : nullptr;
    if (gOpenGLRender && !std::strcmp(name, kOfxOpenGLRenderSuite)) return v == 1 ? &gOpenGLSuite : nullptr;
    if (gOfx14Only) return nullptr; // no extension suite
    if (!std::strcmp(name, kFnOfxImageEffectPlaneSuite)) return v == 2 ? (const void*)&gPlaneSuiteV2 : nullptr;
    return nullptr;
}

inline std::vector<std::string> splitc(const char* e) { std::vector<std::string> r; if (!e) return r; std::string x(e); size_t p = 0; while (p <= x.size()) { size_t q = x.find(',', p); if (q == std::string::npos) q = x.size(); if (q > p) r.push_back(x.substr(p, q - p)); p = q + 1; } return r; }
void initHost()
{
    if (const char* e = std::getenv("OFX14_LEVEL")) gOfxLevel = std::atoi(e);
    for (auto& x : splitc(std::getenv("OFX14_NOSUITE"))) gNoSuites.insert(x);
    if (const char* e = std::getenv("OFX14_SCALE")) gRS = std::atof(e);
    gOpenGLRender = std::getenv("OFX14_GL") != nullptr;
    gKeepMessageSuiteV2 = std::getenv("OFX14_KEEPMSG2") != nullptr;
    {
        // from the headers of the upstream release tags (names-OFX_Release_*.txt diffs)
        const char* a11[] = { "OfxParamPropInteractSize" };
        const char* a12[] = { "kOfxParamPropUseHostOverlayHandle", "OfxImageComponentRGB", "OfxImageEffectPropInteractiveRenderStatus", "OfxImageEffectPropSequentialRenderStatus", "OfxInteractPropPenViewportPosition", "OfxInteractPropSuggestedColour", "OfxParamPropDefaultCoordinateSystem", "OfxParamPropGroupOpen", "OfxParamPropHasHostOverlayHandle", "OfxParamPropParametricDimension", "OfxParamPropParametricInteractBackground", "OfxParamPropParametricRange", "OfxParamPropParametricUIColour", "OfxParamHostPropSupportsParametricAnimation", "OfxPropAPIVersion", "OfxPropHostOSHandle", "OfxPropIcon", "OfxPropParamSetNeedsSyncing", "OfxPropPluginDescription", "OfxPropVersion", "OfxPropVersionLabel" };
        const char* a13[] = { "OfxParamStringIsRichTextFormat" };
        const char* a14[] = { "OfxImageEffectHostPropNativeOrigin", "OfxImageEffectPropRenderQualityDraft" };
        if (gOfxLevel < 11) for (auto x : a11) gExtraUnknown.insert(x);
        if (gOfxLevel < 12) for (auto x : a12) gExtraUnknown.insert(x);
        if (gOfxLevel < 13) for (auto x : a13) gExtraUnknown.insert(x);
        if (gOfxLevel < 14) for (auto x : a14) gExtraUnknown.insert(x);
    }
    OfxPropertySetHandle h = PH(&gHostProps);
    // OFX14_HOSTNAME: the host name, for the plug-ins that work around a host's quirks (e.g. DaVinciResolve)
    const char* hostName = std::getenv("OFX14_HOSTNAME");
    pSetString(h, kOfxPropName, 0, hostName ? hostName : "org.openfx-io.tests.renderhost");
    if (hostName && !std::strncmp(hostName, "DaVinciResolve", 14)) {
        // Resolve (openfx-misc's README-hosts.txt): "kOfxParamPropDefaultCoordinateSystem ... is not present on double
        // parameters (although API version 1.3 is claimed)"
        gExtraUnknown.insert(kOfxParamPropDefaultCoordinateSystem);
    }
    pSetString(h, kOfxPropLabel, 0, "openfx-io render test host");
    pSetInt(h, kOfxImageEffectHostPropIsBackground, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
    pSetInt(h, kOfxImageEffectPropSupportsTiles, 0, std::getenv("OFX14_NOTILES") ? 0 : 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultiResolution, 0, std::getenv("OFX14_NOMULTIRES") ? 0 : 1);
    pSetInt(h, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    pSetInt(h, kOfxImageEffectPropSetableFrameRate, 0, 0);
    pSetInt(h, kOfxImageEffectPropSetableFielding, 0, 0);
    // (the OFX 1.4-only host has overlays, so that the plug-ins register their overlay interacts)
    pSetInt(h, kOfxImageEffectPropSupportsOverlays, 0, (gOfx14Only && !std::getenv("OFX14_NOOVERLAYS")) ? 1 : 0);
    pSetInt(h, kOfxPropAPIVersion, 0, 1); pSetInt(h, kOfxPropAPIVersion, 1, 4);
    pSetInt(h, kOfxPropVersion, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsStringAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsCustomAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsChoiceAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsBooleanAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsCustomInteract, 0, 0);
    pSetInt(h, kOfxParamHostPropMaxParameters, 0, -1);
    pSetInt(h, kOfxParamHostPropMaxPages, 0, 30);
    pSetInt(h, kOfxParamHostPropPageRowColumnCount, 0, 10);
    pSetInt(h, kOfxParamHostPropPageRowColumnCount, 1, 10);
    const char* ctx[] = { kOfxImageEffectContextGenerator, kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral, kOfxImageEffectContextReader, kOfxImageEffectContextWriter };
    pSetStringN(h, kOfxImageEffectPropSupportedContexts, 5, ctx);
    std::vector<std::string> comps = { kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha };
    if (gOfxLevel < 12) comps = { kOfxImageComponentRGBA, kOfxImageComponentAlpha }; // RGB: OFX 1.2
    if (const char* e = std::getenv("OFX14_COMPS")) comps = splitc(e);
    for (size_t k = 0; k < comps.size(); ++k) pSetString(h, kOfxImageEffectPropSupportedComponents, (int)k, comps[k].c_str());
    std::vector<std::string> depths = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat };
    if (const char* e = std::getenv("OFX14_DEPTHS")) depths = splitc(e);
    for (size_t k = 0; k < depths.size(); ++k) pSetString(h, kOfxImageEffectPropSupportedPixelDepths, (int)k, depths[k].c_str());
    if (gOpenGLRender) pSetString(h, "OfxImageEffectPropOpenGLRenderSupported", 0, "true");
    gOfxHost.host = h;
    gOfxHost.fetchSuite = fetchSuite;
}

bool ok(OfxStatus s) { return s == kOfxStatOK || s == kOfxStatReplyDefault; }

// The pixels of an output image that the plug-in did not write are a NaN. It is written and tested on its bits in
// memory (not with std::nanf() and std::isnan(), and never passed as a float value), so that the tests also work when
// built with -ffast-math: the compiler then assumes that there are no NaNs, so std::isnan() is always false, and
// clang marks float arguments and return values as never NaN (nofpclass), which also folds a test on their bits.
// (bits: another value, for a test that needs a value other than NaN, compared with hasBits())
void fillUnwritten(float* begin, size_t count, uint32_t bits = 0x7fc00000u /* quiet NaN */)
{
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(begin + i, &bits, sizeof(bits));
    }
}

bool isUnwritten(const float* p)
{
    uint32_t bits;
    std::memcpy(&bits, p, sizeof(bits));
    return ((bits & 0x7f800000u) == 0x7f800000u) && ((bits & 0x007fffffu) != 0); // a NaN
}

// Are the bits of *p these bits (see fillUnwritten())?
inline bool hasBits(const float* p, uint32_t bits) { return !std::memcmp(p, &bits, sizeof(bits)); }

// ------------------------------------------------------------------ test helpers
OfxPlugin* findPlugin(const char* id)
{
    int n = OfxGetNumberOfPlugins();
    for (int i = 0; i < n; ++i) { OfxPlugin* p = OfxGetPlugin(i); if (p && p->pluginIdentifier && !std::strcmp(p->pluginIdentifier, id)) return p; }
    return nullptr;
}

// The host's value of the string param name (instead of its default), which getValue returns
void setOverrideString(Effect* ctx, const char* name, const std::string& val)
{ auto it = ctx->params.find(name); if (it != ctx->params.end()) { it->second.hasOv = true; it->second.ov.s = { val }; } }

// The host's value of the integer, boolean or choice param name (instead of its default)
void setOverrideInt(Effect* ctx, const char* name, int val)
{ auto it = ctx->params.find(name); if (it != ctx->params.end()) { it->second.hasOv = true; it->second.ov.i = { val }; } }

// The number of components of comps (RGBA, Alpha, else RGB), and the size of a component of depth (float, short,
// else byte)
inline int componentCount(const char* comps) { return !std::strcmp(comps, kOfxImageComponentRGBA) ? 4 : (!std::strcmp(comps, kOfxImageComponentAlpha) ? 1 : 3); }
inline int bytesPerComponent(const char* depth) { return !std::strcmp(depth, kOfxBitDepthFloat) ? 4 : (!std::strcmp(depth, kOfxBitDepthShort) ? 2 : 1); }

// The image of clip c (what clipGetImage returns): w x h pixels from (0, 0), with components comps and depth depth,
// in a buffer of zeros; the clip's props are set to the same format, its RoD to the image and its frame range to
// 1..1000. premultOpaque: Opaque, else PreMultiplied.
void setupClipImage(Clip* c, int w, int h, const char* comps, const char* depth, bool premultOpaque)
{
    int rowBytes = w * componentCount(comps) * bytesPerComponent(depth);
    c->buffer.assign((size_t)rowBytes * h, 0);
    OfxPropertySetHandle ip = PH(&c->image);
    pSetPointer(ip, kOfxImagePropData, 0, c->buffer.data());
    int bounds[4] = { 0, 0, w, h };
    pSetIntN(ip, kOfxImagePropBounds, 4, bounds);
    pSetIntN(ip, kOfxImagePropRegionOfDefinition, 4, bounds);
    pSetInt(ip, kOfxImagePropRowBytes, 0, rowBytes);
    pSetString(ip, kOfxImageEffectPropComponents, 0, comps);
    pSetString(ip, kOfxImageEffectPropPixelDepth, 0, depth);
    pSetString(ip, kOfxImageEffectPropPreMultiplication, 0, premultOpaque ? kOfxImageOpaque : kOfxImagePreMultiplied);
    pSetString(ip, kOfxImagePropField, 0, kOfxImageFieldNone);
    pSetString(ip, kOfxImagePropUniqueIdentifier, 0, "img");
    double rs[2] = { gRS, gRS }; pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
    pSetDouble(ip, kOfxImagePropPixelAspectRatio, 0, 1.);
    // Clip props mirror the image format.
    OfxPropertySetHandle cp = PH(&c->props);
    pSetString(cp, kOfxImageEffectPropComponents, 0, comps);
    pSetString(cp, kOfxImageEffectPropPixelDepth, 0, depth);
    pSetString(cp, kOfxImageEffectPropPreMultiplication, 0, premultOpaque ? kOfxImageOpaque : kOfxImagePreMultiplied);
    pSetInt(cp, kOfxImageClipPropConnected, 0, c->connected ? 1 : 0);
    pSetDouble(cp, kOfxImagePropPixelAspectRatio, 0, 1.);
    double rod[4] = { 0., 0., (double)w, (double)h }; pSetDoubleN(cp, kOfxImageEffectPropRegionOfDefinition, 4, rod);
    double fr[2] = { 1., 1000. }; pSetDoubleN(cp, kOfxImageEffectPropFrameRange, 2, fr);
    pSetString(cp, kOfxImageClipPropFieldOrder, 0, kOfxImageFieldNone);
    pSetInt(cp, kOfxImageClipPropContinuousSamples, 0, 0);
}

void setInstanceProps(Effect& inst, int w, int h, int nFrames, const char* context)
{
    OfxPropertySetHandle ep = PH(&inst.props);
    pSetString(ep, kOfxImageEffectPropContext, 0, context);
    double sz[2] = { (double)w, (double)h };
    pSetDoubleN(ep, kOfxImageEffectPropProjectSize, 2, sz);
    pSetDoubleN(ep, kOfxImageEffectPropProjectExtent, 2, sz);
    double off[2] = { 0., 0. };
    pSetDoubleN(ep, kOfxImageEffectPropProjectOffset, 2, off);
    gProjectSize[0] = sz[0]; gProjectSize[1] = sz[1];
    gProjectOffset[0] = off[0]; gProjectOffset[1] = off[1];
    pSetDouble(ep, kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.);
    pSetDouble(ep, kOfxImageEffectPropFrameRate, 0, 25.);
    pSetDouble(ep, kOfxImageEffectInstancePropEffectDuration, 0, (double)nFrames);
    double fr[2] = { 1., (double)nFrames };
    pSetDoubleN(ep, kOfxImageEffectPropFrameRange, 2, fr);
}

PropSet makeRenderInArgs(double t, int w, int h)
{
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetDouble(ap, kOfxPropTime, 0, t);
    double rs[2] = { gRS, gRS }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    int win[4] = { 0, 0, w, h }; pSetIntN(ap, kOfxImageEffectPropRenderWindow, 4, win);
    pSetString(ap, kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
    pSetInt(ap, kOfxImageEffectPropSequentialRenderStatus, 0, 1);
    pSetInt(ap, kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
    pSetInt(ap, kOfxImageEffectPropRenderQualityDraft, 0, 0);
    pSetString(ap, kOfxImageEffectPropRenderPlanes, 0, kFnOfxImagePlaneColour);
    pSetInt(ap, kFnOfxImageEffectPropView, 0, 0);
    return a;
}

int loadAndDescribe(OfxPlugin* p, Effect& describeEffect, Effect& ctxEffect, const char* context)
{
    p->setHost(&gOfxHost);
    if (!ok(p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr))) return -1;
    if (!ok(p->mainEntry(kOfxActionDescribe, reinterpret_cast<OfxImageEffectHandle>(&describeEffect), nullptr, nullptr))) return -1;
    PropSet inArgs;
    pSetString(PH(&inArgs), kOfxImageEffectPropContext, 0, context);
    if (!ok(p->mainEntry(kOfxImageEffectActionDescribeInContext, reinterpret_cast<OfxImageEffectHandle>(&ctxEffect), PH(&inArgs), nullptr))) return -1;
    return 0;
}

// kOfxActionInstanceChanged on the param (or, with type kOfxTypeClip, the clip) name, as the host calls it on its
// main thread: by default a user edit at time 1, at render scale 1
inline OfxStatus instanceChanged(OfxPlugin* p, Effect& inst, const char* name, const char* reason = kOfxChangeUserEdited,
                                 const char* type = kOfxTypeParameter, double t = 1.)
{
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetString(ap, kOfxPropType, 0, type);
    pSetString(ap, kOfxPropName, 0, name);
    pSetString(ap, kOfxPropChangeReason, 0, reason);
    pSetDouble(ap, kOfxPropTime, 0, t);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    return p->mainEntry(kOfxActionInstanceChanged, EH(inst), ap, nullptr);
}

// A Natron-like host: dynamic choices (e.g. the output-layer menu of ReadOIIO, the Parts and Views params of
// WriteOIIO) and multiple planes. Call it after initHost().
inline void setMultiPlanarHost()
{
    pSetInt(PH(&gHostProps), kNatronOfxParamHostPropSupportsDynamicChoices, 0, 1);
    pSetInt(PH(&gHostProps), kFnOfxImageEffectPropMultiPlanar, 0, 1);
}

// The size of the file, or -1 if there is none
inline long fileSize(const std::string& path)
{
    struct stat st;
    return (stat(path.c_str(), &st) == 0) ? (long)st.st_size : -1L;
}

// The contents of the file ("" if there is none)
inline std::string fileContents(const std::string& path)
{
    std::ifstream ifs(path.c_str(), std::ios::binary);
    std::ostringstream os;
    os << ifs.rdbuf();
    return os.str();
}

// ------------------------------------------------------------------ checks
int gFailures = 0; // the number of failed checks: the test program fails if it is not 0
// Optional hooks (ocio_anim_fuzz): gQuiet: don't print the checks that pass; gOnFail: if set, called with the message
// of a failed check, which is not printed
bool gQuiet = false;
std::function<void(const char*)> gOnFail;

// the printf format checks of the compiler (-Wformat) for a function whose argument fmtIndex is a printf format, and
// whose variable arguments start at argIndex (MinGW: the format of its C runtime)
#if defined(__MINGW_PRINTF_FORMAT)
#define IO_TEST_PRINTF_FORMAT(fmtIndex, argIndex) __attribute__((format(__MINGW_PRINTF_FORMAT, fmtIndex, argIndex)))
#elif defined(__GNUC__)
#define IO_TEST_PRINTF_FORMAT(fmtIndex, argIndex) __attribute__((format(printf, fmtIndex, argIndex)))
#else
#define IO_TEST_PRINTF_FORMAT(fmtIndex, argIndex)
#endif

// Prints "  PASS: message" or "  FAIL: message" (fmt: a printf format; a message that is not a literal goes through the
// std::string overload), and counts a failure in gFailures.
IO_TEST_PRINTF_FORMAT(2, 3) inline void check(bool cond, const char* fmt, ...)
{
    if (cond && gQuiet) {
        return;
    }
    va_list a, b;
    va_start(a, fmt);
    va_copy(b, a);
    std::vector<char> buf((size_t)(std::max)(0, std::vsnprintf(nullptr, 0, fmt, a)) + 1);
    va_end(a);
    std::vsnprintf(buf.data(), buf.size(), fmt, b);
    va_end(b);
    if (!cond) {
        ++gFailures;
        if (gOnFail) {
            gOnFail(buf.data());
            return;
        }
    }
    std::printf("  %s: %s\n", cond ? "PASS" : "FAIL", buf.data());
}
inline void check(bool cond, const std::string& what) { check(cond, "%s", what.c_str()); }

// The mean of value(i, j), for the pixels (i, j) at scale 1 in bounds that pixel (x, y) at the render scale covers
// ([x / scale.x, (x + 1) / scale.x) x [y / scale.y, (y + 1) / scale.y), "Mapping Between The Spatial Coordinate
// Systems", ofxCoordSystem.rst), weighted by the covered area; 0 if it covers none of bounds. The reference of the
// tests of render scales that are not a power of two (#194: reader_window_test, exr_read_test).
template <typename Value>
double
areaMean(const OfxRectI& bounds,
         const OfxPointD& scale,
         int x,
         int y,
         Value value)
{
    const double x1 = (std::max)(x / scale.x, (double)bounds.x1);
    const double x2 = (std::min)((x + 1.) / scale.x, (double)bounds.x2);
    const double y1 = (std::max)(y / scale.y, (double)bounds.y1);
    const double y2 = (std::min)((y + 1.) / scale.y, (double)bounds.y2);
    double sum = 0.;
    double weight = 0.;
    for (long long j = (long long)std::floor(y1); j < y2; ++j) {
        const double wy = (std::min)(j + 1., y2) - (std::max)((double)j, y1);
        for (long long i = (long long)std::floor(x1); i < x2; ++i) {
            const double w = wy * ((std::min)(i + 1., x2) - (std::max)((double)i, x1));
            if (w > 0.) {
                sum += w * value((int)i, (int)j);
                weight += w;
            }
        }
    }

    return (weight > 0.) ? sum / weight : 0.;
}

} // namespace

#endif // IO_TESTS_OFX_TEST_HOST_H
