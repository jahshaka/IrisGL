/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef GRAPHICSHELPER_H
#define GRAPHICSHELPER_H

#include <QDebug>
#include <QString>
#include <QList>

#include "irisglfwd.h"
#include "document/assets/mesh.h"
#include "import/scenesource.h"

struct aiScene;

namespace iris
{

class GraphicsHelper
{
public:
    /// Every mesh of a model file, PARSED (the canonical preset, through the
    /// choke point). NOT A RUN-TIME CALL (SHIPPED-BAKES-1): the app reads every
    /// mesh it draws from a bake, and `source.assimp_import_only` refuses this
    /// outside the import pipeline. What is left calling it is the test suites'
    /// fixture parse (tests/support/testmesh.h) — geometry for a node in a
    /// suite with no library behind it.
    static QList<MeshPtr> loadAllMeshesFromFile(QString filePath,
                                                const ImportTransform &xf = ImportTransform());

    /// IrisGL-internal (a complete aiScene needs assimp's headers).
    static QList<MeshPtr> loadAllMeshesFromAssimpScene(const aiScene* scene);
};

}

#endif // GRAPHICSHELPER_H
