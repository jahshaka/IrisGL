// THE SCREEN-PROBE GATHER — a Component of ours, beside PhotonVoxelLighting and
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
// allocator with the retire window fork b028638c1 (was 0067) taught it, the frame's command
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
// and Epic on, Medium at 36 rays, Low off). SINCE PHOTON-GA-VR: STEREO — a
// two-eye target gathers per EYE in the same four dispatches (each eye its own
// layer and its own probe grid, LAYERED-STEREO-1; each eye reconstructs and
// reprojects through its own basis — the reflection's REFLECT-VR-1 rule), and the pixel
// history is ONE PACKED 8-byte texel (16 B a pixel for the pair, every view).
#pragma once

#include "jahshaka/engine/Types.h"
#include "SurfaceCache.h"   // kViewLayers: the card read's view-term layers (PHOTON-CARDS-5)

#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef JAH_RAY_QUERY
#    include <vulkan/vulkan.h>
#    include <OgreResourceTransition.h>   // the hit set's transitions (hitSetTransitions)
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

/// A CAMERA CUT (PHOTON-2 round 2, N3 - Lumen's IsLargeCameraMovement, adopted as is): between two frames' bases one of
/// the view's axes (right, down, forward - each normalised here: the ray basis carries the field of view in its length)
/// turned more than kCameraCutDegrees, or the eye moved more than kCameraCutMetres. A view's histories RESTART on it
/// (the gather's age 0, the reflection's images cleared): a teleport across the map or a snap turn would otherwise
/// ghost through whatever pixels the reprojection's tests happened to accept. Under that a move is a walk, followed by
/// the reprojection pixel by pixel.
constexpr float kCameraCutDegrees = 75.0f, kCameraCutMetres = 100.0f;
inline bool isCameraCut(const float *prevPos, const float *prevRight, const float *prevDown, const float *prevFwd,
                        const float *pos, const float *right, const float *down, const float *fwd)
{
    const auto dot3 = [](const float *a, const float *b) { return double(a[0]) * b[0] + double(a[1]) * b[1] + double(a[2]) * b[2]; };
    const double cosCut = std::cos(double(kCameraCutDegrees) * 3.14159265358979323846 / 180.0);
    const float *was[3] = { prevRight, prevDown, prevFwd }, *now[3] = { right, down, fwd };
    for (int i = 0; i < 3; ++i) {
        const double la = std::sqrt(dot3(was[i], was[i])), lb = std::sqrt(dot3(now[i], now[i]));
        if (!(la > 0.0) || !(lb > 0.0)) continue;
        if (dot3(was[i], now[i]) / (la * lb) < cosCut) return true;
    }
    const float d[3] = { pos[0] - prevPos[0], pos[1] - prevPos[1], pos[2] - prevPos[2] };
    return dot3(d, d) > double(kCameraCutMetres) * kCameraCutMetres;
}

#ifdef JAH_RAY_QUERY

/// HOW MANY CASCADES THE HIT SHADING READS — the shaders' `kMaxCascades`, and
/// the two must agree.
constexpr unsigned kGatherMaxCascades = 4u;

/// The volumes per cascade the trace binds (the reflection's kRayVoxelKinds' order): the front, X, Y, Z, coverage +/-,
/// position +/-, the back, the normal, the direct term's front and its back (PHOTON-7 stage 7).
constexpr unsigned kGatherVoxelKinds = 12u;
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
    /// The measurement arm "gather.temporal" (lane TEST-1): the pixel history on.
    virtual bool gatherTemporalArm() const = 0;
    /// The measurement arm "gather.decodeHits" (SPECKLE-FIX-1): every hit decoded.
    virtual bool gatherDecodeHitsArm() const = 0;
    /// Outside every encoder, ready for compute and barriers.
    virtual VkCommandBuffer gatherFrameCmd() = 0;

    virtual bool gatherMakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                                  VkBuffer &buffer, VkDeviceMemory &memory, void **mapped,
                                  std::string &err) = 0;
    /// `layers` 0 = a plain 2D image (the probe atlas); n >= 1 = a SCREEN image of
    /// n layers, one per eye, with a 2D_ARRAY view (LAYERED-STEREO-1).
    virtual bool gatherMakeImage(unsigned w, unsigned h, unsigned layers, VkFormat fmt,
                                 VkImage &image, VkDeviceMemory &memory, VkImageView &view,
                                 std::string &err) = 0;
    /// A retired 2D_ARRAY view of every layer of an Ogre screen texture (the
    /// tier's layerView): what the gather's screen shaders declare.
    virtual VkImageView gatherLayerView(Ogre::TextureGpu *t, bool storage) = 0;

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
    /// A compute pipeline THROUGH THE DEVICE'S PIPELINE CACHE (PHOTON-II-1 item 12): the
    /// tier's own builder (persisted with the shader cache, counted and named in
    /// ShaderCacheStats) — never a pipeline of the gather's own outside it.
    virtual VkResult gatherCreatePipeline(const VkComputePipelineCreateInfo &cpi, VkPipeline *out,
                                          const char *name) = 0;
    virtual VkSampler gatherLinearSampler() const = 0;
    /// FAR-SUN-2: the wrapped trilinear sampler (a hit's material maps, the cloud field) and
    /// the flat stand-in as a one-layer 2D array (the texture pool's binding when none).
    virtual VkSampler gatherWrapSampler() const = 0;
    virtual VkImageView gatherDummyFlatArray() const = 0;
    /// ...and an UNSIGNED 2D stand-in, for a usampler (the cards' SunVis layer, RGBA32_UINT: the one read's F3); valid
    /// after gatherDummies.
    virtual VkImageView gatherDummyUint() const = 0;
};

/// EVERYTHING ONE FRAME OF ONE VIEW NEEDS, gathered by the tier from the scene
/// and the compositor pass. The gather takes what it needs as ARGUMENTS and is
/// friends with nothing — the debt `SurfaceCardSpike.h` names for a spike, paid
/// here because this one ships.
/// THE HIT LIST a ray job appends to (PHOTON-HIT-SHADE-1; the ray tier owns it,
/// rayquery/include/jah_rq_hit_record.glsl says what a record is): the two
/// images' storage views, the list's buffer (counters + each record's aux), the GPU scene's instance table (the flags
/// a hit reads), the list's shape and the sun ray's parameters. `on` false binds
/// the tier's stand-ins and appends nothing (a hit no cache shades then keeps
/// the write-back's "unshaded" answer).
struct HitListBinding {
    bool on = false;
    VkImageView ids = VK_NULL_HANDLE, dest = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize bufOffset = 0, bufRange = 0;
    VkBuffer instances = VK_NULL_HANDLE;
    VkDeviceSize instancesOffset = 0, instancesRange = 0;
    uint32_t instanceEntries = 0u;
    uint32_t capacity = 0u, width = 0u;
    bool sun = false;
    float toSun[3] = { 0.0f, 1.0f, 0.0f };
    uint32_t sunMask = 0u;
    /// farLift: a FAR copy's hit's sun-leg surface term (farOverlap, the drift law's gap); a near hit has
    /// none (it is ON level 0) - the one origin law, jah_ray_origin.glsl.
    float sunRange = 0.0f, farLift = 0.0f;
};

/// THE HIT SET (PHOTON-7 stage 10, 10-2a; the lead's ruling: ONE function writes it, never a copy a job): every input
/// the hit read takes - jahHitRadianceStore (jah_rq_hit_radiance.glsl) and its halves: the card read (jah_rq_card.glsl,
/// the tables and the layers), the voxel read (jah_rq_hit.glsl, every cascade's twelve volumes), the hit's geometric
/// normal (jah_rq_geom.glsl), the record a hit no cache shades appends (jah_rq_hit_record.glsl), the hit's own surface
/// (jah_rq_hit_direct.glsl) - and the TLAS the rays trace. THE TIER COLLECTS IT (RayQueryTier::hitSetOf: the scene's
/// cascades, its environment, its surface cache, its GPU scene's rows, its hit table and surface), each ray job that
/// reads hits - the reflection (rq_reflect.comp), the gather (rq_probe_gather.comp), the cards' gather - lays it out
/// (hitSetLayout), writes it (HitSetWrite) and orders it (hitSetTransitions) at ITS OWN binding numbers (HitSetSlots).
struct HitSetSources {
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    /// The cascade chain's light volumes: [cascade][kind], the kinds in kGatherVoxelKinds' order - 0 the total
    /// (level 0), 1..3 the directional chains, 4/5 the coverage per half-axis (+a, -a), 6/7 the surface position per
    /// half (RQ-COV-SLOT-1 / PHOTON-VOXEL-4), 8 level 0's back side, 9 the voxelizer's normal (PHOTON-VOXEL-5), 10 the
    /// direct term's front (MOVER-OCCLUSION-1), 11 its back (PHOTON-7 stage 7: the stores hold two sides). Innermost
    /// first; past `cascadeCount` the slots bind the last cascade's (a shader indexes kGatherMaxCascades).
    Ogre::TextureGpu *voxel[kGatherMaxCascades][kGatherVoxelKinds] = {};
    /// THE WORLD's BOX (PHOTON-7 10-3 R4; L9: "ran out of distance" is not a miss): the union of the traced instances'
    /// world AABBs - where a ray EXITS the world, the only place the sky is. `worldOn` false: no instance.
    float worldMin[3] = {}, worldMax[3] = {};
    bool worldOn = false;
    float voxelOrigin[kGatherMaxCascades][3] = {};
    float voxelSize[kGatherMaxCascades][3] = {};
    /// ...each cascade's cell, its largest side (metres).
    float voxelCell[kGatherMaxCascades] = {};
    unsigned cascadeCount = 0u;
    /// THE LEVEL BY THE CAMERA (PHOTON-7 stage 10, 10-2e, the lead's ruling): a hit reads cascade c only within
    /// `levelRadius[c]` of the GI camera (max-norm, metres) - its guaranteed radius less the margin - blending out over
    /// `levelBand[c]` below it; the outermost by its box. `levelOn` false: no camera known (the box rule alone).
    float levelCam[3] = {};
    float levelRadius[kGatherMaxCascades] = {};
    float levelBand[kGatherMaxCascades] = {};
    /// ...and the coarsest footprint level it answers, K (its placement quantum's: giPlacementLevelK - every level up to
    /// it world-anchored); a footprint past it hands to the next one out
    float levelMaxLod[kGatherMaxCascades] = {};
    bool levelOn = false;
    /// THE ONE ENVIRONMENT (PHOTON-ENV-1, OgreScene::rayEnvironment): the sky's captured cube (null: the flat colour).
    Ogre::TextureGpu *sky = nullptr;
    float skyColour[3] = { 0.0f, 0.0f, 0.0f };
    /// THE SURFACE CACHE THE HITS READ FIRST (PHOTON-CARDS-2, GA-1e): the card table, the instance table, the Depth
    /// and Radiance layers and the view term's seven (SurfaceCache::viewLayers) - all set or all null (the stand-ins
    /// with zero slots: every hit reads the voxels). Bound whenever the cache is built with an instance slot, no card
    /// resident too (PHOTON-7 stage 5, 5-2: the hits' demand stamps are what makes a card resident).
    Ogre::UavBufferPacked *cardTable = nullptr;
    Ogre::UavBufferPacked *cardInstances = nullptr;
    Ogre::TextureGpu *cardDepth = nullptr;
    Ogre::TextureGpu *cardRadiance = nullptr;
    Ogre::TextureGpu *cardView[SurfaceCache::kViewLayers] = {};
    unsigned cardSlots = 0u, cardRecords = 0u;
    bool cardsBound() const { return cardSlots > 0u; }
    /// THE CACHE UNBOUND (the stand-ins, zero slots: every hit reads the voxels) - a reader the cards themselves read
    /// (the voxel sides' TLAS gather, PHOTON-7 stage 10 R1) never reads a card: one order, no loop
    void withoutCards() {
        cardTable = nullptr;
        cardInstances = nullptr;
        cardDepth = nullptr;
        cardRadiance = nullptr;
        for (Ogre::TextureGpu *&t : cardView) t = nullptr;
        cardSlots = cardRecords = 0u;
    }
    /// THE HIT'S GEOMETRIC NORMAL: the per-slot geometry-row table the scene's TLAS was written with (a job copies it
    /// per frame in flight into its own buffer) and the GPU scene's rows, FLUSHED by the tier this frame (a row staged
    /// but not uploaded is a zero address: the trap file's GPU SCENE TABLES rule). Null = none: the card pick faces
    /// the reversed ray.
    const std::vector<uint32_t> *geomRowOfSlot = nullptr;
    Ogre::UavBufferPacked *geomRows = nullptr;
    /// THE HIT'S OWN SURFACE (FAR-SUN-2, jah_rq_hit_direct.glsl): the hit table's device address (0 = none: the hit
    /// reads the store whole), the voxeliser's texture pool the rows sample and the cloud layer's shadow (field, map,
    /// sun; null = none).
    uint64_t hitTable = 0u;
    Ogre::TextureGpu *hitPool = nullptr;
    Ogre::TextureGpu *cloudField = nullptr;
    float cloudMap[4] = {};
    float cloudSun[4] = {};
    /// THE ALPHA TABLE's device address (REFLECT-MOVERS-2, jah_rq_alpha.glsl): a cut-out's candidates; 0 = none.
    uint64_t alphaTable = 0u;
    /// THE TIER'S COUNTERS' device address (jah_rq_alpha.glsl's JahTierCounters: [0] the reading rays' pane-list
    /// overflows - the one read's F4); 0 = none.
    uint64_t tierCounters = 0u;
    /// THE HIT LIST (PHOTON-HIT-SHADE-1): where a hit no cache can shade is appended for the decode - the job's own
    /// (the reflection's and the gather's); `on` false binds the stand-ins and appends nothing.
    HitListBinding hit;
};

/// THE HIT SET'S UNIFORMS (PHOTON-7 stage 10, 10-2; the lead's ruling: one text, the bindings' rule) - the C++ mirror of
/// rayquery/include/jah_rq_hit_params.glsl's JahHitParams (std140, vec4s), one member of every hit-reading job's
/// uniform block, filled by the one function hitSetParams.
struct HitSetParams {
    float voxelOrigin[kGatherMaxCascades][4] = {};
    float voxelInvSize[kGatherMaxCascades][4] = {};
    float counts[4] = {};      ///< the cascades, the sky cube on, the per-slot geometry rows, unused
    float skyColour[4] = {};
    float cards[4] = {};       ///< the card slots, the card records, the reader's demand stamps on (z; its demand span)
    float hitList[4] = {};     ///< the list's capacity, width, instance entries, on
    float hitSun[4] = {};      ///< toward the sun, a sun
    float hitSun2[4] = {};     ///< the hit's sun-ray mask, 0, its length, a far copy's lift
    float tables[4] = {};      ///< the alpha table's address, the hit table's (float bits)
    float surface[4] = {};     ///< the texture pool bound, the cloud field bound, the tier's counters' address (zw bits)
    float cloudMap[4] = {};
    float cloudSun[4] = {};
    float levelCam[4] = {};                          ///< the GI camera (xyz), the level rule (w: HitReader's code)
    float level[kGatherMaxCascades][4] = {};         ///< per cascade: its level radius, its band (metres), its K
    float worldMin[4] = {};    ///< THE WORLD's BOX (PHOTON-7 10-3 R4): its corner (xyz), w 1 = known
    float worldMax[4] = {};    ///< ...its far corner (xyz)
};
static_assert(sizeof(HitSetParams) == 25u * 16u, "JahHitParams is twenty-five vec4s");
/// WHO READS THE HITS (PHOTON-7 stage 10-3, the lead's ruling on the teleport window): the level a hit is read at.
///  - View: a VIEW reader (the gather, the reflection) reads the level by the GI CAMERA (10-2e): its picture may depend
///    on its pose, and two placements the engine can reach at one pose read one level (levelCam.w = 1; 0 without a
///    camera: the box rule).
///  - Cache: a CACHE's rays (the cards' gather, the voxel sides) read a level that is CAMERA-INDEPENDENT - the ray's
///    own footprint at the hit (the clipmap law: a cascade's world-anchored levels up to K carry it, fading out between
///    K - 1 and K to the next one out), in the finest box holding the hit, its edge blend (levelCam.w = 2). A cache's
///    store is a world function: under the camera's level it was a function of where the camera stood (the sealed
///    box's c1 cells inside held 0.040 at the outside pose and 0.0029 at the inside rest) and a teleport read the stale
///    part through the walls for a hundred frames (gi.sealed_sky (8)).
enum class HitReader { View, Cache };
/// Fills `out` from the hit set for `reader`; `geomSlots` = the per-slot row entries the job's own copy holds this frame
/// (0: none).
void hitSetParams(const HitSetSources &src, unsigned geomSlots, HitSetParams &out, HitReader reader);

/// WHERE A JOB'S SET HOLDS THE HIT SET: the binding of each input (its shader's layout). Every one is bound.
struct HitSetSlots {
    uint32_t tlas = 0u;
    uint32_t voxel[kGatherVoxelKinds] = {};   ///< an array of kGatherMaxCascades each
    uint32_t sky = 0u;
    uint32_t card[4] = {};                    ///< the card table, the instance table, the Depth layer, the Radiance layer
    uint32_t cardView = 0u;                   ///< the first of SurfaceCache::kViewLayers consecutive bindings
    uint32_t geom[2] = {};                    ///< the per-slot row table, the GPU scene's rows
    uint32_t hit[4] = {};                     ///< the GPU scene's instances, the list's records, its destinations, its buffer
    uint32_t pool = 0u, cloud = 0u;
};
/// The hit set's bindings: the TLAS, the volumes, the sky, the card read's four and its view term's, the geometric
/// normal's two, the record's four, the hit surface's two.
constexpr unsigned kHitSetBindings = 1u + kGatherVoxelKinds + 1u + 4u + SurfaceCache::kViewLayers + 2u + 4u + 2u;

/// THE HIT SET's LAYOUT: its bindings' types and counts written into `b`, indexed by binding (the caller's own
/// bindings are its own).
void hitSetLayout(const HitSetSlots &slots, VkDescriptorSetLayoutBinding *b);

/// THE HIT SET's WRITE: the descriptor infos live in this object until the caller's vkUpdateDescriptorSets.
class HitSetWrite {
public:
    /// Writes the hit set's kHitSetBindings entries of `w` (indexed by binding; sType, dstSet, dstBinding, type, count
    /// and info), every one valid - the tier's stand-ins where a source is absent. `rowOfSlot` = the job's own copy of
    /// the per-slot row table for this frame in flight, or null (the stand-in). False with `err` where a view could
    /// not be made: the job declines its frame.
    bool write(GatherHost &host, VkDescriptorSet set, const HitSetSlots &slots, const HitSetSources &src,
               VkBuffer rowOfSlot, VkWriteDescriptorSet *w, std::string &err);
    /// THE VOLUMES ALONE (the probe integrate's voxel rays, PHOTON-7 10-3 R3 (b)): kind k written at `binding[k]`
    /// (kNoBinding: not bound), `w` indexed by binding - the same views and stand-ins `write` gives the hit set.
    static constexpr uint32_t kNoBinding = 0xFFFFFFFFu;
    bool writeVolumes(GatherHost &host, VkDescriptorSet set, const uint32_t (&binding)[kGatherVoxelKinds],
                      const HitSetSources &src, VkWriteDescriptorSet *w, std::string &err);

private:
    VkWriteDescriptorSetAccelerationStructureKHR mAs{};
    VkDescriptorImageInfo mVolumes[kGatherVoxelKinds][kGatherMaxCascades] = {};
    VkDescriptorImageInfo mSky{}, mCardImgs[2] = {}, mCardView[SurfaceCache::kViewLayers] = {}, mHitImgs[2] = {};
    VkDescriptorImageInfo mPool{}, mCloud{};
    VkDescriptorBufferInfo mCardBufs[2] = {}, mGeomBufs[2] = {}, mHitBufs[2] = {};
};

/// THE HIT SET's ORDER: every input it reads resolved for a compute read (the card instance table read AND written:
/// the hits' demand stamps), into the caller's transitions before its command buffer is taken.
void hitSetTransitions(Ogre::BarrierSolver &solver, Ogre::ResourceTransitionArray &trans, const HitSetSources &src);

/// THE GATHER'S RESUME INPUTS (PHOTON-7 stage 8B, B1; RayQueryTier::gatherFrame fills them, rq_probe_resume.comp reads
/// them): the frame's CHANGED-REGION LIST (PhotonComponent's one buffer, closed before the render), each cascade's BRICK
/// MASK where this frame's light work wrote it (in the frame: no read-back), and whether the irradiance field is at work.
struct GatherRestInputs {
    static constexpr unsigned kCascades = 4u;
    /// The list's buffer (null: none closed yet) and the close it holds (PhotonComponent::regionCloseSerial: a view
    /// reads each close once), its region count, its overflow bits and its restart.
    VkBuffer regions = VK_NULL_HANDLE;
    VkDeviceSize regionsOffset = 0u, regionsRange = 0u;
    unsigned long long regionSerial = 0ull;
    /// ...and the last close that carried an input (PhotonComponent::lastInputCloseSerial): a view that missed draws re-rests
    /// whole only when a close it missed carried one.
    unsigned long long lastInputClose = 0ull;
    unsigned regionCount = 0u;
    uint32_t overflow = 0u;
    bool restart = false;
    /// One cascade's mask, in the hit set's order: its buffer (two words a brick), its bricks, its grid's corner, a
    /// brick in metres, and whether THIS frame's light work wrote it (`live`; a stale mask is no change).
    struct Bricks {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0u, range = 0u;
        uint32_t dims[3] = { 0u, 0u, 0u };
        float origin[3] = { 0.0f, 0.0f, 0.0f };
        float brickMetres = 0.0f;
        bool live = false;
    } bricks[kCascades];
    /// THE FIELD AT WORK (PhotonComponent::fieldWideWorkOwed: its integration, empties, restarts or refinements): every
    /// cell a pixel of which read the field's fallback since the camera stopped resumes.
    bool fieldBusy = false;
    /// THE SCENE'S DRAW at this record (OgreScene::drawSerial): a view that missed one of its scene's lists re-rests whole.
    uint32_t drawSerial = 0u;
    /// Anything at all this frame (a region, an overflow, a live mask, a field box): with none the cells' tests are
    /// skipped and a view past N + 1 such frames holds whole.
    bool any() const {
        if (regionCount || overflow || restart || fieldBusy) return true;
        for (const Bricks &b : bricks)
            if (b.live) return true;
        return false;
    }
};

struct GatherInputs {
    const detail::OgreScene *scene = nullptr;
    Ogre::SceneManager *sceneMgr = nullptr;
    /// THE HIT SET (the TLAS, the cascades, the sky, the surface cache, the geometric normal, the hit's own surface,
    /// the alpha table and the hit list): the tier's one collection (RayQueryTier::hitSetOf), bound
    /// by the trace through the one writer (HitSetWrite).
    HitSetSources hits;

    /// The prepass' G-buffers, by the names the chain declares them under: the normals, the depth, and the shadow /
    /// roughness pair (`jahSsrShadowRough`: the rough specular's roughness, PHOTON-7 stage 7, 7-3).
    Ogre::TextureGpu *normals = nullptr;
    Ogre::TextureGpu *depth = nullptr;
    Ogre::TextureGpu *shadowRough = nullptr;
    /// THE ROUGH SPECULAR'S GATE (PHOTON-7 stage 7, 7-3), in perceptual roughness: the view's TRACED REFLECTION's
    /// cutoff less its feather - where the probes' lobe is the environment term whole (below it the reflection owns
    /// the lobe and its warm-up hands over to the material's cube) - and the gate's ramp below it; a view with no
    /// traced reflection: -1, the lobe everywhere.
    float roughGateFull = -1.0f;
    float roughGateRamp = 0.1f;
    /// THE QUADRATURE DOOR gather.roughQuadrature (7-3; a test door): 1 the filtered map kept beside the levels and the
    /// shipped read, 2 the lobe by brute force over it.
    unsigned roughQuadrature = 0u;

    /// The camera's basis, in the five vectors the shaders reconstruct with.
    /// Under STEREO these are the LEFT eye's, over its own layer as a 0..1 image.
    float camPos[4] = { 0, 0, 0, 1 };
    float rayTL[3] = {}, rayRight[3] = {}, rayDown[3] = {}, fwd[3] = {};
    /// A TWO-EYE TARGET (PHOTON-GA-VR, LAYERED-STEREO-1): every screen texture is
    /// a two-layer array, one eye per layer (`width` is ONE eye's), and the RIGHT
    /// eye's basis is in the second five vectors. The ray tier fills it
    /// from the session's located eyes exactly as the reflection trace does.
    bool stereo = false;
    float camPos2[4] = { 0, 0, 0, 1 };
    float rayTL2[3] = {}, rayRight2[3] = {}, rayDown2[3] = {}, fwd2[3] = {};
    float viewAxisX[3] = {}, viewAxisY[3] = {}, viewAxisZ[3] = {};
    float projA = 0.0f, projB = 0.0f, farClip = 0.0f;
    /// THE FAR QUERY'S HAND-OVER WIDTH (world units, ATOM-FARBLAS-1 audit F2):
    /// the widest gap between a traced mesh's coarsest level and its fine one,
    /// at its instance's scale. The far query starts that far before the near
    /// length.
    float farOverlap = 0.0f;
    /// THE DRAWN SURFACE'S BOUND against level 0, in full-resolution sample footprints (PHOTON-7 stage 6, READ_A W2,
    /// READ_B F3: the origin law, Types.h rayOriginDrawnFootprints) - the probe sits on the drawn surface while its
    /// rays trace level 0. The host sets it every frame; 0 is the rays.originCut control.
    float drawnCutFootprints = rayOriginDrawnFootprints(kLodBudgetPixels, 0.0f);
    /// THE DEPTH-REBUILT POINT'S TERM (READ_B F5 (b), jah_ray_origin.glsl jahDepthPointError): the rasteriser's
    /// sub-pixel snap in pixels, found at the placement with the probe's depth; 0 = the term off (rays.originPoint 0).
    float pointSnapPx = float(1.4142135623730951 / 512.0);
    /// THE IRRADIANCE FIELD, THE FALLBACK OF THE PIXELS NO PROBE ANSWERS (PHOTON-7 stage 7, 7-2): composed into the
    /// gather's texture after its temporal mean (rq_probe_integrate.comp), so the texture's w is 1 (the pass that reads
    /// it binds no field, 7-5). Its two atlases (null = no field: the pixels no probe answers keep no diffuse), its
    /// render block for an identity view (PhotonIrradianceField::fillConstBufferData), the window word and the Y / Z
    /// counts; and the environment's SH, the field's own fallback (world axes, C0 .. C8).
    Ogre::TextureGpu *fieldIrradiance = nullptr;
    Ogre::TextureGpu *fieldDepth = nullptr;
    float field[24] = {};
    float fieldWin[3] = {};
    float envSh[9][3] = {};

    unsigned width = 0u, height = 0u;

    /// THE TIER TABLE'S GATHER ROW for this scene (Types.h `GiGatherFacts`,
    /// through `giQualityFacts` — the quality dial and the document's Epic
    /// tier): the stride, the octahedral resolution and the adaptive cap. Never
    /// the view's SSR row (GA-TIERROW). ...and the test door's overrides.
    GiGatherFacts facts;
    GatherTuning tuning;
    /// WHAT CHANGED THIS FRAME, WHERE (PHOTON-7 stage 8B, B1): the resume's inputs - the cells they reach resume, every
    /// other cell keeps its rest (rq_probe_resume.comp). The view-wide rest key, the inputs-busy and the hold-owed
    /// gates are DELETED with it: a write lands somewhere, and its cells resume when it lands.
    GatherRestInputs rest;
    /// THE FAST UPDATE's skin (PHOTON-7 stage 8B, B5): a skinned item was posed this frame (the skin pass's posed slots at
    /// this frame) - a ray hitting a skinned slot then counts as moving (rq_probe_gather.comp's fastRayMoving)
    bool fastSkinPosed = false;
    bool streakOff = false;   // MEASURING ARM (8B, to be reverted)
    /// THE DEMAND'S DUE (SurfaceCache::demandDueFrames; 0: no span, stamp always): the view stamps the cards it reads at
    /// its rest indices [1, N + this] alone - its demand span (PHOTON-7 stage 10, the per-reader snapshot).
    unsigned demandDue = 0u;
    /// THE SCENE'S RESTART KEY (OgreScene::gatherRestartKey, PHOTON-GATHER-1d fix
    /// round): moves only on a DISCONTINUITY the history cannot follow per pixel
    /// — a light, sky or material write, an injection landing. A continuous
    /// transform write (an animation, a rider, a mover) is NOT one: reprojection
    /// follows it. It decides the SETTLED term (GatherStatus::settled), never the
    /// hold, which keeps `restKey`.
    unsigned long long restartKey = 0ull;
    /// THE VIEW'S CHAIN PARAMETERS THE ESTIMATE DEPENDS ON (PHOTON-7 stage 7, L2: a settings change is a change): the
    /// rough specular's gate against the view's traced reflection (roughGateFull / roughGateRamp, from its chain's
    /// reflection cutoff) and the quadrature door. A move RESTARTS the view's settle and breaks its rest and hold (the
    /// held texel was gated by the old cutoff) - folded beside the scene's keys, never into them (it is the view's).
    unsigned long long chainKey = 0ull;

    /// THE PHOTON VIEW'S ScreenProbes OVERLAY (PHOTON-VIEW-1): the view's overlay
    /// texture (RGBA16F, the target's size) the integrate writes its probe discs
    /// into, or null (the view is not ScreenProbes: nothing is written).
    Ogre::TextureGpu *photonOverlay = nullptr;
    /// ...and which one (OgreView::photonOverlayGeneration: a re-created overlay
    /// may recycle the pointer).
    unsigned photonOverlayGeneration = 0u;
    /// ...and WHOSE picture it is: false = the integrate's ScreenProbes discs, true = the
    /// trace's GatherHits view (FAR-SUN-1: every probe's cell painted ray by ray).
    bool photonOverlayGatherHits = false;
    /// THE GATHER-HITS READBACK (FAR-SUN-1, the arm "gather.hitsReadback"): every uniform
    /// probe's rays recorded and read back (GatherStatus::rays); the view never holds.
    bool hitsReadback = false;
};

/// The Component.
/// THE REST'S LENGTH (OgreScreenProbeGather.cpp settleFramesOf): N — the frames a step of kGatherSettleCodes codes takes
/// to fall under one code through an EMA of `historyFrames` frames, ceil(ln(1/D) / ln(1 - 1/h)); the gather's rest mean
/// is that long at its 10-frame history (16), and the reflection's (rq_reflect_temporal.comp, PHOTON-2 fix F1) at its
/// 32-frame history (51). One function, so the diffuse and the specular rests are one rule.
unsigned gatherSettleFramesOf(unsigned historyFrames);

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
    /// THE SECOND HALF of a frame (PHOTON-HIT-SHADE-1): `record` places and
    /// TRACES (in front of the hit decode pass, whose records the traces append
    /// to); this runs the filter, the SH, the integrate and the pass-scoped
    /// registration in front of the opaque pass — after the write-back has put
    /// the decoded radiance into the atlas. A held frame's re-bind happens here
    /// too. A no-op when `record` did not run for the key this frame.
    void finish(const void *key);
    /// The atlas the key's trace wrote THIS frame (the write-back's gather
    /// destination), or null.
    VkImageView tracedAtlas(const void *key) const;
    /// The pass this key registered for is over: the Hlms binding is
    /// PASS-scoped and is taken away here (GATHER-0's D2).
    void releaseBinding(const void *key);
    /// THE CAPTURE'S BINDING (CLOSE-SHOT-2): a second workspace of the same view,
    /// drawn after the view's own in the same frame, is handed the irradiance THIS
    /// frame already produced — re-registered, nothing recorded, no history stepped.
    /// False when the view recorded nothing this frame.
    bool rebindThisFrame(const void *key);
    /// THE SKY VISIBILITY the blended pass reads (PHOTON-7 10-4 R11): the view's, once the integrate has written it
    /// (null before, or with no view under `key`).
    Ogre::TextureGpu *skyVisOf(const void *key) const;
    /// A FRAME THE VIEW'S GATHER DECLINED (PHOTON-7 stage 7's read, F3: no TLAS, a missing prepass texture, the
    /// pipelines failed): the LAST irradiance and rough specular it produced are registered for the pass - the
    /// target's size and layers, nothing recorded, no history stepped. False when it has none of that shape or
    /// recorded this frame.
    bool holdLast(const void *key, unsigned w, unsigned h, unsigned layers);
    /// A view's listener is going away, or its scene has disarmed.
    /// `fold`: the view is an offscreen picture's (a shot) - its last counts are kept as the last shot's (mForgotten).
    void forget(const void *key, bool fold = false);
    /// The last frame's numbers for a scene.
    /// `restKey` / `restartKey` are the scene's CURRENT keys: a view whose last
    /// frame saw another restart key has not begun the settle its picture owes
    /// (a light write between two frames), and one that saw another rest key has
    /// not begun its rest.
    /// THE REST'S DEMAND COMPLETE (PHOTON-7 stage 10, R3 (b)): every view of `scene` that drew the latest frame and CAN
    /// rest has traced its rest mean's N frames under the rest key - its rest sample sequence done, the frame it would
    /// hold. A view that cannot rest (stereo, the rest off, no temporal history) has no rest sequence and waits on
    /// nothing; no view, nothing to wait for.
    /// `restFrames`: the completed rest's length so far (the least over those views; 0 with none that can rest);
    /// `restLength`: the rest sequence's N (the longest over those views; the default tuning's with none).
    /// A reader view that has not recorded for more than this many of ITS SCENE'S DRAWS (OgreScene::drawSerial - never
    /// engine frames the scene missed) makes no rest demand (restDemandComplete; RayQueryTier::cardReadersRestComplete
    /// the reflection's): the frames in flight (3) + the cache's one frame late.
    static constexpr uint32_t kReaderStaleFrames = 4u;
    /// ...complete once each of its views has traced its demand span, its rest indices [1, N + `due`] (the per-reader
    /// snapshot, PHOTON-7 stage 10); `active`: a view recorded lately (a stale one makes no demand).
    bool restDemandComplete(const detail::OgreScene *scene, unsigned &restFrames, unsigned &restLength, unsigned due,
                            bool &active) const;
    /// The rest sequence's N at the shipped tuning (the cards' readers' demand length where no gather runs).
    static unsigned shippedRestLength();
    void statsInto(const detail::OgreScene *scene, unsigned long long restartKey, GatherStatus &out) const;
    /// The shot-view fold's numbers alone (statsInto calls it; also with no live view of the scene).
    void shotInto(const detail::OgreScene *scene, GatherStatus &out) const;
    /// ...and its entry's end, with the scene (RayQueryTier::forgetScene).
    void forgetScene(const detail::OgreScene *scene);
    /// Is anything at all held for this key?
    bool holds(const void *key) const { return mViews.count(key) != 0; }
    /// Will this frame's second half run the integrate for `key` (a frame recorded
    /// and not held)? The photon view's overlay is written exactly then.
    bool integrates(const void *key) const {
        auto it = mViews.find(key);
        return it != mViews.end() && it->second.finishPending && !it->second.finishHold;
    }

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
        /// ONE rg32ui texel a pixel (PHOTON-GA-VR — the premultiplied mean E/pi
        /// as a shared-exponent word; the distance, normal, count and mean
        /// coverage as the other) — rq_probe_integrate.comp says what each bit is.
        VkImage history[2] = {};
        VkDeviceMemory historyMemory[2] = {};
        VkImageView historyView[2] = {};
        /// THE HISTORY's VALIDITY STREAK (PHOTON-7 stage 8B, the lead's ruling): R8 at the history's resolution, a
        /// ping-pong pair like it - the frames each pixel's history has been carried unbroken (/ 255), 0 none. The count in
        /// the history word is the blend's N (B5's fast update stores N'), never this.
        VkImage streak[2] = {};
        VkDeviceMemory streakMemory[2] = {};
        VkImageView streakView[2] = {};
        /// Which of the pair is THIS frame's.
        unsigned flip = 0u;
        /// THE VIEW'S AGE: consecutive frames the history has been written
        /// (0 = the previous images hold nothing and are never read).
        unsigned age = 0u;
        /// THE REST, PER CELL (PHOTON-7 stage 8B, B1; jah_probe_rest.glsl): the rest clock K as the host knows it
        /// (restKnown: the read-back's, extended by the frames since where no input could reach a cell; a frame with
        /// inputs since makes it the frames since the last such frame - a lower bound until its read-back lands).
        unsigned restFrames = 0u;
        /// ...the cells' buffers (device-local): their state (two words a cell: the state bits, the ring bit), their
        /// distance store (130 words a cell: 520 B), the clock (eight words), the brick pyramids (sized by the masks)
        VkBuffer cellState = VK_NULL_HANDLE, cellStore = VK_NULL_HANDLE, clock = VK_NULL_HANDLE, pyramid = VK_NULL_HANDLE;
        VkDeviceMemory cellStateMemory = VK_NULL_HANDLE, cellStoreMemory = VK_NULL_HANDLE, clockMemory = VK_NULL_HANDLE,
                       pyramidMemory = VK_NULL_HANDLE;
        VkDeviceSize pyramidBytes = 0u;
        /// ...made this size, and zeroed on the device yet (every cell active, the clock at 0)
        bool cellsCleared = false;
        /// ...the resume's parameter block and set per ring slot
        VkBuffer restParams[3] = {};
        VkDeviceMemory restParamsMemory[3] = {};
        void *restParamsMapped[3] = {};
        VkDescriptorSet resumeSets[3] = {};
        /// ...the changed-region list's close this view read last (a close is read once)
        unsigned long long regionSerialSeen = 0ull;
        /// ...the gather frame of the last frame that had a resume input or moved, and of the last that moved (the
        /// clock's 0 for certain: a retired clock older than it says nothing of this rest)
        unsigned lastInputFrame = 0u, lastMovedFrame = 0u;
        /// ...the latest retired clock: its gather frame, K, and the cells reached / held / active / ring then, and
        /// the frames that resumed (cumulative); `restLog` the last retired records (GatherStatus::restLog)
        bool clockKnown = false;
        unsigned clockFrame = 0u, clockK = 0u, cellsReached = 0u, cellsHeld = 0u, cellsActive = 0u, cellsRing = 0u;
        unsigned long long resumes = 0ull;
        std::vector<GatherRestRecord> restLog;
        /// THE CAMERA CUTS this view detected (N3: isCameraCut) - each one a history restart.
        unsigned cameraCuts = 0u;
        /// PHOTON-2 L8: the pixels whose history the foliage rule ALONE accepted (between the 1 % and 3 % depth
        /// bounds), summed over the retired frames.
        unsigned long long foliageRuleAccepts = 0ull;
        /// PHOTON-7 stage 10-3: the pixels that reached the hold with no rest sample (NO SAMPLE there), the diffuse's and
        /// the rough specular's, the last traced frame's
        unsigned starvedDiffuse = 0u, starvedRough = 0u;
        /// ...and whether it HELD its last frame (GatherStatus::held)
        bool heldLast = false;
        /// THE SETTLE (the fix round's split): frames drawn since the history's
        /// last RESTART — its birth, a resize, a scene bind, the lever, an
        /// estimator-changing tuning, or a move of the scene's restart key.
        /// `settled` is this >= N; the hold above is the stronger rest.
        unsigned sinceRestart = 0u;
        unsigned long long restartKey = 0ull;
        unsigned long long chainKey = 0ull;   // GatherInputs::chainKey, the frame before
        /// THE REST MEAN, full resolution (rq_probe_integrate.comp): a true mean
        /// of the rest frames, premultiplied by coverage. One image — at rest a
        /// pixel reads and writes only itself.
        VkImage restMean = VK_NULL_HANDLE;
        VkDeviceMemory restMeanMemory = VK_NULL_HANDLE;
        VkImageView restMeanView = VK_NULL_HANDLE;
        /// THE PREFILTERED MAPS (PHOTON-7 stage 7, 7-3; the split sum on the probes): the filtered map convolved with
        /// the GGX lobe at each of kRoughSpecLevels roughnesses, one layer a level, at the raw atlas' addressing,
        /// R11G11B10 - what the integrate's rough specular reads (rq_probe_filter.comp's job 3).
        VkImage prefiltered = VK_NULL_HANDLE;
        VkDeviceMemory prefilteredMemory = VK_NULL_HANDLE;
        VkImageView prefilteredView = VK_NULL_HANDLE;
        /// THE ROUGH SPECULAR'S HISTORY (7-3), full resolution, ping-ponged with the diffuse's pair (the same flip): ONE
        /// r32ui word a pixel, the lobe's mean as a shared-exponent word - validated by the diffuse's texel at the same
        /// coordinate (rq_probe_integrate.comp) - and its REST MEAN (rgba16f; a 1 x 1 stand-in for a stereo view).
        VkImage roughHistory[2] = {};
        VkDeviceMemory roughHistoryMemory[2] = {};
        VkImageView roughHistoryView[2] = {};
        VkImage roughRest = VK_NULL_HANDLE;
        VkDeviceMemory roughRestMemory = VK_NULL_HANDLE;
        VkImageView roughRestView = VK_NULL_HANDLE;
        /// The history's EMA floor in frames, as the last frame ran it.
        unsigned historyFramesLast = 10u;
        /// Ogre's frame number of the last frame this view recorded — which of
        /// a scene's views drew the LATEST frame (statsInto reports those).
        uint32_t recordedFrame = 0u;
        /// ...and its scene's draw count then (OgreScene::drawSerial): the reader's clock, which staleness is timed on.
        uint32_t recordedDraw = 0u;
        /// ...and what makes it restart besides new targets: the history
        /// switched back on, or a GatherTuning field that changes the estimator.
        bool temporalLast = false;
        /// THE PHOTON VIEW's overlay generation the discs were written into last
        /// frame (0: none asked, PHOTON-VIEW-1): a change — on, off, or a new texture
        /// after a resize — breaks the REST (never the history) so the integrate
        /// runs and the overlay is written; a held view dispatches nothing.
        unsigned photonLast = 0u;
        GatherTuning tuningLast;
        /// What the last recorded frame ran with (GatherStatus).
        bool lastTemporal = false;
        /// The view CAN rest (temporal, not stereo, the rest not off: its still predicate's standing half), at its last
        /// record - restDemandComplete waits only on a view that can.
        bool restableLast = false;
        /// THE PREVIOUS FRAME'S CAMERA, in the five vectors the shaders invert
        /// (the left eye's under stereo), and the right eye's beside it.
        float prevCamPos[4] = {}, prevRayTL[4] = {}, prevRayRight[4] = {}, prevRayDown[4] = {},
              prevFwd[4] = {};
        float prevCamPos2[4] = {}, prevRayTL2[4] = {}, prevRayRight2[4] = {},
              prevRayDown2[4] = {}, prevFwd2[4] = {};
        Ogre::TextureGpu *irradiance = nullptr;
        /// THE ROUGH SPECULAR (7-3), `jahRoughSpec`: an Ogre texture for the same reason as the irradiance.
        Ogre::TextureGpu *roughSpec = nullptr;
        /// THE SKY VISIBILITY (PHOTON-7 10-4 R11), `jahSkyVis`: R8, the integrate's (rq_probe_integrate.comp binding 18).
        Ogre::TextureGpu *skyVis = nullptr;
        /// The prefiltered image's layers as made: the levels, and one more under the quadrature door (the filtered
        /// map itself).
        unsigned prefilteredLayers = 0u;

        const detail::OgreScene *scene = nullptr;
        Ogre::SceneManager *sceneMgr = nullptr;
        unsigned w = 0u, h = 0u, stride = 0u, octRes = 0u;
        /// The placement counter's per-row words (one per eye and grid row).
        unsigned placeRows = 0u;
        unsigned gridW = 0u, gridH = 0u, uniformProbes = 0u, adaptiveCap = 0u, atlasCols = 0u;
        /// THE TARGET'S SHAPE (PHOTON-GA-VR, LAYERED-STEREO-1): two eyes, one per
        /// LAYER of every screen image (`layers` = 2), each `eyeW` pixels and
        /// `eyeGridW` probe columns wide; the probe LIST holds the two grids side
        /// by side (gridW = 2 x eyeGridW). One eye = one layer and the whole grid.
        bool stereo = false;
        unsigned layers = 1u;
        unsigned eyeW = 0u, eyeGridW = 0u;
        unsigned atlasW = 0u, atlasH = 0u;
        bool targetsReady = false;
        bool atlasNeedsClear = false;
        unsigned frame = 0u;             ///< the sample sequence's input
        unsigned adaptiveLast = 0u;      ///< read back from the counter, a frame late
        unsigned adaptiveAsked = 0u;     ///< ...before the cap clamped it
        unsigned long long vramBytes = 0ull;
        /// ...of which the pixel history's pair (GatherStatus::historyBytes).
        unsigned long long historyBytes = 0ull;

        /// THE IRRADIANCE READBACK (GatherTuning::readback): a host-visible
        /// ring of kFramesInFlight full-resolution copies, allocated on the first
        /// frame that asks and freed with the view, and the last retired copy
        /// decoded to floats for GatherStatus.
        /// ...and the uniform cells' records of the same frames (their twin links,
        /// GatherStatus::adaptiveCells), decoded beside the irradiance.
        VkBuffer recReadback = VK_NULL_HANDLE;
        VkDeviceMemory recReadbackMemory = VK_NULL_HANDLE;
        void *recReadbackMapped = nullptr;
        std::vector<unsigned char> adaptiveHost;
        VkBuffer irrReadback = VK_NULL_HANDLE;
        VkDeviceMemory irrReadbackMemory = VK_NULL_HANDLE;
        void *irrReadbackMapped = nullptr;
        std::vector<float> irrHost;
        unsigned irrHostFrame = 0u;
        /// THE HISTORY-COUNT READBACK (GatherTuning::historyReadback, PHOTON-2 stage 0): the same ring shape
        /// over the pixel history the integrate wrote, its count decoded for GatherStatus::historyCount.
        VkBuffer histReadback = VK_NULL_HANDLE;
        VkDeviceMemory histReadbackMemory = VK_NULL_HANDLE;
        void *histReadbackMapped = nullptr;
        std::vector<unsigned char> histHost;
        std::vector<unsigned char> histClassHost;   ///< ...its class bit (bit 21), the same layout
        std::vector<float> histDistHost;            ///< ...its view distance (bits 0-20), the same layout
        std::vector<unsigned char> histStreakHost;  ///< ...and its validity streak (the streak pair), the same layout
        unsigned histHostFrame = 0u;
        /// THE GATHER-HITS READBACK (FAR-SUN-1): the trace's per-ray records (device, one
        /// uvec4 per ray of every uniform probe), a host ring of them and of the atlas rows the
        /// uniform probes occupy (the radiance each ray returned, after the decode's write-back),
        /// allocated on the first frame the arm asks and freed with the view; the last retired
        /// copy decoded for GatherStatus::rays.
        VkBuffer rays = VK_NULL_HANDLE;
        VkDeviceMemory raysMemory = VK_NULL_HANDLE;
        VkBuffer raysReadback = VK_NULL_HANDLE;
        VkDeviceMemory raysReadbackMemory = VK_NULL_HANDLE;
        void *raysReadbackMapped = nullptr;
        VkBuffer raysAtlasReadback = VK_NULL_HANDLE;
        VkDeviceMemory raysAtlasReadbackMemory = VK_NULL_HANDLE;
        void *raysAtlasReadbackMapped = nullptr;
        std::vector<GatherRay> raysHost;
        unsigned raysHostFrame = 0u;
        /// The bytes of one ring slot of each (the uniform probes' records - TWO uvec4 a ray, the second the hit's position
        /// and distance as floats: REDS-1, a reader must name the texel a hit answered from; their atlas rows).
        VkDeviceSize raysSlotBytes() const { return VkDeviceSize(uniformProbes) * 64u * 32u; }
        unsigned raysAtlasRows() const { return ((uniformProbes + atlasCols - 1u) / std::max(atlasCols, 1u)) * 8u; }
        VkDeviceSize raysAtlasSlotBytes() const { return VkDeviceSize(atlasW) * raysAtlasRows() * 8u; }

        struct Pending {
            uint32_t frame = 0u;
            bool live = false;
            bool irradiance = false;     ///< this slot of the readback ring was written
            bool rays = false;           ///< ...and of the gather-hits ring (FAR-SUN-1)
            bool history = false;        ///< ...and of the history-count ring (PHOTON-2 stage 0)
            unsigned gatherFrame = 0u;   ///< ...by this gather frame
            /// A HELD frame (the rest, PHOTON-GATHER-1d): only the readback was
            /// recorded — no counter copy to read back.
            bool held = false;
        };
        Pending pending[3];
        /// The four jobs' GPU times are the monitor's rows "gather.place" /
        /// "gather.trace" / "gather.filter" / "gather.integrate" (owner = the
        /// view's key; lane TEST-1) — -1 outside a capture.
        float cpuMs = -1.0f;
        /// THE FRAME BETWEEN ITS TWO HALVES (record -> finish, PHOTON-HIT-SHADE-1):
        /// what the second half needs of the first.
        bool finishPending = false;
        bool finishHold = false;
        bool finishTraced = false;
        GatherInputs finishIn;
        unsigned finishRing = 0u;
        bool finishTemporal = false;
        double finishCpuMs = 0.0;
    };

    bool makePipelines(std::string &err);
    /// The rest clock K as the host knows it (statsInto, restDemandComplete; OgreScreenProbeGather.cpp says how).
    static unsigned restKnownOf(const View &v);
    static bool everyCellHeldOf(const View &v, unsigned restN);
    bool ensureTargets(View &v, const GatherInputs &in, unsigned stride, unsigned octRes,
                       unsigned adaptiveCap, std::string &err);
    void drop(View &v);
    /// The adaptive count (and the irradiance readback) of the frames that have retired.
    void readPending(View &v);
    /// A HELD frame (PHOTON-GATHER-1d): the view has been at rest for N frames,
    /// its answer IS the rest mean, and nothing is dispatched — the irradiance
    /// is re-bound (and copied out when the readback door asks).
    void hold(View &v, const GatherInputs &in, bool temporal);
    void clearAtlas(View &v, VkCommandBuffer cmd);

    GatherHost &mHost;
    std::unordered_map<const void *, View> mViews;
    /// THE SHOT-VIEW FOLD (PHOTON-7 stage 10-3): a screenshot's shot view is destroyed right after its pixel readback,
    /// before its last traced frame's counter slot retires - `forget` folds that slot (the device finished it: the
    /// readback waited) and keeps its starved counts here, per scene, with the frame the view last recorded. statsInto
    /// reports them as THE LAST SHOT's (GatherStatus::shotStarved*, never merged into the live views' numbers), and the
    /// entry dies with the scene (forgetScene: no new scene reads a dead one's at its address).
    struct ForgottenView {
        unsigned starvedDiffuse = 0u, starvedRough = 0u;
    };
    std::unordered_map<const detail::OgreScene *, ForgottenView> mForgotten;

    VkDescriptorSetLayout mPlaceLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mTraceLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mFilterLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mIntegrateLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPlacePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mTracePipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mFilterPipeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mIntegratePipeLayout = VK_NULL_HANDLE;
    VkPipeline mPlacePipeline = VK_NULL_HANDLE;
    /// THE PLACEMENT'S SECOND PASS (PHOTON-VIEW-NOISE-1): the same source built with
    /// JAH_PLACE_SELECT, on the placement's layout — the budget's winners in a fixed order.
    VkPipelineLayout mPlaceSelectPipeLayout = VK_NULL_HANDLE;
    VkPipeline mPlaceSelectPipeline = VK_NULL_HANDLE;
    VkShaderModule mPlaceSelectModule = VK_NULL_HANDLE;
    VkPipeline mTracePipeline = VK_NULL_HANDLE;
    VkPipeline mFilterPipeline = VK_NULL_HANDLE;
    VkPipeline mIntegratePipeline = VK_NULL_HANDLE;
    VkShaderModule mPlaceModule = VK_NULL_HANDLE;
    VkShaderModule mTraceModule = VK_NULL_HANDLE;
    VkShaderModule mFilterModule = VK_NULL_HANDLE;
    VkShaderModule mIntegrateModule = VK_NULL_HANDLE;
    /// THE RESUME's FIVE JOBS (PHOTON-7 stage 8B, B1; rq_probe_resume.comp): the test, the brick pyramids, the clock,
    /// the finalize and the ring - one layout, a pipeline each.
    static constexpr unsigned kResumeJobs = 5u;
    enum ResumeJob : unsigned { kResumeTest = 0u, kResumePyramid = 1u, kResumeClock = 2u, kResumeFinal = 3u, kResumeRing = 4u };
    VkDescriptorSetLayout mResumeLayout = VK_NULL_HANDLE;
    VkPipelineLayout mResumePipeLayout[kResumeJobs] = {};
    VkShaderModule mResumeModule[kResumeJobs] = {};
    VkPipeline mResumePipeline[kResumeJobs] = {};
    VkDescriptorPool mPool = VK_NULL_HANDLE;
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
    /// ...and its largest 2D image side (the atlas' height grows with the
    /// adaptive cap; an image past it is not an allocation failure, it is a lost
    /// device — found by vr.gather_stereo's first draft, whose uncapped tuning
    /// asked for a 131,072-texel-tall atlas: Xid 69).
    uint32_t mMaxImageDim = 0u;
    bool mSaidWorkGroupClamp = false;
};

#endif   // JAH_RAY_QUERY

}   // namespace engine
}   // namespace jahshaka
