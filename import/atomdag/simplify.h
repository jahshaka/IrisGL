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

/// THE WEIGHT ARMS (stage 3; the lead picks one at its STOP): Nanite's scale translated per group (correction 4), or
/// POSONLY — every attribute at a tiny positive weight (the positions decide; the attributes ride, re-indexed).
enum class WeightArm { Nanite, PosOnly };

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
    std::vector<float> pos;           ///< n * 3: the positions the SIMPLIFIER reads (the weld's snaps applied, D5;
                                      ///< the originals are the local space's)
    std::vector<float> attr;          ///< n * attrCount, the original attributes
    size_t attrCount = 0;
    int nrmAt = -1, uvAt = -1;        ///< offsets in an attribute record (-1: absent)
    Weights weights;
    std::vector<float> weightOf;      ///< per attribute component
    size_t size() const { return globalOf.size(); }
};

/// The scratch of `tris` (global ids) under the weight arm `arm`.
GroupScratch makeScratch(const MeshView &mesh, const std::vector<unsigned> &tris, WeightArm arm);

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

/// D5 THE WELD WITHIN THE BUDGET (ours; Nanite never merges shells — dossier 1.1, 3.7): the scratch positions of
/// DIFFERENT islands within `radius` (E / 2) of each other are SNAPPED onto one another before the collapse, so kit-
/// bashed shells become one surface the simplifier can reduce. Greedy and deterministic, in the scratch's first-use
/// order over its positions (weld ids): a position takes the NEAREST earlier anchor of another island within the
/// radius that holds no position of its own island yet, else becomes an anchor itself; every snap moves a position at
/// most `radius`. The locked positions (the external border, any lock round's fans) are neither moved nor anchors.
/// The mesh's data is untouched (re-indexing: a snapped survivor keeps its own position), so the gap is a deviation
/// the measurement prices on the REAL output; the islands a snap joins become one in `space` (the island rules see one
/// surface; each vertex keeps its own island's cap). Returns the positions snapped.
size_t weldWithin(GroupScratch &sc, LocalSpace &space, const std::vector<unsigned char> &lock, float radius);

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
