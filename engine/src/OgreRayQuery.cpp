// PHOTON R1 — THE HARDWARE RAY-QUERY TIER.
//
// WHAT THIS IS. A ray-traceable copy of the scene, kept current every frame:
// one bottom-level acceleration structure (BLAS) per unique mesh, built from
// Ogre's OWN vertex and index buffers, and one top-level structure (TLAS) over
// the instances the scene draws. Its consumers — the screen-probe gather, R3
// (hard sun contact shadows, `recordSunContact`) and R5 (ray-traced
// reflections) — every one of them reads this same structure. What R1 ships is
// the structure, the switch that turns it off, and the proof that a ray hits
// where the mathematics says it hits.
//
// WHY IT LOOKS LIKE THIS.
//
//  * OGRE'S RootLayout HAS NO ACCELERATION-STRUCTURE DESCRIPTOR TYPE
//    (OgreRootLayout.h, DescBindingTypes), and its own comment lists the four
//    places that must change if that enum moves — plus the root-layout
//    compatibility hash. So the rays NEVER go through Hlms: this file owns its
//    VkDescriptorSetLayout, its VkPipeline and its descriptor pool, and Ogre's
//    Hlms, RootLayout, BarrierSolver and compositor are not involved in the
//    trace at all. (That is also how Lumen is built: traces are compute,
//    shading reads a cache.)
//  * THE WORK IS RECORDED INTO OGRE'S OWN FRAME COMMAND BUFFER, at the item
//    walk's point (OgreEngine::applyShadowCacheDirties, after updateSceneGraph
//    and before any workspace renders). No private submit, no fence wait and no
//    `stall()` on the frame thread — audit C-13 is explicit that the S3 spike's
//    sync model could not be lifted, and this is what replaces it. The one
//    exception is `traceBlocking` below, the TOOL/TEST path, which says so in
//    its own comment.
//  * THE TRACED SET COMES FROM THE GPU SCENE'S TABLE, which is indexed by the
//    scene's own item slot — never from `SceneManager::getMovableObjectIterator`
//    (audit C-4: the spike traced the editor's gizmo arrows and light icons).
//    See `writeRayInstances` below and GpuScene.h.
//
// WHAT IT NEEDS FROM THE PIN (patches-only law):
//   * 0038 — the instance at Vulkan 1.2 when the loader allows, the seven
//     extension names, and exactly three feature bits (masked, because upstream
//     queries and enables through ONE pNext chain). Without it vkCreateDevice
//     never hears of ray tracing and `hasRayQuery()` is false everywhere.
//   * 0039 — SHADER_DEVICE_ADDRESS + ACCELERATION_STRUCTURE_BUILD_INPUT on the
//     device-local VBO pools, so a BLAS reads Ogre's buffers IN PLACE. Without
//     it every mesh's geometry would have to be read back and duplicated.
//   * NOTHING ELSE. The SPIR-V is compiled at BUILD TIME at spirv1.4 by
//     glslangValidator (see irisgl/engine/CMakeLists.txt) — the pin's own GLSL
//     path never sets a target environment, so it would emit SPIR-V 1.0, and
//     the spike's "commit the bytes" shortcut is gone with it.
//
// THE BOUNDARY. This is the only TU in the engine that includes Vulkan. The
// public headers stay Vulkan-free (RayQueryStatus is plain data), and on a
// platform without the Vulkan render system the whole file compiles to nothing
// (JAH_RAY_QUERY, set by CMake — macOS takes that path, because MoltenVK
// exposes neither extension: SPECS/research/MOLTENVK_RAY_QUERY_2026-09-14.md).
#include "EnginePrivate.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Vao/OgreVertexArrayObject.h>
#include <Vao/OgreVertexBufferPacked.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreUavBufferPacked.h>

#if JAH_RAY_QUERY

#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanDevice.h"
#include "OgreVulkanQueue.h"
#include "Vao/OgreVulkanBufferInterface.h"
#include "Vao/OgreVulkanVaoManager.h"

#include "rayquery/rq_rays_spv.h"
#include "rayquery/rq_reflect_spv.h"
#include "rayquery/rq_reflect_filter_spv.h"
#include "rayquery/rq_card_parity_spv.h"
#include "rayquery/rq_sun_contact_spv.h"
#include "rayquery/rq_hit_composite_spv.h"
#include "rayquery/rq_card_movers_spv.h"
// THE SCREEN-PROBE GATHER — a Component of ours (GATHER-1a). Its three compute
// jobs, its atlases and its pipelines live in OgreScreenProbeGather.cpp; this
// file is its HOST (the device, the retire window, the frame's command buffer,
// the TLAS) and the one that is friends with the scene it reads.
#include "HlmsAtom.h"
#include "ScreenProbeGather.h"
#include "SkinCache.h"
#include "SurfaceCache.h"

#include <Animation/OgreSkeletonInstance.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreItem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreSubItem.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace jahshaka { namespace engine { namespace detail {
namespace {

using Clock = std::chrono::steady_clock;
inline double msSince(const Clock::time_point &t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// HOW MANY FRAMES OF TIMESTAMPS ARE IN FLIGHT. A timestamp is read back with
/// the availability bit and NEVER with VK_QUERY_RESULT_WAIT_BIT — a wait is on
/// availability, not on a fence, and waiting for a query that was reset but
/// never written hangs forever (the S3 spike lost an hour to exactly that).
/// Three is Ogre's own dynamic-buffer multiplier, i.e. the depth of the
/// pipeline this work rides.
constexpr unsigned kFramesInFlight = 3u;
/// Two timestamps per pass (BLAS batch, TLAS), per frame slot, then THE SKIN
/// CACHE's three (PHOTON-SKIN-1): before the skin dispatch, between it and the
/// skinned structures' builds/refits, and after them.
constexpr unsigned kQueriesPerFrame = 8u;
/// HOW MANY SCENES MAY HOLD TIMESTAMP SLOTS AT ONCE. The product case is more
/// than one drawn scene per frame — the editor plus a material-preview or
/// thumbnail scene — and each needs its OWN query range, or the second scene of
/// a frame overwrites the first's timestamps before they are read.
constexpr unsigned kMaxTimedScenes = 8u;
constexpr unsigned kQueryCount = kMaxTimedScenes * kFramesInFlight * kQueriesPerFrame;
/// Compaction size queries live in a RING, not at slots 0..n: two BLAS batches
/// within the read-back window would otherwise write the same slots and the
/// second batch's sizes would be read as the first's — a COMPACT copy into a
/// buffer sized for a different structure.
constexpr unsigned kCompactRing = 64u;

/// A plain owned VkBuffer. Ogre's VaoManager cannot make one with the usage
/// bits an acceleration structure needs (AS storage, scratch, instance arrays),
/// so everything that is not BLAS INPUT is ours.
struct RawBuffer {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize  size = 0;
    void         *mapped = nullptr;
};

/// Every device entry point this tier uses; all are extension functions, so
/// none is in the loader's static export table.
struct RtFuncs {
    PFN_vkGetBufferDeviceAddressKHR getBufferDeviceAddress = nullptr;
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAsDeviceAddress = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteProperties = nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR cmdCopy = nullptr;

    bool load(VkDevice dev) {
#define JAH_RT_LOAD(member, name)                                           \
    member = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name)); \
    if (!member) return false
        JAH_RT_LOAD(getBufferDeviceAddress, vkGetBufferDeviceAddressKHR);
        JAH_RT_LOAD(createAccelerationStructure, vkCreateAccelerationStructureKHR);
        JAH_RT_LOAD(destroyAccelerationStructure, vkDestroyAccelerationStructureKHR);
        JAH_RT_LOAD(getBuildSizes, vkGetAccelerationStructureBuildSizesKHR);
        JAH_RT_LOAD(cmdBuild, vkCmdBuildAccelerationStructuresKHR);
        JAH_RT_LOAD(getAsDeviceAddress, vkGetAccelerationStructureDeviceAddressKHR);
        JAH_RT_LOAD(cmdWriteProperties, vkCmdWriteAccelerationStructuresPropertiesKHR);
        JAH_RT_LOAD(cmdCopy, vkCmdCopyAccelerationStructureKHR);
#undef JAH_RT_LOAD
        return true;
    }
};

/// THE REFIT, off by default. A full TLAS rebuild is NVIDIA's own recommendation
/// ("consider PREFER_FAST_TRACE and perform only rebuilds") and it measures
/// 0.21-0.35 ms GPU at 8,001 instances on this rig — inside any budget — while
/// making the better tree. The refit is the OPTIMISATION, kept behind this
/// switch so it can be measured against the rebuild rather than assumed better.
inline bool preferRefit() {
    static const bool k = getenv("JAH_RQ_REFIT") != nullptr;
    return k;
}

/// One scratch ARENA per batch, handed out at offsets — NOT the spike's one
/// shared buffer with a serialising barrier between every build (which is why
/// its 26 tiny BLAS builds cost 4.6-7.6 ms and why every one of its BLAS
/// figures is an upper bound).
struct ScratchArena {
    RawBuffer     buffer;
    VkDeviceAddress base = 0;
    VkDeviceSize   used = 0;
    VkDeviceSize   alignment = 256;
};

// ---- R5: ray-traced reflections (PHOTON_SPEC §7 R5) ------------------------

// THE GATE'S VALUE IS NOT HERE. It used to be, as `kRayReflectRoughness = 0.4f`,
// documented as sitting "just above `ssrRoughnessCutoff`'s 0.35 default on
// purpose" — and by the time lane SSR-3 read it, every part of that was wrong:
// the constant was DEAD (RAYROW-1 made the cutoff the project's, read at the one
// call site below), and the 0.35 it claimed to sit above was never a perceptual
// number at all (the march compared it to an un-square-rooted GGX alpha, so it
// was a band at perceptual 0.581, ABOVE this constant rather than below it).
// Deleted with the second cutoff it described; there is one cutoff now and the
// project owns it.

// THE GATE'S FEATHER IS NOT HERE EITHER, and for a reason worth the line: since
// lane SSR-3 the screen-space march fades over the SAME 0.1, so a copy of it in
// this file would be two numbers one edit apart. `kRayReflectFeather` lives in
// EnginePrivate.h, which both this TU and OgreChain.cpp already include, and its
// note there says what each half does with it.
/// THE TEMPORAL MEAN'S FLOOR on 1/n. The mean starts as a true running average
/// (fastest convergence) and settles into an exponential one at this weight, so
/// it keeps following a scene whose lighting changes: 1/16 remembers about
/// sixteen frames, which at 60 Hz is a quarter of a second of lag on a moving
/// light and converges a roughness-0.3 lobe well inside the sixteen frames the
/// suite asserts.
/// 1/32 and not 1/16, MEASURED (the convergence case of gi.rt_reflect): one
/// ray per pixel per frame is a BINARY estimator at a reflected silhouette —
/// the ray either finds the bright thing or it does not — so the residual
/// frame-to-frame movement is the floor times that contrast. At 1/16 the worst
/// pixel of the fixture still moved 16/255 after sixteen frames; at 1/32 it is
/// half that and the 99th percentile is inside 2/255. Half a second of lag at
/// 60 Hz on a light that moves, which is the other side of the same number.
constexpr float kReflectHistoryFloor = 1.0f / 32.0f;
/// HOW MANY CASCADES THE HIT SHADING READS — the shader's `kMaxCascades`, and
/// the two must agree.
constexpr unsigned kMaxReflectCascades = 4u;
/// Descriptor sets and parameter buffers in flight per view. A set bound by a
/// command buffer that has not retired may not be rewritten, and this set is
/// rewritten every frame (every input can be recreated behind our back).
constexpr unsigned kReflectRing = 3u;
/// Bindings in rq_reflect.comp's set 0: the trace's fifteen, then the card
/// read's four (jah_rq_card_bindings.glsl at JAH_CARD_BINDING_BASE 15 — the
/// card table, the instance table, the Depth and Radiance layers).
constexpr unsigned kReflectBindings = 29u;
constexpr unsigned kReflectCardBinding = 15u;
/// ...then the hit's geometric normal (PHOTON-CARDS-2 fix round): the per-slot
/// geometry-row table the TLAS writer fills (19) and the GPU scene's geometry
/// rows (20) — rq_reflect.comp through jah_rq_geom.glsl.
constexpr unsigned kReflectGeomBinding = 19u;
/// ...then (PHOTON-VOXEL-4, RQ-COV-SLOT-1) every cascade's PER-HALF-AXIS COVERAGE (21 the
/// faces looking +a, 22 looking -a) and SURFACE POSITION (23, 24), by name: the hit's read
/// takes the opacity along its ray from the coverage. It used to ride the `voxelX` array by
/// the light-volume list's ORDER - right only while the list kept that order. The position
/// is the origin plane's, which the hit's point reads never take; the reader binds it
/// wherever it runs.
constexpr unsigned kReflectCovBinding = 21u;   // then 22, 23, 24: covN, posP, posN
constexpr unsigned kReflectSplitKinds = 4u;
/// ...then THE HIT RECORD (PHOTON-HIT-SHADE-1): the GPU scene's instance table
/// (25), the hit list's two images (26-27) and its buffer (28: the counters and
/// each record's sun, footprint and weight) — jah_rq_hit_record.glsl.
constexpr unsigned kReflectHitBinding = 25u;
/// THE HIT WRITE-BACK's bindings (rq_hit_composite.comp): params, the list's
/// buffer, the destinations, the decoded radiance, the reflection's mean and
/// distance, the gather's atlas.
constexpr unsigned kHitCompositeBindings = 7u;

/// A storage image this file owns outright — the temporal mean and the distance
/// beside it. Not an Ogre texture: nothing but this compute pass ever reads or
/// writes one, it is never a render target, and giving it to the compositor
/// would put a per-frame race (the ping-pong) inside a graph that rebuilds.
struct ReflectImage {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
};

/// ONE BOTTOM-LEVEL STRUCTURE PER (MESH, LEVEL) — ATOM-FARBLAS-1, A5b §4. A mesh
/// used to have exactly one, over level 0. Now every instance is written TWICE
/// into the one TLAS: a NEAR copy over the level the ray rule chose for it
/// (`GpuInstance::ids[3]`, AT-A8r) and a FAR copy over the mesh's COARSEST level,
/// so the table is keyed by the level too. A mesh with one level asks for (mesh,
/// 0) twice and gets ONE structure — the "no second build" of the decision falls
/// out of the key rather than being a special case.
struct BlasKey {
    const Ogre::Mesh *mesh = nullptr;
    uint32_t level = 0;
    bool operator==(const BlasKey &o) const { return mesh == o.mesh && level == o.level; }
};
struct BlasKeyHash {
    size_t operator()(const BlasKey &k) const {
        return std::hash<const void *>()(k.mesh) ^ (size_t(k.level) * 0x9E3779B97F4A7C15ull);
    }
};
/// What a gather asks to have built: the mesh (HELD, see Blas::mesh) and the level.
struct BlasWant {
    Ogre::MeshPtr mesh;
    uint32_t level = 0;
};

/// THE COARSEST LEVEL a mesh carries — the chain's last (ATOM-BAKE-1's
/// 128-triangle floor), 0 for a mesh with no chain. Read off the VAO list the
/// draw indexes, which is the list a BLAS is built from.
inline uint32_t coarsestLevelOf(const Ogre::Mesh *mesh) {
    if (!mesh || mesh->getNumSubMeshes() == 0) return 0u;
    const size_t n = mesh->getSubMesh(0)->mVao[Ogre::VpNormal].size();
    return n > 1u ? uint32_t(n - 1u) : 0u;
}

}   // namespace

// ---------------------------------------------------------------------------
/// THE TIER. One per engine: the device plumbing and the compute pipeline are
/// process-wide, the acceleration structures are per SCENE (each scene draws
/// its own items, and a probe preview or a thumbnail scene must never appear in
/// the editor's structure).
class RayQueryTier final : public GatherHost {
public:
    ~RayQueryTier() { close(); }

    /// Resolves the device, the entry points and the pipeline. False (with a
    /// reason in `err`) when this device has no ray query — which is not an
    /// error anywhere, it is the fallback picture.
    bool open(Ogre::RenderSystem *rs, std::string &err);
    void close();
    bool isOpen() const { return mVk != VK_NULL_HANDLE; }

    /// THE FRAME'S UPDATE for one scene, recorded into `Ogre`'s current command
    /// buffer. Cheap and O(1) on a still frame: the transform epoch gate means
    /// a scene that did not move records nothing at all.
    void updateScene(OgreScene *scene);
    /// Frees everything held for a scene that is going away.
    void forgetScene(OgreScene *scene);

    /// THE TOOL/TEST TRACE — the only place in this file that submits its own
    /// command buffer and waits on a fence, and it must never be called from a
    /// frame. It exists so that a suite can ask "what does this ray hit?" and
    /// get an answer in the same statement; every PRODUCT consumer (R2/R3/R5)
    /// will read the structures from a compute pass inside the frame instead.
    ///
    /// `rays` is {origin.xyz, tMin, dir.xyz, tMax, mask, 0,0,0} per ray; `hits`
    /// comes back as {t (<0 = miss), customIndex, primitiveIndex, hit?1:0}.
    bool traceBlocking(OgreScene *scene, const std::vector<float> &rays,
                       std::vector<float> &hits, std::string &err);
    /// gi.card_read_parity's GPU half (PHOTON-CARDS-2): the reflection's card
    /// read (jah_rq_card.glsl) asked at `queries` through a test-only job
    /// (rq_card_parity.comp) over the scene's own surface cache. Flushes
    /// Ogre's commands, submits, stalls — a suite, never a frame.
    bool cardPickBlocking(OgreScene *scene, const std::vector<CardReadQuery> &queries,
                          std::vector<CardReadPick> &out, std::string &err);

    RayQueryStatus status(const OgreScene *scene) const;

private:
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        RawBuffer storage;
        VkDeviceAddress address = 0;
        unsigned triangles = 0;
        /// The mesh is HELD, not observed: a BLAS that outlives its vertex
        /// buffers would be built over recycled VRAM, and the cache is keyed by
        /// the raw pointer, which a freed-and-reallocated Mesh would alias.
        Ogre::MeshPtr mesh;
        /// The mesh LEVEL this structure was built from (BlasKey).
        uint32_t level = 0;
        /// Compaction state machine: 0 = built, size not asked; 1 = size query
        /// written, waiting; 2 = compacted (or declined).
        unsigned compactState = 0;
        unsigned compactSlot = 0;
        uint32_t compactFrame = 0;
        /// The last frame an instance referenced this mesh (finding 5). A BLAS
        /// nothing has traced for a while is memory — and a held MeshPtr — kept
        /// for a mesh the scene may never show again.
        uint32_t lastSeen = 0;
    };

    struct SceneAs {
        std::unordered_map<BlasKey, size_t, BlasKeyHash> blasOf;
        std::vector<Blas> blas;

        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        RawBuffer tlasStorage, tlasScratch;
        /// Persistently mapped, kFramesInFlight slots: the gather writes
        /// straight into the slot this frame will build from, so a transform
        /// never passes through an intermediate vector.
        RawBuffer instances;
        unsigned  instanceCapacity = 0;
        unsigned  instanceCount = 0;      ///< near + far copies: what the TLAS holds
        unsigned  farInstanceCount = 0;   ///< ...of which the far copies
        /// The widest coarse-vs-fine gap of the set this TLAS was written from
        /// (world units): the gather's far query starts this far BEFORE its near
        /// length (audit F2). Held with the TLAS, so a still frame keeps it.
        float     farOverlap = 0.0f;
        /// The coarsest level's bound per GpuScene mesh index (with the mesh it
        /// was read for), so the writer asks the mesh records once per mesh.
        std::vector<std::pair<const Ogre::Mesh *, float>> coarseBound;
        /// THE GEOMETRY ROW OF EACH SLOT'S NEAR COPY (PHOTON-CARDS-2 fix round):
        /// GpuScene::geomRowIndex(mesh, the level its near BLAS was built from,
        /// submesh 0), 0xFFFFFFFF for a slot not traced — what the reflection
        /// rebuilds a hit's geometric normal from. Written with the instances.
        std::vector<uint32_t> geomRowOfSlot;
        unsigned  slot = 0;
        /// THE HIT DECODE'S DRAWS (PHOTON-HIT-SHADE-1): the material words they were
        /// last synced for, the GPU scene's write count and HlmsAtom's twin epoch
        /// at that sync (updateScene).
        std::vector<uint32_t> decodeWords;
        unsigned long long decodeWrites = ~0ull;
        unsigned long long decodeEpoch = ~0ull;
        /// One item wearing each synced word, and its datablock, Hlms hash and
        /// texture set (HlmsAtom::textureSetKeyOf) when last seen (the twin's
        /// staleness witness).
        struct DecodeWitness {
            uint32_t slot = 0u;
            const Ogre::HlmsDatablock *db = nullptr;
            Ogre::uint32 hash = 0u;
            uint64_t texKey = 0u;
        };
        std::vector<DecodeWitness> decodeWitness;

        /// THE GPU SKIN CACHE (PHOTON-SKIN-1, SkinCache.h): one entry per RIGGED
        /// traced item, by NodeId — its posed vertex buffer, its row block in the
        /// GPU scene, and ITS OWN bottom-level structure (a skinned item's BLAS
        /// is per ITEM, never shared: two characters on one mesh wear two poses).
        struct Skin {
            /// THE CACHE'S IDENTITY is (Item, Mesh, the node's rig generation):
            /// attachSkinnedMesh re-attaches IN PLACE (detachItem, then createItem),
            /// so a new Item can land at the old one's address — the pointer alone
            /// would keep a cache sized and addressed for a mesh that may be gone.
            Ogre::Item *item = nullptr;
            const Ogre::Mesh *mesh = nullptr;
            unsigned long long rigGeneration = 0ull;
            /// What the cache and its structure were built over, re-checked against
            /// the live VAO on every pass before anything is skinned or refit.
            const Ogre::IndexBufferPacked *indices = nullptr;
            SkinCacheBuffer buf;
            uint32_t rowBlock = 0xFFFFFFFFu;  ///< the GpuScene row block (a mesh-table entry)
            uint32_t row = 0xFFFFFFFFu;       ///< its level-0/submesh-0 row: the override
            unsigned long long poseSerial = 0ull;
            bool skinned = false;             ///< the job has written it at least once
            bool seen = false;                ///< in this pass's traced set
            /// The structure, built once from the cache with ALLOW_UPDATE and
            /// REFIT in place on every pose change after that. Its own scratch,
            /// sized for both, so a refit allocates nothing.
            VkAccelerationStructureKHR as = VK_NULL_HANDLE;
            RawBuffer storage, scratch;
            VkDeviceAddress address = 0;
            bool built = false;
            unsigned triangles = 0;
        };
        std::unordered_map<uint32_t, Skin> skins;
        /// What the instance writer reads for a rigged slot: the skin BLAS and the
        /// row, for the entries READY this pass (built and skinned).
        std::unordered_map<uint32_t, std::pair<VkDeviceAddress, uint32_t>> skinUse;
        /// The job's two inputs, grown by doubling (Ogre UAV buffers: the job is an
        /// HlmsComputeJob) and the VaoManager that made them.
        Ogre::VaoManager *skinVao = nullptr;
        Ogre::UavBufferPacked *skinJobs = nullptr;
        Ogre::UavBufferPacked *skinPalette = nullptr;
        uint32_t skinJobCap = 0, skinPaletteCap = 0;

        /// The gate: nothing moved, no item changed and no instance's RAY LEVEL
        /// changed since the last update, so there is nothing to record. The
        /// caster walk's epoch plus the ray rule's refit count.
        unsigned long long lastEpoch = 0;
        bool haveEpoch = false;
        /// The traced set's shape (count + BLAS identities), so a changed set
        /// forces a rebuild rather than a refit.
        unsigned long long setSignature = 0;

        /// THIS SCENE'S OWN timestamp range in the shared pool, and its own
        /// in-flight record. Both are per SCENE, not per frame: two scenes
        /// drawn in one frame each write timestamps, and a single shared ring
        /// indexed by the frame number would have the second overwrite the
        /// first (audit round 2, finding 3).
        /// This scene's timestamp SLOT (not a running count) and the query
        /// index it resolves to. The slot is held until forgetScene hands it
        /// back — see mTimedSceneSlots.
        unsigned querySlot = 0;
        unsigned queryBase = 0;
        bool     hasQueryBase = false;
        struct PendingTimes {
            unsigned frame = 0; bool blas = false; bool tlas = false; bool live = false;
            bool skin = false;   ///< queries 4..6: the skin dispatch and the skinned builds
        };
        PendingTimes pending[kFramesInFlight];

        RayQueryStatus st;
    };

    SceneAs &asFor(OgreScene *scene) { return mScenes[scene]; }

    // --- plumbing ---------------------------------------------------------
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
    bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                    bool deviceAddress, RawBuffer &out, std::string &err);
    void dropBuffer(RawBuffer &b);
    VkDeviceAddress addressOf(VkBuffer b) const;
    bool makePipeline(std::string &err);

    bool ensureBlas(SceneAs &sa, const std::vector<BlasWant> &wants, VkCommandBuffer cmd,
                    unsigned &built, std::string &err);
    /// True when it actually recorded a compaction copy this frame.
    bool runCompaction(SceneAs &sa, VkCommandBuffer cmd);
    bool buildTlas(SceneAs &sa, VkCommandBuffer cmd, bool refit, std::string &err);
    void readTimestamps(SceneAs &sa);
    /// THE SKIN CACHE's frame (PHOTON-SKIN-1): reconciles the rigged traced set
    /// with `sa.skins`, re-skins every item whose POSE moved (one dispatch for all
    /// of them) and builds or refits their structures — all recorded into `cmd`
    /// before the gather that references them. Timestamps into `qBase` + 4..6
    /// when `timed`. Returns false only when the frame must not trace skinned
    /// items at all (the job is missing); the entries simply stay unready then.
    bool skinPass(OgreScene *scene, SceneAs &sa, VkCommandBuffer &cmd, bool timed, unsigned qBase,
                  std::string &err);
    /// Frees one entry: its structure retired, its buffer destroyed (Ogre's
    /// delayed destruction), its row block handed back and the node's override
    /// cleared. `scene` may be null (the tier's close: the GpuScene dies with it).
    void dropSkin(OgreScene *scene, SceneAs &sa, uint32_t node, SceneAs::Skin &sk);
    void dropSkinBuffers(SceneAs &sa);
    /// The pose serial a cache is keyed on (own + a shared skeleton's master).
    static unsigned long long skinPoseSerial(const OgreScene *scene, const OgreScene::Node &n);

    /// Ogre's frame command buffer, with every encoder closed first: an
    /// acceleration-structure build may not be recorded inside a render pass,
    /// and Ogre tracks the encoder state itself (VulkanQueue::getEncoderState).
    VkCommandBuffer frameCmd();

    Ogre::VulkanRenderSystem *mRs = nullptr;
    Ogre::VulkanDevice *mDev = nullptr;
    VkDevice mVk = VK_NULL_HANDLE;
    RtFuncs mFn;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR mAsProps{};
    float mTimestampPeriod = 1.0f;

    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPipeLayout = VK_NULL_HANDLE;
    VkPipeline mPipeline = VK_NULL_HANDLE;
    VkShaderModule mModule = VK_NULL_HANDLE;
    VkDescriptorPool mDescPool = VK_NULL_HANDLE;

    VkQueryPool mTimestamps = VK_NULL_HANDLE;
    VkQueryPool mCompactSizes = VK_NULL_HANDLE;
    /// WHICH COMPACTION SLOTS ARE OWED A READ — one bit per slot, not a count
    /// (round 3, finding 1). A count plus a cursor could not express "slot 3 is
    /// still pending": the wrap `if (cursor + n > ring) cursor = 0` was
    /// unconditional, so a PARKED scene's pending slots at the bottom of the
    /// ring — a preview panel that drew once and was hidden before its queries
    /// were read — were reset and rewritten by the editor's next batch, and
    /// `runCompaction` then read another BLAS's compacted size and copied into
    /// a buffer sized for a different structure. That is the round-2 corruption
    /// arriving by a second door. The mask answers exactly the question the
    /// allocation has to ask.
    uint64_t mCompactBusy = 0;
    /// A contiguous free run of `n` slots, or -1 when the ring cannot hold one.
    /// Contiguous because the pin's cmdWriteProperties takes ONE first-query
    /// index for the whole batch.
    int takeCompactRun(unsigned n) const;
    /// WHICH TIMESTAMP RANGES ARE TAKEN — one bit per slot, not a count
    /// (round 3, finding 2). Allocating positionally from a count
    /// (`base = count * stride`) is only correct if scenes are destroyed in
    /// exactly the reverse of their creation order, which nothing guarantees:
    /// editor/preview/thumbnail hold slots 0/1/2, the preview closes, the count
    /// drops to 2, and the NEXT thumbnail is handed slot 2 — the live
    /// thumbnail's range. Both then reset, write and read the same queries, and
    /// the per-scene split that fixed round 2's finding 3 is undone by ordinary
    /// panel churn. Diagnostics only (the numbers go wrong, nothing renders
    /// wrong), which is exactly why it would have gone unnoticed.
    uint32_t mTimedSceneSlots = 0;

    /// THE FRAME CLOCK IS OGRE'S, NOT OURS (finding 3). The counter this used to
    /// keep was incremented once per updateScene CALL, and updateRayQuery calls
    /// that once per DRAWN scene — so with the editor plus one preview scene it
    /// ran at twice the frame rate and every "wait N frames in flight" guard
    /// waited half as long as it claimed. A scratch arena could be freed while
    /// the build that reads it was still queued.
    ///
    /// VaoManager::getFrameCount() is the number the PIN retires its own
    /// resources on, and getDynamicBufferMultiplier() is the depth it considers
    /// in flight, so taking both means the tier and the pin agree by
    /// construction rather than by a matching constant.
    uint32_t frameNow() const;
    uint32_t framesInFlight() const;
    /// A structure or buffer that must not be freed until the frames that could
    /// still be reading it have retired.
    struct Retired {
        RawBuffer buf;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        /// R5's storage images take the same road: a resize frees them while the
        /// frames that dispatched against them are still in flight.
        ReflectImage img;
        /// ...and the per-frame UNCACHED image views of the chain's textures.
        VkImageView view = VK_NULL_HANDLE;
        /// ...and the reflection pass' descriptor sets: `vkFreeDescriptorSets`
        /// on a set a submitted command buffer still refers to is illegal, and
        /// `flushCommands` submits without waiting, so a workspace rebuild can
        /// reach here with the set still in flight (measured:
        /// VUID-vkFreeDescriptorSets-pDescriptorSets-00309).
        VkDescriptorSet set = VK_NULL_HANDLE;
        /// ...and the pool it came from. Freeing a set into another pool is
        /// invalid, and GATHER-0 brought a second one (audit: the old line
        /// hard-coded mReflectPool).
        VkDescriptorPool setPool = VK_NULL_HANDLE;
        /// GATHER-0: an Ogre-owned texture (the gather's full-resolution
        /// irradiance target) whose descriptor sets may still be in flight.
        Ogre::TextureGpu *tex = nullptr;
        uint32_t frame = 0;
    };
    std::vector<Retired> mRetireBin;
    void retire(RawBuffer &b);
    void retire(VkAccelerationStructureKHR &as, RawBuffer &storage);
    void retireImage(ReflectImage &img);
    void retireView(VkImageView v);
    void retireSet(VkDescriptorSet set);
    void retireSet(VkDescriptorSet set, VkDescriptorPool pool);
    /// GATHER-0: an Ogre texture destroyed once the frames that could still be
    /// sampling it have retired (the same bin as everything else here).
    void retireTexture(Ogre::TextureGpu *&t);
    /// THE BLACK STAND-INS. Every descriptor in a set must be a real view,
    /// whether the shader reads it or not, and two of this pass' inputs can
    /// legitimately be absent: a scene with no captured environment cube (no
    /// sky, no IBL) and a scene whose voxel arm is not built. Rather than
    /// decline the whole trace there — which would mean no reflections at all in
    /// exactly the scene where an escaping ray is the WHOLE answer — the empty
    /// slots take a 1x1 black image of the right TYPE. They are made once,
    /// cleared once, and left in SHADER_READ_ONLY_OPTIMAL for the process' life.
    bool ensureDummyImages(std::string &err);
    void clearDummyImages(VkCommandBuffer cmd);
    /// ...and the card read's (PHOTON-CARDS-2): a scene without a surface cache
    /// binds a 1x1 black 2D image for the two atlas layers and a 256-byte
    /// storage buffer for the two tables, with ZERO instance slots in the
    /// parameters, so the read declines every hit without touching them.
    ReflectImage mDummyCube, mDummyVolume, mDummyFlat;
    RawBuffer mDummyStorage;
    bool mDummiesReady = false;
    bool mDummiesNeedClear = false;
    bool makeStorageImage(unsigned w, unsigned h, VkFormat fmt, ReflectImage &out,
                          std::string &err);
    void drainRetired();
    void evictStaleBlas(SceneAs &sa);

    std::unordered_map<OgreScene *, SceneAs> mScenes;

    // ---- R5: RAY-TRACED REFLECTIONS ------------------------------------
    // (PHOTON_SPEC §7 R5.) One compute dispatch per drawn view per frame,
    // recorded into the same frame command buffer as everything else here,
    // between the SSR resolve and the scene pass that samples its output. The
    // structures it traces are the ones above; it adds no build of its own, so
    // the no-hitch law is satisfied by construction.
public:
    /// Records this frame's trace for one view. Silently does nothing when the
    /// scene has no usable structure, when the chain does not carry the SSR
    /// textures, or when the pipeline could not be made — in every one of those
    /// cases `jahSsrReflection` keeps exactly what the resolve wrote, which is
    /// exactly today's picture.
    void recordReflect(const ReflectPassListener *key, OgreView *view,
                       Ogre::CompositorPass *pass, const HitListBinding &hit);
    /// THE REFLECTION'S SECOND HALF (PHOTON-HIT-SHADE-1): the spatial filter and
    /// the composite, in front of the opaque pass — after the hit write-back has
    /// completed the temporal mean of every texel whose ray the decode shaded.
    void finishReflect(const ReflectPassListener *key);
    /// Frees a view's reflection resources. Called from ~ReflectPassListener.
    void forgetReflect(const ReflectPassListener *key);
    /// How many rays the last recorded trace dispatched, and the GPU
    /// milliseconds it cost (read back several frames later, -1 until measured)
    /// — for GiStatus::rayQuery. Per SCENE, because that is what giStatus asks.
    void reflectStatsInto(const OgreScene *scene, RayQueryStatus &st) const;

private:
    /// ONE EYE'S IMAGE as the shader's five vec4s (rq_reflect.comp's EyeImage):
    /// where the camera is (w = 1 perspective, 0 orthographic) and the world
    /// basis of its 0..1 image. A MONO view has one; a stereo view has two, each
    /// spanning its own HALF of the target.
    struct EyeBasisF {
        float camPos[4] = { 0, 0, 0, 1 };
        float rayTL[4] = { 0, 0, 0, 0 };
        float rayRight[4] = { 0, 0, 0, 0 };
        float rayDown[4] = { 0, 0, 0, 0 };
        float fwd[4] = { 0, 0, 0, 0 };
    };

    struct ReflectView {
        /// The descriptor ring. A set that is bound by a command buffer still in
        /// flight may not be rewritten, and every input of this pass can be
        /// recreated behind our back (a workspace rebuild replaces every texture
        /// in the chain; a GI re-solve replaces every voxel texture), so the set
        /// is rewritten EVERY frame and there is one per frame in flight.
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        /// THE PER-SLOT GEOMETRY ROW the scene's TLAS was written with, copied
        /// per frame in flight (the scene's vector moves under a later frame).
        RawBuffer       geomRowOfSlot[kReflectRing];
        /// The temporal mean and the distance beside it, ping-ponged: read from
        /// [frame & 1], written to [~frame & 1]. See rq_reflect.comp's note on
        /// why one buffer is a race.
        ReflectImage hist[2], dist[2];
        unsigned w = 0, h = 0;
        unsigned frame = 0;              ///< the sample sequence's input
        bool     imagesReady = false;
        /// The pair has been made but not yet zeroed (the clear needs a command
        /// buffer, which is taken later than the creation).
        bool     needsClear = false;
        /// The PREVIOUS frame's camera basis, in the five numbers the shader
        /// reconstructs with — ONE PER EYE since lane REFLECT-VR-1 (index 0 is
        /// the only one a mono view uses, and is that view's camera).
        /// `havePrev` false means "no history is valid", which is what a first
        /// frame, a resize and a scene change all are.
        EyeBasisF prev[2];
        bool  havePrev = false;
        /// HOW MANY CONSECUTIVE FRAMES THIS HISTORY HAS BEEN WRITTEN — the
        /// VIEW's age, which is a different fact from a texel's sample count
        /// (PAN-SMEAR-1). A pixel the camera has just turned onto has no
        /// previous position on screen, and the shader must tell that apart
        /// from a view that is two frames old: the second is what the filter's
        /// warm-up exists for, the first is an ordinary frame of an ordinary
        /// pan. Restarts with `havePrev`.
        unsigned historyFrames = 0;
        /// Was the LAST recorded frame a stereo one? A view that changes shape
        /// (a session beginning or ending on it) has a history whose halves mean
        /// something else, so the mean starts again — the same rule as a resize.
        bool  prevStereo = false;
        /// What the last recorded dispatch covered, for the status readings.
        OgreScene *scene = nullptr;
        unsigned   rays = 0;
        /// This view's own timestamp pair (the same ring discipline as the
        /// structures': a slot held until the view goes away, read with the
        /// availability bit and never with a wait).
        unsigned querySlot = 0;
        unsigned queryBase = 0;
        bool     hasQueryBase = false;
        struct Pending { unsigned frame = 0; bool live = false; };
        Pending  pending[kFramesInFlight];
        float    gpuMs = -1.0f;
        /// THE FRAME BETWEEN ITS TWO HALVES (trace -> finishReflect): the ring slot
        /// the trace bound, its grid, and which of the pair is this frame's mean.
        bool     finishPending = false;
        unsigned finishRing = 0, traceW = 0, traceH = 0, curIdx = 0;
    };

    bool makeReflectPipeline(std::string &err);
    /// The point and linear samplers every job of the tier binds (made once).
    bool ensureSamplers(std::string &err);
    bool ensureReflectImages(ReflectView &rv, unsigned w, unsigned h, std::string &err);
    /// Records the UNDEFINED -> GENERAL transition and the zero clear for a pair
    /// that was just made. Separate from the creation because the descriptor set
    /// needs the image VIEWS before the command buffer may be taken (see the
    /// note on the transitions in recordReflect).
    void clearReflectImages(ReflectView &rv, VkCommandBuffer cmd);
    void dropReflect(ReflectView &rv);
    void readReflectTimestamps(ReflectView &rv);

    std::unordered_map<const ReflectPassListener *, ReflectView> mReflects;
    VkDescriptorSetLayout mReflectSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mReflectPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mReflectPipeline = VK_NULL_HANDLE;
    VkShaderModule        mReflectModule = VK_NULL_HANDLE;
    /// THE SPATIAL FILTER + COMPOSITE (round C). A second pipeline on the
    /// SAME layout and the same descriptor set: it reads the temporal mean
    /// the first dispatch wrote, blurs it by the pixel's own lobe (guided by
    /// depth and normal) and writes `jahSsrReflection`. A pixel cannot blur
    /// with neighbours its own dispatch has not finished writing, which is why
    /// it is a second dispatch and not more code in the first.
    VkPipeline            mFilterPipeline = VK_NULL_HANDLE;
    VkShaderModule        mFilterModule = VK_NULL_HANDLE;
    VkDescriptorPool      mReflectPool = VK_NULL_HANDLE;
    /// gi.card_read_parity's harness (cardPickBlocking), made on first use.
    VkDescriptorSetLayout mCardParitySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCardParityPipeLayout = VK_NULL_HANDLE;
    VkShaderModule        mCardParityModule = VK_NULL_HANDLE;
    VkPipeline            mCardParityPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      mCardParityPool = VK_NULL_HANDLE;
    /// Its own point sampler: the reflection's are made with the reflection
    /// pipeline, which a scene without an SSR chain never builds.
    VkSampler             mCardParitySampler = VK_NULL_HANDLE;
    VkSampler             mPointSampler = VK_NULL_HANDLE;
    VkSampler             mLinearSampler = VK_NULL_HANDLE;
    /// The pipeline could not be made on this device; say so ONCE and take the
    /// fallback picture for the rest of the process.
    bool                  mReflectFailed = false;
    /// Timestamp slots for reflect passes, the same bitmask discipline as
    /// mTimedSceneSlots (and for the same round-3 reason).
    uint32_t              mReflectQuerySlots = 0;
    VkQueryPool           mReflectTimestamps = VK_NULL_HANDLE;

    // ---- THE SCREEN-PROBE GATHER (GATHER-1a) ---------------------------
    // Three more compute dispatches on the SAME frame command buffer and the
    // SAME hook as the reflection trace — after the SSR prepass, before the
    // opaque pass that shades. The ALGORITHM is a Component of ours
    // (`ScreenProbeGather`, OgreScreenProbeGather.cpp); what lives here is
    // what only this file can do: read the scene it is friends with, and lend
    // the Component the device services below.
public:
    /// Records this frame's placement, trace and integrate for one view.
    /// Silently does nothing unless the view's scene has the row on and this
    /// machine traces.
    void recordGather(const ReflectPassListener *key, OgreView *view,
                      Ogre::CompositorPass *pass, const HitListBinding &hit);
    /// Frees a view's gather resources (from ~ReflectPassListener, and when
    /// the row goes off).
    void forgetGather(const ReflectPassListener *key);
    /// Takes this listener's shader registration away the moment its pass
    /// ends — the gather's binding is PASS-scoped (GATHER-0's D2).
    void releaseGatherBinding(const ReflectPassListener *key);
    /// The last measured numbers for a scene, for `GiStatus::gather`.
    void gatherStatsInto(const OgreScene *scene, GatherStatus &out) const;

    // ---- GatherHost: the services the Component borrows -----------------
    VkDevice gatherDevice() const override { return mVk; }
    Ogre::RenderSystem *gatherRenderSystem() const override { return mRs; }
    uint32_t gatherFrameNow() const override { return frameNow(); }
    uint32_t gatherFramesInFlight() const override { return framesInFlight(); }
    VkCommandBuffer gatherFrameCmd() override { return frameCmd(); }
    bool gatherMakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                          VkBuffer &buffer, VkDeviceMemory &memory, void **mapped,
                          std::string &err) override {
        RawBuffer b;
        if (!makeBuffer(size, usage, hostVisible, false, b, err)) return false;
        buffer = b.buffer;
        memory = b.memory;
        if (mapped) *mapped = b.mapped;
        return true;
    }
    bool gatherMakeImage(unsigned w, unsigned h, VkFormat fmt, VkImage &image,
                         VkDeviceMemory &memory, VkImageView &view, std::string &err) override {
        ReflectImage img;
        if (!makeStorageImage(w, h, fmt, img, err)) return false;
        image = img.image;
        memory = img.memory;
        view = img.view;
        return true;
    }
    void gatherRetireBuffer(VkBuffer buffer, VkDeviceMemory memory) override {
        RawBuffer b;
        b.buffer = buffer;
        b.memory = memory;
        retire(b);
    }
    void gatherRetireImage(VkImage image, VkDeviceMemory memory, VkImageView view) override {
        ReflectImage img;
        img.image = image;
        img.memory = memory;
        img.view = view;
        retireImage(img);
    }
    void gatherRetireView(VkImageView view) override { retireView(view); }
    void gatherRetireSet(VkDescriptorSet set, VkDescriptorPool pool) override {
        retireSet(set, pool);
    }
    void gatherRetireTexture(Ogre::TextureGpu *texture) override { retireTexture(texture); }
    bool gatherDummies(VkImageView &cube, VkImageView &volume, VkImageView &flat, VkBuffer &storage,
                       std::string &err) override {
        if (!ensureDummyImages(err) || !ensureSamplers(err)) return false;
        cube = mDummyCube.view;
        volume = mDummyVolume.view;
        flat = mDummyFlat.view;
        storage = mDummyStorage.buffer;
        return cube && volume && flat && storage;
    }
    /// ...AND THE STAND-INS MUST BE CLEARED BY WHOEVER BINDS THEM FIRST (the
    /// lead's read). `clearDummyImages` used to have ONE caller — the reflection
    /// trace — which returns early when the SSR row is off, so a GATHER-ONLY
    /// chain bound a 1x1 cube and volume that had never left UNDEFINED as
    /// SHADER_READ_ONLY. `gi.gather_reference` runs exactly that configuration
    /// (`ssr = 0`). It is a no-op after the first frame that calls it, and it is
    /// safe to call twice in one frame — the flag is cleared by the first.
    void gatherClearDummies(VkCommandBuffer cmd) override { clearDummyImages(cmd); }
    VkSampler gatherPointSampler() const override { return mPointSampler; }
    VkSampler gatherLinearSampler() const override { return mLinearSampler; }

private:
    /// Made on the first frame a scene gathers, destroyed by close().
    ScreenProbeGather *mGather = nullptr;

    // ---- HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1, RY-R3) ----------------
    // One more compute dispatch on the same frame command buffer and the same
    // hook as the reflection trace and the gather — after the prepass, before
    // the PrePassUse pass that shades with its answer. One ray per texel from
    // the prepass' surface towards the sun (rq_sun_contact.comp); the answer is
    // an Ogre R8 texture the PBS pass reads through the Hlms listener's fourth
    // extra slot (OgreFog.cpp kExtraPassSlots, JahSunContact_piece_ps.any).
public:
    /// Records this frame's contact rays for one view, and registers the
    /// texture with the listener for exactly the pass it runs in front of.
    /// Silently does nothing unless the view's scene resolves the row on.
    void recordSunContact(const ReflectPassListener *key, OgreView *view,
                          Ogre::CompositorPass *pass);
    /// ...and its second half (PHOTON-HIT-SHADE-1): the texture's transition to
    /// the pixel stage and the pass-scoped registration, in front of the opaque
    /// pass that reads it (the rays themselves are traced in front of the hit
    /// decode pass, with the other ray jobs).
    void finishSunContact(const ReflectPassListener *key);
    /// Takes the pass-scoped registration away as that pass ends.
    void releaseSunContactBinding(const ReflectPassListener *key);
    /// Frees a view's contact state (from ~ReflectPassListener, a workspace
    /// rebuild, and the row going off).
    void forgetSunContact(const ReflectPassListener *key);
    /// The last frame's numbers for a scene (`Scene::sunContactStatus`).
    void sunContactStatsInto(const OgreScene *scene, SunContactStatus &st) const;

private:
    struct SunContactView {
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        /// The visibility texture the pixel reads (an Ogre texture: it reaches
        /// the PBS pass through the listener, so it must be one the Hlms can
        /// bind), at the job's own resolution.
        Ogre::TextureGpu *vis = nullptr;
        unsigned w = 0, h = 0, fullW = 0, fullH = 0, divisor = 1;
        unsigned frame = 0;
        const Ogre::SceneManager *sceneMgr = nullptr;
        /// What the last record did, for the status.
        OgreScene *scene = nullptr;
        bool      ran = false;
        unsigned long long rays = 0;
        float     range = 0.0f;
        float     toSun[3] = { 0.0f, 0.0f, 0.0f };
        float     cpuMs = -1.0f;
        std::string reason;
        unsigned querySlot = 0;
        unsigned queryBase = 0;
        bool     hasQueryBase = false;
        struct Pending { unsigned frame = 0; bool live = false; };
        Pending  pending[kFramesInFlight];
        float    gpuMs = -1.0f;
        /// The rays were traced this frame; finishSunContact registers them.
        bool     finishPending = false;
    };
    bool makeSunContactPipeline(std::string &err);
    void dropSunContact(SunContactView &sv);
    void readSunContactTimestamps(SunContactView &sv);
    std::unordered_map<const ReflectPassListener *, SunContactView> mSunContacts;
    VkDescriptorSetLayout mSunSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mSunPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mSunPipeline = VK_NULL_HANDLE;
    VkShaderModule        mSunModule = VK_NULL_HANDLE;
    VkDescriptorPool      mSunPool = VK_NULL_HANDLE;
    /// Its own point sampler: the reflection's are made with the reflection
    /// pipeline, which a view with the SSR row off never builds.
    VkSampler             mSunSampler = VK_NULL_HANDLE;
    VkQueryPool           mSunTimestamps = VK_NULL_HANDLE;
    uint32_t              mSunQuerySlots = 0;
    /// The pipeline (or the R8 storage format) is not available on this device:
    /// said ONCE, and the shadow map renders alone for the rest of the process.
    bool                  mSunFailed = false;
    std::string           mSunFailReason;

public:
    // ---- THE HIT DECODE (PHOTON-HIT-SHADE-1; SPECS/atom/D2_HIT_SHADING_DESIGN.md)
    // A ray hit no cache can shade (a mover, a rigged item, a static hit neither
    // its card nor a cascade answers, the gather's far copies) is appended to the
    // chain's HIT LIST by the trace; the chain's "Jahshaka hit decode" pass draws
    // HlmsAtom's decode over the list (one fragment per record); the write-back
    // (rq_hit_composite.comp) scatters the radiance into the reflection's mean and
    // the gather's atlas. The ray jobs' recording is split around the decode pass.
    /// In front of the hit decode pass: the list reset, the TRACES (the sun
    /// contact, the reflection, the gather) appending to it, HlmsAtom armed.
    void beginHitDecode(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass);
    /// After it: HlmsAtom disarmed, the decode draws hidden, the camera's aspect
    /// handed back.
    void endHitDecode(const ReflectPassListener *key);
    /// In front of the opaque pass: the write-back, then every job's second half
    /// (the filters, the SH and integrate, the registrations). A chain that ran no
    /// decode pass this frame traces here first, with no list bound.
    void finishRayJobs(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass);
    void forgetHits(const ReflectPassListener *key);
    /// The hit list's counters (read back several frames late) for a scene.
    void hitStatsInto(const OgreScene *scene, RayQueryStatus &st) const;

private:
    struct HitView {
        /// The list's buffer: [0] records appended (may pass the capacity), [1]
        /// records dropped, then two words per record (the sun, the footprint, the
        /// weight — jah_rq_hit_record.glsl). An Ogre UAV buffer: HlmsAtom reads it
        /// through a read-only view (a buffer, not an image: the decode's pixel
        /// shader's pass textures already reach the pin's table's end —
        /// kHitBufSlot, HlmsAtom.h).
        Ogre::UavBufferPacked *buf = nullptr;
        /// This frame's list, the chain's textures (the names OgreChain.cpp gives).
        Ogre::TextureGpu *ids = nullptr, *dest = nullptr, *radiance = nullptr;
        uint32_t capacity = 0u, width = 0u;
        /// The list was bound for THIS frame's traces (a write-back is owed).
        bool live = false;
        /// Ogre's frame number of the last decode pass this key ran in front of.
        uint32_t decodeFrame = 0xFFFFFFFFu;
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer params[kReflectRing];
        /// The buffer's two counter words, copied per frame into a host ring and read once
        /// the frame retired (never a wait).
        RawBuffer readback;
        struct Pending { uint32_t frame = 0; bool live = false; };
        Pending pending[kFramesInFlight];
        unsigned frame = 0;
        OgreScene *scene = nullptr;
        unsigned long long appended = 0ull, dropped = 0ull;
        /// Armed for the decode pass: the SceneManager whose draws are shown, and
        /// the camera whose auto aspect is held off for the pass (the decode's
        /// target is the list, not a picture — F7).
        Ogre::SceneManager *armedSm = nullptr;
        Ogre::Camera *pinnedCam = nullptr;
        bool camAuto = false;
    };
    std::unordered_map<const ReflectPassListener *, HitView> mHits;
    bool prepareHitList(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass,
                        HitListBinding &out);
    /// A decode twin of `scene` no longer matches its PBS datablock (a witness's
    /// Hlms hash or datablock moved since the last sync) — read only.
    bool decodeTwinsStale(OgreScene *scene) const;
    /// The binding a trace takes when no list is bound: the stand-ins, `on` false.
    void hitStandIns(HitListBinding &out);
    void recordHitComposite(const ReflectPassListener *key);
    bool makeCompositePipeline(std::string &err);
    bool ensureHitDummies(std::string &err);
    void initHitDummies(VkCommandBuffer cmd);
    void readHitCounters(HitView &hv);
    VkDescriptorSetLayout mCompSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCompPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mCompPipeline = VK_NULL_HANDLE;
    VkShaderModule        mCompModule = VK_NULL_HANDLE;
    VkDescriptorPool      mCompPool = VK_NULL_HANDLE;
    bool                  mCompFailed = false;
    /// 1x1 storage stand-ins in GENERAL (a set's every view must be real): the
    /// list's two formats, the RGBA16F of the reflection's mean and the gather's
    /// atlas, and the reflection's distance format.
    ReflectImage mHitDummyIds, mHitDummyColour, mHitDummyDest, mHitDummyDist;
    bool mHitDummiesReady = false;
    bool mHitDummiesNeedInit = false;
    // ---- THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4) ----------------
    // One dispatch per scene per frame a mover moved, recorded from the
    // surface cache's workspacePosUpdate — after the capture's copies (it reads
    // the new Depth and Normal) and before the relight that multiplies its
    // answer in (rq_card_movers.comp). The TLAS it traces was built this frame
    // (updateScene, before any render target).
public:
    bool traceCardMovers(OgreScene *scene, const CardMoverTrace &job);
    /// A timestamp pair around the scene's relight dispatch, and the read-back.
    void timeCardRelight(OgreScene *scene, bool begin);
    void cardMoverTimes(OgreScene *scene, float &traceMs, float &relightMs);
    void forgetCardMovers(OgreScene *scene);

private:
    struct CardMoverView {
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        RawBuffer       records[kReflectRing];
        unsigned frame = 0;
        unsigned querySlot = 0, queryBase = 0;
        bool     hasQueryBase = false;
        struct Pending { uint32_t frame = 0; bool trace = false; bool relight = false; };
        Pending  pending[kFramesInFlight];
        float    traceMs = -1.0f, relightMs = -1.0f;
    };
    bool makeCardMoverPipeline(std::string &err);
    bool cardMoverQueries(CardMoverView &cv);
    void readCardMoverTimestamps(CardMoverView &cv);
    std::unordered_map<const OgreScene *, CardMoverView> mCardMovers;
    VkDescriptorSetLayout mCmSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCmPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mCmPipeline = VK_NULL_HANDLE;
    VkShaderModule        mCmModule = VK_NULL_HANDLE;
    VkDescriptorPool      mCmPool = VK_NULL_HANDLE;
    VkQueryPool           mCmTimestamps = VK_NULL_HANDLE;
    uint32_t              mCmQuerySlots = 0;
    bool                  mCmFailed = false;

    /// ONE IMAGE'S BASIS from a pose and a frustum (rq_reflect.comp's five
    /// numbers) — the arithmetic recordReflect's eyes are built with, shared so
    /// the contact rays and the reflection rays cannot drift apart.
    static EyeBasisF eyeBasis(bool ortho, const Ogre::Vector3 &pos, const Ogre::Quaternion &rot,
                              float el, float er, float et, float eb);
    /// THE LETTERBOX (SSR-LETTERBOX-1's ray half): a constrained-aspect camera
    /// draws the target's INNER rectangle while a compute pass addresses the
    /// whole target, so the basis is EXPANDED to the target on the CPU.
    static void expandEyeToTarget(EyeBasisF &e, const ChainDesc &cd, unsigned fullW,
                                  unsigned fullH);
};

// ---------------------------------------------------------------------------
uint32_t RayQueryTier::memoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    const VkPhysicalDeviceMemoryProperties &mp = mDev->mDeviceMemoryProperties;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return uint32_t(-1);
}

bool RayQueryTier::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                              bool deviceAddress, RawBuffer &out, std::string &err) {
    if (size == 0) size = 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage | (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(mVk, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
        err = "rayquery: vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(mVk, out.buffer, &req);
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = deviceAddress ? &flags : nullptr;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(
        req.memoryTypeBits,
        hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == uint32_t(-1)) {
        err = "rayquery: no suitable memory type";
        vkDestroyBuffer(mVk, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkAllocateMemory(mVk, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        err = "rayquery: vkAllocateMemory failed (" + std::to_string(size_t(req.size)) + " bytes)";
        vkDestroyBuffer(mVk, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(mVk, out.buffer, out.memory, 0);
    out.size = req.size;
    if (hostVisible) vkMapMemory(mVk, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    return true;
}

void RayQueryTier::dropBuffer(RawBuffer &b) {
    if (b.mapped) { vkUnmapMemory(mVk, b.memory); b.mapped = nullptr; }
    if (b.buffer) vkDestroyBuffer(mVk, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(mVk, b.memory, nullptr);
    b = RawBuffer();
}

VkDeviceAddress RayQueryTier::addressOf(VkBuffer b) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    return mFn.getBufferDeviceAddress(mVk, &info);
}

uint32_t RayQueryTier::frameNow() const {
    // Ogre's own frame counter — the one it retires its dynamic buffers on.
    return mDev && mDev->mVaoManager ? mDev->mVaoManager->getFrameCount() : 0u;
}

uint32_t RayQueryTier::framesInFlight() const {
    const uint32_t n = mDev && mDev->mVaoManager
                           ? uint32_t(mDev->mVaoManager->getDynamicBufferMultiplier())
                           : kFramesInFlight;
    return n ? n : kFramesInFlight;
}

/// RETIRE, NEVER FREE IN PLACE. Anything this frame recorded a reference to —
/// a scratch arena a queued build reads, a structure a queued trace reads, the
/// instance array a queued TLAS build reads — is released only once Ogre's own
/// frame counter has moved past the depth it keeps in flight. This is what
/// replaces the vkDeviceWaitIdle calls (finding 6): the GPU is never drained,
/// the memory is simply handed back later.
void RayQueryTier::retire(RawBuffer &b) {
    if (!b.buffer && !b.memory) return;
    Retired r;
    r.buf = b;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    b = RawBuffer();
}

void RayQueryTier::retire(VkAccelerationStructureKHR &as, RawBuffer &storage) {
    Retired r;
    r.as = as;
    r.buf = storage;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    as = VK_NULL_HANDLE;
    storage = RawBuffer();
}

/// EVICT A BLAS NOTHING POINTS AT ANY MORE (finding 5). A bottom-level
/// structure is memory AND a held MeshPtr, and the hold is what keeps a mesh's
/// vertex buffers alive under a structure built over them. Keeping one for a
/// mesh no instance has referenced for a long while pins geometry the scene has
/// finished with — and it compounds the compaction ring, because every stale
/// entry is a slot the ring may still be waiting on.
///
/// The window is generous on purpose: a mesh that flickers in and out of the
/// traced set (an object hidden and shown) must not pay a rebuild each time.
void RayQueryTier::evictStaleBlas(SceneAs &sa) {
    static constexpr uint32_t kEvictAfterFrames = 600u;      // ~10 s at 60 Hz
    const uint32_t now = frameNow();
    for (size_t i = 0; i < sa.blas.size(); ++i) {
        Blas &bl = sa.blas[i];
        if (!bl.as || bl.compactState == 1u) continue;        // a size query is still owed
        if (bl.lastSeen == 0u || uint32_t(now - bl.lastSeen) < kEvictAfterFrames) continue;
        // The slot stays (indices are referenced by blasOf and by the
        // instance patches); only its contents go, and the map entry with them,
        // so the next sighting rebuilds into a fresh slot.
        for (auto it = sa.blasOf.begin(); it != sa.blasOf.end();) {
            if (it->second == i) it = sa.blasOf.erase(it); else ++it;
        }
        retire(bl.as, bl.storage);
        bl.mesh.reset();
        bl.triangles = 0;
        bl.address = 0;
        bl.compactState = 0;
        bl.lastSeen = 0;
        sa.setSignature = 0;      // the set changed: the next TLAS is a rebuild
    }
}

/// The lowest contiguous run of `n` FREE compaction slots, or -1. Contiguous
/// because `vkCmdWriteAccelerationStructuresPropertiesKHR` takes one
/// firstQuery for the whole batch; lowest so the ring packs down rather than
/// drifting upward past parked scenes' reservations.
int RayQueryTier::takeCompactRun(unsigned n) const {
    if (!n || n > kCompactRing) return -1;
    unsigned run = 0;
    for (unsigned i = 0; i < kCompactRing; ++i) {
        if (mCompactBusy & (uint64_t(1) << i)) {
            run = 0;
            continue;
        }
        if (++run == n) return int(i + 1 - n);
    }
    return -1;
}

void RayQueryTier::retireView(VkImageView v) {
    if (!v) return;
    Retired r;
    r.view = v;
    r.frame = frameNow();
    mRetireBin.push_back(r);
}

void RayQueryTier::retireSet(VkDescriptorSet set) { retireSet(set, mReflectPool); }

void RayQueryTier::retireSet(VkDescriptorSet set, VkDescriptorPool pool) {
    if (!set) return;
    Retired r;
    r.set = set;
    r.setPool = pool;
    r.frame = frameNow();
    mRetireBin.push_back(r);
}

/// GATHER-0. An Ogre texture whose descriptor sets may still be in flight; the
/// destroy is the manager's, the WAIT is this bin's.
void RayQueryTier::retireTexture(Ogre::TextureGpu *&t) {
    if (!t) return;
    Retired r;
    r.tex = t;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    t = nullptr;
}

void RayQueryTier::retireImage(ReflectImage &img) {
    if (!img.image && !img.memory && !img.view) return;
    Retired r;
    r.img = img;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    img = ReflectImage();
}

bool RayQueryTier::ensureDummyImages(std::string &err) {
    if (mDummiesReady) return true;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    struct Spec { ReflectImage *img = nullptr; VkImageType type = VK_IMAGE_TYPE_2D;
                  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D; uint32_t layers = 0;
                  VkImageCreateFlags flags = 0; };
    const Spec specs[3] = {
        { &mDummyCube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6u,
          VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT },
        { &mDummyVolume, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1u, 0u },
        { &mDummyFlat, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1u, 0u },
    };
    for (const Spec &sp : specs) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.flags = sp.flags;
        ici.imageType = sp.type;
        ici.format = fmt;
        ici.extent = { 1u, 1u, 1u };
        ici.mipLevels = 1;
        ici.arrayLayers = sp.layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(mVk, &ici, nullptr, &sp.img->image) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateImage (stand-in) failed";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(mVk, sp.img->image, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == uint32_t(-1) ||
            vkAllocateMemory(mVk, &mai, nullptr, &sp.img->memory) != VK_SUCCESS) {
            err = "rayquery/reflect: no memory for the stand-in image";
            return false;
        }
        vkBindImageMemory(mVk, sp.img->image, sp.img->memory, 0);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = sp.img->image;
        vci.viewType = sp.viewType;
        vci.format = fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = sp.layers;
        if (vkCreateImageView(mVk, &vci, nullptr, &sp.img->view) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateImageView (stand-in) failed";
            return false;
        }
    }
    if (!mDummyStorage.buffer &&
        !makeBuffer(256u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, mDummyStorage, err))
        return false;
    std::memset(mDummyStorage.mapped, 0, 256u);
    mDummiesReady = true;
    mDummiesNeedClear = true;
    return true;
}

void RayQueryTier::clearDummyImages(VkCommandBuffer cmd) {
    if (!mDummiesNeedClear) return;
    mDummiesNeedClear = false;
    ReflectImage *imgs[3] = { &mDummyCube, &mDummyVolume, &mDummyFlat };
    const uint32_t layers[3] = { 6u, 1u, 1u };
    for (int i = 0; i < 3; ++i) {
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = layers[i];
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[i]->image;
        b.subresourceRange = range;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue zero{};
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
    }
}

bool RayQueryTier::makeStorageImage(unsigned w, unsigned h, VkFormat fmt, ReflectImage &out,
                                    std::string &err) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, h, 1u };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(mVk, &ici, nullptr, &out.image) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(mVk, out.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == uint32_t(-1) ||
        vkAllocateMemory(mVk, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyImage(mVk, out.image, nullptr);
        out = ReflectImage();
        err = "rayquery/reflect: no device-local memory for the temporal mean";
        return false;
    }
    vkBindImageMemory(mVk, out.image, out.memory, 0);
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(mVk, &vci, nullptr, &out.view) != VK_SUCCESS) {
        vkFreeMemory(mVk, out.memory, nullptr);
        vkDestroyImage(mVk, out.image, nullptr);
        out = ReflectImage();
        err = "rayquery/reflect: vkCreateImageView failed";
        return false;
    }
    return true;
}

void RayQueryTier::drainRetired() {
    const uint32_t now = frameNow(), keep = framesInFlight() + 1u;
    for (size_t i = 0; i < mRetireBin.size();) {
        // Unsigned subtraction: the pin's counter wraps, and "now - then" is
        // the elapsed count either way as long as the gap is small.
        if (uint32_t(now - mRetireBin[i].frame) >= keep) {
            if (mRetireBin[i].as) mFn.destroyAccelerationStructure(mVk, mRetireBin[i].as, nullptr);
            dropBuffer(mRetireBin[i].buf);
            if (mRetireBin[i].set && mRetireBin[i].setPool)
                vkFreeDescriptorSets(mVk, mRetireBin[i].setPool, 1, &mRetireBin[i].set);
            if (mRetireBin[i].tex && mRs && mRs->getTextureGpuManager())
                mRs->getTextureGpuManager()->destroyTexture(mRetireBin[i].tex);
            if (mRetireBin[i].view) vkDestroyImageView(mVk, mRetireBin[i].view, nullptr);
            if (mRetireBin[i].img.view) vkDestroyImageView(mVk, mRetireBin[i].img.view, nullptr);
            if (mRetireBin[i].img.image) vkDestroyImage(mVk, mRetireBin[i].img.image, nullptr);
            if (mRetireBin[i].img.memory) vkFreeMemory(mVk, mRetireBin[i].img.memory, nullptr);
            mRetireBin.erase(mRetireBin.begin() + long(i));
        } else {
            ++i;
        }
    }
}

VkCommandBuffer RayQueryTier::frameCmd() {
    // OUTSIDE ANY ENCODER. Ogre tracks whether it is inside a render, compute
    // or copy encoder; vkCmdBuildAccelerationStructuresKHR may not be recorded
    // inside a render pass, and a copy encoder left open would make the
    // barriers below meaningless.
    mDev->mGraphicsQueue.endAllEncoders();
    // The pin's own public accessor — usable from outside the plugin since
    // ogre-patch 0040 exported Ogre::onVulkanFailure, which its device-lost
    // branch calls and which the Vulkan render system did not export (so this
    // line compiled and failed to LINK).
    //
    // IT NEVER RETURNS NULL: on a lost device the accessor's checkVkResult
    // THROWS (the pin's onVulkanFailure raises, OgreVulkanRenderSystem.cpp).
    // The null guard at the call site is therefore belt-and-braces, not the
    // device-lost path it once claimed to be — the honest device-lost question
    // is the render system's own reason string, asked before we record.
    return mDev->mGraphicsQueue.getCurrentCmdBuffer();
}

// ---------------------------------------------------------------------------
bool RayQueryTier::open(Ogre::RenderSystem *rs, std::string &err) {
    mRs = dynamic_cast<Ogre::VulkanRenderSystem *>(rs);
    if (!mRs) {
        err = "rayquery: this engine is not running on the Vulkan render system";
        return false;
    }
    mDev = mRs->getVulkanDevice();
    if (mDev && mDev->mPhysicalDevice) {
        // SPIR-V 1.4 NEEDS A 1.2 DEVICE (audit C-7). Our module is compiled at
        // --target-env spirv1.4 and is only legal on a device whose own
        // apiVersion is at least 1.2; the extension check below does not imply
        // it. Refusing here is a supported no-rays boot, not a failure.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mDev->mPhysicalDevice, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) {
            err = "rayquery: the device reports Vulkan " +
                  std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
                  std::to_string(VK_VERSION_MINOR(props.apiVersion)) +
                  ", and the tier's SPIR-V 1.4 module needs 1.2";
            return false;
        }
    }
    if (!mDev || !mDev->hasRayQuery()) {
        err = "rayquery: the device has no VK_KHR_ray_query (ogre-patch 0038 missing, or the "
              "driver does not advertise it)";
        return false;
    }
    mVk = mDev->mDevice;
    if (!mFn.load(mVk)) {
        mVk = VK_NULL_HANDLE;
        err = "rayquery: a VK_KHR_acceleration_structure entry point is missing";
        return false;
    }

    mAsProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &mAsProps;
    vkGetPhysicalDeviceProperties2(mDev->mPhysicalDevice, &props2);
    mTimestampPeriod = props2.properties.limits.timestampPeriod;

    VkQueryPoolCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = kQueryCount;
    if (mTimestampPeriod > 0.0f) vkCreateQueryPool(mVk, &qci, nullptr, &mTimestamps);

    // COMPACTED SIZES. One slot per in-flight frame; a BLAS asks for its
    // compacted size in the frame it is built and reads the answer back later
    // WITHOUT waiting.
    VkQueryPoolCreateInfo cci{};
    cci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    cci.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    cci.queryCount = 64;
    vkCreateQueryPool(mVk, &cci, nullptr, &mCompactSizes);

    if (!makePipeline(err)) {
        close();
        return false;
    }
    // OUR FIXED RINGS MUST COVER THE PIN'S PIPELINE DEPTH (round 3, finding 4).
    // The retire logic reads the VaoManager's dynamic-buffer multiplier, but
    // the instance-slot ring, the per-scene timestamp ring and kQueryCount are
    // sized by kFramesInFlight. They agree today (both 3) and would disagree
    // silently if the pin ever deepened: the gather would write the slot a
    // queued build is still reading. Refusing is the honest answer — the tier
    // is optional and the no-rays picture is supported.
    if (framesInFlight() > kReflectRing) {
        err = "rayquery: this device keeps " + std::to_string(framesInFlight()) +
              " frames in flight; the reflection pass' descriptor ring holds " +
              std::to_string(kReflectRing);
        return false;
    }
    if (framesInFlight() > kFramesInFlight) {
        err = "rayquery: the render system keeps " + std::to_string(framesInFlight()) +
              " frames in flight and this tier's rings hold " +
              std::to_string(kFramesInFlight);
        close();
        return false;
    }
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka: hardware ray-query tier open (PHOTON R1)");
    return true;
}

bool RayQueryTier::makePipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].binding = 3;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (int i = 0; i < 4; ++i) {
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = 4;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(mVk, &sli, nullptr, &mSetLayout) != VK_SUCCESS) {
        err = "rayquery: vkCreateDescriptorSetLayout failed";
        return false;
    }
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mPipeLayout) != VK_SUCCESS) {
        err = "rayquery: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_raysSpv);
    smi.pCode = krq_raysSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mModule) != VK_SUCCESS) {
        err = "rayquery: vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mModule;
    cpi.stage.pName = "main";
    cpi.layout = mPipeLayout;
    if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &cpi, nullptr, &mPipeline) != VK_SUCCESS) {
        err = "rayquery: vkCreateComputePipelines failed";
        return false;
    }
    VkDescriptorPoolSize sizes[3] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = 8;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = 16;
    sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[2].descriptorCount = 8;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 8;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mVk, &dpi, nullptr, &mDescPool) != VK_SUCCESS) {
        err = "rayquery: vkCreateDescriptorPool failed";
        return false;
    }
    return true;
}

void RayQueryTier::close() {
    if (!mVk) return;
    // EVERY STRUCTURE DIES WHILE THE DEVICE IS STILL ALIVE. This runs from
    // OgreEngine::shutdownRayQuery, before Root is deleted — the same rule the
    // engine's MeshPtrs obey, and for the same reason.
    vkDeviceWaitIdle(mVk);
    for (auto &kv : mScenes) {
        SceneAs &sa = kv.second;
        for (Blas &bl : sa.blas) {
            if (bl.as) mFn.destroyAccelerationStructure(mVk, bl.as, nullptr);
            dropBuffer(bl.storage);
            bl.mesh.reset();
        }
        if (sa.tlas) mFn.destroyAccelerationStructure(mVk, sa.tlas, nullptr);
        dropBuffer(sa.tlasStorage);
        dropBuffer(sa.tlasScratch);
        dropBuffer(sa.instances);
        // THE SKIN CACHES, destroyed outright (the device is idle): the scene the
        // map is keyed by may already be gone, so the GpuScene side is not
        // touched — its tables die with it.
        for (auto &skv : sa.skins) {
            SceneAs::Skin &sk = skv.second;
            if (sk.as) mFn.destroyAccelerationStructure(mVk, sk.as, nullptr);
            sk.as = VK_NULL_HANDLE;
            dropBuffer(sk.storage);
            dropBuffer(sk.scratch);
            destroySkinCacheBuffer(sa.skinVao, sk.buf);
        }
        sa.skins.clear();
        dropSkinBuffers(sa);
    }
    mScenes.clear();
    for (auto &kv : mReflects) dropReflect(kv.second);
    mReflects.clear();
    // THE HIT LISTS (PHOTON-HIT-SHADE-1): the counters are Ogre UAV buffers (the
    // VaoManager is alive: close() runs before Root goes), the sets die with the
    // pool below, the buffers now (the device is idle).
    for (auto &kv : mHits) {
        HitView &hv = kv.second;
        if (hv.buf && mRs && mRs->getVaoManager()) mRs->getVaoManager()->destroyUavBuffer(hv.buf);
        hv.buf = nullptr;
        for (unsigned i = 0; i < kReflectRing; ++i) dropBuffer(hv.params[i]);
        dropBuffer(hv.readback);
    }
    mHits.clear();
    for (ReflectImage *d : { &mHitDummyIds, &mHitDummyColour, &mHitDummyDest, &mHitDummyDist }) {
        if (d->view) vkDestroyImageView(mVk, d->view, nullptr);
        if (d->image) vkDestroyImage(mVk, d->image, nullptr);
        if (d->memory) vkFreeMemory(mVk, d->memory, nullptr);
        *d = ReflectImage();
    }
    mHitDummiesReady = false;
    mHitDummiesNeedInit = false;
    // THE SUN CONTACT's views (PHOTON-RAYS-1): each takes its registration
    // away first (the listener holds a raw texture pointer) and retires its
    // texture into the bin that is emptied below. The sets are DROPPED, not
    // retired — the pool is destroyed below and frees them (the gather's
    // close() rule, and its reason).
    for (auto &kv : mSunContacts) {
        for (unsigned i = 0; i < kReflectRing; ++i) kv.second.sets[i] = VK_NULL_HANDLE;
        dropSunContact(kv.second);
    }
    mSunContacts.clear();
    FogHlmsListener::clearSunContact();
    // THE GATHER'S SHADER REGISTRATION DIES WITH THE TEXTURES IT NAMES
    // (GATHER-0's D1, kept): `ScreenProbeGather::close` frees every atlas and
    // takes every registration away, because `FogHlmsListener`'s map is a plain
    // pointer table the Hlms cannot see into — a map left non-empty here makes
    // the next colour pass bind a DESTROYED TextureGpu. Reachable: the no-rays
    // switch flipped while a scene is gathering takes `updateRayQuery`'s early
    // return, so the frame-head clear never runs and this teardown is the only
    // place left that knows.
    // THE BIN'S DESCRIPTOR SETS GO FIRST. A set the gather retired on a RESIZE
    // sits in `mRetireBin` naming the gather's OWN pool, and `close()` below
    // destroys that pool — freeing the set afterwards would hand Vulkan a dead
    // handle. The device is idle here, so nothing still reads them.
    for (Retired &r : mRetireBin)
        if (r.set && r.setPool) { vkFreeDescriptorSets(mVk, r.setPool, 1, &r.set); r.set = VK_NULL_HANDLE; }
    if (mGather) { mGather->close(); delete mGather; mGather = nullptr; }
    // (`mDummyArray` — SURFACE-CACHE-0's 1x1x6 stand-in for the card spike's
    // five bindings — went with the spike at SURFACE-CACHE-1b; nothing in the
    // gather or the reflect job binds a 2D ARRAY.)
    for (ReflectImage *d : { &mDummyCube, &mDummyVolume, &mDummyFlat }) {
        if (d->view) vkDestroyImageView(mVk, d->view, nullptr);
        if (d->image) vkDestroyImage(mVk, d->image, nullptr);
        if (d->memory) vkFreeMemory(mVk, d->memory, nullptr);
        *d = ReflectImage();
    }
    dropBuffer(mDummyStorage);
    mDummiesReady = false;
    mDummiesNeedClear = false;
    for (Retired &r : mRetireBin) {
        if (r.as) mFn.destroyAccelerationStructure(mVk, r.as, nullptr);
        dropBuffer(r.buf);
        if (r.set && r.setPool) vkFreeDescriptorSets(mVk, r.setPool, 1, &r.set);
        if (r.tex && mRs && mRs->getTextureGpuManager())
            mRs->getTextureGpuManager()->destroyTexture(r.tex);
        if (r.view) vkDestroyImageView(mVk, r.view, nullptr);
        if (r.img.view) vkDestroyImageView(mVk, r.img.view, nullptr);
        if (r.img.image) vkDestroyImage(mVk, r.img.image, nullptr);
        if (r.img.memory) vkFreeMemory(mVk, r.img.memory, nullptr);
    }
    mRetireBin.clear();
    // THE HIT WRITE-BACK'S POOL, after the bin's sets that name it are freed.
    if (mCompPool) vkDestroyDescriptorPool(mVk, mCompPool, nullptr);
    if (mCompPipeline) vkDestroyPipeline(mVk, mCompPipeline, nullptr);
    if (mCompModule) vkDestroyShaderModule(mVk, mCompModule, nullptr);
    if (mCompPipeLayout) vkDestroyPipelineLayout(mVk, mCompPipeLayout, nullptr);
    if (mCompSetLayout) vkDestroyDescriptorSetLayout(mVk, mCompSetLayout, nullptr);
    mCompPool = VK_NULL_HANDLE; mCompPipeline = VK_NULL_HANDLE; mCompModule = VK_NULL_HANDLE;
    mCompPipeLayout = VK_NULL_HANDLE; mCompSetLayout = VK_NULL_HANDLE;
    if (mSunPool) vkDestroyDescriptorPool(mVk, mSunPool, nullptr);
    if (mSunPipeline) vkDestroyPipeline(mVk, mSunPipeline, nullptr);
    if (mSunModule) vkDestroyShaderModule(mVk, mSunModule, nullptr);
    if (mSunPipeLayout) vkDestroyPipelineLayout(mVk, mSunPipeLayout, nullptr);
    if (mSunSetLayout) vkDestroyDescriptorSetLayout(mVk, mSunSetLayout, nullptr);
    if (mSunSampler) vkDestroySampler(mVk, mSunSampler, nullptr);
    if (mSunTimestamps) vkDestroyQueryPool(mVk, mSunTimestamps, nullptr);
    mSunPool = VK_NULL_HANDLE; mSunPipeline = VK_NULL_HANDLE; mSunModule = VK_NULL_HANDLE;
    mSunPipeLayout = VK_NULL_HANDLE; mSunSetLayout = VK_NULL_HANDLE;
    mSunSampler = VK_NULL_HANDLE; mSunTimestamps = VK_NULL_HANDLE;
    mSunQuerySlots = 0;
    // THE CARD MOVERS' state: sets dropped with the pool (the sun contact's rule).
    for (auto &kv : mCardMovers)
        for (unsigned i = 0; i < kReflectRing; ++i) {
            dropBuffer(kv.second.params[i]);
            dropBuffer(kv.second.records[i]);
        }
    mCardMovers.clear();
    if (mCmPool) vkDestroyDescriptorPool(mVk, mCmPool, nullptr);
    if (mCmPipeline) vkDestroyPipeline(mVk, mCmPipeline, nullptr);
    if (mCmModule) vkDestroyShaderModule(mVk, mCmModule, nullptr);
    if (mCmPipeLayout) vkDestroyPipelineLayout(mVk, mCmPipeLayout, nullptr);
    if (mCmSetLayout) vkDestroyDescriptorSetLayout(mVk, mCmSetLayout, nullptr);
    if (mCmTimestamps) vkDestroyQueryPool(mVk, mCmTimestamps, nullptr);
    mCmPool = VK_NULL_HANDLE; mCmPipeline = VK_NULL_HANDLE; mCmModule = VK_NULL_HANDLE;
    mCmPipeLayout = VK_NULL_HANDLE; mCmSetLayout = VK_NULL_HANDLE; mCmTimestamps = VK_NULL_HANDLE;
    mCmQuerySlots = 0;
    if (mCardParityPool) vkDestroyDescriptorPool(mVk, mCardParityPool, nullptr);
    if (mCardParitySampler) vkDestroySampler(mVk, mCardParitySampler, nullptr);
    mCardParitySampler = VK_NULL_HANDLE;
    if (mCardParityPipeline) vkDestroyPipeline(mVk, mCardParityPipeline, nullptr);
    if (mCardParityModule) vkDestroyShaderModule(mVk, mCardParityModule, nullptr);
    if (mCardParityPipeLayout) vkDestroyPipelineLayout(mVk, mCardParityPipeLayout, nullptr);
    if (mCardParitySetLayout) vkDestroyDescriptorSetLayout(mVk, mCardParitySetLayout, nullptr);
    mCardParityPool = VK_NULL_HANDLE; mCardParityPipeline = VK_NULL_HANDLE;
    mCardParityModule = VK_NULL_HANDLE; mCardParityPipeLayout = VK_NULL_HANDLE;
    mCardParitySetLayout = VK_NULL_HANDLE;
    if (mReflectPool) vkDestroyDescriptorPool(mVk, mReflectPool, nullptr);
    if (mReflectPipeline) vkDestroyPipeline(mVk, mReflectPipeline, nullptr);
    if (mFilterPipeline) vkDestroyPipeline(mVk, mFilterPipeline, nullptr);
    if (mFilterModule) vkDestroyShaderModule(mVk, mFilterModule, nullptr);
    if (mReflectPipeLayout) vkDestroyPipelineLayout(mVk, mReflectPipeLayout, nullptr);
    if (mReflectSetLayout) vkDestroyDescriptorSetLayout(mVk, mReflectSetLayout, nullptr);
    if (mReflectModule) vkDestroyShaderModule(mVk, mReflectModule, nullptr);
    if (mReflectTimestamps) vkDestroyQueryPool(mVk, mReflectTimestamps, nullptr);
    if (mPointSampler) vkDestroySampler(mVk, mPointSampler, nullptr);
    if (mLinearSampler) vkDestroySampler(mVk, mLinearSampler, nullptr);
    mReflectPool = VK_NULL_HANDLE; mReflectPipeline = VK_NULL_HANDLE;
    mReflectPipeLayout = VK_NULL_HANDLE; mReflectSetLayout = VK_NULL_HANDLE;
    mReflectModule = VK_NULL_HANDLE; mReflectTimestamps = VK_NULL_HANDLE;
    mFilterPipeline = VK_NULL_HANDLE; mFilterModule = VK_NULL_HANDLE;
    mPointSampler = VK_NULL_HANDLE; mLinearSampler = VK_NULL_HANDLE;
    if (mDescPool) vkDestroyDescriptorPool(mVk, mDescPool, nullptr);
    if (mPipeline) vkDestroyPipeline(mVk, mPipeline, nullptr);
    if (mPipeLayout) vkDestroyPipelineLayout(mVk, mPipeLayout, nullptr);
    if (mSetLayout) vkDestroyDescriptorSetLayout(mVk, mSetLayout, nullptr);
    if (mModule) vkDestroyShaderModule(mVk, mModule, nullptr);
    if (mTimestamps) vkDestroyQueryPool(mVk, mTimestamps, nullptr);
    if (mCompactSizes) vkDestroyQueryPool(mVk, mCompactSizes, nullptr);
    mDescPool = VK_NULL_HANDLE;
    mPipeline = VK_NULL_HANDLE;
    mPipeLayout = VK_NULL_HANDLE;
    mSetLayout = VK_NULL_HANDLE;
    mModule = VK_NULL_HANDLE;
    mTimestamps = VK_NULL_HANDLE;
    mCompactSizes = VK_NULL_HANDLE;
    mVk = VK_NULL_HANDLE;
}

/// A SCENE IS GONE. Called from OgreScene::destroy(), which is the only place
/// that knows it — without that call (and there was none until audit round 2's
/// finding 2) every destroyed preview or thumbnail scene left its structures,
/// its instance buffer and the MeshPtrs it holds alive for the process's life
/// under a dangling key, and a recycled OgreScene ADDRESS inherited a dead
/// scene's structures: a TLAS built for someone else's items, a blasOf
/// pointing at meshes this scene never had, and an epoch that made the tier
/// think nothing had moved. That is exactly the aliasing the held MeshPtr
/// exists to prevent, arriving by another door.
///
/// Everything retires through the frame-tagged bin rather than behind a device
/// wait: a scene can be destroyed on any frame, and draining the GPU to free a
/// thumbnail's structures would be a stall the user feels.
void RayQueryTier::forgetScene(OgreScene *scene) {
    auto it = mScenes.find(scene);
    if (it == mScenes.end()) return;
    SceneAs &sa = it->second;
    for (Blas &bl : sa.blas) {
        retire(bl.as, bl.storage);
        // THE MESH HOLD ENDS HERE, AND THAT IS CORRECT (round 3, finding 7).
        // A BUILT acceleration structure does not re-read its inputs — the
        // vertex and index buffers are consumed by the build, not referenced by
        // the result — so a structure still in the retire window needs no mesh.
        // The one case that would need the buffers alive is a build recorded
        // this frame and not yet executed, and the pin already covers it: a
        // VertexBufferPacked destroyed now is held by VaoManager's delayed
        // destruction for its own dynamic-buffer multiplier of frames.
        bl.mesh.reset();
    }
    retire(sa.tlas, sa.tlasStorage);
    retire(sa.tlasScratch);
    retire(sa.instances);
    // THE SKIN CACHES (PHOTON-SKIN-1): each hands back its structure, its buffer
    // and its row block, and clears its node's override — the rows stop naming
    // a posed copy nobody will update again.
    for (auto &kv : sa.skins) dropSkin(scene, sa, kv.first, kv.second);
    sa.skins.clear();
    sa.skinUse.clear();
    dropSkinBuffers(sa);
    // The compaction slots this scene still owed a read are never going to be
    // read; hand them back or the ring leaks capacity until compaction stops
    // for EVERY scene (round 3, finding 1: a parked preview did exactly that).
    for (const Blas &bl : sa.blas)
        if (bl.compactState == 1u) mCompactBusy &= ~(uint64_t(1) << bl.compactSlot);
    if (sa.hasQueryBase) mTimedSceneSlots &= ~(uint32_t(1) << sa.querySlot);
    mScenes.erase(it);
    forgetCardMovers(scene);
}

// ---------------------------------------------------------------------------
// THE INSTANCE SINK. The gather writes each instance descriptor straight into
// the mapped slot this frame will build from — no intermediate vector, which is
// the cost audit C's P3 item names (4-6 ms of CPU for 8,026 instances in the
// spike's naive form).
namespace {
struct InstanceWriter final {
    VkAccelerationStructureInstanceKHR *dst = nullptr;
    unsigned capacity = 0;
    unsigned count = 0;
    unsigned overflow = 0;
    /// How many of `count` are FAR copies (mask kRayMaskFar).
    unsigned farCount = 0;
    /// The widest coarse-vs-fine gap over the traced set, world units (F2),
    /// and the per-mesh-index cache of the coarsest bound it is built from.
    float maxCoarseBound = 0.0f;
    std::vector<std::pair<const Ogre::Mesh *, float>> *coarseBound = nullptr;
    /// The per-slot geometry row of the near copy (SceneAs::geomRowOfSlot).
    std::vector<uint32_t> *geomRowOfSlot = nullptr;
    /// THE SKIN CACHE's ready entries by NodeId (SceneAs::skinUse): a rigged
    /// slot's structure and row. A rigged slot NOT in it is not written at all.
    const std::unordered_map<uint32_t, std::pair<VkDeviceAddress, uint32_t>> *skinUse = nullptr;
    /// Rigged slots written this gather (the status's count).
    unsigned skinned = 0;
    unsigned long long signature = 1469598103934665603ull;   // FNV-1a offset basis
    /// THE SIGNATURE IS ONLY EVER READ TO DECIDE REFIT-vs-REBUILD. A rebuild is
    /// the default (NVIDIA's own guidance for a TLAS, and 0.2-0.35 ms even at
    /// 8,000 instances), so with the refit off there is nothing to compare and
    /// three FNV rounds per instance are pure cost.
    bool wantSignature = false;
    std::unordered_map<BlasKey, size_t, BlasKeyHash> *blasOf = nullptr;
    const std::vector<VkDeviceAddress> *blasAddress = nullptr;
    /// Slots referenced by this gather, so a BLAS nothing points at any more can
    /// be evicted (finding 5).
    std::vector<unsigned char> *seen = nullptr;
    /// (mesh, level)s seen this gather that have no BLAS yet, in first-seen
    /// order, and the instances that must have their reference patched once they
    /// do.
    std::vector<BlasWant> newBlas;
    std::unordered_map<BlasKey, unsigned, BlasKeyHash> newIndexOf;
    struct Patch { unsigned instance = 0; unsigned newBlas = 0; };
    std::vector<Patch> patches;

    void hash(unsigned long long v) {
        signature ^= v;
        signature *= 1099511628211ull;
    }

    /// ONE INSTANCE OVER A STRUCTURE THAT IS NOT THE (MESH, LEVEL) TABLE'S — a
    /// rigged item's own skinned BLAS (PHOTON-SKIN-1). Its address is known when
    /// the gather runs (the skin pass creates the structure first), so there is
    /// nothing to patch.
    void addDirect(VkDeviceAddress address, bool far, const float *world, unsigned mask,
                   unsigned customIndex) {
        const unsigned idx = count++;
        if (far) ++farCount;
        if (idx >= capacity) { ++overflow; return; }
        VkAccelerationStructureInstanceKHR inst{};
        std::memcpy(&inst.transform.matrix[0][0], world, 12u * sizeof(float));
        inst.instanceCustomIndex = customIndex & 0xFFFFFFu;
        inst.mask = mask & 0xFFu;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = address;
        if (wantSignature) { hash(0xA24BAED4963EE407ull ^ address); hash(customIndex); hash(mask); }
        std::memcpy(&dst[idx], &inst, sizeof(inst));
    }

    /// ONE-ENTRY MEMO PER COPY KIND (0 near, 1 far). The map lookup below was
    /// 8,001 hash lookups per gather on the lattice; consecutive instances
    /// almost always share a mesh (that scene is 8,001 instances of 22 meshes),
    /// so remembering the last key answers nearly all of them with a compare.
    /// ONE memo per kind because the writer alternates near and far for every
    /// slot — a single memo would miss on every call. A pure cache: a miss
    /// falls through to the map and gives the same answer.
    struct Memo { BlasKey key; size_t slot = 0; bool found = false; };
    Memo memo[2];

    /// One instance descriptor. `world` is the GPU scene's own twelve floats — a
    /// ROW-MAJOR 3x4, which is exactly `VkTransformMatrixKHR`'s layout, so the
    /// transform is ONE memcpy and no longer twelve loads out of an Ogre
    /// Matrix4 (the table already did that conversion once, for everybody).
    void add(const Ogre::MeshPtr &meshPtr, uint32_t level, bool far, const float *world,
             unsigned mask, unsigned customIndex) {
        const BlasKey key{ meshPtr.get(), level };
        const unsigned idx = count++;
        if (far) ++farCount;
        if (idx >= capacity) { ++overflow; return; }
        // BUILD IT ON THE STACK, STORE IT ONCE. `dst` points into HOST-VISIBLE
        // device memory, which on this driver is WRITE-COMBINED: it streams
        // writes beautifully and reads back at a crawl. Four of this struct's
        // fields are BITFIELDS (instanceCustomIndex 24, mask 8,
        // instanceShaderBindingTableRecordOffset 24, flags 8), and assigning a
        // bitfield is a read-modify-write — so writing them in place READ the
        // uncached mapping four times per instance. That, not the map lookup,
        // was the bulk of the 3.5 ms this function cost at 8,001 instances.
        // Assembled here and copied as one 64-byte store, the mapping is only
        // ever written, linearly.
        VkAccelerationStructureInstanceKHR inst{};
        std::memcpy(&inst.transform.matrix[0][0], world, 12u * sizeof(float));
        inst.instanceCustomIndex = customIndex & 0xFFFFFFu;
        inst.mask = mask & 0xFFu;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = 0;

        Memo &m = memo[far ? 1 : 0];
        if (m.found && m.key == key) {
            inst.accelerationStructureReference = (*blasAddress)[m.slot];
            if (seen && m.slot < seen->size()) (*seen)[m.slot] = 1u;
            if (wantSignature) { hash(1ull + m.slot); hash(customIndex); hash(mask); }
            std::memcpy(&dst[idx], &inst, sizeof(inst));
            return;
        }
        auto it = blasOf->find(key);
        if (it != blasOf->end()) {
            m.key = key; m.slot = it->second; m.found = true;
            if (seen && it->second < seen->size()) (*seen)[it->second] = 1u;
            inst.accelerationStructureReference = (*blasAddress)[it->second];
            if (wantSignature) hash(1ull + it->second);
        } else {
            m.found = false;
            auto nit = newIndexOf.find(key);
            unsigned ni;
            if (nit == newIndexOf.end()) {
                ni = unsigned(newBlas.size());
                newBlas.push_back({ meshPtr, level });
                newIndexOf.emplace(key, ni);
            } else {
                ni = nit->second;
            }
            patches.push_back({ idx, ni });
            if (wantSignature) hash(0x9E3779B97F4A7C15ull + ni);
        }
        if (wantSignature) { hash(customIndex); hash(mask); }
        std::memcpy(&dst[idx], &inst, sizeof(inst));
    }
};
}   // namespace

/// THE TRACED SET, READ OUT OF THE GPU SCENE'S TABLE (A3 slice §1.3).
///
/// WHAT CHANGED AND WHY. This used to be `OgreScene::gatherRayInstances`: a walk
/// of the scene's item index that re-asked Ogre, per item, every question the
/// table now answers — the visibility flags, the render queue, the skeleton, the
/// mesh, and a loop over every sub-item's datablock for an alpha test — and then
/// read the node's full world transform. Measured at 3.4-4.0 ms for 8,001
/// instances in a Debug build (inventory LAT-L8 / F11-WALKS). Every one of those
/// predicates is now precomputed into the flags word by the ONE place that
/// computes them (`OgreScene::gpuFlagsFor`), and the transform is already in the
/// exact layout an instance descriptor takes, so this loop is a flags test and a
/// memcpy per slot and asks Ogre nothing at all.
///
/// WHAT IS IN AND WHAT IS OUT: `kGpuRayTraced` IS the old conjunction —
/// carries kVisibleBit or kMovableBit, shown, below the overlay queues, not
/// alpha-tested (every
/// BLAS is VK_GEOMETRY_OPAQUE_BIT_KHR and the rays use gl_RayFlagsOpaqueEXT, so
/// a cut-out leaf would intersect as a solid quad: audit C-16), and it has a
/// mesh. Editor furniture, the backdrop, the sun disc and distortion objects
/// carry their own channel INSTEAD of kVisibleBit precisely so captures can
/// exclude them, and they fail the first test.
///
/// EVERY TRACED OBJECT IS WRITTEN TWICE (ATOM-FARBLAS-1, A5b §4 — ONE TLAS with
/// explicit masks; the two-TLAS alternative was rejected on cost: it doubles the
/// build and refit every frame and puts the near/far split in a binding instead
/// of a bit). The masks (Types.h, `kRayMask*`):
///
///   * THE NEAR COPY over the level the RAY RULE chose (`GpuInstance::ids[3]`,
///     AT-A8r, `OgreScene::updateRayLevels`; clamped to the chain), carrying the
///     per-consumer bits of audit C-15 — bit 0 a shadow caster, bit 1 a mover,
///     bit 2 still world — AND bit 3, "near". A near launch traces 0x0F. A
///     shadow-casting mover's near copy carries bit 5 too (kRayMaskMoverCaster,
///     the surface cache's mover-shadow launch, PHOTON-CARDS-4).
///   * THE FAR COPY over the mesh's COARSEST level, carrying bit 4 ALONE. Only a
///     launch that asks for the far field (the gather's second query, past its
///     near length) sees it; no near launch can hit a far copy, so no ray is
///     answered twice by one object.
///
/// A RIGGED ITEM (PHOTON-SKIN-1) is traced through ITS OWN structure, built from
/// its skin cache (the posed vertices) — the near AND the far copy, since the
/// cache is one level and a character is small — and its per-slot row is the
/// cache's. A rigged slot whose cache is not ready this frame is NOT WRITTEN: it
/// is never traced at the mesh's bind pose (audit C-5's T-pose), which is the
/// exclusion's whole reason and the only fallback there is.
///
/// `instanceCustomIndex` is the SLOT on BOTH copies — the index of the object's
/// entry in the table, so a hit shader reads the object's bounds, its previous
/// transform and its mesh with one fetch whichever copy it hit; the copy is told
/// apart by the MASK the launch asked with, never by the index.
static void writeRayInstances(const OgreScene *scene, InstanceWriter &w) {
    const detail::GpuScene &gs = scene->gpuScene();
    if (!gs.live()) return;
    const detail::GpuInstance *mirror = gs.mirrorData();
    const uint32_t slots = gs.slotCount();
    if (w.geomRowOfSlot) w.geomRowOfSlot->assign(slots, detail::GpuScene::kNoGeomRow);
    for (uint32_t i = 0; i < slots; ++i) {
        const detail::GpuInstance &e = mirror[i];
        Ogre::uint32 flags;
        std::memcpy(&flags, &e.boundsMax[3], sizeof(flags));
        if (!(flags & detail::kGpuRayTraced)) continue;
        Ogre::uint32 meshIndex;
        std::memcpy(&meshIndex, &e.boundsMin[3], sizeof(meshIndex));
        const Ogre::MeshPtr &mesh = gs.meshAt(meshIndex);
        if (!mesh) continue;
        unsigned mask = kRayMaskNear;
        mask |= (flags & detail::kGpuCaster) ? kRayMaskCaster : 0u;
        mask |= (flags & detail::kGpuMover) ? kRayMaskMover : kRayMaskStill;
        // ...and the surface cache's mover-shadow bit (PHOTON-CARDS-4): a mover
        // that casts, on its near copy only.
        if ((flags & detail::kGpuMover) && (flags & detail::kGpuCaster)) mask |= kRayMaskMoverCaster;
        if (flags & detail::kGpuSkinned) {
            if (!w.skinUse) continue;
            auto sit = w.skinUse->find(e.ids[0]);
            if (sit == w.skinUse->end()) continue;
            w.addDirect(sit->second.first, false, e.world, mask, i);
            w.addDirect(sit->second.first, true, e.world, kRayMaskFar, i);
            if (w.geomRowOfSlot) (*w.geomRowOfSlot)[i] = sit->second.second;
            ++w.skinned;
            continue;
        }
        const uint32_t coarsest = coarsestLevelOf(mesh.get());
        const uint32_t nearLevel = std::min(scene->rayLevelOf(i), coarsest);
        w.add(mesh, nearLevel, false, e.world, mask, i);
        w.add(mesh, coarsest, true, e.world, kRayMaskFar, i);
        // The near copy's geometry, as the GPU scene's rows name it (a level the
        // mesh has no row for reads zero addresses there, which the shader tests).
        if (w.geomRowOfSlot && nearLevel < detail::GpuScene::kLevelsPerMesh)
            (*w.geomRowOfSlot)[i] = detail::GpuScene::geomRowIndex(meshIndex, nearLevel, 0u);
        // THE HAND-OVER'S WIDTH (audit F2): how far, in world units, a far copy's
        // surface may lie from its fine one — the coarsest level's measured bound
        // grown by the instance's largest axis scale. The gather starts its far
        // query that much BEFORE the near length, so a coarse surface inside it
        // whose fine surface lies just outside cannot be passed by both queries.
        // The bound is cached per MESH TABLE INDEX (checked against the mesh
        // pointer, so a recycled index re-reads): a lookup through the scene's
        // mesh records per instance doubled this loop on the lattice, whose
        // consecutive instances cycle through eleven meshes.
        if (coarsest > 0u) {
            if (meshIndex >= w.coarseBound->size()) w.coarseBound->resize(meshIndex + 1u);
            auto &cached = (*w.coarseBound)[meshIndex];
            if (cached.first != mesh.get()) {
                const std::vector<float> *b = scene->lodBoundsFor(mesh.get());
                cached = { mesh.get(), (b && !b->empty()) ? b->back() : 0.0f };
            }
            if (cached.second > 0.0f) {
                const float grown = cached.second * worldMaxAxisScale(e.world);
                if (std::isfinite(grown) && grown > w.maxCoarseBound) w.maxCoarseBound = grown;
            }
        }
    }
}

// ---------------------------------------------------------------------------
namespace {
/// One mesh's geometry descriptors, read straight out of Ogre's v2 buffers.
///
/// A VertexBufferPacked lives inside a shared POOL VkBuffer, so the geometry's
/// device address is "the pool's address, plus this buffer's own start inside
/// it, plus the position element's offset inside the vertex". That is exactly
/// the arithmetic Ogre's own draw performs — it binds every vertex buffer at
/// offset 0 and folds the start into baseVertex/firstIndex — so a BLAS built
/// this way reads the same triangles the raster does.
struct MeshGeometry {
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<uint32_t> primCounts;
    unsigned triangles = 0;
};

/// `level` picks the VAO of the mesh's LOD chain (clamped to what a submesh
/// carries): every level shares level 0's vertex buffer and owns its own index
/// buffer (OgreMesh.cpp), so a level's BLAS is the same arithmetic over a
/// different index range.
bool describeMesh(const Ogre::Mesh *mesh, uint32_t level, MeshGeometry &out,
                  VkDeviceAddress (*addressOf)(void *, VkBuffer), void *self) {
    for (unsigned s = 0, e = unsigned(mesh->getNumSubMeshes()); s < e; ++s) {
        const Ogre::SubMesh *sub = mesh->getSubMesh(s);
        if (!sub || sub->mVao[Ogre::VpNormal].empty()) continue;
        const size_t levels = sub->mVao[Ogre::VpNormal].size();
        Ogre::VertexArrayObject *vao =
            sub->mVao[Ogre::VpNormal][std::min<size_t>(level, levels - 1u)];
        // ONLY INDEXED TRIANGLE LISTS. Everything this engine builds is one
        // (buildMeshV2), and a strip or an unindexed draw would need its own
        // geometry shape; skipping is the honest answer, not a silent wrong
        // triangle.
        if (!vao || vao->getOperationType() != Ogre::OT_TRIANGLE_LIST || !vao->getIndexBuffer())
            continue;
        size_t elemIdx = 0, elemOffset = 0;
        const Ogre::VertexElement2 *posElem =
            vao->findBySemantic(Ogre::VES_POSITION, elemIdx, elemOffset);
        if (!posElem || posElem->mType != Ogre::VET_FLOAT3) continue;
        Ogre::VertexBufferPacked *vb = vao->getVertexBuffers()[elemIdx];
        Ogre::IndexBufferPacked *ib = vao->getIndexBuffer();
        Ogre::VulkanBufferInterface *vbi =
            static_cast<Ogre::VulkanBufferInterface *>(vb->getBufferInterface());
        Ogre::VulkanBufferInterface *ibi =
            static_cast<Ogre::VulkanBufferInterface *>(ib->getBufferInterface());
        if (!vbi || !ibi) continue;
        const uint32_t tris = uint32_t(vao->getPrimitiveCount() / 3u);
        if (!tris) continue;

        VkAccelerationStructureGeometryKHR g{};
        g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;      // audit C-16: no any-hit exists
        g.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress =
            addressOf(self, vbi->getVboName()) +
            VkDeviceSize(vb->_getFinalBufferStart()) * vb->getBytesPerElement() +
            VkDeviceSize(elemOffset);
        g.geometry.triangles.vertexStride = vb->getBytesPerElement();
        g.geometry.triangles.maxVertex = uint32_t(vb->getNumElements() - 1u);
        g.geometry.triangles.indexType = ib->getIndexType() == Ogre::IndexBufferPacked::IT_16BIT
                                             ? VK_INDEX_TYPE_UINT16
                                             : VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress =
            addressOf(self, ibi->getVboName()) +
            VkDeviceSize(ib->_getFinalBufferStart()) * ib->getBytesPerElement() +
            VkDeviceSize(vao->getPrimitiveStart()) * ib->getBytesPerElement();
        VkAccelerationStructureBuildRangeInfoKHR r{};
        r.primitiveCount = tris;
        out.geoms.push_back(g);
        out.ranges.push_back(r);
        out.primCounts.push_back(tris);
        out.triangles += tris;
    }
    return !out.geoms.empty();
}
}   // namespace

bool RayQueryTier::ensureBlas(SceneAs &sa, const std::vector<BlasWant> &wants,
                              VkCommandBuffer cmd, unsigned &built, std::string &err) {
    built = 0;
    struct Job {
        Ogre::MeshPtr mesh;
        uint32_t level = 0;
        MeshGeometry geo;
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        size_t slot = 0;
    };
    std::vector<Job> jobs;
    jobs.reserve(wants.size());
    auto addrThunk = [](void *self, VkBuffer b) -> VkDeviceAddress {
        return static_cast<RayQueryTier *>(self)->addressOf(b);
    };
    for (const BlasWant &want : wants) {
        Job j;
        j.mesh = want.mesh;
        j.level = want.level;
        if (!describeMesh(want.mesh.get(), want.level, j.geo, addrThunk, this)) continue;
        j.build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        j.build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // FAST TRACE + ALLOW COMPACTION: a mesh's BLAS is built once and traced
        // for the rest of the session, so the tree's quality is worth paying
        // for, and NVIDIA reports up to ~50 % of its memory back on compaction.
        j.build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        j.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        j.build.geometryCount = uint32_t(j.geo.geoms.size());
        j.sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) return true;

    VkDeviceSize scratchTotal = 0;
    const VkDeviceSize align = std::max<VkDeviceSize>(
        mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
    for (Job &j : jobs) {
        j.build.pGeometries = j.geo.geoms.data();
        mFn.getBuildSizes(mVk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &j.build,
                          j.geo.primCounts.data(), &j.sizes);
        scratchTotal = (scratchTotal + align - 1) / align * align + j.sizes.buildScratchSize;
    }

    // ONE SCRATCH ARENA for the whole batch, at offsets — the builds do not
    // alias, so there is NO serialising barrier between them (the spike's
    // shared buffer is why its 26 tiny builds cost 4.6-7.6 ms).
    RawBuffer scratch;
    if (!makeBuffer(scratchTotal + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, scratch,
                    err))
        return false;
    VkDeviceAddress cursor = (addressOf(scratch.buffer) + align - 1) / align * align;

    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs;
    builds.reserve(jobs.size());
    rangePtrs.reserve(jobs.size());
    for (Job &j : jobs) {
        Blas bl;
        bl.triangles = j.geo.triangles;
        bl.mesh = j.mesh;
        bl.level = j.level;
        if (!makeBuffer(j.sizes.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true,
                        bl.storage, err)) {
            dropBuffer(scratch);
            return false;
        }
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = bl.storage.buffer;
        ci.size = j.sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &bl.as) != VK_SUCCESS) {
            err = "rayquery: vkCreateAccelerationStructureKHR (BLAS) failed";
            dropBuffer(bl.storage);
            dropBuffer(scratch);
            return false;
        }
        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = bl.as;
        bl.address = mFn.getAsDeviceAddress(mVk, &ai);

        j.build.dstAccelerationStructure = bl.as;
        cursor = (cursor + align - 1) / align * align;
        j.build.scratchData.deviceAddress = cursor;
        cursor += j.sizes.buildScratchSize;

        // THE SLOT IS HANDED OUT HERE, in the loop that PUSHES the structure —
        // never in the collection loop (the spike wrote `mBlasOfMesh[mesh] =
        // mBlas.size()` while mBlas was still empty, gave every mesh slot 0,
        // and rendered the world as copies of the first mesh).
        j.slot = sa.blas.size();
        sa.blasOf[BlasKey{ j.mesh.get(), j.level }] = j.slot;
        sa.blas.push_back(std::move(bl));
        builds.push_back(j.build);
        rangePtrs.push_back(j.geo.ranges.data());
        ++built;
    }

    // OGRE'S PENDING UPLOADS FIRST, expressed as a barrier instead of a stall:
    // the staging copies that fill these very vertex buffers are recorded into
    // THIS command buffer (or an earlier submit on this queue), and a BLAS
    // built over vertex memory those copies have not written is built over
    // whatever was in that VRAM before — which does not fail, it takes
    // unbounded time (S3 §7 defect 3, which the spike answered with stall()).
    VkMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &pre, 0,
                         nullptr, 0, nullptr);
    mFn.cmdBuild(cmd, uint32_t(builds.size()), builds.data(), rangePtrs.data());

    // COMPACTION, PHASE A: ask for the compacted sizes of what we just built.
    // The answer is read WITHOUT waiting, several frames later (runCompaction).
    // BUILD -> BUILD, ALWAYS (finding 1): the builds above share one arena and
    // the reads below (compaction's size query, and the next frame's build over
    // the same structures) must not start until they have finished. This barrier
    // used to live INSIDE the compaction block, so a batch of more than
    // kCompactRing structures — which skips compaction — emitted no barrier at
    // all.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);

    // COMPACTION, PHASE A: ask for the compacted sizes of what we just built.
    // The slots come from a RING with a write cursor (finding 4). Writing
    // 0..n every time meant a second batch inside the read-back window
    // overwrote a pending batch's sizes, and phase B then compacted a structure
    // into a buffer sized for a different one. A batch the ring cannot hold
    // without trampling something still pending simply skips compaction — it
    // costs memory, never correctness.
    if (mCompactSizes) {
        std::vector<VkAccelerationStructureKHR> handles;
        handles.reserve(built);
        for (const Job &j : jobs)
            if (j.build.dstAccelerationStructure) handles.push_back(j.build.dstAccelerationStructure);
        const unsigned n = unsigned(handles.size());
        // A run of FREE slots, never "the cursor plus n". A batch that does not
        // fit simply goes uncompacted: that costs memory, never correctness,
        // and the slots come back as soon as their owners read them.
        const int base = takeCompactRun(n);
        if (base >= 0) {
            vkCmdResetQueryPool(cmd, mCompactSizes, unsigned(base), n);
            mFn.cmdWriteProperties(cmd, n, handles.data(),
                                   VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                   mCompactSizes, unsigned(base));
            unsigned q = unsigned(base);
            const uint32_t stamp = frameNow();
            for (const Job &j : jobs) {
                Blas &bl = sa.blas[j.slot];
                bl.compactState = 1;
                bl.compactSlot = q;
                bl.compactFrame = stamp;
                mCompactBusy |= uint64_t(1) << q;
                ++q;
            }
        }
    }

    // The arena is freed once the frames that could still be reading it are
    // certainly done — never immediately (it is referenced by a build this
    // command buffer has not submitted yet).
    retire(scratch);
    return true;
}

// ---------------------------------------------------------------------------
// COMPACTION, PHASE B. Reads the sizes asked for earlier WITHOUT
// VK_QUERY_RESULT_WAIT_BIT (audit C-14: a wait on the frame thread is never
// allowed here) and, when the answer is there and worth it, copies the
// structure into a smaller one. The original is freed a few frames later,
// when nothing can still be reading it.
bool RayQueryTier::runCompaction(SceneAs &sa, VkCommandBuffer cmd) {
    if (!mCompactSizes) return false;
    bool compacted_any = false;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (Blas &bl : sa.blas) {
        if (bl.compactState != 1u) continue;
        if (uint32_t(now - bl.compactFrame) < inFlight) continue;   // not submitted yet
        VkDeviceSize compacted = 0;
        const VkResult r = vkGetQueryPoolResults(mVk, mCompactSizes, bl.compactSlot, 1,
                                                 sizeof(compacted), &compacted, sizeof(compacted),
                                                 VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS || compacted == 0) continue;            // not available yet
        bl.compactState = 2;
        mCompactBusy &= ~(uint64_t(1) << bl.compactSlot);   // answered: reusable
        if (compacted >= bl.storage.size) continue;                 // nothing to win
        RawBuffer small;
        std::string err;
        if (!makeBuffer(compacted, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false,
                        true, small, err))
            continue;
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = small.buffer;
        ci.size = compacted;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR dst = VK_NULL_HANDLE;
        if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &dst) != VK_SUCCESS) {
            dropBuffer(small);
            continue;
        }
        VkCopyAccelerationStructureInfoKHR cp{};
        cp.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
        cp.src = bl.as;
        cp.dst = dst;
        cp.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        mFn.cmdCopy(cmd, &cp);
        // The uncompacted structure is RETIRED, not freed: this frame's copy
        // still reads it, and so may a trace already queued.
        retire(bl.as, bl.storage);
        bl.as = dst;
        bl.storage = small;
        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = dst;
        bl.address = mFn.getAsDeviceAddress(mVk, &ai);
        // The TLAS references the OLD address until it is rebuilt: force one.
        sa.setSignature = 0;
        compacted_any = true;
    }
    return compacted_any;
}

bool RayQueryTier::buildTlas(SceneAs &sa, VkCommandBuffer cmd, bool refit, std::string &err) {
    VkAccelerationStructureGeometryKHR geom{};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    geom.geometry.instances.data.deviceAddress =
        addressOf(sa.instances.buffer) +
        VkDeviceSize(sa.slot) * sa.instanceCapacity * sizeof(VkAccelerationStructureInstanceKHR);

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // ALLOW_UPDATE costs tree quality and memory, and is useless unless a refit
    // is actually going to be taken (finding 13). The default is a full rebuild
    // — NVIDIA's own guidance for a TLAS, 0.21-0.24 ms at 8,001 instances — so
    // the flag rides the same switch the refit does.
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (preferRefit()) build.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    build.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                       : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    // SIZE TO THE CAPACITY, NOT THE COUNT (finding 6). A build may use FEWER
    // primitives than the size query was made for, so asking once per CAPACITY
    // step and building with the live count is legal and means adding objects
    // one at a time does not reallocate — and therefore does not drain the GPU
    // — on every single add. The storage only grows when the capacity does.
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    const uint32_t sizeFor = std::max(sa.instanceCapacity, sa.instanceCount);
    mFn.getBuildSizes(mVk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &sizeFor,
                      &sizes);

    if (!sa.tlas || sa.tlasStorage.size < sizes.accelerationStructureSize) {
        if (sa.tlas) {
            // RETIRED, not waited on: a queued trace may still be reading it.
            retire(sa.tlas, sa.tlasStorage);
            retire(sa.tlasScratch);
        }
        if (!makeBuffer(sizes.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true,
                        sa.tlasStorage, err))
            return false;
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = sa.tlasStorage.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &sa.tlas) != VK_SUCCESS) {
            err = "rayquery: vkCreateAccelerationStructureKHR (TLAS) failed";
            return false;
        }
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;   // nothing to update yet
        refit = false;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(
        mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
    const VkDeviceSize need = std::max(sizes.buildScratchSize, sizes.updateScratchSize) + align;
    if (sa.tlasScratch.size < need) {
        retire(sa.tlasScratch);      // a queued build may still be reading it
        if (!makeBuffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, sa.tlasScratch, err))
            return false;
    }
    VkDeviceAddress scratchAddr = (addressOf(sa.tlasScratch.buffer) + align - 1) / align * align;
    build.dstAccelerationStructure = sa.tlas;
    if (build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR)
        build.srcAccelerationStructure = sa.tlas;
    build.scratchData.deviceAddress = scratchAddr;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = sa.instanceCount;
    const VkAccelerationStructureBuildRangeInfoKHR *rangePtr = &range;

    // WHAT THIS BUILD MUST WAIT FOR — all of it (finding 1). The old barrier
    // named only a previous TRACE (compute, AS read), and missed three writers:
    //
    //   * LAST FRAME'S TLAS BUILD, which wrote this same structure and this
    //     same scratch buffer. A write-after-write across command buffers is
    //     not ordered for free.
    //   * THE COMPACTION COPY recorded into THIS command buffer a few lines
    //     earlier (runCompaction), which writes acceleration-structure memory.
    //   * THE BLAS BUILDS of this same frame, whose structures this build
    //     reads through the instance array's references.
    //
    // So the source is both stages and both accesses; the destination stays the
    // build. It costs nothing on a frame where none of them happened.
    VkMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    pre.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &pre, 0,
                         nullptr, 0, nullptr);
    mFn.cmdBuild(cmd, 1, &build, &rangePtr);
    // BUILD -> TRACE. Ogre's BarrierSolver has no vocabulary for an
    // acceleration structure at all, so this one is ours by necessity, not by
    // preference. (Every TEXTURE this tier's consumers write is a different
    // matter: those go to the solver through assumeTransition.)
    VkMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    post.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &post, 0, nullptr, 0, nullptr);

    sa.st.tlasBytes = sizes.accelerationStructureSize;
    sa.st.lastWasRefit = (build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR);
    if (sa.st.lastWasRefit) ++sa.st.tlasRefits; else ++sa.st.tlasBuilds;
    return true;
}

// ---------------------------------------------------------------------------
// THE GPU SKIN CACHE (PHOTON-SKIN-1, RY-R4; SkinCache.h has the design).
namespace {
/// A UAV binding for the skin job (the voxel gather's shape).
Ogre::DescriptorSetUav::BufferSlot skinSlot(Ogre::UavBufferPacked *buffer,
                                            Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = buffer;
    slot.offset = 0;
    slot.sizeBytes = 0;
    slot.access = access;
    return slot;
}
constexpr uint32_t kSkinThreadsPerGroup = 64u;   // the job's threads_per_group x
}  // namespace

/// THE POSE SERIAL a cache is keyed on: the node's own pose pushes AND, for a
/// shareSkeleton follower (an armour or clothing piece posed by the MASTER's
/// instance), the master's — the recipe the caster walk uses (OgreGi.cpp,
/// walkItems). The follower's own poseEpoch never moves when the body's clip
/// does, so keyed on it alone the piece would be skinned once and frozen.
unsigned long long RayQueryTier::skinPoseSerial(const OgreScene *scene, const OgreScene::Node &n) {
    unsigned long long pose = n.poseEpoch;
    if (n.shareSource) {
        auto sit = scene->mNodes.find(n.shareSource);
        if (sit != scene->mNodes.end()) pose = pose * 1000003ull + sit->second.poseEpoch;
    }
    return pose;
}

void RayQueryTier::dropSkin(OgreScene *scene, SceneAs &sa, uint32_t node, SceneAs::Skin &sk) {
    if (sk.as) retire(sk.as, sk.storage);
    retire(sk.scratch);
    sk.address = 0;
    sk.built = false;
    destroySkinCacheBuffer(sa.skinVao, sk.buf);
    if (scene && scene->mGpuScene.live()) {
        detail::GpuScene &gs = scene->mGpuScene;
        if (gs.skinRowOf(node) != detail::GpuScene::kNoGeomRow) {
            gs.setSkinRow(node, detail::GpuScene::kNoGeomRow);
            // The override goes with the cache: the node's entry is re-composed
            // (if the node still has an Item) on the next scan.
            auto nit = scene->mNodes.find(NodeId(node));
            if (nit != scene->mNodes.end()) scene->markGpuSlotDirty(nit->second);
        }
        if (sk.rowBlock != 0xFFFFFFFFu) gs.releaseRowBlock(sk.rowBlock);
    }
    sk.rowBlock = sk.row = 0xFFFFFFFFu;
}

void RayQueryTier::dropSkinBuffers(SceneAs &sa) {
    if (sa.skinVao) {
        if (sa.skinJobs) sa.skinVao->destroyUavBuffer(sa.skinJobs);
        if (sa.skinPalette) sa.skinVao->destroyUavBuffer(sa.skinPalette);
    }
    sa.skinJobs = sa.skinPalette = nullptr;
    sa.skinJobCap = sa.skinPaletteCap = 0;
}

bool RayQueryTier::skinPass(OgreScene *scene, SceneAs &sa, VkCommandBuffer &cmd, bool timed,
                            unsigned qBase, std::string &err) {
    sa.skinUse.clear();
    sa.st.skinLastItems = 0;
    sa.st.skinLastVertices = 0;
    for (auto &kv : sa.skins) kv.second.seen = false;
    detail::GpuScene &gs = scene->mGpuScene;
    if (!gs.live()) return true;

    // 1. THE RIGGED TRACED SET, out of the GPU scene's mirror (current: the scan
    //    ran this frame, just before this tier). The flags word is the one place
    //    the predicates live; kGpuRayTraced no longer excludes a rigged item.
    struct Want { uint32_t slot = 0; OgreScene::Node *node = nullptr; };
    std::vector<Want> wants;
    const detail::GpuInstance *mirror = gs.mirrorData();
    const uint32_t slots = std::min<uint32_t>(gs.slotCount(), uint32_t(scene->mItemNodes.size()));
    for (uint32_t i = 0; i < slots; ++i) {
        Ogre::uint32 flags;
        std::memcpy(&flags, &mirror[i].boundsMax[3], sizeof(flags));
        if ((flags & (detail::kGpuRayTraced | detail::kGpuSkinned)) !=
            (detail::kGpuRayTraced | detail::kGpuSkinned))
            continue;
        OgreScene::Node *n = scene->mItemNodes[i];
        if (!n || !n->item || !n->item->getSkeletonInstance()) continue;
        wants.push_back({ i, n });
    }
    if (wants.empty()) return true;

    Ogre::VaoManager *vao = mRs ? mRs->getVaoManager() : nullptr;
    Ogre::HlmsManager *hm = Ogre::Root::getSingletonPtr() ? Ogre::Root::getSingleton().getHlmsManager()
                                                          : nullptr;
    Ogre::HlmsCompute *hc = hm ? hm->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow("Jahshaka/SkinCache") : nullptr;
    if (!vao || !vao->supportsBufferDeviceAddress()) {
        sa.st.skinReason = "no buffer device addresses on this device";
        return true;
    }
    if (!job) {
        sa.st.skinReason = "the Jahshaka/SkinCache job is missing from the staged media";
        err = sa.st.skinReason;
        return false;
    }
    if (sa.skinVao && sa.skinVao != vao) dropSkinBuffers(sa);
    sa.skinVao = vao;
    sa.st.skinReason.clear();

    // 2. RECONCILE: a cache per rigged traced item, created on first sight (or
    //    when the Item behind the node was rebuilt), marked seen; the ones not
    //    seen are dropped AFTER the gather (updateScene).
    struct Dirty { uint32_t node = 0; SceneAs::Skin *sk = nullptr; OgreScene::Node *n = nullptr; };
    std::vector<Dirty> dirty;
    bool rowsStaged = false;
    for (const Want &wt : wants) {
        const uint32_t node = uint32_t(wt.node->selfId);
        auto it = sa.skins.find(node);
        // THE IDENTITY, AND THE SOURCE IT WAS BUILT OVER (a mismatch in any of them
        // drops the cache and makes a new one this pass): a refit over an index
        // buffer the mesh released, or a job over a source with fewer vertices
        // than the cache, is a read of freed or foreign memory — an Xid, never a
        // validation error.
        const Ogre::VertexArrayObject *liveVao =
            (wt.node->item->getMesh() && wt.node->item->getMesh()->getNumSubMeshes() &&
             !wt.node->item->getMesh()->getSubMesh(0)->mVao[Ogre::VpNormal].empty())
                ? wt.node->item->getMesh()->getSubMesh(0)->mVao[Ogre::VpNormal][0]
                : nullptr;
        if (it != sa.skins.end() &&
            (it->second.item != wt.node->item ||
             it->second.mesh != wt.node->item->getMesh().get() ||
             it->second.rigGeneration != wt.node->rigGeneration || !liveVao ||
             liveVao->getIndexBuffer() != it->second.indices ||
             liveVao->getVertexBuffers().empty() ||
             uint32_t(liveVao->getVertexBuffers()[0]->getNumElements()) != it->second.buf.vertexCount)) {
            dropSkin(scene, sa, node, it->second);
            sa.skins.erase(it);
            it = sa.skins.end();
        }
        if (it == sa.skins.end()) {
            SceneAs::Skin sk;
            sk.item = wt.node->item;
            sk.mesh = wt.node->item->getMesh().get();
            sk.rigGeneration = wt.node->rigGeneration;
            sk.indices = liveVao ? liveVao->getIndexBuffer() : nullptr;
            std::string why;
            if (!createSkinCacheBuffer(vao, sk.item, sk.buf, why)) {
                // Not traced — and said so once per item, never at bind pose.
                sa.st.skinReason = why;
                continue;
            }
            std::vector<std::vector<uint32_t>> rows;
            sk.rowBlock = gs.acquireRowBlock();
            if (sk.rowBlock == detail::GpuScene::kNoMesh ||
                !describeSkinCacheRows(vao, sk.item, sk.buf, rows) || rows.empty() ||
                rows[0].size() != detail::GpuScene::kGeomRowWords) {
                sa.st.skinReason = "the skin cache's geometry rows could not be described";
                if (sk.rowBlock != detail::GpuScene::kNoMesh) gs.releaseRowBlock(sk.rowBlock);
                destroySkinCacheBuffer(vao, sk.buf);
                continue;
            }
            for (size_t l = 0; l < rows.size() && l < detail::GpuScene::kLevelsPerMesh; ++l)
                if (rows[l].size() == detail::GpuScene::kGeomRowWords)
                    gs.stageGeomRow(detail::GpuScene::geomRowIndex(sk.rowBlock, uint32_t(l), 0u),
                                    rows[l].data());
            sk.row = detail::GpuScene::geomRowIndex(sk.rowBlock, 0u, 0u);
            gs.setSkinRow(node, sk.row);
            scene->markGpuSlotDirty(*wt.node);
            rowsStaged = true;
            it = sa.skins.emplace(node, std::move(sk)).first;
        }
        SceneAs::Skin &sk = it->second;
        sk.seen = true;
        // THE POSE SERIAL: bumped by every pose push (clip time, weights, manual
        // bones — OgreScene::noteNodePosed). A walk moves the node, not the pose,
        // and costs nothing here: the cache is in the item's LOCAL space.
        const unsigned long long serial = skinPoseSerial(scene, *wt.node);
        if (!sk.skinned || !sk.built || serial != sk.poseSerial) dirty.push_back({ node, &sk, wt.node });
    }
    // A row not on the device is a zero address to the job (the ATOM-VOXEL-2 Xid):
    // the new rows go up NOW, before anything below binds the table.
    if (rowsStaged) gs.flushGeomRows();

    if (!dirty.empty()) {
        // 3. THE PALETTE: Ogre's own bone matrices for the renderable — the SAME
        //    `SkeletonInstance::_getBoneFullTransform` values, in the SAME
        //    blend-index order, that HlmsPbs::fillBuffersForV2 streams into its
        //    per-pass buffer for the vertex shader (PREMISE 1's verdict: that
        //    buffer is a per-PASS ring written only for a DRAWN renderable, at an
        //    offset only the draw knows — a character off screen, the one a
        //    mirror or a shadow ray most needs, has no palette in it at all — so
        //    the values are taken from their source instead of the buffer) —
        //    taken back into the item's local space by the node's inverse world.
        std::vector<float> palette;
        std::vector<SkinJobRecord> jobs;
        uint32_t maxVerts = 0u;
        for (Dirty &d : dirty) {
            Ogre::Item *item = d.sk->item;
            Ogre::SkeletonInstance *skel = item->getSkeletonInstance();
            const Ogre::SubItem *sub = item->getNumSubItems() ? item->getSubItem(0) : nullptr;
            const Ogre::RenderableAnimated::IndexMap *map =
                sub ? sub->getBlendIndexToBoneIndexMap() : nullptr;
            Ogre::Node *parent = item->getParentNode();
            if (!skel || !map || map->empty() || !parent) { d.sk = nullptr; continue; }
            const Ogre::Matrix4 invWorld = parent->_getFullTransform().inverseAffine();
            SkinJobRecord r;
            r.sourceRow = gs.meshIndex(item->getMesh().get()) == detail::GpuScene::kNoMesh
                              ? detail::GpuScene::kNoGeomRow
                              : detail::GpuScene::geomRowIndex(gs.meshIndex(item->getMesh().get()), 0u, 0u);
            if (r.sourceRow == detail::GpuScene::kNoGeomRow) { d.sk = nullptr; continue; }
            r.vertexCount = d.sk->buf.vertexCount;
            r.paletteBase = uint32_t(palette.size() / 4u);
            r.cacheAddressLo = uint32_t(d.sk->buf.address & 0xFFFFFFFFull);
            r.cacheAddressHi = uint32_t(d.sk->buf.address >> 32u);
            r.tangentOffset = d.sk->buf.tangentOffset;
            r.blendOffsets = (d.sk->buf.blendIndexOffset & 0xFFFFu) |
                             ((d.sk->buf.blendWeightOffset & 0xFFFFu) << 16u);
            r.boneCount = uint32_t(map->size());
            for (size_t b = 0; b < map->size(); ++b) {
                // store4x3, not streamTo4x3: the stream form is a non-temporal
                // store meant for a mapped GPU buffer; this is a stack copy.
                alignas(16) float m[12];
                skel->_getBoneFullTransform((*map)[b]).store4x3(m);
                const Ogre::Matrix4 world(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
                                          m[9], m[10], m[11], 0.0f, 0.0f, 0.0f, 1.0f);
                const Ogre::Matrix4 local = invWorld.concatenateAffine(world);
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col) palette.push_back(float(local[row][col]));
            }
            jobs.push_back(r);
            maxVerts = std::max(maxVerts, r.vertexCount);
        }

        if (!jobs.empty()) {
            // 4. THE INPUTS, grown by doubling and uploaded (Ogre's staging copy,
            //    ordered in the command stream before the dispatch that reads them).
            const uint32_t paletteRows = uint32_t(palette.size() / 4u);
            if (paletteRows > sa.skinPaletteCap) {
                uint32_t cap = std::max(sa.skinPaletteCap, 1024u);
                while (cap < paletteRows) cap *= 2u;
                if (sa.skinPalette) vao->destroyUavBuffer(sa.skinPalette);
                sa.skinPalette = vao->createUavBuffer(cap, 4u * sizeof(float), 0, nullptr, false);
                sa.skinPaletteCap = cap;
            }
            if (uint32_t(jobs.size()) > sa.skinJobCap) {
                uint32_t cap = std::max(sa.skinJobCap, 8u);
                while (cap < uint32_t(jobs.size())) cap *= 2u;
                if (sa.skinJobs) vao->destroyUavBuffer(sa.skinJobs);
                sa.skinJobs = vao->createUavBuffer(cap, sizeof(SkinJobRecord), 0, nullptr, false);
                sa.skinJobCap = cap;
            }
            sa.skinPalette->upload(palette.data(), 0, paletteRows);
            sa.skinJobs->upload(jobs.data(), 0, jobs.size());

            // 5. WRITE-AFTER-READ, explicitly: last frame's readers of these very
            //    caches (the skinned builds, and the ray jobs' hit decode through
            //    the rows) are in earlier submissions on this queue; the job below
            //    must not overwrite what they have not read. Ogre's barrier solver
            //    cannot see a buffer written through a device address (F5's lesson).
            cmd = frameCmd();
            if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qBase + 4u);
            VkMemoryBarrier war{};
            war.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            war.srcAccessMask = 0;
            war.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &war, 0, nullptr, 0,
                                 nullptr);

            // 6. THE DISPATCH: x = vertices, y = items. The geometry table is
            //    re-read immediately before binding (a grow re-creates it — the
            //    ATOM-VOXEL-2 trap) and every binding is dropped afterwards (the job
            //    outlives the buffers a later grow destroys).
            JAH_TRY {
                job->_setUavBuffer(0u, skinSlot(sa.skinJobs, Ogre::ResourceAccess::Read));
                job->_setUavBuffer(1u, skinSlot(gs.geomBuffer(), Ogre::ResourceAccess::Read));
                job->_setUavBuffer(2u, skinSlot(sa.skinPalette, Ogre::ResourceAccess::Read));
                job->setNumThreadGroups((maxVerts + kSkinThreadsPerGroup - 1u) / kSkinThreadsPerGroup,
                                        uint32_t(jobs.size()), 1u);
                Ogre::ResourceTransitionArray &rt =
                    mRs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
                job->analyzeBarriers(rt);
                mRs->executeResourceTransition(rt);
                hc->dispatch(job, nullptr, nullptr);
                job->clearUavBuffers();
            }
            catch (Ogre::Exception &e) {
                job->clearUavBuffers();
                err = "skin cache: " + e.getFullDescription();
                sa.st.skinReason = err;
                cmd = frameCmd();
                return false;
            }

            // 7. PREMISE 2: the job's writes through the cache's device address are
            //    invisible to Ogre, so the edge to every reader is OURS — the
            //    skinned structures' builds below (vertex input to an AS build is
            //    SHADER_READ at the build stage) and the ray jobs' hit decode later
            //    this frame (compute). No VERTEX_INPUT edge: nothing draws the cache.
            cmd = frameCmd();
            VkMemoryBarrier raw{};
            raw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            raw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            raw.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &raw, 0, nullptr, 0, nullptr);
            if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qBase + 5u);
            ++sa.st.skinDispatches;
            for (const Dirty &d : dirty) {
                if (!d.sk) continue;
                d.sk->skinned = true;
                d.sk->poseSerial = skinPoseSerial(scene, *d.n);
                ++sa.st.skinPasses;
                ++sa.st.skinLastItems;
                sa.st.skinLastVertices += d.sk->buf.vertexCount;
            }
        }

        // 8. THE STRUCTURES: built once (PREFER_FAST_TRACE | ALLOW_UPDATE) and
        //    REFIT in place on every later pose change — one batch, each with its
        //    own scratch so no two alias. A refit keeps the address, so the
        //    instance the gather writes needs nothing else.
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
        std::vector<VkAccelerationStructureGeometryKHR> geoms;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        builds.reserve(dirty.size());
        geoms.reserve(dirty.size());
        ranges.reserve(dirty.size());
        for (const Dirty &d : dirty) {
            if (!d.sk || !d.sk->skinned) continue;
            SceneAs::Skin &sk = *d.sk;
            Ogre::VertexArrayObject *vaoSrc =
                sk.item->getMesh()->getSubMesh(0)->mVao[Ogre::VpNormal][0];
            Ogre::IndexBufferPacked *ib = vaoSrc->getIndexBuffer();
            Ogre::VulkanBufferInterface *ibi =
                static_cast<Ogre::VulkanBufferInterface *>(ib->getBufferInterface());
            const uint32_t tris = uint32_t(vaoSrc->getPrimitiveCount() / 3u);
            if (!ibi || !tris) continue;
            VkAccelerationStructureGeometryKHR g{};
            g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            g.geometry.triangles.vertexData.deviceAddress = VkDeviceAddress(sk.buf.address);
            g.geometry.triangles.vertexStride = kSkinCacheStride;
            g.geometry.triangles.maxVertex = sk.buf.vertexCount - 1u;
            g.geometry.triangles.indexType = ib->getIndexType() == Ogre::IndexBufferPacked::IT_16BIT
                                                 ? VK_INDEX_TYPE_UINT16
                                                 : VK_INDEX_TYPE_UINT32;
            g.geometry.triangles.indexData.deviceAddress =
                addressOf(ibi->getVboName()) +
                VkDeviceSize(ib->_getFinalBufferStart()) * ib->getBytesPerElement() +
                VkDeviceSize(vaoSrc->getPrimitiveStart()) * ib->getBytesPerElement();
            VkAccelerationStructureBuildGeometryInfoKHR b{};
            b.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            b.geometryCount = 1;
            if (!sk.as) {
                VkAccelerationStructureBuildSizesInfoKHR sizes{};
                sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
                b.pGeometries = &g;
                mFn.getBuildSizes(mVk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &tris, &sizes);
                const VkDeviceSize align = std::max<VkDeviceSize>(
                    mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
                if (!makeBuffer(sizes.accelerationStructureSize,
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true,
                                sk.storage, err) ||
                    !makeBuffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize) + align,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, sk.scratch, err)) {
                    dropBuffer(sk.storage);
                    dropBuffer(sk.scratch);
                    continue;
                }
                VkAccelerationStructureCreateInfoKHR ci{};
                ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
                ci.buffer = sk.storage.buffer;
                ci.size = sizes.accelerationStructureSize;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &sk.as) != VK_SUCCESS) {
                    sk.as = VK_NULL_HANDLE;
                    dropBuffer(sk.storage);
                    dropBuffer(sk.scratch);
                    err = "rayquery: vkCreateAccelerationStructureKHR (skinned BLAS) failed";
                    continue;
                }
                VkAccelerationStructureDeviceAddressInfoKHR ai{};
                ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                ai.accelerationStructure = sk.as;
                sk.address = mFn.getAsDeviceAddress(mVk, &ai);
                sk.built = false;
            }
            const VkDeviceSize align = std::max<VkDeviceSize>(
                mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
            b.mode = sk.built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                              : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            b.srcAccelerationStructure = sk.built ? sk.as : VK_NULL_HANDLE;
            b.dstAccelerationStructure = sk.as;
            b.scratchData.deviceAddress = (addressOf(sk.scratch.buffer) + align - 1) / align * align;
            if (sk.built) ++sa.st.skinRefits; else ++sa.st.skinBlasBuilds;
            sk.built = true;
            sk.triangles = tris;
            geoms.push_back(g);
            VkAccelerationStructureBuildRangeInfoKHR r{};
            r.primitiveCount = tris;
            ranges.push_back(r);
            builds.push_back(b);
        }
        if (!builds.empty()) {
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs;
            for (size_t i = 0; i < builds.size(); ++i) {
                builds[i].pGeometries = &geoms[i];
                rangePtrs.push_back(&ranges[i]);
            }
            // Last frame's build or refit of the same structure (and the traces
            // that read it) before this one rewrites it in place.
            VkMemoryBarrier pre{};
            pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            pre.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            pre.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &pre, 0,
                                 nullptr, 0, nullptr);
            mFn.cmdBuild(cmd, uint32_t(builds.size()), builds.data(), rangePtrs.data());
            // BUILD -> the TLAS build that references them (buildTlas' own pre
            // barrier covers it too; this one is the skinned structures' own).
            VkMemoryBarrier post{};
            post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            post.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            post.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &post, 0, nullptr, 0, nullptr);
        }
        if (timed && sa.st.skinLastItems > 0)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qBase + 6u);
    }

    // 9. WHAT THE GATHER MAY WRITE: the entries skinned AND built.
    for (auto &kv : sa.skins) {
        const SceneAs::Skin &sk = kv.second;
        if (sk.seen && sk.skinned && sk.built && sk.as && sk.address)
            sa.skinUse.emplace(kv.first, std::make_pair(sk.address, sk.row));
    }
    return true;
}

// ---------------------------------------------------------------------------
/// Reads back the timestamp pairs a frame old enough to have finished wrote —
/// with the availability bit, NEVER with a wait.
void RayQueryTier::readTimestamps(SceneAs &sa) {
    if (!mTimestamps || !sa.hasQueryBase) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        SceneAs::PendingTimes &p = sa.pending[i];
        if (!p.live || uint32_t(now - p.frame) < inFlight) continue;
        uint64_t data[kQueriesPerFrame * 2] = {};   // value + availability per query
        const VkResult r = vkGetQueryPoolResults(
            mVk, mTimestamps, sa.queryBase + i * kQueriesPerFrame, kQueriesPerFrame, sizeof(data),
            data, 2 * sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r != VK_SUCCESS && r != VK_NOT_READY) { p.live = false; continue; }
        auto span = [&](unsigned a, unsigned b, float &out) {
            if (!data[a * 2 + 1] || !data[b * 2 + 1]) return;        // not available
            if (data[b * 2] <= data[a * 2]) return;
            out = float(double(data[b * 2] - data[a * 2]) * double(mTimestampPeriod) / 1.0e6);
        };
        if (p.blas) span(0, 1, sa.st.blasMs);
        if (p.tlas) span(2, 3, sa.st.tlasMs);
        if (p.skin) {
            span(4, 5, sa.st.skinMs);
            span(5, 6, sa.st.skinRefitMs);
        }
        p.live = false;
    }
}

// ---------------------------------------------------------------------------
// THE FRAME'S UPDATE. Everything here is recorded into Ogre's own command
// buffer; nothing is submitted and nothing waits.
void RayQueryTier::updateScene(OgreScene *scene) {
    if (!isOpen() || !scene) return;
    SceneAs &sa = asFor(scene);
    sa.st.available = true;
    sa.st.enabled = true;

    // THE GATE. The caster walk's epoch — the host's transform-write counter
    // plus the engine's own writes plus the pushed shape events — is exactly
    // "could anything a traced instance depends on have changed?". A still
    // frame therefore records nothing at all, which is what makes a 60 Hz
    // editor with an alive TLAS cost nothing at rest.
    //
    // ...PLUS THE RAY RULE'S REFITS (ATOM-FARBLAS-1): the near copy's level is
    // the rule's answer, and the rule moves with the CAMERA, which is not scene
    // movement — so a still scene whose eye crossed a 2x band must still write
    // its instances once. Both counters only ever grow, so their sum is an
    // epoch too.
    // THE HIT DECODE'S DRAWS (PHOTON-HIT-SHADE-1): a decode twin for every PBS
    // datablock the scene's items wear, and one decode draw per twin in this
    // scene — made HERE, outside the compositor, when the set of material words
    // (GpuInstance::raster x) or HlmsAtom's twin epoch moved. A scan of the GPU
    // scene's mirror only when its slot writes moved.
    {
        const detail::GpuScene &gsc = scene->gpuScene();
        Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
        auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
        // A TWIN IS A COPY, AND A COPY DRIFTS: the engine edits a PBS datablock in
        // place (a texture bound or streamed in, the sky's reflection cube taken
        // away when a probe grid binds, a flag), and the twin made from it earlier
        // would shade the old permutation — measured in the app: a twin still
        // holding the sky cube as its manual reflection under an automatic probe
        // array, a shader that cannot compile. Ogre re-hashes every renderable a
        // datablock changes the permutation of (HlmsDatablock::flushRenderables),
        // so one item wearing each word is the witness: its hash or its datablock
        // moved -> the twin dies (forgetDecodeTwinOf; the epoch moves) and the next
        // sync re-derives it. A SAME-SLOT TEXTURE SWAP keeps the hash (the property
        // vector is unchanged) and the twin would keep the old texture in every hit:
        // the witness carries the datablock's texture set too (audit F6). One hash
        // compare and one texture-set key per material per frame.
        if (atom && gsc.live()) {
            for (auto &w : sa.decodeWitness) {
                if (w.slot >= scene->mItemNodes.size()) continue;
                const OgreScene::Node *nd = scene->mItemNodes[w.slot];
                if (!nd || !nd->item || !nd->item->getNumSubItems()) continue;
                const Ogre::SubItem *sub = nd->item->getSubItem(0);
                const Ogre::HlmsDatablock *db = sub->getDatablock();
                const Ogre::uint32 h = sub->getHlmsHash();
                const uint64_t tk = HlmsAtom::textureSetKeyOf(db);
                if (db == w.db && (h != w.hash || tk != w.texKey) && db) atom->forgetDecodeTwinOf(db);
                w.db = db;
                w.hash = h;
                w.texKey = tk;
            }
        }
        if (atom && gsc.live() &&
            (gsc.writes() != sa.decodeWrites || atom->twinEpoch() != sa.decodeEpoch)) {
            sa.decodeWrites = gsc.writes();
            std::vector<uint32_t> words;
            std::vector<SceneAs::DecodeWitness> witness;
            const detail::GpuInstance *m = gsc.mirrorData();
            for (uint32_t i = 0, n = gsc.slotCount(); i < n; ++i)
                if (m[i].raster[0] != HlmsAtom::kNoMaterialWord) words.push_back(m[i].raster[0]);
            std::sort(words.begin(), words.end());
            words.erase(std::unique(words.begin(), words.end()), words.end());
            if (words != sa.decodeWords || atom->twinEpoch() != sa.decodeEpoch) {
                sa.decodeWords = words;
                atom->syncSceneDecodes(scene->mSceneMgr, words);
                sa.decodeEpoch = atom->twinEpoch();
                // ...and the witnesses: the first slot wearing each word.
                std::vector<uint32_t> seen;
                for (uint32_t i = 0, n = gsc.slotCount(); i < n; ++i) {
                    const uint32_t w = m[i].raster[0];
                    if (w == HlmsAtom::kNoMaterialWord) continue;
                    if (std::find(seen.begin(), seen.end(), w) != seen.end()) continue;
                    seen.push_back(w);
                    SceneAs::DecodeWitness dw;
                    dw.slot = i;
                    if (i < scene->mItemNodes.size() && scene->mItemNodes[i] && scene->mItemNodes[i]->item &&
                        scene->mItemNodes[i]->item->getNumSubItems()) {
                        const Ogre::SubItem *sub = scene->mItemNodes[i]->item->getSubItem(0);
                        dw.db = sub->getDatablock();
                        dw.hash = sub->getHlmsHash();
                        dw.texKey = HlmsAtom::textureSetKeyOf(dw.db);
                    }
                    witness.push_back(dw);
                }
                sa.decodeWitness.swap(witness);
            }
        }
    }
    const unsigned long long epoch = scene->shadowEpoch() + scene->rayLevelRefits();
    const bool moved = !sa.haveEpoch || epoch != sa.lastEpoch;
    // THIS SCENE'S OWN timestamp range, handed out once. Past the budget a
    // scene simply reports no GPU milliseconds — a thumbnail scene's timings
    // are worth nothing and a missing number is better than a wrong one.
    if (!sa.hasQueryBase) {
        for (unsigned slot = 0; slot < kMaxTimedScenes; ++slot) {
            if (mTimedSceneSlots & (uint32_t(1) << slot)) continue;
            mTimedSceneSlots |= uint32_t(1) << slot;
            sa.querySlot = slot;
            sa.queryBase = slot * kFramesInFlight * kQueriesPerFrame;
            sa.hasQueryBase = true;
            break;
        }
        // Past the budget a scene simply reports no GPU milliseconds. A
        // thumbnail's timings are worth nothing and a missing number is better
        // than one taken from somebody else's range.
    }
    readTimestamps(sa);
    drainRetired();
    bool compactionPending = false;
    for (const Blas &bl : sa.blas)
        if (bl.compactState == 1u) { compactionPending = true; break; }
    if (!moved && sa.tlas && !compactionPending) return;

    // COMPACTION FIRST, BEFORE THE GATHER. It REPLACES a bottom-level
    // structure's device address, and the gather writes those addresses into
    // the instance array — so running it after the gather (as this did) built a
    // top-level structure pointing at the structures compaction had just
    // retired. They stayed alive for the retire window and were then freed
    // under a TLAS that a still scene never rebuilds: a device loss a few
    // frames after any compaction, which is exactly what two drawn scenes made
    // reproducible (round 2, cases 7/8).
    //
    // Run here, the addresses are already the new ones when the gather reads
    // them, and compaction's own `setSignature = 0` forces the full rebuild
    // that publishes them.
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    const bool didCompact = runCompaction(sa, cmd);

    // A STILL SCENE WITH NOTHING TO PUBLISH STOPS HERE (round 3, finding 5).
    // `compactionPending` above is true for every frame between a BLAS batch
    // and its size answer, so a still scene ran the WHOLE gather and a full
    // TLAS rebuild once per in-flight frame for nothing — two wasted passes
    // over every instance after each batch. It only has work if compaction
    // actually replaced an address.
    if (!moved && sa.tlas && !didCompact) return;

    // --- the gather, straight into this frame's instance slot ---------------
    // gatherMs MEASURES THE GATHER (finding 14). It used to span everything
    // from here to the end of command recording — the BLAS descriptions, the
    // buffer creation, the compaction pass — and was read as "the cost of the
    // instance walk", which it was not. Only the walk is timed now; the rest is
    // GPU-side work whose cost the timestamps report.
    double gatherMs = 0.0;
    sa.slot = (sa.slot + 1u) % kFramesInFlight;
    const uint32_t frame = frameNow();

    // THIS FRAME'S TIMESTAMP RANGE, reset once and before anything writes into
    // it: the skin pass below writes its three before the gather's four.
    const bool timed = mTimestamps && sa.hasQueryBase;
    const unsigned ring = frame % kFramesInFlight;
    const unsigned qBase = sa.queryBase + ring * kQueriesPerFrame;
    if (timed) vkCmdResetQueryPool(cmd, mTimestamps, qBase, kQueriesPerFrame);
    SceneAs::PendingTimes &pend = sa.pending[ring];
    pend = SceneAs::PendingTimes();
    pend.frame = frame;
    pend.live = timed;

    std::string err;
    // THE SKIN CACHE FIRST (PHOTON-SKIN-1): the gather writes the rigged items'
    // structure addresses, so those structures must exist — and be built or
    // refit over THIS frame's pose — before it runs. The bones are current here:
    // updateSceneGraph has run (updateAllAnimations) and nothing has rendered.
    {
        monitor::CacheScope scope(CacheKind::Gi, WorkReason::Moved, 0, "rq.skin", mRs);
        const unsigned long long before = sa.st.skinPasses;
        const Clock::time_point tSkin = Clock::now();
        if (!skinPass(scene, sa, cmd, timed, qBase, err) && !err.empty())
            Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
        if (sa.st.skinPasses != before) sa.st.skinCpuMs = float(msSince(tSkin));
        pend.skin = sa.st.skinPasses != before;
        scope.setUnits(unsigned(sa.st.skinPasses - before));
        if (sa.st.skinPasses == before) scope.cancel();
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        InstanceWriter w;
        w.blasOf = &sa.blasOf;
        w.coarseBound = &sa.coarseBound;
        w.geomRowOfSlot = &sa.geomRowOfSlot;
        w.skinUse = &sa.skinUse;
        std::vector<VkDeviceAddress> addresses;
        addresses.reserve(sa.blas.size());
        for (const Blas &bl : sa.blas) addresses.push_back(bl.address);
        w.blasAddress = &addresses;
        w.capacity = sa.instanceCapacity;
        w.wantSignature = preferRefit();
        std::vector<unsigned char> seen(sa.blas.size(), 0u);
        w.seen = &seen;
        w.dst = sa.instances.mapped
                    ? static_cast<VkAccelerationStructureInstanceKHR *>(sa.instances.mapped) +
                          size_t(sa.slot) * sa.instanceCapacity
                    : nullptr;
        const Clock::time_point tGather = Clock::now();
        writeRayInstances(scene, w);
        gatherMs += msSince(tGather);

        if (w.count > sa.instanceCapacity || !sa.instances.mapped) {
            if (attempt == 1) { sa.st.enabled = false; return; }
            // GROW. One buffer, kFramesInFlight slots: the gather writes into
            // the slot this frame builds from, so a transform never passes
            // through an intermediate vector and the GPU is never reading the
            // slot being written.
            const unsigned want = std::max<unsigned>(64u, w.count + w.count / 2u + 16u);
            // RETIRED, not waited on (finding 6): a TLAS build queued last
            // frame still reads the old array. This runs on every scene's FIRST
            // frame, so a device wait here was a stall every scene paid.
            retire(sa.instances);
            if (!makeBuffer(VkDeviceSize(want) * kFramesInFlight *
                                sizeof(VkAccelerationStructureInstanceKHR),
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                            true, true, sa.instances, err)) {
                Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
                sa.st.enabled = false;
                return;
            }
            sa.instanceCapacity = want;
            sa.setSignature = 0;
            continue;
        }

        unsigned built = 0;
        if (!w.newBlas.empty()) {
            if (timed)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qBase + 0);
            monitor::CacheScope scope(CacheKind::Gi, WorkReason::Added, 0, "rq.blas", mRs);
            if (!ensureBlas(sa, w.newBlas, cmd, built, err)) {
                Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
                scope.cancel();
                // The TLAS goes with the failure: compaction may already have
                // retired structures it references, and a scene that stops
                // moving would keep that half-built tree for ever (finding 6).
                retire(sa.tlas, sa.tlasStorage);
                sa.setSignature = 0;
                sa.st.enabled = false;
                return;
            }
            scope.setUnits(built);
            if (timed)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps,
                                    qBase + 1);
            pend.blas = built != 0u;
            sa.st.blasBuilds += built;
            // Patch the instances that referenced a structure that did not
            // exist when they were written.
            for (const InstanceWriter::Patch &p : w.patches) {
                const BlasWant &want = w.newBlas[p.newBlas];
                auto it = sa.blasOf.find(BlasKey{ want.mesh.get(), want.level });
                if (it == sa.blasOf.end()) continue;
                w.dst[p.instance].accelerationStructureReference = sa.blas[it->second].address;
            }
        }
        sa.instanceCount = w.count;
        sa.farInstanceCount = w.farCount;
        sa.st.skinnedInstances = int(w.skinned);
        sa.farOverlap = w.maxCoarseBound;
        // REBUILD IS THE DEFAULT, refit the optimisation (NVIDIA's own guidance
        // for a TLAS: "consider PREFER_FAST_TRACE and perform only rebuilds").
        // A refit is only ever taken when the SET is identical — same
        // instances, same structures, only transforms moved — which is exactly
        // what the signature says, and only when the tuning asks for it.
        const bool sameSet = (sa.setSignature == w.signature) && sa.tlas;
        const bool refit = preferRefit() && sameSet;
        sa.setSignature = w.signature;
        if (timed)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qBase + 2);
        {
            monitor::CacheScope scope(CacheKind::Gi, refit ? WorkReason::Moved : WorkReason::Rebuild,
                                      0, "rq.tlas", mRs);
            if (!buildTlas(sa, cmd, refit, err)) {
                Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
                scope.cancel();
                // The TLAS goes with the failure: compaction may already have
                // retired structures it references, and a scene that stops
                // moving would keep that half-built tree for ever (finding 6).
                retire(sa.tlas, sa.tlasStorage);
                sa.setSignature = 0;
                sa.st.enabled = false;
                return;
            }
            // THE TRACED SET, not the TLAS's 2N: a per-unit cost that halved
            // silently when every object gained a far copy would be a lie (F6).
            // The far copies are their own row's units, beside it.
            scope.setUnits(sa.instanceCount - sa.farInstanceCount);
        }
        {
            // Zero-width by construction — the far copies are built by the SAME
            // command as the near ones, so this row carries their COUNT and no
            // time of its own.
            monitor::CacheScope far(CacheKind::Gi, refit ? WorkReason::Moved : WorkReason::Rebuild,
                                    0, "rq.tlas.far", nullptr);
            far.setUnits(sa.farInstanceCount);
        }
        if (timed)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qBase + 3);
        pend.tlas = true;
        for (size_t i = 0; i < seen.size() && i < sa.blas.size(); ++i)
            if (seen[i]) sa.blas[i].lastSeen = frame;
        break;
    }

    sa.lastEpoch = epoch;
    sa.haveEpoch = true;
    evictStaleBlas(sa);
    // A RIGGED ITEM THAT LEFT THE TRACED SET gives its cache back HERE, after the
    // top-level structure this frame built without it — never earlier: the TLAS
    // a still scene keeps is the one that last referenced it (evictStaleBlas'
    // rule, and its reason).
    for (auto it = sa.skins.begin(); it != sa.skins.end();) {
        if (it->second.seen) { ++it; continue; }
        dropSkin(scene, sa, it->first, it->second);
        it = sa.skins.erase(it);
    }
    sa.st.gatherMs = float(gatherMs);
    // LIVE structures only: an evicted slot keeps its place in the vector (the
    // index is referenced by the table) but holds nothing (F6).
    sa.st.blasCount = 0;
    for (const Blas &bl : sa.blas)
        if (bl.as) ++sa.st.blasCount;
    // THE TRACED SET is the near copies; the far copies are the same objects
    // again (A5b §4), counted apart so a reader can tell the two halves.
    sa.st.instances = int(sa.instanceCount - sa.farInstanceCount);
    sa.st.farInstances = int(sa.farInstanceCount);
    sa.st.triangles = 0;
    sa.st.blasBytes = 0;
    sa.st.levelBlasCount = 0;
    sa.st.levelBlasBytes = 0;
    for (const Blas &bl : sa.blas) {
        sa.st.triangles += int(bl.triangles);
        sa.st.blasBytes += bl.storage.size;
        if (bl.as && bl.level > 0u) {
            ++sa.st.levelBlasCount;
            sa.st.levelBlasBytes += bl.storage.size;
        }
    }
    // THE SKINNED STRUCTURES are per ITEM and counted apart as well as in the
    // totals: their bytes and their caches are what a character costs.
    sa.st.skinCaches = int(sa.skins.size());
    sa.st.skinBlasBytes = 0;
    sa.st.skinCacheBytes = 0;
    for (const auto &kv : sa.skins) {
        const SceneAs::Skin &sk = kv.second;
        if (sk.as) {
            ++sa.st.blasCount;
            sa.st.triangles += int(sk.triangles);
            sa.st.blasBytes += sk.storage.size;
            sa.st.skinBlasBytes += sk.storage.size;
        }
        sa.st.skinCacheBytes += (unsigned long long)sk.buf.vertexCount * kSkinCacheStride;
    }
}

RayQueryStatus RayQueryTier::status(const OgreScene *scene) const {
    auto it = mScenes.find(const_cast<OgreScene *>(scene));
    if (it == mScenes.end()) {
        // A scene the tier has not seen yet (nothing has drawn it). `enabled`
        // is the SWITCH's answer, not "false because there is no structure":
        // the tier being open at all means the switch is on, and reporting
        // false here made a fresh scene indistinguishable from a machine with
        // rays switched off (round 2, finding 18).
        RayQueryStatus st;
        st.available = isOpen();
        st.enabled = isOpen();
        return st;
    }
    return it->second.st;
}

// ---------------------------------------------------------------------------
// THE TOOL/TEST TRACE. The ONLY place in this file that submits its own command
// buffer and waits — and it is not a frame path: it exists so a suite can ask
// "what does this ray hit?" and get the answer in the same statement. Every
// product consumer (R2 probe visibility, R3 sun contact, R5 reflections) reads
// the same structures from a compute pass recorded INSIDE the frame.
bool RayQueryTier::traceBlocking(OgreScene *scene, const std::vector<float> &rays,
                                 std::vector<float> &hits, std::string &err) {
    hits.clear();
    if (!isOpen()) { err = "rayquery: the tier is not open"; return false; }
    auto it = mScenes.find(scene);
    if (it == mScenes.end() || !it->second.tlas || it->second.instanceCount == 0u) {
        err = "rayquery: this scene has no acceleration structure yet (render a frame first)";
        return false;
    }
    // A DISABLED SCENE'S STRUCTURE IS NOT TRUSTWORTHY (round 3, finding 6).
    // `enabled` goes false when a frame's build FAILED, and the structure it
    // left behind can reference bottom-level structures compaction retired in
    // the same frame. Refusing is the honest answer; tracing it would return
    // confident nonsense, or lose the device.
    if (!it->second.st.enabled) {
        err = "rayquery: this scene's structure is stale (the last update failed)";
        return false;
    }
    SceneAs &sa = it->second;
    const size_t count = rays.size() / 12u;      // 3 vec4s per ray
    if (!count) { err = "rayquery: an empty ray batch"; return false; }

    // EVERYTHING OGRE HAS RECORDED GOES FIRST. The acceleration structures were
    // built into the frame's command buffer; flushing submits it and waits, so
    // what follows cannot read a structure that has not been built.
    mRs->flushCommands();

    RawBuffer rayBuf, hitBuf, ubo;
    if (!makeBuffer(rays.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false,
                    rayBuf, err))
        return false;
    memcpy(rayBuf.mapped, rays.data(), rays.size() * sizeof(float));
    if (!makeBuffer(count * 4u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false,
                    hitBuf, err)) {
        dropBuffer(rayBuf);
        return false;
    }
    struct Params { uint32_t counts[4] = {}; } params{};
    params.counts[0] = uint32_t(count);
    if (!makeBuffer(sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, ubo, err)) {
        dropBuffer(rayBuf);
        dropBuffer(hitBuf);
        return false;
    }
    memcpy(ubo.mapped, &params, sizeof(params));

    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = mDescPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &mSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(mVk, &dai, &set) != VK_SUCCESS) {
        err = "rayquery: vkAllocateDescriptorSets failed";
        dropBuffer(rayBuf); dropBuffer(hitBuf); dropBuffer(ubo);
        return false;
    }
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo rb{}, hb{}, ub{};
    rb.buffer = rayBuf.buffer; rb.range = VK_WHOLE_SIZE;
    hb.buffer = hitBuf.buffer; hb.range = VK_WHOLE_SIZE;
    ub.buffer = ubo.buffer;    ub.range = sizeof(params);
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &rb;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &hb;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &ub;
    vkUpdateDescriptorSets(mVk, 4, writes, 0, nullptr);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = mDev->mGraphicsQueue.getFamilyIdx();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(mVk, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(mVk, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeLayout, 0, 1, &set, 0,
                            nullptr);
    vkCmdDispatch(cmd, uint32_t((count + 63u) / 64u), 1, 1);
    // THE HIT BUFFER IS READ BY THE HOST as soon as the fence signals, and a
    // fence does not make a shader's writes visible to the CPU by itself
    // (finding 12). The memory is HOST_COHERENT, so no invalidate is needed —
    // but the availability operation is, and this is it.
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &toHost, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(mVk, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(mDev->mGraphicsQueue.mQueue, 1, &si, fence) == VK_SUCCESS;
    // NEVER WAIT ON A FENCE WHOSE SUBMIT FAILED — that is an infinite hang.
    // The WAIT's result matters as much as the submit's: on a lost device it
    // returns VK_ERROR_DEVICE_LOST and the hit buffer holds nothing. Reporting
    // success there would hand a caller a batch of zeros as if they were
    // answers.
    if (ok) ok = vkWaitForFences(mVk, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (ok) {
        hits.resize(count * 4u);
        memcpy(hits.data(), hitBuf.mapped, hits.size() * sizeof(float));
    } else {
        err = "rayquery: the trace did not complete (submit or device-lost wait failed)";
    }
    vkDestroyFence(mVk, fence, nullptr);
    vkFreeCommandBuffers(mVk, pool, 1, &cmd);
    vkDestroyCommandPool(mVk, pool, nullptr);
    vkFreeDescriptorSets(mVk, mDescPool, 1, &set);
    dropBuffer(rayBuf);
    dropBuffer(hitBuf);
    dropBuffer(ubo);
    return ok;
}

// gi.card_read_parity — the reflection's card read, asked directly
// ---------------------------------------------------------------------------
// The same include the trace reads (jah_rq_card.glsl) behind the same four
// bindings (jah_rq_card_bindings.glsl, here at base 2), dispatched once over a
// list of points on its own command buffer, the picks read back. The suite
// holds them against SurfaceCache::readAt, the CPU reference.
bool RayQueryTier::cardPickBlocking(OgreScene *scene, const std::vector<CardReadQuery> &queries,
                                    std::vector<CardReadPick> &out, std::string &err) {
    out.clear();
    if (!isOpen()) { err = "cardReadParity: the ray tier is not open"; return false; }
    const SurfaceCache *cache = scene ? scene->mSurfaceCache.get() : nullptr;
    if (!cache || !cache->cardBuffer() || !cache->instanceBuffer() || !cache->depthLayer() ||
        !cache->radianceLayer() || !cache->cardRecords()) {
        err = "cardReadParity: the scene holds no built surface cache";
        return false;
    }
    if (queries.empty()) { err = "cardReadParity: no queries"; return false; }

    // ---- the harness pipeline, once ------------------------------------------
    if (!mCardParityPipeline) {
        const VkDescriptorType types[10] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER };
        VkDescriptorSetLayoutBinding b[10] = {};
        for (unsigned i = 0; i < 10u; ++i) {
            b[i].binding = i;
            b[i].descriptorType = types[i];
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = 10;
        sli.pBindings = b;
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &mCardParitySetLayout;
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = sizeof(krq_cardParitySpv);
        smi.pCode = krq_cardParitySpv;
        VkDescriptorPoolSize sizes[4] = {};
        sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[0].descriptorCount = 6;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = 2;
        sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[2].descriptorCount = 1;
        sizes[3].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        sizes[3].descriptorCount = 1;
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpi.maxSets = 1;
        dpi.poolSizeCount = 4;
        dpi.pPoolSizes = sizes;
        if (vkCreateDescriptorSetLayout(mVk, &sli, nullptr, &mCardParitySetLayout) != VK_SUCCESS ||
            vkCreatePipelineLayout(mVk, &pli, nullptr, &mCardParityPipeLayout) != VK_SUCCESS ||
            vkCreateShaderModule(mVk, &smi, nullptr, &mCardParityModule) != VK_SUCCESS ||
            vkCreateDescriptorPool(mVk, &dpi, nullptr, &mCardParityPool) != VK_SUCCESS) {
            err = "cardReadParity: the harness pipeline could not be made";
            return false;
        }
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = mCardParityModule;
        cpi.stage.pName = "main";
        cpi.layout = mCardParityPipeLayout;
        if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &cpi, nullptr, &mCardParityPipeline) !=
            VK_SUCCESS) {
            err = "cardReadParity: vkCreateComputePipelines failed";
            return false;
        }
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(mVk, &si, nullptr, &mCardParitySampler) != VK_SUCCESS) {
            err = "cardReadParity: vkCreateSampler failed";
            return false;
        }
    }

    // ---- THE INPUTS IN THEIR READ LAYOUTS, AND EVERYTHING OGRE RECORDED FIRST.
    // The atlas's Depth layer and the Radiance UAV as textures, the two tables
    // as buffers — through Ogre's own solver (its bookkeeping stays true: the
    // harness changes no layout) — then the flush that submits the captures,
    // the relight and the table uploads ahead of this job.
    Ogre::UavBufferPacked *table = cache->cardBuffer();
    Ogre::UavBufferPacked *instances = cache->instanceBuffer();
    Ogre::TextureGpu *depth = cache->depthLayer();
    Ogre::TextureGpu *radiance = cache->radianceLayer();
    // THE TRACED QUESTIONS' INPUTS: the scene's TLAS and the hit-normal tables
    // the reflection binds (the per-slot row copy, the GPU scene's rows).
    auto sceneIt = mScenes.find(scene);
    SceneAs *sa = sceneIt != mScenes.end() && sceneIt->second.tlas ? &sceneIt->second : nullptr;
    detail::GpuScene &gpuScn = scene->gpuScene();
    if (gpuScn.live()) gpuScn.flushGeomRows();
    Ogre::UavBufferPacked *geomRows = gpuScn.live() ? gpuScn.geomBuffer() : nullptr;
    const uint32_t geomSlots = (sa && geomRows) ? uint32_t(sa->geomRowOfSlot.size()) : 0u;
    // The job's layout names the TLAS, so every dispatch binds one (a descriptor
    // a shader uses statically must be valid even when no question traces).
    if (!sa) {
        err = "cardReadParity: the scene has no acceleration structure yet (render a frame with rays on)";
        return false;
    }
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { depth, radiance })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        for (Ogre::UavBufferPacked *b : { table, instances })
            solver.resolveTransition(trans, b, Ogre::ResourceAccess::Read, computeStage);
        if (geomSlots) solver.resolveTransition(trans, geomRows, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    mRs->flushCommands();

    const size_t n = queries.size();
    RawBuffer qBuf, aBuf, ubo;
    if (!makeBuffer(n * 8u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, qBuf, err))
        return false;
    if (!makeBuffer(n * 16u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, aBuf, err)) {
        dropBuffer(qBuf);
        return false;
    }
    if (!makeBuffer(4u * sizeof(uint32_t), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, ubo, err)) {
        dropBuffer(qBuf);
        dropBuffer(aBuf);
        return false;
    }
    {
        float *q = static_cast<float *>(qBuf.mapped);
        for (size_t i = 0; i < n; ++i) {
            const long slot = cache->itemSlotOf(queries[i].node);
            const uint32_t slotBits = slot < 0 ? 0xFFFFFFFFu : uint32_t(slot);
            q[i * 8u + 0] = queries[i].position.x;
            q[i * 8u + 1] = queries[i].position.y;
            q[i * 8u + 2] = queries[i].position.z;
            std::memcpy(&q[i * 8u + 3], &slotBits, sizeof(slotBits));
            q[i * 8u + 4] = queries[i].facing.x;
            q[i * 8u + 5] = queries[i].facing.y;
            q[i * 8u + 6] = queries[i].facing.z;
            q[i * 8u + 7] = queries[i].trace ? 1.0f : 0.0f;
        }
        const uint32_t counts[4] = { uint32_t(n), cache->instanceSlots(), cache->cardRecords(), geomSlots };
        std::memcpy(ubo.mapped, counts, sizeof(counts));
    }

    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = mCardParityPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &mCardParitySetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(mVk, &dai, &set) != VK_SUCCESS) {
        err = "cardReadParity: vkAllocateDescriptorSets failed";
        dropBuffer(qBuf); dropBuffer(aBuf); dropBuffer(ubo);
        return false;
    }
    VkDescriptorBufferInfo bufs[4] = {};
    bufs[0].buffer = qBuf.buffer; bufs[0].range = VK_WHOLE_SIZE;
    bufs[1].buffer = aBuf.buffer; bufs[1].range = VK_WHOLE_SIZE;
    Ogre::UavBufferPacked *const tables[2] = { table, instances };
    for (int i = 0; i < 2; ++i) {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(tables[i]->getBufferInterface());
        bufs[2 + i].buffer = bi->getVboName();
        bufs[2 + i].offset = VkDeviceSize(tables[i]->_getFinalBufferStart()) *
                             tables[i]->getBytesPerElement();
        bufs[2 + i].range = tables[i]->getTotalSizeBytes();
    }
    VkImageView views[2] = {};
    VkDescriptorImageInfo imgs[2] = {};
    Ogre::TextureGpu *const layers[2] = { depth, radiance };
    for (int i = 0; i < 2; ++i) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = layers[i];
        views[i] = static_cast<Ogre::VulkanTextureGpu *>(layers[i])->createView(slot, false);
        imgs[i].sampler = mCardParitySampler;
        imgs[i].imageView = views[i];
        imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorBufferInfo ub{};
    ub.buffer = ubo.buffer;
    ub.range = 4u * sizeof(uint32_t);
    // 7-9: the TLAS (the scene's, or none when no question traces — a
    // descriptor must still be valid, so the stand-in is only for 8/9), the
    // per-slot rows (copied now), the GPU scene's rows.
    RawBuffer rowBuf;
    VkDescriptorBufferInfo geomBufs[2] = {};
    if (geomSlots &&
        makeBuffer(VkDeviceSize(geomSlots) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                   false, rowBuf, err)) {
        std::memcpy(rowBuf.mapped, sa->geomRowOfSlot.data(), size_t(geomSlots) * sizeof(uint32_t));
        geomBufs[0].buffer = rowBuf.buffer;
        geomBufs[0].range = VK_WHOLE_SIZE;
        auto *gbi = static_cast<Ogre::VulkanBufferInterface *>(geomRows->getBufferInterface());
        geomBufs[1].buffer = gbi->getVboName();
        geomBufs[1].offset = VkDeviceSize(geomRows->_getFinalBufferStart()) * geomRows->getBytesPerElement();
        geomBufs[1].range = geomRows->getTotalSizeBytes();
    } else {
        if (!ensureDummyImages(err)) { dropBuffer(qBuf); dropBuffer(aBuf); dropBuffer(ubo); return false; }
        for (VkDescriptorBufferInfo &g : geomBufs) { g.buffer = mDummyStorage.buffer; g.range = VK_WHOLE_SIZE; }
    }
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa->tlas;
    const unsigned nWrites = 10u;
    VkWriteDescriptorSet writes[10] = {};
    for (int i = 0; i < 10; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    for (int i = 0; i < 4; ++i) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufs[i];
    }
    for (int i = 0; i < 2; ++i) {
        writes[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4 + i].pImageInfo = &imgs[i];
    }
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[6].pBufferInfo = &ub;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[7].pNext = &asWrite;
    for (int i = 0; i < 2; ++i) {
        writes[8 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[8 + i].pBufferInfo = &geomBufs[i];
    }
    vkUpdateDescriptorSets(mVk, nWrites, writes, 0, nullptr);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = mDev->mGraphicsQueue.getFamilyIdx();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(mVk, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(mVk, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCardParityPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCardParityPipeLayout, 0, 1, &set,
                            0, nullptr);
    vkCmdDispatch(cmd, uint32_t((n + 63u) / 64u), 1, 1);
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &toHost, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(mVk, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(mDev->mGraphicsQueue.mQueue, 1, &si, fence) == VK_SUCCESS;
    if (ok) ok = vkWaitForFences(mVk, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (ok) {
        out.resize(n);
        const uint32_t *a = static_cast<const uint32_t *>(aBuf.mapped);
        for (size_t i = 0; i < n; ++i) {
            CardReadPick &p = out[i];
            const uint32_t *r = &a[i * 16u];
            p.ok = (r[0] & 1u) != 0u;
            p.lit = (r[0] & 2u) != 0u;
            p.hit = (r[0] & 4u) != 0u;
            p.card = p.ok ? int(r[1]) : -1;
            p.texelX = r[2];
            p.texelY = r[3];
            std::memcpy(p.radiance, &r[4], 3u * sizeof(float));
            std::memcpy(p.hitPoint, &r[8], 3u * sizeof(float));
            std::memcpy(p.hitNormal, &r[12], 3u * sizeof(float));
        }
    } else {
        err = "cardReadParity: the job did not complete (submit or device-lost wait failed)";
    }
    vkDestroyFence(mVk, fence, nullptr);
    vkFreeCommandBuffers(mVk, pool, 1, &cmd);
    vkDestroyCommandPool(mVk, pool, nullptr);
    vkFreeDescriptorSets(mVk, mCardParityPool, 1, &set);
    for (VkImageView v : views)
        if (v) vkDestroyImageView(mVk, v, nullptr);
    dropBuffer(rowBuf);
    dropBuffer(qBuf);
    dropBuffer(aBuf);
    dropBuffer(ubo);
    return ok;
}

bool OgreEngine::cardReadParity(Scene *scene, const std::vector<CardReadQuery> &queries,
                                std::vector<CardReadPick> &out) {
    out.clear();
    if (!mRayTier || !mRayTier->isOpen()) {
        mLastError = "cardReadParity: the ray tier is not open (no ray-query device, or rays off)";
        return false;
    }
    std::string err;
    if (!mRayTier->cardPickBlocking(static_cast<OgreScene *>(scene), queries, out, err)) {
        mLastError = err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// R5 — RAY-TRACED REFLECTIONS. The consumer, in the same TU as the structures.
//
// SCREEN FIRST, RAYS FOR THE REST. The screen-space march (lanes SSR-1/SSR-2)
// keeps every pixel it is confident about — a reflected SHADED pixel of this
// scene is the sharpest and cheapest answer there is — and this pass fills only
// what it could not: off screen, behind an occluder, past its edge fade, and
// the roughness band between its cutoff and ours. The order is Lumen's and it
// is right here for a measurable reason: a ray's hit is shaded from the Photon
// voxels, whose finest DIRECTIONAL mip is half the voxel grid, so wherever the
// screen has an answer the screen's answer is better.
//
// WHERE IT SITS IN THE FRAME, and why. `passPreExecute` of the scene pass that
// SAMPLES `jahSsrReflection` (`mPrePassMode == PrePassUse` — a semantic test,
// not a pass name): the only point at which the prepass' normals and packed
// roughness exist, the resolve's own answer exists, and the texture is still
// writable. Ogre fires that listener BEFORE `analyzeBarriers`
// (OgreCompositorPassScene.cpp:235 vs :289), which is what lets the transitions
// below be resolved through Ogre's own BarrierSolver and then handed back: the
// scene pass' own analysis sees the texture in Uav and emits the Uav -> Texture
// barrier itself. Nothing here manages a layout behind Ogre's back.
//
// NO OGRE PATCH. Everything it needs is public: `getDefinedTexture` for the
// chain's textures, `createView` for the image views, `getBarrierSolver` /
// `executeResourceTransition` for the layouts, and `getCurrentCmdBuffer` for
// the recording (which patch 0040 already made linkable). The one change to the
// engine's own chain is the Uav flag on `jahSsrReflection`, which is ours.

namespace {

/// The parameter block, std140, mirroring rq_reflect.comp's `Params` member for
/// member. A mismatch here is silent and total, so the order is the shader's
/// order and nothing is reordered for packing (every member is a vec4 or an
/// array of them, which std140 lays out identically to C).
struct ReflectParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
    float knobs2[4] = {};
    float skyColour[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float voxelOrigin[kMaxReflectCascades][4] = {};
    float voxelInvSize[kMaxReflectCascades][4] = {};
    float prevCamPos[4] = {};
    float prevRayTL[4] = {};
    float prevRayRight[4] = {};
    float prevRayDown[4] = {};
    float prevFwd[4] = {};
    /// THE SECOND EYE (lane REFLECT-VR-1). `stereo.x` is 1 when the target
    /// carries two eyes side by side, and then everything above is the LEFT
    /// eye's over the left half and everything here is the RIGHT eye's over the
    /// right half. Appended rather than folded into an array of two: the block
    /// above is what a mono view writes and what every reader of this file
    /// already knows, and std140 lays the tail out identically either way.
    float stereo[4] = {};
    float camPos2[4] = {};
    float rayTL2[4] = {};
    float rayRight2[4] = {};
    float rayDown2[4] = {};
    float fwd2[4] = {};
    float prevCamPos2[4] = {};
    float prevRayTL2[4] = {};
    float prevRayRight2[4] = {};
    float prevRayDown2[4] = {};
    float prevFwd2[4] = {};
    /// THE SURFACE CACHE (PHOTON-CARDS-2): x = the instance-table entries bound
    /// (0 = no cache — the card read declines every hit), y = the card records,
    /// z = the footprint gate in card texels (kCardFootprintTexels), w = the
    /// per-slot geometry-row entries bound (0 = the hit's normal is the ray's).
    float cards[4] = {};
    /// THE HIT RECORD (PHOTON-HIT-SHADE-1; rq_reflect.comp's hitList, hitSun,
    /// hitSun2).
    float hitList[4] = {};
    float hitSun[4] = {};
    float hitSun2[4] = {};
};

/// The card read's footprint gate (Types.h kCardFootprintTexels).
/// `JAHSHAKA_CARD_FOOTPRINT_K` is a MEASUREMENT switch, not a mode: the sweep
/// that chose the constant (test_rt_reflect --footprint-sweep) sets it per arm.
float cardFootprintTexels() {
    if (const char *e = std::getenv("JAHSHAKA_CARD_FOOTPRINT_K")) return float(std::atof(e));
    return kCardFootprintTexels;
}

void put3(float *dst, const Ogre::Vector3 &v, float w) {
    dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = w;
}

}   // namespace

// TWO SAMPLERS, AND WHICH IS WHICH MATTERS. The G-buffer and the depth are read
// at exactly one texel — a linear tap across a depth discontinuity reconstructs a
// position on neither surface — so they are POINT. The voxel volumes and the sky
// cube are continuous fields and are LINEAR, with a clamped address mode so a
// sample at a volume's face does not wrap to the other side of the world.
// THE TIER'S OWN, made by whichever job needs them first (PHOTON-GATHER-1d): they
// were made by the reflection's pipeline alone, and the gather borrowed them only
// because the reflection's record ran first on every PrePassUse pass — a
// gather-only chain that no longer asks the reflection anything bound a NULL
// sampler (a driver segfault in vkUpdateDescriptorSets).
bool RayQueryTier::ensureSamplers(std::string &err) {
    if (mPointSampler && mLinearSampler) return true;
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (!mPointSampler && vkCreateSampler(mVk, &si, nullptr, &mPointSampler) != VK_SUCCESS) {
        mPointSampler = VK_NULL_HANDLE;
        err = "rayquery: vkCreateSampler (point) failed";
        return false;
    }
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (!mLinearSampler && vkCreateSampler(mVk, &si, nullptr, &mLinearSampler) != VK_SUCCESS) {
        mLinearSampler = VK_NULL_HANDLE;
        err = "rayquery: vkCreateSampler (linear) failed";
        return false;
    }
    return true;
}

bool RayQueryTier::makeReflectPipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[kReflectBindings] = {};
    const VkDescriptorType types[kReflectBindings] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0  tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1  params
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 2  normals
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3  shadow+roughness
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 4  depth
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 5  jahSsrReflection
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 6  history (read)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 7  history (write)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 8  distance (read)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 9  distance (write)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 10 voxelIso[]
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 11 voxelX[]
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 12 voxelY[]
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 13 voxelZ[]
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 14 sky cube
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 15 the card table
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 16 the card instance table
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 17 the card Depth layer
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 18 the card Radiance layer
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 19 the per-slot geometry row
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 20 the geometry rows
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 21 voxelCovP[] (PHOTON-VOXEL-4)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 22 voxelCovN[]
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 23 voxelPosP[]
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 24 voxelPosN[]
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 25 the GPU scene's instances (HIT-SHADE-1)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 26 the hit list's records
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 27 ...its destinations
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 28 ...its buffer (counters + aux)
    };
    for (unsigned i = 0; i < kReflectBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount =
            ((i >= 10u && i <= 13u) ||
             (i >= kReflectCovBinding && i < kReflectCovBinding + kReflectSplitKinds))
                ? kMaxReflectCascades : 1u;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kReflectBindings;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(mVk, &sli, nullptr, &mReflectSetLayout) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateDescriptorSetLayout failed";
        return false;
    }
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mReflectSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mReflectPipeLayout) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_reflectSpv);
    smi.pCode = krq_reflectSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mReflectModule) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateShaderModule failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mReflectModule;
    cpi.stage.pName = "main";
    cpi.layout = mReflectPipeLayout;
    if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &cpi, nullptr, &mReflectPipeline) !=
        VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateComputePipelines failed";
        return false;
    }
    {
        VkShaderModuleCreateInfo fsmi{};
        fsmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fsmi.codeSize = sizeof(krq_reflectFilterSpv);
        fsmi.pCode = krq_reflectFilterSpv;
        if (vkCreateShaderModule(mVk, &fsmi, nullptr, &mFilterModule) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateShaderModule (filter) failed";
            return false;
        }
        VkComputePipelineCreateInfo fcpi{};
        fcpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        fcpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fcpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        fcpi.stage.module = mFilterModule;
        fcpi.stage.pName = "main";
        fcpi.layout = mReflectPipeLayout;
        if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &fcpi, nullptr, &mFilterPipeline) !=
            VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateComputePipelines (filter) failed";
            return false;
        }
    }
    // Sized for kMaxTimedScenes views' worth of rings, which is the same ceiling
    // the timestamp pool uses and far more views than a product frame draws.
    const unsigned sets = kMaxTimedScenes * kReflectRing;
    VkDescriptorPoolSize sizes[5] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = sets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = sets;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[2].descriptorCount = sets * 7u;   // + the hit list's two (HIT-SHADE-1)
    sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[3].descriptorCount = sets * (3u + 8u * kMaxReflectCascades + 1u + 2u);
    sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[4].descriptorCount = sets * 6u;   // + the instances and the list's buffer
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 5;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mVk, &dpi, nullptr, &mReflectPool) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateDescriptorPool failed";
        return false;
    }
    if (!ensureSamplers(err)) return false;
    if (mTimestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxTimedScenes * kFramesInFlight * 2u;
        vkCreateQueryPool(mVk, &qci, nullptr, &mReflectTimestamps);
    }
    return true;
}

bool RayQueryTier::ensureReflectImages(ReflectView &rv, unsigned w, unsigned h,
                                       std::string &err) {
    if (rv.imagesReady && rv.w == w && rv.h == h) return true;
    // A RESIZE INVALIDATES THE MEAN, which is correct and not a loss: the
    // history is screen space, and every pixel of it now means a different
    // direction. The images are RETIRED rather than destroyed — frames that
    // still reference them are in flight.
    for (int i = 0; i < 2; ++i) {
        retireImage(rv.hist[i]);
        retireImage(rv.dist[i]);
    }
    rv.imagesReady = false;
    rv.havePrev = false;
    rv.historyFrames = 0;
    rv.w = w; rv.h = h;
    // The second image is a PAIR: x = the surface's distance (the reprojection's
    // validity test), y = the mean distance the rays in the mean travelled (what a
    // moved camera's ray is compared against — rq_reflect.comp, PAN-SMEAR-1).
    const VkFormat formats[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32_SFLOAT };
    for (int i = 0; i < 2; ++i) {
        if (!makeStorageImage(w, h, formats[0], rv.hist[i], err)) return false;
        if (!makeStorageImage(w, h, formats[1], rv.dist[i], err)) return false;
    }
    rv.imagesReady = true;
    rv.needsClear = true;
    return true;
}

void RayQueryTier::clearReflectImages(ReflectView &rv, VkCommandBuffer cmd) {
    if (!rv.needsClear) return;
    rv.needsClear = false;
    // UNDEFINED -> GENERAL, and CLEARED: a distance of zero is what
    // rq_reflect.comp reads as "no history here", so a freshly made pair is
    // rejected by the reprojection test rather than believed.
    VkImageMemoryBarrier toGeneral[4] = {};
    ReflectImage *imgs[4] = { &rv.hist[0], &rv.hist[1], &rv.dist[0], &rv.dist[1] };
    for (int i = 0; i < 4; ++i) {
        toGeneral[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral[i].srcQueueFamilyIndex = toGeneral[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral[i].image = imgs[i]->image;
        toGeneral[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral[i].subresourceRange.levelCount = 1;
        toGeneral[i].subresourceRange.layerCount = 1;
        toGeneral[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 4, toGeneral);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    for (int i = 0; i < 4; ++i)
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    VkMemoryBarrier toCompute{};
    toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toCompute, 0, nullptr, 0, nullptr);
}

void RayQueryTier::readReflectTimestamps(ReflectView &rv) {
    if (!mReflectTimestamps || !rv.hasQueryBase) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        ReflectView::Pending &pd = rv.pending[i];
        // `< inFlight`, not `<= inFlight`, and the difference is the whole
        // reading: the pending ring is kFramesInFlight deep, so a slot is
        // REUSED after that many frames — waiting one frame longer than the
        // ring is deep threw every measurement away (measured: 0 readings over
        // 90 frames). This is the same condition the structures' own
        // readTimestamps uses, for the same reason.
        if (!pd.live || uint32_t(now - pd.frame) < inFlight) continue;
        uint64_t v[4] = {};
        const uint32_t base = rv.queryBase + i * 2u;
        if (vkGetQueryPoolResults(mVk, mReflectTimestamps, base, 2, sizeof(v), v,
                                  sizeof(uint64_t) * 2u,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                VK_SUCCESS &&
            v[1] && v[3] && v[2] >= v[0])
            rv.gpuMs = float(double(v[2] - v[0]) * double(mTimestampPeriod) * 1e-6);
        pd.live = false;
    }
}

void RayQueryTier::dropReflect(ReflectView &rv) {
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(rv.sets[i]);
        rv.sets[i] = VK_NULL_HANDLE;
        retire(rv.params[i]);
        retire(rv.geomRowOfSlot[i]);
    }
    for (int i = 0; i < 2; ++i) { retireImage(rv.hist[i]); retireImage(rv.dist[i]); }
    if (rv.hasQueryBase) mReflectQuerySlots &= ~(uint32_t(1) << rv.querySlot);
    rv.hasQueryBase = false;
    rv.imagesReady = false;
    rv.needsClear = false;
}

void RayQueryTier::forgetReflect(const ReflectPassListener *key) {
    auto it = mReflects.find(key);
    if (it == mReflects.end()) return;
    dropReflect(it->second);
    mReflects.erase(it);
}

void RayQueryTier::reflectStatsInto(const OgreScene *scene, RayQueryStatus &st) const {
    for (const auto &kv : mReflects) {
        if (kv.second.scene != scene) continue;
        st.reflect = true;
        st.reflectRays += int(kv.second.rays);
        if (kv.second.gpuMs > st.reflectMs) st.reflectMs = kv.second.gpuMs;
    }
}

RayQueryTier::EyeBasisF RayQueryTier::eyeBasis(bool ortho, const Ogre::Vector3 &pos,
                                               const Ogre::Quaternion &rot, float el, float er,
                                               float et, float eb) {
    EyeBasisF e;
    const Ogre::Vector3 f = rot * Ogre::Vector3::NEGATIVE_UNIT_Z;
    const Ogre::Vector3 r = rot * Ogre::Vector3::UNIT_X;
    const Ogre::Vector3 u = rot * Ogre::Vector3::UNIT_Y;
    put3(e.camPos, pos, ortho ? 0.0f : 1.0f);
    put3(e.rayTL, r * el + u * et + (ortho ? Ogre::Vector3::ZERO : f), 0.0f);
    put3(e.rayRight, r * (er - el), 0.0f);
    put3(e.rayDown, u * (eb - et), 0.0f);
    put3(e.fwd, f, 0.0f);
    return e;
}

// A target uv t is the shot's (t - x0) / w, hence rayTL' = rayTL - rayRight x0/w
// - rayDown y0/h, rayRight' = rayRight / w, rayDown' = rayDown / h. The bars
// hold cleared depth and every job declines them before any ray.
void RayQueryTier::expandEyeToTarget(EyeBasisF &e, const ChainDesc &cd, unsigned fullW,
                                     unsigned fullH) {
    if (!cd.letterbox || !fullH) return;
    float shot[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    chain::letterboxRect(cd.letterboxAspect, float(fullW) / float(fullH), shot);
    for (int k = 0; k < 3; ++k) {
        e.rayTL[k] -= e.rayRight[k] * shot[0] / shot[2] + e.rayDown[k] * shot[1] / shot[3];
        e.rayRight[k] /= shot[2];
        e.rayDown[k] /= shot[3];
    }
}

void RayQueryTier::recordReflect(const ReflectPassListener *key, OgreView *view,
                                 Ogre::CompositorPass *pass, const HitListBinding &hit) {
    if (!isOpen() || mReflectFailed || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;

    // THE STRUCTURE. Every one of these is a legitimate "not this frame", and
    // each leaves `jahSsrReflection` holding exactly what the resolve wrote —
    // i.e. today's picture, which is the whole point of the fallback contract.
    auto sceneIt = mScenes.find(scene);
    if (sceneIt == mScenes.end()) return;
    SceneAs &sa = sceneIt->second;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) return;

    if (!mReflectPipeline) {
        std::string err;
        if (!makeReflectPipeline(err)) {
            mReflectFailed = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: ray-traced reflections off — " + err +
                " (the screen-space march alone renders this frame)");
            return;
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: ray-traced reflections ON (PHOTON R5) — the screen-space march keeps "
            "what it can see, rays answer the rest");
    }

    // THE CHAIN'S TEXTURES, by the names OgreChain.cpp declares them under. A
    // chain shape that does not carry them is not an error: it is a view whose
    // SSR row is off, and this listener should not have been created for it.
    Ogre::TextureGpu *ssrTex = nullptr, *normalTex = nullptr, *roughTex = nullptr,
                     *depthTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        ssrTex    = node->getDefinedTexture(Ogre::IdString("jahSsrReflection"));
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        roughTex  = node->getDefinedTexture(Ogre::IdString("jahSsrShadowRough"));
        depthTex  = node->getDefinedTexture(Ogre::IdString("jahDepth"));
    } catch (Ogre::Exception &) { return; }
    if (!ssrTex || !normalTex || !roughTex || !depthTex) return;
    // THE UAV FLAG IS THE CONTRACT (OgreChain.cpp, ChainDesc::rayReflect). A
    // texture declared without it has no storage-image usage and the view
    // creation below would be a validation error, so a chain built while the
    // machine's answer was "no rays" is declined here rather than trusted.
    if (!ssrTex->isUav()) return;

    const unsigned fullW = ssrTex->getWidth(), fullH = ssrTex->getHeight();
    if (!fullW || !fullH) return;
    const int ssrRow = view->chainDesc().ssr;
    // ---- ONE TRACE, TWO EYES (lane REFLECT-VR-1) ----------------------------
    // A STEREO target is two eyes side by side in one texture, so the trace
    // covers both in one dispatch and each pixel's own column says which eye it
    // belongs to. Only two things have to be true for that to be exact, and
    // both are arranged here rather than hoped for:
    //
    //   * THE SEAM FALLS ON A BLOCK BOUNDARY. At the half-resolution row one
    //     trace texel covers a 2x2 block of the target, and a block straddling
    //     the middle would fetch one eye's depth to trace the other eye's ray.
    //     So the trace width is built from the EYE's width, and an eye whose
    //     half-resolution width would not divide traces at full resolution
    //     instead (a 1-pixel-odd eye size costs the row, not the picture).
    //   * THE EYES ARE KNOWN. They are the runtime's, pushed onto the View by
    //     the session each located frame; a stereo view without them declines
    //     rather than tracing one mono answer across two eyes, which is the
    //     defect this lane exists to remove.
    const bool stereo = view->stereo();
    const StereoEyeBasis *eyes = view->stereoEyes();
    unsigned traceW = ssrRow >= 2 ? fullW : std::max(1u, fullW / 2u);
    unsigned traceH = ssrRow >= 2 ? fullH : std::max(1u, fullH / 2u);
    if (stereo) {
        const unsigned eyeW = fullW / 2u;
        if (!eyeW || fullW != eyeW * 2u) return;      // not a two-eye target after all
        const unsigned eyeTraceW = ssrRow >= 2 ? eyeW : eyeW / 2u;
        traceW = eyeTraceW * 2u;
        if (!eyeTraceW || fullW % traceW != 0u) { traceW = fullW; traceH = fullH; }
    }

    // ---- THE VOXEL CACHE THE HITS ARE SHADED FROM (route A) -----------------
    // Under a Photon cascade chain each cascade is its OWN VctLighting (they are
    // chained with addCascade, OgreGi.cpp), innermost first — which is exactly
    // the order the shader wants: it takes the first volume that contains the
    // hit, so the finest one that can answer does. In the single-volume arm
    // there is one. R5 works in both shapes and the cascade flag gates nothing
    // here.
    Ogre::TextureGpu *vox[kMaxReflectCascades][8] = {};   // iso, X, Y, Z, coverage +/-, position +/-
    Ogre::Vector3 voxOrigin[kMaxReflectCascades], voxSize[kMaxReflectCascades],
                  voxCell[kMaxReflectCascades];
    /// THE CASCADE'S RADIANCE MULTIPLIER (DRAG-1, RENDER_AUDIT PHOTON F2).
    /// `VctLighting::update` with autoMultiplier normalises everything it
    /// injects by the brightest light's radiance over pi, and the PIXEL path
    /// multiplies it back out (`finalMultiplier = mInvBakingMultiplier *
    /// mMultiplier`, OgreVctLighting.cpp; read in Vct_piece_ps.any as
    /// `probeParams.multiplier`). The ray arm read the voxel raw, so a traced
    /// reflection was in baking units — right only when the brightest light has
    /// radiance pi, which is a sun at intensity 1 and every fixture the suites
    /// use. No patch is needed for it: `mMultiplier` is a public member and
    /// `getCurrentBakingMultiplier()` is a public accessor for the inverse of
    /// the other half, so the product is available from outside.
    float voxMultiplier[kMaxReflectCascades];
    unsigned voxCount = 0;
    bool anisotropic = false;
    const auto takeVolume = [&](Ogre::VctLighting *lighting,
                                Ogre::VctVoxelizer *voxelizer) {
        if (voxCount >= kMaxReflectCascades || !lighting || !voxelizer) return;
        Ogre::TextureGpu **tex = lighting->getLightVoxelTextures();
        if (!tex || !tex[0]) return;
        const bool aniso = lighting->isAnisotropic() && tex[1] && tex[2] && tex[3];
        if (voxCount == 0) anisotropic = aniso;
        else if (anisotropic != aniso) return;   // one shader path per dispatch
        // BY NAME (RQ-COV-SLOT-1): the isotropic volume, the three directional ones (the
        // isotropic volume stands in on a Low chain, whose shader never reads them) and
        // the coverage from VctLighting's own index.
        vox[voxCount][0] = tex[0];
        for (int i = 1; i < 4; ++i) vox[voxCount][i] = (aniso && tex[i]) ? tex[i] : tex[0];
        for (unsigned h = 0; h < 2u; ++h) {
            Ogre::TextureGpu *cov = tex[lighting->coverageIndex(h)], *pos = tex[lighting->positionIndex(h)];
            vox[voxCount][4 + h] = cov ? cov : tex[0];
            vox[voxCount][6 + h] = pos ? pos : tex[0];
        }
        voxOrigin[voxCount] = voxelizer->getVoxelOrigin();
        voxSize[voxCount]   = voxelizer->getVoxelSize();
        voxCell[voxCount]   = voxelizer->getVoxelCellSize();
        {
            const float baking = lighting->getCurrentBakingMultiplier();
            voxMultiplier[voxCount] =
                baking > 1e-6f ? lighting->mMultiplier / baking : lighting->mMultiplier;
        }
        ++voxCount;
    };
    if (!scene->mVctCascades.empty()) {
        for (const OgreScene::VctCascade &c : scene->mVctCascades)
            if (c.built) takeVolume(c.lighting, c.voxelizer);
    } else {
        takeVolume(scene->mVctLighting, scene->mVctVoxelizer);
    }
    // NO VOXELS IS NOT "NO REFLECTIONS": an escaping ray still reads the sky,
    // and in an open scene that is the whole answer. A HIT with no cache behind
    // it is what the shader declines (see its note) — it hands the pixel back
    // to the probe, which is the honest fallback.
    // THE ONE ENVIRONMENT (PHOTON-ENV-1): the Sky Light's cube and gain, or the
    // flat environment when there is no cube (OgreScene::rayEnvironment).
    const OgreScene::RayEnvironment rayEnv = scene->rayEnvironment();
    Ogre::TextureGpu *skyTex = rayEnv.cube;
    // ---- THE SURFACE CACHE THE HITS READ FIRST (PHOTON-CARDS-2, SC-1d) -------
    // Its two tables and two atlas layers, when the scene holds a built cache
    // (OgreScene::updateSurfaceCache — the `cards` row); without one the read
    // is bound to the stand-ins with zero slots and every hit reads the voxels.
    const SurfaceCache *cardCache = scene->mSurfaceCache.get();
    Ogre::UavBufferPacked *cardTable = cardCache ? cardCache->cardBuffer() : nullptr;
    Ogre::UavBufferPacked *cardInstances = cardCache ? cardCache->instanceBuffer() : nullptr;
    Ogre::TextureGpu *cardDepth = cardCache ? cardCache->depthLayer() : nullptr;
    Ogre::TextureGpu *cardRadiance = cardCache ? cardCache->radianceLayer() : nullptr;
    const bool cardsBound = cardTable && cardInstances && cardDepth && cardRadiance &&
                            cardCache->cardRecords() > 0u;

    // ---- PER-VIEW STATE -----------------------------------------------------
    ReflectView &rv = mReflects[key];
    rv.scene = scene;
    // A FRAME THAT DECLINES REPORTS ZERO (second reader, L4). Every early
    // return below leaves the picture correct, but leaving `rays` at the last
    // frame's value makes `giStatus().rayQuery.reflectRays` say a trace is
    // running when none is — the one reading a caller would use to find out.
    rv.rays = 0;
    /// EVERY EARLY RETURN BELOW IS A LEGITIMATE "not this frame" and leaves
    /// `jahSsrReflection` holding exactly what the resolve wrote — today's
    /// picture. `JAH_R5_WHY=1` names which one, because a silent decline is
    /// indistinguishable from a trace that draws nothing.
    const auto bail = [](const char *reason) {
        if (getenv("JAH_R5_WHY"))
            Ogre::LogManager::getSingleton().logMessage(std::string("R5 declined: ") + reason);
    };
    readReflectTimestamps(rv);
    if (!rv.hasQueryBase && mReflectTimestamps) {
        for (unsigned s = 0; s < kMaxTimedScenes; ++s) {
            if (mReflectQuerySlots & (uint32_t(1) << s)) continue;
            mReflectQuerySlots |= uint32_t(1) << s;
            rv.querySlot = s;
            rv.queryBase = s * kFramesInFlight * 2u;
            rv.hasQueryBase = true;
            break;
        }
    }

    std::string err;
    if (!ensureDummyImages(err)) {
        mReflectFailed = true;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: ray-traced reflections off — " + err);
        return;
    }
    if (!ensureReflectImages(rv, traceW, traceH, err)) {
        mReflectFailed = true;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: ray-traced reflections off — " + err);
        return;
    }
    const unsigned ring = rv.frame % kReflectRing;
    if (!rv.params[ring].buffer &&
        !makeBuffer(sizeof(ReflectParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    rv.params[ring], err))
        { bail("makeBuffer(params)"); return; }
    if (!rv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mReflectPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mReflectSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &rv.sets[ring]) != VK_SUCCESS) {
            rv.sets[ring] = VK_NULL_HANDLE;
            bail("vkAllocateDescriptorSets"); return;
        }
    }

    // ---- THE PARAMETERS -----------------------------------------------------
    ReflectParams pp{};
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Vector3 camPos = cam->getDerivedPosition();
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    const Ogre::Vector3 fwd = q * Ogre::Vector3::NEGATIVE_UNIT_Z;
    const Ogre::Vector3 right = q * Ogre::Vector3::UNIT_X;
    const Ogre::Vector3 up = q * Ogre::Vector3::UNIT_Y;
    // THE IMAGE PLANE FROM THE FRUSTUM ITSELF, not from fov and aspect: the
    // extents carry a frustum OFFSET, a manually set extent and the projection
    // type, and every one of those is a silent whole-image error if the basis
    // and the projection disagree. Tangents for a perspective camera (the basis
    // is a RAY per pixel), view-space positions for an orthographic one (the
    // basis is an OFFSET per pixel, constant in depth) — which is the same split
    // the shader makes, from the same flag.
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb,
                           ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    // ONE IMAGE'S BASIS, from a pose and a frustum (lane REFLECT-VR-1). The
    // arithmetic is exactly what the single-camera path did; it is a function
    // now because a stereo target needs it twice, from two poses and two
    // frustums, and having two copies of it is how they would drift apart.
    const auto makeEye = [ortho](const Ogre::Vector3 &pos, const Ogre::Quaternion &rot,
                                 float el, float er, float et, float eb) {
        return eyeBasis(ortho, pos, rot, el, er, et, eb);
    };
    // THE EYES, OR THE ONE CAMERA. A stereo view whose eyes have not been
    // pushed yet declines: tracing the head's frustum across a two-eye target
    // maps each eye's half onto HALF of one mono frustum, which is not a small
    // error but a different picture (the reflection of anything off the head's
    // axis lands in the wrong eye or in neither — the owner's "no reflections
    // in the headset", measured).
    EyeBasisF eyeB[2];
    // THE ARM THAT RE-MEASURES THE CLAIM (the shape of JAH_RQ_NO_MULT beside
    // it): `JAH_R5_MONO_EYES=1` traces a stereo target through the RENDERING
    // camera for both halves — the behaviour before lane REFLECT-VR-1 — so the
    // cost of getting this wrong can be measured rather than argued. On the
    // `vr.session` mirror fixture it moves an eye from a mean of 0.34/255
    // against its own mono control (0.39 % of bytes over 8) to 2.70 at 5.6 % in
    // the LEFT eye and 6.26 at 12.8 % in the RIGHT one — the asymmetry being
    // that the rendering camera carries the left eye's projection, so one
    // camera is nearly right for one half and wrong for the other.
    // Read ONCE per process, like `JAH_RQ_REFIT` beside it: an environment
    // variable cannot change under a running process, and this sits in a
    // per-frame path.
    static const bool sMonoEyesArm = getenv("JAH_R5_MONO_EYES") != nullptr;
    const bool monoEyes = stereo && sMonoEyesArm;
    if (stereo && !monoEyes) {
        if (!eyes) { bail("a stereo view with no located eyes"); return; }
        for (int i = 0; i < 2; ++i)
            eyeB[i] = makeEye(eyes[i].position, eyes[i].orientation, eyes[i].tanLeft,
                              eyes[i].tanRight, eyes[i].tanTop, eyes[i].tanBottom);
    } else {
        eyeB[0] = makeEye(camPos, q, float(fl), float(fr), float(ft), float(fb));
        // Written, never read in the mono case (`stereo.x` is 0): a uniform
        // buffer with a half-initialised tail is a thing to read in a debugger
        // one day. Under the measurement arm above it IS read, and reading the
        // same basis for both halves is exactly what the arm reproduces.
        eyeB[1] = eyeB[0];
    }
    // THE LETTERBOX (SSR-LETTERBOX-1's ray half). Under a constrained-aspect
    // camera the picture is the target's INNER rectangle (chain::letterboxRect),
    // while this pass addresses the whole target: so the image basis is
    // EXPANDED to the target — the same conjugation the march takes in its
    // pass buffer, here on the CPU so no shader line moves. A target uv t is the
    // shot's (t - x0) / w, hence rayTL' = rayTL - rayRight x0/w - rayDown y0/h,
    // rayRight' = rayRight / w, rayDown' = rayDown / h. The bars hold cleared
    // depth and are declined before any ray; the previous-frame basis is the
    // expanded one too (rv.prev is written from eyeB below), so the
    // reprojection's uv spans the same target.
    {
        const ChainDesc cd = view->chainDesc();
        for (int i = 0; i < 2; ++i) expandEyeToTarget(eyeB[i], cd, fullW, fullH);
    }
    memcpy(pp.camPos, eyeB[0].camPos, sizeof(pp.camPos));
    memcpy(pp.rayTL, eyeB[0].rayTL, sizeof(pp.rayTL));
    memcpy(pp.rayRight, eyeB[0].rayRight, sizeof(pp.rayRight));
    memcpy(pp.rayDown, eyeB[0].rayDown, sizeof(pp.rayDown));
    memcpy(pp.fwd, eyeB[0].fwd, sizeof(pp.fwd));
    memcpy(pp.camPos2, eyeB[1].camPos, sizeof(pp.camPos2));
    memcpy(pp.rayTL2, eyeB[1].rayTL, sizeof(pp.rayTL2));
    memcpy(pp.rayRight2, eyeB[1].rayRight, sizeof(pp.rayRight2));
    memcpy(pp.rayDown2, eyeB[1].rayDown, sizeof(pp.rayDown2));
    memcpy(pp.fwd2, eyeB[1].fwd, sizeof(pp.fwd2));
    // ...and the flag the shader splits on. Under the measurement arm it is 0
    // on a stereo target, which is precisely the pre-lane shape: ONE image, the
    // rendering camera's, stretched across two eyes' worth of pixels. (What the
    // arm cannot reproduce is that before ogre-patch 0078 that camera's
    // extents were INDETERMINATE as well, because it carries a custom
    // projection matrix — so the shipped defect was the sum of the two.)
    pp.stereo[0] = (stereo && !monoEyes) ? 1.0f : 0.0f;
    // THE VIEW AXES ARE THE RENDERING CAMERA'S, FOR BOTH EYES, AND THAT IS
    // EXACT rather than an approximation (the pin, checked): the G-buffer
    // normal is written in the pass camera's view space — HlmsPbs uploads ONE
    // `mat4 view` per pass and instanced stereo does not make it two
    // (OgreHlmsPbs.cpp:2327) — so under stereo both halves' normals are in the
    // HEAD's view space and this one rotation is what turns either of them back
    // into the world. (It is also why the eyes' own orientations do not enter
    // here: a per-eye rotation applied to a head-space normal would TILT it.)
    put3(pp.viewAxisX, right, 0.0f);
    put3(pp.viewAxisY, up, 0.0f);
    put3(pp.viewAxisZ, -fwd, 0.0f);          // Ogre's view space looks down -Z
    Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    pp.projParams[0] = projAB.x;
    pp.projParams[1] = projAB.y;
    pp.projParams[2] = cam->getFarClipDistance();
    pp.projParams[3] = kRayReflectFeather;
    pp.resolution[0] = float(traceW); pp.resolution[1] = float(traceH);
    pp.resolution[2] = float(fullW);  pp.resolution[3] = float(fullH);
    // THE CUTOFF IS THE PROJECT'S (PostFxDesc::reflectionRoughnessCutoff) and the same
    // one the screen-space march gates on since lane SSR-3; the engine only
    // clamps it into the range a reflection means anything in.
    pp.knobs[0] = std::min(std::max(view->chainDesc().reflectionRoughnessCutoff, 0.0f), 1.0f);
    // THE RAY'S LENGTH. Long enough to cross the lit volume it will be shaded
    // from — a ray that outruns the cache finds geometry nothing can colour —
    // and bounded by the camera's own far plane so an open scene's ray reaches
    // the sky rather than marching the whole world.
    {
        float reach = voxCount ? voxSize[voxCount - 1u].length() : 0.0f;
        pp.knobs[1] = std::min(cam->getFarClipDistance(), std::max(reach, 50.0f));
    }
    pp.knobs[2] = float(rv.frame & 0xFFFFu);
    pp.knobs[3] = float(voxCount);
    pp.knobs2[0] = anisotropic ? 1.0f : 0.0f;
    // THE SURFACE BIAS, in world units, derived from the finest cell rather than
    // chosen: it has to clear the voxel grid's own quantisation (a ray that
    // starts inside the cell holding its own surface reads that surface back as
    // the reflection) and nothing else in the scene has a length scale to offer.
    pp.knobs2[1] = voxCount ? std::max(0.01f, 0.5f * voxCell[0].length()) : 0.02f;
    pp.knobs2[2] = skyTex ? 1.0f : 0.0f;
    pp.knobs2[3] = kReflectHistoryFloor;
    // The cube's gain, or the flat environment with no cube (jah_rq_hit.glsl's
    // JAH_SKY_COLOUR contract).
    for (int i = 0; i < 3; ++i) pp.skyColour[i] = rayEnv.colour[i];
    for (unsigned c = 0; c < kMaxReflectCascades; ++c) {
        const unsigned src = c < voxCount ? c : (voxCount ? voxCount - 1u : 0u);
        const Ogre::Vector3 sz = voxCount ? voxSize[src] : Ogre::Vector3(1.0f);
        const Ogre::Vector3 og = voxCount ? voxOrigin[src] : Ogre::Vector3::ZERO;
        const Ogre::Vector3 cl = voxCount ? voxCell[src] : Ogre::Vector3(1.0f);
        pp.voxelOrigin[c][0] = og.x; pp.voxelOrigin[c][1] = og.y; pp.voxelOrigin[c][2] = og.z;
        // ...AND THE RADIANCE MULTIPLIER in `.w` (DRAG-1). The slot held this
        // cascade's largest extent, which no shader ever read; see voxMultiplier.
        pp.voxelOrigin[c][3] = voxCount ? voxMultiplier[src] : 1.0f;
        // THE ARMS THAT RE-MEASURE THE CLAIM (DRAG-1), the same shape as
        // JAHSHAKA_GI_SWEEPS: `JAH_RQ_NO_MULT` restores the un-multiplied
        // reading this replaced, and `JAH_RQ_SHOW_MULT` prints the factor. On
        // tests/rtreflect's fixture (brightest light radiance 2.0, so the
        // factor is 2/pi = 0.6366) the mirror's red excess reads 0.0737 without
        // it and 0.0434 with it: the ray used to show that fixture 57 % too
        // bright, which is what a voxel read in baking units means.
        if (std::getenv("JAH_RQ_NO_MULT")) pp.voxelOrigin[c][3] = 1.0f;
        if (std::getenv("JAH_RQ_SHOW_MULT") && c == 0)
            std::fprintf(stderr, "JAH_RQ multiplier c0 = %.6f (cascades %u)\n",
                         double(pp.voxelOrigin[c][3]), voxCount);
        pp.voxelInvSize[c][0] = sz.x > 0.0f ? 1.0f / sz.x : 0.0f;
        pp.voxelInvSize[c][1] = sz.y > 0.0f ? 1.0f / sz.y : 0.0f;
        pp.voxelInvSize[c][2] = sz.z > 0.0f ? 1.0f / sz.z : 0.0f;
        pp.voxelInvSize[c][3] = std::max(std::max(cl.x, cl.y), cl.z);
    }
    // NO PREVIOUS FRAME — OR A DIFFERENT SHAPE OF ONE. A zero forward makes
    // every reprojection's `z` zero, which the shader rejects, so the first
    // frame after a resize, a scene bind or a workspace rebuild starts its mean
    // from scratch instead of reading a buffer that means something else. A
    // view that has just BECOME stereo (or stopped being) is the same case: its
    // history's two halves were one picture, or its one picture is now two.
    if (rv.havePrev && rv.prevStereo == stereo) {
        memcpy(pp.prevCamPos, rv.prev[0].camPos, sizeof(pp.prevCamPos));
        memcpy(pp.prevRayTL, rv.prev[0].rayTL, sizeof(pp.prevRayTL));
        memcpy(pp.prevRayRight, rv.prev[0].rayRight, sizeof(pp.prevRayRight));
        memcpy(pp.prevRayDown, rv.prev[0].rayDown, sizeof(pp.prevRayDown));
        memcpy(pp.prevFwd, rv.prev[0].fwd, sizeof(pp.prevFwd));
        memcpy(pp.prevCamPos2, rv.prev[1].camPos, sizeof(pp.prevCamPos2));
        memcpy(pp.prevRayTL2, rv.prev[1].rayTL, sizeof(pp.prevRayTL2));
        memcpy(pp.prevRayRight2, rv.prev[1].rayRight, sizeof(pp.prevRayRight2));
        memcpy(pp.prevRayDown2, rv.prev[1].rayDown, sizeof(pp.prevRayDown2));
        memcpy(pp.prevFwd2, rv.prev[1].fwd, sizeof(pp.prevFwd2));
    } else {
        memset(pp.prevFwd, 0, sizeof(pp.prevFwd));
        memset(pp.prevFwd2, 0, sizeof(pp.prevFwd2));
        rv.historyFrames = 0;
    }
    // THE VIEW'S AGE, in frames of unbroken history (rq_reflect.comp's
    // `stereo.y`; 0 on a first frame, a resize, a scene bind and a change of
    // stereo shape, exactly where the previous basis is withheld above).
    pp.stereo[1] = float(rv.historyFrames);
    pp.cards[0] = cardsBound ? float(cardCache->instanceSlots()) : 0.0f;
    pp.cards[1] = cardsBound ? float(cardCache->cardRecords()) : 0.0f;
    pp.cards[2] = cardFootprintTexels();
    // ---- THE HIT'S GEOMETRIC NORMAL: the per-slot row table (a copy per frame in
    // flight) and the GPU scene's geometry rows, flushed first (a row staged but
    // not uploaded is a zero address - the trap file's GPU SCENE TABLES rule) and
    // the table pointer re-read here, never cached across a frame.
    detail::GpuScene &gpuScn = scene->gpuScene();
    if (gpuScn.live()) gpuScn.flushGeomRows();
    Ogre::UavBufferPacked *geomRows = gpuScn.live() ? gpuScn.geomBuffer() : nullptr;
    uint32_t geomSlots = geomRows ? uint32_t(sa.geomRowOfSlot.size()) : 0u;
    if (geomSlots) {
        RawBuffer &rb = rv.geomRowOfSlot[ring];
        const VkDeviceSize want = VkDeviceSize(geomSlots) * sizeof(uint32_t);
        if (rb.buffer && rb.size < want) retire(rb);
        if (!rb.buffer &&
            !makeBuffer(std::max<VkDeviceSize>(want, 1024u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                        false, rb, err))
            geomSlots = 0u;
        else
            std::memcpy(rb.mapped, sa.geomRowOfSlot.data(), size_t(want));
    }
    pp.cards[3] = float(geomSlots);
    // THE HIT RECORD (PHOTON-HIT-SHADE-1): the list the decode shades, the sun ray.
    pp.hitList[0] = float(hit.capacity);
    pp.hitList[1] = float(hit.width);
    pp.hitList[2] = float(hit.instanceEntries);
    pp.hitList[3] = hit.on ? 1.0f : 0.0f;
    pp.hitSun[0] = hit.toSun[0];
    pp.hitSun[1] = hit.toSun[1];
    pp.hitSun[2] = hit.toSun[2];
    pp.hitSun[3] = hit.sun ? 1.0f : 0.0f;
    pp.hitSun2[0] = float(hit.sunMask);
    pp.hitSun2[1] = hit.lift;
    pp.hitSun2[2] = hit.sunRange;
    pp.hitSun2[3] = hit.farLift;
    if (rv.historyFrames < 4096u) ++rv.historyFrames;   // saturates: "warm" is all it says
    memcpy(rv.params[ring].mapped, &pp, sizeof(pp));
    rv.prev[0] = eyeB[0];
    rv.prev[1] = eyeB[1];
    rv.havePrev = true;
    rv.prevStereo = stereo;

    // ---- THE DESCRIPTOR SET, rewritten every frame --------------------------
    // IMAGE VIEWS, UNCACHED AND RETIRED — and the cache is not an optimisation
    // we passed up, it is a TRAP measured here. `createView(slot, bUseCache =
    // true)` hands back a REFCOUNTED view out of VulkanTextureGpuManager keyed
    // by the slot's contents, and the key contains the TextureGpu POINTER, not
    // the VkImage. A compositor texture that is recreated at the same address —
    // which is every texture in the chain on every window resize — therefore
    // answers with the view of the DESTROYED image, and the descriptor set we
    // write points at memory that is gone. Measured exactly that way: frames 1
    // and 2 bound a view of frame 0's `jahSsrReflection`, the validation layer
    // named the dead image, the command buffer was invalidated by the
    // destruction of frame 0's depth texture, `vkEndCommandBuffer` failed and
    // the frame died with VK_ERROR_DEVICE_LOST. (A cached view must also be
    // handed back with `destroyView`, which is a second thing to get wrong.)
    //
    // So: a fresh view per frame, retired into the same bin as everything else
    // here, destroyed once the frames that could still be reading it have.
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    const auto uavView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetUav::TextureSlot slot =
            Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = t;
        slot.access = Ogre::ResourceAccess::ReadWrite;
        slot.pixelFormat = t->getPixelFormat();
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    const unsigned prev = rv.frame & 1u, cur = 1u - prev;

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo ub{};
    ub.buffer = rv.params[ring].buffer;
    ub.range = sizeof(ReflectParams);
    VkDescriptorImageInfo sampled[3] = {}, storage[5] = {},
                          volumes[8][kMaxReflectCascades] = {}, sky{};
    Ogre::TextureGpu *const sampledSrc[3] = { normalTex, roughTex, depthTex };
    for (int i = 0; i < 3; ++i) {
        sampled[i].sampler = mPointSampler;
        sampled[i].imageView = sampledView(sampledSrc[i]);
        sampled[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // The depth texture's view carries the DEPTH aspect, but its LAYOUT under
    // `ResourceLayout::Texture` is the plain shader-read one for every format
    // (OgreVulkanMappings.cpp:603) — which is the layout the transition below
    // asks the solver for, so the descriptor and the barrier agree.
    storage[0].imageView = uavView(ssrTex);
    storage[1].imageView = rv.hist[prev].view;
    storage[2].imageView = rv.hist[cur].view;
    storage[3].imageView = rv.dist[prev].view;
    storage[4].imageView = rv.dist[cur].view;
    for (int i = 0; i < 5; ++i) storage[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (int axis = 0; axis < 8; ++axis)
        for (unsigned c = 0; c < kMaxReflectCascades; ++c) {
            const unsigned src = c < voxCount ? c : (voxCount ? voxCount - 1u : 0u);
            Ogre::TextureGpu *t = voxCount ? vox[src][axis] : nullptr;
            volumes[axis][c].sampler = mLinearSampler;
            volumes[axis][c].imageView = t ? sampledView(t) : mDummyVolume.view;
            volumes[axis][c].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    sky.sampler = mLinearSampler;
    sky.imageView = skyTex ? sampledView(skyTex) : mDummyCube.view;
    sky.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // EVERY DESCRIPTOR MUST BE A REAL VIEW — a null one in a set the shader may
    // index is undefined behaviour, not a black sample — which is what the 1x1
    // black stand-ins above are for. A slot still empty here is a failure to
    // make one, and declining the frame is the honest answer.
    for (int axis = 0; axis < 8; ++axis)
        for (unsigned c = 0; c < kMaxReflectCascades; ++c)
            if (!volumes[axis][c].imageView) { bail("a voxel view is null"); return; }
    if (!sky.imageView) { bail("the sky view is null"); return; }

    VkWriteDescriptorSet w[kReflectBindings] = {};
    for (unsigned i = 0; i < kReflectBindings; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = rv.sets[ring];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    w[0].pNext = &asWrite;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &ub;
    for (int i = 0; i < 3; ++i) {
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2 + i].pImageInfo = &sampled[i];
    }
    for (int i = 0; i < 5; ++i) {
        w[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[5 + i].pImageInfo = &storage[i];
    }
    for (int axis = 0; axis < 4; ++axis) {
        w[10 + axis].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[10 + axis].descriptorCount = kMaxReflectCascades;
        w[10 + axis].pImageInfo = volumes[axis];
    }
    w[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[14].pImageInfo = &sky;
    for (unsigned k = 0; k < kReflectSplitKinds; ++k) {
        w[kReflectCovBinding + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[kReflectCovBinding + k].descriptorCount = kMaxReflectCascades;
        w[kReflectCovBinding + k].pImageInfo = volumes[4 + k];
    }
    // THE CARD READ'S FOUR (15-18): the cache's own, or the stand-ins.
    VkDescriptorBufferInfo cardBufs[2] = {};
    VkDescriptorImageInfo cardImgs[2] = {};
    {
        Ogre::UavBufferPacked *const bufs[2] = { cardTable, cardInstances };
        for (int i = 0; i < 2; ++i) {
            if (cardsBound) {
                auto *bi = static_cast<Ogre::VulkanBufferInterface *>(bufs[i]->getBufferInterface());
                cardBufs[i].buffer = bi->getVboName();
                cardBufs[i].offset = VkDeviceSize(bufs[i]->_getFinalBufferStart()) *
                                     bufs[i]->getBytesPerElement();
                cardBufs[i].range = bufs[i]->getTotalSizeBytes();
            } else {
                cardBufs[i].buffer = mDummyStorage.buffer;
                cardBufs[i].range = VK_WHOLE_SIZE;
            }
            w[kReflectCardBinding + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[kReflectCardBinding + i].pBufferInfo = &cardBufs[i];
        }
        Ogre::TextureGpu *const layers[2] = { cardDepth, cardRadiance };
        for (int i = 0; i < 2; ++i) {
            cardImgs[i].sampler = mPointSampler;
            cardImgs[i].imageView = cardsBound ? sampledView(layers[i]) : mDummyFlat.view;
            cardImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w[kReflectCardBinding + 2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[kReflectCardBinding + 2 + i].pImageInfo = &cardImgs[i];
        }
        if (!cardImgs[0].imageView || !cardImgs[1].imageView || !cardBufs[0].buffer ||
            !cardBufs[1].buffer) {
            bail("a card-read binding is null");
            return;
        }
    }
    // THE HIT'S GEOMETRIC NORMAL (19, 20): the tables, or the stand-in with zero
    // slots bound (the shader then faces the reversed ray).
    VkDescriptorBufferInfo geomBufs[2] = {};
    if (geomSlots) {
        geomBufs[0].buffer = rv.geomRowOfSlot[ring].buffer;
        geomBufs[0].range = VK_WHOLE_SIZE;
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(geomRows->getBufferInterface());
        geomBufs[1].buffer = bi->getVboName();
        geomBufs[1].offset = VkDeviceSize(geomRows->_getFinalBufferStart()) * geomRows->getBytesPerElement();
        geomBufs[1].range = geomRows->getTotalSizeBytes();
    } else {
        for (VkDescriptorBufferInfo &g : geomBufs) { g.buffer = mDummyStorage.buffer; g.range = VK_WHOLE_SIZE; }
    }
    for (int i = 0; i < 2; ++i) {
        w[kReflectGeomBinding + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kReflectGeomBinding + i].pBufferInfo = &geomBufs[i];
    }
    // THE HIT RECORD (25-28, PHOTON-HIT-SHADE-1): the list, or the stand-ins.
    VkDescriptorBufferInfo hitBufs[2] = {};
    hitBufs[0].buffer = hit.instances ? hit.instances : mDummyStorage.buffer;
    hitBufs[0].offset = hit.instances ? hit.instancesOffset : 0u;
    hitBufs[0].range = hit.instances ? hit.instancesRange : VK_WHOLE_SIZE;
    hitBufs[1].buffer = hit.buf ? hit.buf : mDummyStorage.buffer;
    hitBufs[1].offset = hit.buf ? hit.bufOffset : 0u;
    hitBufs[1].range = hit.buf ? hit.bufRange : VK_WHOLE_SIZE;
    VkDescriptorImageInfo hitImgs[2] = {};
    {
        const VkImageView views[2] = { hit.ids, hit.dest };
        for (int i = 0; i < 2; ++i) {
            if (!views[i]) { bail("a hit-list view is null"); return; }
            hitImgs[i].imageView = views[i];
            hitImgs[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[kReflectHitBinding + 1 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[kReflectHitBinding + 1 + i].pImageInfo = &hitImgs[i];
        }
    }
    w[kReflectHitBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kReflectHitBinding].pBufferInfo = &hitBufs[0];
    w[kReflectHitBinding + 3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kReflectHitBinding + 3].pBufferInfo = &hitBufs[1];
    vkUpdateDescriptorSets(mVk, kReflectBindings, w, 0, nullptr);

    // ---- THE LAYOUTS, THROUGH OGRE'S OWN SOLVER -----------------------------
    // The DOCUMENTED route, and it is the documented route again after a
    // detour (second reader, M1). The first pass here emitted the barriers by
    // hand and wrote `VulkanTextureGpu::mCurrLayout` from outside, on the
    // reading that `resolveTransition` had produced nothing; that reading was
    // taken while the command buffer was ALREADY invalid (a cached image view
    // of a destroyed texture — the real defect, fixed elsewhere in this file),
    // so it was a symptom and not a cause. Writing another module's layout
    // bookkeeping from outside is exactly the class of workaround §325 is
    // about, and it is gone.
    //
    // The array is a LOCAL, not `getNewResourceTransitionsArrayTmp()`: that
    // accessor hands back the solver's one shared scratch and its own header
    // says not to hold it in two places at once.
    //
    // IT RUNS BEFORE THE COMMAND BUFFER IS TAKEN, which is not style:
    // `executeResourceTransition` closes every encoder
    // (OgreVulkanRenderSystem.cpp:3483) and Ogre's queue may roll over to a new
    // command buffer while doing it, so a handle fetched earlier can be an
    // already-ended one.
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, ssrTex, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::ReadWrite, computeStage);
        for (Ogre::TextureGpu *t : { normalTex, roughTex, depthTex })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        for (unsigned c = 0; c < voxCount; ++c)
            for (int axis = 0; axis < 8; ++axis)
                if (vox[c][axis])
                    solver.resolveTransition(trans, vox[c][axis], Ogre::ResourceLayout::Texture,
                                             Ogre::ResourceAccess::Read, computeStage);
        if (skyTex)
            solver.resolveTransition(trans, skyTex, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        // THE CARD READ'S INPUTS: the Radiance layer the CardLight job wrote
        // (a UAV) and the Depth layer the capture copied into, read as
        // textures; the two tables syncBuffers uploaded, read as buffers.
        if (cardsBound) {
            for (Ogre::TextureGpu *t : { cardDepth, cardRadiance })
                solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                         Ogre::ResourceAccess::Read, computeStage);
            for (Ogre::UavBufferPacked *b : { cardTable, cardInstances })
                solver.resolveTransition(trans, b, Ogre::ResourceAccess::Read, computeStage);
        }
        if (geomSlots)
            solver.resolveTransition(trans, geomRows, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }

    // ---- THE DISPATCH -------------------------------------------------------
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) { bail("frameCmd"); return; }   // device lost: record nothing
    clearDummyImages(cmd);
    clearReflectImages(rv, cmd);
    if (mReflectTimestamps && rv.hasQueryBase) {
        const uint32_t base = rv.queryBase + (rv.frame % kFramesInFlight) * 2u;
        vkCmdResetQueryPool(cmd, mReflectTimestamps, base, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mReflectTimestamps, base);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1,
                            &rv.sets[ring], 0, nullptr);
    vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, 1u);
    // ---- THE FIRST HALF ENDS HERE (PHOTON-HIT-SHADE-1): the hits no cache
    // shades are in the hit list; the decode pass shades them and the write-back
    // completes their texels' means before finishReflect filters.
    rv.finishPending = true;
    rv.finishRing = ring;
    rv.traceW = traceW;
    rv.traceH = traceH;
    rv.curIdx = cur;
    rv.rays = traceW * traceH;
    ++rv.frame;
}

void RayQueryTier::finishReflect(const ReflectPassListener *key) {
    auto it = mReflects.find(key);
    if (it == mReflects.end() || !it->second.finishPending) return;
    ReflectView &rv = it->second;
    rv.finishPending = false;
    const unsigned ring = rv.finishRing;
    const unsigned traceW = rv.traceW, traceH = rv.traceH;
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    // THE MEAN IS WRITTEN; NOW IT IS READ BY ITS NEIGHBOURS. A plain memory
    // barrier is enough and an image barrier would be wrong: the temporal mean
    // stays in GENERAL for both passes and only the ACCESS has to be ordered.
    {
        VkMemoryBarrier meanReady{};
        meanReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        meanReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        meanReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &meanReady, 0, nullptr, 0,
                             nullptr);
    }
    // THE FILTER AND THE COMPOSITE — the same descriptor set, a different
    // pipeline. Its timestamp is the SAME pair as the trace's, deliberately:
    // what a budget cares about is what the reflection costs, and the split
    // between tracing and filtering is ours, not the frame's.
    // The set is the trace's (the same layout); a second command-buffer bind,
    // since a scene pass ran between the two halves.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1,
                            &rv.sets[ring], 0, nullptr);
    vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, 1u);
    if (mReflectTimestamps && rv.hasQueryBase) {
        // rv.frame was advanced by the trace: its pair is the previous slot's.
        const unsigned tf = rv.frame - 1u;
        const uint32_t base = rv.queryBase + (tf % kFramesInFlight) * 2u;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mReflectTimestamps,
                            base + 1u);
        ReflectView::Pending &pd = rv.pending[tf % kFramesInFlight];
        pd.frame = frameNow();
        pd.live = true;
    }
}

// ---------------------------------------------------------------------------
// THE SCREEN-PROBE GATHER — the tier's half (GATHER-1a, 2026-09-21).
//
// Everything below reads the SCENE and hands the Component a record of it. The
// split is deliberate and it is the whole of SCREEN_PROBE_GATHER_SPEC section
// 9: an algorithm that will grow a filter, an SH record, history pairs and
// importance sampling belongs in a Component of ours; the device, the
// structures and the friendship with `OgreScene` belong here.

void RayQueryTier::releaseGatherBinding(const ReflectPassListener *key) {
    if (mGather) mGather->releaseBinding(key);
}

void RayQueryTier::forgetGather(const ReflectPassListener *key) {
    if (mGather) mGather->forget(key);
}

void RayQueryTier::gatherStatsInto(const OgreScene *scene, GatherStatus &out) const {
    if (mGather) mGather->statsInto(scene, scene->gatherRestKey(), scene->gatherRestartKey(), out);
}

void RayQueryTier::recordGather(const ReflectPassListener *key, OgreView *view,
                                Ogre::CompositorPass *pass, const HitListBinding &hit) {
    if (!isOpen() || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;
    if (!scene->probeGatherWanted()) {
        // OFF IS FREE, and it also has to be CLEAN: a view that was gathering
        // and stopped must give its atlases back and stop binding its texture
        // to the shader.
        if (mGather && mGather->holds(key)) mGather->forget(key);
        return;
    }
    auto sceneIt = mScenes.find(scene);
    if (sceneIt == mScenes.end()) return;
    SceneAs &sa = sceneIt->second;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) return;
    // ONE EYE ONLY AT PHASE 1. A stereo target is two images in one texture and
    // the probe grid would have to be split at the seam exactly as the
    // reflection's is; that is the spec's phase 7, and gathering a stereo
    // target as if it were one image would measure a wrong picture rather than
    // decline to measure one (the VR column keeps the irradiance field, V-A).
    if (view->stereo()) return;

    // THE CHAIN'S TEXTURES, by the names OgreChain.cpp declares them under —
    // the same two the reflection trace reads, and for the same reason: a
    // probe's surface is the one its pixel drew.
    Ogre::TextureGpu *normalTex = nullptr, *depthTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        depthTex = node->getDefinedTexture(Ogre::IdString("jahDepth"));
    } catch (Ogre::Exception &) { return; }
    if (!normalTex || !depthTex) return;
    if (!depthTex->getWidth() || !depthTex->getHeight()) return;

    GatherInputs in;
    in.scene = scene;
    in.sceneMgr = scene->mSceneMgr;
    in.tlas = sa.tlas;
    in.normals = normalTex;
    in.depth = depthTex;
    in.width = depthTex->getWidth();
    in.height = depthTex->getHeight();
    // THE TIER TABLE'S GATHER ROW (GA-TIERROW: the table, never the SSR row).
    // A stereo view has declined above, so the column is the desktop one.
    in.facts = giQualityFacts(scene->giParams().quality, GiViewProfile::Desktop,
                              scene->giParams().epicTier)
                   .gather;
    in.tuning = scene->gatherTuning();
    in.restKey = scene->gatherRestKey();
    in.restartKey = scene->gatherRestartKey();
    in.farOverlap = sa.farOverlap;

    // ---- the voxel cache the hits are shaded from (the reflection's rule) ---
    const auto takeVolume = [&](Ogre::VctLighting *lighting, Ogre::VctVoxelizer *voxelizer) {
        if (in.cascadeCount >= kGatherMaxCascades || !lighting || !voxelizer) return;
        Ogre::TextureGpu **tex = lighting->getLightVoxelTextures();
        if (!tex || !tex[0]) return;
        const bool aniso = lighting->isAnisotropic() && tex[1] && tex[2] && tex[3];
        if (in.cascadeCount == 0) in.anisotropic = aniso;
        else if (in.anisotropic != aniso) return;
        const unsigned c = in.cascadeCount;
        // BY NAME (RQ-COV-SLOT-1): as the reflection binds them.
        in.voxel[c][0] = tex[0];
        for (int i = 1; i < 4; ++i) in.voxel[c][i] = (aniso && tex[i]) ? tex[i] : tex[0];
        for (unsigned h = 0; h < 2u; ++h) {
            Ogre::TextureGpu *cov = tex[lighting->coverageIndex(h)], *pos = tex[lighting->positionIndex(h)];
            in.voxel[c][4 + h] = cov ? cov : tex[0];
            in.voxel[c][6 + h] = pos ? pos : tex[0];
        }
        const Ogre::Vector3 og = voxelizer->getVoxelOrigin();
        const Ogre::Vector3 sz = voxelizer->getVoxelSize();
        const Ogre::Vector3 cl = voxelizer->getVoxelCellSize();
        in.voxelOrigin[c][0] = og.x; in.voxelOrigin[c][1] = og.y; in.voxelOrigin[c][2] = og.z;
        in.voxelSize[c][0] = sz.x;   in.voxelSize[c][1] = sz.y;   in.voxelSize[c][2] = sz.z;
        in.voxelCell[c] = std::max(std::max(cl.x, cl.y), cl.z);
        const float baking = lighting->getCurrentBakingMultiplier();
        in.voxelMultiplier[c] =
            baking > 1e-6f ? lighting->mMultiplier / baking : lighting->mMultiplier;
        ++in.cascadeCount;
    };
    if (!scene->mVctCascades.empty()) {
        for (const OgreScene::VctCascade &c : scene->mVctCascades)
            if (c.built) takeVolume(c.lighting, c.voxelizer);
    } else {
        takeVolume(scene->mVctLighting, scene->mVctVoxelizer);
    }
    {
        // THE ONE ENVIRONMENT (PHOTON-ENV-1; OgreScene::rayEnvironment).
        const OgreScene::RayEnvironment rayEnv = scene->rayEnvironment();
        in.sky = rayEnv.cube;
        for (int i = 0; i < 3; ++i) in.skyColour[i] = rayEnv.colour[i];
    }

    // ---- THE SURFACE CACHE THE HITS READ FIRST (GA-1e) ----------------------
    // The reflection trace's four bindings, taken from the same place by the
    // same rule (recordReflect): the scene's built cache, or nothing (the
    // stand-ins are bound and every hit reads the voxels).
    {
        const SurfaceCache *cache = scene->mSurfaceCache.get();
        Ogre::UavBufferPacked *table = cache ? cache->cardBuffer() : nullptr;
        Ogre::UavBufferPacked *instances = cache ? cache->instanceBuffer() : nullptr;
        Ogre::TextureGpu *depthLayer = cache ? cache->depthLayer() : nullptr;
        Ogre::TextureGpu *radianceLayer = cache ? cache->radianceLayer() : nullptr;
        if (table && instances && depthLayer && radianceLayer && cache->cardRecords() > 0u) {
            in.cardTable = table;
            in.cardInstances = instances;
            in.cardDepth = depthLayer;
            in.cardRadiance = radianceLayer;
            in.cardSlots = cache->instanceSlots();
            in.cardRecords = cache->cardRecords();
        }
        in.cardFootprintTexels = cardFootprintTexels();
    }
    // ...AND THE HIT'S GEOMETRIC NORMAL: the per-slot row table the TLAS was
    // written with and the GPU scene's rows, FLUSHED first (a row staged but not
    // uploaded is a zero address — the trap file's GPU SCENE TABLES rule) and the
    // table pointer re-read here, never cached across a frame.
    {
        detail::GpuScene &gpuScn = scene->gpuScene();
        if (gpuScn.live()) gpuScn.flushGeomRows();
        Ogre::UavBufferPacked *rows = gpuScn.live() ? gpuScn.geomBuffer() : nullptr;
        if (rows && !sa.geomRowOfSlot.empty()) {
            in.geomRows = rows;
            in.geomRowOfSlot = &sa.geomRowOfSlot;
        }
    }

    // ---- THE CAMERA'S BASIS: the ray tier's one eye basis (eyeBasis), the
    // LETTERBOX folded in exactly as the reflection and the sun contact fold it
    // (expandEyeToTarget) — a probe's pixel and the shot's inner rectangle agree.
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb,
                           ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    EyeBasisF eye = eyeBasis(ortho, cam->getDerivedPosition(), q, float(fl), float(fr),
                             float(ft), float(fb));
    expandEyeToTarget(eye, view->chainDesc(), in.width, in.height);
    std::memcpy(in.camPos, eye.camPos, sizeof(in.camPos));
    std::memcpy(in.rayTL, eye.rayTL, sizeof(in.rayTL));
    std::memcpy(in.rayRight, eye.rayRight, sizeof(in.rayRight));
    std::memcpy(in.rayDown, eye.rayDown, sizeof(in.rayDown));
    std::memcpy(in.fwd, eye.fwd, sizeof(in.fwd));
    const auto put = [](float dst[3], const Ogre::Vector3 &v) {
        dst[0] = float(v.x); dst[1] = float(v.y); dst[2] = float(v.z);
    };
    put(in.viewAxisX, q * Ogre::Vector3::UNIT_X);
    put(in.viewAxisY, q * Ogre::Vector3::UNIT_Y);
    put(in.viewAxisZ, -(q * Ogre::Vector3::NEGATIVE_UNIT_Z));   // view space looks down -Z
    const Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    in.projA = float(projAB.x);
    in.projB = float(projAB.y);
    in.farClip = float(cam->getFarClipDistance());

    // THE HIT LIST (PHOTON-HIT-SHADE-1): a far hit, a mover's, a rigged item's
    // and a static hit no cache answers are appended for the decode.
    in.hit = hit;
    if (!mGather) mGather = new ScreenProbeGather(*this);
    mGather->record(key, in);
}

// ---------------------------------------------------------------------------
// HARD SUN CONTACT SHADOWS — the tier's half (PHOTON-RAYS-1, RY-R3;
// SPECS/photon/C4_RAYS_BEYOND_REFLECTIONS_DESIGN.md section 1).
//
// ONE DISPATCH PER VIEW FRAME in the reflect listener's bracket (the gather's
// shape): one ray per texel from the prepass' surface towards the sun, tMax the
// contact range, the caster copies of the near field only, opaque only; the
// answer an R8 texture the PBS pass folds into the sun's shadow term as
// min( map, ray ). Deterministic per pixel per frame: no sample sequence, no
// history, nothing to reproject.
namespace {

/// The parameter block, std140, mirroring rq_sun_contact.comp's `Params`
/// member for member (every member a vec4, laid out identically to C).
struct SunContactParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float toSun[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
};
constexpr unsigned kSunContactBindings = 5u;
/// THE LIFT, in full-resolution pixel footprints: the near copy's tolerance
/// (kRayFootprintTolerance, one) plus one of margin — see rq_sun_contact.comp.
constexpr float kSunContactLiftFootprints = 2.0f;
/// THE FADE BAND: the last 20 % of the range hands the ray's answer over to the
/// map (the design's 10-20 %; the SSR rim's shape) — rq_sun_contact.comp.
/// Measured on the side edge of a 5 cm board's shadow at 15 m (gi.sun_contact,
/// the adjacent 5 cm step across the range's end): 15.1 codes with no fade,
/// 7.6 at 10 %, 5.4 at 15 %, 3.3-4.7 at 20 % (the map's own edge: 1.1-1.3).
constexpr float kSunContactFadeFraction = 0.2f;
/// THE BIAS RULE's floor, world units: the lift never falls below a millimetre
/// however close the camera stands (depth reconstruction's own precision).
constexpr float kSunContactMinBias = 0.001f;

/// The storage format of the visibility texture. R8 is an EXTENDED storage
/// format (shaderStorageImageExtendedFormats), not a core-mandatory one — so it
/// is asked of the device here, once, and a device that cannot store it runs
/// without the contact term rather than refusing the whole ray tier (the
/// tier-wide table in rayStorageFormatRefused is for the images every ray job
/// needs).
bool sunContactFormatStores(VkPhysicalDevice pd) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8_UNORM, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

}   // namespace

bool RayQueryTier::makeSunContactPipeline(std::string &err) {
    if (!mDev || !mDev->mPhysicalDevice || !sunContactFormatStores(mDev->mPhysicalDevice)) {
        err = "the device cannot store to R8_UNORM (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)";
        return false;
    }
    VkDescriptorSetLayoutBinding b[kSunContactBindings] = {};
    const VkDescriptorType types[kSunContactBindings] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0 tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1 params
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 2 gBufNormals
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3 sceneDepth
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 4 sunVis
    };
    for (unsigned i = 0; i < kSunContactBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kSunContactBindings;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(mVk, &sli, nullptr, &mSunSetLayout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mSunSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mSunPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_sunContactSpv);
    smi.pCode = krq_sunContactSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mSunModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mSunModule;
    cpi.stage.pName = "main";
    cpi.layout = mSunPipeLayout;
    if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &cpi, nullptr, &mSunPipeline) != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }
    // kMaxTimedScenes views' worth of rings — the reflection's ceiling.
    const unsigned sets = kMaxTimedScenes * kReflectRing;
    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = sets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = sets;
    sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[2].descriptorCount = sets * 2u;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[3].descriptorCount = sets;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mVk, &dpi, nullptr, &mSunPool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool failed";
        return false;
    }
    // POINT: the depth and the normal are read at exactly one texel.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(mVk, &si, nullptr, &mSunSampler) != VK_SUCCESS) {
        err = "vkCreateSampler failed";
        return false;
    }
    if (mTimestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxTimedScenes * kFramesInFlight * 2u;
        vkCreateQueryPool(mVk, &qci, nullptr, &mSunTimestamps);
    }
    return true;
}

void RayQueryTier::readSunContactTimestamps(SunContactView &sv) {
    if (!mSunTimestamps || !sv.hasQueryBase) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        SunContactView::Pending &pd = sv.pending[i];
        // The reflection's rule, and its reason: `<`, not `<=` (the ring is
        // kFramesInFlight deep and a slot is reused after that many frames).
        if (!pd.live || uint32_t(now - pd.frame) < inFlight) continue;
        uint64_t v[4] = {};
        const uint32_t base = sv.queryBase + i * 2u;
        if (vkGetQueryPoolResults(mVk, mSunTimestamps, base, 2, sizeof(v), v, sizeof(uint64_t) * 2u,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                VK_SUCCESS &&
            v[1] && v[3] && v[2] >= v[0])
            sv.gpuMs = float(double(v[2] - v[0]) * double(mTimestampPeriod) * 1e-6);
        pd.live = false;
    }
}

void RayQueryTier::dropSunContact(SunContactView &sv) {
    // THE REGISTRATION FIRST: the listener holds a raw pointer to the texture
    // that is about to be retired (GATHER-0's D1, the same rule).
    if (sv.sceneMgr) FogHlmsListener::setSunContact(sv.sceneMgr, nullptr, 1u);
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(sv.sets[i], mSunPool);
        sv.sets[i] = VK_NULL_HANDLE;
        retire(sv.params[i]);
    }
    retireTexture(sv.vis);
    if (sv.hasQueryBase) mSunQuerySlots &= ~(uint32_t(1) << sv.querySlot);
    sv.hasQueryBase = false;
}

void RayQueryTier::forgetSunContact(const ReflectPassListener *key) {
    auto it = mSunContacts.find(key);
    if (it == mSunContacts.end()) return;
    dropSunContact(it->second);
    mSunContacts.erase(it);
}

void RayQueryTier::finishSunContact(const ReflectPassListener *key) {
    auto it = mSunContacts.find(key);
    if (it == mSunContacts.end() || !it->second.finishPending) return;
    SunContactView &sv = it->second;
    sv.finishPending = false;
    if (!sv.vis) return;
    // ...AND NOW THE PIXEL SHADER READS IT: ours to order, since the scene
    // pass does not know the texture exists (it arrives through the listener).
    {
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, sv.vis, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        mRs->executeResourceTransition(trans);
    }
    // THE PROPERTY AND THE TEXTURE TOGETHER, PASS-SCOPED: taken away by
    // releaseSunContactBinding when this pass ends.
    FogHlmsListener::setSunContact(sv.sceneMgr, sv.vis, sv.divisor);
}

void RayQueryTier::releaseSunContactBinding(const ReflectPassListener *key) {
    auto it = mSunContacts.find(key);
    if (it == mSunContacts.end() || !it->second.sceneMgr) return;
    FogHlmsListener::setSunContact(it->second.sceneMgr, nullptr, 1u);
}

void RayQueryTier::sunContactStatsInto(const OgreScene *scene, SunContactStatus &st) const {
    for (const auto &kv : mSunContacts) {
        const SunContactView &sv = kv.second;
        if (sv.scene != scene) continue;
        if (!sv.ran) {
            if (st.reason.empty()) st.reason = sv.reason;
            continue;
        }
        st.running = true;
        st.width = sv.w; st.height = sv.h;
        st.targetW = sv.fullW; st.targetH = sv.fullH;
        st.divisor = sv.divisor;
        st.rays += sv.rays;
        st.range = sv.range;
        for (int i = 0; i < 3; ++i) st.toSun[i] = sv.toSun[i];
        if (sv.gpuMs > st.gpuMs) st.gpuMs = sv.gpuMs;
        if (sv.cpuMs > st.cpuMs) st.cpuMs = sv.cpuMs;
        st.reason.clear();
    }
    if (!st.running && mSunFailed) st.reason = mSunFailReason;
}

void RayQueryTier::recordSunContact(const ReflectPassListener *key, OgreView *view,
                                    Ogre::CompositorPass *pass) {
    if (!isOpen() || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;
    if (!scene->sunContactWanted()) {
        // OFF IS FREE AND CLEAN: a view that was casting and stopped gives its
        // texture back and stops binding it (the gather's rule).
        forgetSunContact(key);
        return;
    }
    const auto cpuStart = Clock::now();
    SunContactView &sv = mSunContacts[key];
    sv.scene = scene;
    sv.sceneMgr = scene->mSceneMgr;
    sv.ran = false;
    sv.rays = 0;
    /// EVERY EARLY RETURN IS A LEGITIMATE "not this frame": nothing is
    /// registered, the property is not set, and the pass shades with the shadow
    /// map alone — the picture without the row. The reason is kept for the
    /// status, because a row that silently does nothing is the worst outcome.
    const auto decline = [&sv](const char *why) { sv.reason = why; };
    readSunContactTimestamps(sv);

    auto sceneIt = mScenes.find(scene);
    if (sceneIt == mScenes.end()) { decline("the scene has no ray structures yet"); return; }
    SceneAs &sa = sceneIt->second;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) {
        decline("the scene's top-level structure is empty");
        return;
    }
    // NEVER IN VR (the design's VR column): the chain already declines the row
    // for a stereo view (`ChainDesc::sunContact`); this is the job's own copy of
    // the rule, so a two-eye target is never traced as one image.
    if (view->stereo()) { decline("a stereo view (never in VR)"); return; }
    if (mSunFailed) { decline("the contact pipeline is unavailable on this device"); return; }
    if (!mSunPipeline) {
        std::string err;
        if (!makeSunContactPipeline(err)) {
            mSunFailed = true;
            mSunFailReason = "sun contact off: " + err;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: " + mSunFailReason + " (the shadow map renders alone)");
            decline("the contact pipeline is unavailable on this device");
            return;
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: sun contact shadows ON (PHOTON R3) — one ray per texel towards the sun");
    }

    // ---- THE SUN: the pass' first directional SHADOW CASTER ----------------
    // The one the pixel's first-light term belongs to: HlmsPbs writes the
    // directional casters into `light0Buf.lights[]` in the shadow node's order
    // (OgreHlmsPbs.cpp fillBuffersFor), and the prepass' shadow term — the
    // fShadow the answer is folded into — is that light's map. A pass with no
    // shadow node, or a node holding no directional caster, has no sun term to
    // correct (the piece's hlms_num_shadow_map_lights gate says the same).
    //
    // THE LIST IS CURRENT BECAUSE OF THE PREPASS, not because of this pass (audit
    // F6): this runs in passPreExecute, BEFORE the PrePassUse pass' own
    // `shadowNode->_update` (OgreCompositorPassScene.cpp:238 vs :259). It is this
    // frame's list only because the PrePassCreate pass names the SAME shadow
    // node (OgreChain.cpp, SHADOW_NODE_FIRST_ONLY) and ran first. A chain whose
    // prepass stops naming the node would hand this read last frame's casters.
    Ogre::Vector3 toSun = Ogre::Vector3::ZERO;
    {
        const Ogre::CompositorShadowNode *sn =
            static_cast<Ogre::CompositorPassScene *>(pass)->getShadowNode();
        if (sn) {
            for (const Ogre::LightClosest &lc : sn->getShadowCastingLights()) {
                if (lc.light && lc.light->getType() == Ogre::Light::LT_DIRECTIONAL) {
                    toSun = -lc.light->getDerivedDirection();
                    break;
                }
            }
        }
    }
    if (toSun.squaredLength() < 1e-12f) { decline("no directional shadow caster in the pass"); return; }
    toSun.normalise();

    // ---- THE CHAIN'S TEXTURES (the gather's two, by the same names) --------
    Ogre::TextureGpu *normalTex = nullptr, *depthTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        depthTex = node->getDefinedTexture(Ogre::IdString("jahDepth"));
    } catch (Ogre::Exception &) { decline("the chain carries no prepass"); return; }
    if (!normalTex || !depthTex) { decline("the chain carries no prepass"); return; }
    const unsigned fullW = depthTex->getWidth(), fullH = depthTex->getHeight();
    if (!fullW || !fullH) return;

    // ---- THE RESOLUTION: the row, or the tier's (half at Low and Medium) ----
    const SunContactDesc &row = scene->sunContact();
    unsigned divisor = 1u;
    switch (row.resolution) {
    case SunContactResolution::Full: divisor = 1u; break;
    case SunContactResolution::Half: divisor = 2u; break;
    case SunContactResolution::Auto:
    default: divisor = scene->giParams().quality == GiQuality::High ? 1u : 2u; break;
    }
    // CEILING, so every pixel's `iFragCoord / divisor` lands inside the texture.
    const unsigned w = (fullW + divisor - 1u) / divisor, h = (fullH + divisor - 1u) / divisor;

    // ---- THE TEXTURE, resident for good and remade only on a resize --------
    if (!sv.vis || sv.w != w || sv.h != h) {
        FogHlmsListener::setSunContact(sv.sceneMgr, nullptr, 1u);
        retireTexture(sv.vis);
        // `Uav` and nothing else — the gather's irradiance target's reasoning:
        // born in GENERAL with the pin's own barrier, SAMPLED because it is a
        // texture, never Reinterpretable.
        Ogre::TextureGpuManager *tm = mRs->getTextureGpuManager();
        static unsigned sSerial = 0u;
        Ogre::TextureGpu *t = tm->createTexture("JahSunContact/" + std::to_string(++sSerial),
                                                Ogre::GpuPageOutStrategy::Discard,
                                                Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
        t->setResolution(w, h, 1u);
        t->setPixelFormat(Ogre::PFG_R8_UNORM);
        t->setNumMipmaps(1u);
        t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        sv.vis = t;
        sv.w = w;
        sv.h = h;
    }
    sv.fullW = fullW;
    sv.fullH = fullH;
    sv.divisor = divisor;

    if (!sv.hasQueryBase && mSunTimestamps) {
        for (unsigned s = 0; s < kMaxTimedScenes; ++s) {
            if (mSunQuerySlots & (uint32_t(1) << s)) continue;
            mSunQuerySlots |= uint32_t(1) << s;
            sv.querySlot = s;
            sv.queryBase = s * kFramesInFlight * 2u;
            sv.hasQueryBase = true;
            break;
        }
    }

    std::string err;
    const unsigned ring = sv.frame % kReflectRing;
    if (!sv.params[ring].buffer &&
        !makeBuffer(sizeof(SunContactParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    sv.params[ring], err)) {
        decline("the parameter buffer could not be made");
        return;
    }
    if (!sv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mSunPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mSunSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &sv.sets[ring]) != VK_SUCCESS) {
            sv.sets[ring] = VK_NULL_HANDLE;
            decline("no descriptor set");
            return;
        }
    }

    // ---- THE PARAMETERS: the ray tier's eye basis, letterbox folded in -----
    SunContactParams pp{};
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb,
                           ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    EyeBasisF eye = eyeBasis(ortho, cam->getDerivedPosition(), q, float(fl), float(fr),
                             float(ft), float(fb));
    expandEyeToTarget(eye, view->chainDesc(), fullW, fullH);
    memcpy(pp.camPos, eye.camPos, sizeof(pp.camPos));
    memcpy(pp.rayTL, eye.rayTL, sizeof(pp.rayTL));
    memcpy(pp.rayRight, eye.rayRight, sizeof(pp.rayRight));
    memcpy(pp.rayDown, eye.rayDown, sizeof(pp.rayDown));
    memcpy(pp.fwd, eye.fwd, sizeof(pp.fwd));
    const Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    pp.projParams[0] = projAB.x;
    pp.projParams[1] = projAB.y;
    pp.projParams[2] = cam->getFarClipDistance();
    pp.projParams[3] = kSunContactFadeFraction;
    put3(pp.viewAxisX, q * Ogre::Vector3::UNIT_X, 0.0f);
    put3(pp.viewAxisY, q * Ogre::Vector3::UNIT_Y, 0.0f);
    put3(pp.viewAxisZ, -(q * Ogre::Vector3::NEGATIVE_UNIT_Z), 0.0f);   // view space looks down -Z
    const float range = std::min(std::max(row.range, kSunContactMinRange), kSunContactMaxRange);
    put3(pp.toSun, toSun, range);
    pp.resolution[0] = float(w);
    pp.resolution[1] = float(h);
    pp.resolution[2] = float(fullW);
    pp.resolution[3] = float(fullH);
    // THE BIAS RULE's lift: kSunContactLiftFootprints FULL-RESOLUTION pixel
    // footprints — per unit of view distance for a perspective camera (the
    // basis is a ray whose forward component is 1), in world units for an
    // orthographic one. Full resolution whatever the divisor: what the lift
    // clears is the near copy's own tolerance, which the ray rule states in the
    // VIEW's pixels (kRayFootprintTolerance), not in this job's texels.
    {
        const float span = std::sqrt(eye.rayRight[0] * eye.rayRight[0] +
                                     eye.rayRight[1] * eye.rayRight[1] +
                                     eye.rayRight[2] * eye.rayRight[2]);
        pp.knobs[0] = kSunContactLiftFootprints * span / float(fullW);
    }
    pp.knobs[1] = kSunContactMinBias;
    // THE MASK: the shadow CASTERS' near copies (kRayMaskCaster) — a subset of
    // the near field, which is the one field a ray this short lives in (A5b §4:
    // one field per ray; the far copies are the coarse levels past the near
    // length). An object whose shadows are off must cast no contact shadow
    // either.
    pp.knobs[2] = float(kRayMaskCaster);
    pp.knobs[3] = float(divisor);
    memcpy(sv.params[ring].mapped, &pp, sizeof(pp));
    sv.range = range;
    sv.toSun[0] = toSun.x; sv.toSun[1] = toSun.y; sv.toSun[2] = toSun.z;

    // ---- THE DESCRIPTOR SET, rewritten every frame (uncached, retired views —
    // the reflection's measured trap about a cached view of a recreated texture)
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo ub{};
    ub.buffer = sv.params[ring].buffer;
    ub.range = sizeof(SunContactParams);
    VkDescriptorImageInfo normals{}, depth{}, store{};
    normals.sampler = mSunSampler;
    normals.imageView = sampledView(normalTex);
    normals.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth.sampler = mSunSampler;
    depth.imageView = sampledView(depthTex);
    depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = sv.vis;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = sv.vis->getPixelFormat();
        store.imageView = static_cast<Ogre::VulkanTextureGpu *>(sv.vis)->createView(slot, false);
        retireView(store.imageView);
        store.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!normals.imageView || !depth.imageView || !store.imageView) {
        decline("an image view could not be made");
        return;
    }
    VkWriteDescriptorSet wds[kSunContactBindings] = {};
    for (unsigned i = 0; i < kSunContactBindings; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = sv.sets[ring];
        wds[i].dstBinding = i;
        wds[i].descriptorCount = 1;
    }
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wds[0].pNext = &asWrite;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wds[1].pBufferInfo = &ub;
    wds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[2].pImageInfo = &normals;
    wds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[3].pImageInfo = &depth;
    wds[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[4].pImageInfo = &store;
    vkUpdateDescriptorSets(mVk, kSunContactBindings, wds, 0, nullptr);

    // ---- THE LAYOUTS, THROUGH OGRE'S OWN SOLVER, before the command buffer
    // is taken (executeResourceTransition closes every encoder and may roll the
    // queue over — the reflection's recorded reason).
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, sv.vis, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::Write, computeStage);
        for (Ogre::TextureGpu *t : { normalTex, depthTex })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }

    // ---- THE DISPATCH -------------------------------------------------------
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) { decline("no command buffer"); return; }
    {
        // The frame monitor's row beside the gather's (`sun.contact`): free
        // while the monitor is off, a GPU-timed row in a capture. `Camera`,
        // not `None`: a view-dependent answer re-made every frame because the
        // thing it describes is the picture.
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "sun.contact", mRs);
        work.setUnits(w * h / 1000u);
        const bool timed = mSunTimestamps && sv.hasQueryBase;
        const uint32_t base = sv.queryBase + (sv.frame % kFramesInFlight) * 2u;
        if (timed) {
            vkCmdResetQueryPool(cmd, mSunTimestamps, base, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSunTimestamps, base);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSunPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSunPipeLayout, 0, 1,
                                &sv.sets[ring], 0, nullptr);
        vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1u);
        if (timed) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mSunTimestamps, base + 1u);
            SunContactView::Pending &pd = sv.pending[sv.frame % kFramesInFlight];
            pd.frame = frameNow();
            pd.live = true;
        }
    }

    // THE REGISTRATION IS THE SECOND HALF'S (finishSunContact, in front of the
    // opaque pass that reads it — PHOTON-HIT-SHADE-1 split the ray jobs around
    // the hit decode pass).
    sv.finishPending = true;
    sv.ran = true;
    sv.reason.clear();
    sv.rays = (unsigned long long)w * h;
    sv.cpuMs = float(msSince(cpuStart));
    ++sv.frame;
}

// ---------------------------------------------------------------------------
// THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4) — the ray tier's half. The
// surface cache selects the cards (OgreSurfaceCache.cpp, "The movers' shadow")
// and hands over their records in the relight's layout; this records one ray
// per texel towards the sun against the shadow-casting movers' near copies
// (kRayMaskMoverCaster) into the cache's R8 mover-visibility layer.
namespace {
struct CardMoverParams {
    float toSun[4] = {};
    float knobs[4] = {};
};
constexpr unsigned kCardMoverBindings = 6u;
/// Four timestamps a frame: the trace's pair and the relight's pair.
constexpr unsigned kCardMoverQueries = 4u;
/// The records ring's capacity: the relight list's (kMaxRelights, 80 B a card).
constexpr unsigned kCardMoverMaxRecords = 1024u;
constexpr unsigned kCardMoverRecordFloats = 20u;
}   // namespace

bool RayQueryTier::makeCardMoverPipeline(std::string &err) {
    if (!mDev || !mDev->mPhysicalDevice || !sunContactFormatStores(mDev->mPhysicalDevice)) {
        err = "the device cannot store to R8_UNORM (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)";
        return false;
    }
    if (!ensureSamplers(err)) return false;
    VkDescriptorSetLayoutBinding b[kCardMoverBindings] = {};
    const VkDescriptorType types[kCardMoverBindings] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0 tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1 params
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 2 the card records
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3 cardDepth
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 4 cardNormal
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 5 moverVis
    };
    for (unsigned i = 0; i < kCardMoverBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kCardMoverBindings;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(mVk, &sli, nullptr, &mCmSetLayout) != VK_SUCCESS) {
        err = "vkCreateDescriptorSetLayout failed";
        return false;
    }
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mCmSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mCmPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_cardMoversSpv);
    smi.pCode = krq_cardMoversSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mCmModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mCmModule;
    cpi.stage.pName = "main";
    cpi.layout = mCmPipeLayout;
    if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &cpi, nullptr, &mCmPipeline) != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }
    const unsigned sets = kMaxTimedScenes * kReflectRing;
    VkDescriptorPoolSize sizes[5] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = sets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = sets;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[2].descriptorCount = sets;
    sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[3].descriptorCount = sets * 2u;
    sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[4].descriptorCount = sets;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 5;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mVk, &dpi, nullptr, &mCmPool) != VK_SUCCESS) {
        err = "vkCreateDescriptorPool failed";
        return false;
    }
    if (mTimestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxTimedScenes * kFramesInFlight * kCardMoverQueries;
        vkCreateQueryPool(mVk, &qci, nullptr, &mCmTimestamps);
    }
    return true;
}

bool RayQueryTier::cardMoverQueries(CardMoverView &cv) {
    if (!mCmTimestamps) return false;
    if (cv.hasQueryBase) return true;
    for (unsigned s = 0; s < kMaxTimedScenes; ++s) {
        if (mCmQuerySlots & (uint32_t(1) << s)) continue;
        mCmQuerySlots |= uint32_t(1) << s;
        cv.querySlot = s;
        cv.queryBase = s * kFramesInFlight * kCardMoverQueries;
        cv.hasQueryBase = true;
        return true;
    }
    return false;
}

void RayQueryTier::readCardMoverTimestamps(CardMoverView &cv) {
    if (!mCmTimestamps || !cv.hasQueryBase) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        CardMoverView::Pending &pd = cv.pending[i];
        if ((!pd.trace && !pd.relight) || uint32_t(now - pd.frame) < inFlight) continue;
        const uint32_t base = cv.queryBase + i * kCardMoverQueries;
        for (unsigned pair = 0; pair < 2u; ++pair) {
            bool &live = pair ? pd.relight : pd.trace;
            if (!live) continue;
            uint64_t v[4] = {};
            if (vkGetQueryPoolResults(mVk, mCmTimestamps, base + pair * 2u, 2, sizeof(v), v,
                                      sizeof(uint64_t) * 2u,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
                    VK_SUCCESS &&
                v[1] && v[3] && v[2] >= v[0]) {
                const float ms = float(double(v[2] - v[0]) * double(mTimestampPeriod) * 1e-6);
                (pair ? cv.relightMs : cv.traceMs) = ms;
            }
            live = false;
        }
    }
}

bool RayQueryTier::traceCardMovers(OgreScene *scene, const CardMoverTrace &job) {
    if (!isOpen() || !scene || !job.count || !job.records || !job.depth || !job.normal || !job.vis)
        return false;
    auto sceneIt = mScenes.find(scene);
    if (sceneIt == mScenes.end()) return false;
    SceneAs &sa = sceneIt->second;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) return false;
    if (mCmFailed) return false;
    if (!mCmPipeline) {
        std::string err;
        if (!makeCardMoverPipeline(err)) {
            mCmFailed = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: card mover shadows off: " + err + " (the cards hold the still world's sun term alone)");
            return false;
        }
    }
    CardMoverView &cv = mCardMovers[scene];
    readCardMoverTimestamps(cv);
    const unsigned count = std::min(job.count, kCardMoverMaxRecords);
    std::string err;
    const unsigned ring = cv.frame % kReflectRing;
    if (!cv.params[ring].buffer &&
        !makeBuffer(sizeof(CardMoverParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    cv.params[ring], err))
        return false;
    const VkDeviceSize recBytes = VkDeviceSize(kCardMoverMaxRecords) * kCardMoverRecordFloats * sizeof(float);
    if (!cv.records[ring].buffer &&
        !makeBuffer(recBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, cv.records[ring], err))
        return false;
    if (!cv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mCmPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mCmSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &cv.sets[ring]) != VK_SUCCESS) {
            cv.sets[ring] = VK_NULL_HANDLE;
            return false;
        }
    }
    CardMoverParams pp{};
    pp.toSun[0] = job.toSun.x; pp.toSun[1] = job.toSun.y; pp.toSun[2] = job.toSun.z;
    pp.toSun[3] = job.range;
    pp.knobs[0] = float(kRayMaskMoverCaster);
    memcpy(cv.params[ring].mapped, &pp, sizeof(pp));
    memcpy(cv.records[ring].mapped, job.records, size_t(count) * kCardMoverRecordFloats * sizeof(float));

    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo ub{}, rb{};
    ub.buffer = cv.params[ring].buffer;
    ub.range = sizeof(CardMoverParams);
    rb.buffer = cv.records[ring].buffer;
    rb.range = recBytes;
    VkDescriptorImageInfo depth{}, normal{}, store{};
    depth.sampler = mPointSampler;
    depth.imageView = sampledView(job.depth);
    depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    normal.sampler = mPointSampler;
    normal.imageView = sampledView(job.normal);
    normal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.vis;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = job.vis->getPixelFormat();
        store.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.vis)->createView(slot, false);
        retireView(store.imageView);
        store.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!depth.imageView || !normal.imageView || !store.imageView) return false;
    VkWriteDescriptorSet wds[kCardMoverBindings] = {};
    for (unsigned i = 0; i < kCardMoverBindings; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = cv.sets[ring];
        wds[i].dstBinding = i;
        wds[i].descriptorCount = 1;
    }
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wds[0].pNext = &asWrite;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wds[1].pBufferInfo = &ub;
    wds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[2].pBufferInfo = &rb;
    wds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[3].pImageInfo = &depth;
    wds[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[4].pImageInfo = &normal;
    wds[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[5].pImageInfo = &store;
    vkUpdateDescriptorSets(mVk, kCardMoverBindings, wds, 0, nullptr);

    // THE LAYOUTS through Ogre's own solver, before the command buffer is taken:
    // the capture's copies wrote Depth and Normal this frame; the relight that
    // follows reads the layer as a UAV and its analyzeBarriers orders it after
    // this write (the solver now knows the layer was written in compute).
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, job.vis, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::Write, computeStage);
        for (Ogre::TextureGpu *t : { job.depth, job.normal })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return false;
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Caster, 0, "cards.movers", mRs);
        work.setUnits(count);
        const bool timed = cardMoverQueries(cv);
        const unsigned slot = frameNow() % kFramesInFlight;
        const uint32_t base = cv.queryBase + slot * kCardMoverQueries;
        if (timed) {
            vkCmdResetQueryPool(cmd, mCmTimestamps, base, 2);
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mCmTimestamps, base);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCmPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCmPipeLayout, 0, 1,
                                &cv.sets[ring], 0, nullptr);
        vkCmdDispatch(cmd, kCardPageSize / 8u, kCardPageSize / 8u, count);
        if (timed) {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mCmTimestamps, base + 1u);
            CardMoverView::Pending &pd = cv.pending[slot];
            if (pd.frame != frameNow()) { pd.frame = frameNow(); pd.relight = false; }
            pd.trace = true;
        }
    }
    ++cv.frame;
    return true;
}

void RayQueryTier::timeCardRelight(OgreScene *scene, bool begin) {
    if (!isOpen() || !mCmTimestamps || !scene) return;
    auto it = mCardMovers.find(scene);
    if (it == mCardMovers.end()) return;   // timed only once the scene has traced
    CardMoverView &cv = it->second;
    if (!cardMoverQueries(cv)) return;
    const unsigned slot = frameNow() % kFramesInFlight;
    const uint32_t base = cv.queryBase + slot * kCardMoverQueries + 2u;
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    if (begin) {
        vkCmdResetQueryPool(cmd, mCmTimestamps, base, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mCmTimestamps, base);
    } else {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mCmTimestamps, base + 1u);
        CardMoverView::Pending &pd = cv.pending[slot];
        if (pd.frame != frameNow()) { pd.frame = frameNow(); pd.trace = false; }
        pd.relight = true;
    }
}

void RayQueryTier::cardMoverTimes(OgreScene *scene, float &traceMs, float &relightMs) {
    auto it = mCardMovers.find(scene);
    if (it == mCardMovers.end()) return;
    readCardMoverTimestamps(it->second);
    traceMs = it->second.traceMs;
    relightMs = it->second.relightMs;
}

void RayQueryTier::forgetCardMovers(OgreScene *scene) {
    auto it = mCardMovers.find(scene);
    if (it == mCardMovers.end()) return;
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(it->second.sets[i], mCmPool);
        retire(it->second.params[i]);
        retire(it->second.records[i]);
    }
    if (it->second.hasQueryBase) mCmQuerySlots &= ~(uint32_t(1) << it->second.querySlot);
    mCardMovers.erase(it);
}

bool OgreScene::traceCardMovers(const CardMoverTrace &job) {
    return mEngine && mEngine->mRayTier && mEngine->mRayTier->traceCardMovers(this, job);
}
void OgreScene::timeCardRelight(bool begin) {
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->timeCardRelight(this, begin);
}
void OgreScene::cardMoverTimes(float &traceMs, float &relightMs) {
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->cardMoverTimes(this, traceMs, relightMs);
}

// ---------------------------------------------------------------------------
/// A DESTRUCTOR MAY NOT THROW — it is implicitly `noexcept`, so an exception
/// leaving it is `std::terminate`, and this one submits to the GPU (lane VR-3b,
/// 2026-09-17; the owner's WiVRn smoke died exactly here). The chain was:
///
///     renderOneFrame -> OgreView::syncReflectListener
///       -> ~ReflectPassListener -> RenderSystem::flushCommands
///       -> VulkanQueue::commitAndNextCommandBuffer -> vkQueueSubmit
///       -> VK_ERROR_DEVICE_LOST -> checkVkResult THROWS -> SIGABRT
///
/// ogre-patch 0069 made the same statement about the pin's own staging buffer;
/// this is OUR destructor and it is ours to make safe. Two rules, in order:
/// a lost device is not flushed at all (a submit on it can only fail, and the
/// engine's own frame tail already reports the loss), and anything that still
/// escapes is caught and logged — the process keeps its stack and its log
/// instead of aborting with neither.
ReflectPassListener::~ReflectPassListener() {
    if (!mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    try {
        // Same rule as dropReflectState: nothing may be freed while a command
        // buffer that has it bound is still recording.
        Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
        if (rs && !rs->isDeviceLost()) rs->flushCommands();
        mView->mEngine->mRayTier->forgetReflect(this);
        mView->mEngine->mRayTier->forgetGather(this);
        mView->mEngine->mRayTier->forgetSunContact(this);
        mView->mEngine->mRayTier->forgetHits(this);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the ray-reflection listener could not be flushed away cleanly (" +
            e.getDescription() + ") - the reflection resources are released without it",
            Ogre::LML_CRITICAL);
        // THE BOOKKEEPING STILL HAS TO HAPPEN, or the tier keeps a record keyed
        // by an address that is about to be freed and the next view allocated at
        // it inherits a stranger's images.
        try { mView->mEngine->mRayTier->forgetReflect(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetGather(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetSunContact(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetHits(this); } catch (...) {}
    } catch (...) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the ray-reflection listener's teardown threw a non-Ogre exception",
            Ogre::LML_CRITICAL);
        try { mView->mEngine->mRayTier->forgetReflect(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetGather(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetSunContact(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetHits(this); } catch (...) {}
    }
}

void ReflectPassListener::passPreExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def) return;
    // THE HIT DECODE PASS (PHOTON-HIT-SHADE-1, ChainDesc::hitDecode): every ray
    // job TRACES in front of it — the sun contact, the reflection, the gather —
    // because a hit no cache can shade is appended to the hit list this pass
    // shades; HlmsAtom is armed for its length (endHitDecode disarms it).
    if (def->mIdentifier == kHitDecodePassIdentifier) {
        mView->mEngine->mRayTier->beginHitDecode(this, mView, pass);
        return;
    }
    // THE PASS, BY WHAT IT DOES AND NOT BY ITS NAME. `PrePassUse` is Ogre's own
    // word for "this pass shades with a prepass' G-buffers and the ssr texture"
    // — the pass every ray job's answer is read by. The write-back, the
    // reflection's filter, the gather's filter/SH/integrate and every job's
    // pass-scoped registration run in front of it (and, where no decode pass ran
    // this frame, the traces first).
    if (def->mPrePassMode != Ogre::PrePassUse) return;
    mView->mEngine->mRayTier->finishRayJobs(this, mView, pass);
}

/// GATHER-0 (fix round, D2). The registration made in `passPreExecute` names
/// this view's full-resolution irradiance texture and is read by every colour
/// pass of the same SceneManager; it is valid for exactly ONE pass — the one
/// this listener runs in front of — so it is taken away the moment that pass
/// is over. What this removes is a whole class of wrong pictures and two ways
/// to lose a frame outright (see the declaration).
void ReflectPassListener::passPosExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def) return;
    if (def->mIdentifier == kHitDecodePassIdentifier) {
        mView->mEngine->mRayTier->endHitDecode(this);
        return;
    }
    if (def->mPrePassMode != Ogre::PrePassUse) return;
    mView->mEngine->mRayTier->releaseGatherBinding(this);
    mView->mEngine->mRayTier->releaseSunContactBinding(this);
}

void OgreView::dropReflectState() {
    if (!mReflectListener || !mEngine || !mEngine->mRayTier) return;
    // SUBMIT AND WAIT FIRST. A descriptor set may not be freed while a command
    // buffer that has it bound is still recording, and the textures it
    // references are about to be destroyed. This is not a frame path: a
    // workspace rebuild already destroys and recreates every texture in the
    // chain, so the flush costs nothing that was not already being paid.
    if (mRoot && mRoot->getRenderSystem()) mRoot->getRenderSystem()->flushCommands();
    mEngine->mRayTier->forgetReflect(mReflectListener.get());
    mEngine->mRayTier->forgetGather(mReflectListener.get());
    mEngine->mRayTier->forgetSunContact(mReflectListener.get());
    mEngine->mRayTier->forgetHits(mReflectListener.get());
}

void OgreView::syncReflectListener() {
    // THE CHAIN'S SHAPE IS RE-CHECKED HERE, once a frame, and that is not
    // belt-and-braces: `ChainDesc::rayReflect` depends on the SCENE (the
    // project's ray row, lane RAYROW-1) and a view has no scene when its chain
    // is first built, nor does `setScene` rebuild one. Without this a view
    // would render its whole life with the shape it was constructed with.
    // Cheap: a comparison against what the current definition was built with.
    // ...and `ChainDesc::probeGather` is the same story for the same reason
    // (GATHER-1a): the gather's row is the SCENE's, so a view whose scene turns
    // it on has to gain the prepass it reads its probes' surfaces from. One
    // `chainDesc()` for both comparisons.
    // ...and `ChainDesc::sunContact` (PHOTON-RAYS-1) the same again: the row
    // is the scene's and it brings the prepass.
    // ...AS THE PREPASS THEY ASK FOR (ChainDesc::prepass, PHOTON-GATHER-1d): a
    // gather or sun-contact toggle where the prepass already runs is not a new
    // graph and rebuilds nothing.
    if (mChainRayReflect != chainDesc().rayReflect || mChainPrepass != chainDesc().prepass() ||
        mChainHitDecode != chainDesc().hitDecode)
        rebuildWorkspaceDef();
    // The same arming rule as the planar and globals listeners, and the same
    // reason it is re-evaluated every frame: the shape above can change, and a
    // view can gain or lose its scene.
    //
    // AND `mEnabled` IS PART OF IT, WHICH IS NOT FREE AND IS NOT A CHOICE
    // (lane VR-3b, 2026-09-17, measured). Tearing the listener down when a view
    // merely blinks off costs a device-wide `flushCommands` — a queue submit,
    // from a destructor, in a frame that may be drawing nothing at all (a VR
    // session's first frames: the runtime answers "no picture", the session's
    // View goes off, the Player's is off by design and the editor's is hidden)
    // — plus the free and re-allocation of the reflection's rings. Keeping it
    // instead looks free and IS NOT: `dropReflect` also drops the trace's
    // TEMPORAL HISTORY, so retaining it across a disable changes the picture.
    // Measured, interleaved on one box: `scripting.e2e.movable_lamp_rest` went
    // from 12/12 passing to 2/14 with the listener retained (the floor probe
    // moves 7/255), and back to base with the teardown restored. So the
    // teardown stays, the destructor is made safe instead (below), and "a view
    // that blinks off keeps its reflection history" is a change that has to be
    // designed and measured, not slipped in.
    // ...AND THE GATHER ARMS IT TOO (GATHER-1a). One listener, two consumers:
    // both are recorded in front of the same `PrePassUse` pass, and the gather
    // does not need the reflection row — a chain may carry the prepass for the
    // probes alone (`ChainDesc::probeGather`). Without this term a project with
    // the gather on and SSR off would build the prepass and nothing would ever
    // run in front of it.
    const ChainDesc shape = chainDesc();
    const bool wanted = mEnabled && mScene && mCamera && mEngine && mEngine->mRayTier != nullptr &&
                        (shape.rayReflect || shape.probeGather || shape.sunContact);
    if (!wanted) {
        if (mReflectListener) {
            removeWorkspaceListener(mReflectListener.get());
            mReflectListener.reset();
        }
        return;
    }
    if (!mReflectListener) {
        mReflectListener.reset(new ReflectPassListener());
        addWorkspaceListener(mReflectListener.get());
    }
    mReflectListener->mView = this;
    mReflectListener->mRoot = mRoot;
}

// ---------------------------------------------------------------------------
// THE HIT DECODE — the tier's half (PHOTON-HIT-SHADE-1; SPECS/atom/
// D2_HIT_SHADING_DESIGN.md with the lead's F1-F8). A ray hit no cache can shade
// is a pixel of a second visibility buffer: the traces append it to the chain's
// hit list (jah_rq_hit_record.glsl), the chain's "Jahshaka hit decode" pass draws
// HlmsAtom's decode over the list in HIT MODE, and rq_hit_composite.comp scatters
// the decoded radiance into the reflection's mean and the gather's atlas — the
// same arithmetic a card- or voxel-lit hit takes. One copy of the lighting.
namespace {
/// The sun of a pass: its shadow node's first directional caster (the light
/// HlmsPbs's first-light shadow term belongs to — the sun contact's rule).
bool passSun(Ogre::CompositorPass *pass, Ogre::Vector3 &toSun) {
    toSun = Ogre::Vector3::ZERO;
    if (!pass || pass->getType() != Ogre::PASS_SCENE) return false;
    const Ogre::CompositorShadowNode *sn = static_cast<Ogre::CompositorPassScene *>(pass)->getShadowNode();
    if (!sn) return false;
    for (const Ogre::LightClosest &lc : sn->getShadowCastingLights()) {
        if (lc.light && lc.light->getType() == Ogre::Light::LT_DIRECTIONAL) {
            toSun = -lc.light->getDerivedDirection();
            break;
        }
    }
    if (toSun.squaredLength() < 1e-12f) return false;
    toSun.normalise();
    return true;
}
/// A read-only view of an Ogre UAV buffer as a raw descriptor.
VkDescriptorBufferInfo rawBufferInfo(Ogre::UavBufferPacked *b) {
    VkDescriptorBufferInfo info{};
    auto *bi = static_cast<Ogre::VulkanBufferInterface *>(b->getBufferInterface());
    info.buffer = bi->getVboName();
    info.offset = VkDeviceSize(b->_getFinalBufferStart()) * b->getBytesPerElement();
    info.range = b->getTotalSizeBytes();
    return info;
}
/// The shadow ray's minimum lift off a hit (metres): the hit point is exact on
/// the traced triangle to float precision, so a millimetre floor (and 1e-4 of the
/// distance, jah_rq_hit_record.glsl) clears it.
constexpr float kHitSunLift = 0.001f;
}   // namespace

bool RayQueryTier::ensureHitDummies(std::string &err) {
    if (mHitDummiesReady) return true;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R32G32B32A32_UINT, mHitDummyIds, err)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT, mHitDummyColour, err)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R32_UINT, mHitDummyDest, err)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R32G32_SFLOAT, mHitDummyDist, err)) return false;
    mHitDummiesReady = true;
    mHitDummiesNeedInit = true;
    return true;
}

void RayQueryTier::initHitDummies(VkCommandBuffer cmd) {
    if (!mHitDummiesNeedInit || !cmd) return;
    mHitDummiesNeedInit = false;
    VkImageMemoryBarrier b[4] = {};
    ReflectImage *imgs[4] = { &mHitDummyIds, &mHitDummyColour, &mHitDummyDest, &mHitDummyDist };
    for (int i = 0; i < 4; ++i) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = imgs[i]->image;
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = 1;
        b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 4, b);
}

void RayQueryTier::hitStandIns(HitListBinding &out) {
    out = HitListBinding();
    std::string err;
    if (!ensureHitDummies(err) || !ensureDummyImages(err)) return;
    out.ids = mHitDummyIds.view;
    out.dest = mHitDummyDest.view;
}

void RayQueryTier::readHitCounters(HitView &hv) {
    if (!hv.readback.mapped) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    uint32_t newest = 0u;
    bool any = false;
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        HitView::Pending &pd = hv.pending[i];
        if (!pd.live || uint32_t(now - pd.frame) < inFlight) continue;
        if (!any || int32_t(pd.frame - newest) > 0) {
            const uint32_t *w = reinterpret_cast<const uint32_t *>(
                static_cast<const unsigned char *>(hv.readback.mapped) + i * 16u);
            hv.appended = w[0];
            hv.dropped = w[1];
            newest = pd.frame;
            any = true;
        }
        pd.live = false;
    }
}

bool RayQueryTier::prepareHitList(const ReflectPassListener *key, OgreView *view,
                                  Ogre::CompositorPass *pass, HitListBinding &out) {
    hitStandIns(out);
    HitView &hv = mHits[key];
    hv.live = false;
    // `JAHSHAKA_HIT_LIST_OFF` — a MEASUREMENT switch, not a mode (read once): the
    // traces bind no list, so a hit no cache shades has no sample this frame (the
    // reflection keeps its history; the gather's ray reads zero) — the picture
    // before PHOTON-HIT-SHADE-1, for attributing a moved hash to the records.
    static const bool sListOff = std::getenv("JAHSHAKA_HIT_LIST_OFF") != nullptr;
    if (sListOff) return false;
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    Ogre::Camera *cam = view ? view->camera() : nullptr;
    if (!scene || !cam || !pass || !out.ids) return false;
    hv.scene = scene;
    readHitCounters(hv);
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return false;
    try {
        hv.ids = node->getDefinedTexture(Ogre::IdString("jahHitIds"));
        hv.dest = node->getDefinedTexture(Ogre::IdString("jahHitDest"));
        hv.radiance = node->getDefinedTexture(Ogre::IdString("jahHitRadiance"));
    } catch (Ogre::Exception &) { return false; }
    if (!hv.ids || !hv.dest || !hv.radiance || !hv.ids->isUav()) return false;
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::UavBufferPacked *instances = gs.live() ? gs.instanceBuffer() : nullptr;
    if (!instances) return false;
    Ogre::VaoManager *vao = mRs->getVaoManager();
    // THE LIST'S BUFFER, sized to the list: the four counter words, then two per
    // record. Re-made when the list grows (Ogre's destroy is delayed past every
    // frame in flight; every reader re-reads `hv.buf` before it binds).
    {
        const size_t words = 4u + 2u * size_t(hv.ids->getWidth()) * hv.ids->getHeight();
        if (hv.buf && hv.buf->getNumElements() < words) {
            vao->destroyUavBuffer(hv.buf);
            hv.buf = nullptr;
        }
        if (!hv.buf) hv.buf = vao->createUavBuffer(words, 4u, Ogre::BB_FLAG_READONLY, nullptr, false);
    }
    std::string err;
    if (!hv.readback.buffer &&
        !makeBuffer(VkDeviceSize(kFramesInFlight) * 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false,
                    hv.readback, err))
        return false;
    hv.capacity = hv.ids->getWidth() * hv.ids->getHeight();
    hv.width = hv.ids->getWidth();

    // THE LAYOUTS through Ogre's solver, before the command buffer is taken (the
    // reflection's rule: executeResourceTransition closes every encoder).
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { hv.ids, hv.dest })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Write,
                                     computeStage);
        solver.resolveTransition(trans, hv.buf, Ogre::ResourceAccess::ReadWrite, computeStage);
        solver.resolveTransition(trans, instances, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return false;
    initHitDummies(cmd);
    // THE COUNTER, ZEROED for this frame: every earlier reader (last frame's
    // decode, write-back, readback copy) is ordered before the transfer.
    const VkDescriptorBufferInfo bufInfo = rawBufferInfo(hv.buf);
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
        vkCmdFillBuffer(cmd, bufInfo.buffer, bufInfo.offset, 16u, 0u);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
    }
    const auto uavView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = t;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = t->getPixelFormat();
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    out.on = true;
    out.ids = uavView(hv.ids);
    out.dest = uavView(hv.dest);
    if (!out.ids || !out.dest) { hitStandIns(out); return false; }
    out.buf = bufInfo.buffer;
    out.bufOffset = bufInfo.offset;
    out.bufRange = bufInfo.range;
    const VkDescriptorBufferInfo instInfo = rawBufferInfo(instances);
    out.instances = instInfo.buffer;
    out.instancesOffset = instInfo.offset;
    out.instancesRange = instInfo.range;
    out.instanceEntries = gs.slotCount();
    out.capacity = hv.capacity;
    out.width = hv.width;
    Ogre::Vector3 toSun;
    out.sun = passSun(pass, toSun);
    out.toSun[0] = toSun.x; out.toSun[1] = toSun.y; out.toSun[2] = toSun.z;
    out.sunMask = kRayMaskCaster;
    // THE SUN RAY'S LENGTH: the camera's far plane (the world the frame draws),
    // floored so an unbounded or tiny far plane still reaches past a room.
    {
        const float farClip = float(cam->getFarClipDistance());
        out.sunRange = farClip > 0.0f ? std::max(farClip, 100.0f) : 10000.0f;
    }
    out.lift = kHitSunLift;
    {
        auto sit = mScenes.find(scene);
        out.farLift = sit != mScenes.end() ? std::max(sit->second.farOverlap, kHitSunLift) : kHitSunLift;
    }
    hv.live = true;
    return true;
}

bool RayQueryTier::decodeTwinsStale(OgreScene *scene) const {
    if (!scene) return false;
    auto it = mScenes.find(scene);
    if (it == mScenes.end()) return false;
    for (const SceneAs::DecodeWitness &w : it->second.decodeWitness) {
        if (w.slot >= scene->mItemNodes.size()) continue;
        const OgreScene::Node *nd = scene->mItemNodes[w.slot];
        if (!nd || !nd->item || !nd->item->getNumSubItems()) continue;
        const Ogre::SubItem *sub = nd->item->getSubItem(0);
        if (sub->getDatablock() != w.db || sub->getHlmsHash() != w.hash ||
            HlmsAtom::textureSetKeyOf(w.db) != w.texKey)
            return true;
    }
    return false;
}

void RayQueryTier::beginHitDecode(const ReflectPassListener *key, OgreView *view,
                                  Ogre::CompositorPass *pass) {
    if (!view || !pass) return;
    HitView &hv = mHits[key];
    // THE CAMERA'S AUTO ASPECT IS HELD OFF FOR THE DECODE PASS, ARMED OR NOT: the
    // pass renders into the list (W x 0.5625 H) whether or not it decodes, and an
    // auto-aspect camera is re-aspected by every PASS_SCENE it renders
    // (OgreCompositorPassScene.cpp) — the projection the pass' shaders read, and
    // the aspect Ogre's PlanarReflections matches cameras by (measured: a frame
    // whose decode was skipped re-aspected the camera and the VR eyes' PBS pass
    // changed permutation, vr.warmup). endHitDecode restores it.
    if (!hv.pinnedCam) {
        hv.pinnedCam = view->camera();
        if (hv.pinnedCam) {
            hv.camAuto = hv.pinnedCam->getAutoAspectRatio();
            hv.pinnedCam->setAutoAspectRatio(false);
        }
    }
    if (!isOpen()) return;
    hv.decodeFrame = frameNow();
    HitListBinding hit;
    prepareHitList(key, view, pass, hit);
    // THE TRACES, in front of the decode (their records are what it shades). The
    // sun contact rides the same hook: it reads the camera before this pass
    // re-aspects anything, and its registration waits for the opaque pass.
    recordSunContact(key, view, pass);
    if (view->chainDesc().rayReflect) recordReflect(key, view, pass, hit);
    recordGather(key, view, pass, hit);
    if (!hv.live) return;
    const bool reflected = mReflects.count(key) && mReflects[key].finishPending;
    const bool gathered = mGather && mGather->tracedAtlas(key) != VK_NULL_HANDLE;
    if (!reflected && !gathered) return;
    // A STALE TWIN NEVER DRAWS: a PBS datablock that changed its permutation since
    // updateScene's sync (the engine edits datablocks in place — the sky's manual
    // cube taken away when a probe grid binds, mid-frame) leaves its twin a copy
    // of the old state. A twin holding a manual cube under the pass' automatic
    // probe array cannot compile (OgreSky.cpp's env-probe note), and a shader made
    // to compile without it would still get the twin's BAKED texture set, one
    // texture larger than its root layout's range — a descriptor write past the
    // set (the pin's count assert is compiled out): the Xid 109s this lane's gate
    // measured. This frame's records go unshaded (the write-back's "unshaded"
    // answer); the next updateScene re-derives the twin.
    if (decodeTwinsStale(view->ogreScene())) return;
    // ARM THE DECODE for this pass alone: the source (the list and the GPU scene's
    // tables, re-read now — they are re-created when they grow), the scene's
    // decode draws shown, and the camera's auto aspect held off so the pass'
    // projection is the view's own (the target is the list, not a picture: F7).
    Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    OgreScene *scene = view->ogreScene();
    if (!atom || !scene) return;
    detail::GpuScene &gs = scene->gpuScene();
    if (!gs.live()) return;
    gs.flushGeomRows();
    HlmsAtom::DecodeSource src;
    src.ids = hv.ids;
    src.instances = gs.instanceBuffer();
    src.levels = gs.levelBuffer();
    src.geomRows = gs.geomBuffer();
    src.hitMode = true;
    src.hitBuf = hv.buf;
    atom->setDecodeSource(src);
    atom->showSceneDecodes(scene->mSceneMgr, true);
    hv.armedSm = scene->mSceneMgr;
}

void RayQueryTier::endHitDecode(const ReflectPassListener *key) {
    auto it = mHits.find(key);
    if (it == mHits.end()) return;
    HitView &hv = it->second;
    if (hv.pinnedCam) hv.pinnedCam->setAutoAspectRatio(hv.camAuto);
    hv.pinnedCam = nullptr;
    if (!hv.armedSm) return;
    Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
    if (auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr) {
        atom->showSceneDecodes(hv.armedSm, false);
        atom->setDecodeSource(HlmsAtom::DecodeSource());
    }
    hv.armedSm = nullptr;
}

void RayQueryTier::finishRayJobs(const ReflectPassListener *key, OgreView *view,
                                 Ogre::CompositorPass *pass) {
    if (!isOpen() || !view || !pass) return;
    // DISARMED WHATEVER HAPPENED: a decode pass that threw never reaches its
    // passPosExecute, and an armed HlmsAtom would decode every later pass of the
    // frame in hit mode (measured once: the capture passes compiled hit-mode
    // shaders against prepass state).
    endHitDecode(key);
    HitView &hv = mHits[key];
    if (hv.decodeFrame != frameNow()) {
        // NO DECODE PASS RAN THIS FRAME (a chain without one): the traces run
        // here, with no list bound — a hit no cache shades then has no sample
        // this frame (the write-back's "unshaded" answer).
        HitListBinding none;
        hitStandIns(none);
        hv.live = false;
        recordSunContact(key, view, pass);
        if (view->chainDesc().rayReflect) recordReflect(key, view, pass, none);
        recordGather(key, view, pass, none);
    }
    if (hv.live) recordHitComposite(key);
    hv.live = false;
    finishReflect(key);
    if (mGather) mGather->finish(key);
    finishSunContact(key);
}

void RayQueryTier::forgetHits(const ReflectPassListener *key) {
    auto it = mHits.find(key);
    if (it == mHits.end()) return;
    endHitDecode(key);
    HitView &hv = it->second;
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(hv.sets[i], mCompPool);
        hv.sets[i] = VK_NULL_HANDLE;
        retire(hv.params[i]);
    }
    retire(hv.readback);
    if (hv.buf && mRs && mRs->getVaoManager()) mRs->getVaoManager()->destroyUavBuffer(hv.buf);
    hv.buf = nullptr;
    mHits.erase(it);
}

void RayQueryTier::hitStatsInto(const OgreScene *scene, RayQueryStatus &st) const {
    Ogre::HlmsManager *hm = Ogre::Root::getSingletonPtr() ? Ogre::Root::getSingleton().getHlmsManager() : nullptr;
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    for (const auto &kv : mHits) {
        if (kv.second.scene != scene) continue;
        st.hitRecords += kv.second.appended;
        st.hitDropped += kv.second.dropped;
        st.hitCapacity = std::max<unsigned long long>(st.hitCapacity, kv.second.capacity);
    }
    if (atom && scene) st.hitDecodeDraws = int(atom->sceneDecodeCount(scene->mSceneMgr));
}

bool RayQueryTier::makeCompositePipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[kHitCompositeBindings] = {};
    const VkDescriptorType types[kHitCompositeBindings] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 the list's buffer (counters + aux)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 the destinations
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 the decoded radiance
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 the reflection's mean
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 5 ...its distances
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 6 the gather's atlas
    };
    for (unsigned i = 0; i < kHitCompositeBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1u;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sli{};
    sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sli.bindingCount = kHitCompositeBindings;
    sli.pBindings = b;
    if (vkCreateDescriptorSetLayout(mVk, &sli, nullptr, &mCompSetLayout) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreateDescriptorSetLayout failed";
        return false;
    }
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mCompSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mCompPipeLayout) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_hitCompositeSpv);
    smi.pCode = krq_hitCompositeSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mCompModule) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreateShaderModule failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mCompModule;
    cpi.stage.pName = "main";
    cpi.layout = mCompPipeLayout;
    if (vkCreateComputePipelines(mVk, VK_NULL_HANDLE, 1, &cpi, nullptr, &mCompPipeline) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreateComputePipelines failed";
        return false;
    }
    const unsigned sets = kMaxTimedScenes * kReflectRing;
    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = sets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = sets;
    sizes[2].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[2].descriptorCount = sets * 2u;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[3].descriptorCount = sets * 3u;
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mVk, &dpi, nullptr, &mCompPool) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreateDescriptorPool failed";
        return false;
    }
    return ensureSamplers(err);
}

void RayQueryTier::recordHitComposite(const ReflectPassListener *key) {
    auto it = mHits.find(key);
    if (it == mHits.end() || !it->second.live || mCompFailed) return;
    HitView &hv = it->second;
    if (!mCompPipeline) {
        std::string err;
        if (!makeCompositePipeline(err)) {
            mCompFailed = true;
            Ogre::LogManager::getSingleton().logMessage("Jahshaka: the hit write-back is off — " + err);
            return;
        }
    }
    const unsigned ring = hv.frame % kReflectRing;
    std::string err;
    if (!hv.params[ring].buffer &&
        !makeBuffer(16u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, hv.params[ring], err))
        return;
    if (!hv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mCompPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mCompSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &hv.sets[ring]) != VK_SUCCESS) {
            hv.sets[ring] = VK_NULL_HANDLE;
            return;
        }
    }
    // THE DESTINATIONS: the reflection's mean and distance of THIS frame (the
    // pair the trace wrote), the gather's atlas the trace wrote — or stand-ins
    // with the consumer's flag down (its records, if any, are skipped).
    VkImageView histView = mHitDummyColour.view, distView = mHitDummyDist.view, atlasView = mHitDummyColour.view;
    bool reflectBound = false, gatherBound = false;
    if (auto rit = mReflects.find(key); rit != mReflects.end() && rit->second.finishPending) {
        histView = rit->second.hist[rit->second.curIdx].view;
        distView = rit->second.dist[rit->second.curIdx].view;
        reflectBound = true;
    }
    if (mGather) {
        if (VkImageView a = mGather->tracedAtlas(key)) { atlasView = a; gatherBound = true; }
    }
    const float pp[4] = { float(hv.capacity), float(hv.width), reflectBound ? 1.0f : 0.0f,
                          gatherBound ? 1.0f : 0.0f };
    std::memcpy(hv.params[ring].mapped, pp, sizeof(pp));

    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkDescriptorBufferInfo ub{};
    ub.buffer = hv.params[ring].buffer;
    ub.range = 16u;
    const VkDescriptorBufferInfo bufInfo = rawBufferInfo(hv.buf);
    VkDescriptorImageInfo sampled[2] = {}, storage[3] = {};
    Ogre::TextureGpu *const src[2] = { hv.dest, hv.radiance };
    for (int i = 0; i < 2; ++i) {
        sampled[i].sampler = mPointSampler;
        sampled[i].imageView = sampledView(src[i]);
        sampled[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!sampled[i].imageView) return;
    }
    const VkImageView dst[3] = { histView, distView, atlasView };
    for (int i = 0; i < 3; ++i) {
        storage[i].imageView = dst[i];
        storage[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!storage[i].imageView) return;
    }
    VkWriteDescriptorSet w[kHitCompositeBindings] = {};
    for (unsigned i = 0; i < kHitCompositeBindings; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = hv.sets[ring];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &ub;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bufInfo;
    for (int i = 0; i < 2; ++i) {
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2 + i].pImageInfo = &sampled[i];
    }
    for (int i = 0; i < 3; ++i) {
        w[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4 + i].pImageInfo = &storage[i];
    }
    vkUpdateDescriptorSets(mVk, kHitCompositeBindings, w, 0, nullptr);

    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { hv.dest, hv.radiance })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                     computeStage);
        solver.resolveTransition(trans, hv.buf, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    // The traces' writes to the mean and the atlas (compute, before the decode
    // pass) are read and rewritten here.
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "hit.composite", mRs);
    work.setUnits(unsigned(hv.appended / 1000ull));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompPipeLayout, 0, 1, &hv.sets[ring], 0,
                            nullptr);
    // ONE THREAD PER RECORD SLOT; a thread past this frame's count returns at once.
    {
        const uint32_t groups = std::max(1u, (hv.capacity + 63u) / 64u);
        const uint32_t gx = std::min(groups, 65535u);
        vkCmdDispatch(cmd, gx, (groups + gx - 1u) / gx, 1u);
    }
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    // THE COUNTERS, on their way to the host (read several frames late).
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        VkBufferCopy region{};
        region.srcOffset = bufInfo.offset;
        region.dstOffset = VkDeviceSize(hv.frame % kFramesInFlight) * 16u;
        region.size = 8u;
        vkCmdCopyBuffer(cmd, bufInfo.buffer, hv.readback.buffer, 1, &region);
        VkMemoryBarrier h{};
        h.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        h.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        h.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &h, 0,
                             nullptr, 0, nullptr);
        HitView::Pending &pd = hv.pending[hv.frame % kFramesInFlight];
        pd.frame = frameNow();
        pd.live = true;
    }
    ++hv.frame;
}

// ---------------------------------------------------------------------------
// THE ENGINE SIDE. Three methods and one frame hook; everything above is
// private to this TU.
void OgreEngine::setRayTracing(bool on) {
    if (mRayTracingWanted == on) return;
    mRayTracingWanted = on;
    if (!on) shutdownRayQuery();
    Ogre::LogManager::getSingleton().logMessage(
        std::string("Jahshaka: hardware ray tracing switched ") + (on ? "ON" : "OFF") +
        " (the no-rays switch)");
}

// THE TIER'S STORAGE FORMATS, ASKED OF THE DEVICE (RAY-FMT-CHECK, PHOTON P3).
//
// Every image the tier writes from a compute shader is a STORAGE image: the
// reflection's temporal pair (the radiance mean, RGBA16F, and the distance pair,
// RG32F — `ensureReflectImages`) and the gather's atlas (RGBA16F). BOTH ARE
// CORE-MANDATORY STORAGE FORMATS in Vulkan 1.0 (the spec's Required Format
// Support; shaderStorageImageExtendedFormats covers the R16G16*, R16*, R8*,
// A2B10G10R10 and B10G11R11 family, not these), so on a conformant driver this
// check never refuses. It is kept as a DRIVER-DEFECT GUARD — one query per
// device, one log line — because the alternative on a driver that got the table
// wrong is `makeStorageImage` creating an image the device cannot store to and
// the first dispatch being undefined behaviour rather than a refusal. A device
// that lacks one is a no-rays device, and every consumer (the chain's
// rayReflect, the cards' Auto, the status) reads that same answer.
//
// JAHSHAKA_RAY_DENY_STORAGE_FORMAT names a format (R16G16B16A16_SFLOAT or
// R32G32_SFLOAT) to treat as unsupported: FAULT INJECTION, the refusal path's
// only door on conformant hardware (a measurement switch for
// gi.rt_reflect_format_refused_lavapipe, not a mode).
namespace {
struct RayStorageFormat { VkFormat format{}; const char *name{}; };
constexpr RayStorageFormat kRayStorageFormats[] = {
    { VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT" },   // the reflection mean, the gather atlas
    { VK_FORMAT_R32G32_SFLOAT,       "R32G32_SFLOAT" },         // the reflection's distance pair
};

/// Empty when every format stores; otherwise the first one that does not.
std::string rayStorageFormatRefused(VkPhysicalDevice pd) {
    const char *deny = std::getenv("JAHSHAKA_RAY_DENY_STORAGE_FORMAT");
    for (const RayStorageFormat &f : kRayStorageFormats) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(pd, f.format, &props);
        const bool stores = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        const bool denied = deny && std::strcmp(deny, f.name) == 0;
        if (!stores || denied) return std::string(f.name) + (denied ? " (denied by JAHSHAKA_RAY_DENY_STORAGE_FORMAT)" : "");
    }
    return std::string();
}
}   // namespace

bool OgreEngine::rayQueryAvailable() const {
    if (mHeadless || !mRoot) return false;
    Ogre::VulkanRenderSystem *rs = dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem());
    Ogre::VulkanDevice *dev = rs ? rs->getVulkanDevice() : nullptr;
    if (!dev || !dev->hasRayQuery() || !dev->mPhysicalDevice) return false;
    // Asked ONCE per physical device: the answer is a property of the device,
    // and this predicate is read every time a view describes its chain.
    static VkPhysicalDevice sAsked = VK_NULL_HANDLE;
    static bool sStores = false;
    if (sAsked != dev->mPhysicalDevice) {
        sAsked = dev->mPhysicalDevice;
        const std::string refused = rayStorageFormatRefused(dev->mPhysicalDevice);
        sStores = refused.empty();
        if (!sStores)
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: ray-query tier refused - the device cannot store to " + refused +
                " (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, optimal tiling), which the tier's "
                "history images need; this is a no-rays device");
    }
    return sStores;
}

void OgreEngine::updateRayQuery(const std::vector<OgreScene *> &drawn) {
    if (!mRayTracingWanted || mHeadless || !mRoot) return;
    if (!mRayTier) {
        if (!rayQueryAvailable()) return;             // the fallback picture, silently
        mRayTier = new detail::RayQueryTier();
        std::string err;
        if (!mRayTier->open(mRoot->getRenderSystem(), err)) {
            Ogre::LogManager::getSingleton().logMessage("Jahshaka: ray-query tier off — " + err);
            delete mRayTier;
            mRayTier = nullptr;
            return;
        }
    }
    // A scene whose project row says OFF must cost nothing, not just look the
    // same (world.rayTracing's contract): no gather, no BLAS/TLAS for it. The
    // structures it may already hold were released when the row flipped
    // (OgreScene::setRayTracing → forgetRayQuery).
    // GATHER-0: A BACKSTOP, and only that since the fix round (D2). The
    // registration is PASS-scoped — `ReflectPassListener::passPosExecute`
    // takes it away as its own pass ends — so by the time a frame starts the
    // map is already empty. This line stays because the map is process-wide
    // state and a frame that begins with it non-empty is a bug this costs
    // nothing to be immune to. NOTE it is NOT the teardown path: the early
    // return above (the no-rays switch) skips it, which is why
    // `RayQueryTier::close()` clears it too (D1).
    FogHlmsListener::clearProbeGather();
    // ...and the sun contact's registration, the same backstop (PHOTON-RAYS-1).
    FogHlmsListener::clearSunContact();
    for (OgreScene *s : drawn) {
        if (!s->rayTracingResolved()) continue;
        mRayTier->updateScene(s);
    }
}

// ---------------------------------------------------------------------------
/// IS THE SCREEN-PROBE GATHER ON FOR THIS SCENE? The project's row resolved
/// against the TIER TABLE and the machine (PHOTON-GATHER-1d, the rule T-A), the
/// same shape `rayReflectionsWanted` has: the row says what the scene asks for,
/// the table says what `Auto` means at this tier — on at High, Epic and Medium,
/// off at Low and in the VR column (`giQualityFacts(...).gather.on`) — and only
/// under a GI mode that is on (there is no diffuse GI to estimate otherwise);
/// the machine answers whether it can trace at all.
bool OgreScene::probeGatherWanted() const {
    switch (mGi.gather) {
    case GiToggle::Off: return false;
    case GiToggle::On:  break;
    default:
        if (mGi.mode == GiMode::Off) return false;
        if (!giQualityFacts(mGi.quality,
                            mGiDriverStereo ? GiViewProfile::Vr : GiViewProfile::Desktop,
                            mGi.epicTier)
                 .gather.on)
            return false;
        break;
    }
    return rayTracingResolved();
}

/// THE GATHER'S REST KEY (PHOTON-GATHER-1d): everything the gather's answer
/// depends on besides the camera — the lighting (giLightingSerial: a light
/// write, an injection landing), the geometry (the ray tier's own gate: the
/// movement epoch and the ray rule's refits) and the surface cache the hits read
/// first (its captures and relights). A frame whose key and camera equal the
/// last frame's is a REST frame (ScreenProbeGather: the rest mean, the hold).
unsigned long long OgreScene::gatherRestKey() const {
    unsigned long long key = 1469598103934665603ull;
    const auto fold = [&key](unsigned long long v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    fold(giLightingSerial());
    fold(shadowEpoch() + rayLevelRefits());
    if (mSurfaceCache) {
        CardCacheStatus cs;
        mSurfaceCache->fillStatus(cs);
        fold(cs.captures);
        fold(cs.relights);
        fold(cs.indirectRelights);
    }
    return key;
}

/// THE RESTART KEY (PHOTON-GATHER-1d fix round): the discontinuities the pixel
/// history cannot follow — the lighting (giLightingSerial: a light or sky write
/// through refreshGiLighting, an injection landing, a cascade rebuild), a
/// material write (the surface cache's precise door counts it; the voxels'
/// re-inject moves the lighting serial) and the cloud layer's GI change. NOT
/// the transform epoch: a mover's per-frame write is followed per pixel by the
/// reprojection, and a scene with one would otherwise never settle.
unsigned long long OgreScene::gatherRestartKey() const {
    unsigned long long key = 1469598103934665603ull;
    const auto fold = [&key](unsigned long long v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    fold(giLightingSerial());
    fold(mCloudGiSerial);
    if (mSurfaceCache) {
        CardCacheStatus cs;
        mSurfaceCache->fillStatus(cs);
        fold(cs.invalidMaterial);
        fold(cs.invalidLight);
    }
    return key;
}

/// The gather's numbers for `GiStatus`.
void OgreScene::gatherStatusInto(GatherStatus &out) const {
    out = GatherStatus();
    out.on = probeGatherWanted();
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->gatherStatsInto(this, out);
}

/// HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1): the row, held inside its band.
/// Storing it builds nothing; the view's chain re-checks its shape each frame.
void OgreScene::setSunContact(const SunContactDesc &d) {
    SunContactDesc c = d;
    if (!(c.range >= kSunContactMinRange)) c.range = kSunContactMinRange;   // NaN too
    if (c.range > kSunContactMaxRange) c.range = kSunContactMaxRange;
    mSunContact = c;
}

/// The row resolved against the machine — `probeGatherWanted`'s shape: the
/// row says what the scene asks for and the machine answers whether it can.
bool OgreScene::sunContactWanted() const {
    return mSunContact.enabled && rayTracingResolved();
}

SunContactStatus OgreScene::sunContactStatus() const {
    SunContactStatus st;
    st.on = sunContactWanted();
    if (st.on && mEngine && mEngine->mRayTier) mEngine->mRayTier->sunContactStatsInto(this, st);
    if (st.on && !st.running && st.reason.empty())
        st.reason = "no view of this scene has drawn with the row on yet";
    return st;
}

void OgreEngine::shutdownRayQuery() {
    if (!mRayTier) return;
    mRayTier->close();
    delete mRayTier;
    mRayTier = nullptr;
}

void OgreScene::forgetRayQuery() {
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->forgetScene(this);
}

bool OgreScene::rayReflectionsWanted() const {
    // The project's own World row (world.rayTracing: off / auto / on — owner,
    // ledger §425) resolved against the machine: OgreScene::rayTracingResolved
    // = the row is not Off × the diagnostic latch (--no-ray-query,
    // JAHSHAKA_NO_RAY_QUERY — the one thing that still lets every
    // ray-consuming suite run BOTH pictures on one GPU) × the device's answer.
    return rayTracingResolved();
}

RayQueryStatus OgreScene::rayQueryStatus() const {
    RayQueryStatus st;
    if (!mEngine) return st;
    st.available = mEngine->rayQueryAvailable();
    if (!mEngine->mRayTier) {
        // NO TIER YET — either nothing has drawn since it was switched on, or
        // it is off. `enabled` must say WHICH (round 2, finding 18): reporting
        // false for both made "switched on, first frame not drawn" look exactly
        // like "this machine has ray tracing switched off", and a caller
        // reading straight after the row flipped to "auto" would be told no.
        st.enabled = st.available && mEngine->rayTracing();
        return st;
    }
    st = mEngine->mRayTier->status(this);
    // R5's own readings ride the same struct: the tier keeps them per VIEW (a
    // trace is a screen-space pass) and this folds the views of THIS scene into
    // the one answer giStatus asks for.
    mEngine->mRayTier->reflectStatsInto(this, st);
    mEngine->mRayTier->hitStatsInto(this, st);
    return st;
}

/// THE BOUNDARY'S TRACE (tests and tools only — see traceBlocking).
bool OgreScene::traceRays(const std::vector<float> &rays, std::vector<float> &hits) {
    hits.clear();
    if (!mEngine || !mEngine->mRayTier) return false;
    std::string err;
    if (!mEngine->mRayTier->traceBlocking(this, rays, hits, err)) {
        Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
        return false;
    }
    return true;
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka

#else   // !JAH_RAY_QUERY

// NO VULKAN RENDER SYSTEM ON THIS PLATFORM (macOS, and any build where the
// plugin or the Vulkan headers are absent): the tier does not exist, and every
// consumer takes the no-rays path — which is a supported picture, not a
// degraded one. Audit C-6's defect was a CMake FATAL_ERROR here; there is none.
namespace jahshaka {
namespace engine {
namespace detail {

void OgreEngine::setRayTracing(bool on) { mRayTracingWanted = on; }
bool OgreEngine::rayQueryAvailable() const { return false; }
void OgreEngine::updateRayQuery(const std::vector<OgreScene *> &) {}
void OgreEngine::shutdownRayQuery() {}
RayQueryStatus OgreScene::rayQueryStatus() const { return RayQueryStatus(); }
bool OgreScene::traceRays(const std::vector<float> &, std::vector<float> &hits) {
    hits.clear();
    return false;
}
void OgreScene::forgetRayQuery() {}
bool OgreScene::probeGatherWanted() const { return false; }
bool OgreScene::traceCardMovers(const CardMoverTrace &) { return false; }
void OgreScene::timeCardRelight(bool) {}
void OgreScene::cardMoverTimes(float &, float &) {}
void OgreScene::gatherStatusInto(GatherStatus &out) const { out = GatherStatus(); }
void OgreScene::setSunContact(const SunContactDesc &d) { mSunContact = d; }
bool OgreScene::sunContactWanted() const { return false; }
SunContactStatus OgreScene::sunContactStatus() const { return SunContactStatus(); }
bool OgreScene::rayReflectionsWanted() const { return false; }
void OgreView::dropReflectState() {}
bool OgreEngine::cardReadParity(Scene *, const std::vector<CardReadQuery> &,
                                std::vector<CardReadPick> &out) {
    out.clear();
    mLastError = "cardReadParity: no ray-query tier on this platform";
    return false;
}

// R5 takes the same road: with no tier there is nothing to hook, so the
// listener is never created and `jahSsrReflection` holds what the screen-space
// resolve wrote — the fallback picture, which is the picture this renderer drew
// before ray tracing existed.
ReflectPassListener::~ReflectPassListener() {}
void ReflectPassListener::passPreExecute(Ogre::CompositorPass *) {}
void ReflectPassListener::passPosExecute(Ogre::CompositorPass *) {}
void OgreView::syncReflectListener() {
    // No tier on this platform: the chain is built with `rayReflect` false by
    // construction (the predicate above answers false), so there is nothing to
    // arm and nothing to rebuild.
    if (mReflectListener) {
        removeWorkspaceListener(mReflectListener.get());
        mReflectListener.reset();
    }
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka

#endif  // JAH_RAY_QUERY
