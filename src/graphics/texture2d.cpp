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
#include <QOpenGLFunctions_3_2_Core>
#include <QOpenGLVersionFunctionsFactory>
#include "../core/logger.h"

namespace iris
{

Texture2D::Texture2D(GLuint texId)
{
    useCustomId = true;
    customId = texId;

    // adapted Qt6
    //    gl = QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_3_2_Core>();
    QOpenGLContext* context = QOpenGLContext::currentContext();
    gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_2_Core>(context);
    if (!gl) {
        qFatal("Failed to get QOpenGLFunctions_3_2_Core");
    }
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
    auto texture = new QOpenGLTexture(image);
    //texture->generateMipMaps();
    texture->setMinMagFilters(QOpenGLTexture::LinearMipMapLinear,QOpenGLTexture::Linear);
	//todo: allow user to set texture anisotrophy
	texture->setMaximumAnisotropy(4); // i think 4 is a good default

    return QSharedPointer<Texture2D>(new Texture2D(texture));
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

    if (!QOpenGLContext::currentContext()) {
        return nullptr;
    }

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

    return QSharedPointer<Texture2D>(new Texture2D(texture));
}

// https://github.com/qt/qt3d/blob/50457f2025f3d38234bd4b27b086e75e4267f68e/tests/auto/render/graphicshelpergl4/tst_graphicshelpergl4.cpp#L303
Texture2DPtr Texture2D::create(int width, int height,QOpenGLTexture::TextureFormat texFormat )
{
    auto texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
    texture->setSize(width, height);
    texture->setFormat(texFormat);
    texture->setMinMagFilters(QOpenGLTexture::Linear, QOpenGLTexture::Linear);
    texture->setWrapMode(QOpenGLTexture::ClampToEdge);
    if (!texture->create())
        qDebug() << "Error creating texture";
    texture->allocateStorage();

    return QSharedPointer<Texture2D>(new Texture2D(texture));
}

Texture2DPtr Texture2D::createDepth(int width, int height)
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

    return Texture2DPtr(new Texture2D(texture));
}

Texture2DPtr Texture2D::createShadowDepth(int width, int height)
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

    return Texture2DPtr(new Texture2D(texture));
}


Texture2D::Texture2D(QOpenGLTexture *tex)
{
    this->texture = tex;

    //gl = QOpenGLContext::currentContext()->versionFunctions<QOpenGLFunctions_3_2_Core>();
    QOpenGLContext* context = QOpenGLContext::currentContext();
    gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_2_Core>(context);
    if (!gl) {
        qFatal("Failed to get QOpenGLFunctions_3_2_Core");
    }
}

void Texture2D::setFilters(QOpenGLTexture::Filter minFilter, QOpenGLTexture::Filter magFilter)
{
    texture->bind();
    texture->setMinMagFilters(minFilter, magFilter);
    texture->release();
}

void Texture2D::setWrapMode(QOpenGLTexture::WrapMode wrapS, QOpenGLTexture::WrapMode wrapT)
{
    texture->bind();
    texture->setWrapMode(QOpenGLTexture::DirectionS, wrapS);
    texture->setWrapMode(QOpenGLTexture::DirectionT, wrapT);
    texture->release();
}

void Texture2D::bind()
{
    if (useCustomId) gl->glBindTexture(GL_TEXTURE_2D, customId);
    else texture->bind();
}

void Texture2D::bind(int index)
{
    if (useCustomId) {
        gl->glActiveTexture(GL_TEXTURE0 + index);
        gl->glBindTexture(GL_TEXTURE_2D, customId);
    }
    else
        texture->bind(index);
}

}
