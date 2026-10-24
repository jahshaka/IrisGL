// THE GPU CHECKPOINTS (GpuCheckpoints.h). Why: a warm launch lost the device (Xid 109, CTX SWITCH TIMEOUT) on ONE
// dispatch of one frame, and no CPU-side door can name a single hung dispatch - the fence wait that would see it
// returns DEVICE_LOST. VK_NV_device_diagnostic_checkpoints keeps, per queue, the last mark each pipeline stage
// reached; read after the loss, the mark at the top of the pipe past the last one at its bottom is the dispatch that
// never finished.
#include "GpuCheckpoints.h"

#include "OgreVulkanDevice.h"
#include "OgreVulkanQueue.h"
#include "OgreVulkanRenderSystem.h"

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreLogManager.h>
#include <OgreRoot.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace jahshaka {
namespace engine {
namespace detail {
namespace {

/// A mark: its name and when it was made (the GPU keeps the pointer; the ring keeps the entry alive and recent).
struct Mark {
    const char *name = nullptr;
    unsigned long long frame = 0ull;
    unsigned long long seq = 0ull;
};
constexpr size_t kRing = 16384u;   // marks a frame are a few hundred: the last ones are always in the ring
Mark gRing[kRing];
std::atomic<unsigned long long> gSeq{ 0ull };

bool gConfigured = false, gWanted = false, gResolved = false, gOn = false, gDumped = false;
PFN_vkCmdSetCheckpointNV gSet = nullptr;
PFN_vkGetQueueCheckpointDataNV gGet = nullptr;
std::mutex gNamesMutex;
std::unordered_map<std::string, std::string *> gNames;   // interned: a job's name string may not outlive the mark

Ogre::VulkanDevice *device() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    auto *vk = root ? dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem()) : nullptr;
    return vk ? vk->getVulkanDevice() : nullptr;
}

const char *intern(const char *name) {
    std::lock_guard<std::mutex> lock(gNamesMutex);
    auto it = gNames.find(name);
    if (it != gNames.end()) return it->second->c_str();
    std::string *s = new std::string(name);   // kept for the process's life (a few hundred names)
    gNames.emplace(*s, s);
    return s->c_str();
}

bool resolve() {
    if (gResolved) return gOn;
    Ogre::VulkanDevice *d = device();
    if (!d || !d->mDevice) return false;   // no device yet: try again at the next mark
    gResolved = true;
    const Ogre::IdString want(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    bool enabled = false;
    for (const Ogre::IdString &e : d->mDeviceExtensions) enabled = enabled || e == want;
    if (enabled) {
        gSet = reinterpret_cast<PFN_vkCmdSetCheckpointNV>(vkGetDeviceProcAddr(d->mDevice, "vkCmdSetCheckpointNV"));
        gGet = reinterpret_cast<PFN_vkGetQueueCheckpointDataNV>(
            vkGetDeviceProcAddr(d->mDevice, "vkGetQueueCheckpointDataNV"));
    }
    gOn = enabled && gSet && gGet;
    Ogre::LogManager::getSingleton().logMessage(
        gOn ? "Jahshaka GPU checkpoints: ON (VK_NV_device_diagnostic_checkpoints) - every dispatch and pass is marked"
            : "Jahshaka GPU checkpoints: REQUESTED but the device has no VK_NV_device_diagnostic_checkpoints - off",
        Ogre::LML_CRITICAL);
    return gOn;
}

void mark(VkCommandBuffer cmd, const char *name) {
    if (!cmd) return;
    const unsigned long long seq = gSeq.fetch_add(1ull);
    Mark &m = gRing[seq % kRing];
    m.name = name;
    m.seq = seq;
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    m.frame = root ? root->getNextFrameNumber() : 0ull;
    gSet(cmd, &m);
}

void dispatchHook(const Ogre::HlmsComputeJob *job) {
    if (!gWanted || !resolve() || !job) return;
    gpuCheckpoint(intern(job->getNameStr().c_str()));
}

const char *stageName(VkPipelineStageFlagBits s) {
    switch (s) {
    case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT: return "TOP_OF_PIPE";
    case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT: return "BOTTOM_OF_PIPE";
    default: return "a stage";
    }
}

void dumpQueue(const char *which, VkQueue q) {
    if (!q) return;
    uint32_t n = 0u;
    gGet(q, &n, nullptr);
    std::vector<VkCheckpointDataNV> data(n);
    for (VkCheckpointDataNV &c : data) {
        c.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV;
        c.pNext = nullptr;
    }
    if (n) gGet(q, &n, data.data());
    char line[512];
    std::snprintf(line, sizeof(line), "Jahshaka GPU checkpoints after the device loss - %s queue: %u mark(s)", which, n);
    Ogre::LogManager::getSingleton().logMessage(line, Ogre::LML_CRITICAL);
    std::fprintf(stderr, "%s\n", line);
    for (const VkCheckpointDataNV &c : data) {
        const Mark *m = static_cast<const Mark *>(c.pCheckpointMarker);
        const char *nm = m && m->name ? m->name : "?";
        if (const char *slash = std::strrchr(nm, '/')) nm = slash + 1;   // a site's path: its file name
        std::snprintf(line, sizeof(line), "  %s (0x%x): '%s' frame %llu mark %llu", stageName(c.stage), unsigned(c.stage),
                      nm, m ? m->frame : 0ull, m ? m->seq : 0ull);
        Ogre::LogManager::getSingleton().logMessage(line, Ogre::LML_CRITICAL);
        std::fprintf(stderr, "%s\n", line);
    }
}

/// THE PASSES' MARKS: every compositor pass of every workspace, named as the frame monitor names it (the
/// definition's profiling id, else its type).
struct PassMarks : Ogre::CompositorWorkspaceListener {
    void passPreExecute(Ogre::CompositorPass *pass) override {
        if (!gWanted || !resolve() || !pass) return;
        const Ogre::CompositorPassDef *def = pass->getDefinition();
        std::string name = def && !def->mProfilingId.empty() ? def->mProfilingId
                                                              : "pass type " + std::to_string(int(pass->getType()));
        gpuCheckpoint(intern(("pass: " + name).c_str()));
    }
};
PassMarks gPassMarks;
bool gPassMarksInstalled = false;

}  // namespace

void gpuCheckpointsInstallPassListener(void *compositorManager2) {
    if (!gWanted || gPassMarksInstalled || !compositorManager2) return;
    static_cast<Ogre::CompositorManager2 *>(compositorManager2)->addPassListener(&gPassMarks);
    gPassMarksInstalled = true;
}

void gpuCheckpointsUninstallPassListener(void *compositorManager2) {
    if (!gPassMarksInstalled || !compositorManager2) return;
    static_cast<Ogre::CompositorManager2 *>(compositorManager2)->removePassListener(&gPassMarks);
    gPassMarksInstalled = false;
}

void gpuCheckpointsConfigure() {
    if (gConfigured) return;
    gConfigured = true;
    const char *v = std::getenv("JAHSHAKA_GPU_CHECKPOINTS");
    gWanted = v && *v && std::string(v) != "0";
    Ogre::VulkanDevice::msDiagnosticCheckpointsAllowed = gWanted;
    Ogre::HlmsCompute::msJahDispatchHook = gWanted ? &dispatchHook : nullptr;
}

bool gpuCheckpointsOn() { return gWanted && resolve(); }

void gpuCheckpoint(const char *name) {
    if (!gWanted || !resolve()) return;
    Ogre::VulkanDevice *d = device();
    if (!d || !d->mJahRecordQueue) return;
    mark(d->_jahRecordQueue().getCurrentCmdBuffer(), name);
}

void gpuCheckpointOn(void *vkCommandBuffer, const char *name) {
    if (!gWanted || !resolve()) return;
    mark(static_cast<VkCommandBuffer>(vkCommandBuffer), name);
}

void gpuCheckpointsDump() {
    if (!gWanted || gDumped || !resolve()) return;
    gDumped = true;
    Ogre::VulkanDevice *d = device();
    if (!d) return;
    dumpQueue("the graphics", d->mGraphicsQueue.mQueue);
    for (size_t i = 0; i < d->mComputeQueues.size(); ++i) dumpQueue("a compute", d->mComputeQueues[i].mQueue);
    for (size_t i = 0; i < d->mTransferQueues.size(); ++i) dumpQueue("a transfer", d->mTransferQueues[i].mQueue);
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
