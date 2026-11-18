// Random-operation fuzzer for the animated colorspaces of GenericOCIO (PR #155), on top of the Natron-like host of
// ocio_colorspace_anim_test.cpp (included, its main renamed).
// Usage: ocio_anim_fuzz <plugin: cs|writer|reader> <ops> <seed>   ($OCIO=swap.ocio; OCIO_ANIM_NATRON_EXACT=1 for
// Natron's index-valued string keys)
#define main ocio_anim_test_main
#include "ocio_colorspace_anim_test.cpp"
#undef main

#include <limits>
#include <random>

namespace {

enum OpType {
    oMenuSet, // user edit of a menu at the current time (key if keyed, or "set key" if op.key)
    oMenuSetKey, // Set Key on the menu (current displayed value)
    oStrSet, // user edit of a string at the current time
    oMenuDel, // curve-editor deletion of menu keys
    oStrDel, // curve-editor deletion of string keys
    oMenuMove, // curve-editor move of a menu key
    oMenuDope, // dope-sheet move of menu keys
    oMenuPaste,
    oRemoveAnim, // on menu (side.v=0) or string (v=1)
    oRestoreDefault, // menu (v=0) or string (v=1)
    oSwap,
    oUndo,
    oRedo,
    oSaveLoad,
    oFilename,
    oExprSet,
    oExprClear,
    oTime,
    oKey1,
    oModifyAtKey, // user edit of a menu at the current time, only if it has a key there (does not add keys)
    oNumOps
};
const char* kOpNames[] = { "menuSet", "menuSetKey", "strSet", "menuDel", "strDel", "menuMove", "menuDope", "menuPaste",
                           "removeAnim", "restoreDefault", "swap", "undo", "redo", "saveLoad", "filename", "exprSet",
                           "exprClear", "time", "key1", "modifyAtKey" };

struct Op {
    int type = 0;
    int side = 0; // 0 input, 1 output
    int v = 0;
    bool key = false;
    double t = 0.;
    double dt = 0.;
    std::vector<double> times;
    std::map<double, int> keys;
    std::string s;
    bool noKey = false; // generated in a phase of ops that add no keys (invariant f)
};

const char* const kNames[] = { "x1", "x2", "x3", "x4", "scene_linear", "reference", "bogus" };

struct Fuzz {
    Plug* plug = nullptr;
    std::string kind;
    bool hasOut = false;
    bool hasSwap = false;
    bool hasFile = false;
    std::mt19937 rng;
    // model-side expectations
    bool synced = true;
    bool swapClean = false; // only swaps (and time changes) since the snapshot
    bool snapBothKeyed = false;
    std::string snapBefore, snapAfter;
    bool undoClean = false; // only undo since the post-swap snapshot
    int failuresSeen = 0;
    std::vector<std::string> failMsgs;
    std::map<std::string, int> maxCallsByOp;
    bool noExpr = false;

    int R(int n) { return (int)(rng() % (unsigned)n); }
    double randTime()
    {
        int c = R(20);
        if (c == 0) {
            return -5.;
        }
        if (c == 1) {
            return 30 + R(5); // (Natron's GUI clamps key times and the current time to integers)
        }
        if (c == 2) {
            return R(2); // 0 or 1, the expression fake key times
        }
        return R(21);
    }
    const char* S(int side) { return side ? kOS : kIS; }
    const char* C(int side) { return side ? kOC : kIC; }
    int nSides() { return hasOut ? 2 : 1; }
};
Fuzz F;
// (before a swap) the input and output strings have keys less than 1e-6 apart, or one of them has
bool gSwapNearKeys = false;
// (before an op) the number of keys hidden from the plug-in (less than 1e-6 after another one), which it may see after
// the op (e.g. once the other one is moved), and mirror
int gHiddenKeysBefore = 0;
int
hiddenKeys()
{
    int n = 0;
    for (const char* name : { kIS, kIC, kOS, kOC }) {
        if (F.plug->ctx.params.count(name)) {
            n += (int)(allKeyTimes(P(name)).size() - keyTimes(P(name)).size());
        }
    }

    return n;
}
int gKeysBeforeOp = 0; // (the number of keys before the op)

std::vector<double>
menuKeys(int side)
{
    std::vector<double> v;
    for (auto& p : gAnim[P(F.C(side))].i) {
        v.push_back(p.first);
    }
    return v;
}
std::vector<double>
strKeys(int side)
{
    std::vector<double> v;
    for (auto& p : gAnim[P(F.S(side))].s) {
        v.push_back(p.first);
    }
    return v;
}

// a description of the strings (effective values), for snapshots
std::string
stringsState()
{
    std::string r = anim(kIS);
    if (F.hasOut) {
        r += " / " + anim(kOS);
    }
    return r;
}
std::string
fullState()
{
    std::string r = "IS=" + anim(kIS) + " IC=" + anim(kIC);
    if (F.hasOut) {
        r += " OS=" + anim(kOS) + " OC=" + anim(kOC);
    }
    if (gAnim[P(kIS)].expr) {
        r += " [IS has expr]";
    }
    if (F.hasOut && gAnim[P(kOS)].expr) {
        r += " [OS has expr]";
    }
    char b[64];
    std::snprintf(b, sizeof(b), " t=%g", gCurrentTime);
    return r + b;
}

std::string
describe(const Op& o)
{
    std::ostringstream os;
    os << kOpNames[o.type];
    switch (o.type) {
    case oMenuSet:
        os << "(" << F.C(o.side) << "=" << o.v << (o.key ? ",setkey" : "") << ")";
        break;
    case oMenuSetKey:
    case oModifyAtKey:
        os << "(" << F.C(o.side) << (o.type == oModifyAtKey ? (std::string("=") + std::to_string(o.v)) : "") << ")";
        break;
    case oStrSet:
        os << "(" << F.S(o.side) << "=\"" << o.s << "\"" << (o.key ? ",setkey" : "") << ")";
        break;
    case oMenuDel:
    case oStrDel:
    case oMenuDope:
        os << "(" << (o.type == oStrDel ? F.S(o.side) : F.C(o.side)) << ",{";
        for (size_t i = 0; i < o.times.size(); ++i) {
            os << (i ? "," : "") << o.times[i];
        }
        os << "}";
        if (o.type == oMenuDope) {
            os << ",dt=" << o.dt;
        }
        os << ")";
        break;
    case oMenuMove:
        os << "(" << F.C(o.side) << "," << o.t << "->" << o.t + o.dt << ")";
        break;
    case oMenuPaste:
        os << "(" << F.C(o.side) << ",{";
        for (auto it = o.keys.begin(); it != o.keys.end(); ++it) {
            os << (it == o.keys.begin() ? "" : ",") << it->first << ":" << it->second;
        }
        os << "})";
        break;
    case oRemoveAnim:
    case oRestoreDefault:
        os << "(" << (o.v ? F.S(o.side) : F.C(o.side)) << ")";
        break;
    case oFilename:
        os << "(" << o.s << ")";
        break;
    case oExprSet:
        os << "(" << F.S(o.side) << ",kind=" << o.v << ",split=" << o.t << ")";
        break;
    case oExprClear:
        os << "(" << F.S(o.side) << ")";
        break;
    case oTime:
        os << "(" << o.t << ")";
        break;
    case oKey1:
        os << "(\"" << o.s << "\")";
        break;
    default:
        break;
    }
    return os.str();
}

std::function<std::string(double)>
makeExpr(int kind,
         double split)
{
    switch (kind) {
    case 0:
        return [split](double t) { return std::string(t < split ? "x2" : "x3"); };
    case 1:
        return [](double t) { return std::string(((int)std::floor(t)) % 2 ? "x4" : "x1"); };
    case 2:
        return [](double) { return std::string("x2"); };
    default:
        return [split](double t) { return std::string(t < split ? "x3" : (t < split + 5 ? "x1" : "x4")); };
    }
}

void
userRestoreDefaultString(const char* name)
{
    UserAction u(std::string("restore default of ") + name + at(), 60);
    ParamRec* r = P(name);
    if (gAnim[r].size() > 0) {
        removeAnimation(r);
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    const char* d = "";
    pGetString(PH(r->props), kOfxParamPropDefault, 0, &d);
    const std::string def = d;
    const bool changed = paramString(r) != def;
    setStatic(r, def);
    if (changed) {
        instanceChanged(name, kOfxChangeUserEdited, gCurrentTime);
    }
    u.done();
}

void
apply(const Op& o)
{
    const int maxc = 200;
    switch (o.type) {
    case oMenuSet:
        userSet(F.C(o.side), o.v, o.key, maxc);
        break;
    case oMenuSetKey:
        userSet(F.C(o.side), intAt(P(F.C(o.side)), gCurrentTime), true, maxc);
        break;
    case oModifyAtKey:
        if (gAnim[P(F.C(o.side))].i.count(gCurrentTime)) {
            userSet(F.C(o.side), o.v, false, maxc);
        }
        break;
    case oStrSet:
        userSetString(F.S(o.side), o.s, o.key, maxc);
        userCommandPushed();
        break;
    case oMenuDel:
        userDeleteKeys(F.C(o.side), o.times, maxc);
        break;
    case oStrDel:
        userDeleteKeys(F.S(o.side), o.times, maxc);
        break;
    case oMenuMove: {
        Anim& k = gAnim[P(F.C(o.side))];
        if (k.i.count(o.t) && !k.i.count(o.t + o.dt)) {
            userMoveKey(F.C(o.side), o.t, o.t + o.dt, maxc);
        }
        break;
    }
    case oMenuDope: {
        Anim& k = gAnim[P(F.C(o.side))];
        std::set<double> moved(o.times.begin(), o.times.end());
        bool okMove = true;
        for (double t : o.times) {
            if (!k.i.count(t)) {
                okMove = false;
            }
            if (k.i.count(t + o.dt) && !moved.count(t + o.dt)) {
                okMove = false;
            }
        }
        if (okMove && !o.times.empty()) {
            userDopeSheetMoveKeys(F.C(o.side), o.times, o.dt, maxc);
        }
        break;
    }
    case oMenuPaste:
        userPasteKeys(F.C(o.side), o.keys, maxc);
        break;
    case oRemoveAnim:
        userRemoveAnimation(o.v ? F.S(o.side) : F.C(o.side), maxc);
        if (o.v) {
            userCommandPushed();
        }
        break;
    case oRestoreDefault:
        if (o.v) {
            userRestoreDefaultString(F.S(o.side));
            userCommandPushed();
        } else {
            userRestoreDefault(F.C(o.side), maxc);
        }
        break;
    case oSwap: {
        if (!F.hasSwap) {
            break;
        }
        // Natron merges consecutive MultipleKnobEditsUndoCommand (Gui/KnobUndoCommand.cpp mergeWith()) if the
        // first knob of the new one is in the last one: see aEditEnd2
        userPress(kSwap, maxc);
        break;
    }
    case oUndo:
        if (!gLastBlock.empty()) {
            userUndo(maxc);
        }
        break;
    case oRedo:
        if (!gRedoBlock.empty()) {
            userRedo(maxc);
        }
        break;
    case oSaveLoad: {
        Restored r;
        Anim& ki = gAnim[P(kIS)];
        r.inputKeys = ki.s;
        if (gNatronExact) {
            r.inputIdx = ki.sidx;
        }
        r.inputExpr = ki.expr;
        if (F.hasOut) {
            Anim& ko = gAnim[P(kOS)];
            r.outputKeys = ko.s;
            if (gNatronExact) {
                r.outputIdx = ko.sidx;
            }
        }
        // the saved values of the other params (strings, ints), from the overrides
        for (auto& kv : F.plug->ctx.params) {
            ParamRec& pr = kv.second;
            if (!pr.hasOv || kv.first == kIC || kv.first == kOC) {
                continue;
            }
            if (pr.type == kOfxParamTypeString) {
                r.strings.push_back({ kv.first, paramString(&pr) });
            } else if (pr.type == kOfxParamTypeInteger || pr.type == kOfxParamTypeBoolean || pr.type == kOfxParamTypeChoice) {
                r.ints.push_back({ kv.first, paramInt(&pr, 0) });
            }
        }
        const double t = gCurrentTime;
        const bool outExpr = F.hasOut && gAnim[P(kOS)].expr;
        std::function<std::string(double)> oe = outExpr ? gAnim[P(kOS)].expr : nullptr;
        gQuiet = true;
        newInstance(*F.plug, r);
        if (oe) {
            gAnim[P(kOS)].expr = oe;
        }
        gCurrentTime = t;
        F.synced = (gAnim[P(kIS)].s.empty() && (!F.hasOut || gAnim[P(kOS)].s.empty()));
        break;
    }
    case oFilename:
        if (F.hasFile) {
            userSetString(kFilename, o.s, false, maxc);
            userCommandPushed();
        }
        break;
    // (setting or clearing an expression changes the param's value, which Natron reports, as NatronInternalEdited,
    // i.e. kOfxChangeUserEdited)
    case oExprSet: {
        UserAction u("expression set", maxc);
        gAnim[P(F.S(o.side))].expr = makeExpr(o.v, o.t);
        instanceChanged(F.S(o.side), kOfxChangeUserEdited, gCurrentTime);
        u.done();
        break;
    }
    case oExprClear: {
        UserAction u("expression cleared", maxc);
        gAnim[P(F.S(o.side))].expr = nullptr;
        instanceChanged(F.S(o.side), kOfxChangeUserEdited, gCurrentTime);
        u.done();
        break;
    }
    case oTime: {
        gCurrentTime = o.t;
        if (F.kind == "reader") {
            // (Engine/KnobFile.h: the file param of a reader evaluates on time change)
            UserAction u("time change", maxc);
            instanceChanged(kFilename, kOfxChangeTime, gCurrentTime);
            u.done();
        }
        break;
    }
    case oKey1:
        userSetString(kKey1, o.s, false, maxc);
        userCommandPushed();
        break;
    default:
        break;
    }
}

Op
generate(bool noKeyAdding)
{
    Op o;
    for (;;) {
        o = Op();
        o.type = F.R(oNumOps);
        o.side = F.R(F.nSides());
        if (noKeyAdding) {
            static const int allowed[] = { oMenuDel, oStrDel, oMenuMove, oMenuDope, oRemoveAnim, oRestoreDefault, oSaveLoad,
                                           oFilename, oExprSet, oExprClear, oTime, oKey1, oModifyAtKey };
            o.type = allowed[F.R(sizeof(allowed) / sizeof(allowed[0]))];
            if (o.type == oRestoreDefault && std::getenv("FUZZ_F_NO_RESTORE")) {
                continue;
            }
        }
        // weights: make swap/undo/time more frequent
        int extra = F.R(10);
        if (!noKeyAdding && extra == 0) {
            o.type = oSwap;
        } else if (!noKeyAdding && extra == 1) {
            o.type = F.R(2) ? oUndo : oRedo;
        } else if (extra == 2) {
            o.type = oTime;
        }
        if ((o.type == oSwap) && !F.hasSwap) {
            continue;
        }
        if ((o.type == oFilename) && !F.hasFile) {
            continue;
        }
        if ((o.type == oSaveLoad || o.type == oExprSet) && F.R(3)) {
            continue; // rarer
        }
        if (F.noExpr && o.type == oExprSet) {
            continue;
        }
        break;
    }
    switch (o.type) {
    case oMenuSet:
        o.v = F.R(4);
        o.key = F.R(3) == 0;
        break;
    case oModifyAtKey:
        o.v = F.R(4);
        break;
    case oStrSet:
        o.s = kNames[F.R(F.R(4) ? 4 : 7)];
        o.key = F.R(3) == 0;
        break;
    case oMenuDel:
    case oStrDel:
    case oMenuDope: {
        std::vector<double> ks = (o.type == oStrDel) ? strKeys(o.side) : menuKeys(o.side);
        for (double t : ks) {
            if (F.R(2)) {
                o.times.push_back(t);
            }
        }
        if (o.times.empty() && !ks.empty()) {
            o.times.push_back(ks[F.R((int)ks.size())]);
        }
        o.dt = (F.R(2) ? 1 : -1) * (1 + F.R(4));
        break;
    }
    case oMenuMove: {
        std::vector<double> ks = menuKeys(o.side);
        o.t = ks.empty() ? 0. : ks[F.R((int)ks.size())];
        o.dt = (F.R(2) ? 1 : -1) * (1 + F.R(6));
        break;
    }
    case oMenuPaste: {
        int n = 1 + F.R(3);
        for (int i = 0; i < n; ++i) {
            // (sometimes at a fractional time, as from a script; with FUZZ_NEAR, often less than 1e-5 from a frame)
            static const double nearOffsets[] = { 1e-7, -1e-7, 5e-7, 9.99e-7, 1e-6, 1.001e-6, 1e-5 };
            const double frac = (std::getenv("FUZZ_NEAR") && F.R(2)) ? nearOffsets[F.R(7)] : (F.R(4) ? 0. : 0.3);
            o.keys[F.randTime() + frac] = F.R(4);
        }
        break;
    }
    case oRemoveAnim:
    case oRestoreDefault:
        o.v = F.R(2);
        if (noKeyAdding && !F.synced) {
            // (a documented limitation: until the menus have the strings' keys, Restore default on a menu without
            // keys is an edit, which sets a key on its string if it has keys)
            o.v = 1;
        }
        break;
    case oFilename: {
        static const char* fn[] = { "out.exr", "out_x1.exr", "out_x2.exr", "out_x3.exr", "out_x4.exr" };
        o.s = gTmpDir + "/" + fn[F.R(5)];
        break;
    }
    case oExprSet:
        o.v = F.R(2) ? 0 : (F.R(2) ? 1 : 3); // (not a constant expression: it is not detected, a documented limitation)
        o.t = 2 + F.R(11);
        break;
    case oTime:
        o.t = F.randTime();
        if (std::getenv("FUZZ_NEAR") && F.R(2)) {
            static const double nearOffsets[] = { 1e-7, -1e-7, 5e-7, 9.99e-7, 1e-6, 1.001e-6, 1e-5 };
            o.t += nearOffsets[F.R(7)];
        }
        break;
    case oKey1:
        o.s = std::string("k") + std::to_string(F.R(1000));
        break;
    default:
        break;
    }
    return o;
}

// the keys of the strings (the menus get theirs, see invariant a)
int
totalKeys()
{
    int n = (int)gAnim[P(kIS)].s.size();
    if (F.hasOut) {
        n += (int)gAnim[P(kOS)].s.size();
    }
    return n;
}

// a canonical entry: x1..x4 -> 0..3, roles -> 0, else -1
int
entryOf(const std::string& s)
{
    int n = scaleOf(s);
    return n ? n - 1 : -1;
}

struct Episode {
    Restored start;
    std::vector<Op> ops;
};

std::string gFailWhat; // the first failure of the current replay

void
fail(const std::string& what)
{
    if (gFailWhat.empty()) {
        gFailWhat = what;
    }
}

long gRenderChecks = 0;

// render the reader at time t: the first output value, or -1
double
renderReader(double t)
{
    Clip* out = gPlug->ctx.clips[kOfxImageEffectOutputClipName];
    out->connected = true;
    setupClipImage(out, W, H, kOfxImageComponentRGBA, kOfxBitDepthFloat, true);
    PropSet outArgs;
    if (!ok(gPlug->p->mainEntry(kOfxImageEffectActionGetClipPreferences, reinterpret_cast<OfxImageEffectHandle>(gInst), nullptr, PH(&outArgs)))) {
        return -2.;
    }
    float* fp = reinterpret_cast<float*>(out->buffer.data());
    fp[0] = -3.f;
    PropSet a = makeRenderInArgs(t, W, H);
    if (!ok(gPlug->p->mainEntry(kOfxImageEffectActionRender, reinterpret_cast<OfxImageEffectHandle>(gInst), PH(&a), nullptr))) {
        return -1.;
    }
    return fp[0];
}

// the invariants after an op
void
checkInvariants(const Op& o,
                bool noKeyPhase,
                int keysAtPhaseStart)
{
    // (b)
    if (gKeyWritesOutsideInstanceChanged) {
        fail("b: key written outside an instance changed action");
    }
    // (g) the plug-in adds no key less than 1e-6 from a key of the param that was reported to it
    if (gNearKeysAdded) {
        fail("g: the plug-in added a key less than 1e-6 from another one");
    }
    // (a)
    if (F.synced) {
        for (int side = 0; side < F.nSides(); ++side) {
            ParamRec* s = P(F.S(side));
            ParamRec* c = P(F.C(side));
            Anim& ks = gAnim[s];
            Anim& kc = gAnim[c];
            if (ks.expr) {
                continue;
            }
            // only if all colorspaces of the string have a menu entry
            bool valid = entryOf(paramString(s)) >= 0 || !ks.s.empty();
            for (auto& p : ks.s) {
                if (entryOf(sValue(ks, p.first)) < 0) {
                    valid = false;
                }
            }
            if (ks.s.empty() && entryOf(paramString(s)) < 0) {
                valid = false;
            }
            if (!valid) {
                continue;
            }
            // (a param whose reported keys are at 0 and 1, with a hidden key less than 1e-6 from one of them, may be
            // taken for a param with an expression: a documented limitation, unless FUZZ_STRICT_A is set)
            const std::vector<double> zeroOne = { 0., 1. };
            if (!std::getenv("FUZZ_STRICT_A") && (((keyTimes(s) == zeroOne) && (allKeyTimes(s).size() != 2)) || ((keyTimes(c) == zeroOne) && (allKeyTimes(c).size() != 2)))) {
                continue;
            }
            const std::vector<double> st = keyTimes(s);
            const std::vector<double> ct = keyTimes(c);
            if (st != ct) {
                fail(std::string("a: key times of ") + F.C(side) + " differ from " + F.S(side));
            }
            // (at frames)
            std::set<double> samples;
            for (double t : st) {
                samples.insert(std::ceil(t));
            }
            for (double t : { -10., -1., 0., 1., 2., 3., 5., 7., 10., 15., 20., 25., 100. }) {
                samples.insert(t);
            }
            for (double t : samples) {
                const double ts = t;
                // (the value at a frame whose last key is hidden from the plug-in, a key less than 1e-6 after another
                // one, may differ: a documented limitation)
                bool hidden = false;
                for (ParamRec* r : { s, c }) {
                    const std::vector<double> all = allKeyTimes(r);
                    const std::vector<double> rep = keyTimes(r);
                    double last = -std::numeric_limits<double>::infinity();
                    for (double k : all) {
                        if (k <= t) {
                            last = k;
                        }
                    }
                    if ((last > -std::numeric_limits<double>::infinity()) && (std::find(rep.begin(), rep.end(), last) == rep.end())) {
                        hidden = true;
                    }
                }
                if (hidden) {
                    continue;
                }
                if (entryOf(stringAt(s, ts)) != intAt(c, t)) {
                    char b[160];
                    std::snprintf(b, sizeof(b), "a: %s differs from %s at t=%g", F.C(side), F.S(side), t);
                    fail(b);
                    break;
                }
            }
        }
    }
    // (c)
    if (gLoop) {
        fail("c: instanceChanged recursion loop");
    }
    // (d) renders (OCIOColorSpace)
    if (F.kind == "cs" && gFailWhat.empty()) {
        for (double t : { gCurrentTime, 0., 1., 5., 10., 20., 7.5 }) {
            double ts = gRoundStringTime ? std::floor(t + 0.5) : t;
            int n = scaleOf(stringAt(P(kIS), ts));
            int m = scaleOf(stringAt(P(kOS), ts));
            if (!n || !m) {
                continue;
            }
            double got = render(t);
            ++gRenderChecks;
            double exp = kInput * n / m;
            if (std::fabs(got - exp) > 1e-5) {
                char b[160];
                std::snprintf(b, sizeof(b), "d: render at t=%g is %g, the strings give x%d->x%d=%g", t, got, n, m, exp);
                fail(b);
                break;
            }
        }
    }
    // (d) renders (ReadOIIO, reading a 0.25 image)
    if (F.kind == "reader" && gFailWhat.empty() && std::getenv("FUZZ_EXR")) {
        for (double t : { gCurrentTime, 0., 5., 10., 20. }) {
            double ts = gRoundStringTime ? std::floor(t + 0.5) : t;
            int n = scaleOf(stringAt(P(kIS), ts));
            int m = scaleOf(stringAt(P(kOS), ts));
            if (!n || !m) {
                continue;
            }
            double got = renderReader(t);
            ++gRenderChecks;
            double exp = kInput * n / m;
            if (std::fabs(got - exp) > 1e-5) {
                char b[160];
                std::snprintf(b, sizeof(b), "d: reader render at t=%g is %g, the strings give x%d->x%d=%g", t, got, n, m, exp);
                fail(b);
                break;
            }
        }
    }
    // (f)
    (void)keysAtPhaseStart;
    // (up to the number of keys that the op revealed, which the plug-in mirrors)
    if (noKeyPhase && (totalKeys() - gKeysBeforeOp > std::max(0, gHiddenKeysBefore - hiddenKeys()))) {
        char b[160];
        std::snprintf(b, sizeof(b), "f: %d keys after an op that adds none (%d at the start of the phase)", totalKeys(), keysAtPhaseStart);
        fail(b);
    }
}

// (e): undo of a swap, both sides keyed
struct SwapTrack {
    bool clean = false; // since the snapshot: only swaps
    bool both = false;
    std::string before;
    bool undone = false; // the last op was an undo of it
    std::string after;
};
SwapTrack gST;

void
trackBefore(const Op& o)
{
    gNearKeysAdded = 0;
    gSwapNearKeys = false;
    gKeysBeforeOp = totalKeys();
    gHiddenKeysBefore = hiddenKeys();
    if ((o.type == oSwap) && F.hasOut) {
        std::vector<double> all = allKeyTimes(P(kIS));
        const std::vector<double> out = allKeyTimes(P(kOS));
        all.insert(all.end(), out.begin(), out.end());
        for (size_t i = 0; i < all.size(); ++i) {
            for (size_t j = 0; j < all.size(); ++j) {
                if ((all[i] != all[j]) && (std::fabs(all[i] - all[j]) < 1e-6)) {
                    gSwapNearKeys = true;
                }
            }
        }
    }
    if (o.type == oSwap) {
        const bool merging = gMergeBlocks && !gLastBlock.empty();
        if (merging && !(gST.clean && !gST.undone)) {
            // Natron merges this block into the last one, which is not followed only by swaps: undo goes back to
            // before the last one, with the other changes in between (e.g. menu edits, which are not undoable)
            gST.both = false;
            gST.clean = true;
            gST.undone = false;
        } else if (!merging) {
            gST.clean = true;
            gST.undone = false;
            gST.before = stringsState();
            // (a swap of strings with keys less than 1e-6 apart deletes the later key, which undo doesn't restore: a
            // documented limitation)
            gST.both = !gAnim[P(kIS)].s.empty() && F.hasOut && !gAnim[P(kOS)].s.empty() && !gAnim[P(kIS)].expr && !gAnim[P(kOS)].expr && !gSwapNearKeys;
        }
    } else if (o.type == oUndo || o.type == oRedo || o.type == oTime) {
        // keep
    } else {
        gST.clean = false;
    }
}

void
trackAfter(const Op& o,
           bool applied)
{
    if (o.type == oSwap) {
        gST.after = stringsState();
    } else if (o.type == oUndo && applied && gST.clean && !gST.undone) {
        gST.undone = true;
        if (gST.both && stringsState() != gST.before) {
            fail("e: undo of a swap (both sides keyed) gives " + stringsState() + ", expected " + gST.before);
        }
    } else if (o.type == oRedo && applied && gST.clean && gST.undone) {
        gST.undone = false;
        if (gST.both && stringsState() != gST.after) {
            fail("e: redo of a swap gives " + stringsState() + ", expected " + gST.after);
        }
    }
}

Restored
randomStart(bool project)
{
    Restored r;
    if (F.hasFile) {
        r.strings.push_back({ kFilename, gTmpDir + (F.R(2) ? "/out.exr" : "/out_x3.exr") });
        r.ints.push_back({ kGuessedParams, 1 });
    }
    if (!project) {
        return r;
    }
    auto keys = [&](std::map<double, std::string>& m) {
        int n = F.R(4);
        for (int i = 0; i < n; ++i) {
            m[F.randTime()] = kNames[F.R(4)];
        }
    };
    keys(r.inputKeys);
    if (F.hasOut) {
        keys(r.outputKeys);
    }
    return r;
}

// run an episode from scratch; returns the index of the first failing op, or -1
int
runEpisode(const Episode& e,
           size_t nOps,
           bool record)
{
    gFailWhat.clear();
    gQuiet = true;
    newInstance(*F.plug, e.start);
    F.synced = e.start.inputKeys.empty() && e.start.outputKeys.empty();
    gST = SwapTrack();
    if (gKeyWritesOutsideInstanceChanged) {
        fail("b: createInstance writes keys");
        return 0;
    }
    int keysAtPhase = 0;
    bool prevNoKey = false;
    for (size_t i = 0; i < nOps && i < e.ops.size(); ++i) {
        const Op& o = e.ops[i];
        bool noKeyPhase = o.noKey;
        if (noKeyPhase && !prevNoKey) {
            keysAtPhase = totalKeys();
        }
        prevNoKey = noKeyPhase;
        const bool canUndo = !gLastBlock.empty();
        const bool canRedo = !gRedoBlock.empty();
        trackBefore(o);
        int calls0 = gCalls;
        apply(o);
        if (record) {
            int n = gCalls - calls0;
            int& m = F.maxCallsByOp[kOpNames[o.type]];
            m = std::max(m, n);
        }
        if ((o.type != oSaveLoad) && (o.type != oTime) && (gCalls > calls0)) {
            F.synced = true;
        }
        if (o.type == oSaveLoad && gKeyWritesOutsideInstanceChanged) {
            fail("b: createInstance (load) writes keys");
        }
        trackAfter(o, (o.type == oUndo && canUndo) || (o.type == oRedo && canRedo));
        checkInvariants(o, noKeyPhase, keysAtPhase);
        if (!gFailWhat.empty()) {
            return (int)i;
        }
    }
    return -1;
}

void
printRepro(const Episode& e,
           int upto,
           const std::string& what)
{
    // replay, printing the state after each op
    std::printf("    start: inputKeys={");
    for (auto& p : e.start.inputKeys) {
        std::printf("%g:%s,", p.first, p.second.c_str());
    }
    std::printf("} outputKeys={");
    for (auto& p : e.start.outputKeys) {
        std::printf("%g:%s,", p.first, p.second.c_str());
    }
    std::printf("}\n");
    gFailWhat.clear();
    newInstance(*F.plug, e.start);
    std::printf("    after load: %s\n", fullState().c_str());
    for (int i = 0; i <= upto; ++i) {
        apply(e.ops[i]);
        std::printf("    %2d %-40s -> %s\n", i, describe(e.ops[i]).c_str(), fullState().c_str());
        if (std::getenv("FUZZ_TRACE") && (i + 1 >= upto)) {
            // (the instance changed actions of the last op, and the exact key times)
            for (size_t j = 0; j < gLog.size(); ++j) {
                std::printf("         %s\n", gLog[j].c_str());
            }
            for (const char* name : { kIS, kIC, kOS, kOC }) {
                if (F.plug->ctx.params.count(name)) {
                    std::printf("         %s:", name);
                    for (double t : allKeyTimes(P(name))) {
                        std::printf(" %.9g", t);
                    }
                    std::printf("\n");
                }
            }
        }
    }
    std::printf("    FAILS: %s\n", what.c_str());
}

// greedy shrinking: remove ops while the same kind of failure remains
Episode
shrink(Episode e,
       int& failAt,
       std::string& what)
{
    const std::string kind = what.substr(0, 2);
    e.ops.resize(failAt + 1);
    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t i = 0; i < e.ops.size(); ++i) {
            Episode e2 = e;
            e2.ops.erase(e2.ops.begin() + i);
            int f = runEpisode(e2, e2.ops.size(), false);
            if (f >= 0 && gFailWhat.substr(0, 2) == kind) {
                e2.ops.resize(f + 1);
                e = e2;
                failAt = f;
                what = gFailWhat;
                progress = true;
                break;
            }
        }
    }
    // also try an empty start
    if (!e.start.inputKeys.empty() || !e.start.outputKeys.empty()) {
        Episode e2 = e;
        e2.start.inputKeys.clear();
        e2.start.outputKeys.clear();
        int f = runEpisode(e2, e2.ops.size(), false);
        if (f >= 0 && gFailWhat.substr(0, 2) == kind) {
            e = e2;
            e.ops.resize(f + 1);
            failAt = f;
            what = gFailWhat;
        }
    }
    return e;
}

} // namespace

int scenarios();

int
main(int argc,
     char** argv)
{
    if (argc < 4 || !std::getenv("OCIO")) {
        std::printf("usage: OCIO=swap.ocio %s cs|writer|reader nops seed\n", argv[0]);
        return 2;
    }
    gNatronExact = std::getenv("OCIO_ANIM_NATRON_EXACT") != nullptr;
    gRoundStringTime = std::getenv("OCIO_ANIM_NO_ROUND") == nullptr;
    gEditBlockEndsOnNestedAction = std::getenv("OCIO_ANIM_EDIT_BLOCK_BUG") != nullptr;
    gMergeBlocks = std::getenv("OCIO_ANIM_NO_MERGE") == nullptr;
    F.noExpr = std::getenv("FUZZ_NO_EXPR") != nullptr;
    initHost();
    // (OCIO_ANIM_NOT_NATRON: another host, which reads string params at the given time)
    if (std::getenv("OCIO_ANIM_NOT_NATRON")) {
        gRoundStringTime = false;
    } else {
        pSetInt(PH(&gHostProps), kNatronOfxHostIsNatron, 0, 1);
    }
    installAnimatedParamSuite();
    if (std::string(argv[1]) == "scenario") {
        load(gColorSpace);
        return scenarios();
    }
    F.kind = argv[1];
    Plug* plugs[] = { &gColorSpace, &gWriter, &gReader };
    F.plug = F.kind == "cs" ? plugs[0] : F.kind == "writer" ? plugs[1]
                                                            : plugs[2];
    load(*F.plug);
    const long nops = std::atol(argv[2]);
    F.rng.seed((unsigned)std::atol(argv[3]));

    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    dir /= "ocio_anim_fuzz." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directory(dir, ec);
    gTmpDir = dir.generic_string();
    if (const char* src = std::getenv("FUZZ_EXR")) {
        for (const char* fn : { "out.exr", "out_x1.exr", "out_x2.exr", "out_x3.exr", "out_x4.exr" }) {
            std::filesystem::copy_file(src, dir / fn, ec);
        }
    }

    F.hasOut = F.plug->ctx.params.count(kOS) > 0;
    F.hasSwap = F.plug->ctx.params.count(kSwap) > 0;
    F.hasFile = F.plug->ctx.params.count(kFilename) > 0;
    std::printf("%s: output colorspace %d, swap %d, file %d; Natron-exact strings %d, rounded string times %d, merged undo %d\n",
                F.plug->id, F.hasOut, F.hasSwap, F.hasFile, gNatronExact, gRoundStringTime, gMergeBlocks);

    std::map<std::string, int> failCounts;
    std::map<std::string, std::pair<Episode, std::pair<int, std::string>>> firstRepro;
    long done = 0;
    int episodes = 0;
    const size_t kLen = 200;
    while (done < nops) {
        Episode e;
        e.start = randomStart(F.R(2));
        // generate on the fly (the generator looks at the state)
        gFailWhat.clear();
        gQuiet = true;
        newInstance(*F.plug, e.start);
        F.synced = e.start.inputKeys.empty() && e.start.outputKeys.empty();
        gST = SwapTrack();
        int keysAtPhase = 0;
        int failAt = -1;
        for (size_t i = 0; i < kLen; ++i) {
            bool noKeyPhase = (i % 100) >= 70;
            if ((i % 100) == 70) {
                keysAtPhase = totalKeys();
            }
            Op o = generate(noKeyPhase);
            o.noKey = noKeyPhase;
            e.ops.push_back(o);
            const bool canUndo = !gLastBlock.empty();
            const bool canRedo = !gRedoBlock.empty();
            trackBefore(o);
            int calls0 = gCalls;
            apply(o);
            int n = gCalls - calls0;
            int& m = F.maxCallsByOp[kOpNames[o.type]];
            m = std::max(m, n);
            // (a time change of a reader doesn't reach GenericOCIO)
            if ((o.type != oSaveLoad) && (o.type != oTime) && (gCalls > calls0)) {
                F.synced = true;
            }
            if (o.type == oSaveLoad && gKeyWritesOutsideInstanceChanged) {
                fail("b: createInstance (load) writes keys");
            }
            trackAfter(o, (o.type == oUndo && canUndo) || (o.type == oRedo && canRedo));
            checkInvariants(o, noKeyPhase, keysAtPhase);
            ++done;
            if (!gFailWhat.empty()) {
                failAt = (int)i;
                break;
            }
        }
        ++episodes;
        if (failAt >= 0) {
            std::string what = gFailWhat;
            // the signature: the invariant and the first words
            std::string sig = what.substr(0, what.find(" at t=") == std::string::npos ? std::min<size_t>(what.size(), 40) : what.find(" at t="));
            if (sig.size() > 50) {
                sig = sig.substr(0, 50);
            }
            if (failCounts[sig]++ == 0) {
                int fa = failAt;
                std::string w = what;
                // check that the replay reproduces it
                int f = runEpisode(e, e.ops.size(), false);
                if (f >= 0) {
                    fa = f;
                    w = gFailWhat;
                    Episode s = shrink(e, fa, w);
                    firstRepro[sig] = { s, { fa, w } };
                } else {
                    std::printf("(non-reproducible failure: %s)\n", what.c_str());
                }
            }
        }
    }
    std::printf("%ld ops in %d episodes, %ld render checks\n", done, episodes, gRenderChecks);
    std::printf("max instance changed actions per op:");
    for (auto& p : F.maxCallsByOp) {
        std::printf(" %s=%d", p.first.c_str(), p.second);
    }
    std::printf("\n");
    for (auto& p : failCounts) {
        std::printf("FAILURE x%d: %s\n", p.second, p.first.c_str());
        auto it = firstRepro.find(p.first);
        if (it != firstRepro.end()) {
            gQuiet = true;
            printRepro(it->second.first, it->second.second.first, it->second.second.second);
        }
    }
    destroyInstance();
    std::filesystem::remove_all(dir, ec);
    std::printf("%s\n", failCounts.empty() ? "FUZZ PASSED" : "FUZZ FAILED");
    return failCounts.empty() ? 0 : 1;
}

// ---- deterministic scenarios (repros), run with: ocio_anim_fuzz scenario
namespace {
void
show(const char* what)
{
    std::printf("  %-55s %s\n", what, fullState().c_str());
}
} // namespace

int
scenarios()
{
    gQuiet = true;
    gOnFail = [](const char* m) { std::printf("  CHECK FAILED: %s\n", m); };
    F.plug = &gColorSpace;
    F.kind = "cs";
    F.hasOut = true;
    if (std::getenv("SCEN") && std::string(std::getenv("SCEN")) == "main") {
        std::printf("R1: keys at frames 0 and 1 (Natron reads strings at floor(t+0.5)): Remove animation of the menu\n");
        newInstance(gColorSpace);
        show("new node");
        gCurrentTime = 0;
        userSet(kIC, 3, true);
        show("t=0: Set Key on the input menu, x4");
        gCurrentTime = 1;
        userSet(kIC, 0);
        show("t=1: input menu x1");
        userRemoveAnimation(kIC);
        show("Remove animation of the input menu");
        std::printf("    render t=0: %g (expected x1->x1 = 0.25)\n", render(0.));
        std::printf("R1b: same keys, after loading a project\n");
        Restored r;
        r.inputKeys = { { 0., "x4" }, { 1., "x1" } };
        newInstance(gColorSpace, r);
        show("load input {0:x4,1:x1}");
        gCurrentTime = 3;
        userSetString(kKey1, "a");
        show("t=3: edit another param");
        userSet(kIC, 1);
        show("t=3: input menu x2");
        std::printf("R2: Swap with input {0:x4,1:x1} and output {5:x2}\n");
        r.outputKeys = { { 5., "x2" } };
        newInstance(gColorSpace, r);
        gCurrentTime = 3;
        userSetString(kKey1, "a");
        show("load, t=3, edit another param");
        userPress(kSwap);
        show("Swap");
        std::printf("R3: Restore default of the menu after loading a project, before any instance changed action\n");
        Restored r3;
        r3.inputKeys = { { 0., "x2" }, { 10., "x4" } };
        newInstance(gColorSpace, r3);
        gCurrentTime = 5;
        show("load input {0:x2,10:x4}, t=5");
        userRestoreDefault(kIC);
        show("Restore default of the input menu");
        newInstance(gColorSpace, r3);
        gCurrentTime = 5;
        userSetString(kKey1, "a");
        show("(same, after an edit of another param)");
        userRestoreDefault(kIC);
        show("Restore default of the input menu");
        return 0;
    }
    if (std::getenv("SCEN") && std::string(std::getenv("SCEN")) == "exact") {
        gNatronExact = true;
        std::printf("N1: Natron's index-valued string keys: a menu edit between two keys, after loading a project\n");
        Restored r;
        r.inputKeys = { { 0., "x2" }, { 10., "x4" } };
        newInstance(gColorSpace, r);
        show("load input {0:x2,10:x4}");
        gCurrentTime = 5;
        userSet(kIC, 2);
        show("t=5: input menu x3");
        std::printf("    render t=10: %g (expected x4->x1 = 1)\n", render(10.));
        std::printf("N2: Swap both sides keyed at t=8, then undo\n");
        newInstance(gColorSpace);
        gCurrentTime = 0;
        userSet(kIC, 1, true);
        gCurrentTime = 10;
        userSet(kIC, 3);
        gCurrentTime = 2;
        userSet(kOC, 2, true);
        gCurrentTime = 8;
        userSet(kOC, 0);
        show("input {0:x2,10:x4}, output {2:x3,8:x1}, t=8");
        userPress(kSwap);
        show("Swap");
        userUndo();
        show("Undo (expected input {0:2,10:4}, output {2:3,8:1})");
        std::printf("N3: deleting a menu key between keys (synced)\n");
        newInstance(gColorSpace);
        int vals[] = { 1, 2, 3, 0 };
        for (int i = 0; i < 4; ++i) {
            gCurrentTime = 5 * i;
            userSet(kIC, vals[i], i == 0);
        }
        show("input menu {0:x2,5:x3,10:x4,15:x1}");
        userDeleteKeys(kIC, { 5. });
        show("delete the menu key at 5 (expected {0:2,10:4,15:1})");
        return 0;
    }
    {
        std::printf("S1: delete a menu key at 0 with string keys {0:x1,1:x1}\n");
        Restored r;
        r.inputKeys = { { 1., "x1" } };
        r.outputKeys = { { 10., "x2" }, { 16., "x1" } };
        newInstance(gColorSpace, r);
        show("load");
        gCurrentTime = 0;
        userSet(kOC, 1, true);
        show("set key OC=x2 at 0");
        userSet(kIC, 0);
        show("set IC=x1 at 0");
        std::printf("  gLog:\n");
        userDeleteKeys(kIC, { 0. });
        for (auto& l : gLog) {
            std::printf("    %s\n", l.c_str());
        }
        show("delete IC key at 0");
    }
    return 0;
}
