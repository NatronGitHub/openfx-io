#include "ofxsImageEffect.h"
#include "OCIOColorSpace.h"
#include "OCIOFileTransform.h"

namespace OFX 
{
    namespace Plugin 
    {
        void getPluginIDs(OFX::PluginFactoryArray &ids)
        {
#ifdef OFX_IO_USING_OCIO
          getOCIOColorSpacePluginID(ids);
          getOCIOFileTransformPluginID(ids);
#endif
        }
    }
}
