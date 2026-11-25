/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of openfx-io <https://github.com/NatronGitHub/openfx-io>,
 * (C) 2018-2026 The Natron Developers
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

#include <atomic>
#include <cerrno>
#include <cfloat> // DBL_MAX
#include <cstdio>
#include <list>
#include <memory>
#include <set>
#include <sstream>
#include <system_error> // std::generic_category()

#include "ofxsMacros.h"

#include "OIIOGlobal.h"
// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagebuf.h>
#include <OpenImageIO/imagebufalgo.h>
GCC_DIAG_ON(unused-parameter)
// clang-format on

#include "GenericOCIO.h"
#include "GenericWriter.h"

#include <ofxsCoords.h>
#include <ofxsMultiPlane.h>

using namespace OFX;
using namespace OFX::IO;
#ifdef OFX_IO_USING_OCIO
namespace OCIO = OCIO_NAMESPACE;
#endif
OIIO_NAMESPACE_USING

using std::map;
using std::string;
using std::stringstream;
using std::vector;

OFXS_NAMESPACE_ANONYMOUS_ENTER

// The ImageOutput that OpenImageIO would use to write filename, to query what its format supports, or null if it
// cannot write it. A failed ImageOutput::create() leaves an error, which OpenImageIO prints at exit if it is never
// retrieved (#228): it is retrieved here. An empty name (no file yet) is not passed to create(), which would fail.
std::unique_ptr<ImageOutput>
createFormatOutput(const string& filename)
{
    if (filename.empty()) {
        return nullptr;
    }
    std::unique_ptr<ImageOutput> output(ImageOutput::create(filename));
    if (!output) {
        (void)OIIO::geterror();
    }
    return output;
}

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

#define kParamBitDepth "bitDepth"
#define kParamBitDepthLabel "Bit Depth"
#define kParamBitDepthHint \
    "Number of bits per sample in the file [TIFF,DPX,TGA,DDS,ICO,IFF,PNM,PIC]."

#define kParamBitDepthOptionAuto "auto", "Guess from the output format", "auto"
//#define kParamBitDepthNone     "none"
#define kParamBitDepthOption8 "8i", "8  bits integer", "8i"
#define kParamBitDepthOption10 "10i", "10 bits integer", "10i"
#define kParamBitDepthOption12 "12i", "12 bits integer", "12i"
#define kParamBitDepthOption16 "16i", "16 bits integer", "16i"
#define kParamBitDepthOption16f "16f", "16 bits floating point", "16f"
#define kParamBitDepthOption32 "32i", "32 bits integer", "32i"
#define kParamBitDepthOption32f "32f", "32 bits floating point", "32f"
#define kParamBitDepthOption64 "64i", "64 bits integer", "64i"
#define kParamBitDepthOption64f "64f", "64 bits floating point", "64f"

enum ETuttlePluginBitDepth {
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

enum ETuttlePluginComponents {
    eTuttlePluginComponentsAuto = 0,
    eTuttlePluginComponentsGray,
    eTuttlePluginComponentsRGB,
    eTuttlePluginComponentsRGBA
};

#define kParamOutputQuality "quality"
#define kParamOutputQualityLabel "Quality"
#define kParamOutputQualityHint \
    "Indicates the quality of compression to use (0-100), for those plugins and compression methods that allow a variable amount of compression, with higher numbers indicating higher image fidelity. [JPEG, TIFF w/ JPEG comp., WEBP]"
#define kParamOutputQualityDefault 100

#define kParamOutputDWACompressionLevel "dwaCompressionLevel"
#define kParamOutputDWACompressionLevelLabel "DWA Compression Level"
#define kParamOutputDWACompressionLevelHint \
    "Amount of compression when using Dreamworks DWAA or DWAB compression options. These lossy formats are variable in quality and can minimize the compression artifacts. Higher values will result in greater compression and likewise smaller file size, but increases the chance for artifacts. Values from 45 to 150 are usually correct for production shots, whereas HDR vacation photos could use up to 500. Values below 45 should give no visible imprrovement on photographs. [EXR w/ DWAa or DWAb comp.]"
#define kParamOutputDWACompressionLevelDefault 45

#define kParamOutputZIPCompressionLevel "zipCompressionLevel"
#define kParamOutputZIPCompressionLevelLabel "ZIP Compression Level"
#define kParamOutputZIPCompressionLevelHint \
    "Compression level for zip/deflate compression, on a scale from 1 (fastest, minimal compression) to 9 (slowest, maximal compression) [EXR, TIFF or Zfile w/ zip or zips comp.]"
#define kParamOutputZIPCompressionLevelDefault 4

#define kParamOutputZSTDCompressionLevel "zstdCompressionLevel"
#define kParamOutputZSTDCompressionLevelLabel "ZSTD Compression Level"
#define kParamOutputZSTDCompressionLevelHint \
    "Compression level for Zstandard compression, on a scale from 1 (fastest, minimal compression) to 22 (slowest, maximal compression). Needs OpenImageIO >= 3.2.1. [EXR w/ ZSTD comp.]"
#define kParamOutputZSTDCompressionLevelDefault 5

#define kParamOutputLineOrder "lineOrder"
#define kParamOutputLineOrderLabel "Line Order"
#define kParamOutputLineOrderHint "Order in which the scanlines (or tiles) are stored in the file. Only matters for downstream tools that depend on the storage order; the image itself is the same. [EXR]"

#define kParamOutputLineOrderOptionIncreasingY "increasingY", "First scanline stored in the file is the top of the image (lowest y in the EXR data window). This is the default.", "increasingY"
#define kParamOutputLineOrderOptionDecreasingY "decreasingY", "First scanline stored in the file is the bottom of the image (highest y in the EXR data window). Requires OpenImageIO 2.6.2 or later.", "decreasingY"
// randomY is not offered: WriteOIIO always writes tiles in order, so it would not be meaningful.

enum EParamLineOrder {
    eParamLineOrderIncreasingY = 0,
    eParamLineOrderDecreasingY
};

#define kParamOutputOrientation "orientation"
#define kParamOutputOrientationLabel "Orientation"
#define kParamOutputOrientationHint                                                     \
    "The orientation of the image data [DPX,TIFF,JPEG,HDR,FITS].\n"                     \
    "By default, image pixels are ordered from the top of the display to the bottom, "  \
    "and within each scanline, from left to right (i.e., the same ordering as English " \
    "text and scan progression on a CRT). But the \"Orientation\" parameter can "       \
    "suggest that it should be displayed with a different orientation, according to "   \
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
#define kParamOutputOrientationNormal "normal", "normal (top to bottom, left to right)", "normal"
#define kParamOutputOrientationFlop "flop", "flipped horizontally (top to bottom, right to left)", "flop"
#define kParamOutputOrientationR180 "180", "rotate 180deg (bottom to top, right to left)", "180"
#define kParamOutputOrientationFlip "flip", "flipped vertically (bottom to top, left to right)", "flip"
#define kParamOutputOrientationTransposed "transposed", "transposed (left to right, top to bottom)", "transposed"
#define kParamOutputOrientationR90Clockwise "90clockwise", "rotated 90deg clockwise (right to left, top to bottom)", "90clockwise"
#define kParamOutputOrientationTransverse "transverse", "transverse (right to left, bottom to top)", "transverse"
#define kParamOutputOrientationR90CounterClockwise "90counter-clockwise", "rotated 90deg counter-clockwise (left to right, bottom to top)", "90counter-clockwise"
enum EOutputOrientation {
    eOutputOrientationNormal = 0,
    eOutputOrientationFlop,
    eOutputOrientationR180,
    eOutputOrientationFlip,
    eOutputOrientationTransposed,
    eOutputOrientationR90Clockwise,
    eOutputOrientationTransverse,
    eOutputOrientationR90CounterClockwise,
};

#define kParamOutputCompression "compression"
#define kParamOutputCompressionLabel "Compression"
#define kParamOutputCompressionHint                                                                                            \
    "Compression type [TIFF,EXR,DDS,IFF,SGI,TGA]\n"                                                                            \
    "Indicates the type of compression the file uses. Supported compression modes will vary from format to format. "           \
    "As an example, the TIFF format supports \"none\", \"lzw\", \"ccittrle\", \"zip\" (the default), \"jpeg\", \"packbits\", " \
    "and the EXR format supports \"none\", \"rle\", \"zip\" (the default), \"zips\", \"piz\", \"pxr24\", \"b44\", "            \
    "\"b44a\", \"dwaa\", \"dwab\", \"htj2k256\", \"htj2k32\" (OpenImageIO built with OpenEXR 3.4 or later) or "                \
    "\"zstd\" (OpenImageIO 3.2.1 or later, built with OpenEXR 3.5 or later). An EXR compression that OpenImageIO does not "    \
    "support falls back to \"zip\". The DWA, ZIP and ZSTD Compression Level parameters set the level of the dwaa/dwab, "       \
    "zip/zips and zstd compressions."

#define kParamOutputCompressionOptionAuto "default", "Guess from the output format", "default"
#define kParamOutputCompressionOptionNone "none", "No compression [EXR, TIFF, IFF]", "none"
#define kParamOutputCompressionOptionZip "zip", "Zlib/Deflate compression (lossless) [EXR, TIFF, Zfile]", "zip"
#define kParamOutputCompressionOptionZips "zips", "Zlib compression (lossless), one scan line at a time [EXR]", "zips"
#define kParamOutputCompressionOptionRle "rle", "Run Length Encoding (lossless) [DPX, IFF, EXR, TGA, RLA]", "rle"
#define kParamOutputCompressionOptionPiz "piz", "Piz-based wavelet compression [EXR]", "piz"
#define kParamOutputCompressionOptionPxr24 "pxr24", "Lossy 24bit float compression [EXR]", "pxr24"
#define kParamOutputCompressionOptionB44 "b44", "Lossy 4-by-4 pixel block compression, fixed compression rate [EXR]", "b44"
#define kParamOutputCompressionOptionB44a "b44a", "Lossy 4-by-4 pixel block compression, flat fields are compressed more [EXR]", "b44a"
#define kParamOutputCompressionOptionDWAa "dwaa", "lossy DCT based compression, in blocks of 32 scanlines. More efficient for partial buffer access. [EXR]", "dwaa"
#define kParamOutputCompressionOptionDWAb "dwab", "lossy DCT based compression, in blocks of 256 scanlines. More efficient space wise and faster to decode full frames than DWAA. [EXR]", "dwab"
#define kParamOutputCompressionOptionHTJ2K256 "htj2k256", "JPEG 2000 lossless coding, in blocks of 256 scanlines and using the High-Throughput (HT) blocker. Offers both speed and high-coding efficiency. Requires OpenImageIO built with OpenEXR >= 3.4, falls back to zip otherwise. [EXR]", "htj2k256"
#define kParamOutputCompressionOptionHTJ2K32 "htj2k32", "JPEG 2000 lossless coding, in blocks of 32 scanlines and using the High-Throughput (HT) blocker. Offers both speed and high-coding efficiency. Requires OpenImageIO built with OpenEXR >= 3.4, falls back to zip otherwise. [EXR]", "htj2k32"
#define kParamOutputCompressionOptionZSTD "zstd", "Zstandard compression (lossless). Requires OpenImageIO >= 3.2.1 built with OpenEXR >= 3.5, falls back to zip otherwise. [EXR]", "zstd"
#define kParamOutputCompressionOptionLZW "lzw", "Lempel-Ziv Welsch compression (lossless) [TIFF]", "lzw"
#define kParamOutputCompressionOptionCCITTRLE "ccittrle", "CCITT modified Huffman RLE (lossless) [TIFF]", "ccittrle"
#define kParamOutputCompressionOptionJPEG "jpeg", "JPEG [TIFF]", "jpeg"
#define kParamOutputCompressionOptionPACKBITS "packbits", "Macintosh RLE (lossless) [TIFF]", "packbits"

enum EParamCompression {
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
    eParamCompressionPACKBITS,
    // new options must be appended at the end, to keep the index of existing ones
    eParamCompressionHTJ2K256,
    eParamCompressionHTJ2K32,
    eParamCompressionZSTD,
};

#define kParamTileSize "tileSize"
#define kParamTileSizeLabel "Tile Size"
#define kParamTileSizeHint "Size of a tile in the output file for formats that support tiles. If scan-line based, the whole image will have a single tile."
#define kParamTileSizeOptionScanLineBased "Scan-Line Based", "", "0"
#define kParamTileSizeOption64 "64", "", "64"
#define kParamTileSizeOption128 "128", "", "128"
#define kParamTileSizeOption256 "256", "", "256"
#define kParamTileSizeOption512 "512", "", "512"

enum EParamTileSize {
    eParamTileSizeScanLineBased = 0,
    eParamTileSize64,
    eParamTileSize128,
    eParamTileSize256,
    eParamTileSize512
};

#define kParamOutputTextureformat "textureFormat"
#define kParamOutputTextureformatLabel "Texture Format"
#define kParamOutputTextureformatHint                                                                      \
    "Write the image as a tiled, MIP-mapped texture, as made by OpenImageIO's maketx. "                     \
    "Only available for EXR, with a single layer and view. Uses the Tile Size param (64x64 if scan-line based)." \
    " [EXR]"
#define kParamOutputTextureformatOptionNone "None", "Write a regular image.", "none"
#define kParamOutputTextureformatOptionPlain "Plain Texture", "Ordinary MIP-mapped 2D texture (maketx default).", "texture"
#define kParamOutputTextureformatOptionLatLong "LatLong Environment", "MIP-mapped latitude-longitude environment map (maketx --envlatl).", "envlatl"
#define kParamOutputTextureformatOptionShadow "Shadow", "Shadow map (maketx --shadow).", "shadow"

enum EParamTextureformat {
    eParamTextureformatNone = 0,
    eParamTextureformatPlain,
    eParamTextureformatLatLong,
    eParamTextureformatShadow
};


#define kParamOutputChannels kNatronOfxParamOutputChannels
#define kParamOutputChannelsChoice kParamOutputChannels "Choice"
#define kParamOutputChannelsLabel "Layer(s)"
#define kParamOutputChannelsHint "Select the layer to write to the file. To write all layers, check \"All Planes\" instead. " \
                                 "It is not yet possible to append a layer to an existing file."

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
#define kParamPartsSplitViewsOptionHint "Each view will have its own part, and each part will contain all layers. This will produce an EXR optimized in size that " \
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

static bool gIsMultiplanarV2 = false;

struct WriteOIIOEncodePlanesData;

class WriteOIIOPlugin
    : public GenericWriterPlugin {
    // counts this instance: the first one starts OpenImageIO's thread pool, the last one stops it (OIIOGlobal.h).
    // (A member of this class, constructed before and destroyed after the bodies of its constructor and
    // destructor: the base class doesn't use the pools.)
    OIIOThreadsUser _oiioThreads;

public:
    WriteOIIOPlugin(OfxImageEffectHandle handle, const vector<string>& extensions);

    virtual ~WriteOIIOPlugin();

    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;
    virtual void getClipPreferences(ClipPreferencesSetter& clipPreferences) OVERRIDE FINAL;
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
                        const float* pixelData,
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
        endEncodeParts(data.getData());
    }

    virtual void encodePart(void* user_data, const string& filename, const float* pixelData, int pixelDataNComps, int planeIndex, int rowBytes) OVERRIDE FINAL;
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
    virtual void beginEncode(const string& filename, const OfxRectI& rodPixel, float pixelAspectRatio, const BeginSequenceRenderArguments& args) OVERRIDE FINAL;
    virtual void endEncode(const EndSequenceRenderArguments& args) OVERRIDE FINAL;
    virtual bool isImageFile(const string& fileExtension) const OVERRIDE FINAL;
    virtual PreMultiplicationEnum getExpectedInputPremultiplication() const OVERRIDE FINAL { return eImagePreMultiplied; }

    virtual bool displayWindowSupportedByFormat(const string& filename) const OVERRIDE FINAL;

    void refreshParamsVisibility(const string& filename);

    void refreshCheckboxesLabels();

private:
    // Sets the error of the last failed call on data.output (and the write error of data.file, if any) as the
    // persistent message, and fails the render.
    [[noreturn]] void reportOutputError(const WriteOIIOEncodePlanesData& data);
    // Sets "Cannot write filename: error" as the persistent message, and fails the render.
    [[noreturn]] void reportWriteError(const string& filename, const string& error);

    ChoiceParam* _bitDepth;
    IntParam* _quality;
    DoubleParam* _dwaCompressionLevel;
    IntParam* _zipCompressionLevel;
    IntParam* _zstdCompressionLevel;
    ChoiceParam* _lineOrder;
    ChoiceParam* _orientation;
    ChoiceParam* _compression;
    ChoiceParam* _textureformat;
    ChoiceParam* _tileSize;
    ChoiceParam* _outputLayers;
    ChoiceParam* _parts;
    ChoiceParam* _views;
    std::list<string> _currentInputComponents;
    std::list<string> _availableViews;
    // a file of this sequence render was written as zip instead of zstd (#171): set by endEncodeParts() (on render
    // threads), reset by beginEncode() and read by endEncode() (beginSequenceRender and endSequenceRender, while no
    // frame of the sequence renders)
    std::atomic<bool> _zstdWrittenAsZip { false };
};

WriteOIIOPlugin::WriteOIIOPlugin(OfxImageEffectHandle handle,
                                 const vector<string>& extensions)
    : GenericWriterPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha)
    , _bitDepth(NULL)
    , _quality(NULL)
    , _dwaCompressionLevel(NULL)
    , _zipCompressionLevel(NULL)
    , _zstdCompressionLevel(NULL)
    , _lineOrder(NULL)
    , _orientation(NULL)
    , _compression(NULL)
    , _textureformat(NULL)
    , _tileSize(NULL)
    , _outputLayers(NULL)
    , _parts(NULL)
    , _views(NULL)
    , _currentInputComponents()
    , _availableViews()
{

    _bitDepth = fetchChoiceParam(kParamBitDepth);
    _quality = fetchIntParam(kParamOutputQuality);
    _dwaCompressionLevel = fetchDoubleParam(kParamOutputDWACompressionLevel);
    _zipCompressionLevel = fetchIntParam(kParamOutputZIPCompressionLevel);
    _zstdCompressionLevel = fetchIntParam(kParamOutputZSTDCompressionLevel);
    _orientation = fetchChoiceParam(kParamOutputOrientation);
    _compression = fetchChoiceParam(kParamOutputCompression);
    _lineOrder = fetchChoiceParam(kParamOutputLineOrder);
    _textureformat = fetchChoiceParam(kParamOutputTextureformat);
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
}

WriteOIIOPlugin::~WriteOIIOPlugin()
{
}

namespace {
static bool
hasListChanged(const std::list<string>& oldList,
               const std::list<string>& newList)
{
    if (oldList.size() != newList.size()) {
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

// "major.minor.patch" of an OpenImageIO version number (openimageio_version(): 10000 * major + 100 * minor + patch)
static string
oiioVersionString(int ver)
{
    std::ostringstream oss;
    oss << ver / 10000 << '.' << (ver % 10000) / 100 << '.' << (ver % 100);

    return oss.str();
}

// The version of the OpenEXR library of OpenImageIO, e.g. "3.5.4" from "openexr:OpenEXR 3.5.4" in its "library_list"
// attribute (OIIO v3.2.1.0 exroutput.cpp:295-298, openexr_imageio_library_version()); empty if it is not there.
static string
oiioOpenEXRVersion()
{
    const string libs = OIIO::get_string_attribute("library_list");
    const string key = "openexr:OpenEXR ";
    const size_t pos = libs.find(key);
    if (pos == string::npos) {
        return string();
    }
    const size_t begin = pos + key.size();

    return libs.substr(begin, libs.find(';', begin) - begin);
}

// #171: true if OpenImageIO writes EXR files with the zstd compression. For a compression that it or its OpenEXR does
// not support, OpenImageIO's EXR writer keeps the default zip compression without an error (OpenEXROutput::
// put_parameter(): `header.compression() = Imf::ZIP_COMPRESSION;  // Default`, exroutput.cpp:1155 in OIIO v3.1.7.0,
// :1317 in v3.2.1.0), and no OpenImageIO API lists the compressions of a format, so this is a version check, specific
// to zstd: it needs OpenImageIO 3.2.1 at runtime (v3.2.1.0 exroutput.cpp:1352, `#ifdef IMF_ZSTD_COMPRESSION`), built
// with OpenEXR 3.5 or later (IMF_ZSTD_COMPRESSION is defined in OpenEXR v3.5.0 ImfCRgbaFile.h:91, not in v3.4.0).
static bool
oiioWritesEXRZstd()
{
    static const bool writesZstd = []() {
        constexpr int kOIIOVersionZstd = 30201; // OpenImageIO 3.2.1
        if (openimageio_version() < kOIIOVersionZstd) {
            return false;
        }
        int major = 0;
        int minor = 0;
        if (std::sscanf(oiioOpenEXRVersion().c_str(), "%d.%d", &major, &minor) != 2) {
            return false;
        }

        return (major > 3) || ((major == 3) && (minor >= 5));
    }();

    return writesZstd;
}

static string
oiio_versions()
{
    std::ostringstream oss;
    oss << "OpenImageIO version (compiled with / running with): " << OIIO_VERSION_STRING << '/';
    oss << oiioVersionString(openimageio_version()) << std::endl;
    string libs = OIIO::get_string_attribute("library_list");
    if (libs.size()) {
        oss << std::endl
            << "Dependent libraries:" << std::endl;
        std::istringstream f(libs);
        string s;
        while (getline(f, s, ';')) {
            size_t pos = s.find(':');
            oss << s.substr(pos + 1) << std::endl;
        }
    }

    return oss.str();
}

void
WriteOIIOPlugin::changedParam(const InstanceChangedArgs& args,
                              const string& paramName)
{
    if (paramName == kParamLibraryInfo) {
        string extensions_list;
        getattribute("extension_list", extensions_list);

        string extensions_pretty;
        {
            stringstream formatss(extensions_list);
            string format;
            vector<string> extensions;
            while (std::getline(formatss, format, ';')) {
                stringstream extensionss(format);
                string extension;
                std::getline(extensionss, extension, ':'); // extract the format
                extensions_pretty += extension;
                extensions_pretty += ": ";
                bool first = true;
                while (std::getline(extensionss, extension, ',')) {
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
    } else if ((paramName == kParamOutputCompression) && (args.reason == eChangeUserEdit)) {
        string filename;
        _fileParam->getValue(filename);
        refreshParamsVisibility(filename);
    } else if (paramName == kParamOutputChannels) {
    }

    GenericWriterPlugin::changedParam(args, paramName);
}

void
WriteOIIOPlugin::getClipPreferences(ClipPreferencesSetter& clipPreferences)
{
    GenericWriterPlugin::getClipPreferences(clipPreferences);

    if (gIsMultiplanarV2) {
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
        // Now build the views choice
        std::list<string> views;
        int nViews = getViewCount();
        for (int i = 0; i < nViews; ++i) {
            string view = getViewName(i);
            views.push_back(view);
        }
        if (hasListChanged(_availableViews, views)) {
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
    if (!_views || _views->getIsSecret()) {
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
    if (!_parts || _parts->getIsSecret()) {
        return eLayerViewsSinglePart;
    }
    int index = _parts->getValue();
    string optionEnum;
    _parts->getEnum(index, optionEnum);
    if (optionEnum.empty()) {
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
static ETuttlePluginBitDepth
getDefaultBitDepth(const string& filepath,
                   ETuttlePluginBitDepth bitDepth)
{
    if (bitDepth != eTuttlePluginBitDepthAuto) {
        return bitDepth;
    }
    string format = Filesystem::extension(filepath);
    Strutil::to_lower(format);
    if ((format == ".exr")) {
        return eTuttlePluginBitDepth16f; // 16f is the most commonly used bit depth in the EXR world
    } else if ((format == ".hdr") || (format == ".rgbe") || (format == ".pfm")) {
        return eTuttlePluginBitDepth32f;
    } else if ((format == ".jpg") || (format == ".jpe") || (format == ".jpeg") || (format == ".jif") || (format == ".jfif") || (format == ".jfi") || (format == ".bmp") || (format == ".dds") || (format == ".ico") || (format == ".pgm") || (format == ".pnm") || (format == ".ppm") || (format == ".pbm") || (format == ".pic") || (format == ".tga") || (format == ".tpic") || (format == ".png")) {
        return eTuttlePluginBitDepth8;
    } else {
        // cin, dpx, fits, heic, heif, j2k, j2c, jp2, jpe, sgi, tif, tiff, tpic, webp
        return eTuttlePluginBitDepth16;
    }

    return bitDepth;
}

bool
WriteOIIOPlugin::displayWindowSupportedByFormat(const string& filename) const
{
    std::unique_ptr<ImageOutput> output = createFormatOutput(filename);
    if (output) {
        return output->supports("displaywindow");
    } else {
        return false;
    }
}

static bool
has_suffix(const string& str,
           const string& suffix)
{
    return (str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0);
}

void
WriteOIIOPlugin::onOutputFileChanged(const string& filename,
                                     bool setColorSpace)
{
    if (setColorSpace) {
#ifdef OFX_IO_USING_OCIO
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
            if (_ocio->hasColorspace("sRGB")) {
                // nuke-default, blender, natron
                _ocio->setOutputColorspace("sRGB");
            } else if (_ocio->hasColorspace("sRGB D65")) {
                // blender-cycles
                _ocio->setOutputColorspace("sRGB D65");
            } else if (_ocio->hasColorspace("rrt_srgb")) {
                // rrt_srgb in aces
                _ocio->setOutputColorspace("rrt_srgb");
            } else if (_ocio->hasColorspace("srgb8")) {
                // srgb8 in spi-vfx
                _ocio->setOutputColorspace("srgb8");
            }
            break;
        }
        case eTuttlePluginBitDepth10:
        case eTuttlePluginBitDepth12:
        case eTuttlePluginBitDepth16: {
            if (has_suffix(filename, ".cin") || has_suffix(filename, ".dpx") || has_suffix(filename, ".CIN") || has_suffix(filename, ".DPX")) {
                // Cineon or DPX file
                if (_ocio->hasColorspace("Cineon")) {
                    // Cineon in nuke-default, blender
                    _ocio->setOutputColorspace("Cineon");
                } else if (_ocio->hasColorspace("Cineon Log Curve")) {
                    // Curves/Cineon Log Curve in natron
                    _ocio->setOutputColorspace("Cineon Log Curve");
                } else if (_ocio->hasColorspace("REDlogFilm")) {
                    // REDlogFilm in aces 1.0.0
                    _ocio->setOutputColorspace("REDlogFilm");
                } else if (_ocio->hasColorspace("cineon")) {
                    // cineon in aces 0.7.1
                    _ocio->setOutputColorspace("cineon");
                } else if (_ocio->hasColorspace("adx10")) {
                    // adx10 in aces 0.1.1
                    _ocio->setOutputColorspace("adx10");
                } else if (_ocio->hasColorspace("lg10")) {
                    // lg10 in spi-vfx
                    _ocio->setOutputColorspace("lg10");
                } else if (_ocio->hasColorspace("lm10")) {
                    // lm10 in spi-anim
                    _ocio->setOutputColorspace("lm10");
                } else {
                    _ocio->setOutputColorspace(OCIO::ROLE_COMPOSITING_LOG);
                }
            } else {
                if (_ocio->hasColorspace("Rec709")) {
                    // nuke-default
                    _ocio->setOutputColorspace("Rec709");
                } else if (_ocio->hasColorspace("nuke_rec709")) {
                    // blender
                    _ocio->setOutputColorspace("nuke_rec709");
                } else if (_ocio->hasColorspace("Rec 709 Curve")) {
                    // natron
                    _ocio->setOutputColorspace("Rec 709 Curve");
                } else if (_ocio->hasColorspace("Rec.709 - Full")) {
                    // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                    _ocio->setOutputColorspace("Rec.709 - Full");
                } else if (_ocio->hasColorspace("out_rec709full")) {
                    // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                    _ocio->setOutputColorspace("out_rec709full");
                } else if (_ocio->hasColorspace("rrt_rec709_full_100nits")) {
                    // rrt_rec709_full_100nits in aces 0.7.1
                    _ocio->setOutputColorspace("rrt_rec709_full_100nits");
                } else if (_ocio->hasColorspace("rrt_rec709")) {
                    // rrt_rec709 in aces 0.1.1
                    _ocio->setOutputColorspace("rrt_rec709");
                } else if (_ocio->hasColorspace("hd10")) {
                    // hd10 in spi-anim and spi-vfx
                    _ocio->setOutputColorspace("hd10");
                }
            }
            break;
        }
        default:
            _ocio->setOutputColorspace(OCIO::ROLE_SCENE_LINEAR);
        } // switch
#endif // ifdef OFX_IO_USING_OCIO
    }

    refreshParamsVisibility(filename);
} // WriteOIIOPlugin::onOutputFileChanged

/**
 * @brief Does the given filename support alpha channel.
 **/
bool
WriteOIIOPlugin::supportsAlpha(const std::string& filename) const
{
    std::unique_ptr<ImageOutput> output = createFormatOutput(filename);

    // output is NULL if OIIO cannot write this format (#80)
    return kSupportsRGBA && output && output->supports("alpha");
}

void
WriteOIIOPlugin::refreshParamsVisibility(const string& filename)
{
    std::unique_ptr<ImageOutput> output = createFormatOutput(filename);
    if (output) {
        _tileSize->setIsSecretAndDisabled(!output->supports("tiles"));
        //_outputLayers->setIsSecretAndDisabled(!output->supports("nchannels"));

        // hasQuality: search for uses of decode_compression_metadata() in OIIO source code.
        // output->supports("quality") still returns false for all formats, but may
        // be implemented someday, see https://github.com/OpenImageIO/oiio/issues/3859
        bool hasQuality = (strcmp(output->format_name(), "jpeg") == 0 ||
                           strcmp(output->format_name(), "webp") == 0 ||
                           strcmp(output->format_name(), "heic") == 0 ||
                           strcmp(output->format_name(), "avif") == 0 ||
                           output->supports("quality"));
        bool hasDWA = false;
        bool hasZIP = false;
        bool hasZSTD = false;
        bool isEXR = strcmp(output->format_name(), "openexr") == 0;
        bool isTIFF = strcmp(output->format_name(), "tiff") == 0;
        if (isEXR || isTIFF) {
            int compression_i;
            _compression->getValue(compression_i);
            EParamCompression compression = (EParamCompression)compression_i;
            hasQuality = (compression == eParamCompressionJPEG);
            hasZIP = (compression == eParamCompressionZip);
            if (isEXR) {
                hasDWA = (compression == eParamCompressionDWAa) || (compression == eParamCompressionDWAb);
                hasZIP |= (compression == eParamCompressionZips);
                hasZSTD = (compression == eParamCompressionZSTD);
            }
        }
        _dwaCompressionLevel->setIsSecretAndDisabled(!hasDWA);
        _zipCompressionLevel->setIsSecretAndDisabled(!hasZIP);
        _zstdCompressionLevel->setIsSecretAndDisabled(!hasZSTD);
        _quality->setIsSecretAndDisabled(!hasQuality);
        if (_views) {
            _views->setIsSecretAndDisabled(!isEXR);
        }
        if (_lineOrder) {
            _lineOrder->setIsSecretAndDisabled(!isEXR);
        }
        if (_textureformat) {
            _textureformat->setIsSecretAndDisabled(!isEXR);
        }
        if (_parts) {
            _parts->setIsSecretAndDisabled(!output->supports("multiimage"));
        }
    } else {
        _tileSize->setIsSecretAndDisabled(true);
        //_outputLayers->setIsSecretAndDisabled(true);
        _quality->setIsSecretAndDisabled(true);
        _dwaCompressionLevel->setIsSecretAndDisabled(true);
        _zipCompressionLevel->setIsSecretAndDisabled(true);
        _zstdCompressionLevel->setIsSecretAndDisabled(true);
        if (_views) {
            _views->setIsSecretAndDisabled(true);
        }
        if (_lineOrder) {
            _lineOrder->setIsSecretAndDisabled(true);
        }
        if (_textureformat) {
            _textureformat->setIsSecretAndDisabled(true);
        }
        if (_parts) {
            _parts->setIsSecretAndDisabled(true);
        }
    }
}

// The file that an ImageOutput writes through (ImageOutput::set_ioproxy()). It keeps the first write error, because
// OpenImageIO does not report every one: its PNG writer ignores them, and its JPEG writer, when it writes to a "file"
// proxy, exits the process on them (libjpeg's default error_exit()) (#362).
// It uses IOFile(FILE*, Mode) and its protected m_file. If a writer does not use the proxy after all, it writes the file
// by name, as without it.
// - proxytype() is not "file", so that the JPEG writer encodes into memory, then writes that through this proxy
//   (OpenImageIO 3.1.17 jpegoutput.cpp:150-157).
// - The file is created by the first write, so that a writer that ignores the proxy and writes the file by name
//   (JPEG 2000 with OpenJPH: jpeg2000output.cpp:228-236, :818-819) is left alone.
// Used by one ImageOutput, which is not called concurrently: no mutex.
class CheckedFile final
    : public Filesystem::IOFile
{
public:
    explicit CheckedFile(const string& filename)
        : IOFile(static_cast<FILE*>(nullptr), Write)
        , _filename(filename)
    {
    }

    CheckedFile(const CheckedFile&) = delete;
    CheckedFile& operator=(const CheckedFile&) = delete;
    ~CheckedFile() override { close(); }

    const char* proxytype() const override { return "checkedfile"; }
    bool seek(int64_t offset) override
    {
        if (!m_file) { // the file is not created yet: it will be positioned there
            m_pos = offset;
            return offset >= 0;
        }
        return IOFile::seek(offset);
    }
    size_t write(const void* buf, size_t size) override { return checked(size, create() ? IOFile::write(buf, size) : 0); }
    // (on MS Windows, IOFile::pwrite() calls seek() and write(), so a short write is checked twice: harmless)
    size_t pwrite(const void* buf, size_t size, int64_t offset) override { return checked(size, create() ? IOFile::pwrite(buf, size, offset) : 0); }
#if OIIO_VERSION >= 30000
    void flush() override
#else
    void flush() const override // const before OpenImageIO 3.0
#endif
    {
        if (m_file && (std::fflush(m_file) != 0)) {
            fail();
        }
    }
    // fclose() writes what stdio buffers, so it may fail too
    void close() override
    {
        if (m_file && (std::fclose(m_file) != 0)) {
            fail();
        }
        m_file = nullptr;
        m_mode = Closed;
    }

    // The first error (empty if none). Call it after close().
    const string& failure() const { return _failure; }

private:
    // true if the file is open (created on the first call)
    bool create()
    {
        if (m_file) {
            return true;
        }
        if (_created || (m_mode != Write)) {
            return false;
        }
        _created = true;
        m_file = Filesystem::fopen(_filename, "w+b"); // as IOFile (UTF-8 paths on MS Windows)
        if (!m_file || ((m_pos != 0) && (Filesystem::fseek(m_file, m_pos, SEEK_SET) != 0))) {
            fail();
            return false;
        }
        return true;
    }
    size_t checked(size_t size, size_t written)
    {
        if (written != size) {
            fail();
        }
        return written;
    }
    // records errno, if it is the first error (const, for flush() before OpenImageIO 3.0)
    void fail() const
    {
        if (_failure.empty()) {
            const int e = errno;
            _failure = e ? std::generic_category().message(e) : string("write error");
        }
    }

    const string _filename;
    bool _created = false;
    mutable string _failure;
};

struct WriteOIIOEncodePlanesData {
    // The file of output, if the format supports ioproxy (#362). Declared before output, whose destructor may still
    // write to it.
    std::unique_ptr<CheckedFile> file;
    std::unique_ptr<ImageOutput> output;
    vector<ImageSpec> specs;
    string filename; // the file that output writes (for the error messages of endEncodeParts())
    bool zstdWrittenAsZip = false; // an EXR file with the zstd compression, which OpenImageIO writes as zip (#171)
    bool makeTexture = false;
    ImageBufAlgo::MakeTextureMode textureMode = ImageBufAlgo::MakeTxTexture;
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
    assert((packingRequired && planes.size() == 1) || !packingRequired);

    assert(!viewsToRender.empty());
    assert(user_data);
    WriteOIIOEncodePlanesData* data = (WriteOIIOEncodePlanesData*)user_data;
    data->filename = filename;
    data->output = ImageOutput::create(filename);
    if (!data->output.get()) {
        // output is NULL: retrieve OpenImageIO's error (#228)
        const string reason = OIIO::geterror();
        setPersistentMessage(Message::eMessageError, "", string("Cannot create output file ") + filename + (reason.empty() ? string() : ": " + reason));
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    if (!data->output->supports("multiimage") && (partsSplitting != eLayerViewsSinglePart)) {
        stringstream ss;
        ss << data->output->format_name() << " does not support writing multiple views/layers into a single file.";
        setPersistentMessage(Message::eMessageError, "", ss.str());
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    bool isEXR = strcmp(data->output->format_name(), "openexr") == 0;
    if (!isEXR && (viewsToRender.size() > 1)) {
        stringstream ss;
        ss << data->output->format_name() << " format cannot render multiple views in a single file, use %v or %V in filename to render separate files per view";
        setPersistentMessage(Message::eMessageError, "", ss.str());
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    OIIO::TypeDesc oiioBitDepth;
    // size_t sizeOfChannel = 0;
    int bitsPerSample = 0;
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
        // sizeOfChannel = 1;
        break;
    case eTuttlePluginBitDepth10:
        oiioBitDepth = TypeDesc::UINT16;
        bitsPerSample = 10;
        // sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth12:
        oiioBitDepth = TypeDesc::UINT16;
        bitsPerSample = 12;
        // sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth16:
        oiioBitDepth = TypeDesc::UINT16;
        bitsPerSample = 16;
        // sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth16f:
        oiioBitDepth = TypeDesc::HALF;
        bitsPerSample = 16;
        // sizeOfChannel = 2;
        break;
    case eTuttlePluginBitDepth32:
        oiioBitDepth = TypeDesc::UINT32;
        bitsPerSample = 32;
        // sizeOfChannel = 4;
        break;
    case eTuttlePluginBitDepth32f:
        oiioBitDepth = TypeDesc::FLOAT;
        bitsPerSample = 32;
        // sizeOfChannel = 4;
        break;
    case eTuttlePluginBitDepth64:
        oiioBitDepth = TypeDesc::UINT64;
        bitsPerSample = 64;
        // sizeOfChannel = 8;
        break;
    case eTuttlePluginBitDepth64f:
        oiioBitDepth = TypeDesc::DOUBLE;
        bitsPerSample = 64;
        // sizeOfChannel = 8;
        break;
    } // switch

    // Base spec with a stub nChannels
    ImageSpec spec(bounds.x2 - bounds.x1, bounds.y2 - bounds.y1, 4, oiioBitDepth);
    int quality = 100;
    if (!_quality->getIsSecret()) {
        _quality->getValue(quality);
    }
    double dwaCompressionLevel = 45.;
    if (!_dwaCompressionLevel->getIsSecret()) {
        _dwaCompressionLevel->getValue(dwaCompressionLevel);
    }
    int zipCompressionLevel = 4;
    if (!_zipCompressionLevel->getIsSecret()) {
        _zipCompressionLevel->getValue(zipCompressionLevel);
    }
    int zstdCompressionLevel = kParamOutputZSTDCompressionLevelDefault;
    if (!_zstdCompressionLevel->getIsSecret()) {
        _zstdCompressionLevel->getValue(zstdCompressionLevel);
    }
    int orientation;
    _orientation->getValue(orientation);
    int compression_i;
    _compression->getValue(compression_i);
    string compression;
    int lineOrder_i;
    _lineOrder->getValue(lineOrder_i);
    string lineOrder;

    switch ((EParamCompression)compression_i) {
    case eParamCompressionAuto: {
        // Set compression string for formats that only have a single compression type.
        static const char* formats[] = {"jpeg", "webp", "heic", "avif", nullptr};
        for (int i = 0; formats[i] != nullptr; ++i) {
            if (strcmp(data->output->format_name(), formats[i]) == 0) {
                compression = formats[i];
                break;
            }
        }
    } break;
    case eParamCompressionNone: // EXR, TIFF, IFF
        compression = "none";
        break;
    case eParamCompressionZip: // EXR, TIFF, Zfile
        compression = "zip";
        break;
    case eParamCompressionZips: // EXR
        compression = "zips";
        break;
    case eParamCompressionRle: // DPX, IFF, EXR, TGA, RLA
        compression = "rle";
        break;
    case eParamCompressionPiz: // EXR
        compression = "piz";
        break;
    case eParamCompressionPxr24: // EXR
        compression = "pxr24";
        break;
    case eParamCompressionB44: // EXR
        compression = "b44";
        break;
    case eParamCompressionB44a: // EXR
        compression = "b44a";
        break;
    case eParamCompressionDWAa: // EXR
        compression = "dwaa";
        break;
    case eParamCompressionDWAb: // EXR
        compression = "dwab";
        break;
    case eParamCompressionHTJ2K256: // EXR
        compression = "htj2k256";
        break;
    case eParamCompressionHTJ2K32: // EXR
        compression = "htj2k32";
        break;
    case eParamCompressionZSTD: // EXR
        compression = "zstd";
        break;
    case eParamCompressionLZW: // TIFF
        compression = "lzw";
        break;
    case eParamCompressionCCITTRLE: // TIFF
        compression = "ccittrle";
        break;
    case eParamCompressionJPEG: // TIFF
        compression = "jpeg";
        break;
    case eParamCompressionPACKBITS: // TIFF
        compression = "packbits";
        break;
    }
    data->zstdWrittenAsZip = isEXR && (compression == "zstd") && !oiioWritesEXRZstd();

    switch ((EParamLineOrder)lineOrder_i) {
    case eParamLineOrderIncreasingY:
        lineOrder = "increasingY";
        break;
    case eParamLineOrderDecreasingY:
#if OIIO_VERSION < 20602
        // OpenImageIO < 2.6.2 reads out of bounds when writing decreasingY EXR files
        // (fixed by https://github.com/AcademySoftwareFoundation/OpenImageIO/pull/4215)
        if (isEXR) {
            setPersistentMessage(Message::eMessageError, "", "Line Order decreasingY requires OpenImageIO 2.6.2 or later.");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
#endif
        lineOrder = "decreasingY";
        break;
    }

    spec.attribute("oiio:BitsPerSample", bitsPerSample);
    // oiio:UnassociatedAlpha should be set if the data buffer is unassociated/unpremultiplied.
    // However, WriteOIIO::getExpectedInputPremultiplication() stated that input to the encode()
    // function should always be premultiplied/associated
    // spec.attribute("oiio:UnassociatedAlpha", premultiply);
#ifdef OFX_IO_USING_OCIO
    string ocioColorspace;
    _ocio->getOutputColorspaceAtTime(time, ocioColorspace);
    float gamma = 0.f;
    string colorSpaceStr;
    if (ocioColorspace == "Gamma1.8") {
        // Gamma1.8 in nuke-default
        colorSpaceStr = "GammaCorrected";
        gamma = 1.8f;
    } else if ((ocioColorspace == "Gamma2.2") || (ocioColorspace == "vd8") || (ocioColorspace == "vd10") || (ocioColorspace == "vd16") || (ocioColorspace == "VD16")) {
        // Gamma2.2 in nuke-default
        // vd8, vd10, vd16 in spi-anim and spi-vfx
        // VD16 in blender
        colorSpaceStr = "GammaCorrected";
        gamma = 2.2f;
    } else if ((ocioColorspace == "sRGB") || (ocioColorspace == "sRGB D65") || (ocioColorspace == "sRGB (D60 sim.)") || (ocioColorspace == "out_srgbd60sim") || (ocioColorspace == "rrt_srgb") || (ocioColorspace == "srgb8")) {
        // sRGB in nuke-default and blender
        // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
        // rrt_srgb in aces
        // srgb8 in spi-vfx
        colorSpaceStr = "sRGB";
    } else if ((ocioColorspace == "Rec709") || (ocioColorspace == "nuke_rec709") || (ocioColorspace == "Rec 709 Curve") || (ocioColorspace == "Rec.709 - Full") || (ocioColorspace == "out_rec709full") || (ocioColorspace == "rrt_rec709") || (ocioColorspace == "hd10")) {
        // Rec709 in nuke-default
        // nuke_rec709 in blender
        // out_rec709full or "Rec.709 - Full" in aces 1.0.0
        // rrt_rec709 in aces
        // hd10 in spi-anim and spi-vfx
        colorSpaceStr = "Rec709";
    } else if ((ocioColorspace == "KodakLog") || (ocioColorspace == "Cineon") || (ocioColorspace == "Cineon Log Curve") || (ocioColorspace == "REDlogFilm") || (ocioColorspace == "lg10")) {
        // Cineon in nuke-default
        // REDlogFilm in aces 1.0.0
        // lg10 in spi-vfx and blender
        colorSpaceStr = "KodakLog";
    } else if ((ocioColorspace == OCIO::ROLE_SCENE_LINEAR) || (ocioColorspace == "Linear") || (ocioColorspace == "linear") || (ocioColorspace == "ACES2065-1") || (ocioColorspace == "aces") || (ocioColorspace == "lnf") || (ocioColorspace == "ln16")) {
        // linear in nuke-default
        // ACES2065-1 in aces 1.0.0
        // aces in aces
        // lnf, ln16 in spi-anim and spi-vfx
        colorSpaceStr = "Linear";
    } else if ((ocioColorspace == OCIO::ROLE_DATA) || (ocioColorspace == "raw") || (ocioColorspace == "Raw") || (ocioColorspace == "ncf")) {
        // raw in nuke-default
        // raw in aces
        // Raw in blender
        // ncf in spi-anim and spi-vfx
        // leave empty
    } else {
        // unknown color-space, don't do anything
    }
    if (!colorSpaceStr.empty()) {
        spec.attribute("oiio:ColorSpace", colorSpaceStr);
    }
    if (gamma != 0.) {
        spec.attribute("oiio:Gamma", gamma);
    }
#endif // ifdef OFX_IO_USING_OCIO
    if (!_quality->getIsSecret()) {
        compression += ':' + std::to_string(quality);
    } else if (!_dwaCompressionLevel->getIsSecret()) {
        compression += ':' + std::to_string((int)dwaCompressionLevel);
    } else if (!_zipCompressionLevel->getIsSecret()) {
        compression += ':' + std::to_string(zipCompressionLevel); // zip and zips compression level 1 to 9 range
    } else if (!_zstdCompressionLevel->getIsSecret()) {
        compression += ':' + std::to_string(zstdCompressionLevel); // zstd compression level 1 to 22 range
    }
    spec.attribute("Orientation", orientation + 1);
    if (isEXR && !lineOrder.empty()) {
        spec.attribute("openexr:lineOrder", lineOrder);
    }
    if (!compression.empty()) { // some formats have a good value for the default compression
        spec.attribute("compression", compression);
    }
    if (pixelAspectRatio != 1.) {
        spec.attribute("PixelAspectRatio", pixelAspectRatio);
    }

    if (data->output->supports("tiles")) {
        spec.x = bounds.x1;
        spec.y = bounds.y1;
        spec.full_x = bounds.x1;
        spec.full_y = bounds.y1;

        bool clipToRoD = false;
        if (_clipToRoD && !_clipToRoD->getIsSecret()) {
            _clipToRoD->getValue(clipToRoD);
        }
        if (clipToRoD) {
            // The bounds were set to the input RoD.
            // Set the display window to format using user prefs
            OfxRectI format;
            double formatPar;
            getSelectedOutputFormat(time, viewsToRender.empty() ? 0 : viewsToRender.begin()->first, &format, &formatPar);
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

    assert(!planes.empty());
    switch (partsSplitting) {
    case eLayerViewsSinglePart: {
        data->specs.resize(1);

        ImageSpec partSpec = spec;
        TypeDesc tv(TypeDesc::STRING, viewsToRender.size());

        vector<ustring> ustrvec(viewsToRender.size());
        {
            int i = 0;
            for (map<int, string>::const_iterator it = viewsToRender.begin(); it != viewsToRender.end(); ++it, ++i) {
                ustrvec[i] = it->second;
            }
        }

        if (viewsToRender.size() > 1) {
            partSpec.attribute("multiView", tv, &ustrvec[0]);
        }
        vector<string> channels;

        for (map<int, string>::const_iterator view = viewsToRender.begin(); view != viewsToRender.end(); ++view) {
            for (std::list<string>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
                bool isColor = ((*it == kFnOfxImagePlaneColour) || (*it == kOfxImageComponentRGB) || (*it == kOfxImageComponentAlpha) || (*it == kOfxImageComponentRGBA));
                string rawComponents;
                if (*it == kFnOfxImagePlaneColour) {
                    rawComponents = _inputClip->getPixelComponentsProperty();
                } else {
                    rawComponents = *it;
                }

                MultiPlane::ImagePlaneDesc plane, pairedPlane;
                MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(rawComponents, &plane, &pairedPlane);

                std::vector<std::string> planeChannels = plane.getChannels();
                if (plane.getNumComponents() > 0 && !isColor) {

                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = plane.getPlaneLabel() + "." + planeChannels[i];
                    }
                }
                if ((viewsToRender.size() > 1) && (view != viewsToRender.begin())) {
                    /// Prefix the view name for all views except the main
                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = view->second + "." + planeChannels[i];
                    }
                }

                if (!packingRequired) {
                    channels.insert(channels.end(), planeChannels.begin(), planeChannels.end());
                } else {
                    // packingMapping holds the selected R/G/B/A checkboxes (and -1 for padding): a selected channel
                    // may be missing from the plane (A of an RGB plane), so don't index past planeChannels (#90)
                    // same prefix (view and/or layer name) as the plane's own channel names, e.g. "right." or "Forward."
                    const string prefix = planeChannels.empty() ? string() : planeChannels[0].substr(0, planeChannels[0].rfind('.') + 1);
                    static const char* const rgbaNames[4] = { "R", "G", "B", "A" };
                    for (std::size_t i = 0; i < packingMapping.size(); ++i) {
                        const int c = packingMapping[i];
                        if ((c >= 0) && (c < (int)planeChannels.size())) {
                            channels.push_back(planeChannels[c]);
                        } else {
                            channels.push_back(prefix + rgbaNames[(c >= 0 && c < 4) ? c : (i < 4 ? i : 3)]);
                        }
                    }
                }
            }
        } //  for (std::size_t v = 0; v < viewsToRender.size(); ++v) {
        if (channels.size() == 4 && (channels[3] == "A" || channels[3] == "alpha")) {
            partSpec.alpha_channel = 3;
        } else if (channels.size() == 1 && (channels[0] == "A" || channels[0] == "alpha")) {
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
        data->specs.resize(viewsToRender.size());

        int specIndex = 0;
        for (map<int, string>::const_iterator view = viewsToRender.begin(); view != viewsToRender.end(); ++view) {
            ImageSpec partSpec = spec;
            if (viewsToRender.size() > 1) {
                partSpec.attribute("view", view->second);
            }
            vector<string> channels;

            for (std::list<string>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
                bool isColor = ((*it == kFnOfxImagePlaneColour) || (*it == kOfxImageComponentRGB) || (*it == kOfxImageComponentAlpha) || (*it == kOfxImageComponentRGBA));
                string rawComponents;
                if (*it == kFnOfxImagePlaneColour) {
                    rawComponents = _inputClip->getPixelComponentsProperty();
                } else {
                    rawComponents = *it;
                }

                MultiPlane::ImagePlaneDesc plane, pairedPlane;
                MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(rawComponents, &plane, &pairedPlane);

                std::vector<std::string> planeChannels = plane.getChannels();

                if (plane.getNumComponents() > 0 && !isColor) {
                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = plane.getPlaneLabel() + "." + planeChannels[i];
                    }
                }

                if (!packingRequired) {
                    channels.insert(channels.end(), planeChannels.begin(), planeChannels.end());
                } else {
                    // packingMapping holds the selected R/G/B/A checkboxes (and -1 for padding): a selected channel
                    // may be missing from the plane (A of an RGB plane), so don't index past planeChannels (#90)
                    // same prefix (view and/or layer name) as the plane's own channel names, e.g. "right." or "Forward."
                    const string prefix = planeChannels.empty() ? string() : planeChannels[0].substr(0, planeChannels[0].rfind('.') + 1);
                    static const char* const rgbaNames[4] = { "R", "G", "B", "A" };
                    for (std::size_t i = 0; i < packingMapping.size(); ++i) {
                        const int c = packingMapping[i];
                        if ((c >= 0) && (c < (int)planeChannels.size())) {
                            channels.push_back(planeChannels[c]);
                        } else {
                            channels.push_back(prefix + rgbaNames[(c >= 0 && c < 4) ? c : (i < 4 ? i : 3)]);
                        }
                    }
                }
            }
            if (channels.size() == 4 && (channels[3] == "A" || channels[3] == "alpha")) {
                partSpec.alpha_channel = 3;
            } else if (channels.size() == 1 && (channels[0] == "A" || channels[0] == "alpha")) {
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
        } //  for (std::size_t v = 0; v < viewsToRender.size(); ++v) {
        break;
    }
    case eLayerViewsSplitViewsLayers: {
        data->specs.resize(viewsToRender.size() * planes.size());

        int specIndex = 0;
        for (map<int, string>::const_iterator view = viewsToRender.begin(); view != viewsToRender.end(); ++view) {
            for (std::list<string>::const_iterator it = planes.begin(); it != planes.end(); ++it) {
                bool isColor = ((*it == kFnOfxImagePlaneColour) || (*it == kOfxImageComponentRGB) || (*it == kOfxImageComponentAlpha) || (*it == kOfxImageComponentRGBA));
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
                if (plane.getNumComponents() > 0 && !isColor) {
                    for (std::size_t i = 0; i < planeChannels.size(); ++i) {
                        planeChannels[i] = plane.getPlaneLabel() + "." + planeChannels[i];
                    }
                }

                vector<string> channels;
                if (!packingRequired) {
                    channels.insert(channels.end(), planeChannels.begin(), planeChannels.end());
                } else {
                    // packingMapping holds the selected R/G/B/A checkboxes (and -1 for padding): a selected channel
                    // may be missing from the plane (A of an RGB plane), so don't index past planeChannels (#90)
                    // same prefix (view and/or layer name) as the plane's own channel names, e.g. "right." or "Forward."
                    const string prefix = planeChannels.empty() ? string() : planeChannels[0].substr(0, planeChannels[0].rfind('.') + 1);
                    static const char* const rgbaNames[4] = { "R", "G", "B", "A" };
                    for (std::size_t i = 0; i < packingMapping.size(); ++i) {
                        const int c = packingMapping[i];
                        if ((c >= 0) && (c < (int)planeChannels.size())) {
                            channels.push_back(planeChannels[c]);
                        } else {
                            channels.push_back(prefix + rgbaNames[(c >= 0 && c < 4) ? c : (i < 4 ? i : 3)]);
                        }
                    }
                }

                if (channels.size() == 4 && (channels[3] == "A" || channels[3] == "alpha")) {
                    partSpec.alpha_channel = 3;
                } else if (channels.size() == 1 && (channels[0] == "A" || channels[0] == "alpha")) {
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
        } //  for (std::size_t v = 0; v < viewsToRender.size(); ++v) {
        break;
    }
    } // switch

    // If the file exists (which means "overwrite" was checked), remove it first.
    // See https://github.com/NatronGitHub/Natron/issues/666
    if (Filesystem::exists(filename)) {
        string err;
        bool ok = Filesystem::remove(filename, err);
        if (!ok) {
            setPersistentMessage(Message::eMessageError, "", err);
            throwSuiteStatusException(kOfxStatFailed);
        }
    }
    int textureformat_i = eParamTextureformatNone;
    if (strcmp(data->output->format_name(), "openexr") == 0) { // texture output is EXR-only
        _textureformat->getValue(textureformat_i);
    }
    if (textureformat_i != eParamTextureformatNone) {
        // Textures are written by ImageBufAlgo::make_texture() in encodePart(), which
        // creates the tiled, MIP-mapped file itself.
        if (data->specs.size() != 1) {
            setPersistentMessage(Message::eMessageError, "", "Texture output only supports writing a single layer and view.");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        data->makeTexture = true;
        switch ((EParamTextureformat)textureformat_i) {
        case eParamTextureformatLatLong:
            data->textureMode = ImageBufAlgo::MakeTxEnvLatl;
            break;
        case eParamTextureformatShadow:
            if (data->specs[0].nchannels != 1) {
                setPersistentMessage(Message::eMessageError, "", "Shadow maps require a single-channel image (e.g. depth or alpha).");
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            data->textureMode = ImageBufAlgo::MakeTxShadow;
            break;
        case eParamTextureformatPlain:
        default:
            data->textureMode = ImageBufAlgo::MakeTxTexture;
            break;
        }

        return;
    }
    if (data->output->supports("ioproxy")) {
        data->file = std::make_unique<CheckedFile>(filename);
        // set_ioproxy(), not the oiio:ioproxy attribute, which the multi-part EXR writer ignores (OpenImageIO 3.1.17
        // exroutput.cpp:765-790); true, since the output supports ioproxy
        (void)data->output->set_ioproxy(data->file.get());
    }
    // Some formats only support opening all subimages at once.
    // See https://openimageio.readthedocs.io/en/stable/imageoutput.html
    if (!data->output->open(filename, data->specs.size(), &data->specs.front())) {
        reportOutputError(*data);
    }
} // WriteOIIOPlugin::beginEncodeParts

void
WriteOIIOPlugin::reportOutputError(const WriteOIIOEncodePlanesData& data)
{
    string error = data.output->geterror();
    // e.g. "File too large", which the error of OpenImageIO may not say ("Failed OpenEXR write: ... File output failed.")
    if (data.file && !data.file->failure().empty() && (error.find(data.file->failure()) == string::npos)) {
        error += (error.empty() ? "" : " (") + data.file->failure() + (error.empty() ? "" : ")");
    }
    reportWriteError(data.filename, error);
}

void
WriteOIIOPlugin::reportWriteError(const string& filename,
                                  const string& error)
{
    setPersistentMessage(Message::eMessageError, "", "Cannot write " + filename + (error.empty() ? string() : ": " + error));
    // not throwSuiteStatusException(), which is not [[noreturn]]
    throw OFX::Exception::Suite(kOfxStatFailed);
}

void
WriteOIIOPlugin::encodePart(void* user_data,
                            const string& filename,
                            const float* pixelData,
                            int pixelDataNComps,
                            int planeIndex,
                            int rowBytes)
{
    assert(user_data);
    WriteOIIOEncodePlanesData* data = (WriteOIIOEncodePlanesData*)user_data;
    if (data->makeTexture) {
        assert(planeIndex == 0);
        const ImageSpec& outSpec = data->specs[planeIndex];
        ImageSpec srcSpec(outSpec.width, outSpec.height, outSpec.nchannels, TypeDesc::FLOAT);
        srcSpec.x = outSpec.x;
        srcSpec.y = outSpec.y;
        srcSpec.full_x = outSpec.full_x;
        srcSpec.full_y = outSpec.full_y;
        srcSpec.full_width = outSpec.full_width;
        srcSpec.full_height = outSpec.full_height;
        srcSpec.channelnames = outSpec.channelnames;
        srcSpec.alpha_channel = outSpec.alpha_channel;
        ImageBuf src(srcSpec);
        // do not use auto-stride as the buffer may have more components that what we want to write
        if (!src.set_pixels(src.roi(), TypeDesc::FLOAT,
                            (const char*)pixelData + (ptrdiff_t)(outSpec.height - 1) * (ptrdiff_t)rowBytes, // invert y
                            sizeof(float) * pixelDataNComps, // xstride
                            -rowBytes)) { // ystride
            setPersistentMessage(Message::eMessageError, "", src.geterror());
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        // The output spec carries the data format, tile size, compression and metadata.
        ImageSpec config = outSpec;
        if (config.tile_width == 0 || config.tile_height == 0) {
            config.tile_width = 64;
            config.tile_height = 64;
            config.tile_depth = 1;
        }
        std::stringstream err;
        (void)OIIO::geterror(); // clear any stale error message, which would be reported if make_texture fails
        if (!ImageBufAlgo::make_texture(data->textureMode, src, filename, config, &err)) {
            // make_texture reports its errors through OIIO::geterror(); outstream gets the verbose output
            string msg = OIIO::geterror();
            if (msg.empty()) {
                msg = err.str();
            }
            setPersistentMessage(Message::eMessageError, "", msg.empty() ? string("Could not write the texture file.") : msg);
            throwSuiteStatusException(kOfxStatFailed);
        }

        return;
    }
    if (planeIndex != 0) {
        if (!data->output->open(filename, data->specs[planeIndex], ImageOutput::AppendSubimage)) {
            reportOutputError(*data);
        }
    }

    TypeDesc format = TypeDesc::FLOAT;

    // do not use auto-stride as the buffer may have more components that what we want to write
    std::size_t xStride = format.size() * pixelDataNComps;
    // A failed write (e.g. a full disk) must fail the render, or the host reports a truncated file as written (#292).
    if (!data->output->write_image(format,
                                   (char*)pixelData + (ptrdiff_t)(data->specs[planeIndex].height - 1) * (ptrdiff_t)rowBytes, // invert y
                                   xStride, // xstride
                                   -rowBytes, // ystride
                                   AutoStride)) { // zstride
        reportOutputError(*data);
    }
}

void
WriteOIIOPlugin::endEncodeParts(void* user_data)
{
    assert(user_data);
    WriteOIIOEncodePlanesData* data = (WriteOIIOEncodePlanesData*)user_data;
    if (!data->makeTexture) { // (a texture is written by encodePart())
        // Many formats only write (or flush) the file when it is closed (#292).
        // Not ImageOutput::has_error(): OpenImageIO 3.1's TIFF writer records an error for the Exif directory of files
        // without Exif metadata ("failed TIFFWriteCustomDirectory() of the Exif data"), and close() succeeds.
        if (!data->output->close()) {
            reportOutputError(*data);
        }
        // Clear that error: OIIO keeps the errors of each ImageOutput in a thread_local map, from which ~ImageOutput()
        // does not erase them (OIIO 3.1.17 imageoutput.cpp, ~ImageOutput(): the output_error_messages erase is
        // commented out), so it would grow by one string per frame.
        (void)data->output->geterror();
        if (data->file) {
            data->file->close();
            if (!data->file->failure().empty()) {
                reportWriteError(data->filename, data->file->failure());
            }
        }
    }
    if (data->zstdWrittenAsZip) {
        _zstdWrittenAsZip = true;
    }
}

void
WriteOIIOPlugin::beginEncode(const string& /*filename*/,
                             const OfxRectI& /*rodPixel*/,
                             float /*pixelAspectRatio*/,
                             const BeginSequenceRenderArguments& /*args*/)
{
    _zstdWrittenAsZip = false;
}

void
WriteOIIOPlugin::endEncode(const EndSequenceRenderArguments& /*args*/)
{
    // #171: shown after the sequence render, since GenericWriterPlugin::renderFile() clears the persistent message
    // after each frame. A warning, not an error, so that the projects that use zstd still render.
    if (_zstdWrittenAsZip) {
        const string exrVersion = oiioOpenEXRVersion();
        setPersistentMessage(Message::eMessageWarning, "",
                             "The zstd compression needs OpenImageIO 3.2.1 or later, built with OpenEXR 3.5 or later (running with OpenImageIO "
                                 + oiioVersionString(openimageio_version()) + ", OpenEXR " + (exrVersion.empty() ? string("unknown") : exrVersion)
                                 + "): the EXR files were written with the zip compression.");
    }
}

bool
WriteOIIOPlugin::isImageFile(const string& /*fileExtension*/) const
{
    return true;
}

mDeclareWriterPluginFactory(WriteOIIOPluginFactory, ;, false);
void
WriteOIIOPluginFactory::unload()
{
    // stop OpenImageIO's thread pool if this was the last OIIO plug-in loaded
    OIIOPluginUnloaded(getUID());
}

void
WriteOIIOPluginFactory::load()
{
    _extensions.clear();
    // get extensions from OIIO: extension_list has the extensions of all formats, readers and writers, so
    // keep only those of the formats in output_format_list (e.g. OIIO reads but can't write Cineon, #80),
    // and among them those that a writer accepts (e.g. the BMP writer takes .bmp but not .dib), see below
    string extensions_list;
    getattribute("extension_list", extensions_list);
    string output_format_list;
    getattribute("output_format_list", output_format_list);
    std::set<string> outputFormats;
    {
        stringstream outputss(output_format_list);
        string outputFormat;
        while (std::getline(outputss, outputFormat, ',')) {
            outputFormats.insert(outputFormat);
        }
    }
    stringstream formatss(extensions_list);
    string format;
    std::list<string> extensionsl;
    while (std::getline(formatss, format, ';')) {
        stringstream extensionss(format);
        string extension;
        std::getline(extensionss, extension, ':'); // extract the format
        if (!outputFormats.empty() && (outputFormats.count(extension) == 0)) {
            continue; // OIIO has no writer for this format
        }
        while (std::getline(extensionss, extension, ',')) {
            extensionsl.push_back(extension);
        }
    }
    const char* extensions_denylist[] = {
        "avi",
        "mov",
        "qt",
        "mp4",
        "m4a",
        "3gp",
        "3g2",
        "mj2",
        "m4v",
        "mpg", // FFmpeg extensions - better supported by WriteFFmpeg
        "gif", // animated GIFs are only supported by FFmpeg
        "null", "nul", // OIIO's null output (discards the image)
        "term", // OIIO's terminal output
        NULL
    };
    for (const char* const* e = extensions_denylist; *e != NULL; ++e) {
        extensionsl.remove(*e);
    }
    // keep only the extensions that OIIO can actually write
    for (std::list<string>::iterator it = extensionsl.begin(); it != extensionsl.end();) {
        if (createFormatOutput("x." + *it)) {
            ++it;
        } else {
            it = extensionsl.erase(it);
        }
    }
    _extensions.assign(extensionsl.begin(), extensionsl.end());
    // MS Windows: OpenEXROutput's constructor (ImageOutput::create("x.exr") above) starts OpenEXR's pool
    // (pvt::set_exr_threads()), outside of any instance: stop it now, a host may exit without kOfxActionUnload
    // (#256). A no-op while an instance uses it; the next ExrThreadsUser starts it again (OpenImageIO would not: it
    // caches the thread count it set).
    stopUnusedExrThreads();
    // last: only count a successful load (if load throws, the host does not call unload)
    OIIOPluginLoaded(getUID());
}

/** @brief The basic describe function, passed a plugin descriptor */
void
WriteOIIOPluginFactory::describe(ImageEffectDescriptor& desc)
{
    GenericWriterDescribe(desc, eRenderFullySafe, _extensions, kPluginEvaluation, true, true);

    // basic labels
    desc.setLabel(kPluginName);
    // the list of extensions that OIIO can actually write (see load())
    string description = "Write images using OpenImageIO.\n\n"
                         "The OpenImageIO library used by this plug-in can write files with the following extensions:\n";
    for (std::size_t i = 0; i < _extensions.size(); ++i) {
        description += (i == 0 ? "" : ", ");
        description += _extensions[i];
    }
    description += ".";
    desc.setPluginDescription(description);

#if defined(OFX_EXTENSIONS_NATRON) && defined(OFX_EXTENSIONS_NUKE)
    gIsMultiplanarV2 = (getImageEffectHostDescription()->supportsDynamicChoices && getImageEffectHostDescription()->isMultiPlanar && fetchSuite(kFnOfxImageEffectPlaneSuite, 2, true));
#else
    gIsMultiplanarV2 = false;
#endif
} // WriteOIIOPluginFactory::describe

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
WriteOIIOPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                          ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor* page = GenericWriterDescribeInContextBegin(desc, context,
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
        IntParamDescriptor* param = desc.defineIntParam(kParamOutputZIPCompressionLevel);
        param->setLabel(kParamOutputZIPCompressionLevelLabel);
        param->setHint(kParamOutputZIPCompressionLevelHint);
        param->setRange(1, 9);
        param->setDisplayRange(1, 9);
        param->setDefault(kParamOutputZIPCompressionLevelDefault);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamOutputZSTDCompressionLevel);
        param->setLabel(kParamOutputZSTDCompressionLevelLabel);
        param->setHint(kParamOutputZSTDCompressionLevelHint);
        param->setRange(1, 22);
        param->setDisplayRange(1, 22);
        param->setDefault(kParamOutputZSTDCompressionLevelDefault);
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
        assert(param->getNOptions() == eParamCompressionHTJ2K256);
        param->appendOption(kParamOutputCompressionOptionHTJ2K256);
        assert(param->getNOptions() == eParamCompressionHTJ2K32);
        param->appendOption(kParamOutputCompressionOptionHTJ2K32);
        assert(param->getNOptions() == eParamCompressionZSTD);
        param->appendOption(kParamOutputCompressionOptionZSTD);
        param->setDefault(eParamCompressionAuto);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputLineOrder);
        param->setLabel(kParamOutputLineOrderLabel);
        param->setHint(kParamOutputLineOrderHint);
        assert(param->getNOptions() == eParamLineOrderIncreasingY);
        param->appendOption(kParamOutputLineOrderOptionIncreasingY);
        assert(param->getNOptions() == eParamLineOrderDecreasingY);
        param->appendOption(kParamOutputLineOrderOptionDecreasingY);
        param->setDefault(eParamLineOrderIncreasingY);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputTextureformat);
        param->setLabel(kParamOutputTextureformatLabel);
        param->setHint(kParamOutputTextureformatHint);
        assert(param->getNOptions() == eParamTextureformatNone);
        param->appendOption(kParamOutputTextureformatOptionNone);
        assert(param->getNOptions() == eParamTextureformatPlain);
        param->appendOption(kParamOutputTextureformatOptionPlain);
        assert(param->getNOptions() == eParamTextureformatLatLong);
        param->appendOption(kParamOutputTextureformatOptionLatLong);
        assert(param->getNOptions() == eParamTextureformatShadow);
        param->appendOption(kParamOutputTextureformatOptionShadow);
        param->setDefault(eParamTextureformatNone);
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
