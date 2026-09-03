#include "irisglfwd.h"
#include "document/assets/skeleton.h"
#include <functional>

namespace iris
{

BonePtr Skeleton::getBone(QString name)
{
    if (boneMap.contains(name))
        return bones[boneMap[name]];
    return BonePtr();// null
}

SkeletonPtr Skeleton::clone() const
{
    auto copy = Skeleton::create();
    // Pass 1: fresh Bone objects, same immutable data, same INDEX ORDER (the
    // bone index is what the mesh's per-vertex blend indices name — reordering
    // here would silently re-target every weight).
    for (const auto &src : bones) {
        auto b = Bone::create(src->name);
        b->inverseMeshSpacePoseMatrix = src->inverseMeshSpacePoseMatrix;
        b->meshSpacePoseMatrix        = src->meshSpacePoseMatrix;
        b->transformMatrix            = src->transformMatrix;
        b->localMatrix                = src->localMatrix;
        b->pos = src->pos; b->scale = src->scale; b->rot = src->rot;
        b->bindingPos = src->bindingPos; b->bindingScale = src->bindingScale;
        b->bindingRot = src->bindingRot;
        b->skinMatrix = src->skinMatrix;
        copy->addBone(b);
    }
    // Pass 2: rebuild the hierarchy by index, so the clone's links point at the
    // clone's bones. (addChild sets parentBone, so only the child walk is needed.)
    for (int i = 0; i < bones.size(); ++i) {
        for (const auto &child : bones[i]->childBones) {
            const auto it = copy->boneMap.constFind(child->name);
            if (it != copy->boneMap.constEnd())
                copy->bones[i]->addChild(copy->bones[it.value()]);
        }
    }
    return copy;
}


}
