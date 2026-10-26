// THE GPU CHECKPOINTS (GpuCheckpoints.h). Why: a warm launch lost the device (Xid 109, CTX SWITCH TIMEOUT) on ONE
// dispatch of one frame, and no CPU-side door can name a single hung dispatch - the fence wait that would see it
// returns DEVICE_LOST. VK_NV_device_diagnostic_checkpoints keeps, per queue, the last mark each pipeline stage
// reached. A mark PRECEDES its dispatch; BOTTOM_OF_PIPE holds the last mark whose preceding work has ALL RETIRED and
// TOP_OF_PIPE the last mark the front end processed - so THE HUNG DISPATCH IS THE ONE RIGHT AFTER THE BOTTOM_OF_PIPE
// MARK, named by that mark (the work between it and the TOP mark was issued and never retired; a dispatch after the
// TOP mark waits behind its barrier).
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
#include <OgreTextureGpuManager.h>
#include "OgreVulkanTextureGpu.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
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

// ---- THE DEVICE FAULT REPORT (PHOTON-7 stage 10-3) -------------------------------------------------------------
// Every memory bind and unbind the driver reports (VK_EXT_device_address_binding_report, through the instance's
// messenger the fork installs from the first bind: VulkanInstance::msJahAddressBindingCallback), kept in a bounded
// record, and after a device loss the driver's own fault record (VK_EXT_device_fault) with each faulting address
// mapped to the bindings that held it.
struct Binding {
    unsigned long long base = 0ull, size = 0ull, handle = 0ull, frame = 0ull, seq = 0ull;
    VkObjectType type = VK_OBJECT_TYPE_UNKNOWN;
    bool bind = true;
    std::string name;
};
constexpr size_t kBindingCap = 262144u;   // a session's binds are tens of thousands: the recent ones always stay
std::mutex gBindingsMutex;
std::deque<Binding> gBindings;
unsigned long long gBindingSeq = 0ull;

VKAPI_ATTR VkBool32 VKAPI_CALL addressBindingCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                      VkDebugUtilsMessageTypeFlagsEXT types,
                                                      const VkDebugUtilsMessengerCallbackDataEXT *data, void *) {
    if (!data || !(types & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT)) return VK_FALSE;
    const VkDeviceAddressBindingCallbackDataEXT *b = nullptr;
    for (const VkBaseInStructure *n = static_cast<const VkBaseInStructure *>(data->pNext); n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT) {
            b = reinterpret_cast<const VkDeviceAddressBindingCallbackDataEXT *>(n);
            break;
        }
    if (!b) return VK_FALSE;
    Binding r;
    r.base = b->baseAddress;
    r.size = b->size;
    r.bind = b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
    if (data->objectCount && data->pObjects) {
        r.type = data->pObjects[0].objectType;
        r.handle = data->pObjects[0].objectHandle;
        if (data->pObjects[0].pObjectName) r.name = data->pObjects[0].pObjectName;
    }
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    r.frame = root ? root->getNextFrameNumber() : 0ull;
    std::lock_guard<std::mutex> lock(gBindingsMutex);
    r.seq = gBindingSeq++;
    gBindings.push_back(std::move(r));
    if (gBindings.size() > kBindingCap) gBindings.pop_front();
    return VK_FALSE;
}

const char *objectTypeName(VkObjectType t) {
    switch (t) {
    case VK_OBJECT_TYPE_IMAGE: return "image";
    case VK_OBJECT_TYPE_BUFFER: return "buffer";
    case VK_OBJECT_TYPE_DEVICE_MEMORY: return "memory";
    case VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR: return "acceleration structure";
    default: return "object";
    }
}

const char *faultAddressTypeName(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "an invalid READ";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "an invalid WRITE";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "an invalid EXECUTE";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "an instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "an instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "an instruction pointer (the fault)";
    default: return "an address";
    }
}

void faultLine(const char *line) {
    Ogre::LogManager::getSingleton().logMessage(line, Ogre::LML_CRITICAL);
    std::fprintf(stderr, "%s\n", line);
}

/// A live Ogre texture's name by its image (the device is lost; the CPU objects are not).
std::string liveTextureNameOf(unsigned long long image) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::TextureGpuManager *tm = root && root->getRenderSystem() ? root->getRenderSystem()->getTextureGpuManager()
                                                                  : nullptr;
    if (!tm || !image) return std::string();
    for (const auto &kv : tm->getEntries()) {
        auto *t = dynamic_cast<Ogre::VulkanTextureGpu *>(kv.second.texture);
        if (t && reinterpret_cast<unsigned long long>(t->getFinalTextureName()) == image) return kv.second.name;
    }
    return std::string();
}

/// Every recorded binding event that overlaps [lo, hi), the last few, oldest first, each named.
void dumpBindingsOf(unsigned long long lo, unsigned long long hi) {
    std::vector<Binding> hits;
    std::unordered_map<unsigned long long, bool> liveOf;   // handle -> its last event was a bind
    {
        std::lock_guard<std::mutex> lock(gBindingsMutex);
        for (const Binding &b : gBindings) {
            liveOf[b.handle] = b.bind;
            if (b.size && b.base < hi && lo < b.base + b.size) hits.push_back(b);
        }
    }
    char line[768];
    if (hits.empty()) {
        std::snprintf(line, sizeof(line), "    no recorded binding overlaps [0x%llx, 0x%llx) (%zu bindings recorded)", lo,
                      hi, gBindings.size());
        faultLine(line);
        return;
    }
    const size_t first = hits.size() > 8u ? hits.size() - 8u : 0u;
    for (size_t i = first; i < hits.size(); ++i) {
        const Binding &b = hits[i];
        std::string name = b.name;
        if (name.empty() && b.type == VK_OBJECT_TYPE_IMAGE) name = liveTextureNameOf(b.handle);
        std::snprintf(line, sizeof(line), "    %s %s 0x%llx [0x%llx, +%llu) at frame %llu (event %llu)%s%s%s",
                      b.bind ? "BIND  " : "UNBIND", objectTypeName(b.type), b.handle, b.base, b.size, b.frame, b.seq,
                      name.empty() ? "" : " '", name.c_str(), name.empty() ? "" : "'");
        faultLine(line);
    }
    for (size_t i = first; i < hits.size(); ++i)
        if (liveOf[hits[i].handle] && hits[i].bind) {
            std::snprintf(line, sizeof(line), "    -> live there now: %s 0x%llx", objectTypeName(hits[i].type),
                          hits[i].handle);
            faultLine(line);
        }
}

void deviceFaultDump(Ogre::VulkanDevice *d) {
    char line[1024];
    if (!d || !d->mDevice || !d->mJahDeviceFault) {
        faultLine("Jahshaka device fault: no report (the device has no VK_EXT_device_fault deviceFault)");
        return;
    }
    auto get = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(vkGetDeviceProcAddr(d->mDevice, "vkGetDeviceFaultInfoEXT"));
    if (!get) {
        faultLine("Jahshaka device fault: no vkGetDeviceFaultInfoEXT entry point");
        return;
    }
    VkDeviceFaultCountsEXT counts{};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    VkResult r = get(d->mDevice, &counts, nullptr);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        std::snprintf(line, sizeof(line), "Jahshaka device fault: the counts query returned %d", int(r));
        faultLine(line);
        return;
    }
    if (!d->mJahDeviceFaultVendorBinary) counts.vendorBinarySize = 0u;
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
    std::vector<char> binary(size_t(counts.vendorBinarySize));
    VkDeviceFaultInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
    info.pVendorBinaryData = binary.empty() ? nullptr : binary.data();
    r = get(d->mDevice, &counts, &info);
    std::snprintf(line, sizeof(line),
                  "Jahshaka device fault (VK_EXT_device_fault, result %d): '%s' - %u address(es), %u vendor record(s), "
                  "%llu vendor binary bytes (deviceFaultVendorBinary %s); %zu bindings recorded "
                  "(VK_EXT_device_address_binding_report %s; VK_NV_device_diagnostics_config %s)",
                  int(r), info.description, counts.addressInfoCount, counts.vendorInfoCount,
                  (unsigned long long)counts.vendorBinarySize, d->mJahDeviceFaultVendorBinary ? "on" : "OFF",
                  gBindings.size(), d->mJahAddressBindingReport ? "on" : "OFF", d->mJahDiagnosticsConfig ? "on" : "OFF");
    faultLine(line);
    for (uint32_t i = 0; i < counts.addressInfoCount; ++i) {
        const VkDeviceFaultAddressInfoEXT &a = addresses[i];
        const unsigned long long prec = a.addressPrecision ? a.addressPrecision : 1ull;
        const unsigned long long lo = a.reportedAddress & ~(prec - 1ull), hi = lo + prec;
        std::snprintf(line, sizeof(line), "  address %u: %s at 0x%llx (precision %llu: [0x%llx, 0x%llx))", i,
                      faultAddressTypeName(a.addressType), (unsigned long long)a.reportedAddress, prec, lo, hi);
        faultLine(line);
        dumpBindingsOf(lo, hi);
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; ++i) {
        std::snprintf(line, sizeof(line), "  vendor %u: '%s' code 0x%llx data 0x%llx", i, vendors[i].description,
                      (unsigned long long)vendors[i].vendorFaultCode, (unsigned long long)vendors[i].vendorFaultData);
        faultLine(line);
    }
    if (!binary.empty()) {
        const char *path = std::getenv("JAHSHAKA_GPU_FAULT_BINARY");
        if (path && *path) {
            std::ofstream f(path, std::ios::binary);
            f.write(binary.data(), std::streamsize(binary.size()));
            std::snprintf(line, sizeof(line), "  the vendor binary (%zu bytes) written to %s", binary.size(), path);
        } else {
            std::snprintf(line, sizeof(line), "  the vendor binary (%zu bytes) kept nowhere: set JAHSHAKA_GPU_FAULT_BINARY",
                          binary.size());
        }
        faultLine(line);
    }
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

/// One of OUR marks (a slot of gRing), or a marker the driver made itself (VK_NV_device_diagnostics_config's automatic
/// checkpoints: an opaque value per draw / dispatch / copy, never ours to dereference).
const Mark *ourMark(const void *p) {
    const char *c = static_cast<const char *>(p), *b = reinterpret_cast<const char *>(gRing);
    return c >= b && c < b + sizeof(gRing) && (size_t(c - b) % sizeof(Mark)) == 0u ? static_cast<const Mark *>(p) : nullptr;
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
        const Mark *m = ourMark(c.pCheckpointMarker);
        if (!m) {   // the driver's automatic checkpoint (diagnostics config): its raw value
            std::snprintf(line, sizeof(line), "  %s (0x%x): a DRIVER checkpoint, marker 0x%llx", stageName(c.stage),
                          unsigned(c.stage), (unsigned long long)reinterpret_cast<uintptr_t>(c.pCheckpointMarker));
            Ogre::LogManager::getSingleton().logMessage(line, Ogre::LML_CRITICAL);
            std::fprintf(stderr, "%s\n", line);
            continue;
        }
        const char *nm = m->name ? m->name : "?";
        if (std::strstr(nm, ".cpp:"))   // a raw site's __FILE__:line - its file name (a job's name keeps its slashes)
            if (const char *slash = std::strrchr(nm, '/')) nm = slash + 1;
        std::snprintf(line, sizeof(line), "  %s (0x%x): '%s' frame %llu mark %llu", stageName(c.stage), unsigned(c.stage),
                      nm, m->frame, m->seq);
        Ogre::LogManager::getSingleton().logMessage(line, Ogre::LML_CRITICAL);
        std::fprintf(stderr, "%s\n", line);
    }
    // ...AND THE MARKS AROUND THE NEWEST ONE REACHED, in the CPU's recording order (the frame's dispatch order): the
    // hung dispatch with its neighbours
    unsigned long long newest = 0ull;
    for (const VkCheckpointDataNV &c : data)
        if (const Mark *m = ourMark(c.pCheckpointMarker)) newest = std::max(newest, m->seq);
    if (!data.empty()) {
        const unsigned long long last = gSeq.load();
        for (unsigned long long q = newest > 12ull ? newest - 12ull : 0ull; q < std::min(last, newest + 7ull); ++q) {
            const Mark &m = gRing[q % kRing];
            if (m.seq != q) continue;   // (overwritten: the ring moved on)
            const char *nm = m.name ? m.name : "?";
            if (std::strstr(nm, ".cpp:"))
                if (const char *slash = std::strrchr(nm, '/')) nm = slash + 1;
            std::snprintf(line, sizeof(line), "    %s mark %llu frame %llu: '%s'", q == newest ? "->" : "  ", q, m.frame, nm);
            Ogre::LogManager::getSingleton().logMessage(line, Ogre::LML_CRITICAL);
            std::fprintf(stderr, "%s\n", line);
        }
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
    // ...AND THE DEVICE FAULT REPORT in the same diagnostic mode: the fault record and the binding report.
    Ogre::VulkanDevice::msDeviceFaultAllowed = gWanted;
    Ogre::VulkanInstance::msJahAddressBindingCallback = gWanted ? &addressBindingCallback : nullptr;
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
    if (!gWanted || gDumped) return;
    gDumped = true;
    Ogre::VulkanDevice *d = device();
    if (!d) return;
    if (resolve()) {
        dumpQueue("the graphics", d->mGraphicsQueue.mQueue);
        for (size_t i = 0; i < d->mComputeQueues.size(); ++i) dumpQueue("a compute", d->mComputeQueues[i].mQueue);
        for (size_t i = 0; i < d->mTransferQueues.size(); ++i) dumpQueue("a transfer", d->mTransferQueues[i].mQueue);
    }
    deviceFaultDump(d);
}

}  // namespace detail
}  // namespace engine
}  // namespace jahshaka
