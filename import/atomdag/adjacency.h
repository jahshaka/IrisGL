/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_ATOMDAG_ADJACENCY_H
#define IRIS_IMPORT_ATOMDAG_ADJACENCY_H

// ATOM-2 D1 — ADJACENCY BY POSITION (SPECS/ATOM_2_SPEC.md; Nanite's FClusterDAG::AddMesh, dossier 1.1).
//
// Triangles are adjacent when they share an EDGE BY POSITION: every directed edge is keyed by its two endpoints'
// weld ids (a weld id = the first vertex at that exact position, ±0 folded), the REVERSED key is its twin, a
// same-direction duplicate (a flipped face) never links, and an edge shared by more than two triangles keeps every
// link. Coincident unindexed vertices (the importer splits a vertex at every UV or normal seam) are therefore one
// node of the adjacency and stay two vertices in the data — the cause the old path never saw: clusterlod's
// clusterizer and partitioner read shared vertex INDICES, so every seam was a border to both and stayed a group
// border at every level (the preflight's §1).

#include "core/math/vec.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace iris
{
namespace atomdag
{

/// What the builder reads of a mesh: positions and the two attributes the simplifier weighs.
struct MeshView
{
    const float *positions = nullptr;
    int posComps = 3;
    size_t nv = 0;
    const float *normals = nullptr;   ///< null = none
    int nrmComps = 3;
    const float *uvs = nullptr;       ///< null = none
    int uvComps = 2;

    Vec3 position(unsigned v) const
    {
        const float *p = positions + size_t(v) * size_t(posComps);
        return Vec3(p[0], p[1], p[2]);
    }
};

/// Per vertex: the first vertex at its exact position (+0 and -0 are one position).
std::vector<unsigned> weldByPosition(const MeshView &mesh);

/// THE ISLANDS OF A MESH (moved from meshbake.cpp's lodchain, ATOM-LOD-BOUND-1's represented-surface rule): level-0
/// connected components, welded by position. A removed point of an island is charged at most its island's extent
/// (the box diagonal); a wholly dropped island costs its extent. The chain and the DAG builder read the same one.
struct Islands
{
    std::vector<unsigned> compOf;      ///< per vertex; UINT_MAX when level 0 does not use it
    std::vector<float>    extent;      ///< per component: its box diagonal
    std::vector<float>    capOf;       ///< per vertex: extent[compOf[v]] (infinity when unused)
    std::vector<size_t>   triangles;   ///< per component: its level-0 triangle count
    std::vector<unsigned> vertStart;   ///< CSR: component c's vertices are
    std::vector<unsigned> verts;       ///<   verts[vertStart[c] .. vertStart[c + 1])
    size_t count() const { return extent.size(); }
};

Islands findIslands(const float *positions, int posComps, size_t nv, const std::vector<unsigned> &base);
/// ...over weld ids already computed (weldByPosition: +-0 folded).
Islands findIslands(const std::vector<unsigned> &weld, const float *positions, int posComps, size_t nv,
                    const std::vector<unsigned> &base);

/// A GROUP'S OWN VERTEX SPACE: local id = the group's scratch id, with its original position, and the weld id, the
/// island and the island cap of its mesh vertex — what the measurement, the prune and the re-split read.
struct LocalSpace
{
    std::vector<float> pos;         ///< n * 3
    std::vector<unsigned> weld;     ///< n: the vertex's weld id (global)
    std::vector<unsigned> comp;     ///< n: the vertex's island (UINT_MAX: none)
    std::vector<float> cap;         ///< n: the vertex's island cap
    size_t islandCount = 0;
    MeshView view() const
    {
        MeshView v;
        v.positions = pos.data();
        v.posComps = 3;
        v.nv = pos.size() / 3;
        return v;
    }
};

inline std::uint64_t edgeKey(unsigned wa, unsigned wb) { return (std::uint64_t(wa) << 32) | std::uint64_t(wb); }

/// THE DIRECTED EDGES of a triangle list (vertex ids into the mesh) by weld id, sorted by key: the twin of edge
/// (t, k) — corners k -> k+1 — is every entry under the reversed key. Degenerate edges (one position) are left out.
struct EdgeTable
{
    struct Edge
    {
        std::uint64_t key = 0;
        unsigned tri = 0;
        unsigned corner = 0;
        bool operator<(const Edge &o) const
        {
            return key != o.key ? key < o.key : (tri != o.tri ? tri < o.tri : corner < o.corner);
        }
    };
    std::vector<Edge> edges;

    void build(const unsigned *tris, size_t triCount, const std::vector<unsigned> &weld);

    /// Every triangle holding the twin of the directed edge (wa -> wb), i.e. an edge (wb -> wa); f(tri, corner).
    template <class F> void forTwins(unsigned wa, unsigned wb, F &&f) const
    {
        const std::uint64_t k = edgeKey(wb, wa);
        size_t lo = 0, hi = edges.size();
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            if (edges[mid].key < k) lo = mid + 1; else hi = mid;
        }
        for (size_t i = lo; i < edges.size() && edges[i].key == k; ++i) f(edges[i].tri, edges[i].corner);
    }
};

/// A weighted undirected graph in CSR form (the partitioner's input): node i's neighbours are
/// adj[start[i] .. start[i + 1]), weight w[...] each, no self loop, each neighbour once (parallel edges summed).
struct Graph
{
    std::vector<unsigned> start;
    std::vector<unsigned> adj;
    std::vector<int> w;
    std::vector<Vec3> centre;   ///< per node: a point (the triangle's centroid, the cluster's box centre)
    size_t nodes() const { return centre.size(); }
};

/// Builds a Graph from an edge list (a, b, weight) over `nodes` nodes: symmetric, duplicates summed, sorted.
struct EdgeList
{
    struct E { unsigned a = 0, b = 0; int w = 0; };
    std::vector<E> list;
    void add(unsigned a, unsigned b, int w) { if (a != b) list.push_back({ a, b, w }); }
};
Graph makeGraph(size_t nodes, const EdgeList &edges, std::vector<Vec3> centres);

/// THE LOCALITY LINKS (Nanite's FGraphPartitioner::BuildLocalityLinks, dossier 1.1): the nodes sorted along a
/// Morton curve of their centres (meshopt_spatialSortRemap); a node whose run of same-island neighbours in that
/// order is shorter than 128 links to at most 5 of the nearest nodes of a DIFFERENT island within +-16 places,
/// at `weight` — what pulls disjoint pieces (cards, props, kit-bashed shells) into one partition by proximity.
void addLocalityLinks(const std::vector<Vec3> &centres, const std::vector<unsigned> &islandOf, int weight,
                      EdgeList &edges);

}   // namespace atomdag
}   // namespace iris

#endif   // IRIS_IMPORT_ATOMDAG_ADJACENCY_H
