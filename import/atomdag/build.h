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
//   D4 a group simplified to half, re-indexing, every attribute at a tiny weight (simplify.h)
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
#include "import/atomdag/cards.h"

#include <QVector>
#include <vector>

namespace iris
{
namespace atomdag
{

constexpr int kMaxLevels = 24;   ///< the depth cap: a chain still going closes with the empty top over its pool (logged)

struct Options
{
    int maxLevels = kMaxLevels;   ///< the depth cap (a test lowers it to reach the cap on a fixture)
    bool wantGroups = false;   ///< one GroupRecord per group ATTEMPTED (accepted and carried)
    bool wantLocks = false;    ///< ...with its external positions (atom2.borders_move)
    bool wantRegions = false;  ///< the level-0 provenance partition per cluster (engine.lod_rule_parity)
};

struct GroupRecord
{
    int level = 0;
    int pass = 0;               ///< the level's pass: > 0 = a re-run of an all-carried level (its budgets doubled)
    int dagGroup = -1;          ///< the DAG group it became; -1 = carried (or the empty top: kind 2)
    int kind = 0;               ///< 0 a reduction, 1 carried, 2 the empty top, 3 not simplified (at its target)
    bool countCarry = false;    ///< carried after a result within its budget (the cluster count, not the budget)
    bool cards = false;         ///< a card group (D9): merged, not collapsed
    int cardSets = 0, cardsMerged = 0;           ///< its quads, and the cards they stand for
    float cardSourceArea = 0.0f, cardQuadArea = 0.0f;   ///< the merged cards' summed area, the quads' (coverage)
    float cardReach = 0.0f;     ///< the largest seed-to-card centre distance it merged
    float cardPrice = 0.0f;     ///< the largest measured price of its merged sets (its step is at least this)
    int members = 0;
    int inTris = 0, outTris = 0;
    float childStored = 0.0f;   ///< max over the members of their stored error
    float ask = 0.0f;           ///< max over the members of their asks (ladder.h); 0 = leaves never granted a budget
    float budget = 0.0f;        ///< THE GRANTED BUDGET E the group was simplified under
    float step = 0.0f;          ///< the measured step (the kept result's exact maximum, bounded by E)
    float measured = 0.0f;      ///< the exact walk's own value (its upper bound: within 5 % above the true maximum)
    float stored = 0.0f, estimate = 0.0f, depth0Estimate = -1.0f;
    float removedVertex = 0.0f, lostFacet = 0.0f, addedFacet = 0.0f, droppedIsland = 0.0f, lostLocked = 0.0f;
    int attempts = 0, pruned = 0, islandsDropped = 0, lockedLost = 0;   ///< attempts: every simplify of every shrink
    std::vector<int> memberClusters;  ///< the DAG cluster ids it was given (the law's children)
    std::vector<unsigned> external;   ///< wantLocks: its external positions (weld ids)
};

struct Stats
{
    int clusters = 0, groups = 0, leaves = 0;
    int depth = 0;               ///< the deepest group's level (the empty top included)
    int levels = 0;              ///< loop iterations
    int reductions = 0, carried = 0, notSimplified = 0, retries = 0, prunedPieces = 0;
    int cardSets = 0, cardsMerged = 0;   ///< D9: the quads and the cards they stand for
    size_t appendedVertices = 0;  ///< the quads' corners appended to the mesh
    int levelsAllCarried = 0;    ///< re-runs of a level where every group was carried (the whole pool's budget doubled in place)
    int sphereGrown = 0;         ///< groups whose enclosing sphere was grown by float rounding to hold a member's
    bool depthCapHit = false;    ///< the chain met the depth cap and closed with the empty top over its pool
    float rootError = 0.0f;      ///< the empty top's error
    float floorLen = 0.0f;       ///< the arithmetic floor of a measured length (1e-5 of the largest axis)
    int rootTriangles = 0;       ///< triangles of the last cluster before the empty top (0 if the chain emptied)
    double msTotal = 0.0, msLeaves = 0.0, msGroups = 0.0, msReduce = 0.0, msSplit = 0.0;
    double msSimplify = 0.0, msMeasure = 0.0;   ///< summed over the groups (thread time, not wall)
    double msExact = 0.0;                       ///< the kept results' exact measurements (summed)
    double msFewGroups = 0.0;                   ///< wall of the levels with at most two groups (one thread each)
    std::vector<GroupRecord> records;
    QVector<QVector<quint32>> clusterRegions;   ///< wantRegions: per DAG cluster, the level-0 triangles it stands for
};

/// THE VERTICES THE BUILD APPENDED (D9's card quads): ids nv0.. of the DAG's indices, four per quad; per vertex its
/// SOURCE (an original mesh vertex: every buffer but the position, the normal and the first UV set copies it) and its
/// corner (position, normal, the first UV set's u and v).
struct Appended
{
    size_t nv0 = 0;
    std::vector<unsigned> source;
    std::vector<CardCorner> corners;
    size_t count() const { return source.size(); }
};

/// Builds `out` for the triangle list `base` (vertex ids into `mesh`). False (and `out` empty) when the input is
/// unusable; a chain at the depth cap closes with the empty top over its pool (stats->depthCapHit). The DAG's
/// indices may name APPENDED vertices (ids >= mesh.nv, the card quads): the caller grows the mesh's buffers from
/// `appended` before the DAG is read.
bool build(const MeshView &mesh, const std::vector<unsigned> &base, MeshClusterDag *out, Stats *stats,
           const Options &options, Appended *appended);

/// THE CONFIG RECORD the blob carries (the builder's constants): a blob written under any other is refused.
QVector<float> configRecord();

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_BUILD_H
