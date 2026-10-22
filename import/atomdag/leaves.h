/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_LEAVES_H
#define IRIS_IMPORT_ATOMDAG_LEAVES_H

// ATOM-2 D2 — THE LEAVES, and the one graph partitioner D2 and D3 share (the preflight's correction 1).
//
// meshoptimizer's partitionClusters is a greedy agglomeration over shared vertex ids — no edge weight, no link, no
// minimum, no vertex cap — so the partitioner is OUR OWN recursive bisection (Nanite's PartitionStrict, METIS's
// recursive form, dossier 1.1): a part is split in two until it FITS — at most `maxSize` nodes and, for leaves, at
// most 256 unique vertices (the format's 8-bit cluster-local indices, meshbake.cpp readClusterDag). Each bisection
// is a greedy graph growth from the spatially extreme node (the frontier node of the highest gain first) refined by
// Fiduccia-Mattheyses passes, inside a balance window that keeps both halves feasible. Deterministic: no random
// choice, every tie broken by spatial rank then node id, and the parts of one recursion depth are split on the
// bake's threads into their own slots (the bakepool rules) — the answer never depends on the width.

#include "import/atomdag/adjacency.h"

#include <functional>
#include <vector>

namespace iris
{
namespace atomdag
{

struct PartitionRule
{
    size_t maxSize = 128;      ///< a part holds at most this many nodes
    size_t targetSize = 126;   ///< the size the part count is derived from (Nanite: (min + max) / 2)
    size_t minSize = 1;        ///< the smallest part the balance window may leave
    double imbalance = 0.005;  ///< per final part: the fraction of its size a bisection may be off by
    /// An extra cap a part of at most maxSize nodes must also meet (the leaves' vertex cap); null = none.
    std::function<bool(const std::vector<unsigned> &part)> fits;
    /// The part's own size in the extra cap's currency (its unique vertices) and that cap: a part is cut into at least
    /// ceil(vertices / maxVertices) final parts, so a vertex-bound part is not halved into half-empty clusters
    /// (128 simplified kit-bash triangles carry 288 vertices: two parts of 64 against one of 113 and one of 15).
    std::function<size_t(const std::vector<unsigned> &part)> vertexCount;
    size_t maxVertices = 0;
};

/// Partitions the graph's nodes into parts (each a sorted list of node ids), in a deterministic order.
std::vector<std::vector<unsigned>> partition(const Graph &graph, const PartitionRule &rule);

constexpr size_t kClusterTriangles = 128;   ///< Nanite's FCluster::ClusterSize; the runtime's per-cluster limit
constexpr size_t kClusterTarget = 126;      ///< (124 + 128) / 2
constexpr size_t kClusterVertices = 256;    ///< the format's cap (8-bit local indices)
constexpr int kSharedEdgeWeight = 260;      ///< Nanite's 4 * 65 per shared edge
constexpr int kLocalityWeight = 1;

/// D2: the clusters of a triangle list (`tris`: vertex ids into the mesh, three per triangle). Returns per cluster
/// its triangle indices into `tris`, sorted. `islandOfTri` = the island of each triangle (the locality links go to a
/// different island only).
std::vector<std::vector<unsigned>> buildClusters(const MeshView &mesh, const std::vector<unsigned> &weld,
                                                 const std::vector<unsigned> &tris,
                                                 const std::vector<unsigned> &islandOfTri);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_LEAVES_H
