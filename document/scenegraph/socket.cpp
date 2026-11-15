/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/socket.h"

#include "document/assets/skeleton.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"

namespace iris
{

bool bindPoseWorldTransforms(MeshNode *node, QHash<QString, Mat4> &out)
{
    out.clear();
    if (!node) return false;
    const SkeletonPtr skeleton = node->getSkeleton();
    if (skeleton.isNull() || skeleton->bones.isEmpty()) return false;

    // meshSpacePoseMatrix IS the bind pose, already composed down the chain —
    // no FK needed, and no disagreement possible with the engine's bind pose,
    // whose per-bone locals SceneMirror::toSkeletonDesc derives from these same
    // matrices (parentInverse * child) precisely so that FK reproduces them.
    const Mat4 world = node->getGlobalTransform();
    for (const BonePtr &bone : skeleton->bones) {
        if (bone.isNull()) continue;
        out.insert(bone->name, world * bone->meshSpacePoseMatrix);
    }
    return !out.isEmpty();
}

/// The pose for one owner: the injected source when it has one for this node,
/// the bind pose otherwise.
static bool poseFor(MeshNode *owner, const BonePoseSource &source, QHash<QString, Mat4> &bones)
{
    if (source && source(owner, bones) && !bones.isEmpty()) return true;
    return bindPoseWorldTransforms(owner, bones);
}

bool socketWorldTransform(MeshNode *owner, const QString &socketName,
                          const BonePoseSource &source, Mat4 &out)
{
    if (!owner) return false;
    const Socket *socket = owner->findSocket(socketName);
    if (!socket) return false;

    QHash<QString, Mat4> bones;
    if (!poseFor(owner, source, bones)) return false;
    const auto bone = bones.constFind(socket->boneName);
    if (bone == bones.constEnd()) return false;

    out = bone.value() * socket->offsetMatrix();
    return true;
}

int SocketResolver::resolve(Scene *scene)
{
    mDangling = 0;
    if (!scene) return 0;
    const QHash<QString, QList<SceneNodePtr>> &attachments = scene->socketAttachments;
    if (attachments.isEmpty()) return 0;

    int moved = 0;
    // The registry is keyed by OWNER, so a rig's pose is read once however many
    // things hang off it: a character with a first-person camera, a third-person
    // camera and a prop in its hand reads one pose, not three. `mBones` is
    // cleared and refilled in place, so the steady state allocates nothing.
    for (auto it = attachments.constBegin(); it != attachments.constEnd(); ++it) {
        const QList<SceneNodePtr> &attached = it.value();
        if (attached.isEmpty()) continue;

        const SceneNodePtr ownerNode = scene->nodes.value(it.key());
        MeshNode *owner = (!ownerNode.isNull() &&
                           ownerNode->getSceneNodeType() == SceneNodeType::Mesh)
                              ? static_cast<MeshNode *>(ownerNode.data())
                              : nullptr;
        const bool posed = owner && poseFor(owner, mSource, mBones);
        if (!posed) { mDangling += int(attached.size()); continue; }

        for (const SceneNodePtr &node : attached) {
            if (node.isNull()) { ++mDangling; continue; }
            const Socket *socket = owner->findSocket(node->socketName);
            if (!socket) { ++mDangling; continue; }
            const auto bone = mBones.constFind(socket->boneName);
            if (bone == mBones.constEnd()) { ++mDangling; continue; }

            // setGlobalTransform decomposes against the node's CURRENT parent,
            // so an attached node keeps whatever place it has in the hierarchy
            // — being socketed is not being reparented.
            node->setGlobalTransform(bone.value() * socket->offsetMatrix());
            // ...and then refresh the cached local/global that the mirror, the
            // picker and the gizmos read directly. update(0) is the tree's own
            // refresh and cascades to the attached node's own children.
            node->update(0.0f);
            ++moved;
        }
    }
    return moved;
}

}   // namespace iris
