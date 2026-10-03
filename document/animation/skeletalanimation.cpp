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

float BoneAnimation::getLength()
{
    float maxLength = 0;
    maxLength = posKeys->getLength();
    maxLength = qMax(maxLength, rotKeys->getLength());
    return qMax(maxLength, scaleKeys->getLength());
}

}
