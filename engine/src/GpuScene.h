// THE GPU SCENE — a description of what is where that LIVES on the device
// (ENGINE V2's V2-2, pulled forward into phase A of the Atom and Photon builds;
// SPECS/atom/A3_GPU_SCENE_SLICE_DESIGN.md, contract §3's first row).
//
// WHY IT EXISTS. Three separate CPU walks over the scene's items used to
// re-derive the same facts every frame a thing moved: the ray tier's instance
// array (3.4-4.0 ms at 8,001 instances in Debug), the GI orchestrator's
// per-item box signatures, and the voxeliser's per-instance feed. Each of them
// wants "world transform, previous world transform, bounds, which mesh, which
// predicates hold" — so those facts are written ONCE, into two resident device
// buffers, for the slots that CHANGED, and every consumer reads the table.
//
// THE INTERFACE IS DELIBERATELY ONE FUNCTION: `update(dirtySlots, epoch)`.
// Today's caller finds the dirty slots with a per-item compare (OgreScene::
// updateGpuScene); ENGINE V2's journal (V2-1) will hand the same function the
// changed subset it already knows, with NO interface change here. That is the
// whole reason the write path takes a list of slots rather than "scan the
// scene".
//
// THE INDEX IS THE ITEM SLOT (`Node::itemSlot`, its place in
// OgreScene::mItemNodes). It is already the TLAS's `instanceCustomIndex` and
// already the surface cache's card-rect index, so a hit shader that knows the
// slot can read this table with no second lookup. The index is DENSE: a removal
// swaps the last item into the freed slot, and `onSlotMoved` is the ONE place
// that event is handled for the table.
//
// WHAT IS NOT HERE. No compute job advances the table: the CPU mirror below is
// authoritative and the previous world is written from it (see `stage`), which
// is exact and costs no dispatch and no barrier. No Vulkan: the tables are
// Ogre `UavBufferPacked`s so an `HlmsComputeJob` (Atom P3's cull) and a raw
// descriptor set (the ray tier's instance job) can both bind them, and so the
// facility exists on a platform with no ray queries at all.
#ifndef JAHSHAKA_ENGINE_GPUSCENE_H
#define JAHSHAKA_ENGINE_GPUSCENE_H

#include <OgreMesh2.h>
#include <OgrePrerequisites.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ogre {
class Mesh;
class UavBufferPacked;
class VaoManager;
}  // namespace Ogre

namespace jahshaka {
namespace engine {
namespace detail {

/// ONE INSTANCE, std430, 160 bytes, indexed by the item slot.
///
/// `world` and `prevWorld` are ROW-MAJOR 3x4 — the top three rows of Ogre's own
/// `Matrix4`, which is also exactly `VkTransformMatrixKHR`'s layout, so one
/// memcpy serves the table AND the TLAS instance the ray job writes.
///
/// THE TWO PACKED INTEGERS. `boundsMin[3]` carries the MESH TABLE INDEX and
/// `boundsMax[3]` the FLAGS WORD, both bit-cast into the float lane (a shader
/// reads them with `floatBitsToUint`). They live there because the design fixes
/// this layout as a contract for Atom P3's cull and Photon's jobs: a vec4 fetch
/// gives a consumer the bounds and the thing it needs to decide with.
struct GpuInstance {
    float    world[12] = {};
    float    prevWorld[12] = {};
    float    boundsMin[4] = {};  ///< xyz world AABB min; [3] = mesh table index (bit-cast uint)
    float    boundsMax[4] = {};  ///< xyz world AABB max; [3] = the flags word (bit-cast uint)
    uint32_t ids[4] = {};        ///< x = the engine NodeId, y = material bucket (P6), z = light mask, w = 0
    uint32_t pad[4] = {};
};
static_assert(sizeof(GpuInstance) == 160, "the GPU instance table's stride is a contract");

/// ONE MESH, std430, 64 bytes, indexed by the mesh table index.
///
/// The two device ADDRESSES are filled by whoever can take them — today the ray
/// tier, when it builds a mesh's bottom-level structure (`GpuScene::
/// setMeshAddresses`). They are zero on a build with no ray-query tier, which is
/// honest: a device address is a Vulkan fact and the only consumers of it are
/// Vulkan jobs.
struct GpuMesh {
    uint32_t positionAddress[2] = {};  ///< the vertex pool's device address + this buffer's start
    uint32_t indexAddress[2] = {};
    uint32_t counts[4] = {};  ///< x vertices, y level-0 indices, z level count, w submesh count
    float    localBoundsMin[4] = {};
    float    localBoundsMax[4] = {};  ///< [3] = the level-0 LOD bound (ATOM-BAKE-1's honest error)
};
static_assert(sizeof(GpuMesh) == 64, "the GPU mesh table's stride is a contract");

/// THE FLAGS WORD — the predicates every consumer used to recompute for itself.
/// ONE place writes them (`OgreScene::gpuFlagsFor`); nobody else asks the
/// question again.
enum GpuInstanceFlag : uint32_t {
    kGpuVisible = 1u << 0,      ///< shown, and in a world-geometry channel (kVisibleBit|kMovableBit)
    kGpuCaster = 1u << 1,       ///< casts a shadow in at least one caster channel
    kGpuMover = 1u << 2,        ///< kMovableBit: the document says it moves
    kGpuGiVisible = 1u << 3,    ///< kGiGeometryBit: it bounces light
    kGpuAlphaTested = 1u << 4,  ///< a sub-item's datablock has an alpha test (no any-hit shader)
    kGpuSkinned = 1u << 5,      ///< it has a skeleton instance (its buffers hold the bind pose)
    kGpuOverlay = 1u << 6,      ///< its render queue is at or above the overlay queues
    kGpuRayTraced = 1u << 7,    ///< the TRACED SET: the conjunction the ray tier used to walk for
    kGpuDragMover = 1u << 8,    ///< MOVER-1: the user has hold of it right now
    kGpuGiExcluded = 1u << 9,   ///< excluded from the GI bounds fit
};

/// The per-(mesh, level) index range Atom P3's selection and P4's voxeliser
/// read: the first index and the index count of that level, of submesh 0.
struct GpuMeshLevel {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
};

class GpuScene {
public:
    /// Levels per mesh entry in the range table. The bake tops out well below
    /// this (six on the deepest shipped chain); a mesh with more is CLAMPED and
    /// says so once in the log rather than writing past its stride.
    static constexpr uint32_t kLevelsPerMesh = 8u;

    ~GpuScene();

    /// Creates the tables. Refuses (and says why) where there is no VaoManager
    /// at all — the NULL render system's headless boot, where nothing on the
    /// device can be read and every consumer keeps its CPU path.
    bool create(Ogre::VaoManager *vao, std::string &err);
    void destroy();
    bool live() const { return mVao != nullptr && mInstanceBuffer != nullptr; }

    // --- the instance table ------------------------------------------------
    /// Grows the table to hold `count` slots (by DOUBLING, never per frame) and
    /// keeps every entry already in it.
    void ensureSlots(uint32_t count);
    uint32_t slotCapacity() const { return mSlotCapacity; }
    uint32_t slotCount() const { return mSlotCount; }
    void setSlotCount(uint32_t n);

    /// Writes one slot's entry into the CPU mirror. `prevWorld` is taken from
    /// the mirror's own current world — so it is EXACTLY the last frame's — and
    /// the caller never supplies it. Born slots have prevWorld = world.
    void stage(uint32_t slot, const GpuInstance &in);

    /// THE ONE WRITE PATH. Copies the staged slots to the device with one
    /// staging map per frame and one copy per CONTIGUOUS RUN of slots, and
    /// closes the previous frame's movers (prevWorld = world for a slot that
    /// moved last frame and not this one, so a mover that stops shows no motion
    /// on the frame after it stopped).
    void update(const std::vector<uint32_t> &dirtySlots, unsigned long long epoch);

    /// Slots that MOVED on the last update and have not been closed yet: the
    /// next update owes them `prevWorld = world` even if nothing moved in it, so
    /// a still frame after a gesture is not free (and the one after it is).
    bool hasOpenMovers() const { return !mMovedLastFrame.empty(); }

    const GpuInstance &entry(uint32_t slot) const { return mMirror[slot]; }
    /// The mirror as a raw pointer. The dirty scan reads it once per item, and
    /// in a Debug build (which is the daily driver) an accessor call per item is
    /// a measurable part of the walk.
    const GpuInstance *mirrorData() const { return mMirror.data(); }
    const std::vector<GpuInstance> &mirror() const { return mMirror; }

    /// A REMOVAL SWAPPED `from` INTO `to`. The ONE place the swap-remove is
    /// handled for the table: the mirror entry moves and `to` is re-copied this
    /// frame (the caller adds it to the dirty list).
    void onSlotMoved(uint32_t from, uint32_t to);

    // --- the mesh table ----------------------------------------------------
    /// Find-or-create this mesh's entry, reference-counted. `levels` holds up to
    /// kLevelsPerMesh ranges. THE ENTRY HOLDS A MeshPtr: an instance that names
    /// a mesh index must be able to reach the mesh, and the hold is what keeps
    /// the geometry alive under a structure built over it. Every hold is dropped
    /// by `destroy()`, which OgreScene::destroy calls while Root is still alive
    /// (a MeshPtr outliving Root throws in VaoManager).
    uint32_t acquireMesh(const Ogre::MeshPtr &mesh, const GpuMesh &desc,
                         const GpuMeshLevel *levels, uint32_t levelCount);
    /// The mesh an entry names, or a null pointer.
    const Ogre::MeshPtr &meshAt(uint32_t index) const;
    /// Drops one reference; the entry is FREED (and the index recycled) with the
    /// last one.
    void releaseMesh(const Ogre::Mesh *mesh);
    /// The index of a mesh already in the table, or npos.
    uint32_t meshIndex(const Ogre::Mesh *mesh) const;
    static constexpr uint32_t kNoMesh = 0xFFFFFFFFu;
    /// How many entries are live, and how many references one holds (the ray
    /// tier's eviction reads the second: a mesh nothing points at any more).
    uint32_t meshEntryCount() const { return uint32_t(mMeshEntries.size()); }
    uint32_t meshRefCount(uint32_t index) const;
    const GpuMesh &meshEntry(uint32_t index) const { return mMeshMirror[index]; }
    /// The device addresses of a mesh's geometry, filled by the one component
    /// that can take them (the ray tier's BLAS description).
    void setMeshAddresses(uint32_t index, uint64_t positionAddress, uint64_t indexAddress);

    // --- what the readers bind --------------------------------------------
    Ogre::UavBufferPacked *instanceBuffer() const { return mInstanceBuffer; }
    Ogre::UavBufferPacked *meshBuffer() const { return mMeshBuffer; }
    Ogre::UavBufferPacked *levelBuffer() const { return mLevelBuffer; }
    /// Bumped whenever a buffer HANDLE changes (a grow). A reader holding a
    /// descriptor set rewrites it when this moves.
    unsigned long long generation() const { return mGeneration; }

    // --- what it cost (the frame monitor's row and the suite's assertions) --
    unsigned long long writes() const { return mWrites; }        ///< slot writes, ever
    unsigned long long copies() const { return mCopies; }        ///< device copy RUNS, ever
    unsigned long long frames() const { return mUpdates; }       ///< update() calls that copied
    unsigned long long grows() const { return mGrows; }
    double lastCopyMs() const { return mLastCopyMs; }
    unsigned lastDirtyCount() const { return mLastDirtyCount; }
    unsigned long long epoch() const { return mEpoch; }

private:
    void growTo(uint32_t capacity);
    void growMeshTable(uint32_t capacity);

    Ogre::VaoManager *mVao = nullptr;
    Ogre::UavBufferPacked *mInstanceBuffer = nullptr;
    Ogre::UavBufferPacked *mMeshBuffer = nullptr;
    Ogre::UavBufferPacked *mLevelBuffer = nullptr;

    /// THE CPU MIRROR — authoritative. The device table is a copy of it, never
    /// the other way round: nothing here ever reads the device buffer back (the
    /// `AsyncTextureTicket`/`flushCommands` trap has no equivalent that is worth
    /// paying inside a frame).
    std::vector<GpuInstance> mMirror;
    std::vector<unsigned char> mBorn;  ///< 0 = this slot has never been staged
    uint32_t mSlotCapacity = 0;
    uint32_t mSlotCount = 0;

    std::vector<GpuMesh> mMeshMirror;
    std::vector<GpuMeshLevel> mLevelMirror;
    struct MeshEntry {
        Ogre::MeshPtr mesh;
        uint32_t refs = 0;
    };
    std::vector<MeshEntry> mMeshEntries;
    std::vector<uint32_t> mFreeMeshSlots;
    std::unordered_map<const Ogre::Mesh *, uint32_t> mMeshIndex;
    uint32_t mMeshCapacity = 0;
    bool mMeshDirty = false;
    bool mLevelDirty = false;

    /// The slots this frame's copy must also close (prevWorld = world).
    std::vector<uint32_t> mMovedLastFrame;
    std::vector<uint32_t> mCopySet;  ///< scratch, kept to avoid a per-frame allocation

    unsigned long long mGeneration = 1ull;
    unsigned long long mWrites = 0ull, mCopies = 0ull, mUpdates = 0ull, mGrows = 0ull;
    unsigned long long mEpoch = 0ull;
    double mLastCopyMs = 0.0;
    unsigned mLastDirtyCount = 0u;
    bool mWarnedLevels = false;
};

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka

#endif  // JAHSHAKA_ENGINE_GPUSCENE_H
