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

#ifndef IO_OCIOPluginBase_h
#define IO_OCIOPluginBase_h

#include <optional>
#include <string>

#include "IOUtility.h"
#include "ofxsImageEffect.h"

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

class OCIOPluginBase : public ImageEffect {
public:
    // Defines enableGPU param in |desc| and adds it as a child of |page|.
    static void defineEnableGPUParam(ImageEffectDescriptor& desc, PageParamDescriptor* page);

protected:
    OCIOPluginBase(OfxImageEffectHandle handle);
    virtual ~OCIOPluginBase() override;

    bool getPremultValueAtTime(double time) { return _premult->getValueAtTime(time); }
    void getPremultAndPremultChannelAtTime(double time, bool& premult, int& premultChannel);

    // Returns true if |paramName| is the name of a parameter that influences the
    // value of the SupportsOpenGLRender & SupportsTiles properties.
    bool paramEffectsOpenGLAndTileSupport(const std::string& paramName);

    // Sets SupportsOpenGLRender & SupportsTiles properties based on the
    // values of premult and enableGPU parameters.
    void setSupportsOpenGLAndTileInfo();
    void setSupportsOpenGLAndTileInfoAtTime(double time);

    void changedSrcClip(Clip* srcClip);

private:
    void setSupportsOpenGLAndTileInfo_internal(bool premult, bool enableGPU);

    BooleanParam* _premult;
    ChoiceParam* _premultChannel;

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    BooleanParam* _enableGPU;
#endif
};

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // IO_OCIOPluginBase_h
