/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/scenesource.h"

#include "assimp/Importer.hpp"
#include "assimp/scene.h"

#include "import/importflags.h"

namespace iris
{

struct SceneSource::Impl
{
    Assimp::Importer importer;
};

SceneSource::SceneSource() : d(new Impl) {}
SceneSource::~SceneSource() = default;

bool SceneSource::read(const QString &filePath)
{
    return d->importer.ReadFile(filePath.toStdString().c_str(), ImportFlags::Canonical) != nullptr;
}

bool SceneSource::hasScene() const
{
    return d->importer.GetScene() != nullptr;
}

QString SceneSource::errorString() const
{
    return QString::fromUtf8(d->importer.GetErrorString());
}

const aiScene *SceneSource::scene() const
{
    return d->importer.GetScene();
}

Assimp::Importer &SceneSource::importer()
{
    return d->importer;
}

} // namespace iris
