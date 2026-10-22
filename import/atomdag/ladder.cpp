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

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

/// Per island part of the input with a box diagonal past `budget`: its largest triangle (an index into the input).
std::vector<unsigned> islandsAtRisk(const LocalSpace &space, const std::vector<unsigned> &tris, float budget)
{
    struct Part { Vec3 lo, hi; float area = -1.0f; unsigned tri = 0; };
    std::vector<std::pair<unsigned, Part>> parts;   // sorted by island
    const auto at = [&](unsigned v) {
        return Vec3(space.pos[size_t(v) * 3], space.pos[size_t(v) * 3 + 1], space.pos[size_t(v) * 3 + 2]);
    };
    for (size_t t = 0; t < tris.size() / 3; ++t) {
        const unsigned c = space.comp[tris[t * 3]];
        auto it = std::lower_bound(parts.begin(), parts.end(), c,
                                   [](const std::pair<unsigned, Part> &p, unsigned k) { return p.first < k; });
        const Vec3 a = at(tris[t * 3]), b = at(tris[t * 3 + 1]), d = at(tris[t * 3 + 2]);
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
                        float childError, float childStored, float floorLen, bool attributes, size_t targetTris)
{
    GroupResult r;
    r.scratch = makeScratch(mesh, inTris, attributes);
    const GroupScratch &sc = r.scratch;
    r.space = makeSpace(sc, weld, islands.compOf, islands.capOf, islands.count());
    const size_t n = sc.size();
    const size_t inCount = inTris.size() / 3;
    const size_t target = targetTris;
    if (inCount <= target) {
        r.accepted = true;
        r.out = sc.tris;
        r.step = floorLen;
        return r;
    }
    r.simplified = true;

    // THE LOCKS, by position: a scratch vertex at a locked weld id.
    std::vector<unsigned> locks = externalWelds;
    std::vector<unsigned char> lock(n, 0u);
    const auto lockMask = [&]() {
        for (size_t v = 0; v < n; ++v)
            lock[v] = std::binary_search(locks.begin(), locks.end(), r.space.weld[v]) ? 1u : 0u;   // meshopt_SimplifyVertex_Lock
    };
    lockMask();

    // THE BUDGET.
    if (!(childStored > 0.0f)) r.depth0Estimate = positionEstimate(sc, lock, target);   // a group of leaves (D6)
    r.budget = kLadderFactor * (childError > 0.0f ? childError : std::max(r.depth0Estimate, 0.0f));
    r.budget = std::max(r.budget, floorLen);

    // per position: the input triangles at it (the fan a lock holds)
    std::vector<std::pair<unsigned, unsigned>> byWeld;   // (weld, input triangle)
    byWeld.reserve(sc.tris.size());
    for (size_t i = 0; i < sc.tris.size(); ++i) byWeld.push_back({ r.space.weld[sc.tris[i]], unsigned(i / 3) });
    std::sort(byWeld.begin(), byWeld.end());
    byWeld.erase(std::unique(byWeld.begin(), byWeld.end()), byWeld.end());

    bool haveBest = false;
    std::vector<unsigned> best;
    std::vector<unsigned> atRisk;   // the largest input triangle of every island part bigger than the budget (lazy)
    const auto attempt = [&](float guess, StepMeasure &m) {
        ++r.attempts;
        const Clock::time_point ta = Clock::now();
        Simplified s = simplifyGroup(sc, lock, target, guess);
        r.msSimplify += msSince(ta);
        const Clock::time_point tb = Clock::now();
        const int pruned = int(pruneUnlocked(r.space, locks, target, r.budget, s.tris));
        m = measureStep(r.space, sc.tris, s.tris, externalWelds, r.budget, floorLen, false);
        r.msMeasure += msSince(tb);
        const bool within = m.step <= r.budget && m.lockedLost == 0;
        if (within && (!haveBest || s.tris.size() < best.size())) {
            haveBest = true;
            best = std::move(s.tris);
            r.estimate = s.resultError;
            r.pruned = pruned;
            r.lockedPositions = locks.size();
        } else if (!haveBest) {
            r.terms = m; r.step = m.step; r.estimate = s.resultError; r.pruned = pruned; r.lockedPositions = locks.size();
        }
        return within;
    };

    // THE ATTEMPTS, RAISED: E_k is the first guess (meshoptimizer compares it to its MIXED position + attribute error,
    // so a faceted or seamed group stops early under it); a result within the budget that is not at the target is
    // simplified again with the guess raised — 8 E_k, then unbounded — so the smaller count is always tried; a result
    // PAST the budget locks its offenders' fans (and every island at risk) and is simplified again at the same guess,
    // at most kRetries lock rounds. The group keeps the MOST REDUCED result that came within its budget.
    // (MEASURED, stage 2: trying the unbounded target first and locking ITS offenders over-constrains every later
    // attempt — the hard 7 at E = 16 drew 52,346 that way against 37,986 raised.)
    StepMeasure m;
    int raises = 0, lockRounds = 0;
    float guess = r.budget;
    for (;;) {
        if (attempt(guess, m)) {
            if (best.size() / 3 <= target || raises >= 2) break;
            ++raises;
            guess = raises < 2 ? guess * 8.0f : FLT_MAX;
            continue;
        }
        if (lockRounds >= kRetries) break;
        ++lockRounds;
        // THE LOCKS: every offender position's fan, every too-big island's largest triangle — EVERY ISLAND AT RISK AT
        // ONCE once one was dropped (the old verify's lesson, DAG-LOCK-1: held one at a time, a debris field is
        // whack-a-mole).
        std::vector<unsigned> add;
        for (unsigned w : m.fanWelds) {
            auto it = std::lower_bound(byWeld.begin(), byWeld.end(), std::make_pair(w, 0u));
            for (; it != byWeld.end() && it->first == w; ++it)
                for (int k = 0; k < 3; ++k) add.push_back(r.space.weld[sc.tris[size_t(it->second) * 3 + size_t(k)]]);
        }
        if (!m.keepTris.empty()) {
            if (atRisk.empty()) atRisk = islandsAtRisk(r.space, sc.tris, r.budget);
            for (unsigned t : atRisk)
                for (int k = 0; k < 3; ++k) add.push_back(r.space.weld[sc.tris[size_t(t) * 3 + size_t(k)]]);
        }
        for (unsigned t : m.keepTris)
            for (int k = 0; k < 3; ++k) add.push_back(r.space.weld[sc.tris[size_t(t) * 3 + size_t(k)]]);
        const size_t before = locks.size();
        locks.insert(locks.end(), add.begin(), add.end());
        std::sort(locks.begin(), locks.end());
        locks.erase(std::unique(locks.begin(), locks.end()), locks.end());
        if (locks.size() == before) break;   // nothing new to hold: another attempt would be the same
        lockMask();
    }
    if (haveBest) {   // THE KEPT RESULT, measured exactly (its stored step)
        const Clock::time_point tm = Clock::now();
        r.terms = measureStep(r.space, sc.tris, best, externalWelds, r.budget, floorLen, true);
        // (the decision proved every term within the budget; the exact walk's upper bound may sit up to its 5 %
        // tolerance above the true maximum, so the budget is the tighter bound when it is lower)
        r.step = std::min(r.terms.step, r.budget);
        r.msMeasure += msSince(tm);
        r.msExact += msSince(tm);
    }
    // THE BUDGET IS THE ACCEPTANCE: the most reduced result within it (the builder carries one that needs as many
    // clusters as the input).
    if (haveBest && best.size() < sc.tris.size()) {
        r.accepted = true;
        r.out = std::move(best);
    }
    return r;
}

}   // namespace atomdag
}   // namespace iris
