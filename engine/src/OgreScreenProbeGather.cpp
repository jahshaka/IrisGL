// THE SCREEN-PROBE GATHER — the Component's implementation (GATHER-1a,
// 2026-09-21; SPECS/SCREEN_PROBE_GATHER_SPEC.md section 9).
//
// FOUR COMPUTE DISPATCHES on the frame's own command buffer, recorded at the
// reflection listener's hook — after the SSR prepass (whose depth and normals
// are the probes' surfaces) and before the opaque pass that shades:
//
//   rq_probe_place.comp      one thread per cell: the probe's jittered pixel,
//                            its world position and normal, and the adaptive
//                            probe the cell wants where one is not enough.
//   rq_probe_gather.comp     one WORKGROUP per probe, one ray per thread, one
//                            ray per octahedral texel; dispatched INDIRECTLY
//                            because the adaptive probes' count is the GPU's.
//   rq_probe_filter.comp     one WORKGROUP per probe, one texel per thread: the
//                            map filtered against its 3x3 neighbourhood in
//                            probe space and projected onto SH9 (same indirect
//                            arguments; PHOTON-GATHER-1b).
//   rq_probe_integrate.comp  one thread per pixel: the four grid probes and the
//                            cell's adaptive twin, weighted by the plane test,
//                            each SH evaluated at the pixel's own normal, then
//                            the pixel HISTORY (PHOTON-GATHER-1c: reprojected,
//                            validated, the count-in-history mean), into
//                            `jahProbeIrradiance`.
//
// WHAT IS PING-PONGED (PHOTON-GATHER-1c): the pixel history's two pairs —
// `View::flip` names this frame's half of each.
//
// THE BOUNDARY THIS FILE KEEPS. It speaks Vulkan and Ogre and knows nothing
// about the scene graph: everything it needs about the frame arrives in
// `GatherInputs`, filled by the ray tier, which is the object that is friends
// with OgreScene. That is the whole difference between a Component and more
// code in the tier, and it is what makes phases 2 and 3 (a filter, an SH
// record, history pairs) edits to this file alone.
#include "ScreenProbeGather.h"

#ifdef JAH_RAY_QUERY

#include "EnginePrivate.h"

#include <OgreRenderSystem.h>
#include <OgreTextureGpuManager.h>
#include <OgreLogManager.h>
#include <OgreResourceTransition.h>
#include <Vao/OgreUavBufferPacked.h>

#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanDevice.h"
#include "Vao/OgreVulkanBufferInterface.h"

#include "rayquery/rq_probe_place_spv.h"
#include "rayquery/rq_probe_gather_spv.h"
#include "rayquery/rq_probe_filter_spv.h"
#include "rayquery/rq_probe_integrate_spv.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace jahshaka {
namespace engine {

namespace {

using Clock = std::chrono::steady_clock;
inline double msSince(const Clock::time_point &t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// Descriptor sets and parameter buffers in flight per view — a set a command
/// buffer still holds may not be rewritten, and this one is rewritten every
/// frame because every input can be recreated behind our back.
constexpr unsigned kRing = 3u;
/// How many views may hold a timestamp range at once.
constexpr unsigned kMaxTimedViews = 8u;
/// Frames of timestamps in flight (Ogre's dynamic-buffer multiplier).
constexpr unsigned kFramesInFlight = 3u;
/// Eight timestamps a frame: a pair around each of the four jobs.
constexpr unsigned kQueriesPerFrame = 8u;

constexpr unsigned kPlaceBindings = 6u;
/// The trace's nine, then GA-1e's six: the card read's four (9-12, the
/// reflection's jah_rq_card_bindings.glsl at base 9) and the geometric
/// normal's two (13, 14); then the split voxel store's four arrays by name
/// (15-18: the coverage and the surface position per half, PHOTON-VOXEL-4).
constexpr unsigned kTraceBindings = 19u;
constexpr unsigned kFilterBindings = 3u;
constexpr unsigned kIntegrateBindings = 10u;

/// One probe's record: ten vec4s — position, normal, the irradiance at its own
/// normal and the SH9 (27 floats in seven vec4s; see JahProbeRecord in the
/// shaders). PHOTON-GATHER-1b grew it from three vec4s.
constexpr unsigned kRecordBytes = 160u;
/// THE WEIGHT FLOOR under which a pixel's plane-weighted probes are not an
/// answer (w = 0: the fallback owns the pixel). A pixel whose ONLY agreeing probe
/// is a corner cell's at a sixteenth of the bilinear weight still clears it; one
/// whose probes all sit off its plane does not.
constexpr float kWeightFloor = 1e-3f;

/// rq_probe_*.comp's uniform block, MEMBER FOR MEMBER (jah_probe_params.glsl).
/// std140 over vec4s only, so the C++ layout is the GLSL layout by
/// construction.
struct GatherParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
    float knobs2[4] = {};
    float knobs3[4] = {};
    float plane[4] = {};
    float skyColour[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float voxelOrigin[kGatherMaxCascades][4] = {};
    float voxelInvSize[kGatherMaxCascades][4] = {};
    float knobs4[4] = {};
    float prevCamPos[4] = {};
    float prevRayTL[4] = {};
    float prevRayRight[4] = {};
    float prevRayDown[4] = {};
    float prevFwd[4] = {};
    float knobs5[4] = {};
    float cards[4] = {};
    float knobs6[4] = {};
};

/// THE PIXEL HISTORY'S BLEND FLOOR (PHOTON-GATHER-1c item 1): the smallest
/// weight a new frame takes once the running mean has that many frames in it —
/// a new frame's share is 1/n until n reaches it, then 1/kHistoryFrames. It is
/// the trade between stillness and lag, both measured by gi.gather_stable on the
/// Showroom-2-shaped room: at 10 (Lumen's own default for its screen-probe
/// history; GatherTuning::historyFrames is the A/B door) a still view steps by at most 1/255 at every sampled pixel after the
/// warm-up (9/255 with each frame alone), and a lamp switched off is within one
/// code of its settled picture 16 frames later (0 with each frame alone).
constexpr float kHistoryFrames = 10.0f;

/// The history's floor in frames as a tuning runs it (the shipped kHistoryFrames,
/// or GatherTuning::historyFrames, 1..63).
unsigned historyFramesOf(const GatherTuning &t) {
    return t.historyFrames ? std::min(std::max(t.historyFrames, 1u), 63u) : unsigned(kHistoryFrames);
}
/// N — the frames a step of kGatherSettleCodes codes takes to fall under one code
/// through the history's EMA, ceil( ln(1/D) / ln(1 - 1/h) ) (16 at h = 10), and
/// the length of the REST MEAN (rq_probe_integrate.comp): at rest the answer IS
/// the mean of N rest frames at the N-th, and the view then holds — whatever
/// else delays the scene's rest, the held picture is the same N samples.
unsigned settleFramesOf(unsigned historyFrames) {
    const double h = double(std::max(historyFrames, 2u));
    return unsigned(std::ceil(std::log(1.0 / double(kGatherSettleCodes)) / std::log(1.0 - 1.0 / h)));
}
/// THE REST FRAMES' SAMPLE SEQUENCE: frame index kRestSequenceBase + k at the
/// k-th rest frame, so the rest mean is the same set of samples whoever asks and
/// whatever came before — a function of the scene and the camera alone.
constexpr unsigned kRestSequenceBase = 0x10000u;

/// Does a tuning change CHANGE THE ESTIMATOR? Every field but the readback (a
/// test door that only copies the answer out). A changed estimator is a new
/// history: its first frame takes its own estimate whole, so an A/B across a
/// tuning push is never an average of its two arms.
bool sameEstimator(const GatherTuning &a, const GatherTuning &b) {
    return a.probeStride == b.probeStride && a.octRes == b.octRes && a.rayLength == b.rayLength &&
           a.adaptiveCap == b.adaptiveCap && a.freezeFrameIndex == b.freezeFrameIndex &&
           a.jitterOff == b.jitterOff && a.farQueryOff == b.farQueryOff &&
           a.shBands == b.shBands && a.filterOff == b.filterOff &&
           a.historyFrames == b.historyFrames &&
           a.historyValidationOff == b.historyValidationOff && a.restOff == b.restOff;
}

/// THE ADAPTIVE TEST'S TWO TOLERANCES, and why they are constants rather than
/// rows. `kPlaneTolerance` is how far a cell's pixel may lie off its probe's
/// plane before the cell wants a second probe, AS A FRACTION OF THE PIXEL'S OWN
/// VIEW DISTANCE — an angular tolerance, so one number works at a metre and at
/// fifty. 1/100 is a little under a degree of plane tilt across a cell at any
/// depth, which passes a flat floor seen at a grazing angle and fails a step or
/// a railing. `kNormalTolerance` is the dot product below which two normals are
/// not the same surface at all (about 25 degrees).
///
/// They are not tier rows because a person cannot reason about them: what a
/// person tunes is the probe STRIDE, which is the tier's own column.
constexpr float kPlaneTolerance = 0.01f;
constexpr float kNormalTolerance = 0.9f;

/// IEEE half to float, for the readback (a test door; subnormals kept).
float halfToFloat(uint16_t h) {
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0u) {
        if (mant == 0u) bits = sign;
        else {
            exp = 127u - 15u + 1u;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31u) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127u - 15u) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

}   // namespace

// ---------------------------------------------------------------------------
ScreenProbeGather::~ScreenProbeGather() { close(); }

bool ScreenProbeGather::makePipelines(std::string &err) {
    VkDevice dev = mHost.gatherDevice();
    const auto makeLayout = [&](unsigned count, const VkDescriptorType *types,
                                const unsigned *counts, VkDescriptorSetLayout &out,
                                const char *what) {
        VkDescriptorSetLayoutBinding b[16] = {};
        for (unsigned i = 0; i < count; ++i) {
            b[i].binding = i;
            b[i].descriptorType = types[i];
            b[i].descriptorCount = counts ? counts[i] : 1u;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sli{};
        sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sli.bindingCount = count;
        sli.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &sli, nullptr, &out) != VK_SUCCESS) {
            err = std::string("gather: vkCreateDescriptorSetLayout (") + what + ") failed";
            return false;
        }
        return true;
    };

    {   // rq_probe_place.comp
        const VkDescriptorType t[kPlaceBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 1 normals
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 depth
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 3 records
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 4 counter
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 5 indirect args
        };
        if (!makeLayout(kPlaceBindings, t, nullptr, mPlaceLayout, "place")) return false;
    }
    {   // rq_probe_gather.comp
        const VkDescriptorType t[kTraceBindings] = {
            VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0 tlas
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 2 records
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 3 atlas
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 4 voxelIso[]
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 5 voxelX[]
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 6 voxelY[]
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 7 voxelZ[]
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 8 sky cube
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 9 the card table
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 10 the card instance table
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 11 the card Depth layer
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 12 the card Radiance layer
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 13 the per-slot geometry row
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 14 the GPU scene's geometry rows
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 15 voxelCovP[] (RQ-COV-SLOT-1)
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 16 voxelCovN[]
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 17 voxelPosP[] (PHOTON-VOXEL-4)
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 18 voxelPosN[]
        };
        const unsigned c[kTraceBindings] = { 1u, 1u, 1u, 1u, kGatherMaxCascades,
                                             kGatherMaxCascades, kGatherMaxCascades,
                                             kGatherMaxCascades, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
                                             kGatherMaxCascades, kGatherMaxCascades,
                                             kGatherMaxCascades, kGatherMaxCascades };
        if (!makeLayout(kTraceBindings, t, c, mTraceLayout, "trace")) return false;
    }
    {   // rq_probe_filter.comp
        const VkDescriptorType t[kFilterBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 records
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 2 the raw atlas
        };
        if (!makeLayout(kFilterBindings, t, nullptr, mFilterLayout, "filter")) return false;
    }
    {   // rq_probe_integrate.comp
        const VkDescriptorType t[kIntegrateBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 records
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 normals
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 depth
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 irradiance
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 5 last frame's history
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 6 this frame's history
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 7 last frame's history geometry
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 8 this frame's history geometry
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 9 the rest mean (PHOTON-GATHER-1d)
        };
        if (!makeLayout(kIntegrateBindings, t, nullptr, mIntegrateLayout, "integrate")) return false;
    }

    const auto makeOne = [&](VkDescriptorSetLayout setLayout, const uint32_t *spv, size_t bytes,
                             VkPipelineLayout &pipeLayout, VkShaderModule &module,
                             VkPipeline &pipeline, const char *what) {
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &pipeLayout) != VK_SUCCESS) {
            err = std::string("gather: vkCreatePipelineLayout (") + what + ") failed";
            return false;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = bytes;
        smi.pCode = spv;
        if (vkCreateShaderModule(dev, &smi, nullptr, &module) != VK_SUCCESS) {
            err = std::string("gather: vkCreateShaderModule (") + what + ") failed";
            return false;
        }
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = pipeLayout;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipeline) !=
            VK_SUCCESS) {
            err = std::string("gather: vkCreateComputePipelines (") + what + ") failed";
            return false;
        }
        return true;
    };
    if (!makeOne(mPlaceLayout, krq_probePlaceSpv, sizeof(krq_probePlaceSpv), mPlacePipeLayout,
                 mPlaceModule, mPlacePipeline, "place"))
        return false;
    if (!makeOne(mTraceLayout, krq_probeGatherSpv, sizeof(krq_probeGatherSpv), mTracePipeLayout,
                 mTraceModule, mTracePipeline, "trace"))
        return false;
    if (!makeOne(mFilterLayout, krq_probeFilterSpv, sizeof(krq_probeFilterSpv), mFilterPipeLayout,
                 mFilterModule, mFilterPipeline, "filter"))
        return false;
    if (!makeOne(mIntegrateLayout, krq_probeIntegrateSpv, sizeof(krq_probeIntegrateSpv),
                 mIntegratePipeLayout, mIntegrateModule, mIntegratePipeline, "integrate"))
        return false;

    // PER VIEW AND RING SLOT, four sets: place (1 uniform, 3 storage buffers,
    // 2 sampled), trace (1 AS, 1 uniform, 5 storage buffers, 1 storage image,
    // 4 x cascades + 3 sampled), filter (1 uniform, 1 storage buffer, 1 storage
    // image), integrate (1 uniform, 1 storage buffer, 2 sampled, 6 storage
    // images).
    const unsigned groups = kMaxTimedViews * kRing;
    const unsigned sets = groups * 4u;
    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = groups;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = groups * 4u;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[2].descriptorCount = groups * 10u;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[3].descriptorCount = groups * 8u;
    VkDescriptorPoolSize sampled{};
    sampled.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampled.descriptorCount = groups * (2u + 8u * kGatherMaxCascades + 3u + 2u);
    VkDescriptorPoolSize all[5] = { sizes[0], sizes[1], sizes[2], sizes[3], sampled };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = sets;
    dpi.poolSizeCount = 5;
    dpi.pPoolSizes = all;
    if (vkCreateDescriptorPool(dev, &dpi, nullptr, &mPool) != VK_SUCCESS) {
        err = "gather: vkCreateDescriptorPool failed";
        return false;
    }

    auto *rs = static_cast<Ogre::VulkanRenderSystem *>(mHost.gatherRenderSystem());
    if (rs && rs->getVulkanDevice()) {
        mTimestampPeriod =
            rs->getVulkanDevice()->mDeviceProperties.limits.timestampPeriod * 1.0f;
        mMaxWorkGroupX =
            rs->getVulkanDevice()->mDeviceProperties.limits.maxComputeWorkGroupCount[0];
    }
    if (mTimestampPeriod > 0.0f) {
        VkQueryPoolCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = kMaxTimedViews * kFramesInFlight * kQueriesPerFrame;
        vkCreateQueryPool(dev, &qci, nullptr, &mTimestamps);
    }
    return true;
}

bool ScreenProbeGather::ensureTargets(View &v, const GatherInputs &in, unsigned stride,
                                      unsigned octRes, unsigned adaptiveCap, std::string &err) {
    if (v.targetsReady && v.w == in.width && v.h == in.height && v.stride == stride &&
        v.octRes == octRes && v.adaptiveCap == adaptiveCap)
        return true;
    // OUTSIDE ANY ENCODER FIRST (PHOTON-GATHER-1d, found by the validation
    // selftest once the gather ran by default): this runs in the scene pass's
    // pre-execute hook, and creating the full-resolution irradiance texture below
    // (TextureGpu::_transitionTo) records a layout barrier — inside the pass's open
    // render pass, VUID-vkCmdPipelineBarrier-None-07889. The host's frame command
    // buffer is taken with every encoder ended.
    if (!mHost.gatherFrameCmd()) { err = "gather: no frame command buffer"; return false; }
    drop(v);
    v.w = in.width;
    v.h = in.height;
    v.stride = stride;
    v.octRes = octRes;
    v.adaptiveCap = adaptiveCap;
    v.gridW = (in.width + stride - 1u) / stride;
    v.gridH = (in.height + stride - 1u) / stride;
    v.uniformProbes = v.gridW * v.gridH;
    if (!v.uniformProbes) { err = "gather: empty probe grid"; return false; }
    const unsigned total = v.uniformProbes + adaptiveCap;

    // THE ATLAS. One 8x8 block per probe whatever the tier's octahedral
    // resolution is: a fixed block keeps the addressing arithmetic one multiply
    // (and phase 2's filter reads neighbours by the same rule), and the padding
    // a 6x6 map leaves is 40 % of six megabytes, which is not a reason to make
    // every reader compute a variable stride.
    v.atlasCols = std::max(1u, v.gridW);
    const unsigned rows = (total + v.atlasCols - 1u) / v.atlasCols;
    v.atlasW = v.atlasCols * kGatherOctResMax;
    v.atlasH = rows * kGatherOctResMax;
    if (!mHost.gatherMakeImage(v.atlasW, v.atlasH, VK_FORMAT_R16G16B16A16_SFLOAT, v.atlas,
                               v.atlasMemory, v.atlasView, err))
        return false;
    if (!mHost.gatherMakeBuffer(VkDeviceSize(total) * kRecordBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, v.records,
                                v.recordsMemory, nullptr, err))
        return false;
    for (unsigned k = 0; k < 2u; ++k) {
        // THE PIXEL HISTORY PAIR at the target's resolution (rq_probe_integrate.comp).
        if (!mHost.gatherMakeImage(in.width, in.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   v.history[k], v.historyMemory[k], v.historyView[k], err))
            return false;
        if (!mHost.gatherMakeImage(in.width, in.height, VK_FORMAT_R32_UINT, v.historyGeom[k],
                                   v.historyGeomMemory[k], v.historyGeomView[k], err))
            return false;
    }
    // THE REST MEAN (PHOTON-GATHER-1d), one image at the target's resolution.
    if (!mHost.gatherMakeImage(in.width, in.height, VK_FORMAT_R16G16B16A16_SFLOAT, v.restMean,
                               v.restMeanMemory, v.restMeanView, err))
        return false;
    if (!mHost.gatherMakeBuffer(64u,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                false, v.counter, v.counterMemory, nullptr, err))
        return false;
    // THE INDIRECT ARGUMENTS the trace dispatches from, written by the
    // placement job's own atomic max (see its header) over a value this file
    // sets by a transfer at the head of every frame.
    if (!mHost.gatherMakeBuffer(16u,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, v.args, v.argsMemory, nullptr, err))
        return false;
    if (!mHost.gatherMakeBuffer(kFramesInFlight * 16u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                v.readback, v.readbackMemory, &v.readbackMapped, err))
        return false;

    // THE FULL-RESOLUTION IRRADIANCE. `Uav` and nothing else: a UAV that is not
    // a render target is born in VK_IMAGE_LAYOUT_GENERAL with the pin's own
    // barrier, it carries SAMPLED usage because it is a texture, and it is NOT
    // Reinterpretable — a float storage image viewed as its own format must not
    // be (PHOTON-M3's trap: the family of every RGBA16 format is the UINT one).
    Ogre::TextureGpuManager *tm = mHost.gatherRenderSystem()->getTextureGpuManager();
    static unsigned sSerial = 0u;
    Ogre::TextureGpu *t = tm->createTexture("JahProbeIrradiance/" + std::to_string(++sSerial),
                                            Ogre::GpuPageOutStrategy::Discard,
                                            Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    t->setResolution(in.width, in.height, 1u);
    t->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
    t->setNumMipmaps(1u);
    // RESIDENT FOR GOOD, never per frame — the 0071 lesson.
    t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    v.irradiance = t;

    // The atlas, the records, the irradiance target (8 bytes a pixel), the
    // pixel history's two pairs (8 + 4 bytes a pixel each) and the rest mean
    // (8 bytes a pixel).
    v.vramBytes = 1ull * v.atlasW * v.atlasH * 8ull +
                  (unsigned long long)total * kRecordBytes +
                  (unsigned long long)in.width * in.height * (8ull + 2ull * 12ull + 8ull);
    v.targetsReady = true;
    v.atlasNeedsClear = true;
    v.age = 0u;
    v.flip = 0u;
    return true;
}

void ScreenProbeGather::drop(View &v) {
    for (unsigned i = 0; i < kRing; ++i) {
        mHost.gatherRetireSet(v.placeSets[i], mPool);
        mHost.gatherRetireSet(v.traceSets[i], mPool);
        mHost.gatherRetireSet(v.filterSets[i], mPool);
        mHost.gatherRetireSet(v.integrateSets[i], mPool);
        v.placeSets[i] = v.traceSets[i] = v.filterSets[i] = v.integrateSets[i] = VK_NULL_HANDLE;
        mHost.gatherRetireBuffer(v.params[i], v.paramsMemory[i]);
        v.params[i] = VK_NULL_HANDLE;
        v.paramsMemory[i] = VK_NULL_HANDLE;
        v.paramsMapped[i] = nullptr;
        mHost.gatherRetireBuffer(v.geomRowOfSlot[i], v.geomRowOfSlotMemory[i]);
        v.geomRowOfSlot[i] = VK_NULL_HANDLE;
        v.geomRowOfSlotMemory[i] = VK_NULL_HANDLE;
        v.geomRowOfSlotMapped[i] = nullptr;
        v.geomRowOfSlotBytes[i] = 0u;
    }
    mHost.gatherRetireBuffer(v.records, v.recordsMemory);
    for (unsigned k = 0; k < 2u; ++k) {
        mHost.gatherRetireImage(v.history[k], v.historyMemory[k], v.historyView[k]);
        v.history[k] = VK_NULL_HANDLE;
        v.historyMemory[k] = VK_NULL_HANDLE;
        v.historyView[k] = VK_NULL_HANDLE;
        mHost.gatherRetireImage(v.historyGeom[k], v.historyGeomMemory[k], v.historyGeomView[k]);
        v.historyGeom[k] = VK_NULL_HANDLE;
        v.historyGeomMemory[k] = VK_NULL_HANDLE;
        v.historyGeomView[k] = VK_NULL_HANDLE;
    }
    mHost.gatherRetireImage(v.restMean, v.restMeanMemory, v.restMeanView);
    v.restMean = VK_NULL_HANDLE;
    v.restMeanMemory = VK_NULL_HANDLE;
    v.restMeanView = VK_NULL_HANDLE;
    v.restFrames = 0u;
    v.sinceRestart = 0u;
    v.age = 0u;
    mHost.gatherRetireBuffer(v.counter, v.counterMemory);
    mHost.gatherRetireBuffer(v.args, v.argsMemory);
    mHost.gatherRetireBuffer(v.readback, v.readbackMemory);
    mHost.gatherRetireBuffer(v.irrReadback, v.irrReadbackMemory);
    v.irrReadback = VK_NULL_HANDLE;
    v.irrReadbackMemory = VK_NULL_HANDLE;
    v.irrReadbackMapped = nullptr;
    v.irrHost.clear();
    v.irrHostFrame = 0u;
    v.records = v.counter = v.args = v.readback = VK_NULL_HANDLE;
    v.recordsMemory = v.counterMemory = v.argsMemory = v.readbackMemory = VK_NULL_HANDLE;
    v.readbackMapped = nullptr;
    mHost.gatherRetireImage(v.atlas, v.atlasMemory, v.atlasView);
    v.atlas = VK_NULL_HANDLE;
    v.atlasMemory = VK_NULL_HANDLE;
    v.atlasView = VK_NULL_HANDLE;
    if (v.irradiance) mHost.gatherRetireTexture(v.irradiance);
    v.irradiance = nullptr;
    if (v.hasQueryBase) mQuerySlots &= ~(uint32_t(1) << v.querySlot);
    v.hasQueryBase = false;
    v.targetsReady = false;
    v.atlasNeedsClear = false;
    // ...AND THE IN-FLIGHT RECORDS GO WITH THE BUFFERS THEY NAME. A resize frees
    // the readback ring and the timestamp slot; leaving `pending[]` live would
    // make the next `readPending` read a slot of a buffer that no longer exists
    // (and a query range this view no longer owns). The frame counter goes back
    // to zero for the same reason: it indexes both rings.
    for (unsigned i = 0; i < kFramesInFlight; ++i) v.pending[i] = View::Pending();
    v.frame = 0u;
    v.adaptiveLast = 0u;
    v.adaptiveAsked = 0u;
    v.placeMs = v.traceMs = v.filterMs = v.integrateMs = v.cpuMs = -1.0f;
}

void ScreenProbeGather::readPending(View &v) {
    const uint32_t nowAll = mHost.gatherFrameNow(), inFlightAll = mHost.gatherFramesInFlight();
    // THE ADAPTIVE COUNT first, because it is read even on a device with no
    // timestamp support: it is a COUNT, not a measurement, and `giStatus()`
    // reports it beside the probe grid.
    if (v.readbackMapped) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) {
            if (!v.pending[i].live || uint32_t(nowAll - v.pending[i].frame) < inFlightAll) continue;
            if (!v.pending[i].held) {
            uint32_t appended = 0u;
            std::memcpy(&appended, static_cast<const char *>(v.readbackMapped) + i * 16u,
                        sizeof(appended));
            // The shader's counter keeps counting past the cap (an atomicAdd
            // cannot un-count), so what a frame actually APPENDED is the
            // smaller of the two.
            v.adaptiveAsked = appended;
            v.adaptiveLast = std::min(appended, v.adaptiveCap);
            }
            // ...AND THE IRRADIANCE READBACK of the same retired frame, decoded
            // from half floats (a test door; see GatherTuning::readback).
            if (v.pending[i].irradiance && v.irrReadbackMapped) {
                const size_t texels = size_t(v.w) * v.h;
                const uint16_t *src = reinterpret_cast<const uint16_t *>(
                    static_cast<const char *>(v.irrReadbackMapped) + i * texels * 8u);
                v.irrHost.resize(texels * 4u);
                for (size_t k = 0; k < texels * 4u; ++k) v.irrHost[k] = halfToFloat(src[k]);
                v.irrHostFrame = v.pending[i].gatherFrame;
                v.pending[i].irradiance = false;
            }
        }
    }
    if (!mTimestamps || !v.hasQueryBase) {
        if (v.readbackMapped)
            for (unsigned i = 0; i < kFramesInFlight; ++i)
                if (v.pending[i].live && uint32_t(nowAll - v.pending[i].frame) >= inFlightAll)
                    v.pending[i].live = false;
        return;
    }
    const uint32_t now = mHost.gatherFrameNow(), inFlight = mHost.gatherFramesInFlight();
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        View::Pending &pd = v.pending[i];
        // `< inFlight`, not `<=`: the ring is kFramesInFlight deep and a slot is
        // REUSED after that many frames (the reflect path's lesson).
        if (!pd.live || uint32_t(now - pd.frame) < inFlight) continue;
        if (pd.held) { pd.live = false; continue; }   // a held frame wrote no timestamps
        uint64_t q[kQueriesPerFrame * 2] = {};
        const uint32_t base = v.queryBase + i * kQueriesPerFrame;
        if (vkGetQueryPoolResults(mHost.gatherDevice(), mTimestamps, base, kQueriesPerFrame,
                                  sizeof(q), q, sizeof(uint64_t) * 2u,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) ==
            VK_SUCCESS) {
            const auto span = [&](unsigned a, unsigned b, float &out) {
                if (q[a * 2 + 1] && q[b * 2 + 1] && q[b * 2] >= q[a * 2])
                    out = float(double(q[b * 2] - q[a * 2]) * double(mTimestampPeriod) * 1e-6);
            };
            span(0, 1, v.placeMs);
            span(2, 3, v.traceMs);
            span(4, 5, v.filterMs);
            span(6, 7, v.integrateMs);
        }
        pd.live = false;
    }
}

void ScreenProbeGather::clearAtlas(View &v, VkCommandBuffer cmd) {
    if (!v.atlasNeedsClear) return;
    v.atlasNeedsClear = false;
    // UNDEFINED -> GENERAL and zeroed. A probe block of zero radiance at a
    // distance of zero is what an untraced probe reads as, which is what the
    // integrate hands back to the pixel path.
    // Every image: the atlas and both history pairs (a zero history word is "no
    // surface", which every distance test rejects — though nothing reads them
    // before the view's age says so).
    // ...and the rest mean (PHOTON-GATHER-1d), which the integrate binds on every
    // frame and writes only at rest.
    const VkImage images[6] = { v.atlas, v.history[0], v.history[1], v.historyGeom[0],
                                v.historyGeom[1], v.restMean };
    VkImageMemoryBarrier b[6] = {};
    for (int i = 0; i < 6; ++i) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = images[i];
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = 1;
        b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 6, b);
    VkClearColorValue zero{};
    for (int i = 0; i < 6; ++i)
        vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &b[i].subresourceRange);
    VkMemoryBarrier toCompute{};
    toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toCompute, 0, nullptr, 0, nullptr);
}

void ScreenProbeGather::releaseBinding(const void *key) {
    auto it = mViews.find(key);
    if (it == mViews.end() || !it->second.sceneMgr) return;
    detail::FogHlmsListener::setProbeGather(it->second.sceneMgr, nullptr);
}

void ScreenProbeGather::forget(const void *key) {
    auto it = mViews.find(key);
    if (it == mViews.end()) return;
    // ...AND THE SHADER'S REGISTRATION WITH IT: this view's irradiance texture
    // is about to be retired, and the Hlms listener holds a raw pointer to it
    // keyed by the scene's manager (GATHER-0's D1).
    if (it->second.sceneMgr) detail::FogHlmsListener::setProbeGather(it->second.sceneMgr, nullptr);
    drop(it->second);
    mViews.erase(it);
}

void ScreenProbeGather::close() {
    VkDevice dev = mHost.gatherDevice();
    for (auto &kv : mViews) {
        if (kv.second.sceneMgr)
            detail::FogHlmsListener::setProbeGather(kv.second.sceneMgr, nullptr);
        // NOT `drop()` HERE, AND THE DIFFERENCE IS A USE-AFTER-FREE (the lead's
        // read). `drop` RETIRES a view's descriptor sets into the host's bin,
        // which frees them later with `vkFreeDescriptorSets(pool, …)` — and the
        // pool is destroyed four lines below, so the flush at the end of the
        // tier's own close() would free sets out of a pool that no longer
        // exists. At close the device is idle and the retire window is not
        // needed for anything: `vkDestroyDescriptorPool` frees every set it
        // ever handed out, so the sets are simply DROPPED here and the buffers,
        // images and textures — which the pool does not own — take the bin as
        // usual.
        View &v = kv.second;
        for (unsigned i = 0; i < kRing; ++i)
            v.placeSets[i] = v.traceSets[i] = v.filterSets[i] = v.integrateSets[i] = VK_NULL_HANDLE;
        drop(v);
    }
    mViews.clear();
    // Belt and braces: a registration keyed by a manager whose view never came
    // back through forget() would outlive every texture this object owned.
    detail::FogHlmsListener::clearProbeGather();
    if (!dev) return;
    if (mPool) vkDestroyDescriptorPool(dev, mPool, nullptr);
    if (mPlacePipeline) vkDestroyPipeline(dev, mPlacePipeline, nullptr);
    if (mTracePipeline) vkDestroyPipeline(dev, mTracePipeline, nullptr);
    if (mFilterPipeline) vkDestroyPipeline(dev, mFilterPipeline, nullptr);
    if (mIntegratePipeline) vkDestroyPipeline(dev, mIntegratePipeline, nullptr);
    if (mPlaceModule) vkDestroyShaderModule(dev, mPlaceModule, nullptr);
    if (mTraceModule) vkDestroyShaderModule(dev, mTraceModule, nullptr);
    if (mFilterModule) vkDestroyShaderModule(dev, mFilterModule, nullptr);
    if (mIntegrateModule) vkDestroyShaderModule(dev, mIntegrateModule, nullptr);
    if (mPlacePipeLayout) vkDestroyPipelineLayout(dev, mPlacePipeLayout, nullptr);
    if (mTracePipeLayout) vkDestroyPipelineLayout(dev, mTracePipeLayout, nullptr);
    if (mFilterPipeLayout) vkDestroyPipelineLayout(dev, mFilterPipeLayout, nullptr);
    if (mIntegratePipeLayout) vkDestroyPipelineLayout(dev, mIntegratePipeLayout, nullptr);
    if (mPlaceLayout) vkDestroyDescriptorSetLayout(dev, mPlaceLayout, nullptr);
    if (mTraceLayout) vkDestroyDescriptorSetLayout(dev, mTraceLayout, nullptr);
    if (mFilterLayout) vkDestroyDescriptorSetLayout(dev, mFilterLayout, nullptr);
    if (mIntegrateLayout) vkDestroyDescriptorSetLayout(dev, mIntegrateLayout, nullptr);
    if (mTimestamps) vkDestroyQueryPool(dev, mTimestamps, nullptr);
    mPool = VK_NULL_HANDLE;
    mPlacePipeline = mTracePipeline = mFilterPipeline = mIntegratePipeline = VK_NULL_HANDLE;
    mPlaceModule = mTraceModule = mFilterModule = mIntegrateModule = VK_NULL_HANDLE;
    mPlacePipeLayout = mTracePipeLayout = mFilterPipeLayout = mIntegratePipeLayout = VK_NULL_HANDLE;
    mPlaceLayout = mTraceLayout = mFilterLayout = mIntegrateLayout = VK_NULL_HANDLE;
    mTimestamps = VK_NULL_HANDLE;
    mQuerySlots = 0u;
}

void ScreenProbeGather::statsInto(const detail::OgreScene *scene, unsigned long long restKey,
                                  unsigned long long restartKey, GatherStatus &out) const {
    out.error = mLastError;
    // THE VIEWS THAT DREW THE LATEST FRAME of this scene. A scene can hold
    // several (the viewport and a screenshot's shot view, which a screenshot's
    // settle loop draws ALONE while the viewport is quiet): the numbers are the
    // latest view's, and the settled history is every latest view's — a young
    // shot view is not settled because the quiet viewport was.
    const View *latest = nullptr;
    for (const auto &kv : mViews) {
        const View &v = kv.second;
        if (v.scene != scene || !v.targetsReady) continue;
        if (!latest || int32_t(v.recordedFrame - latest->recordedFrame) > 0) latest = &v;
    }
    if (!latest) return;
    const View &v = *latest;
    out.running = true;
    out.stride = v.stride;
    out.octRes = v.octRes;
    out.raysPerProbe = v.octRes * v.octRes;
    out.probesX = v.gridW;
    out.probesY = v.gridH;
    out.probes = v.uniformProbes;
    out.adaptive = v.adaptiveLast;
    out.adaptiveRequested = v.adaptiveAsked;
    out.adaptiveCap = v.adaptiveCap;
    out.raysPerFrame = (unsigned long long)(v.uniformProbes + v.adaptiveLast) * out.raysPerProbe;
    out.targetW = v.w;
    out.targetH = v.h;
    out.atlasBytes = v.vramBytes;
    out.placeMs = v.placeMs;
    out.traceMs = v.traceMs;
    out.filterMs = v.filterMs;
    out.integrateMs = v.integrateMs;
    out.cpuMs = v.cpuMs;
    out.temporal = v.lastTemporal;
    out.historyAge = v.age;
    out.irradiance = v.irrHost;
    out.irradianceW = v.irrHost.empty() ? 0u : v.w;
    out.irradianceH = v.irrHost.empty() ? 0u : v.h;
    out.irradianceFrame = v.irrHostFrame;
    // THE SETTLED HISTORY (GatherStatus says what and why): every latest view's
    // history N frames past its last RESTART. The rest (and its hold) is
    // reported beside it and is NOT part of the predicate: a scene with a
    // per-frame writer never rests, and its shot takes the settled EMA picture.
    // A restart key the view has not drawn yet (a light write between two
    // frames) is a restart at frame 0.
    out.settled = true;
    out.restFrames = ~0u;
    out.sinceRestart = ~0u;
    for (const auto &kv : mViews) {
        const View &o = kv.second;
        if (o.scene != scene || !o.targetsReady || o.recordedFrame != v.recordedFrame) continue;
        const unsigned n = settleFramesOf(o.historyFramesLast);
        out.settleFrames = std::max(out.settleFrames, n);
        const unsigned rest = o.restKey == restKey ? o.restFrames : 0u;
        out.restFrames = std::min(out.restFrames, rest);
        const unsigned since = o.restartKey == restartKey ? o.sinceRestart : 0u;
        out.sinceRestart = std::min(out.sinceRestart, since);
        if (o.lastTemporal && since < n) out.settled = false;
    }
    if (out.restFrames == ~0u) out.restFrames = 0u;
    if (out.sinceRestart == ~0u) out.sinceRestart = 0u;
}

// ---------------------------------------------------------------------------
// A HELD FRAME (PHOTON-GATHER-1d). The view has been at rest for N frames: its
// irradiance texture holds the rest mean (rq_probe_integrate.comp) and stays
// exactly that until something moves. Nothing is dispatched; the texture is bound
// to the pass again, and the readback door still copies it out (a suite averaging
// a still scene's frames reads the held answer, not a stall).
void ScreenProbeGather::hold(View &v, const GatherInputs &in, bool temporal) {
    Ogre::RenderSystem *rs = mHost.gatherRenderSystem();
    if (in.tuning.readback && v.irrReadback) {
        {
            Ogre::BarrierSolver &solver = rs->getBarrierSolver();
            Ogre::ResourceTransitionArray trans;
            solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Uav,
                                     Ogre::ResourceAccess::Read, 1u << Ogre::GPT_COMPUTE_PROGRAM);
            rs->executeResourceTransition(trans);
        }
        VkCommandBuffer cmd = mHost.gatherFrameCmd();
        if (cmd) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
            const VkDeviceSize slotBytes = VkDeviceSize(v.w) * v.h * 8u;
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(v.frame % kFramesInFlight) * slotBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { v.w, v.h, 1u };
            vkCmdCopyImageToBuffer(
                cmd, static_cast<Ogre::VulkanTextureGpu *>(v.irradiance)->getFinalTextureName(),
                VK_IMAGE_LAYOUT_GENERAL, v.irrReadback, 1, &region);
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &toHost, 0, nullptr, 0, nullptr);
            View::Pending &pd = v.pending[v.frame % kFramesInFlight];
            pd.frame = mHost.gatherFrameNow();
            pd.live = true;
            pd.irradiance = true;
            pd.held = true;
            pd.gatherFrame = v.frame;
            ++v.frame;
        }
    }
    {
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    detail::FogHlmsListener::setProbeGather(in.sceneMgr, v.irradiance);
    ++v.age;
    v.lastTemporal = temporal;
    v.recordedFrame = mHost.gatherFrameNow();
}

void ScreenProbeGather::record(const void *key, const GatherInputs &in) {
    if (mFailed || !in.scene || !in.sceneMgr || !in.tlas || !in.normals || !in.depth) return;
    if (!in.width || !in.height) return;
    const auto cpuStart = Clock::now();

    if (!mPlacePipeline) {
        std::string err;
        if (!makePipelines(err)) {
            mFailed = true;
            mLastError = err;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: the screen-probe gather is off on this device — " + err);
            return;
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the SCREEN-PROBE GATHER is on — the diffuse GI of every covered pixel "
            "is estimated from probe rays (SPECS/SCREEN_PROBE_GATHER_SPEC.md)");
    }

    // ---- WHAT THE TIER ASKS FOR: the tier table's gather row ----------------
    // (Types.h `GiGatherFacts`, handed over in `in.facts`): 16 pixels a probe at
    // Medium and High, 8 at Epic; 64 rays each, 36 at Medium. The ray budget is
    // NOT what decides these numbers — GATHER-0 measured the trace at 0.21 ns a
    // ray and found the 8,160-probe arm FASTER than a 2,040-probe one at the same
    // ray count (probe count is what fills this GPU) — the picture and the VRAM
    // are. Epic is the TABLE's row, never the view's SSR row (GA-TIERROW).
    unsigned stride = in.facts.stride;
    unsigned octRes = in.facts.octRes;
    if (in.tuning.probeStride) stride = in.tuning.probeStride;
    if (in.tuning.octRes) octRes = in.tuning.octRes;
    stride = std::min(std::max(stride, 2u), 64u);
    octRes = std::min(std::max(octRes, 1u), kGatherOctResMax);

    View &v = mViews[key];
    // A SCENE BIND IS A NEW VIEW for the histories: nothing in them is this
    // scene's.
    if (v.scene != in.scene) v.age = 0u;
    v.scene = in.scene;
    v.sceneMgr = in.sceneMgr;
    readPending(v);

    const unsigned gridW = (in.width + stride - 1u) / stride;
    const unsigned gridH = (in.height + stride - 1u) / stride;
    // HOW MANY ADAPTIVE PROBES A FRAME MAY ADD: the table's share of the grid (a
    // quarter). A CAP rather than a target — a flat scene spends none of it. The
    // cap is part of the allocation (the atlas and the record buffer are sized
    // for the worst case, resident, never per frame — the 0071 lesson), so it
    // cannot be a per-frame decision.
    unsigned adaptiveCap = gridW * gridH / std::max(in.facts.adaptiveCapDivisor, 1u);
    if (in.tuning.adaptiveCap >= 0) adaptiveCap = unsigned(in.tuning.adaptiveCap);

    std::string err;
    if (!ensureTargets(v, in, stride, octRes, adaptiveCap, err)) {
        // NOT A PROCESS-WIDE LATCH (the lead's read). A failure to allocate this
        // view's targets is about THIS view at THIS size — a resize, a second
        // view, a moment of VRAM pressure — and latching `mFailed` turned it
        // into "no scene gathers again until the app restarts", with no reason
        // anywhere a caller could read. The reason is published, the view is
        // dropped, and the next frame that asks for a different size may try
        // again. Only the PIPELINES (below) latch, because a device that cannot
        // compile them this minute cannot compile them next minute either.
        mLastError = err;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the screen-probe gather could not allocate its targets — " + err);
        drop(v);
        return;
    }
    mLastError.clear();

    // ---- THE PIXEL HISTORY'S INPUTS (PHOTON-GATHER-1c) -----------------------
    // THE MEASUREMENT LEVER, read at every frame so a suite drives both arms in
    // one process: each frame's estimate alone, no history read or written.
    const bool temporal = std::getenv("JAHSHAKA_GATHER_NO_TEMPORAL") == nullptr;
    // The history restarts when it would otherwise average two estimators: the
    // history switched back on (its images were not written meanwhile), or a
    // tuning field that changes the estimator (an A/B arm).
    if (temporal && !v.temporalLast) v.age = 0u;
    if (!sameEstimator(in.tuning, v.tuningLast)) v.age = 0u;
    v.temporalLast = temporal;
    v.tuningLast = in.tuning;
    if (v.age == 0u) {
        // Nothing reads it at age 0; this frame's basis keeps the block finite.
        const auto put3w = [](float dst[4], const float src[3], float w) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = w;
        };
        std::memcpy(v.prevCamPos, in.camPos, sizeof(v.prevCamPos));
        put3w(v.prevRayTL, in.rayTL, 0.0f);
        put3w(v.prevRayRight, in.rayRight, 0.0f);
        put3w(v.prevRayDown, in.rayDown, 0.0f);
        put3w(v.prevFwd, in.fwd, 0.0f);
    }
    // ---- THE REST (PHOTON-GATHER-1d) --------------------------------------------
    // A frame is a REST frame when nothing the answer depends on moved since the
    // last one: the camera's basis (bit for bit — a tracked VR head is never at
    // rest, and a still editor camera always is), the scene's rest key (the
    // lighting, the geometry, the surface cache: OgreScene::gatherRestKey) and the
    // estimator (an age of 0 is a restart). At rest the integrate hands the answer
    // over to a true mean of the rest frames (rq_probe_integrate.comp); after N of
    // them the answer IS that mean and the view HOLDS: nothing is dispatched.
    {
        const bool sameCamera =
            v.age > 0u && std::memcmp(v.prevCamPos, in.camPos, sizeof(v.prevCamPos)) == 0 &&
            std::memcmp(v.prevRayTL, in.rayTL, sizeof(in.rayTL)) == 0 &&
            std::memcmp(v.prevRayRight, in.rayRight, sizeof(in.rayRight)) == 0 &&
            std::memcmp(v.prevRayDown, in.rayDown, sizeof(in.rayDown)) == 0 &&
            std::memcmp(v.prevFwd, in.fwd, sizeof(in.fwd)) == 0;
        const bool still = temporal && !in.tuning.restOff && sameCamera && in.restKey == v.restKey;
        v.restKey = in.restKey;
        // THE SETTLE: a restart zeroes it (the view's birth or an estimator change
        // is age 0; the scene's restart key moving is the rest), any other frame
        // — moving camera, moving geometry, held — counts one.
        const bool restart = v.age == 0u || in.restartKey != v.restartKey;
        v.restartKey = in.restartKey;
        v.sinceRestart = restart ? 0u : std::min(v.sinceRestart + 1u, 1u << 20);
        v.restFrames = still ? std::min(v.restFrames + 1u, 1u << 20) : 0u;
        v.historyFramesLast = historyFramesOf(in.tuning);
        if (still && v.restFrames > settleFramesOf(v.historyFramesLast)) {
            hold(v, in, temporal);
            v.cpuMs = float(msSince(cpuStart));
            return;
        }
    }
    const unsigned cur = v.flip & 1u, prev = cur ^ 1u;   // the history pair's halves

    const unsigned ring = v.frame % kRing;
    if (!v.params[ring] &&
        !mHost.gatherMakeBuffer(sizeof(GatherParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                                v.params[ring], v.paramsMemory[ring], &v.paramsMapped[ring], err))
        return;
    const auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet &set) {
        if (set) return true;
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(mHost.gatherDevice(), &dai, &set) != VK_SUCCESS) {
            set = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };
    if (!allocSet(mPlaceLayout, v.placeSets[ring]) || !allocSet(mTraceLayout, v.traceSets[ring]) ||
        !allocSet(mFilterLayout, v.filterSets[ring]) ||
        !allocSet(mIntegrateLayout, v.integrateSets[ring]))
        return;
    if (!v.hasQueryBase && mTimestamps) {
        for (unsigned s = 0; s < kMaxTimedViews; ++s) {
            if (mQuerySlots & (uint32_t(1) << s)) continue;
            mQuerySlots |= uint32_t(1) << s;
            v.querySlot = s;
            v.queryBase = s * kFramesInFlight * kQueriesPerFrame;
            v.hasQueryBase = true;
            break;
        }
    }

    // ---- THE PARAMETERS ----------------------------------------------------
    GatherParams pp{};
    std::memcpy(pp.camPos, in.camPos, sizeof(pp.camPos));
    const auto put3 = [](float dst[4], const float src[3], float w) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = w;
    };
    put3(pp.rayTL, in.rayTL, 0.0f);
    put3(pp.rayRight, in.rayRight, 0.0f);
    put3(pp.rayDown, in.rayDown, 0.0f);
    put3(pp.fwd, in.fwd, 0.0f);
    put3(pp.viewAxisX, in.viewAxisX, 0.0f);
    put3(pp.viewAxisY, in.viewAxisY, 0.0f);
    put3(pp.viewAxisZ, in.viewAxisZ, 0.0f);
    pp.projParams[0] = in.projA;
    pp.projParams[1] = in.projB;
    pp.projParams[2] = in.farClip;
    pp.resolution[0] = float(v.gridW);
    pp.resolution[1] = float(v.gridH);
    pp.resolution[2] = float(in.width);
    pp.resolution[3] = float(in.height);
    pp.knobs[0] = float(stride);
    // THE RAY'S LENGTH = THE LIT VOLUME'S INSCRIBED RADIUS: half the outer
    // cascade's extent (PHOTON-GAFAR-1, measured; spikes/photon-gafar-1). A hit
    // closer than that to the volume's centre is one the cascades can shade; a
    // hit beyond the outer box is one they cannot, which the trace draws BLACK,
    // and a miss is the sky. The sweep that decided it (the shader's own ray set
    // on the CPU against the same TLAS, frozen-frame pictures against the old
    // full-DIAGONAL length, open sky / showroom / sealed box x Medium / High /
    // Epic, outer half extent 60 m at every tier):
    //   * hits the half extent loses: 0.011-0.017 % of rays in the open sky,
    //     0 in the showroom (max hit 54 m) and the box (14 m);
    //   * the picture: 0-487 of 230,400 px move, by 1-2/255 (the bar is 1 %);
    //   * the cost: none measurable -- the gather's GPU time at 60, 104 and
    //     208 m agrees within +-1 % in interleaved rounds (a traversal through
    //     empty space is cheap), so the length is set by what a ray can use,
    //     not by what it costs.
    // The old length was the DIAGONAL. What lies BEYOND this length is not the
    // near trace's business: an escaping ray traces the far copies (the coarse
    // levels) from here to the far plane (knobs3.w below, ATOM-FARBLAS-1). The
    // floor for a chain with no cascades and the camera's far plane as the
    // ceiling are kept.
    {
        float reach = 0.0f;
        if (in.cascadeCount) {
            const float *s = in.voxelSize[in.cascadeCount - 1u];
            reach = 0.5f * std::min(s[0], std::min(s[1], s[2]));
        }
        const float derived = std::min(in.farClip > 0.0f ? in.farClip : 1000.0f,
                                       reach > 0.0f ? reach : 50.0f);
        pp.knobs[1] = in.tuning.rayLength > 0.0f ? in.tuning.rayLength : derived;
    }
    // THE SAMPLE SEQUENCE'S ONLY INPUT, and the determinism arm that holds it.
    // ...and AT REST the rest frame's own index (kRestSequenceBase + k), so the
    // rest mean is the same samples whatever came before it (PHOTON-GATHER-1d).
    pp.knobs[2] = in.tuning.freezeFrameIndex
                      ? 0.0f
                      : float(v.restFrames ? kRestSequenceBase + v.restFrames : (v.frame & 0xFFFFu));
    pp.knobs[3] = float(in.cascadeCount);
    pp.knobs2[0] = in.anisotropic ? 1.0f : 0.0f;
    // THE RAY'S START, OFF THE SURFACE BY AN EPSILON — never by half a voxel
    // (the fix round, gi.gather_cards' near-foot chase): the probe measures the
    // irradiance AT the surface, and a start lifted 4 cm (0.5 x cascade 0's
    // cell) measured it 4 cm up — the floor in front of a floating panel read
    // +7 % at its foot and -11 % at 1.6 m, the sign and size of that lift
    // exactly (spikes/photon-gather-1d). The floor here, 1 mm, and the shader's
    // 1e-4 of the view distance (well above a float depth's reconstruction
    // error) keep a ray off its own triangle; a hit's shading owns its own
    // footprint.
    pp.knobs2[1] = 0.001f;
    pp.knobs2[2] = in.sky ? 1.0f : 0.0f;
    pp.knobs2[3] = float(octRes);
    pp.knobs3[0] = float(v.uniformProbes);
    pp.knobs3[1] = float(adaptiveCap);
    pp.knobs3[2] = float(v.atlasCols);
    // THE FAR QUERY (ATOM-FARBLAS-1, A5b section 4): an escaping ray traces the
    // FAR copies (each mesh's coarsest level, mask kRayMaskFar) from the near
    // length out to the camera's far plane. Off under the tuning's A/B, and off
    // by construction when the far plane is not beyond the near length.
    {
        const float farPlane = in.farClip > 0.0f ? in.farClip : 1000.0f;
        pp.knobs3[3] = (!in.tuning.farQueryOff && farPlane > pp.knobs[1]) ? farPlane : 0.0f;
        // ...and it STARTS `farOverlap` before the near length (audit F2): a
        // coarse surface just inside the near length whose fine surface lies
        // just outside it would otherwise be passed by both queries — the ray
        // would see through the object. A coarse hit in that overlap is honest:
        // the near query has already proved no fine surface lies inside it.
        pp.plane[2] = std::max(pp.knobs2[1], pp.knobs[1] - std::max(0.0f, in.farOverlap));
    }
    pp.plane[0] = kPlaneTolerance;
    pp.plane[1] = kNormalTolerance;
    pp.plane[3] = in.tuning.jitterOff ? 1.0f : 0.0f;
    for (int i = 0; i < 3; ++i) pp.skyColour[i] = std::max(0.0f, in.skyColour[i]);
    for (unsigned c = 0; c < kGatherMaxCascades; ++c) {
        const unsigned src = c < in.cascadeCount ? c : (in.cascadeCount ? in.cascadeCount - 1u : 0u);
        const bool have = in.cascadeCount != 0u;
        const float sx = have ? in.voxelSize[src][0] : 1.0f;
        const float sy = have ? in.voxelSize[src][1] : 1.0f;
        const float sz = have ? in.voxelSize[src][2] : 1.0f;
        pp.voxelOrigin[c][0] = have ? in.voxelOrigin[src][0] : 0.0f;
        pp.voxelOrigin[c][1] = have ? in.voxelOrigin[src][1] : 0.0f;
        pp.voxelOrigin[c][2] = have ? in.voxelOrigin[src][2] : 0.0f;
        pp.voxelOrigin[c][3] = have ? in.voxelMultiplier[src] : 1.0f;
        pp.voxelInvSize[c][0] = sx > 0.0f ? 1.0f / sx : 0.0f;
        pp.voxelInvSize[c][1] = sy > 0.0f ? 1.0f / sy : 0.0f;
        pp.voxelInvSize[c][2] = sz > 0.0f ? 1.0f / sz : 0.0f;
        pp.voxelInvSize[c][3] = have ? in.voxelCell[src] : 1.0f;
    }
    // PHOTON-GATHER-1b: the SH bands the integrate evaluates (9 unless the
    // measurement arm asks for 4), the weight floor, the filter on or off.
    pp.knobs4[0] = in.tuning.shBands == 4u ? 4.0f : 9.0f;
    pp.knobs4[1] = kWeightFloor;
    pp.knobs4[2] = in.tuning.filterOff ? 0.0f : 1.0f;
    // PHOTON-GATHER-1c: the previous camera, the view's age, the history's floor,
    // the lever.
    std::memcpy(pp.prevCamPos, v.prevCamPos, sizeof(pp.prevCamPos));
    std::memcpy(pp.prevRayTL, v.prevRayTL, sizeof(pp.prevRayTL));
    std::memcpy(pp.prevRayRight, v.prevRayRight, sizeof(pp.prevRayRight));
    std::memcpy(pp.prevRayDown, v.prevRayDown, sizeof(pp.prevRayDown));
    std::memcpy(pp.prevFwd, v.prevFwd, sizeof(pp.prevFwd));
    pp.knobs5[0] = float(std::min(v.age, 65535u));
    // The floor: the shipped kHistoryFrames, or the tuning's A/B arm.
    pp.knobs5[1] = 1.0f / float(v.historyFramesLast);
    // THE REST MEAN (PHOTON-GATHER-1d): the rest frame k and N.
    pp.knobs6[0] = temporal ? float(v.restFrames) : 0.0f;
    pp.knobs6[1] = float(settleFramesOf(v.historyFramesLast));
    pp.knobs5[2] = temporal ? 1.0f : 0.0f;
    pp.knobs5[3] = in.tuning.historyValidationOff ? 1.0f : 0.0f;
    // PHOTON-GATHER-1d (GA-1e): the surface cache the hits read first, and the
    // per-slot geometry rows the card pick faces by — the per-slot table copied
    // into this ring slot's buffer (the scene's vector moves under a later
    // frame). No cache or no rows: zero slots, and the shader never indexes the
    // stand-ins bound in their place.
    uint32_t geomSlots = 0u;
    if (in.geomRows && in.geomRowOfSlot && !in.geomRowOfSlot->empty()) {
        const VkDeviceSize want = VkDeviceSize(in.geomRowOfSlot->size()) * sizeof(uint32_t);
        if (v.geomRowOfSlot[ring] && v.geomRowOfSlotBytes[ring] < want) {
            mHost.gatherRetireBuffer(v.geomRowOfSlot[ring], v.geomRowOfSlotMemory[ring]);
            v.geomRowOfSlot[ring] = VK_NULL_HANDLE;
            v.geomRowOfSlotMemory[ring] = VK_NULL_HANDLE;
            v.geomRowOfSlotMapped[ring] = nullptr;
            v.geomRowOfSlotBytes[ring] = 0u;
        }
        if (!v.geomRowOfSlot[ring]) {
            const VkDeviceSize bytes = std::max<VkDeviceSize>(want, 1024u);
            std::string gerr;
            if (mHost.gatherMakeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                       v.geomRowOfSlot[ring], v.geomRowOfSlotMemory[ring],
                                       &v.geomRowOfSlotMapped[ring], gerr))
                v.geomRowOfSlotBytes[ring] = bytes;
        }
        if (v.geomRowOfSlot[ring] && v.geomRowOfSlotMapped[ring]) {
            std::memcpy(v.geomRowOfSlotMapped[ring], in.geomRowOfSlot->data(), size_t(want));
            geomSlots = uint32_t(in.geomRowOfSlot->size());
        }
    }
    const bool cardsBound = in.cardTable && in.cardInstances && in.cardDepth && in.cardRadiance &&
                            in.cardRecords > 0u;
    pp.cards[0] = cardsBound ? float(in.cardSlots) : 0.0f;
    pp.cards[1] = cardsBound ? float(in.cardRecords) : 0.0f;
    pp.cards[2] = in.cardFootprintTexels;
    pp.cards[3] = float(geomSlots);
    std::memcpy(v.paramsMapped[ring], &pp, sizeof(pp));

    // ---- THE DESCRIPTOR SETS, rewritten every frame ------------------------
    // Uncached views, retired: the reflection path's measured trap (a cached
    // view of a texture recreated at the same address is a view of a dead
    // image) applies here word for word.
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView view = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        mHost.gatherRetireView(view);
        return view;
    };
    VkImageView dummyCube = VK_NULL_HANDLE, dummyVolume = VK_NULL_HANDLE,
                dummyFlat = VK_NULL_HANDLE;
    VkBuffer dummyStorage = VK_NULL_HANDLE;
    if (!mHost.gatherDummies(dummyCube, dummyVolume, dummyFlat, dummyStorage, err)) return;

    VkDescriptorBufferInfo paramsInfo{};
    paramsInfo.buffer = v.params[ring];
    paramsInfo.range = sizeof(GatherParams);
    VkDescriptorBufferInfo recordsInfo{};
    recordsInfo.buffer = v.records;
    recordsInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo counterInfo{};
    counterInfo.buffer = v.counter;
    counterInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo argsInfo{};
    argsInfo.buffer = v.args;
    argsInfo.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo normals{}, depth{};
    normals.sampler = mHost.gatherPointSampler();
    normals.imageView = sampledView(in.normals);
    normals.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth.sampler = mHost.gatherPointSampler();
    depth.imageView = sampledView(in.depth);
    depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!normals.imageView || !depth.imageView) return;

    {   // the placement set
        VkWriteDescriptorSet w[kPlaceBindings] = {};
        for (unsigned i = 0; i < kPlaceBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.placeSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &normals;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &depth;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &recordsInfo;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[4].pBufferInfo = &counterInfo;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[5].pBufferInfo = &argsInfo;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kPlaceBindings, w, 0, nullptr);
    }

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &in.tlas;
    VkDescriptorImageInfo atlasStore{};
    atlasStore.imageView = v.atlasView;
    atlasStore.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo volumes[8][kGatherMaxCascades] = {}, sky{};
    for (int axis = 0; axis < 8; ++axis)
        for (unsigned c = 0; c < kGatherMaxCascades; ++c) {
            const unsigned src =
                c < in.cascadeCount ? c : (in.cascadeCount ? in.cascadeCount - 1u : 0u);
            Ogre::TextureGpu *t = in.cascadeCount ? in.voxel[src][axis] : nullptr;
            volumes[axis][c].sampler = mHost.gatherLinearSampler();
            volumes[axis][c].imageView = t ? sampledView(t) : dummyVolume;
            volumes[axis][c].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!volumes[axis][c].imageView) return;
        }
    sky.sampler = mHost.gatherLinearSampler();
    sky.imageView = in.sky ? sampledView(in.sky) : dummyCube;
    sky.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!sky.imageView) return;
    {   // the trace set
        VkWriteDescriptorSet w[kTraceBindings] = {};
        for (unsigned i = 0; i < kTraceBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.traceSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        w[0].pNext = &asWrite;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo = &paramsInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &recordsInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[3].pImageInfo = &atlasStore;
        for (int axis = 0; axis < 4; ++axis) {
            w[4 + axis].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[4 + axis].descriptorCount = kGatherMaxCascades;
            w[4 + axis].pImageInfo = volumes[axis];
        }
        w[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[8].pImageInfo = &sky;
        // THE CARD READ'S FOUR (9-12) — the cache's own or the stand-ins, the
        // reflection trace's rule — and THE GEOMETRIC NORMAL'S TWO (13, 14).
        const auto bufferInfo = [](Ogre::UavBufferPacked *b) {
            VkDescriptorBufferInfo info{};
            auto *bi = static_cast<Ogre::VulkanBufferInterface *>(b->getBufferInterface());
            info.buffer = bi->getVboName();
            info.offset = VkDeviceSize(b->_getFinalBufferStart()) * b->getBytesPerElement();
            info.range = b->getTotalSizeBytes();
            return info;
        };
        VkDescriptorBufferInfo stand{};
        stand.buffer = dummyStorage;
        stand.range = VK_WHOLE_SIZE;
        VkDescriptorBufferInfo cardBufs[2] = { stand, stand };
        VkDescriptorImageInfo cardImgs[2] = {};
        if (cardsBound) {
            cardBufs[0] = bufferInfo(in.cardTable);
            cardBufs[1] = bufferInfo(in.cardInstances);
        }
        Ogre::TextureGpu *const layers[2] = { in.cardDepth, in.cardRadiance };
        for (int i = 0; i < 2; ++i) {
            cardImgs[i].sampler = mHost.gatherPointSampler();
            cardImgs[i].imageView = cardsBound ? sampledView(layers[i]) : dummyFlat;
            cardImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!cardImgs[i].imageView || !cardBufs[i].buffer) return;
        }
        VkDescriptorBufferInfo geomBufs[2] = { stand, stand };
        if (geomSlots) {
            geomBufs[0].buffer = v.geomRowOfSlot[ring];
            geomBufs[0].offset = 0;
            geomBufs[0].range = VK_WHOLE_SIZE;
            geomBufs[1] = bufferInfo(in.geomRows);
        }
        for (int i = 0; i < 2; ++i) {
            w[9 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[9 + i].pBufferInfo = &cardBufs[i];
            w[11 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[11 + i].pImageInfo = &cardImgs[i];
            w[13 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[13 + i].pBufferInfo = &geomBufs[i];
        }
        for (unsigned k = 0; k < 4u; ++k) {   // coverage +/-, position +/- (15-18)
            w[15 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[15 + k].descriptorCount = kGatherMaxCascades;
            w[15 + k].pImageInfo = volumes[4 + k];
        }
        vkUpdateDescriptorSets(mHost.gatherDevice(), kTraceBindings, w, 0, nullptr);
    }

    {   // the filter set
        VkDescriptorImageInfo filterRaw{};
        filterRaw.imageView = v.atlasView;
        filterRaw.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w[kFilterBindings] = {};
        for (unsigned i = 0; i < kFilterBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.filterSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &recordsInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[2].pImageInfo = &filterRaw;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kFilterBindings, w, 0, nullptr);
    }

    VkDescriptorImageInfo irradianceStore{};
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = v.irradiance;
        slot.access = Ogre::ResourceAccess::ReadWrite;
        slot.pixelFormat = v.irradiance->getPixelFormat();
        irradianceStore.imageView =
            static_cast<Ogre::VulkanTextureGpu *>(v.irradiance)->createView(slot, false);
        mHost.gatherRetireView(irradianceStore.imageView);
        irradianceStore.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!irradianceStore.imageView) return;
    }
    {   // the integrate set
        VkWriteDescriptorSet w[kIntegrateBindings] = {};
        for (unsigned i = 0; i < kIntegrateBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.integrateSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &recordsInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &normals;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &depth;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4].pImageInfo = &irradianceStore;
        VkDescriptorImageInfo hist[4] = {};
        const VkImageView histViews[4] = { v.historyView[prev], v.historyView[cur],
                                           v.historyGeomView[prev], v.historyGeomView[cur] };
        for (unsigned k = 0; k < 4u; ++k) {
            hist[k].imageView = histViews[k];
            hist[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[5 + k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[5 + k].pImageInfo = &hist[k];
        }
        VkDescriptorImageInfo rest{};
        rest.imageView = v.restMeanView;
        rest.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[9].pImageInfo = &rest;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kIntegrateBindings, w, 0, nullptr);
    }

    // ---- THE LAYOUTS, THROUGH OGRE'S OWN SOLVER ----------------------------
    // Before the command buffer is taken, for the reason recorded at the
    // reflection's own transition block: executeResourceTransition closes every
    // encoder and the queue may roll over to a new command buffer.
    Ogre::RenderSystem *rs = mHost.gatherRenderSystem();
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::ReadWrite, computeStage);
        for (Ogre::TextureGpu *t : { in.normals, in.depth })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        for (unsigned c = 0; c < in.cascadeCount; ++c)
            for (int axis = 0; axis < 8; ++axis)
                if (in.voxel[c][axis])
                    solver.resolveTransition(trans, in.voxel[c][axis],
                                             Ogre::ResourceLayout::Texture,
                                             Ogre::ResourceAccess::Read, computeStage);
        if (in.sky)
            solver.resolveTransition(trans, in.sky, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        // THE CARD READ'S INPUTS (GA-1e), the reflection's transitions: the
        // Radiance layer the CardLight job wrote and the Depth layer the capture
        // copied into, read as textures; the two tables and the geometry rows,
        // read as buffers.
        if (cardsBound) {
            for (Ogre::TextureGpu *t : { in.cardDepth, in.cardRadiance })
                solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                         Ogre::ResourceAccess::Read, computeStage);
            for (Ogre::UavBufferPacked *b : { in.cardTable, in.cardInstances })
                solver.resolveTransition(trans, b, Ogre::ResourceAccess::Read, computeStage);
        }
        if (geomSlots)
            solver.resolveTransition(trans, in.geomRows, Ogre::ResourceAccess::Read, computeStage);
        rs->executeResourceTransition(trans);
    }

    // ---- THE FOUR DISPATCHES -----------------------------------------------
    VkCommandBuffer cmd = mHost.gatherFrameCmd();
    if (!cmd) return;
    // THE BLACK STAND-INS THIS FRAME BOUND, out of UNDEFINED. With the SSR row
    // off the reflection trace never runs and never clears them, and every
    // descriptor slot a scene cannot fill (no sky cube, no voxel arm) points at
    // one — `gi.gather_reference` is exactly that chain.
    mHost.gatherClearDummies(cmd);
    clearAtlas(v, cmd);
    const bool timed = mTimestamps && v.hasQueryBase;
    const uint32_t qbase = v.queryBase + (v.frame % kFramesInFlight) * kQueriesPerFrame;
    if (timed) vkCmdResetQueryPool(cmd, mTimestamps, qbase, kQueriesPerFrame);

    // The frame's counters: the adaptive count to zero, and the trace's
    // indirect arguments to the uniform grid (the placement job raises x by an
    // atomic max for every adaptive probe it appends).
    // ONE WORKGROUP PER PROBE ON X, AND X HAS A CEILING. Vulkan guarantees only
    // 65,535 work groups on a dimension (`maxComputeWorkGroupCount[0]`; this
    // device allows 2^31, but the number a shipped tier may rely on is the
    // guarantee) and Epic at 4K would ask for 162,000. The probe count is
    // CLAMPED here rather than folded to two dimensions, which would change the
    // shader's index arithmetic for a case no tier reaches today: 1080p Epic is
    // 32,400 probes and the clamp is 65,535, so nothing this lane ships comes
    // near it. A frame that hits it traces the probes it can and says so once.
    const uint32_t maxGroups = mMaxWorkGroupX ? mMaxWorkGroupX : 65535u;
    if (v.uniformProbes > maxGroups && !mSaidWorkGroupClamp) {
        mSaidWorkGroupClamp = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the screen-probe grid asks for " + std::to_string(v.uniformProbes) +
            " work groups and this device allows " + std::to_string(maxGroups) +
            " — the gather traces the first " + std::to_string(maxGroups) +
            " probes of each frame (a coarser stride is the cure)");
    }
    const uint32_t argsInit[4] = { std::min(v.uniformProbes, maxGroups), 1u, 1u, 0u };
    vkCmdFillBuffer(cmd, v.counter, 0, 16, 0u);
    vkCmdUpdateBuffer(cmd, v.args, 0, sizeof(argsInit), argsInit);
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
    }

    // ...AND THE FRAME MONITOR'S OWN ROWS BESIDE THEM (`gather.place`,
    // `gather.trace`, `gather.integrate`). The timestamps above are what
    // `giStatus().gather` reports; these are what a monitor CAPTURE shows,
    // beside `vct.cascadeN` and `ifd.build` — the same instrument the rest of
    // the GI work reports through, so a capture of a gathering frame accounts
    // for all of it. They cost nothing while the monitor is off (`gMonitor` null
    // is the constructor's first line).
    //
    // THE REASON IS `Camera` AND NOT `None`, which the monitor reads as "no
    // recorded input change" — its redundant-work value. A screen probe is a
    // VIEW-DEPENDENT estimate, like the planar reflector's render beside it in
    // the same enum: it is re-made every frame because the thing it describes is
    // the picture, not because nothing told it to stop.
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0,
                                         "gather.place", rs);
        work.setUnits(v.uniformProbes);
        if (timed) vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qbase);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPlacePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPlacePipeLayout, 0, 1,
                                &v.placeSets[ring], 0, nullptr);
        vkCmdDispatch(cmd, (v.gridW + 7u) / 8u, (v.gridH + 7u) / 8u, 1u);
        if (timed)
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps,
                                qbase + 1u);
    }
    {
        // The records and the argument buffer the trace is about to read — and
        // the argument buffer is read by the INDIRECT stage, which is not the
        // compute one.
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    // HOW MANY THE FRAME APPENDED, on its way to the host: four bytes into this
    // frame's slot of a ring the CPU reads once the frame has retired.
    {
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = VkDeviceSize(v.frame % kFramesInFlight) * 16u;
        region.size = 4u;
        vkCmdCopyBuffer(cmd, v.counter, v.readback, 1, &region);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
    }

    detail::monitor::CacheScope traceWork(CacheKind::Gi, WorkReason::Camera, 0,
                                          "gather.trace", rs);
    traceWork.setUnits(v.uniformProbes + v.adaptiveLast);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qbase + 2u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTracePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTracePipeLayout, 0, 1,
                            &v.traceSets[ring], 0, nullptr);
    // ONE WORKGROUP PER PROBE — the trace's 64 threads ARE its 64 rays — and
    // the COUNT is the GPU's own (patch 0032's shape: a count nobody on the CPU
    // can know without a round trip).
    vkCmdDispatchIndirect(cmd, v.args, 0);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qbase + 3u);
    traceWork.close();
    const auto computeToCompute = [&]() {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
    };
    computeToCompute();

    // THE FILTER IN PROBE SPACE AND THE SH (PHOTON-GATHER-1b): one workgroup
    // per probe again, from the SAME indirect arguments — every probe the trace
    // traced is filtered, and none it did not.
    detail::monitor::CacheScope filterWork(CacheKind::Gi, WorkReason::Camera, 0,
                                           "gather.filter", rs);
    filterWork.setUnits(v.uniformProbes + v.adaptiveLast);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qbase + 4u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeLayout, 0, 1,
                            &v.filterSets[ring], 0, nullptr);
    vkCmdDispatchIndirect(cmd, v.args, 0);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qbase + 5u);
    filterWork.close();
    computeToCompute();

    detail::monitor::CacheScope integrateWork(CacheKind::Gi, WorkReason::Camera, 0,
                                              "gather.integrate", rs);
    integrateWork.setUnits(in.width * in.height / 1000u);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qbase + 6u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeLayout, 0, 1,
                            &v.integrateSets[ring], 0, nullptr);
    vkCmdDispatch(cmd, (in.width + 7u) / 8u, (in.height + 7u) / 8u, 1u);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qbase + 7u);
    // THE IRRADIANCE READBACK (a test door): the integrate's output, still in
    // the GENERAL layout its storage writes left it in, copied into this frame's
    // slot of a host ring and decoded once the frame has retired (readPending).
    bool readbackThisFrame = false;
    if (in.tuning.readback) {
        const VkDeviceSize slotBytes = VkDeviceSize(v.w) * v.h * 8u;
        if (!v.irrReadback) {
            std::string rerr;
            if (!mHost.gatherMakeBuffer(slotBytes * kFramesInFlight,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, v.irrReadback,
                                        v.irrReadbackMemory, &v.irrReadbackMapped, rerr))
                v.irrReadback = VK_NULL_HANDLE;
        }
        if (v.irrReadback) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0,
                                 nullptr);
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(v.frame % kFramesInFlight) * slotBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { v.w, v.h, 1u };
            vkCmdCopyImageToBuffer(
                cmd, static_cast<Ogre::VulkanTextureGpu *>(v.irradiance)->getFinalTextureName(),
                VK_IMAGE_LAYOUT_GENERAL, v.irrReadback, 1, &region);
            // The host reads it after the frame retires; and the copy is ordered
            // before whatever the layout transition below does to the image (an
            // execution chain through the compute stage Ogre's barrier starts at).
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &toHost, 0, nullptr, 0, nullptr);
            readbackThisFrame = true;
        }
    }
    {
        // The pending record is written whether or not the device timestamps:
        // it is also what retires this frame's adaptive-count readback.
        View::Pending &pd = v.pending[v.frame % kFramesInFlight];
        pd.frame = mHost.gatherFrameNow();
        pd.live = true;
        pd.irradiance = readbackThisFrame;
        pd.held = false;
        pd.gatherFrame = v.frame;
    }
    integrateWork.close();

    // ...AND NOW THE PIXEL SHADER READS IT. The transition to a sampled layout
    // is ours to ask for: the scene pass that follows does not know this texture
    // exists (it arrives through the Hlms listener's extra slot, not through the
    // compositor), so nobody else would order the barrier.
    {
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    // THE PROPERTY AND THE TEXTURE ARE SET TOGETHER OR NOT AT ALL — the same
    // rule the sky's env slot states: a slot claimed by getNumExtraPassTextures
    // and left unbound is an undefined descriptor. The registration is
    // PASS-SCOPED and `releaseBinding` takes it away when the pass ends.
    detail::FogHlmsListener::setProbeGather(in.sceneMgr, v.irradiance);
    // ...AND THIS FRAME BECOMES THE PREVIOUS ONE: its camera, its half of each
    // pair, one more frame of age.
    std::memcpy(v.prevCamPos, in.camPos, sizeof(v.prevCamPos));
    for (int i = 0; i < 3; ++i) {
        v.prevRayTL[i] = in.rayTL[i];
        v.prevRayRight[i] = in.rayRight[i];
        v.prevRayDown[i] = in.rayDown[i];
        v.prevFwd[i] = in.fwd[i];
    }
    v.flip ^= 1u;
    ++v.age;
    v.recordedFrame = mHost.gatherFrameNow();
    v.lastTemporal = temporal;
    v.cpuMs = float(msSince(cpuStart));
    ++v.frame;
}

}   // namespace engine
}   // namespace jahshaka

#endif   // JAH_RAY_QUERY
