// THE SCENE'S GI GLUE (ENGINE-UPGRADE-2B stage 2). Photon's GI is PhotonComponent's
// (photon/PhotonComponent.h, its methods in photon/PhotonGi.cpp); what stays here is the
// SCENE's part of it: the per-pass GI binding (SceneGiBinding, ScenePbs, bindSceneGi), THE
// WALK (walkItems — one walk for the GI and the shadow scan — with its gate, ensureGiWalk,
// and the caster half, runItemWalk), the item and decal index, the change set's drain
// (applyChangeSet, noteSceneTransformWrite) and the seams the component calls on nodes
// (probeSeesItem, underGesture, demoteDragMover, the probe-grid exclusion flag, the
// binding's arms, the structure edit's close).
#include "EnginePrivate.h"
#include "HlmsAtom.h"       // bindSceneGi's declaration (every PBS-family host calls it)
#include "photon/voxel/PhotonPassBinding.h"
#include "SurfaceCache.h"   // SURFACE-CACHE phase 2: giStatus copies the Component's counters
#include "photon/voxel/PhotonVoxelMaterial.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>

#include <OgreHlmsManager.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsAsyncCompiler.h>
#include <OgreBitwise.h>
#include <OgrePixelFormatGpuUtils.h>

namespace jahshaka { namespace engine { namespace detail {

// THE GI BINDING OF A MANAGER'S SCENE (ENGINE-UPGRADE-2B stage 2): the scene bound on the manager
// (sceneOfManager) holds it — no process-wide registry, nothing to register or unregister; a
// manager no scene holds (the blank scene, the document scene) answers null.
const SceneGiBinding *sceneGiBindingOf(const Ogre::SceneManager *sm) {
    const OgreScene *scene = sm ? sceneOfManager(sm) : nullptr;
    return scene ? &scene->giBinding() : nullptr;
}

bool sceneLightDirt(const Ogre::SceneManager *sm, unsigned long long &serial, unsigned long long &removed) {
    const SceneGiBinding *b = sceneGiBindingOf(sm);
    if (!b || !b->lightWriteSerial || !b->lightsRemoved) return false;
    serial = *b->lightWriteSerial;
    removed = *b->lightsRemoved;
    return true;
}

void bindSceneGi(Ogre::HlmsPbs *host, const Ogre::SceneManager *sm) {
    static const SceneGiBinding kNone;
    const SceneGiBinding *b = sceneGiBindingOf(sm);
    if (!b) b = &kNone;
    // Compared first: a pass of the scene the previous pass drew (every pass of
    // one view, the shadow node's casters in between) costs a few compares. The
    // Photon volumes are not HlmsPbs's (OWN-GI-1): the listener resolves them from
    // the same record (PhotonPassBinding).
    if (host->getParallaxCorrectedCubemap() != b->pcc ||
        (b->pcc && (host->getPccVctMinDistance() != b->pccMinDist ||
                    host->getPccVctMaxDistance() != b->pccMaxDist)))
        host->setParallaxCorrectedCubemap(b->pcc, b->pccMinDist, b->pccMaxDist);
#ifdef OGRE_BUILD_COMPONENT_PLANAR_REFLECTIONS
    if (host->getPlanarReflections() != b->planar) host->setPlanarReflections(b->planar);
#endif
    // resetIblSpecMipmap(n > 0) also turns HlmsPbs's automatic grow-only mode off:
    // the count is the scene's, set here and nowhere else.
    if (host->getMaxSpecIblMipmap() != b->iblMipmaps)
        host->resetIblSpecMipmap(Ogre::uint8(b->iblMipmaps));
}

void forgetGiArms(Ogre::HlmsManager *manager, const Ogre::PhotonVoxelLighting *vct,
                  const Ogre::PhotonIrradianceField *ifd,
                  const Ogre::ParallaxCorrectedCubemapBase *pcc,
                  const Ogre::PlanarReflections *planar) {
    PhotonPassBinding::forget(vct);
    PhotonPassBinding::forget(ifd);
    if (!manager) return;
    for (int t = Ogre::HLMS_LOW_LEVEL + 1; t < Ogre::HLMS_MAX; ++t) {
        auto *host = dynamic_cast<Ogre::HlmsPbs *>(manager->getHlms(Ogre::HlmsTypes(t)));
        if (!host) continue;
        if (pcc && host->getParallaxCorrectedCubemap() == pcc) host->setParallaxCorrectedCubemap(nullptr);
#ifdef OGRE_BUILD_COMPONENT_PLANAR_REFLECTIONS
        if (planar && host->getPlanarReflections() == planar) host->setPlanarReflections(nullptr);
#else
        (void)planar;
#endif
    }
}

void ScenePbs::analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                               Ogre::ResourceTransitionArray &resourceTransitions,
                               Ogre::Camera *renderingCamera, const bool bCasterPass) {
    const Ogre::SceneManager *sm = renderingCamera ? renderingCamera->getSceneManager() : nullptr;
    bindSceneGi(this, sm);
    Ogre::HlmsPbs::analyzeBarriers(barrierSolver, resourceTransitions, renderingCamera, bCasterPass);
    // The Photon volumes the pixel stage reads (HlmsPbs's own walk did this while it
    // held them — OWN-GI-1).
    PhotonPassBinding::analyzeBarriers(barrierSolver, resourceTransitions, sm, bCasterPass);
}

Ogre::HlmsCache ScenePbs::preparePassHash(const Ogre::CompositorShadowNode *shadowNode,
                                          bool casterPass, bool dualParaboloid,
                                          Ogre::SceneManager *sceneManager) {
    bindSceneGi(this, sceneManager);
    return Ogre::HlmsPbs::preparePassHash(shadowNode, casterPass, dualParaboloid, sceneManager);
}

void ScenePbs::calculateHashFor(Ogre::Renderable *renderable, Ogre::uint32 &outHash,
                                Ogre::uint32 &outCasterHash) {
    Ogre::HlmsPbs::calculateHashFor(renderable, outHash, outCasterHash);
    HashNote note;
    note.db = renderable ? renderable->getDatablock() : nullptr;
    if (auto *sub = dynamic_cast<const Ogre::SubItem *>(renderable)) note.owner = sub->getParent();
    // One note per (owner, datablock) run: a flush re-hashes an Item's sub-items
    // back to back, and a caller that re-hashes the same renderable twice (hash,
    // then caster hash) adds nothing.
    if (!mHashNotes.empty() && mHashNotes.back().owner == note.owner && mHashNotes.back().db == note.db)
        return;
    if (mHashNotes.size() >= kMaxHashNotes) {
        mHashNotesOverflow = true;
        return;
    }
    mHashNotes.push_back(note);
}

void ScenePbs::calculateHashForPreCreate(Ogre::Renderable *renderable, Ogre::PiecesMap *inOutPieces) {
#ifdef OGRE_BUILD_COMPONENT_PLANAR_REFLECTIONS
    // THE RENDERABLE'S OWN SCENE: an Item's SubItem -> its Item -> its
    // SceneManager -> that scene's record. Anything else (a v1 renderable, an
    // overlay quad) belongs to no scene's mirrors: no planar pointer at all.
    const Ogre::SceneManager *sm = nullptr;
    if (auto *sub = dynamic_cast<const Ogre::SubItem *>(renderable))
        if (const Ogre::Item *item = sub->getParent()) sm = item->_getManager();
    const SceneGiBinding *b = sm ? sceneGiBindingOf(sm) : nullptr;
    Ogre::PlanarReflections *const passPlanar = getPlanarReflections();
    Ogre::PlanarReflections *const own = b ? b->planar : nullptr;
    if (own != passPlanar) setPlanarReflections(own);
    Ogre::HlmsPbs::calculateHashForPreCreate(renderable, inOutPieces);
    // ...and the pass's pointer back: a hash can be asked for between two
    // passes, and the next pass of the SAME scene must not re-derive its
    // barriers from somebody else's mirrors.
    if (own != passPlanar) setPlanarReflections(passPlanar);
#else
    Ogre::HlmsPbs::calculateHashForPreCreate(renderable, inOutPieces);
#endif
}

// THE PROBE FACE PASS'S RENDER-QUEUE CEILING — `rq_last 200` in
// media/Hlms/Jahshaka/JahshakaPcc.compositor (both face passes). Gizmos and
// selection outlines live at RQ 210 and are never captured; keep in step.
static const Ogre::uint8 kProbeFaceRqLast = 200u;

bool OgreScene::probeSeesItem(const Node &n) const {
    if (!n.item || !n.shown) return false;
    if (!(n.item->getVisibilityFlags() & kVisibleBit)) return false;   // helper / distortion
    return n.item->getRenderQueueGroup() <= kProbeFaceRqLast;
}

// THE GI BINDING's arms (SceneGiBinding: the scene's per-pass record): what the component binds
// and unbinds, never the record itself.
void OgreScene::bindGiVct(Ogre::PhotonVoxelLighting *vct) { mGiBinding.vct = vct; }
void OgreScene::bindGiIfd(Ogre::PhotonIrradianceField *ifd) { mGiBinding.ifd = ifd; }
void OgreScene::setGiPassShape(const GiPassShape &shape) {
    if (shape.cascades > 0 || shape.field) PhotonPassBinding::ensureStandIns(mRoot);
    mGiBinding.shape = shape;
}
void OgreScene::bindGiPcc(Ogre::ParallaxCorrectedCubemapBase *pcc, float minDist, float maxDist) {
    mGiBinding.pcc = pcc;
    if (pcc) { mGiBinding.pccMinDist = minDist; mGiBinding.pccMaxDist = maxDist; }
}
bool OgreScene::unbindGiPcc(const Ogre::ParallaxCorrectedCubemapBase *pcc) {
    if (mGiBinding.pcc != pcc) return false;
    mGiBinding.pcc = nullptr;
    return true;
}

/// A dragged still the gesture put on the mover channel comes to rest: the node leaves the channel
/// (its flags re-applied) and `rest` is the box the walk last saw it in. False when the node is
/// gone or no longer a mover.
bool OgreScene::demoteDragMover(NodeId id, Ogre::Aabb &rest) {
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return false;
    Node &n = it->second;
    if (!n.dragMover) return false;
    n.dragMover = false;
    applyNodeVisibilityFlags(n);
    rest = n.scan.giBox;
    return true;
}

void OgreScene::setNodeProbeGridExcluded(NodeId id, bool excluded) {
    if (postScene([=](OgreScene &s) { s.setNodeProbeGridExcluded(id, excluded); return true; })) return;   // U3 4a: a message
    JAH_HOST_CALL("Scene::setNodeProbeGridExcluded");
    auto it = mNodes.find(id);
    if (it == mNodes.end()) return;
    if (it->second.probeGridExcluded == excluded) return;
    it->second.probeGridExcluded = excluded;
    // The flag changes where the probe grid is placed, so it is as much of a
    // change as moving the geometry: flag the caches and let the frame-time
    // flush answer once, however many nodes the caller toggles in a burst.
    invalidateGiCaches();
}

bool OgreScene::nodeProbeGridExcluded(NodeId id) const {
    auto it = mNodes.find(id);
    return it != mNodes.end() && it->second.probeGridExcluded;
}

void OgreScene::endStructureEdit() {
    if (postScene([=](OgreScene &s) { s.endStructureEdit(); return true; })) return;   // U3 4a: a message
    JAH_HOST_CALL("Scene::endStructureEdit");
    if (mStructureEditDepth == 0 || --mStructureEditDepth > 0) return;
    mPhoton.structureEditClosed();
}

// THE END OF A DRAG GESTURE (MOVER-1) — where a dragged Still becomes still
// world again and pays for the whole gesture at once.
//
// WHEN IT ENDS IS THE RECORD'S (U0 3c): the first frame whose scene record carries no active gesture — the user
// let go. (It used to be INFERRED: kProbeMotionSettleFrames ticks with no promoted mover moving, which ended a drag
// under a hand that paused and kept one alive for a script that moved something every frame.)
//
// WHAT IT COSTS, and it is the whole trade: ONE cascade dirty box per mover —
// the place it came to rest — so the chain re-voxelises it once, at the tier's
// own one-cascade-per-frame budget, instead of once per frame of the drag. The
// probe grid is staled ONCE here as well, with reason Mobility, because the
// object is re-joining the capture set it left at the promotion: every probe
// holding a photograph without it owes one capture, which is exactly what a
// mobility flip has always cost (setNodeMovable's step 2) and is what the
// deferred sweep would have spent at the end of the drag anyway.
//
// AND THE PICTURE AT REST IS THE PICTURE AT REST. Nothing here changes what the
// scene looks like once the settle has run — the object is GI geometry again,
// at its new pose, voxelised from scratch — which is the property gi.drag_mover
// asserts by rendering the same frame under both rules.
/// Is this item under the user's gesture (the node the record names, or anything below it)?
bool OgreScene::underGesture(const Node &n) {
    if (!mGesture.active || mGesture.nodes.empty() || !n.node) return false;
    for (const Ogre::Node *p = n.node; p; p = p->getParent())
        if (const Node *rec = registryNode(p))
            if (std::find(mGesture.nodes.begin(), mGesture.nodes.end(), rec->selfId) != mGesture.nodes.end())
                return true;
    return false;
}

// THE GEOMETRY MOVEMENT SCAN (FIX WAVE B3, engine half).
//
// Walks the GI items once and records which of them MOVED since the last scan,
// as the union of each mover's old and new world AABB. Two consumers, both of
// them cheap paths: the probe round-robin below (a probe whose parallax shape
// contains a moved box goes to the front of the sweep) and — through the
// separate, stateless geometry term (giGeometryKey) — the GI scheduler's.
//
// The comparison is QUANTIZED (giAabbQuantum): sub-voxel jitter (a physics body settling, an idle
// animation's sway) must not read as movement for ever. An item seen for the
// FIRST time is recorded, not reported — appearing is not moving, and the
// arrival is already in the signature.
void OgreScene::runItemWalk(bool shadow) {
    // THE FRAME'S CASTER WALK (ENGINE_CACHE_POLICY_SPEC §4 row 7 + P3), run by
    // OgreEngine::applyShadowCacheDirties for every drawn scene right after
    // updateSceneGraph — so it reads the world AABBs the update has just made
    // current (getWorldAabb, no root-recursive getWorldAabbUpdated per item:
    // measured 2.1 ms -> 0.4 ms at 5k items in Debug+ASan).
    //
    // THE GI HALF IS NOT HERE: its consumers (the GI step's walk, its marks and
    // the probe budget) run in the frame's `engine.plan`, also after the scene
    // graph but BEFORE this caster walk (U4 decisions 8 + 14), and stale the
    // probes in this same frame — ensureGiWalk, below, on the cached boxes.
    // (Before U4 they ran ahead of the scene graph on getWorldAabbUpdated; a
    // walk here, after the probe budget had spent, made every mover stale the
    // probes a frame late: gi.dynamic_probes, gi.budget, gi.probe_inputs.)
    if (!shadow && mShadowScanPrimed) {
        // THE CACHE IS NOT RUNNING HERE (this scene has no shadow node to cache
        // into, or not one cacheable lamp): its caster records go stale, and so
        // do the departures waiting to be reported. Un-priming is what keeps
        // mShadowVanished from growing for a whole session of edits in a scene
        // whose shadows are off — and it costs nothing, because the next cached
        // frame is a FRESH slot assignment, which renders every map anyway.
        mShadowScanPrimed = false;
        mShadowVanished.clear();
        releaseNamed(kNameShadow);   // the next primed walk is a full one (firstShadow)
    }
    if (!shadow) return;
    // THE STILL-FRAME GATE (ENGINE-4 F5), the caster half's version of
    // ensureGiWalk's. Everything this walk reads is either a TRANSFORM (the
    // world AABB) or a PUSHED event that moves nothing — a pose, a rebuilt
    // Item, a material's generated vertex piece, a per-object Cast Shadow flag,
    // a render-queue refile, a visibility or channel change. The first half is
    // the host's transform epoch; the second is why the GI half's gate could
    // not simply be copied, and is now counted at its own seams
    // (markShadowShapeDirty / noteShadowScanInput). With neither moved since
    // the last walk, nothing a lamp map depends on can have changed and the
    // walk is O(1) instead of O(items).
    //
    // TWO THINGS THE GATE MUST NOT SKIP, and neither is a transform:
    //   * a caster whose Item DIED (mShadowVanished) — unindexItemNode NAMES
    //     the node (noteSceneTransformWrite(n)), so the walk runs;
    //   * a caster whose material moves VERTICES every frame off the shader
    //     clock (a vertex-stage generated piece). Its lamps must re-render
    //     every frame, so it must be re-flagged every frame.
    //
    // THE SECOND ONE USED TO TAKE THE GATE OUT FOR THE WHOLE SCENE (audit
    // ON-16): `deforms` was a scene-wide bool, so ONE wind or wave material
    // anywhere meant every item in the scene was visited on every frame, at
    // rest, for ever — O(items) per frame, which is exactly the cost this gate
    // exists to remove. It is PER ITEM now: the full walk keeps a roster of the
    // deforming casters (mShadowDeformers, by node id — see its note for why it
    // cannot go stale while the gate holds), and a still frame re-flags those
    // nodes and nothing else.
    //
    // THE GATE IS THE CHANGE SET (U0 3b): what the drained ChangeSet and the seams NAMED since the last walk (each
    // item re-read, nothing else), a full pass only where a nodeless input owes one (noteShadowScanInput, a nodeless
    // write, an incomplete set) — and with nothing named, nothing.
    if (mShadowScanPrimed && !mScanFullShadow && !hasNamed(kNameShadow)) {
        // The changes are per frame by contract (walkItems clears them at its
        // head), and "the walk ran and found nothing" is what the consumer
        // must see — collectShadowCacheFrame walks for itself when the frame's
        // walk did not happen, which would undo the whole gate.
        mShadowChanges.clear();
        // ...EXCEPT THE DEFORMERS, whose vertices have moved since the last
        // frame even though no transform was written. Their box and channels
        // are the ones the walk recorded, and they are still current for
        // exactly the reason the gate fired: nothing moved.
        for (NodeId id : mShadowDeformers) {
            auto it = mNodes.find(id);
            if (it == mNodes.end()) continue;              // gone; the epoch moved too
            const Node::ScanRec &r = it->second.scan;
            if (r.shadowPresent) mShadowChanges.push_back({ r.shadowBox, r.shadowChannels });
        }
        mShadowWalked = true;
        mCasterWalkMicros = 0.0;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!mShadowScanPrimed || mScanFullShadow) { ++mShadowScansFull; walkItems(false, true, false); }
    else walkItems(false, true, false, &takeScanSubset(kNameShadow));
    mShadowWalked = true;
    mCasterWalkMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
}

// THE GI MOVEMENT SCAN, ONCE PER FRAME, ON DEMAND (the shape FIX WAVE B3 gave
// it): whoever consumes mGiMovedBoxes first this frame runs it — the GI step,
// in the frame's `engine.plan` AFTER the scene graph (U4 decisions 8 + 14), so
// it reads each item's CACHED world AABB, and what it stales is spent in the
// same frame; a scene no view draws (no scene graph this frame) recomputes.
bool OgreScene::ensureGiWalk(const PhotonComponent::WalkInputs &in) {
    if (mGiWalkedThisFrame) return false;
    mGiWalkIn = in;
    mGiWalkedThisFrame = true;
    // NOTHING WROTE A TRANSFORM SINCE THE LAST SCAN, so nothing can have moved
    // (clean-2 lane, 2026-09-13). This walk reads getWorldAabbUpdated() per
    // item — a parent-chain walk each — and it ran on every frame of every
    // probe-lit scene, still or not: 392 / 1961 / 4328 us at 1k / 5k / 10k
    // nodes, measured by lane R2. The epoch is the host's transform-write
    // counter plus our own writes; see transformEpoch().
    //
    // EVERYTHING ELSE THE PROBES CONSUME IS PUSHED, NOT SCANNED: an item
    // arriving, leaving, being hidden, shown, made a helper, re-materialled or
    // re-classified all call staleProbeGrid at their own seam (OgreScene.cpp,
    // OgreMaterials.cpp). The scan's unique job is movement, and movement is
    // exactly what the epoch reports.
    //
    // THE GATE IS THE CHANGE SET (U0 3b), as the caster walk's: the items named since the last scan are re-read and
    // nothing else; nothing named, nothing read (giScanMicros 0).
    if (mGiScannedOnce && !mScanFullGi && !hasNamed(kNameGi)) {
        // The boxes are per frame by contract (walkItems clears them at its
        // head), so a skipped frame must publish "nothing moved" rather than
        // last frame's movers — otherwise a single move would stale the grid
        // for ever.
        mGiWalk.clear();
        mGiScanMicros = 0.0;
        return true;
    }
    ++mGiScans;
    const auto t0 = std::chrono::steady_clock::now();
    // THE BOXES WHERE OGRE MADE THEM (U4 stage 3): after the frame's updateSceneGraph the cached world box IS this
    // frame's — the SoA load, no parent-chain recompute; only a walk before it (a scene no view draws) recomputes.
    const bool fresh = !mBoxesCurrent;
    if (!mGiScannedOnce || mScanFullGi) { ++mGiScansFull; walkItems(true, false, fresh); }
    else walkItems(true, false, fresh, &takeScanSubset(kNameGi));
    mGiScanMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

/// THE CHANGE SET'S ENGINE HALF (U0 3b; EnginePrivate.h). A pending list is bounded by the walk it stands for: a
/// node is named once per take (ENGINE-UPGRADE-2B stage 5 - attachMesh fired three or four seams for one node, and
/// 500 re-adds pushed ~2,000 names past a cap of items + decals + 64 at 1k: the whole walk on the SMALLER scene),
/// and a list naming as many nodes as a full walk visits owes the full walk instead, which is then no dearer. A
/// scene whose scans never run (GI off, no cached lamp) keeps a full pass owed instead of an ever-growing list.
void OgreScene::noteSceneTransformWrite(Node &n) {
    ++mSceneTransformWrites;
    // ONCE, on the one list, for all four consumers: the GPU scene's walk, the GI and caster scans, the geometry
    // signature (U4 decision 12.1 ii).
    nameFor(n, kNameAll);
}

/// THE DRAIN'S HALF: each moved node's SUBTREE — its Ogre children, registered or not (a light's -Y adapter), and the
/// riders hanging off its bones (a TagPoint is a bone's child, not the node's) — named for both scans, once per frame;
/// a light anywhere under them bumps the light serial ONCE (the re-inject's key: injectedAtLightSerial). An incomplete
/// set owes the full pass and, when the scene has a light, the serial (it cannot say which moved).
unsigned OgreScene::applyChangeSet(const ChangeSet &cs) {
    if (!cs.complete) {
        ++mHostMoves;
        owesNamesFull(kNameAll);
        // Every lamp's pose key re-read as it stands now (the set cannot say which moved; the owed flags below say
        // "any", and the next named move compares against this).
        bool any = false;
        for (auto &kv : mNodes) {
            Node &n = kv.second;
            if (!n.light) continue;
            any = true;
            if (Ogre::Node *ln = n.light->getParentNode()) {
                n.lightPos = ln->_getDerivedPositionUpdated();
                n.lightRot = ln->_getDerivedOrientationUpdated();
                n.lightPoseKnown = true;
            }
        }
        if (!any) return 0u;
        ++mGiLightWriteSerial;
        mPhoton.noteLampsChanged();   // it cannot say which lamp moved
        return 1u;
    }
    if (cs.moved.empty()) return 0u;
    // EACH MOVED ID IS A SUBTREE ROOT (U4 stage 3): its subtree named under its own GI box (one box per root, not
    // per item), in one naming pass — a node under two moved roots is named once, under the first.
    beginNamingPass();
    bool light = false;
    for (NodeId id : cs.moved) {
        Node *rec = record(id);
        if (!rec || !rec->node) continue;   // released since the emit
        ++mHostMoves;
        mNameStack.clear();
        mNameStack.push_back(rec->node);
        if (nameMovedSubtrees(mNameStack, openGiRoot())) light = true;
    }
    if (light) ++mGiLightWriteSerial;
    return light ? 1u : 0u;
}

void OgreScene::runStashedGiPlan() {
    if (!mGiPlanStash.owed) return;
    monitor::Stage st("engine.plan.signature");   // planGi: its geometry term and its decisions
    FrameInputScene part;
    part.scene = this;
    part.giRefreshSerial = mGiPlanStash.refreshSerial;
    part.materialPreview = mGiPlanStash.materialPreview;
    const bool pushed = mGiPlanStash.pushed;
    mGiPlanStash = GiPlanStash();
    planGi(part, pushed);
}

void OgreScene::beginNamingPass() {
    if (++mNameSeenEpoch == 0u) {   // the 32-bit pass counter wrapped: no node may read as seen
        for (auto &kv : mNodes) kv.second.nameSeen = 0u;
        mNameSeenEpoch = 1u;
    }
    mNameSeenOther.clear();
}

uint32_t OgreScene::openGiRoot() {
    // Bounded: a scene whose GI walk never runs (no chain) never consumes its roots; past the cap a root is no root
    // and its items' boxes go one per item, as before.
    if (mGiRootBoxes.size() >= kGiRootCap) return kNoGiRoot;
    mGiRootBoxes.push_back(Ogre::Aabb());
    mGiRootUsed.push_back(0);
    return uint32_t(mGiRootBoxes.size() - 1u);
}

/// THE DOOR ITSELF (decision 56: the drain's change set and the free frames' interpolation both name through it).
/// One registered node: named to the scans when it draws (an item or a decal), its lamp compared with its pose key, and
/// what moves with it — its bones' riders and its Ogre children — pushed for the walk. True when its lamp moved.
bool OgreScene::nameMovedNode(Node &rec, Ogre::Node *on, std::vector<Ogre::Node *> &stack) {
    bool light = false;
    if (rec.item || rec.decal) noteSceneTransformWrite(rec);
    // A LAMP UNDER A MOVED NODE: named, then COMPARED with its own pose key — the journal says WRITTEN,
    // and only a pose that changed is a move the injection and the scheduler answer.
    if (rec.light && rec.light->getParentNode()) {
        Ogre::Node *ln = rec.light->getParentNode();
        const Ogre::Vector3 p = ln->_getDerivedPositionUpdated();
        const Ogre::Quaternion q = ln->_getDerivedOrientationUpdated();
        if (rec.lightPoseKnown && !(p == rec.lightPos && q == rec.lightRot)) {
            light = true;
            noteLampChanged(rec);
        }
        rec.lightPos = p; rec.lightRot = q; rec.lightPoseKnown = true;
    }
    for (NodeId rider : rec.boneRiders)   // the direct index, counted (decision 22.7): never the ordered map's descent
        if (Node *r = record(rider); r && r->node) stack.push_back(r->node);
    for (size_t i = 0, e = on->numChildren(); i < e; ++i) stack.push_back(on->getChild(i));
    return light;
}

bool OgreScene::nameMovedSubtrees(std::vector<Ogre::Node *> &stack, uint32_t rootSlot) {
    bool light = false;
    // Each Ogre node ONCE A PASS: a moved parent and its moved child must not walk the child's subtree twice (a
    // whole document written at once — a load, a paste — is O(nodes), never O(nodes x depth)). A registered node
    // carries the pass it was named in (one lookup, no set); an unregistered one (a light's adapter) is in the pass's
    // small set.
    while (!stack.empty()) {
        Ogre::Node *on = stack.back();
        stack.pop_back();
        if (Node *rec = registryNode(on)) {
            if (rec->nameSeen == mNameSeenEpoch) continue;
            rec->nameSeen = mNameSeenEpoch;
            ++mCountNameVisits;
            if (rootSlot != kNoGiRoot) { rec->giRootEpoch = mGiRootEpoch; rec->giRootSlot = rootSlot; }
            if (nameMovedNode(*rec, on, stack)) light = true;
        } else {
            if (!mNameSeenOther.insert(on).second) continue;
            ++mCountNameVisits;
            for (size_t i = 0, e = on->numChildren(); i < e; ++i) stack.push_back(on->getChild(i));
        }
    }
    return light;
}

/// The pending list as the walk's item subset: each named node once, those that still carry an Item. The list is
/// TAKEN (cleared) first, so a seam the walk itself fires (a drag promotion's flag write) names its node for the
/// next frame's walk rather than this one's.
std::vector<OgreScene::Node *> &OgreScene::takeScanSubset(uint8_t bit) {
    takeNamed(bit, mScanTakeNodes);
    // NO SORT (U4 stage 3): a node is on the one name list ONCE (its mask, nameFor), so the take holds no duplicate, and
    // the walk's consumers are order-free — the probes' moved-cover test, the lamp maps' dirty set, and the cascades'
    // boxes, which fold into their roots (an untagged seam's own box is the one ordered input: it merges at the cap
    // in naming order, deterministic as the names are).
    mScanSubset.clear();
    for (Node *rec : mScanTakeNodes)
        if (rec->item && rec->itemSlot != size_t(-1)) mScanSubset.push_back(rec);
    mScanTakeNodes.clear();
    return mScanSubset;
}

void OgreScene::indexItemNode(Node &n) {
    noteSceneTransformWrite(n);     // an Item arriving is an input to every scan (named: its item is re-read)
    owesNamesFull(kNameSig);        // the item set changed: the signature sums its items afresh
    if (n.itemSlot != size_t(-1)) return;
    n.itemSlot = mItemNodes.size();
    mItemNodes.push_back(&n);
    if (n.item) mNodeOfItem[n.item] = &n;
    markGpuSlotDirty(n);            // a newborn slot has no entry in the table yet
}

void OgreScene::indexDecalNode(Node &n) {
    if (n.decalSlot != size_t(-1)) return;
    n.decalSlot = mDecalNodes.size();
    mDecalNodes.push_back(&n);
}

void OgreScene::unindexDecalNode(Node &n) {
    n.scan.decalKnown = false;
    if (n.decalSlot == size_t(-1)) return;
    const size_t i = n.decalSlot;
    Node *last = mDecalNodes.back();
    mDecalNodes[i] = last;
    last->decalSlot = i;
    mDecalNodes.pop_back();
    n.decalSlot = size_t(-1);
}

void OgreScene::unindexItemNode(Node &n) {
    noteSceneTransformWrite(n);     // ...and so is one leaving (the walk runs and reports the departure)
    owesNamesFull(kNameSig);
    // A CASTER LEAVING: its last box is what the lamps around it must re-render
    // without it (the scan never sees a node that has no Item).
    if (n.scan.shadowPresent && mShadowScanPrimed)
        mShadowVanished.push_back({ n.scan.shadowBox, n.scan.shadowChannels });
    n.scan.shadowPresent = false;
    n.scan.shadowItem = nullptr;
    if (n.itemSlot == size_t(-1)) return;
    if (n.item) {
        auto owned = mNodeOfItem.find(n.item);
        if (owned != mNodeOfItem.end() && owned->second == &n) mNodeOfItem.erase(owned);
    }
    const size_t i = n.itemSlot;
    Node *last = mItemNodes.back();
    mItemNodes[i] = last;
    last->itemSlot = i;
    mItemNodes.pop_back();
    n.itemSlot = size_t(-1);
    // THE GPU SCENE'S HALF OF THE SWAP-REMOVE, in the one place the swap
    // happens. TWO CASES, and the second is the one that bites: when the dying
    // item was NOT the tail, the tail's entry travels into the freed slot and
    // that slot is re-composed this frame; when it WAS the tail, nothing moves
    // in and the entry it leaves must be CLEARED — otherwise the next attach
    // lands in a slot that is still "born" and inherits the dead object's world
    // as its PREVIOUS world, i.e. a newborn reprojecting out of another
    // object's grave. Either way the freed entry is copied to the device, so the
    // region past `slotCount()` never holds a traceable ghost.
    if (mGpuScene.live()) {
        if (last != &n) {
            mGpuScene.onSlotMoved(uint32_t(mItemNodes.size()), uint32_t(i));
            mGpuForced.push_back(uint32_t(i));
        } else {
            mGpuScene.onSlotFreed(uint32_t(i));
        }
        // THE RAY LEVEL IS KEYED BY SLOT and the slots have just been
        // renumbered (ATOM P3's AT-A8r), so both ends of the swap are cleared:
        // whatever lands in either is evaluated afresh instead of inheriting
        // the distance band of the object that used to be there. (The band
        // cannot travel with the object: nothing here indexes by object.)
        forgetRayLevel(uint32_t(i));
        forgetRayLevel(uint32_t(mItemNodes.size()));
    }
    mGpuScene.setSlotCount(uint32_t(mItemNodes.size()));
}

// THE ITEM WALK — the GI movement scan (FIX WAVE B3, engine half) and the
// lamp-map cache's caster scan (ENGINE_CACHE_POLICY_SPEC P3), in one pass over
// the nodes that HAVE an Item (mItemNodes), with each node's last-seen state on
// the node itself. (It used to be two walks of the whole std::map, each with a
// hash lookup per item: measured 1.4 + 2.3 ms at 5k nodes in Debug+ASan.)
//
// GI half: records which GI items MOVED since the last scan, as the union of
// each mover's old and new world AABB. One consumer, a cheap path: the probe
// round-robin (a probe whose parallax shape contains a moved box goes to the
// front of the sweep). QUANTIZED to a 64th of
// the lit volume (giAabbMoved): sub-voxel jitter must not read as movement for
// ever. An item seen for the FIRST time is recorded, not reported — appearing
// is not moving, and the arrival is already in the signature.
//
// Shadow half: see collectShadowCacheFrame — a change is the box the lamps
// around it must re-render (old and new for a move, the new one for an arrival
// or a shape/pose change, the old one for a departure), with the channels the
// caster renders into.
void OgreScene::walkItems(bool gi, bool shadow, bool fresh, const std::vector<Node *> *subset) {
    if (!gi && !shadow) return;
    // A FULL WALK answers for everything named so far (U0 3b): its halves' pending lists go, BEFORE the loop, so a
    // seam the walk itself fires names its node for the next walk. A SUBSET walk's caller took its list already.
    if (!subset) {
        if (gi)     { releaseNamed(kNameGi);     mScanFullGi = false; }
        if (shadow) { releaseNamed(kNameShadow); mScanFullShadow = false; }
    }
    bool firstGi = false;
    const bool rootParity = gi && mEngine && mEngine->armRegistry().on(ArmId::GiRootBoxParity);
    if (gi) {
        mGiWalk.clear();
        firstGi = !mGiScannedOnce;
        mGiScannedOnce = true;
    }
    bool firstShadow = false;
    Ogre::uint32 channelsAll = 0u;
    // Materials with a vertex-stage piece (usually NONE). A scratch member, not
    // a local: this walk runs every frame (clean-2 lane).
    std::vector<MaterialId> &deforming = mScanDeforming;
    deforming.clear();
    if (shadow) {
        mShadowChanges.clear();
        firstShadow = !mShadowScanPrimed;
        mShadowScanPrimed = true;
        channelsAll = allShadowCasterChannels();
        // A SCROLLING CUT-OUT deforms too (TORNADO-1): its caster alpha-tests the
        // albedo map through the UV macro, which moves with the shader clock, so
        // its silhouette changes every frame without a vertex moving.
        for (const auto &mk : mMaterials) {
            const PbrParams &p = mk.second.params;
            const bool scrollingCutout = !mk.second.unlit && p.alphaMode == PbrAlphaMode::Cutout &&
                                         (p.uvVelocity[0] != 0.0f || p.uvVelocity[1] != 0.0f);
            if (!mk.second.customPiece[1].empty() || scrollingCutout) deforming.push_back(mk.first);
        }
        // The per-item gate's roster, rebuilt by this walk (ON-16): which
        // CASTERS deform, not which materials do. Cleared here and refilled
        // below, so it describes exactly the state this walk saw.
        //
        // A SUBSET walk keeps the roster's other members (the walk does not visit them) and re-flags them exactly as
        // the still-frame gate does — their vertices moved without a write — then drops the visited ones, whose
        // membership the loop below decides afresh.
        if (!subset) {
            mShadowDeformers.clear();
        } else if (!mShadowDeformers.empty()) {
            std::vector<NodeId> &keep = mScanTake;
            keep.clear();
            for (NodeId id : mShadowDeformers) {
                auto it = mNodes.find(id);
                if (it == mNodes.end()) continue;
                if (std::find(subset->begin(), subset->end(), &it->second) != subset->end()) continue;
                const Node::ScanRec &r = it->second.scan;
                if (r.shadowPresent) mShadowChanges.push_back({ r.shadowBox, r.shadowChannels });
                keep.push_back(id);
            }
            mShadowDeformers.swap(keep);
            keep.clear();
        }
    }
    for (Node *np : subset ? *subset : mItemNodes) {
        Node &n = *np;
        Ogre::Item *item = n.item;
        if (!item) continue;
        // THE BOX. `fresh` (the GI scan, which runs BEFORE the frame's scene
        // graph update) has to see THIS frame's transforms and pays
        // getWorldAabbUpdated — a parent-chain walk per item, ~400 ns in Debug.
        // The caster walk runs after updateSceneGraph and reads the cached
        // world AABB. (Ogre's own "is this node's transform dirty" flag, which
        // would let the fresh walk skip the update where nothing moved, exists
        // only in OGRE_DEBUG_MEDIUM builds — an engine-side movement epoch is
        // the way to cut this, and it is a lane of its own.)
        // (The GI walk's reads are counted whichever form they take: the count is the walk's item boxes, the probe
        // and the scan rows' "the walk ran" — U4 stage 3 made them cached after the scene graph.)
        if (gi) ++mGiWalk.aabbReads;
        const Ogre::Aabb a = fresh ? item->getWorldAabbUpdated() : item->getWorldAabb();
        // Read once per item, both halves use them (each is an SoA read, and
        // this loop is the per-frame cost the whole walk is measured by).
        const Ogre::uint32 flags = item->getVisibilityFlags();
        const Ogre::uint8 rq = item->getRenderQueueGroup();
        // A SHARED skeleton poses through its source (shareSkeleton): an
        // armour piece deforms when the body's clip moves.
        unsigned long long pose = n.poseEpoch;
        if (n.shareSource) {
            auto sit = mNodes.find(n.shareSource);
            if (sit != mNodes.end()) pose = pose * 1000003ull + sit->second.poseEpoch;
        }
        // A VERTEX-STAGE generated piece can move vertices every frame (it
        // reads the shader clock) — risk 6: such a caster keeps its lamps
        // re-rendering, the way Unreal excludes WPO materials from caching.
        const bool deforms = !deforming.empty() &&
            std::find(deforming.begin(), deforming.end(), n.materialRef) != deforming.end();
        // (NO SHARED "unchanged since the last walk" SHORTCUT. The two halves
        // run in different walks reading the box two different ways — updated
        // for GI, cached for the casters — and the two reads are not
        // bit-identical, so one shared snapshot thrashed and BOTH halves took
        // the long path every frame. Each half's own record is the comparison,
        // and each is a handful of float compares.)
        if (gi) {
            // A PROMOTED DRAG MOVER FIRST (MOVER-1). It carries kMovableBit and
            // no kGiGeometryBit, so without this branch it would fall into the
            // probe-only case below and stale the probe grid on every frame of
            // the gesture — the exact cost the promotion removes. Its motion is
            // nobody's input but the GESTURE'S OWN CLOCK: it is out of the
            // voxel bounce, out of the probe captures and out of the cascade
            // dirty list, and its box is tracked only so the settle knows when
            // the drag stopped and what to re-voxelise once at the end.
            if (n.dragMover) {
                if (PhotonComponent::giAabbMoved(n.scan.giBox, a)) n.scan.giBox = a;
            } else if (!(flags & kGiGeometryBit)) {
                // PROBE-ONLY geometry (P7): unlit, captured by the probe faces,
                // never voxelized. Same quantized test, its own record, and a
                // flag rather than a moved box — the voxel side must not hear of it.
                // (probeSeesItem, inlined on the values read above.)
                if (n.shown && (flags & kVisibleBit) && rq <= kProbeFaceRqLast) {
                    if (!n.scan.probeKnown) {
                        n.scan.probeKnown = true;
                        n.scan.probeBox = a;
                        if (!firstGi) mGiWalk.probeOnlyChanged = true;
                    } else if (PhotonComponent::giAabbMoved(n.scan.probeBox, a)) {
                        n.scan.probeBox = a;
                        mGiWalk.probeOnlyChanged = true;
                    }
                }
            } else if (!n.scan.giKnown) {
                n.scan.giKnown = true;
                n.scan.giBox = a;
                // An ARRIVAL, after the first scan (which sees every item for
                // the first time and is not news): the probes must capture it (P1).
                if (!firstGi) mGiWalk.itemsAppeared = true;
                // ...AND THE CHAIN HAS TO GROW INTO IT (PHOTON_SPEC E2 (2)).
                //
                // Without this a NEW GI item that arrived after the chain was
                // built cast no bounce at all, silently and for ever: nothing
                // else ever re-selects a cascade's item set. (NEW is what this branch
                // sees: `scan.giKnown` is set the first time the walk meets a
                // node and is never cleared, so an item that leaves the channel
                // and comes back is not a second arrival — that edge goes
                // through `invalidateGiCaches` like every other channel change.)
                //
                // The dirty box is what gives the cascades that can actually SEE
                // the newcomer a rebuild to take (no attach set to re-select: the
                // gather reads membership from the GPU scene at every build). The
                // scheduler still spends at most one cascade per frame.
                if (!firstGi && mGiWalkIn.chainBuilt) {
                    mGiWalk.cascadeDirty.push_back(a);
                    if (rootParity) mGiWalk.itemDirty.push_back(a);
                }
            } else if (PhotonComponent::giAabbMoved(n.scan.giBox, a)) {
                Ogre::Aabb moved = n.scan.giBox;
                moved.merge(a);
                // IS THIS A GESTURE? (MOVER-1.) THE RECORD SAYS (U0 3c): an object the user's gesture moves — the
                // node the host's FrameInputScene::gesture names, or anything under it — is a drag; any other move
                // is an edit (a scripted setPosition, a nudge, a paste, physics) and is answered where it happens,
                // which is what the branch below does. The engine no longer infers a drag from a second move inside
                // a window of frames: the host has the user's hand and says so.
                if (mGiWalkIn.dragMoverChannel && underGesture(n) && mGiWalkIn.chainBuilt && !n.movable) {
                    // THE PROMOTION. The object leaves the still world for the
                    // length of the gesture: one re-voxelisation HERE clears
                    // the copy of it the cascades hold at its old pose (the
                    // union box, so the place it came from and the place it is
                    // now are both re-voxelised), and from this frame on its
                    // motion costs the voxels nothing at all. What it gives up
                    // is stated rather than hidden: it stops bouncing light
                    // into the room and the room stops storing its shadow,
                    // exactly as a Movable object does — it is LIT by the field
                    // and the cones at its live pose, which is what makes the
                    // drag read correctly instead of through a half-rebuilt
                    // volume.
                    //
                    // THE PROBE GRID IS DELIBERATELY NOT STALED HERE. During a
                    // gesture the probes hold the photograph they hold today
                    // (the capture is deferred to the settle either way,
                    // DRAG-1), and the object's DEPARTURE from the capture set
                    // is answered once, at the demotion, with reason Mobility.
                    n.scan.giBox = a;
                    n.dragMover = true;
                    mGiWalk.promoted.push_back(n.selfId);
                    applyNodeVisibilityFlags(n);
                    mGiWalk.cascadeDirty.push_back(moved);
                    if (rootParity) mGiWalk.itemDirty.push_back(moved);
                } else {
                    mGiWalk.movedBoxes.push_back(moved);
                    // ...AND THE CASCADE CHAIN'S OWN RECORD (G1), which unlike
                    // `mGiMovedBoxes` (per frame, by contract) accumulates until the
                    // host's settle asks the chain to answer for it: the frame a
                    // drag is released is not the frame the box moved.
                    // ONE BOX PER MOVED SUBTREE ROOT (U4 stage 3): an item named
                    // under a root this window folds its old and new box into the
                    // root's (emitted after the loop) — a superset of the item's,
                    // so never a cascade fewer; anything else, its own box.
                    if (mGiWalkIn.chainBuilt) {
                        if (rootParity) mGiWalk.itemDirty.push_back(moved);
                        if (n.giRootEpoch == mGiRootEpoch && n.giRootSlot < mGiRootBoxes.size()) {
                            if (mGiRootUsed[n.giRootSlot]) mGiRootBoxes[n.giRootSlot].merge(moved);
                            else { mGiRootBoxes[n.giRootSlot] = moved; mGiRootUsed[n.giRootSlot] = 1; }
                        } else {
                            mGiWalk.cascadeDirty.push_back(moved);
                        }
                    }
                    n.scan.giBox = a;
                }
            }
        }
        if (shadow) {
            ++mCasterWalkItems;
            // THE CASTER PREDICATE. A helper carries kHelperBit and a distortion
            // item kDistortionBit — neither is in a shadow channel. The on-top
            // overlay queue (gizmos, bone overlays: unlit, depth test off,
            // kVisibleBit — lighting audit L6.3) writes no depth even where a
            // shadow pass draws it, so it is never a caster either.
            const Ogre::uint32 channels = flags & channelsAll;
            const bool present = channels != 0u && n.shown && rq < kOverlayRenderQueue &&
                                 item->getCastShadows();
            Node::ScanRec &r = n.scan;
            if (!firstShadow) {
                if (present && !r.shadowPresent) {
                    mShadowChanges.push_back({ a, channels });
                } else if (!present && r.shadowPresent) {
                    mShadowChanges.push_back({ r.shadowBox, r.shadowChannels });
                } else if (present) {
                    if (r.shadowItem != item || boxMovedForShadows(r.shadowBox, a) ||
                        r.shadowChannels != channels) {
                        Ogre::Aabb both = r.shadowBox; both.merge(a);
                        mShadowChanges.push_back({ both, channels | r.shadowChannels });
                    } else if (r.shadowPose != pose || deforms || n.shadowShapeDirty) {
                        mShadowChanges.push_back({ a, channels });
                    }
                }
            }
            // THE ROSTER (ON-16): a present caster whose material deforms owes
            // a change on every frame, gate or no gate.
            if (deforms && present) mShadowDeformers.push_back(n.selfId);
            n.shadowShapeDirty = false;
            r.shadowPresent = present;
            r.shadowItem = present ? item : nullptr;
            r.shadowBox = present ? a : Ogre::Aabb();
            r.shadowPose = pose;
            r.shadowChannels = channels;
        }
    }
    if (gi) {
        // THE ROOTS' BOXES, one each, after the items' own (U4 stage 3) — and the window closes: a tag from it is
        // no longer a root's.
        for (size_t i = 0; i < mGiRootBoxes.size(); ++i)
            if (mGiRootUsed[i]) mGiWalk.cascadeDirty.push_back(mGiRootBoxes[i]);
        mGiRootBoxes.clear();
        mGiRootUsed.clear();
        if (++mGiRootEpoch == 0u) {   // wrapped: no node may read as tagged
            for (auto &kv : mNodes) kv.second.giRootEpoch = 0u;
            mGiRootEpoch = 1u;
        }
        // THE DECALS (clean-2 lane, 2026-09-13). A decal is not an Item and its
        // node usually carries none, so the loop above cannot see it — yet the
        // probe faces render decals like any other Forward+ surface, which
        // makes a decal that MOVES a probe input exactly as a moved crate is.
        // (Arrival, removal and edits stale the grid at their own seam, in
        // setDecal/removeDecal; this half is the one that cannot be pushed.)
        // Its box is `probeOnly`: a decal paints a surface the probes capture,
        // and the voxels never see one.
        for (Node *np : mDecalNodes) {
            Node &n = *np;
            if (!n.decal || !n.decalNode) continue;
            // (the cached box after the scene graph — `engine.plan`, U4 decision 13.2 — as the items' above)
            const Ogre::Aabb a = fresh ? n.decal->getWorldAabbUpdated() : n.decal->getWorldAabb();
            if (!n.scan.decalKnown) {
                n.scan.decalKnown = true;
                n.scan.decalBox = a;
                continue;                       // arrival: setDecal staled it already
            }
            if (PhotonComponent::giAabbMoved(n.scan.decalBox, a)) {
                n.scan.decalBox = a;
                mGiWalk.probeOnlyChanged = true;
            }
        }
    }
    if (shadow) {
        if (!firstShadow)
            mShadowChanges.insert(mShadowChanges.end(), mShadowVanished.begin(), mShadowVanished.end());
        mShadowVanished.clear();
    }
}

}}}  // namespace jahshaka::engine::detail
