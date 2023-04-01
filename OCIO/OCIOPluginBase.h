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

    // Sets SupportsOpenGLRender & SupportsTiles properties based on the current
    // values of premult and enableGPU parameters.
    // Note: If |time| is not nullptr, then the parameter values are fetched for the
    // time specified by |*time|.
    void setSupportsOpenGLAndTileInfo(BooleanParam* premultParam,
                                      BooleanParam* enableGPUParam,
                                      const double* const time);
};

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // IO_OCIOPluginBase_h