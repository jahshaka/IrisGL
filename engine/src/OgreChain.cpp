// The compositor chain: every view's workspace graph, ASSEMBLED IN C++.
//
// POST_CHAIN_SPEC.md §3 — why this exists rather than a folder of .compositor
// scripts: the chain has six independent switches (MSAA, HDR, SSAO, SMAA, SSR,
// refraction), i.e. 64 shapes. Ogre's own samples ship one hand-written script
// per shape; copying that pattern would mean owning 64 divergent copies of
// upstream sample scripts forever — the patches-only law by the back door.
// Everything needed to build the graph programmatically is public API
// (addNodeDefinition / addTargetPass / addPass / addWorkspaceDefinition /
// connectExternal) and it is already the discipline the shadow node uses
// (OgreEngine::createShadowNode builds it with ShadowNodeHelper, not a script).
//
// PHASE 1 SHAPE (this file's whole content today): the passthrough chain — one
// node, one target, two scene passes split by render queue. It must render
// BIT-IDENTICALLY to CompositorManager2::createBasicWorkspaceDef, which is what
// every view used before. The later phases (HDR, SSAO, SMAA, SSR, refraction)
// add node definitions and textures here and nowhere else.
//
// RENDER-QUEUE POLICY (§6, and the reason the split exists from day one):
//   0            sky rectangle (OgreSky.cpp)
//   10           normal items (Ogre's default)
//   15           PFX2 billboards
//   [.. 199]     everything else opaque      <- the OPAQUE pass
//   200          reserved for refractive items (phase 7)
//   kOverlayRenderQueue (210)  on-top overlays: gizmos, wires, outlines
//   [210 .. 254] the OVERLAY pass
// Keeping the overlays in their own pass is what will later keep a bright unlit
// gizmo out of the SSAO normals G-buffer, out of the HDR luminance average and
// out of SMAA edge detection. Ogre's RenderQueue constructor fixes the modes:
// [0,100) and [200,225) are v2 FAST, so our v2 items can only live there.
#include "EnginePrivate.h"

#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>

namespace jahshaka { namespace engine { namespace detail {
namespace chain {

namespace {

/// The one input channel every chain has: the view's render target (window
/// texture or RTT), connected with connectExternal(0, ...).
constexpr const char *kTargetChannel = "JahTarget";

}   // namespace

std::string sceneNodeDefName(const std::string &workspaceDef) {
    return workspaceDef + "/Scene";
}

void build(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
           const ChainDesc &desc, std::vector<std::string> &nodeDefsOut) {
    // --- The scene node -----------------------------------------------------
    const std::string sceneNode = sceneNodeDefName(workspaceDef);
    Ogre::CompositorNodeDef *nodeDef = cm->addNodeDefinition(sceneNode);
    nodeDefsOut.push_back(sceneNode);

    nodeDef->addTextureSourceName(kTargetChannel, 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);

    nodeDef->setNumTargetPass(1);
    Ogre::CompositorTargetDef *targetDef = nodeDef->addTargetPass(kTargetChannel);
    targetDef->setNumPasses(2);

    // Pass 1 — OPAQUE + sky + particles: render queues [0, kRefractiveRenderQueue).
    {
        Ogre::CompositorPassSceneDef *pass =
            static_cast<Ogre::CompositorPassSceneDef *>(targetDef->addPass(Ogre::PASS_SCENE));
        pass->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName)
                                         : Ogre::IdString();
        pass->setAllClearColours(toOgre(desc.background));
        pass->setAllLoadActions(Ogre::LoadAction::Clear);
        // NOT the compositor default (StoreOrResolve): on an MSAA target that
        // resolves and DISCARDS the multisample contents, and the overlay pass
        // below still has to render into them. Plain Store keeps the samples;
        // the LAST pass on the target does the resolve. On a non-MSAA target
        // Store and StoreOrResolve are the same thing, which is what makes the
        // 1x path bit-identical to createBasicWorkspaceDef.
        pass->mStoreActionColour[0] = Ogre::StoreAction::Store;
        // The overlay pass depth-tests against what the opaque pass wrote, so
        // depth must survive the pass boundary (createBasicWorkspaceDef could
        // throw it away — it had only one pass).
        pass->mStoreActionDepth   = Ogre::StoreAction::Store;
        pass->mStoreActionStencil = Ogre::StoreAction::DontCare;
        pass->mFirstRQ = 0u;
        pass->mLastRQ  = kRefractiveRenderQueue;   // exclusive
        pass->mProfilingId = "Jahshaka opaque";
    }

    // Pass 2 — ON-TOP OVERLAYS: gizmos, selection outlines, wire helpers.
    {
        Ogre::CompositorPassSceneDef *pass =
            static_cast<Ogre::CompositorPassSceneDef *>(targetDef->addPass(Ogre::PASS_SCENE));
        // No shadow node: overlays are unlit and never receive shadows. Leaving
        // it empty also means this pass never triggers a shadow-node update.
        // Load actions keep the compositor defaults (Load for colour, depth and
        // stencil) — this pass continues what the opaque pass left behind.
        // Colour store keeps the default (StoreOrResolve) too: this is the last
        // pass on the target, so it is where an MSAA resolve belongs.
        pass->mStoreActionDepth   = Ogre::StoreAction::DontCare;
        pass->mStoreActionStencil = Ogre::StoreAction::DontCare;
        pass->mFirstRQ = kOverlayRenderQueue;
        pass->mLastRQ  = 255u;
        pass->mProfilingId = "Jahshaka overlays";
    }

    // --- The workspace ------------------------------------------------------
    Ogre::CompositorWorkspaceDef *workDef = cm->addWorkspaceDefinition(workspaceDef);
    workDef->connectExternal(0, nodeDef->getName(), 0);
}

void destroy(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
             std::vector<std::string> &nodeDefs) {
    if (cm->hasWorkspaceDefinition(workspaceDef)) cm->removeWorkspaceDefinition(workspaceDef);
    // Reverse creation order: a node definition is only referenced by the
    // workspace definition, which is already gone, but the ordering keeps the
    // teardown reading like the rest of the backend.
    for (auto it = nodeDefs.rbegin(); it != nodeDefs.rend(); ++it)
        if (cm->hasNodeDefinition(*it)) cm->removeNodeDefinition(*it);
    nodeDefs.clear();
}

}   // namespace chain
}}}  // namespace jahshaka::engine::detail
