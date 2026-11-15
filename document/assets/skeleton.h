#ifndef SKELETON_H
#define SKELETON_H

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "irisglfwd.h"
#include <Qt>

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
    iris::Mat4 inverseMeshSpacePoseMatrix;// mesh space
    iris::Mat4 meshSpacePoseMatrix;// mesh space
    iris::Mat4 transformMatrix;// skeleton space

    iris::Mat4 localMatrix;// local space (to parent bone)

	// local to bone's parent
	iris::Vec3 pos, scale;
	iris::Quat rot;

	// binding local transform of object
	// these arent changed throughout the lifetime of the bone
	iris::Vec3 bindingPos, bindingScale;
	iris::Quat bindingRot;

    iris::Mat4 skinMatrix;// final transform sent to the shader

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

    void addBone(BonePtr bone)
    {
        bones.append(bone);
        boneMap.insert(bone->name, bones.size()-1);
    }

    BonePtr getRootBone()
    {
        for(auto bone : bones)
            if(!bone->parentBone)
                return bone;
        return BonePtr();  // no parentless bone (empty/cyclic skeleton)
    }

    QList<BonePtr> getRootBones()
    {
        QList<BonePtr> roots;
        for(auto bone : bones)
            if(!bone->parentBone)
                roots.append(bone);

        return roots;
    }

    // NO applyAnimation. Both overloads are gone with the document's clip
    // evaluator (ANIMATION_ENGINE_MIGRATION_SPEC, full retirement): one sampled
    // bone-local keys straight onto the bone hierarchy and was already dead on
    // the live path, the other turned scene-node skeleton-space matrices into
    // the per-bone skin matrices the renderer used. A skeleton is now purely
    // the RIG — names, hierarchy, bind matrices — and the pose lives in the
    // engine's SkeletonInstance, one per node, read back with Scene::bonePoses.
    /// A copy of this rig: same bones, same names, same bind matrices, same
    /// hierarchy — but its OWN Bone objects.
    ///
    /// GPU_SKINNING_SPEC §7: the SkeletonPtr on an iris::Mesh is the rig
    /// TEMPLATE and is shared by every MeshNode that references the mesh asset
    /// (MeshNode::createDuplicate passes the same MeshPtr). Pose state used to
    /// live on it, so two duplicates of one character shared one pose and the
    /// last writer per frame won — multiple avatars of one rig were impossible.
    /// MeshNode::setMesh clones the template per node; the template is never
    /// posed.
    SkeletonPtr clone() const;

    static SkeletonPtr create()
    {
        return SkeletonPtr(new Skeleton());
    }
};


}
#endif // SKELETON_H
