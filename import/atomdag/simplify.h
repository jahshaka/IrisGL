/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_SIMPLIFY_H
#define IRIS_IMPORT_ATOMDAG_SIMPLIFY_H

// ATOM-2 D4 — SIMPLIFY ONE GROUP, with its survivors PLACED (stage 2; the spec's correction 3e).
//
// The group's triangles (global vertex ids) become a DENSE SCRATCH in first-use order — positions, and the attributes
// the simplifier solves: the normal, the first two UV components, the tangent — so meshoptimizer normalises positions
// by the GROUP's extent and target_error = E_k is absolute in mesh units (meshopt_SimplifyErrorAbsolute). Options:
// ErrorAbsolute | Permissive — never LockBorder: under Permissive a UV seam has NO edge quadric, so the attribute
// weights are its only price. Locked: every scratch vertex at a LOCKED POSITION (the group's external border, the
// ladder's offender fans); a lock on any wedge locks them all.
//
// PLACEMENT: meshopt_simplifyWithUpdate collapses as before and then MOVES every unlocked survivor to its quadric
// optimum and solves its attributes, in place, on a copy of the scratch. Each output index then names: the original
// vertex (a locked survivor, or one the solve left bitwise unchanged), or the PLACED copy of that scratch vertex
// (local id n + scratch id) — the builder appends it to the mesh as a new vertex. Every stored attribute carries a
// positive weight (a zero-weight attribute is never solved, and would ride unsolved on a moved vertex). With
// `placement` false the group is re-indexed (meshopt_simplifyWithAttributes: survivors keep their ids — stage 1).
//
// THE WEIGHTS (correction 4): an attribute change da costs the distance W * da * sqrt(sum of the fan's area) in
// meshoptimizer's metric and w * da * t / 0.25 in Nanite's, so per group, with S = the mean over the group's wedges of
// sqrt(the incident area) and t = sqrt(area / triangles):
//     W_normal = 4 t / S      W_uv = 4 t / (128 uvTriSize S), uvTriSize = max(sqrt(uvArea / triangles), 1/1024)
//     W_tangent = 0.0625 W_normal (Nanite's TangentX weight)

#include "import/atomdag/adjacency.h"

#include <vector>

namespace iris
{
namespace atomdag
{

struct Weights
{
    float normal = 0.0f;
    float uv = 0.0f;
    float tangent = 0.0f;
    float tGroup = 0.0f;     ///< sqrt(area / triangles), mesh units
    float sGroup = 0.0f;     ///< mean over the wedges of sqrt(sum of incident area)
    float uvTriSize = 0.0f;
};

/// One group's dense scratch.
struct GroupScratch
{
    std::vector<unsigned> globalOf;   ///< scratch id -> mesh vertex
    std::vector<unsigned> tris;       ///< the input, in scratch ids
    std::vector<float> pos;           ///< n * 3, the original positions
    std::vector<float> attr;          ///< n * attrCount, the original attributes
    size_t attrCount = 0;
    int nrmAt = -1, uvAt = -1, tanAt = -1;   ///< offsets in an attribute record (-1: absent)
    Weights weights;
    std::vector<float> weightOf;      ///< per attribute component
    size_t size() const { return globalOf.size(); }
};

/// The scratch of `tris` (global ids); `attributes` false = positions only (the POSONLY arm); `tangents` = the tangent
/// rides as a solved attribute (placement: a moved vertex needs one; re-indexing keeps the vertex's own).
GroupScratch makeScratch(const MeshView &mesh, const std::vector<unsigned> &tris, bool attributes, bool tangents);

/// The group's local space (adjacency.h LocalSpace) over the scratch: the n originals and n placed slots.
LocalSpace makeSpace(const GroupScratch &sc, const std::vector<unsigned> &weld, const std::vector<unsigned> &compOf,
                     const std::vector<float> &capOf, size_t islandCount);

struct Simplified
{
    std::vector<unsigned> tris;   ///< LOCAL ids: < n the original vertex, >= n the placed copy of scratch id (id - n)
    std::vector<float> pos;       ///< n * 3: the solved positions (placement), else empty
    std::vector<float> attr;      ///< n * attrCount: the solved attributes (placement), else empty
    float resultError = 0.0f;     ///< meshoptimizer's own (absolute, pre-solve, position + attribute)
};

/// The simplification of the scratch to at most `targetTris` under `targetError` (absolute), the scratch vertices
/// flagged in `lock` locked; `placement` false = re-indexing.
Simplified simplifyGroup(const GroupScratch &sc, const std::vector<unsigned char> &lock, size_t targetTris,
                         float targetError, bool placement);

/// The position-quadric estimate of the halving (positions only, no placement): the depth-0 reference.
float positionEstimate(const GroupScratch &sc, const std::vector<unsigned char> &lock, size_t targetTris);

/// THE PRUNE WITHIN THE BUDGET (ours): after the collapses, while the output is above `targetTris`, the output's
/// connected pieces (by weld id) that hold NO locked position are removed whole, smallest box diagonal first, as long
/// as that diagonal is within `budget`. Returns the pieces removed. `tris` are local ids of `space`.
size_t pruneUnlocked(const LocalSpace &space, const std::vector<unsigned> &lockedWelds, size_t targetTris,
                     float budget, std::vector<unsigned> &tris);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_SIMPLIFY_H
