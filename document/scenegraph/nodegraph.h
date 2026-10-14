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
// iris::graph — the document's window onto THE scene graph.
//
// TWO HALVES, while ENGINE-UPGRADE-U2 runs. SCENEGRAPH_SPEC D2 made Ogre-Next's
// hierarchy the document's (an `iris::SceneNode` owned an `Ogre::SceneNode` and
// forwarded). U2 reverses it (ENGINE_UPGRADE_SPEC §1, the owner's Q1): the
// document owns its transforms and its hierarchy again — the STORE below
// (DocNode: local TRS, parent, ordered children, a cached world by Ogre's own
// composition rule) — because a render thread cannot share one hierarchy with
// the document. Stage 1 (DOCSTORE) answers every read from the store and still
// writes the Ogre node each document node owns, which the engine adopts and
// draws; stage 2 (EXTRACT) deletes that Ogre half and the engine owns its nodes.
//
// THE BOUNDARY, restated for this file. The old law was "OgreEngine.cpp is the
// only translation unit that includes Ogre". That law is superseded by the
// spec (the document owns Ogre nodes now), and what replaces it is:
//
//     Ogre appears ONLY in sanctioned translation units, and never in a header.
//
// The sanctioned TUs are `irisgl/engine/src/*.cpp` (the backend) and THIS
// file's implementation, `nodegraph.cpp`, which is the only Ogre-including TU
// in the whole document library. Everything above it — scenenode.cpp, scene.cpp,
// every subclass, all of Studio — sees the two opaque handle types below and
// nothing else. `grep -rl "Ogre" --include=*.cpp` outside that list is still a
// review failure.
//
// LIFETIME, which is the part that bites:
//
//   * Every handle belongs to a SceneHandle (an Ogre::SceneManager). A node
//     cannot be re-parented across SceneManagers — migrate() rebuilds it.
//   * A document that is not bound to an engine scene lives in the process-wide
//     STAGING scene manager (stagingScene()), which renders nothing. Fragments
//     built by importers, subtrees held alive by the undo stack and every scene
//     that has not met a SceneMirror yet all live there.
//   * An Ogre::Root must exist before the first handle is created. That is the
//     v1 consequence the spec calls out (§3, "v1 may temporarily require a
//     display for these paths"); v2 replaces it with the NULL render system.
//
// THREADING, and the one thing that was still broken. createNode/destroyNode/
// migrate/attach take a process mutex; transform writes do not (they touch
// only their own SoA slot). That covers the DOCUMENT's own creations on both
// threads — but not the shared state underneath them.
//
// The asset-import worker builds its fragment off the main thread (audit §3.10
// item 34: "the graph's only off-main-thread exposure") while the main thread
// keeps rendering, which means the ENGINE is creating Ogre objects at the same
// time — Items, wire nodes, light adapters — and the engine holds no mutex of
// ours and cannot: `IrisGL` and `JahshakaEngine` are two libraries that never
// link each other (ARCHITECTURE), so there is no lock they could share, and
// injecting one would mean wrapping every Ogre construction in the backend.
//
// THE HAZARD, stated properly (this paragraph used to argue the opposite; it
// was wrong, and a safety comment that is wrong is worse than none). The worker
// creates into the STAGING SceneManager and the main thread edits the bound
// one, so they own different `mSceneNodes` vectors — but "different scene
// managers" was never sufficient, because the RENDER LOOP walked both.
// `Root::renderOneFrame` (OgreRoot.cpp:1101-1126) iterates EVERY SceneManager
// in the process and calls `updateSceneGraph()` on each, staging included. So
// while the worker was inside `SceneManager::createSceneNode` ->
// `ArrayMemoryManager::createNewSlot`, which on growth `OGRE_FREE_SIMD`s the
// old SoA pools (OgreArrayMemoryManager.cpp:167-215), the main thread was
// inside `updateAllTransforms` reading those very pools. That is a
// use-after-free, not a torn read of a value.
//
// TWO FIXES, in two places, and both are needed:
//
//   * the ID COUNTER. `Ogre::Id::generateNewId<T>()` is one function-local
//     static per type, incremented non-atomically, with an upstream comment
//     saying it assumed nobody would do this. A lost increment hands two live
//     objects one id, which in this file also aliases two document handles
//     onto one owner slot. FIXED WHERE IT LIVES:
//     `ogre-patches/0015-id-generator-atomic-counter.patch` makes it a relaxed
//     `std::atomic`. Re-run `irisgl/scripts/build-ogre.sh` after pulling: an
//     engine built before fork 6130df9d1 (was 0015) still has the racy counter.
//   * the POOLS, i.e. the use-after-free above. Fixed STRUCTURALLY, by the
//     engine: `OgreEngine::renderOneFrame` no longer calls
//     `Root::renderOneFrame` — it inlines its body and updates only the scenes
//     an ENABLED View draws (THREADING_ADOPTION_SPEC.md P3). A staging manager
//     never feeds a View, so the render thread never touches it, and the two
//     threads really are disjoint now — by construction rather than by
//     assertion. No lock on the engine's side, no main-thread marshalling of
//     the importer, and no second transform store (the "worker builds pure
//     data" alternative would need one, and §6a rejects that).
//
// If the engine ever goes back to `Root::renderOneFrame()`, this hazard comes
// back with it — that revert is not neutral.
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

/// Distinct incomplete types, so a SceneHandle can never be passed where a
/// NodeHandle is wanted. They are `Ogre::SceneManager*` and `Ogre::SceneNode*`
/// inside nodegraph.cpp and nowhere else.
struct SceneOpaque;
struct NodeOpaque;
using SceneHandle = SceneOpaque *;
using NodeHandle = NodeOpaque *;

/// The process-wide scene manager detached handles live in — every node an
/// importer builds, everything the undo stack holds, every document that has
/// not met a SceneMirror yet.
///
/// Normally the HOST supplies it (setStagingScene, from
/// Engine::documentGraphScene) because only the engine can satisfy the
/// backend's startup order. Without one this falls back to making its own from
/// the live Ogre::Root — which only works once that Root is fully prepared, so
/// it answers null until then rather than crashing inside Ogre.
SceneHandle stagingScene();

/// The host's staging scene manager (Engine::documentGraphScene). Call once,
/// immediately after the engine is created and before the first document node.
void setStagingScene(SceneHandle s);

/// The scene manager `n` belongs to.
SceneHandle sceneOf(NodeHandle n);

/// Is that scene manager still registered with the Root? An engine scene
/// destroyed while a document was still bound to it answers false, and the
/// document's handles are all stale — the caller's job is to say so loudly
/// rather than to walk them.
bool sceneAlive(SceneHandle s);

/// Drops the staging scene manager. Must run BEFORE the Root is deleted; the
/// app calls it from EngineHost::shutdown() and the suites from their teardown.
void shutdown();

// ---- structure ------------------------------------------------------------

/// A new node under `parent` (null = the scene's root node), carrying a
/// back-pointer to its document handle in Ogre's UserObjectBindings.
NodeHandle createNode(SceneHandle s, NodeHandle parent, SceneNode *owner);

/// THE deletion path (spec §2 trap: Ogre's destroySceneNode ORPHANS children
/// and leaks them into mSceneNodes until clearScene). Destroys the subtree,
/// deepest first, and clears every owner's handle on the way.
void destroyNode(NodeHandle n);

/// The document handle that owns `n`, or null for an engine-owned node.
SceneNode *ownerOf(NodeHandle n);

// ---- THE OGRE HALF of the hierarchy (stage 1 of ENGINE-UPGRADE-U2 keeps it; stage 2 deletes it) --------------
//
// The DOCUMENT'S hierarchy is the store below (DocNode::parent / children): every reader asks it. These two keep
// the Ogre nodes the engine still adopts in step with it, and answer nothing.

/// Re-parents `child`'s Ogre node under `parent`'s at `index` (a DOCUMENT sibling index, -1 = append), keeping
/// Ogre's child order the store's. Both must belong to the same SceneHandle; use migrate() otherwise. The LOCAL
/// transform is preserved. A no-op for a socket rider (its Ogre parent is a bone's tag point; its document place is
/// the store's).
void attach(NodeHandle parent, NodeHandle child, int index);
/// The Ogre half catching up with the store after the ENGINE moved a node's Ogre node (a rider released off its
/// bone, re-homed wherever the engine could name): appended under `parent`'s Ogre node unless it is there already.
void followParent(NodeHandle parent, NodeHandle child);

/// Removes `child` from its parent and re-homes it in the STAGING scene, so it
/// stays alive and addressable (the undo stack holds detached subtrees) without
/// staying inside a scene manager the engine is free to destroy. Returns the
/// (possibly new — a cross-manager move rebuilds) handle for `child`.
NodeHandle detach(NodeHandle child);

/// Rebuilds `n`'s subtree in `target` under `newParent` and destroys the
/// original. Returns the new handle for `n`; every owner in the subtree has its
/// handle rewritten as a side effect. Local transforms, visibility and the
/// static flag travel; attachments (which belong to the engine) do not.
NodeHandle migrate(NodeHandle n, SceneHandle target, NodeHandle newParent);

// ---- socket riders (AVATAR_RIG_PERF_SPEC §4.3) ----------------------------------------------------------------
//
// A node attached to a socket hangs off an engine TAG POINT on a bone, so its real Ogre parent is neither its
// document parent nor even a document node. The socket API's contract is explicit that this is NOT a reparent — "it
// keeps its place in the hierarchy" — and since the document owns its hierarchy (the store), it simply does: the
// store's parent and sibling index never change. What the flag below changes is the rider's WORLD: the store cannot
// compose it (the bone's pose is the engine's), so a rider's world — and every world under it — is read from the
// engine's cache, the pose the last rendered frame produced (stage 2: from the frame's output snapshot).

/// `rider` is socket-attached (true) or came off its socket (false). Idempotent.
void setSocketRider(NodeHandle rider, bool on);
bool isSocketRider(NodeHandle rider);

// ---- THE DOCUMENT'S TRANSFORM STORE (ENGINE-UPGRADE-U2 stage 1, DOCSTORE) --------------------------------------
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
// STAGE 1 WRITES BOTH STORES: every write lands here AND on the node's Ogre node (which the engine still adopts and
// draws), so the picture cannot move; every READ is answered here. The one-owner rule ("no second transform store",
// SCENEGRAPH_SPEC §6a) is suspended for exactly this stage — stage 2 deletes the Ogre half and the engine owns its
// own nodes, fed by the frame record.
class PickTable;

struct DocNode
{
    explicit DocNode(SceneNode *o) : owner(o) {}
    DocNode(const DocNode &) = delete;
    DocNode &operator=(const DocNode &) = delete;

    SceneNode *const owner;
    DocNode *parent = nullptr;          ///< the document parent; null = a scene root or a detached subtree
    std::vector<DocNode *> children;    ///< the document children, in sibling order
    Vec3 pos = Vec3(0.0f, 0.0f, 0.0f);
    Quat rot;
    Vec3 scale = Vec3(1.0f, 1.0f, 1.0f);
    bool rider = false;                 ///< socket-attached (setSocketRider)
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

/// A position, a rotation and a scale — a world or a local pose.
struct Trs
{
    Vec3 pos = Vec3(0.0f, 0.0f, 0.0f);
    Quat rot;
    Vec3 scale = Vec3(1.0f, 1.0f, 1.0f);
};

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

// -- the local TRS: THE write funnel (the store, then the Ogre half, the movement epoch and the journal) --
void setLocalPos(DocNode &n, const Vec3 &v);
void setLocalRot(DocNode &n, const Quat &q);
void setLocalScale(DocNode &n, const Vec3 &v);
/// All three at once — one dirty mark instead of three.
void setLocalTrs(DocNode &n, const Vec3 &p, const Quat &r, const Vec3 &s);
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
void setGlobalPos(DocNode &n, const Vec3 &v);
void setGlobalRot(DocNode &n, const Quat &q);
/// BOTH AT ONCE, for a caller that has both — the physics write-back, which runs per body per step: one parent
/// resolution, and none at all when the parent is at identity (what a physics body's parent almost always is).
void setGlobalPosRot(DocNode &n, const Vec3 &v, const Quat &q);
/// The whole world pose, re-expressed exactly (a keepTransform reparent hands the old world TRS straight here).
void setGlobalTrs(DocNode &n, const Trs &world);
/// A world MATRIX: decomposed (iris::decomposeTRS — a shear in `m` is dropped), then setGlobalTrs.
void setGlobalTransform(DocNode &n, const Mat4 &m);

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
/// A camera node says so once, at birth (`setCountsAsMovement(h, false)` in
/// CameraNode's constructor), and a camera that has DOCUMENT CHILDREN counts
/// again — moving it moves whatever is parented under it, and that IS scene
/// movement. Ogre children that carry no document node are the ENGINE's own
/// (a camera's body wire, a light's -Y adapter, a decal's projector box) and
/// are not content: counting those would un-exempt every camera the editor
/// draws a body for.
///
/// THE ENGINE NO LONGER READS IT (U0 3b): the engine learns what moved from the frame's ChangeSet, which each
/// document's mirror drains from the WRITE JOURNAL below — the same writes, by name. The count stays the physics
/// write-back's cheap "did anything else write?" test.
unsigned long long transformWrites();
/// THE WRITE JOURNAL — the change record every frame's ChangeSet is drained from (ENGINE-UPGRADE-1 U0 3b; first
/// built for ENGINE_V2_SPEC V2-1, SPEED-CPU 2026-10-04). The counter above says THAT something moved; the journal
/// says WHAT: every write the counter counts (a transform setter through the one funnel, a reparent, a detach) also
/// records its node — exactly the counter's writes, so a reader of the journal sees everything the counter saw.
///
/// ONE LOG, A CURSOR PER READER. The journal is process-wide (a document can be built on a worker against the
/// staging scene, and every write path touches it), and it has several readers on their own schedules: each
/// document's mirror at its emit (the ChangeSet, every host frame) and each physics world at its write-back (only
/// while it simulates). A shared CLOSE cannot serve them — one reader closing would throw away writes another has
/// not read (the editor's mirror losing a drag to a thumbnail's sync; physics losing its incremental re-check to the
/// mirror's emit) — so each reader holds a `WriteJournalReader`: `take` answers the writes since that reader's own
/// last take, and the log keeps an entry only until every live reader has passed it. A node is logged once per
/// "every reader still owes it", never once per write. With no reader registered nothing is logged at all.
///
/// A take is INCOMPLETE (false) when it cannot name every write: the reader's first take (it never saw what came
/// before it), a write to a node with no document owner (one nobody can walk from), or a reader so far behind that
/// the log's cap dropped entries it had not read. An incomplete take is the caller's cue for its full pass.
class WriteJournalReader {
public:
    /// `wantsWritten`: this reader also hears the journal's WRITTEN half (noteDocumentWrite) — the editor's scene
    /// checker, which re-checks a moved node with its subtree and a written one alone (BISTRO-ENGINE-1's tap, re-homed).
    explicit WriteJournalReader(bool wantsWritten = false);
    ~WriteJournalReader();
    WriteJournalReader(const WriteJournalReader &) = delete;
    WriteJournalReader &operator=(const WriteJournalReader &) = delete;
    /// The DOCUMENT nodes written since this reader's last take (or skip), each once, as live handles (a node destroyed
    /// since its write is gone from the answer; a migrated node answers with its new handle). False = incomplete (above).
    bool take(std::vector<NodeHandle> &out);
    /// ...the same take split in two, for a reader that asked for the written half: `moved` (a journal write reached
    /// the node — its world pose and its subtree's may have changed), `written` (only the node itself changed), each
    /// node once; `gone` = written nodes destroyed since their write.
    bool take(std::vector<NodeHandle> &moved, std::vector<NodeHandle> &written, int *gone = nullptr);
    /// Forget every write so far, unread — a reader whose own writes follow its take (physics' write-back) says so.
    void skip();

    // ---- the log's own bookkeeping (nodegraph.cpp) ----
    unsigned long long cursor = 0;   ///< the sequence number of the first write this reader has not read
    bool               primed = false;
    const bool         wantsWritten;
};
/// Entries the log holds right now (a running diagnostic for the suites: a log no reader drains stays bounded).
std::size_t writeJournalSize();
/// A document write that is not scene movement — the journal's WRITTEN half (WriteJournalReader(true)): a mesh or
/// material swap, a parameter, a visibility, a flag. One relaxed load and nothing else while no reader asked for it.
void noteDocumentWrite(NodeHandle n);
/// NODES THAT CHANGED MEMORY-MANAGER CLASS (a static <-> dynamic migration, one per node), a process-wide
/// running total: what a promotion, a demotion or a reparent's rule-4 round trip cost, in counts
/// (ENGINE-UPGRADE-1 stage 2, mirror.reparent_local).
unsigned long long staticSwitchCount();
/// Does a write to this node's transform count as SCENE MOVEMENT (above)?
/// True for every node by default; a CAMERA says false once, in its
/// constructor. Not a "static" hint and not a visibility flag: it answers one
/// question, "can anything the renderer scans have changed because this node
/// moved?", and the honest answer for the viewer is no.
void setCountsAsMovement(NodeHandle n, bool counts);
/// What the flag above says right now (a suite read; the write path asks the
/// document node itself, see nodegraph.cpp).
bool countsAsMovement(NodeHandle n);

/// EVERYTHING setGlobalPosRot READS AND WRITES, bit for bit: the parent's
/// world position, orientation and scale (what the write undoes) and the node's
/// own local position and orientation (what it writes), all from the store. Two
/// equal witnesses of one node mean a setGlobalPosRot of the SAME world pose
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

// ---- flags ----------------------------------------------------------------

/// Ogre's setVisible walks the node's ATTACHMENTS (spec §2): an empty node has
/// no visibility of its own, which is why the document keeps its own flag and
/// this is the push of it.
void setVisible(NodeHandle n, bool visible, bool cascade);

bool isStatic(NodeHandle n);

// ---- SCENE_STATIC (SPECS/SCENEGRAPH_SPEC.md §6 — the multiplier) -----------
//
// Ogre-Next keeps two node memory managers per SceneManager. Only the DYNAMIC
// one is walked by `updateAllTransforms` every frame; the STATIC one joins the
// update list only for the frames something in it changed
// (SceneManager::highLevelCull tests `mStaticMinDepthLevelDirty`, and
// `updateSceneGraph` clears it again — so static dirties are BATCHED PER FRAME
// by the engine itself, which is upstream's advice in spec §2 and needs no
// batching of ours). The same is true of `updateAllBounds` for the entities
// hanging off those nodes. A scene whose ground, architecture and imported
// props are static therefore pays per-frame transform + bounds work for the
// moving 20% only.
//
// THE RULES, all forced by Ogre and all enforced here:
//
//  1. STATIC IS A SUBTREE PROPERTY. `Node::setParent` gives a child its
//     PARENT's memory manager and `parentDepthLevelChanged` propagates that
//     down the whole subtree, so "this node is static but its child is not"
//     cannot survive a reparent. setStatic therefore switches n AND its whole
//     subtree, and a node's hint takes effect at the subtree's root.
//  2. A STATIC NODE'S PARENT MUST BE STATIC — or be the document root, whose
//     local transform is identity and which nothing ever writes. Ogre says the
//     same in OgreNode.h ("static children, dynamic parent is probably a
//     bug"): a static node's derived transform is refreshed only on static
//     dirties, so a moving dynamic parent would leave it behind.
//  3. ATTACHMENTS TRAVEL WITH THE NODE. `SceneNode::attachObject` THROWS when
//     the object's static flag disagrees with the node's, and lights and PFX2
//     particle definitions cannot switch at all (their object memory managers
//     have no twin). So the engine creates a node's Item in the node's class
//     (OgreMaterials.cpp attachMesh), and setStatic rolls the whole subtree
//     back if any attachment refuses.
//  4. MOVING A STATIC NODE PROMOTES IT. Every transform write goes through
//     this file; a write to a static node demotes it (and its subtree) back to
//     dynamic instead of paying a whole static pass per edit. The document's
//     GRAPH hint is cleared with it (SceneNode::_clearStaticHint) — and ONLY
//     that: since mobility (REALTIME_REFLECTIONS_SPEC §3.3) the user's
//     Static/Movable setting survives a move, because an editor drag is
//     authoring and must not silently rewrite the renderer's classification.
//     A RE-EXPRESSION IS NOT A MOVE (ENGINE-UPGRADE-1 stage 2b): a write inside a
//     ReexpressScope keeps the node's WORLD pose by construction (a reparent with
//     keepTransform re-states it in the new parent's space) — it changes no pose,
//     so it changes no staticness: the static node is marked static-dirty (one
//     static pass at its depth, Ogre's own path) instead of demoted.

/// True when `n` may legally be static: its parent is static, or its parent is
/// the DOCUMENT ROOT (a document node whose own parent is the scene manager's
/// root node). Rule 2 above.
bool canBeStatic(NodeHandle n);

/// Switches `n` AND ITS WHOLE SUBTREE between the two memory managers, moving
/// every attachment with it. Returns false (changing nothing) when the switch
/// is illegal (rule 2) or when some attachment in the subtree refuses (rule 3).
///
/// This is an AUTHORING-TIME call: switching costs a migration per node plus
/// one static pass on the next frame. Never call it per frame.
bool setStatic(NodeHandle n, bool value);

/// THE WRITES INSIDE RE-EXPRESS A POSE, THEY DO NOT MOVE IT (rule 4's exception): while one is alive on this
/// thread, a transform write to a static node notifies Ogre's static pass instead of demoting the subtree.
/// SceneNode::insertChild's keepTransform write is the one user — before it, reparenting a 500-node static
/// fragment cost 1,000 class switches (500 demoted by the write, 500 re-promoted by the hint after it).
struct ReexpressScope {
    ReexpressScope();
    ~ReexpressScope();
    ReexpressScope(const ReexpressScope &) = delete;
    ReexpressScope &operator=(const ReexpressScope &) = delete;
};

/// PROMOTES `n` ALONE — n, its attachments and the ENGINE-OWNED nodes hanging
/// off it (a light's -Y adapter, a decal box: nodes with no document owner,
/// which have no opinion and ride their owner) — and leaves every DOCUMENT
/// child where it is. setStatic(n, true) migrates the whole eligible subtree in
/// one call, which is the right shape for an authoring edit and the wrong one
/// for a pass that must spend a bounded amount per frame: one call on a group
/// over 40,000 props is 40,000 migrations on one frame. Here each document
/// node is its own unit of work, and a top-down caller (the mirror's
/// incremental settle, SceneMirror::kStaticSettleNodesPerSync) reaches the
/// children on their own visits — every one of them with a static parent by
/// then, so every promotion is legal (rule 2) and a child that must stay
/// dynamic simply is never moved (Ogre: a dynamic child of a static parent is
/// valid; it is the opposite that is a bug).
///
/// THE ENGINE-OWNED CHILDREN follow setStatic's reach exactly: they go when
/// `n` itself switches here, or when `carry` says an ancestor's switch would
/// have walked down to `n` (setStatic's subtree walk descends through nodes
/// that were already static, too) — so a top-down pass of promoteNode lands
/// the same static set as one setStatic per switched subtree root. `*switched`
/// reports whether `n` itself changed class.
///
/// Promotion only: a DEMOTION must stay a subtree switch (setStatic(n, false)),
/// because demoting a parent first would leave its static children under a
/// dynamic parent between two calls. Returns whether `n` is static afterwards;
/// false (changing nothing) when the switch is illegal (rule 2) or an
/// attachment refuses (rule 3).
bool promoteNode(NodeHandle n, bool carry, bool *switched);

/// How many nodes in this process currently live in a SCENE_STATIC manager.
/// The benchmark asserts on it; nothing else should need it.
std::size_t staticNodeCount();



}  // namespace graph
}  // namespace iris

#endif  // IRIS_NODEGRAPH_H
