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
#include <QStringList>

#include "irisglfwd.h"
#include "document/assets/mesh.h"

class aiMaterial;

namespace iris
{

class MaterialHelper
{
public:
    static DefaultMaterialPtr createMaterial(aiMaterial* aiMat, QString assetPath);


    /// `assetPath` resolves the file's RELATIVE texture references (the
    /// source's directory). `writeDir` is where extraction OUTPUT lands —
    /// embedded texture files and the split metallic/roughness maps; empty
    /// means beside the source (legacy behavior — never right for read-only
    /// sources; the import pipeline always passes a staging dir).
    static void extractMaterialData(const aiScene *scene,
                                    aiMaterial *aiMat,
                                    QString assetPath,
                                    MeshMaterialData& mat,
                                    const QString &writeDir = QString());
    /// Image type from the leading magic bytes ("jpg", "png", "dds", "gif",
    /// "bmp", "webp", "tif", "ktx"), or an empty string when unrecognized.
    /// Public: importers and tests use it to keep written extensions honest.
    static QString sniffImageExtension(const unsigned char* data, int len);

    /// CONTAINMENT (deep audit 2026-09, finding F2). A model file names its
    /// textures itself, and those names are FILE CONTENT: `../../../.ssh/id_rsa`
    /// and `/etc/passwd` are both legal in an .obj/.mtl/.fbx/.gltf. Resolved
    /// verbatim (what this code used to do) such a reference is opened, copied
    /// into the content-addressed store and shipped in every export — a
    /// one-hop exfiltration primitive out of any downloaded model.
    ///
    /// Resolves `name` against `sourceDir` and returns a path GUARANTEED to sit
    /// inside `sourceDir` (symlinks resolved), or an empty string when no such
    /// file exists. An escaping reference falls back to its own basename inside
    /// `sourceDir` — the overwhelmingly common real-world case is a DCC tool
    /// writing an absolute authoring path for a texture that ships beside the
    /// model — and records a warning either way.
    ///
    /// Warnings are drained by the caller on the SAME THREAD (the sink is
    /// thread-local): the import pipeline takes them right after its parse and
    /// puts them in ImportResult::warnings.
    /// `kind` names the reference in the warning text ("texture", "material
    /// library", …) — the containment rule itself is the same for all of them.
    static QString containedTexturePath(const QString& name, const QString& sourceDir,
                                        const QString& kind = QStringLiteral("texture"));

    /// Take (and clear) this thread's containment warnings.
    static QStringList takeContainmentWarnings();

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

    /// The containment warning sink (thread-local; see takeContainmentWarnings).
    static QStringList& warningSink();

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
