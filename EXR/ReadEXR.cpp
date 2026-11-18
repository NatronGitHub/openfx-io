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
 * OFX exr Reader plugin.
 * Reads a an input image using the OpenEXR library.
 */

#include <algorithm>
#ifdef DEBUG
#include <iostream>
#endif

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <fstream>
#include <string>
#include <windows.h>
#ifndef __MINGW32__
#include <ImfStdIO.h>
#endif
#endif

#include "ofxsMacros.h"

GCC_DIAG_OFF(deprecated)
#include <IlmThreadPool.h>
#include <ImathBox.h>
#include <ImfChannelList.h>
#include <ImfCompression.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>
#include <ImfPixelType.h>
GCC_DIAG_ON(deprecated)

#include "fast_mutex.h"
#include "ofxsMultiThread.h"

#include "GenericOCIO.h"
#include "GenericReader.h"

using namespace OFX;
using namespace OFX::IO;
#ifdef OFX_IO_USING_OCIO
namespace OCIO = OCIO_NAMESPACE;
#endif

using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::vector;
using std::wstring;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "ReadEXR"
#define kPluginGrouping "Image/Readers"
#define kPluginDescription "Read images using OpenEXR."
#define kPluginIdentifier "fr.inria.openfx.ReadEXR"
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 1 // Increment this when you have fixed a bug or made it faster.
#define kPluginEvaluation 10

#ifndef OPENEXR_IMF_NAMESPACE
#define OPENEXR_IMF_NAMESPACE Imf
#endif
namespace Imf_ = OPENEXR_IMF_NAMESPACE;

#define kSupportsRGBA true
#define kSupportsRGB false
#define kSupportsXY false
#define kSupportsAlpha false
#define kSupportsTiles false

class ReadEXRPlugin
    : public GenericReaderPlugin {
public:
    ReadEXRPlugin(OfxImageEffectHandle handle, const vector<string>& extensions);

    virtual ~ReadEXRPlugin();

    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;

private:
    virtual bool isVideoStream(const string& /*filename*/) OVERRIDE FINAL { return false; }

    virtual void decode(const string& filename, OfxTime time, int /*view*/, bool isPlayback, const OfxRectI& renderWindow, const OfxPointD& renderScale, float* pixelData, const OfxRectI& bounds, PixelComponentEnum pixelComponents, int pixelComponentCount, int rowBytes) OVERRIDE FINAL;
    virtual bool getFrameBounds(const string& /*filename*/, OfxTime time, int view, OfxRectI* bounds, OfxRectI* format, double* par, string* error, int* tile_width, int* tile_height) OVERRIDE FINAL;

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
    virtual bool guessParamsFromFilename(const string& newFile, string* colorspace, PreMultiplicationEnum* filePremult, PixelComponentEnum* components, int* componentCount) OVERRIDE FINAL;
};

namespace Exr {
////@enum A small enum to map exr components to Openfx. We can't support more as the openfx standard is limited to R,G,B and A.
enum Channel {
    Channel_red = 0,
    Channel_green,
    Channel_blue,
    Channel_alpha,
    Channel_none
};

static Channel
fromExrChannel(const string& from)
{
    if ((from == "R") || (from == "r") || (from == "Red") || (from == "RED") || (from == "red") || (from == "y") || (from == "Y")) {
        return Channel_red;
    }
    if ((from == "G") || (from == "g") || (from == "Green") || (from == "GREEN") || (from == "green") || (from == "ry") || (from == "RY")) {
        return Channel_green;
    }
    if ((from == "B") || (from == "b") || (from == "Blue") || (from == "BLUE") || (from == "blue") || (from == "by") || (from == "BY")) {
        return Channel_blue;
    }
    if ((from == "A") || (from == "a") || (from == "Alpha") || (from == "ALPHA") || (from == "alpha")) {
        return Channel_alpha;
    }
    throw std::invalid_argument("OpenFX doesn't support the channel " + from);

    return Channel_alpha;
}

class ChannelExtractor {
public:
    ChannelExtractor(const string& name,
                     const vector<string>& views)
        : _mappedChannel(Channel_none)
        , _valid(false)
    {
        _valid = extractExrChannelName(name, views);
    }

    ~ChannelExtractor() { }

    Channel _mappedChannel;
    bool _valid;
    string _chan;
    string _layer;
    string _view;

    string exrName() const
    {
        if (!_layer.empty()) {
            return _layer + '.' + _chan;
        }

        return _chan;
    }

    bool isValid() const { return _valid; }

private:
    static bool IsView(const string& name,
                       const vector<string>& views)
    {
        for (size_t i = 0; i < views.size(); ++i) {
            if (views[i] == name) {
                return true;
            }
        }

        return false;
    }

    vector<string> split(const string& str,
                         char splitChar)
    {
        vector<string> out;
        size_t i = str.find(splitChar);
        size_t offset = 0;
        while (i != str.npos) {
            out.push_back(str.substr(offset, i - offset));
            offset = i + 1;
            i = str.find(splitChar, offset);
        }
        out.push_back(str.substr(offset));

        return out;
    }

    string removePrependingDigits(const string& str)
    {
        string ret;
        unsigned int i = 0;

        while (isdigit(str[i]) && (i < str.size())) {
            ++i;
        }
        for (; i < str.size(); ++i) {
            ret.push_back(str[i]);
        }

        return ret;
    }

    string removeNonAlphaCharacters(const string& str)
    {
        string ret;

        for (unsigned int i = 0; i < str.size(); i++) {
            if (!isalnum(str[i])) {
                ret.push_back('_');
            } else {
                ret.push_back(str[i]);
            }
        }

        return ret;
    }

    bool extractExrChannelName(const string& channelname,
                               const vector<string>& views)
    {
        _chan.clear();
        _layer.clear();
        _view.clear();

        vector<string> splits = split(channelname, '.');
        vector<string> newSplits;
        // remove prepending digits
        for (size_t i = 0; i < splits.size(); ++i) {
            string s = removePrependingDigits(splits[i]);
            if (!s.empty()) {
                newSplits.push_back(removeNonAlphaCharacters(s));
            }
        }

        if (newSplits.size() > 1) {
            for (size_t i = 0; i < (newSplits.size() - 1); ++i) {
                vector<string>::const_iterator foundView = std::find(views.begin(), views.end(), newSplits[i]);
                if (foundView != views.end()) {
                    _view = *foundView;
                } else {
                    if (!_layer.empty()) {
                        _layer += '_';
                    }
                    _layer += newSplits[i];
                }
            }
            _chan = newSplits.back();
        } else {
            _chan = newSplits[0];
        }

        try {
            _mappedChannel = fromExrChannel(_chan);
        } catch (const std::exception& e) {
#ifdef DEBUG
            std::cout << "Error while reading EXR file"
                      << ": " << e.what() << std::endl;
#endif

            return false;
        }

        return true;
    }
};

typedef map<Channel, string> ChannelsMap;

// The channels of the file that map to R, G, B and A: the first one of each, in the order of the channel list
// (by their bare names: the layers are ignored).
static void
mapChannels(const Imf_::Header& header,
            const vector<string>& views,
            ChannelsMap* channels)
{
    const Imf_::ChannelList& imfchannels = header.channels();

    for (Imf_::ChannelList::ConstIterator chan = imfchannels.begin(); chan != imfchannels.end(); ++chan) {
        string chanName(chan.name());

        /// empty channel, discard it
        if (chanName.empty()) {
            continue;
        }

        /// convert the channel to ours
        ChannelExtractor exrExctractor(chan.name(), views);

        /// if we successfully extracted the channels
        if (exrExctractor.isValid()) {
            /// register the extracted channel
            channels->insert(make_pair(exrExctractor._mappedChannel, exrExctractor._chan));
        } else {
#ifdef DEBUG
            std::cout << "Cannot decode channel " << chan.name() << std::endl;
#endif
        }
    }
}

// The display window (the format) and the data window (the RoD) of the file, in OFX pixel coordinates (x2 and y2
// exclusive, y up): the display window is moved to (0,0), and y is flipped. When the windows differ, the RoD is the
// data window grown by 1 pixel, which is black (as in ReadOIIO, with Edge Pixels: Auto).
static void
getWindows(const Imf_::Header& header,
           OfxRectI* displayWindow,
           OfxRectI* dataWindow)
{
    const Imath::Box2i& datawin = header.dataWindow();
    const Imath::Box2i& dispwin = header.displayWindow();
    // Shift both to get dispwindow over to 0,0.
    const int dataOffset = -dispwin.min.x;

    displayWindow->x1 = 0;
    displayWindow->y1 = 0;
    displayWindow->x2 = dispwin.max.x + dataOffset + 1;
    displayWindow->y2 = dispwin.max.y - dispwin.min.y + 1;

    int left = datawin.min.x + dataOffset;
    int bottom = dispwin.max.y - datawin.max.y;
    int right = datawin.max.x + dataOffset;
    int top = dispwin.max.y - datawin.min.y;
    if ((datawin.min.x != dispwin.min.x) || (datawin.max.x != dispwin.max.x) || (datawin.min.y != dispwin.min.y) || (datawin.max.y != dispwin.max.y)) {
        --left;
        --bottom;
        ++right;
        ++top;
    }
    dataWindow->x1 = left;
    dataWindow->x2 = right + 1;
    dataWindow->y1 = bottom;
    dataWindow->y2 = top + 1;
}

#if (defined(_WIN32) || defined(__WIN32__) || defined(WIN32)) && !defined(__MINGW32__)
static wstring
s2ws(const string& s)
{
    int len;
    int slength = (int)s.length() + 1;

    len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, 0, 0);
    wchar_t* buf = new wchar_t[len];
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), slength, buf, len);
    wstring r(buf);
    delete[] buf;

    return r;
}
#endif

// An Imf::InputFile, open for a single use: reading the header, or one render. An Imf::InputFile can not be shared
// by concurrent renders (the plug-in is kOfxImageEffectRenderFullySafe): setFrameBuffer() and readPixels() are two
// calls, and OpenEXR does not support concurrent calls on one InputFile. Opening the file only reads its header and
// its offset table, which is cheap compared to decoding the pixels.
class InputFile {
public:
    explicit InputFile(const string& filename);

    ~InputFile();

    Imf_::InputFile& get() { return *_file; }

private:
    InputFile(const InputFile&); // noncopyable
    InputFile& operator=(const InputFile&);

#if (defined(_WIN32) || defined(__WIN32__) || defined(WIN32)) && !defined(__MINGW32__)
    std::ifstream* _inputStr;
    Imf_::StdIFStream* _inputStdStream;
#endif
    Imf_::InputFile* _file;
};

InputFile::InputFile(const string& filename)
    :
#if (defined(_WIN32) || defined(__WIN32__) || defined(WIN32)) && !defined(__MINGW32__)
    _inputStr(NULL)
    , _inputStdStream(NULL)
    ,
#endif
    _file(NULL)
{
#if (defined(_WIN32) || defined(__WIN32__) || defined(WIN32)) && !defined(__MINGW32__)
    try {
        _inputStr = new std::ifstream(s2ws(filename), std::ios_base::binary);
        _inputStdStream = new Imf_::StdIFStream(*_inputStr, filename.c_str());
        _file = new Imf_::InputFile(*_inputStdStream);
    } catch (...) {
        delete _inputStdStream;
        delete _inputStr;
        throw;
    }
#else
    _file = new Imf_::InputFile(filename.c_str());
#endif
}

InputFile::~InputFile()
{
    delete _file;
#if (defined(_WIN32) || defined(__WIN32__) || defined(WIN32)) && !defined(__MINGW32__)
    delete _inputStdStream;
    delete _inputStr;
#endif
}

// What getFrameBounds() and guessParamsFromFilename() need to know about a file, read from its header. The file is
// not kept open: decode() opens it again (see InputFile).
struct File {
    explicit File(const string& filename);

    ChannelsMap channel_map;
    vector<string> views;
    OfxRectI displayWindow;
    OfxRectI dataWindow;
    float pixelAspectRatio;
};

File::File(const string& filename)
    : channel_map()
    , views()
    , displayWindow()
    , dataWindow()
    , pixelAspectRatio(1.)
{
    InputFile file(filename);
    const Imf_::Header& header = file.get().header();

    mapChannels(header, views, &channel_map);
    getWindows(header, &displayWindow, &dataWindow);
    pixelAspectRatio = header.pixelAspectRatio();
}

// Keeps track of all Exr::File mapped against file name.
class FileManager {
    typedef map<string, File*> FilesMap;

    FilesMap _files;
    // locks _files (concurrent renders, getRegionOfDefinition...): a fast_mutex, which doesn't need the host's
    // multi-thread suite, so that it can be a static object
    tthread::fast_mutex _lock;

public:
    // singleton
    static FileManager s_readerManager;

    FileManager();

    ~FileManager();

    // get a specific reader: throws if the file can not be read
    const File* get(const string& filename);
};

FileManager FileManager::s_readerManager;

// constructor
FileManager::FileManager()
    : _files()
    , _lock()
{
}

FileManager::~FileManager()
{
    for (FilesMap::iterator it = _files.begin(); it != _files.end(); ++it) {
        delete it->second;
    }
}

// get a specific reader
const File*
FileManager::get(const string& filename)
{
    OFX::MultiThread::AutoMutexT<tthread::fast_mutex> g(_lock);
    FilesMap::iterator it = _files.find(filename);
    if (it == _files.end()) {
        File* file = new File(filename); // (may throw)
        pair<FilesMap::iterator, bool> ret = _files.insert(make_pair(string(filename), file));
        assert(ret.second);

        return ret.first->second;
    } else {
        return it->second;
    }
}
} // namespace Exr

ReadEXRPlugin::ReadEXRPlugin(OfxImageEffectHandle handle,
                             const vector<string>& extensions)
    : GenericReaderPlugin(handle, extensions, kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, kSupportsTiles, false)
{
}

ReadEXRPlugin::~ReadEXRPlugin()
{
}

void
ReadEXRPlugin::changedParam(const InstanceChangedArgs& args,
                            const string& paramName)
{
    GenericReaderPlugin::changedParam(args, paramName);
}

void
ReadEXRPlugin::decode(const string& filename,
                      OfxTime /*time*/,
                      int /*view*/,
                      bool /*isPlayback*/,
                      const OfxRectI& renderWindow,
                      const OfxPointD& renderScale,
                      float* pixelData,
                      const OfxRectI& bounds,
                      PixelComponentEnum pixelComponents,
                      int pixelComponentCount,
                      int rowBytes)
{
    assert(renderScale.x == 1. && renderScale.y == 1.);
    unused(renderScale);
    /// we only support RGBA output clip
    if ((pixelComponents != ePixelComponentRGBA) || (pixelComponentCount != 4)) {
        throwSuiteStatusException(kOfxStatErrFormat);

        return;
    }

    // pixelData holds the image of the given bounds: write the render window (the whole RoD: tiles are not
    // supported), clipped to these bounds.
    const int x1 = (std::max)(renderWindow.x1, bounds.x1);
    const int x2 = (std::min)(renderWindow.x2, bounds.x2);
    const int y1 = (std::max)(renderWindow.y1, bounds.y1);
    const int y2 = (std::min)(renderWindow.y2, bounds.y2);
    if ((x1 >= x2) || (y1 >= y2)) {
        return;
    }
    const ptrdiff_t pixelBytes = 4 * sizeof(float);

    bool failed = false;
    string error;
    try {
        // this render's own InputFile (see Exr::InputFile), and its header (not the one getFrameBounds() read, in
        // case the file changed since: the image is written within the render window and the bounds anyway)
        Exr::InputFile in(filename);
        Imf_::InputFile& file = in.get();
        const Imf_::Header& header = file.header();
#ifndef NDEBUG
        {
            OfxRectI displayWindow, dataWindow;
            Exr::getWindows(header, &displayWindow, &dataWindow);
            assert(kSupportsTiles || (renderWindow.x1 == dataWindow.x1 && renderWindow.x2 == dataWindow.x2 && renderWindow.y1 == dataWindow.y1 && renderWindow.y2 == dataWindow.y2));
        }
#endif
        Exr::ChannelsMap channels;
        Exr::mapChannels(header, vector<string>(), &channels);

        // OpenEXR windows are inclusive, with y down. EXR pixel (x, y) is OFX pixel (x + dataOffset, dispwin.max.y - y)
        // (see Exr::getWindows()): the display window is moved to (0,0), and y is flipped.
        const Imath::Box2i& dispwin = header.displayWindow();
        const Imath::Box2i& datawin = header.dataWindow();
        const int dataOffset = -dispwin.min.x;
        // the data window, in OFX pixel coordinates (x2 and y2 exclusive)
        const int dataX1 = datawin.min.x + dataOffset;
        const int dataX2 = datawin.max.x + dataOffset + 1;
        const int dataY1 = dispwin.max.y - datawin.max.y;
        const int dataY2 = dispwin.max.y - datawin.min.y + 1;
        // the part of the image that comes from the file; the rest (the 1-pixel border that getFrameBounds adds
        // when the data and display windows differ, or anything else out of the data window) is black
        const int readX1 = (std::max)(x1, dataX1);
        const int readX2 = (std::min)(x2, dataX2);
        const int readY1 = (std::max)(y1, dataY1);
        const int readY2 = (std::min)(y2, dataY2);
        const bool hasData = (readX1 < readX2) && (readY1 < readY2);

        // Does the file write R, G, B and A at every pixel (no channel missing, none subsampled)? UINT channels
        // are normalized, as in ReadOIIO.
        bool complete = (channels.size() == 4);
        vector<int> uintChannels;
        for (Exr::ChannelsMap::const_iterator it = channels.begin(); it != channels.end(); ++it) {
            const Imf_::Channel* chan = header.channels().findChannel(it->second);
            if (!chan || (chan->xSampling != 1) || (chan->ySampling != 1)) {
                complete = false;
            }
            if (chan && (chan->type == Imf_::UINT)) {
                uintChannels.push_back((int)it->first);
            }
        }

        // black where the file writes nothing: out of the data window, and in it if the file doesn't write every
        // channel of every pixel
        for (int y = y1; y < y2; ++y) {
            float* dst = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * rowBytes + (ptrdiff_t)(x1 - bounds.x1) * pixelBytes);
            if (!hasData || (y < readY1) || (y >= readY2) || !complete) {
                std::fill(dst, dst + (size_t)(x2 - x1) * 4, 0.f);
            } else {
                std::fill(dst, dst + (size_t)(readX1 - x1) * 4, 0.f);
                std::fill(dst + (size_t)(readX2 - x1) * 4, dst + (size_t)(x2 - x1) * 4, 0.f);
            }
        }

        if (hasData) {
            // The EXR lines exrY1..exrY2 (inclusive) are the OFX lines readY2 - 1..readY1. They are read with a
            // single readPixels() call (so that OpenEXR decompresses each chunk of 1 to 32 lines once, and may use
            // its threads) into a frame buffer where EXR pixel (x, y) is at base + x * pixelBytes + y * yStride:
            // straight into pixelData (yStride < 0: y is flipped) if each line of the data window fits in it (it
            // always does, unless the bounds are smaller than the RoD), else into a buffer.
            const int exrY1 = dispwin.max.y - (readY2 - 1);
            const int exrY2 = dispwin.max.y - readY1;
            const bool direct = (readX1 == dataX1) && (readX2 == dataX2);
            const int dataWidth = datawin.max.x - datawin.min.x + 1;
            vector<float> buffer;
            char* base;
            ptrdiff_t yStride;
            if (direct) {
                yStride = -(ptrdiff_t)rowBytes;
                base = (char*)pixelData + (ptrdiff_t)(dispwin.max.y - bounds.y1) * rowBytes + (ptrdiff_t)(dataOffset - bounds.x1) * pixelBytes;
            } else {
                buffer.assign((size_t)dataWidth * (exrY2 - exrY1 + 1) * 4, 0.f);
                yStride = (ptrdiff_t)dataWidth * pixelBytes;
                base = (char*)&buffer[0] - (ptrdiff_t)datawin.min.x * pixelBytes - (ptrdiff_t)exrY1 * yStride;
            }
            Imf_::FrameBuffer fbuf;
            for (Exr::ChannelsMap::const_iterator it = channels.begin(); it != channels.end(); ++it) {
                const Imf_::Channel* chan = header.channels().findChannel(it->second);
                const int xs = chan ? chan->xSampling : 1;
                const int ys = chan ? chan->ySampling : 1;
                // The sample of a Slice at (x, y) is at base + (x / xs) * xStride + (y / ys) * yStride. A
                // subsampled channel (RY, BY: not converted) is written as before #123: its samples side by side
                // from the left of the data window, on the lines that have samples (datawin.min.x, and the x and y
                // of the samples, are multiples of the sampling rates).
                char* chanBase = base + (ptrdiff_t)it->first * sizeof(float) + (ptrdiff_t)(datawin.min.x - datawin.min.x / xs) * pixelBytes;
                fbuf.insert(it->second.c_str(), Imf_::Slice(Imf_::FLOAT, chanBase, pixelBytes, ys * yStride, xs, ys));
            }
            file.setFrameBuffer(fbuf);
            file.readPixels(exrY1, exrY2);

            for (int y = readY1; y < readY2; ++y) {
                float* dst = (float*)((char*)pixelData + (ptrdiff_t)(y - bounds.y1) * rowBytes + (ptrdiff_t)(readX1 - bounds.x1) * pixelBytes);
                if (!direct) {
                    const float* src = &buffer[((size_t)(dispwin.max.y - y - exrY1) * dataWidth + (readX1 - dataX1)) * 4];
                    std::copy(src, src + (size_t)(readX2 - readX1) * 4, dst);
                }
                // UINT: 0..2^32-1 -> 0..1
                for (size_t k = 0; k < uintChannels.size(); ++k) {
                    for (float* p = dst + uintChannels[k]; p < dst + (size_t)(readX2 - readX1) * 4; p += 4) {
                        *p = (float)(*p * (1. / 4294967295.));
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        failed = true;
        error = e.what();
    }
    if (failed) {
        // the image is incomplete: the render fails
        setPersistentMessage(Message::eMessageError, "", string("OpenEXR error") + ": " + error);
        throwSuiteStatusException(kOfxStatFailed);
    }
} // ReadEXRPlugin::decode

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
ReadEXRPlugin::guessParamsFromFilename(const string& newFile,
                                       string* colorspace,
                                       PreMultiplicationEnum* filePremult,
                                       PixelComponentEnum* components,
                                       int* componentCount)
{
    assert(colorspace && filePremult && components && componentCount);

    const Exr::File* file = NULL;
    try {
        file = newFile.empty() ? NULL : Exr::FileManager::s_readerManager.get(newFile);
    } catch (const std::exception& e) {
        setPersistentMessage(Message::eMessageError, "", string("OpenEXR error") + ": " + e.what());

        return false;
    }
    if (!file) {
        return false;
    }

#ifdef OFX_IO_USING_OCIO
    // Unless otherwise specified, exr files are assumed to be linear.
    *colorspace = OCIO::ROLE_SCENE_LINEAR;
#endif

    bool hasRed = file->channel_map.find(Exr::Channel_red) != file->channel_map.end();
    bool hasGreen = file->channel_map.find(Exr::Channel_green) != file->channel_map.end();
    bool hasBlue = file->channel_map.find(Exr::Channel_blue) != file->channel_map.end();
    bool hasAlpha = file->channel_map.find(Exr::Channel_alpha) != file->channel_map.end();

    if (hasAlpha) {
        // if any color channel is present, let it be RGBA
        if (hasRed || hasGreen || hasBlue) {
            *components = ePixelComponentRGBA;
            *componentCount = 4;
        } else {
            *components = ePixelComponentAlpha;
            *componentCount = 1;
        }
    } else {
        // if any color channel is present, let it be RGB
        if (hasRed || hasGreen || hasBlue) {
            *components = ePixelComponentRGB;
            *componentCount = 3;
        } else {
            *components = ePixelComponentNone;
            *componentCount = 0;
        }
    }
    /*
       OpenEXR is always stored premultiplied.

       See page five of the Technical Introduction PDF at http://www.openexr.com/documentation.html.

       "alpha/opacity: 0.0 means the pixel is transparent; 1.0 means the pixel is
       opaque. By convention, all color channels are premultiplied by alpha, so that
       "foreground + (1-alpha) × background" performs a correct "over" operation."
     */
    if ((*components != ePixelComponentRGBA) && (*components != ePixelComponentAlpha)) {
        *filePremult = eImageOpaque;
    } else {
        *filePremult = eImagePreMultiplied;
    }

    return true; // success
}

bool
ReadEXRPlugin::getFrameBounds(const string& filename,
                              OfxTime /*time*/,
                              int /*view*/,
                              OfxRectI* bounds,
                              OfxRectI* format,
                              double* par,
                              string* error,
                              int* tile_width,
                              int* tile_height)
{
    assert(bounds && par);
    const Exr::File* file = NULL;
    try {
        file = Exr::FileManager::s_readerManager.get(filename);
    } catch (const std::exception& e) {
        if (error) {
            *error = string("OpenEXR error") + ": " + e.what();
        }

        return false;
    }
    bounds->x1 = file->dataWindow.x1;
    bounds->x2 = file->dataWindow.x2;
    bounds->y1 = file->dataWindow.y1;
    bounds->y2 = file->dataWindow.y2;
    format->x1 = file->displayWindow.x1;
    format->y1 = file->displayWindow.y1;
    format->x2 = file->displayWindow.x2;
    format->y2 = file->displayWindow.y2;
    *par = file->pixelAspectRatio;
    *tile_width = *tile_height = 0;

    return true;
}

mDeclareReaderPluginFactory(ReadEXRPluginFactory, ;, false);
void
ReadEXRPluginFactory::unload()
{
    // Kill all threads
    IlmThread::ThreadPool::globalThreadPool().setNumThreads(0);
}

void
ReadEXRPluginFactory::load()
{
    _extensions.clear();
    _extensions.push_back("exr");
}

/** @brief The basic describe function, passed a plugin descriptor */
void
ReadEXRPluginFactory::describe(ImageEffectDescriptor& desc)
{
    GenericReaderDescribe(desc, _extensions, kPluginEvaluation, kSupportsTiles, false);
    // basic labels
    desc.setLabel("ReadEXROFX");
    desc.setPluginDescription("Read EXR images using OpenEXR.");
    // (GenericReaderDescribe() declares eRenderFullySafe: see Exr::FileManager and Exr::InputFile)

    desc.setIsDeprecated(true); // This plugin was superseeded by ReadOIIO
}

/** @brief The describe in context function, passed a plugin descriptor and a context */
void
ReadEXRPluginFactory::describeInContext(ImageEffectDescriptor& desc,
                                        ContextEnum context)
{
    // make some pages and to things in
    PageParamDescriptor* page = GenericReaderDescribeInContextBegin(desc, context, isVideoStreamPlugin(),
                                                                    kSupportsRGBA, kSupportsRGB, kSupportsXY, kSupportsAlpha, kSupportsTiles, true);

    GenericReaderDescribeInContextEnd(desc, context, page, "scene_linear", "scene_linear");
}

/** @brief The create instance function, the plugin must return an object derived from the \ref ImageEffect class */
ImageEffect*
ReadEXRPluginFactory::createInstance(OfxImageEffectHandle handle,
                                     ContextEnum /*context*/)
{
    ReadEXRPlugin* ret = new ReadEXRPlugin(handle, _extensions);

    ret->restoreStateFromParams();

    return ret;
}

static ReadEXRPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

    OFXS_NAMESPACE_ANONYMOUS_EXIT
