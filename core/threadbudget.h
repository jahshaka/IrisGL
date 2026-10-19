#ifndef IRIS_CORE_THREADBUDGET_H
#define IRIS_CORE_THREADBUDGET_H

// THE ONE CORE BUDGET (THREADING-1 stage 1). Every thread pool of the process reads its width from here — the engine
// (the texture multiload, the shader cache's boot apply), the document library (Bullet's scheduler, the bake pool) and
// Studio (the scene workers, Qt's global pool, the store's and the tile cache's pools) — so the counts are ONE answer
// for the box, never four copies of an arithmetic that drifted (the "4 dedicated" that were 9; Qt's pool of 20 and the
// bake pool's 19 that nobody counted).
//
// Header-only and Ogre-, GL- and Qt-free on purpose: the engine is a static library that links neither IrisGL nor Qt,
// so it reads this one file of IrisGL's core by include alone (irisgl/engine/CMakeLists.txt), as IrisGL and Studio do.
// One inline function: the numbers are a pure function of the hardware thread count and the three measurement
// overrides, so every library that asks gets the same answer.
//
// THE TIERS (a 10-core / 20-thread box in brackets):
//   foreground  the game thread [1] + Bullet's workers beside it [6]: the physics step (Bullet's index 0 is the game
//               thread). Bullet's workers = hardware - game - render - Ogre's pool, at least 1, at most 6 (7 threads
//               with the game thread: the measured best, spikes/bullet-batching-1 — past it one toppled island caps
//               the step).
//   render      the render thread [1] + the primary scene's SceneManager pool [8] = clamp(hardware, 2, 8), measured.
//               It runs WHILE the game thread steps (physics.step_overlap), so the two sets never share a thread.
//   dedicated   the shader cache's writer [1], Ogre's texture streaming thread [1], the texture multiload pool
//               [6] = min(6, max(2, hardware / 2)). BURSTY, NOT PER-FRAME (a texture load, a cache write): sized as
//               they are and NOT subtracted from the frame's set.
//   background  each below-normal pool's width [4] = hardware - the active set, at least 2: Qt's global pool (the 28
//               QtConcurrent sites: parses, bakes, encodes, file I/O), the bake pool's workers (nice 19), the tile
//               cache's and the bake store's own pools (min(theirs, it)).
//
// THE LAW: the per-frame ACTIVE SET (game + render + Ogre's pool + Bullet's workers: 16 here) never exceeds the box,
// and the background takes what it leaves (20 - 16 = 4 here). A box whose floors (Ogre's pool 2 — clamp(hardware, 2, 8)
// is the whole box below 9 threads —, Bullet 1, the background 2) exceed it still oversubscribes; no fix: those are
// the floors. A scheduler of our own is built when a SECOND foreground job beside the step is MEASURED needing one
// (today the step is the only one: the mirror's extract is ~1 ms at 1,000 movers, the compose and the interpolate run
// on Ogre's pool inside the render thread's frame, the import and the bakes are background).
//
// Overrides (measurement escape hatches, read once, never preferences): JAHSHAKA_SCENE_THREADS (the render tier's
// pool), JAH_TEXTURE_MULTILOAD (the multiload; 0 = off), JAHSHAKA_PHYSICS_THREADS (Bullet's threads, the game thread
// included).

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace iris {

struct ThreadBudget
{
    int hardware = 1;     ///< the box's hardware threads
    int game = 1;         ///< the game thread (the document; Bullet's index 0)
    int foreground = 1;   ///< Bullet's workers beside the game thread
    int render = 1;       ///< the render thread
    int ogrePool = 2;     ///< the primary scene's SceneManager workers (sceneworkers::Tier::Primary)
    int writer = 1;       ///< the shader cache's writer thread
    int streaming = 1;    ///< Ogre's texture streaming thread
    int multiload = 0;    ///< Ogre's texture multiload pool
    int background = 2;   ///< each below-normal pool's width (Qt's global pool, the bake pool's workers)

    int dedicated() const { return writer + streaming + multiload; }
    /// Bullet's scheduler threads: the game thread + its workers (btITaskScheduler::getNumThreads).
    int bulletThreads() const { return game + foreground; }
    /// The per-frame active set: the law's left side (the dedicated set is bursty and not in it).
    int activeSet() const { return game + foreground + render + ogrePool; }
    /// The active set + the background: the box exactly when the background is above its floor.
    int total() const { return activeSet() + background; }
};

/// Bullet's measured best, as a ceiling of workers beside the game thread (7 threads in all; see the header's tiers).
constexpr int kBudgetForegroundCap = 6;
/// The background tier's floor: a parse and a bake progress side by side however full the box is.
constexpr int kBudgetBackgroundFloor = 2;

/// THE FUNCTION: the tiers for a box of `hardware` threads. An override < 0 is "none"; sceneThreads and physicsThreads
/// count from 1, multiload from 0 (off).
inline ThreadBudget threadBudgetFor(int hardware, int sceneThreads = -1, int multiload = -1, int physicsThreads = -1)
{
    ThreadBudget b;
    b.hardware = std::max(1, hardware);
    b.ogrePool = sceneThreads > 0 ? std::clamp(sceneThreads, 1, 32) : std::clamp(b.hardware, 2, 8);
    b.multiload = multiload >= 0 ? std::clamp(multiload, 0, 32) : std::min(6, std::max(2, b.hardware / 2));
    if (physicsThreads > 0)
        b.foreground = std::clamp(physicsThreads, 1, 32) - b.game;
    else
        b.foreground = std::clamp(b.hardware - b.game - b.render - b.ogrePool, 1, kBudgetForegroundCap);
    b.background = std::max(kBudgetBackgroundFloor, b.hardware - b.activeSet());
    return b;
}

namespace detail {
inline int budgetEnvInt(const char *name, int none)
{
    const char *v = std::getenv(name);
    if (!v || !*v) return none;
    char *end = nullptr;
    const long n = std::strtol(v, &end, 10);
    return end && *end == '\0' ? int(std::clamp(n, -1l, 1024l)) : none;
}
}   // namespace detail

/// THIS box's budget, computed once for the process (the hardware count and the overrides do not change under it).
inline const ThreadBudget &threadBudget()
{
    static const ThreadBudget budget = [] {
        const unsigned hw = std::thread::hardware_concurrency();
        const int scene = detail::budgetEnvInt("JAHSHAKA_SCENE_THREADS", -1);
        const int multiload = detail::budgetEnvInt("JAH_TEXTURE_MULTILOAD", -1);
        const int physics = detail::budgetEnvInt("JAHSHAKA_PHYSICS_THREADS", -1);
        return threadBudgetFor(hw > 0 ? int(hw) : 1, scene > 0 ? scene : -1, multiload, physics > 0 ? physics : -1);
    }();
    return budget;
}

}   // namespace iris

#endif   // IRIS_CORE_THREADBUDGET_H
