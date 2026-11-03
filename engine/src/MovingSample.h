#pragma once
// THE MOVING SAMPLE INDEX — ONE RULE FOR EVERY STOCHASTIC PRODUCER (TEST-STREAMLINE-1 stage 3c, URGENT #157).
//
// Each stochastic producer draws two sample sequences per view: AT REST a sequence keyed on the rest's own frame K
// (path-independent by design - "the rest mean is a function of the scene and the eye, never of the path",
// OgreRayQuery.cpp), and MOVING a sequence keyed on the view's own frame counter. That counter counted every frame the
// view had EVER traced, so a moving picture depended on what the view drew before: the self-test's mover pose hashed
// 2d88c4ee after the fixture half alone and 122cb3a8 after the default poses (URGENT #157). An estimator's samples are a
// function of the pose, never of the process.
//
// THE RULE: a view's moving index is the frames it has traced SINCE ITS LAST RESTART, and it restarts at
//   * a SCENE BIND - a different scene is a new history (the same rule as the rest's), told by the scene's history id
//     (OgreScene::historyId, process-unique: a destroyed scene's address can come back, its id never does), and
//   * a SIZE CHANGE - the producer's targets are re-made, its history is gone;
// and NOT at a camera cut (the view keeps its age: the cut's ruling stands). Its 16 bits are the shaders' (`& 0xFFFF`).
//
// The producers' own counters are untouched - they also index ring slots and readbacks in flight - the index is their
// difference from the counter's value at the restart. Used by all three: the reflection (OgreRayQuery.cpp, pp.knobs[2]),
// the gather (OgreScreenProbeGather.cpp, rp.seq[0] and pp.knobs6[2]) and the area visibility (OgreAreaVis.cpp,
// pp.strata[1]); source.moving_sample holds every moving site to it. No Ogre here: a pure function, unit-tested
// (tests/engine test_moving_sample.cpp).

namespace jahshaka::engine::detail {

class MovingSequence {
public:
    /// The index this frame samples with: `counter` (the producer's own frame counter, before this frame's advance)
    /// less its value at the last restart, 16 bits. A different `sceneId` or size than the last call's restarts it at
    /// this frame (index 0); so does a counter that went backwards (a producer that dropped its own state).
    unsigned index(unsigned counter, unsigned long long sceneId, unsigned width, unsigned height)
    {
        if (!mStarted || sceneId != mScene || width != mW || height != mH || counter < mBase) {
            mStarted = true;
            mScene = sceneId;
            mW = width;
            mH = height;
            mBase = counter;
        }
        return (counter - mBase) & 0xFFFFu;
    }

private:
    bool mStarted = false;
    unsigned long long mScene = 0ull;
    unsigned mW = 0u, mH = 0u;
    unsigned mBase = 0u;
};

/// THE ONE SCENE IDENTITY A STORED VIEW COMPARES (TEST-STREAMLINE-1 3c audit): a view's state outlives the scenes it
/// drew, and a destroyed scene's ADDRESS can come back for the next one - so a stored view keeps the scene's history id
/// (OgreScene::historyId, never reused) and compares that. True when `live` is ANOTHER scene than the stored one (a new
/// scene at a freed address included): the view's histories start empty. The stored id becomes `live`.
template <class Id>
inline bool rebindScene(Id &stored, Id live)
{
    const bool changed = stored != live;
    stored = live;
    return changed;
}

}   // namespace jahshaka::engine::detail
