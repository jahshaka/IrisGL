#include "document/animation/skeletalanimation.h"
#include "document/animation/keyframeanimation.h"

namespace iris {

void SkeletalAnimation::addBoneAnimation(QString boneName, iris::BoneAnimation *boneAnim)
{
    boneAnimations.insert(boneName, QSharedPointer<BoneAnimation>(boneAnim));
}

SkeletalAnimationPtr SkeletalAnimation::referencedAs(const SkeletalAnimationPtr &anim,
                                                    const QString &guid)
{
    if (anim.isNull()) return anim;
    if (anim->assetGuid.isEmpty() || anim->assetGuid == guid) {
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
    return trackMoves(posKeys.data(), vecSame) || trackMoves(scaleKeys.data(), vecSame)
        || trackMoves(rotKeys.data(), quatSame);
}

bool SkeletalAnimation::movesNode(const QString &nodeName) const
{
    const auto it = boneAnimations.constFind(nodeName);
    return it != boneAnimations.constEnd() && !it.value().isNull() && it.value()->moves();
}

bool SkeletalAnimation::movesAnything() const
{
    for (auto it = boneAnimations.constBegin(); it != boneAnimations.constEnd(); ++it)
        if (!it.value().isNull() && it.value()->moves()) return true;
    return false;
}

float BoneAnimation::getLength()
{
    float maxLength = 0;
    maxLength = posKeys->getLength();
    maxLength = qMax(maxLength, rotKeys->getLength());
    return qMax(maxLength, scaleKeys->getLength());
}

}
