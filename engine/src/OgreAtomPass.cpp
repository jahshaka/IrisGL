// See AtomPass.h.
#include "AtomPass.h"

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNode.h>
#include <OgreRenderQueue.h>
#include <OgreRenderSystem.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreSceneManager.h>

#include <limits>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
/// A process-lifetime object, not a heap one: CompositorManager2 does not own its
/// provider, and a static outlives every workspace that could still reference it.
AtomPassProvider *gProvider = nullptr;
AtomPassProvider &providerObject() {
    static AtomPassProvider sProvider;
    return sProvider;
}
}  // namespace

AtomPass::AtomPass(const AtomPassDef *definition, Ogre::CompositorNode *parentNode,
                   Ogre::SceneManager *sceneManager, const Ogre::RenderTargetViewDef *rtvDef)
    : Ogre::CompositorPass(definition, parentNode), mDef(definition), mSceneManager(sceneManager) {
    // A target pass that names one gives the pass Ogre's render pass descriptor;
    // a target-less one (the negative control's) keeps none.
    if (rtvDef) initialize(rtvDef, true);
}

bool AtomPass::beginRenderPass() {
    if (!mRenderPassDesc) return false;
    Ogre::RenderSystem *rs = mParentNode->getRenderSystem();
    // Ogre's own barrier analysis of the pass's RTV (CompositorPass::
    // analyzeBarriers: every colour attachment and the depth to their
    // render-target layouts), executed here — after the recorder's compute,
    // which resolved its own buffers through the same solver.
    analyzeBarriers();
    executeResourceTransitions();
    setRenderPassDescToCurrent();
    rs->executeRenderPassDescriptorDelayedActions();
    return true;
}

void AtomPass::execute(const Ogre::Camera *lodCamera) {
    // CompositorPass's own bookkeeping for a pass with a pass count.
    if (mNumPassesLeft != std::numeric_limits<Ogre::uint32>::max()) {
        if (!mNumPassesLeft) return;
        --mNumPassesLeft;
    }
    // THE PASS IS TIMED like every other (the frame monitor's per-pass GPU ms read
    // the pass's profiling id).
    profilingBegin();
    notifyPassEarlyPreExecuteListeners();
    // CompositorPassCompute::execute's discipline: close Ogre's render-pass encoder
    // before recording anything of ours.
    Ogre::RenderSystem *rs = mParentNode->getRenderSystem();
    rs->endRenderPassDescriptor();
    notifyPassPreExecuteListeners();

    const AtomPassRecorder *rec = gProvider ? gProvider->recorder(mDef->mCustomId) : nullptr;
    if (rec && *rec) {
        AtomPassContext ctx;
        ctx.customId = mDef->mCustomId;
        ctx.pass = this;
        ctx.renderSystem = rs;
        ctx.sceneManager = mSceneManager;
        ctx.lodCamera = lodCamera;
        (*rec)(ctx);
        if (ctx.boundRawState) {
            // THE WHOLE RESTORATION (stage 0's P-B), both calls public.
            rs->_setPipelineStateObject(nullptr);
            if (mSceneManager) mSceneManager->getRenderQueue()->clearState();
        }
    }
    notifyPassPosExecuteListeners();
    profilingEnd();
}

AtomPassProvider *AtomPassProvider::install(Ogre::CompositorManager2 *cm) {
    if (!cm) return nullptr;
    gProvider = &providerObject();
    cm->setCompositorPassProvider(gProvider);
    return gProvider;
}

AtomPassProvider *AtomPassProvider::instance() { return gProvider; }

void AtomPassProvider::uninstall(Ogre::CompositorManager2 *cm) {
    if (cm && gProvider && cm->getCompositorPassProvider() == gProvider)
        cm->setCompositorPassProvider(nullptr);
    if (gProvider) gProvider->mRecorders.clear();
    gProvider = nullptr;
}

void AtomPassProvider::setRecorder(const std::string &customId, AtomPassRecorder recorder) {
    const Ogre::IdString id(customId);
    if (recorder) mRecorders[id] = std::move(recorder);
    else mRecorders.erase(id);
}

const AtomPassRecorder *AtomPassProvider::recorder(Ogre::IdString customId) const {
    auto it = mRecorders.find(customId);
    return it == mRecorders.end() ? nullptr : &it->second;
}

Ogre::CompositorPassDef *AtomPassProvider::addPassDef(Ogre::CompositorPassType,
                                                      Ogre::IdString customId,
                                                      Ogre::CompositorTargetDef *parentTargetDef,
                                                      Ogre::CompositorNodeDef *) {
    // THE MASTER PROVIDER MUST RETURN A VALID DEFINITION (OgreCompositorPassProvider.h):
    // every customId is ours — one registered later, or at execution time, is honoured.
    return OGRE_NEW AtomPassDef(parentTargetDef, customId);
}

Ogre::CompositorPass *AtomPassProvider::addPass(const Ogre::CompositorPassDef *definition,
                                                Ogre::Camera *, Ogre::CompositorNode *parentNode,
                                                const Ogre::RenderTargetViewDef *rtvDef,
                                                Ogre::SceneManager *sceneManager) {
    const auto *def = dynamic_cast<const AtomPassDef *>(definition);
    if (!def) return nullptr;
    return OGRE_NEW AtomPass(def, parentNode, sceneManager, rtvDef);
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
