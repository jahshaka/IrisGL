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
#include <climits>
#include <unordered_map>

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
                        const std::vector<unsigned> &inTris, const std::vector<unsigned> &externalWelds, float ask,
                        float floorLen, size_t targetTris, const Vec3 &boxLo, const Vec3 &boxHi, bool cardsAllowed)
{
    GroupResult r;
    r.scratch = makeScratch(mesh, inTris);
    const GroupScratch &sc = r.scratch;
    r.space = makeSpace(sc, weld, islands.compOf, islands.capOf, islands.count());
    const size_t n = sc.size();
    const size_t inCount = inTris.size() / 3;
    const size_t target = targetTris;
    if (inCount <= target) {   // merged as it is: no simplification, no estimate
        r.accepted = true;
        r.budget = std::max(ask, floorLen);
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

    // THE BUDGET: the largest ask; a group of leaves never granted one asks 2 x the position quadric's estimate.
    if (!(ask > 0.0f)) r.depth0Estimate = positionEstimate(sc, lock, target);
    r.budget = std::max(ask > 0.0f ? ask : kLadderFactor * std::max(r.depth0Estimate, 0.0f), floorLen);

    // D9: A CARD GROUP MERGES ITS CARDS (cards.h), then the rest of its input — the branches, the cards no set took —
    // is reduced the ordinary way under the SAME budget (its own target); the step is the larger of the two. The
    // corners join the local space for the re-split.
    if (cardsAllowed && isCardGroup(sc, r.space, islands.triangles)) {
        r.cards = true;
        const Clock::time_point tc = Clock::now();
        r.merge = mergeCards(sc, r.space, lock, r.budget, floorLen, boxLo, boxHi);
        r.msMeasure += msSince(tc);
        r.attempts = r.merge.pricings;
        r.estimate = 0.0f;
        std::vector<unsigned> out;        // local ids
        std::vector<unsigned> restLocal;  // the input the sets did not take
        const std::vector<unsigned> &mo = r.merge.sets > 0 ? r.merge.out : sc.tris;
        for (size_t t = 0; t < mo.size() / 3; ++t) {
            const bool corner = mo[t * 3] >= n || mo[t * 3 + 1] >= n || mo[t * 3 + 2] >= n;
            (corner ? out : restLocal).insert(corner ? out.end() : restLocal.end(), { mo[t * 3], mo[t * 3 + 1], mo[t * 3 + 2] });
        }
        float step = r.merge.sets > 0 ? r.merge.step : 0.0f;
        bool reduced = r.merge.sets > 0;
        if (!restLocal.empty()) {
            std::vector<unsigned> restGlobal(restLocal.size());
            for (size_t i = 0; i < restLocal.size(); ++i) restGlobal[i] = sc.globalOf[restLocal[i]];
            // its target: its own parent count, and never more than what the group's target leaves beside the quads
            // (the builder's shrink lowers the group's target when the output would not need fewer clusters)
            const size_t quadTris = out.size() / 3;
            const size_t restTarget = std::min(parentTarget(restGlobal.size() / 3),
                                               target > quadTris + 1 ? target - quadTris : size_t(1));
            const GroupResult rest = reduceGroup(mesh, weld, islands, restGlobal, externalWelds, r.budget, floorLen,
                                                 restTarget, boxLo, boxHi, false);
            r.attempts += rest.attempts;
            r.msSimplify += rest.msSimplify; r.msMeasure += rest.msMeasure; r.msExact += rest.msExact;
            r.pruned = rest.pruned;
            if (rest.accepted && rest.simplified) {
                // its output back in this group's scratch ids (its vertices are this group's)
                std::unordered_map<unsigned, unsigned> localOf;
                localOf.reserve(n);
                for (size_t v = 0; v < n; ++v) localOf.emplace(sc.globalOf[v], unsigned(v));
                for (unsigned v : rest.out) out.push_back(localOf.at(rest.scratch.globalOf[v]));
                step = std::max(step, rest.step);
                r.terms = rest.terms;
                reduced = true;
            } else {
                out.insert(out.end(), restLocal.begin(), restLocal.end());
            }
        }
        if (reduced && out.size() < sc.tris.size()) {
            r.accepted = true;
            r.out = std::move(out);
            r.step = std::max(step, floorLen);
            r.terms.step = r.step;
            for (const CardCorner &c : r.merge.corners) {
                r.space.pos.insert(r.space.pos.end(), { c.pos[0], c.pos[1], c.pos[2] });
                r.space.weld.push_back(UINT_MAX - 1u - unsigned(r.space.weld.size()));   // a new position, its own
                r.space.comp.push_back(UINT_MAX - 1u - unsigned((r.space.comp.size() - sc.size()) / 4));   // per quad
                r.space.cap.push_back(c.diagonal);
            }
        }
        return r;
    }

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
