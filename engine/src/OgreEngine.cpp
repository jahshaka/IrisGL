// The Ogre-Next 4.0 backend: the Engine object itself (Root, render systems, Hlms
// registration, views, scenes, the frame loop) and the Engine::create factory.
//
// engine/src/ is the only directory that includes Ogre; the shared declarations
// live in EnginePrivate.h, which documents the invariants this backend rests on.
#include "EnginePrivate.h"
#include "GpuCheckpoints.h"
#include "photon/voxel/PhotonMipChain.h"
#include "photon/voxel/PhotonVoxelLighting.h"
#include "photon/voxel/PhotonVoxelScroll.h"
#include "photon/voxel/PhotonPassBinding.h"
#include "photon/voxel/PhotonSpecularAlbedoBuffer.h"

#include <OgreHlmsCompute.h>
#include <OgreHlmsAsyncCompiler.h>
#include "AtomPass.h"
#include "HlmsAtom.h"

#if JAH_RAY_QUERY
// For the injected Vulkan frame faults only (raiseFrameFault): the render
// system's own failure function and its device. Linked whenever the ray tier
// is, which is every Linux build.
#include "OgreVulkanDevice.h"
#include "OgreVulkanRenderSystem.h"
#endif

#include <cctype>
#include <dlfcn.h>
#if defined(__linux__)
#include <link.h>   // dl_iterate_phdr: the layer libraries a process loaded (validation())
#elif defined(__APPLE__)
#include <mach-o/dyld.h>   // _dyld_image_count / _dyld_get_image_name: the same walk on the Mac
#endif
#include <set>
#include <unistd.h>

#include <OgreDepthBuffer.h>
#include <OgreFrameStats.h>
// The frame loop's clock. `Root::getTimer()` returns `Ogre::Timer *` and
// OgreRoot.h forward-declares it only — the inlined FrameStats sample
// (renderOneFrame, mirroring OgreRoot.cpp:1123) calls through it.
#include <OgreTimer.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>

#include "core/threadbudget.h"   // the multiload's width (THREADING-1)

namespace jahshaka { namespace engine {
namespace detail {
namespace {

/// The one live engine in this process. Ogre::Root is a Singleton: a second
/// `new Root` asserts, so create() refuses while this is set.
OgreEngine *gLiveEngine = nullptr;

/// "Lowest latency vsync" — MAILBOX rather than FIFO — is encoded by Ogre in
/// the SIGN BIT of the vsync interval (VulkanWindowSwapChainBased::setVSync
/// masks the low 31 bits into mVSyncInterval and reads bit 31 into
/// mLowestLatencyVSync). Named here so no call site carries a bare 0x80000000.
const Ogre::uint32 kLowestLatencyVSync = 0x80000000u;

}  // namespace

bool OgreEngine::startRenderThread(const EngineConfig &cfg, std::string &error) {
    // THE AFFINITY LAW IS ON from here (decision 39; JAH_DEBUG builds assert it): a counted entry the host calls
    // instead of posting, requesting or joining stops the process with its name.
    gAffinityChecks.store(true, std::memory_order_relaxed);
    // The host's side while it waits on the render thread: the compile observer, delivered here (it repaints Qt's
    // splash, which only the host's thread may touch).
    mRenderThread.setHostPump([this] {
        const unsigned long long ticks = mCompileTicks.load(std::memory_order_relaxed);
        if (ticks != mCompileTicksDelivered) {
            mCompileTicksDelivered = ticks;
            if (mHostCompileObserver) mHostCompileObserver();
        }
    });
    std::string bootError;
    const bool ok = mRenderThread.start([&] { return init(cfg, bootError); });
    if (!ok) error = bootError;
    return ok;
}

bool OgreEngine::init(const EngineConfig &cfg, std::string &error) {
    EngineSideScope engineSide;   // the boot: the engine's own calls inside it are the render side's (U3 4a)
    mHostRecords = cfg.hostRecords || cfg.renderThread;
    mHostRecordsMode = mHostRecords;
#ifdef __linux__
    mDisplay = reinterpret_cast<void *>(cfg.display);
#else
    (void)cfg.display;  // X11-only; 0 on other hosts (Types.h documents the leak)
#endif
    mDefaultSamples = OgreView::sanitizeSamples(cfg.sampleCount);
    mVsync = cfg.vsync;
    mHostVsync = mVsync;
    // THE HARDWARE RAY-TRACING PREFERENCE at boot (PHOTON_SPEC §7 R1): the tier
    // comes up only where the device advertises it AND this says yes.
    // `EngineConfig::rayTracing` is the HOST's answer — Studio fills it from the
    // application preference (Preferences > Rendering) ANDed with
    // --no-ray-query. The environment variable is kept as the override a suite
    // can set when it cannot reach the config.
    mRayTracingWanted = cfg.rayTracing && getenv("JAHSHAKA_NO_RAY_QUERY") == nullptr;
    mHostRayTracing = mRayTracingWanted;   // the host's record (Engine::rayTracing)
    // ...and OFF REACHES THE DEVICE: the fork's VulkanDevice asks for the ray
    // extensions and feature bits only while this static says so, so a no-rays
    // process runs on exactly the device a machine without the hardware gets.
    // Before the render system loads (and before the OpenXR route builds its
    // device request, which reads the same static). A build without the ray
    // tier (the Mac, a Linux box without glslang or the Vulkan headers) has no
    // ray extension to ask for and does not see the device's header
    // (PHOTON-7 stage 0's read R1, the no-ray arm's build).
#if JAH_RAY_QUERY
    Ogre::VulkanDevice::msRayQueryAllowed = mRayTracingWanted;
#endif
    // THE GPU CHECKPOINTS (GpuCheckpoints.h): JAHSHAKA_GPU_CHECKPOINTS=1 asks the device for the marks, before it exists
    detail::gpuCheckpointsConfigure();
    // Process-wide static, read by Mesh::prepareForShadowMapping at mesh-build
    // time (POST_CHAIN_SPEC.md §11). Setting it before Root exists is fine — it
    // is a plain static, not engine state.
    Ogre::Mesh::msOptimizeForShadowMapping = cfg.optimizeShadowMeshes;
    mHostShadowMeshOptimization = cfg.optimizeShadowMeshes;
    mMediaDir = cfg.hlmsMediaDir;
    if (!mMediaDir.empty() && mMediaDir.back() != '/') mMediaDir += '/';
    mHeadless = cfg.headless;
    mIdentityHeadless = mHeadless;
    // THE BACKGROUND SHADER COMPILER (ASYNC-SHADERS-1). JAH_ASYNC_SHADER_THREADS overrides
    // the host's count (0 = none) for A/B measurement without a rebuild.
    mAsyncShaderThreads = cfg.asyncShaderThreads;
    if (const char *forced = std::getenv("JAH_ASYNC_SHADER_THREADS"))
        mAsyncShaderThreads = unsigned(std::max(0l, std::min(8l, std::strtol(forced, nullptr, 10))));
    // The shader cache's fingerprint hashes the staged Hlms tree, so it is
    // configured as soon as the media directory is known — before Root, long
    // before anything could compile. The LOAD waits for ensureHlms().
    //
    // NEVER under the NULL render system: a headless run compiles nothing worth
    // keeping, and the fingerprint (§4.2) does not name the render system — a
    // blob written here would be offered verbatim to a real Vulkan device on
    // the next launch. Silently off, not an error: the caller's config is a
    // reasonable thing to reuse between a windowed and a headless run.
    mShaderCache.configure(cfg.headless ? std::string() : cfg.shaderCacheDir, mMediaDir, cfg.shaderCacheMaxBytes);
    // The TEXTURE cache (THREADING_ADOPTION_SPEC.md P2 items 6-7) rides the same
    // directory and the same headless rule, with its own manifest and its own
    // key (I-5: a resolution and a channel count are properties of a FILE, so
    // its key names neither the GPU nor the driver and a driver update does not
    // throw it away). Configured here — no I/O — and LOADED in ensureHlms().
    // JAH_TEXTURE_CACHE=0 switches BOTH halves off for a run — the third and
    // last step of P2's order of retreat (pool -> wait -> caches), and the arm
    // of the G2-c measurement that isolates what the caches are worth.
    {
        const char *cacheEnv = std::getenv("JAH_TEXTURE_CACHE");
        const bool cacheOff = cacheEnv && cacheEnv[0] == '0';
        textureCache().configure(cfg.headless || cacheOff ? std::string() : cfg.shaderCacheDir);
    }
    try {
        mAbiCookie = Ogre::generateAbiCookie();
        mRoot = new Ogre::Root(&mAbiCookie, "", "",
                               cfg.logFile.empty() ? "jahshaka-ogre.log" : cfg.logFile,
                               "Jahshaka");
#if JAH_VR
        mHostVrBuilt = true;   // a VR-built engine with a Root (Engine::vrAvailable, the host's record)
#endif
        // Shader accounting starts here, with the log: Ogre has no compile
        // callback, but it names every compile and every microcode hit in two
        // fixed sentences, and the counters are what the startup progress
        // display and the cache tests both read. Runs whether or not the cache
        // itself is enabled.
        mShaderCache.attachCounters();
        // The engine's log bridge (SESSION_LOG_SPEC F3-B), attached in the same
        // breath and for the same reason: everything after this point —
        // plugin loads, render-system init, device detection, and every Vulkan
        // validation error — is captured. The only lines missed are those
        // emitted INSIDE Root's constructor, which is the ABI-cookie check; an
        // ABI mismatch still lands in Ogre's own sibling file.
        attachLogBridge();
        if (cfg.logSink) setLogSink(cfg.logSink);
        // HEADLESS = the NULL render system (Types.h EngineConfig::headless).
        // It ships in every engine install unconditionally, needs no display,
        // no driver and no device, and its VaoManager/TextureGpuManager hand
        // out real objects backed by plain memory — meshes, Items, datablocks
        // and queries all behave (verified end to end by the Studio suite
        // tests/engine/test_engine_headless.cpp — the lane's first assertion —
        // and by spikes/scenegraph-null-rs for the graph half).
        const char *plugin = cfg.headless ? "RenderSystem_NULL" : "RenderSystem_Vulkan";
        // OPENXR (VR-START-1): NOTHING AT BOOT. Every engine runs on its own
        // VkInstance/VkDevice, which the fork creates with the OpenXR interop
        // set; a session connects later through XR_KHR_vulkan_enable. The
        // policy only says whether, and to which runtime, a session may connect.
        mVrPolicy = cfg.vr;
        mHostVrPolicy = mVrPolicy;
        mVrInfo.failure = !cfg.vr.enabled ? VrFailure::Disabled
                        : cfg.headless    ? VrFailure::Headless
                                          : VrFailure::None;
        mVrInfo.reason = !cfg.vr.enabled ? "VR is off for this process"
                       : cfg.headless    ? "this engine is headless (the NULL render system renders nothing)"
                                         : "no headset has been asked for yet";
        mRoot->loadPlugin(cfg.pluginDir + "/" + plugin, false, nullptr);
        // ParticleFX2: the SIMULATION half of the particle system. Its core
        // (definitions, instances, the manager, BillboardSet2) lives in
        // OgreNextMain and needs no plugin — but every emitter and affector
        // FACTORY is registered by this plugin's install(), through statics on
        // ParticleSystemManager2. Without it addEmitter("Point") has nothing to
        // ask. install() also calls Hlms::_setHasParticleFX2Plugin(true), which
        // ensureHlms() used to do by hand; that call stays (it is idempotent and
        // it must still hold if this plugin ever fails to load).
        //
        // A missing plugin is NOT fatal: billboard sets (light icons) and every
        // non-particle feature work without it. setParticleSystem reports the
        // failure through lastError() instead of taking the whole engine down.
        try {
            mRoot->loadPlugin(cfg.pluginDir + "/Plugin_ParticleFX2", false, nullptr);
            mHasParticleFX2 = true;
        } catch (const Ogre::Exception &e) {
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: Plugin_ParticleFX2 not loaded, particle simulation disabled: " +
                std::string(e.getDescription()), Ogre::LML_CRITICAL);
        }

        const Ogre::RenderSystemList &list = mRoot->getAvailableRenderers();
        if (list.empty()) { error = "no Ogre render systems available"; return false; }
        mRoot->setRenderSystem(list[0]);
        mBackendName = list[0]->getName();
        // AUTO-CREATE THE WINDOW ONLY WHEN HEADLESS. Ogre's rule (a render
        // target must exist before registerHlms/createSceneManager — really
        // RenderSystem::oneTimePostWindowInit, which is private and only ever
        // called from createRenderWindow) has to be satisfied somehow, and
        // under the NULL system the cheapest satisfaction is the render
        // system's own 1x1 window: initialise(true) makes it, and it needs
        // nothing from the machine. Keeping the pointer means the (refused)
        // view calls never make a second one.
        //
        // A RENDERING boot stays exactly as it always was — initialise(false),
        // no window, no device — so the first window a Vulkan session creates
        // is the host's real one whenever the host can wait that long.
        // NO STENCIL IN THE DEFAULT DEPTH (SPEED-VR-MEM, perf audit M-5). Every depth
        // Ogre pools for a target — the window's, the VR eye pair's, the scratch depth the
        // overlay passes clear and test their gizmos against (OgreChain.cpp
        // clearOverlayDepth) — takes DepthBuffer::DefaultDepthBufferFormat, which Ogre
        // picks at the render system's initialisation from this mask: with the stencil
        // bit it is D32_FLOAT_S8X24_UINT (8 bytes a sample; measured 82 MB on the VR eye
        // pair, spikes/ham-1). Nothing samples or tests a pooled stencil: SMAA, the one
        // stencil user, declares its own D32S8 explicitly. Without the bit the pool depth
        // is D32_FLOAT, the same 32-bit float depth, so every depth test and every pixel
        // is what it was. Ogre's documented knob for exactly this (OgreDepthBuffer.h),
        // set before the render system initialises.
        Ogre::DepthBuffer::AvailableDepthFormats =
            Ogre::uint8(Ogre::DepthBuffer::DFM_D32 | Ogre::DepthBuffer::DFM_D24 | Ogre::DepthBuffer::DFM_D16);
        mNullWindow = mRoot->initialise(cfg.headless, "jahshaka-headless");
        // THE ENGINE HAS NO WALL CLOCK (Engine.h "Simulation clock"): its
        // frame-time source is put in frame-delay mode right here, before any
        // frame, and stays there. The host's SimulationClock pushes the real
        // per-frame value; this default is what a host that never pushes gets.
        // After initialise(), not after the Root constructor: Root creates its
        // ControllerManager in initialise() (OgreRoot.cpp:751).
        Ogre::ControllerManager::getSingleton().setFrameDelay(kDefaultFrameDelta);
        // ATOM stage 1's view rule, before anything can create a mesh: the LOD
        // value arrays are written against the DEFAULT strategy's base value and
        // an Item caches the array's address at _initialise (OgreMesh.cpp).
        detail::installJahLodStrategy();
        // NOTE: Hlms registration is deferred to the first view. The VaoManager
        // does not exist until a render target is created, and HlmsUnlit/HlmsPbs
        // registration walks it via ConstBufferPool::_changeRenderSystem —
        // registering here segfaults.
        // A HEADLESS engine's render system is the device it will ever have (the NULL one): its constants now.
        if (mHeadless) noteDevice();
        return true;
    } JAH_CATCH(error, false);
}

// THE BLANK WORLD (lane STALE-VIEW-1). A compositor workspace needs a
// SceneManager and a camera, and a View with no scene has neither — so the
// clear-only chain a scene-less View owns (chain::buildBlank) runs against this
// one. It holds nothing, ever: no item, no light, no forward+ setup. The only
// pass that names it renders the OVERLAY queues, which are screen-space, and
// the only object it owns is one camera per View that has ever lost a scene.
//
// ZERO WORKER THREADS (THREADING_ADOPTION_SPEC.md P5): at 0 Ogre sets mForceMainThread and runs the
// parallel passes inline with no barrier and no thread. An empty world updated
// once a frame through an inline pass is free; a thread pool for it would not be.
Ogre::SceneManager *OgreEngine::blankSceneManager() {
    if (mBlankScene) return mBlankScene;
    if (!mHlmsRegistered) { mLastError = "blankSceneManager: Hlms unavailable"; return nullptr; }
    JAH_TRY {
        mBlankScene = mRoot->createSceneManager(Ogre::ST_GENERIC, 0u,
                                                processUniqueName("jahshaka-blank"));
        // The engine-drawn overlay's render-queue half is per SceneManager
        // (STATS_OVERLAY_SPEC §2.1) — and this manager exists precisely so the
        // HUD still has somewhere to draw when the world is gone.
        hud::attach(mBlankScene);
        return mBlankScene;
    } JAH_CATCH(mLastError, nullptr);
}

// THE ONE-TIME PREPARATION a scene manager and a device cannot exist without (a render target, then the
// Hlms/resource registration — CLAUDE.md's startup order). A headless engine already has its window (the NULL render
// system's own, made in init); a rendering one gets a surfaceless one. What the document's staging scene manager used
// to force at boot before ENGINE-UPGRADE-U2 deleted it — a document needs no engine — is now asked for by name.
bool OgreEngine::prepareDevice() {
    if (hostPosts()) {   // the boot's door: on the render thread (U3 stage 4b), its refusal the host's (decision 44)
        bool ok = false;
        mRenderThread.runOn([&] { EngineSideScope engineSide; ok = prepareDevice(); moveErrorToHost(); });
        return ok;
    }
    // THE BOOT'S DOOR (EngineHost, once, right after create — stage 4b runs it on the render thread inside create).
    if (mHlmsRegistered) return true;
    JAH_TRY {
        if (!mNullWindow) {
            Ogre::NameValuePairList wp; wp["windowType"] = "null";
            mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"), 8, 8, false, &wp);
            noteDevice();   // the device exists from here (Engine::deviceInfo / rayQueryAvailable)
        }
        ensureHlms();
        if (!mHlmsRegistered) { mLastError = "prepareDevice: Hlms unavailable"; return false; }
        return true;
    } JAH_CATCH(mLastError, false);
}

Scene *OgreEngine::createScene(const std::string &name, unsigned workerThreads) {
    // THE HOST'S CREATE IS A MESSAGE (U3 stage 4a, decision 39 b): the scene object is the host's handle at once — its
    // commands queue behind the create in the record — and its manager is made at the drain (adoptScene).
    if (hostPosts()) {
        if (!mHostSceneNames.insert(name).second) { noteHostError("Scene '" + name + "' already exists"); return nullptr; }
        auto holder = std::make_shared<std::unique_ptr<OgreScene>>(new OgreScene(mRoot, nullptr, name, mLastError));
        OgreScene *raw = holder->get();
        raw->bindEngine(this);
        postedAsMessage([holder, workerThreads](OgreEngine &e) { return e.adoptScene(std::move(*holder), workerThreads); });
        return raw;
    }
    std::unique_ptr<OgreScene> made;
    return adoptScene(std::move(made), workerThreads, name) ? mScenes.back().get() : nullptr;
}

bool OgreEngine::adoptScene(std::unique_ptr<OgreScene> scene, unsigned workerThreads) {
    const std::string name = scene ? scene->name() : std::string();
    return adoptScene(std::move(scene), workerThreads, name);
}

bool OgreEngine::adoptScene(std::unique_ptr<OgreScene> adopted, unsigned workerThreads, const std::string &name) {
    // A SCENE BEFORE ANY VIEW: the backend's one-time preparation happens here (prepareDevice).
    if (!mHlmsRegistered && !prepareDevice()) return false;
    if (!mHlmsRegistered) {
        mLastError = "createScene('" + name + "'): no View exists yet — create a View first";
        return false;
    }
    // THE HOST'S NAME RECORD IS THE LAW when the host's calls are records (decision 45, D2): a destroy and a re-create
    // of one name in one record drain create-first (the destroy is late), so the engine must not refuse the new one.
    if (!mHostRecords || !adopted)
        for (auto &s : mScenes)
            if (s->name() == name) { mLastError = "Scene '" + name + "' already exists"; return false; }
    JAH_TRY {
        // WORKER THREADS ARE PER SCENE, and the caller decides (Engine.h).
        // Ogre forks culling, render-queue building and object updates across
        // this pool and joins them on a barrier; every SceneManager owns its
        // own, so a process with an editor scene, a player scene, a thumbnail
        // scene and three preview scenes has six pools. 2 was hardcoded here
        // for every one of them — the fps audit's F3 — which left a 16-core
        // box drawing the editor on two threads while six thumbnail scenes
        // held two each. 0 keeps that historical default so that no caller
        // that does not care has to think about it.
        // TWO WAYS TO SAY ZERO, and only one of them reaches Ogre as zero
        // (THREADING_ADOPTION_SPEC.md P5). `0` keeps its historical meaning —
        // "I do not care", answered with the old hardcoded 2 — while
        // kSceneMainThreadOnly (Types.h) is the caller who genuinely wants no
        // pool: Ogre then sets mForceMainThread, spawns nothing, and runs every
        // parallel pass inline with no barrier (OgreSceneManager.cpp:171,
        // :4705-4717). Passing 1 instead is strictly worse — a thread is
        // spawned and two barrier syncs are paid per pass to do the same serial
        // work.
        const unsigned threads = workerThreads == kSceneMainThreadOnly ? 0u
                                 : workerThreads == 0u                 ? 2u
                                 : (workerThreads > 32u ? 32u : workerThreads);
        // ...and Ogre's instance name is unique however the host's names overlap in one record (the old manager is
        // still alive when the re-create drains).
        const std::string smName = mRoot->hasSceneManager(name) ? processUniqueName(name.c_str()) : name;
        Ogre::SceneManager *sm = mRoot->createSceneManager(Ogre::ST_GENERIC, threads, smName);
        // HlmsPbs shades point and spot lights ONLY through Forward+ (Forward3D /
        // ForwardClustered); without it only directional lights reach the shader.
        // 16x8 grid, 24 slices, 2..50 units depth range.
        //
        // CUBEMAP PROBES PER CELL: the STARTING budget only
        // (kCubemapProbeSlotsDefault, EnginePrivate.h, which carries the whole
        // argument and the Grand Showroom measurement). A scene that arms the
        // VCT+PCC hybrid grows it to hold its own probe grid in buildPcc, so
        // this value is what a scene with no probes at all pays. Per-pixel PCC
        // is culled through this grid and
        // ForwardClustered::collectObjsForSlice drops, silently and per cell,
        // every probe past the budget — which is the black-rectangle defect the
        // derived budget exists to remove.
        //
        // 96 LIGHTS PER CELL STAYS, and the spec's recommended companion cut to
        // 32 is DELIBERATELY NOT TAKEN — measured, not argued. The spec's case
        // was "32 forward lights overlapping ONE cluster cell is beyond any
        // scene we ship". That is true of lights a user places and false of the
        // ones the renderer plants: INSTANT RADIOSITY's virtual point lights
        // ride this very list (setEnableVpls, OgreGi.cpp), and at Medium quality
        // it plants enough of them that the cap is what decides how much bounce
        // survives. gi.modes' floor bounce, same scene, same everything else:
        //     96 lights/cell -> r 0.831 g 0.506   (the shipped look)
        //     64             -> r 0.471 g 0.451   (nearly gone)
        //     48             -> r 0.427 g 0.427   (gone: no red at all)
        //     32             -> r 0.388 g 0.388   (gone)
        // So the 384 KiB the cut would have saved costs a whole GI mode. Worth
        // recording the other direction too: at 96 the bounce is still being
        // clipped by this cap, so RAISING it would brighten Instant Radiosity —
        // a measurement for whoever next owns that mode, not a change to make
        // while chasing reflections.
        //
        // The slot change shifts hlms properties (the cubemap slot offset,
        // OgreForwardClustered.cpp), so every Forward+ shader recompiles ONCE;
        // the shader cache self-invalidates on the engine build id. A slow first
        // run after this build is that, not a defect. Rendered pixels must come
        // back identical — CPU fill and shader read use the same offsets — which
        // is what the full pixel sweep gates.
        sm->setForwardClustered(true, 16, 8, 24, 96, kDecalsPerCell,
                                kCubemapProbeSlotsDefault, 2.0f, 50.0f);
        // Shadow maps cover nothing until these are set (Ogre's samples set both).
        sm->setShadowDirectionalLightExtrusionDistance(500.0f);
        sm->setShadowFarDistance(500.0f);
        // PARTICLE QUOTA CEILING — must be set HERE, before anything in this
        // scene calls init() on a particle definition or a billboard set.
        // ParticleSystemManager2 sizes ONE shared index buffer for the whole
        // scene, on the first init(), from the highest quota it knows about at
        // that moment (calculateHighestPossibleQuota, OgreParticleSystemManager2
        // .cpp:823-861). Our light-icon billboard sets are quota 1 and they
        // initialise as soon as a scene gets a light — so without this the
        // ceiling would be 1 and the first real emitter would either throw
        // "Raising highest possible quota after initialization is not yet
        // implemented" or draw against an index buffer sized for four vertices.
        //
        // kMaxParticleQuota is the per-DEFINITION cap the whole engine enforces
        // (setParticleSystem clamps to it). The buffer costs
        // kMaxParticleQuota * 4 * 6 * 2 bytes = 768 KiB of immutable index data
        // per scene, allocated lazily on the first particle draw.
        sm->getParticleSystemManager2()->setHighestPossibleQuota(kMaxParticleQuota, 0u);
        // The engine-drawn overlay's render-queue half is registered PER
        // SceneManager (STATS_OVERLAY_SPEC §2.1) — which is what makes it
        // per-scene free. Whether anything is actually DRAWN is decided per
        // pass (ChainDesc::overlays) and per element (hud::apply).
        hud::attach(sm);
        if (adopted) {
            adopted->adoptManager(sm);
            mScenes.emplace_back(std::move(adopted));
        } else {
            mScenes.emplace_back(new OgreScene(mRoot, sm, name, mLastError));
        }
        // The scene's per-scene shadow request resolves against GLOBAL engine
        // state (one filter, one atlas — Scene::setShadowSettings), so it needs
        // its engine.
        mScenes.back()->bindEngine(this);
        return true;
    } JAH_CATCH(mLastError, false);
}

// A DESTROY ASKED FOR FROM INSIDE A FRAME IS DEFERRED, NOT REFUSED
// (VR-INPUT-1E-FIX finding 5).
//
// Nothing in this tree destroys a scene from inside renderOneFrame, and doing
// it there cannot be honoured on the spot: a VR session bound to the scene
// would be ended between its own xrBeginFrame and xrEndFrame (the runtime's
// frame contract, broken with no log to explain it), and the render system is
// holding the scene's own buffers for the frame in flight. The first cut
// REFUSED — and a refusal on a `void` call is a ghost: `lastError()` was not
// even set, and every host in the tree nulls its Scene pointer the moment it
// has asked, so the scene would have leaked with nobody able to name it.
//
// So the request is remembered and drained at the frame's TAIL, where the same
// call is sound. The host's contract is unchanged ("the Scene is gone when you
// asked for it to be"); what moves is by how many microseconds.
void OgreEngine::destroyScene(Scene *scene) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (hostPosts() && scene) {
        mHostSceneNames.erase(scene->name());
        // ...and the host's records of the views it bound to it (decision 66: the engine never writes them).
        std::lock_guard<std::mutex> lock(mHostViewsLock);
        for (View *v : mHostViews)
            if (v && v->scene() == scene) static_cast<OgreView *>(v)->forgetHostBinding();
    }
    if (postedAsMessage([=](OgreEngine &e) { e.destroyScene(scene); return true; })) return;   // in the one order
    if (!scene) return;
    // ITS PART OF THE PENDING RECORD GOES WITH IT (the 3a read, C): a scene re-created at the same address before
    // the next drain must not receive the dead one's fields.
    mRenderThread.withPending([&](FrameInput &p, bool) {
        auto &v = p.scenes;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const FrameInputScene &q) { return q.scene == scene; }), v.end());
    });
    if (mInRenderFrame) {
        for (Scene *pending : mPendingSceneDestroy)
            if (pending == scene) return;          // asked for twice in one frame
        mPendingSceneDestroy.push_back(scene);
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: destroyScene() was called from INSIDE a frame - deferred to this "
            "frame's tail (a session rendering it is ended there, in order)",
            Ogre::LML_NORMAL);
        return;
    }
    for (auto it = mScenes.begin(); it != mScenes.end(); ++it) {
        if (it->get() != scene) continue;
        // The background compiler's draw counts outlive the scene (asyncShaderStats).
        if (Ogre::SceneManager *sm = (*it)->sceneManager()) {
            unsigned long long capPh = 0, capSkip = 0;
            surfaceCacheAsyncDraws(sm, capPh, capSkip, /*forget*/ true);
            mRetiredPlaceholderDraws += sm->getRenderQueue()->getNumPlaceholderDraws() - capPh;
            mRetiredPendingSkips += sm->getRenderQueue()->getNumPendingSkips() - capSkip;
        }
        // A SESSION MUST NOT OUTLIVE THE WORLD IT RENDERS (VR-4-FIX finding 1).
        //
        // The session holds a raw `OgreScene *`: it dereferences it every frame
        // (syncStereoQuads' mark-and-sweep over the scene's Rectangle2Ds) and
        // again in its destructor (the cull camera it created in that scene's
        // manager). A host that closes a project while the editor's VR preview
        // runs used to free this scene with all of that still live — a
        // use-after-free one frame later, every time.
        //
        // The HOST ends it first and properly (EditorVrPreview::end, which also
        // gives the viewport its fly keys back); this is the belt, so no host
        // can repeat it. Ending here rather than at the top of the function is
        // deliberate: only a session bound to THIS scene is anybody's business,
        // and the two advanceResources() calls below are the drain its Views,
        // RTTs and swapchains need — which is exactly the drain endVrSession
        // relies on its caller for.
        if (mVrSession && vrSessionScene(mVrSession) == it->get()) {
            // IN A FRAME THIS IS NOT REACHED AT ALL (VR-INPUT-1E-FIX finding
            // 5): the top of this function defers such a call to the frame's
            // tail, so by the time the session is ended here the frame is
            // closed and xrEndFrame has been paired with its xrBeginFrame.
            //
            // The HOST ends it first and properly; this is the belt, and since
            // the hosts are ordered (the Player and the editor preview both end
            // their session before the scene goes) it is a plain line rather
            // than a critical one.
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka VR: a session was still rendering the scene '" + (*it)->name() +
                    "' when it was destroyed - the engine ends it first",
                Ogre::LML_NORMAL);
            endVrSession();
            mVrMirrorView = nullptr; if (!mHostRecords) mHostVrMirrorView = nullptr;
        }
        for (auto &v : mViews)
            if (static_cast<Scene *>(v->boundOgreScene()) == scene) v->detachScene();   // the engine's binding (U3 65's TSan)
        // THE BELT ON THE TEARDOWN (lane OPEN-FRAMES-1). Destroying a scene
        // frees every mesh, texture and buffer it owned, and each of those
        // goes onto the backend's delayed-release lists — which only
        // advanceResources()/a frame drains. A host that closes a world and
        // opens the next one without rendering in between would otherwise hand
        // the whole previous world to those lists in one go.
        //
        // ONCE BEFORE, so the lists start this teardown empty, and once AFTER
        // (below, past the erase) so what the teardown just freed is submitted
        // and recycled rather than waiting for a frame that may not come.
        // Cheap on a healthy process: a scene destroy is not a hot path.
        advanceResources();
        (*it)->destroy();
        mScenes.erase(it);
        advanceResources();   // see the note above the first call
        return;
    }
    mLastError = "destroyScene: unknown Scene";
}

void OgreEngine::drainPendingSceneDestroys() noexcept {
    if (mPendingSceneDestroy.empty()) return;
    // SWAPPED OUT FIRST: destroyScene tears down Views, workspaces and a VR
    // session, and a host callback reached from any of that may ask for another
    // scene — which then belongs to the NEXT frame's tail, not to this loop.
    std::vector<Scene *> pending;
    pending.swap(mPendingSceneDestroy);
    for (Scene *scene : pending) {
        // NOTHING MAY ESCAPE: the frame guard's destructor calls this.
        try { destroyScene(scene); }
        catch (Ogre::Exception &e) { mLastError = e.getFullDescription(); }
        catch (std::exception &e)  { mLastError = std::string("engine: ") + e.what(); }
        catch (...)                { mLastError = "destroyScene: unknown exception"; }
    }
}

View *OgreEngine::createView(const std::string &name,
                             NativeWindowHandle handle, unsigned width, unsigned height,
                             const Colour &background) {
    // THE FIRST JOIN (U3 stage 4b): the window's surface is made on the render thread; the host posted the winId and
    // waits (Qt's rule: the native window exists and outlives the surface).
    // ...and its refusal is moved into the host's error at the rendezvous (decision 44): the host never reads the
    // engine's sink. The pending record's structure lands first, as destroyView's does (decision 46).
    if (hostPosts()) {
        View *made = nullptr;
        mRenderThread.runOn([&] {
            EngineSideScope engineSide;
            applyPendingEngineMessages();
            made = createView(name, handle, width, height, background);
            moveErrorToHost();
        });
        return made;
    }
    EngineSideScope engineSide;   // a join: the engine's own calls inside it are the render side's (U3 4a)
    if (viewNameTaken(name)) return nullptr;
    if (mHeadless) {
        // A polite, documented refusal (Types.h EngineConfig::headless), not a
        // crash and not a View that draws nothing: the NULL render system has
        // no swapchain and no device to present with.
        (void)handle; (void)width; (void)height; (void)background;
        mLastError = "createView('" + name + "'): this engine is headless (NULL render system) "
                     "— it has no window system and renders nothing";
        return nullptr;
    }
#if !defined(__linux__) && !defined(__APPLE__)
    // On-screen views need a native Vulkan window backend; this platform has
    // none yet. Offscreen views and the null window (headless) work everywhere.
    (void)handle; (void)width; (void)height; (void)background;
    mLastError = "createView: on-screen engine views are not yet supported on this platform "
                 "(headless/offscreen rendering is available)";
    return nullptr;
#else
    JAH_TRY {
        Ogre::NameValuePairList params;
#ifdef __APPLE__
        // macOS: the handle is the host's NSView (Types.h). Ogre's Metal window
        // (fork 1a81f866a+d014b064f+1bccc3f93 (was 0007)) hosts its OWN CAMetalLayer-backed child view inside
        // it and builds the VkSurfaceKHR from that layer through
        // VK_EXT_metal_surface — the host's own layer is never replaced, because
        // toolkits that manage their layer (Qt's QNSView) refuse the replacement.
        if (!handle) { mLastError = "createView: host must supply its NSView"; return nullptr; }
        params["externalWindowHandle"] = Ogre::StringConverter::toString((unsigned long long)handle);
#else
        // Ogre consumes the SDL2x11 struct synchronously inside createRenderWindow;
        // a stack local is correct (the old heap vector was a leak).
        X11Handle x11{ mDisplay, (unsigned long)handle };
        if (mBackendName.find("Vulkan") != std::string::npos) {
            // Vulkan/XCB takes only "SDL2x11": a pointer to {Display*, Window}.
            if (!mDisplay) { mLastError = "createView: host must supply its X display"; return nullptr; }
            params["SDL2x11"] = Ogre::StringConverter::toString((unsigned long)&x11);
        } else {
            params["parentWindowHandle"] = Ogre::StringConverter::toString((unsigned long)handle);
            params["gamma"] = "true";
        }
#endif
        // VSYNC IS THE HOST'S CHOICE (EngineConfig::vsync / Engine::setVsync,
        // fps audit F1) — it was hardcoded true here and in the recreate hook
        // below. Off means an immediate, tearing present mode: the swapchain
        // acquire stops blocking, and the loop's rate stops quantizing to
        // refresh/n. Everything else about the request is unchanged.
        params["vsync"]         = mVsync ? "true" : "false";
        params["vsyncInterval"] = "1";
        // MAILBOX, not FIFO (deep audit area 7 F8). Plain vsync gives Vulkan's
        // FIFO present mode: a queue up to the swapchain's depth, so a frame
        // submitted now is shown up to ~4 refreshes later — four frames of
        // latency between dragging a gizmo and seeing it move — and it paces the
        // whole loop against the 16ms driver timer (two pacers beating against
        // each other, ~62fps on a 60Hz panel and no better on a 144Hz one).
        // MAILBOX keeps the vsync tear-free guarantee and drops every frame the
        // display did not get to: latest-wins, one frame deep.
        //
        // Ogre expresses it two ways and both are set here, deliberately:
        //   * miscParams "vsync_method" = "Lowest Latency" — read by
        //     parseSharedParams BEFORE the first createSwapchain, so the very
        //     first swapchain is already MAILBOX;
        //   * setVSync's sign bit (kLowestLatencyVSync) — setVSync would
        //     otherwise CLEAR mLowestLatencyVSync on this very call (it assigns
        //     the flag before its "nothing changed" early-return), and it is
        //     what survives into every later swapchain rebuild.
        // Selection is graceful BY CONSTRUCTION: OgreVulkanWindow's present-mode
        // search tries MAILBOX, then falls back to FIFO, which the spec
        // guarantees every surface supports. No capability gate is needed here,
        // and the outcome is always in the engine log ("Trying presentMode =
        // MAILBOX_KHR" / "Chosen presentMode = ...").
        //
        // MEASURED 2026-09-04, and not what you would guess: whether MAILBOX
        // exists is a property of the WSI, not of the GPU. On this box's
        // xcb surfaces the NVIDIA driver offers only FIFO / IMMEDIATE /
        // FIFO_LATEST_READY — NO MAILBOX — while lavapipe on the same X server
        // offers it, and NVIDIA offers it on a WAYLAND surface. Studio forces
        // xcb (Ogre has no Wayland backend), so on Linux/NVIDIA this request
        // currently lands on the FIFO fallback and changes nothing; it is the
        // correct request everywhere else (Mesa, MoltenVK, win32) and costs
        // nothing where it is refused. The real Linux/NVIDIA lever is
        // VK_EXT_present_mode_fifo_latest_ready, which that list shows and Ogre
        // does not know about at this pin — an ogre-patch, not this call.
        params["vsync_method"] = "Lowest Latency";
        // MSAA: the FSAA misc param must be passed at EVERY window creation —
        // here AND in the resize lambda below, or a resize silently resets it.
        params["FSAA"] = Ogre::StringConverter::toString(mDefaultSamples);
        Ogre::Window *window = mRoot->createRenderWindow(name, width, height, false, &params);
        noteDevice();   // the device exists from here (Engine::deviceInfo / rayQueryAvailable)
        // THE PRESENTED FRAME CAN BE READ BACK (CLOSE-SHOT-2, View::requestFrameCapture):
        // Ogre's own switch (OgreWindow.h) — TRANSFER_SRC on the swapchain images (on
        // Metal, a layer that is not framebufferOnly). Set before setVSync so the
        // one swapchain rebuild it costs happens here, at creation, never mid-session.
        window->setWantsToDownload(true);
        window->setVSync(mVsync, 1u | kLowestLatencyVSync);
        ensureHlms();
        mViews.emplace_back(new OgreView(mRoot, window, nullptr, name, width, height,
                                         background, mLastError));
        syncHostViews();
        OgreView *view = mViews.back().get();
        view->mEngine = this;
        view->mRequestedSamples = mDefaultSamples;
#ifdef __APPLE__
        // No mCreateWindow on macOS (D2): VulkanMetalWindow implements
        // requestResolution/setFsaa by resizing its layer and rebuilding its own
        // swapchain (depth buffer included), so the window never has to be
        // recreated. OgreView::applyPendingResize takes that path when the
        // recreate hook is absent.
        return view;
#else
        const bool vulkan = mBackendName.find("Vulkan") != std::string::npos;
        void *display = mDisplay;
        Ogre::Root *root = mRoot;
        // `this` is safe to capture: the hook lives on a View, and every View
        // is owned by (and destroyed with) the Engine.
        view->mCreateWindow = [this, root, vulkan, display, handle, name](unsigned w, unsigned h,
                                                                          unsigned samples) -> Ogre::Window * {
            Ogre::NameValuePairList p;
            X11Handle x11{ display, (unsigned long)handle };
            if (vulkan) p["SDL2x11"] = Ogre::StringConverter::toString((unsigned long)&x11);
            else { p["parentWindowHandle"] = Ogre::StringConverter::toString((unsigned long)handle); p["gamma"] = "true"; }
            // The host's CURRENT pacing choice, not the one it started with:
            // a window rebuilt for an MSAA change must not silently switch
            // vsync back on (the same reasoning as the MAILBOX request below).
            p["vsync"] = mVsync ? "true" : "false"; p["vsyncInterval"] = "1";
            // Same MAILBOX request as the first window above — this hook is the
            // MSAA-change recreate path, and a window rebuilt without it would
            // silently drop back to FIFO for the rest of the session.
            p["vsync_method"] = "Lowest Latency";
            p["FSAA"] = Ogre::StringConverter::toString(samples);
            Ogre::Window *win = root->createRenderWindow(name + "/" + processUniqueName("resize"), w, h, false, &p);
            win->setWantsToDownload(true);   // the same capture switch as the first window
            win->setVSync(mVsync, 1u | kLowestLatencyVSync);
            return win;
        };
        return view;
#endif  // __APPLE__
    } JAH_CATCH(mLastError, nullptr);
#endif  // !__linux__ && !__APPLE__
}

View *OgreEngine::createOffscreenView(const std::string &name, unsigned width, unsigned height,
                                      const Colour &background, OffscreenContract contract) {
    // THE HOST'S CREATE IS A MESSAGE (U3 stage 4a, decision 39 b): the view is the host's handle at once (its id, name,
    // size); its target and chain are made at the drain (adoptOffscreenView), before the view's own record part.
    if (hostPosts()) {
        if (!width || !height) { noteHostError("createOffscreenView: zero size"); return nullptr; }
        if (mHeadless) {
            noteHostError("createOffscreenView('" + name + "'): this engine is headless (NULL render system) — "
                          "offscreen views would produce no pixels");
            return nullptr;
        }
        if (!mHostViewNames.insert(name).second) { noteHostError("View '" + name + "' already exists"); return nullptr; }
        auto holder = std::make_shared<std::unique_ptr<OgreView>>(
            new OgreView(OgreView::HostShell{}, mRoot, name, width, height, background, mLastError));
        OgreView *raw = holder->get();
        raw->mEngine = this;
        // THE CONTRACT BEFORE THE ADOPTION (PHOTON-7 stage 10-3): the view is the render thread's from the message
        // below, and no frame may see it without its contract (a setter's value would arrive as a later frame field).
        raw->initOffscreenContract(contract);
        postedAsMessage([holder](OgreEngine &e) { return e.adoptOffscreenView(std::move(*holder)); });
        return raw;
    }
    return createOffscreen(name, width, height, background, 1u, contract);
}

bool OgreEngine::adoptOffscreenView(std::unique_ptr<OgreView> view) {
    if (!view) return false;
    JAH_TRY {
        if (!mHlmsRegistered && !mNullWindow) {
            Ogre::NameValuePairList wp; wp["windowType"] = "null";
            mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"), 8, 8, false, &wp);
            noteDevice();
        }
        ensureHlms();
        Ogre::TextureGpu *rtt = OgreView::createRtt(mRoot, processUniqueName("rtt"), view->width(), view->height(), 1u, 1u);
        view->adoptTarget(rtt);
        mViews.emplace_back(std::move(view));
        syncHostViews();
        return true;
    } JAH_CATCH(mLastError, false);
}

OgreView *OgreEngine::createStereoPairView(const std::string &name, unsigned eyeW,
                                           unsigned eyeH, const Colour &background) {
    return createOffscreen(name, eyeW * 2u, eyeH, background, 2u, OffscreenContract::Live);   // (a stereo pair presents)
}

/// `width` is the view's (for a layered pair, the PAIR's); `layers` > 1 makes the
/// target a Type2DArray of width / layers per layer (OgreView::createRtt).
OgreView *OgreEngine::createOffscreen(const std::string &name, unsigned width, unsigned height,
                                      const Colour &background, unsigned layers, OffscreenContract contract) {
    if (viewNameTaken(name)) return nullptr;
    if (!width || !height) { mLastError = "createOffscreenView: zero size"; return nullptr; }
    if (mHeadless) {
        // REFUSED, deliberately (Types.h EngineConfig::headless). The NULL
        // render system would hand back a TextureGpu whose "contents" are
        // whatever malloc returned, and every readPixels caller in this tree
        // asserts on colours. A clean no with a reason beats a View that lies:
        // suites that need pixels run on the rendering boot, which is why
        // exactly none of them are headless.
        mLastError = "createOffscreenView('" + name + "'): this engine is headless (NULL render "
                     "system) — offscreen views would produce no pixels";
        return nullptr;
    }
    JAH_TRY {
        // Ogre requires a Window before Hlms/SceneManager exist. A purely offscreen
        // engine satisfies it with a surfaceless "null" window, kept for the
        // engine's lifetime (needs Ogre built with OGRE_VULKAN_WINDOW_NULL).
        if (!mHlmsRegistered && !mNullWindow) {
            Ogre::NameValuePairList wp; wp["windowType"] = "null";
            mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"),
                                                    8, 8, false, &wp);
            noteDevice();   // the device exists from here (Engine::deviceInfo / rayQueryAvailable)
        }
        ensureHlms();   // retried on every call until it succeeds (e.g. bad media dir)
        Ogre::TextureGpu *rtt =
            OgreView::createRtt(mRoot, processUniqueName("rtt"), width, height, 1u, layers);
        mViews.emplace_back(new OgreView(mRoot, nullptr, rtt, name, width, height,
                                         background, mLastError));
        syncHostViews();
        mViews.back()->mEngine = this;
        // (the engine's own thread: no frame runs between these lines; the contract's graph shape, applied at once)
        mViews.back()->setOffscreenContract(contract);
        return mViews.back().get();
    } JAH_CATCH(mLastError, nullptr);
}

void OgreEngine::destroyView(View *view) {
    // THE SECOND JOIN (U3 stage 4b): the surface goes on the render thread before Qt destroys its window.
    if (hostPosts()) {
        mRenderThread.runOn([&] { EngineSideScope engineSide; destroyView(view); moveErrorToHost(); });
        return;
    }
    EngineSideScope engineSide;   // a join: the engine's own calls inside it are the render side's (U3 4a)
    // A JOIN'S FIRST STEP (U3 stage 4a): the engine's own stream lands — a view the host made and destroys before
    // its first frame exists to be destroyed.
    applyPendingEngineMessages();
    if (view) mHostViewNames.erase(view->name());
    if (!view) return;
    // ...and the same for a view's part (the 3a read, C).
    mRenderThread.withPending([&](FrameInput &p, bool) {
        auto &v = p.views;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const FrameInputView &q) { return q.view == view; }), v.end());
    });
    // THE MIRROR'S VIEW CAN DIE WHILE A SESSION RUNS (a page closing, a window
    // rebuilt): the session holds a raw OgreView* and a workspace on that
    // view's target, so it is told before anything is freed.
    if (view == mVrMirrorView) setVrMirrorView(nullptr);
    for (auto it = mViews.begin(); it != mViews.end(); ++it) {
        if (it->get() != view) continue;
        // THE SHADOW-PASS COUNTER RIDES A VIEW (SHADOW_TOOLING_SPEC.md §4.3),
        // and this is where that view can die. Unhook it here or the next
        // frame's applyShadowCache dereferences a freed OgreView to
        // detach a listener from it — found by ASan on test_engine_asan's
        // shadow_resolution_rebuilds_the_atlas, which destroys views while the
        // counter is attached.
        noteViewDestroyed(it->get());
        (*it)->destroy();
        mViews.erase(it);
        syncHostViews();
        return;
    }
    mLastError = "destroyView: unknown View";
}

void OgreEngine::scenesFeedingEnabledViews(std::vector<OgreScene *> &out) const {
    // THE GATING PREDICATE (THREADING_ADOPTION_SPEC.md P3). One definition, in
    // one place: a scene takes part in a frame iff some ENABLED View draws it.
    // renderOneFrame used to compute this twice inline (the refraction
    // interlock and the HUD owner); both now read this set instead.
    //
    // Deliberately built from mScenes rather than from the views, so the answer
    // is always a subset of the scenes THIS engine owns and is in a stable
    // order. Both vectors are single digits — the nesting costs nothing.
    out.clear();
    for (const auto &s : mScenes) {
        for (const auto &v : mViews) {
            // THE ENGINE'S BINDING, never View::scene() — the host's record, which the host writes at its call while a
            // frame publishes (the clang TSan arm's race, decision 65's rerun).
            if (v && v->isEnabled() && v->boundOgreScene() == s.get()) { out.push_back(s.get()); break; }
        }
    }
}

namespace {
/// Defined below, beside drainTextureStreaming (its other caller). Declared here
/// so the frame head can name the textures a frame waited on.
size_t countPendingTextures(Ogre::TextureGpuManager *tm, std::string *namesOut);
}   // namespace

namespace {
/// ONE STEP OF A FRAME'S CLOSE (closeRenderFrame, lane FRAME-CATCH-1). The
/// close runs from a destructor, so nothing in it may throw out; and the steps
/// are independent, so a step that fails must not cost the ones after it.
/// Whatever it caught lands in the engine's `lastError`, which is where a host
/// looks anyway. `catch (...)` as well as the two JAH_CATCH kinds: this is the
/// one place in the engine where letting something through is undefined
/// behaviour rather than a bad error message.
///
/// APPENDED, NEVER REPLACED (lead review at merge): the frame's own cause — a
/// real VK_ERROR_DEVICE_LOST text — was written by the catch before this ran,
/// and a close step that also throws (xrEndFrame on a dying session) must not
/// overwrite it. And the message is built inside its own catch-all: this runs
/// from a noexcept destructor, where a bad_alloc raised while composing the
/// string would be std::terminate.
template <class Step>
void frameCloseStep(std::string &sink, Step &&step) {
    auto note = [&sink](const std::string &what) {
        try {
            if (!sink.empty()) sink += "; ";
            sink += what;
        } catch (...) {}
    };
    try { step(); }
    catch (Ogre::Exception &e) { try { note(::jahshaka::engine::detail::describeOgreFailure(e)); } catch (...) {} }
    catch (std::exception &e)  { try { note(std::string("engine: ") + e.what()); } catch (...) {} }
    catch (...)                { note("engine: an unknown exception closing the frame"); }
}
}   // namespace


namespace { std::atomic<int> gAsyncViews{0}; }
int asyncViewCount() { return gAsyncViews.load(std::memory_order_relaxed); }
void noteAsyncView(bool on) { gAsyncViews.fetch_add(on ? 1 : -1, std::memory_order_relaxed); }

// ONE TURN OF THE RENDER LOOP (ENGINE-UPGRADE-U3 stage 1, the seam): the hand-over, the frame, the publish — all
// RenderThread's (RenderThread.cpp), on the caller's thread until stage 4 gives the loop its own.
// THE LOCKSTEP VERB (U3 stage 4b): the frame runs on the render thread and the caller waits for it — "serial >= k
// presented": the record it submitted is the one the frame applies. Inline where there is no thread (a fixture).
namespace {
/// THE SPLIT'S PUBLISH (decision 65): Ogre's frameRenderingQueued — every render target recorded and submitted, the
/// swap next (OgreRoot.cpp:1575-1584) — is where a free frame publishes.
class SplitPublishListener final : public Ogre::FrameListener {
public:
    explicit SplitPublishListener(std::function<void()> at) : mAt(std::move(at)) {}
    bool frameRenderingQueued(const Ogre::FrameEvent &) override { mAt(); return true; }
private:
    std::function<void()> mAt;
};
}   // namespace

void OgreEngine::renderOneFrame() { mRenderThread.runOn([this] { mRenderThread.tickInline(); }); }

// THE FRAME'S BODY (ENGINE-UPGRADE-U3 F7): the loop hands it the record the hand-over took.
bool OgreEngine::runFrame(FrameInput &record, bool any) {
    // LEGAL AND EMPTY WHEN HEADLESS (Types.h EngineConfig::headless): a
    // headless engine can hold no View, so every loop below iterates nothing
    // and Root::renderOneFrame walks a workspace-less render system. Hosts do
    // not have to special-case their frame loop; it simply costs nothing.
    //
    // "WE ARE IN A FRAME", RAII (VR-4-FIX's second read, finding 3): what
    // destroyScene's VR belt asks before it ends a session. RAII rather than a
    // pair of assignments because this function throws (JAH_CATCH below) and a
    // flag left set would refuse every later teardown.
    //
    // ...AND IT IS THE WHOLE CLOSE OF THE FRAME NOW, NOT ONLY THE FLAG (lane
    // FRAME-CATCH-1, 2026-09-18). `JAH_CATCH` RETURNS — that is what makes it
    // usable on a hundred bool-returning boundary calls — so everything this
    // function used to do AFTER the catch was unreachable on a frame that
    // threw, comments and all: the runtime's xrEndFrame, the monitor's close,
    // the lost/stopped session's end and the device-lost latch. `closeRenderFrame`
    // holds all of it and the destructor is what runs it, so it runs on every
    // exit — the normal one, the VR pump's early return, and a throw.
    // A FATAL OOM ENDED THE FRAMES (FORK-OOM-1, GpuFault::OutOfMemoryInFrame):
    // Ogre's frame state is not whole after an OOM half-way through a frame, and
    // the next frame into it crashes (measured: a null deref in the render-pass
    // setup). Nothing is touched from here on; the host ends the process. This is
    // TERMINAL until the evict/retry lane (after D) makes a mid-frame OOM
    // recoverable.
    if (mGpuFault == GpuFault::OutOfMemoryInFrame) return false;
    struct FrameScope {
        OgreEngine *self;
        explicit FrameScope(OgreEngine *s) : self(s) { self->mInRenderFrame = true; }
        ~FrameScope() { self->closeRenderFrame(); }
    } frameScope(this);
    // THE FRAME'S WORK WINDOW OPENS (FORK-OOM-1): the render system's OOM count now.
    mOomAtFrameStart = vulkanOomCount();
    mFrameWorkEnded = false;
    mFrameThrewOom = false;
    JAH_TRY {
        // THE ONE DRAIN (U0 3a): the frame's input lands before anything of the frame reads it — the cause the
        // monitor's record opens with just below, the pace each scene is handed, every scene's environment and
        // every view's camera. BEFORE the VR frame (the 3a read, A): the session notes its own cause (WarmUp) and
        // writes the head pose onto the session view, and both are the engine's — a host's record (the driver's
        // Driver cause; the eyes' part carries no pose) must land under them, never over them.
        // THE PRESENTATION'S FRAME OPENS (U3 stage 5; decision 56): inside a FREE frame's drain the node records leave
        // the write to the interpolation, which each scene runs right after its records (applySceneStream).
        ++mPresentFrame;
        mPresentBegun = false;
        mDeferNodeWrites = mFreeFrame;
        struct DeferEnds { bool &f; ~DeferEnds() { f = false; } } deferEnds{ mDeferNodeWrites };
        {
            const bool budgeted = mFreeFrame && mRenderThread.inFreeFrame();   // a free frame's drain is budgeted
            drainFrameInput(record, any, budgeted);
            if (budgeted) mLastFreeDrainMs = mFrameInputStatus.drainMicros / 1000.0;
        }
        mDeferNodeWrites = false;
        // THE SHADER CACHE'S CHUNKED COPY CONTINUES (FRAME-MONITOR-DOOR-1 stage 4): one step a frame, here at the drain,
        // when the frame's own messages ran none (a save message is a step) — a copy of N new entries costs
        // ceil(N / kCopyChunkEntries) frames of <= 1 ms each, never one frame of all of it.
        if (!mShaderCache.takeStepMark() && mShaderCache.copyInProgress()) {
            mShaderCache.save(mRoot);
            mShaderCache.takeStepMark();
        }
        // ...and every scene the record did not reach is drawn between its two grid states too (a lockstep frame: cur).
        interpolateFrame();
        // ---- THE VR FRAME OPENS HERE (SPECS/VR_SPEC.md §4.3) --------------
        // While a session runs this call IS the frame's clock: it polls the
        // runtime's lifecycle events, blocks in xrWaitFrame until the runtime
        // wants the next picture, locates the eyes and writes the head pose and
        // the two per-eye projections onto the session View's camera. The
        // host's timer is at a zero interval for the duration, so nothing else
        // paces this loop.
        //
        // When the runtime asks for NO picture this frame (it is not visible,
        // or tracking is not valid yet) the pump has already given it its
        // empty frame and the desktop draws anyway (F4) — there is no early
        // return here any more, and there was no path taking one: the bool
        // the pump used to answer was dead (seven `return true`s), deleted at
        // the VR-ENGINE-2 merge. A NO-OP on every engine without a session,
        // which is every engine outside a headset.
        if (mVrSession) {
            if (!mRenderThread.onRenderThread()) ++mVrFramesOffRenderThread;   // the XR-thread law (stage 6's spike)
            vrSessionBeginFrame(mVrSession);
        }
        mFrameWaitedOnRuntime = (mVrSession && vrSessionWaitedThisFrame(mVrSession)) || mFakeRuntimeWaited;
        mFakeRuntimeWaited = false;
        // THE SPLIT'S PUBLISH (decision 65): the free frame publishes at its record/swap split, before a blocking present;
        // the listener is attached once, with Root.
        if (!mSplitPublishAttached && mRoot) {
            if (!mSplitPublish) mSplitPublish.reset(new SplitPublishListener([this] { onRecordSplit(); }));
            mRoot->addFrameListener(mSplitPublish.get());
            mSplitPublishAttached = true;
        }
        // THE RENDER-LOOP MONITOR'S FRAME (RENDER_LOOP_MONITOR_SPEC §4.2).
        // Opened here and closed at the very bottom, so `totalMs` is exactly
        // what one renderOneFrame cost. `mNextFrameCause` is the caller's — the
        // driver tick, a scripted editor.frame, an offscreen readback, the
        // warm-up gate — and it is CONSUMED here: a caller that does not set it
        // gets Driver, which is what the loop is.
        if (monitor::live()) {
            bool onscreen = false;
            for (auto &v : mViews)
                if (v->isEnabled() && !v->isOffscreen()) { onscreen = true; break; }
            // THE RECORD'S FRAME IS THE FRAME THIS CALL PUBLISHES (PublishedFrame::frame: the publish after the close
            // counts it), the stamp every reader joins the record to - never the shadow cache's counter, which a
            // headless engine (no Hlms) never ticks (PHOTON-7 stage 0, AB G3).
            monitor::gMonitor->beginFrame(mPublishedFrames + 1ull, mNextFrameCause, onscreen);
            // The record this frame applied: the host's half of the monitor's record rode the same hand-over
            // (RenderThread::tickInline folded it in before this frame opened), so the two halves are joined by it.
            monitor::gMonitor->current().serial = mRenderThread.serial();
            monitor::gMonitor->noteInput(mFrameInputStatus.changesMoved, mFrameInputStatus.gestureActive,
                                         mFrameInputStatus.fieldsApplied);
        }
        mNextFrameCause = FrameCause::Driver;
        // THE MEASUREMENT ARMS (lane TEST-1) take what was set since the last frame:
        // one value of each for the whole of this one.
        mArms.latch();
        // THE MISFIT COUNTER'S NEGATIVE CONTROL (stage 5's read W5; the arm shader.forceParamsMisfit): each time the arm
        // comes on, the fork refuses the next compute dispatch as a misfit (HlmsCompute::_jahForceParamsMisfits)
        {
            const bool on = mArms.on(ArmId::ShaderForceParamsMisfit);
            if (on && !mMisfitDoorArmed)
                if (Ogre::HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms()) hc->_jahForceParamsMisfits(1u);
            mMisfitDoorArmed = on;
        }
        // WHAT THIS FRAME MAY PUT OFF (OPEN_COVER_SPEC §2.1), consumed exactly
        // like the cause and reset to `Complete` — so a caller that sets
        // nothing renders the frame it always did. Pushed to every scene here,
        // once, because the decisions it changes are per scene (the GI arm's
        // staged build) and per frame (the texture drain below).
        const FramePace pace = mFramePace;
        for (auto &sc : mScenes) sc->setFramePace(pace);
        std::unique_ptr<monitor::Stage> monPre;
        if (monitor::live()) {
            monPre.reset(new monitor::Stage("engine.pre"));
            // BEFORE anything renders and outside every encoder — the only
            // place a Vulkan query pool may be reset (fork 1a81f866a+1bccc3f93 (was 0027)).
            // Only on a capture's FIRST frame: every later turnover ran at the
            // close of the frame before (closeRenderFrame), which opened this
            // frame's own GPU pair (lane TEST-1, F5).
            if (!monitor::gMonitor->mTurnedOver) gpuFrameBegin();
            monitor::gMonitor->mTurnedOver = false;
        }
        // THE ONE TEXTURE WAIT (THREADING_ADOPTION_SPEC.md P2 item 3, decision
        // D-C(1)). `loadTexture` no longer waits per texture; it schedules, and
        // this is where the frame collects. It is at the very TOP of the frame,
        // before _fireFrameStarted and before the per-view pending work below,
        // because that work is not all "drawing": applyPendingIbl and
        // applyPendingGi READ texture contents (an IBL cubemap, the VCT
        // voxelizer's albedo reads), and giving them a texture that has not
        // finished streaming would be a wrong picture rather than a slow one.
        //
        // A NO-OP WHEN NOTHING IS PENDING — isDoneStreaming is a flag and a
        // queue size behind a mutex, so an idle editor pays a few nanoseconds a
        // frame. When something IS pending, this blocks exactly as long as the
        // old per-texture waits did in TOTAL, minus everything the decodes
        // managed to overlap, which is the entire point of the phase.
        //
        // EVERY RENDER PATH FUNNELS THROUGH HERE: the driver tick, thumbnails,
        // asset scenes, the warm-up gate and the selftest all call
        // renderOneFrame. That is what makes one call enough.
        //
        // AND IT IS BOUNDED (defect 2026-09-08). It used to be
        // `waitForStreamingCompletion()`, whose loop can never end if a load
        // request cannot complete; drainTextureStreaming is the same drain with
        // a no-progress deadline and a diagnostic. See its definition.
        // ...AND A STREAMING FRAME DOES NOT WAIT FOR IT (OPEN_COVER_SPEC §2 E).
        // The drain is 25-61 ms per frame while a world's textures arrive, and
        // on a DRIVER frame of a world the user is already looking at the honest
        // answer is to draw the fallback and let `settleTextureResidency` swap
        // the real one in a frame or two later — which is the streaming the
        // owner asked for. Every other frame keeps the wait, so suites,
        // thumbnails, captures and scripted frames stay deterministic.
        //
        // IT IS NOT THE ARM'S WAIT. BOOTVOX-1's `giVoxelTexturesPending` is a
        // different question asked in a different place (OgreGi.cpp), and it
        // still holds the arm back until the albedo it voxelises is resident —
        // skipping the drain makes that wait last a frame or two longer, never
        // less careful.
        // ...AND ONLY WHEN NOTHING DEFERRED IS ABOUT TO READ A TEXTURE'S PIXELS.
        // `applyPendingIbl` convolves the sky's source cube and is a LATCH — it
        // clears its flag on entry — so handing it a cube that has not streamed
        // in is a wrong reflection for the life of that sky, not a slow frame.
        // The GI arm asks the same question for itself (BOOTVOX-1) and is safe
        // either way.
        bool streamTextures = (pace == FramePace::Streaming);
        if (streamTextures)
            for (auto &sc : mScenes)
                if (sc->iblReadPending()) { streamTextures = false; break; }
        if (streamTextures) {
            // COLLECT WITHOUT WAITING: one pass of the same `_update(true)` the
            // drain's loop makes, so every texture that HAS landed becomes
            // resident this frame and nothing blocks on the ones that have not.
            monitor::Stage st("engine.texturestep");
            collectTextureStreaming();
        } else if (monitor::live()) {
            monitor::Stage st("engine.texturewait");
            double ms = 0.0;
            // The pending set BEFORE the drain and after it: what this frame
            // actually waited for, by name. Ogre offers no per-texture "loaded"
            // callback, so the frame-head drain is where a texture load becomes
            // visible at all.
            std::string names;
            Ogre::TextureGpuManager *tm =
                mRoot && mRoot->getRenderSystem() ? mRoot->getRenderSystem()->getTextureGpuManager()
                                                  : nullptr;
            const size_t before = tm ? countPendingTextures(tm, &names) : 0u;
            drainTextureStreaming(&ms);
            monitor::noteTextureWait(float(ms));
            const size_t after = (before && tm) ? countPendingTextures(tm, nullptr) : 0u;
            // ONLY WHEN SOMETHING ACTUALLY HAPPENED (lane MON-P1b's finding,
            // fixed by the lead 2026-09-13). `before` alone is "a texture is
            // pending", which is TRUE EVERY FRAME for a manual texture that
            // never becomes resident: the VCT voxelizer's AccumVal sits at
            // residency OnStorage for the life of the scene, so a 10 s capture
            // logged 626 identical texture.load events — 99% of its event log —
            // none of which recorded a load. The event means a load COMPLETED
            // (or that this frame really waited), never "something is queued".
            if (before > after || ms > 0.0) {
                monitor::noteEvent(MonitorEventKind::TextureLoad, WorkReason::Request,
                                   "texture.load", names, float(ms),
                                   (unsigned long long)(before > after ? before - after : 0u));
                monitor::noteCacheWork(CacheKind::Texture, WorkReason::Request, 0, names.c_str(),
                                       unsigned(before > after ? before - after : 0u), float(ms));
            }
        } else {
            drainTextureStreaming();
        }
        // A TEXTURE THAT ARRIVED IS A PROBE INPUT (clean-2 lane, 2026-09-13).
        // The drain above is where a load becomes visible at all — Ogre has no
        // per-texture completion callback — so it is also the only place the
        // renderer can notice that a material the probes captured WITHOUT its
        // texture now has one. Free on a scene with nothing outstanding.
        for (auto &sc : mScenes) sc->settleTextureResidency();
        // HOW MANY SHADOW MAPS THIS FRAME NEEDS (SHADOW_TOOLING_SPEC.md §4.1).
        // At the top of the frame, before any per-view work, because growing
        // the atlas drops and recreates every workspace that names the shadow
        // node — the same operation a Shadow Quality change performs, and the
        // same place it is safe. Debounced and growth-only, so a steady scene
        // pays one light-list walk per frame and nothing else.
        deriveShadowMapCount();
        // THE LAMP-MAP CACHE, first half (ENGINE_CACHE_POLICY_SPEC P2): the
        // clear strategy and the pass counters. After the derivation, because a
        // rebuild replaces the very CompositorShadowNode instances the cache
        // lives on. The DETECTION half runs after updateSceneGraph below.
        applyShadowCache();
        // ONE AUTHORITATIVE VIEW PER SCENE (FIX WAVE B2 / finding F7). The GI
        // tracker's work is per SCENE and stateful — it spends a per-frame probe
        // budget and carries the Forward+ range hysteresis — while `mViews` can
        // hold several views of the SAME scene (the editor and the player share
        // one; a preview dock is another). Running it once per view spent the
        // budget as many times as there were views and let the last camera in
        // the list decide the probe priority. The rule matches the post chain's
        // "primary on-screen view owns the globals": the first ENABLED on-screen
        // view of a scene wins, and an all-offscreen scene falls back to its
        // first enabled view so headless suites still track.
        std::vector<std::pair<OgreScene *, OgreView *>> giDriver;
        giDriver.reserve(mViews.size());
        const auto driverSlot = [&giDriver](OgreScene *s) -> OgreView ** {
            for (auto &kv : giDriver)
                if (kv.first == s) return &kv.second;
            giDriver.emplace_back(s, nullptr);
            return &giDriver.back().second;
        };
        // PASS -1 IS THE VR SESSION'S (VR_SPEC §3.4 / §7 item 6). The session's
        // View is OFFSCREEN — its target is the both-eyes RTT — and it is
        // created last, so under the creation-order rule below the editor's
        // desktop view would place the cascades and the headset would look at a
        // field centred on somebody else's camera. A view that declares itself
        // the GI driver wins outright; nothing but a VR session ever does.
        //
        // AND IT WINS WHETHER OR NOT IT IS ENABLED THIS FRAME (lane V1-RIG fix
        // round item 1, the Fable read). The session switches its own View OFF
        // on every frame the runtime asks for no picture and on every frame with
        // no valid pose (`VrSession::beginFrame`'s two `setSessionViewEnabled(false)`
        // paths) — which is what keeps the DESKTOP drawing through a doff, an
        // open dashboard or WiVRn's first frames. With `isEnabled()` tested
        // before the priority test, the driver fell to the desktop view on each
        // of those frames and came back on the next, and each flip was seen by
        // `updateGiTracking` as a change of driver PROFILE: `mGiChainShapeDirty`,
        // then `rebuildVct()` — a teardown, every cascade and the whole field,
        // TWICE per doff, with the chain's centre jumping to the editor's camera
        // in between. Every WiVRn session start did it, because its first frames
        // are no-picture ones.
        //
        // So the GI-priority pass reads only `giPriority()`. The slot stays the
        // session's View and the DESKTOP NEVER TAKES THE CHAIN OVER while a
        // session exists; the `authoritative` test below still requires an
        // enabled view, so on such a frame NOBODY drives GI and the chain simply
        // HOLDS STILL — the camera it was placed around stays the headset's last
        // located head, which is the answer that costs nothing and lies about
        // nothing. The profile follows the SESSION (it flips on
        // `setGiPriority(false)` and on the View's destruction, both of which
        // happen when the session ends), never the frame.
        for (int pass = -1; pass < 2; ++pass)        // -1: the GI driver, 0: on-screen, 1: any
            for (auto &v : mViews) {
                if (!v->ogreScene()) continue;
                if (pass == -1 && !v->giPriority()) continue;
                if (pass != -1 && !v->isEnabled()) continue;
                if (pass == 0 && v->isOffscreen()) continue;
                OgreView **slot = driverSlot(v->ogreScene());
                if (!*slot) *slot = v.get();
            }
        // THE GI STEP RUNS AFTER THE SCENE GRAPH (U4 stage 3, decision 8: `engine.plan`, below, where the world boxes
        // are the frame's own); here only its camera and the driver's profile, which the pending flush may owe.
        std::vector<OgreView *> giDrivers;
        for (auto &v : mViews) {
            const bool authoritative = v->isEnabled() && v->ogreScene() &&
                                       *driverSlot(v->ogreScene()) == v.get();
            v->applyPendingResize(); v->updateParticles();
            v->syncLodTolerance();   // the view's pixel-error threshold on its cameras (ATOM-3 stage 1)
            if (authoritative) { v->noteGiCamera(); giDrivers.push_back(v.get()); }
            // THE ATMOSPHERE'S OBSERVER is the same driving camera (SKY-ATMOSPHERE-1):
            // its altitude decides the horizon's dip and the air it looks through.
            if (authoritative && v->camera())
                v->ogreScene()->noteAtmosphereObserver(float(v->camera()->getDerivedPosition().y));
            // Both ends of the planar-reflection wiring move between frames (the
            // scene rebuilds its arm on a parameter change, the view recreates
            // its camera on setScene), so the listener is re-synced rather than
            // hooked up once. Idempotent and cheap when nothing changed.
            v->syncPlanarListener();
            // The post chain's PER-VIEW tuning (CAMERA_LENS_SPEC §4). Same
            // shape and same reason as the planar listener above: both ends
            // move between frames (the chain rebuilds on every enable-flag
            // change, the camera is recreated on setScene), so the listener is
            // re-synced rather than hooked up once.
            v->syncGlobalsListener();
            // ...and the no-ray route's screen integrate (PHOTON-7 7-6), on the chain's shape
            v->syncScreenGiListener();
            // ...and the reflection trace's hook (PHOTON_SPEC §7 R5), for the
            // third time and the third reason: the chain rebuilds when the SSR
            // row or the machine's answer changes, the scene's voxel arm is
            // rebuilt behind the trace's back, and the camera is recreated on
            // setScene.
            v->syncReflectListener();
            // ...and the visibility buffer's (ATOM S3-DRAW): ChainDesc::atomDraw
            // reads the scene's split, which the view learns only once it has one.
            v->syncAtomDraw();
            // ...and THE PHOTON VIEW's (PHOTON-VIEW-1): the listener that gates its
            // passes, the ray tier's overlay while the scene's view needs one.
            v->syncPhotonView();
            // The inset's rectangles are derived from the TARGET's aspect
            // (a normalised rect is not a pixel rect), so a resize that never
            // touched ViewPipDesc still moves the letterbox. Re-derived here,
            // once a frame, right after applyPendingResize; free when there is
            // no inset.
            v->applyLetterboxAndPip();
        }
        // THE MIRROR, AFTER THE RESIZES (phase 3, a measured crash). The loop
        // above may have rebuilt a window's swapchain in place — which moves no
        // workspace generation, because nothing was detached — and the VR
        // session's mirror is a workspace of its own over that very target,
        // decided at the top of this frame and about to execute. Re-checking it
        // here is what stops it running against a target that has changed
        // shape: an "attachment is not a depth format" exception at best, a
        // segfault inside CompositorWorkspace::_update at worst (1 run in 4 of
        // the Player's VR suite, where the page is shown a moment before the
        // session begins and the resize lands in its first frames). A handful
        // of integer compares when nothing moved, and nothing at all with no
        // session.
        if (mVrSession) vrSessionSyncMirror(mVrSession);
        // THE SCENES THIS FRAME BELONGS TO (THREADING_ADOPTION_SPEC.md P3).
        // Computed ONCE, here, and read four times below: by the deferred GI /
        // IBL / planar work on the next line, by the refraction interlock, by
        // the frame loop's update/clear passes, and by the engineObjects
        // census. Nothing between this line and the frame can change a View's
        // enabled flag, so one snapshot is honest.
        std::vector<OgreScene *> updated;
        scenesFeedingEnabledViews(updated);
        // ...AND THE BLANK WORLD, when a scene-less View is drawing its
        // clear-only workspace this frame (lane STALE-VIEW-1). This is the
        // "a future feature that creates a workspace not owned by a View has to
        // extend scenesFeedingEnabledViews" case, answered in its own variable
        // because the blank manager is not an OgreScene and takes part in
        // nothing else: it is updated and cleared with the others, below, and
        // that is all the rule asks.
        Ogre::SceneManager *blankUpdated = nullptr;
        for (auto &v : mViews)
            if (v->drawsBlank()) { blankUpdated = blankSceneManager(); break; }
        const auto drawnThisFrame = [&updated](const OgreScene *s) {
            return std::find(updated.begin(), updated.end(), s) != updated.end();
        };
        // GATED ON "IS ANYONE LOOKING AT IT" (audit F5, lane PLAYER-1). These
        // three are the expensive deferred rebuilds — a from-scratch
        // voxelisation is measured in seconds — and they used to run for EVERY
        // scene the engine owns, drawn or not. A scene with no enabled view
        // cannot present the result, and its pendings are latches: they are
        // still set when a view of it is enabled again, so nothing is skipped,
        // only postponed to the frame it can be seen in.
        // (THE SCENES' PENDING WORK — the sky's read, the clouds' clock, the GI / IBL / planar flushes, the cards' plan,
        // the Photon view — and the post chain's recompile globals run after the GI step, in `engine.plan` below, in
        // the order they always ran after it: U4 stage 3 moved the step after the scene graph.)
        // (THE PROBE CACHE'S LATCH runs after the GI step, in `engine.plan` below.)
        // The refraction interlock (OgreScene::setRefractionsActive). A
        // Refractive datablock drawn by a pass that offers it no refractions
        // fails to COMPILE and loses the whole frame, and one scene can be drawn
        // by views with different chains — the editor viewport, the player, and
        // the throwaway offscreen view a screenshot renders through. So a scene
        // only keeps its refractive materials refractive while EVERY view that
        // draws it has the pass; otherwise they fall back to glass. Recomputed
        // per frame because views come and go; the setter is a no-op on repeat.
        for (auto &s : mScenes) {
            const bool anyView = drawnThisFrame(s.get());
            bool allHaveRefraction = true;
            if (anyView) {
                for (auto &v : mViews) {
                    if (v->boundOgreScene() != s.get() || !v->isEnabled()) continue;
                    if (!v->chainDesc().refractions) allHaveRefraction = false;
                }
            }
            s->setRefractionsActive(anyView && allHaveRefraction);
        }
        // THE HUD OWNER for this frame (STATS_OVERLAY_SPEC §7.8). Ogre's
        // overlay set is process-wide, so one view has to speak for it: the
        // first ENABLED view that is entitled to overlays (on-screen, or
        // offscreen with allowOffscreen) and whose desc asks for something.
        // Same shape as the post chain's globals rule above, and the same
        // reason — the state lives in an Ogre singleton, not per view.
        //
        // Nothing found -> hide(). That is what keeps every thumbnail, preview
        // and pixel suite byte-identical even before the per-pass gate: an
        // offscreen view is never the owner unless it opted in.
        // THE PRESENTED FRAME'S CAPTURE (CLOSE-SHOT-2): which views capture THIS
        // frame, decided once before anything of the frame reads it.
        for (auto &v : mViews) v->beginFrameCapture();
        {
            const OgreView *owner = nullptr;
            for (auto &v : mViews) {
                if (!v->isEnabled() || !v->overlaysAllowed() || !v->overlay().anything()) continue;
                owner = v.get();
                break;
            }
            if (owner) {
                RenderStats stats;
                if (owner->overlay().stats) mHudStatsThisFrame = true;   // the HUD reads the counting (U3 stage 3)
                renderStats(stats);
                // The shadow-atlas inspector's data (SHADOW_TOOLING_SPEC.md
                // §4.4). Collected HERE and not in the HUD because it lives
                // inside a compositor node, which the overlay code cannot and
                // should not reach; and collected only when the overlay asks,
                // because it touches a live workspace.
                hud::setAtlasTiles(owner->overlay().shadowAtlas ? collectAtlasTiles()
                                                                : std::vector<hud::AtlasTileDesc>());
                hud::apply(owner->overlay(), stats, owner->width(), owner->height());
            } else {
                hud::hide();
            }
        }
        // THE FRAME (THREADING_ADOPTION_SPEC.md P3 — "the explicit frame loop").
        //
        // This is `Root::renderOneFrame()`'s body (OgreRoot.cpp:1101-1126)
        // inlined verbatim, with ONE difference: upstream walks EVERY
        // SceneManager in the process, ours walks only the scenes an enabled
        // View draws. Two reasons, and the second is the important one:
        //
        //  1. COST. `updateSceneGraph` is not free on an idle scene — it fires
        //     barrier round-trips per pass (transforms, animations, bounds,
        //     light list) whether or not anything in that scene moved. The
        //     process holds the editor's scene, the player's, the asset page's,
        //     the material preview's, the avatar preview's, a thumbnail scene
        //     and BOTH staging managers; without this gate every one of them
        //     is updated 60 times a second so that at most one of them can be
        //     seen. The render half was always gated (`_updateAllRenderTargets`
        //     walks only enabled workspaces, OgreRoot.cpp:1575-1599, and
        //     View::setEnabled propagates to the workspace) — only the update
        //     half was not.
        //  2. CORRECTNESS. The document's STAGING scene managers are written by
        //     the import worker WHILE this loop runs. Upstream's unconditional
        //     walk had the render thread inside `updateAllTransforms` on the
        //     very SoA pools `ArrayMemoryManager::createNewSlot` frees when it
        //     grows them (OgreArrayMemoryManager.cpp:167-215) — a use-after-free,
        //     not merely a torn read. A staging manager never feeds a View, so
        //     after this gate the render thread never touches it at all. See
        //     the THREADING section of irisgl/document/scenegraph/nodegraph.h.
        //
        // THE RULE THIS MUST NOT BREAK: a scene whose workspace runs must have
        // been updated in the SAME frame. Every workspace in this engine belongs
        // to a View (OgreView owns it), and planar-reflection workspaces belong
        // to a scene some enabled View draws — so the two sets agree today. A
        // future feature that creates a workspace NOT owned by a View has to
        // extend `scenesFeedingEnabledViews` with it, or it will render against
        // a scene graph nobody updated.
        // WHAT COMPILED SINCE THE LAST FRAME, with the permutation. The count
        // comes from the shader cache's log counter (Ogre has no callback) and
        // the names from its bounded queue, drained here on the UI thread —
        // the compiles themselves may have happened on the scene's worker pool
        // (OGRE_SHADER_COMPILATION_THREADING_MODE=2).
        if (monitor::live()) {
            std::vector<std::string> names;
            const unsigned n = mShaderCache.drainCompileNames(names);
            if (n) {
                monitor::noteShaderCompiles(n);
                for (const std::string &nm : names)
                    monitor::noteCacheWork(CacheKind::Shader, WorkReason::Permutation, 0,
                                           nm.c_str(), 1u);
                monitor::noteEvent(MonitorEventKind::ShaderCompile, WorkReason::Permutation,
                                   "shader.compile",
                                   names.empty() ? std::string() : names.front(), -1.0f, n);
            }
        }
        // THE MONITOR'S LISTENERS, re-attached for THIS frame — here and not
        // earlier, for the same reason the shadow counters are re-attached
        // here: applyPendingGi / applyPendingPlanar may have rebuilt a probe or
        // a planar workspace since the top of the frame, and a listener list
        // dies with its workspace.
        syncMonitorListeners(updated);
        monPre.reset();                    // closes the "engine.pre" stage
        if (mRoot) {
            // `_fireFrameStarted()` can veto the frame (a lost device, or a
            // frame listener saying stop); upstream returns false there and
            // does nothing else, so neither do we.
            if (mRoot->_fireFrameStarted()) {
                const auto poolSpanBegin = std::chrono::steady_clock::now();   // (the frame's pool span, U4 stage 6)
                {
                    std::unique_ptr<monitor::Stage> st;
                    if (monitor::live()) st.reset(new monitor::Stage("engine.sceneGraph"));
                    for (OgreScene *s : updated) s->sceneManager()->updateSceneGraph();
                    if (blankUpdated) blankUpdated->updateSceneGraph();
                }
                // THE PLAN, WHERE THE BOXES ARE CURRENT (ENGINE-UPGRADE-U4 stage 3, decision 8). updateSceneGraph has
                // just made every drawn scene's world AABBs this frame's (Ogre's workers, SoA), so the GI scheduler's
                // plan (the geometry term) and the GI step (the walk, its boxes, the marks, the pick and the step's
                // work) read the cached boxes — no parent-chain recompute per item — and nothing has rendered yet.
                // Then the probe cache's latch (after the budget spent itself), the probe collection, and the listeners
                // the plan's refresh may have invalidated (a re-solve can rebuild an arm), re-synced as at the frame's
                // head. (Root's frame counter advances in _fireFrameRenderingQueued, not in frameStarted: inside this
                // block getNextFrameNumber() is still the frame's head — every GI writer stamps the frame it always did.)
                // THE PENDING FLUSHES RUN HERE TOO, after the scene graph (decision 14): a PCC placement's
                // updateAllDirtyProbes leaves the scene manager's light list as it found it (fork, ParallaxCorrected-
                // CubemapAuto) and a probe it places has its node and its bounds current at once (fork, CubemapProbe::
                // syncInternalProbe) — the frame that follows draws with its lamps and the probe where it was set.
                {
                    std::unique_ptr<monitor::Stage> planStage;
                    if (monitor::live()) planStage.reset(new monitor::Stage("engine.plan"));
                    // `boxesCurrent` for exactly this block, whatever throws inside it (decision 13.4): the next
                    // drain's and the requests' box reads must never read cached boxes outside a current graph.
                    struct BoxesCurrent {
                        const std::vector<OgreScene *> &s;
                        explicit BoxesCurrent(const std::vector<OgreScene *> &v) : s(v) { for (OgreScene *x : s) x->setBoxesCurrent(true); }
                        ~BoxesCurrent() { for (OgreScene *x : s) x->setBoxesCurrent(false); }
                    } boxesCurrent(updated);
                    for (auto &s : mScenes) s->runStashedGiPlan();
                    for (OgreView *v : giDrivers) v->updateGi();
                    // ...THEN WHAT ALWAYS FOLLOWED THE GI STEP, in its order: the scenes' pending work and the
                    // recompile globals (moved here with the step, U4 stage 3).
                    for (auto &s : mScenes)
                        if (drawnThisFrame(s.get())) {
                            // THE SKY AMBIENT'S DEFERRED READ (audit ON-14), first: at its DUE
                            // frame, a map and 6 x 1024 texels of integral - never polled for
                            // landing (OgreScene::skyReadDue: the frame it lands on is a
                            // function of the frames) - and here rather than beside the
                            // capture because a read is never due in the frame it was issued in.
                            s->pollSkyShRead();
                            // THE CLOUD LAYER'S FRAME (CLOUDS-2D-1): its clock, its scroll
                            // and its capture cadence — before the capture below it may ask for.
                            s->tickCloudClock();
                            s->applyPendingGiDeferrable(); s->applyPendingIbl(); s->applyPendingPlanar();
                            // THIS SCENE'S IBL CHAIN LENGTH (SceneGiBinding::iblMipmaps), after
                            // the pendings that can change what its env slots hold.
                            s->resolveIblMipmaps();
                            // SURFACE-CACHE, after the pendings: the cache reads the
                            // material generation and the light write serial that
                            // applyPendingGi may just have moved, and PLANS this frame's
                            // capture batch. The capture itself runs inside Root's frame
                            // (its workspace is enabled and first in the manager's list),
                            // after the frame's scene-graph update and light list — the
                            // light list a capture planned here used to run without.
                            // ITS OWN MONITOR STAGE (lane D1-SCALE-FIXTURES, W13): the
                            // candidate walk over every item runs here every frame, and
                            // scale.cpu_walks reads it as a number rather than an A/B.
                            monitor::Stage cardStage("engine.cards");
                            s->updateSurfaceCache();
                            // THE PHOTON VIEW's scene half (PHOTON-VIEW-1), after the cache
                            // planned its frame (the Cards picture reads its table) and the
                            // pendings rebuilt what they rebuild (the Voxels and Probes pictures
                            // follow the arms). Free when the view is Off and nothing is up.
                            s->syncPhotonView();
                        }
                    // THE RECOMPILE HALF ONLY (CAMERA_LENS_SPEC §4 split the old
                    // applyGlobals in two). The MSAA resolve weights and the SMAA preset
                    // are SHADER RELOADS — a hitch — so they cannot be per view without
                    // hitching on every camera cut and every page switch. They keep the
                    // rule POST_CHAIN_SPEC §7.4 wrote: "the primary on-screen view owns the
                    // globals", i.e. the first enabled view whose chain has effects, and
                    // each helper debounces on "did the value actually change".
                    //
                    // Everything CHEAP that used to ride along here — exposure, the bloom
                    // threshold, the AO kernel's camera terms, the SSR march's matrices —
                    // is now pushed per view by chain::ViewGlobalsListener, from that
                    // view's own workspacePreUpdate. That is what ended the two defects
                    // this loop used to cause: two on-screen views fighting over one
                    // exposure, and SSAO marching the first view's projection in the
                    // second view's frame.
                    //
                    // Offscreen views never qualify — their chainDesc() has every effect
                    // off by construction unless the caller deliberately opted in.
                    for (auto &v : mViews) {
                        if (!v->isEnabled()) continue;
                        const ChainDesc d = v->chainDesc();
                        if (!d.anyEffect()) continue;
                        chain::applyRecompileGlobals(mRoot, d);
                        break;
                    }
                    // THE PROBE CACHE'S FRAME COUNTER (ENGINE_CACHE_POLICY_SPEC P1): after
                    // the budget spent itself (updateGi) and any flush rebuilt (applyPendingGi),
                    // and before the frame renders the dirty probes.

                    for (auto &s : mScenes) s->latchProbeCaptures(drawnThisFrame(s.get()));
                    // THE PCC'S DIRTY-PROBE COLLECTION, ONCE, AFTER THE PROBE BUDGET (decision 14.3; fork:
                    // ParallaxCorrectedCubemapAuto::collectDirtyProbes): the engine declared at the PCC's build that it
                    // collects, so Ogre's frameStarted no longer does — the probes the budget chose capture in THIS frame.
                    // Every scene, as frameStarted ran for every PCC (a paused one collects nothing).
                    for (auto &s : mScenes) s->collectDirtyProbes();
                    for (auto &v : mViews) {
                        v->syncPlanarListener();
                        v->syncGlobalsListener();
                        v->syncScreenGiListener();
                        v->syncReflectListener();
                        v->syncAtomDraw();
                        v->syncPhotonView();
                    }
                    syncMonitorListeners(updated);
                }
                // THE LAMP-MAP CACHE, second half: here and nowhere earlier. The
                // scene graph has just made every world AABB and light pose this
                // frame's, and nothing has rendered yet — so a caster that moved
                // this frame re-renders its lamps' maps in this frame, and the
                // scan reads cached bounds instead of paying a root-recursive
                // getWorldAabbUpdated per item (ENGINE_CACHE_POLICY_SPEC P3).
                applyShadowCacheDirties(updated);
                // THE SKY CAPTURE (SKY-GPU), here and not with the other
                // pending work at the top of the frame: it is a SCENE pass over
                // the sky's render queue, so it needs the scene graph this
                // frame's updateSceneGraph just built — above, the static
                // memory manager has not been walked yet and on a process's
                // FIRST frame the sky quad has no world AABB at all, so the
                // capture would read back black (measured). After
                // applyShadowCacheDirties for the same reason that one runs
                // where it does: it wants "nothing has rendered yet", and this
                // renders six 128^2 faces.
                for (OgreScene *s : updated) s->applyPendingSkyCapture();
                // THE CHANGED-REGION LIST CLOSES HERE (PHOTON-7 stage 3), once per drawn scene: every producer of the
                // frame has appended - the drain's lamps, the GI step's slabs and edits, the cards' plan, the GPU scene's
                // commit and the skin pass (applyShadowCacheDirties, above) - and nothing has been submitted. A scene
                // this frame did not draw keeps its list open for the frame that draws it.
                for (OgreScene *s : updated) s->closeGiRegions();
                // THE RECORD/SWAP SPLIT. One call does both; the frame listener
                // marks the instant between them (see FrameSplitListener).
                const auto monFrameStart = std::chrono::steady_clock::now();
                if (monitor::live()) monitor::gMonitor->mSplit.mMarked = false;
                const bool monRendered = mRoot->_updateAllRenderTargets();
                notePoolSpan(poolSpanBegin, std::chrono::steady_clock::now());
                if (monitor::live()) {
                    const auto end = std::chrono::steady_clock::now();
                    const auto &sp = monitor::gMonitor->mSplit;
                    const double total =
                        std::chrono::duration<double, std::milli>(end - monFrameStart).count();
                    const double record =
                        sp.mMarked ? std::chrono::duration<double, std::milli>(
                                         sp.mMark - monFrameStart).count()
                                   : total;
                    monitor::gMonitor->stage("engine.record", record);
                    monitor::gMonitor->stage("engine.swap", total - record);
                }
                if (monRendered) {
                    for (OgreScene *s : updated) s->sceneManager()->clearFrameData();
                    if (blankUpdated) blankUpdated->clearFrameData();
                    // MIRRORS OgreRoot.cpp:1123 EXACTLY. `Root::renderOneFrame`
                    // is the only place upstream samples FrameStats, so skipping
                    // it would zero fps/frameMs/p95/p99/best/worst in
                    // renderStats() (and fail scripting.e2e.render_stats). The
                    // const_cast is well-defined — the FrameStats object is not
                    // const, only the accessor's return type is (OgreRoot.h:540)
                    // — and it is spelled out here so a pin bump re-checks that
                    // upstream still samples in the same place, with the same
                    // clock (Root::getTimer, OgreRoot.h:783).
                    if (const Ogre::FrameStats *fs = mRoot->getFrameStats())
                        const_cast<Ogre::FrameStats *>(fs)->addSample(
                            mRoot->getTimer()->getMicroseconds());
                    mRoot->_fireFrameEnded();
                }
            }
            mUpdatedScenes = unsigned(updated.size());
            // ...and its readings (P8): what the pass counters saw this frame.
            latchShadowCounters();
            // THE INJECTED FAULT (Engine::setFrameFault, lane FRAME-CATCH-1) —
            // TEST-FACING, armed by no shipping path, and HERE rather than
            // anywhere else: the frame has been recorded and submitted, so in a
            // session the eye copies have already ACQUIRED their two swapchain
            // images and still hold them, and nothing has closed yet. That is
            // exactly the position a `VK_ERROR_DEVICE_LOST` from the frame's
            // commit occupies, and a fault raised any earlier would prove
            // nothing about the acquires the close has to release.
            // THE FRAME'S WORK WINDOW CLOSES HERE (FORK-OOM-1): everything above
            // is the frame's work; an OOM after this point (the fault below) is
            // an ordinary error, the frame's state being complete.
            mOomAtFrameWorkEnd = vulkanOomCount();
            mFrameWorkEnded = true;
            if (mFrameFaultLeft && mFrameFault != FrameFault::VulkanOutOfDeviceMemoryInRecord)
                raiseFrameFault();
        }
        // POSE FOLLOWERS (Scene::followSkeleton — the selection silhouette over
        // an animating character). AFTER the frame, deliberately: the source's
        // bones are only resolved inside the render, so copying here takes the
        // pose that was just drawn and shows it on the next frame. One frame of
        // lag on a selection band, against a second full skeleton update per
        // frame if it were done the other way round.
        std::unique_ptr<monitor::Stage> monPost;
        if (monitor::live()) monPost.reset(new monitor::Stage("engine.post"));
        for (auto &s : mScenes) s->applySkeletonFollowers();
        // The frame is drawn and (for window views) presented: every view that
        // took part in it now has its OWN pixels on its target. This is the
        // signal hosts gate a loading cover on (View::framesPresented).
        for (auto &v : mViews) v->notePresented();
        // THE FRAME IS CLOSED HERE, after everything the loop does — the pose
        // followers included — so `totalMs` is what one renderOneFrame cost the
        // caller, not what the render cost.
        monPost.reset();
        if (monitor::live()) {
            // Was the render system counting at all while this frame ran? The
            // record says so outright: a frame rendered with recording off
            // reports zeros for every geometry counter, and analysis must not
            // have to infer that.
            if (Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr)
                monitor::gMonitor->current().metricsRecording =
                    rs->getMetrics().mIsRecordingMetrics;
            monitor::gMonitor->endFrame(mUpdatedScenes);
        }
    }
    // JAH_CATCH spelled out: the frame also records whether what it caught was a
    // Vulkan OOM (latchOutOfMemoryInFrame's fallback without the render system's count).
    catch (Ogre::Exception &e) {
        mLastError = detail::describeOgreFailure(e);
        mFrameThrewOom = detail::isVulkanOutOfMemory(e);
        return true;
    }
    catch (std::exception &e) { mLastError = std::string("engine: ") + e.what(); return true; }
    // NOTHING BELONGS HERE. Every line that used to follow the catch ran on the
    // normal path only (JAH_CATCH returns); the frame's close is
    // `closeRenderFrame`, called by the scope guard above.
    return true;
}

// ---------------------------------------------------------------------------
// EVERY FRAME CLOSES, THROWN OR NOT (lane FRAME-CATCH-1, 2026-09-18).
//
// WHAT WAS WRONG, and it had been written as if it were right: `JAH_CATCH`
// expands to `catch (...) { sink = ...; return ret; }` (EnginePrivate.h:219), so
// on a frame that threw, the four things below were dead code — and their own
// comments claimed the opposite ("outside the JAH_TRY, like the monitor's own
// close"). The measured consequences: the eye copies' two swapchain images stay
// ACQUIRED (the runtime has nothing to hand the next acquire, so the headset
// goes black while `VrStatus::frames` keeps climbing, and the session is wedged
// on XR_ERROR_CALL_ORDER_INVALID), the monitor's record for that frame is
// thrown away by the next `beginFrame`, a session the runtime took away is
// never ended, and a device loss inside a frame never latches — on the very
// path XID-2's "a loss ENDS the session" was written for, because that loss
// surfaces as a VK_ERROR_DEVICE_LOST thrown out of the frame's commit.
//
// WHY A SCOPE GUARD AND NOT A NO-RETURN `JAH_CATCH` VARIANT. A second macro
// would have fixed the throwing path and left the OTHER non-local exit — the VR
// pump's `return` when the runtime wants no picture — still skipping the close,
// so the fix would have had to be written twice and re-written for every future
// early return. One guard covers every exit, needs no new macro, and leaves the
// other 158 JAH_CATCH sites in the engine exactly as they were. The cost is
// that the close cannot answer anything to the caller, which it never did.
//
// THE ORDER, and each step's reason:
//   1. THE RUNTIME'S FRAME, first: it is the only step with a counterparty and
//      a deadline (a compositor inside xrWaitFrame) and the only one that frees
//      something the next frame needs — the acquired eye images. Idempotent:
//      `VrSession::endFrame` returns at once when no XR frame is open, which is
//      what the pump's own early ends leave behind.
//   2. THE MONITOR'S RECORD, so the ring holds one record per frame. On a
//      thrown frame its `totalMs` now includes the xrEndFrame the frame really
//      owed, which is the honest number.
//   3. A LOST OR STOPPED SESSION ENDS ITSELF.
//   4. THE DEVICE-LOST LATCH.
//   5. THE DEFERRED TEARDOWNS, last and with the in-frame flag cleared (or a
//      destroy asked for inside the frame would defer itself for ever).
//
// AND NOTHING MAY THROW OUT OF IT: a destructor runs it. Each step is wrapped
// on its own — a step that fails costs its own work and not the ones after it —
// and the message lands in `lastError()`, which is where a host looks anyway.
void OgreEngine::closeRenderFrame() noexcept {
    // (WHAT THE FRAME PUBLISHES is the render loop's, after this close — RenderThread::publish, ENGINE-UPGRADE-U3 F7:
    // the snapshot then carries what the close latched, the lost device and the out-of-memory included.)
    // ---- 1. the runtime's frame ------------------------------------------
    frameCloseStep(mLastError, [this] {
        if (mVrSession) vrSessionEndFrame(mVrSession);
    });
    // ---- 2. the monitor's record -----------------------------------------
    frameCloseStep(mLastError, [this] {
        if (monitor::live() && monitor::gMonitor->inFrame())
            monitor::gMonitor->endFrame(mUpdatedScenes);
        // THE POOLS TURN OVER HERE, and the next frame's GPU pair opens
        // (gpuFrameBegin's note): the between-frames work is that frame's.
        if (monitor::live()) gpuFrameBegin();
        // ...and every held record whose samples have answered by now goes to the ring: the holding queue is the
        // frame's own (ENGINE-UPGRADE-U3), the host's drain only takes the ring.
        if (monitor::live()) monitor::gMonitor->retirePending(false);
    });
    // ---- 3. a lost or stopped session ------------------------------------
    frameCloseStep(mLastError, [this] { endLostOrStoppedVrSession(); });
    // ---- 4. the fatal-OOM latch, then the device-lost latch ---------------
    // (A loss found in the same frame overwrites the OOM's message: it is the
    // stronger fact.)
    frameCloseStep(mLastError, [this] { latchOutOfMemoryInFrame(); });
    frameCloseStep(mLastError, [this] { latchDeviceLost(); });
    // ---- 5. the deferred teardowns ---------------------------------------
    mInRenderFrame = false;
    drainPendingSceneDestroys();          // noexcept itself
}

// A LOST SESSION ENDS ITSELF (F5). `VrState::Lost` is terminal at
// this pin — an external device has no recovery path (VR_SPEC §2.1 row 10)
// — so a session that reaches it can only be torn down, and leaving that to
// a host that may never ask would leave the render loop in the session's
// pacing (a zero interval with vsync off) spinning against a pump that no
// longer blocks. Ending it here makes `vrStatus().active` false, which is
// the one signal every host already has to watch.
//
// A SESSION THE RUNTIME STOPPED IS THE SAME CASE (lane VR-3b, 2026-09-17).
// `XR_SESSION_STATE_STOPPING` is not a pause: the runtime has taken the
// session away (the wearer took the headset off for good, the dashboard
// closed the app, WiVRn's link went) and the only legal thing left is to
// end it. It cannot be read off the state — a stopped session sits in
// `Idle`, which is also where a session that has not begun sits — so the
// session says it itself.
void OgreEngine::endLostOrStoppedVrSession() {
    if (mVrSession && vrSessionIsOver(mVrSession)) {
        Ogre::LogManager::getSingleton().logMessage(
            vrSessionState(mVrSession) == VrState::Lost
                ? "Jahshaka VR: the session is LOST - the engine ends it"
                : "Jahshaka VR: the session is OVER (the runtime stopped it) - the engine ends it",
            Ogre::LML_CRITICAL);
        endVrSession();
        // ONE CONVENTION FOR THE MIRROR WISH (VR-4-FIX's second read, finding
        // 4): the scene-teardown belt clears it, so the lost/stopped path
        // clears it too. A mirror pointed at a view whose session is gone is
        // nobody's wish, and the next session would inherit it.
        mVrMirrorView = nullptr; if (!mHostRecords) mHostVrMirrorView = nullptr;
    }
}

// THE GPU IS GONE (lane XID-2, 2026-09-17). Said ONCE, loudly, the moment
// the render system reports it: from here on the render system vetoes every
// frame (fork d014b064f+1bccc3f93 (was 0072) -- it no longer tries to recreate the device, which
// on this driver hangs inside vkDestroyDevice for ever), so a host that does
// not ask would see a silent, frozen picture and nothing in the log.
//
// REACHED ON A THROWN FRAME NOW (FRAME-CATCH-1), which is the only kind of
// frame a real loss produces: the wait that notices it throws
// VK_ERROR_DEVICE_LOST out of the commit, and until this lane that throw
// returned out of renderOneFrame past this test.
void OgreEngine::latchDeviceLost() {
    if (mDeviceLost) return;
    Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
    // `mFrameFaultDeviceLost` is the injected fault's second half and the only
    // way this is true without a real loss (FrameFault::ThrowDeviceLost).
    if (!(mFrameFaultDeviceLost || (rs && rs->isDeviceLost()))) return;
    mDeviceLost = true;
    mLastError = "the GPU device was lost";
    mShaderCache.noteDeviceLost();   // FRAME-MONITOR-DOOR-1: the writer never reads a lost device's pipeline cache
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka: THE GPU DEVICE WAS LOST. The session cannot continue; the renderer "
        "does not recreate a lost device. Look for an 'NVRM: Xid' line in the system "
        "log at this time (journalctl -k | grep -i xid).",
        Ogre::LML_CRITICAL);
    detail::gpuCheckpointsDump();   // JAHSHAKA_GPU_CHECKPOINTS=1: the dispatch the GPU never finished, named
}

// A VULKAN OOM INSIDE A FRAME'S WORK IS TERMINAL (lane FORK-OOM-1, 2026-09-27).
//
// The fork no longer latches an OOM as a device loss (the device is fine, and an
// OOM outside a frame is an ordinary error the caller sees). But an OOM half-way
// through a frame leaves Ogre's frame state broken whoever caught it — measured
// in the MERGE tier on a full card: 12 app runs logged the OOM, carried on, and
// crashed in the NEXT frame (VulkanRenderPassDescriptor::performLoadActions, null).
// So an OOM between the window's open (the top of renderOneFrame) and its close
// (the end of the frame's work) ends the engine's frames — the same clean end as
// a loss, with the honest reason. TERMINAL ONLY UNTIL THE EVICT/RETRY LANE (after
// D) MAKES A MID-FRAME OOM RECOVERABLE.
void OgreEngine::latchOutOfMemoryInFrame() {
    if (mGpuFault != GpuFault::None || mDeviceLost) return;
    const unsigned end = mFrameWorkEnded ? mOomAtFrameWorkEnd : vulkanOomCount();
    const bool counted = end != mOomAtFrameStart;
    // Without the render system's count: the frame's own catch, if it threw
    // before its work was done.
    const bool thrown = mFrameThrewOom && !mFrameWorkEnded;
    if (!counted && !thrown) return;
    mGpuFault = GpuFault::OutOfMemoryInFrame;
    std::string what = vulkanLastOom();
    if (what.empty()) what = mLastError;
    for (char &c : what) if (c == '\n') c = ' ';
    const bool host = what.find("VK_ERROR_OUT_OF_HOST_MEMORY") != std::string::npos;
    mLastError = std::string("GPU out of memory (") +
                 (host ? "VK_ERROR_OUT_OF_HOST_MEMORY" : "VK_ERROR_OUT_OF_DEVICE_MEMORY") +
                 "; the device is NOT lost - " + what +
                 ") inside a frame: the session cannot continue (a mid-frame out-of-memory "
                 "is not recoverable yet)";
    Ogre::LogManager::getSingleton().logMessage("Jahshaka: " + mLastError, Ogre::LML_CRITICAL);
}

GpuFault OgreEngine::gpuFault() const {
    return mDeviceLost ? GpuFault::DeviceLost : mGpuFault;
}

unsigned OgreEngine::vulkanOomCount() const {
#if JAH_RAY_QUERY
    auto *vkRs = mRoot ? dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem()) : nullptr;
    if (vkRs && vkRs->getVulkanDevice()) return vkRs->getVulkanDevice()->getOutOfMemoryFailures();
#endif
    return 0u;
}

std::string OgreEngine::vulkanLastOom() const {
#if JAH_RAY_QUERY
    auto *vkRs = mRoot ? dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem()) : nullptr;
    if (vkRs && vkRs->getVulkanDevice()) return vkRs->getVulkanDevice()->getLastOutOfMemory();
#endif
    return std::string();
}

namespace {
/// THE IN-RECORD DOOR (FrameFault::VulkanOutOfDeviceMemoryInRecord): Ogre fires
/// `frameRenderingQueued` from inside `Root::_updateAllRenderTargets`, after the
/// workspaces were recorded and before the swap — inside the frame's recording.
class InRecordFaultListener final : public Ogre::FrameListener {
public:
    explicit InRecordFaultListener(std::function<void()> raise) : mRaise(std::move(raise)) {}
    bool frameRenderingQueued(const Ogre::FrameEvent &) override { mRaise(); return true; }
private:
    std::function<void()> mRaise;
};
}   // namespace

// ---------------------------------------------------------------------------
// THE INJECTED FRAME FAULT (Engine::setFrameFault; FrameFault's note in
// Types.h). Test-facing: the engine's frame close cannot be asserted without a
// frame that throws, and nothing a suite may legally ask of the engine throws
// out of a frame.
void OgreEngine::setFrameFault(FrameFault fault, unsigned frames) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.setFrameFault(fault, frames); return true; })) return;
    mFrameFault = (fault == FrameFault::None || frames == 0u) ? FrameFault::None : fault;
    mFrameFaultLeft = mFrameFault == FrameFault::None ? 0u : frames;
    // The device-lost REPORT is armed with the fault and disarmed with it, so a
    // disarm cannot leave the latch's input stuck true for the rest of the
    // process. (The latch itself is one-way, by design: a lost device never
    // comes back.)
    if (mFrameFault != FrameFault::ThrowDeviceLost) mFrameFaultDeviceLost = false;
    // The in-record door is a frame listener, attached only while it is armed.
    const bool wantListener = mFrameFault == FrameFault::VulkanOutOfDeviceMemoryInRecord;
    if (mRoot && wantListener && !mInRecordFaultAttached) {
        if (!mInRecordFault)
            mInRecordFault.reset(new InRecordFaultListener([this] {
                if (mFrameFaultLeft && mFrameFault == FrameFault::VulkanOutOfDeviceMemoryInRecord)
                    raiseFrameFault();
            }));
        mRoot->addFrameListener(mInRecordFault.get());
        mInRecordFaultAttached = true;
    } else if (mRoot && !wantListener && mInRecordFaultAttached) {
        mRoot->removeFrameListener(mInRecordFault.get());
        mInRecordFaultAttached = false;
    }
}

void OgreEngine::raiseFrameFault() {
    if (!mFrameFaultLeft) return;
    // The kind is read BEFORE the run is spent: the LAST faulting frame of a
    // ThrowDeviceLost run is the one that matters most, and reading it after
    // the disarm below would report that one as a plain throw.
    const FrameFault fault = mFrameFault;
    if (--mFrameFaultLeft == 0u) mFrameFault = FrameFault::None;
    if (fault == FrameFault::ThrowDeviceLost) mFrameFaultDeviceLost = true;
#if JAH_RAY_QUERY
    // THE RENDER SYSTEM'S OWN FAILURE PATH (lane FORK-OOM-1): the function every
    // `checkVkResult` calls, with the live device, so the fork's latch decision
    // is the thing under test. It always throws.
    if (fault == FrameFault::VulkanOutOfDeviceMemory || fault == FrameFault::VulkanDeviceLost ||
        fault == FrameFault::VulkanOutOfDeviceMemoryInRecord) {
        auto *vkRs = mRoot ? dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem())
                           : nullptr;
        if (vkRs) {
            const bool oom = fault != FrameFault::VulkanDeviceLost;
            Ogre::onVulkanFailure(vkRs->getVulkanDevice(),
                                  oom ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_ERROR_DEVICE_LOST,
                                  oom ? "Jahshaka: an injected frame fault (Engine::setFrameFault) - "
                                        "TEST ONLY: vkAllocateMemory failed for a 67108864-byte pool"
                                      : "Jahshaka: an injected frame fault (Engine::setFrameFault) - "
                                        "TEST ONLY: a Vulkan call returned VK_ERROR_DEVICE_LOST",
                                  "OgreEngine::renderOneFrame", __FILE__, __LINE__);
        }
    }
#endif
    OGRE_EXCEPT(Ogre::Exception::ERR_INTERNAL_ERROR,
                "Jahshaka: an injected frame fault (Engine::setFrameFault) - TEST ONLY",
                "OgreEngine::renderOneFrame");
}

// ---------------------------------------------------------------------------
bool OgreEngine::deviceLost() const { return mDeviceLost; }

unsigned long long OgreEngine::queueSubmissionTag() const {
    return (unsigned long long)monitor::queueSubmissionTag(mRoot ? mRoot->getRenderSystem() : nullptr);
}

// THE RESOURCE HALF OF A FRAME, ON ITS OWN (lane OPEN-FRAMES-1, 2026-09-15).
//
// WHAT A FRAME DOES THAT NOTHING ELSE DOES. Inside
// `CompositorManager2::_updateImplementation` (OgreCompositorManager2.cpp:806),
// after every workspace has been recorded and before the final swap, upstream
// calls `RenderSystem::_update()` — and that call is two lines
// (OgreRenderSystem.cpp:1325-1333):
//
//     mTextureGpuManager->_update( false );   // the worker's command buffer,
//                                             // the staging-texture recycle
//     mVaoManager->_update();                 // the frame counter, and with it
//                                             // the delayed-block release
//
// `VulkanVaoManager::_update` is where a destroyed mesh's VBO blocks are
// actually handed back (`flushGpuDelayedBlocks`, one frame after they were
// freed) and where zero-ref staging buffers, used semaphores and delayed
// destroys retire. Nothing else in the process calls it. So a host that
// uploads and destroys GPU resources WITHOUT rendering — Studio's threaded
// project open, whose install slices each take one event-loop turn while a
// posted-event chain starves the render timer — accumulates every one of those
// blocks until `VulkanVaoManager::allocateVbo` notices it is holding more than
// `mDelayedBlocksFlushThreshold` (512 MB) and force-flushes from INSIDE the
// allocation (OgreVulkanVaoManager.cpp:965). THAT FLUSH IS A SYMPTOM, NOT THE
// FAULT (the lane's own measurement: it fires once in every CLEAN run too);
// the corrupting write is somewhere in or under Ogre, still unnamed, and a
// frame between the slices masks it — see the numbers on the shell side.
//
// THIS IS EXACTLY THAT CALL AND NOTHING MORE. No scene graph update, no cull,
// no draw, no present, and no streaming WAIT (`waitForTextureLoads` is the
// call that blocks; this one must not, because it runs between the slices of
// an install that has to stay responsive).
//
// CALLING IT OUTSIDE A FRAME IS THE PIN'S TOLERATED SHAPE (its issue #433),
// not its documented practice — upstream's offline capture path one might
// cite (OgreParallaxCorrectedCubemapAuto.cpp:385-388; the raster irradiance
// field's renderProbes did the same until the fork deleted it) completes the
// bracket with `_endFrameOnce()` every time. AND IT COMMITS ONLY EVERY SECOND CALL:
// `VulkanVaoManager::_update` (OgreVulkanVaoManager.cpp:2041-2070) issues the
// `commitAndNextCommandBuffer( NewFrameIdx )` only when the previous _update was
// not followed by a commit, so one bare call after a normal frame ARMS and the
// next one advances the frame index and releases the delayed blocks — a
// one-call lag. destroyScene's pair (before and after the erase) works because
// it is a pair. The same bare `vao->_update()` has run inside the texture drain
// since 2026-09-08.
//
// WHAT IS DELIBERATELY NOT HERE: `_beginFrameOnce()` / `_endFrameOnce()`. Those
// are the frame's brackets — `_endFrameOnce` commits with
// `SubmissionType::EndFrameAndSwap` and presents whatever windows were acquired
// — and the pin warns in as many words when they are used without an `_update`
// between them (`_notifyNewCommandBuffer`, OgreVulkanVaoManager.cpp:2186-2195).
// The advance needs neither: `_update` alone both recycles and, from the second
// consecutive call on, submits.
// ---------------------------------------------------------------------------
// VR (SPECS/VR_SPEC.md §4). Five short methods: everything that knows what an
// XrSession is lives in OgreVrSession.cpp.



bool OgreEngine::vrConnect(std::string &why) {
    if (!mVrPolicy.enabled) {
        mVrInfo.failure = VrFailure::Disabled;
        why = mVrInfo.reason = "VR is off for this process";
        return false;
    }
    if (mHeadless || !mRoot) {
        mVrInfo.failure = VrFailure::Headless;
        why = mVrInfo.reason = "this engine is headless (the NULL render system renders nothing)";
        return false;
    }
    // THE DEVICE MUST EXIST: the connection binds to it. A host asks after its
    // first view; one that asks earlier gets the surfaceless window here.
    if (!mHlmsRegistered && !mNullWindow) {
        Ogre::NameValuePairList wp; wp["windowType"] = "null";
        mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"), 8, 8, false, &wp);
        noteDevice();   // the device exists from here (Engine::deviceInfo / rayQueryAvailable)
        ensureHlms();
    }
    mVrConnection = vr::connect(mRoot, mVrPolicy, mVrInfo, why);
    if (!mVrConnection)
        Ogre::LogManager::getSingleton().logMessage("Jahshaka VR: not available - " + why,
                                                    Ogre::LML_NORMAL);
    return mVrConnection != nullptr;
}

bool OgreEngine::vrProbe() {
    JAH_HOST_CALL("Engine::vrProbe");
    if (mVrSession) return true;   // a live session is its own answer
    JAH_TRY {
        std::string why;
        if (!vrConnect(why)) return false;
        vr::disconnect(mVrConnection);
        mVrConnection = nullptr;
        return true;
    } JAH_CATCH(mLastError, false);
}

bool OgreEngine::beginVrSession(Scene *scene, const VrConfig &cfg) {
    JAH_HOST_CALL("Engine::beginVrSession");
    // THIS attempt's classification only: a refusal before the connection (a
    // session already running, no such scene) must not wear the last probe's.
    mVrInfo.failure = VrFailure::None;
    if (mVrSession) { mLastError = "beginVrSession: a session is already running"; return false; }
    OgreScene *s = nullptr;
    for (auto &candidate : mScenes)
        if (candidate.get() == scene) s = candidate.get();
    if (!s) { mLastError = "beginVrSession: no such scene"; return false; }
    JAH_TRY {
        std::string why;
        // A FRESH CONNECTION PER SESSION (VR-START-1): the policy's runtime
        // check, then a new XrInstance bound to the engine's own device. A
        // headset plugged in since the last attempt — or a WiVRn reconnect,
        // whose new streaming process the old instance can never reach — is
        // just this.
        if (!vrConnect(why)) { mLastError = "beginVrSession: " + why; return false; }
        mVrSession = vr::sessionBegin(mVrConnection, this, s, cfg, why);
        mHostVrSessionLive = mVrSession != nullptr;
        mHostVrView = mVrSession ? vrSessionView(mVrSession) : nullptr;
        if (!mVrSession) {
            vr::disconnect(mVrConnection);
            mVrConnection = nullptr;
            mVrInfo.available = false;
            mVrInfo.failure = VrFailure::RuntimeBroken;
            mVrInfo.reason = why;
            mLastError = "beginVrSession: " + why;
            return false;
        }
        // NO SESSION INHERITS A SCRIPT'S LEFTOVERS (VR-INPUT-1E-FIX finding 1).
        // The injection store answers with no session at all — that is the
        // headless test backbone — and a sample written there is addressed to
        // THAT situation, not to the wearer who puts a headset on afterwards.
        // Carried in, it replaced a real hand for the life of the session and
        // the refusal rule never saw it (nothing was being written any more).
        vrClearInjectedInput();
        // The host's mirror wish, applied now that there is something to mirror.
        vrSessionSetMirror(mVrSession, mVrMirrorView);
        return true;
    } JAH_CATCH(mLastError, false);
}

void OgreEngine::endVrSession() {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (hostPosts()) { mHostVrSessionLive = false; mHostVrView = nullptr; }   // the host's records, at the call
    if (postedAsMessage([=](OgreEngine &e) { e.endVrSession(); return true; })) return;
    if (!mVrSession) return;
    // CLEARED FIRST, THEN DELETED. The session's destructor calls back into
    // this engine (destroyView for its own View), and anything that re-entered
    // through a live `mVrSession` pointer would be talking to a half-destroyed
    // object. After this line there is no session, which is the truth every
    // caller should see while one is being taken apart.
    VrSession *session = mVrSession;
    mVrSession = nullptr;
    if (!mHostRecords) {   // the host's records are the host's (decision 45, D5): set at its call, never at the drain
        mHostVrSessionLive = false;
        mHostVrView = nullptr;
    }
    // ...AND NO SESSION LEAVES ITS OWN BEHIND (finding 1). A suite that
    // injected through a live session (with the override on) would otherwise
    // hand the next one, or the headless verbs after it, a hand nobody wrote.
    vrClearInjectedInput();
    const bool lost = vrSessionState(session) == VrState::Lost;
    JAH_TRY {
        vr::sessionEnd(session);
    } JAH_CATCH(mLastError, );
    // ...AND ITS CONNECTION (VR-START-1): the next session connects afresh.
    if (mVrConnection) {
        JAH_TRY { vr::disconnect(mVrConnection); } JAH_CATCH(mLastError, );
        mVrConnection = nullptr;
    }
    if (lost) {
        mVrInfo.available = false;
        mVrInfo.failure = VrFailure::ConnectionLost;
        mVrInfo.reason = "the headset or its runtime went away during the session";
    }
}

VrState OgreEngine::vrState() const {
    // A VR READ ON THE HOST'S SIDE IS THE SNAPSHOT'S (U3 stage 6; a request until then).
    if (hostPosts()) {   // THE SNAPSHOT (stage 6): the last frame's, never a wait — a session the host ended is ended
        if (!mHostVrSessionLive && vrStateIsLive(published().vrState)) return vrAvailable() ? VrState::Idle
                                                                                            : VrState::Unavailable;
        return published().vrState;
    }
    return vrStateNow();
}

VrState OgreEngine::vrStateNow() const {
    if (!mVrSession) return vrAvailable() ? VrState::Idle : VrState::Unavailable;
    return vrSessionState(mVrSession);
}

const VrStatus &OgreEngine::vrStatus() const {
    // A VR READ ON THE HOST'S SIDE IS THE SNAPSHOT'S (U3 stage 6; a request until then): the frame's status, the
    // injections it applied included (a host injection is a request — republished before the host reads again) — A
    // REFERENCE INTO THE LATCHED BUFFER (decision 61: no copy per call; valid until the next latch, never held across
    // one).
    if (hostPosts()) {
        const VrStatus &frame = published().vr;
        // ...WITH THE HOST'S OWN WRITES OVER IT where the host is the writer and the frame has not caught up: a session
        // the host ended (its record), and an origin it set since the frame the snapshot is (a host-side copy then).
        if (!mHostVrSessionLive && frame.active) {
            // The engine's answer with no session (its end clears the injection store too): nothing of the session —
            // its counters belong to it, not to the process.
            mHostVrStatusRead = VrStatus();
            mHostVrStatusRead.state = vrAvailable() ? VrState::Idle : VrState::Unavailable;
            mHostVrStatusRead.framesOffRenderThread = frame.framesOffRenderThread;
            return mHostVrStatusRead;
        }
        if (mHostVrOriginSeq > frame.originSeq) {
            mHostVrStatusRead = frame;
            mHostVrStatusRead.origin = mHostVrOrigin;
            mHostVrStatusRead.originYaw = mHostVrOriginYaw;
            mHostVrStatusRead.originSeq = mHostVrOriginSeq;
            return mHostVrStatusRead;
        }
        return frame;
    }
    // THE ENGINE'S OWN SIDE composes its answer into its scratch (the render thread's alone).
    VrStatus &s = mVrStatusScratch;
    if (!mVrSession) {
        s = VrStatus();
        s.state = vrStateNow();
        // WITH NO SESSION THE ONLY HANDS THERE CAN BE ARE INJECTED ONES, and
        // they are reported through exactly the fields a session fills — which
        // is what lets the gesture logic above this boundary be written once
        // and tested with no runtime (VR_INPUT_SPEC §2.4, §10).
        for (unsigned h = 0; h < VrHandCount; ++h) {
            if (!mVrInjected[h]) continue;
            s.input[h] = mVrInject[h];
            s.hands[h] = mVrInject[h].grip;   // `input[i].grip` IS `hands[i]`
            // ...AND WHETHER A SKELETON WAS INJECTED FOR IT (stage 3): with no
            // session there is no runtime to track a hand, so the injected
            // joints are the only ones there can be.
            s.input[h].jointsTracked = mVrJointsInjected[h];
        }
        // THE SESSION'S PROFILE SUMMARY, DERIVED FROM THE HANDS, with no
        // session at all: an injected sample may name a profile (that is how
        // the hand/controller half of stage 3 is driven headlessly), and
        // `vr.state().profile` answers the same question a live session's does.
        s.profile = !s.input[VrHandRight].profile.empty() ? s.input[VrHandRight].profile
                                                          : s.input[VrHandLeft].profile;
        // NOTHING IS FOCUSED WHEN THERE IS NO SESSION — unless a hand is
        // INJECTED, and then focus is whatever the test said (true by default:
        // a test that says nothing about focus means the wearer was there).
        // One bit for the session, never one per hand (VR-INPUT-1E-FIX).
        s.inputFocused = vrAnyInjectedInput() && mVrInjectFocus;
        return s;
    }
    s = vrSessionStatus(mVrSession);
    s.framesOffRenderThread = mVrFramesOffRenderThread;
    s.originSeq = mVrOriginSeq;
    // ...AND WITH A SESSION THE STORE IS STILL THE TRUTH BETWEEN FRAMES
    // (VR-INPUT-1E-FIX, the lead's item 2026-09-17). The session's `input[]` is
    // a per-FRAME copy — the injection is applied inside readInput, which is
    // inside the frame — so an injection written between two frames was
    // invisible here until the next one, while the no-session branch above
    // reported it at once. One boundary cannot answer the same question two
    // ways: a host that injects a gesture and reads the state back had to
    // render a frame it did not otherwise need (the Studio suite did, and said
    // so in a comment).
    //
    // THE FRAME IS STILL WHERE IT COUNTS — that is what moves the proxies, the
    // ray and `hands[]` as drawn — and the refusal rule is unchanged: a hand
    // whose runtime has bound a real profile is not overlaid, exactly as
    // readInput would not apply it.
    bool overlaid = false;
    for (unsigned h = 0; h < VrHandCount; ++h) {
        if (!mVrInjected[h]) continue;
        if (vrSessionHasBoundProfile(mVrSession, int(h)) && !vrTestInjectAllowed()) continue;
        s.input[h] = mVrInject[h];
        s.input[h].fromInjection = true;
        s.hands[h] = mVrInject[h].grip;   // `input[i].grip` IS `hands[i]`
        s.input[h].jointsTracked = mVrJointsInjected[h] ||
                                   vrSessionHasLiveJoints(mVrSession, int(h));
        overlaid = true;
    }
    // ...AND THE SESSION'S FOCUS IS THE TEST'S while it is driving the hands,
    // which is how a focus-loss cancel is asserted against a live runtime whose
    // dashboard nothing can raise.
    if (overlaid) s.inputFocused = mVrInjectFocus;
    return s;
}

// THE DEVICE'S CONSTANTS (Engine::deviceInfo / rayQueryAvailable, ENGINE-UPGRADE-U3 stage 2): fixed once, right after
// the first window — the one that creates the device — exists; every read after is the boundary's.
void OgreEngine::noteDevice() {
    if (mDeviceKnown || !mRoot) return;
    const DeviceInfo info = readDeviceInfo();
    // KNOWN once a render system answers (its name); called only where a device exists — after a window that
    // creates it, or at the end of a headless init, whose NULL system names no device at all.
    if (info.renderSystem.empty()) return;
    mDeviceInfo = info;
    mDeviceRayQuery = deviceRayQuery();
    mDeviceKnown = true;
}

bool OgreEngine::vrEyeScreenshot(unsigned eye, Image &out) {
    JAH_HOST_CALL("Engine::vrEyeScreenshot");
    if (!mVrSession) { mLastError = "vrEyeScreenshot: no session is running"; return false; }
    return vrSessionEyeScreenshot(mVrSession, eye, out, mLastError);
}

void OgreEngine::setVrMirrorView(View *view) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (hostPosts()) mHostVrMirrorView = view;   // the host's record, at the call
    if (postedAsMessage([=](OgreEngine &e) { e.setVrMirrorView(view); return true; })) return;
    mVrMirrorView = static_cast<OgreView *>(view);
    if (!mHostRecords) mHostVrMirrorView = view;   // the host set it at its call (D5)
    if (mVrSession) vrSessionSetMirror(mVrSession, mVrMirrorView);
}

// ---------------------------------------------------------------------------
// THE INJECTION HOOK (Engine::vrInjectInput; VR_INPUT_SPEC §2.4 I1) AND THE ONE
// OUTPUT (Engine::vrHaptic).
//
// WHY THE STORE IS HERE AND NOT IN THE SESSION. The hook's whole value is that
// the interaction logic above it runs with NO runtime: a gesture test drives
// two poses and four booleans through the same struct the runtime fills, on a
// box with no headset. So the engine owns the samples, `vrStatus()` reports
// them when no session exists, and a session that starts later reads them per
// frame (readInput) and reports them exactly as it reports the runtime's own.
bool OgreEngine::vrInjectInput(int hand, const VrHandState &state) {
    JAH_HOST_CALL("Engine::vrInjectInput");
    if (hand < 0 || hand >= int(VrHandCount)) {
        mLastError = "vrInjectInput: hand must be 0 (left) or 1 (right)";
        return false;
    }
    // A WITHDRAWAL IS NEVER REFUSED (VR-INPUT-1E-FIX finding 1). A default
    // state means "stop injecting", and refusing THAT was the rule eating its
    // own purpose: a script that had put a hand somewhere (legally, before the
    // runtime bound a profile, or with the override on) could not take it back
    // again without the override — so the safe direction was the one that
    // needed permission. Taking a fake hand away can never fool a smoke.
    if (!state.valid) {
        vrClearInjectedInput(hand);
        return true;
    }
    // THE WEARER'S HARDWARE ALWAYS WINS. A live session whose runtime has
    // bound a real interaction profile for this hand refuses the write, so a
    // smoke in a headset can never be fooled by an injection a script left
    // behind. The escape is an EXPLICIT process-level one, read live rather
    // than latched at boot so a suite can prove BOTH halves in one process.
    //
    // AND THE REFUSAL IS ONLY THE FRONT DOOR: the session's own per-frame read
    // IGNORES AND CLEARS a sample whose hand has since been bound (the write
    // may have been perfectly legal at the time — the profile arrives a few
    // frames into a session), and the store is emptied at both ends of a
    // session. The three together are what makes the rule a guarantee rather
    // than a check on one code path.
    if (mVrSession && vrSessionHasBoundProfile(mVrSession, hand) && !vrTestInjectAllowed()) {
        mLastError = "vrInjectInput: refused - the runtime has a real interaction profile "
                     "bound for that hand (set JAHSHAKA_VR_TEST_INJECT=1 to override)";
        return false;
    }
    mVrInject[hand] = state;
    mVrInject[hand].fromInjection = true;
    // A SAMPLE THAT SAID NOTHING ABOUT ITS MANIPULATION FRAME HOLDS BY ITS GRIP
    // (stage 3, VrHandState::manipPose) — the rule lives HERE, once, so that
    // every injector gets it and the session's own read does not have to guess.
    if (!mVrInject[hand].manipPose.valid) mVrInject[hand].manipPose = mVrInject[hand].grip;
    mVrInjected[hand] = true;
    return true;
}

// ---------------------------------------------------------------------------
// THE WEARER'S SKELETON (Engine::vrHandJoints / vrInjectJoints; VR_INPUT_SPEC
// §7, stage 3).
//
// WHY THE JOINTS ARE FETCHED AND NOT REPORTED. Fifty-two poses is 1.7 kB, and
// `VrStatus` is copied several times per FRAME by every host that reads it — so
// the status carries one bit per hand (`jointsTracked`) and the poses are asked
// for by the two callers that want them: the mirror, which draws them, and a
// test. The session holds this frame's set; with no session the store below is
// the only source there can be.
unsigned OgreEngine::vrHandJoints(int hand, VrPose *out, unsigned count) const {
    // A VR READ ON THE HOST'S SIDE IS THE SNAPSHOT'S (U3 stage 6): this frame's joints, as the frame located them.
    if (hostPosts()) {
        if (hand < 0 || hand >= int(VrHandCount) || !out) return 0u;
        const std::vector<VrPose> &j = published().vrJoints[hand];
        const unsigned n = unsigned(std::min<size_t>(j.size(), count));
        std::copy(j.begin(), j.begin() + n, out);
        return n;
    }
    if (hand < 0 || hand >= int(VrHandCount)) return 0u;
    // A LIVE SESSION WITH HANDS OFF HAS NO SKELETON TO REPORT (lane
    // HANDS-SWITCH-1), and that includes an injected one: a project that asked
    // for controllers must not be shown a script's fingers. With NO session at
    // all the store below is still the only source there can be — which is what
    // keeps the headless bare-hand suite driving the interaction rules on a box
    // that has no fingers and no runtime.
    if (mVrSession && !vrSessionHandsEnabled(mVrSession)) return 0u;
    if (mVrSession) {
        if (const unsigned n = vrSessionHandJoints(mVrSession, hand, out, count)) return n;
    }
    // THE INJECTED SKELETON, and only for a hand the runtime is not tracking —
    // the session's own answer above has already been asked for and preferred.
    if (!vrInjectedJoints(hand, out, count)) return 0u;
    return kVrHandJointCount;
}

// EVERY SUGGESTED-BINDING BLOCK, AND WHAT THE RUNTIME DID WITH IT (stage 3's
// fix round; Engine::vrBindingBlocks). Only a session can answer: the blocks
// are suggested once, at its creation.
unsigned OgreEngine::vrBindingBlocks(VrBindingBlock *out, unsigned count) const {
    // A VR READ ON THE HOST'S SIDE IS THE SNAPSHOT'S (U3 stage 6): the blocks the session was created with.
    if (hostPosts()) {
        if (!out) return 0u;
        const std::vector<VrBindingBlock> &b = published().vrBindingBlocks;
        const unsigned n = unsigned(std::min<size_t>(b.size(), count));
        std::copy(b.begin(), b.begin() + n, out);
        return n;
    }
    return mVrSession ? vrSessionBindingBlocks(mVrSession, out, count) : 0u;
}

bool OgreEngine::vrInjectJoints(int hand, const VrPose *joints, unsigned count) {
    JAH_HOST_CALL("Engine::vrInjectJoints");
    if (hand < 0 || hand >= int(VrHandCount)) {
        mLastError = "vrInjectJoints: hand must be 0 (left) or 1 (right)";
        return false;
    }
    // A WITHDRAWAL IS NEVER REFUSED, exactly as for a sample (VR-INPUT-1E-FIX
    // finding 1): taking a fake hand away cannot fool anybody.
    if (!joints || count == 0u) {
        mVrJointsInjected[hand] = false;
        return true;
    }
    // THE WEARER'S OWN HAND WINS. A hand the runtime is really tracking is not
    // overwritten by a script's skeleton; the escape is the same explicit,
    // live-read process switch the controls use.
    if (mVrSession && vrSessionHasLiveJoints(mVrSession, hand) && !vrTestInjectAllowed()) {
        mLastError = "vrInjectJoints: refused - the runtime is tracking that hand's joints "
                     "(set JAHSHAKA_VR_TEST_INJECT=1 to override)";
        return false;
    }
    const unsigned n = count < kVrHandJointCount ? count : unsigned(kVrHandJointCount);
    for (unsigned j = 0; j < kVrHandJointCount; ++j)
        mVrInjectJoints[hand][j] = j < n ? joints[j] : VrPose();
    mVrJointsInjected[hand] = true;
    return true;
}

bool OgreEngine::vrHaptic(int hand, float amplitude01, float seconds) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { return e.vrHaptic(hand, amplitude01, seconds); })) return true;
    if (hand < 0 || hand >= int(VrHandCount)) {
        mLastError = "vrHaptic: hand must be 0 (left) or 1 (right)";
        return false;
    }
    if (!mVrSession) { mLastError = "vrHaptic: no session is running"; return false; }
    return vrSessionHaptic(mVrSession, hand, amplitude01, seconds, mLastError);
}

void OgreEngine::setVrOrigin(const Vec3 &position, float yawDegrees) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain — and the
    // host's RECORD of it, sequenced (U3 stage 6): its reads report the write until a frame applied it.
    if (hostPosts()) {
        const unsigned long long seq = ++mHostVrOriginSeq;
        mHostVrOrigin = position;
        mHostVrOriginYaw = yawDegrees;
        if (postedAsMessage([=](OgreEngine &e) { e.setVrOrigin(position, yawDegrees); e.mVrOriginSeq = seq; return true; }))
            return;
    }
    // NOT REMEMBERED ACROSS SESSIONS, deliberately: where the wearer stands is
    // a property of the RUN the host started, and a session that inherited the
    // previous one's origin would put the next wearer wherever the last one
    // walked to. A host sets it right after beginVrSession().
    if (mVrSession) vrSessionSetOrigin(mVrSession, position, yawDegrees);
}

void OgreEngine::advanceResources() {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.advanceResources(); return true; })) return;
    if (!mRoot) return;
    JAH_TRY {
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        // A NULL render system is a headless boot (EngineConfig::headless) or
        // the window between Root and initialise: nothing has been allocated
        // through a VaoManager, so there is nothing to advance.
        if (!rs || !rs->getVaoManager()) return;
        // COUNTED BEFORE THE CALL, on purpose: the count answers "was the
        // renderer asked to advance?", which is the question a host's suite
        // has (an advance that threw is a failure to report, not an advance
        // that never happened).
        ++mResourceAdvances;
        rs->_update();
    } JAH_CATCH(mLastError, );
}

bool OgreEngine::updateScene(Scene *scene) {
    JAH_HOST_CALL("Engine::updateScene");
    applyPendingEngineMessages();   // a scene the host created is adopted before its resolve (U3 stage 4a)
    if (!mRoot || !scene) { mLastError = "updateScene: no engine or no scene"; return false; }
    // Ownership check, not politeness: a Scene* from a destroyed engine, or a
    // pointer this engine never handed out, would otherwise be dereferenced.
    OgreScene *target = nullptr;
    for (auto &s : mScenes)
        if (s.get() == scene) { target = s.get(); break; }
    if (!target) { mLastError = "updateScene: unknown Scene"; return false; }
    JAH_TRY {
        Ogre::SceneManager *sm = target->sceneManager();
        if (!sm) { mLastError = "updateScene: scene has no manager"; return false; }
        // WHAT WAS ALREADY SENT LANDS FIRST (U3 stage 3): the scene's commands are messages in the pending record now
        // (a clip's time is setClipStates, a pose is a node record), and a resolve that ran before them would answer
        // for the previous frame's input. This scene's stream is taken out of the record and applied here, in its
        // emission order; the rest of the record (its environment, the views, every other scene) waits for the frame.
        {
            std::vector<SceneMessage> messages;
            std::vector<NodeRecord> nodes;
            unsigned long long step = 0ull;
            mRenderThread.withPending([&](FrameInput &rec, bool) {
                for (FrameInputScene &p : rec.scenes)
                    if (p.scene == scene) {
                        for (SceneMessage &m : p.messages) messages.push_back(std::move(m));
                        for (NodeRecord &r : p.nodes) nodes.push_back(std::move(r));
                        p.messages.clear();
                        p.nodes.clear();
                    }
                step = rec.stepSerial.value_or(0ull);
            });
            if (!messages.empty() || !nodes.empty()) {
                EngineSideScope engineSide;
                // THE LAST DRAIN'S FIELDS ARE THE DRAIN'S (decision 36): a resolve's stream counts into the totals
                // only — the per-drain counts keep saying what the last frame's drain applied.
                const unsigned lastMessages = mFrameInputStatus.messages;
                const unsigned lastRecords = mFrameInputStatus.nodeRecords;
                const unsigned lastTransforms = mFrameInputStatus.nodeTransforms;
                applySceneStream(target, messages, nodes, step);
                mFrameInputStatus.messages = lastMessages;
                mFrameInputStatus.nodeRecords = lastRecords;
                mFrameInputStatus.nodeTransforms = lastTransforms;
            }
        }
        sm->updateSceneGraph();
        // PAIRED, and it matters: updateSceneGraph APPENDS to the manager's
        // global light list (buildLightList) and leaves render-queue state
        // behind. Root's frame clears both afterwards; a standalone update has
        // to do the same or repeated calls grow the light list without bound.
        sm->clearFrameData();
        // ...AND THE RESOLVE IS PUBLISHED (ENGINE-UPGRADE-U3 stage 2): the scene's half of the snapshot is re-written
        // with the pose this resolve produced (the subscribed rigs' bones, PublishedFrame::bonePoses) — the call's
        // completion, read by the host without asking (stage 3 makes the call itself a request). The frame stamp
        // and the serial are the last frame's: no frame was drawn.
        {
            EngineSideScope engineSide;
            target->publishFrame(mPublishedFrames, mRenderThread.serial());
            // ...and committed on its own (decision 54: a frame's set commits in RenderThread::publish; a resolve is
            // one scene's publish between frames).
            std::lock_guard<std::mutex> l(snapshotLock());
            target->commitPublishedLocked();
        }
        return true;
    } JAH_CATCH(mLastError, false);
}

// THE HOST'S RECORD OF THE VIEWS (Engine::listViews / hasEnabledViews, ENGINE-UPGRADE-U3 stage 2): the boundary's
// list, re-made after every change of mViews (creation, destruction, teardown). Whether one has something to draw is
// the boundary's own reading (View::pushedEnabled, and a VR session always — its frame loop is its heartbeat).
void OgreEngine::syncHostViews() {
    std::lock_guard<std::mutex> lock(mHostViewsLock);
    mHostViews.clear();
    mHostViews.reserve(mViews.size());
    for (const auto &v : mViews) mHostViews.push_back(v.get());
}

// ---------------------------------------------------------------------------
// Texture streaming (THREADING_ADOPTION_SPEC.md P2). One TextureGpuManager per
// process — it belongs to the render system, not to a Scene — so all of these
// are Engine verbs, not Scene ones. Every one of them is safe before the render
// system exists (a headless run, or the window between Root and initialise):
// they answer "done", 0, or do nothing.
namespace {
Ogre::TextureGpuManager *textureManagerOf(Ogre::Root *root) {
    if (!root) return nullptr;
    Ogre::RenderSystem *rs = root->getRenderSystem();
    return rs ? rs->getTextureGpuManager() : nullptr;
}
}   // namespace

bool OgreEngine::texturesDoneStreaming() const {
    JAH_SYNC_GETTER("Engine::texturesDoneStreaming");
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    return tm ? tm->isDoneStreaming() : true;
}

namespace {
/// Textures the manager has not finished preparing, and (optionally) the first
/// few of them written out for a log line.
///
/// MAIN THREAD ONLY, and only between `_update()` calls — which is where both
/// callers sit. `mEntries` is written by createTexture/destroyTexture, both of
/// which are main-thread verbs of ours; the streaming and multiload workers
/// never touch the map (they carry a TextureGpu* in the request), so reading it
/// here needs no lock we are able to take anyway.
size_t countPendingTextures(Ogre::TextureGpuManager *tm, std::string *namesOut) {
    static const size_t kMaxNames = 8u;
    size_t pending = 0;
    for (const auto &kv : tm->getEntries()) {
        Ogre::TextureGpu *t = kv.second.texture;
        if (!t || t->isDataReady()) continue;
        ++pending;
        if (!namesOut || pending > kMaxNames) continue;
        if (!namesOut->empty()) *namesOut += ", ";
        *namesOut += kv.second.name.empty() ? std::string("<unnamed>") : kv.second.name;
        *namesOut += " (group=";
        *namesOut += kv.second.resourceGroup.empty() ? std::string("<NONE>")
                                                     : kv.second.resourceGroup;
        *namesOut += ", residency=";
        switch (t->getResidencyStatus()) {
        case Ogre::GpuResidency::OnStorage:     *namesOut += "OnStorage"; break;
        case Ogre::GpuResidency::OnSystemRam:   *namesOut += "OnSystemRam"; break;
        case Ogre::GpuResidency::Resident:      *namesOut += "Resident"; break;
        default:                                *namesOut += "?"; break;
        }
        *namesOut += ", pendingChanges=";
        *namesOut += std::to_string(unsigned(t->getPendingResidencyChanges()));
        *namesOut += t->isManualTexture() ? ", manual)" : ", from-file)";
    }
    return pending;
}
}   // namespace

// ONE COLLECTION PASS, NO WAIT (OPEN_COVER_SPEC §2 E). The drain below is a
// loop around this same call with a no-progress deadline; a streaming frame
// wants the progress and not the deadline, because the frame it would block is
// a frame of a world the user is already looking at.
void OgreEngine::collectTextureStreaming() {
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    if (!tm) return;
    try { tm->_update(true); } catch (...) {}
}

bool OgreEngine::drainTextureStreaming(double *msSpent) {
    // THE BOUNDED DRAIN — what `waitForStreamingCompletion` should have been.
    //
    // Upstream's version (OgreTextureGpuManager.cpp:3619-3645) is this same
    // loop with `mRequestToMainThreadEvent.wait()` where the sleep is. That
    // wait has NO timeout, so a load request that no worker will ever complete
    // parks the UI thread for the life of the process: measured 2026-09-08,
    // twelve minutes inside a GLB import's thumbnail render, every TxtreLoad
    // and TexStream worker idle. The trigger was ours and is fixed at source
    // (the SSAO noise texture, OgreChain.cpp initSsao) — this is the guard that
    // makes the NEXT one a slow frame and a log line instead of a dead app.
    //
    // THE BUDGET IS A NO-PROGRESS BUDGET. `_update(true)` is the same call
    // upstream makes and does the same work (it swaps the worker's command
    // buffer, executes it, recycles staging textures); as long as the pending
    // set keeps shrinking the deadline keeps moving, so a hundred-texture scene
    // on a slow disk waits as long as it needs to. Only a set that has not
    // moved at all for `mTextureWaitBudgetMs` expires.
    //
    // GIVING UP IS LOUD AND IT IS FINAL. The pending textures are logged by
    // name, group and residency — the group is there because a blank one is
    // exactly the defect that caused this — and `mTextureWaitBroken` latches,
    // so the frame after does not pay the budget again. A host that wants to
    // fail rather than continue reads textureWaitTimeouts().
    //
    // JAH_TEXTURE_WAIT_FAULT IS A TEST HOOK, and it exists because the failure
    // this guards against cannot be provoked any other way: the trigger that
    // produced it (a group-less texture) kills the decode WORKER before the
    // main thread can time anything, so a suite that wanted to prove the give-up
    // path works has nothing to reach for. With the flag set the drain simply
    // never agrees that it is finished, which is precisely what a stuck queue
    // looks like from in here — the deadline, the diagnostic and the latch then
    // run exactly as they would in the wild. Off unless the env var is set.
    if (msSpent) *msSpent = 0.0;
    TextureDrain d;
    int step = beginTextureDrain(d);
    while (step == 1) {
        step = stepTextureDrain(d);
        if (step == 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (d.active) {
        const double spent = d.elapsedMs();
        if (spent > mTextureWaitWorstMs) mTextureWaitWorstMs = spent;
        if (msSpent) *msSpent = spent;
    }
    return step == 0;
}

/// THE DRAIN'S START (WAIT-TEXTURE-LOADS-1): 0 nothing to wait for (or the wait is broken / disabled: the queue's
/// own answer), 1 a drain begun.
int OgreEngine::beginTextureDrain(TextureDrain &d, bool neverDone) {
    d = TextureDrain();
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    if (!tm) return 0;
    if (mTextureWaitBroken || mTextureWaitBudgetMs == 0u) return tm->isDoneStreaming() ? 0 : 2;
    if (!mTextureWaitFault && !neverDone && tm->isDoneStreaming()) return 0;
    d.active = true;
    d.t0 = std::chrono::steady_clock::now();
    return 1;
}

/// ONE STEP OF THE BOUNDED DRAIN, never a sleep (WAIT-TEXTURE-LOADS-1): drainTextureStreaming loops it with a
/// millisecond's sleep between steps; pumpTextureLoads hands each step to a host that waits OFF the render thread.
/// 0 the queues emptied, 1 still streaming, 2 the no-progress budget expired (logged, latched).
int OgreEngine::stepTextureDrain(TextureDrain &d, bool neverDone) {
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    if (!tm || !d.active) return 0;
    Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
    const double budget = double(mTextureWaitBudgetMs);
    // THE VAO ADVANCE IS ON A CADENCE, NOT ON THE POLL (DRAIN-1, audit ON-17;
    // mTextureDrainAdvanceMs has the measurement and the reason). -1e9 so the
    // first iteration advances immediately — and WHAT THAT FIRST ADVANCE DOES
    // IS ARM, NOT COMMIT: a bare `VulkanVaoManager::_update` outside a frame
    // commits at the TOP of the call, only when the previous one left the fence
    // unflushed (the pin's issue #433; the note at `advanceResources` above
    // spells the pair out). So the first advance of a drain arms and the SECOND
    // — one cadence later — commits and advances the frame index. A drain that
    // finishes inside one cadence therefore commits nothing itself, exactly as
    // before this line: the drain never submitted the capture-free first call's
    // work either, and the next frame's own commit carries it.
    {
        const bool workerDone = tm->_update(true);
        if (!mTextureWaitFault && !neverDone && workerDone && tm->isDoneStreaming()) return 0;

        const double now = d.elapsedMs();
        // The pending census is O(textures in the process) — poll it every
        // 250 ms rather than every iteration. Below that interval the loop is
        // two mutex acquisitions and a millisecond of sleep.
        if (now - d.lastPollMs >= 250.0) {
            d.lastPollMs = now;
            const size_t pending = countPendingTextures(tm, nullptr);
            if (pending < d.bestPending) { d.bestPending = pending; d.lastProgressMs = now; }
        }
        if (now - d.lastProgressMs >= budget) {
            std::string names;
            const size_t pending = countPendingTextures(tm, &names);
            Ogre::LogManager::getSingleton().logMessage(
                "ERROR: texture streaming made no progress for " +
                    std::to_string(unsigned(budget)) + " ms and the frame stopped waiting. " +
                    std::to_string(unsigned(pending)) + " texture(s) still pending: " +
                    (names.empty() ? std::string("<none nameable>") : names) +
                    ". Rendering continues WITHOUT them; this is a defect, not a slow disk "
                    "(the budget only expires when the pending set stops shrinking).",
                Ogre::LML_CRITICAL);
            mTextureWaitBroken = true;
            ++mTextureWaitTimeouts;
            return 2;
        }
        if (vao && now - d.lastAdvanceMs >= mTextureDrainAdvanceMs) {
            d.lastAdvanceMs = now;
            ++mTextureWaitAdvances;
            vao->_update();
        }
    }
    return 1;
}

double OgreEngine::waitForTextureLoads() {
    JAH_HOST_CALL("Engine::waitForTextureLoads");
    double ms = 0.0;
    drainTextureStreaming(&ms);
    return ms;
}

int OgreEngine::pumpTextureLoads() {
    JAH_HOST_CALL("Engine::pumpTextureLoads");
    // THE HOST'S WAIT, ONE STEP A CALL (WAIT-TEXTURE-LOADS-1): the drain's state lives here between the calls, so the
    // render thread does one step a request and the host waits between them - the frame is never held by it.
    // A THROW INSIDE A STEP ENDS THE WAIT (decision 22.5): the drain's state is reset on the way out, so the host's
    // next wait begins its own no-progress budget instead of stepping a stale one past it (which latched
    // mTextureWaitBroken for the process). The arm textures.pumpFault is the door that throws.
    struct ResetOnThrow {
        TextureDrain &d;
        bool kept = false;
        ~ResetOnThrow() { if (!kept) d = TextureDrain(); }
    } reset{mHostDrain};
    const double fault = mArms.value(ArmId::TexturePumpFault);
    if (mHostDrain.active && fault > 1.5)
        throw std::runtime_error("textures.pumpFault: the host's texture-drain step threw (a test door)");
    int step = mHostDrain.active ? stepTextureDrain(mHostDrain, fault > 0.5) : beginTextureDrain(mHostDrain, fault > 0.5);
    if (step == 1 && !mHostDrain.active) step = 0;
    if (step != 1) {
        if (mHostDrain.active) {
            const double spent = mHostDrain.elapsedMs();
            if (spent > mTextureWaitWorstMs) mTextureWaitWorstMs = spent;
        }
        mHostDrain = TextureDrain();
    }
    reset.kept = true;
    return step;
}

unsigned long long OgreEngine::textureWaitAdvances() const { JAH_SYNC_GETTER("Engine::textureWaitAdvances"); return mTextureWaitAdvances; }

unsigned long long OgreEngine::textureLoadRequests() const {
    JAH_SYNC_GETTER("Engine::textureLoadRequests");
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    return tm ? static_cast<unsigned long long>(tm->getLoadRequestsCounter()) : 0ull;
}


unsigned OgreEngine::textureMetadataCacheEntries() const {
    JAH_SYNC_GETTER("Engine::textureMetadataCacheEntries");
    return textureCache().metadataEntries(mRoot);
}

unsigned OgreEngine::textureChannelCacheEntries() const {
    JAH_SYNC_GETTER("Engine::textureChannelCacheEntries");
    return textureCache().channelEntries();
}

bool OgreEngine::saveTextureCache() {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { return e.saveTextureCache(); })) return true;
    if (!mRoot) return false;
    JAH_TRY {
        return textureCache().save(mRoot, [this](std::function<void()> write) {
            mShaderCache.dispatchTask(std::move(write));
        });
    } JAH_CATCH(mLastError, false);
}

void OgreEngine::setVsync(bool on) {
    // THE HOST'S CALL IS A MESSAGE (U3 stage 4a, decision 39), applied at the drain; JAH_HOST_CALL counts a direct one.
    if (hostPosts()) mHostVsync = on;   // the host's record, at the call
    if (postedAsMessage([=](OgreEngine &e) { e.setVsync(on); return true; })) return;
    JAH_HOST_CALL("Engine::setVsync");
    if (mVsync == on) return;
    mVsync = on;
    if (!mHostRecords) mHostVsync = on;   // the host's record (Engine::vsync); set at its call when records (D5)
    JAH_TRY {
        for (auto &v : mViews) {
            Ogre::Window *w = v->ogreWindow();
            if (!w) continue;   // offscreen views never present
            // The sign bit goes with EVERY call: setVSync assigns
            // mLowestLatencyVSync from the interval BEFORE its "nothing
            // changed" early-return, so a bare setVSync(x, 1) would clear the
            // lowest-latency request for the rest of the session.
            w->setVSync(on, 1u | kLowestLatencyVSync);
        }
        return;
    } JAH_CATCH(mLastError, );
}

// THE HOST'S ERROR DOOR (U3 stage 4a): what the host's own calls were refused for — a setter refused against the
// host's record (noteHostError), a join's reason — folded into the calls' string the engine-side bodies write. A door,
// not a read of the frame's state: the frame's faults are the snapshot's (frameErrors, refusals).
// THE HOST'S ERROR DOOR (U3 stage 4a; decision 44): on the host's side it is the host's own string ONLY — a setter
// refused against the host's record (noteHostError), a join's refusal moved here at the rendezvous — and NEVER the
// engine's sink (mLastError is the frame's while one runs; a free-running frame writes it on the render thread). The
// frame's faults are the snapshot's (frameErrors, refusals). On the engine's side (a request's question, a fixture
// that drives the engine directly) it is the calls' string, as it always was.
const std::string &OgreEngine::lastError() const {
    if (hostPosts()) return mHostError;
    return mLastError;
}

void OgreEngine::noteHostError(const std::string &why) { mHostError = why; }

void OgreEngine::moveErrorToHost() {
    if (mLastError.empty()) return;
    mHostError = mLastError;
    mLastError.clear();
}

std::string OgreEngine::takeLastError()
{
    std::string taken;
    // Swap, don't copy-then-clear: every OgreScene and OgreView holds `std::string &mError` bound to mLastError, so
    // that object must stay put — swapping its contents is fine, reseating it would not be.
    if (hostPosts()) taken.swap(mHostError);
    else taken.swap(mLastError);
    return taken;
}

// ---- Simulation clock (PARTICLES_FX2_SPEC.md; ENGINEERING_DEBT_SPEC A4.2) --
// SceneManager::updateSceneGraph feeds the particle manager
// `ControllerManager::getFrameTimeSource()->getValue()` — one value, shared by
// every scene in the process. There is no per-scene or per-view delta to hook,
// which is why the header says the verb is process-wide and means it.
//
// FrameTimeControllerValue has two modes and they cancel each other inside
// Ogre (OgrePredefinedControllers.cpp:80-95): setTimeFactor zeroes mFrameDelay
// (wall clock x factor) and setFrameDelay zeroes mTimeFactor (a constant
// delta per frame). Since A4.2 the engine lives in the second mode for its
// whole life — set at boot (init, below) and only ever re-set here — so the
// wall clock is never consulted and a frame delay of 0 means "frozen", not
// "back to the wall clock". A time factor never existed as a host concept:
// the scene's particle time scale is multiplied into the delta by the host.

void OgreEngine::applyFrameDelta(float seconds) {
    JAH_TRY {
        Ogre::ControllerManager::getSingleton().setFrameDelay(std::max(0.0f, seconds));
    } JAH_CATCH(mLastError, );
}

// ---------------------------------------------------------------------------
// THE FRAME INPUT (ENGINE-UPGRADE-1 U0 3a; FrameInput in Types.h). submitFrame MERGES into the render loop's
// hand-over slot (RenderThread::submit, ENGINE-UPGRADE-U3); the frame's head DRAINS the record the hand-over took —
// every value a frame depends on lands here, in one place and one order: the frame's own fields, each scene's
// environment, the engine-wide shadow pair, then each view.

void OgreEngine::submitFrame(FrameInput in) { mRenderThread.submit(std::move(in)); }
void OgreEngine::post(Scene *scene, SceneMessage &&message) { mRenderThread.post(scene, std::move(message)); }

// THE REQUEST DOOR (U3 stage 3): the question runs on the engine's side — its reads are the engine's own, never a
// host's synchronous read — inline today; stage 4 moves it onto the render thread.
void OgreEngine::request(std::function<void(Engine &)> question, std::function<void()> done) {
    // COUNTED WHEN THE HOST ASKS (FRAME-MONITOR-DOOR-1): the door's own count, never a sync getter's (a request is a
    // door — host.sync_getters' law stays 0); a request made on the engine's side (a fixture's nested one) is not.
    if (question && detail::tlsEngineSide == 0) mRenderThread.noteRequest();
    // ANSWERED BETWEEN FRAMES ON THE RENDER THREAD (U3 stage 4b): the render thread runs one task at a time, so the
    // question never meets a frame; `done` runs on the asker's thread once it has its answer.
    // ...AFTER WHAT THE HOST ALREADY SENT (U3 stage 4a): a request is answered on the state the host's earlier calls
    // made — the pending record's structure lands first (a view it just created exists to be asked about).
    if (question) mRenderThread.runOn([&] {
        EngineSideScope engineSide;
        if (mHostRecords) applyPendingEngineMessages();
        question(*this);
        if (mHostRecords) mRenderThread.republishEngine();   // what it applied is in the next read (U3 stage 4b)
    });
    if (done) done();
}

namespace {
/// Apply `want` when it differs from what was last applied from a record (`last`); count it.
template <typename T, typename Apply>
void applyIfChanged(const std::optional<T> &want, std::optional<T> &last, unsigned &applied, Apply &&apply) {
    if (!want) return;
    if (last && *last == *want) return;
    apply(*want);
    last = want;
    ++applied;
}
}   // namespace

// THE STRUCTURAL MESSAGES (ENGINE-UPGRADE-U3 stage 3): create / destroy / attach / material ... in emission order,
// before the node records that may name what they create — the order a sync's direct commands ran in before its record
// drained. The host's structure-edit brackets ride the stream as messages of their own. THEN THE DOCUMENT'S NODES
// (ENGINE-UPGRADE-U2, the extract): every pose, parent and class the document wrote lands before the change set names
// the moved subtrees to the scans.
namespace {
/// ONE TIMED PART OF THE DRAIN (ENGINE-UPGRADE-U4 stage 1): the span added to its FrameInputStatus figure and, while a
/// capture runs, filed as a monitor stage (exclusive of the parts nested in it). The drain runs before the frame's
/// monitor record opens, so its stages bank and the frame that opens right after adopts them (FrameMonitor::stage).
struct DrainPart {
    double &acc;
    monitor::Stage stage;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    DrainPart(double &a, const char *name) : acc(a), stage(name) {}
    ~DrainPart() { acc += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
    DrainPart(const DrainPart &) = delete;
    DrainPart &operator=(const DrainPart &) = delete;
};
}   // namespace

void OgreEngine::applySceneStream(OgreScene *sc, const std::vector<SceneMessage> &messages,
                                  const std::vector<NodeRecord> &nodes,
                                  unsigned long long stepSerial) {
    // A REFUSAL IS ITS MESSAGE'S (decision 34): a named message is answered either way, an unnamed one when refused
    // (token 0); the call's reason goes into the answer and the engine's error string is handed back as it was, so
    // the frame's error stays the frame's own fault.
    for (const SceneMessage &m : messages) applySceneMessage(sc, m);
    // THE DRAIN'S TWO CHILDREN HERE (U4 stage 3, decision 11 f): `.records` the node records alone, `.interpolate`
    // the free frame's interpolation and the reflector arm (timed only inside the drain; a request's stream is not).
    unsigned n = 0;
    {
        std::unique_ptr<DrainPart> part;
        if (mInDrain) part.reset(new DrainPart(mFrameInputStatus.drainRecordsMicros, "engine.drain.records"));
        n = sc->applyNodeRecords(nodes, stepSerial);
    }
    {
        std::unique_ptr<DrainPart> part;
        if (mInDrain) part.reset(new DrainPart(mFrameInputStatus.drainInterpolateMicros, "engine.drain.interpolate"));
        // A FREE FRAME'S ONE WRITE PER NODE (worth-a-look 4), here — before the reflector arm and the change set read
        // a pose, so both see what the frame draws (decision 56).
        if (mDeferNodeWrites) interpolateScene(sc);
        sc->armDeferredReflectors();   // a reflector asked for this frame is measured at its landed pose
    }
    mFrameInputStatus.nodeRecords += n;
    for (const NodeRecord &r : nodes) if (r.fields & NodeRecord::Trs) ++mFrameInputStatus.nodeTransforms;
    mFrameInputStatus.nodeRecordsTotal += n;
}

void OgreEngine::applySceneMessage(OgreScene *sc, const SceneMessage &m) {
    ++mFrameInputStatus.messages;
    ++mFrameInputStatus.messagesTotal;
    if (!m.apply) return;
    const std::string before = mLastError;
    const bool refuse = mArms.on(ArmId::MessagesRefuse);   // the test door (decision 44's row)
    if (refuse) mLastError = "refused by the messages.refuse arm";
    bool ok = false;
    // A MESSAGE THAT THROWS IS REFUSED, ITS COMPLETION CARRYING THE THROW (worth-a-look 2): never a drain cut short.
    try { ok = !refuse && m.apply(*sc); }
    catch (Ogre::Exception &x) { mLastError = describeOgreFailure(x); ok = false; }
    catch (std::exception &x) { mLastError = std::string("engine: ") + x.what(); ok = false; }
    const std::string why = ok ? std::string() : mLastError != before ? mLastError : std::string("refused");
    if (!ok) {
        ++mFrameInputStatus.messagesRefused;
        mRefusals.push_back({ mPublishedFrames + 1, why });   // the frame this drain belongs to
    }
    if (m.token || !ok) sc->noteCompletion(m.token, ok, why);
    mLastError = before;
}

// THE ENGINE'S STREAM (U3 stage 4a, decision 39 c): each message applied on the engine's side, its refusal its own
// completion, the engine's error string handed back as it was.
void OgreEngine::applyEngineMessage(const EngineMessage &m) {
    {
        ++mFrameInputStatus.messages;
        ++mFrameInputStatus.messagesTotal;
        if (!m.apply) return;
        const std::string before = mLastError;
        bool ok = false;
        try { ok = m.apply(*this); }   // a throw is the message's refusal, its completion carrying it (worth-a-look 2)
        catch (Ogre::Exception &x) { mLastError = describeOgreFailure(x); ok = false; }
        catch (std::exception &x) { mLastError = std::string("engine: ") + x.what(); ok = false; }
        const std::string why = ok ? std::string() : mLastError != before ? mLastError : std::string("refused");
        if (!ok) {
            ++mFrameInputStatus.messagesRefused;
            mRefusals.push_back({ mPublishedFrames + 1, why });
        }
        if (m.token || !ok) {
            mEngineCompletions.push_back({ ++mEngineCompletionSeq, m.token, ok, why, mPublishedFrames + 1 });
            mEngineCompletionsDirty = true;
        }
        mLastError = before;
    }
}

// A JOIN APPLIES WHAT THE HOST ALREADY SENT (U3 stage 4a): the pending record's structure — the engine's stream,
// every scene's commands, every view's fields, in the one order — lands before the join's own work, in the drain's
// order; the frame's own fields (its cause, its delta) stay for the frame.
void OgreEngine::applyPendingEngineMessages() {
    FrameInput pending;
    bool any = false;
    mRenderThread.withPending([&](FrameInput &rec, bool a) {
        any = a;
        pending.engineMessages.swap(rec.engineMessages);
        pending.scenes.swap(rec.scenes);
        pending.views.swap(rec.views);
        pending.stepSerial = rec.stepSerial;
        pending.stepPeriod = rec.stepPeriod;
    });
    if (pending.engineMessages.empty() && pending.scenes.empty() && pending.views.empty()) return;
    EngineSideScope engineSide;
    drainFrameInput(pending, any);
    // ...AND ITS GI PLAN, AT ONCE (U4 decision 13.6): a join runs no frame, so no `engine.plan` would run the plan the
    // drain stashed before the request it serves reads the answer (a re-solve after world.refreshGi()). FRAME-LESS:
    // no scene graph ran, nothing is current — the plan's geometry term takes the recomputing reads (boxesCurrent is
    // false here), harmless at a join: the next frame's updateSceneGraph writes the SoA values over the scalars before
    // anything draws (the parity miss was a recompute that persisted INTO a draw).
    for (auto &s : mScenes) s->runStashedGiPlan();
}

// THE PRESENTATION CLOCK (U3 stage 5), in the document's step time and never a wall clock: a FREE frame shows the
// newest record's nodes at alpha = (P - the step before the newest) / the step period, P held inside that interval
// (render one step behind: Tutorial06 GraphicsGameState's rule), then P advances by the display period the driver
// supplies; a LOCKSTEP frame draws cur exactly (alpha 1 — every test, every hash) and P stands at the newest step.
// THE WAITS BEFORE THE HAND-OVER (ENGINE-UPGRADE-U3 decision 65): the free loop blocks HERE — the runtime's
// xrWaitFrame (taken early; the frame's beginFrame uses its result), or the test door vr.fakeRuntimePace standing in
// for one — and only then takes the host's record: the record is the newest the host could submit during the block.
void OgreEngine::waitBeforeFrame() {
    mFakeRuntimeWaited = false;
    if (mVrSession) { vrSessionWaitFrameEarly(mVrSession); return; }
    if (mArms.value(ArmId::FakeRuntimePace) > 0.0) {
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(mArms.value(ArmId::FakeRuntimePace)));
        const auto now = std::chrono::steady_clock::now();
        if (mFakeRuntimeNext.time_since_epoch().count() == 0 || mFakeRuntimeNext + period < now) mFakeRuntimeNext = now;
        std::this_thread::sleep_until(mFakeRuntimeNext);
        mFakeRuntimeNext += period;
        mFakeRuntimeWaited = true;
    }
}

// THE RECORD/SWAP SPLIT (decision 65): a free frame's work is recorded and submitted, the present is next. The frame
// PUBLISHES HERE — the host's tick it posts runs while a blocking present (or the test door view.fakePresentBlock,
// standing in for one) holds this thread — and the close's latches follow as the engine's republish.
void OgreEngine::onRecordSplit() {
    if (!mFreeFrame || !mRenderThread.inFreeFrame()) return;
    mRenderThread.publishAtSplit();
    const double blockMs = mArms.value(ArmId::FakePresentBlock);
    if (blockMs > 0.0) {
        const long long since = mRenderThread.frameInFlightSinceNs();
        const auto start = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(since));
        std::this_thread::sleep_until(start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                  std::chrono::duration<double, std::milli>(blockMs)));
    }
}

void OgreEngine::beginPresentation() {
    if (mPresentBegun) return;
    mPresentBegun = true;
    // THE GRID'S STEP (decision 55): the simulation clock's period the record carries — never the particles' delta.
    const double step = mStepPeriod > 0.0 ? mStepPeriod : 1.0 / 60.0;
    const double hi = double(mNewestStep) * step, lo = hi - step;
    if (!mFreeFrame) { mPresentTime = hi; return; }
    if (mPresentTime < lo) mPresentTime = lo;
    if (mPresentTime > hi) mPresentTime = hi;
}

void OgreEngine::interpolateScene(OgreScene *sc) {
    if (!sc || sc->mInterpFrame == mPresentFrame) return;   // once a frame
    sc->mInterpFrame = mPresentFrame;
    beginPresentation();
    if (!mFreeFrame) { mPresentAlpha = 1.0f; sc->restoreInterpolated(); return; }   // lockstep: cur (written only where owed)
    const double step = mStepPeriod > 0.0 ? mStepPeriod : 1.0 / 60.0;
    const double sLo = (double(sc->interpolationStep()) - 1.0) * step;
    const float alpha = float((mPresentTime - sLo) / step);
    if (!sc->mInterpNodes.empty()) mPresentAlpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    sc->interpolate(alpha);
}

void OgreEngine::interpolateFrame() {
    beginPresentation();
    for (auto &s : mScenes) interpolateScene(s.get());
    if (!mFreeFrame) return;
    const double step = mStepPeriod > 0.0 ? mStepPeriod : 1.0 / 60.0;
    // The display's period the driver sends (a session's predicted one while it runs); under Unlimited none is sent
    // and the measured start-to-start interval of the free frames advances the clock (worth-a-look 3).
    double period = double(mRenderThread.displayPeriod());
    if (!(period > 0.0)) period = double(mRenderThread.measuredPeriod());
    mPresentTime += period > 0.0 ? period : step;
    mLastAlphaPresent = mPresentTime;
}


double OgreEngine::freeDrainBudgetMs() const {
    const double periodMs = mRenderThread.displayPeriod() > 0.0f ? double(mRenderThread.displayPeriod()) * 1000.0
                                                                  : kFreeDrainUnpacedPeriodMs;
    const double lastOtherMs = std::max(0.0, double(mRenderThread.lastFrameWorkNs()) / 1e6 - mLastFreeDrainMs);
    return std::min(std::max(periodMs - lastOtherMs, kFreeDrainShare * lastOtherMs), kFreeDrainCeilingPeriods * periodMs);
}


void OgreEngine::drainFrameInput(FrameInput &handed, bool any, bool budgeted) {
    // THE DRAIN IS A STAGE (U4 stage 1): `engine.drain` and its three parts, and the same spans on the snapshot
    // (FrameInputStatus::drainMicros and its three parts) — frame-counted, never sampled.
    mFrameInputStatus.drainMicros = 0.0;
    mFrameInputStatus.drainNameMicros = 0.0;
    mFrameInputStatus.drainRecordsMicros = 0.0;
    mFrameInputStatus.drainInterpolateMicros = 0.0;
    DrainPart whole(mFrameInputStatus.drainMicros, "engine.drain");
    // THE COUNTS (decision 15): what ran outside a drain is dropped; the drain's own are summed at its end.
    mFrameInputStatus.drainRecordLookups = mFrameInputStatus.drainNameVisits = mFrameInputStatus.drainNameLookups = 0u;
    for (auto &q : mScenes) q->takeDrainCounts(nullptr);
    struct DrainCounts {
        OgreEngine &e;
        ~DrainCounts() { for (auto &q : e.mScenes) q->takeDrainCounts(&e.mFrameInputStatus); }
    } drainCounts{ *this };
    mInDrain = true;
    struct InDrain { bool &f; ~InDrain() { f = false; } } inDrain{ mInDrain };
    ++mFrameInputStatus.drained;
    mFrameInputStatus.fieldsApplied = 0;
    mFrameInputStatus.viewFieldsApplied = 0;
    mFrameInputStatus.changesMoved = 0;
    mFrameInputStatus.changesLights = 0;
    mFrameInputStatus.nodeRecords = 0;
    mFrameInputStatus.nodeTransforms = 0;
    mFrameInputStatus.messages = 0;
    mFrameInputStatus.messagesCarried = 0;
    if (!any) { ++mFrameInputStatus.drainedEmpty; return; }
    FrameInput in = std::move(handed);
    unsigned applied = 0;
    if (in.stepSerial && *in.stepSerial > mNewestStep) mNewestStep = *in.stepSerial;   // the presentation's grid
    if (in.stepPeriod && *in.stepPeriod > 0.0f) mStepPeriod = double(*in.stepPeriod);   // the grid's step (decision 55)

    // THE ONE ORDER (decision 46; stage 3's FIFO law across every stream): the engine's messages, every scene's
    // messages and every view's binding, in the order the host posted them (their sequence). A create precedes the
    // commands posted after it; a view unbound before its scene's destroy is unbound first. The scenes' node records,
    // changes and fields, and the views' other fields, follow — the record's state after its structure.
    // EACH STREAM IS ALREADY IN ITS ORDER (the slot appends, and a carried remainder goes ahead of the newer submits),
    // so the one order is their MERGE by sequence, taken lazily — a free frame that stops at its budget never touches
    // the backlog it leaves (BUGS-7: a sort of the whole record each frame cost a 30,000-message backlog
    // its budget). A tie keeps the streams' order: the engine's, the scenes' in the record's order, the views'.
    {
        std::vector<size_t> viewOrder;   // the view bindings, by their sequence (few)
        for (size_t v = 0; v < in.views.size(); ++v) if (in.views[v].scene) viewOrder.push_back(v);
        std::stable_sort(viewOrder.begin(), viewOrder.end(),
                         [&](size_t x, size_t y) { return in.views[x].sceneSeq < in.views[y].sceneSeq; });
        size_t nextEngine = 0, nextView = 0;
        std::vector<size_t> nextScene(in.scenes.size(), 0u);
        // THE FREE FRAME'S BUDGET (freeDrainBudgetMs): checked before each op after the first.
        const double budgetMs = budgeted ? freeDrainBudgetMs() : 0.0;
        const auto budgetStart = std::chrono::steady_clock::now();
        unsigned long long cutSeq = 0;
        bool cut = false;
        for (unsigned opsApplied = 0;; ++opsApplied) {
            // the stream whose next op is the oldest
            constexpr unsigned long long kNone = ~0ull;
            unsigned long long best = kNone;
            int kind = -1;   // 0 the engine's, 1 a scene's, 2 a view's binding
            size_t which = 0;
            if (nextEngine < in.engineMessages.size()) { best = in.engineMessages[nextEngine].seq; kind = 0; }
            for (size_t j = 0; j < in.scenes.size(); ++j)
                if (nextScene[j] < in.scenes[j].messages.size() && in.scenes[j].messages[nextScene[j]].seq < best) {
                    best = in.scenes[j].messages[nextScene[j]].seq; kind = 1; which = j;
                }
            if (nextView < viewOrder.size() && in.views[viewOrder[nextView]].sceneSeq < best) {
                best = in.views[viewOrder[nextView]].sceneSeq; kind = 2;
            }
            if (kind < 0) break;   // the whole structure applied
            if (budgeted && opsApplied > 0 &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - budgetStart).count() >=
                    budgetMs) {
                cut = true;
                cutSeq = best;
                break;
            }
            if (kind == 0) {
                applyEngineMessage(in.engineMessages[nextEngine++]);
            } else if (kind == 1) {
                const FrameInputScene &part = in.scenes[which];
                const SceneMessage &m = part.messages[nextScene[which]++];
                OgreScene *sc = nullptr;   // resolved now: a message earlier in the order may have destroyed it
                for (auto &q : mScenes) if (static_cast<Scene *>(q.get()) == part.scene) { sc = q.get(); break; }
                if (sc) applySceneMessage(sc, m);
            } else {
                const FrameInputView &part = in.views[viewOrder[nextView++]];
                OgreView *v = nullptr;
                for (auto &q : mViews) if (static_cast<View *>(q.get()) == part.view) { v = q.get(); break; }
                OgreScene *bind = static_cast<OgreScene *>(*part.scene);
                if (v && v->boundOgreScene() != bind) {
                    if (v->boundOgreScene()) v->setScene(nullptr);
                    if (bind) v->setScene(bind);
                }
            }
        }
        // THE REMAINDER, CARRIED (BUGS-7): every op from the cut on — the one order is a sequence, so the
        // remainder is each stream's suffix — and the STATE that follows that structure: a scene whose stream was cut
        // keeps its whole part (its node records, change set and fields name nodes its carried messages create), a
        // scene not created yet (its create rides a carried engine message) keeps its part, a view whose binding was
        // cut keeps its part. Everything else of the record applies now: the frame's own fields, the other scenes, the
        // views (the camera follows the hand while a load streams). Back into the slot AHEAD of the newer submits.
        if (cut) {
            FrameInput carry;
            unsigned carried = unsigned(in.engineMessages.size() - nextEngine);
            in.engineMessages.erase(in.engineMessages.begin(), in.engineMessages.begin() + std::ptrdiff_t(nextEngine));
            carry.engineMessages = std::move(in.engineMessages);
            const bool engineCarried = !carry.engineMessages.empty();
            for (size_t j = 0; j < in.scenes.size(); ++j) {
                FrameInputScene &part = in.scenes[j];
                bool known = false;
                for (auto &q : mScenes) if (static_cast<Scene *>(q.get()) == part.scene) { known = true; break; }
                const bool rest = nextScene[j] < part.messages.size();
                if (!rest && (known || !engineCarried)) continue;   // applied whole (or a dead scene's, dropped below)
                carried += unsigned(part.messages.size() - nextScene[j]);
                part.messages.erase(part.messages.begin(), part.messages.begin() + std::ptrdiff_t(nextScene[j]));
                carry.scenes.push_back(std::move(part));
                part.scene = nullptr;   // moved: the environment loop below skips it
            }
            in.scenes.erase(std::remove_if(in.scenes.begin(), in.scenes.end(),
                                           [](const FrameInputScene &q) { return q.scene == nullptr; }),
                            in.scenes.end());
            for (FrameInputView &part : in.views)
                if (part.scene && part.sceneSeq >= cutSeq) {
                    carry.views.push_back(std::move(part));
                    part.view = nullptr;
                }
            in.views.erase(std::remove_if(in.views.begin(), in.views.end(),
                                          [](const FrameInputView &q) { return q.view == nullptr; }),
                           in.views.end());
            mFrameInputStatus.messagesCarried = carried;
            ++mFrameInputStatus.drainsCut;
            mRenderThread.carryForward(std::move(carry));
        }
    }

    // THE FRAME'S OWN FIELDS. cause / pace are this frame's (renderOneFrame consumes and resets them); the
    // delta persists; the epoch and the gesture are facts the frame reads.
    if (in.frameDelta && *in.frameDelta != appliedFrameDelta()) { applyFrameDelta(*in.frameDelta); ++applied; }
    if (in.cause) mNextFrameCause = *in.cause;

    // EACH SCENE'S ENVIRONMENT, in the order the host used to push it.
    for (const FrameInputScene &part : in.scenes) {
        OgreScene *sc = nullptr;
        for (auto &q : mScenes) if (static_cast<Scene *>(q.get()) == part.scene) { sc = q.get(); break; }
        if (!sc) continue;   // destroyed before its frame
        applySceneStream(sc, {}, part.nodes, in.stepSerial.value_or(0ull));   // its messages ran in the one order above
        // THE CHANGE SET NEXT (U0 3b): what the document wrote, before anything of the frame reads a box.
        mFrameInputStatus.changesMoved += unsigned(part.changes.moved.size());
        if (!part.changes.complete) ++mFrameInputStatus.changesIncomplete;
        {
            DrainPart name(mFrameInputStatus.drainNameMicros, "engine.drain.name");
            mFrameInputStatus.changesLights += sc->applyChangeSet(part.changes);
        }
        // THE GESTURE HOLDS UNTIL THE SCENE'S NEXT RECORD THAT CARRIES ONE (U0 3c): the host that has the user's hand
        // says so every frame it builds (the viewport); a frame some other host renders in between (a thumbnail, a
        // preview) carries none for this scene and must not end the drag under the hand.
        if (part.gesture) sc->mGesture = *part.gesture;
        FrameInputScene &last = sc->mFrameApplied;
        bool giPushed = false;
        if (part.reassert) {
            const std::optional<GiParams> gi = last.gi;   // GI is never re-pushed (P10)
            last = FrameInputScene();
            last.gi = gi;
            for (auto &q : mViews)   // ...and every view showing the scene (the view loop below runs after this one)
                if (q->boundOgreScene() == sc) { q->mFrameApplied = FrameInputView(); q->mFrameAppliedW = q->mFrameAppliedH = 0; }
        }
        applyIfChanged(part.shadows, last.shadows, applied, [&](const ShadowDesc &v) { sc->setShadowSettings(v); });
        applyIfChanged(part.environmentLight, last.environmentLight, applied,
                       [&](const Colour &v) { sc->setEnvironmentLight(v); });
        applyIfChanged(part.fog, last.fog, applied, [&](const FogDesc &v) { sc->setFog(v); });
        applyIfChanged(part.heightFog, last.heightFog, applied, [&](const HeightFogDesc &v) { sc->setHeightFog(v); });
        applyIfChanged(part.rayTracing, last.rayTracing, applied, [&](RayTracingMode v) { sc->setRayTracing(v); });
        applyIfChanged(part.sunContact, last.sunContact, applied,
                       [&](const SunContactDesc &v) { sc->setSunContact(v); });
        applyIfChanged(part.pixelTolerance, last.pixelTolerance, applied,
                       [&](float v) { sc->setPixelTolerance(v); });
        if (part.gi) {
            // THE TUNING ROW FIRST, then the configuration (GiParams: operator== leaves the tuning out, so a
            // tuning-only change is setGiTuning — nothing torn down — and a configuration change is a rebuild).
            const GiParams &g = *part.gi;
            if (!last.gi) {
                sc->setGlobalIllumination(g); last.gi = g; ++applied; giPushed = true;
            } else {
                if (!g.giTuningEqual(*last.gi)) {
                    sc->setGiTuning(g);
                    last.gi->cards = g.cards;
                    last.gi->cardBudgetTexels = g.cardBudgetTexels;
                    ++applied;
                }
                if (g != *last.gi) { sc->setGlobalIllumination(g); last.gi = g; ++applied; giPushed = true; }
            }
        }
        applyIfChanged(part.planar, last.planar, applied,
                       [&](const PlanarReflectionParams &v) { sc->setPlanarReflections(v); });
        applyIfChanged(part.sky, last.sky, applied, [&](const SkyDesc &v) { sc->setSky(v); });
        applyIfChanged(part.shaderTime, last.shaderTime, applied, [&](float v) { sc->setShaderTime(v); });
        applyIfChanged(part.grid, last.grid, applied, [&](const GridDesc &v) { static_cast<Scene *>(sc)->setGrid(v); });
        // THE ONE GI SCHEDULER (U0 3c; OgreGi.cpp planGi): once per record that carries the scene's GI, after every
        // field of it has landed — the configuration it may have just applied included.
        // ...AND IT RUNS AFTER THE SCENE GRAPH (U4 stage 3, decision 8): what it reads of the record is stashed here,
        // the plan itself (its geometry term over the world boxes) runs in the frame's `engine.plan`.
        if (part.gi) sc->stashGiPlan(part, giPushed);
    }

    mFrameInputStatus.gestureActive = false;
    for (auto &q : mScenes) mFrameInputStatus.gestureActive = mFrameInputStatus.gestureActive || q->mGesture.active;

    // THE ENGINE-WIDE SHADOW PAIR (after the scenes' shadow settings, as the host pushed them).
    if (in.refreshShadows) { refreshShadows(); ++applied; }
    if (in.shadowMapBudget && *in.shadowMapBudget > 0 && shadowMapBudget() != *in.shadowMapBudget) {
        setShadowMapBudget(*in.shadowMapBudget);
        ++applied;
    }

    // EACH VIEW: its background and chain first, its camera last (the camera's letterbox reads the background).
    unsigned viewApplied = 0;
    for (const FrameInputView &part : in.views) {
        OgreView *v = nullptr;
        for (auto &q : mViews) if (static_cast<View *>(q.get()) == part.view) { v = q.get(); break; }
        if (!v) continue;
        FrameInputView &last = v->mFrameApplied;
        if (part.scene) ++viewApplied;   // the binding ran in the one order above (decision 46)
        if (part.size) { v->resize(part.size->first, part.size->second); ++viewApplied; }   // before the camera (U3 4a)
        applyIfChanged(part.background, last.background, viewApplied, [&](const Colour &c) { v->applyBackground(c); });
        applyIfChanged(part.shadows, last.shadows, viewApplied, [&](bool on) { v->applyShadows(on); });
        applyIfChanged(part.sampleCount, last.sampleCount, viewApplied, [&](unsigned n) { v->setSampleCount(n); });
        applyIfChanged(part.postFx, last.postFx, viewApplied, [&](const PostFxDesc &fx) { v->applyPostFx(fx); });
        if (part.camera) {
            // A RESIZE RE-APPLIES THE CAMERA: the framing hold and the ortho window are derived from the target's
            // size inside setCamera, so the same description on a new size is a change.
            const bool sizeMoved = v->width() != v->mFrameAppliedW || v->height() != v->mFrameAppliedH;
            if (!last.camera || !(*last.camera == *part.camera) || sizeMoved) {
                v->setCamera(*part.camera);
                last.camera = part.camera;
                v->mFrameAppliedW = v->width();
                v->mFrameAppliedH = v->height();
                ++viewApplied;
            }
        }
        if (part.cameraNode && v->cameraNode() != *part.cameraNode) { v->setCameraNode(*part.cameraNode); ++viewApplied; }
        // THE HOST'S SETTERS (U3 stage 4a, decision 39 a): applied here, on the engine's side, by the setters' own
        // engine-side bodies (each keeps its same-value check).
        if (part.enabled) { v->setEnabled(*part.enabled); ++viewApplied; }
        if (part.offscreenContract) { v->setOffscreenContract(*part.offscreenContract); ++viewApplied; }
        if (part.helpersVisible) { v->setHelpersVisible(*part.helpersVisible); ++viewApplied; }
        if (part.vrHelpersVisible) { v->setVrHelpersVisible(*part.vrHelpersVisible); ++viewApplied; }
        if (part.lodHysteresisOffscreen) { v->setLodHysteresisOffscreen(*part.lodHysteresisOffscreen); ++viewApplied; }
        if (part.asyncShaders) { v->setAsyncShaders(*part.asyncShaders); ++viewApplied; }
        if (part.overlay) { v->setOverlay(*part.overlay); ++viewApplied; }
        if (part.pip) { v->setPip(*part.pip); ++viewApplied; }
        if (part.resetExposure) { v->resetExposureHistory(); ++viewApplied; }
        if (part.seedExposure) { v->seedExposureHistory(*part.seedExposure); ++viewApplied; }
    }
    mFrameInputStatus.viewFieldsApplied = viewApplied;
    mFrameInputStatus.fieldsApplied = applied + viewApplied;
}

// THE ENGINE'S HALF OF THE SNAPSHOT (ENGINE-UPGRADE-U3 stage 1; RenderThread::publish): every per-frame state the
// document side reads today through a synchronous getter, read once here at the frame's close. The monitor's status
// is read only while a capture runs (its GPU half asks the render system by an attribute a production build answers
// with a throw — never once a frame); at Off it is the absence the getter reports, the undrained tail included.
void OgreEngine::fillPublished(PublishedEngineFrame &out) const {
    out.presentAlpha = mPresentAlpha;
    out.presentFree = mFreeFrame;
    out.input = mFrameInputStatus;
    out.vrSessionLive = mVrSession != nullptr;
    // THE REFUSALS OF THE LAST kFrameErrorsKept FRAMES (decision 34), for the error pump.
    while (!mRefusals.empty() && mRefusals.front().frame + PublishedEngineFrame::kFrameErrorsKept <= mPublishedFrames)
        mRefusals.pop_front();
    out.refusals.assign(mRefusals.begin(), mRefusals.end());
    while (!mEngineCompletions.empty() &&
           mEngineCompletions.front().frame + PublishedEngineFrame::kFrameErrorsKept <= mPublishedFrames) {
        mEngineCompletions.pop_front();
        mEngineCompletionsDirty = true;
    }
    if (mEngineCompletionsDirty || !mEngineCompletionsPublished) {
        mEngineCompletionsPublished = std::make_shared<std::vector<PublishedFrame::Completion>>(
            mEngineCompletions.begin(), mEngineCompletions.end());
        mEngineCompletionsDirty = false;
    }
    out.completions = mEngineCompletionsPublished;
    out.vr = vrStatus();
    // THE REST OF THE VR DOOR (stage 6): the host's VR getters read these.
    out.vrState = vrStateNow();
    out.vrInfo = mVrInfo;
    for (unsigned h = 0; h < VrHandCount; ++h) {
        VrPose joints[kVrHandJointCount];
        const unsigned n = vrHandJoints(int(h), joints, kVrHandJointCount);
        out.vrJoints[h].assign(joints, joints + n);
    }
    {
        VrBindingBlock blocks[kVrBindingBlockMax];
        const unsigned n = mVrSession ? vrSessionBindingBlocks(mVrSession, blocks, kVrBindingBlockMax) : 0u;
        out.vrBindingBlocks.assign(blocks, blocks + n);
    }
    out.streaming = streamingWork();
    out.shaders = asyncShaderStats();
    out.hasEnabledViews = hasEnabledViews();
    out.paceOwesWork = framePaceOwesWork();
    out.views.clear();
    for (const auto &v : mViews)
        if (v) {
            PublishedEngineFrame::ViewFrames f;
            f.id = v->id();
            f.name = v->name();
            f.presented = v->framesPresented();
            f.blank = v->blankFramesPresented();
            f.width = v->width();
            f.height = v->height();
            f.sampleCount = v->sampleCount();
            f.pipGeneration = v->pipGeneration();
            f.workspaceGeneration = v->workspaceGeneration();
            f.globalsPushes = v->globalsPushes();
            f.postFx = v->postFx();
            out.views.push_back(std::move(f));
        }
    out.deviceLost = deviceLost();
    out.gpuFault = gpuFault();
    out.monitor = monitorStatus();   // its own answer with the monitor off too (the tail's frames, the GPU timing)
    // THE SHADOW ATLAS AND THE RENDERER'S FIGURES (U3 stage 3): every frame; their counters measured only while
    // subscribed (the shadow pass counters by shadowLeased, the geometry counting below).
    out.shadow = shadowStatus();
    const bool renderLeased = mRenderLeaseUntil != 0 && mPublishedFrames <= mRenderLeaseUntil;
    if (!renderLeased) mRenderLeaseUntil = 0;
    out.render = RenderStats();
    renderStats(out.render);
    syncMetricsRecording(renderLeased || mHudStatsThisFrame || mMonitor != nullptr);   // a capture counts too
}

bool OgreEngine::pendingGiConfiguration(const Scene *scene) const {
    return mRenderThread.withPending([&](const FrameInput &pending, bool any) {
        if (!any) return false;
        for (const FrameInputScene &part : pending.scenes) {
            if (part.scene != scene || !part.gi) continue;
            // ...against the configuration the last frame APPLIED, as it published it (F-B: the submitting side's
            // computation over its own pending record and the snapshot).
            const std::optional<GiParams> &applied = scene->published().giApplied;
            return !applied || *part.gi != *applied;
        }
        return false;
    });
}

float OgreEngine::appliedFrameDelta() const {
    return float(Ogre::ControllerManager::getSingleton().getFrameDelay());
}

// Shadow filter and resolution (and the atlas rebuild they trigger) live in
// OgreShadow.cpp, beside the definition builder they drive.

void OgreEngine::setShadowMeshOptimization(bool on) {
    mHostShadowMeshOptimization = on;   // the host's record
    if (postedAsMessage([=](OgreEngine &e) { e.setShadowMeshOptimization(on); return true; })) return;   // U3 4a
    JAH_HOST_CALL("Engine::setShadowMeshOptimization");
    Ogre::Mesh::msOptimizeForShadowMapping = on;
}

// ---------------------------------------------------------------------------
// RECORDED WARM-UP SETS ARE GONE (WARMUPSET-2, 2026-09-21). The three verbs
// that recorded, saved and replayed a `VertexFormatWarmUpStorage` lived here.
// A set identifies each permutation by one representative MATERIAL NAME and
// resolves that name in the NEXT process; this engine names its datablocks from
// a process-unique counter, so the names never resolved and the replay warmed
// the DEFAULT datablock instead — measured at seven "Can't find HLMS datablock"
// lines and eight never-bound shaders per warm launch. Deleted rather than
// repaired on the owner's word. The per-scene PSO precache
// (`View::warmUpShaders`) and the persistent shader cache are what warm this
// renderer, and neither went anywhere.

// ---------------------------------------------------------------------------
// THE BACKGROUND SHADER COMPILER (ASYNC-SHADERS-1; the fork's HlmsAsyncCompiler).
//
// The service runs from the moment the Hlms exist; WHO uses it is decided per view
// (View::setAsyncShaders — the editor's view after a project is open). Its threads are
// niced by 10 so a compile never competes with the frame's SceneManager workers.
//
// THE PLACEHOLDER is a neutral grey PBS material, metallic workflow: albedo 0.18
// (linear; the 18 % card every exposure in this engine is calibrated on —
// `greyCardFilmInput()` maps it to itself), not metallic, rough 0.8 (a near-
// Lambertian surface that takes the scene's light and shadow without a highlight
// that would pretend to be the real material). An object waiting for its shader is
// drawn with it, its own geometry, lit and shadowed like the scene; its shadow is the
// placeholder's opaque caster.
void OgreEngine::startAsyncShaders() {
    if (!mRoot || mHeadless) return;
    Ogre::HlmsManager *hm = mRoot->getHlmsManager();
    if (auto *pbs = static_cast<Ogre::HlmsPbs *>(hm->getHlms(Ogre::HLMS_PBS))) {
        if (!pbs->getAsyncPlaceholderDatablock()) {
            const Ogre::String name = "Jahshaka/AsyncPlaceholder";
            auto *db = static_cast<Ogre::HlmsPbsDatablock *>(pbs->createDatablock(
                name, name, Ogre::HlmsMacroblock(), Ogre::HlmsBlendblock(), Ogre::HlmsParamVec(),
                /*visibleToManager*/ false));
            db->setWorkflow(Ogre::HlmsPbsDatablock::MetallicWorkflow);
            db->setDiffuse(Ogre::Vector3(0.18f));
            db->setMetalness(0.0f);
            db->setRoughness(0.8f);
            pbs->setAsyncPlaceholderDatablock(db);
        }
        // The Atom decode's placeholder: the same grey, through its own clone (HlmsAtom::
        // installAsyncPlaceholder says why a twin cannot simply be pointed at it).
        if (auto *atom = dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType))) {
            std::string err;
            auto *pbsPh = static_cast<Ogre::HlmsPbsDatablock *>(pbs->getAsyncPlaceholderDatablock());
            if (!atom->installAsyncPlaceholder(pbsPh, err))
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka async shaders: no Atom placeholder (" + err +
                        ") - a pending decode bucket draws nothing until it lands",
                    Ogre::LML_CRITICAL);
        }
    }
    // UNLIT HAS NO PLACEHOLDER, ON PURPOSE. Its set is the editor's furniture (the grid, the
    // gizmos, the wires, the outline, the HUD) plus billboards, and a grey opaque quad would
    // be a lie for every one of them. It still compiles in the background: a pending unlit
    // draw is skipped for the ~0.2 s its permutation takes and appears when it lands — the
    // frame is presented all the same (BUGS-7: no frame is held). (Kept in the frame
    // instead, measured: the Mirror Room and Particles samples compiled unlit permutations on
    // the UI thread after their opens.)
    hm->getAsyncCompiler()->setNumThreads(mAsyncShaderThreads, /*nice*/ 10);
}

void OgreEngine::setAsyncShaderThreads(unsigned threads) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.setAsyncShaderThreads(threads); return true; })) return;
    mAsyncShaderThreads = std::min(threads, 8u);
    if (!mRoot || mHeadless || !mHlmsRegistered) return;
    mRoot->getHlmsManager()->getAsyncCompiler()->setNumThreads(mAsyncShaderThreads, 10);
}

void OgreEngine::setAsyncShaderPlaceholdersOnly(bool on) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.setAsyncShaderPlaceholdersOnly(on); return true; })) return;
    if (!mRoot || mHeadless || !mHlmsRegistered) return;
    mRoot->getHlmsManager()->getAsyncCompiler()->setPlaceholdersOnly(on);
}

AsyncShaderStats OgreEngine::asyncShaderStats() const {
    AsyncShaderStats out;
    if (!mRoot) return out;
    const Ogre::HlmsAsyncCompiler *c = mRoot->getHlmsManager()->getAsyncCompiler();
    out.running   = c->isRunning();
    out.threads   = c->getNumThreads();
    out.pending   = c->getNumOutstanding();
    out.completed = c->getNumCompleted();
    out.failed    = c->getNumFailed();
    out.placeholderDraws = mRetiredPlaceholderDraws;
    out.pendingSkips     = mRetiredPendingSkips;
    for (const auto &sc : mScenes) {
        if (!sc || !sc->sceneManager()) continue;
        // THE VIEWS' draws: a surface-cache capture compiles in the background too, but a
        // batch that met a pending shader is discarded and never seen (SurfaceCache).
        const Ogre::RenderQueue *rq = sc->sceneManager()->getRenderQueue();
        unsigned long long capPh = 0, capSkip = 0;
        surfaceCacheAsyncDraws(sc->sceneManager(), capPh, capSkip);
        out.placeholderDraws += rq->getNumPlaceholderDraws() - capPh;
        out.pendingSkips     += rq->getNumPendingSkips() - capSkip;
        out.captureDeferredDraws += capPh + capSkip;
    }
    unsigned background = 0;
    unsigned foreground = 0;
    mShaderCache.backgroundCompiles(background, foreground);
    out.compiledInBackground = background;
    out.compiledInForeground = foreground;
    if (const Ogre::HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms())
        out.paramsMisfits = hc->getJahParamsMisfits();
    return out;
}

void OgreEngine::waitForAsyncShaders() {
    JAH_HOST_CALL("Engine::waitForAsyncShaders");
    if (!mRoot) return;
    mRoot->getHlmsManager()->getAsyncCompiler()->waitForAll();
}

ShaderCacheStats OgreEngine::shaderCacheStats() const {
    JAH_SYNC_GETTER("Engine::shaderCacheStats");
    return mShaderCache.stats(mRoot);
}

void OgreEngine::publishRenderStats(unsigned frames) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { e.publishRenderStats(frames); return true; })) return;
    mRenderLeaseUntil = frames ? mPublishedFrames + frames : 0ull;
    if (frames) syncMetricsRecording(true);   // the span's first frame counts
}

// THE GEOMETRY COUNTING FOLLOWS ITS READERS (U3 stage 3): on while a subscriber or the HUD wants it, off the frame
// nobody does — never a latch (it used to stay on for the session after the first read).
void OgreEngine::syncMetricsRecording(bool want) const {
    mHudStatsThisFrame = false;
    if (!mRoot) return;
    Ogre::RenderSystem *rs = mRoot->getRenderSystem();
    if (!rs) return;
    const bool on = rs->getMetrics().mIsRecordingMetrics;
    if (want && !on) { rs->setMetricsRecordingEnabled(true); mMetricsByUs = true; }
    else if (!want && on && mMetricsByUs) { rs->setMetricsRecordingEnabled(false); mMetricsByUs = false; }
}

bool OgreEngine::renderStats(RenderStats &out) const {
    JAH_SYNC_GETTER("Engine::renderStats");
    out = RenderStats();
    // BEFORE the early return: the advance counter is the engine's own and is
    // meaningful with no render system at all (it reads 0). A host asking
    // "did the resource bookkeeping move?" must get an answer.
    out.resourceAdvances = mResourceAdvances;
    if (!mRoot) return false;
    JAH_TRY {
        // ---- timing. Already live: Root::renderOneFrame samples FrameStats
        // itself (OgreRoot.cpp:1123), and ours is the call that drives it, so
        // there is nothing to wire. Note what this actually measures — the
        // HOST's 16 ms QTimer, not the renderer's capability (RenderStats' own
        // doc comment says so, and app.frameStats().workMs is the honest one).
        if (const Ogre::FrameStats *fs = mRoot->getFrameStats()) {
            const double rolling = fs->getRollingAverage();
            out.frameMs = rolling * 1000.0;
            out.fps     = rolling > 0.0 ? 1.0 / rolling : 0.0;
            out.lastMs  = fs->getLatestTimeSinceLast() * 1000.0;
            out.p95Ms   = fs->getPercentile95th(true) * 1000.0;
            out.p99Ms   = fs->getPercentile99th(true) * 1000.0;
            out.bestMs  = fs->getBestTime()  * 1000.0;
            out.worstMs = fs->getWorstTime() * 1000.0;
        }
        // ---- geometry. OFF by default in Ogre (OgreCommon.cpp:177) and an integer add per draw batch: on only while
        // a reader subscribes or the HUD's stats show (syncMetricsRecording, U3 stage 3). The first frame of a span
        // honestly reports metricsRecording=false with zeroed counters; every frame after it the real numbers.
        if (Ogre::RenderSystem *rs = mRoot->getRenderSystem()) {
            const Ogre::RenderingMetrics &m = rs->getMetrics();
            out.metricsRecording = m.mIsRecordingMetrics;
            if (m.mIsRecordingMetrics) {
                out.draws     = (unsigned long long)m.mDrawCount;
                out.batches   = (unsigned long long)m.mBatchCount;
                out.triangles = (unsigned long long)m.mFaceCount;
                out.vertices  = (unsigned long long)m.mVertexCount;
                out.instances = (unsigned long long)m.mInstanceCount;
                // (THE ID PASS'S SHARE is in these: its recorder adds its indirect
                // draw's counters to the render system's own, OgreAtomIdPass.cpp — as
                // its stats ring reads them: the counters of the cull recorded one
                // frame before the slot's copy, read `multiplier + 1` frames after it.)
                for (const auto &v : mViews) {
                    unsigned long long tris = 0ull;
                    unsigned surv = 0u;
                    if (v && v->isEnabled() && v->atomStats(tris, surv)) {
                        out.gpuCountLagFrames = unsigned(rs->getVaoManager()->getDynamicBufferMultiplier()) + 2u;
                        break;
                    }
                }
            }
            // THE PSO DEADLINE'S HONEST HALF (THREADING_ADOPTION_SPEC.md P4(b),
            // decision D-E(1)). Ogre can budget PSO compilation per frame and
            // turn anything that misses the deadline into a stub, so the objects
            // using it simply do not appear that frame
            // (RenderSystem::setPsoRequestsTimeout, OgreRenderSystem.h:907-935).
            // WE DELIBERATELY DO NOT USE IT: upstream's own warning is that
            // "techniques that rely on running a shader once (e.g. to fill a
            // texture) may end up uninitialized", which describes our thumbnail
            // renders, IBL cubemap generation, VCT voxelization and every
            // offscreen pixel suite — and the knob is process-wide while the
            // thing worth protecting is one on-screen view.
            //
            // The COUNTER is free and honest, so it is here. At timeout 0 (our
            // setting, and Ogre's default) it reads 0 for ever, which is exactly
            // the useful statement: "no frame in this session dropped a PSO".
            // Reset by the render system at the start of every frame.
            out.incompletePsoRequests = (unsigned)rs->getIncompletePsoRequestsCounter();
        }
        // ---- the Forward+ light census (LIGHTING_FIX fix 8 / F-F2). The
        // WORST case across the live scenes, because a per-cell overflow is a
        // property of one scene and this struct is process-wide.
        for (const auto &s : mScenes) {
            unsigned lights = 0u, budget = 0u;
            s->forwardPlusLightCensus(lights, budget);
            if (lights > out.forwardPlusLights) out.forwardPlusLights = lights;
            out.forwardPlusBudget = budget;
        }
        out.forwardPlusOverBudget = out.forwardPlusLights > out.forwardPlusBudget
                                        ? out.forwardPlusLights - out.forwardPlusBudget : 0u;
        return true;
    }
    // Not JAH_CATCH: this verb is const and the error sink is not. Nothing here
    // can fail in a way a caller could act on anyway — the counters are plain
    // reads and one flag flip — so "false, and `out` is default" is the whole
    // contract.
    catch (...) { out = RenderStats(); return false; }
}

bool OgreEngine::textureMemory(std::vector<TextureMemoryEntry> &out) const {
    JAH_SYNC_GETTER("Engine::textureMemory");
    out.clear();
    if (!mRoot) return false;
    JAH_TRY {
        Ogre::RenderSystem *rs = mRoot->getRenderSystem();
        Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
        if (!tm) return false;
        // The same walk as TextureGpuManager::dumpMemoryUsage (which only
        // knows how to print), row for row, so the two agree.
        const Ogre::TextureGpuManager::ResourceEntryMap &entries = tm->getEntries();
        out.reserve(entries.size());
        for (const auto &kv : entries) {
            const Ogre::TextureGpu *t = kv.second.texture;
            if (!t) continue;
            TextureMemoryEntry e;
            e.name = t->getNameStr();
            e.resource = t->getRealResourceNameStr();
            e.width = t->getWidth();
            e.height = t->getHeight();
            e.depth = t->getDepth();
            e.slices = t->getNumSlices();
            e.mipmaps = t->getNumMipmaps();
            e.msaa = t->getSampleDescription().getColourSamples();
            e.format = Ogre::PixelFormatGpuUtils::toString(t->getPixelFormat());
            e.bytes = t->getSizeBytes();
            e.renderTarget = t->isRenderToTexture();
            e.uav = t->isUav();
            e.manual = t->_isManualTextureFlagPresent();
            e.pooled = t->hasAutomaticBatching();
            // GpuResidency::toString is declared but not exported by the pin.
            switch (t->getResidencyStatus()) {
            case Ogre::GpuResidency::OnStorage:   e.residency = "OnStorage"; break;
            case Ogre::GpuResidency::OnSystemRam: e.residency = "OnSystemRam"; break;
            case Ogre::GpuResidency::Resident:    e.residency = "Resident"; break;
            default:                              e.residency = "Unknown"; break;
            }
            out.push_back(std::move(e));
        }
        return true;
    }
    // Const reader, like memoryStats: nothing here is worth a sink entry.
    catch (...) { out.clear(); return false; }
}

bool OgreEngine::memoryStats(MemoryStats &out) const {
    JAH_SYNC_GETTER("Engine::memoryStats");
    out = MemoryStats();
    if (!mRoot) return false;
    JAH_TRY {
        if (Ogre::RenderSystem *rs = mRoot->getRenderSystem()) {
            if (Ogre::VaoManager *vao = rs->getVaoManager()) {
                Ogre::VaoManager::MemoryStatsEntryVec entries;
                size_t capacity = 0, freeBytes = 0;
                bool includesTextures = false;
                vao->getMemoryStats(entries, capacity, freeBytes, nullptr, includesTextures);
                out.gpuPoolCapacityBytes = capacity;
                out.gpuPoolFreeBytes = freeBytes;
                out.gpuPoolsIncludeTextures = includesTextures;
                // One entry per BLOCK; pools are the distinct (type, index) pairs.
                std::set<std::pair<Ogre::uint32, Ogre::uint32>> pools;
                for (const auto &e : entries) pools.insert({ e.poolType, e.poolIdx });
                out.gpuPools = unsigned(pools.size());
            }
        }
        auto walk = [&](Ogre::SceneManager *sm) {
            if (!sm) return;
            ++out.sceneManagers;
            for (int i = 0; i < Ogre::NUM_SCENE_MEMORY_MANAGER_TYPES; ++i) {
                const auto type = Ogre::SceneMemoryMgrTypes(i);
                Ogre::NodeMemoryManager &nmm = sm->_getNodeMemoryManager(type);
                const size_t depths = nmm.getNumDepths();
                out.simdNodeDepths += unsigned(depths);
                // The node pools per hierarchy depth, read from the pools rather
                // than walked (the document's nodes are DETACHED roots in the
                // staging manager — createSceneNode, no parent — so a walk from
                // the Ogre root would miss every one of them). simdNodes is the
                // LIVE count (getNumLiveNodes); the USED slots (getFirstNode's
                // count) include the holes of non-LIFO removals until a level's
                // cleanup, so their sum moves with the order of past removals —
                // it is reported apart, as simdNodeSlots (MIRROR-SETTLE-1 fix 6:
                // 200 removals took the used sum 469 -> 470 with 468 live both
                // times).
                for (size_t depth = 0; depth < depths; ++depth) {
                    Ogre::Transform first;
                    out.simdNodeSlots += unsigned(nmm.getFirstNode(first, depth));
                    out.simdNodes += unsigned(nmm.getNumLiveNodes(depth));
                }
                out.simdObjects += unsigned(sm->_getEntityMemoryManager(type).getTotalNumObjects());
            }
            out.simdObjects += unsigned(sm->_getLightMemoryManager().getTotalNumObjects());
        };
        for (const auto &s : mScenes) if (s) walk(s->sceneManager());
#ifdef __linux__
        if (FILE *f = std::fopen("/proc/self/statm", "r")) {
            unsigned long size = 0, resident = 0;
            if (std::fscanf(f, "%lu %lu", &size, &resident) == 2)
                out.residentBytes = (unsigned long long)resident * (unsigned long long)sysconf(_SC_PAGESIZE);
            std::fclose(f);
        }
#endif
        return true;
    }
    // Not JAH_CATCH: const, like renderStats — nothing here can fail in a way
    // worth a sink entry.
    catch (...) { out = MemoryStats(); return false; }
}

bool OgreEngine::reclaimMemory(MemoryStats *before, MemoryStats *after) {
    JAH_HOST_CALL("Engine::reclaimMemory");
    EngineSideScope engineSide;   // the reads below are the command's own, on the engine's side (U3 stage 3)
    if (!mRoot) return false;
    MemoryStats b;
    memoryStats(b);
    if (before) *before = b;
    JAH_TRY {
        for (const auto &s : mScenes)
            if (s && s->sceneManager()) s->sceneManager()->shrinkToFitMemoryPools();
        // (No VaoManager call: cleanupEmptyPools() throws ERR_NOT_IMPLEMENTED at
        // this pin, and the Vulkan VaoManager already returns an emptied pool
        // to the driver from its own _update — MemoryStats explains.)
        MemoryStats a;
        memoryStats(a);
        if (after) *after = a;
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "Jahshaka: reclaimMemory — %u scene managers shrunk to fit; GPU pools "
                      "%.1f MB (%.1f MB free) -> %.1f MB (%.1f MB free); RSS %.1f MB -> %.1f MB",
                      a.sceneManagers, b.gpuPoolCapacityBytes / 1048576.0,
                      b.gpuPoolFreeBytes / 1048576.0, a.gpuPoolCapacityBytes / 1048576.0,
                      a.gpuPoolFreeBytes / 1048576.0, b.residentBytes / 1048576.0,
                      a.residentBytes / 1048576.0);
        Ogre::LogManager::getSingleton().logMessage(buf);
        return true;
    } JAH_CATCH(mLastError, false);
}

bool OgreEngine::objectCounts(ObjectCounts &out) const {
    JAH_SYNC_GETTER("Engine::objectCounts");
    out = ObjectCounts();
    if (!mRoot) return false;
    JAH_TRY {
        out.views  = unsigned(mViews.size());
        out.scenes = unsigned(mScenes.size());
        out.updatedScenes = mUpdatedScenes;
        for (const auto &v : mViews) {
            if (v && v->isEnabled()) ++out.enabledViews;
        }
        for (const auto &s : mScenes) {
            if (s) s->addObjectCounts(out);
        }
        // The one PROCESS-WIDE number. Datablocks belong to the HlmsManager,
        // not to a SceneManager (EnginePrivate.h says so where the particle
        // datablocks are declared), so there is nothing per-scene to sum: walk
        // the registered Hlms types once. HLMS_MAX is the end of the ordinary
        // types; HLMS_COMPUTE sits AFTER it in the enum and is not part of
        // mRegisteredHlms's addressable range, which is why the loop stops
        // where it does. The count includes Ogre's own defaults (one per
        // registered Hlms), so only its DELTA carries meaning — ObjectCounts
        // says as much.
        if (Ogre::HlmsManager *hlmsMgr = mRoot->getHlmsManager()) {
            for (int t = 0; t < Ogre::HLMS_MAX; ++t) {
                if (Ogre::Hlms *h = hlmsMgr->getHlms(Ogre::HlmsTypes(t)))
                    out.datablocks += unsigned(h->getDatablockMap().size());
            }
        }
        return true;
    }
    // Same reasoning as renderStats: const method, no error sink, and nothing
    // here fails in a way a caller could act on.
    catch (...) { out = ObjectCounts(); return false; }
}

bool OgreEngine::threading(EngineThreading &out) const {
    JAH_SYNC_GETTER("Engine::threading");
    out = EngineThreading();
    if (!mRoot) return false;
    JAH_TRY {
        // THE MODE, decoded from the macros the CMake option sets
        // (ogre-next/CMakeLists.txt:445-453). This is compiled into THIS
        // translation unit against the INSTALLED OgreBuildSettings.h, so it
        // reports what Studio was built against; the capability query below
        // reports what the LINKED engine can actually do. In a healthy tree the
        // two agree — and if they ever disagree, Root's ABI cookie
        // (generateAbiCookie, which hashes both macros) has already aborted the
        // process before anyone could read either.
#ifndef OGRE_SHADER_THREADING_BACKWARDS_COMPATIBLE_API
        out.shaderThreadingMode = 2u;
#else
        out.shaderThreadingMode = 1u;
#endif
        if (Ogre::RenderSystem *rs = mRoot->getRenderSystem())
            out.multithreadedShaderCompilation = rs->supportsMultithreadedShaderCompilation();
        for (const auto &s : mScenes) {
            if (!s) continue;
            const unsigned n = s->sceneManager()
                                   ? unsigned(s->sceneManager()->getNumWorkerThreads()) : 0u;
            out.sceneWorkerThreads.emplace_back(s->name(), n);
            if (n > out.hlmsThreads) out.hlmsThreads = n;
        }
        return true;
    }
    // Same reasoning as renderStats/objectCounts: const method, no error sink.
    catch (...) { out = EngineThreading(); return false; }
}

bool OgreEngine::saveShaderCache() {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { return e.saveShaderCache(); })) return true;
    if (!mRoot) return false;
    JAH_TRY { return mShaderCache.save(mRoot); } JAH_CATCH(mLastError, false);
}

bool OgreEngine::flushShaderCache(unsigned budgetMs) {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { return e.flushShaderCache(budgetMs); })) return true;
    JAH_TRY { return mShaderCache.flushWrites(budgetMs); } JAH_CATCH(mLastError, false);
}

bool OgreEngine::clearShaderCache() {
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([=](OgreEngine &e) { return e.clearShaderCache(); })) return true;
    // BOTH CACHES, because they are one directory and one lifetime (I-5). The
    // shader cache's wipe() unlinks every file in that directory anyway, so a
    // clear that left the texture cache's in-memory state standing would write
    // a manifest naming files it had just deleted on the next save.
    JAH_TRY {
        const bool ok = mShaderCache.clear();
        textureCache().clear();
        return ok;
    } JAH_CATCH(mLastError, false);
}

void OgreEngine::setCompileObserver(std::function<void()> observer) {
    // ON TWO THREADS (U3 stage 4b) the observer stays the host's: the render side counts the compiles it makes and
    // the host delivers them on its own thread while it waits (the pump, startRenderThread).
    if (mRenderThread.threaded() && detail::tlsEngineSide == 0) {
        mHostCompileObserver = observer;
        const bool on = static_cast<bool>(observer);
        postedAsMessage([on](OgreEngine &e) {
            e.mShaderCache.setCompileObserver(on ? std::function<void()>([&e] { e.mCompileTicks.fetch_add(1); })
                                                 : std::function<void()>());
            return true;
        });
        return;
    }
    // THE HOST'S CALL IS A MESSAGE of the engine's stream (U3 stage 4a, decision 39 c), applied at the drain.
    if (postedAsMessage([observer](OgreEngine &e) { e.setCompileObserver(observer); return true; })) return;
    mShaderCache.setCompileObserver(std::move(observer));
}

void OgreEngine::shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                                     unsigned &expected) const {
    mShaderCache.progress(compiled, fromCache, expected);
}

// THE TEARDOWN RUNS ON THE RENDER THREAD (U3 stage 4b, the third join): the UI thread joins it after every view's
// surface is gone and before Qt destroys the widgets' windows — trap 1's order inside unchanged.
OgreEngine::~OgreEngine() {
    mRenderThread.stop([this] { teardown(); });
}

void OgreEngine::teardown() {
    EngineSideScope engineSide;   // the teardown: the engine's own calls inside it are the render side's (U3 4a)
    // THE HOST CALLS OF THE RUN, DUMPED (U3 stage 4a's census-by-counter): JAH_HOST_CALLS_DUMP=<file> appends every
    // counted site that saw a host call — the work list the counter settles, never a grep.
    if (const char *dump = std::getenv("JAH_HOST_CALLS_DUMP")) {
        if (FILE *f = std::fopen(dump, "a")) {
            for (const detail::SyncGetterSite *site = detail::syncGetterSites(); site; site = site->next)
                if (const unsigned long long n = site->calls.load(std::memory_order_relaxed))
                    std::fprintf(f, "%s %llu\n", site->name, n);
            std::fclose(f);
        }
    }
    // THE FRAME MONITOR GOES FIRST (MONITOR-RETIRE-1 F2; trap 1: teardown
    // mirrors creation). It is a process-wide pointer (`monitor::gMonitor`)
    // into THIS engine's member, and its listener sits on workspaces the loops
    // below destroy: left on, the member died with the engine and the pointer
    // did not — a second engine booted in the same process crashed in
    // FrameMonitor::beginFrame on its first frame. Stopping the capture here
    // detaches every listener while the workspaces live, frees the query pools
    // while the device lives, and clears the global.
    try { setFrameMonitor(MonitorLevel::Off); } catch (...) {}
    // THE VR SESSION GOES FIRST, and it has to: it owns a View (a workspace, a
    // camera, an RTT), a second workspace on somebody else's target and a set
    // of XR swapchains that name VkImages the runtime owns. Every one of those
    // is invalid the moment the loops below start, and the XR session must be
    // ended while its device is still alive (VR_SPEC §4.3's teardown order).
    // The session's CONNECTION (its XrInstance) goes right after it; the
    // Vulkan instance and device are Ogre's own and die with Root.
    if (mVrSession) {
        VrSession *session = mVrSession;
        mVrSession = nullptr;      // see endVrSession: cleared first, then deleted
        mHostVrSessionLive = false;
        mHostVrView = nullptr;
        try { vr::sessionEnd(session); } catch (...) {}
    }
    // The connection's XrInstance, while the device it is bound to is alive.
    if (mVrConnection) { try { vr::disconnect(mVrConnection); } catch (...) {} mVrConnection = nullptr; }
    // The shadow-pass counter is a listener on a live workspace: unhook it
    // before anything that owns a workspace starts dying.
    detachShadowCounter();
    // Save the shader cache FIRST, while every Ogre singleton the three layers
    // read is still alive and before a single view or scene has been torn down.
    // This is the primary save point (SHADER_CACHE_SPEC §4.4): a clean quit is
    // the only moment we are certain nothing is compiling.
    if (mRoot) { try { mShaderCache.saveWhole(mRoot); } catch (...) {} }   // unchunked: nothing draws after this
    // The TEXTURE cache in the same breath and for the same reason
    // (THREADING_ADOPTION_SPEC.md P2): its metadata half is exported from
    // TextureGpuManager, which dies with the render system a few lines below.
    // The host also saves it explicitly at shutdown (EngineHost::shutdown, next
    // to saveShaderCache) — this is the point that runs even when the Engine
    // outlives that call, and save() is idempotent.
    if (mRoot) {
        try {
            textureCache().save(mRoot, [this](std::function<void()> write) {
                mShaderCache.dispatchTask(std::move(write));
            });
        } catch (...) {}
    }
    // THE RAY-QUERY TIER'S acceleration structures, buffers and pipeline: all
    // of them are VkDevice objects and the device dies with the render system a
    // few lines below. Same rule, same reason, as the MeshPtrs — and the tier
    // holds MeshPtrs of its own, which it releases here.
    try { shutdownRayQuery(); } catch (...) {}
    // The SSAO rotation-noise texture is ours and must not outlive Root.
    // Its own try/catch, NOT JAH_TRY: that macro's handler ends in `return`,
    // which inside a destructor abandons the rest of the teardown — views,
    // scenes, meshes and Root itself would all leak, and Engine::isAlive() would
    // never go false. (Found by teardown_is_clean, which is exactly its job.)
    if (mRoot) { try { chain::destroySsao(mRoot); } catch (...) {} }
    // Dependency order, all BEFORE Root: views (workspaces, cameras, windows,
    // textures) -> scenes (items, datablocks, our MeshPtrs, scene managers)
    // -> null window -> any leftover meshes -> Root.
    for (auto &v : mViews)  v->destroy();
    mViews.clear();
    syncHostViews();
    for (auto &s : mScenes) s->destroy();
    mScenes.clear();
    // The IES profile atlas and the area-light mask pool are process-wide, so
    // no scene owns them: free them here, while Root (and its texture manager)
    // is still alive.
    lightextras::shutdown();
    // THE ID PASS'S PIPELINE AND ITS IDENTITY INDEX BUFFER (ATOM S3-DRAW): device
    // objects, after every view (whose workspaces recorded it) and before Root.
    try { releaseAtomIdPass(); } catch (...) {}
    // THE PYRAMID BUILD'S ARRIVAL COUNTER (JahHzbBuild_cs): a device buffer bound on the
    // shared job, after every view whose chain dispatched it and before Root.
    try { releaseHzbBuild(); } catch (...) {}
    try { releaseAtomCasterPass(); } catch (...) {}
    // THE CUT-OUTS' MASKS (AlphaMasks.h): the engine's store, after every scene's table and before Root.
    try { releaseAlphaMasks(); } catch (...) {}
    // ...and THE PASS PROVIDER comes off the compositor manager (V2-E E9), after every
    // workspace that could instantiate its passes died with the views above. The
    // provider is a process static: left installed, it stayed armed across Root's
    // teardown, holding the closures the two releases just emptied.
    if (mRoot) { try { AtomPassProvider::uninstall(mRoot->getCompositorManager2()); } catch (...) {} }
    if (mRoot) { try { detail::PhotonPassBinding::uninstallPassListener(mRoot->getCompositorManager2()); } catch (...) {} }
    if (mRoot) { try { detail::gpuCheckpointsUninstallPassListener(mRoot->getCompositorManager2()); } catch (...) {} }
    // AFTER every scene (each of which removed its own render-queue listener in
    // OgreScene::destroy) and BEFORE Root: ~OverlaySystem deletes the
    // FontManager, whose Font::unloadResource destroys the HlmsUnlit datablock
    // the font created. An OverlaySystem outliving Root is the same class of
    // bug as a MeshPtr outliving Root.
    // The blank world's render-queue listener comes off with the scenes' (each
    // removed its own in OgreScene::destroy) and BEFORE the OverlaySystem that
    // owns it dies; the manager itself goes below. Its
    // cameras are already gone — every View destroyed above took its own.
    if (mBlankScene) { try { hud::detach(mBlankScene); } catch (...) {} }
    try { hud::destroySystem(); } catch (...) {}
    try {
        if (mBlankScene && mRoot) mRoot->destroySceneManager(mBlankScene);
        mBlankScene = nullptr;
    } catch (...) {}
    try {
        if (mNullWindow && mRoot) mRoot->getRenderSystem()->destroyRenderWindow(mNullWindow);
        mNullWindow = nullptr;
        if (mRoot && Ogre::MeshManager::getSingletonPtr())
            Ogre::MeshManager::getSingleton().removeAll();
    } catch (...) {}
    // The decal atlases are process-wide but their TextureGpu pointers belong to
    // THIS Root; a second Engine in the same process (test_engine_recreate)
    // would otherwise inherit stale masters and slice textures.
    detail::resetDecalAtlases();
    // ...and the same for the cross-scene file-texture references: every scene
    // above released its own, so this is empty on a clean teardown.
    detail::resetSharedTextures();
    // THE WRITER THREAD LOGS THROUGH OGRE'S LOG after its fsync (FSYNC-1's
    // second read): join it HERE, before Root — and the LogManager Root owns —
    // dies. The save at the top of this destructor is a hand-off; this is where
    // the old synchronous write used to cost the same wait, so nothing got slower.
    mShaderCache.finishWrites();
    // Both log listeners are registered on Ogre's default log, which Root owns.
    mShaderCache.detachCounters();
    detachLogBridge();
    // The pass listener's samplerblock references go back to the manager that
    // gave them (PHOTON-ENV-1 audit F10) — it dies with Root.
    try { FogHlmsListener::releaseSamplers(); } catch (...) {}
    // ...and the specular albedo table's buffer goes back to the VaoManager that made it (PHOTON-3).
    try { Ogre::PhotonSpecularAlbedo::releaseConstBuffer(); } catch (...) {}
    // The pooled mip jobs (ENGINE-UPGRADE-ASYNC-3) go back to the compute Hlms that cloned them.
    try { Ogre::PhotonMipChain::releasePool(); } catch (...) {}
    // ...and the pooled scroll targets and anisotropic mip-step jobs (BUGS-6 stage 3), every scene's
    // chain having given its own back above.
    try { Ogre::PhotonVoxelScroll::releasePool(); } catch (...) {}
    try { Ogre::PhotonVoxelLighting::releaseMipStepPool(); } catch (...) {}
    if (mRoot && mInRecordFaultAttached) mRoot->removeFrameListener(mInRecordFault.get());
    mInRecordFaultAttached = false;
    if (mRoot && mSplitPublishAttached) mRoot->removeFrameListener(mSplitPublish.get());
    mSplitPublishAttached = false;
    delete mRoot;
    mRoot = nullptr;
    gLiveEngine = nullptr;
}

bool OgreEngine::viewNameTaken(const std::string &name) {
    for (auto &v : mViews)
        if (v->name() == name) { mLastError = "View '" + name + "' already exists"; return true; }
    return false;
}

void OgreEngine::ensureHlms() {
    if (!mFatalInitError.empty())
        OGRE_EXCEPT(Ogre::Exception::ERR_INVALID_STATE, mFatalInitError, "OgreEngine::ensureHlms");
    if (mHlmsRegistered) return;
    // WHAT A HEADLESS ENGINE SKIPS, and why (EngineConfig::headless). Three of
    // the steps below exist only to make PIXELS possible, and one of them is
    // not merely useless without a device — it CRASHES:
    //
    //   * registerCommonMaterials() parses the low-level .material/.program
    //     scripts. Every one of them declares GPU programs in a shader language
    //     (glsl/glslvk/hlsl/metal), and the NULL render system supports none of
    //     them and creates no GpuProgramManager at all. Ogre's script
    //     translator does not check: PassTranslator::translateVertexProgramRef
    //     -> Pass::setVertexProgram -> GpuProgramUsage::recreateParameters ->
    //     UnifiedHighLevelGpuProgram::createParameters, which calls
    //     GpuProgramManager::getSingleton() on a null singleton and SEGFAULTS
    //     (OgreUnifiedHighLevelGpuProgram.cpp:158, reproduced 2026-09-06 on the
    //     first Atmosphere.material). Not catchable, not fixable from here —
    //     the scripts simply must not be parsed without a real render system.
    //   * the overlay/HUD system rasterizes a font into a texture and builds
    //     overlay elements — screen furniture for a screen that does not exist.
    //     Skipping createSystem() leaves every other hud:: call a no-op by
    //     construction (they all test gSystem), including attach() per scene.
    //   * createShadowNode() defines compositor workspaces, and only a View can
    //     instantiate one.
    //
    // What DOES happen headless is everything the document model needs: both
    // Hlms implementations (materials and datablocks are real), the ambient
    // mode, the fog listener and the shadow-filter preference.
    const bool pixels = !mHeadless;
    // THE OVERLAY SYSTEM GOES FIRST, and the order is load-bearing
    // (STATS_OVERLAY_SPEC §2.1, spikes/overlay-v1-vulkan/FINDINGS.md): its
    // constructor creates BOTH the OverlayManager and the FontManager, so it
    // must run after a render window exists — every caller of this function has
    // just made one — and BEFORE registerCommonMaterials() below calls
    // initialiseAllResourceGroups, which is the moment `.fontdef` scripts are
    // parsed. Construct it later and the font script is never seen at all.
    if (pixels) hud::createSystem();
    // BillboardSet2 needs no ParticleFX2 plugin (its core is in OgreNextMain),
    // BUT the Hlms only puts the view matrix in the pass buffer — which the
    // particle vertex shader needs for camera-facing quads — when this static
    // flag is set. The plugin's install() is normally what sets it; without a
    // plugin we set it ourselves, BEFORE any shader is built.
    Ogre::Hlms::_setHasParticleFX2Plugin(true);
    Ogre::ArchiveManager &am = Ogre::ArchiveManager::getSingleton();
    Ogre::String mainPath; Ogre::StringVector libPaths;

    Ogre::HlmsUnlit::getDefaultPaths(mainPath, libPaths);
    {
        Ogre::ArchiveVec libs;
        for (const auto &p : libPaths) libs.push_back(am.load(mMediaDir + p, "FileSystem", true));
        mRoot->getHlmsManager()->registerHlms(
            OGRE_NEW Ogre::HlmsUnlit(am.load(mMediaDir + mainPath, "FileSystem", true), &libs));
    }
    Ogre::HlmsPbs::getDefaultPaths(mainPath, libPaths);
    {
        Ogre::ArchiveVec libs;
        for (const auto &p : libPaths) libs.push_back(am.load(mMediaDir + p, "FileSystem", true));
        // PHOTON'S PIXEL PIECES (OWN-GI-1): the voxel cascades' and the irradiance
        // field's, which replace upstream's of the same names (`@undefpiece`) — so
        // AFTER Pbs's own library, and before Hlms/Jahshaka, as in HlmsAtom's list.
        libs.push_back(am.load(mMediaDir + "Photon/Hlms", "FileSystem", true));
        // Jahshaka's own pieces (fog colour + height fog, base-map UV tiling) go in
        // as a LIBRARY folder rather than as per-datablock custom pieces: one
        // _piece_vs_piece_ps file then defines the pass-buffer members for BOTH
        // shader stages, which is the only way the vertex and pixel shader are
        // guaranteed to agree on the layout of the buffer they both read. It must
        // be LAST — the fog piece redefines a piece of Hlms/Pbs/Any/Atmosphere,
        // and a redefinition only works after the original has been collected.
        libs.push_back(am.load(mMediaDir + "Hlms/Jahshaka", "FileSystem", true));
        // ScenePbs: upstream's HlmsPbs plus the per-pass GI binding (every scene
        // pass binds ITS scene's probe grid, mirrors and IBL length — SceneGiBinding,
        // EnginePrivate.h; the voxel lighting and field are bound by the listener,
        // PhotonPassBinding).
        mRoot->getHlmsManager()->registerHlms(
            OGRE_NEW ScenePbs(am.load(mMediaDir + mainPath, "FileSystem", true), &libs));
    }
    // HLMS ATOM (ATOM-S3-PARITY; SPECS/atom/D1 section 1) — the visibility buffer's
    // material decode, a derived HlmsPbs on the Terra pattern, registered BESIDE
    // PBS and in the same breath: window -> registerHlms -> scene manager is the
    // startup-order trap, and this is the registerHlms step. Nothing in the
    // product's opaque ATOM geometry is drawn by the id pass and shaded by it (the
    // screen decode, ATOM S3-DRAW), and the ray hits no cache shades are too; it is
    // TOLD everything PBS is told by tellEveryHlms below and once per frame before
    // its first read. Its pass provider — the engine's ONE CompositorPassProvider,
    // multiplexed on customId — is installed here too, before any workspace
    // definition could name a custom pass, with the id pass's recorder on it.
    HlmsAtom::getDefaultPaths(mainPath, libPaths);
    {
        Ogre::ArchiveVec libs;
        for (const auto &p : libPaths) libs.push_back(am.load(mMediaDir + p, "FileSystem", true));
        HlmsAtom *atom = OGRE_NEW HlmsAtom(am.load(mMediaDir + mainPath, "FileSystem", true), &libs);
        atom->setArms(&mArms);
        mRoot->getHlmsManager()->registerHlms(atom);
    }
    AtomPassProvider::install(mRoot->getCompositorManager2());
    detail::PhotonPassBinding::installPassListener(mRoot->getCompositorManager2());   // the GI readers (ASYNC-3)
    detail::gpuCheckpointsInstallPassListener(mRoot->getCompositorManager2());   // (nothing unless the marks are on)
    registerAtomIdPass();
    registerAtomCasterPass();
    // Ambient is SPHERICAL HARMONICS, always and everywhere (Scene::setAmbientSh;
    // Scene::setAmbient converts the flat/hemisphere pair exactly). The mode is a
    // property of the HlmsPbs INSTANCE, not of a scene, so it cannot be chosen
    // per scene: every scene therefore speaks SH, and a scene that pushes nothing
    // gets black ambient rather than a stale hemisphere. AmbientSh also means the
    // ambient contributes no specular term of its own — that comes from the
    // GGX-prefiltered sky reflection cubemap instead.
    static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        ->setAmbientLightMode(Ogre::HlmsPbs::AmbientSh);
    {
        // LIGHT-COUNT BUDGETS (LIGHTING_FIX fix 7 / F-L1, F-L2). Both of these
        // exist for one reason: an EDITOR changes its light list constantly, and
        // by default every such change is a shader recompile of the whole scene.
        //
        //  * setMaxNonCasterDirectionalLights(4). Ogre's default is 0, which
        //    means "hardcode the exact count into the shader" — so adding or
        //    removing a directional light recompiles every PBS shader in the
        //    scene. Upstream's own note: "There is little to no performance
        //    impact for setting this value higher than you need... you'll pay
        //    the price of [what you have] (but the RAM price of 4)"
        //    (OgreHlms.h:695-714). Four is the editor's realistic ceiling for
        //    fill lights that cast nothing.
        //  * setStaticBranchingLights(true). Same disease for shadow-casting
        //    spot and point lights: the permutation key is the COMBINATION
        //    (3 spot + 5 point is a different shader set from 4 + 4), and
        //    static branching collapses that (setProperty( LightsPoint, 0 ),
        //    OgreHlms.cpp:3673-3675).
        //
        //    ITS DOCUMENTED PRECONDITION IS MET HERE: "All point and spot
        //    lights must share the same hlms_shadowmap atlas" (OgreHlms.h:733).
        //    buildShadowNode puts every ShadowParam on atlasId 0 — one atlas,
        //    always, for both shadow nodes we define.
        //
        //    WHAT IT DOES NOT BUY, A/B-MEASURED on this pin by lights.hygiene
        //    (engine built with and without these two calls):
        //        add + remove 3 non-caster directionals: 6 compiles -> 0
        //        4 spot<->point swaps at a constant count: 4 compiles -> 2
        //        toggling a spot's cast-shadows:          2 compiles -> 2
        //    So the shadow-caster half is a HALVING, not an elimination:
        //    `hlms_num_shadow_map_lights` is set to the live count
        //    unconditionally (OgreHlms.cpp:3270-3273) and only an extra FLAG is
        //    gated on static branching, so the TOTAL number of casters is still
        //    part of the shader key. Zero compiles on a cast-shadows toggle is
        //    NOT achievable here without an upstream change.
        //
        //    IT IS ALSO A SHADING CHANGE, not just a compile-count one:
        //    HlmsPbs::setStaticBranchingLights forces
        //    setShadowReceiversInPixelShader(true) (OgreHlmsPbs.cpp:3839), so
        //    light vectors are computed per pixel rather than interpolated from
        //    the vertex shader — strictly more correct, and slightly different.
        //    MEASURED CONSEQUENCE: none. The full 203-suite gate, pixel suites
        //    included, moved not one pixel.
        auto *pbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        pbs->setMaxNonCasterDirectionalLights(4u);
        pbs->setStaticBranchingLights(true);
    }
    // Fog: append the per-scene fog colour + height parameters to every PBS pass
    // buffer (the exponential distance term itself is upstream's block, fed by
    // the scene's atmosphere component — OgreFog.cpp, Atmosphere.h). Unlit gets
    // no listener: gizmos, wires and billboards stay unfogged.
    mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS)->setListener(&gFogListener);
    // THE SPECULAR LOBE'S ALBEDO TABLE (PHOTON-3): the one immutable constant buffer every
    // colour pass's environment term (bound by that listener) and the Photon jobs read, made
    // HERE - an immutable upload is a copy on the device queue, never one inside a pass.
    Ogre::PhotonSpecularAlbedo::constBuffer(mRoot->getRenderSystem()->getVaoManager());
    // Shader-generation debugging: JAHSHAKA_HLMS_DEBUG_DIR=/some/dir/ dumps every
    // generated shader (and its properties) there, for EVERY PBS-family host (a
    // debug output path is not relayed by tellEveryHlms). Diagnostic only.
    if (const char *dbg = std::getenv("JAHSHAKA_HLMS_DEBUG_DIR")) {
        mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS)->setDebugOutputPath(true, true, dbg);
        mRoot->getHlmsManager()->getHlms(HlmsAtom::kType)->setDebugOutputPath(true, true, dbg);
    }
    // ...and every other PBS-family host now knows what PBS was just told.
    tellEveryHlms(mRoot->getHlmsManager(), true);
    // THE CACHE LOAD GOES HERE and nowhere else (SHADER_CACHE_SPEC §4.3 rule 5):
    // after BOTH registerHlms calls — HlmsDiskCache::applyTo needs the Hlms
    // instances to exist — and before registerCommonMaterials(), which parses
    // the low-level scripts and is the first thing that can trigger a compile.
    // The order INSIDE load() (pipeline blob, then setSaveMicrocodesToCache +
    // microcode, then the Hlms caches) is upstream's, not ours: see
    // OgreHlmsDiskCache.h:74-77 and Samples/2.0/Common/src/GraphicsSystem.cpp:626.
    //
    // THE "Jahshaka" RESOURCE GROUP EXISTS BEFORE THE LOAD (ENGINE-UPGRADE-1 stage 1): a
    // material's custom piece (a project's piece file, a shader graph's emitted piece) is
    // registered in that group when the material sets it (OgreMaterials.cpp, OgreLights,
    // OgreParticles, OgreDecals — each still creates it if absent), and the Hlms cache a
    // session saves NAMES the group. Before, the next boot's load asked for a group nobody had
    // created yet and the whole cache was thrown away; now a piece whose file is not
    // registered yet is simply not found, and the fork's HlmsDiskCache::applyTo drops only the
    // entries that use it.
    {
        Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
        if (!rgm.resourceGroupExists("Jahshaka")) rgm.createResourceGroup("Jahshaka", false);
    }
    mShaderCache.load(mRoot);
    // THE TEXTURE SIDE, in the same breath and for the same reason
    // (THREADING_ADOPTION_SPEC.md P2 items 5 and 6): the render system and its
    // TextureGpuManager exist by now, and nothing has asked for a texture yet.
    //
    // THE MULTILOAD POOL. Ogre's default is 0 — ONE background thread doing
    // every read and every PNG/JPG decode, serially. Upstream's measured sweet
    // spot is 4-8 (OgreTextureGpuManager.h:1132) and the documented cost is
    // memory ("you may end up with many images loaded in RAM", :1106-1107), so
    // the default here is deliberately conservative: half the machine's threads,
    // clamped to [2, 6]. JAH_TEXTURE_MULTILOAD overrides it (0 disables the
    // feature entirely and restores today's single-threaded loading with no
    // rebuild — the A arm of the G2-c measurement, and the first step of P2's
    // order of retreat).
    //
    // SAFE FOR US, and this needs saying because upstream flags it: multiload
    // loads OUT OF ORDER. That matters only to callers that use reservePoolId()
    // to assign pool slices themselves, which we never do; and the paths that DO
    // need ordering — the grayscale expansion and every createTexture upload —
    // pass an Image2, which sets bSkipMultiload implicitly
    // (OgreTextureGpu.h:405-418).
    if (Ogre::RenderSystem *rs = mRoot->getRenderSystem()) {
        if (Ogre::TextureGpuManager *tm = rs->getTextureGpuManager()) {
            // The ONE core budget's dedicated tier holds it (irisgl/core/threadbudget.h; THREADING-1): min(6, max(2,
            // hardware / 2)), JAH_TEXTURE_MULTILOAD read there.
            unsigned threads = unsigned(iris::threadBudget().multiload);
            // Never under the NULL render system: a headless run decodes nothing
            // worth parallelising and the pool would be four idle threads.
            if (mHeadless) threads = 0;
            if (threads > 0) tm->setMultiLoadPool(threads);
            mMultiLoadThreads = threads;
            mHostMultiLoadThreads = threads;
        }
    }
    // THE WAIT'S NO-PROGRESS BUDGET. The default (8 s) is a member initialiser,
    // not set here, so that a frame rendered before this point still waits; what
    // happens HERE is the env override, read once, beside the pool it belongs
    // with. 8 s is absurd for a healthy queue and short enough that a broken one
    // is a hitch and not a hang: the budget only starts counting once the
    // pending set STOPS shrinking, so no amount of real work can reach it (see
    // drainTextureStreaming). 0 disables the wait, which is a measurement mode
    // and not a supported one.
    if (const char *forced = std::getenv("JAH_TEXTURE_WAIT_MS")) {
        const long n = std::strtol(forced, nullptr, 10);
        mTextureWaitBudgetMs = unsigned(std::max(0l, std::min(600000l, n)));
        mHostTextureWaitBudgetMs = mTextureWaitBudgetMs;
    }
    if (const char *fault = std::getenv("JAH_TEXTURE_WAIT_FAULT"))
        mTextureWaitFault = (std::strtol(fault, nullptr, 10) != 0);
    // The drain's advance cadence (DRAIN-1; the member's note has the why).
    // Read here with the budget and the fault, for the same reason: a frame
    // drawn before this point still drains, at the shipped 16 ms.
    if (const char *cadence = std::getenv("JAH_TEXTURE_DRAIN_ADVANCE_MS")) {
        const long n = std::strtol(cadence, nullptr, 10);
        mTextureDrainAdvanceMs = double(std::max(0l, std::min(1000l, n)));
    }
    // The texture cache (configured beside the shader cache in init(); it shares
    // that directory and its lifetime, with its own manifest and its own simpler
    // validity key — I-5). Loaded HERE, after the Hlms exists and before
    // anything can ask for a texture, exactly like the shader cache above.
    textureCache().load(mRoot);
    mHlmsRegistered = true;
    applyShadowFilter();   // replaces Ogre's PCF_3x3 default with ours (Soft = 4x4)
    if (!pixels) return;   // the rest is rendering-only — see the top of this function
    registerCommonMaterials();
    startAsyncShaders();
    // THE STAGED MEDIA IS THIS BUILD'S, OR THE ENGINE STOPS HERE (FORWARD-ONLY-1
    // D2) — before the blue noise, the overlay and the shadow node, and
    // STICKILY: every later ensureHlms re-reports the same error rather than
    // running views on an engine this function never finished.
    try {
        chain::verifyTonemapMedia();
    } catch (Ogre::Exception &e) {
        mFatalInitError = e.getDescription();
        throw;
    }
    // BLUE NOISE FOR ALPHA HASHING (MATERIAL_GAPS_SPEC A-3). The hashing piece
    // falls back to an ALU white-noise hash unless HlmsManager::mBlueNoise is
    // set (Hlms/Common/Any/AlphaHashing_piece_ps.any:8-25 picks the branch on
    // the `hlms_blue_noise` property, which HlmsPbs/HlmsUnlit set from
    // getBlueNoiseTexture()) — so every hashed surface and every hashed
    // particle in this tree has been dithering with the worse noise since
    // hashing existed. Nothing but this call was missing: the PNG it wants is
    // already staged (media/2.0/scripts/materials/Common/LDR_R_0.png), in a
    // folder registerCommonMaterials just registered and initialised.
    //
    // AFTER registerCommonMaterials, therefore, and not beside registerHlms:
    // the load is AUTODETECT_RESOURCE_GROUP_NAME, which only finds the file
    // once its location has been added and the group initialised.
    //
    // NON-FATAL by construction, the way the upstream sample writes it: a media
    // tree without the PNG must degrade to the ALU hash, not refuse to boot.
    try {
        mRoot->getHlmsManager()->loadBlueNoise();
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: blue-noise texture not loaded, alpha hashing falls back to the ALU "
            "hash: " + e.getFullDescription(), Ogre::LML_CRITICAL);
    }
    // After the resource groups are initialised (the .fontdef has been parsed by
    // now) and after HlmsUnlit exists (overlay elements bind Unlit datablocks):
    // build the overlay's elements and pay the freetype rasterization once,
    // here, rather than on the first frame of a world open.
    hud::build(mRoot);
    createShadowNode();
}

// ---------------------------------------------------------------------------
// tellEveryHlms — THE ONE FUNCTION every PBS-family host is told through
// (ATOM-S3-PARITY; SPECS/atom/D1 section 1; HlmsAtom.h).
//
// WHAT PBS IS TOLD TODAY, by every site that asks the HlmsManager for HLMS_PBS
// (grepped at the lane's base; the list is the relay below, one row per setter):
//   registerHlms (here)   the listener (fog, the environment's slot, the gather),
//                         the ambient mode (SH), the non-caster directional budget,
//                         static-branching lights (and the per-pixel shadow receive
//                         it forces)
//   OgreGi.cpp,           NOTHING relayed: the PCC, the planar mirrors and the IBL
//   OgrePlanar.cpp,       chain length are bound per PASS on every host (bindSceneGi,
//   OgreSky.cpp           SceneGiBinding) — the scene being drawn, not a relay; the
//                         Photon volumes are no host's at all (OWN-GI-1: the
//                         listener binds them, PhotonPassBinding, and reads the
//                         cone count from PBS itself)
//   OgreLights.cpp        setAreaLightForwardSettings (the approximation's budget 0),
//                         setLightProfilesTexture, loadLtcMatrix
//   OgreShadow.cpp        setShadowSettings (the PCF kernel)
//
// WHY A RELAY AND NOT A ROUTE. Those sites live in files other lanes own, so they
// keep telling PBS, and PBS is the source of truth: this copies what PBS HOLDS onto
// every other host. A NEW engine site that tells PBS reaches every host for free; a
// new HlmsPbs SETTER without a getter is the one thing it cannot see (the fork adds
// the getter with the setter — ATOM-S3-PARITY added the four that were missing).
//
// WHEN: at registration (forced), and ONCE PER FRAME (keyed on Root's frame number)
// the first time a host reads it — HlmsAtom's analyzeBarriers/preparePassHash, which
// run before any of its draws. Every engine setter runs on the update thread before
// or after renderOneFrame, never between two passes of one frame, so a pointer PBS
// was told to DROP (a PhotonVoxelLighting about to be deleted) is off every host before the
// next frame's first read. Unchanged state costs one compare per field.
namespace {
unsigned long sRelayFrame = ~0ul;
}  // namespace

void tellEveryHlms(Ogre::HlmsManager *manager, bool force) {
    if (!manager) return;
    const unsigned long frame = Ogre::Root::getSingleton().getNextFrameNumber();
    if (!force && frame == sRelayFrame) return;
    sRelayFrame = frame;
    auto *pbs = dynamic_cast<Ogre::HlmsPbs *>(manager->getHlms(Ogre::HLMS_PBS));
    if (!pbs) return;
    for (int t = Ogre::HLMS_LOW_LEVEL + 1; t < Ogre::HLMS_MAX; ++t) {
        if (t == Ogre::HLMS_PBS) continue;
        auto *host = dynamic_cast<Ogre::HlmsPbs *>(manager->getHlms(Ogre::HlmsTypes(t)));
        if (!host) continue;   // Unlit and anything else that is not PBS-family
        if (host->getListener() != pbs->getListener()) host->setListener(pbs->getListener());
        if (host->getAmbientLightMode() != pbs->getAmbientLightMode())
            host->setAmbientLightMode(pbs->getAmbientLightMode());
        if (host->getMaxNonCasterDirectionalLights() != pbs->getMaxNonCasterDirectionalLights())
            host->setMaxNonCasterDirectionalLights(pbs->getMaxNonCasterDirectionalLights());
        if (host->getStaticBranchingLights() != pbs->getStaticBranchingLights())
            host->setStaticBranchingLights(pbs->getStaticBranchingLights());
        if (host->getShadowReceiversInPixelShader() != pbs->getShadowReceiversInPixelShader())
            host->setShadowReceiversInPixelShader(pbs->getShadowReceiversInPixelShader());
        if (host->getAreaLightsApproxLimit() != pbs->getAreaLightsApproxLimit() ||
            host->getAreaLightsLtcLimit() != pbs->getAreaLightsLtcLimit())
            host->setAreaLightForwardSettings(pbs->getAreaLightsApproxLimit(),
                                              pbs->getAreaLightsLtcLimit());
        if (host->getShadowFilter() != pbs->getShadowFilter())
            host->setShadowSettings(pbs->getShadowFilter());
        if (host->getEsmK() != pbs->getEsmK()) host->setEsmK(pbs->getEsmK());
        if (host->getIrradianceVolume() != pbs->getIrradianceVolume())
            host->setIrradianceVolume(pbs->getIrradianceVolume());
        if (host->getLightProfilesTexture() != pbs->getLightProfilesTexture())
            host->setLightProfilesTexture(pbs->getLightProfilesTexture());
        // NOT the PCC, the planar mirrors or the IBL chain length: those are the
        // SCENE's, and every host binds the pass's own scene's per pass
        // (bindSceneGi). Nor the Photon volumes, which no host holds (OWN-GI-1).
        // THE LTC MATRIX: loaded once and never unloaded (OgreLights.cpp); the host
        // retrieves the same pooled textures.
        if (pbs->getLtcMatrixTexture() && !host->getLtcMatrixTexture()) host->loadLtcMatrix();
    }
}

void OgreEngine::registerCommonMaterials() {
    try {
        Ogre::ResourceGroupManager &rgm = Ogre::ResourceGroupManager::getSingleton();
        const std::string group = Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME;
        // Registered folder by folder like Ogre's own resources2.cfg: a recursive
        // location does not resolve bare shader file names in subfolders.
        const char *dirs[] = { "2.0/scripts/materials/Common", "2.0/scripts/materials/Common/Any",
                               "2.0/scripts/materials/Common/GLSL", "2.0/scripts/materials/Common/HLSL",
                               "2.0/scripts/materials/Common/Metal",
                               "Hlms/Common/Any", "Hlms/Common/GLSL", "Hlms/Common/HLSL", "Hlms/Common/Metal",
                               // The voxel LightInjection compute job includes PBS pieces (area-light
                               // LTC, JahDiffuseAlbedo) by bare file name through the resource system.
                               "Hlms/Pbs/Any",
                               // PHOTON'S OWN MEDIA (OWN-GI-1; irisgl engine/media/Photon, staged
                               // beside the binary): the voxeliser/lighting compute jobs
                               // (Voxelizer.material.json) — Ogre's VCT folder is not registered
                               // at all, our copy replaces it — and, below, the field's.
                               "Photon/Voxel",
                               // The IBL specular integrator the PCC probe workspace's
                               // ibl_specular pass wants (falls back to mips if absent).
                               "Compute/Tools", "Compute/Tools/Any", "Compute/Tools/GLSL",
                               "Compute/Tools/HLSL", "Compute/Tools/Metal",
                               "Compute/Algorithms/IBL",
                               // DDGI's five compute jobs (GI_UNIFIED_SPEC §4
                               // P1). ONE location, flat folder — the .any
                               // pieces sit beside the per-syntax shaders, like
                               // IBL's.
                               "Photon/IrradianceField",
                               // THE PHOTON VIEW's two Ogre pictures (PHOTON-VIEW-1):
                               // the debug materials (upstream's, in our copy) for the lit voxels
                               // (PhotonVoxelLighting::setDebugVisualization) and the field's
                               // probe spheres (PhotonIrradianceField::setDebugVisualization),
                               // parsed here and compiled only when a view first asks.
                               "Photon/Voxel/Visualizer",
                               "Photon/IrradianceField/Visualizer",
                               // Post chain (POST_CHAIN_SPEC.md §4.1). The Vulkan
                               // (glslvk) programs source the SAME .glsl files as
                               // the GL ones, so GLSL is the folder that matters;
                               // the others are staged for the other backends.
                               // SMAA also loads AreaTexDX10.dds / SearchTex.dds
                               // from its own folder.
                               "2.0/scripts/materials/HDR",
                               "2.0/scripts/materials/HDR/GLSL",
                               "2.0/scripts/materials/HDR/HLSL",
                               "2.0/scripts/materials/HDR/Metal",
                               "2.0/scripts/materials/Tutorial_SSAO",
                               "2.0/scripts/materials/Tutorial_SSAO/GLSL",
                               "2.0/scripts/materials/Tutorial_SSAO/HLSL",
                               "2.0/scripts/materials/Tutorial_SSAO/Metal",
                               "2.0/scripts/materials/Tutorial_SMAA",
                               "2.0/scripts/materials/Tutorial_SMAA/GLSL",
                               "2.0/scripts/materials/Tutorial_SMAA/HLSL",
                               "2.0/scripts/materials/Tutorial_SMAA/Metal",
                               "2.0/scripts/materials/Tutorial_SMAA/Vulkan",
                               // Jahshaka's own pieces (fog) + the PCC probe compositor.
                               // The PCC probe compositor (the fog/UV pieces in this
                               // folder reach HlmsPbs as a library path above, not
                               // through the resource system).
                               "Hlms/Jahshaka" };
        for (const char *d : dirs) rgm.addResourceLocation(mMediaDir + d, "FileSystem", group, false);
        // The overlay font pack (DebugFont.fontdef + Inconsolata-Bold.ttf + its
        // OFL licence), staged unzipped by irisgl/engine/CMakeLists.txt. Added
        // HERE, in the same group, so the one initialiseAllResourceGroups below
        // parses the .fontdef — a font location registered after it would never
        // be scanned.
        hud::addFontLocation(mMediaDir, group);
        rgm.initialiseAllResourceGroups(true);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: common material scripts not registered: " + e.getFullDescription());
    }
}

void OgreEngine::createShadowNode() {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    if (!cm->hasShadowNodeDefinition(OgreView::kShadowNodeName))
        buildShadowNode(OgreView::kShadowNodeName, mShadowResolution, mShadowMapCount,
                        mShadowPerMapClears);
    // The planar-reflection pass's own atlas, at HALF resolution. Definitions
    // are free — the VRAM is only allocated where a workspace instantiates one,
    // which for reflections is one atlas PER BUDGET SLOT. At the default 2048 a
    // shared full-resolution node would cost ~56 MB per slot; half is ~14 MB,
    // and nobody has ever measured shadow-map resolution inside a mirror.
    //
    // ITS FOCUSED COUNT FOLLOWS THE MAIN NODE'S DERIVED COUNT (ENGINE_CACHE_
    // POLICY_SPEC D3 = A, lead decision 2026-09-12). It used to hold two
    // whatever the main atlas grew to, which made every lamp past the second
    // shadowless inside a mirror AND kept the reflection off the lamp-map cache
    // (a node caches only while its lamps fit its maps, applyShadowCacheDirties).
    // With the count matched, a mirror reuses every lamp's cached map: at rest
    // its lamp maps cost nothing, the whole point of P5.
    //
    // ...and it clears the way the view node does: per-map quads while the
    // process holds any cacheable lamp, one whole-atlas clear otherwise
    // (buildShadowNode's switch) — a whole-atlas clear would wipe the cached
    // maps every frame.
    if (!cm->hasShadowNodeDefinition(OgreView::kReflectShadowNodeName))
        buildShadowNode(OgreView::kReflectShadowNodeName,
                        std::max(256u, mShadowResolution / 2u), mShadowMapCount,
                        mShadowPerMapClears);
    // The PROBE-CAPTURE node (OgreView::kProbeShadowNodeName has the numbers):
    // instantiated once PER REFLECTION PROBE by the PCC / raster-IFD probe
    // workspaces, so it is the main atlas's layout at a quarter of the
    // resolution (512 at High — the largest probe face), the same derived
    // focused count capped at four and a scratch cube of R/2. It is rebuilt with
    // the other two whenever the resolution, the derived count or the clear
    // strategy changes — the last because a probe's lamp maps are CACHED too
    // (ENGINE_CACHE_POLICY_SPEC P4): a capture's first face renders a dirty
    // lamp map and the other five reuse it, which per-map clears make possible.
    if (!cm->hasShadowNodeDefinition(OgreView::kProbeShadowNodeName)) {
        const unsigned probeRes = probeShadowResolution(mShadowResolution);
        buildShadowNode(OgreView::kProbeShadowNodeName, probeRes,
                        std::min(mShadowMapCount, kProbeShadowMaxFocusedMaps),
                        mShadowPerMapClears, probeRes / 2u);
    }
}

// ---------------------------------------------------------------------------
// THE VALIDATION LAYER'S PROOF (TESTING-CLEANUP-2 H4; Types.h ValidationStatus). The probe
// tests/atom ran alone (its validation_probe.h, deleted), generalised: with VK_LAYER_KHRONOS_validation
// live on a device, that device's own entry points resolve INTO the layer's library (it
// intercepts vkGetDeviceProcAddr), so where vkCmdDraw resolves is the proof.
// ---------------------------------------------------------------------------
namespace {
#if defined(__linux__)
int collectLayerLibrary(struct dl_phdr_info *info, size_t, void *data) {
    if (!info->dlpi_name || !*info->dlpi_name) return 0;
    const std::string path(info->dlpi_name);
    const std::string base = path.substr(path.find_last_of('/') + 1);
    if (base.find("VkLayer_") != std::string::npos)
        static_cast<std::vector<std::string> *>(data)->push_back(base);
    return 0;
}
#endif
bool namesValidation(const char *value) {
    if (!value) return false;
    std::string v(value);
    for (char &c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
    return v.find("validation") != std::string::npos;
}
}   // namespace

ValidationStatus OgreEngine::validation() const {
    JAH_SYNC_GETTER("Engine::validation");
    ValidationStatus s;
    s.requested = namesValidation(std::getenv("VK_INSTANCE_LAYERS")) ||
                  namesValidation(std::getenv("VK_LOADER_LAYERS_ENABLE"));
#if defined(__linux__)
    dl_iterate_phdr(collectLayerLibrary, &s.layers);
#elif defined(__APPLE__)
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; ++i) {
        const char *name = _dyld_get_image_name(i);
        if (!name) continue;
        const std::string path(name);
        const std::string base = path.substr(path.find_last_of('/') + 1);
        if (base.find("VkLayer_") != std::string::npos) s.layers.push_back(base);
    }
#endif
    // THE LAYER IS LOADED: the loader maps VkLayer_khronos_validation only when an instance enables
    // it — the readout every build has, ray query or not (TESTING-CLEANUP-2 fix round).
    bool layerLoaded = false;
    for (const std::string &l : s.layers)
        if (l.find("VkLayer_khronos_validation") != std::string::npos) layerLoaded = true;
    s.active = layerLoaded;
#if JAH_RAY_QUERY
    // ...AND, where this build reaches the Vulkan device, ON THE DEVICE: its vkCmdDraw resolves
    // into the layer's library (the stronger proof; a build without the device headers keeps the
    // first).
    auto *vkRs = mRoot ? dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem()) : nullptr;
    if (vkRs && vkRs->getVulkanDevice() && vkRs->getVulkanDevice()->mDevice) {
        void *fn = reinterpret_cast<void *>(vkGetDeviceProcAddr(vkRs->getVulkanDevice()->mDevice, "vkCmdDraw"));
        Dl_info info{};
        if (fn && dladdr(fn, &info) && info.dli_fname) s.drawEntry = info.dli_fname;
        s.active = layerLoaded && s.drawEntry.find("VkLayer_khronos_validation") != std::string::npos;
    }
#endif
    return s;
}

}  // namespace detail

bool Engine::isAlive() { return detail::gLiveEngine != nullptr || Ogre::Root::getSingletonPtr() != nullptr; }

std::unique_ptr<Engine> Engine::create(const EngineConfig &cfg, std::string &error) {
    if (isAlive()) {
        error = "an Engine already exists in this process; destroy it before creating another";
        return nullptr;
    }
    if (cfg.pluginDir.empty())    { error = "EngineConfig::pluginDir is empty";    return nullptr; }
    if (cfg.hlmsMediaDir.empty()) { error = "EngineConfig::hlmsMediaDir is empty"; return nullptr; }
    auto engine = std::unique_ptr<detail::OgreEngine>(new detail::OgreEngine());
    detail::gLiveEngine = engine.get();
    // THE RENDER THREAD (U3 stage 4b): Root and the device are made on it; create returns when they are up.
    if (cfg.renderThread) {
        detail::OgreEngine *e = engine.get();
        if (!e->startRenderThread(cfg, error)) return nullptr;   // ~OgreEngine joins the thread
        return engine;
    }
    detail::gRenderThreadId.store(std::this_thread::get_id(), std::memory_order_relaxed);
    if (!engine->init(cfg, error)) return nullptr;   // ~OgreEngine clears gLiveEngine
    return engine;
}

}}  // namespace jahshaka::engine
