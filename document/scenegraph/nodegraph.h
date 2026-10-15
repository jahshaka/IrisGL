/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_NODEGRAPH_H
#define IRIS_NODEGRAPH_H

// -----------------------------------------------------------------------------
// iris::graph — the DOCUMENT'S scene graph: its own transforms and hierarchy.
//
// ENGINE-UPGRADE-U2 reversed SCENEGRAPH_SPEC D2 (ENGINE_UPGRADE_SPEC §1, the
// owner's Q1). The document no longer shares Ogre's hierarchy: each document
// node carries a DocNode (below) — its local TRS, its parent, its ordered
// children, its graph class — and derives its world in document maths, by
// Ogre's own composition rule. The ENGINE owns its own nodes and is told what
// the document wrote through the frame record (SceneMirror's extract:
// FrameInputScene::nodes). This library includes no Ogre header and needs no
// Ogre::Root: a document builds on any thread, before any engine exists — the
// staging scene manager, the migration between managers and the off-thread
// Ogre-creation hazard they guarded are gone by construction.
// -----------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <vector>

#include "core/math/mat4.h"
#include "core/math/quat.h"
#include "core/math/vec.h"

namespace iris
{

class SceneNode;

namespace graph
{

// ---- THE DOCUMENT'S TRANSFORM STORE (ENGINE-UPGRADE-U2) ---------------------------------------------------------
//
// SCENEGRAPH_SPEC D2 REVERSED (ENGINE_UPGRADE_SPEC §1, the owner's Q1): the document owns its transforms and its
// hierarchy again. Each document node carries a DocNode — its LOCAL TRS, its PARENT and its ORDERED CHILDREN — and
// the WORLD TRS is derived from them on read, by OGRE'S OWN RULE (OgreNode.cpp updateFromParentImpl with
// OGRE_NODE_INHERIT_TRANSFORM = 0, no inherit flags anywhere in this tree):
//
//     S = pS * S_l (per axis),   R = pR * R_l,   P = pP + pR * (pS * P_l)
//
// and the world MATRIX is Ogre's makeTransform of that triple — never a matrix product down the chain: a rotated
// child of a non-uniformly scaled parent acquires a shear in a matrix product that Ogre's rule drops, so the two
// answers differ by the shear (the first measure of ENGINE-UPGRADE-U2: spikes/engine-upgrade/u2/premises.md).
//
// CACHED PER NODE, INVALIDATED BY SUBTREE. A write (a local TRS, a reparent, a detach) invalidates the node and its
// subtree — stopping at the first node already invalid, so a node written every frame and read every frame costs
// O(1) each; a read re-derives only the invalid part of the chain. A socket rider's world is the engine's (above) and
// is never cached, nor is anything under it.
//
// THE ENGINE IS TOLD, NEVER ASKED: the mirror extracts each written node's LOCAL TRS (and its parent and graph class
// when they change) into the frame record; the engine composes its own hierarchy from them. A moved subtree root is one
// entry, never its subtree.
class PickTable;

/// A position, a rotation and a scale — a world or a local pose.
struct Trs
{
    Vec3 pos = Vec3(0.0f, 0.0f, 0.0f);
    Quat rot;
    Vec3 scale = Vec3(1.0f, 1.0f, 1.0f);
};

struct DocNode
{
    explicit DocNode(SceneNode *o) : owner(o) {}
    ~DocNode() { delete riderTag; }
    DocNode(const DocNode &) = delete;
    DocNode &operator=(const DocNode &) = delete;

    SceneNode *const owner;
    DocNode *parent = nullptr;          ///< the document parent; null = a scene root or a detached subtree
    std::vector<DocNode *> children;    ///< the document children, in sibling order
    Vec3 pos = Vec3(0.0f, 0.0f, 0.0f);
    Quat rot;
    Vec3 scale = Vec3(1.0f, 1.0f, 1.0f);
    bool rider = false;                 ///< socket-attached (setSocketRider)
    bool graphStatic = false;           ///< the GRAPH CLASS (SCENE_STATIC below): the engine's node follows it
    Trs *riderTag = nullptr;            ///< a rider's TAG world as the engine last drew it (setRiderTag); owned
    PickTable *pickTable = nullptr;     ///< the scene broad-phase table this node has a slot in (PickTable), if any
    int pickSlot = -1;

    // ---- the caches (nodegraph.cpp) ----
    // INVARIANT: a node whose world is not valid has no valid world below it (a write invalidates its whole
    // subtree, stopping at the first node already invalid; a read validates the chain top-down).
    mutable bool worldValid = false;
    mutable Vec3 worldPos;
    mutable Quat worldRot;
    mutable Vec3 worldScale = Vec3(1.0f, 1.0f, 1.0f);
    mutable bool boxValid = false;               ///< the world box below, valid with the world
    mutable float boxFrom[6] = {};               ///< ...derived from this LOCAL box (min xyz, max xyz)
    mutable float box[6] = {};                   ///< min xyz, max xyz
};

/// PICKING'S BROAD PHASE, FLAT (a Scene owns one; scenepicking.cpp walks it). Every mesh node of a scene has a slot:
/// its world box as six contiguous floats and a state byte. A pick reads the floats of the valid slots and touches no
/// node at all — the boxes are pushed stale by the store's own invalidation (a write reaching the node, a content
/// swap), never polled: a scan that dereferenced each node to ask was three dependent cache misses a mesh, the whole
/// cost of a 10k-mesh pick (measured, ENGINE-UPGRADE-U2: spikes/engine-upgrade/u2/premises.md).
class PickTable
{
public:
    enum State : unsigned char { Stale = 0, Valid = 1, NoGeometry = 2 };
    PickTable() = default;
    ~PickTable() { clear(); }
    PickTable(const PickTable &) = delete;
    PickTable &operator=(const PickTable &) = delete;

    void add(SceneNode *meshNode);
    void remove(SceneNode *meshNode);
    /// Every slot out (the nodes keep living; their slots are reset).
    void clear();
    std::size_t slotCount() const { return mNode.size(); }
    SceneNode *node(std::size_t slot) const { return mNode[slot]; }
    State state(std::size_t slot) const { return State(mState[slot]); }
    const float *box(std::size_t slot) const { return &mBox[6 * slot]; }
    /// The slot's box from the node's world and its LOCAL box (false = an empty box: the slot stays Stale).
    /// Valid only when the world could be cached (never for a socket rider or anything under one).
    bool refresh(std::size_t slot, const Vec3 &localMin, const Vec3 &localMax);
    /// The node carries no triangle geometry until its content changes.
    void markNoGeometry(std::size_t slot) { mState[slot] = NoGeometry; }
    void markStale(int slot) { if (slot >= 0) mState[std::size_t(slot)] = Stale; }

private:
    std::vector<SceneNode *> mNode;     ///< null = a free slot
    std::vector<unsigned char> mState;
    std::vector<float> mBox;            ///< 6 per slot: min xyz, max xyz
    std::vector<int> mFree;
};

/// A node's CONTENT changed (a mesh or material swapped): its broad-phase slot must re-read the local box.
void invalidatePick(DocNode &n);

// ---- socket riders (AVATAR_RIG_PERF_SPEC §4.3) ----------------------------------------------------------------
//
// A node attached to a socket hangs off an engine TAG POINT on a bone. The socket API's contract is explicit that
// this is NOT a reparent — "it keeps its place in the hierarchy" — and since the document owns its hierarchy it simply
// does: the store's parent and sibling index never change. What the flag changes is the rider's WORLD PARENT: it
// hangs from the engine's TAG on the bone, whose pose is the engine's — so the tag's world as the engine PUBLISHED it
// for the frame it drew (Scene::published(); SceneMirror hands it in each sync: setRiderTag) is the parent its LOCAL
// composes under and a world write re-expresses under (W1, the stage 1 read: under the document parent a gizmo drag
// of a socketed camera ran away). One frame of latency on the bone, explicit; none on the rider's own writes.

/// `rider` is socket-attached (true) or came off its socket (false). Idempotent.
void setSocketRider(DocNode &rider, bool on);
bool isSocketRider(const DocNode &rider);
/// The rider's TAG world as the engine drew it last frame. Ignored for a node that is not a rider.
void setRiderTag(DocNode &rider, const Trs &tagWorld);

// -- the hierarchy --
/// `child` under `parent` at the DOCUMENT sibling index `index` (-1 or past the end = append), out of its old parent
/// first. The LOCAL transform is kept (the caller decides what happens to the world one).
void attachDoc(DocNode &parent, DocNode &child, int index);
/// `child` out of its parent (a root of its own afterwards).
void detachDoc(DocNode &child);
/// A node is being destroyed: out of its parent, and its children orphaned (they are being released with it).
void unlinkDoc(DocNode &n);
/// Position among its parent's children, or -1 when it has no parent.
int indexInParent(const DocNode &n);

// -- the local TRS: THE write funnel (the store, the movement epoch, the journal and SCENE_STATIC rule 4). Named
// write*, not set*: they are not document setters — the SceneNode setter that calls each marks NodeChange::Transform
// (document.no_silent_setters holds every set* to that). --
void writeLocalPos(DocNode &n, const Vec3 &v);
void writeLocalRot(DocNode &n, const Quat &q);
void writeLocalScale(DocNode &n, const Vec3 &v);
/// All three at once — one dirty mark instead of three.
void writeLocalTrs(DocNode &n, const Vec3 &p, const Quat &r, const Vec3 &s);
/// The local matrix: makeTransform of the local triple.
Mat4 localTransform(const DocNode &n);

// -- the world, derived --
Trs world(const DocNode &n);
/// The world matrix: makeTransform of world(n).
Mat4 globalTransform(const DocNode &n);

/// THE RE-EXPRESSION: the local TRS that puts `n` at `world` under its current parent, by the exact inverse of the
/// composition rule above — R_l = pR^-1 R, S_l = S / pS, P_l = (pR^-1 (P - pP)) / pS, per axis. A parent axis of
/// ZERO scale has flattened that axis of every world under it; no local can undo that, and dividing by it would
/// store an infinity or a NaN (SC1-REEXPRESS-SCALE-1), so that axis keeps the node's own local position and scale.
Trs localFromWorld(const DocNode &n, const Trs &world);
void writeGlobalPos(DocNode &n, const Vec3 &v);
void writeGlobalRot(DocNode &n, const Quat &q);
/// BOTH AT ONCE, for a caller that has both — the physics write-back, which runs per body per step: one parent
/// resolution, and none at all when the parent is at identity (what a physics body's parent almost always is).
void writeGlobalPosRot(DocNode &n, const Vec3 &v, const Quat &q);
/// The whole world pose, re-expressed exactly (a keepTransform reparent hands the old world TRS straight here).
void writeGlobalTrs(DocNode &n, const Trs &world);
/// A world MATRIX: decomposed (iris::decomposeTRS — a shear in `m` is dropped), then writeGlobalTrs.
void writeGlobalTransform(DocNode &n, const Mat4 &m);

/// The world AABB of a LOCAL box `local` (a mesh's baked `Mesh::aabb`) carried by `n`: the exact box of the
/// transformed box (|M| applied to the half extents — Ogre's Aabb::transformAffine), padded by one part in 1e5 so a
/// triangle on a face still lies inside it after rounding. Cached with the world, per local box. False for an empty
/// local box.
bool worldBox(const DocNode &n, const Vec3 &localMin, const Vec3 &localMax, float outMin[3], float outMax[3]);

/// THE TRANSFORM-WRITE EPOCH — a monotonic counter bumped by every setter
/// above, and the only way the renderer can learn that NOTHING moved.
///
/// It exists because the document owns the scene graph and writes into it
/// directly: when a mesh is dragged, no call reaches the engine at all. The
/// renderer's answer used to be to look for itself — a per-frame walk of every
/// item's world AABB (`OgreScene::ensureGiWalk`), which in a probe-lit scene
/// costs 0.4 / 2.0 / 4.3 ms at 1k / 5k / 10k nodes EVERY FRAME, moving or
/// still. Reading this counter instead makes a still frame's scan free.
///
/// A CHANGE TEST, NOT A MEASUREMENT, and deliberately relaxed:
///   * it counts WRITES, not moves (writing the same value counts),
///   * it is PROCESS-WIDE, not per scene (a write in one scene makes every
///     scene re-scan once),
///   * it is read once a frame, so it needs to be eventually-visible, never
///     ordered — hence `memory_order_relaxed` on both ends.
/// All three err on the side of scanning when nothing moved, which costs
/// exactly what the old unconditional scan cost.
///
/// THE ONE THING IT DOES NOT COUNT IS THE VIEWER (lane ENGINE-7 item 1, from
/// RR2's measurement). A CAMERA's transform is not scene movement: nothing any
/// consumer of this counter scans — the GI items' world boxes, the shadow
/// casters, the forward-plus scene extent, the floor's world, the skeleton
/// share's world comparisons — can have changed because the camera moved. It
/// was counted anyway, and the bill was exact: on the 8,404-node lattice
/// `host.env` (the mirror's GI push, which reads two O(scene) engine
/// signatures) measured 0.024 ms while the camera was still and 10.28 ms on
/// 92% of FLYING frames, with the same scene and the same path costing
/// 0.014 ms with GI off. Flying re-ran every scan in the program, every frame.
/// A camera node says so once, at birth (`setCountsAsMovement(this, false)` in
/// CameraNode's constructor), and a camera that has DOCUMENT CHILDREN counts
/// again — moving it moves whatever is parented under it, and that IS scene
/// movement. Only DOCUMENT children count: the engine's own nodes (a camera's
/// body wire, a light's -Y adapter, a decal's projector box) are not content
/// and were never in the store.
///
/// THE ENGINE NO LONGER READS IT (U0 3b): the engine learns what moved from the frame's ChangeSet, which each
/// document's mirror drains from the WRITE JOURNAL below — the same writes, by name. The count stays the physics
/// write-back's cheap "did anything else write?" test.
unsigned long long transformWrites();
/// The owner table the journal resolves ids through: SceneNode's constructor, destructor and setNodeId, nothing else.
void registerNode(SceneNode *n);
/// The live document node with this SceneNode::getNodeId, or null (destroyed, or never made).
SceneNode *nodeById(long long id);
/// Clears the node's row ONLY while the row is still the node's: a node that gave its id to another (an undo's
/// replay, SceneNode::setNodeId) must not erase the new owner when it dies.
void unregisterNode(SceneNode *n);
/// THE ID MOVES WITH ITS ROW (ENGINE-UPGRADE-U3 stage 3 fix round, decision 33): the node takes `id` and the owner
/// table names it under that id — the old row is let go when it was the node's. SceneNode::setNodeId.
void reassignNodeId(SceneNode *n, long long id);
/// THE WRITE JOURNAL — the change record every frame's ChangeSet is drained from (ENGINE-UPGRADE-1 U0 3b; first
/// built for ENGINE_V2_SPEC V2-1, SPEED-CPU 2026-10-04). The counter above says THAT something moved; the journal
/// says WHAT: every write the counter counts (a transform setter through the one funnel, a reparent, a detach) also
/// records its node — exactly the counter's writes, so a reader of the journal sees everything the counter saw.
///
/// ONE LOG, A CURSOR PER READER. The journal is process-wide (a document can be built on a worker, and every write
/// path touches it), and it has several readers on their own schedules: each
/// document's mirror at its emit (the ChangeSet, every host frame) and each physics world at its write-back (only
/// while it simulates). A shared CLOSE cannot serve them — one reader closing would throw away writes another has
/// not read (the editor's mirror losing a drag to a thumbnail's sync; physics losing its incremental re-check to the
/// mirror's emit) — so each reader holds a `WriteJournalReader`: `take` answers the writes since that reader's own
/// last take, and the log keeps an entry only until every live reader has passed it. A node is logged once per
/// "every reader still owes it", never once per write. With no reader registered nothing is logged at all.
///
/// A take is INCOMPLETE (false) when it cannot name every write: the reader's first take (it never saw what came
/// before it), or a reader so far behind that the log's cap dropped entries it had not read. An incomplete take is
/// the caller's cue for its full pass. Entries are keyed by SceneNode::getNodeId (a process-wide monotonic id), so a
/// node destroyed since its write simply resolves to nothing.
class WriteJournalReader {
public:
    /// `wantsWritten`: this reader also hears the journal's WRITTEN half (noteDocumentWrite) — the editor's scene
    /// checker, which re-checks a moved node with its subtree and a written one alone (BISTRO-ENGINE-1's tap, re-homed).
    explicit WriteJournalReader(bool wantsWritten = false, bool wantsPosed = false);
    ~WriteJournalReader();
    WriteJournalReader(const WriteJournalReader &) = delete;
    WriteJournalReader &operator=(const WriteJournalReader &) = delete;
    /// The DOCUMENT nodes written since this reader's last take (or skip), each once, as live handles (a node destroyed
    /// since its write is gone from the answer; a migrated node answers with its new handle). False = incomplete (above).
    bool take(std::vector<SceneNode *> &out);
    /// ...the same take split in two, for a reader that asked for the written half: `moved` (a journal write reached
    /// the node — its world pose and its subtree's may have changed), `written` (only the node itself changed), each
    /// node once; `gone` = written nodes destroyed since their write.
    bool take(std::vector<SceneNode *> &moved, std::vector<SceneNode *> &written, int *gone = nullptr);
    /// ...the take the frame record is built from (SceneMirror): `moved` as above, and `posed` = every OTHER node
    /// whose transform was written (a write that is not scene movement — a camera's: the engine still has to draw it
    /// where it is). Only a reader constructed with `wantsPosed` hears the posed half.
    bool takePoses(std::vector<SceneNode *> &moved, std::vector<SceneNode *> &posed);
    /// Forget every write so far, unread — a reader whose own writes follow its take (physics' write-back) says so.
    void skip();

    // ---- the log's own bookkeeping (nodegraph.cpp) ----
    unsigned long long cursor = 0;   ///< the sequence number of the first write this reader has not read
    bool               primed = false;
    const bool         wantsWritten;
    const bool         wantsPosed;
};
/// Entries the log holds right now (a running diagnostic for the suites: a log no reader drains stays bounded).
std::size_t writeJournalSize();
/// A document write that is not scene movement — the journal's WRITTEN half (WriteJournalReader(true)): a mesh or
/// material swap, a parameter, a visibility, a flag. One relaxed load and nothing else while no reader asked for it.
void noteDocumentWrite(SceneNode *n);
/// NODES THAT CHANGED MEMORY-MANAGER CLASS (a static <-> dynamic migration, one per node), a process-wide
/// running total: what a promotion, a demotion or a reparent's rule-4 round trip cost, in counts
/// (ENGINE-UPGRADE-1 stage 2, mirror.reparent_local).
unsigned long long staticSwitchCount();
/// Does a write to this node's transform count as SCENE MOVEMENT (above)?
/// True for every node by default; a CAMERA says false once, in its
/// constructor. Not a "static" hint and not a visibility flag: it answers one
/// question, "can anything the renderer scans have changed because this node
/// moved?", and the honest answer for the viewer is no.
void setCountsAsMovement(SceneNode *n, bool counts);
/// What the flag above says right now (a suite read; the write path asks the
/// document node itself, see nodegraph.cpp).
bool countsAsMovement(const SceneNode *n);

/// EVERYTHING writeGlobalPosRot READS AND WRITES, bit for bit: the parent's
/// world position, orientation and scale (what the write undoes) and the node's
/// own local position and orientation (what it writes), all from the store. Two
/// equal witnesses of one node mean a writeGlobalPosRot of the SAME world pose
/// would write exactly the locals the node already holds — the physics
/// write-back's proof that a sleeping body's node needs no write
/// (Environment::writeBack, SPEED-CPU). A plain value; compare with ==.
struct PoseWitness
{
    const DocNode *parent = nullptr;
    float values[17] = {};   ///< parent pos 3, rot 4, scale 3; local pos 3, rot 4
    bool operator==(const PoseWitness &o) const;
    bool operator!=(const PoseWitness &o) const { return !(*this == o); }
};
PoseWitness poseWitness(const DocNode &n);

// ---- SCENE_STATIC: the GRAPH CLASS (SPECS/SCENEGRAPH_SPEC.md §6 — the multiplier) --------------------------
//
// Ogre-Next keeps two node memory managers per SceneManager. Only the DYNAMIC one is walked by `updateAllTransforms`
// every frame; the STATIC one joins the update list only for the frames something in it changed (static dirties are
// batched per frame by Ogre itself). A scene whose ground, architecture and imported props are static pays per-frame
// transform + bounds work for the moving part only.
//
// THE CLASS IS THE DOCUMENT'S DECISION, THE ENGINE APPLIES IT (ENGINE-UPGRADE-U2). Each DocNode carries its graph
// class (DocNode::graphStatic); the mirror sends it in the frame record, and the engine moves its own node — and the
// attachments and helper nodes it hangs off it — into that memory manager (OgreScene::applyNodeRecords). The rules
// are the document's, decided here, synchronously, where the settle, undo, the scripting surface and the suites read
// them; the engine never answers back (U3: the document thread asks the engine nothing):
//
//  1. STATIC IS A SUBTREE PROPERTY. Ogre's Node::setParent gives a child its parent's memory manager, so setStatic
//     switches n AND its whole subtree (skipping, not vetoing, a branch that cannot be static).
//  2. A STATIC NODE'S PARENT MUST BE STATIC — or be a scene ROOT (a node with no parent), whose transform is
//     identity and which nothing writes. Ogre: a static node is refreshed only on static dirties, so a moving
//     dynamic parent would leave it behind.
//  3. WHAT CAN NEVER BE STATIC SAYS SO: SceneNode::isStaticEligible (lights, particles, decals, cameras, physics
//     bodies, riders, skinned and animated nodes). The engine still refuses an attachment Ogre cannot switch and
//     keeps that node dynamic (a dynamic child of a static parent is valid; the opposite is the bug).
//  4. MOVING A STATIC NODE DEMOTES IT (and its subtree) instead of paying a whole static pass per edit; the GRAPH
//     hint is cleared with it (SceneNode::_clearStaticHint), never the user's mobility. A RE-EXPRESSION IS NOT A
//     MOVE: a write inside a ReexpressScope keeps the world by construction and changes no class — the engine,
//     handed a transform for a static node, marks it static-dirty (one static pass at its depth, Ogre's own path).
//
// Every class change notifies the node (SceneNode::_noteGraphClassChanged → NodeChange::Flags) so its mirror sends
// the new class.

bool isStatic(const DocNode &n);
/// True when `n` may legally be static: its parent is static or a scene root. Rule 2.
bool canBeStatic(const DocNode &n);
/// Switches `n` AND ITS ELIGIBLE SUBTREE. Returns false (changing nothing) when `value` is true and the switch is
/// illegal (rule 2) or `n` itself is ineligible (rule 3). An authoring-time call.
bool setStatic(DocNode &n, bool value);

/// THE WRITES INSIDE RE-EXPRESS A POSE, THEY DO NOT MOVE IT (rule 4's exception): while one is alive on this thread,
/// a transform write to a static node keeps its class. SceneNode::insertChild's keepTransform write is the one user.
struct ReexpressScope {
    ReexpressScope();
    ~ReexpressScope();
    ReexpressScope(const ReexpressScope &) = delete;
    ReexpressScope &operator=(const ReexpressScope &) = delete;
};

/// PROMOTES `n` ALONE and leaves every child where it is — the shape for a pass that spends a bounded amount per
/// frame (the mirror's incremental settle, SceneMirror::kStaticSettleNodesPerSync, top-down: each child is reached
/// on its own visit with a static parent by then). Promotion only: a demotion stays the subtree switch. Returns
/// whether `n` is static afterwards; `*switched` whether it changed class.
bool promoteNode(DocNode &n, bool *switched);

/// How many document nodes in this process are in the static class. The benchmark asserts on it.
std::size_t staticNodeCount();

}  // namespace graph
}  // namespace iris

#endif  // IRIS_NODEGRAPH_H
