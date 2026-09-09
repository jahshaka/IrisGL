// BONE ATTACHMENTS — engine TagPoints behind the document's Socket API
// (AVATAR_RIG_PERF_SPEC §4).
//
// WHAT THIS REPLACES. A socketed node (a camera on a head, a sword in a hand)
// used to be moved by the HOST: read the owner's pose back with bonePoses(), run
// FK over it, and write the rider's world transform — once per rigged node with
// a socket, per frame, and ONE FRAME LATE by construction, because the pose read
// back is the one the last rendered frame produced.
//
// Ogre resolves tag points INSIDE its threaded scene update, in the right order
// (updateAllTransforms -> updateAllAnimations -> updateAllTagPoints,
// OgreSceneManager.cpp:2743-2745). So a rider hung off a bone through a TagPoint
// is in the right place in the frame that renders it, at no per-frame cost to us
// at all: the host's job becomes RECONCILIATION (arm the tag when the attachment
// appears, free it when it goes), which is event-driven.
//
// THE RULES OGRE IMPOSES, and where each is honoured here:
//   * a SceneNode may not be a child of a Bone, but a TagPoint may, and a
//     SceneNode may be a child of a TagPoint (OgreTagPoint2.h:44-56). Hence one
//     tag per rider, with the rider's own node hanging under it.
//   * `TagPoint::_setParentBone` THROWS if the tag already has a bone or a scene
//     parent (OgreTagPoint2.cpp:53-73) — so a tag is created, parented to the
//     bone, and only then given the rider.
//   * `Node::setParent` MIGRATES the child and its whole subtree into the
//     parent's NodeMemoryManager (OgreNode.cpp:169-187), which is what makes the
//     rider's own children ride along with zero lag too — and what makes a
//     STATIC rider a contradiction: the tag manager is dynamic, so a static
//     rider is made dynamic on the way in (the document already forbids one).
//   * `TagPoint::updateFromParentImpl` is `assert(false)` (OgreTagPoint2.cpp:
//     108-112): a tag's world transform cannot be resolved on demand between
//     frames. Readers get the LAST RENDERED transform — the same latency the
//     read-back path had — and iris::graph::globalTransform knows it.
//   * a circular dependency (a skeleton attached to a tag on its own bone) is
//     "undefined, probably very wonky" (OgreTagPoint2.h:83-85), so it is refused
//     here rather than discovered as a hang.
//
// TEARDOWN is the other half: a tag points INTO a SkeletonInstance's bone, so
// every path that can take that instance away — the owner's Item re-created, the
// owner sharing or un-sharing a skeleton, the owner's node destroyed — releases
// the riders first, fail-soft, back under the scene root at the pose they last
// rendered with. The host then re-arms them, which is exactly what it does when
// anything else engine-side is rebuilt.
#include "EnginePrivate.h"

#include <Animation/OgreSkeletonInstance.h>
#include <Animation/OgreBone.h>
#include <Animation/OgreTagPoint2.h>

namespace jahshaka { namespace engine { namespace detail {

namespace {

/// True when `maybeAncestor` is `n` or one of its ancestors — the circular-
/// attachment guard (a rider that owns the owner).
bool isAncestorOf(const Ogre::Node *maybeAncestor, const Ogre::Node *n)
{
    for (const Ogre::Node *p = n; p; p = p->getParent())
        if (p == maybeAncestor) return true;
    return false;
}

}  // namespace

bool OgreScene::attachToBone(NodeId riderId, NodeId ownerId, const std::string &bone,
                             const Vec3 &position, const Quat &rotation, const Vec3 &scale)
{
    auto rit = mNodes.find(riderId);
    if (rit == mNodes.end() || !rit->second.node) {
        mError = "attachToBone: unknown rider node";
        return false;
    }
    auto oit = mNodes.find(ownerId);
    if (oit == mNodes.end() || !oit->second.node) {
        mError = "attachToBone: unknown owner node";
        return false;
    }
    if (riderId == ownerId) { mError = "attachToBone: a node cannot ride its own bone"; return false; }
    Node &r = rit->second;
    Node &o = oit->second;
    if (r.node->getCreator() != mSceneMgr || o.node->getCreator() != mSceneMgr) {
        mError = "attachToBone: both nodes must belong to this scene";
        return false;
    }
    Ogre::SkeletonInstance *skel = skeletonOf(ownerId);
    if (!skel) { mError = "attachToBone: the owner has no rig"; return false; }
    const RigRec *rig = rigOf(ownerId);
    if (!rig) { mError = "attachToBone: the owner's rig is unknown to this scene"; return false; }
    size_t boneIndex = rig->boneNames.size();
    for (size_t i = 0; i < rig->boneNames.size(); ++i)
        if (rig->boneNames[i] == bone) { boneIndex = i; break; }
    if (boneIndex >= rig->boneNames.size() || boneIndex >= skel->getNumBones()) {
        mError = "attachToBone: the owner's rig has no bone '" + bone + "'";
        return false;
    }
    // CIRCULAR: the owner hangs (directly or not) under the rider. Ogre calls
    // the result undefined; we call it a refusal.
    if (isAncestorOf(r.node, o.node)) {
        mError = "attachToBone: the rider is an ancestor of the owner (circular)";
        return false;
    }

    // Already on this exact bone: only the offset can have changed.
    if (r.boneTag && r.boneOwner == ownerId && r.boneName == bone)
        return setBoneAttachmentOffset(riderId, position, rotation, scale);

    // Any previous tag goes first — the rider keeps its world transform, which
    // the new tag is about to overwrite anyway, but the bookkeeping must be
    // clean before a second tag exists.
    if (r.boneTag) releaseBoneTag(riderId, r, 0);

    JAH_TRY {
        Ogre::TagPoint *tag = mSceneMgr->createTagPoint();
        tag->setPosition(toOgre(position));
        tag->setOrientation(Ogre::Quaternion(rotation.w, rotation.x, rotation.y, rotation.z));
        tag->setScale(toOgre(scale));
        // ORDER: the bone first (a tag that already has a scene parent is
        // refused by _setParentBone), then the rider.
        skel->getBone(boneIndex)->addTagPoint(tag);
        // A static rider under a dynamic tag is a memory-manager mismatch; the
        // document forbids static riders anyway, so this is a guard, not a
        // policy (nodeapi.cpp: "socket riders ... are never static").
        if (r.node->isStatic()) r.node->setStatic(false);
        if (r.node->getParent()) r.node->getParent()->removeChild(r.node);
        tag->addChild(r.node);
        r.boneTag = tag;
        r.boneOwner = ownerId;
        r.boneName = bone;
        if (std::find(o.boneRiders.begin(), o.boneRiders.end(), riderId) == o.boneRiders.end())
            o.boneRiders.push_back(riderId);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::detachFromBone(NodeId riderId, NodeId parent)
{
    auto rit = mNodes.find(riderId);
    if (rit == mNodes.end() || !rit->second.node) {
        mError = "detachFromBone: unknown rider node";
        return false;
    }
    if (!rit->second.boneTag) { mError = "detachFromBone: the node is not on a bone"; return false; }
    releaseBoneTag(riderId, rit->second, parent);
    return true;
}

bool OgreScene::setBoneAttachmentOffset(NodeId riderId, const Vec3 &position,
                                        const Quat &rotation, const Vec3 &scale)
{
    auto rit = mNodes.find(riderId);
    if (rit == mNodes.end() || !rit->second.boneTag) {
        mError = "setBoneAttachmentOffset: the node is not on a bone";
        return false;
    }
    JAH_TRY {
        Ogre::TagPoint *tag = rit->second.boneTag;
        tag->setPosition(toOgre(position));
        tag->setOrientation(Ogre::Quaternion(rotation.w, rotation.x, rotation.y, rotation.z));
        tag->setScale(toOgre(scale));
        return true;
    } JAH_CATCH(mError, false);
}

NodeId OgreScene::boneAttachment(NodeId riderId, std::string *bone) const
{
    auto rit = mNodes.find(riderId);
    if (rit == mNodes.end() || !rit->second.boneTag) return 0;
    if (bone) *bone = rit->second.boneName;
    return rit->second.boneOwner;
}

void OgreScene::releaseBoneTag(NodeId id, Node &n, NodeId parent)
{
    if (!n.boneTag) return;
    Ogre::TagPoint *tag = n.boneTag;
    const NodeId ownerId = n.boneOwner;
    n.boneTag = nullptr;
    n.boneOwner = 0;
    n.boneName.clear();
    if (ownerId) {
        auto oit = mNodes.find(ownerId);
        if (oit != mNodes.end()) {
            auto &list = oit->second.boneRiders;
            list.erase(std::remove(list.begin(), list.end(), id), list.end());
        }
    }
    JAH_TRY {
        if (n.node) {
            // THE POSE IT LAST RENDERED WITH, kept: `_getFullTransform` is the
            // cached derived transform (a tag CANNOT be resolved on demand —
            // TagPoint::updateFromParentImpl is assert(false)), which is
            // precisely the "keeps its last pose" the socket API promises when
            // an attachment goes stale.
            const Ogre::Matrix4 world = n.node->_getFullTransform();
            if (n.node->getParent()) n.node->getParent()->removeChild(n.node);
            Ogre::SceneNode *newParent = nullptr;
            if (parent) {
                auto pit = mNodes.find(parent);
                if (pit != mNodes.end() && pit->second.node) newParent = pit->second.node;
            }
            if (!newParent) newParent = mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC);
            newParent->addChild(n.node);
            // Re-express the world transform as a local one under the new
            // parent, so a rider that loses its socket stays where it was
            // instead of jumping to the parent's origin.
            const Ogre::Matrix4 local = newParent->_getFullTransform().inverse() * world;
            Ogre::Vector3 p, s;
            Ogre::Quaternion q;
            local.decomposition(p, s, q);
            n.node->setPosition(p);
            n.node->setOrientation(q);
            n.node->setScale(s);
        }
        mSceneMgr->destroySceneNode(tag);     // its dtor removes it from the bone
    } JAH_CATCH(mError, );
}

void OgreScene::releaseBoneRiders(NodeId id, Node &n)
{
    if (n.boneRiders.empty()) return;
    const std::vector<NodeId> riders = n.boneRiders;   // releaseBoneTag edits the list
    for (NodeId riderId : riders) {
        auto rit = mNodes.find(riderId);
        if (rit == mNodes.end()) continue;
        if (rit->second.boneOwner != id) continue;
        releaseBoneTag(riderId, rit->second, 0);
    }
    n.boneRiders.clear();
}

}}}  // namespace jahshaka::engine::detail
