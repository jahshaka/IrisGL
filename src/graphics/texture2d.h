/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef TEXTURE2D_H
#define TEXTURE2D_H

#include <QSharedPointer>
#include <QImage>
#include <QPixmap>

#include "texture.h"
#include "../irisglfwd.h"

namespace iris
{

// GL-free texture asset. Holds the source path and (for images loaded or
// generated on the CPU) the pixel data, which is what the engine-backed
// viewport reads: SceneMirror re-loads by `source`, and cube skies read the
// six face images via cubeFaces().
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

    /**
     * Returns the path to the source file of the texture
     * @return
     */
    QString getSource() {
        return source;
    }

    static Texture2DPtr createCubeMap(QString, QString, QString, QString, QString, QString, QImage *i = nullptr);

    QPixmap readData();

    int getWidth() override;
    int getHeight() override;

    /// For cubemaps: the six face images (+X, -X, +Y, -Y, +Z, -Z), kept so a renderer
    /// without GL can rebuild the sky. Null for other textures.
    bool isCubeMap() const { return cubeMap; }
    const QImage *cubeFaces() const { return cubeMap ? cubeFaceImages : nullptr; }

private:
    Texture2D();

    QImage image;
    bool cubeMap = false;
    QImage cubeFaceImages[6];
};

}

#endif // TEXTURE2D_H
