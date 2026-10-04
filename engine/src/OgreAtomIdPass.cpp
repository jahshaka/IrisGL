// ATOM S3-DRAW — THE ID PASS (SPECS/atom/D3_S3_DRAW_DESIGN.md §2.1; the stage-0
// spike's P-A, spikes/atom-stage0/src/AtomIdDraw.cpp, made the product's).
//
// WHAT IT RECORDS, inside the chain's `atom_id` PASS_CUSTOM (OgreChain.cpp) and
// after the provider closed Ogre's render pass (AtomPass.h):
//   1. THE CULL — the GPU cull's CUT MODE over the scene's table, into the VIEW's
//      own list (OgreView::atomCull): frustum, the DEPTH PYRAMID (the two-pass
//      occlusion, recordIdPass's note — ATOM-OCCLUSION-1), then THE CLUSTER CUT per (survivor,
//      cluster) at the view's tolerance (one pixel x the scene's LOD bias; no switch
//      band — a frontier change under a pixel is invisible by construction), the
//      drawn clusters' indices compacted into the list's stream and one
//      VkDrawIndexedIndirectCommand per survivor over its run of it
//      (ATOM-CLUSTER-CUT, SPECS/v2/CLUSTER_CUT_DESIGN.md), the predicate "visible
//      and routed to Atom" (kGpuVisible | kGpuAtom). Nothing is read back.
//   2. THE EDGES the barrier solver cannot express — compute write -> indirect read
//      (fork 1bccc3f93+a98e2b0af (was 0032)'s own reason) and the GPU scene's tables, read by the vertex
//      stage through device addresses the solver never sees — as one raw memory
//      barrier; and the write-after-read on the list against the previous frame's
//      draw, as an execution barrier in front of the uploads.
//   3. OGRE'S RENDER PASS for the pass's RTV (the id image + the scene's named
//      depth, AtomPass::beginRenderPass: Ogre's barriers, Ogre's VkRenderPass, the
//      depth CLEARED by its load action), the id image cleared to AtomId::kEmpty
//      (a float clear colour cannot say 0xFFFFFFFF), and ONE
//      vkCmdDrawIndexedIndirectCountKHR over the list with the count the cull
//      wrote (N-1, the fork's VK_KHR_draw_indirect_count request).
// The provider forgets Ogre's pipeline and render-queue caches afterwards
// (boundRawState); the render pass stays open for the next pass to close.
//
// THE PIPELINE is ours: no vertex attributes (atom_id.vert pulls every vertex
// through the mesh's cluster row; the cut's compacted stream is the index buffer, so
// gl_VertexIndex is a real vertex), push constants only (the pass's view-projection
// rows and five device addresses — no descriptor set
// to keep per frame in flight), created against a render pass COMPATIBLE with
// Ogre's (the same two formats, one sample) and rebuilt only when a format does.
// Front face CLOCKWISE and back faces culled, exactly Ogre's Vulkan convention for
// the default macroblock (VulkanRenderSystem's PSO: frontFace CLOCKWISE,
// CULL_CLOCKWISE -> VK_CULL_MODE_BACK_BIT) — and FRONT faces where the pass
// requires texture flipping, Hlms's InvertCullingMode rule; depth test GREATER_OR_EQUAL under
// reverse-Z, write on. A TWO-SIDED material (CULL_NONE, ATOM-TWO-SIDED-1) rides the list's
// two-sided range and a third pipeline that culls nothing: both faces rasterised, the
// nearer one kept by the depth test (a closed mesh's back faces never win it).
#include "EnginePrivate.h"
#include "AtomPass.h"
#include "GpuCull.h"
#include "GpuScene.h"
#include "HlmsAtom.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreRenderSystem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>
#include <cstring>
#include <vector>

#if JAH_RAY_QUERY
#include "OgreVulkanDevice.h"
#include "OgreVulkanMappings.h"
#include "OgreVulkanQueue.h"
#include "OgreVulkanRenderSystem.h"
#include "Vao/OgreVulkanBufferInterface.h"
#include "AtomVulkan.h"

#include "rayquery/atom_id_frag_spv.h"
#include "rayquery/atom_id_vert_spv.h"
#endif

namespace jahshaka {
namespace engine {
namespace detail {

/// THE OCCLUSION'S PYRAMID (EnginePrivate.h): the compositor's own compute-pass discipline
/// (CompositorPassCompute::execute — bind, the job's barriers through Ogre's solver,
/// dispatch) once per level, the level count read off the texture as it is.
bool recordOcclusionPyramid(Ogre::RenderSystem *rs, Ogre::TextureGpu *depth, Ogre::TextureGpu *hzb) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsCompute *hc = root && root->getHlmsManager() ? root->getHlmsManager()->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *seed = hc ? hc->findComputeJobNoThrow("Jahshaka/HzbSeed") : nullptr;
    Ogre::HlmsComputeJob *reduce = hc ? hc->findComputeJobNoThrow("Jahshaka/HzbReduce") : nullptr;
    if (!rs || !depth || !hzb || !seed || !reduce) return false;
    // FARTHEST, in the render system's depth direction (JahHzbReduce_cs; the job's
    // properties, shared with a view's own pyramid — which is farthest wherever this
    // one is built, ChainDesc::atomOcclusion).
    // THE PROPERTIES ARE PUT BACK after the build: a view's own pyramid's compute passes
    // (PostFxDesc::hzb) set them once, at their chain's build, on this same job — a
    // CLOSEST one elsewhere would otherwise be built farthest (chain.hzb caught it).
    const Ogre::int32 reverse = rs->isReverseDepth() ? 1 : 0;
    const Ogre::int32 wasReverse = reduce->getProperty("hzb_reverse_z");
    const Ogre::int32 wasFarthest = reduce->getProperty("hzb_farthest");
    if (wasReverse != reverse) reduce->setProperty("hzb_reverse_z", reverse);
    if (wasFarthest != 1) reduce->setProperty("hzb_farthest", 1);
    auto run = [&](Ogre::HlmsComputeJob *job) {
        Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
        job->analyzeBarriers(rt);
        rs->executeResourceTransition(rt);
        hc->dispatch(job, nullptr, nullptr);
    };
    auto uav = [&](Ogre::uint8 mip, Ogre::ResourceAccess::ResourceAccess access) {
        Ogre::DescriptorSetUav::TextureSlot t = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        t.texture = hzb;
        t.access = access;
        t.mipmapLevel = mip;
        return t;
    };
    rs->endRenderPassDescriptor();
    {
        Ogre::DescriptorSetTexture2::TextureSlot ts = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        ts.texture = depth;
        seed->setTexture(0u, ts);
        seed->_setUavTexture(0u, uav(0u, Ogre::ResourceAccess::Write));
        run(seed);
        seed->setTexture(0u, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        seed->_setUavTexture(0u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
    }
    for (Ogre::uint8 m = 1u; m < hzb->getNumMipmaps(); ++m) {
        reduce->_setUavTexture(0u, uav(m, Ogre::ResourceAccess::Write));
        reduce->_setUavTexture(1u, uav(Ogre::uint8(m - 1u), Ogre::ResourceAccess::Read));
        run(reduce);
    }
    // The jobs' descriptor sets hold raw pointers: no binding outlives this build.
    reduce->_setUavTexture(0u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
    reduce->_setUavTexture(1u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
    if (wasReverse != reverse) reduce->setProperty("hzb_reverse_z", wasReverse);
    if (wasFarthest != 1) reduce->setProperty("hzb_farthest", wasFarthest);
    return true;
}

#if JAH_RAY_QUERY
namespace {

/// The push constants, as atom_id.vert and atom_id.frag declare them (std430
/// push-constant block: four vec4 then six uvec2 — 112 bytes, inside the 128 every
/// device offers). Both stages read the block (the fragment its triangle words).
struct IdPushConstants {
    float    viewProjRow[16] = {};
    uint32_t instances[2] = {};
    uint32_t meshes[2] = {};
    uint32_t rows[2] = {};
    uint32_t triWords[2] = {};
    uint32_t slotBase[2] = {};
    uint32_t pad[2] = {};
};
static_assert(sizeof(IdPushConstants) == 112, "atom_id.vert/.frag's push-constant block");
constexpr VkShaderStageFlags kIdPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

struct IdPipeline {
    VkDevice dev = VK_NULL_HANDLE;
    PFN_vkCmdDrawIndexedIndirectCountKHR drawIndexedIndirectCount = nullptr;
    PFN_vkGetBufferDeviceAddressKHR bufferDeviceAddress = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass compatible = VK_NULL_HANDLE;
    /// [0] culls BACK faces, [1] FRONT faces: the pass that requires texture
    /// flipping gets its y row negated AND its culling inverted (Hlms's own
    /// InvertCullingMode rule, OgreHlms.cpp) — the same pair here. [2] culls NOTHING:
    /// the list's two-sided range (ATOM-TWO-SIDED-1), flipped or not.
    VkPipeline pipeline[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFormat colour = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool refused = false;   ///< a creation failed; said once, the pass records nothing
};
IdPipeline gId;

void logOnce(const std::string &what) {
    static std::string sLast;
    if (what == sLast) return;
    sLast = what;
    Ogre::LogManager::getSingleton().logMessage("Jahshaka atom id pass: " + what, Ogre::LML_CRITICAL);
}

/// A device address as the shader's uvec2 (lo, hi).
void addressOf(Ogre::UavBufferPacked *buf, uint32_t out[2]) {
    atomAddressOf(gId.dev, gId.bufferDeviceAddress, buf, out);
}

/// THE ID PASS'S STATS RING, per view: the cull's count words copied into host-visible
/// memory every frame and read back once the frame that wrote them has retired. An
/// AsyncTicket would submit the command buffer mid-frame (VulkanAsyncTicket's ctor
/// commits it) — measured: +1.0 ms GPU and +0.45 ms CPU on the default scene.
/// TWO HALVES A SLOT (ATOM-OCCLUSION-1): the first list's counters and the LATE list's
/// (the disocclusion pass's), each written by its own pass and read on its own.
struct StatsRing {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const uint32_t *mapped = nullptr;
    uint32_t slots = 0u;
    uint32_t writtenAt[2][8] = {};
    bool written[2][8] = {};
};
std::unordered_map<const OgreView *, StatsRing> gRings;
/// Rings of views that went away, with the frame they left at: destroyed once that
/// frame has retired (a copy into them may still be in flight) — never a device wait.
std::vector<std::pair<StatsRing, uint32_t>> gGrave;

uint32_t currentFrame() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    return rs && rs->getVaoManager() ? rs->getVaoManager()->getFrameCount() : 0u;
}

constexpr VkDeviceSize kStatsHalfBytes = GpuCull::kCountElements * sizeof(uint32_t);
constexpr VkDeviceSize kStatsSlotBytes = 2u * kStatsHalfBytes;

void destroyRing(StatsRing &r) {
    if (!gId.dev) return;
    if (r.memory) {
        vkUnmapMemory(gId.dev, r.memory);
        vkFreeMemory(gId.dev, r.memory, nullptr);
    }
    if (r.buffer) vkDestroyBuffer(gId.dev, r.buffer, nullptr);
    r = StatsRing();
}

bool ensureRing(Ogre::VulkanDevice *device, StatsRing &r, uint32_t slots) {
    if (r.buffer) return true;
    slots = std::min(std::max(slots, 2u), 8u);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kStatsSlotBytes * slots;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(gId.dev, &bi, nullptr, &r.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(gId.dev, r.buffer, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(device->mPhysicalDevice, &props);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    void *p = nullptr;
    if (type == UINT32_MAX || vkAllocateMemory(gId.dev, &ai, nullptr, &r.memory) != VK_SUCCESS ||
        vkBindBufferMemory(gId.dev, r.buffer, r.memory, 0) != VK_SUCCESS ||
        vkMapMemory(gId.dev, r.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        destroyRing(r);
        return false;
    }
    r.mapped = static_cast<const uint32_t *>(p);
    r.slots = slots;
    return true;
}

/// Reads the slot this frame reuses (written `slots` frames ago: retired, since Ogre
/// keeps at most its dynamic-buffer multiplier of frames in flight) into the view,
/// then records this frame's copy of the LAST cull's counters into it — before this
/// frame's request zeroes them.
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

/// `late` names the half: false the first list (the id pass), true the late list (the
/// disocclusion pass, whose read also completes the first's: the cut's counters of the
/// frame are the two lists' sums). `cull` null = this frame's late pass recorded no cull
/// (the first one tested nothing): its half is ZEROED for the frame, so the counters it
/// reads back later describe that frame and not an older one.
void cycleStats(Ogre::VulkanDevice *device, Ogre::VaoManager *vao, OgreView *view, GpuCull *cull,
                bool late) {
    if (!late) reapGrave(vao->getFrameCount());
    if (cull && !cull->count()) return;
    StatsRing &r = gRings[view];
    if (!ensureRing(device, r, uint32_t(vao->getDynamicBufferMultiplier()) + 1u)) return;
    const uint32_t frame = vao->getFrameCount();
    const uint32_t s = frame % r.slots;
    const uint32_t h = late ? 1u : 0u;
    if (r.written[h][s] && frame - r.writtenAt[h][s] >= r.slots) {
        const uint32_t *w = r.mapped + size_t(s) * 2u * GpuCull::kCountElements + h * GpuCull::kCountElements;
        GpuCull &list = late ? view->atomCullLate() : view->atomCull();
        // THE CUT'S WORDS (GpuCull.h's count layout): drawn clusters, indices
        // reserved, instances that did not fit, the pairs evaluated — and the
        // budget grows before the next request when something did not fit.
        // The late half ADDS to the first's (read earlier in this same frame, from the
        // same frame's slot): the cut's counters are the frame's, both lists summed.
        AtomCutStats cs;
        if (late) view->atomCutStats(cs);
        cs.clusters += std::min(w[8], list.cutRecordBudget());
        cs.indices += w[11];
        cs.overflow += w[12];
        cs.missing += w[13];
        cs.overflowIndices += w[11];
        cs.evaluated += w[14];
        if (!late) {
            cs.indexBudget = list.cutIndexBudget();
            view->setAtomStats(w[4], w[0]);
            // No late read follows on a frame-only chain: the late share is nothing.
            if (!view->chainAtomOcclusion()) view->setAtomLateStats(0ull, 0u, 0u, 0u);
        } else {
            // THE OCCLUSION'S WORDS: the late test's rejects are what the frame's depth
            // test kept out (occluded), its survivors what it disoccluded.
            view->setAtomLateStats(w[4], w[0], w[16], w[0]);
        }
        view->setAtomCutStats(cs);
        if (atomTraceOn() && (w[12] || w[13]))
            atomTrace(std::string(late ? "late" : "first") + " list's counters of frame " +
                      std::to_string(r.writtenAt[h][s]) + ": coarse " + std::to_string(w[12]) + ", missing " +
                      std::to_string(w[13]) + ", indices " + std::to_string(w[11]) + ", records " +
                      std::to_string(w[8]) + ", budget " + std::to_string(list.cutIndexBudget()));
        list.noteCutOverflow(w[12] + w[13], w[11], w[8]);
        // THE SCENE'S HIGH-WATER MARK: every later view of this scene is born at it,
        // and both lists of a view are sized by it (the late list can hold the whole
        // frame the frame after a cut: an overflow would draw coarse, never exact).
        if (OgreScene *sc = view->ogreScene()) sc->noteCutIndexNeed(w[11]);
    }
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
    const VkDeviceSize dst = VkDeviceSize(s) * kStatsSlotBytes + VkDeviceSize(h) * kStatsHalfBytes;
    if (cull) {
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        VkBuffer src = VK_NULL_HANDLE;
        VkDeviceSize srcOff = 0;
        atomBufferOf(cull->count(), src, srcOff);
        VkBufferCopy c{};
        c.srcOffset = srcOff;
        c.dstOffset = dst;
        c.size = kStatsHalfBytes;
        vkCmdCopyBuffer(cmd, src, r.buffer, 1, &c);
    } else {
        vkCmdFillBuffer(cmd, r.buffer, dst, kStatsHalfBytes, 0u);
    }
    // ...and the request's reset (a transfer write) waits for this read; the host
    // reads the slot only after the frame's fence, which makes the write visible.
    VkMemoryBarrier hb{};
    hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0,
                         nullptr);
    r.written[h][s] = true;
    r.writtenAt[h][s] = frame;
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

void destroyPipelineObjects() {
    if (!gId.dev) return;
    for (VkPipeline &p : gId.pipeline) {
        if (p) vkDestroyPipeline(gId.dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (gId.compatible) vkDestroyRenderPass(gId.dev, gId.compatible, nullptr);
    gId.compatible = VK_NULL_HANDLE;
    gId.colour = gId.depth = VK_FORMAT_UNDEFINED;
}

/// The pipeline for (colour, depth) — built on first use and when a format moves.
bool ensurePipeline(Ogre::VulkanRenderSystem *vkRs, VkFormat colour, VkFormat depth) {
    if (gId.refused) return false;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    if (!gId.dev) {
        gId.dev = device->mDevice;
        gId.drawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(
            vkGetDeviceProcAddr(gId.dev, "vkCmdDrawIndexedIndirectCountKHR"));
        gId.bufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            vkGetDeviceProcAddr(gId.dev, "vkGetBufferDeviceAddressKHR"));
        if (!gId.drawIndexedIndirectCount || !gId.bufferDeviceAddress) {
            gId.refused = true;
            logOnce("the device gives no vkCmdDrawIndexedIndirectCountKHR or no "
                    "vkGetBufferDeviceAddressKHR; the view keeps drawing the Atom queue through PBS");
            return false;
        }
        VkPushConstantRange pc{};
        pc.stageFlags = kIdPushStages;
        pc.offset = 0;
        pc.size = sizeof(IdPushConstants);
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        if (vkCreatePipelineLayout(gId.dev, &pl, nullptr, &gId.layout) != VK_SUCCESS) {
            gId.refused = true;
            logOnce("vkCreatePipelineLayout failed");
            return false;
        }
    }
    if (gId.pipeline[0] && gId.colour == colour && gId.depth == depth) return true;
    destroyPipelineObjects();

    // A RENDER PASS COMPATIBLE WITH OGRE'S: the same attachment formats and sample
    // counts, one subpass. Load/store ops do not take part in compatibility; this
    // one exists only for pipeline creation — the pass that runs is Ogre's.
    VkAttachmentDescription att[2]{};
    att[0].format = colour;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1] = att[0];
    att[1].format = depth;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colourRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depthRef{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colourRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    if (vkCreateRenderPass(gId.dev, &rp, nullptr, &gId.compatible) != VK_SUCCESS) {
        gId.refused = true;
        logOnce("vkCreateRenderPass (the compatible pass) failed");
        return false;
    }

    VkShaderModule vs = makeModule(gId.dev, katomId_vertSpv, sizeof(katomId_vertSpv));
    VkShaderModule fs = makeModule(gId.dev, katomId_fragSpv, sizeof(katomId_fragSpv));
    if (!vs || !fs) {
        if (vs) vkDestroyShaderModule(gId.dev, vs, nullptr);
        if (fs) vkDestroyShaderModule(gId.dev, fs, nullptr);
        gId.refused = true;
        logOnce("vkCreateShaderModule failed");
        return false;
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
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_BACK_BIT;
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
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy{};
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rast;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dy;
    gp.layout = gId.layout;
    gp.renderPass = gId.compatible;
    gp.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(gId.dev, device->mPipelineCache, 1, &gp, nullptr,
                                           &gId.pipeline[0]);
    if (r == VK_SUCCESS) {
        rast.cullMode = VK_CULL_MODE_FRONT_BIT;
        r = vkCreateGraphicsPipelines(gId.dev, device->mPipelineCache, 1, &gp, nullptr, &gId.pipeline[1]);
    }
    if (r == VK_SUCCESS) {
        rast.cullMode = VK_CULL_MODE_NONE;
        r = vkCreateGraphicsPipelines(gId.dev, device->mPipelineCache, 1, &gp, nullptr, &gId.pipeline[2]);
    }
    vkDestroyShaderModule(gId.dev, vs, nullptr);
    vkDestroyShaderModule(gId.dev, fs, nullptr);
    if (r != VK_SUCCESS) {
        destroyPipelineObjects();
        gId.refused = true;
        logOnce("vkCreateGraphicsPipelines failed");
        return false;
    }
    gId.colour = colour;
    gId.depth = depth;
    return true;
}

/// THE PASS'S DRAW MATRIX (HlmsPbs::preparePassHash's): the RS-depth projection, its y
/// row negated where the pass requires texture flipping, times the camera's view —
/// what the vertex stage draws with, and therefore what the depth the pyramid is
/// built from was drawn with.
Ogre::Matrix4 drawMatrixOf(Ogre::Camera *cam, const Ogre::RenderPassDescriptor *rpd) {
    Ogre::Matrix4 proj = cam->getProjectionMatrixWithRSDepth();
    if (rpd->requiresTextureFlipping())
        for (int c = 0; c < 4; ++c) proj[1][c] = -proj[1][c];
    return proj * cam->getVrViewMatrix(0);
}

/// THE MATRIX A CULL TESTS A PYRAMID WITH, from the pass's draw matrix: the cull turns NDC
/// into rows as `0.5 - y / 2` (JahCullTest_cs — the convention of a pyramid of depth Ogre
/// drew into a flipping target through the camera's own matrix), while this pass draws
/// through a POSITIVE-height viewport, where a row is `0.5 + y / 2` of its draw matrix:
/// the draw matrix with its y row negated makes the one the cull reads rows of correctly,
/// flipping target or not.
Ogre::Matrix4 cullMatrixOf(const Ogre::Matrix4 &draw) {
    Ogre::Matrix4 m = draw;
    for (int c = 0; c < 4; ++c) m[1][c] = -m[1][c];
    return m;
}

/// The pass's viewport rectangle in texels of its target (Ogre's Viewport derives its
/// actual rectangle from the definition's relative one the same way).
void viewportRectOf(const Ogre::CompositorPassDef::ViewportRect &r, uint32_t tw, uint32_t th, unsigned out[4]) {
    out[0] = unsigned(int(r.mVpLeft * float(tw)));
    out[1] = unsigned(int(r.mVpTop * float(th)));
    out[2] = unsigned(int(r.mVpWidth * float(tw)));
    out[3] = unsigned(int(r.mVpHeight * float(th)));
}

/// The recorder (AtomPassProvider's `atom_id`, and `atom_id_late` with `late`).
///
/// THE FIRST PASS'S RENDER PASS ALWAYS BEGINS: it is what clears the scene depth every
/// later pass of the view LOADS (and the id image). Whatever stops the draw — no view in
/// the registry yet, a GPU scene not live, a pipeline or a cull that did not record —
/// stops the DRAW only; an early return before the begin would hand the prepass and
/// the opaque pass last frame's depth.
///
/// THE TWO-PASS OCCLUSION (ATOM-OCCLUSION-1, ChainDesc::atomOcclusion). The FIRST pass
/// culls against the PREVIOUS frame's pyramid — still in `jahHzb`, since nothing has
/// rebuilt it yet this frame — projected with the matrix THAT pyramid's depth was drawn
/// with (the history), so the pyramid is never misread whatever the camera did since:
/// a cut only makes its prediction poor, never the answer wrong. The chain then builds
/// the pyramid from the first pass's depth; the LATE pass tests the set the first cull
/// rejected against it with THIS frame's matrix and draws the survivors into the same
/// id image and depth. An instance the late test also rejects lies behind depth that
/// is in the final picture (the first pass's surfaces are a subset of it), so no pixel
/// it could have won is lost: the id image equals the frustum-only one (the conservative
/// test: the farthest depth over every texel of the box's rectangle, the box's nearest
/// point, a relative margin). With NO usable history — a view's first frame, a rebuilt
/// graph, a resized pyramid, another world — the first cull tests nothing and the late
/// pass records nothing: the frame is the frustum-only frame.
void recordIdPass(AtomPassContext &ctx, bool late) {
    auto *pass = static_cast<AtomPass *>(ctx.pass);
    if (!pass || !pass->renderPassDesc()) return;
    const Ogre::RenderPassDescriptor *rpd = pass->renderPassDesc();
    Ogre::TextureGpu *ids = rpd->mColour[0].texture;
    Ogre::TextureGpu *depthTex = rpd->mDepth.texture;
    Ogre::VulkanRenderSystem *vkRs = atomVulkanOf(ctx.renderSystem);
    if (!ids || !depthTex || !vkRs) return;
    OgreView *view = atomViewOf(pass->getParentNode()->getWorkspace());
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    Ogre::Camera *cam = view ? view->camera() : nullptr;
    GpuScene *gs = scene ? &scene->gpuScene() : nullptr;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    const Ogre::CompositorPassDef::ViewportRect &vpRect = pass->getDefinition()->mVpRect[0];
    const uint32_t tw = uint32_t(ids->getWidth()), th = uint32_t(ids->getHeight());
    // THE LATE PASS draws only on a chain that carries it, after a first pass that
    // tested (and so may have rejected something).
    if (late && (!view || !view->chainAtomOcclusion())) return;
    bool draw = cam && gs && gs->live();
    if (draw) {
        const VkFormat colourFmt = Ogre::VulkanMappings::get(ids->getPixelFormat());
        const VkFormat depthFmt = Ogre::VulkanMappings::get(depthTex->getPixelFormat());
        draw = ensurePipeline(vkRs, colourFmt, depthFmt);
    }
    if (draw) gs->flushGeomRows();
    // THE PYRAMID (ChainDesc::atomOcclusion): the chain's `jahHzb`, the texture both
    // passes' tests bind.
    Ogre::TextureGpu *hzb = nullptr;
    if (view && view->chainAtomOcclusion()) {
        JAH_TRY { hzb = pass->getParentNode()->getDefinedTexture(Ogre::IdString("jahHzb")); }
        catch (const Ogre::Exception &) { hzb = nullptr; }
    }
    // THE LATE PASS BUILDS THE PYRAMID FIRST, from the depth the first pass just wrote
    // (every level the texture has: a resize is no new graph). Built every frame the
    // chain carries the occlusion, drawn or not — it is the next frame's history.
    bool pyramidBuilt = false;
    if (late && hzb) pyramidBuilt = recordOcclusionPyramid(ctx.renderSystem, depthTex, hzb);
    if (late && !pyramidBuilt) hzb = nullptr;
    unsigned rect[4] = { 0u, 0u, 0u, 0u };
    viewportRectOf(vpRect, tw, th, rect);
    Ogre::Matrix4 vpm = Ogre::Matrix4::IDENTITY;
    GpuCull *cullPtr = nullptr;
    if (draw) {
        // THE CAMERA'S ASPECT FIRST (CompositorPassScene::execute -> Viewport::
        // _setupAspectRatio): the scene passes set it when they run, and this pass
        // runs before all of them — a view sharing its camera with a view of another
        // shape (an editor shot) would otherwise cull and project with the other one's.
        {
            const int aw = int(vpRect.mVpWidth * float(tw));
            const int ah = int(vpRect.mVpHeight * float(th));
            const Ogre::Real aspect = Ogre::Real(aw) / Ogre::Real(std::max(1, ah));
            if (cam->getAutoAspectRatio() && cam->getAspectRatio() != aspect) cam->setAspectRatio(aspect);
        }
        vpm = drawMatrixOf(cam, rpd);
        AtomOcclusionHistory &hist = view->atomOcclusionHistory();
        // WHICH PYRAMID THIS PASS TESTS AGAINST, and with which matrix.
        Ogre::TextureGpu *testAgainst = nullptr;
        GpuCullRequest req;
        // THE PASS'S OWN HEIGHT — the viewport's actual rows, the letterbox inset included
        // (Viewport::getActualHeight, what the CPU strategy reads for the casters and the
        // remainder: OgreMesh.cpp's worldPerPixel) — never the whole target's.
        fillCullFrustum(cam, float(std::max(1, int(vpRect.mVpHeight * float(th)))), req);
        if (!late) {
            // THE PREVIOUS FRAME'S PYRAMID, while the history still names it.
            const bool usable = hzb && hist.valid && hist.generation == view->workspaceGeneration() &&
                                hist.scene == scene && hist.width == hzb->getWidth() &&
                                hist.height == hzb->getHeight() && hist.levels == hzb->getNumMipmaps() &&
                                std::equal(rect, rect + 4, hist.rect);
            view->setAtomFirstTested(usable);
            if (usable) {
                testAgainst = hzb;
                std::memcpy(req.viewProj, hist.viewProj, sizeof(req.viewProj));
                std::copy(hist.rect, hist.rect + 4, req.hzbRect);
            }
        } else if (hzb && view->atomFirstTested()) {
            // THIS FRAME'S, just built from the first pass's depth.
            testAgainst = hzb;
            const Ogre::Matrix4 cm = cullMatrixOf(vpm);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) req.viewProj[r * 4 + c] = float(cm[r][c]);
            std::copy(rect, rect + 4, req.hzbRect);
        }
        req.flagsRequired = kGpuVisible | kGpuAtom;
        req.flagsForbidden = 0u;
        req.hzbLevels = testAgainst ? testAgainst->getNumMipmaps() : 0u;
        // THE VIEW STRATEGY'S OWN BUDGET (kLodBudgetPixels), scaled by the scene's LOD bias
        // (OgreScene::applyLodValues divides the baked thresholds by it — the same
        // dial seen from the other side).
        req.pixelTolerance = kLodBudgetPixels * scene->lodBias();
        // NO SWITCH BAND (D4): the band exists because a WHOLE OBJECT switching pops;
        // a cut whose every group is judged under one sample of error changes its
        // frontier invisibly by construction (Nanite has none). THE CUT (mode 3).
        req.lodHysteresis = 0.0f;
        req.mode = 3u;
        cullPtr = late ? &view->atomCullLate() : &view->atomCull();
        if (late && !testAgainst) {
            // THE FIRST PASS TESTED NOTHING: nothing was rejected, there is nothing to
            // draw — the frame is the frustum-only frame. Its half of the ring says so.
            cycleStats(device, vkRs->getVaoManager(), view, nullptr, true);
            draw = false;
        } else {
            // ---- (0) THE LIST'S WRITE-AFTER-READ: the previous frame's draw read it
            // (the commands, the compacted stream as its index buffer, the slot bases in
            // the vertex stage and the triangle words in the fragment stage). ----
            {
                VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
                vkCmdPipelineBarrier(cmd,
                                     VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 0, nullptr);
            }
            // ---- (1) THE CULL, into the pass's own list ---------------------------
            std::string err;
            cycleStats(device, vkRs->getVaoManager(), view, cullPtr, late);   // before this request zeroes them
            // NO DAG-BEARING MESH ATTACHED YET (a new project's first frames): no cluster
            // tables, so there is no cut to record — the depth is still cleared below.
            gs->flushClusterTables();
            if (late) cullPtr->followCutBudget(view->atomCull());
            if (!gs->clusterBuffer() || !gs->groupBuffer()) {
                draw = false;
            } else if (!scene->recordGpuCull(*cullPtr, req, testAgainst, err, nullptr,
                                             late ? &view->atomCull() : nullptr,
                                             OgreScene::CullRows::IdPass)) {
                logOnce("the cull did not record (" + err + ")");
                draw = false;
            }
        }
        if (late) {
            // THE HISTORY THE NEXT FRAME'S FIRST CULL TESTS AGAINST: the pyramid of this
            // frame (built from the first pass's depth, drawn with this matrix).
            hist.valid = hzb != nullptr;
            const Ogre::Matrix4 cm = cullMatrixOf(vpm);
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) hist.viewProj[r * 4 + c] = float(cm[r][c]);
            std::copy(rect, rect + 4, hist.rect);
            hist.width = hzb ? hzb->getWidth() : 0u;
            hist.height = hzb ? hzb->getHeight() : 0u;
            hist.levels = hzb ? hzb->getNumMipmaps() : 0u;
            hist.generation = view->workspaceGeneration();
            hist.scene = scene;
        }
    } else if (late && view) {
        view->atomOcclusionHistory().valid = false;
    }
    if (draw) {
        // ---- (2) THE EDGES THE SOLVER CANNOT EXPRESS -----------------------------
        VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_SHADER_READ_BIT |
                           VK_ACCESS_INDEX_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    // THE LATE PASS WITH NOTHING TO DRAW records nothing at all: the first pass's id
    // image and depth stand, and the next scene pass's own barriers take the depth.
    if (late && !draw) return;

    // ---- (3) OGRE'S RENDER PASS, OUR PIPELINE ------------------------------------
    if (!pass->beginRenderPass()) return;
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
    if (!late) {
        VkClearAttachment ca{};
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        ca.clearValue.color.uint32[0] = AtomId::kEmpty;
        ca.clearValue.color.uint32[1] = 0u;
        VkClearRect cr{};
        cr.rect.extent = { tw, th };
        cr.layerCount = 1;
        vkCmdClearAttachments(cmd, 1, &ca, 1, &cr);
    }
    if (!draw) return;   // the depth is cleared (the begin) and the ids are empty
    GpuCull &cull = *cullPtr;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      gId.pipeline[rpd->requiresTextureFlipping() ? 1 : 0]);
    ctx.boundRawState = true;
    // THE PASS'S VIEWPORT, the way Ogre's Viewport derives its actual rectangle from
    // the definition's relative one (a letterboxed view's inset included).
    {
        const Ogre::CompositorPassDef::ViewportRect &r = vpRect;
        VkViewport v{};
        v.x = float(rect[0]);
        v.y = float(rect[1]);
        v.width = float(rect[2]);
        v.height = float(rect[3]);
        v.minDepth = 0.0f;
        v.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &v);
        VkRect2D sc{};
        sc.offset = { int32_t(r.mVpScissorLeft * float(tw)), int32_t(r.mVpScissorTop * float(th)) };
        sc.extent = { uint32_t(r.mVpScissorWidth * float(tw)), uint32_t(r.mVpScissorHeight * float(th)) };
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    {
        IdPushConstants pc{};
        for (int rr = 0; rr < 4; ++rr)
            for (int c = 0; c < 4; ++c) pc.viewProjRow[rr * 4 + c] = float(vpm[rr][c]);
        addressOf(gs->instanceBuffer(), pc.instances);
        addressOf(gs->meshBuffer(), pc.meshes);
        addressOf(gs->geomBuffer(), pc.rows);
        addressOf(cull.cutTriWords(), pc.triWords);
        addressOf(cull.cutSlotBase(), pc.slotBase);
        vkCmdPushConstants(cmd, gId.layout, kIdPushStages, 0, sizeof(pc), &pc);
    }
    {
        // THE CUT'S COMPACTED STREAM IS THE INDEX BUFFER (the identity buffer it
        // replaced is deleted): each command's run holds real vertex indices.
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize ibOff = 0;
        atomBufferOf(cull.cutStream(), ib, ibOff);
        vkCmdBindIndexBuffer(cmd, ib, ibOff, VK_INDEX_TYPE_UINT32);
        VkBuffer drawBuf = VK_NULL_HANDLE, countBuf = VK_NULL_HANDLE;
        VkDeviceSize drawOff = 0, countOff = 0;
        atomBufferOf(cull.draws(), drawBuf, drawOff);
        atomBufferOf(cull.count(), countBuf, countOff);
        const uint32_t maxDraws = std::min(cull.capacity(), gs->slotCount());
        gId.drawIndexedIndirectCount(cmd, drawBuf, drawOff, countBuf, countOff, maxDraws,
                                     GpuCull::kDrawWords * sizeof(uint32_t));
        // THE TWO-SIDED RANGE (ATOM-TWO-SIDED-1): the survivors flagged kGpuTwoSided,
        // both faces rasterised and resolved by the depth test — the same push
        // constants, viewport and index buffer (one layout), only the cull differs.
        // Recorded only while the scene has such an item.
        if (scene->hasTwoSidedAtomItems()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gId.pipeline[2]);
            gId.drawIndexedIndirectCount(
                cmd, drawBuf, drawOff + VkDeviceSize(cull.twoSidedFirst()) * GpuCull::kDrawWords * sizeof(uint32_t),
                countBuf, countOff + GpuCull::kTwoSidedCountOffsetBytes, maxDraws,
                GpuCull::kDrawWords * sizeof(uint32_t));
        }
    }
    // THE RENDERER'S COUNTERS SEE THIS DRAW TOO: an indirect draw never passes
    // through Ogre's render queue, so its share is added here, in the pass — the
    // frame's totals and the monitor's per-pass rows (a delta around the pass) both
    // include it: each pass its own list's. The GPU's own counters, read back a frame
    // or two late (the stats ring, cycleStats): exact for a still scene, a few frames
    // behind a moving one.
    {
        unsigned long long tris = 0ull;
        unsigned surv = 0u;
        if (late ? view->atomLateStats(tris, surv) : view->atomFirstStats(tris, surv)) {
            Ogre::RenderingMetrics m;
            m.mIsRecordingMetrics = true;
            m.mBatchCount = 1u;
            m.mDrawCount = 1u;
            m.mFaceCount = size_t(tris);
            m.mVertexCount = size_t(tris) * 3u;
            m.mInstanceCount = surv;
            ctx.renderSystem->_addMetrics(m);
        }
    }
}

}  // namespace

bool atomIdPassSupported(Ogre::RenderSystem *rs) {
    Ogre::VulkanRenderSystem *vkRs = atomVulkanOf(rs);
    if (!vkRs || !vkRs->getVulkanDevice() || gId.refused) return false;
    Ogre::VulkanDevice *d = vkRs->getVulkanDevice();
    return d->hasBufferDeviceAddress() &&
           d->hasDeviceExtension(Ogre::IdString(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) &&
           rs->supportsIndirectDispatch();
}

void registerAtomIdPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomIdPassId, [](AtomPassContext &ctx) { recordIdPass(ctx, false); });
        p->setRecorder(kAtomIdLatePassId, [](AtomPassContext &ctx) { recordIdPass(ctx, true); });
    }
}

void releaseAtomIdPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomIdPassId, AtomPassRecorder());
        p->setRecorder(kAtomIdLatePassId, AtomPassRecorder());
    }
    if (gId.dev) {
        vkDeviceWaitIdle(gId.dev);
        for (auto &kv : gRings) destroyRing(kv.second);
        gRings.clear();
        for (auto &g : gGrave) destroyRing(g.first);
        gGrave.clear();
        destroyPipelineObjects();
        if (gId.layout) vkDestroyPipelineLayout(gId.dev, gId.layout, nullptr);
    }
    gId = IdPipeline();
}

void atomIdPassForgetView(const OgreView *view) {
    auto it = gRings.find(view);
    if (it == gRings.end()) return;
    // The ring may be the target of a copy still in flight: to the grave, reaped once
    // its last frame has retired.
    gGrave.emplace_back(it->second, currentFrame());
    gRings.erase(it);
}

#else   // !JAH_RAY_QUERY — no raw Vulkan here (macOS): every view keeps the Atom queue on PBS.

void atomIdPassForgetView(const OgreView *) {}
bool atomIdPassSupported(Ogre::RenderSystem *) { return false; }
void registerAtomIdPass() {}
void releaseAtomIdPass() {}

#endif

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
