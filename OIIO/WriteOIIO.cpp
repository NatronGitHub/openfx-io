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
 * OFX oiio Writer plugin.
 * Writes an image using the OpenImageIO library.
 */

#include <cfloat> // DBL_MAX

#include "ofxsMacros.h"

#include "OIIOGlobal.h"
GCC_DIAG_OFF(unused-parameter)
#include <OpenImageIO/filesystem.h>
GCC_DIAG_ON(unused-parameter)

#include "GenericOCIO.h"
#include "GenericWriter.h"

#include <ofxsMultiPlane.h>
#include <ofxsCoords.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <IlmThreadPool.h>
#endif

using namespace OFX;
using namespace OFX::IO;
#ifdef OFX_IO_USING_OCIO
namespace OCIO = OCIO_NAMESPACE;
#endif

using std::string;
using std::stringstream;
using std::vector;
using std::map;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#if OIIO_PLUGIN_VERSION >= 22
// OIIO_VERSION_MAJOR >= 2
typedef std::unique_ptr<ImageOutput> ImageOutputPtr;
#else
typedef ImageOutput* ImageOutputPtr;
#endif

#define kPluginName "WriteOIIO"
#define kPluginGrouping "Image/Writers"
#define kPluginDescription "Write images using OpenImageIO."
#define kPluginIdentifier "fr.inria.openfx.WriteOIIO"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.
#define kPluginEvaluation 91 // plugin quality from 0 (bad) to 100 (perfect) or -1 if not evaluated

#define kSupportsRGBA true
#define kSupportsRGB true
#define kSupportsXY true
#define kSupportsAlpha true

#define kParamBitDepth    "bitDepth"
#define kParamBitDepthLabel   "Bit Depth"
#define kParamBitDepthHint \
    "Number of bits per sample in the file [TIFF,DPX,TGA,DDS,ICO,IFF,PNM,PIC]."

#define kParamBitDepthOptionAuto     "auto", "Guess from the output format", "auto"
//#define kParamBitDepthNone     "none"
#define kParamBitDepthOption8      "8i", "8  bits integer", "8i"
#define kParamBitDepthOption10     "10i", "10 bits integer", "10i"
#define kParamBitDepthOption12     "12i", "12 bits integer", "12i"
#define kParamBitDepthOption16     "16i", "16 bits integer", "16i"
#define kParamBitDepthOption16f    "16f", "16 bits floating point", "16f"
#define kParamBitDepthOption32     "32i", "32 bits integer", "32i"
#define kParamBitDepthOption32f    "32f", "32 bits floating point", "32f"
#define kParamBitDepthOption64     "64i", "64 bits integer", "64i"
#define kParamBitDepthOption64f    "64f", "64 bits floating point", "64f"


enum ETuttlePluginBitDepth
{
    eTuttlePluginBitDepthAuto = 0,
    eTuttlePluginBitDepth8,
    eTuttlePluginBitDepth10,
    eTuttlePluginBitDepth12,
    eTuttlePluginBitDepth16,
    eTuttlePluginBitDepth16f,
    eTuttlePluginBitDepth32,
    eTuttlePluginBitDepth32f,
    eTuttlePluginBitDepth64,
    eTuttlePluginBitDepth64f
};

enum ETuttlePluginComponents
{
    eTuttlePluginComponentsAuto = 0,
    eTuttlePluginComponentsGray,
    eTuttlePluginComponentsRGB,
    eTuttlePluginComponentsRGBA
};

#define kParamOutputQuality        "quality"
#define kParamOutputQualityLabel   "Quality"
#define kParamOutputQualityHint \
    "Indicates the quality of compression to use (0-100), for those plugins and compression methods that allow a variable amount of compression, with higher numbers indicating higher image fidelity. [JPEG, TIFF w/ JPEG comp., WEBP]"
#define kParamOutputQualityDefault 100

#define kParamOutputDWACompressionLevel        "dwaCompressionLevel"
#define kParamOutputDWACompressionLevelLabel   "DWA Compression Level"
#define kParamOutputDWACompressionLevelHint \
    "Amount of compression when using Dreamworks DWAA or DWAB compression options. These lossy formats are variable in quality and can minimize the compression artifacts. Higher values will result in greater compression and likewise smaller file size, but increases the chance for artifacts. Values from 45 to 150 are usually correct for production shots, whereas HDR vacation photos could use up to 500. Values below 45 should give no visible imprrovement on photographs. [EXR w/ DWAa or DWAb comp.]"
#define kParamOutputDWACompressionLevelDefault 45

#define kParamOutputOrientation        "orientation"
#define kParamOutputOrientationLabel   "Orientation"
#define kParamOutputOrientationHint \
    "The orientation of the image data [DPX,TIFF,JPEG,HDR,FITS].\n" \
    "By default, image pixels are ordered from the top of the display to the bottom, " \
    "and within each scanline, from left to right (i.e., the same ordering as English " \
    "text and scan progression on a CRT). But the \"Orientation\" parameter can " \
    "suggest that it should be displayed with a different orientation, according to " \
    "the TIFF/EXIF conventions."
/*
   TIFF defines these values:

   1 = The 0th row represents the visual top of the image, and the 0th column represents the visual left-hand side.
   2 = The 0th row represents the visual top of the image, and the 0th column represents the visual right-hand side.
   3 = The 0th row represents the visual bottom of the image, and the 0th column represents the visual right-hand side.
   4 = The 0th row represents the visual bottom of the image, and the 0th column represents the visual left-hand side.
   5 = The 0th row represents the visual left-hand side of the image, and the 0th column represents the visual top.
   6 = The 0th row represents the visual right-hand side of the image, and the 0th column represents the visual top.
   7 = The 0th row represents the visual right-hand side of the image, and the 0th column represents the visual bottom.
   8 = The 0th row represents the visual left-hand side of the image, and the 0th column represents the visual bottom.
 */
#define kParamOutputOrientationNormal                "normal", "normal (top to bottom, left to right)", "normal"
#define kParamOutputOrientationFlop                  "flop", "flipped horizontally (top to bottom, right to left)", "flop"
#define kParamOutputOrientationR180                  "180", "rotate 180deg (bottom to top, right to left)", "180"
#define kParamOutputOrientationFlip                  "flip", "flipped vertically (bottom to top, left to right)", "flip"
#define kParamOutputOrientationTransposed            "transposed", "transposed (left to right, top to bottom)", "transposed"
#define kParamOutputOrientationR90Clockwise          "90clockwise", "rotated 90deg clockwise (right to left, top to bottom)", "90clockwise"
#define kParamOutputOrientationTransverse            "transverse", "transverse (right to left, bottom to top)", "transverse"
#define kParamOutputOrientationR90CounterClockwise   "90counter-clockwise", "rotated 90deg counter-clockwise (left to right, bottom to top)", "90counter-clockwise"
enum EOutputOrientation
{
    eOutputOrientationNormal = 0,
    eOutputOrientationFlop,
    eOutputOrientationR180,
    eOutputOrientationFlip,
    eOutputOrientationTransposed,
    eOutputOrientationR90Clockwise,
    eOutputOrientationTransverse,
    eOutputOrientationR90CounterClockwise,
};

#define kParamOutputCompression        "compression"
#define kParamOutputCompressionLabel   "Compression"
#define kParamOutputCompressionHint \
    "Compression type [TIFF,EXR,DDS,IFF,SGI,TGA]\n" \
    "Indicates the type of compression the file uses. Supported compression modes will vary from format to format. " \
    "As an example, the TIFF format supports \"none\", \"lzw\", \"ccittrle\", \"zip\" (the default), \"jpeg\", \"packbits\", " \
    "and the EXR format supports \"none\", \"rle\", \"zip\" (the default), \"piz\", \"pxr24\", \"b44\", \"b44a\", " \
    "\"dwaa\" or \"dwab\"."

#define kParamOutputCompressionOptionAuto        "default", "Guess from the output format", "default"
#define kParamOutputCompressionOptionNone        "none", "No compression [EXR, TIFF, IFF]", "none"
#define kParamOutputCompressionOptionZip         "zip", "Zlib/Deflate compression (lossless) [EXR, TIFF, Zfile]", "zip"
#define kParamOutputCompressionOptionZips        "zips", "Zlib compression (lossless), one scan line at a time [EXR]", "zips"
#define kParamOutputCompressionOptionRle         "rle", "Run Length Encoding (lossless) [DPX, IFF, EXR, TGA, RLA]", "rle"
#define kParamOutputCompressionOptionPiz         "piz", "Piz-based wavelet compression [EXR]", "piz"
#define kParamOutputCompressionOptionPxr24       "pxr24", "Lossy 24bit float compression [EXR]", "pxr24"
#define kParamOutputCompressionOptionB44         "b44", "Lossy 4-by-4 pixel block compression, fixed compression rate [EXR]", "b44"
#define kParamOutputCompressionOptionB44a        "b44a", "Lossy 4-by-4 pixel block compression, flat fields are compressed more [EXR]", "b44a"
#define kParamOutputCompressionOptionDWAa        "dwaa", "lossy DCT based compression, in blocks of 32 scanlines. More efficient for partial buffer access. [EXR]", "dwaa"
#define kParamOutputCompressionOptionDWAb        "dwab", "lossy DCT based compression, in blocks of 256 scanlines. More efficient space wise and faster to decode full frames than DWAA. [EXR]", "dwab"
#define kParamOutputCompressionOptionLZW         "lzw", "Lempel-Ziv Welsch compression (lossless) [TIFF]", "lzw"
#define kParamOutputCompressionOptionCCITTRLE    "ccittrle", "CCITT modified Huffman RLE (lossless) [TIFF]", "ccittrle"
#define kParamOutputCompressionOptionJPEG        "jpeg", "JPEG [TIFF]", "jpeg"
#define kParamOutputCompressionOptionPACKBITS    "packbits", "Macintosh RLE (lossless) [TIFF]", "packbits"

enum EParamCompression
{
    eParamCompressionAuto = 0,
    eParamCompressionNone,
    eParamCompressionZip,
    eParamCompressionZips,
    eParamCompressionRle,
    eParamCompressionPiz,
    eParamCompressionPxr24,
    eParamCompressionB44,
    eParamCompressionB44a,
    eParamCompressionDWAa,
    eParamCompressionDWAb,
    eParamCompressionLZW,
    eParamCompressionCCITTRLE,
    eParamCompressionJPEG,
    eParamCompressionPACKBITS
};

#define kParamTileSize "tileSize"
#define kParamTileSizeLabel "Tile Size"
#define kParamTileSizeHint "Size of a tile in the output file for formats that support tiles. If scan-line based, the whole image will have a single tile."
#define kParamTileSizeOptionScanLineBased "Scan-Line Based", "", "0"
#define kParamTileSizeOption64 "64", "", "64"
#define kParamTileSizeOption128 "128", "", "128"
#define kParamTileSizeOption256 "256", "", "256"
#define kParamTileSizeOption512 "512", "", "512"

enum EParamTileSize
{
    eParamTileSizeScanLineBased = 0,
    eParamTileSize64,
    eParamTileSize128,
    eParamTileSize256,
    eParamTileSize512
};

#define kParamProcessAllLayers "processAllLayers"
#define kParamProcessAllLayersLabel "All Layers"
#define kParamProcessAllLayersHint "When checked, all layers will be written to the file"

#define kParamOutputChannels kNatronOfxParamOutputChannels
#define kParamOutputChannelsChoice kParamOutputChannels "Choice"
#define kParamOutputChannelsLabel "Layer(s)"
#define kParamOutputChannelsHint "Select which layer to write to the file. This is either All or a single layer. " \
    "This is not yet possible to append a layer to an existing file."

#define kParamPartsSplitting "partSplitting"
#define kParamPartsSplittingLabel "Parts"
#define kParamPartsSplittingHint "Defines whether to separate views/layers in different EXR parts or not. " \
    "Note that multi-part files are only supported by OpenEXR >= 2"

#define kParamPartsSinglePart kParamPartsSinglePartOption, kParamPartsSinglePartOptionHint, kParamPartsSinglePartOptionEnum
#define kParamPartsSinglePartOption "Single Part"
#define kParamPartsSinglePartOptionHint "All views and layers will be in the same part, ensuring compatibility with OpenEXR 1.x"
#define kParamPartsSinglePartOptionEnum "single"

#define kParamPartsSplitViews kParamPartsSplitViewsOption, kParamPartsSplitViewsOptionHint, kParamPartsSplitViewsOptionEnum
#define kParamPartsSplitViewsOption "Split Views"
#define kParamPartsSplitViewsOptionHint "All views will have its own part, and each part will contain all layers. This will produce an EXR optimized in size that " \
    "can be opened only with applications supporting OpenEXR 2"
#define kParamPartsSplitViewsOptionEnum "views"

#define kParamPartsSplitViewsLayers kParamPartsSplitViewsLayersOption, kParamPartsSplitViewsLayersOptionHint, kParamPartsSplitViewsLayersOptionEnum
#define kParamPartsSplitViewsLayersOption "Split Views,Layers"
#define kParamPartsSplitViewsLayersOptionHint "Each layer of each view will have its own part. This will produce an EXR optimized for decoding speed that " \
    "can be opened only with applications supporting OpenEXR 2"
#define kParamPartsSplitViewsLayersOptionEnum "views_layers"


#define kParamViewsSelector "viewsSelector"
#define kParamViewsSelectorLabel "Views"
#define kParamViewsSelectorHint "Select the views to render. When choosing All, make sure the output filename does not have a %v or %V view " \
    "pattern in which case each view would be written to a separate file."

#define kParamLibraryInfo "libraryInfo"
#define kParamLibraryInfoLabel "OpenImageIO Info...", "Display information about the underlying library."

static bool gIsMultiplanarV2=false;

class WriteOIIOPlugin
    : public GenericWriterPlugin
{
public:
    WriteOIIOPlugin(OfxImageEffectHandle handle, const vector<string>& extensions);

    virtual ~WriteOIIOPlugin();

    virtual void changedParam(const InstanceChangedArgs &args, const string &paramName) OVERRIDE FINAL;
    virtual void getClipPreferences(ClipPreferencesSetter &clipPreferences) OVERRIDE FINAL;
    virtual OfxStatus getClipComponents(const ClipComponentsArguments& args, ClipComponentsSetter& clipComponents) OVERRIDE FINAL;

private:

    virtual LayerViewsPartsEnum getPartsSplittingPreference() const OVERRIDE FINAL;
    virtual int getViewToRender() const OVERRIDE FINAL;
    virtual void onOutputFileChanged(const string& filename, bool setColorSpace) OVERRIDE FINAL;

    /**
     * @brief Does the given filename support alpha channel.
     **/
    virtual bool supportsAlpha(const std::string&) const OVERRIDE FINAL;

    virtual void encode(const string& filename,
                        const OfxTime time,
                        const string& viewName,
                        const float *pixelData,
                        const OfxRectI& bounds,
                        const float pixelAspectRatio,
                        const int pixelDataNComps,
                        const int pixelDataNCompsStartIndex,
                        const int dstNComps,
                        const int rowBytes) OVERRIDE FINAL
    {
        string rawComps(kFnOfxImagePlaneColour);

        switch (dstNComps) {
        case 1:
            rawComps = kOfxImageComponentAlpha;
            break;
        case 3:
            rawComps = kOfxImageComponentRGB;
            break;
        case 4:
            rawComps = kOfxImageComponentRGBA;
            break;
        case 2:
            rawComps = kFnOfxImageComponentMotionVectors;
            break;
        default:
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }

        std::list<string> comps;
        comps.push_back(rawComps);
        EncodePlanesLocalData_RAII data(this);
        map<int, string> viewsToRender;
        viewsToRender[0] = viewName;

        vector<int> packingMapping(dstNComps);
        for (int i = 0; i < dstNComps; ++i) {
            packingMapping[i] = pixelDataNCompsStartIndex + i;
        }

        beginEncodeParts(data.getData(), filename, time, pixelAspectRatio, eLayerViewsSinglePart, viewsToRender, comps, false, packingMapping, bounds);
        encodePart(data.getData(), filename, pixelData, pixelDataNComps, 0, rowBytes);
        endEncodeParts( data.getData() );
    }

    virtual void encodePart(void* user_data, const string& filename, const float *pixelData, int pixelDataNComps, int planeIndex, int rowBytes) OVERRIDE FINAL;
    virtual void beginEncodeParts(void* user_data,
                                  const string& filename,
                                  OfxTime time,
                                  float pixelAspectRatio,
                                  LayerViewsPartsEnum partsSplitting,
                                  const map<int, string>& viewsToRender,
                                  const std::list<string>& planes,
                                  const bool packingRequired,
                                  const vector<int>& packingMapping,
                                  const OfxRectI& bounds) OVERRIDE FINAL;

    void endEncodeParts(void* user_data) OVERRIDE FINAL;

    virtual void* allocateEncodePlanesUserData() OVERRIDE FINAL;
    virtual void destroyEncodePlanesUserData(void* data) OVERRIDE FINAL;
    virtual bool isImageFile(const string& fileExtension) const OVERRIDE FINAL;
    virtual PreMultiplicationEnum getExpectedInputPremultiplication() const OVERRIDE FINAL { return eImagePreMultiplied; }

    virtual bool displayWindowSupportedByFormat(const string& filename) const OVERRIDE FINAL;

    void refreshParamsVisibility(const string& filename);

    void refreshCheckboxesLabels();

private:
    ChoiceParam* _bitDepth;
    IntParam* _quality;
    DoubleParam* _dwaCompressionLevel;
    ChoiceParam* _orientation;
    ChoiceParam* _compression;
    ChoiceParam* _tileSize;
    ChoiceParam* _outputLayers;
    ChoiceParam* _parts;
    ChoiceParam* _views;
    std::list<string> _currentInputComponents;
    std::list<string> _availableViews;
};

WriteOIIOPlugin::WriteOIIOPlugin(OfxImageEffectHandle handle,
                                 const vector<string>& extensions)
    : GenericWriterPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha)
    , _bitDepth(NULL)
    , _quality(NULL)
    , _dwaCompressionLevel(NULL)
    , _orientation(NULL)
    , _compression(NULL)
    , _tileSize(NULL)
    , _outputLayers(NULL)
    , _parts(NULL)
    , _views(NULL)
    , _currentInputComponents()
    , _availableViews()
{

    _bitDepth = fetchChoiceParam(kParamBitDepth);
    _quality     = fetchIntParam(kParamOutputQuality);
    _dwaCompressionLevel = fetchDoubleParam(kParamOutputDWACompressionLevel);
    _orientation = fetchChoiceParam(kParamOutputOrientation);
    _compression = fetchChoiceParam(kParamOutputCompression);
    _tileSize = fetchChoiceParam(kParamTileSize);
    if (gIsMultiplanarV2) {
        _outputLayers = fetchChoiceParam(kParamOutputChannels);

        {
            FetchChoiceParamOptions args = FetchChoiceParamOptions::createFetchChoiceParamOptionsForOutputPlane();
            args.dependsClips.push_back(_inputClip);
            fetchDynamicMultiplaneChoiceParameter(kParamOutputChannels, args);
        }
        onAllParametersFetched();
        _parts = fetchChoiceParam(kParamPartsSplitting);
        _views = fetchChoiceParam(kParamViewsSelector);
    }

    string filename;
    _fileParam->getValue(filename);
    refreshParamsVisibility(filename);
    initOIIOThreads();
}

WriteOIIOPlugin::~WriteOIIOPlugin()
{
}

namespace  {
static bool
hasListChanged(const std::list<string>& oldList,
               const std::list<string>& newList)
{
    if ( oldList.size() != newList.size() ) {
        return true;
    }

    std::list<string>::const_iterator itNew = newList.begin();
    for (std::list<string>::const_iterator it = oldList.begin(); it != oldList.end(); ++it, ++itNew) {
        if (*it != *itNew) {
            return true;
        }
    }

    return false;
}
}


static string
oiio_versions()
{
    std::ostringstream oss;
    int ver = openimageio_version();
    oss << "OpenImageIO version (compiled with / running with): " << OIIO_VERSION_STRING << '/';
    oss << ver / 10000 << '.' << (ver % 10000) / 100 << '.' << (ver % 100) << std::endl;
# if OIIO_VERSION >= 10705
    string libs = OIIO::get_string_attribute("library_list");
    if (libs.size()) {
        oss << std::endl << "Dependent libraries:" << std::endl;
        std::istringstream f(libs);
        string s;
        while (getline(f, s, ';')) {
            size_t pos = s.find(':');
            oss << s.substr(pos+1) << std::endl;
        }
    }
# endif

    return oss.str();
}


void
WriteOIIOPlugin::changedParam(const InstanceChangedArgs &args,
                              const string &paramName)
{
    if (paramName == kParamLibraryInfo) {
        string extensions_list;
        getattribute("extension_list", extensions_list);

        string extensions_pretty;
        {
            stringstream formatss(extensions_list);
            string format;
            vector<string> extensions;
            while ( std::getline(formatss, format, ';') ) {
                stringstream extensionss(format);
                string extension;
                std::getline(extensionss, extension, ':'); // extract the format
                extensions_pretty += extension;
                extensions_pretty += ": ";
                bool first = true;
                while ( std::getline(extensionss, extension, ',') ) {
                    if (!first) {
                        extensions_pretty += ", ";
                    }
                    first = false;
                    extensions_pretty += extension;
                }
                extensions_pretty += "; ";
            }
        }
        string msg = oiio_versions() + "\nAll supported formats and extensions: " + extensions_pretty;
        sendMessage(Message::eMessageMessage, "", msg);
    } else if ( (paramName == kParamOutputCompression) && (args.reason == eChangeUserEdit) ) {
        string filename;
        _fileParam->getValue(filename);
        refreshParamsVisibility(filename);
    } else if (paramName == kParamOutputChannels) {
    }

    GenericWriterPlugin::changedParam(args, paramName);
}

void
WriteOIIOPlugin::getClipPreferences(ClipPreferencesSetter &clipPreferences)
{
    GenericWriterPlugin::getClipPreferences(clipPreferences);

    if ( gIsMultiplanarV2 ) {
        string filename;
        _fileParam->getValue(filename);
#     if OIIO_PLUGIN_VERSION >= 22
        ImageOutputPtr output = ImageOutput::create(filename);
#     else
        auto_ptr<ImageOutput> output( ImageOutput::create(filename) );
#     endif
        /*
        bool supportsNChannels = false;
        if ( output.get() ) {
            supportsNChannels = output->supports("nchannels");
        }
        */

        MultiPlane::ImagePlaneDesc plane;
        OFX::Clip* clip = 0;
        int channelIndex = -1;
        MultiPlane::MultiPlaneEffect::GetPlaneNeededRetCodeEnum stat = getPlaneNeeded(_outputLayers->getName(), &clip, &plane, &channelIndex);

        if (stat == MultiPlane::MultiPlaneEffect::eGetPlaneNeededRetCodeFailed) {
            _outputComponents->setIsSecretAndDisabled(true);
            for (int i = 0; i < 4; ++i) {
                _processChannels[i]->setIsSecretAndDisabled(true);
            }
        } else {
            _outputComponents->setIsSecretAndDisabled(false);
        }
    }

    if (_views) {
        //Now build the views choice
        std::list<string> views;
        int nViews = getViewCount();
        for (int i = 0; i < nViews; ++i) {
            string view = getViewName(i);
            views.push_back(view);
        }
        if ( hasListChanged(_availableViews, views) ) {
            _availableViews = views;
            if (_views) {
                _views->resetOptions();
                _views->appendOption("All");
                for (std::list<string>::iterator it = views.begin(); it != views.end(); ++it) {
                    _views->appendOption(*it);
                }
            }
        }
    }
}

OfxStatus
WriteOIIOPlugin::getClipComponents(const ClipComponentsArguments& /*args*/,
                                   ClipComponentsSetter& clipComponents)
{
    MultiPlane::ImagePlaneDesc dstPlane;

    OFX::Clip* clip = 0;
    int channelIndex = -1;
    MultiPlane::MultiPlaneEffect::GetPlaneNeededRetCodeEnum stat = getPlaneNeeded(_outputLayers->getName(), &clip, &dstPlane, &channelIndex);
    if (stat == MultiPlane::MultiPlaneEffect::eGetPlaneNeededRetCodeFailed) {
        return kOfxStatFailed;
    }

    if (stat == MultiPlane::MultiPlaneEffect::eGetPlaneNeededRetCodeReturnedAllPlanes) {
        vector<string> components;
        _inputClip->getPlanesPresent(&components);
        for (vector<string>::const_iterator it = components.begin(); it != components.end(); ++it) {
            clipComponents.addClipPlane(*_inputClip, *it);
            clipComponents.addClipPlane(*_outputClip, *it);
        }
    } else {
        assert(stat == MultiPlane::MultiPlaneEffect::eGetPlaneNeededRetCodeReturnedPlane);
        std::string ofxComponentsStr = MultiPlane::ImagePlaneDesc::mapPlaneToOFXPlaneString(dstPlane);
        clipComponents.addClipPlane(*_inputClip, ofxComponentsStr);
        clipComponents.addClipPlane(*_outputClip, ofxComponentsStr);
    }
    return kOfxStatOK;
}

int
WriteOIIOPlugin::getViewToRender() const
{
    if ( !_views || _views->getIsSecret() ) {
        return -2;
    } else {
        int view_i;
        _views->getValue(view_i);

        return view_i - 1;
    }
}

LayerViewsPartsEnum
WriteOIIOPlugin::getPartsSplittingPreference() const
{
    if ( !_parts || _parts->getIsSecret() ) {
        return eLayerViewsSinglePart;
    }
    int index = _parts->getValue();
    string optionEnum;
    _parts->getEnum(index, optionEnum);
    if ( optionEnum.empty() ) {
        // for backward compatibility
        string option;
        _parts->getOption(index, option);
        if (option == kParamPartsSinglePartOption) {
            return eLayerViewsSinglePart;
        } else if (option == kParamPartsSplitViewsOption) {
            return eLayerViewsSplitViews;
        } else if (option == kParamPartsSplitViewsLayersOption) {
            return eLayerViewsSplitViewsLayers;
        }

        return eLayerViewsSinglePart;
    } else if (optionEnum == kParamPartsSinglePartOptionEnum) {
        return eLayerViewsSinglePart;
    } else if (optionEnum == kParamPartsSplitViewsOptionEnum) {
        return eLayerViewsSplitViews;
    } else if (optionEnum == kParamPartsSplitViewsLayersOptionEnum) {
        return eLayerViewsSplitViewsLayers;
    }

    return eLayerViewsSinglePart;
}

/**
 * Deduce the best bitdepth when it hasn't been set by the user
 */
static
ETuttlePluginBitDepth
getDefaultBitDepth(const string& filepath,
                   ETuttlePluginBitDepth bitDepth)
{
    if (bitDepth != eTuttlePluginBitDepthAuto) {
        return bitDepth;
    }
    string format = Filesystem::extension(filepath);
    Strutil::to_lower(format);
    if ( (format == ".exr") ) {
        return eTuttlePluginBitDepth16f; // 16f is the most commonly used bit depth in the EXR world
    } else if ( (format == ".hdr") || (format == ".rgbe") || (format == ".pfm") ) {
        return eTuttlePluginBitDepth32f;
    } else if ( (format == ".jpg") || ( format == ".jpe") ||
               ( format == ".jpeg") || ( format == ".jif") ||
               ( format == ".jfif") || ( format == ".jfi") ||
               ( format == ".bmp") ||
               ( format == ".dds") ||
               ( format == ".ico") ||
               ( format == ".pgm") || ( format == ".pnm") ||
               ( format == ".ppm") || ( format == ".pbm") ||
               ( format == ".pic") ||
               ( format == ".tga") || ( format == ".tpic") ||
               ( format == ".png") ) {
        return eTuttlePluginBitDepth8;
    } else {
        //cin, dpx, fits, heic, heif, j2k, j2c, jp2, jpe, sgi, tif, tiff, tpic, webp
        return eTuttlePluginBitDepth16;
    }

    return bitDepth;
}

bool
WriteOIIOPlugin::displayWindowSupportedByFormat(const string& filename) const
{
# if OIIO_PLUGIN_VERSION >= 22
    ImageOutputPtr output = ImageOutput::create(filename);
# else
    auto_ptr<ImageOutput> output( ImageOutput::create(filename) );
# endif
    if ( output.get() ) {
        return output->supports("displaywindow");
    } else {
        return false;
    }
}

static bool
has_suffix(const string &str,
           const string &suffix)
{
    return (str.size() >= suffix.size() &&
            str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
}

void
WriteOIIOPlugin::onOutputFileChanged(const string &filename,
                                     bool setColorSpace)
{
    if (setColorSpace) {
#     ifdef OFX_IO_USING_OCIO
        int finalBitDepth_i;
        _bitDepth->getValue(finalBitDepth_i);
        ETuttlePluginBitDepth finalBitDepth = getDefaultBitDepth(filename, (ETuttlePluginBitDepth)finalBitDepth_i);

        // no colorspace... we'll probably have to try something else, then.
        // we set the following defaults:
        // sRGB for 8-bit images
        // Rec709 for 10-bits, 12-bits or 16-bits integer images
        // Linear for anything else
        switch (finalBitDepth) {
        case eTuttlePluginBitDepth8: {
            if ( _ocio->hasColorspace("sRGB") ) {
                // nuke-default, blender, natron
                _ocio->setOutputColorspace("sRGB");
            } else if ( _ocio->hasColorspace("sRGB D65") ) {
                // blender-cycles
                _ocio->setOutputColorspace("sRGB D65");
            } else if ( _ocio->hasColorspace("rrt_srgb") ) {
                // rrt_srgb in aces
                _ocio->setOutputColorspace("rrt_srgb");
            } else if ( _ocio->hasColorspace("srgb8") ) {
                // srgb8 in spi-vfx
                _ocio->setOutputColorspace("srgb8");
            }
            break;
        }
        case eTuttlePluginBitDepth10:
        case eTuttlePluginBitDepth12:
        case eTuttlePluginBitDepth16: {
            if ( has_suffix(filename, ".cin") || has_suffix(filename, ".dpx") ||
                 has_suffix(filename, ".CIN") || has_suffix(filename, ".DPX") ) {
                // Cineon or DPX file
                if ( _ocio->hasColorspace("Cineon") ) {
                    // Cineon in nuke-default, blender
                    _ocio->setOutputColorspace("Cineon");
                } else if ( _ocio->hasColorspace("Cineon Log Curve") ) {
                    // Curves/Cineon Log Curve in natron
                    _ocio->setOutputColorspace("Cineon Log Curve");
                } else if ( _ocio->hasColorspace("REDlogFilm") ) {
                    // REDlogFilm in aces 1.0.0
                    _ocio->setOutputColorspace("REDlogFilm");
                } else if ( _ocio->hasColorspace("cineon") ) {
                    // cineon in aces 0.7.1
                    _ocio->setOutputColorspace("cineon");
                } else if ( _ocio->hasColorspace("adx10") ) {
                    // adx10 in aces 0.1.1
                    _ocio->setOutputColorspace("adx10");
                } else if ( _ocio->hasColorspace("lg10") ) {
                    // lg10 in spi-vfx
                    _ocio->setOutputColorspace("lg10");
                } else if ( _ocio->hasColorspace("lm10") ) {
                    // lm10 in spi-anim
                    _ocio->setOutputColorspace("lm10");
                } else {
                    _ocio->setOutputColorspace(OCIO::ROLE_COMPOSITING_LOG);
                }
            } else {
                if ( _ocio->hasColorspace("Rec709") ) {
                    // nuke-default
                    _ocio->setOutputColorspace("Rec709");
                } else if ( _ocio->hasColorspace("nuke_rec709") ) {
                    // blender
                    _ocio->setOutputColorspace("nuke_rec709");
                } else if ( _ocio->hasColorspace("Rec 709 Curve") ) {
                    // natron
                    _ocio->setOutputColorspace("Rec 709 Curve");
                } else if ( _ocio->hasColorspace("Rec.709 - Full") ) {
                    // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                    _ocio->setOutputColorspace("Rec.709 - Full");
                } else if ( _ocio->hasColorspace("out_rec709full") ) {
                    // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                    _ocio->setOutputColorspace("out_rec709full");
                } else if ( _ocio->hasColorspace("rrt_rec709_full_100nits") ) {
                    // rrt_rec709_full_100nits in aces 0.7.1
                    _ocio->setOutputColorspace("rrt_rec709_full_100nits");
                } else if ( _ocio->hasColorspace("rrt_rec709") ) {
                    // rrt_rec709 in aces 0.1.1
                    _ocio->setOutputColorspace("rrt_rec709");
                } else if ( _ocio->hasColorspace("hd10") ) {
                    // hd10 in spi-anim and spi-vfx
                    _ocio->setOutputColorspace("hd10");
                }
            }
            break;
        }
        default:
            _ocio->setOutputColorspace(OCIO::ROLE_SCENE_LINEAR);
        } // switch
#     endif // ifdef OFX_IO_USING_OCIO
    }

    refreshParamsVisibility(filename);
} // WriteOIIOPlugin::onOutputFileChanged


/**
 * @brief Does the given filename support alpha channel.
 **/
bool
WriteOIIOPlugin::supportsAlpha(const std::string& filename) const
{
    auto output = ImageOutput::create(filename);

    return kSupportsRGBA && output->supports("alpha");
}

void
WriteOIIOPlugin::refreshParamsVisibility(const string& filename)
{
# if OIIO_PLUGIN_VERSION >= 22
    ImageOutputPtr output = ImageOutput::create(filename);
# else
    auto_ptr<ImageOutput> output( ImageOutput::create(filename) );
# endif
    if ( output.get() ) {
        _tileSize->setIsSecretAndDisabled( !output->supports("tiles") );
        //_outputLayers->setIsSecretAndDisabled(!output->supports("nchannels"));
        bool hasQuality = (strcmp(output->format_name(), "jpeg") == 0 ||
                           strcmp(output->format_name(), "webp") == 0);
        if ( !hasQuality && (strcmp(output->format_name(), "tiff") == 0) ) {
            int compression_i;
            _compression->getValue(compression_i);
            hasQuality = ( (EParamCompression)compression_i == eParamCompressionJPEG );
        }
        _quality->setIsSecretAndDisabled(!hasQuality);
        bool isEXR = strcmp(output->format_name(), "openexr") == 0;
        bool hasDWA = isEXR;
        if (hasDWA) {
            int compression_i;
            _compression->getValue(compression_i);
            EParamCompression compression = (EParamCompression)compression_i;
            hasDWA = (compression == eParamCompressionDWAa) || (compression == eParamCompressionDWAb);
        }
        _dwaCompressionLevel->setIsSecretAndDisabled(!hasDWA);
        if (_views) {
            _views->setIsSecretAndDisabled(!isEXR);
        }
        if (_parts) {
            _parts->setIsSecretAndDisabled( !output->supports("multiimage") );
        }
    } else {
        _tileSize->setIsSecretAndDisabled(true);
        //_outputLayers->setIsSecretAndDisabled(true);
        _quality->setIsSecretAndDisabled(true);
        _dwaCompressionLevel->setIsSecretAndDisabled(true);
        if (_views) {
            _views->setIsSecretAndDisabled(true);
        }
        if (_parts) {
            _parts->setIsSecretAndDisabled(true);
        }
    }
}

struct WriteOIIOEncodePlanesData
{
# if OIIO_PLUGIN_VERSION >= 22
    ImageOutputPtr output;
#else
    auto_ptr<ImageOutput> output;
#endif
    vector<ImageSpec> specs;
};

void*
WriteOIIOPlugin::allocateEncodePlanesUserData()
{
    WriteOIIOEncodePlanesData* data = new WriteOIIOEncodePlanesData;

    return data;
}

void
WriteOIIOPlugin::destroyEncodePlanesUserData(void* data)
{
    assert(data);
    WriteOIIOEncodePlanesData* d = (WriteOIIOEncodePlanesData*)data;
    delete d;
}

void
WriteOIIOPlugin::beginEncodeParts(void* user_data,
                                  const string& filename,
                                  OfxTime time,
                                  float pixelAspectRatio,
                                  LayerViewsPartsEnum partsSplitting,
                                  const map<int, string>& viewsToRender,
                                  const std::list<string>& planes,
                                  const bool packingRequired,
                                  const vector<int>& packingMapping,
                                  const OfxRectI& bounds)
{
    assert( (packingRequired && planes.size() == 1) || !packingRequired );

    assert( !viewsToRender.empty() );
    assert(user_data);
    WriteOIIOEncodePlanesData* data = (WriteOIIOEncodePlanesData*)user_data;
# if OIIO_PLUGIN_VERSION >= 22
    data->output = ImageOutput::create(filename);
# else
    data->output.reset( ImageOutput::create(filename) );
# endif
    if ( !data->output.get() ) {
        // output is NULL
        setPersistentMessage(Message::eMessageError, "", string("Cannot create output file ") + filename);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    if ( !data->output->supports("multiimage") && (partsSplitting != eLayerViewsSinglePart) ) {
        stringstream ss;
        ss << data->output->format_name() << " does not support writing multiple views/layers into a single file.";
        setPersistentMessage( Message::eMessageError, "", ss.str() );
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    bool isEXR = strcmp(data->output->format_name(), "openexr") == 0;
    if ( !isEXR && (viewsToRender.size() > 1) ) {
        stringstream ss;
        ss << data->output->format_name() << " format cannot render multiple views in a single file, use %v or %V in filename to render separate files per view";
        setPersistentMessage( Message::eMessageError, "", ss.str() );
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }


    OIIO_NAMESPACE::TypeDesc oiioBitDepth;
    //size_t sizeOfChannel = 0;
    int bitsPerSample  = 0;
    int finalBitDepth_i;
    _bitDepth->getValue(finalBitDepth_i);
    ETuttlePluginBitDepth finalBitDepth = getDefaultBitDepth(filename, (ETuttlePluginBitDepth)finalBitDepth_i);

    switch (finalBitDepth) {
    case eTuttlePluginBitDepthAuto:
        throwSuiteStatusException(kOfxStatErrUnknown);

        return;
    case eTuttlePluginBitDepth8:
        oiioBitDepth = TypeDesc::UINT8;
        bitsPerSample = 8;
        //sizeOfChannel = 1;
        break;
    case eTuttlePluginBitDepth10:
        oiioBitDepth = TypeDesc::UINT16;
        bitsPerSample = 10;
        //sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth12:
        oiioBitDepth = TypeDesc::UINT16;
        bitsPerSample = 12;
        //sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth16:
        oiioBitDepth = TypeDesc::UINT16;
        bitsPerSample = 16;
        //sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth16f:
        oiioBitDepth = TypeDesc::HALF;
        bitsPerSample = 16;
        //sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth32:
        oiioBitDepth = TypeDesc::UINT32;
        bitsPerSample = 32;
        //sizeOfChannel = 4;
        break;
    case eTuttlePluginBitDepth32f:
        oiioBitDepth = TypeDesc::FLOAT;
        bitsPerSample = 32;
        //sizeOfChannel = 4;
        break;
    case eTuttlePluginBitDepth64:
        oiioBitDepth = TypeDesc::UINT64;
        bitsPerSample = 64;
        //sizeOfChannel = 8;
        break;
    case eTuttlePluginBitDepth64f:
        oiioBitDepth = TypeDesc::DOUBLE;
        bitsPerSample = 64;
        //sizeOfChannel = 8;
        break;
    } // switch

    //Base spec with a stub nChannels
    ImageSpec spec (bounds.x2 - bounds.x1, bounds.y2 - bounds.y1, 4, oiioBitDepth);
    int quality = 100;
    if ( !_quality->getIsSecret() ) {
        _quality->getValue(quality);
    }
    double dwaCompressionLevel = 45.;
    if ( !_dwaCompressionLevel->getIsSecret() ) {
        _dwaCompressionLevel->getValue(dwaCompressionLevel);
    }
    int orientation;
    _orientation->getValue(orientation);
    int compression_i;
    _compression->getValue(compression_i);
    string compression;

    switch ( (EParamCompression)compression_i ) {
    case eParamCompressionAuto:
        break;
    case eParamCompressionNone:     // EXR, TIFF, IFF
        compression = "none";
        break;
    case eParamCompressionZip:     // EXR, TIFF, Zfile
        compression = "zip";
        break;
    case eParamCompressionZips:     // EXR
        compression = "zips";
        break;
    case eParamCompressionRle:     // DPX, IFF, EXR, TGA, RLA
        compression = "rle";
        break;
    case eParamCompressionPiz:     // EXR
        compression = "piz";
        break;
    case eParamCompressionPxr24:     // EXR
        compression = "pxr24";
        break;
    case eParamCompressionB44:     // EXR
        compression = "b44";
        break;
    case eParamCompressionB44a:     // EXR
        compression = "b44a";
        break;
    case eParamCompressionDWAa:     // EXR
        compression = "dwaa";
        break;
    case eParamCompressionDWAb:     // EXR
        compression = "dwab";
        break;
    case eParamCompressionLZW:     // TIFF
        compression = "lzw";
        break;
    case eParamCompressionCCITTRLE:     // TIFF
        compression = "ccittrle";
        break;
    case eParamCompressionJPEG:     // TIFF
        compression = "jpeg";
        break;
    case eParamCompressionPACKBITS:     // TIFF
        compression = "packbits";
        break;
    }

    spec.attribute("oiio:BitsPerSample", bitsPerSample);
    // oiio:UnassociatedAlpha should be set if the data buffer is unassociated/unpremultiplied.
    // However, WriteOIIO::getExpectedInputPremultiplication() stated that input to the encode()
    // function should always be premultiplied/associated
    //spec.attribute("oiio:UnassociatedAlpha", premultiply);
#ifdef OFX_IO_USING_OCIO
    string ocioColorspace;
    _ocio->getOutputColorspaceAtTime(time, ocioColorspace);
    float gamma = 0.f;
    string colorSpaceStr;
    if (ocioColorspace == "Gamma1.8") {
        // Gamma1.8 in nuke-default
        colorSpaceStr = "GammaCorrected";
        gamma = 1.8f;
    } else if ( (ocioColorspace == "Gamma2.2") || (ocioColorspace == "vd8") || (ocioColorspace == "vd10") || (ocioColorspace == "vd16") || (ocioColorspace == "VD16") ) {
        // Gamma2.2 in nuke-default
        // vd8, vd10, vd16 in spi-anim and spi-vfx
        // VD16 in blender
        colorSpaceStr = "GammaCorrected";
        gamma = 2.2f;
    } else if ( (ocioColorspace == "sRGB") || (ocioColorspace == "sRGB D65") || (ocioColorspace == "sRGB (D60 sim.)") || (ocioColorspace == "out_srgbd60sim") || (ocioColorspace == "rrt_srgb") || (ocioColorspace == "srgb8") ) {
        // sRGB in nuke-default and blender
        // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
        // rrt_srgb in aces
        // srgb8 in spi-vfx
        colorSpaceStr = "sRGB";
    } else if ( (ocioColorspace == "Rec709") || (ocioColorspace == "nuke_rec709") || (ocioColorspace == "Rec 709 Curve") || (ocioColorspace == "Rec.709 - Full") || (ocioColorspace == "out_rec709full") || (ocioColorspace == "rrt_rec709") || (ocioColorspace == "hd10") ) {
        // Rec709 in nuke-default
        // nuke_rec709 in blender
        // out_rec709full or "Rec.709 - Full" in aces 1.0.0
        // rrt_rec709 in aces
        // hd10 in spi-anim and spi-vfx
        colorSpaceStr = "Rec709";
    } else if ( (ocioColorspace == "KodakLog") || (ocioColorspace == "Cineon") || (ocioColorspace == "Cineon Log Curve") || (ocioColorspace == "REDlogFilm") || (ocioColorspace == "lg10") ) {
        // Cineon in nuke-default
        // REDlogFilm in aces 1.0.0
        // lg10 in spi-vfx and blender
        colorSpaceStr = "KodakLog";
    } else if ( (ocioColorspace == OCIO::ROLE_SCENE_LINEAR) || (ocioColorspace == "Linear") || (ocioColorspace == "linear") || (ocioColorspace == "ACES2065-1") || (ocioColorspace == "aces") || (ocioColorspace == "lnf") || (ocioColorspace == "ln16") ) {
        // linear in nuke-default
        // ACES2065-1 in aces 1.0.0
        // aces in aces
        // lnf, ln16 in spi-anim and spi-vfx
        colorSpaceStr = "Linear";
    } else if ( (ocioColorspace == OCIO::ROLE_DATA) || (ocioColorspace == "raw") || (ocioColorspace == "Raw") || (ocioColorspace == "ncf") ) {
        // raw in nuke-default
        // raw in aces
        // Raw in blender
        // ncf in spi-anim and spi-vfx
        // leave empty
    } else {
        //unknown color-space, don't do anything
    }
    if ( !colorSpaceStr.empty() ) {
        spec.attribute("oiio:ColorSpace", colorSpaceStr);
    }
    if (gamma != 0.) {
        spec.attribute("oiio:Gamma", gamma);
    }
#endif // ifdef OFX_IO_USING_OCIO
    if ( !_quality->getIsSecret() ) {
        spec.attribute("CompressionQuality", quality);
    }
    if ( !_dwaCompressionLevel->getIsSecret() ) {
        spec.attribute("openexr:dwaCompressionLevel", (float)dwaCompressionLevel);
    }
    spec.attribute("Orientation", orientation + 1);
    if ( !compression.empty() ) { // some formats have a good value for the default compression
        spec.attribute("compression", compression);
    }
    if (pixelAspectRatio != 1.) {
        spec.attribute("PixelAspectRatio", pixelAspectRatio);
    }

    if ( data->output->supports("tiles") ) {
        spec.x = bounds.x1;
        spec.y = bounds.y1;
        spec.full_x = bounds.x1;
        spec.full_y = bounds.y1;

        bool clipToRoD = false;
        if ( _clipToRoD && !_clipToRoD->getIsSecret() ) {
            _clipToRoD->getValue(clipToRoD);
        }
        if (clipToRoD) {
            // The bounds were set to the input RoD.
            // Set the display window to format using user prefs
            OfxRectI format;
            double formatPar;
            getSelectedOutputFormat(&format, &formatPar);
            spec.full_x = format.x1;
            spec.full_y = format.y1;
            spec.full_width = format.x2 - format.x1;
            spec.full_height = format.y2 - format.y1;

            // Invert y
            spec.y = spec.full_y + spec.full_height - (spec.y + spec.height);
        }

        int tileSize_i;
        _tileSize->getValue(tileSize_i);
        EParamTileSize tileSizeE = (EParamTileSize)tileSize_i;
        switch (tileSizeE) {
        case eParamTileSize64:
            spec.tile_width = (std::min)(64, spec.full_width);
            spec.tile_height = (std::min)(64, spec.full_height);
            break;
        case eParamTileSize128:
            spec.tile_width = (std::min)(128, spec.full_width);
            spec.tile_height = (std::min)(128, spec.full_height);
            break;
        case eParamTileSize256:
            spec.tile_width = (std::min)(256, spec.full_width);
            spec.tile_height = (std::min)(256, spec.full_height);
            break;
        case eParamTileSize512:
            spec.tile_width = (std::min)(512, spec.full_width);
            spec.tile_height = (std::min)(512, spec.full_height);
            break;
        case eParamTileSizeScanLineBased:
        default:
            break;
        }
    }


    assert( !planes.empty() );
    switch (partsSplitting) {
    case eLayerViewsSinglePart: {
        data->specs.resize(1);

        ImageSpec partSpec = spec;
        TypeDesc tv( TypeDesc::STRING, viewsToRender.size() );

        vector<ustring> ustrvec ( viewsToRender.size() );
        {
            int i = 0;
            for (map<int, string>::const_iterator it = viewsToRender.begin(); it != viewsToRender.end(); ++it, ++i) {
                ustrvec[i] = it->second;
            }
        }

        if (viewsToRender.size() > 1) {
            partSpec.attribute("multiView", tv, &ustrvec[0]);
        }
        vector<string>  channels;

        for (map<int, string>::const_iterator view = viewsToRender.begin(); view != viewsToRender.end(); ++view) {
            for (std::list<string>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
                bool isColor = ( (*it == kFnOfxImagePlaneColour) ||
                                (*it == kOfxImageComponentRGB) ||
                                (*it == kOfxImageComponentAlpha) ||
                                (*it == kOfxImageComponentRGBA) );
                string rawComponents;
                if (*it == kFnOfxImagePlaneColour) {
                    rawComponents = _inputClip->getPixelComponentsProperty();
                } else {
                    rawComponents = *it;
                }

                MultiPlane::ImagePlaneDesc plane, pairedPlane;
                MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(rawComponents, &plane, &pairedPlane);

                std::vector<std::string> planeChannels = plane.getChannels();
                if ( plane.getNumComponents() > 0 && !isColor) {

                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = plane.getPlaneLabel() + "." + planeChannels[i];
                    }
                }
                if ( ( viewsToRender.size() > 1) && ( view != viewsToRender.begin() ) ) {
                    ///Prefix the view name for all views except the main
                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = view->second + "." + planeChannels[i];
                    }
                }

                if (!packingRequired) {
                    channels.insert( channels.end(), planeChannels.begin(), planeChannels.end() );
                } else {
                    assert( planeChannels.size() >= packingMapping.size() );
                    for (std::size_t i = 0; i < packingMapping.size(); ++i) {
                        channels.push_back(planeChannels[packingMapping[i]]);
                    }
                }
            }
        }     //  for (std::size_t v = 0; v < viewsToRender.size(); ++v) {
        if ( channels.size() == 4 && (channels[3] == "A" || channels[3] =="alpha") ) {
            partSpec.alpha_channel = 3;
        } else if ( channels.size() == 1 && (channels[0] == "A" || channels[0] =="alpha") ) {
            // Alpha component only
            partSpec.alpha_channel = 0;
        } else {
            // no alpha
            partSpec.alpha_channel = -1;
        }

        partSpec.nchannels = channels.size();
        partSpec.channelnames = channels;
        data->specs[0] = partSpec;
        break;
    }
    case eLayerViewsSplitViews: {
        data->specs.resize( viewsToRender.size() );

        int specIndex = 0;
        for (map<int, string>::const_iterator view = viewsToRender.begin(); view != viewsToRender.end(); ++view) {
            ImageSpec partSpec = spec;
            if (viewsToRender.size() > 1) {
                partSpec.attribute("view", view->second);
            }
            vector<string>  channels;

            for (std::list<string>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
                bool isColor = ( (*it == kFnOfxImagePlaneColour) ||
                                (*it == kOfxImageComponentRGB) ||
                                (*it == kOfxImageComponentAlpha) ||
                                (*it == kOfxImageComponentRGBA) );
                string rawComponents;
                if (*it == kFnOfxImagePlaneColour) {
                    rawComponents = _inputClip->getPixelComponentsProperty();
                } else {
                    rawComponents = *it;
                }

                MultiPlane::ImagePlaneDesc plane, pairedPlane;
                MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(rawComponents, &plane, &pairedPlane);

                std::vector<std::string> planeChannels = plane.getChannels();

                if ( plane.getNumComponents() > 0 && !isColor) {
                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = plane.getPlaneLabel() + "." + planeChannels[i];
                    }
                }

                if (!packingRequired) {
                    channels.insert( channels.end(), planeChannels.begin(), planeChannels.end() );
                } else {
                    assert( planeChannels.size() >= packingMapping.size() );
                    for (std::size_t i = 0; i < packingMapping.size(); ++i) {
                        channels.push_back(planeChannels[packingMapping[i]]);
                    }
                }
            }
            if ( channels.size() == 4 && (channels[3] == "A" || channels[3] =="alpha") ) {
                partSpec.alpha_channel = 3;
            } else if ( channels.size() == 1 && (channels[0] == "A" || channels[0] =="alpha") ) {
                // Alpha component only
                partSpec.alpha_channel = 0;
            } else {
                // no alpha
                partSpec.alpha_channel = -1;
            }

            partSpec.nchannels = channels.size();
            partSpec.channelnames = channels;

            data->specs[specIndex] = partSpec;
            ++specIndex;
        }     //  for (std::size_t v = 0; v < viewsToRender.size(); ++v) {
        break;
    }
    case eLayerViewsSplitViewsLayers: {
        data->specs.resize( viewsToRender.size() * planes.size() );

        int specIndex = 0;
        for (map<int, string>::const_iterator view = viewsToRender.begin(); view != viewsToRender.end(); ++view) {
            for (std::list<string>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
                bool isColor = ( (*it == kFnOfxImagePlaneColour) ||
                                (*it == kOfxImageComponentRGB) ||
                                (*it == kOfxImageComponentAlpha) ||
                                (*it == kOfxImageComponentRGBA) );
                string rawComponents;
                if (*it == kFnOfxImagePlaneColour) {
                    rawComponents = _inputClip->getPixelComponentsProperty();
                } else {
                    rawComponents = *it;
                }

                MultiPlane::ImagePlaneDesc plane, pairedPlane;
                MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(rawComponents, &plane, &pairedPlane);

                std::vector<std::string> planeChannels = plane.getChannels();


                ImageSpec partSpec = spec;
                if ( plane.getNumComponents() > 0 && !isColor) {
                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = plane.getPlaneLabel() + "." + planeChannels[i];
                    }
                }


                vector<string> channels;
                if (!packingRequired) {
                    channels.insert( channels.end(), planeChannels.begin(), planeChannels.end() );
                } else {
                    assert( planeChannels.size() >= packingMapping.size() );
                    for (std::size_t i = 0; i < packingMapping.size(); ++i) {
                        channels.push_back(planeChannels[packingMapping[i]]);
                    }
                }

                if ( channels.size() == 4 && (channels[3] == "A" || channels[3] =="alpha") ) {
                    partSpec.alpha_channel = 3;
                } else if ( channels.size() == 1 && (channels[0] == "A" || channels[0] =="alpha") ) {
                    // Alpha component only
                    partSpec.alpha_channel = 0;
                } else {
                    // no alpha
                    partSpec.alpha_channel = -1;
                }

                partSpec.nchannels = channels.size();
                partSpec.channelnames = channels;
                if (viewsToRender.size() > 1) {
                    partSpec.attribute("view", view->second);
                }
                data->specs[specIndex] = partSpec;

                ++specIndex;
            }
        }     //  for (std::size_t v = 0; v < viewsToRender.size(); ++v) {
        break;
    }
    } // switch


    if ( !data->output->open( filename, data->specs.size(), &data->specs.front() ) ) {
        setPersistentMessage( Message::eMessageError, "", data->output->geterror() );
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
} // WriteOIIOPlugin::beginEncodeParts

void
WriteOIIOPlugin::encodePart(void* user_data,
                            const string& filename,
                            const float *pixelData,
                            int pixelDataNComps,
                            int planeIndex,
                            int rowBytes)
{
    assert(user_data);
    WriteOIIOEncodePlanesData* data = (WriteOIIOEncodePlanesData*)user_data;
    if (planeIndex != 0) {
        if ( !data->output->open(filename, data->specs[planeIndex], ImageOutput::AppendSubimage) ) {
            setPersistentMessage( Message::eMessageError, "", data->output->geterror() );
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
    }

    TypeDesc format = TypeDesc::FLOAT;

    //do not use auto-stride as the buffer may have more components that what we want to write
    std::size_t xStride = format.size() * pixelDataNComps;
    data->output->write_image(format,
                              (char*)pixelData + (data->specs[planeIndex].height - 1) * rowBytes, //invert y
                              xStride, //xstride
                              -rowBytes, //ystride
                              AutoStride //zstride
                              );
}

void
WriteOIIOPlugin::endEncodeParts(void* user_data)
{
    assert(user_data);
    WriteOIIOEncodePlanesData* data = (WriteOIIOEncodePlanesData*)user_data;
    data->output->close();
}

bool
WriteOIIOPlugin::isImageFile(const string& /*fileExtension*/) const
{
    return true;
}

mDeclareWriterPluginFactory(WriteOIIOPluginFactory,; , false);
void
WriteOIIOPluginFactory::unload()
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
    //Kill all threads otherwise when the static global thread pool joins it threads there is a deadlock on Mingw
    IlmThread::ThreadPool::globalThreadPool().setNumThreads(0);

    // Workaround to a bug: https://github.com/OpenImageIO/oiio/issues/1795
    // see also https://github.com/LuxCoreRender/LuxCore/commit/607bfc9bff519ecc32c02ff3203b7ec71d201fde
    OIIO::attribute ("threads", 1);
#endif
}

void
WriteOIIOPluginFactory::load()
{
    _extensions.clear();
#if 0
    // hard-coded extensions list
    const char* extensionsl[] = {
        "bmp", "cin", /*"dds",*/ "dpx", /*"f3d",*/ "fits", "hdr",
#     if OIIO_VERSION >= 20100
        "heic", "heif",
#     endif
        "ico",
        "iff", "jpg", "jpe", "jpeg", "jif", "jfif", "jfi", "jp2", "j2k", "exr", "png",
        "pbm", "pgm", "ppm",
#     if OIIO_VERSION >= 10605
        "pfm", // PFM was flipped before 1.6.5
#     endif
        "psd", "pdd", "psb", /*"ptex",*/ "rla", "sgi", "rgb", "rgba", "bw", "int", "inta", "pic", "tga", "tpic", "tif", "tiff", "tx", "env", "sm", "vsm", "zfile", NULL
    };
    for (const char** ext = extensionsl; *ext != NULL; ++ext) {
        _extensions.push_back(*ext);
    }
#else
    // get extensions from OIIO (but there is no distinctions between readers and writers)
    string extensions_list;
    getattribute("extension_list", extensions_list);
    stringstream formatss(extensions_list);
    string format;
    std::list<string> extensionsl;
    while ( std::getline(formatss, format, ';') ) {
        stringstream extensionss(format);
        string extension;
        std::getline(extensionss, extension, ':'); // extract the format
        while ( std::getline(extensionss, extension, ',') ) {
            extensionsl.push_back(extension);
        }
    }
    const char* extensions_blacklist[] = {
#     if OIIO_VERSION < 10605
        "pfm", // PFM was flipped before 1.6.5
#     endif
        "avi", "mov", "qt", "mp4", "m4a", "3gp", "3g2", "mj2", "m4v", "mpg", // FFmpeg extensions - better supported by WriteFFmpeg
        "gif", // animated GIFs are only supported by FFmpeg
        NULL
    };
    for (const char*const* e = extensions_blacklist; *e != NULL; ++e) {
        extensionsl.remove(*e);
    }
    _extensions.assign( extensionsl.begin(), extensionsl.end() );
#endif
}

/** @brief The basic describe function, passed a plugin descriptor */
void
WriteOIIOPluginFactory::describe(ImageEffectDescriptor &desc)
{
    GenericWriterDescribe(desc, eRenderFullySafe, _extensions, kPluginEvaluation, true, true);

    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginDescription( "Write images using OpenImageIO.\n\n"
                               "OpenImageIO supports writing the following file formats:\n"
                               "BMP (*.bmp)\n"
                               "Cineon (*.cin)\n"
                               //"Direct Draw Surface (*.dds)\n"
                               "DPX (*.dpx)\n"
                               //"Field3D (*.f3d)\n"
                               "FITS (*.fits)\n"
                               "HDR/RGBE (*.hdr)\n"
#                           if OIIO_VERSION >= 20100
                               "HEIC/HEIF (*.heic *.heif)\n"
#                           endif
                               "Icon (*.ico)\n"
                               "IFF (*.iff)\n"
                               "JPEG (*.jpg *.jpe *.jpeg *.jif *.jfif *.jfi)\n"
                               "JPEG-2000 (*.jp2 *.j2k)\n"
                               "OpenEXR (*.exr)\n"
                               "Portable Network Graphics (*.png)\n"
                               "PNM / Netpbm (*.pbm *.pgm *.ppm)\n"
                               "PSD (*.psd *.pdd *.psb)\n"
                               //"Ptex (*.ptex)\n"
                               "RLA (*.rla)\n"
                               "SGI (*.sgi *.rgb *.rgba *.bw *.int *.inta)\n"
                               "Softimage PIC (*.pic)\n"
                               "Targa (*.tga *.tpic)\n"
                               "TIFF (*.tif *.tiff *.tx *.env *.sm *.vsm)\n"
                               "Zfile (*.zfile)" );

# if defined(OFX_EXTENSIONS_NATRON) && defined(OFX_EXTENSIONS_NUKE)
    gIsMultiplanarV2 = ( getImageEffectHostDescription()->supportsDynamicChoices &&
                                          getImageEffectHostDescription()->isMultiPlanar &&
                                          fetchSuite(kFnOfxImageEffectPlaneSuite, 2, true) );
# else
    gIsMultiplanarV2 = false;
# endif
} // WriteOIIOPluginFactory::describe

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
WriteOIIOPluginFactory::describeInContext(ImageEffectDescriptor &desc,
                                          ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor *page = GenericWriterDescribeInContextBegin(desc, context,
                                                                    kSupportsRGBA,
                                                                    kSupportsRGB,
                                                                    kSupportsXY,
                                                                    kSupportsAlpha,
                                                                    "scene_linear", "scene_linear", true);
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamTileSize);
        param->setLabel(kParamTileSizeLabel);
        param->setHint(kParamTileSizeHint);
        assert(param->getNOptions() == eParamTileSizeScanLineBased);
        param->appendOption(kParamTileSizeOptionScanLineBased);
        assert(param->getNOptions() == eParamTileSize64);
        param->appendOption(kParamTileSizeOption64);
        assert(param->getNOptions() == eParamTileSize128);
        param->appendOption(kParamTileSizeOption128);
        assert(param->getNOptions() == eParamTileSize256);
        param->appendOption(kParamTileSizeOption256);
        assert(param->getNOptions() == eParamTileSize512);
        param->appendOption(kParamTileSizeOption512);
        param->setDefault(eParamTileSizeScanLineBased);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamBitDepth);
        param->setLabel(kParamBitDepthLabel);
        param->setHint(kParamBitDepthHint);
        assert(param->getNOptions() == eTuttlePluginBitDepthAuto);
        param->appendOption(kParamBitDepthOptionAuto);
        assert(param->getNOptions() == eTuttlePluginBitDepth8);
        param->appendOption(kParamBitDepthOption8);
        assert(param->getNOptions() == eTuttlePluginBitDepth10);
        param->appendOption(kParamBitDepthOption10);
        assert(param->getNOptions() == eTuttlePluginBitDepth12);
        param->appendOption(kParamBitDepthOption12);
        assert(param->getNOptions() == eTuttlePluginBitDepth16);
        param->appendOption(kParamBitDepthOption16);
        assert(param->getNOptions() == eTuttlePluginBitDepth16f);
        param->appendOption(kParamBitDepthOption16f);
        assert(param->getNOptions() == eTuttlePluginBitDepth32);
        param->appendOption(kParamBitDepthOption32);
        assert(param->getNOptions() == eTuttlePluginBitDepth32f);
        param->appendOption(kParamBitDepthOption32f);
        assert(param->getNOptions() == eTuttlePluginBitDepth64);
        param->appendOption(kParamBitDepthOption64);
        assert(param->getNOptions() == eTuttlePluginBitDepth64f);
        param->appendOption(kParamBitDepthOption64f);
        param->setDefault(eTuttlePluginBitDepthAuto);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamOutputQuality);
        param->setLabel(kParamOutputQualityLabel);
        param->setHint(kParamOutputQualityHint);
        param->setRange(0, 100);
        param->setDisplayRange(0, 100);
        param->setDefault(kParamOutputQualityDefault);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kParamOutputDWACompressionLevel);
        param->setLabel(kParamOutputDWACompressionLevelLabel);
        param->setHint(kParamOutputDWACompressionLevelHint);
        param->setRange(0, DBL_MAX);
        param->setDisplayRange(45, 200);
        param->setDefault(kParamOutputDWACompressionLevelDefault);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputOrientation);
        param->setLabel(kParamOutputOrientationLabel);
        param->setHint(kParamOutputOrientationHint);
        assert(param->getNOptions() == eOutputOrientationNormal);
        param->appendOption(kParamOutputOrientationNormal);
        assert(param->getNOptions() == eOutputOrientationFlop);
        param->appendOption(kParamOutputOrientationFlop);
        assert(param->getNOptions() == eOutputOrientationR180);
        param->appendOption(kParamOutputOrientationR180);
        assert(param->getNOptions() == eOutputOrientationFlip);
        param->appendOption(kParamOutputOrientationFlip);
        assert(param->getNOptions() == eOutputOrientationTransposed);
        param->appendOption(kParamOutputOrientationTransposed);
        assert(param->getNOptions() == eOutputOrientationR90Clockwise);
        param->appendOption(kParamOutputOrientationR90Clockwise);
        assert(param->getNOptions() == eOutputOrientationTransverse);
        param->appendOption(kParamOutputOrientationTransverse);
        assert(param->getNOptions() == eOutputOrientationR90CounterClockwise);
        param->appendOption(kParamOutputOrientationR90CounterClockwise);
        param->setDefault(0);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputCompression);
        param->setLabel(kParamOutputCompressionLabel);
        param->setHint(kParamOutputCompressionHint);
        assert(param->getNOptions() == eParamCompressionAuto);
        param->appendOption(kParamOutputCompressionOptionAuto);
        assert(param->getNOptions() == eParamCompressionNone);
        param->appendOption(kParamOutputCompressionOptionNone);
        assert(param->getNOptions() == eParamCompressionZip);
        param->appendOption(kParamOutputCompressionOptionZip);
        assert(param->getNOptions() == eParamCompressionZips);
        param->appendOption(kParamOutputCompressionOptionZips);
        assert(param->getNOptions() == eParamCompressionRle);
        param->appendOption(kParamOutputCompressionOptionRle);
        assert(param->getNOptions() == eParamCompressionPiz);
        param->appendOption(kParamOutputCompressionOptionPiz);
        assert(param->getNOptions() == eParamCompressionPxr24);
        param->appendOption(kParamOutputCompressionOptionPxr24);
        assert(param->getNOptions() == eParamCompressionB44);
        param->appendOption(kParamOutputCompressionOptionB44);
        assert(param->getNOptions() == eParamCompressionB44a);
        param->appendOption(kParamOutputCompressionOptionB44a);
        assert(param->getNOptions() == eParamCompressionDWAa);
        param->appendOption(kParamOutputCompressionOptionDWAa);
        assert(param->getNOptions() == eParamCompressionDWAb);
        param->appendOption(kParamOutputCompressionOptionDWAb);
        assert(param->getNOptions() == eParamCompressionLZW);
        param->appendOption(kParamOutputCompressionOptionLZW);
        assert(param->getNOptions() == eParamCompressionCCITTRLE);
        param->appendOption(kParamOutputCompressionOptionCCITTRLE);
        assert(param->getNOptions() == eParamCompressionJPEG);
        param->appendOption(kParamOutputCompressionOptionJPEG);
        assert(param->getNOptions() == eParamCompressionPACKBITS);
        param->appendOption(kParamOutputCompressionOptionPACKBITS);
        param->setDefault(eParamCompressionAuto);
        if (page) {
            page->addChild(*param);
        }
    }


    if (gIsMultiplanarV2) {


        MultiPlane::Factory::describeInContextAddPlaneChoice(desc, page, kParamOutputChannels, kParamOutputChannelsLabel, kParamOutputChannelsHint);
        MultiPlane::Factory::describeInContextAddAllPlanesOutputCheckbox(desc, page);
        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamPartsSplitting);
            param->setLabel(kParamPartsSplittingLabel);
            param->setHint(kParamPartsSplittingHint);
            param->appendOption(kParamPartsSinglePart);
            param->appendOption(kParamPartsSplitViews);
            param->appendOption(kParamPartsSplitViewsLayers);
            param->setDefault(2);
            param->setAnimates(false);
            if (page) {
                page->addChild(*param);
            }
        }
        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamViewsSelector);
            param->setLabel(kParamViewsSelectorLabel);
            param->setHint(kParamViewsSelectorHint);
            param->appendOption("All");
            param->setAnimates(false);
            param->setDefault(0);
            if (page) {
                page->addChild(*param);
            }
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamLibraryInfo);
        param->setLabelAndHint(kParamLibraryInfoLabel);
        if (page) {
            page->addChild(*param);
        }
    }
    GenericWriterDescribeInContextEnd(desc, context, page);
} // WriteOIIOPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
WriteOIIOPluginFactory::createInstance(OfxImageEffectHandle handle,
                                       ContextEnum /*context*/)
{
    WriteOIIOPlugin* ret = new WriteOIIOPlugin(handle, _extensions);

    ret->restoreStateFromParams();

    return ret;
}

static WriteOIIOPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

OFXS_NAMESPACE_ANONYMOUS_EXIT
