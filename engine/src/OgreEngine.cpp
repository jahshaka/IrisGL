// The Ogre-Next 4.0 backend: the Engine object itself (Root, render systems, Hlms
// registration, views, scenes, the frame loop) and the Engine::create factory.
//
// engine/src/ is the only directory that includes Ogre; the shared declarations
// live in EnginePrivate.h, which documents the invariants this backend rests on.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine {
namespace detail {
namespace {

/// The one live engine in this process. Ogre::Root is a Singleton: a second
/// `new Root` asserts, so create() refuses while this is set.
OgreEngine *gLiveEngine = nullptr;

}  // namespace

bool OgreEngine::init(const EngineConfig &cfg, std::string &error) {
#ifdef __linux__
    mDisplay = reinterpret_cast<Display *>(cfg.display);
#else
    (void)cfg.display;  // X11-only; 0 on other hosts (Types.h documents the leak)
#endif
    mDefaultSamples = OgreView::sanitizeSamples(cfg.sampleCount);
    mMediaDir = cfg.hlmsMediaDir;
    if (!mMediaDir.empty() && mMediaDir.back() != '/') mMediaDir += '/';
    try {
        mAbiCookie = Ogre::generateAbiCookie();
        mRoot = new Ogre::Root(&mAbiCookie, "", "",
                               cfg.logFile.empty() ? "jahshaka-ogre.log" : cfg.logFile,
                               "Jahshaka");
        const char *plugin = (cfg.backend == Backend::Vulkan) ? "RenderSystem_Vulkan"
                                                              : "RenderSystem_GL3Plus";
        mRoot->loadPlugin(cfg.pluginDir + "/" + plugin, false, nullptr);

        const Ogre::RenderSystemList &list = mRoot->getAvailableRenderers();
        if (list.empty()) { error = "no Ogre render systems available"; return false; }
        mRoot->setRenderSystem(list[0]);
        mBackendName = list[0]->getName();
        mRoot->initialise(false);
        // NOTE: Hlms registration is deferred to the first view. The VaoManager
        // does not exist until a render target is created, and HlmsUnlit/HlmsPbs
        // registration walks it via ConstBufferPool::_changeRenderSystem —
        // registering here segfaults.
        return true;
    } JAH_CATCH(error, false);
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
        // cell, no decals, 2..50 units depth range. 4 cubemap probes per cell:
        // per-pixel PCC (the GI hybrid's reflection probes) is culled through
        // this grid — 0 would silently disable it (costs a slightly larger grid
        // buffer; zero shader cost until probes exist).
        sm->setForwardClustered(true, 16, 8, 24, 96, 0, 4, 2.0f, 50.0f);
        // Shadow maps cover nothing until these are set (Ogre's samples set both).
        sm->setShadowDirectionalLightExtrusionDistance(500.0f);
        sm->setShadowFarDistance(500.0f);
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
        X11Handle x11{ mDisplay, (::Window)handle };
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
        // MSAA: the FSAA misc param must be passed at EVERY window creation —
        // here AND in the resize lambda below, or a resize silently resets it.
        params["FSAA"] = Ogre::StringConverter::toString(mDefaultSamples);
        Ogre::Window *window = mRoot->createRenderWindow(name, width, height, false, &params);
        window->setVSync(true, 1);
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
        Display *display = mDisplay;
        Ogre::Root *root = mRoot;
        view->mCreateWindow = [root, vulkan, display, handle, name](unsigned w, unsigned h,
                                                                    unsigned samples) -> Ogre::Window * {
            Ogre::NameValuePairList p;
            X11Handle x11{ display, (::Window)handle };
            if (vulkan) p["SDL2x11"] = Ogre::StringConverter::toString((unsigned long)&x11);
            else { p["parentWindowHandle"] = Ogre::StringConverter::toString((unsigned long)handle); p["gamma"] = "true"; }
            p["vsync"] = "true"; p["vsyncInterval"] = "1";
            p["FSAA"] = Ogre::StringConverter::toString(samples);
            Ogre::Window *win = root->createRenderWindow(name + "/" + processUniqueName("resize"), w, h, false, &p);
            win->setVSync(true, 1);
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
    JAH_TRY {
        for (auto &v : mViews) { v->applyPendingResize(); v->updateParticles(); v->updateGi(); }
        for (auto &s : mScenes) { s->applyPendingGi(); s->applyPendingIbl(); }
        if (mRoot) mRoot->renderOneFrame();
    } JAH_CATCH(mLastError, );
}

const std::string &OgreEngine::lastError() const { return mLastError; }

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
        if (cm->hasShadowNodeDefinition(OgreView::kShadowNodeName))
            cm->removeShadowNodeDefinition(OgreView::kShadowNodeName);
        createShadowNode();
        for (OgreView *v : rebuilt) v->recreateWorkspaceAfterShadowRebuild();
    } JAH_CATCH(mLastError, );
}

unsigned OgreEngine::shadowResolution() const { return mShadowResolution; }

OgreEngine::~OgreEngine() {
    // Dependency order, all BEFORE Root: views (workspaces, cameras, windows,
    // textures) -> scenes (items, datablocks, our MeshPtrs, scene managers)
    // -> null window -> any leftover meshes -> Root.
    for (auto &v : mViews)  v->destroy();
    mViews.clear();
    for (auto &s : mScenes) s->destroy();
    mScenes.clear();
    try {
        if (mNullWindow && mRoot) mRoot->getRenderSystem()->destroyRenderWindow(mNullWindow);
        mNullWindow = nullptr;
        if (mRoot && Ogre::MeshManager::getSingletonPtr())
            Ogre::MeshManager::getSingleton().removeAll();
    } catch (...) {}
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
    mHlmsRegistered = true;
    applyShadowFilter();   // replaces Ogre's PCF_3x3 default with ours (Soft = 4x4)
    registerCommonMaterials();
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
                               // The PCC probe compositor (the fog/UV pieces in this
                               // folder reach HlmsPbs as a library path above, not
                               // through the resource system).
                               "Hlms/Jahshaka" };
        for (const char *d : dirs) rgm.addResourceLocation(mMediaDir + d, "FileSystem", group, false);
        rgm.initialiseAllResourceGroups(true);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: common material scripts not registered: " + e.getFullDescription());
    }
}

void OgreEngine::createShadowNode() {
    Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
    if (cm->hasShadowNodeDefinition(OgreView::kShadowNodeName)) return;
    // The whole atlas derives from one base size (Engine::setShadowResolution):
    // PSSM split 0 and the two focused maps at R, further splits at R/2 —
    // exactly the historical 2048/1024 layout, scaled.
    const Ogre::uint32 R = mShadowResolution;
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
        cm, mRoot->getRenderSystem()->getCapabilities(), OgreView::kShadowNodeName, params, false);
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
