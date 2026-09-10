/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef IRIS_SIMULATIONCLOCK_H
#define IRIS_SIMULATIONCLOCK_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace iris
{

/// THE ONE SIMULATION CLOCK (ENGINEERING_DEBT_SPEC A4.2, ADDENDUM 4).
///
/// Everything that moves the document over time — Bullet, the avatar movement
/// components and their locomotion machines, possession input, keyframe and
/// skeletal animation — and everything the renderer simulates on the document's
/// behalf (particles, the shader `time` auto-params) advances on ONE fixed
/// grid: `kStepHz` steps per simulated second. A frame hands the clock the
/// wall time it took (or a scripted dt), the clock converts it into a whole
/// number of steps plus a carried remainder, and the frame's consumers step
/// that many times with exactly `kStepSeconds` each.
///
/// What this buys, and what it costs (the FIXED-RATE POLICY):
///
///  - DETERMINISM. Two runs that hand the clock the same total time produce
///    the same step count and therefore the same document state, bit for
///    bit — whether the time arrived as 120 frames of 1/60 s, 60 frames of
///    1/30 s or 240 frames of 1/120 s. That is what lets a suite assert a
///    physics trajectory at all (the bullet-bump lane's finding: with the wall
///    clock feeding `stepSimulation` at maxSubSteps 1, the simulation ran at a
///    speed set by the frame rate, stalled on frames shorter than 1/60 s and
///    fell behind on slow ones).
///  - RATE INDEPENDENCE. A 30 fps Debug session and a 144 Hz panel simulate
///    the same seconds per second: the first steps twice per frame, the second
///    alternates between zero and one step.
///  - NO RENDER INTERPOLATION (the cost). A frame renders the LATEST grid
///    state, so on a panel faster than the grid a rigid body moves in 60 Hz
///    increments — the same policy as Unity's default and as the avatar
///    movement component already shipped with. `alpha()` is the fraction of a
///    step the accumulator is carrying, exposed so a render-side interpolation
///    can be added later without touching the clock; nothing reads it today.
///  - A BOUNDED CATCH-UP. A frame longer than `kMaxStepsPerAdvance` steps
///    (a shader-compile hitch, a debugger pause) runs that many and DROPS the
///    rest: the simulation falls behind the wall clock for that frame rather
///    than spiralling — running ever more steps per frame because the previous
///    frame took ever longer. The scripted paths (editor.frame / player.frame)
///    refuse a dt above that bound instead of silently clamping it.
///
/// The arithmetic is done in double and the simulated time is derived from an
/// INTEGER step count (`time() == steps() * kStepSeconds`), never accumulated
/// as a float: a float running sum of 1/60 drifts from the grid within a few
/// hundred frames and would make `time()` depend on how many frames delivered
/// the same seconds. The tiny epsilon in advance() absorbs the float->double
/// rounding of a dt that is "exactly" one step (1.0f/60.0f is 1e-9 s above
/// the double 1/60; a 1/30 float is 2e-9 above two steps) so those frames
/// count as the whole steps they mean; the carried remainder then grows by at
/// most that epsilon per frame (~2e-4 s after an hour of 1/60 frames, measured
/// by the unit test) instead of creeping up to a phantom extra step.
///
/// Value type, no engine, no Qt: the document owns one (iris::Scene) and the
/// document tests exercise it with no display.
class SimulationClock
{
public:
    /// The grid. 60 steps per simulated second — Bullet's own default fixed
    /// step and what every gate in the tree was already written against.
    static constexpr int    kStepHz      = 60;
    static constexpr double kStepSeconds = 1.0 / double(kStepHz);
    /// The catch-up bound: at most this many steps for one advance (133 ms of
    /// simulation), the remainder dropped.
    static constexpr int    kMaxStepsPerAdvance = 8;
    /// The most a scripted frame may ask for in one go — anything above it is
    /// refused by the verbs (see the class comment). ONE STEP UNDER the catch-up
    /// bound: advance() drops on the ACCUMULATOR, so a dt at the full bound plus
    /// a carried fraction of a step would silently lose that fraction (A4.2 code
    /// review S2); at seven steps the carry (< 1 step) can never cross eight, and
    /// a scripted dt under this bound simulates exactly what it asked for.
    static constexpr double kMaxAdvanceSeconds = (kMaxStepsPerAdvance - 1) * kStepSeconds;

    /// Hands the clock `seconds` of time (wall or scripted; negatives count as
    /// zero) and returns how many whole steps that buys, bounded by
    /// kMaxStepsPerAdvance. The caller steps its consumers that many times
    /// with kStepSeconds each.
    int advance(double seconds)
    {
        mAccumulator += std::max(0.0, seconds);
        int steps = int(std::floor((mAccumulator + kEpsilon) / kStepSeconds));
        if (steps >= kMaxStepsPerAdvance) {
            steps = kMaxStepsPerAdvance;
            mAccumulator = 0.0;          // the rest is dropped, deliberately
        } else {
            mAccumulator = std::max(0.0, mAccumulator - steps * kStepSeconds);
        }
        mStepCount += uint64_t(steps);
        mFrameSteps = steps;
        return steps;
    }

    /// Simulated seconds since reset(): the step count on the grid, exactly.
    double time() const { return double(mStepCount) * kStepSeconds; }
    /// Steps run since reset().
    uint64_t steps() const { return mStepCount; }
    /// Steps the LAST advance() bought.
    int frameSteps() const { return mFrameSteps; }
    /// Simulated seconds the last advance() bought — what a host hands the
    /// renderer's frame delta so the engine-side simulation stays on the grid.
    double frameSeconds() const { return double(mFrameSteps) * kStepSeconds; }
    /// The fraction of a step the accumulator is carrying, in [0, 1). For a
    /// future render-side interpolation between grid states; unread today.
    double alpha() const { return std::min(1.0, mAccumulator / kStepSeconds); }

    /// Back to t = 0 with nothing carried. Play start and stop do this.
    void reset()
    {
        mAccumulator = 0.0;
        mStepCount = 0;
        mFrameSteps = 0;
    }

private:
    static constexpr double kEpsilon = 1e-7;   // ~6e-6 of a step

    double   mAccumulator = 0.0;
    uint64_t mStepCount = 0;
    int      mFrameSteps = 0;
};

} // namespace iris

#endif // IRIS_SIMULATIONCLOCK_H
