/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_LADDER_H
#define IRIS_IMPORT_ATOMDAG_LADDER_H

// ATOM-2 D6 — THE LADDER: no forced halving, nothing at any price.
//
// A group's BUDGET is E_k = 2 x the error its children carry (their stored error, or the budget a carried child
// already failed at — so a carried group meets a DOUBLED budget); a group of leaves reads 2 x the position quadric's
// own estimate of its halving (positions only, the external border locked). THE ATTEMPTS: the group is simplified to
// half (126 x ceil(T / 256) triangles, Nanite's parent count) with E_k as the simplifier's first guess, its pieces with
// no locked position pruned within E_k, and MEASURED (measure.h); a result within the budget that is not at the target
// is tried again with the guess raised (8 E_k, then unbounded) — the smaller count is always tried; a result past it
// locks its offenders' fans (a removed vertex or a lost or added facet past E_k, an external position the output lost)
// and the largest triangle of every island too big to drop, and is tried again — at most kRetries lock rounds. The
// group keeps the MOST REDUCED result that came within its budget. THE ACCEPTANCE IS THE BUDGET, NEVER A SHED RATIO
// (the lead's rule after the stage-1 read): the builder accepts that result when it needs FEWER CLUSTERS than the
// group's input (a true level step — the 128-triangle / 256-vertex caps decide the count, build.cpp); a group with no
// result within its budget, or whose best still needs as many clusters, is CARRIED UP UNCHANGED: its clusters return
// to the next level's pool and meet the doubled
// budget there. A group already at or under its target is not simplified (step 0).

#include "import/atomdag/measure.h"
#include "import/atomdag/simplify.h"

#include <vector>

namespace iris
{
namespace atomdag
{

constexpr float kLadderFactor = 2.0f;   ///< the doubling law: a halving may deviate twice its children
constexpr int kRetries = 4;             ///< lock rounds after the first attempt

/// The target of a group of `triangles`: Nanite's parent count, 126 x ceil(T / 256) triangles.
inline size_t parentTarget(size_t triangles) { return 126u * ((triangles + 255u) / 256u); }

struct GroupResult
{
    bool accepted = false;        ///< a result within the budget (the builder still carries it if it needs as many clusters)
    bool simplified = false;      ///< false: at or under its target already (step 0)
    std::vector<unsigned> out;    ///< the accepted output, LOCAL ids of `space` (scratch ids)
    GroupScratch scratch;         ///< the group's scratch (globalOf maps a local id to the mesh)
    LocalSpace space;             ///< the group's local space
    float budget = 0.0f;
    float step = 0.0f;            ///< the accepted (or the last) attempt's measured step
    float estimate = 0.0f;        ///< the simplifier's own result_error of the kept attempt
    float depth0Estimate = -1.0f; ///< the position quadric's estimate of a group of leaves (children stored 0)
    int attempts = 0;
    int pruned = 0;
    size_t lockedPositions = 0;   ///< external + ladder positions locked at the kept attempt
    StepMeasure terms;            ///< the kept (or the last) attempt's terms
    double msSimplify = 0.0, msMeasure = 0.0;   ///< this group's time in the simplifier and in the measurement
    double msExact = 0.0;                       ///< ...of which the kept result's exact measurement
};

/// `inTris` global ids; `childError` = max over the members of (stored, carried budget), 0 for a group of leaves;
/// `targetTris` = the output's target (parentTarget, or less when the input at its target would not merge into fewer
/// clusters).
GroupResult reduceGroup(const MeshView &mesh, const std::vector<unsigned> &weld, const Islands &islands,
                        const std::vector<unsigned> &inTris, const std::vector<unsigned> &externalWelds,
                        float childError, float childStored, float floorLen, bool attributes, size_t targetTris);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_LADDER_H
