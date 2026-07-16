/*
 * ofx_actions_test — integration test that DIRECTLY LINKS the openfx-io plugin
 * translation units with the OFX C++ Support library and drives the plugin
 * action entry points through a minimal, in-process OFX host (no bundle / no
 * dlopen / no external host).
 *
 * It implements just enough of the seven mandatory OFX suites (Property,
 * ImageEffect, Parameter, Memory, MultiThread, Message, Interact) — backed by a
 * generic property bag — for the Support library to run:
 *   OfxSetHost -> mainEntry(kOfxActionLoad)
 *              -> mainEntry(kOfxActionDescribe)          (per plugin)
 *              -> mainEntry(kOfxImageEffectActionDescribeInContext)
 *              -> mainEntry(kOfxActionUnload)
 * and asserts each returns kOfxStatOK (or kOfxStatReplyDefault), and that
 * Describe declared the expected contexts.
 *
 * Exit status: 0 = all checks passed.
 */
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

// An "effect"/param-set handle owns its own property set plus the property sets
// of any clips/params defined on it, so the Support library's descriptor can
// populate them during Describe.
struct Effect {
    PropSet props;
    std::vector<PropSet*> owned;
    ~Effect()
    {
        for (PropSet* ps : owned) {
            delete ps;
        }
    }
    PropSet* newChild()
    {
        PropSet* ps = new PropSet();
        owned.push_back(ps);
        return ps;
    }
};

PropSet gHostProps;
OfxHost gOfxHost;

inline PropSet* PS(OfxPropertySetHandle h) { return reinterpret_cast<PropSet*>(h); }
inline Effect* EF(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
inline OfxPropertySetHandle H(PropSet* ps) { return reinterpret_cast<OfxPropertySetHandle>(ps); }

// ------------------------------------------------------------------ property suite
OfxStatus pSetPointer(OfxPropertySetHandle h, const char* n, int idx, void* v)
{ auto& val = PS(h)->props[n]; if ((int)val.p.size() <= idx) val.p.resize(idx + 1); val.p[idx] = v; return kOfxStatOK; }
OfxStatus pSetString(OfxPropertySetHandle h, const char* n, int idx, const char* v)
{ auto& val = PS(h)->props[n]; if ((int)val.s.size() <= idx) val.s.resize(idx + 1); val.s[idx] = v ? v : ""; return kOfxStatOK; }
OfxStatus pSetDouble(OfxPropertySetHandle h, const char* n, int idx, double v)
{ auto& val = PS(h)->props[n]; if ((int)val.d.size() <= idx) val.d.resize(idx + 1); val.d[idx] = v; return kOfxStatOK; }
OfxStatus pSetInt(OfxPropertySetHandle h, const char* n, int idx, int v)
{ auto& val = PS(h)->props[n]; if ((int)val.i.size() <= idx) val.i.resize(idx + 1); val.i[idx] = v; return kOfxStatOK; }
OfxStatus pSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v)
{ for (int k = 0; k < c; ++k) pSetPointer(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v)
{ for (int k = 0; k < c; ++k) pSetString(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v)
{ for (int k = 0; k < c; ++k) pSetDouble(h, n, k, v[k]); return kOfxStatOK; }
OfxStatus pSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v)
{ for (int k = 0; k < c; ++k) pSetInt(h, n, k, v[k]); return kOfxStatOK; }

OfxStatus pGetPointer(OfxPropertySetHandle h, const char* n, int idx, void** v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.p.size() <= idx) return kOfxStatErrUnknown; *v = it->second.p[idx]; return kOfxStatOK; }
OfxStatus pGetString(OfxPropertySetHandle h, const char* n, int idx, const char** v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.s.size() <= idx) return kOfxStatErrUnknown; *v = it->second.s[idx].c_str(); return kOfxStatOK; }
OfxStatus pGetDouble(OfxPropertySetHandle h, const char* n, int idx, double* v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.d.size() <= idx) return kOfxStatErrUnknown; *v = it->second.d[idx]; return kOfxStatOK; }
OfxStatus pGetInt(OfxPropertySetHandle h, const char* n, int idx, int* v)
{ auto it = PS(h)->props.find(n); if (it == PS(h)->props.end() || (int)it->second.i.size() <= idx) return kOfxStatErrUnknown; *v = it->second.i[idx]; return kOfxStatOK; }
OfxStatus pGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v)
{ for (int k = 0; k < c; ++k) if (pGetPointer(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetStringN(OfxPropertySetHandle h, const char* n, int c, const char** v)
{ for (int k = 0; k < c; ++k) if (pGetString(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v)
{ for (int k = 0; k < c; ++k) if (pGetDouble(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v)
{ for (int k = 0; k < c; ++k) if (pGetInt(h, n, k, &v[k]) != kOfxStatOK) return kOfxStatErrUnknown; return kOfxStatOK; }
OfxStatus pReset(OfxPropertySetHandle h, const char* n) { PS(h)->props.erase(n); return kOfxStatOK; }
OfxStatus pGetDimension(OfxPropertySetHandle h, const char* n, int* c)
{
    auto it = PS(h)->props.find(n);
    if (it == PS(h)->props.end()) { *c = 0; return kOfxStatOK; }
    const Value& v = it->second;
    size_t m = v.i.size(); if (v.d.size() > m) m = v.d.size(); if (v.s.size() > m) m = v.s.size(); if (v.p.size() > m) m = v.p.size();
    *c = (int)m; return kOfxStatOK;
}

OfxPropertySuiteV1 gPropSuite = {
    pSetPointer, pSetString, pSetDouble, pSetInt,
    pSetPointerN, pSetStringN, pSetDoubleN, pSetIntN,
    pGetPointer, pGetString, pGetDouble, pGetInt,
    pGetPointerN, pGetStringN, pGetDoubleN, pGetIntN,
    pReset, pGetDimension
};

// ------------------------------------------------------------------ image effect suite
OfxStatus ieGetPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* out) { *out = H(&EF(e)->props); return kOfxStatOK; }
OfxStatus ieGetParamSet(OfxImageEffectHandle e, OfxParamSetHandle* out) { *out = reinterpret_cast<OfxParamSetHandle>(e); return kOfxStatOK; }
OfxStatus ieClipDefine(OfxImageEffectHandle e, const char*, OfxPropertySetHandle* out) { PropSet* ps = EF(e)->newChild(); if (out) *out = H(ps); return kOfxStatOK; }
OfxStatus ieClipGetPropertySet(OfxImageClipHandle c, OfxPropertySetHandle* out) { *out = reinterpret_cast<OfxPropertySetHandle>(c); return kOfxStatOK; }
OfxStatus ieClipGetHandle(OfxImageEffectHandle, const char*, OfxImageClipHandle*, OfxPropertySetHandle*) { return kOfxStatErrUnsupported; }
OfxStatus ieClipGetImage(OfxImageClipHandle, OfxTime, const OfxRectD*, OfxPropertySetHandle*) { return kOfxStatErrUnsupported; }
OfxStatus ieClipReleaseImage(OfxPropertySetHandle) { return kOfxStatOK; }
OfxStatus ieClipGetRoD(OfxImageClipHandle, OfxTime, OfxRectD*) { return kOfxStatErrUnsupported; }
int ieAbort(OfxImageEffectHandle) { return 0; }
OfxStatus ieMemAlloc(OfxImageEffectHandle, size_t n, OfxImageMemoryHandle* h) { *h = reinterpret_cast<OfxImageMemoryHandle>(malloc(n)); return *h ? kOfxStatOK : kOfxStatErrMemory; }
OfxStatus ieMemFree(OfxImageMemoryHandle h) { free(reinterpret_cast<void*>(h)); return kOfxStatOK; }
OfxStatus ieMemLock(OfxImageMemoryHandle h, void** p) { *p = reinterpret_cast<void*>(h); return kOfxStatOK; }
OfxStatus ieMemUnlock(OfxImageMemoryHandle) { return kOfxStatOK; }

OfxImageEffectSuiteV1 gEffectSuite = {
    ieGetPropertySet, ieGetParamSet, ieClipDefine, ieClipGetHandle,
    ieClipGetPropertySet, ieClipGetImage, ieClipReleaseImage, ieClipGetRoD,
    ieAbort, ieMemAlloc, ieMemFree, ieMemLock, ieMemUnlock
};

// ------------------------------------------------------------------ parameter suite
OfxStatus prmDefine(OfxParamSetHandle set, const char*, const char*, OfxPropertySetHandle* out) { Effect* e = reinterpret_cast<Effect*>(set); PropSet* ps = e->newChild(); if (out) *out = H(ps); return kOfxStatOK; }
OfxStatus prmGetHandle(OfxParamSetHandle set, const char*, OfxParamHandle* h, OfxPropertySetHandle* ps) { if (h) *h = reinterpret_cast<OfxParamHandle>(set); if (ps) *ps = H(&reinterpret_cast<Effect*>(set)->props); return kOfxStatOK; }
OfxStatus prmSetGetPropertySet(OfxParamSetHandle set, OfxPropertySetHandle* ps) { *ps = H(&reinterpret_cast<Effect*>(set)->props); return kOfxStatOK; }
OfxStatus prmGetPropertySet(OfxParamHandle p, OfxPropertySetHandle* ps) { *ps = H(&reinterpret_cast<Effect*>(p)->props); return kOfxStatOK; }
OfxStatus prmGetValue(OfxParamHandle, ...) { return kOfxStatErrUnsupported; }
OfxStatus prmGetValueAtTime(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus prmGetDerivative(OfxParamHandle, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus prmGetIntegral(OfxParamHandle, OfxTime, OfxTime, ...) { return kOfxStatErrUnsupported; }
OfxStatus prmSetValue(OfxParamHandle, ...) { return kOfxStatOK; }
OfxStatus prmSetValueAtTime(OfxParamHandle, OfxTime, ...) { return kOfxStatOK; }
OfxStatus prmGetNumKeys(OfxParamHandle, unsigned int* n) { if (n) *n = 0; return kOfxStatOK; }
OfxStatus prmGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrBadIndex; }
OfxStatus prmGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
OfxStatus prmDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatOK; }
OfxStatus prmDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
OfxStatus prmCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatOK; }
OfxStatus prmEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus prmEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

OfxParameterSuiteV1 gParamSuite = {
    prmDefine, prmGetHandle, prmSetGetPropertySet, prmGetPropertySet,
    prmGetValue, prmGetValueAtTime, prmGetDerivative, prmGetIntegral,
    prmSetValue, prmSetValueAtTime, prmGetNumKeys, prmGetKeyTime, prmGetKeyIndex,
    prmDeleteKey, prmDeleteAllKeys, prmCopy, prmEditBegin, prmEditEnd
};

// ------------------------------------------------------------------ memory suite
OfxStatus memAlloc(void*, size_t n, void** p) { *p = malloc(n); return *p ? kOfxStatOK : kOfxStatErrMemory; }
OfxStatus memFree(void* p) { free(p); return kOfxStatOK; }
OfxMemorySuiteV1 gMemSuite = { memAlloc, memFree };

// ------------------------------------------------------------------ multithread suite
OfxStatus mtRun(OfxThreadFunctionV1 func, unsigned int nThreads, void* arg)
{ unsigned int t = nThreads ? nThreads : 1; for (unsigned int k = 0; k < t; ++k) func(k, t, arg); return kOfxStatOK; }
OfxStatus mtNumCPUs(unsigned int* n) { *n = 1; return kOfxStatOK; }
OfxStatus mtIndex(unsigned int* n) { *n = 0; return kOfxStatOK; }
int mtIsSpawned(void) { return 0; }
OfxStatus mtxCreate(OfxMutexHandle* m, int) { *m = reinterpret_cast<OfxMutexHandle>(new std::mutex()); return kOfxStatOK; }
OfxStatus mtxDestroy(const OfxMutexHandle m) { delete reinterpret_cast<std::mutex*>(m); return kOfxStatOK; }
OfxStatus mtxLock(const OfxMutexHandle m) { reinterpret_cast<std::mutex*>(m)->lock(); return kOfxStatOK; }
OfxStatus mtxUnlock(const OfxMutexHandle m) { reinterpret_cast<std::mutex*>(m)->unlock(); return kOfxStatOK; }
OfxStatus mtxTryLock(const OfxMutexHandle m) { return reinterpret_cast<std::mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed; }
OfxMultiThreadSuiteV1 gMTSuite = { mtRun, mtNumCPUs, mtIndex, mtIsSpawned, mtxCreate, mtxDestroy, mtxLock, mtxUnlock, mtxTryLock };

// ------------------------------------------------------------------ message suite
OfxStatus msg(void*, const char* type, const char*, const char* fmt, ...)
{ va_list a; va_start(a, fmt); std::fprintf(stderr, "[ofx msg/%s] ", type ? type : "?"); std::vfprintf(stderr, fmt, a); std::fprintf(stderr, "\n"); va_end(a); return kOfxStatOK; }
OfxMessageSuiteV1 gMsgSuite = { msg };

// ------------------------------------------------------------------ interact suite (stubs)
OfxStatus itSwap(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus itRedraw(OfxInteractHandle) { return kOfxStatOK; }
OfxStatus itGetPropSet(OfxInteractHandle, OfxPropertySetHandle*) { return kOfxStatErrUnsupported; }
OfxInteractSuiteV1 gInteractSuite = { itSwap, itRedraw, itGetPropSet };

// ------------------------------------------------------------------ host fetchSuite
const void* fetchSuite(OfxPropertySetHandle, const char* name, int)
{
    if (!std::strcmp(name, kOfxPropertySuite)) return &gPropSuite;
    if (!std::strcmp(name, kOfxImageEffectSuite)) return &gEffectSuite;
    if (!std::strcmp(name, kOfxParameterSuite)) return &gParamSuite;
    if (!std::strcmp(name, kOfxMemorySuite)) return &gMemSuite;
    if (!std::strcmp(name, kOfxMultiThreadSuite)) return &gMTSuite;
    if (!std::strcmp(name, kOfxMessageSuite)) return &gMsgSuite;
    if (!std::strcmp(name, kOfxInteractSuite)) return &gInteractSuite;
    return nullptr; // optional suites not provided
}

void initHostProps()
{
    OfxPropertySetHandle h = H(&gHostProps);
    pSetString(h, kOfxPropName, 0, "org.openfx-io.tests.minhost");
    pSetString(h, kOfxPropLabel, 0, "openfx-io minimal test host");
    pSetInt(h, kOfxImageEffectHostPropIsBackground, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
    pSetInt(h, kOfxImageEffectPropSupportsTiles, 0, 1);
    pSetInt(h, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
    pSetInt(h, kOfxImageEffectPropTemporalClipAccess, 0, 1);
    pSetInt(h, kOfxImageEffectPropSetableFrameRate, 0, 0);
    pSetInt(h, kOfxImageEffectPropSetableFielding, 0, 0);
    pSetInt(h, kOfxImageEffectPropSupportsOverlays, 0, 0);
    const char* ctx[] = { kOfxImageEffectContextGenerator, kOfxImageEffectContextFilter, kOfxImageEffectContextGeneral, kOfxImageEffectContextReader, kOfxImageEffectContextWriter };
    pSetStringN(h, kOfxImageEffectPropSupportedContexts, 5, ctx);
    const char* comps[] = { kOfxImageComponentRGBA, kOfxImageComponentRGB, kOfxImageComponentAlpha };
    pSetStringN(h, kOfxImageEffectPropSupportedComponents, 3, comps);
    const char* depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthFloat };
    pSetStringN(h, kOfxImageEffectPropSupportedPixelDepths, 3, depths);

    // API/version.
    pSetInt(h, kOfxPropAPIVersion, 0, 1);
    pSetInt(h, kOfxPropAPIVersion, 1, 4);
    pSetInt(h, kOfxPropVersion, 0, 1);

    // Parameter-host capabilities (queried when the Support library builds its
    // ParamHostDescription during load).
    pSetInt(h, kOfxParamHostPropSupportsStringAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsCustomAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsChoiceAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsBooleanAnimation, 0, 1);
    pSetInt(h, kOfxParamHostPropSupportsCustomInteract, 0, 0);
    pSetInt(h, kOfxParamHostPropMaxParameters, 0, -1);
    pSetInt(h, kOfxParamHostPropMaxPages, 0, 30);
    pSetInt(h, kOfxParamHostPropPageRowColumnCount, 0, 10);
    pSetInt(h, kOfxParamHostPropPageRowColumnCount, 1, 10);

    gOfxHost.host = h;
    gOfxHost.fetchSuite = fetchSuite;
}

bool actionOK(OfxStatus s) { return s == kOfxStatOK || s == kOfxStatReplyDefault; }

} // namespace

int
main()
{
    initHostProps();
    const int n = OfxGetNumberOfPlugins();
    std::printf("plugins=%d\n", n);
    int failures = 0;

    for (int i = 0; i < n; ++i) {
        OfxPlugin* p = OfxGetPlugin(i);
        std::printf("== %s v%d.%d ==\n", p->pluginIdentifier, p->pluginVersionMajor, p->pluginVersionMinor);
        p->setHost(&gOfxHost);

        OfxStatus st = p->mainEntry(kOfxActionLoad, nullptr, nullptr, nullptr);
        std::printf("  Load = %d %s\n", st, actionOK(st) ? "OK" : "FAIL");
        if (!actionOK(st)) { ++failures; continue; }

        Effect describeEffect;
        st = p->mainEntry(kOfxActionDescribe, reinterpret_cast<OfxImageEffectHandle>(&describeEffect), nullptr, nullptr);
        std::printf("  Describe = %d %s\n", st, actionOK(st) ? "OK" : "FAIL");
        if (!actionOK(st)) { ++failures; }

        std::vector<std::string> contexts;
        {
            auto it = describeEffect.props.props.find(kOfxImageEffectPropSupportedContexts);
            if (it != describeEffect.props.props.end()) {
                contexts = it->second.s;
            }
        }
        std::printf("  supported contexts:");
        for (const std::string& c : contexts) std::printf(" %s", c.c_str());
        std::printf("\n");
        if (contexts.empty()) { std::printf("  FAIL: no contexts declared\n"); ++failures; }

        for (const std::string& c : contexts) {
            Effect ctxEffect;
            PropSet inArgs;
            pSetString(H(&inArgs), kOfxImageEffectPropContext, 0, c.c_str());
            st = p->mainEntry(kOfxImageEffectActionDescribeInContext,
                              reinterpret_cast<OfxImageEffectHandle>(&ctxEffect),
                              H(&inArgs), nullptr);
            std::printf("  DescribeInContext(%s) = %d %s\n", c.c_str(), st, actionOK(st) ? "OK" : "FAIL");
            if (!actionOK(st)) { ++failures; }
        }

        st = p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
        std::printf("  Unload = %d %s\n", st, actionOK(st) ? "OK" : "FAIL");
        if (!actionOK(st)) { ++failures; }
    }

    std::printf("Result: failures=%d\n", failures);
    std::printf(failures == 0 ? "PASS\n" : "FAIL\n");
    return failures == 0 ? 0 : 1;
}
