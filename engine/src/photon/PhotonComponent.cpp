// PHOTON, THE COMPONENT (photon/PhotonComponent.h has the design).
#include "photon/PhotonComponent.h"

#include "EnginePrivate.h"
#include "AtomPass.h"
#include "SurfaceCache.h"
#include "photon/PhotonDispatch.h"
#include "jahshaka/engine/PhotonStatus.h"

#include <OgreCamera.h>
#include <OgreLogManager.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>

#include <OgreRoot.h>
#include <OgreTextureGpuManager.h>
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
unsigned long long gPhotonInRegion = 0ull, gPhotonOutsideRegion = 0ull;
}   // namespace
void notePhotonDispatch() {
    if (gPhotonRegionDepth) { ++gPhotonInRegion; return; }
    if (!gPhotonOutsideRegion++)
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka photon: a Photon compute dispatch OUTSIDE the GI region (PhotonComponent::GiRegion) — "
            "the ASYNC bracket would miss it; PhotonDispatchStatus::outsideRegion counts every one",
            Ogre::LML_CRITICAL);
}
void photonRegionBegin() { ++gPhotonRegionDepth; }
void photonRegionEnd() { --gPhotonRegionDepth; }
unsigned long long photonDispatchesInRegion() { return gPhotonInRegion; }
unsigned long long photonDispatchesOutsideRegion() { return gPhotonOutsideRegion; }

void photonDispatch(Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job, Ogre::RenderSystem *rs) {
    Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
    job->analyzeBarriers(rt);
    rs->executeResourceTransition(rt);
    notePhotonDispatch();
    hc->dispatch(job, 0, 0);
}

void PhotonComponent::beginGiRegion() { photonRegionBegin(); }
void PhotonComponent::endGiRegion() { photonRegionEnd(); }

bool PhotonComponent::updatePassOwed() const {
    return mSurfaceCache && mSurfaceCache->photonUpdateOwed();
}

void PhotonComponent::recordUpdatePass() {
    GiRegion region;   // the cards' work: its own span of the region
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
    return s;
}

}  // namespace engine
}  // namespace jahshaka
