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
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <exception>
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
    /// ONE MESSAGE onto `scene`'s pending part (decision 35): the slot's lock and an append — the last part touched is
    /// tried first, so a sync's run of messages finds its part at once.
    void post(Scene *scene, SceneMessage &&message);
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
    /// THE OWNER'S MODE (U3 stage 5): the render thread draws continuously, paced by the present and the display
    /// period, applying the newest record and interpolating between its two grid states; off = only on request.
    void setFreeRunning(bool on);
    /// One free frame now, waited for (inspect::EngineEcho::presentationFrame).
    void freeFrameNow() { runOn([this] { tickWith(handOver(), true); }); }
    bool freeRunning() const { return mFreeRun.load(std::memory_order_relaxed); }
    /// The engine's half of the snapshot re-made after a request (the frame's stamp unchanged).
    void republishEngine();
    /// The last frame the loop closed (Engine::published()).
    const PublishedEngineFrame &published() const { return mPublished.read(); }
    /// The host's latch (U3 stage 5): its reads see the newest snapshot from here to its next latch.
    void latchSnapshots();
    /// The serial of the last record a hand-over took (0 = none yet).
    unsigned long long serial() const { return mSerial; }
    /// THE DISPLAY'S PERIOD (Engine::setDisplayPeriod): a display property the driver sets on change, read by the
    /// render side (stage 5's interpolation weight).
    void setDisplayPeriod(float seconds) { mDisplayPeriod.store(seconds, std::memory_order_relaxed); }
    float displayPeriod() const { return mDisplayPeriod.load(std::memory_order_relaxed); }

    static constexpr unsigned kHostStageCoalesce = 64u;

    // ---- THE THREAD (ENGINE-UPGRADE-U3 stage 4b) -------------------------------------------------------------------
    // The render thread owns Root, the device, every window surface and every frame. It runs ONE task at a time —
    // a frame (the lockstep verb), a request, a join (createView / destroyView / the boot / the teardown) — and the
    // host that posted it waits for it: LOCKSTEP, so the two threads never run engine code at once. While it waits
    // the host keeps its own side alive (pumpHost: the compile observer's splash, on the host's thread).
    /// Starts the thread and runs `boot` on it (Root, the device); returns boot's answer. Not called = inline: every
    /// task runs on the caller's thread (the engine fixtures).
    bool start(std::function<bool()> boot);
    /// Runs `task` on the render thread and waits for it; inline when the thread is not started or this IS it.
    void runOn(const std::function<void()> &task);
    /// Runs `teardown` on the render thread, then joins it.
    void stop(const std::function<void()> &teardown);
    bool threaded() const { return mThread.joinable(); }
    bool onRenderThread() const { return !threaded() || std::this_thread::get_id() == mThread.get_id(); }
    /// What the host does while it waits (set by OgreEngine: the compile observer's delivery).
    void setHostPump(std::function<void()> pump) { mHostPump = std::move(pump); }

private:
    /// What the game side accumulates between two hand-overs.
    struct Slot {
        FrameInput              record;
        bool                    any = false;     ///< a submit since the last hand-over
        std::vector<FrameStage> hostStages;      ///< the host's half of the monitor's record
    };
    /// THE HAND-OVER (render side, under the lock): swaps the slot out, stamps the serial on a non-empty record.
    Slot handOver();
    void tickWith(Slot &&handed, bool freeFrame);
    /// THE PUBLISH (render side): the frame's error (taken from the engine's sink, which holds the CALLS' error again
    /// after this), every scene's half and the engine's half, each a whole-frame swap.
    void publish(bool recordApplied, std::string &&frameError);

    OgreEngine &mEngine;
    // ---- the thread's task hand-off (stage 4b) ----
    void threadMain();
    std::thread             mThread;
    std::mutex              mTaskLock;
    std::condition_variable mTaskCv;
    std::function<void()>   mTask;
    bool                    mTaskPending = false;
    bool                    mQuit = false;
    std::exception_ptr      mTaskThrown;   ///< what the last task threw (runOn rethrows it on the asker)
    std::function<void()>   mHostPump;
    std::atomic<bool>       mFreeRun{ false };
    bool                    mTornDown = false;   ///< render side: the teardown ran (no frame after it)
    std::thread::id         mHostThread;   ///< the thread that started the engine: the only one that pumps
    mutable std::mutex mSlotLock;
    Slot               mSlot;           ///< guarded by mSlotLock
    FrameSubmitStatus  mSubmit;         ///< guarded by mSlotLock
    size_t             mLastPart = 0;
    unsigned long long mPostSeq = 0;    ///< guarded by mSlotLock: the one order's counter (decision 46)   ///< guarded by mSlotLock: the scene part the last post appended to
    // ---- render side only ----
    unsigned long long mSerial = 0;
    unsigned long long mHandOverNow = 0;   ///< the hand-over the running frame took
    unsigned long long mFramesWithError = 0;
    std::vector<PublishedEngineFrame::FrameError> mFrameErrors;   ///< the newest kFrameErrorsKept, oldest first
    double             mPublishMicros = 0.0;
    std::atomic<float> mDisplayPeriod{ 0.0f };
    std::atomic<float> mMeasuredPeriod{ 0.0f };            ///< the last two free frames' start-to-start (seconds)
    std::chrono::steady_clock::time_point mLastFreeStart{};   ///< render thread only
    mutable SnapshotBuffers<PublishedEngineFrame> mPublished;
};

}}}   // namespace jahshaka::engine::detail
