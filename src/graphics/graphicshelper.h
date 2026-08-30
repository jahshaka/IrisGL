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

#include "../irisglfwd.h"
#include "../graphics/mesh.h"

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
         for (F ao : store) {
             if (ao->path == filePath) {
                // Not every path-matching asset holds an AssimpObject: add-to-project
                // registers an AssetNodeObject (value = SceneNodePtr) under the same
                // ModelTypes::Object, and value<AssimpObject*>() then returns null.
                // Dereferencing unchecked crashed on scene load after a same-session
                // add-to-project — fall back to loading the file instead.
                AssimpObject *assimpObject = ao->getValue().template value<AssimpObject*>();
                const aiScene *scene = assimpObject ? assimpObject->getSceneData() : nullptr;

                if (scene != nullptr) {
                    meshes = loadAllMeshesFromAssimpScene(scene);
                    animations = Mesh::extractAnimations(scene, filePath);
                } else {
                    qWarning() << "loadAllMeshesAndAnimationsFromStore: asset for" << filePath
                               << "carries no assimp scene (add-to-project node asset?) — loading the file instead";
                    loadAllMeshesAndAnimationsFromFile(filePath, meshes, animations);
                }

                break;
             }
         }
     }


    static QList<MeshPtr> loadAllMeshesFromAssimpScene(const aiScene* scene);
};

}

#endif // GRAPHICSHELPER_H
