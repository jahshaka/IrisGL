/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SCENEPICKING_H
#define IRIS_SCENEPICKING_H

// -----------------------------------------------------------------------------
// iris::picking — THE segment/mesh intersection, once.
//
// SPECS/SCENEGRAPH_AUDIT.md F13 recorded two ray-cast implementations that had
// drifted apart in both directions: `Scene::rayCast` honoured `pickingGroups`
// but reported no triangle index, `ScenePicker::pickMeshes` reported the
// triangle index (V-snap depends on it) but ignored groups; both walked the
// document tree recursively and both broad-phased on a bounding SPHERE.
//
// This is the one implementation. `Scene::rayCast` and Studio's `ScenePicker`
// are now two ENTRY POINTS onto it — they differ only in what they add
// afterwards (Scene::rayCast adds nothing; ScenePicker adds the light / decal /
// camera / viewer origin spheres, which are editor helpers and have no
// document geometry to intersect).
//
// THE BROAD PHASE IS THE DOCUMENT'S (ENGINE-UPGRADE-U2 stage 1). Each mesh
// node's baked local AABB (`Mesh::aabb`) carried by its WORLD pose from the
// document's transform store — the exact box of the transformed box, cached
// per store epoch (graph::worldBox) — and one slab test of the SEGMENT against
// it, over the scene's flat broad-phase table (graph::PickTable: six floats a
// mesh, pushed stale by the store's invalidation — no tree walk, no node
// touched for a valid slot): O(meshes) per pick, never per frame. It was Ogre's `RaySceneQuery` over the engine's world AABBs,
// which (a) answered from the LAST updateSceneGraph — a node moved since the
// last frame was tested at its old place — (b) could only see geometry a
// SceneMirror had attached (a document-only fallback with a bounding sphere
// stood beside it), and (c) made the UI thread call into Ogre, which a render
// thread forbids. One path now, exact on the frame it is asked, on any document.
// The NARROW PHASE IS OURS and stays: TriMesh segment intersection, in the
// node's local space, reporting the triangle index (Ogre has no triangle path).
// -----------------------------------------------------------------------------

#include <QList>

#include "core/math/vec.h"
#include "irisglfwd.h"

namespace iris
{

/// One mesh hit: the document node, the world-space hit point, and the
/// triangle of the node's TriMesh that was hit.
struct MeshPick
{
    SceneNodePtr node;
    Vec3 hitPoint;
    float distanceSqrd = 0.0f;   ///< from the segment's start
    int triangleIndex = -1;
};

namespace picking
{

/// Every triangle-level hit on a mesh node of `scene` along the segment,
/// unsorted.
///
/// `pickingMask` is the document's ALL-test (`(node->pickingGroups & mask) ==
/// mask`), applied to the candidates with `pickable` and the effective
/// visibility. `allowUnpickable` ignores the per-node `pickable` flag.
QList<MeshPick> raycastMeshes(Scene *scene, const Vec3 &segStart, const Vec3 &segEnd,
                              uint64_t pickingMask = 0, bool allowUnpickable = false);


}  // namespace picking
}  // namespace iris

#endif  // IRIS_SCENEPICKING_H
