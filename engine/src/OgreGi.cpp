// Global illumination: voxel cone tracing (VCT), the VCT +
// parallax-corrected-cubemap hybrid and Photon's camera-centred cascade chain —
// the public verbs and the internals that drive Ogre's
// VctVoxelizer/VctLighting, IrradianceField and ParallaxCorrectedCubemapAuto.
// (Instant Radiosity was the fourth arm and was deleted 2026-09-15,
// PHOTON_SPEC §7 E2 (4).)
//
// THE LIVE PROGRAM DOC IS SPECS/PHOTON_SPEC.md. The realtime-GI program was
// called RAYON until 2026-09-13 and GI_SPEC.md / GI_UNIFIED_SPEC.md are its
// earlier specs: section numbers cited below still live in those files, and
// every one of them is HISTORY, not a second live design. The `gi*` identifiers
// keep their names by the rename's own mapping rule.
#include "EnginePrivate.h"
#include "SurfaceCache.h"   // SURFACE-CACHE phase 2: giStatus copies the Component's counters
#include <Vct/OgreVctMaterial.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>

#include <OgreHlmsManager.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreBitwise.h>
#include <OgrePixelFormatGpuUtils.h>

namespace jahshaka { namespace engine { namespace detail {

// HlmsPbs is a process-wide singleton: setVctLighting/setParallaxCorrectedCubemap
// bind globally. Exactly one scene owns that binding at a time (last enabler
// wins); teardown only unbinds when the dying scene is the owner, so a takeover
// never yanks the new owner's binding.
static OgreScene *sVctBindingOwner = nullptr;

// THE DIAGNOSTIC LATCH, READ ONCE (the lead's fix-round item 5). This file asked
// `getenv("JAHSHAKA_GI_DEBUG")` at sixteen sites, several of them per cascade
// per rebuild; the answer cannot change inside a process, so it is read on the
// first call and kept. Function-local rather than a namespace-scope global so
// the read happens on first use and not in a static initialiser whose order
// against Ogre's own is nobody's to promise.
/// EVERY VOXELISER READS THE SCENE'S GEOMETRY ROW TABLE (ATOM P4b), and it is bound
/// IMMEDIATELY BEFORE EVERY BUILD, never once at construction.
///
/// THAT IS NOT TIDINESS, IT IS THE BUG THIS LANE ALREADY PAID FOR: `growMeshTable`
/// DESTROYS and re-creates the row buffer when the mesh table doubles (32 entries,
/// then 64...), so a pointer taken at construction is dangling the moment the 33rd
/// mesh attaches. Ogre's destroy is DELAYED, so the dispatch did not fault - it read
/// freed VRAM and hung the channel: NVRM Xid 109 CTX SWITCH TIMEOUT, device lost, on
/// the selftest's second scene. Binding per build costs one pointer store.
/// THE ONE MATERIAL STORE A CHAIN SHARES (A5b §2, decision (a)).
///
/// Every voxeliser used to `new` its own, which made a material's (pool, slot) a
/// PER-CASCADE fact — `findFreeBucketFor` fills buckets in insertion order and no two
/// cascades see the same attach set in the same order — so nothing scene-wide could
/// name a material, which is precisely what one word per instance in the GPU scene's
/// table has to do. One store also means one conversion per datablock instead of one
/// per cascade, one set of pool const buffers, one texture pool, one by-pointer alias
/// cache and one eviction.
///
/// LIFETIME: created here, before any voxeliser asks for it, and destroyed in
/// `destroyVctMaterialStore` AFTER every voxeliser is gone. Measured at the code
/// (A5b's lifetime check): the ONLY reader of `getTexturePool()` is the voxeliser's
/// own dispatch binding — no `VctLighting` touches it — so "outlive every voxeliser"
/// is the whole constraint.
Ogre::VctMaterial *OgreScene::vctMaterialStore() {
    if (!mVctMaterialStore && mRoot) {
        mVctMaterialStore = new Ogre::VctMaterial(
            Ogre::Id::generateNewId<Ogre::VctMaterial>(),
            mRoot->getRenderSystem()->getVaoManager(), mRoot->getCompositorManager2(),
            mRoot->getRenderSystem()->getTextureGpuManager());
    }
    return mVctMaterialStore;
}

void OgreScene::destroyVctMaterialStore() {
    // AFTER every voxeliser: they hold a pointer they do not own, and a dispatch that
    // outlived its pool is the hazard this shape exists to remove.
    if (!mVctMaterialStore) return;
    mVctMaterialStore->destroyTempResources();   // idempotent; a throw may have skipped it
    delete mVctMaterialStore;
    mVctMaterialStore = nullptr;
}

/// THE CHAIN'S BRACKET. The store's temp resources are a 64x64 render target, a dummy
/// camera and two workspaces under FIXED names, so exactly one pair must wrap ALL the
/// builds of one rebuild. Two cascades inside one bracket cannot collide: the builds
/// are sequential on the render thread and share the one store's resources. Both halves
/// are idempotent, so a rebuild that throws between them cannot poison the next.
void OgreScene::beginVctMaterialBracket() {
    if (Ogre::VctMaterial *store = vctMaterialStore()) store->initTempResources(mSceneMgr);
}
void OgreScene::endVctMaterialBracket() {
    if (mVctMaterialStore) mVctMaterialStore->destroyTempResources();
}

void OgreScene::bindGeometrySource(Ogre::VctVoxelizer *v) {
    ensureGpuTables();
    // THE ROWS MUST BE ON THE DEVICE, NOT MERELY IN THE MIRROR. `GpuScene::update`
    // uploads them once per frame with the other tables, but a GI rebuild runs outside
    // that scan: a row still only in the mirror is read as a ZERO ADDRESS, and the
    // voxelise shader dereferences it. Measured: NVRM Xid 109 CTX SWITCH TIMEOUT and a
    // lost device on the selftest's second scene.
    mGpuScene.flushGeomRows();
    if (v) v->setGeometrySource(mGpuScene.geomBuffer());
}


// ---------------------------------------------------------------------------
// THE VOXEL FEED (ATOM P4b, A5b §2) - what replaced `addItem`.
//
// A voxel build used to be fed from the CPU: an `addItem` per GI item, a
// `VctMaterial` conversion and a bucket per sub-item, a queued instance per
// (item, octant) and a CPU-filled instance buffer uploaded per build. Now the
// GPU scene already holds every instance (transform, bounds, flags, mesh, and
// since this lane the MATERIAL WORD in ids.y), so a build is: bring that table
// current, convert whatever material it could not yet name, and let three
// compute jobs (Jahshaka/VoxelGather*) write the voxeliser's records on the
// device. Nothing per item crosses from the CPU on a rebuild.

/// Every GI-visible slot queued for conversion. A from-scratch arm calls it: the
/// store may never have seen these datablocks (GI was off, or the store is new),
/// and a slot composed while there was no store answered "none" without queueing.
void OgreScene::queueAllGiMaterials() {
    for (const Node *np : mItemNodes) {
        if (!np->item || !(np->item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (np->itemSlot != size_t(-1)) mVctPendingMaterialSlots.push_back(uint32_t(np->itemSlot));
    }
}

/// INSIDE THE STORE'S BRACKET: the owed refresh, then every pending slot's datablock
/// converted and its slot marked, so the next scan re-composes it with a real word.
///
/// THE REFRESH IS IN PLACE (VctMaterial::refreshAll): a converted datablock keeps its
/// (pool, slot) and only its row is re-written - which is what keeps every instance's
/// word valid across a material edit. A datablock whose texture CLASS changed (it
/// gained or lost a diffuse or emissive map) moves pool; only then do words change,
/// and every GI slot is re-composed.
void OgreScene::convertPendingMaterials() {
    Ogre::VctMaterial *store = vctMaterialStore();
    if (!store) return;
    // A SLOT IS RE-COMPOSED ONLY WHEN ITS WORD CHANGES. A from-scratch arm queues
    // every GI slot and a refresh re-reads every row, but a datablock that keeps its
    // (pool, slot) leaves the table exactly as it is - so a rebuild of a still scene
    // copies nothing into the GPU scene (gi.voxel_feed asserts it).
    const detail::GpuInstance *mirror = mGpuScene.live() ? mGpuScene.mirrorData() : nullptr;
    const auto restageIfChanged = [&](const Node &n) {
        if (n.itemSlot == size_t(-1) || !n.item || !n.item->getNumSubItems()) return;
        const Ogre::VctMaterial::DatablockConversionResult *r =
            store->lookupDatablock(n.item->getSubItem(0)->getDatablock());
        const uint32_t word = r ? ((r->bucketIdx << 16u) | (r->slotIdx & 0xFFFFu))
                                : detail::GpuScene::kNoMaterialWord;
        if (!mirror || n.itemSlot >= mItemNodes.size() || mirror[n.itemSlot].ids[1] != word)
            markGpuSlotDirty(n);
    };
    if (mVctMaterialRefreshOwed) {
        mVctMaterialRefreshOwed = false;
        Ogre::FastArray<Ogre::HlmsDatablock *> moved;
        store->refreshAll(&moved);
        if (!moved.empty())
            for (const Node *np : mItemNodes)
                if (np->item && (np->item->getVisibilityFlags() & kGiGeometryBit)) restageIfChanged(*np);
    }
    if (mVctPendingMaterialSlots.empty()) return;
    std::vector<uint32_t> pending;
    pending.swap(mVctPendingMaterialSlots);
    for (uint32_t slot : pending) {
        if (slot >= mItemNodes.size()) continue;            // the slot died since it queued
        const Node &n = *mItemNodes[slot];
        if (!n.item || !n.item->getNumSubItems()) continue;
        Ogre::HlmsDatablock *db = n.item->getSubItem(0)->getDatablock();
        if (!db) continue;
        if (!store->lookupDatablock(db)) store->addDatablock(db);
        restageIfChanged(n);
    }
}

/// How many items carry the GI geometry channel - the existence test of the single
/// volume and the chain (an arm over no geometry is not built), and the monitor's
/// units for the single volume. A flag test per item, nothing handed to Ogre.
unsigned OgreScene::countGiItems() const {
    unsigned n = 0;
    for (const Node *np : mItemNodes)
        if (np->item && (np->item->getVisibilityFlags() & kGiGeometryBit)) ++n;
    return n;
}

/// ONE VOXEL BUILD over the device-side feed. The caller has placed the voxeliser's
/// region and brought the scene graph current.
bool OgreScene::gatherAndBuild(detail::VoxelFeed &feed, Ogre::VctVoxelizer *voxelizer,
                               const VoxelGatherInputs &in) {
    if (!voxelizer) return false;
    // THE TABLE, CURRENT - the transforms and flags the gather reads. Composing a slot
    // whose datablock the store has not converted QUEUES it (gpuMaterialWordFor).
    ensureGpuScene(/*graphIsCurrent=*/true);
    // ONE BRACKET around the conversions AND the build: a conversion can render into
    // the store's pool, and build() binds that pool.
    beginVctMaterialBracket();
    struct BracketClose {
        OgreScene *s = nullptr;
        ~BracketClose() { s->endVctMaterialBracket(); }
    } close{ this };
    convertPendingMaterials();
    // The slots just converted, re-composed with their words (a no-op scan otherwise:
    // the epoch has not moved since the call above).
    ensureGpuScene(/*graphIsCurrent=*/true);
    Ogre::HlmsManager *hm = mRoot->getHlmsManager();
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    // Rebuilt only when the mesh set changed; its answer is "did work", not "exists".
    mGpuScene.ensurePartitions(hm, rs, Ogre::VctVoxelizer::kIndicesPerPartition);
    bindGeometrySource(voxelizer);
    // THE REFUSAL HOOK (gi.voxel_resident case 5): the state a device with no buffer
    // device addresses is in for the whole scene - no geometry the voxeliser can
    // read - which must build (an empty volume) and not crash. Read per build, like
    // the cascade fault hooks.
    if (std::getenv("JAH_VCT_REFUSE_GEOMETRY")) {
        voxelizer->setInstanceSource(nullptr, nullptr);
        feed.noteEmpty();
    } else if (!runVoxelGather(feed, voxelizer, in))
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: the voxel gather did not run (" + mError +
            ") - the volume builds empty", Ogre::LML_CRITICAL);
    voxelizer->build(mSceneMgr);
    return true;
}

const detail::VoxelReading &OgreScene::currentReading(detail::VoxelFeed *feed) {
    static const detail::VoxelReading kNone;
    if (!feed) return kNone;
    // ON DEMAND: an owed readout is requested now (the ticket commits the command
    // buffer and map() waits on its fence - a diagnostic's cost, never a frame's).
    if (feed->readoutOwed()) { feed->harvest(); feed->requestReadout(); }
    if (!feed->harvest()) feed->harvestBlocking();
    return feed->reading();
}

/// FRAME START: every feed's finished readout is collected and an owed one is
/// requested while the frame's command buffer holds nothing (VoxelFeed::markReadoutOwed).
void OgreScene::serviceVoxelReadouts() {
    if (mVctFeed) mVctFeed->serviceReadout();
    for (VctCascade &c : mVctCascades)
        if (c.feed) c.feed->serviceReadout();
}

static bool giDebug() {
    static const bool on = std::getenv("JAHSHAKA_GI_DEBUG") != nullptr;
    return on;
}

static Ogre::HlmsPbs *hlmsPbs(Ogre::Root *root) {
    return static_cast<Ogre::HlmsPbs *>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
}

/// GiToggle::Auto defers to the quality dial; Off/On pin it either way. Exists
/// so a suite can measure HDR probes and shadowed probes ONE AT A TIME instead
/// of measuring "GiQuality::High", which changes three things at once.
static bool resolveToggle(GiToggle t, bool autoValue) {
    switch (t) {
    case GiToggle::Off: return false;
    case GiToggle::On:  return true;
    case GiToggle::Auto: default: return autoValue;
    }
}

// HOW FAR A PROBE'S PARALLAX BOX MAY REACH PAST ITS OWN CELL, as a multiple of
// that cell's half-extent, before it is reported as a defect
// (GiStatus::probesExceedingCell, 2026-09-07 fix wave defect 2b).
//
// WHY IT IS NOT 1. The parallax box is a PROXY FOR THE GEOMETRY THE CUBEMAP
// CAPTURED, not a region of influence: a probe standing in a room legitimately
// sees that room's far wall, so its box legitimately reaches several cells away.
// The number that IS wrong is a box unrelated to any surface — the shrink-fit
// returning twice the room because one 1x1 averaged depth sample per face was
// meaningless. So the allowance is generous enough for "this probe can see the
// whole room" on the grids we ship (an 8-wide grid's corner probe needs ~7.5
// half-cells to reach the far wall, and the clamp keeps it inside the region
// anyway) and tight enough that a fit which escaped the room is caught.
static const float kProbeShapeCellAllowance = 8.0f;

// HOW BIG A PROBE'S PHOTOGRAPHED BOX MAY BE, AS A FRACTION OF THE VOLUME THE
// RENDERER LIT, AND STILL BE WORTH BUILDING (buildPcc's depth rule, R5-ROOM).
// At 1 the box the probe's six faces measured IS the lit world; below it the
// probe photographed a smaller space, i.e. something is near it. Measured on
// the suites' scenes: probes inside a room 0.10 - 0.55, probes with nothing
// near them 1.3 - 6.0.
//
// SAID HONESTLY: THE COMPARISON IS AGAINST THE LIT VOLUME'S OWN EXTENT, so a
// room's verdict depends on how much slack the fit leaves around its walls.
// The same roofless 10 m room keeps its four probes over the automatic +-7.43
// fit and loses them over a +-5.5 volume pinned tight around it (measured; it
// is why gi.probe_open case 13 pins the room's own +-8), and a 30 m yard with
// 6 m walls keeps none of the shipped grid's 18 while the 10 m room keeps all
// four — from inside the yard, most of what a probe sees is sky.
//
// That coupling is a STOPGAP and is stated as one. It exists because a grid,
// once it exists at all, takes the sky cubemap off EVERY material in the scene
// (one environment slot — the defect SKY-FALLBACK-1 is about), so the cost of
// keeping a marginal probe is paid scene-wide; with the sky kept as the
// fallback where no probe covers a pixel, the line could be drawn far more
// generously. R2's rays retire the question altogether by making the probe
// fallback rare.
static const float kProbeSeesGeometry = 1.0f;

// WHAT THE SCOUT PASS CAPTURES AT (buildPcc). The placement's shrink-fit reads
// ONE 1x1 AVERAGED TEXEL per cube face — the smallest mip — so the only thing
// the scout's resolution buys is how faithfully that average represents the
// face. 32 is 1024 samples per face: enough that a wall, a column or a doorway
// is in the average, small enough that the whole scout costs a fraction of one
// real capture (a real one is 256 or 512 per face, 64x-256x the pixels).
static const Ogre::uint32 kProbeScoutResolution = 32u;

// ---- DDGI (GI_UNIFIED_SPEC.md §4 P1) constants ---------------------------

// HOW MANY PROBES A FIELD HAS, total. A power of two, because the per-axis
// counts must each be one (upstream asserts it, and the assert is compiled out
// of our release engine) and the aspect fit below splits this budget between
// the axes by handing out one doubling at a time.
//
// 8192 is upstream's own default count (32x8x32) and the number the P0 spike
// measured everything at: 14.25 MB of atlas (2.00 MB irradiance R10G10B10A2 +
// 12.25 MB depth RG32F at depthRes 12), ~5 ms of GPU work to converge once,
// ~0.8 ms/frame CHEAPER than plain VCT once bound. Deliberately NOT tiered off
// GiQuality: the quality dial already moves the voxel resolution the field is
// fed FROM, and the Photon tier table (owner option (b), 2026-09-09) decided
// against a probe-count column too — every DDGI-fed tier (Medium, High, Epic)
// gets this same fitted grid; Epic's columns are bounces and dynamic probes.
static const Ogre::uint32 kIfdTotalProbes = 8192u;

// The DEPTH probe resolution and the irradiance one. Upstream's defaults, kept
// because they are what the spike measured and because depthRes is the VRAM
// knob (86% of the atlas bytes) — moving it is a tuning decision with a
// measurement attached, not a default to drift.
static const Ogre::uint8 kIfdDepthRes  = 12u;
static const Ogre::uint8 kIfdIrradRes  = 6u;
// THE FIELD'S ESTIMATOR (PHOTON-FIELD-ROTATE-1). Ogre's field settings keep ONE ray
// per depth texel (their default: 144 rays a probe at depthRes 12) in a spherical-
// Fibonacci set rotated per integration, every texel integrating every ray (DDGI's
// estimator), and a probe's value is the MEAN of its integrations until it holds
// kIfdTargetSamples. (Upstream's per-texel rays over-read a one-voxel emissive wall
// 1.7x even at 256 rays a texel - the octahedral texels are not equal solid angle -
// and any FIXED set aliases on top: the Fibonacci set unrotated reads the same wall
// at 1.52x, integration after integration; the old kIfdRaysPerPixel = 2 paid twice
// the rays for another aliasing. gi.field_thin_wall, gi.field_alias.)
//
// THE TARGET, DERIVED FROM THE STORE THE RAYS READ: the estimator need not be more
// precise than what it integrates. The field's hardest case - a source of about one
// ray's solid angle, gi.field_thin_wall's one-voxel wall - is uncertain THROUGH THE
// STORE by the trilinear half-cell at its envelope's edge: 1 - ((n-1)/n)^2 = 0.160 of
// its irradiance at n = 12 cells. K is the smallest sample count whose two-sigma
// error sits inside that: K = ceil((2 sigma / 0.160)^2) with sigma = 0.40, one
// integration's relative standard deviation there (0.33-0.35 measured on the GPU,
// 0.38-0.41 simulated) -> 25. (A 5 % target would be 64: 2.5x the refinement for a
// precision the store cannot show.) An ordinary lit room's probes read 2 % (median)
// per integration, so everywhere else the field is converged long before K. The
// cost is time, not frames: K whole-grid passes at the update budget after an event
// (about 8 K = 200 frames at budget 1), a pass skipping every probe already at K.
// Every event's own pass - a build's included - is the first sample everywhere
// (unbiased, every probe valid); a PAUSED field (budget 0) refines nothing and
// stays at that sample.
static const Ogre::uint32 kIfdTargetSamples = 25u;
// A LIGHT CHANGE REPLACES (keep 0): the history integrated the OLD light, so the
// first integration after a change is the probe's value - the latency of the old
// re-converge (one pass) - and the refinements then average the new light. What the
// field shows in between is one integration's noise: 2 % median on the leak room
// (gi.field_alias item 5); keeping k samples would divide that by sqrt(k + 1) at the
// price of showing the old light at weight k / (k + j) for j more integrations.
static const Ogre::uint32 kIfdKeepOnChange  = 0u;

// HOW FAST A RE-CONVERGE RUNS, in "field fractions per frame" at update budget
// 1. The P0 spike's cost table is the argument for converging FAST rather than
// trickling: the incremental cost of update() at any batch size from 8 to 2048
// probes was below the noise floor (<=0.1 ms), and the whole field converges in
// about 5 ms of GPU work — next to the ~2.1 ms a SINGLE live PCC probe costs.
// So a budget-1 scene re-converges its diffuse in 8 frames (~0.6 ms/frame)
// instead of spreading a barely-measurable cost over hundreds of them and
// showing stale bounce for two seconds. Higher budgets scale it linearly, and
// the batch is rounded UP to a power of two so it always divides the field.
static const Ogre::uint32 kIfdConvergeFrames = 8u;



/// How far this probe's fitted SHAPE reaches past its own AREA (its share of
/// the probe region), as a multiple of the area's half-extent — worst axis,
/// worst side. 1.0 = exactly its own cell.
static float probeShapeCellRatio(const Ogre::CubemapProbe *p) {
    const Ogre::Aabb area = p->getArea();
    const Ogre::Aabb shape = p->getProbeShape();
    const Ogre::Vector3 c = area.mCenter, h = area.mHalfSize;
    const Ogre::Vector3 smn = shape.getMinimum(), smx = shape.getMaximum();
    float worst = 0.0f;
    for (size_t ax = 0; ax < 3u; ++ax) {
        const float half = std::max(h[ax], 1e-4f);
        worst = std::max(worst, (c[ax] - smn[ax]) / half);
        worst = std::max(worst, (smx[ax] - c[ax]) / half);
    }
    return worst;
}

bool OgreScene::setGlobalIllumination(const GiParams &p) {
    JAH_TRY {
        switch (p.mode) {
        default:
        case GiMode::Off:
            teardownGi();
            mGi = p;
            mCascadeVoxelLod = p.cascadeVoxelLod && mCascadeLodAllowed;
            // Switching GI off is the user's own "start over": the hysteresis
            // floor forgets what used to be lit, so switching back on fits the
            // scene as it is now rather than as it was.
            noteGiAutoVolume(Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO), false);
            return true;

        case GiMode::Vct:
        case GiMode::VctPccHybrid:
            mGi = p;
            // THE APPLIED far-field-proxy answer (ATOM-2): the scene's request
            // met with the run-wide latch, resolved once per push and reported
            // by giStatus — never read per item, and deliberately NOT written
            // back into `mGi`, which must keep comparing equal to what the
            // document pushes or every push under the latch would rebuild.
            mCascadeVoxelLod = p.cascadeVoxelLod && mCascadeLodAllowed;
            rebuildVct();
            return true;
        }
    } JAH_CATCH(mError, false);
}

// THE TUNING PUSH (PHOTON_SPEC §7 E2 (8) / audit A F6). Constants, no
// rebuild: `ddgiIntensity` is a shader constant the field's
// listener reads (pushIfdState), `rayMarchStepScale` is read by the NEXT light
// injection (giRayMarchStepScale), and none of the three is geometry. So this
// writes them and, when a field is bound, re-pushes its constants — nothing is
// torn down, nothing re-voxelises, and no probe is staled (a probe capture does
// not contain the field's diffuse).
bool OgreScene::setGiTuning(const GiParams &p) {
    JAH_TRY {
        const bool marchMoved = p.rayMarchStepScale != mGi.rayMarchStepScale;
        mGi.ddgiIntensity     = p.ddgiIntensity;
        mGi.rayMarchStepScale = p.rayMarchStepScale;
        // THE CARD CACHE'S TWO PER-FRAME KNOBS. Written and nothing else: the
        // residency pass reads them at the head of the next frame, so a smaller
        // radius gives pages back on that frame and a bigger budget captures
        // more of the queue on it. Nothing is torn down and no card is thrown
        // back on the queue — a budget is not a change to what a card HOLDS.
        // ...AND THE ROW ITSELF. `updateSurfaceCache` builds the atlas on the
        // first frame the row is on and frees it on the first frame it is off,
        // so a toggle needs nothing from the GI configuration push — putting it
        // there would tear down and re-voxelise the whole cascade chain to
        // allocate a texture the next frame allocates anyway.
        mGi.cards                = p.cards;
        mGi.cardBudgetTexels     = p.cardBudgetTexels;
        mGi.cardResidencyRadius  = p.cardResidencyRadius;
        // THE SCREEN-PROBE GATHER'S ROW, for the same reason and with the same
        // nothing-happens-here: `probeGatherWanted()` reads it, the VIEW
        // re-checks its chain shape once a frame (syncReflectListener) and the
        // Component allocates on the first frame it is on and gives everything
        // back on the first frame it is off. Not one voxel is re-injected.
        mGi.gather               = p.gather;
        if (mIfd) pushIfdState(mIfdProbeCounts);
        // THE RAY MARCH IS NOT A CONSTANT — it is read by the light INJECTION, so
        // moving it changes nothing at all until something else happens to
        // re-inject, and this file's own header calls a silently ignored slider
        // the worse outcome. So a change re-injects, on the spot, over the voxels
        // that are already there: no teardown, no re-voxelisation, 0.1-0.3 ms per
        // cascade (S1 §3). The other two ARE constants and are already live in
        // the line above.
        if (marchMoved && mVctLighting) refreshGiLighting(false);
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::refreshGlobalIllumination(GiRefreshReason reason) {
    JAH_TRY {
        // THE HOST ASKED, EXPLICITLY. Recorded before the arm runs so that
        // whatever it does carries `Refresh` as its reason rather than whatever
        // staled the grid last. Read only by the render-loop monitor and by
        // giStatus.
        mLastStaleReason = GiStaleReason::Refresh;
        if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid) {
            // THE PER-CASCADE DIRTY PATH FIRST (G1), then THE REUSE ARM (FIX
            // WAVE B4). Both refuse in exactly the cases the from-scratch rule
            // exists for, and rebuildVct is what happens then — so these lines
            // can only make a refresh cheaper, never wrong.
            //
            // THE DECISION IS THE ENGINE'S, not the host's, and deliberately:
            // only the engine knows whether a chain is live, which cascade can
            // see which edit, and whether anything geometric moved at all. A
            // host that asks for a refresh gets the cheapest correct answer on
            // every arm, so the Player, the previews and a script get it too.
            //
            // AND AN EXPLICIT REFRESH CAN BE DEFERRED TOO — correcting what this
            // comment and BOOTVOX-1's commit message both said (the lead's
            // fix-round item 2). When there is no chain to mark, the answer is
            // `rebuildVct`, which waits for a voxel input that is still
            // streaming exactly as the automatic flush does: the refresh then
            // lands on the frame those pixels arrive (bounded by
            // `kGiVoxelTextureWaitFrames` deferrals), through the `mGiCachesDirty`
            // flag the wait re-arms. That is the right answer and not a
            // concession: a refresh that voxelised without the albedo would be
            // the double build BOOTVOX-1 removed, asked for by hand. A refresh
            // over a LIVE chain (the common case) is never deferred — it marks
            // cascades or re-injects, and touches none of this.
            // AN EXPLICIT REFRESH FINISHES A STAGED BUILD FIRST (OPEN_COVER_SPEC
            // §2 A). The host asking by hand — `world.refreshGi()`, a quality
            // change, and every suite that asserts on the frame after it — is
            // promised the whole answer, and a half-placed probe grid is not
            // one.
            //
            // A SETTLE IS NOT THAT ASK (fix round item 4). The mirror also
            // arrives here when a drag stopped moving, and draining the machine
            // there lands every remaining stage in ONE driver frame — the whole
            // block back, ~700 ms on Grand Showroom 2, for a gesture that ended
            // inside the streaming window. A settle over a staged build marks
            // what it can and lets the stages keep their own pace.
            if (reason == GiRefreshReason::Explicit)
                while (mGiBuildStage != GiBuildStage::Idle) stepStagedGiBuild();
            if (!mVctCascades.empty()) { if (!refreshCascadesFast()) rebuildVct(); }
            else if (!refreshVctFast()) rebuildVct();
        }
    } JAH_CATCH(mError, );
}

// THE REUSE ARM (FIX WAVE B4) — what makes a settling drag affordable.
//
// `rebuildVct` is deliberately from-scratch: it decides the arm's SHAPE and
// re-places the probe grid. A refresh triggered by an object MOVING destroys
// nothing and re-shapes nothing.
//
// So the engine counts destructions instead of assuming them. Every site that
// can invalidate what the GI arms point into already funnels through
// `invalidateGiCaches` BEFORE the pointer dies (destroyNode, detachItem,
// destroyMesh, destroyMaterial, setMaterialShading, destroyTexture, the light
// path); that function now bumps `mGiDestroyGeneration`. When the generation the
// arm was BUILT at is still current, nothing it holds can have died, and:
//
//   * the voxelizer re-runs on itself — `build()` re-derives its mesh buffers,
//     re-buckets the items at their CURRENT world transforms and re-dispatches
//     the voxelization compute jobs, reusing the same voxel textures whenever
//     the resolution is unchanged (OgreVctVoxelizer.cpp createVoxelTextures'
//     early-out) — which also means VctLighting's TextureGpuListener
//     registrations stay pointed at the same textures;
//   * items CREATED since the build are added first. Growth is safe for the same
//     reason: a new Item cannot alias a dead one that never died;
//   * the probes keep their shapes and the grid is marked STALE — re-captured
//     under the budget over the next frames (P6), never all at once. Re-running
//     PccPerPixelGridPlacement would mean `setEnabled(false)`/`setEnabled(true)`
//     — destroying and recreating every probe, its workspace and the cube array
//     — plus six face renders per probe and a GPU readback, which is the bulk of
//     what a refresh costs.
//
// It REFUSES, and takes the full rebuild, when the probe REGION moved: the
// shapes were derived from that region and no longer describe the space. That
// is the honest reading of "probe shapes only re-derive when the geometry
// changed" — small edits inside a room keep the room, so they keep the shapes.
bool OgreScene::refreshVctFast() {
    if (mGi.mode != GiMode::Vct && mGi.mode != GiMode::VctPccHybrid) return false;
    if (!mVctVoxelizer || !mVctLighting) return false;
    // THE PHOTON ARM HAS ITS OWN ARM, AND IT IS NOT THIS ONE. The reuse arm's
    // whole argument is that the VOLUME did not move; under cascades there are N
    // volumes and they move by construction, and the probe-region reasoning
    // below has no cascade meaning. `refreshCascadesFast` is the chain's answer
    // — the per-cascade dirty path (G1) — and every caller reaches for it first,
    // so this is a belt: a chain must never take the single volume's path.
    if (!mVctCascades.empty()) return false;
    if (mGiCachesDirty) return false;                  // a flush is already owed; it rebuilds
    if (mGiBuiltGeneration != mGiDestroyGeneration) return false;   // something may have died
    // NO GRID CAN BE A BUILT STATE, not a failed one (buildPcc's depth rule):
    // when every candidate probe photographed nothing but distance the scene has
    // no grid ON PURPOSE and the sky is its reflection, and forcing a
    // from-scratch rebuild on every refresh because it has none would make the
    // cheapest scene in the editor pay the most. `mProbesDropped` is what
    // separates that from a grid that failed to build at all.
    if (mGi.mode == GiMode::VctPccHybrid && !mPcc && !mProbesDropped) return false;

    Ogre::Vector3 mn, mx;
    if (!computeGiBounds(mn, mx)) return false;
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
    // "Materially the same box", relative to its own size so it is scale-free.
    static const float kReuseEpsilon = 0.02f;
    const auto sameBox = [](const Ogre::Aabb &a, const Ogre::Aabb &b) {
        const Ogre::Vector3 scale = a.getSize() + b.getSize();
        const float tol = std::max(std::max(std::max(scale.x, scale.y), scale.z) * kReuseEpsilon,
                                   1e-4f);
        const Ogre::Vector3 dc = a.mCenter - b.mCenter, dh = a.mHalfSize - b.mHalfSize;
        for (size_t ax = 0; ax < 3u; ++ax)
            if (std::fabs(dc[ax]) > tol || std::fabs(dh[ax]) > tol) return false;
        return true;
    };
    if (mGi.mode == GiMode::VctPccHybrid) {
        // THE PROBE GRID IS A FUNCTION OF THE LIT VOLUME (R5-ROOM): the scout
        // is spread through it, the space it measures is inside it, and the
        // grid is placed in that. So THIS is the box to compare — not the probe
        // region, which is the scout's ANSWER and is smaller by construction
        // (comparing the answer against the question refused every reuse, and
        // an albedo edit then paid for a from-scratch re-placement: measured
        // 12 captures in one frame against a budget of 1, gi.probe_inputs).
        //
        // WHAT IS NOT RE-DECIDED HERE, and deliberately: which probes the rule
        // keeps and where the space is. Both are PHOTOGRAPHS and can only be
        // re-taken by re-capturing, which is the from-scratch rebuild this path
        // exists to avoid. Small edits inside a space keep the space, so they
        // keep its probes; a change big enough to move the lit volume takes the
        // rebuild and is re-photographed there.
        if (!sameBox(aabb, mGiLitVolume)) return false;
    }

    JAH_TRY {
        // Timed for the JAHSHAKA_GI_DEBUG log: the settle frame's cost is the
        // owner's "no hitches" number (CPU side, submission included).
        const auto tStart = std::chrono::steady_clock::now();
        // A MATERIAL the voxeliser read has changed since it was built (P7). NOT a
        // fresh voxeliser any more (A5b fix round): the shared store re-reads its
        // rows in place (mVctMaterialRefreshOwed, set with the generation), so the
        // same voxeliser re-runs below, under the SAME probe grid.
        const bool materialChanged = mGiBuiltMaterialGeneration != mGiMaterialGeneration;
        // ONE ROW FOR THE REFRESH'S VOXEL HALF (ENGINE-5 item 2): re-voxelise +
        // re-inject, with the reason that staled the volume. `units` is the
        // item count the voxeliser walked.
        monitor::CacheScope voxelWork(CacheKind::Gi, monitor::reasonOf(mLastStaleReason), 0,
                                      "vct.refresh", mRoot->getRenderSystem());
        {
            // THE ITEM SET IS NOT HELD (ATOM P4b). The reuse arm used to keep
            // `mVctItemIds` and compare it against the scene every refresh (an
            // O(N) set walk: add what gained kGiGeometryBit, remove what lost
            // it - DRAG-1). The gather reads the flags word of every instance on
            // the device at every build, so a hide or show is answered by the
            // build itself and there is no set to compare.
            // World transforms first: the table reads them, and the whole reason
            // this call exists is that something moved.
            mSceneMgr->updateSceneGraph();
            if (!sameBox(aabb, mGiLitVolume)) {
                mVctVoxelizer->setRegionToVoxelize(aabb);
                mVctVoxelizer->dividideOctants(1u, 1u, 1u);
            }
            if (!mVctFeed) mVctFeed.reset(new detail::VoxelFeed());
            gatherAndBuild(*mVctFeed, mVctVoxelizer, VoxelGatherInputs());
            // The re-voxelised volume's light, through the one writer. A NEW
            // VOXEL STATE: an injection earlier in this frame (the host's tick
            // runs before this flush) lit voxels that no longer exist, so the
            // latch is re-armed for the volume the build just wrote.
            mGiSingleInjectedFrame = ~0ull;
            injectCascade(0);
        }
        const auto tVoxels = std::chrono::steady_clock::now();
        const unsigned giItems = countGiItems();
        voxelWork.setUnits(giItems);
        voxelWork.close();
        mGiLitVolume = aabb;      // materially the box the grid was placed from
        noteGiAutoVolume(aabb, !giBoundsExplicit());
        // NOT re-bound to HlmsPbs, deliberately. `rebuildVct` takes the
        // process-wide binding because a BUILD is a statement about which
        // scene's GI the shader should sample; a refresh is not. If another
        // scene took the binding over in the meantime, a background scene
        // re-solving its own geometry must not snatch it back — and giStatus's
        // vctBound/pccBound go on reporting the truth either way.
        // The probe CONTENTS are stale (the scene moved), the SHAPES are not.
        // STALE the grid — never dirty it (ENGINE_CACHE_POLICY_SPEC P6). Raising
        // mDirty on every probe here is what made a re-solve capture the WHOLE
        // grid inline in one frame (Ogre renders every dirty probe it collects,
        // mNumIterations 1, no cap): measured on the Showroom as 619 / 572 /
        // 623 ms frames of 23 + 32 x 131 draws, fired by the settle after a
        // drag, by a drag pausing for 250 ms, and by world.refreshGi(). Stale,
        // the budget spreads the same captures over ceil(probes / catch-up)
        // frames, and no frame costs more than an ordinary one.
        //
        // EXCEPT WHEN PAUSED (updateBudget 0). Then there is no budget to
        // spread over — nothing would ever capture a stale probe — and a
        // re-solve can only have come from an explicit demand (the mirror arms
        // nothing while paused; world.refreshGi() and a direct call do), whose
        // documented contract is "the picture is what was last built until
        // world.refreshGi() asks for more". So a paused refresh captures the
        // whole grid at once, as it always did, and leaves nothing stale.
        if (mGi.updateBudget > 0) {
            staleProbeGrid(GiStaleReason::Refresh);
        } else if (mPcc) {
            for (Ogre::CubemapProbe *p : mPcc->getProbes()) p->mDirty = true;
            for (ProbeSlot &sl : mProbeSlots) sl.sweepPending = false;
            mLastStaleReason = GiStaleReason::Refresh;
            ++mStaleSerial;
        }
        // The DDGI field is re-INITIALIZED, not reset, on this path. The reuse
        // arm keeps the VctLighting OBJECT but re-voxelizes underneath it and
        // may have moved the volume (the region above), and the field's
        // grid is a function of that volume's origin and size — a reset would
        // leave the probes describing a box that no longer exists. Upstream's
        // own rule, in its own words: minor changes to VctLighting -> reset(),
        // major -> initialize() again.
        {
            monitor::CacheScope fieldWork(CacheKind::Gi, monitor::reasonOf(mLastStaleReason), 0,
                                          "ifd.build", mRoot->getRenderSystem());
            buildIrradianceField();
            fieldWork.setUnits(mIfdTotalProbes);
        }
        // A fresh voxel arm is not a reuse of the voxels (the probes were
        // kept either way, and giStatus.rebuilds does not move).
        mGiReusedLastRefresh = true;
        mGiBuiltMaterialGeneration = mGiMaterialGeneration;
        if (giDebug()) {
            const auto tEnd = std::chrono::steady_clock::now();
            const auto ms = [](std::chrono::steady_clock::time_point a,
                               std::chrono::steady_clock::time_point b) {
                return std::to_string(std::chrono::duration<double, std::milli>(b - a).count());
            };
            Ogre::LogManager::getSingleton().logMessage(
                std::string("Jahshaka GI: refresh ") +
                (materialChanged ? "REUSED the voxel arm, materials re-read in place"
                                 : "REUSED the voxel arm") +
                " (" + std::to_string(giItems) + " items, probe grid staled) in " +
                ms(tStart, tEnd) + " ms: voxels + injection " + ms(tStart, tVoxels) +
                " ms, irradiance field " + ms(tVoxels, tEnd) + " ms");
        }
        return true;
    } JAH_CATCH(mError, false);
}

// The LIGHT-ONLY refresh (REFLECTIONS_ADOPTION_SPEC.md P2). VERIFIED AGAINST
// THE PIN by this lane, because the spec listed it as unproven: VctLighting::
// update() re-collects the scene's lights, re-maps its const buffer and
// re-dispatches the injection compute job against the voxelizer's EXISTING
// albedo/normal/emissive textures (OgreVctLighting.cpp). It touches neither the
// VctVoxelizer's raw Item* cache nor VctMaterial's datablock cache, which is
// what the "always from scratch" rule in rebuildVct exists to protect — and
// upstream's own Voxelizer sample re-calls it on a keypress without rebuilding
// anything (Samples/2.0/Tests/Voxelizer, the F4/F5 handlers). So it is safe to
// re-run, and the from-scratch rule stays exactly as strict for the voxelizer.
//
// updateSceneGraph() first: light injection reads each light's DERIVED position
// (VctLighting::addLight -> getParentNode()->_getDerivedPosition()), and the
// whole point of this call is that a light just moved.
// THE CHAIN IS SETTLED FOR THESE INPUTS (LAMPREST-3 fix round item 3). Recorded
// by WHOEVER pays a full at-rest chain injection — the light tick, or the last
// step of an incremental settle — and consulted by the mirror's stability-window
// tick, so one gesture costs one settle instead of two or three.
void OgreScene::noteChainSettled() {
    mGiSettleStepsOwed = 0;
    noteSettleInputs();
}

// WHAT AN INJECTION READS, remembered so an unfinished settle can tell that it
// changed underneath it (and so `chainCleanNow` can tell the same thing about a
// finished one).
void OgreScene::noteSettleInputs() {
    mGiSettleSerial = mGiLightWriteSerial;
}

// THE DEBT, raised in one place: a rebuild, an environment change and a tick
// the one-writer latch refused all owe the same thing — the chain's at-rest
// answer over its current inputs.
void OgreScene::oweChainSettle() {
    const size_t n = mVctCascades.size();
    if (n < 2u) return;
    mGiSettleCascades  = n;
    mGiSettleStepsOwed = kAtRestSweeps * int(n);
    noteSettleInputs();
}


// ONE INJECTION OF AN OWED SETTLE (LAMPREST-3 fix round item 2), out of the
// scheduler's own one-slot-per-frame budget and in the EXACT order the whole
// tick uses: cascades OUTERMOST-FIRST. That order
// is why the bytes agree — the same injections, over the same voxels, in the
// same sequence — and it is verified as such (sha256 of every cascade's light
// voxel texture after an incremental settle against the same scene after one
// whole `refreshGiLighting(false)`).
void OgreScene::payChainSettleStep() {
    const size_t n = mVctCascades.size();
    if (mGiSettleStepsOwed <= 0 || n < 2u || n != mGiSettleCascades) {
        mGiSettleStepsOwed = 0;                 // the chain changed shape under it
        return;
    }
    JAH_TRY {
        monitor::CacheScope work(CacheKind::Gi, WorkReason::Sweep, 0, "vct.light.settle",
                                 mRoot->getRenderSystem());
        mSceneMgr->updateSceneGraph();
        // A LIGHT THAT MOVED WHILE THE SETTLE WAS RUNNING RESTARTS IT, and this
        // is not caution: an injection reads the lights' DERIVED poses at the
        // moment it runs, so a settle whose first steps saw one lamp pose and
        // its last steps another leaves the chain a MIXTURE of the two and is
        // not the fixed point of either — and `noteChainSettled` would then
        // record that mixture as clean and let the next refresh skip the
        // injection that would have fixed it. Measured:
        // scripting.e2e.movable_lamp_rest read 3/255, 10/10, the moment the
        // settle was allowed to absorb a light write (the lamp's push landed
        // between two steps, the settle finished over it, and the suite's
        // reference refresh was then skipped as clean).
        //
        // A lamp that keeps moving therefore keeps restarting this, which is
        // right: nothing finishes while the scene is still changing, and the
        // mirror's own at-rest tick — which fires one frame after the motion
        // ends and clears the debt outright — is what finishes a light gesture.
        if (mGiSettleSerial != mGiLightWriteSerial) oweChainSettle();
        const int total = kAtRestSweeps * int(n);
        unsigned injections = 0;
        // A cascade with no lighting is skipped exactly as the tick skips it,
        // and without spending the frame: the loop walks on to the next step.
        // A cascade ALREADY INJECTED THIS FRAME (the mirror's tick ran before
        // the scheduler) is not injected twice: the step waits for the next
        // frame, unspent, so the sequence stays the tick's own.
        while (mGiSettleStepsOwed > 0 && injections == 0u) {
            const int step = total - mGiSettleStepsOwed;
            const size_t i = n - 1u - size_t(step % int(n));
            if (mVctCascades[i].lighting && injectedThisFrame(i)) break;
            --mGiSettleStepsOwed;
            if (!mVctCascades[i].lighting) continue;
            if (injectCascade(i)) ++injections;
        }
        work.setUnits(injections);
        if (mGiSettleStepsOwed == 0) {
            // The tick's own closing bookkeeping, once, after the LAST
            // injection: the per-tick skip is spent and the field integrates a
            // chain that has finished moving.
            for (VctCascade &c : mVctCascades) c.injectedSinceTick = false;
            reintegrateFieldAfterInjection();
            ++mGiChainSettles;
            noteChainSettled();
        }
    } JAH_CATCH(mError, );
}

// THE ONE WRITER (PHOTON-WRITER-1; render audit F3/F13). Every write to a voxel
// volume's light in this engine is this function: the from-scratch arm's
// closing injection, a cascade rebuild's, the single volume's re-voxelisation,
// the light tick (moving and at rest) and the incremental settle. What an
// injection IS therefore has one definition — the environment the bounce job
// reads where its cones escape, then `VctLighting::update` at the document's
// own bounce count with the scene's own ray march — and no path can hand a
// volume a different answer from another (DRAG-1: the flicker was two writers
// with two answers alternating on cascade 0).
//
// THE LATCH. A volume is injected at most once per writer frame. The second
// injection of one cascade in one frame was always one of two things: WASTED
// (a tick over voxels a rebuild is about to replace in the same frame) or a
// symptom of two paths not knowing about each other (F3's residue). So it is
// refused here, counted, and every caller that can meet a refusal keeps its
// work OWED rather than dropped: the scheduler leaves a latched cascade's
// rebuild pending, the settle leaves its step unspent, and a refused tick owes
// the chain a settle. A refusal is not asserted in Debug on purpose: Debug is
// the daily driver, and an owed injection one frame late is a correct picture
// one frame late, whereas an abort is not a picture at all; `gi.one_writer`
// asserts the counter stays 0 on a drag + settle.
unsigned long long OgreScene::giWriterFrame() const {
    return (unsigned long long)mRoot->getNextFrameNumber();
}

bool OgreScene::injectedThisFrame(size_t i) const {
    const unsigned long long frame = giWriterFrame();
    if (!mVctCascades.empty())
        return i < mVctCascades.size() && mVctCascades[i].injectedFrame == frame;
    return i == 0u && mGiSingleInjectedFrame == frame;
}

bool OgreScene::injectCascade(size_t i) {
    const bool chain = !mVctCascades.empty();
    Ogre::VctLighting *lighting =
        chain ? (i < mVctCascades.size() ? mVctCascades[i].lighting : nullptr)
              : (i == 0u ? mVctLighting : nullptr);
    if (!lighting) return false;
    unsigned long long &stamp = chain ? mVctCascades[i].injectedFrame : mGiSingleInjectedFrame;
    const unsigned long long frame = giWriterFrame();
    if (stamp == frame) {
        ++mGiInjectionRefusals;
        if (mGiInjectionRefusals == 1u)
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: the one-writer latch refused a second injection of cascade " +
                std::to_string(i) + " in one frame (left owed; GiStatus::chainInjectionRefusals)");
        return false;
    }
    applyCascadeEnvironment(lighting);
    lighting->update(mSceneMgr, cascadeBounces(i), 1.0f /*thinWallCounter*/, true /*autoMultiplier*/,
                     giRayMarchStepScale());
    stamp = frame;
    // The single volume's LANDED injections — the surface cache's indirect
    // signature (PHOTON-CARDS-1; a chain folds its cascades' rebuild counts and
    // settles instead). Counted here, inside the one writer, never at a caller.
    if (!chain) ++mGiMonoInjections;
    if (mGiInjectionCountFrame != frame) {
        mGiInjectionCountFrame = frame;
        mGiInjectionsThisFrame = 0;
    }
    mGiInjectionsPeak = std::max(mGiInjectionsPeak, ++mGiInjectionsThisFrame);
    return true;
}

// THE FIELD IS AN INTEGRAL OF THE CHAIN, so it re-integrates after the chain's
// radiance has finished moving — once, after the LAST injection of a tick or of
// an incremental settle, never per injection (LAMPREST-3 fix round).
void OgreScene::reintegrateFieldAfterInjection() {
    // THE ONE PLACE `reset()` IS CORRECT (spike §8): the same VctLighting
    // object, same voxel textures, same field geometry — only the radiance
    // in the volume changed. reset() re-arms the integration counter and
    // does NOT clear the atlases, so the probes re-converge progressively
    // over the previous converged data and the room never flashes black.
    //
    // At a PAUSED budget nothing would ever spend that counter down, so a
    // reset there would freeze the field half-updated for ever; the field
    // is converged inline instead. (The mirror only runs this path while
    // the budget is above 0, so this is the belt to that braces.)
    if (mIfd) {
        // THE FIELD'S BINDING FIRST, AND UNCONDITIONALLY (E1 reader F2,
        // PHOTON_SPEC §7 E2 (9)). `VctLighting::update` with extra bounces
        // PING-PONGS its light voxel textures (`runBounce`), and the field
        // bound whatever was current ONCE, by pointer, at `initialize()`
        // (ogre-patch 0044). So after an odd number of bounce passes — which
        // a chain reaches whenever the document asks for an even total (two
        // total bounces = one pass per cascade) — a light move left the field
        // integrating from the texture the injection had just stopped
        // writing. Re-binding is five descriptor writes on a path that has
        // just run a compute dispatch per bounce; deciding whether it is
        // needed would mean comparing raw pointers that may have been
        // recycled (the defect class patch 0041 exists for).
        //
        // The head, because the field rides cascade 0 and `mVctLighting`
        // IS cascade 0's lighting under a chain.
        if (mVctLighting) mIfd->setVctLighting(mVctLighting);
        mIfd->reset();
        mIfdProbesDone = 0u;
        // Paused budget: the field converges inline (5 ms of GPU).
        if (!mIfdProbesPerFrame) {
            {
                monitor::CacheScope work(CacheKind::Gi, WorkReason::Light, 0, "ifd.converge.inline",
                                         mRoot->getRenderSystem());
                mIfd->update(mIfdTotalProbes);
                mIfdProbesDone = mIfdTotalProbes;
                work.setUnits(mIfdTotalProbes);
            }
        }
    }}

// THE LIGHT TICK, ASKED FOR. Under a CHAIN the tick is not run here: it is OWED
// and run by the cascade scheduler at the frame's one writer point
// (updateCascades), AFTER the frame's rebuild. That order is the one-writer rule
// made structural (PHOTON-WRITER-1): the host's tick arrives before the render
// (the mirror's push), the scheduler's rebuild inside it, and a tick that ran
// first injected a cascade over voxels the rebuild then replaced — a wasted
// injection at best, and the latch would have had to refuse the rebuild's own
// (whose volume MUST be lit). Run after the rebuild, the tick skips the cascade
// the rebuild has just injected (the latch; while moving that IS the answer, at
// rest it owes the chain a settle) and injects the others once each: a frame
// never injects a cascade twice and never more than the chain's size.
// A moving and an at-rest request in one frame run as the at-rest tick (it is
// the superset). The single volume has no scheduler and nothing to order: its
// tick runs here, now.
bool OgreScene::refreshGiLighting(bool inMotion) {
    JAH_TRY {
        if (!mVctLighting || !mVctVoxelizer) return false;
        if (!mVctCascades.empty()) {
            const GiTickOwed ask = inMotion ? GiTickOwed::Moving : GiTickOwed::Rest;
            if (int(ask) > int(mGiTickOwed)) mGiTickOwed = ask;
            return true;
        }
        mSceneMgr->updateSceneGraph();
        {
            monitor::CacheScope work(CacheKind::Gi, WorkReason::Light, 0,
                                     inMotion ? "vct.light.moving" : "vct.light",
                                     mRoot->getRenderSystem());
            // The single volume is not an iteration: one injection, the one
            // writer's, at the document's bounce count.
            injectCascade(0);
            work.setUnits(cascadeBounces(0) + 1u);
        }
        reintegrateFieldAfterInjection();
        return true;
    } JAH_CATCH(mError, false);
}

// THE CHAIN'S LIGHT TICK, run at the writer point (updateCascades) or by the
// drag's own settle there.
void OgreScene::runChainTick(bool inMotion) {
    JAH_TRY {
        if (mVctCascades.empty()) return;
        mSceneMgr->updateSceneGraph();
        bool refused = false;
        {
            // THE LIGHT-ONLY TICK (ENGINE-5 item 2). The cheap path a drag runs
            // every few frames: one injection dispatch per bounce over the
            // voxels that are already there. `units` = injections actually run.
            monitor::CacheScope work(CacheKind::Gi, WorkReason::Light, 0,
                                     inMotion ? "vct.light.moving" : "vct.light",
                                     mRoot->getRenderSystem());
            // EVERY CASCADE, OUTERMOST FIRST — a light move changes the radiance
            // in all of them, and a chain where only the head was re-injected
            // would bounce yesterday's light in from outside. Injection is the
            // cheap half (0.1-0.3 ms per cascade, S1 §3); it is the
            // VOXELISATION that is expensive, and nothing here re-voxelises.
            //
            // ONE ANSWER PER VOLUME, WHOEVER COMPUTES IT (DRAG-1, PHOTON-M2 F-D):
            // every injection is `injectCascade`, the call a rebuild ends with,
            // so the moving tick, the at-rest tick and the rebuild hand a volume
            // one answer. (The moving tick's zero-bounce coarse march and its
            // measurement switch went with the one writer: nothing measured with
            // them any more.)
            //
            // ONE SWEEP, at rest as while moving: it IS the chain's fixed point
            // (derived beside kAtRestSweeps, EnginePrivate.h; proved byte for byte
            // by gi.chain_converge).
            //
            // HOW MANY CASCADE INJECTIONS THIS TICK ACTUALLY RAN, reported as the
            // work row's `units` (DRAG-1): a chain of four with cascade 0 rebuilt
            // since the last moving tick reads 3.
            unsigned injections = 0;
            for (size_t i = mVctCascades.size(); i--; ) {
                if (!mVctCascades[i].lighting) continue;
                // WHILE MOVING, a cascade a REBUILD has injected since the last
                // tick already holds this tick's answer — the same injection,
                // over current voxels — as long as the lights it was injected
                // with are still the scene's (F5). This skip spans frames (ticks
                // run every few), so it is the rebuild's flag and not the
                // per-frame latch.
                if (inMotion && mVctCascades[i].injectedSinceTick &&
                    mVctCascades[i].injectedAtLightSerial == mGiLightWriteSerial)
                    continue;
                // ...and a cascade the rebuild injected THIS frame is the latch's,
                // not a refusal: that is the order working.
                if (injectedThisFrame(i)) {
                    if (!inMotion) refused = true;   // it read the outer light before this tick
                    continue;
                }
                if (injectCascade(i)) ++injections;
                else refused = true;
            }
            // The skip is per TICK, not for ever: a cascade that was rebuilt
            // before this tick has paid for this tick, and owes the next one
            // unless it is rebuilt again.
            for (VctCascade &c : mVctCascades) c.injectedSinceTick = false;
            work.setUnits(injections);
        }
        reintegrateFieldAfterInjection();
        // WHOEVER PAYS THE INJECTION PAYS THE DEBT (LAMPREST-3 fix round item 3).
        // An at-rest tick over the chain IS the settle the scheduler owes, so an
        // unfinished incremental one is abandoned rather than run on top of the
        // answer this tick has just computed — UNLESS a cascade was not injected
        // by it (the frame's rebuild injected it first, before this tick's outer
        // cascades), in which case the chain is owed its settle.
        if (!inMotion && mVctCascades.size() > 1u) {
            if (refused) oweChainSettle();
            else         noteChainSettled();
        }
    } JAH_CATCH(mError, );
}

GiStatus OgreScene::giStatus() const {
    GiStatus st;
    st.mode = mGi.mode;
    // THE SURFACE CACHE (SURFACE-CACHE phase 2) — outside the try, because it
    // touches no Ogre object of its own: it copies counters the Component holds
    // and leaves the struct's zeroed defaults when there is no cache, which is
    // every scene with the card row off.
    if (mSurfaceCache) mSurfaceCache->fillStatus(st.cards);
    JAH_TRY {
        if (mPcc) st.probeCount = int(mPcc->getProbes().size());
        // "Bound" means the process-wide HlmsPbs is sampling THIS scene's arm.
        // Both halves are checked against our own pointers rather than against
        // sVctBindingOwner alone: the owner flag says who bound last, these say
        // what the shader will actually read this frame.
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        st.pccBound = mPcc && pbs->getParallaxCorrectedCubemap() == mPcc;
        st.vctBound = mVctLighting && pbs->getVctLighting() == mVctLighting;
        const auto toV = [](const Ogre::Vector3 &v) { return Vec3(v.x, v.y, v.z); };
        st.boundsMin      = toV(mGiLitVolume.getMinimum());
        st.boundsMax      = toV(mGiLitVolume.getMaximum());
        // METRES PER VOXEL — of the volume that was actually built (audit B9).
        // Under a cascade chain the lit volume is the OUTERMOST cascade's box
        // and its resolution is that cascade's, which is not the tier's: at
        // High the outer cascade is 64^3 over 120 m, so dividing by the tier's
        // 128 reported 0.94 m for a 1.875 m voxel.
        const Ogre::Vector3 litSize = mGiLitVolume.getSize();
        st.voxelMetres    = !mVctCascades.empty()
                                ? mVctCascades.back().cell()
                                : std::max(std::max(litSize.x, litSize.y), litSize.z) /
                                      float(std::max(giVoxelResolution(), 1u));
        st.probeRegionMin = toV(mGiProbeRegion.getMinimum());
        st.probeRegionMax = toV(mGiProbeRegion.getMaximum());
        // RESOLVED, not requested: both default to GiToggle::Auto, and the
        // shadow half additionally falls back when there is no shadow node.
        st.probeCaptureSize   = mPcc ? mPccCaptureSize : 0;
        // The depth rule's verdict (buildPcc). Reported in EVERY mode so a
        // caller can tell "no grid because every candidate probe saw nothing"
        // from "no grid because the mode does not build one".
        st.probesDropped      = mProbesDropped;
        st.probeHdr     = mPcc && mPccHdr;
        st.probeShadows = mPcc && mPccShadowed;
        // RESOLVED, like the two above: the request is clamped to the probes
        // that exist, and it is only ACTED ON once a view has pushed a tracked
        // camera position (updateGiTracking), so this reads 0 for the frame
        // between the rebuild and the first tracking update.
        st.probeUpdatesPerFrame = mPcc ? mProbeUpdatesPerFrame : 0;
        st.cubemapProbeSlotsPerCell = int(mCubemapProbeSlots);
        if (mPcc) {
            const Ogre::CubemapProbeVec &probes = mPcc->getProbes();
            if (!probes.empty()) {
                Ogre::Vector3 smn(1e30f), smx(-1e30f);
                for (const Ogre::CubemapProbe *p : probes) {
                    smn.makeFloor(p->getProbeShape().getMinimum());
                    smx.makeCeil(p->getProbeShape().getMaximum());
                }
                st.probeShapeMin = toV(smn);
                st.probeShapeMax = toV(smx);
                // THE PER-PROBE LOCALITY CHECK (Types.h says why the union above
                // cannot be one). Measured against the probe's own AREA, which
                // is its share of the region — the same box updateProbeBudget
                // calls "the space this probe is responsible for".
                for (const Ogre::CubemapProbe *p : probes) {
                    const float r = probeShapeCellRatio(p);
                    if (r > st.worstProbeShapeCellRatio) st.worstProbeShapeCellRatio = r;
                    if (r > kProbeShapeCellAllowance) ++st.probesExceedingCell;
                }
                st.probesClampedToRegion = mProbesClampedToRegion;
            }
        }
        st.reusedLastRefresh = mGiReusedLastRefresh;
        // Outside the probe block on purpose: a material can cross the gate in
        // a scene that has no probe grid at all (the shader is rebuilt either
        // way), and the count is the honest answer there too.
        st.probeGateCrossings = mProbeGateCrossings;
        // DDGI, reported the same way pccBound/vctBound are: against the live
        // HlmsPbs pointer, not against what was requested or who bound last.
        st.ifdBound          = mIfd && pbs->getIrradianceField() == mIfd;
        st.ifdProbes         = int(mIfdTotalProbes);
        st.ifdConverged      = mIfd && mIfdProbesDone >= mIfdTotalProbes;
        st.ifdProbesPerFrame = mIfd ? int(mIfdProbesPerFrame) : 0;
        if (mIfd) {
            st.ifdMin = toV(mIfdVolumeOrigin);
            st.ifdMax = toV(mIfdVolumeOrigin + mIfdVolumeSize);
        }
        st.ifdFollows = mIfdFollows;
        st.ifdScrollProbes = mIfdScrolledProbes;
        st.ifdScrolls = mIfdScrolls;
        st.ifdReplacements = mIfdReplacements;
        st.ifdTargetSamples = mIfd ? unsigned(mIfd->getTargetSamples()) : 0u;
        st.ifdRefinesOwed   = mIfd ? unsigned(mIfd->getRefinesOwed() +
                                              (mIfd->isWorkDone() ? 0u : 1u)) : 0u;
        // THE PROBE CACHE (ENGINE_CACHE_POLICY_SPEC P1/P6/P7).
        st.probeCapturesLastFrame = mPcc ? mProbeCapturesLastFrame : 0;
        st.probeCapturesDeferred  = mProbeCapturesDeferred;
        int stale = 0;
        if (mPcc) for (const ProbeSlot &sl : mProbeSlots) if (sl.sweepPending) ++stale;
        st.staleProbes     = stale;
        st.lastStaleReason = mLastStaleReason;
        st.staleSerial     = mStaleSerial;
        st.rebuilds        = mGiRebuilds;
        st.giScans         = mGiScans;
        st.giScanMicros    = mGiScanMicros;
        st.giAabbReads     = mGiAabbReads;
        // THE PHOTON CHAIN, as BUILT (empty in the single-volume arm).
        st.cascades.clear();
        st.cascades.reserve(mVctCascades.size());
        for (const VctCascade &c : mVctCascades) {
            GiStatus::CascadeStatus cs;
            cs.halfSize   = c.halfSize;
            cs.resolution = int(c.resolution);
            cs.cell       = c.cell();
            cs.step       = c.step();
            // THE NEAR-FIELD GUARANTEE THIS CASCADE IS ACHIEVING, in metres —
            // THROUGH THE HEADER'S OWN FUNCTION, never a second copy of the
            // formula here: the radius around the head inside which the near
            // field is voxelised by THIS cascade (Types.h, where the derivation
            // and what it does and does not include are written out).
            GiParams::GiCascadeDesc shape;
            shape.halfSize   = c.halfSize;
            shape.resolution = int(c.resolution);
            shape.stepCells  = c.stepCells;
            cs.guaranteedRadius = giCascadeGuaranteedRadius(shape);
            cs.centre     = toV(c.centre);
            cs.rebuilds   = c.rebuilds;
            cs.pending    = c.pending;
            // WHAT THIS CASCADE VOXELISED - A READING of the device's own counts,
            // written as the gather wrote the records (ATOM P4b, A5b §3). They
            // used to be CPU walks: `items` an AABB test per GI item per rebuild,
            // `attached` the size-filtered set, `lodLevels` the CPU's REQUEST
            // histogram (DELETED - nothing on the CPU decides a level any more)
            // and `voxelTriangles` the voxeliser's CPU queue.
            const detail::VoxelReading &rd = currentReading(c.feed.get());
            cs.items      = int(rd.instances);      // enclosed: at least one record written
            cs.attached   = int(rd.candidates);     // passed every predicate
            cs.lastCpuMs  = c.lastCpuMs;
            cs.voxelLevels = rd.histogram;          // partitions per level, attach set
            cs.voxelTriangles = (long long)(rd.indexTotal / 3u);
            cs.voxelRecords   = (long long)rd.records;
            cs.voxelOverflow  = (long long)rd.overflow;
            // WHAT THE REBUILD COST IN DISPATCHES (ogre-patch 0065): counted by the
            // voxeliser's last build() - one per store bucket per octant, 0 when it
            // only cleared. A dispatch is sized by the whole octant, so this is the
            // material-count half of a cascade's bill.
            cs.voxelDispatches = c.voxelizer ? (long long)c.voxelizer->getLastDispatchCount() : 0;
            st.cascades.push_back(cs);
        }
        st.cascadesAwaitingCamera = mGiCascadeAwaitingCamera;
        // Reported as a STATE and not as a counter: what a reader wants to know
        // is "is the arm waiting for pixels right now", and the frames it has
        // waited are the wait's own business (see giVoxelTexturesPending).
        st.awaitingVoxelTextures = mGiVoxelTextureWaitFrames > 0u;
        st.cascadeVoxelLod = mCascadeVoxelLod;
        // WHICH COLUMN OF THE TIER TABLE THE CHAIN CAME FROM (V1-RIG item 4) —
        // the chain's own record, so the MONOLITHIC arm (no chain at all) reads
        // false whoever is driving.
        st.cascadeProfileVr = !mVctCascades.empty() && mGiChainProfileVr;
        st.cascadeFullRebuilds = mCascadeFullRebuilds;
        st.cascadeDeferrals    = mCascadeDeferrals;
        st.cascadeDirtyMajority = mCascadeDirtyMajority;
        st.chainInjectionRefusals   = mGiInjectionRefusals;
        st.chainInjectionsPeakFrame = mGiInjectionsPeak;
        st.chainSettles         = mGiChainSettles;
        st.dragMovers           = int(mDragMovers.size());     // MOVER-1
        st.dragMoverGestures    = mDragMoverGestures;
        // THE SCREEN-PROBE GATHER (GATHER-1a): the row resolved against the
        // machine, and — where a view has drawn one — the grid it placed, the
        // rays it traced and the GPU milliseconds the three jobs cost. Filled
        // in OgreRayQuery.cpp, which is where the tier is; `on` false with
        // every other field zero is the shipped state.
        gatherStatusInto(st.gather);
    } JAH_CATCH(mError, st);
    return st;
}

// WHAT THE VOXEL LIGHTING VOLUME HOLDS (PHOTON-M3) — the test-and-tool
// readback behind `Scene::giVoxelStats` and `world.giVoxelStats`.
//
// A TOTAL volume is a physical quantity (the surface radiance of the voxel,
// times the store's normalisation k) in a FIXED-RANGE format, and PHOTON-M2's
// F1 is the proof that whether it FITS cannot be read from the picture: a
// clipped gather draws a picture that is simply dimmer, exactly like a scene
// with less bounce in it. So the question is answered where it lives — in the
// bytes.
//
// It blocks: `flushCommands()` (the sky/IBL rule — an AsyncTextureTicket reads
// VRAM, and the injection's dispatches are only RECORDED until something
// submits them) and then a synchronous download of the whole volume. Never on
// a frame path; `traceRays` above is the same contract.
bool OgreScene::giFieldAtlas(GiFieldAtlas &out) {
    out = GiFieldAtlas();
    if (!mIfd || mIfdTotalProbes == 0u) return false;
    JAH_TRY {
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        rs->flushCommands();
        Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
        const auto grab = [&](Ogre::TextureGpu *tex, std::vector<unsigned char> &bytes,
                              unsigned &w, unsigned &h, unsigned &bpp) -> bool {
            if (!tex || tex->getResidencyStatus() != Ogre::GpuResidency::Resident) return false;
            Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
                tex->getWidth(), tex->getHeight(), 1u, Ogre::TextureTypes::Type2D,
                tex->getPixelFormat());
            bool ok = false;
            try {
                ticket->download(tex, 0u, true);
                const Ogre::TextureBox box = ticket->map(0);
                w = tex->getWidth(); h = tex->getHeight(); bpp = unsigned(box.bytesPerPixel);
                bytes.resize(size_t(w) * h * bpp);
                for (unsigned y = 0; y < h; ++y)
                    std::memcpy(&bytes[size_t(y) * w * bpp], box.at(0, y, 0), size_t(w) * bpp);
                ticket->unmap();
                ok = true;
            } catch (Ogre::Exception &e) { mError = e.getFullDescription(); }
            tm->destroyAsyncTextureTicket(ticket);
            return ok;
        };
        if (!grab(mIfd->getIrradianceTex(), out.irradiance, out.irradWidth, out.irradHeight,
                  out.irradBytesPerTexel) ||
            !grab(mIfd->getDepthVarianceTex(), out.depth, out.depthWidth, out.depthHeight,
                  out.depthBytesPerTexel))
            return false;
        for (int a = 0; a < 3; ++a) {
            out.probes[a] = mIfdProbeCounts[a];
            out.windowOffset[a] = mIfd->getWindowOffset()[a];
        }
        out.irradBordered = unsigned(kIfdIrradRes) + 2u;
        out.depthBordered = unsigned(kIfdDepthRes) + 2u;
        out.available = true;
        return true;
    } JAH_CATCH(mError, false);
}

GiVoxelStats OgreScene::giVoxelStats(int cascadeIdx) {
    GiVoxelStats st;
    st.cascade = cascadeIdx;
    JAH_TRY {
        Ogre::VctLighting *lighting = nullptr;
        Ogre::VctVoxelizer *voxelizer = nullptr;
        if (!mVctCascades.empty()) {
            if (cascadeIdx < 0 || size_t(cascadeIdx) >= mVctCascades.size()) return st;
            lighting = mVctCascades[size_t(cascadeIdx)].lighting;
            voxelizer = mVctCascades[size_t(cascadeIdx)].voxelizer;
        } else if (cascadeIdx == 0) {
            lighting = mVctLighting;
            voxelizer = mVctVoxelizer;
        }
        if (!lighting) return st;
        Ogre::TextureGpu *total = lighting->getLightVoxelTextures()[0];
        if (!total || total->getResidencyStatus() != Ogre::GpuResidency::Resident) return st;

        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        rs->flushCommands();
        Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();

        // ONE pass over a volume, for the total and (when the cascade bounces)
        // for the DIRECT term beside it: the same walk answers "is the fixed
        // point clipped" and "is the normalisation itself wrong", which are
        // different defects with the same symptom.
        struct Walk {
            float     peak = 0.0f;
            double    sum = 0.0;
            long long lit = 0, atMax = 0, aboveOne = 0, count = 0;
        };
        const auto walk = [&](Ogre::TextureGpu *tex, Walk &w) -> bool {
            const Ogre::PixelFormatGpu fmt = tex->getPixelFormat();
            const bool isHalf = (fmt == Ogre::PFG_RGBA16_FLOAT);
            const bool isByte = (fmt == Ogre::PFG_RGBA8_UNORM_SRGB || fmt == Ogre::PFG_RGBA8_UNORM);
            if (!isHalf && !isByte) return false;
            const bool decodeSrgb = (fmt == Ogre::PFG_RGBA8_UNORM_SRGB);
            Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
                tex->getWidth(), tex->getHeight(), tex->getDepth(),
                Ogre::TextureTypes::Type3D, fmt);
            bool ok = false;
            // A plain try here, not JAH_CATCH: the macro RETURNS, and the
            // ticket below it must be destroyed on every path.
            try {
                ticket->download(tex, 0u, true);
                const Ogre::TextureBox box = ticket->map(0);
                for (Ogre::uint32 z = 0; z < tex->getDepth(); ++z) {
                    for (Ogre::uint32 y = 0; y < tex->getHeight(); ++y) {
                        const void *row = box.at(0, y, z);
                        for (Ogre::uint32 x = 0; x < tex->getWidth(); ++x) {
                            float c[3];
                            bool top = false;
                            if (isHalf) {
                                const Ogre::uint16 *p =
                                    reinterpret_cast<const Ogre::uint16 *>(row) + size_t(x) * 4u;
                                for (int i = 0; i < 3; ++i) c[i] = Ogre::Bitwise::halfToFloat(p[i]);
                            } else {
                                const Ogre::uint8 *p =
                                    reinterpret_cast<const Ogre::uint8 *>(row) + size_t(x) * 4u;
                                for (int i = 0; i < 3; ++i) {
                                    if (p[i] == 255u) top = true;
                                    const float v = float(p[i]) / 255.0f;
                                    // THE STORE IS sRGB-ENCODED (the injection's
                                    // toSRGB, an 8-bit volume's dark-end
                                    // precision): the value the cone gather
                                    // reads is the DECODE, so that is the value
                                    // reported.
                                    c[i] = decodeSrgb
                                               ? (v <= 0.04045f ? v / 12.92f
                                                                : std::pow((v + 0.055f) / 1.055f, 2.4f))
                                               : v;
                                }
                            }
                            const float m = std::max(std::max(c[0], c[1]), c[2]);
                            ++w.count;
                            if (m > 0.0f) { ++w.lit; w.sum += double(m); }
                            if (m > w.peak) w.peak = m;
                            if (top) ++w.atMax;
                            if (m > 1.0f) ++w.aboveOne;
                        }
                    }
                }
                ticket->unmap();
                ok = true;
            } catch (Ogre::Exception &e) { mError = e.getFullDescription(); }
              catch (std::exception &e)  { mError = std::string("engine: ") + e.what(); }
            tm->destroyAsyncTextureTicket(ticket);
            return ok;
        };

        Walk wt;
        if (!walk(total, wt)) return st;
        st.available = true;
        st.width  = int(total->getWidth());
        st.height = int(total->getHeight());
        st.depth  = int(total->getDepth());
        st.format = Ogre::PixelFormatGpuUtils::toString(total->getPixelFormat());
        st.formatMax = (total->getPixelFormat() == Ogre::PFG_RGBA16_FLOAT) ? 0.0f : 1.0f;
        st.multiplier = lighting->getCurrentBakingMultiplier();
        st.peak = wt.peak;
        st.meanLit = wt.lit ? wt.sum / double(wt.lit) : 0.0;
        st.voxelsLit = wt.lit;
        st.voxelsAtMax = wt.atMax;
        st.voxels = wt.count;
        st.voxelsAboveOne = wt.aboveOne;

        // THE BYTES of every light volume a reader samples (the total, then the
        // anisotropic axes), hashed raw — the one-sweep proof's instrument.
        {
            Ogre::uint64 h = 1469598103934665603ull;
            bool all = true;
            const size_t volumes = lighting->isAnisotropic() ? 4u : 1u;
            for (size_t v = 0; v < volumes && all; ++v) {
                Ogre::TextureGpu *tex = lighting->getLightVoxelTextures()[v];
                if (!tex || tex->getResidencyStatus() != Ogre::GpuResidency::Resident) {
                    all = false;
                    break;
                }
                Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
                    tex->getWidth(), tex->getHeight(), tex->getDepth(),
                    Ogre::TextureTypes::Type3D, tex->getPixelFormat());
                try {
                    ticket->download(tex, 0u, true);
                    const Ogre::TextureBox box = ticket->map(0);
                    const size_t rowBytes = size_t(tex->getWidth()) * box.bytesPerPixel;
                    for (Ogre::uint32 z = 0; z < tex->getDepth(); ++z)
                        for (Ogre::uint32 y = 0; y < tex->getHeight(); ++y) {
                            const Ogre::uint8 *row =
                                reinterpret_cast<const Ogre::uint8 *>(box.at(0, y, z));
                            for (size_t b = 0; b < rowBytes; ++b) {
                                h ^= row[b];
                                h *= 1099511628211ull;
                            }
                        }
                    ticket->unmap();
                } catch (Ogre::Exception &e) { mError = e.getFullDescription(); all = false; }
                  catch (std::exception &e)  { mError = std::string("engine: ") + e.what(); all = false; }
                tm->destroyAsyncTextureTicket(ticket);
            }
            if (all) {
                char buf[17];
                std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
                st.lightDigest = buf;
            }
        }

        // The DIRECT volume exists only on a cascade that bounces (ogre-patch
        // 0076's D term). Its peak is the normalisation's own self-check.
        if (Ogre::TextureGpu *direct = lighting->getLightDirectTexture()) {
            Walk wd;
            if (walk(direct, wd)) {
                st.peakDirect = wd.peak;
                st.directAtMax = wd.atMax;
            }
        }

        // AND THE SOURCE THE INJECTION SEEDS FROM (VOXEL-CLIP-1, ogre-patch
        // 0087): the voxeliser's EMISSIVE volume, in scene radiance. The lit
        // volumes above are the injection's OUTPUT, and an emitter clipped on
        // its way in is indistinguishable there from a dimmer emitter — the
        // same argument that put the volumes in this readback in the first
        // place, one stage earlier. No normalisation: the voxeliser writes the
        // material's emissive as authored.
        if (voxelizer) {
            Ogre::TextureGpu *emissive = voxelizer->getEmissiveVox();
            if (emissive && emissive->getResidencyStatus() == Ogre::GpuResidency::Resident) {
                st.emissiveFormat =
                    Ogre::PixelFormatGpuUtils::toString(emissive->getPixelFormat());
                Walk we;
                if (walk(emissive, we)) {
                    st.peakEmissive = we.peak;
                    st.emissiveAtMax = we.atMax;
                    st.emissiveAboveOne = we.aboveOne;
                }
            }
        }
    } JAH_CATCH(mError, st);
    return st;
}

// THE PROBE FACE PASS'S RENDER-QUEUE CEILING — `rq_last 200` in
// media/Hlms/Jahshaka/JahshakaPcc.compositor (both face passes). Gizmos and
// selection outlines live at RQ 210 and are never captured; keep in step.
static const Ogre::uint8 kProbeFaceRqLast = 200u;

bool OgreScene::probeSeesItem(const Node &n) const {
    if (!n.item || !n.shown) return false;
    if (!(n.item->getVisibilityFlags() & kVisibleBit)) return false;   // helper / distortion
    return n.item->getRenderQueueGroup() <= kProbeFaceRqLast;
}

bool OgreScene::materialSeenByGi(MaterialId id, bool &voxelized) const {
    voxelized = false;
    bool seen = false;
    for (const auto &kv : mNodes) {
        if (kv.second.materialRef != id || !kv.second.item) continue;
        if (kv.second.item->getVisibilityFlags() & kGiGeometryBit) { voxelized = true; seen = true; break; }
        if (probeSeesItem(kv.second)) seen = true;
    }
    return seen;
}

void OgreScene::settleTextureResidency() {
    // ONE empty() TEST on a scene that is not waiting for anything — which is
    // every frame once a scene has opened. Entries appear only in setPbrTexture,
    // and only for a bind whose texture was not resident yet.
    if (mMaterialsAwaitingTexture.empty()) return;
    // EVERY ENTRY THAT BECAME READY THIS FRAME IS ONE EVENT (clean-2 lane
    // review, F5). A scene's textures arrive in batches, and noting each
    // material separately staled the grid once per material and — worse — bumped
    // the material generation once per material, which is what the host re-reads
    // to re-inject the voxel bounce. Ten maps landing together used to buy ten
    // re-injects; they buy one.
    bool stale = false, bumpVoxels = false;
    for (size_t i = 0; i < mMaterialsAwaitingTexture.size();) {
        const MaterialId mat = mMaterialsAwaitingTexture[i].first;
        const bool voxelInput = mMaterialsAwaitingTexture[i].second;
        auto mit = mMaterials.find(mat);
        bool waiting = false;
        if (mit != mMaterials.end()) {
            for (size_t sl = 0; sl < kPbrTextureSlotCount && !waiting; ++sl) {
                const TextureId tid = mit->second.boundTextures[sl];
                if (!tid) continue;
                auto tit = mTextures.find(tid);
                if (tit == mTextures.end() || !tit->second.texture) continue;
                waiting = !tit->second.texture->isDataReady();
            }
        }
        if (waiting) { ++i; continue; }
        // The material (or the whole entry) is settled. A material that died
        // while waiting simply leaves, having staled nothing.
        if (mit != mMaterials.end()) {
            bool bump = false;
            if (giMaterialChangeEffect(mat, voxelInput, bump)) { stale = true; bumpVoxels |= bump; }
        }
        mMaterialsAwaitingTexture[i] = mMaterialsAwaitingTexture.back();
        mMaterialsAwaitingTexture.pop_back();
    }
    if (stale) staleProbeGrid(GiStaleReason::Material);
    if (bumpVoxels) {
        ++mGiMaterialGeneration;
        // THE STORE'S ROWS ARE STALE, and this is the signal that says so: a voxel
        // input on a material a GI-visible item wears has changed. The next build
        // re-reads every converted datablock IN PLACE (convertPendingMaterials) - the
        // slots stay, so every instance's material word stays valid.
        mVctMaterialRefreshOwed = true;
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: material generation -> " + std::to_string(mGiMaterialGeneration) +
                " (a voxel-input texture finished streaming)");
    }
}

// WHAT A MATERIAL EDIT COSTS THE GI CACHES — THE ONE DEFINITION, so the single
// edit and the batched settle below can never decide it differently.
// Returns "the probe grid is stale because of this"; `bumpVoxels` says the
// voxel/IR solve has to be re-read too.
bool OgreScene::giMaterialChangeEffect(MaterialId id, bool voxelInputsChanged,
                                       bool &bumpVoxels) const {
    bumpVoxels = false;
    if (mGi.mode == GiMode::Off) return false;
    // NOTHING CACHED TO INVALIDATE: no probe grid and no voxelizer built yet,
    // or a from-scratch rebuild already owed (it reads every material fresh and
    // stales the whole grid itself). This is also what keeps the node walk
    // below off the LOAD path — every material and every texture bind is pushed
    // once while a scene opens, and an O(nodes) walk per push there is the
    // O(nodes x binds) class that once cost 2.1 s of boot (setPbrTexture's
    // note). A live edit on a built arm walks once per push.
    // (UNDER A CASCADE CHAIN a pending flush is NOT a from-scratch rebuild any
    // more — `applyPendingGi` takes the dirty path, which keeps every voxeliser
    // and therefore every cached material conversion. Swallowing the generation
    // bump there would lose the edit outright, so the chain does not take this
    // early-out; the second clause, which is what keeps the node walk below off
    // the scene-load path, still applies to it.)
    if ((mGiCachesDirty && mVctCascades.empty()) ||
        (!mPcc && !mVctVoxelizer)) return false;
    bool voxelized = false;
    if (!materialSeenByGi(id, voxelized)) return false;   // nothing GI can see wears it
    bumpVoxels = voxelized && voxelInputsChanged;
    return true;
}

void OgreScene::noteMaterialChanged(MaterialId id, bool voxelInputsChanged) {
    // THE SURFACE CACHE'S ONE INVALIDATION HOOK (SURFACE-CACHE phase 2), and
    // it is deliberately BEFORE the GI effect test: a card holds a picture of a
    // surface, so ANY change to what that surface looks like stales it —
    // including the ones `giMaterialChangeEffect` declines because they cannot
    // move a VOXEL (a roughness edit, a normal map, a material nothing GI-lit
    // wears yet). It throws the cards of the instances wearing THAT material
    // back on the queue and frees no page; the counter model is
    // `gi.material_swap`'s.
    if (mSurfaceCache) mSurfaceCache->noteMaterialChanged(id);
    bool bumpVoxels = false;
    if (!giMaterialChangeEffect(id, voxelInputsChanged, bumpVoxels)) return;
    staleProbeGrid(GiStaleReason::Material);
    if (bumpVoxels) {
        ++mGiMaterialGeneration;
        mVctMaterialRefreshOwed = true;   // see the sibling above
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: material generation -> " + std::to_string(mGiMaterialGeneration) +
                " (material " + std::to_string((unsigned long long)id) + " changed a voxel input)");
    }
}

void OgreScene::staleProbeGrid(GiStaleReason why) {
    // THE SCENE'S OWN RECORD FIRST, BEFORE THE PROBE-LESS RETURN (the smoke-fix
    // hand-down, item 4). `mLastStaleReason`/`mStaleSerial` are read by
    // `giStatus()` and by every monitor row this file files — they describe why
    // the GI arm did work, which has nothing to do with whether the scene has a
    // reflection-probe grid. Written after the return, a plain VCT scene (no
    // PCC) reported `None` for ever, so a capture of a cascade scroll could not
    // even see E0's `Camera` reason it exists to report.
    mLastStaleReason = why;
    ++mStaleSerial;
    // WHICH INPUT OWES THE CAPTURE (DRAG-1). The motion deferral below holds
    // only the captures MOTION asked for; any other input still spends the
    // budget in the frame it arrives, drag or no drag.
    if (why != GiStaleReason::Moved) mProbeStaleBeyondMotion = true;
    if (!mPcc) return;
    const size_t n = mPcc->getProbes().size();
    if (mProbeSlots.size() != n) mProbeSlots.assign(n, ProbeSlot());
    for (ProbeSlot &sl : mProbeSlots) { sl.sweepPending = true; sl.staleReason = why; }
}

// WHAT THE FRAME ACTUALLY RE-CAPTURES (GiStatus::probeCapturesLastFrame).
// Counted at the last moment before the render, from the probes' own dirty
// flags, so it covers every source that can raise one — the budget, a shape
// clamp's CubemapProbe::set — rather than trusting any one of them to report
// itself. A from-scratch placement captures synchronously inside buildPcc —
// buildStart's updateAllDirtyProbes over every CANDIDATE probe, and a closing
// one of our own over the survivors once their shapes are final — bypassing
// the flags, so it reports its own count through mPlacementCapturesThisFrame.
void OgreScene::latchProbeCaptures(bool drawn) {
    // A SCENE NOTHING DRAWS THIS FRAME CAPTURES NO PROBES. Ogre's automatic PCC
    // captures from its own frame listener (allWorkspacesBeginUpdate) for every
    // scene in the process, but only the scenes an enabled view draws get
    // updateSceneGraph — so an undrawn scene's probe would render against a
    // light list and transforms from whenever it was last drawn. Measured (E2,
    // 2026-09-12): after a GI rebuild of the off-screen editor scene, the
    // placement's own updateAllDirtyProbes ends in clearFrameData, a same-frame
    // capture's shadow node early-outs with the sun still in its slot 0, and
    // the pass is hashed as "one directional caster, zero lights" — a PBS
    // shader that cannot compile ('lights' : no such field in 'passBuf'), and
    // the shader-cache save then crashed on the broken PSO. Paused, the probes
    // wait for the frame the scene is drawn again; nothing else is gated by
    // mPaused (the rebuild's own placement runs updateAllDirtyProbes directly).
    if (mPcc) mPcc->mPaused = !drawn;
    int captures = mPlacementCapturesThisFrame;
    mPlacementCapturesThisFrame = 0;
    if (drawn && mPcc) {
        for (const Ogre::CubemapProbe *p : mPcc->getProbes())
            if (p->mDirty && p->mEnabled) ++captures;
    }
    mProbeCapturesLastFrame = captures;
    // THE MONITOR'S CACHE-WORK RECORD (RENDER_LOOP_MONITOR_SPEC §4.7). One
    // entry per probe that is about to capture, carrying the input change that
    // staled it — or `None`, which is the value that matters: a probe captured
    // with no recorded input change is redundant work. RECORDED, never judged.
    if (monitor::live()) {
        // A capture the PLACEMENT did (buildPcc's synchronous
        // updateAllDirtyProbes, which bypasses the dirty flags) is a build.
        if (captures > int(mProbeDirtiedThisFrame.size()))
            monitor::noteCacheWork(CacheKind::Probe, WorkReason::Build, 0, "placement",
                                   unsigned(captures - int(mProbeDirtiedThisFrame.size())));
        for (const auto &pd : mProbeDirtiedThisFrame)
            monitor::noteCacheWork(CacheKind::Probe, monitor::reasonOf(pd.second), pd.first,
                                   "capture", 6u);   // six cube faces per capture
        monitor::noteProbeCaptures(unsigned(std::max(0, captures)));
    }
    mProbeDirtiedThisFrame.clear();
}

void OgreScene::setNodeGiBoundsExcluded(NodeId id, bool excluded) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.giBoundsExcluded == excluded) return;
    it->second.giBoundsExcluded = excluded;
    noteSceneTransformWrite();      // the escape signature reads this flag
    // The flag changes WHERE GI happens, so it is exactly as much of a change
    // as moving the geometry: flag the caches and let the frame-time flush
    // rebuild once, however many nodes the caller toggles in a burst.
    invalidateGiCaches();
}

bool OgreScene::nodeGiBoundsExcluded(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.giBoundsExcluded;
}

// ---- GI internals ----
// The GI-participating items' world AABBs, AFTER the two document-driven
// filters: the per-node "exclude from GI bounds" flag, and the extent-outlier
// TRIMMING below. Everything that reasons about the shape of the lit world
// starts here so that the lit volume and the probe region can never disagree
// about which objects define it.
//
// WHY ANY OF THIS EXISTS. Every default scene has a ground plane 200 units
// across under a handful of 1-2 unit primitives. Unioning it puts the voxel
// volume and the probe grid over 40,000 square units of empty air — the "basic
// cubemap" look GI_SPEC blamed on probe count — so an enormous piece of
// scenery must not be allowed to define the lit world on its own.
//
// WHAT WAS WRONG WITH THE FIRST ATTEMPT (the owner's regression, rig-measured
// 2026-09-07). It took the MEDIAN largest extent and DROPPED every item more
// than 4x it, and only ran at four items or more. Both halves are cliffs:
//   * a drop is binary, so an item goes from defining the volume to not
//     existing between one rebuild and the next;
//   * the median is an order statistic, so ONE added object can move it by a
//     factor of five (extents [1,1,10,10] -> median 5.5, all kept; add one more
//     1 -> median 1, both tens dropped);
//   * and the "four items or more" gate is itself a cliff by construction —
//     the scene's fourth mesh changed the answer by two orders of magnitude
//     (measured: 3 cubes -> y +-17, 4 cubes -> y +-1.1).
// The visible symptom is a scene going dark, or losing its bounce, because a
// user added an object.
//
// THE REPLACEMENT — trim, never drop; and never shrink below what is already
// lit. Three properties, each one killing one of the cliffs above:
//
//  1. A SMOOTH POPULATION SCALE. The reference size is the GEOMETRIC MEAN of
//     the items' largest extents, not the median. log(scale) is an arithmetic
//     mean, so adding one item moves it by (log e - log scale)/(n+1): bounded,
//     and it never jumps the way an order statistic does.
//  2. A CONTINUOUS PER-ITEM WEIGHT, not a verdict. w = 1 while the item is at
//     most kOutlierSoftStart scales big, 0 once it is kOutlierSoftEnd scales
//     big, smoothstep in LOG space between. An item's contribution is then
//     lerp(item clipped into the content region, item's own AABB, w) — a box
//     that morphs continuously from trimmed to whole. At w = 1 the result is
//     byte-identical to the plain union, which is the overwhelmingly common
//     case and returns early.
//  3. A HYSTERESIS FLOOR. An item that the PREVIOUS auto-resolved lit volume
//     already covered is never trimmed, whatever its weight. This is what makes
//     "add one object" incapable of collapsing a live scene's volume: the
//     ground that was lit stays lit. It resets when GI is switched off and on,
//     when the scene's bounds are typed by hand, and when the item is removed
//     or flagged out of the bounds — every deliberate user action still gets a
//     fresh, tight fit.
//     ITS ONE QUALIFIER, and it is load-bearing (see noteGiAutoVolume): the
//     floor only remembers a fit made over a real POPULATION, two items or
//     more. A scene holding nothing but a ground plane has nothing to be an
//     outlier against, so the ground IS the volume — recording that would pin
//     the default editor scene's thousand-unit ground for the session. The
//     guarantee is therefore the narrower, honest one: ONCE A SCENE HAS
//     CONTENT, adding more content can never collapse its lit volume.
//
// The document's explicit per-node exclude flag remains the deterministic
// escape hatch, and it runs BEFORE all of this: an excluded item is not in the
// population, cannot be protected by the hysteresis floor, and therefore still
// shrinks the volume the instant it is flagged.
// The PLAIN gather, before any outlier trimming: every GI item's world AABB as
// it actually is.
// AN ITEM IS A SLAB FOR AN AXIS when it is at least kSlabAspect times broader
// on BOTH other axes than it is thick on this one. Self-relative, so it is
// scale-free and population-free — a floor is a floor whatever else is in the
// scene, and nothing here reads a position or a size constant. The lit volume's
// ground clip (giItemBounds) is its ONE caller.
static bool giIsSlab(const Ogre::Aabb &a, size_t ax);

std::vector<Ogre::Aabb> OgreScene::giItemBoundsRaw() const {
    std::vector<Ogre::Aabb> all;
    all.reserve(mNodes.size());
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (kv.second.giBoundsExcluded) continue;
        ++mGiAabbReads;
        all.push_back(const_cast<Ogre::Item *>(item)->getWorldAabbUpdated());
    }
    return all;
}

// THE CONTENT THE CURRENT FIT WAS MADE FOR (lane ENGINE-7 item 2). A hash of
// the gathered boxes, each quantized by its OWN largest extent — the same
// quantum giGeometrySignature uses, so an idle sway or a settling physics body
// reads as "unchanged" here exactly as it does there. Order is mNodes' order,
// which is stable for a still scene.
unsigned long long OgreScene::giContentSignature(const std::vector<Ogre::Aabb> &boxes) {
    unsigned long long h = 1469598103934665603ull;      // FNV-1a
    const auto fold = [&h](unsigned long long v) { h ^= v; h *= 1099511628211ull; };
    for (const Ogre::Aabb &a : boxes) {
        const float quantum = giAabbQuantum(a);
        const Ogre::Vector3 mn = a.getMinimum(), mx = a.getMaximum();
        for (size_t ax = 0; ax < 3u; ++ax) {
            fold((unsigned long long)(long long)std::floor(mn[ax] / quantum));
            fold((unsigned long long)(long long)std::floor(mx[ax] / quantum));
        }
    }
    return h;
}

std::vector<Ogre::Aabb> OgreScene::giItemBounds() const {
    std::vector<Ogre::Aabb> all = giItemBoundsRaw();
    // What this fit is being made FOR. The memo below is keyed on it.
    mGiFitContentNow = giContentSignature(all);
    // One item IS the scene; there is no population to be an outlier against.
    mGiLastItemCount = all.size();
    // THE ANSWER ALREADY ADOPTED FOR THIS CONTENT (lane ENGINE-7 item 2, round
    // 2). Everything below is a pure function of the gathered boxes AND of the
    // volume the last fit produced — and that second input is what makes a
    // re-fit non-idempotent: an outlier the first fit trimmed sits inside the
    // volume that trim produced, so the next fit keeps it WHOLE and the volume
    // grows to hold it (Showroom 2: 48.14 -> 56.62 m, 0.376 -> 0.442 m per
    // voxel, on the second solve of an open nobody touched).
    //
    // So the fit is COMPUTED ONCE PER CONTENT and remembered: while the content
    // signature holds, every caller gets the answer that was adopted for it.
    // That is both halves of the contract in one line — no ratchet (the fit
    // cannot re-read its own output) and no oscillation (a re-solve with
    // unchanged content cannot drop a floor an earlier solve granted; arming
    // the floor on the content change alone did exactly that, 48 -> 56 -> 48,
    // which is worse than the ratchet because it takes light away one solve
    // late).
    if (mGiFitBoxesValid && mGiFitBoxesKey == mGiFitContentNow) return mGiFitBoxes;
    const auto adopt = [this](std::vector<Ogre::Aabb> out) {
        mGiFitBoxes = out;
        mGiFitBoxesKey = mGiFitContentNow;
        mGiFitBoxesValid = true;
        return out;
    };
    if (all.size() < 2u) return adopt(all);

    const size_t n = all.size();
    std::vector<float> extents(n);
    double logSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Ogre::Vector3 s = all[i].getSize();
        extents[i] = std::max(std::max(std::max(s.x, s.y), s.z), 1e-4f);
        logSum += std::log(double(extents[i]));
    }
    const float scale = float(std::exp(logSum / double(n)));
    if (!(scale > 0.0f)) return adopt(all);   // degenerate (all points) — keep everything

    // The weight ramp, in log space so it is scale-invariant.
    static const float kOutlierSoftStart = 4.0f;    // content up to here
    static const float kOutlierSoftEnd   = 16.0f;   // pure scenery beyond here
    const float logStart = std::log(kOutlierSoftStart);
    const float logSpan  = std::log(kOutlierSoftEnd) - logStart;

    // THE HYSTERESIS FLOOR (property 3): items the previous auto volume covered
    // are kept whole, so ADDING an object can never take light away from
    // something that was already lit (gi.cliff's live table).
    //
    // It applies to every fit this function actually COMPUTES — which, since
    // the memo above, is one per content: the volume it reads is the one
    // adopted for the PREVIOUS content, which is exactly the question the floor
    // asks ("did this change take light away from something already lit?").
    const bool havePrev = mGiAutoVolumeValid;
    const Ogre::Vector3 prevMin = mGiAutoVolume.getMinimum();
    const Ogre::Vector3 prevMax = mGiAutoVolume.getMaximum();
    const auto coveredByPrev = [&](const Ogre::Aabb &a) {
        if (!havePrev) return false;
        const Ogre::Vector3 mn = a.getMinimum(), mx = a.getMaximum();
        return mn.x >= prevMin.x && mn.y >= prevMin.y && mn.z >= prevMin.z &&
               mx.x <= prevMax.x && mx.y <= prevMax.y && mx.z <= prevMax.z;
    };

    std::vector<float> w(n, 1.0f);
    // How far into the ramp each item is, kept because the SLAB CLIP below has
    // its own, shorter ramp over the same axis (round-2 review F2).
    std::vector<float> ramp(n, 0.0f);
    bool anyTrimmed = false;
    for (size_t i = 0; i < n; ++i) {
        const float t = (std::log(extents[i] / scale) - logStart) / logSpan;
        if (t <= 0.0f) continue;                                  // content: w = 1
        const float c = std::min(t, 1.0f);
        w[i] = 1.0f - (c * c * (3.0f - 2.0f * c));                // smoothstep
        if (coveredByPrev(all[i])) { w[i] = 1.0f; continue; }     // already lit: keep it whole
        ramp[i] = c;
        if (w[i] < 1.0f) anyTrimmed = true;
    }
    if (!anyTrimmed) return adopt(all);   // the common case: the plain union, untouched

    // THE CONTENT CORE: every item at its weight, an outlier collapsing towards
    // its own centre rather than vanishing. Continuous in w by construction —
    // and GEOMETRICALLY so, for the same reason the final morph below is: a
    // linear collapse leaves 2.4% of a 200-unit ground still measuring 5 units,
    // which would then define the "content" it is supposed to be excluded from.
    // kCoreFloor is a FRACTION of the item's own size rather than an absolute,
    // so the shape of the ramp does not depend on the scene's units.
    static const float kCoreFloor = 1e-3f;
    Ogre::Vector3 coreMin(1e30f), coreMax(-1e30f);
    for (size_t i = 0; i < n; ++i) {
        const Ogre::Vector3 c = all[i].mCenter;
        const float shrink = std::pow(kCoreFloor, 1.0f - w[i]);
        const Ogre::Vector3 h = all[i].mHalfSize * shrink;
        coreMin.makeFloor(c - h); coreMax.makeCeil(c + h);
    }
    // THE REGION an outlier is trimmed into: the core plus half of itself
    // again, so the floor a scene stands on keeps the patch under (and around)
    // the content and loses only the empty acres.
    static const float kRegionGrow = 0.5f;
    const Ogre::Vector3 grow = (coreMax - coreMin) * (kRegionGrow * 0.5f);
    const Ogre::Vector3 regionMin = coreMin - grow, regionMax = coreMax + grow;

    // THE CONTENT PATCH — what a SUPPORTING SLAB is clipped to (round-2 review
    // F2). It is the union of the items that are NOT outliers at all (w == 1),
    // i.e. the content itself, grown by a fraction of ITS OWN size — and that
    // is the whole point of computing it separately from the core above: the
    // core contains the collapsed outlier, so it carries the SLAB's own size
    // into the answer (a 1 m cube on a 200 m ground resolved to a 2 m patch and
    // on a 50 m ground to a 15 m one — the same scene, three answers). Nothing
    // below reads the slab's extent.
    //
    // THE MARGIN IS A FRACTION OF THE CONTENT'S SMALLEST EXTENT, and that
    // choice is measured rather than tasteful. A patch that ends exactly on the
    // content's own boundary takes the floor's bounce away with the acres: on a
    // bare scene — one 2 m crate on the default ground, the case
    // scripting.e2e.live_texture_pixels photographs — the crate's lit surface
    // reads 83 of 255 with a volume its own size, 162 at twice it, 179 at four
    // times and 220 with the whole 46 m the old fit gave it (GI off reads 83,
    // i.e. at its own size the bounce contributes NOTHING). So the patch has to
    // reach a real distance past the content, and the honest scale for "how far
    // does a floor's bounce matter" is the content's own SMALLEST dimension —
    // its height, for anything standing on the ground — not its footprint,
    // which would grow a room's patch by the width of the room.
    //
    // Half of it, each side: a 2 m crate gets a 4 m patch (162, the bounce back
    // within a point of what it was) and an 18 m room whose storey is 4.75 m
    // gets 2.4 m of floor past its walls. Nothing here reads the slab.
    static const float kSlabPatchMargin = 0.5f;    // x the content's smallest extent, a side
    Ogre::Vector3 contentMin(1e30f), contentMax(-1e30f);
    bool haveContent = false;
    for (size_t i = 0; i < n; ++i) {
        if (w[i] < 1.0f) continue;
        contentMin.makeFloor(all[i].getMinimum());
        contentMax.makeCeil(all[i].getMaximum());
        haveContent = true;
    }
    Ogre::Vector3 patchMin = coreMin, patchMax = coreMax;
    if (haveContent) {
        const Ogre::Vector3 csize = contentMax - contentMin;
        const float reach = kSlabPatchMargin *
                            std::max(std::min(std::min(csize.x, csize.y), csize.z), 1e-4f);
        patchMin = contentMin - Ogre::Vector3(reach);
        patchMax = contentMax + Ogre::Vector3(reach);
    }
    // THE GEOMETRIC BLEND between two boxes, by `k` (0 = a, 1 = b): sizes
    // interpolated in LOG space and centres linearly, which is exactly what the
    // outlier morph below does and for the reason stated there (a linear blend
    // of a 2-unit box and a 200-unit one spends almost all of its travel near
    // the large end). Shared so the two paths cannot drift apart.
    const auto blendBoxes = [](const Ogre::Vector3 &amn, const Ogre::Vector3 &amx,
                               const Ogre::Vector3 &bmn, const Ogre::Vector3 &bmx, float k) {
        Ogre::Vector3 omn, omx;
        for (size_t ax = 0; ax < 3u; ++ax) {
            const float ac = 0.5f * (amn[ax] + amx[ax]), ah = 0.5f * (amx[ax] - amn[ax]);
            const float bc = 0.5f * (bmn[ax] + bmx[ax]), bh = 0.5f * (bmx[ax] - bmn[ax]);
            const float eps = 1e-4f;
            const float c = ac + (bc - ac) * k;
            const float h = std::exp(std::log(std::max(ah, eps)) * (1.0f - k) +
                                     std::log(std::max(bh, eps)) * k);
            omn[ax] = c - h; omx[ax] = c + h;
        }
        return std::make_pair(omn, omx);
    };

    std::vector<Ogre::Aabb> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Ogre::Vector3 mn = all[i].getMinimum(), mx = all[i].getMaximum();
        if (w[i] >= 1.0f) { out.push_back(all[i]); continue; }
        // WHAT THE GROUND SUPPORTS, AND NO MORE (UNPIN-1's measurement, lane
        // ENGINE-4 item 5). An oversized item that is a SLAB — a self-relative
        // shape test of this ONE item, and the only thing read here: no
        // position, no size constant, no world origin — is
        // the thing the rest of the scene stands on. Its extent past the
        // content is empty ground, and lighting it costs resolution: on the
        // three shipped rooms the morph below left the 100 m default ground
        // measuring +-20.6 m around an 18 m room (41.3 m of volume at 0.32 m
        // per voxel, with visible cone-trace banding), +-18.3 around 24, and
        // +-32.5 around 48 — roughly twice the room, every time, and exactly
        // the room once the ground was hidden.
        //
        // So a slab contributes on its BROAD axes only where the content is:
        // clipped to the content core, which is the same box the trim region
        // below is grown from. Its THIN axis is kept whole, because that is the
        // surface itself — the floor stays in the volume, it just stops
        // reaching past the walls. Non-slab outliers (a big prop, an imported
        // vehicle) keep the geometric morph: they are content, not scenery.
        size_t thin = 3u;
        for (size_t ax = 0; ax < 3u; ++ax)
            if (giIsSlab(all[i], ax) &&
                (thin == 3u || all[i].mHalfSize[ax] < all[i].mHalfSize[thin]))
                thin = ax;
        if (thin != 3u) {
            Ogre::Vector3 smn = mn, smx = mx;
            for (size_t ax = 0; ax < 3u; ++ax) {
                if (ax == thin) continue;
                smn[ax] = std::max(smn[ax], patchMin[ax]);
                smx[ax] = std::min(smx[ax], patchMax[ax]);
                if (smn[ax] > smx[ax]) {                  // no overlap: a point at the patch
                    const float c = std::min(std::max(all[i].mCenter[ax], patchMin[ax]), patchMax[ax]);
                    smn[ax] = smx[ax] = c;
                }
            }
            // CONTINUOUS AT THE RAMP ENTRANCE (round-2 review F2). Clipping a
            // slab the instant its weight leaves 1 would be a cliff of exactly
            // the kind the trim was rewritten to remove: a floor sitting near
            // kOutlierSoftStart would snap between WHOLE and FOOTPRINT when one
            // prop is added, and that snap is a GI brightness jump plus a full
            // re-voxelise. So the clip has its own ramp over the FIRST QUARTER
            // of the trim's — geometric, like every other blend here — and by
            // the time an item is a quarter of the way to "pure scenery" it is
            // the footprint, which is where every real ground plane already is
            // (the 100 m default ground in an 18 m room sits at 0.62 of the
            // trim ramp, and on a bare 100 m ground under one 2 m crate at
            // 0.41). The width is a MEASURED choice, not a taste: at a quarter
            // the ramp a 50 m ground under that same crate sat at 0.16, i.e.
            // inside the blend, and answered 5.53 m where the 100 m and 200 m
            // grounds both answered 2.23 — the slab size leaking back in
            // through the blend, which is the very thing the patch removes. A
            // tenth puts every one of them past the blend and leaves the
            // transition continuous, which is what it is for.
            static const float kSlabClipRamp = 0.1f;
            const float k = std::min(ramp[i] / kSlabClipRamp, 1.0f);
            const float kb = k * k * (3.0f - 2.0f * k);           // smoothstep
            const auto blended = blendBoxes(mn, mx, smn, smx, kb);
            out.push_back(Ogre::Aabb::newFromExtents(blended.first, blended.second));
            continue;
        }
        // The trimmed box...
        Ogre::Vector3 tmn = mn, tmx = mx;
        tmn.makeCeil(regionMin);  tmx.makeFloor(regionMax);
        for (size_t ax = 0; ax < 3u; ++ax)
            if (tmn[ax] > tmx[ax]) {                 // wholly outside the region
                const float c = all[i].mCenter[ax];  // ...keep it as a point there
                tmn[ax] = tmx[ax] = std::min(std::max(c, regionMin[ax]), regionMax[ax]);
            }
        // ...morphed back towards the whole one by w. The SIZE is interpolated
        // GEOMETRICALLY, not linearly, and that is not decoration: this
        // interpolates between a 2-unit box and a 200-unit one, and a linear
        // blend spends most of its travel near the large end (at w = 0.02 a
        // linear blend of 200 still measures 4 units of ground, i.e. twice the
        // content). Geometrically the RATIO moves smoothly instead, which is
        // exactly the quantity "no order-of-magnitude collapse" is about —
        // measured on the ground-plus-N-cubes table, the same step went from
        // 6.5x to 1.1x. Centres move linearly: they are bounded by the region
        // either way, and a geometric mean of a coordinate is meaningless.
        Ogre::Vector3 fmn, fmx;
        for (size_t ax = 0; ax < 3u; ++ax) {
            const float tc = 0.5f * (tmn[ax] + tmx[ax]), th = 0.5f * (tmx[ax] - tmn[ax]);
            const float fc = all[i].mCenter[ax],         fh = all[i].mHalfSize[ax];
            const float c = tc + (fc - tc) * w[i];
            const float eps = 1e-4f;
            const float h = std::exp(std::log(std::max(th, eps)) * (1.0f - w[i]) +
                                     std::log(std::max(fh, eps)) * w[i]);
            fmn[ax] = c - h; fmx[ax] = c + h;
        }
        out.push_back(Ogre::Aabb::newFromExtents(fmn, fmx));
    }
    return adopt(out);
}

// Does any GI item's world AABB lie (even partly) OUTSIDE the volume that is
// currently lit? Returns a signature rather than a bool: 0 when everything is
// covered, otherwise a hash of the escaping items' quantized AABBs.
//
// WHY A SIGNATURE (LIGHTING_FIX fix 2). The mirror already knows how to debounce
// "something changed that needs a re-solve": it compares a signature every
// frame, restarts a stability window whenever it differs, and re-solves once the
// value has held still. A bool cannot drive that — it stays true for the whole
// of a drag, so the window would either fire mid-drag every time or never. A
// hash of WHERE the escapee is changes on every frame of a drag (window keeps
// restarting, no per-frame rebuilds) and stops changing when the user lets go
// (exactly one re-solve). It is the same contract a light transform already has.
//
// Quantized to 1/64 of the lit volume's size so that sub-voxel jitter (a
// physics body settling, an animation's idle sway) cannot keep the window open
// forever.
unsigned long long OgreScene::giEscapeSignature() const {
    if (mGi.mode == GiMode::Off) return 0ull;
    // NOTHING TO ESCAPE FROM UNDER A CAMERA-CENTRED CHAIN (audit D3). This term
    // exists because a SCENE-FITTED volume has an outside: an object dragged
    // past it kept the lighting it had at the old place until something else
    // re-fitted. A cascade chain is fitted to the CAMERA, is re-centred by its
    // own scheduler, and has no fit to leave — so the old fit, which
    // `rebuildVct` still records for the probe half, was arming a settle and a
    // whole-chain rebuild for a volume nobody builds.
    if (mGi.cascades) return 0ull;
    if (!mGiAutoVolumeValid) return 0ull;      // nothing resolved yet, or hand-typed bounds
    // CACHED AGAINST THE MOVEMENT EPOCH (clean-2 lane, 2026-09-13). The mirror
    // reads this every frame of every VCT-like scene, and it is a pure function
    // of the GI items' boxes and the volume they are tested against — so on a
    // frame where nothing wrote a transform and nothing structural changed, the
    // answer is last frame's and the walk (a root-recursive
    // getWorldAabbUpdated per GI item) is skipped outright.
    // ...AND ONLY WHERE THE EPOCH CAN SEE THE HOST'S WRITES. Without a counter
    // from the host (Engine::setTransformWriteCounter) a document-side move
    // moves nothing the engine can read, so the cache would answer with a
    // signature from before the drag — caught by gi.coalesce and gi.budget,
    // which drive their drags through the document and wire no counter. Same
    // rule as ensureGiWalk's: no epoch, no skipping.
    const unsigned long long epoch = transformEpoch();
    if (detail::gTransformWriteCounter &&
        mEscapeSigValid && mEscapeSigEpoch == epoch && mEscapeSigMode == mGi.mode &&
        mEscapeSigVolumeValid == mGiAutoVolumeValid &&
        mEscapeSigVolume.mCenter == mGiAutoVolume.mCenter &&
        mEscapeSigVolume.mHalfSize == mGiAutoVolume.mHalfSize)
        return mEscapeSig;
    computeGiSignatures();
    return mEscapeSig;
}

// BOTH SIGNATURES, ONE WALK, OVER THE ITEM INDEX (audit D4).
//
// They used to be two separate functions, each walking `mNodes` — the node MAP,
// every node in the scene, lights and empties and helpers included — and each
// paying `getWorldAabbUpdated()` (a parent-chain walk that recomputes the whole
// SIMD block) per GI item. The host reads them in ONE statement, on every frame
// the movement epoch moved, and the epoch moves on EVERY frame of any animation
// or physics: ENGINE-7's "0.02 ms flying" was a STILL-scene number, and an alive
// scene was paying two O(all nodes) walks for two hashes of the same boxes. The
// header at their declarations already said they are "pure functions of the same
// boxes"; this is that sentence made true.
//
// (The MOVEMENT SCAN — ensureGiWalk/walkItems — is deliberately NOT folded in
// with them, and the reason is a contract rather than an oversight: its
// `mGiMovedBoxes` are per FRAME and are consumed by the probe budget inside
// `renderOneFrame`, while these two are read by the mirror
// BEFORE it, after the mirror has written this frame's transforms. One walk
// serving both would have to answer at two different epochs in the same frame,
// and the loser would be the probe budget's movers. Folding them needs the
// moved-box lifetime changed from per-frame to per-epoch first — reported, not
// built here.)
void OgreScene::computeGiSignatures() const {
    const unsigned long long epoch = transformEpoch();
    // The escape half is computed only where it means something: an explicit or
    // auto-fitted volume to be outside of, and not under a camera-centred chain
    // (which has no fit to leave — audit D3).
    const bool wantEscape = mGi.mode != GiMode::Off && !mGi.cascades && mGiAutoVolumeValid;
    const Ogre::Vector3 vmn = mGiAutoVolume.getMinimum(), vmx = mGiAutoVolume.getMaximum();
    const Ogre::Vector3 vsize = vmx - vmn;
    const float vquantum =
        std::max(std::max(std::max(vsize.x, vsize.y), vsize.z) / 64.0f, 1e-4f);
    static const unsigned long long kFnvBasis = 1469598103934665603ull;
    unsigned long long hg = kFnvBasis, he = kFnvBasis;
    const auto fold = [](unsigned long long &h, unsigned long long v) {
        h ^= v; h *= 1099511628211ull;
    };
    for (const Node *np : mItemNodes) {          // the item index, not the map (D4)
        const Ogre::Item *item = np->item;
        // A PROMOTED DRAG MOVER STAYS IN THIS SIGNATURE (MOVER-1), and it has
        // to. The host's settle gate measures STABILITY of exactly this hash:
        // an object that dropped out of it while being dragged would make the
        // scene look still from the mirror's side, and the full re-solve the
        // gate fires after kGiStableFrames would land in the MIDDLE of the
        // gesture — the hitch the whole mobility design exists to avoid. So the
        // predicate is "bounces light, or is a mover because the user has hold
        // of it", and at the gesture's end the hash is exactly the hash the
        // same pose would have had without the promotion (which is what makes
        // the at-rest picture identical under both rules).
        const bool dragged = np->dragMover && np->shown;
        if (!item || (!(item->getVisibilityFlags() & kGiGeometryBit) && !dragged)) continue;
        ++mGiAabbReads;
        const Ogre::Aabb a = const_cast<Ogre::Item *>(item)->getWorldAabbUpdated();
        const Ogre::Vector3 mn = a.getMinimum(), mx = a.getMaximum();
        // The GEOMETRY half: every GI item, quantised to a 64th of its OWN size.
        {
            const float quantum = giAabbQuantum(a);
            fold(hg, (unsigned long long)np->selfId);
            for (size_t ax = 0; ax < 3u; ++ax) {
                fold(hg, (unsigned long long)(long long)std::floor(mn[ax] / quantum));
                fold(hg, (unsigned long long)(long long)std::floor(mx[ax] / quantum));
            }
        }
        // The ESCAPE half: only what is OUTSIDE the fitted volume, quantised to a
        // 64th of THAT volume.
        if (!wantEscape || np->giBoundsExcluded) continue;
        if (mn.x >= vmn.x && mn.y >= vmn.y && mn.z >= vmn.z &&
            mx.x <= vmx.x && mx.y <= vmx.y && mx.z <= vmx.z) continue;
        fold(he, (unsigned long long)np->selfId);
        for (size_t ax = 0; ax < 3u; ++ax) {
            fold(he, (unsigned long long)(long long)std::floor(mn[ax] / vquantum));
            fold(he, (unsigned long long)(long long)std::floor(mx[ax] / vquantum));
        }
    }
    mGeomSig = hg;
    mGeomSigEpoch = epoch;
    mGeomSigMode = mGi.mode;
    mGeomSigValid = true;
    // THE ESCAPE HALF IS ONLY CACHED WHEN IT WAS COMPUTED (round-2 F9). A walk
    // entered through `giGeometrySignature` with no fitted volume to escape from
    // never folded a single item into `he`, and marking that answer valid would
    // have served a 0 to the next `giEscapeSignature` call at the same epoch —
    // after a `noteGiAutoVolume` had given the scene a volume, which is exactly
    // when the term starts meaning something. Its own keys, its own validity.
    if (wantEscape) {
        mEscapeSig = he == kFnvBasis ? 0ull : he;    // untouched == nothing escaped
        mEscapeSigEpoch = epoch;
        mEscapeSigMode = mGi.mode;
        mEscapeSigVolume = mGiAutoVolume;
        mEscapeSigVolumeValid = mGiAutoVolumeValid;
        mEscapeSigValid = true;
    }
}

void OgreScene::noteGiAutoVolume(const Ogre::Aabb &fitted, bool automatic) {
    mGiAutoVolume = fitted;
    // THE FLOOR ONLY REMEMBERS A REAL POPULATION, and this qualifier is the
    // difference between a useful hysteresis and a broken heuristic.
    //
    // A scene with ONE GI item has nothing to be an outlier against, so
    // giItemBounds keeps it whole and the volume is that object — for the
    // DEFAULT EDITOR SCENE that object is the ground, and the volume is a
    // thousand units across. Recording that as "what is already lit" would
    // protect the ground for the rest of the session: every cube the user then
    // added would be lit inside a 1000-unit voxel volume, which is the state
    // the trimming exists to prevent (caught by scripting.e2e.gi_bounds, which
    // measured exactly that: extent 1088 where it expects < 30).
    //
    // So the floor guarantees the narrower, honest thing: ONCE A SCENE HAS
    // CONTENT, adding more content can never collapse its lit volume. The very
    // first object to appear in an empty-but-for-scenery scene legitimately
    // re-centres the volume onto it, and that is the heuristic working.
    mGiAutoVolumeValid = automatic && mGiLastItemCount >= 2u;
    // THE MEMO IS NOT TOUCHED HERE, and that is the load-bearing half: every
    // re-solve is a teardown followed by a build, and the teardown comes
    // through this function with `automatic` false (the zero volume at the top
    // of rebuildGi). Dropping the memo there made the "unchanged content" test
    // fail on every re-solve — measured on a Showroom 2 open, which fits the
    // SAME 16-item signature seven times — so the floor read its own output
    // again and the volume grew 48.10 -> 56.62. The memo is keyed on the
    // content and nothing else, because nothing else changes the answer.
    noteSceneTransformWrite();      // the escape signature is relative to it
}

bool OgreScene::giBoundsExplicit() const {
    const Vec3 &a = mGi.testBoundsMin, &b = mGi.testBoundsMax;
    return a.x != b.x || a.y != b.y || a.z != b.z;
}

// THE CEILING ON THE AUTOMATIC VOLUME (SMOKE_FIX S14 /
// LIGHTING_PIPELINE_AUDIT L4.1).
//
// WHAT IT IS FOR, measured on the shipped default project as it was (born
// Realtime/Epic, one 1024 m ground plane and two lights — mainwindow.cpp's
// createDefaultScene; the same fix re-staged that plane to 100 m, and this
// ceiling is what stops the class rather than that one number):
//
//     Jahshaka GI: voxelized 1 items at 128^3 over -520 -8 -520 .. 520 8 520
//     Jahshaka GI: DDGI field 64x2x64 (8192 probes) over ... size 1040 16 1040
//     Jahshaka GI: PCC region ... grid 3x2x3 ... [all 18 probes CLAMPED]
//
// Voxels 8.1 m across, two irradiance probes on the whole vertical axis,
// eighteen reflection probes 346 m apart with nine of them four metres under a
// solid floor: 437 MB and milliseconds a frame computing a constant. The
// trimming in giItemBounds cannot help here and is right not to — with ONE item
// there is no population for an outlier to be an outlier against, and the
// geometric mean of one extent is that extent, so the ramp is a no-op by
// construction whatever gate it is behind. The ground IS the scene; it is just
// that a scene a kilometre across cannot be lit by 128 voxels.
//
// THE CEILING IS IN METRES, NOT IN METRES PER VOXEL, and that is a deliberate
// choice against the audit's preference (L4.4: metres-per-voxel is the quantity
// that decides whether GI means anything, and it would be tier-independent).
// Measured, a per-voxel ceiling SHRINKS the lit world as the quality dial goes
// down — 0.5 m/voxel is 64 m at High but 16 m at Low — and that took gi.cliff's
// "a scene that is only a ground plane" and gi.pcc_bounds' "a scene that IS one
// big mesh keeps the whole mesh in the lit volume" red at Low: the "a scene
// goes dark because a dial moved" class the LIGHTING_FIX lane exists to
// prevent. A fixed 64 m holds at every tier and still kills the 8 m voxel
// (High 0.5, Medium 1.0, Low 2.0 m per voxel), and GiStatus::voxelMetres
// reports the per-voxel reading the audit wants exposed.
//
// IT NEVER TOUCHES A PINNED VOLUME: computeGiBounds returns the user's own box
// before reaching here, which is what `world.fitGiBounds` and the bounds rows
// are for once a scene outgrows the ceiling.
//
// THE WINDOW IS CENTRED ON THE CONTENT, not on the union: the point of a lit
// volume is the things standing in it, and a ground plane's centre is only the
// content's centre by accident. `giContentCentre` is the MEAN OF THE CENTRES of
// the items that fit inside the ceiling (i.e. everything that is not scenery) —
// a mean rather than the centre of their union, so a window narrower than the
// content still lands where most of the geometry is instead of in the gap
// between two distant clusters. With nothing but scenery in the scene it falls
// back to the union's own centre, which for the default project is the origin
// the camera is already looking at. The window then slides to stay inside the
// union, so a content centre near an edge still spends the whole ceiling on
// real geometry.
bool OgreScene::giContentCentre(float maxEdge, Ogre::Vector3 &centre) const {
    Ogre::Vector3 sum(0.0f);
    size_t n = 0;
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (kv.second.giBoundsExcluded) continue;
        ++mGiAabbReads;
        const Ogre::Aabb a = const_cast<Ogre::Item *>(item)->getWorldAabbUpdated();
        const Ogre::Vector3 size = a.getSize();
        if (std::max(std::max(size.x, size.y), size.z) > maxEdge) continue;   // scenery
        sum += a.mCenter;
        ++n;
    }
    if (!n) return false;
    centre = sum / float(n);
    return true;
}

void OgreScene::clampAutoGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const {
    // THE 64 m CEILING IS THE ENGINE'S, NOT A PUSHED FIELD (PHOTON_SPEC §10's
    // E2 row): nothing a user can reach has ever varied it — the document
    // carries no bounds, `world.gi` refuses the key by name, and the mirror
    // never writes it — so it is a constant here and `testAutoBoundsMax` is a
    // suite's lever over it (negative = this value).
    //
    // WHY 64 METRES AND NOT METRES-PER-VOXEL, which is the quantity that really
    // decides whether GI means anything (LIGHTING_PIPELINE_AUDIT L4.4) and would
    // be tier-independent: a per-voxel ceiling SHRINKS the lit world as the
    // quality dial goes down (0.5 m/voxel is 64 m at High but 16 m at Low), and
    // that breaks standing contracts — measured, it took gi.cliff's "a scene
    // that is only a ground plane" and gi.pcc_bounds' "a scene that IS one big
    // mesh keeps the whole mesh" red at Low, the "scenes go dark when a dial
    // moves" class the LIGHTING_FIX lane exists to prevent. A fixed 64 m holds
    // at every tier and still kills the 8 m voxel a 1 km ground plane used to
    // produce: High 0.5, Medium 1.0, Low 2.0 m per voxel.
    static const float kAutoGiBoundsMax = 64.0f;
    const float maxEdge = mGi.testAutoBoundsMax < 0.0f ? kAutoGiBoundsMax
                                                       : mGi.testAutoBoundsMax;
    if (!(maxEdge > 0.0f)) return;
    const Ogre::Vector3 size = mx - mn;
    if (size.x <= maxEdge && size.y <= maxEdge && size.z <= maxEdge) return;

    Ogre::Vector3 centre = (mn + mx) * 0.5f;
    Ogre::Vector3 content;
    if (giContentCentre(maxEdge, content)) centre = content;

    const float half = maxEdge * 0.5f;
    for (size_t ax = 0; ax < 3u; ++ax) {
        if (mx[ax] - mn[ax] <= maxEdge) continue;
        float lo = centre[ax] - half, hi = centre[ax] + half;
        if (lo < mn[ax]) { lo = mn[ax]; hi = lo + maxEdge; }   // slide, never shrink
        if (hi > mx[ax]) { hi = mx[ax]; lo = hi - maxEdge; }
        mn[ax] = std::max(lo, mn[ax]);
        mx[ax] = std::min(hi, mx[ax]);
    }
}

bool OgreScene::computeGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const {
    const Vec3 &a = mGi.testBoundsMin, &b = mGi.testBoundsMax;
    if (giBoundsExplicit()) {
        mn = Ogre::Vector3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
        mx = Ogre::Vector3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
        return true;
    }
    const std::vector<Ogre::Aabb> items = giItemBounds();
    if (items.empty()) return false;
    mn = Ogre::Vector3(1e30f); mx = Ogre::Vector3(-1e30f);
    for (const Ogre::Aabb &aabb : items) {
        mn.makeFloor(aabb.getMinimum());
        mx.makeCeil(aabb.getMaximum());
    }
    clampAutoGiBounds(mn, mx);
    // The margin is ONE VOXEL per axis, and no more (P1a). It exists for exactly
    // one reason: a surface lying exactly on the union's boundary would sit on
    // the volume's face, where it may or may not be rasterised into a voxel. One
    // voxel of slack removes the question.
    //
    // It used to be 10% + 0.5 absolute, which was our invention — upstream's own
    // `VctVoxelizer::autoCalculateRegion` is the plain union with NO margin at
    // all — and it cost twice: a volume 20% larger per axis at a FIXED voxel
    // resolution spends ~40% of its voxels on empty air, and (measured in
    // gi.pcc_bounds) an over-inflated volume moves the VCT cone hit far enough
    // from the probe's parallax hit that `getPccVctBlendWeight` hands the pixel
    // to VCT and the probe reflection vanishes. In this room the old margin was
    // 1.58 units and the reflection died between 1.0 and 1.3.
    const Ogre::Vector3 size = mx - mn;
    const float res = float(giVoxelResolution());
    Ogre::Vector3 margin = size / res;
    // Degenerate axes (a single ground plane is flat in Y) get a real one from
    // the scene's own scale rather than a magic constant.
    const float maxExtent = std::max(std::max(size.x, size.y), size.z);
    const float floorMargin = std::max(maxExtent / res, 1e-3f);
    for (size_t a = 0; a < 3u; ++a) margin[a] = std::max(margin[a], floorMargin);
    mn -= margin; mx += margin;
    return true;
}

// Voxel volume resolution per axis, by quality. Shared so the bounds margin can
// be expressed in voxels rather than in a made-up percentage.
//
// THE NUMBER COMES FROM THE ONE TIER TABLE (`giQualityFacts`, Types.h): the
// app's tier descriptions are generated from the same function, so what a tier
// SAYS and what it DOES cannot drift (render audit A5).
unsigned OgreScene::giVoxelResolution() const {
    return giQualityFacts(mGi.quality).voxelResolution;
}

// THE ONE SHAPE READING LEFT IN THIS FILE (ENGINE-4 item 5's ground clip).
//
// An item is a SLAB for an axis when it is at least this many times broader on
// BOTH other axes than it is thick on this one. Self-relative, so it is
// scale-free and population-free: nothing about the rest of the scene can make
// a floor stop being a floor, and nothing here reads a position, a count or a
// world origin. Its ONE caller is `giItemBounds`, which clips an oversized slab
// to the content it supports — a statement about ONE item's own shape, not
// about the layout of a scene and not about any enclosure.
//
// 2.0 IS MEASURED, not chosen. The two populations it has to separate, taken
// from the shapes the suites and the shipped samples actually contain:
//   floors and walls    gi.pcc_bounds' THICK room — a 1.4 m wall over a 5 m
//                       storey — is the slimmest real one at 3.57; the thin
//                       rooms are 12.5, a ground plane is hundreds;
//   everything else     a column or pillar is 1.00 whatever its size (square
//                       cross-section, by definition), an imported car 1.36,
//                       a sofa ~1.1.
// So the honest split is the geometric middle of 1.36 and 3.57, and it costs
// 1.8x of margin on the slab side and 1.5x on the other.
//
// (THE ROOM-MEASURING PROBE RULE THAT USED TO LIVE HERE IS GONE — lane R5-ROOM,
// 2026-09-15, owner+lead joint decision "no room in any definition", PHOTON_SPEC
// §13. `computeProbeRegion` read the LAYOUT of a scene's items — facing slabs,
// covering faces, an enclosed-axis count — to decide both where the probe grid
// lived and whether it was built at all. No lighting decision may test for a
// room, an enclosure, a wall or an axis count: the replacement is what each
// probe SEES, measured from its own captured depth, and it is in `buildPcc`.)
static const float kSlabAspect = 2.0f;
static bool giIsSlab(const Ogre::Aabb &a, size_t ax) {
    const size_t o1 = (ax + 1u) % 3u, o2 = (ax + 2u) % 3u;
    const float thin  = std::max(a.mHalfSize[ax], 1e-5f);   // a plane has zero
    const float broad = std::min(a.mHalfSize[o1], a.mHalfSize[o2]);
    return broad > 0.0f && broad >= kSlabAspect * thin;
}

// THE PARALLAX SHAPE CANNOT BE BIGGER THAN THE SPACE THE PROBES LIVE IN
// (FIX WAVE defect A2, 2026-09-07).
//
// `PccPerPixelGridPlacement::buildEnd` shrink-fits every probe from ONE 1x1
// AVERAGED depth value per cube face: `0.5 * fDist / fApproxDist`, where
// fApproxDist is measured to the region box (OgrePccPerPixelGridPlacement.cpp,
// processProbeDepth). Averaging one ratio over a 90-degree face is only well
// behaved when the face sees a wall. Put anything between the probe and the
// wall — the Grand Showroom's columns, a partition, a parked car — and the
// average is pulled towards the NEAR object on some faces and towards the
// FAR wall on others, and the reconstructed box comes out far larger than the
// room: measured on the shipped Showroom, shapes reaching +-23.68 units in a
// room spanning +-12.25.
//
// Downstream that is not a soft error. `getPccVctBlendWeight` compares the hit
// the oversized parallax box reconstructs against the VCT cone hit, finds them
// further apart than the trust window, and hands the pixel to cone tracing —
// which in an interior is black. The artifact is hard-edged and quantized to
// the Forward+ cluster grid, because the trust test flips per cluster cell,
// which is exactly how the owner saw it: black rectangles crawling over the
// metal as the camera moves.
//
// A parallax box larger than the probe REGION is never right — the region IS
// the space the grid was placed in, i.e. the scene's own fitted box, and a
// reflection reprojected onto a box bigger than everything the renderer knows
// about is reprojecting onto nothing. So every fitted shape is clamped into it,
// per axis. This does not fight the shrink-fit: a shape that fits inside the
// region is untouched, which is the case for every probe in a plain empty room.
//
// IT RUNS AFTER THE DEPTH RULE HAS ALREADY READ THE FIT (R5-ROOM), and that
// order is load-bearing in both directions: this clamp is what makes an
// unshrunk box harmless, and it is also what makes an unshrunk box
// indistinguishable from a shrunk one — so the rule that decides which probes
// are worth keeping reads the placement's own ratios BEFORE this runs.
//
// Ogre's own bookkeeping is respected rather than poked around: the shape is
// re-published through `CubemapProbe::set`, keeping the probe's camera
// position, influence AREA, inner region and orientation exactly as the
// placement left them. It also raises `mDirty`, so the clamped probes
// re-capture on the next frame, which is what we want anyway.
//
// AND THE RE-PUBLISH SAYS SO (lane SKY-FALLBACK-1, second read). `set` used to
// re-apply its own 1.005 padding to anything it was handed, so this function
// divided both boxes by a copy of that constant on the way in — a private
// number copied out of the pin, and a divide-then-multiply round trip that is
// not even bit-exact. ogre-patch 0049 gave `set` a `bValuesAlreadyPadded`
// argument for exactly this: these ARE the probe's own boxes, padding included,
// so they are handed back as they are and the constant is gone.
void OgreScene::clampProbeShapesToRegion(const Ogre::Aabb &region) {
    if (!mPcc) return;
    // The clamp target is the region grown slightly. Clamping to the region
    // EXACTLY puts a box face on the floor plane the region was pulled in to,
    // and a parallax ray that leaves a surface lying in its own box face
    // reprojects at ~zero distance: measured on the Grand Showroom, an exact
    // clamp stippled the polished floor (min luminance 0.320 -> 0.260 with
    // visible scan-line noise). Upstream pads its own fit by 1% for the same
    // reason (OgrePccPerPixelGridPlacement.cpp "// Padding").
    static const float kClampPad = 1.05f;
    const Ogre::Aabb padded(region.mCenter, region.mHalfSize * kClampPad);
    const Ogre::Vector3 rmn = padded.getMinimum(), rmx = padded.getMaximum();
    const Ogre::CubemapProbeVec &probes = mPcc->getProbes();
    const bool debug = giDebug();
    mProbesClampedToRegion = 0;
    for (size_t i = 0; i < probes.size(); ++i) {
        Ogre::CubemapProbe *p = probes[i];
        const Ogre::Aabb shape = p->getProbeShape();
        Ogre::Vector3 smn = shape.getMinimum(), smx = shape.getMaximum();
        Ogre::Vector3 cmn = smn, cmx = smx;
        cmn.makeCeil(rmn); cmx.makeFloor(rmx);
        // Degenerate only if the fit ran off the region entirely on some axis.
        // Keep a real box there rather than an inverted one: the probe's own
        // camera position is inside the region by construction, so collapsing
        // onto it is the honest fallback.
        const Ogre::Vector3 cam = p->getProbeCameraPos();
        bool changed = false, degenerate = false;
        for (size_t ax = 0; ax < 3u; ++ax) {
            if (!(cmn[ax] < cmx[ax])) {
                const float c = std::min(std::max(cam[ax], rmn[ax]), rmx[ax]);
                const float h = std::max((rmx[ax] - rmn[ax]) * 0.01f, 1e-3f);
                cmn[ax] = std::max(c - h, rmn[ax]); cmx[ax] = std::min(c + h, rmx[ax]);
                degenerate = true;
            }
            if (cmn[ax] != smn[ax] || cmx[ax] != smx[ax]) changed = true;
        }
        if (!changed) continue;
        // COUNTED, not just logged (2026-09-07). Every clamp here is a probe
        // whose depth-readback shrink-fit came back with a box outside the
        // space the grid was fitted to — i.e. one 1x1 averaged sample per cube
        // face that meant nothing, which is what a room with anything standing
        // in it produces. giStatus reports the count so "the fit is degenerate
        // in this scene" is a number the author can see, instead of a fact
        // buried behind an environment variable.
        ++mProbesClampedToRegion;
        const Ogre::Aabb area = p->getArea();
        const Ogre::Aabb clamped = Ogre::Aabb::newFromExtents(cmn, cmx);
        p->set(cam, area, p->getAreaInnerRegion(), p->getOrientation(), clamped,
               /*bValuesAlreadyPadded*/ true);   // ogre-patch 0049
        if (debug) {
            const auto toS = [](const Ogre::Vector3 &v) {
                return Ogre::StringConverter::toString(v);
            };
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI:  probe " + std::to_string(i) + " shape CLAMPED to region: " +
                toS(smn) + " .. " + toS(smx) + "  ->  " + toS(cmn) + " .. " + toS(cmx) +
                (degenerate ? "  (fit had left the region on some axis)" : ""));
        }
    }
}

void OgreScene::invalidateGiCaches(const Ogre::Aabb *where, bool geometryVoxelsChanged,
                                   bool somethingDied) {
    // EVERY STRUCTURAL CHANGE TO THE SCENE FUNNELS THROUGH HERE — a mesh
    // attached or detached, a node destroyed, a material or texture replaced,
    // a light removed. It deliberately does NOT touch the cached lamp maps any
    // more (ENGINE_CACHE_POLICY_SPEC P3): the caster scan sees an attach, a
    // detach, a removal or a visibility edge by itself, per LIGHT, in the frame
    // it happens (OgreScene::collectShadowCacheFrame) — funnelling them through
    // here re-rendered every lamp in the scene for a prop spawned in one room.
    // The depth-relevant edits the scan cannot see (material parameters, new
    // vertex data) flag their items themselves (noteShadowShapeChanged,
    // updateMeshVertices).
    if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid)
        mGiCachesDirty = true;   // VCT never dereferences stale keys: the flush
                                 // rebuilds the whole arm from scratch — or,
                                 // under a cascade chain, marks the cascades the
                                 // change reaches and keeps everything else (G1)
    // ...AND A STAGED BUILD DESCRIBES A SCENE THAT NO LONGER EXISTS
    // (OPEN_COVER_SPEC §2 A). The stages run in later frames against the box the
    // SCOUT was fitted to, so a spawn between two of them would place the probe
    // grid over the world as it was before the spawn — measured as
    // `scripting.e2e.page_gi`'s 0 probes kept of 18 when a cube arrived after
    // the chain. So the machine REWINDS to its first stage and re-photographs
    // the space; it does NOT stop. Never from INSIDE a stage: those legitimately
    // touch the arm they are building.
    if (!mInGiStage) restartStagedProbeBuild();
    // THE DESTRUCTION GENERATION (FIX WAVE B4). Bumped unconditionally, and
    // unconditionally is the point: every caller of this function either
    // destroys something the GI arms hold a raw pointer into, or wants a
    // from-scratch rebuild for its own reasons. Being conservative here costs a
    // full rebuild that could have been a reuse; being clever here costs heap
    // corruption. The counter is the ONLY thing standing between the reuse arm
    // and the rule this file's header spends a paragraph on.
    // ...UNLESS NOTHING DIED (DRAG-1). A visibility edge is the one caller of
    // this family that destroys nothing — see invalidateGiCachesForVisibility
    // — and bumping here made a HIDE cost a from-scratch rebuild of the whole
    // single-volume arm, more than the DELETE of the same object.
    if (somethingDied) ++mGiDestroyGeneration;
    // (THE CHAIN'S `itemsStale` IS GONE, ATOM P4b. It closed a window in which a
    // scroll rebuild could dereference a dead `Item*` still attached to a
    // cascade's voxeliser; a voxeliser holds no Item* any more - the gather reads
    // the GPU scene, whose slot for a dead item is gone - so there is no window.)
    if (geometryVoxelsChanged) noteGiCascadeDirty(where);
}

// WHERE the scene changed, for the per-cascade dirty path (G1). A cascade is
// only re-voxelised when its own box can see the change, so an edit in one
// corner of a scene costs the cascades that reach that corner and nothing else.
// A null box is "somewhere": the whole chain owes a rebuild, which the scheduler
// still spends one per frame.
void OgreScene::noteGiCascadeDirty(const Ogre::Aabb *box) {
    if (mVctCascades.empty()) return;          // no chain: nothing to describe
    if (mGiCascadeDirtyAll) return;            // already the strongest statement
    if (!box) { mGiCascadeDirtyAll = true; mGiCascadeDirtyBoxes.clear(); return; }
    if (mGiCascadeDirtyBoxes.size() < kGiCascadeDirtyBoxCap) {
        mGiCascadeDirtyBoxes.push_back(*box);
        return;
    }
    // AT THE CAP THE LIST MERGES; IT DOES NOT GIVE UP (PHOTON audit F14). The
    // member's note has the why — seventeen crates settling in one corner used
    // to read as "the scene changed everywhere" and rebuild the chain out to the
    // horizon, one cascade per frame, for as long as they moved.
    //
    // LEAST MARGIN ENLARGEMENT — the R*-tree's metric, and NOT the volume
    // (the lead's read of the first version, 2026-09-18). A box's margin is the
    // sum of its extents; the new box joins the entry whose margin grows least,
    // so boxes near each other coalesce and boxes far apart stay apart.
    //
    // WHY NOT VOLUME, which is what a plain R-tree uses: THIS ENGINE'S BOXES GO
    // FLAT. A Plane primitive's world AABB has zero height, so the volume of
    // every coplanar box — and of their union — is zero, and a flat mover slid
    // in x/z merged at zero growth with any coplanar box HOWEVER FAR AWAY: the
    // first entry won every comparison and a floor of moving planes coalesced
    // into one slab spanning the whole scene. Never WRONG (a merged box is
    // conservative by construction) but the exact opposite of what the merge is
    // for, and it would have dirtied every cascade between two far-apart flat
    // clusters. The margin is degenerate for nothing: a zero-height box still
    // has width and depth, and a union that reaches further has a bigger one.
    // (Surface area would also do; the margin is one add per axis and has no
    // degenerate case at all.)
    //
    // O(cap) per call with cap 16, against a test that is O(cap) per cascade
    // anyway.
    const auto marginOf = [](const Ogre::Aabb &a) {
        // Half-extents, not extents — a factor of 2 changes no ordering. A
        // degenerate (null) box has a negative half size; max() keeps the
        // growth monotonic.
        return double(std::max(a.mHalfSize.x, 0.0f)) + double(std::max(a.mHalfSize.y, 0.0f)) +
               double(std::max(a.mHalfSize.z, 0.0f));
    };
    size_t best = 0;
    double bestGrowth = std::numeric_limits<double>::max();
    for (size_t i = 0; i < mGiCascadeDirtyBoxes.size(); ++i) {
        Ogre::Aabb merged = mGiCascadeDirtyBoxes[i];
        merged.merge(*box);
        const double growth = marginOf(merged) - marginOf(mGiCascadeDirtyBoxes[i]);
        if (growth < bestGrowth) { bestGrowth = growth; best = i; }
    }
    mGiCascadeDirtyBoxes[best].merge(*box);
}

// A DATABLOCK OR A TEXTURE THE MATERIAL STORE HOLDS IS DYING. Narrower than
// `invalidateGiCaches`: the store keys its conversions on the datablock POINTER,
// so a recycled address would silently paint the new material with the old one's
// colour - the eviction below is what prevents it (ogre-patch 0081).
void OgreScene::noteGiDatablockDied(Ogre::HlmsDatablock *dying) {
    if (!dying) {
        // A TEXTURE a converted material bound died (the caller cannot name a
        // datablock): the store's texture pool is re-copied by the owed refresh
        // (VctMaterial::refreshAll forgets its by-pointer slice cache), and every
        // cascade re-voxelises in place.
        mVctMaterialRefreshOwed = true;
        mGiCascadeDirtyAll = true;
        return;
    }
    // Evict from every voxeliser that may hold it (the single volume's and each
    // cascade's; cascade 0 may share the single one). Erasing an entry twice is
    // a no-op. Nothing else moves: the voxels already hold the dead material's
    // albedo where its items stood, and the items' own detach re-voxelised
    // those boxes (destroyMaterial).
    // ONE STORE, ONE EVICTION (A5b §2). This used to be the head voxeliser's store and
    // then every cascade's, one by one, because each owned its own cache.
    if (mVctMaterialStore) mVctMaterialStore->removeDatablock(dying);
}

// The stale reasons by name, for the JAHSHAKA_GI_DEBUG log alone (the monitor
// has its own mapping to WorkReason, and the host's to a script string).
static const char *giStaleReasonName(GiStaleReason r) {
    switch (r) {
    case GiStaleReason::None:     return "none";
    case GiStaleReason::Rebuild:  return "rebuild";
    case GiStaleReason::Refresh:  return "refresh";
    case GiStaleReason::Moved:    return "moved";
    case GiStaleReason::Light:    return "light";
    case GiStaleReason::Material: return "material";
    case GiStaleReason::Sky:      return "sky";
    case GiStaleReason::Ambient:  return "ambient";
    case GiStaleReason::Fog:      return "fog";
    case GiStaleReason::Mobility: return "mobility";
    case GiStaleReason::Camera:   return "camera";
    }
    return "?";
}

// The cascades the recorded dirty region can be seen from, marked `pending`.
// Returns how many were marked; clears the region either way.
size_t OgreScene::markDirtyCascadesPending(GiStaleReason why) {
    size_t marked = 0;
    for (VctCascade &c : mVctCascades) {
        // ONLY THE BOX TEST DECIDES (round-2 F2): a structural edit in one
        // corner, or a light REMOVAL that moved no geometry, must not mark the
        // whole chain - the removed light's bounce would vanish cascade by
        // cascade over N frames.
        bool hit = mGiCascadeDirtyAll;
        if (!hit) {
            const Ogre::Aabb box(c.centre, Ogre::Vector3(c.halfSize));
            for (const Ogre::Aabb &d : mGiCascadeDirtyBoxes)
                if (box.intersects(d)) { hit = true; break; }
        }
        if (!hit) continue;
        c.pending = 1;
        c.pendingReason = why;
        ++marked;
    }
    mGiCascadeDirtyBoxes.clear();
    mGiCascadeDirtyAll = false;
    return marked;
}

// THE END OF A DRAG GESTURE (MOVER-1) — where a dragged Still becomes still
// world again and pays for the whole gesture at once.
//
// WHAT "STILL" MEANS HERE is the same thing it means to the probe deferral:
// kProbeMotionSettleFrames ticks with no promoted mover moving. The clock is the
// scene's GI tick and not a wall clock, for the reason every settle in this
// engine counts frames (CLAUDE.md: a wall-clock settle measures nothing in this
// engine).
//
// WHAT IT COSTS, and it is the whole trade: ONE cascade dirty box per mover —
// the place it came to rest — so the chain re-voxelises it once, at the tier's
// own one-cascade-per-frame budget, instead of once per frame of the drag. The
// probe grid is staled ONCE here as well, with reason Mobility, because the
// object is re-joining the capture set it left at the promotion: every probe
// holding a photograph without it owes one capture, which is exactly what a
// mobility flip has always cost (setNodeMovable's step 2) and is what the
// deferred sweep would have spent at the end of the drag anyway.
//
// AND THE PICTURE AT REST IS THE PICTURE AT REST. Nothing here changes what the
// scene looks like once the settle has run — the object is GI geometry again,
// at its new pose, voxelised from scratch — which is the property gi.drag_mover
// asserts by rendering the same frame under both rules.
void OgreScene::endDragGestureIfStill() {
    if (mDragMovers.empty()) return;
    if (mDragMoverMoved) { mDragQuietTicks = 0; return; }
    if (++mDragQuietTicks < kProbeMotionSettleFrames) return;
    mDragQuietTicks = 0;
    bool anyDemoted = false;
    for (NodeId id : mDragMovers) {
        auto it = mNodes.find(id);
        if (it == mNodes.end()) continue;          // gone mid-gesture; nothing owed
        Node &n = it->second;
        if (!n.dragMover) continue;
        n.dragMover = false;
        n.scan.giMovedTick = 0;
        applyNodeVisibilityFlags(n);
        // The box it came to rest in. `scan.giBox` is where the walk last saw
        // it, which is that box — the place it LEFT was re-voxelised at the
        // promotion, so the chain owes only this one.
        noteGiCascadeDirty(&n.scan.giBox);
        anyDemoted = true;
    }
    mDragMovers.clear();
    if (!anyDemoted) return;
    ++mDragMoverGestures;
    // ...AND THE GESTURE OWES ITS CLOSING WORK, ALL OF IT AT ONCE AND LAST
    // (mDragSettleOwed's note has the measurement): one full at-rest light tick
    // and one probe sweep, paid on the first frame the re-voxelisations have
    // drained. NEITHER may be paid here, and that is the whole lesson of the
    // measurement: a probe capture PHOTOGRAPHS the room as the voxels light it,
    // so a sweep spent while the chain is still being rebuilt stores the room
    // WITHOUT the object that has just come to rest — 161,318 pixels at up to
    // 32/255 on the Mirror Room's dragged torus, stably, until something asked
    // for another capture. The order is the physics: geometry, then light, then
    // the photographs of it.
    mDragSettleOwed = true;
}

// THE PER-CASCADE DIRTY PATH (PHOTON_SPEC G1) — what makes an EDIT under the
// cascade arm affordable, and the counterpart of `refreshVctFast` for a chain.
//
// Before it, every settle re-solve (a box dragged and released), every material
// edit, every spawn, hide, delete, mobility flip and shadow-atlas growth went
// `refreshVctFast` (refuses under cascades) -> `rebuildVct` -> `teardownVct` +
// `buildCascadeArm`: N voxelisers destroyed and rebuilt from scratch in ONE
// frame. At room scale that was N x (3-6 ms GPU + 1.2-1.6 ms CPU); on a dense
// scene N x 17-108 ms.
//
// Nothing about an edit requires that. What an edit really says is:
//
//   * WHERE it happened — so only the cascades whose box reaches there owe a
//     re-voxelisation, and they owe it through the SAME one-per-frame queue the
//     camera scroll uses. Objects, voxel textures, light voxels and mesh
//     buffers are all KEPT.
//   * whether the ITEM SET changed — answered by each build's gather, which
//     reads membership off the GPU scene (ATOM P4b); nothing is held to re-select.
//   * whether a DATABLOCK died or a material PARAMETER changed — the only case
//     that needs a new voxeliser, and it gets one per cascade per frame with
//     the chain's lighting objects (and so its raw `mExtraCascades` pointers)
//     untouched (`VctLighting::setVoxelizer`, ogre-patch 0037).
//   * and, when nothing geometric moved at all, that a LIGHT changed — which is
//     a re-INJECTION over the voxels that are already there, on every cascade,
//     and never a re-voxelisation.
//
// BOUNCE ORDERING. With `numBounces > 1` a partial refresh lets an inner cascade
// read outer light from a cascade that has not caught up yet; the scheduler
// spends innermost-first, so the outer one is a frame or two behind and the next
// spend heals it. At the shipped default of one bounce `cascadeBounces` is 0 for
// every cascade and the order carries no meaning at all.
//
// Returns FALSE when there is no chain to mark — the caller then takes the
// from-scratch `rebuildVct`, which is where a chain gets built in the first
// place.
bool OgreScene::refreshCascadesFast() {
    if (mVctCascades.empty() || !mVctCascades[0].built || !mVctCascades[0].lighting)
        return false;
    if (mGi.mode != GiMode::Vct && mGi.mode != GiMode::VctPccHybrid) return false;
    // A CHAIN IS NOT A WHOLE ARM WHEN THE REST OF IT IS STILL OWED (fix round
    // item 1's belt). This path keeps a LIVE arm cheap, and a hybrid with no
    // probe grid — and no record of having dropped every candidate, which is a
    // BUILT state (`refreshVctFast` says so in the same words) — or a scene
    // that wants an irradiance field and has none is not one. Refusing forces
    // the caller's `rebuildVct`, which is the only thing that can finish it.
    //
    // ONLY WHILE NO BUILD IS STAGED: between two stages this is exactly the
    // state the machine is walking through on purpose, and refusing there would
    // rebuild the 57 ms chain on every edit inside the window.
    if (mGiBuildStage == GiBuildStage::Idle) {
        if (mGi.mode == GiMode::VctPccHybrid && !mPcc && !mProbesDropped) return false;
        if (ddgiWanted() && !mIfd) return false;
    }
    JAH_TRY {
        // A MATERIAL PARAMETER THE VOXELISER READ HAS CHANGED.
        const bool materialGen = mGiBuiltMaterialGeneration != mGiMaterialGeneration;
        if (materialGen) {
            // A PLAIN DIRTY HIT (A5b fix round): the store re-reads the edited rows in
            // place at the next build, so every cascade owes a rebuild of the SAME
            // voxeliser - spread one per frame like any other dirty region.
            mGiCascadeDirtyAll = true;
            mGiBuiltMaterialGeneration = mGiMaterialGeneration;
        }
        // WHY this refresh is about to mark what it marks (BOOTVOX-1's
        // diagnosis needed it and nothing reported it): the three inputs of
        // `markDirtyCascadesPending`'s hit test, beside the reason. A refresh
        // that marks the whole chain for a material generation reads very
        // differently from one that marks two cascades for a box.
        const bool dirtyAllNow = mGiCascadeDirtyAll;
        const size_t dirtyBoxes = mGiCascadeDirtyBoxes.size();
        // (THE MOVERS ARE NOT RE-RECORDED HERE, round-2 F4. `walkItems` folds
        // every mover's box into the region as it finds it — once, at the one
        // place that knows a box moved — and re-reading `mGiMovedBoxes` on this
        // path recorded the same boxes a second and a third time, spending the
        // 16-box cap three times as fast and collapsing a two-object edit into
        // "mark the whole chain".)
        const size_t marked = markDirtyCascadesPending(
            mLastStaleReason == GiStaleReason::None ? GiStaleReason::Refresh : mLastStaleReason);
        // NOTHING GEOMETRIC CHANGED — so this refresh was asked for by a LIGHT
        // (or by the host's explicit Refresh with nothing moved). Re-inject over
        // the voxels that are already there, on every cascade, at the full
        // bounce count: the picture the user is left looking at is the one a
        // full solve would have produced, and not one voxel was re-written.
        // AND IT IS NEVER SKIPPED, however settled the chain looks (LAMPREST-3
        // fix round, measured). Skipping it when the engine believed the chain
        // was already at its fixed point for the scene's lights — keyed on the
        // light-write serial and the ambient — turned
        // `scripting.e2e.movable_lamp_rest` red at 3/255, 10 runs of 10: the
        // suite's reference arm moves a MOVABLE lamp and asks for a refresh,
        // and the engine's serial does not see that pose change the way an
        // injection does, so the refresh injected NOTHING and the lamp's move
        // never reached the voxels at all (`chainSweeps` 0 for the whole arm).
        // A skipped injection that was needed is a wrong picture; the one it
        // would have saved is ~12 ms once per gesture. The saving belongs where
        // the signature actually lives — the mirror, which already decides
        // whether to ask — and is recorded for the lead rather than guessed at
        // here.
        if (!marked) refreshGiLighting(false);
        // NOTHING THE ARM HOLDS CAN STILL BE DANGLING: every cascade that could
        // hold a dead pointer re-selects its item set before its next build, and
        // the ones that need a new voxeliser are flagged for one.
        mGiBuiltGeneration = mGiDestroyGeneration;
        mGiReusedLastRefresh = true;
        // The probe CONTENTS are stale (the scene changed), the SHAPES are not —
        // the same decision, and the same paused-budget exception, as the
        // single-volume reuse arm's.
        if (mGi.updateBudget > 0) {
            staleProbeGrid(GiStaleReason::Refresh);
        } else if (mPcc) {
            for (Ogre::CubemapProbe *p : mPcc->getProbes()) p->mDirty = true;
            for (ProbeSlot &sl : mProbeSlots) sl.sweepPending = false;
            mLastStaleReason = GiStaleReason::Refresh;
            ++mStaleSerial;
        }
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: cascade refresh marked " + std::to_string(marked) + " of " +
                std::to_string(mVctCascades.size()) +
                " cascades pending (one per frame); nothing was torn down — reason " +
                std::string(giStaleReasonName(mLastStaleReason)) +
                ", dirtyAll " + (dirtyAllNow ? "yes" : "no") +
                ", dirty boxes " + std::to_string(dirtyBoxes) +
                ", material generation " + (materialGen ? "changed" : "same"));
        return true;
    } JAH_CATCH(mError, false);
}

// A VOXEL INPUT THAT IS STILL STREAMING MEANS "BUILD LATER", NOT "BUILD TWICE"
// (BOOTVOX-1, 2026-09-18, measured on the default scene at boot).
//
// THE DEFECT. The voxeliser reads a material's ALBEDO and EMISSIVE textures
// (VctMaterial copies exactly those two into its texture pool), so voxelising
// before they are resident stores the wrong albedo — and the engine knows it
// does: `settleTextureResidency` bumps `mGiMaterialGeneration` when the pixels
// arrive, and the next refresh gives every cascade a FRESH voxeliser through
// `refreshCascadesFast`. On the DEFAULT SCENE that fired every single boot: the
// ground's `tile.png` is bound on slot 0 (a voxel input) before it is
// data-ready, the chain is built from scratch in that same frame, the texture
// lands one frame later, and all four cascades were re-voxelised one per frame
// — rebuilds 1 -> 2 on each of them (5.75 / 1.29 / 1.42 / 1.36 ms CPU, plus the
// settle debt LAMPREST-3's incremental settle then owes), with the camera
// perfectly still. That is where the self-test's "boot re-voxelisation" came
// from; no phantom camera move was ever involved.
//
// THE RULE. The flush that BUILDS the arm waits for the voxel inputs it already
// knows are in flight. `settleTextureResidency` runs earlier in the same frame
// (OgreEngine::renderOneFrame), so the wait ends in the frame the last texture
// lands and the build then reads it — one build, correct the first time. And
// while the flush is owed, `updateGiTracking` does not run the cascade
// scheduler either, so nothing is voxelised in the meantime.
//
// IT IS BOUNDED, because a texture that never becomes ready must not park GI
// for ever (a decode that fails, a file that vanished): after
// `kGiVoxelTextureWaitFrames` deferrals the arm is built anyway, which is
// exactly the behaviour this replaces. The counter is per SCENE, it counts
// DEFERRALS (the flush asks once per frame, so that is frames in the case this
// exists for) and it resets the moment nothing is pending.
//
// ONLY THE AUTOMATIC FLUSH WAITS. `refreshGlobalIllumination` — the host asking
// explicitly, which is what a script's `world.refreshGi()` and every suite that
// asserts on the frame after it do — is answered immediately, as it always was.
// THE FIRST ARM WAITS FOR ITS SKY (PHOTON-ENV-1 audit F7). Pending = the capture
// is queued (it runs after updateSceneGraph in THIS frame), its convolution is
// queued (applyPendingIbl, the top of the next), or its SH read is in flight.
// A first build only: a live chain meets a sky change through
// noteEnvironmentChanged's settle, which is the cheap path for an edit. Bounded
// like the albedo wait — a scene whose capture never runs (it is never drawn)
// must not park its GI for ever — and a sky-less scene waits for nothing.
bool OgreScene::giEnvironmentPending() {
    const bool firstArm = mVctCascades.empty() && !mVctVoxelizer;
    const bool pending = firstArm && mSkyDesc.mode != SkyMode::NoSky &&
                         (mSkyCapturePending || mIblPending || mSkyShTicket != nullptr);
    if (!pending || mGiEnvWaitFrames >= kGiEnvWaitFrames) {
        mGiEnvWaitFrames = 0u;
        return false;
    }
    ++mGiEnvWaitFrames;
    return true;
}

bool OgreScene::giVoxelTexturesPending() {
    bool pending = false;
    for (const auto &e : mMaterialsAwaitingTexture)
        if (e.second) { pending = true; break; }        // .second = "a voxel input"
    // NOTHING IN FLIGHT: the wait is over and the next one starts fresh.
    if (!pending) {
        mGiVoxelTextureWaitFrames = 0u;
        mGiVoxelTextureWaitGaveUp = false;
        return false;
    }
    // ...AND ONCE GIVEN UP ON THIS SET, GIVEN UP FOR GOOD (the lead's fix-round
    // item 5). The cap used to reset the COUNTER and return false once, with the
    // entry still parked — so every later from-scratch rebuild re-charged its
    // own thirty frames against a texture that had already been waited out, and
    // a decode that never completes would have deferred every rebuild of the
    // session in bursts of thirty. The latch stands until the pending set is
    // EMPTY again (the pixels arrived, or the material died), which is also when
    // a genuinely new bind deserves its own budget.
    //
    // THE ENTRY IS LEFT PARKED ON PURPOSE: `settleTextureResidency` still owes
    // the material generation bump if those pixels ever land, and that bump is
    // what re-voxelises the chain with the albedo it was built without. Giving
    // up on WAITING is not giving up on the texture.
    if (mGiVoxelTextureWaitGaveUp) return false;
    if (++mGiVoxelTextureWaitFrames > kGiVoxelTextureWaitFrames) {
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: a voxel-input texture never became ready in " +
                std::to_string(kGiVoxelTextureWaitFrames) +
                " frames — building the arm anyway, and not waiting again for this one");
        mGiVoxelTextureWaitGaveUp = true;
        mGiVoxelTextureWaitFrames = 0u;
        return false;
    }
    return true;
}

// ONE STAGE OF A STAGED ARM BUILD (OPEN_COVER_SPEC §2 A). Returns true when the
// build is finished. Each stage is a whole, self-contained piece of the arm —
// never half of a GPU resource — so a scene torn down between two of them is in
// exactly the state `teardownVct` already knows how to unwind.
bool OgreScene::giBuildOwesWork() const { return mGiBuildStage != GiBuildStage::Idle; }

// A STAGED BUILD WHOSE WORLD MOVED REWINDS — IT DOES NOT STOP (fix round item 1).
//
// The first cut of this simply set the machine to `Idle`, and its comment said
// that made the flush build from scratch. It did not, and the hole was total:
// the CHAIN is live by then, so the flush takes `refreshCascadesFast`, which
// refuses only on a missing cascade 0 or the wrong mode — `rebuildVct` never
// ran again. The scene was left with its cascades, NO probe grid, NO irradiance
// field and `applyReflectionToAll` never called, for the rest of its life: sky
// reflections and cone diffuse where a hybrid was asked for. Every caller of
// `invalidateGiCaches` reached it inside the three-to-five frame window a
// streaming build occupies — a node destroyed, a mesh or material destroyed, a
// bounds exclusion, a mobility heal, and a primitive ADDED, which is exactly
// `project.create` followed by `scene.addPrimitive`.
//
// The rewind is the cheap correct answer, and the reason it is cheap is that
// the chain is NOT stale: `invalidateGiCaches` marks every cascade's item set
// and the flush's own dirty path spends them one per frame (G1). What went
// stale is the box the probes were about to be placed in, and that box is
// re-fitted by the scout — so the 57 ms chain is not paid again for an edit.
void OgreScene::restartStagedProbeBuild() {
    if (mGiBuildStage == GiBuildStage::Idle) return;
    // A grid `buildPccFit` created and `buildPccFinish` never bound is an
    // orphan the moment we rewind past it (item 2).
    destroyProbeGrid();
    mGiBuildStage = GiBuildStage::ProbeScout;
    mGiCachesDirty = true;      // the cascades still owe their marks
}

bool OgreScene::stepStagedGiBuild() {
    // A stage builds the very arm `invalidateGiCaches` watches; without this it
    // would abandon itself half way through.
    struct InStage {
        bool &f;
        explicit InStage(bool &b) : f(b) { f = true; }
        ~InStage() { f = false; }
    } inStage(mInGiStage);
    JAH_TRY {
        switch (mGiBuildStage) {
        case GiBuildStage::Idle:
            return true;
        case GiBuildStage::ProbeScout: {
            // THE BOX IS RE-FITTED HERE, never carried from the cascade stage:
            // an edit between two stages rewinds the machine to THIS one, and
            // the whole point of the rewind is that the grid is placed in the
            // world as it is now (`restartStagedProbeBuild`).
            Ogre::Vector3 mn, mx;
            if (!computeGiBounds(mn, mx)) {      // nothing to place a grid in
                mGiBuildStage = GiBuildStage::Field;
                return false;
            }
            buildPccScout(Ogre::Aabb::newFromExtents(mn, mx));
            // The scout is also the refusal: no probe workspace, no camera, no
            // grid. Skip straight to the field rather than placing nothing.
            mGiBuildStage = mPccStageOk ? GiBuildStage::ProbeFit : GiBuildStage::Field;
            return false;
        }
        case GiBuildStage::ProbeFit:
            buildPccFit();
            // `buildPccFit` deletes the grid when every candidate photographed
            // nothing (the sky is then the reflection), and there is nothing
            // left for stage 3 to finish.
            mGiBuildStage = mPcc ? GiBuildStage::ProbeFinish : GiBuildStage::Field;
            if (!mPcc) applyReflectionToAll();
            return false;
        case GiBuildStage::ProbeFinish:
            buildPccFinish();
            applyReflectionToAll();
            mGiBuildStage = GiBuildStage::Field;
            return false;
        case GiBuildStage::Field: {
            const auto tField = std::chrono::steady_clock::now();
            buildIrradianceField();
            const double msField = std::chrono::duration<double, std::milli>(
                                       std::chrono::steady_clock::now() - tField).count();
            if (monitor::live())
                monitor::noteCacheWork(CacheKind::Gi, monitor::reasonOf(mLastStaleReason), 0,
                                       "vct.field.build", 1u, float(msField));
            if (giDebug())
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka GI: irradiance field built in " + std::to_string(msField) +
                    " ms (staged) — the arm is complete");
            mGiBuildStage = GiBuildStage::Idle;
            return true;
        }
        }
        return true;
    // A STAGE THAT THREW LEAVES THE MACHINE IDLE WITH AN UNFINISHED ARM, and
    // that is the one state `refreshCascadesFast`'s belt below exists for: the
    // next flush sees a hybrid with no grid, refuses the cheap path and builds
    // from scratch.
    } JAH_CATCH(mError, (destroyProbeGrid(), mGiBuildStage = GiBuildStage::Idle,
                         mGiCachesDirty = true, true));
}

void OgreScene::applyPendingGi() {
    // NOTHING OF THIS WORLD IS ON SCREEN YET: spend nothing at all
    // (Scene::setLoading). The ordinary flush below is gated on the same thing
    // inside `rebuildVct`; this is the staged machine's half.
    const bool staged = (mGiBuildStage != GiBuildStage::Idle);
    if (staged && mSceneLoading) return;
    // THE ORDINARY FLUSH RUNS FIRST, AND IT RUNS WHILE A BUILD IS STAGED — the
    // first cut returned early here and that was a second hole of the same
    // family as item 1: the CHAIN is live between two stages, so an edit that
    // arrives in the window still has to reach it, and the cascades' dirty
    // marks are made on this path (`refreshCascadesFast` -> the one-per-frame
    // queue). It is cheap by construction: over a live chain it marks or
    // re-injects and never rebuilds.
    applyPendingGiFlush();
    // ...then ONE stage of the staged build. Re-read, because the flush may
    // have rewound the machine (an edit) or built the arm outright.
    if (mGiBuildStage != GiBuildStage::Idle && !mSceneLoading) {
        stepStagedGiBuild();
        // A `Complete` frame owes the WHOLE arm, however it was staged: a
        // thumbnail, a capture, a suite or a script's editor.frame must see the
        // finished picture, which is the rule that keeps every pixel gate and
        // both selftest hashes untouched.
        if (mFramePace == FramePace::Complete)
            while (mGiBuildStage != GiBuildStage::Idle) stepStagedGiBuild();
    }
}

// The flush proper — what `applyPendingGi` was before the staged machine.
void OgreScene::applyPendingGiFlush() {
    if (!mGiCachesDirty) return;
    mGiCachesDirty = false;
    // A STREAMING FRAME BUILDS THE CHAIN AND PARKS (see rebuildVct's tail).
    const bool stage = (mFramePace == FramePace::Streaming);
    struct StageFlag {
        bool &f;
        explicit StageFlag(bool &b, bool v) : f(b) { f = v; }
        ~StageFlag() { f = false; }
    } stageFlag(mGiStageBuild, stage);
    JAH_TRY {
        if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid) {
            // A STRUCTURAL CHANGE UNDER THE CHAIN IS NOT A CHAIN REBUILD (G1).
            // A spawn, a hide, a delete, a mobility flip and a preset apply all
            // arrive here through `invalidateGiCaches`, and all of them used to
            // tear down N voxelisers and re-upload every mesh buffer in one
            // frame. The dirty path answers them by marking the cascades the
            // change can be seen from and letting the scheduler spend them one
            // per frame; `rebuildVct` remains the answer when there is no chain
            // to mark (or nothing built yet).
            // ...AND A CHANGE OF CHAIN SHAPE IS NOT A STRUCTURAL CHANGE EITHER
            // (V1-RIG item 4): the VR column of the tier table has a different
            // CASCADE COUNT and a different outer step, so there is no dirty
            // box that can express it — the chain has to be built again from
            // the other column, exactly as a quality change is.
            if (mGiChainShapeDirty) {
                // THE DEBT IS PAID BY A BUILD, NOT BY A CALL (the lead's
                // fix-round item 1). `rebuildVct` can come back having built
                // nothing — no camera yet, or a voxel input still streaming
                // (BOOTVOX-1) — and clearing the flag first left the chain in
                // the OLD column with nothing to re-arm it: `updateGiTracking`
                // raises this flag on a driver CHANGE only, so a session that
                // began while an albedo was in flight kept the desktop's
                // cascade set for its whole life. The flush itself stays armed
                // through `mGiCachesDirty` either way.
                if (rebuildVct()) mGiChainShapeDirty = false;
            } else if (mVctCascades.empty() || !refreshCascadesFast())
                rebuildVct();  // fresh voxelizer over the LIVE scene
        }
    } JAH_CATCH(mError, );
}

// FORWARD+ DEPTH SLICES (LIGHTING_FIX fix 8 / F-F1).
//
// `setForwardClustered`'s last two arguments are the near and far ends of the
// LOGARITHMIC depth-slice distribution the clustered grid uses: 24 slices are
// spread between them, and lights are binned into the slice their depth falls
// in. They were hardcoded at 2 and 50 — numbers from the sample this engine's
// scene setup was written against. In a scene 8 units across, 22 of the 24
// slices sit behind everything that exists and the whole scene shares two
// cells' worth of depth resolution; in a scene 400 units across, everything
// past 50 shares the last slice. Either way the per-cell light list is far
// longer than it needs to be, which is what makes the 96-light budget bite.
//
// So it is derived from the camera and the scene's own extent instead. Two
// things keep it honest:
//   * RATE LIMIT. The call recreates the grid buffers (and, on a light-slot
//     change, would recompile shaders), so it is considered once every
//     kFwdPlusEveryNFrames frames, never per frame.
//   * HYSTERESIS. Even then it only re-applies when the wanted range is at
//     least 2x away from the one in force. A camera dollying smoothly must not
//     rebuild anything; a camera that has actually changed scale must.
void OgreScene::updateForwardPlusRanges(const Ogre::Camera *cam) {
    static const unsigned kFwdPlusEveryNFrames = 30u;
    // The default 2..50 ratio, kept: 24 log slices over 25x is the shape
    // upstream's sample tuned, and only its ABSOLUTE placement was wrong.
    static const float kRangeRatio = 25.0f;
    if (!cam) return;
    if (++mFwdPlusTick < kFwdPlusEveryNFrames) return;
    mFwdPlusTick = 0;
    JAH_TRY {
        // THE SCENE'S EXTENT IS CACHED AGAINST THE MOVEMENT EPOCH (clean-2
        // lane): the walk below is a root-recursive getWorldAabbUpdated per
        // drawn item, and a still scene's answer cannot have changed. Only the
        // CAMERA half below is recomputed on every tick.
        const unsigned long long epoch = transformEpoch();
        if (!detail::gTransformWriteCounter || !mFwdPlusBoundsValid ||
            mFwdPlusBoundsEpoch != epoch) {
            Ogre::Vector3 bmn(1e30f), bmx(-1e30f);
            size_t found = 0;
            for (auto &kv : mNodes) {
                Ogre::Item *item = kv.second.item;
                // Everything the VIEW draws — the still world and the movers
                // (kMovableBit) — because this is the camera's clustered grid,
                // not a capture's.
                if (!item || !(item->getVisibilityFlags() & (kVisibleBit | kMovableBit))) continue;
                ++mGiAabbReads;
                const Ogre::Aabb a = item->getWorldAabbUpdated();
                bmn.makeFloor(a.getMinimum()); bmx.makeCeil(a.getMaximum());
                ++found;
            }
            mFwdPlusBoundsValid = found != 0;
            if (mFwdPlusBoundsValid) mFwdPlusBounds = Ogre::Aabb::newFromExtents(bmn, bmx);
            mFwdPlusBoundsEpoch = epoch;
        }
        if (!mFwdPlusBoundsValid) return;   // an empty scene keeps whatever it has
        const Ogre::Vector3 mn = mFwdPlusBounds.getMinimum(), mx = mFwdPlusBounds.getMaximum();
        const Ogre::Vector3 camPos = cam->getDerivedPosition();
        // The far end: the distance to the furthest corner of what exists.
        float far2 = 0.0f;
        for (int c = 0; c < 8; ++c) {
            const Ogre::Vector3 corner((c & 1) ? mx.x : mn.x,
                                       (c & 2) ? mx.y : mn.y,
                                       (c & 4) ? mx.z : mn.z);
            far2 = std::max(far2, (corner - camPos).squaredLength());
        }
        const float wantMax = std::min(std::max(std::sqrt(far2), 1.0f),
                                       float(cam->getFarClipDistance()));
        const float wantMin = std::max(wantMax / kRangeRatio,
                                       float(cam->getNearClipDistance()) * 2.0f);
        if (!(wantMax > wantMin)) return;
        const bool maxMoved = wantMax > mFwdPlusMax * 2.0f || wantMax < mFwdPlusMax * 0.5f;
        const bool minMoved = wantMin > mFwdPlusMin * 2.0f || wantMin < mFwdPlusMin * 0.5f;
        if (!maxMoved && !minMoved) return;
        // Every other argument is byte-identical to createScene's call: only the
        // range moves, so no shader property (and therefore no shader) changes.
        applyForwardClustered(wantMin, wantMax);
    } JAH_CATCH(mError, );
}

void OgreScene::applyForwardClustered(float minDistance, float maxDistance) {
    if (!mSceneMgr) return;
    // WHAT MUST BE RE-ASSERTED AFTER THIS CALL: setForwardClustered destroys and
    // recreates the ForwardClustered object, so anything set ON that object goes
    // with it. (Instant Radiosity's `setEnableVpls` was the one such setting and
    // was re-armed here until IR was deleted — PHOTON_SPEC E2 (4). Nothing rides
    // the Forward+ object today; this note is the rule for the next thing that
    // does.)
    mSceneMgr->setForwardClustered(true, 16, 8, 24, 96, kDecalsPerCell, mCubemapProbeSlots,
                                   minDistance, maxDistance);
    mFwdPlusMin = minDistance; mFwdPlusMax = maxDistance;
}

bool OgreScene::ensureCubemapProbeSlots(size_t probeCount) {
    // Quantised, so a grid edit that does not cross a step costs nothing, and
    // capped, because the buffer is uploaded every frame.
    Ogre::uint32 want = kCubemapProbeSlotsDefault;
    while (want < probeCount && want < kCubemapProbeSlotsMax) want *= 2u;
    if (probeCount > kCubemapProbeSlotsMax) {
        // Honest about the one case this cannot cover: a user-chosen grid past
        // the ceiling can still overflow a cell, and the symptom is the black
        // rectangles this budget exists to remove. Said out loud rather than
        // discovered again.
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: " + std::to_string(probeCount) + " probes exceed the Forward+ per-cell "
            "cubemap budget of " + std::to_string(kCubemapProbeSlotsMax) +
            "; cells that see more than that will drop probes (dark patches on reflective "
            "surfaces). Use a smaller probe grid.");
    }
    if (want <= mCubemapProbeSlots) return false;   // monotonic: never pay the recompile twice
    mCubemapProbeSlots = want;
    applyForwardClustered(mFwdPlusMin, mFwdPlusMax);
    return true;
}

// THE FORWARD+ LIGHT CENSUS (fix 8 / F-F2) — and an honest statement of what it
// can and cannot see.
//
// What Forward+ actually does when a cell fills up is drop the light silently:
// `if( numLightsInCell->lightCount[0] < mLightsPerCell )` with no else branch,
// three times over in OgreForwardClustered.cpp (lines 479, 618, 759). There is
// no counter behind it and no accessor for the per-cell counts, so an EXACT
// "lights dropped this frame" number cannot be produced without patching
// upstream — which is a whole-tree engine rebuild for a diagnostic, and was
// left out of this lane deliberately.
//
// What CAN be said exactly is the necessary condition: a cell can only overflow
// if the scene holds more Forward+ lights than the per-cell budget in the first
// place. So this reports the scene's count of lights that go through the
// clustered list at all (everything except directionals, which ride the pass
// buffer) and the budget they compete for. `lights > budget` means drops are
// possible and the scene should be looked at; `lights <= budget` is a proof
// that nothing was dropped.
void OgreScene::forwardPlusLightCensus(unsigned &lights, unsigned &budget) const {
    lights = 0u;
    budget = 96u;      // the lightsPerCell argument createScene passes
    for (const auto &kv : mNodes) {
        const Ogre::Light *l = kv.second.light;
        if (!l) continue;
        if (l->getType() == Ogre::Light::LT_DIRECTIONAL) continue;
        ++lights;
    }
}

void OgreScene::updateGiTracking(const Ogre::Vector3 &camPos, bool driverStereo) {
    serviceVoxelReadouts();     // before any of this frame's GI work is recorded
    // THE VR CASCADE PROFILE FOLLOWS THE DRIVER (V1-RIG item 4). The view that
    // places the chain is the one that reads it, and in a session that is the
    // headset's — five times the pixels for half the frame. A change of driver
    // profile is a change of TABLE, so the chain is rebuilt through the ordinary
    // dirty flush rather than mutated: `resolveCascadeTable` reads this.
    if (driverStereo != mGiDriverStereo) {
        mGiDriverStereo = driverStereo;
        if (mGi.cascades && (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid)) {
            mGiChainShapeDirty = true;
            mGiCachesDirty = true;
        }
    }
    // The DDGI half runs FIRST and unconditionally: it is scene-fitted, so it
    // needs no camera at all, and it must run in plain VCT mode too — where
    // there is no PCC and the probe half below returns immediately. (Both
    // halves are driven once a frame, from the ONE authoritative view of the
    // scene: OgreEngine::renderOneFrame picks it, for the same reason the probe
    // budget must not be spent once per view.)
    mGiWalkedThisFrame = false;     // one GI walk per frame; the first consumer runs it
    ++mGiTick;                      // the gesture clock (MOVER-1)
    // THE AUTHORITATIVE CAMERA, remembered: the Photon arm is built around it,
    // and a rebuild can arrive on a frame where no view has tracked yet.
    const bool firstCamera = !mGiCamPosKnown;
    mGiCamPos = camPos; mGiCamPosKnown = true;
    // ...and the arm that was waiting for it (B3) is asked for now. Through
    // the ordinary pending-GI flush rather than by calling rebuildVct here, so
    // the build lands where every other structural rebuild lands — after the
    // frame's tracking updates, once per frame, inside applyPendingGi's
    // try/catch.
    if (firstCamera && mGiCascadeAwaitingCamera &&
        (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid))
        mGiCachesDirty = true;
    updateIrradianceField();
    // THE CASCADE SCHEDULER (PHOTON_SPEC P0). Before the probe half and
    // unconditionally: it is per SCENE and per FRAME, it costs one quantise
    // and three comparisons per cascade when the camera has not left its step
    // cell, and it is a no-op in every arm but Photon's.
    // ...AND NOT ON A FRAME THAT IS ABOUT TO REBUILD THE WHOLE ARM (audit D7):
    // `applyPendingGi` runs LATER IN THIS SAME FRAME and its `rebuildVct`
    // rebuilds the chain at this very camera, so a scroll rebuild spent here
    // would be torn down within the frame that paid for it.
            // THE MOVEMENT SCAN, UNDER A CHAIN (G1). Without it `mGiMovedBoxes` is only
    // ever filled for a scene with a live probe budget, so a plain-VCT cascade
    // scene would have no idea WHERE anything
    // moved and every edit would mark the whole chain. It is epoch-gated like
    // every other consumer — a still scene runs no walk at all — and D4 makes
    // it the one walk the frame pays for. The walk itself records the boxes
    // (round-2 F4); this only spends them.
    if (!mVctCascades.empty()) {
        JAH_TRY {
            ensureGiWalk();
            // A STILL OBJECT THAT IS MOVING RIGHT NOW IS RE-VOXELISED WHILE IT
            // MOVES (smoke rig 2026-09-15, ledger §320) — not left until the
            // settle. THE DEFECT, measured on the monolithic arm: a STILL-
            // classified cube dragged in the editor keeps a voxel copy of
            // itself at its OLD pose, because a geometry move restarts BOTH of
            // the mirror's stability gates on every frame of the gesture, so
            // the full re-solve never fires during it; the every-tenth-frame
            // `refreshGiLighting(inMotion)` then re-injects light over those
            // stale voxels and the cone's self-occlusion start bias paints
            // vertical stripes on the lit face (a 15 degree turn of a 2 m cube
            // displaces its corners 4.4 voxels at High).
            //
            // The settle was the right place to answer it only while an answer
            // cost a WHOLE from-scratch arm. It no longer does: the cascades the
            // mover's box (old united with new) actually reaches are marked
            // here, and the scheduler below spends AT MOST ONE of them in this
            // frame, innermost first — so the near field, which is what the user
            // is looking at, follows the object live and the outer cascades
            // drain within N frames of the gesture ending. The frame budget is
            // the same one a camera scroll lives inside.
            //
            // The MONOLITHIC arm is deliberately unchanged here: one volume
            // cannot be re-voxelised per frame at any useful resolution, and
            // what to do there is the owner's call, not this lane's.
            // THE GESTURE'S END (MOVER-1), before the marking below so the
            // demotion's own box is spent by this same frame's scheduler.
            endDragGestureIfStill();
            if (!mGiCascadeDirtyBoxes.empty() || mGiCascadeDirtyAll)
                markDirtyCascadesPending(GiStaleReason::Moved);
        } JAH_CATCH(mError, );
    }
    if (!mGiCachesDirty) updateCascades(camPos);
    if (!mPcc || !mGiCamera) return;
    JAH_TRY {
        mGiCamera->setPosition(camPos);
        mPcc->setUpdatedTrackedDataFromCamera(mGiCamera);
        updateProbeBudget(camPos);
    } JAH_CATCH(mError, );
}

// THE GEOMETRY MOVEMENT SCAN (FIX WAVE B3, engine half).
//
// Walks the GI items once and records which of them MOVED since the last scan,
// as the union of each mover's old and new world AABB. Two consumers, both of
// them cheap paths: the probe round-robin below (a probe whose parallax shape
// contains a moved box goes to the front of the sweep) and — through the
// separate, stateless giGeometrySignature() — the mirror's debounce.
//
// The comparison is QUANTIZED to a 64th of the lit volume for the same reason
// giEscapeSignature is: sub-voxel jitter (a physics body settling, an idle
// animation's sway) must not read as movement for ever. An item seen for the
// FIRST time is recorded, not reported — appearing is not moving, and the
// arrival is already in the signature.
void OgreScene::runItemWalk(bool shadow) {
    // THE FRAME'S CASTER WALK (ENGINE_CACHE_POLICY_SPEC §4 row 7 + P3), run by
    // OgreEngine::applyShadowCacheDirties for every drawn scene right after
    // updateSceneGraph — so it reads the world AABBs the update has just made
    // current (getWorldAabb, no root-recursive getWorldAabbUpdated per item:
    // measured 2.1 ms -> 0.4 ms at 5k items in Debug+ASan).
    //
    // THE GI HALF IS NOT HERE, and that is a correctness rule, not a taste:
    // its consumer (the probe budget) runs EARLIER in the frame, before any
    // scene graph update, so it must see THIS
    // frame's transforms through getWorldAabbUpdated — ensureGiWalk, below.
    // Running it here instead made every mover stale the probes a frame late
    // and took gi.dynamic_probes, gi.budget and gi.probe_inputs with it.
    if (!shadow && mShadowScanPrimed) {
        // THE CACHE IS NOT RUNNING HERE (this scene has no shadow node to cache
        // into, or not one cacheable lamp): its caster records go stale, and so
        // do the departures waiting to be reported. Un-priming is what keeps
        // mShadowVanished from growing for a whole session of edits in a scene
        // whose shadows are off — and it costs nothing, because the next cached
        // frame is a FRESH slot assignment, which renders every map anyway.
        mShadowScanPrimed = false;
        mShadowVanished.clear();
    }
    if (!shadow) return;
    // THE STILL-FRAME GATE (ENGINE-4 F5), the caster half's version of
    // ensureGiWalk's. Everything this walk reads is either a TRANSFORM (the
    // world AABB) or a PUSHED event that moves nothing — a pose, a rebuilt
    // Item, a material's generated vertex piece, a per-object Cast Shadow flag,
    // a render-queue refile, a visibility or channel change. The first half is
    // the host's transform epoch; the second is why the GI half's gate could
    // not simply be copied, and is now counted at its own seams
    // (markShadowShapeDirty / noteShadowScanInput). With neither moved since
    // the last walk, nothing a lamp map depends on can have changed and the
    // walk is O(1) instead of O(items).
    //
    // TWO THINGS THE GATE MUST NOT SKIP, and neither is a transform:
    //   * a caster whose Item DIED (mShadowVanished) — unindexItemNode notes a
    //     scene transform write, so the epoch moves and the walk runs;
    //   * a caster whose material moves VERTICES every frame off the shader
    //     clock (a vertex-stage generated piece). Its lamps must re-render
    //     every frame, so it must be re-flagged every frame.
    //
    // THE SECOND ONE USED TO TAKE THE GATE OUT FOR THE WHOLE SCENE (audit
    // ON-16): `deforms` was a scene-wide bool, so ONE wind or wave material
    // anywhere meant every item in the scene was visited on every frame, at
    // rest, for ever — O(items) per frame, which is exactly the cost this gate
    // exists to remove. It is PER ITEM now: the full walk keeps a roster of the
    // deforming casters (mShadowDeformers, by node id — see its note for why it
    // cannot go stale while the gate holds), and a still frame re-flags those
    // nodes and nothing else.
    const unsigned long long epoch = shadowEpoch();
    if (mShadowScanPrimed && mShadowWalkEpochValid && epoch == mShadowWalkEpoch) {
        // The changes are per frame by contract (walkItems clears them at its
        // head), and "the walk ran and found nothing" is what the consumer
        // must see — collectShadowCacheFrame walks for itself when the frame's
        // walk did not happen, which would undo the whole gate.
        mShadowChanges.clear();
        // ...EXCEPT THE DEFORMERS, whose vertices have moved since the last
        // frame even though no transform was written. Their box and channels
        // are the ones the walk recorded, and they are still current for
        // exactly the reason the gate fired: nothing moved.
        for (NodeId id : mShadowDeformers) {
            auto it = mNodes.find(id);
            if (it == mNodes.end()) continue;              // gone; the epoch moved too
            const Node::ScanRec &r = it->second.scan;
            if (r.shadowPresent) mShadowChanges.push_back({ r.shadowBox, r.shadowChannels });
        }
        mShadowWalked = true;
        mCasterWalkMicros = 0.0;
        return;
    }
    mShadowWalkEpoch = epoch;
    mShadowWalkEpochValid = detail::gTransformWriteCounter != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    walkItems(false, true, false);
    mShadowWalked = true;
    mCasterWalkMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

// THE GI MOVEMENT SCAN, ONCE PER FRAME, ON DEMAND (the shape FIX WAVE B3 gave
// it): whoever consumes mGiMovedBoxes first this frame runs it, and it reads
// each item's world AABB UPDATED — the GI consumers run before the scene graph
// does, and a mover has to stale the probes in the frame it moves.
void OgreScene::ensureGiWalk() {
    if (mGiWalkedThisFrame) return;
    mGiWalkedThisFrame = true;
    // NOTHING WROTE A TRANSFORM SINCE THE LAST SCAN, so nothing can have moved
    // (clean-2 lane, 2026-09-13). This walk reads getWorldAabbUpdated() per
    // item — a parent-chain walk each — and it ran on every frame of every
    // probe-lit scene, still or not: 392 / 1961 / 4328 us at 1k / 5k / 10k
    // nodes, measured by lane R2. The epoch is the host's transform-write
    // counter plus our own writes; see transformEpoch().
    //
    // EVERYTHING ELSE THE PROBES CONSUME IS PUSHED, NOT SCANNED: an item
    // arriving, leaving, being hidden, shown, made a helper, re-materialled or
    // re-classified all call staleProbeGrid at their own seam (OgreScene.cpp,
    // OgreMaterials.cpp). The scan's unique job is movement, and movement is
    // exactly what the epoch reports.
    const unsigned long long epoch = transformEpoch();
    if (mGiScannedOnce && mGiWalkEpochValid && epoch == mGiWalkEpoch) {
        // The boxes are per frame by contract (walkItems clears them at its
        // head), so a skipped frame must publish "nothing moved" rather than
        // last frame's movers — otherwise a single move would stale the grid
        // for ever. The drag-gesture flag (MOVER-1) is per frame for the same
        // reason: a skipped walk is a frame in which nothing was written, which
        // is exactly what the settle is counting.
        mGiMovedBoxes.clear();
        mDragMoverMoved = false;
        mGiScanMicros = 0.0;
        return;
    }
    mGiWalkEpoch = epoch;
    mGiWalkEpochValid = detail::gTransformWriteCounter != nullptr;
    ++mGiScans;
    const auto t0 = std::chrono::steady_clock::now();
    walkItems(true, false, true);
    mGiScanMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

/// THE MOVEMENT EPOCH: the host's process-wide transform-write counter (the
/// document writes into the shared scene graph without telling the engine) plus
/// the writes the ENGINE itself makes to nodes of this scene — Scene::
/// setNodeTransform, a socket rider's per-frame placement, a decal's projector
/// box. Both halves are counts of WRITES, so writing the same value again reads
/// as movement and costs one extra scan; that is the safe direction.
unsigned long long OgreScene::transformEpoch() const {
    const unsigned long long host =
        detail::gTransformWriteCounter
            ? detail::gTransformWriteCounter->load(std::memory_order_relaxed)
            : 0ull;
    return host + mSceneTransformWrites;
}

void OgreScene::indexItemNode(Node &n) {
    noteSceneTransformWrite();      // an Item arriving is an input to every scan
    if (n.itemSlot != size_t(-1)) return;
    n.itemSlot = mItemNodes.size();
    mItemNodes.push_back(&n);
    markGpuSlotDirty(n);            // a newborn slot has no entry in the table yet
}

void OgreScene::indexDecalNode(Node &n) {
    if (n.decalSlot != size_t(-1)) return;
    n.decalSlot = mDecalNodes.size();
    mDecalNodes.push_back(&n);
}

void OgreScene::unindexDecalNode(Node &n) {
    n.scan.decalKnown = false;
    if (n.decalSlot == size_t(-1)) return;
    const size_t i = n.decalSlot;
    Node *last = mDecalNodes.back();
    mDecalNodes[i] = last;
    last->decalSlot = i;
    mDecalNodes.pop_back();
    n.decalSlot = size_t(-1);
}

void OgreScene::unindexItemNode(Node &n) {
    noteSceneTransformWrite();      // ...and so is one leaving
    // A CASTER LEAVING: its last box is what the lamps around it must re-render
    // without it (the scan never sees a node that has no Item).
    if (n.scan.shadowPresent && mShadowScanPrimed)
        mShadowVanished.push_back({ n.scan.shadowBox, n.scan.shadowChannels });
    n.scan.shadowPresent = false;
    n.scan.shadowItem = nullptr;
    if (n.itemSlot == size_t(-1)) return;
    const size_t i = n.itemSlot;
    Node *last = mItemNodes.back();
    mItemNodes[i] = last;
    last->itemSlot = i;
    mItemNodes.pop_back();
    n.itemSlot = size_t(-1);
    // THE GPU SCENE'S HALF OF THE SWAP-REMOVE, in the one place the swap
    // happens. TWO CASES, and the second is the one that bites: when the dying
    // item was NOT the tail, the tail's entry travels into the freed slot and
    // that slot is re-composed this frame; when it WAS the tail, nothing moves
    // in and the entry it leaves must be CLEARED — otherwise the next attach
    // lands in a slot that is still "born" and inherits the dead object's world
    // as its PREVIOUS world, i.e. a newborn reprojecting out of another
    // object's grave. Either way the freed entry is copied to the device, so the
    // region past `slotCount()` never holds a traceable ghost.
    if (mGpuScene.live()) {
        if (last != &n) {
            mGpuScene.onSlotMoved(uint32_t(mItemNodes.size()), uint32_t(i));
            mGpuForced.push_back(uint32_t(i));
        } else {
            mGpuScene.onSlotFreed(uint32_t(i));
        }
        // THE RAY LEVEL IS KEYED BY SLOT and the slots have just been
        // renumbered (ATOM P3's AT-A8r), so both ends of the swap are cleared:
        // whatever lands in either is evaluated afresh instead of inheriting
        // the distance band of the object that used to be there. (The band
        // cannot travel with the object: nothing here indexes by object.)
        forgetRayLevel(uint32_t(i));
        forgetRayLevel(uint32_t(mItemNodes.size()));
    }
    mGpuScene.setSlotCount(uint32_t(mItemNodes.size()));
}

// THE ITEM WALK — the GI movement scan (FIX WAVE B3, engine half) and the
// lamp-map cache's caster scan (ENGINE_CACHE_POLICY_SPEC P3), in one pass over
// the nodes that HAVE an Item (mItemNodes), with each node's last-seen state on
// the node itself. (It used to be two walks of the whole std::map, each with a
// hash lookup per item: measured 1.4 + 2.3 ms at 5k nodes in Debug+ASan.)
//
// GI half: records which GI items MOVED since the last scan, as the union of
// each mover's old and new world AABB. One consumer, a cheap path: the probe
// round-robin (a probe whose parallax shape contains a moved box goes to the
// front of the sweep). QUANTIZED to a 64th of
// the lit volume (giAabbMoved): sub-voxel jitter must not read as movement for
// ever. An item seen for the FIRST time is recorded, not reported — appearing
// is not moving, and the arrival is already in the signature.
//
// Shadow half: see collectShadowCacheFrame — a change is the box the lamps
// around it must re-render (old and new for a move, the new one for an arrival
// or a shape/pose change, the old one for a departure), with the channels the
// caster renders into.
void OgreScene::walkItems(bool gi, bool shadow, bool fresh) {
    if (!gi && !shadow) return;
    bool firstGi = false;
    if (gi) {
        mGiMovedBoxes.clear();
        mDragMoverMoved = false;       // per frame, like the boxes (MOVER-1)
        firstGi = !mGiScannedOnce;
        mGiScannedOnce = true;
    }
    bool firstShadow = false;
    Ogre::uint32 channelsAll = 0u;
    // Materials with a vertex-stage piece (usually NONE). A scratch member, not
    // a local: this walk runs every frame (clean-2 lane).
    std::vector<MaterialId> &deforming = mScanDeforming;
    deforming.clear();
    if (shadow) {
        mShadowChanges.clear();
        firstShadow = !mShadowScanPrimed;
        mShadowScanPrimed = true;
        channelsAll = allShadowCasterChannels();
        for (const auto &mk : mMaterials)
            if (!mk.second.customPiece[1].empty()) deforming.push_back(mk.first);
        // The per-item gate's roster, rebuilt by this walk (ON-16): which
        // CASTERS deform, not which materials do. Cleared here and refilled
        // below, so it describes exactly the state this walk saw.
        mShadowDeformers.clear();
    }
    for (Node *np : mItemNodes) {
        Node &n = *np;
        Ogre::Item *item = n.item;
        if (!item) continue;
        // THE BOX. `fresh` (the GI scan, which runs BEFORE the frame's scene
        // graph update) has to see THIS frame's transforms and pays
        // getWorldAabbUpdated — a parent-chain walk per item, ~400 ns in Debug.
        // The caster walk runs after updateSceneGraph and reads the cached
        // world AABB. (Ogre's own "is this node's transform dirty" flag, which
        // would let the fresh walk skip the update where nothing moved, exists
        // only in OGRE_DEBUG_MEDIUM builds — an engine-side movement epoch is
        // the way to cut this, and it is a lane of its own.)
        if (fresh) ++mGiAabbReads;
        const Ogre::Aabb a = fresh ? item->getWorldAabbUpdated() : item->getWorldAabb();
        // Read once per item, both halves use them (each is an SoA read, and
        // this loop is the per-frame cost the whole walk is measured by).
        const Ogre::uint32 flags = item->getVisibilityFlags();
        const Ogre::uint8 rq = item->getRenderQueueGroup();
        // A SHARED skeleton poses through its source (shareSkeleton): an
        // armour piece deforms when the body's clip moves.
        unsigned long long pose = n.poseEpoch;
        if (n.shareSource) {
            auto sit = mNodes.find(n.shareSource);
            if (sit != mNodes.end()) pose = pose * 1000003ull + sit->second.poseEpoch;
        }
        // A VERTEX-STAGE generated piece can move vertices every frame (it
        // reads the shader clock) — risk 6: such a caster keeps its lamps
        // re-rendering, the way Unreal excludes WPO materials from caching.
        const bool deforms = !deforming.empty() &&
            std::find(deforming.begin(), deforming.end(), n.materialRef) != deforming.end();
        // (NO SHARED "unchanged since the last walk" SHORTCUT. The two halves
        // run in different walks reading the box two different ways — updated
        // for GI, cached for the casters — and the two reads are not
        // bit-identical, so one shared snapshot thrashed and BOTH halves took
        // the long path every frame. Each half's own record is the comparison,
        // and each is a handful of float compares.)
        if (gi) {
            // A PROMOTED DRAG MOVER FIRST (MOVER-1). It carries kMovableBit and
            // no kGiGeometryBit, so without this branch it would fall into the
            // probe-only case below and stale the probe grid on every frame of
            // the gesture — the exact cost the promotion removes. Its motion is
            // nobody's input but the GESTURE'S OWN CLOCK: it is out of the
            // voxel bounce, out of the probe captures and out of the cascade
            // dirty list, and its box is tracked only so the settle knows when
            // the drag stopped and what to re-voxelise once at the end.
            if (n.dragMover) {
                if (giAabbMoved(n.scan.giBox, a)) {
                    n.scan.giBox = a;
                    n.scan.giMovedTick = mGiTick;
                    mDragMoverMoved = true;
                }
            } else if (!(flags & kGiGeometryBit)) {
                // PROBE-ONLY geometry (P7): unlit, captured by the probe faces,
                // never voxelized. Same quantized test, its own record, and a
                // flag rather than a moved box — the voxel side must not hear of it.
                // (probeSeesItem, inlined on the values read above.)
                if (n.shown && (flags & kVisibleBit) && rq <= kProbeFaceRqLast) {
                    if (!n.scan.probeKnown) {
                        n.scan.probeKnown = true;
                        n.scan.probeBox = a;
                        if (!firstGi) mProbeOnlyChanged = true;
                    } else if (giAabbMoved(n.scan.probeBox, a)) {
                        n.scan.probeBox = a;
                        mProbeOnlyChanged = true;
                    }
                }
            } else if (!n.scan.giKnown) {
                n.scan.giKnown = true;
                n.scan.giBox = a;
                // An ARRIVAL, after the first scan (which sees every item for
                // the first time and is not news): the probes must capture it (P1).
                if (!firstGi) mGiItemsAppeared = true;
                // ...AND THE CHAIN HAS TO GROW INTO IT (PHOTON_SPEC E2 (2)).
                //
                // The single-volume arm answers an arrival inside the reuse arm
                // (`refreshVctFast`'s "items born since the build" loop), and
                // that path REFUSES under a chain by construction — so a NEW GI
                // item that arrived after the chain was built cast no bounce at
                // all under cascades, silently and for ever: nothing else ever
                // re-selects a cascade's item set. (NEW is what this branch
                // sees: `scan.giKnown` is set the first time the walk meets a
                // node and is never cleared, so an item that leaves the channel
                // and comes back is not a second arrival — that edge goes
                // through `invalidateGiCaches` like every other channel change.)
                //
                // The dirty box is what gives the cascades that can actually SEE
                // the newcomer a rebuild to take (no attach set to re-select: the
                // gather reads membership from the GPU scene at every build). The
                // scheduler still spends at most one cascade per frame.
                if (!firstGi && !mVctCascades.empty()) noteGiCascadeDirty(&a);
            } else if (giAabbMoved(n.scan.giBox, a)) {
                Ogre::Aabb moved = n.scan.giBox;
                moved.merge(a);
                // IS THIS A GESTURE? (MOVER-1, and the probe deferral's rule
                // applied to the voxels.) A SECOND move inside the settle
                // window is a drag; one move on its own is an edit — a scripted
                // setPosition, a nudge, a paste — and an edit must be answered
                // where it happens, which is what the branch below does. The
                // window is measured in the scene's GI TICKS rather than in
                // consecutive frames, because the editor renders about two
                // frames per document edit and a dragged object's boxes
                // therefore arrive on alternate frames (DRAG-1's measurement,
                // which is why a consecutive-frames rule never fired).
                const bool second = n.scan.giMovedTick &&
                                    mGiTick - n.scan.giMovedTick <= kProbeMotionSettleFrames;
                n.scan.giMovedTick = mGiTick;
                if (mGi.dragMoverChannel && second && !mVctCascades.empty() && !n.movable) {
                    // THE PROMOTION. The object leaves the still world for the
                    // length of the gesture: one re-voxelisation HERE clears
                    // the copy of it the cascades hold at its old pose (the
                    // union box, so the place it came from and the place it is
                    // now are both re-voxelised), and from this frame on its
                    // motion costs the voxels nothing at all. What it gives up
                    // is stated rather than hidden: it stops bouncing light
                    // into the room and the room stops storing its shadow,
                    // exactly as a Movable object does — it is LIT by the field
                    // and the cones at its live pose, which is what makes the
                    // drag read correctly instead of through a half-rebuilt
                    // volume.
                    //
                    // THE PROBE GRID IS DELIBERATELY NOT STALED HERE. During a
                    // gesture the probes hold the photograph they hold today
                    // (the capture is deferred to the settle either way,
                    // DRAG-1), and the object's DEPARTURE from the capture set
                    // is answered once, at the demotion, with reason Mobility.
                    n.scan.giBox = a;
                    n.dragMover = true;
                    mDragMovers.push_back(n.selfId);
                    mDragMoverMoved = true;
                    applyNodeVisibilityFlags(n);
                    noteGiCascadeDirty(&moved);
                } else {
                    mGiMovedBoxes.push_back(moved);
                    // ...AND THE CASCADE CHAIN'S OWN RECORD (G1), which unlike
                    // `mGiMovedBoxes` (per frame, by contract) accumulates until the
                    // host's settle asks the chain to answer for it: the frame a
                    // drag is released is not the frame the box moved.
                    if (!mVctCascades.empty()) noteGiCascadeDirty(&moved);
                    n.scan.giBox = a;
                }
            }
        }
        if (shadow) {
            ++mCasterWalkItems;
            // THE CASTER PREDICATE. A helper carries kHelperBit and a distortion
            // item kDistortionBit — neither is in a shadow channel. The on-top
            // overlay queue (gizmos, bone overlays: unlit, depth test off,
            // kVisibleBit — lighting audit L6.3) writes no depth even where a
            // shadow pass draws it, so it is never a caster either.
            const Ogre::uint32 channels = flags & channelsAll;
            const bool present = channels != 0u && n.shown && rq < kOverlayRenderQueue &&
                                 item->getCastShadows();
            Node::ScanRec &r = n.scan;
            if (!firstShadow) {
                if (present && !r.shadowPresent) {
                    mShadowChanges.push_back({ a, channels });
                } else if (!present && r.shadowPresent) {
                    mShadowChanges.push_back({ r.shadowBox, r.shadowChannels });
                } else if (present) {
                    if (r.shadowItem != item || boxMovedForShadows(r.shadowBox, a) ||
                        r.shadowChannels != channels) {
                        Ogre::Aabb both = r.shadowBox; both.merge(a);
                        mShadowChanges.push_back({ both, channels | r.shadowChannels });
                    } else if (r.shadowPose != pose || deforms || n.shadowShapeDirty) {
                        mShadowChanges.push_back({ a, channels });
                    }
                }
            }
            // THE ROSTER (ON-16): a present caster whose material deforms owes
            // a change on every frame, gate or no gate.
            if (deforms && present) mShadowDeformers.push_back(n.selfId);
            n.shadowShapeDirty = false;
            r.shadowPresent = present;
            r.shadowItem = present ? item : nullptr;
            r.shadowBox = present ? a : Ogre::Aabb();
            r.shadowPose = pose;
            r.shadowChannels = channels;
        }
    }
    if (gi) {
        // THE DECALS (clean-2 lane, 2026-09-13). A decal is not an Item and its
        // node usually carries none, so the loop above cannot see it — yet the
        // probe faces render decals like any other Forward+ surface, which
        // makes a decal that MOVES a probe input exactly as a moved crate is.
        // (Arrival, removal and edits stale the grid at their own seam, in
        // setDecal/removeDecal; this half is the one that cannot be pushed.)
        // Its box is `probeOnly`: a decal paints a surface the probes capture,
        // and the voxels never see one.
        for (Node *np : mDecalNodes) {
            Node &n = *np;
            if (!n.decal || !n.decalNode) continue;
            const Ogre::Aabb a = n.decal->getWorldAabbUpdated();
            if (!n.scan.decalKnown) {
                n.scan.decalKnown = true;
                n.scan.decalBox = a;
                continue;                       // arrival: setDecal staled it already
            }
            if (giAabbMoved(n.scan.decalBox, a)) {
                n.scan.decalBox = a;
                mProbeOnlyChanged = true;
            }
        }
    }
    if (shadow) {
        if (!firstShadow)
            mShadowChanges.insert(mShadowChanges.end(), mShadowVanished.begin(), mShadowVanished.end());
        mShadowVanished.clear();
    }
}

// THE PROBE UPDATE BUDGET (FIX WAVE B2) — what replaced P5a's "keep the nearest
// N probes live for ever".
//
// The budget is a RATE: `GiParams::updateBudget` probe re-captures per frame,
// spent on the probes that need them most. Three things make that work, and all
// three are corrections to the shipped dynamic-probe design:
//
//  1. mDirty ONLY, never setStatic. Upstream's collect rule is
//     `( areaLS.contains( trackedPos ) && !mStatic ) || mDirty`
//     (OgreParallaxCorrectedCubemapAuto.cpp:328-346), so raising the PUBLIC
//     mDirty field is sufficient on its own — the same field
//     updateAllDirtyProbes uses, cleared by updateRender after the capture.
//     Flipping mStatic was not: `setStatic` runs `switchInternalProbeStaticValue`,
//     which destroys and recreates the probe's INTERNAL scene node in the other
//     memory manager (F9). Probes therefore stay static for their whole lives
//     and nothing reparents anything.
//
//  2. mNumIterations = 1 on EVERY probe (set once, in buildPcc). In automatic
//     mode it is not an iteration count: above 1 a dirty probe renders in
//     `updateExpensiveCollectedDirtyProbes`, which wraps EACH probe in its own
//     `_beginFrameOnce`/`_endFrameOnce`; at 1 it renders inline in `updateRender`
//     with the frame's other workspaces. A budgeted probe wants the second.
//
//  3. A STALE SET, so the budget is a guarantee and not a heuristic — and so a
//     still scene costs NOTHING (ENGINE_CACHE_POLICY_SPEC P1, 2026-09-12). Every
//     probe carries "stale: owes a capture". Inputs mark it (staleProbeGrid,
//     with a reason — the invalidation table, spec §3: a GI rebuild or re-solve,
//     geometry moving or appearing, a light or material or sky or ambient or fog
//     change, time-varying content); the budget spends itself on stale probes
//     only, and with nothing stale it spends nothing. So every probe re-captures
//     within ceil(probes / budget) frames OF A CHANGE, whatever the priority
//     prefers — the property gi.budget pins — and the priority (staleness x
//     proximity x covers-a-moved-AABB) only decides the ORDER of the catch-up:
//     the probes the viewer is looking at and the ones a moving object is
//     inside come first.
//
//     WHAT IT REPLACED: the sweep used to REFILL itself whenever it emptied, so
//     one probe re-captured every frame of a still scene, for ever — 131 draws
//     and 19.3 ms a frame on the Showroom (spec §1), for a picture that could
//     not change. That refill was also, silently, the only thing that ever
//     brought reflections round after a material, sky or light-colour edit —
//     nothing invalidated a probe on any of them — which is why the refill and
//     the missing invalidations (P7) went out together.
//
//     v1 STALES THE WHOLE GRID per input, a mover included: indoors every
//     parallax shape contains everything (the A2 clamp), so area or shape
//     locality would discriminate nothing; v2 can add it for open fields.
//
// Called once per scene per frame, from the AUTHORITATIVE view only (F7): the
// engine picks one on-screen view per scene in renderOneFrame, because two views
// sharing a scene used to drive the tracker twice a frame with two different
// camera positions — which is both double the probe work and a priority that
// depends on view order.
void OgreScene::updateProbeBudget(const Ogre::Vector3 &camPos) {
    const Ogre::CubemapProbeVec &probes = mPcc->getProbes();
    const size_t n = probes.size();
    if (mProbeSlots.size() != n) mProbeSlots.assign(n, ProbeSlot());
    const int budget = std::max(0, mGi.updateBudget);
    mProbeUpdatesPerFrame = std::min(budget, int(n));
    if (!n || !budget) return;          // paused: nothing dirtied, nothing scanned

    ensureGiWalk();

    // THE INPUTS THIS PASS CAN SEE FOR ITSELF (the rest arrive through the
    // setters: setLight, setPbrMaterial, setSky, attach/detach/visibility ...).
    // Geometry the probes capture that moved or arrived since the last scan —
    // GI geometry and unlit geometry alike — is out of date in every probe
    // that can see it, which in v1 is all of them.
    // WHAT MOVED THIS FRAME, CAPTURED BEFORE THE FLAGS ARE CLEARED (DRAG-1
    // round 2, F3). `mGiMovedBoxes` holds GI geometry only; a PROBE-ONLY item —
    // unlit geometry the probe faces capture but the voxels do not, an image
    // plane, a backdrop card — reports through `mProbeOnlyChanged` instead, and
    // it stales the grid exactly the same way. Reading the boxes alone made a
    // dragged image plane look like a STILL scene to the gesture rule below, so
    // it spent a capture on every frame of the drag, which is the whole defect.
    const bool movedThisFrame = !mGiMovedBoxes.empty() || mProbeOnlyChanged;
    if (movedThisFrame || mGiItemsAppeared)
        staleProbeGrid(GiStaleReason::Moved);
    // (An ARRIVAL is not motion and does not arm the gesture: a thing that
    // appears once is an event the probes should answer. It is still deferred
    // while a gesture is running — it arrived into one — and the ceiling below
    // bounds how long that can last.)
    mGiItemsAppeared = mProbeOnlyChanged = false;

    // ---- THE MOTION DEFERRAL (DRAG-1, REFLECT F3) -------------------------
    // A PHOTOGRAPH OF A MOVING BOX IS OUT OF DATE BEFORE IT IS DISPLAYED, and
    // this frame's move stales it again on the next frame: during a drag the
    // budget bought one 512-square six-face HDR capture per frame — each with
    // `shadows JahshakaProbeShadowNode recalculate` on every face, so both of
    // the Mirror Room's point lamps re-rendered their probe-kind cubes six
    // times over — and every one of those captures was invalidated by the
    // frame after it. The work is not merely wasted: it is the bulk of the
    // measured 5-6 fps while the teapot is dragged.
    //
    // So the SPEND waits for the content to hold still. What does NOT wait is
    // the RECORD: `staleProbeGrid` above has already marked every slot, and
    // `framesSinceUpdate` keeps counting below, so the sweep guarantee keeps
    // its shape — every stale probe is re-captured within ceil(probes/budget)
    // frames OF THE CONTENT COMING TO REST, instead of within that many frames
    // of a move that has not finished happening. The picture the user is left
    // looking at when the drag ends is the same picture; only the frames
    // during the drag differ, and during the drag the probes were showing a
    // photograph of somewhere the object no longer is either way.
    //
    // THIS IS THE SHIPPED POLICY, NOT A NEW ONE. The probes are the STATIC
    // environment layer — the comment above says so for time-varying content
    // (a posing rig, a particle system, a clock-driven material): SSR, the
    // planar mirrors and the ray arm are what show a moving thing live. A box
    // that is still moving is exactly that case.
    //
    // WHAT IS NOT DEFERRED: staleness from any other input
    // (`mProbeStaleBeyondMotion` — a light, a material, the sky, an explicit
    // refresh). A lamp switched on during a drag reaches the probes in the
    // frame it is switched on.
    // A SINGLE MOVE IS NOT A GESTURE. A scripted setPosition, a nudge or a
    // paste has to reach the probes in the frame it happens: nothing can know
    // at the first move whether a second is coming, and one capture is the
    // honest price of finding out. So the deferral arms on the SECOND move,
    // and lifts after kProbeMotionSettleFrames frames with none — a drag costs
    // exactly that one wasted photograph instead of one a frame.
    //
    // "SECOND" IS WITHIN THE SETTLE WINDOW AND NOT ON THE NEXT FRAME, and that
    // is measured rather than cautious: the editor renders about TWO frames per
    // document edit during a drag (SMOKE-41: 170 monitor frames for 90 scripted
    // steps), so the moved boxes arrive on alternate frames and a rule that
    // wanted two CONSECUTIVE moving frames never fired at all — 59 captures
    // over a 60-frame drag, i.e. exactly the behaviour it was replacing.
    if (!movedThisFrame) {
        if (++mProbeMotionQuietFrames >= kProbeMotionSettleFrames) {
            mProbeDragActive = false;
            mProbeMotionRun = 0;
            mProbeDeferredRun = 0;      // a new gesture starts its ceiling fresh
        }
    } else {
        if (mProbeMotionRun) mProbeDragActive = true;   // a second move, still inside the window
        mProbeMotionRun = 1;
        mProbeMotionQuietFrames = 0;
    }
    // THE DIAGNOSTIC THE MEASUREMENT DRIVES: this deferral is a claim about cost and about
    // the picture at the drag's end, and both claims have to be checkable from
    // outside against the behaviour it replaced, in ONE binary at one pose.
    // Read per frame rather than cached because a test arms it between frames,
    // and a getenv against a path that may issue a 512-square six-face capture
    // is not a cost anyone can measure.
    const bool deferMotion = std::getenv("JAHSHAKA_PROBE_NO_MOTION_DEFER") == nullptr;
    bool deferring = deferMotion && mProbeDragActive && !mProbeStaleBeyondMotion;
    if (deferring) {
        // ...WITH A CEILING (DRAG-1 round 2, F4). A drag ends and a keyframed
        // object in play does not: without this, a motion that never stops is
        // one endless gesture and the probes would show the pre-motion room for
        // as long as it lasts. One capture every kProbeDeferredCaptureEvery
        // frames keeps a long motion live at a bounded price — see the constant
        // for the arithmetic.
        if (++mProbeDeferredRun >= kProbeDeferredCaptureEvery) {
            mProbeDeferredRun = 0;
            deferring = false;
        }
    }
    if (deferring) {
        // The queue still ages while it waits, so the order the settle spends
        // in is the order the wait earned.
        for (ProbeSlot &sl : mProbeSlots) ++sl.framesSinceUpdate;
        ++mProbeCapturesDeferred;
        return;
    }
    // TIME-VARYING CONTENT IS FROZEN (REALTIME_REFLECTIONS_SPEC O4 = A, lead
    // decision 2026-09-12): a posing rig, a particle system, a clock-driven
    // material or a live texture stales nothing by itself. SSR and planar
    // reflections show it live; the probes are the static-environment layer.

    // THE CATCH-UP RATE (spec D2) IS THE BUDGET (option A, the shipped answer:
    // no hitch, progressive — a 32-probe grid catches up in about half a second
    // at 60 Hz). Option B, a separate faster rate while probes are stale, was a
    // dead constant at 0 with a branch nobody could reach; raising the rate now
    // means raising the tier's budget, which is the same number the status
    // reports (clean-2 lane, 2026-09-13).

    // How much a probe covering something that just moved may jump the queue.
    // It cannot break the sweep guarantee (it only reorders within one), so the
    // value is about responsiveness, not correctness.
    static const float kMovedBoost = 8.0f;
    const float diag = std::max(mGiProbeRegion.getSize().length(), 1e-3f);

    std::vector<std::pair<float, size_t>> ranked;   // (-score, index): sorts ascending
    ranked.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        ++mProbeSlots[i].framesSinceUpdate;
        if (!mProbeSlots[i].sweepPending) continue;
        const float d = (probes[i]->getArea().mCenter - camPos).length();
        const float proximity = 1.0f / (1.0f + d / diag);
        // The probe's AREA, not its SHAPE. The shape is the parallax box, and
        // since A2 clamps every one of those into the probe region they all
        // contain everything — a "covers what moved" test against them would be
        // true for every probe and would discriminate nothing (measured: the
        // budget-1 sweep picked a probe on the far side of the room while the
        // mover sat in front of the mirror). The AREA is the probe's share of
        // the region, which is exactly "the space this probe is responsible
        // for", and it is per probe.
        bool covers = false;
        for (const Ogre::Aabb &b : mGiMovedBoxes)
            if (probes[i]->getArea().intersects(b)) { covers = true; break; }
        const float score = float(mProbeSlots[i].framesSinceUpdate) * proximity *
                            (covers ? kMovedBoost : 1.0f);
        ranked.emplace_back(-score, i);
    }
    const size_t take = std::min(size_t(budget), ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + std::ptrdiff_t(take), ranked.end());
    // The non-motion debt is paid once the sweep it armed has drained: this
    // frame spends `take` of `ranked.size()` pending slots, so the last of them
    // is the frame the flag goes down (and a new non-motion input raises it
    // again through staleProbeGrid).
    if (take >= ranked.size()) mProbeStaleBeyondMotion = false;
    for (size_t k = 0; k < take; ++k) {
        const size_t i = ranked[k].second;
        probes[i]->mDirty = true;
        mProbeSlots[i].sweepPending = false;
        mProbeSlots[i].framesSinceUpdate = 0;
        // The slot keeps its reason until the capture is COUNTED
        // (latchProbeCaptures), which is where the monitor reads it.
        if (monitor::live())
            mProbeDirtiedThisFrame.emplace_back(unsigned(i), mProbeSlots[i].staleReason);
    }

    // (THE DYNAMIC-PROBE RESERVATION LIVED HERE and is DELETED, lane R2.) It
    // spent up to `GiParams::dynamicProbes` extra captures per frame on the
    // probes a MOVING object was inside, so that its reflection followed it
    // frame by frame. Movers are no longer in a probe capture at all
    // (kMovableBit): the reflection of a moving thing is the per-frame layers'
    // job — SSR and the planar mirrors — and re-photographing the room because
    // something walked through it is exactly the cost this program removes
    // (measured on the alive scene at Epic, where the column was the last
    // ~3 ms of per-frame probe work). What is left is the sweep above: a
    // guarantee about every STALE probe, spent at the tier's budget.
}

// THE MOVEMENT TERM OF THE GI SIGNATURE (FIX WAVE B3, mirror half).
//
// A quantized hash of every GI item's world AABB — the same shape as the light
// signature the mirror already debounces on, and deliberately STATELESS so that
// calling it cannot disturb the movement scan above (the two ran into each other
// in an earlier draft: whichever consumer looked first consumed the movement).
//
// What it buys: geometry that moves now ARMS the mirror's stability window like
// a dragged light does, so the CHEAP paths run during the drag — the probes the
// object is inside re-capture on their next turn in the sweep, and the lights are
// re-injected into the voxels every kGiLightOnlyEveryN frames — and exactly ONE
// full re-solve lands when the movement stops. Before this, moving an object
// changed nothing at all until somebody pressed Refresh.
//
// Quantized to a 64th of the ITEM'S OWN largest extent, for giEscapeSignature's
// reason: a settling physics body or an idle sway must not hold the mirror's
// stability window open for ever.
//
// PER ITEM, not per lit volume, and that is a correction rather than a taste:
// quantizing against the lit volume makes the signature change when the VOLUME
// changes, and the volume changes the moment GI first builds — so an idle scene
// spent one spurious re-solve immediately after arming (caught by gi.coalesce's
// "20 idle frames cost nothing at all"). An item's own size is a property of the
// item, so the hash of a still scene is still whatever the volume does.
unsigned long long OgreScene::giGeometrySignature() const {
    if (mGi.mode == GiMode::Off) return 0ull;
    // CACHED AGAINST THE MOVEMENT EPOCH, exactly as giEscapeSignature is and
    // for the same reason: the mirror reads it once a frame and it is a pure
    // function of the GI items' boxes (clean-2 lane, 2026-09-13).
    // Only where the epoch can see the host's writes — see giEscapeSignature.
    const unsigned long long epoch = transformEpoch();
    if (detail::gTransformWriteCounter &&
        mGeomSigValid && mGeomSigEpoch == epoch && mGeomSigMode == mGi.mode) return mGeomSig;
    computeGiSignatures();
    return mGeomSig;
}

// THE MATERIAL TERM (ENGINE_CACHE_POLICY_SPEC P7), deliberately NOT folded into
// the geometry signature above: the host arms the same one-re-solve-on-settle
// debounce for it but skips the light re-inject cadence that geometry and
// lights get, because nothing a re-inject reads changed (the voxels still hold
// the old conversion until the re-solve builds fresh ones).
unsigned long long OgreScene::giMaterialSignature() const {
    return mGi.mode == GiMode::Off ? 0ull : mGiMaterialGeneration;
}

// The movement quantum for one item, and the test that uses it. Shared by the
// signature above and the engine-side movement scan so the two can never
// disagree about what counts as having moved.
float OgreScene::giAabbQuantum(const Ogre::Aabb &a) {
    const Ogre::Vector3 s = a.getSize();
    return std::max(std::max(std::max(s.x, s.y), s.z) / 64.0f, 1e-4f);
}

bool OgreScene::giAabbMoved(const Ogre::Aabb &before, const Ogre::Aabb &after) {
    const float quantum = std::max(giAabbQuantum(before), giAabbQuantum(after));
    const Ogre::Vector3 dc = after.mCenter - before.mCenter;
    const Ogre::Vector3 dh = after.mHalfSize - before.mHalfSize;
    for (size_t ax = 0; ax < 3u; ++ax)
        if (std::fabs(dc[ax]) >= quantum || std::fabs(dh[ax]) >= quantum) return true;
    return false;
}

// VCT light injection ray-marches towards each light to work out what is
// shadowed, and `rayMarchStepScale` is how coarsely (FIX WAVE B5). Upstream:
// bigger is faster and starts losing shadows; below 1.0 trips an assert.
//
// The document's value, default 1.0, on EVERY injection: the moving tick used
// to raise it to 2 (and drop its bounces), which handed a volume a different
// answer from the rebuild beside it — the one writer (PHOTON-WRITER-1) has one.
float OgreScene::giRayMarchStepScale() const {
    return std::max(1.0f, mGi.rayMarchStepScale);
}


// TRUE WHEN THE ARM WAS ACTUALLY (RE)BUILT, and that is a contract a caller
// depends on (the lead's fix-round item 1): `applyPendingGi` clears
// `mGiChainShapeDirty` — the VR column's debt, which no dirty BOX can express —
// only when a build really happened, because this function has three ways to
// come back having built nothing (no camera yet, a voxel input still streaming,
// nothing to voxelise). It used to clear the flag before calling, so a shape
// rebuild that deferred left the chain in the old column with nothing left to
// re-arm it: the next flush took the cheap `refreshCascadesFast` branch on the
// live chain and the headset kept the desktop's cascade set. The two paths that
// return false AFTER tearing the arm down need no flag: `teardownVct` clears the
// shape debt itself, and every teardown is followed by a build from the current
// table.
bool OgreScene::rebuildVct() {
    // THE PHOTON ARM (GiParams::cascades): camera-centred cascades instead of
    // one scene-fitted box. The scene's own bounds are still computed — the
    // probe half below is placed in the room, not around the camera — but the
    // voxel volumes come from the cascade table and the tracked camera.
    const bool cascadeArm = mGi.cascades;

    // A CAMERA-CENTRED ARM NEEDS A CAMERA, AND WAITS FOR ONE (audit B3).
    // `mGiCamPos` is the authoritative view's last tracked position, and it is
    // the ORIGIN until the first frame has tracked one — while a scene open
    // pushes its GI before any frame renders. Building the chain there and
    // letting the first tracked frame discover that every cascade is a
    // teleport away cost N counted full rebuilds and voxelised the whole chain
    // TWICE on every open (measured: 8 rebuilds for 4 cascades, 4 of them
    // `cascadeFullRebuilds`; gi.cascades case 0). So the arm waits: nothing is
    // built, the request stays armed in `mGi`, and `updateGiTracking` asks for
    // the build on the frame a camera first appears — the same frame, because
    // the tracking update runs before `applyPendingGi` in `renderOneFrame`.
    // A scene that never gets a view never builds a camera-centred arm, which
    // is the honest answer rather than one built around a camera that does not
    // exist — and `giStatus().cascadesAwaitingCamera` says so, which is what
    // tells "no view yet" apart from "the build failed".
    //
    // BEFORE THE REBUILD IS COUNTED OR TIMED (round-2 review F4/F5): waiting is
    // not a rebuild. Counting it left a cascade open reporting `rebuilds` 2 for
    // one build and filed a zero-length `gi.rebuild` event in every capture.
    // The teardown still happens — a scene that had a single-volume arm and is
    // then switched to cascades before its first frame must not keep it bound.
    if (cascadeArm && !mGiCamPosKnown) {
        mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
        teardownVct();
        mGiCascadeAwaitingCamera = true;
        return false;                    // nothing was built; see the return contract
    }
    mGiCascadeAwaitingCamera = false;

    // AND IT WAITS FOR THE ALBEDO IT IS ABOUT TO VOXELISE (BOOTVOX-1), the same
    // shape as the camera wait above and for the same reason: building now
    // means building again. See giVoxelTexturesPending — the request stays
    // armed through `mGiCachesDirty` and the flush retries on the frame the
    // last voxel-input texture is resident. Whatever is bound stays bound
    // while it waits (no teardown): a frame of the previous arm is a better
    // answer than a frame of nothing.
    if (giVoxelTexturesPending()) { mGiCachesDirty = true; return false; }

    // ...AND FOR THE SKY IT WILL READ (PHOTON-ENV-1 audit F7). The bounce
    // injection reads the environment where its cones escape, so a first chain
    // built before its sky's capture and convolution have landed is built over
    // no sky, and the cube's arrival a frame later (noteEnvironmentChanged) owes
    // it a whole settle — on EVERY boot of a sky + bounce + chain scene
    // (measured by gi.chain_converge case 0: one settle per boot before this
    // wait, none after). Same shape as the albedo wait, the same bound; see
    // giEnvironmentPending.
    if (giEnvironmentPending()) { mGiCachesDirty = true; return false; }

    // ...AND IT WAITS FOR THE WORLD TO BE ON SCREEN (OPEN_COVER_SPEC §2 A).
    // The same shape as the two waits above and for a related reason: this is
    // the longest single thing the engine does on the UI thread (1,025 ms on
    // Grand Showroom 2), and doing it while a load is still installing spends
    // it on a frame nobody can see — the loading cover's, the open runner's
    // slice boundary, the shader warm-up's 4x4 target. Nothing is built, the
    // request stays armed through `mGiCachesDirty`, and the first frame after
    // the reveal takes it (one STAGE at a time when that frame says so).
    //
    // ONLY A FIRST arm: a scene that already has one keeps every cheap path
    // while the next world loads, and a rebuild of a LIVE arm is not the cost
    // this defers.
    //
    // IT IS HERE AND NOT AT THE FLUSH because `setGlobalIllumination` — which
    // is how the mirror arms a freshly installed world — calls this function
    // DIRECTLY, and a guard at `applyPendingGi` never saw it (measured: three
    // gate reds whose common cause was the arm building inside the create).
    if (mSceneLoading && mVctCascades.empty() && !mVctVoxelizer) {
        mGiCachesDirty = true;
        return false;
    }

    ++mGiRebuilds;
    // THE MONITOR'S GI EVENT (§4.7 / §4.8). A rebuild is the single most
    // expensive thing this engine does on the UI thread — measured in seconds
    // on a page return — so it is timed and tagged with the stale reason that
    // asked for it, or `None` when nothing recorded one.
    monitor::EventScope giEvent(MonitorEventKind::GiRebuild, monitor::reasonOf(mLastStaleReason),
                                "gi.rebuild", "vct");
    // Every early return below leaves "nothing built" showing in giStatus.
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    // ALWAYS from scratch - the arm's SHAPE (resolution, cascades, probe grid) is
    // decided here. (The reason this used to give - a voxeliser holding raw Item*
    // and a per-voxeliser material cache - is gone: the feed is the GPU scene and
    // the chain shares one store, refreshed in place and evicted on a death.)
    teardownVct();

    Ogre::Vector3 mn, mx;
    const bool haveBounds = computeGiBounds(mn, mx);
    if (!haveBounds && !cascadeArm)
        return false;                        // nothing to voxelize (yet); stay armed via mGi
    const Ogre::Aabb aabb = haveBounds ? Ogre::Aabb::newFromExtents(mn, mx)
                                       : Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);

    size_t itemCount = 0;
    {
        // The voxelisation and the first light injection: compute dispatches,
        // so the GPU half is a 0027 timestamp pair around them (the compositor
        // never sees this work and the frame's pass list cannot show it).
        monitor::CacheScope work(CacheKind::Gi, monitor::reasonOf(mLastStaleReason), 0,
                                 cascadeArm ? "vct.cascades.build" : "vct.rebuild",
                                 mRoot->getRenderSystem());
        itemCount = cascadeArm ? buildCascadeArm(mGiCamPos) : buildVoxelArm(aabb);
        work.setUnits(unsigned(itemCount));
        // NOTHING TO VOXELISE IS NOT WORK (ENGINE-5 review, ledger §208). An
        // empty scene, or one whose every item is excluded, left a units-0
        // `vct.rebuild` row in every capture — a cache row for a cache that was
        // not filled. The row is abandoned, not zeroed: the early return below
        // is the same decision.
        if (!itemCount) work.cancel();
    }
    if (!itemCount) { teardownVct(); return false; }   // stay armed; next churn re-flags

    hlmsPbs(mRoot)->setVctLighting(mVctLighting);
    sVctBindingOwner = this;
    // What this arm was built AT (B4): the reuse path re-runs it only while the
    // count still matches, i.e. while nothing it points into can have died.
    mGiBuiltGeneration = mGiDestroyGeneration;
    mGiReusedLastRefresh = false;

    // WHAT THE ARM LIT. For the cascade arm that is the OUTERMOST cascade's
    // box, which is what `giStatus().boundsMin/Max` and `voxelMetres` must
    // report: the scene's fitted box describes nothing the renderer built.
    mGiLitVolume = cascadeArm ? Ogre::Aabb(mVctCascades.back().centre,
                                           Ogre::Vector3(mVctCascades.back().halfSize))
                              : aabb;
    // THE PROBE REGION IS THE SCENE'S OWN FITTED BOX (R5-ROOM). It used to be a
    // separate, tighter box derived by measuring the scene's walls — and the
    // measuring was the rule that retired, so what is left is the one box the
    // renderer already fits to the content. The probes are spread through it and
    // each one then photographs its own surroundings; `buildPcc` keeps the ones
    // that saw something and drops the rest.
    if (mGi.mode == GiMode::VctPccHybrid && haveBounds) {
        // DELIBERATELY the scene's fitted box and not a cascade: where the
        // probes live is a property of the content, not of where the camera
        // stands. The cascade arm changes where the BOUNCE is computed and
        // nothing about where the probes live.
        mGiProbeRegion = aabb;
        // STAGED, WHEN THE FRAME ASKED FOR IT (OPEN_COVER_SPEC §2 A). The
        // cascade arm above is 57 ms and the probe grid below is 673 on Grand
        // Showroom 2, so this is where a streaming open parks: the world draws
        // with the chain it has, and the next three streaming frames spend one
        // probe stage each. `Complete` — every other caller — falls straight
        // through and builds the grid in this call, as it always did.
        if (mGiStageBuild) {
            mGiStagedVolume = aabb;
            mGiBuildStage = GiBuildStage::ProbeScout;
        } else {
            buildPcc(mGiProbeRegion);
            // The probe grid now owns the shader's one env-probe slot, so the
            // IBL cubemap must come OFF every datablock — see the long note at
            // OgreScene::reflectionTexForDatablocks (OgreSky.cpp). Unconditional:
            // applyReflectionToAll is a no-op walk when there is no sky
            // reflection.
            applyReflectionToAll();
        }
    }
    // LAST, not beside `mGiLitVolume = aabb` above: the hysteresis floor inside
    // giItemBounds must see the same record the lit volume was fitted against or
    // the two could disagree about which items exist.
    if (haveBounds) noteGiAutoVolume(aabb, !giBoundsExplicit());

    // The DDGI layer, over the volume this build just lit. After the VCT
    // binding (it takes the same process-wide ownership) and after the PCC
    // build (the field is diffuse-only; the probes keep the specular they had).
    // A no-op — including a teardown of any previous field — when the toggle is
    // off, which is what makes `rebuildVct` the single place the arm's shape is
    // decided.
    //
    // THE FIELD RIDES CASCADE 0 (PHOTON_SPEC E1). It needs no cascade-specific
    // code here at all: cascade 0 IS `mVctVoxelizer`/`mVctLighting` by the
    // chain's own rule 5, so `buildIrradianceField` fits its probes to the
    // voxel box the innermost cascade was just built over, exactly as it fits
    // them to the scene's box in the single-volume arm. What the cascade arm
    // owes on top of that is the FOLLOWING — `followCascade0Field`, called from
    // the scheduler whenever cascade 0 is re-placed or re-voxelised.
    //
    // (This is where the arm used to REFUSE the field and say so. The refusal
    // was correct at the pin it was written against: `initialize()` is
    // upstream's only placement API and it re-creates the atlases, so a field
    // on a scrolling cascade was either wrong or black. ogre-patch 0044 adds
    // the two hooks that were missing — `setFieldVolume` and `setVctLighting` —
    // and the refusal, and the measured cost it quoted (plain cone diffuse
    // leaking 0.97 of the light through a 0.5 m wall), are history.)
    // ...AND THE FIELD IS THE LAST STAGE, for the same reason (§2 A). When the
    // probe stages are staged it rides behind them; when the scene has no probe
    // grid at all it is a stage of its own, because a cold field is 45 ms of
    // compute-shader compile.
    if (mGiStageBuild) {
        if (mGiBuildStage == GiBuildStage::Idle) mGiBuildStage = GiBuildStage::Field;
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: arm staged — the chain is built, the probe grid and the "
                "irradiance field follow one frame at a time");
        return true;                     // the chain IS built; the stages owe the rest
    }
    const auto tField = std::chrono::steady_clock::now();
    buildIrradianceField();
    const double msField = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - tField).count();
    if (monitor::live())
        monitor::noteCacheWork(CacheKind::Gi, monitor::reasonOf(mLastStaleReason), 0,
                               "vct.field.build", 1u, float(msField));
    if (giDebug())
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: irradiance field built in " + std::to_string(msField) + " ms");

    if (giDebug())
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: voxelized " + std::to_string(itemCount) + " items at " +
            std::to_string(giVoxelResolution()) + "^3 over " + Ogre::StringConverter::toString(mn) +
            " .. " + Ogre::StringConverter::toString(mx) +
            (mPcc ? " (+PCC probe grid)" : ""));
    return true;                             // the arm was built, from the CURRENT table
}

// THE VOXEL ARM — the voxelizer and the lighting over `aabb`, from the live GI
// items. rebuildVct's first half. (freshVoxelArm, the material-edit re-solve that
// built a whole NEW voxeliser and lighting, is DELETED - A5b fix round: the shared
// store re-reads its rows in place and the reuse arm re-runs the same voxeliser.)
size_t OgreScene::buildVoxelArm(const Ogre::Aabb &aabb) {
    // A FROM-SCRATCH ARM RE-READS EVERY MATERIAL (the VCT lifecycle's own rule): the
    // store outlives rebuilds, so the refresh is owed and every GI slot is queued - a
    // slot composed while there was no store could not have queued itself.
    mVctMaterialRefreshOwed = true;
    queueAllGiMaterials();
    // Quality -> voxel volume resolution (the memory/compute knob: 32^3 =~ fast
    // preview, 128^3 =~ crisp indirect shadows) and anisotropic cone mips.
    const Ogre::uint32 res = giVoxelResolution();
    const bool anisotropic = mGi.quality != GiQuality::Low;

    // An arm over no GI geometry is not built (the caller stays armed).
    const size_t itemCount = countGiItems();
    if (!itemCount) return 0;

    // World transforms must be current before voxelization: the GPU scene's table
    // is brought current from them.
    mSceneMgr->updateSceneGraph();

    mVctVoxelizer = new Ogre::VctVoxelizer(
        Ogre::Id::generateNewId<Ogre::VctVoxelizer>(),
        mRoot->getRenderSystem(), mRoot->getHlmsManager(),
        true /*correctAreaLightShadows*/, vctMaterialStore());
    mVctVoxelizer->setResolution(res, res, res);
    mVctVoxelizer->setRegionToVoxelize(aabb);
    mVctVoxelizer->dividideOctants(1u, 1u, 1u);
    // THE SINGLE VOLUME IS A CASCADE OF ONE (A5b §2: one path, not two): the same
    // feed, level 0, no size floor.
    if (!mVctFeed) mVctFeed.reset(new detail::VoxelFeed());
    gatherAndBuild(*mVctFeed, mVctVoxelizer, VoxelGatherInputs());

    mVctLighting = new Ogre::VctLighting(
        Ogre::Id::generateNewId<Ogre::VctLighting>(), mVctVoxelizer, anisotropic);
    // Document bounces are total (1 = one indirect bounce, which light injection
    // itself provides); VctLighting counts the extra propagation passes.
    mVctLighting->setAllowMultipleBounces(cascadeBounces(0) > 0u);
    // A NEW VOLUME: nothing has injected it, whatever this frame did to the one
    // it replaces. Then the first injection, through the one writer.
    mGiSingleInjectedFrame = ~0ull;
    injectCascade(0);
    // The materials this build read are the ones in force NOW.
    mGiBuiltMaterialGeneration = mGiMaterialGeneration;
    return itemCount;
}

// ===========================================================================
// PHOTON — THE CAMERA-CENTRED CASCADE SCHEDULER (PHOTON_SPEC.md P0)
// ===========================================================================
//
// WHAT IT REPLACES AND WHY IT IS OURS.
//
// The single-volume arm above fits ONE voxel box around the scene's content
// (`computeGiBounds`, capped at `autoBoundsMax`). It is an editor-at-rest
// shape: stand inside it and the bounce is right; walk out of it and the world
// goes unlit, and a scene bigger than the cap is voxelised at metres per cell.
// Photon's arm is N camera-centred cascades instead — fine cells near the eye,
// coarse ones far away, and what escapes the outermost cascade takes
// `VctLighting`'s ambient hemisphere, i.e. the Sky Light (measured in
// spikes/photon-s1 §4.2: the horizon reads exactly the GI-off value, so there
// is no wall of darkness at the boundary).
//
// Upstream Ogre-Next HAS a cascade manager (`VctCascadedVoxelizer`); we never used
// it, and since ATOM-VOXEL-2 it (with `VctImageVoxelizer`) is deleted from our
// fork. Three measured reasons (spikes/photon-s1):
//   * it hard-wires `VctImageVoxelizer`, which reproduces NONE of the
//     rasteriser's bounce at any cache resolution (§2: 0 % in the big scene,
//     3.6-6.2 % at 64^3 in the small one, 195-261 % at 128^3 — the error
//     changes sign with geometry, so it cannot be calibrated away);
//   * it rebuilds every dirty cascade in the frame it notices, with no budget:
//     43-75 ms CPU frames on a diagonal walk (§4);
//   * its `buildRelative` has no guard against a camera jump longer than a
//     cascade, and that is not cosmetic — an ordinary 142 m "look at the room
//     from outside" move lost the GPU (`VK_ERROR_DEVICE_LOST`), a 2 km jump
//     segfaulted the next frame (§4.4).
//
// What we DO use is everything underneath it: `VctVoxelizer` (the rasteriser),
// `VctLighting`, and `VctLighting::addCascade` — the chaining that makes
// HlmsPbs sample N volumes with the pin's own cone-continuation (its
// per-cascade brightness stabilisation is NOT used: see cascadeBounces, and
// ogre-patches 0074/0075 for the two defects it was compensating for). The
// cascade transforms reach the shader
// from the LIVE voxelisers every frame (`VctLighting::fillConstBufferData`
// builds `invXform` from `getVoxelOrigin()/getVoxelSize()`), so moving a
// cascade's region is picked up by the next frame with no extra push.
//
// THE SCHEDULER'S RULES, all of them:
//   1. Each cascade owns a lattice: `cell = 2*halfSize/resolution`, and it
//      re-centres when the camera crosses a plane of the `stepCells` lattice
//      (the pin's `consistentCascadeSteps` test). That lattice is ABSOLUTE
//      world space, not a radius about the camera it was built for, so a
//      re-centre arrives anywhere between the hysteresis band and a whole step
//      of travel — `step` is the SUPREMUM of the travel, never the distance
//      between two rebuilds. The new centre is quantised to the CELL lattice,
//      so the voxel grid never slides under the geometry.
//   2. AT MOST ONE cascade is re-voxelised per frame, innermost first. A
//      cascade that owes a rebuild and did not get the frame carries it (the
//      queue), and the queue is bounded: three owed rebuilds collapse into one,
//      because a rebuild is always AT THE CURRENT CAMERA and a backlog of old
//      positions is worthless work.
//   3. Godot's two DIRTY_ALL guards. A move of half a cascade's resolution or
//      more in cells means nothing of the old volume is reusable: every cascade
//      is queued at once and the counter says so. (In this arm a rebuild is
//      whole anyway, so the guard costs nothing to honour — it exists because
//      the incremental arms measured in P0's Stage B DO underflow without it,
//      and because a teleport must be a counted, bounded event rather than a
//      surprise.)
//   4. The ambient pair goes into EVERY cascade after every update that creates
//      or refreshes one. Upstream never does this and the room renders 2,2,2:
//      binding a `VctLighting` suppresses HlmsPbs' own ambient scene-wide
//      (`vctSpecular.w == 1` unconditionally), so a cascade with black
//      hemispheres is a cascade with no ambient at all (S1 §4.3).
//   5. Cascade 0 IS `mVctVoxelizer`/`mVctLighting`. Every binding, teardown,
//      status and material rule in this file goes on working unchanged.
//
// NOT here, and named so nobody looks for them: the sliced build, the double
// buffer and the blend fade (P0 Stage B measures whether they are needed), the
// per-cascade dirty path for edits (P2), the irradiance field riding a cascade
// (P1 — it needs a source patch: `IrradianceField` captures the voxel origin at
// `initialize()` and only ever binds cascade 0's textures, S1 §6).

std::vector<GiParams::GiCascadeDesc> OgreScene::resolveCascadeTable() const {
    std::vector<GiParams::GiCascadeDesc> table;
    const int wanted = std::min(std::max(mGi.cascadeCount, 0), 8);
    for (int i = 0; i < wanted; ++i) {
        const GiParams::GiCascadeDesc &d = mGi.cascadeSet[i];
        // A half-specified row is not a request the engine can honour halfway:
        // the whole table falls back to the tier's, which is the documented
        // meaning of "0 = the tier decides".
        if (d.halfSize <= 0.0f || d.resolution <= 0) { table.clear(); break; }
        GiParams::GiCascadeDesc e = d;
        e.resolution = std::min(std::max(d.resolution, 16), 256);
        // A PINNED STEP IS VALIDATED LIKE A DERIVED ONE (audit B10). The clamp
        // below was applied only where the engine derived the step, so a table
        // could ask for a step of a tenth of a cell (a re-centre every frame,
        // for a volume that moved a tenth of a voxel) or of ten thousand cells
        // (the camera leaves the box long before it re-centres). Both ends are
        // the pin's own guard, and they belong to the value, not to where it
        // came from.
        if (e.stepCells > 0.0f)
            e.stepCells = std::max(1.0f, std::min(e.stepCells, float(e.resolution) * 0.5f));
        // ...AND A TABLE THAT DOES NOT GROW OUTWARD IS NOT A REQUEST THE
        // RENDERER CAN HONOUR HALFWAY. `addCascade` chains coarse over fine:
        // Ogre derives `cascadeMaxLod` from the ratio of the cells, so a finer
        // (or equal) outer cascade gives Log2 of a value <= 1 — a negative or
        // zero maximum LOD — and the march never hands over. Same rule as the
        // half-specified row above and as the mirror's: the whole table falls
        // back to the tier's.
        if (!table.empty() &&
            (e.halfSize <= table.back().halfSize ||
             e.halfSize * 2.0f / float(e.resolution) <=
                 table.back().halfSize * 2.0f / float(table.back().resolution))) {
            table.clear();
            break;
        }
        table.push_back(e);
    }
    if (table.empty()) {
        // THE TIER TABLE. High is the Ogre sample's set, which is the set
        // PHOTON_SPEC §5's cadence table is arithmetic on (5 / 7.5 / 7.5 / 7.5 m
        // steps) — P0 measures it before anything is tuned. Medium and Low keep
        // the same reach at their own resolution: the far cascade is what stops
        // a corridor going black, and losing it would cost more picture than
        // the cells it saves.
        // THE TIER TABLE IS `giQualityFacts` (Types.h) — the same function the
        // app generates its tier descriptions from, so a chain the renderer
        // builds and a chain a tooltip promises are one table (render audit A5).
        // Low is 2 cascades at 64^3 and NOT the quality dial's 32 (PHOTON_SPEC
        // §7 E2 (4)); the reasoning for each row is at the table.
        // ...AND WHICH COLUMN OF IT (V1-RIG item 4): the VR one when the view
        // driving GI is the headset's. `mGiDriverStereo` is written by the
        // once-a-frame driver hook and a change marks the caches dirty, so
        // entering or leaving a session rebuilds the chain from the other
        // column exactly once.
        const GiQualityFacts facts =
            giQualityFacts(mGi.quality, mGiDriverStereo ? GiViewProfile::Vr
                                                        : GiViewProfile::Desktop);
        for (int i = 0; i < facts.cascadeCount; ++i) table.push_back(facts.cascades[i]);
    }
    // THE STEP TABLE, when a row did not pin one: `giResolveCascadeSteps`
    // (Types.h) — the pin's `autoCalculateStepSizes(4)` shape met with THE
    // NEAR-FIELD GUARANTEE (kGiNearFieldRadiusFraction, CASCADE-STEP-1), which
    // is a ceiling on it. It lives in the header rather than here because
    // `world.tierTable()` reports the resolved steps and the guaranteed radius
    // a tier's chain would have, and a chain the renderer builds and a chain a
    // tooltip promises must be one table (render audit A5) — the derivation is
    // pure arithmetic on the row and needs no scene, no device and no Ogre.
    //
    // WHAT THE RULE MOVED, exactly — every row not named here is arithmetically
    // unchanged, in both columns:
    //   Medium  c0  32 -> 16 cells   5.000 -> 2.500 m   r -0.156 -> 2.344
    //           c1  24 -> 16 cells   7.500 -> 5.000 m   r  2.188 -> 4.688
    //   High    c0  64 -> 34 cells   5.000 -> 2.656 m   r -0.078 -> 2.266
    //           c1  48 -> 34 cells   7.500 -> 5.313 m   r  2.344 -> 4.531
    // (Epic is High's table.) LOW WAS ALREADY COMPLIANT at every row and in
    // both columns — its outermost cascade is only 20 m, so the "every cascade
    // steps the outermost one's distance" rule already gave cascade 0 a 2.5 m
    // step — and so is the outermost cascade of every tier, including the VR
    // column's pinned 16 cells (60 m box, 30 m step, 28.125 m guaranteed
    // against the 27.0 the rule asks of it).
    giResolveCascadeSteps(table.data(), int(table.size()));
    return table;
}

// The lattice cell a position falls in, for a lattice of `size` metres. The
// pin's `quantizePosition` semantics (floor, signed), kept identical so a
// cascade of ours lands where upstream's would.
static inline long long jahQuantAxis(float pos, float size) {
    return (long long)std::floor(double(pos) / double(size));
}

// EVERY CASCADE RUNS THE DOCUMENT'S BOUNCE COUNT (PHOTON-M1, 2026-09-17).
//
// The pin gives a coarser cascade MORE bounces — `round(sqrt((b+1) * cellRatio
// - 1))`, which is 1/2/4/8 at Epic's three bounces — and calls it a brightness
// stabilisation: "as cell volume increases, we get darker results ... more
// bounces means brighter cascade" (upstream's VctCascadedVoxelizer::update, not in
// our fork). It is
// not physics. A bounce is a TRANSPORT step: it adds light everywhere, in
// proportion to what is already there, and cannot recover the occlusion a
// bigger cell loses (a coarse cell over-occludes a thin wall — that is a
// resolution term, SEAM-1 §3). The pin reached for it because the OTHER half of
// the same stabilisation, the shader's `( 1 - alpha )` de-amplification, and the
// bounce's own extra 1/pi were both making the outer cascades too dark —
// ogre-patches 0074 and 0075 fix those at the cause, so the compensation goes
// with them.
//
// It also priced: eight injection passes on Epic's outermost 64^3 volume per
// rebuild, 1 + 2 + 4 + 8 = 15 passes per chain, against 2 + 2 + 2 + 2 = 8 for
// the document's own count.
//
// `base` is the document's EXTRA-bounce count (one total bounce = zero extra
// passes), so the default leaves every cascade at 0 exactly as before.
Ogre::uint32 OgreScene::cascadeBounces(size_t idx) const {
    (void)idx;
    return Ogre::uint32(std::min(std::max(mGi.numBounces, 1), 4) - 1);
}

size_t OgreScene::buildCascadeArm(const Ogre::Vector3 &camPos) {
    // The same rule as buildVoxelArm's: a from-scratch chain re-reads every material.
    mVctMaterialRefreshOwed = true;
    queueAllGiMaterials();
    const std::vector<GiParams::GiCascadeDesc> table = resolveCascadeTable();
    if (table.empty()) return 0;
    const bool anisotropic = mGi.quality != GiQuality::Low;

    // World transforms must be current before voxelisation (the same rule
    // buildVoxelArm keeps).
    mSceneMgr->updateSceneGraph();

    mVctCascades.clear();
    mVctCascades.resize(table.size());
    mGiTickOwed = GiTickOwed::None;      // every cascade is injected by this build
    // WHICH COLUMN OF THE TIER TABLE THIS CHAIN IS (V1-RIG fix round item 4).
    // Recorded at the build and not read back from the driver flag: the flag is
    // a live reading of who is driving and the CHAIN is what was built, and the
    // two differ for exactly as long as a profile change is owed (the frame
    // between `mGiChainShapeDirty` and the flush). `giStatus` reports this one,
    // so "the chain is the VR column" is a fact about the chain.
    mGiChainProfileVr = mGiDriverStereo;
    for (size_t i = 0; i < table.size(); ++i) {
        VctCascade &c = mVctCascades[i];
        c.halfSize   = table[i].halfSize;
        c.resolution = Ogre::uint32(table[i].resolution);
        c.stepCells  = table[i].stepCells;
        // `correctAreaLightShadows` on the INNERMOST cascade only, which is the
        // upstream's own recommendation (its VctCascadeSetting, not in our fork): it is a
        // per-cascade memory and time cost and the near field is the only place
        // an area light's shadow is legible.
        c.voxelizer = new Ogre::VctVoxelizer(Ogre::Id::generateNewId<Ogre::VctVoxelizer>(),
                                             mRoot->getRenderSystem(), mRoot->getHlmsManager(),
                                             i == 0u /*correctAreaLightShadows*/,
                                             vctMaterialStore());
        c.voxelizer->setResolution(c.resolution, c.resolution, c.resolution);
        recentreCascade(c, camPos);
    }

    // An arm over no GI geometry is not built (the caller stays armed). Nothing is
    // recorded: each cascade's gather reads the GPU scene for itself.
    const size_t itemCount = countGiItems();
    if (!itemCount) {
        for (VctCascade &c : mVctCascades) { delete c.voxelizer; c.voxelizer = nullptr; }
        mVctCascades.clear();
        return 0;
    }


    // BUILT OUTERMOST FIRST, and the order is load-bearing rather than a
    // preference: a cascade's multi-bounce pass reads the cascades OUTSIDE it
    // (`addCascade` gives cascade i the chain i+1..N-1), so the coarse volumes
    // must hold light before the fine ones propagate through them. Upstream
    // iterates in reverse for the same reason and says so.
    //
    // AND IT IS ALL-OR-NOTHING. A build that throws half way through would
    // otherwise leave a chain whose cascades EXIST but hold nothing — the arm
    // reports four cascades, the shader samples four empty volumes, and the
    // scene renders with no GI and no error anyone can see. A failure tears the whole chain down and
    // reports nothing built, which is the state every caller already handles.
    JAH_TRY {
    for (size_t i = table.size(); i--; ) {
        VctCascade &c = mVctCascades[i];
        const auto tCascade = std::chrono::steady_clock::now();
        c.voxelizer->dividideOctants(1u, 1u, 1u);
        // THE FEED, per cascade (its own records: see VctCascade::feed). An empty
        // cascade needs no special path any more - the gather writes no records
        // and build() clears the volume, which is the honest empty picture.
        if (!c.feed) c.feed.reset(new detail::VoxelFeed());
        std::vector<uint32_t> mask;
        const VoxelGatherInputs in = cascadeGatherInputs(c, mask);
        gatherAndBuild(*c.feed, c.voxelizer, in);
        c.lighting = new Ogre::VctLighting(Ogre::Id::generateNewId<Ogre::VctLighting>(),
                                           c.voxelizer, anisotropic);
        const Ogre::uint32 extraBounces =
            Ogre::uint32(std::min(std::max(mGi.numBounces, 1), 4) - 1);
        c.lighting->setAllowMultipleBounces(extraBounces > 0u);
        if (i + 1u < table.size()) {
            c.lighting->reserveExtraCascades(table.size() - i - 1u);
            for (size_t j = i + 1u; j < table.size(); ++j)
                c.lighting->addCascade(mVctCascades[j].lighting);
        }
        injectCascade(i);                 // a fresh VctCascade: never latched
        c.lastCpuMs = float(std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - tCascade).count());
        c.built = true;
        ++c.rebuilds;
        // WHAT A BOOT COSTS, AND HOW OFTEN (BOOTVOX-1 needed it and nothing
        // reported it per rebuild): giStatus carries only the LAST rebuild's
        // cost, so a burst — a boot's, a teleport's — could not be added up.
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: cascade " + std::to_string(i) + " BUILT (rebuild #" +
                std::to_string(c.rebuilds) + ") in " + std::to_string(c.lastCpuMs) + " ms CPU");
    }

    // The head IS cascade 0 — from here on every existing rule in this file
    // (binding, teardown, material generation, status) sees the arm it knows.
    } JAH_CATCH(mError, abandonCascadeChain());
    if (mVctCascades.empty() || !mVctCascades[0].lighting) return abandonCascadeChain();
    if (giDebug()) {
        std::string row;
        for (size_t i = 0; i < mVctCascades.size(); ++i)
            row += (i ? " / " : "") + std::to_string(cascadeBounces(i));
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: cascade bounce counts (the document's own count on every cascade, at " +
            std::to_string(std::min(std::max(mGi.numBounces, 1), 4)) + " total bounces): " + row);
    }
    mVctVoxelizer = mVctCascades[0].voxelizer;
    mVctLighting  = mVctCascades[0].lighting;
    mGiBuiltMaterialGeneration = mGiMaterialGeneration;
    // A FROM-SCRATCH CHAIN OWES NOTHING: every cascade was just built from the
    // live scene, so whatever the dirty path had recorded is answered (G1).
    mGiCascadeDirtyBoxes.clear();
    mGiCascadeDirtyAll = false;
    return itemCount;
}

// Re-centres one cascade's region on the cell lattice around `camPos` and
// records the camera it was placed for. Does NOT voxelise.
void OgreScene::recentreCascade(VctCascade &c, const Ogre::Vector3 &camPos) {
    const float cell = c.cell();
    c.latticeX = jahQuantAxis(camPos.x, cell);
    c.latticeY = jahQuantAxis(camPos.y, cell);
    c.latticeZ = jahQuantAxis(camPos.z, cell);
    c.centre = Ogre::Vector3(float(double(c.latticeX) * double(cell)),
                             float(double(c.latticeY) * double(cell)),
                             float(double(c.latticeZ) * double(cell)));
    c.builtCam = camPos;
    if (c.voxelizer)
        c.voxelizer->setRegionToVoxelize(Ogre::Aabb(c.centre, Ogre::Vector3(c.halfSize)));
}

// WHAT A CASCADE VOXELISES, and the rule behind it. (An EMPTY cascade needs
// no rule any more: the gather writes no records and build() clears the volume.
// RULE 1 - attach once, never re-select - is gone with the attached set.)
//
// RULE 2 — A COARSE CASCADE DECLINES SUB-VOXEL OBJECTS. The raster voxeliser's
// price is the GEOMETRY INSIDE THE REGION, not the region (measured: in a
// 16-item room one whole build is a flat 2.7-5.3 ms from a 16 m3 box to a
// 1.7 million m3 one; on the lattice it goes 17.5 / 61.6 / 108 ms as the box
// grows 5 / 10 / 20 m, i.e. ~13 us per enclosed instance). An outer cascade
// encloses the most instances and resolves the least — the sample set's
// outermost cell is 1.875 m — so anything that cannot fill half a voxel of it
// is a smear the grid cannot represent. Declining it is not an approximation of
// the picture, it is declining to compute something the grid cannot hold, and
// it needs nothing from Ogre: the gather applies it per instance. Cascade 0 keeps
// everything by construction (its cell is the finest).
//
// THE TRADEOFF, stated: a scene whose distant light comes from MANY small
// objects (a field of lamps, a forest of leaves) loses their far bounce.
// `GiStatus::cascades[].items` reports what each cascade kept, so it is visible
// rather than mysterious. NOT PROVEN ON THE MEASURED SCENES: neither Showroom 2
// nor the lattice has sub-voxel geometry (the lattice's cubes sit exactly on
// the threshold), so this rule fired on nothing there and the numbers above are
// its cost, not its benefit.
static const float kCascadeSubVoxelFactor = 0.5f;

// AND THE LOD FRACTION IS A DIFFERENT NUMBER, MEASURED (ATOM-3 A2, the render
// audit's A2). Until this lane the ONE constant above answered two unrelated
// questions — "is this whole object too small for the grid" (rule 2) and "how
// coarse a baked level may stand in for this mesh" — and the second answer was
// wrong by two orders of magnitude.
//
// WHY HALF A CELL IS NOT THE ANSWER FOR A LEVEL. A voxel is written by a
// TRIANGLE-BOX OVERLAP TEST (Voxelizer_piece_cs.any:411) and the occupancy it
// records is BINARY: one intersecting triangle fills it, none leaves it empty.
// So a level whose worst deviation is `e` moves the surface across a cell
// BOUNDARY wherever the surface sat within `e` of one — about `e / cell` of the
// object's surface voxels — and the cone march's transmittance through a
// flipped voxel changes by a whole factor. At `e = cell / 2` that is HALF the
// shell. The old argument ("a consumer that samples at `cell` cannot represent
// a difference below one sample") is the argument for a POINT-SAMPLED signal;
// this consumer integrates a volume, and it is the boundary, not the sample,
// that moves.
//
// THE NUMBER IS A MEASUREMENT, not an argument (spikes/atom-3/FINDINGS.md §1):
// 1,000 instances of an imported 6,768-triangle sphere (baked errors 3.1 / 5.2 /
// 10.7 / 21.4 mm), the High tier's four cascades (cells 78 / 156 / 469 /
// 1875 mm), three poses, one cascade's level moved per arm against an
// all-level-0 reference that reproduces bit-for-bit:
//
//   cascade / e per cell |  0.004  0.007  0.011  0.027  0.067  0.137  0.274
//   c2 (469 mm cell)     |    -     70/255  70    112     -      -      -
//   c3 (1875 mm)         |   3/255   4      7      -      -      -      -
//   c0 (78 mm)           |    -      -      -      -    2/255   2      3
//
// (worst channel delta over the three poses; the c2 row's 70/255 is 3,026
// pixels of 230,400 above 1/255, spread over the whole frame and stable across
// a repeat.) TWO THINGS FALL OUT. First, the same RATIO costs wildly different
// pictures in different cascades — 0.274 of a cell costs 3/255 in cascade 0 and
// 0.007 of a cell costs 70/255 in cascade 2 — because what decides the damage
// is whether that cascade is the FINEST one holding the surface the camera is
// looking at (at the 25 m pose cascades 0 and 1 stand in empty space and
// cascade 2 owns the near field). So a per-cascade factor by INDEX, which is
// what the audit expected, is not what the measurement supports: the index is
// not the variable. Second, on that fixture the only fraction that holds every
// arm inside 4/255 is below 1/151 (the ratio at which cascade 2 takes level 1),
// so:
//
//   kCascadeLodCellFraction = 1/256
//
// AND WHAT THAT NUMBER IS NOT, measured on two further fixtures (ATOM-3-FIX,
// spikes/atom-3/FINDINGS.md §6): it is NOT a picture bound (measured by capping
// a WHOLE cascade at a level; fractions between the fixture's item scales were
// not run — an extrapolation that changes nothing, since such a fraction would
// forfeit most of the saving). Take the same
// protocol to an AVENUE — 353 imported instances at three scales and mixed
// distances over 90 m of depth, as a thin knot tube and again as ATOM-3's
// convex sphere — and the outermost cascade becomes the only cover of what the
// camera sees at the mid and far poses. There, cascade 3 at LEVEL 1 (an error
// of 0.00094 of its cell, four times STRICTER than this constant) already costs
// 62-80/255 on sparse pixels and 400-2,300 pixels above 4/255, and the fourth
// level does not make the worst pixel any worse — it is the same pixel at the
// same magnitude, because a BINARY occupancy flip is not proportional to the
// error that caused it. No non-zero fraction is inside 4/255 for a cascade in
// that role.
//
// So this constant is honestly described as: the fraction at which a cascade
// that is NOT the finest cover of the camera's surfaces stays inside 4/255,
// and which keeps the outermost cascade's 4-5x saving (measured 4.8x and 3.9x
// on those two fixtures, 4.0x on the lattice). A real picture bound is a
// question about a cascade's ROLE, which the scheduler knows and a constant
// cannot — the measured follow-up, not a number to tune (FINDINGS.md §6).
//
// WHAT IT STILL BUYS, which is the reason it is not simply 0: the win was never
// spread over the chain, it is concentrated in the OUTERMOST cascade (225 ms of
// a ~300 ms chain on that lattice, spikes/atom-2) — and the outermost cascade
// has the largest cell, so it is exactly the one a small fraction still lets
// take a coarse level. On that lattice the outer cascade keeps level 2 (4x less
// geometry) where it used to take level 4 (16x) and its worst pixel goes from
// 112/255 to 4.
//
// AND THE REFERENCE IS NOT THE TRUTH: every number above is a DIFFERENCE from
// voxelising the authored mesh, which at a 1.875 m cell is itself a crude
// integral. The fraction bounds how far the proxy may move the picture we
// already ship; it is not an error bound against light transport.
static const float kCascadeLodCellFraction = 1.0f / 256.0f;

// WHAT A CASCADE'S GATHER IS ASKED (ATOM P4b). The two rules above and the level
// rule are no longer CPU walks: they are three numbers handed to the gather job,
// which applies them to every instance on the device (JahVoxelGather_cs.glsl).
//
//   * RULE 2's floor, `cell * kCascadeSubVoxelFactor`, against the instance's world
//     AABB's longest extent - the same test `selectCascadeItems` made per item.
//   * THE LEVEL RULE: `allowedWorldError(kCascadeLodCellFraction, cell, scale)` and
//     the coarsest baked level whose bound is below it - `cascadeVoxelLod`'s rule,
//     word for word, walking the level table's bounds (GpuMeshLevel::bound), which
//     are the same baked errors `lodBoundsFor` answers. `scale` is the largest
//     column length of the instance's world rows, the derived scale's largest axis.
//   * RULE 1 IS GONE, not moved: it existed to spare `removeAllItems` re-deriving
//     every mesh buffer, and there are no attached items and no per-voxeliser mesh
//     buffers any more - the geometry rows are the GPU scene's, written once.
//
// WHAT IT NEVER READS is unchanged: the camera, the drawn LOD and the LOD bias.
OgreScene::VoxelGatherInputs OgreScene::cascadeGatherInputs(const VctCascade &c,
                                                            std::vector<uint32_t> &mask) const {
    VoxelGatherInputs in;
    in.cell = c.cell();
    in.tolerance = kCascadeLodCellFraction;
    in.minExtent = c.cell() * kCascadeSubVoxelFactor;
    // ONE applied answer: the document's request met with the run-wide latch
    // (`GiStatus::cascadeVoxelLod` reports it).
    in.lod = mCascadeVoxelLod;
    if (mGi.cascadeInstanceCap > 0) {
        budgetMaskFor(c, mask);
        in.budgetMask = &mask;
    }
    return in;
}

// THE INSTANCE BUDGET (PHOTON_SPEC §7 E2 (1), audit B7) - the one CPU walk left on
// the voxel path, and only when the document opts in (`cascadeInstanceCap` > 0).
//
// THE RANKING, and why it is size IN CELLS: `size / cell` is how many of THIS
// cascade's voxels an object spans, which is exactly how much of this cascade's
// picture it can possibly be. Distance to the cascade's own centre breaks ties.
// REACH BEFORE SIZE (round-2 review F3): the primary key is "can this cascade
// reach it" - its box grown by one step, so what it is about to scroll into is
// kept too. The pick is written as a bit per instance slot for the gather.
//
// WHY IT STAYS ON THE CPU: a top-K over a partial sort is a GPU radix select, not a
// one-pass predicate, and this lane moved the predicates. Stated, not hidden.
void OgreScene::budgetMaskFor(const VctCascade &c, std::vector<uint32_t> &mask) const {
    const float cell = c.cell();
    const float minExtent = cell * kCascadeSubVoxelFactor;
    const int cap = std::max(0, mGi.cascadeInstanceCap);
    mask.assign((mItemNodes.size() + 31u) / 32u, 0u);
    if (!cap) return;
    const float reach = c.halfSize + c.step();
    const Ogre::Aabb reachBox(c.centre, Ogre::Vector3(reach));
    struct Ranked { bool reachable = false; float cells = 0.0f; float dist2 = 0.0f;
                    size_t slot = 0; };
    std::vector<Ranked> ranked;
    ranked.reserve(mItemNodes.size());
    for (const Node *np : mItemNodes) {
        Ogre::Item *item = np->item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (np->itemSlot == size_t(-1)) continue;
        const Ogre::Aabb wa = item->getWorldAabb();
        const Ogre::Vector3 h = wa.mHalfSize;
        const float extent = std::max(std::max(h.x, h.y), h.z) * 2.0f;
        if (extent < minExtent) continue;                                     // rule 2
        const Ogre::Vector3 d = wa.mCenter - c.centre;
        ranked.push_back({ reachBox.intersects(wa), extent / cell, d.dotProduct(d), np->itemSlot });
    }
    const size_t take = std::min(size_t(cap), ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + take, ranked.end(),
                      [](const Ranked &a, const Ranked &b) {
                          if (a.reachable != b.reachable) return a.reachable;
                          if (a.cells != b.cells) return a.cells > b.cells;
                          return a.dist2 < b.dist2;
                      });
    for (size_t i = 0; i < take; ++i)
        mask[ranked[i].slot >> 5u] |= 1u << (ranked[i].slot & 31u);
}

bool OgreScene::rebuildCascade(size_t idx, GiStaleReason reason, bool *placementCommitted) {
    if (placementCommitted) *placementCommitted = false;
    if (idx >= mVctCascades.size()) return false;
    VctCascade &c = mVctCascades[idx];
    if (!c.voxelizer || !c.lighting) return false;
    // ONE MONITOR ROW PER CASCADE REBUILD, with the GPU pair (patch 0027) — the
    // number P0 exists to measure. The detail is a compile-time constant per
    // cascade index for the reason EnginePrivate.h's CacheScope header gives:
    // nothing may be constructed at the call site while the monitor is off.
    static const char *kRowNames[8] = { "vct.cascade0", "vct.cascade1", "vct.cascade2",
                                        "vct.cascade3", "vct.cascade4", "vct.cascade5",
                                        "vct.cascade6", "vct.cascade7" };
    monitor::CacheScope work(CacheKind::Gi, monitor::reasonOf(reason), (unsigned long long)idx,
                             kRowNames[std::min(idx, size_t(7))], mRoot->getRenderSystem());
    const auto t0 = std::chrono::steady_clock::now();
    // A FAILED REBUILD MUST NOT REPORT A SUCCESSFUL ONE (audit B4). The caller
    // has already re-centred this cascade — the voxeliser's region, which
    // `fillConstBufferData` reads LIVE, now describes the new place — so a
    // build that throws leaves the shader mapping the new region onto the old
    // place's voxels: a wrong bounce, silently, until the next scroll. The
    // exception is caught here (JAH_CATCH returns) and answered by the caller,
    // which puts the placement back and leaves the rebuild owed.
    // The body is a lambda because JAH_CATCH RETURNS: the bookkeeping below has
    // to run either way, and a failure has to be answerable rather than silent.
    // (A MATERIAL EDIT IS A PLAIN DIRTY HIT NOW, A5b fix round. It used to buy this
    // cascade a REPLACEMENT voxeliser - new volumes, swapped into the lighting -
    // because each voxeliser owned a material cache keyed by raw pointer. The chain
    // shares one store, refreshed IN PLACE (VctMaterial::refreshAll) and evicted on
    // a death (0081), so the rebuild below re-reads the edited rows into the same
    // voxeliser.)
    bool built = false;               // the volumes on the GPU describe the NEW placement
    const auto attempt = [&]() -> bool {
        JAH_TRY {
            mSceneMgr->updateSceneGraph();
            // FAULT INJECTION, for the suite that proves the revert path (F2).
            // The same shape as JAH_TEXTURE_WAIT_FAULT (OgreEngine.cpp): read
            // per rebuild rather than cached, because the test arms it between
            // frames — a getenv against a rebuild that costs milliseconds is
            // not a cost anyone can measure. The region has already moved at
            // this point, which is exactly the state a real `build()` throw
            // leaves behind.
            if (const char *fault = std::getenv("JAH_GI_CASCADE_FAULT")) {
                if (std::strtol(fault, nullptr, 10) == (long)idx)
                    OGRE_EXCEPT(Ogre::Exception::ERR_INTERNAL_ERROR,
                                "JAH_GI_CASCADE_FAULT: forced cascade build failure",
                                "OgreScene::rebuildCascade");
            }
            // THE FEED: the gather over the GPU scene, then the build over what it
            // wrote. The readings (items, levels, triangles) are its readout,
            // counted on the device and taken by giStatus.
            if (!c.feed) c.feed.reset(new detail::VoxelFeed());
            std::vector<uint32_t> mask;
            gatherAndBuild(*c.feed, c.voxelizer, cascadeGatherInputs(c, mask));
            built = true;
            // THE SECOND FAULT ARM (round-2 F1): everything above this line can
            // throw BEFORE the volumes describe the new placement and everything
            // below it AFTER, and the two failure paths are opposites - one puts
            // the placement back, the other keeps it. Both are proven by the suite.
            if (const char *fault = std::getenv("JAH_GI_CASCADE_FAULT_POST")) {
                if (std::strtol(fault, nullptr, 10) == (long)idx)
                    OGRE_EXCEPT(Ogre::Exception::ERR_INTERNAL_ERROR,
                                "JAH_GI_CASCADE_FAULT_POST: forced failure after the build",
                                "OgreScene::rebuildCascade");
            }
            // A RE-VOXELISED CASCADE IS A NEW VOXEL STATE, and the one-writer
            // latch is per state: an injection earlier in this writer frame (a
            // from-scratch build the host asked for between frames, at the
            // placement this rebuild has just left) lit voxels that no longer
            // exist, so the latch is re-armed for the volume the build just wrote
            // (the same rule refreshVctFast keeps for the single volume). Within
            // a frame nothing else injects a cascade before the scheduler's
            // rebuild (the host's tick is owed and runs after it), so a refusal
            // here would be a new path, and it is an error rather than a skip:
            // it would leave re-voxelised volumes lit for the old ones.
            c.injectedFrame = ~0ull;
            if (!injectCascade(idx))
                OGRE_EXCEPT(Ogre::Exception::ERR_INTERNAL_ERROR,
                            "a rebuilt cascade was refused its injection",
                            "OgreScene::rebuildCascade");
            // ...AND THAT IS THE MOVING TICK'S ANSWER TOO (DRAG-1): the one
            // writer's injection, which is exactly what refreshGiLighting would
            // compute for it, so the next in-motion tick skips it. See the tick.
            c.injectedSinceTick = true;
            c.injectedAtLightSerial = mGiLightWriteSerial;
            return true;
        } JAH_CATCH(mError, false);
    };
    const bool ok = attempt();
    // A THROW AFTER THE BUILD (the ambient push, `VctLighting::update`'s dispatch -
    // the VK_ERROR_OUT_OF_DEVICE_MEMORY class is real on this box) leaves volumes
    // that are correct and current for the NEW placement with only the light
    // injection missing: the caller must NOT put the placement back, because the
    // region the shader reads live is the one these voxels were built for.
    if (!ok && built && placementCommitted) *placementCommitted = true;
    c.lastCpuMs = float(std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0).count());
    if (giDebug())
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: cascade " + std::to_string(idx) + (ok ? " re-voxelised" : " FAILED") +
            " (rebuild #" + std::to_string(c.rebuilds) + ", reason " +
            std::string(giStaleReasonName(reason)) + ") in " + std::to_string(c.lastCpuMs) + " ms CPU");
    if (!ok) {
        // The row STANDS (the frame really did spend that time) with no units:
        // nothing was voxelised. Logged once per scene — a cascade that throws
        // usually throws again on the next scroll, and a log line per frame
        // would bury the first one.
        work.setUnits(0);
        if (!mCascadeFailureLogged) {
            mCascadeFailureLogged = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: cascade " + std::to_string(idx) + " failed to rebuild (" +
                mError + ") — its placement is kept, the next frame retries, and a second "
                "failure stands the cascade down until the camera moves again");
        }
        return false;
    }
    c.built = true;
    ++c.rebuilds;
    monitor::noteCascadeRebuild();     // the frame's own "<= 1 per frame" counter
    // WHAT THIS CASCADE VOXELISED, not what the scene holds (audit D6, round-2
    // F1) - per cascade, so a capture can tell the outer cascade's work from the
    // inner one's. A READING now, and so ONE REBUILD LATE: the gather's readout is
    // collected without waiting (never a stall in the rebuild frame), so the row
    // carries the enclosed count this cascade's PREVIOUS gather wrote.
    if (c.feed) c.feed->harvest();
    work.setUnits(c.feed ? c.feed->reading().instances : 0u);
    return true;
}

// THE RE-CENTRE HAS A HYSTERESIS BAND, AND A MEASUREMENT IS WHY (lane V1-RIG,
// 2026-09-18).
//
// THE DEFECT. The scroll test was "the camera's step cell is not the cell it
// was built in" — a plane, with no band. A camera that OSCILLATES across such a
// plane therefore re-voxelises on every crossing, for ever, while standing
// still. That is not a corner case in a headset: a wearer's head sways
// centimetres and the runtime reports every millimetre of it, so a wearer whose
// head happens to sit on a lattice plane pays cascade rebuilds at a fraction of
// every frame. Measured on the rig (spikes/v1-rig, Medium, a +-0.12 m sinusoid
// about x = 0, which IS a step plane at every tier): 64 cascade rebuilds and 16
// whole irradiance-field re-integrations in 200 still frames — 32 % of frames
// doing 5.5-8.8 ms of GPU work each — against ZERO for the same sway at
// x = 3.75, the middle of the same cell. The same mechanism was visible with a
// completely still head: Monado's simulated HMD sways +-0.1 m about x = 0 and
// the "static pose" arms of item 1 rebuilt the whole chain three times in 160
// frames.
//
// THE FIX, and why it costs nothing in placement quality. The lattice the test
// quantises on is SHRUNK by the band and the band is then added back on both
// sides: with `kStepHysteresis` = h, the planes sit every step*(1-h) metres and
// the camera must be h*step PAST the plane it crossed. The worst distance the
// camera can be from the centre it was built at is therefore
// step*(1-h) + h*step = step — EXACTLY what it was before, so no cascade holds
// the camera any further off-centre than it does today. What changes is the
// rebuild count while WALKING, up by 1/(1-h) = 11 % at h = 0.1, and what it buys
// is that no rest pose can thrash the chain at all.
//
// It is deliberately not a velocity or a time rule: the distance is the physics
// (how far the volume is from describing where the camera is), a clock is not.
static const float kStepHysteresis = 0.1f;

void OgreScene::updateCascades(const Ogre::Vector3 &camPos) {
    if (mVctCascades.size() < 1u || !mVctCascades[0].built) return;

    // ---- 0. A DEFERRED FIELD FOLLOW OWNS THE FRAME'S ONE SLOT -------------
    // (V1-RIG item 2 / L11.) It is paid before anything reads the field this
    // frame and it spends the slot, so the frame that pays it rebuilds no
    // cascade: the two halves of a step never share a frame again.
    bool followPaid = false;
    if (mIfdFollowOwed) {
        followCascade0Field(mIfdFollowReason);
        mIfdFollowOwed = 0;
        followPaid = true;
    }

    // ---- 1. WHO MOVED, AND DID ANYTHING JUMP? ------------------------------
    for (VctCascade &c : mVctCascades) {
        const float cell = c.cell();
        const float step = c.step();
        if (cell <= 0.0f || step <= 0.0f) continue;
        // THE HYSTERETIC STEP TEST (kStepHysteresis, above): the planes are
        // `lattice` apart and the camera must be `band` past the one it left.
        const float lattice = step * (1.0f - kStepHysteresis);
        const float band    = step * kStepHysteresis;
        bool moved = false;
        {
            const float cam[3]   = { camPos.x, camPos.y, camPos.z };
            const float built[3] = { c.builtCam.x, c.builtCam.y, c.builtCam.z };
            for (int a = 0; a < 3 && !moved; ++a) {
                const long long kb = jahQuantAxis(built[a], lattice);
                if (jahQuantAxis(cam[a], lattice) == kb) continue;
                const double lo = double(kb) * double(lattice);
                const double hi = lo + double(lattice);
                if (double(cam[a]) < lo - double(band) ||
                    double(cam[a]) >= hi + double(band))
                    moved = true;
            }
        }
        // THE TWO DIRTY_ALL GUARDS (Godot's, and S1 §4.4's three GPU losses are
        // why they are a prerequisite rather than an improvement).
        //
        // GUARD 1 — MOVED AT LEAST ITS OWN SIZE. The new box does not overlap
        // the old at all: nothing of it is reusable, by any scheme. This is a
        // teleport, and it is what `cascadeFullRebuilds` counts. (It is also
        // exactly the case upstream's `buildRelative` underflows on:
        // `srcBox.width -= srcBox.x` with a diff bigger than the volume.)
        //
        // GUARD 2 — MORE THAN HALF THE VOLUME IS NEW. The reusable part is the
        // product over the axes of the overlapping fraction; one minus it is
        // what an incremental arm would have to re-voxelise anyway. Above a
        // half there is nothing left to save and a whole build is the cheaper
        // answer. In THIS arm the rebuild is whole either way, so the guard
        // cannot change the work — it is COUNTED instead
        // (`cascadeDirtyMajority`), because "how often is a scroll already
        // majority-dirty" is precisely the number that decides whether a
        // slab-shifting arm is worth building at all (PHOTON_SPEC D1).
        const long long diff[3] = {
            std::llabs(jahQuantAxis(camPos.x, cell) - jahQuantAxis(c.builtCam.x, cell)),
            std::llabs(jahQuantAxis(camPos.y, cell) - jahQuantAxis(c.builtCam.y, cell)),
            std::llabs(jahQuantAxis(camPos.z, cell) - jahQuantAxis(c.builtCam.z, cell)) };
        const long long res = (long long)c.resolution;
        const bool jumped = diff[0] >= res || diff[1] >= res || diff[2] >= res;
        double reusable = 1.0;
        for (int a = 0; a < 3; ++a)
            reusable *= std::max(0.0, 1.0 - double(diff[a]) / double(res));
        if (!jumped && (moved || diff[0] || diff[1] || diff[2]) && reusable < 0.5)
            ++mCascadeDirtyMajority;
        if (jumped) c.jumped = true;
        // A REBUILD IS OWED, AND OWING IT TWICE MEANS NOTHING. A rebuild always
        // happens AT THE CURRENT CAMERA, so a backlog of old positions is
        // worthless work by construction: `pending` is a FLAG ("this cascade is
        // behind"), and a burst of steps while the budget is spent elsewhere
        // collapses into the one rebuild that catches it up.
        if (moved || jumped) {
            // A SCROLL CLAIMS A PENDING FLAG AN EDIT MAY ALREADY HAVE RAISED —
            // and the work is the same one rebuild either way. The REASON,
            // though, is the edit's: a capture must not read "the camera did
            // this" for a frame an edit paid for (E0's B12/D5 in reverse).
            if (!c.pending) c.pendingReason = GiStaleReason::Camera;
            c.pending = 1;
        }
    }

    // ---- 2. SPEND AT MOST ONE REBUILD, INNERMOST FIRST ---------------------
    // The near field is what the user is looking at, so it is what gets the
    // frame. A cascade that waits keeps showing its old, correctly-lit volume:
    // the picture is never half-built, only slightly behind.
    bool spent = followPaid;
    // ...AND WHETHER A CASCADE REALLY WAS RE-VOXELISED (fix round item 4): a
    // rebuild that threw BEFORE its swap put its placement back and voxelised
    // nothing, so it owes no settle even though the frame was spent on it.
    bool rebuilt = false;
    for (size_t i = 0; i < mVctCascades.size(); ++i) {
        VctCascade &c = mVctCascades[i];
        if (!c.pending) continue;
        if (spent) { ++mCascadeDeferrals; continue; }
        // THE PLACEMENT, SAVED BEFORE IT MOVES (audit B4). recentreCascade
        // writes the voxeliser's region, and the shader reads that region live
        // — so if the build then throws, the old voxels would be sampled as if
        // they described the new place. On a failure it goes back exactly as it
        // was and the rebuild stays owed.
        const long long lx = c.latticeX, ly = c.latticeY, lz = c.latticeZ;
        const Ogre::Vector3 prevCentre = c.centre, prevCam = c.builtCam;
        const bool wasJumped = c.jumped;
        recentreCascade(c, camPos);
        // THE MONITOR'S REASON IS THE CAMERA (audit B12/D5): this work was
        // caused by walking, not by an edit, and `mLastStaleReason` — the last
        // thing that staled the PROBE grid, possibly minutes ago — said
        // "material" or "light" for it. `Rebuild` stays the reason for a
        // teleport-forced one, which is what `cascadeFullRebuilds` counts.
        bool placementCommitted = false;
        const GiStaleReason reason = c.jumped ? GiStaleReason::Rebuild : c.pendingReason;
        if (!rebuildCascade(i, reason, &placementCommitted)) {
            // ...UNLESS THE CASCADE KEPT IT (round-2 F1). A rebuild that failed
            // AFTER its build succeeded has current voxels for the NEW box; putting the old box back would describe them
            // wrong, which is exactly the defect the revert exists to prevent,
            // pointing the other way.
            if (!placementCommitted) {
                c.latticeX = lx; c.latticeY = ly; c.latticeZ = lz;
                c.centre = prevCentre; c.builtCam = prevCam; c.jumped = wasJumped;
                if (c.voxelizer)
                    c.voxelizer->setRegionToVoxelize(
                        Ogre::Aabb(c.centre, Ogre::Vector3(c.halfSize)));
            }
            // ONE RETRY, THEN STAND DOWN (round-2 review F3). The next frame
            // tries again — a build can fail for a reason that passes (a
            // transient allocation, a device that has just come back) — but a
            // cascade whose build is genuinely impossible must not spend the
            // frame's whole GI budget for ever, starving the cascades that
            // still work. After the second failure it keeps the volume it has
            // and waits: the next step of the camera re-arms `pending` through
            // the ordinary scroll test, which is also when its box (and so the
            // reason it threw) has actually changed.
            // A FAILURE THAT KEPT ITS PLACEMENT STILL MOVED CASCADE 0, and the
            // field must not be left describing the place the chain has left:
            // its volume follows (the voxels there are current — only the light
            // injection is missing, which the next rebuild supplies).
            if (i == 0u && placementCommitted) oweCascade0FieldFollow(reason);
            if (placementCommitted) rebuilt = true;   // those voxels ARE new
            spent = true;                          // the frame paid for it either way
            if (++c.failures >= 2u) c.pending = 0; // ...otherwise `pending` stays set
            continue;
        }
        c.failures = 0;
        if (c.jumped) { ++mCascadeFullRebuilds; c.jumped = false; }
        c.pending = 0;
        c.pendingReason = GiStaleReason::Camera;
        // THE FIELD RIDES CASCADE 0 (E1 item 1), and it follows ON THE NEXT
        // FRAME, out of that frame's own GI slot (V1-RIG item 2 /
        // LATER_OPTIMISATIONS L11: the two GPU bursts together are 11.7 ms at a
        // headset's pixel count and neither alone is over 11.1 — see
        // `mIfdFollowOwed`). The pixel transform is rebuilt from the field's
        // volume per pass and the volume moves WITH the atlas, so the one frame
        // in between reads probes integrated where the field still says it is:
        // one step of staleness, never a wrong place.
        if (i == 0u) oweCascade0FieldFollow(reason);
        spent = true;
        rebuilt = true;
    }
    // WHAT THE ARM LIT, KEPT CURRENT (audit B9). `giStatus().boundsMin/Max` is
    // the outermost cascade's box, and that box MOVES — it was written once at
    // build time and went stale on the first scroll, which made every reader
    // (the status verb, the mirror's GI-volume box, a capture) describe where
    // the lighting used to be.
    if (spent && !mVctCascades.empty())
        mGiLitVolume = Ogre::Aabb(mVctCascades.back().centre,
                                  Ogre::Vector3(mVctCascades.back().halfSize));

    // ---- 3. A REBUILT CHAIN IS OFF ITS FIXED POINT UNTIL SOMETHING SAYS SO --
    //
    // THE DEFECT (LAMPREST-3, 2026-09-18, measured at full resolution). A
    // cascade rebuild ends with ONE injection of THAT cascade
    // (`rebuildCascade`'s closing `VctLighting::update`) over the radiance that
    // cascade happened to hold — which, after a scroll, is the light of
    // wherever it was standing before. That is one Jacobi pass of a fixed point
    // over COUPLED volumes (the same mathematics `refreshGiLighting`'s at-rest
    // rule is written from): the cascades inside and outside it still hold the
    // answer they had, and nothing in this engine then ran the chain's at-rest
    // injection, because the mirror's cadence only ticks while a LIGHT or a
    // piece of GEOMETRY is moving and a camera walk moves neither.
    //
    // So the chain simply STAYED off its fixed point, for ever. Measured on the
    // sealed 12 m room of `scripting.e2e.movable_lamp_rest` (Medium, 4 cascades,
    // 4 bounces, the lamp at 0.45): the camera walked 35 m away and back to the
    // SAME pose — the cascade placements returned exactly and the albedo voxels
    // came back byte-identical — and cascade 0's light voxels came back
    // 60,852 of its 82,176 lit bytes different, by up to 55/255 (mean 12.326 →
    // 13.072), the picture 5/255 brighter at six probes, and it never healed.
    // ONE `world.refreshGi()` — the at-rest chain injection, nothing else —
    // put every byte back (spikes/lamprest-3). A from-scratch rebuild of the
    // whole arm at the same pose reproduces the boot answer to 30 bytes of a
    // million, so the fixed point is not in doubt: the scrolled chain was the
    // wrong one.
    //
    // THE RULE. A rebuild raises a DEBT of injections — one sweep over every
    // cascade, outermost first — and the scheduler pays it
    // ONE INJECTION PER FRAME out of the same one-slot budget the rebuilds come
    // from, the rebuild queue keeping priority. It is not a settle "at the
    // stop": there is no camera-still gate at all, deliberately. THE VR CASE IS
    // WHY (fix round item 1): in a session the GI driver is the headset's view
    // and its camera is the tracked HEAD, written every frame, so a rule that
    // waited for two frames at the same position would never fire for a wearer
    // — room-scale walking re-voxelises (cascade 0's step is 5 m at EVERY tier:
    // the derived step is clamped to resolution/2 cells, which is 32 cells of a
    // 64^3 cascade over 10 m and 64 of a 128^3 one over the same 10 m — corrected
    // 2026-09-18, lane V1-RIG; the 0.625 m here was a cell, not a step)
    // and the 55/255 error would simply live in the headset until something
    // else moved. With no gate, a walk that never ends keeps paying one cheap
    // injection per idle slot and never starves.
    //
    // THE COST: spread, one injection a frame (~1 ms in Debug) instead of the
    // whole at-rest tick in one frame. THE BYTES ARE THE SAME: the steps run in
    // the tick's own order (cascades outermost-first), and one sweep IS the
    // fixed point (kAtRestSweeps, EnginePrivate.h; gi.chain_converge).
    //
    // The single-volume arm owes nothing: its rebuild injects the one volume at
    // the document's own bounce count, which IS what the tick would compute.
    //
    // `JAHSHAKA_GI_NO_REBUILD_SETTLE` stands the rule down for measurement:
    // gi.chain_converge drives it so the suite proves the defect it guards against rather than
    // asserting a number that happens to pass (13.00/255 with it set, 0.00
    // without, measured on that suite's room).
    if (rebuilt && mVctCascades.size() > 1u)
        oweChainSettle();            // the lights and environment these steps must all see

    // ---- 2b. THE HOST'S LIGHT TICK, AFTER THE REBUILD (the one writer) ------
    // (PHOTON-WRITER-1; refreshGiLighting says why it is owed rather than run.)
    // After the rebuild so the tick never lights voxels the rebuild replaces,
    // and before the settle step so the step finds the cascades it would inject
    // already injected this frame and waits.
    if (mGiTickOwed != GiTickOwed::None) {
        const bool rest = mGiTickOwed == GiTickOwed::Rest;
        mGiTickOwed = GiTickOwed::None;
        runChainTick(!rest);
    }
    if (!spent && mGiSettleStepsOwed > 0 && !std::getenv("JAHSHAKA_GI_NO_REBUILD_SETTLE")) {
        bool pending = false;
        for (const VctCascade &c : mVctCascades)
            if (c.pending) { pending = true; break; }
        if (!pending) payChainSettleStep();      // one injection, this frame
    }
    // THE END OF A DRAG GESTURE'S OWN DEBT (MOVER-1), paid last and only once
    // everything else has: every re-voxelisation drained and the incremental
    // settle finished. It is the same call the host's explicit Refresh makes —
    // the full at-rest tick, at the scene's own bounce count, followed by the
    // field's re-integration — and it is what makes the picture a gesture ends
    // on identical to the picture the same pose reaches without the promotion.
    // One per gesture, never one per frame.
    if (mDragSettleOwed && !spent && mGiSettleStepsOwed == 0) {
        bool pending = false;
        // ...and on a frame nothing has injected yet: the drag settle is a
        // whole at-rest tick, and the one writer injects a cascade once a frame.
        for (size_t i = 0; i < mVctCascades.size(); ++i)
            if (mVctCascades[i].pending || injectedThisFrame(i)) { pending = true; break; }
        if (!pending) {
            mDragSettleOwed = false;
            runChainTick(false);
            // ...AND ONLY NOW THE PHOTOGRAPHS. The object is GI geometry again,
            // at its new pose, in a chain that has been re-voxelised and
            // re-injected; this is the first frame a capture can hold the room
            // as it actually is. The sweep drains at the tier's own budget like
            // every other staleness — a rate, not a hitch.
            staleProbeGrid(GiStaleReason::Mobility);
        }
    }
}

// The FAILED-BUILD path: nothing is bound yet (mVctVoxelizer/mVctLighting are
// assigned only once the whole chain is up), so cascade 0's objects are owned
// here too and every one of them must go — lighting before voxeliser, as
// always. Returns 0 so it can be the value of a JAH_CATCH.
size_t OgreScene::abandonCascadeChain() {
    for (size_t i = mVctCascades.size(); i--; ) {
        delete mVctCascades[i].lighting;  mVctCascades[i].lighting = nullptr;
        delete mVctCascades[i].voxelizer; mVctCascades[i].voxelizer = nullptr;
    }
    mVctCascades.clear();       // each cascade's feed goes with it
    return 0;
}

void OgreScene::teardownExtraCascades() {
    // Cascades 1..N-1 only: [0] is mVctVoxelizer/mVctLighting and belongs to
    // teardownVct, which deletes it in the order the arm requires (lighting
    // before voxeliser, both after the HlmsPbs unbind). The head's
    // `mExtraCascades` is a vector of raw pointers it does not own, so the
    // order between the head and the extras does not matter — but the extras
    // must still die lighting-first, for the same reason the head does.
    for (size_t i = mVctCascades.size(); i-- > 1u; ) {
        delete mVctCascades[i].lighting;  mVctCascades[i].lighting = nullptr;
        delete mVctCascades[i].voxelizer; mVctCascades[i].voxelizer = nullptr;
    }
    mVctCascades.clear();
}

// THE PLACEMENT'S FIVE PHASES, FILED AS CACHE ROWS (OPEN_COVER_SPEC A(4)).
// `CacheKind::Probe / "placement"` used to carry a unit count and ms -1 — "the
// time is already inside the pass records" — which is true of a probe CAPTURE
// that a compositor pass renders and false of everything else this call does:
// the scout, upstream's placement fit, the keep/drop readback and the grid's
// re-create at the real resolution are UI-thread work no pass record can see.
// Measured on Grand Showroom 2 they are the largest single item in the whole
// arm, so they get rows with real milliseconds and the capture keeps its -1.
static void notePlacementPhases(double msScout, double msPlace, double msDrop,
                                double msRecreate, double msCapture,
                                unsigned candidates, unsigned kept)
{
    if (!monitor::live()) return;
    monitor::noteCacheWork(CacheKind::Probe, WorkReason::Build, 0, "placement.scout",
                           1u, float(msScout));
    monitor::noteCacheWork(CacheKind::Probe, WorkReason::Build, 0, "placement.fit",
                           candidates, float(msPlace));
    monitor::noteCacheWork(CacheKind::Probe, WorkReason::Build, 0, "placement.keepDrop",
                           candidates, float(msDrop));
    if (msRecreate > 0.0)
        monitor::noteCacheWork(CacheKind::Probe, WorkReason::Build, 0, "placement.recreate",
                               kept, float(msRecreate));
    if (msCapture > 0.0)
        monitor::noteCacheWork(CacheKind::Probe, WorkReason::Build, 0, "placement.capture",
                               kept, float(msCapture));
}

// THE PROBE GRID, GONE — unbound if this scene owns the binding, then deleted,
// with every reading that described it put back to "none".
//
// BY POINTER IDENTITY, like teardownVct's unbind: the process-wide HlmsPbs
// binding may belong to ANOTHER scene, and clearing it from here would blank
// that scene's reflections for a grid this one never built. (This scene's own
// pointer is the one being deleted.)
//
// TWO CALLERS, and the second is why it is a function (fix round item 2): the
// "every candidate photographed nothing" path, which always had this code, and
// `restartStagedProbeBuild`, where a grid that `buildPccFit` CREATED and
// `buildPccFinish` never bound would otherwise be orphaned — still registered
// with Root and the CompositorManager2 as a listener by its own `setEnabled`,
// still holding three 32 px textures, reported by `giStatus().probeCount` with
// `pccBound` false, and re-captured at 32 px by the budget every frame once
// something marked it stale.
void OgreScene::destroyProbeGrid() {
    if (!mPcc) return;
    {
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        if (pbs->getParallaxCorrectedCubemap() == mPcc) {
            pbs->setParallaxCorrectedCubemap(nullptr);
            // The env slot's occupancy is a PROCESS-WIDE question
            // (reflectionTexForDatablocks' note): every scene's datablocks may
            // take their own sky cube back now.
            if (mEngine) mEngine->reapplyReflectionsAllScenes();
        }
    }
    delete mPcc; mPcc = nullptr;
    mProbeSlots.clear();
    mProbeUpdatesPerFrame = 0;
    mPccCaptureSize = 0;
    mPccHdr = mPccShadowed = false;
}

// THE PROBE GRID, WHOLE — the three stages back to back, which is the order and
// the code the single function always ran. Every caller but the streaming flush
// (OPEN_COVER_SPEC §2 A) uses this, so a complete frame builds the grid exactly
// as it did before the split.
void OgreScene::buildPcc(const Ogre::Aabb &litVolume) {
    buildPccScout(litVolume);
    if (!mPccStageOk) return;
    buildPccFit();
    buildPccFinish();
}

// Milliseconds since the last phase split, and restarts the clock. A member
// rather than a lambda because the three stages run in three different frames
// when the build is staged.
double OgreScene::pccPhaseSplit() {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - mPccPhaseClock).count();
    mPccPhaseClock = now;
    return ms;
}

// STAGE 1 OF THE PROBE GRID: WHERE IT GOES (OPEN_COVER_SPEC §2 A). One
// photograph of the space from the middle of the scene's own box, and the
// knobs the other two stages read. 69-84 ms measured. Leaves mPccStageOk
// false when the grid cannot be built at all — this stage's only refusal.
void OgreScene::buildPccScout(const Ogre::Aabb &litVolume) {
    mPccStageOk = false;
    mGiStagedVolume = litVolume;
    for (double &v : mPccPhaseMs) v = 0.0;
    // BY VALUE, and it has to be: the caller passes `mGiProbeRegion` itself and
    // this function assigns that member below, so a reference would alias — the
    // volume every measurement here is relative to would silently become the
    // answer halfway through (it did: every probe of a pinned room read as
    // having seen nothing, because its span was divided by the region instead
    // of by the volume).
    const Ogre::Aabb aabb = litVolume;
    // THE PLACEMENT'S PHASES, TIMED (OPEN_COVER_SPEC A(4); lane OPEN-COVER-2a).
    // `probe placement` reported ONE number and the bundle reported -1, so the
    // 770 ms this call costs on Grand Showroom 2 could not be attributed to any
    // of its five phases. Each is a monitor CacheScope of its own — the same
    // instrument the cascades already carry — so a capture names the phase.
    mPccPhaseClock = std::chrono::steady_clock::now();
    // The probe GRID is (re)placed here: every probe workspace in the scene is
    // destroyed and rebuilt, which is why the monitor re-syncs its listeners
    // every frame rather than once.
    if (monitor::live())
        monitor::noteEvent(MonitorEventKind::ProbeGridBuild, monitor::reasonOf(mLastStaleReason),
                           "gi.probeGrid");
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    mPccHdr = mPccShadowed = false;
    mPccCaptureSize = 0;
    mProbesDropped = 0;

    // The slots name probes that are about to be (re)created; the first
    // updateProbeBudget after the build re-sizes and re-fills them.
    mProbeSlots.clear();
    mProbeUpdatesPerFrame = 0;
    // Our own probe workspace (media/Hlms/Jahshaka/JahshakaPcc.compositor):
    // per-face scene render + PCC depth compression + IBL specular mips — the
    // sample's LocalCubemapsProbeWorkspace, with the sample's shadow node behind
    // a quality gate (P3b) rather than always-on.
    if (!cm->hasWorkspaceDefinition("JahshakaPccProbeWorkspace")) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: PCC probe workspace missing; hybrid renders as plain VCT");
        return;
    }
    // SHADOWED CAPTURES (REFLECTIONS_ADOPTION_SPEC.md P3b). Two ways this can be
    // asked for and refused, and neither may take the hybrid down with it: the
    // shadowed workspace definition could be missing (an old staged media tree),
    // and the shadow NODE definition could be missing (a headless engine never
    // builds it — OgreEngine::createShadowNode runs only on the pixels path).
    // Ogre resolves the pass's shadow node at workspace instantiation and THROWS
    // when it is absent, so both are checked here, before the instantiation, and
    // the answer is recorded for giStatus rather than logged and forgotten.
    //
    // The node the shadowed workspace names is the PROBE node (OgreView::
    // kProbeShadowNodeName — the main layout at a quarter of the resolution,
    // four focused maps at most, an R/2 scratch cube), NOT the view's: Ogre
    // instantiates a shadow node per workspace and this workspace is
    // instantiated once per probe, which at the full atlas cost 80 MB per
    // probe (the numbers are at the constant's declaration).
    const bool wantShadows =
        resolveToggle(mGi.probeShadows, giQualityFacts(mGi.quality).probeShadowsDefault);
    mPccWorkspaceName = "JahshakaPccProbeWorkspace";
    if (wantShadows) {
        if (cm->hasWorkspaceDefinition("JahshakaPccProbeWorkspaceShadows") &&
            cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName)) {
            mPccWorkspaceName = "JahshakaPccProbeWorkspaceShadows";
            mPccShadowed = true;
        } else {
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: shadowed probe captures requested but no shadow node is "
                "defined; capturing unshadowed");
        }
    }
    if (!mGiCamera) mGiCamera = mSceneMgr->createCamera(processUniqueName("giPccCamera"));
    mGiCamera->setPosition(aabb.mCenter);

    const auto clampProbes = [](int n) { return Ogre::uint32(std::min(std::max(n, 1), 8)); };
    mPccNumProbes[0] = clampProbes(mGi.pccProbesX);
    mPccNumProbes[1] = clampProbes(mGi.pccProbesY);
    mPccNumProbes[2] = clampProbes(mGi.pccProbesZ);

    // ---- WHERE THE PROBES LIVE: ONE PHOTOGRAPH (lane R5-ROOM, 2026-09-15) --
    //
    // The probes are about to be spread through a box, and WHICH box decides
    // the picture. `aabb` is the scene's fitted LIT VOLUME — the content's union
    // plus a voxel of margin — and that is a box around the GEOMETRY, not
    // around the space inside it. The two differ by a lot in ordinary scenes,
    // because a room's floor and ceiling slabs overhang its walls and every
    // scene stands on a ground. Handing the volume to the placement is
    // measurably wrong, twice over:
    //   * gi.probe_inputs' room — an 8 x 5 x 8 interior on a 17.6 m floor, so
    //     the volume is +-9.07 — puts all four probes of a 2x1x2 grid at +-4.54,
    //     i.e. INSIDE the wall slabs, each photographing the inside of a wall.
    //     No probe's fitted shape then covers the middle of the room and the
    //     mirror there renders BLACK (measured: r 0.004 against 1.000).
    //   * gi.pcc_bounds' columned room renders 467 of its 1344 metal pixels as
    //     hard black holes with the region at the volume (+-8.66), 467 at 0.96x
    //     of it and ZERO at 0.924x — i.e. at the +-8.0 interior. Eight per cent
    //     of slack brings back the artifact clampProbeShapesToRegion documents.
    //
    // THE RULE THAT USED TO COMPUTE THE TIGHT BOX HERE is deleted, not patched:
    // it measured the scene for a ROOM — facing slabs, covering faces, an
    // enclosed-axis count — and no lighting decision may do that any more
    // (PHOTON_SPEC §13, owner+lead joint decision 2026-09-14). So the space is
    // PHOTOGRAPHED instead, by ONE probe at the centre of the scene's own box,
    // captured once at 32 px: its six averaged depth values are exactly "how
    // far is the nearest surface in each direction", `PccPerPixelGridPlacement`
    // fits its shape from them, and that shape — clamped into the volume — is
    // where the grid goes. It reads no wall, counts no axis, and knows nothing
    // about rooms; a room translated across the world measures identically
    // (gi.probe_open cases 7 and 8).
    //
    // TWO THINGS ABOUT THE READING, both measured rather than assumed:
    //
    // 1. IT IS TAKEN AS SYMMETRIC ABOUT THE VIEWPOINT — the FURTHER of the two
    //    readings on each axis sets both faces. A single viewpoint is truncated
    //    by whatever stands in front of it, and taking its raw box reproduced
    //    defect A1 exactly (the Mirror Room's free-standing panel: the region
    //    stopped at the panel's face, the parallax boxes lost the room and the
    //    mirror went black — measured on gi.pcc_bounds' A1 case and on the
    //    partitioned hall of gi.probe_open case 10, whose whole -X half was
    //    lost). The near side of a photograph can only be wrong SHORT — an
    //    occluder — while the far side saw past it, so the far reading is the
    //    one that bounds the space. It cannot over-reach: the volume clamps it.
    //
    // 2. A GRID OF VIEWPOINTS IS NOT BETTER, it is worse, and three ways of
    //    combining one were built and measured before this was written. Each
    //    probe's reading is LOCAL to it, so they cannot be pooled into a
    //    boundary: the UNION of 27 scout boxes is dragged out by the viewpoints
    //    standing outside the space (that same room read +-7.30 instead of
    //    +-3.80, because a probe on the floor's overhang sees floor in the lower
    //    half of every horizontal face); the MINIMUM is dragged across the scene
    //    by the ones beyond the far wall (their "+X boundary" sits behind the
    //    opposite wall); and the MEDIAN mixes readings that are not of the same
    //    boundary at all (measured: a -X median of +2.02 for a room spanning
    //    +-3.8). One viewpoint, from the middle, is the honest reading.
    //
    // The cost is six 32 px renders — the fit consumes ONE 1x1 AVERAGED TEXEL
    // per face, so resolution buys nothing above that — measured at 5-10 ms on
    // this box against 20-150 ms for the real placement, and paid for several
    // times over by the closing re-capture the placement no longer does (see
    // buildEnd below). A centre that opens inside something, or a scene with
    // nothing near it, gives a degenerate or unchanged box and the volume is
    // kept, which is the honest answer in both cases.
    Ogre::Aabb region = aabb;
    {
        Ogre::ParallaxCorrectedCubemapAuto scoutPcc(
            Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapAuto>(), mRoot, mSceneMgr,
            cm->getWorkspaceDefinition("JahshakaPccProbeWorkspace"));
        Ogre::PccPerPixelGridPlacement scout;
        scout.setParallaxCorrectedCubemapAuto(&scoutPcc);
        Ogre::uint32 one[3] = { 1u, 1u, 1u };
        scout.setNumProbes(one);
        scout.setFullRegion(aabb);
        scout.setOverlap(Ogre::Vector3::UNIT_SCALE);
        scout.setSnapDeviationError(Ogre::Vector3::ZERO);
        scout.setSnapSides(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
        const float scoutDiag = aabb.getSize().length();
        scout.buildStart(kProbeScoutResolution, mGiCamera, Ogre::PFG_RGBA8_UNORM_SRGB,
                         std::max(0.02f, scoutDiag * 0.001f), std::max(1.0f, scoutDiag * 2.0f));
        scout.buildEnd(false);
        if (!scoutPcc.getProbes().empty()) {
            const Ogre::Aabb saw = scoutPcc.getProbes()[0]->getProbeShape();
            const Ogre::Vector3 c = aabb.mCenter;
            Ogre::Vector3 mn = saw.getMinimum(), mx = saw.getMaximum();
            // Symmetric about the viewpoint, per axis — point 1 above.
            for (size_t ax = 0; ax < 3u; ++ax) {
                const float h = std::max(c[ax] - mn[ax], mx[ax] - c[ax]);
                mn[ax] = c[ax] - h; mx[ax] = c[ax] + h;
            }
            mn.makeCeil(aabb.getMinimum());
            mx.makeFloor(aabb.getMaximum());
            const Ogre::Vector3 size = mx - mn, whole = aabb.getSize();
            bool usable = true;
            for (size_t ax = 0; ax < 3u; ++ax)
                if (!(size[ax] > 0.05f * std::max(whole[ax], 1e-4f))) usable = false;
            if (usable) region = Ogre::Aabb::newFromExtents(mn, mx);
            if (giDebug()) {
                const auto toS = [](const Ogre::Vector3 &v) {
                    return Ogre::StringConverter::toString(v);
                };
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka GI: scout — volume " + toS(aabb.getMinimum()) + " .. " +
                    toS(aabb.getMaximum()) + " -> space " + toS(region.getMinimum()) + " .. " +
                    toS(region.getMaximum()) + (usable ? "" : " (unusable)"));
            }
        }
        scoutPcc.destroyAllProbes();
    }
    mPccPhaseMs[0] = pccPhaseSplit();
    mGiProbeRegion = region;
    mGiCamera->setPosition(region.mCenter);
    // Quality -> probe face resolution (the probe render + memory knob), and
    // the per-scene override that now sits beside it in the World panel
    // (owner, 2026-09-13 Q4: "yes halve it but add it to the world settings").
    //
    // HIGH WAS 512, WAS HALVED TO 256 ON 2026-09-13, AND IS 512 AGAIN (owner,
    // 2026-09-15, ledger §324, after the rig measured both ends of the trade on
    // the Mirror Room's chrome sphere). A probe costs 6 faces x size^2 x mips,
    // so the size is the grid's biggest memory lever: at High/HDR one probe is
    // 4.0 MiB at 256 and 16.0 MiB at 512 (REFLECTION_PROBE_AUDIT §4.2's table).
    // What the halving lost was NOT only "detail the specular mip chain blurs
    // away": mip 0 is what a near-mirror reads, and a chrome sphere in a room
    // reads it over most of its disc. The rig's measurement of the restore, on
    // that sphere: +85 % strong reflection edges for +152 MB of VRAM and
    // +2.5 ms of still-frame GPU (spikes/smoke-2026-09-15/reportB). 1024 was
    // measured too and refused: +131 % for +1.3 GB and 12.5 ms.
    //
    // WHY IT LIVES HERE AND NOT IN STUDIO'S TIER TABLE (lane SSR-2): the tier
    // table's probeSize column is 0 = "follow this dial" in every row, and a
    // scene stores the RESOLVED value it was given. Writing 512 into the High
    // and Epic rows instead would move only scenes authored after the change —
    // every document already saved carries 0, would keep rendering at 256, and
    // would additionally read as "Custom" in the World panel because its 0 no
    // longer matches the tier's 512 (worldmodes::photonDeviations). The dial is
    // the one place that reaches every scene at once, and it is where Low's 128
    // and Medium's 256 already live.
    //
    // High AND Epic take this branch: they share GiQuality::High (the tier
    // table's quality column), which is exactly the pair the decision names.
    //
    // 0 = follow the dial; anything else is the author's, clamped to a sane
    // power of two because Ogre sizes the IBL mip chain from it.
    // From the ONE tier table (`giQualityFacts`), like the cascade chain and
    // the voxel resolution: 128 at Low, 256 at Medium, 512 at High and Epic.
    Ogre::uint32 probeRes = giQualityFacts(mGi.quality).probeFaceSize;
    if (mGi.probeCaptureSize > 0) {
        unsigned want = unsigned(std::min(std::max(mGi.probeCaptureSize, 64), 1024));
        unsigned pot = 64u;
        while ((pot << 1u) <= want) pot <<= 1u;
        probeRes = pot;
    }
    // The REQUEST. What the grid ends up with is read off the bind texture
    // after it is built (below) — the two are the same number and this line is
    // the provisional one, so a failure to build reports 0 rather than a size
    // nothing has.
    mPccCaptureSize = 0;
    // HDR PROBES (P3a). The main chain renders PFG_RGBA16_FLOAT (OgreChain.cpp),
    // so an LDR probe target clamps every value above 1.0 at CAPTURE time — i.e.
    // before the IBL convolution spreads a highlight across the mip chain, which
    // is exactly the moment the extra range is worth having. Everything
    // downstream is format-generic: the DepthCompressor writes its packed depth
    // into alpha (never sRGB-encoded either way, and more precise as float), and
    // buildEnd reads it back through TextureBox::getColourAt with the bind
    // texture's own format. The cost is 2x the probe VRAM, hence the High gate.
    mPccHdr = resolveToggle(mGi.probeHdr, giQualityFacts(mGi.quality).probeHdrDefault);
    mPccProbeFormat = mPccHdr ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM_SRGB;
    const float diag = region.getSize().length();
    // ...REMEMBERED, because a shadow-atlas rebuild re-creates the probe
    // workspaces (and so their cameras) without re-placing the grid (G2).
    mProbeCamNear = std::max(0.02f, diag * 0.001f);
    mProbeCamFar  = std::max(1.0f, diag * 2.0f);
    mPccProbeRes = probeRes;
    mPccStageOk = true;
}

// STAGE 2 OF THE PROBE GRID: THE PLACEMENT AND THE JUDGEMENT. Upstream's
// PccPerPixelGridPlacement over every candidate at the scout resolution, then
// the per-probe keep/drop. 186-316 ms measured, the largest single stage.
void OgreScene::buildPccFit() {
    if (!mPccStageOk) return;
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    const Ogre::Aabb aabb = mGiStagedVolume;
    const Ogre::Aabb region = mGiProbeRegion;
    mPccPhaseClock = std::chrono::steady_clock::now();

    mPcc = new Ogre::ParallaxCorrectedCubemapAuto(
        Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapAuto>(),
        mRoot, mSceneMgr, cm->getWorkspaceDefinition(mPccWorkspaceName.c_str()));

    Ogre::PccPerPixelGridPlacement placement;
    placement.setParallaxCorrectedCubemapAuto(mPcc);
    placement.setNumProbes(mPccNumProbes);
    placement.setFullRegion(region);
    // PLACEMENT KNOBS (P3c). All three were previously left at the pin's ctor
    // defaults — overlap 1.5 by inheritance rather than by choice, and the snap
    // tolerances never touched at all. They are now OURS and set explicitly, so
    // an upstream default change shows up as a diff instead of as moved probes.
    // The overlap default is 1.25 (upstream's own sample's value): fewer probe
    // volumes over each point, which is cheaper on the Forward+ cubemap slots.
    // Its old workaround value is worth naming — before ogre-patch 0017, MORE
    // overlapping probes meant a DARKER reflection (the count division), so a
    // large overlap was quietly paying for itself in the wrong currency. With
    // 0017 the choice is purely about blend smoothness.
    placement.setOverlap(Ogre::Vector3(std::max(0.01f, mGi.probeOverlap)));
    placement.setSnapDeviationError(Ogre::Vector3(std::max(0.0f, mGi.probeSnapDeviation)));
    placement.setSnapSides(Ogre::Vector3(std::max(0.0f, mGi.probeSnapSidesMin)),
                           Ogre::Vector3(std::max(0.0f, mGi.probeSnapSidesMax)));

    const auto tPlace = std::chrono::steady_clock::now();
    // THE PLACEMENT RUNS AT THE SCOUT'S RESOLUTION (lane SKY-FALLBACK-1; the
    // debt R5-ROOM recorded). Everything buildStart/buildEnd consume is ONE
    // 1x1 AVERAGED TEXEL per cube face — the fit's six reaches and patch 0047's
    // six ratios are read from the smallest mip — so a placement capture at the
    // real probe resolution renders 256 or 512 px faces to average them down to
    // one texel each, for every CANDIDATE, including the ones about to be
    // dropped. In an open scene that is every probe but a handful. The scout a
    // few lines above has run at 32 px since R5-ROOM for exactly this reason
    // and its box agrees with the full-resolution one; this is the same
    // argument applied to the placement itself.
    //
    // AND THE GRID IS RE-CREATED BELOW AT THE REAL RESOLUTION, which is NOT
    // optional: `buildStart` calls `setEnabled(true, resolution, resolution,
    // maxNumProbes, format)` and that creates mRenderTarget, mIblTarget AND
    // mBindTexture — the cube array the SHADER samples — at whatever resolution
    // it was handed. Placing at 32 px and stopping there leaves every reflection
    // in the scene at 32 px, at every tier, silently. The re-create is also what
    // frees the array slices a dropped probe was holding: `destroyProbe` deletes
    // the object, but the array was sized at `getMaxNumProbes()` here, so the
    // dropped candidates' slices stayed allocated for the life of the grid.
    placement.buildStart(kProbeScoutResolution, mGiCamera, mPccProbeFormat,
                         mProbeCamNear, mProbeCamFar);
    // buildEnd's CLOSING RE-CAPTURE IS DEFERRED, not skipped (patch 0047's flag;
    // second read, 2026-09-15). Upstream ends the fit by re-rendering every
    // probe, and that render is NOT redundant: `processProbeDepth` re-publishes
    // each probe through `CubemapProbe::set`, which raises mDirty
    // unconditionally, and the packed depth in a probe's alpha was encoded
    // against the FULL REGION while the shader decodes it against the probe's
    // FITTED SHAPE (Cubemap_piece_all.any) — so the closing capture is exactly
    // the re-encode, and without it the Auto would also collect every
    // still-dirty probe in frameStarted and render ALL of them, unbudgeted, in
    // the next frame. What IS wasted is doing it before the probes this build
    // is about to throw away have been thrown away, and before their shapes
    // have been clamped. So it happens below instead, over the survivors, once.
    placement.buildEnd(false);   // reads probe depth back and re-fits probe shapes
    mPccPhaseMs[1] = pccPhaseSplit();
    // ---- WHICH OF THESE PROBES IS WORTH BUILDING (lane R5-ROOM) ------------
    //
    // The rule this replaced measured the SCENE — facing slabs, covering faces,
    // an enclosed-axis count — and built the whole grid or none of it. It is
    // deleted, not patched: no lighting decision may test for a room, an
    // enclosure, a wall or an axis count (PHOTON_SPEC §13, owner+lead joint
    // decision 2026-09-14). The unit is the PROBE now, and the instrument is
    // the probe itself.
    //
    // The placement has just read one averaged depth value per cube face and
    // ogre-patch 0047 hands those six numbers back: each is the distance that
    // face could see as a multiple of the distance from this probe's camera to
    // the region's face in the same direction: 1 means "on the region's face",
    // and 2 is the encoding's SATURATION — the compressor stores
    // min(0.5 * dist / approxDist, 1) and the decode doubles it, so 2 means
    // "nothing within twice that distance", which is what a face full of sky
    // returns and is the largest value there is. From them the box follows
    // (the placement's own arithmetic): on each axis it reaches
    //     (H - cam) * ratio(+face) + (H + cam) * ratio(-face)
    // with H the region's half size and cam the probe's camera in the region's
    // frame. That is a LENGTH IN WORLD UNITS on each axis, and dividing the
    // three by the extents of THE VOLUME THE RENDERER LIT gives the probe's box
    // as a fraction of that world: their product is its VOLUME RATIO. Below 1
    // this probe photographed a space materially smaller than the world it
    // stands in — something is near it — and at 1 or above it saw nothing that
    // the world does not already hold.
    //
    // THE READING IS RELATIVE TO THE BOX THE PROBE WAS PLACED IN, which is the
    // scene's own fitted volume, and that is what makes it scale-free: the same
    // room measures the same at any size, anywhere in the world, in any units.
    // It is NOT independent of that box — a scene whose author pins very tight
    // bounds is telling the renderer that its whole world is that box, and a
    // probe in it then has less "inside the world" left to see. Measured, with
    // the same roofless 10 m room: at the automatic +-7.4 volume the grid is
    // kept (worst span 0.53), with bounds pinned at +-5.5 it is dropped (1.37)
    // — the walls of a roofless room subtend less of a probe's view than a
    // first reading suggests, and most of what those probes see is sky.
    //
    // It is per probe, positionless, scale-free, and costs no capture of its
    // own: the measurement is a by-product of the placement that already ran.
    // It is read BEFORE `clampProbeShapesToRegion`, and that ordering is the
    // whole measurement — the A2 clamp pins an unshrunk box back to exactly the
    // region, and upstream's padding and its two snaps do the same for anything
    // near it, so the fitted SHAPE cannot tell "saw a wall just inside the
    // region" from "saw nothing and was snapped back to it". The ratios can.
    {
        const Ogre::FastArray<float> &ratios = placement.getProbeDepthRatios();
        const Ogre::CubemapProbeVec &built = mPcc->getProbes();
        const Ogre::Vector3 H = region.mHalfSize, W = aabb.getSize();
        const bool debugFit = giDebug();
        std::vector<Ogre::CubemapProbe *> drop;
        for (size_t i = 0; i < built.size(); ++i) {
            if ((i + 1u) * 6u > ratios.size()) break;      // no reading: keep it
            const float *r = &ratios[i * 6u];
            const Ogre::Vector3 cam = built[i]->getProbeCameraPos() - region.mCenter;
            float spanVol = 1.0f;
            float span[3];
            for (size_t ax = 0; ax < 3u; ++ax) {
                // CubemapSide order is PX, NX, PY, NY, PZ, NZ — the positive
                // face of axis `ax` is 2*ax, the negative 2*ax+1.
                const float reach = (H[ax] - cam[ax]) * r[ax * 2u] +
                                    (H[ax] + cam[ax]) * r[ax * 2u + 1u];
                span[ax] = reach / std::max(W[ax], 1e-6f);
                spanVol *= span[ax];
            }
            // THE PRODUCT OF THE THREE, i.e. the box against the world BY
            // VOLUME, and the alternative was built and measured twice — once
            // when this was written, once again by lane SKY-FALLBACK-1 with
            // both forms switchable on one binary. Taking the SMALLEST of the
            // three instead — "smaller on any one axis" — keeps every probe
            // that stands near a FLOOR, because every scene has one, it fills
            // the lower half of every probe's view and it shrinks exactly one
            // axis. A volume answers "is the space this probe measured smaller
            // than the world the renderer lit?" in all three directions at
            // once: a floor alone does not make it so.
            //
            // WHAT SKY-FALLBACK-1 MEASURED, and it closes half of the original
            // argument. That argument had two halves, and the SECOND is gone:
            // a scene that gains a grid no longer loses the sky, because the
            // sky has its own pass-level slot now (ogre-patch 0048) and answers
            // wherever no probe's box does. What it does NOT close is the
            // avatar preview reading r3 g3 b4 under the any-axis form: that was
            // never the missing sky. Measured on this binary, it is upstream's
            // one-occupant trap firing in a SECOND scene — the PCC binding is
            // PROCESS-WIDE while the sky cube's binding is per scene and per
            // material, so a preview scene's datablocks keep their manual cube
            // while the editor scene's grid owns the env slot, and the pixel
            // shader that generates does not compile (`SampleEnvProbe` against
            // a textureCubeArray; OgreSky.cpp's note lists the three ways).
            // The character is black because there is no shader, not because
            // there is no sky. That defect is independent of which form of this
            // rule ships and is recorded for its own lane.
            //
            // The counts, both forms, same binary, with 0048 in: gi.probe_open
            // is IDENTICAL on every case but the 30 m yard (0 vs 3 of 18 at the
            // shipped grid, 4 vs 8 of 32 at the denser one); LocalCubemaps is
            // 3 of 3 kept either way with identical pixels; a new project with
            // a cube and a mirror sphere keeps 2 of 18 by volume and 4 of 18 by
            // axis, and the mirror's pixels are IDENTICAL between them (229 216
            // 230) because the sky now answers either way. So the two forms
            // differ only in how many probes a nearly-open scene PAYS for, and
            // the volume form pays less for the same picture.
            const bool keep = spanVol < kProbeSeesGeometry;
            if (!keep) drop.push_back(built[i]);
            if (debugFit)
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka GI:  probe " + std::to_string(i) + (keep ? " KEPT" : " DROPPED") +
                    " — faces " + Ogre::StringConverter::toString(r[0]) + " " +
                    Ogre::StringConverter::toString(r[1]) + " " +
                    Ogre::StringConverter::toString(r[2]) + " " +
                    Ogre::StringConverter::toString(r[3]) + " " +
                    Ogre::StringConverter::toString(r[4]) + " " +
                    Ogre::StringConverter::toString(r[5]) + ", spans " +
                    Ogre::StringConverter::toString(span[0]) + " " +
                    Ogre::StringConverter::toString(span[1]) + " " +
                    Ogre::StringConverter::toString(span[2]) + " vol " +
                    Ogre::StringConverter::toString(spanVol) + " (keep below " +
                    Ogre::StringConverter::toString(kProbeSeesGeometry) + ")");
        }
        mProbesDropped = int(drop.size());
        for (Ogre::CubemapProbe *p : drop) mPcc->destroyProbe(p);
        if (mPcc->getProbes().empty()) {
            // NOTHING TO PHOTOGRAPH. No grid, and the sky cubemap goes back onto
            // every datablock — `reflectionTexForDatablocks` hands it back the
            // moment mPcc is null (OgreSky.cpp), and the caller runs
            // applyReflectionToAll right after this. Cheaper and sharper than a
            // grid of photographs of the sky, which is the owner's rule
            // (2026-09-13 Q3) measured per probe instead of per scene.
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: no probe grid — all " + std::to_string(mProbesDropped) +
                " probes photographed nothing inside the lit volume, so reflections come "
                "from the sky and cone tracing");
            mPccPhaseMs[2] = pccPhaseSplit();
            notePlacementPhases(mPccPhaseMs[0], mPccPhaseMs[1], mPccPhaseMs[2], 0.0, 0.0,
                                unsigned(mProbesDropped), 0u);
            if (giDebug())
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka GI: probe placement " +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - tPlace).count()) +
                    " ms — " + std::to_string(mProbesDropped) + " candidates at " +
                    std::to_string(kProbeScoutResolution) + " px, 0 kept — scout " +
                    std::to_string(mPccPhaseMs[0]) + " place " + std::to_string(mPccPhaseMs[1]) +
                    " drop " + std::to_string(mPccPhaseMs[2]) + " ms");
            destroyProbeGrid();
            return;
        }
        if (mProbesDropped)
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: " + std::to_string(mProbesDropped) + " of " +
                std::to_string(mProbesDropped + int(mPcc->getProbes().size())) +
                " probes saw nothing inside the lit volume and were dropped");
    }
    // THE SURVIVORS ARE QUIET UNTIL STAGE 3 BINDS THEM (fix round item 3).
    //
    // `buildEnd` re-publishes every probe through `CubemapProbe::set`, which
    // raises `mDirty` unconditionally, and a probe is born with upstream's
    // `mNumIterations` of 8. When the three stages ran back to back that state
    // lived for microseconds; STAGED, it survives to the end of the frame, and
    // `ParallaxCorrectedCubemapAuto::frameStarted` then collects every dirty
    // probe and renders all of them — 32 probes x 6 faces of full scene passes
    // — at the SCOUT's 32 px, for a grid that stage 3 is about to re-create at
    // the tier's size and capture properly. Pure waste, inside the "fit"
    // number.
    //
    // Cleared here rather than paused: `latchProbeCaptures` writes `mPaused`
    // every frame from "is anyone looking at this scene", so a pause set here
    // would be undone before the frame ends. Stage 3's own `set()` raises
    // `mDirty` again for the one capture that is not waste.
    for (Ogre::CubemapProbe *p : mPcc->getProbes()) {
        p->mNumIterations = 1u;
        p->mDirty = false;
    }
    // ...AND THE SURVIVORS' SHAPES ARE CLAMPED, and then the grid is RE-CREATED
    // at the kept count and the REAL resolution (lane SKY-FALLBACK-1, second
    // read). Both halves of that sentence are load-bearing:
    //
    //   * THE RESOLUTION. `PccPerPixelGridPlacement::buildStart` calls
    //     `mPcc->setEnabled(true, resolution, resolution, maxNumProbes, format)`
    //     and `ParallaxCorrectedCubemapAuto::setEnabled` creates mRenderTarget,
    //     mIblTarget AND mBindTexture — the cube array the SHADER samples — at
    //     that size. Placing at the scout's 32 px without re-creating therefore
    //     leaves every probe reflection in the scene at 32 px, silently, at
    //     every tier: a defect this lane shipped for one round and the second
    //     read caught. The placement wants 32 px (it consumes one 1x1 averaged
    //     texel per face); the PICTURE wants the tier's size; so the grid is
    //     built twice, small then right.
    //   * THE COUNT. `setEnabled` sizes the array at `getMaxNumProbes()`, the
    //     CANDIDATE count, and `destroyProbe` frees only the object — so an
    //     open scene keeping 2 of 18 held 18 slices for the life of the grid
    //     (~300 MB at High/HDR 512). Re-creating at the kept count is what
    //     releases them.
    //
    // setEnabled(false) keeps the CubemapProbe objects and their published
    // geometry — it destroys their workspaces, releases their array slices and
    // drops the three textures — so the survivors are re-armed in place:
    // initWorkspace re-acquires a slice in the new, smaller array and set()
    // re-publishes the same camera, area and shape into the internal probe the
    // acquisition just re-created.
    //
    // AND THE RE-PUBLISH IS EXACT, which needed ogre-patch 0049 (SOURCE):
    // `CubemapProbe::set` applied its 1.005 padding on EVERY call, so handing a
    // probe back its own boxes grew them half a percent — measured here as the
    // probe union leaving the region (gi.pcc_bounds' A2 invariant) and
    // gi.budget's paused re-capture reading (g-r) +0.259 where it reads +0.380.
    // The patch adds `bValuesAlreadyPadded`, which every existing caller
    // defaults to false; this is the one caller that passes true, because these
    // are the probe's OWN boxes coming back. The clamp therefore still runs
    // BEFORE this block, exactly where it always did, and the geometry the
    // shader sees is bit-for-bit what it was.
    mPccPhaseMs[2] = pccPhaseSplit();
}

// STAGE 3 OF THE PROBE GRID: THE GRID THE SHADER SAMPLES. The survivors'
// shapes clamped, the grid re-created at the real resolution, and one closing
// capture each. 288 ms measured on a 32-probe grid at 512 px.
void OgreScene::buildPccFinish() {
    if (!mPccStageOk || !mPcc) return;
    const Ogre::Aabb region = mGiProbeRegion;
    const Ogre::uint32 probeRes = mPccProbeRes;
    const Ogre::PixelFormatGpu probeFormat = mPccProbeFormat;
    const float diag = region.getSize().length();
    mPccPhaseClock = std::chrono::steady_clock::now();
    clampProbeShapesToRegion(region);
    {
        const Ogre::CubemapProbeVec &kept = mPcc->getProbes();
        const Ogre::uint32 keptCount = Ogre::uint32(kept.size());
        mPcc->setEnabled(false, probeRes, probeRes, keptCount, probeFormat);
        mPcc->setEnabled(true, probeRes, probeRes, keptCount, probeFormat);
        for (Ogre::CubemapProbe *p : kept) {
            const Ogre::Vector3 cam    = p->getProbeCameraPos();
            const Ogre::Aabb    area   = p->getArea();
            const Ogre::Vector3 inner  = p->getAreaInnerRegion();
            const Ogre::Matrix3 orient = p->getOrientation();
            const Ogre::Aabb    shape  = p->getProbeShape();
            p->initWorkspace(mProbeCamNear, mProbeCamFar);
            p->set(cam, area, inner, orient, shape, /*bValuesAlreadyPadded*/ true);
        }
    }
    // THE CAPTURE SIZE IS READ FROM THE TEXTURE, never from the local that asked
    // for it (second read): the local said 512 while the array was 32 for a
    // round, and every probe assertion in the suites is a hue check that cannot
    // see the difference. gi.probe_open asserts this against the tier's size.
    mPccCaptureSize = mPcc->getBindTexture() ? int(mPcc->getBindTexture()->getWidth())
                                             : int(probeRes);
    mPccPhaseMs[3] = pccPhaseSplit();
    mPcc->updateAllDirtyProbes();
    mPccPhaseMs[4] = pccPhaseSplit();
    notePlacementPhases(mPccPhaseMs[0], mPccPhaseMs[1], mPccPhaseMs[2], mPccPhaseMs[3],
                        mPccPhaseMs[4],
                        unsigned(mProbesDropped + int(mPcc->getProbes().size())),
                        unsigned(mPcc->getProbes().size()));
    // EVERY probe renders in the INLINE stage from now on (B2 point 2). Set once,
    // here, rather than flipped as probes come and go: in automatic mode this
    // selects a render stage, not an amount of work, and the budget already
    // decides how many probes render at all.
    for (Ogre::CubemapProbe *p : mPcc->getProbes()) p->mNumIterations = 1u;
    // ONE CAPTURE PER CANDIDATE AT THE SCOUT'S 32 px, PLUS ONE PER SURVIVOR AT
    // THE REAL RESOLUTION (lane SKY-FALLBACK-1; it was two per CANDIDATE at the
    // real resolution). Counted as captures either way so a rebuild frame
    // reports what it cost (GiStatus::probeCapturesLastFrame) — but they are not
    // the same size: a scout face is 1/64 of a 256 px one and 1/256 of a 512 px
    // one by pixel count, so the placement's share of a rebuild is now
    // negligible beside the survivors' one real capture each.
    mPlacementCapturesThisFrame += int(mProbesDropped + int(mPcc->getProbes().size()) +
                                       int(mPcc->getProbes().size()));
    // ...and every probe is STALE all the same: the placement captured before
    // the grid was bound to HlmsPbs and before this build's irradiance field
    // existed, so those captures show neither probe reflections nor the DDGI
    // bounce. The budget re-captures each once, over the next frames.
    mProbeSlots.assign(mPcc->getProbes().size(), ProbeSlot());
    staleProbeGrid(GiStaleReason::Rebuild);
    // THE FORWARD+ PER-CELL PROBE BUDGET MUST HOLD THIS GRID
    // (EnginePrivate.h kCubemapProbeSlotsDefault has the measurement and the
    // upstream anchor). Done HERE, right after the probes exist and before the
    // first frame that shades through them, because a cell that overflows
    // silently drops probes and paints black rectangles on every reflective
    // surface it covers.
    ensureCubemapProbeSlots(mPcc->getProbes().size());

    // Diagnostic: JAHSHAKA_GI_DEBUG=1 dumps where every probe ENDED UP. This is
    // the only window onto PccPerPixelGridPlacement's depth-readback shrink-fit,
    // and the shape is what decides whether a surface gets probe reflections at
    // all: the PBS per-pixel path skips any probe whose SHAPE does not contain
    // the shaded point (getProbeFade > 0, ForwardPlus_DecalsCubemaps_piece_ps),
    // so a shape that lost the room reads downstream as "reflections are black"
    // with probeCount and pccBound both still healthy.
    if (giDebug()) {
        // THE PLACEMENT'S WALL COST, which is the number lane SKY-FALLBACK-1
        // moved: it covers the scout-resolution placement, the drop, the clamp,
        // the re-create at the real resolution and the survivors' single
        // capture. It was previously two full-resolution captures of every
        // CANDIDATE.
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: probe placement " +
            std::to_string(mPccPhaseMs[0] + mPccPhaseMs[1] + mPccPhaseMs[2] +
                           mPccPhaseMs[3] + mPccPhaseMs[4]) +
            " ms — " + std::to_string(mProbesDropped + int(mPcc->getProbes().size())) +
            " candidates at " + std::to_string(kProbeScoutResolution) + " px, " +
            std::to_string(mPcc->getProbes().size()) + " kept at " +
            std::to_string(probeRes) + " px — scout " + std::to_string(mPccPhaseMs[0]) +
            " place " + std::to_string(mPccPhaseMs[1]) + " drop " + std::to_string(mPccPhaseMs[2]) +
            " recreate " + std::to_string(mPccPhaseMs[3]) + " capture " +
            std::to_string(mPccPhaseMs[4]) + " ms");
        Ogre::LogManager &lm = Ogre::LogManager::getSingleton();
        const auto toS = [](const Ogre::Vector3 &v) {
            return Ogre::StringConverter::toString(v);
        };
        lm.logMessage("Jahshaka GI: PCC region " + toS(region.getMinimum()) + " .. " +
                      toS(region.getMaximum()) + " grid " + std::to_string(mPccNumProbes[0]) + "x" +
                      std::to_string(mPccNumProbes[1]) + "x" +
                      std::to_string(mPccNumProbes[2]) +
                      " res " + std::to_string(probeRes) +
                      (mPccHdr ? " RGBA16F" : " RGBA8_SRGB") +
                      (mPccShadowed ? " shadowed" : " unshadowed") +
                      " overlap " + Ogre::StringConverter::toString(mGi.probeOverlap));
        const Ogre::CubemapProbeVec &probes = mPcc->getProbes();
        for (size_t i = 0; i < probes.size(); ++i) {
            const Ogre::CubemapProbe *p = probes[i];
            const Ogre::Aabb shape = p->getProbeShape();
            const Ogre::Aabb area  = p->getArea();
            lm.logMessage("Jahshaka GI:  probe " + std::to_string(i) +
                          " cam " + toS(p->getProbeCameraPos()) +
                          " shape " + toS(shape.getMinimum()) + " .. " + toS(shape.getMaximum()) +
                          " area " + toS(area.getMinimum()) + " .. " + toS(area.getMaximum()));
        }
    }

    // The hybrid blend: reflections whose PCC-vs-VCT parallax error is below
    // minDistance come from the probes (near geometry, sharp), above maxDistance
    // from cone tracing (far, soft), faded in between. Scaled to the PROBE
    // REGION now that the region and the lit volume are two different boxes:
    // what the shader measures (getPccVctBlendWeight -> distToVct) is a
    // disagreement between two answers about the space the probes cover, so
    // that space is the right thing to scale it by — not a voxel volume that
    // may be padded out well past it.
    //
    // MEASURED LIMIT, left as a finding rather than tuned away: when the LIT
    // VOLUME is padded to roughly twice the room (a user typing very generous
    // bounds rows — the auto path cannot produce it any more), the VCT cone hit
    // drifts 16+ voxels from the probe's parallax hit and this blend correctly
    // concludes the two disagree, so probe reflections fade out even though the
    // probes themselves are placed perfectly. Widening the window to ~32 voxels
    // restores them, but that is the hybrid abandoning its own judgement, so it
    // is NOT the default. gi.pcc_bounds' header records the numbers.
    float minDist = std::max(0.25f, diag * 0.05f);
    // ...AND THE TEST IS INVERTED WHEN THE PROBES ARE LIVE (P5a). This is the
    // finding of the dynamic-probe half, and it is not a tuning choice — the
    // feature does not work at all without it.
    //
    // The blend above asks "do the probe and the voxel volume agree about where
    // this reflection lands?", and hands the pixel to VCT when they do not. That
    // question assumes both halves are equally current, which is true for a
    // STATIC grid: both were built from the same scene at the same moment. A
    // DYNAMIC probe breaks the assumption on purpose. It re-captures the scene
    // every frame; the voxel volume is only re-voxelized by a full re-solve. So
    // the instant an object MOVES — which is the only reason to turn dynamic
    // probes on — the probe knows and the voxels do not, the two disagree
    // exactly where the movement is, and the disagreement is settled in favour
    // of the half that is out of date. In a sealed room VCT's specular answer is
    // black, so the mirror does not merely lag: it goes BLACK precisely where the
    // moving object should appear.
    //
    // MEASURED IN-LANE (gi.dynamic_probes' room; a green slab sliding across the
    // red wall the mirror reflects; four live probes; the probe grid, the shapes
    // and the geometry all identical, ONLY this window changing):
    //     window x1 (shipped static value)   mirror r 0.000 g 0.000   <- black
    //     window x2, x3, x4, x8, x20         mirror r 0.059 g 1.000   <- the slab
    // and with the probes STATIC the same slide leaves the mirror at r 1.000
    // g 0.059, i.e. showing the wall the slab is now covering. The three
    // readings are the whole argument: static is stale, dynamic-with-the-old-
    // window is broken, dynamic-with-this-window is right.
    //
    // So when any probe is live the window becomes the probe region's own
    // diagonal, which makes the test always pass INSIDE the region: a probe that
    // covers the pixel wins, and VCT keeps everything the probes do not cover
    // (probeFade is 0 there, so nothing changes outside). The rule reads as one
    // sentence — "the fresher of the two answers wins, and with dynamic probes
    // on that is always the probe" — which is exactly the judgement the static
    // case makes in the other direction. Note the sweep above found NO gradient
    // between x2 and x20: the disagreement is either inside the window or it is
    // not, so there is no honest number to tune between them and the principled
    // extreme is the one to take.
    //
    // WHAT IT COSTS, stated rather than hidden: rough surfaces inside the region
    // take their environment from the probes instead of from cone tracing
    // whenever the budget is live, which is every hybrid scene that has not
    // paused GI (measured on the same room's floor pixel: 0.000 ->
    // 0.059/1.000/0.059 when the inversion first came on). The mirror-sharp
    // pixels do not move.
    //
    // THE REFINEMENT THIS IS NOT (recorded, not built — it needs upstream): the
    // honest version of this rule is PER PROBE, since only the recently
    // re-captured probes are fresher than the voxels. `pccVctMinDistance` is a
    // single pass constant, and making it per-probe means a new field in the
    // probe const buffer plus the shader that reads it — an upstream change on
    // both sides, which §9 of the spec makes a stop-and-report rather than a
    // lane decision.
    //
    // WHAT THE FIX WAVE CHANGED HERE (B1/B2): the gate used to be the retired
    // `dynamicProbes > 0` knob, which defaulted to 0, so this inversion was
    // opt-in and almost nobody saw it. The realtime budget model defaults to 1,
    // so it is now the SHIPPED look for every hybrid scene that has not paused
    // GI — and that is stated loudly here, in the panel's tooltip and in the
    // spec rather than discovered later. The freshness gap it papers over is
    // narrowed by the probe cache's invalidations (a change re-captures the
    // probes that can see it, and the lights are re-injected on the cheap
    // cadence), and the principled per-probe fix is still upstream's to make.
    if (mGi.updateBudget > 0) minDist = std::max(minDist, diag);
    mPccBindMinDist = minDist;
    mPccBindMaxDist = minDist * 2.0f;
    hlmsPbs(mRoot)->setParallaxCorrectedCubemap(mPcc, mPccBindMinDist, mPccBindMaxDist);
    // ...and EVERY scene's datablocks must drop their manual cubemap now, not
    // just this one's: the property that makes texEnvProbeMap a cube array is
    // set for every pass in the process (reflectionTexForDatablocks' note). A
    // second scene that kept its sky cube here generated a shader that does not
    // compile — the avatar preview's black character.
    if (mEngine) mEngine->reapplyReflectionsAllScenes();
}

// THE PAGE-RETURN BINDING (ENGINE_CACHE_POLICY_SPEC P10). What a scene coming
// back on screen needs, and ALL it needs: its arms were built against its own
// geometry and nothing about a page switch invalidates them — only the
// process-wide HlmsPbs pointers can have been taken over by another scene's
// build (the player page's own GI, OgreGi.cpp's "last enabler wins").
//
// Each of the three is set to THIS scene's arm, including null for an arm the
// scene does not have: a scene without probes must not be shaded through the
// probes of the scene that last built some (the old re-push could not fix that
// case at all — its teardown only unbinds when it is the owner).
bool OgreScene::reassertGiBinding() {
    JAH_TRY {
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        // THE BOUND POINTERS DECIDE, NOT THE OWNER FLAG (code review 2026-09-12):
        // the flag says who bound last, the pointers say what the shader reads,
        // and any path that let the two disagree left this scene "owning" a
        // binding that pointed into another scene's arms — a use-after-free
        // the moment that scene died. Already ours on all three: a no-op.
        if (pbs->getVctLighting() == mVctLighting &&
            pbs->getParallaxCorrectedCubemap() == mPcc &&
            pbs->getIrradianceField() == mIfd) {
            sVctBindingOwner = (mVctLighting || mPcc || mIfd) ? this : sVctBindingOwner;
            return false;
        }
        pbs->setVctLighting(mVctLighting);
        const bool pccBindingMoved = pbs->getParallaxCorrectedCubemap() != mPcc;
        if (mPcc) pbs->setParallaxCorrectedCubemap(mPcc, mPccBindMinDist, mPccBindMaxDist);
        else      pbs->setParallaxCorrectedCubemap(nullptr);
        // Process-wide, so every scene re-decides (see rebuildVct's call).
        if (pccBindingMoved && mEngine) mEngine->reapplyReflectionsAllScenes();
        pbs->setIrradianceField(mIfd);
        const bool owns = mVctLighting || mPcc || mIfd;
        sVctBindingOwner = owns ? this : nullptr;
        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                std::string("Jahshaka GI: binding re-asserted (") +
                (mVctLighting ? "vct " : "") + (mPcc ? "pcc " : "") + (mIfd ? "ifd" : "") +
                (owns ? ")" : "nothing — unbound)"));
        return true;
    } JAH_CATCH(mError, false);
}

// ===========================================================================
// DDGI — Ogre's IrradianceField as the diffuse GI layer (GI_UNIFIED_SPEC.md
// §4 P1; the P0 spike that gates it: spikes/ddgi-vulkan/FINDINGS.md).
//
// WHAT IT IS. Majercik et al.'s Dynamic Diffuse GI: a grid of probes, each
// storing octahedrally-mapped irradiance plus a depth-variance map used as a
// Chebyshev visibility test. A pixel reads the eight probes of its cell, and
// the depth test is what stops a probe on the far side of a wall from lighting
// it — the leak the cone-traced diffuse term could not avoid. Ours is the
// VCT-FED path: the probes are integrated by cone-tracing the voxel volume
// VctLighting already lit, so DDGI costs no second scene representation.
//
// WHAT IT REPLACES, and this is the fact to carry: binding a field makes
// HlmsPbs set `VctDisableDiffuse` (OgreHlmsPbs.cpp:1784-1788). The field does
// not ADD to the voxel-cone diffuse — it TAKES OVER from it. Measured on the
// spike's closed room, the pure-indirect term goes from a mean 84/54/56 (VCT,
// with a blown-out 1.0 in the dark corner where it leaks) to 6.4/3.3/4.0
// (DDGI, smooth and plausible): the right SHAPE roughly 13x too dim, because
// upstream never scales it. `GiParams::ddgiIntensity` is our answer, applied in
// media/Hlms/Jahshaka/JahIfd_piece_ps.any.
//
// WHERE IT LIVES IN THE LIFECYCLE. Inside the VCT arm and strictly within
// VctLighting's lifetime: the field holds that pointer and binds its voxel
// textures on every update. So it is built at the end of a VCT (re)build, it
// dies FIRST in teardownVct, and the only thing that may re-use an existing
// field is the light-only cheap path (`refreshGiLighting`), which leaves the
// VctLighting object in place and merely re-injects — upstream's `reset()`
// case exactly. `reset()` does not clear the atlases (spike §4 methodology),
// which is a FEATURE here: a re-converge runs progressively over the previous
// converged data, so a light drag never flashes the room black.
// ===========================================================================

bool OgreScene::ddgiWanted() const {
    // Fed by VctLighting: there is nothing to build without a voxel volume.
    if (mGi.mode != GiMode::Vct && mGi.mode != GiMode::VctPccHybrid) return false;
    // Auto = "the quality tier decides", and the tier is resolved DOCUMENT-SIDE
    // (GI_UNIFIED P2: the Photon tier writes a concrete 0/1 through into
    // Scene::giDdgi, the same write-through every other tiered field uses), so
    // what reaches the engine as Auto is a scene no tier has ever touched —
    // OFF, which is what keeps every already-serialized scene rendering exactly
    // as it did before P1 landed.
    return resolveToggle(mGi.ddgi, false);
}

void OgreScene::ifdProbeCounts(const Ogre::Vector3 &size, Ogre::uint32 outCounts[3]) {
    // THE FIT (spike §9b). Upstream's default is a fixed 32x8x32, which over a
    // room-shaped volume leaves the Y probes at 1.13 m apart against 0.56 m on
    // X and Z — and the coarse axis BANDS, visibly, in the indirect term. The
    // grid must therefore come from the volume's aspect, not from a constant.
    //
    // Every axis count must be a power of two: upstream only OGRE_ASSERT_LOWs
    // it (OgreIrradianceField.h:96) and that assert is compiled out of our
    // release-built engine, and `getDepthProbeFullResolution` additionally
    // assumes the TOTAL is one (it takes ctz32 of it). So the fit hands out the
    // total's 13 doublings one at a time, each to whichever axis currently has
    // the coarsest spacing: the greedy "make the three spacings as equal as
    // possible" solution, and a power of two per axis by construction.
    //
    // Every axis starts at 2, never 1: the shader reads a CAGE of eight probes
    // (grid position, then +1 on each axis), so a single-probe axis would index
    // past the field's own row. And no axis may exceed 128, which stops a
    // pathologically flat volume from collapsing the grid into a line.
    float extent[3] = { std::max(std::fabs(size.x), 1e-4f),
                        std::max(std::fabs(size.y), 1e-4f),
                        std::max(std::fabs(size.z), 1e-4f) };
    Ogre::uint32 n[3] = { 2u, 2u, 2u };
    Ogre::uint32 spent = 3u;                     // the three seed doublings
    const Ogre::uint32 doublings = Ogre::Bitwise::ctz32(kIfdTotalProbes);   // 13 at 8192
    const Ogre::uint32 kMaxPerAxis = 128u;
    // TIE-BREAK ORDER X, Z, Y, and it decides the shape of every room-shaped
    // volume. 13 doublings across 3 axes cannot come out even, so ONE axis
    // always ends up at double the others' spacing; this chooses which. On a
    // wide, shallow room (18 x 9 x 18) plain left-to-right order gives
    // 32x16x16 — X and Z have the SAME extent and get different densities,
    // which bands across depth. Preferring the horizontal axes on a tie gives
    // 32x8x32: symmetric in the plane the viewer moves through, with the coarse
    // axis vertical, where indirect light varies least (and which is the shape
    // upstream's own default picked). A genuinely tall volume still wins Y the
    // doublings on merit — 9 x 18 x 9 fits 16x32x16, spacing equal on all three.
    static const int kAxisOrder[3] = { 0, 2, 1 };
    while (spent < doublings) {
        int best = -1;
        float worstSpacing = -1.0f;
        for (int i = 0; i < 3; ++i) {
            const int ax = kAxisOrder[i];
            if (n[ax] >= kMaxPerAxis) continue;
            const float spacing = extent[ax] / float(n[ax]);
            if (spacing > worstSpacing) { worstSpacing = spacing; best = ax; }
        }
        if (best < 0) break;                     // every axis capped (cannot happen at 8192)
        n[best] *= 2u;
        ++spent;
    }
    outCounts[0] = n[0]; outCounts[1] = n[1]; outCounts[2] = n[2];
}

Ogre::uint32 OgreScene::ifdProbesPerFrame(const Ogre::IrradianceFieldSettings &settings,
                                          int updateBudget, Ogre::uint32 totalProbes) {
    // Paused is paused (GiParams::updateBudget == 0): no re-converge, and — the
    // reason this returns 0 rather than 1 — update() is then never called at
    // all, which is the only way to be sure a zero-work-group dispatch cannot
    // happen on the paused path.
    if (updateBudget <= 0 || totalProbes == 0u) return 0u;

    // ONE WORK GROUP PER PROBE (PHOTON-FIELD-ROTATE-1): the generation job's
    // dispatch is the batch's probe count, so every batch of one or more probes
    // dispatches (upstream's ray-count arithmetic could dispatch zero groups and
    // throw at frame time; that floor is gone with it).
    (void)settings;

    // The budget's meaning here: at budget 1 the field re-converges in
    // kIfdConvergeFrames frames, and the cost scales linearly with the dial
    // like every other GI budget. Rounded UP to a power of two so the batch
    // always divides a power-of-two field exactly (the last batch is the same
    // size as every other one). One pass over the field is ONE sample per probe;
    // the refinements the field owes after it (kIfdTargetSamples) run at the
    // same rate.
    Ogre::uint64 desired =
        (Ogre::uint64(totalProbes) * Ogre::uint64(updateBudget) + kIfdConvergeFrames - 1u) /
        kIfdConvergeFrames;
    Ogre::uint32 ppf = 1u;
    while (Ogre::uint64(ppf) < desired && ppf < totalProbes) ppf <<= 1u;
    if (ppf > totalProbes) ppf = totalProbes;
    return ppf;
}

void OgreScene::buildIrradianceField() {
    // Called on every VCT (re)build, including the ones that must DROP the
    // field (the toggle went off, the mode left VCT, the arm failed to build).
    if (!ddgiWanted() || !mVctLighting || !mVctVoxelizer) { teardownIrradianceField(); return; }

    JAH_TRY {
        Ogre::IrradianceFieldSettings settings;
        // `JAHSHAKA_GI_FIELD_RAYS` (rays per depth texel), `_SAMPLES` (the target
        // sample count) and `_STATIC` (no rotation) are MEASUREMENT switches, like
        // `JAHSHAKA_GI_FIELD_NO_SCROLL`: gi.field_alias drives the arms in one process.
        if (const char *r = std::getenv("JAHSHAKA_GI_FIELD_RAYS")) {
            const int n = std::atoi(r);
            if (n >= 1 && n <= 7) settings.mNumRaysPerPixel = Ogre::uint16(n);   // <= 1024 rays a probe
        }
        Ogre::uint32 targetSamples = kIfdTargetSamples;
        if (const char *k = std::getenv("JAHSHAKA_GI_FIELD_SAMPLES")) {
            const int n = std::atoi(k);
            if (n >= 1 && n <= 4096) targetSamples = Ogre::uint32(n);
        }
        const bool rotateRays = std::getenv("JAHSHAKA_GI_FIELD_STATIC") == nullptr;
        settings.mDepthProbeResolution   = kIfdDepthRes;
        settings.mIrradianceResolution   = kIfdIrradRes;
        // THE VOLUME IS THE VOXEL VOLUME THE FIELD READS FROM — whichever arm
        // built it. In the single-volume arm that is the scene's fitted box; in
        // the cascade chain it is cascade 0's camera-centred box (the chain's
        // rule 5: cascade 0 IS mVctVoxelizer/mVctLighting), and the scheduler
        // keeps it there as that cascade scrolls (followCascade0Field). A
        // single volume small enough to follow the camera would delete the
        // far bounce (REFLECTIONS P5b measured it); under a chain the outer
        // cascades hold the far field and hand the ring its bounce (G3), and
        // gi.cascades' 20 m pixel is the bar that says so.
        const Ogre::Vector3 origin = mVctVoxelizer->getVoxelOrigin();
        const Ogre::Vector3 size   = mVctVoxelizer->getVoxelSize();
        ifdProbeCounts(size, settings.mNumProbes);

        // HOST-SIDE VALIDATION. settings.testValidity() is three OGRE_ASSERT_LOWs
        // and our Ogre is built with them off, so the checks it would have made
        // are made here — where a violation logs and declines instead of
        // producing a mis-sized atlas nobody can explain later.
        bool valid = settings.mIrradianceResolution <= settings.mDepthProbeResolution &&
                     (settings.mDepthProbeResolution % settings.mIrradianceResolution) == 0u;
        Ogre::uint32 total = 1u;
        for (size_t i = 0; i < 3u; ++i) {
            const Ogre::uint32 c = settings.mNumProbes[i];
            if (!c || (c & (c - 1u)) != 0u) valid = false;
            total *= c;
        }
        if (!valid || !total || (total & (total - 1u)) != 0u) {
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: DDGI declined — invalid irradiance-field settings (" +
                std::to_string(settings.mNumProbes[0]) + "x" +
                std::to_string(settings.mNumProbes[1]) + "x" +
                std::to_string(settings.mNumProbes[2]) + ")");
            teardownIrradianceField();
            return;
        }

        // Re-INITIALIZE an existing field rather than churning the object: the
        // field is bound to HlmsPbs by POINTER, and initialize() re-creates the
        // atlases for the new settings on its own. Upstream's own instruction
        // for "major changes to VctLighting" is exactly this call.
        if (!mIfd) mIfd = new Ogre::IrradianceField(mRoot, mSceneMgr);
        mIfd->setIntegrationPolicy(targetSamples, kIfdKeepOnChange, rotateRays);
        mIfd->initialize(settings, origin, size, mVctLighting);
        // WHERE THE FIELD IS, recorded as asked for (the field enlarges it by a
        // probe block per side for itself): the scheduler compares against this
        // to tell a re-placement from a re-voxelisation at the same place.
        mIfdVolumeOrigin = origin;
        mIfdVolumeSize   = size;
        for (size_t i = 0; i < 3u; ++i) mIfdProbeCounts[i] = settings.mNumProbes[i];
        mIfdFollows = 0;
        mIfdScrolledProbes = 0;
        mIfdScrolls = mIfdReplacements = 0;
        mIfdTotalProbes    = total;
        mIfdProbesDone     = 0u;
        mIfdProbesPerFrame = ifdProbesPerFrame(settings, mGi.updateBudget, total);
        // One work group per probe (PHOTON-FIELD-ROTATE-1): any batch dispatches, so
        // the old dispatch floor is 1 and nothing reads it.
        mIfdMinProbes      = 1u;

        // CONVERGE NOW, in one dispatch, BEFORE binding. Two reasons and both
        // are load-bearing. (1) A freshly created atlas holds whatever the
        // recycled VRAM held (the stale-VRAM fact, in its texture guise) — a
        // bound half-converged field can therefore show another texture's
        // contents, not black. (2) It makes "bound" and "converged" the same
        // state for every caller and every pixel gate: a suite never has to
        // guess how many frames a build needs. The whole field costs ~5 ms of
        // GPU work once, against the ~2.1 ms a single live PCC probe costs
        // every frame, so paying it at build time is not a trade worth making
        // progressive (spike §6). Progressive convergence is kept for exactly
        // the case it is right for: the light-only cheap path, which re-converges
        // over the PREVIOUS converged atlas and so has nothing ugly to show.
        mIfd->update(mIfdTotalProbes);
        mIfdProbesDone = mIfdTotalProbes;
        // THE FIRST SAMPLE, NOT THE MEAN (PHOTON-FIELD-ROTATE-1). The field is a mean
        // over rotated integrations now: this pass is its first sample everywhere -
        // unbiased, every probe valid, so "bound" is still a whole field - and the
        // kIfdTargetSamples - 1 refinements it owes run at the update budget
        // (updateIrradianceField; GiStatus::ifdRefinesOwed reaches 0 when the field
        // has CONVERGED and stops). Paying them here was measured: 64 passes inline
        // blocked a new project's thumbnail build for 1.8 s at Epic
        // (threading.newproject_stall).

        // Our two scalars, and the two probe counts the shader's sky-visibility
        // threshold needs but upstream's own IrradianceField block does not
        // carry, reach the shader through the pass buffer.
        pushIfdState(settings.mNumProbes);
        // The process-wide binding, under the same discipline as VctLighting's,
        // and ONLY when the shader is reading THIS scene's voxel lighting: a
        // rebuild has just bound it (rebuildVct), a re-solve of a background
        // scene has not (refreshVctFast never snatches the binding). Binding
        // the field and claiming ownership there anyway left HlmsPbs with the
        // OTHER scene's VctLighting under an owner flag naming this one — the
        // other scene's teardown then skipped its unbind (code review
        // 2026-09-12). The field is kept either way; reassertGiBinding binds it
        // when this scene takes the screen back.
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        if (pbs->getVctLighting() == mVctLighting) {
            pbs->setIrradianceField(mIfd);
            sVctBindingOwner = this;
        }

        if (giDebug())
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: DDGI field " + std::to_string(settings.mNumProbes[0]) + "x" +
                std::to_string(settings.mNumProbes[1]) + "x" +
                std::to_string(settings.mNumProbes[2]) + " (" + std::to_string(total) +
                " probes) over " + Ogre::StringConverter::toString(origin) + " size " +
                Ogre::StringConverter::toString(size) + ", intensity " +
                std::to_string(mGi.ddgiIntensity) + ", re-converge " +
                std::to_string(mIfdProbesPerFrame) + " probes/frame");
    } JAH_CATCH(mError, );
}

void OgreScene::pushIfdState(const Ogre::uint32 numProbes[3]) {
    FogHlmsListener::IfdState st;
    st.intensity  = std::max(0.0f, std::min(mGi.ddgiIntensity, 64.0f));
    st.numProbesY = float(numProbes[1]);
    st.numProbesZ = float(numProbes[2]);
    if (mIfd) {
        const Ogre::uint32 *o = mIfd->getWindowOffset();
        st.windowOffsetPacked = float(o[0] + 128u * o[1] + 16384u * o[2]);
    }
    FogHlmsListener::setIfdState(mSceneMgr, st);
}

void OgreScene::teardownIrradianceField() {
    // THE FOLLOW DEBT DIES WITH THE FIELD (V1-RIG fix round item 4, symmetry
    // with `teardownVct`): `oweCascade0FieldFollow` refuses to raise one without
    // a field, so a debt outliving its field could only be paid into the next
    // one — which converges WHOLE at its build and owes nothing.
    mIfdFollowOwed = 0;
    mIfdTotalProbes = mIfdProbesDone = mIfdProbesPerFrame = mIfdMinProbes = 0u;
    mIfdVolumeOrigin = mIfdVolumeSize = Ogre::Vector3::ZERO;
    mIfdProbeCounts[0] = mIfdProbeCounts[1] = mIfdProbeCounts[2] = 0u;
    mIfdFollows = 0;
    mIfdScrolledProbes = 0;
    mIfdScrolls = mIfdReplacements = 0;
    if (!mIfd) return;
    JAH_TRY {
        // Pointer identity, not sVctBindingOwner: the owner flag says who bound
        // last, this says what the shader is about to read. Unbinding someone
        // else's field would be the takeover bug the VCT half already avoids.
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        if (pbs->getIrradianceField() == mIfd) pbs->setIrradianceField(nullptr);
        delete mIfd;
    } JAH_CATCH(mError, );
    mIfd = nullptr;
}

void OgreScene::updateIrradianceField() {
    if (!mIfd) return;
    // A DEFERRED FOLLOW IS OWED: nothing integrates until it is paid
    // (mIfdFollowOwed). Cascade 0's rebuild last frame may have re-created the
    // light voxel textures this job binds, and `followCascade0Field` is what
    // re-binds them — a batch here would integrate from destroyed textures.
    // It would also integrate at the OLD placement, which the follow is about
    // to replace.
    if (mIfdFollowOwed) return;
    // PAUSED (budget 0): nothing re-converges, and update() is not called at
    // all — which is also what keeps the zero-work-group abort unreachable on
    // this path.
    if (!mIfdProbesPerFrame) return;
    if (mIfdProbesDone >= mIfdTotalProbes) {
        // THE FIRST SAMPLE IS EVERYWHERE; THE FIELD REFINES (PHOTON-FIELD-ROTATE-1).
        // Each whole-grid refinement gives every probe below the target one more
        // sample under a new ray rotation, at the budget's rate; the field stops
        // (and costs nothing) once none is owed.
        JAH_TRY {
            if (mIfd->isWorkDone() && !mIfd->beginOwedRefinement()) return;   // converged
            const Ogre::uint32 batch = std::min(mIfdProbesPerFrame, mIfd->getWorkRemaining());
            monitor::CacheScope work(CacheKind::Gi, WorkReason::Sweep, 0, "ifd.refine",
                                     mRoot->getRenderSystem());
            mIfd->update(batch);
            work.setUnits(batch);
        } JAH_CATCH(mError, );
        return;
    }
    JAH_TRY {
        const Ogre::uint32 remaining = mIfdTotalProbes - mIfdProbesDone;
        const Ogre::uint32 batch = std::min(mIfdProbesPerFrame, remaining);
        {
            // THE FIELD'S PROGRESSIVE RE-INTEGRATION (ENGINE-5 item 2) — the
            // budget's turn, one batch of probes a frame, so `Sweep` is its
            // reason: nothing changed, this is the cache catching up. A
            // RE-PLACEMENT's slabs (PHOTON-FIELD-ROTATE-1 part 3: the work has no
            // history) are filed as "ifd.replace", a change's as "ifd.converge".
            const bool replacing =
                mIfd->getWorkMode() == Ogre::IrradianceField::IntegrateFresh;
            monitor::CacheScope work(CacheKind::Gi, WorkReason::Sweep, 0,
                                     replacing ? "ifd.replace" : "ifd.converge",
                                     mRoot->getRenderSystem());
            mIfd->update(batch);
            work.setUnits(batch);
        }
        mIfdProbesDone = std::min(mIfdTotalProbes, mIfdProbesDone + batch);
    } JAH_CATCH(mError, );
}

// THE FIELD FOLLOWS CASCADE 0 (PHOTON_SPEC E1 item 1).
//
// WHAT MOVES. The probes are a grid in the field's own volume, and every
// consumer reads that volume LIVE — the pixel transform is rebuilt from it per
// pass, and the generation job's probe-to-voxel transform is re-derived from it
// and from the voxel volume.
// So "the field follows cascade 0" is two numbers (origin, size) plus a
// re-integration; ogre-patch 0044 adds the setter that moves them without
// destroying the atlases, which is what made the refusal in `rebuildVct`
// necessary before it.
//
// WHY THE RE-INTEGRATION IS WHOLE WHEN THE VOLUME MOVED, and progressive when
// it did not. The atlas has NO per-probe validity: a probe's texels are its
// irradiance at the place the probe stood when it was last integrated, and the
// shader reads whichever probe now surrounds the pixel. Re-placing the volume
// therefore invalidates EVERY probe at once — at the High table cascade 0 steps
// 5 m with a 10 m box, so half the grid lands where the other half stood and
// the rest lands on ground the field has never seen. A progressive re-converge
// would light those pixels with another place's irradiance for as long as the
// budget takes (8 frames at budget 1) — not a lag, a wrong answer, and one that
// moves with the camera. So a moved field is converged WHOLE, in the frame that
// moved it, before anything reads it: the same whole-then-bind rule the build
// path follows and for the same reason.
//
// WHAT IT COSTS, measured (gi.field_follows' monitor rows, RTX 4080S, Debug,
// the High table): the whole 8,192-probe re-integration is 4.9 ms GPU and
// 0.05 ms CPU, on a frame whose cascade-0 rebuild is another 1.8 ms GPU /
// 1.3 ms CPU — worst frame of the walk 2-5 ms. It is not extra work: a
// progressive re-converge dispatches the SAME 8,192 probes, spread over the
// budget's frames (1,024 a frame at budget 1). What it buys is that the frame
// which moved the field is already showing the right answer; measured with the
// progressive policy forced, the field is never converged at all while the
// camera keeps walking (`ifdConverged` false for the whole 100 m), so every
// frame of a walk would carry a mixture of two placements. In a uniform scene
// that mixture is invisible (the lane measured 0.0 % difference on the suite's
// ground band, both policies); the size of the error is exactly how much the
// irradiance differs between the place the camera left and the place it is,
// which is the difference between one room and the next.
//
// A re-voxelisation AT THE SAME PLACE is the opposite case and keeps the
// shipped progressive policy: every probe still stands where it stood, only the
// radiance it gathers changed, so re-converging over the previous atlas shows
// slightly stale bounce and never a wrong place — exactly what `refreshGiLighting`
// does for a light drag.
// THE DEBT, RAISED (V1-RIG item 2). One follow is owed at a time and owing it
// twice means nothing: the follow always re-places the field at cascade 0's
// CURRENT volume, so a second step before the first was paid is answered by the
// one follow that catches it up — the same reasoning as a cascade's `pending`
// flag. The REASON kept is the first one, which is the one that caused the move
// the capture will attribute the work to.
void OgreScene::oweCascade0FieldFollow(GiStaleReason reason) {
    if (!mIfd || mIfdTotalProbes == 0u) return;   // nothing to follow
    if (!mIfdFollowOwed) mIfdFollowReason = reason;
    mIfdFollowOwed = 1;
}

void OgreScene::followCascade0Field(GiStaleReason reason) {
    // A field whose initialize() threw is a non-null mIfd with no atlases;
    // mIfdTotalProbes is written only after a successful build, so it is the
    // "built" reading (setIrradianceFieldGenParams would read a null atlas).
    if (!mIfd || mIfdTotalProbes == 0u || mVctCascades.empty()) return;
    const VctCascade &c0 = mVctCascades[0];
    if (!c0.voxelizer || !c0.lighting) return;
    JAH_TRY {
        // THE BINDING FIRST, AND UNCONDITIONALLY (ogre-patch 0044's second
        // half). Cascade 0's lighting re-creates its light voxel textures
        // whenever it is moved to another voxeliser (VctLighting::setVoxelizer;
        // no path in this file does that since the A5b fix round deleted the
        // material-edit replacement, but the binding costs nothing to keep
        // exact) and the field bound those textures once, by pointer, at initialize().
        // Re-binding costs five descriptor writes on a rebuild frame; deciding
        // whether it is needed would mean comparing raw pointers that may have
        // been recycled, which is the defect class patch 0041 exists for.
        mIfd->setVctLighting(c0.lighting);

        const Ogre::Vector3 origin = c0.voxelizer->getVoxelOrigin();
        const Ogre::Vector3 size   = c0.voxelizer->getVoxelSize();

        // THE FIELD SCROLLS (PHOTON-WRITER-1, FIELD-SCROLL; LATER_OPTIMISATIONS L11).
        // A cascade-0 step used to re-place the field and re-integrate ALL of it in
        // the step frame (8,192 probes at High - the frame a headset drops). The
        // field is a toroidal window over a probe lattice fixed in the world now:
        // the step moves the window by whole probe spacings, the planes that stay
        // inside keep their atlas tiles and their values, and only the planes that
        // entered are integrated here (scrollIrradianceField). A resize or a jump
        // of the whole grid keeps nothing and re-places the field as before.
        if (scrollIrradianceField(origin, size, reason)) return;

        // THE SCROLL REFUSED (a resize, or a jump of the whole grid or more on
        // an axis - a teleport, a headset re-centred far away): nothing of the
        // window is kept, so the field is RE-PLACED onto cascade 0's box from
        // scratch - offset 0 - exactly once; the window's origin is then cascade
        // 0's own, and the next step scrolls from there.
        //
        // A JUMP WITHOUT A HITCH (PHOTON-FIELD-ROTATE-1 part 3). The whole grid used
        // to be integrated in this frame (8,192 probes: 10.7 ms of GPU at two rays,
        // 96 % of a VR frame). setFieldVolume makes the work the whole grid with NO
        // history and INVALIDATES every probe (its count cleared to 0), so the
        // re-placement is integrated like any other pass: in SLABS of the budget's
        // probes a frame (ifdProbesPerFrame; the work list's slowest axis, z, so a
        // slab is whole z-planes), by updateIrradianceField, from the next frame on.
        // Until a probe's first integration the pixel's reader gives it no weight,
        // and a cage with no valid probe hands its pixel to the cone term (the
        // reader's fallback, JahIfd_piece_ps.any) - never the old placement's
        // values, which describe another place. A PAUSED budget integrates the whole
        // placement inline (one sample a probe; a paused field refines nothing).
        mIfd->setFieldVolume(origin, size);
        mIfdVolumeOrigin = origin;
        mIfdVolumeSize   = size;
        ++mIfdFollows;
        ++mIfdReplacements;
        pushIfdState(mIfdProbeCounts);                  // the window's offset is 0 again
        mIfdProbesDone = 0u;
        if (!mIfdProbesPerFrame) {
            {
                monitor::CacheScope work(CacheKind::Gi, monitor::reasonOf(reason), 0, "ifd.follow",
                                         mRoot->getRenderSystem());
                mIfd->update(mIfdTotalProbes);
                mIfdProbesDone = mIfdTotalProbes;
                work.setUnits(mIfdTotalProbes);
            }
        }
    } JAH_CATCH(mError, );
}

bool OgreScene::scrollIrradianceField(const Ogre::Vector3 &origin, const Ogre::Vector3 &size,
                                      GiStaleReason reason) {
    const float tol = 1e-4f;
    if (std::fabs(size.x - mIfdVolumeSize.x) > tol || std::fabs(size.y - mIfdVolumeSize.y) > tol ||
        std::fabs(size.z - mIfdVolumeSize.z) > tol)
        return false;                                   // a resize keeps nothing
    // THE MOVE IN WHOLE SPACINGS. The field's window lives on its own lattice
    // (the spacing of the enlarged volume over the probe counts), so cascade 0's
    // new box is met to the nearest spacing: the window follows the chain to
    // within half a spacing, and the lattice under it never moves.
    const Ogre::Vector3 spacing = mIfd->getProbeSpacing();
    Ogre::int32 d[3];
    bool any = false;
    for (int a = 0; a < 3; ++a) {
        const double moveSp = double(origin[a] - mIfdVolumeOrigin[a]) / double(spacing[a]);
        d[a] = Ogre::int32(std::lround(moveSp));
        if (std::llabs((long long)d[a]) >= (long long)mIfdProbeCounts[a]) return false;   // keeps nothing
        any = any || d[a] != 0;
    }
    if (!any) {
        // In place (a re-voxelisation where the field already stands): the
        // radiance changed, the probes did not move - progressive, over the
        // converged atlas, exactly as a light move re-integrates.
        mIfd->reset();
        mIfdProbesDone = 0u;
        if (!mIfdProbesPerFrame) {
            {
                monitor::CacheScope work(CacheKind::Gi, monitor::reasonOf(reason), 0, "ifd.follow",
                                         mRoot->getRenderSystem());
                mIfd->update(mIfdTotalProbes);
                mIfdProbesDone = mIfdTotalProbes;
                work.setUnits(mIfdTotalProbes);
            }
        }
        return true;
    }
    // `JAHSHAKA_GI_FIELD_NO_SCROLL`, for MEASUREMENT only: the same snapped window,
    // re-placed WHOLE (offset 0, every probe integrated in the step frame) — the
    // behaviour the scroll replaced, on the same lattice, so gi.field_scroll can
    // walk one path both ways in one process and compare the pictures.
    if (std::getenv("JAHSHAKA_GI_FIELD_NO_SCROLL")) {
        Ogre::Vector3 target = mIfdVolumeOrigin;
        for (int a = 0; a < 3; ++a) target[a] += float(d[a]) * spacing[a];
        mIfd->setFieldVolume(target, size);
        mIfdVolumeOrigin = target;
        ++mIfdFollows;
        ++mIfdReplacements;     // ifdFollows = ifdScrolls + ifdReplacements, this arm included
        pushIfdState(mIfdProbeCounts);
        {
            monitor::CacheScope work(CacheKind::Gi, monitor::reasonOf(reason), 0, "ifd.follow",
                                     mRoot->getRenderSystem());
            mIfd->update(mIfdTotalProbes);
            mIfdProbesDone = mIfdTotalProbes;
            work.setUnits(mIfdTotalProbes);
        }
        return true;
    }
    // A whole re-integration still running (a settle's, a light's) is restarted
    // after the scroll's own planes: the kept probes it had not reached still owe it.
    const bool progressivePending = mIfdProbesDone < mIfdTotalProbes;
    mIfd->scrollWindow(d);
    for (int a = 0; a < 3; ++a) mIfdVolumeOrigin[a] += float(d[a]) * spacing[a];
    ++mIfdFollows;
    ++mIfdScrolls;              // (never counted before PHOTON-FIELD-ROTATE-1: the status read 0)
    pushIfdState(mIfdProbeCounts);                      // the reader's modulo offset
    {
        // THE STEP FRAME'S WHOLE COST: the entered planes, inline, before any
        // pass reads them (their tiles held the planes that left).
        const Ogre::uint32 entered = mIfd->getWorkProbeCount();
        monitor::CacheScope work(CacheKind::Gi, monitor::reasonOf(reason), 0, "ifd.follow",
                                 mRoot->getRenderSystem());
        mIfd->update(entered);
        work.setUnits(entered);
        mIfdScrolledProbes = entered;
    }
    if (progressivePending) {
        mIfd->reset();
        mIfdProbesDone = 0u;
    } else {
        mIfdProbesDone = mIfdTotalProbes;
        // The entered planes' refinements run at the budget (the kept probes that
        // hold the target are skipped); a paused field refines nothing.
    }
    return true;
}

void OgreScene::teardownVct() {
    // A STAGED BUILD IS OVER (OPEN_COVER_SPEC §2 A): the arm it was filling in
    // is about to stop existing, so the stages it still owed describe nothing.
    // STOPPED, not rewound — the caller is tearing the whole arm down and the
    // build that follows starts from the beginning anyway. Every stage is whole,
    // so there is never half a GPU resource to unwind; the grid this function
    // deletes below is the same one a rewind would have deleted.
    mGiBuildStage = GiBuildStage::Idle;
    // THE DDGI FIELD DIES FIRST (spike §8, verified across all four shapes: GI
    // off under a bound field, a refresh under one, a rebuild over a refreshed
    // arm, and the Engine destroyed with one live). It holds a raw VctLighting*
    // and binds that object's voxel textures on every update, and its own
    // generation workspace lives in the SceneManager — so it must be gone
    // before either. Unbinding it from HlmsPbs is part of the same call.
    teardownIrradianceField();
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    mPccHdr = mPccShadowed = false;
    mProbeSlots.clear();
    mProbeUpdatesPerFrame = 0;
    mProbesClampedToRegion = 0;
    mPccCaptureSize = 0;
    mProbesDropped = 0;
    // The single volume's feed goes with the arm (a from-scratch rebuild makes a new
    // one): its buffers must not outlive the VaoManager, and teardownVct runs first
    // in the scene's destruction.
    mVctFeed.reset();
    // A CHAIN THAT NO LONGER EXISTS OWES NO CASCADE ANYTHING (G1): whatever the
    // dirty path recorded is answered by the build that follows, and carrying it
    // across would mark every cascade of the NEXT chain pending on its first
    // frame.
    mGiCascadeDirtyBoxes.clear();
    mGiCascadeDirtyAll = false;
    // ...AND IT OWES NO SETTLE EITHER (LAMPREST-3 fix round item 4). The debt is
    // a count of injections into cascades that are about to stop existing; a
    // tier change after a walk would otherwise pay it into the FRESH chain,
    // which has just been injected by its own build.
    mGiSettleStepsOwed = 0;
    mGiSettleCascades  = 0;
    // ...AND NO LIGHT TICK: the next chain's build injects every cascade.
    mGiTickOwed = GiTickOwed::None;
    // ...AND IT OWES NO FIELD FOLLOW (V1-RIG item 2): the debt names a field
    // and a cascade that are about to stop existing, and whatever chain comes
    // next converges its field WHOLE in `buildIrradianceField`.
    mIfdFollowOwed = 0;
    // ...NOR A CHAIN-SHAPE REBUILD (fix round item 4). Every path that tears the
    // arm down is followed by a build from the CURRENT table, so a shape debt
    // raised before it is already paid; left set, a teardown reached directly
    // (`rebuildVct`, a mode or quality change, GI off) would make the next
    // flush spend one more whole rebuild for nothing.
    mGiChainShapeDirty = false;
    mGiBuiltGeneration = ~0ull;      // nothing built: the reuse arm must refuse
    mGiReusedLastRefresh = false;
    // Unbind what the shader reads FROM THIS SCENE, by pointer identity (the
    // same rule teardownIrradianceField uses): the owner flag can disagree with
    // the pointers, and a pointer left bound past the delete below is a
    // use-after-free on the next frame. Another scene's binding is untouched.
    {
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        const bool releasedPcc = mPcc && pbs->getParallaxCorrectedCubemap() == mPcc;
        if (releasedPcc) pbs->setParallaxCorrectedCubemap(nullptr);
        if (mVctLighting && pbs->getVctLighting() == mVctLighting) pbs->setVctLighting(nullptr);
        if (sVctBindingOwner == this) sVctBindingOwner = nullptr;
        // PROCESS-WIDE: every other scene may take its own sky cube back now
        // (reflectionTexForDatablocks' note). This runs on an ordinary GI-off
        // or re-solve as well as on the scene's destruction, and the two need
        // different timing: on a GI-off the walk happens here and now, because
        // nothing else will do it and the other scenes' mirrors would stay
        // unbound (measured: gi.pcc_second_scene's last case went black); on a
        // DESTROY it must wait until the engine has erased this scene from the
        // vector the walk iterates, so it is flagged instead.
        if (releasedPcc) {
            if (mDestroying) mReleasedPccOnDestroy = true;
            else if (mEngine) mEngine->reapplyReflectionsAllScenes();
        }
    }
    // Reverse dependency order, all while the SceneManager is still alive:
    // PCC (probe workspaces + cubemap textures) -> VctLighting (reads the
    // voxelizer's textures) -> VctVoxelizer (drops its MeshPtr refs).
    delete mPcc;          mPcc = nullptr;
    delete mVctLighting;  mVctLighting = nullptr;
    delete mVctVoxelizer; mVctVoxelizer = nullptr;
    // The Photon chain's cascades 1..N-1 (cascade 0 was the two pointers above,
    // so the vector's [0] is already dangling and teardownExtraCascades knows
    // not to touch it). After the head, which is the order the arm requires.
    teardownExtraCascades();
    // ...and LAST, the one material store they all shared (A5b §2). Its texture pool is
    // bound into the shared compute jobs' descriptors by every dispatch, so it may not
    // die while a voxeliser can still be dispatched — which, after the two lines above,
    // none can.
    destroyVctMaterialStore();
    // THE THREE CASCADE COUNTERS ARE NOT RESET HERE (audit B9). Types.h calls
    // them cumulative and every reading that spans a re-solve — an edit, a
    // settle, a shadow-atlas rebuild, all of which tear the arm down — depended
    // on them being exactly that. They are cleared in teardownGi (GI off, or
    // the scene dying), which is the only point at which "this scene's history"
    // really starts again.
    if (mGiCamera) { mSceneMgr->destroyCamera(mGiCamera); mGiCamera = nullptr; }
    // ...and back ON now that the slot is free again (the mirror of the call in
    // rebuildVct). Ordered after `delete mPcc` because the helper reads it.
    applyReflectionToAll();
}

void OgreScene::teardownGi() {
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    teardownVct();
    // ...and only here: the scene's GI history starts again (B9).
    mGiCascadeAwaitingCamera = false;   // no arm is wanted at all now (F5)
    mCascadeDeferrals = 0;
    mCascadeFullRebuilds = 0;
    mCascadeDirtyMajority = 0;
    mCascadeFailureLogged = false;
}

// ---------------------------------------------------------------------------
// The shadow-atlas rebuild's GI half (SHADOW_TOOLING_SPEC.md risk R3)
// ---------------------------------------------------------------------------
// A shadow-node DEFINITION cannot be deleted while anything instantiates it,
// and the shadowed probe captures do: each PCC probe workspace names
// JahshakaProbeShadowNode, so the probe arm holds live CompositorShadowNodes
// exactly like a view's workspace does. Before this
// existed, changing the shadow resolution with hybrid GI at high quality left
// those instances pointing at freed definition memory and the next frame died
// inside Hlms::preparePassHashBase — reproduced as a SEGV by tests/shadow's r3
// mode. It could only ever fire on a Shadow Quality change; the derived map
// count (which grows when a lamp is added) would have made it routine.
//
// IT USED TO BE THE WHOLE VCT ARM, and that was the defect (PHOTON_SPEC G2 /
// audit C finding 2). `teardownVct()` + `rebuildVct()` re-voxelised the volume
// and re-PLACED every probe — thirty-two probes photographed twice,
// synchronously — because a shadow atlas GREW: on the 3rd, 5th and 9th casting
// lamp, and on the one-way first-lamp clear flip. Under Photon's cascade chain
// it was the entire chain as well, N voxelisers from scratch in one frame, for
// a change that says nothing whatever about where the geometry is.
//
// What actually holds the dying shadow-node DEFINITION is the WORKSPACES, one
// per PCC probe. Those are what must go, and they are all that goes. The
// voxels, the cascade chain, the probe shapes, the placement's depth fit and
// the field's converged atlases all survive; the
// probes' CONTENTS are staled instead, so the ordinary per-frame budget
// re-captures them a few at a time rather than the placement capturing the
// whole grid inline.
bool OgreScene::dropGiForShadowRebuild() {
    // THE SURFACE CACHE'S CAPTURE WORKSPACE HOLDS ONE TOO (SURFACE-CACHE phase
    // 2), on the SAME probe definition — a shadow-atlas rebuild removes that
    // definition and creates it again, and a workspace left holding an instance
    // of the removed one is risk R3's SEGV with a different owner. The cache is
    // dropped WHOLE rather than re-created here: its atlas is a cache by
    // definition, the next frame's `updateSurfaceCache` rebuilds it, and the
    // residency pass re-queues everything inside the radius — which costs the
    // tier's budget for a few frames and nothing else.
    if (mSurfaceCache) mSurfaceCache.reset();
    // The shadowed PCC probes hold live CompositorShadowNodes on the probe
    // definition, one workspace each.
    if (!mPcc || !mPccShadowed) return false;
    JAH_TRY {
        for (Ogre::CubemapProbe *p : mPcc->getProbes()) p->destroyWorkspace();
    } JAH_CATCH(mError, false);
    return true;
}

void OgreScene::recreateGiAfterShadowRebuild() {
    JAH_TRY {
        if (mPcc && mPccShadowed) {
            // The same near/far the placement gave them (buildPcc remembers
            // them), and the same workspace definition — `initWorkspace` with no
            // override takes the PCC's own, which is the shadowed one this arm
            // was built with.
            for (Ogre::CubemapProbe *p : mPcc->getProbes())
                p->initWorkspace(mProbeCamNear, mProbeCamFar);
            // THROUGH THE BUDGET, NOT INLINE. The probes' cubemaps went back to
            // the pool with their workspaces, so every one of them is empty and
            // must be re-photographed — but re-photographing them HERE is the
            // 32-probe, 192-face frame the placement used to pay for. Staled,
            // the per-frame budget spends them a few at a time and no frame
            // costs more than an ordinary one (ENGINE_CACHE_POLICY_SPEC P6).
            mProbeSlots.assign(mPcc->getProbes().size(), ProbeSlot());
            staleProbeGrid(GiStaleReason::Rebuild);
            // A PAUSED BUDGET has nothing to spend, and a probe with no picture
            // at all is worse than a slightly old one — so there, and only
            // there, the captures happen now, exactly as they did before.
            if (mGi.updateBudget <= 0) {
                for (Ogre::CubemapProbe *p : mPcc->getProbes()) p->mDirty = true;
                for (ProbeSlot &sl : mProbeSlots) sl.sweepPending = false;
                mPcc->updateAllDirtyProbes();
                mPlacementCapturesThisFrame += int(mPcc->getProbes().size());
            }
        }
    } JAH_CATCH(mError, );
}

}}}  // namespace jahshaka::engine::detail
