/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/scenepicking.h"

#include <vector>

#include "core/geometry/boundingsphere.h"
#include "core/geometry/trimesh.h"
#include "core/math/intersectionhelper.h"
#include "core/math/mat4.h"
#include "document/assets/mesh.h"
#include "document/scenegraph/meshnode.h"
#include "document/scenegraph/nodegraph.h"
#include "document/scenegraph/scene.h"
#include "document/scenegraph/scenenode.h"

namespace iris
{
namespace picking
{

namespace
{

bool gLastUsedEngine = false;

/// The exact document semantics, applied once per CANDIDATE. `pickingGroups`
/// is an ALL-test and Ogre's query mask is an ANY-test (see scenepicking.h), so
/// this is where groups are really decided — and where `pickable` is decided
/// too when the fallback path produced the candidates.
inline bool passesFlags(SceneNode *node, uint64_t pickingMask, bool allowUnpickable)
{
    if (!node->isPickable() && !allowUnpickable) return false;
    return (node->pickingGroups & pickingMask) == pickingMask;
}

/// Every mesh node whose bounds the segment could cross. Two sources, one
/// shape: see the header.
void collectCandidates(Scene *scene, const Vec3 &segStart, const Vec3 &segEnd,
                       std::vector<MeshNode *> &out)
{
    out.clear();
    SceneNodePtr root = scene->getRootNode();
    graph::SceneHandle handle = root ? graph::sceneOf(root->graphNode()) : nullptr;

    gLastUsedEngine = handle && graph::hasQueryableGeometry(handle);
    if (gLastUsedEngine) {
        // THE ENGINE'S BROAD PHASE. The mask keeps unpickable geometry out of
        // the SIMD sweep itself — that is the bulk rejection; `pickable` is
        // still re-checked exactly on what comes back, because a node whose
        // flag changed since the last mirror sync has stale query flags.
        static std::vector<graph::RayCandidate> hits;
        const Vec3 dir = segEnd - segStart;
        graph::rayQuery(handle, segStart, dir, 0xFFFFFFFFu, hits);
        const float segLength = dir.length();
        out.reserve(hits.size());
        for (const graph::RayCandidate &c : hits) {
            // The query's ray is infinite; the document's is a SEGMENT. An AABB
            // entered beyond the segment's far end cannot hold a hit.
            if (c.distance > segLength) continue;
            if (c.node->getSceneNodeType() != SceneNodeType::Mesh) continue;
            out.push_back(static_cast<MeshNode *>(c.node));
        }
        return;
    }

    // THE FALLBACK: no engine geometry for this document. The scene's mesh
    // registry is a flat hash of every mesh node — no tree walk — and the
    // bounding sphere is the broad phase, in the node's local space (the same
    // test both old implementations used).
    out.reserve(scene->meshes.size());
    for (const MeshNodePtr &mn : scene->meshes) {
        if (!mn) continue;
        MeshPtr mesh = mn->getMesh();
        if (!mesh) continue;
        const Mat4 inv = mn->getGlobalTransform().inverted();
        const Vec3 a = inv * segStart, b = inv * segEnd;
        const BoundingSphere sphere = mesh->getBoundingSphere();
        float t;
        Vec3 hitPoint;
        if (IntersectionHelper::raySphereIntersects(a, (b - a).normalized(), sphere.pos,
                                                    sphere.radius, t, hitPoint))
            out.push_back(mn.data());
    }
}

}  // namespace

bool lastUsedEngineBroadPhase() { return gLastUsedEngine; }

QList<MeshPick> raycastMeshes(Scene *scene, const Vec3 &segStart, const Vec3 &segEnd,
                              uint64_t pickingMask, bool allowUnpickable)
{
    QList<MeshPick> out;
    if (!scene || !scene->getRootNode()) return out;

    std::vector<MeshNode *> candidates;
    collectCandidates(scene, segStart, segEnd, candidates);

    for (MeshNode *meshNode : candidates) {
        if (!passesFlags(meshNode, pickingMask, allowUnpickable)) continue;
        MeshPtr mesh = meshNode->getMesh();
        if (!mesh || !mesh->getTriMesh()) continue;

        // THE NARROW PHASE, ours: the segment into the mesh's local space, hits
        // back out to world space.
        const Mat4 world = meshNode->getGlobalTransform();
        const Mat4 inv = world.inverted();
        const Vec3 a = inv * segStart, b = inv * segEnd;

        QList<TriangleIntersectionResult> results;
        if (!mesh->getTriMesh()->getSegmentIntersections(a, b, results)) continue;
        for (const TriangleIntersectionResult &r : results) {
            MeshPick p;
            p.node = meshNode->sharedFromThis();
            p.hitPoint = world * r.hitPoint;
            p.distanceSqrd = (p.hitPoint - segStart).lengthSquared();
            p.triangleIndex = r.triangleIndex;
            out.append(p);
        }
    }
    return out;
}

}  // namespace picking
}  // namespace iris
