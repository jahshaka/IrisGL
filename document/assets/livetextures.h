/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_LIVETEXTURES_H
#define IRIS_LIVETEXTURES_H

// LIVE TEXTURES — pixels a producer writes at runtime, addressed by a guid
// (MATERIAL_GAPS_SPEC ADDENDUM A-1).
//
// A live texture is a SESSION-ONLY identity. It is never persisted, never
// pinned to a project, never a content-addressed object and never exported:
// it exists so that a material row can name pixels the same way it names a
// file, and so that `material.set(node, {baseColorMap: guid})` resolves for a
// generated image exactly as it does for an imported one. Reopening a scene
// that once referenced one finds NOTHING — which is why the scene writer skips
// live references rather than writing a guid that can never resolve again.
//
// THE WHOLE DOCUMENT SURFACE IS A GENERATION COUNTER. A producer writes pixels
// into the Texture2D and the counter moves; SceneMirror notices the move and
// re-uploads once — never per frame, never on a still image. Nothing else in
// the document model knows or cares where the pixels came from (a script, a
// video decoder, a future render target).
//
// The registry is a process-wide table because a texture REFERENCE is a plain
// string ("live://<guid>") stored in a material row, and the row has to be
// resolvable by whoever loads it — the same role a file system plays for a
// file-backed texture. It is NOT cleared at the project boundary — a live
// texture is a session object and outlives every project it was bound in
// (scripting.e2e.live_textures asserts it); the registry dies with the process.

#include <QHash>
#include <QString>
#include <QStringList>

#include "irisglfwd.h"

namespace iris
{

class LiveTextures
{
public:
    /// The scheme every live reference carries. A material row holding one of
    /// these is what tells the writer to skip the row and the mirror to look
    /// here instead of on disk.
    static QString refFor(const QString &guid);
    static bool isLiveRef(const QString &ref);
    /// The guid inside a "live://<guid>" reference; an empty string for
    /// anything else (including a bare guid — a reference is a reference).
    static QString guidOf(const QString &ref);

    /// Registers a new live texture, `width` x `height`, initially opaque
    /// black. Returns null if the guid is already taken or the size is not
    /// positive. `mipmaps` is fixed here because the renderer fixes it at
    /// creation: a write replaces the whole chain, it cannot grow one.
    static Texture2DPtr create(const QString &guid, const QString &name,
                               int width, int height, bool mipmaps = false);

    /// The live texture behind a guid OR a "live://<guid>" reference; null
    /// when there is none. THE MISS IS ORDINARY: it is what a scene written
    /// against an older session looks like.
    static Texture2DPtr find(const QString &guidOrRef);

    static bool destroy(const QString &guidOrRef);
    /// Every live guid, in creation order.
    static QStringList guids();
    static void clear();

private:
    static QHash<QString, Texture2DPtr> &table();
    static QStringList &order();
};

}

#endif // IRIS_LIVETEXTURES_H
