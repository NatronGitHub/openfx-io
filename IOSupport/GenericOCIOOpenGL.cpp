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
#include <cstring>
#ifdef DEBUG
#include <cstdio>
#define DBG(x) x
#else
#define DBG(x) (void)0
#endif
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

#if OCIO_VERSION_HEX >= 0x02000000
#include "glsl.h"
#endif

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
#if OCIO_VERSION_HEX < 0x02000000
                                      "uniform sampler3D tex2;\n"
#endif
                                      "\n"
                                      "void main()\n"
                                      "{\n"
                                      "    vec4 col = texture2D(tex1, gl_TexCoord[0].st);\n"
#if OCIO_VERSION_HEX >= 0x02000000
                                      "    gl_FragColor = OCIODisplay(col);\n"
#else
                                       "    gl_FragColor = OCIODisplay(col, tex2);\n"
#endif
                                      "}\n";

OCIOOpenGLContextData::OCIOOpenGLContextData()
#if OCIO_VERSION_HEX < 0x02000000
    : procLut3D()
    , procShaderCacheID()
    , procLut3DCacheID()
    , procLut3DID(0)
    , procShaderProgramID(0)
    , procFragmentShaderID(0)
#endif
{
    if (!ofxsLoadOpenGLOnce()) {
        // We could use an error message here
        throwSuiteStatusException(kOfxStatFailed);
    }
}

OCIOOpenGLContextData::~OCIOOpenGLContextData()
{
#if OCIO_VERSION_HEX >= 0x02000000
    glBuilder.reset();
#else
    if (procLut3DID != 0) {
        glDeleteTextures(1, &procLut3DID);
    }
    if (procFragmentShaderID != 0) {
        glDeleteShader(procFragmentShaderID);
    }
    if (procShaderProgramID != 0) {
        glDeleteProgram(procShaderProgramID);
    }
#endif
}

#if OCIO_VERSION_HEX < 0x02000000
static GLuint
compileShaderText(GLenum shaderType,
                  const char* text)
{
    GLuint shader;
    GLint stat;

    shader = glCreateShader(shaderType);
    glShaderSource(shader, 1, (const GLchar**)&text, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &stat);

    if (!stat) {
        GLchar log[1000];
        GLsizei len;
        glGetShaderInfoLog(shader, 1000, &len, log);
        DBG(std::fprintf(stderr, "Error: problem compiling shader: %s\n", log));

        return 0;
    }

    return shader;
}

static GLuint
linkShaders(GLuint fragShader)
{
    if (!fragShader) {
        return 0;
    }

    GLuint program = glCreateProgram();

    if (fragShader) {
        glAttachShader(program, fragShader);
    }

    glLinkProgram(program);

    /* check link */
    {
        GLint stat;
        glGetProgramiv(program, GL_LINK_STATUS, &stat);
        if (!stat) {
            GLchar log[1000];
            GLsizei len;
            glGetProgramInfoLog(program, 1000, &len, log);
            DBG(std::fprintf(stderr, "Shader link error:\n%s\n", log));

            return 0;
        }
    }

    return program;
}

static void
allocateLut3D(GLuint* lut3dTexID,
              std::vector<float>* lut3D)
{
    glGenTextures(1, lut3dTexID);

    int num3Dentries = 3 * LUT3D_EDGE_SIZE * LUT3D_EDGE_SIZE * LUT3D_EDGE_SIZE;
    lut3D->resize(num3Dentries);
    std::memset(&(*lut3D)[0], 0, sizeof(float) * num3Dentries);

    // https://github.com/AcademySoftwareFoundation/OpenColorIO/blame/RB-1.1/src/apps/ociodisplay/main.cpp#L234
    glEnable(GL_TEXTURE_3D);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, *lut3dTexID);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F_ARB,
                 LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE,
                 0, GL_RGB, GL_FLOAT, &(*lut3D)[0]);
}
#endif

#if defined(OFX_IO_USING_OCIO)

void
GenericOCIO::applyGL(const Texture* srcImg,
                     const OCIO::ConstProcessorRcPtr& processor,
                     OCIOOpenGLContextData* contextData)
{

#if OCIO_VERSION_HEX >= 0x02000000
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
        gpuProc = gpulegacy ? processor->getOptimizedLegacyGPUProcessor(OCIO::OPTIMIZATION_GOOD, LUT3D_EDGE_SIZE)
                            : processor->getOptimizedGPUProcessor(OCIO::OPTIMIZATION_VERY_GOOD);
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
#else
    // Reference code: https://github.com/imageworks/OpenColorIO/blob/RB-1.1/src/apps/ociodisplay/main.cpp
    // Step 1: Create a GPU Shader Description
    // https://github.com/imageworks/OpenColorIO/blame/RB-1.1/src/apps/ociodisplay/main.cpp#L562
    OCIO::GpuShaderDesc shaderDesc;
    shaderDesc.setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_0);
    shaderDesc.setFunctionName("OCIODisplay");
    shaderDesc.setLut3DEdgeLen(LUT3D_EDGE_SIZE);

    // Allocate CPU lut + init lut 3D texture, this should be done only once
    GLuint lut3dTexID = 0;
    std::vector<float>* lut3D = 0;
    if (contextData) {
        lut3D = &contextData->procLut3D;
    } else {
        lut3D = new std::vector<float>;
    }
    if (contextData) {
        lut3dTexID = contextData->procLut3DID;
    }
    if (lut3D->size() == 0) {
        // The LUT was not allocated yet or the caller does not want to cache the lut
        // allocating at all
        allocateLut3D(&lut3dTexID, lut3D);
        if (contextData) {
            contextData->procLut3DCacheID = lut3dTexID;
        }
    }

    glEnable(GL_TEXTURE_3D);

    // Step 2: Compute the 3D LUT
    // https://github.com/imageworks/OpenColorIO/blame/RB-1.1/src/apps/ociodisplay/main.cpp#L568
    // The lut3D texture should be cached to avoid calling glTexSubImage3D again
    string lut3dCacheID = processor->getGpuLut3DCacheID(shaderDesc);

    if (!contextData || (contextData->procLut3DCacheID != lut3dCacheID)) {
        // Unfortunately the LUT3D is not cached yet, or caller does not want caching
        processor->getGpuLut3D(&(*lut3D)[0], shaderDesc);

        /*for (std::size_t i = 0; i < lut3D->size(); ++i) {
            assert((*lut3D)[i] == (*lut3D)[i] && (*lut3D)[i] != std::numeric_limits<float>::infinity());
           }*/

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_3D, lut3dTexID);
        glTexSubImage3D(GL_TEXTURE_3D, 0,
                        0, 0, 0,
                        LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE, LUT3D_EDGE_SIZE,
                        GL_RGB, GL_FLOAT, &(*lut3D)[0]);

        // update the cache ID
        if (contextData) {
            contextData->procLut3DCacheID = lut3dCacheID;
        }
    }

    if (!contextData) {
        // Ensure we free the vector if we allocated it
        delete lut3D;
    }
    lut3D = 0;

    // Step 3: Compute the Shader
    // https://github.com/imageworks/OpenColorIO/blame/RB-1.1/src/apps/ociodisplay/main.cpp#L584
    // The shader should be cached, to avoid generating it again
    string shaderCacheID = processor->getGpuShaderTextCacheID(shaderDesc);

    GLuint programID;
    GLuint fragShaderID;
    if (!contextData || (contextData->procShaderCacheID != shaderCacheID)) {
        // Unfortunately the shader is not cached yet, or caller does not want caching
        string shaderString;
        shaderString += processor->getGpuShaderText(shaderDesc);
        shaderString += "\n";
        shaderString += g_fragShaderText;

        fragShaderID = compileShaderText(GL_FRAGMENT_SHADER, shaderString.c_str());
        programID = linkShaders(fragShaderID);
        if (contextData) {
            contextData->procShaderProgramID = programID;
            contextData->procFragmentShaderID = fragShaderID;
            // update the cache ID
            contextData->procShaderCacheID = shaderCacheID;
        }
    } else {
        programID = contextData->procShaderProgramID;
        fragShaderID = contextData->procFragmentShaderID;
    }

    // https://github.com/imageworks/OpenColorIO/blame/RB-1.1/src/apps/ociodisplay/main.cpp#L603
    glUseProgram(programID);
    glUniform1i(glGetUniformLocation(programID, "tex1"), 0);
    glUniform1i(glGetUniformLocation(programID, "tex2"), 1);

    // https://github.com/AcademySoftwareFoundation/OpenColorIO/blame/RB-1.1/src/apps/ociodisplay/main.cpp#L192
    // Bind textures and apply texture mapping
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    int srcTarget = srcImg->getTarget();
    glBindTexture(srcTarget, srcImg->getIndex());
    glTexParameteri(srcTarget, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(srcTarget, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(srcTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(srcTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, lut3dTexID);

    const OfxRectI& srcBounds = srcImg->getBounds();

    glBegin(GL_QUADS);
    glTexCoord2f(0., 0.);
    glVertex2f(srcBounds.x1, srcBounds.y1);
    glTexCoord2f(0., 1.);
    glVertex2f(srcBounds.x1, srcBounds.y2);
    glTexCoord2f(1., 1.);
    glVertex2f(srcBounds.x2, srcBounds.y2);
    glTexCoord2f(1., 0.);
    glVertex2f(srcBounds.x2, srcBounds.y1);
    glEnd();

    glUseProgram(0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (!contextData) {
        glDeleteTextures(1, &lut3dTexID);
        glDeleteProgram(programID);
        glDeleteShader(fragShaderID);
    }
#endif // OCIO_VERSION_HEX >= 0x02000000
} // GenericOCIO::applyGL

#endif // defined(OFX_IO_USING_OCIO)

#endif // defined(OFX_SUPPORTS_OPENGLRENDER)

NAMESPACE_OFX_IO_EXIT
NAMESPACE_OFX_EXIT
