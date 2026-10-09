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
#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanDevice.h"
#include "OgreVulkanQueue.h"

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
    mLastFrameNumber = frame;
    mCurrent.cause = cause;
    mCurrent.onscreen = onscreen;
    mCurrent.startMs = nowMs();
    mFrameStart = t0;
    mInFrame = true;
    mStageChild = nullptr;
    mPassStack.clear();
    mWorkspaceDepths.clear();
    mPassSampleIds.clear();
    mCacheSampleIds.clear();
    mPassTop.clear();
    mCacheTop.clear();
    // Host stages pushed before the frame opened (the driver's tick wraps the
    // engine's frame, so `tick` and the mirror's sub-stages are known first)
    // lead the stage list.
    mCurrent.stages.swap(mPendingHostStages);
    mPendingHostStages.clear();
    adoptPendingCacheWork();
    // ...and the passes a workspace executed between frames (a one-shot sky bake, F2).
    for (size_t i = 0; i < mPendingPasses.size(); ++i) {
        mCurrent.passes.push_back(std::move(mPendingPasses[i]));
        if (mGpu) {
            mPassSampleIds.push_back(i < mPendingPassSampleIds.size() ? mPendingPassSampleIds[i] : 0u);
            mPassTop.push_back(i < mPendingPassTop.size() ? mPendingPassTop[i] : char(0));
        }
    }
    mPendingPasses.clear();
    mPendingPassSampleIds.clear();
    mPendingPassTop.clear();
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
    // THE HOLDING QUEUE (P1c). A GPU sample comes back when the GPU has
    // finished the frame, so the record waits here until every sample it asked
    // for has answered (MONITOR-RETIRE-1); without GPU sampling the queue is
    // one deep and the record is published immediately.
    PendingFrame pf;
    pf.rec = std::move(mCurrent);
    pf.passSampleIds.swap(mPassSampleIds);
    pf.cacheSampleIds.swap(mCacheSampleIds);
    pf.passTop.swap(mPassTop);
    pf.cacheTop.swap(mCacheTop);
    // THE FRAME'S PAIR is this record's, and it is STILL OPEN: it closes at the
    // frame's close (after the VR eye copy), with the gaps it holds.
    pf.frameSampleId = mFrameSampleOpen ? mFrameSampleId : 0u;
    if (pf.frameSampleId) { mFrameOwner = pf.rec.frame; mFrameOwned = true; }
    if (mGpu) {
        const unsigned slot = unsigned(mPending.size());
        for (unsigned i = 0; i < pf.passSampleIds.size(); ++i)
            if (pf.passSampleIds[i]) {
                mGpuSampleIndex[pf.passSampleIds[i]] = { slot, i, SampleKind::Pass };
                ++pf.outstanding;
            }
        for (unsigned i = 0; i < pf.cacheSampleIds.size(); ++i)
            if (pf.cacheSampleIds[i]) {
                mGpuSampleIndex[pf.cacheSampleIds[i]] = { slot, i, SampleKind::Cache };
                ++pf.outstanding;
            }
        if (pf.frameSampleId) {
            mGpuSampleIndex[pf.frameSampleId] = { slot, 0u, SampleKind::Frame };
            ++pf.outstanding;
        }
    }
    mPending.push_back(std::move(pf));
    retirePending(false);
    mCurrent = FrameRecord();
    mInFrame = false;
    ++mFramesRecorded;
}

void FrameMonitor::noteGpuSample(unsigned sampleId, float ms) {
    auto it = mGpuSampleIndex.find(sampleId);
    auto key = mSampleKeys.find(sampleId);
    if (it == mGpuSampleIndex.end()) {           // its frame already aged out
        if (key != mSampleKeys.end()) mSampleKeys.erase(key);
        return;
    }
    const GpuSampleSlot where = it->second;
    if (where.frame < mPending.size()) {
        PendingFrame &pf = mPending[where.frame];
        if (pf.outstanding) --pf.outstanding;
        FrameRecord &rec = pf.rec;
        // A NEGATIVE time is the fork's "this sample will never have one" (no
        // query room, a wrapped counter, a lost pool): it answers the sample
        // and leaves the row unmeasured.
        if (ms < 0.0f) {
            if (where.kind == SampleKind::Gap) pf.gapLost = true;
        } else if (where.kind == SampleKind::Gap) {
            const auto sp = mGapSpanned.find(sampleId);
            float &acc = (sp != mGapSpanned.end() && sp->second) ? rec.gpuIdleMs : rec.unattributedGpuMs;
            if (acc < 0.0f) acc = 0.0f;
            acc += ms;
        } else if (where.kind == SampleKind::Frame) {
            rec.frameGpuMs = ms;
        } else if (where.kind == SampleKind::Cache) {
            if (where.row < rec.cacheWork.size()) rec.cacheWork[where.row].gpuMs = ms;
            // THE STATUS READOUT'S VALUE (lastGpuMs): one frame's rows of a key
            // summed, a later frame's replacing them.
            if (key != mSampleKeys.end()) {
                LastGpu &l = mLastGpu[key->second];
                if (l.frame == rec.frame && l.ms >= 0.0f) l.ms += ms;
                else { l.frame = rec.frame; l.ms = ms; }
            }
        } else if (where.row < rec.passes.size()) {
            rec.passes[where.row].gpuMs = ms;
        }
    }
    if (key != mSampleKeys.end()) mSampleKeys.erase(key);
    mGapSpanned.erase(sampleId);
    mGpuSampleIndex.erase(it);
}

void FrameMonitor::noteGpuMarksDropped(unsigned n) {
    if (!n) return;
    mGpuMarksDropped += n;
    // The frame that issued them is the newest one waiting for its samples; a
    // frame already published (no GPU sampling, or aged out) keeps the total only.
    if (!mPending.empty()) mPending.back().rec.gpuMarksDropped += n;
}

void FrameMonitor::retirePending(bool all) {
    // IN ORDER: the oldest frame leaves first, once every sample it asked for
    // has answered — the GPU finishes frames in submission order, so a newer
    // frame never waits long behind an older one. A frame held past
    // kMaxHeldFrames is published unsampled and counted.
    while (!mPending.empty()) {
        const bool answered = mPending.front().outstanding == 0u;
        const bool aged = mPending.size() > size_t(kMaxHeldFrames);
        if (!(all || !mGpu || answered || aged)) break;
        if (!answered && !all && mGpu) ++mGpuFramesAgedOut;
        PendingFrame pf = std::move(mPending.front());
        mPending.pop_front();
        // The frame's GPU total over its passes, from whatever came back: the
        // TOP-LEVEL passes only (a pass's pair is inclusive — the scene pass's
        // encloses its shadow node's). NEGATIVE stays negative: a pass with no
        // sample is "not measured", never zero.
        const auto top = [](const std::vector<char> &v, size_t i) { return i < v.size() && v[i]; };
        for (size_t i = 0; i < pf.rec.passes.size(); ++i) {
            const FramePass &p = pf.rec.passes[i];
            if (p.gpuMs >= 0.0f && top(pf.passTop, i)) {
                if (pf.rec.gpuMs < 0.0f) pf.rec.gpuMs = 0.0f;
                pf.rec.gpuMs += p.gpuMs;
            }
        }
        // COVERAGE (F5), from the GAPS (TEST-1 fix round): what no row covers, split
        // into the GPU waiting for a submission (idle) and GPU work no row names.
        // A gap whose sample was lost leaves both unknowable (-1); a frame whose
        // rows left no gap reads 0.
        if (pf.rec.frameGpuMs >= 0.0f) {
            if (pf.gapLost) {
                pf.rec.gpuIdleMs = pf.rec.unattributedGpuMs = -1.0f;
            } else {
                if (pf.rec.gpuIdleMs < 0.0f) pf.rec.gpuIdleMs = 0.0f;
                if (pf.rec.unattributedGpuMs < 0.0f) pf.rec.unattributedGpuMs = 0.0f;
            }
        } else {
            pf.rec.gpuIdleMs = pf.rec.unattributedGpuMs = -1.0f;
        }
        for (unsigned id : pf.passSampleIds) mGpuSampleIndex.erase(id);
        for (unsigned id : pf.cacheSampleIds) { mGpuSampleIndex.erase(id); mSampleKeys.erase(id); }
        if (pf.frameSampleId) mGpuSampleIndex.erase(pf.frameSampleId);
        for (unsigned id : pf.gapIds) { mGpuSampleIndex.erase(id); mGapSpanned.erase(id); }
        push(std::move(pf.rec));
        // Every surviving frame moved down one slot.
        for (auto &kv : mGpuSampleIndex)
            if (kv.second.frame > 0u) --kv.second.frame;
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
    if (mGpu && f.gpuSampleId && mOrphanRs) endRowSample(mOrphanRs, f.rec.pass);
    f.rec.cpuMs = -1.0f;
    f.rec.orphaned = true;
    pass(std::move(f.rec), f.gpuSampleId, f.gpuTop);
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
void FrameMonitor::cacheWork(const CacheWork &w, unsigned gpuSampleId, bool top) {
    if (mInFrame) {
        mCurrent.cacheWork.push_back(w);
        // INDEX-PARALLEL, always — including the rows that carry no sample, or
        // a late result would be filed against the wrong row.
        if (mGpu) { mCacheSampleIds.push_back(gpuSampleId); mCacheTop.push_back(char(top)); }
    } else {
        // BETWEEN FRAMES the row is adopted by the next frame — and so is its
        // sample id: the timestamp pair was written into the command buffer
        // that frame's passes will also write into, so the result comes back
        // on the same two-frame schedule as theirs.
        mPendingCacheWork.push_back(w);
        if (mGpu) { mPendingCacheSampleIds.push_back(gpuSampleId); mPendingCacheTop.push_back(char(top)); }
    }
}
void FrameMonitor::pass(FramePass &&p, unsigned gpuSampleId, bool top) {
    if (!mInFrame) {
        // BETWEEN FRAMES (a one-shot sky bake, an IBL convolution — F2): the row
        // is the next frame's, like a between-frames cache row, and so is its
        // sample. Bounded: past it the row is dropped (its sample answers nobody).
        if (mPendingPasses.size() >= 4096u) return;
        mPendingPasses.push_back(std::move(p));
        if (mGpu) { mPendingPassSampleIds.push_back(gpuSampleId); mPendingPassTop.push_back(char(top)); }
        return;
    }
    mCurrent.passes.push_back(std::move(p));
    if (mGpu) { mPassSampleIds.push_back(gpuSampleId); mPassTop.push_back(char(top)); }
}

// ---- the one GPU-timing facility (lane TEST-1) ------------------------------
unsigned FrameMonitor::beginRowSample(Ogre::RenderSystem *rs, const std::string &name, bool &top) {
    top = false;
    if (!mGpu || !rs) return 0u;
    const unsigned id = nextGpuSampleId();
    if (!id) return 0u;
    if (mOpenRowSamples == 0u) closeGap();   // the stretch with no row ends here
    unsigned hash = id;
    try { rs->beginGPUSampleProfile(name, &hash); } catch (...) { if (mOpenRowSamples == 0u) openGap(); return 0u; }
    top = mFrameSampleOpen && mOpenRowSamples == 0u;
    ++mOpenRowSamples;
    return id;
}
void FrameMonitor::endRowSample(Ogre::RenderSystem *rs, const std::string &name) {
    if (!rs) return;
    try { rs->endGPUSampleProfile(name); } catch (...) {}
    if (mOpenRowSamples) --mOpenRowSamples;
    if (mOpenRowSamples == 0u) openGap();   // ...and the next one begins
}

namespace {
/// Ogre's CURRENT command buffer, as a tag: it changes exactly when the queue
/// submits (commitAndNextCommandBuffer begins a new one), which is what tells a
/// gap that crossed a submission from one that did not. 0 = cannot tell.
std::uintptr_t cmdTag(Ogre::RenderSystem *rs) {
    auto *vk = dynamic_cast<Ogre::VulkanRenderSystem *>(rs);
    if (!vk || !vk->getVulkanDevice()) return 0u;
    try {
        return reinterpret_cast<std::uintptr_t>(vk->getVulkanDevice()->mGraphicsQueue.getCurrentCmdBuffer());
    } catch (...) {
        return 0u;
    }
}
}   // namespace

void FrameMonitor::openGap() {
    if (!mGpu || !mFrameSampleOpen || mGapOpen || mOpenRowSamples != 0u || !mFrameRs) return;
    const unsigned id = nextGpuSampleId();
    if (!id) return;
    unsigned hash = id;
    try { mFrameRs->beginGPUSampleProfile("gap", &hash); } catch (...) { return; }
    mGapOpen = true;
    mGapId = id;
    mGapCmd = cmdTag(mFrameRs);
}
void FrameMonitor::closeGap() {
    if (!mGapOpen) return;
    mGapOpen = false;
    try { mFrameRs->endGPUSampleProfile("gap"); } catch (...) { return; }
    const std::uintptr_t now = cmdTag(mFrameRs);
    mGapSpanned[mGapId] = now != mGapCmd || now == 0u;
    mFrameGaps.push_back(mGapId);
}
void FrameMonitor::openFrameSample(Ogre::RenderSystem *rs) {
    if (!mGpu || !rs || mFrameSampleOpen) return;
    // The fork cleared its sample stack at the turnover; ours must agree.
    mOpenRowSamples = 0u;
    const unsigned id = nextGpuSampleId();
    if (!id) return;
    unsigned hash = id;
    try { rs->beginGPUSampleProfile("frame", &hash); } catch (...) { return; }
    mFrameSampleId = id;
    mFrameSampleOpen = true;
    mFrameRs = rs;
    mFrameOwned = false;
    mFrameGaps.clear();
    mGapOpen = false;
    openGap();
}
void FrameMonitor::closeFrameSample() {
    if (!mFrameSampleOpen) return;
    closeGap();
    mFrameSampleOpen = false;
    // The record this pair belongs to (endFrame named it), still waiting for its
    // samples: the pair's own id is registered there already; its gaps join now.
    PendingFrame *owner = nullptr;
    unsigned slot = 0u;
    if (mFrameOwned)
        for (unsigned i = 0; i < mPending.size(); ++i)
            if (mPending[i].rec.frame == mFrameOwner && mPending[i].frameSampleId == mFrameSampleId) {
                owner = &mPending[i];
                slot = i;
                break;
            }
    bool ended = false;
    if (mOpenRowSamples == 0u && mFrameRs) {
        try { mFrameRs->endGPUSampleProfile("frame"); ended = true; } catch (...) {}
    }
    if (!ended) {
        // Closing now would end an open ROW's sample (the fork pops the innermost):
        // the pair is abandoned, and the record stops waiting for it.
        if (owner && mGpuSampleIndex.erase(mFrameSampleId) && owner->outstanding) --owner->outstanding;
        if (owner) owner->frameSampleId = 0u;
    }
    if (owner) {
        for (unsigned g : mFrameGaps) {
            mGpuSampleIndex[g] = { slot, unsigned(owner->gapIds.size()), SampleKind::Gap };
            owner->gapIds.push_back(g);
            ++owner->outstanding;
        }
    } else {
        for (unsigned g : mFrameGaps) mGapSpanned.erase(g);
    }
    mFrameGaps.clear();
    mFrameSampleId = 0u;
    mFrameOwned = false;
}
void FrameMonitor::event(MonitorEvent &&e) {
    if (mEvents.size() >= kEventCapacity) { ++mEventsDropped; return; }
    // BETWEEN FRAMES the frame being built is a default record whose number is
    // 0, and every mark a script dropped outside a frame said "frame 0" — which
    // made a capture's marks useless for windowing (ENGINE-5 item 2). The last
    // frame that RAN is the honest answer there.
    if (e.frame == 0) e.frame = mInFrame ? mCurrent.frame : mLastFrameNumber;
    if (e.startMs == 0.0) e.startMs = nowMs();
    mEvents.push_back(std::move(e));
}
void FrameMonitor::adoptPendingCacheWork() {
    for (size_t i = 0; i < mPendingCacheWork.size(); ++i) {
        mCurrent.cacheWork.push_back(std::move(mPendingCacheWork[i]));
        // Index-parallel, always: a row banked before GPU sampling started
        // carries 0 and simply reports no GPU time.
        if (mGpu) {
            mCacheSampleIds.push_back(i < mPendingCacheSampleIds.size()
                                          ? mPendingCacheSampleIds[i] : 0u);
            mCacheTop.push_back(i < mPendingCacheTop.size() ? mPendingCacheTop[i] : char(0));
        }
    }
    mPendingCacheWork.clear();
    mPendingCacheSampleIds.clear();
    mPendingCacheTop.clear();
}

bool FrameSplitListener::frameRenderingQueued(const Ogre::FrameEvent &) {
    mMark = std::chrono::steady_clock::now();
    mMarked = true;
    // (The frame's own GPU pair does NOT close here: it closes at the frame's close,
    // after the VR eye copy and anything else the close records — the fix round.)
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
    f.gpuSampleId = gMonitor->beginRowSample(rs, f.rec.pass, f.gpuTop);
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
    if (gMonitor->mGpu && rs && f.gpuSampleId) gMonitor->endRowSample(rs, f.rec.pass);
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
    gMonitor->pass(std::move(f.rec), f.gpuSampleId, f.gpuTop);
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
    case GiStaleReason::Camera:   return WorkReason::Camera;
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

CacheScope::CacheScope(CacheKind cache, WorkReason reason, unsigned long long id,
                       const char *detail, Ogre::RenderSystem *rs, const void *owner)
    : mId(id), mCache(cache), mReason(reason) {
    if (!gMonitor) return;
    mArmed = true;
    mDetail = detail;
    mStart = std::chrono::steady_clock::now();
    // THE GPU PAIR (fork 1a81f866a+1bccc3f93 (was 0027)). This is the ONLY way a compute dispatch
    // can report GPU time: every other sample in the monitor rides a compositor
    // pass callback, and a GI voxelisation, a light injection or an irradiance
    // field's integration is not a pass. Work between frames samples too — the
    // pair goes into the command buffer the next frame will keep writing, and
    // the row it belongs to is adopted by that frame.
    mGpuSampleId = gMonitor->beginRowSample(rs, detail ? detail : "cache", mGpuTop);
    if (mGpuSampleId) {
        mRs = rs;
        mTurnover = gMonitor->mTurnovers;
        if (owner && detail) gMonitor->mSampleKeys[mGpuSampleId] = { std::string(detail), owner };
    }
}

CacheScope::~CacheScope() { close(); }

void CacheScope::close() {
    if (!mArmed || !gMonitor) return;
    mArmed = false;                       // idempotent: the destructor follows
    if (mRs && mTurnover != gMonitor->mTurnovers) {
        // OPENED BEFORE A TURNOVER: the fork cleared its stack there, so an end
        // now would pop a sample of this frame. Filed untimed.
        gMonitor->mSampleKeys.erase(mGpuSampleId);
        mRs = nullptr;
        mGpuSampleId = 0u;
    }
    if (mRs) gMonitor->endRowSample(mRs, mDetail ? mDetail : "cache");
    if (mCancelled) {
        // The sample is written and will be answered; nobody files it.
        if (mGpuSampleId) gMonitor->mSampleKeys.erase(mGpuSampleId);
        return;
    }
    CacheWork w;
    w.cache = mCache;
    w.reason = mReason;
    w.id = mId;
    if (mDetail) w.detail = mDetail;
    w.units = mUnits;
    w.ms = float(std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - mStart).count());
    gMonitor->cacheWork(w, mGpuSampleId, mGpuTop);
}

float lastGpuMs(const char *detail, const void *owner) {
    if (!gMonitor || !detail) return -1.0f;
    const auto it = gMonitor->mLastGpu.find({ std::string(detail), owner });
    return it == gMonitor->mLastGpu.end() ? -1.0f : it->second.ms;
}

void watchWorkspace(Ogre::CompositorWorkspace *ws) {
    if (gMonitor && ws) ws->addListener(&gMonitor->mListener);
}

void forgetGpuOwner(const void *owner) {
    if (!gMonitor || !owner) return;
    for (auto it = gMonitor->mLastGpu.begin(); it != gMonitor->mLastGpu.end();)
        it = it->first.second == owner ? gMonitor->mLastGpu.erase(it) : std::next(it);
    for (auto it = gMonitor->mSampleKeys.begin(); it != gMonitor->mSampleKeys.end();)
        it = it->second.second == owner ? gMonitor->mSampleKeys.erase(it) : std::next(it);
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
void noteCascadeRebuild() {
    if (gMonitor && gMonitor->inFrame()) ++gMonitor->current().cascadeRebuilds;
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
            for (auto &v : mViews) {
                v->removeWorkspaceListener(&mMonitor->mListener);
                std::vector<Ogre::CompositorWorkspace *> vws;
                v->monitorWorkspaces(vws);
                for (Ogre::CompositorWorkspace *w : vws) w->removeListener(&mMonitor->mListener);
            }
            if (mVrSession)
                if (Ogre::CompositorWorkspace *w = vrSessionMirrorWorkspace(mVrSession))
                    w->removeListener(&mMonitor->mListener);
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
        if (Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr) {
            if (mMonitor->mGpu) try { rs->deinitGPUProfiling(); } catch (...) {}
            // ...and the metrics go back to what they were (renderStats arms them
            // lazily again if anybody asks).
            if (!mMetricsBeforeCapture) rs->setMetricsRecordingEnabled(false);
        }
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
        // A CAPTURE ARMS OGRE'S METRICS (F6): a pass's draw and triangle counts
        // read the render system's counters, which are off until something turns
        // them on — before this, a capture with no renderStats() call before it
        // recorded zeros (`metricsRecording` false). Restored when it stops.
        mMetricsBeforeCapture = rs->getMetrics().mIsRecordingMetrics;
        rs->setMetricsRecordingEnabled(true);
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

// ---------------------------------------------------------------------------
// THE ARM REGISTRY (lane TEST-1, the perf audit's A2) — beside the monitor because
// both are the measuring facility: the monitor reads a frame, an arm switches what
// the frame does for a paired measurement.
// ---------------------------------------------------------------------------
namespace {
struct ArmDef {
    const char *name = nullptr;
    double def = 0.0, lo = 0.0, hi = 0.0;
    const char *what = nullptr;
};
// THE TABLE, in ArmId's order (asserted below). Each row names the door it replaced.
const ArmDef kArms[] = {
    { "reflect.motion", 1.0, 0.0, 1.0,
      "The screen march's object-motion job and the reflection trace's mover branches "
      "(REFLECT-MOVERS-1). 0 = the camera path alone: the pre-lane picture. gi.reflect_mover's "
      "paired cost arm. Was the JAH_R5_NO_MOTION environment door (two per-frame reads)." },
    { "reflect.posed", 1.0, 0.0, 1.0,
      "The POSED identification (SKINNED-VELOCITY-1): the trace and the march's velocity take "
      "the posed geometry rows. 0 = the id image alone, the pre-lane motion for a character. "
      "gi.reflect_mover --cost-posed. Was JAH_R7_NO_POSED (two per-frame reads)." },
    { "reflect.alphaTested", 1.0, 0.0, 1.0,
      "Alpha-tested (cut-out) geometry traced as cut-out by the reflection, the gather and the "
      "sun contact (REFLECT-MOVERS-2's alpha table). 0 = every cut-out traced opaque. "
      "gi.rt_alpha_tested's cost arm. Was JAH_R6_NO_ALPHA (a per-frame read)." },
    { "reflect.edgeClasses", 0.0, 0.0, 2.0,
      "The Hits photon view's history-class overlay (REFLECT-EDGE-2's instrument): 1 = mover "
      "hit / restart / reflected-image path, 2 = the new count's bins; 0 = off. Nothing but "
      "that view reads it. gi.reflect_mover --edge. Was JAH_R7_EDGE_CLASSES (a per-frame read)." },
    { "reflect.monoEyes", 0.0, 0.0, 1.0,
      "A stereo view's trace reconstructs with the MONO basis (the pre-REFLECT-VR-1 arm, "
      "wrong for one eye by construction): the A/B that proves the per-eye basis. Was "
      "JAH_R5_MONO_EYES (read once per process)." },
    { "rayquery.tlasRefit", 0.0, 0.0, 1.0,
      "Refit the top-level structure when the traced SET is unchanged, instead of rebuilding "
      "it (the rebuild is the default: the better tree, 0.21-0.35 ms at 8,001 instances). "
      "far_blas_measure --tlas. Was JAH_RQ_REFIT (read once per process)." },
    { "gather.temporal", 1.0, 0.0, 1.0,
      "The screen-probe gather's pixel history (PHOTON-GATHER-1c). 0 = each frame's estimate alone, "
      "no history read or written (the measurement lever: gi.gather, gi.gather_temporal, "
      "gi.gather_reference, gi.gather_phase2). Was JAHSHAKA_GATHER_NO_TEMPORAL (a per-frame read)." },
    { "atom.decode", 1.0, 0.0, 1.0,
      "The screen decode armed in the decode pass. 0 = unarmed: the Atom items are drawn by NOTHING "
      "in that pass (engine.atom_draw's proof that the view's passes skip their queue; scale's "
      "negative control). Was JAHSHAKA_ATOM_DECODE_OFF (a per-frame read)." },
    { "cards.footprintTexels", 4.0, 0.0, 1e12,
      "The card read's footprint gate in texels (Types.h kCardFootprintTexels = 4): the sweep that "
      "chose it (test_rt_reflect --footprint-sweep) sets it per arm, 1e12 = the gate open, 0 = shut. "
      "Was JAHSHAKA_CARD_FOOTPRINT_K (a per-frame read)." },
    { "gi.fieldScroll", 1.0, 0.0, 1.0,
      "The irradiance field SCROLLS on a cascade-0 step (keeps the probes that stay inside). 0 = the "
      "same snapped window re-placed WHOLE, the behaviour the scroll replaced (gi.field_scroll walks "
      "one path both ways). Was JAHSHAKA_GI_FIELD_NO_SCROLL (a per-step read)." },
    { "photon.diffuseConeSkip", 1.0, 0.0, 1.0,
      "The pixel's four-cone diffuse is marched only where the irradiance field's fallback weight "
      "(1 - confidence) is above zero - the only pixels it reaches (SPEED-GPU, audit PH-2). 0 = "
      "marched in every pixel of a chain with a field, the cost before the skip; the picture is "
      "the same bytes (gi.cone_skip)." },
    { "photon.specularConeSkip", 1.0, 0.0, 1.0,
      "The pixel's specular cone is marched only where the screen/ray reflection's confidence w "
      "is below 1 - the composite replaces the whole environment term where it is 1 (SPEED-GPU, "
      "audit PH-1 / S1). 0 = marched in every pixel, the cost before the skip; the picture is the "
      "same bytes (gi.cone_skip)." },
    { "gather.decodeHits", 0.0, 0.0, 1.0,
      "EVERY screen-probe gather hit is a hit RECORD shaded by the visibility-buffer decode, "
      "never read from the cards or the voxels (SPECKLE-FIX-1). The route-agreement instrument: "
      "the decode answers a gather hit with the diffuse response only, exactly what the caches "
      "store, so the two routes on the same hit agree within the caches' own quantisation "
      "(gi.speckle). While on, the hit list is SIZED for every gather ray of an 8-px stride "
      "(kHitListHeightFactorDecodeAll; toggling it rebuilds the view's graph) - at the shipped size "
      "it dropped 46 % of Showroom 2's hits as black; any drop left is giStatus rayQuery.hitDropped "
      "and a measurement that reads it refuses." },
    { "gather.octRes", 0.0, 0.0, 8.0,
      "The screen-probe gather's probe map resolution: rays per probe = octRes^2 (GATHER-NOISE-1's "
      "noise lever; GatherTuning::octRes). 0 = the tier's (8: 64 rays). At most 8 - one ray is one "
      "thread of the trace's 8x8 workgroup." },
    { "gather.stride", 0.0, 0.0, 64.0,
      "The screen-probe gather's probe stride in pixels (GatherTuning::probeStride). 0 = the tier's "
      "(8 at Epic, 16 at High and Medium)." },
    { "gather.historyFrames", 0.0, 0.0, 64.0,
      "The gather's pixel history floor in frames: a true mean that long, then an EMA at 1/N "
      "(GatherTuning::historyFrames). 0 = the shipped value; clamped to the history count's range." },
    { "gather.filterRadius", 0.0, 0.0, 2.0,
      "The probe-space filter's reach (GatherTuning::filterRadius): 1 = the 3x3 neighbourhood, "
      "2 = plus the ring of eight at two cells (17 taps). 0 = the shipped reach." },
    { "gather.restOff", 0.0, 0.0, 1.0,
      "No rest mean (GatherTuning::restOff): a still view keeps running the history's EMA instead of "
      "handing over to the rest mean and holding - the door that measures the history at rest." },
    { "gather.restFrames", 0.0, 0.0, 4096.0,
      "The rest mean's length in frames (GatherTuning::restFrames): a still view averages this many "
      "rest frames before it holds. 0 = the settle. With gather.restSeed it builds a CONVERGED "
      "reference (gi.flythrough_noise)." },
    { "gather.restSeed", 0.0, 0.0, 255.0,
      "The rest frames' sample-sequence offset (GatherTuning::restSeed): two references of one pose "
      "at two seeds are independent draws. 0 = the shipped sequence. At most 255: the frame index "
      "travels as a float, exact below 2^24." },
    { "gather.ageView", 0.0, 0.0, 64.0,
      "THE HISTORY-AGE VIEW (GatherTuning::ageView, an instrument): N > 0 replaces the gather's "
      "answer with magenta where the pixel's reprojected history held fewer than N frames and black "
      "elsewhere (read in the diffuse photon view). 0 = off." },
    { "gather.freezeFrame", 0.0, 0.0, 1.0,
      "The gather's sample sequence held at one frame (GatherTuning::freezeFrameIndex): every frame "
      "draws the same rays at the same cell, so two runs of one path differ only by what is NOT the "
      "gather's Monte-Carlo (the caches' state, the rounding) - GATHER-NOISE-1's separation arm." },
    { "gather.youngFrames", 0.0, 0.0, 64.0,
      "THE YOUNG HISTORY'S REACH (GatherTuning::youngFrames): a pixel whose history holds fewer "
      "frames than this reads a wider probe neighbourhood, narrowing to the bilinear four as its "
      "history fills. 0 = the shipped value; 1 with gather.youngReach 1 = off." },
    { "gather.youngReach", 0.0, 0.0, 64.0,
      "The young pixel's reach in PIXELS at a history of 0 (GatherTuning::youngReach): a tent that "
      "wide, in probe cells of the view's stride, never under the bilinear's one cell (any reach up "
      "to the stride = off). 0 = the shipped 24 px." },
    { "gather.validationOff", 0.0, 0.0, 1.0,
      "THE HISTORY'S VALIDATION OFF (GatherTuning::historyValidationOff, a test door): every "
      "reprojected texel on the previous picture is accepted, the distance and normal tests off - "
      "what separates a pixel new to the screen from one the tests rejected (GATHER-NOISE-1)." },
    { "gather.crossStrata", 0.0, 0.0, 8.0,
      "THE RAYS STRATIFIED ACROSS NEIGHBOURING PROBES (GatherTuning::crossStrata): a ray's jitter in "
      "its octahedral texel is confined to one of N x N sub-cells, any N x N block of probes covering "
      "all of them once (shifted at random per texel and frame). 1 = off; 0 = the shipped N." },
    { "gi.cascadeFault", -1.0, -1.0, 15.0,
      "FAULT INJECTION (a test door, gi.cascades F2): cascade N's rebuild throws after its region "
      "moved and BEFORE its build - the revert path. -1 = none. Was JAH_GI_CASCADE_FAULT (a "
      "per-rebuild read)." },
    { "gi.cascadeFaultPost", -1.0, -1.0, 15.0,
      "FAULT INJECTION (a test door, gi.cascades round-2 F1): cascade N's rebuild throws AFTER its "
      "build - the keep-the-placement path. -1 = none. Was JAH_GI_CASCADE_FAULT_POST (a per-rebuild "
      "read)." },
    { "gi.rebuildSettle", 1.0, 0.0, 1.0,
      "The rebuild settle (the chain's owed settle steps after a rebuild). 0 = off: "
      "gi.chain_converge's paired arm. Was JAHSHAKA_GI_NO_REBUILD_SETTLE (a per-frame read)." },
    { "gi.fieldRays", 0.0, 0.0, 7.0,
      "The irradiance field's rays per depth texel at its next build (1-7; 0 = the shipped count). "
      "gi.field_thin_wall's noise arms. Was JAHSHAKA_GI_FIELD_RAYS (a per-build read)." },
    { "gi.fieldSamples", 0.0, 0.0, 4096.0,
      "The irradiance field's sample target per texel at its next build (0 = the shipped "
      "kIfdTargetSamples). gi.field_thin_wall. Was JAHSHAKA_GI_FIELD_SAMPLES (a per-build read)." },
    { "gi.fieldStatic", 0.0, 0.0, 1.0,
      "The irradiance field's ray set NOT rotated per frame at its next build (the fixed-set arm). "
      "gi.field_thin_wall. Was JAHSHAKA_GI_FIELD_STATIC (a per-build read)." },
    { "gi.refuseGeometry", 0.0, 0.0, 1.0,
      "THE REFUSAL HOOK (a test door, gi.voxel_resident case 5): a voxel build runs with NO geometry "
      "source - the state of a device with no buffer device addresses - and must build an empty "
      "volume, not crash. Was JAH_VCT_REFUSE_GEOMETRY (a per-build read)." },
    { "atom.discriminate", 0.0, 0.0, 2.0,
      "THE DECODE'S DISCRIMINATOR (a test door, ATOM-BLACK-FRAMES-1): 1 = a colour code per failed "
      "validity term instead of the discard, 2 = also the code chart. 0 = shipped. scale's decode "
      "rows. Was JAHSHAKA_ATOM_DISCRIMINATE (a per-pass read)." },
    { "atom.hitWorldLights", 0.0, 0.0, 2.0,
      "The ray-hit decode's light list: 0 = shipped (the world list where the hit has no cell), "
      "1 = off (the picture before the list), 2 = all (every hit the world list, no Forward+ cell). "
      "gi.hit_shade. Was JAHSHAKA_HIT_WORLD_LIGHTS (a per-pass read)." },
    { "atom.hitVctSpecular", 1.0, 0.0, 1.0,
      "The VCT specular cone in the ray-hit decode. 0 = compiled out (vct_disable_specular). "
      "gi.hit_shade. Was JAHSHAKA_HIT_VCT_SPECULAR (a per-pass read)." },
};
static_assert(sizeof(kArms) / sizeof(kArms[0]) == unsigned(ArmId::Count),
              "ArmId grew: give the new arm its row in kArms, in the same order");
}   // namespace

ArmRegistry::ArmRegistry() {
    for (unsigned i = 0; i < unsigned(ArmId::Count); ++i) mPending[i] = mLive[i] = kArms[i].def;
}

bool ArmRegistry::set(const std::string &name, double value, std::string &err) {
    for (unsigned i = 0; i < unsigned(ArmId::Count); ++i) {
        if (name != kArms[i].name) continue;
        if (!(value >= kArms[i].lo && value <= kArms[i].hi)) {   // a NaN fails too
            err = "arm " + name + ": " + std::to_string(value) + " is outside [" +
                  std::to_string(kArms[i].lo) + ", " + std::to_string(kArms[i].hi) + "]";
            return false;
        }
        mPending[i] = value;
        return true;
    }
    err = "no arm named '" + name + "' (Engine::arms() lists them)";
    return false;
}

std::vector<ArmInfo> ArmRegistry::list() const {
    std::vector<ArmInfo> out;
    for (unsigned i = 0; i < unsigned(ArmId::Count); ++i) {
        ArmInfo a;
        a.name = kArms[i].name;
        a.value = mLive[i];
        a.defaultValue = kArms[i].def;
        a.minValue = kArms[i].lo;
        a.maxValue = kArms[i].hi;
        a.what = kArms[i].what;
        out.push_back(std::move(a));
    }
    return out;
}

bool OgreEngine::setArm(const std::string &name, double value) {
    return mArms.set(name, value, mLastError);
}

std::vector<ArmInfo> OgreEngine::arms() const { return mArms.list(); }

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

// WHAT THE NEXT FRAME MAY PUT OFF (OPEN_COVER_SPEC §2.1). Consumed by
// `renderOneFrame` and reset to `Complete`, like the cause above — and unlike
// the cause it changes what the frame DOES, so the reset is the safety rule,
// not bookkeeping.
void OgreEngine::setNextFramePace(FramePace pace) { mNextFramePace = pace; }

bool OgreEngine::framePaceOwesWork() const
{
    for (const auto &sc : mScenes)
        if (sc->giBuildOwesWork()) return true;
    return false;
}

// WHAT THE WORLD STILL OWES, FOR THE ONE LINE THAT SAYS SO (§2.1, §3, lane
// OPEN-COVER-2b). The same scenes `framePaceOwesWork` walks, with the counts
// instead of the verdict — so the host's indicator and its slow-frame line read
// the same state the pace rule reads, and cannot drift from it.
//
// EVERY FIELD IS ALREADY KEPT. Nothing here computes anything: the arm's stage
// is the scene's own state machine, the texture term is the list
// `settleTextureResidency` spends, and the shader pair comes from the cache's
// `progress()` — DELIBERATELY not from `shaderCacheStats()`, which stats every
// file in the cache directory to report its size and is therefore a once-a-load
// call, not a once-a-frame one.
StreamingWork OgreEngine::streamingWork() const
{
    StreamingWork out;
    for (const auto &sc : mScenes) {
        // The stage reported is the FURTHEST-BEHIND scene's: there is one world
        // arriving at a time in this application, and a second scene that owes
        // nothing must not overwrite the one that does.
        const unsigned left = sc->giBuildStagesLeft();
        if (left > out.giStagesLeft) {
            out.giStagesLeft = left;
            out.giStage = sc->giBuildStage();
        }
        out.materialsAwaitingTexture += sc->materialsAwaitingTexture();
        out.giRebuilds += sc->giRebuildCount();
    }
    unsigned fromCache = 0u, expected = 0u;
    mShaderCache.progress(out.shadersCompiled, fromCache, expected);
    return out;
}

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
        const auto attach = [&](Ogre::CompositorWorkspace *w) {
            if (!w) return;
            const Ogre::CompositorWorkspaceListenerVec &ls = w->getListeners();
            if (std::find(ls.begin(), ls.end(), &mMonitor->mListener) == ls.end())
                w->addListener(&mMonitor->mListener);
            ++attached;
        };
        for (auto &v : mViews) {
            if (!v->isEnabled()) continue;
            v->addWorkspaceListener(&mMonitor->mListener);   // idempotent, rides rebuilds
            if (v->workspace()) ++attached;
            // ...AND ITS OTHER WORKSPACES (lane TEST-1, F2): the inset, the blank one.
            std::vector<Ogre::CompositorWorkspace *> ws;
            v->monitorWorkspaces(ws);
            for (Ogre::CompositorWorkspace *w : ws) attach(w);
        }
        // THE HEADSET'S DESKTOP MIRROR (F2): a workspace of the session's own.
        if (mVrSession) attach(vrSessionMirrorWorkspace(mVrSession));
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
                       "(JAH_GPU_TIMESTAMPS is off — a production build)";
        return;
    }
    // LOCK 2, THE RUNTIME. `available` is true only while a query pool exists,
    // and a pool exists only inside a capture.
    st.gpuSupported = true;
    st.gpuActive = available;
    st.gpuQueryPools = 0u;
    if (available) {
        Ogre::uint32 pools = 0u;
        try { rs->getCustomAttribute("JahGpuQueryPools", &pools); } catch (...) {}
        st.gpuQueryPools = unsigned(pools);
    }
    if (available && mMonitor) st.gpuFramesAgedOut = mMonitor->mGpuFramesAgedOut;
    if (available && mMonitor) st.gpuMarksDropped = mMonitor->gpuMarksDropped();
    if (!available)
        st.gpuReason = mMonitor ? "the device or queue has no usable timestamps"
                                : "no capture is running (no query pool exists)";
    else
        st.gpuReason.clear();
}

// The frame's GPU bookkeeping: collect every query pool whose results are back
// (the render system answers each sample once — a time, or negative for "never"),
// and reset a free pool for this frame. ONE call,
// at the top of the frame and outside every encoder — which is the only place
// vkCmdResetQueryPool is legal (see fork 1a81f866a+1bccc3f93 (was 0027)).
//
// THE TURNOVER RUNS AT THE CLOSE OF A FRAME (lane TEST-1, F5), not at the top of
// the next: closeRenderFrame calls it after the record is closed, and then opens
// the NEXT frame's own pair, so everything recorded before that frame's passes —
// the mirror's between-frames GI half, which its record adopts — is inside the
// span it reports. Between frames nothing is open (the swap ended every encoder),
// which is all vkCmdResetQueryPool asks. The top of renderOneFrame turns over only
// on a capture's first frame (`mTurnedOver` false).
void OgreEngine::gpuFrameBegin() {
    if (!mMonitor || !mMonitor->mGpu) return;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    if (!rs) return;
    // An open pair belongs to the pool being turned over: close it first (the
    // fork clears its stack at the turnover and would never answer it).
    mMonitor->closeFrameSample();
    ++mMonitor->mTurnovers;
    try {
        rs->getCustomAttribute("JahGpuFrameBegin", nullptr);
        // THE MARKS THE POOL COULD NOT HOLD in the frame that just ended — the
        // fork's count for the pool it has just recycled, read EVERY frame so no
        // dropping frame goes unrecorded between two host drains.
        Ogre::uint32 dropped = 0u;
        rs->getCustomAttribute("JahGpuSamplesTruncated", &dropped);
        mMonitor->noteGpuMarksDropped(unsigned(dropped));
        std::vector<std::pair<Ogre::uint32, float>> results;
        rs->getCustomAttribute("JahGpuSampleResults", &results);
        for (const auto &r : results) mMonitor->noteGpuSample(unsigned(r.first), r.second);
        mMonitor->openFrameSample(rs);
        mMonitor->mTurnedOver = true;
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
