#ifndef SKELETALANIMATION_H
#define SKELETALANIMATION_H

#include "irisglfwd.h"
#include "document/animation/keyframeanimation.h"

namespace iris {

// keyframe animation for a single animation for a single bone
class BoneAnimation
{
public:
    QScopedPointer<Vector3DKeyFrame>    posKeys;
    QScopedPointer<QuaternionKeyFrame>  rotKeys;
    QScopedPointer<Vector3DKeyFrame>    scaleKeys;

    BoneAnimation()
        : posKeys(new Vector3DKeyFrame())
        , rotKeys(new QuaternionKeyFrame())
        , scaleKeys(new Vector3DKeyFrame())
    {

    }

    float getLength();
};

class SkeletalAnimation
{
    SkeletalAnimation(){}
public:
    // both read-only
    QString name;
    QString source;

    QMap<QString, QSharedPointer<BoneAnimation>> boneAnimations;

    /// The clip's DECLARED length in seconds — the file's own duration
    /// (assimp's mDuration / mTicksPerSecond), 0 when it declares none.
    ///
    /// The KEYS decide a clip's length (Animation::calculateAnimationLength);
    /// this is consulted only when they span no time at all. That is the
    /// one-frame clip every Mixamo CHARACTER download ships: two identical keys
    /// one frame apart, which the canonical preset's FindInvalidData step
    /// collapses into ONE key at t = 0 while the declared one-frame duration
    /// survives (smoke L10 item 2). Without it the clip was zero seconds long
    /// and the engine padded it on every attach.
    float declaredLength = 0.0f;

    // takes ownership of boneAnim
    void addBoneAnimation(QString boneName,BoneAnimation* boneAnim);

    static SkeletalAnimationPtr create()
    {
        return SkeletalAnimationPtr(new SkeletalAnimation());
    }
};

}

#endif // SKELETALANIMATION_H
