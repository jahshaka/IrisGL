// Global illumination (GI_SPEC.md phase 1: Instant Radiosity) — the public verbs
// and the internals that drive Ogre::InstantRadiosity.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

bool OgreScene::setGlobalIllumination(const GiParams &p) {
    JAH_TRY {
        if (p.mode != GiMode::InstantRadiosity) {
            // Off — and the not-yet-implemented VCT modes render as off rather
            // than faking anything (documents saved with them still load).
            if ((p.mode == GiMode::Vct || p.mode == GiMode::VctPccHybrid) &&
                mGi.mode != p.mode) {
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka: GI mode not implemented yet (VCT); rendering without GI");
            }
            teardownGi();
            mGi = p;
            return true;
        }
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
    } JAH_CATCH(mError, false);
}

void OgreScene::refreshGlobalIllumination() {
    JAH_TRY {
        if (mInstantRadiosity && mGi.mode == GiMode::InstantRadiosity) rebuildGi();
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
    if (mInstantRadiosity) mGiCachesDirty = true;
}

void OgreScene::applyPendingGi() {
    if (!mGiCachesDirty) return;
    mGiCachesDirty = false;
    if (!mInstantRadiosity) return;
    JAH_TRY {
        mInstantRadiosity->freeMemory();   // drops the stale VAO/texture caches
        rebuildGi();                       // re-downloads from the LIVE scene
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

void OgreScene::teardownGi() {
    if (!mInstantRadiosity) return;
    delete mInstantRadiosity;   // ~InstantRadiosity clears the VPLs
    mInstantRadiosity = nullptr;
    if (mSceneMgr && mSceneMgr->getForwardPlus())
        mSceneMgr->getForwardPlus()->setEnableVpls(false);
}

}}}  // namespace jahshaka::engine::detail
