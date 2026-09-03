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
// THREAD CONTRACT: parse() is called from the worker, scene()/contains() from
// the consumer thread after the worker has finished. All of it is mutex-guarded
// anyway, because assimp itself is only thread-safe across INDEPENDENT
// Importer instances — which is exactly what each entry owns.

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <memory>

class aiScene;

namespace iris {

class SceneSource;

class MeshPrewarm
{
public:
    MeshPrewarm();
    ~MeshPrewarm();

    /// Parses `path` with the canonical import preset unless it is already
    /// here. Safe to call from a worker thread. A failed parse is remembered
    /// as a failure (an entry with a null scene) so nobody retries it.
    void parse(const QString &path);

    /// The parsed scene for `path`, or null when absent or unparseable.
    const aiScene *scene(const QString &path) const;
    bool contains(const QString &path) const;
    int count() const;
    /// Paths that produced a usable scene.
    QStringList paths() const;
    void clear();

private:
    Q_DISABLE_COPY(MeshPrewarm)

    mutable QMutex mLock;
    QHash<QString, std::shared_ptr<SceneSource>> mEntries;
};

using MeshPrewarmPtr = std::shared_ptr<MeshPrewarm>;

}   // namespace iris

#endif   // MESHPREWARM_H
