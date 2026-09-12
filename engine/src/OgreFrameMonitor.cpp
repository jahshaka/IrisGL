// THE RENDER-LOOP MONITOR, engine half (SPECS/RENDER_LOOP_MONITOR_SPEC.md).
//
// WHAT THIS IS, in the owner's words (2026-09-12): "the purpose of the monitor
// is to collect as much data as possible for you to be able to review the core
// engine and how it's running, to look for issues and problems." It is a DATA
// COLLECTOR, and the emphasis is on *collector*:
//
//  * IT JUDGES NOTHING. No budgets, no thresholds, no "wasted" verdicts. Work a
//    cache redid with no recorded input change is recorded as work whose reason
//    is `WorkReason::None`; whether that is a defect is the lead's reading of a
//    capture, not a computation in here (owner decision D5).
//  * IT DRAWS NOTHING. There is no on-screen display (owner, 2026-09-12: "we
//    don't need an on-screen display, it will interfere with monitoring"), so
//    the monitor cannot contaminate what it measures with passes of its own.
//  * IT REMEMBERS NOTHING UNTIL ASKED. Capture is FORWARD ONLY: nothing is
//    recorded before the host turns it on, and at Off there is no ring, no
//    listener, no clock read and no GPU query pool — four facts
//    `monitorStatus()` reports so a suite asserts them instead of trusting them.
//
// WHAT IT REPLACES: `chain::PassProfiler` (`--profile` / `app.profiling`), which
// kept ONE start time for passes that nest — every scene pass that owns a shadow
// node executes that node's passes between its own pre/post callbacks, so the
// numbers it printed were the last child's, not the pass's — and which logged
// every C++-built shadow pass as "(unnamed pass)" because those definitions
// carry no profiling id. Deleted with this file's arrival (CRUD law, D4).
//
// THREADING: engine calls are UI-thread-only by contract, so nothing here is
// atomic and nothing is locked.
#include "EnginePrivate.h"

#include <algorithm>
#include <cstring>

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNode.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreCamera.h>
#include <OgreLight.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreTextureGpuManager.h>

namespace jahshaka { namespace engine { namespace detail { namespace monitor {

FrameMonitor *gMonitor = nullptr;

namespace {
/// The capture's zero. Set when the monitor is switched on, so every `startMs`
/// in a bundle is capture-relative and two bundles are comparable.
std::chrono::steady_clock::time_point gEpoch = std::chrono::steady_clock::now();

/// PASS TYPE NAMES. Ogre declares `CompositorPassTypeEnumNames` but does not
/// export it from OgreMain (no _OgreExport on the extern), so it is not
/// linkable from here — recorded as a pin finding. This table is the same
/// order as the enum and is asserted against its size below.
const char *kPassTypeNames[] = { "invalid",   "scene",  "quad",      "clear",
                                 "stencil",   "resolve","depth_copy","uav",
                                 "mipmap",    "ibl_specular", "shadows",
                                 "target_barrier", "warm_up", "compute", "custom" };
static_assert(sizeof(kPassTypeNames) / sizeof(kPassTypeNames[0]) == Ogre::PASS_CUSTOM + 1u,
              "Ogre's CompositorPassType grew: extend kPassTypeNames to match");
const char *passTypeName(Ogre::CompositorPassType t) {
    const unsigned i = unsigned(t);
    return i < sizeof(kPassTypeNames) / sizeof(kPassTypeNames[0]) ? kPassTypeNames[i] : "?";
}

/// The three shadow nodes, as IdStrings — hashed once, compared per pass.
const Ogre::IdString &shadowNodeId(ShadowNodeKind k) {
    static const Ogre::IdString ids[kShadowNodeKinds] = {
        Ogre::IdString(OgreView::kShadowNodeName),
        Ogre::IdString(OgreView::kReflectShadowNodeName),
        Ogre::IdString(OgreView::kProbeShadowNodeName)
    };
    return ids[unsigned(k)];
}

/// RenderingMetrics is reset once per frame by the render system; the monitor
/// only ever READS it, so nothing here changes what any other reader sees.
/// (Recording is already on for the process — `renderStats()` turns it on
/// lazily and never off — so this adds no cost of its own.)
void readMetrics(Ogre::RenderSystem *rs, unsigned &draws, unsigned &batches,
                 unsigned long long &tris, unsigned &instances) {
    draws = batches = instances = 0u;
    tris = 0ull;
    if (!rs) return;
    const Ogre::RenderingMetrics &m = rs->getMetrics();
    draws     = unsigned(m.mDrawCount);
    batches   = unsigned(m.mBatchCount);
    tris      = (unsigned long long)m.mFaceCount;
    instances = unsigned(m.mInstanceCount);
}
}   // namespace

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - gEpoch)
        .count();
}
void resetEpoch() { gEpoch = std::chrono::steady_clock::now(); }

// ---------------------------------------------------------------------------
// FrameMonitor
// ---------------------------------------------------------------------------

FrameMonitor::FrameMonitor() {
    mRing.reserve(kRingCapacity);
    mEvents.reserve(256u);
    mPassStack.reserve(16u);
}

void FrameMonitor::beginFrame(unsigned long long frame, FrameCause cause, bool onscreen) {
    const auto t0 = std::chrono::steady_clock::now();
    mCurrent = FrameRecord();
    mCurrent.frame = frame;
    mCurrent.cause = cause;
    mCurrent.onscreen = onscreen;
    mCurrent.startMs = nowMs();
    mFrameStart = t0;
    mInFrame = true;
    mStageChild = nullptr;
    mPassStack.clear();
    mWorkspaceDepths.clear();
    mPassSampleIds.clear();
    // Host stages pushed before the frame opened (the driver's tick wraps the
    // engine's frame, so `tick` and the mirror's sub-stages are known first)
    // lead the stage list.
    mCurrent.stages.swap(mPendingHostStages);
    mPendingHostStages.clear();
    adoptPendingCacheWork();
    mOverheadMs += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
}

void FrameMonitor::endFrame(unsigned scenesUpdated) {
    if (!mInFrame) return;
    const auto t = std::chrono::steady_clock::now();
    mCurrent.totalMs = float(std::chrono::duration<double, std::milli>(t - mFrameStart).count());
    mCurrent.scenesUpdated = scenesUpdated;
    // The counters the pass records imply, summed here so nothing downstream
    // has to re-derive them and `frames.jsonl` reads on its own.
    for (const FramePass &p : mCurrent.passes) {
        mCurrent.draws     += p.draws;
        mCurrent.batches   += p.batches;
        mCurrent.instances += p.instances;
        mCurrent.triangles += p.triangles;
        switch (p.bucket) {
        case PassBucket::ShadowView:    ++mCurrent.shadowPasses; break;
        case PassBucket::ShadowReflect: ++mCurrent.shadowPassesReflect; break;
        case PassBucket::ShadowProbe:   ++mCurrent.shadowPassesProbe; break;
        default: break;
        }
        if (p.orphaned) ++mCurrent.orphanedPasses;
    }
    mOverheadMs += std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t).count();
    mCurrent.overheadMs = float(mOverheadMs);
    mLastOverheadMs = float(mOverheadMs);
    mOverheadMs = 0.0;
    // THE HOLDING QUEUE (P1c). A GPU sample comes back two frames late, so the
    // record waits here until its samples arrive or it ages out; without GPU
    // sampling the queue is one deep and the record is published immediately.
    PendingFrame pf;
    pf.rec = std::move(mCurrent);
    pf.passSampleIds.swap(mPassSampleIds);
    if (mGpu) {
        const unsigned slot = unsigned(mPending.size());
        for (unsigned i = 0; i < pf.passSampleIds.size(); ++i)
            if (pf.passSampleIds[i]) mGpuSampleIndex[pf.passSampleIds[i]] = { slot, i };
    }
    mPending.push_back(std::move(pf));
    retirePending(false);
    mCurrent = FrameRecord();
    mInFrame = false;
    ++mFramesRecorded;
}

void FrameMonitor::noteGpuSample(unsigned sampleId, float ms) {
    auto it = mGpuSampleIndex.find(sampleId);
    if (it == mGpuSampleIndex.end()) return;     // its frame already aged out
    const unsigned slot = it->second.first, pass = it->second.second;
    if (slot < mPending.size() && pass < mPending[slot].rec.passes.size())
        mPending[slot].rec.passes[pass].gpuMs = ms;
    mGpuSampleIndex.erase(it);
}

void FrameMonitor::retirePending(bool all) {
    const size_t keep = (all || !mGpu) ? 0u : size_t(kGpuLatencyFrames);
    while (mPending.size() > keep) {
        PendingFrame pf = std::move(mPending.front());
        mPending.pop_front();
        // The frame's GPU total, from whatever came back. NEGATIVE stays
        // negative: a pass with no sample is "not measured", never zero.
        for (const FramePass &p : pf.rec.passes)
            if (p.gpuMs >= 0.0f) {
                if (pf.rec.gpuMs < 0.0f) pf.rec.gpuMs = 0.0f;
                pf.rec.gpuMs += p.gpuMs;
            }
        for (unsigned id : pf.passSampleIds) mGpuSampleIndex.erase(id);
        push(std::move(pf.rec));
        // Every surviving frame moved down one slot.
        for (auto &kv : mGpuSampleIndex)
            if (kv.second.first > 0u) --kv.second.first;
    }
}

void FrameMonitor::push(FrameRecord &&r) {
    if (mRing.size() < kRingCapacity) { mRing.push_back(std::move(r)); return; }
    mRing[mWrite] = std::move(r);
    mWrite = (mWrite + 1u) % kRingCapacity;
    ++mFramesDropped;
}

unsigned FrameMonitor::drainFrames(std::vector<FrameRecord> &out) {
    retirePending(false);
    if (mRing.empty()) return 0u;
    const unsigned n = unsigned(mRing.size());
    // OLDEST FIRST. The ring is either still in order (never wrapped) or split
    // at the write cursor.
    for (unsigned i = 0; i < n; ++i) out.push_back(std::move(mRing[(mWrite + i) % n]));
    mRing.clear();
    mRing.reserve(kRingCapacity);
    mWrite = 0u;
    return n;
}

unsigned FrameMonitor::drainEvents(std::vector<MonitorEvent> &out) {
    const unsigned n = unsigned(mEvents.size());
    for (MonitorEvent &e : mEvents) out.push_back(std::move(e));
    mEvents.clear();
    return n;
}

void FrameMonitor::closeOrphanPass() {
    if (mPassStack.empty()) return;
    PassFrame f = std::move(mPassStack.back());
    mPassStack.pop_back();
    // The pass never reported a `passPosExecute`, so its numbers are unknown —
    // recorded as such (negative CPU time, no draw counts) rather than invented,
    // and its GPU sample is closed so the render system's own stack stays
    // balanced. Seeing one of these in a capture IS the finding.
    if (mGpu && f.gpuSampleId && mOrphanRs) {
        try { mOrphanRs->endGPUSampleProfile(f.rec.pass); } catch (...) {}
    }
    f.rec.cpuMs = -1.0f;
    f.rec.orphaned = true;
    pass(std::move(f.rec), f.gpuSampleId);
}

/// Banks a stage that arrived between frames. Past kPendingStageCoalesce the
/// list stops growing and folds by NAME (see the constant): lossless in total
/// time, bounded in size, and never silent.
void FrameMonitor::bankPending(const std::string &name, float ms) {
    if (mPendingHostStages.size() >= kPendingStageCoalesce) {
        for (FrameStage &s : mPendingHostStages)
            if (s.name == name) { s.ms += ms; return; }
    }
    mPendingHostStages.push_back({ name, ms });
}
void FrameMonitor::stage(const char *name, double ms) {
    if (mInFrame) mCurrent.stages.push_back({ name, float(ms) });
    else          bankPending(name, float(ms));
}
void FrameMonitor::hostStage(const std::string &name, float ms) {
    if (mInFrame) mCurrent.stages.push_back({ name, ms });
    else          bankPending(name, ms);
}
void FrameMonitor::cacheWork(const CacheWork &w) {
    if (mInFrame) mCurrent.cacheWork.push_back(w);
    else          mPendingCacheWork.push_back(w);
}
void FrameMonitor::pass(FramePass &&p, unsigned gpuSampleId) {
    if (!mInFrame) return;
    mCurrent.passes.push_back(std::move(p));
    if (mGpu) mPassSampleIds.push_back(gpuSampleId);
}
void FrameMonitor::event(MonitorEvent &&e) {
    if (mEvents.size() >= kEventCapacity) { ++mEventsDropped; return; }
    if (e.frame == 0) e.frame = mCurrent.frame;
    if (e.startMs == 0.0) e.startMs = nowMs();
    mEvents.push_back(std::move(e));
}
void FrameMonitor::adoptPendingCacheWork() {
    for (CacheWork &w : mPendingCacheWork) mCurrent.cacheWork.push_back(std::move(w));
    mPendingCacheWork.clear();
}

bool FrameSplitListener::frameRenderingQueued(const Ogre::FrameEvent &) {
    mMark = std::chrono::steady_clock::now();
    mMarked = true;
    return true;      // never veto a frame; the monitor changes nothing
}

// ---------------------------------------------------------------------------
// Stage
// ---------------------------------------------------------------------------

Stage::Stage(const char *name) {
    if (!gMonitor) return;
    mName = name;
    mStart = std::chrono::steady_clock::now();
    mParentChild = gMonitor->mStageChild;
    gMonitor->mStageChild = &mChild;
}

Stage::~Stage() {
    if (!mName || !gMonitor) return;
    const double total = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - mStart).count();
    gMonitor->mStageChild = mParentChild;
    if (mParentChild) *mParentChild += total;
    gMonitor->stage(mName, total - mChild);   // EXCLUSIVE of nested stages
}

// ---------------------------------------------------------------------------
// PassListener — per-pass records, exclusive by construction
// ---------------------------------------------------------------------------

// THE STACK IS TRIMMED TO A REMEMBERED DEPTH, NOT CLEARED.
//
// A workspace update can happen INSIDE an open pass — a reflection probe
// captures from `allWorkspacesBeginUpdate` and a planar mirror renders its
// workspaces synchronously from `passEarlyPreExecute`, and neither is nested
// inside one of our pass records today. But clearing the whole stack on any
// workspace boundary means the first nested update that IS destroys the outer
// PassFrame: its record is lost, its parent's exclusive time and draw counts
// are wrong, and — worse — its `endGPUSampleProfile` is never called, which
// leaves a sample open on the render system's own stack and mis-attributes
// every GPU time after it (the overflow-sentinel class of bug, from the other
// end). Remembering the depth on the way in and trimming back to it on the way
// out keeps an enclosing pass intact and still guarantees that a workspace
// which threw mid-update cannot leak frames into the next one.
void PassListener::workspacePreUpdate(Ogre::CompositorWorkspace *ws) {
    (void)ws;
    if (!gMonitor) return;
    gMonitor->mWorkspaceDepths.push_back(unsigned(gMonitor->mPassStack.size()));
}

void PassListener::workspacePosUpdate(Ogre::CompositorWorkspace *ws) {
    (void)ws;
    if (!gMonitor) return;
    unsigned depth = 0u;
    if (!gMonitor->mWorkspaceDepths.empty()) {
        depth = gMonitor->mWorkspaceDepths.back();
        gMonitor->mWorkspaceDepths.pop_back();
    }
    // Anything this workspace left open is closed here, in order, so the render
    // system never carries an unbalanced GPU sample into the next workspace.
    while (gMonitor->mPassStack.size() > depth) gMonitor->closeOrphanPass();
}

void PassListener::passPreExecute(Ogre::CompositorPass *pass) {
    if (!gMonitor || !pass) return;
    const auto t0 = std::chrono::steady_clock::now();
    const Ogre::CompositorNode *node = pass->getParentNode();
    Ogre::RenderSystem *rs = node ? node->getRenderSystem() : nullptr;

    gMonitor->mOrphanRs = rs;
    FrameMonitor::PassFrame f;
    f.start = t0;
    readMetrics(rs, f.drawsAt, f.batchesAt, f.trianglesAt, f.instancesAt);

    const Ogre::CompositorPassDef *def = pass->getDefinition();
    const Ogre::CompositorNodeDef *ndef = node ? node->getDefinition() : nullptr;
    f.rec.node = ndef ? ndef->getNameStr() : std::string();
    if (node && node->getWorkspace()) {
        const Ogre::CompositorWorkspaceDef *wdef =
            const_cast<Ogre::CompositorWorkspace *>(node->getWorkspace())->getDefinition();
        if (wdef) f.rec.workspace = wdef->getNameStr();
    }
    bool sceneKind = false;
    if (def) {
        const Ogre::CompositorPassType type = def->getType();
        sceneKind = type == Ogre::PASS_SCENE;
        f.rec.pass = !def->mProfilingId.empty()
                         ? def->mProfilingId
                         : std::string(passTypeName(type));
        if (def->mShadowMapIdx != ~Ogre::uint32(0)) f.rec.shadowMapIdx = def->mShadowMapIdx;
    }
    // THE BUCKET is read from the parent NODE, not inferred from timings: the
    // three shadow nodes are named constants, so "this is a probe's shadow map
    // pass" is a fact and not a guess.
    f.rec.bucket = PassBucket::Other;
    if (node) {
        const Ogre::IdString id = node->getName();
        if (id == shadowNodeId(ShadowNodeKind::View))         f.rec.bucket = PassBucket::ShadowView;
        else if (id == shadowNodeId(ShadowNodeKind::Reflect)) f.rec.bucket = PassBucket::ShadowReflect;
        else if (id == shadowNodeId(ShadowNodeKind::Probe))   f.rec.bucket = PassBucket::ShadowProbe;
        else f.rec.bucket = sceneKind ? PassBucket::Main : PassBucket::Post;
    }
    if (sceneKind) f.rec.shadowMs = 0.0f;   // a scene pass answers the split; see below
    // THE GPU SAMPLE (P1c). Ogre's own profiler is the only upstream caller of
    // these hooks and it is compiled out here (OGRE_PROFILING = 0), so the
    // monitor calls them itself: one sample per pass, nested exactly like the
    // CPU stack. Costs two vkCmdWriteTimestamp calls; nothing is read back.
    if (gMonitor->mGpu && rs) {
        f.gpuSampleId = gMonitor->nextGpuSampleId();
        if (f.gpuSampleId) {
            unsigned hash = f.gpuSampleId;
            try { rs->beginGPUSampleProfile(f.rec.pass, &hash); } catch (...) {}
        }
    }
    gMonitor->mPassStack.push_back(std::move(f));
    gMonitor->addOverhead(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count());
}

void PassListener::passSceneAfterShadowMaps(Ogre::CompositorPassScene *pass) {
    // THE SHADOW/SCENE SPLIT INSIDE ONE SCENE PASS. Ogre fires this after the
    // pass's shadow node has updated (and fires it even when there is no shadow
    // node), between passPreExecute and the scene render — so the time from the
    // pass's start to here IS the shadow half, nested pass records included.
    // Zero is the interesting value: a scene pass whose lamps are all cached
    // executes no shadow pass at all.
    (void)pass;
    if (!gMonitor || gMonitor->mPassStack.empty()) return;
    FrameMonitor::PassFrame &f = gMonitor->mPassStack.back();
    f.rec.shadowMs = float(std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - f.start).count());
}

void PassListener::passPosExecute(Ogre::CompositorPass *pass) {
    if (!gMonitor || gMonitor->mPassStack.empty() || !pass) return;
    const auto t1 = std::chrono::steady_clock::now();
    FrameMonitor::PassFrame f = std::move(gMonitor->mPassStack.back());
    gMonitor->mPassStack.pop_back();

    const Ogre::CompositorNode *node = pass->getParentNode();
    Ogre::RenderSystem *rs = node ? node->getRenderSystem() : nullptr;
    if (gMonitor->mGpu && rs && f.gpuSampleId) {
        try { rs->endGPUSampleProfile(f.rec.pass); } catch (...) {}
    }
    unsigned draws = 0, batches = 0, instances = 0;
    unsigned long long tris = 0;
    readMetrics(rs, draws, batches, tris, instances);

    const double totalMs = std::chrono::duration<double, std::milli>(t1 - f.start).count();
    f.rec.cpuMs = float(totalMs - f.childMs);
    // EXCLUSIVE draw counts: the delta across this pass MINUS what its children
    // already reported. Summing a frame's records therefore reproduces the
    // frame's own totals exactly.
    const auto exclusive = [](unsigned long long now, unsigned long long at,
                              unsigned long long child) -> unsigned long long {
        const unsigned long long d = now >= at ? now - at : 0ull;
        return d >= child ? d - child : 0ull;
    };
    f.rec.draws     = unsigned(exclusive(draws, f.drawsAt, f.childDraws));
    f.rec.batches   = unsigned(exclusive(batches, f.batchesAt, f.childBatches));
    f.rec.instances = unsigned(exclusive(instances, f.instancesAt, f.childInstances));
    f.rec.triangles = exclusive(tris, f.trianglesAt, f.childTriangles);

    if (!gMonitor->mPassStack.empty()) {
        FrameMonitor::PassFrame &parent = gMonitor->mPassStack.back();
        parent.childMs        += totalMs;
        parent.childDraws     += f.rec.draws + f.childDraws;
        parent.childBatches   += f.rec.batches + f.childBatches;
        parent.childInstances += f.rec.instances + f.childInstances;
        parent.childTriangles += f.rec.triangles + f.childTriangles;
    }
    gMonitor->pass(std::move(f.rec), f.gpuSampleId);
    gMonitor->addOverhead(std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t1).count());
}

// ---------------------------------------------------------------------------
// The instrumentation sites' entry points
// ---------------------------------------------------------------------------

WorkReason reasonOf(GiStaleReason why) {
    switch (why) {
    case GiStaleReason::None:     return WorkReason::None;
    case GiStaleReason::Rebuild:  return WorkReason::Rebuild;
    case GiStaleReason::Refresh:  return WorkReason::Refresh;
    case GiStaleReason::Moved:    return WorkReason::Caster;
    case GiStaleReason::Light:    return WorkReason::Light;
    case GiStaleReason::Material: return WorkReason::Material;
    case GiStaleReason::Sky:      return WorkReason::Sky;
    case GiStaleReason::Ambient:  return WorkReason::Ambient;
    case GiStaleReason::Fog:      return WorkReason::Fog;
    case GiStaleReason::Mobility: return WorkReason::Mobility;
    }
    return WorkReason::None;
}

void noteCacheWork(CacheKind cache, WorkReason reason, unsigned long long id,
                   const char *detail, unsigned units, float ms) {
    if (!gMonitor) return;
    CacheWork w;
    w.cache = cache;
    w.reason = reason;
    w.id = id;
    if (detail) w.detail = detail;
    w.units = units;
    w.ms = ms;
    gMonitor->cacheWork(w);
}

void noteEvent(MonitorEventKind kind, WorkReason reason, const std::string &label,
               const std::string &detail, float ms, unsigned long long value) {
    if (!gMonitor) return;
    MonitorEvent e;
    e.kind = kind;
    e.reason = reason;
    e.label = label;
    e.detail = detail;
    e.ms = ms;
    e.value = value;
    gMonitor->event(std::move(e));
}

EventScope::EventScope(MonitorEventKind kind, WorkReason reason, const char *label,
                       const char *detail)
    : mKind(kind), mReason(reason) {
    if (!gMonitor) return;
    mLabel = label;
    mDetail = detail;
    mStart = std::chrono::steady_clock::now();
}

EventScope::~EventScope() {
    if (!mLabel || !gMonitor) return;
    noteEvent(mKind, mReason, mLabel, mDetail ? std::string(mDetail) : std::string(),
              float(std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - mStart).count()));
}

void noteTextureWait(float ms) {
    if (gMonitor && gMonitor->inFrame()) gMonitor->current().textureWaitMs += ms;
}
void noteShaderCompiles(unsigned n) {
    if (gMonitor && gMonitor->inFrame()) gMonitor->current().shaderCompiles += n;
}
void noteProbeCaptures(unsigned captures) {
    if (gMonitor && gMonitor->inFrame()) gMonitor->current().probeCaptures += captures;
}
void notePlanarRender(unsigned slots) {
    if (gMonitor && gMonitor->inFrame()) gMonitor->current().planarRenders += slots;
}

}   // namespace monitor

// ---------------------------------------------------------------------------
// OgreEngine — the boundary implementation
// ---------------------------------------------------------------------------

void OgreEngine::setFrameMonitor(MonitorLevel level) {
    // LEVEL-SENSITIVE, not merely on/off: `Review` is the only recording level
    // today, but a future one must not be a silent no-op when a capture is
    // already running. Same level = nothing to do; a DIFFERENT level while one
    // is live stops the current capture and starts the new one.
    const MonitorLevel current = mMonitor ? mMonitor->mLevel : MonitorLevel::Off;
    if (level == current) return;
    if (level != MonitorLevel::Off && current != MonitorLevel::Off) {
        setFrameMonitor(MonitorLevel::Off);
        setFrameMonitor(level);
        return;
    }
    if (level == MonitorLevel::Off) {
        // DOWN: detach the listener from EVERY workspace this engine owns — not
        // from `mAttached`.
        //
        // WHY NOT A REMEMBERED LIST (a use-after-free, found in review): it
        // was repopulated each frame from the DRAWN scenes only. Draw scene A
        // on frame N (its planar slots and its probes take `&mListener`),
        // switch the editor to scene B on N+1 (the list is now B's), stop the
        // capture — and A's workspaces still hold a pointer into a FrameMonitor
        // that is about to be destroyed, because it owns `mListener` by VALUE.
        // The next render of A calls into freed memory. (A workspace destroyed
        // between the last sync and this call was the second half of the same
        // bug: the list held a dangling pointer of its own.)
        //
        // Walking every scene and every view instead is free: Ogre's
        // `removeListener` is a find-and-erase, so removing from a workspace
        // that never had it costs one failed search, and what survives is a
        // COUNT (`mAttachedCount`) that is only ever reported.
        JAH_TRY {
            for (auto &v : mViews) v->removeWorkspaceListener(&mMonitor->mListener);
            for (auto &s : mScenes) {
                std::vector<Ogre::CompositorWorkspace *> ws;
                s->monitorWorkspaces(ws);
                for (Ogre::CompositorWorkspace *w : ws)
                    w->removeListener(&mMonitor->mListener);
            }
            if (mRoot) mRoot->removeFrameListener(&mMonitor->mSplit);
        } JAH_CATCH(mLastError, );
        mShaderCache.recordCompileNames(false);
        // Everything still in the holding queue is published before the ring
        // dies, minus the GPU samples that were never going to arrive — and
        // then the whole ring is handed to mFinalRecords, because a host that
        // stops the capture and THEN drains (the natural order, and what
        // Ctrl+F4 does) must not lose the tail of what it just recorded.
        mMonitor->retirePending(true);
        mFinalRecords.clear();
        mMonitor->drainFrames(mFinalRecords);
        if (mMonitor->mGpu)
            if (Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr)
                try { rs->deinitGPUProfiling(); } catch (...) {}
        monitor::gMonitor = nullptr;
        mMonitor.reset();
        return;
    }
    // UP: the ring is allocated here and the clock's zero is set here, so every
    // record in a capture is relative to the moment the owner pressed the key.
    // A TAIL NOBODY DRAINED belongs to the PREVIOUS capture; carrying it into
    // the next one would put frames from before the key press into a bundle
    // that is supposed to be forward-only.
    std::vector<FrameRecord>().swap(mFinalRecords);
    monitor::resetEpoch();
    mMonitor.reset(new monitor::FrameMonitor());
    mMonitor->mLevel = level;
    monitor::gMonitor = mMonitor.get();
    mShaderCache.recordCompileNames(true);
    // THE RUNTIME OFF-SWITCH, closed here and nowhere else: the query pools are
    // created when a capture starts and destroyed when it stops, so a dev build
    // with no capture running owns no pool at all (owner decision D3, lock 2).
    if (Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr) {
        try {
            rs->initGPUProfiling();
            bool available = false;
            rs->getCustomAttribute("JahGpuTimestamps", &available);
            mMonitor->mGpu = available;
        } catch (...) {
            mMonitor->mGpu = false;   // no patch in this build: CPU only, honestly
        }
    }
    if (mRoot) mRoot->addFrameListener(&mMonitor->mSplit);
    monitor::noteEvent(MonitorEventKind::Host, WorkReason::Request, "monitor.start");
}

MonitorLevel OgreEngine::frameMonitor() const {
    return mMonitor ? mMonitor->mLevel : MonitorLevel::Off;
}

MonitorStatus OgreEngine::monitorStatus() const {
    MonitorStatus st;
    st.level = mMonitor ? mMonitor->mLevel : MonitorLevel::Off;
    if (!mMonitor) {
        // THE ZERO-COST ASSERTIONS, answered from the ABSENCE of the object
        // rather than from a flag: there is nothing to count. The one thing
        // that can survive a stop is the undrained tail of the last capture.
        st.ringFrames = unsigned(mFinalRecords.size());
        gpuTimingStatus(st);
        return st;
    }
    st.attachedListeners = mMonitor->mAttachedCount;
    st.ringCapacity  = monitor::kRingCapacity;
    st.ringFrames    = mMonitor->ringFrames();
    st.pendingEvents = mMonitor->pendingEvents();
    st.framesRecorded = mMonitor->framesRecorded();
    st.framesDropped  = mMonitor->framesDropped();
    st.eventsDropped  = mMonitor->eventsDropped();
    st.overheadMs     = mMonitor->lastOverheadMs();
    gpuTimingStatus(st);
    return st;
}

unsigned OgreEngine::takeFrameRecords(std::vector<FrameRecord> &out) {
    if (mMonitor) return mMonitor->drainFrames(out);
    // The tail of the last capture, once, and then the storage goes.
    if (mFinalRecords.empty()) return 0u;
    const unsigned n = unsigned(mFinalRecords.size());
    for (FrameRecord &r : mFinalRecords) out.push_back(std::move(r));
    std::vector<FrameRecord>().swap(mFinalRecords);
    return n;
}

unsigned OgreEngine::takeMonitorEvents(std::vector<MonitorEvent> &out) {
    return mMonitor ? mMonitor->drainEvents(out) : 0u;
}

void OgreEngine::noteMonitorEvent(const MonitorEvent &event) {
    if (!mMonitor) return;
    MonitorEvent e = event;
    mMonitor->event(std::move(e));
}

void OgreEngine::noteHostStage(const std::string &name, float ms) {
    if (mMonitor) mMonitor->hostStage(name, ms);
}

void OgreEngine::setNextFrameCause(FrameCause cause) { mNextFrameCause = cause; }

// EVERY LIVE WORKSPACE THIS ENGINE CAN REACH, once a frame.
//
// Same shape and same reason as the shadow counters' re-attach: workspaces are
// recreated by atlas rebuilds, GI rebuilds and probe placement, and a listener
// list dies with its workspace. The view's listener rides the view's SEAM (so it
// survives a rebuild between two frames); the scenes' private workspaces — each
// planar mirror slot, each reflection probe — are attached directly and the
// attached set is rebuilt from scratch, so a workspace that vanished is simply
// not in it any more and is never dereferenced.
void OgreEngine::syncMonitorListeners(const std::vector<OgreScene *> &drawn) {
    if (!mMonitor) return;
    JAH_TRY {
        unsigned attached = 0u;
        for (auto &v : mViews) {
            if (!v->isEnabled()) continue;
            v->addWorkspaceListener(&mMonitor->mListener);   // idempotent, rides rebuilds
            if (v->workspace()) ++attached;
        }
        for (OgreScene *s : drawn) {
            std::vector<Ogre::CompositorWorkspace *> ws;
            s->monitorWorkspaces(ws);
            for (Ogre::CompositorWorkspace *w : ws) {
                const Ogre::CompositorWorkspaceListenerVec &ls = w->getListeners();
                if (std::find(ls.begin(), ls.end(), &mMonitor->mListener) == ls.end())
                    w->addListener(&mMonitor->mListener);
                ++attached;
            }
        }
        // A COUNT, never a list of pointers to keep: a workspace can die
        // between this frame and the next call, and detaching walks the live
        // scenes and views rather than anything remembered here.
        mMonitor->mAttachedCount = attached;
    } JAH_CATCH(mLastError, );
}


// ---------------------------------------------------------------------------
// THE ENGINE SNAPSHOT (§4.8 `snapshot_start.json` / `snapshot_end.json`)
// ---------------------------------------------------------------------------
//
// Everything a capture needs to be read MONTHS later without the scene in front
// of you: what was asked for, what it resolved to, what exists, what it costs
// and — the part nothing else in the engine can see — THE COMPOSITOR GRAPH:
// every live workspace, its nodes, its passes and the scene each renders.
//
// Works with the monitor OFF. It renders nothing, allocates nothing persistent
// and takes no lock, so the host can take one before it starts recording and
// one after it stops.

void OgreEngine::collectCompositorGraph(std::vector<CompositorWorkspaceInfo> &out) const {
    if (!mRoot || mHeadless) return;
    JAH_TRY {
        const auto describe = [&](Ogre::CompositorWorkspace *ws, const std::string &owner) {
            if (!ws) return;
            CompositorWorkspaceInfo info;
            info.owner = owner;
            const Ogre::CompositorWorkspaceDef *wdef = ws->getDefinition();
            if (wdef) info.name = wdef->getNameStr();
            if (ws->getSceneManager()) info.scene = ws->getSceneManager()->getName();
            info.enabled = ws->getEnabled();
            info.listeners = unsigned(ws->getListeners().size());
            if (Ogre::TextureGpu *t = ws->getFinalTarget()) {
                info.width = t->getWidth();
                info.height = t->getHeight();
            }
            const Ogre::CompositorNodeVec &nodes = ws->getNodeSequence();
            for (Ogre::CompositorNode *n : nodes) {
                if (!n) continue;
                CompositorNodeInfo ni;
                const Ogre::CompositorNodeDef *ndef = n->getDefinition();
                ni.name = ndef ? ndef->getNameStr() : std::string();
                if (ndef) {
                    const size_t targets = ndef->getNumTargetPasses();
                    for (size_t t = 0; t < targets; ++t) {
                        const Ogre::CompositorTargetDef *td = ndef->getTargetPass(t);
                        if (!td) continue;
                        const Ogre::CompositorPassDefVec &passes = td->getCompositorPasses();
                        for (const Ogre::CompositorPassDef *pd : passes) {
                            if (!pd) continue;
                            CompositorPassInfo pi;
                            pi.type = monitor::passTypeName(pd->getType());
                            pi.profilingId = pd->mProfilingId;
                            pi.shadowMapIdx = pd->mShadowMapIdx;
                            pi.numInitialPasses =
                                pd->mNumInitialPasses == ~Ogre::uint32(0) ? 0u : pd->mNumInitialPasses;
                            if (pd->getType() == Ogre::PASS_SCENE) {
                                const Ogre::CompositorPassSceneDef *sd =
                                    static_cast<const Ogre::CompositorPassSceneDef *>(pd);
                                pi.camera = sd->mCameraName.getFriendlyText();
                                // WHICH SHADOW NODE THIS PASS NAMES. The single
                                // most expensive line in a capture's graph: a
                                // probe face that names the VIEW's node costs a
                                // full-resolution atlas per probe, and a pass
                                // that names one at all re-renders it.
                                pi.shadowNode = sd->mShadowNode.getFriendlyText();
                            }
                            ni.passes.push_back(std::move(pi));
                        }
                    }
                }
                info.nodes.push_back(std::move(ni));
            }
            out.push_back(std::move(info));
        };

        for (const auto &v : mViews) describe(v->workspace(), "view:" + v->name());
        for (const auto &s : mScenes) {
            std::vector<Ogre::CompositorWorkspace *> ws;
            std::vector<std::string> owners;
            s->monitorWorkspaces(ws, &owners);
            for (size_t i = 0; i < ws.size(); ++i)
                describe(ws[i], (i < owners.size() ? owners[i] : std::string("?")) + " (" +
                                    s->name() + ")");
        }
    } JAH_CATCH(mLastError, );
}

bool OgreEngine::captureSnapshot(EngineSnapshot &out, const std::string &label,
                                 Scene *scene) const {
    out = EngineSnapshot();
    out.label = label;
    out.atMs = monitor::nowMs();
    out.frame = mShadowFrame;
    if (!mRoot) return false;
    JAH_TRY {
        // THE SCENE THE SNAPSHOT DESCRIBES: the caller's, or the first enabled
        // on-screen view's — the same "one view speaks for the process" rule the
        // HUD owner and the post chain's globals follow.
        OgreScene *s = static_cast<OgreScene *>(scene);
        if (!s) {
            for (const auto &v : mViews)
                if (v->isEnabled() && !v->isOffscreen() && v->ogreScene()) { s = v->ogreScene(); break; }
            if (!s)
                for (const auto &v : mViews)
                    if (v->isEnabled() && v->ogreScene()) { s = v->ogreScene(); break; }
        }
        out.live = true;
        out.device = deviceInfo();
        out.shaderCache = shaderCacheStats();
        out.shadow = shadowStatus();
        objectCounts(out.objects);
        memoryStats(out.memory);
        threading(out.threading);
        renderStats(out.render);
        textureMemory(out.textures);
        // The texture list is the biggest thing in a snapshot by far; the
        // largest entries are what an engine review reads, so it is sorted and
        // capped rather than dropped.
        std::sort(out.textures.begin(), out.textures.end(),
                  [](const TextureMemoryEntry &a, const TextureMemoryEntry &b) {
                      return a.bytes > b.bytes;
                  });
        out.textureCount = unsigned(out.textures.size());
        static const unsigned kMaxSnapshotTextures = 256u;
        if (out.textures.size() > kMaxSnapshotTextures) {
            out.texturesTruncated = out.textureCount - kMaxSnapshotTextures;
            out.textures.resize(kMaxSnapshotTextures);
        }
        out.texturesDoneStreaming = texturesDoneStreaming();
        // PER-HLMS DATABLOCK COUNTS. Ogre keeps its compiled-shader cache
        // private at this pin (`Hlms::mShaderCache` has no size accessor —
        // recorded for the upstream list), so what the snapshot can say
        // exactly is how many datablocks each Hlms holds: the number behind
        // the batching-collapse question ("> ~240 datablocks" in the render
        // audit), and the one that grows when materials are per object.
        if (Ogre::HlmsManager *hm = mRoot->getHlmsManager()) {
            static const char *kBlockNames[Ogre::HLMS_MAX] = {
                "low_level", "pbs", "toon", "unlit", "user0", "user1", "user2", "user3"
            };
            for (unsigned i = 0; i < Ogre::HLMS_MAX; ++i) {
                Ogre::Hlms *h = hm->getHlms(Ogre::HlmsTypes(i));
                if (!h) continue;
                out.hlmsDatablocks.emplace_back(kBlockNames[i],
                                                unsigned(h->getDatablockMap().size()));
            }
        }
        if (s) {
            out.scene = s->name();
            out.giParams = s->giParams();
            out.gi = s->giStatus();
            out.mobility = s->mobilityStatus();
            s->collectProbeInfo(out.probes);
            s->collectLightInfo(out.lights);
            // The per-light shadow-cache state is process-wide (it lives on the
            // shadow node instances), so it is joined in here rather than
            // guessed at in the scene.
            for (SnapshotLight &l : out.lights)
                for (const ShadowMapInfo &m : out.shadow.mapped)
                    if (m.node == l.node) {
                        l.cached = m.isCached;
                        l.dirty = m.dirty;
                        l.shadowSlot = m.slot;
                    }
        }
        collectCompositorGraph(out.workspaces);
    } JAH_CATCH(mLastError, false);
    return out.live;
}

// ---------------------------------------------------------------------------
// GPU timing status — BOTH off-switches, visible (owner decision D3)
// ---------------------------------------------------------------------------
void OgreEngine::gpuTimingStatus(MonitorStatus &st) const {
    st.gpuCompiled = false;
    st.gpuSupported = false;
    st.gpuActive = false;
    st.gpuQueryPools = 0u;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (!rs) { st.gpuReason = "no render system"; return; }
    // LOCK 1, THE BUILD. `getCustomAttribute` THROWS on an unknown name, and in
    // a render system built without JAH_GPU_TIMESTAMPS the name does not exist:
    // that exception IS the answer, and it is why a production build needs no
    // runtime flag of its own to be honest here.
    bool available = false;
    try {
        rs->getCustomAttribute("JahGpuTimestamps", &available);
        st.gpuCompiled = true;
    } catch (...) {
        st.gpuReason = "this Ogre build has no GPU timestamp support "
                       "(ogre-patch 0027 / JAH_GPU_TIMESTAMPS is off — a production build)";
        return;
    }
    // LOCK 2, THE RUNTIME. `available` is true only while a query pool exists,
    // and a pool exists only inside a capture.
    st.gpuSupported = true;
    st.gpuActive = available;
    st.gpuQueryPools = available ? 2u : 0u;
    if (available) {
        Ogre::uint32 truncated = 0u;
        try { rs->getCustomAttribute("JahGpuSamplesTruncated", &truncated); } catch (...) {}
        st.gpuSamplesTruncated = unsigned(truncated);
    }
    if (!available)
        st.gpuReason = mMonitor ? "the device or queue has no usable timestamps"
                                : "no capture is running (no query pool exists)";
    else
        st.gpuReason.clear();
}

// The frame's GPU bookkeeping: rotate the query pools, read back what the frame
// two frames ago measured, and reset the pool about to be written. ONE call,
// at the top of the frame and outside every encoder — which is the only place
// vkCmdResetQueryPool is legal (see ogre-patch 0027).
void OgreEngine::gpuFrameBegin() {
    if (!mMonitor || !mMonitor->mGpu) return;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (!rs) return;
    try {
        rs->getCustomAttribute("JahGpuFrameBegin", nullptr);
        std::vector<std::pair<Ogre::uint32, float>> results;
        rs->getCustomAttribute("JahGpuSampleResults", &results);
        for (const auto &r : results) mMonitor->noteGpuSample(unsigned(r.first), r.second);
    } catch (...) {
        // A render system that stopped answering (a device loss took the pools)
        // turns GPU sampling off for the rest of the capture rather than
        // half-filling records.
        mMonitor->mGpu = false;
        monitor::noteEvent(MonitorEventKind::DeviceLost, WorkReason::None, "gpu.timestamps.lost");
    }
}

}   // namespace detail
}}  // namespace jahshaka::engine
