/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2021 The Natron Developers
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

/*
 * OCIOFileTransform plugin.
 * Apply a LUT conversion loaded from file.
 */

#ifdef OFX_IO_USING_OCIO

// #include <stdio.h> // for snprintf & _snprintf
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif
#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

#include <climits> // INT_MIN, INT_MAX
#ifdef DEBUG
#include <cstdio>
#endif
#include <memory>
#include <stdexcept> // std::runtime_error
#include <tuple> // std::tie

#include "GenericOCIO.h"
#include "IOUtility.h"
#include "OCIOPluginBase.h"
#include "OCIOProcessorCache.h"
#include "ofxNatron.h"
#include "ofxsCoords.h"
#include "ofxsCopier.h"
#include "ofxsMacros.h"
#include "ofxsProcessing.H"
#include "ofxsThreadSuite.h"

namespace OCIO = OCIO_NAMESPACE;

using namespace OFX;
using namespace OFX::IO;

using std::string;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "OCIOFileTransformOFX"
#define kPluginGrouping "Color/OCIO"
#define kPluginDescription "Use OpenColorIO to apply a transform loaded from the given "       \
                           "file.\n\n"                                                         \
                           "This is usually a 1D or 3D LUT file, but can be other file-based " \
                           "transform, for example an ASC ColorCorrection XML file.\n\n"       \
                           "Note that the file's transform is applied with no special "        \
                           "input/output colorspace handling - so if the file expects "        \
                           "log-encoded pixels, but you apply the node to a linear "           \
                           "image, you will get incorrect results."

#define kPluginIdentifier "fr.inria.openfx.OCIOFileTransform"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 2 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kParamFile "file"
#define kParamFileLabel "File"
#define kParamFileHint "File containing the transform."

// Reload button, and hidden "version" knob to invalidate cache on reload
#define kParamReload "reload"
#define kParamReloadLabel "Reload"
#define kParamReloadHint "Reloads specified files"
#define kParamVersion "version"

#define kParamCCCID "cccId"
#define kParamCCCIDLabel "CCC Id"
#define kParamCCCIDHint "If the source file is an ASC CDL CCC (color correction collection), " \
                        "this specifies the id to lookup. OpenColorIO::Contexts (envvars) are obeyed."
#define kParamCCCIDChoice "cccIdIndex"

#define kParamDirection "direction"
#define kParamDirectionLabel "Direction"
#define kParamDirectionHint "Transform direction."
#define kParamDirectionOptionForward "Forward", "", "forward"
#define kParamDirectionOptionInverse "Inverse", "", "inverse"

#define kParamInterpolation "interpolation"
#define kParamInterpolationLabel "Interpolation"
#define kParamInterpolationHint "Interpolation method. For files that are not LUTs (mtx, etc) this is ignored."
#define kParamInterpolationOptionNearest "Nearest", "", "nearest"
#define kParamInterpolationOptionLinear "Linear", "", "linear"
#define kParamInterpolationOptionTetrahedral "Tetrahedral", "", "tetrahedral"
#define kParamInterpolationOptionBest "Best", "", "best"

static bool gHostIsNatron = false; // TODO: generate a CCCId choice param kParamCCCIDChoice from available IDs

class OCIOFileTransformPlugin
    : public OCIOPluginBase {
public:
    OCIOFileTransformPlugin(OfxImageEffectHandle handle);

    virtual ~OCIOFileTransformPlugin();

private:
    /* Override the render */
    virtual void render(const RenderArguments& args) OVERRIDE FINAL;

    /* override is identity */
    virtual bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;

    /* override changed clip */
    virtual void changedClip(const InstanceChangedArgs& args, const string& clipName) OVERRIDE FINAL;

    // override the rod call
    // virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

    // override the roi call
    // virtual void getRegionsOfInterest(const RegionsOfInterestArguments &args, RegionOfInterestSetter &rois) OVERRIDE FINAL;

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    /* The purpose of this action is to allow a plugin to set up any data it may need
       to do OpenGL rendering in an instance. */
    virtual void* contextAttached(bool createContextData) OVERRIDE FINAL;
    /* The purpose of this action is to allow a plugin to deallocate any resource
       allocated in \ref ::kOfxActionOpenGLContextAttached just before the host
       decouples a plugin from an OpenGL context. */
    virtual void contextDetached(void* contextData) OVERRIDE FINAL;

    void renderGPU(const RenderArguments& args);
#endif

    OCIO::ConstProcessorRcPtr getProcessor(OfxTime time);

    void updateCCCId();

    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI& renderWindow,
                       const OfxPointD& renderScale,
                       const void* srcPixelData,
                       const OfxRectI& srcBounds,
                       PixelComponentEnum srcPixelComponents,
                       int srcPixelComponentCount,
                       BitDepthEnum srcBitDepth,
                       int srcRowBytes,
                       Image* dstImg)
    {
        void* dstPixelData;
        OfxRectI dstBounds;
        PixelComponentEnum dstPixelComponents;
        BitDepthEnum dstBitDepth;
        int dstRowBytes;

        getImageData(dstImg, &dstPixelData, &dstBounds, &dstPixelComponents, &dstBitDepth, &dstRowBytes);
        int dstPixelComponentCount = dstImg->getPixelComponentCount();
        copyPixelData(unpremult,
                      premult,
                      maskmix,
                      time,
                      renderWindow, renderScale,
                      srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                      dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
    }

    void copyPixelData(bool unpremult,
                       bool premult,
                       bool maskmix,
                       double time,
                       const OfxRectI& renderWindow,
                       const OfxPointD& renderScale,
                       const void* srcPixelData,
                       const OfxRectI& srcBounds,
                       PixelComponentEnum srcPixelComponents,
                       int srcPixelComponentCount,
                       BitDepthEnum srcPixelDepth,
                       int srcRowBytes,
                       void* dstPixelData,
                       const OfxRectI& dstBounds,
                       PixelComponentEnum dstPixelComponents,
                       int dstPixelComponentCount,
                       BitDepthEnum dstBitDepth,
                       int dstRowBytes);

    void apply(double time, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds, PixelComponentEnum pixelComponents, int pixelComponentCount, int rowBytes);

    void setupAndCopy(PixelProcessorFilterBase& processor,
                      double time,
                      const OfxRectI& renderWindow,
                      const OfxPointD& renderScale,
                      const void* srcPixelData,
                      const OfxRectI& srcBounds,
                      PixelComponentEnum srcPixelComponents,
                      int srcPixelComponentCount,
                      BitDepthEnum srcPixelDepth,
                      int srcRowBytes,
                      void* dstPixelData,
                      const OfxRectI& dstBounds,
                      PixelComponentEnum dstPixelComponents,
                      int dstPixelComponentCount,
                      BitDepthEnum dstPixelDepth,
                      int dstRowBytes);

private:
    // do not need to delete these, the ImageEffect is managing them for us
    Clip* _dstClip;
    Clip* _srcClip;
    Clip* _maskClip;
    StringParam* _file;
    IntParam* _version;
    StringParam* _cccid;
    ChoiceParam* _direction;
    ChoiceParam* _interpolation;
    DoubleParam* _mix;
    BooleanParam* _maskApply;
    BooleanParam* _maskInvert;

    // everything that the processor depends on
    struct ProcKey {
        OCIOConfigContextKey configContext; // OCIO::GetCurrentConfig(), its current context (search path)
        string file;
        string cccid;
        int version = -1; // the hidden "version" param, which Reload increments
        OCIO::TransformDirection direction = OCIO::TRANSFORM_DIR_FORWARD;
        OCIO::Interpolation interpolation = OCIO::INTERP_UNKNOWN;

        auto tie() const { return std::tie(configContext, file, cccid, version, direction, interpolation); }
        friend bool operator==(const ProcKey& a, const ProcKey& b) { return a.tie() == b.tie(); }
    };
    OCIOProcessorCache<ProcKey> _procCache; // (render threads)

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    OCIOOpenGLContextData* _openGLContextData; // (OpenGL-only) - the single openGL context, in case the host does not support kNatronOfxImageEffectPropOpenGLContextData
#endif
};

OCIOFileTransformPlugin::OCIOFileTransformPlugin(OfxImageEffectHandle handle)
    : OCIOPluginBase(handle)
    , _dstClip(NULL)
    , _srcClip(NULL)
    , _maskClip(NULL)
    , _file(NULL)
    , _version(NULL)
    , _cccid(NULL)
    , _direction(NULL)
    , _interpolation(NULL)
    , _mix(NULL)
    , _maskApply(NULL)
    , _maskInvert(NULL)
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    , _openGLContextData(NULL)
#endif
{
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    assert(_dstClip && (!_dstClip->isConnected() || _dstClip->getPixelComponents() == ePixelComponentRGBA || _dstClip->getPixelComponents() == ePixelComponentRGB));
    _srcClip = getContext() == eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert((!_srcClip && getContext() == eContextGenerator) || (_srcClip && (!_srcClip->isConnected() || _srcClip->getPixelComponents() == ePixelComponentRGBA || _srcClip->getPixelComponents() == ePixelComponentRGB)));
    _maskClip = fetchClip(getContext() == eContextPaint ? "Brush" : "Mask");
    assert(!_maskClip || !_maskClip->isConnected() || _maskClip->getPixelComponents() == ePixelComponentAlpha);
    _file = fetchStringParam(kParamFile);
    _version = fetchIntParam(kParamVersion);
    _cccid = fetchStringParam(kParamCCCID);
    _direction = fetchChoiceParam(kParamDirection);
    _interpolation = fetchChoiceParam(kParamInterpolation);
    assert(_file && _version && _cccid && _direction && _interpolation);
    _mix = fetchDoubleParam(kParamMix);
    _maskApply = paramExists(kParamMaskApply) ? fetchBooleanParam(kParamMaskApply) : 0;
    _maskInvert = fetchBooleanParam(kParamMaskInvert);
    assert(_mix && _maskInvert);
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    setSupportsOpenGLAndTileInfo();
#endif

    updateCCCId();
}

OCIOFileTransformPlugin::~OCIOFileTransformPlugin()
{
}

/* set up and run a copy processor */
void
OCIOFileTransformPlugin::setupAndCopy(PixelProcessorFilterBase& processor,
                                      double time,
                                      const OfxRectI& renderWindow,
                                      const OfxPointD& renderScale,
                                      const void* srcPixelData,
                                      const OfxRectI& srcBounds,
                                      PixelComponentEnum srcPixelComponents,
                                      int srcPixelComponentCount,
                                      BitDepthEnum srcPixelDepth,
                                      int srcRowBytes,
                                      void* dstPixelData,
                                      const OfxRectI& dstBounds,
                                      PixelComponentEnum dstPixelComponents,
                                      int dstPixelComponentCount,
                                      BitDepthEnum dstPixelDepth,
                                      int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // make sure bit depths are sane
    if ((srcPixelDepth != dstPixelDepth) || (srcPixelComponents != dstPixelComponents)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    std::unique_ptr<const Image> orig((_srcClip && _srcClip->isConnected()) ? _srcClip->fetchImage(time) : nullptr);

    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(time)) && _maskClip && _maskClip->isConnected());
    std::unique_ptr<const Image> mask(doMasking ? _maskClip->fetchImage(time) : nullptr);
    if (doMasking) {
        bool maskInvert;
        _maskInvert->getValueAtTime(time, maskInvert);
        processor.doMasking(true);
        processor.setMaskImg(mask.get(), maskInvert);
    }

    // set the images
    assert(orig.get() && dstPixelData && srcPixelData);
    processor.setOrigImg(orig.get());
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstPixelDepth, dstRowBytes);
    processor.setSrcImg(srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, 0);

    // set the render window
    processor.setRenderWindow(renderWindow, renderScale);

    bool premult;
    int premultChannel;
    getPremultAndPremultChannelAtTime(time, premult, premultChannel);
    double mix;
    _mix->getValueAtTime(time, mix);
    processor.setPremultMaskMix(premult, premultChannel, mix);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

void
OCIOFileTransformPlugin::copyPixelData(bool unpremult,
                                       bool premult,
                                       bool maskmix,
                                       double time,
                                       const OfxRectI& renderWindow,
                                       const OfxPointD& renderScale,
                                       const void* srcPixelData,
                                       const OfxRectI& srcBounds,
                                       PixelComponentEnum srcPixelComponents,
                                       int srcPixelComponentCount,
                                       BitDepthEnum srcBitDepth,
                                       int srcRowBytes,
                                       void* dstPixelData,
                                       const OfxRectI& dstBounds,
                                       PixelComponentEnum dstPixelComponents,
                                       int dstPixelComponentCount,
                                       BitDepthEnum dstBitDepth,
                                       int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);
    // do the rendering
    if ((dstBitDepth != eBitDepthFloat) || ((dstPixelComponents != ePixelComponentRGBA) && (dstPixelComponents != ePixelComponentRGB) && (dstPixelComponents != ePixelComponentAlpha))) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    if (!unpremult && !premult && !maskmix) {
        copyPixels(*this, renderWindow, renderScale,
                   srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                   dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
    } else if (unpremult && !premult && !maskmix) {
        if (dstPixelComponents == ePixelComponentRGBA) {
            PixelCopierUnPremult<float, 4, 1, float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == ePixelComponentRGB) {
            PixelCopierUnPremult<float, 3, 1, float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == ePixelComponentAlpha) {
            PixelCopierUnPremult<float, 1, 1, float, 1, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } // switch
    } else if (!unpremult && !premult && maskmix) {
        if (dstPixelComponents == ePixelComponentRGBA) {
            PixelCopierMaskMix<float, 4, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == ePixelComponentRGB) {
            PixelCopierMaskMix<float, 3, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == ePixelComponentAlpha) {
            PixelCopierMaskMix<float, 1, 1, true> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } // switch
    } else if (!unpremult && premult && maskmix) {
        if (dstPixelComponents == ePixelComponentRGBA) {
            PixelCopierPremultMaskMix<float, 4, 1, float, 4, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == ePixelComponentRGB) {
            PixelCopierPremultMaskMix<float, 3, 1, float, 3, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } else if (dstPixelComponents == ePixelComponentAlpha) {
            PixelCopierPremultMaskMix<float, 1, 1, float, 1, 1> fred(*this);
            setupAndCopy(fred, time, renderWindow, renderScale,
                         srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
                         dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
        } // switch
    } else {
        assert(false); // should never happen
    }
} // OCIOFileTransformPlugin::copyPixelData

OCIO::ConstProcessorRcPtr
OCIOFileTransformPlugin::getProcessor(OfxTime time)
{
    string file;

    _file->getValueAtTime(time, file);
    string cccid;
    _cccid->getValueAtTime(time, cccid);
    const int version = _version->getValueAtTime(time);
    const OCIO::TransformDirection direction = (_direction->getValueAtTime(time) == 0) ? OCIO::TRANSFORM_DIR_FORWARD : OCIO::TRANSFORM_DIR_INVERSE;
    OCIO::Interpolation interpolation;
    switch (_interpolation->getValueAtTime(time)) {
    case 0:
        interpolation = OCIO::INTERP_NEAREST;
        break;
    case 1:
        interpolation = OCIO::INTERP_LINEAR;
        break;
    case 2:
        interpolation = OCIO::INTERP_TETRAHEDRAL;
        break;
    case 3:
        interpolation = OCIO::INTERP_BEST;
        break;
    default:
        // Should never happen
        persistentErrorAndThrow(*this, "OCIO Interpolation value out of bounds");
    }

    return processorOrFail([&] {
        AutoSetAndRestoreThreadLocale locale;
        const OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig(); // (never null)
        const ProcKey key { OCIOConfigContextKey(config), file, cccid, version, direction, interpolation };

        return _procCache.get(key, [&] {
            OCIO::FileTransformRcPtr transform = OCIO::FileTransform::Create();
            transform->setSrc(file.c_str());
            transform->setCCCId(cccid.c_str());
            transform->setDirection(direction);
            transform->setInterpolation(interpolation);

            // Bypass the processor cache of the config: its key is the transform's arguments, not the contents of
            // the file, and OCIO::ClearAllCaches() (Reload) "does not apply to instance-specific caches such as the
            // Processor cache in a Config instance" (OpenColorIO.h). The shared current config is const, and its
            // clearProcessorCache() isn't, so build in an editable copy: the copy's processor cache starts empty
            // (Config::Impl::operator=), so it always builds; the default flags keep the processor's CPU/GPU caches.
            // (_procCache keeps the processor; the LUT itself is in OCIO's file cache, which Reload clears.)
            OCIO::ConfigRcPtr copy = config->createEditableCopy();

            return copy->getProcessor(transform, OCIO::TRANSFORM_DIR_FORWARD);
        });
    });
} // getProcessor

void
OCIOFileTransformPlugin::apply(double time,
                               const OfxRectI& renderWindow,
                               const OfxPointD& renderScale,
                               float* pixelData,
                               const OfxRectI& bounds,
                               PixelComponentEnum pixelComponents,
                               int pixelComponentCount,
                               int rowBytes)
{
    // are we in the image bounds
    if ((renderWindow.x1 < bounds.x1) || (renderWindow.x1 >= bounds.x2) || (renderWindow.y1 < bounds.y1) || (renderWindow.y1 >= bounds.y2) || (renderWindow.x2 <= bounds.x1) || (renderWindow.x2 > bounds.x2) || (renderWindow.y2 <= bounds.y1) || (renderWindow.y2 > bounds.y2)) {
        throw std::runtime_error("OCIO: render window outside of image bounds");
    }
    if ((pixelComponents != ePixelComponentRGBA) && (pixelComponents != ePixelComponentRGB)) {
        throw std::runtime_error("OCIO: invalid components (only RGB and RGBA are supported)");
    }

    OCIOProcessor processor(*this);
    // set the images
    processor.setDstImg(pixelData, bounds, pixelComponents, pixelComponentCount, eBitDepthFloat, rowBytes);

    // set the render window
    processor.setRenderWindow(renderWindow, renderScale);

    processor.setProcessor(getProcessor(time));

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

#if defined(OFX_SUPPORTS_OPENGLRENDER)

/*
 * Action called when an effect has just been attached to an OpenGL
 * context.
 *
 * The purpose of this action is to allow a plugin to set up any data it may need
 * to do OpenGL rendering in an instance. For example...
 *  - allocate a lookup table on a GPU,
 *  - create an openCL or CUDA context that is bound to the host's OpenGL
 *    context so it can share buffers.
 */
void*
OCIOFileTransformPlugin::contextAttached(bool createContextData)
{
#ifdef DEBUG
    if (getImageEffectHostDescription()->isNatron && !createContextData) {
        std::printf("ERROR: Natron did not ask to create context data\n");
    }
#endif
    if (createContextData) {
        // This will load OpenGL functions the first time it is executed (thread-safe)
        return new OCIOOpenGLContextData;
    } else {
        if (_openGLContextData) {
#ifdef DEBUG
            std::printf("ERROR: contextAttached() called but context already attached\n");
#endif
            contextDetached(NULL);
        }
        _openGLContextData = new OCIOOpenGLContextData;
    }

    return NULL;
}

/*
 * Action called when an effect is about to be detached from an
 * OpenGL context
 *
 * The purpose of this action is to allow a plugin to deallocate any resource
 * allocated in \ref ::kOfxActionOpenGLContextAttached just before the host
 * decouples a plugin from an OpenGL context.
 * The host must call this with the same OpenGL context active as it
 * called with the corresponding ::kOfxActionOpenGLContextAttached.
 */
void
OCIOFileTransformPlugin::contextDetached(void* contextData)
{
    if (contextData) {
        OCIOOpenGLContextData* myData = (OCIOOpenGLContextData*)contextData;
        delete myData;
    } else {
        if (!_openGLContextData) {
#ifdef DEBUG
            std::printf("ERROR: contextDetached() called but no context attached\n");
#endif
        }
        delete _openGLContextData;
        _openGLContextData = NULL;
    }
}

void
OCIOFileTransformPlugin::renderGPU(const RenderArguments& args)
{
    std::unique_ptr<Texture> srcImg(_srcClip->loadTexture(args.time));
    if (!srcImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    checkBadRenderScaleOrField(srcImg, args);

    std::unique_ptr<Texture> dstImg(_dstClip->loadTexture(args.time));
    if (!dstImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(dstImg, args);

    BitDepthEnum srcBitDepth = srcImg->getPixelDepth();
    PixelComponentEnum srcComponents = srcImg->getPixelComponents();
    BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if ((dstBitDepth != eBitDepthFloat) || (dstBitDepth != srcBitDepth)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    PixelComponentEnum dstComponents = dstImg->getPixelComponents();
    if (((dstComponents != ePixelComponentRGBA) && (dstComponents != ePixelComponentRGB) && (dstComponents != ePixelComponentAlpha)) || (dstComponents != srcComponents)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    // are we in the image bounds
    OfxRectI dstBounds = dstImg->getBounds();
    if ((args.renderWindow.x1 < dstBounds.x1) || (args.renderWindow.x1 >= dstBounds.x2) || (args.renderWindow.y1 < dstBounds.y1) || (args.renderWindow.y1 >= dstBounds.y2) || (args.renderWindow.x2 <= dstBounds.x1) || (args.renderWindow.x2 > dstBounds.x2) || (args.renderWindow.y2 <= dstBounds.y1) || (args.renderWindow.y2 > dstBounds.y2)) {
        throwSuiteStatusException(kOfxStatErrValue);

        return;
        // throw std::runtime_error("render window outside of image bounds");
    }

    OCIOOpenGLContextData* contextData = NULL;
    if (getImageEffectHostDescription()->isNatron && !args.openGLContextData) {
#ifdef DEBUG
        std::printf("ERROR: Natron did not provide the contextData pointer to the OpenGL render func.\n");
#endif
    }
    if (args.openGLContextData) {
        // host provided kNatronOfxImageEffectPropOpenGLContextData,
        // which was returned by kOfxActionOpenGLContextAttached
        contextData = (OCIOOpenGLContextData*)args.openGLContextData;
    } else {
        if (!_openGLContextData) {
            // Sony Catalyst Edit never calls kOfxActionOpenGLContextAttached
#ifdef DEBUG
            std::printf(("ERROR: OpenGL render() called without calling contextAttached() first. Calling it now.\n"));
#endif
            contextAttached(false);
            assert(_openGLContextData);
        }
        contextData = _openGLContextData;
    }
    if (!contextData) {
        throwSuiteStatusException(kOfxStatFailed);
    }

    OCIO::ConstProcessorRcPtr proc = getProcessor(args.time);
    assert(proc);

    GenericOCIO::applyGL(srcImg.get(), proc, contextData);
} // renderGPU

#endif // defined(OFX_SUPPORTS_OPENGLRENDER)

/* Override the render */
void
OCIOFileTransformPlugin::render(const RenderArguments& args)
{
    if (!_srcClip) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    if (!_dstClip) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    assert(_srcClip && _dstClip);

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    if (args.openGLEnabled) {
        renderGPU(args);

        return;
    }
#endif

    std::unique_ptr<const Image> srcImg(_srcClip->fetchImage(args.time));
    if (!srcImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(srcImg, args);

    BitDepthEnum srcBitDepth = srcImg->getPixelDepth();
    PixelComponentEnum srcComponents = srcImg->getPixelComponents();

    if (!_dstClip) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    assert(_dstClip);
    std::unique_ptr<Image> dstImg(_dstClip->fetchImage(args.time));
    if (!dstImg.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(dstImg, args);

    BitDepthEnum dstBitDepth = dstImg->getPixelDepth();
    if ((dstBitDepth != eBitDepthFloat) || (dstBitDepth != srcBitDepth)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    PixelComponentEnum dstComponents = dstImg->getPixelComponents();
    if (((dstComponents != ePixelComponentRGBA) && (dstComponents != ePixelComponentRGB) && (dstComponents != ePixelComponentAlpha)) || (dstComponents != srcComponents)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    // are we in the image bounds
    OfxRectI dstBounds = dstImg->getBounds();
    if ((args.renderWindow.x1 < dstBounds.x1) || (args.renderWindow.x1 >= dstBounds.x2) || (args.renderWindow.y1 < dstBounds.y1) || (args.renderWindow.y1 >= dstBounds.y2) || (args.renderWindow.x2 <= dstBounds.x1) || (args.renderWindow.x2 > dstBounds.x2) || (args.renderWindow.y2 <= dstBounds.y1) || (args.renderWindow.y2 > dstBounds.y2)) {
        throwSuiteStatusException(kOfxStatErrValue);

        return;
        // throw std::runtime_error("render window outside of image bounds");
    }

    const void* srcPixelData = NULL;
    OfxRectI bounds;
    PixelComponentEnum pixelComponents;
    BitDepthEnum bitDepth;
    int srcRowBytes;
    getImageData(srcImg.get(), &srcPixelData, &bounds, &pixelComponents, &bitDepth, &srcRowBytes);
    int pixelComponentCount = srcImg->getPixelComponentCount();

    // allocate temporary image
    int pixelBytes = pixelComponentCount * getComponentBytes(srcBitDepth);
    int tmpRowBytes = (args.renderWindow.x2 - args.renderWindow.x1) * pixelBytes;
    size_t memSize = (size_t)(args.renderWindow.y2 - args.renderWindow.y1) * (size_t)tmpRowBytes;
    ImageMemory mem(memSize, this);
    float* tmpPixelData = (float*)mem.lock();
    bool premult = getPremultValueAtTime(args.time);

    // copy renderWindow to the temporary image
    copyPixelData(premult, false, false, args.time, args.renderWindow, args.renderScale, srcPixelData, bounds, pixelComponents, pixelComponentCount, bitDepth, srcRowBytes, tmpPixelData, args.renderWindow, pixelComponents, pixelComponentCount, bitDepth, tmpRowBytes);

    /// do the color-space conversion
    apply(args.time, args.renderWindow, args.renderScale, tmpPixelData, args.renderWindow, pixelComponents, pixelComponentCount, tmpRowBytes);

    // copy the color-converted window
    copyPixelData(false, premult, true, args.time, args.renderWindow, args.renderScale, tmpPixelData, args.renderWindow, pixelComponents, pixelComponentCount, bitDepth, tmpRowBytes, dstImg.get());
} // OCIOFileTransformPlugin::render

bool
OCIOFileTransformPlugin::isIdentity(const IsIdentityArguments& args,
                                    Clip*& identityClip,
                                    double& /*identityTime*/
                                    ,
                                    int& /*view*/, std::string& /*plane*/)
{
    string file;
    _file->getValue(file);
    if (file.empty()) {
        identityClip = _srcClip;

        return true;
    }

    double mix;
    _mix->getValueAtTime(args.time, mix);

    if (mix == 0.) {
        identityClip = _srcClip;

        return true;
    }

    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(args.time)) && _maskClip && _maskClip->isConnected());
    if (doMasking) {
        bool maskInvert;
        _maskInvert->getValueAtTime(args.time, maskInvert);
        if (!maskInvert) {
            OfxRectI maskRoD;
            Coords::toPixelEnclosing(_maskClip->getRegionOfDefinition(args.time), args.renderScale, _maskClip->getPixelAspectRatio(), &maskRoD);
            // effect is identity if the renderWindow doesn't intersect the mask RoD
            if (!Coords::rectIntersection<OfxRectI>(args.renderWindow, maskRoD, 0)) {
                identityClip = _srcClip;

                return true;
            }
        }
    }

    return false;
}

void
OCIOFileTransformPlugin::updateCCCId()
{
    // The CCC Id selects a correction of a .cdl, .cc or .ccc file (OCIO's FileTransform: "Load the CDL from the src
    // .cdl, .cc, or .ccc file"). The extension is the part after the last '.', in lowercase.
    // TODO: Could this be queried from the processor?
    string srcstring;

    _file->getValue(srcstring);
    const string ext = extension(srcstring);
    _cccid->setIsSecretAndDisabled( !( (ext == "cc") || (ext == "ccc") || (ext == "cdl") ) );
}

void
OCIOFileTransformPlugin::changedParam(const InstanceChangedArgs& args,
                                      const string& paramName)
{
    // must clear persistent message, or render() is not called by Nuke after an error
    clearPersistentMessage();
    changedPremultParam(args, paramName); // (#457)
    // Only show the cccid knob when loading a .cdl/.cc/.ccc file. Set
    // hidden state when the src is changed, or the node properties
    // are shown
    if (paramName == kParamFile) {
        updateCCCId();
    } else if ((paramName == kParamReload) && (args.reason == eChangeUserEdit)) {
        // clear OCIO's file caches before the version change, which invalidates the host's and _procCache: a render
        // in between would cache the old file contents under the new version
        OCIO::ClearAllCaches();
        _version->setValue(_version->getValue() + 1); // invalidate the node cache
#ifdef OFX_SUPPORTS_OPENGLRENDER
    } else if (paramEffectsOpenGLAndTileSupport(paramName)) {
        setSupportsOpenGLAndTileInfoAtTime(args.time);
#endif
    }
}

void
OCIOFileTransformPlugin::changedClip(const InstanceChangedArgs& args,
                                     const string& clipName)
{
    if ((clipName == kOfxImageEffectSimpleSourceClipName) && _srcClip && (args.reason == eChangeUserEdit)) {
        changedSrcClip(_srcClip);
    }
}

mDeclarePluginFactory(OCIOFileTransformPluginFactory, { ofxsThreadSuiteCheck(); }, {});
static string
supportedFormats()
{
    string s = "Supported formats:\n";

    for (int i = 0; i < OCIO::FileTransform::GetNumFormats(); ++i) {
        const char* name = OCIO::FileTransform::GetFormatNameByIndex(i);
        const char* exten = OCIO::FileTransform::GetFormatExtensionByIndex(i);
        s += string("\n.") + exten + " (" + name + ")";
    }

    return s;
}

/** @brief The basic describe function, passed a plugin descriptor */
void
OCIOFileTransformPluginFactory::describe(ImageEffectDescriptor& desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(string(kPluginDescription) + "\n\n" + supportedFormats());

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextPaint);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    desc.setSupportsOpenGLRender(true);
#endif
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
OCIOFileTransformPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                                  ContextEnum context)
{
    gHostIsNatron = (getImageEffectHostDescription()->isNatron);
    // Source clip only in the filter context
    // create the mandated source clip
    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // create the mandated output clip
    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->setSupportsTiles(kSupportsTiles);

    ClipDescriptor* maskClip = (context == eContextPaint) ? desc.defineClip("Brush") : desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setTemporalClipAccess(false);
    if (context != eContextPaint) {
        maskClip->setOptional(true);
    }
    maskClip->setSupportsTiles(kSupportsTiles);
    maskClip->setIsMask(true);

    // make some pages and to things in
    PageParamDescriptor* page = desc.definePageParam("Controls");

    {
        StringParamDescriptor* param = desc.defineStringParam(kParamFile);
        param->setLabel(kParamFileLabel);
        param->setHint(string(kParamFileHint) + "\n\n" + supportedFormats());
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(true);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamReload);
        param->setLabel(kParamReloadLabel);
        param->setHint(kParamReloadHint);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamVersion);
        param->setRange(INT_MIN, INT_MAX); // Resolve requires range and display range or values are clamped to (0,0)
        param->setDisplayRange(1, 100);
        param->setIsSecretAndDisabled(true); // always secret
        param->setDefault(1);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamCCCID);
        param->setLabel(kParamCCCIDLabel);
        param->setHint(kParamCCCIDHint);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamDirection);
        param->setLabel(kParamDirectionLabel);
        param->setHint(kParamDirectionHint);
        param->appendOption(kParamDirectionOptionForward);
        param->appendOption(kParamDirectionOptionInverse);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamInterpolation);
        param->setLabel(kParamInterpolationLabel);
        param->setHint(kParamInterpolationHint);
        param->appendOption(kParamInterpolationOptionNearest);
        param->appendOption(kParamInterpolationOptionLinear);
        param->appendOption(kParamInterpolationOptionTetrahedral);
        param->appendOption(kParamInterpolationOptionBest);
        param->setDefault(1);
        if (page) {
            page->addChild(*param);
        }
    }

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    OCIOPluginBase::defineEnableGPUParam(desc, page);
#endif

    ofxsPremultDescribeParams(desc, page);
    OCIOPluginBase::definePremultSetParam(desc, page);
    ofxsMaskMixDescribeParams(desc, page);
} // OCIOFileTransformPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
OCIOFileTransformPluginFactory::createInstance(OfxImageEffectHandle handle,
                                               ContextEnum /*context*/)
{
    return new OCIOFileTransformPlugin(handle);
}

static OCIOFileTransformPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

    OFXS_NAMESPACE_ANONYMOUS_EXIT

#endif // OFX_IO_USING_OCIO
