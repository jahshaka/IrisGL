/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/scenegraph/scenepicking.h"

#include <utility>
#include <vector>

#include "core/geometry/aabb.h"
#include "core/geometry/trimesh.h"
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

/// The exact document semantics, applied once per CANDIDATE: `pickable`, the
/// effective visibility and the `pickingGroups` ALL-test.
inline bool passesFlags(SceneNode *node, uint64_t pickingMask, bool allowUnpickable)
{
    if (!node->isPickable() && !allowUnpickable) return false;
    // A HIDDEN MESH IS NEVER A HIT, and hidden means EFFECTIVELY hidden — by
    // its own flag or any ancestor's (SceneNode::isVisibleInScene; the rule the
    // renderer draws by, RENDER_PIPELINE_AUDIT 1.1/1.2). A script that hides a
    // node and raycasts in the same breath must not hit it. O(depth) per
    // candidate; candidates are few.
    if (!node->isVisibleInScene()) return false;
    return (node->pickingGroups & pickingMask) == pickingMask;
}

/// The segment o + t d, t in [0, 1], against the box [mn, mx]: the slab test,
/// inclusive at the faces (a flat mesh has a zero-thickness box), an axis the
/// segment runs parallel to decided by the start point alone. Plain floats: this
/// runs once per mesh of the scene per pick, and the Debug build is the daily one.
inline bool segmentHitsBox(const float o[3], const float d[3], const float inv[3], const float mn[3],
                           const float mx[3])
{
    float t0 = 0.0f, t1 = 1.0f;
    for (int i = 0; i < 3; ++i) {
        if (d[i] == 0.0f) {
            if (o[i] < mn[i] || o[i] > mx[i]) return false;
            continue;
        }
        float tn = (mn[i] - o[i]) * inv[i], tf = (mx[i] - o[i]) * inv[i];
        if (tn > tf) std::swap(tn, tf);
        if (tn > t0) t0 = tn;
        if (tf < t1) t1 = tf;
        if (t0 > t1) return false;
    }
    return true;
}

/// THE BROAD PHASE (see the header): every mesh node with a triangle index whose
/// world box the segment crosses — over the scene's flat table (graph::PickTable):
/// a valid slot is six floats read in place, and only a stale one (a write or a
/// content swap reached its node since the last pick) touches the node to refresh.
void collectCandidates(Scene *scene, const Vec3 &segStart, const Vec3 &segEnd,
                       std::vector<MeshNode *> &out)
{
    out.clear();
    const float o[3] = { segStart.x(), segStart.y(), segStart.z() };
    const float d[3] = { segEnd.x() - o[0], segEnd.y() - o[1], segEnd.z() - o[2] };
    const float inv[3] = { d[0] != 0.0f ? 1.0f / d[0] : 0.0f, d[1] != 0.0f ? 1.0f / d[1] : 0.0f,
                           d[2] != 0.0f ? 1.0f / d[2] : 0.0f };
    graph::PickTable &table = scene->pickTable;
    const std::size_t count = table.slotCount();
    for (std::size_t i = 0; i < count; ++i) {
        graph::PickTable::State st = table.state(i);
        if (st == graph::PickTable::NoGeometry) continue;
        SceneNode *n = table.node(i);
        if (!n) continue;
        if (st == graph::PickTable::Stale) {
            MeshNode *mn = static_cast<MeshNode *>(n);
            Mesh *mesh = mn->mesh.data();   // the member, not getMesh(): no shared-pointer copy
            if (!mesh || !mesh->getTriMesh()) { table.markNoGeometry(i); continue; }   // nothing to test
            if (!table.refresh(i, mesh->aabb.getMin(), mesh->aabb.getMax())) continue;
        }
        const float *b = table.box(i);
        if (segmentHitsBox(o, d, inv, b, b + 3)) out.push_back(static_cast<MeshNode *>(n));
    }
}

}  // namespace

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
