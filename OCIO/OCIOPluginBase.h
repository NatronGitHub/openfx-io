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