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
    /// The file the keys were READ from this session (provenance and logs
    /// only). Never a reference: it is a path on this machine, in this data
    /// root, and is not persisted.
    QString source;
    /// THE REFERENCE (CLIP-REF-1): the guid of the stored asset the keys come
    /// out of — a model's own row for its embedded clips, an animation row for
    /// a clip file. A scene persists {assetGuid, name} and nothing else, and
    /// the reader derives the path from the guid through the store, so the
    /// clip survives a project switch, a moved data root and a re-save.
    /// Empty = a clip no stored asset backs (it cannot be persisted).
    QString assetGuid;

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

    /// `anim` referenced as asset `guid`. The extracted clip objects are
    /// SHARED (one per bake, cached by path), and identical bytes imported
    /// twice are two assets backed by one store object: the first reference
    /// claims the shared object, any OTHER guid gets a shallow copy (the bone
    /// channels stay shared), so one asset's reference never rewrites
    /// another's.
    static SkeletalAnimationPtr referencedAs(const SkeletalAnimationPtr &anim,
                                             const QString &guid);
};

}

#endif // SKELETALANIMATION_H
