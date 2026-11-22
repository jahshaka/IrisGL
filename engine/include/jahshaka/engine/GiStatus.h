#pragma once
// THE ENGINE's GISTATUS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstdint>
#include <string>
#include <vector>
#include "jahshaka/engine/GiCards.h"
#include "jahshaka/engine/GiGather.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

// ---------------------------------------------------------------------------
// THE CHANGED-REGION LIST (PHOTON-7 stage 3, AB S1; law L2: change drives work). ONE list a frame per scene says WHERE
// the world changed: the producers APPEND at their four points of the frame (the drain, the GI step, the cards' plan,
// the GPU scene's commit and the skin pass) and the list CLOSES before the frame's submit, into the one buffer every
// consumer binds (PhotonComponent::regionBuffer) and this door. Its consumers are re-pointed onto it in the stages that
// move them (4, 5, 6, 8); until then nothing reads it but the door.
// ---------------------------------------------------------------------------
/// WHAT CHANGED IN A LISTED BOX. The value is the kind's word in the GPU list.
enum class GiRegionKind : uint32_t {
    /// a scroll's commit: the cells whose light it re-injected (the entering slab and its band - the faces the shift
    /// moved), or the whole box of a cascade placed anew (a whole build, a jump) or re-injected as a delta inside a
    /// committed one
    Slab = 1,
    Card = 2,          ///< a relit card's instance box (the cards' plan), one per instance however many of its cards
    Edit = 3,          ///< the dirty path's box: geometry or a voxel input changed there (a move, an arrival, a material)
    Mover = 4,         ///< a moved or skin-posed ray-tier slot's SWEPT box: its bounds this frame joined with last frame's
    /// ...a shadow-casting mover's swept box again, as the occluder of the SUN: its shadow lies along -toSun from it
    /// (GiChangedRegions::toSun) - an AABB of the extrusion would inflate by up to the shadow's 60 m length
    MoverShadow = 5,
    LampShadow = 6,    ///< the range sphere's box of a lamp whose range meets a casting mover (its shadow cannot leave it)
    Lamp = 7,          ///< a lamp's old range sphere joined with its new one (its pose, colour or range changed; it came or went)
    /// ...a relit card's instance box whose relight a SCENE change caused - its first round after a lamp's, the sun's or
    /// a material edit's write (SurfaceCache CardRec::sceneCause; PHOTON-7 8B (A), the lead's ruling): the world changed,
    /// carried through the cache - its readers resume at once. Card (2) is the cache refining (a capture, an eviction,
    /// a convergence round): an estimate converging, the readers' slew.
    CardScene = 8,
};
constexpr unsigned kGiRegionKindCount = 9u;   ///< indexed by the kind's value (0 unused)
constexpr unsigned kGiRegionCap = 128u;       ///< regions a list holds; past it boxes MERGE (never an overflow)
/// "CANNOT SAY WHERE" (GiChangedRegions::overflow, bits): the sun changed (a directional light, the cloud layer's
/// shadow on it) or the sky did (the environment, the ambient, the fog) - every sun- or sky-lit cache changed - or a
/// change came with no box (a texture's death, a null dirty box).
constexpr uint32_t kGiRegionOverflowSun = 1u;
constexpr uint32_t kGiRegionOverflowSky = 2u;
constexpr uint32_t kGiRegionOverflowSomewhere = 4u;
/// ONE REGION: a world AABB, its kind, and the cascades whose light it may have changed (bit i = cascade i: a box test
/// against each cascade's box at the close; a Slab's own cascade).
struct GiChangedRegion {
    Vec3 lo, hi;
    GiRegionKind kind = GiRegionKind::Edit;
    uint32_t cascadeMask = 0u;
};
/// THE LIST AS IT CLOSED (Scene::giChangedRegions): the last frame that drew the scene.
struct GiChangedRegions {
    bool available = false;          ///< a list has closed since the scene was made
    unsigned long long frame = 0;    ///< the engine frame it closed in
    uint32_t overflow = 0u;          ///< kGiRegionOverflow* bits
    /// THE LIGHTING RESTART (Lumen's only one, LR §2.9): the peak channel of the sun (the first directional light of the
    /// scene's order, colour x power) or of the sky (the ambient's constant band) moved by more than 4x or under 1/4
    /// since the last close. Always with an overflow.
    bool restart = false;
    bool sun = false;                ///< the scene has a sun: `toSun` is its direction
    Vec3 toSun;                      ///< the unit vector from a surface toward the sun, at the close
    uint32_t merges = 0u;            ///< boxes merged into another of their kind at the cap (the dirty path's metric)
    std::vector<GiChangedRegion> regions;   ///< <= kGiRegionCap, in append order
};

/// ONE CASCADE'S BRICK MASK (PHOTON-7 stage 3; Scene::takeGiBrickMask): per 8^3 brick of the cascade's cells, what its
/// LIGHT PASSES changed, in CODES (one code = 1 % of the voxel's own radiance, per channel; the absolute floor is the
/// float store's quantum: kGiBrickFloor), the most over the readings landed since the last take - `direct`: the
/// frame's injections' change of the direct term (exact: the store's own bits compared); `bounce`: how far the frame's
/// FEEDBACK pass moved the total (PHOTON-7 stage 4: the store's own read-back, so a brick at its fixed point reads 0 -
/// the per-brick change the cards' and the field's staleness read in stage 5). Read back a few
/// frames late, never waited on. Bricks x fastest, `dims` per axis, at the box corner `origin` the readings were taken
/// at (a cascade that moved between two readings starts a new mask).
struct GiBrickMask {
    bool available = false;
    int cascade = -1;
    int dims[3] = { 0, 0, 0 };
    Vec3 origin;
    float brickMetres = 0.0f;
    std::vector<float> direct;
    std::vector<float> bounce;
    unsigned readings = 0;           ///< landed readings folded in since the last take
};
/// The brick mask's absolute floor (radiance): the float store's subnormal step (2^-24) is one code (1 %) here, so a
/// change the store can hold is never read below its quantum (R3).
constexpr float kGiBrickFloor = 5.9604645e-08f / 0.01f;

/// What GI is ACHIEVING, as opposed to what GiParams requested — the same
/// "the renderer beats the request" contract as View::sampleCount() and
/// Scene::activePlanarReflectors().
///
/// It exists because the hybrid could silently degrade: a missing probe
/// workspace definition made buildPcc log a line and return, leaving the scene
/// rendering as plain VCT with every caller (and every test) still believing it
/// had probe reflections (REFLECTIONS_ADOPTION_SPEC.md §3, finding F12). Read
/// this after setGlobalIllumination to assert the hybrid actually armed.
struct GiStatus {
    /// The mode currently in force (what the scene last accepted).
    GiMode mode = GiMode::Off;
    /// Live parallax-corrected cubemap probes. In VctPccHybrid this is the
    /// clamped grid product (pccProbesX * pccProbesY * pccProbesZ); 0 in every
    /// other mode, and 0 in the hybrid when the probe arm failed to build.
    int    probeCount = 0;
    /// THE PROBE GRID'S BUDGET (PCC-BUDGET-1): the tier's whole-grid VRAM budget
    /// in bytes (GiQualityFacts::probeGridBudgetBytes), the probe count it
    /// allows at the grid's face size / format / shadowing, the bytes the BUILT
    /// grid holds by the same arithmetic (giProbeGridBytes), and how many kept
    /// probes the last placement cut to fit. All 0 where no grid is placed.
    unsigned long long probeGridBudgetBytes = 0;
    int    probeGridBudgetProbes = 0;
    unsigned long long probeGridBytes = 0;
    int    probesOverBudget = 0;
    /// Whether THIS scene's passes sample its probe grid — i.e. whether probe
    /// reflections are actually being drawn in it (the binding is per scene and
    /// per pass: SceneGiBinding). False when the hybrid degraded to plain VCT,
    /// and while a staged grid is not finished.
    bool   pccBound = false;
    /// Whether THIS scene's passes sample its VCT lighting.
    bool   vctBound = false;
    /// THE LIT VOLUME — the OUTERMOST cascade's box, i.e. where the voxels
    /// reach around the camera. Equal corners mean "no volume" (GI off, or
    /// nothing built yet).
    Vec3   boundsMin;
    Vec3   boundsMax;
    /// METRES PER VOXEL of the volume `boundsMin/Max` describe — and UNDER A
    /// CASCADE CHAIN THAT IS THE OUTERMOST, COARSEST CASCADE'S CELL, because
    /// that is the volume those corners describe (render audit I-10).
    ///
    /// There is no single voxel size under a chain, so this scalar is the
    /// COARSEST one; `cascades[i].cell` is every one of them, and the innermost
    /// is what the eye is usually looking at. 0 when there is no
    /// volume. It is the number that says whether GI in a scene means anything
    /// at all — a kilometre-wide volume at 128^3 is computing a constant.
    float  voxelMetres = 0.0f;
    /// The RESOLVED reflection-probe region — the free space the probe grid was
    /// placed in, which is deliberately NOT the lit volume (it carries no
    /// margin and is pulled in to the room's walls). Equal corners in every mode
    /// but the hybrid.
    Vec3   probeRegionMin;
    Vec3   probeRegionMax;
    /// THE PLACEMENT FIT the probe region was measured from (D4-PHOTON-TIERS): the
    /// content's world box after the per-node probe-grid exclude flag, the outlier
    /// trimming and the 64 m ceiling — a PLACEMENT HEURISTIC PENDING A9, never a
    /// lighting volume. The region above is the scout's answer inside it. Equal
    /// corners where no probe grid is placed (every tier whose reflections are
    /// traced on this machine, or GI off).
    Vec3   probeFitMin;
    Vec3   probeFitMax;
    /// What the probe captures RESOLVED to (REFLECTIONS_ADOPTION_SPEC P3a/P3b),
    /// as opposed to what GiParams::probeHdr/probeShadows asked for: both are
    /// GiToggle::Auto by default, so the request alone never says what happened,
    /// and `probeShadows` additionally falls back to false when the scene has no
    /// shadow node to recalculate. False in every mode but the hybrid.
    bool   probeHdr = false;
    bool   probeShadows = false;
    /// The RESOLVED probe capture size in pixels per cube face — what
    /// `GiParams::probeCaptureSize` (0 = follow the quality dial) actually
    /// became. 0 when no grid was built.
    int    probeCaptureSize = 0;

    // ---- WHAT THE PROBES SAW: why there is (or is not) a grid ---------------
    // Owner decision 2026-09-13: "a user starts in the editor in a new project
    // with an open scene ... I would think the sky is your first reflection
    // asset." A probe that can see nothing but distance photographs the sky,
    // and the sky IBL is both cheaper and sharper than 18-32 captures of it. So
    // each candidate probe is kept or dropped by WHAT IT SAW — its own captured
    // depth, measured during the placement — and a scene whose probes all saw
    // nothing has no grid at all, which this says out loud instead of leaving
    // `probeCount 0` looking like a failure.
    //
    // (The rule that used to live here measured the scene for an ENCLOSURE —
    // facing walls, an axis count — and reported `probeEnclosedAxes` /
    // `probeGridRefused`. Both are deleted with it: owner+lead joint decision
    // 2026-09-14, "no room in any definition".)

    /// How many candidate probes the renderer built, photographed and then
    /// DROPPED because the VOLUME of the box their six faces measured was not
    /// smaller than the volume the renderer lit — i.e. what they could see was
    /// no nearer than the world itself. 0 in every mode but the hybrid.
    /// `probeCount 0` with this NON-ZERO is the open-scene answer (and the sky
    /// IBL is bound instead); `probeCount 0` with this ZERO while the mode is
    /// the hybrid is the silent-degradation failure gi.pcc_mirror exists to
    /// catch — UNLESS `probeGridByRays` says the tier traces its reflections.
    int    probesDropped = 0;
    /// THE THIRD ANSWER (PHOTON-F12-PCC): the hybrid at a RAY tier builds no
    /// grid at all — the tier's facts say its reflections are traced
    /// (`GiQualityFacts::rayReflections`) and this scene traces on this machine
    /// (`Scene::rayTracingResolved`). `probeCount` and `probesDropped` are then
    /// both 0 by design, `pccBound` false, and nothing was placed, photographed
    /// or captured (`probePlacements`, `probeCapturesTotal`).
    bool   probeGridByRays = false;
    /// Probe-grid placements STARTED on this scene over its life (the scout,
    /// the first of the three stages) and probe captures RENDERED over its life
    /// (the placement's and the budget's) — cumulative, never reset. A ray tier
    /// moves neither.
    unsigned probePlacements = 0;
    /// ...of which RE-PLACEMENTS (REGION-REBUILD-1): a refresh whose content moved
    /// the grid's region past its tolerance re-places the grid ALONE — the chain
    /// keeps its cascades and re-voxelises only the edit's own box — and counts
    /// here as well as in `probePlacements`. Cumulative, never reset.
    unsigned probeReplacements = 0;
    unsigned long long probeCapturesTotal = 0;
    /// How many probes the renderer re-captures per frame — the RESOLVED
    /// `GiParams::updateBudget`, clamped to the probes that actually exist, and
    /// 0 whenever the probe arm did not build (FIX WAVE B1/B2). 0 in every mode
    /// but the hybrid. It is the CEILING a frame may spend: a stale grid
    /// catches up within ceil(probeCount / this) frames of a change, and a
    /// still scene spends nothing (probeCapturesLastFrame reads what was
    /// actually spent); see GiParams::updateBudget.
    int    probeUpdatesPerFrame = 0;
    /// The UNION of every probe's fitted PARALLAX SHAPE — the boxes the shader
    /// reprojects reflection rays onto (FIX WAVE A2). Equal corners in every
    /// mode but the hybrid, and in the hybrid it must lie inside
    /// probeRegionMin/Max: a parallax box bigger than the free space the grid
    /// was fitted to makes the hybrid's trust test reject the probe and hand the
    /// pixel to cone tracing, which in an interior is black. The union is what
    /// giStatus can carry in constant size; the per-probe boxes go to the log
    /// under JAHSHAKA_GI_DEBUG.
    Vec3   probeShapeMin;
    Vec3   probeShapeMax;
    /// THE PER-PROBE LOCALITY CHECK (2026-09-07 fix wave, defect 2b) — and the
    /// reason the union above is NOT a self-check.
    ///
    /// The union is clamped into the probe region by construction, so "the union
    /// is inside the region" is a tautology that can never fire: it stayed
    /// perfectly healthy while every probe in the Grand Showroom had been given
    /// the SAME whole-room parallax box and the metals flickered black. What
    /// matters is per probe and RELATIVE: how far each probe's fitted box
    /// reaches past its own cell (its 1/N share of the region), as a multiple of
    /// that cell's half-extent, worst axis.
    ///
    ///   `worstProbeShapeCellRatio` — the largest such multiple in the grid.
    ///   1.0 means "exactly its own cell"; the fit is ALLOWED to reach further,
    ///   because a probe legitimately sees the whole room's walls, up to
    ///   kProbeShapeCellAllowance (OgreGi.cpp).
    ///   `probesExceedingCell`     — how many probes are past that allowance.
    ///   Non-zero is a defect, not a tuning matter: the shrink-fit has returned
    ///   a box unrelated to the space the probe is responsible for.
    ///
    /// Both are 0 in every mode but the hybrid, and 0 when no probe was placed.
    float  worstProbeShapeCellRatio = 0.0f;
    int    probesExceedingCell = 0;
    /// How many probes the REGION CLAMP had to correct at the last build — how
    /// many times `PccPerPixelGridPlacement`'s shrink-fit returned a parallax
    /// box that was not inside the space the grid was fitted to. It fits from
    /// ONE 1x1 AVERAGED depth value per cube face, which is only meaningful
    /// when every face sees a wall; put a column, a partition or a parked car
    /// in the room and the average means nothing (measured on the shipped Grand
    /// Showroom: boxes reaching +-23.7 units in a room spanning +-12.25). A
    /// high count is not itself an artifact — the clamp corrects it — but it is
    /// the honest signal that the fit is not doing the work in this scene, and
    /// unlike the union check above it CAN fire.
    int    probesClampedToRegion = 0;
    /// How many material edits on this scene have CROSSED the reflection-probe
    /// gate — the point at which a material stops (or starts) being able to
    /// reflect anything, and its shader has to be rebuilt with or without the
    /// per-pixel probe loop (fork 36162ff37+16d8e29d4 (was 0028)). An ordinary parameter push is
    /// free; a crossing is a `flushRenderables` over every renderable wearing
    /// that datablock. The only workflow that crosses repeatedly is a user
    /// dragging Specular Color down through black and back, which crosses
    /// exactly twice; a monotonically rising count on a still scene is a
    /// defect. Cumulative, never reset.
    unsigned probeGateCrossings = 0;
    /// The Forward+ per-cell CUBEMAP PROBE budget in force for this scene.
    ///
    /// `ForwardClustered::collectObjsForSlice` writes probes into a cluster cell
    /// only while the cell's count is below this, and drops the rest SILENTLY
    /// (OgreForwardClustered.cpp:291-298). A dropped probe means the pixels in
    /// that cell find no probe covering them and the hybrid hands them to cone
    /// tracing, which in an interior is black — the "hard-edged black rectangles
    /// crawling over the metals" defect of 2026-09-07. The engine therefore
    /// grows this budget to hold the probe grid it just built; a value BELOW
    /// `probeCount` means cells can still drop probes.
    int    cubemapProbeSlotsPerCell = 0;
    /// Whether the LAST full refresh re-used the existing voxel arm instead of
    /// tearing it down and building a new one (FIX WAVE B4). False after a
    /// from-scratch build, which is what every mode change, quality change and
    /// post-destruction flush still does. Exposed because the difference is a
    /// factor of several in refresh cost and is otherwise invisible.
    bool   reusedLastRefresh = false;

    // ---- DDGI / IrradianceField (GI_UNIFIED_SPEC.md §4 P1) ----------------
    // Same "what it ACHIEVED" contract as pccBound above: `GiParams::ddgi` is
    // the request, and it can be refused for reasons the caller cannot see (no
    // VCT volume to feed the field, no IFD media staged, a construction that
    // threw). These four are what the shader is actually doing.

    /// THIS scene's passes sample its irradiance field.
    bool   ifdBound = false;
    /// Probes in the field (the product of the three per-axis counts). 0 when
    /// there is no field.
    int    ifdProbes = 0;
    /// Probes the field integrates per frame while a pass is in flight - the
    /// resolved figure, `GiParams::updateBudget` x the field / 8 frames, rounded
    /// up to a power of two (OgreGi.cpp ifdProbesPerFrame). 0 when the budget is 0
    /// (paused: nothing progresses; every event's own pass runs inline) or when
    /// there is no field.
    int    ifdProbesPerFrame = 0;
    /// WHERE THE FIELD IS: the corners of the volume its probe grid spans, as
    /// the engine placed it (the field enlarges that volume by one probe block
    /// per side for its own borders; this is the volume it was given). Equal
    /// corners = no field.
    ///
    /// It is a box of CASCADE 0's size, CENTRED ON THE EYE: its window steps with
    /// the eye one probe plane at a time (PHOTON-7 stage 5, 5-5), which is the only
    /// way to see, from outside, where the leak-free diffuse actually is.
    Vec3 ifdMin;
    Vec3 ifdMax;
    /// How many times the field's window has moved since the last build (ifdScrolls + ifdReplacements, below): 0 on a
    /// still camera, one per probe spacing the eye walks (5-5: the window steps with the eye). Reset by a build.
    unsigned long long ifdFollows = 0;
    /// THE FIELD SCROLLS (PHOTON-WRITER-1): how many probes the LAST step entered -
    /// the planes that entered the window, not the field (0 before any scroll; a
    /// re-placement's whole convergence does not count here).
    unsigned ifdScrollProbes = 0;
    /// How the follows split (PHOTON-WRITER-1): `ifdScrolls` moved the window and
    /// kept every probe that stayed in it; `ifdReplacements` re-placed the whole
    /// field (a resize, or a jump of the whole grid or more - nothing to keep).
    /// ifdFollows = ifdScrolls + ifdReplacements. A re-placement counts ONCE,
    /// however many frames its slabs take (PHOTON-FIELD-ROTATE-1: they are the
    /// "ifd.replace" monitor rows). Reset by a build.
    unsigned long long ifdScrolls = 0;
    unsigned long long ifdReplacements = 0;
    /// THE FIELD'S ESTIMATOR (PHOTON-FIELD-ROTATE-1): a probe's value is the mean of
    /// its integrations, each under a fresh random rotation of its ray set, until it
    /// holds `ifdTargetSamples` or the variance stop ends it. THE FIELD REFINES ONLY WHAT
    /// CHANGED (PHOTON-7 stage 5, 5-4): a probe is MARKED when what its rays read moved a
    /// code of its own irradiance (the GPU's accumulation of the bricks' absolute moves -
    /// `ifdMarkedByMoves` - or an EDIT's box it stands in - `ifdMarkedByRegion`; the sky or
    /// a change with no box marks every probe, `ifdMarkAlls` calls; a SLAB marks nothing, its
    /// moves do - the lead's ruling, PhotonGi.cpp markFieldFromRegions) and
    /// RESTARTS (keep 0, four samples deep: `ifdRestarts`, cumulative since the build); the
    /// rest keep. `ifdOwedRestarts` are marked and not restarted yet, `ifdOwedRefines` below
    /// the target and not stopped, and `ifdWorking` a pass of the field's own window running
    /// (a build, a re-placement, a scroll's entered planes): all zero
    /// = converged, the field costs nothing until the next mark. A PAUSED field (budget 0)
    /// has a target of 1: its restarts run inline and nothing is owed after them.
    unsigned ifdTargetSamples = 0;
    /// ...a restart's (and a fresh integration's) depth in samples (kIfdChangeSamples), and the marks' read-back lag in
    /// frames (a reading is taken once the frame is past its own by the VaoManager's dynamic buffer multiplier: that + 1) -
    /// the constants a law on the field's last write is made of (PHOTON-7 stage 5, 5-5: never a literal in a row).
    unsigned ifdRestartDepth = 0, ifdMarkLag = 0;
    unsigned ifdOwedRestarts = 0, ifdOwedRefines = 0;
    unsigned long long ifdOwedIntegrations = 0;   ///< at most: each restart deep and to the target, each refine to it
    bool ifdWorking = false;
    unsigned long long ifdMarkedByMoves = 0, ifdMarkedByRegion = 0, ifdMarkAlls = 0, ifdRestarts = 0;
    /// The marks' reports a FULL read-back ring could not ticket, KEPT for the next ticket (never dropped; stage 5's read
    /// B3), cumulative: 0 in the shipped ring (the multiplier + 2), non-zero under the test door gi.fieldMarkRingFull.
    unsigned long long ifdReportsKept = 0;
    /// The probe integrations the field's last recorded frame dispatched (its rows' units: the budget's spend, an
    /// upper bound of the GPU's - a stopped probe's group exits early), and THE GRANT it had (the field's integrations a
    /// frame, the same at rest as in motion: 0 = nothing owed).
    unsigned ifdDispatchedLastFrame = 0;
    unsigned ifdGrantLastFrame = 0;
    /// THE READ REGION'S EMPTY PROBES this frame, after its work (PHOTON-7 stage 5, 5-5): the probes of the window less
    /// its guard band (its outer plane) that hold no sample - each one a probe a reader gives no say, a cage weight below
    /// whole. 0 while the band keeps up with the eye (a re-placement, a build or a jump fills in at the budget).
    unsigned ifdReadUnheld = 0;
    /// THE GPU'S OWN COUNT (PHOTON-2 L6): every probe integration the generation job has made since the field's build,
    /// as its last readback found it (four frames late) - differenced over a window, the true integrations a per-
    /// integration price divides by (a skipped or stopped probe's group exits without one).
    unsigned long long ifdIntegrations = 0;
    /// GI IS AT REST (PHOTON-FIELD-ROTATE-1): nothing this scene's GI owes will
    /// change the picture - no rebuild or re-voxelisation pending (the flush, a
    /// staged build, a cascade step, a chain-shape change, a dirty box), no light
    /// tick or settle injection owed, no field follow owed, and the irradiance
    /// field's passes all done (refinements included, unless the budget is paused),
    /// and THE SURFACE CACHE OWES NOTHING (PHOTON-7 stage 10): the stop's one rest
    /// decision taken, its captures, relights and card-to-card rounds done, no
    /// relight's report in flight - the voxels' sides and the reflection read the
    /// cards. THE one settle predicate: a screenshot, the selftest's poses and every
    /// "the picture has stopped moving" check wait on it, in frames. True with
    /// GI off. (Reflection-probe captures are `staleProbes`, separately.)
    bool giAtRest = true;
    /// THE CHAIN STILL OWES VOXEL WORK (PHOTON-7 stage 1's voxel door): a rebuild, a re-voxelisation, a cascade step or an
    /// injection (a light tick, a settle step, a bounce series being measured) - giAtRest's predicate without the
    /// irradiance field's integration. A readback of the voxels (giVoxelVolume, giVoxelStats) taken while it is false
    /// reads the chain's answer for the placements it stands at. False with GI off.
    bool chainOwed = false;
    /// THE CHANGED-REGION LIST (PHOTON-7 stage 3) as it last CLOSED (GiChangedRegions; the regions themselves:
    /// Scene::giChangedRegions): the frame, its regions and their count per kind (by the kind's value), its overflow
    /// bits and restart flag, the merges its cap made; over the scene's life the closes and those that listed anything
    /// (a region or a flag).
    unsigned long long regionFrame = 0;
    unsigned regionCount = 0;
    unsigned regionsByKind[kGiRegionKindCount] = {};
    uint32_t regionOverflow = 0u;
    bool regionRestart = false;
    unsigned regionMerges = 0;
    unsigned long long regionCloses = 0, regionClosesListing = 0;
    /// SSAO IS REFUSED IN THIS SCENE (SSAO-DOUBLE-1; giCarriesOcclusion): true
    /// whenever the mode is not Off — every view that draws this scene builds its
    /// chain without the SSAO passes, whatever its PostFxDesc::ssao asks — with
    /// `ssaoSuppressedReason` saying why (empty when false). False with GI off,
    /// where a view's SSAO runs as asked.
    bool ssaoSuppressed = false;
    std::string ssaoSuppressedReason;

    // ---- THE PROBE CACHE (ENGINE_CACHE_POLICY_SPEC §2 P1/P6/P7) -------------
    // Reflection probes are re-captured only while STALE. These say what the
    // cache is doing, in counters rather than milliseconds: a still scene reads
    // probeCapturesLastFrame 0 and staleProbes 0, every frame.

    /// How many probes actually re-captured on the LAST rendered frame, from
    /// every source: the budget's picks and anything else that marked a probe
    /// dirty — plus, on a from-scratch build, the placement pass's own captures
    /// of the whole grid. 0 in every mode but the hybrid.
    int  probeCapturesLastFrame = 0;
    /// HOW MANY FRAMES THE MOTION DEFERRAL HELD THE BUDGET, for the life of the
    /// scene (DRAG-1, REFLECT F3). A capture of a box that is still moving is
    /// out of date before it is displayed and the next frame stales it again,
    /// so the spend waits for the content to hold still; the staleness itself
    /// is recorded either way, so the sweep guarantee keeps its shape. A scene
    /// nothing moves in reads 0 for ever; a drag raises it by one per frame.
    unsigned long long probeCapturesDeferred = 0;
    /// How many probes are still stale — owe a capture the budget has not
    /// spent yet. The grid has caught up when this reads 0; it drains at the
    /// resolved budget per frame (probeUpdatesPerFrame).
    int  staleProbes = 0;
    /// The input that last marked the grid stale, and a serial that moves each
    /// time something does (so a reader can tell two events with the same
    /// reason apart).
    GiStaleReason lastStaleReason = GiStaleReason::None;
    unsigned long long staleSerial = 0;
    /// FROM-SCRATCH GI builds (VCT/hybrid arm or an Instant Radiosity re-trace
    /// from setGlobalIllumination or a post-destruction flush) since the scene
    /// was created. A page return, a refresh and an idle frame must never move
    /// it; a mode, quality, grid or bounds change and a destroyed object do.
    unsigned long long rebuilds = 0;

    /// THE MOVEMENT SCAN'S OWN COST (clean-2 lane, 2026-09-13). The renderer
    /// cannot be told that a document node moved — the document writes into the
    /// shared scene graph directly — so it walks every item's world AABB to
    /// find out. That walk used to run on EVERY frame of every probe-lit scene,
    /// still or not (392 / 1961 / 4328 us at 1k / 5k / 10k nodes); it now runs
    /// only on frames where a write was NAMED — the frame's ChangeSet or the
    /// engine's own seams — and re-reads only the named items (U0 3b).
    ///
    /// `giScans` is cumulative and is the assertable half: the delta over N
    /// STILL frames must be 0, and over N frames with something moving, N.
    /// `giScanMicros` is what the last scan that actually ran cost — 0 on a
    /// skipped frame, which is the whole point.
    unsigned long long giScans = 0;
    /// ...of them, the FULL ones (every item re-read: the first, an incomplete change set, a nodeless input). The
    /// rest re-read only the items the frame's change set and the engine's seams named (U0 3b).
    unsigned long long giScansFull = 0;
    double             giScanMicros = 0.0;
    /// EVERY `getWorldAabbUpdated` THIS ENGINE'S OWN GI CODE HAS ASKED FOR,
    /// cumulative. That call walks the node's parent chain and recomputes the
    /// node's whole SIMD block, and there were three separate per-frame walks
    /// making it per item: the movement scan and the two signatures the host
    /// reads to decide whether to re-solve (plus the Forward+ slice walk one
    /// frame in thirty). The acceptance for all four is this counter: its
    /// delta over a STILL frame is 0.
    unsigned long long giAabbReads = 0;

    // ---- PHOTON cascades (PHOTON_SPEC P0) ----------------------------------
    /// ONE live cascade, as BUILT — the counterpart of GiParams::GiCascadeDesc.
    struct CascadeStatus {
        /// Half-extent in metres of the box this cascade covers.
        float halfSize = 0.0f;
        /// Voxel resolution per axis.
        int   resolution = 0;
        /// Metres per voxel (halfSize * 2 / resolution) — the number that says
        /// what this cascade can actually resolve.
        float cell = 0.0f;
        /// Metres of camera travel between re-centres (stepCells * cell).
        float step = 0.0f;
        /// THE NEAR-FIELD RADIUS THIS CASCADE GUARANTEES, in metres:
        /// `halfSize - step - cell` (Types.h's giCascadeGuaranteedRadius and
        /// the rule above it). Inside it the near field is VOXELISED BY THIS
        /// cascade at every moment of any walk — its box always covers that
        /// radius — so the bounce a walker stands in is built at this cell size
        /// and not at the coarser cascade's. It is not a claim about every cone
        /// SAMPLE: a cone aimed outward leaves the box and hands over to the
        /// coarser cascade by construction. Negative means the cascade
        /// guarantees NOTHING — the camera can be outside the box before it
        /// re-centres, which is what every tier but Low did before
        /// CASCADE-STEP-1. The STILL-CAMERA bound: the motion terms are bought
        /// with a cell of slack in the step instead (see the rule). The
        /// assertable form (gi.cascades cases 16a and 16b, and
        /// scripting.e2e.world_modes over both columns).
        float guaranteedRadius = 0.0f;
        /// THE HIT READ'S LEVEL RADIUS (10-2e, giHitLevelRadius): a TLAS hit reads this cascade within it of the GI
        /// camera (max-norm); the guaranteed radius less GiStatus::hitLevelMarginCells of its cells (the outermost reads
        /// by its box).
        float hitLevelRadius = 0.0f;
        /// The world-space centre it is currently built at (quantised to its
        /// own lattice, so it is NOT the camera position).
        Vec3  centre;
        /// The camera position that placement was TAKEN from (the cascade's last re-centre: its lattice cell and the
        /// hysteresis of its next step both read it) — what a placement pin restores (GiPlacementPin, PHOTON-2).
        Vec3  builtCam;
        /// How many times this cascade has been (re)voxelised since the arm was
        /// built. At rest it does not move: a still camera scrolls nothing.
        unsigned long long rebuilds = 0;
        /// This cascade is BEHIND the camera and owes a rebuild it has not been
        /// given a frame for (at most one cascade is rebuilt per frame).
        /// A FLAG, not a queue: a rebuild always happens at the CURRENT camera,
        /// so owing two is the same as owing one. Non-zero only while the
        /// camera is outrunning the scheduler.
        int   pending = 0;
        /// THE INCREMENTAL SCROLL (ENGINE-UPGRADE-U5 stage 1). A camera scroll keeps the voxels the
        /// new box shares with the old one and voxelises only the entering slab, spread over frames
        /// by a per-frame GPU budget; the cascade's placement moves when the slab is complete.
        /// `scrolls` counts the scrolls committed that way (each is also one of `rebuilds`),
        /// `scrollFrames` the frames spent building their slabs, `scrolling` a scroll in flight now.
        unsigned long long scrolls = 0;
        unsigned long long scrollFrames = 0;
        bool  scrolling = false;
        /// TORN, THEN CLEARED (ENGINE-UPGRADE-U5 decision 14.8): a scroll's commit failed half way (the
        /// store part new box, part old), and the whole rebuild that is its only repair failed twice -
        /// so its volumes were set to EMPTY and the cascade stood down: missing bounce, never a wrong
        /// one. Cleared by the next rebuild that succeeds (the camera moving, an edit).
        bool  tornCleared = false;
        /// THE SHELLS' ENDS this cascade's last light injection marched (PHOTON-7 stage 1, L1; its read F1): D_0 ..
        /// D_{n-2} in metres from the march's start - shell j of a voxel's visibility march runs on cascade j's cells
        /// over [D_{j-1}, D_j) - the sum of 8 cells of every cascade of the chain up to j, so EVERY cascade of a chain
        /// reads the same list. Empty before the first injection.
        std::vector<float> shellEnds;
        /// THE WORK LAW'S DOOR (PHOTON-7 stage 2): over every scroll commit of this cascade, cumulative - the voxels its
        /// delta injections LIT (wrote: their direct changed) inside the entering slabs (`commitLitSlab`) and outside
        /// them (`commitLitBand`: the changed direct set), read back a few frames late (`commitCounted` = the commits
        /// whose counts landed; a commit of a cascade INSIDE a committed one, re-injected as a delta, counts too); the
        /// slabs' cells (`commitSlabCells`) and the cells the commit's direct term was COMPUTED over (`commitInjectCells`:
        /// the faces the shift moved, within the cascade's own-cell march). The old commit lit and re-solved the whole
        /// cascade, resolution^3 a commit. `deltaMarched` (PHOTON-7 stage 4): the voxels EVERY delta injection of this
        /// cascade MARCHED, read back - changed or not: its commits', its re-marches inside an outer commit (only the
        /// voxels whose march reaches the outer's moved faces, stage 2's read D4), its lamps'.
        unsigned long long commitLitSlab = 0, commitLitBand = 0, commitSlabCells = 0, commitInjectCells = 0,
                           commitCounted = 0, deltaMarched = 0;
        /// THE FEEDBACK (PHOTON-7 stage 4): the bounces converge by a pass a frame over this cascade's ACTIVE 8^3 bricks -
        /// the bricks of its newest landed reading still active after its frame (`feedbackActive`), whether it owes work
        /// (an activation no landed reading answered yet, or active bricks), and cumulatively the frames it ran a pass,
        /// the bricks those passes ran, the bricks that STOPPED (the tail of their series under a code) and RESUMED (what
        /// their gather could see moved them a code), the bricks whose pass count reached 255 (a door, never a stop) and the
        /// readings that landed.
        unsigned feedbackActive = 0;
        /// ...and of that reading, the bricks STOPPED ON THE OPERATOR'S BOUND: their contraction was never measured from
        /// two real moves, so they resume and stop by the most they re-emit (PHOTON-7 stage 5's read A2) - a door.
        unsigned feedbackOnPrior = 0;
        bool feedbackOwed = false;
        unsigned long long feedbackPasses = 0, feedbackBricksRan = 0, feedbackStops = 0, feedbackResumes = 0,
                           feedbackSaturated = 0, feedbackReadings = 0;
        /// ...THE BUDGET'S CARRY (GiQualityFacts::lightWorkFrames): the active bricks the frames' budgets carried to a
        /// later frame (summed over the readings), and the frames this cascade's owed relight was carried unfinished.
        unsigned long long feedbackCarried = 0, relightCarried = 0;
        /// ...and the relight's rounds completed: its sweep went round the owed box once (cumulative).
        unsigned long long relightRounds = 0;
        /// ...THE RELIGHT'S SPEND (stage 5's read B1): the voxels the spread relit (cumulative), and the cap of its
        /// floor credit at its last owed frame - one 4-plane slab of the owed box or one floor, the larger (a frame
        /// relights at most its credit plus the frame's pool, under GiStatus::relightCount).
        unsigned long long relightVoxels = 0, relightSlab = 0;
        /// THE REGION REBUILDS (PHOTON-7 stage 8A): the region rebuilds served (an edit's box or a mover's, re-voxelised
        /// in place - an edit's also counts in `rebuilds`, the movers' never), the movers' alone, the cells they
        /// re-voxelised (cumulative), and the cascade's debt left (boxes owed to its geometry, to its direct term).
        unsigned long long regionRebuilds = 0, moverRegionRebuilds = 0, regionCells = 0;
        unsigned regionOwed = 0, marchOwed = 0;
        /// THE REGION'S OWN BOUNCE (A2): the passes its rebuilds ran in their frames before their own convergence stopped
        /// them (read back with the feedback's readings; cumulative, and the last frame's - at most 32 a rebuild).
        unsigned long long regionBouncePasses = 0;
        unsigned regionBounceLast = 0;
        /// THE LAG (PHOTON-7 stage 2): the frames from the frame the camera's step test first owed this cascade a
        /// move to the rebuild (a scroll's commit, or a whole rebuild) that placed it for the camera - the last one
        /// and the most since the chain was built.
        unsigned long long lagLast = 0, lagMax = 0;
        /// THE READINGS BELOW ARE THE DEVICE'S OWN COUNTS (ATOM P4b, A5b §3): the
        /// gather that writes the voxeliser's records on the GPU counts as it
        /// writes, into a small readout collected without a stall and read here.
        /// Nothing on the CPU walks the scene to produce them.
        ///
        /// How many GI items this cascade's LAST GATHER put into its box: the
        /// ones with at least one partition inside it — so it follows the
        /// cascade as it scrolls, and reads 0 for one standing in empty space.
        /// Every size is kept at every cascade (a small object by its coverage).
        int   items = 0;
        /// How many GI items passed every predicate of that gather — the attach
        /// set: the GI channel and, with an instance budget
        /// (`GiParams::cascadeInstanceCap` > 0), the budget's pick. Without a
        /// budget that is the whole GI scene and `items` is the part
        /// of it this cascade's box reached.
        int   attached = 0;
        /// CPU milliseconds of that same rebuild (the submission cost on the
        /// frame's own thread). The GPU half is NOT here and cannot be: a
        /// timestamp pair is read back once the GPU has finished the frame, so
        /// it is reported where a late number belongs — the monitor's `vct.cascadeN`
        /// cacheWork rows (fork 1a81f866a+1bccc3f93 (was 0027)).
        float lastCpuMs = -1.0f;
        /// WHICH MESH LOD LEVELS THE ATTACH SET TOOK (ATOM stage 1's hand-off,
        /// ATOM P4 / AT-A10): `voxelLevels[L]` = SUBMESH PARTITIONS at level L,
        /// index 0 the authored geometry, trailing zeros trimmed. A partition is
        /// a 2,001-index run of a level (the voxeliser rejects a whole partition
        /// by its AABB), so a mesh over 667 triangles contributes several
        /// entries for one item; for a scene of meshes with no baked chain it is
        /// `{N}` at level 0.
        ///
        /// The level is decided by THIS CASCADE'S CELL and the mesh's baked
        /// error, never by the camera: the LOD bias (Scene::setLodBias) moves
        /// what is DRAWN and must not move this. (`lodLevels`, the CPU's REQUEST
        /// histogram in items, is DELETED: nothing on the CPU decides a level.)
        std::vector<int> voxelLevels;
        /// HOW MANY TRIANGLES THE ATTACH SET HANDS THIS CASCADE at those levels
        /// — the currency of a voxelisation, since the raster dispatch is sized
        /// by the index count and not by the object count. It is the attach set
        /// and not the enclosed set, so it is the pair of `attached` rather than
        /// of `items`, and it is the number the far-field proxy moves: the same
        /// cascade with the LOD chain off reads the authored total.
        long long voxelTriangles = 0;
        /// HOW MANY COMPUTE DISPATCHES THE LAST BUILD ISSUED (fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065)):
        /// one per material pool of the chain's shared store per octant, each
        /// sized by the whole volume however few records the pool holds (the
        /// count is a loop bound read on the device). So this says whether a
        /// cascade is paying for the MATERIAL COUNT rather than its geometry: a
        /// scene that shares materials reads a handful whatever its size.
        /// `voxelTriangles` is the geometry half of the same bill. 0 when the
        /// build only cleared the volume (no instance source).
        long long voxelDispatches = 0;
        /// THE RECORDS THE GATHER WROTE for this cascade: one per (instance,
        /// partition) whose world box reaches the cascade's box - what the
        /// voxelise dispatches loop over. The per-partition cull is the device's.
        long long voxelRecords = 0;
        /// Records the gather DROPPED for want of capacity. Must be 0: the
        /// capacity is a bound (every instance at its finest level's partition
        /// count), so a non-zero here is a defect, and it is logged critically.
        long long voxelOverflow = 0;
        /// WHEN THE NUMBERS ABOVE WERE TAKEN (VOXEL-READOUT-1). `items`, `attached`,
        /// `voxelLevels`, `voxelTriangles`, `voxelRecords` and `voxelOverflow` are a
        /// READ-BACK of the gather, and a read-back lands a few frames after the
        /// gather it reads (up to Ogre's mDynamicBufferMultiplier frames in flight):
        /// nothing waits for the device.
        /// `voxelGathers` counts the gathers this cascade has run and
        /// `voxelReadingGather` is the one the numbers describe, so equal = the
        /// newest; a caller that needs that renders frames until they agree.
        long long voxelGathers = 0;
        long long voxelReadingGather = 0;
        /// THE NO-DRAIN SENTRY: read-back requests that SUBMITTED the queue mid-frame
        /// (a submit is where a Vulkan read-back used to wait for the whole GPU).
        /// Cumulative; 0 is the invariant (gi.readout_async).
        long long voxelReadoutFlushes = 0;
        /// THE CASCADE'S LIGHTS (P1C-LIGHT-LIST; PHOTON-7 stage 1), as its last light injection selected
        /// them from the scene's one light list, in its ONE ORDER (peak intensity x range^2, a
        /// directional first): the lights whose range reaches its box (a directional always does; a
        /// point/spot/area light whose range sphere misses the box lights nothing in it and is not
        /// counted), how many of those took one of the 128 slots (the rest - `lightsOverBudget`, the
        /// last of the order - missing from this cascade's voxels; the engine log says so once a
        /// cascade), and `lightCapacity` = the lights A VOXEL takes, the first of the order that reach
        /// it (PhotonVoxelLighting::lightBudgetFor, 16 at 128^3, 128 at 64^3).
        int lightsInRange = 0;
        int lightsInjected = 0;
        int lightCapacity = 0;
        int lightsOverBudget = 0;
        /// THE BOX OVER ITS LIGHT SLOTS (PHOTON-7 stage 1's read F4), over the cascade's life: the most lights in range
        /// of its box at any injection, and the injections whose box held more than its 128 slots - there a coarse
        /// voxel (64^3: its budget IS the slots) takes the box's first 128 rather than its own first K, so the light
        /// it holds depends on the box (L1 broken there). Measured on the three Bistros: at most 5 lights in a model.
        int lightsInRangeMax = 0;
        unsigned long long injectionsOverSlots = 0;
        /// THE BRICK MASK (PHOTON-7 stage 3; GiBrickMask): the cascade's 8^3 bricks; of the LAST landed reading, the
        /// bricks whose direct term changed by at least one code, whose frame's feedback pass moved the total by one,
        /// and either; over the cascade's life the frames that wrote a mask, the readings that landed and the frames
        /// whose reading was skipped (every read-back slot in flight).
        unsigned bricks = 0;
        unsigned bricksDirectLast = 0, bricksBounceLast = 0, bricksActiveLast = 0;
        unsigned long long brickUpdates = 0, brickReadings = 0, brickReadingsSkipped = 0;
        /// NONDET-1's DETECTOR (the measuring arm gi.foreignDetect = N codes; 0 = off, every field 0): over the landed
        /// readings of the frames since the arm came on, the largest word 0 (the injection's relative change of a brick's
        /// direct term) and word 1 (the bounce's of its total), in codes (1 % relative), and the FIRST reading over N - its
        /// light frame (0 = none), word (0 direct, 1 bounce; 0 when both cross), its LARGEST brick in that reading, its value,
        /// the bricks over N in it, and the cascade's skipped readings by then; each word's largest over the record with
        /// its brick; and THE SIGN BAR (word 0 over 100 codes: an old and a new direct term of opposite signs, READ-9):
        /// the readings that held one, and the first - its frame, largest brick, value and the bricks over 100.
        /// A word is written by the pass that CHANGES the store: a value written into the direct term by anything else
        /// shows only when the next injection of that voxel reads it as its old term.
        float foreignDirectMax = 0.0f, foreignBounceMax = 0.0f;
        unsigned foreignDirectMaxBrick = 0, foreignBounceMaxBrick = 0;
        unsigned long long foreignReadings = 0, foreignFirstFrame = 0, foreignFirstSkipped = 0;
        unsigned foreignFirstBrick = 0, foreignFirstWord = 0, foreignFirstCount = 0;
        float foreignFirstCodes = 0.0f;
        unsigned long long signReadings = 0, signFirstFrame = 0;
        unsigned signFirstBrick = 0, signFirstCount = 0;
        float signFirstCodes = 0.0f;
    };
    /// The live cascade chain, innermost first — the voxels of every Vct and
    /// VctPccHybrid scene. Empty while GI is off or the chain waits for a camera
    /// (cascadesAwaitingCamera).
    std::vector<CascadeStatus> cascades;
    /// THE FAR-FIELD PROXY, AS APPLIED: whether the cascades are voxelising the
    /// baked LOD levels (`GiParams::cascadeVoxelLod`). It is here so the arm a
    /// measurement is on has a NAME — a switch nothing can report is a switch
    /// nobody can trust. True is the default and the shipped arm.
    bool cascadeVoxelLod = true;
    /// The chain is WANTED but has not been built, because no view has tracked a
    /// camera yet — a camera-centred arm is built around the camera and there is
    /// no honest place to put it before one exists. Distinguishes "no view yet"
    /// from "the build failed", which both read as an empty `cascades` list.
    bool cascadesAwaitingCamera = false;
    /// The arm is WANTED but has not been built because an ALBEDO or EMISSIVE
    /// texture it would voxelise is still streaming (BOOTVOX-1). The voxeliser
    /// copies exactly those two slots into its texture pool, so building now
    /// stores the wrong colours and buys a second, full re-voxelisation the
    /// moment the pixels land — which is what the shipped default scene paid on
    /// every boot. The build happens on the frame the last one is resident, and
    /// the wait is BOUNDED (30 deferrals) so a decode that never completes
    /// cannot park GI: after that the arm is built without them and this reads
    /// false again. The other half of `cascadesAwaitingCamera`'s question —
    /// "the chain is empty, why?".
    bool awaitingVoxelTextures = false;
    /// WHICH COLUMN OF THE TIER TABLE THIS CHAIN WAS BUILT FROM (V1-RIG item 4):
    /// true when the view driving GI is the HEADSET'S, so the chain is the VR
    /// profile's (see GiViewProfile). It is a reading and not a request: the
    /// profile follows the driver, and entering or leaving a session rebuilds
    /// the chain once because the table changed under it.
    bool cascadeProfileVr = false;
    /// How many whole-chain rebuilds the two DIRTY_ALL guards have forced — a
    /// teleport, or a jump longer than a cascade. Cumulative over the scene's
    /// life: a re-solve of the arm does not reset it, only GI going off does.
    unsigned long long cascadeFullRebuilds = 0;
    /// Cascade rebuilds SKIPPED because the frame's budget (one per frame) was
    /// already spent. Cumulative; it is the queue pressure reading.
    unsigned long long cascadeDeferrals = 0;
    /// THE FEEDBACK'S RESTS (PHOTON-7 stage 4): the frames the chain's feedback stopped owing work - its radiance
    /// finished moving (cumulative over the scene's life) - and whether it owes work now (a relight, an unanswered
    /// activation, active bricks: giAtRest waits for it). The post-rebuild settle it replaces (LAMPREST-3) is deleted.
    unsigned long long feedbackRests = 0;
    bool feedbackOwed = false;
    /// THE ONE WRITER (PHOTON-WRITER-1). Every write to a voxel volume's light
    /// goes through one engine function, and a volume is injected AT MOST ONCE
    /// per frame. `chainInjectionRefusals` counts the second injections that
    /// latch refused over the scene's life (the work is left owed, never
    /// dropped) — 0 is the invariant, and a non-zero reading names a path
    /// that asked twice. `chainInjectionsPeakFrame` is the most injections any
    /// ONE frame has spent: at most the chain's size (a from-scratch build or
    /// an at-rest tick injects every cascade once).
    unsigned long long chainInjectionRefusals = 0;
    unsigned chainInjectionsPeakFrame = 0;
    /// HOW MANY OBJECTS ARE RIDING THE MOVER CHANNEL BECAUSE THEY ARE BEING
    /// DRAGGED right now (MOVER-1, GiParams::dragMoverChannel). 0 in a still
    /// scene, 0 for ever with the rule off, and normally 1 during a drag — it
    /// is what the user has hold of. They are NOT in `mobility.movableItems`:
    /// that is what the DOCUMENT resolved, and this is what the gesture
    /// borrowed.
    int dragMovers = 0;
    /// ...and how many gestures have ENDED that way over the scene's life: one
    /// per drag, never one per frame. With `dragMovers` it is the whole reading
    /// — a number that climbs while nothing is being dragged is a defect, and a
    /// drag that never ends is a promotion that never paid its re-voxelisation.
    unsigned long long dragMoverGestures = 0;
    // ---- THE ONE GI SCHEDULER (ENGINE-UPGRADE-1 U0 3c) --------------------------
    // What decides WHEN the scene's GI answers a change — moved over from the mirror's debounce, run once per
    // drained record that carries the scene's GI (an environment frame). Cumulative counts, and the live state.
    unsigned long long schedulerPushes = 0;          ///< configurations applied (a from-scratch answer each)
    unsigned long long schedulerResolves = 0;        ///< re-solves: settles + explicit refreshes
    bool               settlePending = false;        ///< a change waits for the stability window
    unsigned           settleStableFrames = 0;       ///< environment frames it has held still so far
    bool               gestureActive = false;        ///< the drained record's gesture (FrameInputScene::gesture)
    /// Scrolls where MORE THAN HALF of the cascade's volume was new — the
    /// second DIRTY_ALL guard, counted: such a scroll still takes the slab path
    /// (ENGINE-UPGRADE-U5), but its slab is most of the volume, so it is the
    /// reading that says how much of the walking cost the copy is NOT saving.
    unsigned long long cascadeDirtyMajority = 0;
    /// Incremental scrolls dropped before their commit (ENGINE-UPGRADE-U5 stage 1): an edit
    /// reached the scroll's target box while its slab was being built, or a step threw. The
    /// cascade then rebuilds (an edit of its live box) or scrolls again from where it stands.
    unsigned long long cascadeScrollAborts = 0;
    /// Frames a scroll's commit waited for its delta injection's shader, built in the background (PHOTON-7 stage 2:
    /// never compiled in the commit's frame of an ASYNCHRONOUS scene - View::setAsyncShaders - on a frame that owes no
    /// complete picture; 0 in every other scene, which compiles it in its frame: the selftest asserts it, read D3).
    unsigned long long cascadeCommitShaderWaits = 0;
    /// THE RELIGHT'S COUNT A FRAME (GiQualityFacts::lightWorkFrames: the chain's voxels over that many frames) at the
    /// last owed frame: the pool plus the owed cascades' floors (stage 5's read B1; CascadeStatus::relightSlab caps a
    /// cascade's banked floor credit).
    unsigned long long relightCount = 0;
    /// Torn cascades stood down with their volumes cleared (decision 14.8; CascadeStatus::tornCleared).
    unsigned long long cascadeTornCleared = 0;
    /// THE SCREEN-PROBE GATHER's own reading (see GatherStatus). `on` false is
    /// the shipped state and every number below it is zero.
    GatherStatus gather;
    /// THE SURFACE CACHE (SURFACE-CACHE-1b). Zeroed when no cache exists, which
    /// is every scene with `GiParams::cards` off.
    CardCacheStatus cards;
    /// THE VOXEL SIDES' GATHER BY TLAS RAYS (PHOTON-7 stage 10, 10-2b; rq_voxel_sides.comp): the life of the scene's
    /// counts, read back a fixed number of frames after each dispatch - the dispatches (a cascade's pass each), the rays
    /// and what answered each (a card, a cascade, a back face, no sample, the sky), the sides gathered and those with no
    /// sample; `waits` = passes whose structure was not there (the voxel rays that pass); `routeChanges` = passes on
    /// another route than their cascade's last (each resumes the whole cascade). THE e = 0 COUNTER at the ray tier:
    /// inside a watertight room `sky` stays 0.
    struct VoxelSideCounts {
        unsigned long long dispatches = 0ull, rays = 0ull, card = 0ull, voxel = 0ull, backFaces = 0ull,
                           noSample = 0ull, sky = 0ull, sides = 0ull, emptySides = 0ull, waits = 0ull, routeChanges = 0ull;
        /// THE BURIED SIDES (PHOTON-7 stage 10-3: jahRayOriginBuried - the side's lift crosses a front face, its origin
        /// inside a solid, a two-sided cell's back at a wall's corner or a contact face): they gather nothing
        unsigned long long buried = 0ull;
    } voxelSides;
};

}}  // namespace jahshaka::engine
