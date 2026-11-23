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

#include <atomic>
#include <memory>
#include <string>

#include "document/assets/image.h"
#include "document/assets/texture.h"
#include "irisglfwd.h"

namespace iris
{

// GL-free texture asset. Holds the source path and, for the pixels the document
// itself holds (a cube sky's faces, a live texture's frame), an iris::Image,
// which is what the engine-backed viewport reads: SceneMirror re-loads by
// `source`, and cube skies read the six face images via cubeFaces().
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

    /// A texture that names a FILE: records `path` as `source`; no pixel is read (the engine decodes the file
    /// itself). NULL when the process's readable-image probe (below) says the file is no readable image - the one
    /// contract callers depend on: a null load is "no map".
    /// WITH NO PROBE INSTALLED (a Qt-free process: jahlab, jahengine) the path is recorded as it is and THE ENGINE's
    /// OWN LOAD IS THE VERDICT: OgreScene::loadTexture refuses a file that is no image by name (its error sink).
    static Texture2DPtr load(QString path);

    /// THE READABLE-IMAGE PROBE (NOQT-DOCUMENT-1 stage 3): the document reads no file, so whether a path is a
    /// readable image is asked of the host. Studio installs its probe (src/services/imagedecode.h) ONCE at boot,
    /// before any thread that loads exists; it is read from any thread. A second install of the SAME function is a
    /// no-op; a DIFFERENT one is refused by name (an assertion in Debug, a logged error and `false` in Release).
    using ReadableProbe = bool (*)(const std::string &path);
    static bool installReadableProbe(ReadableProbe probe);
    static ReadableProbe readableProbe();

    /**
     * Returns the path to the source file of the texture
     * @return
     */
    QString getSource() {
        return source;
    }

    /// A cube texture from its six decoded faces (+X, -X, +Y, -Y, +Z, -Z), square and of one size (Studio's
    /// imagedecode.h decodes and sizes them). NULL when a face is null or the sizes differ.
    static Texture2DPtr createCubeMap(const Image (&faces)[6]);

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
    const Image *cubeFaces() const { return cubeMap ? cubeFaceImages : nullptr; }

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
    /// The current frame, always 8-bit RGBA and always the creation size.
    const Image &liveImage() const { return image; }
    /// Replaces the pixels. REFUSES a different size (a live texture's
    /// renderer-side twin cannot resize — destroy and create), a null image
    /// and anything that is not a live texture. Bumps the generation on success.
    bool writeLive(const Image &rgba);

private:
    Texture2D();

    /// The pixels the document holds: a live texture's frame, or a cube's +X
    /// face (its size). load(path) leaves this null: it records the path only.
    Image image;
    bool cubeMap = false;
    Image cubeFaceImages[6];

    static std::atomic<ReadableProbe> sProbe;

    bool     live = false;
    bool     liveMips = false;
    QString  liveTextureGuid;
    QString  liveTextureName;
    quint64  generation = 0;
};

}

#endif // TEXTURE2D_H
