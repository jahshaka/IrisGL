#ifndef SKELETON_H
#define SKELETON_H

#include "../irisglfwd.h"
#include <Qt>
#include <QMatrix4x4>

namespace iris
{

/*
A Note on the Bone's matrices:
In Assimp, Meshes' root transform starts at arbitrary bones in the hierarchy.
Thus, the inverseMeshSpacePoseMatrix (and the meshSpacePoseMatrix) isnt relative to the root of the skeleton, but the
bone in the hierarchy that represents the mesh's root.
*/
class Bone : public QEnableSharedFromThis<Bone>
{
    Bone(){}
public:
    QString name;
    QMatrix4x4 inverseMeshSpacePoseMatrix;// mesh space
    QMatrix4x4 meshSpacePoseMatrix;// mesh space
    QMatrix4x4 transformMatrix;// skeleton space

    QMatrix4x4 localMatrix;// local space (to parent bone)

	// local to bone's parent
	QVector3D pos, scale;
	QQuaternion rot;

	// binding local transform of object
	// these arent changed throughout the lifetime of the bone
	QVector3D bindingPos, bindingScale;
	QQuaternion bindingRot;

    QMatrix4x4 skinMatrix;// final transform sent to the shader

    QList<BonePtr> childBones;
    BonePtr parentBone;

    void addChild(BonePtr bone)
    {
        bone->parentBone = this->sharedFromThis();
        childBones.append(bone);
    }

    static BonePtr create(QString name = "")
    {
        auto bone = new Bone();
        bone->name = name;
        bone->inverseMeshSpacePoseMatrix.setToIdentity();
        bone->meshSpacePoseMatrix.setToIdentity();
        bone->transformMatrix.setToIdentity();
        bone->localMatrix.setToIdentity();
        bone->skinMatrix.setToIdentity();
        return BonePtr(bone);
    }
};

class Skeleton
{
    Skeleton(){}
public:
    QMap<QString, int> boneMap;
    QList<BonePtr> bones;

    BonePtr getBone(QString name);
    QVector<QMatrix4x4> boneTransforms;

    void addBone(BonePtr bone)
    {
        bones.append(bone);
        boneMap.insert(bone->name, bones.size()-1);

        QMatrix4x4 transform;
        transform.setToIdentity();
        boneTransforms.append(transform);
    }

    BonePtr getRootBone()
    {
        for(auto bone : bones)
            if(!bone->parentBone)
                return bone;
    }

    QList<BonePtr> getRootBones()
    {
        QList<BonePtr> roots;
        for(auto bone : bones)
            if(!bone->parentBone)
                roots.append(bone);

        return roots;
    }

    void applyAnimation(SkeletalAnimationPtr anim, float time);

    void applyAnimation(QMatrix4x4 inverseMeshMatrix, QMap<QString, QMatrix4x4> skeletonSpaceMatrices);

    static SkeletonPtr create()
    {
        return SkeletonPtr(new Skeleton());
    }
};



}
#endif // SKELETON_H
