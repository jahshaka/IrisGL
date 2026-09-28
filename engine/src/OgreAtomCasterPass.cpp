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
//      kGpuVisible | kGpuAtom | kGpuCaster, and for the probe kind (probes —
//      the still world, shadowCasterChannels) not kGpuMover.
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
// THE PASS'S GATE (AtomPassGate): a scene with no Atom item executes this pass as
// nothing — Ogre's render pass stays open across it, the upstream pass list's cost.
//
// THE PIPELINES: no vertex attributes, push constants only (128 bytes), a render pass
// COMPATIBLE with Ogre's (the map's formats), per (formats, cull face, depth clamp).
// The stock caster's state: the datablock's caster macroblock is the default one for
// every Atom item (atomRouteFor keeps any other on PBS) — back faces culled, FRONT
// where the pass flips (Hlms's InvertCullingMode), depth GREATER_OR_EQUAL under
// reverse-Z with writes, depth CLAMP where the shadow camera asks for it (a
// directional light's maps, OgreCompositorShadowNode.cpp).
#include "EnginePrivate.h"
#include "AtomPass.h"
#include "GpuCull.h"
#include "GpuScene.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <OgreCamera.h>
#include <OgreLight.h>
#include <OgreLogManager.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
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

#include "rayquery/atom_caster_point_frag_spv.h"
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
    uint32_t pad[2] = {};
};
static_assert(sizeof(CasterPushConstants) == 128, "atom_caster.vert/_point.frag's push-constant block");
constexpr VkShaderStageFlags kCasterPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct CasterPipelineKey {
    VkFormat colour = VK_FORMAT_UNDEFINED;   ///< UNDEFINED = depth only (a directional/spot map)
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool cullFront = false;                  ///< the pass flips: Hlms's InvertCullingMode
    bool depthClamp = false;
    bool operator==(const CasterPipelineKey &o) const {
        return colour == o.colour && depth == o.depth && cullFront == o.cullFront && depthClamp == o.depthClamp;
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
CasterPipelines gC;

void logOnce(const std::string &what) {
    static std::string sLast;
    if (what == sLast) return;
    sLast = what;
    Ogre::LogManager::getSingleton().logMessage("Jahshaka atom caster pass: " + what, Ogre::LML_CRITICAL);
}

VkShaderModule makeModule(VkDevice dev, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

void destroyPipelines() {
    if (!gC.dev) return;
    for (CasterPipelines::Entry &e : gC.entries) {
        if (e.pipeline) vkDestroyPipeline(gC.dev, e.pipeline, nullptr);
        if (e.compatible) vkDestroyRenderPass(gC.dev, e.compatible, nullptr);
    }
    gC.entries.clear();
}

/// The pipeline for `key` — built on first use.
VkPipeline pipelineFor(Ogre::VulkanRenderSystem *vkRs, const CasterPipelineKey &key) {
    if (gC.refused) return VK_NULL_HANDLE;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    if (!gC.dev) {
        gC.dev = device->mDevice;
        gC.drawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(
            vkGetDeviceProcAddr(gC.dev, "vkCmdDrawIndexedIndirectCountKHR"));
        gC.bufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            vkGetDeviceProcAddr(gC.dev, "vkGetBufferDeviceAddressKHR"));
        VkPushConstantRange pc{};
        pc.stageFlags = kCasterPushStages;
        pc.offset = 0;
        pc.size = sizeof(CasterPushConstants);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        if (!gC.drawIndexedIndirectCount || !gC.bufferDeviceAddress ||
            vkCreatePipelineLayout(gC.dev, &pl, nullptr, &gC.layout) != VK_SUCCESS) {
            gC.refused = true;
            logOnce("no vkCmdDrawIndexedIndirectCountKHR / vkGetBufferDeviceAddressKHR, or the layout "
                    "failed: the shadow maps draw no Atom caster");
            return VK_NULL_HANDLE;
        }
    }
    for (const CasterPipelines::Entry &e : gC.entries)
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
    if (vkCreateRenderPass(gC.dev, &rp, nullptr, &e.compatible) != VK_SUCCESS) {
        gC.refused = true;
        logOnce("vkCreateRenderPass (the compatible pass) failed");
        return VK_NULL_HANDLE;
    }

    VkShaderModule vs = makeModule(gC.dev, katomCaster_vertSpv, sizeof(katomCaster_vertSpv));
    VkShaderModule fs = point ? makeModule(gC.dev, katomCasterPoint_fragSpv, sizeof(katomCasterPoint_fragSpv))
                              : VK_NULL_HANDLE;
    if (!vs || (point && !fs)) {
        if (vs) vkDestroyShaderModule(gC.dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(gC.dev, fs, nullptr);
        vkDestroyRenderPass(gC.dev, e.compatible, nullptr);
        gC.refused = true;
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
    rast.cullMode = key.cullFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
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
    gp.stageCount = point ? 2u : 1u;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rast;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = gC.layout;
    gp.renderPass = e.compatible;
    gp.subpass = 0;
    const VkResult r = vkCreateGraphicsPipelines(gC.dev, device->mPipelineCache, 1, &gp, nullptr, &e.pipeline);
    vkDestroyShaderModule(gC.dev, vs, nullptr);
    if (fs) vkDestroyShaderModule(gC.dev, fs, nullptr);
    if (r != VK_SUCCESS) {
        vkDestroyRenderPass(gC.dev, e.compatible, nullptr);
        gC.refused = true;
        logOnce("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    gC.entries.push_back(e);
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
constexpr VkDeviceSize kUseBytes = GpuCull::kCountElements * sizeof(uint32_t);

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
};
std::unordered_map<const OgreScene *, CasterRing> gRings;
std::vector<std::pair<CasterRing, uint32_t>> gGrave;

void destroyRing(CasterRing &r) {
    if (!gC.dev) return;
    if (r.memory) {
        vkUnmapMemory(gC.dev, r.memory);
        vkFreeMemory(gC.dev, r.memory, nullptr);
    }
    if (r.buffer) vkDestroyBuffer(gC.dev, r.buffer, nullptr);
    r = CasterRing();
}

bool ensureRing(Ogre::VulkanDevice *device, CasterRing &r, uint32_t slots, uint32_t columns = kFirstUses) {
    if (r.buffer) return true;
    slots = std::min(std::max(slots, 2u), 8u);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kUseBytes * columns * slots;
    // TRANSFER_SRC: a growth copies this frame's columns out of it.
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gC.dev, &bi, nullptr, &r.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gC.dev, r.buffer, &req);
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
    if (type == UINT32_MAX || vkAllocateMemory(gC.dev, &ai, nullptr, &r.memory) != VK_SUCCESS ||
        vkBindBufferMemory(gC.dev, r.buffer, r.memory, 0) != VK_SUCCESS ||
        vkMapMemory(gC.dev, r.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        destroyRing(r);
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
};
std::unordered_map<const OgreScene *, ProbeState> gProbes;

void destroyProbe(ProbeState &p) {
    if (!gC.dev) return;
    if (p.memory) {
        vkUnmapMemory(gC.dev, p.memory);
        vkFreeMemory(gC.dev, p.memory, nullptr);
    }
    if (p.buffer) vkDestroyBuffer(gC.dev, p.buffer, nullptr);
    p = ProbeState();
}

bool ensureProbeBuffer(Ogre::VulkanDevice *device, ProbeState &p) {
    if (p.buffer) return true;
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kUseBytes + VkDeviceSize(kProbeRecords) * 16u;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gC.dev, &bi, nullptr, &p.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gC.dev, p.buffer, &req);
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
    if (type == UINT32_MAX || vkAllocateMemory(gC.dev, &ai, nullptr, &p.memory) != VK_SUCCESS ||
        vkBindBufferMemory(gC.dev, p.buffer, p.memory, 0) != VK_SUCCESS ||
        vkMapMemory(gC.dev, p.memory, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) {
        if (p.memory) vkFreeMemory(gC.dev, p.memory, nullptr);
        if (p.buffer) vkDestroyBuffer(gC.dev, p.buffer, nullptr);
        p.buffer = VK_NULL_HANDLE;
        p.memory = VK_NULL_HANDLE;
        return false;
    }
    p.mapped = static_cast<const uint32_t *>(m);
    return true;
}

/// The armed map's cut, copied right after it was recorded (the list is reused by the
/// next map of the frame).
void captureProbe(Ogre::VulkanDevice *device, ProbeState &p, GpuCull &cull, uint32_t frame) {
    if (!ensureProbeBuffer(device, p)) return;
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
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
    c.size = kUseBytes;
    vkCmdCopyBuffer(cmd, src, p.buffer, 1, &c);
    p.recordBudget = std::min(cull.cutRecordBudget(), kProbeRecords);
    atomBufferOf(cull.cutRecords(), src, srcOff);
    c.srcOffset = srcOff;
    c.dstOffset = kUseBytes;
    c.size = VkDeviceSize(p.recordBudget) * 16u;
    vkCmdCopyBuffer(cmd, src, p.buffer, 1, &c);
    VkMemoryBarrier hb{};
    hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                         nullptr);
    p.captured = true;
    p.armed = false;
    p.frame = frame;
}

void reapGrave(uint32_t frame) {
    for (size_t i = 0; i < gGrave.size();) {
        if (frame - gGrave[i].second > 8u) {
            destroyRing(gGrave[i].first);
            gGrave[i] = gGrave.back();
            gGrave.pop_back();
        } else {
            ++i;
        }
    }
}

/// THE RING DOUBLES, mid-frame (CASTER-USES-1): a bigger ring is made, the columns this
/// frame already wrote are copied into it (in the command buffer, after their own
/// copies), and the old one goes to the grave. The slots of the frames still in flight
/// wrote the OLD buffer, so their stats are skipped once rather than read from the new.
bool growRing(Ogre::VulkanDevice *device, CasterRing &r, uint32_t frame) {
    if (r.columns >= kMaxUses) return false;
    CasterRing bigger;
    if (!ensureRing(device, bigger, r.slots, std::min(r.columns * 2u, kMaxUses))) return false;
    const uint32_t s = frame % r.slots;
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
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
    bigger.peakUses = r.peakUses;
    bigger.unrecorded = r.unrecorded;
    gGrave.emplace_back(r, frame);
    r = bigger;
    return true;
}

/// THE FIRST USE OF A FRAME reads the slot this frame reuses (written `slots` frames
/// ago: retired) — every use of that frame, summed into the scene's caster stats, and
/// each use's words told to the list (an overflow grows its budget before this frame's
/// first cut). Returns the use's column for this frame (growing the ring when the frame
/// needs one more), or kNoUse when it cannot (past kMaxUses, or no ring: the use then
/// records no copy and is counted).
uint32_t beginUse(Ogre::VulkanDevice *device, Ogre::VaoManager *vao, OgreScene *scene, CasterRing &r) {
    const uint32_t frame = vao->getFrameCount();
    if (!ensureRing(device, r, uint32_t(vao->getDynamicBufferMultiplier()) + 1u)) return kNoUse;
    const uint32_t s = frame % r.slots;
    if (r.frame != frame) {
        reapGrave(frame);
        r.frame = frame;
        r.usesThisFrame = 0u;
        if (r.written[s] && frame - r.writtenAt[s] >= r.slots) {
            GpuCull &list = scene->casterCull();
            OgreScene::CasterStats cs;
            cs.valid = true;
            cs.maps = r.uses[s];
            cs.indexBudget = list.cutIndexBudget();
            r.lastTriangles.assign(r.uses[s], 0ull);
            r.peakUses = std::max(r.peakUses, unsigned(r.uses[s]));
            cs.peakMaps = r.peakUses;
            cs.unrecorded = r.unrecorded;
            for (uint32_t u = 0; u < r.uses[s]; ++u) {
                const uint32_t *w = r.mapped + (size_t(s) * r.columns + u) * GpuCull::kCountElements;
                // THE CUT'S WORDS (GpuCull.h's count layout): [0] survivors, [4] triangles,
                // [8] drawn-cluster records, [11] indices reserved, [12] coarse, [13] nothing.
                cs.instances += w[0];
                cs.triangles += w[4];
                cs.clusters += std::min(w[8], list.cutRecordBudget());
                cs.overflow += w[12];
                cs.missing += w[13];
                r.lastTriangles[u] = w[4];
                list.noteCutOverflow(w[12] + w[13], w[11], w[8]);
            }
            scene->setCasterStats(cs);
            r.written[s] = false;
        }
        r.uses[s] = 0u;
    }
    if (r.usesThisFrame >= r.columns && !growRing(device, r, frame)) {
        ++r.unrecorded;
        return kNoUse;
    }
    return r.usesThisFrame++;
}

/// After the use's cut: its counters into the ring's column (the next cut's reset is a
/// transfer write that waits for this read).
void endUse(Ogre::VulkanDevice *device, CasterRing &r, GpuCull &cull, uint32_t frame, uint32_t use) {
    if (use == kNoUse || use >= r.columns || !r.buffer || !cull.count()) return;
    const uint32_t s = frame % r.slots;
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
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
    c.dstOffset = (VkDeviceSize(s) * r.columns + use) * kUseBytes;
    c.size = kUseBytes;
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

bool casterGate(const AtomPassContext &ctx) {
    OgreScene *scene = atomSceneOf(ctx.sceneManager);
    return scene && scene->atomDrawOn() && scene->hasAtomItems();
}

void recordCasterPass(AtomPassContext &ctx) {
    auto *pass = static_cast<AtomPass *>(ctx.pass);
    if (!pass || !pass->renderPassDesc() || !ctx.sceneManager) return;
    const Ogre::RenderPassDescriptor *rpd = pass->renderPassDesc();
    Ogre::TextureGpu *depthTex = rpd->mDepth.texture;
    Ogre::TextureGpu *colourTex = rpd->getNumColourEntries() ? rpd->mColour[0].texture : nullptr;
    Ogre::VulkanRenderSystem *vkRs = atomVulkanOf(ctx.renderSystem);
    OgreScene *scene = atomSceneOf(ctx.sceneManager);
    // THE PASS'S OWN NODE is the shadow node (the scene manager's current one is set by
    // the pass that updates it, which is the same node — this one does not depend on it).
    const auto *shadowNode = dynamic_cast<const Ogre::CompositorShadowNode *>(pass->getParentNode());
    Ogre::CompositorPassScene *scenePass = scenePassBefore(pass);
    Ogre::Camera *cam = scenePass ? scenePass->getCamera() : nullptr;
    if (!depthTex || !vkRs || !scene || !shadowNode || !cam) return;
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
    const VkPipeline pipeline = pipelineFor(vkRs, key);
    if (!pipeline) return;

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
    req.flagsRequired = kGpuVisible | kGpuAtom | kGpuCaster;
    // THE NODE KIND'S CHANNEL (shadowCasterChannels): a probe-kind node (the probes)
    // draws the still world only.
    const Ogre::IdString nodeName = pass->getParentNode()->getName();
    const bool probeKind = nodeName == Ogre::IdString(OgreView::kProbeShadowNodeName);
    req.flagsForbidden = probeKind ? kGpuMover : 0u;
    req.hzbLevels = 0u;
    req.pixelTolerance = kLodBudgetPixels * scene->lodBias();
    req.lodHysteresis = 0.0f;
    req.mode = 3u;
    const Ogre::Matrix4 vpm = drawMatrixOf(cam, rpd);
    const Ogre::Vector3 eye = cam->getDerivedPosition();
    const float biasScale = cam->_getConstantBiasScale();
    Ogre::Real nearD = 0, farD = 1;
    shadowNode->getMinMaxDepthRange(mapIdx, nearD, farD);
    if (reorient) cam->setOrientation(oldOrientation);

    GpuCull &cull = scene->casterCull();
    Ogre::VaoManager *vao = vkRs->getVaoManager();
    CasterRing &ring = gRings[scene];
    const uint32_t use = beginUse(device, vao, scene, ring);
    {
        // (0) THE LIST'S WRITE-AFTER-READ: the previous use's draw (this frame's last
        // map, or the last frame's) read the commands, the stream and the tables.
        VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
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
    if (!scene->recordGpuCull(cull, req, nullptr, err, false, nullptr, nullptr)) {
        logOnce("the caster cut did not record (" + err + ")");
        return;
    }
    endUse(device, ring, cull, vao->getFrameCount(), use);
    // THE TEST DOOR: the armed map of the view kind's node, once.
    {
        auto pit = gProbes.find(scene);
        const Ogre::uint32 face = reorient ? std::min<Ogre::uint32>(sceneDef->getRtIndex(), 5u) : 0u;
        if (pit != gProbes.end() && pit->second.armed && pit->second.map == mapIdx && pit->second.face == face &&
            nodeName == Ogre::IdString(OgreView::kShadowNodeName)) {
            ProbeState &p = pit->second;
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
            std::memcpy(info.viewProj, req.viewProj, sizeof(info.viewProj));
            std::copy(rect, rect + 4, info.rect);
            captureProbe(device, p, cull, vao->getFrameCount());
        }
    }

    // ---- (2) THE EDGES THE SOLVER CANNOT EXPRESS -----------------------------------
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
    {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // ---- (3) OGRE'S RENDER PASS, OUR PIPELINE ----------------------------------------
    if (!pass->beginRenderPass()) return;
    cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
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
        atomAddressOf(gC.dev, gC.bufferDeviceAddress, gs.instanceBuffer(), pc.instances);
        atomAddressOf(gC.dev, gC.bufferDeviceAddress, gs.meshBuffer(), pc.meshes);
        atomAddressOf(gC.dev, gC.bufferDeviceAddress, gs.geomBuffer(), pc.rows);
        vkCmdPushConstants(cmd, gC.layout, kCasterPushStages, 0, sizeof(pc), &pc);
    }
    {
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize ibOff = 0;
        atomBufferOf(cull.cutStream(), ib, ibOff);
        vkCmdBindIndexBuffer(cmd, ib, ibOff, VK_INDEX_TYPE_UINT32);
        VkBuffer drawBuf = VK_NULL_HANDLE, countBuf = VK_NULL_HANDLE;
        VkDeviceSize drawOff = 0, countOff = 0;
        atomBufferOf(cull.draws(), drawBuf, drawOff);
        atomBufferOf(cull.count(), countBuf, countOff);
        gC.drawIndexedIndirectCount(cmd, drawBuf, drawOff, countBuf, countOff, std::min(cull.capacity(), gs.slotCount()),
                                    GpuCull::kDrawWords * sizeof(uint32_t));
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

void releaseAtomCasterPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomCasterPassId, AtomPassRecorder());
        p->setGate(kAtomCasterPassId, AtomPassGate());
    }
    if (gC.dev) {
        vkDeviceWaitIdle(gC.dev);
        for (auto &kv : gRings) destroyRing(kv.second);
        gRings.clear();
        for (auto &g : gGrave) destroyRing(g.first);
        gGrave.clear();
        for (auto &kv : gProbes) destroyProbe(kv.second);
        gProbes.clear();
        destroyPipelines();
        if (gC.layout) vkDestroyPipelineLayout(gC.dev, gC.layout, nullptr);
    }
    gC = CasterPipelines();
}

void atomCasterPassForgetScene(const OgreScene *scene) {
    {
        auto pit = gProbes.find(scene);
        if (pit != gProbes.end()) {
            if (gC.dev) vkDeviceWaitIdle(gC.dev);   // a test door's buffer: a suite's teardown, never a frame's
            destroyProbe(pit->second);
            gProbes.erase(pit);
        }
    }
    auto it = gRings.find(scene);
    if (it == gRings.end()) return;
    // A copy into the ring may still be in flight: to the grave, reaped once its last
    // frame has retired.
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    const uint32_t frame = rs && rs->getVaoManager() ? rs->getVaoManager()->getFrameCount() : 0u;
    gGrave.emplace_back(it->second, frame);
    gRings.erase(it);
}

void OgreScene::armCasterProbeForTest(unsigned map, unsigned face) {
    ProbeState &p = gProbes[this];
    p.armed = true;
    p.captured = false;
    p.map = map;
    p.face = face;
}

bool OgreScene::casterProbeForTest(CasterProbe &out) {
    auto it = gProbes.find(this);
    if (it == gProbes.end() || !it->second.captured || !it->second.mapped) return false;
    ProbeState &p = it->second;
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
    return true;
}

#else   // !JAH_RAY_QUERY — no raw Vulkan here (macOS): no shadow node carries the pass.

void OgreScene::armCasterProbeForTest(unsigned, unsigned) {}
bool OgreScene::casterProbeForTest(CasterProbe &) { return false; }
void registerAtomCasterPass() {}
void releaseAtomCasterPass() {}
void atomCasterPassForgetScene(const OgreScene *) {}

#endif

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
