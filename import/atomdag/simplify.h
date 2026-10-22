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
// THE WEIGHTS (the lead's pick at stage 3's STOP, measured): EVERY ATTRIBUTE AT A TINY POSITIVE WEIGHT (1e-6) — the
// positions decide, the attributes ride re-indexed (a zero weight would be remapped out of the quadrics, correction
// 3c). Nanite's scale translated per group (correction 4) drew 1.6x the hard 7's triangles at E = 16 (42,228 against
// 27,015) for no visible difference in the owner's picture pairs (spikes/atom-2/stage3/pictures, NOTES.txt); deleted.

#include "import/atomdag/adjacency.h"

#include <vector>

namespace iris
{
namespace atomdag
{

/// One group's dense scratch.
struct GroupScratch
{
    std::vector<unsigned> globalOf;   ///< scratch id -> mesh vertex
    std::vector<unsigned> tris;       ///< the input, in scratch ids
    std::vector<float> pos;           ///< n * 3, the positions
    std::vector<float> attr;          ///< n * attrCount, the original attributes
    size_t attrCount = 0;
    int nrmAt = -1, uvAt = -1;        ///< offsets in an attribute record (-1: absent)
    std::vector<float> weightOf;      ///< per attribute component
    size_t size() const { return globalOf.size(); }
};

/// The scratch of `tris` (global ids).
GroupScratch makeScratch(const MeshView &mesh, const std::vector<unsigned> &tris);

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
