#ifndef IO_OCIOPluginBase_h
#define IO_OCIOPluginBase_h

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

    // Returns true if |paramName| is the name of a parameter that influences the
    // value of the SupportsOpenGLRender & SupportsTiles properties.
    bool paramEffectsOpenGLAndTileSupport(const std::string& paramName);

    // Sets SupportsOpenGLRender & SupportsTiles properties based on the current
    // values of premult and _enableGPU parameters.
    // Note: If |time| is not nullptr, then the parameter values are fetched for the
    // time specified by |*time|.
    void setSupportsOpenGLAndTileInfo(BooleanParam* premultParam,
                                      const double* const time);

private:
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    BooleanParam* _enableGPU;
#endif
};

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // IO_OCIOPluginBase_h