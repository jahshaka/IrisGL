/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/assets/texture2d.h"

#include <cassert>
#include <utility>

#include "core/log.h"
#include "document/assets/livetextures.h"

namespace iris
{

std::atomic<Texture2D::ReadableProbe> Texture2D::sProbe{nullptr};

Texture2D::Texture2D()
{
}

bool Texture2D::installReadableProbe(ReadableProbe probe)
{
    ReadableProbe expected = nullptr;
    if (sProbe.compare_exchange_strong(expected, probe)) return true;
    if (expected == probe) return true;   // the same function again: nothing changes
    iris::log::warn("Texture2D::installReadableProbe: REFUSED - a different readable-image probe is already installed "
                    "(the probe is installed once, at boot, never swapped)");
    assert(!"Texture2D::installReadableProbe: a second, different probe");
    return false;
}

Texture2D::ReadableProbe Texture2D::readableProbe()
{
    return sProbe.load();
}

Texture2DPtr Texture2D::load(QString path)
{
    // NO PIXEL DECODE (lane-openasync, 2026-09-03): every consumer of a
    // loaded Texture2D reads `source` (the path) and hands it to the engine,
    // which decodes and uploads it itself (SceneMirror::textureFor, the export
    // walkers). Measured then: decoding here cost 1.11 SECONDS per scene open
    // for pixels nobody read. A null return still means "not a readable
    // image": the host's probe answers that (texture2d.h).
    if (const ReadableProbe probe = sProbe.load()) {
        if (!probe(path.toStdString())) {
            iris::log::warn(("error loading image: " + path).toStdString());
            return Texture2DPtr(nullptr);
        }
    }
    auto tex = new Texture2D();
    tex->source = path;
    return std::shared_ptr<Texture2D>(tex);
}

std::shared_ptr<Texture2D> Texture2D::createCubeMap(Image (&&faces)[6])
{
    for (const Image &face : faces) {
        if (face.isNull() || face.width != faces[0].width || face.height != faces[0].height) return nullptr;
    }
    auto tex = new Texture2D();
    tex->cubeMap = true;
    tex->cubeSize = faces[0].width;
    for (int i = 0; i < 6; ++i) tex->cubeFaceImages[i] = std::move(faces[i]);
    return std::shared_ptr<Texture2D>(tex);
}

// A LIVE texture (MATERIAL_GAPS_SPEC A-1). `source` carries the reference
// form, not a path, so every existing consumer keeps working on a string:
// the mirror's slot table sees a non-empty source, the writer sees a live
// reference and skips the row, and a panel shows something meaningful.
Texture2DPtr Texture2D::createLive(const QString &guid, const QString &name,
                                   int width, int height, bool mipmaps)
{
    if (guid.isEmpty() || width <= 0 || height <= 0) return Texture2DPtr();

    auto tex = new Texture2D();
    tex->image           = Image::filled(width, height, 0, 0, 0, 255);
    tex->live            = true;
    tex->liveMips        = mipmaps;
    tex->liveTextureGuid = guid;
    tex->liveTextureName = name;
    tex->source          = LiveTextures::refFor(guid);
    // Generation 1, never 0: the mirror records 0 for "never uploaded", so a
    // texture that has only ever held its birth pixels still gets its first
    // upload rather than being mistaken for one already in sync.
    tex->generation      = 1;
    return std::shared_ptr<Texture2D>(tex);
}

bool Texture2D::writeLive(Image &rgba)
{
    if (!live || rgba.isNull()) return false;
    if (rgba.width != image.width || rgba.height != image.height) return false;
    std::swap(image, rgba);
    ++generation;
    return true;
}

int Texture2D::getWidth()
{
    return cubeMap ? cubeSize : image.width;
}

int Texture2D::getHeight()
{
    return cubeMap ? cubeSize : image.height;
}

}
