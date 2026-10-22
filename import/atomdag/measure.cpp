/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/measure.h"

#include "import/bakeshared.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <limits>

namespace iris
{
namespace atomdag
{
namespace
{

// ---- THE EXACT PRIMITIVES (kept from meshbake.cpp's old DAG measurement, CLUSTER-LOCK-3 / ATOM-UPGRADE) ----------

Vec3 vertexOf(const float *positions, int posComps, unsigned v)
{
    const float *p = positions + size_t(v) * size_t(posComps);
    return Vec3(p[0], p[1], p[2]);
}

/// CELLS ALONG THE LONGEST AXIS for a grid over a GROUP-sized soup — sized by the
/// soup's own triangles, not by `lodchain::boundGridRes`'s cube root. The grid's
/// cells are cubic (the longest axis over this number), so a cube-root count on a
/// long thin box — a slice of a 40 m bar — gives cells as wide as the bar and every
/// query walks a whole ring of it. The cell here is the soup's mean triangle size
/// (sqrt of area / count: about one triangle per occupied cell, the shell walk's
/// O(1)), grown until the whole box holds at most 2^18 cells (the most a per-group
/// scratch grid may cost). The query stays EXACT at any resolution; only its cost
/// moves.
int soupGridRes(const float *positions, int posComps, const std::vector<unsigned> &idx,
                const Vec3 &lo, const Vec3 &hi)
{
    const size_t tris = idx.size() / 3;
    const Vec3 size = hi - lo;
    const float longest = std::max(std::max(size.x(), size.y()), size.z());
    if (tris == 0 || !(longest > 0.0f)) return 1;
    double area = 0.0;
    for (size_t t = 0; t < tris; ++t) {
        const Vec3 a = vertexOf(positions, posComps, idx[t * 3]);
        const Vec3 b = vertexOf(positions, posComps, idx[t * 3 + 1]);
        const Vec3 c = vertexOf(positions, posComps, idx[t * 3 + 2]);
        area += double(Vec3::crossProduct(b - a, c - a).length()) * 0.5;
    }
    double cell = std::sqrt(std::max(area, 1e-30) / double(tris));
    const auto cellsFor = [&](double c) {
        const double nx = std::floor(double(size.x()) / c) + 1.0, ny = std::floor(double(size.y()) / c) + 1.0,
                     nz = std::floor(double(size.z()) / c) + 1.0;
        return nx * ny * nz;
    };
    cell = std::max(cell, double(longest) / 4096.0);
    while (cellsFor(cell) > double(1u << 18)) cell *= 1.25;
    return std::clamp(int(std::ceil(double(longest) / cell)), 1, 4096);
}

/// THE DENSE REFERENCE of one group (ClusterDagStats::denseReference — the DAG's
/// half of atom.lod_bound_bar's (a)): the same represented-surface rule as the bake,

/// A FACET LYING ON THE SURFACE (CLUSTER-LOCK-3): when the S triangles near a lost
/// facet Q that are parallel to it and within `tol` of its plane cover all of Q (their
/// intersections with Q, projected into Q's plane, sum to Q's area), every point of Q
/// stands over one of them, and its distance is at most that triangle's plane offset
/// at Q's corners (linear over Q) over the planes' cosine — the facet's exact maximum
/// with no refinement. Returns it, or -1 when the cover is not proven (not parallel, not
/// covered, or triangles overlapping). facetMax's bounds cannot close on a flat stretch
/// finer than S (every piece across an S edge needs refining to the tolerance: the
/// 2178-triangle ground plane's DAG took 2.5 s against 22 ms).
float coplanarCover(const float *positions, int posComps, const surface::TriangleGrid &grid,
                    const std::vector<unsigned> &idx, const Vec3 &a, const Vec3 &b, const Vec3 &c, float tol,
                    std::vector<unsigned> &scratch)
{
    Vec3 n = Vec3::crossProduct(b - a, c - a);
    const float area2 = n.length();
    if (!(area2 > 0.0f)) return -1.0f;
    n = n * (1.0f / area2);
    const Vec3 u = (b - a).normalized(), v = Vec3::crossProduct(n, u);
    const auto to2 = [&](const Vec3 &p) { const Vec3 d = p - a; return std::array<float, 2> { Vec3::dotProduct(d, u), Vec3::dotProduct(d, v) }; };
    Vec3 lo(std::min({ a.x(), b.x(), c.x() }), std::min({ a.y(), b.y(), c.y() }), std::min({ a.z(), b.z(), c.z() }));
    Vec3 hi(std::max({ a.x(), b.x(), c.x() }), std::max({ a.y(), b.y(), c.y() }), std::max({ a.z(), b.z(), c.z() }));
    const Vec3 grow(tol, tol, tol);
    grid.collect(lo - grow, hi + grow, scratch);
    const std::array<float, 2> q0 = to2(a), q1 = to2(b), q2 = to2(c);
    const float areaQ = 0.5f * area2;
    double covered = 0.0;
    float worst = 0.0f;
    std::vector<std::array<float, 2>> poly, next;
    for (unsigned t : scratch) {
        const Vec3 t0 = vertexOf(positions, posComps, idx[size_t(t) * 3]), t1 = vertexOf(positions, posComps, idx[size_t(t) * 3 + 1]),
                   t2 = vertexOf(positions, posComps, idx[size_t(t) * 3 + 2]);
        Vec3 m = Vec3::crossProduct(t1 - t0, t2 - t0);
        const float ml = m.length();
        if (!(ml > 0.0f)) continue;
        m = m * (1.0f / ml);
        const float cosine = std::fabs(Vec3::dotProduct(m, n));
        if (cosine < 0.9999f) continue;
        float off = 0.0f;
        for (const Vec3 *p : { &a, &b, &c }) off = std::max(off, std::fabs(Vec3::dotProduct(m, *p - t0)));
        if (off > tol) continue;
        // Q clipped by T's three edges, in Q's plane (T's corners projected along n).
        std::array<std::array<float, 2>, 3> T = { to2(t0), to2(t1), to2(t2) };
        const float orient = (T[1][0] - T[0][0]) * (T[2][1] - T[0][1]) - (T[1][1] - T[0][1]) * (T[2][0] - T[0][0]);
        if (orient == 0.0f) continue;
        poly = { q0, q1, q2 };
        for (int e = 0; e < 3 && !poly.empty(); ++e) {
            const std::array<float, 2> &P = T[size_t(e)], &R = T[size_t((e + 1) % 3)];
            const auto side = [&](const std::array<float, 2> &x) {
                return ((R[0] - P[0]) * (x[1] - P[1]) - (R[1] - P[1]) * (x[0] - P[0])) * (orient > 0.0f ? 1.0f : -1.0f);
            };
            next.clear();
            for (size_t i = 0; i < poly.size(); ++i) {
                const std::array<float, 2> &A = poly[i], &B = poly[(i + 1) % poly.size()];
                const float sa = side(A), sb = side(B);
                if (sa >= 0.0f) next.push_back(A);
                if ((sa >= 0.0f) != (sb >= 0.0f)) {
                    const float k = sa / (sa - sb);
                    next.push_back({ A[0] + k * (B[0] - A[0]), A[1] + k * (B[1] - A[1]) });
                }
            }
            poly.swap(next);
        }
        double ar = 0.0;
        for (size_t i = 0; i + 2 < poly.size() + 0 && poly.size() >= 3; ++i) {
            const std::array<float, 2> &A = poly[0], &B = poly[i + 1], &C = poly[i + 2];
            ar += 0.5 * double((B[0] - A[0]) * (C[1] - A[1]) - (B[1] - A[1]) * (C[0] - A[0]));
        }
        if (ar <= 0.0) continue;
        covered += ar;
        worst = std::max(worst, off / cosine);
    }
    if (covered < double(areaQ) * (1.0 - 1e-4) || covered > double(areaQ) * (1.0 + 1e-3)) return -1.0f;
    return worst;
}

/// THE MAXIMUM OF A DISTANCE OVER ONE FACET, EXACT TO A TOLERANCE (CLUSTER-LOCK-3). The
/// distance to a union of triangles is at most the distance to ANY one of them, and the
/// distance to one triangle is convex — so over a piece of the facet it is at most the
/// largest of the piece's CORNERS' distances to the triangle nearest the piece's centre
/// (and, the distance being 1-Lipschitz, at most the centre's distance + the piece's radius).
/// A branch and bound over a longest-edge bisection prunes every piece whose bound cannot pass
/// the running maximum, refines the rest until the piece's bound is within 5 % of the walk's
/// own lower bound (max(`stop`, the largest distance found), at least `floorTol`) and returns an
/// UPPER bound, TIGHT to 1.05x — no point of the facet reads more. Seven fixed points a facet read 0.139
/// where the dense reference found 0.207 inside one sliver of the endless plane's depth-4
/// group once the DAG's lock became per group (atom.dag_bound_bar's (a)). `dist(p, tri, st)`
/// answers the nearest distance at p and the corners of the triangle it lies on — or, with
/// st >= 0, any value <= st as soon as something is within st (the triangle then unset).
/// Returns a value <= `stop` when the facet's maximum is at most `stop`; NaN when more
/// than `budget` pieces would be walked. (The lock decides on this UPPER bound too, not on
/// the largest distance found: the measurement stores the upper bound, so a lock that
/// read the lower one lets a group store up to the tolerance past its bound — measured,
/// CLUSTER-LOCK-3's audit round: the hemisphere's depth-2 group stored 2.004x its error,
/// and no retry went away on the temple: 271 -> 277.)
constexpr size_t kFacetGrain = 64;   ///< items per chunk of the measurement's parallel loops
constexpr int kFacetMaxLevels = 20;   ///< bisection levels (2^20 pieces of area: the old 1-to-4 split's 10)
constexpr size_t kFacetQuickPieces = 64;   ///< the measurement's first try before the flat-facet cover
/// The nearest distance from p to a grid's soup, and the corners of the triangle it lies on.
/// (`idx` is the soup the grid was built over, on `positions`.) With `within` >= 0 it
/// answers any distance <= within as soon as one is found (and no triangle); past it the
/// walk is the exact nearest and names its triangle.
float nearestOn(const float *positions, int posComps, const surface::TriangleGrid &grid,
                const std::vector<unsigned> &idx, surface::TriangleGrid::Query &q, const Vec3 &p, Vec3 *tri,
                float within)
{
    unsigned t = 0u;
    const float d = grid.closest(q, p, nullptr, nullptr, &t, within);
    if (within >= 0.0f && d <= within) return d;
    if (std::isfinite(d) && size_t(t) * 3 + 2 < idx.size())
        for (int k = 0; k < 3; ++k) tri[k] = vertexOf(positions, posComps, idx[size_t(t) * 3 + size_t(k)]);
    return d;
}
template <class Dist>
float facetMax(const Vec3 &a, const Vec3 &b, const Vec3 &c, float stop, float floorTol, Dist &&dist,
               size_t budget = SIZE_MAX, bool decideOnly = false)
{
    struct Piece { Vec3 a, b, c; int level = 0; };
    std::array<Piece, 3 * kFacetMaxLevels + 4> stack;
    size_t top = 0;
    stack[top++] = Piece { a, b, c, 0 };
    float best = 0.0f, upper = 0.0f;
    Vec3 tri[3];
    while (top) {
        if (budget-- == 0) return std::numeric_limits<float>::quiet_NaN();   // the caller's budget ran out
        const Piece piece = stack[--top];
        const Vec3 centre = (piece.a + piece.b + piece.c) * (1.0f / 3.0f);
        float radius = 0.0f;
        for (const Vec3 *corner : { &piece.a, &piece.b, &piece.c })
            radius = std::max(radius, (*corner - centre).length());
        // Anything within bound - radius of the centre holds the whole piece within the
        // bound (1-Lipschitz): the query stops at the first such triangle.
        // (Nothing below the precision floor can move the answer: every stored error is
        // at least the floor.)
        const float within = std::max(std::max(std::max(stop, best), upper), floorTol) - radius;
        const float d = dist(centre, tri, within >= 0.0f ? within : -1.0f);
        if (!std::isfinite(d) || (within >= 0.0f && d <= within)) continue;
        // Two upper bounds, the tighter wins: the corners against the centre's triangle
        // (convex), and the centre's distance + the piece's radius (1-Lipschitz — the one
        // left when that triangle is degenerate and its closest point is not a number).
        float convex = d;
        for (const Vec3 *corner : { &piece.a, &piece.b, &piece.c })
            convex = std::max(convex, (*corner - surface::closestOnTriangle(*corner, tri[0], tri[1], tri[2])).length());
        const float ub = std::isfinite(convex) ? std::min(convex, d + radius) : d + radius;
        best = std::max(best, d);
        // THE DECISION ALONE (`decideOnly`, the per-group verify; CLUSTER-VERIFY-PAR-1): a
        // caller that asks only "past `stop`?" has its answer the moment the walk's value
        // is — what it returns is at least `best` and at least `upper`, so walking on
        // could only confirm it. Measured on the temple's mesh 2: the verify 4.5 -> 2.0 s
        // (single facets of a scan walked for up to 0.6 s to find a maximum nobody read).
        if (decideOnly && best > stop) return best;
        if (ub <= std::max(std::max(std::max(stop, best), upper), floorTol)) continue;   // cannot pass the bound
        // TIGHT (ATOM-UPGRADE stage 1's fix, kept): a piece closes once its bound is within 5 % of the walk's own
        // running lower bound — never of an outside scale (the old tolerance was 5 % of clusterlod's attribute
        // estimate, so no piece ever refined and the walk stored its first upper bound: finding 4 of the audit).
        const float tol = std::max(floorTol, 0.05f * std::max(stop, best));
        if (ub - d <= tol || piece.level >= kFacetMaxLevels) {
            upper = std::max(upper, ub);
            if (decideOnly && upper > stop) return upper;
            continue;
        }
        // LONGEST-EDGE BISECTION (kept from ATOM-UPGRADE stage 1; was the 1-to-4 split): a sliver's pieces shrink along
        // its length, where its radius is — the 1-to-4 split kept every piece a sliver of the same aspect. Two halves
        // per level, twice the levels: the same smallest area.
        const float lab = (piece.b - piece.a).lengthSquared(), lbc = (piece.c - piece.b).lengthSquared(),
                    lca = (piece.a - piece.c).lengthSquared();
        const int next = piece.level + 1;
        if (lab >= lbc && lab >= lca) {
            const Vec3 m = (piece.a + piece.b) * 0.5f;
            stack[top++] = Piece { piece.a, m, piece.c, next };
            stack[top++] = Piece { m, piece.b, piece.c, next };
        } else if (lbc >= lca) {
            const Vec3 m = (piece.b + piece.c) * 0.5f;
            stack[top++] = Piece { piece.a, piece.b, m, next };
            stack[top++] = Piece { piece.a, m, piece.c, next };
        } else {
            const Vec3 m = (piece.c + piece.a) * 0.5f;
            stack[top++] = Piece { piece.a, piece.b, m, next };
            stack[top++] = Piece { m, piece.b, piece.c, next };
        }
    }
    return std::max(best, upper);
}

using Tri = std::array<unsigned, 3>;

Tri sortedTri(const std::vector<unsigned> &idx, size_t t)
{
    Tri f = { idx[t * 3], idx[t * 3 + 1], idx[t * 3 + 2] };
    std::sort(f.begin(), f.end());
    return f;
}

std::vector<Tri> facetSet(const std::vector<unsigned> &idx)
{
    std::vector<Tri> s(idx.size() / 3);
    for (size_t t = 0; t < s.size(); ++t) s[t] = sortedTri(idx, t);
    std::sort(s.begin(), s.end());
    return s;
}

void growBox(const Vec3 &p, Vec3 &lo, Vec3 &hi)
{
    lo = Vec3(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
    hi = Vec3(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
}

}   // namespace

StepMeasure measureStep(const LocalSpace &space,
                        const std::vector<unsigned> &inTris, const std::vector<unsigned> &outTris,
                        const std::vector<unsigned> &externalWelds, float budget, float floorLen, bool exact)
{
    StepMeasure m;
    const float *P = space.pos.data();
    const int pc = 3;
    const std::vector<unsigned> &weld = space.weld;
    std::vector<unsigned> keptV(outTris.begin(), outTris.end());
    std::sort(keptV.begin(), keptV.end());
    keptV.erase(std::unique(keptV.begin(), keptV.end()), keptV.end());
    std::vector<unsigned> inV(inTris.begin(), inTris.end());
    std::sort(inV.begin(), inV.end());
    inV.erase(std::unique(inV.begin(), inV.end()), inV.end());
    const auto kept = [&](unsigned v) { return std::binary_search(keptV.begin(), keptV.end(), v); };
    std::vector<unsigned> fan;

    // The grids: the output (rules 1, 2, 4) and the input (rule 5), each over the group's box.
    Vec3 lo = vertexOf(P, pc, inTris[0]), hi = lo;
    for (unsigned v : inV) growBox(vertexOf(P, pc, v), lo, hi);
    surface::TriangleGrid gridO, gridI;
    if (!outTris.empty()) gridO.build(P, pc, outTris, lo, hi, soupGridRes(P, pc, outTris, lo, hi));
    gridI.build(P, pc, inTris, lo, hi, soupGridRes(P, pc, inTris, lo, hi));
    surface::TriangleGrid::Query q;

    // 3. THE ISLANDS: an island with input vertices and no output vertex is DROPPED here.
    std::vector<unsigned> survivingComps;
    for (unsigned v : keptV) if (space.comp[v] < space.islandCount) survivingComps.push_back(space.comp[v]);
    std::sort(survivingComps.begin(), survivingComps.end());
    survivingComps.erase(std::unique(survivingComps.begin(), survivingComps.end()), survivingComps.end());
    const auto dropped = [&](unsigned v) {
        const unsigned c = space.comp[v];
        return c < space.islandCount && !std::binary_search(survivingComps.begin(), survivingComps.end(), c);
    };
    {
        struct Part { Vec3 lo, hi; float bestArea = -1.0f; unsigned bestTri = 0; bool seen = false; };
        std::vector<std::pair<unsigned, Part>> parts;   // comp -> its dropped part
        std::vector<unsigned> slotOf;
        std::vector<std::pair<unsigned, size_t>> index;
        for (size_t t = 0; t < inTris.size() / 3; ++t) {
            const unsigned v0 = inTris[t * 3];
            if (!dropped(v0)) continue;
            const unsigned c = space.comp[v0];
            auto it = std::lower_bound(index.begin(), index.end(), std::make_pair(c, size_t(0)));
            size_t k;
            if (it == index.end() || it->first != c) {
                k = parts.size();
                parts.push_back({ c, Part() });
                index.insert(it, { c, k });
            } else {
                k = it->second;
            }
            Part &p = parts[k].second;
            for (int j = 0; j < 3; ++j) {
                const Vec3 x = vertexOf(P, pc, inTris[t * 3 + size_t(j)]);
                if (!p.seen) { p.lo = p.hi = x; p.seen = true; }
                growBox(x, p.lo, p.hi);
            }
            const Vec3 a = vertexOf(P, pc, inTris[t * 3]), b = vertexOf(P, pc, inTris[t * 3 + 1]),
                       c3 = vertexOf(P, pc, inTris[t * 3 + 2]);
            const float area = Vec3::crossProduct(b - a, c3 - a).length();
            if (area > p.bestArea) { p.bestArea = area; p.bestTri = unsigned(t); }
        }
        m.islandsDropped = int(parts.size());
        for (const auto &cp : parts) {
            const float diag = (cp.second.hi - cp.second.lo).length();
            m.droppedIsland = std::max(m.droppedIsland, diag);
            if (diag > budget) m.keepTris.push_back(cp.second.bestTri);
        }
    }

    // THE LOOPS RUN ON THE BAKE'S THREADS (a level of one or two groups — every small mesh, every mesh's last levels —
    // would otherwise measure on one thread): chunks of a size that depends on the input alone, each with its own
    // running maximum (the stop of its queries, which moves how far a query walks, never the decision) and its own
    // offender list, folded in chunk order — the answer does not depend on the width (the bakepool rules).
    const int width = bakepool::width();
    surface::Queries queries(static_cast<size_t>(width));
    struct Chunk { float max = 0.0f; std::vector<unsigned> fan; };
    // THE SEED (exact mode): every chunk's running maximum starts at the largest term found so far — a facet at or under
    // it cannot raise the step and closes at its first piece. The STEP stays exact (it is the maximum of every term);
    // a term's own number is then the largest value found above the seed (a diagnostic).
    float seed = m.droppedIsland;
    const auto loop = [&](size_t n, float &term, auto &&body) {
        const bakepool::Range range(n, kFacetGrain);
        std::vector<Chunk> chunks(range.count);
        if (exact) for (Chunk &c : chunks) c.max = std::min(seed, budget);
        bakepool::forRange(range, width, [&](size_t c, size_t begin, size_t end, int slot) {
            for (size_t i = begin; i < end; ++i) body(i, queries[size_t(slot)], chunks[c]);
        });
        for (const Chunk &c : chunks) {
            term = std::max(term, c.max);
            fan.insert(fan.end(), c.fan.begin(), c.fan.end());
        }
        seed = std::max(seed, term);
    };

    // 1. THE REMOVED VERTICES of surviving islands.
    if (!outTris.empty())
        loop(inV.size(), m.removedVertex, [&](size_t i, surface::TriangleGrid::Query &qq, Chunk &ch) {
            const unsigned v = inV[i];
            if (kept(v) || dropped(v)) return;
            const float d = std::min(gridO.closest(qq, vertexOf(P, pc, v), nullptr, nullptr, nullptr,
                                                   exact ? -1.0f : budget), space.cap[v]);
            if (!std::isfinite(d)) return;
            ch.max = std::max(ch.max, d);
            if (d > budget) ch.fan.push_back(weld[v]);
        });

    // 2. THE LOST FACETS (input facets the output does not have), against the output; 5. THE ADDED FACETS (output
    // facets the input does not have), against the input. One walk each: stop = min(the chunk's running maximum,
    // budget) when exact, the budget when deciding — a facet at or under it is neither a new maximum nor an offender
    // and closes early.
    const auto walk = [&](const Vec3 &a, const Vec3 &b, const Vec3 &c, float stop, const surface::TriangleGrid &grid,
                          const std::vector<unsigned> &soup, bool coverFirst, surface::TriangleGrid::Query &qq) {
        const auto dist = [&](const Vec3 &p, Vec3 *tri, float within) {
            return nearestOn(P, pc, grid, soup, qq, p, tri, within);
        };
        thread_local std::vector<unsigned> coverScratch;
        // AN ADDED FACET USUALLY LIES ON THE INPUT (a flat stretch collapsed into fewer, larger triangles): its cover
        // is exact and cheap, so it is asked first; a lost facet usually closes in a few pieces, so it walks first.
        if (coverFirst) {
            const float r = coplanarCover(P, pc, grid, soup, a, b, c, std::max(floorLen, 0.05f * std::max(stop, floorLen)),
                                          coverScratch);
            if (r >= 0.0f) return r;
        }
        float r = facetMax(a, b, c, stop, floorLen, dist, kFacetQuickPieces, !exact);
        if (!std::isnan(r)) return r;
        if (!coverFirst) {
            r = coplanarCover(P, pc, grid, soup, a, b, c, std::max(floorLen, 0.05f * std::max(stop, floorLen)), coverScratch);
            if (r >= 0.0f) return r;
        }
        return facetMax(a, b, c, stop, floorLen, dist, SIZE_MAX, !exact);
    };
    if (!outTris.empty()) {
        const std::vector<Tri> outSet = facetSet(outTris);
        loop(inTris.size() / 3, m.lostFacet, [&](size_t t, surface::TriangleGrid::Query &qq, Chunk &ch) {
            if (std::binary_search(outSet.begin(), outSet.end(), sortedTri(inTris, t))) return;
            const unsigned v0 = inTris[t * 3];
            if (dropped(v0)) return;
            const float cap = space.cap[v0];
            const float stop = exact ? std::min(ch.max, budget) : budget;
            if (!(cap > stop)) return;   // its island cannot pass it
            const float d = std::min(walk(vertexOf(P, pc, inTris[t * 3]), vertexOf(P, pc, inTris[t * 3 + 1]),
                                          vertexOf(P, pc, inTris[t * 3 + 2]), stop, gridO, outTris, false, qq), cap);
            if (!std::isfinite(d) || d <= stop) return;
            ch.max = std::max(ch.max, d);
            if (d > budget) for (int k = 0; k < 3; ++k) ch.fan.push_back(weld[inTris[t * 3 + size_t(k)]]);
        });
        const std::vector<Tri> inSet = facetSet(inTris);
        loop(outTris.size() / 3, m.addedFacet, [&](size_t t, surface::TriangleGrid::Query &qq, Chunk &ch) {
            if (std::binary_search(inSet.begin(), inSet.end(), sortedTri(outTris, t))) return;
            const float stop = exact ? std::min(ch.max, budget) : budget;
            const float d = walk(vertexOf(P, pc, outTris[t * 3]), vertexOf(P, pc, outTris[t * 3 + 1]),
                                 vertexOf(P, pc, outTris[t * 3 + 2]), stop, gridI, inTris, true, qq);
            if (!std::isfinite(d) || d <= stop) return;
            ch.max = std::max(ch.max, d);
            if (d > budget) for (int k = 0; k < 3; ++k) ch.fan.push_back(weld[outTris[t * 3 + size_t(k)]]);
        });
    }

    // 4. THE EXTERNAL POSITIONS the output lost.
    if (!externalWelds.empty()) {
        std::vector<unsigned> outW;
        for (unsigned v : keptV) outW.push_back(weld[v]);
        std::sort(outW.begin(), outW.end());
        std::vector<unsigned> inW;
        for (unsigned v : inV) inW.push_back(weld[v]);
        std::sort(inW.begin(), inW.end());
        inW.erase(std::unique(inW.begin(), inW.end()), inW.end());
        for (unsigned w : externalWelds) {
            if (!std::binary_search(inW.begin(), inW.end(), w) || std::binary_search(outW.begin(), outW.end(), w)) continue;
            ++m.lockedLost;
            const float d = outTris.empty() ? std::numeric_limits<float>::infinity()
                                            : gridO.closest(q, vertexOf(P, pc, w));
            m.lostLocked = std::max(m.lostLocked, std::isfinite(d) ? d : 0.0f);
            fan.push_back(w);
        }
    }

    std::sort(fan.begin(), fan.end());
    fan.erase(std::unique(fan.begin(), fan.end()), fan.end());
    m.fanWelds = std::move(fan);
    m.step = std::max({ m.removedVertex, m.lostFacet, m.addedFacet, m.droppedIsland, m.lostLocked, floorLen });
    return m;
}

}   // namespace atomdag
}   // namespace iris
