#ifndef IO_OCIOPluginBase_h
#define IO_OCIOPluginBase_h

#include "IOUtility.h"
#include "ofxsImageEffect.h"

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

class OCIOPluginBase : public ImageEffect {
protected:
    OCIOPluginBase(OfxImageEffectHandle handle);
    virtual ~OCIOPluginBase() override;
};

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // IO_OCIOPluginBase_h