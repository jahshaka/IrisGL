/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#include "texture.h"
#include <QOpenGLTexture>

namespace iris
{

GLuint Texture::getTextureId()
{
    if (useCustomId) return customId;
    return texture->textureId();
}

void Texture::bind()
{
    texture->bind();
}

void Texture::bind(int index)
{
    texture->bind(index);
}

int Texture::getWidth()
{
    return texture->width();
}

int Texture::getHeight()
{
    return texture->height();
}

void Texture::resize(int width, int height, bool force)
{
    if((texture->width() == width && texture->height() == height) && !force)
        return;

    auto texFormat = texture->format();
    auto minFilter = texture->minificationFilter();
    auto magFilter = texture->magnificationFilter();
    auto wrapModeS = texture->wrapMode(QOpenGLTexture::DirectionS);
    auto wrapModeT = texture->wrapMode(QOpenGLTexture::DirectionT);

    //return;
    texture->destroy();
    texture->setFormat(texFormat);
    texture->setMinMagFilters(minFilter, magFilter);
    texture->setWrapMode(QOpenGLTexture::DirectionS, wrapModeS);
    texture->setWrapMode(QOpenGLTexture::DirectionT, wrapModeT);
    texture->setSize(width, height);
    texture->create();
    texture->allocateStorage();
}

}
