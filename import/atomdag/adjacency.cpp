/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/adjacency.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

namespace iris
{
namespace atomdag
{

std::vector<unsigned> weldByPosition(const MeshView &mesh)
{
    // ±0 folded (Nanite's HashPosition): the canonical copy is what meshoptimizer's exact position remap reads.
    std::vector<float> canon(mesh.nv * 3);
    for (size_t v = 0; v < mesh.nv; ++v)
        for (int k = 0; k < 3; ++k) {
            const float f = mesh.positions[v * size_t(mesh.posComps) + size_t(k)];
            canon[v * 3 + size_t(k)] = f == 0.0f ? 0.0f : f;
        }
    std::vector<unsigned> weld(mesh.nv);
    meshopt_generatePositionRemap(weld.data(), canon.data(), mesh.nv, sizeof(float) * 3);
    return weld;
}

Islands findIslands(const float *positions, int posComps, size_t nv,
                    const std::vector<unsigned> &base)
{
    Islands is;
    std::vector<unsigned> remap(nv);
    meshopt_generatePositionRemap(remap.data(), positions, nv, sizeof(float) * size_t(posComps));
    std::vector<unsigned> parent(nv);
    for (size_t v = 0; v < nv; ++v) parent[v] = unsigned(v);
    const auto root = [&](unsigned x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    for (size_t t = 0; t < base.size() / 3; ++t) {
        const unsigned a = root(remap[base[t * 3]]);
        for (int k = 1; k < 3; ++k) {
            const unsigned b = root(remap[base[t * 3 + size_t(k)]]);
            if (a != b) parent[b] = a;
        }
    }
    // Numbered in first-use order of the index list: deterministic.
    constexpr unsigned kNone = std::numeric_limits<unsigned>::max();
    std::vector<unsigned> idOfRoot(nv, kNone);
    std::vector<Vec3> lo, hi;
    is.compOf.assign(nv, kNone);
    for (unsigned v : base) {
        const unsigned r = root(remap[v]);
        if (idOfRoot[r] == kNone) {
            idOfRoot[r] = unsigned(lo.size());
            lo.emplace_back(FLT_MAX, FLT_MAX, FLT_MAX);
            hi.emplace_back(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        }
        const unsigned c = idOfRoot[r];
        is.compOf[v] = c;
        const float *p = positions + size_t(v) * size_t(posComps);
        lo[c] = Vec3(std::min(lo[c].x(), p[0]), std::min(lo[c].y(), p[1]), std::min(lo[c].z(), p[2]));
        hi[c] = Vec3(std::max(hi[c].x(), p[0]), std::max(hi[c].y(), p[1]), std::max(hi[c].z(), p[2]));
    }
    const size_t n = lo.size();
    is.extent.resize(n);
    for (size_t c = 0; c < n; ++c) is.extent[c] = (hi[c] - lo[c]).length();
    is.triangles.assign(n, 0);
    for (size_t t = 0; t < base.size() / 3; ++t) ++is.triangles[is.compOf[base[t * 3]]];
    is.capOf.assign(nv, std::numeric_limits<float>::infinity());
    is.vertStart.assign(n + 1, 0);
    for (size_t v = 0; v < nv; ++v)
        if (is.compOf[v] != kNone) { is.capOf[v] = is.extent[is.compOf[v]]; ++is.vertStart[is.compOf[v] + 1]; }
    for (size_t c = 0; c < n; ++c) is.vertStart[c + 1] += is.vertStart[c];
    is.verts.resize(is.vertStart[n]);
    std::vector<unsigned> fill(is.vertStart.begin(), is.vertStart.end() - 1);
    for (size_t v = 0; v < nv; ++v)
        if (is.compOf[v] != kNone) is.verts[fill[is.compOf[v]]++] = unsigned(v);
    return is;
}

void EdgeTable::build(const unsigned *tris, size_t triCount, const std::vector<unsigned> &weld)
{
    edges.clear();
    edges.reserve(triCount * 3);
    for (size_t t = 0; t < triCount; ++t)
        for (unsigned k = 0; k < 3; ++k) {
            const unsigned wa = weld[tris[t * 3 + k]], wb = weld[tris[t * 3 + (k + 1) % 3]];
            if (wa == wb) continue;
            edges.push_back({ edgeKey(wa, wb), unsigned(t), k });
        }
    std::sort(edges.begin(), edges.end());
}

Graph makeGraph(size_t nodes, const EdgeList &edges, std::vector<Vec3> centres)
{
    Graph g;
    g.centre = std::move(centres);
    struct Half { unsigned a = 0, b = 0; int w = 0; };
    std::vector<Half> h;
    h.reserve(edges.list.size() * 2);
    for (const EdgeList::E &e : edges.list) {
        h.push_back({ e.a, e.b, e.w });
        h.push_back({ e.b, e.a, e.w });
    }
    std::sort(h.begin(), h.end(), [](const Half &l, const Half &r) { return l.a != r.a ? l.a < r.a : l.b < r.b; });
    g.start.assign(nodes + 1, 0u);
    g.adj.reserve(h.size());
    g.w.reserve(h.size());
    for (size_t i = 0; i < h.size();) {
        size_t j = i;
        int w = 0;
        while (j < h.size() && h[j].a == h[i].a && h[j].b == h[i].b) w += h[j++].w;
        g.adj.push_back(h[i].b);
        g.w.push_back(w);
        ++g.start[size_t(h[i].a) + 1];
        i = j;
    }
    for (size_t n = 0; n < nodes; ++n) g.start[n + 1] += g.start[n];
    return g;
}

void addLocalityLinks(const std::vector<Vec3> &centres, const std::vector<unsigned> &islandOf, int weight,
                      EdgeList &edges)
{
    const size_t n = centres.size();
    if (n < 2) return;
    std::vector<float> pts(n * 3);
    for (size_t i = 0; i < n; ++i) { pts[i * 3] = centres[i].x(); pts[i * 3 + 1] = centres[i].y(); pts[i * 3 + 2] = centres[i].z(); }
    std::vector<unsigned> remap(n);
    meshopt_spatialSortRemap(remap.data(), pts.data(), n, sizeof(float) * 3);
    std::vector<unsigned> order(n);
    for (size_t i = 0; i < n; ++i) order[remap[i]] = unsigned(i);
    // Runs of one island along the curve: only a node in a SHORT run (< 128) looks for links (Nanite's rule: a
    // large connected surface needs none, its adjacency holds it together).
    std::vector<unsigned> runLength(n, 0u);
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && islandOf[order[j]] == islandOf[order[i]]) ++j;
        for (size_t k = i; k < j; ++k) runLength[k] = unsigned(j - i);
        i = j;
    }
    constexpr int kSearch = 16;
    constexpr size_t kMaxLinks = 5;
    std::vector<std::pair<float, unsigned>> near;
    for (size_t i = 0; i < n; ++i) {
        if (runLength[i] >= 128u) continue;
        const unsigned a = order[i];
        near.clear();
        const size_t lo = i >= size_t(kSearch) ? i - size_t(kSearch) : 0, hi = std::min(n, i + size_t(kSearch) + 1);
        for (size_t j = lo; j < hi; ++j) {
            const unsigned b = order[j];
            if (j == i || islandOf[b] == islandOf[a]) continue;
            near.push_back({ (centres[b] - centres[a]).lengthSquared(), b });
        }
        std::sort(near.begin(), near.end());
        for (size_t k = 0; k < near.size() && k < kMaxLinks; ++k) edges.add(a, near[k].second, weight);
    }
}

}   // namespace atomdag
}   // namespace iris
