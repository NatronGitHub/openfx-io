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
 * OFX GenericOCIO plugin add-on.
 * Adds OpenColorIO functionality to any plugin.
 */

#include "GenericOCIO.h"

/*
   http://opencolorio.org/userguide/config_syntax.html#roles

   A description of all roles. Note that applications may interpret or use these differently.

   color_picking - Colors in a color-selection UI can be displayed in this space, while selecting colors in a different working space (e.g scene_linear or texture_paint)
   color_timing - colorspace used for applying color corrections, e.g user-specified grade within an image viewer (if the application uses the DisplayTransform::setDisplayCC API method)
   compositing_log - a log colorspace used for certain processing operations (plate resizing, pulling keys, degrain, etc). Used by the OCIOLogConvert Nuke node.
   data - used when writing data outputs such as normals, depth data, and other “non color” data. The colorspace in this role should typically have data: true specified, so no color transforms are applied.
   default - when strictparsing: false, this colorspace is used as a fallback. If not defined, the scene_linear role is used
   matte_paint - Colorspace which matte-paintings are created in (for more information, see the guide on baking ICC profiles for Photoshop, and spi-vfx)
   reference - Colorspace used for reference imagery (e.g sRGB images from the internet)
   scene_linear - The scene-referred linear-to-light colorspace, typically used as reference space (see Terminology)
   texture_paint - Similar to matte_paint but for painting textures for 3D objects (see the description of texture painting in SPI’s pipeline)
 */

#include <algorithm> // std::swap (C++03)
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <utility> // std::swap (C++11)
#ifdef DEBUG
#include <cstdio>
#define DBG(x) x
#else
#define DBG(x) (void)0
#endif
#include "ofxsMacros.h"
#include <ofxNatron.h>
#include <ofxsImageEffect.h>
#include <ofxsLog.h>
#include <ofxsParam.h>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef OFX_IO_USING_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

using std::string;

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

#ifdef OFX_IO_USING_OCIO
static bool gWasOCIOEnvVarFound = false;
static bool gHostIsNatron = false;
#endif

// define to disable hiding parameters (useful for debugging)
//#define OFX_OCIO_NOSECRET

static string
trim(string const& str)
{
    const string whitespace = " \t\f\v\n\r";

    std::size_t first = str.find_first_not_of(whitespace);

    // If there is no non-whitespace character, both first and last will be string::npos (-1)
    // There is no point in checking both, since if either doesn't work, the
    // other won't work, either.
    if (first == string::npos) {
        return "";
    }

    std::size_t last = str.find_last_not_of(whitespace);

    return str.substr(first, last - first + 1);
}

static string
whitespacify(string str)
{
    std::replace(str.begin(), str.end(), '\t', ' ');
    std::replace(str.begin(), str.end(), '\f', ' ');
    std::replace(str.begin(), str.end(), '\v', ' ');
    std::replace(str.begin(), str.end(), '\n', ' ');
    std::replace(str.begin(), str.end(), '\r', ' ');

    return str;
}

#ifdef OFX_IO_USING_OCIO
static const char*
colorSpaceName(OCIO::ConstConfigRcPtr config,
               const char* colorSpaceNameDefault)
{
    OCIO::ConstColorSpaceRcPtr cs;
    if (!strcmp(colorSpaceNameDefault, "sRGB") || !strcmp(colorSpaceNameDefault, "srgb")) {
        if ((cs = config->getColorSpace("sRGB"))) {
            // nuke-default, blender, natron
            return cs->getName();
        } else if ((cs = config->getColorSpace("sRGB D65"))) {
            // blender-cycles
            return cs->getName();
        } else if ((cs = config->getColorSpace("sRGB (D60 sim.)"))) {
            // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
            return cs->getName();
        } else if ((cs = config->getColorSpace("out_srgbd60sim"))) {
            // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
            return cs->getName();
        } else if ((cs = config->getColorSpace("rrt_Gamma2.2"))) {
            // rrt_Gamma2.2 in aces 0.7.1
            return cs->getName();
        } else if ((cs = config->getColorSpace("rrt_srgb"))) {
            // rrt_srgb in aces 0.1.1
            return cs->getName();
        } else if ((cs = config->getColorSpace("srgb8"))) {
            // srgb8 in spi-vfx
            return cs->getName();
        } else if ((cs = config->getColorSpace("vd16"))) {
            // vd16 in spi-anim
            return cs->getName();
        } else if ((cs = config->getColorSpace("VD16"))) {
            // VD16 in blender
            return cs->getName();
        }
    } else if (!strcmp(colorSpaceNameDefault, "AdobeRGB") || !strcmp(colorSpaceNameDefault, "adobergb")) {
        if ((cs = config->getColorSpace("AdobeRGB"))) {
            // natron
            return cs->getName();
        }
    } else if (!strcmp(colorSpaceNameDefault, "Rec709") || !strcmp(colorSpaceNameDefault, "rec709")) {
        if ((cs = config->getColorSpace("Rec709"))) {
            // nuke-default
            return cs->getName();
        } else if ((cs = config->getColorSpace("nuke_rec709"))) {
            // blender
            return cs->getName();
        } else if ((cs = config->getColorSpace("Rec.709 - Full"))) {
            // out_rec709full or "Rec.709 - Full" in aces 1.0.0
            return cs->getName();
        } else if ((cs = config->getColorSpace("out_rec709full"))) {
            // out_rec709full or "Rec.709 - Full" in aces 1.0.0
            return cs->getName();
        } else if ((cs = config->getColorSpace("rrt_rec709_full_100nits"))) {
            // rrt_rec709_full_100nits in aces 0.7.1
            return cs->getName();
        } else if ((cs = config->getColorSpace("rrt_rec709"))) {
            // rrt_rec709 in aces 0.1.1
            return cs->getName();
        } else if ((cs = config->getColorSpace("hd10"))) {
            // hd10 in spi-anim and spi-vfx
            return cs->getName();
        }
    } else if (!strcmp(colorSpaceNameDefault, "KodakLog") || !strcmp(colorSpaceNameDefault, "kodaklog")) {
        if ((cs = config->getColorSpace("Cineon"))) {
            // Cineon in nuke-default
            return cs->getName();
        } else if ((cs = config->getColorSpace("REDlogFilm"))) {
            // REDlogFilm in aces 1.0.0
            return cs->getName();
        } else if ((cs = config->getColorSpace("cineon"))) {
            // cineon in aces 0.7.1
            return cs->getName();
        } else if ((cs = config->getColorSpace("adx10"))) {
            // adx10 in aces 0.1.1
            return cs->getName();
        } else if ((cs = config->getColorSpace("lg10"))) {
            // lg10 in spi-vfx
            return cs->getName();
        } else if ((cs = config->getColorSpace("lm10"))) {
            // lm10 in spi-anim
            return cs->getName();
        } else {
            return OCIO::ROLE_COMPOSITING_LOG; // reasonable default
        }
    } else if (!strcmp(colorSpaceNameDefault, "Linear") || !strcmp(colorSpaceNameDefault, "linear")) {
        return OCIO::ROLE_SCENE_LINEAR;
        // lnf in spi-vfx
    } else if ((cs = config->getColorSpace(colorSpaceNameDefault))) {
        // maybe we're lucky
        return cs->getName();
    }

    // unlucky
    return colorSpaceNameDefault;
} // colorSpaceName

static string
canonicalizeColorSpace(OCIO::ConstConfigRcPtr config,
                       const string& csname)
{
    if (!config) {
        return csname;
    }
    const int defaultcs = config->getIndexForColorSpace(OCIO::ROLE_DEFAULT);
    const int referencecs = config->getIndexForColorSpace(OCIO::ROLE_REFERENCE);
    const int datacs = config->getIndexForColorSpace(OCIO::ROLE_DATA);
    const int colorpickingcs = config->getIndexForColorSpace(OCIO::ROLE_COLOR_PICKING);
    const int scenelinearcs = config->getIndexForColorSpace(OCIO::ROLE_SCENE_LINEAR);
    const int compositinglogcs = config->getIndexForColorSpace(OCIO::ROLE_COMPOSITING_LOG);
    const int colortimingcs = config->getIndexForColorSpace(OCIO::ROLE_COLOR_TIMING);
    const int texturepaintcs = config->getIndexForColorSpace(OCIO::ROLE_TEXTURE_PAINT);
    const int mattepaintcs = config->getIndexForColorSpace(OCIO::ROLE_MATTE_PAINT);
    int inputSpaceIndex = config->getIndexForColorSpace(csname.c_str());
    if (inputSpaceIndex == scenelinearcs) {
        return OCIO::ROLE_SCENE_LINEAR;
    } else if (inputSpaceIndex == defaultcs) {
        return OCIO::ROLE_DEFAULT;
    } else if (inputSpaceIndex == referencecs) {
        return OCIO::ROLE_REFERENCE;
    } else if (inputSpaceIndex == datacs) {
        return OCIO::ROLE_DATA;
    } else if (inputSpaceIndex == colorpickingcs) {
        return OCIO::ROLE_COLOR_PICKING;
    } else if (inputSpaceIndex == compositinglogcs) {
        return OCIO::ROLE_COMPOSITING_LOG;
    } else if (inputSpaceIndex == colortimingcs) {
        return OCIO::ROLE_COLOR_TIMING;
    } else if (inputSpaceIndex == texturepaintcs) {
        return OCIO::ROLE_TEXTURE_PAINT;
    } else if (inputSpaceIndex == mattepaintcs) {
        return OCIO::ROLE_MATTE_PAINT;
    }

    return csname;
}

// The number of keys of a string or choice param, 0 if the host doesn't animate this type of param (a host may
// fail to give the keys of a param that doesn't animate)
static unsigned int
numKeys(StringParam* p)
{
    return getImageEffectHostDescription()->supportsStringAnimation ? p->getNumKeys() : 0;
}

static unsigned int
numKeys(ChoiceParam* p)
{
    return getImageEffectHostDescription()->supportsChoiceAnimation ? p->getNumKeys() : 0;
}

#endif // ifdef OFX_IO_USING_OCIO

GenericOCIO::GenericOCIO(ImageEffect* parent)
    : _parent(parent)
    , _created(false)
    , _instanceChangedDepth(0)
#ifdef OFX_IO_USING_OCIO
    , _settingColorspaces(false)
    , _ocioConfigFileName()
    , _ocioConfigFile(NULL)
    , _inputSpace(NULL)
    , _outputSpace(NULL)
    , _inverse(NULL)
#ifdef OFX_OCIO_CHOICE
    , _choiceIsOk(true)
    , _choiceFileName()
    , _choiceAnimSynced(true)
#endif
    , _contextKey1(NULL)
    , _contextValue1(NULL)
    , _contextKey2(NULL)
    , _contextValue2(NULL)
    , _contextKey3(NULL)
    , _contextValue3(NULL)
    , _contextKey4(NULL)
    , _contextValue4(NULL)
    , _config()
#endif
{
#ifdef OFX_IO_USING_OCIO
    _ocioConfigFile = _parent->fetchStringParam(kOCIOParamConfigFile);
    if (_parent->paramExists(kOCIOParamInputSpace)) {
        _inputSpace = _parent->fetchStringParam(kOCIOParamInputSpace);
    }
    if (_parent->paramExists(kOCIOParamOutputSpace)) {
        _outputSpace = _parent->fetchStringParam(kOCIOParamOutputSpace);
    }
    if (_parent->paramExists(kOCIOParamInverse)) {
        _inverse = _parent->fetchBooleanParam(kOCIOParamInverse);
    }
#ifdef OFX_OCIO_CHOICE
    _ocioConfigFile->getDefault(_choiceFileName);
    for (int i = 0; i < 2; ++i) {
        Side& side = _sides[i];
        side.str = i ? _outputSpace : _inputSpace;
        side.choice = side.str ? _parent->fetchChoiceParam(i ? kOCIOParamOutputSpaceChoice : kOCIOParamInputSpaceChoice) : NULL;
        side.initialIndex = -1;
        side.strExpression = -1;
        side.choiceExpression = -1;
    }
#endif
    loadConfig();
#ifdef OFX_OCIO_CHOICE
    if (!_config) {
#ifndef OFX_OCIO_NOSECRET
        for (int i = 0; i < 2; ++i) {
            if (_sides[i].choice) {
                _sides[i].choice->setIsSecretAndDisabled(true);
            }
        }
#endif
    }
#endif
    if (_parent->paramExists(kOCIOParamContextKey1)) {
        _contextKey1 = _parent->fetchStringParam(kOCIOParamContextKey1);
        _contextValue1 = _parent->fetchStringParam(kOCIOParamContextValue1);
        _contextKey2 = _parent->fetchStringParam(kOCIOParamContextKey2);
        _contextValue2 = _parent->fetchStringParam(kOCIOParamContextValue2);
        _contextKey3 = _parent->fetchStringParam(kOCIOParamContextKey3);
        _contextValue3 = _parent->fetchStringParam(kOCIOParamContextValue3);
        _contextKey4 = _parent->fetchStringParam(kOCIOParamContextKey4);
        _contextValue4 = _parent->fetchStringParam(kOCIOParamContextValue4);
        assert(_contextKey1 && _contextKey2 && _contextKey3 && _contextKey4);
        assert(_contextValue1 && _contextValue2 && _contextValue3 && _contextValue4);
    }
#endif
    // setup the GUI
    // setValue() may be called from createInstance, according to
    // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#SettingParams
    // (this only sets the values of the menus, see Side in GenericOCIO.h)
    refreshInputAndOutputState(0.);
#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
    _choiceAnimSynced = true;
    for (int i = 0; i < 2; ++i) {
        Side& side = _sides[i];
        if (side.str) {
            side.choice->getValue(side.initialIndex);
            if (choiceMirrorsString(side) && (numKeys(side.str) > 0)) {
                _choiceAnimSynced = false;
            }
        }
    }
#endif
    _created = true;
}

#ifdef OFX_IO_USING_OCIO
#ifdef OFX_OCIO_CHOICE

// ChoiceParamType may be ChoiceParamDescriptor or ChoiceParam
template <typename ChoiceParamType>
static void
buildChoiceMenu(OCIO::ConstConfigRcPtr config,
                ChoiceParamType* choice,
                bool cascading,
                const string& name = "")
{
    // DBG(std::printf("%p->resetOptions\n", (void*)choice));
    choice->resetOptions();
    assert(choice->getNOptions() == 0);
    if (!config) {
        return;
    }
    int def = -1;
    int defaultcs = config->getIndexForColorSpace(OCIO::ROLE_DEFAULT);
    int referencecs = config->getIndexForColorSpace(OCIO::ROLE_REFERENCE);
    int datacs = config->getIndexForColorSpace(OCIO::ROLE_DATA);
    int colorpickingcs = config->getIndexForColorSpace(OCIO::ROLE_COLOR_PICKING);
    int scenelinearcs = config->getIndexForColorSpace(OCIO::ROLE_SCENE_LINEAR);
    int compositinglogcs = config->getIndexForColorSpace(OCIO::ROLE_COMPOSITING_LOG);
    int colortimingcs = config->getIndexForColorSpace(OCIO::ROLE_COLOR_TIMING);
    int texturepaintcs = config->getIndexForColorSpace(OCIO::ROLE_TEXTURE_PAINT);
    int mattepaintcs = config->getIndexForColorSpace(OCIO::ROLE_MATTE_PAINT);
    for (int i = 0; i < config->getNumColorSpaces(); ++i) {
        string csname = config->getColorSpaceNameByIndex(i);
        string msg;
        // set the default value, in case the GUI uses it
        if (!name.empty() && (csname == name)) {
            def = i;
        }
        OCIO::ConstColorSpaceRcPtr cs = config->getColorSpace(csname.c_str());
        if (cascading) {
            string family = config->getColorSpace(csname.c_str())->getFamily();
            if (!family.empty()) {
                csname = family + "/" + csname;
            }
        }
        string csdesc = cs ? cs->getDescription() : "(no colorspace)";
        csdesc = whitespacify(trim(csdesc));
        int csdesclen = csdesc.size();
        if (csdesclen > 0) {
            msg += csdesc;
        }
        bool first = true;
        int roles = 0;
        if (i == defaultcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_DEFAULT;
            first = false;
            ++roles;
        }
        if (i == referencecs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_REFERENCE;
            first = false;
            ++roles;
        }
        if (i == datacs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_DATA;
            first = false;
            ++roles;
        }
        if (i == colorpickingcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_COLOR_PICKING;
            first = false;
            ++roles;
        }
        if (i == scenelinearcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_SCENE_LINEAR;
            first = false;
            ++roles;
        }
        if (i == compositinglogcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_COMPOSITING_LOG;
            first = false;
            ++roles;
        }
        if (i == colortimingcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_COLOR_TIMING;
            first = false;
            ++roles;
        }
        if (i == texturepaintcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_TEXTURE_PAINT;
            first = false;
            ++roles;
        }
        if (i == mattepaintcs) {
            msg += first ? " (" : ", ";
            msg += OCIO::ROLE_MATTE_PAINT;
            first = false;
            ++roles;
        }
        if (roles > 0) {
            msg += ')';
        }
        // DBG(printf("%p->appendOption(\"%s\",\"%s\") (%d->%d options)\n", (void*)choice, csname.c_str(), msg.c_str(), i, i+1));
        assert(choice->getNOptions() == i);
        choice->appendOption(csname, msg);
        assert(choice->getNOptions() == i + 1);
    }
    if (def != -1) {
        choice->setDefault(def);
    }
} // buildChoiceMenu

#endif // ifdef OFX_OCIO_CHOICE
#endif // ifdef OFX_IO_USING_OCIO

void
GenericOCIO::loadConfig()
{
#ifdef OFX_IO_USING_OCIO
    string filename;
    _ocioConfigFile->getValue(filename);

    if (filename == _ocioConfigFileName) {
        return;
    }
    _config.reset();
    AutoSetAndRestoreThreadLocale locale;
    try {
        _ocioConfigFileName = filename;
        _config = OCIO::Config::CreateFromFile(_ocioConfigFileName.c_str());
    } catch (OCIO::Exception& e) {
        _ocioConfigFileName.clear();
        if (_inputSpace) {
            _inputSpace->setEnabled(false);
        }
        if (_outputSpace) {
            _outputSpace->setEnabled(false);
        }
#ifdef OFX_OCIO_CHOICE
        for (int i = 0; i < 2; ++i) {
            if (_sides[i].choice) {
                _sides[i].choice->setEnabled(false);
            }
        }
#endif
    }
#ifdef OFX_OCIO_CHOICE
    if (_config) {
        if (gHostIsNatron) {
            // the choice menu can only be modified in Natron
            // Natron supports changing the entries in a choiceparam
            // Nuke (at least up to 8.0v3) does not
            for (int i = 0; i < 2; ++i) {
                if (_sides[i].choice) {
                    buildChoiceMenu(_config, _sides[i].choice, _sides[i].choice->getIsCascading());
                }
            }
            _choiceFileName = _ocioConfigFileName;
        }
        _choiceIsOk = (_ocioConfigFileName == _choiceFileName);
    }
#endif
#endif
} // GenericOCIO::loadConfig

bool
GenericOCIO::configIsDefault() const
{
#ifdef OFX_IO_USING_OCIO
    string filename;
    _ocioConfigFile->getValue(filename);
    string defaultFilename;
    _ocioConfigFile->getDefault(defaultFilename);

    return (filename == defaultFilename);
#else

    return true;
#endif
}

#ifdef OFX_IO_USING_OCIO
OCIO::ConstContextRcPtr
GenericOCIO::getLocalContext(double time) const
{
    AutoSetAndRestoreThreadLocale locale;
    OCIO::ConstContextRcPtr context = _config->getCurrentContext();
    OCIO::ContextRcPtr mutableContext;

    if (_contextKey1) {
        string contextKey1;
        _contextKey1->getValueAtTime(time, contextKey1);
        if (!contextKey1.empty()) {
            string contextValue1;
            _contextValue1->getValueAtTime(time, contextValue1);

            if (!mutableContext) {
                mutableContext = context->createEditableCopy();
            }
            mutableContext->setStringVar(contextKey1.c_str(), contextValue1.c_str());
        }
    }
    if (_contextKey2) {
        string contextKey2;
        _contextKey2->getValueAtTime(time, contextKey2);
        if (!contextKey2.empty()) {
            string contextValue2;
            _contextValue2->getValueAtTime(time, contextValue2);

            if (!mutableContext) {
                mutableContext = context->createEditableCopy();
            }
            mutableContext->setStringVar(contextKey2.c_str(), contextValue2.c_str());
        }
    }
    if (_contextKey3) {
        string contextKey3;
        _contextKey3->getValueAtTime(time, contextKey3);
        if (!contextKey3.empty()) {
            string contextValue3;
            _contextValue3->getValueAtTime(time, contextValue3);

            if (!mutableContext) {
                mutableContext = context->createEditableCopy();
            }
            mutableContext->setStringVar(contextKey3.c_str(), contextValue3.c_str());
        }
    }
    if (_contextKey4) {
        string contextKey4;
        _contextKey4->getValueAtTime(time, contextKey4);
        if (!contextKey4.empty()) {
            string contextValue4;
            _contextValue4->getValueAtTime(time, contextValue4);

            if (!mutableContext) {
                mutableContext = context->createEditableCopy();
            }
            mutableContext->setStringVar(contextKey4.c_str(), contextValue4.c_str());
        }
    }

    if (mutableContext) {
        context = mutableContext;
    }

    return context;
} // GenericOCIO::getLocalContext

#endif // ifdef OFX_IO_USING_OCIO

bool
GenericOCIO::isIdentity(double time) const
{
    assert(_created);
#ifdef OFX_IO_USING_OCIO
    if (!_config) {
        string filename;
        _ocioConfigFile->getValue(filename);
        _parent->setPersistentMessage(Message::eMessageError, "", "Invalid OCIO config. file \"" + filename + "\"");
        throwSuiteStatusException(kOfxStatFailed);

        return false;
    }
    string inputSpace;
    string outputSpace;
    getProcessingColorspacesAtTime(time, inputSpace, outputSpace);
    if (inputSpace == outputSpace) {
        return true;
    }
    try {
        // maybe the names are not the same, but it's still a no-op (e.g. "scene_linear" and "linear")
        OCIO::ConstContextRcPtr context = getLocalContext(time); //_config->getCurrentContext();
        OCIO::ConstProcessorRcPtr proc = _config->getProcessor(context, inputSpace.c_str(), outputSpace.c_str());

        return proc->isNoOp();
    } catch (const std::exception& e) {
        _parent->setPersistentMessage(Message::eMessageError, "", e.what());
        throwSuiteStatusException(kOfxStatFailed);

        return false;
    }
#else

    return true;
#endif
}

// sets the correct choice menu items from the string values
void
GenericOCIO::refreshInputAndOutputState(double time)
{
#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
    for (int i = 0; i < 2; ++i) {
        if (_sides[i].str) {
            colorspaceCheck(_sides[i], time);
        }
    }
#else
    (void)time;
#endif
}

void
GenericOCIO::resetExpressionCache()
{
#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
    for (int i = 0; i < 2; ++i) {
        _sides[i].strExpression = -1;
        _sides[i].choiceExpression = -1;
    }
#endif
}

#ifdef OFX_IO_USING_OCIO
namespace {
// OFX doesn't say how string and choice params are interpolated, but their value can only depend on their keys (in
// Natron, it is held from a key to the next one, and before the first key it is that of the first key): two params
// with keys at the same times, with the same values, have the same value at all times.

// (key times are compared exactly: Natron keeps keys at their exact times, even less than 1e-6 apart, but its OFX key
// list only reports the first of the keys less than 1e-6 apart, OfxKeyFrames_compare in Engine/OfxParamInstance.cpp)
bool
containsTime(const std::vector<double>& times,
             double t)
{
    return std::find(times.begin(), times.end(), t) != times.end();
}

template <typename ParamT>
std::vector<double>
getKeyTimes(ParamT* p)
{
    std::vector<double> times;
    const unsigned int n = numKeys(p);
    times.reserve(n);
    for (unsigned int i = 0; i < n; ++i) {
        times.push_back(p->getKeyTime((int)i));
    }

    return times;
}

// Natron reports two keys, at times 0 and 1, for a param that has an expression (e.g. that of "Link to"), whatever
// its keys (so that a plug-in sees that its value may vary): setting or deleting keys at these times would change the
// param's real keys. (A param linked by paste-link or clone has the keys of its master.) String and choice values are
// held from a key to the next one: if the value is not that of the key at 0 before time 0.5, or that of the key at 1
// after it, the param has an expression. (Natron reads a string param at floor(time + 0.5), see
// OfxStringInstance::get(): no sample is in [0.5, 1). An expression that has these values at the times sampled here
// is not detected: e.g. the value of a "Link to" choice param doesn't depend on the time (NatronGitHub/Natron#1336), so
// that its menu gets keys at 0 and 1, with that value, which changes nothing.)
template <typename T, typename ParamT>
bool
hasExpressionKeysT(ParamT* p,
                   bool animates)
{
    if (!animates || (p->getNumKeys() != 2) || (p->getKeyTime(0) != 0.) || (p->getKeyTime(1) != 1.)) {
        return false;
    }
    T v0, v1, v;
    p->getValueAtTime(0., v0);
    p->getValueAtTime(1., v1);
    const double before[] = { -1000., -10., -1., -0.5, 0.25, 0.4 };
    const double after[] = { 1.25, 1.5, 2., 3., 10., 100., 1000. };
    for (size_t i = 0; i < sizeof(before) / sizeof(before[0]); ++i) {
        p->getValueAtTime(before[i], v);
        if (v != v0) {
            return true;
        }
    }
    for (size_t i = 0; i < sizeof(after) / sizeof(after[0]); ++i) {
        p->getValueAtTime(after[i], v);
        if (v != v1) {
            return true;
        }
    }

    return false;
}

bool
hasExpressionKeys(StringParam* str)
{
    return hasExpressionKeysT<string>(str, getImageEffectHostDescription()->supportsStringAnimation);
}

bool
hasExpressionKeys(ChoiceParam* choice)
{
    return hasExpressionKeysT<int>(choice, getImageEffectHostDescription()->supportsChoiceAnimation);
}

// the index of a colorspace in the choice menu (built from the config), or -1
int
colorspaceIndex(const OCIO::ConstConfigRcPtr& config,
                const string& name)
{
    return config->getIndexForColorSpace(name.c_str());
}

// the colorspace of a menu entry, or "" if there is none
string
colorspaceOfIndex(const OCIO::ConstConfigRcPtr& config,
                  int index)
{
    if ((index < 0) || (index >= config->getNumColorSpaces())) {
        return string();
    }

    return canonicalizeColorSpace(config, config->getColorSpaceNameByIndex(index));
}

// The colorspaces of a param, at the time of a change: its keys, if it has any, and its value at that time
struct ColorspaceAnimation {
    std::vector<std::pair<double, string>> keys; // (time, colorspace), in time order; empty if not animated
    string value; // at the time of the change
};

// The value of a string param's key at time t (or, if t is not a key time, the value it has from time t on). Natron
// reads a string param at the frame floor(time + 0.5) (OfxStringInstance::get()), but sets keys at any time (e.g.
// from a script): there, a key at a fractional time is read at the next frame (it can't be read if another key is at
// or before that frame), except the first key, whose value is also that of the param before it. Other hosts read
// it at its time.
string
stringAtKeyTime(StringParam* str,
                double t)
{
    double readTime = t;
    if (getImageEffectHostDescription()->isNatron) {
        const bool beforeKeys = (numKeys(str) == 0) || (t <= str->getKeyTime(0));
        readTime = beforeKeys ? std::floor(t) : std::ceil(t);
    }
    string v;
    str->getValueAtTime(readTime, v);

    return v;
}

// the keys of a string param (time, colorspace), in time order (see stringAtKeyTime())
std::vector<std::pair<double, string>>
getStringKeys(StringParam* str)
{
    std::vector<std::pair<double, string>> keys;
    const std::vector<double> times = getKeyTimes(str);
    for (size_t i = 0; i < times.size(); ++i) {
        keys.push_back(std::make_pair(times[i], stringAtKeyTime(str, times[i])));
    }

    return keys;
}

void
getStringAnimation(StringParam* str,
                   double time,
                   ColorspaceAnimation& a)
{
    str->getValueAtTime(time, a.value);
    a.keys = getStringKeys(str);
}

// the colorspaces of the choice's keys and value (those of the menu entries; a key without a menu entry is skipped)
void
getChoiceAnimation(const OCIO::ConstConfigRcPtr& config,
                   ChoiceParam* choice,
                   double time,
                   ColorspaceAnimation& a)
{
    a.keys.clear();
    int index;
    choice->getValueAtTime(time, index);
    a.value = colorspaceOfIndex(config, index);
    const std::vector<double> times = getKeyTimes(choice);
    for (size_t i = 0; i < times.size(); ++i) {
        choice->getValueAtTime(times[i], index);
        const string v = colorspaceOfIndex(config, index);
        if (!v.empty()) {
            a.keys.push_back(std::make_pair(times[i], v));
        }
    }
}

// true if two colorspaces are the same, or (if byMenu) have the same menu entry (e.g. the role "scene_linear" and its
// colorspace "linear": the menu entries have canonical names)
bool
sameColorspace(const OCIO::ConstConfigRcPtr& config,
               const string& a,
               const string& b,
               bool byMenu)
{
    if (a == b) {
        return true;
    }
    if (!byMenu) {
        return false;
    }
    const int index = colorspaceIndex(config, a);

    return (index >= 0) && (index == colorspaceIndex(config, b));
}

// Give the string param the keys of target (time, colorspace), or the colorspace value if target has no keys (it then
// has no keys). If deleteOthers is set, its keys at other times are deleted, except, if fromMenu is set (target comes
// from the menu), those whose colorspace has no menu entry (and thus no key in the menu); fromMenu also compares the
// colorspaces by menu entry. The keys are deleted first, then set in time order: a key that is not in the param or
// whose colorspace differs, and every key after the first key that is added or deleted, with the string it had (read
// before any change; a key whose new colorspace is the same keeps its name, e.g. "x1" is not replaced by its role
// "scene_linear"). In Natron before 2.6 (without NatronGitHub/Natron#1291), a key of a string param holds its index in
// the param's keys sorted by time (which indexes their strings), which is not updated when an earlier key is added or
// deleted, so that the later keys then have other strings (NatronGitHub/Natron#1290): setting them again, in time
// order, fixes them. Elsewhere, this gives them their own values. In an edit block (Swap Colorspaces), Natron's undo
// command records every key that a plug-in sets: from Natron 2.6, a key set to its own string is a modification if it
// is the first or the last key and its neighbour has another string (the curve's derivatives are compared too), which
// undo sets back to the same string; without it, such a key is recorded as added, and undo removes it.
void
applyStringKeys(const OCIO::ConstConfigRcPtr& config,
                StringParam* str,
                const ColorspaceAnimation& target,
                bool deleteOthers,
                bool fromMenu)
{
    ColorspaceAnimation old;
    old.keys = getStringKeys(str);
    if (target.keys.empty()) {
        if (!old.keys.empty()) {
            str->deleteAllKeys();
        }
        string value;
        str->getValue(value);
        if (!sameColorspace(config, value, target.value, fromMenu)) {
            str->setValue(target.value);
        }

        return;
    }
    // the keys of the param after the change, and the time of the first key that is added or deleted
    std::map<double, string> keys;
    double firstChange = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < old.keys.size(); ++i) {
        keys[old.keys[i].first] = old.keys[i].second;
    }
    // The time of the param's key that each key of target sets: the key at the same time, else, if deleteOthers is not
    // set, the nearest key less than 1e-6 away that no other key of target sets (a new key there would be hidden from
    // GenericOCIO: Natron's OFX key list only reports the first of the keys less than 1e-6 apart), else its own time.
    // If deleteOthers is set (target is the menu, whose reported keys are at least 1e-6 apart), the param's other keys
    // are deleted, including those less than 1e-6 from a key of target that have no menu entry.
    std::vector<double> times(target.keys.size());
    std::vector<bool> oldUsed(old.keys.size(), false);
    std::vector<bool> matched(target.keys.size(), false);
    for (size_t j = 0; j < target.keys.size(); ++j) {
        times[j] = target.keys[j].first;
        for (size_t i = 0; i < old.keys.size(); ++i) {
            if (!oldUsed[i] && (old.keys[i].first == times[j])) {
                oldUsed[i] = true;
                matched[j] = true;
                break;
            }
        }
    }
    for (size_t j = 0; !deleteOthers && (j < target.keys.size()); ++j) {
        int nearest = -1;
        for (size_t i = 0; !matched[j] && (i < old.keys.size()); ++i) {
            const double d = std::fabs(old.keys[i].first - times[j]);
            if (!oldUsed[i] && (d < 1e-6) && ((nearest < 0) || (d < std::fabs(old.keys[nearest].first - times[j])))) {
                nearest = (int)i;
            }
        }
        if (nearest >= 0) {
            oldUsed[nearest] = true;
            times[j] = old.keys[nearest].first;
        }
    }
    if (deleteOthers) {
        for (size_t i = old.keys.size(); i > 0; --i) {
            const double t = old.keys[i - 1].first;
            bool nearTarget = false;
            for (size_t j = 0; j < times.size(); ++j) {
                nearTarget = nearTarget || (std::fabs(times[j] - t) < 1e-6);
            }
            if (oldUsed[i - 1] || (fromMenu && !nearTarget && (colorspaceIndex(config, old.keys[i - 1].second) < 0))) {
                continue;
            }
            str->deleteKeyAtTime(t);
            keys.erase(t);
            firstChange = std::min(firstChange, t);
        }
        // (a key less than 1e-6 after a deleted one is then reported: delete it too)
        for (int pass = 0; pass < 100; ++pass) {
            const std::vector<std::pair<double, string>> now = getStringKeys(str);
            bool deleted = false;
            for (size_t i = now.size(); i > 0; --i) {
                const double t = now[i - 1].first;
                if ((keys.find(t) != keys.end()) || containsTime(times, t)) {
                    continue;
                }
                bool nearTarget = false;
                for (size_t j = 0; j < times.size(); ++j) {
                    nearTarget = nearTarget || (std::fabs(times[j] - t) < 1e-6);
                }
                if (fromMenu && !nearTarget && (colorspaceIndex(config, now[i - 1].second) < 0)) {
                    keys[t] = now[i - 1].second;
                    continue;
                }
                str->deleteKeyAtTime(t);
                firstChange = std::min(firstChange, t);
                deleted = true;
            }
            if (!deleted) {
                break;
            }
        }
    }
    std::map<double, string> before = keys;
    for (size_t j = 0; j < target.keys.size(); ++j) {
        const double t = times[j];
        if (before.find(t) == before.end()) {
            firstChange = std::min(firstChange, t);
        }
        keys[t] = target.keys[j].second;
    }
    for (std::map<double, string>::const_iterator it = keys.begin(); it != keys.end(); ++it) {
        std::map<double, string>::const_iterator itOld = before.find(it->first);
        const bool same = (itOld != before.end()) && sameColorspace(config, itOld->second, it->second, fromMenu);
        if (same && (it->first < firstChange)) {
            continue;
        }
        str->setValueAtTime(it->first, same ? itOld->second : it->second);
    }
}

// Give the choice menu the colorspaces of a: its keys (only those that differ are set, and the others are deleted),
// or its value if it has none. A colorspace that has no menu entry is
// skipped: colorspaceCheck() then shows the string param instead of the menu.
void
setChoiceAnimation(const OCIO::ConstConfigRcPtr& config,
                   ChoiceParam* choice,
                   const ColorspaceAnimation& a,
                   bool animates)
{
    if (animates && !a.keys.empty()) {
        // (the menu gets the exact key times of a: its keys at other times, even less than 1e-6 away, are deleted
        // first)
        std::vector<double> times;
        for (size_t i = 0; i < a.keys.size(); ++i) {
            if (colorspaceIndex(config, a.keys[i].second) >= 0) {
                times.push_back(a.keys[i].first);
            }
        }
        if (!times.empty()) {
            // (a key less than 1e-6 after a deleted one is then reported: delete it too, until the menu has no key at
            // other times)
            std::vector<double> oldTimes;
            for (int pass = 0; pass < 100; ++pass) {
                oldTimes = getKeyTimes(choice);
                bool deleted = false;
                for (size_t i = oldTimes.size(); i > 0; --i) {
                    if (std::find(times.begin(), times.end(), oldTimes[i - 1]) == times.end()) {
                        choice->deleteKeyAtTime(oldTimes[i - 1]);
                        deleted = true;
                    }
                }
                if (!deleted) {
                    break;
                }
            }
            for (size_t i = 0; i < a.keys.size(); ++i) {
                const double t = a.keys[i].first;
                const int index = colorspaceIndex(config, a.keys[i].second);
                if (index < 0) {
                    continue;
                }
                if (std::find(oldTimes.begin(), oldTimes.end(), t) != oldTimes.end()) {
                    int old;
                    choice->getValueAtTime(t, old);
                    if (old == index) {
                        continue;
                    }
                }
                choice->setValueAtTime(t, index);
            }

            return;
        }
    }
    if (numKeys(choice) > 0) {
        choice->deleteAllKeys();
    }
    const int index = colorspaceIndex(config, a.value);
    if (index < 0) {
        return;
    }
    int old;
    choice->getValue(old);
    if (old != index) {
        choice->setValue(index);
    }
}
} // namespace
#endif // ifdef OFX_IO_USING_OCIO

#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
GenericOCIO::Side*
GenericOCIO::sideOfParam(const string& paramName,
                         bool* isChoice)
{
    for (int i = 0; i < 2; ++i) {
        if (!_sides[i].str) {
            continue;
        }
        if (paramName == (i ? kOCIOParamOutputSpace : kOCIOParamInputSpace)) {
            *isChoice = false;

            return &_sides[i];
        }
        if (paramName == (i ? kOCIOParamOutputSpaceChoice : kOCIOParamInputSpaceChoice)) {
            *isChoice = true;

            return &_sides[i];
        }
    }

    return NULL;
}

bool
GenericOCIO::strHasExpression(Side& side)
{
    if ((_instanceChangedDepth > 0) && (side.strExpression >= 0)) {
        return side.strExpression != 0;
    }
    const bool e = hasExpressionKeys(side.str);
    if (_instanceChangedDepth > 0) {
        side.strExpression = e;
    }

    return e;
}

bool
GenericOCIO::choiceHasExpression(Side& side)
{
    if ((_instanceChangedDepth > 0) && (side.choiceExpression >= 0)) {
        return side.choiceExpression != 0;
    }
    const bool e = hasExpressionKeys(side.choice);
    if (_instanceChangedDepth > 0) {
        side.choiceExpression = e;
    }

    return e;
}

// In a host that animates strings and choices (Natron), the menus have the same keys as the string params, with the
// same colorspaces: the string params are rendered and saved, and the menus show them and are used to edit them.
// Else, they are not keyed together: in a host that doesn't animate strings, e.g., only the menus can have keys, and
// the string params get the colorspace of the last edit of a menu. A string param or a menu with an expression (see
// hasExpressionKeys()) is not keyed with the other.
bool
GenericOCIO::choiceMirrorsString(Side& side)
{
    const ImageEffectHostDescription* host = getImageEffectHostDescription();

    return _config && _choiceIsOk && host && host->supportsStringAnimation && host->supportsChoiceAnimation && !strHasExpression(side) && !choiceHasExpression(side);
}

// Give the menus the keys of the string params, and set _choiceAnimSynced. Only in an instance changed action (see
// InstanceChanged).
void
GenericOCIO::syncChoicesFromStrings()
{
    assert(_instanceChangedDepth > 0);
    SettingColorspaces setting(this);
    ColorspaceAnimation a;
    for (int i = 0; i < 2; ++i) {
        Side& side = _sides[i];
        if (side.str && choiceMirrorsString(side)) {
            // (without keys, the value at any time is the param's value)
            getStringAnimation(side.str, 0., a);
            setChoiceAnimation(_config, side.choice, a, true);
        }
    }
    _choiceAnimSynced = true;
}

// Show the menu (and hide the string param) if the string's colorspace at time is in the menu, and set the menu: in an
// instance changed action (see InstanceChanged), if choiceMirrorsString(), give it the string's keys; else set its
// value at time (a key if it has keys, only in an instance changed action, and if it has no expression, which would
// hide the key), only if it differs (to avoid an infinite loop on bad hosts, for examples those which don't set
// args.reason correctly). A value set outside of an instance changed action is the value that the menu shows until
// it has the string's keys (initialIndex, see Side in GenericOCIO.h).
void
GenericOCIO::colorspaceCheck(Side& side,
                             double time)
{
    StringParam* str = side.str;
    ChoiceParam* choice = side.choice;
    if (!_config) {
        return;
    }
    if (!_choiceIsOk) {
        // choice menu is dirty, only use the text entry
#ifdef OFX_OCIO_NOSECRET
        str->setEnabled(true);
        choice->setEnabled(false);
#else
        str->setIsSecretAndDisabled(false);
        choice->setIsSecretAndDisabled(true);
#endif

        return;
    }
    string name;
    str->getValueAtTime(time, name);
    const int index = colorspaceIndex(_config, name);
    const bool inInstanceChanged = (_instanceChangedDepth > 0);
    if (inInstanceChanged && _choiceAnimSynced && choiceMirrorsString(side)) {
        SettingColorspaces setting(this);
        ColorspaceAnimation a;
        getStringAnimation(str, time, a);
        setChoiceAnimation(_config, choice, a, true);
    } else if (index >= 0) {
        int indexOld;
        choice->getValueAtTime(time, indexOld);
        if (indexOld != index) {
            SettingColorspaces setting(this);
            if (numKeys(choice) == 0) {
                choice->setValue(index);
                if (!inInstanceChanged) {
                    side.initialIndex = index;
                }
            } else if (inInstanceChanged && !choiceHasExpression(side)) {
                // (setValue() would set a key at the host's current time; a key less than 1e-6 from another one would
                // be hidden from the plug-in, see applyStringKeys(): set the other one)
                const int k = choice->getKeyIndex(time, eKeySearchNear);
                choice->setValueAtTime((k >= 0) ? choice->getKeyTime(k) : time, index);
            }
        }
    }
    if (index >= 0) {
#ifdef OFX_OCIO_NOSECRET
        str->setEnabled(false);
        choice->setEnabled(true);
#else
        str->setIsSecretAndDisabled(true);
        choice->setIsSecretAndDisabled(false);
#endif
    } else {
        // the colorspace name is not valid
#ifdef OFX_OCIO_NOSECRET
        str->setEnabled(true);
        choice->setEnabled(false);
#else
        str->setIsSecretAndDisabled(false);
        choice->setIsSecretAndDisabled(true);
#endif
    }
}
#endif // defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)

void
GenericOCIO::apply(double time,
                   const OfxRectI& renderWindow,
                   const OfxPointD& renderScale,
                   Image* img)
{
    assert(_created);
#ifdef OFX_IO_USING_OCIO
    BitDepthEnum bitDepth = img->getPixelDepth();
    if (bitDepth != eBitDepthFloat) {
        throw std::runtime_error("OCIO: invalid pixel depth (only float is supported)");
    }

    apply(time, renderWindow, renderScale, (float*)img->getPixelData(), img->getBounds(), img->getPixelComponents(), img->getPixelComponentCount(), img->getRowBytes());
#endif
}

#ifdef OFX_IO_USING_OCIO
OCIO_NAMESPACE::ConstProcessorRcPtr
GenericOCIO::getProcessor() const
{
    AutoMutex guard(_procMutex);

    return _proc;
};
void
GenericOCIO::setValues(const string& inputSpace,
                       const string& outputSpace)
{
    return setValues(_config->getCurrentContext(), inputSpace.c_str(), outputSpace.c_str());
}

void
GenericOCIO::setValues(const OCIO::ConstContextRcPtr& context,
                       const string& inputSpace,
                       const string& outputSpace)
{
    AutoMutex guard(_procMutex);

    if (!_proc || (context != _procContext) || (inputSpace != _procInputSpace) || (outputSpace != _procOutputSpace)) {
        _procContext = context;
        _procInputSpace = inputSpace;
        _procOutputSpace = outputSpace;
        _proc = _config->getProcessor(context, inputSpace.c_str(), outputSpace.c_str());
    }
}

void
OCIOProcessor::multiThreadProcessImages(const OfxRectI& renderWindow, const OfxPointD& renderScale)
{
    unused(renderScale);
    assert(_dstBounds.x1 <= renderWindow.x1 && renderWindow.x1 <= renderWindow.x2 && renderWindow.x2 <= _dstBounds.x2);
    assert(_dstBounds.y1 <= renderWindow.y1 && renderWindow.y1 <= renderWindow.y2 && renderWindow.y2 <= _dstBounds.y2);
    // Ensure there are pixels to render otherwise OCIO::PackedImageDesc will throw an exception.
    if ((renderWindow.y2 <= renderWindow.y1) || (renderWindow.x2 <= renderWindow.x1)) {
        return;
    }
#ifdef OFX_IO_USING_OCIO
    if (!_proc) {
        throw std::logic_error("OCIO configuration not loaded");
    }
    int numChannels;
    int pixelBytes;
    switch (_dstPixelComponents) {
    case ePixelComponentRGBA:
        numChannels = 4;
        break;
    case ePixelComponentRGB:
        numChannels = 3;
        break;
    // case ePixelComponentAlpha: pixelBytes = 1; break;
    default:
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    pixelBytes = numChannels * sizeof(float);
    // signed: _dstRowBytes may be negative (OFX images may be stored bottom-up)
    ptrdiff_t pixelDataOffset = (ptrdiff_t)(renderWindow.y1 - _dstBounds.y1) * (ptrdiff_t)_dstRowBytes + (ptrdiff_t)(renderWindow.x1 - _dstBounds.x1) * pixelBytes;
    float* pix = (float*)(((char*)_dstPixelData) + pixelDataOffset); // (char*)dstImg->getPixelAddress(renderWindow.x1, renderWindow.y1);
    try {
        AutoSetAndRestoreThreadLocale locale;
        if (_proc) {
#if OCIO_VERSION_HEX >= 0x02000000
            OCIO::PackedImageDesc img(pix, renderWindow.x2 - renderWindow.x1, renderWindow.y2 - renderWindow.y1, numChannels,
                                      OCIO::BIT_DEPTH_F32, // For now, only float
                                      sizeof(float), pixelBytes, _dstRowBytes);
            OCIO::ConstCPUProcessorRcPtr cpuproc = _proc->getOptimizedCPUProcessor(OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_F32,
                                                                                   OCIO::OPTIMIZATION_DEFAULT);
            cpuproc->apply(img);
#else
            OCIO::PackedImageDesc img(pix, renderWindow.x2 - renderWindow.x1, renderWindow.y2 - renderWindow.y1, numChannels, sizeof(float), pixelBytes, _dstRowBytes);
            _proc->apply(img);
#endif
        }
    } catch (OCIO::Exception& e) {
        _instance->setPersistentMessage(Message::eMessageError, "", string("OpenColorIO error: ") + e.what());
        throw std::runtime_error(string("OpenColorIO error: ") + e.what());
    }
#endif
}

#endif // OFX_IO_USING_OCIO

#ifdef OFX_IO_USING_OCIO
OCIO::ConstProcessorRcPtr
GenericOCIO::getOrCreateProcessor(double time)
{
    if (!_config) {
        return OCIO::ConstProcessorRcPtr();
    }
    string inputSpace;
    string outputSpace;
    getProcessingColorspacesAtTime(time, inputSpace, outputSpace);
    OCIO::ConstContextRcPtr context = getLocalContext(time); //_config->getCurrentContext();
    setValues(context, inputSpace, outputSpace);

    return getProcessor();
}

#endif // OFX_IO_USING_OCIO

void
GenericOCIO::apply(double time,
                   const OfxRectI& renderWindow,
                   const OfxPointD& renderScale,
                   float* pixelData,
                   const OfxRectI& bounds,
                   PixelComponentEnum pixelComponents,
                   int pixelComponentCount,
                   int rowBytes)
{
    assert(_created);
#ifdef OFX_IO_USING_OCIO

    if (!_created) {
        return;
    }

    if (isIdentity(time)) {
        return;
    }
    // are we in the image bounds
    if ((renderWindow.x1 < bounds.x1) || (renderWindow.x1 >= bounds.x2) || (renderWindow.y1 < bounds.y1) || (renderWindow.y1 >= bounds.y2) || (renderWindow.x2 <= bounds.x1) || (renderWindow.x2 > bounds.x2) || (renderWindow.y2 <= bounds.y1) || (renderWindow.y2 > bounds.y2)) {
        _parent->setPersistentMessage(Message::eMessageError, "", "OCIO: render window outside of image bounds");
        throwSuiteStatusException(kOfxStatFailed);
    }
    if ((pixelComponents != ePixelComponentRGBA) && (pixelComponents != ePixelComponentRGB)) {
        _parent->setPersistentMessage(Message::eMessageError, "", "OCIO: invalid components (only RGB and RGBA are supported)");
        throwSuiteStatusException(kOfxStatFailed);
    }

    OCIO::ConstProcessorRcPtr proc = getOrCreateProcessor(time);
    if (!proc) {
        _parent->setPersistentMessage(Message::eMessageError, "", "Cannot create OCIO processor");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    OCIOProcessor processor(*_parent);
    // set the images
    processor.setDstImg(pixelData, bounds, pixelComponents, pixelComponentCount, eBitDepthFloat, rowBytes);

    processor.setProcessor(proc);

    // set the render window
    processor.setRenderWindow(renderWindow, renderScale);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
#endif
}

#ifdef OFX_IO_USING_OCIO
// set a colorspace string param at time: a key if it has keys (setValue() would set it at the host's current time)
static void
setColorspaceAtTime(StringParam* str,
                    double time,
                    const string& colorspace)
{
    if (numKeys(str) > 0) {
        ColorspaceAnimation a;
        a.keys.push_back(std::make_pair(time, colorspace));
        applyStringKeys(OCIO::ConstConfigRcPtr(), str, a, false, false);
    } else {
        str->setValue(colorspace);
    }
}
#endif

#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
// Set the string param of a side from its menu, after a user edit of the menu at time.
// If choiceMirrorsString() (Natron), the string param gets all the keys of the menu, or its value if it has none.
// OFX has no action for the deletion or the move of keys, and Natron reports them as user edits of the param at
// various times: deleting a key (or several, one by one), at the time of each key; moving keys, at the current time
// (curve editor) or at the new time of each key (dope sheet); pasting keys, removing the animation, restoring the
// default value, at the current time. It also sets a key at the current time before reporting a user edit of a
// param that has keys. So, whatever the edit, the menu's keys are what the user wants, and a menu without keys means
// that the user removed its animation: the string param's keys are deleted. A key of the string param whose
// colorspace has no menu entry (and thus no key in the menu) is kept.
// This needs the menu to have the string's keys before the edit (_choiceAnimSynced, see Side in GenericOCIO.h). If
// the user edits a menu before that, its keys come from that edit: the string param gets them, and keeps its other
// keys, but a key with the value initialIndex (e.g. Set Key on the menu, or a pasted key with that value) gets the
// string's colorspace at its time, which the menu should have shown; if the menu has no keys, the string param gets
// the menu's colorspace at time (a key, if it has keys), unless it is initialIndex. So Restore default (reported as a
// user edit, like an edit of the value) then sets a key with the default value at the current time, unless it is
// initialIndex, where once the menus have the strings' keys it removes the animation.
// Else, the string param gets the menu's colorspace at time: a key, if it has keys and the host animates strings,
// else its value; e.g. if the menu has an expression (see hasExpressionKeys()), whose changes Natron reports as user
// edits (but not the setting of the expression, NatronGitHub/Natron#1336, so that the string param doesn't get its
// value until then). A string param with an expression is not set.
void
GenericOCIO::changedChoice(Side& side,
                           double time)
{
    StringParam* str = side.str;
    ChoiceParam* choice = side.choice;
    int index;
    choice->getValueAtTime(time, index);
    const string colorspace = colorspaceOfIndex(_config, index);
    if (colorspace.empty() || !_choiceIsOk) {
        return;
    }
    SettingColorspaces setting(this);
    const bool mirror = choiceMirrorsString(side);
    if (strHasExpression(side)) {
        // the string param's value is that of its expression: setting it would only add keys under the expression
        // (colorspaceCheck() then gives the menu the expression's value at that time)
    } else if (mirror && !_choiceAnimSynced) {
        ColorspaceAnimation a;
        getChoiceAnimation(_config, choice, time, a);
        ColorspaceAnimation target;
        if (a.keys.empty()) {
            string old;
            str->getValueAtTime(time, old);
            if ((index != side.initialIndex) && (colorspaceIndex(_config, old) != index)) {
                if (numKeys(str) > 0) {
                    target.keys.push_back(std::make_pair(time, colorspace));
                } else {
                    target.value = colorspace;
                }
            } else {
                str->getValue(target.value);
            }
        }
        for (size_t i = 0; i < a.keys.size(); ++i) {
            const double t = a.keys[i].first;
            if (colorspaceIndex(_config, a.keys[i].second) == side.initialIndex) {
                // the colorspace that the menu should have shown at that time
                target.keys.push_back(std::make_pair(t, stringAtKeyTime(str, t)));
            } else {
                target.keys.push_back(a.keys[i]);
            }
        }
        if (target.keys.empty() && (numKeys(str) > 0)) {
            // (nothing changed)
        } else {
            applyStringKeys(_config, str, target, false, true);
        }
    } else if (mirror) {
        ColorspaceAnimation a;
        getChoiceAnimation(_config, choice, time, a);
        applyStringKeys(_config, str, a, true, true);
    } else {
        string old;
        str->getValueAtTime(time, old);
        // (the colorspaces are compared by menu entry, to avoid an infinite loop on bad hosts, for examples those
        // which don't set args.reason correctly)
        if (colorspaceIndex(_config, old) != index) {
            setColorspaceAtTime(str, time, colorspace);
        }
    }
    if (!_choiceAnimSynced) {
        syncChoicesFromStrings();
    }
    refreshInputAndOutputState(time);
}
#endif

#if defined(OFX_IO_USING_OCIO) && defined(OFX_OCIO_CHOICE)
// true if a colorspace string param that changed at time may be canonicalized, or reset to a valid colorspace,
// at that time: if it has no keys, or a key at that time (else, e.g. after the deletion of a key, reported at the
// time of the deleted key, this would set a key again)
static bool
canFixColorspaceAtTime(StringParam* str,
                       double time)
{
    return (numKeys(str) == 0) || (str->getKeyIndex(time, eKeySearchNear) >= 0);
}

// Fix a colorspace string param that changed at time (if canFixColorspaceAtTime()): if canonicalize is set, give it
// the canonical name of its colorspace; if fallback is set and its colorspace is not in the config, give it the
// default colorspace (or the first one), with a warning if warn is set.
void
GenericOCIO::fixColorspaceAtTime(Side& side,
                                 double time,
                                 bool canonicalize,
                                 bool fallback,
                                 bool warn)
{
    StringParam* str = side.str;
    if (!canFixColorspaceAtTime(str, time) || strHasExpression(side)) {
        // (the value of a string param with an expression is that of the expression: setting it would add a key)
        return;
    }
    string colorspace;
    str->getValueAtTime(time, colorspace);
    if (canonicalize) {
        const string canonical = canonicalizeColorSpace(_config, colorspace);
        if (canonical != colorspace) {
            setColorspaceAtTime(str, time, canonical);
            colorspace = canonical;
        }
    }
    if (!fallback || !_config || (_config->getIndexForColorSpace(colorspace.c_str()) >= 0)) {
        return;
    }
    if (warn) {
        _parent->sendMessage(Message::eMessageWarning, "", string("Unknown OCIO colorspace \"") + colorspace + "\"");
    }
    OCIO::ConstColorSpaceRcPtr cs = _config->getColorSpace(OCIO::ROLE_DEFAULT);
    if (!cs) {
        // no default colorspace, fallback to the first one
        cs = _config->getColorSpace(_config->getColorSpaceNameByIndex(0));
    }
    setColorspaceAtTime(str, time, cs ? string(cs->getName()) : string(OCIO::ROLE_DEFAULT));
}
#endif

void
GenericOCIO::changedParam(const InstanceChangedArgs& args,
                          const string& paramName)
{
    assert(_created);
    InstanceChanged instanceChanged(this);
#ifdef OFX_IO_USING_OCIO
    if (_settingColorspaces && ((paramName == kOCIOParamInputSpace) || (paramName == kOCIOParamOutputSpace)
#ifdef OFX_OCIO_CHOICE
                                || (paramName == kOCIOParamInputSpaceChoice) || (paramName == kOCIOParamOutputSpaceChoice)
#endif
                                    )) {
        // GenericOCIO is setting them (see SettingColorspaces)
        return;
    }
#ifdef OFX_OCIO_CHOICE
    if (!_choiceAnimSynced && (paramName != kOCIOParamInputSpaceChoice) && (paramName != kOCIOParamOutputSpaceChoice)) {
        // the first instance changed action after loading a project: give the menus the keys of the string params
        // (changedChoice() does it after an edit of a menu)
        syncChoicesFromStrings();
    }
#endif
    if ((paramName == kOCIOParamConfigFile) && (args.reason != eChangeTime)) {
        SettingColorspaces setting(this);
        // must clear persistent message, or render() is not called by Nuke after an error
        _parent->clearPersistentMessage();
        // compute canonical inputSpace and outputSpace before changing the config,
        // if different from inputSpace and outputSpace they must be set to the canonical value after changing ocio config
        for (int i = 0; i < 2; ++i) {
            if (_sides[i].str) {
                fixColorspaceAtTime(_sides[i], args.time, true, false, false);
            }
        }

        loadConfig(); // re-load the new OCIO config
        // if inputspace or outputspace are not valid in the new config, reset them to "default"
        for (int i = 0; i < 2; ++i) {
            if (_sides[i].str) {
                fixColorspaceAtTime(_sides[i], args.time, false, true, false);
            }
        }
        refreshInputAndOutputState(args.time);

        if (!_config && (args.reason == eChangeUserEdit)) {
            string filename;
            _ocioConfigFile->getValue(filename);
            _parent->sendMessage(Message::eMessageError, "", string("Cannot load OCIO config file \"") + filename + '"');
        }
    } else if ((paramName == kOCIOHelpButton) || (paramName == kOCIOHelpLooksButton) || (paramName == kOCIOHelpDisplaysButton)) {
        string msg = "OpenColorIO Help\n"
                     "The OCIO configuration file can be set using the \"OCIO\" environment variable, which should contain the full path to the .ocio file.\n"
                     "OpenColorIO version (compiled with / running with): " OCIO_VERSION "/";
        msg += OCIO::GetVersion();
        msg += '\n';
        if (_config) {
            string configdesc = _config->getDescription();
            configdesc = whitespacify(trim(configdesc));
            if (configdesc.size() > 0) {
                msg += "\nThis OCIO configuration is ";
                msg += configdesc;
                msg += '\n';
            }
            msg += '\n';
            if (paramName == kOCIOHelpLooksButton) {
                msg += (_config->getNumLooks() <= 0 ? "No look available in this OCIO configuration.\n" : "Available looks in this OCIO Configuration (applied in the given colorspace):\n");
                for (int i = 0; i < _config->getNumLooks(); ++i) {
                    const char* lkname = _config->getLookNameByIndex(i);
                    OCIO::ConstLookRcPtr lk = _config->getLook(lkname);
                    msg += "- ";
                    msg += lkname;
                    string lkspace = lk->getProcessSpace();
                    msg += " (" + lkspace + ")\n";
                }
                msg += '\n';
            }
            if (paramName == kOCIOHelpDisplaysButton) {
                if (_config->getNumDisplays() <= 0) {
                    msg += "No display available in this OCIO configuration.\n";
                } else {
                    msg += "Available displays and views in this OCIO Configuration:\n";
                    string defaultdisplay = _config->getDefaultDisplay();
                    for (int i = 0; i < _config->getNumDisplays(); ++i) {
                        const char* display = _config->getDisplay(i);
                        msg += "- ";
                        msg += display;
                        if (display == defaultdisplay) {
                            msg += " (default)";
                        }
                        int numViews = _config->getNumViews(display);
                        if (numViews <= 0) {
                            msg += ", no view available.\n";
                        } else {
                            msg += ", views: ";
                            string defaultview = _config->getDefaultView(display);
                            for (int j = 0; j < numViews; ++j) {
                                const char* view = _config->getView(display, j);
                                msg += view;
                                if (view == defaultview) {
                                    msg += " (default)";
                                }
                                if (j < numViews - 1) {
                                    msg += ", ";
                                }
                            }
                            msg += '\n';
                        }
                    }
                }
                msg += '\n';
            }
            msg += "Available colorspaces in this OCIO Configuration:\n";
            int defaultcs = _config->getIndexForColorSpace(OCIO::ROLE_DEFAULT);
            int referencecs = _config->getIndexForColorSpace(OCIO::ROLE_REFERENCE);
            int datacs = _config->getIndexForColorSpace(OCIO::ROLE_DATA);
            int colorpickingcs = _config->getIndexForColorSpace(OCIO::ROLE_COLOR_PICKING);
            int scenelinearcs = _config->getIndexForColorSpace(OCIO::ROLE_SCENE_LINEAR);
            int compositinglogcs = _config->getIndexForColorSpace(OCIO::ROLE_COMPOSITING_LOG);
            int colortimingcs = _config->getIndexForColorSpace(OCIO::ROLE_COLOR_TIMING);
            int texturepaintcs = _config->getIndexForColorSpace(OCIO::ROLE_TEXTURE_PAINT);
            int mattepaintcs = _config->getIndexForColorSpace(OCIO::ROLE_MATTE_PAINT);

            for (int i = 0; i < _config->getNumColorSpaces(); ++i) {
                const char* csname = _config->getColorSpaceNameByIndex(i);
                ;
                OCIO::ConstColorSpaceRcPtr cs = _config->getColorSpace(csname);
                msg += "- ";
                msg += csname;
                bool first = true;
                // int roles = 0;
                if (i == defaultcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_DEFAULT;
                    first = false;
                    //++roles;
                }
                if (i == referencecs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_REFERENCE;
                    first = false;
                    //++roles;
                }
                if (i == datacs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_DATA;
                    first = false;
                    //++roles;
                }
                if (i == colorpickingcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_COLOR_PICKING;
                    first = false;
                    //++roles;
                }
                if (i == scenelinearcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_SCENE_LINEAR;
                    first = false;
                    //++roles;
                }
                if (i == compositinglogcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_COMPOSITING_LOG;
                    first = false;
                    //++roles;
                }
                if (i == colortimingcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_COLOR_TIMING;
                    first = false;
                    //++roles;
                }
                if (i == texturepaintcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_TEXTURE_PAINT;
                    first = false;
                    //++roles;
                }
                if (i == mattepaintcs) {
                    msg += first ? " (" : ", ";
                    msg += OCIO::ROLE_MATTE_PAINT;
                    first = false;
                    //++roles;
                }
                if (!first /*&& roles > 0*/) {
                    msg += ')';
                }
                string csdesc = cs ? cs->getDescription() : "(no colorspace)";
                csdesc = whitespacify(trim(csdesc));
                if (!csdesc.empty()) {
                    msg += ": ";
                    msg += csdesc;
                    msg += '\n';
                } else {
                    msg += '\n';
                }
            }
        }
        _parent->sendMessage(Message::eMessageMessage, "", msg);
    } else if (!_config) {
        // the other parameters assume there is a valid config
        return;
    }
#ifdef OFX_OCIO_CHOICE
    else {
        bool isChoice = false;
        Side* side = sideOfParam(paramName, &isChoice);
        if (!side) {
            return;
        }
        if (!isChoice) {
            SettingColorspaces setting(this);
            if (args.reason == eChangeUserEdit) {
                // if the colorspace is not a valid one, reset it to the default one (first, canonicalize it)
                fixColorspaceAtTime(*side, args.time, true, true, true);
            }
            colorspaceCheck(*side, args.time);
        } else if (args.reason == eChangeUserEdit) {
            changedChoice(*side, args.time);
        }
    }
#endif // OFX_OCIO_CHOICE

#endif // ifdef OFX_IO_USING_OCIO
} // GenericOCIO::changedParam

#ifdef OFX_IO_USING_OCIO
void
GenericOCIO::getInputColorspaceDefault(string& v) const
{
    assert(_inputSpace);
    _inputSpace->getDefault(v);
}

void
GenericOCIO::getInputColorspace(string& v) const
{
    assert(_inputSpace);
    _inputSpace->getValue(v);
}

void
GenericOCIO::getInputColorspaceAtTime(double time,
                                      string& v) const
{
    assert(_inputSpace);
    _inputSpace->getValueAtTime(time, v);
}

void
GenericOCIO::getOutputColorspaceDefault(string& v) const
{
    assert(_outputSpace);
    _outputSpace->getDefault(v);
}

void
GenericOCIO::getOutputColorspace(string& v) const
{
    assert(_outputSpace);
    _outputSpace->getValue(v);
}

void
GenericOCIO::getOutputColorspaceAtTime(double time,
                                       string& v) const
{
    assert(_outputSpace);
    _outputSpace->getValueAtTime(time, v);
}

#endif

bool
GenericOCIO::hasColorspace(const char* name) const
{
#ifdef OFX_IO_USING_OCIO

    return _config && (bool)_config->getColorSpace(name);
#else

    return false;
#endif
}

void
GenericOCIO::setInputColorspace(const char* name)
{
#ifdef OFX_IO_USING_OCIO
    assert(_inputSpace);
    // (a colorspace that has keys was set by the user: keep it. Also, setValue() would set a key at the host's
    // current time.)
    if (numKeys(_inputSpace) == 0) {
        _inputSpace->setValue(name);
    }
#else
    (void)name;
#endif
}

void
GenericOCIO::setOutputColorspace(const char* name)
{
#ifdef OFX_IO_USING_OCIO
    assert(_outputSpace);
    // (a colorspace that has keys was set by the user: keep it. Also, setValue() would set a key at the host's
    // current time.)
    if (numKeys(_outputSpace) == 0) {
        _outputSpace->setValue(name);
    }
#else
    (void)name;
#endif
}

void
GenericOCIO::getProcessingColorspacesAtTime(double time,
                                            string& inputSpace,
                                            string& outputSpace) const
{
#ifdef OFX_IO_USING_OCIO
    getInputColorspaceAtTime(time, inputSpace);
    getOutputColorspaceAtTime(time, outputSpace);
    if (_inverse && _inverse->getValueAtTime(time)) {
        std::swap(inputSpace, outputSpace);
    }
#else
    // without OpenColorIO, there are no colorspaces (getInputColorspaceAtTime() is not defined)
    (void)time;
    inputSpace.clear();
    outputSpace.clear();
#endif
}

// Swap the input and output colorspaces: their keys, if they have any, else their values at the given time. The string
// params, which are rendered and saved, get the colorspaces of the other side. If choiceMirrorsString() (Natron), the
// choice menus then get the keys of the string params. Else, in a host that doesn't animate strings, only the menus may
// have keys: they get the keys of the other side's menu, and the string params the other side's colorspace at the given
// time. No key of the string params is deleted, since Natron's undo doesn't restore the keys that a plug-in deletes: if
// a side has keys, both sides get keys at the times of the keys of both sides (string and choice values are held from a
// key to the next one, so that this is the other side's animation), and only the keys that differ are set. In Natron
// with the fix of NatronGitHub/Natron#1335 (NatronGitHub/Natron#1341), the undo of the swap's edit block then removes
// the keys that the swap added and restores those it changed, which restores the animation of a side that had keys
// (without it, e.g. in the current RB-2.6, Natron ends the edit block when the instance changed action of the swap's
// first change returns, so that its undo only undoes that change, e.g. the input's first key, and the swap's other
// changes are not undoable). That of a side that had no keys is not restored: when undo removes its last key, Natron
// gives it the value of that key, i.e. the other side's colorspace (before the swap) at the time of its last key, not
// its value before the swap. The menus are not undone either (their edits are not undoable), but they get the keys of
// the string params again at the next instance changed action of a string param. A string param with an expression (see
// hasExpressionKeys()) has no real keys: then only the values at the given time are swapped, and a side with an
// expression is not set (its value is that of the expression). This is only right in Natron 2.6 and later, which have
// NatronGitHub/Natron#1291 (the fix of NatronGitHub/Natron#1290): before, a key of a string param holds its index in
// the param's keys sorted by time, which is not updated when an earlier key is added, so that later keys may then have
// other strings (applyStringKeys() sets them again), and its undo removes the keys that were changed (and the first and
// last keys that were set again to their own strings, see applyStringKeys()), not only those that were added, so that
// the animation is lost.
void
GenericOCIO::swapColorspaces(double time)
{
#ifdef OFX_IO_USING_OCIO
    if (!_inputSpace || !_outputSpace) {
        return;
    }
    // set first, so that it is still set when the edit block ends (a host may report the changes then)
    InstanceChanged instanceChanged(this);
    SettingColorspaces setting(this);
    const ImageEffectHostDescription* host = getImageEffectHostDescription();
    const bool stringAnimates = host->supportsStringAnimation;
    // read both sides before changing any
    ColorspaceAnimation input;
    ColorspaceAnimation output;
    getStringAnimation(_inputSpace, time, input);
    getStringAnimation(_outputSpace, time, output);
#ifdef OFX_OCIO_CHOICE
    const bool inputExpression = strHasExpression(_sides[0]);
    const bool outputExpression = strHasExpression(_sides[1]);
#else
    const bool inputExpression = hasExpressionKeys(_inputSpace);
    const bool outputExpression = hasExpressionKeys(_outputSpace);
#endif
    const bool expression = inputExpression || outputExpression;
    if (expression) {
        input.keys.clear();
        output.keys.clear();
    } else if (stringAnimates && (!input.keys.empty() || !output.keys.empty())) {
        // the colorspaces of both sides at the times of the keys of both sides
        std::vector<double> times;
        for (size_t i = 0; i < input.keys.size(); ++i) {
            times.push_back(input.keys[i].first);
        }
        for (size_t i = 0; i < output.keys.size(); ++i) {
            if (!containsTime(times, output.keys[i].first)) {
                times.push_back(output.keys[i].first);
            }
        }
        std::sort(times.begin(), times.end());
        input.keys.clear();
        output.keys.clear();
        for (size_t i = 0; i < times.size(); ++i) {
            input.keys.push_back(std::make_pair(times[i], stringAtKeyTime(_inputSpace, times[i])));
            output.keys.push_back(std::make_pair(times[i], stringAtKeyTime(_outputSpace, times[i])));
        }
    }
#ifdef OFX_OCIO_CHOICE
    // the choice menus are only those of the colorspaces if they were built from the current config
    const bool hasChoice = _config && _choiceIsOk;
    const bool mirror = hasChoice && host->supportsStringAnimation && host->supportsChoiceAnimation;
    ColorspaceAnimation inputChoice;
    ColorspaceAnimation outputChoice;
    if (hasChoice && !mirror) {
        getChoiceAnimation(_config, _sides[0].choice, time, inputChoice);
        getChoiceAnimation(_config, _sides[1].choice, time, outputChoice);
        // the menus may have keys, and not the strings (which have the colorspace of the last edit of a menu): the
        // strings get the colorspace of the menu at the given time
        if (input.keys.empty() && !inputChoice.keys.empty()) {
            input.value = inputChoice.value;
        }
        if (output.keys.empty() && !outputChoice.keys.empty()) {
            output.value = outputChoice.value;
        }
    }
#endif
    // one undoable change (hosts may also skip rendering the intermediate state, where both colorspaces are
    // equal, but OFX edit blocks are only defined for undo). The block is closed even if a setValue() throws,
    // and hosts without edit blocks get no block.
    struct EditBlockGuard
    {
        ImageEffect* effect;
        bool open;
        EditBlockGuard(ImageEffect* e, const char* name)
            : effect(e)
            , open(false)
        {
            try {
                effect->beginEditBlock(name);
                open = true;
            } catch (...) {
            }
        }
        ~EditBlockGuard()
        {
            if (open) {
                try {
                    effect->endEditBlock();
                } catch (...) {
                }
            }
        }
    } editBlock(_parent, "Swap Colorspaces");
    if (expression) {
        // (the value of a side with an expression is that of the expression: it is not set)
        if (!inputExpression) {
            setColorspaceAtTime(_inputSpace, time, output.value);
        }
        if (!outputExpression) {
            setColorspaceAtTime(_outputSpace, time, input.value);
        }
    } else {
        // (without string animation, the strings have no keys)
        applyStringKeys(_config, _inputSpace, output, false, false);
        applyStringKeys(_config, _outputSpace, input, false, false);
    }
#ifdef OFX_OCIO_CHOICE
    if (mirror) {
        syncChoicesFromStrings();
    } else if (hasChoice) {
        // (a colorspace that is not in the config has no menu entry: colorspaceCheck() then shows the string param)
        const bool choiceAnimates = host->supportsChoiceAnimation;
        // a menu without keys gives the other menu the value of its string at that time
        if (inputChoice.keys.empty()) {
            inputChoice.value = input.value;
        }
        if (outputChoice.keys.empty()) {
            outputChoice.value = output.value;
        }
        setChoiceAnimation(_config, _sides[0].choice, outputChoice, choiceAnimates);
        setChoiceAnimation(_config, _sides[1].choice, inputChoice, choiceAnimates);
    }
#endif
    // update the state of the choice menus at that time
    refreshInputAndOutputState(time);
#else
    (void)time;
#endif
}

void
GenericOCIO::describeInContextInverseAndSwap(ImageEffectDescriptor& desc,
                                             ContextEnum /*context*/,
                                             PageParamDescriptor* page)
{
#ifdef OFX_IO_USING_OCIO
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kOCIOParamInverse);
        param->setLabel(kOCIOParamInverseLabel);
        param->setHint(kOCIOParamInverseHint);
        param->setDefault(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kOCIOParamSwapColorspaces);
        param->setLabel(kOCIOParamSwapColorspacesLabel);
        param->setHint(kOCIOParamSwapColorspacesHint);
        if (page) {
            page->addChild(*param);
        }
    }
#else
    (void)desc;
    (void)page;
#endif
}

void
GenericOCIO::purgeCaches()
{
#ifdef OFX_IO_USING_OCIO
    OCIO::ClearAllCaches();
#endif
}

void
GenericOCIO::describeInContextInput(ImageEffectDescriptor& desc,
                                    ContextEnum /*context*/,
                                    PageParamDescriptor* page,
                                    const char* inputSpaceNameDefault,
                                    const char* inputSpaceLabel)
{
#ifdef OFX_IO_USING_OCIO
    gHostIsNatron = (getImageEffectHostDescription()->isNatron);

    char* file = std::getenv("OCIO");
    AutoSetAndRestoreThreadLocale locale;
    OCIO::ConstConfigRcPtr config;
    if (file != NULL) {
        // Add choices
        try {
            config = OCIO::Config::CreateFromFile(file);
            gWasOCIOEnvVarFound = true;
        } catch (OCIO::Exception& e) {
        }
    }
    string inputSpaceName, outputSpaceName;
    if (config) {
        inputSpaceName = canonicalizeColorSpace(config, colorSpaceName(config, inputSpaceNameDefault));
    }

    ////////// OCIO config file
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamConfigFile);
        param->setLabelAndHint(kOCIOParamConfigFileLabel);
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(true);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        // the OCIO config can only be set in a portable fashion using the environment variable.
        // Nuke, for example, doesn't support changing the entries in a ChoiceParam outside of describeInContext.
        // disable it, and set the default from the env variable.
        assert(getImageEffectHostDescription());
        if (file == NULL) {
            param->setDefault("WARNING: Open an OCIO config file, or set the OCIO environment variable");
        } else if (config) {
            param->setDefault(file);
        } else {
            string s("ERROR: Invalid OCIO configuration '");
            s += file;
            s += '\'';
            param->setDefault(s);
        }
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Input Color-space
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamInputSpace);
        param->setLabelAndHint(inputSpaceLabel, kOCIOParamInputSpaceHint);
        param->setAnimates(true);
        if (config) {
            param->setDefault(inputSpaceName);
        } else {
            // param->setEnabled(false); // done in constructor
        }
        if (page) {
            page->addChild(*param);
        }
    }

#ifdef OFX_OCIO_CHOICE
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kOCIOParamInputSpaceChoice);
        param->setLabelAndHint(inputSpaceLabel, kOCIOParamInputSpaceHint);
        param->setCascading(getImageEffectHostDescription()->supportsCascadingChoices);
        if (config) {
            buildChoiceMenu(config, param, getImageEffectHostDescription()->supportsCascadingChoices, inputSpaceName);
        } else {
            // param->setEnabled(false); // done in the plugin constructor
            // param->setIsSecret(true); // done in the plugin constructor
        }
        param->setAnimates(true);
        param->setEvaluateOnChange(false); // evaluate only when the StringParam is changed
        param->setIsPersistent(false); // don't save/serialize
        if (page) {
            page->addChild(*param);
        }
    }
#endif
#endif // ifdef OFX_IO_USING_OCIO
} // GenericOCIO::describeInContextInput

void
GenericOCIO::describeInContextOutput(ImageEffectDescriptor& desc,
                                     ContextEnum /*context*/,
                                     PageParamDescriptor* page,
                                     const char* outputSpaceNameDefault,
                                     const char* outputSpaceLabel)
{
#ifdef OFX_IO_USING_OCIO
    gHostIsNatron = (getImageEffectHostDescription()->isNatron);

    char* file = std::getenv("OCIO");
    AutoSetAndRestoreThreadLocale locale;
    OCIO::ConstConfigRcPtr config;
    if (file != NULL) {
        // Add choices
        try {
            config = OCIO::Config::CreateFromFile(file);
            gWasOCIOEnvVarFound = true;
        } catch (OCIO::Exception& e) {
        }
    }
    string outputSpaceName;
    if (config) {
        outputSpaceName = canonicalizeColorSpace(config, colorSpaceName(config, outputSpaceNameDefault));
    }

    ///////////Output Color-space
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamOutputSpace);
        param->setLabelAndHint(outputSpaceLabel, kOCIOParamOutputSpaceHint);
        param->setAnimates(true);
        if (config) {
            param->setDefault(outputSpaceName);
        } else {
            // param->setEnabled(false); // done in constructor
        }
        if (page) {
            page->addChild(*param);
        }
    }

#ifdef OFX_OCIO_CHOICE
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kOCIOParamOutputSpaceChoice);
        param->setLabelAndHint(outputSpaceLabel, kOCIOParamOutputSpaceHint);
        param->setCascading(getImageEffectHostDescription()->supportsCascadingChoices);
        if (config) {
            buildChoiceMenu(config, param, getImageEffectHostDescription()->supportsCascadingChoices, outputSpaceName);
        } else {
            // param->setEnabled(false); // done in the plugin constructor
            // param->setIsSecret(true); // done in the plugin constructor
        }
        param->setAnimates(true);
        param->setEvaluateOnChange(false); // evaluate only when the StringParam is changed
        param->setIsPersistent(false); // don't save/serialize
        if (page) {
            page->addChild(*param);
        }
    }
#endif
#endif // ifdef OFX_IO_USING_OCIO
} // GenericOCIO::describeInContextOutput

void
GenericOCIO::describeInContextContext(ImageEffectDescriptor& desc,
                                      ContextEnum /*context*/,
                                      PageParamDescriptor* page)
{
#ifdef OFX_IO_USING_OCIO
    GroupParamDescriptor* group = desc.defineGroupParam(kOCIOParamContext);
    group->setLabelAndHint(kOCIOParamContextLabel, kOCIOParamContextHint);
    group->setOpen(false);

    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextKey1);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextValue1);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextKey2);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextValue2);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextKey3);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextValue3);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextKey4);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kOCIOParamContextValue4);
        param->setHint(kOCIOParamContextHint);
        param->setAnimates(true);
        param->setParent(*group);
        if (page) {
            page->addChild(*param);
        }
    }
    if (page) {
        page->addChild(*group);
    }
#endif // ifdef OFX_IO_USING_OCIO
} // GenericOCIO::describeInContextContext

// Helper class to set the C locale when doing OCIO calls.
// See https://github.com/AcademySoftwareFoundation/OpenColorIO/issues/297#issuecomment-505636123
AutoSetAndRestoreThreadLocale::AutoSetAndRestoreThreadLocale()
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    // set locale will only change locale on the current thread
    previousThreadConfig = _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);

    // get and store current locale
    ssaLocale.assign(setlocale(LC_ALL, NULL));

    // set to "C" locale
    setlocale(LC_ALL, "C");
#else
    // set to C locale, saving the old one (returned from useLocale)
    currentLocale = newlocale(LC_ALL_MASK, "C", NULL);
    oldLocale = uselocale(currentLocale);
#endif
}

AutoSetAndRestoreThreadLocale::~AutoSetAndRestoreThreadLocale()
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    // thread specific
    setlocale(LC_ALL, ssaLocale.c_str());

    // set back to global settings]
    _configthreadlocale(previousThreadConfig);
#else
    // restore the previous locale and freeing the created locale
    uselocale(oldLocale);
    freelocale(currentLocale);
#endif
}

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT
