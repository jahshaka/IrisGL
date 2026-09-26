// ATOM P3's SUBSTRATE — THE GENERIC CULL: test, compact, and write the draw
// commands, all three on the device (SPECS/atom/A4_SUBSTRATE_CULL_DESIGN.md
// section 1; PHOTON_ATOM_CONTRACT.md section 3's cull row).
//
// WHAT IT IS. One entry point, `OgreScene::runGpuCull`, that takes a consumer's
// request and leaves the answer in device buffers. Three compute jobs:
//
//   1. Jahshaka/CullTest    — one thread per instance of the GPU scene's table:
//                             predicates, frustum, the optional depth pyramid,
//                             and the level rule. Writes a visibility word and a
//                             per-slot level.
//   2. Jahshaka/CullCompact — a shared-memory prefix sum per workgroup plus one
//                             global atomic for its base: the bitmap becomes a
//                             LIST and the count is written by the GPU.
//   3. Jahshaka/CullDraws   — one VkDrawIndexedIndirectCommand per survivor,
//                             DISPATCHED INDIRECTLY off job 2's count
//                             (ogre-patch 0032). After job 1 is dispatched no
//                             host learns how much work there is.
//
// THE HOST'S WHOLE SHARE is the request: 224 bytes and a zeroed 32-byte counter,
// uploaded, and three dispatches recorded. `GpuCullResult::requestMs` measures
// it rather than claiming it.
//
// WHY THE CONSUMER IS A SUITE TODAY. The contract makes Atom own the job and
// write it generically; Photon's ray tiles (GA-2) and Atom's own clustered pass
// (stage 3) are the readers, and neither exists. So the substrate ships with
// `engine.gpu_cull` as its only caller, which is also why this lane moves no
// pixel: nothing in a frame reads a survivor list yet.
//
// THE BARRIERS. Every hand-off between the jobs goes through Ogre's
// BarrierSolver (`analyzeBarriers` + `executeResourceTransition`), which is
// exactly right for compute-write -> compute-read. The ONE edge it cannot
// express is compute-write -> indirect-read, and patch 0032 issues that by hand
// inside `_dispatchIndirect`; the stage-0 spike measured the same edge with
// synchronization validation clean on both devices (FINDINGS 7.2), and
// NANITE_SPEC's N-3 stays closed.
#include "EnginePrivate.h"
#include "GpuCull.h"
#include "GpuScene.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreMesh2.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

GpuCull::~GpuCull() { destroy(); }

void GpuCull::destroy() {
    if (mVao) {
        if (mCutStream) mVao->destroyUavBuffer(mCutStream);
        if (mCutTriWords) mVao->destroyUavBuffer(mCutTriWords);
        if (mCutRecords) mVao->destroyUavBuffer(mCutRecords);
        if (mCutSlotBase) mVao->destroyUavBuffer(mCutSlotBase);
        if (mDraws) mVao->destroyUavBuffer(mDraws);
        if (mHeld) mVao->destroyUavBuffer(mHeld);
        if (mCount) mVao->destroyUavBuffer(mCount);
        if (mSurvivors) mVao->destroyUavBuffer(mSurvivors);
        if (mLevels) mVao->destroyUavBuffer(mLevels);
        if (mVisible) mVao->destroyUavBuffer(mVisible);
        if (mParams) mVao->destroyUavBuffer(mParams);
    }
    mDraws = mCount = mSurvivors = mLevels = mVisible = mParams = mHeld = nullptr;
    mCutStream = mCutTriWords = mCutRecords = mCutSlotBase = nullptr;
    mCutIndexBudget = mCutRecordBudget = mCutSlotCapacity = 0u;
    mVao = nullptr;
    mCapacity = 0u;
}

/// THE CUT'S BUFFERS. The stream, its triangle words and the records are sized by
/// the BUDGET (never per frame: re-created only when an overflow raised it), the
/// per-slot base by the list's slot capacity. Nothing in them is state between
/// requests — every word a reader reads is written by the request it reads.
bool GpuCull::ensureCut(Ogre::VaoManager *vao, std::string &err, uint32_t sceneNeed) {
    if (!mParams || vao != mVao) {
        err = "gpucull: the cut's buffers need the list's (ensure first)";
        return false;
    }
    if (!mCutSlotBase || mCutSlotCapacity < mCapacity) {
        if (mCutSlotBase) vao->destroyUavBuffer(mCutSlotBase);
        mCutSlotBase = vao->createUavBuffer(mCapacity, sizeof(uint32_t), 0, nullptr, false);
        mCutSlotCapacity = mCapacity;
    }
    if (!mCutIgnoreScene && sceneNeed) {
        // The main region must hold the scene's need: budget x 7/8 >= need.
        uint32_t want = mCutWantBudget;
        while (want < kCutIndexBudgetCeiling && want - want / 8u < sceneNeed) want *= 2u;
        mCutWantBudget = std::min(want, kCutIndexBudgetCeiling);
    }
    if (mCutStream && mCutIndexBudget >= mCutWantBudget && !mCutForceCreate) return true;
    mCutForceCreate = false;
    if (mCutStream) vao->destroyUavBuffer(mCutStream);
    if (mCutTriWords) vao->destroyUavBuffer(mCutTriWords);
    if (mCutRecords) vao->destroyUavBuffer(mCutRecords);
    mCutIndexBudget = mCutWantBudget;
    mCutRecordBudget = mCutIndexBudget / kCutIndicesPerRecord;
    mCutStream = vao->createUavBuffer(mCutIndexBudget, sizeof(uint32_t), 0, nullptr, false);
    mCutTriWords = vao->createUavBuffer(size_t(mCutIndexBudget / 3u) * 2u, sizeof(uint32_t), 0, nullptr, false);
    mCutRecords = vao->createUavBuffer(mCutRecordBudget, 4u * sizeof(uint32_t), 0, nullptr, false);
    return mCutStream && mCutTriWords && mCutRecords;
}

void GpuCull::noteCutOverflow(uint32_t overflowedInstances, uint32_t indicesAsked, uint32_t recordsAsked) {
    if (!overflowedInstances) return;
    // SIZED FROM WHAT WAS ASKED, never a blind doubling: the ring reports the same
    // overflow for every frame in flight (4-5 of them), and a doubling per report
    // overshot a 209 k need to 4.4 M (measured, atom.cluster_crack E).
    uint32_t want = mCutWantBudget;
    while (want < kCutIndexBudgetCeiling &&
           (want - want / 8u < indicesAsked || want / kCutIndicesPerRecord < recordsAsked))
        want *= 2u;
    mCutWantBudget = std::min(want, kCutIndexBudgetCeiling);
}

/// `slotCapacity` IS THE TABLE'S CAPACITY AND NOT ITS COUNT, and the difference
/// is a re-create per request: the GPU scene's capacity doubles (64, 128, 256...)
/// and stands still between grows, while its COUNT moves every time an item is
/// attached — sizing to the count would destroy and rebuild all six buffers on
/// the first request after any attach, which is exactly the per-frame allocation
/// the header promises never happens.
bool GpuCull::ensure(Ogre::VaoManager *vao, uint32_t slotCapacity, std::string &err) {
    if (!vao) {
        err = "gpucull: no VaoManager (the NULL render system) — the cull is unsupported";
        return false;
    }
    if (mParams && mVao == vao && slotCapacity <= mCapacity) return true;
    // A GROW IS A RE-CREATE: the buffers hold no state between requests (every
    // one of them is written before it is read), so there is nothing to carry
    // over — which is what makes the grow a free operation rather than a copy.
    const uint32_t want = std::max(slotCapacity, 64u);
    destroy();
    mVao = vao;
    std::vector<uint32_t> zeros(size_t(want) * kDrawWords, 0u);
    GpuCullParams p;
    mParams = vao->createUavBuffer(1u, sizeof(GpuCullParams), 0, &p, false);
    mVisible = vao->createUavBuffer(want, sizeof(uint32_t), 0, zeros.data(), false);
    mLevels = vao->createUavBuffer(want, sizeof(uint32_t), 0, zeros.data(), false);
    mSurvivors = vao->createUavBuffer(want, sizeof(uint32_t), 0, zeros.data(), false);
    mCount = vao->createUavBuffer(kCountElements, sizeof(uint32_t), 0, zeros.data(), false);
    mDraws = vao->createUavBuffer(size_t(want) * kDrawWords, sizeof(uint32_t), 0, zeros.data(),
                                  false);
    {
        // Three words a slot: the held level, and the node id and mesh it was held for.
        const std::vector<uint32_t> none(size_t(want) * 3u, 0xFFFFFFFFu);
        mHeld = vao->createUavBuffer(want * 3u, sizeof(uint32_t), 0, const_cast<uint32_t *>(none.data()),
                                     false);
    }
    mCapacity = want;
    return mParams != nullptr;
}

namespace {

Ogre::DescriptorSetUav::BufferSlot cullSlot(Ogre::UavBufferPacked *buffer,
                                            Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = buffer;
    slot.offset = 0;
    slot.sizeBytes = 0;   // to the end
    slot.access = access;
    return slot;
}

/// Reads `count` uints out of a UAV buffer. `readRequest` records the download
/// and the ticket's map() waits on Ogre's own fence for it — the documented
/// route, and it covers the compute writes (VulkanQueue::prepareForDownload
/// charges a UAV source with SHADER_WRITE from every stage before the copy).
void readUints(Ogre::UavBufferPacked *buffer, uint32_t first, uint32_t count,
               std::vector<unsigned> &out) {
    out.assign(count, 0u);
    if (!count) return;
    Ogre::AsyncTicketPtr ticket = buffer->readRequest(first, count);
    std::memcpy(out.data(), ticket->map(), size_t(count) * sizeof(uint32_t));
    ticket->unmap();
}

/// Clears every binding of the three jobs. Safe on a job that was never bound,
/// and it must stay that way: the failure path calls it at any point in the
/// sequence. THE JOBS OUTLIVE A REQUEST (they live in HlmsCompute) and their
/// descriptor sets hold RAW pointers, so leaving a slot pointing at a buffer a
/// later grow destroyed is a dangling read in whoever dispatches next.
void unbindCullJobs(Ogre::HlmsComputeJob *test, Ogre::HlmsComputeJob *compact,
                    Ogre::HlmsComputeJob *draws) {
    const Ogre::DescriptorSetUav::BufferSlot empty =
        Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    if (test)
        for (uint8_t i = 0; i < 7u; ++i) test->_setUavBuffer(i, empty);
    if (compact)
        for (uint8_t i = 0; i < 4u; ++i) compact->_setUavBuffer(i, empty);
    if (draws) {
        for (uint8_t i = 0; i < 6u; ++i) draws->_setUavBuffer(i, empty);
        draws->setIndirectDispatchBuffer(0);
    }
}

/// The cut's two jobs, cleared the same way.
void unbindCutJobs(Ogre::HlmsComputeJob *cut, Ogre::HlmsComputeJob *emit) {
    const Ogre::DescriptorSetUav::BufferSlot empty =
        Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    if (cut) {
        for (uint8_t i = 0; i < 10u; ++i) cut->_setUavBuffer(i, empty);
        cut->setIndirectDispatchBuffer(0);
    }
    if (emit) {
        for (uint8_t i = 0; i < 9u; ++i) emit->_setUavBuffer(i, empty);
        emit->setIndirectDispatchBuffer(0);
    }
}

/// One dispatch with the barriers its bindings imply.
void dispatchWithBarriers(Ogre::RenderSystem *rs, Ogre::HlmsCompute *hc,
                          Ogre::HlmsComputeJob *job) {
    Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
    job->analyzeBarriers(rt);
    rs->executeResourceTransition(rt);
    hc->dispatch(job, 0, 0);
}

/// THE COST OF ONE JOB, as the queue sees it. There are no per-dispatch GPU
/// timestamps available outside a compositor pass at this pin (patch 0027's
/// samples are keyed to passes and come back two frames late), so the number is
/// a SLOPE and says so: the job is dispatched `iterations` more times over the
/// buffers it has already filled, the command buffer is flushed, and the wall
/// clock of that is divided by the count after an empty flush's own cost has
/// been taken off. It is therefore an upper bound on the GPU time and includes
/// the per-dispatch driver cost, which is the honest thing to compare against a
/// CPU cull anyway.
double measureJob(Ogre::RenderSystem *rs, Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job,
                  unsigned iterations) {
    if (!iterations) return -1.0;
    const auto t0 = std::chrono::steady_clock::now();
    rs->flushCommands();
    const auto t1 = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < iterations; ++i) dispatchWithBarriers(rs, hc, job);
    rs->flushCommands();
    const auto t2 = std::chrono::steady_clock::now();
    const double empty = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double full = std::chrono::duration<double, std::milli>(t2 - t1).count();
    return std::max(0.0, (full - empty)) / double(iterations);
}

}  // namespace

// ---------------------------------------------------------------------------
// THE RECORDING HALF — the request uploaded and the three jobs dispatched, NOTHING
// read back: what a frame's own consumer (the visibility buffer's id pass,
// OgreAtomIdPass.cpp) calls from inside a pass, where a readback would stall the
// frame. The answer stays in the cull's buffers (count, survivors, levels, draws).
bool OgreScene::recordGpuCull(GpuCull &cull, const GpuCullRequest &req, Ogre::TextureGpu *hzb,
                              std::string &err, bool keepBindings, double *requestMs) {
    ensureGpuTables();
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    Ogre::HlmsCompute *hc =
        mRoot && mRoot->getHlmsManager() ? mRoot->getHlmsManager()->getComputeHlms() : nullptr;
    if (!rs || !hc || !mGpuScene.live() || !rs->supportsIndirectDispatch()) {
        err = "the GPU cull has no device here (no compute, no GPU scene or no indirect dispatch)";
        return false;
    }
    Ogre::HlmsComputeJob *test = hc->findComputeJobNoThrow("Jahshaka/CullTest");
    Ogre::HlmsComputeJob *compact = hc->findComputeJobNoThrow("Jahshaka/CullCompact");
    Ogre::HlmsComputeJob *draws = hc->findComputeJobNoThrow("Jahshaka/CullDraws");
    if (!test || !compact || !draws) {
        err = "engine: the cull compute jobs are missing — "
              "media/Hlms/Jahshaka/JahshakaCompute.material.json is not staged";
        return false;
    }
    const uint32_t instances = mGpuScene.slotCount();
    // THE TABLE'S CAPACITY, not this request's instance count (see `ensure`).
    if (!cull.ensure(rs->getVaoManager(), std::max(mGpuScene.slotCapacity(), 1u), err))
        return false;
    // THE CUT (mode 3): its two jobs and its per-frame buffers at the current budget,
    // and the DAG tables on the device before anything binds them.
    const bool cutMode = req.mode >= 3u;
    Ogre::HlmsComputeJob *cutJob = cutMode ? hc->findComputeJobNoThrow("Jahshaka/CullCut") : nullptr;
    Ogre::HlmsComputeJob *emitJob = cutMode ? hc->findComputeJobNoThrow("Jahshaka/CullEmit") : nullptr;
    if (cutMode) {
        if (!cutJob || !emitJob) {
            err = "engine: the cut's compute jobs are missing — "
                  "media/Hlms/Jahshaka/JahshakaCompute.material.json is not staged";
            return false;
        }
        mGpuScene.flushClusterTables();
        mGpuScene.flushGeomRows();
        if (!cull.ensureCut(rs->getVaoManager(), err, mCutIndexNeed)) return false;
        if (!mGpuScene.clusterBuffer() || !mGpuScene.groupBuffer()) {
            err = "the cut's cluster tables could not be created";
            return false;
        }
    }

    // THE HOST'S SHARE (GpuCullResult::requestMs): the request's write and the three
    // dispatches' recording — never the tables' or the list's (re)allocation above.
    const auto tRequest = std::chrono::steady_clock::now();
    JAH_TRY {
        // ---- the request, and the counter it has to start from -------------
        GpuCullParams p;
        std::memcpy(p.planes, req.planes, sizeof(p.planes));
        std::memcpy(p.viewProjRow, req.viewProj, sizeof(p.viewProjRow));
        for (int i = 0; i < 3; ++i) p.eye[i] = req.eye[i];
        p.eye[3] = req.lodHysteresis;
        p.lod[0] = req.pixelTolerance;
        p.lod[1] = req.projScaleY;
        p.lod[2] = req.viewportHeight;
        p.lod[3] = req.orthographic ? 1.0f : 0.0f;
        p.counts[0] = instances;
        p.counts[1] = req.flagsRequired;
        p.counts[2] = req.flagsForbidden;
        p.counts[3] = req.mode;
        p.hzb[0] = hzb ? req.hzbLevels : 0u;
        p.hzb[1] = hzb ? uint32_t(hzb->getWidth()) : 0u;
        p.hzb[2] = hzb ? uint32_t(hzb->getHeight()) : 0u;
        p.hzb[3] = rs->isReverseDepth() ? 1u : 0u;
        p.cut[0] = cutMode ? cull.cutIndexBudget() : 0u;
        p.cut[1] = cutMode ? cull.cutRecordBudget() : 0u;
        p.cut[2] = cutMode ? cull.cutMainBudget() : 0u;
        cull.params()->upload(&p, 0, 1u);
        const uint32_t reset[GpuCull::kCountElements] = { 0u, 0u, 1u, 1u, 0u, 0u, 1u, 1u,
                                                          0u, 1u, 1u, 0u, 0u, 0u, 0u, 0u };
        cull.count()->upload(reset, 0, GpuCull::kCountElements);

        // ---- job 1: test ---------------------------------------------------
        // THE PYRAMID IS A PERMUTATION (`cull_hzb`), and it has to be: a compute
        // job that declares a texture unit with nothing in it segfaults inside
        // Ogre's own DescriptorSetTexture2::checkValidity when it dispatches
        // (measured 2026-09-22), and binding a dummy to satisfy the check would
        // put a texture in the root layout that the shader never reads. Both
        // permutations keep FIXED-SIZE bindings, which is what the microcode
        // cache needs (patch 0063 skips a shader that reflects array bindings).
        test->setProperty("cull_hzb", hzb ? 1 : 0);
        test->setNumTexUnits(hzb ? 1u : 0u);
        if (hzb) {
            Ogre::DescriptorSetTexture2::TextureSlot ts =
                Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
            ts.texture = hzb;
            test->setTexture(0u, ts);
        }
        test->_setUavBuffer(0u, cullSlot(cull.params(), Ogre::ResourceAccess::Read));
        test->_setUavBuffer(1u, cullSlot(mGpuScene.instanceBuffer(), Ogre::ResourceAccess::Read));
        test->_setUavBuffer(2u, cullSlot(mGpuScene.meshBuffer(), Ogre::ResourceAccess::Read));
        test->_setUavBuffer(3u, cullSlot(mGpuScene.levelBuffer(), Ogre::ResourceAccess::Read));
        test->_setUavBuffer(4u, cullSlot(cull.visible(), Ogre::ResourceAccess::Write));
        test->_setUavBuffer(5u, cullSlot(cull.levels(), Ogre::ResourceAccess::Write));
        test->_setUavBuffer(6u, cullSlot(cull.held(), Ogre::ResourceAccess::ReadWrite));
        const uint32_t groups =
            (instances + GpuCull::kThreadsPerGroup - 1u) / GpuCull::kThreadsPerGroup;
        test->setNumThreadGroups(std::max(groups, 1u), 1u, 1u);

        // ---- job 2: compact ------------------------------------------------
        compact->_setUavBuffer(0u, cullSlot(cull.params(), Ogre::ResourceAccess::Read));
        compact->_setUavBuffer(1u, cullSlot(cull.visible(), Ogre::ResourceAccess::Read));
        compact->_setUavBuffer(2u, cullSlot(cull.survivors(), Ogre::ResourceAccess::Write));
        compact->_setUavBuffer(3u, cullSlot(cull.count(), Ogre::ResourceAccess::ReadWrite));
        compact->setNumThreadGroups(std::max(groups, 1u), 1u, 1u);

        // ---- job 3: the draw commands, sized by the GPU --------------------
        draws->_setUavBuffer(0u, cullSlot(mGpuScene.instanceBuffer(), Ogre::ResourceAccess::Read));
        draws->_setUavBuffer(1u, cullSlot(mGpuScene.levelBuffer(), Ogre::ResourceAccess::Read));
        draws->_setUavBuffer(2u, cullSlot(cull.survivors(), Ogre::ResourceAccess::Read));
        draws->_setUavBuffer(3u, cullSlot(cull.levels(), Ogre::ResourceAccess::Read));
        draws->_setUavBuffer(4u, cullSlot(cull.draws(), Ogre::ResourceAccess::Write));
        // ReadWrite: the job adds up the triangles its commands draw (count[4], the
        // id pass's share of the frame's stats — OgreAtomIdPass.cpp's stats ring).
        draws->_setUavBuffer(5u, cullSlot(cull.count(), Ogre::ResourceAccess::ReadWrite));
        draws->setIndirectDispatchBuffer(cull.count(), GpuCull::kIndirectOffsetBytes);

        // ---- the cut (mode 3): job 3' per survivor, job 4' per drawn cluster ----
        if (cutMode) {
            cutJob->_setUavBuffer(0u, cullSlot(cull.params(), Ogre::ResourceAccess::Read));
            cutJob->_setUavBuffer(1u, cullSlot(mGpuScene.instanceBuffer(), Ogre::ResourceAccess::Read));
            cutJob->_setUavBuffer(2u, cullSlot(mGpuScene.meshBuffer(), Ogre::ResourceAccess::Read));
            cutJob->_setUavBuffer(3u, cullSlot(mGpuScene.clusterBuffer(), Ogre::ResourceAccess::Read));
            cutJob->_setUavBuffer(4u, cullSlot(mGpuScene.groupBuffer(), Ogre::ResourceAccess::Read));
            cutJob->_setUavBuffer(5u, cullSlot(cull.survivors(), Ogre::ResourceAccess::Read));
            cutJob->_setUavBuffer(6u, cullSlot(cull.count(), Ogre::ResourceAccess::ReadWrite));
            cutJob->_setUavBuffer(7u, cullSlot(cull.draws(), Ogre::ResourceAccess::Write));
            cutJob->_setUavBuffer(8u, cullSlot(cull.cutRecords(), Ogre::ResourceAccess::Write));
            cutJob->_setUavBuffer(9u, cullSlot(cull.cutSlotBase(), Ogre::ResourceAccess::Write));
            cutJob->setIndirectDispatchBuffer(cull.count(), GpuCull::kCutIndirectOffsetBytes);
            emitJob->_setUavBuffer(0u, cullSlot(cull.params(), Ogre::ResourceAccess::Read));
            emitJob->_setUavBuffer(1u, cullSlot(cull.cutRecords(), Ogre::ResourceAccess::Read));
            emitJob->_setUavBuffer(2u, cullSlot(mGpuScene.instanceBuffer(), Ogre::ResourceAccess::Read));
            emitJob->_setUavBuffer(3u, cullSlot(mGpuScene.meshBuffer(), Ogre::ResourceAccess::Read));
            emitJob->_setUavBuffer(4u, cullSlot(mGpuScene.clusterBuffer(), Ogre::ResourceAccess::Read));
            emitJob->_setUavBuffer(5u, cullSlot(mGpuScene.geomBuffer(), Ogre::ResourceAccess::Read));
            emitJob->_setUavBuffer(6u, cullSlot(cull.cutStream(), Ogre::ResourceAccess::Write));
            emitJob->_setUavBuffer(7u, cullSlot(cull.cutTriWords(), Ogre::ResourceAccess::Write));
            emitJob->_setUavBuffer(8u, cullSlot(cull.count(), Ogre::ResourceAccess::Read));
            emitJob->setIndirectDispatchBuffer(cull.count(), GpuCull::kEmitIndirectOffsetBytes);
        }
        if (requestMs)
            *requestMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tRequest)
                             .count();

        dispatchWithBarriers(rs, hc, test);
        dispatchWithBarriers(rs, hc, compact);
        if (req.mode == 2u) dispatchWithBarriers(rs, hc, draws);
        if (cutMode) {
            dispatchWithBarriers(rs, hc, cutJob);
            dispatchWithBarriers(rs, hc, emitJob);
        }
        // The descriptor sets the dispatches bound were built at dispatch time;
        // the jobs' CPU-side bindings go now (a later grow must not find them),
        // unless a measurement re-dispatches them as they stand.
        if (!keepBindings) {
            unbindCullJobs(test, compact, draws);
            unbindCutJobs(cutJob, emitJob);
        }
        return true;
    }
    catch (Ogre::Exception &e) {
        err = e.getFullDescription();
        unbindCullJobs(test, compact, draws);
        unbindCutJobs(cutJob, emitJob);
        return false;
    }
}

bool OgreScene::runGpuCull(const GpuCullRequest &req, Ogre::TextureGpu *hzb, bool readBack,
                           GpuCullResult &out) {
    out = GpuCullResult();
    ensureGpuTables();
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    Ogre::HlmsCompute *hc =
        mRoot && mRoot->getHlmsManager() ? mRoot->getHlmsManager()->getComputeHlms() : nullptr;
    if (!rs || !hc || !mGpuScene.live()) return false;
    out.supported = rs->supportsIndirectDispatch();
    if (!out.supported) return false;
    const uint32_t instances = mGpuScene.slotCount();
    out.instances = instances;

    std::string err;
    if (!recordGpuCull(mGpuCull, req, hzb, err, req.measureIterations > 0u, &out.requestMs)) {
        mError = err;
        out.supported = false;
        return false;
    }

    JAH_TRY {
        // ---- what the GPU decided ------------------------------------------
        std::vector<unsigned> counter;
        readUints(mGpuCull.count(), 0u, GpuCull::kCountElements, counter);
        out.survivors = counter.size() > 0 ? counter[0] : 0u;
        out.indirectGroups = counter.size() > 1 ? counter[1] : 0u;
        if (out.survivors > instances) out.survivors = instances;

        if (req.mode >= 1u || readBack)
            readUints(mGpuCull.levels(), 0u, instances, out.levels);
        if (readBack && out.survivors)
            readUints(mGpuCull.survivors(), 0u, out.survivors, out.survivorSlots);
        if (req.mode >= 3u) {
            out.draws = out.survivors;
            out.cutClusters = std::min(counter[8], mGpuCull.cutRecordBudget());
            out.cutIndices = counter[11];
            out.cutTriangles = counter[4];
            out.cutOverflow = counter[12];
            out.cutMissing = counter[13];
            out.cutOverflowIndices = counter[11];
            out.cutEvaluated = counter[14];
            out.cutIndexBudget = mGpuCull.cutIndexBudget();
            if (readBack && out.survivors)
                readUints(mGpuCull.draws(), 0u, out.survivors * GpuCull::kDrawWords, out.drawCommands);
            if (readBack && out.cutClusters) {
                std::vector<unsigned> rec;
                readUints(mGpuCull.cutRecords(), 0u, out.cutClusters * 4u, rec);
                out.cutDrawn.reserve(size_t(out.cutClusters) * 3u);
                for (unsigned r = 0; r < out.cutClusters; ++r) {
                    const unsigned slot = rec[r * 4u], global = rec[r * 4u + 1u];
                    if (slot >= mGpuScene.slotCapacity()) continue;   // a skipped (overflow) record
                    uint32_t meshIndex = 0u;
                    std::memcpy(&meshIndex, &mGpuScene.entry(slot).boundsMin[3], sizeof(uint32_t));
                    const unsigned base = meshIndex < mGpuScene.meshEntryCount()
                                              ? mGpuScene.meshEntry(meshIndex).dag[0] : 0u;
                    out.cutDrawn.push_back(slot);
                    out.cutDrawn.push_back(global - base);
                    out.cutDrawn.push_back(rec[r * 4u + 3u]);
                }
            }
            if (req.measureIterations) {
                Ogre::HlmsComputeJob *cutJob = hc->findComputeJobNoThrow("Jahshaka/CullCut");
                Ogre::HlmsComputeJob *emitJob = hc->findComputeJobNoThrow("Jahshaka/CullEmit");
                // THE CUT JOB ACCUMULATES (its cursors), so each measured dispatch starts
                // from the counters the compaction left: survivors and the dispatch
                // arguments kept, the cut's own words zeroed. The emit job only reads.
                std::vector<uint32_t> again(counter.begin(), counter.end());
                again[4] = again[8] = again[11] = again[12] = again[13] = again[14] = again[15] = 0u;
                const auto t0 = std::chrono::steady_clock::now();
                rs->flushCommands();
                const auto t1 = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < req.measureIterations; ++i) {
                    mGpuCull.count()->upload(again.data(), 0, GpuCull::kCountElements);
                    dispatchWithBarriers(rs, hc, cutJob);
                }
                rs->flushCommands();
                const auto t2 = std::chrono::steady_clock::now();
                // ...minus the counter uploads alone.
                for (unsigned i = 0; i < req.measureIterations; ++i)
                    mGpuCull.count()->upload(again.data(), 0, GpuCull::kCountElements);
                rs->flushCommands();
                const auto t3 = std::chrono::steady_clock::now();
                const double empty = std::chrono::duration<double, std::milli>(t1 - t0).count();
                const double full = std::chrono::duration<double, std::milli>(t2 - t1).count();
                const double uploads = std::chrono::duration<double, std::milli>(t3 - t2).count();
                out.cutMs = std::max(0.0, full - std::max(empty, uploads)) / double(req.measureIterations);
                out.emitMs = measureJob(rs, hc, emitJob, req.measureIterations);
                unbindCutJobs(cutJob, emitJob);
            }
        }
        if (req.mode == 2u) {
            out.draws = out.survivors;
            if (readBack && out.survivors)
                readUints(mGpuCull.draws(), 0u, out.survivors * GpuCull::kDrawWords,
                          out.drawCommands);
            // THE SUBMESH FINDING, counted rather than glossed: the level table
            // holds submesh 0's ranges only, so a survivor whose mesh has more
            // needs commands this table cannot describe.
            std::vector<unsigned> slots = out.survivorSlots;
            if (slots.empty() && out.survivors)
                readUints(mGpuCull.survivors(), 0u, out.survivors, slots);
            for (unsigned slot : slots) {
                if (slot >= mGpuScene.slotCapacity()) continue;
                uint32_t meshIndex = 0u;
                std::memcpy(&meshIndex, &mGpuScene.entry(slot).boundsMin[3], sizeof(uint32_t));
                const Ogre::MeshPtr &mesh = meshIndex == GpuScene::kNoMesh
                                                ? Ogre::MeshPtr()
                                                : mGpuScene.meshAt(meshIndex);
                if (mesh && mesh->getNumSubMeshes() > 1u) ++out.multiSubmeshSurvivors;
            }
        }

        if (req.measureIterations) {
            Ogre::HlmsComputeJob *test = hc->findComputeJobNoThrow("Jahshaka/CullTest");
            Ogre::HlmsComputeJob *compact = hc->findComputeJobNoThrow("Jahshaka/CullCompact");
            Ogre::HlmsComputeJob *draws = hc->findComputeJobNoThrow("Jahshaka/CullDraws");
            // The request kept the bindings for this: each job re-dispatches
            // over the buffers the request filled.
            out.testMs = measureJob(rs, hc, test, req.measureIterations);
            out.compactMs = measureJob(rs, hc, compact, req.measureIterations);
            if (req.mode == 2u) out.drawsMs = measureJob(rs, hc, draws, req.measureIterations);
            unbindCullJobs(test, compact, draws);
        }
        return true;
    }
    catch (Ogre::Exception &e) {
        mError = e.getFullDescription();
        if (req.measureIterations) {
            unbindCullJobs(hc->findComputeJobNoThrow("Jahshaka/CullTest"),
                           hc->findComputeJobNoThrow("Jahshaka/CullCompact"),
                           hc->findComputeJobNoThrow("Jahshaka/CullDraws"));
            unbindCutJobs(hc->findComputeJobNoThrow("Jahshaka/CullCut"),
                          hc->findComputeJobNoThrow("Jahshaka/CullEmit"));
        }
        return false;
    }
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
