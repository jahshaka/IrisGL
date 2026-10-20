// ATOM-SHADOWS-1 — THE CASTER CUT (SPECS/briefs/ATOM-SHADOWS-1.md; V2_MASTER_SPEC §3 S3d).
//
// A shadow map's Atom casters are drawn from the LIGHT'S OWN CLUSTER CUT, never at a
// per-object level through the stock PBS caster. Every caster scene pass of a shadow
// node skips the Atom queue and is followed, on the same target and the same map, by
// this PASS_CUSTOM (OgreShadow.cpp), which records:
//   1. THE CUT for the map's camera — the cull's cut mode (OgreScene::recordGpuCull,
//      the id pass's own three stages: frustum, the cluster cut per survivor, the
//      compaction), with the SHADOW CAMERA's frustum, eye and projection and the
//      MAP'S OWN HEIGHT in texels as the viewport: the rule's tolerance is one texel
//      of the map (x the scene's LOD bias), the physically right grain for a depth
//      image — an error under one texel moves no texel's depth by more than the
//      rasteriser's own quantisation. FRUSTUM ONLY: a light has no depth pyramid.
//      The predicate is the node kind's caster channel on the GPU scene's flags:
//      kGpuVisible | kGpuAtomCaster | kGpuCaster (every Atom item, and the items whose
//      colour stays on PBS while the cut draws their caster — a blended item,
//      ENGINE-POLISH-ATOM-1 stage 2; they leave the stock passes' channel, kStockCasterBit),
//      and for the probe kind (probes — the still world, shadowCasterChannels) not kGpuMover.
//   2. THE EDGES the id pass records (the previous use's draw reads before this cut's
//      writes; the cut's writes before the draw's indirect/index/vertex reads).
//   3. OGRE'S RENDER PASS for the map (AtomPass::beginRenderPass: the atlas's depth,
//      or the point light's cube face and its depth buffer, LOADED — the scene pass
//      before this one drew the PBS casters into it), the map's viewport, and ONE
//      vkCmdDrawIndexedIndirectCountKHR over the compacted stream: depth only for a
//      directional or spot map, the biased linear distance for a point map's face —
//      the stock caster's arithmetic exactly (atom_caster.vert / _point.frag).
//
// ONE LIST PER SCENE (OgreScene::casterCull), reused map after map: the uses are
// serialised by the edges above, which the GPU pays anyway (every cut ends in a
// compute -> indirect barrier), so a scene holds one stream however many maps its
// views, mirrors and probes render — a list per map would be ~15 MB each at the first
// budget, up to 59 maps a node.
//
// THE PASS'S GATE (AtomPassGate, casterGate): a scene with no caster for the cut (kGpuAtomCaster) executes this pass as
// nothing — Ogre's render pass stays open across it, the upstream pass list's cost.
//
// THE PIPELINES: no vertex attributes, push constants only (128 bytes), a render pass
// COMPATIBLE with Ogre's (the map's formats), per (formats, cull face, depth clamp).
// The stock caster's state: an Atom item's caster macroblock culls as its material does
// (atomRouteFor keeps any other on PBS) — back faces culled, FRONT where the pass flips
// (Hlms's InvertCullingMode), or NOTHING for a two-sided item (ATOM-TWO-SIDED-1: the
// list's two-sided range, a second draw with the no-cull pipeline); depth
// GREATER_OR_EQUAL under reverse-Z with writes, depth CLAMP where the shadow camera
// asks for it (a directional light's maps, OgreCompositorShadowNode.cpp).
#include "EnginePrivate.h"
#include "AtomPass.h"
#include "GpuCull.h"
#include "GpuScene.h"
#include "HlmsAtom.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreCamera.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreResourceTransition.h>
#include <OgreLight.h>
#include <OgreLogManager.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreTextureGpuManager.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <array>
#include <optional>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if JAH_RAY_QUERY
#include "OgreVulkanDevice.h"
#include "OgreVulkanMappings.h"
#include "OgreVulkanQueue.h"
#include "OgreVulkanRenderSystem.h"
#include "AtomVulkan.h"

#include "rayquery/atom_caster_masked_frag_spv.h"
#include "rayquery/atom_caster_masked_vert_spv.h"
#include "rayquery/atom_caster_point_frag_spv.h"
#include "rayquery/atom_caster_point_masked_frag_spv.h"
#include "rayquery/atom_caster_vert_spv.h"
#endif

namespace jahshaka {
namespace engine {
namespace detail {

#if JAH_RAY_QUERY
namespace {

/// The push constants, as atom_caster.vert and atom_caster_point.frag declare them:
/// six vec4 then four uvec2 — 128 bytes, the size every device guarantees.
struct CasterPushConstants {
    float    viewProjRow[16] = {};
    float    eyeBias[4] = {};      ///< xyz the shadow camera, w its constant-bias scale
    float    depthRange[4] = {};   ///< x near, y 1 / (far - near), z 1 = point map
    uint32_t instances[2] = {};
    uint32_t meshes[2] = {};
    uint32_t rows[2] = {};
    uint32_t maskTable[2] = {};   ///< GpuScene::maskTableAddress (the masked ranges' fragment stage)
};
static_assert(sizeof(CasterPushConstants) == 128, "atom_caster.vert/_point.frag's push-constant block");
constexpr VkShaderStageFlags kCasterPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct CasterPipelineKey {
    VkFormat colour = VK_FORMAT_UNDEFINED;   ///< UNDEFINED = depth only (a directional/spot map)
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool cullFront = false;                  ///< the pass flips: Hlms's InvertCullingMode
    bool cullNone = false;                   ///< the two-sided range (ATOM-TWO-SIDED-1): both faces
    bool depthClamp = false;
    /// THE MASKED RANGES (ENGINE-POLISH-ATOM-1 stage 3): a fragment stage that discards where the cut-out's mask is
    /// clear — for a directional or spot map too (atom_caster_masked.frag), whose opaque ranges have none.
    bool masked = false;
    bool operator==(const CasterPipelineKey &o) const {
        return colour == o.colour && depth == o.depth && cullFront == o.cullFront && cullNone == o.cullNone &&
               depthClamp == o.depthClamp && masked == o.masked;
    }
};

struct CasterPipelines {
    VkDevice dev = VK_NULL_HANDLE;
    PFN_vkCmdDrawIndexedIndirectCountKHR drawIndexedIndirectCount = nullptr;
    PFN_vkGetBufferDeviceAddressKHR bufferDeviceAddress = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    struct Entry {
        CasterPipelineKey key;
        VkRenderPass compatible = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    std::vector<Entry> entries;
    bool refused = false;   ///< a creation failed; said once, the pass records nothing
};
/// Said once per message (the device's lastLog; defined below its type).
void logOnce(const std::string &what);


VkShaderModule makeModule(VkDevice dev, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

void destroyPipelines(CasterPipelines &C) {
    if (!C.dev) return;
    for (CasterPipelines::Entry &e : C.entries) {
        if (e.pipeline) vkDestroyPipeline(C.dev, e.pipeline, nullptr);
        if (e.compatible) vkDestroyRenderPass(C.dev, e.compatible, nullptr);
    }
    C.entries.clear();
}

/// The pipeline for `key` — built on first use.
VkPipeline pipelineFor(CasterPipelines &C, Ogre::VulkanRenderSystem *vkRs, const CasterPipelineKey &key) {
    if (C.refused) return VK_NULL_HANDLE;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    if (!C.dev) {
        C.dev = device->mDevice;
        C.drawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(
            vkGetDeviceProcAddr(C.dev, "vkCmdDrawIndexedIndirectCountKHR"));
        C.bufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            vkGetDeviceProcAddr(C.dev, "vkGetBufferDeviceAddressKHR"));
        VkPushConstantRange pc{};
        pc.stageFlags = kCasterPushStages;
        pc.offset = 0;
        pc.size = sizeof(CasterPushConstants);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        if (!C.drawIndexedIndirectCount || !C.bufferDeviceAddress ||
            vkCreatePipelineLayout(C.dev, &pl, nullptr, &C.layout) != VK_SUCCESS) {
            C.refused = true;
            logOnce("no vkCmdDrawIndexedIndirectCountKHR / vkGetBufferDeviceAddressKHR, or the layout "
                    "failed: the shadow maps draw no Atom caster");
            return VK_NULL_HANDLE;
        }
    }
    for (const CasterPipelines::Entry &e : C.entries)
        if (e.key == key) return e.pipeline;

    CasterPipelines::Entry e;
    e.key = key;
    const bool point = key.colour != VK_FORMAT_UNDEFINED;
    // A RENDER PASS COMPATIBLE WITH OGRE'S: the same attachment formats and sample
    // counts, one subpass (load/store ops do not take part in compatibility).
    VkAttachmentDescription att[2]{};
    uint32_t n = 0;
    if (point) {
        att[n].format = key.colour;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ++n;
    }
    const uint32_t depthIndex = n;
    att[n].format = key.depth;
    att[n].samples = VK_SAMPLE_COUNT_1_BIT;
    att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[n].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    ++n;
    VkAttachmentReference colourRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ depthIndex, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = point ? 1u : 0u;
    sub.pColorAttachments = point ? &colourRef : nullptr;
    sub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = n;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (vkCreateRenderPass(C.dev, &rp, nullptr, &e.compatible) != VK_SUCCESS) {
        C.refused = true;
        logOnce("vkCreateRenderPass (the compatible pass) failed");
        return VK_NULL_HANDLE;
    }

    VkShaderModule vs = key.masked ? makeModule(C.dev, katomCasterMasked_vertSpv, sizeof(katomCasterMasked_vertSpv))
                                   : makeModule(C.dev, katomCaster_vertSpv, sizeof(katomCaster_vertSpv));
    const bool hasFragment = point || key.masked;
    VkShaderModule fs = point ? (key.masked ? makeModule(C.dev, katomCasterPointMasked_fragSpv,
                                                         sizeof(katomCasterPointMasked_fragSpv))
                                            : makeModule(C.dev, katomCasterPoint_fragSpv, sizeof(katomCasterPoint_fragSpv)))
                      : key.masked ? makeModule(C.dev, katomCasterMasked_fragSpv, sizeof(katomCasterMasked_fragSpv))
                                   : VK_NULL_HANDLE;
    if (!vs || (hasFragment && !fs)) {
        if (vs) vkDestroyShaderModule(C.dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(C.dev, fs, nullptr);
        vkDestroyRenderPass(C.dev, e.compatible, nullptr);
        C.refused = true;
        logOnce("vkCreateShaderModule failed");
        return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rast{};
    rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rast.depthClampEnable = key.depthClamp ? VK_TRUE : VK_FALSE;
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = key.cullNone ? VK_CULL_MODE_NONE : key.cullFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    rast.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rast.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse-Z; Ogre's LESS_EQUAL
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = point ? 1u : 0u;
    cb.pAttachments = point ? &cba : nullptr;
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = hasFragment ? 2u : 1u;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rast;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = C.layout;
    gp.renderPass = e.compatible;
    gp.subpass = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult r = vkCreateGraphicsPipelines(C.dev, device->mPipelineCache, 1, &gp, nullptr, &e.pipeline);
    const double createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    vkDestroyShaderModule(C.dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(C.dev, fs, nullptr);
    if (r != VK_SUCCESS) {
        vkDestroyRenderPass(C.dev, e.compatible, nullptr);
        e.compatible = VK_NULL_HANDLE;
        e.pipeline = VK_NULL_HANDLE;
        if (key.cullNone || key.masked) {
            // THE NO-CULL AND THE MASKED PIPELINES ARE OPTIONAL (ATOM-TWO-SIDED-1; ENGINE-POLISH-ATOM-1 stage 3):
            // refused, the map still draws its one-sided opaque Atom casters — only that range goes undrawn.
            // Remembered as a null entry, so it is asked of the device once, never per map.
            logOnce(std::string("vkCreateGraphicsPipelines failed for the ") + (key.masked ? "masked " : "") +
                    (key.cullNone ? "no-cull (two-sided) " : "") + "caster pipeline: those Atom items cast no "
                    "shadow on this device");
            C.entries.push_back(e);
            return VK_NULL_HANDLE;
        }
        C.refused = true;
        logOnce("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka atom caster pass: pipeline (" + std::string(point ? "point" : "depth") +
            (key.cullNone ? ", no cull" : key.cullFront ? ", front cull" : ", back cull") +
            (key.masked ? ", masked" : "") + ") created in " +
            std::to_string(createMs) + " ms",
        Ogre::LML_NORMAL);
    C.entries.push_back(e);
    return e.pipeline;
}

// ---- THE STATS RING, per scene -------------------------------------------------
// The caster list's count words copied into host-visible memory after every use and
// read once the frame that wrote them has retired (the id pass's ring, OgreAtomIdPass.cpp,
// with a column per use: one list serves `columns` maps a frame).
//
// THE WIDTH FOLLOWS THE FRAME (CASTER-USES-1). It was a fixed 64 columns, and a first
// frame renders every map at once — a 59-map view node, a mirror's node and the probes'
// — so the overflow went unrecorded until a later frame. The ring starts at kFirstUses
// and DOUBLES the moment a frame asks for one more column than it has, copying the
// columns this frame already wrote (the other slots belong to frames still in flight,
// which wrote the old buffer: their stats are skipped once, never mis-read). kMaxUses is
// the named bound — a frame past it records no more and says how many it did not
// (`casterUnrecorded`).
constexpr uint32_t kFirstUses = 64u;
constexpr uint32_t kMaxUses = 4096u;
constexpr uint32_t kNoUse = 0xFFFFFFFFu;
/// ONE CUT'S COUNT WORDS, and A USE'S COLUMN: four of them (ENGINE-UPGRADE-U5 stage 4) - the first
/// cut, the late cut (a sun map's second half), and the measuring door's two stock-caster counts
/// (shadow.casterPbsProbe: the frustum's, then the ones the map's depth does not hide). A block no
/// cut wrote this frame reads zero (beginUse clears the column).
constexpr VkDeviceSize kCountBytes = GpuCull::kCountElements * sizeof(uint32_t);
enum UseBlock : uint32_t {
    kBlockFirst = 0u, kBlockLate = 1u, kBlockPbsAll = 2u, kBlockPbsVisible = 3u, kUseBlocks = 4u
};
constexpr VkDeviceSize kUseBytes = kCountBytes * kUseBlocks;

struct CasterRing {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const uint32_t *mapped = nullptr;
    uint32_t slots = 0u;
    uint32_t uses[8] = {};        ///< the uses recorded into each slot
    uint32_t writtenAt[8] = {};
    bool written[8] = {};
    uint32_t frame = ~0u;         ///< the frame the uses below count
    uint32_t usesThisFrame = 0u;
    uint32_t columns = 0u;        ///< uses one slot holds (the ring's width)
    unsigned peakUses = 0u;       ///< the most uses one retired frame recorded
    unsigned long long unrecorded = 0ull;   ///< uses past kMaxUses (or a failed growth)
    /// The triangles each use of the last frame READ drew (the metrics a use adds).
    std::vector<unsigned long long> lastTriangles;
    /// WHICH MAP each use of a slot was (AtomCasterMapStats: kind << 16 | face << 8 | map, and
    /// bit 24 = the light's depth tested it), host-side: the counters carry no name.
    std::vector<uint32_t> tags[8];
    /// ...and each use's receiver dilation in texels along the map's x and y (decision 19.1; 0 without a grid).
    std::vector<std::array<float, 2>> dilations[8];
};

void destroyRing(const CasterPipelines &C, CasterRing &r) {
    if (!C.dev) return;
    if (r.memory) {
        vkUnmapMemory(C.dev, r.memory);
        vkFreeMemory(C.dev, r.memory, nullptr);
    }
    if (r.buffer) vkDestroyBuffer(C.dev, r.buffer, nullptr);
    r = CasterRing();
}

bool ensureRing(const CasterPipelines &C, Ogre::VulkanDevice *device, CasterRing &r, uint32_t slots,
                uint32_t columns = kFirstUses) {
    if (r.buffer) return true;
    slots = std::min(std::max(slots, 2u), 8u);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kUseBytes * columns * slots;
    // TRANSFER_SRC: a growth copies this frame's columns out of it.
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(C.dev, &bi, nullptr, &r.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(C.dev, r.buffer, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(device->mPhysicalDevice, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void *p = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(C.dev, &ai, nullptr, &r.memory) != VK_SUCCESS ||
        vkBindBufferMemory(C.dev, r.buffer, r.memory, 0) != VK_SUCCESS ||
        vkMapMemory(C.dev, r.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        destroyRing(C, r);
        return false;
    }
    r.mapped = static_cast<const uint32_t *>(p);
    r.slots = slots;
    r.columns = columns;
    return true;
}

// ---- THE TEST DOOR (OgreScene::armCasterProbeForTest) -------------------------------
constexpr uint32_t kProbeRecords = 1u << 18;   // 4 MB of records: every fixture's cut fits
struct ProbeState {
    bool armed = false, captured = false;
    uint32_t map = 0, face = 0, frame = 0, recordBudget = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const uint32_t *mapped = nullptr;
    OgreScene::CasterProbe info;
    /// THE SAME MAP'S LATE CUT (stage 4's second half, decision 16.5's per-object law): its own buffer, filled
    /// the frame the first cut was captured when that map ran its late half.
    struct Late {
        bool captured = false;
        uint32_t recordBudget = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        const uint32_t *mapped = nullptr;
    } late;
};
}  // namespace

/// THE CASTER PASS'S DEVICE STATE (HlmsAtom.h): the pipelines, the rings of scenes that
/// went away (and of a ring that doubled) with the frame they left at — destroyed once
/// that frame has retired, never a device wait — and the last line said.
struct AtomCasterDevice {
    CasterPipelines C;
    std::vector<std::pair<CasterRing, uint32_t>> grave;
    std::string lastLog;
    uint32_t pyramidIds = 0u;   ///< the sun maps' pyramid textures' names (JahLightHzb/<n>): the device's, never a static
};
/// A SUN MAP'S OWN DEPTH (ENGINE-UPGRADE-U5 stage 4): the farthest-depth pyramid of one map of one
/// shadow node (its rectangle of the atlas: R32F, the full chain), the rectangle's corner for the
/// build, and the HISTORY - the matrix the pyramid's depth was drawn with (the cull's, y row
/// negated), valid once built. Made on the map's SECOND consecutive frame (a two-frame view - a
/// thumbnail - never pays for one), remade when the map's rectangle or atlas changes, dropped once
/// its node has not drawn the map for kPyramidIdleFrames.
struct LightPyramid {
    Ogre::TextureGpu *tex = nullptr;
    Ogre::UavBufferPacked *origin = nullptr;
    const Ogre::TextureGpu *atlas = nullptr;
    unsigned rect[4] = {};
    bool valid = false;
    float viewProj[16] = {};
    uint32_t lastFrame = 0u;
    bool seen = false;
    /// THE RECEIVER GRID (stage 4b's counting door): its request, its tiles, its counts.
    Ogre::UavBufferPacked *recvParams = nullptr, *recvGrid = nullptr;
    /// The box job's group count, an indirect dispatch's three words (BUGS-7: a count set on the job is
    /// a permutation property, and the instance count it follows grows every frame of a load — GpuCull::groups).
    Ogre::UavBufferPacked *recvGroups = nullptr;
    uint32_t recvGridWords = 0u;   ///< the grid's words: every level of its pyramid
};
/// A PYRAMID ITS MAP DID NOT RENDER FOR this many frames is freed (decision 16.3): the sun removed, the
/// shadows or the Atom draw turned off, a tier's atlas rebuilt - the transition frees its VRAM at once,
/// from the scene's per-frame step (sweepCasterPyramids), not 120 frames later and only when a map renders.
constexpr uint32_t kPyramidIdleFrames = 2u;

/// A SCENE'S caster pass state (atom/AtomComponent.h): its stats ring, its test door, the sun maps'
/// pyramids (by node and map) and the measuring door's list (shadow.casterPbsProbe).
struct AtomCasterScene {
    CasterRing ring;
    ProbeState probe;
    std::map<std::pair<const void *, uint32_t>, LightPyramid> pyramids;
    uint32_t sweptAt = ~0u;
    GpuCull pbsProbe;
};

namespace {
HlmsAtom *atomHlms() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    return hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
}
/// The device's caster pass state, or null where Atom's Hlms is not registered.
AtomCasterDevice *casterDev() {
    HlmsAtom *a = atomHlms();
    return a ? &a->casterDevice() : nullptr;
}

void logOnce(const std::string &what) {
    if (AtomCasterDevice *d = casterDev()) {
        if (what == d->lastLog) return;
        d->lastLog = what;
    }
    Ogre::LogManager::getSingleton().logMessage("Jahshaka atom caster pass: " + what, Ogre::LML_CRITICAL);
}

void destroyPyramid(LightPyramid &p) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    if (p.tex && rs && rs->getTextureGpuManager()) rs->getTextureGpuManager()->destroyTexture(p.tex);
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    for (Ogre::UavBufferPacked **b : { &p.origin, &p.recvParams, &p.recvGrid, &p.recvGroups }) {
        if (*b && vao) vao->destroyUavBuffer(*b);
        *b = nullptr;
    }
    p.recvGridWords = 0u;
    p.tex = nullptr;
    p.valid = false;
}

/// THE MAP'S PYRAMID (LightPyramid), made or remade as the map's rectangle asks; null on the map's
/// first frame (nothing to predict with yet, nothing made) or where the device refuses it.
void sweepPyramids(AtomCasterScene &state, uint32_t frame) {
    if (state.sweptAt == frame) return;
    state.sweptAt = frame;
    for (auto it = state.pyramids.begin(); it != state.pyramids.end();) {
        if (frame - it->second.lastFrame > kPyramidIdleFrames) {
            destroyPyramid(it->second);
            it = state.pyramids.erase(it);
        } else {
            ++it;
        }
    }
}

LightPyramid *lightPyramidFor(Ogre::RenderSystem *rs, AtomCasterScene &state, const void *node, uint32_t map,
                              const Ogre::TextureGpu *atlas, const unsigned rect[4], uint32_t frame) {
    sweepPyramids(state, frame);
    LightPyramid &p = state.pyramids[std::make_pair(node, map)];
    const bool consecutive = p.seen && (p.lastFrame == frame || p.lastFrame + 1u == frame);
    p.seen = true;
    p.lastFrame = frame;
    if (rect[2] == 0u || rect[3] == 0u) return nullptr;
    const bool same = p.tex && p.atlas == atlas && std::equal(rect, rect + 4, p.rect);
    if (!same && p.tex) destroyPyramid(p);
    if (!p.tex) {
        if (!consecutive) return nullptr;
        Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
        Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
        if (!tm || !vao) return nullptr;
        // LEVEL 0 AT HALF THE MAP'S RESOLUTION (ATOM-4 stage 2a): the farthest of each 2 x 2 (JahHzbBuild_cs's
        // depthAt) — the full pyramid's level 1, a quarter of the texels and the bytes (a 2048 map: 32.5 -> 8.1
        // MiB over its chain), the late cut's test conservative at any level. An odd-sized rectangle keeps the
        // full resolution: its halves would not land on whole texels of the cull's rectangle.
        const bool half = (rect[2] % 2u) == 0u && (rect[3] % 2u) == 0u;
        const unsigned w0 = half ? rect[2] / 2u : rect[2], h0 = half ? rect[3] / 2u : rect[3];
        unsigned levels = 1u;
        for (unsigned m = std::max(w0, h0); m > 1u; m >>= 1u) ++levels;
        if (levels > kHzbMaxLevels) return nullptr;
        AtomCasterDevice *dev = casterDev();
        const uint32_t id = dev ? ++dev->pyramidIds : 0u;
        JAH_TRY {
            // A UAV the cull samples (no NotTexture): GPU-written only, never uploaded.
            p.tex = tm->createTexture("JahLightHzb/" + std::to_string(id) + "/" + std::to_string(uintptr_t(&p)),
                                      Ogre::GpuPageOutStrategy::Discard,
                                      Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
            p.tex->setPixelFormat(Ogre::PFG_R32_FLOAT);
            p.tex->setResolution(w0, h0);
            p.tex->setNumMipmaps(Ogre::uint8(levels));
            p.tex->scheduleTransitionTo(Ogre::GpuResidency::Resident);
            const uint32_t corner[4] = { rect[0], rect[1], half ? 1u : 0u, 0u };   // z: mip 0 at half resolution
            p.origin = vao->createUavBuffer(1u, sizeof(corner), 0, const_cast<uint32_t *>(corner), false);
        }
        catch (const Ogre::Exception &e) {
            logOnce(std::string("a sun map's depth pyramid could not be made (") + e.getDescription() +
                    "): the map is culled by its frustum only");
            destroyPyramid(p);
            return nullptr;
        }
        p.atlas = atlas;
        std::copy(rect, rect + 4, p.rect);
        p.valid = false;
    }
    return &p;
}

void destroyProbe(const CasterPipelines &C, ProbeState &p) {
    if (!C.dev) return;
    if (p.memory) {
        vkUnmapMemory(C.dev, p.memory);
        vkFreeMemory(C.dev, p.memory, nullptr);
    }
    if (p.buffer) vkDestroyBuffer(C.dev, p.buffer, nullptr);
    if (p.late.memory) {
        vkUnmapMemory(C.dev, p.late.memory);
        vkFreeMemory(C.dev, p.late.memory, nullptr);
    }
    if (p.late.buffer) vkDestroyBuffer(C.dev, p.late.buffer, nullptr);
    p = ProbeState();
}

bool makeProbeBuffer(const CasterPipelines &C, Ogre::VulkanDevice *device, VkBuffer &buffer, VkDeviceMemory &memory,
                     const uint32_t *&mapped);
bool ensureProbeBuffer(const CasterPipelines &C, Ogre::VulkanDevice *device, ProbeState &p) {
    return p.buffer || makeProbeBuffer(C, device, p.buffer, p.memory, p.mapped);
}
bool makeProbeBuffer(const CasterPipelines &C, Ogre::VulkanDevice *device, VkBuffer &bufferOut, VkDeviceMemory &memoryOut,
                     const uint32_t *&mappedOut) {
    struct { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; const uint32_t *mapped = nullptr; } pp;
    auto &p = pp;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kCountBytes + VkDeviceSize(kProbeRecords) * 16u;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(C.dev, &bi, nullptr, &p.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(C.dev, p.buffer, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(device->mPhysicalDevice, &props);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void *m = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(C.dev, &ai, nullptr, &p.memory) != VK_SUCCESS ||
        vkBindBufferMemory(C.dev, p.buffer, p.memory, 0) != VK_SUCCESS ||
        vkMapMemory(C.dev, p.memory, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) {
        if (p.memory) vkFreeMemory(C.dev, p.memory, nullptr);
        if (p.buffer) vkDestroyBuffer(C.dev, p.buffer, nullptr);
        return false;
    }
    p.mapped = static_cast<const uint32_t *>(m);
    bufferOut = p.buffer;
    memoryOut = p.memory;
    mappedOut = p.mapped;
    return true;
}

/// The armed map's cut, copied right after it was recorded (the list is reused by the
/// next map of the frame).
void captureProbe(const AtomPassContext &ctx, const CasterPipelines &C, Ogre::VulkanDevice *device, ProbeState &p,
                  GpuCull &cull, uint32_t frame, bool late = false) {
    if (late ? (!p.late.buffer && !makeProbeBuffer(C, device, p.late.buffer, p.late.memory, p.late.mapped))
             : !ensureProbeBuffer(C, device, p))
        return;
    const VkBuffer dstBuffer = late ? p.late.buffer : p.buffer;
    VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize srcOff = 0;
    atomBufferOf(cull.count(), src, srcOff);
    VkBufferCopy c{};
    c.srcOffset = srcOff;
    c.dstOffset = 0;
    c.size = kCountBytes;
    vkCmdCopyBuffer(cmd, src, dstBuffer, 1, &c);
    const uint32_t budget = std::min(cull.cutRecordBudget(), kProbeRecords);
    (late ? p.late.recordBudget : p.recordBudget) = budget;
    atomBufferOf(cull.cutRecords(), src, srcOff);
    c.srcOffset = srcOff;
    c.dstOffset = kCountBytes;
    c.size = VkDeviceSize(budget) * 16u;
    vkCmdCopyBuffer(cmd, src, dstBuffer, 1, &c);
    VkMemoryBarrier hb{};
    hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                         nullptr);
    if (late) {
        p.late.captured = true;
        return;
    }
    p.captured = true;
    p.armed = false;
    p.frame = frame;
    p.late.captured = false;
}

void reapGrave(AtomCasterDevice &d, uint32_t frame) {
    for (size_t i = 0; i < d.grave.size();) {
        if (frame - d.grave[i].second > 8u) {
            destroyRing(d.C, d.grave[i].first);
            d.grave[i] = d.grave.back();
            d.grave.pop_back();
        } else {
            ++i;
        }
    }
}

/// THE RING DOUBLES, mid-frame (CASTER-USES-1): a bigger ring is made, the columns this
/// frame already wrote are copied into it (in the command buffer, after their own
/// copies), and the old one goes to the grave. The slots of the frames still in flight
/// wrote the OLD buffer, so their stats are skipped once rather than read from the new.
bool growRing(const AtomPassContext &ctx, AtomCasterDevice &d, Ogre::VulkanDevice *device, CasterRing &r,
              uint32_t frame) {
    if (r.columns >= kMaxUses) return false;
    CasterRing bigger;
    if (!ensureRing(d.C, device, bigger, r.slots, std::min(r.columns * 2u, kMaxUses))) return false;
    const uint32_t s = frame % r.slots;
    VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
    if (r.uses[s]) {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0,
                             nullptr, 0, nullptr);
        VkBufferCopy c{};
        c.srcOffset = VkDeviceSize(s) * r.columns * kUseBytes;
        c.dstOffset = VkDeviceSize(s) * bigger.columns * kUseBytes;
        c.size = VkDeviceSize(r.uses[s]) * kUseBytes;
        vkCmdCopyBuffer(cmd, r.buffer, bigger.buffer, 1, &c);
        VkMemoryBarrier hb{};
        hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                             nullptr);
    }
    bigger.frame = r.frame;
    bigger.usesThisFrame = r.usesThisFrame;
    bigger.uses[s] = r.uses[s];
    bigger.written[s] = r.written[s];
    bigger.writtenAt[s] = r.writtenAt[s];
    bigger.lastTriangles = r.lastTriangles;
    for (uint32_t i = 0; i < 8u; ++i) {
        bigger.tags[i] = r.tags[i];
        bigger.dilations[i] = r.dilations[i];
    }
    bigger.peakUses = r.peakUses;
    bigger.unrecorded = r.unrecorded;
    d.grave.emplace_back(r, frame);
    r = bigger;
    return true;
}

/// THE FIRST USE OF A FRAME reads the slot this frame reuses (written `slots` frames
/// ago: retired) — every use of that frame, summed into the scene's caster stats, and
/// each use's words told to the list (an overflow grows its budget before this frame's
/// first cut). Returns the use's column for this frame (growing the ring when the frame
/// needs one more), or kNoUse when it cannot (past kMaxUses, or no ring: the use then
/// records no copy and is counted).
uint32_t beginUse(const AtomPassContext &ctx, AtomCasterDevice &d, Ogre::VulkanDevice *device, Ogre::VaoManager *vao,
                  AtomComponent &atom, CasterRing &r) {
    const uint32_t frame = vao->getFrameCount();
    if (!ensureRing(d.C, device, r, uint32_t(vao->getDynamicBufferMultiplier()) + 1u)) return kNoUse;
    const uint32_t s = frame % r.slots;
    if (r.frame != frame) {
        reapGrave(d, frame);
        r.frame = frame;
        r.usesThisFrame = 0u;
        if (r.written[s] && frame - r.writtenAt[s] >= r.slots) {
            GpuCull &list = atom.casterCull();
            AtomComponent::CasterStats cs;
            cs.valid = true;
            cs.maps = r.uses[s];
            cs.indexBudget = list.cutIndexBudget();
            GpuCull &late = atom.casterCullLate();
            r.lastTriangles.assign(r.uses[s], 0ull);
            r.peakUses = std::max(r.peakUses, unsigned(r.uses[s]));
            cs.peakMaps = r.peakUses;
            cs.unrecorded = r.unrecorded;
            cs.perMap.resize(r.uses[s]);
            for (uint32_t u = 0; u < r.uses[s]; ++u) {
                const uint32_t *w = r.mapped + (size_t(s) * r.columns + u) * GpuCull::kCountElements * kUseBlocks;
                const uint32_t *l = w + GpuCull::kCountElements * kBlockLate;
                const uint32_t *pa = w + GpuCull::kCountElements * kBlockPbsAll;
                const uint32_t *pv = w + GpuCull::kCountElements * kBlockPbsVisible;
                // THE CUT'S WORDS (GpuCull.h's count layout): [0] survivors, [4] triangles,
                // [8] drawn-cluster records, [11] indices reserved, [12] coarse, [13] nothing,
                // [16] rejected by the depth test.
                cs.instances += w[0] + l[0];
                cs.triangles += w[4] + l[4];
                cs.clusters += std::min(w[8], list.cutRecordBudget()) + std::min(l[8], late.cutRecordBudget());
                cs.overflow += w[12] + l[12];
                cs.missing += w[13] + l[13];
                cs.occluded += w[16];
                cs.lateInstances += l[0];
                cs.lateOccluded += l[16];
                cs.lateTriangles += l[4];
                r.lastTriangles[u] = w[4] + l[4];
                list.noteCutOverflow(w[12] + w[13], w[11], w[8]);
                if (l[11] || l[8]) late.noteCutOverflow(l[12] + l[13], l[11], l[8]);
                AtomCasterMapStats &m = cs.perMap[u];
                const uint32_t tag = u < r.tags[s].size() ? r.tags[s][u] : 0u;
                m.kind = (tag >> 16) & 0xFFu;
                m.face = (tag >> 8) & 0xFFu;
                m.map = tag & 0xFFu;
                m.lightTested = (tag >> 24) & 1u;
                m.instances = w[0] + l[0];
                m.triangles = w[4] + l[4];
                m.occluded = w[16];
                m.lateInstances = l[0];
                m.lateOccluded = l[16];
                m.lateTriangles = l[4];
                m.pbsInstances = pa[0];
                m.pbsTriangles = pa[4];
                m.pbsVisibleInstances = pv[0];
                m.pbsVisibleTriangles = pv[4];
                m.receiverTested = (tag >> 25) & 1u;
                if (u < r.dilations[s].size()) {
                    m.receiverDilation[0] = r.dilations[s][u][0];
                    m.receiverDilation[1] = r.dilations[s][u][1];
                }
            }
            atom.setCasterStats(cs);
            r.written[s] = false;
        }
        r.uses[s] = 0u;
    }
    if (r.usesThisFrame >= r.columns && !growRing(ctx, d, device, r, frame)) {
        ++r.unrecorded;
        return kNoUse;
    }
    // THE COLUMN STARTS AT ZERO: a block no cut of this use writes (a map with no late half, the
    // measuring door shut) reads as nothing, never as the counters of a frame `slots` ago.
    {
        VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
        vkCmdFillBuffer(cmd, r.buffer, (VkDeviceSize(s) * r.columns + r.usesThisFrame) * kUseBytes, kUseBytes, 0u);
    }
    if (r.tags[s].size() <= r.usesThisFrame) r.tags[s].resize(r.usesThisFrame + 1u, 0u);
    r.tags[s][r.usesThisFrame] = 0u;
    if (r.dilations[s].size() <= r.usesThisFrame) r.dilations[s].resize(r.usesThisFrame + 1u, {0.0f, 0.0f});
    r.dilations[s][r.usesThisFrame] = {0.0f, 0.0f};
    return r.usesThisFrame++;
}

/// After the use's cut: its counters into the ring's column (the next cut's reset is a
/// transfer write that waits for this read).
void endUseOf(const AtomPassContext &ctx, CasterRing &r, Ogre::UavBufferPacked *counts, uint32_t frame,
              uint32_t use, UseBlock block);
void endUse(const AtomPassContext &ctx, CasterRing &r, GpuCull &cull, uint32_t frame, uint32_t use,
            UseBlock block = kBlockFirst) {
    endUseOf(ctx, r, cull.count(), frame, use, block);
}
void endUseOf(const AtomPassContext &ctx, CasterRing &r, Ogre::UavBufferPacked *counts, uint32_t frame,
              uint32_t use, UseBlock block) {
    if (use == kNoUse || use >= r.columns || !r.buffer || !counts) return;
    const uint32_t s = frame % r.slots;
    VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    // TRANSFER_WRITE too: the column's clear (beginUse) before this copy into it.
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize srcOff = 0;
    atomBufferOf(counts, src, srcOff);
    VkBufferCopy c{};
    c.srcOffset = srcOff;
    c.dstOffset = (VkDeviceSize(s) * r.columns + use) * kUseBytes + VkDeviceSize(block) * kCountBytes;
    c.size = kCountBytes;
    vkCmdCopyBuffer(cmd, src, r.buffer, 1, &c);
    VkMemoryBarrier hb{};
    hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                         nullptr);
    r.uses[s] = std::max(r.uses[s], use + 1u);
    r.written[s] = true;
    r.writtenAt[s] = frame;
}

/// THE PASS'S DRAW MATRIX (HlmsPbs::preparePassHash's for the rendering camera): the
/// RS-depth projection, its y row negated where the pass requires texture flipping,
/// times the camera's view.
Ogre::Matrix4 drawMatrixOf(const Ogre::Camera *cam, const Ogre::RenderPassDescriptor *rpd) {
    Ogre::Matrix4 proj = cam->getProjectionMatrixWithRSDepth();
    if (rpd->requiresTextureFlipping())
        for (int c = 0; c < 4; ++c) proj[1][c] = -proj[1][c];
    return proj * cam->getVrViewMatrix(0);
}

/// The scene pass this caster pass follows (OgreShadow.cpp emits them as a pair on one
/// target): its camera is the map's shadow camera (CompositorShadowNode::
/// postInitializePass gives it to scene passes only).
Ogre::CompositorPassScene *scenePassBefore(Ogre::CompositorPass *pass) {
    const Ogre::CompositorPassVec &passes = pass->getParentNode()->_getPasses();
    for (size_t i = 1; i < passes.size(); ++i) {
        if (passes[i] != pass) continue;
        Ogre::CompositorPass *prev = passes[i - 1];
        if (prev->getType() != Ogre::PASS_SCENE) return nullptr;
        if (prev->getDefinition()->mShadowMapIdx != pass->getDefinition()->mShadowMapIdx) return nullptr;
        return static_cast<Ogre::CompositorPassScene *>(prev);
    }
    return nullptr;
}

// ---- THE RECEIVER GRID (stage 4b, decisions 15 and 17) ----------------------------------------------------------
/// JahReceiverGrid_cs's and the cut's receiver request, std430: 4 + 4 matrix rows, 6 planes, five vec4-sized lanes.
struct RecvParams {
    float    invViewProjRow[16] = {};
    float    lightRow[16] = {};
    float    planes[24] = {};
    uint32_t dims[4] = {};
    uint32_t map[4] = {};
    float    misc[4] = {};
    uint32_t counts[4] = {};
    uint32_t rect[4] = {};
};
static_assert(sizeof(RecvParams) == 304, "JahReceiverGrid_cs's request layout");
/// The tile: 8 x 8 texels of the map (the premise measured 8 / 16 / 32: within 0.3x of each other).
constexpr unsigned kReceiverTileTexels = 8u;

Ogre::DescriptorSetUav::BufferSlot uavOf(Ogre::UavBufferPacked *b, Ogre::ResourceAccess::ResourceAccess a) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = b;
    slot.offset = 0;
    slot.sizeBytes = 0;
    slot.access = a;
    return slot;
}

/// THE MAP'S RECEIVER GRID of this frame (into `lp.recvGrid`, its request into `lp.recvParams`): the view's id depth
/// projected into the map and every stock-route instance's box in the camera's frustum, each tile keeping the
/// farthest receiver depth; the request also carries the cut's dilation and depth margin. False = no grid this
/// frame (the cut then has no receiver test).
bool buildReceiverGrid(const AtomPassContext &ctx, Ogre::RenderSystem *rs, OgreScene *scene, LightPyramid &lp,
                       const AtomReceiverDepth &rd,
                       const Ogre::Camera *viewCam, const Ogre::Matrix4 &cullM, unsigned mapW, unsigned mapH,
                       const float dilationTexels[2], float depthMargin, bool boxes = true) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsCompute *hc = root && root->getHlmsManager() ? root->getHlmsManager()->getComputeHlms() : nullptr;
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    if (!hc || !vao || !rd.depth || !viewCam) return false;
    Ogre::HlmsComputeJob *depthJob = hc->findComputeJobNoThrow("Jahshaka/ReceiverDepth");
    Ogre::HlmsComputeJob *boxJob = hc->findComputeJobNoThrow("Jahshaka/ReceiverBoxes");
    Ogre::HlmsComputeJob *reduceJob = hc->findComputeJobNoThrow("Jahshaka/ReceiverReduce");
    GpuScene &gs = scene->gpuScene();
    if (!depthJob || !boxJob || !reduceJob || !gs.instanceBuffer()) return false;
    const unsigned tile = kReceiverTileTexels;
    const uint32_t tx = (mapW + tile - 1u) / tile, ty = (mapH + tile - 1u) / tile;
    // THE PYRAMID'S LEVELS, one after the other in the grid's buffer.
    unsigned levels = 1u;
    uint32_t words = 0u;
    for (uint32_t w = tx, h = ty;; ++levels) {
        words += w * h;
        if (w == 1u && h == 1u) break;
        w = std::max(1u, w >> 1u);
        h = std::max(1u, h >> 1u);
    }
    JAH_TRY {
        if (lp.recvGridWords != words) {
            if (lp.recvGrid) vao->destroyUavBuffer(lp.recvGrid);
            lp.recvGrid = vao->createUavBuffer(words, 4u, 0, nullptr, false);
            lp.recvGridWords = words;
        }
        if (!lp.recvParams) lp.recvParams = vao->createUavBuffer(1u, sizeof(RecvParams), 0, nullptr, false);
        if (!lp.recvGroups) lp.recvGroups = vao->createUavBuffer(3u, 4u, 0, nullptr, false);
        RecvParams p;
        const Ogre::Matrix4 inv = rd.drawMatrix.inverse();
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                p.invViewProjRow[r * 4 + c] = float(inv[r][c]);
                p.lightRow[r * 4 + c] = float(cullM[r][c]);
            }
        GpuCullRequest fr;
        fillCullFrustum(viewCam, float(std::max(1u, rd.rect[3])), fr);
        std::memcpy(p.planes, fr.planes, sizeof(p.planes));
        p.dims[0] = rd.rect[2];
        p.dims[1] = rd.rect[3];
        p.dims[2] = tx;
        p.dims[3] = ty;
        p.map[0] = mapW;
        p.map[1] = mapH;
        p.map[2] = tile;
        p.misc[0] = rs->isReverseDepth() ? 0.0f : 1.0f;
        p.misc[1] = dilationTexels[0];
        p.misc[2] = depthMargin;
        p.misc[3] = dilationTexels[1];
        p.counts[0] = gs.slotCount();
        p.counts[1] = 0u;            // every instance the id pass does not draw - hidden ones too (a superset)
        p.counts[2] = kGpuAtom;
        p.counts[3] = levels;        // the pyramid's levels (the cut's mip pick)
        std::copy(rd.rect, rd.rect + 4, p.rect);
        lp.recvParams->upload(&p, 0, 1u);
        {
            // EMPTY, on the device (every level: the reductions rewrite all but level 0 anyway).
            VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
            VkBuffer buf = VK_NULL_HANDLE;
            VkDeviceSize off = 0;
            atomBufferOf(lp.recvGrid, buf, off);
            VkMemoryBarrier pre{};
            pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            pre.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0,
                                 nullptr, 0, nullptr);
            vkCmdFillBuffer(cmd, buf, off, VkDeviceSize(words) * 4u, 0xFFFFFFFFu);
            VkMemoryBarrier post{};
            post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            post.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            post.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &post, 0,
                                 nullptr, 0, nullptr);
        }
        const auto dispatch = [&](Ogre::HlmsComputeJob *job) {
            Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
            job->analyzeBarriers(rt);
            rs->executeResourceTransition(rt);
            hc->dispatch(job, nullptr, nullptr);
        };
        {
            Ogre::DescriptorSetTexture2::TextureSlot ts = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
            ts.texture = rd.depth;
            depthJob->setTexture(0u, ts);
            depthJob->_setUavBuffer(0u, uavOf(lp.recvParams, Ogre::ResourceAccess::Read));
            depthJob->_setUavBuffer(1u, uavOf(lp.recvGrid, Ogre::ResourceAccess::ReadWrite));
            depthJob->setNumThreadGroups((rd.rect[2] + 7u) / 8u, (rd.rect[3] + 7u) / 8u, 1u);
            dispatch(depthJob);
            depthJob->setTexture(0u, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
            for (Ogre::uint8 u = 0u; u < 2u; ++u) depthJob->_setUavBuffer(u, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        }
        {
            boxJob->_setUavBuffer(0u, uavOf(lp.recvParams, Ogre::ResourceAccess::Read));
            boxJob->_setUavBuffer(1u, uavOf(lp.recvGrid, Ogre::ResourceAccess::ReadWrite));
            boxJob->_setUavBuffer(2u, uavOf(gs.instanceBuffer(), Ogre::ResourceAccess::Read));
            // one workgroup an instance, capped at the device's 65,535 (OgreScreenProbeGather.cpp's rule): a workgroup
            // strides the slots past it (JahReceiverGrid_cs, mode 1) — an INDIRECT count, never the job's own
            // (lp.recvGroups: the job's count would be a permutation per scene size)
            const uint32_t groups[3] = { std::min(65535u, std::max(1u, gs.slotCount())), 1u, 1u };
            lp.recvGroups->upload(groups, 0, 3u);
            boxJob->setIndirectDispatchBuffer(lp.recvGroups, 0);
            // (the measuring door shadow.receiverBoxesOff skips it: the fixture's negative control, decision 22.3)
            if (boxes) dispatch(boxJob);
            for (Ogre::uint8 u = 0u; u < 3u; ++u) boxJob->_setUavBuffer(u, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
            boxJob->setIndirectDispatchBuffer(0);
        }
        // THE FARTHEST-RECEIVER PYRAMID, a level a dispatch (a 2048 map's 256 x 256 tiles: eight small ones).
        reduceJob->_setUavBuffer(0u, uavOf(lp.recvParams, Ogre::ResourceAccess::Read));
        reduceJob->_setUavBuffer(1u, uavOf(lp.recvGrid, Ogre::ResourceAccess::ReadWrite));
        Ogre::ShaderParams &rp = reduceJob->getShaderParams("default");
        Ogre::ShaderParams::Param *level = rp.findParameter("recvLevel");
        for (unsigned k = 1u; k < levels && level; ++k) {
            const uint32_t w = std::max(1u, tx >> k), h = std::max(1u, ty >> k);
            level->setManualValue(Ogre::uint32(k));   // a parameter: one compiled shader for every level (decision 19.6)
            rp.setDirty();
            reduceJob->setNumThreadGroups((w + 7u) / 8u, (h + 7u) / 8u, 1u);
            dispatch(reduceJob);
        }
        for (Ogre::uint8 u = 0u; u < 2u; ++u) reduceJob->_setUavBuffer(u, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        return true;
    }
    catch (const Ogre::Exception &e) {
        logOnce(std::string("a sun map's receiver grid did not record (") + e.getDescription() +
                "): the map is culled without its receiver test");
        return false;
    }
}

bool casterGate(const AtomPassContext &ctx) {
    return ctx.scene && ctx.scene->atomDrawOn() && ctx.scene->atom().hasCasters();
}

void recordCasterPass(AtomPassContext &ctx) {
    auto *pass = static_cast<AtomPass *>(ctx.pass);
    if (!pass || !pass->renderPassDesc() || !ctx.sceneManager) return;
    const Ogre::RenderPassDescriptor *rpd = pass->renderPassDesc();
    Ogre::TextureGpu *depthTex = rpd->mDepth.texture;
    Ogre::TextureGpu *colourTex = rpd->getNumColourEntries() ? rpd->mColour[0].texture : nullptr;
    Ogre::VulkanRenderSystem *vkRs = atomVulkanOf(ctx.renderSystem);
    OgreScene *scene = ctx.scene;
    AtomCasterDevice *D = casterDev();
    // THE PASS'S OWN NODE is the shadow node (the scene manager's current one is set by
    // the pass that updates it, which is the same node — this one does not depend on it).
    const auto *shadowNode = dynamic_cast<const Ogre::CompositorShadowNode *>(pass->getParentNode());
    Ogre::CompositorPassScene *scenePass = scenePassBefore(pass);
    Ogre::Camera *cam = scenePass ? scenePass->getCamera() : nullptr;
    if (!depthTex || !vkRs || !scene || !D || !shadowNode || !cam) return;
    CasterPipelines &C = D->C;
    AtomComponent &atom = scene->atom();
    GpuScene &gs = scene->gpuScene();
    if (!gs.live()) return;
    const uint32_t mapIdx = pass->getDefinition()->mShadowMapIdx;
    const Ogre::Light *light = shadowNode->getLightAssociatedWith(mapIdx);
    if (!light) return;
    const bool point = light->getType() == Ogre::Light::LT_POINT;
    if (point != (colourTex != nullptr)) {
        logOnce("a map's target does not match its light (a point face needs its colour): nothing drawn");
        return;
    }
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    CasterPipelineKey key;
    key.colour = colourTex ? Ogre::VulkanMappings::get(colourTex->getPixelFormat()) : VK_FORMAT_UNDEFINED;
    key.depth = Ogre::VulkanMappings::get(depthTex->getPixelFormat());
    key.cullFront = rpd->requiresTextureFlipping();
    key.depthClamp = cam->getNeedsDepthClamp();
    const VkPipeline pipeline = pipelineFor(C, vkRs, key);
    if (!pipeline) return;
    // THE TWO-SIDED RANGE'S PIPELINE (ATOM-TWO-SIDED-1): the same state culling nothing —
    // a CULL_NONE material's stock caster draws both faces too. WARMED with its one-sided
    // twin (the first map of each key creates both: one small depth-only pipeline through
    // the device's pipeline cache, its time in the Ogre log), so the first frame that
    // gains a two-sided caster compiles nothing. A refused one draws the one-sided range
    // alone (pipelineFor logs it once) — never the map's whole Atom set dropped.
    // ...AND THE MASKED RANGES' PAIR (ENGINE-POLISH-ATOM-1 stage 3), warmed the same way: the cut-outs' casters,
    // culled as the pass culls (2) and with no cull (3). rangePipeline[r] draws range r, null = not drawn.
    VkPipeline rangePipeline[GpuCull::kRanges] = { pipeline, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    for (unsigned rg = 1u; rg < GpuCull::kRanges; ++rg) {
        CasterPipelineKey k = key;
        k.cullNone = (rg & 1u) != 0u;
        if (k.cullNone) k.cullFront = false;
        k.masked = rg >= 2u;
        const VkPipeline warmed = pipelineFor(C, vkRs, k);
        if (atom.hasCasterRange(rg)) rangePipeline[rg] = warmed;
    }

    // THE CUBE FACE: the scene pass turned the camera to its face for its own length and
    // back (CompositorPassScene::execute); this pass turns it the same way for its own.
    const Ogre::CompositorPassSceneDef *sceneDef = scenePass->getDefinition();
    const Ogre::Quaternion oldOrientation = cam->getOrientation();
    const bool reorient = sceneDef->mCameraCubemapReorient;
    if (reorient) {
        const Ogre::uint32 face = std::min<Ogre::uint32>(sceneDef->getRtIndex(), 5u);
        cam->setOrientation(oldOrientation * AtomPass::cubemapRotation(face));
    }

    // THE MAP'S RECTANGLE (the shadow node forced the pass's viewport to it).
    const Ogre::CompositorPassDef::ViewportRect &vpRect = pass->getDefinition()->mVpRect[0];
    const uint32_t tw = uint32_t(depthTex->getWidth()), th = uint32_t(depthTex->getHeight());
    const unsigned rect[4] = { unsigned(int(vpRect.mVpLeft * float(tw))), unsigned(int(vpRect.mVpTop * float(th))),
                               unsigned(int(vpRect.mVpWidth * float(tw))),
                               unsigned(int(vpRect.mVpHeight * float(th))) };

    // ---- (1) THE LIGHT'S CUT -------------------------------------------------------
    GpuCullRequest req;
    // THE MAP'S OWN HEIGHT in texels: the rule's footprint is one texel of this map.
    fillCullFrustum(cam, float(std::max(1u, rect[3])), req);
    // THE SIDE PLANES ONLY (SPEED-VR-MEM fix round): a shadow camera draws with DEPTH
    // CLAMP (Camera::getNeedsDepthClamp, the pipeline key above), so a caster between the
    // light and the near plane still writes the near depth and shadows what lies under it.
    // The depth pair (planes 4 and 5, fillCullFrustum's order) culled such a caster at
    // INSTANCE level - its shadow went missing while the stock caster drew it
    // (shadow.atom_parity's high casters). Opened here, never in the shader: the cut's
    // per-cluster test reads the side planes only already.
    for (int p = 4; p < 6; ++p) {
        req.planes[p * 4 + 0] = 0.0f;
        req.planes[p * 4 + 1] = 0.0f;
        req.planes[p * 4 + 2] = 0.0f;
        req.planes[p * 4 + 3] = 1.0f;
    }
    req.flagsRequired = kGpuVisible | kGpuAtomCaster | kGpuCaster;
    // THE NODE KIND'S CHANNEL (shadowCasterChannels): a probe-kind node (the probes)
    // draws the still world only.
    const Ogre::IdString nodeName = pass->getParentNode()->getName();
    const bool probeKind = nodeName == Ogre::IdString(OgreView::kProbeShadowNodeName);
    req.flagsForbidden = probeKind ? kGpuMover : 0u;
    req.hzbLevels = 0u;
    // ONE TEXEL, never the view's pixel-error row (ATOM-3 stage 1): the map's texel is already
    // the tier's lever (shadowResolution / shadowFilter), so the row would count it twice.
    req.pixelTolerance = kLodBudgetPixels * scene->lodBias();
    req.lodHysteresis = 0.0f;
    req.mode = 3u;
    const Ogre::Matrix4 vpm = drawMatrixOf(cam, rpd);
    const Ogre::Vector3 eye = cam->getDerivedPosition();
    const float biasScale = cam->_getConstantBiasScale();
    Ogre::Real nearD = 0, farD = 1;
    shadowNode->getMinMaxDepthRange(mapIdx, nearD, farD);
    if (reorient) cam->setOrientation(oldOrientation);
    float frameViewProj[16];
    std::memcpy(frameViewProj, req.viewProj, sizeof(frameViewProj));

    // ---- (1') THE LIGHT'S OWN DEPTH (ENGINE-UPGRADE-U5 stage 4, the lead's decision 10) -------
    // A SUN MAP of a view's or a mirror's node keeps a farthest-depth pyramid of itself (its
    // rectangle of the atlas, built after its casters are drawn; LIGHT space, so the camera's
    // motion moves only the map's own bounds, which the history's matrix carries). The first cut
    // tests against the PREVIOUS frame's pyramid with the matrix THAT one was drawn with - a
    // prediction only: what it rejects wrongly the late cut (4) draws. Not a spot or point map (a
    // perspective map per face: not measured here) nor the probes' node (re-rendered on demand,
    // its history would always be old).
    const Ogre::Matrix4 cullM = pyramidCullMatrix(vpm);
    OgreEngine *engine = scene->mEngine;
    // shadow.lightDepth 0: no map is a sun map here - no pyramid made or built, no late cut, no receiver grid.
    const bool sunMap = light->getType() == Ogre::Light::LT_DIRECTIONAL && !point && !reorient && !probeKind &&
                        (!engine || engine->armRegistry().on(ArmId::ShadowLightDepth));
    const bool cullOn = !engine || engine->armRegistry().on(ArmId::ShadowCasterCull);
    const bool probeOn = engine && engine->armRegistry().on(ArmId::ShadowCasterPbsProbe);
    AtomCasterScene &state = atom.casterState();
    LightPyramid *lp = sunMap ? lightPyramidFor(ctx.renderSystem, state, shadowNode, mapIdx, depthTex, rect,
                                                vkRs->getVaoManager()->getFrameCount())
                              : nullptr;
    const bool firstTests = lp && lp->tex && lp->valid && cullOn;
    if (firstTests) {
        std::memcpy(req.viewProj, lp->viewProj, sizeof(req.viewProj));
        req.hzbLevels = lp->tex->getNumMipmaps();
        req.hzbRect[0] = 0u;
        req.hzbRect[1] = 0u;
        req.hzbRect[2] = lp->tex->getWidth();    // the pyramid's mip 0 (half the map's: stage 2a)
        req.hzbRect[3] = lp->tex->getHeight();
    }

    GpuCull &cull = atom.casterCull();
    Ogre::VaoManager *vao = vkRs->getVaoManager();
    CasterRing &ring = state.ring;
    const uint32_t use = beginUse(ctx, *D, device, vao, atom, ring);
    {
        // (0) THE LIST'S WRITE-AFTER-READ: the previous use's draw (this frame's last
        // map, or the last frame's) read the commands, the stream and the tables.
        VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 0, nullptr);
    }
    std::string err;
    gs.flushClusterTables();
    if (!gs.clusterBuffer() || !gs.groupBuffer()) return;
    // ---- (1'') THE RECEIVER TEST (stage 4b, decision 17) ---------------------------------------------------------
    // A caster cluster changes the picture only where its shadow can fall on a receiver the camera sees: this map's
    // receiver grid is built from the view's id depth of THIS frame (the id pass runs before every scene pass, so
    // before the shadow node) and the boxes of everything the id pass does not draw; both halves of the cut test
    // their clusters against it. Only for the workspace whose id pass recorded that depth (a capture's or another
    // shape's view has its own), never a view without an id pass (a VR headset's: its maps keep the frustum and
    // light-depth cull), and off with the scene's door (Scene::setShadowReceiverCull, the parity arm).
    OgreScene::CullReceivers receivers;
    bool receiversOn = false;
    float recvDilation[2] = {0.0f, 0.0f};
    if (lp && scene->shadowReceiverCull()) {
        OgreView *rv = atomViewOfPass(pass);
        const AtomReceiverDepth *rd = rv ? &rv->atomReceiverDepth() : nullptr;
        if (rd && rd->depth && rd->frame == vao->getFrameCount() && rd->workspace == pass->getParentNode()->getWorkspace() &&
            rv->camera()) {
            // THE DILATION, PER AXIS (decision 19.1: a focused split's window is not square - map 2's x and y rows
            // differ): the widest PCF kernel's reach (6 x 6: 3 texels), the receivers' normal offset (the shader
            // moves a receiver up to normalOffsetBias / the map's width along its normal - Ogre's
            // getNormalOffsetBias) in each axis' own texels and one tile. The offset's reach along the light is
            // the depth margin.
            const float offsetWorld = float(shadowNode->getNormalOffsetBias(mapIdx)) / float(std::max(1u, rect[2]));
            const Ogre::Vector3 r0(cullM[0][0], cullM[0][1], cullM[0][2]), r1(cullM[1][0], cullM[1][1], cullM[1][2]),
                r2(cullM[2][0], cullM[2][1], cullM[2][2]);
            const float dilation[2] = {
                3.0f + offsetWorld * float(r0.length()) * 0.5f * float(rect[2]) + float(kReceiverTileTexels),
                3.0f + offsetWorld * float(r1.length()) * 0.5f * float(rect[3]) + float(kReceiverTileTexels) };
            const float margin = offsetWorld * float(r2.length());
            // THE GRID'S OWN ROW under the measuring door shadow.casterPbsProbe (decision 17's cost bar).
            std::optional<monitor::CacheScope> row;
            if (engine && engine->armRegistry().on(ArmId::ShadowCasterPbsProbe))
                row.emplace(CacheKind::Cull, WorkReason::Camera, 0, "atom.caster.receivers", ctx.renderSystem);
            const bool boxesOff = engine && engine->armRegistry().on(ArmId::ShadowReceiverBoxesOff);
            receiversOn = buildReceiverGrid(ctx, ctx.renderSystem, scene, *lp, *rd, rv->camera(), cullM, rect[2],
                                            rect[3], dilation, margin, !boxesOff);
            if (receiversOn) {
                recvDilation[0] = dilation[0];
                recvDilation[1] = dilation[1];
            }
            receivers.params = lp->recvParams;
            receivers.grid = lp->recvGrid;
        }
    }
    if (!scene->recordGpuCull(cull, req, firstTests ? lp->tex : nullptr, err, nullptr, nullptr,
                              OgreScene::CullRows::Caster, receiversOn ? &receivers : nullptr)) {
        logOnce("the caster cut did not record (" + err + ")");
        return;
    }
    const uint32_t frameNow = vao->getFrameCount();
    endUse(ctx, ring, cull, frameNow, use);
    const Ogre::uint32 face = reorient ? std::min<Ogre::uint32>(sceneDef->getRtIndex(), 5u) : 0u;
    if (use != kNoUse && use < ring.tags[frameNow % std::max(ring.slots, 1u)].size()) {
        const uint32_t kind = probeKind ? 2u : nodeName == Ogre::IdString(OgreView::kReflectShadowNodeName) ? 1u : 0u;
        ring.tags[frameNow % ring.slots][use] = (firstTests ? 1u << 24 : 0u) | (receiversOn ? 1u << 25 : 0u) |
                                                (kind << 16) | ((face & 0xFFu) << 8) | (mapIdx & 0xFFu);
        if (use < ring.dilations[frameNow % ring.slots].size())
            ring.dilations[frameNow % ring.slots][use] = {recvDilation[0], recvDilation[1]};
    }
    // THE TEST DOOR: the armed map of the view kind's node, once (its FIRST cut: the request and
    // the drawn set of the light's cut at this frame's matrix).
    {
        ProbeState &p = state.probe;
        if (p.armed && p.map == mapIdx && p.face == face && nodeName == Ogre::IdString(OgreView::kShadowNodeName)) {
            OgreScene::CasterProbe &info = p.info;
            info = OgreScene::CasterProbe();
            info.map = mapIdx;
            info.face = face;
            info.orthographic = req.orthographic;
            for (int i = 0; i < 3; ++i) info.eye[i] = req.eye[i];
            info.projScaleY = req.projScaleY;
            info.viewportHeight = req.viewportHeight;
            info.tolerance = req.pixelTolerance;
            info.biasScale = biasScale;
            info.depthNear = float(nearD);
            info.depthFar = float(farD);
            std::memcpy(info.viewProj, frameViewProj, sizeof(info.viewProj));
            std::memcpy(info.planes, req.planes, sizeof(info.planes));
            std::copy(rect, rect + 4, info.rect);
            captureProbe(ctx, C, device, p, cull, frameNow);
        }
    }

    // ---- (2) THE EDGES THE SOLVER CANNOT EXPRESS, (3) OGRE'S RENDER PASS, OUR PIPELINE ----
    // One cut's draw: the compute -> indirect edge, the pass's attachments begun (LOADED: the scene
    // pass before drew the PBS casters, a first cut before a late one), the pipeline, the map's
    // viewport, the push constants and the two ranges.
    const auto drawCut = [&](GpuCull &list) -> bool {
        VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
        {
            VkMemoryBarrier mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
            mb.dstAccessMask =
                VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                     VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
        if (!pass->beginRenderPass()) return false;
        cmd = ctx.graphicsFrameCmd("an atom caster");
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        ctx.boundRawState = true;
        {
            VkViewport v{};
            v.x = float(rect[0]);
            v.y = float(rect[1]);
            v.width = float(rect[2]);
            v.height = float(rect[3]);
            v.minDepth = 0.0f;
            v.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &v);
            VkRect2D sc{};
            sc.offset = { int32_t(vpRect.mVpScissorLeft * float(tw)), int32_t(vpRect.mVpScissorTop * float(th)) };
            sc.extent = { uint32_t(vpRect.mVpScissorWidth * float(tw)), uint32_t(vpRect.mVpScissorHeight * float(th)) };
            vkCmdSetScissor(cmd, 0, 1, &sc);
        }
        {
            CasterPushConstants pc{};
            for (int rr = 0; rr < 4; ++rr)
                for (int c = 0; c < 4; ++c) pc.viewProjRow[rr * 4 + c] = float(vpm[rr][c]);
            pc.eyeBias[0] = float(eye.x);
            pc.eyeBias[1] = float(eye.y);
            pc.eyeBias[2] = float(eye.z);
            pc.eyeBias[3] = biasScale;
            // HlmsPbs::preparePassHash's caster depthRange: near, 1 / (far - near).
            pc.depthRange[0] = float(nearD);
            pc.depthRange[1] = float(1.0 / double(farD - nearD));
            pc.depthRange[2] = point ? 1.0f : 0.0f;
            atomAddressOf(C.dev, C.bufferDeviceAddress, gs.instanceBuffer(), pc.instances);
            atomAddressOf(C.dev, C.bufferDeviceAddress, gs.meshBuffer(), pc.meshes);
            atomAddressOf(C.dev, C.bufferDeviceAddress, gs.geomBuffer(), pc.rows);
            const uint64_t masks = gs.maskTableAddress();
            pc.maskTable[0] = uint32_t(masks & 0xFFFFFFFFu);
            pc.maskTable[1] = uint32_t(masks >> 32u);
            vkCmdPushConstants(cmd, C.layout, kCasterPushStages, 0, sizeof(pc), &pc);
        }
        {
            VkBuffer ib = VK_NULL_HANDLE;
            VkDeviceSize ibOff = 0;
            atomBufferOf(list.cutStream(), ib, ibOff);
            vkCmdBindIndexBuffer(cmd, ib, ibOff, VK_INDEX_TYPE_UINT32);
            VkBuffer drawBuf = VK_NULL_HANDLE, countBuf = VK_NULL_HANDLE;
            VkDeviceSize drawOff = 0, countOff = 0;
            atomBufferOf(list.draws(), drawBuf, drawOff);
            atomBufferOf(list.count(), countBuf, countOff);
            const uint32_t maxDraws = std::min(list.capacity(), gs.slotCount());
            C.drawIndexedIndirectCount(cmd, drawBuf, drawOff, countBuf, countOff, maxDraws,
                                        GpuCull::kDrawWords * sizeof(uint32_t));
            for (unsigned rg = 1u; rg < GpuCull::kRanges; ++rg) {
                if (!rangePipeline[rg]) continue;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rangePipeline[rg]);
                C.drawIndexedIndirectCount(
                    cmd, drawBuf, drawOff + VkDeviceSize(list.rangeFirst(rg)) * GpuCull::kDrawWords * sizeof(uint32_t),
                    countBuf, countOff + GpuCull::rangeCountOffsetBytes(rg), maxDraws,
                    GpuCull::kDrawWords * sizeof(uint32_t));
            }
        }
        return true;
    };
    // A LIST'S WRITE-AFTER-READ before a cut records into it: whatever draw or copy read it last.
    const auto listWar = [&]() {
        VkCommandBuffer cmd = ctx.graphicsFrameCmd("an atom caster");
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                             0, nullptr, 0, nullptr);
    };
    if (!drawCut(cull)) return;

    // ---- (4) THE LIGHT'S OWN DEPTH: the pyramid of this map, and the late cut -------------
    // The map now holds the PBS casters and the first cut's survivors. Its pyramid (farthest depth,
    // the id pass's build over the map's rectangle) is the NEXT frame's prediction and THIS frame's
    // proof: the late cut tests exactly the instances the first one rejected by depth against it,
    // with this frame's matrix, and draws the ones it cannot prove hidden. A rejected instance it
    // also rejects lies wholly behind depth that is in the final map (the map only gains fragments
    // after this, and a depth-only draw keeps the nearest whatever the order), so it could have
    // written no texel: the map is the frustum-only map texel for texel (shadow.caster_cull).
    if (lp && lp->tex) {
        bool built = false;
        {
            // THE BUILD'S OWN ROW under the measuring door (decision 16.6: one pyramid build per map measured).
            std::optional<monitor::CacheScope> row;
            if (probeOn) row.emplace(CacheKind::Cull, WorkReason::Camera, 0, "atom.caster.pyramid", ctx.renderSystem);
            built = recordOcclusionPyramidRect(ctx.renderSystem, depthTex, lp->tex, lp->origin);
        }
        if (built && firstTests) {
            GpuCull &lateList = atom.casterCullLate();
            lateList.followCutBudget(cull);
            GpuCullRequest lateReq = req;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) lateReq.viewProj[r * 4 + c] = float(cullM[r][c]);
            listWar();
            if (!scene->recordGpuCull(lateList, lateReq, lp->tex, err, nullptr, &cull, OgreScene::CullRows::Caster,
                                      receiversOn ? &receivers : nullptr)) {
                logOnce("the caster's late cut did not record (" + err + ")");
            } else {
                endUse(ctx, ring, lateList, frameNow, use, kBlockLate);
                {
                    ProbeState &pr = state.probe;
                    if (pr.captured && !pr.late.captured && pr.frame == frameNow && pr.map == mapIdx && pr.face == face &&
                        nodeName == Ogre::IdString(OgreView::kShadowNodeName))
                        captureProbe(ctx, C, device, pr, lateList, frameNow, true);
                }
                drawCut(lateList);
            }
        }
        // THE MEASURING DOOR (shadow.casterPbsProbe): the stock casters of this map, counted in its
        // frustum and against its final pyramid. Draws nothing.
        if (built && probeOn) {
            GpuCullRequest pr = req;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) pr.viewProj[r * 4 + c] = float(cullM[r][c]);
            pr.flagsRequired = kGpuVisible | kGpuCaster;
            pr.flagsForbidden = kGpuAtomCaster;   // the casters the STOCK pass draws (kStockCasterBit's set)
            pr.mode = 2u;
            // ITS OWN PYRAMID REQUEST (decision 16.4: it inherited the first cut's, 0 with the cull off).
            pr.hzbLevels = lp->tex->getNumMipmaps();
            pr.hzbRect[0] = 0u;
            pr.hzbRect[1] = 0u;
            pr.hzbRect[2] = lp->tex->getWidth();
            pr.hzbRect[3] = lp->tex->getHeight();
            GpuCull &probeList = state.pbsProbe;
            listWar();
            if (scene->recordGpuCull(probeList, pr, nullptr, err, nullptr, nullptr, OgreScene::CullRows::Caster)) {
                endUse(ctx, ring, probeList, frameNow, use, kBlockPbsAll);
                listWar();
                if (scene->recordGpuCull(probeList, pr, lp->tex, err, nullptr, nullptr,
                                         OgreScene::CullRows::Caster))
                    endUse(ctx, ring, probeList, frameNow, use, kBlockPbsVisible);
            }
        }
        // THE HISTORY: this frame's pyramid, read next frame with the matrix it was drawn with.
        lp->valid = built;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) lp->viewProj[r * 4 + c] = float(cullM[r][c]);
    }

    // THE RENDERER'S COUNTERS SEE THIS DRAW TOO (the id pass's rule): this use's share,
    // from the last read (exact for a still scene, a few frames behind a moving one).
    if (use < ring.lastTriangles.size()) {
        Ogre::RenderingMetrics m;
        m.mIsRecordingMetrics = true;
        m.mBatchCount = 1u;
        m.mDrawCount = 1u;
        m.mFaceCount = size_t(ring.lastTriangles[use]);
        m.mVertexCount = size_t(ring.lastTriangles[use]) * 3u;
        ctx.renderSystem->_addMetrics(m);
    }
}

}  // namespace

void registerAtomCasterPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomCasterPassId, [](AtomPassContext &ctx) { recordCasterPass(ctx); });
        p->setGate(kAtomCasterPassId, [](const AtomPassContext &ctx) { return casterGate(ctx); });
    }
}

/// After every scene (whose rings went to the device's window as each scene died) and
/// before Root: one idle device, then the window and the pipelines.
void releaseAtomCasterPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomCasterPassId, AtomPassRecorder());
        p->setGate(kAtomCasterPassId, AtomPassGate());
    }
    AtomCasterDevice *d = casterDev();
    if (!d) return;
    CasterPipelines &C = d->C;
    if (C.dev) {
        vkDeviceWaitIdle(C.dev);
        for (auto &g : d->grave) destroyRing(C, g.first);
        d->grave.clear();
        destroyPipelines(C);
        if (C.layout) vkDestroyPipelineLayout(C.dev, C.layout, nullptr);
    }
    C = CasterPipelines();
}

void AtomCasterSceneDelete::operator()(AtomCasterScene *p) const {
    if (!p) return;
    AtomCasterDevice *d = casterDev();
    if (d) {
        // THE SUN MAPS' PYRAMIDS and the measuring door's list: Ogre's own objects, whose
        // destruction Ogre defers past the frames still reading them.
        for (auto &kv : p->pyramids) destroyPyramid(kv.second);
        p->pyramids.clear();
        p->pbsProbe.destroy();
        // THE TEST DOOR'S BUFFER: a suite's teardown, never a frame's — behind an idle device.
        if (p->probe.buffer) {
            if (d->C.dev) vkDeviceWaitIdle(d->C.dev);
            destroyProbe(d->C, p->probe);
        }
        // A copy into the ring may still be in flight: to the device's window, reaped once
        // its last frame has retired.
        if (p->ring.buffer) {
            Ogre::Root *root = Ogre::Root::getSingletonPtr();
            Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
            const uint32_t frame = rs && rs->getVaoManager() ? rs->getVaoManager()->getFrameCount() : 0u;
            d->grave.emplace_back(p->ring, frame);
        }
    }
    delete p;
}

void AtomCasterDeviceDelete::operator()(AtomCasterDevice *p) const { delete p; }

AtomCasterDevice &HlmsAtom::casterDevice() {
    if (!mCasterDevice) mCasterDevice.reset(new AtomCasterDevice());
    return *mCasterDevice;
}

AtomCasterScene &AtomComponent::casterState() {
    if (!mCasterState) mCasterState.reset(new AtomCasterScene());
    return *mCasterState;
}

void sweepCasterPyramids(AtomCasterScene *state) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    if (!state || !rs || !rs->getVaoManager()) return;
    sweepPyramids(*state, rs->getVaoManager()->getFrameCount());
}

void warmCasterPassJobs(OgreScene *scene, Ogre::RenderSystem *rs, const Ogre::Camera *camera, Ogre::TextureGpu *depth,
                        Ogre::TextureGpu *hzb) {
    if (!scene || !rs || !camera || !depth || !hzb || !rs->getVaoManager()) return;
    Ogre::VaoManager *vao = rs->getVaoManager();
    AtomPassContext ctx;
    ctx.renderSystem = rs;
    ctx.scene = scene;
    ctx.sceneManager = scene->sceneManager();
    LightPyramid lp;
    const float kWarmDilation[2] = { 3.0f + float(kReceiverTileTexels), 3.0f + float(kReceiverTileTexels) };
    JAH_TRY {
        // THE RECTANGLE'S PYRAMID BUILD (Jahshaka/HzbBuildRect: one permutation for every map since it no longer keys
        // on the textures' sizes): the view's pyramid from the view's depth at a zero corner.
        lp.origin = vao->createUavBuffer(1u, 16u, 0, nullptr, false);
        const uint32_t corner[4] = { 0u, 0u, 0u, 0u };
        lp.origin->upload(corner, 0, 1u);
        recordOcclusionPyramidRect(rs, depth, hzb, lp.origin);
        // THE RECEIVER GRID'S THREE JOBS (its params, its pyramid of tiles) over the view's depth, a map of the
        // pyramid's size seen through the camera's own matrix...
        AtomReceiverDepth rd;
        rd.depth = depth;
        rd.rect[2] = depth->getWidth();
        rd.rect[3] = depth->getHeight();
        const Ogre::Matrix4 m = camera->getProjectionMatrixWithRSDepth() * camera->getViewMatrix(true);
        rd.drawMatrix = m;
        rs->endRenderPassDescriptor();
        if (buildReceiverGrid(ctx, rs, scene, lp, rd, camera, m, hzb->getWidth(), hzb->getHeight(), kWarmDilation, 0.0f)) {
            // ...and the cut's receiver test, into a list of its own that nothing draws.
            GpuCull scratch;
            GpuCullRequest req;
            fillCullFrustum(camera, float(std::max(1u, rd.rect[3])), req);
            req.flagsRequired = kGpuVisible | kGpuAtomCaster | kGpuCaster;   // the caster cut's own set (its request above)
            req.mode = 3u;
            OgreScene::CullReceivers receivers;
            receivers.params = lp.recvParams;
            receivers.grid = lp.recvGrid;
            std::string err;
            scene->recordGpuCull(scratch, req, nullptr, err, nullptr, nullptr, OgreScene::CullRows::Caster, &receivers);
        }
    }
    catch (const Ogre::Exception &e) {
        logOnce(std::string("the caster jobs' warm-up did not record (") + e.getDescription() + ")");
    }
    destroyPyramid(lp);
}

void OgreScene::armCasterProbeForTest(unsigned map, unsigned face) {
    ProbeState &p = mAtom.casterState().probe;
    p.armed = true;
    p.captured = false;
    p.map = map;
    p.face = face;
}

bool OgreScene::casterProbeForTest(CasterProbe &out) {
    AtomCasterScene *state = mAtom.casterStateIfAny();
    if (!state || !state->probe.captured || !state->probe.mapped) return false;
    ProbeState &p = state->probe;
    Ogre::VaoManager *vao = mRoot && mRoot->getRenderSystem() ? mRoot->getRenderSystem()->getVaoManager() : nullptr;
    if (!vao || vao->getFrameCount() - p.frame <= uint32_t(vao->getDynamicBufferMultiplier()) + 1u) return false;
    out = p.info;
    const uint32_t *w = p.mapped;
    out.survivors = w[0];
    out.triangles = w[4];
    out.overflow = w[12];
    out.missing = w[13];
    const uint32_t records = std::min(w[8], p.recordBudget);
    out.drawn.reserve(records);
    for (uint32_t r = 0; r < records; ++r) {
        const uint32_t *rec = p.mapped + GpuCull::kCountElements + size_t(r) * 4u;
        OgreScene::CasterProbe::Drawn d{};
        d.slot = rec[0];
        d.depth = rec[3];
        // THE MESH-LOCAL CLUSTER: the record names the global one (the mesh's base + c).
        GpuSceneEntry e;
        uint32_t base = 0u;
        if (gpuSceneEntry(rec[0], e) && e.meshIndex != 0xFFFFFFFFu)
            base = mGpuScene.meshEntry(e.meshIndex).dag[0];
        d.cluster = rec[1] - base;
        out.drawn.push_back(d);
    }
    // THE LATE HALF's drawn set, when the map ran one that frame (stage 4).
    out.lateDrawn.clear();
    if (p.late.captured && p.late.mapped) {
        const uint32_t *lw = p.late.mapped;
        const uint32_t lateRecords = std::min(lw[8], p.late.recordBudget);
        for (uint32_t r = 0; r < lateRecords; ++r) {
            const uint32_t *rec = p.late.mapped + GpuCull::kCountElements + size_t(r) * 4u;
            OgreScene::CasterProbe::Drawn d{};
            d.slot = rec[0];
            d.depth = rec[3];
            GpuSceneEntry e;
            uint32_t base = 0u;
            if (gpuSceneEntry(rec[0], e) && e.meshIndex != 0xFFFFFFFFu) base = mGpuScene.meshEntry(e.meshIndex).dag[0];
            d.cluster = rec[1] - base;
            out.lateDrawn.push_back(d);
        }
    }
    return true;
}

#else   // !JAH_RAY_QUERY — no raw Vulkan here (macOS): no shadow node carries the pass.

void OgreScene::armCasterProbeForTest(unsigned, unsigned) {}
void warmCasterPassJobs(OgreScene *, Ogre::RenderSystem *, const Ogre::Camera *, Ogre::TextureGpu *, Ogre::TextureGpu *) {}
void sweepCasterPyramids(AtomCasterScene *) {}
bool OgreScene::casterProbeForTest(CasterProbe &) { return false; }
void registerAtomCasterPass() {}
void releaseAtomCasterPass() {}
struct AtomCasterDevice {};
struct AtomCasterScene {};
void AtomCasterSceneDelete::operator()(AtomCasterScene *p) const { delete p; }
void AtomCasterDeviceDelete::operator()(AtomCasterDevice *p) const { delete p; }
AtomCasterDevice &HlmsAtom::casterDevice() {
    if (!mCasterDevice) mCasterDevice.reset(new AtomCasterDevice());
    return *mCasterDevice;
}
AtomCasterScene &AtomComponent::casterState() {
    if (!mCasterState) mCasterState.reset(new AtomCasterScene());
    return *mCasterState;
}

#endif

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
