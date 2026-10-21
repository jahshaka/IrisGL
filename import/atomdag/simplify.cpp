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
#include <numeric>
#include <unordered_map>

namespace iris
{
namespace atomdag
{
namespace
{

float triangleArea(const MeshView &m, unsigned a, unsigned b, unsigned c)
{
    const Vec3 pa = m.position(a);
    return Vec3::crossProduct(m.position(b) - pa, m.position(c) - pa).length() * 0.5f;
}

constexpr float kUvTriSizeMin = 1.0f / 1024.0f;   ///< Nanite's THRESH_UVS_ARE_SAME
constexpr float kNanitePositionScale = 4.0f;       ///< 1 / Nanite's DesiredSize 0.25
constexpr float kNaniteUvDivisor = 128.0f;         ///< Nanite's UV weight 1 / (128 uvTriSize)

}   // namespace

Weights groupWeights(const MeshView &mesh, const std::vector<unsigned> &tris)
{
    Weights w;
    const size_t triCount = tris.size() / 3;
    if (triCount == 0) return w;
    double area = 0.0, uvArea = 0.0;
    std::unordered_map<unsigned, double> fan;   // wedge -> sum of incident area
    fan.reserve(tris.size());
    for (size_t t = 0; t < triCount; ++t) {
        const unsigned a = tris[t * 3], b = tris[t * 3 + 1], c = tris[t * 3 + 2];
        const double ar = triangleArea(mesh, a, b, c);
        area += ar;
        fan[a] += ar; fan[b] += ar; fan[c] += ar;
        if (mesh.uvs) {
            const float *ua = mesh.uvs + size_t(a) * size_t(mesh.uvComps), *ub = mesh.uvs + size_t(b) * size_t(mesh.uvComps),
                        *uc = mesh.uvs + size_t(c) * size_t(mesh.uvComps);
            uvArea += 0.5 * std::fabs(double((ub[0] - ua[0]) * (uc[1] - ua[1]) - (ub[1] - ua[1]) * (uc[0] - ua[0])));
        }
    }
    // The mean over the wedges, in first-use order (deterministic; the map is only the accumulator).
    double sum = 0.0;
    size_t wedges = 0;
    for (unsigned v : tris) {
        auto it = fan.find(v);
        if (it == fan.end() || it->second < 0.0) continue;
        sum += std::sqrt(it->second);
        it->second = -1.0;   // counted
        ++wedges;
    }
    w.tGroup = float(std::sqrt(area / double(triCount)));
    w.sGroup = wedges ? float(sum / double(wedges)) : 0.0f;
    w.uvTriSize = std::max(float(std::sqrt(uvArea / double(triCount))), kUvTriSizeMin);
    if (w.sGroup > 0.0f) {
        w.normal = kNanitePositionScale * w.tGroup / w.sGroup;
        w.uv = kNanitePositionScale * w.tGroup / (kNaniteUvDivisor * w.uvTriSize * w.sGroup);
    }
    return w;
}

Simplified simplifyGroup(const MeshView &mesh, const std::vector<unsigned> &weld, const std::vector<unsigned> &tris,
                         const std::vector<unsigned> &lockedWelds, size_t targetTris, float targetError,
                         bool attributes, const Weights &weights)
{
    Simplified out;
    // THE DENSE SCRATCH: the group's vertices in first-use order.
    std::unordered_map<unsigned, unsigned> localOf;
    localOf.reserve(tris.size());
    std::vector<unsigned> globalOf, local(tris.size());
    for (size_t i = 0; i < tris.size(); ++i) {
        auto it = localOf.emplace(tris[i], unsigned(globalOf.size())).first;
        if (it->second == globalOf.size()) globalOf.push_back(tris[i]);
        local[i] = it->second;
    }
    const size_t n = globalOf.size();
    std::vector<float> pos(n * 3);
    for (size_t v = 0; v < n; ++v)
        for (int k = 0; k < 3; ++k) pos[v * 3 + size_t(k)] = mesh.positions[size_t(globalOf[v]) * size_t(mesh.posComps) + size_t(k)];
    const bool nrm = attributes && mesh.normals && weights.normal > 0.0f;
    const bool uv = attributes && mesh.uvs && weights.uv > 0.0f;
    const size_t attrCount = (nrm ? 3u : 0u) + (uv ? 2u : 0u);
    std::vector<float> attr(n * attrCount), aw;
    if (nrm) aw.insert(aw.end(), 3, weights.normal);
    if (uv) aw.insert(aw.end(), 2, weights.uv);
    for (size_t v = 0; v < n && attrCount; ++v) {
        float *dst = &attr[v * attrCount];
        const size_t g = globalOf[v];
        if (nrm) { for (int k = 0; k < 3; ++k) dst[k] = mesh.normals[g * size_t(mesh.nrmComps) + size_t(k)]; dst += 3; }
        if (uv) { dst[0] = mesh.uvs[g * size_t(mesh.uvComps)]; dst[1] = mesh.uvs[g * size_t(mesh.uvComps) + 1]; }
    }
    std::vector<unsigned char> lock(n, 0u);
    bool anyLock = false;
    for (size_t v = 0; v < n; ++v)
        if (std::binary_search(lockedWelds.begin(), lockedWelds.end(), weld[globalOf[v]])) {
            lock[v] = meshopt_SimplifyVertex_Lock;
            anyLock = true;
        }
    std::vector<unsigned> dst(tris.size());
    const unsigned options = meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPermissive;
    const size_t count = meshopt_simplifyWithAttributes(
        dst.data(), local.data(), local.size(), pos.data(), n, sizeof(float) * 3,
        attrCount ? attr.data() : nullptr, sizeof(float) * attrCount, attrCount ? aw.data() : nullptr, attrCount,
        anyLock ? lock.data() : nullptr, targetTris * 3, targetError, options, &out.resultError);
    out.tris.resize(count);
    for (size_t i = 0; i < count; ++i) out.tris[i] = globalOf[dst[i]];
    if (!std::isfinite(out.resultError) || out.resultError < 0.0f) out.resultError = 0.0f;
    return out;
}

size_t pruneUnlocked(const MeshView &mesh, const std::vector<unsigned> &weld, const std::vector<unsigned> &lockedWelds,
                     size_t targetTris, float budget, std::vector<unsigned> &tris)
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
    for (size_t i = 0; i < tris.size(); ++i) triWeld[i] = idOf(weld[tris[i]]);
    for (size_t t = 0; t < triCount; ++t) {
        const unsigned a = root(triWeld[t * 3]);
        for (int k = 1; k < 3; ++k) {
            const unsigned b = root(triWeld[t * 3 + size_t(k)]);
            if (a != b) parent[std::max(a, b)] = std::min(a, b);
        }
    }
    struct Piece { Vec3 lo, hi; size_t tris = 0; bool locked = false; size_t first = 0; bool seen = false; };
    std::vector<Piece> pieces(parent.size());
    for (size_t t = 0; t < triCount; ++t) {
        Piece &p = pieces[root(triWeld[t * 3])];
        if (!p.seen) { p.seen = true; p.first = t; p.lo = p.hi = mesh.position(tris[t * 3]); }
        ++p.tris;
        for (int k = 0; k < 3; ++k) {
            const unsigned v = tris[t * 3 + size_t(k)];
            const Vec3 q = mesh.position(v);
            p.lo = Vec3(std::min(p.lo.x(), q.x()), std::min(p.lo.y(), q.y()), std::min(p.lo.z(), q.z()));
            p.hi = Vec3(std::max(p.hi.x(), q.x()), std::max(p.hi.y(), q.y()), std::max(p.hi.z(), q.z()));
            if (std::binary_search(lockedWelds.begin(), lockedWelds.end(), weld[v])) p.locked = true;
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
