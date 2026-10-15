#pragma once
// THE ENGINE'S RENDER LOOP (ENGINE-UPGRADE-U3, THE SPLIT; SPECS/ENGINE-UPGRADE-U3.md stage 1, decisions F1-F4, F7).
//
// The object that WILL own the render thread (stage 4). Today it owns what that thread will own, and nothing moves
// threads yet: the HAND-OVER SLOT (the record the document side accumulates between two frames), the SERIAL (one per
// frame that applies a record), the LOOP (tickInline: one turn on the caller's thread — Engine::renderOneFrame) and the
// PUBLISH (the engine's PublishedEngineFrame and every scene's PublishedFrame, a whole-frame swap at the close).
//
// THE FRAME'S BODY STAYS IN OgreEngine (F7: it touches 23 engine fields, the monitor's global and 26 private
// functions — moving it gains nothing); this object calls OgreEngine::runFrame, the body, with the record it
// handed over.
//
// TWO SIDES, EACH WRITTEN BY ONE THREAD:
//   * THE GAME SIDE (whoever submits — the document thread from stage 4 on): submit() MERGES into the pending record
//     exactly as the engine always did (F1: the merge is the record's builder; the host's 2-6 submits a frame are one
//     record), noteHostStage() banks the host's half of the monitor's record beside it. Both under the slot's lock.
//   * THE RENDER SIDE: tickInline() takes the slot under the same lock at the frame head (the HAND-OVER), stamps the
//     serial THEN — never per submit — and an EMPTY slot is a legal hand-over that carries the last serial (F2).
// Ogre-free: the engine's private TUs include it through EnginePrivate.h.
#include "jahshaka/engine/Types.h"
#include "SyncGetters.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace jahshaka { namespace engine { namespace detail {

class OgreEngine;

class RenderThread {
public:
    explicit RenderThread(OgreEngine &engine) : mEngine(engine) {}
    RenderThread(const RenderThread &) = delete;
    RenderThread &operator=(const RenderThread &) = delete;

    // ---- THE GAME SIDE -----------------------------------------------------------------------------------------
    /// Accumulates `in` into the pending record (the merge, FrameInput's rules). `in.serial` is ignored: the
    /// hand-over stamps it.
    void submit(FrameInput &&in);
    /// The host's half of the monitor's record (a stage it timed): banked in the slot, handed over with the record,
    /// folded into the frame's record by the render side — never written into the engine's current record across
    /// threads. Coalesced by name past kHostStageCoalesce entries so a slot nobody takes cannot grow without bound.
    void noteHostStage(const std::string &name, float ms);
    FrameSubmitStatus submitStatus() const;
    /// Runs `f(pending, any)` under the slot's lock — the engine's own game-side reads and purges of the pending
    /// record (a destroyed scene's or view's part, pendingGiConfiguration).
    template <class F> auto withPending(F &&f) {
        std::lock_guard<std::mutex> lock(mSlotLock);
        return f(mSlot.record, mSlot.any);
    }
    template <class F> auto withPending(F &&f) const {
        std::lock_guard<std::mutex> lock(mSlotLock);
        return f(static_cast<const FrameInput &>(mSlot.record), mSlot.any);
    }

    // ---- THE RENDER SIDE ---------------------------------------------------------------------------------------
    /// ONE TURN OF THE LOOP on the caller's thread (stage 1's seam; stage 4's run() loops it on its own thread):
    /// the hand-over, the frame (OgreEngine::runFrame), the publish.
    void tickInline();
    /// The last frame the loop closed (Engine::published()).
    const PublishedEngineFrame &published() const {
        return mPublished[mPublishedIdx.load(std::memory_order_acquire)];
    }
    /// The serial of the last record a hand-over took (0 = none yet).
    unsigned long long serial() const { return mSerial; }
    /// THE DISPLAY'S PERIOD (Engine::setDisplayPeriod): a display property the driver sets on change, read by the
    /// render side (stage 5's interpolation weight).
    void setDisplayPeriod(float seconds) { mDisplayPeriod.store(seconds, std::memory_order_relaxed); }
    float displayPeriod() const { return mDisplayPeriod.load(std::memory_order_relaxed); }

    static constexpr unsigned kHostStageCoalesce = 64u;

private:
    /// What the game side accumulates between two hand-overs.
    struct Slot {
        FrameInput              record;
        bool                    any = false;     ///< a submit since the last hand-over
        std::vector<FrameStage> hostStages;      ///< the host's half of the monitor's record
    };
    /// THE HAND-OVER (render side, under the lock): swaps the slot out, stamps the serial on a non-empty record.
    Slot handOver();
    /// THE PUBLISH (render side): the frame's error (taken from the engine's sink, which holds the CALLS' error again
    /// after this), every scene's half and the engine's half, each a whole-frame swap.
    void publish(bool recordApplied, std::string &&frameError);

    OgreEngine &mEngine;
    mutable std::mutex mSlotLock;
    Slot               mSlot;           ///< guarded by mSlotLock
    FrameSubmitStatus  mSubmit;         ///< guarded by mSlotLock
    // ---- render side only ----
    unsigned long long mSerial = 0;
    unsigned long long mFramesWithError = 0;
    std::vector<PublishedEngineFrame::FrameError> mFrameErrors;   ///< the newest kFrameErrorsKept, oldest first
    double             mPublishMicros = 0.0;
    std::atomic<float> mDisplayPeriod{ 0.0f };
    PublishedEngineFrame mPublished[2];
    std::atomic<unsigned> mPublishedIdx{ 0u };
};

}}}   // namespace jahshaka::engine::detail
