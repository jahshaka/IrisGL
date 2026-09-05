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
// THE BROAD PHASE IS THE ENGINE'S (SPECS/SCENEGRAPH_SPEC.md §2). Ogre's
// `RaySceneQuery` sweeps the entity SoA four objects at a time and tests the
// same world AABBs the renderer culls with — tighter than our sphere, masked
// inside the sweep, and with no document walk at all. Ogre's queries are
// AABB-precision and have no triangle path (`IntersectionSceneQuery` throws),
// so the NARROW PHASE STAYS OURS: TriMesh segment intersection, in the node's
// local space, reporting the triangle index.
//
// THE FALLBACK, stated rather than hidden. The query can only see geometry the
// ENGINE holds — i.e. meshes a SceneMirror has attached. A document nobody has
// mirrored (the document-only suites; the `--headless` document paths; a node
// added since the last sync) has none, and a query there answers nothing. So
// when the scene manager holds no query-able geometry at all, candidates come
// from the scene's own MESH REGISTRY instead (a flat hash — not a tree walk)
// with the bounding-sphere test. One narrow phase, one set of semantics, two
// candidate sources; the difference is confined to `collectCandidates`.
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
/// mask`), kept exact — Ogre's query mask is an ANY-test and cannot express it,
/// so it is applied to the candidates, which costs O(hits) instead of
/// O(scene). `allowUnpickable` ignores the per-node `pickable` flag.
QList<MeshPick> raycastMeshes(Scene *scene, const Vec3 &segStart, const Vec3 &segEnd,
                              uint64_t pickingMask = 0, bool allowUnpickable = false);

/// Did the last raycastMeshes() use the engine's broad phase? Diagnostic, for
/// the suites that pin both paths against each other.
bool lastUsedEngineBroadPhase();

}  // namespace picking
}  // namespace iris

#endif  // IRIS_SCENEPICKING_H
