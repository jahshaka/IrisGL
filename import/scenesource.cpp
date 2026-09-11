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

#include <QFile>
#include <QFileInfo>

namespace iris
{

const aiScene *readSceneFile(Assimp::Importer &importer, const QString &filePath,
                             unsigned int flags)
{
    QString resource;
    if (filePath.startsWith(QLatin1String("qrc:"))) resource = filePath.mid(3);   // "qrc:/x" -> ":/x"
    else if (filePath.startsWith(QLatin1Char(':'))) resource = filePath;
    if (resource.isEmpty())
        return importer.ReadFile(filePath.toStdString().c_str(), flags);

    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("readSceneFile: failed to open %s", qUtf8Printable(filePath));
        return nullptr;
    }
    const QByteArray data = file.readAll();
    const QByteArray hint = QFileInfo(resource).suffix().toLower().toLatin1();
    return importer.ReadFileFromMemory(data.constData(), size_t(data.size()), flags,
                                       hint.isEmpty() ? "" : hint.constData());
}

struct SceneSource::Impl
{
    Assimp::Importer importer;
};

SceneSource::SceneSource() : d(new Impl) {}
SceneSource::~SceneSource() = default;

bool SceneSource::read(const QString &filePath)
{
    return readSceneFile(d->importer, filePath, ImportFlags::Canonical) != nullptr;
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
