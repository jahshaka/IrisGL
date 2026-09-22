// ATOM P4b — THE VOXELISER'S INSTANCE FEED, ON THE DEVICE. See GpuVoxelGather.h
// for what it replaces and why it is count-scan-write.
#include "EnginePrivate.h"
#include "GpuScene.h"
#include "GpuVoxelGather.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreLogManager.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <Vct/OgreVctMaterial.h>
#include <Vct/OgreVctVoxelizer.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {

VoxelFeed::~VoxelFeed() { destroy(); }

void VoxelFeed::destroy() {
    // A ticket outliving its buffer would map freed memory: drop it first.
    mTicket.reset();
    if (mVao) {
        if (mParams) mVao->destroyUavBuffer(mParams);
        if (mWork) mVao->destroyUavBuffer(mWork);
        if (mRanges) mVao->destroyUavBuffer(mRanges);
        if (mRecords) mVao->destroyUavBuffer(mRecords);
        if (mReadout) mVao->destroyUavBuffer(mReadout);
        if (mMask) mVao->destroyUavBuffer(mMask);
    }
    mParams = mWork = mRanges = mRecords = mReadout = mMask = nullptr;
    mVao = nullptr;
    mRecordCapacity = 0;
    mRangeCapacity = 0;
    mMaskCapacity = 0;
}

bool VoxelFeed::ensure(Ogre::VaoManager *vao, uint64_t recordCapacity, uint32_t numRanges,
                       uint32_t maskWords, std::string &err) {
    if (!vao) {
        err = "voxel gather: no VaoManager (the NULL render system) - the feed is unsupported";
        return false;
    }
    if (vao != mVao) destroy();
    mVao = vao;
    std::vector<uint32_t> zeros;
    if (!mParams) {
        VoxelGatherParams p;
        mParams = vao->createUavBuffer(1u, sizeof(VoxelGatherParams), 0, &p, false);
        zeros.assign(kReadWords, 0u);
        mReadout = vao->createUavBuffer(kReadWords, sizeof(uint32_t), 0, zeros.data(), false);
    }
    // GROWN BY DOUBLING, and only here: a rebuild that needs no more room allocates
    // nothing, so a scrolling chain costs no Vulkan allocation per rebuild.
    if (numRanges > mRangeCapacity) {
        uint32_t cap = std::max(mRangeCapacity, 16u);
        while (cap < numRanges) cap *= 2u;
        if (mWork) vao->destroyUavBuffer(mWork);
        if (mRanges) vao->destroyUavBuffer(mRanges);
        zeros.assign(size_t(cap) * 2u, 0u);
        mWork = vao->createUavBuffer(size_t(cap) * 2u, sizeof(uint32_t), 0, zeros.data(), false);
        mRanges = vao->createUavBuffer(cap, 2u * sizeof(uint32_t), 0, zeros.data(), false);
        mRangeCapacity = cap;
    }
    if (recordCapacity > mRecordCapacity) {
        uint64_t cap = std::max<uint64_t>(mRecordCapacity, 256u);
        while (cap < recordCapacity) cap *= 2u;
        if (mRecords) vao->destroyUavBuffer(mRecords);
        // UAV (the gather writes it) AND READONLY (the voxeliser reads it through a
        // read-only view, exactly as it read the buffer it used to fill itself).
        mRecords = vao->createUavBuffer(size_t(cap), Ogre::VctVoxelizer::kInstanceRecordBytes,
                                        Ogre::BB_FLAG_UAV | Ogre::BB_FLAG_READONLY, nullptr, false);
        mRecordCapacity = cap;
    }
    const uint32_t wantMask = std::max(maskWords, 1u);
    if (wantMask > mMaskCapacity) {
        uint32_t cap = std::max(mMaskCapacity, 64u);
        while (cap < wantMask) cap *= 2u;
        if (mMask) vao->destroyUavBuffer(mMask);
        zeros.assign(cap, 0u);
        mMask = vao->createUavBuffer(cap, sizeof(uint32_t), 0, zeros.data(), false);
        mMaskCapacity = cap;
    }
    return mParams && mWork && mRanges && mRecords && mReadout && mMask;
}

void VoxelFeed::serviceReadout() {
    harvest();
    if (mReadoutOwed && !mTicket) requestReadout();
}

void VoxelFeed::requestReadout() {
    mReadoutOwed = false;
    if (!mReadout) return;
    // THE ORDER IS THE COMMAND STREAM'S: the copy is recorded after the write job, and
    // Ogre's download path charges a UAV source with SHADER_WRITE from every stage
    // before copying (VulkanQueue::prepareForDownload), so the transfer sees what the
    // job wrote. Nothing is flushed and nothing waits: the ticket is collected once
    // its transfer is done (harvest), which is a frame later in practice.
    mTicket = mReadout->readRequest(0, kReadWords);
}

void VoxelFeed::takeTicket() {
    const uint32_t *w = static_cast<const uint32_t *>(mTicket->map());
    VoxelReading r;
    r.valid = true;
    r.indexTotal = w[kReadIndexTotal];
    r.histogram.assign(w + kReadHistogram, w + kReadHistogram + kReadHistogramLevels);
    while (!r.histogram.empty() && r.histogram.back() == 0) r.histogram.pop_back();
    r.instances = w[kReadInstances];
    r.candidates = w[kReadCandidates];
    r.records = w[kReadRecords];
    r.overflow = w[kReadOverflow];
    mTicket->unmap();
    mTicket.reset();
    mReading = r;
    if (r.overflow) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka GI: the voxel gather DROPPED " + std::to_string(r.overflow) +
            " records for want of capacity - the record bound is wrong and the volume is "
            "missing geometry",
            Ogre::LML_CRITICAL);
    }
}

bool VoxelFeed::harvest() {
    if (!mTicket || !mTicket->queryIsTransferDone()) return false;
    takeTicket();
    return true;
}

void VoxelFeed::noteEmpty() {
    mTicket.reset();
    mReadoutOwed = false;
    mReading = VoxelReading();
    mReading.valid = true;
}

bool VoxelFeed::harvestBlocking() {
    if (!mTicket) return false;
    takeTicket();     // map() waits on the ticket's own fence
    return true;
}

Ogre::DescriptorSetUav::BufferSlot gatherSlotPublic(Ogre::UavBufferPacked *buffer,
                                              Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = buffer;
    slot.offset = 0;
    slot.sizeBytes = 0;
    slot.access = access;
    return slot;
}

static void dispatchGatherImpl(Ogre::RenderSystem *rs, Ogre::HlmsCompute *hc, Ogre::HlmsComputeJob *job) {
    Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
    job->analyzeBarriers(rt);
    rs->executeResourceTransition(rt);
    hc->dispatch(job, 0, 0);
}

/// THE JOBS OUTLIVE A GATHER (they live in HlmsCompute) and their descriptor sets
/// hold RAW pointers into buffers a later grow destroys: nothing stays bound.
static void unbindGather(Ogre::HlmsComputeJob *count, Ogre::HlmsComputeJob *scan,
                  Ogre::HlmsComputeJob *write) {
    if (count) count->clearUavBuffers();
    if (scan) scan->clearUavBuffers();
    if (write) write->clearUavBuffers();
}

// ---------------------------------------------------------------------------
// (OgreScene lives in `detail` - EnginePrivate.h.)
bool OgreScene::runVoxelGather(detail::VoxelFeed &feed, Ogre::VctVoxelizer *voxelizer,
                               const VoxelGatherInputs &in) {
    using namespace detail;
    if (!voxelizer) return false;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    Ogre::HlmsCompute *hc =
        mRoot && mRoot->getHlmsManager() ? mRoot->getHlmsManager()->getComputeHlms() : nullptr;
    if (!rs || !hc || !mGpuScene.live()) {
        voxelizer->setInstanceSource(nullptr, nullptr);
        return false;
    }
    Ogre::HlmsComputeJob *count = hc->findComputeJobNoThrow("Jahshaka/VoxelGatherCount");
    Ogre::HlmsComputeJob *scan = hc->findComputeJobNoThrow("Jahshaka/VoxelGatherScan");
    Ogre::HlmsComputeJob *write = hc->findComputeJobNoThrow("Jahshaka/VoxelGatherWrite");
    if (!count || !scan || !write) {
        mError = "engine: the voxel gather jobs are missing - "
                 "media/Hlms/Jahshaka/JahshakaCompute.material.json is not staged";
        voxelizer->setInstanceSource(nullptr, nullptr);
        return false;
    }

    // The previous gather's readout, if its transfer finished: the new request below
    // replaces the ticket, and a reading nobody took would be lost.
    feed.harvest();

    Ogre::VctMaterial *store = mVctMaterialStore;
    const uint32_t numBuckets = store ? uint32_t(store->getNumBuckets()) : 0u;
    const uint32_t numOctants = uint32_t(std::min<size_t>(voxelizer->getNumOctants(), 8u));
    Ogre::UavBufferPacked *partAabbs = mGpuScene.partitionAabbBuffer();
    if (!numBuckets || !numOctants || !partAabbs || !mGpuScene.partitionCount()) {
        // NOTHING TO GATHER: no material converted, or no geometry. The voxeliser
        // clears its volumes, which is the honest empty picture.
        voxelizer->setInstanceSource(nullptr, nullptr);
        feed.noteEmpty();
        return true;
    }

    const uint32_t slots = mGpuScene.slotCount();
    const uint32_t numRanges = numOctants * numBuckets;
    Ogre::VaoManager *vao = rs->getVaoManager();
    // THE CAPACITY IS A BOUND, NOT A GUESS: every instance at its finest level's
    // partition count, in every octant it could reach. The read-only view has a
    // device ceiling; past it the records are DROPPED AND COUNTED (the readout's
    // overflow word), never written out of bounds.
    uint64_t capacity = std::max<uint64_t>(mGpuScene.recordBound() * numOctants, 64u);
    const uint64_t viewMax = vao->getReadOnlyBufferMaxSize() / Ogre::VctVoxelizer::kInstanceRecordBytes;
    if (viewMax && capacity > viewMax) capacity = viewMax;
    std::string err;
    if (!feed.ensure(vao, capacity, numRanges, (mGpuScene.slotCapacity() + 31u) / 32u, err)) {
        mError = err;
        voxelizer->setInstanceSource(nullptr, nullptr);
        return false;
    }

    // ---- the request -------------------------------------------------------
    VoxelGatherParams p;
    p.counts[0] = slots;
    p.counts[1] = numOctants;
    p.counts[2] = numBuckets;
    p.counts[3] = (in.lod ? kGatherLod : 0u) | (in.budgetMask ? kGatherBudgeted : 0u);
    p.caps[0] = uint32_t(std::min<uint64_t>(feed.recordCapacity(), capacity));
    p.caps[1] = numRanges;
    p.caps[2] = Ogre::VctVoxelizer::kIndicesPerPartition;
    p.lod[0] = in.cell;
    p.lod[1] = in.tolerance;
    p.lod[2] = in.minExtent;
    for (uint32_t o = 0; o < numOctants; ++o) {
        const Ogre::Aabb &box = voxelizer->getOctantRegion(o);
        const Ogre::Vector3 mn = box.getMinimum(), mx = box.getMaximum();
        for (int i = 0; i < 3; ++i) {
            p.octMin[o][i] = mn[i];
            p.octMax[o][i] = mx[i];
        }
    }
    feed.params()->upload(&p, 0, 1u);
    {
        // THE COUNTERS START AT ZERO: counts and cursors in `work`, and the readout.
        std::vector<uint32_t> zeros(size_t(numRanges) * 2u, 0u);
        feed.work()->upload(zeros.data(), 0, zeros.size());
        zeros.assign(kReadWords, 0u);
        feed.readout()->upload(zeros.data(), 0, kReadWords);
    }
    if (in.budgetMask && !in.budgetMask->empty())
        feed.mask()->upload(in.budgetMask->data(), 0,
                            std::min<size_t>(in.budgetMask->size(), feed.mask()->getNumElements()));

    const uint32_t groups = std::max((slots + kGatherThreadsPerGroup - 1u) / kGatherThreadsPerGroup, 1u);
    JAH_TRY {
        // ---- job 1: count ----------------------------------------------------
        for (Ogre::HlmsComputeJob *job : { count, write }) {
            job->_setUavBuffer(0u, gatherSlotPublic(feed.params(), Ogre::ResourceAccess::Read));
            job->_setUavBuffer(1u, gatherSlotPublic(mGpuScene.instanceBuffer(), Ogre::ResourceAccess::Read));
            job->_setUavBuffer(2u, gatherSlotPublic(mGpuScene.meshBuffer(), Ogre::ResourceAccess::Read));
            job->_setUavBuffer(3u, gatherSlotPublic(mGpuScene.levelBuffer(), Ogre::ResourceAccess::Read));
            job->_setUavBuffer(4u, gatherSlotPublic(partAabbs, Ogre::ResourceAccess::Read));
            job->_setUavBuffer(5u, gatherSlotPublic(feed.work(), Ogre::ResourceAccess::ReadWrite));
            job->_setUavBuffer(6u, gatherSlotPublic(feed.ranges(), Ogre::ResourceAccess::Read));
            job->_setUavBuffer(7u, gatherSlotPublic(feed.records(), Ogre::ResourceAccess::Write));
            job->_setUavBuffer(8u, gatherSlotPublic(feed.readout(), Ogre::ResourceAccess::ReadWrite));
            job->_setUavBuffer(9u, gatherSlotPublic(feed.mask(), Ogre::ResourceAccess::Read));
            job->setNumThreadGroups(groups, 1u, 1u);
        }
        scan->_setUavBuffer(0u, gatherSlotPublic(feed.params(), Ogre::ResourceAccess::Read));
        scan->_setUavBuffer(1u, gatherSlotPublic(feed.work(), Ogre::ResourceAccess::ReadWrite));
        scan->_setUavBuffer(2u, gatherSlotPublic(feed.ranges(), Ogre::ResourceAccess::Write));
        scan->setNumThreadGroups(1u, 1u, 1u);

        dispatchGatherImpl(rs, hc, count);
        dispatchGatherImpl(rs, hc, scan);
        dispatchGatherImpl(rs, hc, write);
        feed.markReadoutOwed();
        unbindGather(count, scan, write);
    }
    catch (Ogre::Exception &e) {
        mError = e.getFullDescription();
        unbindGather(count, scan, write);
        voxelizer->setInstanceSource(nullptr, nullptr);
        return false;
    }
    voxelizer->setInstanceSource(feed.records(), feed.ranges());
    return true;
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
