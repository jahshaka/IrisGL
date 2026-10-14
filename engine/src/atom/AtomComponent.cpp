// ATOM, THE COMPONENT (atom/AtomComponent.h has the design).
#include "atom/AtomComponent.h"

#include "GpuSceneSubstrate.h"
#include "HlmsAtom.h"

#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreStagingTexture.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpuManager.h>

#include <algorithm>
#include <cstdlib>
#include <string>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
HlmsAtom *atomHlmsOf(Ogre::Root *root) {
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    return hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
}
constexpr uint32_t kViewTableWidth = 1024u;
}  // namespace

AtomComponent::AtomComponent(Ogre::Root *root, Ogre::SceneManager *sm, GpuSceneSubstrate &substrate,
                             SlotDatablock slotDatablock)
    : mRoot(root), mSceneMgr(sm), mSubstrate(substrate), mSlotDatablock(std::move(slotDatablock)) {
    // THE SELFTEST'S DOORS, a pair: JAHSHAKA_ATOM_DRAW_OFF shuts the split (every item
    // through PBS) and JAHSHAKA_ATOM_OCCLUSION_OFF the id pass's occlusion (frustum-only,
    // ATOM-OCCLUSION-1) — the scriptless --engine-selftest has no other way to take its
    // four hashes' A/B (a whole process one way). Measuring switches, never modes: every
    // other A/B goes through the verbs (world.setAtomDraw, world.setAtomOcclusion),
    // paired in one process.
    if (std::getenv("JAHSHAKA_ATOM_DRAW_OFF")) mDrawEnabled = false;
    if (std::getenv("JAHSHAKA_ATOM_OCCLUSION_OFF")) mOcclusionEnabled = false;
    // THE ATOM ITEMS' WORDS follow the table from its first write (ATOM-CPU-WALKS-1).
    mSubstrate.tables.addObserver(&mFeed);
}

AtomComponent::~AtomComponent() {
    // Before the feed dies: the table's destructor tells its observers.
    mSubstrate.tables.removeObserver(&mFeed);
}

void AtomComponent::release() {
    // THE CASTER CUT'S LIST and its pass state, with the tables they read; the Buckets
    // table, a texture of this tree's manager.
    mCasterCull.destroy();
    mCasterState.reset();
    releaseViewTable();
}

void AtomComponent::step() {
    syncSplit();
    // THE ATOM VIEW'S TABLE follows whatever the split just decided.
    syncViewTable();
}

void AtomComponent::syncSplit() {
    HlmsAtom *atom = atomHlmsOf(mRoot);
    if (!atom || !mSubstrate.tables.live()) return;
    // THE SCREEN DECODE'S DRAWS: one per bucket of the words the atom items wear,
    // re-derived when that SET moved (the change feed) or a twin died. An in-place
    // material edit is not polled for here: the PBS change log (ScenePbs::HashNote) hears
    // it at Ogre's own re-hash, and the frame's drain (OgreEngine::drainPbsChanges)
    // forgets the twin whose bucket moved and re-composes the Items it re-hashed — before
    // this scene's compose, so the route, the queue and the word follow in the same
    // frame. A still frame does nothing.
    if (!mFeed.words.changed() && atom->twinEpoch() == mSyncEpoch) return;
    std::vector<uint32_t> words = mFeed.words.take();
    if (words == mWords && atom->twinEpoch() == mSyncEpoch) return;
    mWords.swap(words);
    atom->syncScreenDecodes(mSceneMgr, mWords);
    mSyncEpoch = atom->twinEpoch();
}

// ---------------------------------------------------------------------------
// THE ATOM VIEW'S BUCKETS TABLE (D0-ATOM-VIEW). The bucket is not in the id image
// (a draw of the decode knows its own, a pixel does not), and a compositor quad
// binds textures, not the GPU scene's buffers — so the view reads one R32_UINT
// texel per GPU scene slot: the bucket (HlmsAtom::bucketIdOf) of the datablock the
// slot's atom item wears, 0 for anything else. The texture exists while the view
// is on (the quad binds it in every mode) and is freed when it goes Off; its
// CONTENTS are walked only while the view is Buckets and only when the slot set or
// a bucket's membership moved, and uploaded only when a value did.
// ---------------------------------------------------------------------------
void AtomComponent::releaseViewTable() {
    if (mViewTable && mRoot && mRoot->getRenderSystem())
        mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(mViewTable);
    mViewTable = nullptr;
    mViewRows.clear();
}

void AtomComponent::syncViewTable() {
    // OFF FREES IT (the next view that is not Off creates it again).
    if (mView == AtomView::Off) {
        if (mViewTable) releaseViewTable();
        mViewWrites = mViewBucketGen = ~0ull;
        return;
    }
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (!rs) return;
    const GpuScene &gs = mSubstrate.tables;
    const uint32_t slots = gs.live() ? uint32_t(gs.slotCount()) : 0u;
    const uint32_t rows = std::max(1u, (slots + kViewTableWidth - 1u) / kViewTableWidth);
    HlmsAtom *atom = atomHlmsOf(mRoot);
    const unsigned long long bucketGen = atom ? atom->bucketGeneration() : 0ull;
    const bool buckets = mView == AtomView::Buckets;
    // CHANGE-DRIVEN: the walk runs only in Buckets mode and only when the slot set
    // (the GPU scene's writes) or a bucket's membership (HlmsAtom's generation)
    // moved since the last one; any other mode reads no contents at all.
    if (mViewTable && mViewTable->getHeight() == rows &&
        (!buckets || (mViewWrites == gs.writes() && mViewBucketGen == bucketGen)))
        return;
    std::vector<uint32_t> table(size_t(rows) * kViewTableWidth, 0u);
    if (buckets && atom) {
        const GpuInstance *m = gs.mirrorData();
        for (uint32_t i = 0; i < slots; ++i) {
            uint32_t flags = 0u;
            std::memcpy(&flags, &m[i].boundsMax[3], sizeof(flags));
            if (!(flags & kGpuAtom)) continue;
            const Ogre::HlmsDatablock *db = mSlotDatablock(i);
            if (db) table[i] = atom->bucketIdOf(db);
        }
    }
    if (buckets) {
        mViewWrites = gs.writes();
        mViewBucketGen = bucketGen;
    } else {
        mViewWrites = mViewBucketGen = ~0ull;   // entering Buckets walks
    }
    if (mViewTable && mViewTable->getHeight() == rows && table == mViewRows) return;
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    if (mViewTable && mViewTable->getHeight() != rows) {
        tm->destroyTexture(mViewTable);
        mViewTable = nullptr;
    }
    if (!mViewTable) {
        // A ManualTexture goes Resident by an immediate transition and is never
        // notifyDataIsReady'd (DOCS/traps/ENGINE.md).
        mViewTable = tm->createTexture(
            "jahAtomViewTable/" + std::to_string(reinterpret_cast<uintptr_t>(this)),
            Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        mViewTable->setResolution(kViewTableWidth, rows);
        mViewTable->setPixelFormat(Ogre::PFG_R32_UINT);
        mViewTable->setNumMipmaps(1u);
        mViewTable->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    }
    Ogre::StagingTexture *st = tm->getStagingTexture(kViewTableWidth, rows, 1u, 1u, Ogre::PFG_R32_UINT);
    st->startMapRegion();
    Ogre::TextureBox box = st->mapRegion(kViewTableWidth, rows, 1u, 1u, Ogre::PFG_R32_UINT);
    for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(box.at(0, y, 0), &table[size_t(y) * kViewTableWidth], kViewTableWidth * sizeof(uint32_t));
    st->stopMapRegion();
    st->upload(box, mViewTable, 0, nullptr, nullptr, true);
    tm->removeStagingTexture(st);
    mViewRows.swap(table);
}

void AtomComponent::status(AtomDrawStatus &st) const {
    st.wordSlotVisits = mFeed.words.visits();
    if (HlmsAtom *atom = atomHlmsOf(mRoot)) {
        st.twins = unsigned(atom->decodeTwinCount());
        st.decodeDraws = unsigned(atom->sceneDecodeCount(mSceneMgr));
        st.screenDraws = unsigned(atom->screenDecodeCount(mSceneMgr));
    }
    st.stereoViews = unsigned(mStereoViews.size());
    st.passthroughViews = unsigned(mPassthroughViews.size());
    // THE CASTER CUT'S COUNTERS (ATOM-SHADOWS-1): the scene's one caster list.
    st.casterValid = mCasterStats.valid;
    st.casterMaps = mCasterStats.maps;
    st.casterClusters = mCasterStats.clusters;
    st.casterInstances = mCasterStats.instances;
    st.casterOverflow = mCasterStats.overflow;
    st.casterMissing = mCasterStats.missing;
    st.casterIndexBudget = mCasterStats.indexBudget;
    st.casterTriangles = mCasterStats.triangles;
    st.casterMapsPeak = mCasterStats.peakMaps;
    st.casterUnrecorded = mCasterStats.unrecorded;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
