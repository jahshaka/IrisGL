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

bool OgreScene::computeGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const {
    const Vec3 &a = mGi.boundsMin, &b = mGi.boundsMax;
    if (a.x != b.x || a.y != b.y || a.z != b.z) {
        mn = Ogre::Vector3(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
        mx = Ogre::Vector3(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
        return true;
    }
    bool any = false;
    mn = Ogre::Vector3(1e30f); mx = Ogre::Vector3(-1e30f);
    for (const auto &kv : mNodes) {
        const Ogre::Item *item = kv.second.item;
        if (!item || !(item->getVisibilityFlags() & kGiGeometryBit)) continue;
        const Ogre::Aabb aabb = const_cast<Ogre::Item *>(item)->getWorldAabbUpdated();
        mn.makeFloor(aabb.getMinimum());
        mx.makeCeil(aabb.getMaximum());
        any = true;
    }
    if (!any) return false;
    const Ogre::Vector3 margin = (mx - mn) * 0.1f + Ogre::Vector3(0.5f);
    mn -= margin; mx += margin;
    return true;
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
    Ogre::Light *driver = markGiLight(mGi.irLight);
    Ogre::Vector3 mn, mx;
    if (!driver || !computeGiBounds(mn, mx)) {
        mInstantRadiosity->clear();   // nothing to bounce (yet); stay armed
        return;
    }
    // One area of interest covering the GI bounds. Directional rays start
    // outside the sphere so nearby geometry occludes correctly.
    const Ogre::Aabb aabb = Ogre::Aabb::newFromExtents(mn, mx);
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
    // ALWAYS from scratch: VctVoxelizer keeps raw Item* until removeAllItems and
    // VctMaterial caches conversions by raw datablock pointer across builds — a
    // recycled address would alias. A fresh voxelizer per (re)build can't.
    teardownVct();

    Ogre::Vector3 mn, mx;
    if (!computeGiBounds(mn, mx)) return;   // nothing to voxelize (yet); stay armed via mGi

    // Quality -> voxel volume resolution (the memory/compute knob: 32^3 =~ fast
    // preview, 128^3 =~ crisp indirect shadows) and anisotropic cone mips.
    Ogre::uint32 res = 64u; bool anisotropic = true;
    switch (mGi.quality) {
    case GiQuality::Low:    res = 32u;  anisotropic = false; break;
    case GiQuality::Medium: res = 64u;  anisotropic = true;  break;
    case GiQuality::High:   res = 128u; anisotropic = true;  break;
    }

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

    if (mGi.mode == GiMode::VctPccHybrid) buildPcc(aabb);

    if (std::getenv("JAHSHAKA_GI_DEBUG"))
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: voxelized " + std::to_string(itemCount) + " items at " +
            std::to_string(res) + "^3 over " + Ogre::StringConverter::toString(mn) +
            " .. " + Ogre::StringConverter::toString(mx) +
            (mPcc ? " (+PCC probe grid)" : ""));
}

void OgreScene::buildPcc(const Ogre::Aabb &aabb) {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    // Our own probe workspace (media/Hlms/Jahshaka/JahshakaPcc.compositor):
    // per-face scene render + PCC depth compression + IBL specular mips — the
    // sample's LocalCubemapsProbeWorkspace minus its shadow node.
    if (!cm->hasWorkspaceDefinition("JahshakaPccProbeWorkspace")) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: PCC probe workspace missing; hybrid renders as plain VCT");
        return;
    }
    mPcc = new Ogre::ParallaxCorrectedCubemapAuto(
        Ogre::Id::generateNewId<Ogre::ParallaxCorrectedCubemapAuto>(),
        mRoot, mSceneMgr, cm->getWorkspaceDefinition("JahshakaPccProbeWorkspace"));

    if (!mGiCamera) mGiCamera = mSceneMgr->createCamera(processUniqueName("giPccCamera"));
    mGiCamera->setPosition(aabb.mCenter);

    const auto clampProbes = [](int n) { return Ogre::uint32(std::min(std::max(n, 1), 8)); };
    Ogre::uint32 numProbes[3] = { clampProbes(mGi.pccProbesX), clampProbes(mGi.pccProbesY),
                                  clampProbes(mGi.pccProbesZ) };
    Ogre::PccPerPixelGridPlacement placement;
    placement.setParallaxCorrectedCubemapAuto(mPcc);
    placement.setNumProbes(numProbes);
    placement.setFullRegion(aabb);
    placement.setOverlap(Ogre::Vector3(1.5f));

    // Quality -> probe face resolution (the probe render + memory knob).
    Ogre::uint32 probeRes = 256u;
    switch (mGi.quality) {
    case GiQuality::Low:    probeRes = 128u; break;
    case GiQuality::Medium: probeRes = 256u; break;
    case GiQuality::High:   probeRes = 512u; break;
    }
    const float diag = aabb.getSize().length();
    placement.buildStart(probeRes, mGiCamera, Ogre::PFG_RGBA8_UNORM_SRGB,
                         std::max(0.02f, diag * 0.001f), std::max(1.0f, diag * 2.0f));
    placement.buildEnd();   // reads probe depth back and re-fits probe shapes

    // The hybrid blend: reflections whose PCC-vs-VCT parallax error is below
    // minDistance come from the probes (near geometry, sharp), above maxDistance
    // from cone tracing (far, soft), faded in between. Scaled to the scene.
    const float minDist = std::max(0.25f, diag * 0.05f);
    hlmsPbs(mRoot)->setParallaxCorrectedCubemap(mPcc, minDist, minDist * 2.0f);
}

void OgreScene::teardownVct() {
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
}

void OgreScene::teardownIr() {
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
