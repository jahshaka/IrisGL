/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_GROUPS_H
#define IRIS_IMPORT_ATOMDAG_GROUPS_H

// ATOM-2 D3 — GROUPS PER LEVEL (Nanite's GroupTriangleClusters / FindAdjacentClusters, dossier 1.2).
//
// The level's clusters (the POOL: every cluster produced and not yet merged, carried ones included) are grouped 8-32
// by the same bisection as the leaves, over the CLUSTER graph: two clusters sharing boundary edges (by position, the
// twin rule of D1) are linked at shared x (16, or 12 when both came from the same group) + 4, plus locality links of
// weight 1 between the box centres of clusters of different connected pieces; a level of at most 32 clusters is one
// group. THE BORDERS ARE RECOMPUTED HERE AT EVERY LEVEL, never inherited: a group's EXTERNAL positions are the
// endpoints of its edges whose twin lies in a pool cluster OUTSIDE the group — an edge between two clusters of one
// group is interior, an edge with no twin anywhere (an open mesh border) is never external. So a border moves level
// to level by construction: no terminal group, no stuck state exists.

#include "import/atomdag/adjacency.h"

#include <vector>

namespace iris
{
namespace atomdag
{

constexpr size_t kGroupMin = 8;
constexpr size_t kGroupMax = 32;
constexpr size_t kGroupTarget = 20;   ///< (8 + 32) / 2
constexpr int kGroupEdgeWeight = 16;
constexpr int kGroupSiblingWeight = 12;
constexpr int kGroupEdgeBase = 4;

/// One pool cluster as the grouping sees it.
struct PoolView
{
    const std::vector<unsigned> *tris = nullptr;   ///< vertex ids, three per triangle
    int refined = -1;                               ///< the group that produced it (-1 = a leaf)
    Vec3 boxCentre;
};

struct Grouping
{
    std::vector<std::vector<unsigned>> groups;        ///< pool indices per group, sorted
    std::vector<std::vector<unsigned>> external;      ///< per group: its external positions (weld ids, sorted)
};

Grouping groupPool(const std::vector<PoolView> &pool, const std::vector<unsigned> &weld);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_GROUPS_H
