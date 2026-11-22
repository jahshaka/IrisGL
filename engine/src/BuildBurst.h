#pragma once
// THE BUILDS' BURST (SHADER-SYSTEM-1 stage 7; the notice's ONE engine fact, the lead's ruling): while shaders or ray
// pipelines build in the background the product SAYS SO - "n of m" - and every host reads the same numbers from the
// engine's publish (AsyncShaderStats::shaderBurst / rayBurst): the app's toolbar indicator, jahlab's overlay and its
// `LAB BUILDING` lines, the headset's loading surround. A BURST is the run of frames in which something is pending: it
// BEGINS when the pending count rises from 0 (its base: the landings so far), `landed` counts the landings since, `total`
// is landed + pending (a request made inside the burst raises it). When pending returns to 0 the burst has ENDED and
// keeps reading its last `n of n` until the next one begins (a host hides its line at pending 0). A landing is a build
// that finished, a failure included (a failure is final: it never stays pending). Work that began and ended between two
// feeds is no burst of its own: the next one counts from after it. The law is a CPU row: engine.build_burst
// (tests/engine/test_build_burst.cpp).

namespace jahshaka { namespace engine {

class BuildBurst {
public:
    /// One publish: what is pending now, and the landings so far (a running total that never falls).
    void feed(unsigned pending, unsigned long long landedTotal)
    {
        if (pending > 0u && mPending == 0u) mBase = mLastLandedTotal;   // a burst begins after the last publish's state
        if (pending > 0u || mPending > 0u) {   // in a burst, or the publish that ends it (its last landings counted)
            mLanded = unsigned(landedTotal - mBase);
            mTotal = mLanded + pending;
        }
        mPending = pending;
        mLastLandedTotal = landedTotal;
    }
    unsigned landed() const { return mLanded; }
    unsigned total() const { return mTotal; }

private:
    unsigned mPending = 0u, mLanded = 0u, mTotal = 0u;
    unsigned long long mBase = 0ull, mLastLandedTotal = 0ull;
};

}}  // namespace jahshaka::engine
