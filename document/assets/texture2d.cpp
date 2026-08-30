/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/texture2d.h"
#include <QDebug>
#include "core/logger.h"

namespace iris
{

Texture2D::Texture2D()
{
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
    auto tex = new Texture2D();
    tex->image = image;
    return QSharedPointer<Texture2D>(tex);
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

    auto tex = new Texture2D();
    tex->cubeMap = true;
    tex->image = faces[0];
    for (int i = 0; i < 6; ++i) tex->cubeFaceImages[i] = faces[i];
    return QSharedPointer<Texture2D>(tex);
}

QPixmap Texture2D::readData()
{
    return QPixmap::fromImage(image);
}

int Texture2D::getWidth()
{
    return image.width();
}

int Texture2D::getHeight()
{
    return image.height();
}

}
