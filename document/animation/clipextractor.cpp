/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/animation/clipextractor.h"

#include "core/math/trs.h"
#include "document/animation/keyframeanimation.h"
#include "document/animation/skeletalanimation.h"
#include "document/assets/skeleton.h"
#include "document/scenegraph/scenenode.h"

#include <QMatrix4x4>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace iris
{

namespace
{

/// A key time that two channels agree on must not become two keys because one
/// stored it as 0.4166667 and the other as 0.41666669. Quantised to a
/// microsecond, which is four orders of magnitude finer than any key any
/// exporter emits.
inline long long quantiseTime(float t)
{
    return static_cast<long long>(std::llround(double(t) * 1e6));
}
inline float unquantiseTime(long long q) { return float(double(q) * 1e-6); }

/// Every ancestor of `node` up to and including `root`, nearest first, with
/// `node` itself at the front. Empty if `root` is not an ancestor.
QVector<SceneNode *> pathToRoot(SceneNode *node, SceneNode *root)
{
    QVector<SceneNode *> path;
    for (SceneNode *n = node; n; n = n->getParent().data()) {
        path.append(n);
        if (n == root) return path;
    }
    return QVector<SceneNode *>();
}

} // namespace

ClipExtractor::RestPose ClipExtractor::captureRest(const SceneNodePtr &root)
{
    RestPose out;
    if (root.isNull()) return out;
    QVector<SceneNode *> stack;
    stack.append(root.data());
    while (!stack.isEmpty()) {
        SceneNode *n = stack.takeLast();
        RestLocal r;
        r.pos = n->getLocalPos();
        r.rot = n->getLocalRot();
        r.scale = n->getLocalScale();
        out.insert(n, r);
        for (const auto &child : n->children) stack.append(child.data());
    }
    return out;
}

bool ClipExtractor::extract(const SceneNodePtr &root, const SceneNodePtr &meshNode,
                            const SkeletonPtr &rig, const SkeletalAnimationPtr &clip,
                            const QString &clipName, float length, const RestPose *rest,
                            ExtractedClip &out, QString *error)
{
    const auto fail = [error](const char *why) {
        if (error) *error = QString::fromLatin1(why);
        return false;
    };

    out = ExtractedClip();
    out.name = clipName;
    out.length = length > 0.0f ? length : 0.0f;

    if (root.isNull()) return fail("clip extraction: no subtree root");
    if (meshNode.isNull()) return fail("clip extraction: no mesh node");
    if (rig.isNull() || rig->bones.isEmpty()) return fail("clip extraction: the rig has no bones");
    if (clip.isNull()) return fail("clip extraction: no clip");

    // ---- the subtree, by name -------------------------------------------
    // The document evaluator joins clip channels to scene nodes BY NAME and
    // stores skeleton-space matrices in a QMap keyed by name, so a duplicate
    // name means the last node visited in the depth-first walk wins. Mirrored
    // here exactly, duplicates and all: parity is the point.
    QHash<QString, SceneNode *> byName;
    {
        QVector<SceneNode *> order;
        order.append(root.data());
        for (int i = 0; i < order.size(); ++i) {
            SceneNode *n = order[i];
            byName.insert(n->name, n);
            for (const auto &child : n->children) order.append(child.data());
        }
    }

    // ---- per-node local transform at a time ------------------------------
    const auto localAt = [&](SceneNode *n, float t) -> QMatrix4x4 {
        const auto it = clip->boneAnimations.constFind(n->name);
        if (it != clip->boneAnimations.constEnd() && !it.value().isNull()) {
            // SceneNode::updateAnimation writes all THREE components whenever a
            // channel exists, normalising the quaternion. Not "position if
            // there are position keys" — so a channel with an empty scale track
            // really does zero the scale, in both evaluators.
            const auto &ba = it.value();
            return composeTRS(ba->posKeys->getValueAt(t),
                              ba->rotKeys->getValueAt(t).normalized(),
                              ba->scaleKeys->getValueAt(t));
        }
        if (rest) {
            const auto rit = rest->constFind(n);
            if (rit != rest->constEnd())
                return composeTRS(rit->pos, rit->rot, rit->scale);
        }
        return composeTRS(n->getLocalPos(), n->getLocalRot(), n->getLocalScale());
    };

    // The product of `path[startIndex] … path[0]`, parent first — the path
    // arrives nearest-first, so it is walked backwards.
    const auto chainBelow = [&](const QVector<SceneNode *> &path, int startIndex, float t) {
        QMatrix4x4 m;
        m.setToIdentity();
        for (int i = startIndex; i >= 0; --i) m = m * localAt(path[i], t);
        return m;
    };

    SceneNode *rootNode = root.data();
    const QVector<SceneNode *> meshPath = pathToRoot(meshNode.data(), rootNode);
    if (meshPath.isEmpty()) return fail("clip extraction: the mesh node is not in the subtree");

    const QList<BonePtr> &bones = rig->bones;
    out.tracks.reserve(bones.size());

    for (int i = 0; i < bones.size(); ++i) {
        const BonePtr &bone = bones[i];
        SceneNode *boneNode = byName.value(bone->name, nullptr);
        if (!boneNode) continue;                       // a bone with no scene node moves nothing
        const QVector<SceneNode *> bonePath = pathToRoot(boneNode, rootNode);
        if (bonePath.isEmpty()) continue;

        // The FRAME: the parent bone's node, or — for a root bone — the mesh
        // node, which is the frame SceneMirror::toSkeletonDesc authors root
        // bind locals in (`bindLocal = meshSpacePoseMatrix`, i.e. mesh space).
        int parentIndex = -1;
        if (!bone->parentBone.isNull()) {
            const auto pit = rig->boneMap.constFind(bone->parentBone->name);
            if (pit != rig->boneMap.constEnd() && pit.value() != i) parentIndex = pit.value();
        }
        SceneNode *frameNode = nullptr;
        if (parentIndex >= 0) frameNode = byName.value(bones[parentIndex]->name, nullptr);
        if (!frameNode) frameNode = meshNode.data();
        const QVector<SceneNode *> framePath = pathToRoot(frameNode, rootNode);
        if (framePath.isEmpty()) continue;

        // The two paths share a suffix (at minimum the subtree root itself),
        // and in
        //     local = inverse(path(frame)) * path(bone)
        // that shared prefix-of-the-hierarchy cancels ALGEBRAICALLY. Dropping
        // it is therefore exact, and it is what keeps the relevant channel set
        // small: for a deep Mixamo rig almost the whole path is shared, so a
        // bone's chain is the two or three pivot nodes above it and no more.
        //
        // When the frame IS an ancestor of the bone (every non-root bone) the
        // whole frame path is shared and `frameDepth` comes out -1, i.e. the
        // frame factor is the identity and the composition is just the pivot
        // chain — §3.1's `(p, b]` exactly.
        int frameDepth = framePath.size(), boneDepth = bonePath.size();
        while (frameDepth > 0 && boneDepth > 0 &&
               framePath[frameDepth - 1] == bonePath[boneDepth - 1]) {
            --frameDepth;
            --boneDepth;
        }
        --frameDepth;   // now an INDEX: the deepest node that does not cancel
        --boneDepth;

        // ---- step 1+2: the channel set and its key-time union ------------
        QSet<long long> timeSet;
        int sourceKeys = 0;
        const auto collect = [&](const QVector<SceneNode *> &path, int startIndex) {
            for (int k = startIndex; k >= 0; --k) {
                const auto it = clip->boneAnimations.constFind(path[k]->name);
                if (it == clip->boneAnimations.constEnd() || it.value().isNull()) continue;
                const auto &ba = it.value();
                for (const auto *key : ba->posKeys->keys)   { timeSet.insert(quantiseTime(key->time)); ++sourceKeys; }
                for (const auto *key : ba->rotKeys->keys)   { timeSet.insert(quantiseTime(key->time)); ++sourceKeys; }
                for (const auto *key : ba->scaleKeys->keys) { timeSet.insert(quantiseTime(key->time)); ++sourceKeys; }
            }
        };
        collect(bonePath, boneDepth);
        collect(framePath, frameDepth);

        if (timeSet.isEmpty()) continue;               // this clip does not drive this bone

        // R4 — END-OF-CLIP SEMANTICS. Our keyframes HOLD the last value past
        // the last key (keyframeanimation.h getKeyFramesAtTime); a v1 engine
        // track WRAPS toward keyframe[0]. A bone whose channels end before the
        // clip does would therefore drift back engine-side and hold
        // document-side. Pinning a terminal key at the clip length (and a key
        // at 0) makes the two agree by construction. One line; without it the
        // parity gate fails on exactly the clips a user will test with.
        timeSet.insert(quantiseTime(0.0f));
        if (out.length > 0.0f) timeSet.insert(quantiseTime(out.length));

        QVector<long long> times(timeSet.begin(), timeSet.end());
        std::sort(times.begin(), times.end());

        ClipBoneTrack track;
        track.bone = i;
        track.boneName = bone->name;
        track.keys.reserve(times.size());
        for (long long q : times) {
            const float t = unquantiseTime(q);
            // ---- step 3: compose the chain at t, both frames --------------
            const QMatrix4x4 boneChain  = chainBelow(bonePath, boneDepth, t);
            const QMatrix4x4 frameChain = chainBelow(framePath, frameDepth, t);
            const QMatrix4x4 local = frameChain.inverted() * boneChain;
            // ---- step 4: decompose -----------------------------------------
            ClipBoneKey key;
            key.time = t;
            decomposeTRS(local, key.position, key.rotation, key.scale);
            track.keys.append(key);
        }
        out.sourceKeyCount += sourceKeys;
        out.emittedKeyCount += track.keys.size();
        out.tracks.append(track);
    }

    // ---- rest-vs-bind diagnostic ----------------------------------------
    // (See ExtractedClip::restDiffersFromBind. Computed here because this is
    // the one place that holds the rig, the subtree and the rest pose at once.)
    for (int i = 0; i < bones.size(); ++i) {
        const BonePtr &bone = bones[i];
        SceneNode *boneNode = byName.value(bone->name, nullptr);
        if (!boneNode) continue;
        int parentIndex = -1;
        if (!bone->parentBone.isNull()) {
            const auto pit = rig->boneMap.constFind(bone->parentBone->name);
            if (pit != rig->boneMap.constEnd() && pit.value() != i) parentIndex = pit.value();
        }
        const QMatrix4x4 bindLocal = parentIndex >= 0
            ? bones[parentIndex]->inverseMeshSpacePoseMatrix * bone->meshSpacePoseMatrix
            : bone->meshSpacePoseMatrix;

        SceneNode *frameNode = parentIndex >= 0
            ? byName.value(bones[parentIndex]->name, nullptr) : nullptr;
        if (!frameNode) frameNode = meshNode.data();
        const QVector<SceneNode *> bonePath = pathToRoot(boneNode, rootNode);
        const QVector<SceneNode *> framePath = pathToRoot(frameNode, rootNode);
        if (bonePath.isEmpty() || framePath.isEmpty()) continue;

        // The rest local is the same composition with NO clip driving it.
        const auto restChain = [&](const QVector<SceneNode *> &path) {
            QMatrix4x4 m;
            m.setToIdentity();
            for (int k = path.size() - 1; k >= 0; --k) {
                SceneNode *n = path[k];
                if (rest) {
                    const auto rit = rest->constFind(n);
                    if (rit != rest->constEnd()) {
                        m = m * composeTRS(rit->pos, rit->rot, rit->scale);
                        continue;
                    }
                }
                m = m * composeTRS(n->getLocalPos(), n->getLocalRot(), n->getLocalScale());
            }
            return m;
        };
        const QMatrix4x4 restLocal = restChain(framePath).inverted() * restChain(bonePath);
        float worst = 0.0f;
        for (int c = 0; c < 16; ++c)
            worst = std::max(worst, std::fabs(restLocal.constData()[c] - bindLocal.constData()[c]));
        if (worst > 1e-3f) out.restDiffersFromBind.append(bone->name);
    }

    return true;
}

} // namespace iris
