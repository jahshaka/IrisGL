/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef TEXTURECUBE_H
#define TEXTURECUBE_H

#include <QSharedPointer>
#include "texture.h"
#include <QOpenGLTexture>
#include <QImage>
#include "../irisglfwd.h"

class QOpenGLFunctions_3_2_Core;

namespace iris
{

class TextureCube: public Texture
{

public:

    /**
     * Returns a null shared pointer
     * @return
     */
    static TextureCubePtr null()
    {
        return TextureCubePtr(nullptr);
    }

    /**
     * Returns the path to the source file of the texture
     * @return
     */
    QString getSource() {
        return source;
    }

    static TextureCubePtr load(QString, QString, QString, QString, QString, QString, QImage *i = nullptr);

	static TextureCubePtr create(int width, int height);

    void setFilters(QOpenGLTexture::Filter minFilter, QOpenGLTexture::Filter magFilter);
    void setWrapMode(QOpenGLTexture::WrapMode wrapS, QOpenGLTexture::WrapMode wrapT);

    void bind() override;
    void bind(int index) override;

    ~TextureCube() override {}

private:
    QOpenGLFunctions_3_2_Core* gl;

    TextureCube(QOpenGLTexture *tex);
};

}

#endif // TEXTURECUBE_H
