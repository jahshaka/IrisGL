// PHOTON R1 — THE HARDWARE RAY-QUERY TIER.
//
// WHAT THIS IS. A ray-traceable copy of the scene, kept current every frame:
// one bottom-level acceleration structure (BLAS) per unique mesh, built from
// Ogre's OWN vertex and index buffers, and one top-level structure (TLAS) over
// the instances the scene draws. Nothing consumes it yet — R2 (probe
// visibility), R3 (hard sun contact shadows) and R5 (ray-traced reflections)
// are the consumers, and every one of them reads this same structure. What R1
// ships is the structure, the switch that turns it off, and the proof that a
// ray hits where the mathematics says it hits.
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
//  * THE TRACED SET COMES FROM THE SCENE'S ITEM INDEX, never from
//    `SceneManager::getMovableObjectIterator` (audit C-4: the spike traced the
//    editor's gizmo arrows and light icons). See OgreScene::gatherRayInstances.
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
#include <Vao/OgreVertexArrayObject.h>
#include <Vao/OgreVertexBufferPacked.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreVaoManager.h>

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
/// Two timestamps per pass (BLAS batch, TLAS), per frame slot.
constexpr unsigned kQueriesPerFrame = 4u;
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

/// THE ROUGHNESS GATE. Above it a reflection is a wide lobe, the probe's own
/// prefiltered photograph is a good enough integral of it, and a ray per pixel
/// buys a blurrier answer for the same cost — so the budget stops here. It sits
/// just above `ssrRoughnessCutoff`'s 0.35 default on purpose: the band between
/// the two is where the screen-space march declines and only a ray can answer.
constexpr float kRayReflectRoughness = 0.4f;
/// HOW WIDE THE GATE'S FEATHER IS, either side of the cutoff (owner, ledger
/// §426). The ray's confidence runs from full at `cutoff - kRayReflectFeather`
/// to zero at `cutoff + kRayReflectFeather`, and the existing confidence
/// composite hands the remainder to the probe — so a surface whose roughness
/// varies across it has no seam in it. A CONSTANT and deliberately not a second
/// dial: the cutoff says WHERE the technique stops being worth it (content), the
/// feather only says that it stops smoothly (renderer). 0.1 is about two and a
/// half times the ±0.04 that a roughness map's 8-bit quantisation can move a
/// neighbouring pixel by, so the ramp is always wider than the noise it hides.
constexpr float kRayReflectFeather = 0.1f;
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
/// Bindings in rq_reflect.comp's set 0.
constexpr unsigned kReflectBindings = 15u;

/// A storage image this file owns outright — the temporal mean and the distance
/// beside it. Not an Ogre texture: nothing but this compute pass ever reads or
/// writes one, it is never a render target, and giving it to the compositor
/// would put a per-frame race (the ping-pong) inside a graph that rebuilds.
struct ReflectImage {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
};

}   // namespace

// ---------------------------------------------------------------------------
/// THE TIER. One per engine: the device plumbing and the compute pipeline are
/// process-wide, the acceleration structures are per SCENE (each scene draws
/// its own items, and a probe preview or a thumbnail scene must never appear in
/// the editor's structure).
class RayQueryTier {
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
        std::unordered_map<const Ogre::Mesh *, size_t> blasOfMesh;
        std::vector<Blas> blas;

        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        RawBuffer tlasStorage, tlasScratch;
        /// Persistently mapped, kFramesInFlight slots: the gather writes
        /// straight into the slot this frame will build from, so a transform
        /// never passes through an intermediate vector.
        RawBuffer instances;
        unsigned  instanceCapacity = 0;
        unsigned  instanceCount = 0;
        unsigned  slot = 0;

        /// The gate: nothing moved and no item changed since the last update,
        /// so there is nothing to record. Same epoch the caster walk uses.
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

    bool ensureBlas(SceneAs &sa, const std::vector<Ogre::MeshPtr> &meshes, VkCommandBuffer cmd,
                    unsigned &built, std::string &err);
    /// True when it actually recorded a compaction copy this frame.
    bool runCompaction(SceneAs &sa, VkCommandBuffer cmd);
    bool buildTlas(SceneAs &sa, VkCommandBuffer cmd, bool refit, std::string &err);
    void readTimestamps(SceneAs &sa);

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
        uint32_t frame = 0;
    };
    std::vector<Retired> mRetireBin;
    void retire(RawBuffer &b);
    void retire(VkAccelerationStructureKHR &as, RawBuffer &storage);
    void retireImage(ReflectImage &img);
    void retireView(VkImageView v);
    void retireSet(VkDescriptorSet set);
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
    ReflectImage mDummyCube, mDummyVolume;
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
                       Ogre::CompositorPass *pass);
    /// Frees a view's reflection resources. Called from ~ReflectPassListener.
    void forgetReflect(const ReflectPassListener *key);
    /// How many rays the last recorded trace dispatched, and the GPU
    /// milliseconds it cost (read back several frames later, -1 until measured)
    /// — for GiStatus::rayQuery. Per SCENE, because that is what giStatus asks.
    void reflectStatsInto(const OgreScene *scene, RayQueryStatus &st) const;

private:
    struct ReflectView {
        /// The descriptor ring. A set that is bound by a command buffer still in
        /// flight may not be rewritten, and every input of this pass can be
        /// recreated behind our back (a workspace rebuild replaces every texture
        /// in the chain; a GI re-solve replaces every voxel texture), so the set
        /// is rewritten EVERY frame and there is one per frame in flight.
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
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
        /// reconstructs with. `havePrev` false means "no history is valid", which
        /// is what a first frame, a resize and a scene change all are.
        float prevCamPos[4] = { 0, 0, 0, 1 };
        float prevRayTL[4] = { 0, 0, 0, 0 };
        float prevRayRight[4] = { 0, 0, 0, 0 };
        float prevRayDown[4] = { 0, 0, 0, 0 };
        float prevFwd[4] = { 0, 0, 0, 0 };
        bool  havePrev = false;
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
    };

    bool makeReflectPipeline(std::string &err);
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
    VkSampler             mPointSampler = VK_NULL_HANDLE;
    VkSampler             mLinearSampler = VK_NULL_HANDLE;
    /// The pipeline could not be made on this device; say so ONCE and take the
    /// fallback picture for the rest of the process.
    bool                  mReflectFailed = false;
    /// Timestamp slots for reflect passes, the same bitmask discipline as
    /// mTimedSceneSlots (and for the same round-3 reason).
    uint32_t              mReflectQuerySlots = 0;
    VkQueryPool           mReflectTimestamps = VK_NULL_HANDLE;
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
        // The slot stays (indices are referenced by blasOfMesh and by the
        // instance patches); only its contents go, and the map entry with them,
        // so the next sighting rebuilds into a fresh slot.
        for (auto it = sa.blasOfMesh.begin(); it != sa.blasOfMesh.end();) {
            if (it->second == i) it = sa.blasOfMesh.erase(it); else ++it;
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

void RayQueryTier::retireSet(VkDescriptorSet set) {
    if (!set) return;
    Retired r;
    r.set = set;
    r.frame = frameNow();
    mRetireBin.push_back(r);
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
    struct Spec { ReflectImage *img; VkImageType type; VkImageViewType viewType; uint32_t layers;
                  VkImageCreateFlags flags; };
    const Spec specs[2] = {
        { &mDummyCube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6u,
          VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT },
        { &mDummyVolume, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1u, 0u },
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
    mDummiesReady = true;
    mDummiesNeedClear = true;
    return true;
}

void RayQueryTier::clearDummyImages(VkCommandBuffer cmd) {
    if (!mDummiesNeedClear) return;
    mDummiesNeedClear = false;
    ReflectImage *imgs[2] = { &mDummyCube, &mDummyVolume };
    const uint32_t layers[2] = { 6u, 1u };
    for (int i = 0; i < 2; ++i) {
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
            if (mRetireBin[i].set) vkFreeDescriptorSets(mVk, mReflectPool, 1, &mRetireBin[i].set);
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
    }
    mScenes.clear();
    for (auto &kv : mReflects) dropReflect(kv.second);
    mReflects.clear();
    for (ReflectImage *d : { &mDummyCube, &mDummyVolume }) {
        if (d->view) vkDestroyImageView(mVk, d->view, nullptr);
        if (d->image) vkDestroyImage(mVk, d->image, nullptr);
        if (d->memory) vkFreeMemory(mVk, d->memory, nullptr);
        *d = ReflectImage();
    }
    mDummiesReady = false;
    mDummiesNeedClear = false;
    for (Retired &r : mRetireBin) {
        if (r.as) mFn.destroyAccelerationStructure(mVk, r.as, nullptr);
        dropBuffer(r.buf);
        if (r.set) vkFreeDescriptorSets(mVk, mReflectPool, 1, &r.set);
        if (r.view) vkDestroyImageView(mVk, r.view, nullptr);
        if (r.img.view) vkDestroyImageView(mVk, r.img.view, nullptr);
        if (r.img.image) vkDestroyImage(mVk, r.img.image, nullptr);
        if (r.img.memory) vkFreeMemory(mVk, r.img.memory, nullptr);
    }
    mRetireBin.clear();
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
/// scene's structures: a TLAS built for someone else's items, a blasOfMesh
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
    // The compaction slots this scene still owed a read are never going to be
    // read; hand them back or the ring leaks capacity until compaction stops
    // for EVERY scene (round 3, finding 1: a parked preview did exactly that).
    for (const Blas &bl : sa.blas)
        if (bl.compactState == 1u) mCompactBusy &= ~(uint64_t(1) << bl.compactSlot);
    if (sa.hasQueryBase) mTimedSceneSlots &= ~(uint32_t(1) << sa.querySlot);
    mScenes.erase(it);
}

// ---------------------------------------------------------------------------
// THE INSTANCE SINK. The gather writes each instance descriptor straight into
// the mapped slot this frame will build from — no intermediate vector, which is
// the cost audit C's P3 item names (4-6 ms of CPU for 8,026 instances in the
// spike's naive form).
namespace {
struct InstanceWriter final : public RayInstanceSink {
    VkAccelerationStructureInstanceKHR *dst = nullptr;
    unsigned capacity = 0;
    unsigned count = 0;
    unsigned overflow = 0;
    unsigned long long signature = 1469598103934665603ull;   // FNV-1a offset basis
    /// THE SIGNATURE IS ONLY EVER READ TO DECIDE REFIT-vs-REBUILD. A rebuild is
    /// the default (NVIDIA's own guidance for a TLAS, and 0.2-0.35 ms even at
    /// 8,000 instances), so with the refit off there is nothing to compare and
    /// three FNV rounds per instance are pure cost.
    bool wantSignature = false;
    std::unordered_map<const Ogre::Mesh *, size_t> *blasOfMesh = nullptr;
    const std::vector<VkDeviceAddress> *blasAddress = nullptr;
    /// Slots referenced by this gather, so a BLAS nothing points at any more can
    /// be evicted (finding 5).
    std::vector<unsigned char> *seen = nullptr;
    /// Meshes seen this gather that have no BLAS yet, in first-seen order, and
    /// the instances that must have their reference patched once they do.
    std::vector<Ogre::MeshPtr> newMeshes;
    std::unordered_map<const Ogre::Mesh *, unsigned> newIndexOf;
    struct Patch { unsigned instance; unsigned newMesh; };
    std::vector<Patch> patches;

    void hash(unsigned long long v) {
        signature ^= v;
        signature *= 1099511628211ull;
    }

    /// ONE-ENTRY MESH MEMO. The map lookup below was 8,001 hash lookups per
    /// gather on the lattice; consecutive instances almost always share a mesh
    /// (that scene is 8,001 instances of 22 meshes), so remembering the last
    /// one answers nearly all of them with a pointer compare. It is a pure
    /// cache — a miss falls through to the map and gives the same answer.
    const Ogre::Mesh *lastMesh = nullptr;
    size_t lastSlot = 0;
    bool lastFound = false;

    void add(const Ogre::MeshPtr &meshPtr, const Ogre::Matrix4 &xform, unsigned mask,
             unsigned customIndex) override {
        const Ogre::Mesh *mesh = meshPtr.get();
        const unsigned idx = count++;
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
        // ROW-MAJOR 3x4, which is exactly Ogre's own layout for the top three
        // rows of a Matrix4 — no transpose, no conversion. operator[] hands
        // back the ROW pointer, so this is three calls and twelve plain loads.
        for (int r = 0; r < 3; ++r) {
            const Ogre::Real *row = xform[r];
            for (int c = 0; c < 4; ++c) inst.transform.matrix[r][c] = float(row[c]);
        }
        inst.instanceCustomIndex = customIndex & 0xFFFFFFu;
        inst.mask = mask & 0xFFu;
        inst.instanceShaderBindingTableRecordOffset = 0;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = 0;

        if (mesh == lastMesh && lastFound) {
            inst.accelerationStructureReference = (*blasAddress)[lastSlot];
            if (seen && lastSlot < seen->size()) (*seen)[lastSlot] = 1u;
            if (wantSignature) { hash(1ull + lastSlot); hash(customIndex); hash(mask); }
            std::memcpy(&dst[idx], &inst, sizeof(inst));
            return;
        }
        auto it = blasOfMesh->find(mesh);
        if (it != blasOfMesh->end()) {
            lastMesh = mesh; lastSlot = it->second; lastFound = true;
            if (seen && it->second < seen->size()) (*seen)[it->second] = 1u;
            inst.accelerationStructureReference = (*blasAddress)[it->second];
            if (wantSignature) hash(1ull + it->second);
        } else {
            lastMesh = nullptr; lastFound = false;
            auto nit = newIndexOf.find(mesh);
            unsigned ni;
            if (nit == newIndexOf.end()) {
                ni = unsigned(newMeshes.size());
                newMeshes.push_back(meshPtr);
                newIndexOf.emplace(mesh, ni);
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

/// THE TRACED SET. Walked out of the scene's own item index once per update.
///
/// WHAT IS IN: every Item that draws real world geometry — it carries
/// kVisibleBit or kMovableBit, it is shown, it is below the overlay queues, it
/// has a mesh with triangles.
///
/// WHAT IS OUT, and why each exclusion is load-bearing:
///   * EDITOR FURNITURE (kHelperBit), the BACKDROP (kBackdropBit — the 2 km
///     horizon plane), the SUN DISC (kSunDiscBit) and DISTORTION objects
///     (kDistortionBit). Each of these carries its own channel INSTEAD OF
///     kVisibleBit precisely so that captures can exclude it; a gizmo arrow in
///     the acceleration structure is what the S3 spike shipped and audit C-4
///     caught.
///   * THE OVERLAY QUEUES (rq >= kOverlayRenderQueue): unlit, depth-test off,
///     never part of the world.
///   * SKINNED ITEMS (audit C-5). A BLAS reads the mesh's own vertex buffer,
///     which holds the BIND POSE — a walking character would cast a T-pose
///     shadow and reflect as a T-pose. They stay on the shadow atlas (D3's
///     complement makes that free) until R4 builds a GPU skin cache.
///   * ALPHA-TESTED DATABLOCKS (audit C-16). Every BLAS here is
///     VK_GEOMETRY_OPAQUE_BIT_KHR and the rays use gl_RayFlagsOpaqueEXT,
///     because without ray-tracing PIPELINES there is no any-hit shader; a
///     cut-out leaf would intersect as a solid quad. The follow-up that would
///     retire this is VK_EXT_opacity_micromap (native on Ada) — recorded in
///     SPECS/LATER_OPTIMISATIONS.md.
void OgreScene::gatherRayInstances(RayInstanceSink &sink) const {
    const Ogre::uint32 casterChannels = allShadowCasterChannels();
    for (const Node *np : mItemNodes) {
        const Node &n = *np;
        Ogre::Item *item = n.item;
        if (!item || !n.shown) continue;
        const Ogre::uint32 flags = item->getVisibilityFlags();
        if (!(flags & (kVisibleBit | kMovableBit))) continue;
        if (item->getRenderQueueGroup() >= kOverlayRenderQueue) continue;
        if (item->getSkeletonInstance()) continue;              // C-5
        const Ogre::MeshPtr &mesh = item->getMesh();
        if (!mesh) continue;
        bool alphaTested = false;
        for (size_t i = 0, e = item->getNumSubItems(); i < e && !alphaTested; ++i) {
            const Ogre::HlmsDatablock *db = item->getSubItem(i)->getDatablock();
            if (db && db->getAlphaTest() != Ogre::CMPF_ALWAYS_PASS) alphaTested = true;
        }
        if (alphaTested) continue;                              // C-16
        Ogre::Node *node = item->getParentNode();
        if (!node) continue;
        // PER-CONSUMER MASKS (audit C-15): bit 0 a shadow caster, bit 1 a
        // mover, bit 2 still world. A sun-contact ray (R3) tests bit 0; a
        // "what does this probe see of the room that stands still" ray (R2)
        // tests bit 2; a reflection (R5) tests everything.
        unsigned mask = 0u;
        if (item->getCastShadows() && (flags & casterChannels)) mask |= 0x01u;
        mask |= (flags & kMovableBit) ? 0x02u : 0x04u;
        sink.add(mesh, node->_getFullTransform(), mask,
                 n.itemSlot == size_t(-1) ? 0u : unsigned(n.itemSlot));
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

bool describeMesh(const Ogre::Mesh *mesh, RayQueryTier *, MeshGeometry &out,
                  VkDeviceAddress (*addressOf)(void *, VkBuffer), void *self) {
    for (unsigned s = 0, e = unsigned(mesh->getNumSubMeshes()); s < e; ++s) {
        const Ogre::SubMesh *sub = mesh->getSubMesh(s);
        if (!sub || sub->mVao[Ogre::VpNormal].empty()) continue;
        Ogre::VertexArrayObject *vao = sub->mVao[Ogre::VpNormal][0];
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

bool RayQueryTier::ensureBlas(SceneAs &sa, const std::vector<Ogre::MeshPtr> &meshes,
                              VkCommandBuffer cmd, unsigned &built, std::string &err) {
    built = 0;
    struct Job {
        Ogre::MeshPtr mesh;
        MeshGeometry geo;
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        size_t slot = 0;
    };
    std::vector<Job> jobs;
    jobs.reserve(meshes.size());
    auto addrThunk = [](void *self, VkBuffer b) -> VkDeviceAddress {
        return static_cast<RayQueryTier *>(self)->addressOf(b);
    };
    for (const Ogre::MeshPtr &mesh : meshes) {
        Job j;
        j.mesh = mesh;
        if (!describeMesh(mesh.get(), this, j.geo, addrThunk, this)) continue;
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
        sa.blasOfMesh[j.mesh.get()] = j.slot;
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
    const unsigned long long epoch = scene->shadowEpoch();
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

    std::string err;
    for (int attempt = 0; attempt < 2; ++attempt) {
        InstanceWriter w;
        w.blasOfMesh = &sa.blasOfMesh;
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
        scene->gatherRayInstances(w);
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

        const bool timed = mTimestamps && sa.hasQueryBase;
        const unsigned ring = frame % kFramesInFlight;
        const unsigned qBase = sa.queryBase + ring * kQueriesPerFrame;
        if (timed) vkCmdResetQueryPool(cmd, mTimestamps, qBase, kQueriesPerFrame);
        SceneAs::PendingTimes &pend = sa.pending[ring];
        pend = SceneAs::PendingTimes();
        pend.frame = frame;
        pend.live = timed;

        unsigned built = 0;
        if (!w.newMeshes.empty()) {
            if (timed)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qBase + 0);
            monitor::CacheScope scope(CacheKind::Gi, WorkReason::Added, 0, "rq.blas", mRs);
            if (!ensureBlas(sa, w.newMeshes, cmd, built, err)) {
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
                auto it = sa.blasOfMesh.find(w.newMeshes[p.newMesh].get());
                if (it == sa.blasOfMesh.end()) continue;
                w.dst[p.instance].accelerationStructureReference = sa.blas[it->second].address;
            }
        }
        sa.instanceCount = w.count;
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
            scope.setUnits(sa.instanceCount);
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
    sa.st.gatherMs = float(gatherMs);
    sa.st.blasCount = int(sa.blas.size());
    sa.st.instances = int(sa.instanceCount);
    sa.st.triangles = 0;
    sa.st.blasBytes = 0;
    for (const Blas &bl : sa.blas) {
        sa.st.triangles += int(bl.triangles);
        sa.st.blasBytes += bl.storage.size;
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
    struct Params { uint32_t counts[4]; } params{};
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
    float camPos[4];
    float rayTL[4];
    float rayRight[4];
    float rayDown[4];
    float fwd[4];
    float projParams[4];
    float resolution[4];
    float knobs[4];
    float knobs2[4];
    float skyColour[4];
    float viewAxisX[4];
    float viewAxisY[4];
    float viewAxisZ[4];
    float voxelOrigin[kMaxReflectCascades][4];
    float voxelInvSize[kMaxReflectCascades][4];
    float prevCamPos[4];
    float prevRayTL[4];
    float prevRayRight[4];
    float prevRayDown[4];
    float prevFwd[4];
};

void put3(float *dst, const Ogre::Vector3 &v, float w) {
    dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = w;
}

}   // namespace

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
    };
    for (unsigned i = 0; i < kReflectBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = (i >= 10u && i <= 13u) ? kMaxReflectCascades : 1u;
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
    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = sets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = sets;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[2].descriptorCount = sets * 5u;
    sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[3].descriptorCount = sets * (3u + 4u * kMaxReflectCascades + 1u);
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 4;
    dpi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(mVk, &dpi, nullptr, &mReflectPool) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateDescriptorPool failed";
        return false;
    }
    // TWO SAMPLERS, AND WHICH IS WHICH MATTERS. The G-buffer and the depth are
    // read at exactly one texel — a linear tap across a depth discontinuity
    // reconstructs a position on neither surface — so they are POINT. The voxel
    // volumes and the sky cube are continuous fields and are LINEAR, with a
    // clamped address mode so a sample at a volume's face does not wrap to the
    // other side of the world.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(mVk, &si, nullptr, &mPointSampler) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateSampler (point) failed";
        return false;
    }
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (vkCreateSampler(mVk, &si, nullptr, &mLinearSampler) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateSampler (linear) failed";
        return false;
    }
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
    rv.w = w; rv.h = h;
    const VkFormat formats[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32_SFLOAT };
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

void RayQueryTier::recordReflect(const ReflectPassListener *key, OgreView *view,
                                 Ogre::CompositorPass *pass) {
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
    const unsigned traceW = ssrRow >= 2 ? fullW : std::max(1u, fullW / 2u);
    const unsigned traceH = ssrRow >= 2 ? fullH : std::max(1u, fullH / 2u);

    // ---- THE VOXEL CACHE THE HITS ARE SHADED FROM (route A) -----------------
    // Under a Photon cascade chain each cascade is its OWN VctLighting (they are
    // chained with addCascade, OgreGi.cpp), innermost first — which is exactly
    // the order the shader wants: it takes the first volume that contains the
    // hit, so the finest one that can answer does. In the single-volume arm
    // there is one. R5 works in both shapes and the cascade flag gates nothing
    // here.
    Ogre::TextureGpu *vox[kMaxReflectCascades][4] = {};
    Ogre::Vector3 voxOrigin[kMaxReflectCascades], voxSize[kMaxReflectCascades],
                  voxCell[kMaxReflectCascades];
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
        for (int i = 0; i < 4; ++i) vox[voxCount][i] = tex[i] ? tex[i] : tex[0];
        voxOrigin[voxCount] = voxelizer->getVoxelOrigin();
        voxSize[voxCount]   = voxelizer->getVoxelSize();
        voxCell[voxCount]   = voxelizer->getVoxelCellSize();
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
    Ogre::TextureGpu *skyTex = scene->mReflectionTex;

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
    put3(pp.camPos, camPos, ortho ? 0.0f : 1.0f);
    put3(pp.rayTL, right * fl + up * ft + (ortho ? Ogre::Vector3::ZERO : fwd), 0.0f);
    put3(pp.rayRight, right * (fr - fl), 0.0f);
    put3(pp.rayDown, up * (fb - ft), 0.0f);
    put3(pp.fwd, fwd, 0.0f);
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
    // THE CUTOFF IS THE PROJECT'S (PostFxDesc::rayReflectRoughness); the engine
    // only clamps it into the range a reflection means anything in.
    pp.knobs[0] = std::min(std::max(view->chainDesc().rayReflectRoughness, 0.0f), 1.0f);
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
    // The sky's own average, for a scene with no captured cube at all. Band 0 of
    // the sky SH is the mean radiance over the sphere (Y00 = 0.282095), which is
    // the honest constant answer when there is no direction-dependent one.
    for (int i = 0; i < 3; ++i) pp.skyColour[i] = std::max(0.0f, scene->mSkySh[i] * 0.282095f);
    for (unsigned c = 0; c < kMaxReflectCascades; ++c) {
        const unsigned src = c < voxCount ? c : (voxCount ? voxCount - 1u : 0u);
        const Ogre::Vector3 sz = voxCount ? voxSize[src] : Ogre::Vector3(1.0f);
        const Ogre::Vector3 og = voxCount ? voxOrigin[src] : Ogre::Vector3::ZERO;
        const Ogre::Vector3 cl = voxCount ? voxCell[src] : Ogre::Vector3(1.0f);
        pp.voxelOrigin[c][0] = og.x; pp.voxelOrigin[c][1] = og.y; pp.voxelOrigin[c][2] = og.z;
        pp.voxelOrigin[c][3] = std::max(std::max(sz.x, sz.y), sz.z);
        pp.voxelInvSize[c][0] = sz.x > 0.0f ? 1.0f / sz.x : 0.0f;
        pp.voxelInvSize[c][1] = sz.y > 0.0f ? 1.0f / sz.y : 0.0f;
        pp.voxelInvSize[c][2] = sz.z > 0.0f ? 1.0f / sz.z : 0.0f;
        pp.voxelInvSize[c][3] = std::max(std::max(cl.x, cl.y), cl.z);
    }
    if (rv.havePrev) {
        memcpy(pp.prevCamPos, rv.prevCamPos, sizeof(pp.prevCamPos));
        memcpy(pp.prevRayTL, rv.prevRayTL, sizeof(pp.prevRayTL));
        memcpy(pp.prevRayRight, rv.prevRayRight, sizeof(pp.prevRayRight));
        memcpy(pp.prevRayDown, rv.prevRayDown, sizeof(pp.prevRayDown));
        memcpy(pp.prevFwd, rv.prevFwd, sizeof(pp.prevFwd));
    } else {
        // NO PREVIOUS FRAME. A zero forward makes every reprojection's `z` zero,
        // which the shader rejects — so the first frame after a resize, a scene
        // bind or a workspace rebuild starts its mean from scratch instead of
        // reading a buffer that means something else.
        memset(pp.prevFwd, 0, sizeof(pp.prevFwd));
    }
    memcpy(rv.params[ring].mapped, &pp, sizeof(pp));
    memcpy(rv.prevCamPos, pp.camPos, sizeof(pp.camPos));
    memcpy(rv.prevRayTL, pp.rayTL, sizeof(pp.rayTL));
    memcpy(rv.prevRayRight, pp.rayRight, sizeof(pp.rayRight));
    memcpy(rv.prevRayDown, pp.rayDown, sizeof(pp.rayDown));
    memcpy(rv.prevFwd, pp.fwd, sizeof(pp.fwd));
    rv.havePrev = true;

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
                          volumes[4][kMaxReflectCascades] = {}, sky{};
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
    for (int axis = 0; axis < 4; ++axis)
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
    for (int axis = 0; axis < 4; ++axis)
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
            for (int axis = 0; axis < 4; ++axis)
                if (vox[c][axis])
                    solver.resolveTransition(trans, vox[c][axis], Ogre::ResourceLayout::Texture,
                                             Ogre::ResourceAccess::Read, computeStage);
        if (skyTex)
            solver.resolveTransition(trans, skyTex, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
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
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeline);
    vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, 1u);
    if (mReflectTimestamps && rv.hasQueryBase) {
        const uint32_t base = rv.queryBase + (rv.frame % kFramesInFlight) * 2u;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mReflectTimestamps,
                            base + 1u);
        ReflectView::Pending &pd = rv.pending[rv.frame % kFramesInFlight];
        pd.frame = frameNow();
        pd.live = true;
    }
    rv.rays = traceW * traceH;
    ++rv.frame;
}

// ---------------------------------------------------------------------------
ReflectPassListener::~ReflectPassListener() {
    if (!mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    // Same rule as dropReflectState: nothing may be freed while a command buffer
    // that has it bound is still recording.
    if (mRoot && mRoot->getRenderSystem()) mRoot->getRenderSystem()->flushCommands();
    mView->mEngine->mRayTier->forgetReflect(this);
}

void ReflectPassListener::passPreExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    // THE PASS, BY WHAT IT DOES AND NOT BY ITS NAME. `PrePassUse` is Ogre's own
    // word for "this pass shades with a prepass' G-buffers and the ssr texture"
    // — it is the pass the trace must run before, and the one the compositor
    // would break if anyone renamed a profiling string.
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def || def->mPrePassMode != Ogre::PrePassUse) return;
    mView->mEngine->mRayTier->recordReflect(this, mView, pass);
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
}

void OgreView::syncReflectListener() {
    // THE CHAIN'S SHAPE IS RE-CHECKED HERE, once a frame, and that is not
    // belt-and-braces: `ChainDesc::rayReflect` depends on the SCENE (the
    // project's ray row, lane RAYROW-1) and a view has no scene when its chain
    // is first built, nor does `setScene` rebuild one. Without this a view
    // would render its whole life with the shape it was constructed with.
    // Cheap: a comparison against what the current definition was built with.
    if (mChainRayReflect != chainDesc().rayReflect) rebuildWorkspaceDef();
    // The same arming rule as the planar and globals listeners, and the same
    // reason it is re-evaluated every frame: the shape above can change, and a
    // view can gain or lose its scene.
    const bool wanted = mEnabled && mScene && mCamera && mEngine && mEngine->mRayTier != nullptr &&
                        chainDesc().rayReflect;
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

bool OgreEngine::rayQueryAvailable() const {
    if (mHeadless || !mRoot) return false;
    Ogre::VulkanRenderSystem *rs = dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem());
    Ogre::VulkanDevice *dev = rs ? rs->getVulkanDevice() : nullptr;
    return dev && dev->hasRayQuery();
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
    for (OgreScene *s : drawn) {
        if (!s->rayTracingResolved()) continue;
        mRayTier->updateScene(s);
    }
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
void OgreScene::gatherRayInstances(RayInstanceSink &) const {}
void OgreScene::forgetRayQuery() {}
bool OgreScene::rayReflectionsWanted() const { return false; }
void OgreView::dropReflectState() {}

// R5 takes the same road: with no tier there is nothing to hook, so the
// listener is never created and `jahSsrReflection` holds what the screen-space
// resolve wrote — the fallback picture, which is the picture this renderer drew
// before ray tracing existed.
ReflectPassListener::~ReflectPassListener() {}
void ReflectPassListener::passPreExecute(Ogre::CompositorPass *) {}
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
