/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "assimp/scene.h"
#include "import/graphicshelper.h"


#include "assimp/postprocess.h"
#include "import/importflags.h"
#include "assimp/Importer.hpp"
#include "assimp/mesh.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"


#include "document/assets/vertexlayout.h"

namespace iris
{

QList<iris::MeshPtr> GraphicsHelper::loadAllMeshesFromFile(QString filePath)
{
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(filePath.toStdString().c_str(), iris::ImportFlags::Canonical);
    return loadAllMeshesFromAssimpScene(scene);
}

void GraphicsHelper::loadAllMeshesAndAnimationsFromFile(
    QString filePath,
    QList<MeshPtr> &meshes,
    QMap<QString, SkeletalAnimationPtr> &animations)
{
    Assimp::Importer importer;
    const aiScene *scene = importer.ReadFile(filePath.toStdString().c_str(), iris::ImportFlags::Canonical);

    if (scene != nullptr) {
        meshes = loadAllMeshesFromAssimpScene(scene);
        animations = Mesh::extractAnimations(scene, filePath);
    }
}

QList<MeshPtr> GraphicsHelper::loadAllMeshesFromAssimpScene(const aiScene *scene)
{
    QList<MeshPtr> meshes;

    if (scene) {
        for (unsigned i = 0; i < scene->mNumMeshes; i++) {
            auto m = scene->mMeshes[i];
            auto mesh = iris::MeshPtr(new Mesh(m));
            if (m->HasBones()) {
                auto skel = Mesh::extractSkeleton(m, scene);
                mesh->setSkeleton(skel);
            }
            meshes.append(mesh);
        }
    }

    return meshes;
}

}
