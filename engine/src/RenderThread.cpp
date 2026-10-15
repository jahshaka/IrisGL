// THE ENGINE'S RENDER LOOP (ENGINE-UPGRADE-U3 stage 1, the seam) — RenderThread.h says what it owns and why.
#include "EnginePrivate.h"

#include <algorithm>
#include <chrono>

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

// ---- THE GAME SIDE -----------------------------------------------------------------------------------------------

// THE MERGE STAYS (U3 F1): one frame today is the merge of 2-6 submits — the driver's cause, the viewport's pace, the
// mirror's record, the viewport's delta + gesture, a preview's scene part — and that merge IS the record's builder.
// What changed is where it lands: the hand-over slot, under its lock, never the record a frame is draining.
void RenderThread::submit(const FrameInput &in) {
    std::lock_guard<std::mutex> lock(mSlotLock);
    ++mSubmit.submitted;
    mSlot.any = true;
    FrameInput &p = mSlot.record;
    if (in.frameDelta) p.frameDelta = in.frameDelta;
    if (in.frameDelta) mEngine.mPushedFrameDelta = *in.frameDelta;   // the host's record (Engine::fixedFrameDelta)
    if (in.cause) p.cause = in.cause;
    if (in.pace) p.pace = in.pace;
    if (in.shadowMapBudget) p.shadowMapBudget = in.shadowMapBudget;
    p.refreshShadows = p.refreshShadows || in.refreshShadows;
    if (in.stepSerial) p.stepSerial = in.stepSerial;
    for (const FrameInputScene &sc : in.scenes) {
        auto it = std::find_if(p.scenes.begin(), p.scenes.end(),
                               [&](const FrameInputScene &q) { return q.scene == sc.scene; });
        if (it == p.scenes.end()) { p.scenes.push_back(sc); continue; }
        // THE CHANGE SET APPENDS (events, not values): both submits' writes are this frame's.
        it->changes.complete = it->changes.complete && sc.changes.complete;
        it->changes.moved.insert(it->changes.moved.end(), sc.changes.moved.begin(), sc.changes.moved.end());
        it->nodes.insert(it->nodes.end(), sc.nodes.begin(), sc.nodes.end());   // events, in order
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
    if (out.any) ++mSerial;
    out.record.serial = mSerial;
    return out;
}

void RenderThread::tickInline() {
    Slot handed = handOver();
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
    const bool ran = mEngine.runFrame(handed.record, handed.any);
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
    PublishedEngineFrame &back = mPublished[mPublishedIdx.load(std::memory_order_relaxed) ^ 1u];
    try { e.fillPublished(back); }
    catch (Ogre::Exception &x) { frameError = describeOgreFailure(x); }
    catch (std::exception &x) { frameError = std::string("engine: ") + x.what(); }
    back.frame = e.mPublishedFrames;
    back.serial = mSerial;
    back.recordApplied = recordApplied;
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
    mPublishedIdx.store(mPublishedIdx.load(std::memory_order_relaxed) ^ 1u, std::memory_order_release);
}

}}}   // namespace jahshaka::engine::detail
