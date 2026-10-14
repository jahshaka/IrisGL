/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

// THE document library's one Ogre translation unit (SPECS/SCENEGRAPH_SPEC.md).
// Nothing above it may include an Ogre header; see nodegraph.h for the law and
// the list of sanctioned TUs.

#include "document/scenegraph/nodegraph.h"
#include "document/scenegraph/scenenode.h"
#include "core/math/trs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QDebug>

#include "OgreMovableObject.h"
#include "OgreQuaternion.h"
#include "OgreRoot.h"
#include "OgreHlmsManager.h"
#include "OgreSceneManager.h"
#include "OgreSceneNode.h"
#include "OgreVector3.h"
#include "Math/Array/OgreObjectMemoryManager.h"

namespace iris
{
namespace graph
{

namespace
{

/// IS THERE STILL AN ENGINE? Ogre::Root is a Singleton whose base destructor
/// clears the instance pointer, so this answers false the moment the engine is
/// gone — and every handle in the program dangles at that instant, because the
/// nodes were the Root's scene managers' nodes.
///
/// The document outlives the engine by design at shutdown (MainWindow destroys
/// the Engine at step 5 and its scene later), so every entry point here is
/// guarded by this rather than by an ordering rule nobody can enforce. It costs
/// one load of a static pointer.
inline bool engineAlive() { return Ogre::Root::getSingletonPtr() != nullptr; }

inline Ogre::SceneManager *sm(SceneHandle h) { return reinterpret_cast<Ogre::SceneManager *>(h); }
inline SceneHandle wrap(Ogre::SceneManager *s) { return reinterpret_cast<SceneHandle>(s); }
inline Ogre::SceneNode *nd(NodeHandle h) { return reinterpret_cast<Ogre::SceneNode *>(h); }
inline NodeHandle wrap(Ogre::SceneNode *n) { return reinterpret_cast<NodeHandle>(n); }

inline Ogre::Vector3 toOgre(const Vec3 &v) { return Ogre::Vector3(v.x(), v.y(), v.z()); }
inline Vec3 toIris(const Ogre::Vector3 &v) { return Vec3(v.x, v.y, v.z); }
inline Ogre::Quaternion toOgre(const Quat &q)
{
    return Ogre::Quaternion(q.scalar(), q.x(), q.y(), q.z());
}
inline Quat toIris(const Ogre::Quaternion &q) { return Quat(q.w, q.x, q.y, q.z); }

// ---------------------------------------------------------------------------
// THE BACK-POINTER TABLE — Ogre node id -> document handle.
//
// The obvious home for this is Ogre's UserObjectBindings, and that is where it
// started. It cost too much: `setUserAny` allocates an Attributes block on
// demand and `Ogre::Any`'s copy constructor CLONES its holder, so a back-
// pointer was three heap allocations on the path every document node creation
// takes. Measured on the §6 benchmark: 1.3 us per node, +23% on e.build_doc,
// i.e. the single largest cost the whole swap added. Removing it puts document
// build back at pre-swap parity.
//
// So: a chunked flat array indexed by `Ogre::Node::getId()` (a process-wide
// monotonic counter — OgreSceneManager.cpp:915). Lookup is two loads and no
// hashing, which matters because ownerOf() is on the hot path of every
// children() and every getParent(). Writes take the graph mutex; reads take
// nothing, and are safe against a concurrent write because the chunk POINTERS
// are atomic and a chunk, once published, is never moved or freed.
//
// Cost: 512 KB of untouched BSS for the chunk directory, and 64 KB per 8192
// live ids actually used. Ids are never reused, so a session that creates
// hundreds of millions of nodes would run off the end — that is reported once
// and then the node simply has no owner (it reads as engine-owned), which
// degrades to "invisible to the document" rather than to a wrong answer.
constexpr std::size_t kOwnerChunkShift = 13;                     // 8192 ids / chunk
constexpr std::size_t kOwnerChunkSize = std::size_t(1) << kOwnerChunkShift;
constexpr std::size_t kOwnerChunkCount = 65536;                  // 536 M ids
std::atomic<SceneNode **> gOwnerChunks[kOwnerChunkCount];

inline SceneNode *ownerById(Ogre::IdType id)
{
    const std::size_t chunk = std::size_t(id) >> kOwnerChunkShift;
    if (chunk >= kOwnerChunkCount) return nullptr;
    SceneNode **entries = gOwnerChunks[chunk].load(std::memory_order_acquire);
    return entries ? entries[std::size_t(id) & (kOwnerChunkSize - 1)] : nullptr;
}

/// Under graphMutex().
void setOwnerById(Ogre::IdType id, SceneNode *owner)
{
    const std::size_t chunk = std::size_t(id) >> kOwnerChunkShift;
    if (chunk >= kOwnerChunkCount) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("iris::graph: the scene-node id space is exhausted (id %llu) — document "
                     "nodes created from here on have no back-pointer.",
                     static_cast<unsigned long long>(id));
        }
        return;
    }
    SceneNode **entries = gOwnerChunks[chunk].load(std::memory_order_relaxed);
    if (!entries) {
        entries = new SceneNode *[kOwnerChunkSize]();
        gOwnerChunks[chunk].store(entries, std::memory_order_release);
    }
    entries[std::size_t(id) & (kOwnerChunkSize - 1)] = owner;
}

std::recursive_mutex &graphMutex()
{
    static std::recursive_mutex m;
    return m;
}

/// The staging scene manager and the Root it was made from. The Root pointer is
/// how a stale cache is spotted after an engine restart inside one process
/// (the suites do that): a different Root means our SceneManager died with the
/// old one.
Ogre::Root *gStagingRoot = nullptr;
Ogre::SceneManager *gStaging = nullptr;
/// Did WE create it (the fallback path)? A host-supplied one belongs to the
/// engine and must not be destroyed here.
bool gStagingIsOurs = false;
/// How many live handles sit in a SCENE_STATIC memory manager. Maintained by
/// applyStaticSubtree and by the destroy/migrate paths, so the benchmark can
/// assert that its 80% static population really reached the static manager
/// rather than reporting a hint nobody applied.
std::size_t gStaticNodes = 0;

/// THE WRITE JOURNAL (see graph::WriteJournalReader): one log of the nodes the movement counter counted, in write
/// order, BY OGRE'S NODE ID, never by pointer. A node can die between its write and the read through paths that never
/// pass destroyRecursive (a staging migration's drop, a scene manager torn down whole), and a pointer kept here would
/// be read after free (the play_select SIGSEGV that found it); an ID resolves through the owner table, which every
/// destroy path clears, so a dead node simply resolves to nothing. `owned` records whether a DOCUMENT node was
/// written: an engine-owned one cannot be resolved later, so its presence makes the take incomplete.
///
/// Each entry carries its sequence number; a reader's cursor is the first sequence it has not read. An entry is
/// dropped once every live reader's cursor is past it, and a write is logged only if some reader would not otherwise
/// see that node again (its newest entry is behind the furthest cursor) — so a node dragged for a minute is one entry
/// per reader-take, not one per write.
/// `moved`: a journal write (a transform through the one funnel, a reparent, a detach — the node's world pose and its
/// subtree's may have changed); false: the WRITTEN half — any other document write SceneNode::notifyChanged reports
/// (a mesh or material swap, a parameter, a visibility, a flag), logged only while a reader that asked for it lives.
struct JournalEntry { Ogre::IdType id = 0; bool owned = false; bool moved = true; unsigned long long seq = 0; };
inline unsigned long long journalKey(Ogre::IdType id, bool moved) { return (static_cast<unsigned long long>(id) << 1) | (moved ? 1u : 0u); }
std::mutex gJournalMutex;
std::deque<JournalEntry> gJournal;
std::unordered_map<unsigned long long, unsigned long long> gJournalNewest;   ///< (node, kind) -> its newest entry's seq
std::atomic<int> gJournalWrittenReaders{0};   ///< readers that asked for the WRITTEN half (noteDocumentWrite)
std::vector<graph::WriteJournalReader *> gJournalReaders;
unsigned long long gJournalNextSeq = 1;
unsigned long long gJournalMaxCursor = 0;   ///< the furthest reader's cursor (the dedupe's bar)
unsigned long long gJournalCapFloor = 0;    ///< every seq below this one that the CAP dropped (not the readers)
/// THE CAP: a reader that stops taking (a mirror whose host stopped syncing; physics while the editor edits for an
/// hour) must not grow the log for ever. Past the cap the oldest entries go and a reader behind them gets an
/// incomplete take — its full pass — which is the honest answer, never a silent miss.
constexpr std::size_t kJournalCap = std::size_t(1) << 16;

inline void journalTrimLocked()
{
    unsigned long long minCursor = gJournalNextSeq;
    for (const graph::WriteJournalReader *r : gJournalReaders) minCursor = std::min(minCursor, r->cursor);
    while (!gJournal.empty() && (gJournal.front().seq < minCursor || gJournal.size() > kJournalCap)) {
        const JournalEntry &e = gJournal.front();
        if (e.seq >= minCursor) gJournalCapFloor = e.seq + 1;   // a reader still owed it
        auto it = gJournalNewest.find(journalKey(e.id, e.moved));
        if (it != gJournalNewest.end() && it->second == e.seq) gJournalNewest.erase(it);
        gJournal.pop_front();
    }
}

/// One entry, the dedupe and the cap (the caller holds gJournalMutex). See graph::WriteJournalReader.
inline void journalLogLocked(Ogre::IdType id, bool moved)
{
    const unsigned long long key = journalKey(id, moved);
    auto it = gJournalNewest.find(key);
    if (it != gJournalNewest.end() && it->second >= gJournalMaxCursor) return;   // every reader still sees that one
    const unsigned long long seq = gJournalNextSeq++;
    gJournal.push_back({ id, ownerById(id) != nullptr, moved, seq });
    gJournalNewest[key] = seq;
    if (gJournal.size() > kJournalCap) journalTrimLocked();
}

inline void journalWrite(Ogre::SceneNode *n)
{
    const Ogre::IdType id = n->getId();
    std::lock_guard<std::mutex> lock(gJournalMutex);
    if (gJournalReaders.empty()) return;          // nobody will ever read it
    journalLogLocked(id, true);
}

Ogre::SceneNode *rootOf(Ogre::SceneManager *s)
{
    return s->getRootSceneNode(Ogre::SCENE_DYNAMIC);
}

inline SceneNode *ownerRaw(Ogre::SceneNode *n) { return ownerById(n->getId()); }

/// Is this Ogre node a document node riding a socket (graph::setSocketRider)? The flag is the store's.
inline bool isRiderNode(Ogre::SceneNode *n)
{
    const SceneNode *o = ownerRaw(n);
    return o && o->docNode().rider;
}

/// INVALIDATION (nodegraph.h, the store's caches): `n` and its subtree lose their cached world and box. Stops at a
/// node already invalid (the invariant: nothing below it is valid) unless `force` — a rider flag flipping changes
/// where a world comes from, and a rider's subtree is never valid, so the walk must not trust the early stop there.
/// The import worker invalidates its own fragment's nodes on its own thread; a fragment is handed over whole.
void invalidateSubtree(const DocNode &n, bool force = false)
{
    std::vector<const DocNode *> stack{ &n };
    while (!stack.empty()) {
        const DocNode *d = stack.back();
        stack.pop_back();
        if (!d->worldValid && !force) continue;
        d->worldValid = false;
        d->boxValid = false;
        if (d->pickTable) d->pickTable->markStale(d->pickSlot);
        for (const DocNode *c : d->children) stack.push_back(c);
    }
}

/// The OGRE HALF of a rotation write: the store's value (canonicalRotation, below — already unit, or normalised
/// once by Ogre's own formula) goes into the Ogre node BIT-EXACT. Ogre::Node::setOrientation would re-normalise it
/// unconditionally (OgreNode.cpp:572), and float normalisation is not idempotent, so the two stores would drift by
/// a normalisation step per write of a node's own value.
void setOrientationExact(Ogre::Node *o, const Ogre::Quaternion &q)
{
    if (std::fabs(q.Norm() - 1.0f) > 1e-5f) {
        o->setOrientation(q);
        return;
    }
    Ogre::Transform &t = o->_getTransform();
    t.mOrientation->setFromQuaternion(q, t.mIndex);
    // The cached full-transform matrix must be re-derived (setOrientation's
    // CACHED_TRANSFORM_OUT_OF_DATE); the flag is protected, and a position
    // write of the position's own value raises it without touching the value.
    o->setPosition(o->getPosition());
}

// ---- SCENE_STATIC ---------------------------------------------------------

/// Rule 2 (nodegraph.h): a static node's parent must be static, or must be the
/// DOCUMENT ROOT. The document root is recognised structurally — it is the
/// document node whose own Ogre parent is the scene manager's root node — and
/// its local transform is identity for the life of the scene (nothing in
/// either repo writes a transform on `Scene::getRootNode()`; SceneNode's
/// transform setters say so and Scene::rootNode is never handed to a command).
bool isDocumentRoot(Ogre::Node *n)
{
    // A document node with no document node above it: either its Ogre parent is
    // the scene manager's own root node (a scene bound to an engine, and the
    // node detach() re-homes things under), or it has NO parent at all — which
    // is how every node is born and how `Scene::Scene` builds its root before
    // the document has ever met an engine scene.
    Ogre::Node *p = n->getParent();
    return !p || ownerById(p->getId()) == nullptr;
}

bool eligibleStaticParent(Ogre::Node *p)
{
    if (!p) return false;                       // detached: nothing anchors it
    if (p->isStatic()) return true;
    return isDocumentRoot(p);
}

/// staticSwitchCount(): nodes that changed memory-manager class (switchOne's one Node::setStatic each).
std::atomic<unsigned long long> gStaticSwitches{0};
/// ReexpressScope's depth on this thread (graph writes happen on the document's thread).
thread_local int gReexpressDepth = 0;

/// Switches ONE node, attachments included, without SceneNode::setStatic's
/// half-way failure mode: that one migrates the node FIRST and only then
/// throws if an attachment refuses, leaving the node in the new manager and
/// the object in the old — which is the state `attachObject` calls a bug.
/// Returns false having changed nothing.
bool switchOne(Ogre::SceneNode *n, bool value)
{
    if (n->isStatic() == value) return true;
    // Attachments first. MovableObject::setStatic REPORTS a refusal (its
    // object memory manager has no twin — lights and PFX2 particle systems)
    // instead of throwing, and it drags the parent node along with it, so
    // after the first success the node is already in the right manager.
    std::vector<Ogre::MovableObject *> switched;
    switched.reserve(n->numAttachedObjects());
    for (std::size_t i = 0; i < n->numAttachedObjects(); ++i) {
        Ogre::MovableObject *obj = n->getAttachedObject(i);
        if (obj->isStatic() == value) continue;
        if (!obj->setStatic(value)) {
            for (Ogre::MovableObject *done : switched) done->setStatic(!value);
            return false;
        }
        switched.push_back(obj);
    }
    // Node::setStatic, not SceneNode::setStatic: the attachments are already
    // where they belong, and the override would walk them a second time.
    n->Ogre::Node::setStatic(value);
    gStaticSwitches.fetch_add(1, std::memory_order_relaxed);
    if (value && n->getCreator()) n->getCreator()->notifyStaticDirty(n);
    return true;
}

/// May THIS node go static? An engine-owned node (a light's -Y adapter, a
/// decal box, a wire) has no opinion and rides its parent; a document node
/// answers with its own kind (SceneNode::isStaticEligible).
inline bool eligibleForStatic(Ogre::SceneNode *n)
{
    SceneNode *o = ownerById(n->getId());
    return !o || o->isStaticEligible();
}

/// Rule 1: static is a SUBTREE property.
///
/// SKIP, DO NOT VETO. A branch that cannot go static — a light inside an
/// imported model, an animated node, a particle emitter — is left dynamic
/// together with everything under it, and the REST of the subtree still goes
/// static. (Ogre is explicit that a dynamic child of a static parent is valid
/// and useful; it is the opposite that is a bug.) Vetoing instead would mean
/// one lamp in a building denied the whole building its classification.
///
/// Returns whether the ROOT `n` itself changed class — that is the only
/// refusal a caller can act on. Demotion never fails: the only objects that
/// cannot switch (lights, particle systems) are permanently dynamic anyway, so
/// they are never in a state a demotion has to undo.
bool applyStaticSubtree(Ogre::SceneNode *n, bool value)
{
    std::function<bool(Ogre::SceneNode *)> walk = [&](Ogre::SceneNode *cur) -> bool {
        if (value && !eligibleForStatic(cur)) return false;   // skip it and its branch
        const bool was = cur->isStatic();
        if (!switchOne(cur, value)) return false;
        if (was != cur->isStatic()) {
            if (value) ++gStaticNodes;
            else if (gStaticNodes) --gStaticNodes;
        }
        for (std::size_t i = 0; i < cur->numChildren(); ++i)
            walk(static_cast<Ogre::SceneNode *>(cur->getChild(i)));
        return true;
    };
    return walk(n);
}

/// The document's own answer to "should this subtree be static?", read through
/// the owner back-pointer. An engine-owned node has no opinion.
inline bool ownerWantsStatic(Ogre::SceneNode *n)
{
    SceneNode *owner = ownerById(n->getId());
    return owner && owner->wantsStatic();
}

/// Brings a freshly (re-)parented subtree's class in line with the document's
/// hint. Called once per structural move, on the SUBTREE ROOT.
///
/// The default case costs NOTHING: a node with no static hint attached to a
/// dynamic parent is already dynamic (it inherited the parent's manager), so
/// the comparison fails and no migration happens. Only a static-hinted subtree
/// under an eligible parent, or a dynamic subtree that inherited static from a
/// static parent, pays.
void reconcileStatic(Ogre::SceneNode *n)
{
    const bool want = ownerWantsStatic(n) && eligibleStaticParent(n->getParent());
    if (n->isStatic() == want) return;
    applyStaticSubtree(n, want);
}

/// Depth-first, deepest-first destruction. Ogre's destroySceneNode ORPHANS the
/// children it does not destroy and leaks them into mSceneNodes until
/// clearScene (spec §5, probe-verified) — this is the one path that does not.
///
/// A child with NO document owner belongs to the ENGINE (a light's -Y adapter,
/// a decal's projector box, a light's range wire). Those are not ours to
/// destroy — the engine's releaseNode still holds raw pointers to them — so
/// they are re-homed under their scene's root and left for it to clean up.
void destroyRecursive(Ogre::SceneNode *n)
{
    Ogre::SceneNode *sceneRoot = rootOf(n->getCreator());
    while (n->numChildren() > 0) {
        Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(n->getChild(0));
        if (ownerRaw(c)) {
            destroyRecursive(c);
        } else {
            n->removeChild(c);
            if (c != sceneRoot) sceneRoot->addChild(c);
        }
    }
    if (SceneNode *owner = ownerRaw(n)) owner->_setGraphNode(nullptr);
    setOwnerById(n->getId(), nullptr);
    if (n->isStatic() && gStaticNodes) --gStaticNodes;
    if (n->getParent()) n->getParent()->removeChild(n);
    n->getCreator()->destroySceneNode(n);
}

/// See graph::transformWrites(). Relaxed because it is a CHANGE TEST read once
/// a frame by the renderer: it needs to observe "different from last frame"
/// eventually, not to order anything. (Deleted with the static-shadow opt-in
/// in lane E2, 2026-09-11, and reinstated here for the GI movement scan, which
/// was left walking every item of every probe-lit scene every frame.)
std::atomic<unsigned long long> gTransformWrites{0};

}  // namespace

void setStagingScene(SceneHandle s)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    gStaging = sm(s);
    gStagingRoot = Ogre::Root::getSingletonPtr();
    gStagingIsOurs = false;
}

SceneHandle stagingScene()
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return nullptr;
    if (gStaging && gStagingRoot == root) return wrap(gStaging);
    // A new Root: whatever we cached died with the old one.
    gStaging = nullptr;
    gStagingIsOurs = false;
    gStagingRoot = root;
    // FALLBACK, for hosts that never called setStagingScene (the suites, which
    // always create their View before their first document node). A scene
    // manager cannot be constructed before the backend has a Window and its
    // resource groups initialised — its constructor looks up a material the
    // common scripts define and dereferences the result — and "the Hlms is
    // registered" is the one test for that state visible from here. Answer null
    // rather than crash inside Ogre: the caller reports it.
    if (!root->getHlmsManager() || !root->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        return nullptr;
    // NO worker threads (SPECS/THREADING_ADOPTION_SPEC.md P5): this manager is
    // never drawn and never culled, and since the explicit frame loop (P3) the
    // render loop does not even walk it. 1 spawned a thread and paid two
    // barrier syncs per parallel pass for nothing; 0 makes Ogre run those
    // passes inline with no thread and no barrier (mForceMainThread,
    // OgreSceneManager.cpp:171). The engine-side twin is
    // OgreEngine::documentGraphScene.
    gStaging = root->createSceneManager(Ogre::ST_GENERIC, 0u, "iris-staging");
    gStagingIsOurs = true;
    return wrap(gStaging);
}

SceneHandle sceneOf(NodeHandle n)
{
    if (!n || !engineAlive()) return nullptr;
    return wrap(nd(n)->getCreator());
}

bool sceneAlive(SceneHandle s)
{
    if (!s) return false;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return false;
    Ogre::SceneManagerEnumerator::SceneManagerIterator it = root->getSceneManagerIterator();
    for (auto cur = it.begin(); cur != it.end(); ++cur)
        if (cur->second == sm(s)) return true;
    return false;
}

void shutdown()
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (gStagingIsOurs && gStaging && Ogre::Root::getSingletonPtr() == gStagingRoot && gStagingRoot)
        gStagingRoot->destroySceneManager(gStaging);
    gStaging = nullptr;
    gStagingRoot = nullptr;
    gStagingIsOurs = false;
}

NodeHandle createNode(SceneHandle s, NodeHandle parent, SceneNode *owner)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (!s || !engineAlive()) return nullptr;
    // PARENTLESS when no parent is named. A node created under the scene's root
    // and then re-parented by insertChild pays TWO NodeMemoryManager migrations
    // (Node::setParent detaches from one depth level and attaches to another),
    // and "create it, set it up, then add it to something" is how every caller
    // in the tree builds a document. Ogre-Next supports a parentless node — it
    // sits at depth 0 against the manager's dummy parent transform.
    Ogre::SceneNode *n = parent ? nd(parent)->createChildSceneNode(Ogre::SCENE_DYNAMIC)
                                : sm(s)->createSceneNode(Ogre::SCENE_DYNAMIC);
    setOwnerById(n->getId(), owner);
    return wrap(n);
}

void destroyNode(NodeHandle n)
{
    if (!n || !engineAlive()) return;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    destroyRecursive(nd(n));
}

SceneNode *ownerOf(NodeHandle n) { return (n && engineAlive()) ? ownerRaw(nd(n)) : nullptr; }

/// AFTER A REPARENT, A STATIC CHILD'S CACHED DERIVED TRANSFORMS ARE STALE (stage 2b's read, item a): Ogre refreshes a
/// static node only on a static dirty, and neither Node::setParent nor reconcileStatic issues one for a static child
/// that keeps its class under a new eligible parent — its local is kept, its parent changed, so its world moved.
/// One notifyStaticDirty at its depth (Ogre's own path) re-derives it and its subtree next frame.
static void settleAttached(Ogre::SceneNode *c)
{
    reconcileStatic(c);
    if (c->isStatic() && c->getCreator()) c->getCreator()->notifyStaticDirty(c);
}

void attach(NodeHandle parent, NodeHandle child, int index)
{
    if (!parent || !child || !engineAlive()) return;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *p = nd(parent);
    Ogre::SceneNode *c = nd(child);
    // A SOCKET RIDER is not in Ogre's hierarchy to move: its document place is
    // the store's, and its Ogre node stays on its bone. (Taking it off the
    // socket is a socket operation — Scene::detachFromSocket — not a reparent,
    // and the reconciler is what performs it.)
    if (isRiderNode(c)) return;
    // A REPARENT MOVES THE CHILD unless the caller re-adopts its world pose
    // (clean-2 lane review, F4): the local transform is kept and the parent
    // under it changes, so the whole subtree's world AABBs move without any
    // setter being called. The renderer's scans read that counter to decide
    // whether to look at all, so a reparent has to cost one scan — every live
    // caller today happens to preserve the pose, which is exactly the kind of
    // thing that stops being true silently.
    gTransformWrites.fetch_add(1, std::memory_order_relaxed);
    journalWrite(c);
    if (c->getParent()) c->getParent()->removeChild(c);
    // APPEND is the overwhelming majority (addChild passes -1) and must not pay
    // for the sibling-index machinery below: at a fan-out of k that scan is
    // O(k) per insert, i.e. quadratic in a node's child count over a whole
    // document build.
    if (index < 0 || std::size_t(index) >= p->numChildren()) {
        p->addChild(c);
        settleAttached(c);
        return;
    }

    // Ogre only appends, and its child vector also carries the ENGINE's own
    // children (a light's -Y adapter, a decal box, a range wire), which the
    // document cannot see. `index` is a DOCUMENT sibling index, so it is
    // resolved against owned children only — and the sibling index is real
    // semantics (the hierarchy panel shows it, DeleteSceneNodeCommand captures
    // it for undo — audit item 3).
    std::vector<Ogre::Node *> kids;
    kids.reserve(p->numChildren());
    for (std::size_t i = 0; i < p->numChildren(); ++i) kids.push_back(p->getChild(i));

    std::size_t insertAt = kids.size();
    int owned = 0;
    for (std::size_t i = 0; i < kids.size(); ++i) {
        if (!ownerRaw(static_cast<Ogre::SceneNode *>(kids[i]))) continue;
        if (owned == index) { insertAt = i; break; }
        ++owned;
    }
    if (insertAt >= kids.size()) { p->addChild(c); settleAttached(c); return; }

    for (Ogre::Node *k : kids) p->removeChild(k);
    kids.insert(kids.begin() + std::ptrdiff_t(insertAt), c);
    for (Ogre::Node *k : kids) p->addChild(k);
    settleAttached(c);
}

void followParent(NodeHandle parent, NodeHandle child)
{
    if (!parent || !child || !engineAlive()) return;
    if (nd(child)->getParent() == nd(parent)) return;
    attach(parent, child, -1);
}

NodeHandle detach(NodeHandle child)
{
    if (!child) return nullptr;
    if (!engineAlive()) return child;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *c = nd(child);
    // A rider taken out of the document hierarchy keeps riding its bone until
    // the reconciler says otherwise (its Ogre parent is the TagPoint, so the
    // re-home below would tear it off the socket).
    if (isRiderNode(c)) return child;
    gTransformWrites.fetch_add(1, std::memory_order_relaxed);   // see attach (F4)
    journalWrite(c);
    // Out of its parent and under its scene manager's root — NOT migrated to
    // the staging manager, which is what this used to do. A migration rebuilds
    // the whole subtree, which changes every handle in it, which makes the
    // mirror release and re-adopt every one of those nodes (and re-attach every
    // mesh and material) on the next sync. Measured on the §6 benchmark, that
    // turned a 500-node reparent — remove from one parent, add to another —
    // into two full subtree rebuilds plus 1000 re-adoptions, ~10% of
    // c.reparent_500 at 1k.
    //
    // What the migration bought was safety against "the engine scene is
    // destroyed while the undo stack still holds a node that was in it". That
    // is now iris::Scene's job instead: it remembers the subtrees detached from
    // it (Scene::rememberDetached) and takes them along when it unbinds.
    if (c->getParent()) c->getParent()->removeChild(c);
    Ogre::SceneNode *root = rootOf(c->getCreator());
    if (c != root) root->addChild(c);
    return child;
}

NodeHandle migrate(NodeHandle n, SceneHandle target, NodeHandle newParent)
{
    if (!n || !target) return nullptr;
    if (!engineAlive()) return n;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *src = nd(n);
    if (src->getCreator() == sm(target) && !newParent) return n;

    // Rebuild, do not move: an Ogre::SceneNode belongs to the SceneManager that
    // created it (mCreator decides who may destroy it and who creates its
    // children), and Node::setParent's cross-memory-manager migration moves
    // only the transform storage. Rebuilding keeps ONE owner per node.
    std::function<Ogre::SceneNode *(Ogre::SceneNode *, Ogre::SceneNode *)> copy =
        [&](Ogre::SceneNode *s, Ogre::SceneNode *parent) -> Ogre::SceneNode * {
        Ogre::SceneNode *dst = parent->createChildSceneNode(Ogre::SCENE_DYNAMIC);
        dst->setPosition(s->getPosition());
        setOrientationExact(dst, s->getOrientation());   // never Ogre's re-normalise: the store holds these bits
        dst->setScale(s->getScale());
        SceneNode *owner = ownerRaw(s);
        if (owner) {
            setOwnerById(dst->getId(), owner);
            owner->_setGraphNode(wrap(dst));
        }
        // Visibility is NOT copied: Ogre's setVisible walks a node's
        // ATTACHMENTS, and a document node in the staging manager has none. The
        // handle's own `visible` flag is the truth and the mirror re-applies it.
            // Engine-owned children (no document owner) are NOT copied: they belong
        // to the scene manager that is being left behind, and the engine drops
        // them on its own schedule.
        for (std::size_t i = 0; i < s->numChildren(); ++i) {
            Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(s->getChild(i));
            if (ownerRaw(c)) copy(c, dst);
        }
        // Static-ness is NOT carried here: `dst` was created under `parent`
        // and has already inherited its class (Node::setParent gives a child
        // its parent's memory manager). The caller reconciles the SUBTREE ROOT
        // with the document's hint once the whole copy exists — doing it per
        // node inside the recursion would migrate the same node twice.
        return dst;
    };

    Ogre::SceneNode *parent = newParent ? nd(newParent) : rootOf(sm(target));
    Ogre::SceneNode *dst = copy(src, parent);
    // The source subtree's owners have already been re-pointed at the copies,
    // so this drop must NOT clear them again — hence its own walk rather than
    // destroyRecursive. Engine-owned children are re-homed, never destroyed.
    std::function<void(Ogre::SceneNode *)> drop = [&](Ogre::SceneNode *s) {
        Ogre::SceneNode *sceneRoot = rootOf(s->getCreator());
        while (s->numChildren() > 0) {
            Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(s->getChild(0));
            if (ownerRaw(c)) {
                drop(c);
            } else {
                s->removeChild(c);
                if (c != sceneRoot) sceneRoot->addChild(c);
            }
        }
        setOwnerById(s->getId(), nullptr);
            if (s->isStatic() && gStaticNodes) --gStaticNodes;
        if (s->getParent()) s->getParent()->removeChild(s);
        s->getCreator()->destroySceneNode(s);
    };
    drop(src);
    reconcileStatic(dst);
    return wrap(dst);
}

// ---- transforms -----------------------------------------------------------

void setSocketRider(NodeHandle rider, bool on)
{
    if (!rider || !engineAlive()) return;
    SceneNode *o = ownerRaw(nd(rider));
    if (!o || o->docNode().rider == on) return;
    o->docNode().rider = on;
    // A rider's world (and every world under it) changes source: the caches must not answer from before.
    invalidateSubtree(o->docNode(), true);
}

bool isSocketRider(NodeHandle rider)
{
    return rider && engineAlive() && isRiderNode(nd(rider));
}

namespace
{
/// RULE 4 (nodegraph.h): MOVING A STATIC NODE PROMOTES IT.
///
/// The alternative — notifyStaticDirty on every write, which is what v1 did —
/// re-runs the whole static transform+bounds pass from that node's depth on
/// the next frame. For a node the user is DRAGGING that is one full static
/// pass per frame, i.e. exactly the cost SCENE_STATIC exists to avoid, paid
/// while the scene is least able to afford it. Demoting the node (and its
/// subtree, rule 1) makes the second write and every one after it free, and
/// the document's GRAPH hint is cleared with it so nothing puts it back. The
/// user's MOBILITY setting is NOT touched (REALTIME_REFLECTIONS_SPEC §3.3.3):
/// a drag is authoring, and rewriting the renderer's classification in the
/// middle of a gesture would cost a full GI rebuild.
///
/// Out of line and behind an `isStatic()` test that is false for almost every
/// node in almost every scene: this sits on the path of every transform write
/// in the program.
void promoteOnWrite(Ogre::SceneNode *n);
void promoteStaticChildren(Ogre::SceneNode *n);

/// IS THIS WRITE SCENE MOVEMENT? (nodegraph.h, the transform-write epoch.)
///
/// Everything is, except the VIEWER: a camera's transform cannot change
/// anything any consumer of the epoch scans — the GI items' world boxes, the
/// shadow casters, the forward-plus scene extent, the floor's world, the
/// skeleton share's world comparisons — and counting it made a camera ORBIT
/// re-run every one of those scans on every frame of the orbit (RR2: 10.28 ms
/// of `host.env` on 92% of flying frames on the 8,404-node lattice, against
/// 0.024 ms still).
///
/// A CAMERA WITH CHILDREN COUNTS AGAIN, and that is not a detail: moving it
/// moves whatever is parented under it, which is scene movement by any
/// definition.
///
/// DOCUMENT CHILDREN, NOT OGRE'S (round-2 review, item 2). The two trees are
/// one, and the ENGINE hangs its own nodes off document nodes: every non-view
/// camera with a visible body gets a wire node (SceneMirror::syncCameraWires),
/// a light gets its -Y adapter, a decal its projector box. Counting Ogre's
/// children therefore un-exempted every camera the mirror draws a body for —
/// the play-mode subject and every cinematic camera — which is exactly the
/// per-frame cost this exemption exists to remove. An engine child is not
/// scene content by construction: it carries no document node, the GI and
/// shadow scans do not read it (OgreScene::writeIsSceneMovement asks the same
/// question from the other side), and it moves only because we moved it.
///
/// The DOCUMENT children are the store's (DocNode::children): an Ogre child
/// with no document owner never appears there.
///
/// The flag itself lives on the document node (SceneNode::_countsAsMovement)
/// and is read through the back-pointer table — two loads, no hashing — rather
/// than in a second table of its own. A graph node with NO document owner (one
/// the engine made for itself) counts, like everything else.
inline bool writeIsSceneMovement(Ogre::SceneNode *n)
{
    const SceneNode *owner = ownerById(n->getId());
    if (!owner || owner->_countsAsMovement()) return true;
    return !owner->docNode().children.empty();   // a camera carrying DOCUMENT children moves them
}

inline void markMoved(Ogre::SceneNode *n)
{
    if (writeIsSceneMovement(n)) {
        gTransformWrites.fetch_add(1, std::memory_order_relaxed);
        journalWrite(n);
    }
    if (n->isStatic()) {
        // A RE-EXPRESSION (ReexpressScope) keeps the world pose: Ogre's static pass re-derives the subtree
        // once from the new parent, and nothing changes class (rule 4 is for a pose that moved).
        if (gReexpressDepth > 0) {
            if (n->getCreator()) n->getCreator()->notifyStaticDirty(n);
            return;
        }
        promoteOnWrite(n);
        return;
    }
    // The root-moved case (see promoteStaticChildren). Two loads, and only in a
    // process that has static nodes at all.
    if (gStaticNodes && isDocumentRoot(n)) promoteStaticChildren(n);
}

void promoteOnWrite(Ogre::SceneNode *n)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (SceneNode *owner = ownerById(n->getId())) owner->_clearStaticHint();
    applyStaticSubtree(n, false);
    // _clearStaticHint marks the owner (Flags): that mark is how the mirror of
    // its document learns a node left the static half and owes a settle.
}

/// THE ONE HOLE RULE 2 LEAVES, closed. A static node's parent must be static
/// OR be the document root, and the root is exempt because its transform is
/// identity and nothing writes it. "Nothing writes it" is a claim, not a
/// mechanism — so if something DOES write it, every static node under it would
/// freeze at the root's old world position. This promotes them instead.
///
/// It runs only for a node that is itself a document root (two loads to tell)
/// in a process that has static nodes at all (one load), so the ordinary
/// transform write — a child, in a scene with or without statics — pays a
/// predictable compare and nothing else.
void promoteStaticChildren(Ogre::SceneNode *n)
{
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    bool any = false;
    for (std::size_t i = 0; i < n->numChildren(); ++i) {
        Ogre::SceneNode *c = static_cast<Ogre::SceneNode *>(n->getChild(i));
        if (!c->isStatic()) continue;
        if (SceneNode *owner = ownerById(c->getId())) owner->_clearStaticHint();
        applyStaticSubtree(c, false);
        any = true;
    }
    if (any)
        qWarning("iris::graph: the scene's ROOT node was moved — every static subtree under it "
                 "has been promoted back to dynamic, because a static node cannot follow a "
                 "moving parent (SCENEGRAPH_SPEC §6 rule 2).");
}
}  // namespace


unsigned long long transformWrites()
{
    return gTransformWrites.load(std::memory_order_relaxed);
}

WriteJournalReader::WriteJournalReader(bool wantsWritten) : wantsWritten(wantsWritten)
{
    if (wantsWritten) gJournalWrittenReaders.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(gJournalMutex);
    cursor = gJournalNextSeq;
    gJournalMaxCursor = std::max(gJournalMaxCursor, cursor);
    gJournalReaders.push_back(this);
}

WriteJournalReader::~WriteJournalReader()
{
    if (wantsWritten) gJournalWrittenReaders.fetch_sub(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(gJournalMutex);
    gJournalReaders.erase(std::remove(gJournalReaders.begin(), gJournalReaders.end(), this), gJournalReaders.end());
    gJournalMaxCursor = 0;
    for (const WriteJournalReader *r : gJournalReaders) gJournalMaxCursor = std::max(gJournalMaxCursor, r->cursor);
    if (gJournalReaders.empty()) { gJournal.clear(); gJournalNewest.clear(); }
    else journalTrimLocked();
}

bool WriteJournalReader::take(std::vector<NodeHandle> &out)
{
    out.clear();
    std::vector<Ogre::IdType> ids;
    bool complete = primed;
    {
        std::lock_guard<std::mutex> lock(gJournalMutex);
        // The cap dropped entries this reader had not read: what is left is not every write.
        if (cursor < gJournalCapFloor) complete = false;
        for (const JournalEntry &e : gJournal) {
            if (e.seq < cursor || !e.moved) continue;
            if (!e.owned) { complete = false; continue; }   // engine-owned: no way back to it
            ids.push_back(e.id);
        }
        cursor = gJournalNextSeq;
        primed = true;
        gJournalMaxCursor = std::max(gJournalMaxCursor, cursor);
        journalTrimLocked();
    }
    // Resolved outside the journal's lock (the owner table has its own discipline); each node once.
    std::unordered_set<Ogre::IdType> seen;
    out.reserve(ids.size());
    for (Ogre::IdType id : ids) {
        if (!seen.insert(id).second) continue;
        SceneNode *owner = ownerById(id);
        if (!owner || !owner->graphNode()) continue;          // destroyed since its write
        out.push_back(owner->graphNode());
    }
    return complete;
}

void noteDocumentWrite(NodeHandle n)
{
    if (!n || gJournalWrittenReaders.load(std::memory_order_relaxed) == 0) return;
    const Ogre::IdType id = nd(n)->getId();
    std::lock_guard<std::mutex> lock(gJournalMutex);
    journalLogLocked(id, false);
}

bool WriteJournalReader::take(std::vector<NodeHandle> &moved, std::vector<NodeHandle> &written, int *gone)
{
    moved.clear();
    written.clear();
    if (gone) *gone = 0;
    std::vector<std::pair<Ogre::IdType, bool>> ids;
    bool complete = primed;
    {
        std::lock_guard<std::mutex> lock(gJournalMutex);
        if (cursor < gJournalCapFloor) complete = false;
        for (const JournalEntry &e : gJournal) {
            if (e.seq < cursor) continue;
            if (!e.owned) { complete = false; continue; }
            ids.emplace_back(e.id, e.moved);
        }
        cursor = gJournalNextSeq;
        primed = true;
        gJournalMaxCursor = std::max(gJournalMaxCursor, cursor);
        journalTrimLocked();
    }
    // A node both moved and written is MOVED (its subtree is re-read anyway); each node once.
    std::unordered_map<Ogre::IdType, bool> kind;
    for (const auto &e : ids) { bool &m = kind[e.first]; m = m || e.second; }
    for (const auto &e : ids) {
        auto it = kind.find(e.first);
        if (it == kind.end()) continue;              // already answered
        const bool isMoved = it->second;
        kind.erase(it);
        SceneNode *owner = ownerById(e.first);
        if (!owner || !owner->graphNode()) { if (gone) ++*gone; continue; }   // destroyed since its write
        (isMoved ? moved : written).push_back(owner->graphNode());
    }
    return complete;
}

void WriteJournalReader::skip()
{
    std::lock_guard<std::mutex> lock(gJournalMutex);
    cursor = gJournalNextSeq;
    primed = true;
    gJournalMaxCursor = std::max(gJournalMaxCursor, cursor);
    journalTrimLocked();
}

std::size_t writeJournalSize()
{
    std::lock_guard<std::mutex> lock(gJournalMutex);
    return gJournal.size();
}

void setCountsAsMovement(NodeHandle n, bool counts)
{
    if (!n) return;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    if (SceneNode *owner = ownerById(nd(n)->getId())) owner->_setCountsAsMovement(counts);
}

bool countsAsMovement(NodeHandle n)
{
    if (!n) return true;
    const SceneNode *owner = ownerById(nd(n)->getId());
    return !owner || owner->_countsAsMovement();
}

unsigned long long staticSwitchCount()
{
    return gStaticSwitches.load(std::memory_order_relaxed);
}

ReexpressScope::ReexpressScope() { ++gReexpressDepth; }
ReexpressScope::~ReexpressScope() { --gReexpressDepth; }

// ---- THE DOCUMENT'S TRANSFORM STORE (nodegraph.h, ENGINE-UPGRADE-U2 stage 1) --------------------------------

namespace
{
// OGRE'S OWN FLOAT MATHS, written out rather than called: the store's answers must not move a bit when stage 2
// takes Ogre out of this file. The two the WORLD is composed with are the forms Node::updateAllTransforms runs
// (Math/Array/SSE2/Single/OgreArrayQuaternionSSE2.inl), in the same association. Agreement is to float rounding,
// not to the bit: Ogre's own two paths (the per-frame batch and the per-node `_get…Updated`) already differ from
// each other by a few ulps on the same inputs (measured, ENGINE-UPGRADE-U2; mirror.doc_store holds the store to
// 1e-6 of the engine).

/// ArrayQuaternion * ArrayQuaternion — the Hamilton product, Ogre's grouping.
inline Quat qmul(const Quat &a, const Quat &b)
{
    const float aw = a.scalar(), ax = a.x(), ay = a.y(), az = a.z();
    const float bw = b.scalar(), bx = b.x(), by = b.y(), bz = b.z();
    return Quat((aw * bw - ax * bx) - (ay * by + az * bz),
                (aw * bx + ax * bw) + (ay * bz - az * by),
                (aw * by + ay * bw) + (az * bx - ax * bz),
                (aw * bz + az * bw) + (ax * by - ay * bx));
}

/// ArrayQuaternion * ArrayVector3 — the nVidia SDK form: v + ((q x v) * 2w + (q x (q x v)) * 2).
inline Vec3 qrot(const Quat &q, const Vec3 &v)
{
    const float qx = q.x(), qy = q.y(), qz = q.z(), w2 = q.scalar() + q.scalar();
    const float vx = v.x(), vy = v.y(), vz = v.z();
    float ux = qy * vz - qz * vy, uy = qz * vx - qx * vz, uz = qx * vy - qy * vx;      // uv = q x v
    float uux = qy * uz - qz * uy, uuy = qz * ux - qx * uz, uuz = qx * uy - qy * ux;  // uuv = q x uv
    ux *= w2; uy *= w2; uz *= w2;
    uux += uux; uuy += uuy; uuz += uuz;
    return Vec3(vx + (ux + uux), vy + (uy + uuy), vz + (uz + uuz));
}

/// Quaternion::Inverse().
inline Quat qinv(const Quat &q)
{
    const float w = q.scalar(), x = q.x(), y = q.y(), z = q.z();
    const float norm = w * w + x * x + y * y + z * z;
    if (!(norm > 0.0f)) return Quat(0.0f, 0.0f, 0.0f, 0.0f);
    const float inv = 1.0f / norm;
    return Quat(w * inv, -x * inv, -y * inv, -z * inv);
}

/// WHAT A ROTATION WRITE STORES: `q` BIT-EXACT when it is already unit length (to float precision), and
/// Quaternion::normalise() of it only when it genuinely is not.
///
/// Ogre::Node::setOrientation re-normalises UNCONDITIONALLY (OgreNode.cpp:572), and float normalisation is not
/// idempotent: n(q) != q for many unit q, and n(n(q)) is often q again — the stored quaternion walks between two
/// float fixed points on every write of its own value. That made a play/stop cycle change the document by one
/// normalisation step (PlayBack::restoreNodeTransforms writes back exactly what it saved), which is how the
/// fixed-clock determinism suite found two IDENTICAL 120-frame physics runs landing tumbling bodies on different
/// bits (ENGINEERING_DEBT_SPEC A4.2). A restore, an undo, a deserialise and the per-step physics write-back all
/// pass through here and must leave the node holding exactly the value they were given.
///
/// The tolerance is on the SQUARED norm: 1e-5 is ~1e3 float ulps around 1 — far wider than any product of unit
/// quaternions drifts, far narrower than a quaternion that was never unit (a lossy matrix decomposition of a
/// scaled transform, hand-authored data). Those are normalised once, after which they pass through exactly — and
/// the Ogre half (setOrientationExact) then stores this same value bit for bit.
inline Quat canonicalRotation(const Quat &q)
{
    const float w = q.scalar(), x = q.x(), y = q.y(), z = q.z();
    const float norm = w * w + x * x + y * y + z * z;
    if (std::fabs(norm - 1.0f) <= 1e-5f) return q;
    const float factor = 1.0f / std::sqrt(norm);
    return Quat(w * factor, x * factor, y * factor, z * factor);
}

/// Matrix4::makeTransform(position, scale, orientation) with Quaternion::ToRotationMatrix inside: scale, then
/// rotate, then translate. iris::Mat4's 16-argument constructor reads row-major, as Ogre::Matrix4 stores.
inline Mat4 makeTransform(const Vec3 &p, const Vec3 &s, const Quat &q)
{
    const float x = q.x(), y = q.y(), z = q.z(), w = q.scalar();
    const float tx = x + x, ty = y + y, tz = z + z;
    const float twx = tx * w, twy = ty * w, twz = tz * w;
    const float txx = tx * x, txy = ty * x, txz = tz * x;
    const float tyy = ty * y, tyz = tz * y, tzz = tz * z;
    const float r00 = 1.0f - (tyy + tzz), r01 = txy - twz, r02 = txz + twy;
    const float r10 = txy + twz, r11 = 1.0f - (txx + tzz), r12 = tyz - twx;
    const float r20 = txz - twy, r21 = tyz + twx, r22 = 1.0f - (txx + tyy);
    return Mat4(s.x() * r00, s.y() * r01, s.z() * r02, p.x(),
                s.x() * r10, s.y() * r11, s.z() * r12, p.y(),
                s.x() * r20, s.y() * r21, s.z() * r22, p.z(),
                0.0f, 0.0f, 0.0f, 1.0f);
}

inline bool isIdentityTrs(const Vec3 &p, const Quat &r, const Vec3 &s)
{
    return p.x() == 0.0f && p.y() == 0.0f && p.z() == 0.0f &&
           r.scalar() == 1.0f && r.x() == 0.0f && r.y() == 0.0f && r.z() == 0.0f &&
           s.x() == 1.0f && s.y() == 1.0f && s.z() == 1.0f;
}


/// THE WORLD, DERIVED (nodegraph.h): Ogre's composition rule down the store's parent chain, each node cached
/// until a write invalidates it. Returns whether the answer may be cached — false for a socket rider and everything under one
/// (their world is the engine's, read from the pose the last rendered frame produced: a TagPoint cannot be
/// resolved on demand, TagPoint::updateFromParentImpl is a plain assert(false)).
bool deriveWorld(const DocNode &n, Vec3 &P, Quat &R, Vec3 &S)
{
    if (n.rider) {
        const NodeHandle h = n.owner ? n.owner->graphNode() : nullptr;
        if (h && engineAlive()) {
            const Ogre::SceneNode *o = nd(h);
            P = toIris(o->_getDerivedPosition());
            R = toIris(o->_getDerivedOrientation());
            S = toIris(o->_getDerivedScale());
        } else {
            P = n.pos; R = n.rot; S = n.scale;
        }
        return false;
    }
    if (n.worldValid) {
        P = n.worldPos; R = n.worldRot; S = n.worldScale;
        return true;
    }
    bool cacheable = true;
    if (!n.parent) {
        P = n.pos; R = n.rot; S = n.scale;
    } else {
        Vec3 pP, pS;
        Quat pR;
        cacheable = deriveWorld(*n.parent, pP, pR, pS);
        // OgreNode.cpp updateFromParentImpl (OGRE_NODE_INHERIT_TRANSFORM = 0): S = pS*S, R = pR*R,
        // P = pR*(pS*P) + pP — the scale per axis, never a shear.
        S = pS * n.scale;
        R = qmul(pR, n.rot);
        P = qrot(pR, pS * n.pos) + pP;
    }
    if (cacheable) {
        n.worldPos = P; n.worldRot = R; n.worldScale = S;
        n.worldValid = true;
    }
    return cacheable;
}

/// The parent's world (identity for a root), and whether it IS identity — the fast path every re-expression takes
/// for a child of the document root, which writes the world values straight through, bit for bit.
inline bool parentWorld(const DocNode &n, Vec3 &P, Quat &R, Vec3 &S)
{
    if (!n.parent) {
        P = Vec3(0.0f, 0.0f, 0.0f); R = Quat(); S = Vec3(1.0f, 1.0f, 1.0f);
        return true;
    }
    deriveWorld(*n.parent, P, R, S);
    return isIdentityTrs(P, R, S);
}

/// The local position that puts `n` at world `v` under a parent at (pP, pR, pS): (pR^-1 (v - pP)) / pS, an axis of
/// ZERO parent scale keeping the node's own local value (nodegraph.h, SC1-REEXPRESS-SCALE-1).
inline Vec3 localPosFrom(const DocNode &n, const Vec3 &v, const Vec3 &pP, const Quat &pRinv, const Vec3 &pS)
{
    const Vec3 d = qrot(pRinv, v - pP);
    return Vec3(pS.x() != 0.0f ? d.x() / pS.x() : n.pos.x(),
                pS.y() != 0.0f ? d.y() / pS.y() : n.pos.y(),
                pS.z() != 0.0f ? d.z() / pS.z() : n.pos.z());
}

/// THE OGRE HALF of a local write (stage 1 only): the node's own Ogre node gets exactly the store's values, then
/// the movement epoch, the journal and SCENE_STATIC rule 4 (markMoved) as every transform write always paid.
inline Ogre::SceneNode *ogreHalf(const DocNode &n)
{
    const NodeHandle h = n.owner ? n.owner->graphNode() : nullptr;
    return (h && engineAlive()) ? nd(h) : nullptr;
}
}  // namespace

void attachDoc(DocNode &parent, DocNode &child, int index)
{
    if (&parent == &child) return;
    if (child.parent) {
        auto &old = child.parent->children;
        old.erase(std::remove(old.begin(), old.end(), &child), old.end());
    }
    auto &kids = parent.children;
    if (index < 0 || std::size_t(index) >= kids.size()) kids.push_back(&child);
    else kids.insert(kids.begin() + std::ptrdiff_t(index), &child);
    child.parent = &parent;
    invalidateSubtree(child);
}

void detachDoc(DocNode &child)
{
    if (!child.parent) return;
    auto &old = child.parent->children;
    old.erase(std::remove(old.begin(), old.end(), &child), old.end());
    child.parent = nullptr;
    invalidateSubtree(child);
}

void unlinkDoc(DocNode &n)
{
    detachDoc(n);
    for (DocNode *c : n.children) {
        c->parent = nullptr;
        invalidateSubtree(*c);
    }
    n.children.clear();
}

int indexInParent(const DocNode &n)
{
    if (!n.parent) return -1;
    const auto &kids = n.parent->children;
    const auto it = std::find(kids.begin(), kids.end(), &n);
    return it == kids.end() ? -1 : int(it - kids.begin());
}

void setLocalPos(DocNode &n, const Vec3 &v)
{
    n.pos = v;
    invalidateSubtree(n);
    if (Ogre::SceneNode *o = ogreHalf(n)) {
        o->setPosition(toOgre(v));
        markMoved(o);
    }
}

void setLocalRot(DocNode &n, const Quat &q)
{
    n.rot = canonicalRotation(q);
    invalidateSubtree(n);
    if (Ogre::SceneNode *o = ogreHalf(n)) {
        setOrientationExact(o, toOgre(n.rot));
        markMoved(o);
    }
}

void setLocalScale(DocNode &n, const Vec3 &v)
{
    n.scale = v;
    invalidateSubtree(n);
    if (Ogre::SceneNode *o = ogreHalf(n)) {
        o->setScale(toOgre(v));
        markMoved(o);
    }
}

void setLocalTrs(DocNode &n, const Vec3 &p, const Quat &r, const Vec3 &s)
{
    n.pos = p;
    n.rot = canonicalRotation(r);
    n.scale = s;
    invalidateSubtree(n);
    if (Ogre::SceneNode *o = ogreHalf(n)) {
        o->setPosition(toOgre(p));
        setOrientationExact(o, toOgre(n.rot));
        o->setScale(toOgre(s));
        markMoved(o);
    }
}

Mat4 localTransform(const DocNode &n) { return makeTransform(n.pos, n.scale, n.rot); }

Trs world(const DocNode &n)
{
    Trs w;
    deriveWorld(n, w.pos, w.rot, w.scale);
    return w;
}

Mat4 globalTransform(const DocNode &n)
{
    const Trs w = world(n);
    return makeTransform(w.pos, w.scale, w.rot);
}

Trs localFromWorld(const DocNode &n, const Trs &w)
{
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) return w;            // under identity: the world IS the local, bit for bit
    const Quat inv = qinv(pR);
    Trs l;
    l.rot = qmul(inv, w.rot);
    l.pos = localPosFrom(n, w.pos, pP, inv, pS);
    l.scale = Vec3(pS.x() != 0.0f ? w.scale.x() / pS.x() : n.scale.x(),
                   pS.y() != 0.0f ? w.scale.y() / pS.y() : n.scale.y(),
                   pS.z() != 0.0f ? w.scale.z() / pS.z() : n.scale.z());
    return l;
}

void setGlobalPos(DocNode &n, const Vec3 &v)
{
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) { setLocalPos(n, v); return; }
    setLocalPos(n, localPosFrom(n, v, pP, qinv(pR), pS));
}

void setGlobalRot(DocNode &n, const Quat &q)
{
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) { setLocalRot(n, q); return; }
    setLocalRot(n, qmul(qinv(pR), q));
}

void setGlobalPosRot(DocNode &n, const Vec3 &v, const Quat &q)
{
    // ONE resolution of the parent, for both halves — and none of the inverse when the parent is at identity (a
    // physics body's parent is the document root, identity by construction: nodegraph.h rule 2's exemption).
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) { setLocalTrs(n, v, q, n.scale); return; }
    const Quat inv = qinv(pR);
    setLocalTrs(n, localPosFrom(n, v, pP, inv, pS), qmul(inv, q), n.scale);
}

void setGlobalTrs(DocNode &n, const Trs &w)
{
    const Trs l = localFromWorld(n, w);
    setLocalTrs(n, l.pos, l.rot, l.scale);
}

void setGlobalTransform(DocNode &n, const Mat4 &m)
{
    Trs w;
    iris::decomposeTRS(m, w.pos, w.rot, w.scale);
    setGlobalTrs(n, w);
}

bool worldBox(const DocNode &n, const Vec3 &lmin, const Vec3 &lmax, float outMin[3], float outMax[3])
{
    const float from[6] = { lmin.x(), lmin.y(), lmin.z(), lmax.x(), lmax.y(), lmax.z() };
    if (!(from[0] <= from[3] && from[1] <= from[4] && from[2] <= from[5])) return false;   // empty
    // THE HOT PATH (a pick visits every mesh of the scene): plain floats, one compare of the local box.
    if (n.boxValid && std::memcmp(n.boxFrom, from, sizeof from) == 0) {
        std::memcpy(outMin, n.box, 3 * sizeof(float));
        std::memcpy(outMax, n.box + 3, 3 * sizeof(float));
        return true;
    }
    Vec3 P, S;
    Quat R;
    const bool cacheable = deriveWorld(n, P, R, S);
    const Mat4 m = makeTransform(P, S, R);
    const float c[3] = { (from[0] + from[3]) * 0.5f, (from[1] + from[4]) * 0.5f, (from[2] + from[5]) * 0.5f };
    const float h[3] = { (from[3] - from[0]) * 0.5f, (from[4] - from[1]) * 0.5f, (from[5] - from[2]) * 0.5f };
    for (int r = 0; r < 3; ++r) {
        const float wc = m(r, 0) * c[0] + m(r, 1) * c[1] + m(r, 2) * c[2] + m(r, 3);
        const float wh = std::fabs(m(r, 0)) * h[0] + std::fabs(m(r, 1)) * h[1] + std::fabs(m(r, 2)) * h[2];
        // The pad: rounding in the triangle path's own inverse transform is relative to the box's size and to its
        // distance from the origin, so the pad is too (plus an absolute floor for a box at the origin).
        const float pad = 1e-5f * (wh + std::fabs(wc)) + 1e-6f;
        outMin[r] = wc - wh - pad;
        outMax[r] = wc + wh + pad;
    }
    if (cacheable) {
        std::memcpy(n.boxFrom, from, sizeof from);
        std::memcpy(n.box, outMin, 3 * sizeof(float));
        std::memcpy(n.box + 3, outMax, 3 * sizeof(float));
        n.boxValid = true;
    }
    return true;
}

void invalidatePick(DocNode &n)
{
    n.boxValid = false;
    if (n.pickTable) n.pickTable->markStale(n.pickSlot);
}

void PickTable::add(SceneNode *meshNode)
{
    if (!meshNode) return;
    DocNode &d = meshNode->docNode();
    if (d.pickTable == this) return;
    if (d.pickTable) d.pickTable->remove(meshNode);
    std::size_t slot;
    if (!mFree.empty()) {
        slot = std::size_t(mFree.back());
        mFree.pop_back();
    } else {
        slot = mNode.size();
        mNode.push_back(nullptr);
        mState.push_back(Stale);
        mBox.resize(mBox.size() + 6, 0.0f);
    }
    mNode[slot] = meshNode;
    mState[slot] = Stale;
    d.pickTable = this;
    d.pickSlot = int(slot);
}

void PickTable::remove(SceneNode *meshNode)
{
    if (!meshNode) return;
    DocNode &d = meshNode->docNode();
    if (d.pickTable != this || d.pickSlot < 0) return;
    const std::size_t slot = std::size_t(d.pickSlot);
    mNode[slot] = nullptr;
    mState[slot] = Stale;
    mFree.push_back(int(slot));
    d.pickTable = nullptr;
    d.pickSlot = -1;
}

void PickTable::clear()
{
    for (SceneNode *n : mNode)
        if (n) { n->docNode().pickTable = nullptr; n->docNode().pickSlot = -1; }
    mNode.clear();
    mState.clear();
    mBox.clear();
    mFree.clear();
}

bool PickTable::refresh(std::size_t slot, const Vec3 &localMin, const Vec3 &localMax)
{
    SceneNode *n = mNode[slot];
    if (!n) return false;
    float *b = &mBox[6 * slot];
    if (!worldBox(n->docNode(), localMin, localMax, b, b + 3)) return false;
    // worldBox cached the box only if the world could be cached; the slot follows it exactly (a rider's box is
    // recomputed on every pick, as its world is).
    mState[slot] = n->docNode().boxValid ? Valid : Stale;
    return true;
}

bool PoseWitness::operator==(const PoseWitness &o) const
{
    // BITS, not floats: -0 against +0 and a NaN against itself are differences
    // a write could make, so `==` on the values would be the wrong question.
    return parent == o.parent && std::memcmp(values, o.values, sizeof values) == 0;
}

PoseWitness poseWitness(const DocNode &n)
{
    PoseWitness w;
    float *v = w.values;
    // The parent half: what setGlobalPosRot resolves (the store's parent world, so a parent moved since the last
    // write shows up here).
    if (n.parent) {
        w.parent = n.parent;
        const Trs p = world(*n.parent);
        v[0] = p.pos.x(); v[1] = p.pos.y(); v[2] = p.pos.z();
        v[3] = p.rot.scalar(); v[4] = p.rot.x(); v[5] = p.rot.y(); v[6] = p.rot.z();
        v[7] = p.scale.x(); v[8] = p.scale.y(); v[9] = p.scale.z();
    }
    // The node half: what setGlobalPosRot writes.
    v[10] = n.pos.x(); v[11] = n.pos.y(); v[12] = n.pos.z();
    v[13] = n.rot.scalar(); v[14] = n.rot.x(); v[15] = n.rot.y(); v[16] = n.rot.z();
    return w;
}

// ---- flags ----------------------------------------------------------------

void setVisible(NodeHandle n, bool visible, bool cascade)
{
    if (!n || !engineAlive()) return;
    nd(n)->setVisible(visible, cascade);
}

bool isStatic(NodeHandle n) { return n && engineAlive() && nd(n)->isStatic(); }

bool canBeStatic(NodeHandle n)
{
    if (!n || !engineAlive()) return false;
    return eligibleStaticParent(nd(n)->getParent());
}

bool setStatic(NodeHandle n, bool value)
{
    if (!n || !engineAlive()) return false;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *o = nd(n);
    if (o->isStatic() == value) return true;                    // already there
    if (value && !eligibleStaticParent(o->getParent())) {
        qWarning("iris::graph::setStatic(true) refused: a static node's parent must be static "
                 "or the document root (SCENEGRAPH_SPEC §6 rule 2).");
        return false;
    }
    return applyStaticSubtree(o, value);
}

bool promoteNode(NodeHandle n, bool carry, bool *switched)
{
    if (switched) *switched = false;
    if (!n || !engineAlive()) return false;
    std::lock_guard<std::recursive_mutex> lock(graphMutex());
    Ogre::SceneNode *o = nd(n);
    bool did = false;
    if (!o->isStatic()) {
        if (!eligibleStaticParent(o->getParent())) {
            qWarning("iris::graph::promoteNode refused: a static node's parent must be static "
                     "or the document root (SCENEGRAPH_SPEC §6 rule 2).");
            return false;
        }
        if (!eligibleForStatic(o) || !switchOne(o, true)) return false;
        ++gStaticNodes;
        did = true;
    }
    if (switched) *switched = did;
    // The ENGINE-OWNED children, exactly where the subtree switch reaches
    // them: applyStaticSubtree descends from the node it switched through
    // every eligible node below, already-static ones included, and an engine
    // node answers eligibleForStatic with yes. So they go when THIS node
    // switched, or when an ancestor's switch would have walked down to here
    // (`carry`) — and never on a node nobody switched, which setStatic's
    // "already there" early return leaves alone too.
    if (did || carry) {
        for (std::size_t i = 0; i < o->numChildren(); ++i) {
            auto *c = static_cast<Ogre::SceneNode *>(o->getChild(i));
            if (!ownerById(c->getId())) applyStaticSubtree(c, true);
        }
    }
    return true;
}

std::size_t staticNodeCount() { return gStaticNodes; }



}  // namespace graph
}  // namespace iris
