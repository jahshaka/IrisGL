// THE GPU SCENE SUBSTRATE'S STATUS — the test and tool door of ENGINE-UPGRADE-2's
// stage 1 (the substrate: irisgl/engine/src/GpuSceneSubstrate.h). Its own header
// for the parallel window (SPECS/ENGINE-UPGRADE-2.md, THE BOUNDARY: Types.h and
// Engine.h are EU-1's until U0 merges); its fields join GpuSceneStatus then.
// Counters only: reading it costs nothing and changes nothing. No Ogre types.
#ifndef JAHSHAKA_ENGINE_GPUSUBSTRATE_H
#define JAHSHAKA_ENGINE_GPUSUBSTRATE_H

namespace jahshaka {
namespace engine {

class Scene;

struct GpuSubstrateStatus {
    /// False where there are no tables (the NULL render system's headless boot).
    bool live = false;
    /// THE COPIES IN FLIGHT: the device's frames in flight, and the instance
    /// table's copies — one per frame in flight plus the one a frame writes.
    unsigned framesInFlight = 0;
    unsigned copies = 0;
    /// The copy readers bind now (the last writing frame's).
    unsigned liveCopy = 0;
    /// The walk's width: the scene's worker threads plus the frame's own.
    unsigned workers = 0;
    /// Frames that wrote a copy, ever (one copy per writing frame).
    unsigned long long copyWrites = 0;
    /// Writes into a copy a frame still in flight may read (the rule's breach): 0.
    unsigned long long inFlightOverwrites = 0;
    /// Item walks (the dirty scan's compare of every slot), ever: at most one a frame.
    unsigned long long walks = 0;
    /// ...of which in a frame that had already walked (a transform written inside
    /// the frame, after its walk): 0.
    unsigned long long extraWalks = 0;
    /// Calls into a frame that had already called (a pre-frame GI reader and the
    /// frame's own read): each a compose of its marks only, never a walk.
    unsigned long long laterCalls = 0;
    /// Commits that copied anything, ever (GpuSceneStatus::updates).
    unsigned long long commits = 0;
    /// THE WALK'S WORK (ENGINE-UPGRADE-2B stage 2), ever: the slots whose world transform a
    /// walk compared, and the flags words it recomputed. A walk visits the slots the frame's
    /// change set and the engine's seams NAMED (the whole index only where a nodeless write
    /// or an incomplete set owes it), so a one-mover frame compares the mover's subtree.
    unsigned long long slotsCompared = 0;
    unsigned long long flagsRecomputed = 0;
    /// THE NAMED WALK'S GUARD (the arm "gpuscene.walkParity", engine.gpu_walk_parity): slots checked
    /// after a frame's compose against what the table holds, and the MISSES — a slot whose world
    /// transform or flags word moved with neither a name nor a mark (what the old whole walk's flags
    /// backstop hid). 0 misses is the named walk's completeness. The slot of the last miss, or ~0u.
    unsigned long long walkChecked = 0;
    unsigned long long walkMissed = 0;
    unsigned walkLastMissed = 0xFFFFFFFFu;
    /// True while the substrate holds the scene's ray structures (the TLAS, the
    /// BLAS set): they live and die with the scene, not in the ray tier.
    bool holdsRayStructures = false;
};

/// The status of `scene`'s substrate (a default status for a scene that is not
/// an engine scene).
GpuSubstrateStatus gpuSubstrateStatus(const Scene &scene);

}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_GPUSUBSTRATE_H
