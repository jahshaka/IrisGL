/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef MATERIALHELPER_H
#define MATERIALHELPER_H

#include <QColor>
#include <QFuture>

#include "irisglfwd.h"
#include "document/assets/mesh.h"

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
    /// Image type from the leading magic bytes ("jpg", "png", "dds", "gif",
    /// "bmp", "webp", "tif", "ktx"), or an empty string when unrecognized.
    /// Public: importers and tests use it to keep written extensions honest.
    static QString sniffImageExtension(const unsigned char* data, int len);

private:
    static QImage loadOMEmbeddedTexture(const aiScene* scene,
                                        const QString& texPath,
                                        QString& fileName,
                                        QByteArray& rawBytes);

    static QImage loadGLBEmbeddedTexture(const aiScene* scene,
                                         const QString& texName,
                                         QString& fileName,
                                         QByteArray& rawBytes);

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
    /// Verbatim byte write for embedded compressed textures — no re-encode,
    /// so the bytes on disk always match the (sniffed) extension.
    static void saveTextureBytesAsync(const QByteArray& bytes, const QString& path);

    static QVector<SaveTask> g_textureSaveTasks;
    static QSet<QString> g_savedPaths;
    static QMutex g_saveMutex;
    static QThreadPool* g_threadPool;

};

}

#endif // MATERIALHELPER_H
