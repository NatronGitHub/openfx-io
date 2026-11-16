/*
 * ofx_test_host.h — a minimal in-process OFX host for the direct-link plugin tests
 * (ofx_render_test.cpp, reader_hooks_test.cpp).
 *
 * Implements the Property, ImageEffect, Parameter, Memory, MultiThread, Message and
 * Interact suites, plus FnOfxImageEffectPlaneSuiteV2, over a generic property bag:
 * param values (definition defaults or host overrides), image clips backed by a pixel
 * buffer, and a few helpers to load/describe a plugin and build action arguments.
 * Param values are not animated: getValueAtTime() ignores the time.
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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

inline PropSet* PS(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
inline Effect* EF(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
inline OfxPropertySetHandle PH(PropSet* ps) { return reinterpret_cast<OfxPropertySetHandle>(ps); }

// ------------------------------------------------------------------ property suite
OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int idx, void* v)
{ auto& a = PS(h)->props[n]; if ((int)a.p.size() <= idx) a.p.resize(idx + 1); a.p[idx] = v; return kOfxStatOK; }
OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int idx, const char* v)
{ auto& a = PS(h)->props[n]; if ((int)a.s.size() <= idx) a.s.resize(idx + 1); a.s[idx] = v ? v : ""; return kOfxStatOK; }
OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int idx, double v)
{ auto& a = PS(h)->props[n]; if ((int)a.d.size() <= idx) a.d.resize(idx + 1); a.d[idx] = v; return kOfxStatOK; }
OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int idx, int v)
{ auto& a = PS(h)->props[n]; if ((int)a.i.size() <= idx) a.i.resize(idx + 1); a.i[idx] = v; return kOfxStatOK; }
OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) { for (int k = 0; k < c; ++k) pSetPointer(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) { for (int k = 0; k < c; ++k) pSetString(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) { for (int k = 0; k < c; ++k) pSetDouble(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) { for (int k = 0; k < c; ++k) pSetInt(h, n, k, v[k]); return kOfxStatOK; }

OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int idx, void** v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.p.size() <= idx) return kOfxStatErrUnknown; *v = it->second.p[idx]; return kOfxStatOK; }
OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int idx, const char** v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.s.size() <= idx) return kOfxStatErrUnknown; *v = it->second.s[idx].c_str(); return kOfxStatOK; }
OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int idx, double* v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.d.size() <= idx) return kOfxStatErrUnknown; *v = it->second.d[idx]; return kOfxStatOK; }
OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int idx, int* v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.i.size() <= idx) return kOfxStatErrUnknown; *v = it->second.i[idx]; return kOfxStatOK; }
OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) { for (int k = 0; k < c; ++k) if (pGetPointer(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, const char** v) { for (int k = 0; k < c; ++k) if (pGetString(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) { for (int k = 0; k < c; ++k) if (pGetDouble(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { for (int k = 0; k < c; ++k) if (pGetInt(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pReset(OfxPropertySetHandle h, const char* n) { PS(h)->props.erase(n); return kOfxStatOK; }
OfxStatus pGetDimension(OfxPropertySetHandle h, const char* n, int* c)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end()) { *c = 0; return kOfxStatOK; }
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
OfxStatus prmGetValueAtTime(OfxParamHandle ph, OfxTime, ...) { va_list ap; va_start(ap, ph); OfxStatus s = prmGetV(ph, ap); va_end(ap); return s; }
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
OfxStatus prmSetValueAtTime(OfxParamHandle ph, OfxTime, ...) { va_list ap; va_start(ap, ph); OfxStatus s = prmSetV(ph, ap); va_end(ap); return s; }
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

OfxStatus msg(void*, const char* type, const char*, const char* fmt, ...)
{ va_list a; va_start(a, fmt); std::fprintf(stderr, "[ofx msg/%s] ", type ? type : "?"); std::vfprintf(stderr, fmt, a); std::fprintf(stderr, "\n"); va_end(a); return kOfxStatOK; }
OfxMessageSuiteV1 gMsgSuite = { msg };

OfxStatus msgPersist(void*, const char* type, const char*, const char* fmt, ...)
{ va_list a; va_start(a, fmt); std::fprintf(stderr, "[ofx persist/%s] ", type ? type : "?"); std::vfprintf(stderr, fmt, a); std::fprintf(stderr, "\n"); va_end(a); return kOfxStatOK; }
OfxStatus msgClear(void*) { return kOfxStatOK; }
OfxMessageSuiteV2 gMsgSuiteV2 = { msg, msgPersist, msgClear };

OfxStatus itSwap(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus itRedraw(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus itGetPropSet(OfxInteractHandle, OfxPropertySetHandle*) { return kOfxStatErrUnsupported; }
OfxInteractSuiteV1 gInteractSuite = { itSwap, itRedraw, itGetPropSet };

// ------------------------------------------------------------------ FnOfx multi-plane suite v2
OfxStatus fnClipGetImagePlaneV2(OfxImageClipHandle c, OfxTime, int, const char*, const OfxRectD*, OfxPropertySetHandle* out)
{ if (reinterpret_cast<Clip*>(c)->noImage) return kOfxStatFailed; if (out) *out = PH(&reinterpret_cast<Clip*>(c)->image); return kOfxStatOK; }
OfxStatus fnClipGetRoDV2(OfxImageClipHandle c, OfxTime t, int, OfxRectD* b) { return ieClipGetRoD(c, t, b); }
OfxStatus fnGetViewName(OfxImageEffectHandle, int, const char** n) { if (n) *n = "main"; return kOfxStatOK; }
OfxStatus fnGetViewCount(OfxImageEffectHandle, int* n) { if (n) *n = 1; return kOfxStatOK; }
FnOfxImageEffectPlaneSuiteV2 gPlaneSuiteV2 = { fnClipGetImagePlaneV2, fnClipGetRoDV2, fnGetViewName, fnGetViewCount };

const void* fetchSuite(OfxPropertySetHandle, const char* name, int v)
{
    if (!std::strcmp(name, kOfxPropertySuite)) return &gPropSuite;
    if (!std::strcmp(name, kOfxImageEffectSuite)) return &gEffectSuite;
    if (!std::strcmp(name, kOfxParameterSuite)) return &gParamSuite;
    if (!std::strcmp(name, kOfxMemorySuite)) return &gMemSuite;
    if (!std::strcmp(name, kOfxMultiThreadSuite)) return &gMTSuite;
    if (!std::strcmp(name, kOfxMessageSuite)) return v >= 2 ? (const void*)&gMsgSuiteV2 : (const void*)&gMsgSuite;
    if (!std::strcmp(name, kOfxInteractSuite)) return &gInteractSuite;
    if (!std::strcmp(name, kFnOfxImageEffectPlaneSuite)) return v >= 2 ? (const void*)&gPlaneSuiteV2 : nullptr;
    return nullptr;
}

void initHost()
{
    OfxPropertySetHandle h = PH(&gHostProps);
    pSetString(h, kOfxPropName, 0, "org.openfx-io.tests.renderhost");
    pSetString(h, kOfxPropLabel, 0, "openfx-io render test host");
    pSetInt(h, kOfxImageEffectHostPropIsBackground, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
    pSetInt(h, kOfxImageEffectPropSupportsTiles, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    pSetInt(h, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    pSetInt(h, kOfxImageEffectPropSetableFrameRate, 0, 0);
    pSetInt(h, kOfxImageEffectPropSetableFielding, 0, 0);
    pSetInt(h, kOfxImageEffectPropSupportsOverlays, 0, 0);
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
    const char* comps[] = { kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha };
    pSetStringN(h, kOfxImageEffectPropSupportedComponents, 3, comps);
    const char* depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat };
    pSetStringN(h, kOfxImageEffectPropSupportedPixelDepths, 3, depths);
    gOfxHost.host = h;
    gOfxHost.fetchSuite = fetchSuite;
}

bool ok(OfxStatus s) { return s == kOfxStatOK || s == kOfxStatReplyDefault; }

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
    double rs[2] = { 1., 1. }; pSetDoubleN(ip, kOfxImageEffectPropRenderScale, 2, rs);
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
    double rs[2] = { 1., 1. }; pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
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
