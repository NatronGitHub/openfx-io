/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2023 The Natron Developers
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

#include "OCIOPluginBase.h"

#include "IOUtility.h"
#include "ofxsImageEffect.h"
#include "ofxsMaskMix.h"

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

#if defined(OFX_SUPPORTS_OPENGLRENDER)
#define kParamEnableGPU "enableGPU"
#define kParamEnableGPULabel "Enable GPU Render"
#if OCIO_VERSION_HEX >= 0x02000000
#define kParamEnableGPUHint_warn ""
#else
// OCIO1's GPU render is not accurate enough.
// see https://github.com/imageworks/OpenColorIO/issues/394
// and https://github.com/imageworks/OpenColorIO/issues/456
#define kParamEnableGPUHint_warn "Note that GPU render is not as accurate as CPU render, so this should be enabled with care.\n"
#endif
#define kParamEnableGPUHint                                                                                                                                                              \
    "Enable GPU-based OpenGL render (only available when \"(Un)premult\" is not checked).\n" kParamEnableGPUHint_warn                                                                    \
    "If the checkbox is checked but is not enabled (i.e. it cannot be unchecked), GPU render can not be enabled or disabled from the plugin and is probably part of the host options.\n" \
    "If the checkbox is not checked and is not enabled (i.e. it cannot be checked), GPU render is not available on this host."
#endif

OCIOPluginBase::OCIOPluginBase(OfxImageEffectHandle handle)
    : ImageEffect(handle)
    , _premult(fetchBooleanParam(kParamPremult))
    , _premultChannel(fetchChoiceParam(kParamPremultChannel))
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    , _enableGPU(fetchBooleanParam(kParamEnableGPU))
#endif
{
    assert(_premult && _premultChannel);
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    assert(_enableGPU);
#endif
}

OCIOPluginBase::~OCIOPluginBase() { }

// static
void
OCIOPluginBase::defineEnableGPUParam(ImageEffectDescriptor& desc, PageParamDescriptor* page)
{
    BooleanParamDescriptor* param = desc.defineBooleanParam(kParamEnableGPU);
    param->setLabel(kParamEnableGPULabel);
    param->setHint(kParamEnableGPUHint);
    const ImageEffectHostDescription& gHostDescription = *getImageEffectHostDescription();
    // Resolve advertises OpenGL support in its host description, but never calls render with OpenGL enabled
    if (gHostDescription.supportsOpenGLRender && (gHostDescription.hostName != "DaVinciResolveLite")) {
        // OCIO's GPU render is not accurate enough.
        // see https://github.com/imageworks/OpenColorIO/issues/394
        param->setDefault(/*true*/ false);
        if (gHostDescription.APIVersionMajor * 100 + gHostDescription.APIVersionMinor < 104) {
            // Switching OpenGL render from the plugin was introduced in OFX 1.4
            param->setEnabled(false);
        }
    } else {
        param->setDefault(false);
        param->setEnabled(false);
    }

    if (page) {
        page->addChild(*param);
    }
}

void
OCIOPluginBase::getPremultAndPremultChannelAtTime(double time, bool& premult, int& premultChannel)
{
    _premult->getValueAtTime(time, premult);
    _premultChannel->getValueAtTime(time, premultChannel);
}

bool
OCIOPluginBase::paramEffectsOpenGLAndTileSupport(const std::string& paramName)
{
    return paramName == kParamEnableGPU || paramName == kParamPremult;
}

void
OCIOPluginBase::setSupportsOpenGLAndTileInfo()
{
    setSupportsOpenGLAndTileInfo_internal(_premult->getValue(), _enableGPU->getValue());
}

void
OCIOPluginBase::setSupportsOpenGLAndTileInfoAtTime(double time)
{
    setSupportsOpenGLAndTileInfo_internal(_premult->getValueAtTime(time), _enableGPU->getValueAtTime(time));
}

void
OCIOPluginBase::setSupportsOpenGLAndTileInfo_internal(bool premult, bool enableGPU)
{
    _enableGPU->setEnabled(getImageEffectHostDescription()->supportsOpenGLRender && !premult);
    const bool supportsGL = !premult && enableGPU;
    setSupportsOpenGLRender(supportsGL);
    setSupportsTiles(!supportsGL);
}

void
OCIOPluginBase::changedSrcClip(Clip* srcClip)
{
    if (srcClip->getPixelComponents() != ePixelComponentRGBA) {
        _premult->setValue(false);
    } else {
        switch (srcClip->getPreMultiplication()) {
        case eImageOpaque:
            _premult->setValue(false);
            break;
        case eImagePreMultiplied:
            _premult->setValue(true);
            break;
        case eImageUnPreMultiplied:
            _premult->setValue(false);
            break;
        }
    }
}

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT
