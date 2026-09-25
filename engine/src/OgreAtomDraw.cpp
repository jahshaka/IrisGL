// ATOM S3-DRAW — the visibility buffer as the product's opaque path
// (SPECS/atom/D3_S3_DRAW_DESIGN.md; SPECS/briefs/ATOM-S3-DRAW.md).
//
// THIS FILE HOLDS THE SPLIT'S ONE DECISION (`atomRouteFor`: which items the id
// pass draws and the decode shades, and why the rest stay on stock HlmsPbs) and
// the scene's reading of it (`atomDrawStatus`: the split's counts and the decode
// BUCKETS the atom items' materials need — HlmsAtom::BucketKey).
#include "EnginePrivate.h"
#include "GpuScene.h"
#include "HlmsAtom.h"

#include <OgreHlmsManager.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreItem.h>
#include <OgreMesh2.h>
#include <OgreRoot.h>
#include <OgreSubItem.h>

#include <unordered_set>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
/// The queue renderQueueFor gives an ordinary item (Ogre's default).
constexpr Ogre::uint8 kOpaqueItemQueue = 10u;
}  // namespace

OgreScene::AtomRoute OgreScene::atomRouteFor(const Node &n, Ogre::uint32 flags) const {
    Ogre::Item *item = n.item;
    if (!item || !item->getNumSubItems()) return AtomRoute::Stock;
    const Ogre::uint8 rq = item->getRenderQueueGroup();
    if (rq != kOpaqueItemQueue && rq != kRefractiveRenderQueue) return AtomRoute::Stock;
    const Ogre::HlmsDatablock *db = item->getSubItem(0)->getDatablock();
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS)
        return AtomRoute::NotPbs;
    for (size_t s = 0; s < Ogre::CustomPieceStage::NumCustomPieceStages; ++s)
        if (db->getCustomPieceFileIdHash(Ogre::CustomPieceStage::CustomPieceStage(s)))
            return AtomRoute::CustomPiece;
    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    if (rq == kRefractiveRenderQueue || db->getBlendblock()->isAutoTransparent() ||
        pbs->getTransparencyMode() == Ogre::HlmsPbsDatablock::Refractive)
        return AtomRoute::Blended;
    if (flags & kGpuAlphaTested) return AtomRoute::AlphaTested;
    if (flags & kGpuSkinned) return AtomRoute::Skinned;
    if (item->getNumSubItems() > 1u) return AtomRoute::MultiSubmesh;
    if (n.gpuMeshSlot == GpuScene::kNoMesh || !mGpuScene.live() ||
        n.gpuMeshSlot >= mGpuScene.levelMirrorEntries() / GpuScene::kLevelsPerMesh ||
        mGpuScene.levelAt(n.gpuMeshSlot, 0u).geomRow == GpuScene::kNoGeomRow ||
        HlmsAtom::materialWordOf(db) == HlmsAtom::kNoMaterialWord)
        return AtomRoute::NoRow;
    return AtomRoute::Atom;
}

AtomDrawStatus OgreScene::atomDrawStatus() {
    AtomDrawStatus st;
    st.live = mGpuScene.live();
    Ogre::HlmsManager *hm = mRoot ? mRoot->getHlmsManager() : nullptr;
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    std::unordered_set<uint32_t> words;
    std::unordered_set<HlmsAtom::BucketKey, HlmsAtom::BucketKeyHash> buckets;
    for (Node *n : mItemNodes) {
        if (!n || !n->item) continue;
        const Ogre::uint32 flags = gpuFlagsFor(*n);
        if (!(flags & kGpuVisible)) continue;
        const AtomRoute r = atomRouteFor(*n, flags);
        switch (r) {
        case AtomRoute::Stock: ++st.stockItems; continue;
        case AtomRoute::NotPbs: ++st.notPbs; break;
        case AtomRoute::CustomPiece: ++st.customPiece; break;
        case AtomRoute::Blended: ++st.blended; break;
        case AtomRoute::AlphaTested: ++st.alphaTested; break;
        case AtomRoute::Skinned: ++st.skinned; break;
        case AtomRoute::MultiSubmesh: ++st.multiSubmesh; break;
        case AtomRoute::NoRow: ++st.noRow; break;
        case AtomRoute::Atom: break;
        }
        if (r != AtomRoute::Atom) {
            ++st.pbsItems;
            continue;
        }
        ++st.atomItems;
        const auto *db =
            static_cast<const Ogre::HlmsPbsDatablock *>(n->item->getSubItem(0)->getDatablock());
        if (!words.insert(HlmsAtom::materialWordOf(db)).second) continue;
        HlmsAtom::BucketKey key;
        std::string err;
        if (atom && atom->bucketKeyOf(db, key, err)) buckets.insert(key);
    }
    st.materials = unsigned(words.size());
    st.buckets = unsigned(buckets.size());
    st.twins = atom ? unsigned(atom->decodeTwinCount()) : 0u;
    st.decodeDraws = atom ? unsigned(atom->sceneDecodeCount(mSceneMgr)) : 0u;
    return st;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
