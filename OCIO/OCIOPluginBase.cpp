#include "OCIOPluginBase.h"

#include "IOUtility.h"
#include "ofxsImageEffect.h"

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

OCIOPluginBase::OCIOPluginBase(OfxImageEffectHandle handle)
    : ImageEffect(handle)
{
}

OCIOPluginBase::~OCIOPluginBase() { }

void
OCIOPluginBase::setSupportsOpenGLAndTileInfo(BooleanParam* premultParam, BooleanParam* enableGPUParam, const double* const time)
{
    const ImageEffectHostDescription& gHostDescription = *getImageEffectHostDescription();

    // GPU rendering is wrong when (un)premult is checked
    const bool premult = time ? premultParam->getValueAtTime(*time) : premultParam->getValue();
    const bool enableGPU = time ? enableGPUParam->getValueAtTime(*time) : enableGPUParam->getValue();
    enableGPUParam->setEnabled(gHostDescription.supportsOpenGLRender && !premult);
    const bool supportsGL = !premult && enableGPU;
    setSupportsOpenGLRender(supportsGL);
    setSupportsTiles(!supportsGL);
}

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT