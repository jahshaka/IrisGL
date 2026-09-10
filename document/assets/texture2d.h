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

#include "document/assets/texture.h"
#include "irisglfwd.h"

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

    /// A LIVE texture (MATERIAL_GAPS_SPEC A-1): pixels a producer writes at
    /// runtime instead of a file on disk. Born opaque black at `width` x
    /// `height`; `source` is the "live://<guid>" reference every consumer
    /// (material rows, the mirror, the writer) recognises. Created through
    /// LiveTextures::create, which owns the session table — this is its
    /// constructor, not a second registry.
    static Texture2DPtr createLive(const QString &guid, const QString &name,
                                   int width, int height, bool mipmaps);

    int getWidth() override;
    int getHeight() override;

    /// For cubemaps: the six face images (+X, -X, +Y, -Y, +Z, -Z), kept so a renderer
    /// without GL can rebuild the sky. Null for other textures.
    bool isCubeMap() const { return cubeMap; }
    const QImage *cubeFaces() const { return cubeMap ? cubeFaceImages : nullptr; }

    // ---- live textures (MATERIAL_GAPS_SPEC A-1) --------------------------
    bool isLive() const { return live; }
    QString liveGuid() const { return liveTextureGuid; }
    QString liveName() const { return liveTextureName; }
    bool liveMipmaps() const { return liveMips; }
    /// Moves on every accepted write and NEVER back. The whole synchronisation
    /// contract between a producer and the renderer: the mirror uploads when
    /// this number differs from the one it last uploaded, so a still image
    /// costs one integer compare per frame and no engine call at all.
    quint64 liveGeneration() const { return generation; }
    /// The current frame, always RGBA8888 and always the creation size.
    const QImage &liveImage() const { return image; }
    /// Replaces the pixels. REFUSES a different size (a live texture's
    /// renderer-side twin cannot resize — destroy and create) and anything
    /// that is not a live texture. Bumps the generation on success.
    bool writeLive(const QImage &rgba);

private:
    Texture2D();

    /// The decoded pixels — populated ONLY by create(QImage) (skies, decals,
    /// light masks, thumbnails build from memory). load(path) deliberately
    /// leaves this null: see the note there.
    QImage image;
    bool cubeMap = false;
    QImage cubeFaceImages[6];

    bool     live = false;
    bool     liveMips = false;
    QString  liveTextureGuid;
    QString  liveTextureName;
    quint64  generation = 0;
};

}

#endif // TEXTURE2D_H
