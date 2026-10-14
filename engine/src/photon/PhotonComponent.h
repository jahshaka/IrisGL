// PHOTON, THE COMPONENT (ENGINE-UPGRADE-2 stage 2, SPECS/ENGINE-UPGRADE-2.md; the Terra
// pattern: a facility of ours with a type and a boundary, never OgreScene's members read
// from outside).
//
// WHAT IT IS. The scene's Photon GI — the voxel cascades, the irradiance field, the
// surface cache, the gather's inputs — as ONE object with ONE interface. Every reader
// outside Photon (the ray tier's traces and resolves, a view's binding) takes Photon's
// PRODUCTS through it; nobody reaches into the scene's members (the ray tier's friend
// access is gone: GPU-scene facts come through the substrate's view on OgreScene, the
// GI products through here).
//
// ITS GPU WORK IS THREE COMPOSITOR PASSES (PASS_CUSTOM, served by the engine's one pass
// provider, AtomPass.h):
//   photon_update  — the pass of the GI workspace right after the card capture's (the
//                    surface cache's two workspaces, positions 0 and 1): the cards' sun
//                    trace, the batch's close, the relight. The voxel cascades' and the
//                    irradiance field's dispatches are NOT in it yet: OgreGi.cpp's
//                    orchestration records them BEFORE the frame (engine.pre), each where
//                    its parameters are written — the voxeliser's compute jobs are shared
//                    across cascades and re-parameterised per dispatch, the field maps its
//                    parameter buffer per update — so a dispatch cannot be deferred alone;
//                    they move into the pass with the orchestration, at stage 4.
//   photon_trace   — per view, in front of the pass that shades the hits (the hit decode
//                    pass, or the first pass that reads the ray jobs' answer).
//   photon_resolve — per view, in front of the first pass that reads the answer: the
//                    write-back, the filters, the pass-scoped bindings.
// The compositor's own listing is the order (gi.photon_passes asserts it).
//
// ITS STATE IS ITS OWN (ENGINE-UPGRADE-2B stage 1). Every member of Photon's — the voxel
// cascades and the arm, the irradiance field, the probe grid and its budget, the GI scheduler
// (GiPlan) and the settle, the cards' inputs, the photon view — is the component's (the
// named list tests/hygiene/photon_members.txt; source.scene_no_photon_members holds it),
// and so is every method of OgreGi.cpp's that is Photon's, under its own name. The scene
// keeps what is the scene's: THE WALK (walkItems — one walk for the GI and the shadow scan;
// the component reads it through ensureGiWalk and its outputs), the item index, the change
// set's drain, the per-pass binding record, the card feed. The component reaches the scene
// through ONE back-reference (mScene) — the four facts it reads everywhere (the root, the
// manager, the engine, the error sink) bound by reference at construction, the rest through
// the scene's "as Photon reads it" seams — and the scene's public GI interface forwards to it.
// NOTHING OUTSIDE THE COMPONENT TOUCHES ITS STATE (stage 2): the scene and the engine go
// through THE INTERFACE below (its reads and its events) and its STEPS; the walk hands the
// component a record (WalkRecord) instead of writing into it; there is no friend.
#ifndef JAHSHAKA_ENGINE_PHOTONCOMPONENT_H
#define JAHSHAKA_ENGINE_PHOTONCOMPONENT_H

#include "jahshaka/engine/Types.h"

#include <OgrePixelFormatGpu.h>
#include <OgrePrerequisites.h>
#include <OgreVector3.h>
#include <Math/Simple/OgreAabb.h>

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Ogre {
class Camera;
class CompositorWorkspace;
class ManualObject;
class ParallaxCorrectedCubemapAuto;
class PhotonIrradianceField;
struct PhotonIrradianceFieldSettings;
class PhotonVoxelLighting;
class PhotonVoxelMaterial;
class PhotonVoxelizer;
class SceneManager;
class TextureGpu;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
class SurfaceCache;
struct CardMoverFrame;
namespace detail {

class OgreEngine;
class OgreScene;
class VoxelFeed;
struct VoxelReading;

// FORWARD+ CUBEMAP-PROBE SLOTS PER CELL — the budget a scene starts with, and
// the ceiling a probe grid may raise it to.
//
// THE DEFECT THIS EXISTS FOR (owner report 2026-09-07: "hard-edged black
// rectangles crawling over the Showroom's metals"). Per-pixel PCC is culled
// through the Forward+ cluster grid, and
// `ForwardClustered::collectObjsForSlice` writes a probe into a cell only while
// `numLightsInCell->objCount[objType] < currObjsPerCell`
// (OgreForwardClustered.cpp:291-298) — past the budget the probe is SILENTLY
// DROPPED, per cell, in scene order. A cluster cell is a frustum chunk spanning
// a whole logarithmic depth slice, so it is large in world space; the shipped
// Grand Showroom's 4x2x4 grid gives every probe an influence area of about
// 7.3 x 4.1 x 7.3 units at a 5.9-unit spacing, and one far cell routinely
// intersects a dozen of them. The pixels whose own probe was dropped fall
// through to VCT cone tracing, which inside a sealed room is black — hence
// hard-edged, SCREEN-AXIS-ALIGNED rectangles that move with the camera and
// cannot be tuned away by any GI knob.
//
// MEASURED, on the shipped Grand Showroom at the rig's camera
// (position 1.5, 5, -11), fraction of hard-black pixels in two fixed boxes on
// the chrome spheres, everything else identical:
//     8 slots per cell   0.742 / 0.298      <- the owner's black rectangles
//     32 slots per cell  0.000 / 0.000      <- gone, at every camera tested
//
// So the budget is DERIVED FROM THE GRID (PhotonComponent::ensureCubemapProbeSlots):
// a cell can never intersect more probes than exist, so a budget at least the
// probe count cannot drop one. It is quantised (8/16/32/64) and only ever grows
// within a scene's life, because changing it moves the Forward+ hlms property
// offsets and recompiles those shaders once — a scene that re-arms the same
// grid must not pay that twice.
//
// COST, stated: the grid buffer is cells x objsPerCell x uint16 and is uploaded
// every frame. At 16x8x24 = 3072 cells, each extra slot is +6 KiB per frame, so
// 8 -> 32 is +144 KiB and the 64 ceiling is +336 KiB. Runtime cost is nil: the
// shader loops over the probes a cell ACTUALLY holds, not over the budget.
constexpr Ogre::uint32 kCubemapProbeSlotsDefault = 8u;
constexpr Ogre::uint32 kCubemapProbeSlotsMax     = 64u;

class AtomPassProvider;
/// The ray passes' recorders (OgreRayQuery.cpp: photon_trace, photon_resolve).
void registerPhotonRayPasses(AtomPassProvider *provider);
/// The update pass's recorder (OgreSurfaceCache.cpp: photon_update).
void registerPhotonUpdatePass(AtomPassProvider *provider);
/// photon_update's GI workspace (null without a surface cache): the monitor times it.
Ogre::CompositorWorkspace *photonUpdateWorkspace(const SurfaceCache *cache);


class PhotonComponent {
public:
    /// One built voxel cascade, innermost first: what a ray reads the cascade chain by.
    struct Volume {
        Ogre::PhotonVoxelLighting *lighting = nullptr;
        Ogre::PhotonVoxelizer *voxelizer = nullptr;
    };
    /// THE BACK-REFERENCE: the scene that holds the component, and the four scene facts
    /// every Photon path reads, bound BY REFERENCE to the scene's own members (never a
    /// copy: the manager pointer goes null in OgreScene::destroy, the engine is set after
    /// construction).
    PhotonComponent(OgreScene &scene, Ogre::Root *const &root, Ogre::SceneManager *const &sceneMgr,
                    OgreEngine *const &engine, std::string &error);
    ~PhotonComponent();
    PhotonComponent(const PhotonComponent &) = delete;
    PhotonComponent &operator=(const PhotonComponent &) = delete;

    // --- THE PRODUCTS ---------------------------------------------------------
    /// The built cascades, innermost first (cleared first).
    void volumes(std::vector<Volume> &out) const;
    /// The surface cache (the cards), or null where the row is off.
    SurfaceCache *surfaceCache() const { return mSurfaceCache.get(); }
    /// The chain's one voxel material store, or null before the first voxeliser.
    Ogre::PhotonVoxelMaterial *materials() const { return mVctMaterialStore; }
    /// A headset drives the scene's GI (the VR column of the quality table).
    bool driverStereo() const { return mGiDriverStereo; }

    // --- THE PASSES -------------------------------------------------------------
    static const char *const kUpdatePass;    ///< "photon_update"
    static const char *const kTracePass;     ///< "photon_trace"
    static const char *const kResolvePass;   ///< "photon_resolve"
    /// Registers the three passes' recorders and gates on the engine's pass provider
    /// (AtomPassProvider::install calls it: the provider is Ogre's one per manager).
    static void registerPasses(AtomPassProvider *provider);

    // --- ITS TYPES --------------------------------------------------------------
    /// THE WALK'S GI RECORD — what one GI walk found (OgreScene::walkItems: the scene's one
    /// walk for the GI and the shadow scan). The component's input: absorbed right after the
    /// walk ran (absorbWalk), never written by the walk into the component's state.
    struct WalkRecord {
        /// The AABBs that moved (union of each mover's old and new box), world space. Per walk;
        /// empty when still. Read in place (the probe budget's moved-cover test).
        std::vector<Ogre::Aabb> movedBoxes;
        std::vector<Ogre::Aabb> cascadeDirty;   ///< the boxes the cascades owe, in walk order
        std::vector<NodeId> promoted;           ///< stills the gesture put on the mover channel
        bool probeOnlyChanged = false;          ///< a probe-only input moved (a non-GI item, a decal)
        bool itemsAppeared = false;             ///< a GI item seen for the first time
        unsigned long long aabbReads = 0;       ///< getWorldAabbUpdated calls the walk made
        void clear() {
            movedBoxes.clear(); cascadeDirty.clear(); promoted.clear();
            probeOnlyChanged = itemsAppeared = false; aabbReads = 0;
        }
    };
    /// What the walk asks of the component: the drag-mover channel's switch, and whether a
    /// cascade chain is built (an edit then owes the chain its box).
    struct WalkInputs { bool dragMoverChannel = false; bool cascades = false; };
    /// The mover list as last walked, and the slot count it was walked at: the
    /// walk runs only on a frame whose moved set is not empty or whose slot
    /// count changed (or a moved slot now holds another node).
    struct CardMoverRec { NodeId node = 0; Ogre::Vector3 min, max; };
    /// Every slot's caster state as the cache last saw it (the node, its world
    /// transform, its box, its flags) — a still caster's OLD box when it moves,
    /// changes class or dies. Slots are renumbered by the swap-remove, so a walk
    /// compares these to the table BY NODE. Not the GPU scene's prevWorld: two
    /// updates in one frame (a reader before the frame, the frame's own) can
    /// re-stage a slot and erase it.
    /// `material` = GpuInstance::raster[0] (the PBS material word): a cut-out's
    /// shadow is its material's mask (REFLECT-MOVERS-2).
    struct CardCasterRec { NodeId node = 0; float world[12] = {}; Ogre::Vector3 min, max; Ogre::uint32 flags = 0u; uint32_t material = 0u; uint32_t maskGen = 0u; };
    // ---- THE ARM, IN STAGES (SPECS/OPEN_COVER_SPEC.md §2 A) ---------------
    //
    // WHY THE ARM IS SPLIT AT ALL, and where the split had to go. A world's
    // FIRST arm build is the single longest thing this engine does on the UI
    // thread, and until this lane it happened inside ONE frame — measured on
    // Grand Showroom 2 (quiet box, warm shader cache): 1,025 ms, of which the
    // cascade chain is 57 ms and the PROBE PLACEMENT is 673 (scout 69, the
    // placement fit 316, the re-create 19, the closing capture 269). A scene
    // that keeps NO probes still pays 261 ms of it, every boot and every
    // create, because the candidates are photographed before they are judged.
    // So the unit of the split is a STAGE OF THE ARM, and the probe grid is
    // three of the five; splitting the cascades instead would have moved 57 ms
    // of a second.
    //
    // A `FramePace::Streaming` frame spends exactly one stage and then draws,
    // so the world appears and its lighting arrives over the next few frames.
    // A `Complete` frame runs all of them back to back, in this order, which
    // is the order the un-staged function always ran them in — that is what
    // keeps every suite, thumbnail, capture and selftest byte-identical.
    enum class GiBuildStage : unsigned char {
        Idle,          ///< no staged build in flight
        ProbeScout,    ///< one photograph of the space, to place the grid in
        ProbeFit,      ///< upstream's placement over every candidate, then keep/drop
        ProbeFinish,   ///< clamp, re-create at the real resolution, the closing capture
        Field          ///< the irradiance field and the volume bookkeeping
    };
    // ---- PHOTON: the camera-centred cascade scheduler (PHOTON_SPEC P0) ------
    //
    // N camera-centred rasterising PhotonVoxelizer + PhotonVoxelLighting pairs, chained
    // through PhotonVoxelLighting::addCascade on the innermost one, scheduled BY US.
    // Upstream's VctCascadedVoxelizer was never used and is deleted from our fork
    // (ATOM-VOXEL-2, with VctImageVoxelizer): it hard-wired
    // VctImageVoxelizer (measured in spikes/photon-s1 to reproduce none of the
    // rasteriser's bounce), it rebuilds every dirty cascade in one frame with
    // no budget, and its buildRelative has no guard against a camera jump
    // longer than a cascade (three GPU losses in that spike).
    //
    // Cascade 0 IS mVctVoxelizer/mVctLighting — the head of the chain, the
    // object every pass binds (PhotonPassBinding) — so every existing binding, teardown, status
    // and irradiance-field rule keeps working unchanged; mVctCascades[0]
    // mirrors those two pointers and 1..N-1 are owned here.
    /// What a voxel gather is asked (GpuVoxelGather.h's params, in engine terms).
    struct VoxelGatherInputs {
        float cell = 0.0f;          ///< the volume's cell, metres (the level rule's sample)
        float tolerance = 0.0f;     ///< kCascadeLodCellFraction; 0 = level 0 always
        float minExtent = 0.0f;     ///< rule 2's floor; 0 = keep everything
        bool  lod = false;          ///< walk the level rule
        const std::vector<uint32_t> *budgetMask = nullptr;   ///< the budget's pick, or none
    };
    /// The tick the host asked for this frame under a chain, not yet run.
    enum class GiTickOwed { None = 0, Moving = 1, Rest = 2 };
    /// What one view of this scene can show (OgreView::syncPhotonView's report,
    /// every frame; a detach forgets the view).
    struct PhotonViewShape {
        bool passes = false;        ///< its chain carries the photon passes (the post-chain shape)
        bool rayReflect = false;    ///< ...and traces reflections (Hits)
        bool probeGather = false;   ///< ...and gathers screen probes (ScreenProbes)
        /// ...and draws both eyes (a headset's view, ChainDesc::stereo): its photon
        /// pass is instanced-stereo, and Ogre's voxel and probe visualizers are
        /// low-level materials the pin's stereo scheme does not serve (no per-eye
        /// matrix in their vertex programs) — Voxels and Probes refuse while one
        /// is live (photonViewRefusal) and its listener never draws them.
        bool stereo = false;
    };
    /// THE PROBE CACHE'S STALE SET (FIX WAVE B2; ENGINE_CACHE_POLICY_SPEC P1).
    /// One entry per probe, rebuilt with the grid. `sweepPending` is true while
    /// the probe is STALE — owes a capture because an input changed
    /// (staleProbeGrid) — and the budget spends only on those, which is what
    /// makes "every probe within ceil(probes / budget) frames of a change" a
    /// guarantee and a still scene free. (It no longer refills when it empties:
    /// that refill was the forever-sweep P1 removed.) `framesSinceUpdate` and
    /// the moved-cover test only decide the ORDER of the catch-up.
    struct ProbeSlot {
        bool     sweepPending = true;
        unsigned framesSinceUpdate = 0;
        /// WHY this probe is stale — carried from staleProbeGrid to the frame
        /// that actually captures it, so the render-loop monitor records a
        /// probe capture with the input change that justified it (or `None`).
        /// Costs one byte per probe and is written on paths that already
        /// touch the slot.
        GiStaleReason staleReason = GiStaleReason::None;
    };
    /// THE ONE GI SCHEDULER'S STATE (U0 3c — the mirror's debounce, its 19 declarations, moved here and reduced to
    /// what it decides; PhotonComponent::plan() takes it at EU-2's stage 4). Every count is in environment FRAMES —
    /// records that carried the scene's GI — never wall time (COLDGI-1).
    struct GiPlan {
        bool pushed = false;                ///< a configuration has been applied (the scheduler is running)
        bool pendingRefresh = false;        ///< a change waits for the stability window
        bool pendingInject = false;         ///< ...and it was lights or geometry: the cheap cadence runs meanwhile
        bool movableLightsMoving = false;   ///< a moving lamp moved: its cadence (owner decision O2)
        bool movableSettleOwed = false;     ///< one at-rest tick owed after a moving one (round-2 review F1)
        bool stillLampOwed = false;         ///< a still lamp moved / changed / arrived / left (until answered)
        bool movableLampOwed = false;       ///< ...a moving lamp did
        /// A mobility CLASS FLIP of an item the GI had already seen (setNodeMovable): its geometry move is ADOPTED,
        /// never armed (MobilityChange::Soft's promise; an authoring flip's rebuild is setNodeMovable's own).
        bool mobilityFlipped = false;
        unsigned stableFrames = 0;
        unsigned framesSinceLightOnly = 0;
        unsigned long long geometrySig = 0, materialSig = 0, refreshSerialSeen = 0;
        unsigned long long pushes = 0, resolves = 0, lightTicks = 0, lightTicksAtRest = 0;
        /// HOW LONG IS STILL: 15 environment frames with no change before one re-solve (a 250 ms window at 60 Hz),
        /// and the cheap re-inject every 10th while moving (1/6 s) — the debounce's constants, unchanged.
        static constexpr unsigned kStableFrames = 15u;
        static constexpr unsigned kLightOnlyEveryN = 10u;
    };
    struct VctCascade {
        Ogre::PhotonVoxelizer *voxelizer = nullptr;   ///< owned, except [0]
        Ogre::PhotonVoxelLighting  *lighting  = nullptr;   ///< owned, except [0]
        float        halfSize   = 0.0f;
        Ogre::uint32 resolution = 64u;
        float        stepCells  = 4.0f;
        /// The lattice cell this cascade is centred on (integer coordinates in
        /// units of `step`), and the world centre that resolves to.
        long long    latticeX = 0, latticeY = 0, latticeZ = 0;
        Ogre::Vector3 centre = Ogre::Vector3::ZERO;
        bool         built   = false;
        /// THIS CASCADE'S FEED (ATOM P4b): the records a gather writes on the
        /// device and this cascade's voxeliser reads, the ranges, and the readout
        /// of what it voxelised. ONE PER CASCADE so a rebuild can never overwrite
        /// records another cascade's recorded dispatches are about to read. Owned
        /// by pointer so the vector of cascades can move without copying buffers.
        /// `itemsAttached`, `itemsStale` and `attachedItems` are GONE: nothing is
        /// attached any more (the gather re-reads every predicate from the GPU
        /// scene on every rebuild), so there is no set to hold, stale or compare.
        std::unique_ptr<detail::VoxelFeed> feed;
        /// WHAT THIS CASCADE VOXELISED - items, attach set, levels, triangles - is
        /// no longer a CPU member: it is the feed's READOUT, counted on the device
        /// as the records were written and read by giStatus (`VoxelFeed::reading`).
        /// This cascade is BEHIND the camera and owes a rebuild — a flag, not a
        /// queue: a rebuild always happens at the CURRENT camera, so owing two
        /// of them is the same as owing one.
        int          pending = 0;
        /// WHY it is pending — the reason the monitor row and the event carry.
        /// `Camera` for an ordinary scroll (E0's B12/D5), the edit's own reason
        /// when the dirty path marked it (G1), so a capture can separate the
        /// cost of walking around a scene from the cost of changing it.
        GiStaleReason pendingReason = GiStaleReason::Camera;
        /// This cascade's queued rebuild came from the JUMP guard, not from an
        /// ordinary scroll — i.e. nothing of its old volume was reusable.
        /// Cleared when the rebuild is serviced, and counted there, so the
        /// counter is one per cascade per teleport rather than one per frame
        /// the camera spends far away.
        bool         jumped  = false;
        /// Consecutive failed rebuilds. One retry on the next frame, then the
        /// cascade stands down and waits for the camera to move again (round-2
        /// review F3) — a cascade that cannot build must not spend the frame's
        /// whole GI budget for ever and starve the ones that still can.
        unsigned     failures = 0;
        unsigned long long rebuilds = 0;
        /// THIS CASCADE HAS BEEN INJECTED SINCE THE LAST IN-MOTION LIGHT TICK
        /// (DRAG-1, REFLECT F2). `rebuildCascade` ends with a full-count
        /// `PhotonVoxelLighting::update`, so a cascade the scheduler rebuilt this frame
        /// already holds the answer the tick would compute — re-computing it is
        /// duplicate work, and computing a DIFFERENT one (which is what the
        /// zero-bounce moving tick did) is the pulse. Set by every rebuild,
        /// cleared by the tick that skipped on it.
        bool         injectedSinceTick = false;
        /// ...AND THE LIGHT STATE IT WAS INJECTED AT (DRAG-1 round 2, F5). The
        /// latch above says "a rebuild already computed what the tick would
        /// compute", and that is only true while nothing the INJECTION reads has
        /// changed since. A lamp that moved or was re-parameterised between the
        /// rebuild and the tick makes the rebuild's answer stale, and skipping
        /// on the latch alone would leave that cascade holding the old light
        /// until its next rebuild — which, for an outer cascade deferred behind
        /// cascade 0, is the whole drag. The tick skips only when this still
        /// equals the scene's current light-write serial.
        unsigned long long injectedAtLightSerial = 0;
        /// THE ONE-WRITER LATCH (PHOTON-WRITER-1): the writer frame
        /// (`giWriterFrame()`) this cascade was last injected in. `injectCascade`
        /// refuses a second injection of one cascade inside one frame. The two
        /// fields above STAY beside it because they say something a per-frame
        /// latch cannot: "a rebuild has injected this cascade since the last
        /// MOVING TICK", and the ticks run every few frames, so that fact spans
        /// frames.
        unsigned long long injectedFrame = ~0ull;
        float        lastCpuMs = -1.0f;
        /// The camera position this cascade was last BUILT for. The scroll test
        /// quantises BOTH on an absolute world lattice of `step * (1 -
        /// kStepHysteresis)` metres and requires the camera to be the band past
        /// the plane it left (OgreGi.cpp) — the pin's `consistentCascadeSteps`
        /// reading with the hysteresis added, which is what keeps two cascades
        /// from stepping on different frames for the same metre AND what stops a
        /// head swaying on a plane from thrashing the chain. Because the lattice
        /// is absolute, `step()` is the SUPREMUM of the travel between rebuilds,
        /// not the distance between them.
        Ogre::Vector3 builtCam = Ogre::Vector3::ZERO;
        float step() const { return stepCells * (halfSize * 2.0f / float(resolution)); }
        float cell() const { return halfSize * 2.0f / float(resolution); }
    };

    // --- THE INTERFACE: what the scene and the engine ask of it (ENGINE-UPGRADE-2B stage 2) ---
    // The reads (the arms, the parameters, the counters a status or a pass binds) and the
    // events (a lamp, a mobility flip, a structure edit closed, a material or texture the
    // scene changed). Nothing outside the component touches its state but through these.
    const GiParams &giParams() const { return mGi; }
    Ogre::ParallaxCorrectedCubemapAuto *pcc() const { return mPcc; }
    bool pccShadowed() const { return mPccShadowed; }
    Ogre::PhotonVoxelLighting *voxelLighting() const { return mVctLighting; }
    Ogre::PhotonIrradianceField *irradianceField() const { return mIfd; }
    size_t cascadeCount() const { return mVctCascades.size(); }
    Ogre::PhotonVoxelLighting *cascadeLighting(size_t i) const { return mVctCascades[i].lighting; }
    bool giDeferralPending() const { return mGiDeferralRetry; }
    /// HOW MUCH of a staged arm build is still owed, and which stage is next
    /// (OPEN_COVER_SPEC §2.1, lane OPEN-COVER-2b). The public mirror of
    /// `mGiBuildStage`, which names the stage that will run NEXT — so `Field`
    /// is one stage from done and `ProbeScout` is four. Read once per frame while
    /// a world streams in, by the indicator and by the slow-frame line.
    unsigned giBuildStagesLeft() const {
        switch (mGiBuildStage) {
        case GiBuildStage::Idle:        return 0u;
        case GiBuildStage::ProbeScout:  return 4u;
        case GiBuildStage::ProbeFit:    return 3u;
        case GiBuildStage::ProbeFinish: return 2u;
        case GiBuildStage::Field:       return 1u;
        }
        return 0u;
    }
    GiArmStage giBuildStage() const {
        switch (mGiBuildStage) {
        case GiBuildStage::Idle:        return GiArmStage::None;
        case GiBuildStage::ProbeScout:  return GiArmStage::ProbeScout;
        case GiBuildStage::ProbeFit:    return GiArmStage::ProbeFit;
        case GiBuildStage::ProbeFinish: return GiArmStage::ProbeFinish;
        case GiBuildStage::Field:       return GiArmStage::Field;
        }
        return GiArmStage::None;
    }
    unsigned materialsAwaitingTexture() const { return unsigned(mMaterialsAwaitingTexture.size()); }
    unsigned long long giRebuildCount() const { return mGiRebuilds; }
    unsigned long long giMaterialGeneration() const { return mGiMaterialGeneration; }
    SunContactDesc sunContact() const { return mSunContact; }
    void setPhotonView(PhotonView view) { mPhotonView = view; }
    PhotonView photonView() const { return mPhotonView; }
    void setPhotonVoxelCascade(int cascade) { mPhotonVoxelCascade = cascade < 0 ? -1 : cascade; }
    int photonVoxelCascade() const { return mPhotonVoxelCascade; }
    /// A lamp event for the GI scheduler (GiPlan): one lamp of the given class, or every lamp
    /// (an incomplete change set cannot say which one moved).
    void noteLampChanged(bool movable) { (movable ? mGiPlan.movableLampOwed : mGiPlan.stillLampOwed) = true; }
    void noteLampsChanged() { mGiPlan.stillLampOwed = mGiPlan.movableLampOwed = true; }
    /// An item the GI walk has seen changed its mobility class (setNodeMovable).
    void noteMobilityFlipped() { mGiPlan.mobilityFlipped = true; }
    /// Would an invalidation now cost a rebuild? (an arm is built and none is owed yet)
    bool giArmWouldRebuild() const { return !mGiCachesDirty && (mPcc || mVctVoxelizer || mIfd); }
    void markGiCachesDirty() { mGiCachesDirty = true; }
    void releaseSurfaceCache();
    void countProbeGateCrossing() { ++mProbeGateCrossings; }
    /// A material whose bound texture is still streaming: parked for settleTextureResidency
    /// (once per material; the voxel half ORs in).
    void awaitTexture(MaterialId mat, bool voxelInput) {
        for (auto &e : mMaterialsAwaitingTexture)
            if (e.first == mat) { e.second = e.second || voxelInput; return; }
        mMaterialsAwaitingTexture.push_back({ mat, voxelInput });
    }
    /// A slot composed before the store converted its datablock: converted by the next build.
    void queueMaterialSlot(uint32_t slot) const { mVctPendingMaterialSlots.push_back(slot); }
    /// The scene's outermost structure edit closed: the invalidation it collected, once.
    void structureEditClosed();
    /// THE WALK, absorbed: the record of a walk that ran (OgreScene::ensureGiWalk returned true).
    void absorbWalk(const WalkRecord &w);
    /// The walk's inputs from the component's state.
    WalkInputs walkInputs() const { return WalkInputs{ mGi.dragMoverChannel, !mVctCascades.empty() }; }
    /// The environment moved (the sky, the ambient, the fog): every chain owes a settle and the
    /// field re-integrates (OgreScene::noteEnvironmentChanged applies the environment first).
    void noteEnvironmentChanged() {
        oweChainSettle();
        if (mIfd) reintegrateFieldAfterInjection();
    }
    // --- THE STEPS the scene and the engine call by name (the scene's GI interface forwards to
    // them; stage 2's plan() / harvest() take over the per-frame ones) ---------------------------
    // ---- Global illumination (PHOTON_SPEC.md; GI_SPEC.md phases 1-3 is its
    // ---- earlier spec, history like the Rayon name) ----
    // Instant Radiosity traces rays from ONE chosen light against the scene's
    // PBR items and plants point lights (LT_VPL) where the rays bounce.
    // The VPLs live in THIS SceneManager and ride its Forward+ clustered list —
    // nothing binds to the process-wide HlmsPbs, so IR never leaks into the
    // player/asset/preview scenes. (The sample's optional IrradianceVolume WOULD
    // be such a global binding — deliberately not used.)
    // VCT voxelizes the scene's PBR items over the GI bounds and cone-traces the
    // result; the hybrid adds a parallax-corrected cubemap probe grid whose
    // reflections blend with VCT's by distance (HlmsPbs PccVctMinDistance).
    // Every scene's arms are its own: the PBS pass of a scene binds that scene's
    // PhotonVoxelLighting, field and grid, per pass (SceneGiBinding, above).
    bool setGlobalIllumination(const GiParams &p);
    bool setGiTuning(const GiParams &p);
    void refreshGlobalIllumination(GiRefreshReason reason);
    GiStatus giStatus() const;
    /// PHOTON-M3's readback: what the voxel lighting volume holds. Blocks on a
    /// flush and a whole-volume download — a test and tool path (Engine.h).
    GiVoxelStats giVoxelStats(int cascade);
    bool giFieldAtlas(GiFieldAtlas &out);
    bool giVoxelVolume(int cascade, GiVoxelVolume &out);
    /// AT A RAY TIER THE PROBE GRID IS NOT BUILT (PHOTON-F12-PCC; OgreGi.cpp).
    /// `probeGridByRays` is the rule — the tier's facts say its reflections
    /// are traced (GiQualityFacts::rayReflections) AND this scene traces on
    /// this machine (rayTracingResolved), the gather's own shape — and
    /// `probeGridWanted` is the one question every grid path asks: the hybrid,
    /// and not a ray tier.
    bool probeGridByRays() const;
    bool probeGridWanted() const;
    /// One probe's shadow-node bytes when this grid's captures are shadowed, else
    /// 0 (PCC-BUDGET-2: OgreEngine::probeShadowNodeBytes at the live settings).
    unsigned long long probeShadowBytes() const;
    /// ...and the grid such a tier does not build, taken down (OgreGi.cpp).
    void dropProbeGridByRays();
    /// THE LIGHTING SERIAL the gather's settled history counts from
    /// (PHOTON-GATHER-1d, OgreGi.cpp): folded from the light-write serial and
    /// from what moves when an injection LANDS (the chain's settles, each
    /// cascade's rebuilds and lattice cell).
    unsigned long long giLightingSerial() const;
    bool refreshGiLighting(bool inMotion);
    /// The SAME contract for the hybrid's reflection-probe arm, and the fix for
    /// SHADOW_TOOLING_SPEC.md risk R3: when the probe captures are SHADOWED,
    /// every probe workspace instantiates JahshakaProbeShadowNode too, so deleting
    /// the definition under them leaves live CompositorShadowNodes pointing at
    /// freed memory. Reproduced as a SEGV in Hlms::preparePassHashBase
    /// (tests/shadow, mode r3) before this existed. Returns true when the arm
    /// was dropped and the caller must call the recreate below.
    /// THE SHADOW-ATLAS REBUILD'S GI HALF (PHOTON_SPEC G2). Drops exactly the
    /// WORKSPACES that instantiate the probe shadow node — each PCC probe's —
    /// and nothing else. The voxels, the cascade chain and
    /// the probes' own shapes survive: a shadow atlas growing on the 3rd, 5th or
    /// 9th casting lamp is shadow bookkeeping, not a statement about the scene's
    /// geometry. Returns false when this scene holds no such workspace.
    bool dropGiForShadowRebuild();
    void recreateGiAfterShadowRebuild();
    /// A TEXTURE THAT ARRIVED LATE IS A LATE PROBE INPUT (clean-2 lane,
    /// 2026-09-13). `setPbrTexture` stales the grid when the bind happens, but
    /// a texture is only SCHEDULED there: what the probes captured that frame
    /// was the stub, and the frame the pixels actually arrive is nobody's
    /// input. Materials bound to a not-yet-resident texture are parked here and
    /// re-noted on the frame the last of their textures becomes ready. Called
    /// once a frame per scene, right after the engine's texture drain; returns
    /// immediately (one empty() test) when nothing is parked, which is every
    /// frame after a scene has finished loading.
    void settleTextureResidency();
    /// Whether the GI flush should wait for a voxel-input texture that is still
    /// streaming (BOOTVOX-1). Counts the frames it has waited, so it is not
    /// const. See the definition in OgreGi.cpp.
    bool giVoxelTexturesPending();
    bool giEnvironmentPending();
    /// The GI arms hold by-pointer state (the material store's conversions, keyed
    /// on the datablock pointer; Instant Radiosity's caches), so every
    /// geometry/material/texture destroy path calls this BEFORE the object
    /// actually dies. The rebuild
    /// still happens ONCE at frame time (bursty destroys = one rebuild); the
    /// flush tears the whole arm down and re-voxelizes from the LIVE scene — or,
    /// under a cascade chain, marks only the cascades the change reaches (G1) —
    /// so a recycled pointer can never alias.
    void invalidateGiCaches() { invalidateGiCaches(nullptr, true); }
    /// ...with the world box the edit touched, where the call site knows it:
    /// that box is what decides which cascades owe a rebuild (G1). nullptr =
    /// "somewhere in the scene". A hide or a show (DRAG-1) calls this too: the
    /// item SET changed and every build's gather reads it off the GPU scene.
    void invalidateGiCaches(const Ogre::Aabb *where) { invalidateGiCaches(where, true); }
    /// `geometryVoxelsChanged == false` says NOTHING A VOXEL HOLDS MOVED — a
    /// LIGHT left the scene. No cascade owes a RE-VOXELISATION: a light is
    /// answered by a re-injection, which is what the dirty path does when
    /// nothing geometric is marked (G1).
    void invalidateGiCaches(const Ogre::Aabb *where, bool geometryVoxelsChanged);
    /// Called by Engine::renderOneFrame before rendering.
    void applyPendingGi();
    /// applyPendingGi as an ASYNCHRONOUS scene runs it (ASYNC-SHADERS-1): the lighting arm's
    /// compute permutations are never compiled in the frame. Under the fork's deferred
    /// dispatch a missing one goes to the background compiler and its dispatch is skipped;
    /// the arm built that frame is then not a result: the frame is HELD (AsyncHoldListener)
    /// and the arm is rebuilt once the background compiler has drained. A blocking scene
    /// runs applyPendingGi unchanged.
    void applyPendingGiDeferrable();
    /// Runs `work` (a lighting-arm build) under the deferred dispatch when the scene is
    /// asynchronous; a deferral owes the arm a whole rebuild (giDeferralPending).
    void giDeferrable(const std::function<void()> &work);
    /// The flush proper — what applyPendingGi was before the staged machine.
    void applyPendingGiFlush();
    /// Does this scene still owe a stage of a staged arm build? Read by
    /// Engine::framePaceOwesWork.
    bool giBuildOwesWork() const;
    /// Called by Engine::renderOneFrame once the frame's GI work is decided and
    /// before it renders: records how many probes this frame re-captures
    /// (GiStatus::probeCapturesLastFrame). `drawn` = a View draws this scene
    /// this frame (an undrawn scene's dirty probes do not render).
    void latchProbeCaptures(bool drawn);
    /// Called by OgreView each frame with its camera position: the PCC probe
    /// blend tracks the viewer. No-op unless the hybrid mode is live.
    void updateGiTracking(const Ogre::Vector3 &camPos, bool driverStereo);
    /// Re-derives the Forward+ clustered depth-slice range from this camera and
    /// the scene's own extent (LIGHTING_FIX fix 8 / F-F1). Rate-limited AND
    /// hysteretic — `setForwardClustered` recreates the grid buffers, so it must
    /// never run per frame.
    void updateForwardPlusRanges(const Ogre::Camera *cam);
    /// How many lights this scene holds that Forward+ has to fit into its
    /// per-cell budget, and the budget itself (F-F2). See RenderStats.
    void forwardPlusLightCensus(unsigned &lights, unsigned &budget) const;
    /// Marks every probe of the grid stale — owed a capture the budget will
    /// spend over the next frames — and records why (P1/P6/P7). Cheap: flags
    /// only; nothing renders here. A no-op without a probe grid.
    void staleProbeGrid(GiStaleReason why);
    /// P7, materials: a visible material changed — stale the probes and, when
    /// the change reaches the voxelizer's conversion, bump the material
    /// generation (see mGiMaterialGeneration).
    void noteMaterialChanged(MaterialId id, bool voxelInputsChanged);
    /// A DATABLOCK OR TEXTURE THE MATERIAL STORE HOLDS IS DYING. `PhotonVoxelMaterial`
    /// keys its conversions on the datablock POINTER and a recycled address
    /// would alias.
    /// THE BY-POINTER ALIAS GUARD (MATERIAL-SWAP-GI-1, fork ad452604a+0338ca7f2+c4c80b5f7 (was 0081)): PhotonVoxelMaterial
    /// caches conversions by raw datablock pointer across builds, so a dying
    /// datablock is EVICTED from every live voxeliser's cache — no volume is
    /// re-voxelised for a death. With a null pointer (a caller that cannot name
    /// the datablock) it falls back to marking every cascade's voxels fresh.
    void noteGiDatablockDied(Ogre::HlmsDatablock *dying = nullptr);
    /// The movement quantum for one item's world AABB (a 64th of its own
    /// largest extent), and "did this AABB move by at least that much?". Shared
    /// by giGeometryKey and walkItems so the GI scheduler and
    /// the probe round-robin can never disagree about what moved.
    static float giAabbQuantum(const Ogre::Aabb &a);
    static bool  giAabbMoved(const Ogre::Aabb &before, const Ogre::Aabb &after);
    /// Deletes every GI object. Safe to call twice; must run BEFORE the
    /// SceneManager dies (probe workspaces, GI camera).
    void teardownGi();
    /// THE ONE GI SCHEDULER (U0 3c; OgreGi.cpp): run by the drain once per record that carries the scene's GI.
    void planGi(const FrameInputScene &part, bool pushed);
    std::string photonViewRefusal(PhotonView view) const;
    /// ONCE A FRAME PER DRAWN SCENE (renderOneFrame, after updateSurfaceCache):
    /// the scene half of the view — Ogre's voxel visualizer on the finest cascade
    /// that holds the camera (Voxels), Ogre's probe visualizer on the field
    /// (Probes), the cards' quads from the cache's table (Cards) — each put up,
    /// followed and taken down as the view and the GI arms move. Free when Off
    /// and nothing is up.
    void syncPhotonView();
    /// Every drawable down (the scene's teardown, before the GI arms and the
    /// SceneManager; idempotent).
    void releasePhotonView();
    /// `presents`: a view a person watches — a window, or a headset's eye pair (an
    /// offscreen target only because the runtime copies it); an editor's shot is
    /// neither and is forgotten here.
    /// The cards' frame: the cache built, fed and updated (engine.cards, once a frame).
    void updateSurfaceCache();
    void notePhotonView(const void *view, bool presents, const PhotonViewShape &shape);
    bool readCardTexel(NodeId node, unsigned card, float u, float v,
                       CardSample &out);
    bool readCardAt(const Vec3 &world, const Vec3 &normal, CardSample &out,
                    NodeId onlyNode = 0);
    bool dumpCardAtlas(const std::string &prefix, std::string &err);
    /// THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4) — the scene's in-frame
    /// answers to the cache (OgreGpuScene.cpp: the traced shadow-casting movers,
    /// the frame's moved set, the still casters a transform write moved) and the
    /// ray tier's trace and timestamps (OgreRayQuery.cpp).
    bool cardMoverFrame(CardMoverFrame &out);
    /// THE SCREEN-PROBE GATHER (GATHER-1a). Both are defined in
    /// OgreRayQuery.cpp — like the tier's own members, so that not one line
    /// of the ray tier lives in a TU that does not include Vulkan — and both
    /// answer for the SCENE, not for a view: the row is the project's and the
    /// machine's, and every view of the scene that carries a prepass gathers.
    ///
    /// With the row off the tier records no dispatch, the Component allocates
    /// nothing, the Hlms listener sets no property and no pixel moves.
    bool probeGatherWanted() const;
    /// ...and the gather's REST KEY (OgreRayQuery.cpp): that serial, the
    /// geometry's movement epoch and the surface cache's captures and relights.
    unsigned long long gatherRestKey() const;
    /// The settle's key: the discontinuities only (OgreRayQuery.cpp).
    unsigned long long gatherRestartKey() const;
    /// HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1). The row is stored here and
    /// read by the view (the chain's prepass) and the ray tier (the job);
    /// `sunContactWanted` is the row resolved against the machine, defined in
    /// OgreRayQuery.cpp beside `probeGatherWanted` for the same reason.
    void setSunContact(const SunContactDesc &d);
    bool sunContactWanted() const;
private:
    OgreScene &mScene;
    Ogre::Root *const &mRoot;
    Ogre::SceneManager *const &mSceneMgr;
    OgreEngine *const &mEngine;
    std::string &mError;
    // Photon's constants.
    /// HOW MANY INJECTION SWEEPS AN AT-REST TICK SPENDS over a cascade chain:
    /// ONE, and it is DERIVED (PHOTON-WRITER-1 SWEEPS-3; render audit F13).
    ///
    /// The chain's radiance is the fixed point of
    ///     L_i = D_i + rho * G_i(L_i, L_{i+1}, ..., L_{N-1})
    /// where cascade i's bounce cones read its OWN volume and the cascades
    /// OUTSIDE it (`PhotonVoxelLighting::addCascade` gives cascade i the chain
    /// i+1..N-1) and never one inside it: the coupling is TRIANGULAR. Within a
    /// cascade, `PhotonVoxelLighting::update` rebuilds the light from scratch every
    /// time: the injection dispatch writes the direct term D_i over the whole
    /// volume, and each bounce pass writes direct + rho * G(total) from the
    /// volume the previous pass wrote (fork ae2ed529f+155a56bf8 (was 0076)'s Jacobi form, the
    /// direct volume kept beside it) — so one injection of cascade i is a
    /// function of its voxels, the lights, the environment and the CURRENT
    /// light of cascades i+1..N-1, and of NOTHING the volume held before.
    /// Swept OUTERMOST FIRST (the tick's order, the settle's order), cascade
    /// N-1 reads nothing outside itself and is final after its injection;
    /// cascade N-2 then reads a final N-1 and is final; and so on inward. One
    /// sweep IS the fixed point, exactly, and a second sweep can change no byte
    /// unless an INWARD coupling exists (an outer cascade reading an inner one,
    /// or a volume reading its own previous contents).
    ///
    /// THE PROOF IS A MEASUREMENT OF BYTES: gi.chain_converge (the sealed room,
    /// Medium and High) and gi.chain_converge_scenes (the default scene and
    /// Showroom 2, High and Epic) hash every cascade's light volumes after one
    /// at-rest sweep from a perturbed history and after a second one — equal,
    /// per cascade, everywhere (and a lamp that travelled leaves the bytes the
    /// same lamp jumped there leaves). The three sweeps LAMPREST-2 measured
    /// were needed BEFORE fork b028638c1 (was 0067) (a recycled Vulkan block aliased a frame
    /// in flight) and fork ae2ed529f+155a56bf8 (was 0076) (the bounce re-gathered the total and
    /// added to it: history-dependent by construction); neither is true now.
    /// The moving tick is one sweep by the same argument.
    static constexpr int kAtRestSweeps = 1;
    static const size_t kGiCascadeDirtyBoxCap = 16;
    /// HOW STILL IS STILL. Ten frames — the same 1/6 s the GI scheduler's
    /// in-motion cadence (GiPlan::kLightOnlyEveryN) uses, and chosen from the
    /// movement quantum rather than from taste: a box counts as moved when its
    /// AABB steps a 64th of its own largest extent (giAabbQuantum), so a 1 m
    /// object dragged as slowly as 20 cm/s still records a move about every
    /// fifth frame at 60 Hz. A settle shorter than that would let a slow drag
    /// spend captures between its own steps, which is the defect. It costs a
    /// single deliberate move (a scripted setPosition) ten frames of latency
    /// before its probes start catching up — against a sweep that already
    /// takes `probes / budget` frames (18 on the Mirror Room's grid).
    static constexpr unsigned kProbeMotionSettleFrames = 10u;
    /// THE CEILING ON A DEFERRAL (DRAG-1 round 2, F4). A drag ends; a KEYFRAMED
    /// object in play, a physics body settling, or a script that moves something
    /// every frame does NOT — and without a ceiling that is one endless gesture,
    /// so the probes would hold the pre-motion room for as long as it lasts and
    /// then pay a whole sweep at the end. One capture every N frames while the
    /// deferral holds keeps a long motion LIVE at a bounded price.
    ///
    /// N = 30 is derived from what a capture costs and from the sweep it feeds,
    /// not chosen: one capture is six faces at the tier's size with the probe
    /// shadow node recalculated on each, measured at about 4.3 ms (SMOKE-41), so
    /// one every 30 frames is 0.14 ms amortised — under 1 % of a 16.7 ms frame,
    /// against the 26 % the un-deferred rate was costing. And it is COMFORTABLY
    /// ABOVE the sweep length (18 frames on the Mirror Room's grid at the
    /// shipped budget of 1), which is the other half of the requirement: a
    /// ceiling below the sweep would spend a whole grid's worth of captures
    /// inside one gesture and be back where it started.
    static constexpr unsigned kProbeDeferredCaptureEvery = 30u;
    static const unsigned kGiVoxelTextureWaitFrames = 30u;
    static const unsigned kGiEnvWaitFrames = 8u;
    /// THE PHOTON CASCADE CHAIN, innermost first. Empty when no arm is built.
    /// [0] mirrors the scene's mVctVoxelizer/mVctLighting (NOT owned through here).
    std::vector<VctCascade> mVctCascades;
    std::unique_ptr<SurfaceCache> mSurfaceCache;
    Ogre::PhotonVoxelMaterial *mVctMaterialStore = nullptr;
    /// Is the view that DRIVES GI a stereo (headset) one? It picks the tier
    /// table's VR column (GiViewProfile, V1-RIG item 4) and is written by the
    /// once-a-frame driver hook.
    bool mGiDriverStereo = false;
    std::vector<CardMoverRec> mCardMovers;
    uint32_t mCardMoverSlots = 0xFFFFFFFFu;
    std::vector<CardCasterRec> mCardCasters;
    unsigned long long mCardUploadSerialSeen = 0;
    /// The lighting arm met a permutation still building (applyPendingGiDeferrable).
    bool mGiDeferralRetry = false;
    unsigned long long mGiDeferrals = 0;
    SunContactDesc mSunContact;
    /// The near/far the probe cameras were placed with — remembered so the
    /// workspaces above can be re-created exactly as they were (G2).
    float mProbeCamNear = 0.5f, mProbeCamFar = 500.0f;
    // ---- GI internals ----
    /// THE REFLECTION-PROBE GRID'S PLACEMENT REGION (a placement heuristic
    /// pending A9, never a lighting volume): the suite's pinned region, or
    /// (min == max) the world AABB of the placement-eligible GI items plus a margin.
    bool computeProbeRegion(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const;
    /// Applies kProbeGridFitMax to an AUTOMATIC fit (see the long note in
    /// OgreGi.cpp): the region's largest axis may not exceed it, and the window
    /// that survives is centred on the content rather than on the union.
    /// Never called for a pinned region.
    void clampProbeRegion(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const;
    /// The mean of the CENTRES of the GI items that fit inside `maxEdge` —
    /// "everything that is not scenery". False when the scene is only scenery.
    bool giContentCentre(float maxEdge, Ogre::Vector3 &centre) const;
    /// Builds the camera-centred cascade chain from scratch and binds it to
    /// HlmsPbs (see invalidateGiCaches). Where a probe grid is wanted, also
    /// places it over computeProbeRegion.
    /// TRUE when the arm was actually (re)built (see the definition): the
    /// chain-shape debt is cleared by a BUILD, never by a call.
    bool rebuildVct();
    /// Builds the ParallaxCorrectedCubemapAuto probe grid over `region` — the
    /// scene's own fitted box — and binds it with distance-blended VCT specular
    /// (PccVctMinDistance). Every candidate probe photographs its surroundings
    /// during the placement; the ones that saw nothing within twice their own
    /// region are DROPPED, and a grid left with none is not built at all (the
    /// sky stays the reflection source). See the long note on the definition.
    void buildPcc(const Ogre::Aabb &region);
    /// Where a staged build has got to. `Idle` outside one.
    GiBuildStage mGiBuildStage = GiBuildStage::Idle;
    /// True for the duration of ONE `rebuildVct` call that is allowed to park
    /// after the cascade arm instead of running the probe stages inline.
    bool mGiStageBuild = false;
    /// True while a stage is running, so the invalidation funnel does not
    /// abandon the build the stage is making.
    bool mInGiStage = false;
    /// The volume the staged probe stages are relative to — `rebuildVct`'s
    /// `aabb`, kept because the stages run in later frames.
    Ogre::Aabb mGiStagedVolume;
    /// Spends ONE stage of a staged arm build. Returns true when the build is
    /// finished (the stage machine is back to Idle).
    bool stepStagedGiBuild();
    /// The probe grid's three stages. Each is `buildPcc`'s corresponding block,
    /// verbatim; `buildPcc` itself is the three of them in a row.
    void buildPccScout(const Ogre::Aabb &volume);
    void buildPccFit();
    void buildPccFinish();
    /// Milliseconds since the last phase split; restarts the clock.
    double pccPhaseSplit();
    /// What the three probe stages share, because they run in three frames when
    /// the build is staged. They were locals of the one function.
    bool                 mPccStageOk = false;     ///< the scout found a buildable grid
    Ogre::uint32         mPccNumProbes[3] = { 1u, 1u, 1u };
    Ogre::uint32         mPccProbeRes = 0u;
    Ogre::PixelFormatGpu mPccProbeFormat = Ogre::PFG_RGBA8_UNORM_SRGB;
    std::string          mPccWorkspaceName;
    /// scout, placement fit, keep/drop, re-create, closing capture.
    double               mPccPhaseMs[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    std::chrono::steady_clock::time_point mPccPhaseClock;
    /// Rewinds a staged build to its first stage because the world moved under
    /// it. Idempotent; see the definition for why it rewinds and does not stop.
    void restartStagedProbeBuild();
    /// Unbinds (if this scene owns the binding) and deletes the probe grid.
    void destroyProbeGrid();
    /// True when this scene should have a field: the toggle resolves on AND the
    /// mode is one that produces a PhotonVoxelLighting to feed it.
    bool ddgiWanted() const;
    /// Creates + initializes the field over the CURRENT voxel volume, converges
    /// it in one dispatch, binds it to every PBS pass (SceneGiBinding, read by
    /// PhotonPassBinding) and takes the process-wide binding. No-op (and unbinds) when ddgiWanted() is false. Called at the
    /// end of rebuildVct — a VCT (re)build invalidates the
    /// field entirely, which upstream answers with re-initialize, not reset.
    void buildIrradianceField();
    /// Unbinds (if this scene owns the binding) and destroys the field. Called
    /// first in teardownVct, and by buildIrradianceField before it rebuilds.
    void teardownIrradianceField();
    /// Spends the frame's convergence budget on an in-flight re-converge.
    /// No-op when the field is converged, when there is no field, or when the
    /// update budget is 0 (paused). Called once a frame from updateGiTracking.
    void updateIrradianceField();
    void oweCascade0FieldFollow(GiStaleReason reason);
    /// Per-axis PROBE COUNTS for a field over `size`, each a power of two
    /// (upstream only ASSERTS that, and the assert is compiled out of our
    /// release engine) and together kIfdTotalProbes. Fitted from the volume's
    /// ASPECT: upstream's 32x8x32 default bands visibly on a room-shaped volume
    /// because the Y spacing is twice the X/Z one (P0 spike §9b).
    static void ifdProbeCounts(const Ogre::Vector3 &size, Ogre::uint32 outCounts[3]);
    /// The probes-per-frame a re-converge may spend, given the update budget —
    /// clamped to the engine's dispatch rule. THE CLAMP IS MANDATORY, not
    /// defensive: `numRays = ppf * depthRes^2 * raysPerPixel` under
    /// `threadsPerGroup` dispatches ZERO work groups, which throws out of
    /// HlmsCompute::compileShader and terminates the process, and the assert
    /// that would have caught it is compiled out of a release-built Ogre
    /// (P0 spike §4). Returns 0 for a paused budget, meaning "do not call
    /// update() at all".
    static Ogre::uint32 ifdProbesPerFrame(const Ogre::PhotonIrradianceFieldSettings &settings,
                                          int updateBudget, Ogre::uint32 totalProbes);
    /// Clamps every probe's fitted PARALLAX SHAPE into `region`, per axis
    /// (FIX WAVE defect A2). buildEnd's 1x1 averaged depth readback overshoots
    /// badly whenever anything stands between a probe and the wall behind it,
    /// and a parallax box larger than the free space the grid was fitted to is
    /// never right. See the long-form argument on the definition.
    void clampProbeShapesToRegion(const Ogre::Aabb &region);
    /// Re-applies Forward+ clustering with the CURRENT budgets and the given
    /// depth-slice range. One funnel, so the two call sites (the range update
    /// and the probe-budget growth) cannot drift apart in their other seven
    /// arguments — they did not, and the way to keep it that way is to have one
    /// place where the arguments are written.
    void applyForwardClustered(float minDistance, float maxDistance);
    /// Grows the Forward+ per-cell cubemap-probe budget to hold `probeCount`
    /// (kCubemapProbeSlotsDefault says why). Returns true if the budget moved —
    /// which costs one Forward+ shader recompile, so it is deliberately
    /// quantised and monotonic.
    bool ensureCubemapProbeSlots(size_t probeCount);
    /// Spends this frame's probe-update budget on STALE probes only: picks the
    /// highest-priority stale probes and raises `mDirty` on them (FIX WAVE B2;
    /// ENGINE_CACHE_POLICY_SPEC P1 — a grid with nothing stale spends nothing).
    /// Called from updateGiTracking with the AUTHORITATIVE camera position; a
    /// no-op at budget 0.
    void updateProbeBudget(const Ogre::Vector3 &camPos);
    /// P7 hooks: does any item that probes (probeSeesItem) or the voxelizer
    /// (kGiGeometryBit) can see use this material?
    bool materialSeenByGi(MaterialId id, bool &voxelized) const;
    /// A hash of the GI gather's boxes — "is this the same content the current
    /// automatic volume was fitted to?" (OgreGi.cpp, lane ENGINE-7 item 2).
    static unsigned long long giContentSignature(const std::vector<Ogre::Aabb> &boxes);
    /// The rule noteMaterialChanged applies, as a QUESTION — so the batched
    /// settle in settleTextureResidency can ask it per material and act ONCE
    /// (F5: one probe stale and one material-generation bump per frame,
    /// however many textures arrived together).
    bool giMaterialChangeEffect(MaterialId id, bool voxelInputsChanged, bool &bumpVoxels) const;
    /// Builds the whole chain around `camPos` from scratch. Returns the item
    /// count (0 = nothing built).
    size_t buildCascadeArm(const Ogre::Vector3 &camPos);
    /// The resolved cascade table (GiParams::cascadeSet, else the tier table).
    std::vector<GiParams::GiCascadeDesc> resolveCascadeTable() const;
    /// One cascade's whole re-voxelisation at its current centre: region ->
    /// build -> ambient -> light. `reason` only labels the monitor row.
    /// FALSE when the build threw: the caller must then put the placement back,
    /// because the voxeliser's region (read live by the shader) has already
    /// moved and the voxels have not (audit B4).
    /// `placementCommitted` (out, optional) is set when the rebuild FAILED but
    /// the cascade's new placement must NOT be put back: the replacement
    /// voxeliser's `build()` had already succeeded and the lighting already
    /// reads it, so the voxels on the GPU describe the NEW region and reverting
    /// the region would point the shader at the new voxels through the old box
    /// (audit B4's defect, in reverse). See the failure paths inside.
    bool rebuildCascade(size_t idx, GiStaleReason reason,
                        bool *placementCommitted = nullptr);
    /// THE PER-CASCADE DIRTY PATH (PHOTON_SPEC G1). An EDIT under the cascade
    /// arm, answered without tearing the chain down: the cascades the edit can
    /// be seen from are marked `pending` and spent by `updateCascades` ONE PER
    /// FRAME, with every voxeliser, texture and mesh-buffer upload KEPT. False
    /// when the chain cannot answer at all (no chain, or nothing built yet), in
    /// which case the caller takes the from-scratch `rebuildVct`.
    bool refreshCascadesFast();
    /// THE ONE WRITER (PHOTON-WRITER-1): the ONLY call to
    /// `PhotonVoxelLighting::update` in the engine. Cascade `i` of the chain. The
    /// environment first, then the injection at the document's own bounce count
    /// with the scene's own ray march: one answer per volume, whoever asks
    /// (DRAG-1's rule, stated once). FALSE, and nothing written, when this
    /// cascade has already been injected in this writer frame — the latch is
    /// what makes "two writers on one volume in one frame" (render audit F3)
    /// impossible rather than avoided; a refusal is counted
    /// (GiStatus::chainInjectionRefusals) and every caller that can be refused
    /// leaves its work owed rather than dropped.
    bool injectCascade(size_t i);
    /// Whether cascade `i` was injected in the
    /// current writer frame.
    bool injectedThisFrame(size_t i) const;
    /// THE WRITER FRAME: Ogre's frame number, which moves inside
    /// `_updateAllRenderTargets` — after every GI entry point of a frame (the
    /// mirror's push, `updateGi`, `applyPendingGi`) and before the next frame's.
    unsigned long long giWriterFrame() const;
    /// The chain's light tick, run at the writer point (refreshGiLighting owes
    /// it under a chain; updateCascades runs it after the frame's rebuild).
    void runChainTick(bool inMotion);
    GiTickOwed mGiTickOwed = GiTickOwed::None;
    /// Owe the chain an at-rest settle over its current inputs, paid one
    /// injection per frame by the scheduler, outermost first. `top` is the
    /// OUTERMOST STALE cascade (PHOTON-GATHER-1b item 6): the settle injects
    /// top, top-1, ..., 0 and leaves the cascades outside it alone — a cascade
    /// reads only the cascades outside it, so the ones outside a stale one
    /// were not made stale by it. A light or environment change owes the whole
    /// chain (the default); a refused at-rest tick owes from the refused
    /// cascade inward; a rebuild owes from the cascade INSIDE the rebuilt one
    /// (its own injection read current outer light). An unfinished debt merges
    /// by restarting from the outermost of the two.
    void oweChainSettle(size_t top = ~size_t(0));
    /// The field re-integrates once, after the LAST injection of a tick or of a
    /// settle — never per injection.
    void reintegrateFieldAfterInjection();
    /// One injection of an owed settle, out of the scheduler's frame slot.
    void payChainSettleStep();
    /// Remember the light-write serial an owed settle's steps must all see: a
    /// light that moves mid-settle — or an environment that changes, which moves
    /// the same serial (noteEnvironmentChanged) — restarts it (a settle that
    /// mixed two inputs is the fixed point of neither).
    void noteSettleInputs();
    /// Record the chain as settled for the inputs it was injected with.
    void noteChainSettled();
    /// Records WHERE the scene changed, for the dirty path above. `box` is the
    /// region the edit touched (a mover's old box united with its new one, or a
    /// vanishing item's last box); nullptr means "somewhere" and marks the whole
    /// chain. A no-op unless a cascade chain is live, so every call site can
    /// call it unconditionally. NOT called at all where nothing a voxel holds
    /// moved — a light leaving the scene — which is what makes that a
    /// re-injection and nothing else.
    void noteGiCascadeDirty(const Ogre::Aabb *box);
    /// Marks every cascade whose box intersects the recorded dirty region (or
    /// all of them when the region is unknown) `pending`, and clears the region.
    /// `why` is the reason the monitor row will carry. Returns how many cascades
    /// it marked.
    size_t markDirtyCascadesPending(GiStaleReason why);
    /// The end of a drag gesture (MOVER-1): every promoted mover goes back to
    /// being still world, owing one cascade box and one probe stale between
    /// them. Called once a frame from updateGiTracking; a no-op with no movers.
    void   endDragGesture();
    /// THE SCHEDULER, called once a frame from updateGiTracking with the
    /// authoritative camera. Re-quantises every cascade, queues the ones that
    /// moved, and spends AT MOST ONE rebuild this frame, innermost first.
    void updateCascades(const Ogre::Vector3 &camPos);
    /// Destroys cascades 1..N-1 (cascade 0 is teardownVct's own business).
    void teardownExtraCascades();
    /// THE INSTANCE BUDGET'S PICK (GiParams::cascadeInstanceCap > 0) as a bit per
    /// instance slot, for the gather to test. The ONE CPU walk left on the voxel
    /// path, and only when the opt-in budget is on: its ranking (reach, size in
    /// cells, distance) is a partial sort, and a GPU top-K was not in this lane.
    void budgetMaskFor(const VctCascade &c, std::vector<uint32_t> &mask) const;
    /// What one cascade's gather is asked: rule 2's floor, the level rule's terms,
    /// the budget's pick (filled into `mask` when a budget is on).
    VoxelGatherInputs cascadeGatherInputs(const VctCascade &c, std::vector<uint32_t> &mask) const;
    /// ONE VOXEL BUILD, fed on the device (ATOM P4b): the owed material work inside
    /// the store's bracket, the GPU scene brought current, the partitions, the
    /// gather, and the voxeliser's build over what the gather wrote.
    bool gatherAndBuild(detail::VoxelFeed &feed, Ogre::PhotonVoxelizer *voxelizer,
                        const VoxelGatherInputs &in);
    /// How many items carry the GI geometry channel (the arms' existence test).
    unsigned countGiItems() const;
    /// A feed's LAST LANDED reading - never a request, never a wait (giStatus is
    /// read from the UI thread between frames). `reading.gather` says which gather.
    static const detail::VoxelReading &currentReading(detail::VoxelFeed *feed);
    void serviceVoxelReadouts();
    /// Destroys a chain that never finished building (nothing is bound yet, so
    /// cascade 0 belongs to it too). Returns 0 — it is a JAH_CATCH value.
    size_t abandonCascadeChain();
    /// Re-centres one cascade's region on ITS cell lattice around `camPos` and
    /// records the camera it was placed for. Does not voxelise.
    void recentreCascade(VctCascade &c, const Ogre::Vector3 &camPos);
    /// Extra bounce passes for cascade `idx` — the DOCUMENT's own count, on
    /// every cascade alike (PHOTON-M1 retired the pin's "a coarser cell gets
    /// more bounces" stabilisation: a bounce adds energy, it does not recover
    /// occlusion). 0 when the document asks for a single indirect bounce,
    /// which is the default.
    Ogre::uint32 cascadeBounces(size_t idx) const;
    /// A CASCADE REBUILD LEFT THE CHAIN OFF ITS FIXED POINT (LAMPREST-3). A
    /// rebuild injects ONE cascade once, over the radiance it held somewhere
    /// else, and the GI scheduler's cadence never ticks for a camera walk — so the
    /// chain stayed one Jacobi pass from where it belonged until something
    /// unrelated happened to re-inject it (measured: 60,852 of cascade 0's
    /// 82,176 lit light-voxel bytes wrong by up to 55/255, permanently, after
    /// a walk that returned to its own starting pose).
    ///
    /// THE DEBT IS A COUNT OF INJECTIONS, NOT A FLAG, and it is paid ONE PER
    /// FRAME out of the scheduler's own one-slot budget: a sweep over the STALE
    /// cascades (mGiSettleTop + 1 injections, outermost first — the whole chain
    /// only when the whole chain is stale) and spreading it keeps a frame that
    /// only owes it because the camera moved at one cheap injection. In the
    /// tick's own order it leaves the tick's bytes, the rebuild queue keeps
    /// priority, and a walk that never ends never starves.
    int    mGiSettleStepsOwed = 0;
    /// ...and the outermost cascade that sweep starts at (PHOTON-GATHER-1b).
    size_t mGiSettleTop = 0;
    /// ...and how many frames that could pay a step have passed since the debt
    /// was last (re)started: the steps are paid on the last top+1 of the
    /// chain's size in such frames, so a partial settle finishes exactly when a
    /// whole sweep would have (the scheduler's note says why).
    int    mGiSettlePayableFrames = 0;
    /// The cascade count the debt was raised against — a chain that changed
    /// shape under an unfinished settle abandons it rather than injecting a
    /// cascade the sequence no longer describes.
    size_t mGiSettleCascades = 0;
    /// The light-write serial the running settle was raised with.
    unsigned long long mGiSettleSerial = 0;
    /// How many of those settles this scene has paid (GiStatus::chainSettles) —
    /// cumulative, so a suite can assert "one per gesture, not one per frame"
    /// from outside.
    long long mGiChainSettles = 0;
    /// WHOEVER PAYS THE INJECTION PAYS THE DEBT (fix round item 3): a full
    /// at-rest chain tick IS the settle the scheduler owes, so it clears an
    /// unfinished one rather than letting it run on top of the answer the tick
    /// just computed. (The other half of that item — letting the mirror's
    /// stability tick SKIP a chain that is already settled — was built, measured
    /// and NOT shipped: the engine's light-write serial does not see a movable
    /// lamp's pose change the way an injection does, and the skip cost
    /// scripting.e2e.movable_lamp_rest 3/255 in 10 runs of 10. The saving
    /// belongs in the mirror, which owns the signature.)
    /// The irradiance field's pass-buffer block (JahIfd_piece_ps.any).
    void          pushIfdState(const Ogre::uint32 numProbes[3]);
    /// THE FIELD SCROLLS (PHOTON-WRITER-1, FIELD-SCROLL): moves the field's window
    /// onto cascade 0's new box by whole probe spacings and integrates only the
    /// planes that entered it (PhotonIrradianceField::scrollWindow). False when the move
    /// keeps nothing (a jump of the whole grid, or a resize) - the caller then
    /// re-places and converges the whole field.
    bool          scrollIrradianceField(const Ogre::Vector3 &origin, const Ogre::Vector3 &size,
                                        GiStaleReason reason);
    /// THE FIELD FOLLOWS CASCADE 0 (PHOTON_SPEC E1 item 1). Called by the
    /// cascade scheduler whenever cascade 0 has been re-placed or re-voxelised:
    /// moves the field's volume onto cascade 0's voxel box (fork 822d538f5 (was 0044)'s
    /// `setFieldVolume`), re-binds it if that cascade's lighting re-created its
    /// light voxel textures, and re-integrates — whole when the volume MOVED
    /// (the atlases describe another place and there is no per-probe validity),
    /// progressively over the converged atlas when only the voxels changed.
    /// A no-op with no field, or with the toggle off.
    void          followCascade0Field(GiStaleReason reason);
    /// The GI items' world AABBs after the exclude flag and the extent-outlier
    /// trimming: the one place that decides which objects place the probe grid.
    std::vector<Ogre::Aabb> giItemBounds() const;
    /// The same list BEFORE the outlier trim — every GI item's world AABB as it
    /// is.
    std::vector<Ogre::Aabb> giItemBoundsRaw() const;
    /// Records (or clears) mProbeFitVolume after a placement. `fitted` is what the
    /// AUTO path resolved; a suite's pinned region clears the record instead.
    void noteProbeFitVolume(const Ogre::Aabb &fitted, bool automatic);
    /// True when a suite pinned the probe region (min != max).
    bool probeRegionPinned() const;
    /// Unbinds from HlmsPbs (when this scene owns the binding) and deletes the
    /// PCC, PhotonVoxelLighting and PhotonVoxelizer, in that order. Safe to call twice;
    /// must run BEFORE the SceneManager dies.
    void teardownVct();
    /// THE FAR-FIELD PROXY, AS APPLIED (ATOM stage 1): the scene's
    /// `GiParams::cascadeVoxelLod` met with the run-wide diagnostic latch,
    /// resolved once per `setGlobalIllumination` and reported as
    /// `GiStatus::cascadeVoxelLod`. Kept apart from `mGi` on purpose — `mGi`
    /// must keep comparing equal to what the document pushes.
    bool mCascadeVoxelLod = true;
    // VCT arm (null unless a VCT mode is live). Teardown order within the arm:
    // unbind HlmsPbs -> PCC -> PhotonVoxelLighting -> PhotonVoxelizer, all before the
    // SceneManager (probe workspaces and the GI camera live in it).
    Ogre::PhotonVoxelizer               *mVctVoxelizer = nullptr;
    Ogre::PhotonVoxelLighting                *mVctLighting  = nullptr;
    /// The scheduler's counters, reported through GiStatus. CUMULATIVE over the
    /// scene's life, which is what Types.h has always said they are: a rebuild
    /// of the arm (a settle, an edit, an atlas change) must not reset them, or
    /// any reading that spans one is a reading of nothing (audit B9). Only
    /// teardownGi — GI switched off, or the scene dying — clears them.
    unsigned long long mCascadeFullRebuilds = 0;
    unsigned long long mCascadeDeferrals = 0;
    unsigned long long mCascadeDirtyMajority = 0;
    /// One log line per scene for a cascade rebuild that threw (B4).
    bool mCascadeFailureLogged = false;
    /// WHERE THE SCENE CHANGED SINCE THE CHAIN LAST ANSWERED FOR IT (G1) — the
    /// union of each mover's old and new world AABB (the movement scan's own
    /// boxes) plus the box of every edit that carried one. A cascade is marked
    /// `pending` only when its own box intersects one of these, which is what
    /// makes dragging a chair in one corner cost the cascades that can see the
    /// chair and nothing else.
    ///
    /// BOUNDED BY MERGING, NOT BY GIVING UP (PHOTON audit F14, 2026-09-18). Past
    /// `kGiCascadeDirtyBoxCap` the list used to collapse into
    /// `mGiCascadeDirtyAll` — "the scene changed everywhere" — on the reasoning
    /// that a scene changing in sixteen places is one the whole chain has to
    /// answer for anyway. That reasoning is wrong for the commonest case there
    /// is: SEVENTEEN OBJECTS SETTLING IN ONE CORNER (a physics pile, an animated
    /// set, a crowd) produce seventeen SMALL boxes a metre apart, and answering
    /// "everywhere" marks every cascade out to the horizon, so the chain
    /// rebuilds one cascade per frame for as long as they keep moving — the
    /// exact cost the per-cascade path exists to avoid, reached by having TOO
    /// MUCH information rather than too little.
    ///
    /// At the cap a new box is therefore MERGED into the existing entry whose
    /// MARGIN (the sum of its extents) grows least — the R*-tree's metric, and
    /// deliberately not the volume: this engine's boxes go FLAT, a Plane's world
    /// AABB has zero height, and under a volume metric every coplanar box merges
    /// at zero growth however far apart it is (a floor of moving planes
    /// coalesces into one scene-spanning slab — conservative, never wrong, and
    /// the exact opposite of the point). See noteGiCascadeDirty. The list stays
    /// bounded, the description stays conservative in the only direction that is
    /// safe — a merged box covers everything both boxes did — and seventeen
    /// crates in a corner stay a corner. `mGiCascadeDirtyAll` survives for the
    /// one statement that really is scene-wide: a null box, i.e. "somewhere, I
    /// cannot say where".
    std::vector<Ogre::Aabb> mGiCascadeDirtyBoxes;
    bool mGiCascadeDirtyAll = false;
    /// The authoritative camera position the GI tracker last saw, and whether
    /// any view has ever tracked one. A cascade arm is built AROUND it, so a
    /// rebuild that arrives before the first tracked frame (every scene open:
    /// the document pushes GI before a frame renders) builds NOTHING and sets
    /// the flag below instead — building at the origin and re-centring on the
    /// first tracked frame cost a second whole-chain voxelisation per open
    /// (audit B3).
    Ogre::Vector3 mGiCamPos = Ogre::Vector3::ZERO;
    bool mGiCamPosKnown = false;
    /// A cascade arm was asked for while no camera was known. The next tracking
    /// update flags the caches dirty, and that frame's own applyPendingGi
    /// builds the chain where the camera actually is.
    bool mGiCascadeAwaitingCamera = false;
    Ogre::ParallaxCorrectedCubemapAuto *mPcc        = nullptr;
    Ogre::Camera                     *mGiCamera     = nullptr;   // PCC build + tracking
    // ---- DDGI: the IrradianceField arm (GI_UNIFIED_SPEC.md §4 P1) ---------
    // Built INSIDE the VCT arm and owned by it: the field cone-traces
    // mVctLighting's volume, holds a raw pointer to it, and binds its voxel
    // textures on every update — so it lives strictly inside that pointer's
    // lifetime (P0 spike §8) and dies FIRST in teardownVct.
    /// The DDGI field, owned, null unless GiParams::ddgi resolved on over a
    /// live VCT arm. Dies BEFORE mVctLighting (it holds that pointer). The
    /// source switch, the placement and the accessors it needs are all public
    /// API since fork 822d538f5+ae2ed529f (was 0044 and 0050), so this is the engine's own type —
    /// which also retires the slicing hazard of holding a derived type through
    /// a non-~IrradianceField.
    Ogre::PhotonIrradianceField            *mIfd          = nullptr;
    /// Convergence bookkeeping. `IrradianceField` counts processed probes
    /// internally and exposes nothing, so the engine keeps its own count —
    /// which it needs anyway to know when a re-converge has finished and to
    /// keep every dispatch a whole multiple of the batch size (the last batch
    /// of an uneven split is where the zero-work-group abort lives).
    Ogre::uint32                      mIfdTotalProbes     = 0;
    Ogre::uint32                      mIfdProbesDone      = 0;
    Ogre::uint32                      mIfdProbesPerFrame  = 0;
    /// THE VOLUME THE FIELD IS PLACED OVER, as asked for (the field enlarges it
    /// by one probe block per side for itself). Recorded so the scheduler can
    /// tell a cascade-0 re-placement from a plain re-voxelisation at the same
    /// place — the first invalidates every probe in the atlas, the second does
    /// not — and so giStatus can report where the field actually is.
    Ogre::Vector3                     mIfdVolumeOrigin = Ogre::Vector3::ZERO;
    Ogre::Vector3                     mIfdVolumeSize   = Ogre::Vector3::ZERO;
    /// The field's probe counts, kept because the volume can move without a
    /// rebuild (E1).
    Ogre::uint32                      mIfdProbeCounts[3] = { 0u, 0u, 0u };
    /// How many times the field has been re-placed onto cascade 0 since the
    /// last build (GiStatus::ifdFollows) — the counter the follow suite reads,
    /// and the honest answer to "is the field tracking the chain at all".
    unsigned long long                mIfdFollows = 0;
    /// The probes the last scroll integrated in its step frame (GiStatus::ifdScrollProbes).
    unsigned                          mIfdScrolledProbes = 0;
    /// ...and how the follows split: window scrolls (the kept probes stand) and
    /// whole re-placements (a resize or a jump of the whole grid; nothing kept).
    unsigned long long                mIfdScrolls = 0;
    unsigned long long                mIfdReplacements = 0;
    /// A FIELD FOLLOW OWED TO THE NEXT FRAME, AND WHY IT IS NOT PAID ON THE
    /// FRAME THAT MOVED THE CASCADE (lane V1-RIG item 2, LATER_OPTIMISATIONS
    /// L11, measured).
    ///
    /// Cascade 0's rebuild and the field's WHOLE re-integration used to land on
    /// one frame, and at a headset's pixel count that frame is over the 90 Hz
    /// bar: measured in a Monado session with the eye render forced to Quest
    /// Pro size (2160x2376 per eye, 10.26 Mpx of stereo target), a Medium walk's
    /// quiet frame is 5.84 ms of GPU and its step frame 11.72 ms mean / 12.45
    /// max, twelve of a hundred and sixty frames over 11.1 — one dropped frame
    /// every five metres of travel (spikes/v1-rig/COST.txt). The two halves are
    /// 2.43 ms (the cascade) and 3.40 ms (the field) and NEITHER alone crosses
    /// the bar: split across two frames the same walk peaks at 8.3 and 9.2 ms.
    ///
    /// WHAT IT COSTS IN CORRECTNESS: for exactly one frame the field describes
    /// the place cascade 0 has just left — one step of staleness, 11 ms of it,
    /// against L11's double-buffered atlas which accepts the same staleness for
    /// as many frames as a progressive re-integration takes. It is never a
    /// WRONG PLACE: the field's volume and its atlas move together, so the
    /// shader reads probes that were integrated where the field says it is.
    ///
    /// It also owns the frame's one GI slot, so the frame that pays it rebuilds
    /// no cascade — which is the whole point — and `updateIrradianceField`
    /// refuses to run a progressive batch while it is owed: cascade 0's rebuild
    /// may have re-created the light voxel textures the field's generation job
    /// binds, and the re-bind is the first thing `followCascade0Field` does.
    int          mIfdFollowOwed = 0;
    GiStaleReason mIfdFollowReason = GiStaleReason::Camera;
    /// The chain's SHAPE (its cascade count and steps) no longer matches the
    /// table it should be built from — the driver's profile changed. The dirty
    /// BOX path cannot express it, so the flush builds the chain again.
    bool mGiChainShapeDirty = false;
    /// Which column of the tier table the LIVE chain was built from, recorded at
    /// the build (`GiStatus::cascadeProfileVr`). Not the same reading as
    /// `mGiDriverStereo`, which says who is driving NOW: the two differ for the
    /// one frame a profile change is owed, and only this one is a fact about the
    /// chain the shader is sampling.
    bool mGiChainProfileVr = false;
    bool mGiCachesDirty = false;   // mesh/texture/material died while GI live; flush at frame time
    GiParams         mGi;                                  // last applied GI state
    /// What the last (re)build ACTUALLY used, recorded rather than recomputed:
    /// giStatus() must report the volume the renderer is using, not the one the
    /// scene would resolve to if it rebuilt right now (geometry moves between
    /// rebuilds — that is the whole point of the refresh verb). Equal corners =
    /// nothing built.
    Ogre::Aabb mGiLitVolume    = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    Ogre::Aabb mGiProbeRegion  = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    /// The last region the AUTO probe fit resolved (never a pinned one), kept
    /// across rebuilds for giItemBounds' hysteresis floor — an item this box
    /// covered is never trimmed by the outlier ramp, which is what makes adding
    /// an object incapable of collapsing a live scene's probe region. Invalid
    /// until the first successful auto fit; cleared whenever a suite pins the
    /// region or GI switches off.
    Ogre::Aabb mProbeFitVolume   = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    bool       mProbeFitVolumeValid = false;
    /// How many GI items the last giItemBounds() call saw. Read by
    /// noteProbeFitVolume: a fit over fewer than two items is not a population,
    /// and must not arm the hysteresis floor.
    mutable size_t mGiLastItemCount = 0;
    /// The Forward+ depth-slice range currently in force, and the frame counter
    /// that rate-limits re-deriving it (fix 8). Seeded with the values
    /// createScene passes to setForwardClustered.
    float      mFwdPlusMin = 2.0f;
    float      mFwdPlusMax = 50.0f;
    unsigned   mFwdPlusTick = 0;
    /// The Forward+ per-cell CUBEMAP PROBE budget currently in force
    /// (kCubemapProbeSlotsDefault above says why it moves). Grows with the
    /// probe grid, never shrinks while the scene lives.
    Ogre::uint32 mCubemapProbeSlots = kCubemapProbeSlotsDefault;
    /// How many probes the region clamp had to correct at the last buildPcc —
    /// i.e. how many times the 1x1 averaged-depth shrink-fit came back with a
    /// box that was not inside the space the grid was fitted to. Reported by
    /// giStatus, because "the fit is degenerate in this scene" is a fact about
    /// the scene the author can act on (GiStatus::probesClampedToRegion).
    int mProbesClampedToRegion = 0;
    /// How many material pushes have CROSSED the reflection-probe gate
    /// (fork 36162ff37+16d8e29d4 (was 0028)'s HlmsPbsDatablock::hasZeroSpecularResponse) on this
    /// scene. A crossing flushes every renderable wearing the datablock so the
    /// shader is rebuilt with or without the per-pixel probe loop; an ordinary
    /// edit that leaves the material reflective either way costs nothing. The
    /// counter exists so BOTH halves of that are gateable
    /// (GiStatus::probeGateCrossings).
    unsigned mProbeGateCrossings = 0;
    /// THE ONE MATERIAL STORE THE WHOLE CHAIN SHARES (A5b §2) and its bracket.
    /// Created lazily before the first voxeliser, destroyed after the last.
    Ogre::PhotonVoxelMaterial *vctMaterialStore();
    void destroyVctMaterialStore();
    void beginVctMaterialBracket();
    void endVctMaterialBracket();
    void bindGeometrySource(Ogre::PhotonVoxelizer *v);
    /// Inside a GI build's bracket: the owed store refresh, then every pending slot's
    /// datablock converted and its slot marked for re-composition.
    void convertPendingMaterials();
    /// A from-scratch arm: every GI-visible slot queued for conversion (the store may
    /// not have seen any of them - GI was off, or the store is new).
    void queueAllGiMaterials();
    /// Slots whose material word is "none" because the store had not converted their
    /// datablock when the scan composed them (gpuMaterialWordFor). Mutable because the
    /// composer is const; drained by the next GI build, inside its bracket.
    mutable std::vector<uint32_t> mVctPendingMaterialSlots;
    /// A material's voxel inputs may have changed (an edit, a texture that finished
    /// streaming, a from-scratch arm): the next GI build re-reads EVERY converted
    /// datablock in place (PhotonVoxelMaterial::refreshAll) before it gathers.
    bool mVctMaterialRefreshOwed = false;
    /// THE PHOTON VIEW (not saved; a new scene starts Off) and what is up for it.
    PhotonView mPhotonView = PhotonView::Off;
    int mPhotonVoxelCascade = -1;
    std::unordered_map<const void *, PhotonViewShape> mPhotonViews;   ///< the presenting views only
    /// Voxels: the cascade whose lighting shows its visualizer
    /// (PhotonVoxelLighting::getDebugVisualizer) and what it was built from — a change of
    /// any of them (a rebuild that swapped the voxeliser, re-created the textures
    /// or moved the volume) rebuilds the picture.
    Ogre::PhotonVoxelLighting *mPhotonVoxelLighting = nullptr;
    const void *mPhotonVoxelSource = nullptr;
    Ogre::TextureGpu *mPhotonVoxelTex = nullptr;
    Ogre::Vector3 mPhotonVoxelOrigin = Ogre::Vector3::ZERO;
    /// Probes: the field showing its visualizer (PhotonIrradianceField::getDebugVisualizer;
    /// the fork keeps it placed through every follow).
    Ogre::PhotonIrradianceField *mPhotonIfd = nullptr;
    /// Cards: the quads, how many vertices they hold, and the cache's table
    /// generation and frame they were built at (re-uploaded only when the table
    /// moves or the age ramp's clock ticks — kPhotonCardAgeFrames).
    Ogre::ManualObject *mPhotonCards = nullptr;
    size_t mPhotonCardVerts = 0u;
    unsigned long long mPhotonCardGeneration = ~0ull, mPhotonCardFrame = 0ull;
    /// (materialId, the albedo/emissive half of noteMaterialChanged) for
    /// materials waiting on a texture — see settleTextureResidency.
    std::vector<std::pair<MaterialId, bool>> mMaterialsAwaitingTexture;
    /// getWorldAabbUpdated calls made by OUR GI code, ever (GiStatus::
    /// giAabbReads). Mutable: two of the four readers are const signatures.
    mutable unsigned long long mGiAabbReads = 0;
    /// THE FIT, MEMOISED AGAINST THE CONTENT IT WAS MADE FOR (ENGINE-7 item 2).
    /// giItemBounds is a pure function of the gathered boxes AND of the volume
    /// the last fit produced, which is what makes a re-fit non-idempotent: it
    /// would read its own output through the hysteresis floor. Computing it
    /// ONCE PER CONTENT removes both failures at once — the ratchet (a re-fit
    /// growing its own answer) and the oscillation (a later re-solve dropping a
    /// floor an earlier one granted). `mGiFitContentNow` is what the last
    /// gather saw; the rest is the answer adopted for it.
    mutable unsigned long long mGiFitContentNow = 0;
    mutable std::vector<Ogre::Aabb> mGiFitBoxes;
    mutable unsigned long long mGiFitBoxesKey = 0;
    mutable bool mGiFitBoxesValid = false;
    /// THE GI SCHEDULER'S GEOMETRY TERM (giGeometryKey), cached
    /// against the movement epoch (clean-2 lane, 2026-09-13).
    mutable unsigned long long mGeomSigEpoch = 0, mGeomSig = 0;
    mutable bool               mGeomSigValid = false;
    mutable GiMode             mGeomSigMode = GiMode::Off;
    /// ONE WALK FOR BOTH SIGNATURES (audit D4). They are pure functions of the
    /// same boxes and the host reads them in the same statement, so a miss on
    /// either computes both — over `mItemNodes`, never the node map.
    void computeGiGeometrySignature() const;
    unsigned long long giGeometryKey() const;   ///< the scheduler's geometry term, cached on the epoch
    /// ...and the same for the Forward+ slice walk's scene bounds, which runs
    /// one frame in thirty and read every item's updated AABB to do it.
    Ogre::Aabb         mFwdPlusBounds;
    bool               mFwdPlusBoundsValid = false;
    unsigned long long mFwdPlusBoundsEpoch = 0;
    /// Same contract for the two probe-capture options (P3a/P3b): what the last
    /// buildPcc RESOLVED, after GiToggle::Auto consulted the quality dial and
    /// after the shadow half checked that a shadow node exists to recalculate.
    bool mPccHdr      = false;
    bool mPccShadowed = false;
    /// RESOLVED probe capture size (pixels per cube face) of the live grid, 0
    /// when there is none. GiParams::probeCaptureSize is the request.
    int  mPccCaptureSize = 0;
    /// How many candidate probes the last buildPcc DROPPED because their own
    /// captured depth showed nothing within twice their region (GiStatus::
    /// probesDropped — the owner's "the sky is your first reflection asset",
    /// measured per probe instead of by measuring the scene for a room).
    /// Non-zero with no grid at all means every probe saw nothing, which is a
    /// built state; zero with no grid in the hybrid means the build failed.
    int  mProbesDropped = 0;
    /// Probes the last placement kept but cut to fit the tier's VRAM budget
    /// (PCC-BUDGET-1; GiStatus::probesOverBudget).
    int  mProbesOverBudget = 0;
    std::vector<ProbeSlot> mProbeSlots;
    /// (probe index, reason) for every probe the BUDGET dirtied this frame —
    /// the monitor's record of "this capture happened because X". Cleared by
    /// latchProbeCaptures, which is where a capture becomes a fact. Empty and
    /// never grown while the monitor is off.
    std::vector<std::pair<unsigned, GiStaleReason>> mProbeDirtiedThisFrame;
    /// The resolved per-frame budget (the request clamped to the grid) — the
    /// CEILING a frame may spend on stale probes. Reported by giStatus; what a
    /// frame actually spent is mProbeCapturesLastFrame.
    int mProbeUpdatesPerFrame = 0;
    /// THE ONE WRITER'S BOOKS (PHOTON-WRITER-1). Each cascade keeps its own
    /// latch (VctCascade::injectedFrame); the injections a latch refused (GiStatus::chainInjectionRefusals — 0 is the invariant);
    /// and the injections spent in the current writer frame and the most any one
    /// frame has spent (GiStatus::chainInjectionsPeakFrame — at most the chain's
    /// size once a tick is one sweep).
    unsigned long long mGiInjectionRefusals   = 0;
    unsigned long long mGiInjectionCountFrame = ~0ull;
    unsigned           mGiInjectionsThisFrame = 0;
    unsigned           mGiInjectionsPeak      = 0;
    bool mGiInvalidationPending = false;
    bool mGiRestartPending = false;
    void applyGiInvalidation(bool restartStagedBuild);
    /// THE CARDS' LIGHT FOLD, kept between frames (SPEED-CPU, perf audit E4):
    /// updateSurfaceCache re-walks the lights only when one of the four counters
    /// it reads moved, this frame or the last (see there).
    unsigned long long mCardLightKey[4] = {};
    bool mCardLightFoldValid = false;
    bool mCardLightKeyMovedLastFrame = false;
    unsigned long long mCardLightFold = 0;
    /// The chain's at-rest light ticks that landed an injection (runChainTick) —
    /// folded into the surface cache's indirect signature.
    unsigned long long mGiRestTicks = 0;
    /// Whether the last full refresh took the chain's dirty path (G1) rather than
    /// a from-scratch build. Reported by giStatus; cleared by every such build.
    bool mGiReusedLastRefresh = false;
    /// (The GI items' last-seen world AABBs live on each Node — Node::scan,
    /// walkItems — and the boxes that moved on the most recent scan are the walk's
    /// record: mScene.giWalk().movedBoxes.)
    /// A GI item was seen for the FIRST time by a scan after the first one — a
    /// new object arrived. Not a move, but it is an input the probes must see
    /// (P1): the next budget pass stales the grid with reason Moved and clears
    /// it.
    bool mGiItemsAppeared = false;
    /// PROBE-ONLY items (P7): unlit geometry the probe faces capture
    /// (probeSeesItem) but that is not GI geometry — tracked by the same scan
    /// in their own record (Node::scan.probeBox), so they stale the probes when
    /// they move or arrive and never enter mGiMovedBoxes (the dynamic
    /// reservation) nor the scheduler's geometry term (the voxel re-solve).
    bool mProbeOnlyChanged = false;
    /// FRAMES SINCE THE LAST FRAME THAT SAW A MOVED BOX (DRAG-1, REFLECT F3).
    /// A probe capture is a 512-square six-face HDR photograph of the room with
    /// the probe shadow node recalculated on every face; a photograph of a box
    /// that is still moving is wrong before it is displayed, and the next
    /// frame's move stales it again — so during a drag the budget bought one
    /// such photograph per frame and threw every one of them away. The capture
    /// is DEFERRED to the settle instead: the staleness stays recorded (every
    /// slot keeps `sweepPending`, and `framesSinceUpdate` keeps counting, so
    /// the sweep guarantee is unchanged in shape — it is measured from the
    /// frame the content stopped moving rather than from the frame it started).
    /// Counts up from 0 on every frame that records a moved box.
    unsigned mProbeMotionQuietFrames = 0;
    /// CONSECUTIVE frames that recorded a moved box, and whether those add up
    /// to a GESTURE yet. A single deliberate move — a scripted setPosition, a
    /// nudge, a paste — is NOT "still moving" and must reach the probes in the
    /// frame it happens: at the moment of the first move nothing can know
    /// whether a second is coming, and spending one capture to find out is a
    /// capture, not a drag. The deferral therefore starts on the SECOND
    /// consecutive moving frame, which costs a gesture exactly one wasted
    /// photograph instead of one per frame.
    unsigned mProbeMotionRun = 0;
    bool     mProbeDragActive = false;
    // ---- THE MOVER CHANNEL FOR A DRAGGED STILL (MOVER-1) ------------------
    /// The nodes currently riding the mover channel for a gesture, in
    /// promotion order. Small by construction — it is what the user has hold of.
    std::vector<NodeId> mDragMovers;
    /// Gestures that have ended (a cumulative reading for gi.drag_mover and
    /// world.giStatus: one per drag, never one per frame).
    unsigned long long mDragMoverGestures = 0;
    /// A GESTURE THAT ENDED OWES ONE FULL AT-REST RE-SOLVE OF THE LIGHT, paid
    /// once its re-voxelisations have drained. MEASURED, and it is the one
    /// thing the promotion cannot get for free: with the irradiance field on,
    /// a drag that ended without it left the Mirror Room's dragged torus 31/255
    /// away from the same pose reached without the promotion, stably (the field
    /// converged during the gesture over a chain that did not contain the
    /// object and never re-converged afterwards — LAMPREST-2's latch), and one
    /// `world.refreshGi()` collapsed the difference to 201 pixels at 6/255.
    /// With the field OFF the two rules already agreed to 258 px at 9/255, which
    /// is what names the term.
    bool     mDragSettleOwed = false;
    GiPlan mGiPlan;
    /// Frames since the deferral last let a capture through. Reset when the
    /// gesture ends, so a NEW gesture never inherits a nearly-expired counter.
    unsigned mProbeDeferredRun = 0;
    /// A NON-MOTION INPUT IS OUTSTANDING (a light, a material, the sky, the fog
    /// or an explicit refresh staled the grid). Motion defers only the captures
    /// MOTION asked for: a lamp switched on while something is being dragged
    /// must still reach the probes. Cleared when no slot is pending any more.
    bool mProbeStaleBeyondMotion = false;
    /// THE MATERIAL GENERATION (ENGINE_CACHE_POLICY_SPEC P7). Bumped when a
    /// parameter the VOXELIZER reads (albedo, emissive, alpha, workflow, the
    /// albedo/emissive maps) changes on a material that GI geometry uses.
    /// A voxel input on a material a GI item wears changed. The bump marks the
    /// shared store's in-place refresh owed (PhotonVoxelMaterial::refreshAll re-reads every
    /// row and re-copies the texture pool), and the chain re-runs the SAME voxelisers:
    /// a dirty hit on every cascade. Reported as its
    /// own term (the material generation), so the GI scheduler coalesces a
    /// slider drag into one re-voxelize when it stops WITHOUT running the
    /// light re-inject cadence a material cannot need.
    unsigned long long mGiMaterialGeneration      = 0;
    /// BOOTVOX-1: how many consecutive frames the GI flush has waited for a
    /// voxel-input texture, and the cap past which it builds anyway. One frame
    /// or two is the normal case (the default scene's ground tile); the cap
    /// exists so a decode that never completes cannot park GI for ever.
    unsigned           mGiVoxelTextureWaitFrames  = 0u;
    /// ...and whether the wait for THIS pending set has already been given up
    /// on, so a texture that never becomes ready costs thirty deferrals ONCE
    /// and not thirty per rebuild for the life of the scene. Cleared the moment
    /// no voxel input is in flight.
    bool               mGiVoxelTextureWaitGaveUp  = false;
    /// Deferrals of a first arm waiting for its sky (giEnvironmentPending); the
    /// capture, convolution and SH read land within three drawn frames, so eight
    /// is a bound for a scene that is never drawn, not a budget.
    unsigned           mGiEnvWaitFrames = 0u;
    unsigned long long mGiBuiltMaterialGeneration = 0;
    /// THE PROBE CACHE's bookkeeping (ENGINE_CACHE_POLICY_SPEC P1). See
    /// staleProbeGrid and GiStatus: why the grid was last staled, a serial per
    /// stale event, the captures the last rendered frame actually made, the
    /// captures a from-scratch placement made this frame (they bypass mDirty),
    /// and how many from-scratch builds the scene has had.
    GiStaleReason      mLastStaleReason = GiStaleReason::None;
    unsigned long long mStaleSerial = 0;
    int                mProbeCapturesLastFrame = 0;
    /// FRAMES THE MOTION DEFERRAL HELD THE BUDGET (DRAG-1). Monotonic for the
    /// life of the scene; `GiStatus::probeCapturesDeferred` reports it, which
    /// is what lets a suite assert "the drag spent nothing" from outside.
    unsigned long long mProbeCapturesDeferred = 0;
    int                mPlacementCapturesThisFrame = 0;
    /// Cumulative over the scene's life (GiStatus::probePlacements /
    /// probeCapturesTotal): scouts started, and probe captures rendered.
    unsigned           mProbePlacements = 0;
    unsigned           mProbeReplacements = 0;   // GiStatus::probeReplacements
    unsigned long long mProbeCapturesTotal = 0;
    unsigned long long mGiRebuilds = 0;
    /// The lighting showing the Voxels picture is still one of the chain's and
    /// still shows it (checked by pointer before any dereference).
    bool photonVoxelLive() const;
    void photonVoxelsOff();
    void photonProbesOff();
    void photonCardsOff();
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_PHOTONCOMPONENT_H
