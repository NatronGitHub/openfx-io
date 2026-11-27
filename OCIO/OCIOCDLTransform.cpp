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
 * OCIOCDLTransform plugin.
 * Apply an ASC CDL grade.
 */

#ifdef OFX_IO_USING_OCIO

#include <array>
#include <cerrno>
#include <cstdio> // fopen...
#ifdef _WIN32
#include <fcntl.h> // _O_*
#include <io.h> // _wopen
#include <sys/stat.h> // _S_*
#endif
#include <locale>
#include <memory> // std::unique_ptr
#include <optional>
#include <sstream> // std::ostringstream
#include <stdexcept> // std::runtime_error
#include <string>
#include <system_error> // std::error_code
#include <tuple> // std::tie

#include "IOUtility.h"
#include "ofxsFileOpen.h"
#include "ofxNatron.h"
#include "ofxsCoords.h"
#include "ofxsCopier.h"
#include "ofxsMacros.h"
#include "ofxsProcessing.H"
#include "ofxsThreadSuite.h"

#include "GenericOCIO.h"
#include "OCIOPluginBase.h"
#include "OCIOProcessorCache.h"

namespace OCIO = OCIO_NAMESPACE;

using namespace OFX;
using namespace OFX::IO;

using std::string;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "OCIOCDLTransformOFX"
#define kPluginGrouping "Color/OCIO"
#define kPluginDescription                                                                               \
    "Use OpenColorIO to apply an ASC Color Decision List (CDL) grade.\n"                                 \
    "The formula applied for each channel is:\nout = (in * slope + offset)^power.\n"                     \
    "The saturation is then applied to all channel using the standard rec709 saturation coefficients:\n" \
    "luma = 0.2126 * inR + 0.7152 * inG + 0.0722 * inB\n"                                                \
    "outR = Clamp( luma + sat * (inR - luma) )\n"                                                        \
    "outG = Clamp( luma + sat * (inG - luma) )\n"                                                        \
    "outB = Clamp( luma + sat * (inB - luma) ).\n\n"                                                     \
    "The grade can be loaded from an ASC .ccc (Color Correction Collection) or .cc (Color Correction) file."
//, and saved to a .cc file."

#define kPluginIdentifier "fr.inria.openfx.OCIOCDLTransform"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 2 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kParamSlope "slope"
#define kParamSlopeLabel "Slope"
#define kParamSlopeHint "ASC CDL slope"
#define kParamSlopeMin 0.
#define kParamSlopeMax 4.

#define kParamOffset "offset"
#define kParamOffsetLabel "Offset"
#define kParamOffsetHint "ASC CDL offset"
#define kParamOffsetMin -0.2
#define kParamOffsetMax 0.2

#define kParamPower "power"
#define kParamPowerLabel "Power"
#define kParamPowerHint "ASC CDL power"
#define kParamPowerMin 0.
#define kParamPowerMax 4.

#define kParamSaturation "saturation"
#define kParamSaturationLabel "Saturation"
#define kParamSaturationHint "ASC CDL saturation"
#define kParamSaturationMin 0.
#define kParamSaturationMax 4.

#define kParamDirection "direction"
#define kParamDirectionLabel "Direction"
#define kParamDirectionHint "Transform direction."
#define kParamDirectionOptionForward "Forward", "", "forward"
#define kParamDirectionOptionInverse "Inverse", "", "inverse"

#define kParamReadFromFile "readFromFile"
#define kParamReadFromFileLabel "Read from file"
#define kParamReadFromFileHint \
    "Load color correction information from the .cc or .ccc file."

#define kParamFile "file"
#define kParamFileLabel "File"
#define kParamFileHint                                                   \
    "Specify the src ASC CDL file, on disk, to use for this transform. " \
    "This can be either a .cc or .ccc file. If .ccc is specified, the cccid is required."

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

#define kParamExport "export"
#define kParamExportLabel "Export"
#define kParamExportHint "Enter the name of a new file to export this grade as a ColorCorrection XML file (.cc), which can be loaded with the OCIOFileTransform, or using a FileTransform in an OCIO config. The file must not already exist."

static bool gHostIsNatron = false; // TODO: generate a CCCId choice param kParamCCCIDChoice from available IDs

// An ASC CDL grade, in OCIO's order: the slope, offset and power of red, green and blue (SOP), then the saturation
struct CDLGrade {
    std::array<double, 9> sop { { 1., 1., 1., 0., 0., 0., 1., 1., 1. } };
    double saturation = 1.;

    bool operator==(const CDLGrade& other) const { return (sop == other.sop) && (saturation == other.saturation); }
    bool operator!=(const CDLGrade& other) const { return !(*this == other); }
};

// A grade, or (if grade is empty) the reason why it can't be read
struct CDLGradeResult {
    std::optional<CDLGrade> grade;
    string error;
};

// closes a std::FILE (RAII, R.1: https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rr-raii)
struct FileCloser {
    void operator()(std::FILE* f) const noexcept { std::fclose(f); }
};

// Read the grade of a .cc file, or of the id cccid in a .ccc file. It makes no OFX call, so it may be called from
// any action, render included, and it is thread-safe (OCIO's file cache has its own lock).
CDLGradeResult
readCDLFile(const string& file,
            const string& cccid)
{
    CDLGradeResult result;
    // OCIO also caches the files that it can't read, until OCIO::ClearAllCaches() (Reload): check first that the file
    // can be opened, so that a file that is missing for a while is read when it appears
    {
        const std::unique_ptr<std::FILE, FileCloser> f(OFX::fopen_utf8(file.c_str(), "rb"));
        if (!f) {
            const std::error_code error(errno, std::generic_category());
            result.error = string("Cannot open the CDL file \"") + file + "\": " + error.message();

            return result;
        }
    }
    try {
        AutoSetAndRestoreThreadLocale locale;
        // OCIO caches the file (OCIO::ClearAllCaches() clears its cache)
        const OCIO::ConstCDLTransformRcPtr transform = OCIO::CDLTransform::CreateFromFile(file.c_str(), cccid.c_str());
        CDLGrade grade;
        transform->getSOP(grade.sop.data());
        grade.saturation = transform->getSat();
        result.grade = grade;
    } catch (const std::exception& e) { // OCIO::Exception is a std::runtime_error
        result.error = string("Cannot read the CDL file: ") + e.what(); // OCIO names the file
    }

    return result;
}

// The OCIO transform of grade, forward if direction is 0, else inverse
OCIO::CDLTransformRcPtr
createCDLTransform(const CDLGrade& grade,
                   int direction)
{
    OCIO::CDLTransformRcPtr cc = OCIO::CDLTransform::Create();
    cc->setSOP(grade.sop.data());
    cc->setSat(grade.saturation); // a double in OCIO 2 (an exported file has 0.7, not 0.699999988079071)
    cc->setDirection((direction == 0) ? OCIO::TRANSFORM_DIR_FORWARD : OCIO::TRANSFORM_DIR_INVERSE);

    return cc;
}

class OCIOCDLTransformPlugin
    : public OCIOPluginBase {
public:
    OCIOCDLTransformPlugin(OfxImageEffectHandle handle);

    virtual ~OCIOCDLTransformPlugin();

    /* Override the render */
    virtual void render(const RenderArguments& args) OVERRIDE FINAL;

    /* override is identity */
    virtual bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;
    void exportCDL(double time, const string& exportName);

    /* override changed clip */
    virtual void changedClip(const InstanceChangedArgs& args, const string& clipName) OVERRIDE FINAL;

    // override the rod call
    // virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments &args, OfxRectD &rod) OVERRIDE FINAL;

    // override the roi call
    // virtual void getRegionsOfInterest(const RegionsOfInterestArguments &args, RegionOfInterestSetter &rois) OVERRIDE FINAL;

private:
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

    // The processor of the grade at time. On error, sets a persistent message and throws kOfxStatFailed.
    // Render-safe (no paramSetValue).
    OCIO::ConstProcessorRcPtr getProcessor(OfxTime time);

    // The processor of grade in direction directioni, from _procCache. Throws an OCIO::Exception on error.
    OCIO::ConstProcessorRcPtr processorForGrade(const CDLGrade& grade, int directioni);

    // Enable the grade params, unless they are read from the file. Sets no param value.
    void refreshKnobEnabledState(bool readFromFile);

    // The grade params at time. Render-safe.
    CDLGrade getParamsGradeAtTime(double time) const;

    // The grade of the CDL file at time, through _cdlFileCache. Render-safe (it never sets a param).
    CDLGradeResult getFileGradeAtTime(double time);

    // The grade that is applied at time: the CDL file's if "Read from file" is on, else the params'. Render-safe.
    CDLGradeResult getGradeAtTime(double time);

    // Set the grade params to grade (paramSetValue): only from changedParam(), see the constructor.
    void setGradeParams(const CDLGrade& grade);

    // Load the grade of the CDL file into the grade params (paramSetValue): only from changedParam(), see the
    // constructor. On error, sets a persistent message (or, if userEdit and the host has no persistent message suite,
    // sends it), and leaves the params unchanged.
    void loadCDLFileIntoParams(double time, bool userEdit);

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
    // Thread safety: render() and isIdentity() may run concurrently in several threads (eRenderFullySafe), and
    // concurrently with the main-thread actions (constructor, changedParam()).
    // - The clip and param pointers are set by the constructor and never changed: they may be read without a lock.
    // - _cdlFileMutex guards _cdlFileCache.
    // - _procCache has its own mutex (see OCIOProcessorCache).
    // The two mutexes are never held at the same time.

    // do not need to delete these, the ImageEffect is managing them for us
    Clip* _dstClip;
    Clip* _srcClip;
    Clip* _maskClip;
    RGBParam* _slope;
    RGBParam* _offset;
    RGBParam* _power;
    DoubleParam* _saturation;
    ChoiceParam* _direction;
    BooleanParam* _readFromFile;
    StringParam* _file;
    IntParam* _version;
    StringParam* _cccid;
    StringParam* _export;
    DoubleParam* _mix;
    BooleanParam* _maskApply;
    BooleanParam* _maskInvert;

    // The last CDL file that getFileGradeAtTime() read successfully, and its grade. Failures are not cached: a file
    // that is missing for a while (e.g. at the start of a NatronRenderer batch, which has no Reload button) is read
    // again by the next render. version is the hidden "version" param, which Reload increments, so that the file is
    // read again.
    struct CDLFileCache {
        string file;
        string cccid;
        int version = 0;
        CDLGrade grade;
    };
    GenericOCIO::Mutex _cdlFileMutex;
    std::optional<CDLFileCache> _cdlFileCache; // guarded by _cdlFileMutex

    // everything that the processor depends on
    struct ProcKey {
        OCIOConfigContextKey configContext; // OCIO::GetCurrentConfig(), its current context
        CDLGrade grade;
        int direction = -1;

        auto tie() const { return std::tie(configContext, grade, direction); }
        friend bool operator==(const ProcKey& a, const ProcKey& b) { return a.tie() == b.tie(); }
    };
    OCIOProcessorCache<ProcKey> _procCache;

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    OCIOOpenGLContextData* _openGLContextData; // (OpenGL-only) - the single openGL context, in case the host does not support kNatronOfxImageEffectPropOpenGLContextData
#endif
};

OCIOCDLTransformPlugin::OCIOCDLTransformPlugin(OfxImageEffectHandle handle)
    : OCIOPluginBase(handle)
    , _dstClip(NULL)
    , _srcClip(NULL)
    , _maskClip(NULL)
    , _slope(NULL)
    , _offset(NULL)
    , _power(NULL)
    , _saturation(NULL)
    , _direction(NULL)
    , _readFromFile(NULL)
    , _file(NULL)
    , _version(NULL)
    , _cccid(NULL)
    , _export(NULL)
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
    _slope = fetchRGBParam(kParamSlope);
    _offset = fetchRGBParam(kParamOffset);
    _power = fetchRGBParam(kParamPower);
    _saturation = fetchDoubleParam(kParamSaturation);
    _direction = fetchChoiceParam(kParamDirection);
    _readFromFile = fetchBooleanParam(kParamReadFromFile);
    _file = fetchStringParam(kParamFile);
    _version = fetchIntParam(kParamVersion);
    _cccid = fetchStringParam(kParamCCCID);
    _export = fetchStringParam(kParamExport);
    assert(_slope && _offset && _power && _saturation && _direction && _readFromFile && _file && _version && _cccid && _export);
    _mix = fetchDoubleParam(kParamMix);
    _maskApply = paramExists(kParamMaskApply) ? fetchBooleanParam(kParamMaskApply) : 0;
    _maskInvert = fetchBooleanParam(kParamMaskInvert);
    assert(_mix && _maskInvert);

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    setSupportsOpenGLAndTileInfo();
#endif

    // Only the enabled state of the params is set here: the constructor neither reads the CDL file nor sets a param.
    // - It must never fail because of the file: on kOfxStatFailed from kOfxActionCreateInstance, "the host should
    //   destroy the instanace handle and not attempt to proceed further" (ofxCore.h), so Natron would drop the node
    //   when it loads a project whose CDL file is missing.
    // - The spec allows setting params here (ofxParameter.rst, "Setting Parameters": "You can only set parameters in
    //   the following actions [...] kOfxActionCreateInstance, [...] kOfxActionInstanceChanged", and GenericOCIO's
    //   constructor relies on it), although ofxParam.h says "paramSetValue should only be called from within a
    //   kOfxActionInstanceChanged or interact action". But a setValue() here makes Natron call changedParam() on an
    //   object that is not fully constructed yet, and it marks a project that is only being loaded as modified.
    // So render() reads the file itself, and changedParam() loads it into the params. By design, opening the UI
    // doesn't load the file into the params either: Reload does.
    refreshKnobEnabledState(_readFromFile->getValue());
}

OCIOCDLTransformPlugin::~OCIOCDLTransformPlugin()
{
}

/* set up and run a copy processor */
void
OCIOCDLTransformPlugin::setupAndCopy(PixelProcessorFilterBase& processor,
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
OCIOCDLTransformPlugin::copyPixelData(bool unpremult,
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
} // OCIOCDLTransformPlugin::copyPixelData

CDLGrade
OCIOCDLTransformPlugin::getParamsGradeAtTime(double time) const
{
    CDLGrade grade;
    _slope->getValueAtTime(time, grade.sop[0], grade.sop[1], grade.sop[2]);
    _offset->getValueAtTime(time, grade.sop[3], grade.sop[4], grade.sop[5]);
    _power->getValueAtTime(time, grade.sop[6], grade.sop[7], grade.sop[8]);
    _saturation->getValueAtTime(time, grade.saturation);

    return grade;
}

CDLGradeResult
OCIOCDLTransformPlugin::getFileGradeAtTime(double time)
{
    string file;
    _file->getValueAtTime(time, file);
    string cccid;
    _cccid->getValueAtTime(time, cccid);
    const int version = _version->getValueAtTime(time);

    // The lock is deliberately held while OCIO reads the file (CP.43:
    // https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#rconc-time): the render threads that need the same
    // file wait for one read instead of all reading it, and OCIO never calls back into the plug-in.
    GenericOCIO::AutoMutex guard(_cdlFileMutex);
    if (_cdlFileCache && (_cdlFileCache->file == file) && (_cdlFileCache->cccid == cccid) && (_cdlFileCache->version == version)) {
        CDLGradeResult result;
        result.grade = _cdlFileCache->grade;

        return result;
    }
    CDLGradeResult result = readCDLFile(file, cccid);
    if (result.grade) {
        _cdlFileCache = CDLFileCache { file, cccid, version, *result.grade };
    }

    return result;
}

CDLGradeResult
OCIOCDLTransformPlugin::getGradeAtTime(double time)
{
    bool readFromFile;
    _readFromFile->getValueAtTime(time, readFromFile);
    if (readFromFile) {
        return getFileGradeAtTime(time);
    }
    CDLGradeResult result;
    result.grade = getParamsGradeAtTime(time);

    return result;
}

OCIO::ConstProcessorRcPtr
OCIOCDLTransformPlugin::getProcessor(OfxTime time)
{
    const CDLGradeResult result = getGradeAtTime(time);
    if (!result.grade) {
        persistentErrorAndThrow(*this, result.error);
    }
    const int directioni = _direction->getValueAtTime(time);

    return processorOrFail([&] { return processorForGrade(*result.grade, directioni); });
} // OCIOCDLTransformPlugin::getProcessor

OCIO::ConstProcessorRcPtr
OCIOCDLTransformPlugin::processorForGrade(const CDLGrade& grade,
                                          int directioni)
{
    AutoSetAndRestoreThreadLocale locale;
    const OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig(); // (never null)
    const ProcKey key { OCIOConfigContextKey(config), grade, directioni };

    return _procCache.get(key, [&] { return config->getProcessor(createCDLTransform(grade, directioni)); });
}

void
OCIOCDLTransformPlugin::apply(double time,
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

    processor.setProcessor(getProcessor(time));

    // set the render window
    processor.setRenderWindow(renderWindow, renderScale);

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
OCIOCDLTransformPlugin::contextAttached(bool createContextData)
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
OCIOCDLTransformPlugin::contextDetached(void* contextData)
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
OCIOCDLTransformPlugin::renderGPU(const RenderArguments& args)
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
OCIOCDLTransformPlugin::render(const RenderArguments& args)
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
} // OCIOCDLTransformPlugin::render

bool
OCIOCDLTransformPlugin::isIdentity(const IsIdentityArguments& args,
                                   Clip*& identityClip,
                                   double& /*identityTime*/
                                   ,
                                   int& /*view*/, std::string& /*plane*/)
{
    const double time = args.time;
    // if the CDL file can't be read, the grade is not an identity: render() reports the error
    const CDLGradeResult result = getGradeAtTime(time);
    if (result.grade) {
        const int directioni = _direction->getValueAtTime(time);
        try {
            // (the processor is cached: render() then uses the same one)
            if (processorForGrade(*result.grade, directioni)->isNoOp()) {
                identityClip = _srcClip;

                return true;
            }
        } catch (const std::exception& e) {
            persistentErrorAndThrow(*this, e.what());
        }
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
} // OCIOCDLTransformPlugin::isIdentity

void
OCIOCDLTransformPlugin::refreshKnobEnabledState(bool readFromFile)
{
    if (readFromFile) {
        _slope->setEnabled(false);
        _offset->setEnabled(false);
        _power->setEnabled(false);
        _saturation->setEnabled(false);

        // We leave these active to allow knob re-use with the import/export buttons
        // m_fileKnob->enable();
        // m_cccidKnob->enable();
    } else {
        _slope->setEnabled(true);
        _offset->setEnabled(true);
        _power->setEnabled(true);
        _saturation->setEnabled(true);

        // We leave these active to allow knob re-use with the import/export buttons
        // m_fileKnob->disable();
        // m_cccidKnob->disable();
    }
}

void
OCIOCDLTransformPlugin::loadCDLFileIntoParams(double time,
                                              bool userEdit)
{
    const CDLGradeResult result = getFileGradeAtTime(time);
    if (!result.grade) {
        // sendMessage() (a modal dialog in Natron) only if the host has no persistent message (OFX 1.4 hosts without
        // OfxMessageSuiteV2)
        if ((setPersistentMessage(Message::eMessageError, "", result.error) == Message::eMessageReplyFailed) && userEdit) {
            sendMessage(Message::eMessageError, "", result.error);
        }

        return;
    }
    setGradeParams(*result.grade);
}

void
OCIOCDLTransformPlugin::setGradeParams(const CDLGrade& grade)
{
    const std::array<double, 9>& sop = grade.sop;
    _slope->deleteAllKeys();
    _slope->setValue(sop[0], sop[1], sop[2]);
    _offset->deleteAllKeys();
    _offset->setValue(sop[3], sop[4], sop[5]);
    _power->deleteAllKeys();
    _power->setValue(sop[6], sop[7], sop[8]);
    _saturation->deleteAllKeys();
    _saturation->setValue(grade.saturation);
}

// open a new file (UTF-8 path) for writing; fail with errno == EEXIST if it already exists
static std::FILE*
createNewFile(const string& path)
{
#ifdef _WIN32
    // not fopen's "x" mode: it's C11, which msvcrt.dll (MinGW64 builds) doesn't support, only the UCRT
    const int fd = ::_wopen(OFX::utf8_to_utf16(path).c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_TRUNC | _O_TEXT, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        return NULL;
    }
    std::FILE* file = ::_wfdopen(fd, L"w");
    if (!file) {
        ::_close(fd);
    }

    return file;
#else
    // "x": C11 exclusive create (glibc, macOS)
    return OFX::fopen_utf8(path.c_str(), "wx");
#endif
}

// write the grade at time to a new ColorCorrection XML file (.cc), or report why it can't be written
void
OCIOCDLTransformPlugin::exportCDL(double time,
                                  const string& exportName)
{
    if (exportName.empty()) {
        sendMessage(Message::eMessageError, "", "Export file name is empty, please enter a valid non-existing file name.");

        return;
    }
    if (_direction->getValueAtTime(time) != 0) {
        // the .cc format has no direction: the file would apply the forward grade
        sendMessage(Message::eMessageError, "", "The ColorCorrection (.cc) format cannot express an inverse grade: set Direction to Forward to export this grade.");

        return;
    }
    // the grade that is applied (the CDL file's if "Read from file" is on)
    const CDLGradeResult result = getGradeAtTime(time);
    if (!result.grade) {
        sendMessage(Message::eMessageError, "", string("Cannot export the grade: ") + result.error);

        return;
    }

    string xml; // the grade, as a ColorCorrection XML file
    try {
        AutoSetAndRestoreThreadLocale locale;
        const OCIO::ConstConfigRcPtr config = OCIO::GetCurrentConfig(); // (never null)
        const OCIO::CDLTransformRcPtr cc = createCDLTransform(*result.grade, 0); // forward
        // the same ColorCorrection XML (.cc) as OCIO 1's CDLTransform::getXML()
        OCIO::GroupTransformRcPtr g = OCIO::GroupTransform::Create();
        g->appendTransform(cc);
        std::ostringstream os;
        os.imbue(std::locale::classic());
        g->write(config, "ColorCorrection", os);
        xml = os.str();
    } catch (const std::exception& e) {
        sendMessage(Message::eMessageError, "", string("Cannot export the grade: ") + e.what());

        return;
    }
    // create a new file, never overwrite an existing one (atomically, even if it can't be read)
    std::FILE* file = createNewFile(exportName);
    if (!file) {
        if (errno == EEXIST) {
            sendMessage(Message::eMessageError, "", string("File ") + exportName + " already exists, please select another filename");
        } else {
            sendMessage(Message::eMessageError, "", string("File ") + exportName + " cannot be written");
        }

        return;
    }
    const bool written = std::fputs(xml.c_str(), file) >= 0;
    if ((std::fclose(file) != 0) || !written) {
        sendMessage(Message::eMessageError, "", string("Error while writing file ") + exportName);
    }
} // OCIOCDLTransformPlugin::exportCDL

void
OCIOCDLTransformPlugin::changedParam(const InstanceChangedArgs& args,
                                     const string& paramName)
{
    // must clear persistent message, or render() is not called by Nuke after an error
    clearPersistentMessage();
    changedPremultParam(args, paramName); // (#457)

    if ((paramName == kParamReadFromFile) || (paramName == kParamFile) || (paramName == kParamCCCID) || (paramName == kParamReload)) {
        if (paramName == kParamReload) {
            OCIO::ClearAllCaches();
            // invalidate the node cache and _cdlFileCache (before loading the file: the version change clears the
            // persistent message)
            _version->setValue(_version->getValue() + 1);
        }
        bool readFromFile;
        _readFromFile->getValueAtTime(args.time, readFromFile);
        refreshKnobEnabledState(readFromFile);
        // kOfxActionInstanceChanged is where the file is loaded into the params (paramSetValue)
        if (readFromFile) {
            loadCDLFileIntoParams(args.time, args.reason == eChangeUserEdit);
        } else if (paramName == kParamReadFromFile) {
            // "Read from file" was turned off: keep the grade that was applied, the file's, rather than jump to the
            // params, which may be stale (they are only loaded from the file by changedParam()). An unreadable file
            // leaves them unchanged, without an error: the file is no longer used.
            const CDLGradeResult result = getFileGradeAtTime(args.time);
            if (result.grade) {
                setGradeParams(*result.grade);
            }
        }
    } else if ((paramName == kParamExport) && (args.reason == eChangeUserEdit)) {
        string exportName;
        _export->getValueAtTime(args.time, exportName);
        // Export is a side effect: errors are reported with sendMessage() (not a persistent message, which would
        // stop rendering), and the param is always reset to its default (empty: exportCDL() refuses to export, see #302)
        exportCDL(args.time, exportName);
        _export->setValue(string());
#if defined(OFX_SUPPORTS_OPENGLRENDER)
    } else if (paramEffectsOpenGLAndTileSupport(paramName)) {
        setSupportsOpenGLAndTileInfoAtTime(args.time);
#endif
    }
} // OCIOCDLTransformPlugin::changedParam

void
OCIOCDLTransformPlugin::changedClip(const InstanceChangedArgs& args,
                                    const string& clipName)
{
    if ((clipName == kOfxImageEffectSimpleSourceClipName) && _srcClip && (args.reason == eChangeUserEdit)) {
        changedSrcClip(_srcClip);
    }
}

mDeclarePluginFactory(OCIOCDLTransformPluginFactory, { ofxsThreadSuiteCheck(); }, {});

/** @brief The basic describe function, passed a plugin descriptor */
void
OCIOCDLTransformPluginFactory::describe(ImageEffectDescriptor& desc)
{
    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    desc.setPluginDescription(kPluginDescription);

    // add the supported contexts
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextPaint);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthFloat);

    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);

#if defined(OFX_SUPPORTS_OPENGLRENDER)
    desc.setSupportsOpenGLRender(true);
#endif
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
OCIOCDLTransformPluginFactory::describeInContext(ImageEffectDescriptor& desc,
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

    // ASC CDL grade numbers
    {
        RGBParamDescriptor* param = desc.defineRGBParam(kParamSlope);
        param->setLabel(kParamSlopeLabel);
        param->setHint(kParamSlopeHint);
        param->setRange(kParamSlopeMin, kParamSlopeMin, kParamSlopeMin, kParamSlopeMax, kParamSlopeMax, kParamSlopeMax);
        param->setDisplayRange(kParamSlopeMin, kParamSlopeMin, kParamSlopeMin, kParamSlopeMax, kParamSlopeMax, kParamSlopeMax);
        param->setDefault(1., 1., 1.);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        RGBParamDescriptor* param = desc.defineRGBParam(kParamOffset);
        param->setLabel(kParamOffsetLabel);
        param->setHint(kParamOffsetHint);
        param->setRange(kParamOffsetMin, kParamOffsetMin, kParamOffsetMin, kParamOffsetMax, kParamOffsetMax, kParamOffsetMax);
        param->setDisplayRange(kParamOffsetMin, kParamOffsetMin, kParamOffsetMin, kParamOffsetMax, kParamOffsetMax, kParamOffsetMax);
        param->setDefault(0., 0., 0.);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        RGBParamDescriptor* param = desc.defineRGBParam(kParamPower);
        param->setLabel(kParamPowerLabel);
        param->setHint(kParamPowerHint);
        param->setRange(kParamPowerMin, kParamPowerMin, kParamPowerMin, kParamPowerMax, kParamPowerMax, kParamPowerMax);
        param->setDisplayRange(kParamPowerMin, kParamPowerMin, kParamPowerMin, kParamPowerMax, kParamPowerMax, kParamPowerMax);
        param->setDefault(1., 1., 1.);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kParamSaturation);
        param->setLabel(kParamSaturationLabel);
        param->setHint(kParamSaturationHint);
        param->setRange(kParamSaturationMin, kParamSaturationMax);
        param->setDisplayRange(kParamSaturationMin, kParamSaturationMax);
        param->setDefault(1.);
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
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamReadFromFile);
        param->setLabel(kParamReadFromFileLabel);
        param->setHint(kParamReadFromFileHint);
        param->setAnimates(false);
        param->setDefault(false);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamFile);
        param->setLabel(kParamFileLabel);
        param->setHint(kParamFileHint);
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
        StringParamDescriptor* param = desc.defineStringParam(kParamExport);
        param->setLabel(kParamExportLabel);
        param->setHint(kParamExportHint);
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(false); // necessary for output files
        param->setEvaluateOnChange(false);
        param->setIsPersistent(false);
        param->setAnimates(false);
        // the default is empty (a placeholder default would be a valid file name, #302)
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
} // OCIOCDLTransformPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
OCIOCDLTransformPluginFactory::createInstance(OfxImageEffectHandle handle,
                                              ContextEnum /*context*/)
{
    return new OCIOCDLTransformPlugin(handle);
}

static OCIOCDLTransformPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

    OFXS_NAMESPACE_ANONYMOUS_EXIT

#endif // OFX_IO_USING_OCIO
