/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef TEXTURE_H
#define TEXTURE_H

#include <QSharedPointer>
#include <qopengl.h>

class QOpenGLTexture;

namespace iris
{

class Texture
{
protected:
    GLuint customId;
    bool useCustomId = false;

public:
    QOpenGLTexture* texture;
    QString source;

    GLuint getTextureId();
    virtual void bind();
    virtual void bind(int index);

    virtual int getWidth();
    virtual int getHeight();
    virtual void resize(int width, int height, bool force = false);

    virtual ~Texture(){}
};

}

#endif // TEXTURE_H
