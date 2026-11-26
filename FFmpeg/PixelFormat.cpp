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

#include "PixelFormat.h"

#include <algorithm>

extern "C" {
#include <libavutil/pixdesc.h>
}

namespace OFX {
namespace FFmpeg {

    bool
    pixelFormatIsYUV(AVPixelFormat pix_fmt)
    {
        // from swscale_internal.h
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix_fmt);

        return desc && !(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components >= 2;
    }

    bool
    pixelFormatAlpha(AVPixelFormat pix_fmt)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pix_fmt);

        return desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
    }

    int
    pixelFormatBPP(const AVPixelFormat pixelFormat)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixelFormat);

        return desc ? av_get_bits_per_pixel(desc) : 0;
    } // FFmpeg::pixelFormatBPP

    // The bit depth of a pixel format, from its descriptor (#403): the smallest depth of its components (e.g. 5 for
    // rgb565, whose green has 6 bits), or the bits per pixel of a Bayer format (whose components describe the 2x2
    // pattern: BAYER8_DESC_COMMON, FFmpeg n9.0 libavutil/pixdesc.c:2149-2157). 0 for hardware formats.
    int
    pixelFormatBitDepth(const AVPixelFormat pixelFormat)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixelFormat);
        if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) || (desc->nb_components == 0)) {
            return 0;
        }
        if (desc->flags & AV_PIX_FMT_FLAG_BAYER) {
            return av_get_bits_per_pixel(desc);
        }
        int depth = desc->comp[0].depth;
        for (int i = 1; i < desc->nb_components; ++i) {
            depth = (std::min)(depth, desc->comp[i].depth);
        }

        return depth;
    } // pixelFormatBitDepth

    // The bit depth of the samples of a decoded pixel format (issue #213): the largest depth of its components
    // (AVComponentDescriptor::depth, libavutil/pixdesc.h:55-57 at n9.0.2). Not AVCodecContext::bits_per_raw_sample, which
    // the vp9, hevc, av1 and libdav1d decoders leave at 0 (h264 sets it, libavcodec/h264_slice.c:1009), nor
    // av_get_bits_per_pixel() / nb_components, the mean bits per pixel, which chroma subsampling lowers
    // (libavutil/pixdesc.c:3408-3419): yuv420p10 gave 15 / 3 = 5, and was decoded to 8 bits.
    // A Bayer format has one sample per pixel, whose depth is split among the components (BAYER16_DESC_COMMON gives them
    // 4, 8 and 4 bits, libavutil/pixdesc.c:2159-2167): its bit depth is the bits per pixel (16 for bayer_rggb16, which
    // the ffv1 and cfhd decoders output, libavcodec/ffv1_parse.c:443 and cfhd.c:567). 0 for an unknown pixel format.
    int
    pixelFormatMaxComponentDepth(const AVPixelFormat pixelFormat)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixelFormat);
        if (!desc) {
            return 0;
        }
        if (desc->flags & AV_PIX_FMT_FLAG_BAYER) {
            return av_get_bits_per_pixel(desc);
        }
        int depth = 0;
        for (int c = 0; c < desc->nb_components; ++c) {
            depth = (std::max)(depth, desc->comp[c].depth);
        }

        return depth;
    } // pixelFormatMaxComponentDepth

    // The coding of a pixel format, from its descriptor (FFmpeg n9.0 libavutil/pixdesc.h:69-110 and :116-163, the same
    // in n8.0), so that every pixel format that FFmpeg knows is classified, without a list of names (#403):
    // the palette, Bayer, XYZ and RGB flags, then gray (one color component, maybe with alpha), then the chroma
    // subsampling of Y'CbCr (log2_chroma_w and log2_chroma_h, as get_pix_fmt_score() uses them, n9.0
    // libavutil/pixdesc.c:3637-3645). Hardware formats (AV_PIX_FMT_FLAG_HWACCEL) have no coding.
    PixelCodingEnum
    pixelFormatCoding(const AVPixelFormat pixelFormat)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixelFormat);
        if (!desc || (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) || (desc->nb_components == 0)) {
            return ePixelCodingNone;
        }
        if (desc->flags & AV_PIX_FMT_FLAG_PAL) {
            return ePixelCodingPalette;
        }
        if (desc->flags & AV_PIX_FMT_FLAG_BAYER) { // (Bayer formats also have AV_PIX_FMT_FLAG_RGB)
            return ePixelCodingBayer;
        }
        if (desc->flags & AV_PIX_FMT_FLAG_XYZ) {
            return ePixelCodingXYZ;
        }
        if (desc->flags & AV_PIX_FMT_FLAG_RGB) {
            return ePixelCodingRGB;
        }
        const int colorComponents = desc->nb_components - ((desc->flags & AV_PIX_FMT_FLAG_ALPHA) ? 1 : 0);
        if (colorComponents < 3) {
            return ePixelCodingGray;
        }
        switch ((desc->log2_chroma_w << 4) | desc->log2_chroma_h) {
        case 0x00:
            return ePixelCodingYUV444;
        case 0x10:
            return ePixelCodingYUV422;
        case 0x11:
            return ePixelCodingYUV420;
        case 0x01:
            return ePixelCodingYUV440;
        case 0x20:
            return ePixelCodingYUV411;
        case 0x22:
            return ePixelCodingYUV410;
        default: // (no such format in n8.0 or n9.0)
            return ePixelCodingNone;
        }
    } // pixelFormatCoding

    int
    pixelFormatBPPFromSpec(PixelCodingEnum coding, int bitdepth, bool alpha)
    {
        int bits = 0;
        switch (coding) {
        case ePixelCodingNone:
            bits = 0;
            break;
        case ePixelCodingGray:
        case ePixelCodingPalette:
        case ePixelCodingBayer:
            bits = bitdepth;
            break;
        case ePixelCodingYUV410:
            bits = (bitdepth * (16 + 2)) / 16;
            break;
        case ePixelCodingYUV411: // 1 Cr & Cb sample per 4x1 Y samples
        case ePixelCodingYUV420: // 1 Cr & Cb sample per 2x2 Y samples
            bits = (bitdepth * (4 + 2)) / 4;
            break;
        case ePixelCodingYUV440: // 1 Cr & Cb sample per 1x2 Y samples
        case ePixelCodingYUV422: // 1 Cr & Cb sample per 2x1 Y samples
            bits = (bitdepth * (2 + 2)) / 2;
            break;
        case ePixelCodingYUV444: // 1 Cr & Cb sample per 1x1 Y samples
        case ePixelCodingRGB: // RGB
        case ePixelCodingXYZ: // XYZ
            bits = bitdepth * 3;
        }
        return alpha ? (bits + bitdepth) : bits;
    }

}
}
