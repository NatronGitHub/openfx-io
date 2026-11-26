/*
   OFX I/O utility functions.
   Adds OpenColorIO functionality to any plugin.

   Copyright (C) 2013-2018 INRIA
   Author: Frederic Devernay <frederic.devernay@inria.fr>

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

#ifndef IO_Utility_h
#define IO_Utility_h

#include <algorithm>
#include <cassert>
#include <climits> // INT_MIN, INT_MAX
#include <cmath>
#include <cstdint>
#include <functional>
#include <locale>
#include <string>

#include "ofxsImageEffect.h"
#include "ofxsMacros.h" // OFXS_LN2

#define NAMESPACE_OFX_ENTER namespace OFX {
#define NAMESPACE_OFX_EXIT }

#define NAMESPACE_OFX_IO_ENTER namespace IO {
#define NAMESPACE_OFX_IO_EXIT }

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

inline std::string
basename(std::string const& pathname)
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(WIN64)
    std::size_t found = pathname.find_last_of("/\\");
#else
    std::size_t found = pathname.find_last_of("/");
#endif

    return pathname.substr(found + 1);
}

inline std::string
dirname(std::string const& pathname)
{
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32) || defined(WIN64)
    std::size_t found = pathname.find_last_of("/\\");
#else
    std::size_t found = pathname.find_last_of("/");
#endif

    return pathname.substr(0, found);
}

inline std::string
extension(const std::string& filename)
{
    std::string::const_reverse_iterator pivot = std::find(filename.rbegin(), filename.rend(), '.');
    if (pivot == filename.rend()) {
        return "";
    }
    std::string ext;
    std::locale loc;
    for (std::string::const_iterator it = pivot.base(); it != filename.end(); ++it) {
        ext.append(1, std::tolower(*it, loc));
    }

    return ext;
}

/// numvals should be 256 for byte, 65536 for 16-bits, etc.
template <int numvals>
float
intToFloat(int value)
{
    return value / (float)(numvals - 1);
}

template <int numvals>
int
floatToInt(float value)
{
    // A NaN pixel value (image data may contain NaNs) fails every comparison and gives 0, without a NaN test (as
    // clampToInt() below: with -ffast-math, the compiler may assume that there is no NaN). Only a value in (0, 1)
    // reaches the conversion to int.
    if ((value > 0.f) && (value < 1.f)) {
        return (int)(value * (numvals - 1) + 0.5);
    }

    return (value >= 1.f) ? (numvals - 1) : 0; // value <= 0, or NaN
}

// v converted to int (rounded toward zero, as (int)v) after clamping it to [lo, hi], for a value that comes from the
// host or from a file: out of the range of int, the conversion is undefined behaviour (#184). Values in [lo, hi] are
// unchanged. A NaN fails both comparisons and gives lo; this is not a NaN test (as Coords::pixelToIntClamped(): with
// -ffinite-math-only, the compiler may assume that there is no NaN).
inline int
clampToInt(double v,
           int lo = INT_MIN,
           int hi = INT_MAX)
{
    assert(lo <= hi);
    if (v >= (double)hi) {
        return hi;
    }
    if (v > (double)lo) {
        return (int)v; // lo < v < hi
    }

    return lo; // v <= lo, or NaN
}

/**
 * @brief Upscales the bounds assuming this rectangle is the Nth level of mipmap
 **/
inline OfxRectI
upscalePowerOfTwo(const OfxRectI& r,
                  unsigned int thisLevel)
{
    if (thisLevel == 0) {
        return r;
    }
    // multiply rather than shift: left-shifting a negative value (e.g. an EXR data window with a negative
    // origin) is undefined behaviour before C++20. In 64 bits, clamped to the int range, i.e. to kOfxFlagInfiniteMin
    // and kOfxFlagInfiniteMax (ofxCore.h): the product overflowed an int for bounds near its limits (#371), and an
    // infinite bound stays infinite. Any int times 2^32 fits in 64 bits, and a level beyond 32 gives the same result.
    const long long scale = 1LL << (std::min)(thisLevel, 32u);
    const auto up = [scale](int v) {
        return (int)(std::max)((long long)kOfxFlagInfiniteMin, (std::min)((long long)kOfxFlagInfiniteMax, v * scale));
    };
    OfxRectI ret;
    ret.x1 = up(r.x1);
    ret.x2 = up(r.x2);
    ret.y1 = up(r.y1);
    ret.y2 = up(r.y2);

    return ret;
}

/**
 * @brief Upscales the bounds assuming this rectangle is the Nth level of mipmap
 **/
inline OfxRectD
upscalePowerOfTwo(const OfxRectD& r,
                  double thisLevel)
{
    if (thisLevel == 0) {
        return r;
    }
    OfxRectD ret;
    // multiply by 2^thisLevel (std::pow(r.x1, thisLevel) was NaN for a negative coordinate and a non-integer level)
    const double scale = std::pow(2., thisLevel);
    ret.x1 = r.x1 * scale;
    ret.x2 = r.x2 * scale;
    ret.y1 = r.y1 * scale;
    ret.y2 = r.y2 * scale;

    return ret;
}

inline bool
isRectNull(const OfxRectI& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

inline bool
intersect(const OfxRectI& r1,
          const OfxRectI& r2,
          OfxRectI* intersection)
{
    if (isRectNull(r1) || isRectNull(r2)) {
        return false;
    }

    if ((r1.x1 > r2.x2) || (r2.x1 > r1.x2) || (r1.y1 > r2.y2) || (r2.y1 > r1.y2)) {
        return false;
    }

    intersection->x1 = (std::max)(r1.x1, r2.x1);
    intersection->x2 = (std::min)(r1.x2, r2.x2);
    intersection->y1 = (std::max)(r1.y1, r2.y1);
    intersection->y2 = (std::min)(r1.y2, r2.y2);

    return true;
}

inline unsigned int
getLevelFromScale(double s)
{
    // a scale of 1 or more is level 0; a scale of 0 or less is invalid (std::log() would give -inf or NaN, which can
    // not be converted to int): use the full resolution
    if (!((s > 0.) && (s < 1.))) {
        return 0; // also a NaN scale, which fails both comparisons (#184)
    }
    int retval = -(int)std::floor(std::log(s) / OFXS_LN2 + 0.5);
    assert(retval >= 0);

    return retval;
}

/**
 * @brief Helper class to make fast buffers that are ensured to be deallocated in a RAII style
 **/
class RamBuffer {
    unsigned char* data;

public:
    RamBuffer(std::size_t nBytes)
        : data(0)
    {
        data = (unsigned char*)malloc(nBytes);
    }

    unsigned char* getData() const
    {
        return data;
    }

    ~RamBuffer()
    {
        if (data) {
            free(data);
        }
    }
};

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT

#endif // ifndef IO_Utility_h
