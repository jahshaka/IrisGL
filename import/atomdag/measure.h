/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_MEASURE_H
#define IRIS_IMPORT_ATOMDAG_MEASURE_H

// ATOM-2 D6 — THE PRICE: THE MEASURED ONE-STEP DEVIATION of a group's OUTPUT against its INPUT (the triangles it was
// given — never level 0: Nanite's max(children, this step) with a MEASURED step, dossier 3.2; the cumulative drift
// from level 0 is a diagnostic only, MeshBake::dagDrift). Exact both ways, no sampled term, no margin:
//   1. every REMOVED input vertex against the output surface;
//   2. every LOST input facet walked against the output (facetMax: the distance from a union of triangles peaks
//      inside a facet), the input's island cap on both (a level-0 point is charged at most its island's extent);
//   5. every ADDED output facet walked against the input (surface the step created: a fold, a bridge across air);
//   3. a WHOLLY DROPPED island (no output vertex of it) charged the box diagonal of the part the input held;
//   4. an EXTERNAL position the output lost (a crack in waiting) charged its distance to the output.
// The step = the maximum, at least the arithmetic floor. Every term also names its OFFENDERS past the budget, the
// ladder's locks: the positions whose fans are locked, and the largest input triangle of a dropped island too big
// to drop. `exact` false = THE DECISION ONLY (every query stops at the budget: an attempt's "within, or which
// offenders"); true = the exact maximum of every term (the kept result's stored step).

#include "import/atomdag/adjacency.h"

#include <vector>

namespace iris
{
namespace atomdag
{

struct StepMeasure
{
    float removedVertex = 0.0f;
    float lostFacet = 0.0f;
    float addedFacet = 0.0f;
    float droppedIsland = 0.0f;
    float lostLocked = 0.0f;
    float step = 0.0f;
    int lockedLost = 0;              ///< external positions the output lost
    int islandsDropped = 0;
    std::vector<unsigned> fanWelds;   ///< offender positions (weld ids, sorted unique): lock every input triangle at them
    std::vector<unsigned> keepTris;   ///< offender islands: the input triangle (index into the input) to lock whole
};

/// `inTris` and `outTris` are LOCAL ids of `space`.
StepMeasure measureStep(const LocalSpace &space,
                        const std::vector<unsigned> &inTris, const std::vector<unsigned> &outTris,
                        const std::vector<unsigned> &externalWelds, float budget, float floorLen, bool exact);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_MEASURE_H
