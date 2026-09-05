// The Ogre-Next 4.0 backend: the Engine object itself (Root, render systems, Hlms
// registration, views, scenes, the frame loop) and the Engine::create factory.
//
// engine/src/ is the only directory that includes Ogre; the shared declarations
// live in EnginePrivate.h, which documents the invariants this backend rests on.
#include "EnginePrivate.h"

#include <OgreFrameStats.h>

#include <fstream>

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
        // ONE worker thread and no Forward+ setup: nothing in here is ever
        // culled, lit or drawn. It exists to hold nodes.
        mDocumentScene = mRoot->createSceneManager(Ogre::ST_GENERIC, 1u,
                                                   processUniqueName("jahshaka-document"));
        return mDocumentScene;
    } JAH_CATCH(mLastError, nullptr);
}

Scene *OgreEngine::createScene(const std::string &name) {
    if (!mHlmsRegistered) {
        mLastError = "createScene('" + name + "'): no View exists yet — create a View first";
        return nullptr;
    }
    for (auto &s : mScenes)
        if (s->name() == name) { mLastError = "Scene '" + name + "' already exists"; return nullptr; }
    JAH_TRY {
        Ogre::SceneManager *sm = mRoot->createSceneManager(Ogre::ST_GENERIC, 2, name);
        // HlmsPbs shades point and spot lights ONLY through Forward+ (Forward3D /
        // ForwardClustered); without it only directional lights reach the shader.
        // Values are Ogre's sample defaults: 16x8 grid, 24 slices, 96 lights per
        // cell, 2..50 units depth range. 4 cubemap probes per cell:
        // per-pixel PCC (the GI hybrid's reflection probes) is culled through
        // this grid — 0 would silently disable it (costs a slightly larger grid
        // buffer; zero shader cost until probes exist).
        //
        // 8 DECALS PER CELL (DECALS_SPEC D5), always on. This is a per-CELL cap,
        // not a scene-wide one: more than 8 decals overlapping one cluster cell
        // drop the farthest. Turning it on costs ~54 KiB more per cached grid
        // buffer and NOTHING in the shader until a scene actually binds a decal
        // atlas (the decal code is gated on a non-null SceneManager decal
        // texture, OgreForwardClustered.cpp:1095-1120). It does shift
        // hlms_forwardplus_lights_per_cell and the cubemap slot offset, so every
        // PBS shader recompiles ONCE — CPU fill and shader read use the same
        // offsets, and the phase-0 gate proved the rendered pixels are
        // byte-identical either way.
        sm->setForwardClustered(true, 16, 8, 24, 96, kDecalsPerCell, 4, 2.0f, 50.0f);
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
        params["vsync"]         = "true";
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
        window->setVSync(true, 1u | kLowestLatencyVSync);
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
        view->mCreateWindow = [root, vulkan, display, handle, name](unsigned w, unsigned h,
                                                                    unsigned samples) -> Ogre::Window * {
            Ogre::NameValuePairList p;
            X11Handle x11{ display, (unsigned long)handle };
            if (vulkan) p["SDL2x11"] = Ogre::StringConverter::toString((unsigned long)&x11);
            else { p["parentWindowHandle"] = Ogre::StringConverter::toString((unsigned long)handle); p["gamma"] = "true"; }
            p["vsync"] = "true"; p["vsyncInterval"] = "1";
            // Same MAILBOX request as the first window above — this hook is the
            // MSAA-change recreate path, and a window rebuilt without it would
            // silently drop back to FIFO for the rest of the session.
            p["vsync_method"] = "Lowest Latency";
            p["FSAA"] = Ogre::StringConverter::toString(samples);
            Ogre::Window *win = root->createRenderWindow(name + "/" + processUniqueName("resize"), w, h, false, &p);
            win->setVSync(true, 1u | kLowestLatencyVSync);
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
        (*it)->destroy();
        mViews.erase(it);
        return;
    }
    mLastError = "destroyView: unknown View";
}

void OgreEngine::renderOneFrame() {
    // LEGAL AND EMPTY WHEN HEADLESS (Types.h EngineConfig::headless): a
    // headless engine can hold no View, so every loop below iterates nothing
    // and Root::renderOneFrame walks a workspace-less render system. Hosts do
    // not have to special-case their frame loop; it simply costs nothing.
    JAH_TRY {
        for (auto &v : mViews) {
            v->applyPendingResize(); v->updateParticles(); v->updateGi();
            // Both ends of the planar-reflection wiring move between frames (the
            // scene rebuilds its arm on a parameter change, the view recreates
            // its camera on setScene), so the listener is re-synced rather than
            // hooked up once. Idempotent and cheap when nothing changed.
            v->syncPlanarListener();
            // The inset's rectangles are derived from the TARGET's aspect
            // (a normalised rect is not a pixel rect), so a resize that never
            // touched ViewPipDesc still moves the letterbox. Re-derived here,
            // once a frame, right after applyPendingResize; free when there is
            // no inset.
            v->applyLetterboxAndPip();
        }
        for (auto &s : mScenes) { s->applyPendingGi(); s->applyPendingIbl(); s->applyPendingPlanar(); }
        // The post chain's tuning lives in MaterialManager singletons — exposure,
        // bloom threshold, the AO kernel and the SMAA preset are per PROCESS even
        // though the enable flags are per view (POST_CHAIN_SPEC.md §7.4). The rule
        // is "the primary on-screen view owns the globals": the first enabled
        // on-screen view whose chain has effects. Offscreen views never qualify —
        // their chainDesc() has every effect off by construction.
        for (auto &v : mViews) {
            if (!v->isEnabled()) continue;
            const ChainDesc d = v->chainDesc();
            if (!d.anyEffect()) continue;   // offscreen views only qualify if they opted in
            chain::applyGlobals(mRoot, v->camera(), d, v->width(), v->height());
            break;
        }
        // The refraction interlock (OgreScene::setRefractionsActive). A
        // Refractive datablock drawn by a pass that offers it no refractions
        // fails to COMPILE and loses the whole frame, and one scene can be drawn
        // by views with different chains — the editor viewport, the player, and
        // the throwaway offscreen view a screenshot renders through. So a scene
        // only keeps its refractive materials refractive while EVERY view that
        // draws it has the pass; otherwise they fall back to glass. Recomputed
        // per frame because views come and go; the setter is a no-op on repeat.
        for (auto &s : mScenes) {
            bool anyView = false, allHaveRefraction = true;
            for (auto &v : mViews) {
                if (v->scene() != s.get() || !v->isEnabled()) continue;
                anyView = true;
                if (!v->chainDesc().refractions) allHaveRefraction = false;
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
                hud::apply(owner->overlay(), stats, owner->width(), owner->height());
            } else {
                hud::hide();
            }
        }
        if (mRoot) mRoot->renderOneFrame();
        // THE ONE-SHOT RE-CAPTION (OgreOverlayHud.cpp's `Caption`): a TextArea
        // whose caption was set before its first rendered frame built its
        // geometry against an unloaded font and renders nothing, for ever,
        // silently. This is the "after the first frame" the workaround needs.
        hud::afterFrame();
        // The frame is drawn and (for window views) presented: every view that
        // took part in it now has its OWN pixels on its target. This is the
        // signal hosts gate a loading cover on (View::framesPresented).
        for (auto &v : mViews) v->notePresented();
    } JAH_CATCH(mLastError, );
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

// ---- Simulation clock (PARTICLES_FX2_SPEC.md) ------------------------------
// SceneManager::updateSceneGraph feeds the particle manager
// `ControllerManager::getFrameTimeSource()->getValue()` — one value, shared by
// every scene in the process. There is no per-scene or per-view delta to hook,
// which is why the header says these verbs are process-wide and means it.
//
// The two settings CANCEL EACH OTHER inside Ogre, not here:
// FrameTimeControllerValue::setTimeFactor zeroes mFrameDelay and setFrameDelay
// zeroes mTimeFactor (OgrePredefinedControllers.cpp:80-95).

void OgreEngine::setParticleTimeScale(float scale) {
    JAH_TRY {
        Ogre::ControllerManager::getSingleton().setTimeFactor(std::max(0.0f, scale));
    } JAH_CATCH(mLastError, );
}

float OgreEngine::particleTimeScale() const {
    return float(Ogre::ControllerManager::getSingleton().getTimeFactor());
}

void OgreEngine::setFixedFrameDelta(float seconds) {
    JAH_TRY {
        if (seconds > 0.0f)
            Ogre::ControllerManager::getSingleton().setFrameDelay(seconds);
        else
            Ogre::ControllerManager::getSingleton().setTimeFactor(1.0f);   // back to the wall clock
    } JAH_CATCH(mLastError, );
}

float OgreEngine::fixedFrameDelta() const {
    return float(Ogre::ControllerManager::getSingleton().getFrameDelay());
}

void OgreEngine::setShadowFilter(ShadowFilter f) {
    mShadowFilter = f;
    if (mHlmsRegistered) applyShadowFilter();
    // else: applied by ensureHlms() once the Hlms exists.
}

ShadowFilter OgreEngine::shadowFilter() const { return mShadowFilter; }

void OgreEngine::setShadowResolution(unsigned pixels) {
    const unsigned res = std::min(8192u, std::max(256u, pixels));
    if (res == mShadowResolution) return;
    mShadowResolution = res;
    if (!mHlmsRegistered) return;   // first createShadowNode() picks it up
    JAH_TRY {
        Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
        std::vector<OgreView *> rebuilt;
        for (auto &v : mViews)
            if (v->dropWorkspaceForShadowRebuild()) rebuilt.push_back(v.get());
        // The planar-reflection arm instantiates the HALF-resolution shadow node
        // in each of its private workspaces, so it holds the same kind of
        // reference a view's workspace does and must be dropped for the same
        // reason. Scenes whose reflections do not use shadows report false.
        std::vector<OgreScene *> planarRebuilt;
        for (auto &s : mScenes)
            if (s->dropPlanarForShadowRebuild()) planarRebuilt.push_back(s.get());
        if (cm->hasShadowNodeDefinition(OgreView::kShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kShadowNodeName);
        if (cm->hasShadowNodeDefinition(OgreView::kReflectShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kReflectShadowNodeName);
        createShadowNode();
        for (OgreView *v : rebuilt) v->recreateWorkspaceAfterShadowRebuild();
        for (OgreScene *s : planarRebuilt) s->recreatePlanarAfterShadowRebuild();
    } JAH_CATCH(mLastError, );
}

unsigned OgreEngine::shadowResolution() const { return mShadowResolution; }

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
        }
        return true;
    }
    // Not JAH_CATCH: this verb is const and the error sink is not. Nothing here
    // can fail in a way a caller could act on anyway — the counters are plain
    // reads and one flag flip — so "false, and `out` is default" is the whole
    // contract.
    catch (...) { out = RenderStats(); return false; }
}

bool OgreEngine::saveShaderCache() {
    if (!mRoot) return false;
    JAH_TRY { return mShaderCache.save(mRoot); } JAH_CATCH(mLastError, false);
}

bool OgreEngine::clearShaderCache() {
    JAH_TRY { return mShaderCache.clear(); } JAH_CATCH(mLastError, false);
}

void OgreEngine::shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                                     unsigned &expected) const {
    mShaderCache.progress(compiled, fromCache, expected);
}

OgreEngine::~OgreEngine() {
    // Save the shader cache FIRST, while every Ogre singleton the three layers
    // read is still alive and before a single view or scene has been torn down.
    // This is the primary save point (SHADER_CACHE_SPEC §4.4): a clean quit is
    // the only moment we are certain nothing is compiling.
    if (mRoot) { try { mShaderCache.save(mRoot); } catch (...) {} }
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
    // The counter listener is registered on Ogre's default log, which Root owns.
    mShaderCache.detachCounters();
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
    // Ambient is SPHERICAL HARMONICS, always and everywhere (Scene::setAmbientSh;
    // Scene::setAmbient converts the flat/hemisphere pair exactly). The mode is a
    // property of the HlmsPbs INSTANCE, not of a scene, so it cannot be chosen
    // per scene: every scene therefore speaks SH, and a scene that pushes nothing
    // gets black ambient rather than a stale hemisphere. AmbientSh also means the
    // ambient contributes no specular term of its own — that comes from the
    // GGX-prefiltered sky reflection cubemap instead.
    static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS))
        ->setAmbientLightMode(Ogre::HlmsPbs::AmbientSh);
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
    mHlmsRegistered = true;
    applyShadowFilter();   // replaces Ogre's PCF_3x3 default with ours (Soft = 4x4)
    if (!pixels) return;   // the rest is rendering-only — see the top of this function
    registerCommonMaterials();
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
        buildShadowNode(OgreView::kShadowNodeName, mShadowResolution);
    // The planar-reflection pass's own atlas, at HALF resolution. Definitions
    // are free — the VRAM is only allocated where a workspace instantiates one,
    // which for reflections is one atlas PER BUDGET SLOT. At the default 2048 a
    // shared full-resolution node would cost ~56 MB per slot; half is ~14 MB,
    // and nobody has ever measured shadow-map resolution inside a mirror.
    if (!cm->hasShadowNodeDefinition(OgreView::kReflectShadowNodeName))
        buildShadowNode(OgreView::kReflectShadowNodeName, std::max(256u, mShadowResolution / 2u));
}

void OgreEngine::buildShadowNode(const char *name, unsigned baseResolution) {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    // The whole atlas derives from one base size (Engine::setShadowResolution):
    // PSSM split 0 and the two focused maps at R, further splits at R/2 —
    // exactly the historical 2048/1024 layout, scaled.
    const Ogre::uint32 R = baseResolution;
    const Ogre::uint32 H = std::max(128u, R / 2u);
    Ogre::ShadowNodeHelper::ShadowParamVec params;
    Ogre::ShadowNodeHelper::ShadowParam p;
    memset(&p, 0, sizeof(p));
    p.technique = Ogre::SHADOWMAP_PSSM;
    p.numPssmSplits = 3u;
    p.resolution[0].x = R; p.resolution[0].y = R;
    for (size_t i = 1u; i < 4u; ++i) { p.resolution[i].x = H; p.resolution[i].y = H; }
    p.atlasStart[0].x = 0u; p.atlasStart[0].y = 0u;
    p.atlasStart[1].x = 0u; p.atlasStart[1].y = R;
    p.atlasStart[2].x = H;  p.atlasStart[2].y = R;
    p.supportedLightTypes = 0u;
    p.addLightType(Ogre::Light::LT_DIRECTIONAL);
    params.push_back(p);
    // Two focused maps for point/spot lights (dual-paraboloid for point). Needs
    // the 'Ogre/DPSM/CubeToDpsm' material from the staged common scripts.
    p.technique = Ogre::SHADOWMAP_FOCUSED;
    p.resolution[0].x = R; p.resolution[0].y = R;
    p.atlasStart[0].x = 0u; p.atlasStart[0].y = R + H;
    p.supportedLightTypes = 0u;
    p.addLightType(Ogre::Light::LT_POINT);
    p.addLightType(Ogre::Light::LT_SPOTLIGHT);
    params.push_back(p);
    p.atlasStart[0].y = R + H + R;
    params.push_back(p);
    Ogre::ShadowNodeHelper::createShadowNodeWithSettings(
        cm, mRoot->getRenderSystem()->getCapabilities(), name, params, false);
}

void OgreEngine::applyShadowFilter() {
    JAH_TRY {
        auto *pbs = static_cast<Ogre::HlmsPbs *>(mRoot->getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        if (!pbs) return;
        Ogre::HlmsPbs::ShadowFilter f = Ogre::HlmsPbs::PCF_4x4;
        switch (mShadowFilter) {
        case ShadowFilter::Hard:     f = Ogre::HlmsPbs::PCF_2x2; break;
        case ShadowFilter::Soft:     f = Ogre::HlmsPbs::PCF_4x4; break;
        case ShadowFilter::VerySoft: f = Ogre::HlmsPbs::PCF_6x6; break;
        }
        pbs->setShadowSettings(f);
    } JAH_CATCH(mLastError, );
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
