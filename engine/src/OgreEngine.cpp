// The Ogre-Next 4.0 backend: the Engine object itself (Root, render systems, Hlms
// registration, views, scenes, the frame loop) and the Engine::create factory.
//
// engine/src/ is the only directory that includes Ogre; the shared declarations
// live in EnginePrivate.h, which documents the invariants this backend rests on.
#include "EnginePrivate.h"

#include <set>
#include <unistd.h>

#include <OgreFrameStats.h>
// The frame loop's clock. `Root::getTimer()` returns `Ogre::Timer *` and
// OgreRoot.h forward-declares it only — the inlined FrameStats sample
// (renderOneFrame, mirroring OgreRoot.cpp:1123) calls through it.
#include <OgreTimer.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <thread>

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

bool OgreEngine::init(const EngineConfig &cfg, std::string &error) {
#ifdef __linux__
    mDisplay = reinterpret_cast<void *>(cfg.display);
#else
    (void)cfg.display;  // X11-only; 0 on other hosts (Types.h documents the leak)
#endif
    mDefaultSamples = OgreView::sanitizeSamples(cfg.sampleCount);
    mVsync = cfg.vsync;
    // THE HARDWARE RAY-TRACING PREFERENCE at boot (PHOTON_SPEC §7 R1): the tier
    // comes up only where the device advertises it AND this says yes.
    // `EngineConfig::rayTracing` is the HOST's answer — Studio fills it from the
    // application preference (Preferences > Rendering) ANDed with
    // --no-ray-query. The environment variable is kept as the override a suite
    // can set when it cannot reach the config (it is also what ogre-patch 0038
    // reads at vkCreateDevice, so the two must agree).
    mRayTracingWanted = cfg.rayTracing && getenv("JAHSHAKA_NO_RAY_QUERY") == nullptr;
    // Process-wide static, read by Mesh::prepareForShadowMapping at mesh-build
    // time (POST_CHAIN_SPEC.md §11). Setting it before Root exists is fine — it
    // is a plain static, not engine state.
    Ogre::Mesh::msOptimizeForShadowMapping = cfg.optimizeShadowMeshes;
    mMediaDir = cfg.hlmsMediaDir;
    if (!mMediaDir.empty() && mMediaDir.back() != '/') mMediaDir += '/';
    mHeadless = cfg.headless;
    // The shader cache's fingerprint hashes the staged Hlms tree, so it is
    // configured as soon as the media directory is known — before Root, long
    // before anything could compile. The LOAD waits for ensureHlms().
    //
    // NEVER under the NULL render system: a headless run compiles nothing worth
    // keeping, and the fingerprint (§4.2) does not name the render system — a
    // blob written here would be offered verbatim to a real Vulkan device on
    // the next launch. Silently off, not an error: the caller's config is a
    // reasonable thing to reuse between a windowed and a headless run.
    mShaderCache.configure(cfg.headless ? std::string() : cfg.shaderCacheDir,
                           cfg.appBuildId, mMediaDir);
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
        textureCache().configure(cfg.headless || cacheOff ? std::string() : cfg.shaderCacheDir,
                                 cfg.appBuildId);
    }
    try {
        mAbiCookie = Ogre::generateAbiCookie();
        mRoot = new Ogre::Root(&mAbiCookie, "", "",
                               cfg.logFile.empty() ? "jahshaka-ogre.log" : cfg.logFile,
                               "Jahshaka");
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
        const char *plugin = cfg.headless      ? "RenderSystem_NULL"
                             : (cfg.backend == Backend::Vulkan) ? "RenderSystem_Vulkan"
                                                                : "RenderSystem_GL3Plus";
        // ---- OPENXR, STEP 1 (SPECS/VR_SPEC.md §4.1) -----------------------
        // BEFORE loadPlugin, because the Vulkan render system reads
        // `external_instance` in its CONSTRUCTOR, which is what loadPlugin
        // runs. On the vulkan_enable2 route the RUNTIME creates the VkInstance
        // (from our own VkInstanceCreateInfo) and later the VkDevice (from the
        // VkDeviceCreateInfo ogre-patch 0068 exports), and Ogre runs on both.
        //
        // NOTHING HERE IS FATAL. No loader, no manifest, no runtime, no headset
        // on the cable: the reason is logged and recorded in vrInfo(), the
        // plain boot continues unchanged, and vrAvailable() is false for the
        // life of the process. A plain boot never enters this branch at all.
        Ogre::NameValuePairList vrPluginOpts;
        Ogre::NameValuePairList *pluginOpts = nullptr;
        // THE REASON IS ALWAYS A SENTENCE, including the commonest one of all:
        // "nobody asked". A host that reads vrInfo().reason on a plain boot
        // must get an answer it can show a user, not an empty string.
        mVrInfo.reason = cfg.vr == VrMode::Disabled
                             ? "VR was not requested for this process (start it with --vr)"
                         : cfg.headless
                             ? "this engine is headless (the NULL render system renders nothing)"
                         : cfg.backend != Backend::Vulkan
                             ? "VR needs the Vulkan backend"
                             : "";
        if (cfg.vr == VrMode::IfAvailable && !cfg.headless &&
            cfg.backend == Backend::Vulkan) {
            std::string why;
            mVrBoot = vr::bootBegin(mVrInfo, why);
            if (!mVrBoot) {
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka VR: not available - " + why, Ogre::LML_NORMAL);
            } else if (void *ext = vr::bootExternalInstance(mVrBoot)) {
                vrPluginOpts["external_instance"] =
                    Ogre::StringConverter::toString(uintptr_t(ext));
                pluginOpts = &vrPluginOpts;
            }
        }
        mRoot->loadPlugin(cfg.pluginDir + "/" + plugin, false, pluginOpts);
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
        // nothing from the machine. Keeping the pointer means documentGraphScene
        // and the (refused) view calls never make a second one.
        //
        // A RENDERING boot stays exactly as it always was — initialise(false),
        // no window, no device — so the first window a Vulkan session creates
        // is the host's real one whenever the host can wait that long.
        mNullWindow = mRoot->initialise(cfg.headless, "jahshaka-headless");
        // ---- OPENXR, STEP 2 -----------------------------------------------
        // AFTER initialise and BEFORE any window: ogre-patch 0068's exporter
        // reads the instance-extension list the render system's constructor
        // filled, so a request built earlier silently loses the feature chain
        // (phase 1a's §8.5 — the one correction the spike made to the spec's
        // order). The device it creates is consumed by the FIRST
        // createRenderWindow, whichever of the three that turns out to be.
        if (mVrBoot) {
            std::string why;
            if (!vr::bootDevice(mVrBoot, mRoot, mVrInfo, why)) {
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka VR: not available - " + why, Ogre::LML_CRITICAL);
                // THE INSTANCE STAYS (it is already Ogre's), THE BOOT GOES: the
                // engine runs on it exactly as it runs on its own, and
                // vrAvailable() answers false. Deleting the boot here would
                // destroy the VkInstance the render system is holding.
                mVrDeviceFailed = true;
            }
        }
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
        return true;
    } JAH_CATCH(error, false);
}

void *OgreEngine::documentGraphScene() {
    JAH_TRY {
        if (mDocumentScene) return mDocumentScene;
        // The same one-time preparation createOffscreenView does. A scene
        // manager cannot exist before a Window and before the Hlms/resource
        // registration (CLAUDE.md's startup-order fact; the crash is inside
        // SceneManager's constructor, which looks up a material the common
        // scripts define). The host asks for this scene once, before its first
        // document node, so that from then on "an engine exists" and "a
        // document node has real graph storage" are the same statement.
        //
        // HEADLESS runs never enter the branch below: the NULL render system
        // created mNullWindow itself inside init(), so the rule is already
        // satisfied and this call only registers the Hlms and makes the scene
        // manager. A RENDERING run that asks before its first View is what
        // makes the surfaceless window happen — see Engine.h.
        if (!mHlmsRegistered && !mNullWindow) {
            Ogre::NameValuePairList wp; wp["windowType"] = "null";
            applyVrExternalDevice(wp);
            mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"),
                                                    8, 8, false, &wp);
        }
        ensureHlms();
        if (!mHlmsRegistered) { mLastError = "documentGraphScene: Hlms unavailable"; return nullptr; }
        // NO worker threads and no Forward+ setup: nothing in here is ever
        // culled, lit or drawn. It exists to hold nodes.
        //
        // ZERO, NOT ONE (THREADING_ADOPTION_SPEC.md P5). This asked for 1 until
        // the hygiene phase, which spawned a thread and paid two barrier syncs
        // per parallel pass so that a scene NOBODY EVER DRAWS could do its
        // serial work on someone else's stack. At 0 Ogre sets mForceMainThread
        // and runs those passes inline with no barrier and no thread
        // (OgreSceneManager.cpp:171, :4705-4717). The frame loop has skipped
        // this manager entirely since P3, so in practice the passes do not run
        // at all — but the THREAD was created regardless, once per process.
        mDocumentScene = mRoot->createSceneManager(Ogre::ST_GENERIC, 0u,
                                                   processUniqueName("jahshaka-document"));
        return mDocumentScene;
    } JAH_CATCH(mLastError, nullptr);
}

// THE BLANK WORLD (lane STALE-VIEW-1). A compositor workspace needs a
// SceneManager and a camera, and a View with no scene has neither — so the
// clear-only chain a scene-less View owns (chain::buildBlank) runs against this
// one. It holds nothing, ever: no item, no light, no forward+ setup. The only
// pass that names it renders the OVERLAY queues, which are screen-space, and
// the only object it owns is one camera per View that has ever lost a scene.
//
// ZERO WORKER THREADS, for exactly the reason mDocumentScene takes zero
// (THREADING_ADOPTION_SPEC.md P5): at 0 Ogre sets mForceMainThread and runs the
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

Scene *OgreEngine::createScene(const std::string &name, unsigned workerThreads) {
    if (!mHlmsRegistered) {
        mLastError = "createScene('" + name + "'): no View exists yet — create a View first";
        return nullptr;
    }
    for (auto &s : mScenes)
        if (s->name() == name) { mLastError = "Scene '" + name + "' already exists"; return nullptr; }
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
        Ogre::SceneManager *sm = mRoot->createSceneManager(Ogre::ST_GENERIC, threads, name);
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
        mScenes.emplace_back(new OgreScene(mRoot, sm, name, mLastError));
        // The scene's per-scene shadow request resolves against GLOBAL engine
        // state (one filter, one atlas — Scene::setShadowSettings), so it needs
        // its engine.
        mScenes.back()->mEngine = this;
        return mScenes.back().get();
    } JAH_CATCH(mLastError, nullptr);
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
    if (!scene) return;
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
            mVrMirrorView = nullptr;
        }
        for (auto &v : mViews)
            if (v->scene() == scene) v->detachScene();
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
        const bool releasedPcc = (*it)->mReleasedPccOnDestroy;
        mScenes.erase(it);
        // A scene taking the process-wide probe binding down with it lets every
        // REMAINING scene bind its own sky cube again (reflectionTexForDatablocks'
        // note). After the erase: the walk must not see the corpse.
        if (releasedPcc) reapplyReflectionsAllScenes();
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
        // (ogre-patches 0007) hosts its OWN CAMetalLayer-backed child view inside
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
        applyVrExternalDevice(params);
        Ogre::Window *window = mRoot->createRenderWindow(name, width, height, false, &params);
        window->setVSync(mVsync, 1u | kLowestLatencyVSync);
        ensureHlms();
        mViews.emplace_back(new OgreView(mRoot, window, nullptr, name, width, height,
                                         background, mLastError));
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
            win->setVSync(mVsync, 1u | kLowestLatencyVSync);
            return win;
        };
        return view;
#endif  // __APPLE__
    } JAH_CATCH(mLastError, nullptr);
#endif  // !__linux__ && !__APPLE__
}

View *OgreEngine::createOffscreenView(const std::string &name, unsigned width, unsigned height,
                                      const Colour &background) {
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
            applyVrExternalDevice(wp);
            mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"),
                                                    8, 8, false, &wp);
        }
        ensureHlms();   // retried on every call until it succeeds (e.g. bad media dir)
        Ogre::TextureGpu *rtt = OgreView::createRtt(mRoot, processUniqueName("rtt"), width, height);
        mViews.emplace_back(new OgreView(mRoot, nullptr, rtt, name, width, height,
                                         background, mLastError));
        mViews.back()->mEngine = this;
        return mViews.back().get();
    } JAH_CATCH(mLastError, nullptr);
}

void OgreEngine::destroyView(View *view) {
    if (!view) return;
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
        return;
    }
    mLastError = "destroyView: unknown View";
}

void OgreEngine::reapplyReflectionsAllScenes() {
    // EVERY scene, including the ones nothing is drawing right now: a preview or
    // a thumbnail scene that is re-shown later must already hold the right
    // answer, and the walk is a handful of datablock binds per scene on a
    // transition that happens when a grid is built or torn down.
    for (auto &s : mScenes) {
        if (!s) continue;
        s->applyReflectionToAll();
        // ...and the roughness-to-LOD map with it: the grid that just came or
        // went pushed ITS mip count into the one number the whole pass shares,
        // and only a transition can leave that number describing a texture
        // nobody is sampling any more (OgreScene::renotifyReflectionMipmaps).
        s->renotifyReflectionMipmaps();
    }
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
            if (v && v->isEnabled() && v->scene() == s.get()) { out.push_back(s.get()); break; }
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
    catch (Ogre::Exception &e) { try { note(e.getFullDescription()); } catch (...) {} }
    catch (std::exception &e)  { try { note(std::string("engine: ") + e.what()); } catch (...) {} }
    catch (...)                { note("engine: an unknown exception closing the frame"); }
}
}   // namespace

const std::atomic<unsigned long long> *gTransformWriteCounter = nullptr;

void OgreEngine::setTransformWriteCounter(const std::atomic<unsigned long long> *counter) {
    // PROCESS-WIDE, like the counter it names: one document graph feeds every
    // scene in the process, and a second host handing over the same address
    // changes nothing. Null clears it and every scene goes back to scanning
    // every frame — which is correct, not a failure mode.
    detail::gTransformWriteCounter = counter;
}

void OgreEngine::renderOneFrame() {
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
    struct FrameScope {
        OgreEngine *self;
        explicit FrameScope(OgreEngine *s) : self(s) { self->mInRenderFrame = true; }
        ~FrameScope() { self->closeRenderFrame(); }
    } frameScope(this);
    JAH_TRY {
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
        if (mVrSession) vrSessionBeginFrame(mVrSession);
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
            monitor::gMonitor->beginFrame(mShadowFrame + 1ull, mNextFrameCause, onscreen);
        }
        mNextFrameCause = FrameCause::Driver;
        // WHAT THIS FRAME MAY PUT OFF (OPEN_COVER_SPEC §2.1), consumed exactly
        // like the cause and reset to `Complete` — so a caller that sets
        // nothing renders the frame it always did. Pushed to every scene here,
        // once, because the decisions it changes are per scene (the GI arm's
        // staged build) and per frame (the texture drain below).
        const FramePace pace = mNextFramePace;
        mNextFramePace = FramePace::Complete;
        for (auto &sc : mScenes) sc->setFramePace(pace);
        std::unique_ptr<monitor::Stage> monPre;
        if (monitor::live()) {
            monPre.reset(new monitor::Stage("engine.pre"));
            // BEFORE anything renders and outside every encoder — the only
            // place a Vulkan query pool may be reset (ogre-patch 0027).
            gpuFrameBegin();
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
        for (auto &v : mViews) {
            const bool authoritative = v->isEnabled() && v->ogreScene() &&
                                       *driverSlot(v->ogreScene()) == v.get();
            v->applyPendingResize(); v->updateParticles();
            if (authoritative) v->updateGi();
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
            // ...and the reflection trace's hook (PHOTON_SPEC §7 R5), for the
            // third time and the third reason: the chain rebuilds when the SSR
            // row or the machine's answer changes, the scene's voxel arm is
            // rebuilt behind the trace's back, and the camera is recreated on
            // setScene.
            v->syncReflectListener();
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
        for (auto &s : mScenes)
            if (drawnThisFrame(s.get())) {
                // THE SKY AMBIENT'S DEFERRED READ (audit ON-14), first: it is a
                // `queryIsTransferDone` and, when the copy has landed, a map
                // and 6 x 1024 texels of integral. Never a wait — see
                // OgreScene::pollSkyShRead — and here rather than beside the
                // capture because a read issued inside a frame must not be
                // polled in that same frame.
                s->pollSkyShRead();
                s->applyPendingGi(); s->applyPendingIbl(); s->applyPendingPlanar();
                // SURFACE-CACHE, after the pendings: the cache reads the
                // material generation and the light write serial that
                // applyPendingGi may just have moved, and PLANS this frame's
                // capture batch. The capture itself runs inside Root's frame
                // (its workspace is enabled and first in the manager's list),
                // after the frame's scene-graph update and light list — the
                // light list a capture planned here used to run without.
                s->updateSurfaceCache();
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
                    if (v->scene() != s.get() || !v->isEnabled()) continue;
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
        {
            const OgreView *owner = nullptr;
            for (auto &v : mViews) {
                if (!v->isEnabled() || !v->overlaysAllowed() || !v->overlay().anything()) continue;
                owner = v.get();
                break;
            }
            if (owner) {
                RenderStats stats;
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
                {
                    std::unique_ptr<monitor::Stage> st;
                    if (monitor::live()) st.reset(new monitor::Stage("engine.sceneGraph"));
                    for (OgreScene *s : updated) s->sceneManager()->updateSceneGraph();
                    if (blankUpdated) blankUpdated->updateSceneGraph();
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
                // THE RECORD/SWAP SPLIT. One call does both; the frame listener
                // marks the instant between them (see FrameSplitListener).
                const auto monFrameStart = std::chrono::steady_clock::now();
                if (monitor::live()) monitor::gMonitor->mSplit.mMarked = false;
                const bool monRendered = mRoot->_updateAllRenderTargets();
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
            if (mFrameFaultLeft) raiseFrameFault();
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
    } JAH_CATCH(mLastError, );
    // NOTHING BELONGS HERE. Every line that used to follow the catch ran on the
    // normal path only (JAH_CATCH returns); the frame's close is
    // `closeRenderFrame`, called by the scope guard above.
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
    // ---- 1. the runtime's frame ------------------------------------------
    frameCloseStep(mLastError, [this] {
        if (mVrSession) vrSessionEndFrame(mVrSession);
    });
    // ---- 2. the monitor's record -----------------------------------------
    frameCloseStep(mLastError, [this] {
        if (monitor::live() && monitor::gMonitor->inFrame())
            monitor::gMonitor->endFrame(mUpdatedScenes);
    });
    // ---- 3. a lost or stopped session ------------------------------------
    frameCloseStep(mLastError, [this] { endLostOrStoppedVrSession(); });
    // ---- 4. the device-lost latch ----------------------------------------
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
        mVrMirrorView = nullptr;
    }
}

// THE GPU IS GONE (lane XID-2, 2026-09-17). Said ONCE, loudly, the moment
// the render system reports it: from here on the render system vetoes every
// frame (ogre-patch 0072 -- it no longer tries to recreate the device, which
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
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka: THE GPU DEVICE WAS LOST. The session cannot continue; the renderer "
        "does not recreate a lost device. Look for an 'NVRM: Xid' line in the system "
        "log at this time (journalctl -k | grep -i xid).",
        Ogre::LML_CRITICAL);
}

// ---------------------------------------------------------------------------
// THE INJECTED FRAME FAULT (Engine::setFrameFault; FrameFault's note in
// Types.h). Test-facing: the engine's frame close cannot be asserted without a
// frame that throws, and nothing a suite may legally ask of the engine throws
// out of a frame.
void OgreEngine::setFrameFault(FrameFault fault, unsigned frames) {
    mFrameFault = (fault == FrameFault::None || frames == 0u) ? FrameFault::None : fault;
    mFrameFaultLeft = mFrameFault == FrameFault::None ? 0u : frames;
    // The device-lost REPORT is armed with the fault and disarmed with it, so a
    // disarm cannot leave the latch's input stuck true for the rest of the
    // process. (The latch itself is one-way, by design: a lost device never
    // comes back.)
    if (mFrameFault != FrameFault::ThrowDeviceLost) mFrameFaultDeviceLost = false;
}

void OgreEngine::raiseFrameFault() {
    if (!mFrameFaultLeft) return;
    // The kind is read BEFORE the run is spent: the LAST faulting frame of a
    // ThrowDeviceLost run is the one that matters most, and reading it after
    // the disarm below would report that one as a plain throw.
    const FrameFault fault = mFrameFault;
    if (--mFrameFaultLeft == 0u) mFrameFault = FrameFault::None;
    if (fault == FrameFault::ThrowDeviceLost) mFrameFaultDeviceLost = true;
    OGRE_EXCEPT(Ogre::Exception::ERR_INTERNAL_ERROR,
                "Jahshaka: an injected frame fault (Engine::setFrameFault) - TEST ONLY",
                "OgreEngine::renderOneFrame");
}

// ---------------------------------------------------------------------------
bool OgreEngine::deviceLost() const { return mDeviceLost; }

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
// not its documented practice — the two upstream offline capture paths one
// might cite (OgreParallaxCorrectedCubemapAuto.cpp:385-388,
// OgreIrradianceFieldRaster.cpp:284-288) complete the bracket with
// `_endFrameOnce()` every time. AND IT COMMITS ONLY EVERY SECOND CALL:
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

void OgreEngine::applyVrExternalDevice(Ogre::NameValuePairList &params) {
    // ONLY THE FIRST WINDOW CAN CONSUME IT (VR_SPEC §2.1 row 5): the render
    // system reads `external_device` while `!mInitialized`, and a second window
    // carrying it would be read by nobody. Three call sites ask, in whichever
    // order the host happens to create things; exactly one of them wins.
    if (!mVrBoot || mVrDeviceFailed || mVrDeviceConsumed) return;
    void *ext = vr::bootExternalDevice(mVrBoot);
    if (!ext) return;
    params["external_device"] = Ogre::StringConverter::toString(uintptr_t(ext));
    mVrDeviceConsumed = true;
}

bool OgreEngine::beginVrSession(Scene *scene, const VrConfig &cfg) {
    if (!vrAvailable()) {
        mLastError = "beginVrSession: no OpenXR runtime (" +
                     (mVrInfo.reason.empty() ? std::string("VR is disabled for this process")
                                             : mVrInfo.reason) + ")";
        return false;
    }
    if (mVrSession) { mLastError = "beginVrSession: a session is already running"; return false; }
    OgreScene *s = nullptr;
    for (auto &candidate : mScenes)
        if (candidate.get() == scene) s = candidate.get();
    if (!s) { mLastError = "beginVrSession: no such scene"; return false; }
    JAH_TRY {
        std::string why;
        mVrSession = vr::sessionBegin(mVrBoot, this, s, cfg, why);
        if (!mVrSession) { mLastError = "beginVrSession: " + why; return false; }
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
    if (!mVrSession) return;
    // CLEARED FIRST, THEN DELETED. The session's destructor calls back into
    // this engine (destroyView for its own View), and anything that re-entered
    // through a live `mVrSession` pointer would be talking to a half-destroyed
    // object. After this line there is no session, which is the truth every
    // caller should see while one is being taken apart.
    VrSession *session = mVrSession;
    mVrSession = nullptr;
    // ...AND NO SESSION LEAVES ITS OWN BEHIND (finding 1). A suite that
    // injected through a live session (with the override on) would otherwise
    // hand the next one, or the headless verbs after it, a hand nobody wrote.
    vrClearInjectedInput();
    JAH_TRY {
        vr::sessionEnd(session);
    } JAH_CATCH(mLastError, );
}

VrState OgreEngine::vrState() const {
    if (!mVrSession) return vrAvailable() ? VrState::Idle : VrState::Unavailable;
    return vrSessionState(mVrSession);
}

VrStatus OgreEngine::vrStatus() const {
    if (!mVrSession) {
        VrStatus s;
        s.state = vrState();
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
    VrStatus s = vrSessionStatus(mVrSession);
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

View *OgreEngine::vrView() const { return mVrSession ? vrSessionView(mVrSession) : nullptr; }

bool OgreEngine::vrEyeScreenshot(unsigned eye, Image &out) {
    if (!mVrSession) { mLastError = "vrEyeScreenshot: no session is running"; return false; }
    return vrSessionEyeScreenshot(mVrSession, eye, out, mLastError);
}

void OgreEngine::setVrMirrorView(View *view) {
    mVrMirrorView = static_cast<OgreView *>(view);
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
    return mVrSession ? vrSessionBindingBlocks(mVrSession, out, count) : 0u;
}

bool OgreEngine::vrInjectJoints(int hand, const VrPose *joints, unsigned count) {
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
    if (hand < 0 || hand >= int(VrHandCount)) {
        mLastError = "vrHaptic: hand must be 0 (left) or 1 (right)";
        return false;
    }
    if (!mVrSession) { mLastError = "vrHaptic: no session is running"; return false; }
    return vrSessionHaptic(mVrSession, hand, amplitude01, seconds, mLastError);
}

void OgreEngine::setVrOrigin(const Vec3 &position, float yawDegrees) {
    // NOT REMEMBERED ACROSS SESSIONS, deliberately: where the wearer stands is
    // a property of the RUN the host started, and a session that inherited the
    // previous one's origin would put the next wearer wherever the last one
    // walked to. A host sets it right after beginVrSession().
    if (mVrSession) vrSessionSetOrigin(mVrSession, position, yawDegrees);
}

void OgreEngine::advanceResources() {
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
        sm->updateSceneGraph();
        // PAIRED, and it matters: updateSceneGraph APPENDS to the manager's
        // global light list (buildLightList) and leaves render-queue state
        // behind. Root's frame clears both afterwards; a standalone update has
        // to do the same or repeated calls grow the light list without bound.
        sm->clearFrameData();
        return true;
    } JAH_CATCH(mLastError, false);
}

bool OgreEngine::hasEnabledViews() const {
    // A VR SESSION IS SOMETHING TO DRAW even when every View is off (the owner's
    // WiVRn smoke, 2026-09-17): the runtime answers "no picture" on its first
    // frames, the pump switches the session's View off for those frames, the
    // Player's View is off by design and the editor's is hidden — and a host
    // that skips renderOneFrame on "no enabled views" then never runs the pump
    // again, so xrWaitFrame is never called, the runtime never synchronises
    // and keeps answering "no picture" for ever: black in the headset, a spin
    // on the desktop. The frame loop IS the session's heartbeat.
    if (mVrSession) return true;
    // Offscreen views count: they are enabled by construction and something is
    // waiting on their pixels (a thumbnail, a preview dock, a scripted
    // screenshot). The only state this reports is View::setEnabled, which the
    // hosts drive from widget visibility.
    for (const auto &v : mViews)
        if (v->isEnabled()) return true;
    return false;
}

void OgreEngine::listViews(std::vector<View *> &out) const {
    out.clear();
    out.reserve(mViews.size());
    for (const auto &v : mViews) out.push_back(v.get());
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
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    if (!tm) return true;
    if (mTextureWaitBroken || mTextureWaitBudgetMs == 0u) return tm->isDoneStreaming();
    if (!mTextureWaitFault && tm->isDoneStreaming()) return true;

    Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsedMs = [&t0]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count();
    };
    const double budget = double(mTextureWaitBudgetMs);
    size_t bestPending = std::numeric_limits<size_t>::max();
    double lastProgressMs = 0.0;
    double lastPollMs = -1000.0;
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
    double lastAdvanceMs = -1.0e9;
    bool ok = true;

    for (;;) {
        const bool workerDone = tm->_update(true);
        if (!mTextureWaitFault && workerDone && tm->isDoneStreaming()) break;

        const double now = elapsedMs();
        // The pending census is O(textures in the process) — poll it every
        // 250 ms rather than every iteration. Below that interval the loop is
        // two mutex acquisitions and a millisecond of sleep.
        if (now - lastPollMs >= 250.0) {
            lastPollMs = now;
            const size_t pending = countPendingTextures(tm, nullptr);
            if (pending < bestPending) { bestPending = pending; lastProgressMs = now; }
        }
        if (now - lastProgressMs >= budget) {
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
            ok = false;
            break;
        }
        if (vao && now - lastAdvanceMs >= mTextureDrainAdvanceMs) {
            lastAdvanceMs = now;
            ++mTextureWaitAdvances;
            vao->_update();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const double spent = elapsedMs();
    if (spent > mTextureWaitWorstMs) mTextureWaitWorstMs = spent;
    if (msSpent) *msSpent = spent;
    return ok;
}

double OgreEngine::waitForTextureLoads() {
    double ms = 0.0;
    drainTextureStreaming(&ms);
    return ms;
}

unsigned long long OgreEngine::textureWaitAdvances() const { return mTextureWaitAdvances; }

unsigned long long OgreEngine::textureLoadRequests() const {
    Ogre::TextureGpuManager *tm = textureManagerOf(mRoot);
    return tm ? static_cast<unsigned long long>(tm->getLoadRequestsCounter()) : 0ull;
}

unsigned OgreEngine::textureMultiLoadThreads() const { return mMultiLoadThreads; }

unsigned OgreEngine::textureMetadataCacheEntries() const {
    return textureCache().metadataEntries(mRoot);
}

unsigned OgreEngine::textureChannelCacheEntries() const {
    return textureCache().channelEntries();
}

bool OgreEngine::saveTextureCache() {
    if (!mRoot) return false;
    JAH_TRY { return textureCache().save(mRoot); } JAH_CATCH(mLastError, false);
}

void OgreEngine::setVsync(bool on) {
    if (mVsync == on) return;
    mVsync = on;
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

const std::string &OgreEngine::lastError() const { return mLastError; }

std::string OgreEngine::takeLastError()
{
    // Swap, don't copy-then-clear: every OgreScene and OgreView holds
    // `std::string &mError` bound to THIS member (OgreEngine.cpp createScene /
    // createView), so the object must stay put — swapping its contents is fine,
    // reseating it would not be.
    std::string taken;
    taken.swap(mLastError);
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

void OgreEngine::setFixedFrameDelta(float seconds) {
    JAH_TRY {
        Ogre::ControllerManager::getSingleton().setFrameDelay(std::max(0.0f, seconds));
    } JAH_CATCH(mLastError, );
}

float OgreEngine::fixedFrameDelta() const {
    return float(Ogre::ControllerManager::getSingleton().getFrameDelay());
}

// Shadow filter and resolution (and the atlas rebuild they trigger) live in
// OgreShadow.cpp, beside the definition builder they drive.

void OgreEngine::setShadowMeshOptimization(bool on) { Ogre::Mesh::msOptimizeForShadowMapping = on; }
bool OgreEngine::shadowMeshOptimization() const { return Ogre::Mesh::msOptimizeForShadowMapping; }

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

ShaderCacheStats OgreEngine::shaderCacheStats() const {
    return mShaderCache.stats(mRoot);
}

bool OgreEngine::renderStats(RenderStats &out) const {
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
        // ---- geometry. LAZY, and this is the whole reason the accessor is not
        // a plain getter: recording is OFF by default in Ogre (OgreCommon.cpp:
        // 177) and costs integer adds per draw batch, so nothing that never
        // asks for stats ever pays for them. The FIRST call switches it on and
        // honestly reports metricsRecording=false with zeroed counters; every
        // call after a rendered frame reports real numbers.
        if (Ogre::RenderSystem *rs = mRoot->getRenderSystem()) {
            const Ogre::RenderingMetrics &m = rs->getMetrics();
            out.metricsRecording = m.mIsRecordingMetrics;
            if (!m.mIsRecordingMetrics) {
                rs->setMetricsRecordingEnabled(true);
            } else {
                out.draws     = (unsigned long long)m.mDrawCount;
                out.batches   = (unsigned long long)m.mBatchCount;
                out.triangles = (unsigned long long)m.mFaceCount;
                out.vertices  = (unsigned long long)m.mVertexCount;
                out.instances = (unsigned long long)m.mInstanceCount;
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
                // The node pools' USED slots, per hierarchy depth: getFirstNode
                // answers the count (the document's nodes are DETACHED roots in
                // the staging manager — createSceneNode, no parent — so a walk
                // from the Ogre root would miss every one of them).
                for (size_t depth = 0; depth < depths; ++depth) {
                    Ogre::Transform first;
                    out.simdNodes += unsigned(nmm.getFirstNode(first, depth));
                }
                out.simdObjects += unsigned(sm->_getEntityMemoryManager(type).getTotalNumObjects());
            }
            out.simdObjects += unsigned(sm->_getLightMemoryManager().getTotalNumObjects());
        };
        for (const auto &s : mScenes) if (s) walk(s->sceneManager());
        walk(mDocumentScene);
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
    if (!mRoot) return false;
    MemoryStats b;
    memoryStats(b);
    if (before) *before = b;
    JAH_TRY {
        for (const auto &s : mScenes)
            if (s && s->sceneManager()) s->sceneManager()->shrinkToFitMemoryPools();
        if (mDocumentScene) mDocumentScene->shrinkToFitMemoryPools();
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
    out = ObjectCounts();
    if (!mRoot) return false;
    JAH_TRY {
        out.views  = unsigned(mViews.size());
        out.scenes = unsigned(mScenes.size());
        out.updatedScenes = mUpdatedScenes;
        // STAGING SCENES ARE THEIR OWN ROW, not folded into `scenes`
        // (THREADING_ADOPTION_SPEC.md P5, audit F10). `mScenes` holds the
        // Scene OBJECTS this boundary handed out; the document's staging
        // manager is an Ogre::SceneManager with no Scene wrapper, no View, no
        // workspace and no place in the frame loop — invisible in this census
        // until now. Folding it into `scenes` would have made the two kinds
        // indistinguishable, and they behave nothing alike: this one is written
        // by the import worker and never drawn.
        //
        // At most one per engine (documentGraphScene is memoised); the
        // document's own fallback manager (iris::graph, "iris-staging") belongs
        // to a Root this engine may not own and is deliberately not counted
        // here — it exists only for hosts that never called setStagingScene.
        if (mDocumentScene) out.stagingScenes = 1u;
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
    if (!mRoot) return false;
    JAH_TRY { return mShaderCache.save(mRoot); } JAH_CATCH(mLastError, false);
}

bool OgreEngine::flushShaderCache(unsigned budgetMs) {
    JAH_TRY { return mShaderCache.flushWrites(budgetMs); } JAH_CATCH(mLastError, false);
}

bool OgreEngine::clearShaderCache() {
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

void OgreEngine::shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                                     unsigned &expected) const {
    mShaderCache.progress(compiled, fromCache, expected);
}

OgreEngine::~OgreEngine() {
    // THE VR SESSION GOES FIRST, and it has to: it owns a View (a workspace, a
    // camera, an RTT), a second workspace on somebody else's target and a set
    // of XR swapchains that name VkImages the runtime owns. Every one of those
    // is invalid the moment the loops below start, and the XR session must be
    // ended while its device is still alive (VR_SPEC §4.3's teardown order).
    // The BOOT — the VkInstance and VkDevice the runtime made — is destroyed at
    // the very bottom, AFTER Root: Ogre destroys neither (§2.1 row 8), and it
    // is using both until its own destructor has run.
    if (mVrSession) {
        VrSession *session = mVrSession;
        mVrSession = nullptr;      // see endVrSession: cleared first, then deleted
        try { vr::sessionEnd(session); } catch (...) {}
    }
    // The shadow-pass counter is a listener on a live workspace: unhook it
    // before anything that owns a workspace starts dying.
    detachShadowCounter();
    // Save the shader cache FIRST, while every Ogre singleton the three layers
    // read is still alive and before a single view or scene has been torn down.
    // This is the primary save point (SHADER_CACHE_SPEC §4.4): a clean quit is
    // the only moment we are certain nothing is compiling.
    if (mRoot) { try { mShaderCache.save(mRoot); } catch (...) {} }
    // The TEXTURE cache in the same breath and for the same reason
    // (THREADING_ADOPTION_SPEC.md P2): its metadata half is exported from
    // TextureGpuManager, which dies with the render system a few lines below.
    // The host also saves it explicitly at shutdown (EngineHost::shutdown, next
    // to saveShaderCache) — this is the point that runs even when the Engine
    // outlives that call, and save() is idempotent.
    if (mRoot) { try { textureCache().save(mRoot); } catch (...) {} }
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
    for (auto &s : mScenes) s->destroy();
    mScenes.clear();
    // The IES profile atlas and the area-light mask pool are process-wide, so
    // no scene owns them: free them here, while Root (and its texture manager)
    // is still alive.
    lightextras::shutdown();
    // AFTER every scene (each of which removed its own render-queue listener in
    // OgreScene::destroy) and BEFORE Root: ~OverlaySystem deletes the
    // FontManager, whose Font::unloadResource destroys the HlmsUnlit datablock
    // the font created. An OverlaySystem outliving Root is the same class of
    // bug as a MeshPtr outliving Root.
    // The blank world's render-queue listener comes off with the scenes' (each
    // removed its own in OgreScene::destroy) and BEFORE the OverlaySystem that
    // owns it dies; the manager itself goes with mDocumentScene below. Its
    // cameras are already gone — every View destroyed above took its own.
    if (mBlankScene) { try { hud::detach(mBlankScene); } catch (...) {} }
    try { hud::destroySystem(); } catch (...) {}
    try {
        // The document's staging manager goes with the rest of the scenes and
        // before the Root. Every handle in it goes stale afterwards, which is why
        // iris::graph tests Ogre::Root's liveness on every call.
        if (mDocumentScene && mRoot) mRoot->destroySceneManager(mDocumentScene);
        mDocumentScene = nullptr;
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
    delete mRoot;
    mRoot = nullptr;
    // ...and now, with Root gone, the instance and device the OpenXR runtime
    // created for us. Ogre destroys NEITHER an external instance nor an
    // external device, so this is the only place they die (and the only order
    // in which they may).
    if (mVrBoot) { try { vr::bootEnd(mVrBoot); } catch (...) {} mVrBoot = nullptr; }
    gLiveEngine = nullptr;
}

bool OgreEngine::viewNameTaken(const std::string &name) {
    for (auto &v : mViews)
        if (v->name() == name) { mLastError = "View '" + name + "' already exists"; return true; }
    return false;
}

void OgreEngine::ensureHlms() {
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
        // Jahshaka's own pieces (fog colour + height fog, base-map UV tiling) go in
        // as a LIBRARY folder rather than as per-datablock custom pieces: one
        // _piece_vs_piece_ps file then defines the pass-buffer members for BOTH
        // shader stages, which is the only way the vertex and pixel shader are
        // guaranteed to agree on the layout of the buffer they both read. It must
        // be LAST — the fog piece redefines a piece of Hlms/Pbs/Any/Atmosphere,
        // and a redefinition only works after the original has been collected.
        libs.push_back(am.load(mMediaDir + "Hlms/Jahshaka", "FileSystem", true));
        mRoot->getHlmsManager()->registerHlms(
            OGRE_NEW Ogre::HlmsPbs(am.load(mMediaDir + mainPath, "FileSystem", true), &libs));
    }
    // The pass-buffer listener asks HlmsPbs, on the render thread, for the state
    // of the pass it is building: which PCC owns the env-probe slot (the sky
    // cube's register, SKY-FALLBACK-1). Asking the Hlms itself rather than
    // mirroring the state in a flag of ours is what makes the two impossible to
    // disagree.
    FogHlmsListener::setPbs(
        static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS)));
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
    // buffer (the exponential distance term itself comes from the scene's
    // AtmosphereNpr — OgreFog.cpp). Unlit gets no listener: gizmos, wires and
    // billboards stay unfogged.
    mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS)->setListener(&gFogListener);
    // Shader-generation debugging: JAHSHAKA_HLMS_DEBUG_DIR=/some/dir/ dumps every
    // generated shader (and its properties) there. Diagnostic only.
    if (const char *dbg = std::getenv("JAHSHAKA_HLMS_DEBUG_DIR"))
        mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS)->setDebugOutputPath(true, true, dbg);
    // THE CACHE LOAD GOES HERE and nowhere else (SHADER_CACHE_SPEC §4.3 rule 5):
    // after BOTH registerHlms calls — HlmsDiskCache::applyTo needs the Hlms
    // instances to exist — and before registerCommonMaterials(), which parses
    // the low-level scripts and is the first thing that can trigger a compile.
    // The order INSIDE load() (pipeline blob, then setSaveMicrocodesToCache +
    // microcode, then the Hlms caches) is upstream's, not ours: see
    // OgreHlmsDiskCache.h:74-77 and Samples/2.0/Common/src/GraphicsSystem.cpp:626.
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
            unsigned threads = 0;
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            threads = std::min(6u, std::max(2u, cores / 2u));
            if (const char *forced = std::getenv("JAH_TEXTURE_MULTILOAD")) {
                const long n = std::strtol(forced, nullptr, 10);
                threads = unsigned(std::max(0l, std::min(32l, n)));
            }
            // Never under the NULL render system: a headless run decodes nothing
            // worth parallelising and the pool would be four idle threads.
            if (mHeadless) threads = 0;
            if (threads > 0) tm->setMultiLoadPool(threads);
            mMultiLoadThreads = threads;
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
                               // The VCT LightInjection compute job includes PBS pieces (area-light
                               // LTC) by bare file name through the resource system.
                               "Hlms/Pbs/Any",
                               // VCT voxelizer/lighting compute jobs (Voxelizer.material.json)
                               // and the IBL specular integrator the PCC probe workspace's
                               // ibl_specular pass wants (falls back to mips if absent).
                               "VCT",
                               "Compute/Tools", "Compute/Tools/Any", "Compute/Tools/GLSL",
                               "Compute/Tools/HLSL", "Compute/Tools/Metal",
                               "Compute/Algorithms/IBL",
                               // DDGI's five compute jobs (GI_UNIFIED_SPEC §4
                               // P1). ONE location, flat folder — the .any
                               // pieces sit beside the per-syntax shaders, like
                               // IBL's. `IrradianceFields/Visualizer` is staged
                               // beside it but deliberately NOT registered: it
                               // is upstream's debug probe-sphere material and
                               // we never draw it.
                               "Compute/Algorithms/IrradianceFields",
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
    // The CARD-CAPTURE node (OgreView::kCardShadowNodeName has the numbers):
    // the sun's PSSM at the probe resolution, no focused maps, one whole-atlas
    // clear — instantiated once per scene whose surface cache is on.
    if (!cm->hasShadowNodeDefinition(OgreView::kCardShadowNodeName))
        buildShadowNode(OgreView::kCardShadowNodeName, probeShadowResolution(mShadowResolution), 0u,
                        false, 0u);
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
    if (!engine->init(cfg, error)) return nullptr;   // ~OgreEngine clears gLiveEngine
    return engine;
}

}}  // namespace jahshaka::engine
