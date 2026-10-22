/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/simplify.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace iris
{
namespace atomdag
{
namespace
{

constexpr float kUvTriSizeMin = 1.0f / 1024.0f;   ///< Nanite's THRESH_UVS_ARE_SAME
constexpr float kNanitePositionScale = 4.0f;       ///< 1 / Nanite's DesiredSize 0.25
constexpr float kNaniteUvDivisor = 128.0f;         ///< Nanite's UV weight 1 / (128 uvTriSize)

Vec3 at(const std::vector<float> &p, unsigned i) { return Vec3(p[size_t(i) * 3], p[size_t(i) * 3 + 1], p[size_t(i) * 3 + 2]); }

/// The weights' algebra over the scratch (positions and the UV slot).
Weights weightsOf(const GroupScratch &sc)
{
    Weights w;
    const size_t triCount = sc.tris.size() / 3;
    if (triCount == 0) return w;
    double area = 0.0, uvArea = 0.0;
    std::vector<double> fan(sc.size(), 0.0);
    for (size_t t = 0; t < triCount; ++t) {
        const unsigned a = sc.tris[t * 3], b = sc.tris[t * 3 + 1], c = sc.tris[t * 3 + 2];
        const Vec3 pa = at(sc.pos, a);
        const double ar = double(Vec3::crossProduct(at(sc.pos, b) - pa, at(sc.pos, c) - pa).length()) * 0.5;
        area += ar;
        fan[a] += ar; fan[b] += ar; fan[c] += ar;
        if (sc.uvAt >= 0) {
            const float *ua = &sc.attr[size_t(a) * sc.attrCount + size_t(sc.uvAt)],
                        *ub = &sc.attr[size_t(b) * sc.attrCount + size_t(sc.uvAt)],
                        *uc = &sc.attr[size_t(c) * sc.attrCount + size_t(sc.uvAt)];
            uvArea += 0.5 * std::fabs(double((ub[0] - ua[0]) * (uc[1] - ua[1]) - (ub[1] - ua[1]) * (uc[0] - ua[0])));
        }
    }
    double sum = 0.0;
    for (double f : fan) sum += std::sqrt(f);   // every scratch vertex is a wedge of the input
    w.tGroup = float(std::sqrt(area / double(triCount)));
    w.sGroup = sc.size() ? float(sum / double(sc.size())) : 0.0f;
    w.uvTriSize = std::max(float(std::sqrt(uvArea / double(triCount))), kUvTriSizeMin);
    if (w.sGroup > 0.0f) {
        w.normal = kNanitePositionScale * w.tGroup / w.sGroup;
        w.uv = kNanitePositionScale * w.tGroup / (kNaniteUvDivisor * w.uvTriSize * w.sGroup);
    }
    return w;
}

}   // namespace

GroupScratch makeScratch(const MeshView &mesh, const std::vector<unsigned> &tris, WeightArm arm)
{
    GroupScratch sc;
    std::unordered_map<unsigned, unsigned> localOf;
    localOf.reserve(tris.size());
    sc.tris.resize(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        auto it = localOf.emplace(tris[i], unsigned(sc.globalOf.size())).first;
        if (it->second == sc.globalOf.size()) sc.globalOf.push_back(tris[i]);
        sc.tris[i] = it->second;
    }
    const size_t n = sc.size();
    sc.pos.resize(n * 3);
    for (size_t v = 0; v < n; ++v)
        for (int k = 0; k < 3; ++k) sc.pos[v * 3 + size_t(k)] = mesh.positions[size_t(sc.globalOf[v]) * size_t(mesh.posComps) + size_t(k)];
    if (mesh.normals) { sc.nrmAt = int(sc.attrCount); sc.attrCount += 3; }
    if (mesh.uvs) { sc.uvAt = int(sc.attrCount); sc.attrCount += 2; }
    sc.attr.resize(n * sc.attrCount);
    for (size_t v = 0; v < n && sc.attrCount; ++v) {
        float *dst = &sc.attr[v * sc.attrCount];
        const size_t g = sc.globalOf[v];
        if (sc.nrmAt >= 0) for (int k = 0; k < 3; ++k) dst[sc.nrmAt + k] = mesh.normals[g * size_t(mesh.nrmComps) + size_t(k)];
        if (sc.uvAt >= 0) for (int k = 0; k < 2; ++k) dst[sc.uvAt + k] = mesh.uvs[g * size_t(mesh.uvComps) + size_t(k)];
    }
    sc.weights = weightsOf(sc);
    sc.weightOf.assign(sc.attrCount, 0.0f);
    // EVERY WEIGHED ATTRIBUTE AT A POSITIVE WEIGHT (correction 3c): a zero weight is remapped out of the quadrics;
    // POSONLY = every attribute at the floor.
    const float floorW = 1e-6f;
    const bool nanite = arm == WeightArm::Nanite;
    if (sc.nrmAt >= 0)
        for (int k = 0; k < 3; ++k) sc.weightOf[size_t(sc.nrmAt + k)] = nanite ? std::max(sc.weights.normal, floorW) : floorW;
    if (sc.uvAt >= 0)
        for (int k = 0; k < 2; ++k) sc.weightOf[size_t(sc.uvAt + k)] = nanite ? std::max(sc.weights.uv, floorW) : floorW;
    return sc;
}

LocalSpace makeSpace(const GroupScratch &sc, const std::vector<unsigned> &weld, const std::vector<unsigned> &compOf,
                     const std::vector<float> &capOf, size_t islandCount)
{
    LocalSpace s;
    const size_t n = sc.size();
    s.islandCount = islandCount;
    s.pos = sc.pos;
    s.weld.resize(n);
    s.comp.resize(n);
    s.cap.resize(n);
    for (size_t v = 0; v < n; ++v) {
        const unsigned g = sc.globalOf[v];
        s.weld[v] = weld[g];
        s.comp[v] = compOf[g];
        s.cap[v] = capOf[g];
    }
    return s;
}

Simplified simplifyGroup(const GroupScratch &sc, const std::vector<unsigned char> &lock, size_t targetTris,
                         float targetError)
{
    Simplified out;
    bool anyLock = false;
    for (unsigned char l : lock) anyLock = anyLock || l;
    std::vector<unsigned> dst(sc.tris.size());
    const size_t count = meshopt_simplifyWithAttributes(
        dst.data(), sc.tris.data(), sc.tris.size(), sc.pos.data(), sc.size(), sizeof(float) * 3,
        sc.attrCount ? sc.attr.data() : nullptr, sizeof(float) * sc.attrCount,
        sc.attrCount ? sc.weightOf.data() : nullptr, sc.attrCount, anyLock ? lock.data() : nullptr,
        targetTris * 3, targetError, meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive, &out.resultError);
    out.tris.assign(dst.begin(), dst.begin() + std::ptrdiff_t(count));
    if (!std::isfinite(out.resultError) || out.resultError < 0.0f) out.resultError = 0.0f;
    return out;
}

size_t weldWithin(GroupScratch &sc, LocalSpace &space, const std::vector<unsigned char> &lock, float radius)
{
    const size_t n = sc.size();
    if (!(radius > 0.0f) || !std::isfinite(radius) || n == 0) return 0;
    // the group's positions: per weld id its first scratch vertex, in first-use order
    std::unordered_map<unsigned, unsigned> slotOf;
    slotOf.reserve(n);
    std::vector<unsigned> posOf(n), first;
    for (size_t v = 0; v < n; ++v) {
        const auto it = slotOf.emplace(space.weld[v], unsigned(first.size())).first;
        if (it->second == first.size()) first.push_back(unsigned(v));
        posOf[v] = it->second;
    }
    const size_t np = first.size();
    std::vector<unsigned char> held(np, 0u);
    for (size_t v = 0; v < n; ++v) if (lock[v]) held[posOf[v]] = 1u;
    const auto island = [&](unsigned p) { return space.comp[first[p]]; };
    const auto at = [&](unsigned p) { return Vec3(sc.pos[size_t(first[p]) * 3], sc.pos[size_t(first[p]) * 3 + 1], sc.pos[size_t(first[p]) * 3 + 2]); };
    // the anchors in a hash grid of cell = radius (the 27 cells around a position cover its radius)
    struct Cell { long long x = 0, y = 0, z = 0; bool operator==(const Cell &o) const { return x == o.x && y == o.y && z == o.z; } };
    struct CellHash { size_t operator()(const Cell &c) const { return size_t(c.x * 73856093LL) ^ size_t(c.y * 19349663LL) ^ size_t(c.z * 83492791LL); } };
    const auto cellOf = [&](const Vec3 &q) {
        const auto k = [&](float x) { return (long long)std::floor(std::clamp(double(x) / double(radius), -1e15, 1e15)); };
        return Cell { k(q.x()), k(q.y()), k(q.z()) };
    };
    std::unordered_map<Cell, std::vector<unsigned>, CellHash> grid;
    std::vector<std::vector<unsigned>> joined(np);   // per anchor: the islands snapped onto it
    std::vector<int> target(np, -1);
    // the islands a snap joins: union-find over island ids
    std::unordered_map<unsigned, unsigned> parent;
    const std::function<unsigned(unsigned)> root = [&](unsigned c) {
        auto it = parent.find(c);
        if (it == parent.end() || it->second == c) return c;
        const unsigned r = root(it->second);
        parent[c] = r;
        return r;
    };
    size_t snapped = 0;
    for (unsigned p = 0; p < np; ++p) {
        const unsigned c = island(p);
        if (held[p] || c >= space.islandCount) continue;
        const Vec3 q = at(p);
        const Cell k = cellOf(q);
        int best = -1;
        float bestD = radius;
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                for (long long dz = -1; dz <= 1; ++dz) {
                    const auto it = grid.find(Cell { k.x + dx, k.y + dy, k.z + dz });
                    if (it == grid.end()) continue;
                    for (unsigned a : it->second) {
                        if (island(a) == c) continue;
                        if (std::find(joined[a].begin(), joined[a].end(), c) != joined[a].end()) continue;
                        const float d = (at(a) - q).length();
                        if (d < bestD || (d == bestD && (best < 0 || int(a) < best))) { bestD = d; best = int(a); }
                    }
                }
        if (best < 0) { grid[k].push_back(p); continue; }
        target[p] = best;
        joined[size_t(best)].push_back(c);
        const unsigned ra = root(c), rb = root(island(unsigned(best)));
        if (ra != rb) parent[std::max(ra, rb)] = std::min(ra, rb);
        ++snapped;
    }
    if (!snapped) return 0;
    for (size_t v = 0; v < n; ++v) {
        const int t = target[posOf[v]];
        if (t >= 0) for (int k = 0; k < 3; ++k) sc.pos[v * 3 + size_t(k)] = sc.pos[size_t(first[size_t(t)]) * 3 + size_t(k)];
        if (space.comp[v] < space.islandCount) space.comp[v] = root(space.comp[v]);
    }
    return snapped;
}

float positionEstimate(const GroupScratch &sc, const std::vector<unsigned char> &lock, size_t targetTris)
{
    bool anyLock = false;
    for (unsigned char l : lock) anyLock = anyLock || l;
    std::vector<unsigned> dst(sc.tris.size());
    float err = 0.0f;
    meshopt_simplifyWithAttributes(dst.data(), sc.tris.data(), sc.tris.size(), sc.pos.data(), sc.size(),
                                   sizeof(float) * 3, nullptr, 0, nullptr, 0, anyLock ? lock.data() : nullptr,
                                   targetTris * 3, FLT_MAX, meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive,
                                   &err);
    return std::isfinite(err) && err > 0.0f ? err : 0.0f;
}

size_t pruneUnlocked(const LocalSpace &space, const std::vector<unsigned> &lockedWelds, size_t targetTris,
                     float budget, std::vector<unsigned> &tris)
{
    size_t triCount = tris.size() / 3;
    if (triCount <= targetTris) return 0;
    // Pieces by position: union-find over the weld ids of the output.
    std::unordered_map<unsigned, unsigned> slot;
    std::vector<unsigned> parent;
    const auto idOf = [&](unsigned w) {
        auto it = slot.emplace(w, unsigned(parent.size())).first;
        if (it->second == parent.size()) parent.push_back(it->second);
        return it->second;
    };
    const auto root = [&](unsigned x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    std::vector<unsigned> triWeld(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) triWeld[i] = idOf(space.weld[tris[i]]);
    for (size_t t = 0; t < triCount; ++t) {
        const unsigned a = root(triWeld[t * 3]);
        for (int k = 1; k < 3; ++k) {
            const unsigned b = root(triWeld[t * 3 + size_t(k)]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    }
    struct Piece { Vec3 lo, hi; size_t tris = 0; bool locked = false; size_t first = 0; bool seen = false; };
    std::vector<Piece> pieces(parent.size());
    const auto position = [&](unsigned v) {
        return Vec3(space.pos[size_t(v) * 3], space.pos[size_t(v) * 3 + 1], space.pos[size_t(v) * 3 + 2]);
    };
    for (size_t t = 0; t < triCount; ++t) {
        Piece &p = pieces[root(triWeld[t * 3])];
        if (!p.seen) { p.seen = true; p.first = t; p.lo = p.hi = position(tris[t * 3]); }
        ++p.tris;
        for (int k = 0; k < 3; ++k) {
            const unsigned v = tris[t * 3 + size_t(k)];
            const Vec3 q = position(v);
            p.lo = Vec3(std::min(p.lo.x(), q.x()), std::min(p.lo.y(), q.y()), std::min(p.lo.z(), q.z()));
            p.hi = Vec3(std::max(p.hi.x(), q.x()), std::max(p.hi.y(), q.y()), std::max(p.hi.z(), q.z()));
            if (std::binary_search(lockedWelds.begin(), lockedWelds.end(), space.weld[v])) p.locked = true;
        }
    }
    std::vector<unsigned> order;
    for (size_t r = 0; r < pieces.size(); ++r)
        if (pieces[r].seen && !pieces[r].locked && (pieces[r].hi - pieces[r].lo).length() <= budget) order.push_back(unsigned(r));
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        const float da = (pieces[a].hi - pieces[a].lo).length(), db = (pieces[b].hi - pieces[b].lo).length();
        return da != db ? da < db : pieces[a].first < pieces[b].first;
    });
    std::vector<unsigned char> drop(pieces.size(), 0u);
    size_t removed = 0;
    for (unsigned r : order) {
        if (triCount <= targetTris) break;
        drop[r] = 1u;
        triCount -= pieces[r].tris;
        ++removed;
    }
    if (!removed) return 0;
    size_t write = 0;
    for (size_t t = 0; t < tris.size() / 3; ++t) {
        if (drop[root(triWeld[t * 3])]) continue;
        for (int k = 0; k < 3; ++k) tris[write++] = tris[t * 3 + size_t(k)];
    }
    tris.resize(write);
    return removed;
}

}   // namespace atomdag
}   // namespace iris
