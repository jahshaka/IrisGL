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

// MeshPrewarm — the BAKES an open needs, read ahead of the thread that needs them.
//
// A worker fills a MeshPrewarm from a PLAN resolved on the UI thread (the bake
// lookup is a catalog query, and QSqlDatabase connections are per-thread), and
// the open's readers on the UI thread consume the deserialized models instead
// of reading the files themselves: both open-path consumers (the scene reader
// and the session registration) get the SAME model. It is a CACHE OF ONE OPEN,
// owned by the open that created it and released with it.
//
// BAKES ONLY (FORWARD-ONLY-1). There is no parse behind a missing, stale or
// corrupt bake: an archive import bakes every model it brings, the asset import
// bakes at import, and a model with no current bake is shown MISSING by the
// open (a scene issue names it).
//
// THREAD CONTRACT: parse() is called from the worker, baked()/contains() from
// the consumer thread after the worker has finished. All of it is mutex-guarded.

#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>

#include "import/meshbake.h"

namespace iris {

/// One entry of a prewarm PLAN: the model file, and where its bake should be
/// (both resolved on the UI thread, before the worker runs). `bakePath` empty =
/// this build has no bake for the file.
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

    /// Reads `item`'s bake unless `item.path` was already tried. Safe from a
    /// worker thread. A miss is remembered, so nobody retries it.
    void parse(const PrewarmItem &item);

    /// The baked model for `path`, or null (a miss, or never planned).
    BakedModelPtr baked(const QString &path) const;
    bool contains(const QString &path) const;
    /// How many entries were served by a bake (= bakedCount()).
    int count() const;
    int bakedCount() const;
    /// Paths whose bake was read.
    QStringList paths() const;
    void clear();

private:
    Q_DISABLE_COPY(MeshPrewarm)

    mutable QMutex mLock;
    QSet<QString> mTried;
    QHash<QString, BakedModelPtr> mBaked;
};

using MeshPrewarmPtr = std::shared_ptr<MeshPrewarm>;

}   // namespace iris

#endif // MESHPREWARM_H
