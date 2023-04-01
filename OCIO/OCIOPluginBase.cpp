#include "OCIOPluginBase.h"

#include "IOUtility.h"
#include "ofxsImageEffect.h"

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
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    , _enableGPU(fetchBooleanParam(kParamEnableGPU))
#endif
{
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

bool OCIOPluginBase::paramEffectsOpenGLAndTileSupport(const std::string& paramName) {
    return paramName == kParamEnableGPU;
}

void
OCIOPluginBase::setSupportsOpenGLAndTileInfo(BooleanParam* premultParam, const double* const time)
{
    const ImageEffectHostDescription& gHostDescription = *getImageEffectHostDescription();

    // GPU rendering is wrong when (un)premult is checked
    const bool premult = time ? premultParam->getValueAtTime(*time) : premultParam->getValue();
    const bool enableGPU = time ? _enableGPU->getValueAtTime(*time) : _enableGPU->getValue();
    _enableGPU->setEnabled(gHostDescription.supportsOpenGLRender && !premult);
    const bool supportsGL = !premult && enableGPU;
    setSupportsOpenGLRender(supportsGL);
    setSupportsTiles(!supportsGL);
}

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT