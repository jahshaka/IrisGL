#include "../irisglfwd.h"
#include "skeleton.h"
#include "../animation/skeletalanimation.h"
#include <functional>

namespace iris
{

BonePtr Skeleton::getBone(QString name)
{
    if (boneMap.contains(name))
        return bones[boneMap[name]];
    return BonePtr();// null
}

void Skeleton::applyAnimation(iris::SkeletalAnimationPtr anim, float time)
{
	/*
    for (auto i = 0; i < bones.size(); i++) {
        bones[i]->transformMatrix.setToIdentity();
        bones[i]->localMatrix.setToIdentity();
        bones[i]->skinMatrix.setToIdentity();
    }*/
	// recursively update the animation for each bone
	std::function<void(SkeletalAnimationPtr anim, iris::BonePtr bone)> animateHierarchy;
	animateHierarchy = [&animateHierarchy, time](SkeletalAnimationPtr anim, iris::BonePtr bone)
	{
		// skeleton-space transform of current node
		QMatrix4x4 skelTrans;
		skelTrans.setToIdentity();

		if (anim->boneAnimations.contains(bone->name)) {
			auto boneAnim = anim->boneAnimations[bone->name];

			auto pos = boneAnim->posKeys->getValueAt(time);
			auto rot = boneAnim->rotKeys->getValueAt(time);
			auto scale = boneAnim->scaleKeys->getValueAt(time);

			bone->localMatrix.setToIdentity();
			bone->localMatrix.translate(pos);
			bone->localMatrix.rotate(rot);
			bone->localMatrix.scale(scale);
		}
		else {
			// use original transform
			bone->localMatrix.setToIdentity();
			bone->localMatrix.translate(bone->bindingPos);
			bone->localMatrix.rotate(bone->bindingRot);
			bone->localMatrix.scale(bone->bindingScale);
		}

		if (!!bone->parentBone) {
			bone->transformMatrix = bone->parentBone->transformMatrix * bone->localMatrix;
		}
		else {
			bone->transformMatrix = bone->localMatrix;
		}

		for (auto child : bone->childBones) {
			animateHierarchy(anim, child);
		}
	};
	
    auto roots = this->getRootBones();
    for (auto rootBone : roots) {
        rootBone->transformMatrix = rootBone->localMatrix;
		
		animateHierarchy(anim, rootBone);
    }

}

// https://github.com/acgessler/open3mod/blob/master/open3mod/SceneAnimator.cs#L338
void Skeleton::applyAnimation(QMatrix4x4 inverseMeshMatrix, QMap<QString, QMatrix4x4> skeletonSpaceMatrices)
{
    for (auto i = 0; i < bones.size(); i++) {
        auto bone = bones[i];
        if (skeletonSpaceMatrices.contains(bone->name)) {
            //bones->transformMatrix.setToIdentity();
            //bones->localMatrix.setToIdentity();

            // https://github.com/acgessler/open3mod/blob/master/open3mod/SceneAnimator.cs#L356
			//					(to mesh space)		<-	 (to skeleton space)		<-	(to bone space)
            bone->skinMatrix = inverseMeshMatrix * skeletonSpaceMatrices[bone->name] * bone->inverseMeshSpacePoseMatrix;
        }
        else {
            bone->skinMatrix.setToIdentity();
        }
        boneTransforms[i] = bone->skinMatrix;
    }
}

}
