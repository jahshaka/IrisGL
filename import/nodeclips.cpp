/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/nodeclips.h"

#include "core/math/quat.h"
#include "document/animation/animation.h"
#include "document/animation/keyframeanimation.h"
#include "document/animation/propertyanim.h"
#include "document/animation/skeletalanimation.h"
#include "document/assets/skeleton.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/scenenode.h"

#include <QHash>
#include <QSet>
#include <cmath>
#include <functional>

namespace iris
{

namespace {

/// One component track of a property animation from a channel's keys.
void addVecTrack(Animation &anim, const char *property, const Vector3DKeyFrame &keys)
{
    if (keys.keys.isEmpty()) return;
    auto *track = new Vector3DPropertyAnim();
    track->setName(QString::fromLatin1(property));
    for (const auto *k : keys.keys)
        for (int c = 0; c < 3; ++c) track->getKeyFrame(c)->addKey(k->value[c], k->time);
    anim.addPropertyAnim(track);
}

/// The rotation track, in the Euler degrees SceneNode::updateAnimation composes with
/// Quat::fromEulerAngles — each key's angles unwrapped to the previous key's, so a
/// linear blend never takes the long way round through +-180.
void addRotTrack(Animation &anim, const QuaternionKeyFrame &keys)
{
    if (keys.keys.isEmpty()) return;
    auto *track = new Vector3DPropertyAnim();
    track->setName(QStringLiteral("rotation"));
    Vec3 prev;
    bool first = true;
    for (const auto *k : keys.keys) {
        Vec3 e = k->value.normalized().toEulerAngles();
        if (!first)
            for (int c = 0; c < 3; ++c) {
                while (e[c] - prev[c] > 180.0f) e[c] -= 360.0f;
                while (e[c] - prev[c] < -180.0f) e[c] += 360.0f;
            }
        for (int c = 0; c < 3; ++c) track->getKeyFrame(c)->addKey(e[c], k->time);
        prev = e;
        first = false;
    }
    anim.addPropertyAnim(track);
}

}   // namespace

PlayedNodeChannels playNodeChannels(const SceneNodePtr &fragment)
{
    PlayedNodeChannels out;
    if (!fragment || fragment->getAnimations().isEmpty()) return out;

    // The rigs' bones (a channel naming one belongs to the engine's skeleton) and the
    // fragment's nodes by name, first in depth order.
    QSet<QString> bones;
    QHash<QString, SceneNodePtr> byName;
    std::function<void(const SceneNodePtr &)> walk = [&](const SceneNodePtr &n) {
        if (n->getSceneNodeType() == SceneNodeType::Mesh) {
            const auto mesh = n.staticCast<MeshNode>();
            if (!mesh->skeleton.isNull())
                for (const auto &bone : mesh->skeleton->bones)
                    if (bone) bones.insert(bone->name);
        }
        if (!byName.contains(n->getName())) byName.insert(n->getName(), n);
        for (const auto &c : n->children()) walk(c);
    };
    walk(fragment);

    const AnimationPtr active = fragment->getAnimation();
    QSet<SceneNode *> given;
    for (const AnimationPtr &clip : fragment->getAnimations()) {
        if (!clip || clip->skeletalAnimation.isNull()) continue;
        bool keysARig = false;
        const auto &channels = clip->skeletalAnimation->boneAnimations;
        for (auto it = channels.constBegin(); it != channels.constEnd(); ++it) {
            if (bones.contains(it.key())) { keysARig = true; continue; }
            const BoneAnimation *channel = it.value().data();
            const SceneNodePtr node = byName.value(it.key());
            if (!channel || !node || node == fragment || !channel->moves()) continue;
            auto anim = Animation::create(clip->getName());
            addVecTrack(*anim, "position", *channel->posKeys);
            addRotTrack(*anim, *channel->rotKeys);
            addVecTrack(*anim, "scale", *channel->scaleKeys);
            anim->setLooping(true);
            anim->calculateAnimationLength();
            node->addAnimation(anim);
            if (clip == active || !node->getAnimation()) node->setAnimation(anim);
            given.insert(node.data());
        }
        if (!keysARig) clip->setSkeletalAnimation(SkeletalAnimationPtr());   // marked for removal
    }
    // The clips that keyed no rig leave the root: their channels play on their nodes.
    for (const AnimationPtr &clip : fragment->getAnimations()) {
        if (!clip || !clip->skeletalAnimation.isNull() || !clip->properties.isEmpty()) continue;
        if (fragment->getAnimation() == clip) fragment->setAnimation(AnimationPtr());
        fragment->deleteAnimation(clip);
        ++out.clipsRemoved;
    }
    if (!fragment->getAnimation() && !fragment->getAnimations().isEmpty())
        fragment->setAnimation(fragment->getAnimations().first());
    out.nodes = given.size();
    return out;
}

}   // namespace iris
