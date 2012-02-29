/*
 * Copyright 2011 The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "VideoLayerAndroid.h"

#include "TilesManager.h"
#include <GLES2/gl2.h>
#include <cutils/log.h>
#include <gui/SurfaceTexture.h>
#include <wtf/text/CString.h>

#if USE(ACCELERATED_COMPOSITING)

#undef XLOGC
#define XLOGC(...) android_printLog(ANDROID_LOG_DEBUG, "VideoLayerAndroid", __VA_ARGS__)

#ifdef DEBUG

#undef XLOG
#define XLOG(...) android_printLog(ANDROID_LOG_DEBUG, "VideoLayerAndroid", __VA_ARGS__)

#else

#undef XLOG
#define XLOG(...)

#endif // DEBUG

namespace WebCore {

double VideoLayerAndroid::m_rotateDegree = 0;

VideoLayerAndroid::VideoLayerAndroid()
    : LayerAndroid((RenderLayer*)0)
{
    init();
}

VideoLayerAndroid::VideoLayerAndroid(const VideoLayerAndroid& layer)
    : LayerAndroid(layer)
{
    init();
}

void VideoLayerAndroid::init()
{
    // m_surfaceTexture is only useful on UI thread, no need to copy.
    // And it will be set at setBaseLayer timeframe
    m_playerState = INITIALIZED;
}

// We can use this function to set the Layer to point to surface texture.
void VideoLayerAndroid::setSurfaceTexture(sp<SurfaceTexture> texture,
                                          int textureName, PlayerState playerState)
{
    m_surfaceTexture = texture;
    m_playerState = playerState;
    TilesManager::instance()->videoLayerManager()->registerTexture(uniqueId(), textureName);
}

void VideoLayerAndroid::showPreparingAnimation(const SkRect& rect,
                                               const SkRect innerRect)
{
    ShaderProgram* shader = TilesManager::instance()->shader();
    VideoLayerManager* manager = TilesManager::instance()->videoLayerManager();
    // Paint the video content's background.
    shader->drawLayerQuad(m_drawTransform, rect, 0, 1, true, GL_TEXTURE_2D,
                          Color(128, 128, 128, 255));

    TransformationMatrix addReverseRotation;
    TransformationMatrix addRotation = m_drawTransform;
    addRotation.translate(innerRect.fLeft, innerRect.fTop);
    double halfButtonSize = manager->getButtonSize() / 2;
    addRotation.translate(halfButtonSize, halfButtonSize);
    addReverseRotation = addRotation;
    addRotation.rotate(m_rotateDegree);
    addRotation.translate(-halfButtonSize, -halfButtonSize);

    SkRect size = SkRect::MakeWH(innerRect.width(), innerRect.height());
    shader->drawLayerQuad(addRotation, size,
                          manager->getSpinnerOuterTextureId(), 1, true);

    addReverseRotation.rotate(-m_rotateDegree);
    addReverseRotation.translate(-halfButtonSize, -halfButtonSize);

    shader->drawLayerQuad(addReverseRotation, size,
                          manager->getSpinnerInnerTextureId(), 1, true);

    m_rotateDegree += ROTATESTEP;
}

SkRect VideoLayerAndroid::calVideoRect(const SkRect& rect)
{
    SkRect videoRect = rect;
    VideoLayerManager* manager = TilesManager::instance()->videoLayerManager();
    float aspectRatio = manager->getAspectRatio(uniqueId());
    float deltaY = rect.height() - rect.width() / aspectRatio;
    if (deltaY >= 0)
        videoRect.inset(0, deltaY / 2);
    else {
        float deltaX = rect.width() - rect.height() * aspectRatio;
        if (deltaX >= 0)
            videoRect.inset(deltaX / 2, 0);
    }
    return videoRect;
}

bool VideoLayerAndroid::drawGL()
{
    // Lazily allocated the textures.
    TilesManager* tilesManager = TilesManager::instance();
    VideoLayerManager* manager = tilesManager->videoLayerManager();
    manager->initGLResourcesIfNeeded();

    ShaderProgram* shader = tilesManager->shader();

    SkRect rect = SkRect::MakeSize(getSize());
    GLfloat surfaceMatrix[16];

    // Calculate the video rect based on the aspect ratio and the element rect.
    SkRect videoRect = calVideoRect(rect);
    if (videoRect != rect) {
        // Paint the whole video element with black color when video content
        // can't cover the whole area.
        shader->drawLayerQuad(m_drawTransform, rect, 0, 1, true, GL_TEXTURE_2D,
                              Color(0, 0, 0, 255));
    }

    // Inner rect is for the progressing / play / pause animation.
    SkRect innerRect = SkRect::MakeWH(manager->getButtonSize(),
                                      manager->getButtonSize());
    if (innerRect.contains(videoRect))
        innerRect = videoRect;
    double buttonSize = manager->getButtonSize();
    innerRect.offset(videoRect.fLeft + (videoRect.width() - buttonSize) / 2,
                     videoRect.fTop + (videoRect.height() - buttonSize) / 2);

    // When we are drawing the animation of the play/pause button in the
    // middle of the video, we need to ask for redraw.
    bool needRedraw = false;

    // Draw the poster image, the progressing image or the Video depending
    // on the player's state.
    if (m_playerState == PREPARING) {
        // Show the progressing animation, with two rotating circles
        showPreparingAnimation(videoRect, innerRect);
        needRedraw = true;
    } else if (m_playerState == PLAYING && m_surfaceTexture.get()) {
        // Show the real video.
        m_surfaceTexture->updateTexImage();
        m_surfaceTexture->getTransformMatrix(surfaceMatrix);
        GLuint textureId = manager->getTextureId(uniqueId());
        shader->drawVideoLayerQuad(m_drawTransform, surfaceMatrix,
                                   videoRect, textureId);
        manager->updateMatrix(uniqueId(), surfaceMatrix);

        // Use the scale to control the fading the sizing during animation
        double scale = manager->drawIcon(uniqueId(), PlayIcon);
        if (scale) {
            innerRect.inset(manager->getButtonSize() / 4 * scale,
                            manager->getButtonSize() / 4 * scale);
            shader->drawLayerQuad(m_drawTransform, innerRect,
                                  manager->getPlayTextureId(), scale, true);
            needRedraw = true;
        }

    } else {
        GLuint textureId = manager->getTextureId(uniqueId());
        GLfloat* matrix = manager->getMatrix(uniqueId());
        if (textureId && matrix) {
            // Show the screen shot for each video.
            shader->drawVideoLayerQuad(m_drawTransform, matrix,
                                       videoRect, textureId);
        } else {
            // Show the static poster b/c there is no screen shot available.
            shader->drawLayerQuad(m_drawTransform, rect, 0, 1, true, GL_TEXTURE_2D,
                              Color(128, 128, 128, 255));
            shader->drawLayerQuad(m_drawTransform, innerRect,
                                  manager->getPosterTextureId(), 1, true);
        }

        // Use the scale to control the fading and the sizing during animation.
        double scale = manager->drawIcon(uniqueId(), PauseIcon);
        if (scale) {
            innerRect.inset(manager->getButtonSize() / 4 * scale,
                            manager->getButtonSize() / 4 * scale);
            shader->drawLayerQuad(m_drawTransform, innerRect,
                                  manager->getPauseTextureId(), scale, true);
            needRedraw = true;
        }

    }
    // Don't short circuit here since we still want to draw the children.
    return drawChildrenGL() || needRedraw;
}

}
#endif // USE(ACCELERATED_COMPOSITING)
