// THE ENGINE'S RENDER LOOP (ENGINE-UPGRADE-U3 stage 1, the seam) — RenderThread.h says what it owns and why.
#include "EnginePrivate.h"

#include <algorithm>
#include <iterator>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <exception>
#include <utility>

namespace jahshaka { namespace engine { namespace detail {

// ---- host.sync_getters (SyncGetters.h) ---------------------------------------------------------------------------
thread_local int tlsEngineSide = 0;
namespace { std::atomic<SyncGetterSite *> gSyncSites{ nullptr }; }
SyncGetterSite::SyncGetterSite(const char *n) : name(n) {
    SyncGetterSite *head = gSyncSites.load(std::memory_order_relaxed);
    do { next = head; } while (!gSyncSites.compare_exchange_weak(head, this, std::memory_order_release,
                                                                 std::memory_order_relaxed));
}
SyncGetterSite *syncGetterSites() { return gSyncSites.load(std::memory_order_acquire); }

std::atomic<bool> gAffinityChecks{ false };
std::mutex &snapshotLock() { static std::mutex m; return m; }
namespace { SnapshotLatchable *gSnapshots = nullptr; }   // under snapshotLock()
void registerSnapshot(SnapshotLatchable *s) {
    std::lock_guard<std::mutex> l(snapshotLock());
    s->nextLatchable = gSnapshots;
    s->prevLatchable = nullptr;
    if (gSnapshots) gSnapshots->prevLatchable = s;
    gSnapshots = s;
}
void unregisterSnapshot(SnapshotLatchable *s) {
    std::lock_guard<std::mutex> l(snapshotLock());
    if (s->prevLatchable) s->prevLatchable->nextLatchable = s->nextLatchable;
    else if (gSnapshots == s) gSnapshots = s->nextLatchable;
    if (s->nextLatchable) s->nextLatchable->prevLatchable = s->prevLatchable;
    s->prevLatchable = s->nextLatchable = nullptr;
}
void latchAllSnapshots() {
    std::lock_guard<std::mutex> l(snapshotLock());
    for (SnapshotLatchable *s = gSnapshots; s; s = s->nextLatchable) s->latchLocked();
}
std::atomic<bool> gRenderThreaded{ false };
/// The render thread's id (set by the loop that owns the engine: the creating thread until stage 4b's thread).
std::atomic<std::thread::id> gRenderThreadId{};
void checkAffinity(const char *entry) {
    if (tlsEngineSide != 0) return;
    if (std::this_thread::get_id() == gRenderThreadId.load(std::memory_order_relaxed)) return;
    std::fprintf(stderr, "AFFINITY: engine entry %s called off the render thread\n", entry);
    std::fflush(stderr);
    std::abort();
}

// ---- THE GAME SIDE -----------------------------------------------------------------------------------------------

// THE MERGE STAYS (U3 F1): one frame today is the merge of 2-6 submits — the driver's cause, the viewport's pace, the
// mirror's record, the viewport's delta + gesture, a preview's scene part — and that merge IS the record's builder.
// What changed is where it lands: the hand-over slot, under its lock, never the record a frame is draining.
void RenderThread::post(Scene *scene, SceneMessage &&message) {
    if (!scene || !message.apply) return;
    std::lock_guard<std::mutex> lock(mSlotLock);
    ++mSubmit.messagesPosted;
    if (!mSlot.any) mSlot.firstAt = mSubmit.handOvers;   // the record's age is counted from its first submit
    mSlot.any = true;
    std::vector<FrameInputScene> &parts = mSlot.record.scenes;
    if (mLastPart >= parts.size() || parts[mLastPart].scene != scene) {
        mLastPart = 0;
        while (mLastPart < parts.size() && parts[mLastPart].scene != scene) ++mLastPart;
        if (mLastPart == parts.size()) {
            FrameInputScene part;
            part.scene = scene;
            parts.push_back(std::move(part));
        }
    }
    message.seq = ++mPostSeq;   // the one order across streams (decision 46)
    parts[mLastPart].messages.push_back(std::move(message));
}

void RenderThread::submit(FrameInput &&in) {
    std::lock_guard<std::mutex> lock(mSlotLock);
    ++mSubmit.submitted;
    if (!mSlot.any) mSlot.firstAt = mSubmit.handOvers;
    mSlot.any = true;
    FrameInput &p = mSlot.record;
    for (EngineMessage &m : in.engineMessages) m.seq = ++mPostSeq;   // the one order (decision 46)
    p.engineMessages.insert(p.engineMessages.end(), std::make_move_iterator(in.engineMessages.begin()),
                            std::make_move_iterator(in.engineMessages.end()));   // the engine's stream, in order
    for (FrameInputScene &sc : in.scenes) for (SceneMessage &m : sc.messages) m.seq = ++mPostSeq;
    for (FrameInputView &v : in.views) if (v.scene) v.sceneSeq = ++mPostSeq;
    if (in.frameDelta) p.frameDelta = in.frameDelta;
    if (in.frameDelta) mEngine.mPushedFrameDelta = *in.frameDelta;   // the host's record (Engine::fixedFrameDelta)
    if (in.cause) p.cause = in.cause;
    if (in.shadowMapBudget) p.shadowMapBudget = in.shadowMapBudget;
    p.refreshShadows = p.refreshShadows || in.refreshShadows;
    if (in.stepSerial) p.stepSerial = in.stepSerial;
    for (FrameInputScene &sc : in.scenes) {
        auto it = std::find_if(p.scenes.begin(), p.scenes.end(),
                               [&](const FrameInputScene &q) { return q.scene == sc.scene; });
        if (it == p.scenes.end()) { p.scenes.push_back(std::move(sc)); continue; }
        // THE CHANGE SET APPENDS (events, not values): both submits' writes are this frame's.
        it->changes.complete = it->changes.complete && sc.changes.complete;
        it->changes.moved.insert(it->changes.moved.end(), sc.changes.moved.begin(), sc.changes.moved.end());
        it->nodes.insert(it->nodes.end(), sc.nodes.begin(), sc.nodes.end());   // events, in order
        it->messages.insert(it->messages.end(), std::make_move_iterator(sc.messages.begin()),
                            std::make_move_iterator(sc.messages.end()));   // commands, in order
        // A scene's part is REPLACED by the newest, FIELD BY FIELD: every field the newer record carries wins, and
        // one it does not carry keeps the older record's value (a sparser second sync never drops the first's).
        if (sc.shadows) it->shadows = sc.shadows;
        if (sc.environmentLight) it->environmentLight = sc.environmentLight;
        if (sc.fog) it->fog = sc.fog;
        if (sc.heightFog) it->heightFog = sc.heightFog;
        if (sc.rayTracing) it->rayTracing = sc.rayTracing;
        if (sc.sunContact) it->sunContact = sc.sunContact;
        if (sc.gi) it->gi = sc.gi;
        if (sc.planar) it->planar = sc.planar;
        if (sc.sky) it->sky = sc.sky;
        if (sc.shaderTime) it->shaderTime = sc.shaderTime;
        if (sc.grid) it->grid = sc.grid;
        it->reassert = it->reassert || sc.reassert;
        if (sc.giRefreshSerial) it->giRefreshSerial = sc.giRefreshSerial;
        it->materialPreview = it->materialPreview || sc.materialPreview;
        if (sc.gesture) it->gesture = sc.gesture;
        ++mSubmit.scenePartsMerged;
    }
    for (const FrameInputView &v : in.views) {
        // WHAT THE HOST PUSHED, kept on its side (View::pushed*, ENGINE-UPGRADE-U3 stage 2 F5): a part a host builds
        // on top of its own last push reads it, never the engine's applied state.
        if (v.view) static_cast<OgreView *>(v.view)->notePushed(v);
        auto it = std::find_if(p.views.begin(), p.views.end(),
                               [&](const FrameInputView &q) { return q.view == v.view; });
        if (it == p.views.end()) { p.views.push_back(v); continue; }
        // A view's fields MERGE: the newest value of each, a reset requested by either.
        if (v.camera) it->camera = v.camera;
        if (v.cameraNode) it->cameraNode = v.cameraNode;
        if (v.postFx) it->postFx = v.postFx;
        if (v.shadows) it->shadows = v.shadows;
        if (v.sampleCount) it->sampleCount = v.sampleCount;
        if (v.background) it->background = v.background;
        if (v.scene) { it->scene = v.scene; it->sceneSeq = v.sceneSeq; }
        if (v.enabled) it->enabled = v.enabled;
        if (v.offscreenContract) it->offscreenContract = v.offscreenContract;
        if (v.helpersVisible) it->helpersVisible = v.helpersVisible;
        if (v.vrHelpersVisible) it->vrHelpersVisible = v.vrHelpersVisible;
        if (v.asyncShaders) it->asyncShaders = v.asyncShaders;
        if (v.overlay) it->overlay = v.overlay;
        if (v.pip) it->pip = v.pip;
        if (v.lodHysteresisOffscreen) it->lodHysteresisOffscreen = v.lodHysteresisOffscreen;
        if (v.size) it->size = v.size;
        it->resetExposure = it->resetExposure || v.resetExposure;
        if (v.seedExposure) it->seedExposure = v.seedExposure;
    }
}


void RenderThread::noteHostStage(const std::string &name, float ms) {
    std::lock_guard<std::mutex> lock(mSlotLock);
    std::vector<FrameStage> &st = mSlot.hostStages;
    if (st.size() >= kHostStageCoalesce) {
        for (FrameStage &s : st)
            if (s.name == name) { s.ms += ms; return; }
    }
    st.push_back({ name, ms });
}

FrameSubmitStatus RenderThread::submitStatus() const {
    std::lock_guard<std::mutex> lock(mSlotLock);
    FrameSubmitStatus st = mSubmit;
    st.nextSerial = mSerial + (mSlot.any ? 1ull : 0ull);   // mSerial moves only under this lock (handOver)
    st.pending = mSlot.any;
    return st;
}

// ---- THE RENDER SIDE ---------------------------------------------------------------------------------------------

// THE HAND-OVER (U3 F1/F2): the whole slot, once, at the frame head. The serial is stamped HERE — one per frame that
// applies a record, never one per submit — and an empty slot is a legal hand-over that carries the last serial: the
// frame draws the last applied state with the engine's own frameDelta, exactly as a frame nobody submitted for always
// did. A submit made after this point (a trailing settle's, a photo's) is the next hand-over's.
RenderThread::Slot RenderThread::handOver() {
    std::lock_guard<std::mutex> lock(mSlotLock);
    Slot out = std::move(mSlot);
    mSlot = Slot();
    ++mSubmit.handOvers;
    mHandOverNow = mSubmit.handOvers;
    // THE RECORD'S LATENESS IN FRAMES (decision 50, editor.drag_latency): the hand-overs since its first submit — 1 is
    // the next frame begun; more would be a record held back past a frame that began without it.
    if (out.any) {
        const unsigned long long late = mSubmit.handOvers - out.firstAt;
        ++mSubmit.recordsHanded;
        if (late > 1ull) ++mSubmit.recordsLate;
        if (late > mSubmit.worstLatency) mSubmit.worstLatency = late;
    }
    if (out.any) ++mSerial;
    out.record.serial = mSerial;
    return out;
}

// ---- THE THREAD (U3 stage 4b) -----------------------------------------------------------------------------------

bool RenderThread::start(std::function<bool()> boot) {
    mHostThread = std::this_thread::get_id();   // the host's thread: the one that started the engine
    gRenderThreaded.store(true, std::memory_order_relaxed);
    bool ok = false;
    std::mutex bootLock;
    std::condition_variable bootCv;
    bool booted = false;
    mThread = std::thread([&, boot = std::move(boot)] {
        gRenderThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
        {
            EngineSideScope engineSide;
            ok = boot();
        }
        {   // NOTIFIED UNDER THE LOCK (decision 45, D4): start() cannot return and take the condition with it first.
            std::lock_guard<std::mutex> l(bootLock);
            booted = true;
            bootCv.notify_all();
        }
        threadMain();
    });
    std::unique_lock<std::mutex> l(bootLock);
    bootCv.wait(l, [&] { return booted; });
    return ok;
}

void RenderThread::threadMain() {
    for (;;) {
        std::function<void()> task;
        bool freeFrame = false;
        {
            std::unique_lock<std::mutex> l(mTaskLock);
            // FREE-RUNNING (U3 stage 5): with no task waiting the render thread draws on its own — a display frame
            // of the newest record, paced by the present (the vsync acquire) and, where the present does not block,
            // by the display period the driver supplies. A task (a request, a join, a lockstep frame) goes between two.
            mTaskCv.wait(l, [this] { return mTaskPending || mQuit || mFreeRun.load(std::memory_order_relaxed); });
            if (!mTaskPending && mQuit) return;
            if (mTaskPending) task = std::move(mTask);
            else freeFrame = true;
        }
        if (freeFrame) {
            if (mTornDown) { mFreeRun.store(false, std::memory_order_relaxed); continue; }
            const auto t0 = std::chrono::steady_clock::now();
            try { tickWith(handOver(), true); } catch (...) {}
            // THE PACE WHERE THE PRESENT DID NOT BLOCK (an offscreen session, vsync off): the rest of the display
            // period, waited on the task condition so a task still goes at once.
            const double period = double(mDisplayPeriod.load(std::memory_order_relaxed));
            const double spent = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (period > 0.0 && spent < period) {
                std::unique_lock<std::mutex> l(mTaskLock);
                mTaskCv.wait_for(l, std::chrono::duration<double>(period - spent),
                                 [this] { return mTaskPending || mQuit || !mFreeRun.load(std::memory_order_relaxed); });
            }
            continue;
        }
        // A THROW ON THE RENDER THREAD IS THE ASKER'S (decision 45, D3): caught into the slot and rethrown on the waiting
        // thread by runOn — never std::terminate.
        std::exception_ptr thrown;
        if (task) {
            try { task(); }
            catch (...) { thrown = std::current_exception(); }
        }
        bool quit = false;
        {
            std::lock_guard<std::mutex> l(mTaskLock);
            mTaskPending = false;
            mTask = nullptr;
            mTaskThrown = thrown;
            quit = mQuit;
        }
        mTaskCv.notify_all();
        if (quit) return;
    }
}

void RenderThread::runOn(const std::function<void()> &task) {
    if (onRenderThread()) {
        task();
        if (tlsEngineSide == 0) latchSnapshots();   // inline (no thread): the caller's reads see the task's publish
        return;
    }
    std::unique_lock<std::mutex> l(mTaskLock);
    mTaskCv.wait(l, [this] { return !mTaskPending; });   // one task at a time (a second host thread queues)
    mTaskThrown = nullptr;   // a kicked frame's throw before this is the frame's (the kick has no asker)
    // THE PUMP IS THE HOST'S THREAD'S (decision 46): another thread that waits (a worker's request) never repaints it.
    const bool pump = mHostPump && std::this_thread::get_id() == mHostThread;
    mTask = task;
    mTaskPending = true;
    mTaskCv.notify_all();
    // THE HOST WAITS — and keeps its own side alive while it does (the splash's progress inside a long frame).
    while (mTaskPending) {
        mTaskCv.wait_for(l, std::chrono::milliseconds(10), [this] { return !mTaskPending; });
        if (mTaskPending && pump) { l.unlock(); mHostPump(); l.lock(); }
    }
    std::exception_ptr thrown = std::exchange(mTaskThrown, nullptr);
    l.unlock();
    latchSnapshots();   // what the task published is what the asker reads next (lockstep: every task)
    if (pump) mHostPump();
    if (thrown) std::rethrow_exception(thrown);
}

void RenderThread::stop(const std::function<void()> &teardown) {
    // NO FREE FRAME AFTER THIS (stage 5): the teardown is the render thread's last task, and nothing draws after it.
    mFreeRun.store(false, std::memory_order_relaxed);
    if (!threaded()) { if (teardown) teardown(); return; }
    runOn([&] { EngineSideScope engineSide; if (teardown) teardown(); mTornDown = true; });
    {
        std::lock_guard<std::mutex> l(mTaskLock);
        mQuit = true;
    }
    mTaskCv.notify_all();
    mThread.join();
    gRenderThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
    gRenderThreaded.store(false, std::memory_order_relaxed);
}

void RenderThread::tickInline() { tickWith(handOver(), false); }

void RenderThread::latchSnapshots() {
    latchAllSnapshots();   // EAGER (decision 54): every snapshot's newest frame, one frame for all of them
}

void RenderThread::setFreeRunning(bool on) {
    if (mFreeRun.exchange(on, std::memory_order_relaxed) == on) return;
    std::lock_guard<std::mutex> l(mTaskLock);
    mTaskCv.notify_all();
}

void RenderThread::tickWith(Slot &&handed, bool freeFrame) {
    mEngine.mFreeFrame = freeFrame;   // the interpolation's switch (OgreEngine::runFrame): a lockstep frame draws cur
    // THE HOST'S HALF OF THE MONITOR'S RECORD, folded in before the frame opens (the next beginFrame adopts what is
    // banked between frames, so it leads the stage list as it always did). Dropped when no capture runs, as the
    // host's stages always were.
    if (monitor::live())
        for (const FrameStage &st : handed.hostStages) monitor::gMonitor->hostStage(st.name, st.ms);
    // THE FRAME'S ERROR IS THE FRAME'S (U3 F3): the sink — every Scene's and View's error string is a reference to it
    // — is swapped aside for the length of the frame, so what the frame writes (its drain, its work, its close) is
    // the frame's error and goes to the snapshot, and the CALLS' error the host may still be about to read comes
    // back untouched.
    // THE RENDER SIDE from here to the publish: the engine's own reads of its state are not the host's door.
    EngineSideScope engineSide;
    std::string callError;
    callError.swap(mEngine.mLastError);
    // A THROW INSIDE THE FRAME IS THE FRAME'S PUBLISHED ERROR (worth-a-look 2), never swallowed: the frame that
    // threw publishes, its error text the frame's.
    bool ran = false;
    try { ran = mEngine.runFrame(handed.record, handed.any); }
    catch (Ogre::Exception &x) { mEngine.mLastError = describeOgreFailure(x); ran = true; }
    catch (std::exception &x) { mEngine.mLastError = std::string("engine: ") + x.what(); ran = true; }
    catch (...) { mEngine.mLastError = "engine: an exception of an unknown type left the frame"; ran = true; }
    std::string frameError;
    frameError.swap(mEngine.mLastError);
    mEngine.mLastError.swap(callError);
    if (ran) publish(handed.any, std::move(frameError));
}

// THE PUBLISH (U2's whole-frame swap, now the loop's — U3 F7): after the frame's close, so the snapshot carries what
// the close latched (a lost device, an out-of-memory, a session the runtime ended). Every scene's half first (its
// rider tags, its GI machine), then the engine's.
void RenderThread::publish(bool recordApplied, std::string &&frameError) {
    const auto t0 = std::chrono::steady_clock::now();
    OgreEngine &e = mEngine;
    ++e.mPublishedFrames;
    for (auto &s : e.mScenes) {
        if (!s) continue;
        try { s->publishFrame(e.mPublishedFrames, mSerial); }
        catch (Ogre::Exception &x) { frameError = describeOgreFailure(x); }
        catch (std::exception &x) { frameError = std::string("engine: ") + x.what(); }
    }
    PublishedEngineFrame &back = mPublished.back();
    try { e.fillPublished(back); }
    catch (Ogre::Exception &x) { frameError = describeOgreFailure(x); }
    catch (std::exception &x) { frameError = std::string("engine: ") + x.what(); }
    back.frame = e.mPublishedFrames;
    back.serial = mSerial;
    back.recordApplied = recordApplied;
    back.handOver = mHandOverNow;
    if (!frameError.empty()) {
        ++mFramesWithError;
        mFrameErrors.push_back({ mSerial, e.mPublishedFrames, frameError });
        if (mFrameErrors.size() > PublishedEngineFrame::kFrameErrorsKept) mFrameErrors.erase(mFrameErrors.begin());
    }
    back.frameError = std::move(frameError);
    back.frameErrors = mFrameErrors;
    back.framesWithError = mFramesWithError;
    back.publishMicros = mPublishMicros;
    mPublishMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    // THE FRAME'S WHOLE SET AT ONCE (decision 54): every scene's buffer and the engine's exchanged under the one lock
    // the host's latch takes — a latch sees one frame everywhere.
    std::lock_guard<std::mutex> l(snapshotLock());
    for (auto &s : e.mScenes) if (s) s->commitPublishedLocked();
    mPublished.commitLocked();
}

// THE ENGINE'S HALF RE-PUBLISHED AFTER A REQUEST (U3 stage 4b): what the request applied (the pending record's
// structure, the question's own change — a monitor started or stopped) is in the snapshot the asker reads next, the
// frame's stamp unchanged (no frame was drawn).
void RenderThread::republishEngine() {
    const PublishedEngineFrame &front = mPublished.newest();
    PublishedEngineFrame &back = mPublished.back();
    try { mEngine.fillPublished(back); }
    catch (...) { return; }
    back.frame = front.frame;
    back.serial = front.serial;
    back.recordApplied = front.recordApplied;
    back.handOver = front.handOver;
    back.frameError = front.frameError;
    back.frameErrors = mFrameErrors;
    back.framesWithError = mFramesWithError;
    back.publishMicros = front.publishMicros;
    mPublished.commit();
}

}}}   // namespace jahshaka::engine::detail
