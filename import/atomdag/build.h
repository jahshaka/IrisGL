/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_BUILD_H
#define IRIS_IMPORT_ATOMDAG_BUILD_H

// ATOM-2 — THE CLUSTER DAG BUILDER OF OUR OWN (SPECS/ATOM_2_SPEC.md part A; replaces clusterlod.h, its patch stack
// 0002-0005 and meshbake.cpp's clusterdag:: section). The structure is Nanite's (the dossier, Part 1) where it is
// physics; ours where the owner's law is stricter:
//   D1 adjacency by position (adjacency.h)       D2 128-triangle / 256-vertex leaves by bisection (leaves.h)
//   D3 groups of 8-32 per level, borders recomputed every level (groups.h)
//   D4 a group simplified to half, re-indexing in stage 1, Nanite-scale attribute weights (simplify.h)
//   D6 the measured one-step price and the ladder: no forced halving, a group that cannot is carried (measure.h,
//      ladder.h)
//   D7 every parent of a group shares the group's error = max(children's stored, the step), and the group's LOD
//      sphere = the sphere enclosing its members' LOD spheres — monotone by construction
//   D8 the chain runs until one cluster remains, then an EMPTY TOP group (no output) whose error is the box diagonal
//      of what remains (the island rule on the whole): a thing under a pixel stops drawing. Every triangle mesh gets
//      a DAG (a mesh of at most 128 triangles: one leaf + the empty top).
// The level of a group = the loop iteration that formed it; children always have lower group ids.

#include "document/assets/mesh.h"   // MeshClusterDag
#include "import/atomdag/adjacency.h"

#include <QVector>
#include <vector>

namespace iris
{
namespace atomdag
{

constexpr int kMaxLevels = 24;   ///< the hard depth cap: a chain still going past it is a defect (logged, no DAG)

struct Options
{
    bool attributes = true;    ///< false = positions only (the cut curve's POSONLY arm)
    bool wantGroups = false;   ///< one GroupRecord per group ATTEMPTED (accepted and carried)
    bool wantLocks = false;    ///< ...with its external positions (atom2.borders_move)
    bool wantRegions = false;  ///< the level-0 provenance partition per cluster (engine.lod_rule_parity)
};

struct GroupRecord
{
    int level = 0;
    int dagGroup = -1;          ///< the DAG group it became; -1 = carried (or the empty top: kind 2)
    int kind = 0;               ///< 0 a reduction, 1 carried, 2 the empty top, 3 not simplified (at its target)
    int members = 0;
    int inTris = 0, outTris = 0;
    float childError = 0.0f;    ///< max over the members of (stored, carried budget)
    float budget = 0.0f, step = 0.0f, stored = 0.0f, estimate = 0.0f, depth0Estimate = -1.0f;
    float removedVertex = 0.0f, lostFacet = 0.0f, addedFacet = 0.0f, droppedIsland = 0.0f, lostLocked = 0.0f;
    int attempts = 0, pruned = 0, islandsDropped = 0, lockedLost = 0;
    float weightNormal = 0.0f, weightUv = 0.0f;
    std::vector<unsigned> external;   ///< wantLocks: its external positions (weld ids)
    std::vector<int> memberClusters;  ///< wantLocks: the DAG cluster ids it merged
};

struct Stats
{
    int clusters = 0, groups = 0, leaves = 0;
    int depth = 0;               ///< the deepest group's level (the empty top included)
    int levels = 0;              ///< loop iterations
    int reductions = 0, carried = 0, notSimplified = 0, retries = 0, prunedPieces = 0;
    int levelsAllCarried = 0;    ///< re-runs of a level where every group was carried (the whole pool's budget doubled in place)
    int sphereGrown = 0;         ///< groups whose enclosing sphere was grown by float rounding to hold a member's
    bool depthCapHit = false;
    float rootError = 0.0f;      ///< the empty top's error
    int rootTriangles = 0;       ///< triangles of the last cluster before the empty top (0 if the chain emptied)
    double msTotal = 0.0, msLeaves = 0.0, msGroups = 0.0, msReduce = 0.0, msSplit = 0.0;
    double msSimplify = 0.0, msMeasure = 0.0;   ///< summed over the groups (thread time, not wall)
    double msExact = 0.0;                       ///< the kept results' exact measurements (summed)
    double msFewGroups = 0.0;                   ///< wall of the levels with at most two groups (one thread each)
    std::vector<GroupRecord> records;
    QVector<QVector<quint32>> clusterRegions;   ///< wantRegions: per DAG cluster, the level-0 triangles it stands for
};

/// Builds `out` for the triangle list `base` (vertex ids into `mesh`). False (and `out` empty) when the input is
/// unusable or the chain passed kMaxLevels.
bool build(const MeshView &mesh, const std::vector<unsigned> &base, MeshClusterDag *out, Stats *stats,
           const Options &options);

/// THE CONFIG RECORD the blob carries (the builder's constants): a blob written under any other is refused.
QVector<float> configRecord();

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_BUILD_H
