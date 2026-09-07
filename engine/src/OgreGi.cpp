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
        mVctLighting->update(mSceneMgr, extraBounces, 1.0f /*thinWallCounter*/, hasVctLights());
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
        mSceneMgr->setForwardClustered(true, 16, 8, 24, 96, kDecalsPerCell, 8, wantMin, wantMax);
        mFwdPlusMin = wantMin; mFwdPlusMax = wantMax;
    } JAH_CATCH(mError, );
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
    // The scene's ambient, BEFORE the first update(): the pair is read when the
    // probe const buffer is filled, and a volume built with black hemispheres
    // shows a black ambient for the frame between build and the next ambient
    // push. See applyVctAmbient (OgreScene.cpp) for why it is a genuine pair.
    applyVctAmbient();
    mVctLighting->update(mSceneMgr, extraBounces, 1.0f /*thinWallCounter*/, hasVctLights());

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
