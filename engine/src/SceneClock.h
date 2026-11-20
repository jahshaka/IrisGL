#pragma once
// THE SCENE'S CLOCK (ONE-FRAME-1 stage 1; Types.h FrameInputScene::stepSerial says the law) — a pure value type: no Ogre,
// no Qt, no engine. OgreScene keeps one; the CPU rows (frame.clock_once, frame.two_scenes, frame.particle_scale,
// frame.clock_drawn_only) drive it directly.
//
// A scene's time is its DOCUMENT's steps. A drain (a frame's or a join's) takes the scene's part: the steps its serial
// moved since the last part are OWED. A frame that DRAWS the scene consumes them — at most kMaxStepsPerFrame (the
// document's own catch-up bound, iris::SimulationClock::kMaxStepsPerAdvance): the excess is DROPPED, never replayed
// (time nobody saw is not shown at once: a window restored after ten minutes minimised resumes, it does not leap). A
// frame that does not draw the scene leaves its owed steps for the frame that does (a thumbnail's frame never eats the
// editor's steps) and hands it 0.
#include <algorithm>
#include <cstdint>

#include "jahshaka/engine/Types.h"

namespace jahshaka { namespace engine { namespace detail {

struct SceneClock {
    static constexpr unsigned long long kMaxStepsPerFrame = FrameInputScene::kMaxStepsPerFrame;

    bool               primed = false;   ///< a part's serial has been seen (since the last document change)
    unsigned long long seen = 0;         ///< the last serial a part carried
    unsigned long long epoch = 0;        ///< the document the serials count (FrameInputScene::clockEpoch)
    unsigned long long owed = 0;         ///< steps counted by drains, not yet consumed by a frame that drew the scene
    bool               hostStamped = false;   ///< a host ever stamped it: the harness door never steps it again
    double             period = 1.0 / 60.0;   ///< FrameInputScene::stepPeriod
    float              particleScale = 1.0f;  ///< FrameInputScene::particleTimeScale
    unsigned long long steps = 0;        ///< the steps the scene's drawn frames consumed, in all (scene time = x period)
    unsigned long long dropped = 0;      ///< steps over a frame's bound, dropped (a hidden stretch's time, counted)
    double             dt = 0.0;         ///< THIS frame's scene time (consumed steps x period): what every reader takes
    double             particleDt = 0.0; ///< ...and its particles' (dt x particleScale)
    unsigned long long newest = 0;       ///< the newest grid state (the presentation's hi)
    double             presentTime = 0.0;   ///< the presentation clock (U3 stage 5), in this scene's step time

    /// A drain's part (the frame's or a join's).
    void take(const FrameInputScene &part)
    {
        if (part.stepPeriod && *part.stepPeriod > 0.0f) period = double(*part.stepPeriod);
        if (part.particleTimeScale) particleScale = std::max(0.0f, *part.particleTimeScale);
        // ONLY A NEW DOCUMENT RE-PRIMES (the stage-1 audit's bug 2): its mirror's setSource moves the epoch. A `reassert`
        // (re-apply every field: a page switch, a re-take, a Player shot) is no new clock and keeps the steps.
        if (part.clockEpoch && *part.clockEpoch != epoch) {
            epoch = *part.clockEpoch;
            primed = false;
        }
        if (!part.stepSerial) return;
        const unsigned long long serial = *part.stepSerial;
        hostStamped = true;   // the harness door never steps it again: one source of this scene's time
        if (primed && serial >= seen) {
            owed += serial - seen;   // the steps since the last part, ONCE (an equal serial: 0)
            if (serial > newest) newest = serial;
        } else {
            // PRIMED HERE: the scene's first serial, or the first of a new document (a lower serial in the same epoch is a
            // host stamping another clock on this scene: re-primed the same way, 0 steps, never a wrap). The presentation
            // starts at the incoming grid state.
            newest = serial;
            presentTime = double(serial) * period;
        }
        seen = serial;
        primed = true;
    }

    /// THE HARNESS's GRID (inspect::EngineEcho::harnessStep): a scene no host ever stamped owes `n` steps.
    void harnessStep(unsigned long long n)
    {
        if (!hostStamped) owed += n;
    }

    /// THE FRAME's clock, once per frame before anything of it reads time. `drawn`: the frame draws this scene.
    void beginFrame(bool drawn)
    {
        if (!drawn) {   // its steps wait for the frame that draws it
            dt = 0.0;
            particleDt = 0.0;
            return;
        }
        const unsigned long long take = std::min(owed, kMaxStepsPerFrame);
        dropped += owed - take;
        owed = 0;
        steps += take;
        dt = double(take) * period;
        particleDt = dt * double(particleScale);
    }

    double sceneSeconds() const { return double(steps) * period; }
};

}}}  // namespace jahshaka::engine::detail
