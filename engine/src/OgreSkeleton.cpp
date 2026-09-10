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
Ogre::v1::SkeletonPtr OgreScene::buildV1Skeleton(const std::string &resName,
                                                 const SkeletonDesc &rig,
                                                 std::vector<Ogre::v1::OldBone *> *madeOut) {
    Ogre::v1::OldSkeletonManager &oldMgr = Ogre::v1::OldSkeletonManager::getSingleton();
    // Manual resource, no loader: Resource::load() takes the manual branch,
    // finds no loader, logs one LML_TRIVIAL line and marks it LOADED without
    // ever entering loadImpl() (the file/serializer path).
    Ogre::v1::SkeletonPtr v1skel = std::static_pointer_cast<Ogre::v1::Skeleton>(oldMgr.create(
        resName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        /*isManual=*/true, /*loader=*/nullptr));
    if (!v1skel) return v1skel;

    // Bones in DESC ORDER: createBone auto-assigns handles 0,1,2..., and the
    // def's bone index is that handle (SkeletonDef.cpp:50-74 iterates
    // mBoneList, which is handle-indexed). So desc index == def index ==
    // the blend index in our vertex data. Names must be unique — Ogre asserts
    // on a duplicate — so collisions get an index suffix; nothing reads these
    // names back except boneNames(), which serves the DESC's names.
    //
    // CLIP DEFS COME THROUGH HERE TOO, with the same desc, which is what makes
    // addAnimationsFromSkeleton legal: same bone count, same hierarchy, same
    // creation order, so the same block layout.
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
    if (madeOut) *madeOut = made;
    return v1skel;
}

// ---------------------------------------------------------------------------
bool OgreScene::bindRigToMesh(MeshRec &meshRec, const SkeletonDesc &rig,
                              const unsigned short *blendToRig, size_t blendToRigCount) {
    // The map we are being asked to write. An absent map is the IDENTITY over
    // the whole rig — today's behaviour, byte for byte.
    std::vector<Ogre::uint16> wanted;
    if (blendToRig && blendToRigCount) {
        wanted.assign(blendToRig, blendToRig + blendToRigCount);
    } else {
        wanted.reserve(rig.bones.size());
        for (size_t i = 0; i < rig.bones.size(); ++i) wanted.push_back(Ogre::uint16(i));
    }

    if (!meshRec.rigId.empty()) {
        // A mesh holds exactly one SkeletonDef. Re-binding the same rig is a
        // no-op; a different one would silently re-target every weight.
        if (meshRec.rigId != rig.id) {
            mError = "attachSkinnedMesh: the mesh is already bound to a different rig";
            return false;
        }
        // ...and exactly one blend-index map, because the map is a SubMesh
        // member. Two nodes on one mesh asset therefore have to agree about it:
        // rewriting it here would re-target the OTHER node's weights on its next
        // draw, with nothing anywhere saying so.
        if (meshRec.blendToRig != wanted) {
            mError = "attachSkinnedMesh: the mesh is already bound to this rig with a "
                     "different blend-index map";
            return false;
        }
        return true;
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
        v1skel = buildV1Skeleton(resName, rig);
        if (!v1skel) { mError = "attachSkinnedMesh: could not create the rig resource"; return false; }
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

    // The renderable's blend index -> bone index map. HlmsPbs streams one 3x4
    // matrix per entry of this map per draw (OgreHlmsPbs.cpp:3558-3566), in map
    // order — so the map is both the TRANSLATION of the mesh's blend indices and
    // the per-pass bone COST of this renderable.
    //
    // Identity when the caller passed no map: our document's blend indices then
    // already name bones by rig index (a single-piece character, and every
    // caller before the union rig). A piece of a MULTI-PIECE character passes
    // its own map instead: the union rig is the character's, the piece's blend
    // indices stay compact and piece-local, and this is where the two meet.
    // Ogre's own `_buildBoneIndexMap` would compact the map for us, but it needs
    // mBoneAssignments — a full vertex-buffer readback and rewrite
    // (SubMesh2.cpp:243-245) — and the caller already knows the answer.
    Ogre::SubMesh *sub = meshRec.mesh->getSubMesh(0);
    sub->mBlendIndexToBoneIndexMap.clear();
    sub->mBlendIndexToBoneIndexMap.reserve(wanted.size());
    for (Ogre::uint16 b : wanted) sub->mBlendIndexToBoneIndexMap.push_back(b);
    meshRec.blendToRig = wanted;

    meshRec.rigId = rig.id;
    RigRec &rec = mRigs[rig.id];
    if (rec.boneNames.empty()) {
        rec.boneNames.reserve(rig.bones.size());
        for (const BoneDesc &bd : rig.bones) rec.boneNames.push_back(bd.name);
        // The whole desc is kept: every CLIP def must be built from it (a clip
        // def with any other bone layout is undefined behaviour in
        // addAnimationsFromSkeleton), and bonePoses needs the parent indices.
        rec.desc = rig;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool OgreScene::attachSkinnedMesh(NodeId id, MeshId meshId, MaterialId matId,
                                  const SkeletonDesc &rig, const unsigned short *blendToRig,
                                  size_t blendToRigCount) {
    auto nit = mNodes.find(id);
    auto mit = mMeshes.find(meshId);
    auto tit = mMaterials.find(matId);
    if (nit == mNodes.end()) { mError = "attachSkinnedMesh: unknown node"; return false; }
    if (mit == mMeshes.end()) { mError = "attachSkinnedMesh: unknown mesh"; return false; }
    if (tit == mMaterials.end()) { mError = "attachSkinnedMesh: unknown material"; return false; }
    // The other half of setShadingModel's rule 5 (HLMS_ADOPTION P4a): that verb
    // refuses to make a RIGGED mesh's material unlit; this refuses to rig a mesh
    // whose material ALREADY is. Both exist because the Unlit family hard-zeroes
    // the skeleton properties when it hashes a renderable, so the failure is a
    // character standing at its bind pose with no error anywhere.
    // Overlay unlit materials (the non-skinnable selection outline) are
    // deliberately not covered — createOutlineMaterial(skinnable) already picks
    // the family that can skin, and nothing rigs the others.
    if (tit->second.shadingUnlit) {
        mError = "attachSkinnedMesh: the material's shading model is Unlit, which cannot "
                 "skin — the mesh would render at its bind pose";
        return false;
    }
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
    // WITH a map the mesh's blend indices index the MAP, not the rig, so the
    // range check moves to the map's length and every entry of the map is then
    // checked against the rig. Without one the two are the same array.
    const size_t indexSpace = (blendToRig && blendToRigCount) ? blendToRigCount : rig.bones.size();
    if (mit->second.maxBlendIndex >= indexSpace) {
        mError = "attachSkinnedMesh: the mesh references a bone the rig does not have";
        return false;
    }
    if (blendToRig && blendToRigCount) {
        for (size_t i = 0; i < blendToRigCount; ++i) {
            if (blendToRig[i] < rig.bones.size()) continue;
            mError = "attachSkinnedMesh: the blend-index map names a bone the rig does not have";
            return false;
        }
    }
    if (!rigHierarchyIsSane(rig)) {
        mError = "attachSkinnedMesh: rig parent indices are out of range or cyclic";
        return false;
    }

    JAH_TRY {
        if (!bindRigToMesh(mit->second, rig, blendToRig, blendToRigCount)) return false;

        Node &n = nit->second;
        detachItem(id, n);
        // Order is load-bearing: the SubMesh's blend index map must be non-empty
        // BEFORE the Item exists. Item::_initialise creates the SkeletonInstance
        // (OgreItem.cpp:104-107) and buildSubItems calls SubItem::setupSkeleton
        // (OgreSubItem.cpp:67-73), which is the only place mHasSkeletonAnimation
        // is ever set — and that is what puts `hlms_skeleton` in the shader hash.
        n.item = mSceneMgr->createItem(mit->second.mesh, Ogre::SCENE_DYNAMIC);
        n.item->setDatablock(hlmsFor(tit->second)->getDatablock(Ogre::IdString(tit->second.datablockName)));
        n.item->setVisibilityFlags(
            itemVisibilityFlags(n, tit->second.unlit, tit->second.distortion));
        n.item->setLightMask(n.lightMask);   // same reason as attachMesh's
        n.item->setRenderQueueGroup(renderQueueFor(tit->second));
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
bool OgreScene::followSkeleton(NodeId followerId, NodeId sourceId) {
    auto fit = mNodes.find(followerId);
    if (fit == mNodes.end()) { mError = "followSkeleton: unknown follower node"; return false; }
    Node &f = fit->second;
    // source = 0 means "stop following".
    if (!sourceId) {
        if (f.skeletonSource) {
            auto old = mNodes.find(f.skeletonSource);
            if (old != mNodes.end()) {
                auto &list = old->second.skeletonFollowers;
                list.erase(std::remove(list.begin(), list.end(), followerId), list.end());
            }
            f.skeletonSource = 0;
        }
        return true;
    }
    if (followerId == sourceId) { mError = "followSkeleton: a node cannot follow itself"; return false; }
    auto sit = mNodes.find(sourceId);
    if (sit == mNodes.end()) { mError = "followSkeleton: unknown source node"; return false; }
    Node &s = sit->second;
    if (!f.item || !s.item) { mError = "followSkeleton: both nodes need a renderable"; return false; }
    Ogre::SkeletonInstance *fs = f.item->getSkeletonInstance();
    Ogre::SkeletonInstance *ss = s.item->getSkeletonInstance();
    if (!fs || !ss) { mError = "followSkeleton: both renderables must be skinned"; return false; }
    if (fs->getNumBones() != ss->getNumBones()) {
        mError = "followSkeleton: the two rigs have different bone counts";
        return false;
    }
    if (f.skeletonSource && f.skeletonSource != sourceId) {
        auto old = mNodes.find(f.skeletonSource);
        if (old != mNodes.end()) {
            auto &list = old->second.skeletonFollowers;
            list.erase(std::remove(list.begin(), list.end(), followerId), list.end());
        }
    }
    f.skeletonSource = sourceId;
    if (std::find(s.skeletonFollowers.begin(), s.skeletonFollowers.end(), followerId)
        == s.skeletonFollowers.end())
        s.skeletonFollowers.push_back(followerId);
    // The per-frame pass walks THIS list, never mNodes: a scene with thousands
    // of nodes and no follower must not pay a sweep per frame for a feature
    // nothing in it uses.
    if (std::find(mFollowSources.begin(), mFollowSources.end(), sourceId) == mFollowSources.end())
        mFollowSources.push_back(sourceId);
    // One copy right now, so a follower that appears mid-animation is posed on
    // the frame it appears rather than flashing its bind pose once.
    copySkeletonPose(f, s);
    return true;
}

void OgreScene::copySkeletonPose(Node &follower, Node &source) {
    Ogre::SkeletonInstance *fs = follower.item ? follower.item->getSkeletonInstance() : nullptr;
    Ogre::SkeletonInstance *ss = source.item ? source.item->getSkeletonInstance() : nullptr;
    if (!fs || !ss || fs == ss || fs->getNumBones() != ss->getNumBones()) return;
    JAH_TRY {
        // LOCAL transforms (Bone::getPosition/Orientation/Scale are
        // parent-relative), so the follower's own scene node still decides where
        // the result lands — which is the whole reason this is a copy and not
        // Ogre's Item::useSkeletonInstanceFrom.
        const size_t bones = fs->getNumBones();
        for (size_t i = 0; i < bones; ++i) {
            Ogre::Bone *d = fs->getBone(i);
            const Ogre::Bone *s = ss->getBone(i);
            d->setPosition(s->getPosition());
            d->setOrientation(s->getOrientation());
            d->setScale(s->getScale());
        }
    } JAH_CATCH(mError, );
}

void OgreScene::applySkeletonFollowers() {
    if (!mSceneMgr || mFollowSources.empty()) return;
    for (auto srcIt = mFollowSources.begin(); srcIt != mFollowSources.end();) {
        auto nit = mNodes.find(*srcIt);
        if (nit == mNodes.end()) { srcIt = mFollowSources.erase(srcIt); continue; }
        Node &src = nit->second;
        for (auto it = src.skeletonFollowers.begin(); it != src.skeletonFollowers.end();) {
            auto fit = mNodes.find(*it);
            if (fit == mNodes.end()) { it = src.skeletonFollowers.erase(it); continue; }
            copySkeletonPose(fit->second, src);
            ++it;
        }
        if (src.skeletonFollowers.empty()) { srcIt = mFollowSources.erase(srcIt); continue; }
        ++srcIt;
    }
}

// ---------------------------------------------------------------------------
Ogre::SkeletonInstance *OgreScene::skeletonOf(NodeId id) const {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.item) return nullptr;
    return it->second.item->getSkeletonInstance();
}

bool OgreScene::hasSkeleton(NodeId id) const { return skeletonOf(id) != nullptr; }

std::vector<std::string> OgreScene::boneNames(NodeId id) const {
    const RigRec *rig = rigOf(id);
    return rig ? rig->boneNames : std::vector<std::string>();
}

bool OgreScene::setBonePoses(NodeId id, const BonePose *poses, size_t count) {
    ++mRigPoseEpoch;   // rayon2 S3: a raster-fed irradiance field re-arms on this

    // A FOLLOWER's `skeletonOf` IS the master's instance, so a pose written here
    // would silently move the whole character (AVATAR_RIG_PERF_SPEC §3.3).
    if (sharesSkeleton(id)) {
        mError = "setBonePoses: this node shares another node's skeleton — pose the source";
        return false;
    }
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

// ---------------------------------------------------------------------------
// SKELETON SHARING (AVATAR_RIG_PERF_SPEC §3.2/§3.3).
//
// THE THREE TRAPS this code exists to avoid, each read out of the pinned engine
// and each covered by a case in skeletal.share:
//
//  1. `MovableObject::_notifyAttached` ends with
//     `mSkeletonInstance->setParentNode(parent)` (OgreMovableObject.cpp:155-156).
//     On a SLAVE that instance is the MASTER's, so detaching a slave's Item —
//     which our own detachItem does on every material or mesh swap — sets the
//     shared instance's parent node to NULL and the whole character renders at
//     the origin. Every path here therefore stops sharing BEFORE the Item
//     leaves its node.
//  2. `stopUsingSkeletonInstanceFromMaster` creates the fresh instance and
//     never parents it (OgreItem.cpp:266-280). Re-attaching the Item to its own
//     node is what re-points it, so that is what unshareFollower does.
//  3. `Bone::_setNodeParent` keeps a raw SoA pointer into the parent node
//     ("This Hack just works", OgreBone.cpp:214-226), so a master node that
//     dies under its slaves leaves them reading recycled memory. Followers are
//     released before the master's Item or node goes.
//
// And one behaviour that is not a trap but reads like one: `sharesSkeletonInstance()`
// is refcount > 1, so it is true for the MASTER as well. Our bookkeeping
// (`shareSource`) is what distinguishes "renders from somebody else's rig" from
// "somebody else renders from mine".
bool OgreScene::shareSkeleton(NodeId followerId, NodeId sourceId) {
    auto fit = mNodes.find(followerId);
    if (fit == mNodes.end()) { mError = "shareSkeleton: unknown follower node"; return false; }
    Node &f = fit->second;

    if (!sourceId) {                       // stop sharing
        if (!f.shareSource) return true;
        auto sit = mNodes.find(f.shareSource);
        unshareFollower(followerId, f, sit == mNodes.end() ? nullptr : &sit->second);
        return true;
    }
    if (followerId == sourceId) { mError = "shareSkeleton: a node cannot share with itself"; return false; }
    auto sit = mNodes.find(sourceId);
    if (sit == mNodes.end()) { mError = "shareSkeleton: unknown source node"; return false; }
    Node &s = sit->second;
    if (!f.item || !s.item) { mError = "shareSkeleton: both nodes need a renderable"; return false; }
    if (!f.item->getSkeletonInstance() || !s.item->getSkeletonInstance()) {
        mError = "shareSkeleton: both renderables must be skinned";
        return false;
    }
    // NO CHAINS. A source that is itself a follower would work in Ogre (the
    // instance is just refcounted) but it makes "un-share the master" a
    // recursive lifetime problem for no gain — the host groups by character and
    // always has a real master.
    if (s.shareSource) { mError = "shareSkeleton: the source is itself sharing"; return false; }
    if (!f.shareFollowers.empty()) {
        mError = "shareSkeleton: the follower has followers of its own";
        return false;
    }
    if (f.shareSource == sourceId) return true;                 // idempotent
    // The SAME RIG or nothing: Ogre throws on a skeleton-name mismatch
    // (OgreItem.cpp:249-254), and a throw across the boundary is not an answer.
    // Our rig id IS that name (rigResourceName of MeshRec::rigId).
    const auto fm = mMeshes.find(f.meshRef), sm = mMeshes.find(s.meshRef);
    if (fm == mMeshes.end() || sm == mMeshes.end() || fm->second.rigId.empty() ||
        fm->second.rigId != sm->second.rigId) {
        mError = "shareSkeleton: the two nodes are rigged to different rigs";
        return false;
    }
    // An existing share with somebody else goes first, cleanly.
    if (f.shareSource) {
        auto old = mNodes.find(f.shareSource);
        unshareFollower(followerId, f, old == mNodes.end() ? nullptr : &old->second);
    }
    JAH_TRY {
        // THE FOLLOWER'S CLIPS GO FIRST. ClipRec caches a float* into the
        // instance's per-animation weight arrays and an index into its
        // animation list (EnginePrivate.h ClipRec::weightPtr/index); the
        // follower's instance is about to be released, so every one of those
        // would dangle. A follower carries no clips at all — it renders from the
        // master's pose — and the host re-attaches them if it ever un-shares.
        mClips.erase(followerId);
        ++f.rigGeneration;   // S16: a new instance means a new generation
        // ...and anything riding THIS node's bones: the instance those tags
        // point into is about to be released (AVATAR_RIG_PERF_SPEC §4). The
        // host re-arms them against whatever the node holds afterwards.
        releaseBoneRiders(followerId, f);
        f.item->useSkeletonInstanceFrom(s.item);
        f.shareSource = sourceId;
        if (std::find(s.shareFollowers.begin(), s.shareFollowers.end(), followerId) ==
            s.shareFollowers.end())
            s.shareFollowers.push_back(followerId);
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreScene::sharesSkeleton(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.shareSource != 0;
}

void OgreScene::unshareFollower(NodeId followerId, Node &f, Node *master) {
    if (!f.shareSource) return;
    const NodeId sourceId = f.shareSource;
    f.shareSource = 0;
    if (master) {
        auto &list = master->shareFollowers;
        list.erase(std::remove(list.begin(), list.end(), followerId), list.end());
    } else if (sourceId) {
        auto sit = mNodes.find(sourceId);
        if (sit != mNodes.end()) {
            auto &list = sit->second.shareFollowers;
            list.erase(std::remove(list.begin(), list.end(), followerId), list.end());
        }
    }
    if (!f.item || !f.item->sharesSkeletonInstance()) return;
    JAH_TRY {
        // The pose it was rendering, kept: `stopUsing...` hands back a fresh
        // instance at the BIND pose, and a piece that popped to bind for one
        // frame every time the editor swapped a material would be a visible
        // defect rather than an optimisation.
        std::vector<BonePose> keep;
        Ogre::SkeletonInstance *shared = f.item->getSkeletonInstance();
        if (shared) {
            keep.resize(shared->getNumBones());
            for (size_t i = 0; i < keep.size(); ++i) {
                const Ogre::Bone *b = shared->getBone(i);
                const Ogre::Vector3 p = b->getPosition(), sc = b->getScale();
                const Ogre::Quaternion q = b->getOrientation();
                keep[i].position = Vec3(p.x, p.y, p.z);
                keep[i].rotation = Quat(q.x, q.y, q.z, q.w);
                keep[i].scale = Vec3(sc.x, sc.y, sc.z);
            }
        }
        releaseBoneRiders(followerId, f);   // same reason as at share time
        f.item->stopUsingSkeletonInstanceFromMaster();
        // Same reason as at share time, from the other side: a fresh instance
        // means any cached clip pointers for this node name the OLD one.
        mClips.erase(followerId);
        ++f.rigGeneration;   // S16: a new instance means a new generation
        // TRAP 2: the fresh instance has NO parent node. Re-attaching the Item
        // to its own node is the only thing that gives it one
        // (MovableObject::_notifyAttached) — and it is safe now, because the
        // instance being re-parented is the follower's own.
        if (f.node) {
            f.item->detachFromParent();
            f.node->attachObject(f.item);
        }
        Ogre::SkeletonInstance *own = f.item->getSkeletonInstance();
        if (own && own->getNumBones() == keep.size()) {
            for (size_t i = 0; i < keep.size(); ++i) {
                Ogre::Bone *b = own->getBone(i);
                b->setPosition(toOgre(keep[i].position));
                b->setOrientation(Ogre::Quaternion(keep[i].rotation.w, keep[i].rotation.x,
                                                   keep[i].rotation.y, keep[i].rotation.z));
                b->setScale(toOgre(keep[i].scale));
            }
            // Every bone manual, exactly as attachSkinnedMesh leaves a fresh
            // rig: without it the first update lerps this pose back to bind.
            for (size_t i = 0; i < own->getNumBones(); ++i)
                own->setManualBone(own->getBone(i), true);
        }
    } JAH_CATCH(mError, );
}

void OgreScene::releaseShareFollowers(NodeId id, Node &n) {
    // A COPY: unshareFollower edits n.shareFollowers through the master pointer.
    const std::vector<NodeId> followers = n.shareFollowers;
    for (NodeId followerId : followers) {
        auto fit = mNodes.find(followerId);
        if (fit == mNodes.end()) continue;
        if (fit->second.shareSource != id) continue;
        unshareFollower(followerId, fit->second, &n);
    }
    n.shareFollowers.clear();
}

void OgreScene::dropShareFollowers(NodeId id, Node &n) {
    releaseShareFollowers(id, n);
    if (n.shareSource) {
        auto sit = mNodes.find(n.shareSource);
        if (sit != mNodes.end()) {
            auto &list = sit->second.shareFollowers;
            list.erase(std::remove(list.begin(), list.end(), id), list.end());
        }
        n.shareSource = 0;
    }
}

// ---------------------------------------------------------------------------
// THE MEASUREMENT SURFACE (AVATAR_RIG_PERF_SPEC §3.5).
//
// Both read OGRE's state rather than our bookkeeping, on purpose: the blend
// index map IS what HlmsPbs streams (OgreHlmsPbs.cpp:3529-3566 walks
// `indexMap`), and the SkeletonInstance pointer IS what updateAllAnimations
// evaluates. A remap or a share that silently did not land therefore reads as
// the OLD number here instead of as the number we meant.
size_t OgreScene::streamedBoneCount(NodeId id) const {
    auto it = mNodes.find(id);
    if (it == mNodes.end() || !it->second.item) return 0;
    const Ogre::Item *item = it->second.item;
    size_t n = 0;
    for (size_t i = 0; i < item->getNumSubItems(); ++i) {
        const Ogre::SubItem *sub = item->getSubItem(i);
        if (!sub->hasSkeletonAnimation()) continue;
        const Ogre::SubMesh *sm = sub->getSubMesh();
        if (!sm) continue;
        n += sm->mBlendIndexToBoneIndexMap.size();
    }
    return n;
}

RigStats OgreScene::rigStats() const {
    RigStats out;
    // DISTINCT instances, counted by pointer: two Items sharing one instance
    // are one evaluation in updateAllAnimations, however the sharing was
    // arranged, and a follower whose master went away is honestly its own again.
    std::vector<const Ogre::SkeletonInstance *> seen;
    for (const auto &kv : mNodes) {
        const Node &n = kv.second;
        if (!n.item) continue;
        const Ogre::SkeletonInstance *skel = n.item->getSkeletonInstance();
        if (!skel) continue;
        ++out.rigged;
        out.streamedBones += streamedBoneCount(kv.first);
        if (std::find(seen.begin(), seen.end(), skel) == seen.end()) seen.push_back(skel);
        // OUR bookkeeping, not Ogre's `sharesSkeletonInstance()`: that is
        // refcount > 1, which is true of the MASTER too, and "shared" here means
        // "renders from somebody else's instance".
        if (n.shareSource) ++out.shared;
    }
    out.instances = seen.size();
    return out;
}

}}}  // namespace jahshaka::engine::detail
