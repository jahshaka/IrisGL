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
// SPECS/SCENEGRAPH_SPEC.md D2: there is exactly ONE hierarchy in this program
// and it is Ogre-Next's. `iris::SceneNode` keeps its name and its public API,
// but it no longer STORES a position, a rotation, a scale, a parent, a child
// list or a cached matrix: it owns an `Ogre::SceneNode` and forwards.
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
// Almost everything the two threads touch is disjoint by construction: they
// create into DIFFERENT SceneManagers (the worker's fragments are born in the
// staging manager), so different `mSceneNodes` vectors and different node
// memory managers. The single piece of genuinely shared mutable state was
// `Ogre::Id::generateNewId<T>()` — one function-local static counter per type,
// incremented non-atomically, with an upstream comment saying it assumed no
// one would do this. A lost increment there hands two live objects one id,
// which in this file also aliases two document handles onto one owner slot.
//
// FIXED WHERE IT LIVES: `ogre-patches/0015-id-generator-atomic-counter.patch`
// makes the counter a relaxed `std::atomic`. That is the whole fix — no lock
// on the engine's side, no main-thread marshalling of the importer, and no
// second transform store (the "worker builds pure data" alternative would need
// one, and §6a rejects that). Re-run `irisgl/scripts/build-ogre.sh` after
// pulling this: an engine built before patch 0015 still has the racy counter.
// -----------------------------------------------------------------------------

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

/// True once an Ogre::Root exists — i.e. once the engine has booted. Creating a
/// document node before that is a programming error, not a supported mode.
bool available();

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
/// document's handles are all dangling — the caller's job is to say so loudly
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

NodeHandle parentOf(NodeHandle n);
std::size_t childCount(NodeHandle n);
NodeHandle childAt(NodeHandle n, std::size_t i);
/// Position among its parent's children, or -1 when it has no parent. Ogre
/// keeps children in a vector, so this is the sibling index undo needs.
int indexInParent(NodeHandle n);

/// Re-parents `child` under `parent` at `index` (-1 = append). Both must belong
/// to the same SceneHandle; use migrate() otherwise. The LOCAL transform is
/// preserved — the caller decides what to do about the world one.
void attach(NodeHandle parent, NodeHandle child, int index);

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

// ---- transforms -----------------------------------------------------------

Vec3 localPos(NodeHandle n);
Quat localRot(NodeHandle n);
Vec3 localScale(NodeHandle n);
void setLocalPos(NodeHandle n, const Vec3 &v);
void setLocalRot(NodeHandle n, const Quat &q);
void setLocalScale(NodeHandle n, const Vec3 &v);
/// Writes all three at once — one dirty mark instead of three.
void setLocalTrs(NodeHandle n, const Vec3 &p, const Quat &r, const Vec3 &s);

Mat4 localTransform(NodeHandle n);
/// The world transform, RESOLVED: Ogre's `_getFullTransformUpdated()` walks up
/// and recomputes only what is dirty. This is the honest replacement for the
/// old getGlobalTransform() double-writer (audit F2) — same shape, but it is
/// Ogre's own dirty-gated path rather than an unconditional recompose.
Mat4 globalTransform(NodeHandle n);
Vec3 globalPos(NodeHandle n);
Quat globalRot(NodeHandle n);
void setGlobalPos(NodeHandle n, const Vec3 &v);
void setGlobalRot(NodeHandle n, const Quat &q);
void setGlobalTransform(NodeHandle n, const Mat4 &m);

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
//     hint is cleared with it — see SceneNode::setStaticHint.

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

/// How many nodes in this process currently live in a SCENE_STATIC manager.
/// The benchmark asserts on it; nothing else should need it.
std::size_t staticNodeCount();

/// Debug/diagnostic: how many live handles this process has made.
std::size_t liveNodeCount();

// ---- picking: the broad phase (SPECS/SCENEGRAPH_SPEC.md §2) ----------------
//
// Ogre's ray queries are AABB-precision and have no triangle path, so the
// TRIANGLE NARROW PHASE STAYS OURS (V-snap reads the triangle index back).
// What moves to the engine is the broad phase: instead of walking the document
// tree and testing a bounding SPHERE per mesh, `rayQuery` runs
// `RaySceneQuery` — a 4-wide SIMD sweep of the entity SoA that tests the same
// WORLD AABBs the renderer culls with, and is masked in the sweep itself.
//
// Hits come back as DOCUMENT nodes: every engine object hangs off an
// `Ogre::SceneNode`, and that node's back-pointer names the handle that owns
// it (nodegraph.cpp's owner table — the UserObjectBindings role, done as a
// flat array because Any allocates).

/// One broad-phase candidate: the document node an engine object hangs off,
/// and the ray parameter at which the ray entered its world AABB.
struct RayCandidate
{
    SceneNode *node;
    float distance;
};

/// True when `s` holds query-able geometry at all. A document that no mirror
/// has met yet has NONE — nothing has been attached to its nodes — and picking
/// there falls back to the document's own bounds walk (see picking.cpp).
bool hasQueryableGeometry(SceneHandle s);

/// The broad phase. `out` is CLEARED and filled with one entry per distinct
/// document node whose geometry's world AABB the ray crosses, unsorted.
///
/// TWO ENGINE RULES the caller inherits: the sweep also rejects anything whose
/// LAYER_VISIBILITY bit is clear (an invisible object is not a candidate), and
/// the AABBs it reads are the ones the last `updateSceneGraph` computed — so a
/// node moved since the last frame is tested at its previous position.
void rayQuery(SceneHandle s, const Vec3 &origin, const Vec3 &dir, unsigned queryMask,
              std::vector<RayCandidate> &out);

/// The query-flag vocabulary — THE one place document picking semantics are
/// mapped onto Ogre's masks. Only the `pickable` split lives here: Ogre's mask
/// test is `(flags & mask) != 0` (ANY) while `pickingGroups` is an ALL test
/// (`(groups & mask) == mask`), so groups cannot be expressed as query flags
/// and stay an exact test on the CANDIDATES, which costs O(hits), not O(scene).
enum : unsigned {
    kPickableQueryBit = 1u << 0,
    kUnpickableQueryBit = 1u << 1,
};

/// Pushes `pickable` onto every engine object attached to `n` as query flags.
/// Called by the mirror when it attaches geometry and when the flag changes;
/// a node with no attachments remembers nothing (there is nothing to query).
void setPickable(NodeHandle n, bool pickable);

}  // namespace graph
}  // namespace iris

#endif  // IRIS_NODEGRAPH_H
