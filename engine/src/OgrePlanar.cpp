// Planar reflections: mirrors and glossy floors (PLANAR_REFLECTIONS_SPEC.md).
//
// WHAT OGRE GIVES US, so nobody re-reads the component: Ogre::PlanarReflections
// owns N reflection cameras, N render targets and N private workspaces ("slots",
// the BUDGET). An ACTOR is a world-space rectangle with a normal. Every frame,
// update() culls the actors against the main camera, keeps the `budget` nearest,
// mirrors the main camera about each of their planes and renders the whole scene
// into that slot's target. HlmsPbs then matches every REGISTERED renderable to
// the best-aligned active actor within 20 degrees and maxDistance, and the pixel
// shader samples the slot at roughness * numMips.
//
// The three things that are ours, not Ogre's:
//
//  1. THE VISIBILITY MASK IS INVERTED relative to Ogre's sample. See kNoReflectBit
//     in EnginePrivate.h — the sample's mask would render an empty reflection here.
//  2. THE WORKSPACE IS BUILT IN C++, like the main chain and the shadow node, not
//     copied from Samples/.../PlanarReflections.compositor (whose hard-coded
//     2048x7168 shadow atlas would fight Engine::setShadowResolution).
//  3. THE ACTOR PLANE IS DERIVED, not authored. A reflector is an ordinary flat
//     mesh the user ticked; its own bounds give the centre, size and normal. No
//     MirrorNode type, no manual plane fields.
//
// LIFECYCLE, the part that bites (the VCT lesson again):
//   * ~PlanarReflections does NOT unbind itself from HlmsPbs. Always
//     pbs->setPlanarReflections(nullptr) first or the next pass reads freed memory.
//   * It destroys its cameras through the SceneManager, so it must die BEFORE the
//     SceneManager does.
//   * removeRenderable() must run BEFORE any tracked Item is destroyed — the
//     component's own header says so, and it keeps a raw Renderable*.
//   * setMaxActiveActors can only GROW a slot set and bakes width/height/mips into
//     the slots it creates. Any parameter change therefore rebuilds the whole arm.
#include "EnginePrivate.h"

#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassMipmap/OgreCompositorPassMipmapDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>

namespace jahshaka { namespace engine { namespace detail {
namespace planar {

namespace {

constexpr const char *kTargetChannel = "JahReflectTarget";

/// The one HlmsPbs binding for the process. HlmsPbs::mPlanarReflections is a
/// single pointer shared by every scene in the process (the sVctBindingOwner
/// shape in OgreGi.cpp, and the same accepted v1 compromise): the last scene to
/// arm owns it, and a scene that is not the owner must not clear it on teardown
/// or it would silently switch reflections off for the scene that is.
const OgreScene *sBindingOwner = nullptr;

Ogre::HlmsPbs *pbsOf(Ogre::Root *root) {
    if (!root || !root->getHlmsManager()) return nullptr;
    return dynamic_cast<Ogre::HlmsPbs *>(root->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
}

}   // namespace

// ---------------------------------------------------------------------------
void WorkspaceListener::workspacePreUpdate(Ogre::CompositorWorkspace *) {
    if (mReflections) mReflections->beginFrame();
}

void WorkspaceListener::passEarlyPreExecute(Ogre::CompositorPass *pass) {
    if (!mReflections || !mCamera) return;
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    // A shadow-caster pass renders from a light, not from a camera.
    if (def->mShadowNodeRecalculation == Ogre::SHADOW_NODE_CASTER_PASS) return;
    auto *scenePass = static_cast<Ogre::CompositorPassScene *>(pass);
    // THE discriminator. Ogre's sample tags its main pass with the magic
    // identifier 25001; matching the view's own camera pointer is stronger (it
    // cannot be copied wrong into a new chain phase) and, crucially, it is what
    // stops this callback from recursing: update() runs the reflection
    // workspaces synchronously, and those draw from the reflection cameras.
    if (scenePass->getCamera() != mCamera) return;
    // The aspect ratio must be the one the pass is actually rendering at. Our
    // view cameras are setAutoAspectRatio(true), so the camera's own cached
    // value is stale until Ogre updates it — read the viewport's.
    const Ogre::Real aspect = mCamera->getAutoAspectRatio()
                                  ? pass->getViewportAspectRatio(0u)
                                  : mCamera->getAspectRatio();
    mReflections->update(mCamera, aspect);
}

// ---------------------------------------------------------------------------
void buildWorkspace(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                    const PlanarReflectionParams &p, const std::string &shadowNodeName,
                    std::vector<std::string> &nodeDefsOut) {
    const std::string nodeName = workspaceDef + "/Reflect";
    Ogre::CompositorNodeDef *nodeDef = cm->addNodeDefinition(nodeName);
    nodeDefsOut.push_back(nodeName);

    nodeDef->addTextureSourceName(kTargetChannel, 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    nodeDef->setNumTargetPass(1);
    Ogre::CompositorTargetDef *targetDef = nodeDef->addTargetPass(kTargetChannel);
    targetDef->setNumPasses(p.mipmaps ? 2u : 1u);

    {
        auto *pass = static_cast<Ogre::CompositorPassSceneDef *>(targetDef->addPass(Ogre::PASS_SCENE));
        pass->setAllClearColours(toOgre(p.background));
        pass->setAllLoadActions(Ogre::LoadAction::Clear);
        pass->mStoreActionColour[0] = Ogre::StoreAction::StoreOrResolve;
        pass->mStoreActionDepth   = Ogre::StoreAction::DontCare;
        pass->mStoreActionStencil = Ogre::StoreAction::DontCare;
        // Overlays out by RENDER QUEUE (see kReflectLastRQ), reflector planes
        // out by VISIBILITY BIT (kNoReflectBit) — the two mechanisms cover
        // different things and the spec asks for both.
        pass->mFirstRQ = 0u;
        pass->mLastRQ  = kReflectLastRQ;
        // RESERVED_VISIBILITY_FLAGS must survive: CompositorPassSceneDef's own
        // ctor masks with it, and dropping the reserved bits would hide
        // everything Ogre marks with layer visibility.
        pass->mVisibilityMask = ~kNoReflectBit & Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS;
        // Ogre's own overlay set (the stats readout / loading cover) out too —
        // explicitly, even though kReflectLastRQ (199) already cuts below the
        // overlay RQ 254. The rule in OgreChain.cpp's kIncludeOverlaysNote is
        // "every scene pass says false except THE overlay pass", and a
        // guarantee that leans on an RQ constant somebody may widen later is
        // not a guarantee.
        pass->mIncludeOverlays = false;
        if (!shadowNodeName.empty()) pass->mShadowNode = Ogre::IdString(shadowNodeName);
        pass->mProfilingId = "Jahshaka planar reflection";
    }
    if (p.mipmaps) {
        // Mips ARE the glossiness: the shader samples at
        // perceptualRoughness * planarReflNumMips, so a reflection with one mip
        // is a mirror and nothing else. ApiDefault (a plain hardware mip
        // generate) rather than ComputeHQ deliberately: compute mips force the
        // slot texture to linear instead of sRGB (OgrePlanarReflections.cpp
        // flips the format), pull in a compute dependency we do not need on
        // MoltenVK, and the difference on a 512-1024 px reflection is not
        // visible.
        auto *mips = static_cast<Ogre::CompositorPassMipmapDef *>(targetDef->addPass(Ogre::PASS_MIPMAP));
        mips->mMipmapGenerationMethod = Ogre::CompositorPassMipmapDef::ApiDefault;
        mips->mProfilingId = "Jahshaka planar mipmaps";
    }

    Ogre::CompositorWorkspaceDef *workDef = cm->addWorkspaceDefinition(workspaceDef);
    workDef->connectExternal(0, nodeDef->getName(), 0);
}

void destroyWorkspace(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                      std::vector<std::string> &nodeDefs) {
    if (cm->hasWorkspaceDefinition(workspaceDef)) cm->removeWorkspaceDefinition(workspaceDef);
    for (auto it = nodeDefs.rbegin(); it != nodeDefs.rend(); ++it)
        if (cm->hasNodeDefinition(*it)) cm->removeNodeDefinition(*it);
    nodeDefs.clear();
}

// ---------------------------------------------------------------------------
bool derivePlane(Ogre::SceneNode *node, const Ogre::Item *item, Plane &out,
                 std::string &error) {
    if (!node || !item || !item->getMesh()) { error = "planar reflector: node has no mesh"; return false; }
    const Ogre::Aabb aabb = item->getMesh()->getAabb();
    // The _Updated_ accessors, not the plain ones. Ogre only refreshes derived
    // transforms during SceneManager::updateAllTransforms, i.e. inside
    // renderOneFrame — so a node transformed and marked a reflector in the same
    // tick (which is exactly what a script or an undo does) would otherwise be
    // measured at its PREVIOUS scale, and a plate that had not been flattened
    // yet would be refused as "not flat enough".
    const Ogre::Vector3 scale = node->_getDerivedScaleUpdated();
    const Ogre::Vector3 half(std::abs(aabb.mHalfSize.x * scale.x),
                             std::abs(aabb.mHalfSize.y * scale.y),
                             std::abs(aabb.mHalfSize.z * scale.z));

    // The thin axis is the plane normal; the other two are the rectangle.
    int thin = 0;
    if (half.y < half[thin]) thin = 1;
    if (half.z < half[thin]) thin = 2;
    const int a = (thin + 1) % 3, b = (thin + 2) % 3;
    const float next = std::min(half[a], half[b]);
    // Plate check. A sphere or a cube would produce a plane that matches almost
    // nothing (the 20-degree rule) and reflects at an arbitrary orientation —
    // an obviously broken mirror rather than an approximate one. Refuse it and
    // say why. A zero-thickness plane (half[thin] == 0) is the ideal case.
    if (next <= 1e-6f || half[thin] > kPlateRatio * next) {
        error = "planar reflector: the mesh is not flat enough to be a mirror (its thinnest "
                "extent must be under a tenth of the next). Use a plane or a thin box.";
        return false;
    }

    // The actor's normal is orientation.zAxis(), so build the rotation that
    // takes local +Z onto the local thin axis and let the node's world
    // orientation carry it the rest of the way.
    //   thin X: rotate +90 about Y  (+Z -> +X)
    //   thin Y: rotate -90 about X  (+Z -> +Y)
    //   thin Z: identity
    // and the actor's local X/Y extents follow from where that rotation sends
    // the other two axes.
    Ogre::Quaternion toNormal;
    Ogre::Vector2    halfSize;
    switch (thin) {
    case 0:
        toNormal = Ogre::Quaternion(Ogre::Radian(Ogre::Math::HALF_PI), Ogre::Vector3::UNIT_Y);
        halfSize = Ogre::Vector2(half.z, half.y);
        out.localNormal = Ogre::Vector3::UNIT_X;
        break;
    case 1:
        toNormal = Ogre::Quaternion(Ogre::Radian(-Ogre::Math::HALF_PI), Ogre::Vector3::UNIT_X);
        halfSize = Ogre::Vector2(half.x, half.z);
        out.localNormal = Ogre::Vector3::UNIT_Y;
        break;
    default:
        toNormal = Ogre::Quaternion::IDENTITY;
        halfSize = Ogre::Vector2(half.x, half.y);
        out.localNormal = Ogre::Vector3::UNIT_Z;
        break;
    }
    // The POSITIVE axis, always: the top face of a floor reflects and the
    // underside does not. The shader fades the reflection out by
    // dot(planeNormal, surfaceNormal) over 20 degrees, so the back face falls
    // to zero on its own — no per-face bookkeeping, and the choice is the one a
    // user means by "make this floor reflective".
    const Ogre::Quaternion worldRot = node->_getDerivedOrientationUpdated();
    out.orientation = worldRot * toNormal;
    out.localCentre = aabb.mCenter;
    out.centre      = node->_getDerivedPositionUpdated() + worldRot * (aabb.mCenter * scale);
    out.halfSize    = halfSize;
    return true;
}

}   // namespace planar

// ---------------------------------------------------------------------------
// OgreScene: the scene-level arm.
// ---------------------------------------------------------------------------
bool OgreScene::setPlanarReflections(const PlanarReflectionParams &p) {
    PlanarReflectionParams q = p;
    q.budget     = std::max(0, std::min(8, q.budget));
    q.resolution = std::max(64u, std::min(2048u, q.resolution));
    while (q.resolution & (q.resolution - 1)) q.resolution &= q.resolution - 1;   // down to a POT
    q.maxDistance = std::max(0.01f, q.maxDistance);

    const PlanarReflectionParams &c = mPlanarParams;
    const bool same = c.budget == q.budget && c.resolution == q.resolution &&
                      c.mipmaps == q.mipmaps && c.shadows == q.shadows &&
                      c.accurateLighting == q.accurateLighting &&
                      std::abs(c.maxDistance - q.maxDistance) < 1e-4f &&
                      std::abs(c.background.r - q.background.r) < 1e-4f &&
                      std::abs(c.background.g - q.background.g) < 1e-4f &&
                      std::abs(c.background.b - q.background.b) < 1e-4f;
    // Idempotent on purpose: SceneMirror pushes this every frame, and a rebuild
    // recreates render targets AND recompiles PBS shaders. Same shape as
    // setGlobalIllumination.
    if (same && (q.budget == 0) == (mPlanar == nullptr)) return true;

    mPlanarParams = q;
    JAH_TRY {
        teardownPlanar();
        if (q.budget > 0) rebuildPlanar();
        return true;
    } JAH_CATCH(mError, false);
}

void OgreScene::rebuildPlanar() {
    if (mPlanar || mPlanarParams.budget <= 0) return;
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();

    mPlanarWorkspaceDef = processUniqueName((mName + "/PlanarReflect").c_str());
    // The reflection pass may reference the half-resolution shadow node; the
    // definition is created once by the engine (createShadowNode) and only
    // COSTS memory here, where a workspace instantiates it — one atlas per slot.
    const std::string shadowNode =
        mPlanarParams.shadows && cm->hasShadowNodeDefinition(OgreView::kReflectShadowNodeName)
            ? OgreView::kReflectShadowNodeName
            : std::string();
    planar::buildWorkspace(cm, mPlanarWorkspaceDef, mPlanarParams, shadowNode, mPlanarNodeDefs);

    // lockCamera is inert at our pin (its member and its early-out are both
    // commented out in OgrePlanarReflections.cpp) — pass null and do the camera
    // discrimination ourselves, in the listener.
    mPlanar = new Ogre::PlanarReflections(mSceneMgr, cm, mPlanarParams.maxDistance, nullptr);
    mPlanar->setMaxActiveActors(
        static_cast<Ogre::uint8>(mPlanarParams.budget), Ogre::IdString(mPlanarWorkspaceDef),
        mPlanarParams.accurateLighting, mPlanarParams.resolution, mPlanarParams.resolution,
        mPlanarParams.mipmaps, Ogre::PFG_RGBA8_UNORM_SRGB, /*mipmapMethodCompute*/ false);

    // The receiving half is process-wide, like VCT's.
    if (Ogre::HlmsPbs *pbs = planar::pbsOf(mRoot)) {
        pbs->setPlanarReflections(mPlanar);
        planar::sBindingOwner = this;
    }

    // Re-arm every reflector the document already has. A node whose mesh has
    // since stopped being plate-like simply fails to arm; it keeps its flag so
    // the document is not silently rewritten.
    for (NodeId id : mReflectors) {
        auto it = mNodes.find(id);
        if (it != mNodes.end()) armReflector(id, it->second);
    }
}

void OgreScene::teardownPlanar() {
    if (!mPlanar) {
        // Definitions may outlive a failed build.
        if (mRoot && !mPlanarNodeDefs.empty())
            planar::destroyWorkspace(mRoot->getCompositorManager2(), mPlanarWorkspaceDef, mPlanarNodeDefs);
        return;
    }
    JAH_TRY {
        disarmAllReflectors();
        // ~PlanarReflections leaves HlmsPbs::mPlanarReflections DANGLING — it
        // does not unbind itself. Only the owner clears it (another scene may
        // have taken the binding since).
        if (planar::sBindingOwner == this) {
            if (Ogre::HlmsPbs *pbs = planar::pbsOf(mRoot)) pbs->setPlanarReflections(nullptr);
            planar::sBindingOwner = nullptr;
        }
        delete mPlanar;   // destroys its cameras through mSceneMgr: must precede it
        mPlanar = nullptr;
        planar::destroyWorkspace(mRoot->getCompositorManager2(), mPlanarWorkspaceDef, mPlanarNodeDefs);
        mPlanarWorkspaceDef.clear();
    } JAH_CATCH(mError, );
    mPlanar = nullptr;
}

bool OgreScene::armReflector(NodeId id, Node &n) {
    if (!mPlanar) return true;                  // flag remembered; nothing to arm
    if (mActors.find(id) != mActors.end()) return true;
    if (!n.node || !n.item) { mError = "planar reflector: node has no mesh attached"; return false; }
    planar::Plane pl;
    if (!planar::derivePlane(n.node, n.item, pl, mError)) return false;
    Ogre::PlanarReflectionActor *actor =
        mPlanar->addActor(Ogre::PlanarReflectionActor(pl.centre, pl.halfSize, pl.orientation));
    mActors[id] = actor;
    // The mirror must not appear in its own reflection.
    n.item->setVisibilityFlags(n.item->getVisibilityFlags() | kNoReflectBit);
    // ... and it must RECEIVE one. PBS matches registered renderables to actors
    // dynamically, ignoring the datablock; addRenderable asserts the
    // renderable's mCustomParameter is 0, which is the channel it then owns
    // (nothing else in this backend touches mCustomParameter). Every mesh we
    // build has exactly one submesh, so getSubItem(0) is unambiguous.
    mPlanar->addRenderable(Ogre::PlanarReflections::TrackedRenderable(
        n.item->getSubItem(0), n.item, pl.localNormal, pl.localCentre));
    return true;
}

void OgreScene::disarmReflector(NodeId id, Node &n) {
    auto it = mActors.find(id);
    if (it == mActors.end()) return;
    JAH_TRY {
        // Order matters and the component says so: "You must call
        // removeRenderable before destroying the Renderable."
        if (mPlanar && n.item && n.item->getNumSubItems() > 0)
            mPlanar->removeRenderable(n.item->getSubItem(0));
        if (mPlanar) mPlanar->destroyActor(it->second);
    } JAH_CATCH(mError, );
    mActors.erase(it);
    if (n.item) n.item->setVisibilityFlags(n.item->getVisibilityFlags() & ~kNoReflectBit);
}

void OgreScene::disarmAllReflectors() {
    while (!mActors.empty()) {
        const NodeId id = mActors.begin()->first;
        auto nit = mNodes.find(id);
        if (nit == mNodes.end()) { mActors.erase(mActors.begin()); continue; }
        disarmReflector(id, nit->second);
    }
}

bool OgreScene::setNodePlanarReflector(NodeId id, bool on) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) { mError = "setNodePlanarReflector: unknown node"; return false; }
    if (!on) {
        disarmReflector(id, it->second);
        mReflectors.erase(id);
        return true;
    }
    if (mReflectors.count(id)) return true;
    // Validate NOW even when the arm is down, so the user is told immediately
    // that a sphere cannot be a mirror instead of at some later mode switch.
    if (!it->second.item) { mError = "setNodePlanarReflector: node has no mesh attached"; return false; }
    planar::Plane probe;
    if (!planar::derivePlane(it->second.node, it->second.item, probe, mError)) return false;
    mReflectors.insert(id);
    if (mPlanar && !armReflector(id, it->second)) { mReflectors.erase(id); return false; }
    return true;
}

bool OgreScene::nodePlanarReflector(NodeId id) const { return mReflectors.count(id) != 0; }

int OgreScene::activePlanarReflectors() const {
    return mPlanar ? int(mPlanar->countActiveActors()) : 0;
}

bool OgreScene::dropPlanarForShadowRebuild() {
    if (!mPlanar || !mPlanarParams.shadows) return false;
    teardownPlanar();
    return true;
}

void OgreScene::recreatePlanarAfterShadowRebuild() {
    JAH_TRY { rebuildPlanar(); } JAH_CATCH(mError, );
}

void OgreScene::applyPendingPlanar() {
    if (!mPlanar || mActors.empty()) return;
    // Actors are world-space and do NOT follow a SceneNode, so a moved (or
    // re-parented, or re-scaled) reflector would otherwise reflect from where it
    // used to be. Re-deriving is a handful of floating-point ops per REFLECTOR
    // (not per budget slot, and not per renderable), so it is cheaper than
    // tracking which transforms changed. Note this deliberately does NOT
    // invalidate anything: unlike GI, moving a mirror costs nothing extra.
    JAH_TRY {
        for (auto &kv : mActors) {
            auto nit = mNodes.find(kv.first);
            if (nit == mNodes.end() || !nit->second.node || !nit->second.item) continue;
            planar::Plane pl;
            std::string ignored;
            if (!planar::derivePlane(nit->second.node, nit->second.item, pl, ignored)) continue;
            kv.second->setPlane(pl.centre, pl.halfSize, pl.orientation);
        }
    } JAH_CATCH(mError, );
}

// ---------------------------------------------------------------------------
// OgreView: the per-frame driver.
// ---------------------------------------------------------------------------
void OgreView::syncPlanarListener() {
    Ogre::PlanarReflections *pr = mScene ? mScene->planarReflections() : nullptr;
    if (!pr || !mCamera || !mEnabled) {
        if (mPlanarListener) {
            removeWorkspaceListener(mPlanarListener.get());
            mPlanarListener.reset();
        }
        return;
    }
    if (!mPlanarListener) {
        mPlanarListener.reset(new planar::WorkspaceListener());
        // Registered through the seam, so it is re-attached on every workspace
        // rebuild — setScene, background/shadow change, RTT rebuild, window
        // recreate, MSAA change. That is exactly why the seam exists.
        addWorkspaceListener(mPlanarListener.get());
    }
    // Both ends move underneath us: the scene rebuilds its arm on any parameter
    // change and the view recreates its camera on every setScene.
    mPlanarListener->mReflections = pr;
    mPlanarListener->mCamera      = mCamera;
}

}}}  // namespace jahshaka::engine::detail
