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
    /**
     * Loads all meshes from mesh file
     * Useful for loading a mesh file containing multiple meshes
     * Caller is responsible for releasing returned Mesh pointers
     * @param filePath
     * @return
     */
    static QList<MeshPtr> loadAllMeshesFromFile(QString filePath);

    static void loadAllMeshesAndAnimationsFromFile(QString filePath,
                                                   QList<MeshPtr> &meshes,
                                                   QMap<QString, SkeletalAnimationPtr> &animations);

    /// The meshes and clips of a parse the caller already holds (the threaded
    /// open's prewarm): a copy out of the scene, never a file read.
    static void loadAllMeshesAndAnimationsFromSource(const SceneSource &source,
                                                     const QString &filePath,
                                                     QList<MeshPtr> &meshes,
                                                     QMap<QString, SkeletalAnimationPtr> &animations);

    /// IrisGL-internal (a complete aiScene needs assimp's headers).
    static QList<MeshPtr> loadAllMeshesFromAssimpScene(const aiScene* scene);

    /**
     * Reads a file for its animation CLIPS only (ImportFlags::ClipNamesOnly —
     * no geometry post-processing, the file's unit factor still applied so the
     * translation keys match a character parsed with the canonical preset).
     * Returns the clips uniquified the way Mesh::extractAnimations uniquifies
     * them. An unreadable file returns an empty map and sets *error (never
     * empty); a readable file with no animation returns an empty map and an
     * empty *error. Studio's clip readers go through here so that assimp stays
     * an irisgl-private import dependency.
     */
    static QMap<QString, SkeletalAnimationPtr> loadAnimationsFromClipFile(const QString &filePath,
                                                                          QString *error = nullptr);
};

}

#endif // GRAPHICSHELPER_H
