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

// No assimp include (ENGINEERING_DEBT L4 part 3): the importer's types are
// named by forward declaration only; the TUs that read them include assimp
// themselves (IrisGL-private).
struct aiScene;
struct aiMaterial;
struct aiTexture;
#include <QColor>
#include <QFuture>
#include <QStringList>

#include "irisglfwd.h"
#include "document/assets/mesh.h"


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
    /// `sourceFile` is the MODEL FILE itself (assetPath is only its
    /// directory). glTF/GLB material import needs it: assimp cannot say which
    /// PBR block a material actually carried — it reports metallic-roughness
    /// factors for every glTF material whether the file stated them or not —
    /// so the source JSON is re-read (once per file, cached) to tell a
    /// spec-gloss, an unlit and a block-less material apart from a real
    /// metallic-roughness one. Empty keeps the assimp-only reading.
    static void extractMaterialData(const aiScene *scene,
                                    aiMaterial *aiMat,
                                    QString assetPath,
                                    MeshMaterialData& mat,
                                    const QString &writeDir = QString(),
                                    const QString &sourceFile = QString());

    /// KHR_materials_pbrSpecularGlossiness → metallic-roughness, the
    /// conversion published in the extension's own appendix (the same one
    /// Blender and three.js run). `diffuse` is RGBA, `specular` RGB, all in
    /// 0..1 linear; the alpha rides through onto the base colour.
    /// Public because the import suite asserts the formula on known inputs.
    static void specularGlossinessToMetallicRoughness(const float diffuse[4],
                                                      const float specular[3],
                                                      float glossiness,
                                                      QColor& baseColorOut,
                                                      float& metallicOut,
                                                      float& roughnessOut);
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

    /// Registers the withdrawal of `name`'s containment warning, conditional on
    /// `extractedPath` existing when the writes are settled, and applies the
    /// registered withdrawals whose bytes really landed. PUBLIC for the suite
    /// that pins the two rules this pair exists for (importer.security §1b):
    /// the withdrawal is EXACT (never a substring match on the texture name)
    /// and EARNED (never before the bytes are on disk).
    static void registerRetraction(const QString &name, const QString &extractedPath);
    static void settleRetractions();

    // A containment warning is WITHDRAWN when the reference turns out to be
    // resolvable after all (AV1, 2026-09-13). A Mixamo "with skin" FBX names
    // its maps by the EXPORTER's temp directory
    // ("../../../../home/app/mixamo-mini/tmp/skins_….fbm/Ch47_1001_Diffuse.png")
    // and carries the bytes EMBEDDED: containment correctly refuses the path,
    // the embedded lookup then finds the media by short name and extracts it,
    // and the import used to report five scary "the reference was dropped"
    // warnings for textures that are on the character.
    //
    // Withdrawal is EXACT and EARNED, both deliberately (lead review, AV1
    // round 2): the warning to remove is rebuilt from the same two message
    // templates containedTexturePath wrote (a substring match on the texture
    // NAME would delete another texture's warning whenever one model's escaped
    // path ends in another's base name), and it is removed only once the
    // extracted bytes are ON DISK — a texture whose write failed is exactly
    // the case the user must still be told about. So the retraction is
    // REGISTERED during extraction and SETTLED after the writes are joined.

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

    /// Joins THIS THREAD's texture writes (the task list is thread-local: two
    /// parses running side by side — an Assets-page import and the Avatar
    /// module's preview parse — used to drain each other's futures through one
    /// process-wide list and each could return with the other's files still
    /// half-written).
    static void waitForAllTextureSaves();

    /// The containment warning sink (thread-local; see takeContainmentWarnings).
    static QStringList& warningSink();

    /// The two messages containedTexturePath can write, built in ONE place so
    /// a retraction can name the exact string it withdraws.
    static QString containmentFallbackWarning(const QString &kind, const QString &name,
                                              const QString &base);
    static QString containmentDroppedWarning(const QString &kind, const QString &name);

    struct SaveTask {
        QFuture<void> future;
        QString path;
    };

    /// THREAD-LOCAL, one queue per parse (see the .cpp): the write queue and
    /// the already-written set.
    static QVector<SaveTask> &textureSaveTasks();
    static QSet<QString> &savedTexturePaths();

    static void saveTextureAsync(const QImage& image, const QString& path);
    /// Verbatim byte write for embedded compressed textures — no re-encode,
    /// so the bytes on disk always match the (sniffed) extension.
    static void saveTextureBytesAsync(const QByteArray& bytes, const QString& path);

    static QThreadPool* g_threadPool;

};

}

#endif // MATERIALHELPER_H
