// THE OPENXR SESSION (SPECS/VR_SPEC.md v3 §4, phase 2) — the ONE TU in this
// engine that includes OpenXR, and the second (after OgreRayQuery.cpp) allowed
// to include Vulkan.
//
// WHAT IT IS. Phase 1a proved the hard half as a standalone binary
// (tests/vr/xr_spike.cpp, ~/Developer/spikes/openxr-vulkan/FINDINGS.md): an
// engine booted on a Vulkan device the RUNTIME created renders, byte for byte,
// what an engine on its own device renders, and a session paced by xrWaitFrame
// submits frames the runtime accepts. Phase 1b put that picture in a Quest Pro.
// This file is the same order, inside the engine, with the two things the
// spike did not have:
//
//   * INSTANCED STEREO instead of two renderOneFrame calls per frame. One
//     scene pass draws both eyes into one target two eyes wide, with the
//     per-eye view and projection taken from the camera's VrData
//     (ChainDesc::stereo -> chain::applyStereo). It is the first use of the
//     pin's instanced-stereo path on Vulkan (VR_SPEC §2.5).
//   * THE COPY INSIDE THE FRAME. The spike copied each eye into the runtime's
//     swapchain image AFTER renderOneFrame, which needed a BarrierSolver
//     ::assumeTransition to repair the tracking that the frame's own commit had
//     just reset (FINDINGS §4). Here the copy is recorded from a compositor
//     workspace listener, i.e. INSIDE the frame, where the solver still knows
//     the target was written — which is where the spike's own conclusion said
//     it belonged, and which needs no repair call at all.
//
// WHAT IT DELIBERATELY IS NOT. It renders INTO its own target and copies; it
// does not render into the runtime's images (VR_SPEC §2.4 B — a ~250-400 line
// SOURCE patch, phase 5 of the program). It submits ONE projection layer and
// no depth layer (WiVRn does not advertise the extension; the submit function
// takes the layer so Windows can turn it on without a refactor). It reads no
// input and locates no hands: phase 4's.
//
// THE CONSTRAINT THAT OUTRANKS EVERYTHING HERE (VR_SPEC §0): without a headset
// the tool is today's tool, unchanged. `EngineConfig::vr` defaults to Disabled,
// nothing below runs on a plain boot, and the desktop selftest hash does not
// move for VR work.
#include "EnginePrivate.h"

#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>

#if JAH_VR

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "OgreRectangle2D2.h"
#include "OgreTechnique.h"
#include "OgreMaterialManager.h"

#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanDevice.h"
#include "OgreVulkanQueue.h"
#include "OgreVulkanTextureGpu.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#endif  // JAH_VR

namespace jahshaka { namespace engine { namespace detail {

#if !JAH_VR

// ---------------------------------------------------------------------------
// NO LOADER, NO VULKAN RENDER SYSTEM, OR A PLATFORM WITHOUT EITHER (macOS
// today). The whole feature compiles to five refusals; `vrAvailable()` is
// false, every verb answers "no" with a reason, and nothing else in the engine
// knows the difference. This is the ray-query tier's rule: a build without the
// capability is a supported build, never a broken one.
namespace vr {
VrBoot *bootBegin(VrInfo &info, std::string &reason) {
    info = VrInfo();
    reason = "this build has no OpenXR support (the loader or the Vulkan render "
             "system headers were absent at configure time)";
    info.reason = reason;
    return nullptr;
}
void *bootExternalInstance(VrBoot *) { return nullptr; }
bool bootDevice(VrBoot *, Ogre::Root *, VrInfo &, std::string &reason) {
    reason = "this build has no OpenXR support";
    return false;
}
void *bootExternalDevice(VrBoot *) { return nullptr; }
void bootEnd(VrBoot *) {}
VrSession *sessionBegin(VrBoot *, OgreEngine *, OgreScene *, const VrConfig &,
                        std::string &reason) {
    reason = "this build has no OpenXR support";
    return nullptr;
}
void sessionEnd(VrSession *) {}
}  // namespace vr

// The engine's four session entry points, for the same reason.
bool vrSessionBeginFrame(VrSession *) { return true; }
void vrSessionEndFrame(VrSession *) {}
VrState vrSessionState(const VrSession *) { return VrState::Unavailable; }
VrStatus vrSessionStatus(const VrSession *) { return VrStatus(); }
View *vrSessionView(const VrSession *) { return nullptr; }
void vrSessionSetMirror(VrSession *, OgreView *) {}
bool vrSessionEyeScreenshot(VrSession *, unsigned, Image &, std::string &error) {
    error = "this build has no OpenXR support";
    return false;
}

#else   // JAH_VR

namespace {

/// THE SESSION'S DEFAULT CLIP PLANES (F10). A VR near plane is CLOSER than a
/// desktop one — a hand, a controller or a wall comes inside 10 cm and a
/// desktop default would clip it away — and the far plane matches the engine's
/// own CameraDesc default so the headset sees the distance the desktop sees.
/// They are only DEFAULTS: the pump reads the View's camera every frame, so a
/// host that pushes its own CameraDesc moves the headset's frustum with it.
constexpr float kVrDefaultNear = 0.05f;
constexpr float kVrDefaultFar = 1000.0f;

void vrLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vrLog(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Ogre::LogManager::getSingleton().logMessage(std::string("Jahshaka VR: ") + buf);
}

std::string xrResultName(XrInstance instance, XrResult r) {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf)))
        return buf;
    return std::to_string(int(r));
}

/// The standard OpenXR asymmetric projection in OGRE's convention ([-1,1]
/// depth, +Y up). `Camera::setCustomProjectionMatrix` hands it to
/// `RenderSystem::_convertProjectionMatrix`, which applies the Vulkan clip-space
/// correction (and our reverse depth) itself — so this must NOT pre-apply
/// either. Lifted verbatim from the phase-1a spike, where it was proved by the
/// "left eye == a mono render at that eye's projection" check, 0 of 3,609,088
/// bytes different.
Ogre::Matrix4 projectionFromFov(const XrFovf &fov, float zNear, float zFar) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float d = std::tan(fov.angleDown), u = std::tan(fov.angleUp);
    const float w = r - l, h = u - d;
    Ogre::Matrix4 m = Ogre::Matrix4::ZERO;
    m[0][0] = 2.0f / w;   m[0][2] = (r + l) / w;
    m[1][1] = 2.0f / h;   m[1][2] = (u + d) / h;
    m[2][2] = -(zFar + zNear) / (zFar - zNear);
    m[2][3] = -(2.0f * zFar * zNear) / (zFar - zNear);
    m[3][2] = -1.0f;
    return m;
}

Ogre::Quaternion toOgreQuat(const XrQuaternionf &q) {
    return Ogre::Quaternion(q.w, q.x, q.y, q.z);
}
Ogre::Vector3 toOgreVec(const XrVector3f &v) { return Ogre::Vector3(v.x, v.y, v.z); }

}   // namespace

// ===========================================================================
// VrBoot — the XrInstance, the VkInstance and the VkDevice.
//
// THE ORDER IS THE PRODUCT (phase 1a §8.5, the one correction the spike made to
// VR_SPEC §4.1): `loadPlugin` must come BEFORE buildDeviceCreationRequest,
// because ogre-patch 0068's exporter tests
// `VulkanInstance::hasExtension(VK_KHR_get_physical_device_properties2)` against
// the static list the RENDER SYSTEM'S CONSTRUCTOR fills. Build the request
// first and the feature chain is silently skipped — i.e. Ogre would compile
// shaders for features the device never enabled.
//
//   xrCreateInstance -> xrGetSystem -> xrGetVulkanGraphicsRequirements2KHR
//   -> xrCreateVulkanInstanceKHR (OUR VkInstanceCreateInfo)          [bootBegin]
//   -> Root -> loadPlugin(external_instance) -> setRenderSystem -> initialise
//   -> xrGetVulkanGraphicsDevice2KHR -> buildDeviceCreationRequest
//   -> xrCreateVulkanDeviceKHR (OGRE'S OWN VkDeviceCreateInfo)       [bootDevice]
//   -> createRenderWindow(external_device) -> Hlms -> scene -> xrCreateSession
// ===========================================================================
class VrBoot {
public:
    XrInstance   mInstance = XR_NULL_HANDLE;
    XrSystemId   mSystemId = XR_NULL_SYSTEM_ID;
    XrVersion    mApiVersion = 0;
    XrViewConfigurationView mViewCfg[2] = {};
    bool         mHasVisibilityMask = false;
    bool         mHasDepthLayer = false;

    VkInstance       mVkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice         mDevice = VK_NULL_HANDLE;
    VkQueue          mQueue = VK_NULL_HANDLE;
    uint32_t         mGraphicsFamily = uint32_t(-1);

    Ogre::VulkanExternalInstance      mExternalInstance;
    Ogre::VulkanExternalDevice        mExternalDevice;
    Ogre::VulkanDeviceCreationRequest mRequest;

    PFN_xrGetVulkanGraphicsRequirements2KHR GetVulkanGraphicsRequirements2 = nullptr;
    PFN_xrCreateVulkanInstanceKHR           CreateVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR       GetVulkanGraphicsDevice2 = nullptr;
    PFN_xrCreateVulkanDeviceKHR             CreateVulkanDevice = nullptr;

    ~VrBoot() {
        // WE destroy both, because Ogre destroys NEITHER an external instance
        // nor an external device (VR_SPEC §2.1 row 8). Called after Root is
        // deleted, which is the only order in which the device is free.
        if (mInstance != XR_NULL_HANDLE) xrDestroyInstance(mInstance);
        if (mDevice != VK_NULL_HANDLE) vkDestroyDevice(mDevice, nullptr);
        if (mVkInstance != VK_NULL_HANDLE) vkDestroyInstance(mVkInstance, nullptr);
    }

    bool begin(VrInfo &info, std::string &reason);
    bool device(Ogre::Root *root, VrInfo &info, std::string &reason);
};

bool VrBoot::begin(VrInfo &info, std::string &reason) {
    uint32_t extCount = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr))) {
        reason = "no OpenXR runtime answered (no manifest, or the loader found none)";
        return false;
    }
    std::vector<XrExtensionProperties> exts(extCount, { XR_TYPE_EXTENSION_PROPERTIES });
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    bool hasEnable2 = false;
    for (const auto &e : exts) {
        if (!std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) hasEnable2 = true;
        if (!std::strcmp(e.extensionName, "XR_KHR_visibility_mask")) mHasVisibilityMask = true;
        if (!std::strcmp(e.extensionName, "XR_KHR_composition_layer_depth")) mHasDepthLayer = true;
    }
    if (!hasEnable2) {
        reason = "the runtime does not advertise XR_KHR_vulkan_enable2";
        return false;
    }

    // THE API VERSION IS NEGOTIATED, NOT ASSUMED (the Oculus audit, ledger
    // §580): everything used here is OpenXR 1.0 core plus vulkan_enable2, and
    // Meta's PC runtime has no 1.1 conformance — so 1.1 is asked for and 1.0 is
    // the retry, on exactly XR_ERROR_API_VERSION_UNSUPPORTED.
    const char *want[] = {
        XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
        "XR_KHR_visibility_mask",
    };
    XrInstanceCreateInfo ici{ XR_TYPE_INSTANCE_CREATE_INFO };
    std::strncpy(ici.applicationInfo.applicationName, "Jahshaka",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    ici.applicationInfo.applicationVersion = 1;
    std::strncpy(ici.applicationInfo.engineName, "Jahshaka Engine", XR_MAX_ENGINE_NAME_SIZE - 1);
    ici.enabledExtensionCount = mHasVisibilityMask ? 2u : 1u;
    ici.enabledExtensionNames = want;

    const XrVersion ladder[2] = { XR_MAKE_VERSION(1, 1, 0), XR_MAKE_VERSION(1, 0, 0) };
    XrResult created = XR_ERROR_RUNTIME_FAILURE;
    for (XrVersion v : ladder) {
        ici.applicationInfo.apiVersion = v;
        created = xrCreateInstance(&ici, &mInstance);
        if (XR_SUCCEEDED(created)) { mApiVersion = v; break; }
        if (created != XR_ERROR_API_VERSION_UNSUPPORTED) break;
        vrLog("the runtime refused OpenXR %d.%d - trying the next one down",
              int(XR_VERSION_MAJOR(v)), int(XR_VERSION_MINOR(v)));
    }
    if (XR_FAILED(created)) {
        reason = "xrCreateInstance failed: " + xrResultName(XR_NULL_HANDLE, created);
        return false;
    }
    info.apiMajor = unsigned(XR_VERSION_MAJOR(mApiVersion));
    info.apiMinor = unsigned(XR_VERSION_MINOR(mApiVersion));

    // THE RUNTIME'S NAME AND VERSION, ALWAYS LOGGED (VR_SPEC §2.6's manifest
    // law): the user manifest is whatever a headset last wrote, so a transcript
    // that does not name its runtime cannot be read a week later.
    XrInstanceProperties ip{ XR_TYPE_INSTANCE_PROPERTIES };
    if (XR_SUCCEEDED(xrGetInstanceProperties(mInstance, &ip))) {
        info.runtime = ip.runtimeName;
        char v[32];
        std::snprintf(v, sizeof(v), "%d.%d.%d", int(XR_VERSION_MAJOR(ip.runtimeVersion)),
                      int(XR_VERSION_MINOR(ip.runtimeVersion)),
                      int(XR_VERSION_PATCH(ip.runtimeVersion)));
        info.runtimeVersion = v;
    }
    vrLog("runtime '%s' %s, OpenXR %u.%u, visibility_mask=%d depth_layer=%d",
          info.runtime.c_str(), info.runtimeVersion.c_str(), info.apiMajor, info.apiMinor,
          int(mHasVisibilityMask), int(mHasDepthLayer));
    info.visibilityMask = mHasVisibilityMask;
    info.depthLayer = mHasDepthLayer;

    XrSystemGetInfo sgi{ XR_TYPE_SYSTEM_GET_INFO };
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult sys = xrGetSystem(mInstance, &sgi, &mSystemId);
    if (XR_FAILED(sys)) {
        // THE ORDINARY "no headset is plugged in" ANSWER, and not an error:
        // WiVRn writes its manifest on connect, so a box with the runtime
        // installed and the cable out lands exactly here.
        reason = "no head-mounted display: " + xrResultName(mInstance, sys);
        return false;
    }
    XrSystemProperties sp{ XR_TYPE_SYSTEM_PROPERTIES };
    if (XR_SUCCEEDED(xrGetSystemProperties(mInstance, mSystemId, &sp)))
        info.system = sp.systemName;

    uint32_t viewCount = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(
            mInstance, mSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount,
            nullptr)) ||
        viewCount != 2u) {
        reason = "the system's primary view configuration is not stereo";
        return false;
    }
    for (auto &v : mViewCfg) v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(mInstance, mSystemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount,
                                      mViewCfg);
    info.eyeWidth  = mViewCfg[0].recommendedImageRectWidth;
    info.eyeHeight = mViewCfg[0].recommendedImageRectHeight;
    vrLog("system '%s', recommended %ux%u per eye", info.system.c_str(), info.eyeWidth,
          info.eyeHeight);

    xrGetInstanceProcAddr(mInstance, "xrGetVulkanGraphicsRequirements2KHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&GetVulkanGraphicsRequirements2));
    xrGetInstanceProcAddr(mInstance, "xrCreateVulkanInstanceKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&CreateVulkanInstance));
    xrGetInstanceProcAddr(mInstance, "xrGetVulkanGraphicsDevice2KHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&GetVulkanGraphicsDevice2));
    xrGetInstanceProcAddr(mInstance, "xrCreateVulkanDeviceKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&CreateVulkanDevice));
    if (!GetVulkanGraphicsRequirements2 || !CreateVulkanInstance || !GetVulkanGraphicsDevice2 ||
        !CreateVulkanDevice) {
        reason = "the XR_KHR_vulkan_enable2 entry points did not resolve";
        return false;
    }

    XrGraphicsRequirementsVulkan2KHR req{ XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    if (XR_FAILED(GetVulkanGraphicsRequirements2(mInstance, mSystemId, &req))) {
        reason = "xrGetVulkanGraphicsRequirements2KHR failed";
        return false;
    }
    // Ogre asks for a 1.2 instance when the loader allows it (ogre-patch 0038's
    // ray query needs 1.2), so 1.2 must fall inside the runtime's window. Monado
    // answers 1.0 .. 1023.1023.1023 and WiVRn the same; a runtime that capped
    // below 1.2 would be a real refusal and is reported as one.
    const XrVersion want12 = XR_MAKE_VERSION(1, 2, 0);
    if (want12 < req.minApiVersionSupported || want12 > req.maxApiVersionSupported) {
        reason = "the runtime does not admit a Vulkan 1.2 instance";
        return false;
    }

    // OUR VkInstanceCreateInfo, created BY THE RUNTIME. The extension list is
    // ours and not Ogre's on this path (VR_SPEC §2.1 row 3): the external branch
    // copies this struct's list verbatim, and `VK_KHR_xcb_surface` in it is what
    // decides whether the xcb window backend is usable at all — which the
    // desktop mirror needs.
    std::vector<const char *> instExt = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(__linux__) && !defined(__APPLE__)
        "VK_KHR_xcb_surface",
#endif
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "Jahshaka";
    app.pEngineName = "Ogre-Next";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo vici{};
    vici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vici.pApplicationInfo = &app;
    vici.enabledExtensionCount = uint32_t(instExt.size());
    vici.ppEnabledExtensionNames = instExt.data();

    XrVulkanInstanceCreateInfoKHR xrIci{ XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
    xrIci.systemId = mSystemId;
    xrIci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xrIci.vulkanCreateInfo = &vici;
    VkResult vkErr = VK_SUCCESS;
    const XrResult made = CreateVulkanInstance(mInstance, &xrIci, &mVkInstance, &vkErr);
    if (XR_FAILED(made) || vkErr != VK_SUCCESS || mVkInstance == VK_NULL_HANDLE) {
        reason = "xrCreateVulkanInstanceKHR failed (VkResult " + std::to_string(int(vkErr)) + ")";
        mVkInstance = VK_NULL_HANDLE;
        return false;
    }

    mExternalInstance.instance = mVkInstance;
    for (const char *e : instExt) {
        VkExtensionProperties p{};
        std::strncpy(p.extensionName, e, VK_MAX_EXTENSION_NAME_SIZE - 1);
        mExternalInstance.instanceExtensions.push_back(p);
    }
    return true;
}

bool VrBoot::device(Ogre::Root *root, VrInfo &info, std::string &reason) {
    (void)info;
    auto *vkRs = dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem());
    if (!vkRs) {
        reason = "the render system is not Vulkan";
        return false;
    }
    XrVulkanGraphicsDeviceGetInfoKHR gdi{ XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
    gdi.systemId = mSystemId;
    gdi.vulkanInstance = mVkInstance;
    if (XR_FAILED(GetVulkanGraphicsDevice2(mInstance, &gdi, &mPhysicalDevice))) {
        reason = "xrGetVulkanGraphicsDevice2KHR failed";
        return false;
    }
    VkPhysicalDeviceProperties pdp{};
    vkGetPhysicalDeviceProperties(mPhysicalDevice, &pdp);
    vrLog("the runtime picked '%s'", pdp.deviceName);

    // THE DEVICE OGRE WOULD HAVE BUILT (ogre-patch 0068 hunk 2): the exact
    // extension list and the exact VkPhysicalDeviceFeatures2 chain
    // `VulkanDevice::createDevice` would have passed, so the device the runtime
    // creates is the device Ogre believes it has. Without it Ogre reads the
    // feature bits back from what the PHYSICAL device supports and compiles
    // shaders for features nobody enabled (VR_SPEC §2.1 row 6).
    uint32_t numExt = 0;
    vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &numExt, nullptr);
    Ogre::FastArray<VkExtensionProperties> availExt;
    availExt.resize(numExt);
    vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &numExt, availExt.begin());
    Ogre::VulkanDevice::buildDeviceCreationRequest(mVkInstance, mPhysicalDevice, availExt,
                                                   mRequest);

    // ONE queue from the FIRST graphics family: what Ogre's own
    // `findGraphicsQueue` picks, and what `VulkanQueue::setExternalQueue` can
    // find again by matching vkGetDeviceQueue — it THROWS when it cannot.
    uint32_t numFam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &numFam, nullptr);
    std::vector<VkQueueFamilyProperties> fam(numFam);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &numFam, fam.data());
    for (uint32_t i = 0; i < numFam; ++i)
        if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { mGraphicsFamily = i; break; }
    if (mGraphicsFamily == uint32_t(-1)) {
        reason = "the device the runtime chose has no graphics queue family";
        return false;
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = mGraphicsFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(mRequest.extensions.size());
    dci.ppEnabledExtensionNames = mRequest.extensions.begin();
    if (mRequest.hasFeatures2) dci.pNext = mRequest.pNext();
    else                       dci.pEnabledFeatures = &mRequest.features;

    XrVulkanDeviceCreateInfoKHR xrDci{ XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
    xrDci.systemId = mSystemId;
    xrDci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xrDci.vulkanPhysicalDevice = mPhysicalDevice;
    xrDci.vulkanCreateInfo = &dci;
    VkResult vkErr = VK_SUCCESS;
    const XrResult made = CreateVulkanDevice(mInstance, &xrDci, &mDevice, &vkErr);
    if (XR_FAILED(made) || vkErr != VK_SUCCESS || mDevice == VK_NULL_HANDLE) {
        reason = "xrCreateVulkanDeviceKHR failed (VkResult " + std::to_string(int(vkErr)) + ")";
        mDevice = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(mDevice, mGraphicsFamily, 0, &mQueue);

    mExternalDevice.physicalDevice = mPhysicalDevice;
    mExternalDevice.device = mDevice;
    mExternalDevice.graphicsQueue = mQueue;
    mExternalDevice.presentQueue = mQueue;
    mExternalDevice.creationRequest = &mRequest;   // ogre-patch 0068: the ENABLED set
    for (const char *e : mRequest.extensions) {
        VkExtensionProperties p{};
        std::strncpy(p.extensionName, e, VK_MAX_EXTENSION_NAME_SIZE - 1);
        mExternalDevice.deviceExtensions.push_back(p);
    }
    vrLog("the runtime created the VkDevice from Ogre's own createInfo (%zu extensions)",
          mRequest.extensions.size());
    return true;
}

// ===========================================================================
// VrSession — the XrSession, the stereo View, the pump and the copy.
// ===========================================================================
class VrSession final : public Ogre::CompositorWorkspaceListener {
public:
    VrSession(VrBoot *boot, OgreEngine *engine, OgreScene *scene, const VrConfig &cfg)
        : mBoot(boot), mEngine(engine), mScene(scene), mConfig(cfg) {}
    /// NOT `override`: Ogre's CompositorWorkspaceListener has no virtual
    /// destructor (the ReflectPassListener rule). Nothing ever deletes one of
    /// these through the base pointer.
    ~VrSession();

    bool create(std::string &reason);

    // ---- the pump, called by OgreEngine::renderOneFrame -------------------
    /// True = draw this frame. False = the runtime asked for no picture (or
    /// the session is ending); the XR frame has already been closed.
    bool beginFrame();
    void endFrame();

    void workspacePosUpdate(Ogre::CompositorWorkspace *workspace) override;

    VrState state() const { return mState; }
    VrStatus status() const;
    OgreView *view() const { return mView; }
    void setMirrorView(OgreView *v);
    /// ONE EYE, RENDERED MONO — the reverse-Z detector and the VR screenshot
    /// (see the engine-side declaration on Engine::vrEyeScreenshot).
    bool eyeScreenshot(unsigned eye, Image &out, std::string &error);

private:
    void pollEvents();
    void applyState(XrSessionState s);
    void teardownMirror();
    void syncMirror();
    /// The session's own View, on for a frame the runtime wants a picture for
    /// and off for one it does not (F4).
    void setSessionViewEnabled(bool on);
    /// Has the Vulkan device gone? (F3 — the frame's commit is where a loss
    /// surfaces, and the engine's catch swallows it.)
    bool deviceLost() const;
    /// THE SCREEN QUADS THIS SESSION SWAPPED, so every one of them can be put
    /// back exactly as it was. The base material is held by STRONG reference:
    /// an owner that drops its material while a session runs must not free it
    /// under the quad that is about to have it back.
    struct StereoQuad {
        Ogre::Rectangle2D *quad = nullptr;
        Ogre::MaterialPtr  baseMaterial;
        Ogre::MaterialPtr  vrMaterial;
        /// Was this quad among the scene's live Rectangle2Ds this frame? The
        /// answer is how a destroyed quad is noticed at all (V2F-2).
        bool               seen = false;
    };

    /// THE THREE SCREEN QUADS, TAUGHT TO BE STEREO (F2). Gives each one a clone
    /// of its material pointed at the stereo vertex program, keeps that clone in
    /// step with what its owner writes, and pushes the eyes' rays into it.
    void syncStereoQuads();
    /// Puts every screen quad back the way it was found, so a material a
    /// session touched is exactly the material it was.
    void dropStereoQuads();
    /// Unregisters one quad's clone (not a restore — see its definition).
    void dropClone(StereoQuad &q);
    /// The one Vulkan routine: the two eye copies, recorded on the frame's own
    /// command buffer while the BarrierSolver still knows the target's state.
    void copyEyes();
    /// Everything that must be released before Ogre's objects go: the XR
    /// swapchains, the space and the session. Safe twice.
    void destroyXr();

    VrBoot     *mBoot;
    OgreEngine *mEngine;
    OgreScene  *mScene;
    VrConfig    mConfig;

    XrSession   mSession = XR_NULL_HANDLE;
    XrSpace     mSpace = XR_NULL_HANDLE;
    XrSwapchain mSwapchain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::vector<XrSwapchainImageVulkanKHR> mImages[2];
    uint32_t    mAcquired[2] = { 0u, 0u };
    bool        mHasAcquired[2] = { false, false };
    int64_t     mSwapchainFormat = 0;

    VrState     mState = VrState::Idle;
    bool        mRunning = false;     ///< between xrBeginSession and xrEndSession
    bool        mInFrame = false;     ///< between xrBeginFrame and xrEndFrame
    bool        mDrewThisFrame = false;
    XrFrameState mFrameState{ XR_TYPE_FRAME_STATE };
    XrView      mViews[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
    XrCompositionLayerProjectionView mProjViews[2] = {};

    unsigned    mEyeWidth = 0, mEyeHeight = 0;
    Ogre::VrData mVrData;
    /// The per-eye projections in OGRE's [-1,1] depth convention, BEFORE the
    /// render system's reverse-Z conversion (F1). VrData holds the converted
    /// pair because nothing on the Hlms path converts; this is what a caller
    /// that converts for itself (Camera::setCustomProjectionMatrix) needs.
    Ogre::Matrix4 mEyeProjection[2];
    /// ...and the same pair AFTER the render system's conversion, which is the
    /// convention every auto-param consumer works in (the screen quads
    /// unproject a point at `rs_depth_range`'s far value).
    Ogre::Matrix4 mEyeProjectionRS[2];
    Ogre::Vector3 mEyeWorldPos[2];
    /// BOTH eyes' four corner rays in world space (see the shader).
    Ogre::Vector3 mEyeCornerRay[2][4];
    Ogre::Quaternion mEyeWorldRot[2];
    bool        mHavePose = false;
    bool        mViewEnabled = true;
    std::vector<StereoQuad> mStereoQuads;
    OgreView   *mView = nullptr;
    Ogre::Camera *mCullCamera = nullptr;

    OgreView   *mMirrorView = nullptr;
    Ogre::CompositorWorkspace *mMirrorWorkspace = nullptr;
    std::vector<std::string> mMirrorNodeDefs;
    std::string mMirrorWorkspaceDef;
    unsigned    mMirrorGeneration = 0u;

    unsigned long long mFrames = 0ull, mRendered = 0ull;
    float       mIpd = 0.0f;
    bool        mAsymmetricFov = false;
    float       mRefreshHz = 0.0f;
};

// ---------------------------------------------------------------------------
bool VrSession::create(std::string &reason) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) { reason = "no Ogre::Root"; return false; }

    XrGraphicsBindingVulkan2KHR binding{ XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
    binding.instance = mBoot->mVkInstance;
    binding.physicalDevice = mBoot->mPhysicalDevice;
    binding.device = mBoot->mDevice;
    binding.queueFamilyIndex = mBoot->mGraphicsFamily;
    binding.queueIndex = 0;
    XrSessionCreateInfo sci{ XR_TYPE_SESSION_CREATE_INFO };
    sci.next = &binding;
    sci.systemId = mBoot->mSystemId;
    XrResult r = xrCreateSession(mBoot->mInstance, &sci, &mSession);
    if (XR_FAILED(r)) {
        reason = "xrCreateSession failed: " + xrResultName(mBoot->mInstance, r);
        mSession = XR_NULL_HANDLE;
        return false;
    }

    // STAGE SPACE WHERE THE RUNTIME HAS ONE, and this is not a preference — it
    // is phase 1b's measured lesson (ledger §585). STAGE's origin is on the
    // FLOOR, so a scene authored with its ground at y = 0 puts the wearer's
    // head where a head is; in LOCAL the Quest Pro's eyes located at y = -0.70
    // and the owner saw the underside of the floor and no scene at all. LOCAL
    // stays the fallback for a runtime that offers no stage.
    XrReferenceSpaceType spaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    {
        uint32_t n = 0;
        xrEnumerateReferenceSpaces(mSession, 0, &n, nullptr);
        std::vector<XrReferenceSpaceType> spaces(n);
        if (n) xrEnumerateReferenceSpaces(mSession, n, &n, spaces.data());
        for (XrReferenceSpaceType t : spaces)
            if (t == XR_REFERENCE_SPACE_TYPE_STAGE) spaceType = t;
    }
    XrReferenceSpaceCreateInfo rsci{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    rsci.referenceSpaceType = spaceType;
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    r = xrCreateReferenceSpace(mSession, &rsci, &mSpace);
    if (XR_FAILED(r)) {
        reason = "xrCreateReferenceSpace failed: " + xrResultName(mBoot->mInstance, r);
        return false;
    }
    mEngine->mVrInfo.space = spaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "stage" : "local";
    vrLog("reference space: %s", mEngine->mVrInfo.space.c_str());

    // THE SWAPCHAIN FORMAT, WITH NO SILENT FALLBACK (phase 1a fix round F4).
    // The copy is a vkCmdCopyImage, which demands format COMPATIBILITY — the
    // same texel block size — so a 4-byte eye target cannot be copied into
    // Monado's first preference (R16G16B16A16_UNORM, 8 bytes). We ask for the
    // UNORM 8-bit format because that is what this engine's own view targets
    // are (OgreView::createRtt, PFG_RGBA8_UNORM) and what its windows are
    // (VulkanWindow picks a non-sRGB format without the `gamma` param): the
    // HlmsPbs pixel shader does the linear->gamma conversion ITSELF when the
    // target is not sRGB (`@property( !hw_gamma_write ) outPs_colour0.xyz =
    // sqrt( finalColour )`), so our bytes are already display-encoded and a
    // pass-through is exactly right. Taking an _SRGB swapchain instead would
    // ask the runtime to encode them a second time.
    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(mSession, 0, &fmtCount, nullptr);
    std::vector<int64_t> formats(fmtCount);
    if (fmtCount) xrEnumerateSwapchainFormats(mSession, fmtCount, &fmtCount, formats.data());
    mSwapchainFormat = 0;
    for (int64_t f : formats)
        if (f == VK_FORMAT_R8G8B8A8_UNORM) { mSwapchainFormat = f; break; }
    if (!mSwapchainFormat) {
        reason = "the runtime does not offer VK_FORMAT_R8G8B8A8_UNORM (the eye target's format)";
        return false;
    }

    mEyeWidth  = mConfig.overrideEyeWidth  ? mConfig.overrideEyeWidth
                                           : mBoot->mViewCfg[0].recommendedImageRectWidth;
    mEyeHeight = mConfig.overrideEyeHeight ? mConfig.overrideEyeHeight
                                           : mBoot->mViewCfg[0].recommendedImageRectHeight;
    if (!mEyeWidth || !mEyeHeight) { reason = "the runtime recommends a zero eye size"; return false; }

    // THE SWAPCHAINS are always the RUNTIME's size; only the RENDER follows the
    // override, so a measurement at a foreign eye size stays a measurement of
    // our renderer and not of the runtime (VrConfig::overrideEyeWidth).
    const unsigned scW = mBoot->mViewCfg[0].recommendedImageRectWidth;
    const unsigned scH = mBoot->mViewCfg[0].recommendedImageRectHeight;
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo swci{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                          XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        swci.format = mSwapchainFormat;
        swci.sampleCount = 1;
        swci.width = scW; swci.height = scH;
        swci.faceCount = 1; swci.arraySize = 1; swci.mipCount = 1;
        r = xrCreateSwapchain(mSession, &swci, &mSwapchain[eye]);
        if (XR_FAILED(r)) {
            reason = "xrCreateSwapchain failed: " + xrResultName(mBoot->mInstance, r);
            return false;
        }
        uint32_t n = 0;
        xrEnumerateSwapchainImages(mSwapchain[eye], 0, &n, nullptr);
        mImages[eye].assign(n, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR });
        xrEnumerateSwapchainImages(
            mSwapchain[eye], n, &n,
            reinterpret_cast<XrSwapchainImageBaseHeader *>(mImages[eye].data()));
    }
    vrLog("swapchains: 2 x %ux%u, %zu images each; rendering %ux%u per eye", scW, scH,
          mImages[0].size(), mEyeWidth, mEyeHeight);

    // THE CULL CAMERA (VR_SPEC §2.5): one frustum for both eyes, so the two
    // eyes cull and light identically. Its projection is written per frame from
    // the union of the located fovs; it lives in the scene and dies with the
    // session.
    mCullCamera = mScene->sceneManager()->createCamera("JahshakaVrCullCamera");
    mCullCamera->setNearClipDistance(kVrDefaultNear);
    mCullCamera->setFarClipDistance(kVrDefaultFar);

    // THE VIEW. Offscreen, two eyes wide, and the ONE offscreen view in this
    // engine that keeps the post chain (PostFxDesc::allowOffscreen) — because
    // it is not a thumbnail, it is the picture the user is standing in.
    //
    // THE PHASE-2 PROFILE, stated here and nowhere else (VR_SPEC §9 item 6):
    // HDR and its tonemap ON, MSAA at 1 (HDR + MSAA segfaults this driver —
    // OgreChain.cpp's own note), SSAO / SMAA / SSR OFF because every one of
    // them samples a neighbourhood and would read across the seam between the
    // eyes, and ray-traced reflections off with SSR (they ride its chain).
    View *v = mEngine->createOffscreenView("jahshaka-vr", mEyeWidth * 2u, mEyeHeight,
                                           Colour{ 0.0f, 0.0f, 0.0f, 1.0f });
    if (!v) { reason = "createOffscreenView failed: " + mEngine->lastError(); return false; }
    mView = static_cast<OgreView *>(v);
    PostFxDesc fx;
    fx.allowOffscreen = true;
    fx.hdr = true;
    fx.bloom = false;
    fx.ssao = false;
    fx.smaaPreset = -1;
    fx.ssr = 0;
    mView->setPostFx(fx);
    mView->setSampleCount(1u);
    mView->setStereo(true, "JahshakaVrCullCamera");
    // THE GI DRIVER. Without this the cascades would follow whichever on-screen
    // view was created first — the editor's — and the headset would look at a
    // field centred somewhere else entirely (VR_SPEC §3.4).
    mView->setGiPriority(true);
    if (!mView->setScene(mScene)) {
        reason = "the VR view refused the scene: " + mEngine->lastError();
        return false;
    }
    CameraDesc cam;
    cam.nearClip = kVrDefaultNear;
    cam.farClip = kVrDefaultFar;
    mView->setCamera(cam);
    if (mView->camera()) mView->camera()->setVrData(&mVrData);
    mView->addWorkspaceListener(this);
    mView->setEnabled(true);

    vrLog("session created on the runtime's device");
    return true;
}

// ---------------------------------------------------------------------------
void VrSession::applyState(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE:         mState = VrState::Idle; break;
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{ XR_TYPE_SESSION_BEGIN_INFO };
            bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult r = xrBeginSession(mSession, &bi);
            if (XR_FAILED(r)) {
                vrLog("xrBeginSession failed: %s", xrResultName(mBoot->mInstance, r).c_str());
                mState = VrState::Lost;
                break;
            }
            mRunning = true;
            mState = VrState::Ready;
            break;
        }
        case XR_SESSION_STATE_SYNCHRONIZED: mState = VrState::Synchronized; break;
        case XR_SESSION_STATE_VISIBLE:      mState = VrState::Visible; break;
        case XR_SESSION_STATE_FOCUSED:      mState = VrState::Focused; break;
        case XR_SESSION_STATE_STOPPING:
            mState = VrState::Stopping;
            // THE RUNTIME ASKED US TO STOP. Never between xrBeginFrame and
            // xrEndFrame — the pump closes its frame before it polls again.
            xrEndSession(mSession);
            mRunning = false;
            mState = VrState::Idle;
            break;
        case XR_SESSION_STATE_LOSS_PENDING:
        case XR_SESSION_STATE_EXITING:
            mState = VrState::Lost;
            mRunning = false;
            break;
        default: break;
    }
}

void VrSession::pollEvents() {
    XrEventDataBuffer ev{ XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(mBoot->mInstance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *ss = reinterpret_cast<XrEventDataSessionStateChanged *>(&ev);
            applyState(ss->state);
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            // THE RUNTIME IS GOING AWAY (a WiVRn disconnect, a crashed
            // service). The session ends cleanly and stays ended: on an
            // EXTERNAL device this pin has no recovery path at all
            // (VR_SPEC §2.1 row 10), so the honest answer is Lost and a
            // restart, never a hang.
            vrLog("the OpenXR instance is going away - ending the session");
            mState = VrState::Lost;
            mRunning = false;
        }
        ev = { XR_TYPE_EVENT_DATA_BUFFER };
    }
}

// ---------------------------------------------------------------------------
bool VrSession::beginFrame() {
    if (mState == VrState::Lost) { teardownMirror(); setSessionViewEnabled(false); return true; }
    pollEvents();
    if (!mRunning) {
        // NOTHING HAS BEEN DRAWN INTO THE EYE TARGET YET, so a mirror would
        // paint black over the desktop's own picture. It appears when the
        // session starts producing frames and goes again when it stops.
        teardownMirror();
        setSessionViewEnabled(false);
        return true;
    }
    syncMirror();

    XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
    mFrameState = XrFrameState{ XR_TYPE_FRAME_STATE };
    // THE CLOCK. This is where a VR frame waits — not in a timer and not in a
    // swapchain acquire (VR_SPEC §4.3). The host's driver runs at a zero
    // interval with the mirror window's vsync off for the duration, so this
    // call is the only pacer.
    XrResult r = xrWaitFrame(mSession, &fwi, &mFrameState);
    if (XR_FAILED(r)) {
        vrLog("xrWaitFrame failed: %s", xrResultName(mBoot->mInstance, r).c_str());
        mState = VrState::Lost; mRunning = false;
        setSessionViewEnabled(false);
        return true;
    }
    if (mRefreshHz <= 0.0f && mFrameState.predictedDisplayPeriod > 0) {
        mRefreshHz = float(1.0e9 / double(mFrameState.predictedDisplayPeriod));
        mEngine->mVrInfo.refreshHz = mRefreshHz;
        vrLog("pacing: predictedDisplayPeriod %lld ns (%.1f Hz)",
              (long long)mFrameState.predictedDisplayPeriod, mRefreshHz);
    }
    XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
    r = xrBeginFrame(mSession, &fbi);
    if (XR_FAILED(r) && r != XR_FRAME_DISCARDED) {
        vrLog("xrBeginFrame failed: %s", xrResultName(mBoot->mInstance, r).c_str());
        mState = VrState::Lost; mRunning = false;
        setSessionViewEnabled(false);
        return true;
    }
    mInFrame = true;
    mDrewThisFrame = false;

    if (!mFrameState.shouldRender) {
        // A FRAME IS STILL OWED, with no layers (the spec's contract, and what
        // Monado's first frames ask for) — and THE DESKTOP MUST KEEP DRAWING.
        //
        // THE FIX (F4): this used to `return false`, which made
        // OgreEngine::renderOneFrame return before the monitor's frame, the
        // texture wait and EVERY view's render — so a doffed headset, an open
        // dashboard or a paused WiVRn froze the editor's viewport, its
        // thumbnails and every frame-counting host, for as long as the session
        // was up. The XR frame is closed here (it is owed and it is paid), the
        // session's own View is switched off so nothing renders two eyes for a
        // picture nobody will see, and the frame goes ahead for everybody else.
        endFrame();
        setSessionViewEnabled(false);
        return true;
    }

    XrViewState vs{ XR_TYPE_VIEW_STATE };
    XrViewLocateInfo vli{ XR_TYPE_VIEW_LOCATE_INFO };
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = mFrameState.predictedDisplayTime;
    vli.space = mSpace;
    uint32_t got = 0;
    mViews[0] = { XR_TYPE_VIEW }; mViews[1] = { XR_TYPE_VIEW };
    r = xrLocateViews(mSession, &vli, &vs, 2, &got, mViews);
    const bool posesValid = XR_SUCCEEDED(r) && got == 2u &&
                            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (!posesValid) {
        // No tracking this frame (the headset is off the head, the runtime is
        // still coming up). Draw no EYE rather than draw a lie — and, as above,
        // never stop the desktop's frame over it.
        endFrame();
        setSessionViewEnabled(false);
        return true;
    }
    setSessionViewEnabled(true);

    // ---- the pose, in three parts -----------------------------------------
    // THE HEAD is the midpoint of the two eyes, oriented like the left eye (the
    // two orientations are the same on every runtime measured). The RENDERING
    // camera is the head; the per-eye offsets ride VrData, which is what makes
    // one scene pass draw two eyes.
    const Ogre::Vector3 eyePos[2] = { toOgreVec(mViews[0].pose.position),
                                      toOgreVec(mViews[1].pose.position) };
    const Ogre::Quaternion eyeRot[2] = { toOgreQuat(mViews[0].pose.orientation),
                                         toOgreQuat(mViews[1].pose.orientation) };
    mIpd = (eyePos[1] - eyePos[0]).length();
    const Ogre::Vector3 headPos = (eyePos[0] + eyePos[1]) * 0.5f;
    const Ogre::Quaternion headRot = eyeRot[0];

    // WORLD SCALE is applied to the OFFSET from the space's origin, never to
    // the orientation: at scale 2 a step of one metre moves you two metres
    // through the world and the horizon does not tilt.
    const Ogre::Vector3 worldHead = headPos * mConfig.worldScale;

    // THE CLIP PLANES ARE THE VIEW'S, NOT A PAIR OF CONSTANTS (F10). A host
    // that moves the session View's camera desc moves the headset's frustum
    // with it; the session only supplies the VR-appropriate defaults at
    // create() (a 5 cm near plane — hands come closer than the desktop's 10 cm).
    Ogre::Camera *cam = mView ? mView->camera() : nullptr;
    const float zNear = cam ? float(cam->getNearClipDistance()) : kVrDefaultNear;
    const float zFar  = cam ? float(cam->getFarClipDistance()) : kVrDefaultFar;

    Ogre::Matrix4 eyeToHead[2], proj[2], projRS[2];
    Ogre::Matrix4 head(headRot);
    head.setTrans(headPos);
    const Ogre::Matrix4 headInv = head.inverseAffine();
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    for (int eye = 0; eye < 2; ++eye) {
        Ogre::Matrix4 eyeWorld(eyeRot[eye]);
        eyeWorld.setTrans(eyePos[eye]);
        eyeToHead[eye] = headInv * eyeWorld;
        // THE SCALE, in the one place it can be right: the eye separation is a
        // distance in the ROOM, so at world scale s the eyes are s times
        // further apart in the world.
        eyeToHead[eye].setTrans(eyeToHead[eye].getTrans() * mConfig.worldScale);
        proj[eye] = projectionFromFov(mViews[eye].fov, zNear, zFar);
        // ...AND THE RENDER SYSTEM'S CONVENTION, WHICH VrData DOES NOT APPLY
        // (F1, the critical one). `VrData::set` STORES the matrix raw
        // (OgreCamera.h:51-56) and HlmsPbs multiplies it raw into the pass
        // buffer with only the texture-flip on Y (OgreHlmsPbs.cpp:2240-2252) —
        // nothing on that path converts a [-1,1] GL-depth projection into this
        // render system's REVERSE-Z [1,0] range (OgreRenderSystem.cpp:112 sets
        // mReverseDepth). Only `Camera::setCustomProjectionMatrix` converts,
        // which is the path phase 1a proved and the path this pump does NOT
        // use for the eyes. The pin's own VR sample converts before the set
        // (Tutorial_OpenVR/OpenVRCompositorListener.cpp:136-151), and so do we.
        // Left raw, every depth test in the headset runs inverted: the picture
        // is not subtly wrong, it is sorted backwards.
        if (rs) rs->_convertProjectionMatrix(proj[eye], projRS[eye]);
        else    projRS[eye] = proj[eye];
    }
    mVrData.set(eyeToHead, projRS);
    // The UNCONVERTED pair is kept for anything that needs a plain projection
    // (the suite's mono control renders through setCustomProjectionMatrix,
    // which converts for itself, and would double-convert one of these).
    mEyeProjection[0] = proj[0];
    mEyeProjection[1] = proj[1];
    mEyeProjectionRS[0] = projRS[0];
    mEyeProjectionRS[1] = projRS[1];
    mAsymmetricFov =
        std::fabs(mViews[0].fov.angleLeft - mViews[1].fov.angleLeft) > 1e-6f ||
        std::fabs(mViews[0].fov.angleRight - mViews[1].fov.angleRight) > 1e-6f ||
        std::fabs(mViews[0].fov.angleUp - mViews[1].fov.angleUp) > 1e-6f ||
        std::fabs(mViews[0].fov.angleDown - mViews[1].fov.angleDown) > 1e-6f;

    if (cam) {
        cam->setPosition(worldHead);
        cam->setOrientation(headRot);
        cam->setVrData(&mVrData);
        // THE RENDERING CAMERA CARRIES THE LEFT EYE'S PROJECTION, and it is not
        // cosmetic (F2). Everything drawn by the Hlms takes its matrices from
        // VrData and never looks at the camera's own projection — but the three
        // SCREEN QUADS are low-level materials whose shaders read Ogre's
        // AUTO-PARAMS, and an auto-param is the RENDERING camera's. Left at the
        // View's CameraDesc angle, the sky in the headset would be drawn
        // through a 45-degree frustum while the eyes render at eighty-odd.
        //
        // With the left eye's projection here, ONE stereo shader is correct
        // everywhere: the first instance (the left eye) takes the auto-params
        // it is already given, the second takes the right eye's pair written
        // below, and the SAME material drawn by any OTHER pass in the process —
        // the desktop mirror view, a probe capture, a thumbnail — takes that
        // pass's own camera's auto-params and is therefore right too.
        cam->setCustomProjectionMatrix(true, mEyeProjection[0]);
    }
    // THE EYES' WORLD POSES, kept for the mono control (vrEyeScreenshot): the
    // same composition the shader performs, `headToEye^-1` applied to the head,
    // so a control render cannot drift from what the eye actually drew.
    for (int eye = 0; eye < 2; ++eye) {
        // DIRECTLY, not through a matrix decomposition: with the rig's origin
        // at identity (phase 2) the eye's world pose IS the pose the runtime
        // reported, scaled — origin(head) * (head^-1 * eye) reduces to the eye
        // — and a QDU decomposition of the product is the same answer with
        // seven digits instead of all of them. That difference is invisible
        // everywhere except at a high-contrast edge, where it moves one pixel,
        // which is exactly where a bit-exact assertion looks.
        mEyeWorldPos[eye] = eyePos[eye] * mConfig.worldScale;
        mEyeWorldRot[eye] = eyeRot[eye];
    }
    mHavePose = true;
    // THE SECOND EYE'S FOUR CORNER RAYS (F2), in world space, from its own fov
    // and its own orientation — the same quantity SceneManager writes into the
    // sky quad's normals for a mono camera (OgreSceneManager.cpp:1487-1499),
    // computed here for an eye no camera exists for. Order: bottom-left,
    // bottom-right, top-left, top-right in the quad's own NDC, which is what
    // the shader's bilinear pick expects; the suite pins that order by
    // rendering the FIRST eye through the same path and comparing it with a
    // mono render (JahVrScreenQuad_vs.glsl's note).
    for (int eye = 0; eye < 2; ++eye) {
        const XrFovf &f = mViews[eye].fov;
        const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight);
        const float u = std::tan(f.angleUp), d = std::tan(f.angleDown);
        // THE QUAD'S v = 1 EDGE IS THE TOP OF THE PICTURE, and that is
        // CALIBRATED, not assumed: the same convention question has two
        // plausible answers on a Vulkan backend (the API's clip space is
        // Y-down, the quad's own vertex data is not), and the suite settles it
        // — with the up-tangent at v = 1 the second eye reads mean 0.66/255
        // against a mono render of that eye, with the down-tangent 1.86 and
        // three times as many pixels past the tolerance.
        const float xs[4] = { l, r, l, r };
        const float ys[4] = { d, d, u, u };
        for (int c = 0; c < 4; ++c)
            mEyeCornerRay[eye][c] = mEyeWorldRot[eye] * Ogre::Vector3(xs[c], ys[c], -1.0f);
    }
    if (mCullCamera) {
        // THE CULL FRUSTUM MUST CONTAIN BOTH EYES, AND A UNION OF ANGLES AT THE
        // HEAD DOES NOT (F6). Two frusta that share an apex are contained by
        // the widest angles; two frusta whose apexes are an IPD apart are not —
        // each eye sees a sliver past the other's edge, and an object in that
        // sliver is culled out of the frame it belongs in (and missing from the
        // Forward+ light grid at the wider eye's edge, which is the same defect
        // one shading term later).
        //
        // The pin's own recipe closes it (Tutorial_OpenVR's
        // OpenVRCompositorListener.cpp:161-171): take the union of the TANGENT
        // extents, then PULL THE APEX BACK along the head's -Z by
        // (ipd/2) / |tan(leftmost)| — the distance at which the widened frustum
        // from the single apex swallows both eyes' — and give the near and far
        // planes that same offset so nothing near or far is lost to the move.
        //
        // TANGENT extents, not a custom projection matrix: Ogre then builds the
        // projection itself (so the render system's reverse-Z conversion
        // happens where it always happens) and may re-derive the near plane for
        // the PSSM and Forward+ passes, which is exactly what this camera is for.
        const float tanL = std::min(std::tan(mViews[0].fov.angleLeft),
                                    std::tan(mViews[1].fov.angleLeft));
        const float tanR = std::max(std::tan(mViews[0].fov.angleRight),
                                    std::tan(mViews[1].fov.angleRight));
        const float tanU = std::max(std::tan(mViews[0].fov.angleUp),
                                    std::tan(mViews[1].fov.angleUp));
        const float tanD = std::min(std::tan(mViews[0].fov.angleDown),
                                    std::tan(mViews[1].fov.angleDown));
        mCullCamera->setCustomProjectionMatrix(false);
        mCullCamera->setFrustumExtents(tanL, tanR, tanU, tanD,
                                       Ogre::FrustrumExtentsType::FET_TAN_HALF_ANGLES);
        const float halfIpd = 0.5f * mIpd * mConfig.worldScale;
        // THE NARROWER SIDE DECIDES, which is where this parts company with the
        // tutorial (V2F-5). The pin's sample divides by |tan(left)| alone
        // (OpenVRCompositorListener.cpp:167) — fine for a symmetric pair, wrong
        // for a headset whose eyes are asymmetric: if the RIGHT extent is the
        // narrower one, an apex pulled back by the LEFT one does not swallow
        // the right eye's outer sliver and that sliver is culled out of the
        // frame it belongs in. The offset a cone needs is set by its tightest
        // side, so the smaller tangent is the divisor.
        const float narrow = std::min(std::fabs(tanL), std::fabs(tanR));
        const float offset = narrow > 1e-6f ? halfIpd / narrow : 0.0f;
        mCullCamera->setNearClipDistance(std::max(zNear + offset, 0.001f));
        mCullCamera->setFarClipDistance(zFar + offset);
        // +Z in camera space is BEHIND the eye (Ogre cameras look down -Z).
        mCullCamera->setPosition(worldHead + headRot * Ogre::Vector3(0.0f, 0.0f, offset));
        mCullCamera->setOrientation(headRot);
    }
    // THE SCREEN QUADS, with the poses this frame located (F2).
    syncStereoQuads();
    ++mRendered;
    return true;
}

/// HAS THE DEVICE GONE? (F3.) The frame that just ran may have thrown
/// `VK_ERROR_DEVICE_LOST` out of its commit and been swallowed by the engine's
/// JAH_CATCH — from here that is indistinguishable from a frame that worked,
/// except that the render system knows. Asking costs a pointer chase.
bool VrSession::deviceLost() const {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return false;
    auto *vkRs = dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem());
    if (!vkRs) return false;
    Ogre::VulkanDevice *dev = vkRs->getVulkanDevice();
    return dev && dev->isDeviceLost();
}

void VrSession::endFrame() {
    if (!mInFrame) return;
    // A LOST DEVICE ENDS THE SESSION — IT DOES NOT KEEP SUBMITTING (F3).
    //
    // `copyEyes` marks the frame drawn when it has RECORDED the two copies; the
    // commit that executes them happens later and is where a device loss
    // actually surfaces, through an exception the engine's frame catches and
    // turns into `lastError`. Left alone, this function would then hand the
    // runtime a projection layer over swapchain images nothing ever wrote,
    // every frame, for ever — at a zero-interval pace, because the pump no
    // longer blocks on a dead device. So: no layer, no frames counted, and the
    // state goes to Lost, which is what makes the engine end the session and
    // the host put its pacing back (VrState::Lost is terminal at this pin —
    // an external device has no recovery path, VR_SPEC §2.1 row 10).
    const bool lost = deviceLost();
    if (lost) {
        mDrewThisFrame = false;
        if (mState != VrState::Lost)
            vrLog("the Vulkan device was lost inside a session - ending it");
    }
    for (int eye = 0; eye < 2; ++eye) {
        if (!mHasAcquired[eye]) continue;
        XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(mSwapchain[eye], &ri);
        mHasAcquired[eye] = false;
    }
    XrCompositionLayerProjection layer{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    const XrCompositionLayerBaseHeader *layers[1] = {
        reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer)
    };
    if (mDrewThisFrame) {
        layer.space = mSpace;
        layer.viewCount = 2;
        layer.views = mProjViews;
    }
    XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
    fei.displayTime = mFrameState.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = mDrewThisFrame ? 1u : 0u;
    fei.layers = mDrewThisFrame ? layers : nullptr;
    const XrResult r = xrEndFrame(mSession, &fei);
    mInFrame = false;
    if (lost) { mState = VrState::Lost; mRunning = false; return; }
    if (XR_FAILED(r)) {
        vrLog("xrEndFrame failed: %s", xrResultName(mBoot->mInstance, r).c_str());
        return;
    }
    ++mFrames;
}

/// The session's own View, switched off for a frame the runtime does not want
/// (F4). `View::setEnabled` is a workspace flag, not a rebuild: flipping it per
/// frame costs nothing and is what the Player already does with its own view.
void VrSession::setSessionViewEnabled(bool on) {
    if (!mView || mViewEnabled == on) return;
    mViewEnabled = on;
    mView->setEnabled(on);
}

// ---------------------------------------------------------------------------
// THE COPY (VR_SPEC §2.4 option A), recorded INSIDE the frame.
//
// This is the one routine phase 1a's §4 is about. After `renderOneFrame` ends,
// `commitAndNextCommandBuffer` has submitted the frame AND reset the
// BarrierSolver's per-frame tracking, so a CopySrc transition asked for at that
// point is emitted with `oldAccess == Undefined` — no source access, no
// COLOR_ATTACHMENT_OUTPUT source stage — and the layout transition (itself a
// write) races the render pass that just wrote the image. The synchronization
// validation layer reported exactly that, ten times out of ten.
//
// Recorded HERE, from the workspace's own pos-update, the solver still knows
// the target is in RenderTarget and written: the barrier it emits carries the
// right source scope, no `assumeTransition` repair is needed, and the copy
// rides the frame's own submit instead of paying a second one.
void VrSession::copyEyes() {
    OgreView *view = mView;
    if (!view) return;
    Ogre::TextureGpu *rtt = view->targetTexture();
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!rtt || !root) return;
    auto *vkRs = dynamic_cast<Ogre::VulkanRenderSystem *>(root->getRenderSystem());
    if (!vkRs) return;
    Ogre::VulkanDevice *dev = vkRs->getVulkanDevice();
    if (!dev) return;

    Ogre::BarrierSolver &solver = vkRs->getBarrierSolver();
    Ogre::ResourceTransitionArray trans;
    solver.resolveTransition(trans, rtt, Ogre::ResourceLayout::CopySrc,
                             Ogre::ResourceAccess::Read, 0);
    vkRs->executeResourceTransition(trans);
    dev->mGraphicsQueue.endAllEncoders();
    // getCurrentCmdBuffer never returns null: on a lost device its own
    // checkVkResult throws (the accessor ogre-patch 0040 made linkable).
    VkCommandBuffer cmd = dev->mGraphicsQueue.getCurrentCmdBuffer();
    const VkImage src = static_cast<Ogre::VulkanTextureGpu *>(rtt)->getFinalTextureName();

    const unsigned scW = mBoot->mViewCfg[0].recommendedImageRectWidth;
    const unsigned scH = mBoot->mViewCfg[0].recommendedImageRectHeight;
    const bool sameSize = (scW == mEyeWidth && scH == mEyeHeight);

    for (int eye = 0; eye < 2; ++eye) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(xrAcquireSwapchainImage(mSwapchain[eye], &ai, &idx))) return;
        XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        wi.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(mSwapchain[eye], &wi))) return;
        mAcquired[eye] = idx;
        mHasAcquired[eye] = true;
        const VkImage dst = mImages[eye][idx].image;

        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dst;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        // The runtime hands the image over in COLOR_ATTACHMENT_OPTIMAL and
        // wants it back that way; its contents are ours to overwrite, so
        // UNDEFINED as the old layout is legal and cheaper than preserving them.
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        if (sameSize) {
            VkImageCopy region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffset = { int32_t(eye * mEyeWidth), 0, 0 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.extent = { mEyeWidth, mEyeHeight, 1 };
            vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        } else {
            // ONLY WHEN THE MEASUREMENT OVERRIDE IS IN USE (VrConfig::
            // overrideEyeWidth): a copy cannot scale, so the eye is blitted.
            // The product path never takes this branch.
            VkImageBlit region{};
            region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.srcOffsets[0] = { int32_t(eye * mEyeWidth), 0, 0 };
            region.srcOffsets[1] = { int32_t((eye + 1) * mEyeWidth), int32_t(mEyeHeight), 1 };
            region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            region.dstOffsets[0] = { 0, 0, 0 };
            region.dstOffsets[1] = { int32_t(scW), int32_t(scH), 1 };
            vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                           VK_FILTER_LINEAR);
        }

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);

        mProjViews[eye] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
        mProjViews[eye].pose = mViews[eye].pose;
        mProjViews[eye].fov = mViews[eye].fov;
        mProjViews[eye].subImage.swapchain = mSwapchain[eye];
        mProjViews[eye].subImage.imageRect.offset = { 0, 0 };
        mProjViews[eye].subImage.imageRect.extent = { int32_t(scW), int32_t(scH) };
        mProjViews[eye].subImage.imageArrayIndex = 0;
    }

    // BACK TO RenderTarget, and not only for tidiness: a texture left in
    // CopySrc is refused by Ogre's own download path ("already in CopySrc or
    // CopyDst layout, externally set", VulkanQueue::prepareForDownload), which
    // is what a mirror, a screenshot or a pixel suite reads it with.
    trans.clear();
    solver.resolveTransition(trans, rtt, Ogre::ResourceLayout::RenderTarget,
                             Ogre::ResourceAccess::ReadWrite, 0);
    vkRs->executeResourceTransition(trans);

    mDrewThisFrame = true;
}

void VrSession::workspacePosUpdate(Ogre::CompositorWorkspace *workspace) {
    if (!mInFrame || !mView || workspace != mView->workspace()) return;
    copyEyes();
}

// ---------------------------------------------------------------------------
// THE MIRROR (VR_SPEC §4.3): a second workspace on the desktop View's target,
// appended after that View's own, painting one half of the eye target over the
// picture it just drew. See JahVrMirror.material for why it is a quad and not
// a blit.
void VrSession::teardownMirror() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return;
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    if (mMirrorWorkspace) { cm->removeWorkspace(mMirrorWorkspace); mMirrorWorkspace = nullptr; }
    if (!mMirrorWorkspaceDef.empty()) {
        chain::destroy(cm, mMirrorWorkspaceDef, mMirrorNodeDefs);
        mMirrorWorkspaceDef.clear();
    }
}

void VrSession::setMirrorView(OgreView *v) {
    if (mMirrorView != v) teardownMirror();
    mMirrorView = v;
    syncMirror();
}

/// THE MIRROR SHOWS THE LAST EYE PICTURE, AND WHILE THE SESSION VIEW IS OFF
/// THAT PICTURE IS STALE (V2F-8, by design). The mirror is a quad over the eye
/// TARGET, and that target is only rewritten by a frame the runtime asked for:
/// when it asks for none — the headset is off the head, the dashboard is up,
/// the runtime is paused — the session's own View is switched off for those
/// frames (F4) and the mirror keeps painting the last eye that was drawn. That
/// is the right answer for a mirror (a frozen last frame beats a black hole,
/// and the desktop's own picture is still being drawn underneath it), and it is
/// stated here so that "the mirror froze" is read as the runtime pausing rather
/// than as the loop stopping — `vr.state()` says which.
void VrSession::syncMirror() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root || !mView) return;
    // A camera IS required even though every pass in the mirror node is a quad:
    // CompositorWorkspace takes a default camera and dereferences it. A view
    // whose scene has not been set yet has none.
    const bool wanted = mMirrorView && mMirrorView->isEnabled() &&
                        mConfig.mirror != VrMirrorMode::None && mView->targetTexture() &&
                        mMirrorView->targetTexture() && mMirrorView->camera();
    if (!wanted) { teardownMirror(); return; }
    // A WORKSPACE REBUILD ON EITHER SIDE INVALIDATES THE ORDER (the inset's
    // rule, CAMERAS_SPEC §7.2): attachWorkspace always APPENDS, so a rebuilt
    // desktop workspace would be added after the mirror and paint over it.
    // There is no reorder API; the mirror is rebuilt instead, which happens
    // only when something else already rebuilt.
    const unsigned gen = mMirrorView->workspaceGeneration() + mView->workspaceGeneration();
    if (mMirrorWorkspace && gen == mMirrorGeneration) return;
    teardownMirror();
    if (!mMirrorView->workspace()) return;   // nothing to paint over yet

    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    mMirrorWorkspaceDef = "JahshakaVrMirror/" + mMirrorView->name();
    chain::buildVrMirror(cm, mMirrorWorkspaceDef, mMirrorNodeDefs);
    switch (mConfig.mirror) {
        case VrMirrorMode::Right: chain::setVrMirrorUv(0.5f, 1.0f, 0.5f, 0.0f); break;
        case VrMirrorMode::Both:  chain::setVrMirrorUv(1.0f, 1.0f, 0.0f, 0.0f); break;
        default:                  chain::setVrMirrorUv(0.5f, 1.0f, 0.0f, 0.0f); break;
    }
    Ogre::CompositorChannelVec targets;
    targets.push_back(mMirrorView->targetTexture());
    targets.push_back(mView->targetTexture());
    mMirrorWorkspace = cm->addWorkspace(mScene->sceneManager(), targets, mMirrorView->camera(),
                                        mMirrorWorkspaceDef, true);
    mMirrorGeneration = gen;
    if (!mMirrorWorkspace) vrLog("the mirror workspace could not be created");
}

// ---------------------------------------------------------------------------
VrStatus VrSession::status() const {
    VrStatus s;
    s.state = mState;
    s.active = true;
    s.frames = mFrames;
    s.rendered = mRendered;
    s.ipd = mIpd;
    s.eyeWidth = mEyeWidth;
    s.eyeHeight = mEyeHeight;
    s.mirror = mMirrorView ? mConfig.mirror : VrMirrorMode::None;
    s.worldScale = mConfig.worldScale;
    s.asymmetricFov = mAsymmetricFov;
    return s;
}

void VrSession::destroyXr() {
    if (mInFrame) endFrame();
    // A LOST SESSION NEVER REACHES THE DRAIN BELOW, and that is by
    // construction rather than by a test here (V2F-7): `endFrame` clears
    // `mRunning` the moment it sees a lost device, so a session that died that
    // way arrives with nothing running and is simply DESTROYED — which is
    // legal for a session in any state, and is the only thing that can work
    // when the runtime's compositor is waiting on a queue that will never
    // finish. The drain below is for the ordinary end: a live device, a
    // runtime that still answers, and a lifecycle to walk down.
    if (mRunning && mSession != XR_NULL_HANDLE) {
        // ASK, THEN DRAIN. xrRequestExitSession makes the runtime walk the
        // session down to STOPPING, which is where xrEndSession is legal; a
        // runtime that never answers must not hang the editor, so the drain is
        // bounded and the session is destroyed regardless (destroying a running
        // session is legal; leaving the process wedged is not).
        xrRequestExitSession(mSession);
        for (int i = 0; i < 100 && mRunning; ++i) {
            pollEvents();
            if (!mRunning) break;
            // THE RUNTIME ONLY WALKS THE SESSION DOWN WHILE IT IS BEING FED:
            // empty frames, no layers, until it says STOPPING (which is where
            // applyState calls xrEndSession).
            XrFrameState fs{ XR_TYPE_FRAME_STATE };
            XrFrameWaitInfo fwi{ XR_TYPE_FRAME_WAIT_INFO };
            if (XR_FAILED(xrWaitFrame(mSession, &fwi, &fs))) break;
            XrFrameBeginInfo fbi{ XR_TYPE_FRAME_BEGIN_INFO };
            if (XR_FAILED(xrBeginFrame(mSession, &fbi))) break;
            XrFrameEndInfo fei{ XR_TYPE_FRAME_END_INFO };
            fei.displayTime = fs.predictedDisplayTime;
            fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            if (XR_FAILED(xrEndFrame(mSession, &fei))) break;
        }
        if (mRunning) { xrEndSession(mSession); mRunning = false; }
    }
    for (int eye = 0; eye < 2; ++eye) {
        if (mSwapchain[eye] != XR_NULL_HANDLE) xrDestroySwapchain(mSwapchain[eye]);
        mSwapchain[eye] = XR_NULL_HANDLE;
        mImages[eye].clear();
    }
    if (mSpace != XR_NULL_HANDLE) { xrDestroySpace(mSpace); mSpace = XR_NULL_HANDLE; }
    if (mSession != XR_NULL_HANDLE) { xrDestroySession(mSession); mSession = XR_NULL_HANDLE; }
}

VrSession::~VrSession() {
    // ORDER (VR_SPEC §4.3's teardown): the XR objects first — never between
    // xrBeginFrame and xrEndFrame — then the mirror, then the View (which takes
    // its workspace and its RTT), then the cull camera, which goes LAST because
    // a pass holds a raw Camera* and destroying it first segfaults on the next
    // frame (the PiP lane's T6).
    destroyXr();
    teardownMirror();
    dropStereoQuads();
    if (mView) {
        mView->removeWorkspaceListener(this);
        if (mView->camera()) mView->camera()->setVrData(nullptr);
        mEngine->destroyView(mView);
        mView = nullptr;
    }
    if (mCullCamera && mScene && mScene->sceneManager()) {
        mScene->sceneManager()->destroyCamera(mCullCamera);
        mCullCamera = nullptr;
    }
}

// ---------------------------------------------------------------------------
// THE THREE SCREEN QUADS (F2): the sky, the atmosphere and the sun disc.
//
// WHAT THEY HAVE IN COMMON, and why one routine answers for all three: each is
// a full-screen `Rectangle2D` drawn with a LOW-LEVEL material whose vertex
// program turns the quad's corners into a camera ray. Instanced stereo doubles
// their draw like every other (OgreRenderQueue.cpp:697-699), but their vertex
// programs were written for ONE viewport — so both copies land in the first
// eye and THE RIGHT EYE HAS NO SKY. (Measured on this lane's fixture before the
// fix: the two halves of a worldScale-0 frame, which must be identical, differ
// by 30,306 bytes with a worst of 255/255.)
//
// THE FIX IS A MATERIAL, AND IT HAS TO BE. The pin's per-pass MATERIAL SCHEME
// (`CompositorPassSceneDef::mMaterialScheme`) looks like the answer and is not:
// for a low-level material the technique is resolved into the renderable's
// cached Hlms hash when its MATERIAL is set (`HlmsLowLevel::calculateHashFor`
// via Renderable::setMaterial), not per pass, so a scheme switched on by the VR
// pass changes nothing at all. Measured, not reasoned: the technique existed,
// the shader compiled, and every eye still drew the default one.
//
// So the session gives each quad a CLONE of its material whose vertex program
// is the stereo one, and puts the original back when it ends. `setMaterial` is
// what re-resolves the hash, which is exactly the mechanism that was missing.
//
// AND THE CLONE IS CORRECT IN EVERY OTHER PASS TOO, which is what makes the
// swap safe: the stereo shader takes the FIRST eye's ray from Ogre's ordinary
// auto-params — the rendering camera's own inverse view-projection, which for
// the VR view is the left eye's (see the camera's setCustomProjectionMatrix
// above) and for the desktop mirror view, a probe capture or a thumbnail is
// that camera's. Only the SECOND instance reads the pair written here, and only
// a stereo pass ever draws a second instance.
void VrSession::syncStereoQuads() {
    if (!mScene || !mScene->sceneManager()) return;
    Ogre::SceneManager *sm = mScene->sceneManager();
    Ogre::MaterialManager *mm = Ogre::MaterialManager::getSingletonPtr();
    if (!mm) return;

    // MARK AND SWEEP, and the sweep is the point (V2F-2). The entries below
    // hold a RAW `Rectangle2D *`, and those quads are DESTROYED under us: a
    // sky pushed to None destroys `SceneManager::mSky`
    // (OgreSceneManager.cpp:1157-1161) and our own sun disc goes with its
    // scene. Nothing signals it. So the list is rebuilt against the live
    // objects every frame — an entry nobody saw this frame names a quad that no
    // longer exists, and the one thing that must NOT happen to it is a
    // `setMaterial` on freed memory.
    for (StereoQuad &q : mStereoQuads) q.seen = false;

    // The quads THIS scene actually draws, found by WHAT THEY ARE rather than
    // by name: a screen quad is a Rectangle2D, and the ones that need an eye
    // are the ones whose vertex program derives a camera ray.
    Ogre::SceneManager::MovableObjectIterator it =
        sm->getMovableObjectIterator(Ogre::Rectangle2DFactory::FACTORY_TYPE_NAME);
    while (it.hasMoreElements()) {
        Ogre::MovableObject *obj = it.getNext();
        if (!obj) continue;
        Ogre::Rectangle2D *quad = static_cast<Ogre::Rectangle2D *>(obj);
        Ogre::MaterialPtr mat = quad->getMaterial();
        if (!mat) continue;

        StereoQuad *known = nullptr;
        for (StereoQuad &q : mStereoQuads)
            if (q.quad == quad) { known = &q; break; }
        if (known) known->seen = true;

        // ---- IS THIS QUAD ALREADY OURS? ----------------------------------
        // THE OWNERS RE-APPLY THEIR MATERIAL CONSTANTLY (V2F-1):
        // `SceneManager::setSky` calls `mSky->setMaterial(mSkyMaterial)` on
        // EVERY call (OgreSceneManager.cpp:1153), and this engine re-calls
        // setSky for any equirect/cubemap/tint change (OgreSky.cpp:249, :305).
        // So a quad that was ours a frame ago can be holding its base again,
        // and the answer to that is to PUT OUR CLONE BACK — not to clone a
        // second time under a name the material manager already has, which
        // throws ERR_DUPLICATE_ITEM inside beginFrame and takes the whole
        // frame down with it (no eye, no desktop, every frame until the
        // session ends).
        if (known && known->vrMaterial) {
            if (mat == known->vrMaterial) {
                // nothing moved
            } else if (mat == known->baseMaterial) {
                quad->setMaterial(known->vrMaterial);   // re-resolves the hash
            } else {
                // The base material really changed (a sky method swap, the
                // atmosphere replacing the cubemap sky). The old clone is dead
                // and its NAME has to go with it, or the rebuild below cannot
                // have it.
                dropClone(*known);
                known->seen = true;                     // the entry is reused
                known->quad = quad;
                known->baseMaterial.reset();
            }
        }

        if (!known || !known->vrMaterial) {
            Ogre::Technique *base = mat->getTechnique(0u);
            Ogre::Pass *basePass = base && base->getNumPasses() ? base->getPass(0u) : nullptr;
            if (!basePass || !basePass->hasVertexProgram()) continue;
            const Ogre::String &vs = basePass->getVertexProgramName();
            // THE TWO PROGRAMS THAT READ A CAMERA RAY. Upstream's is shared by
            // the sky and the atmosphere; the second is our sun disc's.
            // Anything else drawn as a Rectangle2D is left alone, which is the
            // right answer for a quad with no ray.
            if (vs != "Ogre/Compositor/QuadCameraDirNoUV_vs" && vs != "Jahshaka/SunDisc_vs")
                continue;

            const Ogre::String cloneName = mat->getName() + "/JahVrStereo";
            // A LEFTOVER FROM A PREVIOUS SESSION, OR FROM A MODE CHANGE THAT
            // DID NOT COME BACK THROUGH US: the name is the material manager's,
            // not ours, and `clone` on a registered name THROWS.
            if (Ogre::MaterialPtr stale = mm->getByName(cloneName)) {
                stale.reset();
                mm->remove(cloneName);
            }
            Ogre::MaterialPtr clone;
            try {
                clone = mat->clone(cloneName);
            } catch (const Ogre::Exception &e) {
                vrLog("the screen quad's material could not be cloned: %s",
                      e.getDescription().c_str());
                continue;
            }
            if (!clone) continue;
            Ogre::Technique *ct = clone->getTechnique(0u);
            if (!ct || ct->getNumPasses() == 0u) continue;
            ct->getPass(0u)->setVertexProgram("Jahshaka/VrScreenQuad_vs");
            // THE CLONE SHARES THE BASE'S FRAGMENT PARAMETERS — the same
            // object, not a copy (V2F-3's real cause). The owners of these
            // materials write into their pass PER CAMERA, inside the frame:
            // `AtmosphereNpr::_update` pushes its whole preset — including a
            // camera-dependent displacement — once for every camera that
            // renders (OgreSceneManager.cpp:1484). A copy taken once a frame is
            // therefore whatever the LAST camera of the previous frame left,
            // which in a session is the desktop mirror's, and the headset's sky
            // would be graded for a camera the wearer is not looking through.
            // Measured as a 1.2/255 mean over the sky rows of whichever picture
            // was rendered second. Sharing the object removes the question:
            // both passes use the same fragment program, so the layout is the
            // same, and whatever the owner writes is what the clone draws with.
            if (basePass->hasFragmentProgram() && ct->getPass(0u)->hasFragmentProgram())
                ct->getPass(0u)->setFragmentProgramParameters(
                    basePass->getFragmentProgramParameters());
            // COMPILE, THEN LOAD, AND compile() IS THE ONE THAT MATTERS: a
            // material's per-scheme technique lists are built by compile(), and
            // load() on an already-loaded resource returns without doing it.
            clone->compile(false);
            clone->load();
            quad->setMaterial(clone);       // ...which re-resolves the hash
            if (!known) {
                mStereoQuads.push_back(StereoQuad());
                known = &mStereoQuads.back();
            }
            known->quad = quad;
            known->baseMaterial = mat;
            known->vrMaterial = clone;
            known->seen = true;
            vrLog("stereo screen quad: '%s' (was %s)", mat->getName().c_str(), vs.c_str());
        }

        // ---- the TEXTURES the owner binds, mirrored -----------------------
        // The fragment PARAMETERS are shared with the base (see the clone
        // above), so nothing has to be copied for them. Texture bindings are
        // not parameters: `SceneManager::setSky` binds the sky's cubemap or
        // equirect map into its own pass's texture unit, and that has to be
        // followed.
        Ogre::Technique *baseT = known->baseMaterial ? known->baseMaterial->getTechnique(0u) : nullptr;
        Ogre::Technique *vrT = known->vrMaterial ? known->vrMaterial->getTechnique(0u) : nullptr;
        Ogre::Pass *basePass = baseT && baseT->getNumPasses() ? baseT->getPass(0u) : nullptr;
        Ogre::Pass *vrPass = vrT && vrT->getNumPasses() ? vrT->getPass(0u) : nullptr;
        if (!basePass || !vrPass) continue;
        const unsigned short units =
            std::min(basePass->getNumTextureUnitStates(), vrPass->getNumTextureUnitStates());
        for (unsigned short u = 0; u < units; ++u) {
            Ogre::TextureUnitState *src = basePass->getTextureUnitState(u);
            Ogre::TextureUnitState *dst = vrPass->getTextureUnitState(u);
            // `_getTexturePtr` is the only read side this class has (there is
            // no getTexture); the underscore is upstream's spelling for "the
            // resolved pointer", not a private.
            if (src && dst && src->_getTexturePtr() != dst->_getTexturePtr())
                dst->setTexture(src->_getTexturePtr());
        }

        // ---- and the eyes' corner rays ------------------------------------
        if (!vrPass->hasVertexProgram()) continue;
        Ogre::GpuProgramParametersSharedPtr vp = vrPass->getVertexProgramParameters();
        vp->setIgnoreMissingParams(true);
        for (int eye = 0; eye < 2; ++eye) {
            for (int corner = 0; corner < 4; ++corner) {
                const Ogre::Vector3 &d = mEyeCornerRay[eye][corner];
                vp->setNamedConstant("jahEyeCorner[" + std::to_string(eye * 4 + corner) + "]",
                                     Ogre::Vector4(d.x, d.y, d.z, 0.0f));
            }
        }
    }

    // ---- the sweep ---------------------------------------------------------
    for (size_t i = mStereoQuads.size(); i-- > 0;) {
        if (mStereoQuads[i].seen) continue;
        // The quad is GONE. Its clone is ours to unregister; the pointer is not
        // ours to touch.
        dropClone(mStereoQuads[i]);
        mStereoQuads.erase(mStereoQuads.begin() + long(i));
    }
}

/// Unregisters one quad's clone. NOT a restore — the caller decides whether
/// there is still a quad to give the base material back to.
void VrSession::dropClone(StereoQuad &q) {
    Ogre::MaterialManager *mm = Ogre::MaterialManager::getSingletonPtr();
    if (q.vrMaterial && mm) {
        const Ogre::String name = q.vrMaterial->getName();
        q.vrMaterial.reset();
        mm->remove(name);
    }
    q.vrMaterial.reset();
}

void VrSession::dropStereoQuads() {
    // ONLY QUADS THAT ARE STILL ALIVE GET THEIR MATERIAL BACK (V2F-2). The
    // entries hold raw pointers and a session can outlive the objects they
    // name — a sky switched off mid-session destroys its quad — so the live
    // set is re-derived here rather than trusted.
    std::vector<Ogre::Rectangle2D *> live;
    if (mScene && mScene->sceneManager()) {
        Ogre::SceneManager::MovableObjectIterator it =
            mScene->sceneManager()->getMovableObjectIterator(
                Ogre::Rectangle2DFactory::FACTORY_TYPE_NAME);
        while (it.hasMoreElements()) {
            if (Ogre::MovableObject *obj = it.getNext())
                live.push_back(static_cast<Ogre::Rectangle2D *>(obj));
        }
    }
    for (StereoQuad &q : mStereoQuads) {
        const bool alive = std::find(live.begin(), live.end(), q.quad) != live.end();
        if (alive && q.baseMaterial && q.quad->getMaterial() == q.vrMaterial)
            q.quad->setMaterial(q.baseMaterial);
        dropClone(q);
    }
    mStereoQuads.clear();
}

// ---------------------------------------------------------------------------
// ONE EYE, RENDERED MONO (Engine::vrEyeScreenshot).
//
// WHAT IT IS FOR, and it is two things at once. It is the picture a user wants
// when they ask "what did I see in there" — a screenshot of an eye, at the eye's
// own size, through the eye's own pose and projection. And it is THE REVERSE-Z
// DETECTOR (VR_SPEC §6): the stereo path hands its projections to `VrData`,
// which stores them RAW, while this path hands the same matrix to
// `Camera::setCustomProjectionMatrix`, which runs it through the render
// system's own conversion. If the session ever stops converting for VrData —
// the defect this round fixed — the two pictures disagree about depth, and the
// disagreement is total, not subtle: the far surface wins every test.
//
// THE CONTROL IS A REAL VIEW rendered by REAL FRAMES. It is created with the
// session View's own chain shape and helper policy, seeded with the session
// View's measured exposure so the tonemapper starts where the session's is, and
// read once the picture STOPS MOVING (never after a fixed frame count — the
// rule cameras.exposure taught this tree).
bool VrSession::eyeScreenshot(unsigned eye, Image &out, std::string &error) {
    if (eye > 1u) { error = "vrEyeScreenshot: eye must be 0 (left) or 1 (right)"; return false; }
    if (!mHavePose || !mView) {
        error = "vrEyeScreenshot: the session has not located its eyes yet";
        return false;
    }
    View *v = mEngine->createOffscreenView("jahshaka-vr-eye", mEyeWidth, mEyeHeight,
                                           Colour{ 0.0f, 0.0f, 0.0f, 1.0f });
    if (!v) { error = "vrEyeScreenshot: " + mEngine->lastError(); return false; }
    OgreView *control = static_cast<OgreView *>(v);
    bool ok = false;
    JAH_TRY {
        // THE CONTROL'S EXPOSURE IS THE SESSION'S, AS A CONSTANT, and the
        // alternative was measured rather than argued (V2F-3). Both chains
        // carry the same filmic composite (POST_CHAIN_SPEC §14: one material,
        // two forms), but the AUTO form's exposure is a per-chain FEEDBACK
        // HISTORY: a control that keeps the auto form converges on its own and
        // read 8-9/255 away from the session's picture — when it settled at all
        // — while the fixed form, handed the number the session's chain
        // actually converged to, reads 0.000. That is the difference between a
        // control that can prove something and one that cannot.
        //
        // WHAT IT COSTS, stated because it is real: the control's ~90 frames
        // move the session's own auto-exposure a little (its picture drifts
        // ~1.2/255 over the sky rows across one call), so a SECOND control in
        // the same session is compared across that drift. The suite says so
        // rather than hiding it, and the order experiment behind
        // JAH_VR_RIGHT_FIRST is what proved the residual is the drift and not
        // the second eye's ray route.
        PostFxDesc fx = mView->postFx();
        const float exposure = mView->measuredExposureScale();
        if (fx.hdr && exposure > 0.0f) {
            fx.tonemapFixed = true;
            fx.exposureScale = exposure;
        }
        control->setPostFx(fx);
        control->setHelpersVisible(mView->helpersVisible());
        control->setShadows(mView->shadows());
        if (control->setScene(mScene)) {
            CameraDesc desc;
            desc.nearClip = float(mView->camera() ? mView->camera()->getNearClipDistance()
                                                  : kVrDefaultNear);
            desc.farClip = float(mView->camera() ? mView->camera()->getFarClipDistance()
                                                 : kVrDefaultFar);
            control->setCamera(desc);
            if (Ogre::Camera *c = control->camera()) {
                c->setPosition(mEyeWorldPos[eye]);
                c->setOrientation(mEyeWorldRot[eye]);
                // THE EYE'S EXACT PROJECTION, through the one call that
                // converts it for this render system.
                c->setCustomProjectionMatrix(true, mEyeProjection[eye]);
            }
            // READ UNTIL IT HOLDS STILL. The chain's history textures are new,
            // so the first frames of any chain are its own warm-up.
            Image prev;
            for (int i = 0; i < 90 && !ok; ++i) {
                mEngine->renderOneFrame();
                if (!control->readPixels(out)) break;
                if (i > 0 && !out.rgba.empty() && out.rgba == prev.rgba) ok = true;
                else prev = out;
            }
            if (!ok) error = "vrEyeScreenshot: the control picture never settled";
        } else {
            error = "vrEyeScreenshot: the control view refused the scene";
        }
    } JAH_CATCH(error, (mEngine->destroyView(v), false));
    mEngine->destroyView(v);
    return ok;
}

// ---------------------------------------------------------------------------
// The engine's entry points (declared in EnginePrivate.h).
namespace vr {

VrBoot *bootBegin(VrInfo &info, std::string &reason) {
    info = VrInfo();
    VrBoot *boot = new VrBoot();
    if (!boot->begin(info, reason)) {
        info.available = false;
        info.reason = reason;
        delete boot;
        return nullptr;
    }
    return boot;
}

void *bootExternalInstance(VrBoot *boot) {
    return boot ? static_cast<void *>(&boot->mExternalInstance) : nullptr;
}

bool bootDevice(VrBoot *boot, Ogre::Root *root, VrInfo &info, std::string &reason) {
    if (!boot) { reason = "no VR boot"; return false; }
    if (!boot->device(root, info, reason)) {
        info.available = false;
        info.reason = reason;
        return false;
    }
    info.available = true;
    info.reason.clear();
    return true;
}

void *bootExternalDevice(VrBoot *boot) {
    return boot ? static_cast<void *>(&boot->mExternalDevice) : nullptr;
}

void bootEnd(VrBoot *boot) { delete boot; }

VrSession *sessionBegin(VrBoot *boot, OgreEngine *engine, OgreScene *scene, const VrConfig &cfg,
                        std::string &reason) {
    if (!boot) { reason = "no OpenXR runtime"; return nullptr; }
    VrSession *s = new VrSession(boot, engine, scene, cfg);
    if (!s->create(reason)) { delete s; return nullptr; }
    return s;
}

void sessionEnd(VrSession *s) { delete s; }

}  // namespace vr

bool vrSessionBeginFrame(VrSession *s) { return s ? s->beginFrame() : true; }
void vrSessionEndFrame(VrSession *s) { if (s) s->endFrame(); }
VrState vrSessionState(const VrSession *s) { return s ? s->state() : VrState::Unavailable; }
VrStatus vrSessionStatus(const VrSession *s) { return s ? s->status() : VrStatus(); }
View *vrSessionView(const VrSession *s) { return s ? s->view() : nullptr; }
void vrSessionSetMirror(VrSession *s, OgreView *v) { if (s) s->setMirrorView(v); }
bool vrSessionEyeScreenshot(VrSession *s, unsigned eye, Image &out, std::string &error) {
    if (!s) { error = "vrEyeScreenshot: no session is running"; return false; }
    return s->eyeScreenshot(eye, out, error);
}

#endif  // JAH_VR

}}}   // namespace jahshaka::engine::detail
