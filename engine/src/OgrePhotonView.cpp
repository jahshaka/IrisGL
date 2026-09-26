// THE PHOTON VIEW (PHOTON-VIEW-1; Types.h PhotonView) — the lighting's own debug
// pictures, beside the Atom view and in its shape: an enum in the boundary header,
// a verb, a gated chain stage, nothing rebuilt when it switches, Off byte-identical.
//
// WHICH PICTURE IS WHOSE:
//   * OGRE'S (the engine's own facilities, wired under the view's lifetime):
//       Voxels  VctLighting::setDebugVisualization — a VoxelVisualizer (a
//               MovableObject + Renderable of the Hlms/Pbs component, drawn with
//               upstream's VCT/VoxelVisualizer material) over the lit voxels of
//               ONE cascade: the finest that holds the camera;
//       Probes  IrradianceField::setDebugVisualization — an IfdProbeVisualizer
//               (the same shape) drawing every probe as a sphere shaded by the
//               field's irradiance atlas.
//     Both attach themselves to the scene's SceneManager on a SCENE_STATIC node
//     of their own and are destroyed by their owner (VctLighting's and
//     IrradianceField's destructors switch the picture off first), so their
//     lifetime is the GI arm's: the scene only puts the picture up, re-puts it
//     when the arm rebuilt under it, and takes it down.
//   * OURS:
//       Cards         the surface cache's table as quads (an Ogre ManualObject
//                     with an HlmsUnlit vertex-colour datablock);
//       ScreenProbes  the gather's records as discs, written by its own
//                     integrate into the view's overlay (OgreRayQuery.cpp);
//       Hits          the reflection trace's per-pixel answer class, written by
//                     the trace into the same overlay;
//       Diffuse /     a pass property on the one lighting text
//       Reflections   (JahPhotonView_piece_ps.any), scoped to the opaque pass.
//
// WHERE THE DRAWABLES ARE SEEN: only by the chain's photon pass (OgreChain.cpp,
// addPhotonViewPasses), through their own visibility channel (kPhotonViewBit) —
// every other pass of every workspace of the manager is blind to them.
#include "EnginePrivate.h"
#include "SurfaceCache.h"

#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <IrradianceField/OgreIfdProbeVisualizer.h>
#include <IrradianceField/OgreIrradianceField.h>
#include <OgreGpuProgramParams.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreLogManager.h>
#include <OgreManualObject2.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreTechnique.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureUnitState.h>
#include <Vct/OgreVctLighting.h>
#include <Vct/OgreVctVoxelizerSourceBase.h>
#include <Vct/OgreVoxelVisualizer.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {

bool drawsGeometry(PhotonView v) {
    return v == PhotonView::Voxels || v == PhotonView::Probes || v == PhotonView::Cards;
}
bool usesOverlay(PhotonView v) { return v == PhotonView::ScreenProbes || v == PhotonView::Hits; }
int isolationOf(PhotonView v) {
    return v == PhotonView::Diffuse ? 1 : v == PhotonView::Reflections ? 2 : 0;
}

/// THE OVERLAY QUAD'S MATERIAL PASS (JahPhotonView.material), or null before the
/// media is parsed.
Ogre::Pass *overlayMaterialPass() {
    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(
        kPhotonOverlayMaterial, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    if (!m) return nullptr;
    m->load();
    Ogre::Technique *t = m->getBestTechnique();
    return t && t->getNumPasses() ? t->getPass(0) : nullptr;
}

/// THE VISUALIZER OGRE JUST ATTACHED. `VctLighting::setDebugVisualization` and
/// `IrradianceField::setDebugVisualization` create their visualizer on a NEW child
/// of the manager's static root and keep the pointer to themselves (no getter at
/// the pin), and the photon view must put it on its own channel — so it is taken
/// from where Ogre put it: the root's last child, its one attached object, checked
/// by type. A pin that moves the node answers null here and the view says so in
/// the log rather than drawing the visualizer into every pass. (The clean fix is a
/// getter on both classes — reported upstream of this lane.)
template <class T>
T *attachedVisualizer(Ogre::SceneManager *sm) {
    if (!sm) return nullptr;
    Ogre::SceneNode *root = sm->getRootSceneNode(Ogre::SCENE_STATIC);
    const size_t n = root ? root->numChildren() : 0u;
    if (!n) return nullptr;
    auto *node = static_cast<Ogre::SceneNode *>(root->getChild(n - 1u));
    if (!node || node->numAttachedObjects() != 1u) return nullptr;
    return dynamic_cast<T *>(node->getAttachedObject(0));
}

/// Puts a debug drawable on the photon channel alone, cast-less, and makes its
/// static world box current (a SCENE_STATIC object's box is otherwise the one it
/// was born with — DOCS/traps/ENGINE.md).
void toPhotonChannel(Ogre::SceneManager *sm, Ogre::MovableObject *mo) {
    mo->setVisibilityFlags(kPhotonViewBit);
    mo->setCastShadows(false);
    if (mo->getParentNode()) sm->notifyStaticDirty(mo->getParentNode());
}

// ---------------------------------------------------------------------------
// THE VIEW'S LISTENER. Every frame, immediately before the view's own workspace
// executes: the photon passes' execution bits (exactly the stage the scene's view
// needs; Off runs none) and the overlay's texture on the quad's material (a
// process singleton, so pushed per view — the Atom view's rule). Around the
// OPAQUE pass (planar::kPlanarUpdatePassIdentifier, the pass that shades the
// picture in every chain shape): the isolation property, pass-scoped.
// ---------------------------------------------------------------------------
class PhotonListener final : public Ogre::CompositorWorkspaceListener {
public:
    explicit PhotonListener(OgreView *view) : mView(view) {}
    ~PhotonListener() { disarm(); }

    void workspacePreUpdate(Ogre::CompositorWorkspace *ws) override {
        if (!ws) return;
        OgreScene *scene = mView ? mView->ogreScene() : nullptr;
        const PhotonView view = scene ? scene->photonView() : PhotonView::Off;
        // A STEREO VIEW never draws Ogre's visualizers (PhotonViewShape::stereo): a
        // mode set before the headset came up keeps painting the desktop's view only.
        bool sceneBit = drawsGeometry(view) &&
                        !(mView->stereo() && (view == PhotonView::Voxels || view == PhotonView::Probes));
        // THE OVERLAY'S COMPOSITE RUNS WHEREVER THE VIEW HOLDS AN OVERLAY: whether the
        // ray tier writes it THIS frame is only known inside the frame, after this
        // mask is read, so the quad itself is told (passEarlyPreExecute) and draws
        // nothing until the first write — no frame of latency, no undefined texel.
        const bool overlayBit = usesOverlay(view) && mView->photonOverlay();
        Ogre::uint8 mask = Ogre::uint8(ws->getExecutionMask() & ~kPhotonExecutionBits);
        if (sceneBit)
            mask = Ogre::uint8(mask | kPhotonSceneExecutionBit |
                               (view == PhotonView::Cards ? kPhotonDepthCopyBit : kPhotonDepthClearBit));
        if (overlayBit) mask = Ogre::uint8(mask | kPhotonOverlayExecutionBit);
        ws->setExecutionMask(mask);
        mIsolation = isolationOf(view);
    }
    /// THE OVERLAY'S COMPOSITE, in front of its quad (before the render pass opens):
    /// the view's overlay bound on the (process-wide) material with its validity
    /// (the tier has written it — this frame's write, recorded earlier in the
    /// frame, counts), and the texture taken from the UAV layout the tier left it
    /// in to a texture, through Ogre's own barrier solver.
    void passEarlyPreExecute(Ogre::CompositorPass *pass) override {
        if (!pass) return;
        // THE PHOTON SCENE PASS: its drawables' world boxes are made current before
        // it culls (OgreScene::photonRefreshBounds says why).
        if (pass->getType() == Ogre::PASS_SCENE &&
            pass->getDefinition()->mExecutionMask == kPhotonSceneExecutionBit) {
            if (OgreScene *scene = mView ? mView->ogreScene() : nullptr) scene->photonRefreshBounds();
            return;
        }
        if (pass->getType() != Ogre::PASS_QUAD) return;
        if (pass->getDefinition()->mExecutionMask != kPhotonOverlayExecutionBit) return;
        Ogre::TextureGpu *overlay = mView ? mView->photonOverlay() : nullptr;
        Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
        if (!overlay || !rs) return;
        Ogre::Pass *mat = overlayMaterialPass();
        if (!mat || !mat->hasFragmentProgram() || mat->getNumTextureUnitStates() < 1u) return;
        mat->getTextureUnitState(0)->setTexture(overlay);
        Ogre::GpuProgramParametersSharedPtr ps = mat->getFragmentProgramParameters();
        ps->setIgnoreMissingParams(true);
        ps->setNamedConstant("photonParams",
                             Ogre::Vector4(mView->photonOverlayWritten() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f));
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, overlay, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    void passPreExecute(Ogre::CompositorPass *pass) override {
        if (!mIsolation || !pass || pass->getType() != Ogre::PASS_SCENE) return;
        if (pass->getDefinition()->mIdentifier != planar::kPlanarUpdatePassIdentifier) return;
        OgreScene *scene = mView ? mView->ogreScene() : nullptr;
        if (!scene || !scene->sceneManager()) return;
        mArmed = scene->sceneManager();
        mArmedPass = pass;
        FogHlmsListener::setPhotonIsolation(mArmed, mIsolation);
    }
    /// Only the pass that armed disarms (a shadow node's passes run inside it and
    /// fire this workspace's listeners too — the Atom listener's lesson).
    void passPosExecute(Ogre::CompositorPass *pass) override {
        if (pass == mArmedPass) disarm();
    }
    void disarm() {
        if (mArmed) FogHlmsListener::setPhotonIsolation(mArmed, 0);
        mArmed = nullptr;
        mArmedPass = nullptr;
    }

private:
    OgreView *mView = nullptr;
    int mIsolation = 0;
    Ogre::SceneManager *mArmed = nullptr;
    Ogre::CompositorPass *mArmedPass = nullptr;
};

}   // namespace

PhotonListenerPtr makePhotonListener(OgreView *view) {
    return PhotonListenerPtr(new PhotonListener(view));
}
void PhotonListenerDeleter::operator()(Ogre::CompositorWorkspaceListener *l) const {
    delete static_cast<PhotonListener *>(l);
}

// ---------------------------------------------------------------------------
// THE VIEW HALF
// ---------------------------------------------------------------------------
void OgreView::syncPhotonView() {
    if (!mScene || !mCamera) {
        if (mPhotonListener) {
            removeWorkspaceListener(mPhotonListener.get());
            mPhotonListener.reset();
        }
        if (mPhotonOverlay) retirePhotonOverlay();
        return;
    }
    if (!mPhotonListener) {
        mPhotonListener = makePhotonListener(this);
        addWorkspaceListener(mPhotonListener.get());
    }
    const ChainDesc cd = chainDesc();
    OgreScene::PhotonViewShape shape;
    shape.passes = cd.anyEffect();
    shape.rayReflect = cd.rayReflect;
    shape.probeGather = cd.probeGather;
    shape.stereo = cd.stereo;
    mScene->notePhotonView(this, !isOffscreen() || stereo(), shape);

    // THE OVERLAY follows the scene's view and the target's size. Created here,
    // between frames; the tier writes it inside the frame and says so.
    const PhotonView view = mScene->photonView();
    Ogre::TextureGpu *t = target();
    // Not on the chain's ray rows: a new view (a screenshot's) learns its scene and
    // rebuilds its chain with the ray jobs a frame or two later, and a shot of a few
    // frames must not wait a frame more for the overlay the tier then writes.
    const bool want = usesOverlay(view) && shape.passes && t;
    if (mPhotonOverlay && (!want || mPhotonOverlay->getWidth() != t->getWidth() ||
                           mPhotonOverlay->getHeight() != t->getHeight()))
        retirePhotonOverlay();
    if (want && !mPhotonOverlay) {
        JAH_TRY {
            static unsigned sSerial = 0u;
            Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
            Ogre::TextureGpu *o = tm->createTexture(
                "JahPhotonOverlay/" + std::to_string(++sSerial), Ogre::GpuPageOutStrategy::Discard,
                Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
            o->setResolution(t->getWidth(), t->getHeight(), 1u);
            o->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
            o->setNumMipmaps(1u);
            // RESIDENT FOR GOOD, never per frame (the gather's irradiance rule).
            o->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
            mPhotonOverlay = o;
            mPhotonOverlayWritten = false;
            ++mPhotonOverlayGeneration;
        } JAH_CATCH(mError, );
    }
}

// ---------------------------------------------------------------------------
// THE SCENE HALF
// ---------------------------------------------------------------------------
void OgreScene::notePhotonView(const void *view, bool presents, const PhotonViewShape &shape) {
    if (presents) mPhotonViews[view] = shape;
    else mPhotonViews.erase(view);
}

std::string OgreScene::photonViewRefusal(PhotonView view) const {
    if (view == PhotonView::Off) return std::string();
    bool passes = false, reflect = false, gather = false, stereo = false;
    for (const auto &kv : mPhotonViews) {
        if (!kv.second.passes) continue;
        passes = true;
        reflect = reflect || kv.second.rayReflect;
        gather = gather || kv.second.probeGather;
        stereo = stereo || kv.second.stereo;
    }
    if (!passes)
        return "no viewport of this scene draws the post chain (the Low tier's viewport draws straight "
               "into its window), so the photon view has nowhere to paint";
    const auto stereoRefusal = [](const char *what) {
        return std::string("a stereo (headset) view draws this scene, and Ogre's ") + what +
               " visualizer is a low-level material the engine's instanced stereo does not serve";
    };
    switch (view) {
    case PhotonView::Voxels:
        if (!mVctLighting)
            return "this scene holds no voxel lighting (GI off, or a mode that builds no voxels)";
        if (stereo) return stereoRefusal("voxel");
        break;
    case PhotonView::Probes:
        if (!mIfd)
            return "this scene holds no irradiance field (world.gi({ddgi}) is off, or the tier builds none)";
        if (stereo) return stereoRefusal("probe");
        break;
    case PhotonView::Cards:
        if (!mSurfaceCache || !mSurfaceCache->built())
            return "this scene holds no surface cache (the cards row is off: it follows the reflection trace)";
        break;
    case PhotonView::ScreenProbes:
        if (!gather)
            return "the screen-probe gather is off here (it traces with the ray tier: High and Epic with "
                   "ray tracing on)";
        break;
    case PhotonView::Hits:
        if (!reflect)
            return "no viewport traces reflections here (ray tracing off, or the tier traces none)";
        break;
    default:
        break;
    }
    return std::string();
}

bool OgreScene::photonVoxelLive() const {
    if (!mPhotonVoxelLighting) return false;
    bool live = mPhotonVoxelLighting == mVctLighting;
    for (const VctCascade &c : mVctCascades) live = live || c.lighting == mPhotonVoxelLighting;
    return live && mPhotonVoxelLighting->getDebugVisualizationMode();
}

void OgreScene::photonVoxelsOff() {
    // A lighting that died took its visualizer with it (~VctLighting switches the
    // picture off); only a LIVE one is asked to.
    if (photonVoxelLive()) mPhotonVoxelLighting->setDebugVisualization(false, mSceneMgr);
    mPhotonVoxelLighting = nullptr;
    mPhotonVoxelVis = nullptr;
    mPhotonVoxelSource = nullptr;
    mPhotonVoxelTex = nullptr;
}

void OgreScene::photonProbesOff() {
    // mPhotonIfd is always the LIVE field or null: teardownIrradianceField
    // forgets it before the field (and Ogre's visualizer with it) is deleted.
    // (IrradianceField::getDebugVisualizationMode returns the mode as a BOOL at
    // the pin — Colour reads false — so the scene's own record is the witness.)
    if (mPhotonIfd && mPhotonIfd == mIfd)
        mPhotonIfd->setDebugVisualization(Ogre::IrradianceField::DebugVisualizationNone, mSceneMgr,
                                          mPhotonIfd->getDebugTessellation());
    mPhotonIfd = nullptr;
    mPhotonIfdVis = nullptr;
    mPhotonIfdFollows = ~0ull;
}

void OgreScene::photonCardsOff() {
    if (mPhotonCards && mSceneMgr) {
        mPhotonCards->detachFromParent();
        mSceneMgr->destroyManualObject(mPhotonCards);
    }
    mPhotonCards = nullptr;
    mPhotonCardQuads = mPhotonCardVerts = 0u;
}

void OgreScene::photonRefreshBounds() {
    // ONLY A LIVE ONE: a cascade rebuilt since syncPhotonView (inside this frame)
    // deleted its lighting and Ogre's visualizer with it — the pointer is then
    // matched against the live chain before anything is dereferenced (and a new
    // lighting recycled at the same address holds no visualizer of ours).
    if (mPhotonVoxelVis && photonVoxelLive() && mPhotonVoxelVis->getParentNode())
        mPhotonVoxelVis->getWorldAabbUpdated();
    if (mPhotonIfdVis && mPhotonIfdVis->getParentNode()) mPhotonIfdVis->getWorldAabbUpdated();
}

void OgreScene::releasePhotonView() {
    photonVoxelsOff();
    photonProbesOff();
    photonCardsOff();
}

namespace {
/// THE CARDS' DATABLOCK: HlmsUnlit with the quads' vertex colours, opaque in the
/// photon layer (the layer's composite is the blend), depth-tested against the
/// scene's depth and never written, with a small bias so a card on its own surface
/// wins, both faces. Created once per process, on first use.
const char *photonCardsDatablock(Ogre::Root *root) {
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    if (!hm) return nullptr;
    auto *unlit = static_cast<Ogre::HlmsUnlit *>(hm->getHlms(Ogre::HLMS_UNLIT));
    if (!unlit) return nullptr;
    if (!unlit->getDatablock(kPhotonCardsMaterial)) {
        Ogre::HlmsMacroblock mb;
        mb.mDepthCheck = true;
        mb.mDepthWrite = false;
        mb.mCullMode = Ogre::CULL_NONE;
        mb.mDepthBiasConstant = 2.0f;
        mb.mDepthBiasSlopeScale = 1.0f;
        Ogre::HlmsBlendblock bb;
        unlit->createDatablock(kPhotonCardsMaterial, kPhotonCardsMaterial, mb, bb,
                               Ogre::HlmsParamVec());
    }
    return kPhotonCardsMaterial;
}

/// Capture age -> colour, on a LOG scale of cache frames: green when fresh,
/// yellow at about thirty seconds (1,900 frames), red at ten minutes (36,000) and
/// beyond. Logarithmic because a still scene recaptures nothing: a linear ramp
/// turned every card of a settled room red within seconds.
Ogre::ColourValue ageColour(unsigned long long age) {
    const float t = std::min(1.0f, std::log1p(float(age)) / std::log1p(36000.0f));
    return t < 0.5f ? Ogre::ColourValue(2.0f * t, 1.0f, 0.0f, 1.0f)
                    : Ogre::ColourValue(1.0f, 2.0f * (1.0f - t), 0.0f, 1.0f);
}
}   // namespace

void OgreScene::syncPhotonView() {
    if (!mSceneMgr) return;
    const PhotonView view = mPhotonView;

    // ---- VOXELS: Ogre's VoxelVisualizer on the finest cascade holding the camera.
    {
        Ogre::VctLighting *want = nullptr;
        if (view == PhotonView::Voxels) {
            if (mPhotonVoxelCascade >= 0 && !mVctCascades.empty()) {
                // THE CASCADE ASKED FOR, clamped to the chain.
                const size_t ci = std::min(size_t(mPhotonVoxelCascade), mVctCascades.size() - 1u);
                if (mVctCascades[ci].built) want = mVctCascades[ci].lighting;
            } else {
                // THE FINEST CASCADE THAT HOLDS THE SCENE'S LIT CONTENT (the fitted GI
                // volume, giStatus().boundsMin/Max) — not the finest that holds the
                // camera: that one is a box of a few metres centred on the eye, and from
                // the saved cameras of both the default scene and Showroom 2 its voxels
                // are out of frame (measured: 0 pixels changed). A scene larger than
                // every cascade shows the outermost.
                const Ogre::Vector3 &h = mGiLitVolume.mHalfSize;
                const bool haveLit = h.squaredLength() > 0.0f && std::isfinite(h.x) &&
                                     std::isfinite(h.y) && std::isfinite(h.z);
                const Ogre::Vector3 lo = mGiLitVolume.getMinimum(), hi = mGiLitVolume.getMaximum();
                for (const VctCascade &c : mVctCascades) {
                    if (!c.lighting || !c.built) continue;
                    want = c.lighting;   // the outermost built one, if none holds it
                    if (!haveLit) break;
                    const Ogre::Vector3 cl = c.centre - Ogre::Vector3(c.halfSize),
                                        ch = c.centre + Ogre::Vector3(c.halfSize);
                    if (cl.x <= lo.x && cl.y <= lo.y && cl.z <= lo.z && ch.x >= hi.x && ch.y >= hi.y &&
                        ch.z >= hi.z)
                        break;
                }
            }
            // No chain (the single volume), or a camera outside every cascade: the
            // head of the chain.
            if (!want) want = mVctLighting;
        }
        const void *source = want ? static_cast<const void *>(want->getVoxelizer()) : nullptr;
        Ogre::TextureGpu *tex = want && want->getLightVoxelTextures() ? want->getLightVoxelTextures()[0] : nullptr;
        const Ogre::Vector3 origin =
            want && want->getVoxelizer() ? want->getVoxelizer()->getVoxelOrigin() : Ogre::Vector3::ZERO;
        const bool same = want == mPhotonVoxelLighting &&
                          (!want || (source == mPhotonVoxelSource && tex == mPhotonVoxelTex &&
                                     origin == mPhotonVoxelOrigin && want->getDebugVisualizationMode()));
        if (!same) {
            photonVoxelsOff();
            if (want && source && tex) {
                want->setDebugVisualization(true, mSceneMgr);
                auto *vis = attachedVisualizer<Ogre::VoxelVisualizer>(mSceneMgr);
                if (!vis) {
                    // Never leave Ogre's visualizer on the default channels, where
                    // every pass would draw it.
                    want->setDebugVisualization(false, mSceneMgr);
                    static bool said = false;
                    if (!said) {
                        said = true;
                        Ogre::LogManager::getSingleton().logMessage(
                            "Jahshaka photon view: the voxel visualizer was not where Ogre attaches it — "
                            "the Voxels view draws nothing",
                            Ogre::LML_CRITICAL);
                    }
                } else {
                    toPhotonChannel(mSceneMgr, vis);
                    // The visualizer is STATIC: its transform and box are forced
                    // current here, as upstream's own resetTexturesFromBuildRelative does.
                    vis->getParentNode()->_getFullTransformUpdated();
                    vis->getWorldAabbUpdated();
                    mPhotonVoxelLighting = want;
                    mPhotonVoxelVis = vis;
                    mPhotonVoxelSource = source;
                    mPhotonVoxelTex = tex;
                    mPhotonVoxelOrigin = origin;
                }
            }
        }
    }

    // ---- PROBES: Ogre's IfdProbeVisualizer on the field. A re-initialize keeps it
    // (IrradianceField::initialize re-points it), a follow moves its STATIC node
    // (the fork's setFieldVolume / scrollWindow), whose box is then re-derived.
    {
        Ogre::IrradianceField *want = view == PhotonView::Probes ? mIfd : nullptr;
        if (want != mPhotonIfd) {
            photonProbesOff();
            if (want) {
                // Tessellation 4: a 7 x 16 band sphere, ~110 triangles a probe (8
                // would be 32,000 — 266 M triangles for a 8,192-probe field).
                want->setDebugVisualization(Ogre::IrradianceField::DebugVisualizationColour, mSceneMgr, 4u);
                auto *vis = attachedVisualizer<Ogre::IfdProbeVisualizer>(mSceneMgr);
                if (!vis) {
                    want->setDebugVisualization(Ogre::IrradianceField::DebugVisualizationNone, mSceneMgr, 4u);
                    static bool said = false;
                    if (!said) {
                        said = true;
                        Ogre::LogManager::getSingleton().logMessage(
                            "Jahshaka photon view: the probe visualizer was not where Ogre attaches it — "
                            "the Probes view draws nothing",
                            Ogre::LML_CRITICAL);
                    }
                } else {
                    toPhotonChannel(mSceneMgr, vis);
                    mPhotonIfd = want;
                    mPhotonIfdVis = vis;
                    mPhotonIfdFollows = mIfdFollows;
                }
            }
        } else if (mPhotonIfd && mIfdFollows != mPhotonIfdFollows) {
            mPhotonIfdFollows = mIfdFollows;
            if (mPhotonIfdVis && mPhotonIfdVis->getParentNode())
                mSceneMgr->notifyStaticDirty(mPhotonIfdVis->getParentNode());
        }
    }

    // ---- CARDS: the cache's table as quads, rebuilt every frame the view is on.
    if (view != PhotonView::Cards || !mSurfaceCache || !mSurfaceCache->built()) {
        if (mPhotonCards) photonCardsOff();
    } else {
        static std::vector<PhotonCardQuad> quads;
        mSurfaceCache->photonQuads(quads);
        size_t held = 0u;
        for (const PhotonCardQuad &q : quads) held += q.held ? 1u : 0u;
        const size_t outlined = quads.size() - held;
        // A held card is one quad (4 vertices); a waiting one an outline of four
        // thin quads (16).
        const size_t verts = held * 4u + outlined * 16u;
        const char *db = photonCardsDatablock(mRoot);
        if (!verts || !db) {
            if (mPhotonCards) photonCardsOff();
        } else {
            const bool rebuild = !mPhotonCards || verts != mPhotonCardVerts;
            if (!mPhotonCards) {
                mPhotonCards = mSceneMgr->createManualObject(Ogre::SCENE_DYNAMIC);
                mSceneMgr->getRootSceneNode(Ogre::SCENE_DYNAMIC)->attachObject(mPhotonCards);
                mPhotonCards->setVisibilityFlags(kPhotonViewBit);
                mPhotonCards->setCastShadows(false);
            }
            if (rebuild) {
                mPhotonCards->clear();
                mPhotonCards->begin(db, Ogre::OT_TRIANGLE_LIST);
            } else {
                mPhotonCards->beginUpdate(0);
            }
            Ogre::uint32 base = 0u;
            const auto quad = [&](const Ogre::Vector3 &a, const Ogre::Vector3 &b, const Ogre::Vector3 &c,
                                  const Ogre::Vector3 &d, const Ogre::ColourValue &col) {
                for (const Ogre::Vector3 *p : { &a, &b, &c, &d }) {
                    mPhotonCards->position(*p);
                    mPhotonCards->colour(col);
                }
                mPhotonCards->quad(base, base + 1u, base + 2u, base + 3u);
                base += 4u;
            };
            const Ogre::ColourValue waiting(1.0f, 0.9f, 0.1f, 1.0f);
            for (const PhotonCardQuad &q : quads) {
                // Lifted off its plane by a millimetre and a thousandth of its size:
                // the depth bias does the rest.
                const float lift = 0.001f + 0.001f * std::max(q.halfU.length(), q.halfV.length());
                const Ogre::Vector3 c = q.centre + q.normal * lift;
                const Ogre::Vector3 p00 = c - q.halfU - q.halfV, p10 = c + q.halfU - q.halfV,
                                    p11 = c + q.halfU + q.halfV, p01 = c - q.halfU + q.halfV;
                if (q.held) {
                    quad(p00, p10, p11, p01, ageColour(q.age));
                } else {
                    // The outline's width: a twentieth of the card's shorter side, at
                    // most 4 cm (a ground card is tens of metres wide).
                    const float w = std::min(0.04f, 0.1f * std::min(q.halfU.length(), q.halfV.length()));
                    const Ogre::Vector3 du = q.halfU.normalisedCopy() * w, dv = q.halfV.normalisedCopy() * w;
                    quad(p00, p10, p10 + dv, p00 + dv, waiting);
                    quad(p01 - dv, p11 - dv, p11, p01, waiting);
                    quad(p00, p00 + du, p01 + du, p01, waiting);
                    quad(p10 - du, p10, p11, p11 - du, waiting);
                }
            }
            mPhotonCards->end();
            mPhotonCardVerts = verts;
            mPhotonCardQuads = quads.size();
        }
    }
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka
