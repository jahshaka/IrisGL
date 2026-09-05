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
// THREADING: Ogre's id generator is a plain non-atomic counter with a comment
// saying so (OgreId.h: "assumes creation of new objects can't be made from
// multiple threads"). createNode/destroyNode/migrate therefore take a process
// mutex. Transform writes do not — they touch only their own SoA slot.
// -----------------------------------------------------------------------------

#include <cstddef>

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
/// SCENE_STATIC (spec §6): a static node is skipped by updateAllTransforms
/// entirely — that is where the multiplier is. Switching costs a full static
/// pass, so this is an authoring-time hint, never a per-frame call. Moving a
/// static node afterwards still works: setLocalPos notifies the manager.
void setStatic(NodeHandle n, bool value);

/// Debug/diagnostic: how many live handles this process has made.
std::size_t liveNodeCount();

}  // namespace graph
}  // namespace iris

#endif  // IRIS_NODEGRAPH_H
