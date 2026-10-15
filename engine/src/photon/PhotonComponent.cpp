// PHOTON, THE COMPONENT (photon/PhotonComponent.h has the design).
#include "photon/PhotonComponent.h"

#include "EnginePrivate.h"
#include "AtomPass.h"
#include "SurfaceCache.h"
#include "photon/PhotonDispatch.h"
#include "photon/voxel/PhotonIrradianceField.h"
#include "photon/voxel/PhotonMipChain.h"
#include "jahshaka/engine/PhotonStatus.h"

#include <Compute/OgreComputeTools.h>
#include <OgreCamera.h>
#include <cassert>
#include <OgreLogManager.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>

#include <OgreRoot.h>
#include <OgreTextureGpuManager.h>
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
unsigned long long gDoorInCompute = 0ull, gRsInCompute = 0ull, gRsAtComputeOpen = 0ull;
unsigned long long renderSystemDispatches() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    auto *vk = root ? dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem()) : nullptr;
    return vk ? vk->_jahGetNumDispatches() : 0ull;
}
unsigned long long gPhotonInRegion = 0ull, gPhotonOutsideRegion = 0ull;

// THE REGION'S BRACKET CHECK (jahshaka/engine/PhotonStatus.h setPhotonRegionBracketCheck).
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
    // async bracket wraps the region, so a dispatch outside it would record on the wrong queue.
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
    if (gPhotonRegionCompute) gRsAtComputeOpen = renderSystemDispatches();
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
    if (mSurfaceCache) mSurfaceCache->photonUpdate();
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

}  // namespace detail

PhotonDispatchStatus photonDispatchStatus() {
    PhotonDispatchStatus s;
    s.inRegion = detail::photonDispatchesInRegion();
    s.outsideRegion = detail::photonDispatchesOutsideRegion();
    s.mipDispatches = Ogre::PhotonMipChain::numDispatches();
    s.regionBrackets = detail::gRegionBrackets;
    s.doorInCompute = detail::gDoorInCompute;
    s.renderSystemInCompute = detail::gRsInCompute;
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
