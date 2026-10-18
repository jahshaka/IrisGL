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
#include <OgreHlmsManager.h>
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
    bool cullNone = false;                   ///< the two-sided range (ATOM-TWO-SIDED-1): both faces
    bool depthClamp = false;
    bool operator==(const CasterPipelineKey &o) const {
        return colour == o.colour && depth == o.depth && cullFront == o.cullFront && cullNone == o.cullNone &&
               depthClamp == o.depthClamp;
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

    VkShaderModule vs = makeModule(C.dev, katomCaster_vertSpv, sizeof(katomCaster_vertSpv));
    VkShaderModule fs = point ? makeModule(C.dev, katomCasterPoint_fragSpv, sizeof(katomCasterPoint_fragSpv))
                              : VK_NULL_HANDLE;
    if (!vs || (point && !fs)) {
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
        if (key.cullNone) {
            // THE NO-CULL PIPELINE IS OPTIONAL (ATOM-TWO-SIDED-1): refused, the map still draws
            // its one-sided Atom casters — only the two-sided range goes undrawn. Remembered as
            // a null entry, so it is asked of the device once, never per map.
            logOnce("vkCreateGraphicsPipelines failed for the no-cull (two-sided) caster pipeline: "
                    "two-sided Atom items cast no shadow on this device");
            C.entries.push_back(e);
            return VK_NULL_HANDLE;
        }
        C.refused = true;
        logOnce("vkCreateGraphicsPipelines failed");
        return VK_NULL_HANDLE;
    }
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka atom caster pass: pipeline (" + std::string(point ? "point" : "depth") +
            (key.cullNone ? ", no cull" : key.cullFront ? ", front cull" : ", back cull") + ") created in " +
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
enum UseBlock : uint32_t { kBlockFirst = 0u, kBlockLate = 1u, kBlockPbsAll = 2u, kBlockPbsVisible = 3u, kUseBlocks = 4u };
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
};
}  // namespace

/// THE CASTER PASS'S DEVICE STATE (HlmsAtom.h): the pipelines, the rings of scenes that
/// went away (and of a ring that doubled) with the frame they left at — destroyed once
/// that frame has retired, never a device wait — and the last line said.
struct AtomCasterDevice {
    CasterPipelines C;
    std::vector<std::pair<CasterRing, uint32_t>> grave;
    std::string lastLog;
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
};
constexpr uint32_t kPyramidIdleFrames = 120u;

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
    if (p.origin && rs && rs->getVaoManager()) rs->getVaoManager()->destroyUavBuffer(p.origin);
    p.tex = nullptr;
    p.origin = nullptr;
    p.valid = false;
}

/// THE MAP'S PYRAMID (LightPyramid), made or remade as the map's rectangle asks; null on the map's
/// first frame (nothing to predict with yet, nothing made) or where the device refuses it.
LightPyramid *lightPyramidFor(Ogre::RenderSystem *rs, AtomCasterScene &state, const void *node, uint32_t map,
                              const Ogre::TextureGpu *atlas, const unsigned rect[4], uint32_t frame) {
    if (state.sweptAt != frame) {
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
        unsigned levels = 1u;
        for (unsigned m = std::max(rect[2], rect[3]); m > 1u; m >>= 1u) ++levels;
        if (levels > kHzbMaxLevels) return nullptr;
        static uint32_t gPyramidId = 0u;
        JAH_TRY {
            // A UAV the cull samples (no NotTexture): GPU-written only, never uploaded.
            p.tex = tm->createTexture("JahLightHzb/" + std::to_string(++gPyramidId), Ogre::GpuPageOutStrategy::Discard,
                                      Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
            p.tex->setPixelFormat(Ogre::PFG_R32_FLOAT);
            p.tex->setResolution(rect[2], rect[3]);
            p.tex->setNumMipmaps(Ogre::uint8(levels));
            p.tex->scheduleTransitionTo(Ogre::GpuResidency::Resident);
            const uint32_t corner[4] = { rect[0], rect[1], 0u, 0u };
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
    p = ProbeState();
}

bool ensureProbeBuffer(const CasterPipelines &C, Ogre::VulkanDevice *device, ProbeState &p) {
    if (p.buffer) return true;
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
        p.buffer = VK_NULL_HANDLE;
        p.memory = VK_NULL_HANDLE;
        return false;
    }
    p.mapped = static_cast<const uint32_t *>(m);
    return true;
}

/// The armed map's cut, copied right after it was recorded (the list is reused by the
/// next map of the frame).
void captureProbe(const AtomPassContext &ctx, const CasterPipelines &C, Ogre::VulkanDevice *device, ProbeState &p,
                  GpuCull &cull, uint32_t frame) {
    if (!ensureProbeBuffer(C, device, p)) return;
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
    vkCmdCopyBuffer(cmd, src, p.buffer, 1, &c);
    p.recordBudget = std::min(cull.cutRecordBudget(), kProbeRecords);
    atomBufferOf(cull.cutRecords(), src, srcOff);
    c.srcOffset = srcOff;
    c.dstOffset = kCountBytes;
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
    for (uint32_t i = 0; i < 8u; ++i) bigger.tags[i] = r.tags[i];
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
    return r.usesThisFrame++;
}

/// After the use's cut: its counters into the ring's column (the next cut's reset is a
/// transfer write that waits for this read).
void endUse(const AtomPassContext &ctx, CasterRing &r, GpuCull &cull, uint32_t frame, uint32_t use,
            UseBlock block = kBlockFirst) {
    if (use == kNoUse || use >= r.columns || !r.buffer || !cull.count()) return;
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
    atomBufferOf(cull.count(), src, srcOff);
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

bool casterGate(const AtomPassContext &ctx) {
    return ctx.scene && ctx.scene->atomDrawOn() && ctx.scene->atom().hasItems();
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
    VkPipeline pipelineTwoSided = VK_NULL_HANDLE;
    {
        CasterPipelineKey two = key;
        two.cullFront = false;
        two.cullNone = true;
        const VkPipeline warmed = pipelineFor(C, vkRs, two);
        if (atom.hasTwoSidedItems()) pipelineTwoSided = warmed;
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
    const bool sunMap = light->getType() == Ogre::Light::LT_DIRECTIONAL && !point && !reorient && !probeKind;
    OgreEngine *engine = scene->mEngine;
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
        req.hzbRect[2] = rect[2];
        req.hzbRect[3] = rect[3];
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
    if (!scene->recordGpuCull(cull, req, firstTests ? lp->tex : nullptr, err, nullptr, nullptr,
                              OgreScene::CullRows::Caster)) {
        logOnce("the caster cut did not record (" + err + ")");
        return;
    }
    const uint32_t frameNow = vao->getFrameCount();
    endUse(ctx, ring, cull, frameNow, use);
    const Ogre::uint32 face = reorient ? std::min<Ogre::uint32>(sceneDef->getRtIndex(), 5u) : 0u;
    if (use != kNoUse && use < ring.tags[frameNow % std::max(ring.slots, 1u)].size()) {
        const uint32_t kind = probeKind ? 2u : nodeName == Ogre::IdString(OgreView::kReflectShadowNodeName) ? 1u : 0u;
        ring.tags[frameNow % ring.slots][use] =
            (firstTests ? 1u << 24 : 0u) | (kind << 16) | ((face & 0xFFu) << 8) | (mapIdx & 0xFFu);
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
            if (pipelineTwoSided) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineTwoSided);
                C.drawIndexedIndirectCount(
                    cmd, drawBuf,
                    drawOff + VkDeviceSize(list.twoSidedFirst()) * GpuCull::kDrawWords * sizeof(uint32_t),
                    countBuf, countOff + GpuCull::kTwoSidedCountOffsetBytes, maxDraws,
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
        const bool built = recordOcclusionPyramidRect(ctx.renderSystem, depthTex, lp->tex, lp->origin);
        if (built && firstTests) {
            GpuCull &lateList = atom.casterCullLate();
            lateList.followCutBudget(cull);
            GpuCullRequest lateReq = req;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) lateReq.viewProj[r * 4 + c] = float(cullM[r][c]);
            listWar();
            if (!scene->recordGpuCull(lateList, lateReq, lp->tex, err, nullptr, &cull,
                                      OgreScene::CullRows::Caster)) {
                logOnce("the caster's late cut did not record (" + err + ")");
            } else {
                endUse(ctx, ring, lateList, frameNow, use, kBlockLate);
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
            pr.flagsForbidden = kGpuAtom;
            pr.mode = 2u;
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
    return true;
}

#else   // !JAH_RAY_QUERY — no raw Vulkan here (macOS): no shadow node carries the pass.

void OgreScene::armCasterProbeForTest(unsigned, unsigned) {}
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
