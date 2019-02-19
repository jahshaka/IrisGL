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
    for (auto i = 0; i < bones.size(); i++) {
        bones[i]->transformMatrix.setToIdentity();
        bones[i]->localMatrix.setToIdentity();
        bones[i]->skinMatrix.setToIdentity();
    }

	// We dont have the bones in a hierarchy here
	// so we eval the local transform
	/*
	for (auto bone : bones) {
        if (anim->boneAnimations.contains(bone->name))
        {
			auto boneAnim = anim->boneAnimations[bone->name];

            //qDebug()<< boneName <<" verified";
            auto pos = boneAnim->posKeys->getValueAt(time);
            auto rot = boneAnim->rotKeys->getValueAt(time);
            auto scale = boneAnim->scaleKeys->getValueAt(time);


            bone->localMatrix.setToIdentity();
            bone->localMatrix.translate(pos);
            bone->localMatrix.rotate(rot);
            bone->localMatrix.scale(scale);
        }
        else
        {
			// use original transform
			bone->localMatrix.setToIdentity();
			bone->localMatrix.translate(bone->pos);
			bone->localMatrix.rotate(bone->rot);
			bone->localMatrix.scale(bone->scale);
        }
    }
	*/

	// recursively update the animation for each node
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

	//QMatrix4x4 rootTransform;
	//rootTransform.setToIdentity();
	//rootTransform = this->getLocalTransform();
	//animateHierarchy(animation->getSkeletalAnimation(), this->sharedFromThis(), rootTransform);

    //recursively calculate final transform
    std::function<void(BonePtr)> calcFinalTransform;
    calcFinalTransform = [&calcFinalTransform](BonePtr bone)
    {
        for (auto child : bone->childBones) {
            child->transformMatrix = bone->transformMatrix * child->localMatrix;
            //child->skinMatrix = child->transformMatrix * child->inverseMeshSpacePoseMatrix;
            //qDebug() << child->transformMatrix;
            calcFinalTransform(child);
        }
    };

    //auto rootBone = bones[0];//its assumed that the first bone is the root bone
    //auto rootBone = this->getRootBone();
	
    auto roots = this->getRootBones();
    for (auto rootBone : roots) {
        rootBone->transformMatrix = rootBone->localMatrix;
        //rootBone->transformMatrix.setToIdentity();
        //rootBone->skinMatrix = rootBone->transformMatrix * rootBone->inverseMeshSpacePoseMatrix;
        //calcFinalTransform(rootBone);
		
		animateHierarchy(anim, rootBone);
    }
	

    //assign transforms to list
	/*
    for (auto i = 0; i < bones.size(); i++) {
        boneTransforms[i] = bones[i]->skinMatrix;
    }
	*/
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
