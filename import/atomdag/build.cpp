/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "import/atomdag/build.h"

#include "import/atomdag/groups.h"
#include "import/atomdag/ladder.h"
#include "import/atomdag/leaves.h"
#include "import/bakeshared.h"

#include "meshoptimizer.h"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>

namespace iris
{
namespace atomdag
{
namespace
{

/// The arithmetic floor of a measured length: 1e-5 of the mesh's largest axis (meshbake.cpp's kBoundFloorRel —
/// a distance between two float surfaces finer than ~100 ULPs of the coordinate scale is noise).
constexpr float kFloorRel = 1e-5f;
/// The re-runs of a level where every group was carried (each doubles the budgets): 2^48 x the floor passes any extent.
constexpr int kMaxReruns = 48;

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

struct Sphere { Vec3 c; float r = 0.0f; };

/// The cluster under construction.
struct Cl
{
    std::vector<unsigned> tris;   ///< mesh vertex ids
    int refined = -1;
    int group = -1;
    float stored = 0.0f;          ///< its generating group's error (0 for a leaf)
    float carriedBudget = 0.0f;   ///< the largest budget it was carried at
    Vec3 lo, hi;                  ///< its box
    Sphere cull;                  ///< its own bounding sphere
    Sphere lod;                   ///< its generating group's LOD sphere (its own for a leaf)
    std::vector<quint32> region;  ///< wantRegions: level-0 triangles
};

void boundsOf(const MeshView &m, Cl &c)
{
    std::vector<unsigned> v(c.tris);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    std::vector<float> pts(v.size() * 3);
    c.lo = c.hi = m.position(v[0]);
    for (size_t i = 0; i < v.size(); ++i) {
        const Vec3 p = m.position(v[i]);
        pts[i * 3] = p.x(); pts[i * 3 + 1] = p.y(); pts[i * 3 + 2] = p.z();
        c.lo = Vec3(std::min(c.lo.x(), p.x()), std::min(c.lo.y(), p.y()), std::min(c.lo.z(), p.z()));
        c.hi = Vec3(std::max(c.hi.x(), p.x()), std::max(c.hi.y(), p.y()), std::max(c.hi.z(), p.z()));
    }
    const meshopt_Bounds b = meshopt_computeSphereBounds(pts.data(), v.size(), sizeof(float) * 3, nullptr, 0);
    c.cull.c = Vec3(b.center[0], b.center[1], b.center[2]);
    c.cull.r = b.radius;
    // the sphere must hold every vertex in float, exactly as the runtime evaluates it
    for (size_t i = 0; i < v.size(); ++i) c.cull.r = std::max(c.cull.r, (m.position(v[i]) - c.cull.c).length());
}

/// D7: the sphere enclosing the members' LOD spheres; grown where float rounding leaves a member out (counted).
Sphere enclose(const std::vector<Sphere> &s, int *grown)
{
    std::vector<float> c(s.size() * 3), r(s.size());
    for (size_t i = 0; i < s.size(); ++i) { c[i * 3] = s[i].c.x(); c[i * 3 + 1] = s[i].c.y(); c[i * 3 + 2] = s[i].c.z(); r[i] = s[i].r; }
    const meshopt_Bounds b = meshopt_computeSphereBounds(c.data(), s.size(), sizeof(float) * 3, r.data(), sizeof(float));
    Sphere out { Vec3(b.center[0], b.center[1], b.center[2]), b.radius };
    bool g = false;
    for (const Sphere &m : s) {
        const float need = (m.c - out.c).length() + m.r;
        if (out.r < need) { out.r = need; g = true; }
    }
    if (g) { out.r *= 1.0f + 1e-6f; if (grown) ++*grown; }
    return out;
}

/// The parent clusters of an accepted group's output (D4's re-split: the leaves' bisection on the output).
struct Split
{
    std::vector<std::vector<unsigned>> parts;   ///< per parent: triangle indices into the output
};

}   // namespace

QVector<float> configRecord()
{
    return QVector<float>{
        2.0f,   // the builder: atomdag (1 was clusterlod's record shape)
        float(kClusterTriangles), float(kClusterVertices), float(kClusterTarget), float(kSharedEdgeWeight),
        float(kLocalityWeight), float(kGroupMin), float(kGroupMax), float(kGroupTarget), float(kGroupEdgeWeight),
        float(kGroupSiblingWeight), float(kGroupEdgeBase), kLadderFactor, float(kRetries), kAcceptRatio,
        float(kMaxLevels), 4.0f /* Nanite's position scale 1/0.25 */, 128.0f /* Nanite's UV divisor */, kFloorRel };
}

bool build(const MeshView &mesh, const std::vector<unsigned> &base, MeshClusterDag *out, Stats *stats,
           const Options &options)
{
    *out = MeshClusterDag();
    const Clock::time_point t0 = Clock::now();
    const size_t baseTris = base.size() / 3;
    if (baseTris == 0 || !mesh.positions || mesh.nv == 0) return false;
    const float extent = meshopt_simplifyScale(mesh.positions, mesh.nv, sizeof(float) * size_t(mesh.posComps));
    if (!(extent > 0.0f) || !std::isfinite(extent)) return false;
    const float floorLen = extent * kFloorRel;

    const std::vector<unsigned> weld = weldByPosition(mesh);
    const Islands islands = findIslands(mesh.positions, mesh.posComps, mesh.nv, base);
    const auto islandsOf = [&](const std::vector<unsigned> &tris) {
        std::vector<unsigned> is(tris.size() / 3);
        for (size_t t = 0; t < is.size(); ++t) is[t] = islands.compOf[tris[t * 3]];
        return is;
    };

    // D2: THE LEAVES.
    std::vector<Cl> cl;
    {
        const Clock::time_point tl = Clock::now();
        const std::vector<std::vector<unsigned>> parts = buildClusters(mesh, weld, base, islandsOf(base));
        cl.resize(parts.size());
        for (size_t c = 0; c < parts.size(); ++c) {
            Cl &x = cl[c];
            for (unsigned t : parts[c]) {
                x.tris.insert(x.tris.end(), { base[size_t(t) * 3], base[size_t(t) * 3 + 1], base[size_t(t) * 3 + 2] });
                if (options.wantRegions) x.region.push_back(quint32(t));
            }
            boundsOf(mesh, x);
            x.lod = x.cull;
        }
        if (stats) { stats->msLeaves = msSince(tl); stats->leaves = int(cl.size()); }
    }

    std::vector<MeshClusterDag::Group> groups;
    std::vector<int> pool(cl.size());
    for (size_t c = 0; c < cl.size(); ++c) pool[c] = int(c);
    int level = 0;
    const int width = bakepool::width();
    while (pool.size() > 1) {
        if (level >= kMaxLevels) {
            if (stats) stats->depthCapHit = true;
            *out = MeshClusterDag();
            return false;
        }
        // D3: THE GROUPS of this level.
        const Clock::time_point tg = Clock::now();
        std::vector<PoolView> view(pool.size());
        for (size_t i = 0; i < pool.size(); ++i) {
            const Cl &c = cl[size_t(pool[i])];
            view[i].tris = &c.tris;
            view[i].refined = c.refined;
            view[i].boxCentre = (c.lo + c.hi) * 0.5f;
        }
        const Grouping grouping = groupPool(view, weld);
        if (stats) stats->msGroups += msSince(tg);

        // D4 + D6: every group reduced on the bake's threads, each into its own slot.
        const size_t ng = grouping.groups.size();
        std::vector<GroupResult> results(ng);
        std::vector<Split> splits(ng);
        std::vector<float> childStored(ng, 0.0f), childError(ng, 0.0f);
        std::vector<double> splitMs(ng, 0.0);
        const Clock::time_point tr = Clock::now();
        // A LEVEL WHERE EVERY GROUP IS CARRIED is not emitted: the pool and therefore its grouping would be the same
        // at the next level, so the level re-runs here with every member's budget doubled (the ladder's carry,
        // without an identical level in the DAG) — until some group reduces.
        for (int rerun = 0;; ++rerun) {
            std::fill(childStored.begin(), childStored.end(), 0.0f);
            std::fill(childError.begin(), childError.end(), 0.0f);
            for (Split &sp : splits) sp.parts.clear();
            bakepool::forRange(bakepool::Range(ng, 1), width, [&](size_t, size_t begin, size_t end, int) {
                for (size_t g = begin; g < end; ++g) {
                    std::vector<unsigned> in;
                    for (unsigned p : grouping.groups[g]) {
                        const Cl &c = cl[size_t(pool[p])];
                        in.insert(in.end(), c.tris.begin(), c.tris.end());
                        childStored[g] = std::max(childStored[g], c.stored);
                        childError[g] = std::max(childError[g], std::max(c.stored, c.carriedBudget));
                    }
                    // THE TARGET: Nanite's parent count — and a group already at it that would not merge into FEWER
                    // clusters (the vertex cap: 126 loose triangles are 378 vertices, two clusters again) halves instead,
                    // or the level would repeat itself for ever (Nanite's answer is the same: a split that fails retries
                    // at a smaller target, dossier 1.5).
                    const size_t inCount = in.size() / 3;
                    size_t target = parentTarget(inCount);
                    if (inCount <= target) {
                        splits[g].parts = buildClusters(mesh, weld, in, islandsOf(in));
                        if (splits[g].parts.size() >= grouping.groups[g].size()) {
                            splits[g].parts.clear();
                            target = std::max<size_t>(1, inCount / 2);
                        }
                    }
                    results[g] = reduceGroup(mesh, weld, islands, in, grouping.external[g], childError[g], floorLen,
                                             options.attributes, target);
                    if (results[g].accepted && !results[g].simplified) {
                        // not simplified: its output is its input, already split above
                    } else if (results[g].accepted && !results[g].out.empty()) {
                        const Clock::time_point ts = Clock::now();
                        splits[g].parts = buildClusters(mesh, weld, results[g].out, islandsOf(results[g].out));
                        splitMs[g] = msSince(ts);
                    }
                }
            });
            bool any = false;
            for (const GroupResult &r : results) any = any || r.accepted;
            if (any || rerun >= kMaxReruns) break;
            if (stats) ++stats->levelsAllCarried;
            for (size_t g = 0; g < ng; ++g)
                for (unsigned p : grouping.groups[g]) {
                    Cl &c = cl[size_t(pool[p])];
                    c.carriedBudget = std::max(c.carriedBudget, results[g].budget);
                }
        }
        if (stats) {
            if (ng <= 2) stats->msFewGroups += msSince(tr);
            stats->msReduce += msSince(tr);
            for (double ms : splitMs) stats->msSplit += ms;
            for (const GroupResult &r : results) { stats->msSimplify += r.msSimplify; stats->msMeasure += r.msMeasure; stats->msExact += r.msExact; }
        }

        // The level's outcome, in group order (deterministic ids).
        std::vector<int> next;
        for (size_t g = 0; g < ng; ++g) {
            const GroupResult &r = results[g];
            GroupRecord rec;
            rec.level = level;
            rec.members = int(grouping.groups[g].size());
            rec.inTris = 0;
            for (unsigned p : grouping.groups[g]) rec.inTris += int(cl[size_t(pool[p])].tris.size() / 3);
            rec.childError = childError[g];
            rec.budget = r.budget;
            rec.step = r.step;
            rec.estimate = r.estimate;
            rec.depth0Estimate = r.depth0Estimate;
            rec.attempts = r.attempts;
            rec.pruned = r.pruned;
            rec.removedVertex = r.terms.removedVertex;
            rec.lostFacet = r.terms.lostFacet;
            rec.addedFacet = r.terms.addedFacet;
            rec.droppedIsland = r.terms.droppedIsland;
            rec.lostLocked = r.terms.lostLocked;
            rec.islandsDropped = r.terms.islandsDropped;
            rec.lockedLost = r.terms.lockedLost;
            rec.weightNormal = r.weights.normal;
            rec.weightUv = r.weights.uv;
            if (options.wantLocks) {
                rec.external = grouping.external[g];
                for (unsigned p : grouping.groups[g]) rec.memberClusters.push_back(pool[p]);
            }
            if (stats) {
                stats->retries += std::max(0, r.attempts - 1);
                stats->prunedPieces += r.pruned;
            }
            if (!r.accepted) {
                rec.kind = 1;
                for (unsigned p : grouping.groups[g]) {
                    Cl &c = cl[size_t(pool[p])];
                    c.carriedBudget = std::max(c.carriedBudget, r.budget);
                    next.push_back(pool[p]);
                }
                if (stats) { ++stats->carried; if (options.wantGroups) stats->records.push_back(std::move(rec)); }
                continue;
            }
            // THE DAG GROUP (D7).
            const int id = int(groups.size());
            MeshClusterDag::Group G;
            G.depth = level;
            G.error = std::max(std::max(childStored[g], r.step), floorLen);
            G.estimate = r.estimate;
            std::vector<Sphere> lods;
            for (unsigned p : grouping.groups[g]) {
                Cl &c = cl[size_t(pool[p])];
                c.group = id;
                lods.push_back(c.lod);
            }
            const Sphere lod = enclose(lods, stats ? &stats->sphereGrown : nullptr);
            for (int k = 0; k < 3; ++k) G.centre[k] = k == 0 ? lod.c.x() : (k == 1 ? lod.c.y() : lod.c.z());
            G.radius = lod.r;
            groups.push_back(G);
            rec.kind = r.simplified ? 0 : 3;
            rec.dagGroup = id;
            rec.stored = G.error;
            rec.outTris = int(r.out.size() / 3);
            if (stats) { if (r.simplified) ++stats->reductions; else ++stats->notSimplified; }

            // The parents.
            const size_t firstParent = cl.size();
            for (const std::vector<unsigned> &part : splits[g].parts) {
                Cl c;
                for (unsigned t : part)
                    c.tris.insert(c.tris.end(), { r.out[size_t(t) * 3], r.out[size_t(t) * 3 + 1], r.out[size_t(t) * 3 + 2] });
                c.refined = id;
                c.stored = G.error;
                boundsOf(mesh, c);
                c.lod = lod;
                cl.push_back(std::move(c));
                next.push_back(int(cl.size() - 1));
            }
            // THE PROVENANCE (wantRegions): the members' level-0 triangles partitioned over the parents, each to the
            // parent nearest its centroid (a partition per group is what makes "one cut covers every level-0
            // triangle once" countable).
            if (options.wantRegions && cl.size() > firstParent) {
                std::vector<unsigned> parentOf(r.out.size() / 3, 0u);
                for (size_t k = 0; k < splits[g].parts.size(); ++k)
                    for (unsigned t : splits[g].parts[k]) parentOf[t] = unsigned(k);
                Vec3 lo = mesh.position(r.out[0]), hi = lo;
                for (unsigned v : r.out) {
                    const Vec3 p = mesh.position(v);
                    lo = Vec3(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
                    hi = Vec3(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
                }
                surface::TriangleGrid grid;
                grid.build(mesh.positions, mesh.posComps, r.out, lo, hi, 32);
                surface::TriangleGrid::Query q;
                for (unsigned p : grouping.groups[g])
                    for (quint32 t : cl[size_t(pool[p])].region) {
                        const Vec3 c = (mesh.position(base[size_t(t) * 3]) + mesh.position(base[size_t(t) * 3 + 1]) +
                                        mesh.position(base[size_t(t) * 3 + 2])) * (1.0f / 3.0f);
                        unsigned tri = 0;
                        grid.closest(q, c, nullptr, nullptr, &tri);
                        cl[firstParent + parentOf[std::min<size_t>(tri, parentOf.size() - 1)]].region.push_back(t);
                    }
            }
            if (stats && options.wantGroups) stats->records.push_back(std::move(rec));
        }
        pool.swap(next);
        ++level;
    }

    // D8: THE EMPTY TOP over the last cluster.
    if (pool.size() == 1) {
        Cl &root = cl[size_t(pool[0])];
        const int id = int(groups.size());
        MeshClusterDag::Group G;
        G.depth = level;
        G.error = std::max(std::max(root.stored, (root.hi - root.lo).length()), floorLen);
        G.estimate = 0.0f;
        for (int k = 0; k < 3; ++k) G.centre[k] = k == 0 ? root.lod.c.x() : (k == 1 ? root.lod.c.y() : root.lod.c.z());
        G.radius = root.lod.r;
        root.group = id;
        groups.push_back(G);
        if (stats) {
            stats->rootError = G.error;
            stats->rootTriangles = int(root.tris.size() / 3);
            if (options.wantGroups) {
                GroupRecord rec;
                rec.level = level;
                rec.dagGroup = id;
                rec.kind = 2;
                rec.members = 1;
                rec.inTris = int(root.tris.size() / 3);
                rec.stored = G.error;
                rec.step = G.error;
                stats->records.push_back(std::move(rec));
            }
        }
    }

    // THE FORMAT: meshlet-local (per cluster its vertices in first-use order, three 8-bit locals per triangle).
    MeshClusterDag dag;
    dag.groups = QVector<MeshClusterDag::Group>(groups.begin(), groups.end());
    dag.clusters.resize(int(cl.size()));
    int maxDepth = 0;
    for (const MeshClusterDag::Group &g : groups) maxDepth = std::max(maxDepth, int(g.depth));
    for (size_t c = 0; c < cl.size(); ++c) {
        const Cl &x = cl[c];
        MeshClusterDag::Cluster &o = dag.clusters[int(c)];
        o.vertexOffset = quint32(dag.vertices.size());
        o.triangleOffset = quint32(dag.triangles.size() / 3);
        std::vector<unsigned> local;
        for (unsigned v : x.tris) {
            auto it = std::find(local.begin(), local.end(), v);
            const size_t l = size_t(it - local.begin());
            if (it == local.end()) local.push_back(v);
            dag.triangles.append(char(quint8(l)));
        }
        for (unsigned v : local) dag.vertices.append(quint32(v));
        o.vertexCount = quint16(local.size());
        o.triangleCount = quint16(x.tris.size() / 3);
        o.group = x.group;
        o.refined = x.refined;
        o.centre[0] = x.cull.c.x(); o.centre[1] = x.cull.c.y(); o.centre[2] = x.cull.c.z();
        o.radius = x.cull.r;
    }
    *out = dag;
    if (stats) {
        stats->clusters = int(cl.size());
        stats->groups = int(groups.size());
        stats->depth = maxDepth;
        stats->levels = level;
        stats->msTotal = msSince(t0);
        if (options.wantRegions) {
            stats->clusterRegions.resize(int(cl.size()));
            for (size_t c = 0; c < cl.size(); ++c)
                stats->clusterRegions[int(c)] = QVector<quint32>(cl[c].region.begin(), cl[c].region.end());
        }
    }
    return true;
}

}   // namespace atomdag
}   // namespace iris
