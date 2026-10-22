/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/cards.h"

#include "import/bakeshared.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <unordered_map>

namespace iris
{
namespace atomdag
{
namespace
{

Vec3 posOf(const GroupScratch &sc, unsigned v)
{
    return Vec3(sc.pos[size_t(v) * 3], sc.pos[size_t(v) * 3 + 1], sc.pos[size_t(v) * 3 + 2]);
}

struct Card
{
    unsigned comp = 0;
    std::vector<unsigned> tris;   ///< triangle indices into the scratch's input
    Vec3 centre, normal, lo, hi;  ///< area-weighted centre, summed (unnormalised) normal, box
    float area = 0.0f;
    bool locked = false;
    bool taken = false;
};

/// The quad of a set (its four corners and two triangles' winding), or false when the seed's UV frame is degenerate.
struct Quad
{
    CardCorner c[4];
    bool flip = false;
    float area = 0.0f;
};

/// Solves the 3 x 3 system a x = b (Cramer); false when singular.
bool solve3(const double a[3][3], const double b[3], double x[3])
{
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
                       a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::fabs(det) > 1e-18)) return false;
    for (int k = 0; k < 3; ++k) {
        double m[3][3];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) m[i][j] = j == k ? b[i] : a[i][j];
        x[k] = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) / det;
    }
    return true;
}

bool buildQuad(const GroupScratch &sc, const std::vector<Card> &cards, const std::vector<unsigned> &set, Quad &q)
{
    // THE SEED: the set's largest card.
    unsigned seed = set[0];
    for (unsigned c : set) if (cards[c].area > cards[seed].area) seed = c;
    const Card &S = cards[seed];
    const Vec3 nSeed = S.normal.lengthSquared() > 0.0f ? S.normal.normalized() : Vec3(0, 0, 1);
    // THE PLANE: through the area-weighted centre, its normal the sources' (each triangle turned to the seed's side).
    Vec3 nSum(0, 0, 0), centre(0, 0, 0);
    double area = 0.0;
    for (unsigned c : set)
        for (unsigned t : cards[c].tris) {
            const Vec3 a = posOf(sc, sc.tris[size_t(t) * 3]), b = posOf(sc, sc.tris[size_t(t) * 3 + 1]),
                       d = posOf(sc, sc.tris[size_t(t) * 3 + 2]);
            const Vec3 x = Vec3::crossProduct(b - a, d - a);
            nSum += Vec3::dotProduct(x, nSeed) < 0.0f ? -x : x;
            const float ar = 0.5f * x.length();
            centre += (a + b + d) * (ar / 3.0f);
            area += double(ar);
        }
    if (!(area > 0.0)) return false;
    centre = centre * float(1.0 / area);
    const Vec3 n = nSum.lengthSquared() > 0.0f ? nSum.normalized() : nSeed;
    // THE SEED'S OWN FRAME and its affine map to its first UV set (least squares over its vertices).
    if (sc.uvAt < 0) return false;
    std::vector<unsigned> sv;
    for (unsigned t : S.tris) for (int k = 0; k < 3; ++k) sv.push_back(sc.tris[size_t(t) * 3 + size_t(k)]);
    std::sort(sv.begin(), sv.end());
    sv.erase(std::unique(sv.begin(), sv.end()), sv.end());
    Vec3 a1 = posOf(sc, sv.back()) - posOf(sc, sv.front());
    a1 = a1 - nSeed * Vec3::dotProduct(a1, nSeed);
    if (!(a1.lengthSquared() > 0.0f)) return false;
    a1 = a1.normalized();
    const Vec3 a2 = Vec3::crossProduct(nSeed, a1);
    const Vec3 cs = S.centre;
    double m[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } }, ru[3] = { 0, 0, 0 }, rv[3] = { 0, 0, 0 };
    float u0 = FLT_MAX, u1 = -FLT_MAX, v0 = FLT_MAX, v1 = -FLT_MAX;
    for (unsigned v : sv) {
        const Vec3 d = posOf(sc, v) - cs;
        const double row[3] = { double(Vec3::dotProduct(d, a1)), double(Vec3::dotProduct(d, a2)), 1.0 };
        const float *uv = &sc.attr[size_t(v) * sc.attrCount + size_t(sc.uvAt)];
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) m[i][j] += row[i] * row[j];
            ru[i] += row[i] * double(uv[0]);
            rv[i] += row[i] * double(uv[1]);
        }
        u0 = std::min(u0, uv[0]); u1 = std::max(u1, uv[0]); v0 = std::min(v0, uv[1]); v1 = std::max(v1, uv[1]);
    }
    double cu[3], cv[3];
    if (!solve3(m, ru, cu) || !solve3(m, rv, cv)) return false;
    // the seed's UV rectangle back in its frame: (x, y) = M^-1 (uv - b)
    const double det = cu[0] * cv[1] - cu[1] * cv[0];
    if (!(std::fabs(det) > 1e-18) || !(u1 > u0) || !(v1 > v0)) return false;
    const float rect[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
    Vec3 d[4], mid(0, 0, 0);
    Vec3 seedPoint[4];
    for (int i = 0; i < 4; ++i) {
        const double du = double(rect[i][0]) - cu[2], dv = double(rect[i][1]) - cv[2];
        const double x = (du * cv[1] - dv * cu[1]) / det, y = (cu[0] * dv - cv[0] * du) / det;
        seedPoint[i] = cs + a1 * float(x) + a2 * float(y);
        d[i] = a1 * float(x) + a2 * float(y);
        d[i] = d[i] - n * Vec3::dotProduct(d[i], n);   // onto the merged plane
        mid += d[i] * 0.25f;
    }
    for (Vec3 &x : d) x = x - mid;
    const float parArea = Vec3::crossProduct(d[1] - d[0], d[3] - d[0]).length();
    if (!(parArea > 0.0f)) return false;
    // AREA = the sources' summed area, NEVER SHRUNK (the coverage law; the mesh's box is recomputed after the build)
    const float f = std::sqrt(float(area) / parArea);
    for (int i = 0; i < 4; ++i) {
        const Vec3 p = centre + d[i] * f;
        CardCorner &c = q.c[i];
        c.pos[0] = p.x(); c.pos[1] = p.y(); c.pos[2] = p.z();
        c.nrm[0] = n.x(); c.nrm[1] = n.y(); c.nrm[2] = n.z();
        c.uv[0] = rect[i][0]; c.uv[1] = rect[i][1];
        // the seed's vertex nearest this corner of its own rectangle carries every other buffer
        unsigned best = sv[0];
        float bestD = FLT_MAX;
        for (unsigned v : sv) {
            const float dd = (posOf(sc, v) - seedPoint[i]).lengthSquared();
            if (dd < bestD) { bestD = dd; best = v; }
        }
        c.source = best;
    }
    const Vec3 p0(q.c[0].pos[0], q.c[0].pos[1], q.c[0].pos[2]), p1(q.c[1].pos[0], q.c[1].pos[1], q.c[1].pos[2]),
               p2(q.c[2].pos[0], q.c[2].pos[1], q.c[2].pos[2]), p3(q.c[3].pos[0], q.c[3].pos[1], q.c[3].pos[2]);
    q.area = 0.5f * (Vec3::crossProduct(p1 - p0, p2 - p0).length() + Vec3::crossProduct(p2 - p0, p3 - p0).length());
    if (!(q.area > 0.0f)) return false;
    q.flip = Vec3::dotProduct(Vec3::crossProduct(p1 - p0, p3 - p0), n) < 0.0f;
    for (CardCorner &c : q.c) c.diagonal = (p2 - p0).length();
    return true;
}

/// THE MEASURED PRICE of a set's quad — the one-step deviation of measure.h for this shape, exact, both ways, without
/// its general machinery (a set is a few dozen triangles against two):
///   the SOURCES against the quad (measure.h's removed vertices and lost facets): the distance to a convex polygon is a
///   convex function, so over every source facet it peaks at a corner — the maximum over the source vertices is exact;
///   each charged at most its own island's extent (the quad stands for its sources' islands: none is dropped);
///   the QUAD against the sources (the added facets, uncapped as measure.h's): a branch-and-bound walk over the quad's
///   two triangles — a piece is bounded below by its corners' distances and above by its centre's plus its
///   circumradius (the distance is 1-Lipschitz), split in four until the bounds close within 5 %; the exact answer is
///   the UPPER bound over the closed pieces (CLUSTER-LOCK-3's rule: never the sampled lower one). `exact` false = THE
///   DECISION: past the budget the moment a lower bound passes it (or a piece at the depth limit is not proven under
///   it), within it once every piece's upper bound is.
float priceOf(const GroupScratch &sc, const LocalSpace &space, const std::vector<Card> &cards,
              const std::vector<unsigned> &set, const Quad &q, float budget, float floorLen, bool exact)
{
    const Vec3 Q[4] = { Vec3(q.c[0].pos[0], q.c[0].pos[1], q.c[0].pos[2]), Vec3(q.c[1].pos[0], q.c[1].pos[1], q.c[1].pos[2]),
                        Vec3(q.c[2].pos[0], q.c[2].pos[1], q.c[2].pos[2]), Vec3(q.c[3].pos[0], q.c[3].pos[1], q.c[3].pos[2]) };
    const auto toQuad = [&](const Vec3 &p) {
        return std::min((p - surface::closestOnTriangle(p, Q[0], Q[1], Q[2])).length(),
                        (p - surface::closestOnTriangle(p, Q[0], Q[2], Q[3])).length());
    };
    // the sources against the quad (exact at the vertices)
    float price = floorLen;
    std::vector<Vec3> src;   // the sources' triangles, three corners each
    for (unsigned c : set)
        for (unsigned t : cards[c].tris)
            for (int k = 0; k < 3; ++k) {
                const unsigned v = sc.tris[size_t(t) * 3 + size_t(k)];
                const Vec3 p = posOf(sc, v);
                src.push_back(p);
                price = std::max(price, std::min(toQuad(p), space.cap[v]));
                if (!exact && price > budget) return price;
            }
    // the quad against the sources (branch and bound; no cap — measure.h's added facets carry none)
    const auto toSources = [&](const Vec3 &p) {
        float d = FLT_MAX;
        for (size_t i = 0; i < src.size(); i += 3)
            d = std::min(d, (p - surface::closestOnTriangle(p, src[i], src[i + 1], src[i + 2])).length());
        return d;
    };
    struct Piece { Vec3 a, b, c; int depth = 0; };
    std::vector<Piece> stack { { Q[0], Q[1], Q[2], 0 }, { Q[0], Q[2], Q[3], 0 } };
    constexpr float kTolerance = 0.05f;
    constexpr int kMaxDepth = 12;
    float upper = 0.0f;   // the exact mode's answer: the UPPER bound over the closed pieces (CLUSTER-LOCK-3's rule)
    while (!stack.empty()) {
        const Piece pc = stack.back();
        stack.pop_back();
        const float lo = std::max(toSources(pc.a), std::max(toSources(pc.b), toSources(pc.c)));
        price = std::max(price, lo);
        if (!exact && price > budget) return price;
        const Vec3 g = (pc.a + pc.b + pc.c) * (1.0f / 3.0f);
        const float r = std::max((pc.a - g).length(), std::max((pc.b - g).length(), (pc.c - g).length()));
        const float up = toSources(g) + r;
        if (!exact) {
            if (up <= budget) continue;                                       // proven under the budget
            if (pc.depth >= kMaxDepth) return up;                             // not proven: past it
        } else {
            if (up <= price * (1.0f + kTolerance) || pc.depth >= kMaxDepth) {   // closed: its bound stands
                upper = std::max(upper, up);
                continue;
            }
        }
        const Vec3 ab = (pc.a + pc.b) * 0.5f, bc = (pc.b + pc.c) * 0.5f, ca = (pc.c + pc.a) * 0.5f;
        stack.push_back({ pc.a, ab, ca, pc.depth + 1 });
        stack.push_back({ ab, pc.b, bc, pc.depth + 1 });
        stack.push_back({ ca, bc, pc.c, pc.depth + 1 });
        stack.push_back({ ab, bc, ca, pc.depth + 1 });
    }
    return exact ? std::max(price, upper) : price;
}

}   // namespace

bool isCardGroup(const GroupScratch &sc, const LocalSpace &space, const std::vector<size_t> &islandTriangles)
{
    const size_t tris = sc.tris.size() / 3;
    size_t card = 0;
    for (size_t t = 0; t < tris; ++t) {
        const unsigned c = space.comp[sc.tris[t * 3]];
        if (c < islandTriangles.size() && islandTriangles[c] < size_t(kCardTriangles)) ++card;
    }
    return card * 2 > tris;
}

CardMerge mergeCards(const GroupScratch &sc, const LocalSpace &space, const std::vector<unsigned char> &lock,
                     float budget, float floorLen)
{
    CardMerge r;
    const size_t tris = sc.tris.size() / 3, n = sc.size();
    // THE CARDS: the input's islands under kCardTriangles level-0 triangles... read here as every island of the input
    // with fewer than kCardTriangles triangles IN the input (a card split across groups holds a locked position).
    std::vector<Card> cards;
    std::unordered_map<unsigned, unsigned> cardOf;
    std::vector<int> triCard(tris, -1);
    for (size_t t = 0; t < tris; ++t) {
        const unsigned c = space.comp[sc.tris[t * 3]];
        if (c >= space.islandCount) continue;
        const auto it = cardOf.emplace(c, unsigned(cards.size())).first;
        if (it->second == cards.size()) { cards.emplace_back(); cards.back().comp = c; }
        cards[it->second].tris.push_back(unsigned(t));
        triCard[t] = int(it->second);
    }
    for (Card &c : cards) {
        if (c.tris.size() >= size_t(kCardTriangles)) { c.locked = true; continue; }
        double area = 0.0;
        Vec3 centre(0, 0, 0), normal(0, 0, 0);
        bool first = true;
        for (unsigned t : c.tris) {
            const Vec3 a = posOf(sc, sc.tris[size_t(t) * 3]), b = posOf(sc, sc.tris[size_t(t) * 3 + 1]),
                       d = posOf(sc, sc.tris[size_t(t) * 3 + 2]);
            const Vec3 x = Vec3::crossProduct(b - a, d - a);
            normal += x;
            const float ar = 0.5f * x.length();
            area += double(ar);
            centre += (a + b + d) * (ar / 3.0f);
            for (int k = 0; k < 3; ++k) {
                const unsigned v = sc.tris[size_t(t) * 3 + size_t(k)];
                if (lock[v]) c.locked = true;
                const Vec3 p = posOf(sc, v);
                if (first) { c.lo = c.hi = p; first = false; }
                c.lo = Vec3(std::min(c.lo.x(), p.x()), std::min(c.lo.y(), p.y()), std::min(c.lo.z(), p.z()));
                c.hi = Vec3(std::max(c.hi.x(), p.x()), std::max(c.hi.y(), p.y()), std::max(c.hi.z(), p.z()));
            }
        }
        c.area = float(area);
        c.normal = normal;
        c.centre = area > 0.0 ? centre * float(1.0 / area) : (c.lo + c.hi) * 0.5f;
        if (!(area > 0.0)) c.locked = true;
    }
    // THE NEIGHBOURS: a hash grid of the card centres, cell = the budget (the 27 cells around a seed cover it).
    struct Cell { long long x = 0, y = 0, z = 0; bool operator==(const Cell &o) const { return x == o.x && y == o.y && z == o.z; } };
    struct CellHash { size_t operator()(const Cell &c) const { return size_t(c.x * 73856093LL) ^ size_t(c.y * 19349663LL) ^ size_t(c.z * 83492791LL); } };
    const auto cellOf = [&](const Vec3 &p) {
        const auto k = [&](float x) { return (long long)std::floor(std::clamp(double(x) / double(budget), -1e15, 1e15)); };
        return Cell { k(p.x()), k(p.y()), k(p.z()) };
    };
    std::unordered_map<Cell, std::vector<unsigned>, CellHash> grid;
    if (budget > 0.0f && std::isfinite(budget))
        for (unsigned c = 0; c < cards.size(); ++c) if (!cards[c].locked) grid[cellOf(cards[c].centre)].push_back(c);
    std::vector<Quad> quads;
    std::vector<unsigned char> merged(cards.size(), 0u);
    for (unsigned s = 0; s < cards.size() && !grid.empty(); ++s) {
        if (cards[s].locked || cards[s].taken) continue;
        const Cell k = cellOf(cards[s].centre);
        std::vector<std::pair<float, unsigned>> near;
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find(Cell { k.x + dx, k.y + dy, k.z + dz });
                    if (it == grid.end()) continue;
                    for (unsigned c : it->second) {
                        if (c == s || cards[c].taken) continue;
                        const float d = (cards[c].centre - cards[s].centre).length();
                        if (d <= budget) near.push_back({ d, c });
                    }
                }
        if (near.empty()) continue;
        std::sort(near.begin(), near.end());
        if (near.size() > size_t(kCardsPerSet - 1)) near.resize(size_t(kCardsPerSet - 1));
        // the set, priced; past the budget it drops its farthest card, down to a pair
        for (size_t take = near.size(); take >= 1; --take) {
            std::vector<unsigned> set { s };
            for (size_t i = 0; i < take; ++i) set.push_back(near[i].second);
            Quad q;
            if (!buildQuad(sc, cards, set, q)) break;
            ++r.pricings;
            // THE DECISION first (every walk stops at the budget: a set past it costs little), then the kept set's
            // exact price (the stored step)
            if (!(priceOf(sc, space, cards, set, q, budget, floorLen, false) <= budget)) continue;
            const float walked = priceOf(sc, space, cards, set, q, budget, floorLen, true);   // its upper bound
            const float price = std::min(walked, budget);   // the decision proved it within the budget
            r.measured = std::max(r.measured, walked);
            for (unsigned c : set) { cards[c].taken = true; merged[c] = 1u; r.sourceArea += double(cards[c].area); }
            r.quadArea += double(q.area);
            r.step = std::max(r.step, price);
            r.maxCentreDistance = std::max(r.maxCentreDistance, near[take - 1].first);
            r.cardsMerged += int(set.size());
            quads.push_back(q);
            break;
        }
    }
    r.sets = int(quads.size());
    if (quads.empty()) return r;
    for (size_t t = 0; t < tris; ++t)
        if (triCard[t] < 0 || !merged[size_t(triCard[t])])
            r.out.insert(r.out.end(), { sc.tris[t * 3], sc.tris[t * 3 + 1], sc.tris[t * 3 + 2] });
    for (const Quad &q : quads) {
        const unsigned b = unsigned(n + r.corners.size());
        for (const CardCorner &c : q.c) r.corners.push_back(c);
        if (q.flip) r.out.insert(r.out.end(), { b, b + 2, b + 1, b, b + 3, b + 2 });
        else r.out.insert(r.out.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
    }
    r.step = std::max(r.step, floorLen);
    return r;
}

}   // namespace atomdag
}   // namespace iris
