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
#include "rayquery/atom_id_masked_frag_spv.h"
#include "rayquery/atom_id_masked_vert_spv.h"
#include "rayquery/atom_id_vert_spv.h"
#endif

namespace jahshaka {
namespace engine {
namespace detail {

/// THE PYRAMID'S ARRIVAL COUNTER (JahHzbBuild_cs, UAV slot kHzbCounterSlot): one
/// engine-lifetime buffer, ZERO at creation and put back to zero by the last workgroup of
/// every build, so every build — the id pass's and any view's own — shares it. Builds are
/// recorded in sequence and Ogre's barrier solver orders each one's counter traffic after
/// the previous one's. OWNED BY THE JOB'S OWN BINDING: created and bound on the job at its
/// first use (no pass declares it), found there again, and unbound and destroyed by
/// releaseHzbBuild before Root — no pointer of ours holds it.
namespace {
/// The whole-depth build and the RECTANGLE build (a shadow map's, ENGINE-UPGRADE-U5 stage 4): one
/// source, two jobs, each with its own counter (the rectangle's job binds its corner after it).
constexpr const char *kHzbJobs[2] = { "Jahshaka/HzbBuild", "Jahshaka/HzbBuildRect" };
constexpr Ogre::uint8 kHzbOriginSlot = kHzbCounterSlot + 1u;
Ogre::HlmsComputeJob *hzbJob(unsigned which = 0u) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsCompute *hc = root && root->getHlmsManager() ? root->getHlmsManager()->getComputeHlms() : nullptr;
    return hc ? hc->findComputeJobNoThrow(kHzbJobs[which]) : nullptr;
}
Ogre::UavBufferPacked *hzbCounterOf(const Ogre::HlmsComputeJob *job) {
    return job && job->getNumUavUnits() > kHzbCounterSlot ? job->getUavBuffer(kHzbCounterSlot) : nullptr;
}
Ogre::HlmsComputeJob *hzbJobWithCounter(unsigned which);
}  // namespace

Ogre::HlmsComputeJob *hzbBuildJob() { return hzbJobWithCounter(0u); }

namespace {
Ogre::HlmsComputeJob *hzbJobWithCounter(unsigned which) {
    Ogre::HlmsComputeJob *job = hzbJob(which);
    if (!job) return nullptr;
    if (!hzbCounterOf(job)) {
        Ogre::Root *root = Ogre::Root::getSingletonPtr();
        Ogre::VaoManager *vao = root->getRenderSystem() ? root->getRenderSystem()->getVaoManager() : nullptr;
        if (!vao) return nullptr;
        const uint32_t zeros[4] = { 0u, 0u, 0u, 0u };
        Ogre::DescriptorSetUav::BufferSlot slot(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        slot.buffer = vao->createUavBuffer(1u, sizeof(zeros), 0, const_cast<uint32_t *>(zeros), false);
        slot.offset = 0;
        slot.sizeBytes = 0;
        slot.access = Ogre::ResourceAccess::ReadWrite;
        job->_setUavBuffer(kHzbCounterSlot, slot);
    }
    return job;
}
}  // namespace

void releaseHzbBuild() {
    for (unsigned which = 0u; which < 2u; ++which) {
        Ogre::HlmsComputeJob *job = hzbJob(which);
        Ogre::UavBufferPacked *counter = hzbCounterOf(job);
        if (!counter) continue;
        job->_setUavBuffer(kHzbCounterSlot, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        Ogre::Root *root = Ogre::Root::getSingletonPtr();
        if (root && root->getRenderSystem() && root->getRenderSystem()->getVaoManager())
            root->getRenderSystem()->getVaoManager()->destroyUavBuffer(counter);
    }
}

/// THE OCCLUSION'S PYRAMID (EnginePrivate.h): ONE dispatch of the single-pass build
/// (JahHzbBuild_cs — mip 0's copy and every level, the tail by the last workgroup), the
/// compositor's own compute-pass discipline (CompositorPassCompute::execute — bind, the
/// job's barriers through Ogre's solver, dispatch), the level count read off the texture
/// as it is.
namespace {
bool recordPyramid(Ogre::RenderSystem *rs, Ogre::TextureGpu *depth, Ogre::TextureGpu *hzb,
                   Ogre::UavBufferPacked *origin) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsCompute *hc = root && root->getHlmsManager() ? root->getHlmsManager()->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *job = hzbJobWithCounter(origin ? 1u : 0u);
    if (!rs || !hc || !depth || !hzb || !job) return false;
    const Ogre::uint8 levels = hzb->getNumMipmaps();
    if (levels == 0u || levels > kHzbMaxLevels) return false;
    // FARTHEST, in the render system's depth direction (JahHzbBuild_cs; the job's
    // properties, shared with a view's own pyramid — which is farthest wherever this
    // one is built, ChainDesc::atomOcclusion).
    // THE PROPERTIES ARE PUT BACK after the build: a view's own pyramid's compute pass
    // (PostFxDesc::hzb) sets them once, at its chain's build, on this same job — a
    // CLOSEST one elsewhere would otherwise be built farthest (chain.hzb caught it).
    const Ogre::int32 reverse = rs->isReverseDepth() ? 1 : 0;
    const Ogre::int32 wasReverse = job->getProperty("hzb_reverse_z");
    const Ogre::int32 wasFarthest = job->getProperty("hzb_farthest");
    if (wasReverse != reverse) job->setProperty("hzb_reverse_z", reverse);
    if (wasFarthest != 1) job->setProperty("hzb_farthest", 1);
    rs->endRenderPassDescriptor();
    {
        Ogre::DescriptorSetTexture2::TextureSlot ts = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        ts.texture = depth;
        job->setTexture(0u, ts);
    }
    // Every mip in its own slot; the slots past the last level hold the last level
    // again (the shader stops at the texture's own count and never writes them).
    for (Ogre::uint8 m = 0u; m < kHzbMaxLevels; ++m) {
        Ogre::DescriptorSetUav::TextureSlot t = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        t.texture = hzb;
        t.access = Ogre::ResourceAccess::ReadWrite;
        t.mipmapLevel = std::min<Ogre::uint8>(m, Ogre::uint8(levels - 1u));
        job->_setUavTexture(m, t);
    }
    if (origin) {
        // THE RECTANGLE'S CORNER, and the groups the PYRAMID's size asks for (the job is sized
        // by nothing: the depth it reads is the whole atlas) - one 64 x 64 tile a group, the
        // whole-depth job's own divisor.
        Ogre::DescriptorSetUav::BufferSlot b(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        b.buffer = origin;
        b.offset = 0;
        b.sizeBytes = 0;
        b.access = Ogre::ResourceAccess::Read;
        job->_setUavBuffer(kHzbOriginSlot, b);
        job->setNumThreadGroups((hzb->getWidth() + 63u) / 64u, (hzb->getHeight() + 63u) / 64u, 1u);
    }
    {
        Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
        job->analyzeBarriers(rt);
        rs->executeResourceTransition(rt);
        hc->dispatch(job, nullptr, nullptr);
    }
    // The job's descriptor sets hold raw pointers: no texture binding outlives this
    // build (the counter's is the engine's, and stays).
    job->setTexture(0u, Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
    for (Ogre::uint8 m = 0u; m < kHzbMaxLevels; ++m)
        job->_setUavTexture(m, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
    if (origin) job->_setUavBuffer(kHzbOriginSlot, Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
    if (wasReverse != reverse) job->setProperty("hzb_reverse_z", wasReverse);
    if (wasFarthest != 1) job->setProperty("hzb_farthest", wasFarthest);
    return true;
}
}  // namespace

bool recordOcclusionPyramid(Ogre::RenderSystem *rs, Ogre::TextureGpu *depth, Ogre::TextureGpu *hzb) {
    return recordPyramid(rs, depth, hzb, nullptr);
}

bool recordOcclusionPyramidRect(Ogre::RenderSystem *rs, Ogre::TextureGpu *depth, Ogre::TextureGpu *hzb,
                                Ogre::UavBufferPacked *origin) {
    return origin && recordPyramid(rs, depth, hzb, origin);
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
    uint32_t maskTable[2] = {};   ///< GpuScene::maskTableAddress (the masked ranges' fragment stage)
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
    /// the list's two-sided range (ATOM-TWO-SIDED-1), flipped or not. [3..5] the same three
    /// for the MASKED ranges (ENGINE-POLISH-ATOM-1 stage 3): the vertex stage carries the
    /// texture coordinate and the fragment stage discards where the cut-out's mask is clear.
    VkPipeline pipeline[6] = {};
    VkFormat colour = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    bool refused = false;   ///< a creation failed; said once, the pass records nothing
};
/// THE ID PASS'S STATS RING, per view (the view owns it: OgreView::atomStatsRing): the cull's count words copied into host-visible
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
}  // namespace

/// THE ID PASS'S DEVICE STATE (HlmsAtom.h): the pipelines, the rings of views that went
/// away with the frame they left at (destroyed once that frame has retired — a copy into
/// them may still be in flight — never a device wait), and the last line said.
struct AtomIdDevice {
    IdPipeline ids;
    std::vector<std::pair<StatsRing, uint32_t>> grave;
    std::string lastLog;
};
/// A view's ring (EnginePrivate.h's AtomIdViewRing).
struct AtomIdViewRing {
    StatsRing ring;
};

namespace {
HlmsAtom *atomHlms() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::HlmsManager *hm = root ? root->getHlmsManager() : nullptr;
    return hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
}
/// The device's id pass state, or null where Atom's Hlms is not registered (the id pass
/// is then not supported: atomIdPassSupported).
AtomIdDevice *idDev() {
    HlmsAtom *a = atomHlms();
    return a ? &a->idDevice() : nullptr;
}

void logOnce(const std::string &what) {
    if (AtomIdDevice *d = idDev()) {
        if (what == d->lastLog) return;
        d->lastLog = what;
    }
    Ogre::LogManager::getSingleton().logMessage("Jahshaka atom id pass: " + what, Ogre::LML_CRITICAL);
}

/// A device address as the shader's uvec2 (lo, hi).
void addressOf(const IdPipeline &ids, Ogre::UavBufferPacked *buf, uint32_t out[2]) {
    atomAddressOf(ids.dev, ids.bufferDeviceAddress, buf, out);
}


uint32_t currentFrame() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    return rs && rs->getVaoManager() ? rs->getVaoManager()->getFrameCount() : 0u;
}

constexpr VkDeviceSize kStatsHalfBytes = GpuCull::kCountElements * sizeof(uint32_t);
constexpr VkDeviceSize kStatsSlotBytes = 2u * kStatsHalfBytes;

void destroyRing(const IdPipeline &ids, StatsRing &r) {
    if (!ids.dev) return;
    if (r.memory) {
        vkUnmapMemory(ids.dev, r.memory);
        vkFreeMemory(ids.dev, r.memory, nullptr);
    }
    if (r.buffer) vkDestroyBuffer(ids.dev, r.buffer, nullptr);
    r = StatsRing();
}

bool ensureRing(const IdPipeline &ids, Ogre::VulkanDevice *device, StatsRing &r, uint32_t slots) {
    if (r.buffer) return true;
    slots = std::min(std::max(slots, 2u), 8u);
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = kStatsSlotBytes * slots;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(ids.dev, &bi, nullptr, &r.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(ids.dev, r.buffer, &req);
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
    if (type == UINT32_MAX || vkAllocateMemory(ids.dev, &ai, nullptr, &r.memory) != VK_SUCCESS ||
        vkBindBufferMemory(ids.dev, r.buffer, r.memory, 0) != VK_SUCCESS ||
        vkMapMemory(ids.dev, r.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        destroyRing(ids, r);
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
void reapGrave(AtomIdDevice &d, uint32_t frame) {
    for (size_t i = 0; i < d.grave.size();) {
        if (frame - d.grave[i].second > 8u) {
            destroyRing(d.ids, d.grave[i].first);
            d.grave[i] = d.grave.back();
            d.grave.pop_back();
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
void cycleStats(const AtomPassContext &ctx, AtomIdDevice &d, Ogre::VulkanDevice *device, Ogre::VaoManager *vao,
                OgreView *view, GpuCull *cull, bool late) {
    if (!late) reapGrave(d, vao->getFrameCount());
    if (cull && !cull->count()) return;
    AtomIdViewRingPtr &owned = view->atomStatsRing();
    if (!owned) owned.reset(new AtomIdViewRing());
    StatsRing &r = owned->ring;
    if (!ensureRing(d.ids, device, r, uint32_t(vao->getDynamicBufferMultiplier()) + 1u)) return;
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
    VkCommandBuffer cmd = ctx.frameCmd();
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

void destroyPipelineObjects(IdPipeline &ids) {
    if (!ids.dev) return;
    for (VkPipeline &p : ids.pipeline) {
        if (p) vkDestroyPipeline(ids.dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (ids.compatible) vkDestroyRenderPass(ids.dev, ids.compatible, nullptr);
    ids.compatible = VK_NULL_HANDLE;
    ids.colour = ids.depth = VK_FORMAT_UNDEFINED;
}

/// The pipeline for (colour, depth) — built on first use and when a format moves.
bool ensurePipeline(IdPipeline &ids, Ogre::VulkanRenderSystem *vkRs, VkFormat colour, VkFormat depth) {
    if (ids.refused) return false;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    if (!ids.dev) {
        ids.dev = device->mDevice;
        ids.drawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCountKHR>(
            vkGetDeviceProcAddr(ids.dev, "vkCmdDrawIndexedIndirectCountKHR"));
        ids.bufferDeviceAddress = reinterpret_cast<PFN_vkGetBufferDeviceAddressKHR>(
            vkGetDeviceProcAddr(ids.dev, "vkGetBufferDeviceAddressKHR"));
        if (!ids.drawIndexedIndirectCount || !ids.bufferDeviceAddress) {
            ids.refused = true;
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
        if (vkCreatePipelineLayout(ids.dev, &pl, nullptr, &ids.layout) != VK_SUCCESS) {
            ids.refused = true;
            logOnce("vkCreatePipelineLayout failed");
            return false;
        }
    }
    if (ids.pipeline[0] && ids.colour == colour && ids.depth == depth) return true;
    destroyPipelineObjects(ids);

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
    if (vkCreateRenderPass(ids.dev, &rp, nullptr, &ids.compatible) != VK_SUCCESS) {
        ids.refused = true;
        logOnce("vkCreateRenderPass (the compatible pass) failed");
        return false;
    }

    VkShaderModule vs = makeModule(ids.dev, katomId_vertSpv, sizeof(katomId_vertSpv));
    VkShaderModule fs = makeModule(ids.dev, katomId_fragSpv, sizeof(katomId_fragSpv));
    VkShaderModule mvs = makeModule(ids.dev, katomIdMasked_vertSpv, sizeof(katomIdMasked_vertSpv));
    VkShaderModule mfs = makeModule(ids.dev, katomIdMasked_fragSpv, sizeof(katomIdMasked_fragSpv));
    const auto dropModules = [&] {
        for (VkShaderModule m : { vs, fs, mvs, mfs })
            if (m) vkDestroyShaderModule(ids.dev, m, nullptr);
    };
    if (!vs || !fs || !mvs || !mfs) {
        dropModules();
        ids.refused = true;
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
    gp.layout = ids.layout;
    gp.renderPass = ids.compatible;
    gp.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(ids.dev, device->mPipelineCache, 1, &gp, nullptr,
                                           &ids.pipeline[0]);
    if (r == VK_SUCCESS) {
        rast.cullMode = VK_CULL_MODE_FRONT_BIT;
        r = vkCreateGraphicsPipelines(ids.dev, device->mPipelineCache, 1, &gp, nullptr, &ids.pipeline[1]);
    }
    if (r == VK_SUCCESS) {
        rast.cullMode = VK_CULL_MODE_NONE;
        r = vkCreateGraphicsPipelines(ids.dev, device->mPipelineCache, 1, &gp, nullptr, &ids.pipeline[2]);
    }
    // THE MASKED THREE (ENGINE-POLISH-ATOM-1 stage 3): the same state, the JAH_MASKED stages.
    stages[0].module = mvs;
    stages[1].module = mfs;
    const VkCullModeFlags culls[3] = { VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_NONE };
    for (unsigned k = 0; k < 3u && r == VK_SUCCESS; ++k) {
        rast.cullMode = culls[k];
        r = vkCreateGraphicsPipelines(ids.dev, device->mPipelineCache, 1, &gp, nullptr, &ids.pipeline[3u + k]);
    }
    dropModules();
    if (r != VK_SUCCESS) {
        destroyPipelineObjects(ids);
        ids.refused = true;
        logOnce("vkCreateGraphicsPipelines failed");
        return false;
    }
    ids.colour = colour;
    ids.depth = depth;
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
/// a cut only makes its prediction poor, never the answer wrong. Between the two passes
/// the chain copies the first pass's depth into the Atom view's (kAtomViewDepth) and
/// draws the STOCK opaque depth into the scene's (ATOM-4 stage 1, OgreChain.cpp
/// addAtomOcclusionPasses); the LATE pass builds the pyramid from the scene's depth —
/// the first pass's surfaces and the stock ones — tests the set the first cull rejected
/// against it with THIS frame's matrix and draws the survivors into the id image and the
/// Atom view's depth (a merge quad then brings them into the scene's). An instance the
/// late test also rejects lies behind depth that is in the final picture (the first
/// pass's surfaces and the stock opaque ones are a subset of it), so no pixel it could
/// have won is lost: wherever an Atom surface is the picture's, the id image equals the
/// frustum-only one (the conservative test: the farthest depth over every texel of the
/// box's rectangle, the box's nearest point, a relative margin) — and a cluster wholly
/// behind a stock wall is drawn by neither pass. With NO usable history — a view's first frame, a rebuilt
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
    OgreView *view = atomViewOfPass(pass);
    AtomIdDevice *D = idDev();
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    Ogre::Camera *cam = view ? view->camera() : nullptr;
    GpuScene *gs = scene ? &scene->gpuScene() : nullptr;
    Ogre::VulkanDevice *device = vkRs->getVulkanDevice();
    const Ogre::CompositorPassDef::ViewportRect &vpRect = pass->getDefinition()->mVpRect[0];
    const uint32_t tw = uint32_t(ids->getWidth()), th = uint32_t(ids->getHeight());
    // THE LATE PASS draws only on a chain that carries it, after a first pass that
    // tested (and so may have rejected something).
    if (late && (!view || !view->chainAtomOcclusion())) return;
    // THE CAPTURE'S WORKSPACE (CLOSE-SHOT-2, View::requestFrameCapture) is the view's
    // chain drawn a second time in the frame: its first cull tests NOTHING (its own
    // pyramid was never built, and the view's history names the view's), so it draws
    // the frustum's every cluster and its late pass has nothing left to add; and it
    // touches none of the view's state — no history, no first-tested flag, no stats
    // ring, no cut budget.
    const bool capture = view && view->isCaptureWorkspace(pass->getParentNode()->getWorkspace());
    if (late && capture) return;
    bool draw = D && cam && gs && gs->live();
    if (draw) {
        const VkFormat colourFmt = Ogre::VulkanMappings::get(ids->getPixelFormat());
        const VkFormat depthFmt = Ogre::VulkanMappings::get(depthTex->getPixelFormat());
        draw = ensurePipeline(D->ids, vkRs, colourFmt, depthFmt);
    }
    if (draw) gs->flushGeomRows();
    // THE PYRAMID (ChainDesc::atomOcclusion): the chain's `jahHzb`, the texture both
    // passes' tests bind.
    Ogre::TextureGpu *hzb = nullptr;
    if (view && view->chainAtomOcclusion()) {
        JAH_TRY { hzb = pass->getParentNode()->getDefinedTexture(Ogre::IdString("jahHzb")); }
        catch (const Ogre::Exception &) { hzb = nullptr; }
    }
    // THE SCENE'S DEPTH AND THE ID PASS'S OWN on a chain with the occlusion (ATOM-4 stages 1 and 1b). In the frame's
    // STOCK arm (kAtomStockExecutionBit: a stock item in view) the late pass draws into the Atom view's depth (its
    // RTV's), the pyramid is built from the scene's — the first pass's surfaces and the stock depth drawn between
    // the passes — and the receivers read the Atom-only one, the Atom view's; in the other arm both are the scene's
    // depth, as before the stock depth existed.
    Ogre::TextureGpu *sceneDepth = depthTex, *atomDepth = depthTex;
    if (hzb) {
        const Ogre::CompositorNode *node = pass->getParentNode();
        const bool stockArm = (node->getWorkspace()->getExecutionMask() & kAtomStockExecutionBit) != 0u;
        JAH_TRY {
            sceneDepth = node->getDefinedTexture(Ogre::IdString(kSceneDepthTexture));
            if (!late && stockArm) atomDepth = node->getDefinedTexture(Ogre::IdString(kAtomViewDepthTexture));
        }
        catch (const Ogre::Exception &) { sceneDepth = atomDepth = nullptr; }
        if (!sceneDepth || !atomDepth) hzb = nullptr;
    }
    // THE LATE PASS BUILDS THE PYRAMID FIRST, from the scene's depth as it stands: the first pass's and the stock
    // depth (every level the texture has: a resize is no new graph). Built every frame the chain carries the
    // occlusion, drawn or not — it is the next frame's history, and it holds the stock walls.
    bool pyramidBuilt = false;
    if (late && hzb) pyramidBuilt = recordOcclusionPyramid(ctx.renderSystem, sceneDepth, hzb);
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
            const bool usable = !capture && hzb && hist.valid &&
                                hist.generation == view->workspaceGeneration() &&
                                hist.scene == scene && hist.width == hzb->getWidth() &&
                                hist.height == hzb->getHeight() && hist.levels == hzb->getNumMipmaps() &&
                                std::equal(rect, rect + 4, hist.rect);
            if (!capture) view->setAtomFirstTested(usable);
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
        // THE VIEW'S THRESHOLD (ATOM-3 stage 1): the World Mode's pixel-error row
        // (Scene::setPixelTolerance; kLodBudgetPixels until pushed), scaled by the scene's LOD
        // bias (OgreScene::applyLodValues divides the baked thresholds by it — the same dial
        // seen from the other side). The stock chain takes the same row through the view
        // camera's LOD factor (OgreView::syncLodTolerance), so the two routes cut alike.
        req.pixelTolerance = scene->pixelTolerance() * scene->lodBias();
        // NO SWITCH BAND (D4): the band exists because a WHOLE OBJECT switching pops;
        // a cut whose every group is judged under one sample of error changes its
        // frontier invisibly by construction (Nanite has none). THE CUT (mode 3).
        req.lodHysteresis = 0.0f;
        req.mode = 3u;
        cullPtr = capture ? &view->atomCullCapture() : late ? &view->atomCullLate() : &view->atomCull();
        // The capture's list holds the frame the view's first list was sized for.
        if (capture) cullPtr->followCutBudget(view->atomCull());
        if (late && !testAgainst) {
            // THE FIRST PASS TESTED NOTHING: nothing was rejected, there is nothing to
            // draw — the frame is the frustum-only frame. Its half of the ring says so.
            cycleStats(ctx, *D, device, vkRs->getVaoManager(), view, nullptr, true);
            draw = false;
        } else {
            // ---- (0) THE LIST'S WRITE-AFTER-READ: the previous frame's draw read it
            // (the commands, the compacted stream as its index buffer, the slot bases in
            // the vertex stage and the triangle words in the fragment stage). ----
            {
                VkCommandBuffer cmd = ctx.frameCmd();
                vkCmdPipelineBarrier(cmd,
                                     VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                                     nullptr, 0, nullptr, 0, nullptr);
            }
            // ---- (1) THE CULL, into the pass's own list ---------------------------
            std::string err;
            if (!capture) cycleStats(ctx, *D, device, vkRs->getVaoManager(), view, cullPtr, late);   // before this request zeroes them
            // NO DAG-BEARING MESH ATTACHED YET (a new project's first frames): no cluster
            // tables, so there is no cut to record — the depth is still cleared below.
            gs->flushClusterTables();
            if (late) cullPtr->followCutBudget(view->atomCull());
            if (!gs->clusterBuffer() || !gs->groupBuffer()) {
                draw = false;
            } else if (!scene->recordGpuCull(*cullPtr, req, testAgainst, err, nullptr,
                                             late ? &view->atomCull() : nullptr,
                                             OgreScene::CullRows::IdPass, nullptr,
                                             capture ? nullptr : atomCutProbeBuffer(view, late))) {
                logOnce("the cull did not record (" + err + ")");
                draw = false;
            }
        }
        if (late) {
            // THE HISTORY THE NEXT FRAME'S FIRST CULL TESTS AGAINST: the pyramid of this
            // frame (built from the first pass's depth and the stock depth, drawn with this matrix).
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
    if (draw && !capture && view) {
        // THE RECEIVERS' DEPTH (stage 4b): this frame's, for the sun maps that render after — the id pass's own
        // (with the occlusion the Atom view's depth, which holds both passes' surfaces by then).
        AtomReceiverDepth &rd = view->atomReceiverDepth();
        rd.depth = atomDepth;
        rd.drawMatrix = vpm;
        std::copy(rect, rect + 4, rd.rect);
        rd.frame = vkRs->getVaoManager()->getFrameCount();
        rd.workspace = pass->getParentNode()->getWorkspace();
    }
    if (draw) {
        // ---- (2) THE EDGES THE SOLVER CANNOT EXPRESS -----------------------------
        VkCommandBuffer cmd = ctx.frameCmd();
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
    VkCommandBuffer cmd = ctx.frameCmd();
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
                      D->ids.pipeline[rpd->requiresTextureFlipping() ? 1 : 0]);
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
        addressOf(D->ids, gs->instanceBuffer(), pc.instances);
        addressOf(D->ids, gs->meshBuffer(), pc.meshes);
        addressOf(D->ids, gs->geomBuffer(), pc.rows);
        addressOf(D->ids, cull.cutTriWords(), pc.triWords);
        addressOf(D->ids, cull.cutSlotBase(), pc.slotBase);
        const uint64_t masks = gs->maskTableAddress();
        pc.maskTable[0] = uint32_t(masks & 0xFFFFFFFFu);
        pc.maskTable[1] = uint32_t(masks >> 32u);
        vkCmdPushConstants(cmd, D->ids.layout, kIdPushStages, 0, sizeof(pc), &pc);
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
        D->ids.drawIndexedIndirectCount(cmd, drawBuf, drawOff, countBuf, countOff, maxDraws,
                                     GpuCull::kDrawWords * sizeof(uint32_t));
        // THE OTHER RANGES (ATOM-TWO-SIDED-1; ENGINE-POLISH-ATOM-1 stage 3): 1 the survivors flagged
        // kGpuTwoSided, both faces rasterised and resolved by the depth test; 2 the CUT-OUTS (kGpuAlphaTested),
        // discarded at their masks, culled as the pass culls; 3 the two-sided cut-outs — the same push constants,
        // viewport and index buffer (one layout), only the pipeline differs. Each recorded only while the scene has
        // such an item (AtomComponent::hasIdRange).
        const bool flip = rpd->requiresTextureFlipping();
        const unsigned rangePipe[4] = { 0u, 2u, flip ? 4u : 3u, 5u };
        for (unsigned rg = 1u; rg < GpuCull::kRanges; ++rg) {
            if (!scene->atom().hasIdRange(rg)) continue;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, D->ids.pipeline[rangePipe[rg]]);
            D->ids.drawIndexedIndirectCount(
                cmd, drawBuf, drawOff + VkDeviceSize(cull.rangeFirst(rg)) * GpuCull::kDrawWords * sizeof(uint32_t),
                countBuf, countOff + GpuCull::rangeCountOffsetBytes(rg), maxDraws,
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

/// THE MATRIX A CULL TESTS A PYRAMID WITH (cullMatrixOf above), for the caster pass's own pyramids
/// (OgreAtomCasterPass.cpp: a sun map draws through a positive-height viewport too).
Ogre::Matrix4 pyramidCullMatrix(const Ogre::Matrix4 &draw) { return cullMatrixOf(draw); }

bool atomIdPassSupported(Ogre::RenderSystem *rs) {
    Ogre::VulkanRenderSystem *vkRs = atomVulkanOf(rs);
    AtomIdDevice *d = idDev();
    if (!vkRs || !vkRs->getVulkanDevice() || !d || d->ids.refused) return false;
    Ogre::VulkanDevice *dv = vkRs->getVulkanDevice();
    return dv->hasBufferDeviceAddress() &&
           dv->hasDeviceExtension(Ogre::IdString(VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) &&
           rs->supportsIndirectDispatch();
}

void registerAtomIdPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomIdPassId, [](AtomPassContext &ctx) { recordIdPass(ctx, false); });
        p->setRecorder(kAtomIdLatePassId, [](AtomPassContext &ctx) { recordIdPass(ctx, true); });
    }
}

/// After every view (whose rings went to the device's window as each view died) and
/// before Root: one idle device, then the window and the pipelines.
void releaseAtomIdPass() {
    if (AtomPassProvider *p = AtomPassProvider::instance()) {
        p->setRecorder(kAtomIdPassId, AtomPassRecorder());
        p->setRecorder(kAtomIdLatePassId, AtomPassRecorder());
    }
    AtomIdDevice *d = idDev();
    if (!d) return;
    IdPipeline &ids = d->ids;
    if (ids.dev) {
        vkDeviceWaitIdle(ids.dev);
        for (auto &g : d->grave) destroyRing(ids, g.first);
        d->grave.clear();
        destroyPipelineObjects(ids);
        if (ids.layout) vkDestroyPipelineLayout(ids.dev, ids.layout, nullptr);
    }
    ids = IdPipeline();
}

void AtomIdViewRingDelete::operator()(AtomIdViewRing *p) const {
    if (!p) return;
    // The ring may be the target of a copy still in flight: to the device's window,
    // reaped once its last frame has retired. Without the device state (no Atom Hlms)
    // nothing was ever created in it.
    AtomIdDevice *d = idDev();
    if (d && p->ring.buffer) d->grave.emplace_back(p->ring, currentFrame());
    delete p;
}

void AtomIdDeviceDelete::operator()(AtomIdDevice *p) const { delete p; }

AtomIdDevice &HlmsAtom::idDevice() {
    if (!mIdDevice) mIdDevice.reset(new AtomIdDevice());
    return *mIdDevice;
}

#else   // !JAH_RAY_QUERY — no raw Vulkan here (macOS): every view keeps the Atom queue on PBS.

struct AtomIdDevice {};
struct AtomIdViewRing {};
void AtomIdViewRingDelete::operator()(AtomIdViewRing *p) const { delete p; }
void AtomIdDeviceDelete::operator()(AtomIdDevice *p) const { delete p; }
AtomIdDevice &HlmsAtom::idDevice() {
    if (!mIdDevice) mIdDevice.reset(new AtomIdDevice());
    return *mIdDevice;
}
bool atomIdPassSupported(Ogre::RenderSystem *) { return false; }
void registerAtomIdPass() {}
void releaseAtomIdPass() {}

#endif

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
