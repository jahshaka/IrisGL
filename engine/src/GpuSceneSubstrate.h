// THE GPU SCENE'S SUBSTRATE — ONE WRITER PER SCENE MANAGER (ENGINE-UPGRADE-2 stage 1,
// SPECS/ENGINE-UPGRADE-2.md; SPECS/ENGINE_UPGRADE_SPEC.md §2 row U1).
//
// WHAT IT IS. Everything that WRITES a scene's device-side description lives here,
// once per SceneManager: the tables (GpuScene.h: the instance table — a ring of one
// copy per frame in flight plus the one being written —, the mesh, level, geometry
// and cluster tables), the dirty scan's state, its frame protocol and its counters,
// and the scene's ray structures (the TLAS, the BLAS set, the skin caches: the ray
// tier's type, held here so they live and die with the scene they describe). Every
// other reader — the ray tier, Atom's culls and decodes, Photon's gather, the card
// cache — reads the tables; nobody else writes them. A seam that changed what a slot
// says asks for it by name (`marks`, OgreScene::markGpuSlotDirty) and the substrate
// composes it.
//
// THE FRAME PROTOCOL (one walk per frame). The table is read before the frame (the GI
// chain's builds gather from it, OgreGi.cpp gatherAndBuild, twice per build) and after
// the scene graph (the frame's own read, OgreShadow.cpp). Every call is a COMPOSE: the
// explicit marks are staged, and the item WALK (the dirty scan: one compare per item)
// runs at most once per frame — at the frame's first graph-current call. A later call
// in the same frame composes only what was named to it since (a GI build's converted
// materials), and walks again only if the scene's own movement epoch moved after the
// frame's walk (a transform written INSIDE the frame — counted as `extraWalks`, which
// nothing in the engine does today). The protocol is keyed on the VaoManager's frame,
// the engine's own "a frame has passed" (there is no wall clock here).
//
// THE WALK IS A UNIFORM SCALABLE TASK on the SceneManager's own worker pool (Ogre's
// `executeUserScalableTask`), the main thread taking a share of its own: each share
// compares a contiguous range of slots and lists the ones that changed; the main
// thread then composes the changed slots in order (the composer edits Ogre state — an
// Item's render queue — and queues GI conversions, so it stays on the frame's thread).
//
// THE COMMIT. A compose stages slots into the CPU mirror; the commit copies them to
// the device (one staging map, one copy per contiguous run) into the frame's copy.
// The frame's own call composes, lets the ray levels patch the mirror, then commits
// ONCE; a pre-frame reader commits at once, because it dispatches before the frame.
#ifndef JAHSHAKA_ENGINE_GPUSCENESUBSTRATE_H
#define JAHSHAKA_ENGINE_GPUSCENESUBSTRATE_H

#include "GpuScene.h"

#include <Threading/OgreUniformScalableTask.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

/// The ray tier's per-scene structures (OgreRayQuery.cpp): defined there, owned here.
struct RaySceneStructures;
struct RaySceneStructuresDelete {
    void operator()(RaySceneStructures *p) const;   ///< OgreRayQuery.cpp
};

class GpuSceneSubstrate {
public:
    /// THE TABLES — the only object that writes them (GpuScene.h).
    GpuScene tables;
    /// Explicit marks since the last compose (OgreScene::markGpuSlotDirty): a seam
    /// that changed a slot's flags word without moving it, and every newborn slot.
    std::vector<uint32_t> marks;
    /// This compose's slots (kept, not reallocated).
    std::vector<uint32_t> dirty;
    /// The movement epoch the last consuming walk ran at, valid once a frame's walk set it (every write reaches
    /// the epoch named since ENGINE-UPGRADE-1 U0 3b: there is no host counter to be missing).
    unsigned long long epoch = 0ull;
    bool epochValid = false;
    /// create() said no (the NULL render system's headless boot): never retried.
    bool refused = false;

    // ---- the frame protocol ------------------------------------------------
    /// The VaoManager frame the last call ran in, whether that frame has walked,
    /// and the epoch its walk saw.
    uint32_t frame = 0xFFFFFFFFu;
    bool walkedThisFrame = false;
    unsigned long long frameWalkEpoch = 0ull;
    /// Starts a call in VaoManager frame `f`; true when it is the frame's first.
    bool beginCall(uint32_t f) {
        if (f == frame) {
            ++laterCalls;
            return false;
        }
        frame = f;
        walkedThisFrame = false;
        return true;
    }

    // ---- the counters (GpuSceneStatus, GpuSubstrateStatus) ----------------
    unsigned long long walks = 0ull;       ///< item walks run, ever (GpuSceneStatus::scans)
    unsigned long long composes = 0ull;    ///< composes that staged anything, ever
    unsigned long long laterCalls = 0ull;  ///< calls into a frame that had already called
    unsigned long long extraWalks = 0ull;  ///< walks in a frame that had already walked
    unsigned long long commits = 0ull;     ///< commits that copied anything, ever
    unsigned long long aabbReads = 0ull;   ///< world AABBs the walk's composer asked for, ever
    /// THE WALK'S WORK (ENGINE-UPGRADE-2B stage 2): slots whose world transform the walk
    /// compared, and flags words it recomputed (gpuFlagsFor) — ever.
    unsigned long long slotsCompared = 0ull;
    unsigned long long flagsRecomputed = 0ull;
    /// THE GUARD (the arm gpuscene.walkParity): slots checked against the table after a frame's
    /// compose, and the misses — a slot whose transform or flags word the table does not hold.
    unsigned long long walkChecked = 0ull;
    unsigned long long walkMissed = 0ull;
    uint32_t walkLastMissed = 0xFFFFFFFFu;
    double scanMicros = 0.0;               ///< the last frame's compose + commit (engine.gpuscene)
    /// The split's switch (OgreScene::atomDrawOn) as the running compose asked it once:
    /// 1 / 0 inside a compose, -1 outside (a predicate then asks for itself).
    int atomDraw = -1;
    /// A compose staged (or walked) since the last commit: the commit owes an update.
    bool commitOwed = false;
    /// The frame's own read, timed across its compose and its commit.
    double frameComposeMicros = 0.0;
    unsigned long long frameComposeWalks = 0ull;
    /// The walk's width last time: the SceneManager's workers plus the main thread.
    unsigned walkShares = 0u;

    // ---- the walk's shares (the task's scratch, kept between frames) -------
    std::vector<std::vector<uint32_t>> shareDirty;
    std::vector<unsigned long long> shareFlags;   ///< per share: the flags words it recomputed

    // ---- THE NAMED WALK (ENGINE-UPGRADE-2B stage 2) -------------------------
    // The walk compares the slots of the nodes WRITTEN since a walk last consumed the epoch
    // — the change set's moved subtrees and the engine's own named seams, the same names the
    // GI and shadow scans take (OgreScene::noteSceneTransformWrite) — never every slot: a
    // flags word that moves without a transform is marked by its seam (markGpuSlotDirty, the
    // PBS change log), so the old per-slot flags compare over the whole index (the backstop)
    // is gone. A nodeless write, an incomplete change set or an overflow owes the whole index
    // once (`namedFull`, which also makes the first walk whole).
    std::vector<uint32_t> namedNodes;   ///< NodeIds named since the consuming walk
    bool namedFull = true;
    std::vector<uint32_t> namedSlots;   ///< the walk's scratch: the named nodes' slots
    void nameNode(uint32_t node, size_t cap) {
        if (namedFull) return;
        if (namedNodes.size() >= cap) { namedNodes.clear(); namedFull = true; return; }
        namedNodes.push_back(node);
    }
    void nameAll() { namedNodes.clear(); namedFull = true; }

    // ---- the ray structures ------------------------------------------------
    /// The scene's TLAS, BLAS set and skin caches; created by the ray tier on the
    /// scene's first ray frame, released by it (OgreScene::forgetRayQuery, the tier's
    /// close) while the device lives, and only then deleted with the scene.
    using RayStructures = std::unique_ptr<RaySceneStructures, RaySceneStructuresDelete>;
    RayStructures rays;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_GPUSCENESUBSTRATE_H
