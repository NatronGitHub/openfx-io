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

#ifndef PixelFormat_h
#define PixelFormat_h

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace OFX {
namespace FFmpeg {

    // these are ordered
    enum PixelCodingEnum {
        ePixelCodingNone = 0,
        ePixelCodingGray, // Grayscale
        ePixelCodingPalette, // Palette (color-capable)
        ePixelCodingBayer, // Bayer mosaic
        ePixelCodingYUV410, // 1 Cr & Cb sample per 4x4 Y samples
        ePixelCodingYUV411, // 1 Cr & Cb sample per 4x1 Y samples
        ePixelCodingYUV420, // 1 Cr & Cb sample per 2x2 Y samples
        ePixelCodingYUV422, // 1 Cr & Cb sample per 2x1 Y samples
        ePixelCodingYUV440 = ePixelCodingYUV422, // 1 Cr & Cb sample per 1x2 Y samples
        ePixelCodingYUV444, // 1 Cr & Cb sample per 1x1 Y samples
        ePixelCodingRGB, // RGB
        ePixelCodingXYZ, // XYZ
    };

    bool pixelFormatIsYUV(AVPixelFormat pixelFormat);
    int pixelFormatBitDepth(AVPixelFormat pixelFormat);
    int pixelFormatBPP(AVPixelFormat pixelFormat);
    PixelCodingEnum pixelFormatCoding(AVPixelFormat pixelFormat);
    bool pixelFormatAlpha(AVPixelFormat pixelFormat);

    int pixelFormatBPPFromSpec(PixelCodingEnum coding, int bitdepth, bool alpha);

    // The swscale Y'CbCr coefficient table (the SWS_CS_* argument of sws_getCoefficients(), libswscale/swscale.h)
    // for a Y'CbCr matrix (AVColorSpace, libavutil/pixfmt.h), or -1 if swscale has no table for it (unspecified,
    // RGB, YCgCo, ICtCp...).
    // The SWS_CS_* values are the AVColorSpace values, which index ff_yuv2rgb_coeffs[] in libswscale/yuv2rgb.c
    // (FFmpeg n9.0 libswscale/swscale.h:458-465, libswscale/yuv2rgb.c:47-59; the same in n4.0). That table has
    // BT.2020 NCL and CL (entries 9 and 10, the same coefficients: swscale does not implement the constant
    // luminance system, so a BT.2020 CL stream is converted as NCL), FCC (4, Kr = 0.30, Kb = 0.11, which differ from
    // BT.601's) and SMPTE 240M (7); BT.470BG and SMPTE 170M are both BT.601 (5 and 6). Entry 8 (YCgCo) is empty:
    // sws_getCoefficients() gives the default (BT.601) table for it, which would be wrong.
    inline int swsColorspaceFromAVColorSpace(int colorspace)
    {
        switch (colorspace) {
        case AVCOL_SPC_BT709:
            return SWS_CS_ITU709;
        case AVCOL_SPC_FCC:
            return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M:
            return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M:
            return SWS_CS_SMPTE240M;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return SWS_CS_BT2020;
        default:
            return -1;
        }
    }

}
}

#endif /* defined(PixelFormat_h) */
