// ATOM S3-DRAW — the visibility buffer as the product's opaque path
// (SPECS/atom/D3_S3_DRAW_DESIGN.md; SPECS/briefs/ATOM-S3-DRAW.md).
//
// THIS FILE HOLDS THE SPLIT'S ONE DECISION (`atomRouteFor`: which items the id
// pass draws and the decode shades, and why the rest stay on stock HlmsPbs) and
// the scene's reading of it (`atomDrawStatus`: the split's counts and the decode
// BUCKETS the atom items' materials need — HlmsAtom::BucketKey).
#include "EnginePrivate.h"
#include "GpuScene.h"
#include "AlphaMasks.h"
#include "HlmsAtom.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreItem.h>
#include <OgreMaterial.h>
#include <OgreMaterialManager.h>
#include <OgrePass.h>
#include <OgreRenderSystem.h>
#include <OgreStagingTexture.h>
#include <OgreTechnique.h>
#include <OgreTextureBox.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureUnitState.h>
#include <OgreGpuProgramParams.h>
#include <OgreCamera.h>
#include <OgreMesh2.h>
#include <OgreSubMesh2.h>
#include <OgreRoot.h>
#include <OgreSubItem.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace jahshaka {
namespace engine {
namespace detail {

namespace {
/// The queue renderQueueFor gives an ordinary item (Ogre's default).
constexpr Ogre::uint8 kOpaqueItemQueue = 10u;

HlmsAtom *registeredAtom() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    return hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
}
}  // namespace

// ---------------------------------------------------------------------------
// THE SCREEN DECODE'S ARMING (ATOM S3-DRAW; ATOM-DECODE-CLASS-1). The view's SCREEN
// DECODE PASSES (kScreenDecodePassIdentifier: one in front of the prepass, one in
// front of the opaque pass) are the passes whose draws are the screen decode: for
// the length of each HlmsAtom is handed the view's id image and the GPU scene's
// tables, the scene's screen decode draws and its classifier are shown, and the
// pass is CLASSIFIED (the material depth). The prepass and the opaque pass SKIP the
// Atom queue (its items are the id pass's) and draw no decode.
// ---------------------------------------------------------------------------
namespace {
/// THE ATOM VIEW'S QUAD MATERIAL PASS (JahAtomView.material), or null before the
/// media is parsed.
Ogre::Pass *atomViewMaterialPass() {
    Ogre::MaterialPtr m = Ogre::MaterialManager::getSingleton().getByName(
        kAtomViewMaterial, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    if (!m) return nullptr;
    m->load();
    Ogre::Technique *t = m->getBestTechnique();
    return t && t->getNumPasses() ? t->getPass(0) : nullptr;
}

/// A STOCK SLOT IN THE CAMERA'S VIEW (ATOM-4 stage 1b): any live slot of the scene's table that the id pass does not
/// draw (GpuScene::stockSlots) whose world box reaches the camera's frustum — the stock depth pass's subject. No GPU
/// scene: true (the stock arm is exact whatever the frame holds).
bool stockInView(OgreScene *scene, Ogre::Camera *cam) {
    detail::GpuScene &gs = scene->gpuScene();
    if (!gs.live() || !cam) return true;
    for (uint32_t slot : gs.stockSlots()) {
        const detail::GpuInstance &e = gs.entry(slot);
        const Ogre::AxisAlignedBox box(Ogre::Vector3(e.boundsMin[0], e.boundsMin[1], e.boundsMin[2]),
                                       Ogre::Vector3(e.boundsMax[0], e.boundsMax[1], e.boundsMax[2]));
        if (cam->isVisible(box)) return true;
    }
    return false;
}

class AtomDrawListener final : public Ogre::CompositorWorkspaceListener {
public:
    explicit AtomDrawListener(OgreView *view) : mView(view) {}
    /// THE ATOM VIEW'S SWITCH (D0-ATOM-VIEW), immediately before THIS workspace's
    /// passes execute: its two passes (the depth copy, the quad) carry
    /// kAtomViewExecutionBit and nothing else, so the bit in the workspace's mask IS
    /// the view — no rebuild, and Off runs neither. The quad's material is a process
    /// singleton, so its mode and its Buckets table are pushed here, per view, the
    /// way the looks push theirs (the values are read at pass execute time).
    void workspacePreUpdate(Ogre::CompositorWorkspace *ws) override {
        if (!ws) return;
        OgreScene *scene = mView ? mView->ogreScene() : nullptr;
        // THE CAPTURE'S RENDER (View::requestFrameCapture) is the picture, never the
        // viewing aid: the Atom view stays off in that workspace alone.
        const bool own = scene && ws != mView->captureWorkspace();
        const AtomView view = own ? scene->atomView() : AtomView::Off;
        // THE DENSITY COUNT AND THE CUT PROBE (ATOM-2): the view's own frames take the scene's probe arming and
        // count their visible Atom pixels for the Density mode or the probe; the Density mode's table is the
        // view's own (its pixels are this view's), and the probe's frame runs the two passes with the quad's
        // mode 0 (it paints nothing) where the scene's view is Off.
        bool probePaint = false;
        Ogre::TextureGpu *ownTable = own ? atomDensityFrame(mView, scene, view, probePaint) : nullptr;
        // ...and a Density frame whose count could not be made paints nothing (the scene's table holds no densities).
        Ogre::TextureGpu *table = ownTable                     ? ownTable
                                  : view == AtomView::Density ? nullptr
                                  : scene                     ? scene->atomViewTable()
                                                              : nullptr;
        Ogre::Pass *pass = ((view != AtomView::Off || probePaint) && table) ? atomViewMaterialPass() : nullptr;
        const bool on = pass && pass->hasFragmentProgram() && pass->getNumTextureUnitStates() >= 4u;
        Ogre::uint8 mask = ws->getExecutionMask();
        mask = on ? Ogre::uint8(mask | kAtomViewExecutionBit) : Ogre::uint8(mask & ~kAtomViewExecutionBit);
        // THE STOCK DEPTH'S ARM (ATOM-4 stage 1b): the pass exists to carry stock occluders into the late pyramid;
        // with no stock slot's box in the camera's view there is nothing to draw, and the late pass's pyramid of
        // the Atom depth alone is exact (today's frame). THIS frame's boxes, read before the passes record.
        const bool stock = !scene || stockInView(scene, mView->camera());
        mask = Ogre::uint8((mask & ~(kAtomStockExecutionBit | kAtomNoStockExecutionBit)) |
                           (stock ? kAtomStockExecutionBit : kAtomNoStockExecutionBit));
        ws->setExecutionMask(mask);
        if (!on) return;
        Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
        ps->setIgnoreMissingParams(true);
        ps->setNamedConstant("atomViewParams", Ogre::Vector4(Ogre::Real(int(view)), 0, 0, 0));
        pass->getTextureUnitState(3)->setTexture(table);
    }
    /// THE DENSITY COUNT (ATOM-2): at the Atom view's quad, before its render pass begins — the id image, the id
    /// pass's depth copy and the scene's final depth are all this frame's there.
    void passEarlyPreExecute(Ogre::CompositorPass *pass) override {
        if (!pass || pass->getType() != Ogre::PASS_QUAD || !mView) return;
        const auto *def = static_cast<const Ogre::CompositorPassQuadDef *>(pass->getDefinition());
        if (!def || def->mMaterialName != kAtomViewMaterial) return;
        if (pass->getParentNode()->getWorkspace() == mView->captureWorkspace()) return;
        if (OgreScene *scene = mView->ogreScene()) atomDensityRecord(mView, scene, pass);
    }
    void passPreExecute(Ogre::CompositorPass *pass) override {
        if (!pass || pass->getType() != Ogre::PASS_SCENE || !mView) return;
        const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
        if (!def || def->mIdentifier != kScreenDecodePassIdentifier) return;
        // A MEASUREMENT ARM, never a mode ("atom.decode", engine.atom_draw sets it): the
        // decode left unarmed, so the Atom items are drawn by NOTHING in this pass —
        // engine.atom_draw's proof that the view's passes really skip their queue.
        if (mView->mEngine && !mView->mEngine->armRegistry().on(detail::ArmId::AtomDecode)) return;
        OgreScene *scene = mView->ogreScene();
        HlmsAtom *atom = registeredAtom();
        if (!scene || !atom) return;
        detail::GpuScene &gs = scene->gpuScene();
        if (!gs.live()) return;
        Ogre::TextureGpu *ids = nullptr;
        try {
            ids = pass->getParentNode()->getDefinedTexture(Ogre::IdString(kAtomIdTexture));
        } catch (Ogre::Exception &) {
            if (!mSaidNoIds) {
                mSaidNoIds = true;
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka atom draw: a pass skipping the Atom queue ('" + def->mProfilingId +
                        "', node '" + pass->getParentNode()->getName().getFriendlyText() +
                        "') has no id image in its node — its decode is not armed",
                    Ogre::LML_CRITICAL);
            }
        }
        if (!ids) return;
        gs.flushGeomRows();
        HlmsAtom::DecodeSource src;
        src.ids = ids;
        src.instances = gs.instanceBuffer();
        src.levels = gs.levelBuffer();
        src.geomRows = gs.geomBuffer();
        if (atomTraceOn() && gs.clusterDirty())
            atomTrace("the cluster tables were dirtied between the id pass and the decode");
        gs.flushClusterTables();
        src.meshes = gs.meshBuffer();
        src.clusters = gs.clusterBuffer();
        src.classified = true;
        atom->setDecodeSource(src);
        atom->showScreenDecodes(scene->sceneManager(), true);
        mArmed = scene->sceneManager();
        mArmedPass = pass;
    }
    /// Only the pass that armed disarms: the SHADOW NODE's passes run INSIDE the
    /// pass that executes it (after its pre-execute, before its cull) and fire this
    /// workspace's listeners too — disarming on theirs left the prepass without its
    /// decode (measured: no prepass permutation was ever compiled in the app, whose
    /// maps re-render every frame).
    void passPosExecute(Ogre::CompositorPass *pass) override {
        if (pass == mArmedPass) disarm();
    }
    void disarm() {
        if (!mArmed) return;
        if (HlmsAtom *atom = registeredAtom()) {
            atom->showScreenDecodes(mArmed, false);
            atom->setDecodeSource(HlmsAtom::DecodeSource());
        }
        mArmed = nullptr;
        mArmedPass = nullptr;
    }
    ~AtomDrawListener() { disarm(); }
    OgreView *view() const { return mView; }

private:
    OgreView *mView = nullptr;
    bool mSaidNoIds = false;   ///< the missing id image said once, per view
    Ogre::SceneManager *mArmed = nullptr;
    Ogre::CompositorPass *mArmedPass = nullptr;
};
}  // namespace

OgreView *atomViewOfPass(const Ogre::CompositorPass *pass) {
    const Ogre::CompositorNode *node = pass ? pass->getParentNode() : nullptr;
    const Ogre::CompositorWorkspace *ws = node ? node->getWorkspace() : nullptr;
    if (!ws) return nullptr;
    for (Ogre::CompositorWorkspaceListener *l : ws->getListeners())
        if (auto *al = dynamic_cast<AtomDrawListener *>(l)) return al->view();
    return nullptr;
}

AtomDrawListenerPtr makeAtomDrawListener(OgreView *view) {
    return AtomDrawListenerPtr(new AtomDrawListener(view));
}
void AtomDrawListenerDeleter::operator()(Ogre::CompositorWorkspaceListener *l) const {
    delete static_cast<AtomDrawListener *>(l);
}

void OgreView::syncAtomDraw() {
    const ChainDesc cd = chainDesc();
    const bool atomDraw = cd.atomDraw;
    if (mScene) {
        const bool on = mScene->atomDrawWanted();
        mScene->noteAtomPbsView(this, on && mStereo, on && !mStereo && !atomDraw);
    }
    // THE SHAPE: the scene's split decides it, and a view learns its scene late
    // (and its target's sample count can change under it). ...AND SSAO's
    // (SSAO-DOUBLE-1): the scene's GI mode refuses it, and that mode moves between
    // frames — sceneShapeMoved compares all three.
    if (sceneShapeMoved()) rebuildWorkspaceDef();
    const bool wanted = mChainAtomDraw && mScene && mCamera;
    if (!wanted) {
        if (mAtomListener) {
            removeWorkspaceListener(mAtomListener.get());
            mAtomListener.reset();
        }
        return;
    }
    if (!mAtomListener) {
        mAtomListener = makeAtomDrawListener(this);
        addWorkspaceListener(mAtomListener.get());
    }
}

bool OgreScene::atomDrawOn() const {
    // U3 stage 4a: a read on the host's side is a REQUEST, answered on the engine's side (atomDrawStatus's rule).
    if (hostPosts()) {
        bool r = false;
        const_cast<OgreScene *>(this)->requestOnEngine([&] { r = atomDrawOn(); });
        return r;
    }
    return mGpuScene.live() && atomDrawWanted();
}

bool OgreScene::atomDrawWanted() const {
    return mAtom.drawEnabled() && mRoot && atomIdPassSupported(mRoot->getRenderSystem());
}

void OgreScene::setAtomDrawEnabled(bool on) {
    // THE HOST'S CALL IS A MESSAGE of the scene's stream (U3 stage 4a, decision 39), applied at the drain.
    if (postScene([=](OgreScene &s) { s.setAtomDrawEnabled(on); return true; })) return;
    if (on == mAtom.drawEnabled()) return;
    mAtom.setDrawEnabled(on);
    // Every slot re-composes (its route and its queue follow), and every view of
    // this scene re-derives its chain (the id pass comes or goes) on its next
    // description push — the engine's per-frame view sync compares shapes.
    for (Node *n : mItemNodes)
        if (n) markGpuSlotDirty(*n);
}

void OgreScene::setAtomOcclusionEnabled(bool on) {
    if (postScene([=](OgreScene &s) { s.setAtomOcclusionEnabled(on); return true; })) return;   // U3 4a: a message
    JAH_HOST_CALL("Scene::setAtomOcclusionEnabled");
    // A SHAPE CHANGE and nothing else: every view of this scene re-derives its chain
    // (the pyramid and the late pass come or go) on its next sync, as for the split.
    mAtom.setOcclusionEnabled(on);
}

void OgreScene::setAtomCutBudgetForTest(unsigned indices) {
    if (postScene([=](OgreScene &s) { s.setAtomCutBudgetForTest(indices); return true; })) return;   // U3 4a: a message
    JAH_HOST_CALL("Scene::setAtomCutBudgetForTest");
    for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>()) {
        if (!v) continue;
        v->atomCull().setCutBudgetForTest(indices);
        v->atomCullLate().setCutBudgetForTest(indices);
    }
}

void OgreScene::placeAtomQueue(const Node &n, bool atom) const {
    Ogre::Item *item = n.item;
    if (!item) return;
    const Ogre::uint8 rq = item->getRenderQueueGroup();
    if (atom) {
        if (rq != kAtomRenderQueue) item->setRenderQueueGroup(kAtomRenderQueue);
        return;
    }
    if (rq != kAtomRenderQueue) return;
    // BACK WHERE ITS MATERIAL FILES IT (renderQueueFor) — the route left Atom.
    auto mit = mMaterials.find(n.materialRef);
    item->setRenderQueueGroup(mit != mMaterials.end() ? renderQueueFor(mit->second)
                                                      : kOpaqueItemQueue);
}

/// THE PBS CHANGE LOG'S DRAIN (EnginePrivate.h's declaration says what and why).
/// Distinct datablocks first (a flush notes every renderable of one datablock), then
/// the Items, each told to the scene that indexes it.
void OgreEngine::drainPbsChanges() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    auto *pbs = hm ? dynamic_cast<ScenePbs *>(hm->getHlms(Ogre::HLMS_PBS)) : nullptr;
    if (!pbs) return;
    const std::vector<ScenePbs::HashNote> &notes = pbs->hashNotes();
    if (pbs->hashNotesOverflowed()) {
        // THE LOG OVERFLOWED (kMaxHashNotes): every twin whose bucket moved leaves,
        // and every item of every scene is re-composed — once.
        if (HlmsAtom *atom = registeredAtom()) atom->forgetMovedDecodeTwins();
        for (auto &s : mScenes)
            if (s) s->markAllItemsRehashed();
        ++mPbsDrainOverflows;
        pbs->clearHashNotes();
        return;
    }
    std::vector<const Ogre::HlmsDatablock *> &dbs = mPbsDrainScratch;
    dbs.clear();
    for (const ScenePbs::HashNote &n : notes)
        if (n.db) dbs.push_back(n.db);
    pbs->forEachDirtyDatablock([&dbs](const Ogre::HlmsDatablock *db) { dbs.push_back(db); });
    mPbsDrainNotes += notes.size();
    if (dbs.empty() && notes.empty()) return;
    std::sort(dbs.begin(), dbs.end());
    dbs.erase(std::unique(dbs.begin(), dbs.end()), dbs.end());
    // A TWIN LEAVES ONLY WHEN ITS BUCKET MOVED (forgetDecodeTwinIfMoved; a lookup
    // and nothing else for a datablock that serves no twin). The epoch it moves is
    // what makes every scene's decode draws re-sync.
    if (HlmsAtom *atom = registeredAtom())
        for (const Ogre::HlmsDatablock *db : dbs) atom->forgetDecodeTwinIfMoved(db);
    mPbsDrainDatablocks += dbs.size();
    for (const ScenePbs::HashNote &n : notes) {
        if (!n.owner) continue;
        for (auto &s : mScenes)
            if (s) s->markItemRehashed(n.owner);
    }
    pbs->clearHashNotes();
}

OgreScene::AtomRoute OgreScene::atomRouteFor(const Node &n, Ogre::uint32 flags) const {
    Ogre::Item *item = n.item;
    if (!item || !item->getNumSubItems()) return AtomRoute::Stock;
    const Ogre::uint8 rq = item->getRenderQueueGroup();
    // (a BLENDED item has its own queue since REFLECT-FIX-1, kBlendedRenderQueue: it goes on
    // to its own reason below, Blended, as it did from the opaque queue)
    if (rq != kOpaqueItemQueue && rq != kAtomRenderQueue && rq != kRefractiveRenderQueue &&
        rq != kBlendedRenderQueue)
        return AtomRoute::Stock;
    // THE ID PASS DRAWS THE WORLD CHANNELS ONLY (its cull asks for kGpuVisible): a
    // backdrop (the editor's 4 km Ground plane quad) or a helper draws in the views that
    // show it through PBS, or the split would skip it where nothing draws it.
    if (!(flags & kGpuVisible)) return AtomRoute::NotWorld;
    const Ogre::HlmsDatablock *db = item->getSubItem(0)->getDatablock();
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS)
        return AtomRoute::NotPbs;
    for (size_t s = 0; s < Ogre::CustomPieceStage::NumCustomPieceStages; ++s)
        if (db->getCustomPieceFileIdHash(Ogre::CustomPieceStage::CustomPieceStage(s)))
            return AtomRoute::CustomPiece;
    // A LIVE MATERIAL (TORNADO-1): one that reads the shader clock or scrolls its
    // maps (our `jah_shader_clock` / `jah_uv_scroll`, applyClockProperty). The
    // decode samples the maps through its own shader, which applies no scroll,
    // so it would draw the map frozen — it stays on the stock shader, counted
    // with the pieces.
    // (The two names hashed once, not per slot per walk: this runs in the dirty scan's
    // backstop for every item of a frame that moved anything.)
    static const Ogre::IdString kShaderClock("jah_shader_clock"), kUvScroll("jah_uv_scroll");
    for (const Ogre::HlmsDatablock::CustomProperty &cp : db->getCustomProperties())
        if (cp.keyName == kShaderClock || cp.keyName == kUvScroll)
            return AtomRoute::CustomPiece;
    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    if (rq == kRefractiveRenderQueue || db->getBlendblock()->isAutoTransparent() ||
        pbs->getTransparencyMode() == Ogre::HlmsPbsDatablock::Refractive)
        return AtomRoute::Blended;
    // A CUT-OUT GOES THROUGH THE MASKED RANGES (ENGINE-POLISH-ATOM-1 stage 3, shipped by ATOM-3 stage 2 with the
    // mask's coverage-preserving pyramid): the id pass and the caster cut discard at its mask (the engine's store,
    // AlphaMasks.h; the scene's table names it), so it is an ordinary Atom item once its mask is MADE — while it is
    // being made the stock shader draws it (Pending), and a texture that can never have one keeps it on the stock
    // route (AlphaTested). A SKINNED cut-out goes on to the Skinned reason below: the stock route, with Ogre's own
    // mips (the stage's stated residual; its mask still serves the rays).
    if (flags & kGpuAlphaTested) {
        switch (cutoutMaskState(db)) {
        case AlphaMaskStore::State::Refused: return AtomRoute::AlphaTested;
        case AlphaMaskStore::State::Pending: return AtomRoute::Pending;
        case AlphaMaskStore::State::Ready: break;
        }
    }
    // THE FACES, PER PASS (ATOM-TWO-SIDED-1; ENGINE-POLISH-ATOM-1 stage 3): the id pass and the caster cut each draw
    // back-culled (Ogre's default macroblock) or BOTH faces (CULL_NONE: the item's command rides the list's
    // two-sided range, drawn by the no-cull pipelines) — the colour macroblock decides the id pass's
    // (kGpuTwoSided), the caster's the caster cut's (kGpuCasterTwoSided: a cut-out's caster culls nothing by law,
    // BISTRO-FIXES-3, while its colour pass keeps its cull). A FRONT-culled colour pass or caster (only back faces
    // drawn: an inverted hull) stays on PBS.
    {
        const Ogre::CullingMode cull = db->getMacroblock()->mCullMode, casterCull = db->getMacroblock(true)->mCullMode;
        if ((cull != Ogre::CULL_CLOCKWISE && cull != Ogre::CULL_NONE) ||
            (casterCull != Ogre::CULL_CLOCKWISE && casterCull != Ogre::CULL_NONE))
            return AtomRoute::CullFront;
    }
    // A PLANAR MIRROR: PBS matches the RENDERABLE to its actor at hash time and binds
    // that actor's reflection per draw (HlmsPbs::calculateHashForPreCreate / fillBuffersFor);
    // a decode twin serves a bucket, not a renderable, so the mirror stays on PBS.
    if (mPlanar && mPlanar->hasPlanarReflections(item->getSubItem(0))) return AtomRoute::Planar;
    // ITS TEXTURES STILL BAKING: no bucket yet (HlmsAtom::isBucketPending) — PBS
    // draws it for those frames. The bake re-hashes every renderable wearing the
    // datablock (updateDescriptorSets -> flushRenderables), which the PBS change log
    // notes: the next drain re-composes the item and re-routes it.
    if (HlmsAtom::isBucketPending(db)) return AtomRoute::Pending;
    if (flags & kGpuSkinned) return AtomRoute::Skinned;
    if (!atomRowReadable(n, db, true)) return AtomRoute::NoRow;
    return AtomRoute::Atom;
}

AlphaMaskStore::State OgreScene::cutoutMaskState(const Ogre::HlmsDatablock *db) const {
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS ||
        db->getAlphaTest() == Ogre::CMPF_ALWAYS_PASS)
        return AlphaMaskStore::State::Ready;   // its own surface is solid (another submesh's test: noRow anyway)
    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    return alphaMaskStore().stateOf(pbs->getTexture(Ogre::PBSM_DIFFUSE), uint32_t(db->getAlphaTest()),
                                    db->getAlphaTestThreshold());
}

bool OgreScene::atomRowReadable(const Node &n, const Ogre::HlmsDatablock *db, bool decode) const {
    const Ogre::Item *item = n.item;
    // THE ROW IS SUBMESH 0's OF A TRIANGLE LIST: every mesh this engine builds has
    // one submesh (buildMeshV2 is the one createSubMesh a scene mesh meets), and a
    // line mesh has no index buffer and so no row — both are `noRow`.
    if (!item || item->getNumSubItems() != 1u) return false;
    {
        const Ogre::VertexArrayObjectArray &vaos = item->getSubItem(0)->getSubMesh()->mVao[Ogre::VpNormal];
        if (vaos.empty() || !vaos[0] || vaos[0]->getOperationType() != Ogre::OT_TRIANGLE_LIST) return false;
    }
    if (n.gpuMeshSlot == GpuScene::kNoMesh || !mGpuScene.live() ||
        n.gpuMeshSlot >= mGpuScene.levelMirrorEntries() / GpuScene::kLevelsPerMesh)
        return false;
    if (decode && HlmsAtom::materialWordOf(db) == HlmsAtom::kNoMaterialWord) return false;
    // THE ROW MUST BE ONE THE DECODE READS: device addresses for both buffers and a
    // float3 normal (800.Atom_piece_ps.any refuses any other layout, and a refused
    // pixel would be a hole where the id pass drew). The caster cut reads the same row.
    const uint32_t row = mGpuScene.levelAt(n.gpuMeshSlot, 0u).geomRow;
    const uint32_t *r = row == GpuScene::kNoGeomRow ? nullptr : mGpuScene.geomRowAt(row);
    return r && (r[0] | r[1]) && (r[2] | r[3]) && (r[8] & 0x70u) == 0x10u;
}

bool OgreScene::atomCasterThroughCut(const Node &n, Ogre::uint32 flags, AtomRoute route) const {
    if (route == AtomRoute::Atom) return true;
    // A BLENDED ITEM'S CASTER (ENGINE-POLISH-ATOM-1 stage 2): an id buffer has no slot for order-dependent colour,
    // so its main pass stays PBS — but its stock caster is the OPAQUE silhouette (HlmsPbs::calculateHashForPreCaster
    // keeps no blending; the caster's blendblock writes depth), which the caster cut draws exactly. A blended material
    // that is ALSO alpha-tested casts through the cut's masked ranges once its mask is made (ATOM-3 stage 2). Not
    // skinned (the cut reads the bind pose), nor a caster that culls its FRONT faces (the cut's pipelines cull back
    // faces or none), nor a row the cut cannot read.
    if (route != AtomRoute::Blended) return false;
    if (flags & kGpuSkinned) return false;
    const Ogre::HlmsDatablock *db = n.item->getSubItem(0)->getDatablock();
    // ...a blended cut-out's stock caster discards at its mask: the cut's masked ranges do the same, once it is made
    // (stage 3).
    if ((flags & kGpuAlphaTested) && cutoutMaskState(db) != AlphaMaskStore::State::Ready) return false;
    const Ogre::CullingMode casterCull = db->getMacroblock(true)->mCullMode;
    if (casterCull != Ogre::CULL_CLOCKWISE && casterCull != Ogre::CULL_NONE) return false;
    return atomRowReadable(n, db, false);
}

void OgreScene::placeAtomCaster(const Node &n, bool casterInCut) const {
    Ogre::Item *item = n.item;
    if (!item) return;
    // The pair follows the world channels the item carries now (stockCasterBits): none for a helper, a backdrop or
    // a distortion item, which no shadow pass draws either way.
    const Ogre::uint32 cur = item->getVisibilityFlags();
    const Ogre::uint32 want = (cur & ~kStockCasterBits) | (casterInCut ? 0u : stockCasterBits(cur));
    if (want != cur) item->setVisibilityFlags(want);
}

unsigned long long itemLevel0Triangles(const Ogre::Item &item) {
    unsigned long long tris = 0ull;
    const Ogre::MeshPtr &mesh = item.getMesh();
    for (size_t sm = 0; mesh && sm < mesh->getNumSubMeshes(); ++sm) {
        const Ogre::SubMesh *sub = mesh->getSubMesh(sm);
        if (sub->mVao[Ogre::VpNormal].empty() || !sub->mVao[Ogre::VpNormal][0]) continue;
        const Ogre::VertexArrayObject *vao = sub->mVao[Ogre::VpNormal][0];
        // THE VAO'S OWN RANGE (a level's indices, never its whole buffer), and triangles only: a line has none.
        if (vao->getOperationType() == Ogre::OT_TRIANGLE_LIST) tris += vao->getPrimitiveCount() / 3u;
    }
    return tris;
}

const Ogre::HlmsDatablock *OgreScene::atomSlotDatablock(uint32_t slot) const {
    const Node *nd = slot < mItemNodes.size() ? mItemNodes[slot] : nullptr;
    if (!nd || !nd->item || !nd->item->getNumSubItems()) return nullptr;
    return nd->item->getSubItem(0)->getDatablock();
}

AtomDrawStatus OgreScene::atomDrawStatus() {
    // U3 stage 4a: a read on the host's side is a REQUEST, answered on the engine's side.
    if (hostPosts()) { AtomDrawStatus r; requestOnEngine([&] { r = atomDrawStatus(); }); return r; }
    JAH_HOST_CALL("Scene::atomDrawStatus");
    AtomDrawStatus st;
    st.live = mGpuScene.live();
    // THE COMPONENT'S SHARE: the words, the decode draws and twins, the PBS views, the
    // caster cut's counters.
    mAtom.status(st);
    if (mEngine) {
        st.pbsNotes = mEngine->mPbsDrainNotes;
        st.pbsDatablocks = mEngine->mPbsDrainDatablocks;
    }
    Ogre::HlmsManager *hm = mRoot ? mRoot->getHlmsManager() : nullptr;
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    std::unordered_set<uint32_t> words;
    std::unordered_set<HlmsAtom::BucketKey, HlmsAtom::BucketKeyHash> buckets;
    // THE CAMERA VIEW'S ROUTES (ENGINE-POLISH-ATOM-1 stage 1): the first enabled view's camera; an item whose
    // cached world box (the last frame's: this is a read between frames) reaches its frustum is counted under its
    // route with its level-0 triangles — the main pass's share per route, beside the maps' (shadowCasterRoutes).
    const Ogre::Camera *viewCam = nullptr;
    for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>())
        if (v && v->camera()) { viewCam = v->camera(); break; }
    st.viewRoutesValid = viewCam != nullptr;
    for (Node *n : mItemNodes) {
        if (!n || !n->item) continue;
        if (!n->shown) continue;
        const Ogre::uint32 flags = gpuFlagsFor(*n);
        const AtomRoute r = atomRouteFor(*n, flags);
        if (viewCam) {
            const Ogre::Aabb box = n->item->getWorldAabb();
            if (viewCam->isVisible(Ogre::AxisAlignedBox(box.getMinimum(), box.getMaximum()))) {
                const unsigned ri = std::min(unsigned(r), ShadowRouteShare::kAtomCaster - 1u);
                ++st.viewRouteItems[ri];
                st.viewRouteTriangles[ri] += itemLevel0Triangles(*n->item);
            }
        }
        switch (r) {
        case AtomRoute::Stock: ++st.stockItems; continue;
        case AtomRoute::NotWorld: ++st.notWorld; break;
        case AtomRoute::NotPbs: ++st.notPbs; break;
        case AtomRoute::CustomPiece: ++st.customPiece; break;
        case AtomRoute::Blended: ++st.blended; break;
        case AtomRoute::CullFront: ++st.cullFront; break;
        case AtomRoute::Planar: ++st.planar; break;
        case AtomRoute::AlphaTested: ++st.alphaTested; break;
        case AtomRoute::Skinned: ++st.skinned; break;
        case AtomRoute::NoRow: ++st.noRow; break;
        case AtomRoute::Pending: ++st.pending; break;
        case AtomRoute::Atom: break;
        }
        if (r != AtomRoute::Atom) {
            ++st.pbsItems;
            if (flags & kGpuAtomCaster) ++st.atomCaster;   // ...whose caster the cut draws (stage 2)
            continue;
        }
        ++st.atomItems;
        if (flags & kGpuTwoSided) ++st.atomTwoSided;
        if (flags & kGpuAlphaTested) ++st.atomMasked;   // a cut-out through the masked ranges (stage 3)
        const auto *db =
            static_cast<const Ogre::HlmsPbsDatablock *>(n->item->getSubItem(0)->getDatablock());
        if (!words.insert(HlmsAtom::materialWordOf(db)).second) continue;
        HlmsAtom::BucketKey key;
        std::string err;
        if (atom && atom->bucketKeyOf(db, key, err)) buckets.insert(key);
    }
    st.materials = unsigned(words.size());
    st.buckets = unsigned(buckets.size());
    st.on = atomDrawOn();
    st.viewPaintable = atomViewPaintable();
    if (mRoot && mRoot->getRenderSystem() && mRoot->getRenderSystem()->getVaoManager())
        st.frame = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
    // THE CUT'S COUNTERS, the first enabled view of this scene that has read any.
    for (OgreView *v : mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>()) {
        AtomCutStats cs;
        unsigned long long tris = 0ull;
        unsigned surv = 0u;
        if (!v || !v->atomStats(tris, surv) || !v->atomCutStats(cs)) continue;
        st.cutValid = true;
        st.cutClusters = cs.clusters;
        st.cutIndices = cs.indices;
        st.cutEvaluated = cs.evaluated;
        st.cutOverflow = cs.overflow;
        st.cutMissing = cs.missing;
        st.cutOverflowIndices = cs.overflowIndices;
        st.cutIndexBudget = cs.indexBudget;
        st.cutTriangles = tris;
        st.occlusion = v->atomOcclusionStats(st.occluded, st.disoccluded);
        break;
    }
    // THE DENSITY (ATOM-2): while the scene's view is Density, the first view's last frame, read back.
    atomDensityStatus(this, mEngine ? mEngine->viewsOf(this) : std::vector<OgreView *>(), st);
    return st;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
