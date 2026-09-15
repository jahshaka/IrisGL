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
#include "import/parsecensus.h"
#include "assimp/Importer.hpp"
#include "assimp/mesh.h"
#include "assimp/matrix4x4.h"
#include "assimp/vector3.h"
#include "assimp/quaternion.h"


#include "document/assets/vertexlayout.h"

namespace iris
{

QList<iris::MeshPtr> GraphicsHelper::loadAllMeshesFromFile(QString filePath,
                                                           const ImportTransform &xf)
{
    Assimp::Importer importer;
    // THE CHOKE POINT (import/scenesource.h): the asset's import transform is
    // applied here or the geometry is a different size from its bake.
    const aiScene *scene = readSceneFile(importer, filePath, iris::ImportFlags::Canonical, xf);
    return loadAllMeshesFromAssimpScene(scene);
}

void GraphicsHelper::loadAllMeshesAndAnimationsFromFile(
    QString filePath,
    QList<MeshPtr> &meshes,
    QMap<QString, SkeletalAnimationPtr> &animations,
    const ImportTransform &xf)
{
    Assimp::Importer importer;
    // THE PARSE, COUNTED AND ATTRIBUTED TO ITS THREAD (import/parsecensus.h,
    // through the choke point): this is the read a project open pays when no
    // bake and no prewarm served the file, and open.responsive asserts it
    // never runs on the UI thread.
    const aiScene *scene = readSceneFile(importer, filePath, iris::ImportFlags::Canonical, xf);

    if (scene != nullptr) {
        meshes = loadAllMeshesFromAssimpScene(scene);
        animations = Mesh::extractAnimations(scene, filePath);
    }
}

QMap<QString, SkeletalAnimationPtr> GraphicsHelper::loadAnimationsFromClipFile(const QString &filePath,
                                                                              QString *error,
                                                                              const ImportTransform &xf)
{
    if (error) error->clear();
    Assimp::Importer importer;
    // A CLIP FILE TAKES THE RIG'S TRANSFORM, not its own: a clip's translation
    // keys are in the FILE's units and drive a skeleton that was parsed — and
    // baked — under the character asset's import settings. Read with identity
    // (the default) they exploded a rescaled rig, which is the same defect
    // ImportFlags::ClipNamesOnly was created to close for the unit factor.
    const aiScene *scene = readSceneFile(importer, filePath, iris::ImportFlags::ClipNamesOnly, xf);
    if (!scene) {
        if (error) {
            *error = QString::fromUtf8(importer.GetErrorString());
            if (error->isEmpty()) *error = QStringLiteral("the file could not be read");
        }
        return {};
    }
    if (scene->mNumAnimations == 0) return {};
    return Mesh::extractAnimations(scene, filePath);
}

void GraphicsHelper::loadAllMeshesAndAnimationsFromSource(
    const SceneSource &source,
    const QString &filePath,
    QList<MeshPtr> &meshes,
    QMap<QString, SkeletalAnimationPtr> &animations)
{
    const aiScene *scene = source.scene();
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
