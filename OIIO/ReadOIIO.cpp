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
 * OFX oiioReader plugin.
 * Reads an image using the OpenImageIO library.
 */

#include <algorithm>
#include <cctype> // std::isdigit
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring> // memset
#include <fstream>
#include <iostream>
#include <memory> // std::unique_ptr
#include <new> // std::bad_alloc
#include <set>
#include <sstream>

#include "ofxsMacros.h"

#include "OIIOGlobal.h"
// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <OpenImageIO/imageio.h>
GCC_DIAG_ON(unused-parameter)
// clang-format on

#ifdef OFX_IO_USING_LIBRAW
// clang-format off
GCC_DIAG_OFF(deprecated-declarations)
// ReadOIIO uses none of LibRaw's datastreams: libraw_datastream.h is not included (it includes <winsock2.h> on MS
// Windows, and still uses std::auto_ptr if LIBRAW_USE_AUTOPTR is defined)
#define __LIBRAW_DATASTREAM_H
class LibRaw_abstract_datastream;
#include <libraw.h>
#include <libraw_version.h>
GCC_DIAG_ON(deprecated-declarations)
// clang-format on
#else
// default to a recent version
#define LIBRAW_MAJOR_VERSION 0
#define LIBRAW_MINOR_VERSION 20
#define LIBRAW_PATCH_VERSION 2
#define _LIBRAW_VERSION_MAKE(a, b, c, d) #a "." #b "." #c "-" #d
#define LIBRAW_VERSION_MAKE(a, b, c, d) _LIBRAW_VERSION_MAKE(a, b, c, d)
#define LIBRAW_VERSION_STR                                          \
    LIBRAW_VERSION_MAKE(LIBRAW_MAJOR_VERSION, LIBRAW_MINOR_VERSION, \
                        LIBRAW_PATCH_VERSION, LIBRAW_VERSION_TAIL)
#define LIBRAW_MAKE_VERSION(major, minor, patch) \
    (((major) << 16) | ((minor) << 8) | (patch))
#define LIBRAW_VERSION                                              \
    LIBRAW_MAKE_VERSION(LIBRAW_MAJOR_VERSION, LIBRAW_MINOR_VERSION, \
                        LIBRAW_PATCH_VERSION)
#endif // OFX_IO_USING_LIBRAW
// LibRaw 0.18.x reports the GPL2/GPL3 demosaic packs through libraw_capabilities().
// LibRaw >= 0.19 dropped the packs; a patched LibRaw (e.g. Natron's MacPorts port
// with the gpl2/gpl3 variants) restores them and defines LIBRAW_HAS_DEMOSAIC_GPL_CAPS
// (in libraw_const.h). In both cases availability is detected at runtime.
#if defined(OFX_IO_USING_LIBRAW) && (defined(LIBRAW_HAS_DEMOSAIC_GPL_CAPS) || (LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 18, 0) && LIBRAW_VERSION < LIBRAW_MAKE_VERSION(0, 19, 0)))
#define OFX_IO_LIBRAW_RUNTIME_DEMOSAIC_CAPS
#endif
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 19, 0) && !defined(OFX_IO_LIBRAW_RUNTIME_DEMOSAIC_CAPS)
#pragma message WARN("ReadOIIO will not support advanced demosaicing options (AHD-Mod, AFD, VCD, Mixed, LMMSE, AMaZE), because LibRaw is more recent than 0.18 and does not provide the GPL demosaic packs.")
#endif

#include <ofxNatron.h>

#include "GenericOCIO.h"
#include "GenericReader.h"
#include "IOMutex.h"
#include "IOUtility.h"

#include <ofxsCoords.h>
#include <ofxsMultiPlane.h>

#define OFX_READ_OIIO_SUPPORTS_SUBIMAGES

using namespace OFX;
using namespace OFX::IO;
#ifdef OFX_IO_USING_OCIO
namespace OCIO = OCIO_NAMESPACE;
#endif
OIIO_NAMESPACE_USING

using std::make_pair;
using std::pair;
using std::string;
using std::stringstream;
using std::vector;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "ReadOIIO"
#define kPluginGrouping "Image/Readers"
#define kPluginDescription                                                              \
    "Read images using OpenImageIO.\n\n"                                                \
    "Output is always Premultiplied (alpha is associated).\n\n"                         \
    "The \"Image Premult\" parameter controls the file premultiplication state, "       \
    "and can be used to fix wrong file metadata (see the help for that parameter).\n\n" \
    "When reading an OpenEXR file with a dataWindow, it is converted to an OpenFX "     \
    "region of definition by flipping the y axis (y axis goes down in OpenEXR, "        \
    "up in OpenFX), and adding 1 pixel in every direction if the \"Edge Pixels\" "      \
    "parameter says that a black border should be added."
#define kPluginIdentifier "fr.inria.openfx.ReadOIIO"
#define kPluginVersionMajor 2 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.
#define kPluginEvaluation 91

#define kSupportsRGBA true
#define kSupportsRGB true
#define kSupportsXY true
#define kSupportsAlpha true
// (the default of the descriptor: an instance supports tiles only for a tiled file, see setSupportsTiles() below)
#define kSupportsTiles true
#define kIsMultiPlanar true

#define kParamShowMetadata "showMetadata"
#define kParamShowMetadataLabel "Image Info..."
#define kParamShowMetadataHint "Shows information and metadata from the image at current time."

#define kGroupAdvanced "advanced"
#define kGroupAdvancedLabel "Advanced Options", "Advanced format-specific option"

#define kGroupRaw "advancedRaw"
#define kGroupRawLabel "RAW", "Options for a variety of digital camera \"raw\" formats supported by the LibRaw library (http://www.libraw.org/)."

// int no_auto_bright
#define kParamRawAutoBright "rawAutoBright"
#define kParamRawAutoBrightLabel "Auto Bright", "If checked, use libraw's automatic increase of brightness by histogram (exposure correction)." // default: unckecked

// int use_camera_wb;
#define kParamRawUseCameraWB "rawUseCameraWB"
#define kParamRawUseCameraWBLabel "Use Camera WB", "If checked, and if possible, use the white balance from the camera." // default: checked

// float adjust_maximum_thr
#define kParamRawAdjustMaximumThr "rawAdjustMaximumThr"
#define kParamRawAdjustMaximumThrLabel "Adjust Maximum Thr.", "This parameters controls auto-adjusting of maximum value based on channel_maximum[] data, calculated from real frame data. If calculated maximum is greater than adjust_maximum_thr*maximum, than maximum is set to calculated_maximum.\n"                \
                                                              "Default: 0. If you set this value above 0.99999, then default value will be used. If you set this value below 0.00001, then no maximum adjustment will be performed. A value of 0.75 is reasonable for still shots, but sequences should always use 0.\n" \
                                                              "Adjusting maximum should not damage any picture (esp. if you use default value) and is very useful for correcting channel overflow problems (magenta clouds on landscape shots, green-blue highlights for indoor shots)." // default: 0.

#define kParamRawUserSat "rawUserSat"
#define kParamRawUserSatLabel "Max. value", "The camera sensor saturation (maximum) value. Raw values greater or equal to this are considered saturated and are processed using the algorithm specified by the rawHighlightMode parameter. 0 means to use the default value."

// int output_color;
#define kParamRawOutputColor "rawOutputColor"
#define kParamRawOutputColorLabel "Output Colorspace", "Output colorspace." // default: sRGB
#define kParamRawOutputColorRaw "Raw", "Raw data", "raw"
#define kParamRawOutputColorSRGB "sRGB", "sRGB", "srgb"
#define kParamRawOutputColorAdobe "Adobe", "Adobe RGB (1998)", "adobergb"
#define kParamRawOutputColorWide "Wide", "Wide-gamut RGB color space (or Adobe Wide Gamut RGB)", "wide"
#define kParamRawOutputColorProPhoto "ProPhoto", "Kodak ProPhoto RGB (or ROMM RGB)", "prophoto"
#define kParamRawOutputColorXYZ "XYZ", "CIE XYZ", "xyz"
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 18, 0)
#define kParamRawOutputColorACES "ACES", "AMPAS ACES", "aces"
#endif
enum RawOutputColorEnum {
    eRawOutputColorRaw = 0,
    eRawOutputColorSRGB,
    eRawOutputColorAdobe,
    eRawOutputColorWide,
    eRawOutputColorProPhoto,
    eRawOutputColorXYZ,
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 18, 0)
    eRawOutputColorACES,
#endif
};

#define kParamRawAber "rawAber"
#define kParamRawAberLabel "Aber.", "Correction of chromatic aberrations, given as a red multiplier and a blue multiplier. The default values of (1.,1.) correspond to no correction."

// int use_camera_matrix;
// 0: do not use embedded color profile
// 1 (default): use embedded color profile (if present) for DNG files (always); for other files only if use_camera_wb is set;
// 3: use embedded color data (if present) regardless of white balance setting.
#define kParamRawUseCameraMatrix "rawUseCameraMatrix"
#define kParamRawUseCameraMatrixLabel "Camera Matrix", "Use/don't use an embedded color matrix."
#define kParamRawUseCameraMatrixNone "None", "Do not use the embedded color matrix.", "none"
#define kParamRawUseCameraMatrixDefault "Default", "Use embedded color profile (if present) for DNG files (always); for other files only if rawUseCameraWb is set.", "default"
#define kParamRawUseCameraMatrixForce "Force", "Use embedded color data (if present) regardless of white balance setting.", "force"
enum RawUseCameraMatrixEnum {
    eRawUseCameraMatrixNone = 0,
    eRawUseCameraMatrixDefault,
    eRawUseCameraMatrixForce,
};

#define kParamRawHighlightMode "rawHighlightMode"
#define kParamRawHighlightModeLabel "Highlight Mode", "Algorithm for restoring highlight clippings. Highlights are part of your images that are burned due to the inability of your camera to capture the highlights. Highlight recovery is applied after white balance and demosaic."
#define kParamRawHighlightModeClip "Clip", "Clip all highlights to white.", "clip"
#define kParamRawHighlightModeUnclip "Unclip", "Leave highlights unclipped in various shades of pink.", "unclip"
#define kParamRawHighlightModeBlend "Blend", "Blend clipped and unclipped values for a gradual fade to white.", "blend"
#define kParamRawHighlightModeRebuild "Rebuild", "Reconstruct highlights with various levels of aggressiveness.", "rebuild"
enum RawHighlightModeEnum {
    eRawHighlightModeClip = 0,
    eRawHighlightModeUnclip,
    eRawHighlightModeBlend,
    eRawHighlightModeRebuild
};
#define kParamRawHighlightRebuildLevel "rawHighlightRebuildLevel"
#define kParamRawHighlightRebuildLevelLabel "Rebuild Level", "Level of aggressiveness used to rebuild highlights. rawHighlightRebuildLevel=2 (which corresponds to -H 5 in LibRaw/dcraw) is a good compromise.  If that's not good enough, use rawHighlightRebuildLevel=6, cut out the non-white highlights, and paste them into an image generated with rawHighlightRebuildLevel=0. "

// int exp_correc; float exp_shift
#define kParamRawExposure "rawExposure"
#define kParamRawExposureLabel "Exposure", "Amount of exposure correction before de-mosaicing, from 0.25 (2-stop darken) to 8 (3-stop brighten). (Default: 1., meaning no correction.)" // default: 1

#define kParamRawDemosaic "rawDemosaic"
#define kParamRawDemosaicLabel "Demosaic", "Force a demosaicing algorithm. Will fall back on AHD if the demosaicing algorithm is not available due to licence restrictions (AHD-Mod, AFD, VCD, Mixed, LMMSE are GPL2, AMaZE is GPL3)."
#define kParamRawDemosaicNone "None", "No demosaicing.", "none"
#define kParamRawDemosaicLinear "Linear", "Linear interpolation.", "linear"
#define kParamRawDemosaicVNG "VNG", "VNG interpolation.", "vng"
#define kParamRawDemosaicPPG "PPG", "PPG interpolation.", "ppg"
#define kParamRawDemosaicAHD "AHD", "AHD interpolation.", "ahd"
#define kParamRawDemosaicDCB "DCB", "DCB interpolation.", "dcb"
#define kParamRawDemosaicAHDMod "AHD-Mod", "Modified AHD interpolation by Paul Lee.", "ahdmod"
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 19, 0) && !defined(OFX_IO_LIBRAW_RUNTIME_DEMOSAIC_CAPS)
#define WARN_DEMOSAIC " Not available with this version of LibRaw (" LIBRAW_VERSION_STR "), AHD will be used instead."
#else
#define WARN_DEMOSAIC
#endif
#define kParamRawDemosaicAFD "AFD", "AFD interpolation (5-pass)." WARN_DEMOSAIC, "afd"
#define kParamRawDemosaicVCD "VCD", "VCD interpolation." WARN_DEMOSAIC, "vcd"
#define kParamRawDemosaicMixed "Mixed", "Mixed VCD/Modified AHD interpolation." WARN_DEMOSAIC, "mixed"
#define kParamRawDemosaicLMMSE "LMMSE", "LMMSE interpolation." WARN_DEMOSAIC, "lmmse"
#define kParamRawDemosaicAMaZE "AMaZE", "AMaZE interpolation." WARN_DEMOSAIC, "amaze"
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 16, 0)
#define kParamRawDemosaicDHT "DHT", "DHT interpolation.", "dht"
#define kParamRawDemosaicAAHD "AAHD", "Modified AHD interpolation by Anton Petrusevich.", "aahd"
#endif
enum RawDemosaicEnum {
    eRawDemosaicNone = 0,
    eRawDemosaicLinear,
    eRawDemosaicVNG,
    eRawDemosaicPPG,
    eRawDemosaicAHD,
    eRawDemosaicDCB,
    eRawDemosaicAHDMod,
    eRawDemosaicAFD,
    eRawDemosaicVCD,
    eRawDemosaicMixed,
    eRawDemosaicLMMSE,
    eRawDemosaicAMaZE,
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 16, 0)
    eRawDemosaicDHT,
    eRawDemosaicAAHD,
#endif
    eRawDemosaicCount
};

RawDemosaicEnum libraw_demosaic[eRawDemosaicCount];

// Whether the GPL2 (AHD-Mod, AFD, VCD, Mixed, LMMSE) and GPL3 (AMaZE) demosaic
// algorithms are available in the LibRaw build in use.
static void
getLibRawDemosaicCaps(bool* gpl2,
                      bool* gpl3)
{
#if defined(OFX_IO_LIBRAW_RUNTIME_DEMOSAIC_CAPS)
    unsigned caps = libraw_capabilities();
    *gpl2 = (caps & LIBRAW_CAPS_DEMOSAICSGPL2) != 0;
    *gpl3 = (caps & LIBRAW_CAPS_DEMOSAICSGPL3) != 0;
#elif LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 19, 0)
    *gpl2 = false;
    *gpl3 = false;
#else
    *gpl2 = true;
    *gpl3 = true;
#endif
}

// number of channels for hosts that don't support modifying choice menus (e.g. Nuke)
#define kDefaultChannelCount 16

// Channels 0 and 1 are reserved for 0 and 1 constants
#define kXChannelFirst 2

#define kParamChannelOutputLayerHint "This is the layer that will be set to the color plane. This is relevant only for image formats that can have multiple layers: " \
                                     "exr, tiff, psd, etc... Note that in Natron you can access other layers with a Shuffle node downstream of this node."

#define kParamChannelOutputLayer "outputLayer"
#define kParamChannelOutputLayerLabel "Output Layer"

// The string param behind the dynamic choice menu
#define kParamChannelOutputLayerChoice kParamChannelOutputLayer "Choice"

#define kParamAvailableViews "availableViews"
#define kParamAvailableViewsLabel "Available Views"
#define kParamAvailableViewsHint "Comma separated list of available views"

#define kParamOffsetNegativeDisplayWindow "offsetNegativeDispWindow"
#define kParamOffsetNegativeDisplayWindowLabel "Offset Negative Display Window"
#define kParamOffsetNegativeDisplayWindowHint "The EXR file format can have its \"display window\" origin at another location than (0,0). "                                               \
                                              "However in OpenFX, formats should have their origin at (0,0). If the left edge of the display window is not 0, either you can offset the " \
                                              "display window so it goes to 0, or you can treat the negative portion as overscan and resize the format."

#define kParamEdgePixels "edgePixels"
#define kParamEdgePixelsLabel "Edge Pixels", "Specifies how pixels in the border of the region of definition are handled"
#define kParamEdgePixelsAuto "Auto", "If the region of definition and format match exactly then repeat the border pixel otherwise use black", "auto"
#define kParamEdgePixelsEdgeDetect "Edge Detect", "For each edge, if the region of definition and format match exactly then repeat border pixel, otherwise use black", "edge"
#define kParamEdgePixelsRepeat "Repeat", "Repeat pixels outside the region of definition", "repeat"
#define kParamEdgePixelsBlack "Black", "Add black pixels outside the region of definition", "black"

enum EdgePixelsEnum {
    eEdgePixelsAuto,
    eEdgePixelsEdgeDetect,
    eEdgePixelsRepeat,
    eEdgePixelsBlack
};

#define kParamLibraryInfo "libraryInfo"
#define kParamLibraryInfoLabel "OpenImageIO Info...", "Display information about the underlying library."

template <typename T>
static inline void
unused(const T&)
{
}

static bool gHostSupportsDynamicChoices = false;
static bool gHostSupportsMultiPlane = false;

struct ReadOIIOTestAccess; // tests/oiio_layers_test.cpp

class ReadOIIOPlugin
    : public GenericReaderPlugin {
    // counts this instance: the first one starts OpenImageIO's thread pool, the last one stops it (OIIOGlobal.h).
    // (A member of this class, constructed before and destroyed after the bodies of its constructor and
    // destructor: the base class doesn't use the pools.)
    OIIOThreadsUser _oiioThreads;

    friend struct ReadOIIOTestAccess;

public:
    ReadOIIOPlugin(OfxImageEffectHandle handle,
                   const vector<string>& extensions);

    virtual ~ReadOIIOPlugin();

    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;
    virtual OfxStatus getClipComponents(const ClipComponentsArguments& args, ClipComponentsSetter& clipComponents) OVERRIDE FINAL;
    virtual void getClipPreferences(ClipPreferencesSetter& clipPreferences) OVERRIDE FINAL;

    /**
     * @brief Restore any state from the parameters set
     * Called from createInstance() and changedParam() (via changedFilename()), must restore the
     * state of the Reader, such as Choice param options, data members and non-persistent param values.
     * We don't do this in the ctor of the plug-in since we can't call virtuals yet.
     * Any derived implementation must call GenericReaderPlugin::restoreStateFromParams() first
     **/
    virtual void restoreStateFromParams() OVERRIDE FINAL;

private:
    virtual bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE FINAL;

    /**
     * @brief Called when the input image/video file changed.
     *
     * returns true if file exists and parameters successfully guessed, false in case of error.
     *
     * This function is only called once: when the filename is first set.
     *
     * Besides returning colorspace, premult, components, and componentcount, if it returns true
     * this function may also set extra format-specific parameters using Param::setValue.
     * The parameters must not be animated, since their value must remain the same for a whole sequence.
     *
     * You shouldn't do any strong processing as this is called on the main thread and
     * the getRegionOfDefinition() and  decode() should open the file in a separate thread.
     *
     * The colorspace may be set if available, else a default colorspace is used.
     *
     * You must also return the premultiplication state and pixel components of the image.
     * When reading an image sequence, this is called only for the first image when the user actually selects the new sequence.
     **/
    virtual bool guessParamsFromFilename(const string& filename, string* colorspace, PreMultiplicationEnum* filePremult, PixelComponentEnum* components, int* componentCount) OVERRIDE FINAL;
    virtual bool isVideoStream(const string& /*filename*/) OVERRIDE FINAL { return false; }

    virtual void decode(const string& filename,
                        OfxTime time,
                        int view,
                        const OfxRectI& renderWindow,
                        const OfxPointD& renderScale,
                        float* pixelData,
                        const OfxRectI& bounds,
                        PixelComponentEnum pixelComponents,
                        int pixelComponentCount,
                        int rowBytes) OVERRIDE FINAL
    {
        string rawComps;

        switch (pixelComponents) {
        case ePixelComponentAlpha:
            rawComps = kOfxImageComponentAlpha;
            break;
        case ePixelComponentRGB:
            rawComps = kOfxImageComponentRGB;
            break;
        case ePixelComponentRGBA:
            rawComps = kOfxImageComponentRGBA;
            break;
        default:
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
        decodePlane(filename, time, view, renderWindow, renderScale, pixelData, bounds, pixelComponents, pixelComponents, pixelComponentCount, rawComps, rowBytes);
    }

    virtual void decodePlane(const string& filename, OfxTime time, int view, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds,
                             PixelComponentEnum pixelComponents, PixelComponentEnum remappedComponents,
                             int pixelComponentCount, const string& rawComponents, int rowBytes) OVERRIDE FINAL;

    void getOIIOChannelIndexesFromLayerName(const string& filename, int view, const string& layerName, PixelComponentEnum pixelComponents, const vector<ImageSpec>& subimages, vector<int>& channels, int& numChannels, int& subImageIndex);

    // throws (with a persistent message) if the file cannot be opened: never returns null
    std::unique_ptr<ImageInput> openFile(const string& filename, vector<ImageSpec>* subimages);

    virtual bool getFrameBounds(const string& filename, OfxTime time, int view, OfxRectI* bounds, OfxRectI* format, double* par, string* error, int* tile_width, int* tile_height) OVERRIDE FINAL;

    string metadata(const string& filename);

    void getSpecsFromImageInput(ImageInput& img, vector<ImageSpec>* subimages) const;

    void getSpecs(const string& filename, vector<ImageSpec>* subimages, string* error = 0) const;

    void guessColorspace(const string& filename, const ImageSpec& imagespec, string* colorspace) const;

    /// This may warn the user if some views do not exist in the project

    static void getLayers(const vector<ImageSpec>& subimages, ViewsLayersMap* layersMap, LayersUnionVect* layersUnion);

    // builds the layers menu and updates _outputLayerMenu, to be called from restoreState
    void buildOutputLayerMenu(const vector<ImageSpec>& subimages);

    // retrieve the config used to open the file
    void getConfig(ImageSpec* config) const;

    BooleanParam* _rawAutoBright;
    BooleanParam* _rawUseCameraWB;
    DoubleParam* _rawAdjustMaximumThr;
    IntParam* _rawUserSat;
    ChoiceParam* _rawOutputColor;
    ChoiceParam* _rawUseCameraMatrix;
    ChoiceParam* _rawHighlightMode;
    IntParam* _rawHighlightRebuildLevel;
    DoubleParam* _rawExposure;
    ChoiceParam* _rawDemosaic;
    Double2DParam* _rawAber;

    /// V2 params
    ChoiceParam* _outputLayer;
    StringParam* _outputLayerString;
    StringParam* _availableViews;
    BooleanParam* _offsetNegativeDispWindow;
    ChoiceParam* _edgePixels;

    // _outputLayerMenuMutex guards _outputLayerMenu. Only buildOutputLayerMenu() writes it, on the main thread
    // (createInstance, instanceChanged), and holds the lock until _outputLayer's options and value match the new
    // menu. The main thread's other actions read it without the lock; the actions that may run on render threads
    // (render, isIdentity, getClipComponents) copy what they need under the lock. No other mutex of the plug-in is
    // taken, and no file is read, while it is held; host param calls are made under it (resetOptions, getValue,
    // setValue...; lock order: this mutex, then the host's), so the host must not wait for renders in them.
    // The lock removes the data race, not a render that started on one file using the menu of the next one: it then
    // fails with a message, and the host aborts it and renders again (ofxThreadSafety.rst).
    IOMutex _outputLayerMenuMutex;
    LayersUnionVect _outputLayerMenu;
};

ReadOIIOPlugin::ReadOIIOPlugin(OfxImageEffectHandle handle,
                               const vector<string>& extensions)
    : GenericReaderPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, kSupportsTiles,
#ifdef OFX_EXTENSIONS_NUKE
                          (getImageEffectHostDescription() && getImageEffectHostDescription()->isMultiPlanar) ? kIsMultiPlanar : false
#else
                          false
#endif
                          )
    , _outputLayer(NULL)
    , _outputLayerString(NULL)
    , _availableViews(NULL)
    , _offsetNegativeDispWindow(NULL)
    , _edgePixels(NULL)
    , _outputLayerMenuMutex()
    , _outputLayerMenu()
{
    if (gHostSupportsDynamicChoices && gHostSupportsMultiPlane) {
        _outputLayer = fetchChoiceParam(kParamChannelOutputLayer);
        _outputLayerString = fetchStringParam(kParamChannelOutputLayerChoice);
        _availableViews = fetchStringParam(kParamAvailableViews);
        assert(_outputLayer && _outputLayerString);
    }

    _rawAutoBright = fetchBooleanParam(kParamRawAutoBright);
    _rawUseCameraWB = fetchBooleanParam(kParamRawUseCameraWB);
    _rawAdjustMaximumThr = fetchDoubleParam(kParamRawAdjustMaximumThr);
    _rawUserSat = fetchIntParam(kParamRawUserSat);
    _rawOutputColor = fetchChoiceParam(kParamRawOutputColor);
    _rawUseCameraMatrix = fetchChoiceParam(kParamRawUseCameraMatrix);
    _rawHighlightMode = fetchChoiceParam(kParamRawHighlightMode);
    _rawHighlightRebuildLevel = fetchIntParam(kParamRawHighlightRebuildLevel);
    _rawExposure = fetchDoubleParam(kParamRawExposure);
    _rawDemosaic = fetchChoiceParam(kParamRawDemosaic);
    _rawAber = fetchDouble2DParam(kParamRawAber);
    _offsetNegativeDispWindow = fetchBooleanParam(kParamOffsetNegativeDisplayWindow);
    _edgePixels = fetchChoiceParam(kParamEdgePixels);

    // Don't try to restore any state in here, do so in restoreState instead which is called
    // right away after the constructor.
}

ReadOIIOPlugin::~ReadOIIOPlugin()
{
}

void
ReadOIIOPlugin::changedParam(const InstanceChangedArgs& args,
                             const string& paramName)
{
    if (paramName == kParamLibraryInfo) {
        sendMessage(Message::eMessageMessage, "", oiioLibraryInfo());
    } else if (paramName == kParamShowMetadata) {
        string filename;
        OfxStatus st = getFilenameAtTime(args.time, &filename);
        stringstream ss;
        if (st == kOfxStatOK) {
            ss << metadata(filename);
        } else if (filename.empty()) {
            ss << "Impossible to read image info:\nCould not get filename at time " << args.time << '.';
        } else {
            ss << "Impossible to read image info:\nCould not read file " << filename << " corresponding to time " << args.time << '.';
        }
        sendMessage(Message::eMessageMessage, "", ss.str());
    } else if (_outputLayerString && (paramName == kParamChannelOutputLayer)) {
        int index;
        _outputLayer->getValue(index);
        string optionName;
        _outputLayer->getOption(index, optionName);
        if (args.reason == eChangeUserEdit) {
            _outputLayerString->setValue(optionName);

            // only set the output components if this change comes from user interaction
            // (on the main thread, which is the only one that writes _outputLayerMenu: no lock)
            for (LayersUnionVect::iterator it = _outputLayerMenu.begin(); it != _outputLayerMenu.end(); ++it) {
                if (it->second.choiceOption == optionName) {
                    PixelComponentEnum comps;
                    switch (it->second.layer.channelNames.size()) {
                    case 1:
                        comps = ePixelComponentAlpha;
                        break;
                    case 3:
                        comps = ePixelComponentRGB;
                        break;
                    case 4:
                    default:
                        comps = ePixelComponentRGBA;
                        break;
                    }
                    setOutputComponents(comps);
                    break;
                }
            }
        }
    } else if ((paramName == kParamRawAutoBright) || (paramName == kParamRawUseCameraWB) || (paramName == kParamRawAdjustMaximumThr) ||
               (paramName == kParamRawUserSat) ||
               (paramName == kParamRawOutputColor) || (paramName == kParamRawUseCameraMatrix) || (paramName == kParamRawExposure) ||
               (paramName == kParamRawHighlightMode) || (paramName == kParamRawHighlightRebuildLevel) ||
               (paramName == kParamRawAber) ||
               (paramName == kParamRawDemosaic)) {
        // advanced (RAW) parameters: nothing to do here, they are read when the file is opened (getConfig())
    } else {
        GenericReaderPlugin::changedParam(args, paramName);
    }
}

void
ReadOIIOPlugin::getClipPreferences(ClipPreferencesSetter& clipPreferences)
{
    GenericReaderPlugin::getClipPreferences(clipPreferences);
}

OfxStatus
ReadOIIOPlugin::getClipComponents(const ClipComponentsArguments& args,
                                  ClipComponentsSetter& clipComponents)
{
    // (a copy: setClipPlanes() calls the host, without the lock)
    LayersUnionVect layers;
    {
        IOAutoMutex lock(_outputLayerMenuMutex);
        layers = _outputLayerMenu;
    }
    setClipPlanes(args, clipComponents, layers);

    return kOfxStatOK;
}

namespace {
/*static bool startsWith(const string& str,
                       const string& prefix)
   {
    return str.substr(0,prefix.size()) == prefix;
   }*/

static bool
endsWith(const string& str,
         const string& suffix)
{
    return ((str.size() >= suffix.size()) && (str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0));
}

} // anon namespace

// The layers of the subimages (see getImageLayers())
void
ReadOIIOPlugin::getLayers(const vector<ImageSpec>& subimages,
                          ViewsLayersMap* layersMap,
                          LayersUnionVect* layersUnion)
{
    vector<ImagePartChannels> parts(subimages.size());
    for (std::size_t i = 0; i < subimages.size(); ++i) {
        ImagePartChannels& part = parts[i];
        const ImageSpec& spec = subimages[i];
        for (int j = 0; j < spec.nchannels; ++j) {
            if (j >= (int)spec.channelnames.size()) {
                // give it a generic name since it's not in the channelnames
                stringstream ss;
                ss << "channel " << i;
                part.channelNames.push_back(ss.str());
            } else {
                part.channelNames.push_back(spec.channelnames[j]);
            }
        }
        if (subimages.size() == 1) {
            // Check the "multiView" property
            // We have to pass TypeDesc::UNKNOWN because if we pass TypeDesc::String OIIO will also check the type length which is encoded in the type
            // but we do not know it yet
            // See https://github.com/OpenImageIO/oiio/issues/1247
            const ParamValue* multiviewValue = spec.find_attribute("multiView", TypeDesc::UNKNOWN);
            if (multiviewValue) {
                /// This is the only way to retrieve the array size currently, see issue above
                int nValues = multiviewValue->type().arraylen;
                const ustring* dataPtr = (const ustring*)multiviewValue->data();
                for (int v = 0; v < nValues; ++v) {
                    part.multiView.push_back(string(dataPtr[v].data()));
                }
            }
        } else {
            // the "view" property of each subimage
            const ParamValue* viewValue = spec.find_attribute("view", TypeDesc::STRING);
            if (viewValue) {
                part.view = string(*(const char**)viewValue->data());
            }
        }
        // the "name" attribute of an OpenEXR 2 file (converted to oiio:subimagename by OIIO), the layer of its
        // channels that have none
        const ParamValue* nameValue = spec.find_attribute("oiio:subimagename", TypeDesc::STRING);
        if (nameValue) {
            part.name = string(*(const char**)nameValue->data());
        }
    }
    getImageLayers(parts, layersMap, layersUnion);
}

void
ReadOIIOPlugin::buildOutputLayerMenu(const vector<ImageSpec>& subimages)
{
    assert(gHostSupportsMultiPlane && gHostSupportsDynamicChoices);

    vector<string> options, optionsLabel;

    // Protect the map until we finished building the menu
    IOAutoMutex lock(_outputLayerMenuMutex);
    _outputLayerMenu.clear();

    ViewsLayersMap layersMap;

    getLayers(subimages, &layersMap, &_outputLayerMenu);

    string viewsEncoded;
    for (std::size_t i = 0; i < layersMap.size(); ++i) {
        viewsEncoded.append(layersMap[i].first);
        if (i < layersMap.size() - 1) {
            viewsEncoded.push_back(',');
        }
    }

    _availableViews->setValue(viewsEncoded);

    /// Now build the choice options
    for (std::size_t i = 0; i < _outputLayerMenu.size(); ++i) {
        const string& layerName = _outputLayerMenu[i].first;
        string choice;
        if (layerName == kReaderColorLayer) {
            switch (_outputLayerMenu[i].second.layer.channelNames.size()) {
            case 1:
                choice = kReaderColorLayer ".Alpha";
                break;
            default: {
                choice.append(kReaderColorLayer ".");
                for (std::size_t j = 0; j < _outputLayerMenu[i].second.layer.channelNames.size(); ++j) {
                    choice.append(_outputLayerMenu[i].second.layer.channelNames[j]);
                }
                break;
            }
            }
        } else if ((_outputLayerMenu[i].second.layer.channelNames.size() == 1) && (layerName == _outputLayerMenu[i].second.layer.channelNames[0])) {
            // Depth.Depth for instance
            for (std::size_t j = 0; j < _outputLayerMenu[i].second.layer.channelNames.size(); ++j) {
                choice.append(_outputLayerMenu[i].second.layer.channelNames[j]);
            }
        } else {
            choice.append(layerName);
            choice.push_back('.');
            for (std::size_t j = 0; j < _outputLayerMenu[i].second.layer.channelNames.size(); ++j) {
                choice.append(_outputLayerMenu[i].second.layer.channelNames[j]);
            }
        }

        string optionLabel;
        if (layersMap.size() > 1) {
            stringstream ss;
            ss << "Present in views: ";
            for (std::size_t j = 0; j < _outputLayerMenu[i].second.views.size(); ++j) {
                ss << _outputLayerMenu[i].second.views[j];
                if (j < _outputLayerMenu[i].second.views.size() - 1) {
                    ss << ", ";
                }
            }
            optionLabel = ss.str();
        }
        options.push_back(choice);
        optionsLabel.push_back(optionLabel);
        _outputLayerMenu[i].second.choiceOption = choice;
    }

    assert(options.size() == _outputLayerMenu.size());

    /// Actually build the menu
    _outputLayer->resetOptions(options, optionsLabel);

    /// synchronize with the value stored in the string param
    string valueStr;
    _outputLayerString->getValue(valueStr);
    if (valueStr.empty()) {
        int cur_i;
        _outputLayer->getValue(cur_i);
        if ((cur_i >= 0) && (cur_i < (int)options.size())) {
            valueStr = options[cur_i];
        } else if (!options.empty()) {
            // No choice but to change the choice value
            valueStr = options[0];
            _outputLayer->setValue(0);
        }
        _outputLayerString->setValue(valueStr);
    } else {
        int foundOption = -1;
        for (int i = 0; i < (int)options.size(); ++i) {
            if (options[i] == valueStr) {
                foundOption = i;
                break;
            }
        }
        if (foundOption != -1) {
            _outputLayer->setValue(foundOption);
        } else {
            _outputLayer->setValue(0);
            _outputLayerString->setValue(options[0]);
        }
    }
} // buildOutputLayerMenu

void
ReadOIIOPlugin::getSpecsFromImageInput(ImageInput& img,
                                       vector<ImageSpec>* subimages) const
{
    subimages->clear();
    int subImageIndex = 0;
    ImageSpec spec;
    while (img.seek_subimage(subImageIndex, 0, spec)) {
        subimages->push_back(spec);
        ++subImageIndex;
#ifndef OFX_READ_OIIO_SUPPORTS_SUBIMAGES
        break;
#endif
    }
    // clear the error of the failed seek past the last subimage, which is expected: else it would be the message of
    // the next read error (#242)
    (void)img.geterror();
}

void
ReadOIIOPlugin::getSpecs(const string& filename,
                         vector<ImageSpec>* subimages,
                         string* error) const
{
    subimages->clear();
    {
        // use the right config
        ImageSpec config;
        getConfig(&config);

        std::unique_ptr<ImageInput> img = ImageInput::open(filename, &config);
        if (!img.get()) {
            if (error) {
                *error = "Could node open file " + filename;
            }

            return;
        }
        getSpecsFromImageInput(*img, subimages);
        img->close();
    }
    if (subimages->empty()) {
        if (error) {
            *error = "Could node open file " + filename;
        }

        return;
    }

    // check that no subimage is deep
    for (std::size_t i = 0; i < subimages->size(); ++i) {
        if ((*subimages)[i].deep) {
            if (error) {
                *error = "Cannot read deep image file " + filename;
            }
            subimages->clear();

            return;
        }
    }
}

/**
 * @brief Restore any state from the parameters set
 * Called from createInstance() and changedParam() (via changedFilename()), must restore the
 * state of the Reader, such as Choice param options, data members and non-persistent param values.
 * We don't do this in the ctor of the plug-in since we can't call virtuals yet.
 * Any derived implementation must call GenericReaderPlugin::restoreStateFromParams() first
 **/
void
ReadOIIOPlugin::restoreStateFromParams()
{
    GenericReaderPlugin::restoreStateFromParams();

    /// http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#SettingParams
    /// The Create instance action is in the list of actions where you can set param values

    string filename;
    _fileParam->getValueAtTime(_firstFrame->getValue(), filename); // the time in _fileParam is the *file* time
    if (filename.empty()) {
        return;
    }
    vector<ImageSpec> subimages;
    getSpecs(filename, &subimages);

    if (subimages.empty()) {
        return;
    }

    // (the layer params only exist if the host supports dynamic choices and multi-plane effects)
    if (gHostSupportsDynamicChoices && gHostSupportsMultiPlane) {
        buildOutputLayerMenu(subimages);
    }

    // Only support tiles if tile size is set
    const ImageSpec& spec = subimages[0];
    int width = /*spec.width == 0 ? spec.full_width :*/ spec.width;
    int height = /*spec.height == 0 ? spec.full_height :*/ spec.height;
    setSupportsTiles(spec.tile_width != 0 && spec.tile_width != width && spec.tile_height != 0 && spec.tile_height != height);

    // Show these parameters only for exr
    string ext;
    {
        std::locale l;
        std::size_t foundDot = filename.find_last_of(".");
        if (foundDot != string::npos) {
            ext = filename.substr(foundDot + 1);
        }
        for (std::size_t i = 0; i < ext.size(); ++i) {
            ext[i] = std::tolower(ext[i], l);
        }
    }
    bool supportsDisplayWindow = ext == "exr";
    _edgePixels->setIsSecretAndDisabled(!supportsDisplayWindow);
    _offsetNegativeDispWindow->setIsSecretAndDisabled(!supportsDisplayWindow);
}

// The color space of a file that has none (or an unknown one), from its bit depth:
// sRGB for 8-bit images, Rec709 for 10-bits, 12-bits or 16-bits integer images (KodakLog for Cineon and DPX),
// Linear for anything else
static const char*
colorSpaceFromBitDepth(const string& filename,
                       const ImageSpec& imagespec)
{
    switch (imagespec.format.basetype) {
    case TypeDesc::UCHAR:
    case TypeDesc::CHAR:
        return "sRGB";
    case TypeDesc::USHORT:
    case TypeDesc::SHORT:
        if (endsWith(filename, ".cin") || endsWith(filename, ".dpx") || endsWith(filename, ".CIN") || endsWith(filename, ".DPX")) {
            // Cineon or DPX file
            return "KodakLog";
        }
        return "Rec709";
    default:
        return "Linear";
    }
}

void
ReadOIIOPlugin::guessColorspace(const string& filename,
                                const ImageSpec& imagespec,
                                string* colorspace) const
{
    /// find-out the image color-space
    const ParamValue* colorSpaceValue = imagespec.find_attribute("oiio:ColorSpace", TypeDesc::STRING);
    const ParamValue* photoshopICCProfileValue = imagespec.find_attribute("photoshop:ICCProfile", TypeDesc::STRING);

    // photoshop:ICCProfile: "HDTV (Rec. 709)"

#ifdef OFX_IO_USING_OCIO
    // make sure the OCIO config is const
    GenericOCIO const* ocio = _ocio.get();
#endif

    // we found a color-space hint, use it to do the color-space conversion
    const char* colorSpaceStr = NULL;
    float gamma = imagespec.get_float_attribute("oiio:Gamma");
    if (colorSpaceValue) {
        colorSpaceStr = *(const char**)colorSpaceValue->data();
        // OpenImageIO 3 names the color spaces with the color interop IDs (e.g. srgb_rec709_scene), OpenImageIO 2
        // with sRGB, Linear, GammaCorrected (and oiio:Gamma), AdobeRGB...: map the usual ones to the latter, which
        // are mapped to the color spaces of the usual OCIO configs below, unless the config has them
        bool hasIt = false;
#ifdef OFX_IO_USING_OCIO
        hasIt = colorSpaceStr && ocio->hasColorspace(colorSpaceStr);
#endif
        if (colorSpaceStr && !hasIt) {
            const string cs = colorSpaceStr;
            if (cs == "srgb_rec709_scene" || cs == "srgb_rec709_display" || cs == "srgb" || cs == "srgb_texture" || cs == "srgb_tx") {
                colorSpaceStr = "sRGB";
            } else if (cs == "lin_rec709_scene" || cs == "lin_rec709" || cs == "lin_srgb" || cs == "linear") {
                colorSpaceStr = "Linear";
            } else if (cs == "g22_adobergb_scene" || cs == "g22_adobergb_display") {
                colorSpaceStr = "AdobeRGB";
            } else if ((cs.size() >= 10) && (cs[0] == 'g') && std::isdigit((unsigned char)cs[1]) && std::isdigit((unsigned char)cs[2])
                       && (cs.compare(3, 7, "_rec709") == 0) && ((cs.size() == 10) || (cs[10] == '_'))) {
                // g<NN>_rec709[_scene|_display]: gamma NN/10 (g18, g22, g24...)
                colorSpaceStr = "GammaCorrected";
                gamma = ((cs[1] - '0') * 10 + (cs[2] - '0')) / 10.f;
            } else if ((cs != "sRGB") && (cs != "Linear") && (cs != "GammaCorrected") && (cs != "AdobeRGB")
                       && (cs != "Rec709") && (cs != "KodakLog")) {
                // another name that is not in the config (e.g. lin_ap1_scene, lin_rec2020_scene, srgb_p3d65_display,
                // pq_rec2020_display): guess from the bit depth below, as for a file without a color space
                colorSpaceStr = NULL;
            }
        }
    } else if (photoshopICCProfileValue) {
        const char* ICCProfileStr = *(const char**)photoshopICCProfileValue->data();
        if (!strcmp(ICCProfileStr, "HDTV (Rec. 709)") || !strcmp(ICCProfileStr, "SDTV NTSC") || !strcmp(ICCProfileStr, "SDTV PAL") || !strcmp(ICCProfileStr, "HDTV (Rec. 709) 16-235") || !strcmp(ICCProfileStr, "SDTV NTSC 16-235") || !strcmp(ICCProfileStr, "SDTV PAL 16-235") || !strcmp(ICCProfileStr, "SDTV NTSC 16-235")) {
            colorSpaceStr = "Rec709";
        } else if (!strcmp(ICCProfileStr, "sRGB IEC61966-2.1")) {
            colorSpaceStr = "sRGB";
        } else if (!strcmp(ICCProfileStr, "Universal Camera Film Printing Density)")) {
            colorSpaceStr = "KodakLog";
        }
    }
    if (!colorSpaceStr) {
        // no colorspace... we'll probably have to try something else, then.
        // we set the following defaults:
        // sRGB for 8-bit images
        // Rec709 for 10-bits, 12-bits or 16-bits integer images
        // Linear for anything else
        colorSpaceStr = colorSpaceFromBitDepth(filename, imagespec);
    }
    if (colorSpaceStr) {
        if (colorSpaceStr && !strcmp(colorSpaceStr, "GammaCorrected")) {
            if (endsWith(filename, ".cin") || endsWith(filename, ".CIN")) {
                // Cineon files (e.g. Kodak Digital LAD, see link below) get wrongly attributed
                // a GammaCorrected colorspace <https://github.com/OpenImageIO/oiio/issues/1463>
                // The standard Kodak DLAD images get gamma=0 for example:
                // http://motion.kodak.com/motion/support/technical_information/lab_tools_and_techniques/digital_lad/default.htm
                // Digital_LAD_cin/Digital_LAD_2048x1556.cin gets oiio:Gamma: 0
                // Nuke_BasicWorkflows_Media/BasicWorkflows_ColourManagement/COLOR MANAGEMENT/Source Pics/clouds.cin gets oiio:Gamma: 1
                // Nuke_BasicWorkflows_Media/BasicWorkflows_ColourManagement/COLOR MANAGEMENT/Source Pics/greenscreen_boy.cin gets oiio:Gamma: 4.6006e-41
                // all these files are in reality log-encoded.
                colorSpaceStr = "KodakLog";
            } else if (std::fabs(gamma - 1.8) < 0.01) {
#ifdef OFX_IO_USING_OCIO
                if (ocio->hasColorspace("Gamma1.8")) {
                    // nuke-default
                    *colorspace = "Gamma1.8";
                    colorSpaceStr = NULL;
                }
#endif
            } else if (std::fabs(gamma - 2.2) < 0.01) {
#ifdef OFX_IO_USING_OCIO
                if (ocio->hasColorspace("Gamma2.2")) {
                    // nuke-default
                    *colorspace = "Gamma2.2";
                    colorSpaceStr = NULL;
                } else if (ocio->hasColorspace("VD16")) {
                    // VD16 in blender
                    *colorspace = "VD16";
                    colorSpaceStr = NULL;
                } else if (ocio->hasColorspace("vd16")) {
                    // vd16 in spi-anim and spi-vfx
                    *colorspace = "vd16";
                    colorSpaceStr = NULL;
                } else
#endif
                {
                    colorSpaceStr = "sRGB";
                }
            }
            if (colorSpaceStr && !strcmp(colorSpaceStr, "GammaCorrected")) {
                // another gamma (e.g. 2.4), or no Gamma1.8 color space in the config: guess from the bit depth
                colorSpaceStr = colorSpaceFromBitDepth(filename, imagespec);
            }
        }
#ifdef OFX_IO_USING_OCIO
        if (colorSpaceStr && !strcmp(colorSpaceStr, "sRGB")) {
            if (ocio->hasColorspace("sRGB")) {
                // nuke-default, blender, natron
                *colorspace = "sRGB";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("sRGB D65")) {
                // blender-cycles
                *colorspace = "sRGB D65";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("sRGB (D60 sim.)")) {
                // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
                *colorspace = "sRGB (D60 sim.)";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("out_srgbd60sim")) {
                // out_srgbd60sim or "sRGB (D60 sim.)" in aces 1.0.0
                *colorspace = "out_srgbd60sim";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("rrt_Gamma2.2")) {
                // rrt_Gamma2.2 in aces 0.7.1
                *colorspace = "rrt_Gamma2.2";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("rrt_srgb")) {
                // rrt_srgb in aces 0.1.1
                *colorspace = "rrt_srgb";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("srgb8")) {
                // srgb8 in spi-vfx
                *colorspace = "srgb8";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("Gamma2.2")) {
                // nuke-default
                *colorspace = "Gamma2.2";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("srgb8")) {
                // srgb8 in spi-vfx
                *colorspace = "srgb8";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("vd16")) {
                // vd16 in spi-anim
                *colorspace = "vd16";
                colorSpaceStr = NULL;
            }
        }
        if (colorSpaceStr && !strcmp(colorSpaceStr, "AdobeRGB")) {
            if (ocio->hasColorspace("AdobeRGB")) {
                // natron
                *colorspace = "AdobeRGB";
                colorSpaceStr = NULL;
            }
        }
        if (colorSpaceStr && !strcmp(colorSpaceStr, "Rec709")) {
            if (ocio->hasColorspace("Rec709")) {
                // nuke-default
                *colorspace = "Rec709";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("nuke_rec709")) {
                // blender
                *colorspace = "nuke_rec709";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("Rec 709 Curve")) {
                // natron
                *colorspace = "Rec 709 Curve";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("Rec.709 - Full")) {
                // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                *colorspace = "Rec.709 - Full";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("out_rec709full")) {
                // out_rec709full or "Rec.709 - Full" in aces 1.0.0
                *colorspace = "out_rec709full";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("rrt_rec709_full_100nits")) {
                // rrt_rec709_full_100nits in aces 0.7.1
                *colorspace = "rrt_rec709_full_100nits";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("rrt_rec709")) {
                // rrt_rec709 in aces 0.1.1
                *colorspace = "rrt_rec709";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("hd10")) {
                // hd10 in spi-anim and spi-vfx
                *colorspace = "hd10";
                colorSpaceStr = NULL;
            }
        }
        if (colorSpaceStr && !strcmp(colorSpaceStr, "KodakLog")) {
            if (ocio->hasColorspace("Cineon")) {
                // Cineon in nuke-default
                *colorspace = "Cineon";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("Cineon Log Curve")) {
                // Curves/Cineon Log Curve in natron
                *colorspace = "Cineon Log Curve";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("REDlogFilm")) {
                // REDlogFilm in aces 1.0.0
                *colorspace = "REDlogFilm";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("cineon")) {
                // cineon in aces 0.7.1
                *colorspace = "cineon";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("adx10")) {
                // adx10 in aces 0.1.1
                *colorspace = "adx10";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("lg10")) {
                // lg10 in spi-vfx
                *colorspace = "lg10";
                colorSpaceStr = NULL;
            } else if (ocio->hasColorspace("lm10")) {
                // lm10 in spi-anim
                *colorspace = "lm10";
                colorSpaceStr = NULL;
            } else {
                *colorspace = OCIO::ROLE_COMPOSITING_LOG;
                colorSpaceStr = NULL;
            }
        }
        if (colorSpaceStr && !strcmp(colorSpaceStr, "Linear")) {
            *colorspace = OCIO::ROLE_SCENE_LINEAR;
            colorSpaceStr = NULL;
            // lnf in spi-vfx
        }
        if (colorSpaceStr && ocio->hasColorspace(colorSpaceStr)) {
            // maybe we're lucky
            *colorspace = colorSpaceStr;
            colorSpaceStr = NULL;
        }
        if (colorSpaceStr) {
            // unknown color-space: keep the default one (a color space that is not in the config would make
            // rendering fail)
            colorSpaceStr = NULL;
        }
#endif // OFX_IO_USING_OCIO
    }
    if (colorSpaceStr) {
        *colorspace = colorSpaceStr;
    }
} // ReadOIIOPlugin::guessColorspace

/**
 * @brief Called when the input image/video file changed.
 *
 * returns true if file exists and parameters successfully guessed, false in case of error.
 *
 * This function is only called once: when the filename is first set.
 *
 * Besides returning colorspace, premult, components, and componentcount, if it returns true
 * this function may also set extra format-specific parameters using Param::setValue.
 * The parameters must not be animated, since their value must remain the same for a whole sequence.
 *
 * You shouldn't do any strong processing as this is called on the main thread and
 * the getRegionOfDefinition() and  decode() should open the file in a separate thread.
 *
 * The colorspace may be set if available, else a default colorspace is used.
 *
 * You must also return the premultiplication state and pixel components of the image.
 * When reading an image sequence, this is called only for the first image when the user actually selects the new sequence.
 **/
bool
ReadOIIOPlugin::guessParamsFromFilename(const string& filename,
                                        string* colorspace,
                                        PreMultiplicationEnum* filePremult,
                                        PixelComponentEnum* components,
                                        int* componentCount)
{
    string error;

    vector<ImageSpec> subimages;
    getSpecs(filename, &subimages, &error);

    if (subimages.empty()) {
        // setPersistentMessage(Message::eMessageError, "", error);

        return false;
    }

    guessColorspace(filename, subimages[0], colorspace);

    ViewsLayersMap layersMap;
    LayersUnionVect layersUnion;
    getLayers(subimages, &layersMap, &layersUnion);

    if (layersUnion.empty()) {
        *components = ePixelComponentNone;
    } else {
        const vector<string>& channels = layersUnion[0].second.layer.channelNames;
        switch (channels.size()) {
        case 0:
            *components = ePixelComponentNone;
            *componentCount = 0;
            break;
        case 1:
            *components = ePixelComponentAlpha;
            *componentCount = 1;
            break;
        case 3:
            *components = ePixelComponentRGB;
            *componentCount = 3;
            break;
        case 4:
            *components = ePixelComponentRGBA;
            *componentCount = 4;
            break;
        case 2: {
            // in OIIO, PNG with alpha are stored with as a 2-channel image
            bool hasI = false;
            bool hasA = false;
            for (std::size_t i = 0; i < channels.size(); ++i) {
                // luminance may be I or Y
                if ((channels[i] == "I") || (channels[i] == "i") || (channels[i] == "Y") || (channels[i] == "y")) {
                    hasI = true;
                }
                if ((channels[i] == "A") || (channels[i] == "a")) {
                    hasA = true;
                }
            }
            if (hasI && hasA) {
                *components = ePixelComponentRGBA;
                *componentCount = 4;
            } else {
                *components = ePixelComponentXY;
                *componentCount = 2;
            }
            break;
        }
        default:
            *components = ePixelComponentRGBA;
            *componentCount = 4;
            break;
        }
        //*componentCount = subimages[0].nchannels;
    }

    if ((*components != ePixelComponentRGBA) && (*components != ePixelComponentAlpha)) {
        *filePremult = eImageOpaque;
    } else {
        bool unassociatedAlpha = subimages[0].get_int_attribute("oiio:UnassociatedAlpha", 0);
        if (unassociatedAlpha) {
            *filePremult = eImageUnPreMultiplied;
        } else {
            *filePremult = eImagePreMultiplied;
        }
    }

    return true;
} // ReadOIIOPlugin::guessParamsFromFilename

void
ReadOIIOPlugin::getConfig(ImageSpec* config) const
{
    // Always keep unassociated alpha.
    // Don't let OIIO premultiply, because if the image is 8bits,
    // it multiplies in 8bits (see TIFFInput::unassalpha_to_assocalpha()),
    // which causes a lot of precision loss.
    // see also https://github.com/OpenImageIO/oiio/issues/960
    config->attribute("oiio:UnassociatedAlpha", 1);

    if (_rawAutoBright->getValue()) {
        config->attribute("raw:auto_bright", 1);
    }

    if (!_rawUseCameraWB->getValue()) {
        config->attribute("raw:use_camera_wb", 0);
    }

    config->attribute("raw:adjust_maximum_thr", (float)_rawAdjustMaximumThr->getValue());

    // Add "raw:user_sat" configuration attribute to the reader. (OpenImageIO #1666)
    // Set camera maximum value if "raw:user_sat" is not 0
    // user_sat is used to set sensor saturation (max possible) value.
    int rawUserSat = _rawUserSat->getValue();
    if (rawUserSat > 0) {
        config->attribute("raw:user_sat", rawUserSat);
    }

    // Check to see if the user has explicitly set the output colorspace primaries
    const char* cs = NULL;
    RawOutputColorEnum rawOutputColor = (RawOutputColorEnum)_rawOutputColor->getValue();
    switch (rawOutputColor) {
    case eRawOutputColorRaw:
        cs = "raw";
        break;
    case eRawOutputColorSRGB:
    default:
        cs = "sRGB";
        break;
    case eRawOutputColorAdobe:
        cs = "Adobe";
        break;
    case eRawOutputColorWide:
        cs = "Wide";
        break;
    case eRawOutputColorProPhoto:
        cs = "ProPhoto";
        break;
    case eRawOutputColorXYZ:
        cs = "XYZ";
        break;
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 18, 0)
    case eRawOutputColorACES:
        cs = "ACES";
        break;
#endif
    }
    if (cs != NULL) {
        config->attribute("raw:ColorSpace", cs);
    }

    // Use embedded color profile. Values mean:
    // 0: do not use embedded color profile
    // 1 (default): use embedded color profile (if present) for DNG files
    //    (always), for other files only if use_camera_wb is set.
    // 3: use embedded color data (if present) regardless of white
    //    balance setting.
    RawUseCameraMatrixEnum rawUseCameraMatrix = (RawUseCameraMatrixEnum)_rawUseCameraMatrix->getValue();
    switch (rawUseCameraMatrix) {
    case eRawUseCameraMatrixNone:
        config->attribute("raw:use_camera_matrix", 0);
        break;
    case eRawUseCameraMatrixDefault:
        config->attribute("raw:use_camera_matrix", 1);
        break;
    case eRawUseCameraMatrixForce:
        config->attribute("raw:use_camera_matrix", 3);
        break;
    }

    // Exposure adjustment
    double rawExposure = _rawExposure->getValue();
    if (rawExposure != 1.) {
        config->attribute("raw:Exposure", (float)rawExposure);
    }

    // Chromatic Aberration
    // see:
    // https://github.com/NatronGitHub/Natron/issues/309
    // https://github.com/OpenImageIO/oiio/issues/2029
    // https://github.com/OpenImageIO/oiio/pull/2030
    // Correction of chromatic aberrations; the only specified values are
    // - the red multiplier
    // - the blue multiplier.
    OfxPointD rawAber = _rawAber->getValue();
    if (rawAber.x != 1. || rawAber.y != 1.) {
        float floats[2] = { (float)rawAber.x, (float)rawAber.y };
        config->attribute("raw:aber", TypeDesc(TypeDesc::FLOAT, 2), &floats[0]);
    }

    // Highlight adjustment
    // (0=clip, 1=unclip, 2=blend, 3+=rebuild)
    // LibRaw offers several algorithms for restoring highlight clippings — Solid White, Unclip, Blend, and Rebuild
    // Default is here to consider highlights (read: part of your images that are burned due to the inability of your camera to capture the highlights) as plain / solid white (solid white option). You can get some fancy results with the unclip option which will paint the highlights in various pinks. At last you can try to consider recovering some parts of the missing information from the highlights (reconstruct option).

    // This is possible because the blue pixels tends to saturate less quickly than the greens and the reds. digiKam/dcraw will try to reconstruct the missing green and red colors from the remaining none saturated blue pixels. Of course here everything is a question of tradeoff between how much color or white you want.

    // If you select Reconstruct as the option, you will be given the choice to set a level. A value of 3 is a compromise and can/should be adapted on a per image basis.
    RawHighlightModeEnum rawHighlightMode = (RawHighlightModeEnum)_rawHighlightMode->getValue();
    if (rawHighlightMode != eRawHighlightModeClip) {
        if (rawHighlightMode != eRawHighlightModeRebuild) {
            config->attribute("raw:HighlightMode", (int)rawHighlightMode);
        } else {
            // rebuild level, from 0 to 7
            int rawHighlightRebuildLevel = (std::max)(0, (std::min)(_rawHighlightRebuildLevel->getValue(), 7));
            config->attribute("raw:HighlightMode", (int)rawHighlightMode + rawHighlightRebuildLevel);
        }
    }

    // Interpolation quality
    // note: LibRaw must be compiled with demosaic pack GPL2 to use demosaic
    // algorithms 5-9. It must be compiled with demosaic pack GPL3 for
    // algorithm 10 (AMAzE). These packs exist for LibRaw <= 0.18, or for a
    // newer LibRaw patched to restore them (LIBRAW_HAS_DEMOSAIC_GPL_CAPS).
    // The menu only lists algorithms reported as available by
    // getLibRawDemosaicCaps(); if LibRaw is asked for one it lacks anyway, it
    // will silently use option 3 - AHD.
    int rawDemosaic = _rawDemosaic->getValue();
    const char* d = NULL;
    // out-of-range values (e.g. from a project saved with another LibRaw build) use AHD
    RawDemosaicEnum demosaic = (rawDemosaic >= 0 && rawDemosaic < eRawDemosaicCount) ? libraw_demosaic[rawDemosaic] : eRawDemosaicAHD;
    switch (demosaic) {
    case eRawDemosaicNone:
        d = "none";
        break;
    case eRawDemosaicLinear:
        d = "linear";
        break;
    case eRawDemosaicVNG:
        d = "VNG";
        break;
    case eRawDemosaicPPG:
        d = "PPG";
        break;
    case eRawDemosaicAHD:
    default:
        d = "AHD";
        break;
    case eRawDemosaicDCB:
        d = "DCB";
        break;
    case eRawDemosaicAHDMod:
        d = "AHD-Mod";
        break;
    case eRawDemosaicAFD:
        d = "AFD";
        break;
    case eRawDemosaicVCD:
        d = "VCD";
        break;
    case eRawDemosaicMixed:
        d = "Mixed";
        break;
    case eRawDemosaicLMMSE:
        d = "LMMSE";
        break;
    case eRawDemosaicAMaZE:
        d = "AMaZE";
        break;
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 16, 0)
    case eRawDemosaicDHT:
        d = "DHT";
        break;
    case eRawDemosaicAAHD:
        d = "AAHD";
        break;
#endif
    }
    if (d != NULL) {
        config->attribute("raw:Demosaic", d);
    }
}

std::unique_ptr<ImageInput>
ReadOIIOPlugin::openFile(const string& filename,
                         vector<ImageSpec>* subimages)
{
    // use the right config
    ImageSpec config;
    getConfig(&config);

    std::unique_ptr<ImageInput> img = ImageInput::open(filename, &config);
    if (!img) {
        // (OIIO::geterror() also clears OpenImageIO's pending error)
        const string error = OIIO::geterror();
        setPersistentMessage(Message::eMessageError, "", string("Cannot open file ") + filename + (error.empty() ? "" : ": " + error));
        throwSuiteStatusException(kOfxStatFailed);
    }
    getSpecsFromImageInput(*img, subimages);

    return img;
}

void
ReadOIIOPlugin::getOIIOChannelIndexesFromLayerName(const string& filename,
                                                   int view,
                                                   const string& layerName,
                                                   PixelComponentEnum pixelComponents,
                                                   const vector<ImageSpec>& subimages,
                                                   vector<int>& channels,
                                                   int& numChannels,
                                                   int& subImageIndex)
{
    ViewsLayersMap layersMap;

    getLayers(subimages, &layersMap, 0);

    /// Find the view
    string viewName = getViewName(view);
    ViewsLayersMap::const_iterator foundView = findImageView(layersMap, viewName, view);

    int foundLayer = -1;
    for (std::size_t i = 0; i < foundView->second.size(); ++i) {
        if (foundView->second[i].first == layerName) {
            foundLayer = (int)i;
            break;
        }
    }
    if (foundLayer == -1) {
        stringstream ss;
        ss << "Could not find layer " << layerName << " in view " << viewName << " in " << filename;
        setPersistentMessage(Message::eMessageError, "", ss.str());
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    const vector<int>& layerChannels = foundView->second[foundLayer].second.channelIndexes;
    subImageIndex = foundView->second[foundLayer].second.subImageIdx;

    // Some pngs are 2-channel intensity + alpha
    bool isIA = (layerChannels.size() == 2 && (foundView->second[foundLayer].second.channelNames[0] == "I" || foundView->second[foundLayer].second.channelNames[0] == "Y") && foundView->second[foundLayer].second.channelNames[1] == "A");

    switch (pixelComponents) {
    case ePixelComponentRGBA:
        numChannels = 4;
        channels.resize(numChannels);
        if (isIA) {
            channels[0] = layerChannels[0] + kXChannelFirst;
            channels[1] = layerChannels[0] + kXChannelFirst;
            channels[2] = layerChannels[0] + kXChannelFirst;
            channels[3] = layerChannels[1] + kXChannelFirst;
        } else {
            if (layerChannels.size() == 1) {
                channels[0] = layerChannels[0] + kXChannelFirst;
                channels[1] = layerChannels[0] + kXChannelFirst;
                channels[2] = layerChannels[0] + kXChannelFirst;
                channels[3] = 1;
            } else if (layerChannels.size() == 2) {
                channels[0] = layerChannels[0] + kXChannelFirst;
                channels[1] = layerChannels[1] + kXChannelFirst;
                channels[2] = 0;
                channels[3] = 1;
            } else if (layerChannels.size() == 3) {
                channels[0] = layerChannels[0] + kXChannelFirst;
                channels[1] = layerChannels[1] + kXChannelFirst;
                channels[2] = layerChannels[2] + kXChannelFirst;
                channels[3] = 1;
            } else {
                channels[0] = layerChannels[0] + kXChannelFirst;
                channels[1] = layerChannels[1] + kXChannelFirst;
                channels[2] = layerChannels[2] + kXChannelFirst;
                channels[3] = layerChannels[3] + kXChannelFirst;
            }
        }
        break;
    case ePixelComponentRGB:
        numChannels = 3;
        channels.resize(numChannels);
        if (layerChannels.size() == 1) {
            channels[0] = layerChannels[0] + kXChannelFirst;
            channels[1] = layerChannels[0] + kXChannelFirst;
            channels[2] = layerChannels[0] + kXChannelFirst;
        } else if (layerChannels.size() == 2) {
            channels[0] = layerChannels[0] + kXChannelFirst;
            channels[1] = layerChannels[1] + kXChannelFirst;
            channels[2] = 0;
        } else if (layerChannels.size() >= 3) {
            channels[0] = layerChannels[0] + kXChannelFirst;
            channels[1] = layerChannels[1] + kXChannelFirst;
            channels[2] = layerChannels[2] + kXChannelFirst;
        }
        break;
    case ePixelComponentXY: {
        numChannels = 2;
        channels.resize(numChannels);
        channels[0] = layerChannels[0] + kXChannelFirst;
        if (layerChannels.size() == 1) {
            channels[1] = layerChannels[0] + kXChannelFirst;
        } else if ((layerChannels.size() == 2) || (layerChannels.size() == 3)) {
            channels[1] = layerChannels[1] + kXChannelFirst;
        } else {
            channels[1] = layerChannels[3] + kXChannelFirst;
        }
        break;
    }
    case ePixelComponentAlpha:
        numChannels = 1;
        channels.resize(numChannels);
        if (layerChannels.size() == 1) {
            channels[0] = layerChannels[0] + kXChannelFirst;
        } else if (layerChannels.size() == 2) {
            if (isIA) {
                channels[0] = layerChannels[1] + kXChannelFirst;
            } else {
                channels[0] = layerChannels[0] + kXChannelFirst;
            }
        } else if (layerChannels.size() == 3) {
            channels[0] = 1;
        } else if (layerChannels.size() == 4) {
            channels[0] = layerChannels[3] + kXChannelFirst;
        }
        break;
    case ePixelComponentCustom:
        // numChannels has been already set
        assert(numChannels != 0);
        channels.resize(numChannels);
        for (int i = 0; i < numChannels; ++i) {
            int defIndex = i == 3 ? 1 : 0;
            channels[i] = i < (int)layerChannels.size() ? layerChannels[i] + kXChannelFirst : defIndex;
        }
        break;
    default:
        assert(false);
        break;
    } // switch
} // ReadOIIOPlugin::getOIIOChannelIndexesFromLayerName

// Whether a and b have the same windows, tiles and channels: decodePlane() computes the windows to read, the sizes of
// its buffers and the channel indexes from one spec of a subimage, and OpenImageIO's read_scanlines() and read_tiles()
// write whole lines and tiles of the other, the ImageInput's current spec (#242). The pixel formats (format,
// channelformats) are not compared: decodePlane() reads all channels as FLOAT, which OpenImageIO converts to.
static bool
sameLayout(const ImageSpec& a,
           const ImageSpec& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.width == b.width && a.height == b.height && a.depth == b.depth
        && a.full_x == b.full_x && a.full_y == b.full_y && a.full_z == b.full_z && a.full_width == b.full_width
        && a.full_height == b.full_height && a.full_depth == b.full_depth && a.tile_width == b.tile_width
        && a.tile_height == b.tile_height && a.tile_depth == b.tile_depth && a.nchannels == b.nchannels
        && a.alpha_channel == b.alpha_channel && a.channelnames == b.channelnames && a.deep == b.deep;
}

// The end of the tiles of size tile, in a grid that starts at origin, that cover [origin, origin + length) (length
// >= 0): in 64 bits, because it may be past INT_MAX for a data window that ends near it (#466).
static std::int64_t
tileGridEnd(int origin,
            int length,
            int tile)
{
    return (tile > 0) ? origin + ((std::int64_t)length + tile - 1) / tile * tile : (std::int64_t)origin + length;
}

// The data window of a subimage in OFX pixel coordinates (y up), without the black border of the edge pixels mode,
// and the X offset of the data (dataOffset, also used to read it): with offsetNegativeDisplayWindow, or a positive
// display window origin, the data and the display window are shifted so that the display window starts at x = 0.
static OfxRectI
specDataWindow(const ImageSpec& spec,
               bool offsetNegativeDisplayWindow,
               int* dataOffset)
{
    *dataOffset = 0;
    if ((spec.full_x != 0) && (offsetNegativeDisplayWindow || (spec.full_x >= 0))) {
        *dataOffset = -spec.full_x;
    }
    // Remember that exr boxes start at top left, and OpenFX at bottom left
    // so we need to flip the bbox relative to the frame.
    OfxRectI specBounds;
    specBounds.x1 = spec.x + *dataOffset;
    specBounds.y1 = spec.full_y + spec.full_height - (spec.y + spec.height);
    specBounds.x2 = spec.x + spec.width + *dataOffset;
    specBounds.y2 = spec.full_y + spec.full_height - spec.y;

    return specBounds;
}

// The frame bounds and the format of an image whose subimages are specs (see ReadOIIOPlugin::getFrameBounds()): the
// union of the data windows (with the black border of edgePixelsMode), and the intersection of the display windows.
static void
getFrameBoundsOfSpecs(const vector<ImageSpec>& specs,
                      bool offsetNegativeDisplayWindow,
                      EdgePixelsEnum edgePixelsMode,
                      OfxRectI* bounds,
                      OfxRectI* format)
{
    // Union bounds across all specs to get the RoD
    // Intersect formats across all specs to get the format
    OfxRectI mergeBounds = { 0, 0, 0, 0 }; // start with empty bounds - rectBoundingBox grows them
    OfxRectI formatIntersection = { 0, 0, 0, 0 };
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const ImageSpec& spec = specs[i];
        OfxRectI specFormat;
        // OpenFX requires format to start at 0,0 but EXR does not. User has choice to offset both
        // data window + display window over by the negative amount or to consider the negative display window
        // area as overscan and remove it from the format on all sides.
        specFormat.x1 = specFormat.y1 = 0;
        specFormat.x2 = spec.full_x + spec.full_width;

        // EXR origin is top left, but OpenFX expects lower left
        // keep data where it is and set spec.full_height at y = 0
        specFormat.y2 = spec.full_height;

        if (spec.full_x != 0) {
            if (!offsetNegativeDisplayWindow && (spec.full_x < 0)) {
                // Leave data where it is and shrink the format by the negative
                // amount on both sides so that it starts at (0,0)
                specFormat.x2 = spec.full_width + spec.full_x - (-spec.full_x);
            } else {
                // Shift both to get dispwindow over to 0,0 (the data is shifted by specDataWindow()).
                specFormat.x2 = spec.full_width;
            }
        }

        int dataOffset;
        OfxRectI specBounds = specDataWindow(spec, offsetNegativeDisplayWindow, &dataOffset);

        switch (edgePixelsMode) {
        case eEdgePixelsAuto:
            if ((spec.x != spec.full_x) || (spec.y != spec.full_y) || (spec.width != spec.full_width) || (spec.height != spec.full_height)) {
                specBounds.x1 -= 1;
                specBounds.y1 -= 1;
                specBounds.x2 += 1;
                specBounds.y2 += 1;
            }
            break;
        case eEdgePixelsEdgeDetect:
            if ((spec.x != spec.full_x) && (spec.y != spec.full_y) && (spec.width != spec.full_width) && (spec.height != spec.full_height)) {
                specBounds.x1 -= 1;
                specBounds.y1 -= 1;
                specBounds.x2 += 1;
                specBounds.y2 += 1;
            } else {
                if (spec.x != spec.full_x) {
                    specBounds.x1 -= 1;
                }
                if (spec.width != spec.full_width) {
                    specBounds.x2 += 1;
                }
                if (spec.y != spec.full_y) {
                    specBounds.y2 += 1;
                }
                if (spec.height != spec.full_height) {
                    specBounds.y1 -= 1;
                }
            }
            break;
        case eEdgePixelsRepeat:
            // Don't add any black pixels
            break;
        case eEdgePixelsBlack:
            // Always add black pixels around the edges of the box.
            specBounds.x1 -= 1;
            specBounds.y1 -= 1;
            specBounds.x2 += 1;
            specBounds.y2 += 1;
            break;
        }

        if (i == 0) {
            mergeBounds = specBounds;
            formatIntersection = specFormat;
        } else {
            Coords::rectBoundingBox(specBounds, mergeBounds, &mergeBounds);
            Coords::rectIntersection(specFormat, formatIntersection, &formatIntersection);
        }
    }
    *bounds = mergeBounds;
    *format = formatIntersection;
}

void
ReadOIIOPlugin::decodePlane(const string& filename,
                            OfxTime /*time*/,
                            int view,
                            const OfxRectI& renderWindow,
                            const OfxPointD& renderScale,
                            float* pixelData,
                            const OfxRectI& bounds,
                            PixelComponentEnum pixelComponents,
                            PixelComponentEnum remappedComponents,
                            int pixelComponentCount,
                            const string& rawComponents,
                            int rowBytes)
{
    assert(renderScale.x == 1. && renderScale.y == 1.);
    unused(renderScale);
    unused(pixelComponentCount);
    // assert(kSupportsTiles || (renderWindow.x1 == 0 && renderWindow.x2 == spec.full_width && renderWindow.y1 == 0 && renderWindow.y2 == spec.full_height));
    // assert((renderWindow.x2 - renderWindow.x1) <= spec.width && (renderWindow.y2 - renderWindow.y1) <= spec.height);
    assert(bounds.x1 <= renderWindow.x1 && renderWindow.x1 <= renderWindow.x2 && renderWindow.x2 <= bounds.x2);
    assert(bounds.y1 <= renderWindow.y1 && renderWindow.y1 <= renderWindow.y2 && renderWindow.y2 <= bounds.y2);

    // we only support RGBA, RGB or Alpha output clip on the color plane
    if ((pixelComponents != ePixelComponentRGBA) && (pixelComponents != ePixelComponentRGB) && (pixelComponents != ePixelComponentXY) && (pixelComponents != ePixelComponentAlpha)
        && (pixelComponents != ePixelComponentCustom)) {
        setPersistentMessage(Message::eMessageError, "", "OIIO: can only read RGBA, RGB, RG, Alpha or custom components images");
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    // After a RAW file was read (with LibRaw, which uses OpenMP), on every exit of this function, including the error
    // paths: pin the OpenMP runtime. An OpenImageIO built with non-embedded plug-ins loads its raw reader, LibRaw and
    // OpenMP only on the first RAW read, after ReadOIIO's load action pinned what was loaded then (#239).
    struct PinOpenMPAfterRawRead {
        bool raw = false;
        ~PinOpenMPAfterRawRead()
        {
            if (raw) {
                // (a destructor is noexcept: nothing may escape, e.g. std::bad_alloc)
                try {
                    pinOpenMPRuntime();
                } catch (...) {
                }
            }
        }
    } pinOpenMPAfterRawRead;

    vector<int> channels;
    int numChannels = 0;
    vector<ImageSpec> subimages;
    std::unique_ptr<ImageInput> img = openFile(filename, &subimages);
    if (!std::strcmp(img->format_name(), "raw")) {
        pinOpenMPAfterRawRead.raw = true;
    }

    if (subimages.empty()) {
        setPersistentMessage(Message::eMessageError, "", string("Cannot open file ") + filename);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    int subImageIndex = 0;
    if (pixelComponents != ePixelComponentCustom) {
        if (!_outputLayer) { // host is not multilayer nor anything, just use basic indexes
            // The indexes in channels are those of the file's channels + kXChannelFirst, or 0 and 1 for the
            // constants 0 and 1: the first channels of the first subimage are R, G, B (or one grey channel), and
            // alpha is the file's alpha channel, if any.
            const ImageSpec& spec = subimages[0];
            const int nc = spec.nchannels;
            const int alpha = (spec.alpha_channel >= 0 && spec.alpha_channel < nc) ? spec.alpha_channel : -1;
            const int nColor = (alpha >= 0 && alpha < 3) ? alpha : (std::min)(nc, 3); // the color channels before alpha
            int rgb[3];
            for (int c = 0; c < 3; ++c) {
                // grey: R = G = B
                rgb[c] = (nColor == 1) ? kXChannelFirst : ((c < nColor) ? c + kXChannelFirst : 0);
            }
            const int a = (alpha >= 0) ? alpha + kXChannelFirst : 1;
            switch (pixelComponents) {
            case ePixelComponentRGBA:
                numChannels = 4;
                channels.resize(numChannels);
                channels[0] = rgb[0];
                channels[1] = rgb[1];
                channels[2] = rgb[2];
                channels[3] = a;
                break;
            case ePixelComponentRGB:
                numChannels = 3;
                channels.resize(numChannels);
                channels[0] = rgb[0];
                channels[1] = rgb[1];
                channels[2] = rgb[2];
                break;
            case ePixelComponentXY:
                numChannels = 2;
                channels.resize(numChannels);
                channels[0] = (nc > 0) ? kXChannelFirst : 0;
                channels[1] = (nc > 1) ? kXChannelFirst + 1 : 0;
                break;

            case ePixelComponentAlpha:
                numChannels = 1;
                channels.resize(numChannels);
                // a single-channel file is an alpha image
                channels[0] = (alpha >= 0) ? a : ((nc == 1) ? kXChannelFirst : 1);
                break;
            default:
                assert(false);
                break;
            }
        } else {
            // the main thread may rebuild the menu meanwhile: copy the layer name under the lock
            string layerName;
            bool found = false;
            {
                IOAutoMutex lock(_outputLayerMenuMutex);
                // buildOutputLayerMenu keeps these in sync (under the lock)
                assert(_outputLayer->getNOptions() == (int)_outputLayerMenu.size());
                const int layer_i = _outputLayer->getValue();
                if ((layer_i < (int)_outputLayerMenu.size()) && (layer_i >= 0)) {
                    layerName = _outputLayerMenu[layer_i].first;
                    found = true;
                }
            }
            if (!found) {
                setPersistentMessage(Message::eMessageError, "", "Failure to find requested layer in file");
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            getOIIOChannelIndexesFromLayerName(filename, view, layerName, pixelComponents, subimages, channels, numChannels, subImageIndex);
        }
    } // if (pixelComponents != ePixelComponentCustom) {
#ifdef OFX_EXTENSIONS_NATRON
    else {
        MultiPlane::ImagePlaneDesc plane, pairedPlane;
        MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(rawComponents, &plane, &pairedPlane);
        numChannels = (int)plane.getNumComponents();
        if (plane.getNumComponents() > 0) {
            channels.resize(numChannels);
            string layer = plane.getPlaneID();

            if (_outputLayer) {
                getOIIOChannelIndexesFromLayerName(filename, view, layer, pixelComponents, subimages, channels, numChannels, subImageIndex);
            } else {
                const std::vector<std::string>& layerChannels = plane.getChannels();
                if ((numChannels == 1) && (layerChannels[0] == layer)) {
                    layer.clear();
                }

                for (int i = 0; i < numChannels; ++i) {
                    bool found = false;
                    for (std::size_t j = 0; j < subimages[0].channelnames.size(); ++j) {
                        string realChan;
                        if (!layer.empty()) {
                            realChan.append(layer);
                            realChan.push_back('.');
                        }
                        realChan.append(layerChannels[i]);
                        if (subimages[0].channelnames[j] == realChan) {
                            channels[i] = j + kXChannelFirst;
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        setPersistentMessage(Message::eMessageError, "", "Could not find channel named " + layerChannels[i + 1]);
                        throwSuiteStatusException(kOfxStatFailed);

                        return;
                    }
                }
            }
        }
    }
#endif

    // (into a local spec: subimages must stay the specs of all the subimages, see getFrameBoundsOfSpecs() below)
    ImageSpec seekSpec;
    if (!img->seek_subimage(subImageIndex, 0, seekSpec)) {
        stringstream ss;
        ss << "Cannot seek subimage " << subImageIndex << " in " << filename;
        setPersistentMessage(Message::eMessageError, "", ss.str());
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    // seek_subimage() may have read the subimage's header again (e.g. the TIFF reader does, after the failed seek past
    // the last subimage in getSpecsFromImageInput()): if the file was rewritten in place since openFile(), the header
    // is not the one that subimages, and thus the windows, buffers and channels below, come from, and reading would
    // write out of them (#242).
    if (!sameLayout(seekSpec, subimages[subImageIndex])) {
        setPersistentMessage(Message::eMessageError, "", string("File changed while it was read: ") + filename);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    bool offsetNegativeDisplayWindow;
    _offsetNegativeDispWindow->getValue(offsetNegativeDisplayWindow);

    // Non const because ImageSpec::valid_tile_range is not const...
    ImageSpec& spec = subimages[subImageIndex];

    // The data window and the X offset, as in getFrameBounds()
    int dataOffset;
    const OfxRectI specBounds = specDataWindow(spec, offsetNegativeDisplayWindow, &dataOffset);

    // Where to write the data in the buffer, everything outside of that is black (including the extra padding we
    // added in getFrameBounds). The render window may be any part of bounds (e.g. a few columns or rows), and may not
    // be in the data window if the file changed since getFrameBounds().
    OfxRectI renderWindowUnPadded;
    const bool inData = clipToFile(renderWindow, specBounds, pixelData, bounds, numChannels, eBitDepthFloat, rowBytes, &renderWindowUnPadded);
    {
        // The image of this file is its frame bounds, as getFrameBounds() computes them, which include the black
        // padding (that black is in the RoD: for an Opaque file, its alpha is 1, as elsewhere in the RoD). Out of
        // them, e.g. if the file was rewritten smaller since getFrameBounds(), the black is transparent.
        EdgePixelsEnum edgePixelsMode;
        {
            int edgeMode_i;
            _edgePixels->getValue(edgeMode_i);
            edgePixelsMode = (EdgePixelsEnum)edgeMode_i;
        }
        OfxRectI fileBounds, fileFormat;
        getFrameBoundsOfSpecs(subimages, offsetNegativeDisplayWindow, edgePixelsMode, &fileBounds, &fileFormat);
        OfxRectI fileWindow;
        intersectWindow(renderWindow, fileBounds, &fileWindow);
        setDecodedWindow(fileWindow);
    }
    if (!inData) {
        // nothing to read

        return;
    }
    // A tiled file whose last tile ends past INT_MAX (a crafted or broken file, #466): OpenImageIO before 3.1.19 and
    // 3.2.2 (OpenImageIO#5494) steps from tile to tile in int in read_tiles() (y += tile_height, x += tile_width),
    // and format readers' own tile loops still do (TIFF, 3.2.2), which overflows on the last tile row or column. Refuse
    // the whole file, not only the windows that reach its last tiles: one consistent error, and it keeps the rounding
    // of the decode window to the tiles below in the range of int.
    if ((spec.tile_width != 0)
        && ((tileGridEnd(spec.x, spec.width, spec.tile_width) > INT_MAX) || (tileGridEnd(spec.y, spec.height, spec.tile_height) > INT_MAX))) {
        setPersistentMessage(Message::eMessageError, "", string("ReadOIIO: cannot read the tiles of this file, its data window ends too close to the largest int for its tile size: ") + filename);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    const int pixelBytes = numChannels * getComponentBytes(eBitDepthFloat);
    const int xStride = pixelBytes;
    const int yStride = -rowBytes;

    // The renderWindowUnPadded must be contained in the original render Window
    assert(renderWindowUnPadded.x1 >= renderWindow.x1 && renderWindowUnPadded.x2 <= renderWindow.x2 && renderWindowUnPadded.y1 >= renderWindow.y1 && renderWindowUnPadded.y2 <= renderWindow.y2);

    int zbegin = 0;
    int zend = 1;

    // Invert what was done in getframesbounds
    int xbegin, xend, ybegin, yend;
    xbegin = renderWindowUnPadded.x1 - dataOffset;
    xend = renderWindowUnPadded.x2 - dataOffset;

    // Invert what was done in getframebound
    yend = spec.full_height + spec.full_y - renderWindowUnPadded.y1;
    ybegin = spec.full_height + spec.full_y - renderWindowUnPadded.y2;
    // (in the data window)
    assert(spec.x <= xbegin && xbegin < xend && xend <= spec.x + spec.width);
    assert(spec.y <= ybegin && ybegin < yend && yend <= spec.y + spec.height);

    // Pixel offset to the start of the render window first line
    size_t bottomScanLineDataStartOffset = (size_t)(renderWindowUnPadded.y1 - bounds.y1) * rowBytes + (size_t)(renderWindowUnPadded.x1 - bounds.x1) * pixelBytes;
    // Pixel offset to the start of the last line of the render window
    size_t topScanLineDataStartOffset = (size_t)(renderWindowUnPadded.y2 - 1 - bounds.y1) * rowBytes + (size_t)(renderWindowUnPadded.x1 - bounds.x1) * pixelBytes; // offset for line y2-1

    std::size_t incr; // number of channels processed
    for (std::size_t i = 0; i < channels.size(); i += incr) {
        incr = 1;
        // if the channel was already read, just duplicate it (rather than reading the same channel again)
        bool duplicate = false;
        for (std::size_t j = 0; !duplicate && j < i; ++j) {
            if (channels[i] == channels[j]) {
                // (the channel j was read in renderWindowUnPadded, and is black elsewhere)
                char* yptr = (char*)pixelData + bottomScanLineDataStartOffset;
                for (int y = renderWindowUnPadded.y1; y < renderWindowUnPadded.y2; ++y, yptr += rowBytes) {
                    float* xptr = (float*)yptr;
                    for (int x = renderWindowUnPadded.x1; x < renderWindowUnPadded.x2; ++x, xptr += numChannels) {
                        xptr[i] = xptr[j];
                    }
                }
                duplicate = true;
            }
        }
        if (duplicate) {
            // this channels was a duplicate
            continue;
        }

        if (channels[i] < kXChannelFirst) {
            // fill channel with constant value, in the data window (everything outside of it is black)
            char* lineStart = (char*)pixelData + bottomScanLineDataStartOffset;
            for (int y = renderWindowUnPadded.y1; y < renderWindowUnPadded.y2; ++y, lineStart += rowBytes) {
                float* cur = (float*)lineStart;
                for (int x = renderWindowUnPadded.x1; x < renderWindowUnPadded.x2; ++x, cur += numChannels) {
                    cur[i] = float(channels[i]);
                }
            }
        } else {
            // read as many contiguous channels as we can
            while ((i + incr) < channels.size() && channels[i + incr] == channels[i + incr - 1] + 1) {
                ++incr;
            }

            const int outputChannelBegin = i;
            const int chbegin = channels[i] - kXChannelFirst; // start channel for reading
            const int chend = chbegin + incr; // last channel + 1

            // Start on the last line to invert Y with a negative stride
            // Pass to OIIO the pointer to the first pixel of the last scan-line of the render window.
            float* topScanLineDataStartPtr = (float*)((char*)pixelData + topScanLineDataStartOffset) + outputChannelBegin;
            // We clamp to the valid scanlines portion.
            int ybeginClamped = (std::min)((std::max)(spec.y, ybegin), spec.y + spec.height);
            int yendClamped = (std::min)((std::max)(spec.y, yend), spec.y + spec.height);
            int xbeginClamped = (std::min)((std::max)(spec.x, xbegin), spec.x + spec.width);
            int xendClamped = (std::min)((std::max)(spec.x, xend), spec.x + spec.width);

            // Do not call valid_tile_range because a tiled file can only be read with read_tiles with OpenImageIO.
            // Otherwise it will give the following error: called OpenEXRInput::read_native_scanlines without an open file
            if ((spec.tile_width == 0) && (xbeginClamped == spec.x) && (xendClamped == spec.x + spec.width)) {
                // Read by scanlines: read_scanlines() reads whole lines of the data window, here as wide as the
                // render window

                if (!img->read_scanlines(subImageIndex, 0, ybeginClamped, // y begin
                                         yendClamped, // y end
                                         zbegin, // z
                                         chbegin, // chan begin
                                         chend, // chan end
                                         TypeDesc::FLOAT, // data type
                                         topScanLineDataStartPtr,
                                         xStride, // x stride
                                         yStride)) { // y stride < make it invert Y;
                    setPersistentMessage(Message::eMessageError, "", img->geterror());
                    throwSuiteStatusException(kOfxStatFailed);

                    return;
                }
            } else if (spec.tile_width == 0) {
                // Read by scanlines, the render window is narrower than the data window: read_scanlines() reads
                // whole lines, so read blocks of lines into a buffer, and copy the render window part of them.
                const int nch = chend - chbegin;
                const std::size_t lineFloats = (std::size_t)spec.width * nch;
                // at most 256 lines, and at most 16 MB (but at least one line)
                const int kBlockLines = 256;
                const std::size_t kBlockBytes = 16 << 20;
                const int blockLines = (std::max)(1, (int)(std::min)((std::size_t)(std::min)(kBlockLines, yendClamped - ybeginClamped), kBlockBytes / (lineFloats * sizeof(float))));
                vector<float> lines(lineFloats * blockLines);
                char* dstLine = (char*)topScanLineDataStartPtr; // the line ybeginClamped (the top line)
                for (int yb = ybeginClamped; yb < yendClamped; yb += blockLines) {
                    const int ye = (std::min)(yb + blockLines, yendClamped);
                    if (!img->read_scanlines(subImageIndex, 0, yb, ye, zbegin, chbegin, chend, TypeDesc::FLOAT, &lines.front(), AutoStride, AutoStride)) {
                        setPersistentMessage(Message::eMessageError, "", img->geterror());
                        throwSuiteStatusException(kOfxStatFailed);

                        return;
                    }
                    for (int y = yb; y < ye; ++y, dstLine -= rowBytes) {
                        const float* srcPtr = &lines.front() + (std::size_t)(y - yb) * lineFloats + (std::size_t)(xbeginClamped - spec.x) * nch;
                        float* dstPtr = (float*)dstLine;
                        for (int x = xbeginClamped; x < xendClamped; ++x, srcPtr += nch, dstPtr += numChannels) {
                            for (int c = 0; c < nch; ++c) {
                                dstPtr[c] = srcPtr[c];
                            }
                        }
                    }
                }
            } else {
                // If the region to read is not a multiple of tile size we must provide a buffer
                // with the appropriate size.
                float* tiledBuffer = topScanLineDataStartPtr;
                vector<float> tiledBufferStorage; // (freed on all paths, also if read_tiles() fails)
                int tiledXBegin = xbeginClamped;
                int tiledYBegin = ybeginClamped;
                int tiledXEnd = xendClamped;
                int tiledYEnd = yendClamped;
                bool validRange = spec.valid_tile_range(xbegin, xend, ybegin, yend, zbegin, zend);

                // This is the number of extra pixels we decoded on the left
                int xBeginPadToTileSize = 0;
                // This is the numner of extra pixels we decoded on the bottom
                int yBeginPadToTileSize = 0;
                std::size_t tiledBufferRowSize = rowBytes;
                std::size_t tiledBufferPixelSize = xStride;
                if (!validRange) {
                    // If the tile range is invalid, expand to the closest enclosing valid tile range.

                    // tiledXBegin must be at a valid multiple of tile_width from spec.x
                    tiledXBegin = spec.x + (int)std::floor((double)(xbeginClamped - spec.x) / spec.tile_width) * spec.tile_width;

                    // tiledYBegin must be at a valid multiple of tile_height from spec.y
                    tiledYBegin = spec.y + (int)std::floor((double)(ybeginClamped - spec.y) / spec.tile_height) * spec.tile_height;

                    // tiledXEnd must be at a valid multiple of tile_width from spec.x, or the end of the data window.
                    // (The int conversions are safe: the tile grid ends at INT_MAX at most, checked above.)
                    tiledXEnd = (int)(std::min)(tileGridEnd(spec.x, xendClamped - spec.x, spec.tile_width), (std::int64_t)spec.x + spec.width);

                    // tiledYEnd must be at a valid multiple of tile_height from spec.y, or the end of the data window
                    tiledYEnd = (int)(std::min)(tileGridEnd(spec.y, yendClamped - spec.y, spec.tile_height), (std::int64_t)spec.y + spec.height);

                    tiledXBegin = (std::max)(spec.x, tiledXBegin);
                    tiledYBegin = (std::max)(spec.y, tiledYBegin);

                    // Check that we made up a correct tile range
                    assert(spec.valid_tile_range(tiledXBegin, tiledXEnd, tiledYBegin, tiledYEnd, zbegin, zend));

                    xBeginPadToTileSize = xbeginClamped - tiledXBegin;
                    yBeginPadToTileSize = ybeginClamped - tiledYBegin;

                    tiledBufferPixelSize = getComponentBytes(eBitDepthFloat) * (chend - chbegin);
                    tiledBufferRowSize = (tiledXEnd - tiledXBegin) * tiledBufferPixelSize;
                    std::size_t nBytes = tiledBufferRowSize * (tiledYEnd - tiledYBegin);
                    try {
                        tiledBufferStorage.resize((nBytes + sizeof(float) - 1) / sizeof(float));
                    } catch (const std::bad_alloc&) {
                        throwSuiteStatusException(kOfxStatErrMemory);

                        return;
                    }
                    char* tiledBufferStart = (char*)&tiledBufferStorage.front();

                    // Make tile buffer point to the first pixel of the last scan-line of our temporary tile-adjusted buffer.
                    tiledBuffer = (float*)(tiledBufferStart + (tiledYEnd - tiledYBegin - 1) * tiledBufferRowSize);
                }

                // Pass the valid tile range and buffer to OIIO and decode with a negative Y stride from
                // top to bottom
                if (!img->read_tiles(subImageIndex, 0, tiledXBegin, // x begin
                                     tiledXEnd, // x end
                                     tiledYBegin, // y begin
                                     tiledYEnd, // y end
                                     zbegin, // z begin
                                     zend, // z end
                                     chbegin, // chan begin
                                     chend, // chan end
                                     TypeDesc::FLOAT, // data type
                                     tiledBuffer,
                                     tiledBufferPixelSize, // x stride
                                     -tiledBufferRowSize, // y stride < make it invert Y
                                     AutoStride)) { // z stride
                    setPersistentMessage(Message::eMessageError, "", img->geterror());
                    throwSuiteStatusException(kOfxStatFailed);

                    return;
                }

                if (!validRange) {
                    // If we allocated a temporary tile-adjusted buffer, we must copy it back into the pixelData buffer.

                    // This points to the start of the first pixel of the last scan-line of the render window
                    char* dst_pix = (char*)topScanLineDataStartPtr;

                    // Copy each scan-line from our temporary buffer to the final buffer. Since each buffer is pointing to the last
                    // scan-line at the begining, we pass negative pixel offsets in the iteration loop.

                    // Position the tiled buffer to the start of the content that should have been read in the original range.
                    // To retrieve the position of the original range, we substract the number of extra lines that were decoded
                    // from the tiledBuffer: tiledBuffer points to tiledYend - tiledYBegin - 1, so we make it point to tiledYend - tiledYbegin - 1 - yEndPadToTileSize

                    assert((tiledYBegin + yBeginPadToTileSize) == ybeginClamped);
                    assert((tiledXBegin + xBeginPadToTileSize) == xbeginClamped);
                    const char* src_pix = (const char*)((char*)tiledBuffer - yBeginPadToTileSize * tiledBufferRowSize + xBeginPadToTileSize * tiledBufferPixelSize);

                    for (int y = ybeginClamped; y < yendClamped;
                         ++y,
                             src_pix -= tiledBufferRowSize,
                             dst_pix -= rowBytes) {

                        const float* srcPtr = (const float*)src_pix;
                        float* dstPtr = (float*)dst_pix;
                        for (int x = xbeginClamped; x < xendClamped;
                             ++x,
                                 srcPtr += (chend - chbegin),
                                 dstPtr += numChannels) {
                            for (int c = 0; c < (chend - chbegin); ++c) {
                                dstPtr[c] = srcPtr[c];
                            }
                        }
                    }
                }
            }
        } // if (channels[i] < kXChannelFirst) {
    } // for (std::size_t i = 0; i < channels.size(); i+=incr) {

    img->close();
} // ReadOIIOPlugin::decodePlane

bool
ReadOIIOPlugin::getFrameBounds(const string& filename,
                               OfxTime /*time*/,
                               int /*view*/,
                               OfxRectI* bounds,
                               OfxRectI* format,
                               double* par,
                               string* /*error*/,
                               int* tile_width,
                               int* tile_height)
{
    assert(bounds && par);
    vector<ImageSpec> specs;
    getSpecs(filename, &specs);

    bool offsetNegativeDisplayWindow;
    _offsetNegativeDispWindow->getValue(offsetNegativeDisplayWindow);

    EdgePixelsEnum edgePixelsMode;
    {
        int edgeMode_i;
        _edgePixels->getValue(edgeMode_i);
        edgePixelsMode = (EdgePixelsEnum)edgeMode_i;
    }

    getFrameBoundsOfSpecs(specs, offsetNegativeDisplayWindow, edgePixelsMode, bounds, format);
    if (specs.size() > 0) {
        *tile_width = specs[0].tile_width;
        *tile_height = specs[0].tile_height;
        const float pixelAspectRatio = specs[0].get_float_attribute("PixelAspectRatio", 1);
        // a PAR of 0 or less in the file is invalid (the host divides by it)
        *par = (pixelAspectRatio > 0.f) ? pixelAspectRatio : 1.;
    } else {
        *tile_width = *tile_height = 0;
        *par = 1.;
    }

    return true;
} // ReadOIIOPlugin::getFrameBounds

string
ReadOIIOPlugin::metadata(const string& filename)
{
    stringstream ss;

    std::unique_ptr<ImageInput> img;

    {
        // use the right config
        ImageSpec config;
        getConfig(&config);

        img = ImageInput::open(filename, &config);
        if (!img.get()) {
            setPersistentMessage(Message::eMessageError, "", string("ReadOIIO: cannot open file ") + filename);
            throwSuiteStatusException(kOfxStatFailed);

            return string();
        }
    }
    vector<ImageSpec> subImages;
    getSpecs(filename, &subImages);
    if (subImages.empty()) {
        setPersistentMessage(Message::eMessageError, "", string("No information found in") + filename);
        throwSuiteStatusException(kOfxStatFailed);

        return string();
    }

    ss << filename << " : ";
    if (subImages.size() > 1) {
        ss << "    " << subImages.size() << " subimages:" << std::endl;
    }
    int movie = 0;
    for (std::size_t sIt = 0; sIt < subImages.size() && !movie; ++sIt) {
        if (sIt == 0) {
            // only print the info about the first subimage in movies
            movie = subImages[sIt].get_int_attribute("oiio:Movie");
        }
        if (!movie && subImages.size() > 1) {
            ss << "subimage " << sIt << ":" << std::endl;
        }
        ss << subImages[sIt].width << " x " << subImages[sIt].height;
        if (subImages[sIt].depth > 1) {
            ss << " x " << subImages[sIt].depth;
        }
        int bits = subImages[sIt].get_int_attribute("oiio:BitsPerSample", 0);
        ss << ", " << subImages[sIt].nchannels << " channel, ";
        if (subImages[sIt].deep) {
            ss << "deep ";
        }
        if (subImages[sIt].depth > 1) {
            ss << "volume ";
        }
        TypeDesc type = subImages[sIt].format;
        if (bits && bits < (int)type.size() * 8) {
            // The "oiio:BitsPerSample" betrays a different bit depth in the
            // file than the data type we are passing.
            if (type == TypeDesc::UINT8 || type == TypeDesc::UINT16 || type == TypeDesc::UINT32 || type == TypeDesc::UINT64) {
                ss << "uint" << bits;
            } else if (type == TypeDesc::INT8 || type == TypeDesc::INT16 || type == TypeDesc::INT32 || type == TypeDesc::INT64) {
                ss << "int" << bits;
            } else {
                ss << type.c_str(); // use the name implied by type
            }
        } else {
            ss << type.c_str(); // use the name implied by type
        }
        ss << " " << img->format_name() << std::endl;

        ss << "    channel list: ";
        for (int i = 0; i < subImages[sIt].nchannels; ++i) {
            if (i < (int)subImages[sIt].channelnames.size()) {
                ss << subImages[sIt].channelnames[i];
                if (i == subImages[sIt].alpha_channel) {
                    ss << " - alpha channel";
                }
            } else {
                ss << "unknown";
            }
            if (i < (int)subImages[sIt].channelformats.size()) {
                ss << " (" << subImages[sIt].channelformats[i].c_str() << ")";
            }
            if (i < subImages[sIt].nchannels - 1) {
                if (subImages[sIt].nchannels <= 4) {
                    ss << ", ";
                } else {
                    ss << std::endl
                       << "                  ";
                }
            }
        }
        ss << std::endl;

        if (subImages[sIt].x || subImages[sIt].y || subImages[sIt].z) {
            ss << "    pixel data origin: x=" << subImages[sIt].x << ", y=" << subImages[sIt].y;
            if (subImages[sIt].depth > 1) {
                ss << ", z=" << subImages[sIt].z;
            }
            ss << std::endl;
        }
        if (subImages[sIt].full_x || subImages[sIt].full_y || subImages[sIt].full_z || ((subImages[sIt].full_width != subImages[sIt].width) && (subImages[sIt].full_width != 0)) || ((subImages[sIt].full_height != subImages[sIt].height) && (subImages[sIt].full_height != 0)) || ((subImages[sIt].full_depth != subImages[sIt].depth) && (subImages[sIt].full_depth != 0))) {
            ss << "    full/display size: " << subImages[sIt].full_width << " x " << subImages[sIt].full_height;
            if (subImages[sIt].depth > 1) {
                ss << " x " << subImages[sIt].full_depth;
            }
            ss << std::endl;
            ss << "    full/display origin: " << subImages[sIt].full_x << ", " << subImages[sIt].full_y;
            if (subImages[sIt].depth > 1) {
                ss << ", " << subImages[sIt].full_z;
            }
            ss << std::endl;
        }
        if (subImages[sIt].tile_width) {
            ss << "    tile size: " << subImages[sIt].tile_width << " x " << subImages[sIt].tile_height;
            if (subImages[sIt].depth > 1) {
                ss << " x " << subImages[sIt].tile_depth;
            }
            ss << std::endl;
        }

        for (ParamValueList::const_iterator p = subImages[sIt].extra_attribs.begin(); p != subImages[sIt].extra_attribs.end(); ++p) {
            string s = subImages[sIt].metadata_val(*p, true);
            ss << "    " << p->name() << ": ";
            if (s == "1.#INF") {
                ss << "inf";
            } else {
                ss << s;
            }
            ss << std::endl;
        }

        if ((subImages.size() > 1) && (sIt < subImages.size() - 1)) {
            ss << std::endl;
        }
    }
    assert(img.get());
    img->close();

    return ss.str();
} // ReadOIIOPlugin::metadata

inline bool
startsWith(const std::string& str,
           const std::string& prefix)
{
    return str.substr(0, prefix.size()) == prefix;
    // case insensitive version:
    // return ci_string(str.substr(0,prefix.size()).c_str()) == prefix.c_str();
}

bool
ReadOIIOPlugin::isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime, int& view, std::string& plane)
{
    if (GenericReaderPlugin::isIdentity(args, identityClip, identityTime, view, plane)) {
        return true;
    }
    // If the plane in argument is the color-plane and the user selected another plane in the output plane selector, be identity on that plane.
    // is the one selected in output of the plug-in as color plane
    if (_outputLayer && (args.plane == kOfxMultiplaneColorPlaneID)) {
        // (isIdentity may run on a render thread while the main thread rebuilds the menu: copy the layer under the lock)
        const int layer_i = _outputLayer->getValue(); // (may be stale: checked under the lock)
        string layerName;
        vector<string> channelNames;
        {
            IOAutoMutex lock(_outputLayerMenuMutex);
            if (layer_i < 0 || layer_i >= (int)_outputLayerMenu.size()) {
                return false;
            }
            layerName = _outputLayerMenu[layer_i].first;
            channelNames = _outputLayerMenu[layer_i].second.layer.channelNames;
        }
        if (startsWith(layerName, kReaderColorLayer)) {
            return false;
        }
        identityClip = _outputClip;
        identityTime = args.time;
        view = args.view;
        MultiPlane::ImagePlaneDesc planeDesc(layerName, "", "", channelNames);
        plane = MultiPlane::ImagePlaneDesc::mapPlaneToOFXPlaneString(planeDesc);
        return true;
    }
    return false;
} // isIdentity

class ReadOIIOPluginFactory
    : public PluginFactoryHelper<ReadOIIOPluginFactory> {
public:
    ReadOIIOPluginFactory(const string& id,
                          unsigned int verMaj,
                          unsigned int verMin)
        : PluginFactoryHelper<ReadOIIOPluginFactory>(id, verMaj, verMin)
    {
    }

    virtual void describe(ImageEffectDescriptor& desc) OVERRIDE FINAL;
    virtual void describeInContext(ImageEffectDescriptor& desc, ContextEnum context) OVERRIDE FINAL;
    virtual ImageEffect* createInstance(OfxImageEffectHandle handle, ContextEnum context) OVERRIDE FINAL;
    virtual void load() OVERRIDE FINAL;
    virtual void unload() OVERRIDE FINAL;
    bool isVideoStreamPlugin() const { return false; }

    vector<string> _extensions;
};

void
ReadOIIOPluginFactory::load()
{
    {
        int i = 0;
        bool libraw_gpl2, libraw_gpl3;
        getLibRawDemosaicCaps(&libraw_gpl2, &libraw_gpl3);
        // AHD-Mod, AFD, VCD, Mixed, LMMSE are GPL2, AMaZE is GPL3
        libraw_demosaic[i++] = eRawDemosaicNone;
        libraw_demosaic[i++] = eRawDemosaicLinear;
        libraw_demosaic[i++] = eRawDemosaicVNG;
        libraw_demosaic[i++] = eRawDemosaicPPG;
        libraw_demosaic[i++] = eRawDemosaicAHD;
        libraw_demosaic[i++] = eRawDemosaicDCB;
        if (libraw_gpl2) {
            libraw_demosaic[i++] = eRawDemosaicAHDMod;
            libraw_demosaic[i++] = eRawDemosaicAFD;
            libraw_demosaic[i++] = eRawDemosaicVCD;
            libraw_demosaic[i++] = eRawDemosaicMixed;
            libraw_demosaic[i++] = eRawDemosaicLMMSE;
        }
        if (libraw_gpl3) {
            libraw_demosaic[i++] = eRawDemosaicAMaZE;
        }
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 16, 0)
        libraw_demosaic[i++] = eRawDemosaicDHT;
        libraw_demosaic[i++] = eRawDemosaicAAHD;
#endif
        // Indices past the end of the menu can come from a project saved with a
        // LibRaw build that had more options (e.g. with the GPL demosaic packs):
        // fall back on AHD rather than eRawDemosaicNone (zero-initialized).
        for (; i < eRawDemosaicCount; ++i) {
            libraw_demosaic[i] = eRawDemosaicAHD;
        }
    }

    _extensions.clear();
    // get extensions from OIIO (but there is no distinctions between readers and writers)
    string extensions_list;
    getattribute("extension_list", extensions_list);
    stringstream formatss(extensions_list);
    string format;
    std::list<string> extensionsl;
    while (std::getline(formatss, format, ';')) {
        stringstream extensionss(format);
        string extension;
        std::getline(extensionss, extension, ':'); // extract the format
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
        "mpg", // FFmpeg extensions - better supported by ReadFFmpeg
        "gif", // animated GIFs are only supported by FFmpeg
        "bmp", // OpenImageIO does not read correctly https://raw.githubusercontent.com/NatronGitHub/Natron-Tests/master/TestImageBMP/input.bmp, which is better handled by ImageMagick/ReadMisc from Arena.ofx
        NULL
    };
    for (const char* const* e = extensions_denylist; *e != NULL; ++e) {
        extensionsl.remove(*e);
    }
    _extensions.assign(extensionsl.begin(), extensionsl.end());
    // LibRaw's OpenMP runtime must outlive IO.ofx, as its worker threads do (#239)
    pinOpenMPRuntime();
    // last: only count a successful load (if load throws, the host does not call unload)
    OIIOPluginLoaded(getUID());
}

void
ReadOIIOPluginFactory::unload()
{
    _extensions.clear();

    // stop OpenImageIO's thread pool if this was the last OIIO plug-in loaded
    OIIOPluginUnloaded(getUID());
}

/** @brief The basic describe function, passed a plugin descriptor */
void
ReadOIIOPluginFactory::describe(ImageEffectDescriptor& desc)
{
    GenericReaderDescribe(desc, _extensions, kPluginEvaluation, kSupportsTiles, kIsMultiPlanar);

    // basic labels
    desc.setLabel(kPluginName);
    desc.setPluginDescription(kPluginDescription
                              "\n\n"
                              "This plug-in reads the following file formats, if the OpenImageIO library supports them. BMP files are left to ReadMisc (from openfx-arena), GIF files to ReadFFmpeg.\n"
                              "Cineon (*.cin)\n"
                              "Direct Draw Surface (*.dds)\n"
                              "DPX (*.dpx)\n"
                              "Field3D (*.f3d)\n"
                              "FITS (*.fits)\n"
                              "HDR/RGBE (*.hdr)\n"
                              "HEIC/HEIF (*.heic *.heif)\n"
                              "ICO (*.ico)\n"
                              "IFF (*.iff)\n"
                              "JPEG (*.jpg *.jpe *.jpeg *.jif *.jfif *.jfi)\n"
                              "JPEG-2000 (*.jp2 *.j2k)\n"
                              "JPEG XL (*.jxl)\n"
                              "OpenEXR (*.exr)\n"
                              "PNG / Portable Network Graphics (*.png)\n"
                              "PNM / Netpbm (*.pbm *.pgm *.ppm *.pfm)\n"
                              "PSD (*.psd *.pdd *.psb)\n"
                              "Ptex (*.ptex)\n"
                              "RAW digital camera files (*.crw *.cr2 *.nef *.raf *.dng and others)\n"
                              "RLA (*.rla)\n"
                              "SGI (*.sgi *.rgb *.rgba *.bw *.int *.inta)\n"
                              "Softimage PIC (*.pic)\n"
                              "Targa (*.tga *.tpic)\n"
                              "TIFF (*.tif *.tiff *.tx *.env *.sm *.vsm)\n"
                              "Webp (*.webp)\n"
                              "Zfile (*.zfile)");
} // ReadOIIOPluginFactory::describe

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
ReadOIIOPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                         ContextEnum context)
{
    gHostSupportsDynamicChoices = (getImageEffectHostDescription()->supportsDynamicChoices);
    gHostSupportsMultiPlane = (fetchSuite(kFnOfxImageEffectPlaneSuite, 2, true)) != 0;

    // make some pages and to things in
    PageParamDescriptor* page = GenericReaderDescribeInContextBegin(desc, context, isVideoStreamPlugin(), kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, kSupportsTiles, false);

    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamShowMetadata);
        param->setLabel(kParamShowMetadataLabel);
        param->setHint(kParamShowMetadataHint);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        GroupParamDescriptor* group = desc.defineGroupParam(kGroupAdvanced);
        group->setLabelAndHint(kGroupAdvancedLabel);
        group->setOpen(false);
        if (page) {
            page->addChild(*group);
        }

        GroupParamDescriptor* topgroup = group;
        {
            GroupParamDescriptor* group = desc.defineGroupParam(kGroupRaw);
            if (group) {
                group->setLabelAndHint(kGroupRawLabel);
                group->setOpen(false);
                if (topgroup) {
                    group->setParent(*topgroup);
                }
                if (page) {
                    page->addChild(*group);
                }
            }

            {
                BooleanParamDescriptor* param = desc.defineBooleanParam(kParamRawAutoBright);
                param->setLabelAndHint(kParamRawAutoBrightLabel);
                param->setDefault(false);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                BooleanParamDescriptor* param = desc.defineBooleanParam(kParamRawUseCameraWB);
                param->setLabelAndHint(kParamRawUseCameraWBLabel);
                param->setDefault(true);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                DoubleParamDescriptor* param = desc.defineDoubleParam(kParamRawAdjustMaximumThr);
                param->setLabelAndHint(kParamRawAdjustMaximumThrLabel);
                param->setRange(0., 1.);
                param->setDisplayRange(0., 1.);
                param->setDefault(0.);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                IntParamDescriptor* param = desc.defineIntParam(kParamRawUserSat);
                param->setLabelAndHint(kParamRawUserSatLabel);
                param->setRange(0, INT_MAX);
                param->setDisplayRange(0, 0xffff);
                param->setDefault(0);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamRawOutputColor);
                param->setLabelAndHint(kParamRawOutputColorLabel);
                assert(param->getNOptions() == eRawOutputColorRaw);
                param->appendOption(kParamRawOutputColorRaw);
                assert(param->getNOptions() == eRawOutputColorSRGB);
                param->appendOption(kParamRawOutputColorSRGB);
                assert(param->getNOptions() == eRawOutputColorAdobe);
                param->appendOption(kParamRawOutputColorAdobe);
                assert(param->getNOptions() == eRawOutputColorWide);
                param->appendOption(kParamRawOutputColorWide);
                assert(param->getNOptions() == eRawOutputColorProPhoto);
                param->appendOption(kParamRawOutputColorProPhoto);
                assert(param->getNOptions() == eRawOutputColorXYZ);
                param->appendOption(kParamRawOutputColorXYZ);
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 18, 0)
                assert(param->getNOptions() == eRawOutputColorACES);
                param->appendOption(kParamRawOutputColorACES);
#endif
                param->setDefault(eRawOutputColorSRGB);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamRawUseCameraMatrix);
                param->setLabelAndHint(kParamRawUseCameraMatrixLabel);
                assert(param->getNOptions() == eRawUseCameraMatrixNone);
                param->appendOption(kParamRawUseCameraMatrixNone);
                assert(param->getNOptions() == eRawUseCameraMatrixDefault);
                param->appendOption(kParamRawUseCameraMatrixDefault);
                assert(param->getNOptions() == eRawUseCameraMatrixForce);
                param->appendOption(kParamRawUseCameraMatrixForce);
                param->setDefault(eRawUseCameraMatrixDefault);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamRawHighlightMode);
                param->setLabelAndHint(kParamRawHighlightModeLabel);
                assert(param->getNOptions() == eRawHighlightModeClip);
                param->appendOption(kParamRawHighlightModeClip);
                assert(param->getNOptions() == eRawHighlightModeUnclip);
                param->appendOption(kParamRawHighlightModeUnclip);
                assert(param->getNOptions() == eRawHighlightModeBlend);
                param->appendOption(kParamRawHighlightModeBlend);
                assert(param->getNOptions() == eRawHighlightModeRebuild);
                param->appendOption(kParamRawHighlightModeRebuild);
                param->setAnimates(false);
                param->setLayoutHint(eLayoutHintNoNewLine);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                IntParamDescriptor* param = desc.defineIntParam(kParamRawHighlightRebuildLevel);
                param->setLabelAndHint(kParamRawHighlightRebuildLevelLabel);
                param->setRange(0, 9 - (int)eRawHighlightModeRebuild);
                param->setDisplayRange(0, 9 - (int)eRawHighlightModeRebuild);
                param->setDefault(5 - (int)eRawHighlightModeRebuild);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                DoubleParamDescriptor* param = desc.defineDoubleParam(kParamRawExposure);
                param->setLabelAndHint(kParamRawExposureLabel);
                param->setRange(0.25, 8.);
                param->setDisplayRange(0.25, 8.);
                param->setDefault(1.);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamRawDemosaic);
                param->setLabelAndHint(kParamRawDemosaicLabel);
                bool libraw_gpl2, libraw_gpl3;
                getLibRawDemosaicCaps(&libraw_gpl2, &libraw_gpl3);
                // AHD-Mod, AFD, VCD, Mixed, LMMSE are GPL2, AMaZE is GPL3
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicNone);
                param->appendOption(kParamRawDemosaicNone);
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicLinear);
                param->appendOption(kParamRawDemosaicLinear);
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicVNG);
                param->appendOption(kParamRawDemosaicVNG);
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicPPG);
                param->appendOption(kParamRawDemosaicPPG);
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicAHD);
                param->appendOption(kParamRawDemosaicAHD);
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicDCB);
                param->appendOption(kParamRawDemosaicDCB);
                if (libraw_gpl2) {
                    assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicAHDMod);
                    param->appendOption(kParamRawDemosaicAHDMod);
                    assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicAFD);
                    param->appendOption(kParamRawDemosaicAFD);
                    assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicVCD);
                    param->appendOption(kParamRawDemosaicVCD);
                    assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicMixed);
                    param->appendOption(kParamRawDemosaicMixed);
                    assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicLMMSE);
                    param->appendOption(kParamRawDemosaicLMMSE);
                }
                if (libraw_gpl3) {
                    assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicAMaZE);
                    param->appendOption(kParamRawDemosaicAMaZE);
                }
#if LIBRAW_VERSION >= LIBRAW_MAKE_VERSION(0, 16, 0)
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicDHT);
                param->appendOption(kParamRawDemosaicDHT);
                assert(libraw_demosaic[param->getNOptions()] == eRawDemosaicAAHD);
                param->appendOption(kParamRawDemosaicAAHD);
#endif
                param->setDefault(eRawDemosaicAHD);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
            {
                Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamRawAber);
                param->setLabelAndHint(kParamRawAberLabel);
                param->setRange(0.5, 0.5, 1.5, 1.5);
                param->setDisplayRange(0.99, 0.99, 1.01, 1.01);
                param->setDefault(1., 1.);
                param->setAnimates(false);
                if (group) {
                    param->setParent(*group);
                }
                if (page) {
                    page->addChild(*param);
                }
            }
        }
    }

    if (gHostSupportsMultiPlane && gHostSupportsDynamicChoices) {
        {
            ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamChannelOutputLayer);
            param->setLabel(kParamChannelOutputLayerLabel);
            param->setHint(kParamChannelOutputLayerHint);
            param->setEvaluateOnChange(false);
            param->setIsPersistent(false);
            param->setAnimates(false);
            if (page) {
                page->addChild(*param);
            }
        }
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamChannelOutputLayerChoice);
            param->setLabel(kParamChannelOutputLayerChoice);
            param->setIsSecretAndDisabled(true);
            param->setAnimates(false);
            desc.addClipPreferencesSlaveParam(*param);
            if (page) {
                page->addChild(*param);
            }
        }
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamAvailableViews);
            param->setLabel(kParamAvailableViewsLabel);
            param->setHint(kParamAvailableViewsHint);
            param->setAnimates(false);
            param->setIsSecretAndDisabled(true);
            param->setEvaluateOnChange(false);
            param->setIsPersistent(false);
            if (page) {
                page->addChild(*param);
            }
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamEdgePixels);
        param->setLabelAndHint(kParamEdgePixelsLabel);
        param->setAnimates(false);
        assert(param->getNOptions() == eEdgePixelsAuto);
        param->appendOption(kParamEdgePixelsAuto);
        assert(param->getNOptions() == eEdgePixelsEdgeDetect);
        param->appendOption(kParamEdgePixelsEdgeDetect);
        assert(param->getNOptions() == eEdgePixelsRepeat);
        param->appendOption(kParamEdgePixelsRepeat);
        assert(param->getNOptions() == eEdgePixelsBlack);
        param->appendOption(kParamEdgePixelsBlack);
        param->setDefault((int)eEdgePixelsAuto);
        param->setLayoutHint(eLayoutHintNoNewLine);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamOffsetNegativeDisplayWindow);
        param->setLabel(kParamOffsetNegativeDisplayWindowLabel);
        param->setHint(kParamOffsetNegativeDisplayWindowHint);
        param->setDefault(true);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintDivider);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamLibraryInfo);
        param->setLabelAndHint(kParamLibraryInfoLabel);
        if (page) {
            page->addChild(*param);
        }
    }

    GenericReaderDescribeInContextEnd(desc, context, page, "scene_linear", "scene_linear");
} // ReadOIIOPluginFactory::describeInContext

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
ReadOIIOPluginFactory::createInstance(OfxImageEffectHandle handle,
                                      ContextEnum /*context*/)
{
    ReadOIIOPlugin* ret = new ReadOIIOPlugin(handle, _extensions);

    ret->restoreStateFromParams();

    return ret;
}

static ReadOIIOPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

    OFXS_NAMESPACE_ANONYMOUS_EXIT
