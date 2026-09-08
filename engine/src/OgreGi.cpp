// Global illumination (GI_SPEC.md phases 1-3): Instant Radiosity, voxel cone
// tracing (VCT) and the VCT + parallax-corrected-cubemap hybrid — the public
// verbs and the internals that drive Ogre's InstantRadiosity, VctVoxelizer/
// VctLighting and ParallaxCorrectedCubemapAuto.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

// HlmsPbs is a process-wide singleton: setVctLighting/setParallaxCorrectedCubemap
// bind globally. Exactly one scene owns that binding at a time (last enabler
// wins); teardown only unbinds when the dying scene is the owner, so a takeover
// never yanks the new owner's binding.
static OgreScene *sVctBindingOwner = nullptr;

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
//   * the probes keep their shapes and are simply re-dirtied. Re-running
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
        // Items born since the build. Nothing can have DIED (the generation says
        // so), so the voxelizer's item list only ever grows on this path.
        for (auto &kv : mNodes) {
            Ogre::Item *item = kv.second.item;
            if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
            if (std::find(mVctItemIds.begin(), mVctItemIds.end(), kv.first) != mVctItemIds.end())
                continue;
            mVctVoxelizer->addItem(item, false);
            mVctItemIds.push_back(kv.first);
        }
        // World transforms first: the voxelizer reads them, and the whole reason
        // this call exists is that something moved.
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
        // Dirty every probe and restart the sweep, so the budget spends itself on
        // a grid that all needs the same thing.
        if (mPcc) {
            const Ogre::CubemapProbeVec &probes = mPcc->getProbes();
            for (size_t i = 0; i < probes.size(); ++i) probes[i]->mDirty = true;
            for (ProbeSlot &s : mProbeSlots) s.sweepPending = true;
        }
        mGiReusedLastRefresh = true;
        if (std::getenv("JAHSHAKA_GI_DEBUG"))
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka GI: refresh REUSED the voxel arm (" +
                std::to_string(mVctItemIds.size()) + " items, probes re-dirtied)");
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
        st.probeRegionMin = toV(mGiProbeRegion.getMinimum());
        st.probeRegionMax = toV(mGiProbeRegion.getMaximum());
        // RESOLVED, not requested: both default to GiToggle::Auto, and the
        // shadow half additionally falls back when there is no shadow node.
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
    } JAH_CATCH(mError, st);
    return st;
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
        if (!l) continue;
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
void OgreScene::scanGiMovement() {
    mGiMovedBoxes.clear();
    size_t live = 0;
    for (auto &kv : mNodes) {
        Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        ++live;
        const Ogre::Aabb a = item->getWorldAabbUpdated();
        auto it = mGiItemAabbs.find(kv.first);
        if (it == mGiItemAabbs.end()) { mGiItemAabbs.emplace(kv.first, a); continue; }
        if (!giAabbMoved(it->second, a)) continue;
        Ogre::Aabb moved = it->second;
        moved.merge(a);
        mGiMovedBoxes.push_back(moved);
        it->second = a;
    }
    // Destroyed nodes leave entries behind. Bounded by the scene either way, but
    // a long editing session should not pay for every object it ever deleted.
    if (mGiItemAabbs.size() > live * 2u + 16u) {
        mGiItemAabbs.clear();
        for (auto &kv : mNodes) {
            Ogre::Item *item = kv.second.item;
            if (item && (item->getVisibilityFlags() & kGiGeometryBit))
                mGiItemAabbs.emplace(kv.first, item->getWorldAabbUpdated());
        }
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
//  3. A SWEEP, so the budget is a guarantee and not a heuristic. Every probe
//     carries "still owes this sweep an update"; the sweep refills when it
//     empties. So the whole grid refreshes within ceil(probes / budget) frames
//     no matter what the priority prefers — the property gi.budget pins — and
//     the priority (staleness x proximity x covers-a-moved-AABB) only decides
//     the ORDER within a sweep, which is where it matters: the probes the viewer
//     is looking at and the ones a moving object is inside come first.
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

    scanGiMovement();

    bool anyPending = false;
    for (const ProbeSlot &s : mProbeSlots) if (s.sweepPending) { anyPending = true; break; }
    if (!anyPending) for (ProbeSlot &s : mProbeSlots) s.sweepPending = true;

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
    for (size_t k = 0; k < take; ++k) {
        const size_t i = ranked[k].second;
        probes[i]->mDirty = true;
        mProbeSlots[i].sweepPending = false;
        mProbeSlots[i].framesSinceUpdate = 0;
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
    // Every early return below leaves "nothing built" showing in giStatus.
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    // ALWAYS from scratch: VctVoxelizer keeps raw Item* until removeAllItems and
    // VctMaterial caches conversions by raw datablock pointer across builds — a
    // recycled address would alias. A fresh voxelizer per (re)build can't.
    teardownVct();

    Ogre::Vector3 mn, mx;
    if (!computeGiBounds(mn, mx)) return;   // nothing to voxelize (yet); stay armed via mGi

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
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
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
    if (!itemCount) { teardownVct(); return; }   // stay armed; next churn re-flags

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

    if (std::getenv("JAHSHAKA_GI_DEBUG"))
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: voxelized " + std::to_string(itemCount) + " items at " +
            std::to_string(res) + "^3 over " + Ogre::StringConverter::toString(mn) +
            " .. " + Ogre::StringConverter::toString(mx) +
            (mPcc ? " (+PCC probe grid)" : ""));
}

void OgreScene::buildPcc(const Ogre::Aabb &aabb) {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    mPccHdr = mPccShadowed = false;
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
    const bool wantShadows = resolveToggle(mGi.probeShadows, mGi.quality == GiQuality::High);
    const char *probeWorkspace = "JahshakaPccProbeWorkspace";
    if (wantShadows) {
        if (cm->hasWorkspaceDefinition("JahshakaPccProbeWorkspaceShadows") &&
            cm->hasShadowNodeDefinition(OgreView::kShadowNodeName)) {
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
    mProbeSlots.assign(mPcc->getProbes().size(), ProbeSlot());
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
    hlmsPbs(mRoot)->setParallaxCorrectedCubemap(mPcc, minDist, minDist * 2.0f);
}

void OgreScene::teardownVct() {
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    mPccHdr = mPccShadowed = false;
    mProbeSlots.clear();
    mProbeUpdatesPerFrame = 0;
    mProbesClampedToRegion = 0;
    mVctItemIds.clear();
    mGiBuiltGeneration = ~0ull;      // nothing built: the reuse arm must refuse
    mGiReusedLastRefresh = false;
    if (sVctBindingOwner == this) {
        hlmsPbs(mRoot)->setParallaxCorrectedCubemap(nullptr);
        hlmsPbs(mRoot)->setVctLighting(nullptr);
        sVctBindingOwner = nullptr;
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

}}}  // namespace jahshaka::engine::detail
