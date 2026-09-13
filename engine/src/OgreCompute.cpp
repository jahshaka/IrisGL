// PHOTON SHARED INFRASTRUCTURE — GPU-DRIVEN COMPUTE DISPATCH
// (SPECS/NANITE_SPEC.md §4.2; SPECS/research/LUMEN_SUPPORTING_TECH_2026-09-13.md §6;
//  ogre-patch 0032).
//
// WHY THIS EXISTS. Every Lumen-shaped stage narrows its work as it goes: screen
// traces that miss are compacted into a shorter list, that list is re-traced,
// what misses again is compacted once more. Epic's own words: "it was essential
// that we utilize indirect dispatch where supported", worth up to a 50% tracing
// speedup. Without it every later pass has to be dispatched at its WORST CASE
// from the CPU, because the CPU cannot know how many items survived.
//
// The pin had no such thing: the only dispatch in the Vulkan render system was
// vkCmdDispatch with CPU-side group counts, and HlmsComputeJob's
// "thread_groups_based_on_texture/uav" is CPU-side arithmetic over a resource's
// DIMENSIONS, not over anything the GPU computed. Patch 0032 adds
// HlmsComputeJob::setIndirectDispatchBuffer + RenderSystem::_dispatchIndirect,
// implemented on Vulkan as vkCmdDispatchIndirect plus the one barrier the
// BarrierSolver cannot express (compute SHADER_WRITE -> INDIRECT_COMMAND_READ at
// the DRAW_INDIRECT stage).
//
// WHAT IS IN THIS FILE. Only the PROOF: the capability has no consumer yet, and
// the arms it exists for are not built. `indirectDispatchProbe` runs the smallest
// honest two-job chain — count survivors, dispatch one group per survivor — and
// reports numbers a suite can assert. It allocates, dispatches, reads back and
// frees; it renders nothing and leaves no state behind.
#include "EnginePrivate.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreTextureGpu.h>
#include <OgreTextureGpuManager.h>
#include <OgreAsyncTextureTicket.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorNode.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace jahshaka { namespace engine {
namespace detail {

namespace {

/// The input list's length. 4096 items = 64 counting groups of 64 threads, and
/// 4096 is the largest case the brief asks for, so one size serves every case.
constexpr unsigned kListLength = 4096u;
/// Must match "Jahshaka/IndirectCount"'s threads_per_group in JahshakaCompute.material.json.
constexpr unsigned kCountThreadsPerGroup = 64u;

/// A UAV buffer slot, filled in. Ogre wants one of these per binding.
Ogre::DescriptorSetUav::BufferSlot bufferSlot(Ogre::UavBufferPacked *buffer,
                                             Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = buffer;
    slot.offset = 0;
    slot.sizeBytes = 0;   // to the end
    slot.access = access;
    return slot;
}

/// Reads a whole UAV buffer back to the CPU. `readRequest` records the download
/// and the ticket's map() waits on the fence Ogre took for it — that is the
/// documented route (Vao/OgreBufferPacked.h:264) and it covers the compute write
/// for us: VulkanQueue::prepareForDownload charges a BP_TYPE_UAV source with
/// SHADER_WRITE from every shader stage before the copy.
void readBack(Ogre::UavBufferPacked *buffer, std::vector<Ogre::uint32> &out) {
    out.assign(buffer->getNumElements(), 0u);
    Ogre::AsyncTicketPtr ticket = buffer->readRequest(0, buffer->getNumElements());
    const void *mapped = ticket->map();
    std::memcpy(out.data(), mapped, out.size() * sizeof(Ogre::uint32));
    ticket->unmap();
}

/// How many leading slots of a stamp buffer were written, and what they say.
/// The consumer job writes gl_NumWorkGroups.x into its own group's slot, so a
/// run of N identical non-zero values followed by zeros IS the group count, and
/// the value is what the GPU read out of the argument buffer.
void tallyStamps(const std::vector<Ogre::uint32> &stamps, unsigned &ranOut, unsigned &seenOut) {
    ranOut = 0u;
    seenOut = 0u;
    for (Ogre::uint32 v : stamps) {
        if (v == 0u) break;
        if (seenOut == 0u) seenOut = v;
        ++ranOut;
    }
}

/// Clears every binding the probe makes. Safe on a job that was never bound, and
/// it must stay that way: the failure path calls it at any point in the sequence.
void unbindProbeJobs(Ogre::HlmsComputeJob *countJob, Ogre::HlmsComputeJob *workJob,
                     Ogre::HlmsComputeJob *cpuJob) {
    const Ogre::DescriptorSetUav::BufferSlot empty =
        Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    if (countJob) {
        countJob->_setUavBuffer(0, empty);
        countJob->_setUavBuffer(1, empty);
    }
    if (workJob) {
        workJob->_setUavBuffer(0, empty);
        workJob->setIndirectDispatchBuffer(0);
    }
    if (cpuJob) cpuJob->_setUavBuffer(0, empty);
}

/// Frees whatever was created, in reverse order. Nulls are skipped, so it serves
/// a partial failure as well as a complete run.
void destroyProbeBuffers(Ogre::VaoManager *vao, Ogre::UavBufferPacked *srcBuf,
                         Ogre::UavBufferPacked *argBuf, Ogre::UavBufferPacked *outIndirect,
                         Ogre::UavBufferPacked *outCpu, Ogre::UavBufferPacked *outNoBarrier) {
    if (outNoBarrier) vao->destroyUavBuffer(outNoBarrier);
    if (outCpu) vao->destroyUavBuffer(outCpu);
    if (outIndirect) vao->destroyUavBuffer(outIndirect);
    if (argBuf) vao->destroyUavBuffer(argBuf);
    if (srcBuf) vao->destroyUavBuffer(srcBuf);
}

}  // namespace

bool OgreEngine::indirectDispatchProbe(unsigned survivors, IndirectDispatchProbe &out) {
    out = IndirectDispatchProbe();
    if (!mRoot) return false;
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    if (!rs) return false;
    out.supported = rs->supportsIndirectDispatch();
    if (!out.supported) return false;
    if (survivors > kListLength) survivors = kListLength;

    Ogre::VaoManager *vao = rs->getVaoManager();
    Ogre::HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms();
    if (!vao || !hc) return false;

    Ogre::HlmsComputeJob *countJob = hc->findComputeJobNoThrow("Jahshaka/IndirectCount");
    // TWO DEFINITIONS OF THE SAME SHADER, deliberately. `Jahshaka/IndirectWork`
    // declares no thread_groups at all, so its CPU-side count is zero and it can
    // only compile because patch 0032 relaxes that requirement for an indirectly
    // dispatched job — the relaxation is therefore EXERCISED by every run of this
    // probe rather than merely present in the patch. `Jahshaka/IndirectWorkCpu`
    // carries a real count and is the control.
    Ogre::HlmsComputeJob *workJob = hc->findComputeJobNoThrow("Jahshaka/IndirectWork");
    Ogre::HlmsComputeJob *cpuJob = hc->findComputeJobNoThrow("Jahshaka/IndirectWorkCpu");
    if (!countJob || !workJob || !cpuJob) {
        mLastError = "engine: the indirect-dispatch compute jobs are missing — "
                     "media/Hlms/Jahshaka/JahshakaCompute.material.json is not staged";
        return false;
    }

    Ogre::UavBufferPacked *srcBuf = 0;
    Ogre::UavBufferPacked *argBuf = 0;
    Ogre::UavBufferPacked *outIndirect = 0;
    Ogre::UavBufferPacked *outCpu = 0;
    Ogre::UavBufferPacked *outNoBarrier = 0;

    JAH_TRY {
        // The input list: `survivors` non-zero entries, the rest zero.
        std::vector<Ogre::uint32> src(kListLength, 0u);
        for (unsigned i = 0; i < survivors; ++i) src[i] = 1u;
        // The dispatch arguments: x counted by the GPU, y and z seeded to 1 so a
        // count of N means N groups and not N*0*0; [3] is the count again, for
        // the readback (reading the argument itself would not prove the GPU used it).
        Ogre::uint32 args[4] = { 0u, 1u, 1u, 0u };
        std::vector<Ogre::uint32> zeros(kListLength, 0u);

        srcBuf = vao->createUavBuffer(kListLength, sizeof(Ogre::uint32), 0, src.data(), false);
        argBuf = vao->createUavBuffer(4u, sizeof(Ogre::uint32), 0, args, false);
        outIndirect = vao->createUavBuffer(kListLength, sizeof(Ogre::uint32), 0, zeros.data(), false);
        outCpu = vao->createUavBuffer(kListLength, sizeof(Ogre::uint32), 0, zeros.data(), false);
        outNoBarrier = vao->createUavBuffer(kListLength, sizeof(Ogre::uint32), 0, zeros.data(), false);

        Ogre::BarrierSolver &solver = rs->getBarrierSolver();

        // ---- job A: count the survivors, write the group count ----------------
        countJob->_setUavBuffer(0, bufferSlot(srcBuf, Ogre::ResourceAccess::Read));
        countJob->_setUavBuffer(1, bufferSlot(argBuf, Ogre::ResourceAccess::ReadWrite));
        countJob->setNumThreadGroups(kListLength / kCountThreadsPerGroup, 1u, 1u);
        {
            Ogre::ResourceTransitionArray &rt = solver.getNewResourceTransitionsArrayTmp();
            countJob->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
        }
        hc->dispatch(countJob, 0, 0);

        // ---- job B: one group per survivor, dispatched FROM THE GPU ------------
        workJob->_setUavBuffer(0, bufferSlot(outIndirect, Ogre::ResourceAccess::Write));
        workJob->setIndirectDispatchBuffer(argBuf, 0u);
        {
            Ogre::ResourceTransitionArray &rt = solver.getNewResourceTransitionsArrayTmp();
            workJob->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
        }
        hc->dispatch(workJob, 0, 0);

        std::vector<Ogre::uint32> stamps;
        readBack(outIndirect, stamps);
        tallyStamps(stamps, out.groupsRan, out.groupsSeen);

        std::vector<Ogre::uint32> argsBack;
        readBack(argBuf, argsBack);
        out.groupsRequested = argsBack.size() > 3u ? argsBack[3] : 0u;

        // ---- the control: the SAME SHADER, sized from the CPU ------------------
        // WITH ONE ASYMMETRY WORTH RECORDING: a CPU-sized dispatch cannot express
        // "run nothing". Ogre refuses to compile a job whose num_thread_groups
        // multiply to zero (HlmsCompute::compileShader), so the CPU-side
        // equivalent of an empty list is the HOST BRANCHING and not dispatching
        // at all — which is exactly the branch a GPU-driven pipeline does not
        // get to take, because the CPU does not know the list is empty. The
        // control below therefore skips the dispatch at zero and compares
        // against the untouched (zeroed) buffer, which is the honest comparison.
        cpuJob->_setUavBuffer(0, bufferSlot(outCpu, Ogre::ResourceAccess::Write));
        if (out.groupsRequested > 0u) {
            cpuJob->setNumThreadGroups(out.groupsRequested, 1u, 1u);
            Ogre::ResourceTransitionArray &rt = solver.getNewResourceTransitionsArrayTmp();
            cpuJob->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
            hc->dispatch(cpuJob, 0, 0);
        }

        std::vector<Ogre::uint32> cpuStamps;
        readBack(outCpu, cpuStamps);
        unsigned cpuSeen = 0u;
        tallyStamps(cpuStamps, out.groupsRanCpuSized, cpuSeen);
        out.matchesCpuSized = (cpuStamps == stamps);

        // ---- the control for the BARRIER --------------------------------------
        // Re-count into a freshly zeroed argument buffer and dispatch off it with
        // the barrier suppressed, back to back, so the hazard is as tight as it
        // can be made. Whatever comes back is reported, never asserted: a driver
        // that happens to win the race proves nothing about the next one.
        {
            Ogre::uint32 reset[4] = { 0u, 1u, 1u, 0u };
            argBuf->upload(reset, 0, 4u);

            Ogre::ResourceTransitionArray &rt = solver.getNewResourceTransitionsArrayTmp();
            countJob->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
            hc->dispatch(countJob, 0, 0);

            workJob->_setUavBuffer(0, bufferSlot(outNoBarrier, Ogre::ResourceAccess::Write));
            workJob->setIndirectDispatchBuffer(argBuf, 0u, /*issueBarrier*/ false);
            Ogre::ResourceTransitionArray &rt2 = solver.getNewResourceTransitionsArrayTmp();
            workJob->analyzeBarriers(rt2);
            rs->executeResourceTransition(rt2);
            hc->dispatch(workJob, 0, 0);

            std::vector<Ogre::uint32> nbStamps;
            readBack(outNoBarrier, nbStamps);
            unsigned nbSeen = 0u;
            tallyStamps(nbStamps, out.groupsRanNoBarrier, nbSeen);
            out.noBarrierDiffered = (nbStamps != stamps);
        }

        unbindProbeJobs(countJob, workJob, cpuJob);
        destroyProbeBuffers(vao, srcBuf, argBuf, outIndirect, outCpu, outNoBarrier);
        return true;
    }
    catch (Ogre::Exception &e) {
        // THE SAME UNBIND ON THE FAILURE PATH. The jobs outlive this call (they
        // live in HlmsCompute) and their descriptor sets hold RAW pointers, so a
        // throw between binding and freeing would otherwise leave three UAV slots
        // and an indirect-dispatch buffer pointing at freed memory — and the next
        // caller of this probe, or of those jobs, would dereference them.
        mLastError = e.getFullDescription();
        unbindProbeJobs(countJob, workJob, cpuJob);
        destroyProbeBuffers(vao, srcBuf, argBuf, outIndirect, outCpu, outNoBarrier);
        return false;
    }
}


// ===========================================================================
// THE HIERARCHICAL DEPTH PYRAMID — readback and shape (NANITE_SPEC §4.3)
//
// The pyramid itself is built by the compositor (OgreChain.cpp): one R32_FLOAT
// texture with a full mip chain, written by one compute pass per level right
// after the opaque pass. These two verbs exist so a suite can ASSERT the
// reduction rather than trust it, and so a future consumer can ask whether
// there is anything to bind.
// ===========================================================================
namespace {

/// The live `jahHzb` texture of a view's workspace, or null. Walks the node
/// sequence because a chain is several nodes and only one of them declares it.
Ogre::TextureGpu *findHzbTexture(OgreView *view) {
    if (!view) return nullptr;
    Ogre::CompositorWorkspace *ws = view->workspace();
    if (!ws) return nullptr;
    const Ogre::CompositorNodeVec &nodes = ws->getNodeSequence();
    for (Ogre::CompositorNode *node : nodes) {
        if (!node) continue;
        // getDefinedTexture THROWS on a node that does not declare it, and only
        // one node of the chain does — so the miss is caught, not tested for.
        try {
            if (Ogre::TextureGpu *tex = node->getDefinedTexture(Ogre::IdString("jahHzb")))
                return tex;
        } catch (const Ogre::Exception &) {}
    }
    return nullptr;
}

}  // namespace

bool OgreEngine::hzbStatus(View *view, HzbStatus &out) const {
    out = HzbStatus();
    if (!mRoot) return false;
    JAH_TRY {
        Ogre::TextureGpu *tex = findHzbTexture(static_cast<OgreView *>(view));
        if (!tex) return false;
        out.built = true;
        out.levels = tex->getNumMipmaps();
        out.width = tex->getWidth();
        out.height = tex->getHeight();
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        out.reverseDepth = rs && rs->isReverseDepth();
        return true;
    }
    catch (...) { out = HzbStatus(); return false; }
}

bool OgreEngine::readHzbLevel(View *view, unsigned level, std::vector<float> &out,
                              unsigned &width, unsigned &height) {
    out.clear();
    width = height = 0u;
    if (!mRoot) return false;
    JAH_TRY {
        Ogre::TextureGpu *tex = findHzbTexture(static_cast<OgreView *>(view));
        if (!tex) { mLastError = "readHzbLevel: this view builds no HZB"; return false; }
        if (level >= tex->getNumMipmaps()) {
            mLastError = "readHzbLevel: level out of range";
            return false;
        }
        const Ogre::uint32 w = std::max(tex->getWidth() >> level, 1u);
        const Ogre::uint32 h = std::max(tex->getHeight() >> level, 1u);

        // THE FLUSH IS LOAD-BEARING (CLAUDE.md, sky/IBL adoption facts): an
        // AsyncTextureTicket reads whatever is in VRAM, and the pyramid's
        // dispatches are only RECORDED into the open command buffer until
        // something submits it. Without this the readback can return a freed
        // allocation's pixels — not zeros, somebody else's picture.
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        rs->flushCommands();

        Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
        Ogre::AsyncTextureTicket *ticket = tm->createAsyncTextureTicket(
            w, h, 1u, Ogre::TextureTypes::Type2D, tex->getPixelFormat());
        ticket->download(tex, Ogre::uint8(level), true);
        const Ogre::TextureBox box = ticket->map(0);
        out.resize(size_t(w) * size_t(h));
        for (Ogre::uint32 y = 0; y < h; ++y)
            std::memcpy(&out[size_t(y) * w], box.at(0, y, 0), size_t(w) * sizeof(float));
        ticket->unmap();
        tm->destroyAsyncTextureTicket(ticket);
        width = w; height = h;
        return true;
    }
    catch (Ogre::Exception &e) { mLastError = e.getFullDescription(); return false; }
}

}  // namespace detail
}}  // namespace jahshaka::engine
