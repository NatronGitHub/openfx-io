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

#ifndef Io_GenericReader_h
#define Io_GenericReader_h

#include "IOUtility.h"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <ofxsImageEffect.h>
#include <ofxsMacros.h>

namespace SequenceParsing {
class SequenceFromFiles;
}

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

#ifdef OFX_IO_USING_OCIO
class GenericOCIO;
#endif

/**
 * @brief A generic reader plugin, derive this to create a new reader for a specific file format.
 * This class propose to handle the common stuff among readers:
 * - common params
 * - a tiny cache to speed-up the successive getRegionOfDefinition() calls
 * - a way to inform the host about the colour-space of the data.
 **/
class GenericReaderPlugin
    : public OFX::ImageEffect {
public:
    GenericReaderPlugin(OfxImageEffectHandle handle,
                        const std::vector<std::string>& extensions,
                        bool supportsRGBA,
                        bool supportsRGB,
                        bool supportsXY,
                        bool supportsAlpha,
                        bool supportsTiles,
                        bool isMultiPlanar);

    virtual ~GenericReaderPlugin();

    /**
     * @brief Don't override this function, the GenericReaderPlugin class already does the rendering. The "decoding" of the frame
     * must be done by the pure virtual function decode(...) instead.
     **/
    virtual void render(const OFX::RenderArguments& args) OVERRIDE FINAL;

    /**
     * @brief Don't override this. Basically this function will call getTimeDomainForVideoStream(...),
     * which your reader should implement to read from a video-stream the time range.
     * If the file is not a video stream, the function getTimeDomainForVideoStream() should return false, indicating that
     * we're reading a sequence of images and that the host should get the time domain for us.
     **/
    virtual bool getTimeDomain(OfxRangeD& range) OVERRIDE FINAL;

    /**
     * @brief Don't override this. If the pure virtual function areHeaderAndDataTied() returns true, this
     * function will call decode() to read the region of definition of the image and cache away the decoded image
     * into the _dstImg member.
     * If areHeaderAndDataTied() returns false instead, this function will call the virtual function
     * getFrameBounds() which should read the header of the image to only extract the bounds and PAR of the image.
     **/
    virtual bool getRegionOfDefinition(const OFX::RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL;

    /**
     * @brief You can override this to take actions in response to a param change.
     * Make sure you call the base-class version of this function at the end: i.e:
     *
     * void MyReader::changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName) {
     *      if (.....) {
     *
     *      } else if(.....) {
     *
     *      } else {
     *          GenericReaderPlugin::changedParam(args,paramName);
     *      }
     * }
     **/
    virtual void changedParam(const OFX::InstanceChangedArgs& args, const std::string& paramName) OVERRIDE;

    /* override is identity */
    virtual bool isIdentity(const OFX::IsIdentityArguments& args, OFX::Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE;

    /**
     * @brief Set the output components and premultiplication state for the input image automatically.
     * This is filled from directly the info returned by guessParamsFromFilename
     **/
    virtual void getClipPreferences(OFX::ClipPreferencesSetter& clipPreferences) OVERRIDE;

    /**
     * @brief Overridden to clear any OCIO cache.
     * This function calls clearAnyCache() if you have any cache to clear.
     **/
    virtual void purgeCaches(void) OVERRIDE;

    /**
     * @brief Restore any state from the parameters set
     * Called from createInstance() and changedParam() (via changedFilename()), must restore the
     * state of the Reader, such as Choice param options, data members and non-persistent param values.
     * We don't do this in the ctor of the plug-in since we can't call virtuals yet.
     * Any derived implementation must call GenericReaderPlugin::restoreStateFromParams() first
     **/
    virtual void restoreStateFromParams();

    bool isMultiPlanar() const
    {
        return _isMultiPlanar;
    }

    enum BeforeAfterEnum {
        eBeforeAfterHold,
        eBeforeAfterLoop,
        eBeforeAfterBounce,
        eBeforeAfterBlack,
        eBeforeAfterError,
    };

    // Options of the kParamOnMissingFrame ("onMissingFrame") choice param, in order.
    // Do not reorder: the value is saved in projects.
    enum MissingEnum {
        eMissingPrevious,
        eMissingNext,
        eMissingNearest,
        eMissingError, // the default
        eMissingBlack,
    };

protected:
    /**
     * @brief Called from changedParam() when kParamFilename is changed for any reason other than eChangeTime.
     * Calls restoreStateFromParams() to update any non-persistent params that may depend on the filename.
     * If reason is eChangeUserEdit and the params where never guessed (see _guessedParams) also sets these from the file contents.
     * Any derived implementation must call GenericReaderPlugin::changedFilename() first
     **/
    virtual void changedFilename(const OFX::InstanceChangedArgs& args);

    OFX::ChoiceParam* _missingFrameParam; //< what to do on missing frame

    /**
     * @brief Thrown by decode() or decodePlane() when the frame is missing from the file (e.g. a frame
     * outside of a video stream) and kParamOnMissingFrame is Black (eMissingBlack). render() catches it and
     * fills the render window of every output plane with transparent black (0,0,0,0), as for a missing
     * file in an image sequence: no colorspace conversion, no opaque alpha.
     * A frame that is in the file but fails to decode is an error, whatever kParamOnMissingFrame.
     **/
    class MissingFrameBlackException
        : public std::runtime_error
    {
    public:
        MissingFrameBlackException()
            : std::runtime_error("Missing frame")
        {
        }
    };

    OfxStatus getFilenameAtTime(double t, std::string* filename) const;

    int getStartingTime() const;

    // get the value of kParamOutputComponents as a OFX::PixelComponentEnum
    OFX::PixelComponentEnum getOutputComponents() const;

    // sets the value of kParamOutputComponents
    void setOutputComponents(OFX::PixelComponentEnum comps);

    struct PlaneToRender {
        float* pixelData;
        int rowBytes;
        int numChans;
        OFX::PixelComponentEnum comps;
        std::string rawComps;
    };

    /**
     * @brief The part of renderWindow that is in fileBounds (for decode() and decodePlane(), the bounds of the image
     * in the file that they opened). Returns false if it is empty (*window is then (0,0,0,0)).
     **/
    static bool intersectWindow(const OfxRectI& renderWindow,
                                const OfxRectI& fileBounds,
                                OfxRectI* window)
    {
        window->x1 = (std::max)(renderWindow.x1, fileBounds.x1);
        window->y1 = (std::max)(renderWindow.y1, fileBounds.y1);
        window->x2 = (std::min)(renderWindow.x2, fileBounds.x2);
        window->y2 = (std::min)(renderWindow.y2, fileBounds.y2);
        if ((window->x1 >= window->x2) || (window->y1 >= window->y2)) {
            window->x1 = window->y1 = window->x2 = window->y2 = 0;

            return false;
        }

        return true;
    }

    /**
     * @brief Clips the render window of decode() or decodePlane() to the image in the file that they actually opened,
     * and clears the rest of it. For the readers that do not write the whole render window themselves.
     *
     * - renderWindow, pixelData, bounds, pixelComponentCount and rowBytes are the arguments of decode() or
     *   decodePlane() (or of an image with the same layout: any depth, e.g. eBitDepthUByte, and any rowBytes,
     *   which may be padded or negative). bitDepth is the depth of pixelData (eBitDepthFloat for decode()).
     * - fileBounds is the bounds of the image in the file that was opened, in the same pixel coordinates. It may
     *   differ from the frame bounds that getFrameBounds() returned (and from which the host got the RoD) if the
     *   file changed since.
     * - *window is set to the part of renderWindow that is in fileBounds and in bounds: the reader writes the image
     *   there (and only there). Everything else in renderWindow (and in bounds) is set to 0 (transparent black), on
     *   the calling thread, with one memset per row, so that no pixel is written twice.
     * - Returns false if *window is empty (it is then (0,0,0,0), and the whole render window was cleared): the
     *   reader then has nothing to read.
     * - Called from decode() or decodePlane(), on the thread that render() called them on, it also tells render()
     *   that the image is only in *window (see setDecodedWindow()). So call it once per decode() or decodePlane(),
     *   with their renderWindow, on that thread, and not once per tile, chunk or line of the window: the last call
     *   (of clipToFile() or setDecodedWindow()) wins. A reader that also clears another buffer with it calls
     *   setDecodedWindow() afterwards, with the window that it wrote in decode()'s image.
     **/
    static bool clipToFile(const OfxRectI& renderWindow,
                           const OfxRectI& fileBounds,
                           void* pixelData,
                           const OfxRectI& bounds,
                           int pixelComponentCount,
                           OFX::BitDepthEnum bitDepth,
                           int rowBytes,
                           OfxRectI* window);

    /**
     * @brief Tells render() which part of the render window of decode() or decodePlane() comes from the file: the
     * rest is transparent black, e.g. because the file was rewritten smaller since getFrameBounds(). render()
     * then sets alpha = 1 for an Opaque file (kOfxImageOpaque) only in that window, and leaves the black area
     * transparent. clipToFile() calls it: a reader only calls it to report a larger window than the one that
     * clipToFile() returned (ReadOIIO: the black border that getFrameBounds() adds around the data window, which is
     * in the RoD), or if it does not use clipToFile(). By default (a reader that calls neither), the whole render
     * window comes from the file. It only has an effect when called from decode() or decodePlane(), on the thread
     * that render() called them on (once per call: the last one wins). An empty window (e.g. (0,0,0,0)) means that
     * nothing comes from the file.
     **/
    static void setDecodedWindow(const OfxRectI& window);

    /**
     * @brief Converts the renderWindow part of srcPixelData to dstPixelData (float).
     * srcPixelData is top-down: its first row is the top row of srcBounds (y = srcBounds.y2 - 1), and its first
     * pixel in a row is at x = srcBounds.x1. dstPixelData is bottom-up, with bounds dstBounds. renderWindow must be
     * in srcBounds and in dstBounds.
     **/
    void convertDepthAndComponents(const void* srcPixelData,
                                   const OfxRectI& renderWindow,
                                   const OfxPointD& renderScale,
                                   const OfxRectI& srcBounds,
                                   OFX::PixelComponentEnum srcPixelComponents,
                                   OFX::BitDepthEnum srcBitDepth,
                                   int srcRowBytes,
                                   float* dstPixelData,
                                   const OfxRectI& dstBounds,
                                   OFX::PixelComponentEnum dstPixelComponents,
                                   int dstRowBytes);

    /**
     * @brief Override and return true if the output may change over time even when the file
     * sequence is a single still image held before and after, e.g. because decode()/decodePlane()
     * evaluate animated parameters at getRenderTime(). GenericReader then sets the output as
     * frame varying (kOfxImageEffectFrameVarying) in getClipPreferences(), so that the host
     * renders every frame instead of reusing a single image.
     * The host only calls getClipPreferences() again when a param listed in
     * kOfxImageEffectPropClipPreferencesSlaveParam changes: a reader whose answer depends on
     * whether some params are animated must add these params to that list (desc.addClipPreferencesSlaveParam()).
     **/
    virtual bool isOutputFrameVarying() const { return false; }

    /**
     * @brief The time of the render action being processed (RenderArguments::time), in host time.
     * Only meaningful when called from decode() or decodePlane() (it is 0 otherwise). Unlike their
     * `time` argument, which is the time in the file sequence (constant for a held still image),
     * this is the time to use to evaluate animated parameters of the reader. Safe with concurrent
     * renders: the value is per thread.
     * Threads started from decode() (e.g. with the OFX multithread suite) do not see it: read it
     * in decode() and pass it to them.
     **/
    static double getRenderTime();

private:
    /**
     * @brief Called when the input image/video file changed.
     *
     * returns true if file exists and parameters successfully guessed, false in case of error.
     *
     * This function is only called once: when the filename is first set.
     *
     * Besides returning colorspace, premult, components, and componentcount, if it returns true
     * this function may also set extra format-specific parameters using OFX::Param::setValue.
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
    virtual bool guessParamsFromFilename(const std::string& newFile,
                                         std::string* colorspace,
                                         OFX::PreMultiplicationEnum* filePremult,
                                         OFX::PixelComponentEnum* components,
                                         int* componentCount)
        = 0;

    /**
     * @brief Override to clear any cache you may have.
     **/
    virtual void clearAnyCache() { }

    /**
     * @brief Overload this function to extract the bound of the pixel data
     * in pixel coordinates and the pixel aspect ratio out of the header
     * of the image targeted by the filename.
     **/
    virtual bool getFrameBounds(const std::string& filename,
                                OfxTime time,
                                int view,
                                OfxRectI* bounds,
                                OfxRectI* format,
                                double* par,
                                std::string* error,
                                int* tile_width,
                                int* tile_height)
        = 0;

    /*
     * In case of plug-ins that can read tiled files, determines whether the image is oriented bottom up or top down.
     * This is so that the rounding to the tile size is applied correctly. Currently this is only useful for the OIIO plug-in.
     */
    virtual bool isTileOrientationTopDown() const { return true; }

    /**
     * @brief Called by render() when the part of the render window that is in the RoD the host got (the output
     * clip's) is out of the frame bounds that getFrameBounds() returned (this happens when the file changed since
     * the host got the RoD; a render window larger than the RoD, e.g. a display window, does not call it). A reader
     * that caches the frame bounds (e.g. ReadEXR, from the file headers) reads them again from the file, updates its
     * cache, and returns true if they changed: render() then calls getFrameBounds() again, instead of cropping the
     * render window to old frame bounds. The default does nothing, and returns false.
     **/
    virtual bool refreshFrameBounds(const std::string& /*filename*/,
                                    OfxTime /*time*/,
                                    int /*view*/) { return false; }

    virtual bool getFrameRate(const std::string& /*filename*/,
                              double* /*fps*/) const { return false; }

    /**
     * @brief Override this function to actually decode the image contained in the file pointed to by filename.
     * If the file is a video-stream then you should decode the frame at the time given in parameters.
     * You must write the decoded image into dstImg. This function should convert the read pixels into the
     * bitdepth of the dstImg. You can inform the host of the bitdepth you support in the describe() function.
     * You can always skip the color-space conversion, but for all linear hosts it would produce either
     * false colors or sub-par performances in the case the end-user has to append a color-space conversion
     * effect her/himself.
     *
     * renderWindow and bounds are in the pixel coordinates of the file (proxy or not), at full resolution:
     * render() scales the image to the render scale itself, so renderScale is always (1,1). renderWindow may be any
     * part of bounds (e.g. a tile, a few columns or a few rows, or the RoD that the host got before the file changed):
     * decode() must only write in renderWindow, and fill the part of renderWindow that is not in the file with black.
     * The simplest way is to open the file, then call clipToFile() with the bounds of the image in it, and write the
     * window that it returns (nothing if it returns false):
     *
     *     OfxRectI window;
     *     if (clipToFile(renderWindow, imageBounds, pixelData, bounds, pixelComponentCount, OFX::eBitDepthFloat, rowBytes, &window)) {
     *         // write the pixels of window, e.g. with convertDepthAndComponents()
     *     }
     *
     * The black part is then transparent, even for an Opaque file (see setDecodedWindow()). The same holds for
     * decodePlane(). Both are called on the thread of the render action.
     *
     * They don't get kOfxImageEffectPropSequentialRenderStatus: the OFX spec says it is "set to indicate whether the
     * effect is currently being rendered in frame order on a single effect instance", which says nothing about
     * playback or interactivity, and a reader's output must not depend on it.
     **/
    virtual void decode(const std::string& filename, OfxTime time, int view, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds, OFX::PixelComponentEnum pixelComponents, int pixelComponentCount, int rowBytes);
    virtual void decodePlane(const std::string& filename, OfxTime time, int view, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds,
                             OFX::PixelComponentEnum pixelComponents, OFX::PixelComponentEnum remappedComponents,
                             int pixelComponentCount, const std::string& rawComponents, int rowBytes);

    /**
     * @brief Override to indicate the time domain. Return false if you know that the
     * file isn't a video-stream, true when you can find-out the frame range.
     **/
    virtual bool getSequenceTimeDomain(const std::string& /*filename*/,
                                       OfxRangeI& /*range*/) { return false; }

    /**
     * @brief Called internally by getTimeDomain(...)
     **/
    bool getSequenceTimeDomainInternal(OfxRangeI& range, bool canSetOriginalFrameRange);

    /**
     * @brief Used internally by the GenericReader.
     **/
    void timeDomainFromSequenceTimeDomain(const OfxRangeI& sequenceTimeDomain, int startingTime, OfxRangeI* timeDomain);

    /**
     * @brief Sets kParamTimeOffset to kParamStartingTime - kParamFirstFrame, if it differs.
     * The time domain and the clip preferences use kParamStartingTime, render, the RoD and isIdentity
     * use kParamTimeOffset (getSequenceTime()), so the two must agree. Call it whenever the plug-in
     * itself sets kParamStartingTime or kParamFirstFrame: the host may not call instanceChanged for
     * these changes (OFX 1.4 hosts don't have to), and the kParamStartingTime handler only syncs the
     * time offset on a user edit.
     **/
    void syncTimeOffset();

    /**
     * @brief Should return true if the file indicated by filename is a video-stream and not
     * a single image file.
     **/
    virtual bool isVideoStream(const std::string& filename) = 0;
    enum GetSequenceTimeRetEnum {
        eGetSequenceTimeWithinSequence = 0,
        eGetSequenceTimeBeforeSequence,
        eGetSequenceTimeAfterSequence,
        eGetSequenceTimeBlack,
        eGetSequenceTimeError,
    };

    /**
     * @brief compute the sequence/file time from time
     */
    GetSequenceTimeRetEnum getSequenceTime(double t, double* sequenceTime) const WARN_UNUSED_RETURN;
    // GetSequenceTimeRetEnum getSequenceTimeHold(double t, double *sequenceTime) const WARN_UNUSED_RETURN;
    GetSequenceTimeRetEnum getSequenceTimeBefore(const OfxRangeI& sequenceTimeDomain, BeforeAfterEnum beforeChoice, double* sequenceTime) const;
    GetSequenceTimeRetEnum getSequenceTimeAfter(const OfxRangeI& sequenceTimeDomain, BeforeAfterEnum afterChoice, double* sequenceTime) const;

    enum GetFilenameRetCodeEnum {
        eGetFileNameFailed = 0,
        eGetFileNameReturnedFullRes,
        eGetFileNameReturnedProxy,
        eGetFileNameBlack,
    };

    /**
     * @brief Returns the filename of the image at the sequence time t.
     * time is the host time, at which animated params (kParamOnMissingFrame) are evaluated.
     **/
    GetFilenameRetCodeEnum getFilenameAtSequenceTime(double time,
                                                     double t,
                                                     bool proxyFiles,
                                                     bool checkForExistingFile,
                                                     std::string* filename) const WARN_UNUSED_RETURN;

    void copyPixelData(const OfxRectI& renderWindow,
                       const OfxPointD& renderScale,
                       const void* srcPixelData,
                       const OfxRectI& srcBounds,
                       OFX::PixelComponentEnum srcPixelComponents,
                       int srcPixelComponentCount,
                       OFX::BitDepthEnum srcPixelDepth,
                       int srcRowBytes,
                       void* dstPixelData,
                       const OfxRectI& dstBounds,
                       OFX::PixelComponentEnum dstPixelComponents,
                       int dstPixelComponentCount,
                       OFX::BitDepthEnum dstBitDepth,
                       int dstRowBytes);

    void scalePixelData(const OfxRectI& originalRenderWindow,
                        const OfxPointD& renderScale,
                        const OfxRectI& renderWindow,
                        unsigned int levels,
                        const void* srcPixelData,
                        OFX::PixelComponentEnum srcPixelComponents,
                        int srcPixelComponentCount,
                        OFX::BitDepthEnum srcPixelDepth,
                        const OfxRectI& srcBounds,
                        int srcRowBytes,
                        void* dstPixelData,
                        OFX::PixelComponentEnum dstPixelComponents,
                        int dstPixelComponentCount,
                        OFX::BitDepthEnum dstPixelDepth,
                        const OfxRectI& dstBounds,
                        int dstRowBytes);

    void premultPixelData(const OfxRectI& renderWindow,
                          const OfxPointD& renderScale,
                          const void* srcPixelData,
                          const OfxRectI& srcBounds,
                          OFX::PixelComponentEnum srcPixelComponents,
                          int srcPixelComponentCount,
                          OFX::BitDepthEnum srcPixelDepth,
                          int srcRowBytes,
                          void* dstPixelData,
                          const OfxRectI& dstBounds,
                          OFX::PixelComponentEnum dstPixelComponents,
                          int dstPixelComponentCount,
                          OFX::BitDepthEnum dstBitDepth,
                          int dstRowBytes);

    void unPremultPixelData(const OfxRectI& renderWindow,
                            const OfxPointD& renderScale,
                            const void* srcPixelData,
                            const OfxRectI& srcBounds,
                            OFX::PixelComponentEnum srcPixelComponents,
                            int srcPixelComponentCount,
                            OFX::BitDepthEnum srcPixelDepth,
                            int srcRowBytes,
                            void* dstPixelData,
                            const OfxRectI& dstBounds,
                            OFX::PixelComponentEnum dstPixelComponents,
                            int dstPixelComponentCount,
                            OFX::BitDepthEnum dstBitDepth,
                            int dstRowBytes);

    // sequenceTime is the sequence (file) time, passed to getFrameBounds()
    OfxPointD detectProxyScale(const std::string& originalFileName, const std::string& proxyFileName, OfxTime sequenceTime);

    void setSequenceFromFile(const std::string& filename);

    void refreshSubLabel(OfxTime time);

    bool checkExtension(const std::string& ext);

protected:
#ifdef OFX_IO_USING_OCIO
    OFX::BooleanParam* _inputSpaceSet;
    auto_ptr<GenericOCIO> _ocio;
#endif

    OFX::Clip* _syncClip; //< Mantated input clip
    OFX::Clip* _outputClip; //< Mandated output clip
    OFX::StringParam* _fileParam; //< The input file

    OFX::IntParam* _firstFrame; //< the first frame in the video file (first frame is 1) or file sequence (>=0)

    OFX::IntParam* _timeOffset; //< the time offset applied to the sequence
    OFX::IntParam* _startingTime; //< the starting frame of the sequence

    OFX::Int2DParam* _originalFrameRange; //< the original frame range computed the first time by getSequenceTimeDomainInternal

private:
    // the following params should not be needed in derived classes.

    OFX::StringParam* _proxyFileParam; //< the proxy input files
    OFX::Double2DParam* _proxyThreshold; //< the proxy  images scale threshold
    OFX::Double2DParam* _originalProxyScale; //< the original proxy image scale
    OFX::BooleanParam* _enableCustomScale; //< is custom proxy scale enabled

    OFX::ChoiceParam* _beforeFirst; //< what to do before the first frame
    OFX::IntParam* _lastFrame; //< the last frame in the sequence (clamped to the time domain)
    OFX::ChoiceParam* _afterLast; //< what to do after the last frame

    OFX::ChoiceParam* _frameMode; //< do we use a time offset or an absolute starting frame

    OFX::ChoiceParam* _outputComponents;
    OFX::ChoiceParam* _filePremult;
    OFX::ChoiceParam* _outputPremult;

    OFX::BooleanParam* _timeDomainUserSet; //< true when the time domain has bee nuser edited

    OFX::BooleanParam* _customFPS;
    OFX::DoubleParam* _fps;

    OFX::StringParam* _sublabel;
    OFX::BooleanParam* _guessedParams; //!< was guessParamsFromFilename already successfully called once on this instance

    const std::vector<std::string>& _extensions;

private:
    /**
     * @brief Fills the renderWindow part of dstPixelData (whose bounds are dstBounds) with transparent black.
     * (decode() and decodePlane() use clipToFile() instead.)
     **/
    void fillWithBlack(const OfxRectI& renderWindow,
                       const OfxPointD& renderScale,
                       void* dstPixelData,
                       const OfxRectI& dstBounds,
                       OFX::PixelComponentEnum dstPixelComponents,
                       int dstPixelComponentCount,
                       OFX::BitDepthEnum dstBitDepth,
                       int dstRowBytes);

    const bool _supportsRGBA;
    const bool _supportsRGB;
    const bool _supportsXY;
    const bool _supportsAlpha;
    const bool _supportsTiles;
    const bool _isMultiPlanar;

    OFX::PixelComponentEnum _outputComponentsTable[5];
};

void GenericReaderDescribe(OFX::ImageEffectDescriptor& desc,
                           const std::vector<std::string>& extensions, // list of supported extensions
                           int evaluation, // plugin quality from 0 (bad) to 100 (perfect) or -1 if not evaluated
                           bool supportsTiles, bool multiPlanar);

OFX::PageParamDescriptor* GenericReaderDescribeInContextBegin(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context, bool isVideoStreamPlugin, bool supportsRGBA, bool supportsRGB, bool supportsXY, bool supportsAlpha, bool supportsTiles, bool addSeparatorAfterLastParameter);
void GenericReaderDescribeInContextEnd(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context, OFX::PageParamDescriptor* page, const char* inputSpaceNameDefault, const char* outputSpaceNameDefault);

#define mDeclareReaderPluginFactory(CLASS, UNLOADFUNCDEF, ISVIDEOSTREAM)                                 \
    class CLASS                                                                                          \
        : public OFX::PluginFactoryHelper<CLASS> {                                                       \
    public:                                                                                              \
        CLASS(const std::string& id, unsigned int verMaj, unsigned int verMin)                           \
            : OFX::PluginFactoryHelper<CLASS>(id, verMaj, verMin)                                        \
        {                                                                                                \
        }                                                                                                \
        virtual void load();                                                                             \
        virtual void unload() UNLOADFUNCDEF;                                                             \
        virtual OFX::ImageEffect* createInstance(OfxImageEffectHandle handle, OFX::ContextEnum context); \
        bool isVideoStreamPlugin() const { return ISVIDEOSTREAM; }                                       \
        virtual void describe(OFX::ImageEffectDescriptor& desc);                                         \
        virtual void describeInContext(OFX::ImageEffectDescriptor& desc, OFX::ContextEnum context);      \
        std::vector<std::string> _extensions;                                                            \
    };

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // ifndef Io_GenericReader_h
