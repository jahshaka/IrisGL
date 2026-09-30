/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/meshprewarm.h"
#include "import/parsecensus.h"

#include <QMutexLocker>


namespace iris {

MeshPrewarm::MeshPrewarm() = default;
MeshPrewarm::~MeshPrewarm() = default;

void MeshPrewarm::parse(const PrewarmItem &item)
{
    const QString &path = item.path;
    if (path.isEmpty() || path.startsWith(':')) return;   // built-in primitives
    {
        QMutexLocker locked(&mLock);
        if (mTried.contains(path)) return;
    }
    // THE BAKE, AND ONLY THE BAKE (FORWARD-ONLY-1). A missing, stale or
    // corrupt bake is a miss — the open shows the model missing; there is no
    // parse behind it.
    MeshBake::Model model;
    if (!item.bakePath.isEmpty()) model = MeshBake::read(item.bakePath, item.bakeFingerprint);
    ParseCensus::recordBake(model.valid);   // app.openStats()
    QMutexLocker locked(&mLock);
    mTried.insert(path);
    if (model.valid)
        mBaked.insert(path, std::make_shared<const MeshBake::Model>(std::move(model)));
}

BakedModelPtr MeshPrewarm::baked(const QString &path) const
{
    QMutexLocker locked(&mLock);
    return mBaked.value(path);
}

bool MeshPrewarm::contains(const QString &path) const
{
    QMutexLocker locked(&mLock);
    return mTried.contains(path);
}

int MeshPrewarm::count() const
{
    QMutexLocker locked(&mLock);
    return mBaked.size();
}

int MeshPrewarm::bakedCount() const
{
    QMutexLocker locked(&mLock);
    return mBaked.size();
}

QStringList MeshPrewarm::paths() const
{
    QMutexLocker locked(&mLock);
    return mBaked.keys();
}

void MeshPrewarm::clear()
{
    QMutexLocker locked(&mLock);
    mTried.clear();
    mBaked.clear();
}

}   // namespace iris
