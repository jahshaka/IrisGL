// THE SCREEN-PROBE GATHER — the Component's implementation (GATHER-1a,
// 2026-09-21; SPECS/SCREEN_PROBE_GATHER_SPEC.md section 9).
//
// THREE COMPUTE DISPATCHES on the frame's own command buffer, recorded at the
// reflection listener's hook — after the SSR prepass (whose depth and normals
// are the probes' surfaces) and before the opaque pass that shades:
//
//   rq_probe_place.comp      one thread per cell: the probe's jittered pixel,
//                            its world position and normal, and the adaptive
//                            probe the cell wants where one is not enough.
//   rq_probe_gather.comp     one WORKGROUP per probe, one ray per thread, one
//                            ray per octahedral texel; dispatched INDIRECTLY
//                            because the adaptive probes' count is the GPU's.
//   rq_probe_integrate.comp  one thread per pixel: the probe its plane agrees
//                            with, into `jahProbeIrradiance`.
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

#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanDevice.h"

#include "rayquery/rq_probe_place_spv.h"
#include "rayquery/rq_probe_gather_spv.h"
#include "rayquery/rq_probe_integrate_spv.h"

#include <algorithm>
#include <chrono>
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
/// Six timestamps a frame: a pair around each of the three jobs.
constexpr unsigned kQueriesPerFrame = 6u;

constexpr unsigned kPlaceBindings = 6u;
constexpr unsigned kTraceBindings = 9u;
constexpr unsigned kIntegrateBindings = 5u;

/// One probe's record: three vec4s (see JahProbeRecord in the shaders).
constexpr unsigned kRecordBytes = 48u;

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
};

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
        };
        const unsigned c[kTraceBindings] = { 1u, 1u, 1u, 1u, kGatherMaxCascades,
                                             kGatherMaxCascades, kGatherMaxCascades,
                                             kGatherMaxCascades, 1u };
        if (!makeLayout(kTraceBindings, t, c, mTraceLayout, "trace")) return false;
    }
    {   // rq_probe_integrate.comp
        const VkDescriptorType t[kIntegrateBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 records
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 normals
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 depth
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 irradiance
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
    if (!makeOne(mIntegrateLayout, krq_probeIntegrateSpv, sizeof(krq_probeIntegrateSpv),
                 mIntegratePipeLayout, mIntegrateModule, mIntegratePipeline, "integrate"))
        return false;

    const unsigned sets = kMaxTimedViews * kRing * 3u;
    VkDescriptorPoolSize sizes[4] = {};
    sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    sizes[0].descriptorCount = sets;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = sets;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[2].descriptorCount = sets * 3u;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[3].descriptorCount = sets * 2u;
    VkDescriptorPoolSize sampled{};
    sampled.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampled.descriptorCount = sets * (2u + 4u * kGatherMaxCascades + 1u);
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

    v.vramBytes = (unsigned long long)v.atlasW * v.atlasH * 8ull +
                  (unsigned long long)total * kRecordBytes +
                  (unsigned long long)in.width * in.height * 8ull;
    v.targetsReady = true;
    v.atlasNeedsClear = true;
    return true;
}

void ScreenProbeGather::drop(View &v) {
    for (unsigned i = 0; i < kRing; ++i) {
        mHost.gatherRetireSet(v.placeSets[i], mPool);
        mHost.gatherRetireSet(v.traceSets[i], mPool);
        mHost.gatherRetireSet(v.integrateSets[i], mPool);
        v.placeSets[i] = v.traceSets[i] = v.integrateSets[i] = VK_NULL_HANDLE;
        mHost.gatherRetireBuffer(v.params[i], v.paramsMemory[i]);
        v.params[i] = VK_NULL_HANDLE;
        v.paramsMemory[i] = VK_NULL_HANDLE;
        v.paramsMapped[i] = nullptr;
    }
    mHost.gatherRetireBuffer(v.records, v.recordsMemory);
    mHost.gatherRetireBuffer(v.counter, v.counterMemory);
    mHost.gatherRetireBuffer(v.args, v.argsMemory);
    mHost.gatherRetireBuffer(v.readback, v.readbackMemory);
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
    v.placeMs = v.traceMs = v.integrateMs = v.cpuMs = -1.0f;
}

void ScreenProbeGather::readPending(View &v) {
    const uint32_t nowAll = mHost.gatherFrameNow(), inFlightAll = mHost.gatherFramesInFlight();
    // THE ADAPTIVE COUNT first, because it is read even on a device with no
    // timestamp support: it is a COUNT, not a measurement, and `giStatus()`
    // reports it beside the probe grid.
    if (v.readbackMapped) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) {
            if (!v.pending[i].live || uint32_t(nowAll - v.pending[i].frame) < inFlightAll) continue;
            uint32_t appended = 0u;
            std::memcpy(&appended, static_cast<const char *>(v.readbackMapped) + i * 16u,
                        sizeof(appended));
            // The shader's counter keeps counting past the cap (an atomicAdd
            // cannot un-count), so what a frame actually APPENDED is the
            // smaller of the two.
            v.adaptiveAsked = appended;
            v.adaptiveLast = std::min(appended, v.adaptiveCap);
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
            span(4, 5, v.integrateMs);
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
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = v.atlas;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &b);
    VkClearColorValue zero{};
    vkCmdClearColorImage(cmd, v.atlas, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
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
            v.placeSets[i] = v.traceSets[i] = v.integrateSets[i] = VK_NULL_HANDLE;
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
    if (mIntegratePipeline) vkDestroyPipeline(dev, mIntegratePipeline, nullptr);
    if (mPlaceModule) vkDestroyShaderModule(dev, mPlaceModule, nullptr);
    if (mTraceModule) vkDestroyShaderModule(dev, mTraceModule, nullptr);
    if (mIntegrateModule) vkDestroyShaderModule(dev, mIntegrateModule, nullptr);
    if (mPlacePipeLayout) vkDestroyPipelineLayout(dev, mPlacePipeLayout, nullptr);
    if (mTracePipeLayout) vkDestroyPipelineLayout(dev, mTracePipeLayout, nullptr);
    if (mIntegratePipeLayout) vkDestroyPipelineLayout(dev, mIntegratePipeLayout, nullptr);
    if (mPlaceLayout) vkDestroyDescriptorSetLayout(dev, mPlaceLayout, nullptr);
    if (mTraceLayout) vkDestroyDescriptorSetLayout(dev, mTraceLayout, nullptr);
    if (mIntegrateLayout) vkDestroyDescriptorSetLayout(dev, mIntegrateLayout, nullptr);
    if (mTimestamps) vkDestroyQueryPool(dev, mTimestamps, nullptr);
    mPool = VK_NULL_HANDLE;
    mPlacePipeline = mTracePipeline = mIntegratePipeline = VK_NULL_HANDLE;
    mPlaceModule = mTraceModule = mIntegrateModule = VK_NULL_HANDLE;
    mPlacePipeLayout = mTracePipeLayout = mIntegratePipeLayout = VK_NULL_HANDLE;
    mPlaceLayout = mTraceLayout = mIntegrateLayout = VK_NULL_HANDLE;
    mTimestamps = VK_NULL_HANDLE;
    mQuerySlots = 0u;
}

void ScreenProbeGather::statsInto(const detail::OgreScene *scene, GatherStatus &out) const {
    out.error = mLastError;
    for (const auto &kv : mViews) {
        const View &v = kv.second;
        if (v.scene != scene || !v.targetsReady) continue;
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
        out.raysPerFrame =
            (unsigned long long)(v.uniformProbes + v.adaptiveLast) * out.raysPerProbe;
        out.targetW = v.w;
        out.targetH = v.h;
        out.atlasBytes = v.vramBytes;
        out.placeMs = v.placeMs;
        out.traceMs = v.traceMs;
        out.integrateMs = v.integrateMs;
        out.cpuMs = v.cpuMs;
        return;
    }
}

// ---------------------------------------------------------------------------
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

    // ---- WHAT THE TIER ASKS FOR, and what the quality row derives ----------
    // 16 pixels per probe at Medium and High, 8 at Epic (four times the
    // probes); 64 rays each, 36 at Medium. The ray budget is NOT what decides
    // these numbers — GATHER-0 measured the trace at 0.21 ns a ray and found
    // the 8,160-probe arm FASTER than a 2,040-probe one at the same ray count
    // (probe count is what fills this GPU) — the picture and the VRAM are.
    //
    // WHERE "EPIC" COMES FROM, since the engine's `GiQuality` has three values
    // and Epic is not one of them (the tier table maps Epic onto High plus a
    // bigger cascade set): the VIEW's SSR row, exactly as the reflection trace
    // reads it for its own resolution — 2 at Epic, 1 at High, 0 below. One row,
    // two consumers, no second setting, and the same sentence R5's note makes.
    // The Studio tier table gains a column of its own at phase 4, where the
    // gather is turned on by tier at all.
    unsigned stride = in.epicRow ? 8u : 16u;
    unsigned octRes = in.quality == GiQuality::Medium ? 6u : 8u;
    if (in.tuning.probeStride) stride = in.tuning.probeStride;
    if (in.tuning.octRes) octRes = in.tuning.octRes;
    stride = std::min(std::max(stride, 2u), 64u);
    octRes = std::min(std::max(octRes, 1u), kGatherOctResMax);

    View &v = mViews[key];
    v.scene = in.scene;
    v.sceneMgr = in.sceneMgr;
    readPending(v);

    const unsigned gridW = (in.width + stride - 1u) / stride;
    const unsigned gridH = (in.height + stride - 1u) / stride;
    // HOW MANY ADAPTIVE PROBES A FRAME MAY ADD. A quarter of the grid: Lumen's
    // own budget is a fixed allocation of the same order, and the number is a
    // CAP rather than a target — a flat scene spends none of it. The cap is
    // part of the allocation (the atlas and the record buffer are sized for the
    // worst case, resident, never per frame — the 0071 lesson), so it cannot be
    // a per-frame decision.
    unsigned adaptiveCap = gridW * gridH / 4u;
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
    pp.knobs[2] = in.tuning.freezeFrameIndex ? 0.0f : float(v.frame & 0xFFFFu);
    pp.knobs[3] = float(in.cascadeCount);
    pp.knobs2[0] = in.anisotropic ? 1.0f : 0.0f;
    pp.knobs2[1] = in.cascadeCount ? std::max(0.01f, 0.5f * in.voxelCell[0]) : 0.02f;
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
    VkImageView dummyCube = VK_NULL_HANDLE, dummyVolume = VK_NULL_HANDLE;
    if (!mHost.gatherDummies(dummyCube, dummyVolume, err)) return;

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
    VkDescriptorImageInfo volumes[4][kGatherMaxCascades] = {}, sky{};
    for (int axis = 0; axis < 4; ++axis)
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
        vkUpdateDescriptorSets(mHost.gatherDevice(), kTraceBindings, w, 0, nullptr);
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
            for (int axis = 0; axis < 4; ++axis)
                if (in.voxel[c][axis])
                    solver.resolveTransition(trans, in.voxel[c][axis],
                                             Ogre::ResourceLayout::Texture,
                                             Ogre::ResourceAccess::Read, computeStage);
        if (in.sky)
            solver.resolveTransition(trans, in.sky, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        rs->executeResourceTransition(trans);
    }

    // ---- THE THREE DISPATCHES ----------------------------------------------
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
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
    }

    detail::monitor::CacheScope integrateWork(CacheKind::Gi, WorkReason::Camera, 0,
                                              "gather.integrate", rs);
    integrateWork.setUnits(in.width * in.height / 1000u);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mTimestamps, qbase + 4u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeLayout, 0, 1,
                            &v.integrateSets[ring], 0, nullptr);
    vkCmdDispatch(cmd, (in.width + 7u) / 8u, (in.height + 7u) / 8u, 1u);
    if (timed)
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, mTimestamps, qbase + 5u);
    {
        // The pending record is written whether or not the device timestamps:
        // it is also what retires this frame's adaptive-count readback.
        View::Pending &pd = v.pending[v.frame % kFramesInFlight];
        pd.frame = mHost.gatherFrameNow();
        pd.live = true;
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
    v.cpuMs = float(msSince(cpuStart));
    ++v.frame;
}

}   // namespace engine
}   // namespace jahshaka

#endif   // JAH_RAY_QUERY
