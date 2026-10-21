/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/leaves.h"

#include "import/bakeshared.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <queue>
#include <tuple>

namespace iris
{
namespace atomdag
{
namespace
{

/// How many final parts `n` nodes make: at least two, at least n / maxSize, about n / targetSize.
size_t partsFor(size_t n, const PartitionRule &rule)
{
    const size_t byMax = (n + rule.maxSize - 1) / rule.maxSize;
    const size_t byTarget = size_t(std::llround(double(n) / double(rule.targetSize)));
    return std::max<size_t>(2, std::max(byMax, byTarget));
}

bool fits(const std::vector<unsigned> &part, const PartitionRule &rule)
{
    if (part.size() > rule.maxSize) return false;
    return !rule.fits || rule.fits(part);
}

/// One thread's scratch: global node -> local index (-1 outside the part being split).
struct Scratch
{
    std::vector<int> local;
};

/// THE BISECTION of one part: returns the two halves (sorted node ids).
void bisect(const Graph &g, const std::vector<unsigned> &nodes, const PartitionRule &rule, Scratch &sc,
            std::vector<unsigned> &outA, std::vector<unsigned> &outB)
{
    const size_t n = nodes.size();
    if (sc.local.size() < g.nodes()) sc.local.assign(g.nodes(), -1);
    for (size_t i = 0; i < n; ++i) sc.local[nodes[i]] = int(i);

    // The part's own graph (edges leaving it ignored).
    std::vector<unsigned> ls(n + 1, 0u), ladj;
    std::vector<int> lw;
    std::vector<std::int64_t> tot(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const unsigned u = nodes[i];
        for (unsigned e = g.start[u]; e < g.start[size_t(u) + 1]; ++e) {
            const int l = sc.local[g.adj[e]];
            if (l < 0) continue;
            ladj.push_back(unsigned(l));
            lw.push_back(g.w[e]);
            tot[i] += g.w[e];
        }
        ls[i + 1] = unsigned(ladj.size());
    }
    for (size_t i = 0; i < n; ++i) sc.local[nodes[i]] = -1;

    // Spatial rank along the part's longest axis (the seed and every tie, and the fallback when the growth's
    // frontier is empty — a part of several disconnected pieces with no link between them).
    Vec3 lo = g.centre[nodes[0]], hi = lo;
    for (unsigned u : nodes) {
        const Vec3 &c = g.centre[u];
        lo = Vec3(std::min(lo.x(), c.x()), std::min(lo.y(), c.y()), std::min(lo.z(), c.z()));
        hi = Vec3(std::max(hi.x(), c.x()), std::max(hi.y(), c.y()), std::max(hi.z(), c.z()));
    }
    const Vec3 size = hi - lo;
    const int axis = size.x() >= size.y() && size.x() >= size.z() ? 0 : (size.y() >= size.z() ? 1 : 2);
    const auto proj = [&](unsigned u) {
        const Vec3 &c = g.centre[u];
        return axis == 0 ? c.x() : (axis == 1 ? c.y() : c.z());
    };
    std::vector<unsigned> order(n);
    for (size_t i = 0; i < n; ++i) order[i] = unsigned(i);
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        const float pa = proj(nodes[a]), pb = proj(nodes[b]);
        return pa != pb ? pa < pb : nodes[a] < nodes[b];
    });
    std::vector<unsigned> rank(n);
    for (size_t r = 0; r < n; ++r) rank[order[r]] = unsigned(r);

    // THE BALANCE WINDOW: half A holds KL of the K final parts.
    const size_t K = partsFor(n, rule), KL = K / 2, KR = K - KL;
    const size_t targetA = size_t(std::llround(double(n) * double(KL) / double(K)));
    const double perPart = double(n) / double(K);
    const size_t tol = std::max<size_t>(1, size_t(rule.imbalance * perPart * double(std::min(KL, KR))));
    size_t loA = targetA > tol ? targetA - tol : 1, hiA = std::min(n - 1, targetA + tol);
    // ...clipped so both halves stay feasible (each final part within [minSize, maxSize]).
    loA = std::max(loA, std::max(KL * rule.minSize, n > KR * rule.maxSize ? n - KR * rule.maxSize : size_t(1)));
    hiA = std::min(hiA, std::min(KL * rule.maxSize, n > KR * rule.minSize ? n - KR * rule.minSize : n - 1));
    if (loA > hiA) { loA = hiA = std::clamp<size_t>(targetA, 1, n - 1); }
    const size_t growTo = std::clamp(targetA, loA, hiA);

    // GREEDY GROWTH: the frontier node with the highest gain (2 x its weight into A - its total) joins A.
    std::vector<unsigned char> side(n, 1u);   // 1 = B
    std::vector<std::int64_t> connA(n, 0);
    using Entry = std::tuple<std::int64_t, std::int64_t, unsigned>;   // gain, -rank, node
    std::priority_queue<Entry> heap;
    size_t sizeA = 0, cursor = 0;
    const auto addToA = [&](unsigned u) {
        side[u] = 0u;
        ++sizeA;
        for (unsigned e = ls[u]; e < ls[u + 1]; ++e) {
            const unsigned v = ladj[e];
            if (!side[v]) continue;
            connA[v] += lw[e];
            heap.emplace(2 * connA[v] - tot[v], -std::int64_t(rank[v]), v);
        }
    };
    addToA(order[0]);
    while (sizeA < growTo) {
        unsigned next = unsigned(n);
        while (!heap.empty()) {
            const Entry top = heap.top();
            heap.pop();
            const unsigned v = std::get<2>(top);
            if (side[v] && std::get<0>(top) == 2 * connA[v] - tot[v]) { next = v; break; }
        }
        if (next == unsigned(n)) {   // the frontier is empty: the spatially next node not in A
            while (cursor < n && !side[order[cursor]]) ++cursor;
            next = order[cursor];
        }
        addToA(next);
    }

    // FIDUCCIA-MATTHEYSES: move the node of the best gain whose move keeps A in the window, lock it, keep the best
    // prefix of the pass; stop on a pass that improves nothing.
    std::vector<std::int64_t> gain(n, 0);
    std::vector<unsigned char> locked(n, 0u);
    std::vector<unsigned> moves;
    constexpr int kPasses = 4;
    constexpr int kPatience = 64;
    for (int pass = 0; pass < kPasses; ++pass) {
        std::priority_queue<Entry> heaps[2];
        for (size_t v = 0; v < n; ++v) {
            std::int64_t ext = 0;
            for (unsigned e = ls[v]; e < ls[v + 1]; ++e) if (side[ladj[e]] != side[v]) ext += lw[e];
            gain[v] = 2 * ext - tot[v];
            locked[v] = 0u;
            if (ext > 0) heaps[side[v]].emplace(gain[v], -std::int64_t(rank[v]), unsigned(v));
        }
        moves.clear();
        std::int64_t cum = 0, best = 0;
        size_t bestLen = 0;
        int sinceBest = 0;
        for (;;) {
            const auto clean = [&](std::priority_queue<Entry> &h) {
                while (!h.empty()) {
                    const unsigned v = std::get<2>(h.top());
                    if (!locked[v] && std::get<0>(h.top()) == gain[v] && side[v] == (&h == &heaps[0] ? 0u : 1u)) return true;
                    h.pop();
                }
                return false;
            };
            const bool canA = sizeA > loA && clean(heaps[0]);   // A -> B
            const bool canB = sizeA < hiA && clean(heaps[1]);   // B -> A
            if (!canA && !canB) break;
            int from;
            if (canA && canB) from = heaps[0].top() > heaps[1].top() ? 0 : 1;
            else from = canA ? 0 : 1;
            const unsigned v = std::get<2>(heaps[from].top());
            heaps[from].pop();
            cum += gain[v];
            locked[v] = 1u;
            side[v] = from ? 0u : 1u;
            sizeA = from ? sizeA + 1 : sizeA - 1;
            moves.push_back(v);
            for (unsigned e = ls[v]; e < ls[v + 1]; ++e) {
                const unsigned u = ladj[e];
                if (locked[u]) continue;
                // v left u's side (u's external weight grows) or joined it (shrinks)
                gain[u] += side[u] == side[v] ? -2 * std::int64_t(lw[e]) : 2 * std::int64_t(lw[e]);
                heaps[side[u]].emplace(gain[u], -std::int64_t(rank[u]), u);
            }
            if (cum > best) { best = cum; bestLen = moves.size(); sinceBest = 0; }
            else if (++sinceBest > kPatience) break;
        }
        for (size_t i = moves.size(); i > bestLen; --i) {   // roll back past the best prefix
            const unsigned v = moves[i - 1];
            side[v] = side[v] ? 0u : 1u;
            sizeA = side[v] ? sizeA - 1 : sizeA + 1;
        }
        if (best <= 0) break;
    }

    outA.clear();
    outB.clear();
    for (size_t i = 0; i < n; ++i) (side[i] ? outB : outA).push_back(nodes[i]);
}

}   // namespace

std::vector<std::vector<unsigned>> partition(const Graph &graph, const PartitionRule &rule)
{
    std::vector<std::vector<unsigned>> parts;
    if (graph.nodes() == 0) return parts;
    parts.emplace_back(graph.nodes());
    for (size_t i = 0; i < graph.nodes(); ++i) parts[0][i] = unsigned(i);
    const int width = bakepool::width();
    std::vector<Scratch> scratch(static_cast<size_t>(width));
    for (;;) {
        std::vector<size_t> need;
        for (size_t p = 0; p < parts.size(); ++p)
            if (parts[p].size() > 1 && !fits(parts[p], rule)) need.push_back(p);
        if (need.empty()) break;
        std::vector<std::vector<unsigned>> halfA(need.size()), halfB(need.size());
        bakepool::forRange(bakepool::Range(need.size(), 1), width, [&](size_t, size_t begin, size_t end, int slot) {
            for (size_t i = begin; i < end; ++i)
                bisect(graph, parts[need[i]], rule, scratch[size_t(slot)], halfA[i], halfB[i]);
        });
        std::vector<std::vector<unsigned>> next;
        next.reserve(parts.size() + need.size());
        size_t k = 0;
        for (size_t p = 0; p < parts.size(); ++p) {
            if (k < need.size() && need[k] == p) {
                if (!halfA[k].empty()) next.push_back(std::move(halfA[k]));
                if (!halfB[k].empty()) next.push_back(std::move(halfB[k]));
                ++k;
            } else {
                next.push_back(std::move(parts[p]));
            }
        }
        parts.swap(next);
    }
    return parts;
}

std::vector<std::vector<unsigned>> buildClusters(const MeshView &mesh, const std::vector<unsigned> &weld,
                                                 const std::vector<unsigned> &tris,
                                                 const std::vector<unsigned> &islandOfTri)
{
    const size_t triCount = tris.size() / 3;
    std::vector<std::vector<unsigned>> out;
    if (triCount == 0) return out;
    std::vector<Vec3> centres(triCount);
    for (size_t t = 0; t < triCount; ++t)
        centres[t] = (mesh.position(tris[t * 3]) + mesh.position(tris[t * 3 + 1]) + mesh.position(tris[t * 3 + 2])) *
                     (1.0f / 3.0f);
    EdgeList edges;
    {
        EdgeTable table;
        table.build(tris.data(), triCount, weld);
        for (const EdgeTable::Edge &e : table.edges) {
            const unsigned wa = unsigned(e.key >> 32), wb = unsigned(e.key & 0xffffffffu);
            table.forTwins(wa, wb, [&](unsigned other, unsigned) {
                if (e.tri < other) edges.add(e.tri, other, kSharedEdgeWeight);
            });
        }
    }
    addLocalityLinks(centres, islandOfTri, kLocalityWeight, edges);
    const Graph graph = makeGraph(triCount, edges, std::move(centres));
    PartitionRule rule;
    rule.maxSize = kClusterTriangles;
    rule.targetSize = kClusterTarget;
    rule.minSize = 1;
    rule.imbalance = 0.005;
    rule.fits = [&](const std::vector<unsigned> &part) {
        std::vector<unsigned> v;
        v.reserve(part.size() * 3);
        for (unsigned t : part) v.insert(v.end(), { tris[size_t(t) * 3], tris[size_t(t) * 3 + 1], tris[size_t(t) * 3 + 2] });
        std::sort(v.begin(), v.end());
        return size_t(std::unique(v.begin(), v.end()) - v.begin()) <= kClusterVertices;
    };
    return partition(graph, rule);
}

}   // namespace atomdag
}   // namespace iris
