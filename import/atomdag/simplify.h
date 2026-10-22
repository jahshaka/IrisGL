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

// ATOM-2 D4 — SIMPLIFY ONE GROUP, RE-INDEXING (stage 2's placement arm measured a no-go and deleted: the hard 7 fell
// 1.00-1.11x at E = 8-32 for 1.694x level 0's vertex bytes — spikes/atom-2/stage2/NOTES.txt).
//
// The group's triangles (global vertex ids) become a DENSE SCRATCH in first-use order — positions, and the attributes
// the simplifier weighs: the normal and the first two UV components — so meshoptimizer normalises positions by the
// GROUP's extent and target_error = E_k is absolute in mesh units (meshopt_SimplifyErrorAbsolute). Options:
// ErrorAbsolute | Permissive — never LockBorder: under Permissive a UV seam has NO edge quadric, so the attribute
// weights are its only price. Locked: every scratch vertex at a LOCKED POSITION (the group's external border, the
// ladder's offender fans); a lock on any wedge locks them all. The survivors keep their ids
// (meshopt_simplifyWithAttributes): every output index is a vertex of the input.
//
// THE WEIGHTS (correction 4): an attribute change da costs the distance W * da * sqrt(sum of the fan's area) in
// meshoptimizer's metric and w * da * t / 0.25 in Nanite's, so per group, with S = the mean over the group's wedges of
// sqrt(the incident area) and t = sqrt(area / triangles):
//     W_normal = 4 t / S      W_uv = 4 t / (128 uvTriSize S), uvTriSize = max(sqrt(uvArea / triangles), 1/1024)

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
    int nrmAt = -1, uvAt = -1;        ///< offsets in an attribute record (-1: absent)
    Weights weights;
    std::vector<float> weightOf;      ///< per attribute component
    size_t size() const { return globalOf.size(); }
};

/// The scratch of `tris` (global ids); `attributes` false = positions only (the POSONLY arm).
GroupScratch makeScratch(const MeshView &mesh, const std::vector<unsigned> &tris, bool attributes);

/// The group's local space (adjacency.h LocalSpace) over the scratch.
LocalSpace makeSpace(const GroupScratch &sc, const std::vector<unsigned> &weld, const std::vector<unsigned> &compOf,
                     const std::vector<float> &capOf, size_t islandCount);

struct Simplified
{
    std::vector<unsigned> tris;   ///< scratch ids
    float resultError = 0.0f;     ///< meshoptimizer's own (absolute, position + attribute)
};

/// The simplification of the scratch to at most `targetTris` under `targetError` (absolute), the scratch vertices
/// flagged in `lock` locked.
Simplified simplifyGroup(const GroupScratch &sc, const std::vector<unsigned char> &lock, size_t targetTris,
                         float targetError);

/// The position-quadric estimate of the halving (positions only): the depth-0 reference.
float positionEstimate(const GroupScratch &sc, const std::vector<unsigned char> &lock, size_t targetTris);

/// THE PRUNE WITHIN THE BUDGET (ours): after the collapses, while the output is above `targetTris`, the output's
/// connected pieces (by weld id) that hold NO locked position are removed whole, smallest box diagonal first, as long
/// as that diagonal is within `budget`. Returns the pieces removed. `tris` are local ids of `space`.
size_t pruneUnlocked(const LocalSpace &space, const std::vector<unsigned> &lockedWelds, size_t targetTris,
                     float budget, std::vector<unsigned> &tris);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_SIMPLIFY_H
