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
#include "../core/logger.h"

namespace iris
{

Texture2D::Texture2D(GLuint texId)
{
    useCustomId = true;
    customId = texId;
    this->gl = new QOpenGLFunctions_3_2_Core();
    this->gl->initializeOpenGLFunctions();
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

Texture2DPtr Texture2D::createCubeMap(QString negZ, QString posZ,
                                      QString posY, QString negY,
                                      QString negX, QString posX,
                                      QImage *info)
{
    int width, height, depth;

    const QImage pos_x = QImage(posX).convertToFormat(QImage::Format_RGBA8888);
    const QImage neg_x = QImage(negX).convertToFormat(QImage::Format_RGBA8888);
    const QImage pos_y = QImage(posY).convertToFormat(QImage::Format_RGBA8888);
    const QImage neg_y = QImage(negY).convertToFormat(QImage::Format_RGBA8888);
    const QImage pos_z = QImage(posZ).convertToFormat(QImage::Format_RGBA8888);
    const QImage neg_z = QImage(negZ).convertToFormat(QImage::Format_RGBA8888);

    if (info) {
        width = info->width();
        height = info->height();
        depth = info->depth();
    }

    auto texture = new QOpenGLTexture(QOpenGLTexture::TargetCubeMap);
    texture->create();
    texture->setSize(width, height, depth);
    texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
    texture->allocateStorage();

    if (!pos_x.isNull()) {
        texture->setData(0, 0, QOpenGLTexture::CubeMapPositiveX,
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         (const void*) pos_x.constBits(), 0);
    }

    if (!pos_y.isNull()) {
        texture->setData(0, 0, QOpenGLTexture::CubeMapPositiveY,
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         (const void*) pos_y.constBits(), 0);
    }

    if (!pos_z.isNull()) {
        texture->setData(0, 0, QOpenGLTexture::CubeMapPositiveZ,
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         (const void*) pos_z.constBits(), 0);
    }

    if (!neg_x.isNull()) {
        texture->setData(0, 0, QOpenGLTexture::CubeMapNegativeX,
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         (const void*) neg_x.constBits(), 0);
    }

    if (!neg_y.isNull()) {
        texture->setData(0, 0, QOpenGLTexture::CubeMapNegativeY,
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         (const void*) neg_y.constBits(), 0);
    }

    if (!neg_z.isNull()) {
        texture->setData(0, 0, QOpenGLTexture::CubeMapNegativeZ,
                         QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                         (const void*) neg_z.constBits(), 0);
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
    this->gl = new QOpenGLFunctions_3_2_Core();
    this->gl->initializeOpenGLFunctions();
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
