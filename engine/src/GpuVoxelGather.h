// ATOM P4b — THE VOXELISER'S INSTANCE FEED, ON THE DEVICE
// (SPECS/atom/A5b_VOXELISER_FEED_AND_FAR_BLAS_DESIGN.md §2-§3).
//
// WHAT IT REPLACES. Every voxeliser rebuild used to walk the scene's items on the
// CPU: addItem per GI item, a VctMaterial conversion and a bucket per sub-item, an
// octant test per partition, a 96-byte record per survivor written into a CPU
// array and uploaded. None of it is here. Three compute jobs read the GPU scene's
// tables and write the records the voxelise shader reads, and the CPU's share of a
// rebuild is a 304-byte request and three dispatches:
//
//   1. Jahshaka/VoxelGatherCount — ONE THREAD PER INSTANCE SLOT: the predicates
//      (GI-visible, a mesh, a converted material, rule 2's sub-voxel extent, the
//      budget), the LEVEL (the cascade's case of the quality currency, walked over
//      the level table's measured bounds), and per partition of that level its
//      world AABB against each octant — an atomic count per (octant, bucket).
//   2. Jahshaka/VoxelGatherScan  — ONE THREAD: the counts become (start, count)
//      ranges. Tiny (octants x material pools) and serial, which is also what
//      makes it deterministic.
//   3. Jahshaka/VoxelGatherWrite — the same decisions as job 1, taking a place in
//      its range per record, writing it, and adding what it wrote to the READOUT.
//
// WHY COUNT-SCAN-WRITE AND NOT FIXED-CAPACITY PARTITIONS. A fixed capacity per
// (octant, bucket) must hold the worst case in EVERY range, which multiplies the
// record buffer by the pool count (8 pools on the lattice: 8x the memory, nearly
// all of it empty). Counting first lets every range be exactly as long as it is,
// and the buffer is sized by the one bound that is honest — every instance at its
// finest level's partition count (GpuScene::recordBound) — for two extra, tiny
// dispatches.
//
// THE READOUT (A5b §3). What a rebuild voxelised is counted on the device, as it is
// written, into a 32-word block, and read back through an AsyncTicket that is
// REQUESTED AT THE NEXT FRAME'S START (never inside a rebuild: see markReadoutOwed)
// and harvested when its transfer is done — never waited on, except by giStatus.
// It is the truth the CPU prediction it replaces could only approximate.
//
// NO VULKAN HERE (the GPU scene's rule): the buffers are Ogre `UavBufferPacked`s,
// bound to `HlmsComputeJob`s.
#ifndef JAHSHAKA_ENGINE_GPUVOXELGATHER_H
#define JAHSHAKA_ENGINE_GPUVOXELGATHER_H

#include <OgrePrerequisites.h>
#include <Vao/OgreAsyncTicket.h>

#include <cstdint>
#include <string>
#include <vector>

namespace Ogre {
class UavBufferPacked;
class VaoManager;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

/// THE REQUEST, as the shaders read it: 304 bytes, std430, every lane vec4-sized.
struct VoxelGatherParams {
    uint32_t counts[4] = {};   ///< x instance slots, y octants, z material buckets, w flags
    uint32_t caps[4] = {};     ///< x record capacity, y range count, z, w unused
    float    lod[4] = {};      ///< x cell (m), y tolerance (samples), z rule 2's minimum extent, w unused
    float    octMin[8][4] = {};///< the octants' world boxes (the engine divides 1x1x1)
    float    octMax[8][4] = {};
};
static_assert(sizeof(VoxelGatherParams) == 304, "the gather request's layout is a shader contract");

enum VoxelGatherFlag : uint32_t {
    kGatherLod = 1u << 0,       ///< walk the level rule (GiParams::cascadeVoxelLod met); off = level 0
    kGatherBudgeted = 1u << 1,  ///< an instance budget is on: the mask decides membership
};

/// THE READOUT BLOCK, word offsets. Everything is counted where it is WRITTEN, so
/// it is a reading and not a prediction.
enum VoxelReadoutWord : uint32_t {
    kReadIndexTotal = 0,     ///< indices of every CANDIDATE partition (the attach set, Types.h)
    kReadHistogram = 1,      ///< [1..16]: candidate partitions per LOD level
    kReadInstances = 17,     ///< instances with at least one record written (the enclosed set)
    kReadCandidates = 18,    ///< instances that passed every predicate (the attach set)
    kReadRecords = 19,       ///< records written, all octants
    kReadOverflow = 20,      ///< records DROPPED because the capacity was exceeded (must be 0)
    kReadWords = 32,
};
static constexpr uint32_t kReadHistogramLevels = 16u;

/// What the last HARVESTED readout said. `valid` is false until one has arrived.
struct VoxelReading {
    bool valid = false;
    unsigned long long indexTotal = 0;
    std::vector<int> histogram;       ///< trailing zeros trimmed
    unsigned instances = 0;
    unsigned candidates = 0;
    unsigned records = 0;
    unsigned overflow = 0;
};

/// ONE ARM'S FEED: what one gather writes and one voxeliser reads. A chain owns one
/// per cascade — so a cascade's rebuild can never overwrite records another
/// cascade's recorded dispatches are about to read — and the single volume owns one.
class VoxelFeed {
public:
    ~VoxelFeed();
    /// Creates or GROWS (by doubling; never shrinks) the buffers. A grow destroys
    /// the old ones, which is why the voxeliser is re-bound before every build.
    bool ensure(Ogre::VaoManager *vao, uint64_t recordCapacity, uint32_t numRanges,
                uint32_t maskWords, std::string &err);
    void destroy();
    bool live() const { return mParams != nullptr; }

    Ogre::UavBufferPacked *params() const { return mParams; }
    Ogre::UavBufferPacked *work() const { return mWork; }       ///< counts[ranges] then cursors[ranges]
    Ogre::UavBufferPacked *ranges() const { return mRanges; }   ///< uvec2 (start, count) per range
    Ogre::UavBufferPacked *records() const { return mRecords; }
    Ogre::UavBufferPacked *readout() const { return mReadout; }
    Ogre::UavBufferPacked *mask() const { return mMask; }       ///< one bit per instance slot
    uint64_t recordCapacity() const { return mRecordCapacity; }
    uint32_t rangeCapacity() const { return mRangeCapacity; }

    /// The gather wrote a readout nobody has asked the device for yet. NOT requested
    /// at the gather: Ogre's readRequest COMMITS the command buffer, and a commit in
    /// the middle of a rebuild left a GPU bubble inside it (measured on Showroom 2:
    /// +2-4 ms per cascade rebuild on the monitor's GPU pair, +0.3-0.5 ms CPU). It is
    /// requested at the start of the next frame's GI work (`serviceReadout`), where
    /// the command buffer holds nothing yet, or on demand by giStatus.
    void markReadoutOwed() { mReadoutOwed = true; }
    bool readoutOwed() const { return mReadoutOwed; }
    /// Frame start: collect a finished readout; issue an owed one.
    void serviceReadout();
    /// Queues the readout's download (after the gather's dispatches, in the same
    /// command stream) — NON-blocking, but it commits the command buffer.
    void requestReadout();
    /// Takes the queued readout if its transfer is done; returns whether it did.
    /// Never waits: a caller that asks too early keeps the previous reading.
    bool harvest();
    /// Waits for the queued readout (a TEST AND TOOL path only — a stall).
    bool harvestBlocking();
    /// A build that gathered NOTHING (no geometry, no material, the refusal hook):
    /// the reading is an honest zero now, not the last gather's numbers.
    void noteEmpty();
    const VoxelReading &reading() const { return mReading; }

private:
    void takeTicket();
    Ogre::VaoManager *mVao = nullptr;
    Ogre::UavBufferPacked *mParams = nullptr;
    Ogre::UavBufferPacked *mWork = nullptr;
    Ogre::UavBufferPacked *mRanges = nullptr;
    Ogre::UavBufferPacked *mRecords = nullptr;
    Ogre::UavBufferPacked *mReadout = nullptr;
    Ogre::UavBufferPacked *mMask = nullptr;
    uint64_t mRecordCapacity = 0;
    uint32_t mRangeCapacity = 0;
    uint32_t mMaskCapacity = 0;
    Ogre::AsyncTicketPtr mTicket;
    bool mReadoutOwed = false;
    VoxelReading mReading;
};

/// Threads per group of the count and write jobs (JahshakaCompute.material.json).
static constexpr uint32_t kGatherThreadsPerGroup = 64u;

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_GPUVOXELGATHER_H
