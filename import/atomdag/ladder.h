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
// THE LAW (the lead's word on the stage-2 read, replacing "every level <= 2 x the children's stored" — ATOM-7's chain-
// quality rule, which D6's carry breaks by construction; the no-pops law is the runtime's one-pixel switch, which holds
// for any stored error): every group is simplified under a GRANTED budget E, and
//   (a) its measured step <= E — the ladder never forces past it;
//   (b) E <= 2 x max over its children of max(child's stored error, the budget the child was last granted) — a budget
//       at most DOUBLES per level from what its children carried (a group of leaves never granted one: 2 x the position
//       quadric's own estimate of its halving, positions only, the external border locked).
// What a member ASKS (build.cpp Cl::ask): a new parent 2 x its stored error; a member carried for its BUDGET (no result
// within E after the retries) 2E; a member carried for its CLUSTER COUNT (a result within E that still needs as many
// clusters — the 256-vertex cap) E again, unchanged: its regroup with other members may reduce it; an ALL-CARRIED level
// (every group carried, the regroup identical) is the one place the level's budgets double, and each such re-run is a
// recorded pass of its own. The group's E = the largest ask (floored). Every group, accepted or carried, is a GroupRecord
// with its granted E, its measured step and its members (atom2.ladder_never_forced replays (a) and (b)).
//
// A CARD GROUP (D9, cards.h) — more than half its input in islands under kCardTriangles — merges its neighbouring
// cards into quads, each set priced within E; the rest of its input (the branches, the cards no set took) is reduced
// the ordinary way under the same E; the attempts are the sets priced and the rest's simplifies.
//
// THE ATTEMPTS: the group is simplified to half (126 x ceil(T / 256) triangles, Nanite's parent count) with E as the
// simplifier's first guess, its pieces with no locked position pruned within E, and MEASURED (measure.h); a result
// within the budget that is not at the target is tried again with the guess raised (8 E, then unbounded) — the smaller
// count is always tried; a result past it locks its offenders' fans (a removed vertex or a lost or added facet past E,
// an external position the output lost) and the largest triangle of every island too big to drop, and is tried again —
// at most kRetries lock rounds. The group keeps the MOST REDUCED result that came within its budget; the builder accepts
// it when it needs FEWER CLUSTERS than the group's input (a true level step). A group already at or under its target is
// not simplified (step 0).

#include "import/atomdag/cards.h"
#include "import/atomdag/measure.h"
#include "import/atomdag/simplify.h"

#include <vector>

namespace iris
{
namespace atomdag
{

constexpr float kLadderFactor = 2.0f;   ///< the doubling law: a budget at most doubles per level
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
    float budget = 0.0f;          ///< the granted E
    float step = 0.0f;            ///< the accepted (or the last) attempt's measured step
    float estimate = 0.0f;        ///< the simplifier's own result_error of the kept attempt
    float depth0Estimate = -1.0f; ///< the position quadric's estimate of a group of leaves (children stored 0)
    int attempts = 0;
    int pruned = 0;
    size_t lockedPositions = 0;   ///< external + ladder positions locked at the kept attempt
    StepMeasure terms;            ///< the kept (or the last) attempt's terms
    bool cards = false;           ///< a card group (cards.h): merged, not collapsed
    CardMerge merge;              ///< ...its sets, its quads' corners (local ids space.n + i) and its coverage
    double msSimplify = 0.0, msMeasure = 0.0;   ///< this group's time in the simplifier and in the measurement
    double msExact = 0.0;                       ///< ...of which the kept result's exact measurement
};

/// `inTris` global ids; `ask` = the largest of the members' asks (0: a group of leaves never granted a budget — the
/// estimate is taken); `targetTris` = the output's target (parentTarget, or less when the input at its target would not
/// merge into fewer clusters).
GroupResult reduceGroup(const MeshView &mesh, const std::vector<unsigned> &weld, const Islands &islands,
                        const std::vector<unsigned> &inTris, const std::vector<unsigned> &externalWelds, float ask,
                        float floorLen, size_t targetTris, bool cardsAllowed = true);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_LADDER_H
