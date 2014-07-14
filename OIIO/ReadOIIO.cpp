/*
 OFX oiioReader plugin.
 Reads an image using the OpenImageIO library.
 
 Copyright (C) 2013 INRIA
 Author Alexandre Gauthier-Foichat alexandre.gauthier-foichat@inria.fr
 
 Redistribution and use in source and binary forms, with or without modification,
 are permitted provided that the following conditions are met:
 
 Redistributions of source code must retain the above copyright notice, this
 list of conditions and the following disclaimer.
 
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.
 
 Neither the name of the {organization} nor the names of its
 contributors may be used to endorse or promote products derived from
 this software without specific prior written permission.
 
 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 
 INRIA
 Domaine de Voluceau
 Rocquencourt - B.P. 105
 78153 Le Chesnay Cedex - France
 
 */


#include "ReadOIIO.h"
#include "GenericOCIO.h"

#include <iostream>
#include <sstream>
#include <cmath>

#include <OpenImageIO/imageio.h>
#include <OpenImageIO/imagecache.h>

#include "IOUtility.h"

OIIO_NAMESPACE_USING

#define OFX_READ_OIIO_USES_CACHE

///This has been disabled because shared caches won't work if several instances do not have the same value for
///the unassociated alpha parameter.
//#define OFX_READ_OIIO_SHARED_CACHE
#define kMetadataButtonName "showMetadata"
#define kMetadataButtonLabel "Image Info..."
#define kMetadataButtonHint "Shows information and metadata from the image at current time."

#define kParamUnassociatedAlphaName "unassociatedAlpha"
#define kParamUnassociatedAlphaLabel "Keep Unassoc. Alpha"
#define kParamUnassociatedAlphaHint "When checked, don't associate alpha (i.e. don't premultiply) if alpha is marked as unassociated in the metadata. Images which have associated alpha (i.e. are already premultiplied) are unaffected."

#define kOutputComponentsParamName "outputComponents"
#define kOutputComponentsParamLabel "Output Components"
#define kOutputComponentsParamHint "Components in the output"
#define kOutputComponentsRGBAOption "RGBA"
#define kOutputComponentsRGBOption "RGB"
#define kOutputComponentsAlphaOption "Alpha"

#define kFirstChannelParamName "firstChannel"
#define kFirstChannelParamLabel "First Channel"
#define kFirstChannelParamHint "Channel from the input file corresponding to the first component. See \"Image Info...\" for a list of image channels."

#ifdef OFX_READ_OIIO_USES_CACHE
static const bool kSupportsTiles = true;
#else
static const bool kSupportsTiles = false;
#endif

static bool gSupportsRGBA   = false;
static bool gSupportsRGB    = false;
static bool gSupportsAlpha  = false;

static OFX::PixelComponentEnum gOutputComponentsMap[4];

class ReadOIIOPlugin : public GenericReaderPlugin {

public:

    ReadOIIOPlugin(OfxImageEffectHandle handle);

    virtual ~ReadOIIOPlugin();

    virtual void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName);

    virtual void clearAnyCache();
private:

    virtual void onInputFileChanged(const std::string& filename);

    virtual bool isVideoStream(const std::string& /*filename*/) { return false; }

    virtual void decode(const std::string& filename, OfxTime time, const OfxRectI& renderWindow, float *pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int rowBytes);

    virtual bool getFrameRegionOfDefinition(const std::string& /*filename*/,OfxTime time,OfxRectD& rod,std::string& error);

    std::string metadata(const std::string& filename);

    /** @brief get the clip preferences */
    virtual void getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences) /* OVERRIDE FINAL */;

#ifdef OFX_READ_OIIO_USES_CACHE
    //// OIIO image cache
    ImageCache* _cache;
#endif
    OFX::BooleanParam* _unassociatedAlpha;
    OFX::ChoiceParam *_outputComponents;
    OFX::IntParam *_firstChannel;
};

ReadOIIOPlugin::ReadOIIOPlugin(OfxImageEffectHandle handle)
: GenericReaderPlugin(handle, kSupportsTiles)
#ifdef OFX_READ_OIIO_USES_CACHE
#  ifdef OFX_READ_OIIO_SHARED_CACHE
, _cache(ImageCache::create(true)) // shared cache
#  else
, _cache(ImageCache::create(false)) // non-shared cache
#  endif
#endif
, _unassociatedAlpha(0)
, _outputComponents(0)
, _firstChannel(0)
{
    _unassociatedAlpha = fetchBooleanParam(kParamUnassociatedAlphaName);
#ifdef OFX_READ_OIIO_USES_CACHE
    bool unassociatedAlpha;
    _unassociatedAlpha->getValue(unassociatedAlpha);
    _cache->attribute("unassociatedalpha", (int)unassociatedAlpha);
#endif
    _outputComponents = fetchChoiceParam(kOutputComponentsParamName);
    _firstChannel = fetchIntParam(kFirstChannelParamName);
}

ReadOIIOPlugin::~ReadOIIOPlugin()
{
#ifdef OFX_READ_OIIO_USES_CACHE
#  ifdef OFX_READ_OIIO_SHARED_CACHE
    ImageCache::destroy(_cache); // don't teardown if it's a shared cache
#  else
    ImageCache::destroy(_cache, true); // teardown non-shared cache
#  endif
#endif
}

void ReadOIIOPlugin::clearAnyCache()
{
#ifdef OFX_READ_OIIO_USES_CACHE
    ///flush the OIIO cache
    _cache->invalidate_all();
#endif
}

void ReadOIIOPlugin::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName)
{
    if (paramName == kMetadataButtonName) {
        std::string filename;
        getCurrentFileName(filename);
        sendMessage(OFX::Message::eMessageMessage, "", metadata(filename));
    }
#ifdef OFX_READ_OIIO_USES_CACHE
    ///This cannot be done elsewhere as the Cache::attribute function is not thread safe!
    else if (paramName == kParamUnassociatedAlphaName) {
        bool unassociatedAlpha;
        _unassociatedAlpha->getValue(unassociatedAlpha); // non-animatable
        _cache->attribute("unassociatedalpha", (int)unassociatedAlpha);
    }
#endif
    else if (paramName == kOutputComponentsParamName) {
        // set the first channel to the alpha channel if output is alpha
        int outputComponents_i;
        _outputComponents->getValue(outputComponents_i);
        OFX::PixelComponentEnum outputComponents = gOutputComponentsMap[outputComponents_i];
        if (outputComponents == OFX::ePixelComponentAlpha) {
            std::string filename;
            _fileParam->getValueAtTime(args.time, filename);
            onInputFileChanged(filename);
        }
    } else {
        GenericReaderPlugin::changedParam(args,paramName);
    }
}

void ReadOIIOPlugin::onInputFileChanged(const std::string &filename)
{
    ///uncomment to use OCIO meta-data as a hint to set the correct color-space for the file.
    
#ifdef OFX_IO_USING_OCIO
#ifdef OFX_READ_OIIO_USES_CACHE
    ImageSpec spec;
    //use the thread-safe version of get_imagespec (i.e: make a copy of the imagespec)
    if(!_cache->get_imagespec(ustring(filename), spec)){
        setPersistentMessage(OFX::Message::eMessageError, "", _cache->geterror());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
#else
    std::auto_ptr<ImageInput> img(ImageInput::open(filename));
    if (!img.get()) {
        setPersistentMessage(OFX::Message::eMessageError, "", std::string("ReadOIIO: cannot open file ") + filename);
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    const ImageSpec &spec = img->spec();
#endif
    

    ///find-out the image color-space
    const ParamValue* colorSpaceValue = spec.find_attribute("oiio:ColorSpace",TypeDesc::STRING);

    //we found a color-space hint, use it to do the color-space conversion
    const char* colorSpaceStr = NULL;
    if (colorSpaceValue) {
        colorSpaceStr = *(const char**)colorSpaceValue->data();
    } else {
        // no colorspace... we'll probably have to try something else, then.
        // we set the following defaults:
        // sRGB for 8-bit images
        // Rec709 for 10-bits, 12-bits or 16-bits integer images
        // Linear for anything else
        switch (spec.format.basetype) {
            case TypeDesc::UCHAR:
            case TypeDesc::CHAR:
                colorSpaceStr = "sRGB";
                break;
            case TypeDesc::USHORT:
            case TypeDesc::SHORT:
                colorSpaceStr = "Rec709";
                break;
            default:
                colorSpaceStr = "Linear";
                break;
        }
    }
    if (colorSpaceStr) {
        if (!strcmp(colorSpaceStr, "GammaCorrected")) {
            float gamma = spec.get_float_attribute("oiio:Gamma");
            if (std::fabs(gamma-1.8) < 0.01) {
                if (_ocio->hasColorspace("Gamma1.8")) {
                    // nuke-default
                    _ocio->setInputColorspace("Gamma1.8");
                }
            } else if (std::fabs(gamma-2.2) < 0.01) {
                if (_ocio->hasColorspace("Gamma2.2")) {
                    // nuke-default
                    _ocio->setInputColorspace("Gamma2.2");
                } else if (_ocio->hasColorspace("vd16")) {
                    // vd16 in spi-anim and spi-vfx
                    _ocio->setInputColorspace("vd16");
                } else if (_ocio->hasColorspace("sRGB")) {
                    // nuke-default
                    _ocio->setInputColorspace("sRGB");
                } else if (_ocio->hasColorspace("rrt_srgb")) {
                    // rrt_srgb in aces
                    _ocio->setInputColorspace("rrt_srgb");
                } else if (_ocio->hasColorspace("srgb8")) {
                    // srgb8 in spi-vfx
                    _ocio->setInputColorspace("srgb8");
                }
            }
        } else if(!strcmp(colorSpaceStr, "sRGB")) {
            if (_ocio->hasColorspace("sRGB")) {
                // nuke-default
                _ocio->setInputColorspace("sRGB");
            } else if (_ocio->hasColorspace("rrt_srgb")) {
                // rrt_srgb in aces
                _ocio->setInputColorspace("rrt_srgb");
            } else if (_ocio->hasColorspace("srgb8")) {
                // srgb8 in spi-vfx
                _ocio->setInputColorspace("srgb8");
            } else if (_ocio->hasColorspace("Gamma2.2")) {
                // nuke-default
                _ocio->setInputColorspace("Gamma2.2");
            } else if (_ocio->hasColorspace("vd16")) {
                // vd16 in spi-anim and spi-vfx
                _ocio->setInputColorspace("vd16");
            }
        } else if(!strcmp(colorSpaceStr, "AdobeRGB")) {
            // ???
        } else if(!strcmp(colorSpaceStr, "Rec709")) {
            if (_ocio->hasColorspace("Rec709")) {
                // nuke-default
                _ocio->setInputColorspace("Rec709");
            } else if (_ocio->hasColorspace("rrt_rec709")) {
                // rrt_rec709 in aces
                _ocio->setInputColorspace("rrt_rec709");
            } else if (_ocio->hasColorspace("hd10")) {
                // hd10 in spi-anim and spi-vfx
                _ocio->setInputColorspace("hd10");
            }
        } else if(!strcmp(colorSpaceStr, "KodakLog")) {
            if (_ocio->hasColorspace("Cineon")) {
                // Cineon in nuke-default
                _ocio->setInputColorspace("Cineon");
            } else if (_ocio->hasColorspace("lg10")) {
                // lg10 in spi-vfx
                _ocio->setInputColorspace("lg10");
            }
        } else if(!strcmp(colorSpaceStr, "Linear")) {
            _ocio->setInputColorspace("scene_linear");
            // lnf in spi-vfx
        } else if (_ocio->hasColorspace(colorSpaceStr)) {
            // maybe we're lucky
            _ocio->setInputColorspace(colorSpaceStr);
        } else {
            // unknown color-space or Linear, don't do anything
        }
    }
#ifdef OFX_READ_OIIO_USES_CACHE
#else
    img->close();
#endif
#endif
    _firstChannel->setDisplayRange(0, spec.nchannels);

    // set the first channel to the alpha channel if output is alpha
    int outputComponents_i;
    _outputComponents->getValue(outputComponents_i);
    OFX::PixelComponentEnum outputComponents = gOutputComponentsMap[outputComponents_i];
    if (spec.alpha_channel != -1 && outputComponents == OFX::ePixelComponentAlpha) {
        _firstChannel->setValue(spec.alpha_channel);
    }
}

void ReadOIIOPlugin::decode(const std::string& filename, OfxTime time, const OfxRectI& renderWindow, float *pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int rowBytes)
{
#ifdef OFX_READ_OIIO_USES_CACHE
    ImageSpec spec;
    //use the thread-safe version of get_imagespec (i.e: make a copy of the imagespec)
    if(!_cache->get_imagespec(ustring(filename), spec)){
        setPersistentMessage(OFX::Message::eMessageError, "", _cache->geterror());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
#else
    bool unassociatedAlpha;
    _unassociatedAlpha->getValueAtTime(time, unassociatedAlpha);
    ImageSpec config;
    if (unassociatedAlpha) {
        config.attribute("oiio:UnassociatedAlpha",1);
    }

    std::auto_ptr<ImageInput> img(ImageInput::open(filename, &config));
    if (!img.get()) {
        setPersistentMessage(OFX::Message::eMessageError, "", std::string("ReadOIIO: cannot open file ") + filename);
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    const ImageSpec &spec = img->spec();
#endif
    int outputComponents_i;
    _outputComponents->getValue(outputComponents_i);
    OFX::PixelComponentEnum outputComponents = gOutputComponentsMap[outputComponents_i];
    if (pixelComponents != outputComponents) {
        setPersistentMessage(OFX::Message::eMessageError, "", "ReadOIIO: OFX Host dit not take into account output components");
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    int firstChannel;
    _firstChannel->getValueAtTime(time, firstChannel);

    // we only support RGBA, RGB or Alpha output clip
    if (pixelComponents != OFX::ePixelComponentRGBA && pixelComponents != OFX::ePixelComponentRGB && pixelComponents != OFX::ePixelComponentAlpha) {
        setPersistentMessage(OFX::Message::eMessageError, "", "OIIO: can only read RGBA, RGB or Alpha components images");
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    assert(kSupportsTiles || (renderWindow.x1 == 0 && renderWindow.x2 == spec.width && renderWindow.y1 == 0 && renderWindow.y2 == spec.height));
    assert((renderWindow.x2 - renderWindow.x1) <= spec.width && (renderWindow.y2 - renderWindow.y1) <= spec.height);
    assert(bounds.x1 <= renderWindow.x1 && renderWindow.x1 <= renderWindow.x2 && renderWindow.x2 <= bounds.x2);
    assert(bounds.y1 <= renderWindow.y1 && renderWindow.y1 <= renderWindow.y2 && renderWindow.y2 <= bounds.y2);
    int chcount = spec.nchannels - firstChannel; // number of available channels
    if (chcount <= 0) {
        std::ostringstream oss;
        oss << "ReadOIIO: Cannot read, first channel is " << firstChannel << ", but image has only " << spec.nchannels << " channels";
        setPersistentMessage(OFX::Message::eMessageError, "", oss.str());
        OFX::throwSuiteStatusException(kOfxStatErrImageFormat);
    }
    int numChannels = 0;
    int outputChannelBegin = 0;
    int chbegin; // start channel for reading
    int chend; // last channel + 1
    bool fillRGB = false;
    bool fillAlpha = false;
    bool moveAlpha = false;
    bool copyRtoGB = false;
    switch(pixelComponents) {
        case OFX::ePixelComponentRGBA:
            numChannels = 4;
            if (chcount == 1) {
                // only one channel to read from input
                chbegin = firstChannel;
                chend = firstChannel + 1;
                if (spec.alpha_channel == -1 || spec.alpha_channel != firstChannel) {
                    // Most probably a luminance image.
                    // fill alpha with 0, duplicate the single channel to r,g,b
                    fillAlpha = true;
                    copyRtoGB = true;
                } else {
                    // An alpha image.
                    fillRGB = true;
                    fillAlpha = false;
                    outputChannelBegin = 3;
                }
            } else {
                chbegin = firstChannel;
                chend = std::min(spec.nchannels, firstChannel + numChannels);
                // After reading, if spec.alpha_channel != 3 and -1,
                // move the channel spec.alpha_channel to channel 3 and fill it
                // with zeroes
                moveAlpha = (firstChannel <= spec.alpha_channel && spec.alpha_channel < firstChannel+3);
                fillAlpha = (chcount < 4); //(spec.alpha_channel == -1);

                fillRGB = (chcount < 3); // need to fill B with black
            }
            break;
        case OFX::ePixelComponentRGB:
            numChannels = 3;
            fillRGB = (spec.nchannels == 1) || (spec.nchannels == 2);
            if (chcount == 1) {
                chbegin = chend = -1;
            } else {
                chbegin = firstChannel;
                chend = std::min(spec.nchannels, firstChannel + numChannels);
            }
            break;
        case OFX::ePixelComponentAlpha:
            numChannels = 1;
            chbegin = firstChannel;
            chend = chbegin + numChannels;
            break;
        default:
#ifndef OFX_READ_OIIO_USES_CACHE
            img->close();
#endif
            OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }
    assert(numChannels);
    int pixelBytes = getPixelBytes(pixelComponents, OFX::eBitDepthFloat);
    size_t pixelDataOffset = (size_t)(renderWindow.y1 - bounds.y1) * rowBytes + (size_t)(renderWindow.x1 - bounds.x1) * pixelBytes;

    if (fillRGB) {
        // fill RGB values with black
        assert(pixelComponents != OFX::ePixelComponentAlpha);
        char* lineStart = (char*)pixelData + pixelDataOffset; // (char*)dstImg->getPixelAddress(renderWindow.x1, renderWindow.y1);
        for (int y = renderWindow.y1; y < renderWindow.y2; ++y, lineStart += rowBytes) {
            float *cur = (float*)lineStart;
            for (int x = renderWindow.x1; x < renderWindow.x2; ++x, cur += numChannels) {
                cur[0] = 0.;
                cur[1] = 0.;
                cur[2] = 0.;
            }
        }
    }
    if (fillAlpha) {
        // fill Alpha values with opaque
        assert(pixelComponents != OFX::ePixelComponentRGB);
        int outputChannelAlpha = (pixelComponents == OFX::ePixelComponentAlpha) ? 0 : 3;
        char* lineStart = (char*)pixelData + pixelDataOffset; // (char*)dstImg->getPixelAddress(renderWindow.x1, renderWindow.y1);
        for (int y = renderWindow.y1; y < renderWindow.y2; ++y, lineStart += rowBytes) {
            float *cur = (float*)lineStart + outputChannelAlpha;
            for (int x = renderWindow.x1; x < renderWindow.x2; ++x, cur += numChannels) {
                cur[0] = 1.;
            }
        }
    }

    if (chbegin != -1 && chend != -1) {
        assert(0 <= chbegin && chbegin < spec.nchannels && chbegin < chend && 0 < chend && chend <= spec.nchannels);
        size_t pixelDataOffset2 = (size_t)(renderWindow.y2 - 1 - bounds.y1) * rowBytes + (size_t)(renderWindow.x1 - bounds.x1) * pixelBytes;
#ifdef OFX_READ_OIIO_USES_CACHE
        // offset for line y2-1
        if (!_cache->get_pixels(ustring(filename),
                               0, //subimage
                               0, //miplevel
                               renderWindow.x1, //x begin
                               renderWindow.x2, //x end
                               spec.height - renderWindow.y2, //y begin
                               spec.height - renderWindow.y1, //y end
                               0, //z begin
                               1, //z end
                               chbegin, //chan begin
                               chend, // chan end
                               TypeDesc::FLOAT, // data type
                               //(float*)dstImg->getPixelAddress(renderWindow.x1, renderWindow.y2 - 1) + outputChannelBegin,// output buffer
                               (float*)((char*)pixelData + pixelDataOffset2)
                               + outputChannelBegin,// output buffer
                               numChannels * sizeof(float), //x stride
                               -rowBytes, //y stride < make it invert Y
                               AutoStride //z stride
                               )) {
            setPersistentMessage(OFX::Message::eMessageError, "", _cache->geterror());
            return;
        }
#else
        assert(!kSupportsTiles && renderWindow.x1 == 0 && renderWindow.x2 == spec.width && renderWindow.y1 == 0 && renderWindow.y2 == spec.height);
        if (spec.tile_width == 0) {
           ///read by scanlines
            img->read_scanlines(spec.height - renderWindow.y2, //y begin
                                spec.height - renderWindow.y1, //y end
                                0, // z
                                chbegin, // chan begin
                                chend, // chan end
                                TypeDesc::FLOAT, // data type
                                (float*)((char*)pixelData + pixelDataOffset2) + outputChannelBegin,
                                numChannels * sizeof(float), //x stride
                                -rowBytes); //y stride < make it invert Y;
        } else {
            img->read_tiles(renderWindow.x1, //x begin
                            renderWindow.x2,//x end
                            spec.height - renderWindow.y2,//y begin
                            spec.height - renderWindow.y1,//y end
                            0, // z begin
                            1, // z end
                            chbegin, // chan begin
                            chend, // chan end
                            TypeDesc::FLOAT,  // data type
                            (float*)((char*)pixelData + pixelDataOffset2) + outputChannelBegin,
                            numChannels * sizeof(float), //x stride
                            -rowBytes, //y stride < make it invert Y
                            AutoStride); //z stride
        }
        img->close();
#endif
    }
    if (moveAlpha) {
        // move alpha channel to the right place
        assert(pixelComponents == OFX::ePixelComponentRGB && spec.alpha_channel < 3 && spec.alpha_channel != -1);
        char* lineStart = (char*)pixelData + pixelDataOffset; // (char*)dstImg->getPixelAddress(renderWindow.x1, renderWindow.y1);
        for (int y = renderWindow.y1; y < renderWindow.y2; ++y, lineStart += rowBytes) {
            float *cur = (float*)lineStart;
            for (int x = renderWindow.x1; x < renderWindow.x2; ++x, cur += numChannels) {
                cur[3] = cur[spec.alpha_channel];
                cur[spec.alpha_channel] = 0.;
            }
        }
    }
    if (copyRtoGB) {
        // copy red to green and blue RGB values with black
        assert(pixelComponents != OFX::ePixelComponentAlpha);
        char* lineStart = (char*)pixelData + pixelDataOffset; // (char*)dstImg->getPixelAddress(renderWindow.x1, renderWindow.y1);
        for (int y = renderWindow.y1; y < renderWindow.y2; ++y, lineStart += rowBytes) {
            float *cur = (float*)lineStart;
            for (int x = renderWindow.x1; x < renderWindow.x2; ++x, cur += numChannels) {
                cur[1] = cur[2] = cur[0];
            }
        }
    }
    
#ifndef OFX_READ_OIIO_USES_CACHE
    img->close();
#endif
}

bool ReadOIIOPlugin::getFrameRegionOfDefinition(const std::string& filename,OfxTime /*time*/,OfxRectD& rod,std::string& error)
{
#ifdef OFX_READ_OIIO_USES_CACHE
    ImageSpec spec;
    //use the thread-safe version of get_imagespec (i.e: make a copy of the imagespec)
    if(!_cache->get_imagespec(ustring(filename), spec)) {
        error = _cache->geterror();
        return false;
    }
#else 
    std::auto_ptr<ImageInput> img(ImageInput::open(filename));
    if (!img.get()) {
        setPersistentMessage(OFX::Message::eMessageError, "", std::string("ReadOIIO: cannot open file ") + filename);
        return false;
    }
    const ImageSpec &spec = img->spec();
#endif
    rod.x1 = spec.x;
    rod.x2 = spec.x + spec.width;
    rod.y1 = spec.y;
    rod.y2 = spec.y + spec.height;
#ifdef OFX_READ_OIIO_USES_CACHE
#else
    img->close();
#endif
    return true;
}

std::string ReadOIIOPlugin::metadata(const std::string& filename)
{
    std::stringstream ss;

#ifdef OFX_READ_OIIO_USES_CACHE
    ImageSpec spec;
    //use the thread-safe version of get_imagespec (i.e: make a copy of the imagespec)
    if(!_cache->get_imagespec(ustring(filename), spec)){
        setPersistentMessage(OFX::Message::eMessageError, "", _cache->geterror());
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
#else 
    std::auto_ptr<ImageInput> img(ImageInput::open(filename));
    if (!img.get()) {
        setPersistentMessage(OFX::Message::eMessageError, "", std::string("ReadOIIO: cannot open file ") + filename);
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    const ImageSpec& spec = img->spec();
#endif
    ss << "file: " << filename << std::endl;
    ss << "    channel list: ";
    for (int i = 0;  i < spec.nchannels;  ++i) {
        ss << i << ":";
        if (i < (int)spec.channelnames.size()) {
            ss << spec.channelnames[i];
        } else {
            ss << "unknown";
        }
        if (i < (int)spec.channelformats.size()) {
            ss << " (" << spec.channelformats[i].c_str() << ")";
        }
        if (i < spec.nchannels-1) {
            ss << ", ";
        }
    }
    ss << std::endl;

    if (spec.x || spec.y || spec.z) {
        ss << "    pixel data origin: x=" << spec.x << ", y=" << spec.y;
        if (spec.depth > 1) {
                ss << ", z=" << spec.z;
        }
        ss << std::endl;
    }
    if (spec.full_x || spec.full_y || spec.full_z ||
        (spec.full_width != spec.width && spec.full_width != 0) ||
        (spec.full_height != spec.height && spec.full_height != 0) ||
        (spec.full_depth != spec.depth && spec.full_depth != 0)) {
        ss << "    full/display size: " << spec.full_width << " x " << spec.full_height;
        if (spec.depth > 1) {
            ss << " x " << spec.full_depth;
        }
        ss << std::endl;
        ss << "    full/display origin: " << spec.full_x << ", " << spec.full_y;
        if (spec.depth > 1) {
            ss << ", " << spec.full_z;
        }
        ss << std::endl;
    }
    if (spec.tile_width) {
        ss << "    tile size: " << spec.tile_width << " x " << spec.tile_height;
        if (spec.depth > 1) {
            ss << " x " << spec.tile_depth;
        }
        ss << std::endl;
    }

    for (ImageIOParameterList::const_iterator p = spec.extra_attribs.begin(); p != spec.extra_attribs.end(); ++p) {
        std::string s = spec.metadata_val (*p, true);
        ss << "    " << p->name() << ": ";
        if (s == "1.#INF") {
            ss << "inf";
        } else {
            ss << s;
        }
        ss << std::endl;
    }
#ifdef OFX_READ_OIIO_USES_CACHE
#else
    img->close();
#endif

    return ss.str();
}

/* Override the clip preferences */
void
ReadOIIOPlugin::getClipPreferences(OFX::ClipPreferencesSetter &clipPreferences)
{
    // set the premultiplication of _outputClip
    // OIIO always outputs premultiplied images, except if it's tol
    bool unassociatedAlpha = false;

    // We assume that if "unassociatedAlpha" is checked, output is UnPremultiplied,
    // but its only true if the image had originally unassociated alpha
    // (OIIO metadata "oiio:UnassociatedAlpha").
    // However, it is not possible to check here if the alpha in the
    // images is associated or not. If the user checked the option, it's
    // probably because it was not associated/premultiplied.
    _unassociatedAlpha->getValue(unassociatedAlpha);
    clipPreferences.setOutputPremultiplication(unassociatedAlpha ? OFX::eImageUnPreMultiplied : OFX::eImagePreMultiplied);
    // set the components of _outputClip
    int outputComponents_i;
    _outputComponents->getValue(outputComponents_i);
    OFX::PixelComponentEnum outputComponents = gOutputComponentsMap[outputComponents_i];
    clipPreferences.setClipComponents(*_outputClip, outputComponents);
}

using namespace OFX;

void ReadOIIOPluginFactory::load() {
}

void ReadOIIOPluginFactory::unload()
{
#  ifdef OFX_READ_OIIO_SHARED_CACHE
    // get the shared image cache (may be shared with other plugins using OIIO)
    ImageCache* sharedcache = ImageCache::create(true);
    // purge it
    // teardown is dangerous if there are other users
    ImageCache::destroy(sharedcache);
#  endif
}

static std::string oiio_versions()
{
    std::ostringstream oss;
    int ver = openimageio_version();
    oss << "OIIO versions:" << std::endl;
    oss << "compiled with " << OIIO_VERSION_STRING << std::endl;
    oss << "running with " << ver/10000 << '.' << (ver%10000)/100 << '.' << (ver%100) << std::endl;
    return oss.str();
}

/** @brief The basic describe function, passed a plugin descriptor */
void ReadOIIOPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
    GenericReaderDescribe(desc, kSupportsTiles);
    ///set OIIO to use as many threads as there are cores on the CPU
    if(!attribute("threads", 0)){
        std::cerr << "Failed to set the number of threads for OIIO" << std::endl;
    }
    
    // basic labels
    desc.setLabels("ReadOIIOOFX", "ReadOIIOOFX", "ReadOIIOOFX");
    desc.setPluginDescription("Read images using OpenImageIO.\n\n"
                              "OpenImageIO supports reading/writing the following file formats:\n"
                              "BMP (*.bmp)\n"
                              "Cineon (*.cin)\n"
                              "Direct Draw Surface (*.dds)\n"
                              "DPX (*.dpx)\n"
                              "Field3D (*.f3d)\n"
                              "FITS (*.fits)\n"
                              "HDR/RGBE (*.hdr)\n"
                              "Icon (*.ico)\n"
                              "IFF (*.iff)\n"
                              "JPEG (*.jpg *.jpe *.jpeg *.jif *.jfif *.jfi)\n"
                              "JPEG-2000 (*.jp2 *.j2k)\n"
                              "OpenEXR (*.exr)\n"
                              "Portable Network Graphics (*.png)\n"
#                           if OIIO_VERSION >= 10400
                              "PNM / Netpbm (*.pbm *.pgm *.ppm *.pfm)\n"
#                           else
                              "PNM / Netpbm (*.pbm *.pgm *.ppm)\n"
#                           endif
                              "PSD (*.psd *.pdd *.psb)\n"
                              "Ptex (*.ptex)\n"
                              "RLA (*.rla)\n"
                              "SGI (*.sgi *.rgb *.rgba *.bw *.int *.inta)\n"
                              "Softimage PIC (*.pic)\n"
                              "Targa (*.tga *.tpic)\n"
                              "TIFF (*.tif *.tiff *.tx *.env *.sm *.vsm)\n"
                              "Zfile (*.zfile)\n\n"
                              + oiio_versions());


#ifdef OFX_EXTENSIONS_TUTTLE

    const char* extensions[] = { "bmp", "cin", "dds", "dpx", "f3d", "fits", "hdr", "ico",
        "iff", "jpg", "jpe", "jpeg", "jif", "jfif", "jfi", "jp2", "j2k", "exr", "png",
        "pbm", "pgm", "ppm",
#     if OIIO_VERSION >= 10400
        "pfm",
#     endif
        "psd", "pdd", "psb", "ptex", "rla", "sgi", "rgb", "rgba", "bw", "int", "inta", "pic", "tga", "tpic", "tif", "tiff", "tx", "env", "sm", "vsm", "zfile", NULL };
    desc.addSupportedExtensions(extensions);
    desc.setPluginEvaluation(50);
#endif

    for (ImageEffectHostDescription::PixelComponentArray::const_iterator it = getImageEffectHostDescription()->_supportedComponents.begin();
         it != getImageEffectHostDescription()->_supportedComponents.end();
         ++it) {
        switch (*it) {
            case ePixelComponentRGBA:
                gSupportsRGBA  = true;
                break;
            case ePixelComponentRGB:
                gSupportsRGB = true;
                break;
            case ePixelComponentAlpha:
                gSupportsAlpha = true;
                break;
            default:
                // other components are not supported by this plugin
                break;
        }
    }
    {
        int i = 0;
        if (gSupportsRGBA) {
            gOutputComponentsMap[i] = ePixelComponentRGBA;
            ++i;
        }
        if (gSupportsRGB) {
            gOutputComponentsMap[i] = ePixelComponentRGB;
            ++i;
        }
        if (gSupportsAlpha) {
            gOutputComponentsMap[i] = ePixelComponentAlpha;
            ++i;
        }
        gOutputComponentsMap[i] = ePixelComponentNone;
    }
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void ReadOIIOPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor *page = GenericReaderDescribeInContextBegin(desc, context, isVideoStreamPlugin(), /*supportsRGBA =*/ true, /*supportsRGB =*/ true, /*supportsAlpha =*/ true, /*supportsTiles =*/ kSupportsTiles);

    OFX::BooleanParamDescriptor* unassociatedAlpha = desc.defineBooleanParam(kParamUnassociatedAlphaName);
    unassociatedAlpha->setLabels(kParamUnassociatedAlphaLabel, kParamUnassociatedAlphaLabel, kParamUnassociatedAlphaLabel);
    unassociatedAlpha->setHint(kParamUnassociatedAlphaHint);
#ifdef OFX_READ_OIIO_USES_CACHE
    unassociatedAlpha->setAnimates(false); // cannot be animated, because relies on changedParam()
#endif
    page->addChild(*unassociatedAlpha);
    desc.addClipPreferencesSlaveParam(*unassociatedAlpha);

    OFX::PushButtonParamDescriptor* pb = desc.definePushButtonParam(kMetadataButtonName);
    pb->setLabels(kMetadataButtonLabel, kMetadataButtonLabel, kMetadataButtonLabel);
    pb->setHint(kMetadataButtonHint);
    page->addChild(*pb);

    IntParamDescriptor *firstChannel = desc.defineIntParam(kFirstChannelParamName);
    firstChannel->setLabels(kFirstChannelParamLabel, kFirstChannelParamLabel, kFirstChannelParamLabel);
    firstChannel->setHint(kFirstChannelParamHint);
    page->addChild(*firstChannel);

    ChoiceParamDescriptor *outputComponents = desc.defineChoiceParam(kOutputComponentsParamName);
    outputComponents->setLabels(kOutputComponentsParamLabel, kOutputComponentsParamLabel, kOutputComponentsParamLabel);
    outputComponents->setHint(kOutputComponentsParamHint);
    // the following must be in the same order as in describe(), so that the map works
    if (gSupportsRGBA) {
        assert(gOutputComponentsMap[outputComponents->getNOptions()] == ePixelComponentRGBA);
        outputComponents->appendOption(kOutputComponentsRGBAOption);
    }
    if (gSupportsRGB) {
        assert(gOutputComponentsMap[outputComponents->getNOptions()] == ePixelComponentRGB);
        outputComponents->appendOption(kOutputComponentsRGBOption);
    }
    if (gSupportsAlpha) {
        assert(gOutputComponentsMap[outputComponents->getNOptions()] == ePixelComponentAlpha);
        outputComponents->appendOption(kOutputComponentsAlphaOption);
    }
    outputComponents->setDefault(0);
    outputComponents->setAnimates(false);
    page->addChild(*outputComponents);
    desc.addClipPreferencesSlaveParam(*outputComponents);

    GenericReaderDescribeInContextEnd(desc, context, page, "reference", "reference");
}

/** @brief The create instance function, the plugin must return an object derived from the \ref OFX::ImageEffect class */
ImageEffect* ReadOIIOPluginFactory::createInstance(OfxImageEffectHandle handle, ContextEnum /*context*/)
{
    return new ReadOIIOPlugin(handle);
}
