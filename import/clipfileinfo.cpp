/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/clipfileinfo.h"

#include <QMap>
#include <functional>
#include <type_traits>

#include "assimp/Importer.hpp"
#include "assimp/anim.h"
#include "assimp/scene.h"

#include "import/importflags.h"

namespace iris
{

namespace {

/// Interpolated local transform of one animated node at `tick` — the
/// importer's own key types and slerp, moved verbatim from Studio's
/// animationfile.cpp so the strip's poses are the poses it always drew.
aiMatrix4x4 sampleChannel(const aiNodeAnim *channel, double tick)
{
    auto lerpKey = [&](auto *keys, unsigned count, auto fallback) {
        using KeyType = decltype(fallback);
        if (count == 0) return fallback;
        if (count == 1 || tick <= keys[0].mTime) return KeyType(keys[0].mValue);
        for (unsigned i = 1; i < count; ++i) {
            if (tick > keys[i].mTime) continue;
            const double span = keys[i].mTime - keys[i - 1].mTime;
            const float f = span > 0.0 ? float((tick - keys[i - 1].mTime) / span) : 0.0f;
            KeyType a(keys[i - 1].mValue), b(keys[i].mValue);
            if constexpr (std::is_same_v<KeyType, aiQuaternion>) {
                aiQuaternion out;
                aiQuaternion::Interpolate(out, a, b, f);
                out.Normalize();
                return out;
            } else {
                return KeyType(a + (b - a) * f);
            }
        }
        return KeyType(keys[count - 1].mValue);
    };

    const aiVector3D position = lerpKey(channel->mPositionKeys, channel->mNumPositionKeys,
                                        aiVector3D(0, 0, 0));
    const aiQuaternion rotation = lerpKey(channel->mRotationKeys, channel->mNumRotationKeys,
                                          aiQuaternion(1, 0, 0, 0));
    const aiVector3D scale = lerpKey(channel->mScalingKeys, channel->mNumScalingKeys,
                                     aiVector3D(1, 1, 1));

    aiMatrix4x4 out(scale, rotation, position);
    return out;
}

/// The hierarchy, pre-order, and one joint position per node at `tick`.
void samplePose(const aiScene *scene, const aiAnimation *anim, double tick,
                ClipFileInfo::Pose &pose)
{
    QMap<QString, const aiNodeAnim *> channels;
    for (unsigned i = 0; i < anim->mNumChannels; ++i)
        channels.insert(QString::fromUtf8(anim->mChannels[i]->mNodeName.C_Str()),
                        anim->mChannels[i]);

    std::function<void(const aiNode *, const aiMatrix4x4 &)> walk =
        [&](const aiNode *node, const aiMatrix4x4 &parentXform) {
            if (!node) return;
            const QString name = QString::fromUtf8(node->mName.C_Str());
            const auto channel = channels.constFind(name);
            const aiMatrix4x4 local = channel != channels.constEnd()
                                          ? sampleChannel(*channel, tick)
                                          : node->mTransformation;
            const aiMatrix4x4 global = parentXform * local;
            pose.positions.append(QVector3D(global.a4, global.b4, global.c4));
            for (unsigned i = 0; i < node->mNumChildren; ++i)
                walk(node->mChildren[i], global);
        };
    walk(scene->mRootNode, aiMatrix4x4());
}

void listHierarchy(const aiNode *node, int parent, ClipFileInfo &out)
{
    if (!node) return;
    const int index = out.nodeNames.size();
    out.nodeNames.append(QString::fromUtf8(node->mName.C_Str()));
    out.nodeParents.append(parent);
    for (unsigned i = 0; i < node->mNumChildren; ++i)
        listHierarchy(node->mChildren[i], index, out);
}

} // namespace

ClipFileInfo ClipFileInfo::read(const QString &filePath, const QVector<double> &poseFractions)
{
    ClipFileInfo out;
    Assimp::Importer importer;
    const aiScene *scene =
        importer.ReadFile(filePath.toStdString().c_str(), ImportFlags::ClipNamesOnly);
    if (!scene) {
        out.error = QString::fromUtf8(importer.GetErrorString());
        if (out.error.isEmpty()) out.error = QStringLiteral("the file could not be read");
        return out;
    }
    out.parsed = true;
    out.meshes = int(scene->mNumMeshes);
    out.animations = int(scene->mNumAnimations);

    for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
        const aiAnimation *anim = scene->mAnimations[i];
        if (!anim) continue;
        Clip clip;
        clip.name = QString::fromUtf8(anim->mName.C_Str());
        clip.ticksPerSecond = anim->mTicksPerSecond;
        clip.durationTicks = anim->mDuration;
        // ticksPerSecond is 0 in more exports than not — the same fallback
        // Mesh::extractAnimations uses.
        const double tps = anim->mTicksPerSecond > 0.0 ? anim->mTicksPerSecond : 25.0;
        clip.lengthSeconds = anim->mDuration / tps;
        for (unsigned c = 0; c < anim->mNumChannels; ++c)
            clip.channelNames.append(QString::fromUtf8(anim->mChannels[c]->mNodeName.C_Str()));
        out.clips.append(clip);
    }

    listHierarchy(scene->mRootNode, -1, out);

    if (poseFractions.isEmpty() || scene->mNumAnimations == 0 || !scene->mRootNode) return out;
    const aiAnimation *anim = scene->mAnimations[0];
    if (!anim || anim->mNumChannels == 0 || anim->mDuration <= 0.0) return out;
    for (double fraction : poseFractions) {
        Pose pose;
        pose.positions.reserve(out.nodeNames.size());
        samplePose(scene, anim, anim->mDuration * fraction, pose);
        out.poses.append(pose);
    }
    return out;
}

} // namespace iris
