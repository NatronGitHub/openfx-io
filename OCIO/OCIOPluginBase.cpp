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

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT