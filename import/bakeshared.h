/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_IMPORT_BAKESHARED_H
#define IRIS_IMPORT_BAKESHARED_H

// THE BAKE'S SHARED MACHINERY (ATOM-2 stage 1): the pool every bake stage runs its loops on, and the one surface
// sampler + nearest-surface grid every measurement reads. They lived inside meshbake.cpp's anonymous namespace; the
// cluster DAG's builder (import/atomdag/) is its own set of translation units and reads the same two, so they are one
// header now. Header-only (inline): one definition, every bake TU. A BAKE PRODUCER (irisgl/CMakeLists.txt's hashed
// list): what these compute is written into every .jmb.

#include "core/math/vec.h"
#include "core/threadbudget.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <pthread.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace iris
{

// ---- THE BAKE'S THREADS (IMPORT-SPEED-1) ----------------------------------
//
// The bake is ~all nearest-surface queries (the LOD bound's samples, removed
// vertices and facets; the cluster DAG's per-group measurement; the SDF's exact
// band), and every query is independent of every other: it READS a grid and
// writes one number. So the bake runs them on every hardware thread — and its
// OUTPUT stays a function of the model alone, byte for byte, for three reasons
// that each hold by construction:
//
//   1. THE PARTITION IS THE INPUT'S, NEVER THE WIDTH'S. `Range` cuts [0, n) into
//      chunks whose size depends on n (and a grain) only; one thread or twenty
//      walk the same chunks, in whatever order the pool hands them out.
//   2. EVERY REDUCTION IS ORDERED. A chunk writes its result into ITS OWN slot of
//      a per-chunk array; the caller folds the array in chunk order after the
//      pool returns — a maximum (order-free on floats anyway, and taken with the
//      serial loop's strict `>` so the first arg-max wins), a list (concatenated
//      in chunk order = the serial loop's order), a count (a sum of integers).
//   3. THE GRID IS READ-ONLY. A query's scratch (the triangle stamps, the tie set)
//      is a `TriangleGrid::Query` owned by the calling thread's SLOT, never by the
//      grid; the answer of one query does not depend on which queries ran before
//      it on the same scratch.
//
// THE POOL IS ONE, PROCESS-WIDE, THE BACKGROUND TIER'S WIDTH (the memory law: no
// unbounded fan-out; THREADING-1: the ONE core budget, core/threadbudget.h —
// it used to spawn hardware - 1 = 19 workers no budget counted). Its workers are created
// once, with the pool, and live for the process. The CALLER
// ALWAYS WORKS ITS OWN JOB, so a job started from inside another job's chunk
// (the meshes of a model run concurrently, and each mesh's stages call the pool
// again) finishes even if every worker is busy: nesting cannot deadlock and never
// adds a thread.
namespace bakepool {

inline std::atomic<int> gWidth { 0 };   ///< MeshBake::setBakeThreads; 0 = kDefaultWidth

/// THE DEFAULT WIDTH, MEASURED (BAKE-WIDTH-2, spikes/atom-engine-1/fix10-width-series.txt):
/// the editor's frames while the greek temple scan bakes, workers at idle priority, the
/// app at nice 0 on the 20-thread box, two runs per width. Frames over 33 / 50 ms during
/// the bake: 20 threads 39/31 and 37/27, 8 threads 34/25 and 39/30, 6 and 5 threads p95
/// still 66-84 ms, 4 threads 11/3 and 11/2 (p95 28-30 ms against 73-87). The import's
/// wall: 5.3-5.4 s at 5-20 threads (the serial verify chain sets it), 6.3 s at 4. Idle
/// priority alone does not keep the render loop whole above 4 — so 4, capped at the
/// hardware: a hitch-free editor beats 0.9 s of a background bake.
constexpr int kDefaultWidth = 4;

inline int hardware()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n > 0 ? int(n) : 1;
}

/// The width a bake started now uses: the configured count (else kDefaultWidth),
/// capped at the hardware.
inline int width()
{
    const int w = gWidth.load(std::memory_order_relaxed);
    return std::min(w > 0 ? w : kDefaultWidth, hardware());
}

/// [0, n) in chunks of a size that depends on `n` and `grain` alone (rule 1): at
/// most ~256 chunks, none smaller than `grain` items.
struct Range
{
    size_t n = 0, size = 1, count = 0;
    Range(size_t items, size_t grain)
        : n(items), size(std::max<size_t>(std::max<size_t>(grain, 1), (items + 255) / 256)),
          count((items + size - 1) / size) {}
    size_t begin(size_t c) const { return c * size; }
    size_t end(size_t c) const { return std::min(n, (c + 1) * size); }
};

class Pool
{
public:
    static Pool &instance()
    {
        // Leaked on purpose: detached workers may still be parked on the condition
        // variable when static destructors run, and a pool that outlives them is
        // the only order that is always safe.
        static Pool *pool = new Pool;
        return *pool;
    }

    /// The workers, all of them started here, once: the background tier's width.
    Pool()
    {
        const int workers = iris::threadBudget().background;
        std::lock_guard<std::mutex> lock(mMutex);
        for (int i = 0; i < workers; ++i) {
            std::thread([this] { worker(); }).detach();
            ++mThreads;
        }
    }

    int threads() const { return int(mThreads); }

    /// Runs body(chunk, slot) for every chunk in [0, chunks), on at most `width`
    /// threads (the caller is one of them), and returns when all are done. `slot`
    /// is in [0, width) and unique among the threads working THIS job at once —
    /// the index of the caller's per-thread scratch.
    ///
    /// AN EXCEPTION IN ANY CHUNK (a bad_alloc on a huge asset, anything a stage
    /// throws) behaves as it does in a serial loop: the job stops handing out
    /// chunks, the chunks already running finish, and the FIRST exception is
    /// rethrown here, on the caller — never on a pool thread (where it would
    /// std::terminate the app), and never with this job still listed (the Job
    /// lives on this frame: a listed dangling Job is what a worker would read
    /// next). MeshBake::buildFromScene turns it into "no bake" and the pool
    /// serves the next bake as if nothing happened.
    void run(size_t chunks, int width, const std::function<void(size_t, int)> &body)
    {
        if (chunks == 0) return;
        if (width <= 1 || chunks == 1) {
            for (size_t c = 0; c < chunks; ++c) runChunk(body, c, 0);
            return;
        }
        Job job;
        job.body = &body;
        job.chunks = chunks;
        job.maxSlots = width;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            mJobs.push_back(&job);
        }
        {
            // Unlisted and quiescent before this frame can end, however it ends.
            struct Leave
            {
                Pool &pool;
                Job &job;
                ~Leave()
                {
                    std::unique_lock<std::mutex> lock(pool.mMutex);
                    pool.mJobs.erase(std::find(pool.mJobs.begin(), pool.mJobs.end(), &job));
                    pool.mIdle.wait(lock, [&] { return job.active == 0; });
                }
            } leave { *this, job };
            mWake.notify_all();
            work(job, 0);
        }
        if (job.error) std::rethrow_exception(job.error);
    }

    /// TEST HOOK (bake.determinism's failure half): the n-th chunk run from now
    /// on, on whatever thread, throws std::runtime_error; 0 disarms it. Counts
    /// the serial path too, so a width-1 bake fails the same way.
    static void failAfterChunks(int n) { sFailCountdown.store(n); }

private:
    struct Job
    {
        const std::function<void(size_t, int)> *body = nullptr;
        size_t chunks = 0;
        int maxSlots = 1;
        std::atomic<size_t> next { 0 };
        int joined = 1;   ///< under mMutex; the caller holds slot 0
        int active = 0;   ///< under mMutex; workers inside work()
        std::exception_ptr error;   ///< under mMutex; the first chunk's that threw
    };

    static void runChunk(const std::function<void(size_t, int)> &body, size_t c, int slot)
    {
        if (sFailCountdown.load(std::memory_order_relaxed) > 0 && sFailCountdown.fetch_sub(1) == 1)
            throw std::runtime_error("mesh bake: injected failure (MeshBake::failBakeAfterChunksForTest)");
        body(c, slot);
    }

    void work(Job &job, int slot)
    {
        for (;;) {
            const size_t c = job.next.fetch_add(1);
            if (c >= job.chunks) return;
            try {
                runChunk(*job.body, c, slot);
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(mMutex);
                    if (!job.error) job.error = std::current_exception();
                }
                job.next.store(job.chunks);   // drain: no thread claims another chunk
                return;
            }
        }
    }

    static inline std::atomic<int> sFailCountdown { 0 };

    Job *joinable() const
    {
        for (Job *job : mJobs)
            if (job->joined < job->maxSlots && job->next.load() < job->chunks) return job;
        return nullptr;
    }

    void worker()
    {
        // AT IDLE PRIORITY (BAKE-WIDTH-2). A bake is background work the person did not
        // ask to watch; the render loop and the UI thread are what they are looking at.
        // D7 measured the editor's frame p95 at 2-5x idle while a temple baked on 20
        // threads at the default priority. The lowest NICE on Linux (per thread: a
        // weight of 15 against the render threads' 1024) — not SCHED_IDLE, which starves
        // outright under any other load (a gate's own nice-19 suites included) and would
        // turn every concurrent bake into a single-threaded one; the BACKGROUND QoS class
        // on macOS. The caller (slot 0) is the import's own worker and keeps its
        // priority: a bake always progresses.
#if defined(__linux__)
        setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 19);
#elif defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#endif
        std::unique_lock<std::mutex> lock(mMutex);
        for (;;) {
            Job *job = nullptr;
            mWake.wait(lock, [&] { return (job = joinable()) != nullptr; });
            const int slot = job->joined++;
            ++job->active;
            lock.unlock();
            work(*job, slot);
            lock.lock();
            if (--job->active == 0) mIdle.notify_all();
        }
    }

    std::mutex mMutex;
    std::condition_variable mWake, mIdle;
    std::deque<Job *> mJobs;
    size_t mThreads = 0;
};

/// The one parallel loop of the bake: body(chunk, begin, end, slot) over `range`,
/// on `width` threads. Callers size their per-slot scratch by the SAME `width`.
template <class Body>
void forRange(const Range &range, int width, Body &&body)
{
    const std::function<void(size_t, int)> f = [&](size_t c, int slot) {
        body(c, range.begin(c), range.end(c), slot);
    };
    Pool::instance().run(range.count, width, f);
}

}   // namespace bakepool

// ---- THE SURFACE: one sampler, one nearest-surface query -------------------
//
// ATOM-BAKE-1 (ATOM P1's AT-A5). Two products of the bake need to treat a mesh
// as a SURFACE rather than as a list of triangles — the card generator (which
// surfels it) and the LOD chain's honest error (which measures a distance
// between two of its levels) — and the surface sampler was written inside the
// card generator first. It lives HERE now, once, because a second stratified
// area sampler would be a second definition of "a point of this mesh".
namespace surface {

/// One sample of a triangle soup: a point on it, and the GEOMETRIC normal of
/// the triangle it came from (the shading normal can point somewhere the
/// surface does not face, and both consumers here raster or measure geometry).
struct Sample
{
    Vec3 pos;
    Vec3 nrm;
    unsigned tri = 0;   ///< the triangle (in the sampled soup's own order) it lies on
};

/// The van der Corput radical inverse — the low-discrepancy sequence that
/// replaces a random number generator here. `index` is the sample's own index,
/// so the k-th sample of a mesh is the k-th sample of that mesh forever: this
/// file contains no seed, which is stronger than "deterministic with a fixed
/// seed".
inline float radicalInverse(unsigned index, unsigned base)
{
    float result = 0.0f, f = 1.0f / float(base);
    while (index > 0) { result += f * float(index % base); index /= base; f /= float(base); }
    return result;
}

/// THE ONE STRATIFIED AREA SAMPLER.
///
/// What both consumers must cover is SURFACE, not vertices and not triangles: a
/// mesh with one enormous floor triangle and ten thousand tiny ones in a corner
/// would be sampled entirely in the corner by any per-triangle scheme. So the
/// triangle is chosen by a STRATIFIED walk of the cumulative-area array and the
/// barycentric coordinates come from a van der Corput pair.
///
/// Returns the soup's total area; fills `out` with AT MOST `count` samples (a
/// degenerate triangle contributes none — it has no facing, and a sample with no
/// normal is not a sample of a surface).
inline float sample(const float *positions, int posComps, const std::vector<unsigned> &indices,
             size_t count, std::vector<Sample> *out)
{
    out->clear();
    const size_t triCount = indices.size() / 3;
    if (triCount == 0 || count == 0 || !positions || posComps < 3) return 0.0f;

    const auto vertexAt = [&](unsigned i) {
        const float *v = positions + size_t(i) * size_t(posComps);
        return Vec3(v[0], v[1], v[2]);
    };

    std::vector<float> cumulative(triCount + 1, 0.0f);
    for (size_t t = 0; t < triCount; ++t) {
        const Vec3 a = vertexAt(indices[t * 3]);
        const Vec3 b = vertexAt(indices[t * 3 + 1]);
        const Vec3 c = vertexAt(indices[t * 3 + 2]);
        const float area = Vec3::crossProduct(b - a, c - a).length() * 0.5f;
        cumulative[t + 1] = cumulative[t] + (std::isfinite(area) ? area : 0.0f);
    }
    const float totalArea = cumulative.back();
    if (!(totalArea > 0.0f)) return 0.0f;

    out->reserve(count);
    for (size_t k = 0; k < count; ++k) {
        const float target = (float(k) + 0.5f) / float(count) * totalArea;
        const size_t t = size_t(std::upper_bound(cumulative.begin() + 1, cumulative.end(), target)
                                - cumulative.begin() - 1);
        if (t >= triCount) continue;
        const Vec3 a = vertexAt(indices[t * 3]);
        const Vec3 b = vertexAt(indices[t * 3 + 1]);
        const Vec3 c = vertexAt(indices[t * 3 + 2]);
        const Vec3 n = Vec3::crossProduct(b - a, c - a).normalized();
        if (n.lengthSquared() < 0.5f) continue;   // a degenerate triangle has no facing
        const float r1 = radicalInverse(unsigned(k) + 1u, 2u);
        const float r2 = radicalInverse(unsigned(k) + 1u, 3u);
        const float su = std::sqrt(r1);
        Sample s;
        s.pos = a * (1.0f - su) + b * (su * (1.0f - r2)) + c * (su * r2);
        s.nrm = n;
        s.tri = unsigned(t);
        out->push_back(s);
    }
    return totalArea;
}

/// The nearest point of one triangle to `p` (Ericson, Real-Time Collision
/// Detection §5.1.5 — the seven-region form, no square roots inside it).
inline Vec3 closestOnTriangle(const Vec3 &p, const Vec3 &a, const Vec3 &b, const Vec3 &c)
{
    const Vec3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = Vec3::dotProduct(ab, ap), d2 = Vec3::dotProduct(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const Vec3 bp = p - b;
    const float d3 = Vec3::dotProduct(ab, bp), d4 = Vec3::dotProduct(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        return a + ab * (std::fabs(denom) > 0.0f ? d1 / denom : 0.0f);
    }

    const Vec3 cp = p - c;
    const float d5 = Vec3::dotProduct(ab, cp), d6 = Vec3::dotProduct(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        return a + ac * (std::fabs(denom) > 0.0f ? d2 / denom : 0.0f);
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const float denom = (d4 - d3) + (d5 - d6);
        return b + (c - b) * (std::fabs(denom) > 0.0f ? (d4 - d3) / denom : 0.0f);
    }

    const float denom = va + vb + vc;
    if (!(std::fabs(denom) > 0.0f)) return a;
    return a + ab * (vb / denom) + ac * (vc / denom);
}

/// THE ANGLE THIS TRIANGLE SUBTENDS AT `q`, a point on it — the weight of the
/// ANGLE-WEIGHTED PSEUDONORMAL (Baerentzen & Aanaes, "Signed distance computation
/// using the angle weighted pseudonormal").
///
/// WHY A SIGNED DISTANCE FIELD CANNOT USE A FACE NORMAL. The sign of a point is
/// `dot(p - q, n)` where `n` is the surface normal AT `q`, and when `q` is on an
/// EDGE or a VERTEX there is no single face normal there — there are several, and
/// picking one of them is wrong for every point in the wedge between the others.
/// On a convex edge sharper than a right angle the error changes the SIGN: an
/// exterior point near a cone's tip, a pyramid's apex or a wedge's spine has its
/// nearest point on that feature, and the one face whose normal happens to be
/// picked can face away from it, so the point reads INSIDE. The angle-weighted sum
/// is the normal field whose sign is correct for every exterior point — that is
/// the theorem the paper proves, and it is why the weights are angles and not
/// areas.
///
///   * `q` interior to the face — one face, the whole 2*pi of directions: the face
///     normal, weight 2*pi.
///   * `q` on an edge — the two faces sharing it each subtend pi.
///   * `q` at a vertex — each incident face subtends its own INTERIOR ANGLE there,
///     which is the term that makes an asymmetric corner come out right.
inline float angleWeightAt(const Vec3 &q, const Vec3 &a, const Vec3 &b, const Vec3 &c, float eps)
{
    const float da = (q - a).length(), db = (q - b).length(), dc = (q - c).length();
    const auto interiorAngle = [](const Vec3 &at, const Vec3 &u, const Vec3 &v) {
        const Vec3 e1 = (u - at).normalized(), e2 = (v - at).normalized();
        const float d = std::clamp(Vec3::dotProduct(e1, e2), -1.0f, 1.0f);
        return std::acos(d);
    };
    if (da <= eps) return interiorAngle(a, b, c);
    if (db <= eps) return interiorAngle(b, a, c);
    if (dc <= eps) return interiorAngle(c, a, b);
    // On an edge? The distance from `q` to the line through that edge is zero, and
    // `q` is inside the segment (it is a closest point, so it cannot be outside).
    const auto onSegment = [&](const Vec3 &u, const Vec3 &v) {
        const Vec3 e = v - u;
        const float len2 = e.lengthSquared();
        if (!(len2 > 0.0f)) return false;
        const float s = std::clamp(Vec3::dotProduct(q - u, e) / len2, 0.0f, 1.0f);
        return ((u + e * s) - q).length() <= eps;
    };
    const float kPi = 3.14159265358979323846f;
    if (onSegment(a, b) || onSegment(b, c) || onSegment(c, a)) return kPi;
    return 2.0f * kPi;
}

/// A UNIFORM GRID OVER A TRIANGLE SOUP, and the EXACT nearest-surface query over
/// it. Never a brute-force loop: the bound measurement asks this thousands of
/// times per level and the SDF asks it per seeded cell.
///
/// Each triangle is filed in every cell its own AABB touches, so a query is a
/// shell walk outwards from the query point's cell that stops as soon as the
/// NEXT shell cannot hold anything closer than the best hit so far. That last
/// clause is what makes the answer EXACT and not approximate — it is the same
/// number the brute-force loop would return, found without visiting most of the
/// mesh. (A cell at Chebyshev ring distance s from the query point's own cell
/// cannot contain a point nearer than (s - 1) * cell, because the query point is
/// somewhere inside its own cell.)
class TriangleGrid
{
public:
    /// `res` is the ceiling on cells per axis; the cell is CUBIC (the largest
    /// axis divided by `res`) so the ring bound above is one length and not
    /// three.
    void build(const float *positions, int posComps, const std::vector<unsigned> &indices,
               const Vec3 &lo, const Vec3 &hi, int res)
    {
        mPositions = positions;
        mPosComps = posComps;
        mIndices = &indices;
        mLo = lo;
        mTriCount = indices.size() / 3;
        const Vec3 size = hi - lo;
        const float extent = std::max(std::max(size.x(), size.y()), size.z());
        mCell = extent > 0.0f ? extent / float(std::max(res, 1)) : 1.0f;
        for (int a = 0; a < 3; ++a) {
            const float s = a == 0 ? size.x() : (a == 1 ? size.y() : size.z());
            mDim[a] = std::clamp(int(std::floor(s / mCell)) + 1, 1, std::max(res, 1));
        }
        // THE CELLS ARE ONE FLAT LIST (CSR: `mStart` per cell into `mTris`), filed by
        // a counting pass and a fill pass over the triangles in index order — so each
        // cell lists its triangles in ascending order, exactly as appending to a
        // vector per cell did, and every query walks the same candidates in the same
        // order. What it saves is the vector per cell: 24 bytes for every EMPTY cell
        // and an allocation for every occupied one, on a grid rebuilt per level and
        // per lock pass, and one per cluster-DAG group on every pool thread at once
        // (IMPORT-SPEED-1: the per-cell vectors were most of the parallel bake's
        // peak memory).
        const size_t cells = size_t(mDim[0]) * size_t(mDim[1]) * size_t(mDim[2]);
        mStart.assign(cells + 1, 0u);
        const auto cellRange = [&](size_t t, int c0[3], int c1[3]) {
            Vec3 tlo = vertexAt((*mIndices)[t * 3]), thi = tlo;
            for (int k = 1; k < 3; ++k) {
                const Vec3 v = vertexAt((*mIndices)[t * 3 + size_t(k)]);
                tlo = Vec3(std::min(tlo.x(), v.x()), std::min(tlo.y(), v.y()), std::min(tlo.z(), v.z()));
                thi = Vec3(std::max(thi.x(), v.x()), std::max(thi.y(), v.y()), std::max(thi.z(), v.z()));
            }
            cellOf(tlo, c0);
            cellOf(thi, c1);
        };
        // A triangle filed in ONE cell is met at most once by any walk (a walk visits
        // each cell once), so only the ones filed in several need the query's
        // visited set: `mShared` marks them, and the set stays small and cold.
        mShared.assign(mTriCount, 0u);
        for (size_t t = 0; t < mTriCount; ++t) {
            int c0[3], c1[3];
            cellRange(t, c0, c1);
            mShared[t] = (c0[0] != c1[0] || c0[1] != c1[1] || c0[2] != c1[2]) ? 1u : 0u;
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x)
                        ++mStart[index(x, y, z) + 1];
        }
        for (size_t c = 0; c < cells; ++c) mStart[c + 1] += mStart[c];
        mTris.resize(mStart[cells]);
        std::vector<unsigned> fill(mStart.begin(), mStart.end() - 1);
        for (size_t t = 0; t < mTriCount; ++t) {
            int c0[3], c1[3];
            cellRange(t, c0, c1);
            for (int z = c0[2]; z <= c1[2]; ++z)
                for (int y = c0[1]; y <= c1[1]; ++y)
                    for (int x = c0[0]; x <= c1[0]; ++x)
                        mTris[fill[index(x, y, z)]++] = unsigned(t);
        }
    }

    bool empty() const { return mTriCount == 0; }

    /// One candidate nearest feature: where on the triangle, the triangle, and how
    /// far. Kept only while a caller asks for a normal.
    struct Tie { Vec3 q, a, b, c; float d = 0.0f; };

    /// A QUERY'S SCRATCH, owned by the thread asking — never by the grid, which is
    /// READ-ONLY once built, so any number of threads query one grid at once
    /// (IMPORT-SPEED-1; `bakepool`). The visited set dedupes a triangle filed in
    /// several cells within ONE query (a fresh generation per query), and the tie
    /// set is the pseudonormal's; neither carries anything from one query to the
    /// next, so an answer never depends on which queries the same scratch served
    /// before it.
    ///
    /// THE VISITED SET IS ONE BIT PER TRIANGLE (IMPORT-SPEED-1 F2), cleared after
    /// each query through the list of words it touched. A per-triangle 32-bit stamp
    /// was 4 bytes x the grid's triangles in EVERY thread's scratch — 800 MB at 10 M
    /// triangles on 20 threads for the stamps alone; the bits are 1/32 of that
    /// (25 MB), and keep the dense array's cache behaviour (neighbouring triangles
    /// have neighbouring indices). A hash set sized by the query was measured and
    /// refused: a bake walks ~1e9 shared triangles and the probes cost the scan
    /// 20 %. Only triangles filed in more than one cell are ever marked (`mShared`).
    /// Same answer: "skip a triangle already tested in this query", exactly.
    struct Query
    {
        std::vector<std::uint64_t> bits;
        std::vector<unsigned> touched;   ///< words of `bits` this query set
        std::vector<Tie> ties;
    };

    /// The nearest point of the soup to `p`, and — when `normalOut` is asked for —
    /// the ANGLE-WEIGHTED PSEUDONORMAL there (see `angleWeightAt`: a face normal is
    /// the wrong answer on an edge or a vertex, and on a convex feature sharper
    /// than a right angle it gets the SIGN wrong). Returns the distance, or
    /// infinity for an empty soup.
    ///
    /// The tie set is collected as the walk goes and resolved at the end, because
    /// `best` only stops shrinking when the walk stops: a candidate is kept when it
    /// is within `eps` of the best SO FAR, and the final pass drops whatever the
    /// eventual best left behind. The scratch lives in the caller's `Query` (one per
    /// thread), so the hundreds of thousands of queries a bake makes allocate once.
    ///
    /// `triangleOut` receives the index (in the soup's own order) of the triangle
    /// the nearest point lies on — the cluster DAG's region assignment asks which
    /// simplified triangle a level-0 triangle now stands under.
    ///
    /// `stopAtOrBelow` >= 0 turns the query into "is anything within this?": the
    /// walk returns the first distance at or below it, which is NOT the nearest
    /// — only for a caller taking a MAXIMUM of distances whose running value is
    /// that threshold, where no such answer can move the maximum (the cluster
    /// DAG's area terms). Never with a point or a normal asked for; a triangle asked
    /// for is written only when the walk returns the nearest (a return above the
    /// stop) — at or below it the triangle is whichever was nearest so far and
    /// callers must not read it (`nearestOn`).
    float closest(Query &query, const Vec3 &p, Vec3 *pointOut = nullptr, Vec3 *normalOut = nullptr,
                  unsigned *triangleOut = nullptr, float stopAtOrBelow = -1.0f) const
    {
        if (mTriCount == 0) return std::numeric_limits<float>::infinity();
        int base[3];
        cellOf(p, base);
        beginQuery(query);
        std::vector<Tie> &ties = query.ties;
        ties.clear();
        float best = std::numeric_limits<float>::infinity();
        unsigned bestTri = 0u;
        Vec3 bestPoint, bestNormal(0, 1, 0);
        const int maxRing = std::max(std::max(mDim[0], mDim[1]), mDim[2]);
        // p's clearance inside its own cell (0 when p lies outside the grid's box): every cell of ring r is at least
        // (r - 1) * cell + clearance away (ATOM-2: a point on the surface stops after its own cell, not after 27).
        float clearance = std::numeric_limits<float>::infinity();
        {
            const float c[3] = { p.x() - mLo.x(), p.y() - mLo.y(), p.z() - mLo.z() };
            for (int a = 0; a < 3; ++a) {
                const float local = c[a] - float(base[a]) * mCell;
                clearance = std::min(clearance, std::min(local, mCell - local));
            }
            if (!(clearance > 0.0f)) clearance = 0.0f;
        }
        for (int ring = 0; ring <= maxRing; ++ring) {
            // The stop rule, and the whole reason this is exact: nothing in ring
            // `ring` can beat `best` once (ring - 1) * cell + clearance already exceeds it.
            if (ring > 0 && float(ring - 1) * mCell + clearance > best) break;
            bool any = false;
            for (int z = base[2] - ring; z <= base[2] + ring; ++z) {
                if (z < 0 || z >= mDim[2]) continue;
                for (int y = base[1] - ring; y <= base[1] + ring; ++y) {
                    if (y < 0 || y >= mDim[1]) continue;
                    for (int x = base[0] - ring; x <= base[0] + ring; ++x) {
                        if (x < 0 || x >= mDim[0]) continue;
                        // Only the SHELL, not the solid block: the interior was
                        // scanned by the earlier rings.
                        if (ring > 0 && std::abs(x - base[0]) != ring &&
                            std::abs(y - base[1]) != ring && std::abs(z - base[2]) != ring)
                            continue;
                        any = true;
                        const size_t cell = index(x, y, z);
                        for (unsigned i = mStart[cell]; i < mStart[cell + 1]; ++i) {
                            const unsigned t = mTris[i];
                            if (mShared[t] && !firstVisit(query, t)) continue;   // filed in several cells
                            const Vec3 a = vertexAt((*mIndices)[size_t(t) * 3]);
                            const Vec3 b = vertexAt((*mIndices)[size_t(t) * 3 + 1]);
                            const Vec3 c = vertexAt((*mIndices)[size_t(t) * 3 + 2]);
                            // THE BOX FIRST (ATOM-2: the one-step measurement queries this grid tens of millions of
                            // times): a triangle whose box is further than the best so far cannot be nearer — skipped
                            // without the closest-point solve. Exact: the box contains the triangle. (Not on the normal
                            // query: its tie set reads distances within eps of the best.)
                            if (!normalOut && best < std::numeric_limits<float>::infinity()) {
                                const float bx = std::max(std::max(std::min(std::min(a.x(), b.x()), c.x()) - p.x(),
                                                                   p.x() - std::max(std::max(a.x(), b.x()), c.x())), 0.0f);
                                const float by = std::max(std::max(std::min(std::min(a.y(), b.y()), c.y()) - p.y(),
                                                                   p.y() - std::max(std::max(a.y(), b.y()), c.y())), 0.0f);
                                const float bz = std::max(std::max(std::min(std::min(a.z(), b.z()), c.z()) - p.z(),
                                                                   p.z() - std::max(std::max(a.z(), b.z()), c.z())), 0.0f);
                                if (bx * bx + by * by + bz * bz > best * best) continue;
                            }
                            const Vec3 q = closestOnTriangle(p, a, b, c);
                            const float d = std::sqrt((q - p).lengthSquared());
                            const float eps = tieEps(d);
                            if (normalOut) {
                                // THE TIE SET IS THE NEAREST FEATURE'S FACES AND
                                // NOTHING ELSE, and bounding it is not tidiness —
                                // it is the difference between 0.4 s and 9 s of
                                // bake on `endlessplane.obj`. A cell high above a
                                // large flat mesh is nearly equidistant from
                                // THOUSANDS of its triangles, so "within eps of the
                                // best so far" collected the mesh; an angle weight
                                // (an acos and two normalises) per collected
                                // triangle per cell then dominated everything.
                                // A strictly nearer hit RESTARTS the set, since the
                                // feature has changed; `kMaxTies` is generous for
                                // the real cases — the faces around one vertex.
                                if (d < best - eps) ties.clear();
                                if (d <= best + eps && ties.size() < kMaxTies)
                                    ties.push_back({ q, a, b, c, d });
                            }
                            if (d < best) { best = d; bestPoint = q; bestTri = t; }
                            if (d <= stopAtOrBelow) return d;
                        }
                    }
                }
            }
            (void)any;
        }
        if (normalOut) {
            // The angle-weighted sum over every triangle that really is nearest.
            const float eps = tieEps(best);
            Vec3 sum(0, 0, 0);
            for (const Tie &tie : ties) {
                if (tie.d > best + eps) continue;
                const Vec3 n = Vec3::crossProduct(tie.b - tie.a, tie.c - tie.a).normalized();
                if (n.lengthSquared() < 0.5f) continue;
                sum = sum + n * angleWeightAt(tie.q, tie.a, tie.b, tie.c, eps);
            }
            const float len = sum.length();
            if (len > 0.0f) bestNormal = sum / len;
            *normalOut = bestNormal;
        }
        if (pointOut) *pointOut = bestPoint;
        if (triangleOut) *triangleOut = bestTri;
        return best;
    }

    float cell() const { return mCell; }

    /// Every triangle (by its index in the soup) whose AABB may touch the box
    /// [lo, hi]: the union of the cells the box covers, each triangle once. A
    /// SUPERSET of the triangles that intersect the box — never a subset, because
    /// a triangle is filed in every cell its own AABB touches and both boxes clamp
    /// to the grid the same way — which is what makes a local soup built from it
    /// exact for any query whose nearest surface lies inside the box.
    ///
    /// Its visited set is its OWN (one bit per triangle, freed on return), so no
    /// caller's query scratch has to be passed in or sized for this grid.
    void collect(const Vec3 &lo, const Vec3 &hi, std::vector<unsigned> &out) const
    {
        out.clear();
        if (mTriCount == 0) return;
        int c0[3], c1[3];
        cellOf(lo, c0);
        cellOf(hi, c1);
        Query visited;
        beginQuery(visited);   // its own (see above), sized to this grid
        for (int z = c0[2]; z <= c1[2]; ++z)
            for (int y = c0[1]; y <= c1[1]; ++y)
                for (int x = c0[0]; x <= c1[0]; ++x)
                    for (unsigned i = mStart[index(x, y, z)]; i < mStart[index(x, y, z) + 1]; ++i) {
                        const unsigned t = mTris[i];
                        if (mShared[t] && !firstVisit(visited, t)) continue;
                        out.push_back(t);
                    }
    }

    /// The tolerance that decides "the same nearest feature". Two faces sharing an
    /// edge give analytically equal distances to a point on it and differ only by
    /// round-off, so the test has to be a tolerance; it is tied to the CELL (the
    /// coordinate scale) with a term in the distance itself for far queries.
    /// Genuinely distinct surfaces at the same distance are a medial-axis point,
    /// where the sign is ill-defined however it is computed.
    float tieEps(float d) const { return std::max(mCell * 1e-4f, std::fabs(d) * 1e-4f); }

private:
    /// Empties the visited set (the words the last query touched) and sizes it for
    /// this grid; one scratch may serve grids of any size.
    void beginQuery(Query &query) const
    {
        for (unsigned w : query.touched) query.bits[w] = 0u;
        query.touched.clear();
        const size_t words = (mTriCount + 63) / 64;
        if (query.bits.size() < words) query.bits.resize(words, 0u);
    }
    /// True the first time `t` is seen in the current query (and records it).
    static bool firstVisit(Query &query, unsigned t)
    {
        std::uint64_t &word = query.bits[t >> 6];
        const std::uint64_t bit = std::uint64_t(1) << (t & 63u);
        if (word & bit) return false;
        if (!word) query.touched.push_back(t >> 6);
        word |= bit;
        return true;
    }
    Vec3 vertexAt(unsigned i) const
    {
        const float *v = mPositions + size_t(i) * size_t(mPosComps);
        return Vec3(v[0], v[1], v[2]);
    }
    void cellOf(const Vec3 &p, int out[3]) const
    {
        const float c[3] = { p.x() - mLo.x(), p.y() - mLo.y(), p.z() - mLo.z() };
        for (int a = 0; a < 3; ++a)
            out[a] = std::clamp(int(std::floor(c[a] / mCell)), 0, mDim[a] - 1);
    }
    size_t index(int x, int y, int z) const
    {
        return (size_t(z) * size_t(mDim[1]) + size_t(y)) * size_t(mDim[0]) + size_t(x);
    }

    const float *mPositions = nullptr;
    int mPosComps = 3;
    const std::vector<unsigned> *mIndices = nullptr;
    size_t mTriCount = 0;
    Vec3 mLo;
    float mCell = 1.0f;
    int mDim[3] = { 1, 1, 1 };
    std::vector<unsigned> mStart;   ///< per cell, into mTris (cells + 1 entries)
    std::vector<unsigned> mTris;    ///< every cell's triangles, cell after cell
    std::vector<unsigned char> mShared;   ///< per triangle: filed in more than one cell
    /// The faces meeting at one vertex, with room to spare. A nearest FEATURE is a
    /// face (1), an edge (2) or a vertex (its incident faces); anything beyond this
    /// is a near-equidistant crowd that contributes nothing to a pseudonormal.
    static constexpr size_t kMaxTies = 32;
};

/// One query scratch per pool slot (`bakepool::forRange`'s `slot`).
using Queries = std::vector<TriangleGrid::Query>;

}   // namespace surface

}   // namespace iris

#endif   // IRIS_IMPORT_BAKESHARED_H
