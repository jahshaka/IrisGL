#pragma once
// THE ENGINE's CULLING TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <vector>

namespace jahshaka { namespace engine {

/// ---- ATOM P3: THE GENERIC GPU CULL (SPECS/atom/A4_SUBSTRATE_CULL_DESIGN.md) --
///
/// WHAT A CONSUMER ASKS FOR. One request per pass: a camera, its frustum, the
/// predicates an instance must satisfy, optionally the depth pyramid to test
/// against, and how much of the chain to run. The answer is device buffers the
/// consumer binds — no host ever learns the survivor count unless it asks.
///
/// ITS FIRST READER IS A SUITE, deliberately (contract section 3): the substrate
/// is written once, before the programs that stand on it (Photon's ray tiles at
/// GA-2, Atom's own clustered draw at stage 3). Nothing shipped reads a cull
/// result today, which is why this lane moves no pixel.
struct GpuCullRequest {
    /// THE MATRIX THAT MATCHES THE PYRAMID. Row-major, row i in element i (the
    /// GPU scene's own convention — a column-major read is silently right for a
    /// pure translation, which is the worst way to find out). When `hzbLevels`
    /// is non-zero this must be the view-projection the bound pyramid was BUILT
    /// with, not necessarily this frame's.
    float viewProj[16] = {};
    /// The six frustum planes, INWARD-pointing and normalised: (a, b, c, d) with
    /// a point inside satisfying dot(n, p) + d >= 0.
    float planes[24] = {};
    /// The camera position the level rule measures distances from.
    float eye[3] = {};
    /// proj[1][1] and the pass's target height — the two terms the quality
    /// currency needs to turn a distance into a sample footprint (Types.h,
    /// `sampleFootprintPerspective`).
    float projScaleY = 0.0f;
    float viewportHeight = 0.0f;
    /// AN ORTHOGRAPHIC VIEW: one sample is the same world length at every depth,
    /// so the level rule takes no distance term (the CPU strategy's ortho case,
    /// OgreMesh.cpp): the footprint is 2 / (projScaleY * viewportHeight), i.e. the
    /// ortho window's height over the target's.
    bool orthographic = false;
    /// THE VIEW'S LOD SWITCH BAND (fork 5230c9390+8282f6d70 (was 0075)'s `hysteresis`, the fraction of
    /// the threshold being crossed): 0 = the exact level. A banded request holds each
    /// slot's last banded level until the allowed error leaves the band. Levels only
    /// (modes 1-2): the CUT (mode 3, the id pass's) takes no band — its frontier
    /// changes under one sample of error, invisibly by construction (CLUSTER_CUT D4).
    float lodHysteresis = 0.0f;
    /// GpuInstance flag predicates (GpuSceneEntry::flags documents the bits):
    /// every required bit must be set and no forbidden bit may be.
    unsigned flagsRequired = 0u, flagsForbidden = 0u;
    /// 0 = frustum only. Otherwise the number of mip levels in the pyramid to
    /// test against, which the engine takes from the view's own HzbStatus.
    unsigned hzbLevels = 0u;
    /// THE VIEWPORT'S RECTANGLE IN THE PYRAMID'S MIP 0 (x0, y0, width, height in
    /// texels): NDC spans the pass's viewport — a letterboxed view's inset, not its
    /// whole target. Width 0 (the default) = the whole pyramid.
    unsigned hzbRect[4] = { 0u, 0u, 0u, 0u };
    /// The level rule's input, in SAMPLES: the consumer's own threshold — the view's is the
    /// World Mode's `atomPixelError` row (Scene::setPixelTolerance) times the session's lodBias,
    /// a shadow map's the texel (OgreAtomCasterPass) times the same bias. 0 keeps every
    /// survivor on level 0.
    float pixelTolerance = 0.0f;
    /// 0 = survivors only; 1 = + the per-instance level; 2 = + one
    /// VkDrawIndexedIndirectCommand per survivor (the level's range); 3 = THE CLUSTER
    /// CUT (ATOM-CLUSTER-CUT): no level — the cluster rule per (survivor, cluster),
    /// the drawn clusters' indices compacted into the list's stream and one command
    /// per survivor over its run of it (the id pass's request).
    unsigned mode = 0u;
    /// THE PER-CLUSTER OCCLUSION (ATOM-8 stage 1; mode 3, a view's own lists): `clusterOcclusion` makes the cut test
    /// every cluster it would draw against the bound pyramid (a first list appends the hidden ones to its rejected
    /// list for the late pass; the late list drops them); `pixelCentreCull` culls a cluster whose rectangle, through
    /// `viewProjNow` (THIS frame's cull matrix - `viewProj` is the pyramid's), covers no pixel centre of `viewRect`
    /// (the pass's viewport in its target's pixels: x0, y0, width, height).
    bool clusterOcclusion = false;
    bool pixelCentreCull = false;
    /// The cluster test's footprint: the pyramid mip where the cluster's rectangle spans at most this many texels a
    /// side (8 = at most 81 reads, the view lists' shipped value; 4 = 25, the instance test's).
    unsigned clusterFootprint = 8u;
    float viewProjNow[16] = {};
    unsigned viewRect[4] = { 0u, 0u, 0u, 0u };
};

/// WHAT THE CHAIN DID, and what it cost. The buffers themselves stay on the
/// device; these are the numbers a suite asserts and a report prints, plus the
/// readbacks a suite asks for explicitly.
struct GpuCullResult {
    /// False when there is no compute at all (the NULL render system) or the
    /// jobs are missing because the media is not staged. Every consumer keeps
    /// its CPU path then — there is no half-answer.
    bool supported = false;
    /// Instances the request was evaluated over: the GPU scene's `slotCount()`,
    /// which is THE bound every reader of that table must use.
    unsigned instances = 0;
    /// THE COUNT THE GPU WROTE, read back from the count buffer.
    unsigned survivors = 0;
    /// Instances the DEPTH TEST rejected (in the frustum, behind the pyramid; count[16]):
    /// 0 with no pyramid (ATOM-OCCLUSION-1).
    unsigned occluded = 0;
    /// The thread-group count job 2 left for job 3, i.e. what the indirect
    /// dispatch ran: ceil(survivors / 64). 0 when the mode did not reach job 3.
    unsigned indirectGroups = 0;
    /// Draw commands job 3 wrote (mode 2), counted from the buffer rather than
    /// assumed: it must equal `survivors`.
    unsigned draws = 0;
    /// Survivors whose mesh has more than one submesh — the commands above
    /// describe submesh 0 only, because the level table does (GpuScene.h). A
    /// non-zero number here is a FINDING for the consumer, not an error.
    unsigned multiSubmeshSurvivors = 0;
    /// (The jobs' GPU time is no field here: in a capture each is a monitor row,
    /// "cull.test" / "cull.compact" / "cull.draws" / "cull.cut" / "cull.emit" —
    /// lane TEST-1 deleted the wall-clock slope these fields carried.)
    /// What the HOST spent: writing the request and dispatching. The design's
    /// claim is that this is ~0 — a small uniform write — and it is measured
    /// rather than asserted.
    double requestMs = 0.0;
    /// The readbacks, filled only when the caller asked for them (they flush the
    /// command buffer and stall, so they belong in a suite and never in a
    /// frame). `survivorSlots` is a SET: its order is the workgroups' atomic
    /// arrival order and is not stable between runs.
    std::vector<unsigned> survivorSlots;
    /// Per-SLOT level (not per survivor), as job 1 wrote it.
    std::vector<unsigned> levels;
    /// Five uints per draw: indexCount, instanceCount, firstIndex,
    /// vertexOffset, firstInstance.
    std::vector<unsigned> drawCommands;
    /// THE CUT (mode 3), from the count buffer: drawn clusters (records written),
    /// indices asked of the stream's main region, the triangles the commands draw, the
    /// (instance, cluster) pairs the rule evaluated, the instances whose cut did not fit
    /// and drew their ROOT CUT from the coarse reserve (`cutOverflow`), the instances
    /// that fit neither and drew NOTHING (`cutMissing`), the indices asked
    /// (`cutOverflowIndices`, the main cursor), and the budget the request ran under.
    unsigned cutClusters = 0, cutIndices = 0, cutTriangles = 0, cutEvaluated = 0;
    unsigned cutOverflow = 0, cutMissing = 0, cutOverflowIndices = 0, cutIndexBudget = 0;
    /// With `readBack`: THE DRAWN SET, three uints per drawn cluster — the item slot,
    /// the cluster's index in ITS MESH's DAG (`MeshData::clusters`) and its depth —
    /// in the records' (arrival) order, which is not stable between runs.
    std::vector<unsigned> cutDrawn;
};

/// THE HIERARCHICAL DEPTH PYRAMID, as built (PostFxDesc::hzb; NANITE_SPEC
/// §4.3). Photon shared infrastructure: reported so a suite can assert the
/// shape instead of trusting it, and so a future consumer can ask whether there
/// is anything to read before it binds one.
struct HzbStatus {
    /// A pyramid exists in this view's compositor graph right now.
    bool built = false;
    /// Mip levels, i.e. compute passes: 1 (the seed) + one per reduction, down
    /// to 1x1. 11 at 1920x1080.
    unsigned levels = 0;
    /// Mip 0's size — the view's own, since the pyramid is full resolution.
    unsigned width = 0, height = 0;
    /// Which way is CLOSE (RenderSystem::isReverseDepth). True — the Vulkan
    /// default at this pin — means the near plane is 1. Reported rather than
    /// assumed because the reduction operator flips with it.
    bool reverseDepth = true;
    /// WHICH DEPTH THE LEVELS KEEP (PostFxDesc::hzbFarthest). A reader must know:
    /// an occlusion test is only sound against the FARTHEST chain.
    bool farthest = true;
    /// HAS ANYTHING BEEN WRITTEN INTO IT YET? The texture exists from the moment
    /// the chain is built, and holds UNDEFINED CONTENT until the seed pass of a
    /// presented frame fills mip 0 — and again after every chain rebuild, which
    /// resets this. A consumer that culled against an unwritten pyramid would be
    /// testing against whatever the allocation last held; `fillCullView` refuses
    /// to ask for the pyramid until this is true.
    bool primed = false;
};

}}  // namespace jahshaka::engine
