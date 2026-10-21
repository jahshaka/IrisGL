/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/ladder.h"

#include <algorithm>
#include <cfloat>
#include <chrono>

namespace iris
{
namespace atomdag
{

namespace
{
/// Per island part of the input with a box diagonal past `budget`: its largest triangle (an index into the input).
std::vector<unsigned> islandsAtRisk(const MeshView &mesh, const Islands &islands, const std::vector<unsigned> &inTris,
                                    float budget)
{
    struct Part { Vec3 lo, hi; float area = -1.0f; unsigned tri = 0; };
    std::vector<std::pair<unsigned, Part>> parts;   // sorted by island
    for (size_t t = 0; t < inTris.size() / 3; ++t) {
        const unsigned c = islands.compOf[inTris[t * 3]];
        auto it = std::lower_bound(parts.begin(), parts.end(), c,
                                   [](const std::pair<unsigned, Part> &p, unsigned k) { return p.first < k; });
        const Vec3 a = mesh.position(inTris[t * 3]), b = mesh.position(inTris[t * 3 + 1]), d = mesh.position(inTris[t * 3 + 2]);
        if (it == parts.end() || it->first != c) it = parts.insert(it, { c, Part { a, a, -1.0f, 0 } });
        Part &p = it->second;
        for (const Vec3 *x : { &a, &b, &d }) {
            p.lo = Vec3(std::min(p.lo.x(), x->x()), std::min(p.lo.y(), x->y()), std::min(p.lo.z(), x->z()));
            p.hi = Vec3(std::max(p.hi.x(), x->x()), std::max(p.hi.y(), x->y()), std::max(p.hi.z(), x->z()));
        }
        const float area = Vec3::crossProduct(b - a, d - a).length();
        if (area > p.area) { p.area = area; p.tri = unsigned(t); }
    }
    std::vector<unsigned> out;
    for (const auto &p : parts) if ((p.second.hi - p.second.lo).length() > budget) out.push_back(p.second.tri);
    return out;
}
}   // namespace

GroupResult reduceGroup(const MeshView &mesh, const std::vector<unsigned> &weld, const Islands &islands,
                        const std::vector<unsigned> &inTris, const std::vector<unsigned> &externalWelds,
                        float childError, float floorLen, bool attributes, size_t targetTris)
{
    GroupResult r;
    const size_t inCount = inTris.size() / 3;
    const size_t target = targetTris;
    if (inCount <= target) {
        r.accepted = true;
        r.out = inTris;
        r.step = floorLen;
        return r;
    }
    r.simplified = true;
    r.weights = groupWeights(mesh, inTris);
    // THE BUDGET.
    if (childError > 0.0f) {
        r.budget = kLadderFactor * childError;
    } else {
        const Simplified est = simplifyGroup(mesh, weld, inTris, externalWelds, target, FLT_MAX, false, r.weights);
        r.depth0Estimate = est.resultError;
        r.budget = kLadderFactor * est.resultError;
    }
    r.budget = std::max(r.budget, floorLen);

    std::vector<unsigned> locks = externalWelds;
    // per position: the input triangles at it (the fan a lock holds)
    std::vector<std::pair<unsigned, unsigned>> byWeld;   // (weld, input triangle)
    byWeld.reserve(inTris.size());
    for (size_t i = 0; i < inTris.size(); ++i) byWeld.push_back({ weld[inTris[i]], unsigned(i / 3) });
    std::sort(byWeld.begin(), byWeld.end());
    byWeld.erase(std::unique(byWeld.begin(), byWeld.end()), byWeld.end());

    // THE ATTEMPTS. meshoptimizer's own error limit compares E_k with its MIXED collapse error (position + attribute):
    // as a first guess it stops a faceted or seamed group long before its geometry reaches E_k. So the guess is RAISED
    // (x8, then unbounded) while the measured step stays within the budget and the target is not reached; a result
    // PAST the budget locks its offenders and is simplified again at the same guess (at most kRetries lock rounds).
    // The group keeps the most reduced result that was within its budget — only the measurement judges. Every attempt
    // is DECIDED (each query stops at the budget); the kept one is then measured exactly for its stored step.
    constexpr int kRaises = 2;
    float guess = r.budget;
    int lockRounds = 0, raises = 0;
    bool haveBest = false;
    std::vector<unsigned> best;
    std::vector<unsigned> atRisk;   // the largest input triangle of every island part bigger than the budget (lazy)
    for (int attempt = 0;; ++attempt) {
        r.attempts = attempt + 1;
        const auto ta = std::chrono::steady_clock::now();
        Simplified s = simplifyGroup(mesh, weld, inTris, locks, target, guess, attributes, r.weights);
        const auto tb = std::chrono::steady_clock::now();
        const int pruned = int(pruneUnlocked(mesh, weld, locks, target, r.budget, s.tris));
        const StepMeasure m = measureStep(mesh, weld, islands, inTris, s.tris, externalWelds, r.budget, floorLen, false);
        r.msSimplify += std::chrono::duration<double, std::milli>(tb - ta).count();
        r.msMeasure += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb).count();
        const bool within = m.step <= r.budget && m.lockedLost == 0;
        if (within) {
            if (!haveBest || s.tris.size() < best.size()) {
                haveBest = true;
                best = s.tris;
                r.estimate = s.resultError;
                r.pruned = pruned;
                r.lockedPositions = locks.size();
            }
            if (s.tris.size() / 3 <= target || raises >= kRaises) break;
            ++raises;
            guess = raises < kRaises ? guess * 8.0f : FLT_MAX;
            continue;
        }
        if (!haveBest) { r.terms = m; r.step = m.step; r.estimate = s.resultError; r.pruned = pruned; r.lockedPositions = locks.size(); }
        if (lockRounds >= kRetries) break;
        ++lockRounds;
        // THE LOCKS: every offender position's fan, every too-big island's largest triangle.
        std::vector<unsigned> add;
        for (unsigned w : m.fanWelds) {
            auto it = std::lower_bound(byWeld.begin(), byWeld.end(), std::make_pair(w, 0u));
            for (; it != byWeld.end() && it->first == w; ++it)
                for (int k = 0; k < 3; ++k) add.push_back(weld[inTris[size_t(it->second) * 3 + size_t(k)]]);
        }
        // EVERY ISLAND AT RISK AT ONCE (the old verify's lesson, DAG-LOCK-1): once one island too big to drop was
        // dropped, every island part of the input bigger than the budget keeps its largest triangle — held one at a
        // time, a debris field is whack-a-mole (the next attempt reaches its target by dropping the next island).
        if (!m.keepTris.empty()) {
            if (atRisk.empty()) atRisk = islandsAtRisk(mesh, islands, inTris, r.budget);
            for (unsigned t : atRisk)
                for (int k = 0; k < 3; ++k) add.push_back(weld[inTris[size_t(t) * 3 + size_t(k)]]);
        }
        for (unsigned t : m.keepTris)
            for (int k = 0; k < 3; ++k) add.push_back(weld[inTris[size_t(t) * 3 + size_t(k)]]);
        const size_t before = locks.size();
        locks.insert(locks.end(), add.begin(), add.end());
        std::sort(locks.begin(), locks.end());
        locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
        if (locks.size() == before) break;   // nothing new to hold: another attempt would be the same
    }
    if (haveBest) {   // THE KEPT RESULT, measured exactly (its stored step)
        const auto tm = std::chrono::steady_clock::now();
        r.terms = measureStep(mesh, weld, islands, inTris, best, externalWelds, r.budget, floorLen, true);
        // (the decision proved every term within the budget; the exact walk's upper bound may sit up to its 5 %
        // tolerance above the true maximum, so the budget is the tighter bound when it is lower)
        r.step = std::min(r.terms.step, r.budget);
        r.msMeasure += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tm).count();
        r.msExact += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tm).count();
    }
    // A LEVEL: the target reached, or at least 15 % shed (a small group's target can be above 0.85 x its input).
    if (haveBest && (best.size() / 3 <= target || float(best.size() / 3) <= kAcceptRatio * float(inCount))) {
        r.accepted = true;
        r.out = std::move(best);
    }
    return r;
}

}   // namespace atomdag
}   // namespace iris
