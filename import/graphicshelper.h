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

class aiScene;

class AssimpObject {
public:
	AssimpObject() = default;
    AssimpObject(const aiScene *ai, QString g) : scene(ai), GUID(g) {}
    const aiScene *getSceneData() { return scene; }
    QString getGUID() { return GUID; }
    ~AssimpObject() {}

private:
    const aiScene *scene = nullptr;
    QString GUID;
};

Q_DECLARE_METATYPE(AssimpObject)
Q_DECLARE_METATYPE(AssimpObject*)

namespace iris
{

class GraphicsHelper
{
public:
    static QString loadAndProcessShader(QString shaderPath);

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

    template <typename F>
    static void loadAllMeshesAndAnimationsFromStore(const QVector<F> &store,
                                                    QString filePath,
                                                    QList<MeshPtr> &meshes,
                                                    QMap<QString, SkeletalAnimationPtr> &animations)
     {
         // Prefer the first entry that actually CARRIES an assimp scene. The
         // old first-match stopped at whichever entry shared the path — often
         // an add-to-project AssetNodeObject (value = SceneNodePtr, no scene)
         // registered alongside the import's AssimpObject — and reloaded the
         // file although the parsed scene sat one element further on. Worse,
         // when NO entry matched at all it returned silently and the caller
         // got empty meshes (missing geometry on scene load).
         for (F ao : store) {
             if (ao->path != filePath) continue;
             AssimpObject *assimpObject = ao->getValue().template value<AssimpObject*>();
             const aiScene *scene = assimpObject ? assimpObject->getSceneData() : nullptr;
             if (scene != nullptr) {
                 meshes = loadAllMeshesFromAssimpScene(scene);
                 animations = Mesh::extractAnimations(scene, filePath);
                 return;
             }
         }

         // No cached scene anywhere in the store: load the file itself.
         loadAllMeshesAndAnimationsFromFile(filePath, meshes, animations);
     }


    static QList<MeshPtr> loadAllMeshesFromAssimpScene(const aiScene* scene);
};

}

#endif // GRAPHICSHELPER_H
