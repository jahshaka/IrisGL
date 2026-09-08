/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MESHPREWARM_H
#define MESHPREWARM_H

// MeshPrewarm — model files parsed AHEAD of the thread that needs them.
//
// Opening a scene re-parses every model it references with assimp, twice: once
// for the document (SceneReader::getMesh) and once for the asset panel's
// session entry (ProjectAssets::registerSessionAsset). Nothing caches a baked
// form — the import pipeline dropped the up-front preloader and the
// import-time mesh bake is still recorded debt — so a single 89k-vertex
// dragon costs ~0.9 s of pure parse on every open, on the UI thread, which is
// exactly the kind of stall the desktop offers to force-quit.
//
// This is the parse, hoisted: a worker thread fills a MeshPrewarm with the
// aiScenes for a known set of paths, and the readers on the UI thread consume
// them instead of calling assimp. It is a CACHE OF ONE OPEN, owned by the open
// that created it and released with it — not a process-wide cache, because an
// aiScene is large and the document keeps its own copies of everything it
// needs.
//
// SINCE MESH_BAKE PHASE 1 the parse is usually not paid at all: an entry whose
// plan carries a bake path reads the BAKE (import/meshbake.h) — a few memcpys
// — and never touches assimp. The plan is resolved on the UI thread (bake
// lookup is a database query, and QSqlDatabase connections are per-thread), so
// the worker only ever reads files. A missing, stale or corrupt bake falls
// straight back to the parse below; the caller cannot tell the difference
// except in the ledger.
//
// THREAD CONTRACT: parse() is called from the worker, scene()/contains() from
// the consumer thread after the worker has finished. All of it is mutex-guarded
// anyway, because assimp itself is only thread-safe across INDEPENDENT
// Importer instances — which is exactly what each entry owns.

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <memory>

#include "import/meshbake.h"

class aiScene;

namespace iris {

class SceneSource;

/// One entry of a prewarm PLAN: the model file, and where its bake should be
/// (both resolved on the UI thread, before the worker runs).
struct PrewarmItem
{
    QString path;
    QString bakePath;
    QString bakeFingerprint;
};

using BakedModelPtr = std::shared_ptr<const MeshBake::Model>;

class MeshPrewarm
{
public:
    MeshPrewarm();
    ~MeshPrewarm();

    /// Parses `path` with the canonical import preset unless it is already
    /// here. Safe to call from a worker thread. A failed parse is remembered
    /// as a failure (an entry with a null scene) so nobody retries it.
    void parse(const QString &path);

    /// Bake first, parse on a miss. `item.bakePath` empty = the old behaviour.
    void parse(const PrewarmItem &item);

    /// The parsed scene for `path`, or null when absent or unparseable.
    /// NULL for an entry served by a bake — ask baked() first.
    const aiScene *scene(const QString &path) const;

    /// The baked model for `path`, or null when the entry was parsed (or is
    /// absent). Shared: BOTH open-path consumers get the SAME meshes, which is
    /// what kills the double build the old two-parse path had.
    BakedModelPtr baked(const QString &path) const;

    bool contains(const QString &path) const;
    int count() const;
    /// How many entries were served by a bake instead of a parse.
    int bakedCount() const;
    /// Paths that produced a usable scene.
    QStringList paths() const;
    void clear();

private:
    Q_DISABLE_COPY(MeshPrewarm)

    mutable QMutex mLock;
    QHash<QString, std::shared_ptr<SceneSource>> mEntries;
    QHash<QString, BakedModelPtr> mBaked;
};

using MeshPrewarmPtr = std::shared_ptr<MeshPrewarm>;

}   // namespace iris

#endif   // MESHPREWARM_H
