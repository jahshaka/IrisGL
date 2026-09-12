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
        // nothing from the machine. Keeping the pointer means documentGraphScene
        // and the (refused) view calls never make a second one.
        //
        // A RENDERING boot stays exactly as it always was — initialise(false),
        // no window, no device — so the first window a Vulkan session creates
        // is the host's real one whenever the host can wait that long.
        mNullWindow = mRoot->initialise(cfg.headless, "jahshaka-headless");
        // THE ENGINE HAS NO WALL CLOCK (Engine.h "Simulation clock"): its
        // frame-time source is put in frame-delay mode right here, before any
        // frame, and stays there. The host's SimulationClock pushes the real
        // per-frame value; this default is what a host that never pushes gets.
        // After initialise(), not after the Root constructor: Root creates its
        // ControllerManager in initialise() (OgreRoot.cpp:751).
        Ogre::ControllerManager::getSingleton().setFrameDelay(kDefaultFrameDelta);
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

void OgreEngine::destroyScene(Scene *scene) {
    if (!scene) return;
    for (auto it = mScenes.begin(); it != mScenes.end(); ++it) {
        if (it->get() != scene) continue;
        for (auto &v : mViews)
            if (v->scene() == scene) v->detachScene();
        (*it)->destroy();
        mScenes.erase(it);
        return;
    }
    mLastError = "destroyScene: unknown Scene";
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
        Ogre::Window *window = mRoot->createRenderWindow(name, width, height, false, &params);
        window->setVSync(mVsync, 1u | kLowestLatencyVSync);
        ensureHlms();
        mViews.emplace_back(new OgreView(mRoot, window, nullptr, name, width, height,
                                         background, mLastError));
        OgreView *view = mViews.back().get();
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
            mNullWindow = mRoot->createRenderWindow(processUniqueName("jahshaka-null"),
                                                    8, 8, false, &wp);
        }
        ensureHlms();   // retried on every call until it succeeds (e.g. bad media dir)
        Ogre::TextureGpu *rtt = OgreView::createRtt(mRoot, processUniqueName("rtt"), width, height);
        mViews.emplace_back(new OgreView(mRoot, nullptr, rtt, name, width, height,
                                         background, mLastError));
        return mViews.back().get();
    } JAH_CATCH(mLastError, nullptr);
}

void OgreEngine::destroyView(View *view) {
    if (!view) return;
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

void OgreEngine::renderOneFrame() {
    // LEGAL AND EMPTY WHEN HEADLESS (Types.h EngineConfig::headless): a
    // headless engine can hold no View, so every loop below iterates nothing
    // and Root::renderOneFrame walks a workspace-less render system. Hosts do
    // not have to special-case their frame loop; it simply costs nothing.
    JAH_TRY {
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
        if (monitor::live()) {
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
            if (before) {
                const size_t after = tm ? countPendingTextures(tm, nullptr) : 0u;
                monitor::noteEvent(MonitorEventKind::TextureLoad, WorkReason::Request,
                                   "texture.load", names, float(ms),
                                   (unsigned long long)(before > after ? before - after : 0u));
                monitor::noteCacheWork(CacheKind::Texture, WorkReason::Request, 0, names.c_str(),
                                       unsigned(before), float(ms));
            }
        } else {
            drainTextureStreaming();
        }
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
        for (int pass = 0; pass < 2; ++pass)          // 0: on-screen, 1: the fallback
            for (auto &v : mViews) {
                if (!v->isEnabled() || !v->ogreScene()) continue;
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
            // The inset's rectangles are derived from the TARGET's aspect
            // (a normalised rect is not a pixel rect), so a resize that never
            // touched ViewPipDesc still moves the letterbox. Re-derived here,
            // once a frame, right after applyPendingResize; free when there is
            // no inset.
            v->applyLetterboxAndPip();
        }
        for (auto &s : mScenes) { s->applyPendingGi(); s->applyPendingIbl(); s->applyPendingPlanar(); }
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
        // THE SCENES THIS FRAME BELONGS TO (THREADING_ADOPTION_SPEC.md P3).
        // Computed ONCE, here, and read three times below: by the refraction
        // interlock, by the frame loop's update/clear passes, and by the
        // engineObjects census. Nothing between this line and the frame can
        // change a View's enabled flag, so one snapshot is honest.
        std::vector<OgreScene *> updated;
        scenesFeedingEnabledViews(updated);
        const auto drawnThisFrame = [&updated](const OgreScene *s) {
            return std::find(updated.begin(), updated.end(), s) != updated.end();
        };
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
                }
                // THE LAMP-MAP CACHE, second half: here and nowhere earlier. The
                // scene graph has just made every world AABB and light pose this
                // frame's, and nothing has rendered yet — so a caster that moved
                // this frame re-renders its lamps' maps in this frame, and the
                // scan reads cached bounds instead of paying a root-recursive
                // getWorldAabbUpdated per item (ENGINE_CACHE_POLICY_SPEC P3).
                applyShadowCacheDirties(updated);
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
        // THE ONE-SHOT RE-CAPTION (OgreOverlayHud.cpp's `Caption`): a TextArea
        // whose caption was set before its first rendered frame built its
        // geometry against an unloaded font and renders nothing, for ever,
        // silently. This is the "after the first frame" the workaround needs.
        hud::afterFrame();
        // The frame is drawn and (for window views) presented: every view that
        // took part in it now has its OWN pixels on its target. This is the
        // signal hosts gate a loading cover on (View::framesPresented).
        for (auto &v : mViews) v->notePresented();
        // THE FRAME IS CLOSED HERE, after everything the loop does — the pose
        // followers and the HUD's one-shot re-caption included — so `totalMs`
        // is what one renderOneFrame cost the caller, not what the render cost.
        monPost.reset();
        if (monitor::live()) monitor::gMonitor->endFrame(mUpdatedScenes);
    } JAH_CATCH(mLastError, );
    // A frame that THREW still has to close, or the next one appends to it and
    // the ring holds one record that never ends.
    if (monitor::live() && monitor::gMonitor->inFrame())
        monitor::gMonitor->endFrame(mUpdatedScenes);
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
        if (vao) vao->_update();
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
// Recorded warm-up sets (SHADER_CACHE_SPEC.md §2.7b / phase 3) — Unreal's
// ".rec" recordings.
//
// The storage is PROCESS-wide, not per scene, and that is what makes "merge the
// recordings" a non-problem: Ogre's analyze() ACCUMULATES into the same entry
// vector and de-duplicates by the 48-bit {Hlms hash, render queue} key, so
// recording every scene a session opens and saving once IS the merged set.
// (loadFrom, by contrast, CLEARS — so merging FILES would mean re-implementing
// Ogre's serialization format, which is exactly the second source of truth this
// whole program avoids.)
bool OgreEngine::recordWarmUpSet(Scene *scene) {
    JAH_TRY {
        if (!mWarmUpSet) mWarmUpSet.reset(new Ogre::VertexFormatWarmUpStorage);
        if (scene) {
            mWarmUpSet->analyze(static_cast<OgreScene *>(scene)->sceneManager());
        } else {
            // Null = every live scene. The host does not keep a scene registry
            // and should not have to grow one just to say "record the session".
            if (mScenes.empty()) { mLastError = "recordWarmUpSet: no scenes"; return false; }
            for (auto &s : mScenes) mWarmUpSet->analyze(s->sceneManager());
        }
        return true;
    } JAH_CATCH(mLastError, false);
}

bool OgreEngine::saveWarmUpSet(const std::string &file) {
    if (!mWarmUpSet) { mLastError = "saveWarmUpSet: nothing has been recorded"; return false; }
    JAH_TRY {
        // Through a real fstream rather than an Archive: the set lands beside
        // the shader cache, in a directory we own outright, and Ogre's Archive
        // API would need the folder registered as a resource location.
        std::fstream *fs = OGRE_NEW_T(std::fstream, Ogre::MEMCATEGORY_GENERAL)(
            file.c_str(), std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        Ogre::DataStreamPtr out(OGRE_NEW Ogre::FileStreamDataStream(file, fs, 0, true));
        mWarmUpSet->saveTo(out);
        out->close();
        return true;
    } JAH_CATCH(mLastError, false);
}

unsigned OgreEngine::applyWarmUpSet(const std::string &file, Scene *scene) {
    // Null = the first live scene. The degenerate renderables live for exactly
    // one frame and are destroyed again, so any scene will do, and a host that
    // just wants "warm this session up" should not have to pick one.
    auto *s = static_cast<OgreScene *>(scene);
    if (!s && !mScenes.empty()) s = mScenes.front().get();
    if (!s) { mLastError = "applyWarmUpSet: no scene to warm up in"; return 0u; }
    unsigned before = 0, cached = 0, expected = 0;
    mShaderCache.progress(before, cached, expected);
    JAH_TRY {
        std::ifstream probe(file, std::ios::binary);
        if (!probe) { mLastError = "applyWarmUpSet: no such file: " + file; return 0u; }
        probe.close();
        std::ifstream *is = OGRE_NEW_T(std::ifstream, Ogre::MEMCATEGORY_GENERAL)(
            file.c_str(), std::ios::in | std::ios::binary);
        Ogre::DataStreamPtr in(OGRE_NEW Ogre::FileStreamDataStream(file, is, true));
        // A SEPARATE storage from the recording one: loadFrom clears, and
        // wiping this session's accumulated recording just because we warmed
        // from a previous one would lose everything opened before now.
        Ogre::VertexFormatWarmUpStorage loaded;
        loaded.loadFrom(in);
        in->close();
        // createWarmUp builds degenerate 4-vertex buffers in the recorded
        // formats and applies the recorded materials to them — no mesh, no
        // skeleton, no texture is loaded. One frame is what compiles them.
        loaded.createWarmUp(s->sceneManager());
        renderOneFrame();
        loaded.destroyWarmUp();
    } JAH_CATCH(mLastError, 0u);
    unsigned after = 0;
    mShaderCache.progress(after, cached, expected);
    return after > before ? after - before : 0u;
}

ShaderCacheStats OgreEngine::shaderCacheStats() const {
    return mShaderCache.stats(mRoot);
}

bool OgreEngine::renderStats(RenderStats &out) const {
    out = RenderStats();
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
    try { hud::destroySystem(); } catch (...) {}
    try {
        // The document's staging manager goes with the rest of the scenes and
        // before the Root. Every handle in it goes stale afterwards, which is why
        // iris::graph tests Ogre::Root's liveness on every call.
        if (mDocumentScene && mRoot) mRoot->destroySceneManager(mDocumentScene);
        mDocumentScene = nullptr;
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
    // Both log listeners are registered on Ogre's default log, which Root owns.
    mShaderCache.detachCounters();
    detachLogBridge();
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
    // The pass-buffer listener needs to know when an irradiance field is bound
    // — not for the DDGI intensity (that is per scene) but for the FOUR-FLOAT
    // ALIGNMENT PAD that corrects the size upstream's IrradianceField block
    // under-reports (FogHlmsListener::ifdAlignFloats says why). Asking HlmsPbs
    // itself, rather than mirroring the state in a flag of ours, is what makes
    // the pad and the shader property that declares it impossible to disagree.
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
                               // VCT voxelizer/lighting compute jobs (Voxelizer.material.json —
                               // it also declares the ImageVoxelizer jobs, whose sources live in
                               // the subfolder) and the IBL specular integrator the PCC probe
                               // workspace's ibl_specular pass wants (falls back to mips if absent).
                               "VCT", "VCT/ImageVoxelizer",
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
