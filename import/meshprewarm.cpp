/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/meshprewarm.h"

#include <QFileInfo>
#include <QMutexLocker>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"

#include "document/scenegraph/meshnode.h"   // SceneSource (owns the Importer)
#include "import/importflags.h"

namespace iris {

MeshPrewarm::MeshPrewarm() = default;
MeshPrewarm::~MeshPrewarm() = default;

void MeshPrewarm::parse(const QString &path)
{
    if (path.isEmpty() || path.startsWith(':')) return;   // built-in primitives
    {
        QMutexLocker locked(&mLock);
        if (mEntries.contains(path)) return;
    }
    if (!QFileInfo::exists(path)) {
        QMutexLocker locked(&mLock);
        mEntries.insert(path, std::shared_ptr<SceneSource>());
        return;
    }

    // Its OWN importer: assimp is thread-safe only across independent
    // Importer instances, and the aiScene must outlive this call (the entry
    // owns the importer, so it does).
    auto source = std::make_shared<SceneSource>();
    const aiScene *scene =
        source->importer.ReadFile(path.toStdString().c_str(), iris::ImportFlags::Canonical);

    QMutexLocker locked(&mLock);
    mEntries.insert(path, scene ? source : std::shared_ptr<SceneSource>());
}

const aiScene *MeshPrewarm::scene(const QString &path) const
{
    QMutexLocker locked(&mLock);
    const auto it = mEntries.constFind(path);
    if (it == mEntries.constEnd() || !it->get()) return nullptr;
    return (*it)->importer.GetScene();
}

bool MeshPrewarm::contains(const QString &path) const
{
    QMutexLocker locked(&mLock);
    return mEntries.contains(path);
}

int MeshPrewarm::count() const
{
    QMutexLocker locked(&mLock);
    int n = 0;
    for (const auto &entry : mEntries)
        if (entry) ++n;
    return n;
}

QStringList MeshPrewarm::paths() const
{
    QMutexLocker locked(&mLock);
    QStringList out;
    for (auto it = mEntries.constBegin(); it != mEntries.constEnd(); ++it)
        if (it.value()) out.append(it.key());
    return out;
}

void MeshPrewarm::clear()
{
    QMutexLocker locked(&mLock);
    mEntries.clear();
}

}   // namespace iris
