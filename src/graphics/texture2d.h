/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef TEXTURE2D_H
#define TEXTURE2D_H

#include <QSharedPointer>
#include <QOpenGLTexture>
#include <QImage>

#include "texture.h"
#include "../irisglfwd.h"

class QOpenGLFunctions_3_2_Core;

namespace iris
{

class Texture2D: public Texture
{

public:

    /**
     * Returns a null shared pointer
     * @return
     */
    static Texture2DPtr null()
    {
        return Texture2DPtr(nullptr);
    }

    //todo: move mipmap generation and texture filter responsibilities to Texture2D class's non-static members

    /**
     * Loads a texture. The image is flipped on the y-axis.
     * @param path
     * @return
     */
    static Texture2DPtr load(QString path);

    /**
     * Loads a texture. Setting flipY to true flips the image on the y-axis
     * @param path
     * @return
     */
    static Texture2DPtr load(QString path, bool flipY);

    /**
     * Created texture from QImage
     * @param image
     * @return
     */
    static Texture2DPtr create(QImage image);
    static Texture2DPtr createFromId(uint textureId);

    static Texture2DPtr create(int width, int height,QOpenGLTexture::TextureFormat texFormat = QOpenGLTexture::RGBAFormat);
    static Texture2DPtr createDepth(int width, int height);
    static Texture2DPtr createShadowDepth(int width, int height);
//    {
//        return create(width, height, QOpenGLTexture::DepthFormat);
//    }

    /**
     * Returns the path to the source file of the texture
     * @return
     */
    QString getSource() {
        return source;
    }

    // todo: REMOVE!! (nick)
    static Texture2DPtr createCubeMap(QString, QString, QString, QString, QString, QString, QImage *i = nullptr);

    //void resize(int width, int height, bool force = false) override;

    QPixmap readData();

    void setFilters(QOpenGLTexture::Filter minFilter, QOpenGLTexture::Filter magFilter);
    void setWrapMode(QOpenGLTexture::WrapMode wrapS, QOpenGLTexture::WrapMode wrapT);
    void bind() override;
    void bind(int index) override;
    GLuint getTextureId() override;
    int getWidth() override;
    int getHeight() override;

    /**
     * Creates the GL texture now if creation was deferred and a GL context is
     * current. Returns true when the texture is usable.
     *
     * Texture2D no longer requires a GL context at construction. When none is
     * current, the creator stores what it would have uploaded and the upload
     * happens on first use (bind / getTextureId / setFilters ...). This lets
     * the scene document (Scene, LightNode, materials) be built with no GL at
     * all — required for the engine-backed viewport, where IrisGL never draws.
     */
    bool ensureCreated();
    bool isDeferred() const { return deferred != Deferred::None; }
    /// For cubemaps: the six face images (+X, -X, +Y, -Y, +Z, -Z), kept so a renderer
    /// without GL can rebuild the sky. Empty for other textures.
    bool isCubeMap() const { return cubeMap; }
    const QImage *cubeFaces() const { return cubeMap ? cubeFaceImages : nullptr; }

private:
    Texture2D();                       // deferred: no GL object yet
    Texture2D(QOpenGLTexture* tex);
    Texture2D(GLuint texId);
    QOpenGLFunctions_3_2_Core* gl = nullptr;

    enum class Deferred { None, Image, Empty, Depth, ShadowDepth, CubeMap };
    Deferred deferred = Deferred::None;
    QImage deferredImage;
    QImage deferredFaces[6];
    bool cubeMap = false;
    QImage cubeFaceImages[6];
    int deferredWidth = 0, deferredHeight = 0;
    QOpenGLTexture::TextureFormat deferredFormat = QOpenGLTexture::RGBAFormat;
};

}

#endif // TEXTURE2D_H
