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
 * OFX GenericOCIO plugin add-on.
 * Adds OpenColorIO functionality to any plugin.
 */

#include "GenericOCIO.h"

#include <cstdlib>
#include <ofxNatron.h>
#include <ofxsImageEffect.h>
#include <ofxsLog.h>
#include <ofxsOGLUtilities.h>
#include <ofxsParam.h>
#include <stdexcept>
#include <string>

// Use OpenGL function directly, no need to use ofxsOGLFunctions.h directly because we don't use OSMesa
#include "glad.h"

#ifdef OFX_IO_USING_OCIO

#include "glsl.h"

namespace OCIO = OCIO_NAMESPACE;
#endif

using std::string;

NAMESPACE_OFX_ENTER
NAMESPACE_OFX_IO_ENTER

#if defined(OFX_SUPPORTS_OPENGLRENDER)

static const int LUT3D_EDGE_SIZE = 32;
static const char* g_fragShaderText = ""
                                      "\n"
                                      "uniform sampler2D tex1;\n"
                                      "\n"
                                      "void main()\n"
                                      "{\n"
                                      "    vec4 col = texture2D(tex1, gl_TexCoord[0].st);\n"
                                      "    gl_FragColor = OCIODisplay(col);\n"
                                      "}\n";

OCIOOpenGLContextData::OCIOOpenGLContextData()
{
    if (!ofxsLoadOpenGLOnce()) {
        // We could use an error message here
        throwSuiteStatusException(kOfxStatFailed);
    }
}

OCIOOpenGLContextData::~OCIOOpenGLContextData()
{
    glBuilder.reset();
}

#if defined(OFX_IO_USING_OCIO)

void
GenericOCIO::applyGL(const Texture* srcImg,
                     const OCIO::ConstProcessorRcPtr& processor,
                     OCIOOpenGLContextData* contextData)
{
    // See https://github.com/imageworks/OpenColorIO/blob/master/src/apps/ociodisplay/main.cpp

    // Create an OpenGL helper, this should be done only once
    OCIO::OpenGLBuilderRcPtr glBuilder;
    if (contextData) {
        if (contextData->processorCacheID != processor->getCacheID()) {
            contextData->glBuilder.reset();
        }
        glBuilder = contextData->glBuilder;
    }
    if (!glBuilder) {
        // Extract the shader information.
        bool gpulegacy = false;
        OCIO::ConstGPUProcessorRcPtr gpuProc;
        {
            OCIOOptimizedProcessorLock lock;
            gpuProc = gpulegacy ? processor->getOptimizedLegacyGPUProcessor(OCIO::OPTIMIZATION_GOOD, LUT3D_EDGE_SIZE)
                                : processor->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_VERY_GOOD);
        }
        // See https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/b2e88b195c1e1a82a51818e0a4aa2975e03b6a88/vendor/aftereffects/OpenColorIO_AE_Context.cpp#L851
        // Step 1: Create a GPU Shader Description
        OCIO::GpuShaderDescRcPtr shaderDesc = OCIO::GpuShaderDesc::CreateShaderDesc();
        shaderDesc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_2);
        shaderDesc->setFunctionName("OCIODisplay");
        shaderDesc->setResourcePrefix("ocio_");

        // Step 2: Collect the shader program information for a specific processor
        gpuProc->extractGpuShaderInfo(shaderDesc);

        // Step 3: Use the helper OpenGL builder
        glBuilder = OCIO::OpenGLBuilder::Create(shaderDesc);
        if (contextData) {
            contextData->processorCacheID = processor->getCacheID();
            contextData->glBuilder = glBuilder;
        }

        // Step 4: Allocate & upload all the LUTs
        //
        // NB: The start index for the texture indices is 1 as one texture
        //     was already created for the input image.
        //
        glBuilder->allocateAllTextures(1);

        // Step 5: Build the fragment shader program
        glBuilder->buildProgram(g_fragShaderText, false);
    }

    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    int srcTarget = srcImg->getTarget();
    glBindTexture(srcTarget, srcImg->getIndex());
    glTexParameteri(srcTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(srcTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(srcTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(srcTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Step 6: Enable the fragment shader program, and all needed textures
    glBuilder->useProgram();
    glUniform1i(glGetUniformLocation(glBuilder->getProgramHandle(), "tex1"), 0); // image texture

    // Bind textures and apply texture mapping
    glBuilder->useAllTextures(); // LUT textures
    glBuilder->useAllUniforms();

    if (GL_NO_ERROR != glGetError()) {
        throwSuiteStatusException(kOfxStatFailed);
    }
    const OfxRectI& srcBounds = srcImg->getBounds();

    glPushMatrix();
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(srcBounds.x1, srcBounds.y1);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(srcBounds.x1, srcBounds.y2);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(srcBounds.x2, srcBounds.y2);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(srcBounds.x2, srcBounds.y1);
    glEnd();
    glPopMatrix();
    if (GL_NO_ERROR != glGetError()) {
        throwSuiteStatusException(kOfxStatFailed);
    }
} // GenericOCIO::applyGL

#endif // defined(OFX_IO_USING_OCIO)

#endif // defined(OFX_SUPPORTS_OPENGLRENDER)

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT
