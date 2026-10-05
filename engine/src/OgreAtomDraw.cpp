// ATOM S3-DRAW — the visibility buffer as the product's opaque path
// (SPECS/atom/D3_S3_DRAW_DESIGN.md; SPECS/briefs/ATOM-S3-DRAW.md).
//
// THIS FILE HOLDS THE SPLIT'S ONE DECISION (`atomRouteFor`: which items the id
// pass draws and the decode shades, and why the rest stay on stock HlmsPbs) and
// the scene's reading of it (`atomDrawStatus`: the split's counts and the decode
// BUCKETS the atom items' materials need — HlmsAtom::BucketKey).
#include "EnginePrivate.h"
#include "GpuScene.h"
#include "HlmsAtom.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>
#include <Compositor/Pass/OgreCompositorPass.h>
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
#include <OgreMesh2.h>
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

/// Which view a workspace belongs to — how the id pass's recorder (a provider
/// callback that is handed a pass) finds its view.
std::unordered_map<const Ogre::CompositorWorkspace *, OgreView *> &atomViews() {
    static std::unordered_map<const Ogre::CompositorWorkspace *, OgreView *> sViews;
    return sViews;
}

HlmsAtom *registeredAtom() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    return hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
}
}  // namespace

void atomRegisterView(const Ogre::CompositorWorkspace *ws, OgreView *view) {
    if (ws && view) atomViews()[ws] = view;
}
void atomUnregisterView(const Ogre::CompositorWorkspace *ws) {
    if (ws) atomViews().erase(ws);
}
OgreView *atomViewOf(const Ogre::CompositorWorkspace *ws) {
    auto it = atomViews().find(ws);
    return it == atomViews().end() ? nullptr : it->second;
}

namespace {
std::unordered_map<const Ogre::SceneManager *, OgreScene *> &atomScenes() {
    static std::unordered_map<const Ogre::SceneManager *, OgreScene *> sScenes;
    return sScenes;
}
}  // namespace
void atomRegisterScene(const Ogre::SceneManager *sm, OgreScene *scene) {
    if (sm && scene) atomScenes()[sm] = scene;
}
void atomUnregisterScene(const Ogre::SceneManager *sm) {
    if (sm) atomScenes().erase(sm);
}
OgreScene *atomSceneOf(const Ogre::SceneManager *sm) {
    auto it = atomScenes().find(sm);
    return it == atomScenes().end() ? nullptr : it->second;
}

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
        const AtomView view = scene ? scene->atomView() : AtomView::Off;
        Ogre::TextureGpu *table = scene ? scene->atomViewTable() : nullptr;
        Ogre::Pass *pass = (view != AtomView::Off && table) ? atomViewMaterialPass() : nullptr;
        const bool on = pass && pass->hasFragmentProgram() && pass->getNumTextureUnitStates() >= 4u;
        const Ogre::uint8 mask = ws->getExecutionMask();
        ws->setExecutionMask(on ? Ogre::uint8(mask | kAtomViewExecutionBit)
                                : Ogre::uint8(mask & ~kAtomViewExecutionBit));
        if (!on) return;
        Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
        ps->setIgnoreMissingParams(true);
        ps->setNamedConstant("atomViewParams", Ogre::Vector4(Ogre::Real(int(view)), 0, 0, 0));
        pass->getTextureUnitState(3)->setTexture(table);
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
            static bool said = false;
            if (!said) {
                said = true;
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

private:
    OgreView *mView = nullptr;
    Ogre::SceneManager *mArmed = nullptr;
    Ogre::CompositorPass *mArmedPass = nullptr;
};
}  // namespace

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

bool OgreScene::atomDrawOn() const { return mGpuScene.live() && atomDrawWanted(); }

bool OgreScene::atomDrawWanted() const {
    return mAtomDrawEnabled && mRoot && atomIdPassSupported(mRoot->getRenderSystem());
}

void OgreScene::setAtomDrawEnabled(bool on) {
    if (on == mAtomDrawEnabled) return;
    mAtomDrawEnabled = on;
    // Every slot re-composes (its route and its queue follow), and every view of
    // this scene re-derives its chain (the id pass comes or goes) on its next
    // description push — the engine's per-frame view sync compares shapes.
    for (Node *n : mItemNodes)
        if (n) markGpuSlotDirty(*n);
}

void OgreScene::setAtomOcclusionEnabled(bool on) {
    // A SHAPE CHANGE and nothing else: every view of this scene re-derives its chain
    // (the pyramid and the late pass come or go) on its next sync, as for the split.
    mAtomOcclusionEnabled = on;
}

void OgreScene::setAtomCutBudgetForTest(unsigned indices) {
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

void OgreScene::updateAtomDraw() {
    updateAtomSplit();
    // THE ATOM VIEW'S TABLE follows whatever the split just decided.
    syncAtomViewTable();
}

void OgreScene::updateAtomSplit() {
    HlmsAtom *atom = registeredAtom();
    if (!atom || !mGpuScene.live()) return;
    // THE SCREEN DECODE'S DRAWS: one per bucket of the words the atom items wear,
    // re-derived when that SET moved (the change feed, AtomWordFeed) or a twin died.
    // An in-place material edit is not polled for here any more: the PBS change log
    // (ScenePbs::HashNote) hears it at Ogre's own re-hash, and the frame's drain
    // (OgreEngine::drainPbsChanges) forgets the twin whose bucket moved and
    // re-composes the Items it re-hashed — before this scene's scan, so the route,
    // the queue and the word follow in the same frame. A still frame does nothing.
    if (!mAtomFeed.words.changed() && atom->twinEpoch() == mAtomSyncEpoch) return;
    std::vector<uint32_t> words = mAtomFeed.words.take();
    if (words == mAtomWords && atom->twinEpoch() == mAtomSyncEpoch) return;
    mAtomWords.swap(words);
    atom->syncScreenDecodes(mSceneMgr, mAtomWords);
    mAtomSyncEpoch = atom->twinEpoch();
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
    if (rq != kOpaqueItemQueue && rq != kAtomRenderQueue && rq != kRefractiveRenderQueue)
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
    for (const Ogre::HlmsDatablock::CustomProperty &cp : db->getCustomProperties())
        if (cp.keyName == Ogre::IdString("jah_shader_clock") || cp.keyName == Ogre::IdString("jah_uv_scroll"))
            return AtomRoute::CustomPiece;
    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    if (rq == kRefractiveRenderQueue || db->getBlendblock()->isAutoTransparent() ||
        pbs->getTransparencyMode() == Ogre::HlmsPbsDatablock::Refractive)
        return AtomRoute::Blended;
    // THE FACES (ATOM-TWO-SIDED-1): the id pass and the caster cut draw back-culled
    // (Ogre's default macroblock) or BOTH faces (CULL_NONE: the item's command rides
    // the list's two-sided range, kGpuTwoSided, drawn by the no-cull pipelines) — one
    // flag serves both passes, so the caster macroblock must cull as the material
    // does. A FRONT-culled material (only back faces drawn: an inverted hull) or a
    // caster set apart from its material stays on PBS.
    {
        const Ogre::CullingMode cull = db->getMacroblock()->mCullMode;
        if ((cull != Ogre::CULL_CLOCKWISE && cull != Ogre::CULL_NONE) ||
            db->getMacroblock(true)->mCullMode != cull)
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
    if (flags & kGpuAlphaTested) return AtomRoute::AlphaTested;
    if (flags & kGpuSkinned) return AtomRoute::Skinned;
    // THE ROW IS SUBMESH 0's OF A TRIANGLE LIST: every mesh this engine builds has
    // one submesh (buildMeshV2 is the one createSubMesh a scene mesh meets), and a
    // line mesh has no index buffer and so no row — both are `noRow`.
    if (item->getNumSubItems() > 1u) return AtomRoute::NoRow;
    {
        const Ogre::VertexArrayObjectArray &vaos = item->getSubItem(0)->getSubMesh()->mVao[Ogre::VpNormal];
        if (vaos.empty() || !vaos[0] || vaos[0]->getOperationType() != Ogre::OT_TRIANGLE_LIST)
            return AtomRoute::NoRow;
    }
    if (n.gpuMeshSlot == GpuScene::kNoMesh || !mGpuScene.live() ||
        n.gpuMeshSlot >= mGpuScene.levelMirrorEntries() / GpuScene::kLevelsPerMesh ||
        HlmsAtom::materialWordOf(db) == HlmsAtom::kNoMaterialWord)
        return AtomRoute::NoRow;
    // THE ROW MUST BE ONE THE DECODE READS: device addresses for both buffers and a
    // float3 normal (800.Atom_piece_ps.any refuses any other layout, and a refused
    // pixel would be a hole where the id pass drew).
    const uint32_t row = mGpuScene.levelAt(n.gpuMeshSlot, 0u).geomRow;
    const uint32_t *r = row == GpuScene::kNoGeomRow ? nullptr : mGpuScene.geomRowAt(row);
    if (!r || !(r[0] | r[1]) || !(r[2] | r[3]) || (r[8] & 0x70u) != 0x10u) return AtomRoute::NoRow;
    return AtomRoute::Atom;
}

// ---------------------------------------------------------------------------
// THE ATOM VIEW'S BUCKETS TABLE (D0-ATOM-VIEW). The bucket is not in the id image
// (a draw of the decode knows its own, a pixel does not), and a compositor quad
// binds textures, not the GPU scene's buffers — so the view reads one R32_UINT
// texel per GPU scene slot: the bucket (HlmsAtom::bucketIdOf) of the datablock the
// slot's atom item wears, 0 for anything else. The texture exists while the view
// is on (the quad binds it in every mode) and is freed when it goes Off; its
// CONTENTS are walked only while the view is Buckets and only when the slot set or
// a bucket's membership moved, and uploaded only when a value did.
// ---------------------------------------------------------------------------
namespace {
constexpr uint32_t kAtomViewTableWidth = 1024u;
}

void OgreScene::releaseAtomViewTable() {
    if (mAtomViewTable && mRoot && mRoot->getRenderSystem())
        mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(mAtomViewTable);
    mAtomViewTable = nullptr;
    mAtomViewRows.clear();
}

void OgreScene::syncAtomViewTable() {
    // OFF FREES IT (the next view that is not Off creates it again).
    if (mAtomView == AtomView::Off) {
        if (mAtomViewTable) releaseAtomViewTable();
        mAtomViewWrites = mAtomViewBucketGen = ~0ull;
        return;
    }
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (!rs) return;
    const uint32_t slots = mGpuScene.live() ? uint32_t(mGpuScene.slotCount()) : 0u;
    const uint32_t rows = std::max(1u, (slots + kAtomViewTableWidth - 1u) / kAtomViewTableWidth);
    HlmsAtom *atom = registeredAtom();
    const unsigned long long bucketGen = atom ? atom->bucketGeneration() : 0ull;
    const bool buckets = mAtomView == AtomView::Buckets;
    // CHANGE-DRIVEN: the walk runs only in Buckets mode and only when the slot set
    // (the GPU scene's writes) or a bucket's membership (HlmsAtom's generation)
    // moved since the last one; any other mode reads no contents at all.
    if (mAtomViewTable && mAtomViewTable->getHeight() == rows &&
        (!buckets || (mAtomViewWrites == mGpuScene.writes() && mAtomViewBucketGen == bucketGen)))
        return;
    std::vector<uint32_t> table(size_t(rows) * kAtomViewTableWidth, 0u);
    if (buckets && atom) {
        const GpuInstance *m = mGpuScene.mirrorData();
        for (uint32_t i = 0; i < slots && i < mItemNodes.size(); ++i) {
            uint32_t flags = 0u;
            std::memcpy(&flags, &m[i].boundsMax[3], sizeof(flags));
            const Node *nd = mItemNodes[i];
            if (!(flags & kGpuAtom) || !nd || !nd->item || !nd->item->getNumSubItems()) continue;
            table[i] = atom->bucketIdOf(nd->item->getSubItem(0)->getDatablock());
        }
    }
    if (buckets) {
        mAtomViewWrites = mGpuScene.writes();
        mAtomViewBucketGen = bucketGen;
    } else {
        mAtomViewWrites = mAtomViewBucketGen = ~0ull;   // entering Buckets walks
    }
    if (mAtomViewTable && mAtomViewTable->getHeight() == rows && table == mAtomViewRows) return;
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    if (mAtomViewTable && mAtomViewTable->getHeight() != rows) {
        tm->destroyTexture(mAtomViewTable);
        mAtomViewTable = nullptr;
    }
    if (!mAtomViewTable) {
        // A ManualTexture goes Resident by an immediate transition and is never
        // notifyDataIsReady'd (DOCS/traps/ENGINE.md).
        mAtomViewTable = tm->createTexture(
            "jahAtomViewTable/" + std::to_string(reinterpret_cast<uintptr_t>(this)),
            Ogre::GpuPageOutStrategy::Discard, Ogre::TextureFlags::ManualTexture,
            Ogre::TextureTypes::Type2D);
        mAtomViewTable->setResolution(kAtomViewTableWidth, rows);
        mAtomViewTable->setPixelFormat(Ogre::PFG_R32_UINT);
        mAtomViewTable->setNumMipmaps(1u);
        mAtomViewTable->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    }
    Ogre::StagingTexture *st = tm->getStagingTexture(kAtomViewTableWidth, rows, 1u, 1u, Ogre::PFG_R32_UINT);
    st->startMapRegion();
    Ogre::TextureBox box = st->mapRegion(kAtomViewTableWidth, rows, 1u, 1u, Ogre::PFG_R32_UINT);
    for (uint32_t y = 0; y < rows; ++y)
        std::memcpy(box.at(0, y, 0), &table[size_t(y) * kAtomViewTableWidth],
                    kAtomViewTableWidth * sizeof(uint32_t));
    st->stopMapRegion();
    st->upload(box, mAtomViewTable, 0, nullptr, nullptr, true);
    tm->removeStagingTexture(st);
    mAtomViewRows.swap(table);
}

AtomDrawStatus OgreScene::atomDrawStatus() {
    AtomDrawStatus st;
    st.live = mGpuScene.live();
    st.wordSlotVisits = mAtomFeed.words.visits();
    if (mEngine) {
        st.pbsNotes = mEngine->mPbsDrainNotes;
        st.pbsDatablocks = mEngine->mPbsDrainDatablocks;
    }
    Ogre::HlmsManager *hm = mRoot ? mRoot->getHlmsManager() : nullptr;
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    std::unordered_set<uint32_t> words;
    std::unordered_set<HlmsAtom::BucketKey, HlmsAtom::BucketKeyHash> buckets;
    for (Node *n : mItemNodes) {
        if (!n || !n->item) continue;
        if (!n->shown) continue;
        const Ogre::uint32 flags = gpuFlagsFor(*n);
        const AtomRoute r = atomRouteFor(*n, flags);
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
            continue;
        }
        ++st.atomItems;
        if (flags & kGpuTwoSided) ++st.atomTwoSided;
        const auto *db =
            static_cast<const Ogre::HlmsPbsDatablock *>(n->item->getSubItem(0)->getDatablock());
        if (!words.insert(HlmsAtom::materialWordOf(db)).second) continue;
        HlmsAtom::BucketKey key;
        std::string err;
        if (atom && atom->bucketKeyOf(db, key, err)) buckets.insert(key);
    }
    st.materials = unsigned(words.size());
    st.buckets = unsigned(buckets.size());
    st.twins = atom ? unsigned(atom->decodeTwinCount()) : 0u;
    st.decodeDraws = atom ? unsigned(atom->sceneDecodeCount(mSceneMgr)) : 0u;
    st.screenDraws = atom ? unsigned(atom->screenDecodeCount(mSceneMgr)) : 0u;
    st.on = atomDrawOn();
    st.stereoViews = unsigned(mAtomStereoViews.size());
    st.passthroughViews = unsigned(mAtomPassthroughViews.size());
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
    // THE CASTER CUT'S COUNTERS (ATOM-SHADOWS-1): the scene's one caster list.
    st.casterValid = mCasterStats.valid;
    st.casterMaps = mCasterStats.maps;
    st.casterClusters = mCasterStats.clusters;
    st.casterInstances = mCasterStats.instances;
    st.casterOverflow = mCasterStats.overflow;
    st.casterMissing = mCasterStats.missing;
    st.casterIndexBudget = mCasterStats.indexBudget;
    st.casterTriangles = mCasterStats.triangles;
    st.casterMapsPeak = mCasterStats.peakMaps;
    st.casterUnrecorded = mCasterStats.unrecorded;
    return st;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
