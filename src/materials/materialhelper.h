/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

#ifndef MATERIALHELPER_H
#define MATERIALHELPER_H

#include <QColor>
#include <QFuture>

#include "../irisglfwd.h"
#include "../graphics/mesh.h"

class aiMaterial;

namespace iris
{

class MaterialHelper
{
public:
    static DefaultMaterialPtr createMaterial(aiMaterial* aiMat, QString assetPath);


    static void extractMaterialData(const aiScene *scene,
                                    aiMaterial *aiMat,
                                    QString assetPath,
                                    MeshMaterialData& mat);
private:
    static QImage loadOMEmbeddedTexture(const aiScene* scene,
                                        const QString& texPath,
                                        QString& fileName);

    static QImage loadGLBEmbeddedTexture(const aiScene* scene,
                                         const QString& texName,
                                         QString& fileName);

    static QImage convertAiTextureToImage(const aiTexture *at);

    static void loadEmbeddedTexture(const aiScene* scene,
                                    const QString& texName,
                                    const QString& assetPath,
                                    QString& texPath,
                                    bool& hasEmbedded);

    static void waitForTextureSave(const QString& path);
    static void waitForAllTextureSaves();

    struct SaveTask {
        QFuture<void> future;
        QString path;
    };

    static void saveTextureAsync(const QImage& image, const QString& path);

    static QVector<SaveTask> g_textureSaveTasks;
    static QSet<QString> g_savedPaths;
    static QMutex g_saveMutex;
    static QThreadPool* g_threadPool;

};

}

#endif // MATERIALHELPER_H
