// Rigs: the v1-skeleton translation layer and GPU skinning (GPU_SKINNING_SPEC).
//
// WHY A v1 SKELETON AT ALL. `Ogre::SkeletonDef` — the v2 rig every Item's
// SkeletonInstance is stamped from — has exactly ONE constructor and it takes a
// `const v1::Skeleton *` (OgreMain/include/Animation/OgreSkeletonDef.h:145).
// There is no builder and no setter. Every route in goes through
// `SkeletonManager::getSkeletonDef(v1::Skeleton*)`
// (OgreMain/src/Animation/OgreSkeletonManager.cpp:52). So the v1 skeleton is a
// BUILD-TIME SCAFFOLD: we assemble one in memory (no file, no serializer — a
// manual resource with no loader is marked LOADED without loadImpl ever running,
// OgreResource.cpp:208-226), hand it over, and never look at it again. NOTHING
// v1 reaches the render path: geometry stays in our v2 buffers (v1 meshes render
// nothing at all on Vulkan), and `Mesh::_notifySkeleton` keeps only the
// resulting SkeletonDefPtr.
//
// No engine edit and no patch is needed for any of this — the v1 API is public
// and sufficient. What we own forever is this file: ~300 lines that depend on v1
// bind-pose and bone-handle semantics staying stable across Ogre-Next versions.
// If upstream ever deprecates the v1 skeleton, this is the thing that breaks.
//
// THE BIND-POSE RECONCILIATION (R1) — the one place this fails if it fails.
// Our document's skin matrix for bone i is MESH-NODE-relative:
//     skin_i = inv(meshNodeSkelSpace) · boneSkelSpace_i · offset_i
// where `offset_i` is assimp's inverse bind (Bone::inverseMeshSpacePoseMatrix).
// Ogre's per-bone matrix is WORLD-relative through the Item's SceneNode:
//     full_i = nodeWorld · derived_i · reverseBind_i          (OgreBone.cpp:363-365)
// and the skinned vertex never gets multiplied by a world matrix separately —
// the node transform reaches the vertex ONLY through the bones. Since the host
// puts the Item on the mesh node, `nodeWorld` is exactly `meshNodeSkelSpace`, so
// the two coincide iff
//     derived_i · reverseBind_i  ==  skin_i
// SkeletonDef derives `reverseBind_i` by running FK over the v1 bind locals and
// inverting (OgreSkeletonDef.cpp:131-135, :270-280). So we author the v1 bind so
// that FK_bind_i == meshSpacePose_i == inverse(offset_i); then
// reverseBind_i ≡ offset_i, and the host's per-frame job is to send
// derived_i = skin_i · meshSpacePose_i, decomposed into parent-local TRS. Both
// halves are unit-tested (skeletal.rig_translation) before anything renders:
// "the character explodes" is the symptom of an inverse dropped in one of three
// places, and it is not a debuggable symptom.
#include "EnginePrivate.h"

#include <OgreSkeleton.h>
#include <Animation/OgreBone.h>
#include <OgreOldBone.h>
#include <OgreOldSkeletonManager.h>
#include <Animation/OgreSkeletonInstance.h>
#include <Animation/OgreSkeletonManager.h>

#include <cstdio>

namespace jahshaka { namespace engine { namespace detail {

namespace {

/// FNV-1a over the bytes we are handed. Only used to shorten a caller-supplied
/// id into a legal, collision-resistant resource name.
inline std::string rigResourceName(const std::string &id) {
    unsigned long long h = 1469598103934665603ull;
    for (unsigned char c : id) { h ^= c; h *= 1099511628211ull; }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "jahRig_%016llx", h);
    return std::string(buf);
}

/// Parent indices must be in range and ACYCLIC. Bone ORDER is deliberately not
/// constrained: SkeletonDef walks `mBones[parent]` by index to compute depth
/// levels and creates bones depth level by depth level (OgreSkeletonDef.cpp:
/// 76-92, OgreSkeletonInstance.cpp:53-80), so a parent may sit anywhere in the
/// array — while the array INDEX is what the mesh's blend indices name, so
/// reordering would mean remapping every vertex for no gain. A cycle, though,
/// makes that depth walk loop forever, so it is checked here and not discovered
/// as a hang.
bool rigHierarchyIsSane(const SkeletonDesc &rig) {
    const size_t n = rig.bones.size();
    for (size_t i = 0; i < n; ++i)
        if (rig.bones[i].parent < -1 || rig.bones[i].parent >= int(n)) return false;
    for (size_t i = 0; i < n; ++i) {
        size_t steps = 0;
        for (int p = rig.bones[i].parent; p >= 0; p = rig.bones[size_t(p)].parent)
            if (++steps > n) return false;      // cycle
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
bool OgreScene::bindRigToMesh(MeshRec &meshRec, const SkeletonDesc &rig) {
    if (!meshRec.rigId.empty()) {
        // A mesh holds exactly one SkeletonDef. Re-binding the same rig is a
        // no-op; a different one would silently re-target every weight.
        if (meshRec.rigId == rig.id) return true;
        mError = "attachSkinnedMesh: the mesh is already bound to a different rig";
        return false;
    }

    const std::string resName = rigResourceName(rig.id);

    // The SkeletonDef cache is process-wide and keyed on this name
    // (SkeletonManager.cpp:52-56, cache-first: the FIRST def wins forever). The
    // id is structure-derived by contract, so two files of one rig land on one
    // def — which is what lets clips authored elsewhere drive this character
    // later — and a rig that differs anywhere lands on a different name.
    Ogre::v1::OldSkeletonManager &oldMgr = Ogre::v1::OldSkeletonManager::getSingleton();
    Ogre::v1::SkeletonPtr v1skel =
        std::static_pointer_cast<Ogre::v1::Skeleton>(oldMgr.getByName(resName));

    if (!v1skel) {
        // Manual resource, no loader: Resource::load() takes the manual branch,
        // finds no loader, logs one LML_TRIVIAL line and marks it LOADED without
        // ever entering loadImpl() (the file/serializer path).
        v1skel = std::static_pointer_cast<Ogre::v1::Skeleton>(oldMgr.create(
            resName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
            /*isManual=*/true, /*loader=*/nullptr));
        if (!v1skel) { mError = "attachSkinnedMesh: could not create the rig resource"; return false; }

        // Bones in DESC ORDER: createBone auto-assigns handles 0,1,2..., and the
        // def's bone index is that handle (SkeletonDef.cpp:50-74 iterates
        // mBoneList, which is handle-indexed). So desc index == def index ==
        // the blend index in our vertex data. Names must be unique — Ogre asserts
        // on a duplicate — so collisions get an index suffix; nothing reads these
        // names back except boneNames(), which serves the DESC's names.
        std::vector<Ogre::v1::OldBone *> made;
        made.reserve(rig.bones.size());
        for (size_t i = 0; i < rig.bones.size(); ++i) {
            const BoneDesc &bd = rig.bones[i];
            std::string name = bd.name.empty() ? ("bone" + std::to_string(i)) : bd.name;
            if (v1skel->hasBone(name)) name += "#" + std::to_string(i);
            Ogre::v1::OldBone *b = v1skel->createBone(name);
            // THE BIND POSE (R1). These locals are authored so that FK over them
            // reproduces the mesh-space bind pose, i.e. the inverse of assimp's
            // offset matrix — which makes SkeletonDef's derived reverseBindPose
            // exactly that offset matrix, and the two skinning formulations
            // coincide. The caller owns that algebra; this just stores it.
            b->setPosition(toOgre(bd.bindPosition));
            b->setOrientation(Ogre::Quaternion(bd.bindRotation.w, bd.bindRotation.x,
                                               bd.bindRotation.y, bd.bindRotation.z));
            b->setScale(toOgre(bd.bindScale));
            made.push_back(b);
        }
        // Hierarchy in a SECOND pass, so bone order is free (a parent may follow
        // its child in the array; the index is fixed by the vertex data).
        for (size_t i = 0; i < rig.bones.size(); ++i)
            if (rig.bones[i].parent >= 0)
                made[size_t(rig.bones[i].parent)]->addChild(made[i]);
        v1skel->setBindingPose();
    }

    // Two lines inside Ogre: stores the name and calls
    // SkeletonManager::getSkeletonDef(v1) — which load()s the manual skeleton and
    // builds (or returns the cached) v2 def. Our v1::SkeletonPtr is dropped on
    // return; OldSkeletonManager keeps the resource alive until Root dies, and it
    // holds no GPU memory.
    meshRec.mesh->_notifySkeleton(v1skel);
    if (!meshRec.mesh->hasSkeleton() || !meshRec.mesh->getSkeleton()) {
        mError = "attachSkinnedMesh: the engine refused the rig";
        return false;
    }

    // The renderable's blend index -> bone index map. IDENTITY: our document's
    // blend indices already name bones by rig index, and HlmsPbs streams one 3x4
    // matrix per entry of this map per draw (OgreHlmsPbs.cpp:3558-3566), in map
    // order. `_buildBoneIndexMap` would compact it to the bones actually used,
    // but that needs mBoneAssignments, which needs a full vertex-buffer readback
    // and rewrite (SubMesh2.cpp:243-245) — we already know the answer.
    Ogre::SubMesh *sub = meshRec.mesh->getSubMesh(0);
    sub->mBlendIndexToBoneIndexMap.clear();
    sub->mBlendIndexToBoneIndexMap.reserve(rig.bones.size());
    for (size_t i = 0; i < rig.bones.size(); ++i)
        sub->mBlendIndexToBoneIndexMap.push_back(static_cast<unsigned short>(i));

    meshRec.rigId = rig.id;
    RigRec &rec = mRigs[rig.id];
    if (rec.boneNames.empty()) {
        rec.boneNames.reserve(rig.bones.size());
        for (const BoneDesc &bd : rig.bones) rec.boneNames.push_back(bd.name);
    }
    return true;
}

// ---------------------------------------------------------------------------
bool OgreScene::attachSkinnedMesh(NodeId id, MeshId meshId, MaterialId matId,
                                  const SkeletonDesc &rig) {
    auto nit = mNodes.find(id);
    auto mit = mMeshes.find(meshId);
    auto tit = mMaterials.find(matId);
    if (nit == mNodes.end()) { mError = "attachSkinnedMesh: unknown node"; return false; }
    if (mit == mMeshes.end()) { mError = "attachSkinnedMesh: unknown mesh"; return false; }
    if (tit == mMaterials.end()) { mError = "attachSkinnedMesh: unknown material"; return false; }
    if (rig.bones.empty()) { mError = "attachSkinnedMesh: the rig has no bones"; return false; }
    if (rig.id.empty()) { mError = "attachSkinnedMesh: the rig has no id"; return false; }
    if (!mit->second.hasSkinData) {
        // Ogre needs the blend elements in the vertex DECLARATION, which is fixed
        // when the buffer is created — attaching first and skinning later
        // silently yields an unskinned object, so this is refused loudly.
        mError = "attachSkinnedMesh: the mesh was created without blend indices/weights";
        return false;
    }
    // Blend indices are uint8 in the vertex buffer and the def's bone index is
    // uint16, but a submesh may only reference 256 distinct bones
    // (OGRE_MAX_NUM_BONES, and the v1 createBone that builds the def throws above
    // it). Over-limit is a warning + an UNSKINNED attach at bind pose, never a
    // second renderer and never a crash.
    if (rig.bones.size() > 256) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: rig '" + rig.id + "' has " + std::to_string(rig.bones.size()) +
                " bones; the limit is 256. Rendering it at bind pose, unskinned.",
            Ogre::LML_CRITICAL);
        mError = "attachSkinnedMesh: rig exceeds 256 bones (attached unskinned)";
        attachMesh(id, meshId, matId);
        return false;
    }
    if (mit->second.maxBlendIndex >= rig.bones.size()) {
        mError = "attachSkinnedMesh: the mesh references a bone the rig does not have";
        return false;
    }
    if (!rigHierarchyIsSane(rig)) {
        mError = "attachSkinnedMesh: rig parent indices are out of range or cyclic";
        return false;
    }

    JAH_TRY {
        if (!bindRigToMesh(mit->second, rig)) return false;

        Node &n = nit->second;
        detachItem(n);
        // Order is load-bearing: the SubMesh's blend index map must be non-empty
        // BEFORE the Item exists. Item::_initialise creates the SkeletonInstance
        // (OgreItem.cpp:104-107) and buildSubItems calls SubItem::setupSkeleton
        // (OgreSubItem.cpp:67-73), which is the only place mHasSkeletonAnimation
        // is ever set — and that is what puts `hlms_skeleton` in the shader hash.
        n.item = mSceneMgr->createItem(mit->second.mesh, Ogre::SCENE_DYNAMIC);
        n.item->setDatablock(hlmsFor(tit->second)->getDatablock(Ogre::IdString(tit->second.datablockName)));
        n.item->setVisibilityFlags(tit->second.unlit ? kVisibleBit
                                                     : (kVisibleBit | kGiGeometryBit));
        if (tit->second.onTop) n.item->setRenderQueueGroup(200);
        n.node->attachObject(n.item);   // also hands the skeleton its parent node
        n.meshRef = meshId; n.materialRef = matId;

        Ogre::SkeletonInstance *skel = n.item->getSkeletonInstance();
        if (!skel) { mError = "attachSkinnedMesh: no skeleton instance was created"; return false; }
        // The two invariants that make this a v2 GPU-skinned object and not a
        // silently-unskinned one, checked on the production path so nothing can
        // regress them unnoticed (GPU_SKINNING_SPEC T2 / R7 / R9):
        //  - the geometry is still the ONE v2 VAO we uploaded — v1 geometry
        //    renders NOTHING on Vulkan, and nothing here may have rebuilt it;
        //  - the renderable is marked skeleton-animated, which is what puts
        //    `hlms_skeleton` in the shader hash (OgreHlms.cpp:3160). Without it
        //    the object draws at bind pose forever and never says so.
        if (mit->second.mesh->getSubMesh(0)->mVao[Ogre::VpNormal].size() != 1) {
            mError = "attachSkinnedMesh: the mesh lost its single v2 vertex array";
            return false;
        }
        if (n.item->getNumSubItems() != 1 || !n.item->getSubItem(0)->hasSkeletonAnimation()) {
            mError = "attachSkinnedMesh: the renderable did not come out skeleton-animated";
            return false;
        }
        // EVERY bone manual. Manual bones are the ones resetToPose leaves alone
        // (SkeletonInstance.cpp:255-289 lerps toward the bind pose with the
        // manual flag as the weight), so the values the host writes survive.
        // update() is a no-op with no active animation, but
        // updateAnimationTransforms still runs the FK every frame
        // (SceneManager.cpp:1811-1840) — which is exactly what we want.
        for (size_t i = 0; i < skel->getNumBones(); ++i)
            skel->setManualBone(skel->getBone(i), true);

        // Skinned geometry deforms every frame; GI must not try to cache it.
        // (Item MOVES already don't invalidate GI — this keeps that property.)
        if (!tit->second.unlit) invalidateGiCaches();
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
Ogre::SkeletonInstance *OgreScene::skeletonOf(NodeId id) const {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.item) return nullptr;
    return it->second.item->getSkeletonInstance();
}

bool OgreScene::hasSkeleton(NodeId id) const { return skeletonOf(id) != nullptr; }

std::vector<std::string> OgreScene::boneNames(NodeId id) const {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.meshRef) return {};
    auto mit = mMeshes.find(it->second.meshRef);
    if (mit == mMeshes.end() || mit->second.rigId.empty()) return {};
    auto rit = mRigs.find(mit->second.rigId);
    return rit == mRigs.end() ? std::vector<std::string>() : rit->second.boneNames;
}

bool OgreScene::setBonePoses(NodeId id, const BonePose *poses, size_t count) {
    Ogre::SkeletonInstance *skel = skeletonOf(id);
    if (!skel) { mError = "setBonePoses: the node has no rig"; return false; }
    if (!poses && count) { mError = "setBonePoses: null poses"; return false; }
    if (count != skel->getNumBones()) {
        mError = "setBonePoses: pose count does not match the rig's bone count";
        return false;
    }
    JAH_TRY {
        for (size_t i = 0; i < count; ++i) {
            const BonePose &p = poses[i];
            Ogre::Bone *b = skel->getBone(i);
            b->setPosition(toOgre(p.position));
            b->setOrientation(Ogre::Quaternion(p.rotation.w, p.rotation.x,
                                               p.rotation.y, p.rotation.z));
            b->setScale(toOgre(p.scale));
        }
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::boneMatrices(NodeId id, float *out, size_t count) const {
    Ogre::SkeletonInstance *skel = skeletonOf(id);
    if (!skel) { mError = "boneMatrices: the node has no rig"; return false; }
    if (!out || count != skel->getNumBones()) {
        mError = "boneMatrices: bone count does not match the rig";
        return false;
    }
    JAH_TRY {
        for (size_t i = 0; i < count; ++i) {
            // Exactly the matrix HlmsPbs streams into the bone tex buffer per
            // draw (OgreHlmsPbs.cpp:3560-3562) — nodeWorld * derived * reverseBind.
            // store4x3 is a SIMD store and needs 16-byte alignment; `out` is the
            // caller's array. The layout is already row-major 3x4 (mChunkBase[r]
            // is row r), so this is a copy, not a transpose.
            alignas(16) float tmp[12];
            skel->_getBoneFullTransform(i).store4x3(tmp);
            std::memcpy(out + i * 12, tmp, sizeof(tmp));
        }
        return true;
    } JAH_CATCH(mError, false);
}

}}}  // namespace jahshaka::engine::detail
