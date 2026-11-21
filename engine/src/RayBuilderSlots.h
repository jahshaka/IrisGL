#pragma once
// THE RAY PIPELINE BUILDERS' SLOTS (COLD-BOOT-1 stage 4; irisgl/core/threadbudget.h, ThreadBudget::rayBuilds): no more than
// the tier's width of driver compiles run at once. A builder WAITS for a slot on its own thread before its first compile
// (take - never the render thread) and gives it back after its last (give). A builder that already holds one may ask for a
// SECOND builder of its own (the reflection pair: reflect and reflect-compact side by side) only through tryTake, which never
// waits: a builder holding a slot never waits on the tier for another, so a tier one wide builds the pair serially and no
// width can deadlock. The law is a CPU row: engine.ray_builder_slots (tests/engine/test_ray_builder_slots.cpp).
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace jahshaka { namespace engine {

class RayBuilderSlots {
public:
    /// The tier's width (at least one), set once before any builder runs (RayQueryTier::startAhead).
    void reset(int width)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mFree = std::max(1, width);
    }
    /// A builder's slot, waited for on ITS OWN thread.
    void take()
    {
        std::unique_lock<std::mutex> lock(mMutex);
        mCv.wait(lock, [this] { return mFree > 0; });
        --mFree;
    }
    /// A second builder's slot, only if one is free NOW - never a wait.
    bool tryTake()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mFree <= 0) return false;
        --mFree;
        return true;
    }
    void give()
    {
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mFree;
        }
        mCv.notify_one();
    }
    int freeNow() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mFree;
    }

private:
    mutable std::mutex mMutex;
    std::condition_variable mCv;
    int mFree = 0;
};

}}   // namespace jahshaka::engine
