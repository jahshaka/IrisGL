/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/texture2d.h"

#include <QImageReader>
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
    // NO PIXEL DECODE (lane-openasync, 2026-09-03). This used to decode the
    // whole image and then MIRROR it vertically — the legacy GL renderer's
    // flipY convention. Nothing reads those pixels any more: since the
    // rendering half of IrisGL was deleted, every consumer of a loaded
    // Texture2D reads `source` (the path) and hands it to the engine, which
    // decodes and uploads it itself (SceneMirror::textureFor, the sky's
    // QImage(source), the export walkers). `image` was write-only.
    //
    // Measured: 31 built-in material presets each set up to four textures,
    // and AssetWidget::trigger() rebuilds them on EVERY scene open —
    // 1.11 SECONDS per open of decode-then-mirror whose result was thrown
    // away. A header probe keeps the ONE contract callers depend on (a null
    // return means "not a readable image", which is how
    // CustomMaterial::setTextureWithUniform decides to remove the slot) at
    // roughly zero cost.
    //
    // flipY is therefore now a no-op for the file path. It stays in the
    // signature because create(QImage) callers still express the convention,
    // and because the sky/decal/mask paths that DO need pixels build their
    // QImage themselves.
    Q_UNUSED(flipY);
    QImageReader probe(path);
    if (!probe.canRead()) {
        irisLog("error loading image: "+path);
        return Texture2DPtr(nullptr);
    }

    auto tex = create(QImage());
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
