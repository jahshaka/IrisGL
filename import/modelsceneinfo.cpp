/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/modelsceneinfo.h"

#include <QSet>
#include <algorithm>
#include <functional>
#include <limits>
#include <vector>

#include "assimp/Importer.hpp"
#include "assimp/material.h"
#include "assimp/scene.h"
#include "assimp/version.h"

#include "import/clipnaming.h"
#include "import/parsecensus.h"
#include "import/scenesource.h"

namespace iris
{

namespace {

/// Mesh-local AABBs through each instancing node's accumulated transform.
/// Moved verbatim from Studio's assetmetadata.cpp (measureSceneExtent): the
/// float arithmetic is the number every recorded fit was computed with.
/// BOTH CORNERS: the metadata block wants the size, the import dialog's origin
/// helpers want to know where the box sits (import/modelsceneinfo.h).
bool measureBox(const aiScene *scene, double lo[3], double hi[3])
{
    if (!scene || scene->mNumMeshes == 0 || !scene->mRootNode) return false;

    // Per-mesh local AABB, computed once even when several nodes instance it.
    struct Box { aiVector3D mn, mx; bool any = false; };
    std::vector<Box> boxes(scene->mNumMeshes);
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *mesh = scene->mMeshes[i];
        if (!mesh || mesh->mNumVertices == 0) continue;
        Box &box = boxes[i];
        for (unsigned v = 0; v < mesh->mNumVertices; ++v) {
            const aiVector3D &p = mesh->mVertices[v];
            if (!box.any) { box.mn = box.mx = p; box.any = true; continue; }
            box.mn.x = std::min(box.mn.x, p.x); box.mx.x = std::max(box.mx.x, p.x);
            box.mn.y = std::min(box.mn.y, p.y); box.mx.y = std::max(box.mx.y, p.y);
            box.mn.z = std::min(box.mn.z, p.z); box.mx.z = std::max(box.mx.z, p.z);
        }
    }

    for (int a = 0; a < 3; ++a) {
        lo[a] = std::numeric_limits<double>::max();
        hi[a] = -std::numeric_limits<double>::max();
    }
    bool any = false;

    std::function<void(const aiNode *, const aiMatrix4x4 &)> walk =
        [&](const aiNode *node, const aiMatrix4x4 &parent) {
            if (!node) return;
            const aiMatrix4x4 world = parent * node->mTransformation;
            for (unsigned m = 0; m < node->mNumMeshes; ++m) {
                const unsigned index = node->mMeshes[m];
                if (index >= boxes.size() || !boxes[index].any) continue;
                const Box &box = boxes[index];
                for (int c = 0; c < 8; ++c) {
                    aiVector3D corner((c & 1) ? box.mx.x : box.mn.x,
                                      (c & 2) ? box.mx.y : box.mn.y,
                                      (c & 4) ? box.mx.z : box.mn.z);
                    corner *= world;
                    const double p[3] = { corner.x, corner.y, corner.z };
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = std::min(lo[a], p[a]);
                        hi[a] = std::max(hi[a], p[a]);
                    }
                    any = true;
                }
            }
            for (unsigned c = 0; c < node->mNumChildren; ++c) walk(node->mChildren[c], world);
        };
    walk(scene->mRootNode, aiMatrix4x4());

    return any;
}

/// The size half of the box, which is what the metadata block records.
void measureExtent(const aiScene *scene, ModelSceneInfo &out)
{
    double lo[3], hi[3];
    if (!measureBox(scene, lo, hi)) return;
    out.extentX = hi[0] - lo[0];
    out.extentY = hi[1] - lo[1];
    out.extentZ = hi[2] - lo[2];
    out.extentValid = out.extentX > 0.0 || out.extentY > 0.0 || out.extentZ > 0.0;
}

/// FBX is the only format in the set that declares a unit
/// (GlobalSettings::UnitScaleFactor, centimetres per unit); glTF fixes the
/// metre by spec and .obj/.ply/.stl declare nothing, so they read 1.
double declaredUnitScaleOf(const aiScene *scene)
{
    if (!scene || !scene->mMetaData) return 1.0;
    double factor = 0.0;
    if (scene->mMetaData->Get("UnitScaleFactor", factor) && factor > 0.0)
        return factor / 100.0;
    float ffactor = 0.0f;
    if (scene->mMetaData->Get("UnitScaleFactor", ffactor) && ffactor > 0.0f)
        return double(ffactor) / 100.0;
    return 1.0;
}

ModelSceneInfo describe(const aiScene *scene)
{
    ModelSceneInfo out;
    if (!scene) return out;
    out.parsed = true;

    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        out.vertices += scene->mMeshes[i]->mNumVertices;
        out.triangles += scene->mMeshes[i]->mNumFaces;
    }
    out.meshes = int(scene->mNumMeshes);
    out.materials = int(scene->mNumMaterials);

    QSet<QString> textureSeen;
    for (unsigned m = 0; m < scene->mNumMaterials; ++m) {
        for (int t = aiTextureType_DIFFUSE; t <= aiTextureType_UNKNOWN; ++t) {
            const auto type = static_cast<aiTextureType>(t);
            const unsigned count = scene->mMaterials[m]->GetTextureCount(type);
            for (unsigned s = 0; s < count; ++s) {
                aiString path;
                if (scene->mMaterials[m]->GetTexture(type, s, &path) != AI_SUCCESS) continue;
                const QString reference = QString::fromUtf8(path.C_Str());
                if (textureSeen.contains(reference)) continue;
                textureSeen.insert(reference);
                out.textureReferences.append(reference);
            }
        }
    }
    out.embeddedTextures = int(scene->mNumTextures);

    QSet<QString> boneSeen;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *mesh = scene->mMeshes[i];
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const QString name = QString::fromUtf8(mesh->mBones[b]->mName.C_Str());
            if (name.isEmpty() || boneSeen.contains(name)) continue;
            boneSeen.insert(name);
            out.boneNames.append(name);
        }
    }

    QSet<QString> nodeSeen;
    std::function<void(const aiNode *)> walk = [&](const aiNode *node) {
        if (!node) return;
        const QString name = QString::fromUtf8(node->mName.C_Str());
        if (!name.isEmpty() && !nodeSeen.contains(name)) {
            nodeSeen.insert(name);
            out.nodeNames.append(name);
        }
        for (unsigned i = 0; i < node->mNumChildren; ++i) walk(node->mChildren[i]);
    };
    walk(scene->mRootNode);

    for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
        const aiAnimation *anim = scene->mAnimations[i];
        if (!anim) continue;
        ModelSceneInfo::Animation clip;
        clip.name = QString::fromUtf8(anim->mName.C_Str());
        clip.ticksPerSecond = anim->mTicksPerSecond;
        clip.durationTicks = anim->mDuration;
        const double tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
        clip.lengthSeconds = anim->mDuration / tps;
        for (unsigned c = 0; c < anim->mNumChannels; ++c)
            clip.channelNames.append(QString::fromUtf8(anim->mChannels[c]->mNodeName.C_Str()));
        out.animations.append(clip);
    }

    measureExtent(scene, out);
    out.declaredUnitScale = declaredUnitScaleOf(scene);
    return out;
}

} // namespace

ModelSceneInfo ModelSceneInfo::fromSource(const SceneSource &source)
{
    ModelSceneInfo out = describe(source.scene());
    if (!out.parsed) out.error = source.errorString();
    return out;
}

double ModelSceneInfo::readDeclaredUnitScale(const QString &filePath)
{
    // THE LIGHT PARSE: no post-processing at all, which is what makes it the
    // cheap half of a read (the clip parsers measured it at ~3x faster than
    // the canonical preset — import/importflags.h). It exists for ONE caller:
    // readSceneFile resolving a UNIT OVERRIDE, where assimp has to be told a
    // factor BEFORE it parses and the factor depends on what the file
    // declares. A file that cannot be read declares nothing: 1 metre per unit.
    // Through the choke point like every other parse (identity transform, no
    // flags): readSceneFile only ever calls back into here for a UNIT
    // OVERRIDE, which an identity transform is not, so there is no recursion.
    Assimp::Importer importer;
    const aiScene *scene = readSceneFile(importer, filePath, 0u);
    if (!scene) return 1.0;
    return declaredUnitScaleOf(scene);
}

ModelPreRead ModelPreRead::read(const QString &filePath, const QString &formatHint)
{
    ModelPreRead out;

    // THE LIGHT PARSE — no post-processing flags at all, so no triangulation,
    // no tangents and NO aiProcess_GlobalScale: the vertices below are in the
    // file's own units and `declaredUnitScale` is what the file says one of
    // them is worth. The header says why the dialog wants them apart.
    // Through the choke point with an identity transform, like every parse in
    // this library (tests/hygiene/one_readfile.sh).
    Assimp::Importer importer;
    const aiScene *scene = readSceneFile(importer, filePath, 0u, ImportTransform(), formatHint);
    if (!scene) {
        out.error = QString::fromUtf8(importer.GetErrorString());
        if (out.error.isEmpty()) out.error = QStringLiteral("the file could not be read");
        return out;
    }

    out.parsed = true;
    out.declaredUnitScale = declaredUnitScaleOf(scene);
    out.meshes = int(scene->mNumMeshes);
    out.materials = int(scene->mNumMaterials);

    double lo[3], hi[3];
    if (measureBox(scene, lo, hi)) {
        for (int a = 0; a < 3; ++a) { out.aabbMin[a] = lo[a]; out.aabbMax[a] = hi[a]; }
        out.aabbValid = hi[0] > lo[0] || hi[1] > lo[1] || hi[2] > lo[2];
    }

    QSet<QString> boneSeen;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh *mesh = scene->mMeshes[i];
        if (!mesh) continue;
        for (unsigned b = 0; b < mesh->mNumBones; ++b) {
            const QString name = QString::fromUtf8(mesh->mBones[b]->mName.C_Str());
            if (name.isEmpty() || boneSeen.contains(name)) continue;
            boneSeen.insert(name);
        }
    }
    out.bones = boneSeen.size();

    // THE NAMES THE IMPORT WOULD PRODUCE, by the one rule both sides share
    // (import/clipnaming.h) — so the dialog's checklist and the `clips`
    // filter can never disagree.
    for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
        const aiAnimation *anim = scene->mAnimations[i];
        if (!anim) continue;
        out.clipNames.append(
            clipNameFor(QString::fromUtf8(anim->mName.C_Str()), i, out.clipNames));
    }
    return out;
}

ModelSceneInfo ModelSceneInfo::read(const QString &filePath, const ImportTransform &xf)
{
    SceneSource source;
    if (!source.read(filePath, xf)) {
        ModelSceneInfo out;
        out.error = source.errorString();
        if (out.error.isEmpty()) out.error = QStringLiteral("the file could not be read");
        return out;
    }
    return fromSource(source);
}

QString ModelSceneInfo::importerVersion()
{
    return QStringLiteral("%1.%2.%3")
        .arg(aiGetVersionMajor())
        .arg(aiGetVersionMinor())
        .arg(aiGetVersionRevision());
}

} // namespace iris
