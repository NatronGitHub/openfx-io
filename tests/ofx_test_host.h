/*
 * ofx_test_host.h — a minimal in-process OFX host for the direct-link plugin tests
 * (ofx_render_test.cpp, reader_hooks_test.cpp).
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
 * Message V1 and V2, Interact V1), and every property suite function fails with
 * kOfxStatErrUnknown on a property that is not defined in the OFX 1.4 headers
 * (ofx14_names.h), on any property set (host, effect, clip, param, image, action
 * arguments): reading it fails, and setting it stores nothing. The host has no Natron,
 * Nuke, Vegas or other extension property, and no OpenGL render suite.
 * Param values are not animated: getValueAtTime() ignores the time, except for a string
 * param whose stringAtTime is set (e.g. a file param naming each frame of an image sequence).
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
#include "nuke/fnOfxExtensions.h" // kOfxImageEffectPropRenderPlanes, kFnOfxImagePlaneColour, kFnOfxImageEffectPropView
#include "ofxNatron.h" // kNatronOfxParamProcessR/G/B/A

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
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
    std::map<std::string, Value> props;
};

struct ParamRec {
    std::string type;
    std::string name;
    PropSet* props = nullptr; // definition props (holds kOfxParamPropDefault)
    Value ov;                 // host override value (optional)
    bool hasOv = false;
    std::function<std::string(double)> stringAtTime; // string param: the value returned by getValueAtTime() (optional)
};

struct Clip {
    std::string name;
    PropSet props;            // clip props (components, depth, connected, ...)
    PropSet image;            // image props returned by clipGetImage
    std::vector<uint8_t> buffer; // pixel buffer backing the image
    bool connected = false;
    bool noImage = false;     // clipGetImage fails with kOfxStatFailed ("assume black and transparent")
};

struct Effect {
    PropSet props;                          // effect props (context, instance data, ...)
    std::map<std::string, ParamRec> params; // by name
    std::map<std::string, Clip*> clips;     // by name
    std::vector<PropSet*> owned;
    Effect* delegate = nullptr;             // instance -> context descriptor for params/clips
    ~Effect() { for (PropSet* ps : owned) delete ps; for (auto& kv : clips) delete kv.second; }
    PropSet* newChild() { PropSet* ps = new PropSet(); owned.push_back(ps); return ps; }
    Effect* src() { return delegate ? delegate : this; }
};

PropSet gHostProps;
OfxHost gOfxHost;

// OFX 1.4-only host mode (see the top of this file). Set it before initHost().
bool gOfx14Only = false;

// Is the property name defined by OFX 1.4?
// [review] older-host emulation and knobs (env, read by initHost())
int gOfxLevel = 14;                  // OFX14_LEVEL: 10, 11, 12, 13 or 14
std::set<std::string> gNoSuites;     // OFX14_NOSUITE: "name Vn" or "name", comma-separated
std::set<std::string> gExtraUnknown; // names added after gOfxLevel
double gRS = 1.;                     // OFX14_SCALE
bool isOfx14Name(const char* n)
{
    if (gExtraUnknown.count(n)) return false;
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
bool gDefaultUnsetProps = false;
OfxStatus unknownProp(const char* n) { return (gOfx14Only && !isOfx14Name(n)) ? kOfxStatErrUnknown : kOfxStatOK; }
#define CHECK_PROP_KNOWN(n) do { if (unknownProp(n) != kOfxStatOK) return kOfxStatErrUnknown; } while (0)

inline PropSet* PS(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
inline Effect* EF(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
inline OfxPropertySetHandle PH(PropSet* ps) { return reinterpret_cast<OfxPropertySetHandle>(ps); }

// ------------------------------------------------------------------ property suite
OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int idx, void* v)
{ CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.p.size() <= idx) a.p.resize(idx + 1); a.p[idx] = v; return kOfxStatOK; }
OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int idx, const char* v)
{ CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.s.size() <= idx) a.s.resize(idx + 1); a.s[idx] = v ? v : ""; return kOfxStatOK; }
OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int idx, double v)
{ CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.d.size() <= idx) a.d.resize(idx + 1); a.d[idx] = v; return kOfxStatOK; }
OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int idx, int v)
{ CHECK_PROP_KNOWN(n); auto& a = PS(h)->props[n]; if ((int)a.i.size() <= idx) a.i.resize(idx + 1); a.i[idx] = v; return kOfxStatOK; }
// (setting N values sets the dimension of the property to N, e.g. 0 to empty a choice param's options)
OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { CHECK_PROP_KNOWN(n); PS(h)->props[n].p.resize(c); for (int k = 0; k < c; ++k) pSetPointer(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { CHECK_PROP_KNOWN(n); PS(h)->props[n].s.resize(c); for (int k = 0; k < c; ++k) pSetString(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) { CHECK_PROP_KNOWN(n); PS(h)->props[n].d.resize(c); for (int k = 0; k < c; ++k) pSetDouble(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { CHECK_PROP_KNOWN(n); PS(h)->props[n].i.resize(c); for (int k = 0; k < c; ++k) pSetInt(h, n, k, v[k]); return kOfxStatOK; }

OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int idx, void** v)
{ CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (gDefaultUnsetProps && it == PS(h)->props.end() && idx == 0) { *v = nullptr; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.p.size() <= idx) return kOfxStatErrUnknown; *v = it->second.p[idx]; return kOfxStatOK; }
OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int idx, const char** v)
{ CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (gDefaultUnsetProps && it == PS(h)->props.end() && idx == 0) { *v = ""; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.s.size() <= idx) return kOfxStatErrUnknown; *v = it->second.s[idx].c_str(); return kOfxStatOK; }
OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int idx, double* v)
{ CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (gDefaultUnsetProps && it == PS(h)->props.end() && idx == 0) { *v = 0.; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.d.size() <= idx) return kOfxStatErrUnknown; *v = it->second.d[idx]; return kOfxStatOK; }
OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int idx, int* v)
{ CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (gDefaultUnsetProps && it == PS(h)->props.end() && idx == 0) { *v = 0; return kOfxStatOK; } if (it == PS(h)->props.end() || (int)it->second.i.size() <= idx) return kOfxStatErrUnknown; *v = it->second.i[idx]; return kOfxStatOK; }
OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) { for (int k = 0; k < c; ++k) if (pGetPointer(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, const char** v) { for (int k = 0; k < c; ++k) if (pGetString(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) { for (int k = 0; k < c; ++k) if (pGetDouble(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { for (int k = 0; k < c; ++k) if (pGetInt(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pReset(OfxPropertySetHandle h, const char* n) { CHECK_PROP_KNOWN(n); PS(h)->props.erase(n); return kOfxStatOK; }
OfxStatus pGetDimension(OfxPropertySetHandle h, const char* n, int* c)
{ CHECK_PROP_KNOWN(n); auto it = PS(h)->props.find(n); if (it == PS(h)->props.end()) { *c = 0; return kOfxStatOK; }
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
OfxStatus ieClipGetImage(OfxImageClipHandle c, OfxTime, const OfxRectD*, OfxPropertySetHandle* out) { if (reinterpret_cast<Clip*>(c)->noImage) return kOfxStatFailed; if (out) *out = PH(&reinterpret_cast<Clip*>(c)->image); return kOfxStatOK; }
OfxStatus ieClipReleaseImage(OfxPropertySetHandle) { return kOfxStatOK; }
OfxStatus ieClipGetRoD(OfxImageClipHandle c, OfxTime, OfxRectD* rod)
{ Clip* clip = reinterpret_cast<Clip*>(c); double v; if (pGetDouble(PH(&clip->props), kOfxImageEffectPropRegionOfDefinition, 0, &v) == kOfxStatOK) { pGetDouble(PH(&clip->props), kOfxImageEffectPropRegionOfDefinition, 0, &rod->x1); pGetDouble(PH(&clip->props), kOfxImageEffectPropRegionOfDefinition, 1, &rod->y1); pGetDouble(PH(&clip->props), kOfxImageEffectPropRegionOfDefinition, 2, &rod->x2); pGetDouble(PH(&clip->props), kOfxImageEffectPropRegionOfDefinition, 3, &rod->y2); return kOfxStatOK; } return kOfxStatFailed; }
int gAbort = 0; // what ieAbort() returns (a render the host is aborting)
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
  double v = 0; pGetDouble(PH(r->props), kOfxParamPropDefault, idx, &v); return v; }
int paramInt(ParamRec* r, int idx)
{ if (r->hasOv && (int)r->ov.i.size() > idx) return r->ov.i[idx];
  int v = 0; pGetInt(PH(r->props), kOfxParamPropDefault, idx, &v); return v; }
const char* paramString(ParamRec* r)
{ if (r->hasOv && !r->ov.s.empty()) return r->ov.s[0].c_str();
  const char* v = ""; pGetString(PH(r->props), kOfxParamPropDefault, 0, &v); return v; }

OfxStatus prmGetV(OfxParamHandle ph, va_list ap)
{
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    const std::string& t = r->type;
    if (t == kOfxParamTypeString) { const char** o = va_arg(ap, const char**); *o = paramString(r); }
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
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    va_list ap;
    va_start(ap, time);
    OfxStatus s = kOfxStatOK;
    if (r->type == kOfxParamTypeString && r->stringAtTime) {
        thread_local std::string value; // valid until the next call in this thread
        value = r->stringAtTime(time);
        const char** o = va_arg(ap, const char**);
        *o = value.c_str();
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
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    const std::string& t = r->type;
    r->hasOv = true;
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
// (called with gMessagesMutex locked)
std::string gMessages;
void recordMessage(const char* type, const char* fmt, va_list a)
{
    char buf[4096];
    std::vsnprintf(buf, sizeof(buf), fmt, a);
    gMessages += std::string(type ? type : "?") + ": " + buf + "\n";
}

OfxStatus msg(void*, const char* type, const char*, const char* fmt, ...)
{ va_list a; va_start(a, fmt); std::fprintf(stderr, "[ofx msg/%s] ", type ? type : "?"); std::vfprintf(stderr, fmt, a); std::fprintf(stderr, "\n"); va_end(a);
  std::lock_guard<std::mutex> lock(gMessagesMutex);
  va_start(a, fmt); recordMessage(type, fmt, a); va_end(a); return kOfxStatOK; }
OfxMessageSuiteV1 gMsgSuite = { msg };

// The current persistent error message of the plug-in, if any (cleared by clearPersistentMessage).
std::string gPersistentError;
OfxStatus msgPersist(void*, const char* type, const char*, const char* fmt, ...)
{ va_list a; va_start(a, fmt); std::fprintf(stderr, "[ofx persist/%s] ", type ? type : "?"); std::vfprintf(stderr, fmt, a); std::fprintf(stderr, "\n"); va_end(a);
  std::lock_guard<std::mutex> lock(gMessagesMutex);
  va_start(a, fmt); recordMessage(type, fmt, a); va_end(a);
  if (type && !std::strcmp(type, kOfxMessageError)) { char buf[4096]; va_start(a, fmt); std::vsnprintf(buf, sizeof(buf), fmt, a); va_end(a); gPersistentError = buf; } else { gPersistentError.clear(); }
  return kOfxStatOK; }
OfxStatus msgClear(void*) { std::lock_guard<std::mutex> lock(gMessagesMutex); gPersistentError.clear(); return kOfxStatOK; }
OfxMessageSuiteV2 gMsgSuiteV2 = { msg, msgPersist, msgClear };

OfxStatus itSwap(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus itRedraw(OfxInteractHandle) { return kOfxStatOK; }
// an interact handle is its PropSet
OfxStatus itGetPropSet(OfxInteractHandle h, OfxPropertySetHandle* ps) { if (!h || !ps) return kOfxStatErrBadHandle; *ps = reinterpret_cast<OfxPropertySetHandle>(h); return kOfxStatOK; }
OfxInteractSuiteV1 gInteractSuite = { itSwap, itRedraw, itGetPropSet };

// ------------------------------------------------------------------ FnOfx multi-plane suite v2
OfxStatus fnClipGetImagePlaneV2(OfxImageClipHandle c, OfxTime, int, const char*, const OfxRectD*, OfxPropertySetHandle* out)
{ if (reinterpret_cast<Clip*>(c)->noImage) return kOfxStatFailed; if (out) *out = PH(&reinterpret_cast<Clip*>(c)->image); return kOfxStatOK; }
OfxStatus fnClipGetRoDV2(OfxImageClipHandle c, OfxTime t, int, OfxRectD* b) { return ieClipGetRoD(c, t, b); }
OfxStatus fnGetViewName(OfxImageEffectHandle, int, const char** n) { if (n) *n = "main"; return kOfxStatOK; }
OfxStatus fnGetViewCount(OfxImageEffectHandle, int* n) { if (n) *n = 1; return kOfxStatOK; }
FnOfxImageEffectPlaneSuiteV2 gPlaneSuiteV2 = { fnClipGetImagePlaneV2, fnClipGetRoDV2, fnGetViewName, fnGetViewCount };

// The names of the suites that the plug-ins fetched (ofx14_host_test checks them in the OFX 1.4-only mode)
std::set<std::string> gFetchedSuites;

// Only the versions that the host implements (e.g. no OfxMultiThreadSuiteV2, OFX 1.5): NULL for any other suite or
// version, as the spec allows ("returns a pointer to the suite, or NULL if the suite is not supported").
const void* fetchSuite(OfxPropertySetHandle, const char* name, int v)
{
    gFetchedSuites.insert(std::string(name) + " V" + std::to_string(v));
    if (gOfx14Only && (gNoSuites.count(name) || gNoSuites.count(std::string(name) + " V" + std::to_string(v)))) return nullptr;
    if (gOfx14Only && gOfxLevel < 12 && !std::getenv("OFX14_KEEPMSG2") && !std::strcmp(name, kOfxMessageSuite) && v == 2) return nullptr; // V2: OFX 1.2
    if (!std::strcmp(name, kOfxPropertySuite)) return v == 1 ? &gPropSuite : nullptr;
    if (!std::strcmp(name, kOfxImageEffectSuite)) return v == 1 ? &gEffectSuite : nullptr;
    if (!std::strcmp(name, kOfxParameterSuite)) return v == 1 ? &gParamSuite : nullptr;
    if (!std::strcmp(name, kOfxMemorySuite)) return v == 1 ? &gMemSuite : nullptr;
    if (!std::strcmp(name, kOfxMultiThreadSuite)) return v == 1 ? &gMTSuite : nullptr;
    if (!std::strcmp(name, kOfxMessageSuite)) return v == 2 ? (const void*)&gMsgSuiteV2 : (v == 1 ? (const void*)&gMsgSuite : nullptr);
    if (!std::strcmp(name, kOfxInteractSuite)) return v == 1 ? &gInteractSuite : nullptr;
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
    pSetString(h, kOfxPropName, 0, "org.openfx-io.tests.renderhost");
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
    gOfxHost.host = h;
    gOfxHost.fetchSuite = fetchSuite;
}

bool ok(OfxStatus s) { return s == kOfxStatOK || s == kOfxStatReplyDefault; }

// The pixels of an output image that the plug-in did not write are a NaN. It is written and tested on its bits in
// memory (not with std::nanf() and std::isnan(), and never passed as a float value), so that the tests also work when
// built with -ffast-math: the compiler then assumes that there are no NaNs, so std::isnan() is always false, and
// clang marks float arguments and return values as never NaN (nofpclass), which also folds a test on their bits.
void fillUnwritten(float* begin, size_t count)
{
    const uint32_t bits = 0x7fc00000u; // quiet NaN
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

// ------------------------------------------------------------------ test helpers
OfxPlugin* findPlugin(const char* id)
{
    int n = OfxGetNumberOfPlugins();
    for (int i = 0; i < n; ++i) { OfxPlugin* p = OfxGetPlugin(i); if (p && p->pluginIdentifier && !std::strcmp(p->pluginIdentifier, id)) return p; }
    return nullptr;
}

// Set default value (int) on a param definition prop set so instance getValue works.
void setOverrideString(Effect* ctx, const char* name, const std::string& val)
{ auto it = ctx->params.find(name); if (it != ctx->params.end()) { it->second.hasOv = true; it->second.ov.s = { val }; } }

void setOverrideInt(Effect* ctx, const char* name, int val)
{ auto it = ctx->params.find(name); if (it != ctx->params.end()) { it->second.hasOv = true; it->second.ov.i = { val }; } }

// Build an image property set for a clip: dims WxH, RGBA float, buffer allocated & pattern-filled optional.
void setupClipImage(Clip* c, int w, int h, const char* comps, const char* depth, bool premultOpaque)
{
    int nComps = (!std::strcmp(comps, kOfxImageComponentRGBA)) ? 4 : (!std::strcmp(comps, kOfxImageComponentAlpha) ? 1 : 3);
    int bpc = (!std::strcmp(depth, kOfxBitDepthFloat)) ? 4 : (!std::strcmp(depth, kOfxBitDepthShort) ? 2 : 1);
    int rowBytes = w * nComps * bpc;
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

} // namespace

#endif // IO_TESTS_OFX_TEST_HOST_H
