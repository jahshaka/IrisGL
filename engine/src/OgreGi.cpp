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

bool OgreScene::setGlobalIllumination(const GiParams &p) {
    JAH_TRY {
        switch (p.mode) {
        default:
        case GiMode::Off:
            teardownGi();
            mGi = p;
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
        else if (mGi.mode == GiMode::Vct || mGi.mode == GiMode::VctPccHybrid)
            rebuildVct();
    } JAH_CATCH(mError, );
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
        mVctLighting->update(mSceneMgr, extraBounces);
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
// rejection below. Everything that reasons about the shape of the lit world
// starts here so that the lit volume and the probe region can never disagree
// about which objects define it.
//
// OUTLIER REJECTION (REFLECTIONS_ADOPTION_SPEC.md P1a.1): take each item's
// LARGEST world-AABB extent, take the median of those, and drop items whose
// largest extent is more than 4x the median. The case it exists for is the one
// every default scene has: a ground plane 200 units across sitting under a
// handful of 1-2 unit primitives. Unioning it puts the voxel volume and the
// probe grid over 40,000 square units of empty air — the "basic cubemap" look
// GI_SPEC blamed on probe count. The median (not the mean) is what makes one
// enormous object unable to drag the threshold up to cover itself.
//
// It only runs with FOUR items or more: below that "the median" is not a
// statement about a population, and a three-object scene where one object is
// genuinely the subject would lose it. The document's explicit exclude flag is
// the deterministic escape hatch when the heuristic guesses wrong either way.
std::vector<Ogre::Aabb> OgreScene::giItemBounds() const {
    std::vector<Ogre::Aabb> all;
    all.reserve(mNodes.size());
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        if (kv.second.giBoundsExcluded) continue;
        all.push_back(const_cast<Ogre::Item *>(item)->getWorldAabbUpdated());
    }
    if (all.size() < 4u) return all;

    std::vector<float> extents;
    extents.reserve(all.size());
    for (const Ogre::Aabb &a : all) {
        const Ogre::Vector3 s = a.getSize();
        extents.push_back(std::max(std::max(s.x, s.y), s.z));
    }
    std::vector<float> sorted = extents;
    std::sort(sorted.begin(), sorted.end());
    const size_t n = sorted.size();
    const float median = (n % 2u) ? sorted[n / 2u]
                                  : 0.5f * (sorted[n / 2u - 1u] + sorted[n / 2u]);
    if (median <= 0.0f) return all;          // degenerate (all points) — keep everything
    const float limit = median * 4.0f;

    std::vector<Ogre::Aabb> kept;
    kept.reserve(all.size());
    for (size_t i = 0; i < all.size(); ++i)
        if (extents[i] <= limit) kept.push_back(all[i]);
    // Never return nothing: if the filter somehow ate the whole scene the plain
    // union is a worse answer than no answer at all.
    return kept.empty() ? all : kept;
}

bool OgreScene::computeGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const {
    const Vec3 &a = mGi.boundsMin, &b = mGi.boundsMax;
    if (a.x != b.x || a.y != b.y || a.z != b.z) {
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
// for a direction when it lies wholly on that side of the hull's centre and
// spans at least half of the hull on both other axes; furniture and the subject
// of the scene fail that test and are ignored. In an open scene no wall is
// found on most axes and the tight hull stands, which is the right answer
// there.
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

    for (size_t ax = 0; ax < 3u; ++ax) {
        const size_t o1 = (ax + 1u) % 3u, o2 = (ax + 2u) % 3u;
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
            if (amn[ax] > centre[ax] && amn[ax] < nearestMax) nearestMax = amn[ax];
            if (amx[ax] < centre[ax] && amx[ax] > nearestMin) nearestMin = amx[ax];
        }
        // Only accept the pull if it leaves a real volume behind.
        if (nearestMin < nearestMax) { mn[ax] = nearestMin; mx[ax] = nearestMax; }
    }
    return Ogre::Aabb::newFromExtents(mn, mx);
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

void OgreScene::updateGiTracking(const Ogre::Vector3 &camPos) {
    if (!mPcc || !mGiCamera) return;
    JAH_TRY {
        mGiCamera->setPosition(camPos);
        mPcc->setUpdatedTrackedDataFromCamera(mGiCamera);
    } JAH_CATCH(mError, );
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
    for (auto &kv : mNodes) {
        Ogre::Item *item = kv.second.item;
        // PBR items only — the same set IR traces (never sky/overlays/billboards).
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        mVctVoxelizer->addItem(item, false);
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
    mVctLighting->update(mSceneMgr, extraBounces);

    hlmsPbs(mRoot)->setVctLighting(mVctLighting);
    sVctBindingOwner = this;

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
    const float minDist = std::max(0.25f, diag * 0.05f);
    hlmsPbs(mRoot)->setParallaxCorrectedCubemap(mPcc, minDist, minDist * 2.0f);
}

void OgreScene::teardownVct() {
    mGiLitVolume = mGiProbeRegion = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    mPccHdr = mPccShadowed = false;
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
