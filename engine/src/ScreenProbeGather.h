// THE SCREEN-PROBE GATHER — a Component of ours, beside VctLighting and
// IrradianceField (GATHER-1a, 2026-09-21; SPECS/SCREEN_PROBE_GATHER_SPEC.md
// section 9, phase 1).
//
// WHAT IT IS, in one sentence: one probe per N x N pixels of a view traces a
// hemisphere of hardware rays into the scene's acceleration structure and the
// pixels of its cell read the integral back at the diffuse-GI slot of the PBS
// shader — so the integral is estimated once per cell with RAYS instead of once
// per pixel with six voxel cones.
//
// WHY IT IS A COMPONENT AND NOT MORE CODE IN THE RAY TIER (the owner's rule,
// 2026-09-21: full use of Ogre-Next's own extension shape, never a band-aid
// around it). The tier owns the DEVICE and the structures: the TLAS, the
// allocator with the retire window patch 0067 taught it, the frame's command
// buffer, the black stand-ins every empty descriptor takes. The gather owns an
// ALGORITHM: where probes go, what they trace, what an atlas holds, what a
// pixel reads. Those are two lifetimes and two rates of change — phases 2 and 3
// added a filter, an SH record and a pixel history, none of which the tier has
// an opinion about — so they are two objects, and what
// passes between them is this file: a small service interface (`Host`) and a
// per-frame input record the tier fills from the scene it is friends with.
//
// WHAT IT DOES SINCE PHASE 2 (PHOTON-GATHER-1b): the trace's map is FILTERED
// in probe space (the 3x3 neighbourhood, weighted by plane and hit-distance
// agreement), projected onto SH9 per probe, and every pixel reads its four grid
// probes and its cell's adaptive twin weighted by the plane test, each
// evaluated at the pixel's own normal. SINCE PHASE 3 (PHOTON-GATHER-1c): a
// full-resolution pixel HISTORY behind the integrate (reprojected through the
// previous camera, validated on distance and normal, the count-in-history
// running mean). The trace stays the stratified sampler: reprojected importance
// sampling was measured and refused (spikes/photon-gather-1c). SINCE
// PHOTON-GATHER-1d: a hit reads its surface CARD first (the reflection's own
// `jahHitRadiance`, GA-1e), and the row is ON BY TIER — `GiToggle::Auto`
// resolves through the tier table's gather row (Types.h `GiGatherFacts`: High
// and Epic on, Medium at 36 rays, Low off). WHAT IS STILL ABSENT: stereo (the
// VR column keeps the gather off — GA-VR).
#pragma once

#include "jahshaka/engine/Types.h"

#include <string>
#include <unordered_map>
#include <vector>

#ifdef JAH_RAY_QUERY
#    include <vulkan/vulkan.h>
#endif

namespace Ogre {
class CompositorPass;
class RenderSystem;
class SceneManager;
class TextureGpu;
class UavBufferPacked;
}   // namespace Ogre

namespace jahshaka {
namespace engine {

namespace detail {
class OgreScene;
}

#ifdef JAH_RAY_QUERY

/// HOW MANY CASCADES THE HIT SHADING READS — the shaders' `kMaxCascades`, and
/// the two must agree.
constexpr unsigned kGatherMaxCascades = 4u;
/// The octahedral probe map is at most 8x8, because one ray is one thread of
/// the trace's 8x8 workgroup (the shaders' `kOctResMax`).
constexpr unsigned kGatherOctResMax = 8u;

/// THE VULKAN SERVICES THE GATHER BORROWS from the ray tier. Every one of them
/// is a rule the tier already owns and the gather must not re-decide: which
/// memory type a buffer takes, WHEN a freed object may actually be freed (the
/// 0067 retire window — a block handed back one frame early is the Xid 109
/// class), which command buffer this frame is recording into, and what a
/// descriptor slot with nothing to bind gets instead.
class GatherHost {
public:
    virtual ~GatherHost() = default;

    virtual VkDevice gatherDevice() const = 0;
    virtual Ogre::RenderSystem *gatherRenderSystem() const = 0;
    /// Ogre's own frame counter and in-flight depth — never a counter of ours
    /// (the tier's finding 3: a per-call counter runs at N times the frame rate
    /// with N drawn scenes, and every "wait for the frames in flight" guard
    /// then waits a fraction of what it promised).
    virtual uint32_t gatherFrameNow() const = 0;
    virtual uint32_t gatherFramesInFlight() const = 0;
    /// Outside every encoder, ready for compute and barriers.
    virtual VkCommandBuffer gatherFrameCmd() = 0;

    virtual bool gatherMakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                                  VkBuffer &buffer, VkDeviceMemory &memory, void **mapped,
                                  std::string &err) = 0;
    virtual bool gatherMakeImage(unsigned w, unsigned h, VkFormat fmt, VkImage &image,
                                 VkDeviceMemory &memory, VkImageView &view, std::string &err) = 0;

    /// ...and the retire window on every one of them.
    virtual void gatherRetireBuffer(VkBuffer buffer, VkDeviceMemory memory) = 0;
    virtual void gatherRetireImage(VkImage image, VkDeviceMemory memory, VkImageView view) = 0;
    virtual void gatherRetireView(VkImageView view) = 0;
    virtual void gatherRetireSet(VkDescriptorSet set, VkDescriptorPool pool) = 0;
    virtual void gatherRetireTexture(Ogre::TextureGpu *texture) = 0;

    /// The black stand-ins for the voxel volumes and the sky cube a scene may
    /// legitimately not have, the 2D one the card layers take in a scene with no
    /// surface cache, and the zeroed storage buffer its two tables and the
    /// geometry rows take there (GA-1e). Recorded (cleared once) by the tier.
    virtual bool gatherDummies(VkImageView &cube, VkImageView &volume, VkImageView &flat,
                               VkBuffer &storage, std::string &err) = 0;
    /// ...and the transition that takes them out of UNDEFINED, which only the
    /// FIRST pass to bind them can order (see the tier's note).
    virtual void gatherClearDummies(VkCommandBuffer cmd) = 0;
    virtual VkSampler gatherPointSampler() const = 0;
    virtual VkSampler gatherLinearSampler() const = 0;
};

/// EVERYTHING ONE FRAME OF ONE VIEW NEEDS, gathered by the tier from the scene
/// and the compositor pass. The gather takes what it needs as ARGUMENTS and is
/// friends with nothing — the debt `SurfaceCardSpike.h` names for a spike, paid
/// here because this one ships.
struct GatherInputs {
    const detail::OgreScene *scene = nullptr;
    Ogre::SceneManager *sceneMgr = nullptr;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;

    /// The prepass' G-buffers, by the names the chain declares them under.
    Ogre::TextureGpu *normals = nullptr;
    Ogre::TextureGpu *depth = nullptr;
    /// The scene's captured environment cube, or null.
    Ogre::TextureGpu *sky = nullptr;
    float skyColour[3] = { 0.0f, 0.0f, 0.0f };

    /// The cascade chain's light volumes: [cascade][slot], slot 0 isotropic and
    /// 1..3 the anisotropic chains.
    Ogre::TextureGpu *voxel[kGatherMaxCascades][4] = {};
    float voxelOrigin[kGatherMaxCascades][3] = {};
    float voxelSize[kGatherMaxCascades][3] = {};
    float voxelCell[kGatherMaxCascades] = {};
    float voxelMultiplier[kGatherMaxCascades] = {};
    unsigned cascadeCount = 0u;
    bool anisotropic = false;

    /// The camera's basis, in the five vectors the shaders reconstruct with.
    float camPos[4] = { 0, 0, 0, 1 };
    float rayTL[3] = {}, rayRight[3] = {}, rayDown[3] = {}, fwd[3] = {};
    float viewAxisX[3] = {}, viewAxisY[3] = {}, viewAxisZ[3] = {};
    float projA = 0.0f, projB = 0.0f, farClip = 0.0f;
    /// THE FAR QUERY'S HAND-OVER WIDTH (world units, ATOM-FARBLAS-1 audit F2):
    /// the widest gap between a traced mesh's coarsest level and its fine one,
    /// at its instance's scale. The far query starts that far before the near
    /// length.
    float farOverlap = 0.0f;

    unsigned width = 0u, height = 0u;

    /// THE TIER TABLE'S GATHER ROW for this scene (Types.h `GiGatherFacts`,
    /// through `giQualityFacts` — the quality dial and the document's Epic
    /// tier): the stride, the octahedral resolution and the adaptive cap. Never
    /// the view's SSR row (GA-TIERROW). ...and the test door's overrides.
    GiGatherFacts facts;
    GatherTuning tuning;
    /// THE SCENE'S REST KEY (OgreScene::gatherRestKey): moves on a light write,
    /// wherever an injection LANDS, when the scene's geometry moves and when the
    /// surface cache captures or relights. With the camera's basis it decides
    /// whether this frame is a REST frame (the rest mean, then the hold).
    unsigned long long restKey = 0ull;
    /// THE SCENE'S RESTART KEY (OgreScene::gatherRestartKey, PHOTON-GATHER-1d fix
    /// round): moves only on a DISCONTINUITY the history cannot follow per pixel
    /// — a light, sky or material write, an injection landing. A continuous
    /// transform write (an animation, a rider, a mover) is NOT one: reprojection
    /// follows it. It decides the SETTLED term (GatherStatus::settled), never the
    /// hold, which keeps `restKey`.
    unsigned long long restartKey = 0ull;

    /// THE SURFACE CACHE THE HITS READ FIRST (PHOTON-GATHER-1d, GA-1e) — the
    /// scene's two tables and two atlas layers, bound as the reflection trace
    /// binds them, or all null (the stand-ins are bound and every hit reads the
    /// voxels). `cardSlots` / `cardRecords` are what the shader may index.
    Ogre::UavBufferPacked *cardTable = nullptr;
    Ogre::UavBufferPacked *cardInstances = nullptr;
    Ogre::TextureGpu *cardDepth = nullptr;
    Ogre::TextureGpu *cardRadiance = nullptr;
    unsigned cardSlots = 0u, cardRecords = 0u;
    float cardFootprintTexels = 0.0f;
    /// THE HIT'S GEOMETRIC NORMAL: the per-slot geometry-row table the scene's
    /// TLAS was written with (copied per frame in flight by the gather) and the
    /// GPU scene's rows, already FLUSHED by the tier this frame. Null = none: the
    /// card pick faces the reversed ray.
    const std::vector<uint32_t> *geomRowOfSlot = nullptr;
    Ogre::UavBufferPacked *geomRows = nullptr;
};

/// The Component.
class ScreenProbeGather {
public:
    explicit ScreenProbeGather(GatherHost &host) : mHost(host) {}
    ~ScreenProbeGather();

    /// Every Vulkan object released. Called by the tier's own close(), and it
    /// also takes every shader REGISTRATION away — a pass that binds a texture
    /// this object owned after it is gone is the D1 defect GATHER-0 recorded.
    void close();

    /// One view's frame, recorded into the frame's command buffer at the
    /// reflection listener's hook (after the prepass, before the opaque pass
    /// that shades). `key` identifies the view's listener and nothing else.
    void record(const void *key, const GatherInputs &in);
    /// The pass this key registered for is over: the Hlms binding is
    /// PASS-scoped and is taken away here (GATHER-0's D2).
    void releaseBinding(const void *key);
    /// A view's listener is going away, or its scene has disarmed.
    void forget(const void *key);
    /// The last frame's numbers for a scene.
    /// `restKey` / `restartKey` are the scene's CURRENT keys: a view whose last
    /// frame saw another restart key has not begun the settle its picture owes
    /// (a light write between two frames), and one that saw another rest key has
    /// not begun its rest.
    void statsInto(const detail::OgreScene *scene, unsigned long long restKey,
                   unsigned long long restartKey, GatherStatus &out) const;
    /// Is anything at all held for this key?
    bool holds(const void *key) const { return mViews.count(key) != 0; }

private:
    /// One view's resident state. The atlas and the records are RAW Vulkan
    /// objects this file owns outright — nothing but these three compute jobs
    /// ever touches them, they are never render targets, and handing them to
    /// the compositor would put a per-frame race inside a graph that rebuilds.
    /// The full-resolution irradiance IS an Ogre texture, because HlmsPbs binds
    /// it through the listener's extra-texture slot and HlmsPbs speaks
    /// TextureGpu.
    struct View {
        VkDescriptorSet placeSets[3] = {};
        VkDescriptorSet traceSets[3] = {};
        VkDescriptorSet filterSets[3] = {};
        VkDescriptorSet integrateSets[3] = {};
        VkBuffer params[3] = {};
        VkDeviceMemory paramsMemory[3] = {};
        void *paramsMapped[3] = {};
        /// THE PER-SLOT GEOMETRY ROW TABLE, a copy per frame in flight (the
        /// scene's vector moves under a later frame) — GA-1e's card pick.
        VkBuffer geomRowOfSlot[3] = {};
        VkDeviceMemory geomRowOfSlotMemory[3] = {};
        void *geomRowOfSlotMapped[3] = {};
        VkDeviceSize geomRowOfSlotBytes[3] = {};

        VkBuffer records = VK_NULL_HANDLE;
        VkDeviceMemory recordsMemory = VK_NULL_HANDLE;
        VkBuffer counter = VK_NULL_HANDLE;
        VkDeviceMemory counterMemory = VK_NULL_HANDLE;
        VkBuffer args = VK_NULL_HANDLE;
        VkDeviceMemory argsMemory = VK_NULL_HANDLE;
        /// HOW MANY ADAPTIVE PROBES THE FRAME APPENDED, copied out of the
        /// device-local counter into host memory and read back several frames
        /// late — never mapped for the GPU to increment directly: an atomic in
        /// host-visible memory crosses the bus once per appending cell, and
        /// there can be thousands of them.
        VkBuffer readback = VK_NULL_HANDLE;
        VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
        void *readbackMapped = nullptr;

        VkImage atlas = VK_NULL_HANDLE;
        VkDeviceMemory atlasMemory = VK_NULL_HANDLE;
        VkImageView atlasView = VK_NULL_HANDLE;
        /// THE PIXEL HISTORY (PHOTON-GATHER-1c), full resolution, ping-ponged:
        /// `history` rgba16f (premultiplied mean E/pi, mean coverage) and
        /// `historyGeom` r32ui (distance, normal, count) — rq_probe_integrate.comp
        /// says what each bit is.
        VkImage history[2] = {};
        VkDeviceMemory historyMemory[2] = {};
        VkImageView historyView[2] = {};
        VkImage historyGeom[2] = {};
        VkDeviceMemory historyGeomMemory[2] = {};
        VkImageView historyGeomView[2] = {};
        /// Which of the pair is THIS frame's.
        unsigned flip = 0u;
        /// THE VIEW'S AGE: consecutive frames the history has been written
        /// (0 = the previous images hold nothing and are never read).
        unsigned age = 0u;
        /// THE REST (PHOTON-GATHER-1d): consecutive frames with the camera, the
        /// scene's rest key and the estimator unchanged, and the key the last
        /// frame saw. Past N (settleFramesOf) the view HOLDS: nothing is dispatched.
        unsigned restFrames = 0u;
        unsigned long long restKey = 0ull;
        /// THE SETTLE (the fix round's split): frames drawn since the history's
        /// last RESTART — its birth, a resize, a scene bind, the lever, an
        /// estimator-changing tuning, or a move of the scene's restart key.
        /// `settled` is this >= N; the hold above is the stronger rest.
        unsigned sinceRestart = 0u;
        unsigned long long restartKey = 0ull;
        /// THE REST MEAN, full resolution (rq_probe_integrate.comp): a true mean
        /// of the rest frames, premultiplied by coverage. One image — at rest a
        /// pixel reads and writes only itself.
        VkImage restMean = VK_NULL_HANDLE;
        VkDeviceMemory restMeanMemory = VK_NULL_HANDLE;
        VkImageView restMeanView = VK_NULL_HANDLE;
        /// The history's EMA floor in frames, as the last frame ran it.
        unsigned historyFramesLast = 10u;
        /// Ogre's frame number of the last frame this view recorded — which of
        /// a scene's views drew the LATEST frame (statsInto reports those).
        uint32_t recordedFrame = 0u;
        /// ...and what makes it restart besides new targets: the history
        /// switched back on, or a GatherTuning field that changes the estimator.
        bool temporalLast = false;
        GatherTuning tuningLast;
        /// What the last recorded frame ran with (GatherStatus).
        bool lastTemporal = false;
        /// THE PREVIOUS FRAME'S CAMERA, in the five vectors the shaders invert.
        float prevCamPos[4] = {}, prevRayTL[4] = {}, prevRayRight[4] = {}, prevRayDown[4] = {},
              prevFwd[4] = {};
        Ogre::TextureGpu *irradiance = nullptr;

        const detail::OgreScene *scene = nullptr;
        Ogre::SceneManager *sceneMgr = nullptr;
        unsigned w = 0u, h = 0u, stride = 0u, octRes = 0u;
        unsigned gridW = 0u, gridH = 0u, uniformProbes = 0u, adaptiveCap = 0u, atlasCols = 0u;
        unsigned atlasW = 0u, atlasH = 0u;
        bool targetsReady = false;
        bool atlasNeedsClear = false;
        unsigned frame = 0u;             ///< the sample sequence's input
        unsigned adaptiveLast = 0u;      ///< read back from the counter, a frame late
        unsigned adaptiveAsked = 0u;     ///< ...before the cap clamped it
        unsigned long long vramBytes = 0ull;

        /// THE IRRADIANCE READBACK (GatherTuning::readback): a host-visible
        /// ring of kFramesInFlight full-resolution copies, allocated on the first
        /// frame that asks and freed with the view, and the last retired copy
        /// decoded to floats for GatherStatus.
        VkBuffer irrReadback = VK_NULL_HANDLE;
        VkDeviceMemory irrReadbackMemory = VK_NULL_HANDLE;
        void *irrReadbackMapped = nullptr;
        std::vector<float> irrHost;
        unsigned irrHostFrame = 0u;

        unsigned querySlot = 0u, queryBase = 0u;
        bool hasQueryBase = false;
        struct Pending {
            uint32_t frame = 0u;
            bool live = false;
            bool irradiance = false;     ///< this slot of the readback ring was written
            unsigned gatherFrame = 0u;   ///< ...by this gather frame
            /// A HELD frame (the rest, PHOTON-GATHER-1d): only the readback was
            /// recorded — no counter copy and no timestamps to read back.
            bool held = false;
        };
        Pending pending[3];
        float placeMs = -1.0f, traceMs = -1.0f, filterMs = -1.0f, integrateMs = -1.0f, cpuMs = -1.0f;
    };

    bool makePipelines(std::string &err);
    bool ensureTargets(View &v, const GatherInputs &in, unsigned stride, unsigned octRes,
                       unsigned adaptiveCap, std::string &err);
    void drop(View &v);
    /// The timestamps AND the adaptive count of the frames that have retired.
    void readPending(View &v);
    /// A HELD frame (PHOTON-GATHER-1d): the view has been at rest for N frames,
    /// its answer IS the rest mean, and nothing is dispatched — the irradiance
    /// is re-bound (and copied out when the readback door asks).
    void hold(View &v, const GatherInputs &in, bool temporal);
    void clearAtlas(View &v, VkCommandBuffer cmd);

    GatherHost &mHost;
    std::unordered_map<const void *, View> mViews;

    VkDescriptorSetLayout mPlaceLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mTraceLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mFilterLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mIntegrateLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPlacePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mTracePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mFilterPipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mIntegratePipeLayout = VK_NULL_HANDLE;
    VkPipeline mPlacePipeline = VK_NULL_HANDLE;
    VkPipeline mTracePipeline = VK_NULL_HANDLE;
    VkPipeline mFilterPipeline = VK_NULL_HANDLE;
    VkPipeline mIntegratePipeline = VK_NULL_HANDLE;
    VkShaderModule mPlaceModule = VK_NULL_HANDLE;
    VkShaderModule mTraceModule = VK_NULL_HANDLE;
    VkShaderModule mFilterModule = VK_NULL_HANDLE;
    VkShaderModule mIntegrateModule = VK_NULL_HANDLE;
    VkDescriptorPool mPool = VK_NULL_HANDLE;
    VkQueryPool mTimestamps = VK_NULL_HANDLE;
    uint32_t mQuerySlots = 0u;
    float mTimestampPeriod = 0.0f;
    /// The pipelines could not be made on this device: say so ONCE and take the
    /// fallback picture (the cones and the field) for the rest of the process.
    /// A failed ALLOCATION does not latch — see `record`.
    bool mFailed = false;
    /// ...and why, for `GiStatus::gather.error`: a row that silently does
    /// nothing is the worst of the three outcomes.
    std::string mLastError;
    /// This device's guaranteed work-group ceiling on X (0 until the pipelines
    /// are made), and whether the clamp has been said once.
    uint32_t mMaxWorkGroupX = 0u;
    bool mSaidWorkGroupClamp = false;
};

#endif   // JAH_RAY_QUERY

}   // namespace engine
}   // namespace jahshaka
