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

// ATOM-2 D4 — SIMPLIFY ONE GROUP (stage 1: RE-INDEXING; stage 2 places vertices).
//
// The group's triangles (global vertex ids) become a DENSE SCRATCH (first-use order: positions + the weighed
// attributes), so meshoptimizer normalises positions by the GROUP's extent and target_error = E_k is absolute in mesh
// units (meshopt_SimplifyErrorAbsolute). Options: ErrorAbsolute | Permissive — never LockBorder: under Permissive a
// UV seam has NO edge quadric, so the attribute weights below are its only price (the preflight's correction 3).
// Locked: every scratch vertex at a LOCKED POSITION (the group's external border, and the ladder's offender fans).
//
// THE WEIGHTS (correction 4, the algebra replacing the LESSONS block): meshoptimizer prices a displacement d off a
// fan as (d/L)^2 and an attribute change da as sum_fan A'(W da)^2 with A' = A / L^2, so an attribute change costs the
// distance d = W * da * sqrt(sum A_fan) — scale-free. Nanite's quadric (dossier 1.3c) prices it as w * da * t / 0.25
// (positions scaled to a triangle size 0.25). Matched per group with S = the mean over the group's wedges of
// sqrt(sum of incident triangle area) and t = sqrt(area / triangles):
//     W_normal = 4 t / S          (about 1.63 on a manifold)
//     W_uv     = 4 t / (128 uvTriSize S),  uvTriSize = max(sqrt(uvArea / triangles), 1/1024)
// computed before the group's first simplification.

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

Weights groupWeights(const MeshView &mesh, const std::vector<unsigned> &tris);

struct Simplified
{
    std::vector<unsigned> tris;   ///< global vertex ids
    float resultError = 0.0f;     ///< meshoptimizer's own (absolute, pre-measurement, position + attribute)
};

/// The re-indexing simplification of `tris` to at most `targetTris` triangles under `targetError` (absolute), every
/// vertex whose weld id is in `lockedWelds` (sorted) locked. `attributes` false = positions only (the POSONLY arm and
/// the depth-0 estimate).
Simplified simplifyGroup(const MeshView &mesh, const std::vector<unsigned> &weld, const std::vector<unsigned> &tris,
                         const std::vector<unsigned> &lockedWelds, size_t targetTris, float targetError,
                         bool attributes, const Weights &weights);

/// THE PRUNE WITHIN THE BUDGET (ours): after the collapses, while the output is above `targetTris`, the output's
/// connected pieces (by position) that hold NO locked position are removed whole, smallest box diagonal first, as
/// long as that diagonal is within `budget` — a piece past the budget would make the step unaffordable, and a piece
/// with a locked position is the group's border (never removed: no crack). Returns the pieces removed.
size_t pruneUnlocked(const MeshView &mesh, const std::vector<unsigned> &weld, const std::vector<unsigned> &lockedWelds,
                     size_t targetTris, float budget, std::vector<unsigned> &tris);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_SIMPLIFY_H
