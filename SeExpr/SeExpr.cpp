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
 * OFX SeExpr plugin.
 * Execute a SeExpr script.
 */

#include <algorithm>
#include <cfloat> // DBL_MAX
#include <climits> // INT_MAX
#include <cctype>
#include <map>
#include <limits>
#include <set>
#include <utility>
#include <vector>

//#include <stdio.h> // for snprintf & _snprintf
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define NOMINMAX 1
// windows - defined for both Win32 and Win64
#include <windows.h>
// the following must be included before SePlatform.h tries to include
// them with _CRT_NONSTDC_NO_DEPRECATE=1 and _CRT_SECURE_NO_DEPRECATE=1
#include <io.h>
#include <malloc.h>
#include <process.h>
#include <tchar.h>
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif
#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

#include "ofxsCoords.h"
#include "ofxsCopier.h"
#include "ofxsFilter.h"
#include "ofxsFormatResolution.h"
#include "ofxsMacros.h"
#include "ofxsMultiThread.h"
#include "ofxsRectangleInteract.h"

#define SEEXPR2
#ifdef SEEXPR2
GCC_DIAG_OFF(deprecated)
#include <SeExprBuiltins.h>
#include <SeExprFunc.h>
#include <SeExprNode.h>
#include <SeExpression.h>
#include <SeMutex.h>
GCC_DIAG_ON(deprecated)
#define SeExpr2 SeExpr
namespace SeExpr {
typedef SeExprFuncNode ExprFuncNode;
typedef SeVec3d Vec3d;
typedef SeExprFuncX ExprFuncX;
}
#else
#include <SeExpr2/ExprBuiltins.h>
#include <SeExpr2/ExprFunc.h>
#include <SeExpr2/ExprNode.h>
#include <SeExpr2/Expression.h>
#include <SeExpr2/Mutex.h>
#error "Migration of SeExpr.cpp to SeExpr3 is not finished"
#endif

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
// fix SePlatform.h's bad defines, see https://github.com/wdas/SeExpr/issues/33
#undef snprintf
#undef strtok_r
#if defined(_MSC_VER) && _MSC_VER < 1900
#define snprintf _snprintf
#endif
#if defined(_MSC_VER) && _MSC_VER >= 1400
#define strtok_r(s, d, p) strtok_s(s, d, p)
#endif
#endif // defined(_WIN32) || defined(__WIN32__) || defined(WIN32)

using namespace OFX;

using std::make_pair;
using std::map;
using std::pair;
using std::string;
using std::vector;

OFXS_NAMESPACE_ANONYMOUS_ENTER

#define kPluginName "SeExpr"
#define kPluginNameSimple "SeExprSimple"
#define kPluginGrouping "Merge"
#define kPluginDescriptionHead                                                                                                                                                                                                                                 \
    "Use the SeExpr expression language (by Walt Disney Animation Studios) to process images.\n"                                                                                                                                                               \
    "\n"                                                                                                                                                                                                                                                       \
    "### What is SeExpr?\n"                                                                                                                                                                                                                                    \
    "\n"                                                                                                                                                                                                                                                       \
    "SeExpr is a very simple mathematical expression language used in graphics software (RenderMan, Maya, Mudbox, Yeti).\n"                                                                                                                                    \
    "\n"                                                                                                                                                                                                                                                       \
    "See the [SeExpr Home Page](http://www.disneyanimation.com/technology/seexpr.html) and "                                                                                                                                                                   \
    "[SeExpr Language Documentation](http://wdas.github.io/SeExpr/doxygen/userdoc.html) "                                                                                                                                                                      \
    "for more information.\n"                                                                                                                                                                                                                                  \
    "\n"                                                                                                                                                                                                                                                       \
    "SeExpr is licensed under the Apache License, Version 2.0, and is Copyright Disney Enterprises, Inc.\n"                                                                                                                                                    \
    "\n"                                                                                                                                                                                                                                                       \
    "### SeExpr vs. SeExprSimple\n"                                                                                                                                                                                                                            \
    "\n"                                                                                                                                                                                                                                                       \
    "The SeExpr plugin comes in two versions:\n"                                                                                                                                                                                                               \
    "\n"                                                                                                                                                                                                                                                       \
    "- *SeExpr* has a single vector expression for the color channels, and a scalar expression for the alpha channel. The source color is accessed through the `Cs`vector, and alpha through the `As` scalar, as specified in the original SeExpr language.\n" \
    "- *SeExprSimple* has one scalar expression per channel, and the source channels may also be accessed through scalars (`r`, `g`, `b`, `a`).\n"                                                                                                             \
    "\n"                                                                                                                                                                                                                                                       \
    "### SeExpr extensions\n"                                                                                                                                                                                                                                  \
    "\n"                                                                                                                                                                                                                                                       \
    "A few pre-defined variables and functions were added to the language for filtering and blending several input images.\n"                                                                                                                                  \
    "\n"                                                                                                                                                                                                                                                       \
    "The following pre-defined variables can be used in the script:\n"                                                                                                                                                                                         \
    "\n"                                                                                                                                                                                                                                                       \
    "- `x`: X coordinate (in pixel units) of the pixel to render.\n"                                                                                                                                                                                           \
    "- `y`: Y coordinate (in pixel units) of the pixel to render.\n"                                                                                                                                                                                           \
    "- `u`: X coordinate (normalized in the [0,1] range) of the output pixel to render.\n"                                                                                                                                                                     \
    "- `v`: Y coordinate (normalized in the [0,1] range) of the output pixel to render.\n"                                                                                                                                                                     \
    "- `sx`, `sy`: Scale at which the image is being rendered. Depending on the zoom level "                                                                                                                                                                   \
    "of the viewer, the image might be rendered at a lower scale than usual. "                                                                                                                                                                                 \
    "This parameter is useful when producing spatial effects that need to be invariant "                                                                                                                                                                       \
    "to the pixel scale, especially when using X and Y coordinates. (0.5,0.5) means that the "                                                                                                                                                                 \
    "image is being rendered at half of its original size.\n"                                                                                                                                                                                                  \
    "- `par`: The pixel aspect ratio.\n"                                                                                                                                                                                                                       \
    "- `cx`, `cy`: Shortcuts for `(x + 0.5)/par/sx` and `(y + 0.5)/sy`, i.e. the canonical "                                                                                                                                                                   \
    "coordinates of the current pixel.\n"                                                                                                                                                                                                                      \
    "- `frame`: Current frame being rendered\n"
#define kPluginDescriptionMid ""
#define kPluginDescriptionMidSimple                                                                     \
    "- *SeExprSimple only:* `r`, `g`, `b`, `a`: RGBA channels (scalar) of the image from input 1.\n"    \
    "- *SeExprSimple only:* `rN`, `gN`, `bN`, `aN`: RGBA channels (scalar) of the image from input N, " \
    "e.g. `r2` and `a2` are red and alpha channels from input 2.\n"
#define kPluginDescriptionFoot                                                                                                                        \
    "- `Cs`, `As`: Color (RGB vector) and alpha (scalar) of the image from input 1.\n"                                                                \
    "- `CsN`, `AsN`: Color (RGB vector) and alpha (scalar) of the image from input N, "                                                               \
    "e.g. `Cs2` and `As2` for input 2.\n"                                                                                                             \
    "- `output_width`, `output_height`: Dimensions of the output image being rendered.\n"                                                             \
    "- `input_width`, `input_height`: Dimensions of image from input 1, in pixels.\n"                                                                 \
    "- `input_widthN`, `input_heightN`: Dimensions of image from input N, e.g. `input_width2` and "                                                   \
    "`input_height2` for input 2.\n"                                                                                                                  \
    "\n"                                                                                                                                              \
    "The following additional functions are available:\n"                                                                                             \
    "\n"                                                                                                                                              \
    "- `color cpixel(int i, int f, float x, float y, int interp = 0)`: interpolates the "                                                             \
    "color from input i at the pixel position (x,y) in the image, at frame f.\n"                                                                      \
    "- `float apixel(int i, int f, float x, float y, int interp = 0)`: interpolates the "                                                             \
    "alpha from input i at the pixel position (x,y) in the image, at frame f.\n"                                                                      \
    "\n"                                                                                                                                              \
    "The pixel position of the center of the bottom-left pixel is (0., 0.).\n"                                                                        \
    "\n"                                                                                                                                              \
    "The first input has index i=1.\n"                                                                                                                \
    "\n"                                                                                                                                              \
    "`interp` controls the interpolation filter, and can take one of the following values:\n"                                                         \
    "\n"                                                                                                                                              \
    "- 0: impulse - (nearest neighbor / box) Use original values\n"                                                                                   \
    "- 1: bilinear - (tent / triangle) Bilinear interpolation between original values\n"                                                              \
    "- 2: cubic - (cubic spline) Some smoothing\n"                                                                                                    \
    "- 3: Keys - (Catmull-Rom / Hermite spline) Some smoothing, plus minor sharpening (\\*)\n"                                                        \
    "- 4: Simon - Some smoothing, plus medium sharpening (\\*)\n"                                                                                     \
    "- 5: Rifman - Some smoothing, plus significant sharpening (\\*)\n"                                                                               \
    "- 6: Mitchell - Some smoothing, plus blurring to hide pixelation (\\*)(\\+)\n"                                                                   \
    "- 7: Parzen - (cubic B-spline) Greatest smoothing of all filters (\\+)\n"                                                                        \
    "- 8: notch - Flat smoothing (which tends to hide moire' patterns) (\\+)\n"                                                                       \
    "\n"                                                                                                                                              \
    "Some filters may produce values outside of the initial range (\\*) or modify the values even at integer positions (\\+).\n"                      \
    "\n"                                                                                                                                              \
    "### Sample scripts\n"                                                                                                                            \
    "\n"                                                                                                                                              \
    "#### Add green channel to red, keep green, and apply a 50% gain on blue\n"                                                                       \
    "\n"                                                                                                                                              \
    "*SeExprSimple:*\n"                                                                                                                               \
    "\n"                                                                                                                                              \
    "    r+g\n"                                                                                                                                       \
    "    g\n"                                                                                                                                         \
    "    0.5*b\n"                                                                                                                                     \
    "\n"                                                                                                                                              \
    "*SeExpr:*\n"                                                                                                                                     \
    "\n"                                                                                                                                              \
    "    [Cs[0]+Cs[1], Cs[1], 0.5*Cs[2]]\n"                                                                                                           \
    "\n"                                                                                                                                              \
    "#### \"Multiply\" merge operator on inputs 1 and 2\n"                                                                                            \
    "\n"                                                                                                                                              \
    "*SeExprSimple:*\n"                                                                                                                               \
    "\n"                                                                                                                                              \
    "    r*r2\n"                                                                                                                                      \
    "    g*g2\n"                                                                                                                                      \
    "    b*b2\n"                                                                                                                                      \
    "    a+a2-a*a2\n"                                                                                                                                 \
    "\n"                                                                                                                                              \
    "*SeExpr:*\n"                                                                                                                                     \
    "\n"                                                                                                                                              \
    "    Cs * Cs2\n"                                                                                                                                  \
    "    As + As2 - As * As2\n"                                                                                                                       \
    "\n"                                                                                                                                              \
    "#### \"Over\" merge operator on inputs 1 and 2\n"                                                                                                \
    "\n"                                                                                                                                              \
    "*SeExprSimple:*\n"                                                                                                                               \
    "\n"                                                                                                                                              \
    "    r+r2*(1-a)\n"                                                                                                                                \
    "    g+g2*(1-a)\n"                                                                                                                                \
    "    b+b2*(1-a)\n"                                                                                                                                \
    "    a+a2-a*a2\n"                                                                                                                                 \
    "\n"                                                                                                                                              \
    "*SeExpr:*\n"                                                                                                                                     \
    "\n"                                                                                                                                              \
    "    Cs + Cs2 * (1 -  As)\n"                                                                                                                      \
    "    As + As2 - As * As2\n"                                                                                                                       \
    "\n"                                                                                                                                              \
    "#### Generating a time-varying colored Perlin noise with size x1\n"                                                                              \
    "\n"                                                                                                                                              \
    "    cnoise([cx/x1,cy/x1,frame])\n"                                                                                                               \
    "\n"                                                                                                                                              \
    "#### Average pixels over the previous, current and next frame\n"                                                                                 \
    "\n"                                                                                                                                              \
    "*SeExpr:*\n"                                                                                                                                     \
    "\n"                                                                                                                                              \
    "    prev = cpixel(1,frame - 1,x,y);\n"                                                                                                           \
    "    cur = Cs;\n"                                                                                                                                 \
    "    next = cpixel(1,frame + 1,x,y);\n"                                                                                                           \
    "    (prev + cur + next) / 3;\n"                                                                                                                  \
    "\n"                                                                                                                                              \
    "#### \"Wave\" - displace columns of pixels vertically according to a sine wave function\n"                                                       \
    "\n"                                                                                                                                              \
    "*SeExpr:*\n"                                                                                                                                     \
    "\n"                                                                                                                                              \
    "    cpixel(1,frame,x,y+x2*sy*sin(2*3.1416*(x/sx - x3)/x1),2)\n"                                                                                  \
    "\n"                                                                                                                                              \
    "Set the No. of scalar params to 3.\n"                                                                                                            \
    "\n"                                                                                                                                              \
    "- x1 is the horizontal wavelength in pixels.\n"                                                                                                  \
    "- x2 is the vertical amplitude in pixels.\n"                                                                                                     \
    "- x3 is the horizontal shift in pixels.\n"                                                                                                       \
    "\n"                                                                                                                                              \
    "### Custom parameters\n"                                                                                                                         \
    "\n"                                                                                                                                              \
    "To use custom variables that are pre-defined in the plug-in (scalars, positions and colors) you must reference them "                            \
    "using their script-name in the expression. For example, the parameter x1 can be referenced using x1 in the script:\n"                            \
    "\n"                                                                                                                                              \
    "    Cs + x1\n"                                                                                                                                   \
    "\n"                                                                                                                                              \
    "### Multi-instruction expressions\n"                                                                                                             \
    "\n"                                                                                                                                              \
    "If an expression spans multiple instructions (usually written one per line), "                                                                   \
    "each instruction must end with a semicolon (';'). The last instruction "                                                                         \
    "of the expression is considered as the final value of the pixel (a RGB vector or an Alpha scalar, depending "                                    \
    "on the script), and must not be terminated by a semicolon.\n"                                                                                    \
    "More documentation is available on the [SeExpr website](http://www.disneyanimation.com/technology/seexpr.html).\n"                               \
    "\n"                                                                                                                                              \
    "### Accessing pixel values from other frames\n"                                                                                                  \
    "\n"                                                                                                                                              \
    "The frames fetched by cpixel/apixel to render a given output frame are computed automatically, for every call, even in a branch that is not taken:\n\n" \
    "- The frames of a call are computed if its `frame` argument (and its input index) is made only of `frame`, numbers, operators, `?:` and the usual math functions (e.g. floor, round, abs, min, max, clamp, sqrt, pow, sin...).\n" \
    "- Local variables computed the same way are followed, also through if blocks (`f = frame - 1; cpixel(1, f, x, y)`).\n" \
    "- If the `frame` argument of a call depends on anything else (another variable, the color or alpha of a pixel, the result of another call to cpixel/apixel), the frames of the Input Frame Range parameter are asked for that input, in addition to the frames computed for it."
#define kPluginDescription kPluginDescriptionHead kPluginDescriptionMid kPluginDescriptionFoot
#define kPluginDescriptionSimple kPluginDescriptionHead kPluginDescriptionMidSimple kPluginDescriptionFoot

#define kPluginIdentifier "fr.inria.openfx.SeExpr"
#define kPluginIdentifierSimple "fr.inria.openfx.SeExprSimple"
#define kPluginVersionMajor 2 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.
// History:
// version 1: initial version
// version 2: $scale replaced with $scalex, $scaley; added $par, $cx, $cy; getPixel replaced by cpixel/apixel

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kRenderThreadSafety eRenderFullySafe

#define kSourceClipCount 10

// The arguments of cpixel() and apixel() are computed by the expression, possibly from pixel values: they may be NaN,
// infinite, or out of the range of int. They are tested with OFX::IsNaN() and OFX::IsInfinite(), which test the bits
// and work even if the compiler assumes that there are no NaNs or infinities (-ffast-math), unlike comparisons.
// - The coordinates are clamped to [-kSeExprMaxArg, kSeExprMaxArg] (far out of any image, so that the result is the same
//   as without clamping: the edge pixel for coordinates out of the image) before their conversion to int.
// - A finite frame is used as is (OfxTime is a double); an infinite one is clamped to +-kSeExprMaxArg.
// - A NaN coordinate or frame gives a black pixel.
#define kSeExprMaxArg 1e9

// Clamp a coordinate of cpixel() or apixel() to [-kSeExprMaxArg, kSeExprMaxArg]. Returns false if it is NaN.
static bool
clampPixelArg(double* v)
{
    if (OFX::IsNaN(*v)) {
        return false;
    }
    if (OFX::IsInfinite(*v)) {
        *v = (*v > 0.) ? kSeExprMaxArg : -kSeExprMaxArg;
    } else if (*v > kSeExprMaxArg) {
        *v = kSeExprMaxArg;
    } else if (*v < -kSeExprMaxArg) {
        *v = -kSeExprMaxArg;
    }

    return true;
}

// The frame of cpixel() or apixel(): unchanged if finite, clamped to +-kSeExprMaxArg if infinite. Returns false if it
// is NaN.
static bool
clampPixelFrame(double* v)
{
    if (OFX::IsNaN(*v)) {
        return false;
    }
    if (OFX::IsInfinite(*v)) {
        *v = (*v > 0.) ? kSeExprMaxArg : -kSeExprMaxArg;
    }

    return true;
}

// The input index (0 to kSourceClipCount - 1) of the first argument of cpixel() and apixel(), rounded and clamped (without
// converting a value that is not in the range of int, or NaN, to int).
static int
pixelInputIndex(double v)
{
    if (OFX::IsNaN(v)) {
        return 0;
    }
    const double r = SeExpr::round(v);
    if ((1. <= r) && (r <= (double)kSourceClipCount)) {
        return (int)r - 1;
    }

    return (r > (double)kSourceClipCount) ? (kSourceClipCount - 1) : 0;
}

// The size x2 - x1 of a rectangle, computed in double (it may overflow int, e.g. for an infinite RoD) and clamped to
// the range of int
static int
rectSize(int x1,
         int x2)
{
    const double size = (double)x2 - x1;

    return (size >= (double)INT_MAX) ? INT_MAX : ((size <= (double)INT_MIN) ? INT_MIN : (int)size);
}

// Is the first argument of cpixel() and apixel() a valid input index (1 to kSourceClipCount, once rounded)?
static bool
pixelInputIndexIsValid(double v)
{
    if (OFX::IsNaN(v)) {
        return false;
    }
    // compared once rounded, as master did (0.49999999999999994 rounds to 1 with SeExpr::round())
    const double r = SeExpr::round(v);

    return (1. <= r) && (r <= (double)kSourceClipCount);
}
#define kParamsCount 10

#define kSeExprCPixelFuncName "cpixel"
#define kSeExprAPixelFuncName "apixel"
#define kSeExprCurrentTimeVarName "frame"
#define kSeExprXCoordVarName "x"
#define kSeExprYCoordVarName "y"
#define kSeExprUCoordVarName "u"
#define kSeExprVCoordVarName "v"
#define kSeExprPARVarName "par"
#define kSeExprXCanCoordVarName "cx"
#define kSeExprYCanCoordVarName "cy"
#define kSeExprInputWidthVarName "input_width"
#define kSeExprInputHeightVarName "input_height"
#define kSeExprOutputWidthVarName "output_width"
#define kSeExprOutputHeightVarName "output_height"
#define kSeExprColorVarName "Cs"
#define kSeExprAlphaVarName "As"
#define kSeExprRVarName "r"
#define kSeExprGVarName "g"
#define kSeExprBVarName "b"
#define kSeExprAVarName "a"
#define kSeExprRenderScaleXVarName "sx"
#define kSeExprRenderScaleYVarName "sy"

#define kSeExprDefaultRGBScript "#Just copy the source RGB\nCs"
#define kSeExprDefaultAlphaScript "#Just copy the source alpha\nAs"

#define kParamRegionOfDefinition "rod"
#define kParamRegionOfDefinitionLabel "Region of Definition"
#define kParamRegionOfDefinitionHint "Region of definition (extent) of the output."

#define kParamRegionOfDefinitionOptionUnion "Union", "The output region is the union of the regions of definition of all connected inputs.", "union"
#define kParamRegionOfDefinitionOptionIntersection "Intersection", "The output region is the intersection the regions of definition of all connected inputs.", "intersection"
#define kParamRegionOfDefinitionOptionSize "Size", "The output region is the size of the rectangle overlay.", "size"
#define kParamRegionOfDefinitionOptionFormat "Format", "The output region is the specified format.", "format"
#define kParamRegionOfDefinitionOptionProject "Project", "The output region is the size of the project.", "project"
#define kParamRegionOfDefinitionOptionCustomInput "Input"
#define kParamRegionOfDefinitionOptionCustomInputHelp "The output region is the region of definition of input "
#define kParamRegionOfDefinitionOptionCustomInputEnum "input"
enum RegionOfDefinitionEnum {
    eRegionOfDefinitionOptionUnion,
    eRegionOfDefinitionOptionIntersection,
    eRegionOfDefinitionOptionSize,
    eRegionOfDefinitionOptionFormat,
    eRegionOfDefinitionOptionProject,
    eRegionOfDefinitionOptionCustom,
};

#define kParamGeneratorFormat "format"
#define kParamGeneratorFormatLabel "Format"
#define kParamGeneratorFormatHint "The output format"

#define kParamOutputComponents "outputComponents"
#define kParamOutputComponentsLabel "Output components"
#define kParamOutputComponentsHint "Specify what components to output. In RGB only, the alpha script will not be executed. Similarly, in alpha only, the RGB script " \
                                   "will not be executed."
#define kParamOutputComponentsOptionRGBA "RGBA"
#define kParamOutputComponentsOptionRGB "RGB"
#define kParamOutputComponentsOptionAlpha "Alpha"

#define kParamDoubleParamNumber "doubleParamsNb"
#define kParamDoubleParamNumberLabel "No. of Scalar Params"
#define kParamDoubleParamNumberHint "Use this to control how many scalar parameters should be exposed to the SeExpr expression."

#define kParamDouble "x"
#define kParamDoubleLabel "x"
#define kParamDoubleHint "A custom 1-dimensional variable that can be referenced in the expression by its script-name, x"

#define kParamDouble2DParamNumber "double2DParamsNb"
#define kParamDouble2DParamNumberLabel "No. of 2D Params"
#define kParamDouble2DParamNumberHint "Use this to control how many 2D (position) parameters should be exposed to the SeExpr expression."

#define kParamDouble2D "pos"
#define kParamDouble2DLabel "pos"
#define kParamDouble2DHint "A custom 2-dimensional variable that can be referenced in the expression by its script-name, pos"

#define kParamColorNumber "colorParamsNb"
#define kParamColorNumberLabel "No. of Color Params"
#define kParamColorNumberHint "Use this to control how many color parameters should be exposed to the SeExpr expression."

#define kParamColor "color"
#define kParamColorLabel "color"
#define kParamColorHint "A custom RGB variable that can be referenced in the expression by its script-name, color"

#define kParamFrameRange "frameRange"
#define kParamFrameRangeLabel "Input Frame Range"
#define kParamFrameRangeHint "Default input frame range to fetch images from (may be relative or absolute, depending on the \"frameRangeAbsolute\" parameter). Only used if the frame range cannot be statically computed from the expression. This parameter can be animated."
#define kParamFrameRangeDefault 0, 0

#define kParamFrameRangeAbsolute "frameRangeAbsolute"
#define kParamFrameRangeAbsoluteLabel "Absolute Frame Range"
#define kParamFrameRangeAbsoluteHint "If checked, the frame range is given as absolute frame numbers, else it is relative to the current frame."
#define kParamFrameRangeAbsoluteDefault false

#define kParamRExpr "rExpr"
#define kParamRExprLabel "R="
#define kParamRExprHint "Expression to compute the output red channel. If empty, the channel is left unchanged."
#define kParamGExpr "gExpr"
#define kParamGExprLabel "G="
#define kParamGExprHint "Expression to compute the output green channel. If empty, the channel is left unchanged."
#define kParamBExpr "bExpr"
#define kParamBExprLabel "B="
#define kParamBExprHint "Expression to compute the output blue channel. If empty, the channel is left unchanged."
#define kParamAExpr "aExpr"
#define kParamAExprLabel "A="
#define kParamAExprHint "Expression to compute the output alpha channel. If empty, the channel is left unchanged."

#define kNukeWarnTcl "On Nuke, the characters '$', '[' ']' must be preceded with a backslash (as '\\$', '\\[', '\\]') to avoid TCL variable and expression substitution."

#define kParamScript "script"
#define kParamScriptLabel "RGB Script"
#define kParamScriptHint "Contents of the SeExpr expression. This expression should output the RGB components as a SeExpr vector. See the description of the plug-in and " \
                         "http://www.disneyanimation.com/technology/seexpr.html for documentation."

#define kParamShowExprs "showExprs"
#define kParamShowExprsLabel "Show Exprs"
#define kParamShowExprsHint "Show the contents of the expressions as seen by SeExpr in a dialog window. It may be different from the expressions visible in the GUI, because the host may perform variable or expression substitution on the expressions."

#define kParamShowScript "showScript"
#define kParamShowScriptLabel "Show RGB Script"
#define kParamShowScriptHint "Show the contents of the RGB script as seen by SeExpr in a dialog window. It may be different from the script visible in the GUI, because the host may perform variable or expression substitution on the RGB script parameter."

#define kParamAlphaScript "alphaScript"
#define kParamAlphaScriptLabel "Alpha Script"
#define kParamAlphaScriptHint "Contents of the SeExpr expression. This expression should output the alpha component only as a scalar. See the description of the plug-in and " \
                              "http://www.disneyanimation.com/technology/seexpr.html for documentation."

#define kParamShowAlphaScript "showAlphaScript"
#define kParamShowAlphaScriptLabel "Show Alpha Script"
#define kParamShowAlphaScriptHint "Show the contents of the Alpha script as seen by SeExpr in a dialog window. It may be different from the script visible in the GUI, because the host may perform variable or expression substitution on the Alpha script parameter."

#define kParamValidate "validate"
#define kParamValidateLabel "Validate"
#define kParamValidateHint "Validate the script contents and execute it on next render. This locks the script and all its parameters."

#define kParamHelp "helpButton"
#define kParamHelpLabel "Help..."
#define kParamHelpHint "Display help about using SeExpr."

// Some hosts (e.g. Resolve) may not support normalized defaults (setDefaultCoordinateSystem(eCoordinatesNormalised))
#define kParamDefaultsNormalised "defaultsNormalised"

static bool gHostSupportsDefaultCoordinateSystem = true; // for kParamDefaultsNormalised

static bool gHostIsNatron = false;
static bool gHostSupportsRGBA = false;
static bool gHostSupportsRGB = false;
static bool gHostSupportsAlpha = false;
static PixelComponentEnum gOutputComponentsMap[4];
static string
unsignedToString(unsigned i)
{
    if (i == 0) {
        return "0";
    }
    string nb;
    for (unsigned j = i; j != 0; j /= 10) {
        nb = (char)('0' + (j % 10)) + nb;
    }

    return nb;
}

// Check if s consists only of whitespaces
static bool
isSpaces(const string& s)
{
    return s.find_first_not_of(" \t\n\v\f\r") == string::npos;
}

class SeExprProcessorBase;

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class SeExprPlugin
    : public ImageEffect {
public:
    /** @brief ctor */
    SeExprPlugin(OfxImageEffectHandle handle, bool simple);

    /* Override the render */
    virtual void render(const RenderArguments& args) OVERRIDE FINAL;

    // override isIdentity
    virtual bool isIdentity(const IsIdentityArguments& args, Clip*& identityClip, double& identityTime, int& view, std::string& plane) OVERRIDE FINAL;

    /* override changedParam */
    virtual void changedParam(const InstanceChangedArgs& args, const string& paramName) OVERRIDE FINAL;

    // override the rod call
    virtual bool getRegionOfDefinition(const RegionOfDefinitionArguments& args, OfxRectD& rod) OVERRIDE FINAL;

    // override the roi call
    virtual void getRegionsOfInterest(const RegionsOfInterestArguments& args, RegionOfInterestSetter& rois) OVERRIDE FINAL;
    virtual void getFramesNeeded(const FramesNeededArguments& args, FramesNeededSetter& frames) OVERRIDE FINAL;
    virtual void getClipPreferences(ClipPreferencesSetter& clipPreferences) OVERRIDE FINAL;
    Clip* getClip(int index) const
    {
        assert(index >= 0 && index < kSourceClipCount);

        return _srcClip[index];
    }

    DoubleParam** getDoubleParams() { return _doubleParams; }

    Double2DParam** getDouble2DParams() { return _double2DParams; }

    RGBParam** getRGBParams() { return _colorParams; }

private:
    void setupAndProcess(SeExprProcessorBase& processor, const RenderArguments& args);

    PixelComponentEnum getOutputComponents() const;

private:
    const bool _simple;
    Clip* _srcClip[kSourceClipCount];
    Clip* _maskClip;
    Clip* _dstClip;

    IntParam* _doubleParamCount;
    DoubleParam* _doubleParams[kParamsCount];
    IntParam* _double2DParamCount;
    Double2DParam* _double2DParams[kParamsCount];
    IntParam* _colorParamCount;
    RGBParam* _colorParams[kParamsCount];
    Int2DParam* _frameRange;
    BooleanParam* _frameRangeAbsolute;
    StringParam* _rExpr;
    StringParam* _gExpr;
    StringParam* _bExpr;
    StringParam* _aExpr;
    StringParam* _rgbScript;
    StringParam* _alphaScript;
    BooleanParam* _validate;
    DoubleParam* _mix;
    BooleanParam* _maskApply;
    BooleanParam* _maskInvert;
    ChoiceParam* _boundingBox;
    ChoiceParam* _format;
    Double2DParam* _btmLeft;
    Double2DParam* _size;
    BooleanParam* _interactive;
    ChoiceParam* _outputComponents;
};

PixelComponentEnum
SeExprPlugin::getOutputComponents() const
{
    int outputComponents_i;

    _outputComponents->getValue(outputComponents_i);

    return gOutputComponentsMap[outputComponents_i];
}

class OFXSeExpression;

// Base class for processor, note that we do not use the multi-thread suite.
class SeExprProcessorBase {
protected:
    OfxTime _renderTime;
    int _renderView;
    SeExprPlugin* _plugin;
    OFXSeExpression* _rExpr;
    OFXSeExpression* _gExpr;
    OFXSeExpression* _bExpr;
    OFXSeExpression* _rgbExpr;
    OFXSeExpression* _alphaExpr;
    const Image* _srcCurTime[kSourceClipCount];
    int _nSrcComponents[kSourceClipCount];
    Image* _dstImg;
    bool _maskInvert;
    const Image* _maskImg;
    bool _doMasking;
    double _mix;

    // <clipIndex, <time, image> >
    typedef map<OfxTime, const Image*> FetchedImagesForClipMap;
    typedef map<int, FetchedImagesForClipMap> FetchedImagesMap;
    FetchedImagesMap _images;

public:
    SeExprProcessorBase(SeExprPlugin* instance);

    virtual ~SeExprProcessorBase();

    SeExprPlugin* getPlugin() const
    {
        return _plugin;
    }

    void setDstImg(Image* dstImg)
    {
        _dstImg = dstImg;
    }

    void setMaskImg(const Image* v,
                    bool maskInvert)
    {
        _maskImg = v;
        _maskInvert = maskInvert;
    }

    void doMasking(bool v) { _doMasking = v; }

    void setValues(OfxTime time,
                   int view,
                   double mix,
                   const string& rgbExpr,
                   const string& alphaExpr,
                   const OfxRectI& dstPixelRod,
                   OfxPointI* inputSizes,
                   const OfxPointI& outputSize,
                   const OfxPointD& renderScale,
                   double par);

    void setValuesSimple(OfxTime time,
                         int view,
                         double mix,
                         const string& rExpr,
                         const string& gExpr,
                         const string& bExpr,
                         const string& aExpr,
                         const OfxRectI& dstPixelRod,
                         OfxPointI* inputSizes,
                         const OfxPointI& outputSize,
                         const OfxPointD& renderScale,
                         double par);

    bool isExprOk(string* error);

    void prefetchImage(int inputIndex,
                       OfxTime time)
    {
        // find or create input
        FetchedImagesForClipMap& foundInput = _images[inputIndex];

        FetchedImagesForClipMap::iterator foundImage = foundInput.find(time);
        if (foundImage != foundInput.end()) {
            // image already fetched
            return;
        }

        Clip* clip = _plugin->getClip(inputIndex);
        assert(clip);

        if (!clip->isConnected()) {
            // clip is not connected, image is NULL
            return;
        }

        Image* img = clip->fetchImage(time);
        if (!img) {
            return;
        }
        pair<FetchedImagesForClipMap::iterator, bool> ret = foundInput.insert(make_pair(time, img));
        assert(ret.second);
    }

    const Image* getImage(int inputIndex,
                          OfxTime time)
    {
        // find or create input
        FetchedImagesForClipMap& foundInput = _images[inputIndex];

        FetchedImagesForClipMap::iterator foundImage = foundInput.find(time);
        if (foundImage != foundInput.end()) {
            return foundImage->second;
        }

        return NULL;
    }

    virtual void process(OfxRectI procWindow) = 0;

private:
    void setExprs(OfxTime time,
                  const string& rgbExpr,
                  const string& alphaExpr,
                  const OfxRectI& dstPixelRod,
                  const OfxPointD& renderScale,
                  double par);

    void setExprsSimple(OfxTime time,
                        const string& rExpr,
                        const string& gExpr,
                        const string& bExpr,
                        const string& aExpr,
                        const OfxRectI& dstPixelRod,
                        const OfxPointD& renderScale,
                        double par);

    void setValuesOther(OfxTime time,
                        int view,
                        double mix,
                        OfxPointI* inputSizes,
                        const OfxPointI& outputSize);
};

// implementation of the "apixel" function
template <typename PIX, int nComps, FilterEnum interp, bool alpha>
static void
pixelForDepthCompsFilter(const Image* img,
                         double x,
                         double y,
                         SeExpr2::Vec3d& result)
{
    result = SeExpr2::Vec3d(0., 0., 0.);
    if ((alpha && (nComps != 1) && (nComps != 4)) || (!alpha && (nComps <= 1))) {
        // no value
        return;
    }
    float pix[4];
    // In OFX pixel coordinates, the center of pixel (0,0) has coordinates (0.5,0.5)
    ofxsFilterInterpolate2D<PIX, nComps, interp, /*clamp=*/true>(x + 0.5, y + 0.5, img, /*blackOutside=*/false, pix);
    if (alpha) {
        if (nComps == 1) {
            // alpha input
            result = SeExpr2::Vec3d(pix[0], 0., 0.);
        } else if (nComps == 4) {
            // RGBA input
            result = SeExpr2::Vec3d(pix[3], 0., 0.);
        }
    } else {
        if (nComps == 2) {
            // XY input: no B color
            result = SeExpr2::Vec3d(pix[0], pix[1], 0.);
        } else if (nComps >= 3) {
            // alpha input: no color
            result = SeExpr2::Vec3d(pix[0], pix[1], pix[2]);
        }
    }
}

template <typename PIX, int nComps, bool alpha>
static void
pixelForDepthComps(const Image* img,
                   FilterEnum interp,
                   double x,
                   double y,
                   SeExpr2::Vec3d& result)
{
    switch (interp) {
    case eFilterImpulse:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterImpulse, alpha>(img, x, y, result);
    case eFilterBilinear:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterBilinear, alpha>(img, x, y, result);
    case eFilterCubic:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterCubic, alpha>(img, x, y, result);
    case eFilterKeys:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterKeys, alpha>(img, x, y, result);
    case eFilterSimon:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterSimon, alpha>(img, x, y, result);
    case eFilterRifman:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterRifman, alpha>(img, x, y, result);
    case eFilterMitchell:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterMitchell, alpha>(img, x, y, result);
    case eFilterParzen:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterParzen, alpha>(img, x, y, result);
    case eFilterNotch:

        return pixelForDepthCompsFilter<PIX, nComps, eFilterNotch, alpha>(img, x, y, result);
    default:
        result = SeExpr2::Vec3d(0., 0., 0.);
    }
}

template <typename PIX, bool alpha>
static void
pixelForDepth(const Image* img,
              FilterEnum interp,
              double x,
              double y,
              SeExpr2::Vec3d& result)
{
    int nComponents = img->getPixelComponentCount();

    switch (nComponents) {
    case 1:

        return pixelForDepthComps<PIX, 1, alpha>(img, interp, x, y, result);
    case 2:

        return pixelForDepthComps<PIX, 2, alpha>(img, interp, x, y, result);
    case 3:

        return pixelForDepthComps<PIX, 3, alpha>(img, interp, x, y, result);
    case 4:

        return pixelForDepthComps<PIX, 4, alpha>(img, interp, x, y, result);
    default:
        result = SeExpr2::Vec3d(0., 0., 0.);
    }
}

template <bool alpha>
class PixelFuncX
    : public SeExpr2::ExprFuncX {
    SeExprProcessorBase* _processor;

public:
    PixelFuncX(SeExprProcessorBase* processor)
        : SeExpr2::ExprFuncX(true) // Thread Safe
        , _processor(processor)
    {
    }

    virtual ~PixelFuncX() { }

private:
    virtual bool prep(SeExpr2::ExprFuncNode* node,
                      bool /*wantVec*/)
    {
        // check number of arguments
#ifdef SEEXPR2
        int nargs = node->nargs();
#else
        int nargs = node->numChildren();
#endif

        if ((nargs < 4) || (5 < nargs)) {
            node->addError("Wrong number of arguments, should be 4 or 5");

            return false;
        }

        for (int i = 0; i < nargs; ++i) {
            if (node->child(i)->isVec()) {
                node->addError("Wrong arguments, should be all scalars");

                return false;
            }
            if (!node->child(i)->prep(false)) {
                return false;
            }
        }

        SeExpr2::Vec3d v;
        node->child(0)->eval(v);
        if (!pixelInputIndexIsValid(v[0])) {
            node->addError("Invalid input index");

            return false;
        }

        return true;
    }

    virtual void eval(const SeExpr2::ExprFuncNode* node,
                      SeExpr2::Vec3d& result) const
    {
        SeExpr2::Vec3d v;

        node->child(0)->eval(v);
        const int inputIndex = pixelInputIndex(v[0]);
        node->child(1)->eval(v);
        const double frameArg = v[0];
        node->child(2)->eval(v);
        double x = v[0];
        node->child(3)->eval(v);
        double y = v[0];
        FilterEnum interp = eFilterImpulse;
#ifdef SEEXPR2
        int nargs = node->nargs();
#else
        int nargs = node->numChildren();
#endif
        if (nargs == 5) {
            node->child(4)->eval(v);
            // rounded and clamped to 0..eFilterNotch (without converting a value out of the range of int to int)
            const double interpArg = v[0];
            // compared once rounded, as master did
            const double r = OFX::IsNaN(interpArg) ? 0. : SeExpr::round(interpArg);
            if ((0. <= r) && (r <= (double)eFilterNotch)) {
                interp = (FilterEnum)(int)r;
            } else if (r > (double)eFilterNotch) {
                interp = eFilterNotch;
            }
        }
        double frameClamped = frameArg;
        const bool argsInRange = clampPixelFrame(&frameClamped) && clampPixelArg(&x) && clampPixelArg(&y);
        if (!argsInRange) {
            // the frame or the coordinates are NaN
            result = SeExpr2::Vec3d(0., 0., 0.);

            return;
        }
        const OfxTime frame = SeExpr::round(frameClamped);
        _processor->prefetchImage(inputIndex, frame);
        const Image* img = _processor->getImage(inputIndex, frame);
        if (!img) {
            // be black and transparent
            result.setValue(0., 0., 0.);
        } else {
            BitDepthEnum depth = img->getPixelDepth();
            switch (depth) {
            case eBitDepthFloat:
                pixelForDepth<float, alpha>(img, interp, x, y, result);
                break;
            case eBitDepthUByte:
                pixelForDepth<unsigned char, alpha>(img, interp, x, y, result);
                break;
            case eBitDepthUShort:
                pixelForDepth<unsigned short, alpha>(img, interp, x, y, result);
                break;
            default:
                result.setValue(0., 0., 0.);
                break;
            }
        }
    } // eval
};

class DoubleParamVarRef
    : public SeExprVarRef {
    // Used to call getValue only once per expression evaluation and not once per pixel
    // Using SeExpr lock is faster than calling the multi-thread suite to get a mutex
    SeExprInternal::Mutex _lock;
    bool _varSet;
    double _value;
    DoubleParam* _param;

public:
    DoubleParamVarRef(DoubleParam* param)
        : SeExprVarRef()
        , _lock()
        , _varSet(false)
        , _value(0)
        , _param(param)
    {
    }

    virtual ~DoubleParamVarRef() { }

    //! returns true for a vector type, false for a scalar type
    virtual bool isVec() { return false; }

    //! returns this variable's value by setting result, node refers to
    //! where in the parse tree the evaluation is occurring
    virtual void eval(const SeExprVarNode* /*node*/,
                      SeExpr2::Vec3d& result)
    {
        SeExprInternal::AutoLock<SeExprInternal::Mutex> locker(_lock);
        if (!_varSet) {
            _param->getValue(_value);
            _varSet = true;
        } else {
            result[0] = _value;
        }
    }
};

class Double2DParamVarRef
    : public SeExprVarRef {
    // Used to call getValue only once per expression evaluation and not once per pixel
    // Using SeExpr lock is faster than calling the multi-thread suite to get a mutex
    SeExprInternal::Mutex _lock;
    bool _varSet;
    double _value[2];
    Double2DParam* _param;

public:
    Double2DParamVarRef(Double2DParam* param)
        : SeExprVarRef()
        , _lock()
        , _varSet(false)
        , _value()
        , _param(param)
    {
    }

    virtual ~Double2DParamVarRef() { }

    //! returns true for a vector type, false for a scalar type
    virtual bool isVec() { return true; }

    //! returns this variable's value by setting result, node refers to
    //! where in the parse tree the evaluation is occurring
    virtual void eval(const SeExprVarNode* /*node*/,
                      SeExpr2::Vec3d& result)
    {
        SeExprInternal::AutoLock<SeExprInternal::Mutex> locker(_lock);
        if (!_varSet) {
            _param->getValue(_value[0], _value[1]);
            _varSet = true;
        } else {
            result[0] = _value[0];
            result[1] = _value[1];
        }
    }
};

class ColorParamVarRef
    : public SeExprVarRef {
    // Used to call getValue only once per expression evaluation and not once per pixel
    // Using SeExpr lock is faster than calling the multi-thread suite to get a mutex
    SeExprInternal::Mutex _lock;
    bool _varSet;
    double _value[3];
    RGBParam* _param;

public:
    ColorParamVarRef(RGBParam* param)
        : SeExprVarRef()
        , _lock()
        , _varSet(false)
        , _value()
        , _param(param)
    {
    }

    virtual ~ColorParamVarRef() { }

    //! returns true for a vector type, false for a scalar type
    virtual bool isVec() { return true; }

    //! returns this variable's value by setting result, node refers to
    //! where in the parse tree the evaluation is occurring
    virtual void eval(const SeExprVarNode* /*node*/,
                      SeExpr2::Vec3d& result)
    {
        SeExprInternal::AutoLock<SeExprInternal::Mutex> locker(_lock);
        if (!_varSet) {
            _param->getValue(_value[0], _value[1], _value[2]);
            _varSet = true;
        } else {
            result[0] = _value[0];
            result[1] = _value[1];
            result[2] = _value[2];
        }
    }
};

class SimpleScalar
    : public SeExprVarRef {
public:
    double _value;

    SimpleScalar()
        : SeExprVarRef()
        , _value(0)
    {
    }

    virtual ~SimpleScalar() { }

    virtual bool isVec() { return false; }

    virtual void eval(const SeExprVarNode* /*node*/,
                      SeExpr2::Vec3d& result)
    {
        result[0] = _value;
    }
};

class SimpleVec
    : public SeExprVarRef {
public:
    double _value[3];

    SimpleVec()
        : SeExprVarRef()
        , _value()
    {
        _value[0] = _value[1] = _value[2] = 0.;
    }

    virtual ~SimpleVec() { }

    virtual bool isVec() { return true; }

    virtual void eval(const SeExprVarNode* /*node*/,
                      SeExpr2::Vec3d& result)
    {
        result[0] = _value[0];
        result[1] = _value[1];
        result[2] = _value[2];
    }
};

class StubSeExpression;

class StubPixelFuncX
    : public SeExpr2::ExprFuncX {
    StubSeExpression* _expr;

public:
    StubPixelFuncX(StubSeExpression* expr)
        : SeExpr2::ExprFuncX(true) // Thread Safe
        , _expr(expr)
    {
    }

    virtual ~StubPixelFuncX() { }

private:
    virtual bool prep(SeExpr2::ExprFuncNode* node, bool /*wantVec*/);
    virtual void eval(const SeExpr2::ExprFuncNode* node, SeExpr2::Vec3d& result) const;
};

typedef map<int, vector<OfxTime>> FramesNeeded;

// The value given to the variables other than frame, and returned by cpixel() and apixel(), in the stub expression
// (a finite value). It is never used to compute a frame: the frames are computed from the frame and constants only
// (see StubFrameAnalysis).
#define kStubUnknownValue 0.5

// The name of a variable, without its leading '$'
static string
stubVarName(const char* name)
{
    return (name && (name[0] == '$')) ? string(name + 1) : string(name ? name : "");
}

// Is a function one of the builtin functions of SeExpr 2.11 whose result depends only on its arguments (checked in
// SeExprBuiltins.cpp)?
static bool
stubIsPureMathFunction(const string& name)
{
    static const char* const functions[] = {
        // math
        "abs", "acos", "acosd", "acosh", "asin", "asind", "asinh", "atan", "atand", "atan2", "atan2d", "atanh",
        "bias", "boxstep", "cbrt", "ceil", "clamp", "compress", "contrast", "cos", "cosd", "cosh", "deg", "exp",
        "expand", "fit", "floor", "fmod", "gamma", "gaussstep", "hypot", "invert", "linearstep", "log", "log10", "max",
        "min", "mix", "pow", "rad", "remap", "round", "sin", "sind", "sinh", "smoothstep", "sqrt", "tan", "tand",
        "tanh", "trunc",
        // selection and curves (functions of their arguments; choose() and wchoose() protect against NaN, their
        // allocations depend only on the number of arguments)
        "choose", "wchoose", "curve", "ccurve",
        // vectors and colors
        "length", "dist", "dot", "cross", "norm", "ortho", "up", "angle", "rotate", "rgbtohsl", "hsltorgb", "hsi",
        "saturate",
    };
    // Not here: getVar() (a variable given by its name), printf() (a side effect), cpixel() and apixel() (see
    // StubFrameAnalysis::evaluateFunction() for getVar() and printf()); and the builtins that convert an argument to
    // int, divide by it, or allocate on the stack a size that comes from it: they are evaluated only with valid
    // arguments (stubIsGuardedFunction(), stubGuardedArgsAreValid()), since the calls of a branch that is not taken are
    // evaluated here, which must not crash where the render would not.
    // A function of the list redefined by a SeExpr plug-in (SE_EXPR_PLUGINS) is trusted to be a function of its
    // arguments too.
    for (std::size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        if (name == functions[i]) {
            return true;
        }
    }

    return false;
}

// Is a node one of those whose value is a function of their children (operators, ?:, vector literals, indexing)?
static bool
stubIsPureOperator(const SeExprNode* node)
{
    return dynamic_cast<const SeExprNegNode*>(node) || dynamic_cast<const SeExprNotNode*>(node)
           || dynamic_cast<const SeExprCondNode*>(node) || dynamic_cast<const SeExprAndNode*>(node)
           || dynamic_cast<const SeExprOrNode*>(node) || dynamic_cast<const SeExprCompareEqNode*>(node)
           || dynamic_cast<const SeExprCompareNode*>(node) || dynamic_cast<const SeExprAddNode*>(node)
           || dynamic_cast<const SeExprSubNode*>(node) || dynamic_cast<const SeExprMulNode*>(node)
           || dynamic_cast<const SeExprDivNode*>(node) || dynamic_cast<const SeExprModNode*>(node)
           || dynamic_cast<const SeExprExpNode*>(node) || dynamic_cast<const SeExprInvertNode*>(node)
           || dynamic_cast<const SeExprVecNode*>(node) || dynamic_cast<const SeExprSubscriptNode*>(node);
}

// Is a function one of the builtins of SeExpr 2.11 that are functions of their arguments, but convert an argument to
// int (undefined behaviour out of the range of int, or for NaN), divide by an argument converted to int, or allocate on
// the stack a size that comes from an argument? They are evaluated by the analysis only if stubGuardedArgsAreValid()
// (checked in SeExprBuiltins.cpp and SeNoise.cpp of SeExpr 2.11).
static bool
stubIsGuardedFunction(const string& name)
{
    static const char* const functions[] = {
        "noise", "snoise", "vnoise", "cnoise", "snoise4", "vnoise4", "cnoise4", // floor(argument) to int
        "fbm", "vfbm", "cfbm", "turbulence", "vturbulence", "cturbulence", // the same, at p * lacunarity^k + 1234...
        "pnoise", // floor(argument) to int, int(period), and % period
        "cellnoise", "ccellnoise", // floor(argument) to uint32_t
        "voronoi", "cvoronoi", "pvoronoi", // cellnoise of the cells around the point, int(type), fbm
        "hash", // frexp(argument) to uint32_t
        "pick", // alloca(int(hi) - int(lo) + 1), twice
        "cycle", "spline", "remap", "midhsi", // an argument to int
    };
    for (std::size_t i = 0; i < sizeof(functions) / sizeof(functions[0]); ++i) {
        if (name == functions[i]) {
            return true;
        }
    }

    return false;
}

// The bound of |p| after the octaves of FBM() (SeNoise.cpp: p = p * lacunarity + 1234 at each octave), or more than
// 2^29 if it is too large
static double
stubFbmBound(double p,
             double octaves,
             double lacunarity)
{
    const double maxValue = 536870912.; // 2^29
    const int n = (int)((octaves < 1.) ? 1. : ((octaves > 8.) ? 8. : octaves)); // int(clamp(octaves, 1, 8))
    double bound = p;
    for (int k = 1; (k < n) && (bound <= maxValue); ++k) {
        bound = bound * ((lacunarity < 0.) ? -lacunarity : lacunarity) + 1234.;
    }

    return bound;
}

// Are the values of the arguments of a guarded node (a function of stubIsGuardedFunction(), or a subscript, name "[]")
// such that it is defined, and does not allocate too much? They are computed from the frame and constants
// (OFX::IsNaN() and OFX::IsInfinite() test them, they are not markers):
// - all the components must be finite and in [-2^29, 2^29] (so that floor(v) + 1, the conversions to int and the
//   int arithmetic of the builtins are defined);
// - cellnoise(), ccellnoise(): in [0, 2^29] (a conversion to uint32_t);
// - pick(index, lo, hi, ...): hi - lo at most 4096; cycle(index, lo, hi): hi - lo less than 2^29;
// - fbm(p, octaves = 6, lacunarity = 2, gain = 0.5) and the others of the family: p * |lacunarity|^k + 1234...
//   (the point of each octave) in [-2^29, 2^29];
// - pnoise(p, period): int(period) at least 1 (it is the divisor of %);
// - voronoi(p, type, jitter, fbmScale, fbmOctaves, fbmLacunarity, fbmGain), cvoronoi() (the same arguments) and
//   pvoronoi(p, jitter, fbmScale, fbmOctaves, fbmLacunarity, fbmGain) (no type: its fbmScale is the third argument,
//   whatever its docstring says): fbmScale 0 (no fbm distortion), and p in [2, 2^29] (the cells around it,
//   floor(p) - 1, are converted to uint32_t).
static bool
stubGuardedArgsAreValid(const string& name,
                        const vector<SeVec3d>& args)
{
    const double maxValue = 536870912.; // 2^29
    if (name == "[]") {
        // v[index]: int(index)
        return (args.size() == 2) && !OFX::IsNaN(args[1][0]) && !OFX::IsInfinite(args[1][0]) && (args[1][0] <= maxValue) && (args[1][0] >= -maxValue);
    }
    const bool nonNegative = (name == "cellnoise") || (name == "ccellnoise");
    double maxP = 0.;
    for (std::size_t i = 0; i < args.size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            const double v = args[i][c];
            if (OFX::IsNaN(v) || OFX::IsInfinite(v) || (v > maxValue) || (v < (nonNegative ? 0. : -maxValue))) {
                return false;
            }
            if (i == 0) {
                maxP = (std::max)(maxP, (v < 0.) ? -v : v);
            }
        }
    }
    if (name == "pick") {
        // pick(index, lo, hi, weights...)
        return (args.size() >= 3) && ((int)args[2][0] - (int)args[1][0] <= 4096);
    }
    if (name == "cycle") {
        return (args.size() >= 3) && ((int)args[2][0] - (int)args[1][0] < (int)maxValue);
    }
    if ((name == "fbm") || (name == "vfbm") || (name == "cfbm") || (name == "turbulence") || (name == "vturbulence")
        || (name == "cturbulence")) {
        const double octaves = (args.size() >= 2) ? args[1][0] : 6.;
        const double lacunarity = (args.size() >= 3) ? args[2][0] : 2.;

        return stubFbmBound(maxP, octaves, lacunarity) <= maxValue;
    }
    if (name == "pnoise") {
        if (args.size() < 2) {
            return false;
        }
        for (int c = 0; c < 3; ++c) {
            if ((int)args[1][c] < 1) {
                return false;
            }
        }

        return true;
    }
    if ((name == "voronoi") || (name == "cvoronoi") || (name == "pvoronoi")) {
        const std::size_t fbmScale = (name == "pvoronoi") ? 2 : 3; // SeExprBuiltins.cpp: pvoronoiFn() has no type
        if ((args.size() > fbmScale) && (args[fbmScale][0] != 0.)) {
            return false; // fbm distortion
        }
        for (int c = 0; c < 3; ++c) {
            if ((args.empty()) || (args[0][c] < 2.)) {
                return false;
            }
        }

        return true;
    }

    return true;
}

// Is the subtree made only of numbers, variables, the operators of stubIsPureOperator() and the functions of
// stubIsPureMathFunction() and stubIsGuardedFunction()? Its variable nodes are added to vars, its guarded nodes (the
// calls to guarded functions, and the subscripts, whose index is converted to int) to guarded (in postorder: the inner
// ones first).
static bool
stubIsPureExpression(const SeExprNode* node,
                     vector<const SeExprVarNode*>* vars,
                     vector<const SeExprNode*>* guarded = NULL)
{
    if (const SeExprVarNode* var = dynamic_cast<const SeExprVarNode*>(node)) {
        vars->push_back(var);

        return true;
    }
    if (dynamic_cast<const SeExprNumNode*>(node)) {
        return true;
    }
    const SeExprFuncNode* func = dynamic_cast<const SeExprFuncNode*>(node);
    if (func) {
        if (!stubIsPureMathFunction(func->name()) && !stubIsGuardedFunction(func->name())) {
            return false;
        }
    } else if (!stubIsPureOperator(node)) {
        return false;
    }
    for (int i = 0; i < node->numChildren(); ++i) {
        if (!stubIsPureExpression(node->child(i), vars, guarded)) {
            return false;
        }
    }
    if (guarded && ((func && stubIsGuardedFunction(func->name())) || dynamic_cast<const SeExprSubscriptNode*>(node))) {
        guarded->push_back(node);
    }

    return true;
}

// The name of a guarded node for stubGuardedArgsAreValid()
static string
stubGuardedName(const SeExprNode* node)
{
    const SeExprFuncNode* func = dynamic_cast<const SeExprFuncNode*>(node);

    return func ? string(func->name()) : string("[]");
}

// Is a computed value finite (all its components)? A value that is not (e.g. sqrt(frame - 20) at frame 10) is not used:
// the set arithmetic of the analysis, compiled with -ffast-math, would not handle it.
static bool
stubIsFinite(const SeVec3d& v)
{
    for (int c = 0; c < 3; ++c) {
        if (OFX::IsNaN(v[c]) || OFX::IsInfinite(v[c])) {
            return false;
        }
    }

    return true;
}

// The variables other than frame, in the stub expression: their values are not known. Reading one (eval()) sets the
// flag touched: an evaluation that read it does not give a known value. (Its value, kStubUnknownValue, is never used.)
class StubUnknownScalar
    : public SeExprVarRef {
public:
    mutable bool touched;

    StubUnknownScalar()
        : SeExprVarRef()
        , touched(false)
    {
    }

    virtual ~StubUnknownScalar() { }

    virtual bool isVec() { return false; }

    virtual void eval(const SeExprVarNode* /*node*/,
                      SeExpr2::Vec3d& result)
    {
        touched = true;
        result[0] = result[1] = result[2] = kStubUnknownValue;
    }
};

/**
 * @brief Used to determine what are the frames needed and RoIs of the expression
 *
 * Every cpixel() and apixel() call of the expression is recorded when it is prepped (whether or not it would be
 * evaluated), and analyzed by StubFrameAnalysis. The variables other than frame are given kStubUnknownValue; the
 * expression itself is not evaluated.
 **/
class StubSeExpression
    : public SeExpression {
    mutable StubUnknownScalar _unknownScalar;
    mutable StubPixelFuncX _pixel;
    mutable SeExprFunc _pixelFunction;
    mutable SimpleScalar _currentTime;
    mutable vector<const SeExprFuncNode*> _pixelCalls; // the cpixel() and apixel() calls (prepped)

public:
    StubSeExpression(const string& expr,
                     bool wantVec,
                     OfxTime time);

    virtual ~StubSeExpression();

    /** override resolveVar to add external variables */
    virtual SeExprVarRef* resolveVar(const string& name) const OVERRIDE FINAL;

    /** override resolveFunc to add external functions */
    virtual SeExprFunc* resolveFunc(const string& name) const OVERRIDE FINAL;

    void onPixelPrepped(const SeExprFuncNode* node) const
    {
        if (std::find(_pixelCalls.begin(), _pixelCalls.end(), node) == _pixelCalls.end()) {
            _pixelCalls.push_back(node);
        }
    }

    const vector<const SeExprFuncNode*>& getPixelCalls() const
    {
        return _pixelCalls;
    }

    // was a variable that is not known read (see StubUnknownScalar)?
    bool touched() const
    {
        return _unknownScalar.touched;
    }

    void resetTouched() const
    {
        _unknownScalar.touched = false;
    }
};

class OFXSeExpression
    : public SeExpression {
    const bool _simple;
    mutable PixelFuncX<false> _cpixel;
    mutable SeExprFunc _cpixelFunction;
    mutable PixelFuncX<true> _apixel;
    mutable SeExprFunc _apixelFunction;
    OfxRectI _dstPixelRod;
    typedef map<string, SeExprVarRef*> VariablesMap;
    VariablesMap _variables;
    SimpleScalar _scalex;
    SimpleScalar _scaley;
    SimpleScalar _curTime;
    SimpleScalar _xCoord;
    SimpleScalar _yCoord;
    SimpleScalar _uCoord;
    SimpleScalar _vCoord;
    SimpleScalar _par;
    SimpleScalar _xCanCoord;
    SimpleScalar _yCanCoord;
    SimpleScalar _outputWidth;
    SimpleScalar _outputHeight;
    SimpleScalar _inputWidths[kSourceClipCount];
    SimpleScalar _inputHeights[kSourceClipCount];
    SimpleScalar _inputR[kSourceClipCount];
    SimpleScalar _inputG[kSourceClipCount];
    SimpleScalar _inputB[kSourceClipCount];
    SimpleVec _inputColors[kSourceClipCount];
    SimpleScalar _inputAlphas[kSourceClipCount];
    DoubleParamVarRef* _doubleRef[kParamsCount];
    Double2DParamVarRef* _double2DRef[kParamsCount];
    ColorParamVarRef* _colorRef[kParamsCount];

public:
    OFXSeExpression(SeExprProcessorBase* processor,
                    const string& expr,
                    bool wantVec,
                    bool simple,
                    OfxTime time,
                    const OfxPointD& renderScale,
                    double par,
                    const OfxRectI& outputRod);

    virtual ~OFXSeExpression();

    /** override resolveVar to add external variables */
    virtual SeExprVarRef* resolveVar(const string& name) const OVERRIDE FINAL;

    /** override resolveFunc to add external functions */
    virtual SeExprFunc* resolveFunc(const string& name) const OVERRIDE FINAL;

    /** NOT MT-SAFE, this object is to be used PER-THREAD*/
    void setXY(int x,
               int y)
    {
        _xCoord._value = x;
        _yCoord._value = y;
        // u and v are 0 if the output RoD is empty
        // in double: the difference may overflow int (e.g. an infinite RoD)
        const double rodWidth = (double)_dstPixelRod.x2 - _dstPixelRod.x1;
        const double rodHeight = (double)_dstPixelRod.y2 - _dstPixelRod.y1;
        _uCoord._value = (rodWidth > 0) ? (x + 0.5 - _dstPixelRod.x1) / rodWidth : 0.;
        _vCoord._value = (rodHeight > 0) ? (y + 0.5 - _dstPixelRod.y1) / rodHeight : 0.;
        _xCanCoord._value = (x + 0.5) * _par._value / _scalex._value;
        _yCanCoord._value = (y + 0.5) / _scaley._value;
    }

    void setRGBA(int inputIndex,
                 float r,
                 float g,
                 float b,
                 float a)
    {
        if (_simple) {
            _inputR[inputIndex]._value = r;
            _inputG[inputIndex]._value = g;
            _inputB[inputIndex]._value = b;
        }
        _inputColors[inputIndex]._value[0] = r;
        _inputColors[inputIndex]._value[1] = g;
        _inputColors[inputIndex]._value[2] = b;
        _inputAlphas[inputIndex]._value = a;
    }

    void setSize(int inputNumber,
                 int w,
                 int h)
    {
        if (inputNumber == -1) {
            _outputWidth._value = w;
            _outputHeight._value = h;
        } else {
            _inputWidths[inputNumber]._value = w;
            _inputHeights[inputNumber]._value = h;
        }
    }
};

OFXSeExpression::OFXSeExpression(SeExprProcessorBase* processor,
                                 const string& expr,
                                 bool wantVec,
                                 bool simple,
                                 OfxTime time,
                                 const OfxPointD& renderScale,
                                 double par,
                                 const OfxRectI& outputRod)
    : SeExpression(expr, wantVec)
    , _simple(simple)
    , _cpixel(processor)
    , _cpixelFunction(_cpixel, 4, 5)
    , _apixel(processor)
    , _apixelFunction(_apixel, 4, 5)
    , _dstPixelRod(outputRod)
    , _variables()
    , _scalex()
    , _scaley()
    , _curTime()
    , _xCoord()
    , _yCoord()
    , _uCoord()
    , _vCoord()
    , _par()
    , _xCanCoord()
    , _yCanCoord()
    , _outputWidth()
    , _outputHeight()
    , _inputWidths()
    , _inputHeights()
    , _inputColors()
    , _inputAlphas()
    , _doubleRef()
    , _double2DRef()
    , _colorRef()
{
    _dstPixelRod = outputRod;

    // a render scale of 0 or less, NaN or infinite is invalid (x and y are divided by it)
    _scalex._value = (!OFX::IsNaN(renderScale.x) && !OFX::IsInfinite(renderScale.x) && (renderScale.x > 0.)) ? renderScale.x : 1.;
    _variables[kSeExprRenderScaleXVarName] = &_scalex;

    _scaley._value = (!OFX::IsNaN(renderScale.y) && !OFX::IsInfinite(renderScale.y) && (renderScale.y > 0.)) ? renderScale.y : 1.;
    _variables[kSeExprRenderScaleYVarName] = &_scaley;

    _curTime._value = time;
    _variables[kSeExprCurrentTimeVarName] = &_curTime;

    _variables[kSeExprXCoordVarName] = &_xCoord;

    _variables[kSeExprYCoordVarName] = &_yCoord;

    _variables[kSeExprUCoordVarName] = &_uCoord;

    _variables[kSeExprVCoordVarName] = &_vCoord;

    _par._value = par;
    _variables[kSeExprPARVarName] = &_par;

    _variables[kSeExprXCanCoordVarName] = &_xCanCoord;

    _variables[kSeExprYCanCoordVarName] = &_yCanCoord;

    _variables[kSeExprOutputWidthVarName] = &_outputWidth;

    _variables[kSeExprOutputHeightVarName] = &_outputHeight;

    for (int i = 0; i < kSourceClipCount; ++i) {
        const string istr = unsignedToString(i + 1);
        _variables[kSeExprInputWidthVarName + istr] = &_inputWidths[i];
        _variables[kSeExprInputHeightVarName + istr] = &_inputHeights[i];
        if (_simple) {
            _variables[kSeExprRVarName + istr] = &_inputR[i];
            _variables[kSeExprGVarName + istr] = &_inputG[i];
            _variables[kSeExprBVarName + istr] = &_inputB[i];
            _variables[kSeExprAVarName + istr] = &_inputAlphas[i];
        }
        _variables[kSeExprColorVarName + istr] = &_inputColors[i];
        _variables[kSeExprAlphaVarName + istr] = &_inputAlphas[i];
        if (i == 0) {
            // default names for the first input
            _variables[kSeExprInputWidthVarName] = &_inputWidths[i];
            _variables[kSeExprInputHeightVarName] = &_inputHeights[i];
            if (_simple) {
                _variables[kSeExprRVarName] = &_inputR[i];
                _variables[kSeExprGVarName] = &_inputG[i];
                _variables[kSeExprBVarName] = &_inputB[i];
                _variables[kSeExprAVarName] = &_inputAlphas[i];
            }
            _variables[kSeExprColorVarName] = &_inputColors[i];
            _variables[kSeExprAlphaVarName] = &_inputAlphas[i];
        }
    }

    assert(processor);
    SeExprPlugin* plugin = processor->getPlugin();
    DoubleParam** doubleParams = plugin->getDoubleParams();
    Double2DParam** double2DParams = plugin->getDouble2DParams();
    RGBParam** colorParams = plugin->getRGBParams();

    for (int i = 0; i < kParamsCount; ++i) {
        _doubleRef[i] = new DoubleParamVarRef(doubleParams[i]);
        _double2DRef[i] = new Double2DParamVarRef(double2DParams[i]);
        _colorRef[i] = new ColorParamVarRef(colorParams[i]);
        const string istr = unsignedToString(i + 1);
        _variables[kParamDouble + istr] = _doubleRef[i];
        _variables[kParamDouble2D + istr] = _double2DRef[i];
        _variables[kParamColor + istr] = _colorRef[i];
    }
}

OFXSeExpression::~OFXSeExpression()
{
    for (int i = 0; i < kParamsCount; ++i) {
        delete _doubleRef[i];
        delete _double2DRef[i];
        delete _colorRef[i];
    }
}

SeExprVarRef*
OFXSeExpression::resolveVar(const string& varName) const
{
    VariablesMap::const_iterator found = _variables.find(varName);
    if (found == _variables.end()) {
        return 0;
    }

    return found->second;
}

SeExprFunc*
OFXSeExpression::resolveFunc(const string& funcName) const
{
    // check if it is builtin so we get proper behavior
    if (SeExprFunc::lookup(funcName)) {
        return 0;
    }
    if (funcName == kSeExprCPixelFuncName) {
        return &_cpixelFunction;
    }
    if (funcName == kSeExprAPixelFuncName) {
        return &_apixelFunction;
    }

    return 0;
}

bool
StubPixelFuncX::prep(SeExpr2::ExprFuncNode* node,
                     bool /*wantVec*/)
{
    // check number of arguments
    int nargs = node->nargs();

    if ((nargs < 4) || (5 < nargs)) {
        node->addError("Wrong number of arguments, should be 4 or 5");

        return false;
    }

    for (int i = 0; i < nargs; ++i) {
        if (node->child(i)->isVec()) {
            node->addError("Wrong arguments, should be all scalars");

            return false;
        }
        if (!node->child(i)->prep(false)) {
            return false;
        }
    }

    // the input index is checked if it is a number (an input index that is computed is checked by the analysis, and
    // one that is not computable gives the default frame range to all the inputs)
    if (dynamic_cast<const SeExprNumNode*>(node->child(0))) {
        SeExpr2::Vec3d v;
        node->child(0)->eval(v);
        if (!pixelInputIndexIsValid(v[0])) {
            node->addError("Invalid input index");

            return false;
        }
    }
    _expr->onPixelPrepped(node);

    return true;
}

void
StubPixelFuncX::eval(const SeExpr2::ExprFuncNode* /*node*/,
                     SeExpr2::Vec3d& result) const
{
    // the pixel value is unknown (only evaluated to find the variable of an assignment, see StubFrameAnalysis)
    result[0] = result[1] = result[2] = kStubUnknownValue;
}

StubSeExpression::StubSeExpression(const string& expr,
                                   bool wantVec,
                                   OfxTime time)
    : SeExpression(expr, wantVec)
    , _unknownScalar()
    , _pixel(this)
    , _pixelFunction(_pixel, 4, 5)
    , _currentTime()
    , _pixelCalls()
{
    _currentTime._value = time;
}

StubSeExpression::~StubSeExpression()
{
}

// Does the parse tree of SeExpr give the node classes that StubFrameAnalysis expects? If dynamic_cast failed
// (e.g. without the type information of the SeExpr library), every node would look like an unknown one; this checks
// it once, on cpixel(1, frame, x, 0) and cpixel(1, frame + 1, x, 0).
static bool
stubAnalysisWorks()
{
    static const bool works = []() {
        StubSeExpression expr("cpixel(1, frame, x, 0) + cpixel(1, frame + 1, x, 0)", false, 0.);
        if (!expr.isValid() || (expr.getPixelCalls().size() != 2)) {
            return false;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            const SeExprFuncNode* call = expr.getPixelCalls()[i];
            vector<const SeExprVarNode*> vars;
            if ((call->numChildren() < 3) || !dynamic_cast<const SeExprNumNode*>(call->child(0))
                || !dynamic_cast<const SeExprVarNode*>(call->child(2)) || !stubIsPureExpression(call->child(1), &vars)
                || (vars.size() != 1) || (stubVarName(vars[0]->name()) != kSeExprCurrentTimeVarName)) {
                return false;
            }
        }

        return true;
    }();

    return works;
}

// The frames of an input are not known: the default frame range is used (in addition to its known frames)
static void
stubAddUnknown(int input,
               FramesNeeded* frames,
               std::set<int>* unknownFrames)
{
    unknownFrames->insert(input);
    (*frames)[input];
}

// A frame of an input: a NaN frame gives the default frame range (cpixel() gives black; the frame is computed from the
// frame and constants, and tested with OFX::IsNaN()), an infinite frame is clamped as in the render
// (clampPixelFrame()), and the frame is rounded as on master
static void
stubAddFrame(int input,
             double frame,
             FramesNeeded* frames,
             std::set<int>* unknownFrames)
{
    if (!clampPixelFrame(&frame)) {
        stubAddUnknown(input, frames, unknownFrames);

        return;
    }
    vector<OfxTime>& times = (*frames)[input];
    const OfxTime t = SeExpr::round(frame);
    if (std::find(times.begin(), times.end(), t) == times.end()) {
        times.push_back(t);
    }
}

#define kStubMaxValues 64 // above, the values of a variable are not exact

// The values that a variable may have at a point of the expression. known: the variable has one of these values (a
// superset); otherwise, it may also have other values (not known): its values, if any, are used, and the default
// frame range too.
struct StubValues
{
    bool known;
    vector<SeVec3d> values;

    StubValues()
        : known(false)
        , values()
    {
    }

    explicit StubValues(const SeVec3d& v)
        : known(true)
        , values(1, v)
    {
    }

    void add(const SeVec3d& v)
    {
        for (std::size_t i = 0; i < values.size(); ++i) {
            if ((values[i][0] == v[0]) && (values[i][1] == v[1]) && (values[i][2] == v[2])) {
                return;
            }
        }
        values.push_back(v);
    }

    // add the values of another set (a union: not exact if either is not exact)
    void merge(const StubValues& other)
    {
        known = known && other.known;
        for (std::size_t i = 0; i < other.values.size(); ++i) {
            add(other.values[i]);
        }
        cap();
    }

    // at most kStubMaxValues values: above, the first ones, not exact
    void cap()
    {
        if (values.size() > kStubMaxValues) {
            values.resize(kStubMaxValues);
            known = false;
        }
    }

    bool operator==(const StubValues& other) const
    {
        if ((known != other.known) || (values.size() != other.values.size())) {
            return false;
        }
        for (std::size_t i = 0; i < values.size(); ++i) {
            bool found = false;
            for (std::size_t k = 0; !found && (k < other.values.size()); ++k) {
                found = (values[i][0] == other.values[k][0]) && (values[i][1] == other.values[k][1]) && (values[i][2] == other.values[k][2]);
            }
            if (!found) {
                return false;
            }
        }

        return true;
    }
};

typedef map<string, StubValues> StubState; // the values of the local variables, by name (without '$')

#define kStubMaxCombinations 1024 // the combinations of the values of the variables of an expression evaluated
#define kStubMaxIterations 8 // the evaluations of the statements to find the values that the locals keep between pixels
#define kStubMaxWork 100000. // the maximum of (the number of nodes of the parse tree) * (the number of locals + 1)
#define kStubMaxDepth 256 // the maximum depth of the parse tree (the analysis is recursive: its stack grows with it)
#define kStubMaxWalk 1000000. // the maximum of (the sum of the sizes of the subtrees of all the nodes) * (the number of locals + 1)

// The variables assigned in the text of an expression, in order (without '$'), found with the rules of the lexer of
// SeExpr 2.11 (SeExprParserLex.l): the tokens are skipped up to each variable (VAR, $name or $name::name, or NAME,
// [a-zA-Z_][a-zA-Z0-9_.]*) followed by '=' (not "==") or by one of +=, -=, *=, /=, %=, ^=; strings ("..." and '...',
// with escaped quotes, on one line), comments (# up to the end of the line or a "\n" escape), numbers, the escapes
// "\n" and "\t", and the two-character operators (==, !=, <=, >=, ||, &&, ->) are skipped.
static void
stubAssignedVariables(const string& text,
                      vector<string>* names)
{
    const std::size_t n = text.size();
    std::size_t i = 0;
    string previous; // the name of the previous token, if it was a variable
    while (i < n) {
        const char c = text[i];
        if ((c == ' ') || (c == '\t') || (c == '\n') || (c == '\r')) {
            ++i;
            continue; // whitespace: the previous token is still the last one
        }
        if ((c == '\\') && (i + 1 < n) && ((text[i + 1] == 'n') || (text[i + 1] == 't'))) {
            i += 2;
            continue; // a quoted newline or tab, ignored
        }
        if (c == '#') {
            // a comment, up to the end of the line, or to a "\n" escape
            ++i;
            // (\#([^\\\n]|\\[^n\n])*: a backslash followed by n, a newline or nothing ends it)
            while ((i < n) && (text[i] != '\n')
                   && !((text[i] == '\\') && ((i + 1 >= n) || (text[i + 1] == 'n') || (text[i + 1] == '\n')))) {
                i += (text[i] == '\\') ? 2 : 1;
            }
            continue;
        }
        if ((c == '"') || (c == '\'')) {
            // a string on one line (\"(\\\"|[^"\n])*\"): the longest match, which ends at the last quote of the line
            // before the first one that is not escaped (a quote after a backslash may end the string or be escaped);
            // no closing quote: the quote is a character token
            std::size_t end = n;
            for (std::size_t k = i + 1; (k < n) && (text[k] != '\n'); ++k) {
                if (text[k] == c) {
                    end = k;
                    if (text[k - 1] != '\\') {
                        break;
                    }
                }
            }
            i = (end < n) ? end + 1 : i + 1;
            previous.clear();
            continue;
        }
        if ((c == '$') || (c == '_') || std::isalpha((unsigned char)c)) {
            std::size_t k = i + ((c == '$') ? 1 : 0);
            while ((k < n) && ((text[k] == '_') || (text[k] == '.') || std::isalnum((unsigned char)text[k]))) {
                ++k;
            }
            if ((c == '$') && (k + 1 < n) && (text[k] == ':') && (text[k + 1] == ':')) {
                // $name::name
                k += 2;
                while ((k < n) && ((text[k] == '_') || (text[k] == '.') || std::isalnum((unsigned char)text[k]))) {
                    ++k;
                }
            }
            previous = stubVarName(text.substr(i, k - i).c_str());
            i = k;
            continue;
        }
        if (std::isdigit((unsigned char)c) || ((c == '.') && (i + 1 < n) && std::isdigit((unsigned char)text[i + 1]))) {
            // a number: digits, '.', and an exponent
            while ((i < n) && (std::isdigit((unsigned char)text[i]) || (text[i] == '.'))) {
                ++i;
            }
            if ((i < n) && ((text[i] == 'e') || (text[i] == 'E'))) {
                std::size_t k = i + 1;
                if ((k < n) && ((text[k] == '+') || (text[k] == '-'))) {
                    ++k;
                }
                if ((k < n) && std::isdigit((unsigned char)text[k])) {
                    i = k;
                    while ((i < n) && std::isdigit((unsigned char)text[i])) {
                        ++i;
                    }
                }
            }
            previous.clear();
            continue;
        }
        const char d = (i + 1 < n) ? text[i + 1] : '\0';
        const bool twoChars = ((c == '=') && (d == '=')) || ((c == '!') && (d == '=')) || ((c == '<') && (d == '='))
                              || ((c == '>') && (d == '=')) || ((c == '|') && (d == '|')) || ((c == '&') && (d == '&'))
                              || ((c == '-') && (d == '>'));
        const bool assignment = ((c == '=') && (d != '=')) || ((d == '=') && ((c == '+') || (c == '-') || (c == '*') || (c == '/') || (c == '%') || (c == '^')));
        if (assignment && !previous.empty() && (previous != "if") && (previous != "else")) {
            names->push_back(previous);
        }
        i += (twoChars || (assignment && (c != '='))) ? 2 : 1;
        previous.clear();
    }
}

/**
 * @brief The frames fetched by cpixel() and apixel(), found by an analysis of the parse tree
 *
 * The statements of the expression (assignments of local variables, if blocks) are followed in order, with the set of
 * the values that each local variable may have (StubValues):
 * - an expression is evaluated by SeExpr for every combination of the values of the local variables it reads, if it is
 *   made only of numbers, variables, operators, ?: and the functions of stubIsPureMathFunction(), or those of
 *   stubIsGuardedFunction() whose arguments are valid. The variable frame (until it is assigned) is the render time.
 *   The other variables (x, u, Cs, a param...) are not known: if one of them is read (StubUnknownScalar), or if a
 *   local variable it reads is not known, the values are unknown. (An operand that SeExpr does not evaluate, e.g. x
 *   in 1 || x, does not make the value unknown.) For a ?: whose condition is not known, the values are those of both
 *   branches.
 * - an assignment gives its variable the values of its expression;
 * - after an if block, a variable has the values of both branches (the condition is not evaluated);
 * - a local variable keeps its value between the pixels: before its first assignment, it has the value 0 (the first
 *   pixel) or one of its values at the end of the expression (the previous pixels), found by evaluating the
 *   statements until these values do not change.
 * The input index and the frame of each call to cpixel() and apixel() are computed the same way, at their point of
 * the expression, whether or not the call would be evaluated for a pixel (both branches of an if or a ?: are
 * followed): the frames are a superset. A frame that is unknown gives the default frame range to its input (in
 * addition to its known frames), an input index that is unknown to all the inputs.
 * No value is a NaN marker, and nothing is sampled: the values that are not computable from the frame and constants
 * are unknown. Unlike master (which evaluated the expression with NaN for the other variables), a frame that depends
 * on another variable (e.g. frame + (x != x) * 5, or frame + max(x, 0)) gives the default range, not the frame
 * computed with NaN.
 *
 * SeExpr 2.11 does not give the variable of an assignment node (SeExprAssignNode::_name is private): it is found in
 * the text of the expression (stubAssignedVariables()), without evaluating anything. If that fails, all the local
 * variables are unknown. A variable node refers to a local variable if
 * an assignment of that variable is before it in the parse tree (the order of the prep, which binds the variables).
 **/
class StubFrameAnalysis
{
    const StubSeExpression& _expr;
    const OfxTime _time;
    FramesNeeded* _frames;
    std::set<int>* _unknownFrames;
    bool _record; // record the frames of the calls (the last evaluation of the statements)
    bool _namesKnown;
    map<const SeExprNode*, string> _assignmentNames;
    map<const SeExprNode*, std::size_t> _order; // the position of each node in the parse tree (preorder)
    map<string, std::size_t> _firstAssignmentEnd; // the position after the first assignment of each local variable
    std::set<const SeExprNode*> _analyzedCalls;

public:
    StubFrameAnalysis(const StubSeExpression& expr,
                      OfxTime time,
                      FramesNeeded* frames,
                      std::set<int>* unknownFrames)
        : _expr(expr)
        , _time(time)
        , _frames(frames)
        , _unknownFrames(unknownFrames)
        , _record(false)
        , _namesKnown(true)
        , _assignmentNames()
        , _order()
        , _firstAssignmentEnd()
        , _analyzedCalls()
    {
    }

    void run()
    {
        const vector<const SeExprFuncNode*>& calls = _expr.getPixelCalls();
        if (calls.empty()) {
            return;
        }
        if (!stubAnalysisWorks()) {
            // the analysis can not be done: all the inputs use the default frame range
            for (int input = 0; input < kSourceClipCount; ++input) {
                stubAddUnknown(input, _frames, _unknownFrames);
            }

            return;
        }
        const SeExprNode* root = calls[0];
        while (root->parent()) {
            root = root->parent();
        }
        // the cost of the analysis grows with the size of the expression times the number of local variables (each
        // if block copies and merges their values, at each evaluation of the statements), and with the sizes of the
        // subtrees (an expression whose value is not known is evaluated again operand by operand: each node is walked
        // once for each of its ancestors); its stack grows with the depth of the tree, which is measured without
        // recursion. Above kStubMaxWork, kStubMaxWalk or kStubMaxDepth, it is not done, and all the inputs use the
        // default frame range.
        std::size_t nodeCount = 0;
        std::size_t maxDepth = 0;
        double walk = 0.;
        {
            vector<std::pair<const SeExprNode*, std::size_t> > stack(1, std::make_pair(root, (std::size_t)1));
            while (!stack.empty() && (maxDepth <= kStubMaxDepth)) {
                const SeExprNode* node = stack.back().first;
                const std::size_t depth = stack.back().second;
                stack.pop_back();
                ++nodeCount;
                maxDepth = (std::max)(maxDepth, depth);
                walk += (double)depth;
                for (int i = 0; i < node->numChildren(); ++i) {
                    stack.push_back(std::make_pair(node->child(i), depth + 1));
                }
            }
        }
        const double localCount = (double)(_expr.getLocalVars().size() + 1);
        if ((maxDepth > kStubMaxDepth) || ((double)nodeCount * localCount > kStubMaxWork) || (walk * localCount > kStubMaxWalk)) {
            for (int input = 0; input < kSourceClipCount; ++input) {
                stubAddUnknown(input, _frames, _unknownFrames);
            }
            for (std::size_t i = 0; i < calls.size(); ++i) {
                _analyzedCalls.insert(calls[i]);
            }

            return;
        }
        std::size_t position = 0;
        numberNodes(root, &position);
        findAssignmentNames(root);
        // the values of the local variables before the statements: 0, or their values at the end (previous pixels)
        StubState initial;
        const SeExpression::LocalVarTable& locals = _expr.getLocalVars();
        for (SeExpression::LocalVarTable::const_iterator l = locals.begin(); l != locals.end(); ++l) {
            initial[stubVarName(l->first.c_str())] = StubValues(SeVec3d(0.));
        }
        for (int iteration = 0; iteration <= kStubMaxIterations; ++iteration) {
            _record = (iteration == kStubMaxIterations);
            StubState state = initial;
            program(root, &state);
            if (_record) {
                break;
            }
            StubState next = initial;
            bool changed = false;
            for (StubState::iterator it = next.begin(); it != next.end(); ++it) {
                StubValues v = it->second;
                v.merge(state[it->first]);
                changed = changed || !(v == it->second);
                it->second = v;
            }
            initial = next;
            if (!changed) {
                // the last evaluation, which records the frames
                _record = true;
                StubState last = initial;
                program(root, &last);
                break;
            }
            if (iteration == kStubMaxIterations - 1) {
                // the values keep changing (e.g. frame = frame + 2 at each pixel): the values found so far are used,
                // but they are not exact
                for (StubState::iterator it = initial.begin(); it != initial.end(); ++it) {
                    it->second.known = false;
                }
            }
        }
        // a call that was not found in the tree (it should not happen): its input is not known
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (_analyzedCalls.find(calls[i]) == _analyzedCalls.end()) {
                for (int input = 0; input < kSourceClipCount; ++input) {
                    stubAddUnknown(input, _frames, _unknownFrames);
                }
            }
        }
    }

private:
    void program(const SeExprNode* root,
                 StubState* state)
    {
        if (dynamic_cast<const SeExprBlockNode*>(root) && (root->numChildren() == 2)) {
            statements(root->child(0), state);
            expression(root->child(1), *state);
        } else {
            expression(root, *state);
        }
    }

    void numberNodes(const SeExprNode* node,
                     std::size_t* position)
    {
        _order[node] = (*position)++;
        for (int i = 0; i < node->numChildren(); ++i) {
            numberNodes(node->child(i), position);
        }
    }

    // the position after the subtree of node
    std::size_t endOf(const SeExprNode* node) const
    {
        while (node->numChildren() > 0) {
            node = node->child(node->numChildren() - 1);
        }

        return _order.find(node)->second + 1;
    }

    // the local variable of the expression named name (without '$'), or NULL
    SeExprLocalVarRef* localVar(const string& name) const
    {
        const SeExpression::LocalVarTable& locals = _expr.getLocalVars();
        if (locals.find(name) != locals.end()) {
            return _expr.getLocalVar(name.c_str());
        }
        if (locals.find("$" + name) != locals.end()) {
            return _expr.getLocalVar(("$" + name).c_str());
        }

        return NULL;
    }

    // does a variable node refer to a local variable (an assignment of it is before it)?
    bool isLocal(const SeExprVarNode* var) const
    {
        if (!_namesKnown) {
            return localVar(stubVarName(var->name())) != NULL;
        }
        map<string, std::size_t>::const_iterator it = _firstAssignmentEnd.find(stubVarName(var->name()));

        return (it != _firstAssignmentEnd.end()) && (_order.find(var)->second >= it->second);
    }

    // the values of an expression, or unknown (see the class description); allowInfinite: the frame of a call, whose
    // infinite values the render clamps
    StubValues evaluate(const SeExprNode* node,
                        const StubState& state,
                        bool allowInfinite = false) const
    {
        StubValues result = evaluateWhole(node, state, allowInfinite);
        if (!result.known) {
            StubValues partial = evaluatePartial(node, state);
            if (partial.known || (partial.values.size() > result.values.size())) {
                result = partial;
            }
        }
        result.cap();

        return result;
    }

    // are the components of all the values equal (an unknown value is uniform: it is not used)?
    static bool uniform(const StubValues& v)
    {
        for (std::size_t i = 0; i < v.values.size(); ++i) {
            if ((v.values[i][1] != v.values[i][0]) || (v.values[i][2] != v.values[i][0])) {
                return false;
            }
        }

        return true;
    }

    static bool allTrue(const StubValues& v)
    {
        for (std::size_t i = 0; i < v.values.size(); ++i) {
            if (!v.values[i][0]) {
                return false;
            }
        }

        return v.known;
    }

    static bool allFalse(const StubValues& v)
    {
        for (std::size_t i = 0; i < v.values.size(); ++i) {
            if (v.values[i][0]) {
                return false;
            }
        }

        return v.known;
    }

    // The values of a scalar operator from the values of its operands, when the whole subtree can not be evaluated by
    // SeExpr (evaluateWhole()), with the semantics of SeExpr 2.11 (SeExprNode.cpp): a ?: whose condition is not known
    // has the values of both branches; a || b is 1 if a or b is always true, a && b is 0 if a or b is always false
    // (whatever the other operand); a comparison or a logical operator whose operands are not known is 0 or 1;
    // printf() is 0 (its arguments are not evaluated: no output); the arithmetic and comparison operators combine the
    // values of their operands. Otherwise, the values are unknown.
    StubValues evaluatePartial(const SeExprNode* node,
                               const StubState& state) const
    {
        StubValues result = evaluateOperator(node, state);
        if (!result.known && isBoolean(node)) {
            // a comparison or a logical operator is 0 or 1, whatever its operands
            result = StubValues(SeVec3d(0.));
            result.add(SeVec3d(1.));
        }

        return result;
    }

    // is the value of a node always 0 or 1 (SeExprNode.cpp: comparisons, !, &&, ||, all scalar; SeExprBuiltins.cpp:
    // boxstep(x, a) is x < a ? 0 : 1)?
    static bool isBoolean(const SeExprNode* node)
    {
        const SeExprFuncNode* func = dynamic_cast<const SeExprFuncNode*>(node);
        if (func && (string(func->name()) == "boxstep") && !node->isVec()) {
            return true;
        }

        return (dynamic_cast<const SeExprNotNode*>(node) && !node->isVec()) || dynamic_cast<const SeExprAndNode*>(node)
               || dynamic_cast<const SeExprOrNode*>(node) || dynamic_cast<const SeExprCompareNode*>(node)
               || dynamic_cast<const SeExprCompareEqNode*>(node);
    }

    StubValues evaluateOperator(const SeExprNode* node,
                                const StubState& state) const
    {
        if (dynamic_cast<const SeExprCondNode*>(node) && (node->numChildren() == 3)) {
            const StubValues c = evaluate(node->child(0), state);
            StubValues result;
            result.known = true;
            bool any = false;
            if (!c.known || !allFalse(c)) {
                result.merge(evaluate(node->child(1), state));
                any = true;
            }
            if (!c.known || !allTrue(c)) {
                result.merge(evaluate(node->child(2), state));
                any = true;
            }
            if (!any) {
                return StubValues();
            }

            return result;
        }
        if (const SeExprFuncNode* func = dynamic_cast<const SeExprFuncNode*>(node)) {
            return evaluateFunction(func, state);
        }
        if (dynamic_cast<const SeExprSubscriptNode*>(node) && (node->numChildren() == 2)) {
            return evaluateSubscript(node, state);
        }
        const bool unary = dynamic_cast<const SeExprNegNode*>(node) || dynamic_cast<const SeExprNotNode*>(node) || dynamic_cast<const SeExprInvertNode*>(node);
        const bool logical = dynamic_cast<const SeExprAndNode*>(node) || dynamic_cast<const SeExprOrNode*>(node);
        const bool comparison = dynamic_cast<const SeExprCompareNode*>(node) || dynamic_cast<const SeExprCompareEqNode*>(node);
        const bool arithmetic = dynamic_cast<const SeExprAddNode*>(node) || dynamic_cast<const SeExprSubNode*>(node)
                                || dynamic_cast<const SeExprMulNode*>(node) || dynamic_cast<const SeExprDivNode*>(node)
                                || dynamic_cast<const SeExprModNode*>(node) || dynamic_cast<const SeExprExpNode*>(node);
        if (!(unary && (node->numChildren() == 1)) && !((logical || comparison || arithmetic) && (node->numChildren() == 2))) {
            return StubValues();
        }
        // the scalar operators (logical, comparison: their operands are scalars, except for == and != of vectors,
        // which are not combined here) use the first component; the vector ones (unary, arithmetic) are applied to each
        // component, a scalar operand being broadcast (SeExprNode.cpp)
        const StubValues a = evaluate(node->child(0), state);
        if (unary) {
            if (a.values.empty()) {
                return StubValues();
            }
            StubValues result;
            result.known = a.known;
            for (std::size_t i = 0; i < a.values.size(); ++i) {
                SeVec3d v;
                for (int k = 0; k < 3; ++k) {
                    const double x = a.values[i][k];
                    v[k] = dynamic_cast<const SeExprNegNode*>(node) ? -x : (dynamic_cast<const SeExprNotNode*>(node) ? (double)!x : 1 - x);
                }
                if (!stubIsFinite(v)) {
                    return StubValues();
                }
                result.add(v);
            }

            return result;
        }
        if ((logical || comparison) && (node->child(0)->isVec() || node->child(1)->isVec())) {
            return StubValues();
        }
        const StubValues b = evaluate(node->child(1), state);
        if (dynamic_cast<const SeExprOrNode*>(node)) {
            if (allTrue(a) || allTrue(b)) {
                return StubValues(SeVec3d(1.));
            }
        } else if (dynamic_cast<const SeExprAndNode*>(node)) {
            if (allFalse(a) || allFalse(b)) {
                return StubValues(SeVec3d(0.));
            }
        }
        if (a.values.empty() || b.values.empty() || (a.values.size() * b.values.size() > kStubMaxCombinations)) {
            return StubValues();
        }
        StubValues result;
        result.known = a.known && b.known;
        for (std::size_t i = 0; i < a.values.size(); ++i) {
            for (std::size_t k = 0; k < b.values.size(); ++k) {
                SeVec3d v;
                for (int c = 0; c < 3; ++c) {
                    // a scalar operand: its components are equal (evaluateWhole() broadcasts them)
                    const double x = a.values[i][(logical || comparison) ? 0 : c];
                    const double y = b.values[k][(logical || comparison) ? 0 : c];
                    if (dynamic_cast<const SeExprDivNode*>(node) && (y == 0.)) {
                        // a division by 0 (inf or NaN) is not computed here
                        return StubValues();
                    }
                    v[c] = binaryOperator(node, x, y);
                }
                if (!stubIsFinite(v)) {
                    // a NaN or an infinite value is not used (unknown)
                    return StubValues();
                }
                result.add(v);
            }
        }

        return result;
    }

    // the values of a function whose arguments are not all known (SeExprBuiltins.cpp semantics): printf() is 0 (not
    // evaluated: no output); pow(x, 0) is 1 and contrast(x, c <= 0) is 0.5, whatever x; getVar("frame", default) is the
    // frame variable; floor(), ceil(), round(), trunc() and abs() of a known set of values (e.g. of a ?:)
    StubValues evaluateFunction(const SeExprFuncNode* func,
                                const StubState& state) const
    {
        const string name = func->name();
        const int n = func->numChildren();
        if (name == "printf") {
            return StubValues(SeVec3d(0.));
        }
        if ((name == "pow") && (n == 2)) {
            const StubValues e = evaluate(func->child(1), state);
            if (allFalse(e) && uniform(e)) {
                return StubValues(SeVec3d(1.)); // pow(x, 0) is 1, even for NaN or inf
            }
        }
        if ((name == "contrast") && (n == 2)) {
            const StubValues c = evaluate(func->child(1), state);
            bool nonPositive = c.known && uniform(c);
            for (std::size_t i = 0; nonPositive && (i < c.values.size()); ++i) {
                nonPositive = c.values[i][0] <= 0.;
            }
            if (nonPositive) {
                return StubValues(SeVec3d(0.5)); // if (c <= 0) return .5;
            }
        }
        if ((name == "getVar") && (n == 2) && func->isStrArg(0)) {
            // getVar(name, default): the variable, or the default if there is none (all the names are variables here).
            // Its variable node is bound at the prep of the call (GetVarFunc::prep()), like a variable node at that
            // point: a local variable if it is assigned before, else the external variable.
            const string var = stubVarName(func->getStrArg(0).c_str());
            if (!_namesKnown) {
                return StubValues();
            }
            map<string, std::size_t>::const_iterator it = _firstAssignmentEnd.find(var);
            if ((it != _firstAssignmentEnd.end()) && (_order.find(func)->second >= it->second)) {
                StubState::const_iterator v = state.find(var);

                return (v != state.end()) ? v->second : StubValues(); // a local variable
            }
            if (var == kSeExprCurrentTimeVarName) {
                return StubValues(SeVec3d(_time)); // the external frame variable
            }

            return StubValues();
        }
        if (((name == "pick") || (name == "cycle")) && (n >= 3)) {
            // pick(index, lo, hi, ...) and cycle(index, lo, hi) are integers from lo to hi (or lo, if hi < lo), whatever
            // the index: all of them, if the range is small (SeExprBuiltins.cpp)
            const StubValues lo = evaluate(func->child(1), state);
            const StubValues hi = evaluate(func->child(2), state);
            if (lo.known && hi.known && (lo.values.size() == 1) && (hi.values.size() == 1) && uniform(lo) && uniform(hi)) {
                vector<SeVec3d> args(2);
                args[0] = lo.values[0];
                args[1] = hi.values[0];
                if (stubGuardedArgsAreValid("range", args)) {
                    const int l = (int)lo.values[0][0];
                    const int h = (int)hi.values[0][0];
                    if (h - l <= kStubMaxValues) {
                        StubValues result(SeVec3d((double)l));
                        for (int k = l + 1; k <= h; ++k) {
                            result.add(SeVec3d((double)k));
                        }

                        return result;
                    }
                }
            }
        }
        if (((name == "floor") || (name == "ceil") || (name == "round") || (name == "trunc") || (name == "abs")) && (n == 1)) {
            const StubValues a = evaluate(func->child(0), state);
            if (a.values.empty() || (!uniform(a) && !func->isVec())) {
                return StubValues();
            }
            StubValues result;
            result.known = a.known;
            for (std::size_t i = 0; i < a.values.size(); ++i) {
                SeVec3d v;
                for (int k = 0; k < 3; ++k) {
                    const double x = a.values[i][k];
                    v[k] = (name == "floor") ? std::floor(x) : ((name == "ceil") ? std::ceil(x) : ((name == "round") ? SeExpr::round(x) : ((name == "trunc") ? ((x < 0.) ? std::ceil(x) : std::floor(x)) : std::fabs(x))));
                }
                result.add(v);
            }

            return result;
        }

        return StubValues();
    }

    // the values of v[index] whose operands are not all known: v may be a vector literal ([frame, x, y][0]), or a set of
    // vectors (e.g. of a ?:); the index must be known (SeExprSubscriptNode::eval(): int(index), 0 out of 0..2)
    StubValues evaluateSubscript(const SeExprNode* node,
                                 const StubState& state) const
    {
        const StubValues index = evaluate(node->child(1), state);
        const SeExprNode* base = node->child(0);
        if (!index.known) {
            // an index that is not known: any component, or 0 (out of 0..2)
            StubValues result(SeVec3d(0.));
            if (dynamic_cast<const SeExprVecNode*>(base) && (base->numChildren() == 3)) {
                for (int k = 0; k < 3; ++k) {
                    result.merge(evaluate(base->child(k), state));
                }
            } else {
                const StubValues v = evaluate(base, state);
                if (v.values.empty()) {
                    return StubValues();
                }
                result.known = v.known;
                for (std::size_t j = 0; j < v.values.size(); ++j) {
                    for (int k = 0; k < (base->isVec() ? 3 : 1); ++k) {
                        result.add(SeVec3d(v.values[j][k]));
                    }
                }
            }

            return result.values.empty() ? StubValues() : result;
        }
        StubValues result;
        result.known = true;
        for (std::size_t i = 0; i < index.values.size(); ++i) {
            const double x = index.values[i][0];
            if ((x <= -2.) || (x >= 3.)) {
                result.add(SeVec3d(0.)); // out of 0..2, also for a scalar base (int(index) is defined: |x| < 3)
                continue;
            }
            const int k = (int)x;
            if ((k < 0) || (k > 2)) {
                result.add(SeVec3d(0.));
                continue;
            }
            if (!base->isVec()) {
                result.merge(evaluate(base, state));
            } else if (dynamic_cast<const SeExprVecNode*>(base) && (base->numChildren() == 3)) {
                result.merge(evaluate(base->child(k), state));
            } else {
                const StubValues v = evaluate(base, state);
                if (v.values.empty()) {
                    return StubValues();
                }
                result.known = result.known && v.known;
                for (std::size_t j = 0; j < v.values.size(); ++j) {
                    result.add(SeVec3d(v.values[j][k]));
                }
            }
            if (result.values.empty()) {
                return StubValues();
            }
        }

        return result;
    }

    // a scalar binary operator of SeExpr 2.11 (SeExprNode.cpp)
    static double binaryOperator(const SeExprNode* node,
                                 double a,
                                 double b)
    {
        if (dynamic_cast<const SeExprOrNode*>(node)) {
            return a ? 1. : (double)(b != 0.);
        }
        if (dynamic_cast<const SeExprAndNode*>(node)) {
            return !a ? 0. : (double)(b != 0.);
        }
        if (dynamic_cast<const SeExprEqNode*>(node)) {
            return (double)(a == b);
        }
        if (dynamic_cast<const SeExprNeNode*>(node)) {
            return (double)(a != b);
        }
        if (dynamic_cast<const SeExprLtNode*>(node)) {
            return (double)(a < b);
        }
        if (dynamic_cast<const SeExprGtNode*>(node)) {
            return (double)(a > b);
        }
        if (dynamic_cast<const SeExprLeNode*>(node)) {
            return (double)(a <= b);
        }
        if (dynamic_cast<const SeExprGeNode*>(node)) {
            return (double)(a >= b);
        }
        if (dynamic_cast<const SeExprAddNode*>(node)) {
            return a + b;
        }
        if (dynamic_cast<const SeExprSubNode*>(node)) {
            return a - b;
        }
        if (dynamic_cast<const SeExprMulNode*>(node)) {
            return a * b;
        }
        if (dynamic_cast<const SeExprDivNode*>(node)) {
            return a / b;
        }
        if (dynamic_cast<const SeExprModNode*>(node)) {
            return (b == 0) ? 0. : (a - std::floor(a / b) * b);
        }
        // SeExprExpNode
        if ((a < 0) && (std::floor(b) != b)) {
            a = 0.;
        }

        return std::pow(a, b);
    }

    StubValues evaluateWhole(const SeExprNode* node,
                             const StubState& state,
                             bool allowInfinite) const
    {
        vector<const SeExprVarNode*> varNodes;
        vector<const SeExprNode*> guarded;
        if (!stubIsPureExpression(node, &varNodes, &guarded)) {
            return StubValues();
        }
        // the local variables read, and their values
        vector<string> names;
        for (std::size_t i = 0; i < varNodes.size(); ++i) {
            if (isLocal(varNodes[i])) {
                const string name = stubVarName(varNodes[i]->name());
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    names.push_back(name);
                }
            }
            // an external variable: frame is the render time, the others are StubUnknownScalar
        }
        vector<StubValues> values(names.size());
        vector<SeExprLocalVarRef*> refs(names.size(), (SeExprLocalVarRef*)NULL);
        std::size_t combinations = 1;
        bool exact = true; // all the local variables read have exact values
        for (std::size_t i = 0; i < names.size(); ++i) {
            StubState::const_iterator it = state.find(names[i]);
            refs[i] = localVar(names[i]);
            if ((it == state.end()) || it->second.values.empty() || !refs[i]) {
                return StubValues();
            }
            values[i] = it->second;
            exact = exact && it->second.known;
            combinations *= values[i].values.size();
            if (combinations > kStubMaxCombinations) {
                return StubValues();
            }
        }
        StubValues result;
        result.known = exact;
        for (std::size_t c = 0; c < combinations; ++c) {
            std::size_t rest = c;
            for (std::size_t i = 0; i < names.size(); ++i) {
                refs[i]->val = values[i].values[rest % values[i].values.size()];
                rest /= values[i].values.size();
            }
            _expr.resetTouched();
            // the arguments of the guarded functions (inner calls first)
            for (std::size_t g = 0; g < guarded.size(); ++g) {
                vector<SeVec3d> args(guarded[g]->numChildren());
                for (int a = 0; a < guarded[g]->numChildren(); ++a) {
                    guarded[g]->child(a)->eval(args[a]);
                    if (!guarded[g]->child(a)->isVec()) {
                        // a scalar argument is broadcast (its other components are not set)
                        args[a][1] = args[a][2] = args[a][0];
                    }
                }
                if (_expr.touched() || !stubGuardedArgsAreValid(stubGuardedName(guarded[g]), args)) {
                    return StubValues();
                }
            }
            SeVec3d v;
            node->eval(v);
            if (_expr.touched()) {
                // it depends on a variable that is not known
                return StubValues();
            }
            if (!node->isVec()) {
                v[1] = v[2] = v[0];
            }
            // a NaN (or an infinite value, except for the frame of a call, which the render clamps) is not used: it is
            // unknown (OFX::IsNaN() and OFX::IsInfinite() test the bits of the computed value)
            for (int k = 0; k < 3; ++k) {
                if (OFX::IsNaN(v[k]) || (!allowInfinite && OFX::IsInfinite(v[k]))) {
                    return StubValues();
                }
            }
            result.add(v);
            if (result.values.size() > kStubMaxValues) {
                result.cap();

                return result;
            }
        }

        return result;
    }

    // the calls to cpixel() and apixel() in an expression, with the values of the local variables at that point
    void expression(const SeExprNode* node,
                    const StubState& state)
    {
        if (const SeExprFuncNode* func = dynamic_cast<const SeExprFuncNode*>(node)) {
            const string name = func->name();
            if (_record && ((name == kSeExprCPixelFuncName) || (name == kSeExprAPixelFuncName))) {
                call(func, state);
            }
        }
        for (int i = 0; i < node->numChildren(); ++i) {
            expression(node->child(i), state);
        }
    }

    void call(const SeExprFuncNode* node,
              const StubState& state)
    {
        _analyzedCalls.insert(node);
        if (node->numChildren() < 2) {
            return;
        }
        const StubValues indexes = evaluate(node->child(0), state);
        const StubValues frameValues = evaluate(node->child(1), state, /*allowInfinite=*/true);
        vector<int> inputs;
        bool allInputs = !indexes.known; // (all the inputs: those of the known values among them)
        for (std::size_t i = 0; !allInputs && (i < indexes.values.size()); ++i) {
            if (OFX::IsNaN(indexes.values[i][0])) {
                allInputs = true;
            } else {
                inputs.push_back(pixelInputIndex(indexes.values[i][0]));
            }
        }
        if (allInputs) {
            // the input is not known: all the inputs, with the frames of the call if they are known
            inputs.clear();
            for (int input = 0; input < kSourceClipCount; ++input) {
                inputs.push_back(input);
                stubAddUnknown(input, _frames, _unknownFrames);
            }
        }
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            (*_frames)[inputs[i]]; // the input is needed
            if (!frameValues.known) {
                // not all the frames are known: the default range, and the frames that are known
                stubAddUnknown(inputs[i], _frames, _unknownFrames);
            }
            for (std::size_t k = 0; k < frameValues.values.size(); ++k) {
                stubAddFrame(inputs[i], frameValues.values[k][0], _frames, _unknownFrames);
            }
        }
    }

    // a list of statements (assignments, if blocks)
    void statements(const SeExprNode* node,
                    StubState* state)
    {
        if (dynamic_cast<const SeExprIfThenElseNode*>(node) || dynamic_cast<const SeExprAssignNode*>(node)) {
            statement(node, state);

            return;
        }
        for (int i = 0; i < node->numChildren(); ++i) {
            statement(node->child(i), state);
        }
    }

    void statement(const SeExprNode* node,
                   StubState* state)
    {
        if (dynamic_cast<const SeExprAssignNode*>(node)) {
            if (node->numChildren() < 1) {
                return;
            }
            expression(node->child(0), *state);
            map<const SeExprNode*, string>::const_iterator it = _assignmentNames.find(node);
            if (!_namesKnown || (it == _assignmentNames.end())) {
                // the variable of the assignment is not known: all the local variables are unknown
                for (StubState::iterator l = state->begin(); l != state->end(); ++l) {
                    l->second = StubValues();
                }

                return;
            }
            (*state)[it->second] = evaluate(node->child(0), *state);
        } else if (dynamic_cast<const SeExprIfThenElseNode*>(node)) {
            if (node->numChildren() < 3) {
                return;
            }
            expression(node->child(0), *state);
            StubState thenState = *state;
            StubState elseState = *state;
            statements(node->child(1), &thenState);
            statements(node->child(2), &elseState);
            // the values of both branches
            for (StubState::iterator it = state->begin(); it != state->end(); ++it) {
                StubValues u = thenState[it->first];
                u.merge(elseState[it->first]);
                it->second = u;
            }
        } else {
            expression(node, *state);
        }
    }

    // the variable of each assignment node (see the class description): the n-th assignment of the text (a variable
    // followed by = or by an operator-assignment, +=, -=, *=, /=, %= or ^=; stubAssignedVariables()) is the n-th
    // assignment node of the parse tree, in preorder (the parser builds the statements in the order of the text). If
    // the numbers differ, the variables are not known. No right-hand side is evaluated here.
    void findAssignmentNames(const SeExprNode* root)
    {
        vector<const SeExprNode*> assignments;
        collectAssignments(root, &assignments);
        if (assignments.empty()) {
            return;
        }
        vector<string> names;
        stubAssignedVariables(_expr.getExpr(), &names);
        if (names.size() != assignments.size()) {
            _namesKnown = false;

            return;
        }
        for (std::size_t a = 0; a < assignments.size(); ++a) {
            _assignmentNames[assignments[a]] = names[a];
            if (_firstAssignmentEnd.find(names[a]) == _firstAssignmentEnd.end()) {
                // assignments are in preorder: this is the first one
                _firstAssignmentEnd[names[a]] = endOf(assignments[a]);
            }
        }
    }

    static void collectAssignments(const SeExprNode* node,
                                   vector<const SeExprNode*>* assignments)
    {
        if (dynamic_cast<const SeExprAssignNode*>(node)) {
            assignments->push_back(node);
        }
        for (int i = 0; i < node->numChildren(); ++i) {
            collectAssignments(node->child(i), assignments);
        }
    }

};

// The frames of each input that the expression fetches with cpixel() and apixel(), for the frames needed and the RoIs
// (see StubFrameAnalysis): unknownFrames are the inputs that use the default frame range, in addition to their known
// frames. The frames of each input are sorted.
// Returns false and sets error if the expression is not valid.
static bool
getStubFramesNeeded(const string& script,
                    bool wantVec,
                    OfxTime time,
                    FramesNeeded* frames,
                    std::set<int>* unknownFrames,
                    string* error)
{
    StubSeExpression expr(script, wantVec, time);
    if (!expr.isValid()) {
        *error = expr.parseError();

        return false;
    }
    StubFrameAnalysis analysis(expr, time, frames, unknownFrames);
    analysis.run();
    for (FramesNeeded::iterator it = frames->begin(); it != frames->end(); ++it) {
        std::sort(it->second.begin(), it->second.end());
    }

    return true;
}

/** override resolveVar to add external variables */
SeExprVarRef*
StubSeExpression::resolveVar(const string& varName) const
{
    if (varName == kSeExprCurrentTimeVarName) {
        return &_currentTime;
    }

    return &_unknownScalar;
}

/** override resolveFunc to add external functions */
SeExprFunc*
StubSeExpression::resolveFunc(const string& funcName) const
{
    // check if it is builtin so we get proper behavior
    if (SeExprFunc::lookup(funcName)) {
        return 0;
    }
    if ((funcName == kSeExprCPixelFuncName) || (funcName == kSeExprAPixelFuncName)) {
        return &_pixelFunction;
    }

    return 0;
}

SeExprProcessorBase::SeExprProcessorBase(SeExprPlugin* instance)
    : _renderTime(0.)
    , _renderView(0)
    , _plugin(instance)
    , _rExpr(NULL)
    , _gExpr(NULL)
    , _bExpr(NULL)
    , _rgbExpr(NULL)
    , _alphaExpr(NULL)
    , _srcCurTime()
    , _dstImg(NULL)
    , _maskInvert(false)
    , _maskImg(NULL)
    , _doMasking(false)
    , _mix(0.)
    , _images()
{
    for (int i = 0; i < kSourceClipCount; ++i) {
        _srcCurTime[i] = 0;
        _nSrcComponents[i] = 0;
    }
}

SeExprProcessorBase::~SeExprProcessorBase()
{
    delete _rExpr;
    delete _gExpr;
    delete _bExpr;
    delete _rgbExpr;
    delete _alphaExpr;
    for (FetchedImagesMap::iterator it = _images.begin(); it != _images.end(); ++it) {
        for (FetchedImagesForClipMap::iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
            delete it2->second;
        }
    }
}

void
SeExprProcessorBase::setValues(OfxTime time,
                               int view,
                               double mix,
                               const string& rgbExpr,
                               const string& alphaExpr,
                               const OfxRectI& dstPixelRod,
                               OfxPointI* inputSizes,
                               const OfxPointI& outputSize,
                               const OfxPointD& renderScale,
                               double par)
{
    setExprs(time, rgbExpr, alphaExpr, dstPixelRod, renderScale, par);
    setValuesOther(time, view, mix, inputSizes, outputSize);
}

void
SeExprProcessorBase::setValuesSimple(OfxTime time,
                                     int view,
                                     double mix,
                                     const string& rExpr,
                                     const string& gExpr,
                                     const string& bExpr,
                                     const string& aExpr,
                                     const OfxRectI& dstPixelRod,
                                     OfxPointI* inputSizes,
                                     const OfxPointI& outputSize,
                                     const OfxPointD& renderScale,
                                     double par)
{
    setExprsSimple(time, rExpr, gExpr, bExpr, aExpr, dstPixelRod, renderScale, par);
    setValuesOther(time, view, mix, inputSizes, outputSize);
}

void
SeExprProcessorBase::setExprs(OfxTime time,
                              const string& rgbExpr,
                              const string& alphaExpr,
                              const OfxRectI& dstPixelRod,
                              const OfxPointD& renderScale,
                              double par)
{
    if (!isSpaces(rgbExpr)) {
        _rgbExpr = new OFXSeExpression(this, rgbExpr, /*wantVec=*/true, /*simple=*/false, time, renderScale, par, dstPixelRod);
    }
    if (!isSpaces(alphaExpr)) {
        _alphaExpr = new OFXSeExpression(this, alphaExpr, /*wantVec=*/false, /*simple=*/false, time, renderScale, par, dstPixelRod);
    }
}

void
SeExprProcessorBase::setExprsSimple(OfxTime time,
                                    const string& rExpr,
                                    const string& gExpr,
                                    const string& bExpr,
                                    const string& aExpr,
                                    const OfxRectI& dstPixelRod,
                                    const OfxPointD& renderScale,
                                    double par)
{
    if (!isSpaces(rExpr)) {
        _rExpr = new OFXSeExpression(this, rExpr, /*wantVec=*/false, /*simple=*/true, time, renderScale, par, dstPixelRod);
    }
    if (!isSpaces(gExpr)) {
        _gExpr = new OFXSeExpression(this, gExpr, /*wantVec=*/false, /*simple=*/true, time, renderScale, par, dstPixelRod);
    }
    if (!isSpaces(bExpr)) {
        _bExpr = new OFXSeExpression(this, bExpr, /*wantVec=*/false, /*simple=*/true, time, renderScale, par, dstPixelRod);
    }
    if (!isSpaces(aExpr)) {
        _alphaExpr = new OFXSeExpression(this, aExpr, /*wantVec=*/false, /*simple=*/true, time, renderScale, par, dstPixelRod);
    }
}

void
SeExprProcessorBase::setValuesOther(OfxTime time,
                                    int view,
                                    double mix,
                                    OfxPointI* inputSizes,
                                    const OfxPointI& outputSize)
{
    _renderTime = time;
    _renderView = view;

    for (int i = 0; i < kSourceClipCount; ++i) {
        if (_rExpr) {
            _rExpr->setSize(i, inputSizes[i].x, inputSizes[i].y);
        }
        if (_gExpr) {
            _gExpr->setSize(i, inputSizes[i].x, inputSizes[i].y);
        }
        if (_bExpr) {
            _bExpr->setSize(i, inputSizes[i].x, inputSizes[i].y);
        }
        if (_rgbExpr) {
            _rgbExpr->setSize(i, inputSizes[i].x, inputSizes[i].y);
        }
        if (_alphaExpr) {
            _alphaExpr->setSize(i, inputSizes[i].x, inputSizes[i].y);
        }
    }
    if (_rExpr) {
        _rExpr->setSize(-1, outputSize.x, outputSize.y);
    }
    if (_gExpr) {
        _gExpr->setSize(-1, outputSize.x, outputSize.y);
    }
    if (_bExpr) {
        _bExpr->setSize(-1, outputSize.x, outputSize.y);
    }
    if (_rgbExpr) {
        _rgbExpr->setSize(-1, outputSize.x, outputSize.y);
    }
    if (_alphaExpr) {
        _alphaExpr->setSize(-1, outputSize.x, outputSize.y);
    }
    // assert(_alphaExpr || _rgbExpr); // both may be empty!
    _mix = mix;
}

bool
SeExprProcessorBase::isExprOk(string* error)
{
    if (_rExpr && !_rExpr->isValid()) {
        *error = _rExpr->parseError();

        return false;
    }
    if (_gExpr && !_gExpr->isValid()) {
        *error = _gExpr->parseError();

        return false;
    }
    if (_bExpr && !_bExpr->isValid()) {
        *error = _bExpr->parseError();

        return false;
    }
    if (_rgbExpr && !_rgbExpr->isValid()) {
        *error = _rgbExpr->parseError();

        return false;
    }
    if (_alphaExpr && !_alphaExpr->isValid()) {
        *error = _alphaExpr->parseError();

        return false;
    }

    // Run the expression once to initialize all the images fields before multi-threading
    if (_rExpr) {
        (void)_rExpr->evaluate();
    }
    if (_gExpr) {
        (void)_gExpr->evaluate();
    }
    if (_bExpr) {
        (void)_bExpr->evaluate();
    }
    if (_rgbExpr) {
        (void)_rgbExpr->evaluate();
    }
    if (_alphaExpr) {
        (void)_alphaExpr->evaluate();
    }

    // Ensure the image of the input 0 at the current time exists for the mix

    for (int i = 0; i < kSourceClipCount; ++i) {
        prefetchImage(i, _renderTime);
        _srcCurTime[i] = getImage(i, _renderTime);
        _nSrcComponents[i] = _srcCurTime[i] ? _srcCurTime[i]->getPixelComponentCount() : 0;
    }

    return true;
} // SeExprProcessorBase::isExprOk

// template to do the RGBA processing
template <class PIX, int nComponents, int maxValue>
class SeExprProcessor
    : public SeExprProcessorBase {
public:
    // ctor
    SeExprProcessor(SeExprPlugin* instance)
        : SeExprProcessorBase(instance)
    {
        assert(maxValue);
    }

private:
    // and do some processing
    virtual void process(OfxRectI procWindow) OVERRIDE FINAL
    {
        assert((nComponents == 4 /*&& _rgbExpr && _alphaExpr*/) || (nComponents == 3 /*&& _rgbExpr && !_alphaExpr*/) || (nComponents == 1 /*&& !_rgbExpr && _alphaExpr*/));

        float tmpPix[4];
        PIX srcPixels[kSourceClipCount][4];

        for (int y = procWindow.y1; y < procWindow.y2; ++y) {
            if (_plugin->abort()) {
                break;
            }

            PIX* dstPix = (PIX*)_dstImg->getPixelAddress(procWindow.x1, y);

            for (int x = procWindow.x1; x < procWindow.x2; ++x) {
                for (int i = kSourceClipCount - 1; i >= 0; --i) {
                    const PIX* src_pixels = _srcCurTime[i] ? (const PIX*)_srcCurTime[i]->getPixelAddress(x, y) : 0;
                    if (_nSrcComponents[i] == 4) {
                        for (int k = 0; k < 4; ++k) {
                            srcPixels[i][k] = src_pixels ? src_pixels[k] : 0;
                        }
                    } else if (_nSrcComponents[i] == 3) {
                        for (int k = 0; k < 3; ++k) {
                            srcPixels[i][k] = src_pixels ? src_pixels[k] : 0;
                        }
                        srcPixels[i][3] = src_pixels ? 1 : 0;
                    } else if (_nSrcComponents[i] == 2) {
                        for (int k = 0; k < 2; ++k) {
                            srcPixels[i][k] = src_pixels ? src_pixels[k] : 0;
                        }
                        srcPixels[i][2] = 0;
                        srcPixels[i][3] = src_pixels ? 1 : 0;
                    } else {
                        for (int k = 0; k < 3; ++k) {
                            srcPixels[i][k] = 0;
                        }
                        srcPixels[i][3] = src_pixels ? src_pixels[0] : 0;
                    }
                    float r = srcPixels[i][0] / (float)maxValue;
                    float g = srcPixels[i][1] / (float)maxValue;
                    float b = srcPixels[i][2] / (float)maxValue;
                    float a = srcPixels[i][3] / (float)maxValue;
                    if (_rExpr) {
                        _rExpr->setRGBA(i, r, g, b, a);
                    }
                    if (_gExpr) {
                        _gExpr->setRGBA(i, r, g, b, a);
                    }
                    if (_bExpr) {
                        _bExpr->setRGBA(i, r, g, b, a);
                    }
                    if (_rgbExpr) {
                        _rgbExpr->setRGBA(i, r, g, b, a);
                    }
                    if (_alphaExpr) {
                        _alphaExpr->setRGBA(i, r, g, b, a);
                    }
                }

                // initialize with values from first input (some expressions may be empty)
                if (nComponents == 1) {
                    tmpPix[0] = srcPixels[0][3];
                }
                if (nComponents >= 3) {
                    tmpPix[0] = srcPixels[0][0];
                    tmpPix[1] = srcPixels[0][1];
                    tmpPix[2] = srcPixels[0][2];
                }
                if (nComponents == 4) {
                    tmpPix[3] = srcPixels[0][3];
                }

                // execute the valid expressions
                if (_rExpr) {
                    _rExpr->setXY(x, y);
                    SeExpr2::Vec3d result = _rExpr->evaluate();
                    if (nComponents >= 3) {
                        tmpPix[0] = result[0] * maxValue;
                    }
                }
                if (_gExpr) {
                    _gExpr->setXY(x, y);
                    SeExpr2::Vec3d result = _gExpr->evaluate();
                    if (nComponents >= 3) {
                        tmpPix[1] = result[0] * maxValue;
                    }
                }
                if (_bExpr) {
                    _bExpr->setXY(x, y);
                    SeExpr2::Vec3d result = _bExpr->evaluate();
                    if (nComponents >= 3) {
                        tmpPix[2] = result[0] * maxValue;
                    }
                }
                if (_rgbExpr) {
                    _rgbExpr->setXY(x, y);
                    SeExpr2::Vec3d result = _rgbExpr->evaluate();
                    if (nComponents >= 3) {
                        tmpPix[0] = result[0] * maxValue;
                        tmpPix[1] = result[1] * maxValue;
                        tmpPix[2] = result[2] * maxValue;
                    }
                }
                if (_alphaExpr) {
                    _alphaExpr->setXY(x, y);
                    SeExpr2::Vec3d result = _alphaExpr->evaluate();
                    if (nComponents == 4) {
                        tmpPix[3] = result[0] * maxValue;
                    } else if (nComponents == 1) {
                        tmpPix[0] = result[0] * maxValue;
                    }
                }

                ofxsMaskMixPix<PIX, nComponents, maxValue, true>(tmpPix, x, y, srcPixels[0], _doMasking, _maskImg, (float)_mix, _maskInvert, dstPix);

                // increment the dst pixel
                dstPix += nComponents;
            }
        }
    } // process
};

SeExprPlugin::SeExprPlugin(OfxImageEffectHandle handle,
                           bool simple)
    : ImageEffect(handle)
    , _simple(simple)
{
    if (getContext() != eContextGenerator) {
        for (int i = 0; i < kSourceClipCount; ++i) {
            if ((i == 0) && (getContext() == eContextFilter)) {
                _srcClip[i] = fetchClip(kOfxImageEffectSimpleSourceClipName);
            } else {
                const string istr = unsignedToString(i + 1);
                _srcClip[i] = fetchClip(istr);
            }
        }
    }

    _maskClip = fetchClip(getContext() == eContextPaint ? "Brush" : "Mask");
    assert(!_maskClip || !_maskClip->isConnected() || _maskClip->getPixelComponents() == ePixelComponentAlpha);
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);

    _doubleParamCount = fetchIntParam(kParamDoubleParamNumber);
    assert(_doubleParamCount);
    _double2DParamCount = fetchIntParam(kParamDouble2DParamNumber);
    assert(_double2DParamCount);
    _colorParamCount = fetchIntParam(kParamColorNumber);
    assert(_colorParamCount);

    for (int i = 0; i < kParamsCount; ++i) {
        const string istr = unsignedToString(i + 1);

        _doubleParams[i] = fetchDoubleParam(kParamDouble + istr);
        _double2DParams[i] = fetchDouble2DParam(kParamDouble2D + istr);
        _colorParams[i] = fetchRGBParam(kParamColor + istr);
    }

    _frameRange = fetchInt2DParam(kParamFrameRange);
    _frameRangeAbsolute = fetchBooleanParam(kParamFrameRangeAbsolute);
    assert(_frameRange && _frameRangeAbsolute);

    if (_simple) {
        _rExpr = fetchStringParam(kParamRExpr);
        _gExpr = fetchStringParam(kParamGExpr);
        _bExpr = fetchStringParam(kParamBExpr);
        _aExpr = fetchStringParam(kParamAExpr);
        assert(_rExpr && _gExpr && _bExpr && _aExpr);
        _rgbScript = _alphaScript = NULL;
    } else {
        _rExpr = _gExpr = _bExpr = _aExpr = NULL;
        _rgbScript = fetchStringParam(kParamScript);
        assert(_rgbScript);
        _alphaScript = fetchStringParam(kParamAlphaScript);
        assert(_alphaScript);
    }
    _validate = fetchBooleanParam(kParamValidate);
    assert(_validate);

    _mix = fetchDoubleParam(kParamMix);
    _maskApply = paramExists(kParamMaskApply) ? fetchBooleanParam(kParamMaskApply) : 0;
    _maskInvert = fetchBooleanParam(kParamMaskInvert);
    assert(_mix && _maskInvert);

    _boundingBox = fetchChoiceParam(kParamRegionOfDefinition);
    assert(_boundingBox);

    _outputComponents = fetchChoiceParam(kParamOutputComponents);
    assert(_outputComponents);

    _format = fetchChoiceParam(kParamGeneratorFormat);
    _btmLeft = fetchDouble2DParam(kParamRectangleInteractBtmLeft);
    _size = fetchDouble2DParam(kParamRectangleInteractSize);
    _interactive = fetchBooleanParam(kParamRectangleInteractInteractive);
    assert(_format && _btmLeft && _size && _interactive);

    // update visibility
    InstanceChangedArgs args = {}; // value-initialize: the struct may gain members (e.g. isThumbnailRender)
    args.reason = eChangeUserEdit;
    args.time = 0;
    args.renderScale.x = args.renderScale.y = 1;
    changedParam(args, kParamDoubleParamNumber);
    changedParam(args, kParamDouble2DParamNumber);
    changedParam(args, kParamColorNumber);
    changedParam(args, kParamValidate);
    changedParam(args, kParamRegionOfDefinition);
    changedParam(args, kParamOutputComponents);

    // honor kParamDefaultsNormalised
    if (paramExists(kParamDefaultsNormalised)) {
        // Some hosts (e.g. Resolve) may not support normalized defaults (setDefaultCoordinateSystem(eCoordinatesNormalised))
        // handle these ourselves!
        BooleanParam* param = fetchBooleanParam(kParamDefaultsNormalised);
        assert(param);
        bool normalised = param->getValue();
        if (normalised) {
            OfxPointD size = getProjectExtent();
            OfxPointD origin = getProjectOffset();
            OfxPointD p;
            // we must denormalise all parameters for which setDefaultCoordinateSystem(eCoordinatesNormalised) couldn't be done
            // beginEditBlock(kParamDefaultsNormalised);
            p = _btmLeft->getValue();
            _btmLeft->setValue(p.x * size.x + origin.x, p.y * size.y + origin.y);
            p = _size->getValue();
            _size->setValue(p.x * size.x, p.y * size.y);
            param->setValue(false);
            // endEditBlock();
        }
    }
}

void
SeExprPlugin::setupAndProcess(SeExprProcessorBase& processor,
                              const RenderArguments& args)
{
    const double time = args.time;

    auto_ptr<Image> dst(_dstClip->fetchImage(time));
    if (!dst.get()) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    BitDepthEnum dstBitDepth = dst->getPixelDepth();
    PixelComponentEnum dstComponents = dst->getPixelComponents();
    if ((dstBitDepth != _dstClip->getPixelDepth()) || (dstComponents != _dstClip->getPixelComponents())) {
        setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }
    checkBadRenderScaleOrField(dst, args);

    string rExpr, gExpr, bExpr, aExpr;
    string rgbScript, alphaScript;
    if ((dstComponents == ePixelComponentRGB) || (dstComponents == ePixelComponentRGBA)) {
        if (_simple) {
            _rExpr->getValue(rExpr);
            _gExpr->getValue(gExpr);
            _bExpr->getValue(bExpr);
            unused(rgbScript);
        } else {
            unused(rExpr);
            unused(gExpr);
            unused(bExpr);
            _rgbScript->getValue(rgbScript);
        }
    }
    if ((dstComponents == ePixelComponentRGBA) || (dstComponents == ePixelComponentAlpha)) {
        if (_simple) {
            _aExpr->getValue(aExpr);
            unused(alphaScript);
        } else {
            unused(_aExpr);
            _alphaScript->getValue(alphaScript);
        }
    }

    double mix;
    _mix->getValue(mix);

    processor.setDstImg(dst.get());

    // auto ptr for the mask.
    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(time)) && _maskClip && _maskClip->isConnected());
    auto_ptr<const Image> mask(doMasking ? _maskClip->fetchImage(time) : 0);
    // do we do masking
    if (doMasking) {
        bool maskInvert;
        _maskInvert->getValueAtTime(time, maskInvert);

        // say we are masking
        processor.doMasking(true);

        // Set it in the processor
        processor.setMaskImg(mask.get(), maskInvert);
    }

    OfxPointI inputSizes[kSourceClipCount];
    for (int i = 0; i < kSourceClipCount; ++i) {
        if (_srcClip[i]->isConnected()) {
            OfxRectD rod = _srcClip[i]->getRegionOfDefinition(time);
            double par = _srcClip[i]->getPixelAspectRatio();
            OfxRectI pixelRod;
            Coords::toPixelEnclosing(rod, args.renderScale, par, &pixelRod);
            inputSizes[i].x = rectSize(pixelRod.x1, pixelRod.x2);
            inputSizes[i].y = rectSize(pixelRod.y1, pixelRod.y2);
        } else {
            inputSizes[i].x = inputSizes[i].y = 0.;
        }
    }

    RegionOfDefinitionArguments rodArgs;
    rodArgs.time = time;
    rodArgs.view = args.viewsToRender;
    rodArgs.renderScale = args.renderScale;
    OfxRectD outputRod;
    getRegionOfDefinition(rodArgs, outputRod);
    OfxRectI outputPixelRod;
    double par = dst->getPixelAspectRatio();

    Coords::toPixelEnclosing(outputRod, args.renderScale, par, &outputPixelRod);
    OfxPointI outputSize;
    outputSize.x = rectSize(outputPixelRod.x1, outputPixelRod.x2);
    outputSize.y = rectSize(outputPixelRod.y1, outputPixelRod.y2);

    if (_simple) {
        processor.setValuesSimple(time, args.renderView, mix, rExpr, gExpr, bExpr, aExpr, outputPixelRod, inputSizes, outputSize, args.renderScale, par);
    } else {
        processor.setValues(time, args.renderView, mix, rgbScript, alphaScript, outputPixelRod, inputSizes, outputSize, args.renderScale, par);
    }

    string error;
    if (!processor.isExprOk(&error)) {
        setPersistentMessage(Message::eMessageError, "", error);
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    processor.process(args.renderWindow);
} // SeExprPlugin::setupAndProcess

void
SeExprPlugin::render(const RenderArguments& args)
{
    clearPersistentMessage();
    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    if (!gHostIsNatron) {
        bool validated;
        _validate->getValue(validated);
        if (!validated) {
            setPersistentMessage(Message::eMessageError, "", "Validate the script before rendering/running.");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
    }

    BitDepthEnum dstBitDepth = _dstClip->getPixelDepth();
    PixelComponentEnum dstComponents = _dstClip->getPixelComponents();

    assert(dstComponents == ePixelComponentRGB || dstComponents == ePixelComponentRGBA || dstComponents == ePixelComponentAlpha);

    int outputComponents_i;
    _outputComponents->getValue(outputComponents_i);
    PixelComponentEnum outputComponents = gOutputComponentsMap[outputComponents_i];
    if (dstComponents != outputComponents) {
        setPersistentMessage(Message::eMessageError, "", "SeExpr: OFX Host did not take into account output components");
        throwSuiteStatusException(kOfxStatErrImageFormat);
    }

    if (dstComponents == ePixelComponentRGBA) {
        switch (dstBitDepth) {
        case eBitDepthUByte: {
            SeExprProcessor<unsigned char, 4, 255> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        case eBitDepthUShort: {
            SeExprProcessor<unsigned short, 4, 65535> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        case eBitDepthFloat: {
            SeExprProcessor<float, 4, 1> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        default:
            throwSuiteStatusException(kOfxStatErrUnsupported);

            return;
        }
    } else if (dstComponents == ePixelComponentRGB) {
        switch (dstBitDepth) {
        case eBitDepthUByte: {
            SeExprProcessor<unsigned char, 3, 255> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        case eBitDepthUShort: {
            SeExprProcessor<unsigned short, 3, 65535> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        case eBitDepthFloat: {
            SeExprProcessor<float, 3, 1> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        default:
            throwSuiteStatusException(kOfxStatErrUnsupported);

            return;
        }
    } else {
        assert(dstComponents == ePixelComponentAlpha);
        switch (dstBitDepth) {
        case eBitDepthUByte: {
            SeExprProcessor<unsigned char, 1, 255> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        case eBitDepthUShort: {
            SeExprProcessor<unsigned short, 1, 65535> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        case eBitDepthFloat: {
            SeExprProcessor<float, 1, 1> fred(this);
            setupAndProcess(fred, args);
            break;
        }
        default:
            throwSuiteStatusException(kOfxStatErrUnsupported);

            return;
        }
    }
} // SeExprPlugin::render

void
SeExprPlugin::changedParam(const InstanceChangedArgs& args,
                           const string& paramName)
{
    const double time = args.time;

    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    // must clear persistent message, or render() is not called by Nuke after an error
    clearPersistentMessage();

    if ((paramName == kParamDoubleParamNumber) && (args.reason == eChangeUserEdit)) {
        int numVisible;
        _doubleParamCount->getValue(numVisible);
        assert(numVisible <= kParamsCount && numVisible >= 0);
        for (int i = 0; i < kParamsCount; ++i) {
            bool visible = i < numVisible;
            _doubleParams[i]->setIsSecret(!visible);
        }
    } else if ((paramName == kParamDouble2DParamNumber) && (args.reason == eChangeUserEdit)) {
        int numVisible;
        _double2DParamCount->getValue(numVisible);
        assert(numVisible <= kParamsCount && numVisible >= 0);
        for (int i = 0; i < kParamsCount; ++i) {
            bool visible = i < numVisible;
            _double2DParams[i]->setIsSecret(!visible);
        }
    } else if ((paramName == kParamColorNumber) && (args.reason == eChangeUserEdit)) {
        int numVisible;
        _colorParamCount->getValue(numVisible);
        assert(numVisible <= kParamsCount && numVisible >= 0);
        for (int i = 0; i < kParamsCount; ++i) {
            bool visible = i < numVisible;
            _colorParams[i]->setIsSecret(!visible);
        }
    } else if ((paramName == kParamValidate) && (args.reason == eChangeUserEdit)) {
        if (!gHostIsNatron) {
            bool validated;
            _validate->getValue(validated);

            _doubleParamCount->setEnabled(!validated);
            _double2DParamCount->setEnabled(!validated);
            _colorParamCount->setEnabled(!validated);
            _doubleParamCount->setEvaluateOnChange(validated);
            _double2DParamCount->setEvaluateOnChange(validated);
            _colorParamCount->setEvaluateOnChange(validated);
            if (_simple) {
                _rExpr->setEnabled(!validated);
                _rExpr->setEvaluateOnChange(validated);
                _gExpr->setEnabled(!validated);
                _gExpr->setEvaluateOnChange(validated);
                _bExpr->setEnabled(!validated);
                _bExpr->setEvaluateOnChange(validated);
                _aExpr->setEnabled(!validated);
                _aExpr->setEvaluateOnChange(validated);
            } else {
                _rgbScript->setEnabled(!validated);
                _rgbScript->setEvaluateOnChange(validated);
                _alphaScript->setEnabled(!validated);
                _alphaScript->setEvaluateOnChange(validated);
            }
            clearPersistentMessage();
        }
    } else if ((paramName == kParamRegionOfDefinition) && (args.reason == eChangeUserEdit)) {
        int boundingBox_i;
        _boundingBox->getValue(boundingBox_i);
        RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
        bool hasFormat = (boundingBox == eRegionOfDefinitionOptionFormat);
        bool hasSize = (boundingBox == eRegionOfDefinitionOptionSize);

        _format->setIsSecretAndDisabled(!hasFormat);
        _size->setIsSecretAndDisabled(!hasSize);
        _btmLeft->setIsSecretAndDisabled(!hasSize);
        _interactive->setIsSecretAndDisabled(!hasSize);
    } else if ((paramName == kParamOutputComponents) && (args.reason == eChangeUserEdit)) {
        PixelComponentEnum outputComponents = getOutputComponents();
        const bool hasRGB = (outputComponents == ePixelComponentRGB || outputComponents == ePixelComponentRGBA); // RGB || RGBA
        if (_simple) {
            _rExpr->setIsSecretAndDisabled(!hasRGB);
            _gExpr->setIsSecretAndDisabled(!hasRGB);
            _bExpr->setIsSecretAndDisabled(!hasRGB);
        } else {
            _rgbScript->setIsSecretAndDisabled(!hasRGB);
        }
        const bool hasAlpha = (outputComponents == ePixelComponentRGBA || outputComponents == ePixelComponentAlpha); // RGBA || alpha
        if (_simple) {
            _aExpr->setIsSecretAndDisabled(!hasAlpha);
        } else {
            _alphaScript->setIsSecretAndDisabled(!hasAlpha);
        }
    } else if ((paramName == kParamShowExprs) && (args.reason == eChangeUserEdit)) {
        string rExpr, gExpr, bExpr, aExpr;
        if (_rExpr) {
            _rExpr->getValueAtTime(time, rExpr);
        }
        if (_gExpr) {
            _gExpr->getValueAtTime(time, gExpr);
        }
        if (_bExpr) {
            _bExpr->getValueAtTime(time, bExpr);
        }
        if (_aExpr) {
            _aExpr->getValueAtTime(time, aExpr);
        }
        sendMessage(Message::eMessageMessage, "", "R Expr:\n" + rExpr + "\n\nG Expr:\n" + gExpr + "\n\nB Expr:\n" + bExpr + "\n\nA Expr:\n" + aExpr);
    } else if ((paramName == kParamShowScript) && (args.reason == eChangeUserEdit)) {
        string script;
        if (_rgbScript) {
            _rgbScript->getValueAtTime(time, script);
        }
        sendMessage(Message::eMessageMessage, "", "RGB Script:\n" + script);
    } else if ((paramName == kParamShowAlphaScript) && (args.reason == eChangeUserEdit)) {
        string script;
        if (_alphaScript) {
            _alphaScript->getValueAtTime(time, script);
        }
        sendMessage(Message::eMessageMessage, "", "Alpha Script:\n" + script);
    } else if (paramName == kParamHelp) {
        sendMessage(Message::eMessageMessage, "", _simple ? kPluginDescriptionSimple : kPluginDescription);
    }
} // SeExprPlugin::changedParam

bool
SeExprPlugin::isIdentity(const IsIdentityArguments& args,
                         Clip*& identityClip,
                         double& /*identityTime*/
                         ,
                         int& /*view*/, std::string& /*plane*/)
{
    const double time = args.time;

    bool doMasking = ((!_maskApply || _maskApply->getValueAtTime(time)) && _maskClip && _maskClip->isConnected());
    if (doMasking) {
        bool maskInvert;
        _maskInvert->getValueAtTime(time, maskInvert);
        if (!maskInvert) {
            OfxRectI maskRoD;
            Coords::toPixelEnclosing(_maskClip->getRegionOfDefinition(time), args.renderScale, _maskClip->getPixelAspectRatio(), &maskRoD);
            // effect is identity if the renderWindow doesn't intersect the mask RoD
            if (!Coords::rectIntersection<OfxRectI>(args.renderWindow, maskRoD, 0)) {
                identityClip = _srcClip[0];

                return true;
            }
        }
    }

    // check if all expressions are empty
    PixelComponentEnum outputComponents = getOutputComponents();
    string script;
    if ((outputComponents == ePixelComponentRGB) || (outputComponents == ePixelComponentRGBA)) { // RGB || RGBA
        if (_simple) {
            assert(_rExpr && _gExpr && _bExpr);
            _rExpr->getValueAtTime(time, script);
            if (!isSpaces(script)) {
                return false;
            }
            _gExpr->getValueAtTime(time, script);
            if (!isSpaces(script)) {
                return false;
            }
            _bExpr->getValueAtTime(time, script);
            if (!isSpaces(script)) {
                return false;
            }
        } else {
            assert(_rgbScript);
            _rgbScript->getValueAtTime(time, script);
            if (!isSpaces(script)) {
                return false;
            }
        }
    }
    if ((outputComponents == ePixelComponentRGBA) || (outputComponents == ePixelComponentAlpha)) { // RGBA || alpha
        if (_simple) {
            assert(_aExpr);
            _aExpr->getValueAtTime(time, script);
        } else {
            assert(_alphaScript);
            _alphaScript->getValueAtTime(time, script);
        }
        if (!isSpaces(script)) {
            return false;
        }
    }

    identityClip = _srcClip[0];

    return true;
} // SeExprPlugin::isIdentity

void
SeExprPlugin::getClipPreferences(ClipPreferencesSetter& clipPreferences)
{

    double par = 0.;
    int boundingBox_i;
    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    switch (boundingBox) {
    case eRegionOfDefinitionOptionSize: {
        // size
        break;
    }
    case eRegionOfDefinitionOptionFormat: {
        // format
        int index;
        _format->getValue(index);
        int w, h;
        getFormatResolution((EParamFormat)index, &w, &h, &par);
        break;
    }
    case eRegionOfDefinitionOptionProject: {
        // project format

        /// this should be the defalut value given by the host, no need to set it.
        /// @see Instance::setupClipPreferencesArgs() in HostSupport, it should have
        /// the line:
        /// double inputPar = getProjectPixelAspectRatio();

        // par = getProjectPixelAspectRatio();
        break;
    }
    default:
        break;
    }

    if (par != 0.) {
        clipPreferences.setPixelAspectRatio(*_dstClip, par);
    }

    // We're frame varying since we don't know what the user may output at any frame
    clipPreferences.setOutputFrameVarying(true);
    clipPreferences.setOutputHasContinuousSamples(true);

    PixelComponentEnum outputComponents = getOutputComponents();
    if (outputComponents == ePixelComponentRGB) {
        clipPreferences.setOutputPremultiplication(eImageOpaque);
    }
    clipPreferences.setClipComponents(*_dstClip, outputComponents);
} // SeExprPlugin::getClipPreferences

void
SeExprPlugin::getFramesNeeded(const FramesNeededArguments& args,
                              FramesNeededSetter& framesNeededSetter)
{
    if (!gHostIsNatron) {
        bool validated;
        _validate->getValue(validated);
        if (!validated) {
            setPersistentMessage(Message::eMessageError, "", "Validate the script before rendering/running.");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
    }

    // The frames needed of the expression are found by getStubFramesNeeded() (StubFrameAnalysis): every call to
    // cpixel() and apixel() is analyzed, also in a branch that is not taken (e.g. if (x > 0) { srcCol = cpixel(1,
    // frame + 1, 5, 5); }), with the values of the local variables computed from the frame and constants. The inputs
    // whose frames are not known (a frame that depends on another variable or on a pixel value) get the input frame
    // range param, in addition to their known frames.
    const double time = args.time;
    FramesNeeded framesNeeded;
    std::set<int> unknownFrames; // the inputs whose frames depend on unknown values (e.g. pixel values)
    PixelComponentEnum outputComponents = getOutputComponents();
    if ((outputComponents == ePixelComponentRGB) || (outputComponents == ePixelComponentRGBA)) { // RGB || RGBA
        for (int e = 0; e < (_simple ? 3 : 1); ++e) {
            StringParam* param = !_simple ? _rgbScript : (e == 0 ? _rExpr : (e == 1 ? _gExpr : _bExpr));
            string script;
            if (param) {
                param->getValue(script);
            }

            if (isSpaces(script)) {
                framesNeeded[0].push_back(time);
            } else {
                FramesNeeded rgbNeeded;
                string error;
                if (!getStubFramesNeeded(script, /*wantVec=*/!_simple, time, &rgbNeeded, &unknownFrames, &error)) {
                    setPersistentMessage(Message::eMessageError, "", error);
                    throwSuiteStatusException(kOfxStatFailed);

                    return;
                }

                for (FramesNeeded::const_iterator it = rgbNeeded.begin(); it != rgbNeeded.end(); ++it) {
                    vector<OfxTime>& frames = framesNeeded[it->first];
                    for (std::size_t j = 0; j < it->second.size(); ++j) {
                        bool found = false;
                        for (std::size_t i = 0; i < frames.size(); ++i) {
                            if (frames[i] == it->second[j]) {
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            frames.push_back(it->second[j]);
                        }
                    }
                }
            }
        }
    }
    if ((outputComponents == ePixelComponentRGBA) || (outputComponents == ePixelComponentAlpha)) { // RGBA || alpha
        StringParam* param = !_simple ? _alphaScript : _aExpr;
        string script;
        if (param) {
            param->getValue(script);
        }

        if (isSpaces(script)) {
            framesNeeded[0].push_back(time);
        } else {
            FramesNeeded alphaNeeded;
            string error;
            if (!getStubFramesNeeded(script, /*wantVec=*/false, time, &alphaNeeded, &unknownFrames, &error)) {
                setPersistentMessage(Message::eMessageError, "", error);
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }

            for (FramesNeeded::const_iterator it = alphaNeeded.begin(); it != alphaNeeded.end(); ++it) {
                vector<OfxTime>& frames = framesNeeded[it->first];
                for (std::size_t j = 0; j < it->second.size(); ++j) {
                    bool found = false;
                    for (std::size_t i = 0; i < frames.size(); ++i) {
                        if (frames[i] == it->second[j]) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        frames.push_back(it->second[j]);
                    }
                }
            }
        }
    }

    bool useDefaultRange[kSourceClipCount];
    std::fill(useDefaultRange, useDefaultRange + kSourceClipCount, false);
    for (std::set<int>::const_iterator it = unknownFrames.begin(); it != unknownFrames.end(); ++it) {
        assert(*it >= 0 && *it < kSourceClipCount);
        // The user probably used something dependant on a pixel value as a time for the getPixel function:
        // we fall back on using the default frame range
        useDefaultRange[*it] = true;
    }

    // the known frames of each input, also for the inputs that use the default frame range (some of their calls have
    // a frame that is not known): the frames are a superset
    for (FramesNeeded::const_iterator it = framesNeeded.begin(); it != framesNeeded.end(); ++it) {
        Clip* clip = getClip(it->first);
        assert(clip);

        bool hasFetchedCurrentTime = false;
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            OfxRangeD range;
            if (it->second[i] == time) {
                hasFetchedCurrentTime = true;
            }
            range.min = range.max = it->second[i];
            framesNeededSetter.setFramesNeeded(*clip, range);
        }
        if (!hasFetchedCurrentTime) {
            OfxRangeD range;
            range.min = range.max = time;
            framesNeededSetter.setFramesNeeded(*clip, range);
        }
    }

    // for clips that could not have their range computed from the expression,
    // use the default range
    OfxRangeD range;
    int t1, t2;
    _frameRange->getValueAtTime(time, t1, t2);
    bool absolute;
    _frameRangeAbsolute->getValueAtTime(time, absolute);
    if (absolute) {
        range.min = (std::min)(t1, t2);
        range.max = (std::max)(t1, t2);
    } else {
        range.min = time + (std::min)(t1, t2);
        range.max = time + (std::max)(t1, t2);
    }

    for (int i = 0; i < kSourceClipCount; ++i) {
        if (useDefaultRange[i]) {
            Clip* clip = getClip(i);
            assert(clip);
            framesNeededSetter.setFramesNeeded(*clip, range);
        }
    }
} // SeExprPlugin::getFramesNeeded

// override the roi call
void
SeExprPlugin::getRegionsOfInterest(const RegionsOfInterestArguments& args,
                                   RegionOfInterestSetter& rois)
{
    const double time = args.time;

    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return;
    }

    if (!gHostIsNatron) {
        bool validated;
        _validate->getValue(validated);
        if (!validated) {
            setPersistentMessage(Message::eMessageError, "", "Validate the script before rendering/running.");
            throwSuiteStatusException(kOfxStatFailed);

            return;
        }
    }

    if (!kSupportsTiles) {
        // The effect requires full images to render any region
        for (int i = 0; i < kSourceClipCount; ++i) {
            OfxRectD srcRoI;

            if (_srcClip[i] && _srcClip[i]->isConnected()) {
                srcRoI = _srcClip[i]->getRegionOfDefinition(time);
                rois.setRegionOfInterest(*_srcClip[i], srcRoI);
            }
        }
    } else {
        // Notify that we will need the RoI for all connected input clips at the current time
        for (int i = 0; i < kSourceClipCount; ++i) {
            Clip* clip = getClip(i);
            assert(clip);
            if (clip->isConnected()) {
                rois.setRegionOfInterest(*clip, args.regionOfInterest);
            }
        }

        // To determine the ROIs of the expression, we just execute the expression at the 4 corners of the render window
        // and record what are the calls made to getPixel in order to figure out the Roi.

        std::set<Clip*> processedClips;

        PixelComponentEnum outputComponents = getOutputComponents();

        for (int e = 0; e < 6; ++e) {
            string script;
            bool wantVec = false;
            switch (e) {
            case 0: // rExpr
                if (_rExpr && _simple && ((outputComponents == ePixelComponentRGB) || (outputComponents == ePixelComponentRGBA))) {
                    _rExpr->getValue(script);
                }
                break;

            case 1: // gExpr
                if (_gExpr && _simple && ((outputComponents == ePixelComponentRGB) || (outputComponents == ePixelComponentRGBA))) {
                    _gExpr->getValue(script);
                }
                break;

            case 2: // bExpr
                if (_bExpr && _simple && ((outputComponents == ePixelComponentRGB) || (outputComponents == ePixelComponentRGBA))) {
                    _bExpr->getValue(script);
                }
                break;

            case 3: // aExpr
                if (_aExpr && _simple && ((outputComponents == ePixelComponentRGBA) || (outputComponents == ePixelComponentAlpha))) {
                    _aExpr->getValue(script);
                }
                break;

            case 4: // rgbScript
                if (_rgbScript && !_simple && ((outputComponents == ePixelComponentRGB) || (outputComponents == ePixelComponentRGBA))) {
                    _rgbScript->getValue(script);
                    wantVec = true;
                }
                break;

            case 5: // alphaScript
                if (_alphaScript && !_simple && ((outputComponents == ePixelComponentRGBA) || (outputComponents == ePixelComponentAlpha))) {
                    _alphaScript->getValue(script);
                }
                break;
            }
            if (isSpaces(script)) {
                continue;
            }

            // Now evaluate the expression and determine whether the user will call getPixel.
            // If he/she does, then we have no choice but to ask for the entire input image because we do not know
            // what the user may need (typically when applying UVMaps and stuff)
            FramesNeeded framesNeeded;
            std::set<int> unknownFrames;
            string error;
            if (!getStubFramesNeeded(script, wantVec, time, &framesNeeded, &unknownFrames, &error)) {
                setPersistentMessage(Message::eMessageError, "", error);
                throwSuiteStatusException(kOfxStatFailed);

                return;
            }
            std::set<int> inputs(unknownFrames);
            for (FramesNeeded::const_iterator it = framesNeeded.begin(); it != framesNeeded.end(); ++it) {
                inputs.insert(it->first);
            }

            for (std::set<int>::const_iterator it = inputs.begin(); it != inputs.end(); ++it) {
                Clip* clip = getClip(*it);
                assert(clip);
                pair<std::set<Clip*>::iterator, bool> ret = processedClips.insert(clip);
                if (ret.second) {
                    if (clip->isConnected()) {
                        rois.setRegionOfInterest(*clip, clip->getRegionOfDefinition(time));
                    }
                }
            }
        }
    }
} // SeExprPlugin::getRegionsOfInterest

bool
SeExprPlugin::getRegionOfDefinition(const RegionOfDefinitionArguments& args,
                                    OfxRectD& rod)
{
    const double time = args.time;

    if (!kSupportsRenderScale && ((args.renderScale.x != 1.) || (args.renderScale.y != 1.))) {
        throwSuiteStatusException(kOfxStatFailed);

        return false;
    }

    bool rodSet = false;
    int boundingBox_i;
    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    switch (boundingBox) {
    case eRegionOfDefinitionOptionUnion: {
        // union of inputs
        for (int i = 0; i < kSourceClipCount; ++i) {
            if (_srcClip[i]->isConnected()) {
                OfxRectD srcRod = _srcClip[i]->getRegionOfDefinition(time);
                if (rodSet) {
                    Coords::rectBoundingBox(srcRod, rod, &rod);
                } else {
                    rod = srcRod;
                    rodSet = true;
                }
            }
        }
        break;
    }
    case eRegionOfDefinitionOptionIntersection: {
        // intersection of inputs
        bool rodSet = false;
        for (int i = 0; i < kSourceClipCount; ++i) {
            if (_srcClip[i]->isConnected()) {
                OfxRectD srcRod = _srcClip[i]->getRegionOfDefinition(time);
                if (rodSet) {
                    Coords::rectIntersection<OfxRectD>(srcRod, rod, &rod);
                } else {
                    rod = srcRod;
                }
                rodSet = true;
            }
        }
        break;
    }
    case eRegionOfDefinitionOptionSize: {
        // custom size
        _size->getValue(rod.x2, rod.y2);
        _btmLeft->getValue(rod.x1, rod.y1);
        rod.x2 += rod.x1;
        rod.y2 += rod.y1;

        rodSet = true;
        break;
    }
    case eRegionOfDefinitionOptionFormat: {
        // format

        int format_i;
        _format->getValue(format_i);
        double par = -1;
        int w = 0, h = 0;
        getFormatResolution((EParamFormat)format_i, &w, &h, &par);
        assert(par != -1);
        rod.x1 = rod.y1 = 0;
        rod.x2 = w;
        rod.y2 = h;

        rodSet = true;
        break;
    }
    case eRegionOfDefinitionOptionProject: {
        // project
        OfxPointD size = getProjectSize();
        OfxPointD offset = getProjectOffset();
        rod.x1 = offset.x;
        rod.y1 = offset.y;
        rod.x2 = offset.x + size.x;
        rod.y2 = offset.y + size.y;
        rodSet = true;
        break;
    }
    default: {
        int inputIndex = boundingBox_i - (int)eRegionOfDefinitionOptionCustom;
        assert(inputIndex >= 0 && inputIndex < kSourceClipCount);
        rod = _srcClip[inputIndex]->getRegionOfDefinition(time);
        rodSet = true;
        break;
    }
    } // switch
    if (!rodSet) {
        OfxPointD size = getProjectSize();
        OfxPointD offset = getProjectOffset();
        rod.x1 = offset.x;
        rod.y1 = offset.y;
        rod.x2 = offset.x + size.x;
        rod.y2 = offset.y + size.y;
    }

    return true;
} // SeExprPlugin::getRegionOfDefinition

class SeExprInteract
    : public RectangleInteract {
public:
    SeExprInteract(OfxInteractHandle handle,
                   ImageEffect* effect);

    virtual bool draw(const DrawArgs& args) OVERRIDE FINAL;
    virtual bool penMotion(const PenArgs& args) OVERRIDE FINAL;
    virtual bool penDown(const PenArgs& args) OVERRIDE FINAL;
    virtual bool penUp(const PenArgs& args) OVERRIDE FINAL;
    virtual bool keyDown(const KeyArgs& args) OVERRIDE FINAL;
    virtual bool keyUp(const KeyArgs& args) OVERRIDE FINAL;
    virtual void loseFocus(const FocusArgs& args) OVERRIDE FINAL;

private:
    virtual void aboutToCheckInteractivity(OfxTime time) OVERRIDE FINAL;
    virtual bool allowTopLeftInteraction() const OVERRIDE FINAL;
    virtual bool allowBtmRightInteraction() const OVERRIDE FINAL;
    virtual bool allowBtmLeftInteraction() const OVERRIDE FINAL;
    virtual bool allowBtmMidInteraction() const OVERRIDE FINAL;
    virtual bool allowMidLeftInteraction() const OVERRIDE FINAL;
    virtual bool allowCenterInteraction() const OVERRIDE FINAL;
    ChoiceParam* _boundingBox;
    RegionOfDefinitionEnum _bboxType;
};

SeExprInteract::SeExprInteract(OfxInteractHandle handle,
                               ImageEffect* effect)
    : RectangleInteract(handle, effect)
    , _boundingBox(0)
    , _bboxType(eRegionOfDefinitionOptionUnion)
{
    _boundingBox = effect->fetchChoiceParam(kParamRegionOfDefinition);
    assert(_boundingBox);
}

void
SeExprInteract::aboutToCheckInteractivity(OfxTime /*time*/)
{
    int type_i;

    _boundingBox->getValue(type_i);
    _bboxType = (RegionOfDefinitionEnum)type_i;
}

bool
SeExprInteract::allowTopLeftInteraction() const
{
    return _bboxType == eRegionOfDefinitionOptionSize;
}

bool
SeExprInteract::allowBtmRightInteraction() const
{
    return _bboxType == eRegionOfDefinitionOptionSize;
}

bool
SeExprInteract::allowBtmLeftInteraction() const
{
    return _bboxType == eRegionOfDefinitionOptionSize;
}

bool
SeExprInteract::allowBtmMidInteraction() const
{
    return _bboxType == eRegionOfDefinitionOptionSize;
}

bool
SeExprInteract::allowMidLeftInteraction() const
{
    return _bboxType == eRegionOfDefinitionOptionSize;
}

bool
SeExprInteract::allowCenterInteraction() const
{
    return _bboxType == eRegionOfDefinitionOptionSize;
}

bool
SeExprInteract::draw(const DrawArgs& args)
{
    int boundingBox_i;

    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    if (boundingBox != eRegionOfDefinitionOptionSize) {
        return false;
    }

    return RectangleInteract::draw(args);
}

bool
SeExprInteract::penMotion(const PenArgs& args)
{
    int boundingBox_i;

    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    if (boundingBox != eRegionOfDefinitionOptionSize) {
        return false;
    }

    return RectangleInteract::penMotion(args);
}

bool
SeExprInteract::penDown(const PenArgs& args)
{
    int boundingBox_i;

    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    if (boundingBox != eRegionOfDefinitionOptionSize) {
        return false;
    }

    return RectangleInteract::penDown(args);
}

bool
SeExprInteract::penUp(const PenArgs& args)
{
    int boundingBox_i;

    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    if (boundingBox != eRegionOfDefinitionOptionSize) {
        // the bounding box changed during the drag (if any): abandon it, as GeneratorInteract does
        abandonDrag();

        return false;
    }

    return RectangleInteract::penUp(args);
}

void
SeExprInteract::loseFocus(const FocusArgs& args)
{
    return RectangleInteract::loseFocus(args);
}

bool
SeExprInteract::keyDown(const KeyArgs& args)
{
    int boundingBox_i;

    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    if (boundingBox != eRegionOfDefinitionOptionSize) {
        return false;
    }

    return RectangleInteract::keyDown(args);
}

bool
SeExprInteract::keyUp(const KeyArgs& args)
{
    int boundingBox_i;

    _boundingBox->getValue(boundingBox_i);
    RegionOfDefinitionEnum boundingBox = (RegionOfDefinitionEnum)boundingBox_i;
    if (boundingBox != eRegionOfDefinitionOptionSize) {
        return false;
    }

    return RectangleInteract::keyUp(args);
}

// Draws with the DrawSuite when the host provides it, as RectangleOverlayDescriptorV2.
class SeExprOverlayDescriptorV2
    : public DefaultEffectOverlayDescriptorV2<SeExprOverlayDescriptorV2, SeExprInteract> {
};

// mDeclarePluginFactory(SeExprPluginFactory, {ofxsThreadSuiteCheck();}, {});

template <bool simple>
class SeExprPluginFactory
    : public PluginFactoryHelper<SeExprPluginFactory<simple>> {
public:
    SeExprPluginFactory(const string& id,
                        unsigned int verMaj,
                        unsigned int verMin)
        : PluginFactoryHelper<SeExprPluginFactory>(id, verMaj, verMin)
    {
    }

    virtual void load() OVERRIDE FINAL { ofxsThreadSuiteCheck(); }
    virtual void describe(ImageEffectDescriptor& desc) OVERRIDE FINAL;
    virtual void describeInContext(ImageEffectDescriptor& desc, ContextEnum context) OVERRIDE FINAL;
    virtual ImageEffect* createInstance(OfxImageEffectHandle handle, ContextEnum context) OVERRIDE FINAL;
};

template <bool simple>
void
SeExprPluginFactory<simple>::describe(ImageEffectDescriptor& desc)
{
    // basic labels
    desc.setLabel(simple ? kPluginNameSimple : kPluginName);
    desc.setPluginGrouping(kPluginGrouping);
    if (desc.getPropertySet().propGetDimension(kNatronOfxPropDescriptionIsMarkdown, false)) {
        desc.setPluginDescription(simple ? kPluginDescriptionSimple /*Markdown*/ : kPluginDescription /*Markdown*/, false);
        desc.getPropertySet().propSetInt(kNatronOfxPropDescriptionIsMarkdown, 1);
    } else {
        desc.setPluginDescription(simple ? kPluginDescriptionSimple : kPluginDescription);
    }

    // add the supported contexts
    desc.addSupportedContext(eContextFilter);
    desc.addSupportedContext(eContextGeneral);
    desc.addSupportedContext(eContextGenerator);

    // add supported pixel depths
    desc.addSupportedBitDepth(eBitDepthUByte);
    desc.addSupportedBitDepth(eBitDepthUShort);
    desc.addSupportedBitDepth(eBitDepthHalf);
    desc.addSupportedBitDepth(eBitDepthFloat);
    // desc.addSupportedBitDepth(eBitDepthCustom);

    // set a few flags
    desc.setSingleInstance(false);
    desc.setHostFrameThreading(true);
    desc.setSupportsTiles(kSupportsTiles);
    desc.setSupportsMultiResolution(kSupportsMultiResolution);
    desc.setRenderThreadSafety(kRenderThreadSafety);
    desc.setTemporalClipAccess(true);
    desc.setRenderTwiceAlways(false);
    desc.setSupportsMultipleClipPARs(false);

    ofxsSetOverlayInteractDescriptorV2(desc, new SeExprOverlayDescriptorV2);
} // >::describe

template <bool simple>
void
SeExprPluginFactory<simple>::describeInContext(ImageEffectDescriptor& desc,
                                               ContextEnum context)
{
    const ImageEffectHostDescription& gHostDescription = *getImageEffectHostDescription();

    gHostIsNatron = gHostDescription.isNatron;
    bool hostIsNuke = (gHostDescription.hostName.find("nuke") != string::npos || gHostDescription.hostName.find("Nuke") != string::npos);

    for (ImageEffectHostDescription::PixelComponentArray::const_iterator it = getImageEffectHostDescription()->_supportedComponents.begin();
         it != getImageEffectHostDescription()->_supportedComponents.end();
         ++it) {
        switch (*it) {
        case ePixelComponentRGBA:
            gHostSupportsRGBA = true;
            break;
        case ePixelComponentRGB:
            gHostSupportsRGB = true;
            break;
        case ePixelComponentAlpha:
            gHostSupportsAlpha = true;
            break;
        default:
            // other components are not supported by this plugin
            break;
        }
    }

    // Source clip only in the filter context
    // create the mandated source clip
    for (int i = 0; i < kSourceClipCount; ++i) {
        ClipDescriptor* srcClip;
        if ((i == 0) && (context == eContextFilter)) {
            srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName); // mandatory clip for the filter context
        } else {
            const string istr = unsignedToString(i + 1);
            srcClip = desc.defineClip(istr);
        }
        if (gHostSupportsRGBA) {
            srcClip->addSupportedComponent(ePixelComponentRGBA);
        }
        if (gHostSupportsRGB) {
            srcClip->addSupportedComponent(ePixelComponentRGB);
        }
        if (gHostSupportsAlpha) {
            srcClip->addSupportedComponent(ePixelComponentAlpha);
        }
        // srcClip->addSupportedComponent(ePixelComponentCustom);
        srcClip->setTemporalClipAccess(true);
        srcClip->setSupportsTiles(true);
        srcClip->setIsMask(false);
        srcClip->setOptional(true);
    }

    ClipDescriptor* maskClip = (context == eContextPaint) ? desc.defineClip("Brush") : desc.defineClip("Mask");
    maskClip->addSupportedComponent(ePixelComponentAlpha);
    maskClip->setTemporalClipAccess(false);
    if (context != eContextPaint) {
        maskClip->setOptional(true);
    }
    maskClip->setSupportsTiles(kSupportsTiles);
    maskClip->setIsMask(true);

    // create the mandated output clip
    ClipDescriptor* dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
    if (gHostSupportsRGBA) {
        dstClip->addSupportedComponent(ePixelComponentRGBA);
    }
    if (gHostSupportsRGB) {
        dstClip->addSupportedComponent(ePixelComponentRGB);
    }
    if (gHostSupportsAlpha) {
        dstClip->addSupportedComponent(ePixelComponentAlpha);
    }
    // dstClip->addSupportedComponent(ePixelComponentCustom);
    dstClip->setSupportsTiles(true);

    // make some pages and to things in
    PageParamDescriptor* page = desc.definePageParam("Controls");

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamRegionOfDefinition);
        param->setLabel(kParamRegionOfDefinitionLabel);
        param->setHint(kParamRegionOfDefinitionHint);
        param->setLayoutHint(eLayoutHintNoNewLine, 1);

        assert(param->getNOptions() == eRegionOfDefinitionOptionUnion);
        param->appendOption(kParamRegionOfDefinitionOptionUnion);
        assert(param->getNOptions() == eRegionOfDefinitionOptionIntersection);
        param->appendOption(kParamRegionOfDefinitionOptionIntersection);
        assert(param->getNOptions() == eRegionOfDefinitionOptionSize);
        param->appendOption(kParamRegionOfDefinitionOptionSize);
        assert(param->getNOptions() == eRegionOfDefinitionOptionFormat);
        param->appendOption(kParamRegionOfDefinitionOptionFormat);
        assert(param->getNOptions() == eRegionOfDefinitionOptionProject);
        param->appendOption(kParamRegionOfDefinitionOptionProject);

        assert(param->getNOptions() == eRegionOfDefinitionOptionCustom);
        for (int i = 0; i < kSourceClipCount; ++i) {
            const string istr = unsignedToString(i + 1);
            param->appendOption(kParamRegionOfDefinitionOptionCustomInput + istr, kParamRegionOfDefinitionOptionCustomInputHelp + istr, kParamRegionOfDefinitionOptionCustomInputEnum + istr);
        }
        param->setAnimates(false);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamOutputComponents);
        param->setLabel(kParamOutputComponentsLabel);
        param->setHint(kParamOutputComponentsHint);
        int i = 0;

        if (gHostSupportsRGBA) {
            gOutputComponentsMap[i] = ePixelComponentRGBA;
            ++i;
            // coverity[check_return]
            assert(param->getNOptions() >= 0 && gOutputComponentsMap[param->getNOptions()] == ePixelComponentRGBA);
            param->appendOption(kParamOutputComponentsOptionRGBA);
        }
        if (gHostSupportsRGB) {
            gOutputComponentsMap[i] = ePixelComponentRGB;
            ++i;
            // coverity[check_return]
            assert(param->getNOptions() >= 0 && gOutputComponentsMap[param->getNOptions()] == ePixelComponentRGB);
            param->appendOption(kParamOutputComponentsOptionRGB);
        }
        if (gHostSupportsAlpha) {
            gOutputComponentsMap[i] = ePixelComponentAlpha;
            ++i;
            // coverity[check_return]
            assert(param->getNOptions() >= 0 && gOutputComponentsMap[param->getNOptions()] == ePixelComponentAlpha);
            param->appendOption(kParamOutputComponentsOptionAlpha);
        }
        gOutputComponentsMap[i] = ePixelComponentNone;

        param->setDefault(0); // default to the first one available, i.e. the most chromatic
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        ChoiceParamDescriptor* param = desc.defineChoiceParam(kParamGeneratorFormat);
        param->setLabel(kParamGeneratorFormatLabel);
        assert(param->getNOptions() == eParamFormatPCVideo);
        assert(param->getNOptions() == eParamFormatPCVideo);
        param->appendOption(kParamFormatPCVideoLabel, "", kParamFormatPCVideo);
        assert(param->getNOptions() == eParamFormatNTSC);
        param->appendOption(kParamFormatNTSCLabel, "", kParamFormatNTSC);
        assert(param->getNOptions() == eParamFormatPAL);
        param->appendOption(kParamFormatPALLabel, "", kParamFormatPAL);
        assert(param->getNOptions() == eParamFormatNTSC169);
        param->appendOption(kParamFormatNTSC169Label, "", kParamFormatNTSC169);
        assert(param->getNOptions() == eParamFormatPAL169);
        param->appendOption(kParamFormatPAL169Label, "", kParamFormatPAL169);
        assert(param->getNOptions() == eParamFormatHD720);
        param->appendOption(kParamFormatHD720Label, "", kParamFormatHD720);
        assert(param->getNOptions() == eParamFormatHD);
        param->appendOption(kParamFormatHDLabel, "", kParamFormatHD);
        assert(param->getNOptions() == eParamFormatUHD4K);
        param->appendOption(kParamFormatUHD4KLabel, "", kParamFormatUHD4K);
        assert(param->getNOptions() == eParamFormat1kSuper35);
        param->appendOption(kParamFormat1kSuper35Label, "", kParamFormat1kSuper35);
        assert(param->getNOptions() == eParamFormat1kCinemascope);
        param->appendOption(kParamFormat1kCinemascopeLabel, "", kParamFormat1kCinemascope);
        assert(param->getNOptions() == eParamFormat2kSuper35);
        param->appendOption(kParamFormat2kSuper35Label, "", kParamFormat2kSuper35);
        assert(param->getNOptions() == eParamFormat2kCinemascope);
        param->appendOption(kParamFormat2kCinemascopeLabel, "", kParamFormat2kCinemascope);
        assert(param->getNOptions() == eParamFormat2kDCP);
        param->appendOption(kParamFormat2kDCPLabel, "", kParamFormat2kDCP);
        assert(param->getNOptions() == eParamFormat4kSuper35);
        param->appendOption(kParamFormat4kSuper35Label, "", kParamFormat4kSuper35);
        assert(param->getNOptions() == eParamFormat4kCinemascope);
        param->appendOption(kParamFormat4kCinemascopeLabel, "", kParamFormat4kCinemascope);
        assert(param->getNOptions() == eParamFormat4kDCP);
        param->appendOption(kParamFormat4kDCPLabel, "", kParamFormat4kDCP);
        assert(param->getNOptions() == eParamFormatSquare256);
        param->appendOption(kParamFormatSquare256Label, "", kParamFormatSquare256);
        assert(param->getNOptions() == eParamFormatSquare512);
        param->appendOption(kParamFormatSquare512Label, "", kParamFormatSquare512);
        assert(param->getNOptions() == eParamFormatSquare1k);
        param->appendOption(kParamFormatSquare1kLabel, "", kParamFormatSquare1k);
        assert(param->getNOptions() == eParamFormatSquare2k);
        param->appendOption(kParamFormatSquare2kLabel, "", kParamFormatSquare2k);
        param->setDefault(0);
        param->setHint(kParamGeneratorFormatHint);
        param->setAnimates(false);
        desc.addClipPreferencesSlaveParam(*param);
        if (page) {
            page->addChild(*param);
        }
    }

    // btmLeft
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamRectangleInteractBtmLeft);
        param->setLabel(kParamRectangleInteractBtmLeftLabel);
        param->setDoubleType(eDoubleTypeXYAbsolute);
        if (param->supportsDefaultCoordinateSystem()) {
            param->setDefaultCoordinateSystem(eCoordinatesNormalised); // no need of kParamDefaultsNormalised
        } else {
            gHostSupportsDefaultCoordinateSystem = false; // no multithread here, see kParamDefaultsNormalised
        }
        param->setDefault(0., 0.);
        param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX);
        param->setDisplayRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX);
        param->setIncrement(1.);
        param->setHint("Coordinates of the bottom left corner of the size rectangle.");
        param->setDigits(0);
        if (page) {
            page->addChild(*param);
        }
    }

    // size
    {
        Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamRectangleInteractSize);
        param->setLabel(kParamRectangleInteractSizeLabel);
        param->setDoubleType(eDoubleTypeXY);
        if (param->supportsDefaultCoordinateSystem()) {
            param->setDefaultCoordinateSystem(eCoordinatesNormalised); // no need of kParamDefaultsNormalised
        } else {
            gHostSupportsDefaultCoordinateSystem = false; // no multithread here, see kParamDefaultsNormalised
        }
        param->setDefault(1., 1.);
        param->setRange(0, 0, DBL_MAX, DBL_MAX);
        param->setDisplayRange(0, 0, 10000., 10000.);
        param->setIncrement(1.);
        param->setDimensionLabels(kParamRectangleInteractSizeDim1, kParamRectangleInteractSizeDim1);
        param->setHint("Width and height of the size rectangle.");
        param->setIncrement(1.);
        param->setDigits(0);
        if (page) {
            page->addChild(*param);
        }
    }

    // interactive
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamRectangleInteractInteractive);
        param->setLabel(kParamRectangleInteractInteractiveLabel);
        param->setHint(kParamRectangleInteractInteractiveHint);
        param->setEvaluateOnChange(false);
        if (page) {
            page->addChild(*param);
        }
    }

    {
        GroupParamDescriptor* group = desc.defineGroupParam("Scalar Parameters");
        group->setLabel("Scalar Parameters");
        group->setOpen(false);
        if (page) {
            page->addChild(*group);
        }

        {
            IntParamDescriptor* param = desc.defineIntParam(kParamDoubleParamNumber);
            param->setLabel(kParamDoubleParamNumberLabel);
            param->setHint(kParamDoubleParamNumberHint);
            param->setRange(0, kParamsCount);
            param->setDisplayRange(0, kParamsCount);
            param->setDefault(0);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
        for (int i = 0; i < kSourceClipCount; ++i) {
            const string istr = unsignedToString(i + 1);
            DoubleParamDescriptor* param = desc.defineDoubleParam(kParamDouble + istr);
            param->setLabel(kParamDoubleLabel + istr);
            param->setHint(kParamDoubleHint + istr);
            param->setAnimates(true);
            // param->setIsSecretAndDisabled(true); // done in the plugin constructor
            param->setRange(-DBL_MAX, DBL_MAX);
            param->setDisplayRange(-1000., 1000.);
            param->setDoubleType(eDoubleTypePlain);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
    }

    {
        GroupParamDescriptor* group = desc.defineGroupParam("Position Parameters");
        group->setLabel("Position Parameters");
        group->setOpen(false);
        if (page) {
            page->addChild(*group);
        }

        {
            IntParamDescriptor* param = desc.defineIntParam(kParamDouble2DParamNumber);
            param->setLabel(kParamDouble2DParamNumberLabel);
            param->setHint(kParamDouble2DParamNumberHint);
            param->setRange(0, kParamsCount);
            param->setDisplayRange(0, kParamsCount);
            param->setDefault(0);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }

        for (int i = 0; i < kSourceClipCount; ++i) {
            const string istr = unsignedToString(i + 1);
            Double2DParamDescriptor* param = desc.defineDouble2DParam(kParamDouble2D + istr);
            param->setLabel(kParamDouble2DLabel + istr);
            param->setHint(kParamDouble2DHint + istr);
            param->setAnimates(true);
            param->setRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX);
            param->setDisplayRange(-DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX);
            // param->setIsSecretAndDisabled(true); // done in the plugin constructor
            param->setDoubleType(eDoubleTypeXYAbsolute);
            bool hostHasNativeOverlayForPosition = param->getHostHasNativeOverlayHandle();
            if (hostHasNativeOverlayForPosition) {
                param->setUseHostNativeOverlayHandle(true);
            }
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
    }

    {
        GroupParamDescriptor* group = desc.defineGroupParam("Color Parameters");
        group->setLabel("Color Parameters");
        group->setOpen(false);
        if (page) {
            page->addChild(*group);
        }
        {
            IntParamDescriptor* param = desc.defineIntParam(kParamColorNumber);
            param->setLabel(kParamColorNumberLabel);
            param->setHint(kParamColorNumberHint);
            param->setRange(0, kParamsCount);
            param->setDisplayRange(0, kParamsCount);
            param->setDefault(0);
            param->setAnimates(false);
            param->setParent(*group);
            if (page) {
                page->addChild(*param);
            }
        }
        for (int i = 0; i < kSourceClipCount; ++i) {
            const string istr = unsignedToString(i + 1);
            RGBParamDescriptor* param = desc.defineRGBParam(kParamColor + istr);
            param->setLabel(kParamColorLabel + istr);
            param->setHint(kParamColorHint + istr);
            param->setAnimates(true);
            param->setParent(*group);
            // param->setIsSecretAndDisabled(true); // done in the plugin constructor
            if (page) {
                page->addChild(*param);
            }
        }
    }
    {
        Int2DParamDescriptor* param = desc.defineInt2DParam(kParamFrameRange);
        param->setDefault(kParamFrameRangeDefault);
        param->setHint(kParamFrameRangeHint);
        param->setLabel(kParamFrameRangeLabel);
        param->setDimensionLabels("min", "max");
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }
    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamFrameRangeAbsolute);
        param->setDefault(kParamFrameRangeAbsoluteDefault);
        param->setHint(kParamFrameRangeAbsoluteHint);
        param->setLabel(kParamFrameRangeAbsoluteLabel);
        param->setAnimates(true);
        if (page) {
            page->addChild(*param);
        }
    }

    if (simple) {
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamRExpr);
            param->setLabel(kParamRExprLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamRExprHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamRExprHint);
            }
            param->setAnimates(true);
            if (page) {
                page->addChild(*param);
            }
        }
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamGExpr);
            param->setLabel(kParamGExprLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamGExprHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamGExprHint);
            }
            param->setAnimates(true);
            if (page) {
                page->addChild(*param);
            }
        }
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamBExpr);
            param->setLabel(kParamBExprLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamBExprHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamBExprHint);
            }
            param->setAnimates(true);
            if (page) {
                page->addChild(*param);
            }
        }
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamAExpr);
            param->setLabel(kParamAExprLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamAExprHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamAExprHint);
            }
            param->setAnimates(true);
            if (page) {
                page->addChild(*param);
            }
        }

        if (!gHostIsNatron) {
            PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamShowExprs);
            param->setLabel(kParamShowExprsLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamShowExprsHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamShowExprsHint);
            }
            if (page) {
                page->addChild(*param);
            }
        }
    } else {
        {
            StringParamDescriptor* param = desc.defineStringParam(kParamScript);
            param->setLabel(kParamScriptLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamScriptHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamScriptHint);
            }
            param->setStringType(eStringTypeMultiLine);
            param->setAnimates(true);
            // param->setDefault(kSeExprDefaultRGBScript);
            if (page) {
                page->addChild(*param);
            }
        }
        if (!gHostIsNatron) {
            PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamShowScript);
            param->setLabel(kParamShowScriptLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamShowScriptHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamShowScriptHint);
            }
            if (page) {
                page->addChild(*param);
            }
        }

        {
            StringParamDescriptor* param = desc.defineStringParam(kParamAlphaScript);
            param->setLabel(kParamAlphaScriptLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamAlphaScriptHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamAlphaScriptHint);
            }
            param->setStringType(eStringTypeMultiLine);
            param->setAnimates(true);
            // param->setDefault(kSeExprDefaultAlphaScript);
            if (page) {
                page->addChild(*param);
            }
        }
        if (!gHostIsNatron) {
            PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamShowAlphaScript);
            param->setLabel(kParamShowAlphaScriptLabel);
            if (hostIsNuke) {
                param->setHint(string(kParamShowAlphaScriptHint) + " " kNukeWarnTcl);
            } else {
                param->setHint(kParamShowAlphaScriptHint);
            }
            if (page) {
                page->addChild(*param);
            }
        }
    }

    {
        BooleanParamDescriptor* param = desc.defineBooleanParam(kParamValidate);
        param->setLabel(kParamValidateLabel);
        param->setHint(kParamValidateHint);
        param->setEvaluateOnChange(true);
        if (gHostIsNatron) {
            param->setIsSecretAndDisabled(true);
        }
        if (page) {
            page->addChild(*param);
        }
    }

    {
        PushButtonParamDescriptor* param = desc.definePushButtonParam(kParamHelp);
        param->setLabel(kParamHelpLabel);
        param->setHint(kParamHelpHint);
        if (page) {
            page->addChild(*param);
        }
    }

    ofxsMaskMixDescribeParams(desc, page);
} // >::describeInContext

template <bool simple>
ImageEffect*
SeExprPluginFactory<simple>::createInstance(OfxImageEffectHandle handle,
                                            ContextEnum /*context*/)
{
    return new SeExprPlugin(handle, simple);
}

static SeExprPluginFactory<false> p1(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
static SeExprPluginFactory<true> p2(kPluginIdentifierSimple, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p1)
    mRegisterPluginFactoryInstance(p2)

        OFXS_NAMESPACE_ANONYMOUS_EXIT
