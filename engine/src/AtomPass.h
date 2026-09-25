// THE ENGINE'S CompositorPassProvider — ONE per CompositorManager2 (Ogre holds exactly
// one), multiplexed on the pass's `customId` (stage 0's P-C: `atom_id atom_marker`
// in definition order, every frame, both devices). Ported from the spike's AtomPass
// (spikes/atom-stage0/src/AtomPass.{h,cpp}).
//
// WHAT A CUSTOM PASS OF OURS OWES OGRE, and why it lives here once rather than in
// every recorder: a recorder writes raw Vulkan into Ogre's command buffer, so
//   1. Ogre's render-pass encoder is CLOSED first (`endRenderPassDescriptor`,
//      CompositorPassCompute::execute's own discipline), and
//   2. Ogre's caches are FORGOTTEN after — `_setPipelineStateObject(nullptr)`
//      re-arms the PSO bind (whose `mPso != pso` early-out would otherwise skip the
//      next real bind) and re-dirties the global descriptor table, and
//      `RenderQueue::clearState()` drops the last VAO / index buffer / texture sets
//      the render queue keeps across draws. Both public; together they are the whole
//      restoration (stage 0's P-B: 0 differing bytes over 120 frames with our
//      pipeline bound in the same frame, RTX and lavapipe — engine.atom_negative_control
//      keeps it that way).
//
// THE REGISTRY. A customId is served by a RECORDER registered here; the product's
// is the visibility buffer's id pass (`atom_id`, OgreAtomIdPass.cpp). Registering is
// how a test or a later lane gets a custom pass without a second provider.
//
// A PASS WITH A TARGET. When its target pass names a texture or an RTV, the pass
// owns Ogre's render pass descriptor for it (CompositorPass::initialize), so the
// attachments get Ogre's own barriers and Ogre's own VkRenderPass: a recorder that
// draws calls `beginRenderPass()` after whatever compute it records, draws inside
// the pass Ogre opened, and leaves it open (the next pass closes it).
#ifndef JAHSHAKA_ENGINE_ATOMPASS_H
#define JAHSHAKA_ENGINE_ATOMPASS_H

#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/OgreCompositorPassProvider.h>

#include <functional>
#include <map>
#include <string>

namespace Ogre {
class CompositorManager2;
class RenderSystem;
class SceneManager;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

/// What a recorder is handed while its pass executes. Ogre's render pass is already
/// closed; the recorder records whatever it records; the provider restores Ogre's
/// state after it returns.
struct AtomPassContext {
    Ogre::IdString customId;
    Ogre::CompositorPass *pass = nullptr;
    Ogre::RenderSystem *renderSystem = nullptr;
    Ogre::SceneManager *sceneManager = nullptr;
    const Ogre::Camera *lodCamera = nullptr;
    /// True when the recorder bound state of its own (a pipeline, descriptor sets, an
    /// index buffer): the provider then forgets Ogre's caches. A recorder that only
    /// used Ogre's own calls leaves it false and the caches are kept.
    bool boundRawState = false;
};
using AtomPassRecorder = std::function<void(AtomPassContext &)>;

class AtomPassDef final : public Ogre::CompositorPassDef {
public:
    AtomPassDef(Ogre::CompositorTargetDef *parentTargetDef, Ogre::IdString customId)
        : Ogre::CompositorPassDef(Ogre::PASS_CUSTOM, parentTargetDef), mCustomId(customId) {}
    Ogre::IdString mCustomId;
};

class AtomPass final : public Ogre::CompositorPass {
public:
    AtomPass(const AtomPassDef *definition, Ogre::CompositorNode *parentNode,
             Ogre::SceneManager *sceneManager, const Ogre::RenderTargetViewDef *rtvDef);
    void execute(const Ogre::Camera *lodCamera) override;
    /// The pass's attachments to their render-target layouts (Ogre's solver) and
    /// Ogre's render pass for them, OPEN (its load actions run now). For a
    /// recorder that draws; false when the pass has no target.
    bool beginRenderPass();
    const Ogre::RenderPassDescriptor *renderPassDesc() const { return mRenderPassDesc; }

private:
    const AtomPassDef *mDef;
    Ogre::SceneManager *mSceneManager;
};

class AtomPassProvider final : public Ogre::CompositorPassProvider {
public:
    /// Installs the provider on `cm` (once per process; the manager holds one).
    static AtomPassProvider *install(Ogre::CompositorManager2 *cm);
    /// The installed provider, or null before `install`.
    static AtomPassProvider *instance();
    /// Drops the installed provider (Root's teardown, after every workspace died).
    static void uninstall(Ogre::CompositorManager2 *cm);

    /// A customId's recorder. Registering an id already registered replaces it;
    /// an empty recorder removes it. A pass whose id has no recorder executes as a
    /// no-op (closed and reopened render pass, nothing recorded, no cache reset).
    void setRecorder(const std::string &customId, AtomPassRecorder recorder);
    const AtomPassRecorder *recorder(Ogre::IdString customId) const;

    Ogre::CompositorPassDef *addPassDef(Ogre::CompositorPassType passType, Ogre::IdString customId,
                                        Ogre::CompositorTargetDef *parentTargetDef,
                                        Ogre::CompositorNodeDef *parentNodeDef) override;
    Ogre::CompositorPass *addPass(const Ogre::CompositorPassDef *definition, Ogre::Camera *defaultCamera,
                                  Ogre::CompositorNode *parentNode,
                                  const Ogre::RenderTargetViewDef *rtvDef,
                                  Ogre::SceneManager *sceneManager) override;

private:
    std::map<Ogre::IdString, AtomPassRecorder> mRecorders;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_ATOMPASS_H
