#pragma once
// THE ENGINE's ATOM TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>
#include "jahshaka/engine/Ids.h"

namespace jahshaka { namespace engine {

/// THE VISIBILITY BUFFER'S SPLIT AND ITS BUCKETS (ATOM S3-DRAW,
/// SPECS/atom/D3_S3_DRAW_DESIGN.md §2.2/§2.4) — what the render-queue split
/// decides for this scene's items and how many decode draws their materials need.
/// Every count is over the items the scene SHOWS (hidden ones are neither).
///   atomItems    items the id pass draws and the decode shades: a GPU-scene row,
///                an opaque PBS material the decode can serve, one submesh;
///   atomTwoSided those of them drawn from BOTH sides (a CULL_NONE material: the
///                id pass and the caster cut draw both faces, the decode lights
///                the facing one — ATOM-TWO-SIDED-1).
///   pbsItems     items that stay on stock HlmsPbs, split by the FIRST reason
///                that holds, in this order (OgreScene::atomRouteFor): `notWorld` (a
///                backdrop or helper: the id pass draws world channels only),
///                `notPbs` (an Unlit or other non-PBS datablock), `customPiece` (a
///                per-datablock custom piece or a live material — the twin cannot
///                carry one), `blended` (a transparent, faded or refractive
///                material), `alphaTested` (a cut-out, whatever its shadow
///                caster's cull — a cutout's caster culls nothing by law),
///                `cullFront` (a material that culls its FRONT faces — only back
///                faces drawn, an inverted hull — or whose shadow caster culls
///                unlike its surface), `planar` (a planar mirror), `pending` (its
///                textures are still being baked, so its bucket is not known yet:
///                PBS draws it for those frames),
///                `skinned` (the id pass reads the mesh's bind-pose rows), `noRow`
///                (no readable level-0 triangle row: a line mesh, no device
///                addresses, a normal the decode does not read — or more than one
///                submesh, which no mesh this engine builds has).
///   stockItems   items in a queue that is not the opaque item queue (gizmos,
///                wires, the sun disc, distortion): never split, never counted.
///   materials    distinct PBS materials the atom items wear;
///   buckets      the decode draws those need (HlmsAtom::BucketKey: one shader
///                permutation x one texture set x one const-buffer pool);
///   twins        decode twins HlmsAtom holds (every scene),
///   decodeDraws  this scene's hit decode draws, and
///   screenDraws  its screen decode draws.
/// ONE SHADOW MAP'S CASTERS (ENGINE-UPGRADE-U5 stage 4), as the scene last read them back
/// (AtomDrawStatus::casterPerMap): which map (`kind` 0 a view's node, 1 a mirror's, 2 the
/// probes'; `map` its index in the node; `face` a point light's cube face), the Atom casters it
/// drew in its two halves - the FIRST cut (tested against the previous frame's pyramid of this
/// map) and the LATE one (the first's rejected, tested again against this frame's) - and the
/// instances each half's depth test rejected (`lateOccluded` = drawn by neither: wholly behind
/// the map's own depth). `lightTested` is false for a map the light's depth does not reach (a
/// spot or point map, the probes', a map's first frame), `receiverTested` false for a map with no receiver
/// grid that frame (a view without an id pass - a VR headset's -, a capture, the scene's door shut). The pbs*
/// fields are filled only under
/// the measuring door shadow.casterPbsProbe: the stock casters in the map's frustum and those
/// NOT wholly behind its final depth (the triangles at the level the light's footprint picks,
/// submesh 0 only).
/// ONE SUN MAP'S CONTENT (Scene::shadowMapDigests, a test door; ENGINE-UPGRADE-U5 stage 4): the
/// first view's shadow atlas (and every planar mirror's, kind 1) read back and each active directional
/// map's rectangle hashed texel
/// for texel (FNV-1a 64 over the float bits, row by row) - two maps are equal iff the digests are,
/// the parity arm of the caster cull (shadow.casterCull on and off). `covered` counts the texels
/// holding a depth other than the clear.
struct ShadowMapDigest {
    unsigned kind = 0;   ///< 0 the view's node, 1 a mirror's
    unsigned map = 0;
    unsigned x = 0, y = 0, width = 0, height = 0;
    unsigned long long digest = 0ull;
    unsigned covered = 0;
};

/// ONE SUN MAP'S CASTERS BY ROUTE (Scene::shadowCasterRoutes, a measuring door; ENGINE-UPGRADE-U5 decision
/// 15.1, ATOM-ROUTE-COVERAGE-1): the shown shadow-casting items whose world box reaches the map's light
/// frustum, by the render-queue split's route (AtomDrawStatus's reasons, in its order: 0 atom, 1 notWorld,
/// 2 notPbs, 3 customPiece, 4 blended, 5 cullFront, 6 planar, 7 pending, 8 alphaTested, 9 skinned, 10 noRow,
/// 11 stock) - the items and their level-0 triangles - and 12 atomCaster: an item whose main pass stays on PBS
/// (blended) while the caster cut draws its caster (ENGINE-POLISH-ATOM-1 stage 2), so its own reason counts it
/// no more. Read on the host from the items' boxes (a door, not a frame's work).
struct ShadowRouteShare {
    static constexpr unsigned kAtomCaster = 12u;   ///< the caster-only route (the split's own routes are 0..11)
    static constexpr unsigned kRoutes = 13u;
    unsigned map = 0;
    unsigned items[kRoutes] = {};
    unsigned long long triangles[kRoutes] = {};
};

/// TWO SNAPSHOTS' COVERAGE, COMPARED (Scene::coverageSnapshot / coverageCompare, a measuring door; ENGINE-POLISH-ATOM-1
/// stage 3's silhouette bound): the first view's final depth and its sun maps (the PSSM splits) as two arms of one
/// process drew them. A pixel MISMATCHES where one arm drew a surface the other did not hold there - its view distances
/// differ by more than `viewRelDepth` of the nearer (a sky pixel against a surface included) - with `maskedOnly`, only
/// where either snapshot's id names a CUT-OUT the id pass drew (an Atom item through the masked ranges); a map texel
/// where the depths differ by more than `mapDepthTexels` of that map's texel footprints along the light (a surface
/// the cut simplified within its bound moves less; an outline that moved jumps to what lies behind it). A mismatch's
/// BAND is the shorter of the horizontal and the vertical run of mismatches through it, in pixels (texels): a
/// silhouette moved by s pixels makes bands s wide. The bands' 50th / 99th percentile and maximum; `sameLight` says
/// both arms' maps were drawn from one light camera.
struct CoverageCompare {
    bool valid = false;
    unsigned width = 0, height = 0;
    unsigned long long viewMismatch = 0ull;
    float viewBandP50 = 0.0f, viewBandP99 = 0.0f, viewBandMax = 0.0f;
    struct Map {
        unsigned map = 0, width = 0, height = 0;
        bool sameLight = false;
        unsigned long long mismatch = 0ull;
        float bandP50 = 0.0f, bandP99 = 0.0f, bandMax = 0.0f;
    };
    std::vector<Map> maps;
};

struct AtomCasterMapStats {
    unsigned kind = 0, map = 0, face = 0;
    bool lightTested = false;
    unsigned instances = 0, occluded = 0, lateInstances = 0, lateOccluded = 0;
    unsigned long long triangles = 0ull, lateTriangles = 0ull;
    unsigned pbsInstances = 0, pbsVisibleInstances = 0;
    unsigned long long pbsTriangles = 0ull, pbsVisibleTriangles = 0ull;
    /// THE RECEIVER TEST RAN on this map (stage 4b: the view's id depth of that frame built its receiver grid;
    /// the triangles above are after it).
    bool receiverTested = false;
    /// ...and the receivers' DILATION it ran with, in texels along the map's x and y (decision 19.1: the widest
    /// filter's reach, the normal offset in each axis' own texels and one tile - unequal on a map whose window is
    /// not square, Ogre's focused split). 0 without a grid.
    float receiverDilation[2] = {0.0f, 0.0f};
};

struct AtomDrawStatus {
    bool     live = false;
    /// The split is live: the GPU scene exists, this device runs the id pass (it
    /// needs Vulkan buffer device addresses and VK_KHR_draw_indirect_count) and
    /// the measurement door is open. Off, every item draws through PBS.
    bool     on = false;
    unsigned atomItems = 0;
    unsigned atomTwoSided = 0;
    /// ...and the CUT-OUTS among them (ENGINE-POLISH-ATOM-1 stage 3): alpha-tested items the id pass and the caster cut
    /// draw through their MASKED ranges, discarding at the material's mask (a cut-out whose mask is still being made
    /// is `pending`, one that can never have one `alphaTested`).
    unsigned atomMasked = 0;
    unsigned pbsItems = 0;
    /// Shown, but in no world channel (a backdrop such as the Ground plane widget's
    /// quad, or a helper): the id pass draws the world channels only.
    unsigned notWorld = 0;
    unsigned notPbs = 0;
    unsigned customPiece = 0;
    unsigned blended = 0;
    unsigned cullFront = 0;
    /// A planar mirror (Scene::setNodePlanarReflector): PBS binds its reflection per
    /// renderable, which a decode serving a whole bucket cannot.
    unsigned planar = 0;
    unsigned pending = 0;
    unsigned alphaTested = 0;
    unsigned skinned = 0;
    unsigned noRow = 0;
    /// THE CASTER-ONLY ROUTE (ENGINE-POLISH-ATOM-1 stage 2): PBS items (counted under their reason above) whose
    /// shadow-map caster the caster cut draws - a blended item: its stock caster is the opaque silhouette.
    unsigned atomCaster = 0;
    unsigned stockItems = 0;
    /// THE CAMERA VIEW BY ROUTE (ENGINE-POLISH-ATOM-1 stage 1): the shown items whose world box reaches the first
    /// view's camera frustum, by route (ShadowRouteShare's order: 0 atom ... 11 stock), and their level-0 triangles
    /// (every submesh's finest level) - the main pass's share per route, beside the maps' (Scene::shadowCasterRoutes).
    /// `viewRoutesValid` false without a view.
    bool     viewRoutesValid = false;
    unsigned viewRouteItems[ShadowRouteShare::kAtomCaster] = {};
    unsigned long long viewRouteTriangles[ShadowRouteShare::kAtomCaster] = {};
    unsigned materials = 0;
    unsigned buckets = 0;
    unsigned twins = 0;
    unsigned decodeDraws = 0;
    /// This scene's SCREEN decode draws (one per bucket of the atom items' words).
    unsigned screenDraws = 0;
    /// Views of this scene that draw the atom items through PBS anyway: STEREO
    /// (VR) chains carry no id pass (it renders one eye).
    unsigned stereoViews = 0;
    /// ...and views whose scene pass renders STRAIGHT INTO A WINDOW or a
    /// multisampled target (the passthrough shape — the Low tier's editor viewport,
    /// which has no post chain and takes its anti-aliasing from the window's
    /// samples): the id pass's depth cannot be that pass's depth (Ogre pairs a
    /// window's colour only with the window's own depth, and one sample with one).
    unsigned passthroughViews = 0;
    /// THE ATOM VIEW CAN PAINT (Scene::atomViewPaintable): `on` and no passthrough
    /// view — at the Low tier the viewport carries no id pass and the view paints
    /// nothing, so Scene::setAtomView's callers refuse there.
    bool     viewPaintable = false;
    /// THE CUT (ATOM-CLUSTER-CUT), as the first enabled view of this scene last read
    /// its id pass's counters back (a few frames late, never waited on): drawn
    /// clusters, indices in the compacted stream, the triangles drawn, the
    /// (instance, cluster) pairs the rule evaluated, the instances that did not fit
    /// the stream's main region (drawn COARSE that frame: their root cut, from the
    /// reserve), the instances that fit nothing (drawn NOTHING — 0 is the invariant),
    /// and the budget itself. `cutValid` false before a view has read any.
    /// THE CHANGE-DRIVEN WORDS (ATOM-CPU-WALKS-1), cumulative: the slots the GPU
    /// scene's change feed told the split's word set about (a still frame adds none —
    /// atom.words_still), and the PBS change log's drain, process-wide: the notes and
    /// the distinct datablocks it handled.
    unsigned long long wordSlotVisits = 0ull, pbsNotes = 0ull, pbsDatablocks = 0ull;
    bool     cutValid = false;
    unsigned cutClusters = 0, cutIndices = 0, cutEvaluated = 0;
    unsigned cutOverflow = 0, cutMissing = 0, cutOverflowIndices = 0, cutIndexBudget = 0;
    unsigned long long cutTriangles = 0ull;
    /// THE PER-CLUSTER OCCLUSION (ATOM-8 stage 1; AtomCutStats' figures, the same read).
    unsigned cutRejected = 0, cutRejectedOverflow = 0, cutPixelCulled = 0, cutLateClusters = 0, cutLateDropped = 0,
             cutSlackClusters = 0, cutSkyClusters = 0, cutReserveIndices = 0;
    unsigned long long cutSlackTriangles = 0ull, cutSkyTriangles = 0ull;
    /// THE OCCLUSION (ATOM-OCCLUSION-1), from the same view and the same read: whether
    /// its chain culls against the depth pyramid (the door open and the id pass live),
    /// the instances the frame's depth test REJECTED — in the frustum, not drawn — and
    /// the instances the late pass DISOCCLUDED (rejected against the previous frame's
    /// pyramid, found visible against this frame's, drawn by the late pass).
    bool     occlusion = false;
    unsigned occluded = 0, disoccluded = 0;
    /// THE CASTER CUT (ATOM-SHADOWS-1): the shadow maps' Atom casters, each map drawn from
    /// its light view's own cluster cut (the rule at the map's texel), as the scene last
    /// read its caster counters back (a few frames late, never waited on) — of the last
    /// frame that rendered any map: the caster draws recorded (one per map or cube face
    /// re-rendered), the triangles and clusters they drew and the instances that survived
    /// their frusta (summed over the maps), the instances drawn COARSE and drawn NOTHING
    /// (summed; 0 is the invariant for the second), and the caster stream's budget in
    /// indices (one stream, reused map after map). `casterValid` false before any read.
    bool     casterValid = false;
    unsigned casterMaps = 0, casterClusters = 0, casterInstances = 0;
    unsigned casterOverflow = 0, casterMissing = 0, casterIndexBudget = 0;
    unsigned long long casterTriangles = 0ull;
    /// THE MOST MAPS ONE FRAME RECORDED since the scene's first caster cut, and the
    /// maps a frame rendered but could NOT record (CASTER-USES-1): the stats ring
    /// grows mid-frame (doubling, up to 4,096 maps a frame), so the second is 0
    /// unless one frame renders more than that — or a growth failed.
    unsigned casterMapsPeak = 0;
    unsigned long long casterUnrecorded = 0ull;
    /// THE LIGHT'S OWN DEPTH (ENGINE-UPGRADE-U5 stage 4; shadow.casterCull), summed over the maps
    /// of that frame: the instances the first cut's depth test rejected, the late cut's survivors
    /// and their triangles (in casterTriangles too), and the instances neither drew. Per map:
    /// casterPerMap.
    unsigned casterOccluded = 0, casterLateInstances = 0, casterLateOccluded = 0;
    unsigned long long casterLateTriangles = 0ull;
    std::vector<AtomCasterMapStats> casterPerMap;
    /// THE DENSITY (ATOM-2 stage 1, the 'density' Atom view): while the view is on, the first such view's last frame
    /// read back on demand (a measuring read): the visible Atom pixels and the objects they belong to, split by the
    /// triangles the CUT DREW for the object per visible pixel (ATOM-8 stage 0) — [0] under 1 (green), [1] 1 to 4
    /// (yellow), [2] over 4 (red) — and the pixels by the DAG depth the cut drew them at. `densityValid` false while
    /// the view is not 'density'.
    bool     densityValid = false;
    unsigned densityPixels = 0, densityObjects = 0;
    unsigned densityClassPixels[3] = {}, densityClassObjects[3] = {};
    unsigned densityDepthPixels[16] = {};
    /// THE HIDDEN SHARE (ATOM-8 stage 0): while the Atom view is Triangles or Density, the first such view's last
    /// counted frame (read without a wait, a few frames late): `hiddenDrawnTriangles` every triangle both lists' cuts
    /// drew (their drawn-cluster records), `drawnHiddenTriangles` those of objects with NO visible pixel in that
    /// frame. `hiddenValid` false with the view on neither mode.
    bool     hiddenValid = false;
    unsigned long long hiddenDrawnTriangles = 0ull, drawnHiddenTriangles = 0ull;
    /// The render system's frame counter at this read — the stamp the engine's
    /// JAHSHAKA_ATOM_TRACE log lines carry (ATOM-BLACK-FRAMES-1's coverage trace).
    unsigned long long frame = 0ull;
};

/// The id pass's cut counters, one view's (AtomDrawStatus carries them).
struct AtomCutStats {
    unsigned clusters = 0, indices = 0, evaluated = 0, overflow = 0, missing = 0, overflowIndices = 0,
             indexBudget = 0;
    /// THE PER-CLUSTER OCCLUSION (ATOM-8 stage 1; GpuCull.h's count words 23-35): the clusters the first cut's depth
    /// test rejected (to the late pass), those past the rejected list's budget (drawn), those culled for covering no
    /// pixel centre (both lists), those the late pass drew (rejected, then seen), those a late test found behind this
    /// frame's depth (dropped), and the clusters (and triangles) kept by the sphere's depth slack alone.
    unsigned rejected = 0, rejectedOverflow = 0, pixelCulled = 0, lateClusters = 0, lateDropped = 0,
             slackClusters = 0, skyClusters = 0;
    unsigned long long slackTriangles = 0ull, skyTriangles = 0ull;
    /// THE COARSE RESERVE (ATOM-8 R8): the indices of the stream kept for the root cuts (the scene's attached slots'
    /// root cuts, at most half the budget).
    unsigned reserve = 0;
};

/// THE CUT PROBE (ATOM-2 stage 1; Scene::armAtomCutProbe / Scene::atomCutProbe): ONE frame of the first view's id-pass
/// cut, read back on demand — a measuring door, never a frame's path. Every group of every surviving instance's DAG,
/// judged by the cut job's own function in that frame (JahCullCut_cs.glsl, the permutation `cut_probe`), with the
/// terms it was judged on; the clusters the cut drew; and the frame's visible Atom pixels per object and depth (the
/// id image where the final depth is the id pass's - exactly the pixels the Atom view paints).
struct AtomCutProbeGroup {
    unsigned slot = 0;        ///< the GPU scene slot of the instance
    unsigned group = 0;       ///< the group's index in its MESH's DAG
    unsigned depth = 0;       ///< its DAG depth
    bool     affordable = false;  ///< error < allowed: the verdict the cut drew with
    bool     terminal = false;    ///< the bake's FLT_MAX (a group that does not simplify): never affordable
    bool     late = false;        ///< judged by the disocclusion pass's list (ATOM-OCCLUSION-1)
    float    error = 0.0f;    ///< the bake's measured error, mesh units
    float    allowed = 0.0f;  ///< what the view affords at the group, mesh units (jahClusterGroupAllowed)
    float    distance = 0.0f; ///< d: the eye to the transformed sphere minus its radius, metres (1 orthographic)
    float    radius = 0.0f;   ///< the sphere's radius x the instance's largest axis scale, metres
};
/// What one list's instance depth test read for one object (ATOM-6 stage 3; JahCullTest_cs.glsl `hzbProbe`):
/// `state` bit 0 tested, bit 1 occluded (rejected), bit 2 not testable (the box crosses the near plane); 0 = the
/// list did not test it (no pyramid, outside the frustum, or — the disocclusion list — the first did not reject it).
struct AtomCutProbeHzb {
    unsigned state = 0;
    unsigned mip = 0;         ///< the pyramid level the rectangle was read at
    float    nearest = 0.0f;  ///< the box's nearest depth (device depth; reverse-Z: larger is nearer)
    float    farthest = 0.0f; ///< the farthest depth under its rectangle at that mip
};
/// One drawn object of the probe's frame (a slot the cut drew or whose groups it judged).
struct AtomCutProbeObject {
    unsigned slot = 0;
    NodeId   node = 0;
    unsigned meshEntry = 0;
    float    scale = 0.0f;                    ///< the instance's largest axis scale (the currency's meshToWorldScale)
    unsigned long long level0Triangles = 0ull;   ///< the BASE mesh's (the GPU mesh table's level-0 indices / 3)
    unsigned pixels = 0;                      ///< its visible Atom pixels this frame
    unsigned depthPixels[16] = {};            ///< ...by the DAG depth the pixel was drawn at (15 = 15 and deeper)
    unsigned drawnClusters = 0;
    unsigned long long drawnTriangles = 0ull;
    unsigned long long drawnTrianglesLate = 0ull;   ///< ...of them drawn by the disocclusion (late) list
    unsigned long long depthTriangles[16] = {};  ///< the cut's drawn triangles by depth
    /// THE OBJECT'S CLUSTERS WITH A VISIBLE PIXEL this frame (ATOM-8 stage 0): of `drawnClusters`, those whose
    /// (instance, cluster) pair names at least one pixel the picture shows.
    unsigned visibleClusters = 0;
    /// Of the drawn triangles (ATOM-8 stage 2), those of clusters the depth test kept with the far plane under their
    /// box's rectangle (the sky class) and those it kept by the box's depth slack alone.
    unsigned long long skyTriangles = 0ull, slackTriangles = 0ull;
    AtomCutProbeHzb hzbFirst, hzbLate;           ///< what each list's depth test read (ATOM-6 stage 3)
    unsigned groups = 0, groupsAffordable = 0, groupsTerminal = 0;
};
struct AtomCutProbe {
    bool        valid = false;
    std::string error;                        ///< why not, when not
    /// THE REQUEST THE FIRST LIST WAS CULLED WITH, read back from the device: the tolerance (pixels), proj[1][1],
    /// the pass's viewport height, the eye, orthographic.
    float       tolerance = 0.0f, projScaleY = 0.0f, viewportHeight = 0.0f;
    float       eye[3] = { 0.0f, 0.0f, 0.0f };
    bool        orthographic = false;
    unsigned    width = 0, height = 0;        ///< the id image's
    unsigned    atomPixels = 0;               ///< visible Atom pixels in the frame
    unsigned    depthPixels[16] = {};
    unsigned    groupsDropped = 0;            ///< records past the probe buffer's capacity (0 is the invariant)
    /// THE DRAWN CLUSTERS WITH NO VISIBLE PIXEL (ATOM-8 stage 2): their triangles, and of them the sky class and the
    /// depth-slack class (the depth test's reading of the cluster's own box).
    unsigned long long unseenClusterTriangles = 0ull, unseenSkyTriangles = 0ull, unseenSlackTriangles = 0ull;
    std::vector<AtomCutProbeGroup>  groups;
    std::vector<AtomCutProbeObject> objects;  ///< by slot
};

}}  // namespace jahshaka::engine
