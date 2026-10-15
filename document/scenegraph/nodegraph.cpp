/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

// THE DOCUMENT'S SCENE GRAPH (nodegraph.h): the transform store, the write journal and the graph class. No Ogre
// header: the engine owns its own nodes since ENGINE-UPGRADE-U2 and hears of the document's writes through the frame
// record (SceneMirror's extract).

#include "document/scenegraph/nodegraph.h"
#include "document/scenegraph/scenenode.h"
#include "core/math/trs.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QDebug>

namespace iris
{
namespace graph
{

namespace
{

// ---------------------------------------------------------------------------
// THE OWNER TABLE — SceneNode::getNodeId() -> the live node, for the journal.
//
// The journal keeps IDS, never pointers: a node can die between its write and a reader's take, and an id resolves
// through this table, which a node's destructor clears, so a dead node simply resolves to nothing. A chunked flat
// array (two loads a lookup, no hashing); writes take a mutex, reads take nothing and are safe against a concurrent
// write because the chunk POINTERS are atomic and a chunk, once published, is never moved or freed. Ids are the
// document's own process-wide monotonic counter (SceneNode::generateNodeId), so they are dense from 1.
constexpr std::size_t kOwnerChunkShift = 13;                     // 8192 ids / chunk
constexpr std::size_t kOwnerChunkSize = std::size_t(1) << kOwnerChunkShift;
constexpr std::size_t kOwnerChunkCount = 65536;                  // 536 M ids
std::atomic<SceneNode **> gOwnerChunks[kOwnerChunkCount];
std::mutex gOwnerMutex;

inline SceneNode *ownerById(long long id)
{
    if (id < 0) return nullptr;
    const std::size_t chunk = std::size_t(id) >> kOwnerChunkShift;
    if (chunk >= kOwnerChunkCount) return nullptr;
    SceneNode **entries = gOwnerChunks[chunk].load(std::memory_order_acquire);
    return entries ? entries[std::size_t(id) & (kOwnerChunkSize - 1)] : nullptr;
}

void setOwnerById(long long id, SceneNode *owner)
{
    if (id < 0) return;
    const std::size_t chunk = std::size_t(id) >> kOwnerChunkShift;
    if (chunk >= kOwnerChunkCount) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            qWarning("iris::graph: the document node id space is exhausted (id %lld) — nodes created from here on "
                     "are not journaled.", id);
        }
        return;
    }
    std::lock_guard<std::mutex> lock(gOwnerMutex);
    SceneNode **entries = gOwnerChunks[chunk].load(std::memory_order_relaxed);
    if (!entries) {
        entries = new SceneNode *[kOwnerChunkSize]();
        gOwnerChunks[chunk].store(entries, std::memory_order_release);
    }
    entries[std::size_t(id) & (kOwnerChunkSize - 1)] = owner;
}

/// THE WRITE JOURNAL (see graph::WriteJournalReader): one log of the nodes written, in write order, BY DOCUMENT ID.
/// Each entry carries its sequence number; a reader's cursor is the first sequence it has not read. An entry is dropped
/// once every live reader's cursor is past it, and a write is logged only if some reader would not otherwise see that
/// node again (its newest entry of that kind is behind the furthest cursor) — so a node dragged for a minute is one
/// entry per reader-take, not one per write.
/// KINDS: Moved — a transform write that is scene movement, a reparent, a detach (the node's world and its subtree's
/// may have changed); Posed — a transform write that is NOT scene movement (a camera's), logged only while a reader
/// that asked for poses lives; Written — any other document write (SceneNode::notifyChanged), logged only while a
/// reader that asked for it lives.
enum class Kind : unsigned char { Moved = 0, Posed = 1, Written = 2 };
struct JournalEntry { long long id = 0; Kind kind = Kind::Moved; unsigned long long seq = 0; };
inline unsigned long long journalKey(long long id, Kind k) { return (static_cast<unsigned long long>(id) << 2) | unsigned(k); }
std::mutex gJournalMutex;
std::deque<JournalEntry> gJournal;
std::unordered_map<unsigned long long, unsigned long long> gJournalNewest;   ///< (node, kind) -> its newest entry's seq
std::atomic<int> gJournalWrittenReaders{0};   ///< readers that asked for the WRITTEN half (noteDocumentWrite)
std::atomic<int> gJournalPosedReaders{0};     ///< readers that asked for the POSED half (takePoses)
std::vector<graph::WriteJournalReader *> gJournalReaders;
unsigned long long gJournalNextSeq = 1;
unsigned long long gJournalMaxCursor = 0;   ///< the furthest reader's cursor (the dedupe's bar)
unsigned long long gJournalCapFloor = 0;    ///< every seq below this one that the CAP dropped (not the readers)
/// THE CAP: a reader that stops taking must not grow the log for ever. Past the cap the oldest entries go and a reader
/// behind them gets an incomplete take — its full pass — which is the honest answer, never a silent miss.
constexpr std::size_t kJournalCap = std::size_t(1) << 16;

inline void journalTrimLocked()
{
    unsigned long long minCursor = gJournalNextSeq;
    for (const graph::WriteJournalReader *r : gJournalReaders) minCursor = std::min(minCursor, r->cursor);
    while (!gJournal.empty() && (gJournal.front().seq < minCursor || gJournal.size() > kJournalCap)) {
        const JournalEntry &e = gJournal.front();
        if (e.seq >= minCursor) gJournalCapFloor = e.seq + 1;   // a reader still owed it
        auto it = gJournalNewest.find(journalKey(e.id, e.kind));
        if (it != gJournalNewest.end() && it->second == e.seq) gJournalNewest.erase(it);
        gJournal.pop_front();
    }
}

/// One entry, the dedupe and the cap (the caller holds gJournalMutex).
inline void journalLogLocked(long long id, Kind kind)
{
    const unsigned long long key = journalKey(id, kind);
    auto it = gJournalNewest.find(key);
    if (it != gJournalNewest.end() && it->second >= gJournalMaxCursor) return;   // every reader still sees that one
    const unsigned long long seq = gJournalNextSeq++;
    gJournal.push_back({ id, kind, seq });
    gJournalNewest[key] = seq;
    if (gJournal.size() > kJournalCap) journalTrimLocked();
}

inline void journalWrite(SceneNode *n, Kind kind)
{
    if (!n) return;
    if (kind == Kind::Posed && gJournalPosedReaders.load(std::memory_order_relaxed) == 0) return;
    const long long id = n->getNodeId();
    std::lock_guard<std::mutex> lock(gJournalMutex);
    if (gJournalReaders.empty()) return;          // nobody will ever read it
    journalLogLocked(id, kind);
}

/// See graph::transformWrites(). Relaxed: a CHANGE TEST, read once a frame.
std::atomic<unsigned long long> gTransformWrites{0};

// ---- SCENE_STATIC: the graph class (nodegraph.h) --------------------------

std::atomic<unsigned long long> gStaticSwitches{0};
std::atomic<long long> gStaticNodes{0};
/// ReexpressScope's depth on this thread (graph writes happen on the document's thread).
thread_local int gReexpressDepth = 0;

/// Rule 2: a static node's parent is static, or a scene root (a node with no parent).
inline bool eligibleStaticParent(const DocNode *p)
{
    if (!p) return false;                   // detached / a root itself: nothing anchors it
    return p->graphStatic || !p->parent;
}

/// One node's class, changed and counted. Returns whether it changed. The caller tells the TOP of each switched region
/// (the mirror walks the region under it: SceneMirror::noteGraphClass) — a subtree switch marks one node, never its
/// subtree (the change list's rule: a moved group marks the group).
inline bool switchOne(DocNode &n, bool value)
{
    if (n.graphStatic == value) return false;
    n.graphStatic = value;
    gStaticSwitches.fetch_add(1, std::memory_order_relaxed);
    gStaticNodes.fetch_add(value ? 1 : -1, std::memory_order_relaxed);
    return true;
}

/// Rule 1: static is a SUBTREE property. SKIP, DO NOT VETO: a branch that cannot be static (a light inside an imported
/// model, an animated node) is left dynamic together with everything under it, and the REST of the subtree still goes
/// static. Demotion never fails.
void applyStaticSubtree(DocNode &n, bool value)
{
    std::vector<std::pair<DocNode *, bool>> stack{ { &n, false } };   // (node, its parent switched here)
    while (!stack.empty()) {
        const auto [cur, parentSwitched] = stack.back();
        stack.pop_back();
        if (value && cur->owner && !cur->owner->isStaticEligible()) continue;   // skip it and its branch
        const bool switched = switchOne(*cur, value);
        if (switched && !parentSwitched && cur->owner) cur->owner->_noteGraphClassChanged();   // a region's top
        for (DocNode *c : cur->children) stack.push_back({ c, switched });
    }
}

/// Brings a freshly (re-)parented subtree's class in line with the document's hint (once per structural move, on the
/// SUBTREE ROOT). A node with no static hint under a dynamic parent costs nothing.
void reconcileStatic(DocNode &n)
{
    const bool want = n.owner && n.owner->wantsStatic() && eligibleStaticParent(n.parent);
    if (n.graphStatic == want) return;
    applyStaticSubtree(n, want);
}

/// IS THIS WRITE SCENE MOVEMENT? (nodegraph.h, the transform-write epoch.) Everything is, except the VIEWER — a
/// camera without DOCUMENT children (the store's children: the engine's helper nodes were never in it).
inline bool writeIsSceneMovement(const DocNode &n)
{
    const SceneNode *owner = n.owner;
    if (!owner || owner->_countsAsMovement()) return true;
    return !n.children.empty();
}

/// RULE 4 + the epoch + the journal: what every transform write pays after the store holds the new value.
void markMoved(DocNode &n)
{
    if (writeIsSceneMovement(n)) {
        gTransformWrites.fetch_add(1, std::memory_order_relaxed);
        journalWrite(n.owner, Kind::Moved);
    } else {
        journalWrite(n.owner, Kind::Posed);
    }
    if (n.graphStatic) {
        // A RE-EXPRESSION keeps the world pose and changes no class (the engine static-dirties the node it is handed).
        if (gReexpressDepth > 0) return;
        if (n.owner) n.owner->_clearStaticHint();
        applyStaticSubtree(n, false);
        return;
    }
    // THE ONE HOLE RULE 2 LEAVES, closed: a scene ROOT is exempt because nothing writes it — if something does, every
    // static subtree under it is demoted rather than left behind.
    if (!n.parent && gStaticNodes.load(std::memory_order_relaxed) > 0) {
        bool any = false;
        for (DocNode *c : n.children) {
            if (!c->graphStatic) continue;
            if (c->owner) c->owner->_clearStaticHint();
            applyStaticSubtree(*c, false);
            any = true;
        }
        if (any)
            qWarning("iris::graph: a scene ROOT node was moved — every static subtree under it has been demoted, "
                     "because a static node cannot follow a moving parent (SCENEGRAPH_SPEC §6 rule 2).");
    }
}

/// INVALIDATION (nodegraph.h, the store's caches): `n` and its subtree lose their cached world and box. Stops at a
/// node already invalid (the invariant: nothing below it is valid) unless `force` — a rider flag flipping or a
/// rider's published world changing moves worlds no write announced.
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

}  // namespace

// ---- the owner table and the journal --------------------------------------------------------------------------

void registerNode(SceneNode *n)
{
    if (n) setOwnerById(n->getNodeId(), n);
}

SceneNode *nodeById(long long id) { return ownerById(id); }

void unregisterNode(SceneNode *n)
{
    if (!n) return;
    if (n->docNode().graphStatic) gStaticNodes.fetch_sub(1, std::memory_order_relaxed);
    if (ownerById(n->getNodeId()) == n) setOwnerById(n->getNodeId(), nullptr);
}

void reassignNodeId(SceneNode *n, long long id)
{
    if (!n || n->getNodeId() == id) return;
    if (ownerById(n->getNodeId()) == n) setOwnerById(n->getNodeId(), nullptr);
    n->nodeId = id;
    setOwnerById(id, n);
}

unsigned long long transformWrites() { return gTransformWrites.load(std::memory_order_relaxed); }

WriteJournalReader::WriteJournalReader(bool wantsWritten, bool wantsPosed)
    : wantsWritten(wantsWritten), wantsPosed(wantsPosed)
{
    if (wantsWritten) gJournalWrittenReaders.fetch_add(1, std::memory_order_relaxed);
    if (wantsPosed) gJournalPosedReaders.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(gJournalMutex);
    cursor = gJournalNextSeq;
    gJournalMaxCursor = std::max(gJournalMaxCursor, cursor);
    gJournalReaders.push_back(this);
}

WriteJournalReader::~WriteJournalReader()
{
    if (wantsWritten) gJournalWrittenReaders.fetch_sub(1, std::memory_order_relaxed);
    if (wantsPosed) gJournalPosedReaders.fetch_sub(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(gJournalMutex);
    gJournalReaders.erase(std::remove(gJournalReaders.begin(), gJournalReaders.end(), this), gJournalReaders.end());
    gJournalMaxCursor = 0;
    for (const WriteJournalReader *r : gJournalReaders) gJournalMaxCursor = std::max(gJournalMaxCursor, r->cursor);
    if (gJournalReaders.empty()) { gJournal.clear(); gJournalNewest.clear(); }
    else journalTrimLocked();
}

namespace
{
/// The entries since `r`'s cursor (the reader's lock held), the cursor advanced. False = incomplete.
bool takeLocked(WriteJournalReader &r, std::vector<std::pair<long long, Kind>> &ids)
{
    bool complete = r.primed;
    std::lock_guard<std::mutex> lock(gJournalMutex);
    if (r.cursor < gJournalCapFloor) complete = false;   // the cap dropped entries this reader had not read
    // FROM THE READER'S CURSOR, not from the log's front: the log is in sequence order, and a LAGGING reader (a
    // physics world that is not simulating keeps its cursor while the editor writes) holds the log at its cap — a
    // scan from the front made every other reader's take O(cap) twice a sync (measured: the mirror's settle slice
    // 1.07 ms at 1k nodes behind a 65,536-entry log, against 0.13 ms of its own work).
    auto first = std::lower_bound(gJournal.begin(), gJournal.end(), r.cursor,
                                  [](const JournalEntry &e, unsigned long long c) { return e.seq < c; });
    for (auto it = first; it != gJournal.end(); ++it) ids.emplace_back(it->id, it->kind);
    r.cursor = gJournalNextSeq;
    r.primed = true;
    gJournalMaxCursor = std::max(gJournalMaxCursor, r.cursor);
    journalTrimLocked();
    return complete;
}

/// Each node once, resolved through the owner table (a node destroyed since its write is gone); `rank` orders the
/// kinds a node logged twice is answered as (the lower wins: moved beats posed beats written).
template <typename Sink>
void resolveEach(const std::vector<std::pair<long long, Kind>> &ids, Sink sink)
{
    std::unordered_map<long long, Kind> best;
    best.reserve(ids.size());
    for (const auto &e : ids) {
        auto it = best.find(e.first);
        if (it == best.end()) best.emplace(e.first, e.second);
        else if (unsigned(e.second) < unsigned(it->second)) it->second = e.second;
    }
    for (const auto &e : ids) {
        auto it = best.find(e.first);
        if (it == best.end()) continue;               // already answered
        const Kind k = it->second;
        best.erase(it);
        sink(ownerById(e.first), k);
    }
}
}  // namespace

bool WriteJournalReader::take(std::vector<SceneNode *> &out)
{
    out.clear();
    std::vector<std::pair<long long, Kind>> ids;
    const bool complete = takeLocked(*this, ids);
    resolveEach(ids, [&](SceneNode *n, Kind k) { if (n && k == Kind::Moved) out.push_back(n); });
    return complete;
}

bool WriteJournalReader::take(std::vector<SceneNode *> &moved, std::vector<SceneNode *> &written, int *gone)
{
    moved.clear();
    written.clear();
    if (gone) *gone = 0;
    std::vector<std::pair<long long, Kind>> ids;
    const bool complete = takeLocked(*this, ids);
    resolveEach(ids, [&](SceneNode *n, Kind k) {
        if (k == Kind::Posed) return;
        if (!n) { if (gone) ++*gone; return; }
        (k == Kind::Moved ? moved : written).push_back(n);
    });
    return complete;
}

bool WriteJournalReader::takePoses(std::vector<SceneNode *> &moved, std::vector<SceneNode *> &posed)
{
    moved.clear();
    posed.clear();
    std::vector<std::pair<long long, Kind>> ids;
    const bool complete = takeLocked(*this, ids);
    resolveEach(ids, [&](SceneNode *n, Kind k) {
        if (!n || k == Kind::Written) return;
        (k == Kind::Moved ? moved : posed).push_back(n);
    });
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

void noteDocumentWrite(SceneNode *n)
{
    if (!n || gJournalWrittenReaders.load(std::memory_order_relaxed) == 0) return;
    const long long id = n->getNodeId();
    std::lock_guard<std::mutex> lock(gJournalMutex);
    if (gJournalReaders.empty()) return;
    journalLogLocked(id, Kind::Written);
}

void setCountsAsMovement(SceneNode *n, bool counts)
{
    if (n) n->_setCountsAsMovement(counts);
}

bool countsAsMovement(const SceneNode *n)
{
    return !n || n->_countsAsMovement();
}

unsigned long long staticSwitchCount() { return gStaticSwitches.load(std::memory_order_relaxed); }

ReexpressScope::ReexpressScope() { ++gReexpressDepth; }
ReexpressScope::~ReexpressScope() { --gReexpressDepth; }

// ---- SCENE_STATIC ------------------------------------------------------------------------------------------------

bool isStatic(const DocNode &n) { return n.graphStatic; }

bool canBeStatic(const DocNode &n) { return eligibleStaticParent(n.parent); }

bool setStatic(DocNode &n, bool value)
{
    if (n.graphStatic == value) {
        if (value) applyStaticSubtree(n, true);   // the subtree follows its root (a child added since)
        return true;
    }
    if (value) {
        if (!eligibleStaticParent(n.parent)) {
            qWarning("iris::graph::setStatic(true) refused: a static node's parent must be static or a scene root "
                     "(SCENEGRAPH_SPEC §6 rule 2).");
            return false;
        }
        if (n.owner && !n.owner->isStaticEligible()) return false;
    }
    applyStaticSubtree(n, value);
    return true;
}

bool promoteNode(DocNode &n, bool *switched)
{
    if (switched) *switched = false;
    if (n.graphStatic) return true;
    if (!eligibleStaticParent(n.parent)) {
        qWarning("iris::graph::promoteNode refused: a static node's parent must be static or a scene root "
                 "(SCENEGRAPH_SPEC §6 rule 2).");
        return false;
    }
    if (n.owner && !n.owner->isStaticEligible()) return false;
    switchOne(n, true);
    if (n.owner) n.owner->_noteGraphClassChanged();
    if (switched) *switched = true;
    return true;
}

std::size_t staticNodeCount()
{
    const long long v = gStaticNodes.load(std::memory_order_relaxed);
    return v > 0 ? std::size_t(v) : 0;
}

// ---- socket riders -----------------------------------------------------------------------------------------------

void setSocketRider(DocNode &rider, bool on)
{
    if (rider.rider == on) return;
    rider.rider = on;
    if (!on) { delete rider.riderTag; rider.riderTag = nullptr; }
    invalidateSubtree(rider, true);   // its world (and every world under it) changes source
}

bool isSocketRider(const DocNode &rider) { return rider.rider; }

void setRiderTag(DocNode &rider, const Trs &world)
{
    if (!rider.rider) return;
    if (!rider.riderTag) rider.riderTag = new Trs(world);
    else {
        const Trs &w = *rider.riderTag;
        if (std::memcmp(&w, &world, sizeof(Trs)) == 0) return;   // the same pose as drawn last frame
        *rider.riderTag = world;
    }
    invalidateSubtree(rider, true);
}

// ---- THE DOCUMENT'S TRANSFORM STORE (nodegraph.h, ENGINE-UPGRADE-U2) -----------------------------------------

namespace
{
// OGRE'S OWN FLOAT MATHS, written out (this library includes no Ogre header). The two the WORLD is composed
// with are the forms Node::updateAllTransforms runs
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
/// scaled transform, hand-authored data). Those are normalised once, after which they pass through exactly — and the
/// engine, handed the value through the frame record, stores it bit for bit (OgreScene::applyNodeRecords).
inline Quat canonicalRotation(const Quat &q)
{
    const float w = q.scalar(), x = q.x(), y = q.y(), z = q.z();
    const float norm = w * w + x * x + y * y + z * z;
    if (std::fabs(norm - 1.0f) <= 1e-5f) return q;
    const float factor = 1.0f / std::sqrt(norm);
    return Quat(w * factor, x * factor, y * factor, z * factor);
}

/// Matrix4::makeTransform(position, scale, orientation) with Quaternion::ToRotationMatrix inside: scale, then
/// rotate, then translate. iris::Mat4's 16-argument constructor reads row-major.
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
/// (their parent is the engine's tag: the pose of the frame it last drew, setRiderTag).
bool deriveWorld(const DocNode &n, Vec3 &P, Quat &R, Vec3 &S)
{
    if (n.rider) {
        // THE TAG IS THE RIDER'S PARENT (W1): its world composes the rider's LOCAL by the same rule as any parent's,
        // so a local write shows at once and a world write re-expresses under the tag (parentWorld).
        if (n.riderTag) {
            const Trs &t = *n.riderTag;
            S = t.scale * n.scale;
            R = qmul(t.rot, n.rot);
            P = qrot(t.rot, t.scale * n.pos) + t.pos;
        } else {
            P = n.pos; R = n.rot; S = n.scale;   // armed, not drawn yet
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
    // A RIDER re-expresses under the TAG it hangs from, never under its document parent (W1).
    if (n.rider && n.riderTag) {
        P = n.riderTag->pos; R = n.riderTag->rot; S = n.riderTag->scale;
        return isIdentityTrs(P, R, S);
    }
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
    // A REPARENT MOVES THE CHILD unless the caller re-adopts its world pose: the local is kept and the parent under it
    // changes, so the whole subtree's worlds move without a setter — one movement write, journaled.
    gTransformWrites.fetch_add(1, std::memory_order_relaxed);
    journalWrite(child.owner, Kind::Moved);
    reconcileStatic(child);
}

void detachDoc(DocNode &child)
{
    if (!child.parent) return;
    auto &old = child.parent->children;
    old.erase(std::remove(old.begin(), old.end(), &child), old.end());
    child.parent = nullptr;
    invalidateSubtree(child);
    gTransformWrites.fetch_add(1, std::memory_order_relaxed);
    journalWrite(child.owner, Kind::Moved);
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

void writeLocalPos(DocNode &n, const Vec3 &v)
{
    n.pos = v;
    invalidateSubtree(n);
    markMoved(n);
}

void writeLocalRot(DocNode &n, const Quat &q)
{
    n.rot = canonicalRotation(q);
    invalidateSubtree(n);
    markMoved(n);
}

void writeLocalScale(DocNode &n, const Vec3 &v)
{
    n.scale = v;
    invalidateSubtree(n);
    markMoved(n);
}

void writeLocalTrs(DocNode &n, const Vec3 &p, const Quat &r, const Vec3 &s)
{
    n.pos = p;
    n.rot = canonicalRotation(r);
    n.scale = s;
    invalidateSubtree(n);
    markMoved(n);
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

void writeGlobalPos(DocNode &n, const Vec3 &v)
{
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) { writeLocalPos(n, v); return; }
    writeLocalPos(n, localPosFrom(n, v, pP, qinv(pR), pS));
}

void writeGlobalRot(DocNode &n, const Quat &q)
{
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) { writeLocalRot(n, q); return; }
    writeLocalRot(n, qmul(qinv(pR), q));
}

void writeGlobalPosRot(DocNode &n, const Vec3 &v, const Quat &q)
{
    // ONE resolution of the parent, for both halves — and none of the inverse when the parent is at identity (a
    // physics body's parent is the document root, identity by construction: nodegraph.h rule 2's exemption).
    Vec3 pP, pS;
    Quat pR;
    if (parentWorld(n, pP, pR, pS)) { writeLocalTrs(n, v, q, n.scale); return; }
    const Quat inv = qinv(pR);
    writeLocalTrs(n, localPosFrom(n, v, pP, inv, pS), qmul(inv, q), n.scale);
}

void writeGlobalTrs(DocNode &n, const Trs &w)
{
    const Trs l = localFromWorld(n, w);
    writeLocalTrs(n, l.pos, l.rot, l.scale);
}

void writeGlobalTransform(DocNode &n, const Mat4 &m)
{
    Trs w;
    iris::decomposeTRS(m, w.pos, w.rot, w.scale);
    writeGlobalTrs(n, w);
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
    // The parent half: what writeGlobalPosRot resolves (the store's parent world, so a parent moved since the last
    // write shows up here).
    if (n.parent) {
        w.parent = n.parent;
        const Trs p = world(*n.parent);
        v[0] = p.pos.x(); v[1] = p.pos.y(); v[2] = p.pos.z();
        v[3] = p.rot.scalar(); v[4] = p.rot.x(); v[5] = p.rot.y(); v[6] = p.rot.z();
        v[7] = p.scale.x(); v[8] = p.scale.y(); v[9] = p.scale.z();
    }
    // The node half: what writeGlobalPosRot writes.
    v[10] = n.pos.x(); v[11] = n.pos.y(); v[12] = n.pos.z();
    v[13] = n.rot.scalar(); v[14] = n.rot.x(); v[15] = n.rot.y(); v[16] = n.rot.z();
    return w;
}

}  // namespace graph
}  // namespace iris
