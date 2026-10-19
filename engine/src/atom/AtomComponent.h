// ATOM, THE COMPONENT (ENGINE-UPGRADE-2 stage 3, SPECS/ENGINE-UPGRADE-2.md; the Terra
// pattern: a facility of ours with a type and a boundary, its state in ONE object per
// scene, never in a process map).
//
// WHAT IT IS. A scene's visibility-buffer split — the part of Atom that is the SCENE'S:
//   - the split's doors (the measurement arms: the split, its occlusion),
//   - THE WORDS STEP (once a frame, after the substrate's commit): the screen decode's
//     draws follow the set of material words the Atom items wear (the substrate's change
//     feed, `Words`), re-synced only when that set moved or a decode twin died,
//   - the Atom view's Buckets table,
//   - the views of the scene that draw the Atom queue through PBS (stereo, passthrough),
//   - the caster cut's list, its counters, and its per-scene pass state (the stats ring,
//     the test door: OgreAtomCasterPass.cpp's `AtomCasterScene`),
//   - the status (AtomDrawStatus's fields that are the component's).
// It takes the GPU scene READ-ONLY (the substrate's tables; its words arrive through the
// substrate's observer, which is the change set the substrate already publishes) and the
// scene's slot -> datablock answer (Inputs, the adapter until the frame's record exists).
//
// WHAT IT IS NOT — the other owners, each where its lifetime is:
//   - the per-item ROUTE (OgreScene::atomRouteFor) is decided by the substrate's composer,
//     the one writer of the flags word it lands in;
//   - the DEVICE's objects (the id pass's and the caster pass's pipelines, the retire
//     window of their host-visible rings) are Atom's Hlms's (HlmsAtom::passDevice), one
//     per Root; the pyramid build's arrival counter is bound on its own compute job;
//   - a VIEW's cull lists and their stats ring are the view's (OgreView::mAtomStatsRing);
//   - the decode TWINS are per datablock — a material is shared by every scene — and so
//     HlmsAtom's, with each scene's draws of them (a twin's death reaches every scene).
//
// ITS PASSES are on the engine's one pass provider (AtomPass.h: atom_id, atom_id_late,
// atom_caster), each handed the pass's scene in the PassContext (the scene is bound on its
// SceneManager — sceneOfManager — never looked up in a registry) and the frame's command
// buffer through the context's source (AtomPassContext::frameCmd).
#ifndef JAHSHAKA_ENGINE_ATOMCOMPONENT_H
#define JAHSHAKA_ENGINE_ATOMCOMPONENT_H

#include "GpuCull.h"
#include "GpuScene.h"
#include "jahshaka/engine/Types.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace Ogre {
class HlmsDatablock;
class Root;
class SceneManager;
class TextureGpu;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

class GpuSceneSubstrate;

/// The caster pass's per-scene state (OgreAtomCasterPass.cpp: its stats ring and its
/// test door); its deleter retires the ring through the device's window.
struct AtomCasterScene;
struct AtomCasterSceneDelete {
    void operator()(AtomCasterScene *p) const;   ///< OgreAtomCasterPass.cpp
};
/// THE SUN MAPS' PYRAMIDS A MAP NO LONGER RENDERS, freed (OgreAtomCasterPass.cpp; ENGINE-UPGRADE-U5 decision
/// 16.3) - from the component's per-frame step.
void sweepCasterPyramids(AtomCasterScene *state);

class AtomComponent {
public:
    /// THE ADAPTER'S INPUT (until the frame's record): the datablock the Item in a GPU
    /// scene slot wears (its first sub-item's), or null — the Buckets view's walk.
    using SlotDatablock = std::function<const Ogre::HlmsDatablock *(uint32_t slot)>;

    AtomComponent(Ogre::Root *root, Ogre::SceneManager *const &sm, GpuSceneSubstrate &substrate,
                  SlotDatablock slotDatablock);
    ~AtomComponent();
    AtomComponent(const AtomComponent &) = delete;
    AtomComponent &operator=(const AtomComponent &) = delete;

    // --- THE DOORS (measurement arms, never modes) --------------------------------------
    bool drawEnabled() const { return mDrawEnabled; }
    void setDrawEnabled(bool on) { mDrawEnabled = on; }
    bool occlusionEnabled() const { return mOcclusionEnabled; }
    void setOcclusionEnabled(bool on) { mOcclusionEnabled = on; }

    // --- THE WORDS STEP -------------------------------------------------------------------
    /// Once a frame after the substrate's commit (OgreShadow.cpp's post-graph block; the
    /// warm-up's second frame, OgreView.cpp): the screen decode's draws for the words the
    /// Atom items wear, then the Atom view's table. A still frame does nothing.
    void step();
    /// The caster cut has a caster to draw (kGpuAtomCaster: an Atom item, or a caster-only one — the caster pass's
    /// gate; ENGINE-POLISH-ATOM-1 stage 2).
    bool hasCasters() const { return mFeed.casterCount != 0u; }
    /// The GPU scene holds an Atom item drawn from both sides (kGpuTwoSided).
    bool hasTwoSidedItems() const { return mFeed.twoSidedCount != 0u; }
    /// ...and the CASTER CUT's: the slots whose caster the cut draws with both faces (kGpuAtomCaster | kGpuTwoSided).
    bool hasTwoSidedCasters() const { return mFeed.twoSidedCasterCount != 0u; }
    unsigned long long wordSlotVisits() const { return mFeed.words.visits(); }

    // --- THE ATOM VIEW ----------------------------------------------------------------------
    AtomView view() const { return mView; }
    void setView(AtomView v) { mView = v; }
    /// One R32_UINT texel per GPU scene slot: the decode bucket that shades it (0 = none),
    /// 1024 slots a row; written only while the view is Buckets. Null until then.
    Ogre::TextureGpu *viewTable() const { return mViewTable; }
    /// Destroys the table (the scene's teardown, before its SceneManager).
    void releaseViewTable();

    // --- THE VIEWS THAT DRAW THE ATOM QUEUE THROUGH PBS ------------------------------------
    void notePbsView(const void *view, bool stereo, bool passthrough) {
        if (stereo) mStereoViews.insert(view); else mStereoViews.erase(view);
        if (passthrough) mPassthroughViews.insert(view); else mPassthroughViews.erase(view);
    }
    bool hasPassthroughViews() const { return !mPassthroughViews.empty(); }

    // --- THE CASTER CUT -----------------------------------------------------------------------
    /// ONE list per scene, reused map after map inside a frame (OgreAtomCasterPass.cpp).
    GpuCull &casterCull() { return mCasterCull; }
    /// THE LATE LIST (ENGINE-UPGRADE-U5 stage 4): a sun map's second cut - the first list's
    /// depth-rejected instances tested again against the map's own pyramid of this frame.
    GpuCull &casterCullLate() { return mCasterCullLate; }
    /// What the caster recorder read back (AtomDrawStatus's caster fields).
    struct CasterStats {
        bool valid = false;
        unsigned maps = 0, clusters = 0, instances = 0, overflow = 0, missing = 0, indexBudget = 0;
        unsigned long long triangles = 0ull;
        unsigned peakMaps = 0;              ///< the most maps one frame recorded (cumulative)
        unsigned long long unrecorded = 0;  ///< maps rendered but not recorded (cumulative)
        /// The light's own depth (ENGINE-UPGRADE-U5 stage 4): AtomDrawStatus's caster* twins.
        unsigned occluded = 0, lateInstances = 0, lateOccluded = 0;
        unsigned long long lateTriangles = 0ull;
        std::vector<AtomCasterMapStats> perMap;
    };
    void setCasterStats(const CasterStats &s) { mCasterStats = s; }
    /// The caster pass's per-scene state, created on its first use.
    AtomCasterScene &casterState();
    AtomCasterScene *casterStateIfAny() { return mCasterState.get(); }

    /// The scene's teardown, while its tree's managers live: the caster list, the caster
    /// pass state (its ring retires through the device's window), the Buckets table.
    void release();

    // --- THE STATUS ---------------------------------------------------------------------------
    /// The component's fields of AtomDrawStatus: the words, the decode draws and twins, the
    /// PBS views, the caster cut's counters. The routes are the scene's (its composer).
    void status(AtomDrawStatus &st) const;

private:
    void syncSplit();
    void syncViewTable();

    Ogre::Root *mRoot;
    /// THE SCENE'S MANAGER, BY REFERENCE (U3 stage 4a): a scene the host created gets its manager at the create's drain.
    Ogre::SceneManager *const &mSceneMgr;
    GpuSceneSubstrate &mSubstrate;
    SlotDatablock mSlotDatablock;

    bool mDrawEnabled = true;
    bool mOcclusionEnabled = true;

    /// THE ATOM ITEMS' WORDS, kept by the substrate's change feed (ATOM-CPU-WALKS-1): a
    /// slot's word is its PBS material word when the table routes it Atom (kGpuAtom), none
    /// otherwise. What the screen decode's draws follow — the walk + sort over every slot
    /// this replaces ran on every mover frame (W13: 18-19 ms at 10k).
    struct Words final : GpuSceneObserver {
        WordCounts words;
        /// THE SLOTS THE PASSES' GATES COUNT, per slot and counted (slotBits): bit 0 a two-sided Atom item
        /// (kGpuAtom | kGpuTwoSided, ATOM-TWO-SIDED-1: the id pass's no-cull draw of the two-sided range), bit 1 a
        /// caster the cut draws with both faces (kGpuAtomCaster | kGpuTwoSided: the caster cut's), bit 2 a caster
        /// the cut draws at all (kGpuAtomCaster: the caster pass's gate; ENGINE-POLISH-ATOM-1 stage 2).
        std::vector<uint8_t> slotBits;
        unsigned twoSidedCount = 0u, twoSidedCasterCount = 0u, casterCount = 0u;
        void gpuSlotChanged(uint32_t slot, const GpuInstance *now) override {
            uint32_t flags = 0u;
            if (now) std::memcpy(&flags, &now->boundsMax[3], sizeof(flags));
            words.set(slot, (now && (flags & (kGpuAtom | kGpuAtomWaiting))) ? now->raster[0] : WordCounts::kNone);
            const bool twoFaces = now && (flags & kGpuTwoSided);
            const uint8_t bits = uint8_t((twoFaces && (flags & kGpuAtom) ? 1u : 0u) |
                                        (twoFaces && (flags & kGpuAtomCaster) ? 2u : 0u) |
                                        (now && (flags & kGpuAtomCaster) ? 4u : 0u));
            if (slot >= slotBits.size()) {
                if (!bits) return;
                slotBits.resize(size_t(slot) + 1u, 0u);
            }
            twoSidedCount = twoSidedCount - (slotBits[slot] & 1u) + (bits & 1u);
            twoSidedCasterCount = twoSidedCasterCount - ((slotBits[slot] >> 1) & 1u) + ((bits >> 1) & 1u);
            casterCount = casterCount - ((slotBits[slot] >> 2) & 1u) + ((bits >> 2) & 1u);
            slotBits[slot] = bits;
        }
        void gpuSceneReset() override {
            words.reset();
            slotBits.clear();
            twoSidedCount = 0u;
            twoSidedCasterCount = 0u;
            casterCount = 0u;
        }
    };
    Words mFeed;
    /// The step's memory: HlmsAtom's twin epoch and the words at the last sync.
    unsigned long long mSyncEpoch = ~0ull;
    std::vector<uint32_t> mWords;

    AtomView mView = AtomView::Off;
    Ogre::TextureGpu *mViewTable = nullptr;
    std::vector<uint32_t> mViewRows;   ///< what the table holds (its CPU copy)
    /// The Buckets walk's witness: the GPU scene's slot writes and HlmsAtom's bucket
    /// generation it last walked at (~0 = walk on the next sync).
    unsigned long long mViewWrites = ~0ull, mViewBucketGen = ~0ull;
    /// The mode the table was last written for (a Buckets table holds bucket ids, a Triangles one mesh entries).
    AtomView mViewMode = AtomView::Off;

    std::unordered_set<const void *> mStereoViews, mPassthroughViews;

    GpuCull mCasterCull;
    GpuCull mCasterCullLate;
    CasterStats mCasterStats;
    std::unique_ptr<AtomCasterScene, AtomCasterSceneDelete> mCasterState;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_ATOMCOMPONENT_H
