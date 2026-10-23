// PHOTON, THE COMPONENT (photon/PhotonComponent.h has the design).
#include "photon/PhotonComponent.h"

#include "EnginePrivate.h"
#include "AtomPass.h"
#include "SurfaceCache.h"
#include "photon/PhotonDispatch.h"
#include "photon/voxel/PhotonIrradianceField.h"
#include "photon/voxel/PhotonMipChain.h"
#include "photon/voxel/PhotonPassBinding.h"
#include "photon/voxel/PhotonVoxelScroll.h"
#include "jahshaka/engine/PhotonStatus.h"
#include "SceneLightList.h"

#include <Compute/OgreComputeTools.h>
#include <OgreCamera.h>
#include <cassert>
#include <OgreLight.h>
#include <OgreLogManager.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>

#include <OgreRoot.h>
#include <OgreSceneNode.h>
#include <OgreTextureGpuManager.h>
#include <Vao/OgreConstBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <OgreVulkanAsyncComputeQueue.h>
#include <OgreVulkanDevice.h>
#include <OgreVulkanRenderSystem.h>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>

namespace jahshaka {
namespace engine {
namespace detail {

const char *const PhotonComponent::kUpdatePass = "photon_update";
const char *const PhotonComponent::kTracePass = "photon_trace";
const char *const PhotonComponent::kResolvePass = "photon_resolve";

void PhotonComponent::registerPasses(AtomPassProvider *provider) {
    registerPhotonRayPasses(provider);
    registerPhotonUpdatePass(provider);
}

void PhotonComponent::volumes(std::vector<Volume> &out) const {
    out.clear();
    for (const VctCascade &c : mVctCascades)
        if (c.built) out.push_back(Volume{ c.lighting, c.voxelizer });
}

PhotonComponent::PhotonComponent(OgreScene &scene, Ogre::Root *const &root,
                                 Ogre::SceneManager *const &sceneMgr, OgreEngine *const &engine,
                                 std::string &error)
    : mScene(scene), mRoot(root), mSceneMgr(sceneMgr), mEngine(engine), mError(error) {}
// Out of line: the cache's and the feeds' types are complete here.
PhotonComponent::~PhotonComponent() = default;

void PhotonComponent::releaseSurfaceCache() {
    releaseUpdatePass();
    mSurfaceCache.reset();
}

// PHOTON_UPDATE'S WORKSPACE (ENGINE-UPGRADE-2B stage 3): the component's, so the pass exists for
// EVERY scene that runs Photon — a plain VCT scene included, which had none while the pass was the
// surface cache's. One PASS_CUSTOM that never draws (AtomPassDef::mDrawsNothing), on a 4x4 target
// of its own. Its place in the manager's list is the one the cache's pair had: right after the
// card capture's workspace (position 0 then 1) when the cards run, so the cards' trace and
// relight follow their capture; at the front otherwise (views append at the back).
void PhotonComponent::syncUpdatePass(bool afterNewCapture) {
    const bool want = mSceneMgr && (mGi.mode != GiMode::Off || mSurfaceCache);
    if (!want) { releaseUpdatePass(); return; }
    if (mUpdateWs && !afterNewCapture) return;
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    if (mUpdateWs) {   // a capture workspace was just put at the front: follow it again
        cm->removeWorkspace(mUpdateWs);
        mUpdateWs = nullptr;
    }
    if (!mUpdateTarget) {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        mUpdateTarget = tm->createTexture(processUniqueName("photonUpdateTarget"), Ogre::GpuPageOutStrategy::Discard,
                                          Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2D);
        mUpdateTarget->setResolution(4u, 4u, 1u);
        mUpdateTarget->setPixelFormat(Ogre::PFG_RGBA8_UNORM);
        mUpdateTarget->setNumMipmaps(1u);
        mUpdateTarget->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    }
    if (!mUpdateCamera) mUpdateCamera = mSceneMgr->createCamera(processUniqueName("photonUpdateCam"), false);
    if (mUpdateWsDef.empty()) {
        mUpdateNodeDef = processUniqueName("JahPhotonUpdateNode");
        mUpdateWsDef = processUniqueName("JahPhotonUpdateWs");
        Ogre::CompositorNodeDef *un = cm->addNodeDefinition(mUpdateNodeDef);
        un->addTextureSourceName("photonUpdateTarget", 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
        Ogre::RenderTargetViewDef *rtv = un->addRenderTextureView("photonUpdateRtv");
        Ogre::RenderTargetViewEntry e;
        e.textureName = "photonUpdateTarget";
        rtv->colourAttachments.push_back(e);
        un->setNumTargetPass(1u);
        Ogre::CompositorTargetDef *t = un->addTargetPass("photonUpdateRtv");
        t->setNumPasses(1u);
        Ogre::CompositorPassDef *p = t->addPass(Ogre::PASS_CUSTOM, Ogre::IdString(kUpdatePass));
        static_cast<AtomPassDef *>(p)->mDrawsNothing = true;
        p->mProfilingId = "Jahshaka photon update";
        Ogre::CompositorWorkspaceDef *uwd = cm->addWorkspaceDefinition(mUpdateWsDef);
        uwd->connectExternal(0, mUpdateNodeDef, 0);
    }
    Ogre::CompositorChannelVec externals(1u, mUpdateTarget);
    mUpdateWs = cm->addWorkspace(mSceneMgr, externals, mUpdateCamera, mUpdateWsDef, true, mSurfaceCache ? 1 : 0);
}

void PhotonComponent::releaseUpdatePass() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return;
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    if (mUpdateWs) { cm->removeWorkspace(mUpdateWs); mUpdateWs = nullptr; }
    if (!mUpdateWsDef.empty() && cm->hasWorkspaceDefinition(mUpdateWsDef)) cm->removeWorkspaceDefinition(mUpdateWsDef);
    if (!mUpdateNodeDef.empty() && cm->hasNodeDefinition(mUpdateNodeDef)) cm->removeNodeDefinition(mUpdateNodeDef);
    mUpdateWsDef.clear();
    mUpdateNodeDef.clear();
    if (mUpdateCamera && mSceneMgr) mSceneMgr->destroyCamera(mUpdateCamera);
    mUpdateCamera = nullptr;
    if (mUpdateTarget) {
        root->getRenderSystem()->getTextureGpuManager()->destroyTexture(mUpdateTarget);
        mUpdateTarget = nullptr;
    }
}

// THE WITNESS AND THE DOOR (photon/PhotonDispatch.h): process-wide, render thread only.
namespace {
unsigned gPhotonRegionDepth = 0u;
bool gPhotonRegionCompute = false;     // the outermost open region's kind
unsigned long long gDoorInCompute = 0ull, gRsInCompute = 0ull, gRsAtComputeOpen = 0ull, gDoorAtComputeOpen = 0ull;
unsigned long long renderSystemDispatches() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    auto *vk = root ? dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem()) : nullptr;
    return vk ? vk->_jahGetNumDispatches() : 0ull;
}
unsigned long long gPhotonInRegion = 0ull, gPhotonOutsideRegion = 0ull;

// THE REGION'S BRACKET CHECK (jahshaka/engine/PhotonStatus.h setPhotonRegionBracketCheck): stage 1's
// acceptance instrument for gi.region_compute_only - the in-order bracket is the row's door, not a
// shipped path (nothing in the product turns it on).
bool gRegionCheck = false;
bool gRegionCheckWasEnabled = false;   // the queue's arm before the check switched it off
bool gRegionCheckOpen = false;         // the bracket the outermost open began
unsigned long long gRegionBrackets = 0ull;
Ogre::VulkanAsyncComputeQueue *regionQueue() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    auto *vk = root ? dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem()) : nullptr;
    Ogre::VulkanDevice *dev = vk ? vk->getVulkanDevice() : nullptr;
    return dev ? dev->getAsyncCompute() : nullptr;
}
}   // namespace
void notePhotonDispatch() {
    if (gPhotonRegionDepth) {
        ++gPhotonInRegion;
        if (gPhotonRegionCompute) ++gDoorInCompute;
        return;
    }
    if (!gPhotonOutsideRegion++)
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka photon: a Photon compute dispatch OUTSIDE the GI region (PhotonComponent::GiRegion) — "
            "the ASYNC bracket would miss it; PhotonDispatchStatus::outsideRegion counts every one",
            Ogre::LML_CRITICAL);
    // THE DOOR ASSERTS (ENGINE-UPGRADE-ASYNC-3 stage 1): a debug build stops at the first one — the
    // Compute region is the compute-legal span (bracketed in the region row only), and a dispatch
    // outside every region escapes its witness.
    assert(false && "a Photon compute dispatch outside the GI region (PhotonComponent::GiRegion)");
}
unsigned long long photonDispatchesInRegion() { return gPhotonInRegion; }
unsigned long long photonDispatchesOutsideRegion() { return gPhotonOutsideRegion; }

void photonDispatch(Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job, Ogre::RenderSystem *rs) {
    Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
    job->analyzeBarriers(rt);
    rs->executeResourceTransition(rt);
    notePhotonDispatch();
    hc->dispatch(job, 0, 0);
}

void photonClearUavFloat(Ogre::ComputeTools *tools, Ogre::TextureGpu *tex, const float value[4]) {
    notePhotonDispatch();
    tools->clearUavFloat(tex, value);
}
void photonClearUavUint(Ogre::ComputeTools *tools, Ogre::TextureGpu *tex, const unsigned value[4]) {
    notePhotonDispatch();
    tools->clearUavUint(tex, value);
}

// The region's open depth (nesting counts once); GiRegion is the only door. A close with no
// open is a broken pairing: logged, never wrapped to 4 billion open regions.
bool PhotonComponent::inComputeRegion() { return gPhotonRegionDepth && gPhotonRegionCompute; }
bool PhotonComponent::computeRegionDispatched() { return gDoorInCompute != gDoorAtComputeOpen; }

void PhotonComponent::beginGiRegion(RegionKind kind) {
    if (gPhotonRegionDepth) {
        // A graphics region inside the compute one would record graphics work in the bracket.
        assert(!(gPhotonRegionCompute && kind == RegionKind::Graphics) &&
               "a Graphics GI region opened inside the Compute region");
        ++gPhotonRegionDepth;
        return;
    }
    ++gPhotonRegionDepth;
    gPhotonRegionCompute = kind == RegionKind::Compute;
    if (gPhotonRegionCompute) {
        gRsAtComputeOpen = renderSystemDispatches();
        gDoorAtComputeOpen = gDoorInCompute;
    }
    if (!gRegionCheck || !gPhotonRegionCompute) return;
    Ogre::VulkanAsyncComputeQueue *q = regionQueue();
    if (!q || q->isOpen()) return;
    if (q->isEnabled()) {   // the check runs the IN-ORDER arm (switched once, restored at its end)
        gRegionCheckWasEnabled = true;
        q->setEnabled(false);
    }
    q->begin(0u, "jah.gi.check");
    gRegionCheckOpen = true;
    ++gRegionBrackets;
}
void PhotonComponent::endGiRegion() {
    if (gPhotonRegionDepth) {
        if (--gPhotonRegionDepth == 0u) {
            if (gPhotonRegionCompute) gRsInCompute += renderSystemDispatches() - gRsAtComputeOpen;
            gPhotonRegionCompute = false;
        }
        if (gPhotonRegionDepth == 0u && gRegionCheckOpen) {
            gRegionCheckOpen = false;
            if (Ogre::VulkanAsyncComputeQueue *q = regionQueue())
                if (q->isOpen()) q->end();
        }
        return;
    }
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka photon: a GI region closed that was never opened", Ogre::LML_CRITICAL);
}

bool PhotonComponent::updatePassOwed() const {
    return mSurfaceCache && mSurfaceCache->photonUpdateOwed();
}

void PhotonComponent::recordUpdatePass() {
    GiRegion region(RegionKind::Graphics);   // the cards' work (this frame's capture): graphics, in order
    if (!mSurfaceCache) return;
    mSurfaceCache->setSunGridParity(mEngine && mEngine->armRegistry().on(ArmId::CardsSunGridParity));
    mSurfaceCache->setNullMove(mEngine && mEngine->armRegistry().on(ArmId::CardsNullMove));
    mSurfaceCache->photonUpdate();
}

/// photon_update's recorder and gate: the pass's scene (bound on its manager) and its component.
void registerPhotonUpdatePass(AtomPassProvider *provider) {
    if (!provider) return;
    provider->setGate(PhotonComponent::kUpdatePass, [](const AtomPassContext &ctx) {
        return ctx.scene && ctx.scene->photon().updatePassOwed();
    });
    provider->setRecorder(PhotonComponent::kUpdatePass, [](AtomPassContext &ctx) {
        if (ctx.scene) ctx.scene->recordPhotonUpdatePass();
    });
}

// ---- THE CHANGED-REGION LIST (PHOTON-7 stage 3, AB S1; law L2) ----------------------------------------------------------
// ONE LIST A FRAME of where the world changed. The producers append at their own points of the frame - the drain (a
// lamp's sphere: OgreScene::listLampChange), the GI step (a scroll's slabs and a placement's box: injectCascade; the
// dirty path's boxes: noteGiCascadeDirty), the cards' plan (the relit instances: updateSurfaceCacheFrame) and the GPU
// scene's commit with the skin pass (the movers: OgreEngine::listRayMovers) - and the list CLOSES once, before the
// frame's submit (OgreEngine::renderOneFrame), into the one buffer the consumers bind. A sun or sky change cannot be
// placed: it is an overflow bit, never a box.
namespace {
/// A box's margin - the sum of its half-extents: the R*-tree's metric the dirty path merges by (noteGiCascadeDirty has
/// why not the volume: this engine's boxes go flat, and every coplanar box has zero volume).
double regionMargin(const Ogre::Aabb &a) {
    return double(std::max(a.mHalfSize.x, 0.0f)) + double(std::max(a.mHalfSize.y, 0.0f)) +
           double(std::max(a.mHalfSize.z, 0.0f));
}
/// No box (a null box's negative half size, or a non-finite one) lists nothing.
bool regionEmpty(const Ogre::Aabb &a) {
    for (int k = 0; k < 3; ++k)
        if (!(a.mHalfSize[size_t(k)] >= 0.0f) || !std::isfinite(a.mCenter[size_t(k)])) return true;
    return false;
}
/// A light's range sphere as a box (the sphere is everything the lamp lights: past its range JahBrdf's window is 0).
Ogre::Aabb sphereBox(const Ogre::Vector3 &c, float r) { return Ogre::Aabb(c, Ogre::Vector3(std::max(r, 0.0f))); }
/// THE LIGHTING RESTART'S TEST (Lumen's UpdateGlobalLightingState, LR §2.9): the peak channel moved by more than 4x or
/// under a quarter between two closes (a light appearing from nothing, or going to nothing, is such a jump). `before`
/// negative = never read: no jump.
bool peakJumped(float before, float now) {
    if (!(before >= 0.0f)) return false;
    if (!(before > 0.0f) && !(now > 0.0f)) return false;
    return now > 4.0f * before || now < 0.25f * before;
}
}   // namespace

void PhotonComponent::appendRegion(GiRegionKind kind, const Ogre::Aabb &box, uint32_t cascadeMask) {
    if (regionEmpty(box)) return;
    if (mRegionsOpen.size() < kGiRegionCap) {
        mRegionsOpen.push_back(RegionEntry{ box, kind, cascadeMask });
        return;
    }
    // AT THE CAP THE LIST MERGES, IT NEVER GIVES UP (the dirty path's lesson, PhotonComponent.h's seventeen crates: an
    // "everything changed" at a cap rebuilt the world for a corner). The box joins the entry OF ITS KIND whose margin
    // grows least - the kind is what a consumer filters by, so a merge never crosses it.
    size_t best = mRegionsOpen.size();
    double bestGrowth = std::numeric_limits<double>::max();
    for (size_t i = 0; i < mRegionsOpen.size(); ++i) {
        if (mRegionsOpen[i].kind != kind) continue;
        Ogre::Aabb m = mRegionsOpen[i].box;
        m.merge(box);
        const double g = regionMargin(m) - regionMargin(mRegionsOpen[i].box);
        if (g < bestGrowth) { bestGrowth = g; best = i; }
    }
    if (best < mRegionsOpen.size()) {
        mRegionsOpen[best].box.merge(box);
        mRegionsOpen[best].mask |= cascadeMask;
        ++mRegionMergesOpen;
        return;
    }
    // ...and a kind the full list does not hold yet takes a slot two entries of one other kind free by merging: the
    // pair whose union grows the larger of them least (128 entries over seven kinds: some kind holds at least 19).
    size_t bi = mRegionsOpen.size(), bj = bi;
    double bg = std::numeric_limits<double>::max();
    for (size_t i = 0; i < mRegionsOpen.size(); ++i)
        for (size_t j = i + 1; j < mRegionsOpen.size(); ++j) {
            if (mRegionsOpen[i].kind != mRegionsOpen[j].kind) continue;
            Ogre::Aabb m = mRegionsOpen[i].box;
            m.merge(mRegionsOpen[j].box);
            const double g = regionMargin(m) - std::max(regionMargin(mRegionsOpen[i].box), regionMargin(mRegionsOpen[j].box));
            if (g < bg) { bg = g; bi = i; bj = j; }
        }
    if (bi < mRegionsOpen.size()) {
        mRegionsOpen[bi].box.merge(mRegionsOpen[bj].box);
        mRegionsOpen[bi].mask |= mRegionsOpen[bj].mask;
        mRegionsOpen.erase(mRegionsOpen.begin() + std::ptrdiff_t(bj));
        ++mRegionMergesOpen;
    }
    mRegionsOpen.push_back(RegionEntry{ box, kind, cascadeMask });
}

void PhotonComponent::listMover(const Ogre::Aabb &swept, bool casts) {
    if (regionEmpty(swept)) return;
    appendRegion(GiRegionKind::Mover, swept);
    if (!casts || !mSceneMgr) return;
    // ITS SHADOWS: the sun's along -toSun from the box (the header carries the direction - the box, never the
    // extrusion's AABB), and each lamp's inside the lamp's range sphere (conservative: a lamp's shadow cannot leave
    // it), the lamps the scene's light list says reach the box by its own rule, once a frame per lamp.
    const SceneLightList &lights = sceneLightListOf(mSceneMgr);
    SceneLightList::Consumer box;
    box.boxMin = swept.getMinimum();
    box.boxMax = swept.getMaximum();
    bool sun = false;
    for (Ogre::Light *l : lights.byRank(mSceneMgr)) {
        if (l->getType() == Ogre::Light::LT_DIRECTIONAL) { sun = true; continue; }
        if (!l->getParentNode()) continue;
        if (std::find(mRegionLampShadows.begin(), mRegionLampShadows.end(), l) != mRegionLampShadows.end()) continue;
        if (!SceneLightList::reaches(l, box)) continue;
        mRegionLampShadows.push_back(l);
        appendRegion(GiRegionKind::LampShadow,
                     sphereBox(l->getParentNode()->_getDerivedPosition(), float(l->getAttenuationRange())));
    }
    if (sun) appendRegion(GiRegionKind::MoverShadow, swept);
}

void PhotonComponent::listLamp(bool hadSphere, const Ogre::Vector3 &oldCentre, float oldRange, bool hasSphere,
                               const Ogre::Vector3 &centre, float range) {
    if (!hadSphere && !hasSphere) return;
    Ogre::Vector3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
    const auto join = [&](const Ogre::Vector3 &c, float r) {
        const Ogre::Aabb b = sphereBox(c, r);
        lo.makeFloor(b.getMinimum());
        hi.makeCeil(b.getMaximum());
    };
    if (hadSphere) join(oldCentre, oldRange);
    if (hasSphere) join(centre, range);
    appendRegion(GiRegionKind::Lamp, Ogre::Aabb::newFromExtents(lo, hi));
}

void PhotonComponent::closeRegions() {
    const unsigned long long frame = giWriterFrame();
    // THE CASCADES EACH REGION REACHES, against the boxes as they stand at the close (a scroll's commit earlier in the
    // frame moved one): a box test, as the dirty path's marks; a Slab names its own.
    for (RegionEntry &e : mRegionsOpen) {
        if (e.mask) continue;
        for (size_t i = 0; i < mVctCascades.size() && i < 32u; ++i)
            if (Ogre::Aabb(mVctCascades[i].centre, Ogre::Vector3(mVctCascades[i].halfSize)).intersects(e.box))
                e.mask |= 1u << i;
    }
    // THE SUN: the first directional light of the scene's one order (SceneLightList: directional first) - the
    // direction toward it, and its peak channel (colour x power) for the restart; the SKY's peak channel, the
    // ambient's constant band.
    bool sun = false;
    Ogre::Vector3 toSun = Ogre::Vector3::UNIT_Y;
    float sunPeak = 0.0f;
    if (mSceneMgr) {
        for (Ogre::Light *l : sceneLightListOf(mSceneMgr).byRank(mSceneMgr)) {
            if (l->getType() != Ogre::Light::LT_DIRECTIONAL) continue;
            sun = true;
            toSun = -l->getDerivedDirection().normalisedCopy();
            const Ogre::ColourValue d = l->getDiffuseColour();
            sunPeak = std::max(d.r, std::max(d.g, d.b)) * float(l->getPowerScale());
            break;
        }
    }
    const float skyPeak = mScene.ambientPeakForGi();
    const bool sunJump = peakJumped(mRegionSunPeak, sunPeak), skyJump = peakJumped(mRegionSkyPeak, skyPeak);
    mRegionSunPeak = sunPeak;
    mRegionSkyPeak = skyPeak;
    // A JUMP IS A CHANGE: its overflow stands beside the restart whatever path the change came by.
    if (sunJump) mRegionOverflowOpen |= kGiRegionOverflowSun;
    if (skyJump) mRegionOverflowOpen |= kGiRegionOverflowSky;

    GiChangedRegions &c = mRegionsClosed;
    c.available = true;
    c.frame = frame;
    c.overflow = mRegionOverflowOpen;
    c.restart = sunJump || skyJump;
    c.sun = sun;
    c.toSun = Vec3(float(toSun.x), float(toSun.y), float(toSun.z));
    c.merges = mRegionMergesOpen;
    c.regions.clear();
    c.regions.reserve(mRegionsOpen.size());
    for (const RegionEntry &e : mRegionsOpen) {
        const Ogre::Vector3 lo = e.box.getMinimum(), hi = e.box.getMaximum();
        GiChangedRegion r;
        r.lo = Vec3(float(lo.x), float(lo.y), float(lo.z));
        r.hi = Vec3(float(hi.x), float(hi.y), float(hi.z));
        r.kind = e.kind;
        r.cascadeMask = e.mask;
        c.regions.push_back(r);
    }
    ++mRegionCloses;
    if (!c.regions.empty() || c.overflow || c.restart) ++mRegionClosesListing;
    // ...AND THE NEXT FRAME'S LIST STARTS EMPTY.
    mRegionsOpen.clear();
    mRegionOverflowOpen = 0u;
    mRegionMergesOpen = 0u;
    mRegionLampShadows.clear();

    // THE ONE BUFFER (regionBuffer's layout), the used prefix written at every close.
    JAH_TRY {
        Ogre::VaoManager *vao = mRoot && mRoot->getRenderSystem() ? mRoot->getRenderSystem()->getVaoManager() : nullptr;
        if (vao) {
            const size_t floats = 8u + 8u * size_t(kGiRegionCap);
            if (!mRegionBuffer)
                mRegionBuffer = vao->createConstBuffer(floats * sizeof(float), Ogre::BT_DEFAULT, nullptr, false);
            mRegionCpu.assign(8u + 8u * c.regions.size(), 0.0f);
            const auto putUint = [this](size_t at, uint32_t v) { std::memcpy(&mRegionCpu[at], &v, sizeof v); };
            putUint(0, uint32_t(c.regions.size()));
            putUint(1, c.overflow);
            putUint(2, c.restart ? 1u : 0u);
            putUint(3, uint32_t(frame & 0xFFFFFFFFull));
            mRegionCpu[4] = c.toSun.x; mRegionCpu[5] = c.toSun.y; mRegionCpu[6] = c.toSun.z;
            mRegionCpu[7] = sun ? 1.0f : 0.0f;
            for (size_t i = 0; i < c.regions.size(); ++i) {
                const GiChangedRegion &r = c.regions[i];
                float *f = &mRegionCpu[8u + 8u * i];
                f[0] = r.lo.x; f[1] = r.lo.y; f[2] = r.lo.z;
                f[4] = r.hi.x; f[5] = r.hi.y; f[6] = r.hi.z;
                putUint(8u + 8u * i + 3u, uint32_t(r.kind));
                putUint(8u + 8u * i + 7u, r.cascadeMask);
            }
            mRegionBuffer->upload(mRegionCpu.data(), 0u, mRegionCpu.size() * sizeof(float));
        }
    } JAH_CATCH(mError, );

}

void PhotonComponent::releaseRegionBuffer() {
    if (!mRegionBuffer) return;
    Ogre::VaoManager *vao = mRoot && mRoot->getRenderSystem() ? mRoot->getRenderSystem()->getVaoManager() : nullptr;
    if (vao) vao->destroyConstBuffer(mRegionBuffer);
    mRegionBuffer = nullptr;
}

}  // namespace detail

PhotonDispatchStatus photonDispatchStatus() {
    PhotonDispatchStatus s;
    s.inRegion = detail::photonDispatchesInRegion();
    s.outsideRegion = detail::photonDispatchesOutsideRegion();
    s.mipDispatches = Ogre::PhotonMipChain::numDispatches();
    s.mipJobs = Ogre::PhotonMipChain::numJobs();
    s.regionBrackets = detail::gRegionBrackets;
    s.doorInCompute = detail::gDoorInCompute;
    s.renderSystemInCompute = detail::gRsInCompute;
    return s;
}

PhotonBindingStatus photonBindingStatus() {
    PhotonBindingStatus s;
    s.readerPrepares = detail::PhotonPassBinding::readerPrepares();
    s.nonReaderPrepares = detail::PhotonPassBinding::nonReaderPrepares();
    return s;
}

void setPhotonRegionBracketCheck(bool on) {
    using namespace detail;
    if (on == gRegionCheck) return;
    // The check runs the IN-ORDER arm (the graphics queue keeps recording; the bracket's refusals
    // are the same as on the async arm): the first region it opens switches the queue's arm off
    // (the device may not exist yet when the check is turned on); turning it off restores it.
    if (!on) {
        Ogre::VulkanAsyncComputeQueue *q = regionQueue();
        if (q && gRegionCheckWasEnabled && !q->isOpen()) q->setEnabled(true);
        gRegionCheckWasEnabled = false;
    }
    gRegionCheck = on;
}

PhotonFieldStatus photonFieldStatus(const Scene &scene) {
    PhotonFieldStatus st;
    const auto *s = dynamic_cast<const detail::OgreScene *>(&scene);
    const Ogre::PhotonIrradianceField *f = s ? s->photon().irradianceField() : nullptr;
    if (!f) return st;
    st.field = true;
    st.probes = f->getTotalNumProbes();
    st.integrations = f->getIntegrations();
    st.probesStopped = f->getProbesStopped();
    st.stopMinSamples = f->getStopMinSamples();
    st.stopPrecision = f->getStopPrecision();
    return st;
}

}  // namespace engine
}  // namespace jahshaka
