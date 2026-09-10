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

#include "assimp/material.h"
#include "assimp/scene.h"
#include "assimp/version.h"

#include "import/scenesource.h"

namespace iris
{

namespace {

/// Mesh-local AABBs through each instancing node's accumulated transform.
/// Moved verbatim from Studio's assetmetadata.cpp (measureSceneExtent): the
/// float arithmetic is the number every recorded fit was computed with.
void measureExtent(const aiScene *scene, ModelSceneInfo &out)
{
    if (!scene || scene->mNumMeshes == 0 || !scene->mRootNode) return;

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

    double lo[3] = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                     std::numeric_limits<double>::max() };
    double hi[3] = { -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
                     -std::numeric_limits<double>::max() };
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

    if (!any) return;
    out.extentX = hi[0] - lo[0];
    out.extentY = hi[1] - lo[1];
    out.extentZ = hi[2] - lo[2];
    out.extentValid = out.extentX > 0.0 || out.extentY > 0.0 || out.extentZ > 0.0;
}

/// FBX is the only format in the set that declares a unit
/// (GlobalSettings::UnitScaleFactor, centimetres per unit); glTF fixes the
/// metre by spec and .obj/.ply/.stl declare nothing, so they read 1.
double declaredUnitScale(const aiScene *scene)
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
    out.declaredUnitScale = declaredUnitScale(scene);
    return out;
}

} // namespace

ModelSceneInfo ModelSceneInfo::fromSource(const SceneSource &source)
{
    ModelSceneInfo out = describe(source.scene());
    if (!out.parsed) out.error = source.errorString();
    return out;
}

ModelSceneInfo ModelSceneInfo::read(const QString &filePath)
{
    SceneSource source;
    if (!source.read(filePath)) {
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
