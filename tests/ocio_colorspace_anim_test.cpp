/*
 * ocio_colorspace_anim_test — the animated colorspaces of GenericOCIO (IOSupport/GenericOCIO.cpp, issue #128), in
 * OCIOColorSpace, WriteOIIO and ReadOIIO.
 *
 * The colorspace string params (ocioInputSpace, ocioOutputSpace) are rendered and saved; the choice params
 * (ocioInputSpaceIndex, ocioOutputSpaceIndex) are their menus, which are not saved. In a host that animates strings
 * and choices, the menus must have the same keys as the strings, whatever the user does to the menus' keys, Swap
 * Colorspaces must swap the keys, and no key may be set outside of an instance changed action.
 *
 * The plug-ins are linked directly and driven by the in-process host of ofx_test_host.h, whose parameter suite is
 * replaced here by one that animates string and choice params like Natron does (Engine/Knob*.cpp,
 * Engine/OfxParamInstance.cpp, Gui/KnobUndoCommand.cpp), and which declares kNatronOfxHostIsNatron:
 * - string and choice params have keys, and their value is held from a key to the next one (and before the first
 *   key, it is that of the first key);
 * - a param with an expression reports two keys, at times 0 and 1, whatever its keys, and its value is that of the
 *   expression;
 * - a plug-in's paramSetValue on a param that has keys sets a key at the host's current time; paramSetValueAtTime
 *   sets a key at the given time; if this changes the param, both call kOfxActionInstanceChanged
 *   (kOfxChangePluginEdited, at the time of the key, or the current time) before they return;
 * - a plug-in's paramDeleteKey calls kOfxActionInstanceChanged as a user edit (kOfxChangeUserEdited) at the time of
 *   the key (the param's value is not changed, even if this was its last key); paramDeleteAllKeys, at the current
 *   time, after giving the param its value at the current time (nothing happens if it has no keys);
 * - a user edit of a param that has keys sets a key at the current time, then calls kOfxActionInstanceChanged at
 *   that time; deleting keys in the curve editor gives the param its value at the current time, then deletes them
 *   one by one, each reported at its time; moving keys is reported at the current time (curve editor) or at each
 *   key's new time (dope sheet); pasting keys and removing the animation (if there are keys) at the current time;
 *   restoring the default value is reported twice;
 * - the plug-in's changes between paramEditBegin and paramEditEnd are one undo step, as in Natron's
 *   MultipleKnobEditsUndoCommand: its undo removes the keys that were added (a param whose last key is removed gets
 *   its value at the current time), sets back the previous values of the keys that were changed, and the previous
 *   value of a param without keys; the keys that were deleted are not restored, and params that don't evaluate on
 *   change (the menus) are not undone;
 * - loading a project restores the params before kOfxActionCreateInstance (the choice params, which are not
 *   persistent, keep their default value), and calls no kOfxActionInstanceChanged.
 * The colorspaces are those of swap.ocio, where xN is N times the reference (x1, the default): the image (0.25) is
 * rendered as 0.25 * N / M from xN to xM.
 *
 * Usage: ocio_colorspace_anim_test   (needs $OCIO=swap.ocio, see tests/Makefile)
 * Exit status: 0 = all checks passed.
 */
#include "ofx_test_host.h"

#include "ofxsImageEffect.h" // OFX::getImageEffectHostDescription()

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

// from IOSupport/GenericOCIO.h, GenericReader.cpp, GenericWriter.cpp
const char* const kIS = "ocioInputSpace";
const char* const kIC = "ocioInputSpaceIndex";
const char* const kOS = "ocioOutputSpace";
const char* const kOC = "ocioOutputSpaceIndex";
const char* const kSwap = "ocioSwapColorspaces";
const char* const kConfigFile = "ocioConfigFile";
const char* const kKey1 = "key1"; // an OCIO context key: a param that GenericOCIO::changedParam() gets, but ignores
const char* const kInputSpaceSet = "ocioInputSpaceSet";
const char* const kOutputSpaceSet = "ocioOutputSpaceSet";
const char* const kGuessedParams = "ParamExistingInstance";
const char* const kFilename = "filename";

const int W = 4;
const int H = 2;
const double kInput = 0.25;

int gFailures = 0;
bool gQuiet = false; // (fuzzer) don't print the checks that pass
std::function<void(const char*)> gOnFail; // (fuzzer) called with the message of a failed check

void
check(bool cond,
      const char* fmt,
      ...)
{
    if (cond && gQuiet) {
        return;
    }
    if (!cond && gOnFail) {
        char buf[1024];
        va_list b;
        va_start(b, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, b);
        va_end(b);
        ++gFailures;
        gOnFail(buf);
        return;
    }
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

// ------------------------------------------------------------------ the Natron-like animated parameter suite
bool gStringAnimation = true; // kOfxParamHostPropSupportsStringAnimation
bool gChoiceAnimation = true; // kOfxParamHostPropSupportsChoiceAnimation
double gCurrentTime = 0.; // the time of the host's timeline

// the keys of a string or choice param (time -> value), and its expression, by param
struct Anim {
    std::map<double, std::string> s;
    std::map<double, int> sidx; // (gNatronExact) the curve of a string param: the index of the key's string, as of its last set
    std::map<double, int> i;
    std::function<std::string(double)> expr; // a string param's expression (optional)
    std::function<int(double)> iexpr; // a choice param's expression, or link (optional)
    size_t size() const { return s.size() + i.size(); }
};
std::map<ParamRec*, Anim> gAnim;

struct Plug {
    const char* id;
    const char* context;
    OfxPlugin* p = nullptr;
    Effect describe;
    Effect ctx;
};
Plug* gPlug = nullptr; // the plug-in of the instance
Effect* gInst = nullptr; // the instance, once created: the param suite then calls its instance changed action
int gCalls = 0; // the number of kOfxActionInstanceChanged calls
int gDepth = 0;
int gMaxDepth = 0;
const int kMaxDepth = 16; // a deeper re-entry is a loop
bool gLoop = false;
std::vector<std::string> gLog; // "name reason time", for the failures
std::function<void(const char*, const char*, double)> gOnInstanceChanged; // (fuzzer)
int gKeyWritesOutsideInstanceChanged = 0; // keys set by the plug-in outside of an instance changed action

bool
isString(ParamRec* r)
{
    return r->type == kOfxParamTypeString;
}

bool
isChoice(ParamRec* r)
{
    return r->type == kOfxParamTypeChoice;
}

bool
animates(ParamRec* r)
{
    if (!((isString(r) && gStringAnimation) || (isChoice(r) && gChoiceAnimation))) {
        return false;
    }
    int a = 1;
    pGetInt(PH(r->props), kOfxParamPropAnimates, 0, &a);

    return a != 0;
}

bool
evaluatesOnChange(ParamRec* r)
{
    int e = 1;
    pGetInt(PH(r->props), kOfxParamPropEvaluateOnChange, 0, &e);

    return e != 0;
}

// gNatronExact (optional, OCIO_ANIM_NATRON_EXACT): model the string keys of Natron versions without
// NatronGitHub/Natron#1291, which fixes NatronGitHub/Natron#1290 (Engine/StringAnimationManager.cpp, Curve.cpp,
// KnobImpl.h); by default, the host models Natron with the fix. Without it:
// - the curve key of a string param holds the index of its string in the sorted list of string keys, computed when
//   the key is set: inserting or deleting a key before other keys does not update theirs, which then read another
//   string (or the last one, if the index is out of range);
// - setting an existing key whose index is unchanged is "nothing changed": no kOfxActionInstanceChanged;
// - KnobGui::setValueAtTime() returns true for a modified key too, so that MultipleKnobEditsUndoCommand records it as
//   an added key, and its undo removes it.
bool gNatronExact = false;
// OfxStringInstance::get(time) reads the value at floor(time + 0.5) (not a bug: Natron's OFX string params), but
// OfxStringInstance::set(time) sets a key at time
bool gRoundStringTime = true;

template <typename T>
const T&
heldValue(const std::map<double, T>& keys,
          double t)
{
    typename std::map<double, T>::const_iterator it = keys.upper_bound(t + 1e-9);
    if (it == keys.begin()) {
        return it->second;
    }
    --it;

    return it->second;
}

// the string of a param with keys at time t
std::string
sValue(const Anim& k,
       double t)
{
    if (!gNatronExact) {
        return heldValue(k.s, t);
    }
    const int idx = heldValue(k.sidx, t);
    if (idx >= (int)k.s.size()) {
        return k.s.rbegin()->second;
    }
    std::map<double, std::string>::const_iterator it = k.s.begin();
    std::advance(it, idx);

    return it->second;
}

// set a string key: 0 = nothing changed, 1 = modified, 2 = added
int
sSetKey(Anim& k,
        double t,
        const std::string& v)
{
    const bool existed = k.s.count(t) > 0;
    if (!gNatronExact) {
        // (with NatronGitHub/Natron#1291, setting a key that has neighbours to its string is a modification: the
        // derivatives of the curve are compared too)
        const bool ch = !existed || (k.s[t] != v) || (k.s.size() > 1);
        k.s[t] = v;
        k.sidx[t] = 0;

        return !existed ? 2 : (ch ? 1 : 0);
    }
    k.s[t] = v;
    const int idx = (int)std::distance(k.s.begin(), k.s.find(t));
    if (!existed) {
        k.sidx[t] = idx;

        return 2;
    }
    const bool ch = (k.sidx[t] != idx);
    k.sidx[t] = idx;

    return ch ? 1 : 0;
}

void
sErase(Anim& k,
       double t)
{
    k.s.erase(t);
    k.sidx.erase(t);
}

void
sAssign(Anim& k,
        const std::map<double, std::string>& m,
        const std::map<double, int>* idx = nullptr)
{
    k.s = m;
    k.sidx.clear();
    int i = 0;
    for (std::map<double, std::string>::const_iterator it = m.begin(); it != m.end(); ++it, ++i) {
        k.sidx[it->first] = (idx && idx->count(it->first)) ? idx->at(it->first) : i;
    }
}

std::string
stringAt(ParamRec* r,
         double t)
{
    const Anim& k = gAnim[r];
    if (k.expr) {
        return k.expr(t);
    }

    return k.s.empty() ? std::string(paramString(r)) : sValue(k, t);
}

int
intAt(ParamRec* r,
      double t)
{
    const Anim& k = gAnim[r];
    if (k.iexpr) {
        return k.iexpr(t);
    }

    return k.i.empty() ? paramInt(r, 0) : heldValue(k.i, t);
}

void
setStatic(ParamRec* r,
          const std::string& s)
{
    r->hasOv = true;
    r->ov.s = { s };
}

void
setStatic(ParamRec* r,
          int v)
{
    r->hasOv = true;
    r->ov.i = { v };
}

// the value at the current time becomes the param's value, and its keys are deleted
void
removeAnimation(ParamRec* r)
{
    Anim& k = gAnim[r];
    if (isString(r) && !k.s.empty()) {
        setStatic(r, stringAt(r, gCurrentTime));
    } else if (!k.i.empty()) {
        setStatic(r, intAt(r, gCurrentTime));
    }
    k.s.clear();
    k.sidx.clear();
    k.i.clear();
}

// the key times that the host reports (0 and 1 for a param with an expression)
std::vector<double>
keyTimes(ParamRec* r)
{
    std::vector<double> times;
    const Anim& k = gAnim[r];
    if (k.expr || k.iexpr) {
        return { 0., 1. };
    }
    for (std::map<double, std::string>::const_iterator it = k.s.begin(); it != k.s.end(); ++it) {
        times.push_back(it->first);
    }
    for (std::map<double, int>::const_iterator it = k.i.begin(); it != k.i.end(); ++it) {
        times.push_back(it->first);
    }

    return times;
}

// kOfxActionInstanceChanged, as the host calls it
void
instanceChanged(const char* name,
                const char* reason,
                double t)
{
    if (!gInst) {
        return;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%*s%s %s t=%g", 2 * gDepth, "", name, reason, t);
    gLog.push_back(buf);
    if (gDepth >= kMaxDepth) {
        gLoop = true;

        return;
    }
    ++gCalls;
    if (gOnInstanceChanged) {
        gOnInstanceChanged(name, reason, t);
    }
    ++gDepth;
    gMaxDepth = std::max(gMaxDepth, gDepth);
    PropSet a;
    OfxPropertySetHandle ap = PH(&a);
    pSetString(ap, kOfxPropType, 0, kOfxTypeParameter);
    pSetString(ap, kOfxPropName, 0, name);
    pSetString(ap, kOfxPropChangeReason, 0, reason);
    pSetDouble(ap, kOfxPropTime, 0, t);
    double rs[2] = { 1., 1. };
    pSetDoubleN(ap, kOfxImageEffectPropRenderScale, 2, rs);
    OfxStatus st = gPlug->p->mainEntry(kOfxActionInstanceChanged, reinterpret_cast<OfxImageEffectHandle>(gInst), ap, nullptr);
    if (!ok(st)) {
        check(false, "instanceChanged(%s, %s, %g) returns %d", name, reason, t, st);
    }
    --gDepth;
}

// ---- undo (Gui/KnobUndoCommand.cpp, MultipleKnobEditsUndoCommand)
struct UndoOp {
    ParamRec* r;
    double time;
    bool key; // a key was set
    bool keyAdded; // ... where there was none
    std::string oldS; // the previous value (at that time)
    int oldI;
    std::string newS; // the new value (for redo)
    int newI;
};
bool gRecording = false;
std::vector<UndoOp> gBlock; // the changes of the current edit block
std::vector<UndoOp> gLastBlock; // those of the last one: the undo step

std::vector<UndoOp> gRedoBlock; // the undone block (redo)
bool gMergeBlocks = true; // model Natron's merge of consecutive MultipleKnobEditsUndoCommands

// A user edit of a param that evaluates on change is pushed on the panel's undo stack (Gui/KnobGui.cpp,
// pushUndoCommand()): the plug-in's edit block is then no longer the last command, and the undone ones are deleted
void
userCommandPushed()
{
    gLastBlock.clear();
    gRedoBlock.clear();
}

OfxStatus
aEditBegin(OfxParamSetHandle,
           const char*)
{
    gRecording = true;
    gBlock.clear();

    return kOfxStatOK;
}

// The new undo command is pushed (QUndoStack::push() deletes the undone commands), and merged into the last one if
// its first param is in it (MultipleKnobEditsUndoCommand::mergeWith(), Gui/KnobUndoCommand.cpp): the merged command
// keeps the previous values of the first one
OfxStatus
aEditEnd(OfxParamSetHandle)
{
    gRecording = false;
    gRedoBlock.clear();
    if (gBlock.empty()) {
        return kOfxStatOK;
    }
    bool merge = false;
    if (gMergeBlocks) {
        for (size_t i = 0; i < gLastBlock.size(); ++i) {
            if (gLastBlock[i].r == gBlock[0].r) {
                merge = true;
            }
        }
    }
    if (!merge) {
        gLastBlock = gBlock;

        return kOfxStatOK;
    }
    for (size_t i = 0; i < gBlock.size(); ++i) {
        const UndoOp& v = gBlock[i];
        bool found = false;
        for (size_t j = 0; j < gLastBlock.size(); ++j) {
            UndoOp& w = gLastBlock[j];
            if ((w.r == v.r) && (w.time == v.time) && (w.key == v.key)) {
                w.newS = v.newS;
                w.newI = v.newI;
                found = true;
                break;
            }
        }
        if (!found) {
            gLastBlock.push_back(v);
        }
    }

    return kOfxStatOK;
}

// a copy of s that stays valid until 16 more calls (the plug-in copies the value of a string param at once)
const char*
keepString(const std::string& s)
{
    static std::string ring[16];
    static int n = 0;
    std::string& r = ring[n++ % 16];
    r = s;

    return r.c_str();
}

OfxStatus
aGetValueAtTime(OfxParamHandle ph,
                OfxTime time,
                ...)
{
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    va_list ap;
    va_start(ap, time);
    OfxStatus s = kOfxStatOK;
    if (isString(r)) {
        // (Engine/OfxParamInstance.cpp, OfxStringInstance::get(time))
        *va_arg(ap, const char**) = keepString(stringAt(r, gRoundStringTime ? std::floor(time + 0.5) : time));
    } else if (isChoice(r)) {
        *va_arg(ap, int*) = intAt(r, time);
    } else {
        s = prmGetV(ph, ap);
    }
    va_end(ap);

    return s;
}

OfxStatus
aGetValue(OfxParamHandle ph,
          ...)
{
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    va_list ap;
    va_start(ap, ph);
    OfxStatus s = kOfxStatOK;
    if (isString(r)) {
        *va_arg(ap, const char**) = keepString(stringAt(r, gCurrentTime));
    } else if (isChoice(r)) {
        *va_arg(ap, int*) = intAt(r, gCurrentTime);
    } else {
        s = prmGetV(ph, ap);
    }
    va_end(ap);

    return s;
}

// set a value, a key at time if atTime (or if the param has keys, at the current time), and report it if it changed
OfxStatus
aSet(ParamRec* r,
     bool atTime,
     double time,
     va_list ap)
{
    Anim& k = gAnim[r];
    if (!atTime) {
        time = gCurrentTime;
    }
    const bool key = animates(r) && (atTime || (k.size() > 0));
    UndoOp op = { r, time, key, false, std::string(), 0, std::string(), 0 };
    bool changed = true;
    if (isString(r)) {
        const char* c = va_arg(ap, const char*);
        const std::string v = c ? c : "";
        op.newS = v;
        if (key) {
            op.oldS = stringAt(r, time);
            const int rc = sSetKey(k, time, v);
            // (KnobGui::setValueAtTime() returns true for an added or a modified key)
            op.keyAdded = gNatronExact ? (rc != 0) : (rc == 2);
            changed = (rc != 0);
        } else {
            op.oldS = paramString(r);
            changed = (op.oldS != v);
            setStatic(r, v);
        }
    } else if (isChoice(r)) {
        const int v = va_arg(ap, int);
        op.newI = v;
        if (key) {
            std::map<double, int>::iterator it = k.i.find(time);
            op.keyAdded = (it == k.i.end());
            op.oldI = op.keyAdded ? intAt(r, time) : it->second;
            changed = op.keyAdded || (it->second != v);
            k.i[time] = v;
        } else {
            op.oldI = paramInt(r, 0);
            changed = (op.oldI != v);
            setStatic(r, v);
        }
    } else {
        prmSetV(reinterpret_cast<OfxParamHandle>(r), ap);
    }
    if (key && changed && (gDepth == 0)) {
        ++gKeyWritesOutsideInstanceChanged;
    }
    if ((changed || gNatronExact) && gRecording && evaluatesOnChange(r) && (isString(r) || isChoice(r))) {
        gBlock.push_back(op);
    }
    if (changed) {
        instanceChanged(r->name.c_str(), kOfxChangePluginEdited, time);
    }

    return kOfxStatOK;
}

OfxStatus
aSetValue(OfxParamHandle ph,
          ...)
{
    va_list ap;
    va_start(ap, ph);
    OfxStatus s = aSet(reinterpret_cast<ParamRec*>(ph), false, 0., ap);
    va_end(ap);

    return s;
}

OfxStatus
aSetValueAtTime(OfxParamHandle ph,
                OfxTime time,
                ...)
{
    va_list ap;
    va_start(ap, time);
    OfxStatus s = aSet(reinterpret_cast<ParamRec*>(ph), true, time, ap);
    va_end(ap);

    return s;
}

// (a host that doesn't animate a type of param fails to give the keys of these params)
bool
hostAnimatesType(ParamRec* r)
{
    return !((isString(r) && !gStringAnimation) || (isChoice(r) && !gChoiceAnimation));
}

OfxStatus
aGetNumKeys(OfxParamHandle ph,
            unsigned int* n)
{
    if (!hostAnimatesType(reinterpret_cast<ParamRec*>(ph))) {
        return kOfxStatErrUnsupported;
    }
    *n = (unsigned int)keyTimes(reinterpret_cast<ParamRec*>(ph)).size();

    return kOfxStatOK;
}

OfxStatus
aGetKeyTime(OfxParamHandle ph,
            unsigned int nth,
            OfxTime* time)
{
    if (!hostAnimatesType(reinterpret_cast<ParamRec*>(ph))) {
        return kOfxStatErrUnsupported;
    }
    const std::vector<double> times = keyTimes(reinterpret_cast<ParamRec*>(ph));
    if (nth >= times.size()) {
        return kOfxStatErrBadIndex;
    }
    *time = times[nth];

    return kOfxStatOK;
}

OfxStatus
aGetKeyIndex(OfxParamHandle ph,
             OfxTime time,
             int direction,
             int* index)
{
    if (!hostAnimatesType(reinterpret_cast<ParamRec*>(ph))) {
        return kOfxStatErrUnsupported;
    }
    const std::vector<double> times = keyTimes(reinterpret_cast<ParamRec*>(ph));
    *index = -1;
    for (size_t i = 0; i < times.size(); ++i) {
        if ((direction == 0) && (std::fabs(times[i] - time) < 1e-6)) {
            *index = (int)i;
        } else if ((direction > 0) && (times[i] > time + 1e-6) && (*index < 0)) {
            *index = (int)i;
        } else if ((direction < 0) && (times[i] < time - 1e-6)) {
            *index = (int)i;
        }
    }

    return (*index >= 0) ? kOfxStatOK : kOfxStatFailed;
}

// (the param's value is not changed: Natron's deleteValueAtTime() from a plug-in doesn't copy the curve's value)
OfxStatus
aDeleteKey(OfxParamHandle ph,
           OfxTime time)
{
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    int index;
    if (aGetKeyIndex(ph, time, 0, &index) != kOfxStatOK) {
        return kOfxStatErrBadIndex;
    }
    Anim& k = gAnim[r];
    const double t = keyTimes(r)[index];
    const size_t n = k.size();
    sErase(k, t);
    k.i.erase(t);
    if (gDepth == 0) {
        ++gKeyWritesOutsideInstanceChanged;
    }
    if (k.size() != n) {
        instanceChanged(r->name.c_str(), kOfxChangeUserEdited, time);
    }

    return kOfxStatOK;
}

OfxStatus
aDeleteAllKeys(OfxParamHandle ph)
{
    ParamRec* r = reinterpret_cast<ParamRec*>(ph);
    if (gAnim[r].size() == 0) {
        return kOfxStatOK;
    }
    if (gDepth == 0) {
        ++gKeyWritesOutsideInstanceChanged;
    }
    removeAnimation(r);
    instanceChanged(r->name.c_str(), kOfxChangeUserEdited, gCurrentTime);

    return kOfxStatOK;
}

void
installAnimatedParamSuite()
{
    gParamSuite.paramGetValue = aGetValue;
    gParamSuite.paramGetValueAtTime = aGetValueAtTime;
    gParamSuite.paramSetValue = aSetValue;
    gParamSuite.paramSetValueAtTime = aSetValueAtTime;
    gParamSuite.paramGetNumKeys = aGetNumKeys;
    gParamSuite.paramGetKeyTime = aGetKeyTime;
    gParamSuite.paramGetKeyIndex = aGetKeyIndex;
    gParamSuite.paramDeleteKey = aDeleteKey;
    gParamSuite.paramDeleteAllKeys = aDeleteAllKeys;
    gParamSuite.paramEditBegin = aEditBegin;
    gParamSuite.paramEditEnd = aEditEnd;
}

// ------------------------------------------------------------------ the test session
ParamRec*
P(const char* name)
{
    std::map<std::string, ParamRec>::iterator it = gPlug->ctx.params.find(name);
    if (it == gPlug->ctx.params.end()) {
        std::printf("FAIL: no param %s\n", name);
        std::exit(1);
    }

    return &it->second;
}

// the scale N of a colorspace xN of swap.ocio (a role is x1), or 0
int
scaleOf(const std::string& name)
{
    if ((name == "x1") || (name == "scene_linear") || (name == "reference") || (name == "default") || (name == "data")) {
        return 1;
    }
    if ((name.size() == 2) && (name[0] == 'x') && (name[1] >= '1') && (name[1] <= '4')) {
        return name[1] - '0';
    }

    return 0;
}

// a param's colorspaces, as "{t:N,...}" (keys) or "=N" (no keys); for a choice, N is the menu entry + 1 (the menu is
// x1 x2 x3 x4). The real keys of a param with an expression.
std::string
anim(const char* name)
{
    ParamRec* r = P(name);
    const Anim& k = gAnim[r];
    std::ostringstream os;
    if (k.size() == 0) {
        os << '=' << (isString(r) ? scaleOf(paramString(r)) : paramInt(r, 0) + 1);

        return os.str();
    }
    os << '{';
    bool first = true;
    for (std::map<double, std::string>::const_iterator it = k.s.begin(); it != k.s.end(); ++it) {
        os << (first ? "" : ",") << it->first << ':' << scaleOf(sValue(k, it->first));
        first = false;
    }
    for (std::map<double, int>::const_iterator it = k.i.begin(); it != k.i.end(); ++it) {
        os << (first ? "" : ",") << it->first << ':' << it->second + 1;
        first = false;
    }
    os << '}';

    return os.str();
}

void
checkAnim(const char* name,
          const std::string& expected)
{
    const std::string got = anim(name);
    check(got == expected, "%s: %s (expected %s)", name, got.c_str(), expected.c_str());
}

// the string and choice params of both sides
void
checkSides(const std::string& input,
           const std::string& output,
           bool mirror = true)
{
    checkAnim(kIS, input);
    checkAnim(kOS, output);
    if (mirror) {
        checkAnim(kIC, input);
        checkAnim(kOC, output);
    }
}

void
dumpLog()
{
    if (gQuiet) {
        return;
    }
    for (size_t i = 0; i < gLog.size(); ++i) {
        std::printf("      %s\n", gLog[i].c_str());
    }
}

// a user action: checks the number of instance changed actions, and that there was no loop
struct UserAction {
    std::string what;
    int calls0;
    int maxCalls;
    int failures0;

    UserAction(const std::string& w,
               int maxc)
        : what(w)
        , calls0(gCalls)
        , maxCalls(maxc)
        , failures0(gFailures)
    {
        gLog.clear();
        gMaxDepth = 0;
        gLoop = false;
    }

    void done()
    {
        const int n = gCalls - calls0;
        check(!gLoop && (n <= maxCalls), "%s: %d instance changed actions (at most %d), depth %d%s", what.c_str(), n, maxCalls, gMaxDepth, gLoop ? ", LOOP" : "");
        if (gFailures != failures0) {
            dumpLog();
        }
    }
};

std::string
at()
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), " at t=%g", gCurrentTime);

    return buf;
}

// the user sets a param at the current time (a key, if it has keys or if key is set)
void
userSet(const char* name,
        int v,
        bool key = false,
        int maxCalls = 12)
{
    UserAction u(std::string(key ? "set key on " : "set ") + name + at(), maxCalls);
    ParamRec* r = P(name);
    Anim& k = gAnim[r];
    bool changed;
    if (animates(r) && (key || (k.size() > 0))) {
        changed = (k.i.count(gCurrentTime) == 0) || (k.i[gCurrentTime] != v);
        k.i[gCurrentTime] = v;
    } else {
        changed = (paramInt(r, 0) != v);
        setStatic(r, v);
    }
    if (changed && evaluatesOnChange(r)) {
        userCommandPushed();
    }
    if (changed) {
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    u.done();
}

void
userSetString(const char* name,
              const std::string& v,
              bool key = false,
              int maxCalls = 12)
{
    UserAction u(std::string(key ? "set key on " : "set ") + name + at(), maxCalls);
    ParamRec* r = P(name);
    Anim& k = gAnim[r];
    bool changed;
    if (animates(r) && (key || (k.size() > 0))) {
        changed = (sSetKey(k, gCurrentTime, v) != 0);
    } else {
        changed = (paramString(r) != v);
        setStatic(r, v);
    }
    if (changed && evaluatesOnChange(r)) {
        userCommandPushed();
    }
    if (changed) {
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    u.done();
}

void
userPress(const char* name,
          int maxCalls = 40)
{
    UserAction u(std::string("press ") + name + at(), maxCalls);
    instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    u.done();
}

// the user deletes keys in the curve editor: the param gets its value at the current time, then the keys are
// deleted one by one, each reported at its time
void
userDeleteKeys(const char* name,
               const std::vector<double>& times,
               int maxCalls = 24)
{
    UserAction u(std::string("delete keys of ") + name + at(), maxCalls);
    ParamRec* r = P(name);
    Anim& k = gAnim[r];
    if (k.size() > 0) {
        if (isString(r)) {
            setStatic(r, stringAt(r, gCurrentTime));
        } else {
            setStatic(r, intAt(r, gCurrentTime));
        }
    }
    for (size_t i = 0; i < times.size(); ++i) {
        const size_t n = k.size();
        sErase(k, times[i]);
        k.i.erase(times[i]);
        if (k.size() != n) {
            instanceChanged(name, kOfxChangeUserEdited, times[i]);
        }
    }
    u.done();
}

// the user moves a key in the curve editor (reported at the current time)
void
userMoveKey(const char* name,
            double from,
            double to,
            int maxCalls = 12)
{
    UserAction u(std::string("move key of ") + name + " in the curve editor" + at(), maxCalls);
    Anim& k = gAnim[P(name)];
    const int v = k.i[from];
    k.i.erase(from);
    k.i[to] = v;
    instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    u.done();
}

// the user moves keys in the dope sheet (each reported at its new time)
void
userDopeSheetMoveKeys(const char* name,
                      const std::vector<double>& from,
                      double dt,
                      int maxCalls = 24)
{
    UserAction u(std::string("move keys of ") + name + " in the dope sheet", maxCalls);
    Anim& k = gAnim[P(name)];
    std::map<double, int> moved;
    for (size_t i = 0; i < from.size(); ++i) {
        moved[from[i] + dt] = k.i[from[i]];
        k.i.erase(from[i]);
    }
    for (std::map<double, int>::const_iterator it = moved.begin(); it != moved.end(); ++it) {
        k.i[it->first] = it->second;
    }
    for (std::map<double, int>::const_iterator it = moved.begin(); it != moved.end(); ++it) {
        instanceChanged(name, kOfxChangeUserEdited, it->first);
    }
    u.done();
}

// the user pastes keys (reported at the current time)
void
userPasteKeys(const char* name,
              const std::map<double, int>& keys,
              int maxCalls = 12)
{
    UserAction u(std::string("paste keys of ") + name + at(), maxCalls);
    Anim& k = gAnim[P(name)];
    for (std::map<double, int>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        k.i[it->first] = it->second;
    }
    instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    u.done();
}

void
userRemoveAnimation(const char* name,
                    int maxCalls = 12)
{
    UserAction u(std::string("remove animation of ") + name + at(), maxCalls);
    ParamRec* r = P(name);
    if (gAnim[r].size() > 0) {
        removeAnimation(r);
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    u.done();
}

// Natron's Knob<T>::resetToDefaultValueWithoutSecretNessAndEnabledNess() (Engine/KnobImpl.h): removeAnimation()
// (reported, if the param has keys), then setValue(default, eValueChangedReasonRestoreDefault, NULL): the static value,
// never a key (even if the plug-in set keys again in the meantime), reported if it changed
void
userRestoreDefault(const char* name,
                   int maxCalls = 24)
{
    UserAction u(std::string("restore default of ") + name + at(), maxCalls);
    ParamRec* r = P(name);
    if (gAnim[r].size() > 0) {
        removeAnimation(r);
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    int def = 0;
    pGetInt(PH(r->props), kOfxParamPropDefault, 0, &def);
    const bool changed = (paramInt(r, 0) != def);
    setStatic(r, def);
    if (changed) {
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    u.done();
}

// Undo the last edit block of the plug-in, as Natron's MultipleKnobEditsUndoCommand::undo() does
void
userUndo(int maxCalls = 40)
{
    UserAction u("undo", maxCalls);
    std::set<ParamRec*> valuesUndone;
    std::vector<UndoOp> ops = gLastBlock;
    gLastBlock.clear();
    for (size_t i = 0; i < ops.size(); ++i) {
        const UndoOp& op = ops[i];
        ParamRec* r = op.r;
        Anim& k = gAnim[r];
        bool changed = true;
        if (op.keyAdded) {
            // Gui/KnobGui10.cpp: removing the last key gives the param its value at the current time
            if (k.size() == 1) {
                if (isString(r)) {
                    setStatic(r, stringAt(r, gCurrentTime));
                } else {
                    setStatic(r, intAt(r, gCurrentTime));
                }
            }
            // (deleteValueAtTime() reports the change even if there was no key, if the param has keys)
            changed = (k.size() > 0);
            sErase(k, op.time);
            k.i.erase(op.time);
        } else if (op.key) {
            if (isString(r)) {
                const int rc = sSetKey(k, op.time, op.oldS);
                changed = !gNatronExact || (rc != 0);
            } else {
                k.i[op.time] = op.oldI;
            }
        } else {
            if (valuesUndone.count(r)) {
                continue;
            }
            valuesUndone.insert(r);
            if (isString(r)) {
                changed = !gNatronExact || (paramString(r) != op.oldS);
                setStatic(r, op.oldS);
            } else {
                setStatic(r, op.oldI);
            }
        }
        if (changed) {
            instanceChanged(r->name.c_str(), kOfxChangePluginEdited, op.time);
        }
    }
    gRedoBlock = ops;
    u.done();
}

// Redo the undone edit block, as Natron's MultipleKnobEditsUndoCommand::redo() does (not the first redo)
void
userRedo(int maxCalls = 40)
{
    UserAction u("redo", maxCalls);
    std::vector<UndoOp> ops = gRedoBlock;
    gRedoBlock.clear();
    for (size_t i = 0; i < ops.size(); ++i) {
        UndoOp& op = ops[i];
        ParamRec* r = op.r;
        Anim& k = gAnim[r];
        bool changed = true;
        if (op.key) {
            if (isString(r)) {
                const int rc = sSetKey(k, op.time, op.newS);
                op.keyAdded = gNatronExact ? (rc != 0) : (rc == 2);
                changed = (rc != 0);
            } else {
                op.keyAdded = (k.i.count(op.time) == 0);
                changed = op.keyAdded || (k.i[op.time] != op.newI);
                k.i[op.time] = op.newI;
            }
        } else {
            // (Natron compares the value at the time of the first redo)
            if (isString(r)) {
                changed = (stringAt(r, op.time) != op.newS);
                if (changed) {
                    if (k.size() > 0) {
                        sSetKey(k, gCurrentTime, op.newS);
                    } else {
                        setStatic(r, op.newS);
                    }
                }
            } else {
                changed = (intAt(r, op.time) != op.newI);
                if (changed) {
                    setStatic(r, op.newI);
                }
            }
        }
        if (changed) {
            instanceChanged(r->name.c_str(), kOfxChangePluginEdited, op.time);
        }
    }
    gLastBlock = ops;
    u.done();
}

// destroy the instance, if any
void
destroyInstance()
{
    if (gInst) {
        Effect* inst = gInst;
        gInst = nullptr;
        gPlug->p->mainEntry(kOfxActionDestroyInstance, reinterpret_cast<OfxImageEffectHandle>(inst), nullptr, nullptr);
        delete inst;
    }
}

// The params of an instance, as restored by the host before kOfxActionCreateInstance
struct Restored {
    std::map<double, std::string> inputKeys, outputKeys; // the strings' keys
    std::map<double, int> inputChoiceKeys; // e.g. those of a duplicated node
    std::map<double, int> inputIdx, outputIdx; // (gNatronExact) the saved curves of the strings, if not consistent
    std::string configFile;
    std::vector<std::pair<std::string, int>> ints; // (name, value)
    std::vector<std::pair<std::string, std::string>> strings;
    std::function<std::string(double)> inputExpr;
};

// A new instance of the plug-in, as when creating a node (no keys) or loading a project. Checks that
// kOfxActionCreateInstance sets no key.
void
newInstance(Plug& plug,
            const Restored& restored = Restored())
{
    destroyInstance();
    gPlug = &plug;
    for (std::map<std::string, ParamRec>::iterator it = plug.ctx.params.begin(); it != plug.ctx.params.end(); ++it) {
        gAnim[&it->second] = Anim();
        it->second.hasOv = false;
        it->second.ov = Value();
    }
    sAssign(gAnim[P(kIS)], restored.inputKeys, &restored.inputIdx);
    gAnim[P(kIS)].expr = restored.inputExpr;
    gAnim[P(kIC)].i = restored.inputChoiceKeys;
    if (plug.ctx.params.count(kOS)) {
        sAssign(gAnim[P(kOS)], restored.outputKeys, &restored.outputIdx);
    }
    if (!restored.configFile.empty()) {
        setOverrideString(&plug.ctx, kConfigFile, restored.configFile);
    }
    for (size_t i = 0; i < restored.ints.size(); ++i) {
        setOverrideInt(&plug.ctx, restored.ints[i].first.c_str(), restored.ints[i].second);
    }
    for (size_t i = 0; i < restored.strings.size(); ++i) {
        setOverrideString(&plug.ctx, restored.strings[i].first.c_str(), restored.strings[i].second);
    }
    gCurrentTime = 0.;
    gLastBlock.clear();
    gRedoBlock.clear();
    gKeyWritesOutsideInstanceChanged = 0;
    Effect* inst = new Effect();
    inst->delegate = &plug.ctx;
    setInstanceProps(*inst, W, H, 100, plug.context);
    // (no instance changed action during kOfxActionCreateInstance)
    if (!ok(plug.p->mainEntry(kOfxActionCreateInstance, reinterpret_cast<OfxImageEffectHandle>(inst), nullptr, nullptr))) {
        std::printf("FAIL: createInstance of %s\n", plug.id);
        std::exit(1);
    }
    gInst = inst;
    check(gKeyWritesOutsideInstanceChanged == 0, "createInstance sets no key (%d)", gKeyWritesOutsideInstanceChanged);
    check(gAnim[P(kIS)].s == restored.inputKeys, "createInstance doesn't change the keys of the input string");
    if (plug.ctx.params.count(kOS)) {
        check(gAnim[P(kOS)].s == restored.outputKeys, "createInstance doesn't change the keys of the output string");
        check(gAnim[P(kOC)].size() == 0, "createInstance sets no key on the output menu");
    }
    check(gAnim[P(kIC)].i == restored.inputChoiceKeys, "createInstance sets no key on the input menu");
}

// render at time t: the output value of the 0.25 image
double
render(double t)
{
    Clip* src = gPlug->ctx.clips[kOfxImageEffectSimpleSourceClipName];
    Clip* dst = gPlug->ctx.clips[kOfxImageEffectOutputClipName];
    src->connected = true;
    dst->connected = true;
    setupClipImage(src, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    setupClipImage(dst, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    float* p = reinterpret_cast<float*>(src->buffer.data());
    for (int i = 0; i < W * H; ++i) {
        p[4 * i] = p[4 * i + 1] = p[4 * i + 2] = (float)kInput;
        p[4 * i + 3] = 1.f;
    }
    PropSet a = makeRenderInArgs(t, W, H);
    if (!ok(gPlug->p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(gInst), PH(&a), nullptr))) {
        return -1.;
    }

    return reinterpret_cast<float*>(dst->buffer.data())[0];
}

// render at time t from xN to xM
void
checkRender(double t,
            int n,
            int m)
{
    const double expected = kInput * n / m;
    const double got = render(t);
    check(std::fabs(got - expected) < 1e-5, "render at t=%g: %g (expected x%d -> x%d: %g)", t, got, n, m, expected);
}

Plug gColorSpace = { "fr.inria.openfx.OCIOColorSpace", kOfxImageEffectContextFilter };
Plug gWriter = { "fr.inria.openfx.WriteOIIO", kOfxImageEffectContextWriter };
Plug gReader = { "fr.inria.openfx.ReadOIIO", kOfxImageEffectContextReader };
std::string gTmpDir;

// ------------------------------------------------------------------ the tests (menu entries: x1=0 ... x4=3)
void
testKeyingTheMenu()
{
    std::printf("==== Keying the input menu (#128) ====\n");
    newInstance(gColorSpace);
    checkSides("=1", "=1");
    gCurrentTime = 0.;
    userSet(kIC, 1, true); // "Set key" on the menu: x2
    checkSides("{0:2}", "=1");
    gCurrentTime = 10.;
    userSet(kIC, 3); // an edit of a menu that has keys: a key at the current time
    checkSides("{0:2,10:4}", "=1");
    checkRender(0., 2, 1);
    checkRender(5., 2, 1);
    checkRender(10., 4, 1);

    std::printf("==== An edit at a time without a key (t=5) ====\n");
    gCurrentTime = 5.;
    userSet(kIC, 2);
    checkSides("{0:2,5:3,10:4}", "=1");
    checkRender(5., 3, 1);
    checkRender(7., 3, 1);

    std::printf("==== Deleting the key at t=10, at the current time ====\n");
    gCurrentTime = 10.;
    userDeleteKeys(kIC, { 10. });
    checkSides("{0:2,5:3}", "=1");
    checkRender(10., 3, 1);

    std::printf("==== Deleting the key at t=10, at another current time ====\n");
    userSet(kIC, 3);
    checkSides("{0:2,5:3,10:4}", "=1");
    gCurrentTime = 3.;
    userDeleteKeys(kIC, { 10. });
    checkSides("{0:2,5:3}", "=1");

    std::printf("==== Deleting several keys ====\n");
    gCurrentTime = 10.;
    userSet(kIC, 3);
    gCurrentTime = 15.;
    userSet(kIC, 0);
    checkSides("{0:2,5:3,10:4,15:1}", "=1");
    gCurrentTime = 3.;
    userDeleteKeys(kIC, { 5., 10. });
    checkSides("{0:2,15:1}", "=1");
    checkRender(12., 2, 1);

    std::printf("==== Moving a key in the curve editor (reported at the current time) ====\n");
    userMoveKey(kIC, 15., 20.);
    checkSides("{0:2,20:1}", "=1");
    checkRender(17., 2, 1);
    checkRender(20., 1, 1);

    std::printf("==== Moving keys in the dope sheet (reported at their new times) ====\n");
    userDopeSheetMoveKeys(kIC, { 0., 20. }, 2.);
    checkSides("{2:2,22:1}", "=1");
    userDopeSheetMoveKeys(kIC, { 2., 22. }, -2.);
    checkSides("{0:2,20:1}", "=1");

    std::printf("==== Pasting keys (reported at the current time) ====\n");
    userPasteKeys(kIC, { { 30., 2 }, { 40., 3 } });
    checkSides("{0:2,20:1,30:3,40:4}", "=1");

    std::printf("==== Removing the animation of the menu ====\n");
    gCurrentTime = 35.;
    userRemoveAnimation(kIC);
    checkSides("=3", "=1");
    checkRender(0., 3, 1);
    checkRender(40., 3, 1);
    userRemoveAnimation(kIC, 0); // (nothing happens)

    std::printf("==== Restoring the default value of an animated menu ====\n");
    gCurrentTime = 0.;
    userSet(kIC, 1, true);
    gCurrentTime = 10.;
    userSet(kIC, 3);
    checkSides("{0:2,10:4}", "=1");
    userRestoreDefault(kIC);
    checkSides("=1", "=1");
    checkRender(10., 1, 1);

    std::printf("==== An edit of a menu without keys ====\n");
    gCurrentTime = 4.;
    userSet(kOC, 3);
    checkSides("=1", "=4");
    checkRender(0., 1, 4);

    std::printf("==== An edit of the string param ====\n");
    gCurrentTime = 0.;
    userSetString(kIS, "x2", true);
    gCurrentTime = 10.;
    userSetString(kIS, "x3");
    checkSides("{0:2,10:3}", "=4");
    checkRender(10., 3, 4);
}

void
testProjectLoad()
{
    Restored restored;
    restored.inputKeys = { { 0., "x2" }, { 10., "x4" } };

    std::printf("==== After loading a project: the string param has keys, not the menu ====\n");
    newInstance(gColorSpace, restored);
    checkAnim(kIS, "{0:2,10:4}");
    checkAnim(kIC, "=2"); // (the colorspace at time 0, set by the constructor)
    checkRender(10., 4, 1);

    std::printf("==== ... then an edit of the menu at t=5 ====\n");
    gCurrentTime = 5.;
    userSet(kIC, 2);
    checkSides("{0:2,5:3,10:4}", "=1");

    std::printf("==== ... then deleting a key of the menu ====\n");
    userDeleteKeys(kIC, { 5. });
    checkSides("{0:2,10:4}", "=1");

    std::printf("==== After loading a project: an edit of another param, then removing the menu's animation ====\n");
    newInstance(gColorSpace, restored);
    gCurrentTime = 3.;
    userSetString(kKey1, "a");
    checkSides("{0:2,10:4}", "=1");
    // once the menus have the keys of the strings, they are not set again
    userSetString(kKey1, "b", false, 1);
    userRemoveAnimation(kIC);
    checkSides("=2", "=1");
    checkRender(10., 2, 1);

    std::printf("==== After loading a project: Set Key on the menu, at a time where it shows another colorspace ====\n");
    newInstance(gColorSpace, restored);
    gCurrentTime = 12.;
    userSet(kIC, 1, true); // (the menu shows x2, the colorspace at time 0)
    checkSides("{0:2,10:4,12:4}", "=1");
    checkRender(12., 4, 1);
    checkRender(15., 4, 1);

    std::printf("==== After loading a project: Set Key on the menu, then a new colorspace at that time ====\n");
    newInstance(gColorSpace, restored);
    gCurrentTime = 5.;
    userSet(kIC, 1, true);
    checkSides("{0:2,5:2,10:4}", "=1");
    userSet(kIC, 2);
    checkSides("{0:2,5:3,10:4}", "=1");

    std::printf("==== After loading a project: restoring the default value of the menu ====\n");
    newInstance(gColorSpace, restored);
    gCurrentTime = 5.;
    userRestoreDefault(kIC);
    // (the menu had no keys: this is an edit of the menu at t=5)
    checkSides("{0:2,5:1,10:4}", "=1");

    std::printf("==== After loading a project: Swap Colorspaces ====\n");
    newInstance(gColorSpace, restored);
    gCurrentTime = 5.;
    userPress(kSwap);
    checkSides("{0:1,10:1}", "{0:2,10:4}");
    checkRender(0., 1, 2);
    checkRender(10., 1, 4);

    std::printf("==== After loading a project: a string key with the canonical name of its colorspace ====\n");
    Restored raw;
    raw.inputKeys = { { 0., "x1" }, { 10., "x2" } }; // (the canonical name of x1 is scene_linear)
    newInstance(gColorSpace, raw);
    gCurrentTime = 3.;
    userSetString(kKey1, "a");
    // deleting the key at 10, reported at that time, where the colorspace is x1: it is not canonicalized
    userDeleteKeys(kIS, { 10. });
    checkSides("{0:1}", "=1");
}

void
testSwap()
{
    std::printf("==== Swap with only the input keyed ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 1, true);
    gCurrentTime = 10.;
    userSet(kIC, 3);
    gCurrentTime = 5.;
    userPress(kSwap);
    // (no key is deleted: the input keeps keys, with the output's colorspace)
    checkSides("{0:1,10:1}", "{0:2,10:4}");
    checkRender(0., 1, 2);
    checkRender(10., 1, 4);

    std::printf("==== ... undo ====\n");
    userUndo();
    // the input's keys are restored; the output, which had no keys, gets its value at the current time when its last
    // key is removed (x4, the input's colorspace at t=5 before the swap: x1 would be right)
    checkSides("{0:2,10:4}", "=4");

    std::printf("==== Swap again ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 1, true);
    gCurrentTime = 10.;
    userSet(kIC, 3);
    gCurrentTime = 5.;
    userPress(kSwap);
    userPress(kSwap);
    checkSides("{0:2,10:4}", "{0:1,10:1}");
    checkRender(0., 2, 1);
    checkRender(10., 4, 1);

    std::printf("==== Swap with both sides keyed, at t=8 ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 1, true);
    gCurrentTime = 10.;
    userSet(kIC, 3);
    gCurrentTime = 2.;
    userSet(kOC, 2, true);
    gCurrentTime = 8.;
    userSet(kOC, 0);
    checkSides("{0:2,10:4}", "{2:3,8:1}");
    userPress(kSwap);
    // both sides get keys at the times of both sides' keys
    checkSides("{0:3,2:3,8:1,10:1}", "{0:2,2:2,8:2,10:4}");
    checkRender(0., 3, 2);
    checkRender(5., 3, 2);
    checkRender(8., 1, 2);
    checkRender(10., 1, 4);

    std::printf("==== ... undo ====\n");
    userUndo();
    checkSides("{0:2,10:4}", "{2:3,8:1}");
    checkRender(0., 2, 3);
    checkRender(9., 2, 1);
    checkRender(10., 4, 1);

    std::printf("==== Swap with only the output keyed, at t=0, then undo ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 1);
    gCurrentTime = 5.;
    userSet(kOC, 2, true);
    checkSides("=2", "{5:3}");
    gCurrentTime = 0.;
    userPress(kSwap);
    checkSides("{5:3}", "{5:2}");
    userUndo();
    // the output's key is restored; the input gets its value at the current time when its last key is removed (x3:
    // x2 would be right)
    checkSides("=3", "{5:3}");

    std::printf("==== Swap with neither side keyed, then undo ====\n");
    newInstance(gColorSpace);
    userSet(kIC, 1);
    userSet(kOC, 2);
    checkSides("=2", "=3");
    userPress(kSwap);
    checkSides("=3", "=2");
    checkRender(0., 3, 2);
    userUndo();
    checkSides("=2", "=3");
}

void
testExpression()
{
    std::printf("==== A string param with an expression (Natron reports keys at 0 and 1) ====\n");
    Restored restored;
    restored.inputKeys = { { 0., "x2" }, { 10., "x4" } }; // (its real keys, which the plug-in doesn't see)
    restored.inputExpr = [](double t) { return std::string(t < 5. ? "x2" : "x3"); };
    newInstance(gColorSpace, restored);
    gCurrentTime = 3.;
    userSetString(kKey1, "a");
    checkAnim(kIS, "{0:2,10:4}");
    checkAnim(kIC, "=2"); // (not keyed with the string)
    gCurrentTime = 7.;
    userSet(kIC, 0);
    checkAnim(kIS, "{0:2,10:4}"); // (no key under the expression)
    checkAnim(kIC, "=3"); // the expression's value at t=7
    checkRender(7., 3, 1);
    userPress(kSwap);
    checkAnim(kIS, "{0:2,10:4}"); // (only the values at t=7 are swapped, and the side with an expression is not set)
    checkAnim(kOS, "=3"); // the expression's value at t=7

    std::printf("==== A menu with an expression, e.g. that of Link to (reported as a user edit) ====\n");
    restored = Restored();
    restored.inputKeys = { { 0., "x2" }, { 10., "x4" } };
    newInstance(gColorSpace, restored);
    gCurrentTime = 3.;
    userSetString(kKey1, "a");
    checkSides("{0:2,10:4}", "=1");
    gAnim[P(kIC)].iexpr = [](double t) { return t < 5. ? 0 : 2; };
    {
        UserAction u("expression of the input menu at t=3", 12);
        instanceChanged(kIC, kOfxChangeUserEdited, gCurrentTime);
        u.done();
    }
    // the string gets the menu's colorspace at that time only, and keeps its keys
    checkAnim(kIS, "{0:2,3:1,10:4}");
    gAnim[P(kIC)].iexpr = nullptr;

    std::printf("==== After loading a project: a menu edit, the other string has an expression ====\n");
    restored = Restored();
    restored.inputExpr = [](double t) { return std::string(t < 5. ? "x2" : "x3"); };
    restored.outputKeys = { { 0., "x2" }, { 10., "x4" } };
    newInstance(gColorSpace, restored);
    gCurrentTime = 3.;
    userSet(kIC, 0);
    checkAnim(kOC, "{0:2,10:4}"); // (the other menu has the keys of its string)
}

void
testFractionalKeyTimes()
{
    std::printf("==== Keys at fractional times (e.g. from a script): Natron reads strings at floor(t + 0.5) ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSetString(kIS, "x4", true);
    userPasteKeys(kIC, { { 10.3, 2 } });
    checkSides("{0:4,10.3:3}", "=1");
    checkRender(15., 3, 1);
    checkRender(10., 4, 1);

    std::printf("==== Keys at fractional times, then Swap ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 1, true);
    gCurrentTime = 10.;
    userSet(kIC, 3);
    userPasteKeys(kIC, { { 2.3, 2 } });
    gCurrentTime = 5.;
    userSet(kIC, 0);
    checkSides("{0:2,2.3:3,5:1,10:4}", "=1");
    userPress(kSwap);
    checkSides("{0:1,2.3:1,5:1,10:1}", "{0:2,2.3:3,5:1,10:4}");
    checkRender(2., 1, 2);
    checkRender(3., 1, 3);
    checkRender(10., 1, 4);
}

void
testNatronStringKeys()
{
    std::printf("==== Natron without NatronGitHub/Natron#1291: string keys hold the index of their string ====\n");
    gNatronExact = true;
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 0, true);
    gCurrentTime = 10.;
    userSet(kIC, 1);
    gCurrentTime = 20.;
    userSet(kIC, 1);
    checkSides("{0:1,10:2,20:2}", "=1");
    // deleting the first key: the later keys must be set again, even the one that reads the right string
    userDeleteKeys(kIC, { 0. });
    checkSides("{10:2,20:2}", "=1");
    userSet(kIC, 2);
    checkSides("{10:2,20:3}", "=1");
    checkRender(15., 2, 1);

    std::printf("==== ... a menu edit between keys, after loading a project ====\n");
    Restored restored;
    restored.inputKeys = { { 0., "x2" }, { 10., "x4" } };
    newInstance(gColorSpace, restored);
    gCurrentTime = 5.;
    userSet(kIC, 2);
    checkSides("{0:2,5:3,10:4}", "=1");
    checkRender(10., 4, 1);
    gNatronExact = false;
}

void
testKeysAt0And1()
{
    std::printf("==== Real keys at times 0 and 1 (not an expression), then removing the animation ====\n");
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 3, true);
    gCurrentTime = 1.;
    userSet(kIC, 0);
    checkSides("{0:4,1:1}", "=1");
    userRemoveAnimation(kIC);
    checkSides("=1", "=1");
    checkRender(0., 1, 1);

    std::printf("==== Real keys at times 0 and 1: Swap ====\n");
    Restored restored;
    restored.inputKeys = { { 0., "x4" }, { 1., "x1" } };
    restored.outputKeys = { { 5., "x2" } };
    newInstance(gColorSpace, restored);
    gCurrentTime = 3.;
    userPress(kSwap);
    checkSides("{0:2,1:2,5:2}", "{0:4,1:1,5:1}");
    checkRender(0., 2, 4);
    checkRender(3., 2, 1);
}

void
testHostWithoutStringAnimation()
{
    std::printf("==== A host that animates choices, not strings (e.g. hostDemo) ====\n");
    gStringAnimation = false;
    OFX::getImageEffectHostDescription()->supportsStringAnimation = false;
    newInstance(gColorSpace);
    gCurrentTime = 0.;
    userSet(kIC, 1, true);
    gCurrentTime = 1.;
    userSet(kIC, 3);
    // the string param gets the colorspace of the last edit of the menu
    checkAnim(kIC, "{0:2,1:4}");
    checkAnim(kIS, "=4");
    gCurrentTime = 0.;
    userPress(kSwap);
    checkAnim(kIC, "=1");
    checkAnim(kOC, "{0:2,1:4}");
    checkAnim(kIS, "=1");
    checkAnim(kOS, "=2"); // the input menu's colorspace at the time of the swap, not that of the last edit
    checkRender(0., 1, 2);
    checkRender(1., 1, 2);
    gCurrentTime = 1.;
    userPress(kSwap);
    checkAnim(kIC, "{0:2,1:4}");
    checkAnim(kOC, "=1");
    checkAnim(kIS, "=4");
    checkAnim(kOS, "=1");
    gStringAnimation = true;
    OFX::getImageEffectHostDescription()->supportsStringAnimation = true;
}

void
testHostWithoutChoiceAnimation()
{
    std::printf("==== A host that animates strings, not choices ====\n");
    gChoiceAnimation = false;
    OFX::getImageEffectHostDescription()->supportsChoiceAnimation = false;
    Restored restored;
    restored.inputKeys = { { 0., "x2" }, { 10., "x4" } };
    newInstance(gColorSpace, restored);
    gCurrentTime = 5.;
    userSet(kIC, 2);
    // the string param gets a key at the time of the edit
    checkAnim(kIS, "{0:2,5:3,10:4}");
    checkAnim(kIC, "=3");
    gCurrentTime = 7.;
    userPress(kSwap);
    checkAnim(kIS, "{0:1,5:1,10:1}");
    checkAnim(kOS, "{0:2,5:3,10:4}");
    checkAnim(kIC, "=1");
    checkAnim(kOC, "=3");
    checkRender(0., 1, 2);
    checkRender(10., 1, 4);
    gChoiceAnimation = true;
    OFX::getImageEffectHostDescription()->supportsChoiceAnimation = true;
}

void
testNoConfig()
{
    std::printf("==== Without a config (no menus) ====\n");
    Restored restored;
    restored.inputKeys = { { 0., "x2" }, { 10., "x4" } };
    restored.configFile = gTmpDir + "/no such config.ocio";
    newInstance(gColorSpace, restored);
    gCurrentTime = 5.;
    userPress(kSwap);
    checkAnim(kIS, "{0:1,10:1}");
    checkAnim(kOS, "{0:2,10:4}");
    check(gAnim[P(kIC)].size() == 0 && gAnim[P(kOC)].size() == 0, "the menus have no keys");
}

void
testWriter()
{
    std::printf("==== WriteOIIO: loading a project with keyed colorspaces ====\n");
    Restored restored;
    restored.inputKeys = { { 0., "x3" }, { 10., "x1" } };
    restored.outputKeys = { { 0., "x2" }, { 10., "x4" } };
    restored.strings = { { kFilename, gTmpDir + "/out.exr" } };
    restored.ints = { { kOutputSpaceSet, 1 }, { kGuessedParams, 1 } };
    newInstance(gWriter, restored);
    checkSides("{0:3,10:1}", "{0:2,10:4}", false);

    std::printf("==== WriteOIIO: loading a project with a keyed output colorspace, whose file name has a colorspace ====\n");
    restored.strings = { { kFilename, gTmpDir + "/out_x3.exr" } };
    restored.ints = { { kGuessedParams, 1 } };
    newInstance(gWriter, restored);
    checkSides("{0:3,10:1}", "{0:2,10:4}", false);

    std::printf("==== WriteOIIO: a new file name with a colorspace, on a keyed output colorspace ====\n");
    gCurrentTime = 7.;
    userSetString(kFilename, gTmpDir + "/out_x1.exr");
    checkSides("{0:3,10:1}", "{0:2,10:4}");

    std::printf("==== WriteOIIO: loading a project, the colorspace guessed by createInstance, then a menu edit ====\n");
    restored.strings = { { kFilename, gTmpDir + "/out_x3.exr" } };
    restored.ints = { { kGuessedParams, 1 } };
    restored.outputKeys.clear();
    newInstance(gWriter, restored);
    checkAnim(kOS, "=3");
    checkAnim(kOC, "=3");
    gCurrentTime = 4.;
    userSet(kOC, 0); // (x1, the menu's value before the guess)
    checkAnim(kOS, "=1");
    checkAnim(kOC, "=1");

    std::printf("==== WriteOIIO: a new file name with a colorspace, on an output colorspace without keys ====\n");
    restored.outputKeys.clear();
    newInstance(gWriter, restored);
    checkAnim(kOS, "=3"); // (guessed by createInstance)
    userSetString(kFilename, gTmpDir + "/out_x2.exr");
    checkAnim(kOS, "=2");
}

void
testReader()
{
    std::printf("==== ReadOIIO: loading a project with a keyed colorspace ====\n");
    Restored restored;
    restored.inputKeys = { { 0., "x3" }, { 10., "x1" } };
    restored.ints = { { kGuessedParams, 1 } };
    newInstance(gReader, restored);
    checkAnim(kIS, "{0:3,10:1}");

    std::printf("==== ReadOIIO: the plug-in deletes a key of a menu (e.g. that of a duplicated node) ====\n");
    restored.inputChoiceKeys = { { 0., 2 }, { 5., 1 }, { 10., 0 } };
    newInstance(gReader, restored);
    gCurrentTime = 3.;
    userSetString(kKey1, "a");
    checkAnim(kIS, "{0:3,10:1}");
    checkAnim(kIC, "{0:3,10:1}");
    check(paramInt(P(kInputSpaceSet), 0) == 0, "%s is not set by the deletion of a key by the plug-in", kInputSpaceSet);
    gCurrentTime = 10.;
    userSet(kIC, 1);
    check(paramInt(P(kInputSpaceSet), 0) == 1, "%s is set by a user edit of the menu", kInputSpaceSet);
}

void
load(Plug& plug)
{
    plug.p = findPlugin(plug.id);
    if (!plug.p) {
        std::printf("FAIL: plugin %s not found\n", plug.id);
        std::exit(1);
    }
    if (loadAndDescribe(plug.p, plug.describe, plug.ctx, plug.context) != 0) {
        std::printf("FAIL: load/describe %s\n", plug.id);
        std::exit(1);
    }
}

} // namespace

int
main(int, char**)
{
    const char* ocio = std::getenv("OCIO");
    if (!ocio) {
        std::printf("FAIL: $OCIO is not set (see tests/Makefile)\n");

        return 1;
    }
    gNatronExact = std::getenv("OCIO_ANIM_NATRON_EXACT") != nullptr;
    gRoundStringTime = std::getenv("OCIO_ANIM_NO_ROUND") == nullptr;
    std::printf("Natron-exact string keys: %s, string times rounded: %s\n", gNatronExact ? "yes" : "no", gRoundStringTime ? "yes" : "no");
    initHost();
    // Natron (e.g. GenericOCIO builds the menus of each instance from its config)
    pSetInt(PH(&gHostProps), kNatronOfxHostIsNatron, 0, 1);
    installAnimatedParamSuite();
    load(gColorSpace);
    load(gWriter);
    load(gReader);

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) {
        dir = ".";
    }
    dir /= "ocio_colorspace_anim_test." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    if (!std::filesystem::create_directory(dir, ec) || ec) {
        std::printf("FAIL: cannot create directory %s\n", dir.string().c_str());

        return 1;
    }
    gTmpDir = dir.generic_string();

    testKeyingTheMenu();
    testProjectLoad();
    testSwap();
    testExpression();
    testKeysAt0And1();
    testNatronStringKeys();
    testFractionalKeyTimes();
    testHostWithoutStringAnimation();
    testHostWithoutChoiceAnimation();
    testNoConfig();
    testWriter();
    testReader();

    destroyInstance();
    gColorSpace.p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    gWriter.p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    gReader.p->mainEntry(kOfxActionUnload, nullptr, nullptr, nullptr);
    std::filesystem::remove_all(dir, ec);

    std::printf("\n%s (%d failure%s)\n", gFailures ? "FAILED" : "ALL PASSED", gFailures, gFailures == 1 ? "" : "s");

    return gFailures ? 1 : 0;
}
