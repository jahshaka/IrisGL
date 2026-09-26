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

// ---------------------------------------------------------------------------
// THE SCREEN DECODE'S ARMING (ATOM S3-DRAW). A view's pass that SKIPS the Atom
// queue is a pass the id pass stands in for — the prepass and the opaque pass of
// a chain with ChainDesc::atomDraw — so it is the pass whose first draws are the
// screen decode: for its length HlmsAtom is handed the view's id image and the
// GPU scene's tables, and the scene's screen decode draws are shown.
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
        if (!def || !def->skipsRenderQueue(kAtomRenderQueue)) return;
        // A MEASUREMENT SWITCH, never a mode (JAHSHAKA_HIT_LIST_OFF's kind): the
        // decode left unarmed, so the Atom items are drawn by NOTHING in this pass —
        // engine.atom_draw's proof that the view's passes really skip their queue.
        if (std::getenv("JAHSHAKA_ATOM_DECODE_OFF")) return;
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
    const bool atomDraw = chainDesc().atomDraw;
    if (mScene) {
        const bool on = mScene->atomDrawWanted();
        mScene->noteAtomPbsView(this, on && mStereo, on && !mStereo && !atomDraw);
    }
    // THE SHAPE: the scene's split decides it, and a view learns its scene late
    // (and its target's sample count can change under it).
    if (mChainAtomDraw != atomDraw) rebuildWorkspaceDef();
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
    // THE ATOM VIEW'S TABLE follows whatever this call decides (every return below).
    struct ViewTableAtExit {
        OgreScene *s;
        ~ViewTableAtExit() {
            try { s->syncAtomViewTable(); } catch (Ogre::Exception &) {}
        }
    } viewTableAtExit{ this };
    HlmsAtom *atom = registeredAtom();
    if (!atom || !mGpuScene.live()) return;
    // THE WITNESS: a material edited IN PLACE (a blend, an alpha test, a texture)
    // may leave or change its bucket without any seam marking a slot: one item per
    // atom word says so, and every item wearing the word re-composes (its route and
    // its queue follow) — the ray tier's own rule for its twins.
    bool moved = false;
    for (AtomWitness &w : mAtomWitness) {
        if (w.slot >= mItemNodes.size()) continue;
        const Node *nd = mItemNodes[w.slot];
        if (!nd || !nd->item || !nd->item->getNumSubItems()) continue;
        const Ogre::SubItem *sub = nd->item->getSubItem(0);
        const Ogre::HlmsDatablock *db = sub->getDatablock();
        const Ogre::uint32 h = sub->getHlmsHash();
        const uint64_t tk = HlmsAtom::textureSetKeyOf(db);
        if (db == w.db && (h != w.hash || tk != w.texKey)) {
            // Its twin only if its bucket moved; its items ALWAYS re-compose (a
            // blend or cull edit keeps the key but moves the route).
            atom->forgetDecodeTwinIfMoved(db);
            for (Node *n : mItemNodes)
                if (n && n->item && n->item->getNumSubItems() &&
                    n->item->getSubItem(0)->getDatablock() == db)
                    markGpuSlotDirty(*n);
            moved = true;
        }
        w.db = db;
        w.hash = h;
        w.texKey = tk;
    }
    // THE ITEMS WAITING ON THEIR TEXTURES: re-composed (and so re-routed) once
    // their datablock's descriptor sets are baked. A walk only while something waits.
    if (mAtomPendingSeen) {
        mAtomPendingSeen = false;
        for (Node *n : mItemNodes) {
            if (!n || !n->item || !n->item->getNumSubItems()) continue;
            const Ogre::HlmsDatablock *db = n->item->getSubItem(0)->getDatablock();
            if (HlmsAtom::isBucketPending(db)) {
                mAtomPendingSeen = true;
                continue;
            }
            uint32_t flags = 0u;
            if (n->itemSlot < mGpuScene.slotCount())
                std::memcpy(&flags, &mGpuScene.entry(uint32_t(n->itemSlot)).boundsMax[3], sizeof(flags));
            if (!(flags & kGpuAtom) && n->item->getRenderQueueGroup() == kOpaqueItemQueue) {
                markGpuSlotDirty(*n);
                moved = true;
            }
        }
    }
    if (moved) ensureGpuScene(/*graphIsCurrent=*/true);
    // THE SCREEN DECODE'S DRAWS: one per bucket of the words the atom items wear,
    // re-derived when the table's writes or the twins moved.
    if (mGpuScene.writes() == mAtomSyncWrites && atom->twinEpoch() == mAtomSyncEpoch) return;
    mAtomSyncWrites = mGpuScene.writes();
    std::vector<uint32_t> words;
    std::vector<AtomWitness> witness;
    const GpuInstance *m = mGpuScene.mirrorData();
    for (uint32_t i = 0, e = mGpuScene.slotCount(); i < e; ++i) {
        uint32_t flags = 0u;
        std::memcpy(&flags, &m[i].boundsMax[3], sizeof(flags));
        if (!(flags & kGpuAtom) || m[i].raster[0] == HlmsAtom::kNoMaterialWord) continue;
        words.push_back(m[i].raster[0]);
    }
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    if (words == mAtomWords && atom->twinEpoch() == mAtomSyncEpoch) return;
    mAtomWords = words;
    atom->syncScreenDecodes(mSceneMgr, words);
    mAtomSyncEpoch = atom->twinEpoch();
    for (uint32_t i = 0, e = mGpuScene.slotCount(); i < e && i < mItemNodes.size(); ++i) {
        uint32_t flags = 0u;
        std::memcpy(&flags, &m[i].boundsMax[3], sizeof(flags));
        if (!(flags & kGpuAtom)) continue;
        const uint32_t word = m[i].raster[0];
        if (std::find_if(witness.begin(), witness.end(),
                         [word](const AtomWitness &w) { return w.word == word; }) != witness.end())
            continue;
        const Node *nd = mItemNodes[i];
        if (!nd || !nd->item || !nd->item->getNumSubItems()) continue;
        AtomWitness w;
        w.slot = i;
        w.word = word;
        w.db = nd->item->getSubItem(0)->getDatablock();
        w.hash = nd->item->getSubItem(0)->getHlmsHash();
        w.texKey = HlmsAtom::textureSetKeyOf(w.db);
        witness.push_back(w);
    }
    mAtomWitness.swap(witness);
}

OgreScene::AtomRoute OgreScene::atomRouteFor(const Node &n, Ogre::uint32 flags) const {
    Ogre::Item *item = n.item;
    if (!item || !item->getNumSubItems()) return AtomRoute::Stock;
    const Ogre::uint8 rq = item->getRenderQueueGroup();
    if (rq != kOpaqueItemQueue && rq != kAtomRenderQueue && rq != kRefractiveRenderQueue)
        return AtomRoute::Stock;
    // THE ID PASS DRAWS THE WORLD CHANNELS ONLY (its cull asks for kGpuVisible): a
    // backdrop (the ground's 4 km horizon quad) or a helper draws in the views that
    // show it through PBS, or the split would skip it where nothing draws it.
    if (!(flags & kGpuVisible)) return AtomRoute::NotWorld;
    const Ogre::HlmsDatablock *db = item->getSubItem(0)->getDatablock();
    if (!db || !db->getCreator() || db->getCreator()->getType() != Ogre::HLMS_PBS)
        return AtomRoute::NotPbs;
    for (size_t s = 0; s < Ogre::CustomPieceStage::NumCustomPieceStages; ++s)
        if (db->getCustomPieceFileIdHash(Ogre::CustomPieceStage::CustomPieceStage(s)))
            return AtomRoute::CustomPiece;
    const auto *pbs = static_cast<const Ogre::HlmsPbsDatablock *>(db);
    if (rq == kRefractiveRenderQueue || db->getBlendblock()->isAutoTransparent() ||
        pbs->getTransparencyMode() == Ogre::HlmsPbsDatablock::Refractive)
        return AtomRoute::Blended;
    // The id pass culls back faces (Ogre's default macroblock); a material drawn
    // with any other cull mode stays where its faces are drawn as authored.
    if (db->getMacroblock()->mCullMode != Ogre::CULL_CLOCKWISE) return AtomRoute::TwoSided;
    // A PLANAR MIRROR: PBS matches the RENDERABLE to its actor at hash time and binds
    // that actor's reflection per draw (HlmsPbs::calculateHashForPreCreate / fillBuffersFor);
    // a decode twin serves a bucket, not a renderable, so the mirror stays on PBS.
    if (mPlanar && mPlanar->hasPlanarReflections(item->getSubItem(0))) return AtomRoute::Planar;
    // ITS TEXTURES STILL BAKING: no bucket yet (HlmsAtom::isBucketPending) — PBS
    // draws it for those frames; updateAtomDraw re-routes it when they land.
    if (HlmsAtom::isBucketPending(db)) {
        mAtomPendingSeen = true;
        return AtomRoute::Pending;
    }
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
// is on (the quad binds it in every mode); its CONTENTS are walked and uploaded
// only while the view is Buckets, and only when a value moved.
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
    if (mAtomView == AtomView::Off) return;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (!rs) return;
    const uint32_t slots = mGpuScene.live() ? uint32_t(mGpuScene.slotCount()) : 0u;
    const uint32_t rows = std::max(1u, (slots + kAtomViewTableWidth - 1u) / kAtomViewTableWidth);
    std::vector<uint32_t> table(size_t(rows) * kAtomViewTableWidth, 0u);
    if (mAtomView == AtomView::Buckets) {
        if (HlmsAtom *atom = registeredAtom()) {
            const GpuInstance *m = mGpuScene.mirrorData();
            for (uint32_t i = 0; i < slots && i < mItemNodes.size(); ++i) {
                uint32_t flags = 0u;
                std::memcpy(&flags, &m[i].boundsMax[3], sizeof(flags));
                const Node *nd = mItemNodes[i];
                if (!(flags & kGpuAtom) || !nd || !nd->item || !nd->item->getNumSubItems()) continue;
                table[i] = atom->bucketIdOf(nd->item->getSubItem(0)->getDatablock());
            }
        }
    } else if (mAtomViewTable && mAtomViewTable->getHeight() == rows) {
        return;   // another mode never reads the contents
    }
    if (mAtomViewTable && mAtomViewTable->getHeight() == rows && table == mAtomViewRows) return;
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    if (mAtomViewTable && mAtomViewTable->getHeight() != rows) releaseAtomViewTable();
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
        case AtomRoute::TwoSided: ++st.twoSided; break;
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
    return st;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
