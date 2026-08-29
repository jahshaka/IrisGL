/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "texture2d.h"
#include <QDebug>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLVersionFunctionsFactory>
#include "../core/logger.h"

namespace iris
{

// ---------------------------------------------------------------------------
// GL object builders. Each is exactly the former creator body; the public
// creators either call these immediately (a context is current — the legacy
// path, behaviour unchanged) or defer them until first use.
// ---------------------------------------------------------------------------

static QOpenGLFunctions_3_2_Core* currentGl()
{
    QOpenGLContext* context = QOpenGLContext::currentContext();
    if (!context) return nullptr;
    return QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_2_Core>(context);
}

static QOpenGLTexture* buildFromImage(const QImage& image)
{
    auto texture = new QOpenGLTexture(image);
    //texture->generateMipMaps();
    texture->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,QOpenGLTexture::Linear);
    //todo: allow user to set texture anisotrophy
    texture->setMaximumAnisotropy(4); // i think 4 is a good default
    return texture;
}

// https://github.com/qt/qt3d/blob/50457f2025f3d38234bd4b27b086e75e4267f68e/tests/auto/render/graphicshelpergl4/tst_graphicshelpergl4.cpp#L303
static QOpenGLTexture* buildEmpty(int width, int height, QOpenGLTexture::TextureFormat texFormat)
{
    auto texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
    texture->setSize(width, height);
    texture->setFormat(texFormat);
    texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    if (!texture->create())
        qDebug() << "Error creating texture";
    texture->allocateStorage();
    return texture;
}

static QOpenGLTexture* buildDepth(int width, int height)
{
    auto texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
    texture->setSize(width, height);
    texture->setFormat(QOpenGLTexture::DepthFormat);
    texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    texture->setComparisonMode(QOpenGLTexture::CompareNone);
    texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    if (!texture->create())
        qDebug() << "Error creating texture";
    texture->allocateStorage(QOpenGLTexture::Depth,QOpenGLTexture::Float32);
    return texture;
}

static QOpenGLTexture* buildShadowDepth(int width, int height)
{
    auto texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
    texture->setSize(width, height);
    texture->setFormat(QOpenGLTexture::DepthFormat);
    texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    // http://fabiensanglard.net/shadowmappingPCF/
    texture->setComparisonMode(QOpenGLTexture::CompareNone);
    texture->setComparisonFunction(QOpenGLTexture::CompareLessEqual);
    if (!texture->create())
        qDebug() << "Error creating texture";
    texture->allocateStorage(QOpenGLTexture::Depth,QOpenGLTexture::Float32);
    return texture;
}

// faces: +X, -X, +Y, -Y, +Z, -Z — already validated and square.
static QOpenGLTexture* buildCubeMap(const QImage faces[6])
{
    const int size = faces[0].width();
    QOpenGLTexture *texture = new QOpenGLTexture(QOpenGLTexture::TargetCubeMap);
    texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
    texture->setSize(size, size, 1);
    texture->allocateStorage();

    QOpenGLTexture::CubeMapFace cubeFaces[6] = {
        QOpenGLTexture::CubeMapPositiveX,
        QOpenGLTexture::CubeMapNegativeX,
        QOpenGLTexture::CubeMapPositiveY,
        QOpenGLTexture::CubeMapNegativeY,
        QOpenGLTexture::CubeMapPositiveZ,
        QOpenGLTexture::CubeMapNegativeZ
    };
    for (int i = 0; i < 6; ++i) {
        texture->setData(0, 0, cubeFaces[i],
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         faces[i].constBits());
    }
    texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    texture->setMinificationFilter(QOpenGLTexture::Linear);
    texture->setMagnificationFilter(QOpenGLTexture::Linear);
    return texture;
}

// ---------------------------------------------------------------------------

Texture2D::Texture2D()
{
    texture = nullptr;
    gl = nullptr;
}

Texture2D::Texture2D(GLuint texId)
{
    texture = nullptr;
    useCustomId = true;
    customId = texId;
    gl = currentGl();   // may be null: resolved again on first use
}

Texture2D::Texture2D(QOpenGLTexture *tex)
{
    this->texture = tex;
    gl = currentGl();
}

bool Texture2D::ensureCreated()
{
    if (texture || useCustomId) {
        if (!gl) gl = currentGl();
        return true;
    }
    if (deferred == Deferred::None) return false;
    if (!QOpenGLContext::currentContext()) return false;

    switch (deferred) {
    case Deferred::Image:       texture = buildFromImage(deferredImage); break;
    case Deferred::Empty:       texture = buildEmpty(deferredWidth, deferredHeight, deferredFormat); break;
    case Deferred::Depth:       texture = buildDepth(deferredWidth, deferredHeight); break;
    case Deferred::ShadowDepth: texture = buildShadowDepth(deferredWidth, deferredHeight); break;
    case Deferred::CubeMap:     texture = buildCubeMap(deferredFaces); break;
    case Deferred::None:        break;
    }
    deferred = Deferred::None;
    deferredImage = QImage();
    for (auto &f : deferredFaces) f = QImage();
    gl = currentGl();
    return texture != nullptr;
}

Texture2DPtr Texture2D::load(QString path)
{
    return load(path,true);
}

Texture2DPtr Texture2D::load(QString path,bool flipY)
{
    auto image = QImage(path);
    if(image.isNull())
    {
        irisLog("error loading image: "+path);
        return Texture2DPtr(nullptr);
    }

    if(flipY)
        image = image.mirrored(false,true);

    auto tex = create(image);
    tex->source = path;

    return tex;
}

Texture2DPtr Texture2D::create(QImage image)
{
    if (QOpenGLContext::currentContext())
        return QSharedPointer<Texture2D>(new Texture2D(buildFromImage(image)));

    auto tex = new Texture2D();
    tex->deferred = Deferred::Image;
    tex->deferredImage = image;
    tex->deferredWidth = image.width();
    tex->deferredHeight = image.height();
    return QSharedPointer<Texture2D>(tex);
}

Texture2DPtr Texture2D::createFromId(uint textureId)
{
    auto tex = new Texture2D(textureId);
    return Texture2DPtr(tex);
}

QSharedPointer<Texture2D> Texture2D::createCubeMap(QString negZ, QString posZ,
                                                   QString posY, QString negY,
                                                   QString negX, QString posX,
                                                   QImage *info)
{
    QImage faces[6] = {
        QImage(posX).convertToFormat(QImage::Format_RGBA8888),
        QImage(negX).convertToFormat(QImage::Format_RGBA8888),
        QImage(posY).convertToFormat(QImage::Format_RGBA8888),
        QImage(negY).convertToFormat(QImage::Format_RGBA8888),
        QImage(posZ).convertToFormat(QImage::Format_RGBA8888),
        QImage(negZ).convertToFormat(QImage::Format_RGBA8888)
    };
    for (auto &face : faces) {
        if (face.isNull()) {
            return nullptr;
        }
    }
    int size = faces[0].width();
    for (auto &face : faces) {
        if (face.width() != size || face.height() != size) {
            face = face.scaled(size, size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
    }

    if (QOpenGLContext::currentContext())
        return QSharedPointer<Texture2D>(new Texture2D(buildCubeMap(faces)));

    // Previously returned null here, which silently dropped the sky. Defer instead.
    auto tex = new Texture2D();
    tex->deferred = Deferred::CubeMap;
    for (int i = 0; i < 6; ++i) tex->deferredFaces[i] = faces[i];
    tex->deferredWidth = tex->deferredHeight = size;
    return QSharedPointer<Texture2D>(tex);
}

Texture2DPtr Texture2D::create(int width, int height,QOpenGLTexture::TextureFormat texFormat )
{
    if (QOpenGLContext::currentContext())
        return QSharedPointer<Texture2D>(new Texture2D(buildEmpty(width, height, texFormat)));

    auto tex = new Texture2D();
    tex->deferred = Deferred::Empty;
    tex->deferredWidth = width; tex->deferredHeight = height; tex->deferredFormat = texFormat;
    return QSharedPointer<Texture2D>(tex);
}

Texture2DPtr Texture2D::createDepth(int width, int height)
{
    if (QOpenGLContext::currentContext())
        return Texture2DPtr(new Texture2D(buildDepth(width, height)));

    auto tex = new Texture2D();
    tex->deferred = Deferred::Depth;
    tex->deferredWidth = width; tex->deferredHeight = height;
    return Texture2DPtr(tex);
}

Texture2DPtr Texture2D::createShadowDepth(int width, int height)
{
    if (QOpenGLContext::currentContext())
        return Texture2DPtr(new Texture2D(buildShadowDepth(width, height)));

    auto tex = new Texture2D();
    tex->deferred = Deferred::ShadowDepth;
    tex->deferredWidth = width; tex->deferredHeight = height;
    return Texture2DPtr(tex);
}

void Texture2D::setFilters(QOpenGLTexture::Filter minFilter, QOpenGLTexture::Filter magFilter)
{
    if (!ensureCreated() || !texture) return;
    texture->bind();
    texture->setMinMagFilters(minFilter, magFilter);
    texture->release();
}

void Texture2D::setWrapMode(QOpenGLTexture::WrapMode wrapS, QOpenGLTexture::WrapMode wrapT)
{
    if (!ensureCreated() || !texture) return;
    texture->bind();
    texture->setWrapMode(QOpenGLTexture::DirectionS, wrapS);
    texture->setWrapMode(QOpenGLTexture::DirectionT, wrapT);
    texture->release();
}

GLuint Texture2D::getTextureId()
{
    if (useCustomId) return customId;
    if (!ensureCreated() || !texture) return 0;
    return texture->textureId();
}

int Texture2D::getWidth()
{
    if (texture) return texture->width();
    return deferredWidth;
}

int Texture2D::getHeight()
{
    if (texture) return texture->height();
    return deferredHeight;
}

void Texture2D::bind()
{
    if (!ensureCreated()) return;
    if (useCustomId) { if (gl) gl->glBindTexture(GL_TEXTURE_2D, customId); }
    else if (texture) texture->bind();
}

void Texture2D::bind(int index)
{
    if (!ensureCreated()) return;
    if (useCustomId) {
        if (!gl) return;
        gl->glActiveTexture(GL_TEXTURE0 + index);
        gl->glBindTexture(GL_TEXTURE_2D, customId);
    }
    else if (texture)
        texture->bind(index);
}

}
