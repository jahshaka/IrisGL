// ATOM S3-DRAW — THE ID PASS (SPECS/atom/D3_S3_DRAW_DESIGN.md §2.1; the stage-0
// spike's P-A, spikes/atom-stage0/src/AtomIdDraw.cpp, made the product's).
//
// WHAT IT RECORDS, inside the chain's `atom_id` PASS_CUSTOM (OgreChain.cpp) and
// after the provider closed Ogre's render pass (AtomPass.h):
//   1. THE CULL — the GPU cull's three jobs over the scene's table, into the VIEW's
//      own list (OgreView::atomCull): frustum, the level rule (one pixel x the
//      scene's LOD bias, the draw path's own tolerance) and one
//      VkDrawIndexedIndirectCommand per survivor, the predicate "visible and routed
//      to Atom" (kGpuVisible | kGpuAtom). Nothing is read back.
//   2. THE EDGES the barrier solver cannot express — compute write -> indirect read
//      (patch 0032's own reason) and the GPU scene's tables, read by the vertex
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
// through the geometry rows over an IDENTITY index buffer), push constants only
// (the pass's view-projection rows and four device addresses — no descriptor set
// to keep per frame in flight), created against a render pass COMPATIBLE with
// Ogre's (the same two formats, one sample) and rebuilt only when a format does.
// Front face CLOCKWISE and back faces culled, exactly Ogre's Vulkan convention for
// the default macroblock (VulkanRenderSystem's PSO: frontFace CLOCKWISE,
// CULL_CLOCKWISE -> VK_CULL_MODE_BACK_BIT) — and FRONT faces where the pass
// requires texture flipping, Hlms's InvertCullingMode rule; depth test GREATER_OR_EQUAL under
// reverse-Z, write on. A two-sided material never reaches it (the split's
// `twoSided` reason keeps it on PBS).
#include "EnginePrivate.h"
#include "AtomPass.h"
#include "GpuCull.h"
#include "GpuScene.h"
#include "HlmsAtom.h"

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <OgreRenderPassDescriptor.h>
#include <OgreRenderSystem.h>
#include <OgreRoot.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Vao/OgreVaoManager.h>

#include <algorithm>
#include <cstring>
#include <vector>

#if JAH_RAY_QUERY
#include "OgreVulkanDevice.h"
#include "OgreVulkanMappings.h"
#include "OgreVulkanQueue.h"
#include "OgreVulkanRenderSystem.h"
#include "Vao/OgreVulkanBufferInterface.h"

#include "rayquery/atom_id_frag_spv.h"
#include "rayquery/atom_id_vert_spv.h"
#endif

namespace jahshaka {
namespace engine {
namespace detail {

#if JAH_RAY_QUERY
namespace {

/// The push constants, as atom_id.vert declares them (std430 push-constant block:
/// four vec4 then four uvec2 — 96 bytes, inside the 128 every device offers).
struct IdPushConstants {
    float    viewProjRow[16];
    uint32_t instances[2];
    uint32_t levels[2];
    uint32_t rows[2];
    uint32_t cullLevels[2];
};
static_assert(sizeof(IdPushConstants) == 96, "atom_id.vert's push-constant block");

struct IdPipeline {
    VkDevice dev = VK_NULL_HANDLE;
    PFN_vkCmdDrawIndexedIndirectCountKHR drawIndexedIndirectCount = nullptr;
    PFN_vkGetBufferDeviceAddressKHR bufferDeviceAddress = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass compatible = VK_NULL_HANDLE;
    /// [0] culls BACK faces, [1] FRONT faces: the pass that requires texture
    /// flipping gets its y row negated AND its culling inverted (Hlms's own
    /// InvertCullingMode rule, OgreHlms.cpp) — the same pair here.
    VkPipeline pipeline[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkFormat colour = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    /// THE IDENTITY INDEX BUFFER: element i holds i, so a command's firstIndex and
    /// indexCount address the level's own index range through gl_VertexIndex.
    Ogre::IndexBufferPacked *identity = nullptr;
    uint32_t identityCount = 0u;
    bool refused = false;   ///< a creation failed; said once, the pass records nothing
};
IdPipeline gId;

void logOnce(const std::string &what) {
    static std::string sLast;
    if (what == sLast) return;
    sLast = what;
    Ogre::LogManager::getSingleton().logMessage("Jahshaka atom id pass: " + what, Ogre::LML_CRITICAL);
}

Ogre::VulkanRenderSystem *vulkanOf(Ogre::RenderSystem *rs) {
    return rs ? dynamic_cast<Ogre::VulkanRenderSystem *>(rs) : nullptr;
}

/// The VkBuffer + byte offset of an Ogre buffer (the ray tier's own reach).
template <typename T>
void bufferOf(T *buf, VkBuffer &outBuffer, VkDeviceSize &outOffset) {
    auto *bi = static_cast<Ogre::VulkanBufferInterface *>(buf->getBufferInterface());
    outBuffer = bi->getVboName();
    outOffset = VkDeviceSize(buf->_getFinalBufferStart()) * buf->getBytesPerElement();
}

/// A device address as the shader's uvec2 (lo, hi).
void addressOf(Ogre::UavBufferPacked *buf, uint32_t out[2]) {
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    bufferOf(buf, b, off);
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    const VkDeviceAddress a = gId.bufferDeviceAddress(gId.dev, &info) + off;
    out[0] = uint32_t(a & 0xFFFFFFFFull);
    out[1] = uint32_t(a >> 32u);
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
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
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

/// The identity index buffer, grown (by doubling) to cover every level range the
/// scene's table holds.
bool ensureIdentity(Ogre::VaoManager *vao, const GpuScene &gs) {
    uint32_t need = 0u;
    for (uint32_t m = 0, e = gs.levelMirrorEntries() / GpuScene::kLevelsPerMesh; m < e; ++m)
        for (uint32_t l = 0; l < GpuScene::kLevelsPerMesh; ++l) {
            const GpuMeshLevel &lv = gs.levelAt(m, l);
            need = std::max(need, lv.firstIndex + lv.indexCount);
        }
    if (gId.identity && need <= gId.identityCount) return true;
    uint32_t count = std::max(gId.identityCount, 65536u);
    while (count < need) count *= 2u;
    if (gId.identity) {
        vao->destroyIndexBuffer(gId.identity);
        gId.identity = nullptr;
        gId.identityCount = 0u;
    }
    std::vector<uint32_t> data(count);
    for (uint32_t i = 0; i < count; ++i) data[i] = i;
    gId.identity = vao->createIndexBuffer(Ogre::IndexBufferPacked::IT_32BIT, count, Ogre::BT_IMMUTABLE,
                                          data.data(), false);
    gId.identityCount = gId.identity ? count : 0u;
    return gId.identity != nullptr;
}

/// The recorder (AtomPassProvider's `atom_id`).
///
/// THE RENDER PASS ALWAYS BEGINS: it is what clears the scene depth every later pass
/// of the view LOADS (and the id image). Whatever stops the draw — no view in the
/// registry yet, a GPU scene not live, a pipeline or a cull that did not record —
/// stops the DRAW only; an early return before the begin would hand the prepass and
/// the opaque pass last frame's depth.
void recordIdPass(AtomPassContext &ctx) {
    auto *pass = static_cast<AtomPass *>(ctx.pass);
    if (!pass || !pass->renderPassDesc()) return;
    const Ogre::RenderPassDescriptor *rpd = pass->renderPassDesc();
    Ogre::TextureGpu *ids = rpd->mColour[0].texture;
    Ogre::TextureGpu *depthTex = rpd->mDepth.texture;
    Ogre::VulkanRenderSystem *vkRs = vulkanOf(ctx.renderSystem);
    if (!ids || !depthTex || !vkRs) return;
    OgreView *view = atomViewOf(pass->getParentNode()->getWorkspace());
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    Ogre::Camera *cam = view ? view->camera() : nullptr;
    GpuScene *gs = scene ? &scene->gpuScene() : nullptr;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    const Ogre::CompositorPassDef::ViewportRect &vpRect = pass->getDefinition()->mVpRect[0];
    bool draw = cam && gs && gs->live();
    if (draw) {
        const VkFormat colourFmt = Ogre::VulkanMappings::get(ids->getPixelFormat());
        const VkFormat depthFmt = Ogre::VulkanMappings::get(depthTex->getPixelFormat());
        draw = ensurePipeline(vkRs, colourFmt, depthFmt);
    }
    if (draw) {
        gs->flushGeomRows();
        draw = ensureIdentity(vkRs->getVaoManager(), *gs);
        if (!draw) logOnce("the identity index buffer could not be created");
    }
    GpuCull *cullPtr = nullptr;
    if (draw) {
        // ---- (0) THE LIST'S WRITE-AFTER-READ: the previous frame's draw read it. ----
        {
            VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 0, nullptr);
        }

        // ---- (1) THE CULL, into the view's own list ------------------------------
        // THE CAMERA'S ASPECT FIRST (CompositorPassScene::execute -> Viewport::
        // _setupAspectRatio): the scene passes set it when they run, and this pass
        // runs before all of them — a view sharing its camera with a view of another
        // shape (an editor shot) would otherwise cull and project with the other one's.
        {
            const int aw = int(vpRect.mVpWidth * float(ids->getWidth()));
            const int ah = int(vpRect.mVpHeight * float(ids->getHeight()));
            const Ogre::Real aspect = Ogre::Real(aw) / Ogre::Real(std::max(1, ah));
            if (cam->getAutoAspectRatio() && cam->getAspectRatio() != aspect) cam->setAspectRatio(aspect);
        }
        GpuCullRequest req;
        fillCullFrustum(cam, float(ids->getHeight()), req);
        req.flagsRequired = kGpuVisible | kGpuAtom;
        req.flagsForbidden = 0u;
        req.hzbLevels = 0u;
        // THE VIEW STRATEGY'S OWN BUDGET (kLodBudgetPixels), scaled by the scene's LOD bias
        // (OgreScene::applyLodValues divides the baked thresholds by it — the same
        // dial seen from the other side).
        req.pixelTolerance = kLodBudgetPixels * scene->lodBias();
        // ...and THE VIEW'S SWITCH BAND, the one its scene passes carry (ogre-patch
        // 0075; ChainDesc::lodHysteresis): a watched view holds a level across a
        // threshold on this path exactly as it does on Ogre's.
        req.lodHysteresis = view->chainDesc().lodHysteresis;
        req.mode = 2u;
        std::string err;
        view->harvestAtomStats();   // the last frame's counters, before this request zeroes them
        cullPtr = &view->atomCull();
        if (!scene->recordGpuCull(*cullPtr, req, nullptr, err)) {
            logOnce("the cull did not record (" + err + ")");
            draw = false;
        }
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
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &mb, 0, nullptr, 0, nullptr);
    }

    // ---- (3) OGRE'S RENDER PASS, OUR PIPELINE ------------------------------------
    if (!pass->beginRenderPass()) return;
    VkCommandBuffer cmd = device->mGraphicsQueue.getCurrentCmdBuffer();
    const uint32_t tw = uint32_t(ids->getWidth()), th = uint32_t(ids->getHeight());
    {
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
        v.x = float(int(r.mVpLeft * float(tw)));
        v.y = float(int(r.mVpTop * float(th)));
        v.width = float(int(r.mVpWidth * float(tw)));
        v.height = float(int(r.mVpHeight * float(th)));
        v.minDepth = 0.0f;
        v.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &v);
        VkRect2D s{};
        s.offset = { int32_t(r.mVpScissorLeft * float(tw)), int32_t(r.mVpScissorTop * float(th)) };
        s.extent = { uint32_t(r.mVpScissorWidth * float(tw)), uint32_t(r.mVpScissorHeight * float(th)) };
        vkCmdSetScissor(cmd, 0, 1, &s);
    }
    {
        // THE PASS BUFFER'S OWN PROJECTION (HlmsPbs::preparePassHash): the RS-depth
        // projection, its y row negated where the pass requires texture flipping,
        // times the camera's view.
        Ogre::Matrix4 proj = cam->getProjectionMatrixWithRSDepth();
        if (rpd->requiresTextureFlipping())
            for (int c = 0; c < 4; ++c) proj[1][c] = -proj[1][c];
        const Ogre::Matrix4 vpm = proj * cam->getVrViewMatrix(0);
        IdPushConstants pc{};
        for (int rr = 0; rr < 4; ++rr)
            for (int c = 0; c < 4; ++c) pc.viewProjRow[rr * 4 + c] = float(vpm[rr][c]);
        addressOf(gs->instanceBuffer(), pc.instances);
        addressOf(gs->levelBuffer(), pc.levels);
        addressOf(gs->geomBuffer(), pc.rows);
        addressOf(cull.levels(), pc.cullLevels);
        vkCmdPushConstants(cmd, gId.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
    }
    {
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize ibOff = 0;
        bufferOf(gId.identity, ib, ibOff);
        vkCmdBindIndexBuffer(cmd, ib, ibOff, VK_INDEX_TYPE_UINT32);
        VkBuffer drawBuf = VK_NULL_HANDLE, countBuf = VK_NULL_HANDLE;
        VkDeviceSize drawOff = 0, countOff = 0;
        bufferOf(cull.draws(), drawBuf, drawOff);
        bufferOf(cull.count(), countBuf, countOff);
        gId.drawIndexedIndirectCount(cmd, drawBuf, drawOff, countBuf, countOff,
                                     std::min(cull.capacity(), gs->slotCount()),
                                     GpuCull::kDrawWords * sizeof(uint32_t));
    }
    // THE RENDERER'S COUNTERS SEE THIS DRAW TOO: an indirect draw never passes
    // through Ogre's render queue, so its share is added here, in the pass — the
    // frame's totals and the monitor's per-pass rows (a delta around the pass) both
    // include it. The GPU's own counters, read back a frame or two late
    // (OgreView::harvestAtomStats): exact for a still scene, one frame behind a moving one.
    {
        unsigned long long tris = 0ull;
        unsigned surv = 0u;
        if (view->atomStats(tris, surv)) {
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
    Ogre::VulkanRenderSystem *vkRs = vulkanOf(rs);
    if (!vkRs || !vkRs->getVulkanDevice() || gId.refused) return false;
    Ogre::VulkanDevice *d = vkRs->getVulkanDevice();
    return d->hasBufferDeviceAddress() &&
           d->hasDeviceExtension(Ogre::IdString(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) &&
           rs->supportsIndirectDispatch();
}

void registerAtomIdPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance())
        p->setRecorder(kAtomIdPassId, [](AtomPassContext &ctx) { recordIdPass(ctx); });
}

void releaseAtomIdPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) p->setRecorder(kAtomIdPassId, AtomPassRecorder());
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    if (gId.identity && rs && rs->getVaoManager()) rs->getVaoManager()->destroyIndexBuffer(gId.identity);
    gId.identity = nullptr;
    gId.identityCount = 0u;
    if (gId.dev) {
        vkDeviceWaitIdle(gId.dev);
        destroyPipelineObjects();
        if (gId.layout) vkDestroyPipelineLayout(gId.dev, gId.layout, nullptr);
    }
    gId = IdPipeline();
}

#else   // !JAH_RAY_QUERY — no raw Vulkan here (macOS): every view keeps the Atom queue on PBS.

bool atomIdPassSupported(Ogre::RenderSystem *) { return false; }
void registerAtomIdPass() {}
void releaseAtomIdPass() {}

#endif

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
