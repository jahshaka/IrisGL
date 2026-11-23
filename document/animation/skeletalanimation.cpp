#include "document/animation/skeletalanimation.h"
#include "document/animation/keyframeanimation.h"
#include <algorithm>

namespace iris {

void SkeletalAnimation::addBoneAnimation(std::string boneName, iris::BoneAnimation *boneAnim)
{
    boneAnimations.insert_or_assign(boneName, std::shared_ptr<BoneAnimation>(boneAnim));
}

SkeletalAnimationPtr SkeletalAnimation::referencedAs(const SkeletalAnimationPtr &anim,
                                                    const std::string &guid)
{
    if (!anim) return anim;
    if (anim->assetGuid.empty() || anim->assetGuid == guid) {
        anim->assetGuid = guid;
        return anim;
    }
    auto copy = create();
    copy->name = anim->name;
    copy->source = anim->source;
    copy->assetGuid = guid;
    copy->declaredLength = anim->declaredLength;
    copy->boneAnimations = anim->boneAnimations;
    return copy;
}

namespace {
template <typename KF, typename Same>
bool trackMoves(const KF *track, Same same)
{
    if (!track || track->keys.size() < 2) return false;
    const auto &first = track->keys[0]->value;
    for (int i = 1; i < track->keys.size(); ++i)
        if (!same(track->keys[i]->value, first)) return true;
    return false;
}
}   // namespace

bool BoneAnimation::moves() const
{
    const auto vecSame = [](const iris::Vec3 &a, const iris::Vec3 &b) { return a == b; };
    // q and -q are one rotation.
    const auto quatSame = [](const iris::Quat &a, const iris::Quat &b) { return a == b || a == -b; };
    return trackMoves(posKeys.get(), vecSame) || trackMoves(scaleKeys.get(), vecSame)
        || trackMoves(rotKeys.get(), quatSame);
}

bool SkeletalAnimation::movesNode(const std::string &nodeName) const
{
    const auto it = boneAnimations.find(nodeName);
    return it != boneAnimations.cend() && !!it->second && it->second->moves();
}

bool SkeletalAnimation::movesAnything() const
{
    for (auto it = boneAnimations.cbegin(); it != boneAnimations.cend(); ++it)
        if (!!it->second && it->second->moves()) return true;
    return false;
}

float BoneAnimation::getLength()
{
    float maxLength = 0;
    maxLength = posKeys->getLength();
    maxLength = std::max(maxLength, rotKeys->getLength());
    return std::max(maxLength, scaleKeys->getLength());
}

}
