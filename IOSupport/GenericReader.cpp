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
 * OFX GenericReader plugin.
 * A base class for all OpenFX-based decoders.
 */

#include "GenericReader.h"

#include <algorithm>
#include <cfloat> // DBL_MAX
#include <climits>
#include <cmath>
#include <cstddef> // std::ptrdiff_t
#include <cstring> // memset
#include <iterator> // std::advance
#include <memory>
#include <new> // std::bad_alloc
#include <sstream>
#include <system_error>
#include <vector>
#ifndef _WIN32
#include <pthread.h>
#endif
#if defined(DEBUG) && defined(DEBUG_READER)
#include <cstdio>
#define DBG(x) x
#else
#define DBG(x) (void)0
#endif

#include "ofxsCoords.h"
#include "ofxsCopier.h"
#include "ofxsFileOpen.h"
#include "ofxsLog.h"
#include "ofxsMacros.h"

#ifdef OFX_EXTENSIONS_TUTTLE
#include <tuttle/ofxReadWrite.h>
#endif
#include <ofxNatron.h>
#include <ofxsMultiPlane.h>

#include "SequenceParsing/SequenceParsing.h"
#ifdef OFX_IO_USING_OCIO
#include "GenericOCIO.h"
#endif
#include "IOUtility.h"

// The frame params (first/last frame, ...) get their INT_MIN..INT_MAX range and display range from the Support
// library (the spec's defaults): frames may be negative, and Resolve clamps a param without a range to 0..0. A stale
// openfx would silently lose them, also in the per-plug-in Makefile builds, which compile no guarded SupportExt file.
#ifndef OFXS_SUPPORT_HAS_SPEC_PARAM_DEFAULTS
#error "openfx Support must set the spec's default param ranges (OFXS_SUPPORT_HAS_SPEC_PARAM_DEFAULTS, openfx 96a53ac4)"
#endif

#ifdef OFX_IO_USING_OCIO
namespace OCIO = OCIO_NAMESPACE;
#endif

using std::make_pair;
using std::size_t;
using std::string;
using std::stringstream;
using std::vector;

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

#define kPluginGrouping "Image/Readers"

// in the Reader context, the script name must be kOfxImageEffectFileParamName, @see kOfxImageEffectContextReader
#define kParamFilename kOfxImageEffectFileParamName
#define kParamFilenameLabel "File"
#define kParamFilenameHint "The input image sequence/video stream file(s)."

#define kParamProxy kOfxImageEffectProxyParamName
#define kParamProxyLabel "Proxy File"
#define kParamProxyHint                                                                                   \
    "Filename of the proxy images. They will be used instead of the images read from the File parameter " \
    "when the proxy mode (downscaling of the images) is activated."

#define kParamProxyThreshold "proxyThreshold"
#define kParamProxyThresholdLabel "Proxy threshold"
#define kParamProxyThresholdHint                                                                   \
    "The scale of the proxy images. By default it will be automatically computed out of the "      \
    "images headers when you set the proxy file(s) path. When the render scale (proxy) is set to " \
    "a scale lower or equal to this value then the proxy image files will be used instead of the " \
    "original images. You can change this parameter by checking the \"Custom scale\" checkbox "    \
    "so that you can change the scale at which the proxy images should be used instead of the original images."

#define kParamOriginalProxyScale "originalProxyScale"
#define kParamOriginalProxyScaleLabel "Original Proxy Scale"
#define kParamOriginalProxyScaleHint \
    "The original scale of the proxy image."

#define kParamCustomProxyScale "customProxyScale"
#define kParamCustomProxyScaleLabel "Custom Proxy Scale"
#define kParamCustomProxyScaleHint \
    "Check to enable the Proxy scale edition."

#define kParamOnMissingFrame "onMissingFrame"
#define kParamOnMissingFrameLabel "On Missing Frame"
#define kParamOnMissingFrameHint \
    "What to do when a frame is missing from the sequence/stream."

#define kParamFrameMode "frameMode"
#define kParamFrameModeLabel "Frame Mode"
enum FrameModeEnum {
    eFrameModeStartingTime,
    eFrameModeTimeOffset,
};

#define kParamFrameModeOptionStartingTime "Starting Time", "Set at what output frame the first sequence frame is output. The sequence frame designated by the firstFrame parameter is output at frame timeOffset.", "startingTime"
#define kParamFrameModeOptionTimeOffset "Time Offset", "Set an offset to be applied as a number of frames. The sequence frame designated by the firstFrame parameter is output at frame firstFrame+timeOffset.", "timeOffset"

#define kParamTimeOffset "timeOffset"
#define kParamTimeOffsetLabel "Time Offset"
#define kParamTimeOffsetHint \
    "Offset applied to the sequence in time units (i.e. frames)."

#define kParamStartingTime "startingTime"
#define kParamStartingTimeLabel "Starting Time"
#define kParamStartingTimeHint \
    "At what time (on the timeline) should this sequence/video start."

#define kParamOriginalFrameRange "originalFrameRange"
#define kParamOriginalFrameRangeLabel "Original Range"

#define kParamFirstFrame "firstFrame"
#define kParamFirstFrameLabel "First Frame"
#define kParamFirstFrameHint                                                                         \
    "The first frame number to read from this image sequence or video file. This cannot be less "    \
    " than the first frame of the image sequence or video file, and cannot be greater than the last" \
    " frame of the image sequence or video file. The first frame of a video file is numbered 1. "    \
    "If startingTime is 1 or timeOffset is 0, this is also the first output frame."

#define kParamLastFrame "lastFrame"
#define kParamLastFrameLabel "Last Frame"
#define kParamLastFrameHint                                                                          \
    "The last frame number to read from this image sequence or video file. This cannot be less "     \
    "than the first frame of the image sequence or video file, and cannot be greater than the last " \
    "frame of the image sequence or video file. The first frame of a video file is numbered 1. "     \
    "If startingTime is 1 or timeOffset is 0, this is also the last output frame."

#define kParamBefore "before"
#define kParamBeforeLabel "Before"
#define kParamBeforeHint \
    "What to do before the first frame of the sequence."

#define kParamAfter "after"
#define kParamAfterLabel "After"
#define kParamAfterHint \
    "What to do after the last frame of the sequence."

#define kParamTimeDomainUserEdited "timeDomainUserEdited"

#define kReaderOptionHold "Hold", "While before the sequence, load the first frame.", "hold"
#define kReaderOptionLoop "Loop", "Repeat the sequence before the first frame", "loop"
#define kReaderOptionBounce "Bounce", "Repeat the sequence in reverse before the first frame", "bounce"
#define kReaderOptionBlack "Black", "Render a black image", "black"
#define kReaderOptionError "Error", "Report an error", "error"
#define kReaderOptionPrevious "Hold previous", "Try to load the previous frame in the sequence/stream, if any.", "previous"
#define kReaderOptionNext "Load next", "Try to load the next frame in the sequence/stream, if any.", "next"
#define kReaderOptionNearest "Load nearest", "Try to load the nearest frame in the sequence/stream, if any.", "nearest"

#define kParamFilePremult "filePremult"
#define kParamFilePremultLabel "File Premult"
#define kParamFilePremultHint                                                                                                                               \
    "The image file being read is considered to have this premultiplication state.\n"                                                                       \
    "To get UnPremultiplied (or \"unassociated alpha\") images, set the \"Output Premult\" parameter to Unpremultiplied. \n"                                \
    "By default the value should be correctly be guessed by the image file, but this parameter can be edited if the metadatas inside the file are wrong.\n" \
    "- Opaque means that the alpha channel of the color plane is considered to be 1 (one), and it is not taken into account in colorspace conversion.\n"    \
    "- Premultiplied, red, green and blue channels are divided by the alpha channel "                                                                       \
    "before applying the colorspace conversion, and re-multiplied by alpha after colorspace conversion.\n"                                                  \
    "- UnPremultiplied, means that red, green and blue channels are not modified "                                                                          \
    "before applying the colorspace conversion, and are multiplied by alpha after colorspace conversion.\n"                                                 \
    "This is set automatically from the image file and the plugin, but can be adjusted if this information is wrong in the file metadata.\n"                \
    "RGB images can only be Opaque, and Alpha images can only be Premultiplied (the value of this parameter doesn't matter)."
#define kParamFilePremultOptionOpaqueHint \
    "The image is opaque and so has no premultiplication state, as if the alpha component in all pixels were set to the white point.", "opaque"
#define kParamFilePremultOptionPreMultipliedHint \
    "The image is premultiplied by its alpha (also called \"associated alpha\").", "premult"
#define kParamFilePremultOptionUnPreMultipliedHint \
    "The image is unpremultiplied (also called \"unassociated alpha\").", "unpremult"

#define kParamOutputPremult "outputPremult"
#define kParamOutputPremultLabel "Output Premult"
#define kParamOutputPremultHint "The alpha premultiplication in output of this node will have this state."

#define kParamOutputComponents "outputComponents"
#define kParamOutputComponentsLabel "Output Components"
#define kParamOutputComponentsHint "What type of components this effect should output when the main color plane is requested." \
                                   " For the Read node it will map (in number of components) the Output Layer choice to these."
#define kParamOutputComponentsOptionRGBA "RGBA"
#define kParamOutputComponentsOptionRGB "RGB"
#define kParamOutputComponentsOptionXY "RG"
#define kParamOutputComponentsOptionAlpha "Alpha"

#define kParamInputSpaceLabel "File Colorspace"

#define kParamFrameRate "frameRate"
#define kParamFrameRateLabel "Frame rate"
#define kParamFrameRateHint "By default this value is guessed from the file. You can override it by checking the Custom fps parameter. " \
                            "The value of this parameter is what will be visible by the effects down-stream."

#define kParamCustomFps "customFps"
#define kParamCustomFpsLabel "Custom FPS"
#define kParamCustomFpsHint "If checked, you can freely force the value of the frame rate parameter. The frame-rate is just the meta-data that will be passed " \
                            "downstream to the graph, no retime will actually take place."

#define kParamGuessedParams "ParamExistingInstance" // was guessParamsFromFilename already successfully called once on this instance

#ifdef OFX_IO_USING_OCIO
#define kParamInputSpaceSet "ocioInputSpaceSet" // was the input colorspace set by user?
#endif

#define MISSING_FRAME_NEAREST_RANGE 100

#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1 // GenericReader supports render scale: it scales images and uses proxy image when applicable

static bool gHostIsNatron = false;
static bool gHostSupportsRGBA = false;
static bool gHostSupportsRGB = false;
static bool gHostSupportsXY = false;
static bool gHostSupportsAlpha = false;
static const char*
premultString(PreMultiplicationEnum e)
{
    switch (e) {
    case eImageOpaque:

        return "Opaque";
    case eImagePreMultiplied:

        return "PreMultiplied";
    case eImageUnPreMultiplied:

        return "UnPreMultiplied";
    }

    return "Unknown";
}

GenericReaderPlugin::GenericReaderPlugin(OfxImageEffectHandle handle,
                                         const std::vector<string>& extensions,
                                         bool supportsRGBA,
                                         bool supportsRGB,
                                         bool supportsXY,
                                         bool supportsAlpha,
                                         bool supportsTiles,
                                         bool isMultiPlanar)
    : ImageEffect(handle)
    , _missingFrameParam(NULL)
#ifdef OFX_IO_USING_OCIO
    , _inputSpaceSet(NULL)
    , _ocio(new GenericOCIO(this))
#endif
    , _syncClip(NULL)
    , _outputClip(NULL)
    , _fileParam(NULL)
    , _firstFrame(NULL)
    , _timeOffset(NULL)
    , _startingTime(NULL)
    , _originalFrameRange(NULL)
    , _proxyFileParam(NULL)
    , _proxyThreshold(NULL)
    , _originalProxyScale(NULL)
    , _enableCustomScale(NULL)
    , _beforeFirst(NULL)
    , _lastFrame(NULL)
    , _afterLast(NULL)
    , _frameMode(NULL)
    , _outputComponents(NULL)
    , _filePremult(NULL)
    , _outputPremult(NULL)
    , _timeDomainUserSet(NULL)
    , _customFPS(NULL)
    , _fps(NULL)
    , _sublabel(NULL)
    , _guessedParams(NULL)
    , _extensions(extensions)
    , _supportsRGBA(supportsRGBA)
    , _supportsRGB(supportsRGB)
    , _supportsXY(supportsXY)
    , _supportsAlpha(supportsAlpha)
    , _supportsTiles(supportsTiles)
    , _isMultiPlanar(isMultiPlanar)
{
    _syncClip = fetchClip(kOfxImageEffectSimpleSourceClipName);
    _outputClip = fetchClip(kOfxImageEffectOutputClipName);

    _fileParam = fetchStringParam(kParamFilename);
    _proxyFileParam = fetchStringParam(kParamProxy);
    _proxyThreshold = fetchDouble2DParam(kParamProxyThreshold);
    _originalProxyScale = fetchDouble2DParam(kParamOriginalProxyScale);
    _enableCustomScale = fetchBooleanParam(kParamCustomProxyScale);
    _missingFrameParam = fetchChoiceParam(kParamOnMissingFrame);
    _firstFrame = fetchIntParam(kParamFirstFrame);
    _beforeFirst = fetchChoiceParam(kParamBefore);
    _lastFrame = fetchIntParam(kParamLastFrame);
    _afterLast = fetchChoiceParam(kParamAfter);
    _frameMode = fetchChoiceParam(kParamFrameMode);
    _timeOffset = fetchIntParam(kParamTimeOffset);
    _startingTime = fetchIntParam(kParamStartingTime);
    _originalFrameRange = fetchInt2DParam(kParamOriginalFrameRange);
    _timeDomainUserSet = fetchBooleanParam(kParamTimeDomainUserEdited);
    _outputComponents = fetchChoiceParam(kParamOutputComponents);
    _filePremult = fetchChoiceParam(kParamFilePremult);
    _outputPremult = fetchChoiceParam(kParamOutputPremult);
    _customFPS = fetchBooleanParam(kParamCustomFps);
    _fps = fetchDoubleParam(kParamFrameRate);
    if (gHostIsNatron) {
        _sublabel = fetchStringParam(kNatronOfxParamStringSublabelName);
        assert(_sublabel);
    }
    _guessedParams = fetchBooleanParam(kParamGuessedParams);

#ifdef OFX_IO_USING_OCIO
    _inputSpaceSet = fetchBooleanParam(kParamInputSpaceSet);
#endif

    // must be in sync with GenericReaderDescribeInContextBegin
    int i = 0;

    if (gHostSupportsRGBA && supportsRGBA) {
        _outputComponentsTable[i] = ePixelComponentRGBA;
        ++i;
    }
    if (gHostSupportsRGB && supportsRGB) {
        _outputComponentsTable[i] = ePixelComponentRGB;
        ++i;
    }
    if (gHostSupportsXY && supportsXY) {
        _outputComponentsTable[i] = ePixelComponentXY;
        ++i;
    }

    if (gHostSupportsAlpha && supportsAlpha) {
        _outputComponentsTable[i] = ePixelComponentAlpha;
        ++i;
    }
    _outputComponentsTable[i] = ePixelComponentNone;
}

GenericReaderPlugin::~GenericReaderPlugin()
{
}

void
GenericReaderPlugin::refreshSubLabel(OfxTime time)
{
    assert(_sublabel);
    double sequenceTime;
    GetSequenceTimeRetEnum getTimeRet = getSequenceTime(time, &sequenceTime);
    if ((getTimeRet == eGetSequenceTimeWithinSequence) || (getTimeRet == eGetSequenceTimeBeforeSequence) || (getTimeRet == eGetSequenceTimeAfterSequence)) {
        string filename;
        GetFilenameRetCodeEnum getFileNameRet = getFilenameAtSequenceTime(time, sequenceTime, false, &filename, NULL);
        if (getFileNameRet == eGetFileNameReturnedFullRes) {
            _sublabel->setValue(basename(filename));
        } else {
            _sublabel->setValue("");
        }
    } else {
        _sublabel->setValue("");
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
GenericReaderPlugin::restoreStateFromParams()
{
    int frameMode_i;

    _frameMode->getValue(frameMode_i);
    FrameModeEnum frameMode = FrameModeEnum(frameMode_i);
    switch (frameMode) {
    case eFrameModeStartingTime: // starting frame
        _startingTime->setIsSecretAndDisabled(false);
        _timeOffset->setIsSecretAndDisabled(true);
        break;
    case eFrameModeTimeOffset: // time offset
        _startingTime->setIsSecretAndDisabled(true);
        _timeOffset->setIsSecretAndDisabled(false);
        break;
    }

    /// Detect the scale of the proxy.
    string proxyFile;
    _proxyFileParam->getValue(proxyFile);
    if (!proxyFile.empty()) {
        _proxyThreshold->setIsSecretAndDisabled(false);
        _enableCustomScale->setIsSecretAndDisabled(false);
    } else {
        _proxyThreshold->setIsSecretAndDisabled(true);
        _enableCustomScale->setIsSecretAndDisabled(true);
    }

    bool customFps = _customFPS->getValue();
    _fps->setEnabled(customFps);

    if (gHostIsNatron) {
        refreshSubLabel(timeLineGetTime());
    }
}

bool
GenericReaderPlugin::getTimeDomain(OfxRangeD& range)
{
    OfxRangeI rangeI;
    bool ret = getSequenceTimeDomainInternal(rangeI, false);

    if (ret) {
        /// these are the value held by the "First frame" and "Last frame" param
        OfxRangeI sequenceTimeDomain;
        sequenceTimeDomain.min = _firstFrame->getValue();
        sequenceTimeDomain.max = _lastFrame->getValue();
        int startingTime = _startingTime->getValue();
        timeDomainFromSequenceTimeDomain(sequenceTimeDomain, startingTime, &rangeI);
        range.min = rangeI.min;
        range.max = rangeI.max;
    }

    return ret;
}

bool
GenericReaderPlugin::getSequenceTimeDomainInternal(OfxRangeI& range,
                                                   bool canSetOriginalFrameRange)
{
    ////first-off check if the original frame range param has valid values, in which
    /// case we don't bother calculating the frame range
    int originalMin, originalMax;

    _originalFrameRange->getValue(originalMin, originalMax);

    // test if the host (probably Natron) set kParamOriginalFrameRange
    if ((originalMin != INT_MIN) && (originalMax != INT_MAX)) {
        range.min = originalMin;
        range.max = originalMax;

        return true;
    }

    string filename;
    _fileParam->getValue(filename);

    /// call the plugin specific getTimeDomain (if it is a video-stream , it is responsible to
    /// find-out the time domain. If this function return false, it means this is an image sequence
    /// in which case our sequence parser will give us the sequence range
    bool gotRange = getSequenceTimeDomain(filename, range);

    if (!gotRange) {
        // POOR MAN's solution: try to guess the range

        SequenceParsing::FileNameContent content(filename);
        string pattern;
        /// We try to match all the files in the same directory that match the pattern with the frame number
        /// assumed to be in the last part of the filename. This is a harsh assumption but we can't just verify
        /// everything as it would take too much time.
        string noStr;
        int nbFrameNumbers = content.getPotentialFrameNumbersCount();
        content.getNumberByIndex(nbFrameNumbers - 1, &noStr);

        int numHashes = content.getLeadingZeroes();
        string noStrWithoutZeroes;
        for (std::size_t i = 0; i < noStr.size(); ++i) {
            if ((noStr[i] == '0') && noStrWithoutZeroes.empty()) {
                continue;
            }
            noStrWithoutZeroes.push_back(noStr[i]);
        }

        if ((int)noStr.size() > numHashes) {
            numHashes += noStrWithoutZeroes.size();
        } else {
            numHashes = 1;
        }
        content.generatePatternWithFrameNumberAtIndex(nbFrameNumbers - 1,
                                                      numHashes,
                                                      &pattern);

        SequenceParsing::SequenceFromPattern sequenceFromFiles;
        SequenceParsing::filesListFromPattern_slow(pattern, &sequenceFromFiles);

        range.min = range.max = 1;
        if (sequenceFromFiles.size() > 1) {
            range.min = sequenceFromFiles.begin()->first;
            range.max = sequenceFromFiles.rbegin()->first;
        }
    }

    //// From http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#SettingParams
    //    Plugins are free to set parameters in limited set of circumstances, typically relating to user interaction. You can only set parameters in the following actions passed to the plug-in's main entry function...
    //
    //    The Create Instance Action
    //    The The Begin Instance Changed Action
    //    The The Instance Changed Action
    //    The The End Instance Changed Action
    //    The The Sync Private Data Action
    if (!filename.empty() && canSetOriginalFrameRange) {
        _originalFrameRange->setValue(range.min, range.max);
    }

    return true;
} // GenericReaderPlugin::getSequenceTimeDomainInternal

void
GenericReaderPlugin::timeDomainFromSequenceTimeDomain(const OfxRangeI& sequenceTimeDomain,
                                                      int startingTime,
                                                      OfxRangeI* timeDomain)
{
    timeDomain->min = startingTime;
    timeDomain->max = startingTime + (sequenceTimeDomain.max - sequenceTimeDomain.min);
}

void
GenericReaderPlugin::syncTimeParams()
{
    // only set a param if it changes, so that the instance is not modified for nothing
    const int firstFrame = _firstFrame->getValue();
    if (static_cast<FrameModeEnum>(_frameMode->getValue()) == eFrameModeTimeOffset) {
        // the user sees and sets the time offset: keep it (#260)
        const int startingTime = firstFrame + _timeOffset->getValue();
        if (_startingTime->getValue() != startingTime) {
            _startingTime->setValue(startingTime);
        }
    } else {
        const int offset = _startingTime->getValue() - firstFrame;
        if (_timeOffset->getValue() != offset) {
            _timeOffset->setValue(offset);
        }
    }
}

GenericReaderPlugin::GetSequenceTimeRetEnum
GenericReaderPlugin::getSequenceTimeBefore(const OfxRangeI& sequenceTimeDomain,
                                           BeforeAfterEnum beforeChoice,
                                           double* sequenceTime) const
{
    /// get the offset from the starting time of the sequence in case we bounce or loop. In 64 bits: the time comes
    /// from the host, and the frame range params go from INT_MIN to INT_MAX, so the offset and the number of frames
    /// may be out of the range of int (#184).
    long long timeOffsetFromStart = (long long)clampToInt(*sequenceTime) - sequenceTimeDomain.min;
    const long long seqFrames = (long long)sequenceTimeDomain.max - sequenceTimeDomain.min + 1;

    switch (beforeChoice) {
    case eBeforeAfterHold: // hold
        *sequenceTime = sequenceTimeDomain.min;

        return eGetSequenceTimeBeforeSequence;

    case eBeforeAfterLoop: { // loop
        if (seqFrames <= 1) {
            *sequenceTime = sequenceTimeDomain.min;
        } else {
            // positive modulo
            timeOffsetFromStart = (timeOffsetFromStart % seqFrames + seqFrames) % seqFrames;
            *sequenceTime = sequenceTimeDomain.min + timeOffsetFromStart;
        }

        return eGetSequenceTimeBeforeSequence;
    }
    case eBeforeAfterBounce: { // bounce
        if (seqFrames <= 1) {
            *sequenceTime = sequenceTimeDomain.min;
        } else {
            // number of frames in a loop
            const long long loopFrames = seqFrames * 2 - 2;
            // positive modulo
            timeOffsetFromStart = (timeOffsetFromStart % loopFrames + loopFrames) % loopFrames;
            // bounce
            if (timeOffsetFromStart >= seqFrames) {
                timeOffsetFromStart = loopFrames - timeOffsetFromStart;
            }
            *sequenceTime = sequenceTimeDomain.min + timeOffsetFromStart;
        }

        return eGetSequenceTimeBeforeSequence;
    }
    case eBeforeAfterBlack: // black
        return eGetSequenceTimeBlack;

    case eBeforeAfterError: // error

        // setPersistentMessage(Message::eMessageError, "", "Out of frame range");
        return eGetSequenceTimeError;
    }

    return eGetSequenceTimeError;
}

GenericReaderPlugin::GetSequenceTimeRetEnum
GenericReaderPlugin::getSequenceTimeAfter(const OfxRangeI& sequenceTimeDomain,
                                          BeforeAfterEnum afterChoice,
                                          double* sequenceTime) const
{
    /// get the offset from the starting time of the sequence in case we bounce or loop, in 64 bits (see
    /// getSequenceTimeBefore())
    long long timeOffsetFromStart = (long long)clampToInt(*sequenceTime) - sequenceTimeDomain.min;
    const long long seqFrames = (long long)sequenceTimeDomain.max - sequenceTimeDomain.min + 1;

    switch (afterChoice) {
    case eBeforeAfterHold: { // hold
        *sequenceTime = sequenceTimeDomain.max;

        return eGetSequenceTimeAfterSequence;
    }
    case eBeforeAfterLoop: { // loop
        if (seqFrames <= 1) {
            *sequenceTime = sequenceTimeDomain.min;
        } else {
            // positive modulo
            timeOffsetFromStart = (timeOffsetFromStart % seqFrames + seqFrames) % seqFrames;
            *sequenceTime = sequenceTimeDomain.min + timeOffsetFromStart;
        }

        return eGetSequenceTimeAfterSequence;
    }
    case eBeforeAfterBounce: { // bounce
        if (seqFrames <= 1) {
            *sequenceTime = sequenceTimeDomain.min;
        } else {
            // number of frames in a loop
            const long long loopFrames = seqFrames * 2 - 2;
            // positive modulo
            timeOffsetFromStart = (timeOffsetFromStart % loopFrames + loopFrames) % loopFrames;
            // bounce
            if (timeOffsetFromStart >= seqFrames) {
                timeOffsetFromStart = loopFrames - timeOffsetFromStart;
            }
            *sequenceTime = sequenceTimeDomain.min + timeOffsetFromStart;
        }

        return eGetSequenceTimeAfterSequence;
    }
    case eBeforeAfterBlack: { // black
        return eGetSequenceTimeBlack;
    }
    case eBeforeAfterError: { // error
        // setPersistentMessage(Message::eMessageError, "", "Out of frame range");

        return eGetSequenceTimeError;
    }
    }

    return eGetSequenceTimeError;
} // GenericReaderPlugin::getSequenceTimeAfter

#if 0
GenericReaderPlugin::GetSequenceTimeRetEnum
GenericReaderPlugin::getSequenceTimeHold(double t,
                                         double *sequenceTime) const
{
    int timeOffset;

    _timeOffset->getValue(timeOffset);


    ///get the time sequence domain
    OfxRangeI sequenceTimeDomain;
    _firstFrame->getValue(sequenceTimeDomain.min);
    _lastFrame->getValue(sequenceTimeDomain.max);


    ///the return value
    *sequenceTime = t - timeOffset;

    ///if the time given is before the sequence
    if ( (sequenceTimeDomain.min <= *sequenceTime) && (*sequenceTime <= sequenceTimeDomain.max) ) {
        return eGetSequenceTimeWithinSequence;
    } else if (*sequenceTime < sequenceTimeDomain.min) {
        return getSequenceTimeBefore(sequenceTimeDomain, eBeforeAfterHold, sequenceTime);
    } else {
        assert(*sequenceTime > sequenceTimeDomain.max); ///the time given is after the sequence
        return getSequenceTimeAfter(sequenceTimeDomain, eBeforeAfterHold, sequenceTime);
    }

    return eGetSequenceTimeError;
}
#endif

GenericReaderPlugin::GetSequenceTimeRetEnum
GenericReaderPlugin::getSequenceTime(double t,
                                     double* sequenceTime) const
{
    int timeOffset;

    _timeOffset->getValue(timeOffset);

    /// get the time sequence domain
    OfxRangeI sequenceTimeDomain;
    _firstFrame->getValue(sequenceTimeDomain.min);
    _lastFrame->getValue(sequenceTimeDomain.max);

    /// the return value
    *sequenceTime = t - timeOffset;

    /// if the time given is before the sequence
    if ((sequenceTimeDomain.min <= *sequenceTime) && (*sequenceTime <= sequenceTimeDomain.max)) {
        return eGetSequenceTimeWithinSequence;
    } else if (*sequenceTime < sequenceTimeDomain.min) {
        /////if we're before the first frame
        int beforeChoice_i;
        _beforeFirst->getValue(beforeChoice_i);
        BeforeAfterEnum beforeChoice = BeforeAfterEnum(beforeChoice_i);

        return getSequenceTimeBefore(sequenceTimeDomain, beforeChoice, sequenceTime);
    } else {
        assert(*sequenceTime > sequenceTimeDomain.max); /// the time given is after the sequence
        /////if we're after the last frame
        int afterChoice_i;
        _afterLast->getValue(afterChoice_i);
        BeforeAfterEnum afterChoice = BeforeAfterEnum(afterChoice_i);

        return getSequenceTimeAfter(sequenceTimeDomain, afterChoice, sequenceTime);
    }

    return eGetSequenceTimeError;
}

// Whether the frame's file exists (#414): OFX::exists_utf8() tests existence on every platform. An existing file that
// can not be read is then not a missing frame (kParamOnMissingFrame): its decode fails with the reader's error.
static bool
checkIfFileExists(const string& path)
{
    return OFX::exists_utf8(path.c_str());
}

GenericReaderPlugin::GetFilenameRetCodeEnum
GenericReaderPlugin::getFilenameAtSequenceTime(double time,
                                               double sequenceTime,
                                               bool checkForExistingFile,
                                               string* filename,
                                               string* proxyFilename) const
{
    GetFilenameRetCodeEnum ret;
    // kParamOnMissingFrame animates: evaluate it at the host time, not at the sequence time
    const MissingEnum missingFrame = (MissingEnum)_missingFrameParam->getValueAtTime(time);
    string filename0;
    bool filenameGood = true;
    int offset = 0;

    sequenceTime = std::floor(sequenceTime + 0.5); // round to the nearest frame

    const bool searchOtherFrame = ((missingFrame == eMissingPrevious) || (missingFrame == eMissingNearest) || (missingFrame == eMissingNext) || (missingFrame == eMissingNearest));
    do {
        _fileParam->getValueAtTime(sequenceTime + offset, *filename); // the time in _fileParam is the *file* time
        if (offset == 0) {
            filename0 = *filename; // for error reporting
        }
        if (filename->empty()) {
            // filenameGood = false;
            return eGetFileNameBlack; // if filename is empty, just return a black frame. this happens eg when the plugin is created
        } else {
            if (checkForExistingFile) {
                filenameGood = checkIfFileExists(*filename);
            }
        }
        if (filenameGood) {
            ret = eGetFileNameReturnedFullRes;
            // now, try the proxy file
            if (proxyFilename) {
                string proxyFileName;
                bool proxyGood = true;
                _proxyFileParam->getValueAtTime(sequenceTime + offset, proxyFileName);
                if (proxyFileName.empty()) {
                    proxyGood = false;
                } else {
                    if (checkForExistingFile) {
                        proxyGood = checkIfFileExists(proxyFileName);
                    }
                }
                if (proxyGood) {
                    // proxy file exists
                    *proxyFilename = proxyFileName;
                    ret = eGetFileNameReturnedProxy;
                }
            }
        }
        if (missingFrame == eMissingPrevious) {
            --offset;
        } else if (missingFrame == eMissingNext) {
            ++offset;
        } else if (missingFrame == eMissingNearest) {
            if (offset <= 0) {
                offset = -offset + 1;
            } else if (sequenceTime - offset >= 0) {
                offset = -offset;
            } else {
                ++offset;
            }
        }
    } while (searchOtherFrame && // only loop if searchOtherFrame
             !filenameGood && // and no valid file was found
             std::abs(offset) <= MISSING_FRAME_NEAREST_RANGE && // and we are within range

             (sequenceTime + offset >= 0)); // and index is positive
    if (filenameGood) {
        // ret is already set (see above)
    } else {
        *filename = filename0; // reset to the original frame name;
        switch (missingFrame) {
        case eMissingPrevious: // Hold previous
        case eMissingNext: // Load next
        case eMissingNearest: // Load nearest
        case eMissingError: // Error
            /// For images sequences, we can safely say this is  a missing frame. For video-streams we do not know and the derived class
            // will have to handle the case itself.
            ret = eGetFileNameFailed;
            // return a black image
            break;
        case eMissingBlack: // Black image
            /// For images sequences, we can safely say this is  a missing frame. For video-streams we do not know and the derived class
            // will have to handle the case itself.
            ret = eGetFileNameBlack;
            break;
        }
    }

    return ret;
} // GenericReaderPlugin::getFilenameAtSequenceTime

OfxStatus
GenericReaderPlugin::getFilenameAtTime(double t,
                                       string* filename) const
{
    double sequenceTime;
    GetSequenceTimeRetEnum getSequenceTimeRet = getSequenceTime(t, &sequenceTime);

    switch (getSequenceTimeRet) {
    case eGetSequenceTimeBlack:

        return kOfxStatReplyDefault;

    case eGetSequenceTimeError:

        return kOfxStatFailed;

    case eGetSequenceTimeBeforeSequence:
    case eGetSequenceTimeWithinSequence:
    case eGetSequenceTimeAfterSequence:
        break;
    }
    GetFilenameRetCodeEnum getFilenameAtSequenceTimeRet = getFilenameAtSequenceTime(t, sequenceTime, true, filename, NULL);
    switch (getFilenameAtSequenceTimeRet) {
    case eGetFileNameFailed:

        // do not setPersistentMessage()!
        return kOfxStatFailed;

    case eGetFileNameBlack:

        return kOfxStatReplyDefault;

    case eGetFileNameReturnedFullRes:
    case eGetFileNameReturnedProxy:
        break;
    }

    return kOfxStatOK;
}

int
GenericReaderPlugin::getStartingTime() const
{
    int startingTime;

    _startingTime->getValue(startingTime);

    return startingTime;
}

void
GenericReaderPlugin::copyPixelData(const OfxRectI& renderWindow,
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
    assert(srcBounds.y1 <= renderWindow.y1 && renderWindow.y1 <= renderWindow.y2 && renderWindow.y2 <= srcBounds.y2);
    assert(srcBounds.x1 <= renderWindow.x1 && renderWindow.x1 <= renderWindow.x2 && renderWindow.x2 <= srcBounds.x2);

    copyPixels(*this, renderWindow, renderScale,
               srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcBitDepth, srcRowBytes,
               dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
}

// update the window of dst defined by nextRenderWindow by halving the corresponding area in src.
// proofread and fixed by F. Devernay on 3/10/2014
// The pixel offsets are in std::ptrdiff_t: in int, they overflowed for bounds far from the origin (e.g. a PNG oFFs
// offset or an EXR data window, #371), and the address of pixel (0,0) may be out of the buffers, so each row pointer
// is computed from the buffer's start.
template <typename PIX>
static void
halveWindow(const OfxRectI& nextRenderWindow,
            const PIX* srcPixels,
            const OfxRectI& srcBounds,
            int srcRowBytes,
            PIX* dstPixels,
            const OfxRectI& dstBounds,
            int dstRowBytes,
            int nComponents)
{
    const std::ptrdiff_t srcRowSize = srcRowBytes / (std::ptrdiff_t)sizeof(PIX);
    const std::ptrdiff_t dstRowSize = dstRowBytes / (std::ptrdiff_t)sizeof(PIX);

    assert(nextRenderWindow.x1 * 2LL >= (srcBounds.x1 - 1LL) && (nextRenderWindow.x2 - 1LL) * 2 < srcBounds.x2 && nextRenderWindow.y1 * 2LL >= (srcBounds.y1 - 1LL) && (nextRenderWindow.y2 - 1LL) * 2 < srcBounds.y2);
    for (int y = nextRenderWindow.y1; y < nextRenderWindow.y2; ++y) {
        // the rows 2y (this row) and 2y + 1 (the next row) of src
        const std::ptrdiff_t srcY = 2 * (std::ptrdiff_t)y;
        const bool pickNextRow = srcY < (srcBounds.y2 - 1);
        const bool pickThisRow = srcY >= srcBounds.y1;
        const int sumH = (int)pickNextRow + (int)pickThisRow;
        assert(sumH == 1 || sumH == 2);
        const PIX* srcThisRow = pickThisRow ? srcPixels + (srcY - srcBounds.y1) * srcRowSize : NULL;
        const PIX* srcNextRow = pickNextRow ? srcPixels + (srcY + 1 - srcBounds.y1) * srcRowSize : NULL;
        PIX* dstLineStart = dstPixels + ((std::ptrdiff_t)y - dstBounds.y1) * dstRowSize;
        for (int x = nextRenderWindow.x1; x < nextRenderWindow.x2; ++x) {
            const std::ptrdiff_t srcX = 2 * (std::ptrdiff_t)x;
            const bool pickNextCol = srcX < (srcBounds.x2 - 1);
            const bool pickThisCol = srcX >= srcBounds.x1;
            const int sumW = (int)pickThisCol + (int)pickNextCol;
            assert(sumW == 1 || sumW == 2);
            // the offsets of the columns 2x and 2x + 1 of src in a row, and of the column x of dst
            const std::ptrdiff_t thisCol = (srcX - srcBounds.x1) * nComponents;
            const std::ptrdiff_t nextCol = thisCol + nComponents;
            const std::ptrdiff_t dstCol = ((std::ptrdiff_t)x - dstBounds.x1) * nComponents;
            for (int k = 0; k < nComponents; ++k) {
                /// a b
                /// c d

                PIX a = (pickThisCol && pickThisRow) ? srcThisRow[thisCol + k] : 0;
                PIX b = (pickNextCol && pickThisRow) ? srcThisRow[nextCol + k] : 0;
                PIX c = (pickThisCol && pickNextRow) ? srcNextRow[thisCol + k] : 0;
                PIX d = (pickNextCol && pickNextRow) ? srcNextRow[nextCol + k] : 0;

                assert(sumW == 2 || (sumW == 1 && ((a == 0 && c == 0) || (b == 0 && d == 0))));
                assert(sumH == 2 || (sumH == 1 && ((a == 0 && b == 0) || (c == 0 && d == 0))));
                int sum = sumH * sumW;
                // guard against division by zero
                assert(sum);
                dstLineStart[dstCol + k] = sum ? ((a + b + c + d) / sum) : 0;
            }
        }
    }
}

// At a render scale below 1 (downscaleLevels > 0), keep the frame bounds off the infinite flags: "Infinite regions
// are flagged by setting x1 = kOfxFlagInfiniteMin, y1 = kOfxFlagInfiniteMin, x2 = kOfxFlagInfiniteMax,
// y2 = kOfxFlagInfiniteMax" (ofxCore.h), and Coords::downscalePowerOfTwoSmallestEnclosing() keeps them, so a window
// at the render scale built from bounds on a flag would be infinite (#371). The bounds are half-open: clamping
// x2 = kOfxFlagInfiniteMax to kOfxFlagInfiniteMax - 1 drops the last column of the image (x = kOfxFlagInfiniteMax - 1),
// which is black at the render scale, and likewise for the other sides. The host got an infinite RoD for these bounds
// anyway. At scale 1, the bounds are unchanged.
static void
clampProxyFrameBounds(int downscaleLevels,
                      OfxRectI* bounds)
{
    if (downscaleLevels <= 0) {
        return;
    }
    bounds->x1 = (std::max)(bounds->x1, kOfxFlagInfiniteMin + 1);
    bounds->y1 = (std::max)(bounds->y1, kOfxFlagInfiniteMin + 1);
    bounds->x2 = (std::min)(bounds->x2, kOfxFlagInfiniteMax - 1);
    bounds->y2 = (std::min)(bounds->y2, kOfxFlagInfiniteMax - 1);
}

// Render scales that are not the same power of two in x and y (#194). "To map between the two [spatial coordinate
// systems] the pixel aspect ratio and the render scale need to be known, and it is a simple case of multiplication and
// rounding" (ofxCoordSystem.rst, "Mapping Between The Spatial Coordinate Systems"): pixel (x, y) at the render scale s
// covers [x / s.x, (x + 1) / s.x) x [y / s.y, (y + 1) / s.y) at scale 1, whatever the PAR.

// the smallest rectangle at the render scale that encloses r (in pixels at scale 1): pixels at scale 1 are canonical
// coordinates with a PAR of 1
static OfxRectI
scaleSmallestEnclosing(const OfxRectI& r,
                       const OfxPointD& renderScale)
{
    const OfxRectD rd = { (double)r.x1, (double)r.y1, (double)r.x2, (double)r.y2 };
    OfxRectI ret;
    Coords::toPixelEnclosing(rd, renderScale, 1., &ret);

    return ret;
}

// the smallest rectangle at scale 1 that encloses r (in pixels at the render scale). It divides by the render scale,
// as resampleWindow() does for each pixel, so that the footprints of the pixels of r are in it: x * (1 / s) may be
// rounded differently from x / s.
static OfxRectI
unscaleSmallestEnclosing(const OfxRectI& r,
                         const OfxPointD& renderScale)
{
    OfxRectI ret;
    ret.x1 = Coords::pixelToIntClamped(std::floor(r.x1 / renderScale.x), kOfxFlagInfiniteMin);
    ret.y1 = Coords::pixelToIntClamped(std::floor(r.y1 / renderScale.y), kOfxFlagInfiniteMin);
    ret.x2 = Coords::pixelToIntClamped(std::ceil(r.x2 / renderScale.x), kOfxFlagInfiniteMax);
    ret.y2 = Coords::pixelToIntClamped(std::ceil(r.y2 / renderScale.y), kOfxFlagInfiniteMax);

    return ret;
}

// Update the window of dst (at the render scale) from src (at scale 1): each pixel is the mean of the pixels of
// srcWindow that it covers, weighted by the covered area. As in halveWindow(), the pixels out of srcWindow (the
// frame bounds) are not counted; a pixel that covers none of srcWindow is transparent black. Each pixel depends only
// on its own position, so that a render window gives the pixels of the render of the whole RoD.
static void
resampleWindow(const OfxRectI& window,
               const OfxPointD& renderScale,
               const float* srcPixels,
               const OfxRectI& srcBounds,
               int srcRowBytes,
               const OfxRectI& srcWindow,
               float* dstPixels,
               const OfxRectI& dstBounds,
               int dstRowBytes,
               int nComponents)
{
    assert(renderScale.x > 0. && renderScale.y > 0.);
    assert(srcBounds.x1 <= srcWindow.x1 && srcWindow.x2 <= srcBounds.x2 && srcBounds.y1 <= srcWindow.y1 && srcWindow.y2 <= srcBounds.y2);
    const std::ptrdiff_t srcRowSize = srcRowBytes / (std::ptrdiff_t)sizeof(float);
    const std::ptrdiff_t dstRowSize = dstRowBytes / (std::ptrdiff_t)sizeof(float);
    std::vector<double> sum(nComponents);
    for (int y = window.y1; y < window.y2; ++y) {
        // the rows that pixel y covers, in srcWindow
        const double y1 = (std::max)(y / renderScale.y, (double)srcWindow.y1);
        const double y2 = (std::min)((y + 1.) / renderScale.y, (double)srcWindow.y2);
        float* dst = dstPixels + ((std::ptrdiff_t)y - dstBounds.y1) * dstRowSize + ((std::ptrdiff_t)window.x1 - dstBounds.x1) * nComponents;
        for (int x = window.x1; x < window.x2; ++x, dst += nComponents) {
            const double x1 = (std::max)(x / renderScale.x, (double)srcWindow.x1);
            const double x2 = (std::min)((x + 1.) / renderScale.x, (double)srcWindow.x2);
            std::fill(sum.begin(), sum.end(), 0.);
            double weight = 0.;
            if ((y1 < y2) && (x1 < x2)) {
                for (std::ptrdiff_t j = (std::ptrdiff_t)std::floor(y1); j < y2; ++j) {
                    const double wy = (std::min)(j + 1., y2) - (std::max)((double)j, y1);
                    const float* srcRow = srcPixels + (j - srcBounds.y1) * srcRowSize;
                    for (std::ptrdiff_t i = (std::ptrdiff_t)std::floor(x1); i < x2; ++i) {
                        const double w = wy * ((std::min)(i + 1., x2) - (std::max)((double)i, x1));
                        const float* src = srcRow + (i - srcBounds.x1) * nComponents;
                        for (int k = 0; k < nComponents; ++k) {
                            sum[k] += w * src[k];
                        }
                        weight += w;
                    }
                }
            }
            for (int k = 0; k < nComponents; ++k) {
                dst[k] = (weight > 0.) ? (float)(sum[k] / weight) : 0.f;
            }
        }
    }
}

template <typename PIX>
static void
buildMipMapLevelGeneric(ImageEffect* instance,
                        const OfxRectI& originalRenderWindow,
                        const OfxRectI& renderWindowFullRes,
                        unsigned int level,
                        const PIX* srcPixels,
                        const OfxRectI& srcBounds,
                        int srcRowBytes,
                        PIX* dstPixels,
                        const OfxRectI& dstBounds,
                        int dstRowBytes,
                        int nComponents)
{
    assert(level > 0);

    // two temporary images, used alternately, so that each halving reads one buffer and writes the other (halving
    // in place would only work because of halveWindow's access order)
    std::unique_ptr<ImageMemory> tmpMem[2];
    size_t tmpMemSize[2] = { 0, 0 };
    PIX* nextImg = NULL;
    const PIX* previousImg = srcPixels;
    OfxRectI previousBounds = srcBounds;
    int previousRowBytes = srcRowBytes;
    OfxRectI nextRenderWindow = renderWindowFullRes;

    /// Build all the mipmap levels until we reach the one we are interested in
    for (unsigned int i = 1; i < level; ++i) {
        // loop invariant:
        // - previousImg, previousBounds, previousRowBytes describe the data ate the level before i
        // - nextRenderWindow contains the renderWindow at the level before i
        //
        /// Halve the smallest enclosing po2 rect as we need to render a minimum of the renderWindow
        nextRenderWindow = Coords::downscalePowerOfTwoSmallestEnclosing(nextRenderWindow, 1);
#ifdef DEBUG
        {
            // check that doing i times 1 level is the same as doing i levels
            OfxRectI nrw = Coords::downscalePowerOfTwoSmallestEnclosing(renderWindowFullRes, i);
            assert(nrw.x1 == nextRenderWindow.x1 && nrw.x2 == nextRenderWindow.x2 && nrw.y1 == nextRenderWindow.y1 && nrw.y2 == nextRenderWindow.y2);
        }
#endif
        /// Allocate a temporary image if necessary, or reuse the one allocated two levels before (which is larger)
        int nextRowBytes = (nextRenderWindow.x2 - nextRenderWindow.x1) * nComponents * sizeof(PIX);
        size_t newMemSize = (size_t)(nextRenderWindow.y2 - nextRenderWindow.y1) * (size_t)nextRowBytes;
        std::unique_ptr<ImageMemory>& mem = tmpMem[i % 2];
        if (!mem.get() || tmpMemSize[i % 2] < newMemSize) {
            mem.reset(new ImageMemory(newMemSize, instance));
            tmpMemSize[i % 2] = newMemSize;
        }
        nextImg = (PIX*)mem->lock();
        if (!nextImg) {
            throwSuiteStatusException(kOfxStatErrMemory);

            return;
        }

        halveWindow<PIX>(nextRenderWindow, previousImg, previousBounds, previousRowBytes, nextImg, nextRenderWindow, nextRowBytes, nComponents);

        /// Switch for next pass
        previousBounds = nextRenderWindow;
        previousRowBytes = nextRowBytes;
        previousImg = nextImg;
    }
    // here:
    // - previousImg, previousBounds, previousRowBytes describe the data ate the level before 'level'
    // - nextRenderWindow contains the renderWindow at the level before 'level'

    /// On the last iteration halve directly into the dstPixels
    /// The nextRenderWindow should be equal to the original render window.
    nextRenderWindow = Coords::downscalePowerOfTwoSmallestEnclosing(nextRenderWindow, 1);
    assert(originalRenderWindow.x1 == nextRenderWindow.x1 && originalRenderWindow.x2 == nextRenderWindow.x2 && originalRenderWindow.y1 == nextRenderWindow.y1 && originalRenderWindow.y2 == nextRenderWindow.y2);

    halveWindow<PIX>(nextRenderWindow, previousImg, previousBounds, previousRowBytes, dstPixels, dstBounds, dstRowBytes, nComponents);
    // mem and tmpMem are freed at destruction
} // buildMipMapLevelGeneric

// update the window of dst defined by originalRenderWindow by mipmapping the windows of src defined by renderWindowFullRes
// proofread and fixed by F. Devernay on 3/10/2014
template <typename PIX, int nComponents>
static void
buildMipMapLevel(ImageEffect* instance,
                 const OfxRectI& originalRenderWindow,
                 const OfxRectI& renderWindowFullRes,
                 unsigned int level,
                 const PIX* srcPixels,
                 const OfxRectI& srcBounds,
                 int srcRowBytes,
                 PIX* dstPixels,
                 const OfxRectI& dstBounds,
                 int dstRowBytes)
{
    buildMipMapLevelGeneric<PIX>(instance, originalRenderWindow, renderWindowFullRes, level, srcPixels, srcBounds, srcRowBytes, dstPixels, dstBounds, dstRowBytes, nComponents);
}

void
GenericReaderPlugin::scalePixelData(const OfxRectI& originalRenderWindow,
                                    const OfxPointD& renderScale,
                                    const OfxRectI& renderWindow,
                                    unsigned int levels,
                                    const void* srcPixelData,
                                    PixelComponentEnum srcPixelComponents,
                                    int srcPixelComponentCount,
                                    BitDepthEnum srcPixelDepth,
                                    const OfxRectI& srcBounds,
                                    int srcRowBytes,
                                    void* dstPixelData,
                                    PixelComponentEnum dstPixelComponents,
                                    int dstPixelComponentCount,
                                    BitDepthEnum dstPixelDepth,
                                    const OfxRectI& dstBounds,
                                    int dstRowBytes)
{
    unused(renderScale);
    assert(srcPixelData && dstPixelData);

    // do the rendering
    if ((dstPixelDepth != eBitDepthFloat) || ((dstPixelComponents != ePixelComponentRGBA) && (dstPixelComponents != ePixelComponentRGB) && (dstPixelComponents != ePixelComponentXY) && (dstPixelComponents != ePixelComponentAlpha) && (dstPixelComponents != ePixelComponentCustom)) || (dstPixelDepth != srcPixelDepth) || (dstPixelComponents != srcPixelComponents) || (dstPixelComponentCount != srcPixelComponentCount)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    if (dstPixelComponents == ePixelComponentRGBA) {
        if (!_supportsRGBA) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        buildMipMapLevel<float, 4>(this, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    } else if (dstPixelComponents == ePixelComponentRGB) {
        if (!_supportsRGB) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        buildMipMapLevel<float, 3>(this, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    } else if (dstPixelComponents == ePixelComponentXY) {
        if (!_supportsXY) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        buildMipMapLevel<float, 2>(this, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    } else if (dstPixelComponents == ePixelComponentAlpha) {
        if (!_supportsAlpha) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        buildMipMapLevel<float, 1>(this, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                   srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes);
    } else {
        assert(dstPixelComponents == ePixelComponentCustom);

        buildMipMapLevelGeneric<float>(this, originalRenderWindow, renderWindow, levels, (const float*)srcPixelData,
                                       srcBounds, srcRowBytes, (float*)dstPixelData, dstBounds, dstRowBytes, dstPixelComponentCount);
    }
} // GenericReaderPlugin::scalePixelData

/* set up and run a copy processor */
static void
setupAndFillWithBlack(PixelProcessorFilterBase& processor,
                      const OfxRectI& renderWindow,
                      const OfxPointD& renderScale,
                      void* dstPixelData,
                      const OfxRectI& dstBounds,
                      PixelComponentEnum dstPixelComponents,
                      int dstPixelComponentCount,
                      BitDepthEnum dstPixelDepth,
                      int dstRowBytes)
{
    // set the images
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstPixelDepth, dstRowBytes);

    // set the render window
    processor.setRenderWindow(renderWindow, renderScale);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

void
GenericReaderPlugin::fillWithBlack(const OfxRectI& renderWindow,
                                   const OfxPointD& renderScale,
                                   void* dstPixelData,
                                   const OfxRectI& dstBounds,
                                   PixelComponentEnum dstPixelComponents,
                                   int dstPixelComponentCount,
                                   BitDepthEnum dstBitDepth,
                                   int dstRowBytes)
{
    BlackFiller<float> fred(*this, dstPixelComponentCount);
    setupAndFillWithBlack(fred, renderWindow, renderScale, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
}

static void
setupAndProcess(PixelProcessorFilterBase& processor,
                int premultChannel,
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

    // set the images
    processor.setDstImg(dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstPixelDepth, dstRowBytes);
    processor.setSrcImg(srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, 0);

    // set the render window
    processor.setRenderWindow(renderWindow, renderScale);

    processor.setPremultMaskMix(true, premultChannel, 1.);

    // Call the base class process member, this will call the derived templated process code
    processor.process();
}

void
GenericReaderPlugin::unPremultPixelData(const OfxRectI& renderWindow,
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
                                        int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // do the rendering
    if ((dstBitDepth != eBitDepthFloat) || ((dstPixelComponents != ePixelComponentRGBA) && (dstPixelComponents != ePixelComponentRGB) && (dstPixelComponents != ePixelComponentAlpha))) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }
    if (dstPixelComponents == ePixelComponentRGBA) {
        if (!_supportsRGBA) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        PixelCopierUnPremult<float, 4, 1, float, 4, 1> fred(*this);
        setupAndProcess(fred, 3, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
    } else {
        /// other pixel components means you want to copy only...
        assert(false);
    }
}

void
GenericReaderPlugin::premultPixelData(const OfxRectI& renderWindow,
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
                                      int dstRowBytes)
{
    assert(srcPixelData && dstPixelData);

    // do the rendering
    if ((dstBitDepth != eBitDepthFloat) || ((dstPixelComponents != ePixelComponentRGBA) && (dstPixelComponents != ePixelComponentRGB) && (dstPixelComponents != ePixelComponentAlpha))) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    if (dstPixelComponents == ePixelComponentRGBA) {
        if (!_supportsRGBA) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        PixelCopierPremult<float, 4, 1, float, 4, 1> fred(*this);
        setupAndProcess(fred, 3, renderWindow, renderScale, srcPixelData, srcBounds, srcPixelComponents, srcPixelComponentCount, srcPixelDepth, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstPixelComponentCount, dstBitDepth, dstRowBytes);
    } else {
        /// other pixel components means you want to copy only...
        assert(false);
    }
}

bool
GenericReaderPlugin::getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                                           OfxRectD& rod)
{
    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return false;
    }

    double sequenceTime;
    GetSequenceTimeRetEnum getSequenceTimeRet = getSequenceTime(args.time, &sequenceTime);
    switch (getSequenceTimeRet) {
    case eGetSequenceTimeBlack:

        return false;

    case eGetSequenceTimeError:
        throwSuiteStatusException(kOfxStatFailed);

        return false;

    case eGetSequenceTimeBeforeSequence:
    case eGetSequenceTimeWithinSequence:
    case eGetSequenceTimeAfterSequence:
        break;
    }

    /*We don't want to use the proxy image for the region of definition*/
    string filename;
    GetFilenameRetCodeEnum getFilenameAtSequenceTimeRet = getFilenameAtSequenceTime(args.time, sequenceTime, true, &filename, NULL);
    switch (getFilenameAtSequenceTimeRet) {
    case eGetFileNameFailed:
        if (_syncClip && _syncClip->isConnected()) {
            // if the Sync input is connected, just return the RoD from this input
            // Because Natron calls getRoD() before render(), so the image may not yet be here.
            return false;
        }
        setPersistentMessage(Message::eMessageError, "", filename + ": Cannot load frame");
        throwSuiteStatusException(kOfxStatFailed);

        return false;

    case eGetFileNameBlack:
        clearPersistentMessage();

        return false;

    case eGetFileNameReturnedFullRes:
    case eGetFileNameReturnedProxy:
        // clearPersistentMessage();
        break;
    }

    string error;
    OfxRectI bounds, format;
    double par = 1.;
    int tile_width, tile_height;
    bool success = getFrameBounds(filename, sequenceTime, args.view, &bounds, &format, &par, &error, &tile_width, &tile_height);
    if (!success) {
        setPersistentMessage(Message::eMessageError, "", error);
        throwSuiteStatusException(kOfxStatFailed);

        return false;
    }
    // get the PAR from the clip preferences, since it should be constant over time
    par = _outputClip->getPixelAspectRatio();
    rod.x1 = bounds.x1 * par;
    rod.x2 = bounds.x2 * par;
    rod.y1 = bounds.y1;
    rod.y2 = bounds.y2;

    //    if (getFilenameAtSequenceTimeRet == eGetFileNameReturnedProxy) {
    //        ///upscale the proxy RoD to be in canonical coords.
    //        unsigned int mipmapLvl = getLevelFromScale(args.renderScale.x);
    //        rod = upscalePowerOfTwo(rod, (double)mipmapLvl);
    //    }

    return true;
} // GenericReaderPlugin::getRegionOfDefinition

class OutputImagesHolder_RAII {
    std::vector<Image*> dstImages;

public:
    OutputImagesHolder_RAII()
        : dstImages()
    {
    }

    void appendImage(Image* img)
    {
        dstImages.push_back(img);
    }

    const std::vector<Image*>& getOutputPlanes() const
    {
        return dstImages;
    }

    ~OutputImagesHolder_RAII()
    {
        for (std::size_t i = 0; i < dstImages.size(); ++i) {
            delete dstImages[i];
        }
    }
};

namespace {
// The innermost Scope (RenderTimeSetter or DecodedWindowScope, below) of the calling thread, or nullptr. Not a
// thread_local on macOS and Linux: macOS's dyld never unloads an image that has thread-local variables, so the
// plug-in binary stayed loaded after the host unloaded it (#264). (MS Windows unloads a DLL that has them.)
template <class Scope>
class InnermostScope
{
public:
    static Scope* get()
    {
#ifdef _WIN32
        return slot();
#else
        return static_cast<Scope*>(pthread_getspecific(key().key));
#endif
    }

    // Makes scope the innermost one, and returns the previous one, for pop(). Throws std::bad_alloc if the thread's
    // storage could not be allocated (only possible the first time a thread sets it), before any change.
    static Scope* push(Scope* scope)
    {
        Scope* previous = get();
#ifdef _WIN32
        slot() = scope;
#else
        if (pthread_setspecific(key().key, scope) != 0) {
            throw std::bad_alloc();
        }
#endif

        return previous;
    }

    // Restores the scope that push() returned (the thread's storage exists: this cannot fail)
    static void pop(Scope* previous) noexcept
    {
#ifdef _WIN32
        slot() = previous;
#else
        pthread_setspecific(key().key, previous);
#endif
    }

private:
#ifdef _WIN32
    static Scope*& slot()
    {
        static thread_local Scope* scope = nullptr;

        return scope;
    }
#else
    // a pthread key, deleted when the binary is unloaded (each scope resets its thread's value on exit: there is no
    // value left to destroy, so the key has no destructor function)
    struct Key {
        Key()
        {
            const int err = pthread_key_create(&key, nullptr);
            if (err != 0) {
                throw std::system_error(err, std::generic_category(), "pthread_key_create");
            }
        }
        // (at exit without kOfxActionUnload, #250, this may run while a host thread is in a decode: its
        // pthread_getspecific() then returns nullptr, so getRenderTime() is 0, and its pop() fails, harmlessly)
        ~Key() { pthread_key_delete(key); }
        Key(const Key&) = delete;
        Key& operator=(const Key&) = delete;

        pthread_key_t key;
    };

    // (created at the first use: an exception there propagates to the action, and the next use tries again)
    static const Key& key()
    {
        static const Key k;

        return k;
    }
#endif
};

// The time of the render action being processed by this thread, for its duration (see
// GenericReaderPlugin::getRenderTime()); the enclosing one is restored on exit, including on exceptions.
class RenderTimeSetter
{
public:
    explicit RenderTimeSetter(double t)
        : _time(t)
        , _previous(InnermostScope<RenderTimeSetter>::push(this))
    {
    }

    ~RenderTimeSetter() { InnermostScope<RenderTimeSetter>::pop(_previous); }

    RenderTimeSetter(const RenderTimeSetter&) = delete;
    RenderTimeSetter& operator=(const RenderTimeSetter&) = delete;

    double time() const { return _time; }

private:
    const double _time;
    RenderTimeSetter* const _previous;
};
}

double
GenericReaderPlugin::getRenderTime()
{
    const RenderTimeSetter* scope = InnermostScope<RenderTimeSetter>::get();

    return scope ? scope->time() : 0.;
}

namespace {
// Around a call to decode() or decodePlane() with the given render window, on this thread: the decoded window is the
// whole render window, unless decode() reports a smaller one (GenericReaderPlugin::setDecodedWindow(), outside such
// a call, does nothing). decodedWindow() is the part of renderWindow that comes from the file (returns false if it is
// empty). The enclosing scope is restored on exit (including on exceptions), as in RenderTimeSetter, for a render
// nested in a decode() on the same thread.
class DecodedWindowScope
{
public:
    explicit DecodedWindowScope(const OfxRectI& renderWindow)
        : _renderWindow(renderWindow)
        , _decodedWindow(renderWindow)
        , _previous(InnermostScope<DecodedWindowScope>::push(this))
    {
    }

    ~DecodedWindowScope() { InnermostScope<DecodedWindowScope>::pop(_previous); }

    DecodedWindowScope(const DecodedWindowScope&) = delete;
    DecodedWindowScope& operator=(const DecodedWindowScope&) = delete;

    void setDecodedWindow(const OfxRectI& window) { _decodedWindow = window; }

    // (an empty decoded window has an empty intersection)
    bool decodedWindow(OfxRectI* window) const { return intersectWindow(_renderWindow, _decodedWindow, window); }

private:
    const OfxRectI _renderWindow;
    OfxRectI _decodedWindow;
    DecodedWindowScope* const _previous;
};
}

void
GenericReaderPlugin::setDecodedWindow(const OfxRectI& window)
{
    if (DecodedWindowScope* scope = InnermostScope<DecodedWindowScope>::get()) {
        scope->setDecodedWindow(window);
    }
}

bool
GenericReaderPlugin::clipToFile(const OfxRectI& renderWindow,
                                const OfxRectI& fileBounds,
                                void* pixelData,
                                const OfxRectI& bounds,
                                int pixelComponentCount,
                                BitDepthEnum bitDepth,
                                int rowBytes,
                                OfxRectI* window)
{
    assert(window);
    // the part of the render window that is in the image (all of it, for decode()), and its part in the file
    OfxRectI w;
    const bool inImage = intersectWindow(renderWindow, bounds, &w);
    const bool inFile = inImage && intersectWindow(w, fileBounds, window);
    if (!inFile) {
        window->x1 = window->y1 = window->x2 = window->y2 = 0;
    }
    setDecodedWindow(*window);
    if (!inImage || !pixelData) {
        return inFile;
    }
    if (inFile && (window->x1 == w.x1) && (window->y1 == w.y1) && (window->x2 == w.x2) && (window->y2 == w.y2)) {
        // nothing to clear
        return true;
    }
    const std::size_t pixelBytes = (std::size_t)pixelComponentCount * (std::size_t)getComponentBytes(bitDepth);
    for (int y = w.y1; y < w.y2; ++y) {
        // the pixel (w.x1, y) (rowBytes may be negative: signed offsets)
        char* row = (char*)pixelData + (ptrdiff_t)(y - bounds.y1) * (ptrdiff_t)rowBytes + (ptrdiff_t)(w.x1 - bounds.x1) * (ptrdiff_t)pixelBytes;
        if (!inFile || (y < window->y1) || (y >= window->y2)) {
            std::memset(row, 0, (std::size_t)(w.x2 - w.x1) * pixelBytes);
            continue;
        }
        if (w.x1 < window->x1) {
            std::memset(row, 0, (std::size_t)(window->x1 - w.x1) * pixelBytes);
        }
        if (window->x2 < w.x2) {
            std::memset(row + (std::size_t)(window->x2 - w.x1) * pixelBytes, 0, (std::size_t)(w.x2 - window->x2) * pixelBytes);
        }
    }

    return inFile;
}

// Set the alpha channel of an RGBA float buffer to the white point (1) over renderWindow.
// Used for files whose premultiplication state is Opaque: the OpenFX spec defines kOfxImageOpaque as "the image is
// opaque and so has no premultiplication state, but the alpha component in all pixels is set to the white point".
static void
setOpaqueAlpha(const OfxRectI& renderWindow,
               float* pixelData,
               const OfxRectI& bounds,
               int pixelComponentCount,
               int rowBytes)
{
    assert(pixelComponentCount == 4);
    OfxRectI w;
    if (!intersectWindow(renderWindow, bounds, &w)) {
        return;
    }
    for (int y = w.y1; y < w.y2; ++y) {
        // rowBytes may be negative (OFX images may be stored top-down): use signed offsets
        float* pix = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * (ptrdiff_t)rowBytes) + (ptrdiff_t)(w.x1 - bounds.x1) * pixelComponentCount + 3;
        for (int x = w.x1; x < w.x2; ++x, pix += pixelComponentCount) {
            *pix = 1.f;
        }
    }
}

void
GenericReaderPlugin::render(const RenderArguments& args)
{
    RenderTimeSetter renderTimeSetter(args.time); // for getRenderTime(), used by decode()/decodePlane()
    if (!_outputClip) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    assert(kSupportsRenderScale || (args.renderScale.x == 1. && args.renderScale.y == 1.));
    /// The image will have the appropriate size since we support the render scale (multi-resolution)
    OutputImagesHolder_RAII outputImagesHolder;
#ifdef OFX_EXTENSIONS_NUKE
    if (!isMultiPlanar()) {
#endif

        Image* dstImg = _outputClip->fetchImage(args.time);
        if (!dstImg) {
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
#ifndef NDEBUG
        if (!_supportsTiles) {
            // kOfxImageEffectPropSupportsTiles: "If a clip or plugin does not support tiled images, then the host
            // should supply full RoD images to the effect whenever it fetches one." The image has the whole RoD; it
            // may be larger (e.g. it covers a render window that is not clipped to the RoD).
            OfxRectI dstRod; // = dst->getRegionOfDefinition(); //  Nuke's image RoDs are wrong
            Coords::toPixelEnclosing(_outputClip->getRegionOfDefinition(args.time), args.renderScale, _outputClip->getPixelAspectRatio(), &dstRod);
            const OfxRectI& dstBounds = dstImg->getBounds();

            assert(dstBounds.x1 <= dstRod.x1);
            assert(dstRod.x2 <= dstBounds.x2);
            assert(dstBounds.y1 <= dstRod.y1);
            assert(dstRod.y2 <= dstBounds.y2); // crashes on Natron if kSupportsTiles=0 & kSupportsMultiResolution=1
        }
#endif

        outputImagesHolder.appendImage(dstImg);
#ifdef OFX_EXTENSIONS_NUKE
    } else {
        assert(getImageEffectHostDescription()->isMultiPlanar);

        for (std::list<string>::const_iterator it = args.planes.begin(); it != args.planes.end(); ++it) {
            Image* dstPlane = _outputClip->fetchImagePlane(args.time, args.renderView, it->c_str());
            if (!dstPlane) {
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            outputImagesHolder.appendImage(dstPlane);
        }
    }

#endif

    OfxRectI firstBounds = { 0, 0, 0, 0 };
    BitDepthEnum firstDepth = eBitDepthNone;
    bool firstImageSet = false;

    std::list<PlaneToRender> planes;

    const std::vector<Image*>& outputImages = outputImagesHolder.getOutputPlanes();
    for (std::size_t i = 0; i < outputImages.size(); ++i) {
        checkBadRenderScaleOrField(outputImages[i], args);

        PlaneToRender plane;
        void* dstPixelData = NULL;
        OfxRectI bounds;
        BitDepthEnum bitDepth;
        getImageData(outputImages[i], &dstPixelData, &bounds, &plane.comps, &bitDepth, &plane.rowBytes);
        if (bitDepth != eBitDepthFloat) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        if (!firstImageSet) {
            firstBounds = bounds;
            firstDepth = bitDepth;
            firstImageSet = true;
        } else {
            if ((firstBounds.x1 != bounds.x1) || (firstBounds.x2 != bounds.x2) || (firstBounds.y1 != bounds.y1) || (firstBounds.y2 != bounds.y2)
                || (firstDepth != bitDepth)) {
                setPersistentMessage(Message::eMessageError, "", "OFX Host gave image plane with wrong bounds/bitdepth");
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
        }
        plane.pixelData = (float*)dstPixelData;
        if (!plane.pixelData) {
            // decode()/decodePlane(), fillWithBlack() and the copies below write through it (#278)
            setPersistentMessage(Message::eMessageError, "", "OFX Host provided an invalid image buffer");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }

        plane.rawComps = outputImages[i]->getPixelComponentsProperty();

#ifdef OFX_EXTENSIONS_NUKE
        if (plane.comps != ePixelComponentCustom) {
#endif
#ifdef OFX_EXTENSIONS_NATRON
            assert(plane.rawComps == kOfxImageComponentAlpha || plane.rawComps == kNatronOfxImageComponentXY || plane.rawComps == kOfxImageComponentRGB || plane.rawComps == kOfxImageComponentRGBA);
#else
        assert(plane.rawComps == kOfxImageComponentAlpha || plane.rawComps == kOfxImageComponentRGB || plane.rawComps == kOfxImageComponentRGBA);
#endif
            PixelComponentEnum outputComponents = getOutputComponents();
            if (plane.comps != outputComponents) {
                setPersistentMessage(Message::eMessageError, "", "OFX Host dit not take into account output components");
                throwSuiteStatusException(kOfxStatErrImageFormat);

                return;
            }
#ifdef OFX_EXTENSIONS_NUKE
        }
#endif

        plane.numChans = outputImages[i]->getPixelComponentCount();
        planes.push_back(plane);
    }

    // are we in the image bounds
    if ((args.renderWindow.x1 < firstBounds.x1) || (args.renderWindow.x1 >= firstBounds.x2) || (args.renderWindow.y1 < firstBounds.y1) || (args.renderWindow.y1 >= firstBounds.y2) || (args.renderWindow.x2 <= firstBounds.x1) || (args.renderWindow.x2 > firstBounds.x2) || (args.renderWindow.y2 <= firstBounds.y1) || (args.renderWindow.y2 > firstBounds.y2)) {
        throwSuiteStatusException(kOfxStatErrValue);

        // throw std::runtime_error("render window outside of image bounds");
        return;
    }

    double sequenceTime;
    GetSequenceTimeRetEnum getSequenceTimeRet = getSequenceTime(args.time, &sequenceTime);
    switch (getSequenceTimeRet) {
    case eGetSequenceTimeBlack:
        for (std::list<PlaneToRender>::iterator it = planes.begin(); it != planes.end(); ++it) {
            fillWithBlack(args.renderWindow, args.renderScale, it->pixelData, firstBounds, it->comps, it->numChans, firstDepth, it->rowBytes);
        }

        return;

    case eGetSequenceTimeError:
        throwSuiteStatusException(kOfxStatFailed);

        return;

    case eGetSequenceTimeBeforeSequence:
    case eGetSequenceTimeWithinSequence:
    case eGetSequenceTimeAfterSequence:
        break;
    }

    bool useProxy = false;
    OfxPointD proxyScaleThreshold;
    _proxyThreshold->getValue(proxyScaleThreshold.x, proxyScaleThreshold.y);

    OfxPointD proxyOriginalScale;
    _originalProxyScale->getValue(proxyOriginalScale.x, proxyOriginalScale.y);

    // At a render scale that is the same power of two in x and y (as in Natron), GenericReader halves the
    // full-resolution image renderMipmapLevel times, or reads a proxy file. Nothing in OFX restricts the render scale
    // to powers of two (kOfxImageEffectPropRenderScale: "The proxy render scale currently being applied"): at any
    // other one (e.g. 0.75, 1/3, or 1 in x and 1/2 in y), it resamples the full-resolution image (#194), unless it is
    // the scale of the proxy file (detectProxyScale(), e.g. 1/3), whose pixels are then used as they are.
    unsigned int renderMipmapLevel = getLevelFromScale((std::min)(args.renderScale.x, args.renderScale.y));
    const bool notPowerOfTwo = (args.renderScale.x > 0.) && (args.renderScale.y > 0.)
                               && ((args.renderScale.x != args.renderScale.y) || (args.renderScale.x != Coords::scaleFromMipmapLevel(renderMipmapLevel)));
    const bool atProxyScale = (args.renderScale.x == proxyOriginalScale.x) && (args.renderScale.y == proxyOriginalScale.y);
    unsigned int proxyMipMapThresholdLevel = (proxyScaleThreshold.x == 0 || proxyScaleThreshold.y == 0) ? renderMipmapLevel : getLevelFromScale((std::min)(proxyScaleThreshold.x, proxyScaleThreshold.y));
    unsigned int originalProxyMipMapLevel = (proxyOriginalScale.x == 0 || proxyOriginalScale.y == 0) ? renderMipmapLevel : getLevelFromScale((std::min)(proxyOriginalScale.x, proxyOriginalScale.y));

    if (kSupportsRenderScale && (!notPowerOfTwo || atProxyScale) && (renderMipmapLevel >= proxyMipMapThresholdLevel)) {
        useProxy = true;
    }

    // Read the file param once (#373): the host may change it on the main thread during the render, and the
    // full-resolution and proxy file names must come from the same value.
    string filename, proxyFile;
    GetFilenameRetCodeEnum getFilenameAtSequenceTimeRet = getFilenameAtSequenceTime(args.time, sequenceTime, true, &filename, useProxy ? &proxyFile : NULL);
    switch (getFilenameAtSequenceTimeRet) {
    case eGetFileNameFailed:
        setPersistentMessage(Message::eMessageError, "", filename + ": Cannot load frame");
        throwSuiteStatusException(kOfxStatFailed);

        return;

    case eGetFileNameBlack:
        clearPersistentMessage();
        for (std::list<PlaneToRender>::iterator it = planes.begin(); it != planes.end(); ++it) {
            fillWithBlack(args.renderWindow, args.renderScale, it->pixelData, firstBounds, it->comps, it->numChans, firstDepth, it->rowBytes);
        }

        return;

    case eGetFileNameReturnedFullRes:
        clearPersistentMessage();
        useProxy = false;
        break;

    case eGetFileNameReturnedProxy:
        clearPersistentMessage();
        assert(useProxy && !proxyFile.empty());
        /// Use the proxy only if it is different from the original file
        useProxy = (proxyFile != filename);
        break;
    }

    // The args.renderWindow is already in pixels coordinate (render scale is already taken into account).
    // If the filename IS NOT a proxy file we have to make sure the renderWindow is
    // upscaled to a scale of (1,1). On the other hand if the filename IS a proxy we have to determine the actual RoD
    // of the proxy file and adjust the scale so it fits the given scale.
    // When in proxy mode renderWindowFullRes is the render window at the proxy mip map level
    // At a scale that is not a power of two, the proxy file is only read at its own scale (atProxyScale), and
    // downscaleLevels is then 0; otherwise the full-resolution file is resampled.
    const bool readsProxy = useProxy && !proxyFile.empty(); // the proxy file is read, not the full-resolution one
    const bool resample = notPowerOfTwo && !readsProxy;
    if (resample) {
        renderMipmapLevel = 0;
    }
    int downscaleLevels = renderMipmapLevel; // the number of mipmap levels from the actual file (proxy or not) to the renderWindow

    if (readsProxy) {
        filename = proxyFile;
        downscaleLevels -= originalProxyMipMapLevel;
    }
    assert(downscaleLevels >= 0);
    assert(!notPowerOfTwo || resample || (downscaleLevels == 0));

    if (filename.empty() || !checkIfFileExists(filename)) {
        for (std::list<PlaneToRender>::iterator it = planes.begin(); it != planes.end(); ++it) {
            fillWithBlack(args.renderWindow, args.renderScale, it->pixelData, firstBounds, it->comps, it->numChans, firstDepth, it->rowBytes);
        }

        return;
    }

    OfxRectI renderWindowFullRes, renderWindowNotRounded;
    OfxRectI frameBounds, format;
    double par = 1.;
    int tile_width, tile_height;
    string error;

    /// if the plug-in doesn't support tiles, just render the full rod
    bool success = getFrameBounds(filename, sequenceTime, args.renderView, &frameBounds, &format, &par, &error, &tile_width, &tile_height);
    /// We shouldve checked above for any failure, now this is too late.
    if (!success) {
        setPersistentMessage(Message::eMessageError, "", error);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    clampProxyFrameBounds(downscaleLevels, &frameBounds);

    // The render window, in the frame bounds (at the render scale, the smallest enclosing rectangle). It is not in
    // them if the file changed since the host got the RoD (a reader may update its cached frame bounds when it reads
    // the file, e.g. ReadEXR): the part of the render window out of the new frame bounds is black.
    OfxRectI renderWindow = args.renderWindow;
    {
        // the frame bounds at the render scale
        const auto scaledFrameBounds = [&]() {
            return resample ? scaleSmallestEnclosing(frameBounds, args.renderScale) : Coords::downscalePowerOfTwoSmallestEnclosing(frameBounds, downscaleLevels);
        };
        OfxRectI frameWindow = scaledFrameBounds();
        // The part of the render window in the RoD that the host got: if it is out of the frame bounds, the reader's
        // cached frame bounds may be older than the file (e.g. the file reverted to the header that the host got the
        // RoD from), and the reader may read them again. A render window larger than an up-to-date RoD (e.g. the
        // display window of a cropped image) is not in that case: its part out of the RoD is black anyway, and the
        // file is not read again for each render.
        OfxRectI hostRoD;
        {
            // The RoD is bounds.x1 * par... (getRegionOfDefinition()): x * par / par is not always x (e.g. 1920 at a
            // PAR of 40/33 gives 1920.0000000000002), so shrink it by much less than a pixel before converting it,
            // so that an up-to-date RoD is never one pixel larger than the frame bounds (a stale one is at least one
            // pixel larger)
            OfxRectD hostRoDCanonical = _outputClip->getRegionOfDefinition(args.time);
            hostRoDCanonical.x1 += 1e-6 * (1. + std::fabs(hostRoDCanonical.x1));
            hostRoDCanonical.y1 += 1e-6 * (1. + std::fabs(hostRoDCanonical.y1));
            hostRoDCanonical.x2 -= 1e-6 * (1. + std::fabs(hostRoDCanonical.x2));
            hostRoDCanonical.y2 -= 1e-6 * (1. + std::fabs(hostRoDCanonical.y2));
            Coords::toPixelEnclosing(hostRoDCanonical, args.renderScale, _outputClip->getPixelAspectRatio(), &hostRoD);
        }
        OfxRectI hostWindow;
        if (!intersectWindow(renderWindow, hostRoD, &hostWindow)) {
            hostWindow = frameWindow; // (nothing to refresh)
        }
        // (not for a proxy file, whose frame bounds are not those of the RoD: its aspect ratio may differ)
        if (!readsProxy
            && ((hostWindow.x1 < frameWindow.x1) || (hostWindow.y1 < frameWindow.y1) || (hostWindow.x2 > frameWindow.x2) || (hostWindow.y2 > frameWindow.y2))
            && refreshFrameBounds(filename, sequenceTime, args.renderView)) {
            // the reader's cached frame bounds were older than the file: get the new ones
            success = getFrameBounds(filename, sequenceTime, args.renderView, &frameBounds, &format, &par, &error, &tile_width, &tile_height);
            if (!success) {
                setPersistentMessage(Message::eMessageError, "", error);
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            clampProxyFrameBounds(downscaleLevels, &frameBounds);
            frameWindow = scaledFrameBounds();
        }
        if (readsProxy) {
            // The RoD is the full-resolution file's (getRegionOfDefinition() does not read the proxy file): a proxy
            // file whose aspect ratio is not the original's covers more or less than the RoD at the render scale.
            // Its pixels out of the RoD are not written (they are black, as out of the RoD at scale 1), and the part
            // of the RoD out of the proxy file is black (#439).
            OfxRectI inRoD;
            (void)intersectWindow(frameWindow, hostRoD, &inRoD); // all 0 if they don't intersect
            frameWindow = inRoD;
        }
        if ((renderWindow.x1 < frameWindow.x1) || (renderWindow.y1 < frameWindow.y1) || (renderWindow.x2 > frameWindow.x2) || (renderWindow.y2 > frameWindow.y2)) {
            for (std::list<PlaneToRender>::iterator it = planes.begin(); it != planes.end(); ++it) {
                fillWithBlack(args.renderWindow, args.renderScale, it->pixelData, firstBounds, it->comps, it->numChans, firstDepth, it->rowBytes);
            }
            if (!intersectWindow(args.renderWindow, frameWindow, &renderWindow)) {
                return;
            }
        }
    }

    if (resample) {
        renderWindowFullRes = unscaleSmallestEnclosing(renderWindow, args.renderScale);
    } else {
        renderWindowFullRes = upscalePowerOfTwo(renderWindow, downscaleLevels); // works even if downscaleLevels == 0
        // we may have gone a few pixels too far (but never more than 2^downscaleLevels-1 pixels)
        assert(renderWindowFullRes.x1 >= frameBounds.x1 - std::pow(2., (double)downscaleLevels) + 1 && renderWindowFullRes.x2 <= frameBounds.x2 + std::pow(2., (double)downscaleLevels) - 1 && renderWindowFullRes.y1 >= frameBounds.y1 - std::pow(2., (double)downscaleLevels) + 1 && renderWindowFullRes.y2 <= frameBounds.y2 + std::pow(2., (double)downscaleLevels) - 1);
    }
    // Intersect the full res renderwindow to the real rod
    intersect(renderWindowFullRes, frameBounds, &renderWindowFullRes);

    // See below: we round the render window to the tile size
    renderWindowNotRounded = renderWindowFullRes;

    // decode() and decodePlane() get a window in the pixel coordinates of the file, at full resolution (the
    // scaling to the render scale, if any, is done below by scalePixelData() or resampleWindow()): their render scale
    // is 1.
    const OfxPointD fileScale = { 1., 1. };

    try {
        for (std::list<PlaneToRender>::iterator it = planes.begin(); it != planes.end(); ++it) {
            // Read into a temporary image, apply colorspace conversion, then copy
            bool isOCIOIdentity = true;

            // if components are custom, remap it to a OFX components with the same number of channels
            PixelComponentEnum remappedComponents = it->comps;

            bool isColor = (it->comps == ePixelComponentRGB) || (it->comps == ePixelComponentRGBA);
            bool isCustom = false;
            if (remappedComponents == ePixelComponentCustom) {
                MultiPlane::ImagePlaneDesc plane, pairedPlane;
                MultiPlane::ImagePlaneDesc::mapOFXComponentsTypeStringToPlanes(it->rawComps, &plane, &pairedPlane);
                const std::vector<std::string>& channels = plane.getChannels();
                isColor = ((channels.size() == 3 && channels[0] == "R" && channels[1] == "G" && channels[2] == "B") || (channels.size() == 4 && channels[0] == "R" && channels[1] == "G" && channels[2] == "B" && channels[3] == "A"));
                isCustom = true;

                if (it->numChans == 3) {
                    remappedComponents = ePixelComponentRGB;
                } else if (it->numChans == 4) {
                    remappedComponents = ePixelComponentRGBA;
                } else if (it->numChans == 2) {
                    remappedComponents = ePixelComponentXY;
                } else if (it->numChans == 1) {
                    remappedComponents = ePixelComponentAlpha;
                }
            }
#ifdef OFX_IO_USING_OCIO
            if (isColor) {
                isOCIOIdentity = _ocio->isIdentity(args.time);
            }
#endif

            PreMultiplicationEnum filePremult = eImageUnPreMultiplied;
            PreMultiplicationEnum outputPremult = eImageUnPreMultiplied;
            if ((remappedComponents == ePixelComponentRGB) || (remappedComponents == ePixelComponentXY)) {
                filePremult = outputPremult = eImageOpaque;
            } else if (remappedComponents == ePixelComponentAlpha) {
                filePremult = outputPremult = eImagePreMultiplied;
            } else if ((it->comps == ePixelComponentRGBA) || (isCustom && isColor && (remappedComponents == ePixelComponentRGBA))) {
                int premult_i;
                _filePremult->getValue(premult_i);
                filePremult = (PreMultiplicationEnum)premult_i;

                int oPremult_i;
                _outputPremult->getValue(oPremult_i);
                outputPremult = (PreMultiplicationEnum)oPremult_i;
            }

            // we have to do the final premultiplication if:
            // - pixelComponents is RGBA
            //  AND
            //   - buffer is PreMultiplied AND OCIO is not identity (OCIO works only on unpremultiplied data)
            //   OR
            //   - premult is unpremultiplied
            bool mustPremult = (isColor && (remappedComponents == ePixelComponentRGBA) && ((filePremult == eImageUnPreMultiplied || !isOCIOIdentity) && outputPremult == eImagePreMultiplied));
            // An Opaque color plane has alpha = 1 everywhere (see setOpaqueAlpha()): there is nothing to premultiply by.
            // Opaque (alpha = 1) applies to the color plane only (it is forced for an RGB one): it says nothing about the
            // alpha of the other RGBA planes of a multi-planar file, which keep the decoded alpha (NatronGitHub/Natron#1632).
            const bool opaqueAlpha = !isCustom && (filePremult == eImageOpaque) && (remappedComponents == ePixelComponentRGBA) && (it->numChans == 4);
            if (opaqueAlpha) {
                mustPremult = false;
            }

            if (!mustPremult && isOCIOIdentity && (!kSupportsRenderScale || (!resample && (renderMipmapLevel == 0)))) {
                // no colorspace conversion, no premultiplication, no proxy, just read file
                DBG(std::printf("decode (to dst)\n"));

                DecodedWindowScope decodedWindowScope(renderWindow);
                if (!_isMultiPlanar) {
                    decode(filename, sequenceTime, args.renderView, renderWindow, fileScale, it->pixelData, firstBounds, it->comps, it->numChans, it->rowBytes);
                } else {
                    decodePlane(filename, sequenceTime, args.renderView, renderWindow, fileScale, it->pixelData, firstBounds, it->comps, remappedComponents, it->numChans, it->rawComps, it->rowBytes);
                }
                // only where the image comes from the file: the part of the render window that is not in the file
                // that decode() opened (e.g. rewritten smaller) is out of its RoD, and stays transparent black
                OfxRectI decodedWindow;
                if (opaqueAlpha && decodedWindowScope.decodedWindow(&decodedWindow)) {
                    setOpaqueAlpha(decodedWindow, it->pixelData, firstBounds, it->numChans, it->rowBytes);
                }
            } else {
                int pixelBytes;
                if (it->comps == ePixelComponentCustom) {
                    pixelBytes = it->numChans * sizeof(float);
                } else {
                    pixelBytes = it->numChans * getComponentBytes(firstDepth);
                }
                assert(pixelBytes > 0);

                /*
                   If tile_width and tile_height is set, round the renderWindow to the enclosing tile size to make sure the plug-in has a buffer
                   large enough to decode tiles. This is needed for OpenImageIO. Note that
                 */
                if ((tile_width > 0) && (tile_height > 0)) {
                    // In double, and clamped to the range of int when stored: Y inverted near the limits of an int,
                    // or a window rounded to the tiles, may be out of it (#184). The results are the same as before
                    // (in int) otherwise.
                    const double tw = tile_width;
                    const double th = tile_height;
                    const OfxRectD rw = { (double)renderWindowFullRes.x1, (double)renderWindowFullRes.y1, (double)renderWindowFullRes.x2, (double)renderWindowFullRes.y2 };
                    const OfxRectD fb = { (double)frameBounds.x1, (double)frameBounds.y1, (double)frameBounds.x2, (double)frameBounds.y2 };
                    OfxRectD rounded;
                    if (isTileOrientationTopDown()) {
                        // invert Y before rounding, and back after
                        const double frameHeight = fb.y2 - fb.y1;
                        rounded.x1 = (std::min)(std::ceil(rw.x1 / tw) * tw, fb.x1);
                        rounded.y1 = frameHeight - (std::min)(std::ceil((frameHeight - rw.y1) / th) * th, frameHeight - fb.y1);
                        rounded.x2 = (std::max)(std::floor(rw.x2 / tw) * tw, fb.x2);
                        rounded.y2 = frameHeight - (std::max)(std::floor((frameHeight - rw.y2) / th) * th, frameHeight - fb.y2);
                    } else {
                        rounded.x1 = (std::max)(std::floor(rw.x1 / tw) * tw, fb.x1);
                        rounded.y1 = (std::max)(std::floor(rw.y1 / th) * th, fb.y1);
                        rounded.x2 = (std::min)(std::ceil(rw.x2 / tw) * tw, fb.x2);
                        rounded.y2 = (std::min)(std::ceil(rw.y2 / th) * th, fb.y2);
                    }
                    renderWindowFullRes.x1 = Coords::pixelToIntClamped(rounded.x1, kOfxFlagInfiniteMin);
                    renderWindowFullRes.y1 = Coords::pixelToIntClamped(rounded.y1, kOfxFlagInfiniteMin);
                    renderWindowFullRes.x2 = Coords::pixelToIntClamped(rounded.x2, kOfxFlagInfiniteMax);
                    renderWindowFullRes.y2 = Coords::pixelToIntClamped(rounded.y2, kOfxFlagInfiniteMax);
                }

                int tmpRowBytes = (renderWindowFullRes.x2 - renderWindowFullRes.x1) * pixelBytes;
                size_t memSize = (size_t)(renderWindowFullRes.y2 - renderWindowFullRes.y1) * (size_t)tmpRowBytes;
                ImageMemory mem(memSize, this);
                float* tmpPixelData = (float*)mem.lock();

                // read file
                DBG(std::printf("decode (to tmp)\n"));

                OfxRectI decodedWindow;
                bool hasDecodedWindow;
                {
                    DecodedWindowScope decodedWindowScope(renderWindowFullRes);
                    if (!_isMultiPlanar) {
                        decode(filename, sequenceTime, args.renderView, renderWindowFullRes, fileScale, tmpPixelData, renderWindowFullRes, it->comps, it->numChans, tmpRowBytes);
                    } else {
                        decodePlane(filename, sequenceTime, args.renderView, renderWindowFullRes, fileScale, tmpPixelData, renderWindowFullRes, it->comps, remappedComponents, it->numChans, it->rawComps, tmpRowBytes);
                    }
                    hasDecodedWindow = decodedWindowScope.decodedWindow(&decodedWindow);
                }

                if (abort()) {
                    return;
                }

                /// do the color-space conversion
                if (!isOCIOIdentity && isColor) {
                    if (filePremult == eImagePreMultiplied) {
                        assert(remappedComponents == ePixelComponentRGBA);
                        DBG(std::printf("unpremult (tmp in-place)\n"));
                        // tmpPixelData[0] = tmpPixelData[1] = tmpPixelData[2] = tmpPixelData[3] = 0.5;
                        unPremultPixelData(renderWindowNotRounded, args.renderScale, tmpPixelData, renderWindowFullRes, remappedComponents, it->numChans, firstDepth, tmpRowBytes, tmpPixelData, renderWindowFullRes, remappedComponents, it->numChans, firstDepth, tmpRowBytes);

                        if (abort()) {
                            return;
                        }

                        // assert(tmpPixelData[0] == 1. && tmpPixelData[1] == 1. && tmpPixelData[2] == 1. && tmpPixelData[3] == 0.5);
                    }
#ifdef OFX_IO_USING_OCIO
                    DBG(std::printf("OCIO (tmp in-place)\n"));
                    _ocio->apply(args.time, renderWindowFullRes, args.renderScale, tmpPixelData, renderWindowFullRes, remappedComponents, it->numChans, tmpRowBytes);
#endif
                }

                // after the colorspace conversion, which may also modify the alpha channel
                // (only where the image comes from the file, as above)
                if (opaqueAlpha && hasDecodedWindow) {
                    setOpaqueAlpha(decodedWindow, tmpPixelData, renderWindowFullRes, it->numChans, tmpRowBytes);
                }

                if (kSupportsRenderScale && (resample || (downscaleLevels > 0))) {
                    if (!mustPremult) {
                        // we can write directly to dstPixelData
                        /// adjust the scale to match the given output image
                        DBG(std::printf("scale (no premult, tmp to dst)\n"));
                        if (resample) {
                            assert(firstDepth == eBitDepthFloat); // (checked above)
                            resampleWindow(renderWindow, args.renderScale, tmpPixelData, renderWindowFullRes, tmpRowBytes, renderWindowNotRounded,
                                           it->pixelData, firstBounds, it->rowBytes, it->numChans);
                        } else {
                            scalePixelData(renderWindow, args.renderScale, renderWindowNotRounded, (unsigned int)downscaleLevels, tmpPixelData, remappedComponents,
                                           it->numChans, firstDepth, renderWindowFullRes, tmpRowBytes, it->pixelData,
                                           remappedComponents, it->numChans, firstDepth, firstBounds, it->rowBytes);
                        }
                    } else {
                        // allocate a temporary image (we must avoid reading from dstPixelData, in case several threads are rendering the same area)
                        int mem2RowBytes = (firstBounds.x2 - firstBounds.x1) * pixelBytes;
                        size_t mem2Size = (size_t)(firstBounds.y2 - firstBounds.y1) * (size_t)mem2RowBytes;
                        ImageMemory mem2(mem2Size, this);
                        float* scaledPixelData = (float*)mem2.lock();

                        /// adjust the scale to match the given output image
                        DBG(std::printf("scale (tmp to scaled)\n"));
                        if (resample) {
                            assert(firstDepth == eBitDepthFloat); // (checked above)
                            resampleWindow(renderWindow, args.renderScale, tmpPixelData, renderWindowFullRes, tmpRowBytes, renderWindowNotRounded,
                                           scaledPixelData, firstBounds, mem2RowBytes, it->numChans);
                        } else {
                            scalePixelData(renderWindow, args.renderScale, renderWindowNotRounded, (unsigned int)downscaleLevels, tmpPixelData,
                                           remappedComponents, it->numChans, firstDepth,
                                           renderWindowFullRes, tmpRowBytes, scaledPixelData,
                                           remappedComponents, it->numChans, firstDepth,
                                           firstBounds, mem2RowBytes);
                        }

                        if (abort()) {
                            return;
                        }

                        // apply premult
                        DBG(std::printf("premult (scaled to dst)\n"));
                        // scaledPixelData[0] = scaledPixelData[1] = scaledPixelData[2] = 1.; scaledPixelData[3] = 0.5;
                        premultPixelData(renderWindow, args.renderScale, scaledPixelData, firstBounds, remappedComponents, it->numChans, firstDepth, mem2RowBytes, it->pixelData, firstBounds, remappedComponents, it->numChans, firstDepth, it->rowBytes);
                        // assert(dstPixelDataF[0] == 0.5 && dstPixelDataF[1] == 0.5 && dstPixelDataF[2] == 0.5 && dstPixelDataF[3] == 0.5);
                    }
                } else {
                    // copy
                    if (mustPremult) {
                        DBG(std::printf("premult (no scale, tmp to dst)\n"));
                        // tmpPixelData[0] = tmpPixelData[1] = tmpPixelData[2] = 1.; tmpPixelData[3] = 0.5;
                        premultPixelData(renderWindow, args.renderScale, tmpPixelData, renderWindowFullRes, remappedComponents, it->numChans, firstDepth, tmpRowBytes, it->pixelData, firstBounds, remappedComponents, it->numChans, firstDepth, it->rowBytes);
                        // assert(dstPixelDataF[0] == 0.5 && dstPixelDataF[1] == 0.5 && dstPixelDataF[2] == 0.5 && dstPixelDataF[3] == 0.5);
                    } else {
                        DBG(std::printf("copy (no premult no scale, tmp to dst)\n"));
                        copyPixelData(renderWindow, args.renderScale, tmpPixelData, renderWindowFullRes, remappedComponents, it->numChans, firstDepth, tmpRowBytes, it->pixelData, firstBounds, remappedComponents, it->numChans, firstDepth, it->rowBytes);
                    }
                }
                mem.unlock();
            }
        } // for (std::list<PlaneToRender>::iterator it = planes.begin(); it!=planes.end(); ++it) {
    } catch (const MissingFrameBlackException&) {
        // decode()/decodePlane(): the frame is not in the file and kParamOnMissingFrame is Black
        clearPersistentMessage();
        for (std::list<PlaneToRender>::iterator it = planes.begin(); it != planes.end(); ++it) {
            fillWithBlack(args.renderWindow, args.renderScale, it->pixelData, firstBounds, it->comps, it->numChans, firstDepth, it->rowBytes);
        }
    }
}

void
GenericReaderPlugin::decode(const string& /*filename*/,
                            OfxTime /*time*/,
                            int /*view*/,
                            const OfxRectI& /*renderWindow*/,
                            const OfxPointD& /*renderScale*/,
                            float* /*pixelData*/,
                            const OfxRectI& /*bounds*/,
                            PixelComponentEnum /*pixelComponents*/,
                            int /*pixelComponentCount*/,
                            int /*rowBytes*/)
{
    // does nothing
}

void
GenericReaderPlugin::decodePlane(const string& /*filename*/,
                                 OfxTime /*time*/,
                                 int /*view*/,
                                 const OfxRectI& /*renderWindow*/,
                                 const OfxPointD& /*renderScale*/,
                                 float* /*pixelData*/,
                                 const OfxRectI& /*bounds*/,
                                 PixelComponentEnum /*pixelComponents*/,
                                 PixelComponentEnum /*remappedComponents*/,
                                 int /*pixelComponentCount*/,
                                 const string& /*rawComponents*/,
                                 int /*rowBytes*/)
{
    // does nothing
}

#ifdef OFX_EXTENSIONS_NUKE
void
GenericReaderPlugin::setClipPlanes(const ClipComponentsArguments& args,
                                   ClipComponentsSetter& clipComponents,
                                   const LayersUnionVect& layers)
{
    // Should only be called if multi-planar
    assert(isMultiPlanar());

    // no pass-through
    clipComponents.setPassThroughClip(NULL, args.time, args.view);

    // ask for the color components from the input (needed for the Sync connection to work)
    clipComponents.addClipPlane(*_syncClip, kFnOfxImagePlaneColour);

    for (LayersUnionVect::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if (it->first != kReaderColorLayer) {
            MultiPlane::ImagePlaneDesc plane(it->first, "", "", it->second.layer.channelNames);
            clipComponents.addClipPlane(*_outputClip, MultiPlane::ImagePlaneDesc::mapPlaneToOFXPlaneString(plane));
        }
    }

    // Also add the color plane
    int nOutputComps = 0;
    switch (getOutputComponents()) {
    case ePixelComponentAlpha:
        nOutputComps = 1;
        break;
    case ePixelComponentRGB:
        nOutputComps = 3;
        break;
    case ePixelComponentRGBA:
        nOutputComps = 4;
        break;
#ifdef OFX_EXTENSIONS_NATRON
    case ePixelComponentXY:
        nOutputComps = 2;
        break;
#endif
    default:
        nOutputComps = 0;
        break;
    }

    MultiPlane::ImagePlaneDesc colorPlane = MultiPlane::ImagePlaneDesc::mapNCompsToColorPlane(nOutputComps);
    clipComponents.addClipPlane(*_outputClip, MultiPlane::ImagePlaneDesc::mapPlaneToOFXPlaneString(colorPlane));
}
#endif // OFX_EXTENSIONS_NUKE

bool
GenericReaderPlugin::checkExtension(const string& ext)
{
    if (ext.empty()) {
        // no extension
        return false;
    }

    return std::find(_extensions.begin(), _extensions.end(), ext) != _extensions.end();
}

/**
 * @brief Called from changedParam() when kParamFilename is changed for any reason other than eChangeTime.
 * Calls restoreStateFromParams() to update any non-persistent params that may depend on the filename.
 * If reason is eChangeUserEdit and the params where never guessed (see _guessedParams) also sets these from the file contents.
 * Any derived implementation must call GenericReaderPlugin::changedFilename() first
 **/
void
GenericReaderPlugin::changedFilename(const InstanceChangedArgs& args)
{
    assert(args.reason != eChangeTime);
    if (args.reason == eChangeTime) {
        return;
    }
    string filename;

    _fileParam->getValue(filename);

    clearPersistentMessage();

    if (filename.empty()) {
        // if the file name is set to an empty string,
        // reset so that values are automatically set on next call to changedFilename().
        // (not resetToDefault(), which calls paramDeleteAllKeys: the param doesn't animate, and this is also called
        // from createInstance, where a new instance has the default value already)
        if (_guessedParams->getValue()) {
            _guessedParams->setValue(false);
        }

        return;
    }

    OfxRangeI sequenceTimeDomain;
    bool gotSequenceTimeDomain = getSequenceTimeDomainInternal(sequenceTimeDomain, true);
    if (!gotSequenceTimeDomain) {
        return;
    }

    bool customFps;
    _customFPS->getValue(customFps);
    if (!customFps) {
        double fps;
        bool gotFps = getFrameRate(filename, &fps);
        if (gotFps) {
            _fps->setValue(fps);
        }
    }

    OfxRangeI timeDomain;

    timeDomainFromSequenceTimeDomain(sequenceTimeDomain, sequenceTimeDomain.min, &timeDomain);

    if (args.reason == eChangeUserEdit) {
        _firstFrame->setValue(sequenceTimeDomain.min);
        _firstFrame->setDefault(sequenceTimeDomain.min);
        _lastFrame->setValue(sequenceTimeDomain.max);
        _lastFrame->setDefault(sequenceTimeDomain.max);
        _startingTime->setDefault(sequenceTimeDomain.min);
        if (!_guessedParams->getValue()) {
            // the first file: the sequence starts at its first frame
            _startingTime->setValue(timeDomain.min);
        }
        // the first frame changed: keep the time offset in sync with the starting time (#154). If the
        // params were already guessed: in Time Offset mode, the time offset is kept and the starting time follows
        // (#260); in Starting Time mode, the starting time is kept (Natron already set it to the first frame from
        // kParamOriginalFrameRange).
        syncTimeParams();
    }

    // restore state! let the derived class (which calls GenericReaderPlugin::restoreStateFromParams()) initialize any other parameters or structures
    restoreStateFromParams();

    // should we guess some parameters? only if it's user-set and it's the first time
    if ((args.reason == eChangeUserEdit) && !_guessedParams->getValue()) {
        /// We call guessParamsFromFilename with the first frame of the sequence so we're almost sure it will work
        /// unless the user did a mistake. We are also safe to assume that images specs are the same for
        /// all the sequence
        _fileParam->getValueAtTime(_firstFrame->getValue(), filename); // the time in _fileParam is the *file* time
        if (filename.empty()) {
            // no need to trigger a useless error or a persistent message

            return;
        }

        string colorspace;

#ifdef OFX_IO_USING_OCIO
        _ocio->getInputColorspaceDefault(colorspace);
#else
        colorspace = "default";
#endif
        PixelComponentEnum components = ePixelComponentNone;
        int componentCount = 0;
        PreMultiplicationEnum filePremult = eImageOpaque;

        assert(!_guessedParams->getValue());
        bool success = guessParamsFromFilename(filename, &colorspace, &filePremult, &components, &componentCount);
        if (!success) {
            return;
        }

        bool setColorSpace = true;
#ifdef OFX_IO_USING_OCIO
        // if inputSpaceSet == true (input space was manually set by user) then setColorSpace = false
        if (_inputSpaceSet->getValue()) {
            setColorSpace = false;
        }
        // Colorspace parsed from filename overrides the file colorspace,
        // following recommendations from http://opencolorio.org/configurations/spi_pipeline.html
        // (OCIO Config::getColorSpaceFromFilepath(), then only if that colorspace name is right before the extension,
        // see below)
        AutoSetAndRestoreThreadLocale locale;
        OCIO::ConstConfigRcPtr ocioConfig = _ocio->getConfig();
        if (setColorSpace && ocioConfig) {
            string name = filename;
            (void)SequenceParsing::removePath(name); // only use the file NAME
            const char* colorSpaceStr = ocioConfig->getColorSpaceFromFilepath(name.c_str());
            size_t colorSpaceStrLen = colorSpaceStr ? std::strlen(colorSpaceStr) : 0;
            if (colorSpaceStrLen == 0) {
                colorSpaceStr = NULL;
            }
            if (colorSpaceStr) {
                // Only use this colorspace name if it is the last thing before the extension name,
                // and it is preceded by '_' or '-' or ' ' or '.' (we exclude '/' and '\\'),
                // as in http://opencolorio.org/configurations/spi_pipeline.html
                // https://github.com/imageworks/OpenColorIO/pull/413
                size_t pos = name.find_last_of('.');
                if (pos == string::npos || pos < (colorSpaceStrLen + 1)) { // +1 for the delimiter
                    // no dot, or the colorspace name and the delimiter cannot hold before the dot
                    colorSpaceStr = NULL;
                    colorSpaceStrLen = 0;
                } else if ((name.compare(pos - colorSpaceStrLen, colorSpaceStrLen, colorSpaceStr) != 0) || ((name[pos - colorSpaceStrLen - 1] != '_') && (name[pos - colorSpaceStrLen - 1] != '-') && (name[pos - colorSpaceStrLen - 1] != ' ') && (name[pos - colorSpaceStrLen - 1] != '.'))) {
                    // the colorspace name is not before the last dot or is not preceded by a valid delimiter
                    colorSpaceStr = NULL;
                    colorSpaceStrLen = 0;
                }
            }
            if (colorSpaceStr && colorSpaceStrLen > 0) {
                assert(_ocio->hasColorspace(colorSpaceStr)); // getColorSpaceFromFilepath() always returns an existing colorspace
                // we're lucky
                _ocio->setInputColorspace(colorSpaceStr);
                setColorSpace = false;
            }
            if (setColorSpace) {
                _ocio->setInputColorspace(colorspace.c_str());
            }
        }

        // Refreshing the choice menus of ocio is required since the instanceChanged action
        // may not be called recursively during the createInstance action
        _ocio->refreshInputAndOutputState(timeDomain.min);
#endif
        // RGB is always Opaque, Alpha is always PreMultiplied
        if (components == ePixelComponentRGB) {
            filePremult = eImageOpaque;
        } else if (components == ePixelComponentAlpha) {
            filePremult = eImagePreMultiplied;
        }
        if (components != ePixelComponentNone) {
            setOutputComponents(components);
        }
        _filePremult->setValue((int)filePremult);

        if (components == ePixelComponentRGB) {
            // RGB is always opaque
            _outputPremult->setValue(eImageOpaque);
        } else if (components == ePixelComponentAlpha) {
            // Alpha is always premultiplied
            _outputPremult->setValue(eImagePreMultiplied);
        }

        _guessedParams->setValue(true); // do not try to guess params anymore on this instance
    } // if ( args.reason == eChangeUserEdit && !_guessedParams->getValue() ) {
} // GenericReaderPlugin::changedFilename

void
GenericReaderPlugin::changedParam(const InstanceChangedArgs& args,
                                  const string& paramName)
{
    const double time = args.time;
#ifdef OFX_IO_USING_OCIO
    // (GenericOCIO may set keys on the colorspace params, e.g. from changedFilename())
    GenericOCIO::InstanceChanged ocioInstanceChanged(_ocio.get());
    bool colorspaceSetByUser = false;
#endif

    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // please check the reason for each parameter when it makes sense!

    if (paramName == kParamFilename) {
        if (args.reason != eChangeTime) {
            // must clear persistent message, or render() is not called by Nuke after an error
            clearPersistentMessage();
            changedFilename(args);
        }
        if (_sublabel && (args.reason != eChangePluginEdit)) {
            refreshSubLabel(args.time);
        }
    } else if (paramName == kParamProxy) {
        /// Detect the scale of the proxy.
        string proxyFile, originalFileName;
        double sequenceTime;
        GetSequenceTimeRetEnum getSequenceTimeRet = getSequenceTime(args.time, &sequenceTime);
        switch (getSequenceTimeRet) {
        case eGetSequenceTimeBlack:
        case eGetSequenceTimeError:
            break;

        case eGetSequenceTimeBeforeSequence:
        case eGetSequenceTimeWithinSequence:
        case eGetSequenceTimeAfterSequence: {
            GetFilenameRetCodeEnum getFilenameAtSequenceTimeRet = getFilenameAtSequenceTime(args.time, sequenceTime, true, &originalFileName, &proxyFile);

            if ((getFilenameAtSequenceTimeRet == eGetFileNameReturnedProxy) && (proxyFile != originalFileName)) {
                assert(!proxyFile.empty());
                /// show the scale param
                _proxyThreshold->setIsSecretAndDisabled(false);
                _enableCustomScale->setIsSecretAndDisabled(false);

                // getFrameBounds() takes the sequence time
                OfxPointD scale = detectProxyScale(originalFileName, proxyFile, sequenceTime);
                _proxyThreshold->setValue(scale.x, scale.y);
                _originalProxyScale->setValue(scale.x, scale.y);
            } else {
                _proxyThreshold->setIsSecretAndDisabled(true);
                _enableCustomScale->setIsSecretAndDisabled(true);
            }
            break;
        }
        }
    } else if (paramName == kParamCustomProxyScale) {
        bool enabled = _enableCustomScale->getValueAtTime(time);
        _proxyThreshold->setEnabled(enabled);
    } else if (paramName == kParamOriginalFrameRange) {
        int oFirst, oLast;
        _originalFrameRange->getValue(oFirst, oLast);
        string filename;
        _fileParam->getValueAtTime(oFirst, filename); // the time in _fileParam is the *file* time
        if (isVideoStream(filename)) {
            return;
        }

        _firstFrame->setDisplayRange(oFirst, oLast);
        _lastFrame->setDisplayRange(oFirst, oLast);

        _firstFrame->setValue(oFirst);
        _firstFrame->setDefault(oFirst);
        _lastFrame->setValue(oLast);
        _lastFrame->setDefault(oLast);

        _startingTime->setDefault(oFirst);
        if (static_cast<FrameModeEnum>(_frameMode->getValue()) != eFrameModeTimeOffset) {
            // Starting Time mode: the starting time is reset to the first frame, and the time offset is now 0 (#154)
            _startingTime->setValue(oFirst);
        }
        // in Time Offset mode, the time offset is kept and the starting time follows the first frame (#260)
        syncTimeParams();
    } else if ((paramName == kParamFirstFrame) && (args.reason == eChangeUserEdit)) {
        int first;
        int oFirst, oLast;
        _originalFrameRange->getValue(oFirst, oLast);
        _firstFrame->getValue(first);
        _lastFrame->setDisplayRange(first, oLast);

        int offset;
        _timeOffset->getValue(offset);
        _startingTime->setValue(first + offset); // will be called with reason == eChangePluginEdit

        _timeDomainUserSet->setValue(true);
    } else if ((paramName == kParamLastFrame) && (args.reason == eChangeUserEdit)) {
        int first;
        int last;
        int oFirst, oLast;
        _originalFrameRange->getValue(oFirst, oLast);
        _firstFrame->getValue(first);
        _lastFrame->getValue(last);
        _firstFrame->setDisplayRange(oFirst, last);

        _timeDomainUserSet->setValue(true);
    } else if ((paramName == kParamFrameMode) && (args.reason == eChangeUserEdit)) {
        FrameModeEnum frameMode = (FrameModeEnum)_frameMode->getValueAtTime(time);
        switch (frameMode) {
        case eFrameModeStartingTime: // starting frame
            _startingTime->setIsSecretAndDisabled(false);
            _timeOffset->setIsSecretAndDisabled(true);
            break;
        case eFrameModeTimeOffset: // time offset
            _startingTime->setIsSecretAndDisabled(true);
            _timeOffset->setIsSecretAndDisabled(false);
            break;
        }
    } else if ((paramName == kParamStartingTime) && (args.reason == eChangeUserEdit)) {
        /// recompute the timedomain
        OfxRangeI sequenceTimeDomain;
        getSequenceTimeDomainInternal(sequenceTimeDomain, true);

        // also update the time offset
        int startingTime = _startingTime->getValueAtTime(time);
        int firstFrame = _firstFrame->getValueAtTime(time);

        _timeOffset->setValue(startingTime - firstFrame);
        _timeDomainUserSet->setValue(true);
    } else if ((paramName == kParamTimeOffset) && (args.reason == eChangeUserEdit)) {
        // also update the starting frame
        int offset = _timeOffset->getValueAtTime(time);
        int first = _firstFrame->getValueAtTime(time);

        _startingTime->setValue(offset + first);
        _timeDomainUserSet->setValue(true);
    } else if ((paramName == kParamOutputComponents) && (args.reason == eChangeUserEdit)) {
        PixelComponentEnum comps = getOutputComponents();
        PreMultiplicationEnum premult = (PreMultiplicationEnum)_outputPremult->getValueAtTime(time);
        if ((comps == ePixelComponentRGB) && (premult != eImageOpaque)) {
            // RGB is always opaque
            _outputPremult->setValue(eImageOpaque);
        } else if ((comps == ePixelComponentAlpha) && (premult != eImagePreMultiplied)) {
            // Alpha is always premultiplied
            _outputPremult->setValue(eImagePreMultiplied);
        }
    } else if ((paramName == kParamOutputPremult) && (args.reason == eChangeUserEdit)) {
        PreMultiplicationEnum premult = (PreMultiplicationEnum)_outputPremult->getValueAtTime(time);
        PixelComponentEnum comps = getOutputComponents();
        // reset to authorized values if necessary
        if ((comps == ePixelComponentRGB) && (premult != eImageOpaque)) {
            // RGB is always opaque
            _outputPremult->setValue((int)eImageOpaque);
        } else if ((comps == ePixelComponentAlpha) && (premult != eImagePreMultiplied)) {
            // Alpha is always premultiplied
            _outputPremult->setValue((int)eImagePreMultiplied);
        }
    } else if (paramName == kParamCustomFps) {
        bool customFps = _customFPS->getValueAtTime(time);
        _fps->setEnabled(customFps);

        if (!customFps) {
            OfxRangeI sequenceTimeDomain;
            if (getSequenceTimeDomainInternal(sequenceTimeDomain, false)) {
                OfxRangeI timeDomain;
                /// these are the value held by the "First frame" and "Last frame" param
                sequenceTimeDomain.min = _firstFrame->getValue();
                sequenceTimeDomain.max = _lastFrame->getValue();
                int startingTime = _startingTime->getValue();
                timeDomainFromSequenceTimeDomain(sequenceTimeDomain, startingTime, &timeDomain);
                _startingTime->setValue(timeDomain.min);

                /// We call guessParamsFromFilename with the first frame of the sequence so we're almost sure it will work
                /// unless the user did a mistake. We are also safe to assume that images specs are the same for
                /// all the sequence
                string filename;
                _fileParam->getValueAtTime(_firstFrame->getValue(), filename); // the time in _fileParam is the *file* time

                double fps;
                bool gotFps = getFrameRate(filename, &fps);
                if (gotFps) {
                    _fps->setValue(fps);
                }
            }
        }
#ifdef OFX_IO_USING_OCIO
    } else if (_ocio->isUserInputColorspaceEdit(args, paramName)) {
        // set the inputSpaceSet param to true https://github.com/MrKepzie/Natron/issues/1492
        // (not when GenericOCIO deletes a key, which Natron reports as a user edit)
        colorspaceSetByUser = true;
#endif
    }

#ifdef OFX_IO_USING_OCIO
    _ocio->changedParam(args, paramName);
    // (after GenericOCIO handled the edit: Natron reports the change of a param before setValue() returns, and after
    // loading a project, the first instance changed action that GenericOCIO gets gives the menus the keys of the
    // strings, which would undo the edit of a menu)
    if (colorspaceSetByUser) {
        _inputSpaceSet->setValue(true);
    }
#endif
} // GenericReaderPlugin::changedParam

PixelComponentEnum
GenericReaderPlugin::getOutputComponents() const
{
    int outputComponents_i = _outputComponents->getValue();

    return _outputComponentsTable[outputComponents_i];
}

void
GenericReaderPlugin::setOutputComponents(PixelComponentEnum comps)
{
    int i;

    // the table ends with ePixelComponentNone: the entries after it are not set
    for (i = 0; i < 4 && _outputComponentsTable[i] != ePixelComponentNone && _outputComponentsTable[i] != comps; ++i) {
    }
    // (the loop stopped at the end of the table, at its ePixelComponentNone end marker, or at comps)
    if ((i >= 4) || (_outputComponentsTable[i] == ePixelComponentNone) || (_outputComponentsTable[i] != comps)) {
        // not found, set the first supported component
        i = 0;
    }
    assert(i < _outputComponents->getNOptions());
    _outputComponents->setValue(i);
}

/* Override the clip preferences */
void
GenericReaderPlugin::getClipPreferences(ClipPreferencesSetter& clipPreferences)
{
    // if there is only one frame and before/after behaviour is hold, then
    // the output is not framevarying
    bool frameVarying = true;
    OfxRangeI sequenceTimeDomain;
    bool gotSequenceTimeDomain = getSequenceTimeDomainInternal(sequenceTimeDomain, false);

    if (sequenceTimeDomain.min == sequenceTimeDomain.max) {
        int beforeChoice_i;
        _beforeFirst->getValue(beforeChoice_i);
        BeforeAfterEnum beforeChoice = BeforeAfterEnum(beforeChoice_i);
        int afterChoice_i;
        _afterLast->getValue(afterChoice_i);
        BeforeAfterEnum afterChoice = BeforeAfterEnum(afterChoice_i);
        if ((beforeChoice == eBeforeAfterHold) && (afterChoice == eBeforeAfterHold)) {
            frameVarying = false;
        }
    }
    if (!frameVarying && isOutputFrameVarying()) {
        // e.g. a still image rendered with animated reader params
        frameVarying = true;
    }
    clipPreferences.setOutputFrameVarying(frameVarying); // true for readers and frame-varying generators/effects @see kOfxImageEffectFrameVarying

    PixelComponentEnum outputComponents = getOutputComponents();
    clipPreferences.setClipComponents(*_outputClip, outputComponents);

    int premult_i;
    _outputPremult->getValue(premult_i);
    PreMultiplicationEnum premult = (PreMultiplicationEnum)premult_i;
    switch (outputComponents) {
    case ePixelComponentAlpha:
        if (!_supportsAlpha) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        // alpha is always premultiplied
        premult = eImagePreMultiplied;
        break;

    case ePixelComponentRGBA:
        break;
    default:
        if (!_supportsRGB) {
            throwSuiteStatusException(kOfxStatErrFormat);

            return;
        }
        // RGB is always Opaque
        premult = eImageOpaque;
        break;
    }
    clipPreferences.setOutputPremultiplication(premult);

    // get the pixel aspect ratio from the first frame
    if (gotSequenceTimeDomain) {
        OfxRangeI timeDomain;
        /// these are the value held by the "First frame" and "Last frame" param
        sequenceTimeDomain.min = _firstFrame->getValue();
        sequenceTimeDomain.max = _lastFrame->getValue();
        int startingTime = _startingTime->getValue();
        timeDomainFromSequenceTimeDomain(sequenceTimeDomain, startingTime, &timeDomain);

        // the first argument is the host time (at which animated params are evaluated),
        // the second one is the sequence (file) time
        string filename;
        GetFilenameRetCodeEnum e = getFilenameAtSequenceTime(timeDomain.min, sequenceTimeDomain.min, true, &filename, NULL);
        if (e == eGetFileNameReturnedFullRes) {
            OfxRectI bounds, format;
            double par = 1.;
            string error;
            int tile_width, tile_height;
            // getFrameBounds() takes the sequence time
            bool success = getFrameBounds(filename, sequenceTimeDomain.min, /*view=*/0, &bounds, &format, &par, &error, &tile_width, &tile_height);
            if (success) {
                // the PAR comes from the file: a PAR of 0 or less is invalid (the host divides by it)
                clipPreferences.setPixelAspectRatio(*_outputClip, validPixelAspectRatio(par));
                clipPreferences.setOutputFormat(format);
            }

            bool customFPS;
            _customFPS->getValue(customFPS);

            double fps;
            if (customFPS) {
                _fps->getValue(fps);
                clipPreferences.setOutputFrameRate(fps);
            } else {
                success = getFrameRate(filename, &fps);
                if (success) {
                    clipPreferences.setOutputFrameRate(fps);
                }
            }
        }
    }
} // GenericReaderPlugin::getClipPreferences

void
GenericReaderPlugin::purgeCaches()
{
    clearAnyCache();
#ifdef OFX_IO_USING_OCIO
    _ocio->purgeCaches();
#endif
}

bool
GenericReaderPlugin::isIdentity(const IsIdentityArguments& args,
                                Clip*& identityClip,
                                double& identityTime, int& /*view*/, std::string& /*plane*/)
{
    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return false;
    }

    if (!gHostIsNatron) {
        // only Natron supports setting the identityClip to the output clip
        return false;
    }

    double sequenceTime;
    GetSequenceTimeRetEnum getSequenceTimeRet = getSequenceTime(args.time, &sequenceTime);
    switch (getSequenceTimeRet) {
    case eGetSequenceTimeBlack:

        return false;

    case eGetSequenceTimeError:
        throwSuiteStatusException(kOfxStatFailed);

        return false;

    case eGetSequenceTimeBeforeSequence:
    case eGetSequenceTimeAfterSequence: {
        /// Transform the sequence time to "real" time
        int timeOffset;
        _timeOffset->getValue(timeOffset);
        identityTime = std::floor(sequenceTime + 0.5) + timeOffset; // round to the nearest frame
        identityClip = _outputClip;

        return true;
    }
    case eGetSequenceTimeWithinSequence: {
        if (sequenceTime == (int)sequenceTime) {
            return false;
        }
        // fractional time, round to nearest frame
        sequenceTime = std::floor(sequenceTime + 0.5); // round to the nearest frame
        /// Transform the sequence time to "real" time
        int timeOffset;
        _timeOffset->getValue(timeOffset);
        identityTime = sequenceTime + timeOffset;
        identityClip = _outputClip;

        return true;
    }
    }

    return false;
} // GenericReaderPlugin::isIdentity

OfxPointD
GenericReaderPlugin::detectProxyScale(const string& originalFileName,
                                      const string& proxyFileName,
                                      OfxTime sequenceTime)
{
    OfxRectI originalBounds, proxyBounds, originalFormat, proxyFormat;
    string error;
    double originalPAR = 1., proxyPAR = 1.;
    int tile_width, tile_height;
    bool success = getFrameBounds(originalFileName, sequenceTime, /*view=*/0, &originalBounds, &originalFormat, &originalPAR, &error, &tile_width, &tile_height);

    proxyBounds.x1 = proxyBounds.x2 = proxyBounds.y1 = proxyBounds.y2 = 0.f;
    success = success && getFrameBounds(proxyFileName, sequenceTime, /*view=*/0, &proxyBounds, &proxyFormat, &proxyPAR, &error, &tile_width, &tile_height);
    OfxPointD ret;
    // the PARs come from the files: a PAR of 0 or less is invalid
    originalPAR = validPixelAspectRatio(originalPAR);
    proxyPAR = validPixelAspectRatio(proxyPAR);
    if (!success || (originalBounds.x1 == originalBounds.x2) || (originalBounds.y1 == originalBounds.y2) || (proxyBounds.x1 == proxyBounds.x2) || (proxyBounds.y1 == proxyBounds.y2)) {
        ret.x = 1.;
        ret.y = 1.;
        setPersistentMessage(Message::eMessageError, "", "Cannot read the proxy file.");

        return ret;
    }
    assert(originalBounds.x2 - originalBounds.x1);
    assert(originalBounds.y2 - originalBounds.y1);
    ret.x = ((proxyBounds.x2 - proxyBounds.x1) * proxyPAR) / ((originalBounds.x2 - originalBounds.x1) * originalPAR);
    ret.y = (proxyBounds.y2 - proxyBounds.y1) / (double)(originalBounds.y2 - originalBounds.y1);

    return ret;
}

template <typename SRCPIX, int srcMaxValue, int nSrcComp, int nDstComp>
class PixelConverterProcessor
    : public PixelProcessor {
    const SRCPIX* _srcPixelData;
    int _dstBufferRowBytes;
    int _srcBufferRowBytes;
    OfxRectI _srcBufferBounds;

public:
    // ctor
    PixelConverterProcessor(ImageEffect& instance)
        : PixelProcessor(instance)
        , _srcPixelData(NULL)
        , _dstBufferRowBytes(0)
        , _srcBufferRowBytes(0)
    {
        assert(srcMaxValue);
        _srcBufferBounds.x1 = _srcBufferBounds.y1 = _srcBufferBounds.x2 = _srcBufferBounds.y2 = 0;
    }

    void setValues(const SRCPIX* srcPixelData,
                   const OfxRectI& srcBufferBounds,
                   int srcBufferRowBytes,
                   float* dstPixelData,
                   int dstBufferRowBytes,
                   const OfxRectI& dstBufferBounds)
    {
        _srcPixelData = srcPixelData;
        _srcBufferBounds = srcBufferBounds;
        _srcBufferRowBytes = srcBufferRowBytes;
        _dstBufferRowBytes = dstBufferRowBytes;

        _dstBounds = dstBufferBounds;
        _dstPixelData = dstPixelData;
    }

    // and do some processing
    void multiThreadProcessImages(const OfxRectI& procWindow, const OfxPointD& rs)
    {
        unused(rs);
        assert(nSrcComp == 1 || nSrcComp == 2 || nSrcComp == 3 || nSrcComp == 4);
        assert(nDstComp == 1 || nDstComp == 2 || nDstComp == 3 || nDstComp == 4);

        for (int dsty = procWindow.y1; dsty < procWindow.y2; ++dsty) {
            if (_effect.abort()) {
                break;
            }

            // the source is top-down: its first row is y = _srcBufferBounds.y2 - 1
            assert(_srcBufferBounds.y1 <= dsty && dsty < _srcBufferBounds.y2 && _dstBounds.y1 <= dsty && dsty < _dstBounds.y2);
            assert(_srcBufferBounds.x1 <= procWindow.x1 && procWindow.x2 <= _srcBufferBounds.x2);
            assert(_dstBounds.x1 <= procWindow.x1 && procWindow.x2 <= _dstBounds.x2);
            const int srcRow = _srcBufferBounds.y2 - 1 - dsty;
            // the first pixel of the row in each buffer (at x = bounds.x1); signed, as the row bytes may be negative
            float* dst_pixels = (float*)((char*)_dstPixelData + (std::ptrdiff_t)(dsty - _dstBounds.y1) * _dstBufferRowBytes);
            const SRCPIX* src_pixels = (const SRCPIX*)((const char*)_srcPixelData + (std::ptrdiff_t)srcRow * _srcBufferRowBytes);

            assert(dst_pixels && src_pixels);

            for (int x = procWindow.x1; x < procWindow.x2; ++x) {
                const size_t srcCol = (size_t)(x - _srcBufferBounds.x1) * nSrcComp;
                const size_t dstCol = (size_t)(x - _dstBounds.x1) * nDstComp;

                switch (nSrcComp) {
                case 1:
                    // alpha
                    switch (nDstComp) {
                    case 1:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        break;
                    case 2:
                        dst_pixels[dstCol + 0] = 0.;
                        dst_pixels[dstCol + 1] = 0.;
                        break;

                    case 3:
                        dst_pixels[dstCol + 0] = 0.;
                        dst_pixels[dstCol + 1] = 0.;
                        dst_pixels[dstCol + 2] = 0.;
                        break;

                    case 4:
                        dst_pixels[dstCol + 0] = 0.;
                        dst_pixels[dstCol + 1] = 0.;
                        dst_pixels[dstCol + 2] = 0.;
                        dst_pixels[dstCol + 3] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        break;
                    default:
                        assert(false);

                        return;
                    }
                    break;
                case 2:
                    // XY
                    switch (nDstComp) {
                    case 1:
                        dst_pixels[dstCol + 0] = 0.;
                        break;
                    case 2:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        break;

                    case 3:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 2] = 0;
                        break;

                    case 4:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 2] = 0.;
                        dst_pixels[dstCol + 3] = 1.f;
                        break;
                    default:
                        assert(false);

                        return;
                    }

                    break;
                case 3:
                    // RGB
                    switch (nDstComp) {
                    case 1: {
                        dst_pixels[dstCol + 0] = 0.;
                        break;
                    }
                    case 2:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        break;

                    case 3:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 2] = src_pixels[srcCol + 2] * (1.f / srcMaxValue);
                        break;

                    case 4:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 2] = src_pixels[srcCol + 2] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 3] = 1.f;
                        break;
                    default:
                        assert(false);

                        return;
                    }

                    break;

                case 4:
                    switch (nDstComp) {
                    case 1:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 3] * (1.f / srcMaxValue);
                        break;
                    case 2:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        break;

                    case 3:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 2] = src_pixels[srcCol + 2] * (1.f / srcMaxValue);
                        break;

                    case 4:
                        dst_pixels[dstCol + 0] = src_pixels[srcCol + 0] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 1] = src_pixels[srcCol + 1] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 2] = src_pixels[srcCol + 2] * (1.f / srcMaxValue);
                        dst_pixels[dstCol + 3] = src_pixels[srcCol + 3] * (1.f / srcMaxValue);
                        break;
                    default:
                        assert(false);

                        return;
                    }

                    break;
                default:
                    assert(false);

                    return;
                } // switch
            }
        }
    } // multiThreadProcessImages
};

template <typename SRCPIX, int srcMaxValue, int nSrcComp, int nDstComp>
void
convertForDstNComps(ImageEffect* effect,
                    const SRCPIX* srcPixelData,
                    const OfxRectI& renderWindow,
                    const OfxPointD& renderScale,
                    const OfxRectI& srcBounds,
                    int srcRowBytes,
                    float* dstPixelData,
                    const OfxRectI& dstBounds,
                    int dstRowBytes)
{
    PixelConverterProcessor<SRCPIX, srcMaxValue, nSrcComp, nDstComp> p(*effect);
    p.setValues(srcPixelData, srcBounds, srcRowBytes, dstPixelData, dstRowBytes, dstBounds);
    p.setRenderWindow(renderWindow, renderScale);
    p.process();
}

template <typename SRCPIX, int srcMaxValue, int nSrcComp>
void
convertForSrcNComps(ImageEffect* effect,
                    const SRCPIX* srcPixelData,
                    const OfxRectI& renderWindow,
                    const OfxPointD& renderScale,
                    const OfxRectI& srcBounds,
                    int srcRowBytes,
                    float* dstPixelData,
                    const OfxRectI& dstBounds,
                    PixelComponentEnum dstPixelComponents,
                    int dstRowBytes)
{
    switch (dstPixelComponents) {
    case ePixelComponentAlpha: {
        convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 1>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        break;
    }
    case ePixelComponentXY: {
        convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 2>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        break;
    }
    case ePixelComponentRGB: {
        convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 3>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        break;
    }
    case ePixelComponentRGBA: {
        convertForDstNComps<SRCPIX, srcMaxValue, nSrcComp, 4>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstRowBytes);
        break;
    }
    default:
        assert(false);
        break;
    }
}

template <typename SRCPIX, int srcMaxValue>
void
convertForDepth(ImageEffect* effect,
                const SRCPIX* srcPixelData,
                const OfxRectI& renderWindow,
                const OfxPointD& renderScale,
                const OfxRectI& srcBounds,
                PixelComponentEnum srcPixelComponents,
                int srcRowBytes,
                float* dstPixelData,
                const OfxRectI& dstBounds,
                PixelComponentEnum dstPixelComponents,
                int dstRowBytes)
{
    switch (srcPixelComponents) {
    case ePixelComponentAlpha:
        convertForSrcNComps<SRCPIX, srcMaxValue, 1>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    case ePixelComponentXY:
        convertForSrcNComps<SRCPIX, srcMaxValue, 2>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    case ePixelComponentRGB:
        convertForSrcNComps<SRCPIX, srcMaxValue, 3>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    case ePixelComponentRGBA:
        convertForSrcNComps<SRCPIX, srcMaxValue, 4>(effect, srcPixelData, renderWindow, renderScale, srcBounds, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    default:
        assert(false);
        break;
    }
}

void
GenericReaderPlugin::convertDepthAndComponents(const void* srcPixelData,
                                               const OfxRectI& renderWindow,
                                               const OfxPointD& renderScale,
                                               const OfxRectI& srcBounds,
                                               PixelComponentEnum srcPixelComponents,
                                               BitDepthEnum srcBitDepth,
                                               int srcRowBytes,
                                               float* dstPixelData,
                                               const OfxRectI& dstBounds,
                                               PixelComponentEnum dstPixelComponents,
                                               int dstRowBytes)
{
    switch (srcBitDepth) {
    case eBitDepthFloat:
        convertForDepth<float, 1>(this, (const float*)srcPixelData, renderWindow, renderScale, srcBounds, srcPixelComponents, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    case eBitDepthUShort:
        convertForDepth<unsigned short, 65535>(this, (const unsigned short*)srcPixelData, renderWindow, renderScale, srcBounds, srcPixelComponents, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    case eBitDepthUByte:
        convertForDepth<unsigned char, 255>(this, (const unsigned char*)srcPixelData, renderWindow, renderScale, srcBounds, srcPixelComponents, srcRowBytes, dstPixelData, dstBounds, dstPixelComponents, dstRowBytes);
        break;
    default:
        assert(false);
        break;
    }
}

void
GenericReaderDescribe(ImageEffectDescriptor& desc,
                      const std::vector<string>& extensions,
                      int evaluation,
                      bool supportsTiles,
                      bool multiPlanar)
{
    desc.setPluginGrouping(kPluginGrouping);

#ifdef OFX_EXTENSIONS_TUTTLE
    desc.addSupportedContext(eContextReader);
#endif
    desc.addSupportedContext(eContextGenerator);
    desc.addSupportedContext(eContextGeneral);

    // add supported pixel depths
    // desc.addSupportedBitDepth( eBitDepthUByte );
    // desc.addSupportedBitDepth( eBitDepthUShort );
    desc.addSupportedBitDepth(eBitDepthFloat);

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(false);

    desc.setSupportsMultiResolution(kSupportsMultiResolution);

    desc.setSupportsTiles(supportsTiles);
    desc.setTemporalClipAccess(false); // say we will not be doing random time access on clips
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(true); // plugin may setPixelAspectRatio on output clip
    desc.setRenderThreadSafety(eRenderFullySafe);

#ifdef OFX_EXTENSIONS_NUKE
    if (getImageEffectHostDescription()
        && getImageEffectHostDescription()->isMultiPlanar) {
        desc.setIsMultiPlanar(multiPlanar);
        if (multiPlanar) {
            // We let all un-rendered planes pass-through so that they can be retrieved below by a shuffle node
            desc.setPassThroughForNotProcessedPlanes(ePassThroughLevelPassThroughNonRenderedPlanes);
        }
        desc.setIsViewAware(true);
        desc.setIsViewInvariant(eViewInvarianceAllViewsVariant);
    }
#endif
#ifdef OFX_EXTENSIONS_TUTTLE
    desc.addSupportedExtensions(extensions);
    desc.setPluginEvaluation(evaluation);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    desc.setChannelSelector(ePixelComponentNone);
#endif
}

PageParamDescriptor*
GenericReaderDescribeInContextBegin(ImageEffectDescriptor& desc,
                                    ContextEnum /*context*/,
                                    bool /*isVideoStreamPlugin*/,
                                    bool supportsRGBA,
                                    bool supportsRGB,
                                    bool supportsXY,
                                    bool supportsAlpha,
                                    bool supportsTiles,
                                    bool addSeparatorAfterLastParameter)
{
    gHostIsNatron = (getImageEffectHostDescription()->isNatron);

    // recomputed at each describe: the host description is fetched again after each load
    // computed in locals and assigned once below, so that a concurrent instance never sees a transient false
    bool supportsRGBAHost = false;
    bool supportsRGBHost = false;
    bool supportsXYHost = false;
    bool supportsAlphaHost = false;
    for (ImageEffectHostDescription::PixelComponentArray::const_iterator it = getImageEffectHostDescription()->_supportedComponents.begin();
         it != getImageEffectHostDescription()->_supportedComponents.end();
         ++it) {
        switch (*it) {
        case ePixelComponentRGBA:
            supportsRGBAHost = true;
            break;
        case ePixelComponentRGB:
            supportsRGBHost = true;
            break;
        case ePixelComponentXY:
            supportsXYHost = true;
            break;
        case ePixelComponentAlpha:
            supportsAlphaHost = true;
            break;
        default:
            // other components are not supported by this plugin
            break;
        }
    }
    gHostSupportsRGBA = supportsRGBAHost;
    gHostSupportsRGB = supportsRGBHost;
    gHostSupportsXY = supportsXYHost;
    gHostSupportsAlpha = supportsAlphaHost;

    // make some pages and to things in
    PageParamDescriptor* page = desc.definePageParam("Controls");

    // create the optional source clip
    ClipDescriptor* srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    if (supportsRGBA) {
        srcClip->addSupportedComponent(ePixelComponentRGBA);
    }
    if (supportsRGB) {
        srcClip->addSupportedComponent(ePixelComponentRGB);
    }
    if (supportsXY) {
        srcClip->addSupportedComponent(ePixelComponentXY);
    }
    if (supportsAlpha) {
        srcClip->addSupportedComponent(ePixelComponentAlpha);
    }
    srcClip->setSupportsTiles(supportsTiles);
    srcClip->setOptional(true);

    // create the mandated output clip
    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    if (supportsRGBA) {
        dstClip->addSupportedComponent(ePixelComponentRGBA);
    }
    if (supportsRGB) {
        dstClip->addSupportedComponent(ePixelComponentRGB);
    }
    if (supportsXY) {
        dstClip->addSupportedComponent(ePixelComponentXY);
    }
    if (supportsAlpha) {
        dstClip->addSupportedComponent(ePixelComponentAlpha);
    }
    dstClip->setSupportsTiles(supportsTiles);

    //////////Input file
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamFilename);
        param->setLabelAndHint(kParamFilenameLabel, kParamFilenameHint);
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(true);
        // in the Reader context, the script name must be kOfxImageEffectFileParamName, @see kOfxImageEffectContextReader
        param->setScriptName(kParamFilename);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    //////////First-frame
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamFirstFrame);
        param->setLabelAndHint(kParamFirstFrameLabel, kParamFirstFrameHint);
        param->setDefault(0);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Before first
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamBefore);
        param->setLabelAndHint(kParamBeforeLabel, kParamBeforeHint);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterHold);
        param->appendOption(kReaderOptionHold);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterLoop);
        param->appendOption(kReaderOptionLoop);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterBounce);
        param->appendOption(kReaderOptionBounce);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterBlack);
        param->appendOption(kReaderOptionBlack);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterError);
        param->appendOption(kReaderOptionError);
        param->setAnimates(true);
        param->setDefault(GenericReaderPlugin::eBeforeAfterHold);
        if (page) {
            page->addChild(*param);
        }
    }

    //////////Last-frame
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamLastFrame);
        param->setLabelAndHint(kParamLastFrameLabel, kParamLastFrameHint);
        param->setDefault(0);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////After first
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamAfter);
        param->setLabel(kParamAfterLabel);
        param->setHint(kParamAfterHint);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterHold);
        param->appendOption(kReaderOptionHold);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterLoop);
        param->appendOption(kReaderOptionLoop);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterBounce);
        param->appendOption(kReaderOptionBounce);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterBlack);
        param->appendOption(kReaderOptionBlack);
        assert(param->getNOptions() == GenericReaderPlugin::eBeforeAfterError);
        param->appendOption(kReaderOptionError);
        param->setAnimates(true);
        param->setDefault(GenericReaderPlugin::eBeforeAfterHold);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Missing frame choice
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOnMissingFrame);
        param->setLabel(kParamOnMissingFrameLabel);
        param->setHint(kParamOnMissingFrameHint);
        assert(param->getNOptions() == GenericReaderPlugin::eMissingPrevious);
        param->appendOption(kReaderOptionPrevious);
        assert(param->getNOptions() == GenericReaderPlugin::eMissingNext);
        param->appendOption(kReaderOptionNext);
        assert(param->getNOptions() == GenericReaderPlugin::eMissingNearest);
        param->appendOption(kReaderOptionNearest);
        assert(param->getNOptions() == GenericReaderPlugin::eMissingError);
        param->appendOption(kReaderOptionError);
        assert(param->getNOptions() == GenericReaderPlugin::eMissingBlack);
        param->appendOption(kReaderOptionBlack);
        param->setAnimates(true);
        param->setDefault(GenericReaderPlugin::eMissingError);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Frame-mode
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamFrameMode);
        param->setLabel(kParamFrameModeLabel);
        assert(param->getNOptions() == eFrameModeStartingTime);
        param->appendOption(kParamFrameModeOptionStartingTime);
        assert(param->getNOptions() == eFrameModeTimeOffset);
        param->appendOption(kParamFrameModeOptionTimeOffset);
        param->setAnimates(false);
        param->setDefault(eFrameModeStartingTime);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Starting frame
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamStartingTime);
        param->setLabel(kParamStartingTimeLabel);
        param->setHint(kParamStartingTimeHint);
        param->setDefault(0);
        param->setAnimates(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Time offset
    {
        IntParamDescriptor* param = desc.defineIntParam(kParamTimeOffset);
        param->setLabel(kParamTimeOffsetLabel);
        param->setHint(kParamTimeOffsetHint);
        param->setDefault(0);
        param->setAnimates(false);
        // param->setIsSecretAndDisabled(true); // done in the plugin constructor
        if (page) {
            page->addChild(*param);
        }
    }

    /////////// Secret param set to true if the time domain was edited by the user
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamTimeDomainUserEdited);
        param->setLabel(kParamTimeDomainUserEdited);
        param->setIsSecretAndDisabled(true); // always secret
        param->setDefault(false);
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }

    ///////////Original frame range
    {
        Int2DParamDescriptor* param = desc.defineInt2DParam(kParamOriginalFrameRange);
        param->setLabel(kParamOriginalFrameRangeLabel);
        param->setDefault(INT_MIN, INT_MAX);
        param->setAnimates(true);
        param->setIsSecretAndDisabled(true); // always secret
        param->setIsPersistent(false);
        if (page) {
            page->addChild(*param);
        }
    }

    //////////Input proxy file
    {
        StringParamDescriptor* param = desc.defineStringParam(kParamProxy);
        param->setLabel(kParamProxyLabel);
        param->setStringType(eStringTypeFilePath);
        param->setFilePathExists(true);
        param->setHint(kParamProxyHint);
        // in the Reader context, the script name must be kOfxImageEffectFileParamName, @see kOfxImageEffectContextReader
        param->setScriptName(kParamProxy);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    ////Proxy original scale
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamOriginalProxyScale);
        param->setLabel(kParamOriginalProxyScaleLabel);
        param->setDefault(1., 1.);
        param->setRange(0., 0., 1., 1.);
        param->setDisplayRange(0., 0., 1., 1.);
        param->setIsSecretAndDisabled(true); // always secret
        param->setHint(kParamOriginalProxyScaleHint);
        // param->setLayoutHint(eLayoutHintNoNewLine, 1);
        param->setDoubleType(eDoubleTypeScale);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }

    ////Proxy  scale threshold
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamProxyThreshold);
        param->setLabel(kParamProxyThresholdLabel);
        param->setDefault(1., 1.);
        param->setRange(0.01, 0.01, 1., 1.);
        param->setDisplayRange(0.01, 0.01, 1., 1.);
        // param->setIsSecretAndDisabled(true); // done in the plugin constructor
        param->setHint(kParamProxyThresholdHint);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        param->setDoubleType(eDoubleTypeScale);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }

    /// Enable custom proxy scale
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamCustomProxyScale);
        param->setLabel(kParamCustomProxyScaleLabel);
        // param->setIsSecretAndDisabled(true); // done in the plugin constructor
        param->setDefault(false);
        param->setHint(kParamCustomProxyScaleHint);
        param->setAnimates(false);
        param->setEvaluateOnChange(false);
        if (page) {
            page->addChild(*param);
        }
    }

    //// File premult
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamFilePremult);
        param->setLabel(kParamFilePremultLabel);
        param->setHint(kParamFilePremultHint);
        assert(param->getNOptions() == eImageOpaque);
        param->appendOption(premultString(eImageOpaque), kParamFilePremultOptionOpaqueHint);
        if (gHostSupportsRGBA && supportsRGBA) {
            assert(param->getNOptions() == eImagePreMultiplied);
            param->appendOption(premultString(eImagePreMultiplied), kParamFilePremultOptionPreMultipliedHint);
            assert(param->getNOptions() == eImageUnPreMultiplied);
            param->appendOption(premultString(eImageUnPreMultiplied), kParamFilePremultOptionUnPreMultipliedHint);
            param->setDefault(eImagePreMultiplied); // images should be premultiplied in a compositing context
        }
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    //// Output premult
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputPremult);
        param->setLabel(kParamOutputPremultLabel);
        param->setHint(kParamOutputPremultHint);
        assert(param->getNOptions() == eImageOpaque);
        param->appendOption(premultString(eImageOpaque), kParamFilePremultOptionOpaqueHint);
        if (gHostSupportsRGBA && supportsRGBA) {
            assert(param->getNOptions() == eImagePreMultiplied);
            param->appendOption(premultString(eImagePreMultiplied), kParamFilePremultOptionPreMultipliedHint);
            assert(param->getNOptions() == eImageUnPreMultiplied);
            param->appendOption(premultString(eImageUnPreMultiplied), kParamFilePremultOptionUnPreMultipliedHint);
            param->setDefault(eImagePreMultiplied); // images should be premultiplied in a compositing context
        }
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    //// Output components
    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputComponents);
        param->setLabel(kParamOutputComponentsLabel);
        param->setHint(kParamOutputComponentsHint);

        // must be in sync with the building of _outputComponentsTable in the GenericReaderPlugin constructor
        if (gHostSupportsRGBA && supportsRGBA) {
            param->appendOption(kParamOutputComponentsOptionRGBA);
        }
        if (gHostSupportsRGB && supportsRGB) {
            param->appendOption(kParamOutputComponentsOptionRGB);
        }
        if (gHostSupportsXY && supportsXY) {
            param->appendOption(kParamOutputComponentsOptionXY);
        }

        if (gHostSupportsAlpha && supportsAlpha) {
            param->appendOption(kParamOutputComponentsOptionAlpha);
        }

        param->setDefault(0); // default to the first one available, i.e. the most chromatic
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    /// Frame rate
    {
        DoubleParamDescriptor* param = desc.defineDoubleParam(kParamFrameRate);
        param->setLabel(kParamFrameRateLabel);
        param->setHint(kParamFrameRateHint);
        param->setEvaluateOnChange(false);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);
        // param->setEnabled(false); // done in the restoreStateFromParams()
        param->setDefault(24.);
        param->setRange(0., DBL_MAX);
        param->setDisplayRange(0., 300.);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    /// Custom FPS
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamCustomFps);
        param->setLabel(kParamCustomFpsLabel);
        param->setHint(kParamCustomFpsHint);
        param->setEvaluateOnChange(false);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (addSeparatorAfterLastParameter) {
            // param->setLayoutHint(eLayoutHintDivider);
        }
        if (page) {
            page->addChild(*param);
        }
    }

    // sublabel
    if (gHostIsNatron) {
        StringParamDescriptor* param = desc.defineStringParam(kNatronOfxParamStringSublabelName);
        param->setIsSecretAndDisabled(true); // always secret
        param->setIsPersistent(false);
        param->setEvaluateOnChange(false);
        // param->setDefault();
        if (page) {
            page->addChild(*param);
        }
    }

    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamGuessedParams);
        param->setEvaluateOnChange(false);
        param->setAnimates(false);
        param->setIsSecretAndDisabled(true);
        param->setDefault(false);
        if (page) {
            page->addChild(*param);
        }
    }

    return page;
} // GenericReaderDescribeInContextBegin

void
GenericReaderDescribeInContextEnd(ImageEffectDescriptor& desc,
                                  ContextEnum context,
                                  PageParamDescriptor* page,
                                  const char* inputSpaceNameDefault,
                                  const char* outputSpaceNameDefault)
{
#ifdef OFX_IO_USING_OCIO
    // insert OCIO parameters
    GenericOCIO::describeInContextInput(desc, context, page, inputSpaceNameDefault, kParamInputSpaceLabel);
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamInputSpaceSet);
        param->setEvaluateOnChange(false);
        param->setAnimates(false);
        param->setIsSecretAndDisabled(true);
        param->setDefault(false);
        if (page) {
            page->addChild(*param);
        }
    }
    GenericOCIO::describeInContextOutput(desc, context, page, outputSpaceNameDefault);
    GenericOCIO::describeInContextContext(desc, context, page);
    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kOCIOHelpButton);
        param->setLabel(kOCIOHelpButtonLabel);
        param->setHint(kOCIOHelpButtonHint);
        if (page) {
            page->addChild(*param);
        }
    }
#endif
}

bool
iequals(const string& a,
        const string& b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = ((a[i] >= 'A') && (a[i] <= 'Z')) ? (char)(a[i] - 'A' + 'a') : a[i];
        const char cb = ((b[i] >= 'A') && (b[i] <= 'Z')) ? (char)(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) {
            return false;
        }
    }

    return true;
}

namespace {
/*
 * @brief Remap channel, to a known channel name, that is a single upper case letter
 */
static string
remapToKnownChannelName(const string& channel)
{
    if ((channel == "r") || (channel == "red") || (channel == "RED") || (channel == "Red")) {
        return "R";
    }

    if ((channel == "g") || (channel == "green") || (channel == "GREEN") || (channel == "Green")) {
        return "G";
    }

    if ((channel == "b") || (channel == "blue") || (channel == "BLUE") || (channel == "Blue")) {
        return "B";
    }

    if ((channel == "a") || (channel == "alpha") || (channel == "ALPHA") || (channel == "Alpha")) {
        return "A";
    }

    if ((channel == "z") || (channel == "depth") || (channel == "DEPTH") || (channel == "Depth")) {
        return "Z";
    }

    return channel;
}

/// Returns true if one is found
static bool
hasDuplicate(const LayersMap& layers,
             const string& layer,
             const string& channel)
{
    // Try to find an existing layer, or a duplicate
    for (std::size_t c = 0; c < layers.size(); ++c) {
        if (layers[c].first == layer) {
            for (std::size_t i = 0; i < layers[c].second.channelNames.size(); ++i) {
                if (layers[c].second.channelNames[i] == channel) {
                    return true;
                }
            }
            break;
        }
    }

    return false;
}

/// encodedLayerName is in the format view.layer.channel
static void
extractLayerName(const string& encodedLayerName,
                 const vector<string>& viewsList,
                 string* viewName,
                 string* layerName,
                 string* channelName)
{
    /// if there is a layer/view prefix, this will be non empty
    string layerDotPrefix;
    size_t lastdot = encodedLayerName.find_last_of(".");

    if (lastdot != string::npos) {
        layerDotPrefix = encodedLayerName.substr(0, lastdot);
        *channelName = encodedLayerName.substr(lastdot + 1);
        *channelName = remapToKnownChannelName(*channelName);
    } else {
        *channelName = encodedLayerName;
        *channelName = remapToKnownChannelName(*channelName);

        return;
    }

    if (viewsList.empty()) {
        // The view meta-data is not set, assume that everything prefixing the last '.' is a layer name
        *layerName = layerDotPrefix;
    } else {
        /*According to OpenEXR spec :

        http://www.openexr.com/MultiViewOpenEXR.pdf

        The view name must be the ultimate layer name, that is, the penultimate
        period-delimited component in each channel name. In other words, the
        view name is followed by a period and a final channel name in the
        format layer.view.channel or view.channel.*/

        size_t firstDot = layerDotPrefix.find_last_of(".", lastdot - 1);
        if (firstDot != string::npos) {

            *viewName = layerDotPrefix.substr(firstDot + 1, lastdot - (firstDot + 1));

            // Check if the view name matches an existing view
            bool foundView = false;
            for (std::size_t i = 0; i < viewsList.size(); ++i) {
                if (iequals(viewsList[i], *viewName)) {
                    foundView = true;
                    *viewName = viewsList[i]; // the declared view name: getImageLayers() compares view names exactly
                    break;
                }
            }
            if (foundView) {
                *layerName = layerDotPrefix.substr(0, firstDot);
            } else {
                viewName->clear();
                *layerName = layerDotPrefix;
                // Nuke writes view.layer.channel instead (e.g. "right.depth.Z", with the hero view's channels
                // unprefixed): if the first component is a view, that's the view and the rest is the layer.
                size_t viewDot = layerDotPrefix.find('.');
                assert(viewDot != string::npos); // firstDot != npos
                const string firstComponent = layerDotPrefix.substr(0, viewDot);
                for (std::size_t i = 0; i < viewsList.size(); ++i) {
                    if (iequals(viewsList[i], firstComponent)) {
                        *viewName = viewsList[i];
                        *layerName = layerDotPrefix.substr(viewDot + 1);
                        break;
                    }
                }
                // otherwise: no matching view, assume that this is the layer name and add the layer to the
                // "default" view
            }
        } else {
            *layerName = layerDotPrefix;
        }
    }
}

// e.g: find "X" in view.layer.z
static bool
hasChannelName(const vector<string>& viewsList,
               const string& viewName,
               const string& layerName,
               const string& mappedChannelName,
               const vector<string>& originalUnMappedNames)
{
    for (std::size_t i = 0; i < originalUnMappedNames.size(); ++i) {
        string view, layer, channel;
        extractLayerName(originalUnMappedNames[i], viewsList, &view, &layer, &channel);
        if ((viewName != view) || (layerName != layer)) {
            continue;
        }
        if (channel == mappedChannelName) {
            return true;
        }
    }

    return false;
}

} // anon namespace

/// layersUnion:
/// Union all layers across views to build the layers choice.
/// This is because we cannot provide a choice with different entries across views, so if there are some disparities,
/// let the render action just return a black image if the layer requested cannot be found for the given view.
void
getImageLayers(const vector<ImagePartChannels>& parts,
               ViewsLayersMap* layersMap,
               LayersUnionVect* layersUnion)
{
    assert(!parts.empty());

    vector<string> views;

    /*
       First off, detect views.
     */
    vector<string> partsViewAttribute;
    if (parts.size() == 1) {
        // the "multiView" attribute
        for (std::size_t i = 0; i < parts[0].multiView.size(); ++i) {
            const string& view = parts[0].multiView[i];
            if (!view.empty()) {
                if (std::find(views.begin(), views.end(), view) == views.end()) {
                    views.push_back(view);
                }
            }
        }
    } else {
        // Check for each part the "view" attribute
        partsViewAttribute.resize(parts.size());
        for (std::size_t i = 0; i < parts.size(); ++i) {
            const string& view = parts[i].view;
            if (!view.empty()) {
                if (std::find(views.begin(), views.end(), view) == views.end()) {
                    views.push_back(view);
                }
                partsViewAttribute[i] = view;
            }
        }
    }

    for (std::size_t i = 0; i < views.size(); ++i) {
        layersMap->push_back(make_pair(views[i], LayersMap()));
    }

    if (views.empty()) {
        layersMap->push_back(make_pair("Main", LayersMap()));
    }

    /// Layers are considered to be named as view.layer.channels. If no view prefix then it is considered to be part of the "main" view
    /// that is, the first view declared.

    for (std::size_t i = 0; i < parts.size(); ++i) {
        for (int j = 0; j < (int)parts[i].channelNames.size(); ++j) {
            const string& layerChanName = parts[i].channelNames[j];

            // Extract the view layer and channel to our format so we can compare strings
            string originalView, originalLayer, channel;
            extractLayerName(layerChanName, views, &originalView, &originalLayer, &channel);
            if (channel.empty()) {
                // a name that ends with '.' ("", ".", "layer."): not a channel of a layer
                continue;
            }
            string view = originalView;
            string layer = originalLayer;

            if (view.empty() && !partsViewAttribute.empty() && (i < partsViewAttribute.size()) && !partsViewAttribute[i].empty()) {
                view = partsViewAttribute[i];
            }
            if (view.empty() && !layer.empty()) {
                /// Check if the layer we parsed is in fact not a view name
                // Note: This code was removed in commit https://github.com/NatronGitHub/openfx-io/commit/79ac6546e0a1aed7f14fd15fbc3dfd634b91d4c5
                // but it is necessary to read all views of multiview EXRs such
                // as https://github.com/openexr/openexr-images/raw/master/MultiView/Balls.exr
                // see issue https://github.com/NatronGitHub/Natron/issues/429
                for (std::size_t v = 0; v < views.size(); ++v) {
                    if (iequals(views[v], layer)) {
                        view = views[v]; // the declared view name (layersMap is looked up exactly)
                        layer.clear();
                        break;
                    }
                }
            }

            ViewsLayersMap::iterator foundView = layersMap->end();
            if (view.empty()) {
                /// Set to main view (view 0)
                foundView = layersMap->begin();
            } else {
                for (ViewsLayersMap::iterator it = layersMap->begin(); it != layersMap->end(); ++it) {
                    if (it->first == view) {
                        foundView = it;
                        break;
                    }
                }
            }
            if (foundView == layersMap->end()) {
                // The view does not exist in the metadata, this is probably a channel named aaa.bbb.c, just concatenate aaa.bbb as a single layer name
                // and put it in the "Main" view
                layer = view + "." + layer;
                view.clear();
                foundView = layersMap->begin();
            }

            if (layer.empty()) {
                // The layer name is empty, for OpenEXR 2 files, check for the "name" attribute (converted to oiio:subimagename by OIIO) which may contain the layer name.
                layer = parts[i].name;
            }

            assert(foundView != layersMap->end());

            // If the layer name is empty, try to map it to something known
            if (layer.empty()) {
                // channel  has already been remapped to our formatting of channels, i.e: 1 upper-case letter
                if ((channel == "R") || (channel == "G") || (channel == "B") || (channel == "A") || (channel == "I")) {
                    layer = kReaderColorLayer;
                } else if (channel == "X") {
                    // try to put XYZ together, unless Z is alone
                    bool hasY = hasChannelName(views, originalView, originalLayer, "Y", parts[i].channelNames);
                    bool hasZ = hasChannelName(views, originalView, originalLayer, "Z", parts[i].channelNames);
                    if (hasY && hasZ) {
                        layer = kReaderXYZLayer;
                    }
                } else if (channel == "Y") {
                    // try to put XYZ together, unless Z is alone
                    bool hasX = hasChannelName(views, originalView, originalLayer, "X", parts[i].channelNames);
                    bool hasZ = hasChannelName(views, originalView, originalLayer, "Z", parts[i].channelNames);
                    if (hasX && hasZ) {
                        layer = kReaderXYZLayer;
                    } else {
                        bool hasR = hasChannelName(views, originalView, originalLayer, "R", parts[i].channelNames);
                        bool hasG = hasChannelName(views, originalView, originalLayer, "G", parts[i].channelNames);
                        bool hasB = hasChannelName(views, originalView, originalLayer, "B", parts[i].channelNames);
                        bool hasI = hasChannelName(views, originalView, originalLayer, "I", parts[i].channelNames);
                        if (!hasR && !hasG && !hasB && !hasI) {
                            // Y is for luminance in this case
                            layer = kReaderColorLayer;
                        }
                    }
                } else if (channel == "Z") {
                    // try to put XYZ together, unless Z is alone
                    bool hasX = hasChannelName(views, originalView, originalLayer, "X", parts[i].channelNames);
                    bool hasY = hasChannelName(views, originalView, originalLayer, "Y", parts[i].channelNames);
                    if (hasX && hasY) {
                        layer = kReaderXYZLayer;
                    } else {
                        layer = kReaderDepthLayer;
                    }
                }
            }

            // The layer is still empty, put the channel alone in a new layer
            if (layer.empty()) {
                layer = channel;
            }

            // There may be duplicates, e.g: 2 parts of a EXR file with same RGBA layer, we have no choice but to prepend the part index
            {
                int attempts = 1;
                string baseLayerName = layer;
                while (hasDuplicate(foundView->second, layer, channel)) {
                    stringstream ss;

                    ss << "Part" << attempts;

                    ss << '.' << baseLayerName;
                    layer = ss.str();
                    ++attempts;
                }
            }

            assert(!layer.empty());

            int layerIndex = -1;
            for (std::size_t c = 0; c < foundView->second.size(); ++c) {
                if (foundView->second[c].first == layer) {
                    layerIndex = (int)c;
                    break;
                }
            }
            if (layerIndex == -1) {
                foundView->second.push_back(make_pair(layer, LayerChannelIndexes()));
                layerIndex = (int)foundView->second.size() - 1;
            }
            // Now we are sure there are no duplicates
            foundView->second[layerIndex].second.subImageIdx = i;
            foundView->second[layerIndex].second.channelIndexes.push_back(j);
            foundView->second[layerIndex].second.channelNames.push_back(channel);
        } // for (int j = 0; j < (int)parts[i].channelNames.size(); ++j) {
    } // for (std::size_t i = 0; i < parts.size(); ++i) {

    /// Union all layers across views
    if (layersUnion) {
        for (ViewsLayersMap::iterator it = layersMap->begin(); it != layersMap->end(); ++it) {
            for (LayersMap::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
                LayersUnionVect::iterator found = layersUnion->end();
                for (LayersUnionVect::iterator it3 = layersUnion->begin(); it3 != layersUnion->end(); ++it3) {
                    if (it3->first == it2->first) {
                        found = it3;
                        break;
                    }
                }

                if (found == layersUnion->end()) {
                    // We did not find a view in the layersUnion with this name
                    LayerUnionData d;
                    d.layer = it2->second;
                    d.views.push_back(it->first);
                    layersUnion->push_back(make_pair(it2->first, d));
                } else {
                    // We already found a view in the layersUnion with this name
                    if (views.size() > 1) {
                        // register views that have this layer
                        found->second.views.push_back(it->first);
                    }
                }
            }
        }
    }
} // getImageLayers

ViewsLayersMap::const_iterator
findImageView(const ViewsLayersMap& layersMap,
              const string& viewName,
              int view)
{
    assert(!layersMap.empty());
    for (ViewsLayersMap::const_iterator it = layersMap.begin(); it != layersMap.end(); ++it) {
        if (iequals(it->first, viewName)) {
            return it;
        }
    }
    /*
       We did not find the view by name. To offer some sort of compatibility and not fail, just load the view corresponding to the given
       index, even though the names do not match.
       If the index is out of range, just load the main view (index 0)
     */
    ViewsLayersMap::const_iterator foundView = layersMap.begin();
    if ((view >= 0) && (view < (int)layersMap.size())) {
        std::advance(foundView, view);
    }

    return foundView;
}

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT
