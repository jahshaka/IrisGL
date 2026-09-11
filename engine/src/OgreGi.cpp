// Global illumination (GI_SPEC.md phases 1-3): Instant Radiosity, voxel cone
// tracing (VCT) and the VCT + parallax-corrected-cubemap hybrid — the public
// verbs and the internals that drive Ogre's InstantRadiosity, VctVoxelizer/
// VctLighting and ParallaxCorrectedCubemapAuto.
#include "EnginePrivate.h"

#include <chrono>
#include <cstring>

#include <IrradianceField/OgreIrradianceFieldRaster.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreShaderParams.h>

namespace jahshaka { namespace engine { namespace detail {

// HlmsPbs is a process-wide singleton: setVctLighting/setParallaxCorrectedCubemap
// bind globally. Exactly one scene owns that binding at a time (last enabler
// wins); teardown only unbinds when the dying scene is the owner, so a takeover
// never yanks the new owner's binding.
static OgreScene *sVctBindingOwner = nullptr;

static Ogre::HlmsPbs *hlmsPbs(Ogre::Root *root) {
    return static_cast<Ogre::HlmsPbs *>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
}

// ---- JahIrradianceField: the field, plus the source switch ---------------
//
// Ogre's IrradianceField feeds its probes from ONE of two places, decided at
// initialize(): the VctLighting it is handed (cone tracing the voxel volume) or,
// when the settings name a raster workspace, six 32x32 scene renders per probe
// (IrradianceFieldRaster). Switching means another initialize() — and
// initialize() calls createTextures(), which DESTROYS the atlases. A raster
// field would therefore be born black and stay black for as long as its
// budget takes to render 8192 probes six faces at a time.
//
// Every member the switch needs is `protected` (OgreIrradianceField.h:152-214)
// and IrradianceFieldRaster's constructor and createWorkspace() are public, so
// this derived class does the switch IN PLACE: same textures, same settings,
// same field geometry; only the source and the probe counter change. The
// atlases keep the voxel field's converged answer and the raster captures
// overwrite it probe by probe under the budget — "never dark", by
// construction. The ledger's note applies: ~IrradianceField is non-virtual, so
// the field is held and deleted as THIS type (EnginePrivate.h mIfd).
//
// The reverse switch is not needed: a source change arrives through
// setGlobalIllumination, which rebuilds the VCT arm from scratch, and the
// field is rebuilt voxel-fed at the end of that as it always was.
class JahIrradianceField : public Ogre::IrradianceField {
public:
    JahIrradianceField(Ogre::Root *root, Ogre::SceneManager *sm)
        : Ogre::IrradianceField(root, sm) {}
    bool rasterFed() const { return mSettings.isRaster(); }
    /// The field the probes integrate over — initialize() enlarges the volume
    /// it was given by one probe cell on every side, and the raster depth's
    /// grid-unit conversion (patch 0023) must use THAT size.
    Ogre::Vector3 enlargedFieldSize() const { return mFieldSize; }
    Ogre::uint32  probesProcessed() const { return mNumProbesProcessed; }
    void sourceFromRaster(const Ogre::RasterParams &rp) {
        mSettings.mRasterParams = rp;
        mVctLighting = nullptr;
        mNumProbesProcessed = 0u;
        setIrradianceFieldGenParams();          // zeroes the gen params for raster
        if (!mIfRaster) mIfRaster = OGRE_NEW Ogre::IrradianceFieldRaster(this);
        mIfRaster->createWorkspace();           // binds the EXISTING atlases as channels
    }
};

// ---- Raster-source constants (rayon2 S3) ----------------------------------

// HOW MANY RASTER PROBES ONE UNIT OF THE UPDATE BUDGET BUYS PER FRAME: ONE.
// The dial's unit is reflection-probe equivalents (one PCC probe = six 512^2
// faces with HDR and shadows at High, ~2.1 ms in Debug); a raster IFD probe is
// six 32^2 faces plus a depth copy per face and one CubemapToIfd dispatch —
// 256x fewer pixels per face, but the same twelve compositor passes, camera
// moves and barriers per probe, and that overhead is what it costs, not the
// fill. MEASURED (rayon2 S3 gate, app.renderStats on a fresh Epic project,
// Debug, RTX 4080S, a sibling gate live): 64 probes/frame re-converging =
// 244 ms/frame against 27-30 ms converged, i.e. ~3.4 ms per raster probe —
// about 1.6 PCC-probe equivalents. One per budget unit is the nearest whole
// number, and it means a raster field of 8192 probes follows motion over
// 8192 / budget frames: a rig that must be tracked within seconds wants a
// budget of 8-16, which is the dial's job (the recorded follow-up is a
// cheaper capture: the depth copy pass folded into the face render, and
// fewer passes per probe).
static const Ogre::uint32 kIfdRasterProbesPerBudget = 1u;

// THE RASTER ESCAPE CALIBRATION, the raster twin of the S1 piece's
// ifdVisEscapeScale (0.9 for the voxel source, measured). A raster miss is a
// sky pixel at the capture camera's far plane EXACTLY (no cone overshoot, no
// early exit), so 1.0 was the expected value; MEASURED (gi.ddgi_raster case 7,
// gi.ddgi_ambient's open scene, ambient only, no sky): at 1.0 the raster
// field's proxy lands at 106.9% of the voxel field's (whose own scale was
// pinned to the cone reference in S1), so 1.05 brings the two sources to the
// same ambient — the raster depth is a 32^2 cubemap estimate, not a cone
// march, and the 7% is the difference between the two estimators of the same
// hemisphere. Re-measured at 1.05 in the build record.
static const float kIfdRasterEscapeScale = 1.05f;

// The capture camera's far plane, as a multiple of the enlarged field's
// diagonal. Any hit INSIDE the field is nearer than one diagonal, so a miss —
// nothing at all along the ray, out to twice the field — reads as exactly far,
// which is what the escape threshold compares against.
static const float kIfdRasterFarDiagonals = 2.0f;

/// The one raster job is SHARED across every field (IrradianceFieldRaster finds
/// it, it does not clone it), so patch 0023's grid-unit conversion is pushed to
/// it right before every raster update. A media tree that predates the patch
/// has no such parameter: logged once, and the raster source is then refused
/// at build (buildIrradianceField checks the same thing) rather than run with
/// world-unit depths.
static bool setIfdRasterInvFieldSize(Ogre::Root *root, const Ogre::Vector3 &invFieldSize) {
    Ogre::HlmsCompute *hc = root->getHlmsManager()->getComputeHlms();
    if (!hc) return false;
    Ogre::HlmsComputeJob *job = hc->findComputeJobNoThrow("IrradianceField/CubemapToIfd");
    if (!job) return false;
    Ogre::ShaderParams &sp = job->getShaderParams("default");
    Ogre::ShaderParams::Param *param = sp.findParameter("invFieldSize");
    if (!param) return false;
    param->setManualValue(invFieldSize);
    sp.setDirty();
    return true;
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

// THE PROBE CATCH-UP RATE (ENGINE_CACHE_POLICY_SPEC D2) — how many STALE probes
// one frame may re-capture. 0 = the tier's normal update budget (option A, the
// shipped default: no hitch, progressive). A positive value raises the rate to
// at least that many while probes are stale (option B was 4). The owner's
// reflections design may move it; it is deliberately this one line.
static const int kProbeCatchUpPerFrame = 0;

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
// fed FROM, and the Rayon tier table (owner option (b), 2026-09-09) decided
// against a probe-count column too — every DDGI-fed tier (Medium, High, Epic)
// gets this same fitted grid; Epic's columns are bounces and dynamic probes.
static const Ogre::uint32 kIfdTotalProbes = 8192u;

// The DEPTH probe resolution and the irradiance one. Upstream's defaults, kept
// because they are what the spike measured and because depthRes is the VRAM
// knob (86% of the atlas bytes) — moving it is a tuning decision with a
// measurement attached, not a default to drift.
static const Ogre::uint8 kIfdDepthRes  = 12u;
static const Ogre::uint8 kIfdIrradRes  = 6u;
static const Ogre::uint16 kIfdRaysPerPixel = 1u;

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

bool OgreScene::setGiDynamicProbes(int extraPerFrame) {
    mGi.dynamicProbes = std::min(std::max(extraPerFrame, 0), 8);
    // updateProbeBudget re-resolves mDynamicProbes from mGi next frame; nothing
    // built (voxels, probes, the field) depends on the value.
    return mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid;
}

bool OgreScene::setGlobalIllumination(const GiParams &p) {
    JAH_TRY {
        switch (p.mode) {
        default:
        case GiMode::Off:
            teardownGi();
            mGi = p;
            // Switching GI off is the user's own "start over": the hysteresis
            // floor forgets what used to be lit, so switching back on fits the
            // scene as it is now rather than as it was.
            noteGiAutoVolume(Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO), false);
            return true;

        case GiMode::InstantRadiosity: {
            teardownVct();
            if (!mInstantRadiosity) {
                // VPLs ride the Forward+ clustered list every scene already has;
                // this flag merely lets LT_VPL lights into it.
                mSceneMgr->getForwardPlus()->setEnableVpls(true);
                mInstantRadiosity = new Ogre::InstantRadiosity(mSceneMgr, mRoot->getHlmsManager());
                mInstantRadiosity->mVisibilityMask = kGiGeometryBit;   // PBR items only
                mInstantRadiosity->mLightMask = kGiLightBit;           // the one driving light
                // NOT setUseIrradianceVolume: the volume binds process-wide to
                // HlmsPbs (multi-scene caveat); plain VPLs already give the bounce.
            }
            // Quality -> ray/VPL budget. Rays are the cost knob (trace time and VPL
            // count); the cell size clusters VPLs (smaller = more VPLs, softer look).
            switch (p.quality) {
            case GiQuality::Low:    mInstantRadiosity->mNumRays = 128;  mInstantRadiosity->mCellSize = 4.0f; break;
            case GiQuality::Medium: mInstantRadiosity->mNumRays = 512;  mInstantRadiosity->mCellSize = 2.0f; break;
            case GiQuality::High:   mInstantRadiosity->mNumRays = 2048; mInstantRadiosity->mCellSize = 1.0f; break;
            }
            // Document bounces are total (1 = one indirect bounce); IR counts extra
            // ray bounces beyond the first hit.
            mInstantRadiosity->mNumRayBounces =
                size_t(std::min(std::max(p.numBounces, 1), 4) - 1);
            mGi = p;
            rebuildGi();
            return true;
        }

        case GiMode::Vct:
        case GiMode::VctPccHybrid:
            teardownIr();
            mGi = p;
            rebuildVct();
            return true;
        }
    } JAH_CATCH(mError, false);
}

void OgreScene::refreshGlobalIllumination() {
    JAH_TRY {
        if (mInstantRadiosity && mGi.mode == GiMode::InstantRadiosity)
            rebuildGi();
        else if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid) {
            // THE REUSE ARM FIRST (FIX WAVE B4). It refuses in exactly the cases
            // the from-scratch rule exists for, and rebuildVct is what happens
            // then — so this line can only make a refresh cheaper, never wrong.
            if (!refreshVctFast()) rebuildVct();
        }
    } JAH_CATCH(mError, );
}

// THE REUSE ARM (FIX WAVE B4) — what makes a settling drag affordable.
//
// `rebuildVct` is deliberately from-scratch, and the reason is not caution: the
// VctVoxelizer keeps raw `Item*` until removeAllItems and VctMaterial caches its
// conversions by raw datablock POINTER across builds, so a recycled address
// after a destroy would alias silently (the InstantRadiosity::freeMemory class
// of bug this file's header records). But that argument is about DESTRUCTION,
// and a refresh triggered by an object MOVING destroys nothing at all.
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
    if (mGiCachesDirty) return false;                  // a flush is already owed; it rebuilds
    if (mGiBuiltGeneration != mGiDestroyGeneration) return false;   // something may have died
    if (mGi.mode == GiMode::VctPccHybrid && !mPcc) return false;

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
    Ogre::Aabb region = mGiProbeRegion;
    if (mGi.mode == GiMode::VctPccHybrid) {
        region = computeProbeRegion(aabb);
        if (!sameBox(region, mGiProbeRegion)) return false;   // shapes must be re-derived
    }

    JAH_TRY {
        // Timed for the JAHSHAKA_GI_DEBUG log: the settle frame's cost is the
        // owner's "no hitches" number (CPU side, submission included).
        const auto tStart = std::chrono::steady_clock::now();
        // A MATERIAL the voxelizer converted has changed since it was built
        // (P7): its VctMaterial cache would re-voxelize the old colour, so the
        // voxel half is rebuilt fresh — under the SAME probe grid.
        const bool freshVoxels = mGiBuiltMaterialGeneration != mGiMaterialGeneration;
        if (freshVoxels) {
            if (!freshVoxelArm(aabb)) return false;          // the caller rebuilds
        } else {
            // Items born since the build. Nothing can have DIED (the generation
            // says so), so the voxelizer's item list only ever grows on this path.
            for (auto &kv : mNodes) {
                Ogre::Item *item = kv.second.item;
                if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
                if (std::find(mVctItemIds.begin(), mVctItemIds.end(), kv.first) != mVctItemIds.end())
                    continue;
                mVctVoxelizer->addItem(item, false);
                mVctItemIds.push_back(kv.first);
            }
            // World transforms first: the voxelizer reads them, and the whole
            // reason this call exists is that something moved.
            mSceneMgr->updateSceneGraph();
            if (!sameBox(aabb, mGiLitVolume)) {
                mVctVoxelizer->setRegionToVoxelize(false, aabb);
                mVctVoxelizer->dividideOctants(1u, 1u, 1u);
            }
            mVctVoxelizer->build(mSceneMgr);
            applyVctAmbient();
            const Ogre::uint32 extraBounces =
                Ogre::uint32(std::min(std::max(mGi.numBounces, 1), 4) - 1);
            mVctLighting->update(mSceneMgr, extraBounces, 1.0f /*thinWallCounter*/, hasVctLights(),
                                 giRayMarchStepScale(false));
        }
        const auto tVoxels = std::chrono::steady_clock::now();
        mGiLitVolume = aabb;
        mGiProbeRegion = region;
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
        // may have moved the volume (setRegionToVoxelize above), and the field's
        // grid is a function of that volume's origin and size — a reset would
        // leave the probes describing a box that no longer exists. Upstream's
        // own rule, in its own words: minor changes to VctLighting -> reset(),
        // major -> initialize() again.
        buildIrradianceField();
        // A fresh voxel arm is not a reuse of the voxels (the probes were
        // kept either way, and giStatus.rebuilds does not move).
        mGiReusedLastRefresh = !freshVoxels;
        if (std::getenv("JAHSHAKA_GI_DEBUG")) {
            const auto tEnd = std::chrono::steady_clock::now();
            const auto ms = [](std::chrono::steady_clock::time_point a,
                               std::chrono::steady_clock::time_point b) {
                return std::to_string(std::chrono::duration<double, std::milli>(b - a).count());
            };
            Ogre::LogManager::getSingleton().logMessage(
                std::string("Jahshaka GI: refresh ") +
                (freshVoxels ? "re-voxelized FRESH (a material changed)" : "REUSED the voxel arm") +
                " (" + std::to_string(mVctItemIds.size()) + " items, probe grid staled) in " +
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
bool OgreScene::refreshGiLighting() {
    JAH_TRY {
        if (mInstantRadiosity && mGi.mode == GiMode::InstantRadiosity) {
            rebuildGi();          // IR has no cheaper path: the re-trace IS it
            return true;
        }
        if (!mVctLighting || !mVctVoxelizer) return false;
        mSceneMgr->updateSceneGraph();
        const Ogre::uint32 extraBounces =
            Ogre::uint32(std::min(std::max(mGi.numBounces, 1), 4) - 1);
        // IN MOTION, by definition: this path only runs while the mirror's
        // stability window is open, i.e. while something is being dragged. B5's
        // coarser ray march is charged here and nowhere else.
        mVctLighting->update(mSceneMgr, extraBounces, 1.0f /*thinWallCounter*/, hasVctLights(),
                             giRayMarchStepScale(true));
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
            mIfd->reset();
            mIfdProbesDone = 0u;
            mIfdRigEpochSeen = mRigPoseEpoch;
            // Paused budget: the voxel field converges inline (5 ms of GPU);
            // a raster one would be 8192 x 6 scene renders in one frame, so it
            // keeps its previous answer until the budget is raised.
            if (!mIfdProbesPerFrame && mIfdSource != GiSource::Raster) {
                mIfd->update(mIfdTotalProbes);
                mIfdProbesDone = mIfdTotalProbes;
            }
        }
        return true;
    } JAH_CATCH(mError, false);
}

GiStatus OgreScene::giStatus() const {
    GiStatus st;
    st.mode = mGi.mode;
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
        const Ogre::Vector3 litSize = mGiLitVolume.getSize();
        st.voxelMetres    = std::max(std::max(litSize.x, litSize.y), litSize.z) /
                            float(std::max(giVoxelResolution(), 1u));
        st.probeRegionMin = toV(mGiProbeRegion.getMinimum());
        st.probeRegionMax = toV(mGiProbeRegion.getMaximum());
        // RESOLVED, not requested: both default to GiToggle::Auto, and the
        // shadow half additionally falls back when there is no shadow node.
        st.probeHdr     = mPcc && mPccHdr;
        st.probeShadows = (mPcc && mPccShadowed) || (mIfd && mIfdShadowed);   // either shadowed capture arm
        // RESOLVED, like the two above: the request is clamped to the probes
        // that exist, and it is only ACTED ON once a view has pushed a tracked
        // camera position (updateGiTracking), so this reads 0 for the frame
        // between the rebuild and the first tracking update.
        st.probeUpdatesPerFrame = mPcc ? mProbeUpdatesPerFrame : 0;
        st.dynamicProbes        = mPcc ? mDynamicProbes : 0;
        st.dynamicProbeUpdates  = mPcc ? mDynamicProbeUpdates : 0;
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
        // DDGI, reported the same way pccBound/vctBound are: against the live
        // HlmsPbs pointer, not against what was requested or who bound last.
        st.ifdBound          = mIfd && pbs->getIrradianceField() == mIfd;
        st.ifdProbes         = int(mIfdTotalProbes);
        st.ifdConverged      = mIfd && mIfdProbesDone >= mIfdTotalProbes;
        st.ifdProbesPerFrame = mIfd ? int(mIfdProbesPerFrame) : 0;
        st.ifdSource         = (mIfd && st.ifdBound) ? mIfdSource : GiSource::Voxel;
        // THE PROBE CACHE (ENGINE_CACHE_POLICY_SPEC P1/P6/P7).
        st.probeCapturesLastFrame = mPcc ? mProbeCapturesLastFrame : 0;
        int stale = 0;
        if (mPcc) for (const ProbeSlot &sl : mProbeSlots) if (sl.sweepPending) ++stale;
        st.staleProbes     = stale;
        st.lastStaleReason = mLastStaleReason;
        st.staleSerial     = mStaleSerial;
        st.rebuilds        = mGiRebuilds;
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

void OgreScene::noteMaterialChanged(MaterialId id, bool voxelInputsChanged) {
    if (mGi.mode == GiMode::Off) return;
    // NOTHING CACHED TO INVALIDATE: no probe grid and no voxelizer built yet,
    // or a from-scratch rebuild already owed (it reads every material fresh and
    // stales the whole grid itself). This is also what keeps the node walk
    // below off the LOAD path — every material and every texture bind is pushed
    // once while a scene opens, and an O(nodes) walk per push there is the
    // O(nodes x binds) class that once cost 2.1 s of boot (setPbrTexture's
    // note). A live edit on a built arm walks once per push.
    // Instant Radiosity counts as a cache too: its trace reads the same
    // diffuse colours, and the generation is what makes the host re-trace it.
    if (mGiCachesDirty || (!mPcc && !mVctVoxelizer && !mInstantRadiosity)) return;
    bool voxelized = false;
    if (!materialSeenByGi(id, voxelized)) return;     // nothing GI can see wears it
    staleProbeGrid(GiStaleReason::Material);
    if (voxelized && voxelInputsChanged) ++mGiMaterialGeneration;
}

void OgreScene::staleProbeGrid(GiStaleReason why) {
    if (!mPcc) return;
    const size_t n = mPcc->getProbes().size();
    if (mProbeSlots.size() != n) mProbeSlots.assign(n, ProbeSlot());
    for (ProbeSlot &sl : mProbeSlots) sl.sweepPending = true;
    mLastStaleReason = why;
    ++mStaleSerial;
}

// WHAT THE FRAME ACTUALLY RE-CAPTURES (GiStatus::probeCapturesLastFrame).
// Counted at the last moment before the render, from the probes' own dirty
// flags, so it covers every source that can raise one — the budget, the
// dynamic reservation, a shape clamp's CubemapProbe::set — rather than trusting
// any one of them to report itself. A from-scratch placement captures the whole
// grid synchronously inside buildPcc (PccPerPixelGridPlacement's buildStart AND
// buildEnd each run updateAllDirtyProbes), bypassing the flags, so it reports
// its own count through mPlacementCapturesThisFrame.
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
}

void OgreScene::setNodeGiBoundsExcluded(NodeId id, bool excluded) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.giBoundsExcluded == excluded) return;
    it->second.giBoundsExcluded = excluded;
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
Ogre::Light *OgreScene::markGiLight(NodeId requested) {
    Ogre::Light *chosen = nullptr;
    if (requested) {
        auto it = mNodes.find(requested);
        if (it != mNodes.end()) chosen = it->second.light;
    }
    if (!chosen)
        for (auto &kv : mNodes)
            if (kv.second.light && kv.second.light->getType() == Ogre::Light::LT_DIRECTIONAL) {
                chosen = kv.second.light; break;
            }
    if (!chosen)
        for (auto &kv : mNodes)
            if (kv.second.light) { chosen = kv.second.light; break; }
    for (auto &kv : mNodes) {
        if (!kv.second.light) continue;
        const Ogre::uint32 flags = kv.second.light->getVisibilityFlags();
        const Ogre::uint32 want = (kv.second.light == chosen) ? (flags | kGiLightBit)
                                                              : (flags & ~kGiLightBit);
        if (want != flags) kv.second.light->setVisibilityFlags(want);
    }
    return chosen;
}

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
std::vector<Ogre::Aabb> OgreScene::giItemBounds() const {
    std::vector<Ogre::Aabb> all;
    all.reserve(mNodes.size());
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (kv.second.giBoundsExcluded) continue;
        all.push_back(const_cast<Ogre::Item *>(item)->getWorldAabbUpdated());
    }
    // One item IS the scene; there is no population to be an outlier against.
    mGiLastItemCount = all.size();
    if (all.size() < 2u) return all;

    const size_t n = all.size();
    std::vector<float> extents(n);
    double logSum = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const Ogre::Vector3 s = all[i].getSize();
        extents[i] = std::max(std::max(std::max(s.x, s.y), s.z), 1e-4f);
        logSum += std::log(double(extents[i]));
    }
    const float scale = float(std::exp(logSum / double(n)));
    if (!(scale > 0.0f)) return all;      // degenerate (all points) — keep everything

    // The weight ramp, in log space so it is scale-invariant.
    static const float kOutlierSoftStart = 4.0f;    // content up to here
    static const float kOutlierSoftEnd   = 16.0f;   // pure scenery beyond here
    const float logStart = std::log(kOutlierSoftStart);
    const float logSpan  = std::log(kOutlierSoftEnd) - logStart;

    // The hysteresis floor (property 3): items the previous auto volume covered.
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
    bool anyTrimmed = false;
    for (size_t i = 0; i < n; ++i) {
        const float t = (std::log(extents[i] / scale) - logStart) / logSpan;
        if (t <= 0.0f) continue;                                  // content: w = 1
        const float c = std::min(t, 1.0f);
        w[i] = 1.0f - (c * c * (3.0f - 2.0f * c));                // smoothstep
        if (coveredByPrev(all[i])) { w[i] = 1.0f; continue; }     // already lit: keep it whole
        if (w[i] < 1.0f) anyTrimmed = true;
    }
    if (!anyTrimmed) return all;      // the common case: the plain union, untouched

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

    std::vector<Ogre::Aabb> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Ogre::Vector3 mn = all[i].getMinimum(), mx = all[i].getMaximum();
        if (w[i] >= 1.0f) { out.push_back(all[i]); continue; }
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
    return out;
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
    if (!mGiAutoVolumeValid) return 0ull;      // nothing resolved yet, or hand-typed bounds
    const Ogre::Vector3 vmn = mGiAutoVolume.getMinimum(), vmx = mGiAutoVolume.getMaximum();
    const Ogre::Vector3 size = vmx - vmn;
    const float quantum = std::max(std::max(std::max(size.x, size.y), size.z) / 64.0f, 1e-4f);
    unsigned long long h = 1469598103934665603ull;      // FNV-1a
    const auto fold = [&h](unsigned long long v) {
        h ^= v; h *= 1099511628211ull;
    };
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (kv.second.giBoundsExcluded) continue;
        const Ogre::Aabb a = const_cast<Ogre::Item *>(item)->getWorldAabbUpdated();
        const Ogre::Vector3 mn = a.getMinimum(), mx = a.getMaximum();
        if (mn.x >= vmn.x && mn.y >= vmn.y && mn.z >= vmn.z &&
            mx.x <= vmx.x && mx.y <= vmx.y && mx.z <= vmx.z) continue;
        fold((unsigned long long)kv.first);
        for (size_t ax = 0; ax < 3u; ++ax) {
            fold((unsigned long long)(long long)std::floor(mn[ax] / quantum));
            fold((unsigned long long)(long long)std::floor(mx[ax] / quantum));
        }
    }
    return h == 1469598103934665603ull ? 0ull : h;      // untouched hash == nothing escaped
}

// A SCENE WITH NO LIGHTS AT ALL BREAKS VctLighting's AUTO MULTIPLIER, and after
// LIGHTING_FIX fix 3 that is visible rather than academic. `update()`'s
// auto-multiplier pass takes the maximum radiance over the scene's lights, then
// inverts it (OgreVctLighting.cpp:952-956): with no lights the maximum is 0, the
// inverse is infinity, `mInvBakingMultiplier` comes out 0, and the shader's
// `blendWeight = blendFade * blend * multiplier` is 0 — so the VCT arm
// contributes NOTHING. That used to be invisible (nothing to contribute), but
// the ambient now rides the same multiplier, and the PBS ambient pieces are
// switched off inside a VCT volume, so an ambient-lit scene with no lights went
// BLACK the moment VCT was enabled. Passing autoMultiplier = false falls back to
// `mBakingMultiplier` (1.0), which is the right answer when there is nothing to
// normalise against. Upstream behaviour, reported, worked around here.
bool OgreScene::hasVctLights() const {
    for (const auto &kv : mNodes) {
        const Ogre::Light *l = kv.second.light;
        // A HIDDEN light is no light to VctLighting (update() gathers only
        // LAYER_VISIBILITY lights), so switching off a room's only lamp is the
        // no-lights case below, not a scene with one light and a zero maximum.
        if (!l || !l->getVisible()) continue;
        switch (l->getType()) {
        case Ogre::Light::LT_DIRECTIONAL: case Ogre::Light::LT_POINT:
        case Ogre::Light::LT_SPOTLIGHT:   case Ogre::Light::LT_AREA_APPROX:
        case Ogre::Light::LT_AREA_LTC:    return true;
        default: break;
        }
    }
    return false;
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
}

bool OgreScene::giBoundsExplicit() const {
    const Vec3 &a = mGi.boundsMin, &b = mGi.boundsMax;
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
    const float maxEdge = mGi.autoBoundsMax;
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
    const Vec3 &a = mGi.boundsMin, &b = mGi.boundsMax;
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
unsigned OgreScene::giVoxelResolution() const {
    switch (mGi.quality) {
    case GiQuality::Low:  return 32u;
    case GiQuality::High: return 128u;
    default:              return 64u;
    }
}

// THE PROBE REGION — not the lit volume (REFLECTIONS_ADOPTION_SPEC.md P1a, the
// root cause of P4's finding 2).
//
// `PccPerPixelGridPlacement::setFullRegion` does NOT take a bounding box of the
// geometry. It takes the FREE SPACE the probes will live in: upstream's own
// sample hands it the interior cube's exact interior (half-size 0.5 for a
// 1x1x1 room, no margin at all). We used to hand it the voxel volume — the
// geometry union plus 10% plus 0.5, or whatever the user typed into the bounds
// rows — and that is a materially different box.
//
// Why it matters, measured (gi.pcc_bounds, 2x1x2 probes, identical geometry,
// ONLY the region changing):
//     region = geometry + 0.2   mirror pixel r = 0.251
//     region = geometry + 0.4                   0.063
//     region = geometry + 0.6                   0.000   <- black
//     region = geometry + 1.6                   0.000
// The chain: buildEnd shrink-fits each probe by reading ONE 1x1 averaged depth
// value per cube face, encoded as 0.5 * fDist / fApproxDist where fApproxDist
// is measured to the REGION box. Averaging that ratio over a 90-degree face is
// only well behaved while the region is close to the geometry; once it is not,
// the fitted parallax boxes overshoot the room by many units (measured: a probe
// in a room spanning x in [-4,4] fitted to x in [-4.6, +7.7]). The PBS hybrid
// then compares the probe's parallax-reconstructed hit against the VCT cone hit
// (getPccVctBlendWeight -> distToVct), finds them further apart than
// pccVctMinDistance, and hands the pixel to VCT — which in a sealed room has
// nothing, i.e. black. probeCount and pccBound stay perfectly healthy
// throughout, which is exactly why P4 could not see it.
//
// So: start from the TIGHT union (no margin) of the same items the lit volume
// uses, clamp it into the lit volume (an explicit user bounds box therefore
// still governs the extent, and a user who types the room's interior gets the
// room's interior), and then pull each of the six faces in to the nearest
// ENCLOSING slab — the floor, the ceiling, the walls. An item counts as a wall
// for a direction when it (1) lies wholly on that side of the hull's centre,
// (2) spans at least half of the hull on both other axes, and (3) HAS ITS OUTER
// FACE AT THE HULL'S FACE. Furniture and the subject of the scene fail (2); a
// free-standing partition in the middle of the room fails (3). In an open scene
// no wall is found on most axes and the tight hull stands, which is the right
// answer there.
//
// CONDITION (3) IS THE FIX FOR THE MIRROR ROOM'S BLACK REFLECTIONS (FIX WAVE
// defect A1, 2026-09-07; found by the debug-runner on the shipped sample).
// Without it, "wall-like" meant nothing more than "big and off-centre", so the
// sample's free-standing MirrorPanel — a 5.2 x 3.0 x 0.24 slab standing at
// z = -2.2 in the MIDDLE of a room whose walls are at z = +-5.25 — qualified as
// the room's -Z wall and truncated the probe region at its own face (measured:
// probeRegionMin.z came back EQUAL to the panel's zMax to five decimals). Every
// probe was then placed and shrink-fitted inside a region that stopped a third
// of the way across the room, the parallax boxes that came out of buildEnd
// disagreed with the voxel volume by more than the hybrid's trust window, and
// `getPccVctBlendWeight` handed those pixels to VCT — black, in a sealed room.
// The defect needs no thin panel to appear, only a big enough object standing
// clear of the walls: any partition, screen, counter or bookcase would do it.
//
// The epsilon is RELATIVE (kWallFaceEpsilon of the hull's own extent on that
// axis) rather than absolute, so it is scale-invariant, and it is generous
// enough for the case that would otherwise regress: a floor slab wider than the
// room leaves the side walls' outer faces a little inside the hull. A wall stops
// counting as one when it stands further in than a tenth of the room; the
// MirrorPanel stands 30% in.
//
// KNOWN LIMIT, documented rather than papered over: a room imported as ONE
// hollow mesh has an AABB that IS its outer shell, and no axis-aligned test can
// find its interior. Such a scene needs the explicit bounds rows (which clamp
// this region) or the per-node exclude flag. Only a second depth-readback pass
// could do better, and that doubles the probe render cost.
Ogre::Aabb OgreScene::computeProbeRegion(const Ogre::Aabb &litVolume) const {
    const std::vector<Ogre::Aabb> items = giItemBounds();
    if (items.empty()) return litVolume;

    Ogre::Vector3 mn(1e30f), mx(-1e30f);
    for (const Ogre::Aabb &a : items) { mn.makeFloor(a.getMinimum()); mx.makeCeil(a.getMaximum()); }
    mn.makeCeil(litVolume.getMinimum());     // clamp INTO the lit volume
    mx.makeFloor(litVolume.getMaximum());
    for (size_t ax = 0; ax < 3u; ++ax)
        if (!(mn[ax] < mx[ax])) return litVolume;   // clamped to nothing: keep the caller's box

    const Ogre::Vector3 hullMin = mn, hullMax = mx;
    const Ogre::Vector3 centre = (hullMin + hullMax) * 0.5f;
    const Ogre::Vector3 hullSize = hullMax - hullMin;

    // How far a slab's outer face may sit inside the hull's face and still be
    // read as part of the enclosure, as a fraction of the hull's extent on that
    // axis. Measured against both cases it has to separate: a room whose floor
    // overhangs its walls (wall faces ~7% in — must still count) and the Mirror
    // Room's free-standing panel (~30% in — must not).
    static const float kWallFaceEpsilon = 0.1f;

    for (size_t ax = 0; ax < 3u; ++ax) {
        const size_t o1 = (ax + 1u) % 3u, o2 = (ax + 2u) % 3u;
        const float faceEps = std::max(hullSize[ax] * kWallFaceEpsilon, 1e-4f);
        float nearestMax = hullMax[ax];      // the +axis face, pulled inwards
        float nearestMin = hullMin[ax];      // the -axis face, pulled inwards
        for (const Ogre::Aabb &a : items) {
            const Ogre::Vector3 amn = a.getMinimum(), amx = a.getMaximum();
            // Wall-like for this axis: covers at least half of the hull on both
            // of the OTHER axes. A 2-unit box in a 9-unit room never qualifies.
            const auto covers = [&](size_t k) {
                if (hullSize[k] <= 0.0f) return true;
                const float lo = std::max(amn[k], hullMin[k]);
                const float hi = std::min(amx[k], hullMax[k]);
                return (hi - lo) >= hullSize[k] * 0.5f;
            };
            if (!covers(o1) || !covers(o2)) continue;
            // ...AND its OUTER face is the hull's face (condition 3, the A1 fix
            // — see the header). `>=`/`<=` rather than a two-sided band: a slab
            // reaching PAST the clamped hull (a wall outside user-typed bounds)
            // is still a wall.
            if (amn[ax] > centre[ax] && amn[ax] < nearestMax && amx[ax] >= hullMax[ax] - faceEps)
                nearestMax = amn[ax];
            if (amx[ax] < centre[ax] && amx[ax] > nearestMin && amn[ax] <= hullMin[ax] + faceEps)
                nearestMin = amx[ax];
        }
        // Only accept the pull if it leaves a real volume behind.
        if (nearestMin < nearestMax) { mn[ax] = nearestMin; mx[ax] = nearestMax; }
    }
    return Ogre::Aabb::newFromExtents(mn, mx);
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
// the free space the grid was fitted to, and the fix for the region itself
// (P1a's computeProbeRegion, plus A1 above) is what makes that statement true.
// So every fitted shape is clamped into it, per axis. This does not fight the
// shrink-fit: a shape that fits inside the region is untouched, which is the
// case for every probe in a plain empty room.
//
// Ogre's own bookkeeping is respected rather than poked around: the shape is
// re-published through `CubemapProbe::set`, keeping the probe's camera
// position, influence AREA, inner region and orientation exactly as the
// placement left them. `set` re-applies its own 1.005 padding to whatever it is
// handed, so both boxes are un-padded on the way in and the values that land in
// the probe are exactly the intended ones. It also raises `mDirty`, so the
// clamped probes re-capture on the next frame, which is what we want anyway.
void OgreScene::clampProbeShapesToRegion(const Ogre::Aabb &region) {
    if (!mPcc) return;
    static const float kSetPadding = 1.005f;    // CubemapProbe::set's own padding
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
    const bool debug = std::getenv("JAHSHAKA_GI_DEBUG") != nullptr;
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
        p->set(cam, Ogre::Aabb(area.mCenter, area.mHalfSize / kSetPadding),
               p->getAreaInnerRegion(), p->getOrientation(),
               Ogre::Aabb(clamped.mCenter, clamped.mHalfSize / kSetPadding));
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

void OgreScene::invalidateGiCaches() {
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
    if (mInstantRadiosity) {
        // The cache FREE must happen NOW, while the dying mesh/texture is still
        // alive: InstantRadiosity::freeMemory dereferences its cache KEYS
        // (itor->first->getIndexBuffer()->getShadowCopy() on the raw
        // VertexArrayObject*) — calling it after the mesh died is itself the
        // heap corruption. Callers therefore invalidate BEFORE destroying.
        // Repeat calls in a burst are no-ops (the maps are already empty);
        // the re-trace still happens ONCE, at frame time.
        JAH_TRY { mInstantRadiosity->freeMemory(); } JAH_CATCH(mError, );
        mGiCachesDirty = true;
    }
    if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid)
        mGiCachesDirty = true;   // VCT never dereferences stale keys: the flush
                                 // rebuilds the whole arm from scratch
    // THE DESTRUCTION GENERATION (FIX WAVE B4). Bumped unconditionally, and
    // unconditionally is the point: every caller of this function either
    // destroys something the GI arms hold a raw pointer into, or wants a
    // from-scratch rebuild for its own reasons. Being conservative here costs a
    // full rebuild that could have been a reuse; being clever here costs heap
    // corruption. The counter is the ONLY thing standing between the reuse arm
    // and the rule this file's header spends a paragraph on.
    ++mGiDestroyGeneration;
}

void OgreScene::applyPendingGi() {
    if (!mGiCachesDirty) return;
    mGiCachesDirty = false;
    JAH_TRY {
        if (mInstantRadiosity)
            rebuildGi();       // caches were freed at invalidate time; re-downloads live
        else if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid)
            rebuildVct();      // fresh voxelizer over the LIVE scene
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
        Ogre::Vector3 mn(1e30f), mx(-1e30f);
        size_t count = 0;
        for (auto &kv : mNodes) {
            Ogre::Item *item = kv.second.item;
            if (!item || !(item->getVisibilityFlags() & kVisibleBit)) continue;
            const Ogre::Aabb a = item->getWorldAabbUpdated();
            mn.makeFloor(a.getMinimum()); mx.makeCeil(a.getMaximum());
            ++count;
        }
        if (!count) return;                 // an empty scene keeps whatever it has
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
    // with it. Instant Radiosity's VPLs ride the Forward+ list through
    // `setEnableVpls`, which is exactly such a setting — re-armed here so a
    // range update or a probe-budget growth cannot silently switch the bounce
    // off. (Found while fixing the probe budget, 2026-09-07: the range update
    // has been recreating this object every time the camera changed scale since
    // fix 8, and IR's VPL flag has been going with it.)
    mSceneMgr->setForwardClustered(true, 16, 8, 24, 96, kDecalsPerCell, mCubemapProbeSlots,
                                   minDistance, maxDistance);
    if (mInstantRadiosity && mSceneMgr->getForwardPlus())
        mSceneMgr->getForwardPlus()->setEnableVpls(true);
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

void OgreScene::updateGiTracking(const Ogre::Vector3 &camPos) {
    // The DDGI half runs FIRST and unconditionally: it is scene-fitted, so it
    // needs no camera at all, and it must run in plain VCT mode too — where
    // there is no PCC and the probe half below returns immediately. (Both
    // halves are driven once a frame, from the ONE authoritative view of the
    // scene: OgreEngine::renderOneFrame picks it, for the same reason the probe
    // budget must not be spent once per view.)
    mGiWalkedThisFrame = false;     // one GI walk per frame; the first consumer runs it
    updateIrradianceField();
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
    // its consumers (the probe budget, the raster field's re-arm) run EARLIER
    // in the frame, before any scene graph update, so they must see THIS
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
    const auto t0 = std::chrono::steady_clock::now();
    walkItems(true, false, true);
    mGiScanMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

void OgreScene::indexItemNode(Node &n) {
    if (n.itemSlot != size_t(-1)) return;
    n.itemSlot = mItemNodes.size();
    mItemNodes.push_back(&n);
}

void OgreScene::unindexItemNode(Node &n) {
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
}

// THE ITEM WALK — the GI movement scan (FIX WAVE B3, engine half) and the
// lamp-map cache's caster scan (ENGINE_CACHE_POLICY_SPEC P3), in one pass over
// the nodes that HAVE an Item (mItemNodes), with each node's last-seen state on
// the node itself. (It used to be two walks of the whole std::map, each with a
// hash lookup per item: measured 1.4 + 2.3 ms at 5k nodes in Debug+ASan.)
//
// GI half: records which GI items MOVED since the last scan, as the union of
// each mover's old and new world AABB. Two consumers, both cheap paths: the
// probe round-robin (a probe whose parallax shape contains a moved box goes to
// the front of the sweep) and the raster field's re-arm. QUANTIZED to a 64th of
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
        firstGi = !mGiScannedOnce;
        mGiScannedOnce = true;
    }
    bool firstShadow = false;
    Ogre::uint32 channelsAll = 0u;
    std::vector<MaterialId> deforming;   // materials with a vertex-stage piece (usually none)
    if (shadow) {
        mShadowChanges.clear();
        firstShadow = !mShadowScanPrimed;
        mShadowScanPrimed = true;
        channelsAll = allShadowCasterChannels();
        for (const auto &mk : mMaterials)
            if (!mk.second.customPiece[1].empty()) deforming.push_back(mk.first);
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
            if (!(flags & kGiGeometryBit)) {
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
            } else if (giAabbMoved(n.scan.giBox, a)) {
                Ogre::Aabb moved = n.scan.giBox;
                moved.merge(a);
                mGiMovedBoxes.push_back(moved);
                n.scan.giBox = a;
            }
        }
        if (shadow) {
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
                    } else if (r.shadowPose != pose || (deforms && present) || n.shadowShapeDirty) {
                        mShadowChanges.push_back({ a, channels });
                    }
                }
            }
            n.shadowShapeDirty = false;
            r.shadowPresent = present;
            r.shadowItem = present ? item : nullptr;
            r.shadowBox = present ? a : Ogre::Aabb();
            r.shadowPose = pose;
            r.shadowChannels = channels;
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
//     v1 STALES THE WHOLE GRID per input, a mover included (on top of the
//     dynamic reservation below, which is unchanged): indoors every parallax
//     shape contains everything (the A2 clamp), so area or shape locality would
//     discriminate nothing; v2 can add it for open fields.
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
    // Epic's DYNAMIC-PROBE reservation (GiParams::dynamicProbes): resolved here
    // like the budget, and 0 whenever the budget is paused — "paused" means no
    // probe re-captures at all, and a moved-covering re-capture is one.
    mDynamicProbes = budget ? std::min(std::max(0, std::min(mGi.dynamicProbes, 8)), int(n)) : 0;
    mDynamicProbeUpdates = 0;
    if (!n || !budget) return;          // paused: nothing dirtied, nothing scanned

    ensureGiWalk();

    // THE INPUTS THIS PASS CAN SEE FOR ITSELF (the rest arrive through the
    // setters: setLight, setPbrMaterial, setSky, attach/detach/visibility ...).
    // Geometry the probes capture that moved or arrived since the last scan —
    // GI geometry and unlit geometry alike — is out of date in every probe
    // that can see it, which in v1 is all of them.
    if (!mGiMovedBoxes.empty() || mGiItemsAppeared || mProbeOnlyChanged)
        staleProbeGrid(GiStaleReason::Moved);
    mGiItemsAppeared = mProbeOnlyChanged = false;
    // TIME-VARYING CONTENT IS FROZEN (REALTIME_REFLECTIONS_SPEC O4 = A, lead
    // decision 2026-09-12): a posing rig, a particle system, a clock-driven
    // material or a live texture stales nothing by itself. SSR and planar
    // reflections show it live; the probes are the static-environment layer.

    // THE CATCH-UP RATE (spec D2). How many STALE probes a frame may capture:
    // the tier's normal budget (option A, the default — no hitch, a 32-probe
    // grid catches up in about half a second at 60 Hz). The reflections design
    // being studied separately may raise it; this is the one line to change
    // (option B was a temporary 4 per frame).
    const int catchUp = kProbeCatchUpPerFrame > 0 ? std::max(budget, kProbeCatchUpPerFrame) : budget;

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
    const size_t take = std::min(size_t(catchUp), ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + std::ptrdiff_t(take), ranked.end());
    for (size_t k = 0; k < take; ++k) {
        const size_t i = ranked[k].second;
        probes[i]->mDirty = true;
        mProbeSlots[i].sweepPending = false;
        mProbeSlots[i].framesSinceUpdate = 0;
    }

    // THE DYNAMIC-PROBE RESERVATION (Epic's column, GiParams::dynamicProbes).
    //
    // The sweep above is a guarantee about EVERY probe and it spends the budget
    // in priority order — so with budget 1 and a grid of 18, the probe a moving
    // object is inside re-captures once (it jumps the queue on kMovedBoost) and
    // then waits up to 17 frames for the sweep to refill before it can go
    // again. That is the right economy for a still scene and the wrong one for
    // a moving one: the reflection of the mover updates in steps. Epic reserves
    // `dynamicProbes` extra captures per frame for probes whose AREA covers a
    // box that moved THIS frame, ranked by how long they have waited, skipping
    // any the sweep already took. A still scene has no moved box and spends
    // nothing here — the column is free at rest by construction — and a probe
    // taken here counts as fresh for the sweep (it IS a capture), so the sweep
    // guarantee is untouched: it only ever gets ahead, never behind.
    if (mDynamicProbes > 0 && !mGiMovedBoxes.empty()) {
        std::vector<std::pair<unsigned, size_t>> movers;   // (framesSinceUpdate, index)
        for (size_t i = 0; i < n; ++i) {
            if (!mProbeSlots[i].framesSinceUpdate) continue;   // taken this frame already
            bool covers = false;
            for (const Ogre::Aabb &b : mGiMovedBoxes)
                if (probes[i]->getArea().intersects(b)) { covers = true; break; }
            if (covers) movers.emplace_back(mProbeSlots[i].framesSinceUpdate, i);
        }
        const size_t extra = std::min(size_t(mDynamicProbes), movers.size());
        std::partial_sort(movers.begin(), movers.begin() + std::ptrdiff_t(extra), movers.end(),
                          [](const std::pair<unsigned, size_t> &a,
                             const std::pair<unsigned, size_t> &b) { return a.first > b.first; });
        for (size_t k = 0; k < extra; ++k) {
            const size_t i = movers[k].second;
            // Count only captures the reservation ADDS: a probe something else
            // already dirtied this frame (a shape clamp's CubemapProbe::set)
            // is going to render anyway, and reporting it as spent reservation
            // would over-state giStatus.dynamicProbeUpdates.
            if (!probes[i]->mDirty) ++mDynamicProbeUpdates;
            probes[i]->mDirty = true;
            mProbeSlots[i].sweepPending = false;
            mProbeSlots[i].framesSinceUpdate = 0;
        }
    }
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
    unsigned long long h = 1469598103934665603ull;      // FNV-1a
    const auto fold = [&h](unsigned long long v) { h ^= v; h *= 1099511628211ull; };
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        const Ogre::Aabb a = const_cast<Ogre::Item *>(item)->getWorldAabbUpdated();
        const float quantum = giAabbQuantum(a);
        const Ogre::Vector3 mn = a.getMinimum(), mx = a.getMaximum();
        fold((unsigned long long)kv.first);
        for (size_t ax = 0; ax < 3u; ++ax) {
            fold((unsigned long long)(long long)std::floor(mn[ax] / quantum));
            fold((unsigned long long)(long long)std::floor(mx[ax] / quantum));
        }
    }
    return h;
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
// The document's value is the AT-REST one and defaults to 1.0, i.e. nothing
// changes for a scene that never touches it. The engine raises it on ONE path:
// the cheap re-injection that runs every few frames WHILE something is being
// dragged (refreshGiLighting). That injection exists so bounced light follows
// the drag; it is replaced by a full re-solve the moment the drag stops, so
// spending less time on it is exactly the right trade — and it is the only
// place in the engine where "this answer is about to be thrown away" is true.
float OgreScene::giRayMarchStepScale(bool inMotion) const {
    static const float kMotionRayMarchStepScale = 2.0f;
    const float rest = std::max(1.0f, mGi.rayMarchStepScale);
    return inMotion ? std::max(rest, kMotionRayMarchStepScale) : rest;
}

void OgreScene::rebuildGi() {
    ++mGiRebuilds;
    // Every early return below leaves "nothing built" showing in giStatus.
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    Ogre::Light *driver = markGiLight(mGi.irLight);
    Ogre::Vector3 mn, mx;
    if (!driver || !computeGiBounds(mn, mx)) {
        mInstantRadiosity->clear();   // nothing to bounce (yet); stay armed
        return;
    }
    // One area of interest covering the GI bounds. Directional rays start
    // outside the sphere so nearby geometry occludes correctly.
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
    mGiLitVolume = aabb;
    noteGiAutoVolume(aabb, !giBoundsExplicit());
    mInstantRadiosity->mAoI.clear();
    mInstantRadiosity->mAoI.push_back(
        Ogre::InstantRadiosity::AreaOfInterest(aabb, aabb.getRadius() * 2.0f));
    mInstantRadiosity->build();
    // Diagnostic: JAHSHAKA_GI_DEBUG=1 logs how many VPLs the trace planted.
    if (std::getenv("JAHSHAKA_GI_DEBUG")) {
        size_t vpls = 0;
        Ogre::ObjectMemoryManager &mm = mSceneMgr->_getLightMemoryManager();
        for (size_t rq = 0; rq < mm.getNumRenderQueues(); ++rq) {
            Ogre::ObjectData objData;
            const size_t total = mm.getFirstObjectData(objData, rq);
            for (size_t i = 0; i < total; i += ARRAY_PACKED_REALS) {
                for (size_t k = 0; k < ARRAY_PACKED_REALS && i + k < total; ++k) {
                    const Ogre::Light *l = static_cast<Ogre::Light *>(objData.mOwner[k]);
                    if (l && l->getType() == Ogre::Light::LT_VPL) {
                        ++vpls;
                        if (vpls <= 4) {
                            const Ogre::ColourValue c = l->getDiffuseColour();
                            const Ogre::Vector3 pos = l->getParentNode()->_getDerivedPosition();
                            Ogre::LogManager::getSingleton().logMessage(
                                "Jahshaka GI: vpl at " + Ogre::StringConverter::toString(pos) +
                                " diffuse " + Ogre::StringConverter::toString(c) +
                                " range " + std::to_string(l->getAttenuationRange()));
                        }
                    }
                }
                objData.advancePack();
            }
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: instant radiosity planted " + std::to_string(vpls) + " VPLs");
    }
}

void OgreScene::rebuildVct() {
    ++mGiRebuilds;
    // Every early return below leaves "nothing built" showing in giStatus.
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    // ALWAYS from scratch: VctVoxelizer keeps raw Item* until removeAllItems and
    // VctMaterial caches conversions by raw datablock pointer across builds — a
    // recycled address would alias. A fresh voxelizer per (re)build can't.
    teardownVct();

    Ogre::Vector3 mn, mx;
    if (!computeGiBounds(mn, mx)) return;   // nothing to voxelize (yet); stay armed via mGi
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);

    const size_t itemCount = buildVoxelArm(aabb);
    if (!itemCount) { teardownVct(); return; }   // stay armed; next churn re-flags

    hlmsPbs(mRoot)->setVctLighting(mVctLighting);
    sVctBindingOwner = this;
    // What this arm was built AT (B4): the reuse path re-runs it only while the
    // count still matches, i.e. while nothing it points into can have died.
    mGiBuiltGeneration = mGiDestroyGeneration;
    mGiReusedLastRefresh = false;

    mGiLitVolume = aabb;
    // The probe grid gets its OWN region — the free space, not the padded voxel
    // volume. computeProbeRegion's header is the whole argument (P4 finding 2).
    if (mGi.mode == GiMode::VctPccHybrid) {
        mGiProbeRegion = computeProbeRegion(aabb);
        buildPcc(mGiProbeRegion);
        // The probe grid now owns the shader's one env-probe slot, so the IBL
        // cubemap must come OFF every datablock — see the long note at
        // OgreScene::reflectionTexForDatablocks (OgreSky.cpp). Unconditional:
        // applyReflectionToAll is a no-op walk when there is no sky reflection.
        applyReflectionToAll();
    }
    // LAST, not beside `mGiLitVolume = aabb` above: computeProbeRegion calls
    // giItemBounds again, and the hysteresis floor inside it must see the same
    // record the lit volume was fitted against or the two could disagree about
    // which items exist.
    noteGiAutoVolume(aabb, !giBoundsExplicit());

    // The DDGI layer, over the volume this build just lit. After the VCT
    // binding (it takes the same process-wide ownership) and after the PCC
    // build (the field is diffuse-only; the probes keep the specular they had).
    // A no-op — including a teardown of any previous field — when the toggle is
    // off, which is what makes `rebuildVct` the single place the arm's shape is
    // decided.
    buildIrradianceField();

    if (std::getenv("JAHSHAKA_GI_DEBUG"))
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: voxelized " + std::to_string(itemCount) + " items at " +
            std::to_string(giVoxelResolution()) + "^3 over " + Ogre::StringConverter::toString(mn) +
            " .. " + Ogre::StringConverter::toString(mx) +
            (mPcc ? " (+PCC probe grid)" : ""));
}

// THE VOXEL ARM — the voxelizer and the lighting over `aabb`, from the live GI
// items. rebuildVct's first half, shared with freshVoxelArm (the material-edit
// re-solve, P7), so the two can never build the arm differently. Always a NEW
// voxelizer: its VctMaterial caches every datablock's conversion by raw pointer
// for its whole life, which is the rule this file's header is about.
size_t OgreScene::buildVoxelArm(const Ogre::Aabb &aabb) {
    // Quality -> voxel volume resolution (the memory/compute knob: 32^3 =~ fast
    // preview, 128^3 =~ crisp indirect shadows) and anisotropic cone mips.
    const Ogre::uint32 res = giVoxelResolution();
    const bool anisotropic = mGi.quality != GiQuality::Low;

    // World transforms must be current before voxelization (the sample calls
    // this before every voxelizeScene; outside the render loop it is a no-op
    // repeat at worst).
    mSceneMgr->updateSceneGraph();

    mVctVoxelizer = new Ogre::VctVoxelizer(
        Ogre::Id::generateNewId<Ogre::VctVoxelizer>(),
        mRoot->getRenderSystem(), mRoot->getHlmsManager(),
        true /*correctAreaLightShadows*/);
    mVctVoxelizer->setResolution(res, res, res);
    mVctVoxelizer->setRegionToVoxelize(false, aabb);

    size_t itemCount = 0;
    mVctItemIds.clear();
    for (auto &kv : mNodes) {
        Ogre::Item *item = kv.second.item;
        // PBR items only — the same set IR traces (never sky/overlays/billboards).
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        mVctVoxelizer->addItem(item, false);
        mVctItemIds.push_back(kv.first);     // what the reuse arm compares against (B4)
        ++itemCount;
    }
    if (!itemCount) {
        delete mVctVoxelizer; mVctVoxelizer = nullptr;
        mVctItemIds.clear();
        return 0;
    }

    mVctVoxelizer->dividideOctants(1u, 1u, 1u);
    mVctVoxelizer->build(mSceneMgr);

    mVctLighting = new Ogre::VctLighting(
        Ogre::Id::generateNewId<Ogre::VctLighting>(), mVctVoxelizer, anisotropic);
    // Document bounces are total (1 = one indirect bounce, which light injection
    // itself provides); VctLighting counts the extra propagation passes.
    const Ogre::uint32 extraBounces =
        Ogre::uint32(std::min(std::max(mGi.numBounces, 1), 4) - 1);
    mVctLighting->setAllowMultipleBounces(extraBounces > 0u);
    // The scene's ambient, BEFORE the first update(): the pair is read when the
    // probe const buffer is filled, and a volume built with black hemispheres
    // shows a black ambient for the frame between build and the next ambient
    // push. See applyVctAmbient (OgreScene.cpp) for why it is a genuine pair.
    applyVctAmbient();
    mVctLighting->update(mSceneMgr, extraBounces, 1.0f /*thinWallCounter*/, hasVctLights(),
                         giRayMarchStepScale(false));
    // The materials this voxelizer converted are the ones in force NOW.
    mGiBuiltMaterialGeneration = mGiMaterialGeneration;
    return itemCount;
}

// THE MATERIAL-EDIT RE-SOLVE (P7). A material edit destroys nothing, so the
// destruction-generation rule does not apply and the probe grid — whose shapes
// come from the room's geometry, not its colours — stays exactly as placed. But
// the voxelizer's VctMaterial holds each datablock's conversion from the moment
// it first saw it, so re-running THAT voxelizer would re-voxelize the old
// albedo. So the voxel half is rebuilt fresh: the irradiance field first (it
// holds the old VctLighting), then the lighting, then the voxelizer, then
// buildVoxelArm. The HlmsPbs pointer follows the new lighting only if it was
// pointing at the old one — a background scene must not snatch the binding
// (refreshVctFast's own rule).
bool OgreScene::freshVoxelArm(const Ogre::Aabb &aabb) {
    teardownIrradianceField();
    Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
    const bool wasBound = mVctLighting && pbs->getVctLighting() == mVctLighting;
    if (wasBound) pbs->setVctLighting(nullptr);
    delete mVctLighting;  mVctLighting = nullptr;
    delete mVctVoxelizer; mVctVoxelizer = nullptr;
    mVctItemIds.clear();
    if (!buildVoxelArm(aabb)) return false;
    if (wasBound) pbs->setVctLighting(mVctLighting);
    return true;
}

void OgreScene::buildPcc(const Ogre::Aabb &aabb) {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    mPccHdr = mPccShadowed = false;
    // The slots name probes that are about to be (re)created; the first
    // updateProbeBudget after the build re-sizes and re-fills them.
    mProbeSlots.clear();
    mProbeUpdatesPerFrame = 0;
    mDynamicProbes = mDynamicProbeUpdates = 0;
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
    const bool wantShadows = resolveToggle(mGi.probeShadows, mGi.quality == GiQuality::High);
    const char *probeWorkspace = "JahshakaPccProbeWorkspace";
    if (wantShadows) {
        if (cm->hasWorkspaceDefinition("JahshakaPccProbeWorkspaceShadows") &&
            cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName)) {
            probeWorkspace = "JahshakaPccProbeWorkspaceShadows";
            mPccShadowed = true;
        } else {
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: shadowed probe captures requested but no shadow node is "
                "defined; capturing unshadowed");
        }
    }
    mPcc = new Ogre::ParallaxCorrectedCubemapAuto(
        Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapAuto>(),
        mRoot, mSceneMgr, cm->getWorkspaceDefinition(probeWorkspace));

    if (!mGiCamera) mGiCamera = mSceneMgr->createCamera(processUniqueName("giPccCamera"));
    mGiCamera->setPosition(aabb.mCenter);

    const auto clampProbes = [](int n) { return Ogre::uint32(std::min(std::max(n, 1), 8)); };
    Ogre::uint32 numProbes[3] = { clampProbes(mGi.pccProbesX), clampProbes(mGi.pccProbesY),
                                  clampProbes(mGi.pccProbesZ) };
    Ogre::PccPerPixelGridPlacement placement;
    placement.setParallaxCorrectedCubemapAuto(mPcc);
    placement.setNumProbes(numProbes);
    placement.setFullRegion(aabb);
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

    // Quality -> probe face resolution (the probe render + memory knob).
    Ogre::uint32 probeRes = 256u;
    switch (mGi.quality) {
    case GiQuality::Low:    probeRes = 128u; break;
    case GiQuality::Medium: probeRes = 256u; break;
    case GiQuality::High:   probeRes = 512u; break;
    }
    // HDR PROBES (P3a). The main chain renders PFG_RGBA16_FLOAT (OgreChain.cpp),
    // so an LDR probe target clamps every value above 1.0 at CAPTURE time — i.e.
    // before the IBL convolution spreads a highlight across the mip chain, which
    // is exactly the moment the extra range is worth having. Everything
    // downstream is format-generic: the DepthCompressor writes its packed depth
    // into alpha (never sRGB-encoded either way, and more precise as float), and
    // buildEnd reads it back through TextureBox::getColourAt with the bind
    // texture's own format. The cost is 2x the probe VRAM, hence the High gate.
    mPccHdr = resolveToggle(mGi.probeHdr, mGi.quality == GiQuality::High);
    const Ogre::PixelFormatGpu probeFormat =
        mPccHdr ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM_SRGB;
    const float diag = aabb.getSize().length();
    placement.buildStart(probeRes, mGiCamera, probeFormat,
                         std::max(0.02f, diag * 0.001f), std::max(1.0f, diag * 2.0f));
    placement.buildEnd();   // reads probe depth back and re-fits probe shapes
    clampProbeShapesToRegion(aabb);
    // EVERY probe renders in the INLINE stage from now on (B2 point 2). Set once,
    // here, rather than flipped as probes come and go: in automatic mode this
    // selects a render stage, not an amount of work, and the budget already
    // decides how many probes render at all.
    for (Ogre::CubemapProbe *p : mPcc->getProbes()) p->mNumIterations = 1u;
    // The placement above captured the whole grid TWICE, synchronously
    // (buildStart and buildEnd each run updateAllDirtyProbes) — counted, so a
    // rebuild frame reports what it cost (GiStatus::probeCapturesLastFrame).
    mPlacementCapturesThisFrame += int(2u * mPcc->getProbes().size());
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
    if (std::getenv("JAHSHAKA_GI_DEBUG")) {
        Ogre::LogManager &lm = Ogre::LogManager::getSingleton();
        const auto toS = [](const Ogre::Vector3 &v) {
            return Ogre::StringConverter::toString(v);
        };
        lm.logMessage("Jahshaka GI: PCC region " + toS(aabb.getMinimum()) + " .. " +
                      toS(aabb.getMaximum()) + " grid " + std::to_string(numProbes[0]) + "x" +
                      std::to_string(numProbes[1]) + "x" + std::to_string(numProbes[2]) +
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
    // take their environment from the probes instead of from cone tracing while
    // dynamic probes are on, so a scene's ROUGH pixels change when the author
    // raises dynamicProbes above 0 even if nothing moves (measured on the same
    // room's floor pixel: 0.000 -> 0.059/1.000/0.059). The mirror-sharp pixels
    // do not move. Nothing changes for a scene at the default of 0.
    //
    // THE REFINEMENT THIS IS NOT (recorded, not built — it needs upstream): the
    // honest version of this rule is PER PROBE, since only the dynamic probes
    // are fresher than the voxels. `pccVctMinDistance` is a single pass
    // constant, and making it per-probe means a new field in the probe const
    // buffer plus the shader that reads it — an upstream change on both sides,
    // which §9 of the spec makes a stop-and-report rather than a lane decision.
    //
    // WHAT THE FIX WAVE CHANGED HERE (B1/B2): the gate used to be
    // `dynamicProbes > 0`, a knob that defaulted to 0, so this inversion was
    // opt-in and almost nobody saw it. The realtime budget model defaults to 1,
    // so it is now the SHIPPED look for every hybrid scene that has not paused
    // GI — and that is stated loudly here, in the panel's tooltip and in the
    // spec rather than discovered later. The freshness gap the inversion papers
    // over is narrowed by B3 (a moving object re-dirties the probes that cover
    // it and re-injects the lights on the cheap cadence, so the voxels are much
    // less stale than they used to be during a drag), and the principled
    // per-probe fix is still upstream's to make.
    if (mGi.updateBudget > 0) minDist = std::max(minDist, diag);
    mPccBindMinDist = minDist;
    mPccBindMaxDist = minDist * 2.0f;
    hlmsPbs(mRoot)->setParallaxCorrectedCubemap(mPcc, mPccBindMinDist, mPccBindMaxDist);
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
// case at all — its teardown only unbinds when it is the owner). The shared
// raster CubemapToIfd parameter needs nothing here: updateIrradianceField
// re-pushes it before every raster dispatch, for exactly this reason.
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
        if (mPcc) pbs->setParallaxCorrectedCubemap(mPcc, mPccBindMinDist, mPccBindMaxDist);
        else      pbs->setParallaxCorrectedCubemap(nullptr);
        pbs->setIrradianceField(mIfd);
        const bool owns = mVctLighting || mPcc || mIfd;
        sVctBindingOwner = owns ? this : nullptr;
        if (std::getenv("JAHSHAKA_GI_DEBUG"))
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
    // (GI_UNIFIED P2: the Rayon tier writes a concrete 0/1 through into
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

    // Upstream's dispatch arithmetic, reproduced because the engine must obey
    // it rather than hope (OgreIrradianceField::update, and spike §4):
    //     rays        = ppf * depthRes^2 * raysPerPixel
    //     tpg         = alignToNextMultiple( 128, raysPerIrradiancePixel )
    //     workGroups  = rays / tpg              <-- INTEGER division
    // `rays < tpg` therefore dispatches ZERO work groups, which throws inside
    // HlmsCompute::compileShader and — uncaught, at frame time — terminates the
    // process. The OGRE_ASSERT_LOW that would have caught it in a debug Ogre is
    // compiled out of ours. `rays % tpg == 0` is the softer rule (a leftover
    // silently mis-sizes the dispatch; measured harmless at this pin, obeyed
    // anyway).
    const Ogre::uint32 tpg =
        Ogre::alignToNextMultiple<Ogre::uint32>(128u, settings.getNumRaysPerIrradiancePixel());
    const Ogre::uint32 raysPerProbe = Ogre::uint32(settings.mDepthProbeResolution) *
                                      settings.mDepthProbeResolution * settings.mNumRaysPerPixel;
    if (!raysPerProbe) return 0u;

    // The budget's meaning here: at budget 1 the field re-converges in
    // kIfdConvergeFrames frames, and the cost scales linearly with the dial
    // like every other GI budget. Rounded UP to a power of two so the batch
    // always divides a power-of-two field exactly — which is what guarantees
    // the LAST batch is the same size as every other one, and therefore that
    // the crash floor below holds for every dispatch and not just the first.
    Ogre::uint64 desired =
        (Ogre::uint64(totalProbes) * Ogre::uint64(updateBudget) + kIfdConvergeFrames - 1u) /
        kIfdConvergeFrames;
    Ogre::uint32 ppf = 1u;
    while (Ogre::uint64(ppf) < desired && ppf < totalProbes) ppf <<= 1u;
    while (Ogre::uint64(ppf) * raysPerProbe < tpg && ppf < totalProbes) ppf <<= 1u;   // the floor
    while (ppf < totalProbes && (Ogre::uint64(ppf) * raysPerProbe) % tpg != 0u) ppf <<= 1u;
    if (ppf > totalProbes) ppf = totalProbes;
    return ppf;
}

void OgreScene::buildIrradianceField() {
    // Called on every VCT (re)build, including the ones that must DROP the
    // field (the toggle went off, the mode left VCT, the arm failed to build).
    if (!ddgiWanted() || !mVctLighting || !mVctVoxelizer) { teardownIrradianceField(); return; }

    JAH_TRY {
        Ogre::IrradianceFieldSettings settings;
        settings.mNumRaysPerPixel        = kIfdRaysPerPixel;
        settings.mDepthProbeResolution   = kIfdDepthRes;
        settings.mIrradianceResolution   = kIfdIrradRes;
        // SCENE-FITTED, NEVER CAMERA-CENTRED. The volume is the one the
        // voxelizer was given — REFLECTIONS P5b measured a camera-centred GI
        // volume deleting the bounce outright (floor 0.475 -> 0.353 = off), and
        // that finding is the design constraint here, not a preference.
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
        if (!mIfd) mIfd = new JahIrradianceField(mRoot, mSceneMgr);
        mIfd->initialize(settings, origin, size, mVctLighting);
        mIfdSource = GiSource::Voxel;
        mIfdRasterFar = 0.0f;
        mIfdTotalProbes    = total;
        mIfdProbesDone     = 0u;
        mIfdProbesPerFrame = ifdProbesPerFrame(settings, mGi.updateBudget, total);
        mIfdMinProbes      = 0u;
        {
            // The smallest batch that still dispatches at least one work group.
            const Ogre::uint32 tpg = Ogre::alignToNextMultiple<Ogre::uint32>(
                128u, settings.getNumRaysPerIrradiancePixel());
            const Ogre::uint32 raysPerProbe = Ogre::uint32(settings.mDepthProbeResolution) *
                                              settings.mDepthProbeResolution *
                                              settings.mNumRaysPerPixel;
            mIfdMinProbes = raysPerProbe ? Ogre::uint32((tpg + raysPerProbe - 1u) / raysPerProbe) : 1u;
        }

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
        mIfdRigEpochSeen = mRigPoseEpoch;

        // THE RASTER SOURCE (GI_UNIFIED_SPEC.md P3 "A2", rayon2 S3), switched
        // in AFTER the voxel converge above so the atlases start from the voxel
        // answer (JahIrradianceField). Refused — logged, the field stays
        // voxel-fed and giStatus says so — when the raster workspace is not
        // staged or the media predates patch 0023 (world-unit depths would
        // defeat the cage visibility test the field exists for).
        if (resolveSource() == GiSource::Raster) applyRasterSource(settings, origin, size);

        // Our two scalars, and the two probe counts the shader's sky-visibility
        // threshold needs but upstream's own IrradianceField block does not
        // carry, reach the shader through the pass buffer.
        pushIfdState(settings);
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

        if (std::getenv("JAHSHAKA_GI_DEBUG"))
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: DDGI field " + std::to_string(settings.mNumProbes[0]) + "x" +
                std::to_string(settings.mNumProbes[1]) + "x" +
                std::to_string(settings.mNumProbes[2]) + " (" + std::to_string(total) +
                " probes) over " + Ogre::StringConverter::toString(origin) + " size " +
                Ogre::StringConverter::toString(size) + ", intensity " +
                std::to_string(mGi.ddgiIntensity) + ", ambient " +
                std::to_string(mGi.ddgiAmbient) + ", re-converge " +
                std::to_string(mIfdProbesPerFrame) + " probes/frame, source " +
                (mIfdSource == GiSource::Raster ? "raster" : "voxel"));
    } JAH_CATCH(mError, );
}

GiSource OgreScene::resolveSource() const {
    // Auto is the tier's choice, and the tier's choice is voxel at EVERY tier
    // (Epic included, by decree): raster is an Advanced opt-in, never a default.
    return mGi.ddgiSource == GiSource::Raster ? GiSource::Raster : GiSource::Voxel;
}

void OgreScene::pushIfdState(const Ogre::IrradianceFieldSettings &settings) {
    FogHlmsListener::IfdState st;
    st.intensity  = std::max(0.0f, std::min(mGi.ddgiIntensity, 64.0f));
    st.ambient    = std::max(0.0f, std::min(mGi.ddgiAmbient, 8.0f));
    st.numProbesY = float(settings.mNumProbes[1]);
    st.numProbesZ = float(settings.mNumProbes[2]);
    if (mIfd && mIfdSource == GiSource::Raster) {
        // THE RASTER ESCAPE VECTOR: a miss is stored at far x dot( |dir|,
        // probesPerUnit ) (CubemapToIfd with patch 0023), so the shader's
        // integrated threshold is dot( A(d), far x scale x probesPerUnit ).
        const Ogre::Vector3 fieldSize = mIfd->enlargedFieldSize();
        const Ogre::Vector3 probesPerUnit(
            float(settings.mNumProbes[0]) / std::max(fieldSize.x, 1e-4f),
            float(settings.mNumProbes[1]) / std::max(fieldSize.y, 1e-4f),
            float(settings.mNumProbes[2]) / std::max(fieldSize.z, 1e-4f));
        const Ogre::Vector3 esc = probesPerUnit * (mIfdRasterFar * kIfdRasterEscapeScale);
        st.escapeX = esc.x; st.escapeY = esc.y; st.escapeZ = esc.z;
        st.rasterSource = 1.0f;
        // A raster probe SEES the sky (RQ 0 is inside the capture range), so
        // with a sky bound the field already carries the sky's light and the
        // ambient proxy would count it twice: off. With no sky the captures
        // clear to black and the proxy is the only ambient there is: on.
        if (mSceneMgr->getSky()) st.ambient = 0.0f;
    }
    FogHlmsListener::setIfdState(mSceneMgr, st);
}

void OgreScene::applyRasterSource(const Ogre::IrradianceFieldSettings &settings,
                                  const Ogre::Vector3 &origin, const Ogre::Vector3 &size) {
    (void)origin;
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    const char *workspace = "JahshakaIfdRasterWorkspace";
    if (!cm->hasWorkspaceDefinition(workspace)) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: raster probe source requested but JahshakaIfdRasterWorkspace is not "
            "staged; the field stays voxel-fed");
        return;
    }
    // Patch 0023's parameter must exist or the raster depths are in world units
    // and the field leaks through everything (the refusal is deliberate).
    if (!setIfdRasterInvFieldSize(mRoot, Ogre::Vector3(1.0f, 1.0f, 1.0f))) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: raster probe source requested but the staged IrradianceField media "
            "predates ogre-patch 0023 (no invFieldSize on CubemapToIfd); the field stays "
            "voxel-fed");
        return;
    }
    // Shadowed captures follow the same P3b rule as the reflection probes:
    // only when the PROBE shadow node exists (Ogre THROWS at workspace
    // creation otherwise), and only when the quality dial asks for them. The
    // raster field owns ONE workspace (IrradianceFieldRaster::createWorkspace,
    // the camera moves per probe), not one per probe — it names the
    // quarter-resolution probe node because its faces are 32 px
    // (ClosestPow2(kIfdDepthRes*2)): the view's 2048 atlas would be 80 MB of
    // resolution no face can observe.
    const bool wantShadows = resolveToggle(mGi.probeShadows, mGi.quality == GiQuality::High);
    mIfdShadowed = false;
    if (wantShadows && cm->hasWorkspaceDefinition("JahshakaIfdRasterWorkspaceShadows") &&
        cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName)) {
        workspace = "JahshakaIfdRasterWorkspaceShadows";
        mIfdShadowed = true;
    }

    Ogre::RasterParams rp;
    rp.mWorkspaceName = workspace;
    rp.mPixelFormat   = Ogre::PFG_RGBA8_UNORM_SRGB;    // upstream's default; the atlas is LDR anyway
    // Near: a sliver of the smallest probe cell (the probe sits in free space
    // by construction, but a wall a hair away must still register). Far: twice
    // the ENLARGED field's diagonal, so a miss reads as exactly far.
    const Ogre::Vector3 cell(size.x / float(settings.mNumProbes[0]),
                             size.y / float(settings.mNumProbes[1]),
                             size.z / float(settings.mNumProbes[2]));
    const float minCell = std::max(std::min(std::min(cell.x, cell.y), cell.z), 1e-3f);
    rp.mCameraNear = std::max(0.005f * minCell, 1e-3f);
    const Ogre::Vector3 enlarged = size + cell * 2.0f;
    rp.mCameraFar  = std::max(enlarged.length() * kIfdRasterFarDiagonals, rp.mCameraNear * 10.0f);

    mIfd->sourceFromRaster(rp);
    mIfdSource    = GiSource::Raster;
    mIfdRasterFar = rp.mCameraFar;
    // The grid-unit conversion, from the field the probes actually integrate
    // over (initialize enlarged it by one cell per side; enlargedFieldSize is
    // that number, not our `size`).
    const Ogre::Vector3 fieldSize = mIfd->enlargedFieldSize();
    setIfdRasterInvFieldSize(mRoot, Ogre::Vector3(1.0f / std::max(fieldSize.x, 1e-4f),
                                                  1.0f / std::max(fieldSize.y, 1e-4f),
                                                  1.0f / std::max(fieldSize.z, 1e-4f)));
    // The raster re-converge: the same dial, in raster probes. No thread-group
    // floor here (the raster path renders whole probes), so the only clamp is
    // the field itself. NOT converged inline: that is 8192 x 6 scene renders.
    mIfdProbesDone     = 0u;
    mIfdMinProbes      = 1u;
    mIfdProbesPerFrame = ifdRasterProbesPerFrame(mGi.updateBudget, mIfdTotalProbes);
    mIfdRigEpochSeen   = mRigPoseEpoch;
}

Ogre::uint32 OgreScene::ifdRasterProbesPerFrame(int updateBudget, Ogre::uint32 totalProbes) {
    if (updateBudget <= 0 || totalProbes == 0u) return 0u;
    const Ogre::uint64 want = Ogre::uint64(updateBudget) * kIfdRasterProbesPerBudget;
    return Ogre::uint32(std::min<Ogre::uint64>(want, totalProbes));
}

void OgreScene::teardownIrradianceField() {
    mIfdTotalProbes = mIfdProbesDone = mIfdProbesPerFrame = mIfdMinProbes = 0u;
    mIfdSource = GiSource::Voxel;
    mIfdRasterFar = 0.0f;
    mIfdShadowed = false;
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
    // PAUSED (budget 0): nothing re-converges, and update() is not called at
    // all — which is also what keeps the zero-work-group abort unreachable on
    // this path.
    if (!mIfdProbesPerFrame) return;
    const bool raster = mIfdSource == GiSource::Raster;
    if (raster && mIfdProbesDone >= mIfdTotalProbes) {
        // A CONVERGED RASTER FIELD RE-ARMS ON MOVEMENT — the whole point of
        // the source: two signals, either one restarts the sweep. The movement
        // scan (an item's AABB moved past sub-voxel jitter) is the same one the
        // reflection probes read, run once per frame by whichever consumer
        // comes first; the rig epoch is for what that scan cannot see — a
        // skinned character posing in place keeps its bind-pose bounds. A
        // sweep in flight is never restarted (that would starve the far
        // probes); the epoch is re-read when the next one starts.
        JAH_TRY { ensureGiWalk(); } JAH_CATCH(mError, );
        const bool moved = !mGiMovedBoxes.empty() || mRigPoseEpoch != mIfdRigEpochSeen;
        if (!moved) return;
        mIfdRigEpochSeen = mRigPoseEpoch;
        JAH_TRY { mIfd->reset(); } JAH_CATCH(mError, );
        mIfdProbesDone = 0u;
    }
    if (mIfdProbesDone >= mIfdTotalProbes) return;              // converged (voxel)
    JAH_TRY {
        if (raster) {
            // The shared CubemapToIfd job: patch 0023's conversion, every time,
            // because another scene's raster field may have set its own.
            const Ogre::Vector3 fieldSize = mIfd->enlargedFieldSize();
            setIfdRasterInvFieldSize(mRoot, Ogre::Vector3(1.0f / std::max(fieldSize.x, 1e-4f),
                                                          1.0f / std::max(fieldSize.y, 1e-4f),
                                                          1.0f / std::max(fieldSize.z, 1e-4f)));
        }
        const Ogre::uint32 remaining = mIfdTotalProbes - mIfdProbesDone;
        const Ogre::uint32 batch = std::min(mIfdProbesPerFrame, remaining);
        // THE CRASH FLOOR, checked at the dispatch rather than trusted from the
        // arithmetic that chose the batch. It cannot fire as things stand — the
        // batch and the field are both powers of two, so the batch divides the
        // field and every dispatch is a full batch — and it is here because the
        // failure it guards is not a glitch but an uncaught throw at frame time
        // that takes the process with it (spike §4; the guarding assert is
        // compiled out of our Ogre). Declaring the field converged leaves a few
        // probes on their previous data, which is wrong pixels; dispatching
        // would be no pixels at all.
        if (batch < mIfdMinProbes) {
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: DDGI re-converge stopped " + std::to_string(remaining) +
                " probes short — a batch of " + std::to_string(batch) +
                " is below the dispatch floor of " + std::to_string(mIfdMinProbes));
            mIfdProbesDone = mIfdTotalProbes;
            return;
        }
        mIfd->update(batch);
        mIfdProbesDone = std::min(mIfdTotalProbes, mIfdProbesDone + batch);
    } JAH_CATCH(mError, );
}

void OgreScene::teardownVct() {
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
    mDynamicProbes = mDynamicProbeUpdates = 0;
    mProbesClampedToRegion = 0;
    mVctItemIds.clear();
    mGiBuiltGeneration = ~0ull;      // nothing built: the reuse arm must refuse
    mGiReusedLastRefresh = false;
    // Unbind what the shader reads FROM THIS SCENE, by pointer identity (the
    // same rule teardownIrradianceField uses): the owner flag can disagree with
    // the pointers, and a pointer left bound past the delete below is a
    // use-after-free on the next frame. Another scene's binding is untouched.
    {
        Ogre::HlmsPbs *pbs = hlmsPbs(mRoot);
        if (mPcc && pbs->getParallaxCorrectedCubemap() == mPcc) pbs->setParallaxCorrectedCubemap(nullptr);
        if (mVctLighting && pbs->getVctLighting() == mVctLighting) pbs->setVctLighting(nullptr);
        if (sVctBindingOwner == this) sVctBindingOwner = nullptr;
    }
    // Reverse dependency order, all while the SceneManager is still alive:
    // PCC (probe workspaces + cubemap textures) -> VctLighting (reads the
    // voxelizer's textures) -> VctVoxelizer (drops its MeshPtr refs).
    delete mPcc;          mPcc = nullptr;
    delete mVctLighting;  mVctLighting = nullptr;
    delete mVctVoxelizer; mVctVoxelizer = nullptr;
    if (mGiCamera) { mSceneMgr->destroyCamera(mGiCamera); mGiCamera = nullptr; }
    // ...and back ON now that the slot is free again (the mirror of the call in
    // rebuildVct). Ordered after `delete mPcc` because the helper reads it.
    applyReflectionToAll();
}

void OgreScene::teardownIr() {
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    if (!mInstantRadiosity) return;
    delete mInstantRadiosity;   // ~InstantRadiosity clears the VPLs
    mInstantRadiosity = nullptr;
    if (mSceneMgr && mSceneMgr->getForwardPlus())
        mSceneMgr->getForwardPlus()->setEnableVpls(false);
}

void OgreScene::teardownGi() {
    teardownIr();
    teardownVct();
}

// ---------------------------------------------------------------------------
// The shadow-atlas rebuild's GI half (SHADOW_TOOLING_SPEC.md risk R3)
// ---------------------------------------------------------------------------
// A shadow-node DEFINITION cannot be deleted while anything instantiates it,
// and the shadowed probe captures do: each PCC probe workspace and the raster
// field's one workspace name JahshakaProbeShadowNode, so the probe arm holds live
// CompositorShadowNodes exactly like a view's workspace does. Before this
// existed, changing the shadow resolution with hybrid GI at high quality left
// those instances pointing at freed definition memory and the next frame died
// inside Hlms::preparePassHashBase — reproduced as a SEGV by tests/shadow's r3
// mode. It could only ever fire on a Shadow Quality change; the derived map
// count (which grows when a lamp is added) would have made it routine.
//
// The teardown is the whole VCT arm, not just the probes, because that is the
// only honest granularity here: rebuildVct's own comment is the reason (raw
// Item* and datablock-pointer caches make anything but from-scratch an aliasing
// risk), and this path runs on a Shadow Quality change or an atlas growth — not
// per frame.
bool OgreScene::dropGiForShadowRebuild() {
    // BOTH shadowed arms hold live CompositorShadowNodes on the probe
    // definition: the PCC probes (one workspace each) AND the raster
    // IrradianceField (ONE workspace per field, kept for the field's whole
    // life in IrradianceFieldRaster::mRenderWorkspace). VCT + DDGI-raster at
    // High with no PCC reached here with mPcc null, returned false, and the
    // atlas rebuild deleted the definition under the field's workspace: a
    // use-after-free at the next raster sweep or at teardown
    // (~CompositorNode reads mDefinition). Code review 2026-09-10.
    const bool pccShadowed = mPcc && mPccShadowed;
    const bool ifdShadowed = mIfd && mIfdShadowed;
    if (!pccShadowed && !ifdShadowed) return false;
    JAH_TRY { teardownVct(); } JAH_CATCH(mError, false);
    return true;
}

void OgreScene::recreateGiAfterShadowRebuild() {
    JAH_TRY { rebuildVct(); } JAH_CATCH(mError, );
}

}}}  // namespace jahshaka::engine::detail
