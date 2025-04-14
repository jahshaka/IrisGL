/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "texturecube.h"
#include <QDebug>
#include <QOpenGLFunctions_3_2_Core>
#include "../core/logger.h"

namespace iris
{

TextureCubePtr TextureCube::load(QString negZ, QString posZ,
                                      QString posY, QString negY,
                                      QString negX, QString posX,
                                      QImage *info)
{
    int width = 0;
    int height = 0;
    int depth = 0;

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

    return TextureCubePtr(new TextureCube(texture));
}

TextureCubePtr TextureCube::create(int width, int height)
{
	auto texture = new QOpenGLTexture(QOpenGLTexture::TargetCubeMap);
	texture->create();
	texture->setSize(width, height, 1);
	texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
	texture->allocateStorage();

	texture->setWrapMode(QOpenGLTexture::ClampToEdge);
	texture->setMinificationFilter(QOpenGLTexture::Linear);
	texture->setMagnificationFilter(QOpenGLTexture::Linear);

    return TextureCubePtr(new TextureCube(texture));
}

TextureCube::TextureCube(QOpenGLTexture *tex)
{
    this->texture = tex;
    this->gl = new QOpenGLFunctions_3_2_Core();
    this->gl->initializeOpenGLFunctions();
}

void TextureCube::setFilters(QOpenGLTexture::Filter minFilter, QOpenGLTexture::Filter magFilter)
{
    texture->bind();
    texture->setMinMagFilters(minFilter, magFilter);
    texture->release();
}

void TextureCube::setWrapMode(QOpenGLTexture::WrapMode wrapS, QOpenGLTexture::WrapMode wrapT)
{
    texture->bind();
    texture->setWrapMode(QOpenGLTexture::DirectionS, wrapS);
    texture->setWrapMode(QOpenGLTexture::DirectionT, wrapT);
    texture->release();
}

void TextureCube::bind()
{
    if (useCustomId) gl->glBindTexture(GL_TEXTURE_CUBE_MAP, customId);
    else texture->bind();
}

void TextureCube::bind(int index)
{
    if (useCustomId) {
        gl->glActiveTexture(GL_TEXTURE0 + index);
        gl->glBindTexture(GL_TEXTURE_CUBE_MAP, customId);
    }
    else
        texture->bind(index);
}

}
