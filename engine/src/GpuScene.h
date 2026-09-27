// THE GPU SCENE — a description of what is where that LIVES on the device
// (ENGINE V2's V2-2, pulled forward into phase A of the Atom and Photon builds;
// SPECS/atom/A3_GPU_SCENE_SLICE_DESIGN.md, contract §3's first row).
//
// WHY IT EXISTS. Three separate CPU walks over the scene's items re-derive the
// same facts every frame a thing moves: the ray tier's instance array (3.4-4.0
// ms at 8,001 instances in Debug), the GI orchestrator's per-item box
// signatures, and the voxeliser's per-instance feed. Each wants "world
// transform, previous world transform, bounds, which mesh, which predicates
// hold" — so those facts are written ONCE, into resident device buffers, for
// the slots that CHANGED, and the consumers read the table. THE RAY TIER IS
// CONVERTED; the GI signature walk and the voxeliser's feed are NOT, and the
// reason is measured (see `ensureGpuScene`'s `graphIsCurrent`): the GI
// signatures are read BEFORE the frame, where the compare costs 2.3 ms at 8,001
// instead of 0.6 ms, so converting that reader today would make the slice a
// smaller win. It waits for ENGINE V2's journal (V2-1).
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
#include <Vao/OgreStagingBuffer.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Ogre {
class HlmsManager;
class Mesh;
class RenderSystem;
class TexBufferPacked;
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
/// THE GLSL SIDE MUST DECLARE THEM AS `vec4 world[3]` (row i in element i), NOT
/// as `mat3x4`: a GLSL matrix in std430 is COLUMN-major, so a shader written
/// with `mat3x4` would read this table transposed — silently, and correctly for
/// a pure translation, which is the worst way to find out. The struct is copied
/// to the device as RAW BYTES; C++ and GLSL agree because every member is a
/// vec4-sized lane and nothing is padded.
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
    /// x = the engine NodeId, y = THE MATERIAL WORD, z = light mask,
    /// w = THE RAY LEVEL (ATOM P3's SUB-ERROR, AT-A8r): the mesh level this
    /// instance's bottom-level acceleration structure should be built from,
    /// re-evaluated only when the instance's distance from the camera changes by
    /// 2x (the hysteresis is what keeps a BLAS refit rare — `OgreScene::
    /// updateRayLevels`, PATCHED in place: patchRayLevel). ITS CONSUMER IS THE RAY
    /// TIER'S NEAR COPY (ATOM-FARBLAS-1; the Jahshaka/TlasWrite job reads it here):
    /// the instance's near BLAS is built from this level, its far copy from the
    /// mesh's coarsest.
    ///
    /// y = THE MATERIAL WORD (ATOM P4b): {pool : 16 | slot : 16} in the chain's ONE
    /// shared `VctMaterial` store — the bucket whose const buffer holds this item's
    /// material and the row inside it. It is a SCENE-WIDE fact only because every
    /// cascade shares that store (a per-cascade store numbered the same datablock
    /// differently in each). 0xFFFFFFFF until the store has converted the item's
    /// datablock; the voxel gather skips such an instance, and the next GI build
    /// converts it and re-composes the slot. Written by ONE place,
    /// `OgreScene::gpuMaterialWordFor`, `gpuFlagsFor`'s sibling.
    uint32_t ids[4] = {};
    /// THE RASTER WORDS (ATOM-S3-PARITY) — what the visibility buffer's decode
    /// (HlmsAtom, Hlms/Atom) needs of an instance beyond the geometry rows:
    ///   x = THE PBS MATERIAL WORD, {pool : 16 | slot : 16} of sub-item 0's datablock
    ///       in HlmsPbs's const-buffer pool (`HlmsAtom::materialWordOf`) — the index
    ///       the decode's LoadMaterial reads `materialArray` with, exactly as the
    ///       rasterised draw of the same item would. 0xFFFFFFFF for an item whose
    ///       datablock is not HlmsPbs's (unlit furniture), which the decode skips.
    ///   y = the byte offset of the float4 VES_TANGENT in the vertex, 0xFFFFFFFF
    ///       when the mesh has none. It rides HERE because the geometry row is
    ///       Ogre's format (VctVoxelizer::GeometryRow) and has no tangent lane at
    ///       this pin; a fork commit giving the row one moves it there.
    ///   z = THE SKIN ROW (PHOTON-SKIN-1, RY-R4): the PER-INSTANCE ROW OVERRIDE of a
    ///       rigged item — the geometry row (level 0, submesh 0) of THIS ITEM's skin
    ///       cache, the second vertex buffer the `Jahshaka/SkinCache` job writes the
    ///       posed vertices into (SkinCache.h). A consumer that reads rows asks this
    ///       lane first: kNoGeomRow means "no override, the mesh's own rows are the
    ///       geometry" (every unrigged item, and a rigged one whose cache does not
    ///       exist yet); anything else names a row block of `GpuScene::kGeomRowsPerMesh`
    ///       rows laid out exactly like a mesh entry's (level l, submesh s at
    ///       `row + l * kSubmeshesPerMesh + s`), whose vertex address is the cache's
    ///       and whose index addresses are the mesh's own levels. A field beside the
    ///       two above, NOT `ids.z` (the light mask) and not the mesh index: the
    ///       mesh index keeps naming the MESH (its bounds, its levels, its BLAS key),
    ///       and only the geometry an item presents changes.
    ///   w = 0.
    /// Written by ONE place, `OgreScene::composeGpuInstance`, beside the ids — at
    /// attach and at a material change (both mark the slot), and when the ray tier
    /// creates or drops a skin cache (`GpuScene::setSkinRow`, which marks it too).
    ///
    /// The mirrors of this struct (JahCullTest_cs, JahVoxelGather_cs) name the lane
    /// `raster` too; neither reads it.
    /// Defaults to "no material, no tangent, no override": a CLEARED slot
    /// (onSlotFreed) names no material the decode could shade with.
    uint32_t raster[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u };
};
static_assert(sizeof(GpuInstance) == 160, "the GPU instance table's stride is a contract");

/// ONE MESH, std430, 64 bytes, indexed by the mesh table index.
///
/// `positionAddress` and `indexAddress` USED TO BE HERE AND ARE DELETED (ATOM
/// P4b). Nothing ever wrote them, and ATOM-VOXEL-1 found out why they could not
/// be written: a LOD level is its OWN `IndexBufferPacked` with its own device
/// address, so one address per MESH cannot name a level's indices — and the
/// consumer that wanted them (the voxeliser's compute shader) needs the layout
/// beside the address anyway. Both now live in the GEOMETRY ROW table below, one
/// row per (mesh, level, submesh). A3 §1.1's "GpuMesh carries the pool's device
/// addresses" is amended by A5b §1.
///
/// `dag` (ATOM-CLUSTER-CUT) names the mesh's CLUSTER DAG in the two cluster tables
/// below: x = its first cluster in `GpuScene::clusterBuffer`, y = its cluster count,
/// z = its first group in `GpuScene::groupBuffer`, w = THE CLUSTER ROW — the geometry
/// row whose index address is the mesh's cluster stream (GpuScene::kClusterRow), so a
/// cluster's triangle t reads its corners at `firstIndex + 3t` through the same row
/// decode every other reader uses. A mesh the bake gave no DAG (a small one, a
/// runtime primitive) gets a FLAT one: level 0 cut into runs of kFlatClusterTriangles,
/// all leaves of one terminal group, over level 0's own index buffer.
struct GpuMesh {
    uint32_t counts[4] = {};  ///< x vertices, y level-0 indices, z level count, w submesh count
    float    localBoundsMin[4] = {};
    float    localBoundsMax[4] = {};  ///< [3] = the level-0 LOD bound (ATOM-BAKE-1's honest error)
    uint32_t dag[4] = { 0u, 0u, 0u, 0xFFFFFFFFu };   ///< cluster base, count, group base, cluster row
    // localBoundsMin[3] carries the DAG's GROUP COUNT, bit-cast (the cut job's per-group
    // mask needs it; written by flushClusterTables, 0 for a mesh with no DAG).
};
static_assert(sizeof(GpuMesh) == 64, "the GPU mesh table's stride is a contract");

/// ONE CLUSTER of a mesh's DAG, std430, 32 bytes — the layout JahClusterCut.glsl's
/// `JahCluster` declares: `firstIndex`/`indexCount` its range of the mesh's cluster
/// stream (the cluster row's index buffer), `group` the group it is a MEMBER of and
/// `refined` the group that PRODUCED it (kNoGroup = level 0), both GLOBAL indices of
/// `GpuScene::groupBuffer`; the sphere is the header's culling bound (mesh space).
struct GpuCluster {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t group = 0;
    uint32_t refined = 0xFFFFFFFFu;
    float    sphere[4] = {};
};
static_assert(sizeof(GpuCluster) == 32, "the GPU cluster table's stride is a contract");

/// ONE GROUP of a mesh's DAG, std430, 32 bytes — JahClusterCut.glsl's
/// `JahClusterGroup`: the sphere the bake grew to contain every child group's, and
/// x = the MEASURED error (mesh units; FLT_MAX = terminal), y = clusterlod's estimate,
/// z = the DAG depth, w = 0.
struct GpuClusterGroup {
    float sphere[4] = {};
    float error[4] = {};
};
static_assert(sizeof(GpuClusterGroup) == 32, "the GPU group table's stride is a contract");

/// THE FLAGS WORD — the predicates every consumer used to recompute for itself.
/// ONE place writes them (`OgreScene::gpuFlagsFor`); nobody else asks the
/// question again.
enum GpuInstanceFlag : uint32_t {
    kGpuVisible = 1u << 0,      ///< shown, and in a world-geometry channel (kVisibleBit|kMovableBit)
    kGpuCaster = 1u << 1,       ///< casts a shadow in at least one caster channel
    kGpuMover = 1u << 2,        ///< kMovableBit: the document says it moves
    kGpuGiVisible = 1u << 3,    ///< kGiGeometryBit: it bounces light
    kGpuAlphaTested = 1u << 4,  ///< a sub-item's datablock has an alpha test (no any-hit shader)
    kGpuSkinned = 1u << 5,      ///< it has a skeleton instance (its MESH's buffers hold the bind pose; its skin cache the pose)
    kGpuOverlay = 1u << 6,      ///< its render queue is at or above the overlay queues
    kGpuRayTraced = 1u << 7,    ///< the TRACED SET: the conjunction the ray tier used to walk for
    kGpuDragMover = 1u << 8,    ///< MOVER-1: the user has hold of it right now
    kGpuAtom = 1u << 9,         ///< ATOM S3-DRAW: the id pass draws it, the decode shades it (OgreScene::atomRouteFor)
};

/// The per-(mesh, level) row Atom P3's selection and P4's voxeliser read: the
/// index range of that level (of submesh 0) AND THE LEVEL'S MEASURED BOUND,
/// which is what makes the level RULE evaluable on the device.
///
/// THE BOUND IS HERE BECAUSE P3 FOUND THE TABLE COULD NOT ANSWER WITHOUT IT
/// (ATOM-SUBSTRATE-1, 2026-09-22). `GpuMesh` carries only the level-0 bound —
/// which is 0 by definition — so a shader holding these tables could decide
/// "level 0 or coarser" and no more: `lodLevelForWorldError` WALKS the bounds
/// and there was nothing to walk. The bound belongs beside the range it
/// selects, so the row grew from 8 to 16 bytes rather than a second table
/// appearing next to this one. `bound` is `MeshData::lodBounds[level-1]` — the
/// measured two-sided distance of this level's surface from the authored one,
/// IN THE MESH'S OWN UNITS — and it is 0 for level 0, which is the honest value
/// and also exactly what makes the walk's first comparison free.
///
/// `geomRow` (ATOM P4b, was `reserved`) IS THE SUBMESH DIMENSION: the GEOMETRY
/// ROW of (this mesh, this level, SUBMESH 0). The rows of one level are
/// contiguous, so submesh s is `geomRow + s` — which is how a shader holding
/// only this table reaches a submesh's vertex and index addresses. `kNoGeomRow`
/// when this (mesh, level) has no readable geometry, which a consumer must test:
/// it is the honest state of a mesh without a float3 position, without an index
/// buffer, or on a device with no buffer device addresses.
///
/// `partBase`/`partCount` (ATOM P4b, the row grew from 16 to 32 bytes) name this
/// level's PARTITIONS in the partition tables: its index range split into pieces
/// of `VctVoxelizer::kIndicesPerPartition`, each with its own mesh-local AABB, so
/// a voxel group that misses a piece skips it whole. A partition is a fact about
/// the MESH (like its rows), so the table is rebuilt only when the mesh set
/// changes and never per voxelisation.
struct GpuMeshLevel {
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    float    bound = 0.0f;
    uint32_t geomRow = 0xFFFFFFFFu;
    uint32_t partBase = 0;
    uint32_t partCount = 0;
    uint32_t pad[2] = {};
};
static_assert(sizeof(GpuMeshLevel) == 32, "the GPU level table's stride is a contract");

/// THE CHANGE FEED (ATOM-CPU-WALKS-1). A consumer that keeps a fact DERIVED from
/// the table — the ray tier's traced set and its BLAS references, the material
/// words a scene's decode draws follow, the card candidates — registers here and is
/// told about every slot whose entry changed, AT the write: a stage, a swap-remove's
/// renumbering, a free, a ray-level patch. That is what lets each of them keep its
/// fact current in O(changes) instead of re-deriving it with a walk over every slot
/// every frame (W13: four such walks cost 30-45 ms a frame at 10k instances).
///
/// THE CONTRACT: `gpuSlotChanged(slot, now)` is called with the mirror's entry as it
/// is NOW (null = the slot is cleared: freed, or the tail a swap-remove emptied);
/// the consumer replaces whatever it derived from that slot. It is never told what
/// the entry USED to say — a consumer that needs the old contribution keeps its own
/// per-slot record (the entry's old mesh may already be another mesh's index). A
/// consumer registered late is PRIMED with every live slot (`addObserver`), once.
/// `gpuSceneReset` means the table was destroyed: every slot reads as cleared.
class GpuSceneObserver {
public:
    virtual ~GpuSceneObserver() = default;
    virtual void gpuSlotChanged(uint32_t slot, const GpuInstance *now) = 0;
    virtual void gpuSceneReset() = 0;
};

/// THE MATERIAL WORDS A SCENE'S ITEMS WEAR, kept by the change feed (ATOM-CPU-WALKS-1):
/// a count per word, and the word each slot contributed. The decode draws — the
/// split's screen draws and the ray tier's hit draws — follow the SET of words, which
/// used to be re-derived by a walk + sort over every slot whenever any slot was
/// written (every mover frame). `set` is O(log W) and marks the set changed only when
/// a word appears or disappears; `take` hands the sorted set over and clears the mark.
/// `visits` counts the slots it was told about — the suite's "a still frame visits
/// nothing" (atom.words_still).
class WordCounts {
public:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    void set(uint32_t slot, uint32_t word) {
        ++mVisits;
        if (slot >= mOfSlot.size()) mOfSlot.resize(size_t(slot) + 1u, kNone);
        uint32_t &was = mOfSlot[slot];
        if (was == word) return;
        if (was != kNone) {
            auto it = mCount.find(was);
            if (it != mCount.end() && --it->second == 0u) {
                mCount.erase(it);
                mChanged = true;
            }
        }
        if (word != kNone && mCount[word]++ == 0u) mChanged = true;
        was = word;
    }
    void reset() {
        mOfSlot.clear();
        if (!mCount.empty()) mChanged = true;
        mCount.clear();
    }
    bool changed() const { return mChanged; }
    /// The set, ascending (what `syncScreenDecodes` / `syncSceneDecodes` take).
    std::vector<uint32_t> take() {
        mChanged = false;
        std::vector<uint32_t> out;
        out.reserve(mCount.size());
        for (const auto &kv : mCount) out.push_back(kv.first);
        return out;
    }
    unsigned long long visits() const { return mVisits; }

private:
    std::vector<uint32_t> mOfSlot;
    std::map<uint32_t, uint32_t> mCount;
    bool mChanged = true;
    unsigned long long mVisits = 0ull;
};

class GpuScene {
public:
    /// Levels per mesh entry in the range table. The bake tops out well below
    /// this (six on the deepest shipped chain); a mesh with more is CLAMPED and
    /// says so once in the log rather than writing past its stride.
    static constexpr uint32_t kLevelsPerMesh = 8u;
    /// Submeshes per mesh entry in the GEOMETRY ROW table. Every mesh this engine
    /// bakes has exactly one; a mesh with more than this is CLAMPED and says so
    /// once, like the level bound above.
    static constexpr uint32_t kSubmeshesPerMesh = 8u;
    /// A geometry row is `Ogre::VctVoxelizer::GeometryRow` — 12 words, 48 bytes.
    /// THE FORMAT IS OGRE'S ON PURPOSE (the shader that reads it is Ogre's), so
    /// this header does not name the type and no consumer of GpuScene.h drags in
    /// HlmsPbs; the writer (`OgreScene::acquireGpuMesh`) hands over 48 bytes.
    static constexpr uint32_t kGeomRowWords = 12u;
    /// Rows per mesh entry: one per (level, submesh), then THE CLUSTER ROW
    /// (ATOM-CLUSTER-CUT) — the level-0 row with the cluster stream as its index
    /// buffer (GpuMesh::dag.w names it; `clusterRowIndex`).
    static constexpr uint32_t kClusterRow = kLevelsPerMesh * kSubmeshesPerMesh;
    static constexpr uint32_t kGeomRowsPerMesh = kClusterRow + 1u;
    /// A flat DAG's leaf (a mesh with no baked DAG): the bake's own leaf size.
    static constexpr uint32_t kFlatClusterTriangles = 128u;
    static constexpr uint32_t kNoGroup = 0xFFFFFFFFu;
    static constexpr uint32_t kNoGeomRow = 0xFFFFFFFFu;
    /// No material word yet (see GpuInstance::ids).
    static constexpr uint32_t kNoMaterialWord = 0xFFFFFFFFu;

    /// The row index of (mesh entry, level, submesh). The rows of ONE LEVEL are
    /// contiguous, which is the contract `GpuMeshLevel::geomRow` relies on: a
    /// shader that has the level's row can reach submesh s at `geomRow + s`.
    static uint32_t geomRowIndex(uint32_t meshIndex, uint32_t level, uint32_t submesh) {
        return meshIndex * kGeomRowsPerMesh + level * kSubmeshesPerMesh + submesh;
    }
    static uint32_t clusterRowIndex(uint32_t meshIndex) { return meshIndex * kGeomRowsPerMesh + kClusterRow; }

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
    /// THE BOUND EVERY READER MUST USE. The buffer is `slotCapacity()` entries
    /// long and only the first `slotCount()` of them describe live items: the
    /// tail between the two holds whatever the last scene state left there. A
    /// freed slot IS cleared and copied (see `onSlotFreed`), so the tail reads
    /// as an empty entry rather than as a ghost — but a reader that iterates to
    /// the capacity is reading memory that means nothing, and a compute job must
    /// take the count as a parameter.
    uint32_t slotCount() const { return mSlotCount; }
    void setSlotCount(uint32_t n);

    /// Writes one slot's entry into the CPU mirror. `prevWorld` is taken from
    /// the mirror's own current world — so it is EXACTLY the last frame's — and
    /// the caller never supplies it. Born slots have prevWorld = world.
    ///
    /// A SLOT IS STAGED AT MOST ONCE PER UPDATE, and that is load-bearing rather
    /// than tidy: seven seams call `markGpuSlotDirty` and two of them fire on one
    /// event (an attach marks through `indexItemNode` and again through the
    /// attach), so a second stage in the same update would read the world the
    /// first had just written and set `prevWorld` to THIS frame's pose — erasing
    /// the motion of anything that moved in the same frame. The second call is
    /// dropped (`mStagedAt`).
    void stage(uint32_t slot, const GpuInstance &in);
    /// Has this slot already been staged in the update now being assembled?
    bool stagedThisUpdate(uint32_t slot) const {
        return slot < mSlotCapacity && mStagedAt[slot] == mUpdateSerial;
    }

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
    /// THE FRAME'S MOVED SET, read-only (PHOTON-CARDS-4: the surface cache's
    /// mover-shadow trigger): the slots the frame's update staged — a transform
    /// write, a flags change, a birth — valid only while `movedThisFrame()` says
    /// the last closing update ran in the VaoManager frame `frame`; any other
    /// frame moved nothing.
    const std::vector<uint32_t> &movedSlots() const { return mMovedLastFrame; }
    bool movedThisFrame(uint32_t frame) const { return mLastCloseFrame == frame; }

    const GpuInstance &entry(uint32_t slot) const { return mMirror[slot]; }

    /// THE CHANGE FEED (GpuSceneObserver): registers and PRIMES `o` with every
    /// live slot (one walk, at registration only); `removeObserver` before `o` dies.
    void addObserver(GpuSceneObserver *o);
    void removeObserver(GpuSceneObserver *o);

    /// THE RAY LEVEL, PATCHED IN PLACE (ATOM-CPU-WALKS-1). `OgreScene::
    /// updateRayLevels` runs AFTER the frame's scan, so a re-stage would reach the
    /// device a frame late — and a second stage in the same frame would take this
    /// frame's pose as the previous one. A patch writes `ids[3]` alone into the
    /// mirror (prevWorld untouched), tells the observers, and queues the slot;
    /// `flushPatched` copies the queued entries NOW (the ray tier reads the table
    /// right after), and `update` takes any still queued.
    void patchRayLevel(uint32_t slot, uint32_t level);
    void flushPatched();
    /// The mirror as a raw pointer. The dirty scan reads it once per item, and
    /// in a Debug build (which is the daily driver) an accessor call per item is
    /// a measurable part of the walk.
    const GpuInstance *mirrorData() const { return mMirror.data(); }

    /// A REMOVAL SWAPPED `from` INTO `to`. The ONE place the swap-remove is
    /// handled for the table: the mirror entry moves and `to` is re-copied this
    /// frame (the caller adds it to the dirty list).
    void onSlotMoved(uint32_t from, uint32_t to);
    /// A SLOT IS NO LONGER LIVE. The LAST item in the index has no swap partner,
    /// so nothing moves into its place and the entry it leaves behind must be
    /// cleared — otherwise the next attach lands in a slot that is still "born"
    /// and takes the DEAD object's world as its previous world, which is a
    /// motion vector out of another object's grave. Cleared in the mirror AND
    /// queued for the device, so the tail past `slotCount()` is never a ghost.
    void onSlotFreed(uint32_t slot);

    // --- the mesh table ----------------------------------------------------
    /// Find-or-create this mesh's entry, reference-counted. `levels` holds up to
    /// kLevelsPerMesh ranges. THE ENTRY HOLDS A MeshPtr: an instance that names
    /// a mesh index must be able to reach the mesh, and the hold is what keeps
    /// the geometry alive under a structure built over it. Every hold is dropped
    /// by `destroy()`, which OgreScene::destroy calls while Root is still alive
    /// (a MeshPtr outliving Root throws in VaoManager).
    uint32_t acquireMesh(const Ogre::MeshPtr &mesh, const GpuMesh &desc,
                         const GpuMeshLevel *levels, uint32_t levelCount);
    /// THE MESH'S CLUSTER DAG (ATOM-CLUSTER-CUT), given once at its first acquire:
    /// its clusters with LOCAL group indices (rebased onto the global group table
    /// here) and its groups. The two tables are rebuilt WHOLE when the mesh set
    /// changes — like the partitions, a DAG cannot change while its mesh lives — and
    /// `dag` x/y/z of the entry are written then (w, the cluster row, by the caller).
    void setMeshDag(uint32_t meshIndex, std::vector<GpuCluster> clusters,
                    std::vector<GpuClusterGroup> groups, uint32_t clusterRow);
    /// Uploads the cluster tables if the mesh set moved (update() calls it; a
    /// consumer binding the tables outside the frame's scan calls it first).
    void flushClusterTables();
    Ogre::UavBufferPacked *clusterBuffer() const { return mClusterBuffer; }
    Ogre::UavBufferPacked *groupBuffer() const { return mGroupBuffer; }
    uint32_t clusterCount() const { return mClusterCount; }
    /// The most clusters one live mesh carries (the cut's per-instance loop bound).
    uint32_t maxClustersPerMesh() const { return mMaxClustersPerMesh; }
    const GpuMesh &meshEntry(uint32_t index) const { return mMeshMirror[index]; }
    /// The CPU mirror of the global tables (valid after flushClusterTables).
    const std::vector<GpuCluster> &clusterMirror() const { return mClusterMirror; }
    const std::vector<GpuClusterGroup> &groupMirror() const { return mGroupMirror; }
    /// The mesh an entry names, or a null pointer.
    const Ogre::MeshPtr &meshAt(uint32_t index) const;
    uint32_t meshEntryCount() const { return uint32_t(mMeshEntries.size()); }
    /// Moves whenever an entry starts or stops naming a mesh (a new mesh acquired,
    /// the last reference to one released): what a consumer that caches per-mesh
    /// facts by entry index compares (the ray tier's TLAS job inputs).
    unsigned long long meshSetSerial() const { return mMeshSetSerial; }
    /// Drops one reference; the entry is FREED (and the index recycled) with the
    /// last one.
    void releaseMesh(const Ogre::Mesh *mesh);
    /// The index of a mesh already in the table, or npos.
    uint32_t meshIndex(const Ogre::Mesh *mesh) const;
    static constexpr uint32_t kNoMesh = 0xFFFFFFFFu;

    // --- THE ROW OVERRIDE (PHOTON-SKIN-1) -----------------------------------
    /// A BLOCK OF GEOMETRY ROWS THAT BELONGS TO NO MESH: one mesh-table entry's
    /// worth (kGeomRowsPerMesh rows, 3 KB) taken out of the same table, so its row
    /// indices are the same arithmetic (`geomRowIndex(block, level, submesh)`) and
    /// it grows, flushes and is re-created with the table like every other row.
    /// The entry itself describes nothing (no mesh, zero counts, no levels, no
    /// partitions — `ensurePartitions` and `recordBound` skip it as they skip a
    /// free entry), and no instance names it as its mesh. Returns the ENTRY index
    /// (kNoMesh when the table is not live); the block's level-0/submesh-0 row is
    /// `geomRowIndex(entry, 0, 0)`.
    uint32_t acquireRowBlock();
    /// Zeroes the block's rows (a recycled block must never read as the dead
    /// cache's addresses) and hands the entry back.
    void releaseRowBlock(uint32_t entry);
    /// THE OVERRIDE ITSELF, by NODE: the skin row `composeGpuInstance` writes into
    /// `GpuInstance::raster[2]` for this node (kNoGeomRow = none). By node and not
    /// by slot because a slot is renumbered by every removal (the swap-remove) and
    /// a node is not; the caller marks the node's slot dirty so the next scan
    /// re-composes the entry with it.
    void setSkinRow(uint32_t node, uint32_t row);
    uint32_t skinRowOf(uint32_t node) const {
        auto it = mSkinRows.find(node);
        return it == mSkinRows.end() ? kNoGeomRow : it->second;
    }
    size_t skinRowCount() const { return mSkinRows.size(); }

    // --- what the readers bind --------------------------------------------
    // The three buffers ARE the facility: `instanceBuffer` is what the test and
    // tool readback downloads and what Atom P3's cull will bind as a UAV, and
    // the mesh and level tables are P3's and P4's selection inputs. They are
    // `UavBufferPacked`s rather than raw Vulkan buffers precisely so an
    // `HlmsComputeJob` can take them with no render-system knowledge.
    Ogre::UavBufferPacked *instanceBuffer() const { return mInstanceBuffer; }
    Ogre::UavBufferPacked *meshBuffer() const { return mMeshBuffer; }
    Ogre::UavBufferPacked *levelBuffer() const { return mLevelBuffer; }
    /// The CPU mirror's (mesh entry, level) row — what the device's level table
    /// holds after the next update.
    const GpuMeshLevel &levelAt(uint32_t meshIndex, uint32_t level) const {
        return mLevelMirror[size_t(meshIndex) * kLevelsPerMesh + level];
    }
    uint32_t levelMirrorEntries() const { return uint32_t(mLevelMirror.size()); }
    /// THE GEOMETRY ROW TABLE (ATOM P4b): the vertex and index device addresses and
    /// the vertex layout of every (mesh, level, submesh) the scene holds, written
    /// ONCE at attach. The voxeliser binds it for every dispatch
    /// (`VctVoxelizer::setGeometrySource`) instead of re-describing the world's
    /// geometry on the CPU per build; phase D's raster decode reads the same rows.
    Ogre::UavBufferPacked *geomBuffer() const { return mGeomBuffer; }

    /// Writes one geometry row (48 bytes, the caller's `GeometryRow`) into the
    /// mirror and queues it for the device. Called from `acquireMesh`'s caller
    /// while it still holds the mesh; a row nobody writes stays zero, and
    /// `GpuMeshLevel::geomRow` is what says whether it means anything.
    void stageGeomRow(uint32_t rowIndex, const void *row48Bytes);
    /// The CPU mirror's row (kGeomRowWords words), or null past the table.
    const uint32_t *geomRowAt(uint32_t rowIndex) const {
        const size_t at = size_t(rowIndex) * kGeomRowWords;
        return at + kGeomRowWords <= mGeomMirror.size() ? &mGeomMirror[at] : nullptr;
    }
    /// Points a level entry at its submesh-0 geometry row. Separate from
    /// `acquireMesh` because a row's index depends on the ENTRY's index, which
    /// acquireMesh is the thing that decides.
    void setLevelGeomRow(uint32_t meshIndex, uint32_t level, uint32_t rowIndex);
    /// UPLOADS THE GEOMETRY ROWS IF ANY ARE OWED, NOW. `update()` does it once per
    /// frame with the rest of the tables, but a GI rebuild reads the rows OUTSIDE a
    /// frame's dirty scan — and a row that has not reached the device is a ZERO
    /// address, which a shader dereferences and the channel hangs on (Xid 109). Any
    /// consumer that binds the table before a dispatch calls this first.
    void flushGeomRows();
    /// THE PARTITION TABLES (ATOM P4b): every live (mesh, level)'s index range cut
    /// into pieces of `partitionIndices` indices, one (geometry row, first index,
    /// index count, 0) per piece, and the piece's MESH-LOCAL AABB computed on the
    /// device by Ogre's own VCT/AabbCalculator job. Rebuilt only when the mesh set
    /// changed (a new mesh entry, or the last reference to one released) — a
    /// partition, like a row, cannot change while its mesh lives — so a voxel
    /// rebuild reads them and never describes geometry. The geometry rows are
    /// flushed first: the AABB job reads positions through them.
    /// Returns true when it had to rebuild (a caller that reports work counts it).
    bool ensurePartitions(Ogre::HlmsManager *hlmsManager, Ogre::RenderSystem *renderSystem,
                          uint32_t partitionIndices);
    Ogre::UavBufferPacked *partitionAabbBuffer() const { return mPartAabbBuffer; }
    uint32_t partitionCount() const { return mPartCount; }
    /// THE MOST RECORDS A GATHER CAN WRITE PER OCTANT: every instance of every mesh
    /// at its FINEST level's partition count (a coarser level has fewer). O(mesh
    /// entries), not O(instances) — the per-entry reference count IS the instance
    /// count. Valid after ensurePartitions.
    uint64_t recordBound() const;

    // --- what it cost (the frame monitor's row and the suite's assertions) --
    unsigned long long writes() const { return mWrites; }        ///< slot writes, ever
    unsigned long long copies() const { return mCopies; }        ///< device copy RUNS, ever
    unsigned long long frames() const { return mUpdates; }       ///< update() calls that copied
    unsigned long long grows() const { return mGrows; }
    double lastCopyMs() const { return mLastCopyMs; }
    unsigned lastDirtyCount() const { return mLastDirtyCount; }
    /// Slot changes the change feed has told its consumers, ever (priming excluded).
    unsigned long long notifies() const { return mNotifies; }
    unsigned long long epoch() const { return mEpoch; }

private:
    void growTo(uint32_t capacity);
    void growMeshTable(uint32_t capacity);

    Ogre::VaoManager *mVao = nullptr;
    Ogre::UavBufferPacked *mInstanceBuffer = nullptr;
    Ogre::UavBufferPacked *mMeshBuffer = nullptr;
    Ogre::UavBufferPacked *mLevelBuffer = nullptr;
    Ogre::UavBufferPacked *mGeomBuffer = nullptr;
    /// The partition list (input to the AABB job: a TEX buffer, which is how
    /// VCT/AabbCalculator binds it) and the partition AABBs it writes.
    Ogre::TexBufferPacked *mPartBuffer = nullptr;
    Ogre::UavBufferPacked *mPartAabbBuffer = nullptr;
    uint32_t mPartCount = 0;
    uint32_t mPartCapacity = 0;
    bool mPartitionsDirty = true;

    /// THE CPU MIRROR — authoritative. The device table is a copy of it, never
    /// the other way round: nothing here ever reads the device buffer back (the
    /// `AsyncTextureTicket`/`flushCommands` trap has no equivalent that is worth
    /// paying inside a frame).
    std::vector<GpuInstance> mMirror;
    std::vector<unsigned char> mBorn;  ///< 0 = this slot has never been staged
    /// THE UPDATE THIS SLOT WAS LAST STAGED IN — the stamp that replaces every
    /// membership SEARCH in the write path. A linear `std::find` over the dirty
    /// list per slot is O(N^2) on exactly the case that matters: one parent with
    /// N children dragged for two frames running makes both the dirty set and
    /// the previous frame's mover set N long (64 million compares per frame at
    /// 8,000 children). A stamp answers the same question in one load.
    std::vector<uint32_t> mStagedAt;
    uint32_t mUpdateSerial = 1u;   ///< never 0: 0 is "never staged"
    uint32_t mSlotCapacity = 0;
    uint32_t mSlotCount = 0;

    std::vector<GpuMesh> mMeshMirror;
    std::vector<GpuMeshLevel> mLevelMirror;
    /// The geometry rows as RAW WORDS (see kGeomRowWords): 12 per row,
    /// kGeomRowsPerMesh rows per mesh entry.
    std::vector<uint32_t> mGeomMirror;
    bool mGeomDirty = false;
    struct MeshEntry {
        Ogre::MeshPtr mesh;
        uint32_t refs = 0;
        /// A ROW BLOCK (acquireRowBlock): no mesh, no references, rows only.
        bool rowBlock = false;
        /// The mesh's DAG (setMeshDag), local group indices; the global tables are
        /// rebuilt from these.
        std::vector<GpuCluster> clusters;
        std::vector<GpuClusterGroup> groups;
    };
    Ogre::UavBufferPacked *mClusterBuffer = nullptr;
    Ogre::UavBufferPacked *mGroupBuffer = nullptr;
    std::vector<GpuCluster> mClusterMirror;
    std::vector<GpuClusterGroup> mGroupMirror;
    uint32_t mClusterCapacity = 0, mGroupCapacity = 0, mClusterCount = 0, mMaxClustersPerMesh = 0;
    bool mClusterDirty = false;
    /// The row override per NODE (setSkinRow).
    std::unordered_map<uint32_t, uint32_t> mSkinRows;
    std::vector<MeshEntry> mMeshEntries;
    std::vector<uint32_t> mFreeMeshSlots;
    std::unordered_map<const Ogre::Mesh *, uint32_t> mMeshIndex;
    uint32_t mMeshCapacity = 0;
    unsigned long long mMeshSetSerial = 0ull;
    bool mMeshDirty = false;
    bool mLevelDirty = false;

    /// THE CHANGE FEED's consumers, and the one place they are told.
    std::vector<GpuSceneObserver *> mObservers;
    void notify(uint32_t slot, const GpuInstance *now) {
        ++mNotifies;
        for (GpuSceneObserver *o : mObservers) o->gpuSlotChanged(slot, now);
    }
    unsigned long long mNotifies = 0ull;
    /// Slots whose ray level was patched since their last copy (patchRayLevel).
    std::vector<uint32_t> mPatched;
    /// Copies `mCopySet` (sorted, unique, in range) to the device: one staging map,
    /// one Destination per contiguous run.
    void copySet();
    /// The slots this frame's copy must also close (prevWorld = world).
    std::vector<uint32_t> mMovedLastFrame;
    /// Slots freed since the last update: cleared in the mirror and owed a copy,
    /// so the device never holds a dead item's entry past `slotCount()`.
    std::vector<uint32_t> mFreedSlots;
    // Scratch kept between updates so a frame allocates nothing: the copy set,
    // its contiguous runs, and the staging destinations those runs become.
    std::vector<uint32_t> mCopySet;
    struct Run {
        uint32_t first = 0, count = 0;
    };
    std::vector<Run> mRuns;
    std::vector<Ogre::StagingBuffer::Destination> mDests;
    /// The VaoManager frame the last close ran in. The previous frame's movers
    /// are closed ONCE PER FRAME, not once per CALL: two updates inside one
    /// frame (a pre-frame reader and the frame's own pass) must not close
    /// `prevWorld` before the frame that reads it has rendered.
    uint32_t mLastCloseFrame = 0xFFFFFFFFu;
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
