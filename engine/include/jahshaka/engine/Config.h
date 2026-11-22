#pragma once
// THE ENGINE's CONFIG TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <functional>
#include <string>
#include <vector>
#include "jahshaka/engine/Ids.h"

namespace jahshaka { namespace engine {

/// `Engine::createScene(name, kSceneMainThreadOnly)` — "this scene gets NO
/// worker threads" (SPECS/THREADING_ADOPTION_SPEC.md P5).
///
/// It needs a spelling of its own because the natural one is taken: `0` has
/// always meant "I do not care, give me the backend's default", and every
/// caller that passes nothing passes 0. So the boundary needs two ways to say
/// "zero", and only one of them may reach the backend as zero.
///
/// WHY IT IS NOT JUST A SMALLER NUMBER. At 1 the backend still SPAWNS a worker
/// thread and still pays two barrier syncs per parallel pass, to do exactly the
/// serial work the calling thread could have done inline. At 0 it spawns
/// nothing and every pass runs inline with no barrier at all
/// (`mForceMainThread`, OgreSceneManager.cpp:171 and :4705-4717). For a scene
/// that is never drawn — the document's staging scene manager — or one that
/// draws 128x128 once and is emptied again, 1 is strictly worse than 0.
///
/// The value is deliberately not 0 and deliberately not a plausible thread
/// count, so a caller that computes a number can never land on it by accident.
constexpr unsigned kSceneMainThreadOnly = ~0u;

/// The host's sink for the engine's OWN log records (SESSION_LOG_SPEC F3-B).
/// `level` is 0 for ordinary messages and 1 for LML_CRITICAL. See
/// Engine::setLogSink for the three rules an implementation must obey — it is
/// called under Ogre's log mutex, on whatever thread logged, and must never
/// call back into the engine.
using EngineLogSink = std::function<void(int level, const std::string &message)>;

/// THE PROCESS'S VR POLICY (VR-START-1), set by the host: may this process talk
/// to an OpenXR runtime at all, and to which ones. `runtimes` empty = ANY
/// runtime (a developer's `--vr`, the suites' private Monado); otherwise the
/// ACTIVE runtime's manifest (resolved the way the loader resolves it, WITHOUT
/// opening the loader) must name one of them — "WiVRn", "SteamVR", "Monado",
/// matched case-insensitively against the manifest's name and library file —
/// or the start fails WrongRuntime with no OpenXR call at all. That is what
/// keeps a socket-activated system runtime from ever being spawned by a
/// desktop that has no headset.
struct VrPolicy {
    bool enabled = false;
    std::vector<std::string> runtimes;
};

/// Everything the engine needs to start. All paths are resolved by the HOST at
/// runtime (next to the executable, an env override, or a compile-time default).
/// Nothing in the engine is baked to a build-machine path.
struct EngineConfig {
    // The renderer is Vulkan, always (the Backend enum and its unreachable OpenGL
    // value are gone with the GL3Plus build, D6-FORK-TOOLING).
    /// HEADLESS: boot the NULL render system instead of Vulkan
    /// (SPECS/SCENEGRAPH_SPEC.md §3b, v2). Everything that is not pixels works
    /// exactly as it does on a real device — scenes, the document scene graph,
    /// nodes, meshes, materials, queries, transforms — and NOTHING renders:
    ///
    ///   * no display, no GPU and no driver are needed or opened (this is the
    ///     one mode that runs with DISPLAY unset);
    ///   * the render system creates its own 1x1 surfaceless window at boot,
    ///     which is what satisfies Ogre's window-before-SceneManager rule, so
    ///     the engine never makes a window of its own;
    ///   * createView() AND createOffscreenView() REFUSE (lastError() says so):
    ///     a NULL-render-system texture has no contents, and returning a View
    ///     whose readPixels answers uninitialised memory would be worse than a
    ///     clean "no";
    ///   * renderOneFrame() is legal and does nothing — with no View there is
    ///     nothing to draw into.
    ///
    /// ONE RENDER SYSTEM PER PROCESS: `Ogre::Root` is a singleton and its
    /// render system is chosen once, at boot. A process is EITHER headless OR
    /// rendering; there is no mixed mode and no way to upgrade one into the
    /// other. Hosts pick at startup (Studio: `--headless` / `--dump-api-docs`
    /// and the document-only suites).
    ///
    /// Also forces the persistent shader cache OFF: nothing compiles here, and
    /// a cache written under the NULL system must never be read back by a real
    /// one (the fingerprint does not name the render system).
    /// THE BACKGROUND SHADER COMPILE SERVICE's thread count (ASYNC-SHADERS-1;
    /// AsyncShaderStats, View::setAsyncShaders). The threads run at a lowered priority
    /// so a compile never competes with the frame's own workers. 0 = no service: every
    /// view compiles inside its frame. Ignored headless.
    unsigned    asyncShaderThreads = 2;
    bool        headless = false;
    /// Directory holding the render-system plugins (RenderSystem_Vulkan.so ...).
    std::string pluginDir;
    /// Directory that CONTAINS the `Hlms/` folder (Hlms/Common, Hlms/Pbs, Hlms/Unlit).
    /// These shader templates are required at runtime, not optional sample data.
    std::string hlmsMediaDir;
    /// Log file path; empty means the backend's default name in the working directory.
    std::string logFile = "jahshaka-ogre.log";
    /// Optional forwarding sink, installed with the log listener itself — i.e.
    /// BEFORE the plugins load and the render system initialises, which is
    /// where the boot-time criticals (a missing render-system plugin, a Vulkan
    /// validation error, an ABI complaint) actually happen. Setting it through
    /// Engine::setLogSink afterwards works too and misses exactly that window.
    EngineLogSink logSink;
    /// Initial MSAA sample count for ON-SCREEN views (1 = off; 2/4/8 typical).
    /// Offscreen views (thumbnails, previews, tests) always start at 1 so their
    /// pixel readbacks stay exact — raise per view with View::setSampleCount.
    /// The driver may clamp; View::sampleCount() reports what was achieved.
    unsigned sampleCount = 1;
    /// THE HOST'S CALLS ARE RECORDS (ENGINE-UPGRADE-U3 stage 4a, decision 39): a View setter, an engine verb or a
    /// scene command the host calls becomes a field of the record or a message of its stream, applied at the drain —
    /// the shape the render thread (stage 4b) needs. The app turns it on (bridge/enginehost.cpp); an engine fixture
    /// that drives the engine directly on its own thread leaves it off and its calls apply where they are made.
    bool hostRecords = false;
    /// THE RENDER THREAD (ENGINE-UPGRADE-U3 stage 4b): Root, the device, the windows and every frame on a thread of the
    /// engine's own; the caller's frames, requests and the view joins wait for it (lockstep). Implies hostRecords.
    bool renderThread = false;
    /// Build a dedicated, de-duplicated, position-only vertex buffer for each
    /// mesh's shadow-map pass instead of re-streaming the full vertex. Shadow
    /// passes then read ~4x less vertex bandwidth, at the cost of extra VRAM per
    /// mesh and a GPU->CPU readback while the mesh is being created (import
    /// latency, never frame time). Process-wide and consumed at mesh-build time,
    /// so it cannot honestly be a per-scene value — a mesh built while it was on
    /// keeps its optimized buffers. Change at runtime with
    /// Engine::setShadowMeshOptimization(); it affects meshes built afterwards.
    bool optimizeShadowMeshes = true;
    /// Host's display connection; required only for on-screen Views (see above).
    NativeDisplayHandle display = 0;
    /// HARDWARE RAY TRACING, at boot (PHOTON_SPEC §7 R1). True (the default)
    /// lets the tier come up wherever the device advertises it; false is the
    /// fallback picture a machine without ray tracing gets — the one path a Mac
    /// takes, and the one every ray-consuming suite must be able to run on this
    /// GPU so the fallback is proved on every push.
    ///
    /// FALSE REACHES THE DEVICE, not just this tier: the engine hands it to the
    /// fork's VulkanDevice::msRayQueryAllowed before the render system loads (fork
    /// 98a6f4d34's static, gating 0038's requests at vkCreateDevice), so the
    /// process comes up on exactly the instance, extension list and feature set
    /// it would have had if the tier did not exist. "The picture a machine without ray tracing renders" is therefore
    /// literal and not a manner of speaking — which is what makes the suites
    /// that assert the fallback worth anything.
    ///
    /// THE HOST OWNS THIS ANSWER, and since 2026-09-15 (ledger §425) it is the
    /// DIAGNOSTIC LATCH and nothing else: Studio fills it from
    /// `--no-ray-query` alone. What a PROJECT asks for is a document field
    /// pushed per scene (Scene::setRayTracing / RayTracingMode), because ray
    /// tracing is a property of the project met with a property of the machine
    /// — an application preference beside the document's row would be two
    /// layers that can disagree. Change this one at runtime with
    /// Engine::setRayTracing.
    ///
    /// It is NOT a quality dial: with rays off the tier builds nothing at all,
    /// costs nothing at all, and `giStatus().rayQuery.enabled` reads false.
    bool rayTracing = true;
    /// OPENXR (VR-START-1): the process's VR policy (see VrPolicy). Disabled by
    /// default; a host enables it per process. It changes NOTHING about the
    /// boot — every engine runs on its own device — only whether, and to which
    /// runtime, a session may connect. Engine::setVrPolicy changes it later.
    VrPolicy vr;
    /// Vertical sync for ON-SCREEN views, as they are created (fps audit F1).
    /// True is what every window did unconditionally before this field existed.
    /// False asks for an immediate, tearing present mode — the "unlimited"
    /// pacing mode. Change it later with Engine::setVsync(), which documents
    /// what it costs and what it means; this only decides where windows start.
    bool vsync = true;

    // ---- Persistent shader cache (SHADER_CACHE_SPEC.md) ----
    /// Directory the backend may persist compiled-shader artifacts in. EMPTY =
    /// the cache is off: nothing is read, nothing is written, and every launch
    /// recompiles from scratch (the behaviour before the cache existed). The
    /// directory is DERIVED DATA — deleting it costs one slow launch and never
    /// anything a user could miss.
    std::string shaderCacheDir;
    /// The directory's SIZE CAP: a save that leaves it larger WIPES it (the next launch re-warms; there is
    /// no eviction — see OgreShaderCache.cpp's save). 256 MiB is the app's, measured against one user's
    /// projects. The engine test fixtures share ONE union of every family's shaders (ENGINE-UPGRADE-1:
    /// 220 MB / 1,686 shaders on one tree's night), so their helper raises it (enginetesthelpers.h): a
    /// cap meant for the app must never wipe the fixtures' union.
    unsigned long long shaderCacheMaxBytes = 256ull * 1024ull * 1024ull;
    /// THE SHIPPED MICROCODE SEED (COLD-BOOT-1 S1): a directory a BUILD carries — `cache-manifest.txt` naming one
    /// `microcode.cache` in the cache's own format — read as a READ-ONLY FALLBACK under shaderCacheDir. When the
    /// cache directory holds no microcode layer valid at this build's microcode key, the seed's is loaded instead,
    /// before any shader compiles, so a first launch serves its SPIR-V from it and glslang runs only for a miss.
    /// The engine never writes, locks or wipes it; a seed made at another microcode key (another fork commit,
    /// glslang, build type or render system) is not read at all — a miss, never a wrong blob. The layer it seeds is the
    /// one whose key ignores the GPU and the driver, so a seed made on one machine is valid on any other (upstream:
    /// OgreGpuProgramManager.h, "Vulkan: Yes"). EMPTY = no seed; ignored when shaderCacheDir is empty.
    std::string shaderCacheSeedDir;
};

/// The device the engine is actually running on (SESSION_LOG_SPEC §4).
///
/// Every field already existed engine-side — the shader cache's fingerprint has
/// read vendor/device/driver out of RenderSystemCapabilities since it shipped —
/// but none of it was on the boundary, so a session log could not say which GPU
/// produced a picture. Ogre-free by construction: plain strings, filled by the
/// one TU that may include Ogre.
///
/// `apiVersion` is a SCRAPE, not an accessor: RenderSystemCapabilities carries
/// no API version, and the Vulkan render system only ever states it as a log
/// line ("Vulkan: API Version: X.Y.Z (0x...)"). The engine's log listener
/// captures it — the same "the verdict exists only as a log line" pattern the
/// shader cache's pipelineCacheReason already uses. Empty on a backend that
/// does not say, which is not an error.
struct DeviceInfo {
    std::string renderSystem;    ///< "Vulkan Rendering Subsystem", "NULL Rendering Subsystem", ...
    std::string vendor;          ///< RenderSystemCapabilities::getVendor()
    std::string deviceName;      ///< getDeviceName()
    std::string driverVersion;   ///< getDriverVersion().toString()
    std::string apiVersion;      ///< scraped; empty when the backend never says
};

/// What the monitor is doing. `Review` is the one recording level — the spec's
/// old Recorder/Compact/Full ladder collapsed to it when the HUD was cut.
enum class MonitorLevel {
    Off,     ///< nothing attached, nothing allocated, nothing read
    Review   ///< per-pass records, cache work + reasons, events, GPU samples
};

}}  // namespace jahshaka::engine
