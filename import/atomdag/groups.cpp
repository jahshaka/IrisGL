/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/groups.h"

#include "import/atomdag/leaves.h"

#include <algorithm>
#include <map>

namespace iris
{
namespace atomdag
{

Grouping groupPool(const std::vector<PoolView> &pool, const std::vector<unsigned> &weld)
{
    Grouping out;
    const size_t n = pool.size();
    if (n == 0) return out;

    // Every pool triangle, with the cluster it belongs to.
    std::vector<unsigned> tris, clusterOf;
    for (size_t c = 0; c < n; ++c) {
        tris.insert(tris.end(), pool[c].tris->begin(), pool[c].tris->end());
        clusterOf.insert(clusterOf.end(), pool[c].tris->size() / 3, unsigned(c));
    }
    EdgeTable table;
    table.build(tris.data(), tris.size() / 3, weld);

    // THE CLUSTER GRAPH: shared boundary edges per pair (each twin pair counted once, from the lower cluster).
    std::map<std::pair<unsigned, unsigned>, int> shared;
    for (const EdgeTable::Edge &e : table.edges) {
        const unsigned ca = clusterOf[e.tri];
        table.forTwins(unsigned(e.key >> 32), unsigned(e.key & 0xffffffffu), [&](unsigned other, unsigned) {
            const unsigned cb = clusterOf[other];
            if (ca < cb) ++shared[{ ca, cb }];
        });
    }

    if (n <= kGroupMax) {
        out.groups.emplace_back(n);
        for (size_t c = 0; c < n; ++c) out.groups[0][c] = unsigned(c);
    } else {
        EdgeList edges;
        std::vector<unsigned> parent(n);
        for (size_t c = 0; c < n; ++c) parent[c] = unsigned(c);
        const auto root = [&](unsigned x) {
            while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
            return x;
        };
        for (const auto &s : shared) {
            const unsigned a = s.first.first, b = s.first.second;
            const bool siblings = pool[a].refined >= 0 && pool[a].refined == pool[b].refined;
            edges.add(a, b, s.second * (siblings ? kGroupSiblingWeight : kGroupEdgeWeight) + kGroupEdgeBase);
            const unsigned ra = root(a), rb = root(b);
            if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
        }
        std::vector<Vec3> centres(n);
        std::vector<unsigned> pieceOf(n);
        for (size_t c = 0; c < n; ++c) { centres[c] = pool[c].boxCentre; pieceOf[c] = root(unsigned(c)); }
        addLocalityLinks(centres, pieceOf, kLocalityWeight, edges);
        const Graph graph = makeGraph(n, edges, std::move(centres));
        PartitionRule rule;
        rule.maxSize = kGroupMax;
        rule.targetSize = kGroupTarget;
        rule.minSize = kGroupMin;
        rule.imbalance = 0.2;   // Nanite's loose UFACTOR for groups (8-32)
        out.groups = partition(graph, rule);
    }

    // THE EXTERNAL POSITIONS of every group (recomputed, never inherited).
    std::vector<unsigned> groupOf(n, 0u);
    for (size_t g = 0; g < out.groups.size(); ++g)
        for (unsigned c : out.groups[g]) groupOf[c] = unsigned(g);
    out.external.assign(out.groups.size(), {});
    for (const EdgeTable::Edge &e : table.edges) {
        const unsigned g = groupOf[clusterOf[e.tri]];
        const unsigned wa = unsigned(e.key >> 32), wb = unsigned(e.key & 0xffffffffu);
        bool external = false;
        table.forTwins(wa, wb, [&](unsigned other, unsigned) {
            if (groupOf[clusterOf[other]] != g) external = true;
        });
        if (external) { out.external[g].push_back(wa); out.external[g].push_back(wb); }
    }
    for (std::vector<unsigned> &x : out.external) {
        std::sort(x.begin(), x.end());
        x.erase(std::unique(x.begin(), x.end()), x.end());
    }
    return out;
}

}   // namespace atomdag
}   // namespace iris
