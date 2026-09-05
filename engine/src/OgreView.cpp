// OgreView: a render target (window or RTT) plus the compositor workspace and
// camera that draw a scene into it.
#include "EnginePrivate.h"

namespace jahshaka { namespace engine { namespace detail {

OgreView::OgreView(Ogre::Root *root, Ogre::Window *window, Ogre::TextureGpu *texture,
                   const std::string &name, unsigned w, unsigned h, const Colour &background,
                   std::string &errorSink)
    : mRoot(root), mWindow(window), mTexture(texture), mName(name),
      mWidth(w), mHeight(h), mBackground(background), mError(errorSink) {
    mWorkspaceDef = name + "/Workspace";
    // The compositor chain (POST_CHAIN_SPEC.md §3) replaces
    // CompositorManager2::createBasicWorkspaceDef: same pixels in the
    // passthrough shape, but the graph is ours to grow.
    chain::build(mRoot->getCompositorManager2(), mWorkspaceDef, chainDesc(), mNodeDefs);
}

ChainDesc OgreView::chainDesc() const {
    ChainDesc d;
    d.background = mBackground;
    d.shadows    = mShadows;
    d.samples    = sampleCount();
    // Set BEFORE the offscreen early-out below: the overlay's entitlement is
    // its own opt-in (ViewOverlayDesc::allowOffscreen), not the post chain's,
    // so an offscreen view may legitimately keep the passthrough shape AND be
    // allowed to draw the HUD — which is exactly what the engine suite does.
    d.overlays   = overlaysAllowed();
    // THE offscreen guarantee, in ONE place (POST_CHAIN_SPEC.md §7.3): an
    // offscreen view never gets the post chain, whatever the host pushed.
    // Thumbnails, material previews, the asset viewer, the avatar preview and
    // every pixel suite render through createOffscreenView, and their exact
    // colours are what makes them assertable.
    if (isOffscreen() && !mPostFx.allowOffscreen) {
        // ...except REFRACTION, which is not a post-process at all: it is how a
        // refractive material renders. A scene that has one needs the pass in
        // EVERY view that draws it (thumbnails, previews, screenshots), or the
        // interlock in OgreScene::setRefractionsActive downgrades the material
        // to glass everywhere. Scenes without refractive materials never ask for
        // it, so no existing offscreen view changes shape.
        d.refractions = mPostFx.refractions;
        return d;
    }
    d.hdr            = mPostFx.hdr;
    d.exposure       = mPostFx.exposure;
    d.exposureMin    = mPostFx.exposureMin;
    d.exposureMax    = mPostFx.exposureMax;
    // Bloom rides the HDR node; without HDR there is nothing to bright-pass.
    d.bloom          = mPostFx.bloom && mPostFx.hdr;
    d.bloomThreshold = mPostFx.bloomThreshold;
    d.ssao           = mPostFx.ssao;
    d.ssaoScale      = mPostFx.ssaoScale;
    d.ssaoPower      = mPostFx.ssaoPower;
    d.ssaoRadius     = mPostFx.ssaoRadius;
    d.smaaPreset     = mPostFx.smaaPreset;
    d.ssr            = mPostFx.ssr;
    d.refractions    = mPostFx.refractions;
    return d;
}

bool OgreView::overlaysAllowed() const {
    // THE offscreen guarantee for the engine-drawn overlay, in the SAME single
    // place as the post chain's (STATS_OVERLAY_SPEC §5.2). It is a property of
    // the VIEW, not of what the desc currently asks to draw — that is what
    // makes toggling the stats readout or the loading cover free (element
    // state, never a workspace rebuild). See kIncludeOverlaysNote.
    return !isOffscreen() || mOverlay.allowOffscreen;
}

void OgreView::setOverlay(const ViewOverlayDesc &d) {
    if (d == mOverlay) return;          // hosts push per frame; the same value is free
    const bool wasAllowed = overlaysAllowed();
    mOverlay = d;
    // ONLY the entitlement can change the graph. Everything else (stats on/off,
    // cover state, captions, colours) is Ogre overlay-element state, applied
    // once a frame by hud::apply — so workspaceGeneration does not move, which
    // test_engine's hud_overlay_toggle_does_not_rebuild_the_workspace pins.
    if (overlaysAllowed() != wasAllowed) rebuildWorkspaceDef();
}

const ViewOverlayDesc &OgreView::overlay() const { return mOverlay; }

void OgreView::setPostFx(const PostFxDesc &fx) {
    if (fx == mPostFx) return;   // hosts push per frame; the same value is free
    const ChainDesc before = chainDesc();
    mPostFx = fx;
    const ChainDesc after = chainDesc();
    // Only a SHAPE change rebuilds. Exposure, bloom threshold, AO power and the
    // SMAA preset are uniforms or shader reloads, not graph edits.
    if (!ChainDesc::sameShape(before, after)) rebuildWorkspaceDef();
}

const PostFxDesc &OgreView::postFx() const { return mPostFx; }

OgreView::~OgreView() { destroy(); }

const std::string &OgreView::name() const { return mName; }
Scene *OgreView::scene() const { return mScene; }

bool OgreView::setScene(Scene *scene) {
    JAH_TRY {
        if (!scene) { detachScene(); return true; }
        if (mScene) {
            mError = "View '" + mName + "' already shows scene '" + mScene->name() +
                     "'; detach it (setScene(nullptr)) before binding another";
            return false;
        }
        auto *s = static_cast<OgreScene *>(scene);
        mCamera = s->sceneManager()->createCamera(mName + "/Camera");
        mCamera->setNearClipDistance(0.1f);
        // Ogre's default far plane is 100000; PSSM splits computed over that range
        // leave no shadow-map resolution for the actual scene (a View that is
        // never handed a CameraDesc keeps these defaults).
        mCamera->setFarClipDistance(1000.0f);
        mCamera->setAutoAspectRatio(true);
        mScene = s;
        // A new scene means nothing of it has been drawn yet: whatever is in
        // the window belongs to the previous scene (or to whoever owned those
        // pixels before this view existed). Hosts gate their loading cover on
        // this being 0.
        mFramesPresented = 0;
        return attachWorkspace();
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
// The workspace seam. Everything that (re)creates this view's workspace goes
// through these two functions — see the comment on the declarations.
bool OgreView::attachWorkspace() {
    JAH_TRY {
        if (mWorkspace) return true;
        if (!mScene || !mCamera) return false;
        Ogre::TextureGpu *t = target();
        if (!t) return false;
        mWorkspace = mRoot->getCompositorManager2()->addWorkspace(
            mScene->sceneManager(), t, mCamera, mWorkspaceDef, mEnabled);
        if (!mWorkspace) return false;
        for (Ogre::CompositorWorkspaceListener *l : mWorkspaceListeners)
            mWorkspace->addListener(l);
        ++mWorkspaceGeneration;
        return true;
    } JAH_CATCH(mError, false);
}

bool OgreView::detachWorkspace() {
    if (!mWorkspace) return false;
    JAH_TRY {
        for (Ogre::CompositorWorkspaceListener *l : mWorkspaceListeners)
            mWorkspace->removeListener(l);
        mRoot->getCompositorManager2()->removeWorkspace(mWorkspace);
        mWorkspace = nullptr;
        return true;
    } JAH_CATCH(mError, false);
}

void OgreView::addWorkspaceListener(Ogre::CompositorWorkspaceListener *l) {
    if (!l) return;
    if (std::find(mWorkspaceListeners.begin(), mWorkspaceListeners.end(), l) !=
        mWorkspaceListeners.end())
        return;
    mWorkspaceListeners.push_back(l);
    JAH_TRY { if (mWorkspace) mWorkspace->addListener(l); } JAH_CATCH(mError, );
}

void OgreView::removeWorkspaceListener(Ogre::CompositorWorkspaceListener *l) {
    auto it = std::find(mWorkspaceListeners.begin(), mWorkspaceListeners.end(), l);
    if (it == mWorkspaceListeners.end()) return;
    mWorkspaceListeners.erase(it);
    JAH_TRY { if (mWorkspace) mWorkspace->removeListener(l); } JAH_CATCH(mError, );
}

unsigned OgreView::workspaceGeneration() const { return mWorkspaceGeneration; }

unsigned long long OgreView::framesPresented() const { return mFramesPresented; }

void OgreView::notePresented() {
    // Deliberately conservative: a disabled view's workspace is skipped by the
    // compositor, and a view with no scene or no workspace draws nothing. Only
    // frames that really put this view's pixels on the target count.
    if (mEnabled && mWorkspace && mScene) ++mFramesPresented;
}

void OgreView::detachScene() {
    JAH_TRY {
        detachWorkspace();
        if (mCamera && mScene && mScene->sceneManager()) mScene->sceneManager()->destroyCamera(mCamera);
        mCamera = nullptr;
        mScene  = nullptr;
        mFramesPresented = 0;
    } JAH_CATCH(mError, );
}

void OgreView::setCamera(const CameraDesc &c) {
    JAH_TRY {
        if (!mCamera) return;
        mCamera->setPosition(toOgre(c.position));
        mCamera->setOrientation(Ogre::Quaternion(c.orientation.w, c.orientation.x, c.orientation.y, c.orientation.z));
        mCamera->setNearClipDistance(std::max(c.nearClip, 0.001f));
        mCamera->setFarClipDistance(std::max(c.farClip, c.nearClip + 0.01f));
        if (c.orthographic) {
            mCamera->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
            // The REAL target size, not the last requested one: an ortho view
            // whose window is mid-resize would otherwise project through a stale
            // aspect and its pick rays would miss.
            const unsigned tw = width(), th = height();
            const float aspect = th ? float(tw) / float(th) : 1.0f;
            // orthoSize is the HALF vertical extent (the document camera's
            // ortho(-orthoSize..+orthoSize) convention); Ogre's setOrthoWindow
            // takes FULL extents. Passing orthoSize directly rendered 2x
            // zoomed relative to the document's pick-ray mapping — off-center
            // clicks then selected the wrong object in axis views.
            mCamera->setOrthoWindow(2.0f * c.orthoSize * aspect, 2.0f * c.orthoSize);
        } else {
            mCamera->setProjectionType(Ogre::PT_PERSPECTIVE);
            mCamera->setFOVy(Ogre::Degree(std::max(1.0f, std::min(c.fovDegrees, 179.0f))));
        }
    } JAH_CATCH(mError, );
}

void OgreView::setEnabled(bool on) {
    mEnabled = on;
    JAH_TRY { if (mWorkspace) mWorkspace->setEnabled(on); } JAH_CATCH(mError, );
}

Colour OgreView::background() const { return mBackground; }
bool OgreView::shadows() const { return mShadows; }

void OgreView::setShadows(bool on) {
    if (on == mShadows) return;
    mShadows = on;
    rebuildWorkspaceDef();
}

void OgreView::setBackground(const Colour &c) {
    const bool same = std::abs(c.r - mBackground.r) < 1e-4f && std::abs(c.g - mBackground.g) < 1e-4f &&
                      std::abs(c.b - mBackground.b) < 1e-4f && std::abs(c.a - mBackground.a) < 1e-4f;
    if (same) return;
    mBackground = c;
    rebuildWorkspaceDef();
}

void OgreView::rebuildWorkspaceDef() {
    JAH_TRY {
        Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
        const bool hadWorkspace = detachWorkspace();
        chain::destroy(cm, mWorkspaceDef, mNodeDefs);
        chain::build(cm, mWorkspaceDef, chainDesc(), mNodeDefs);
        if (hadWorkspace) attachWorkspace();
    } JAH_CATCH(mError, );
}

bool OgreView::dropWorkspaceForShadowRebuild() {
    if (!mShadows || !mWorkspace) return false;
    return detachWorkspace();
}

void OgreView::recreateWorkspaceAfterShadowRebuild() {
    attachWorkspace();
}

bool OgreView::isEnabled() const { return mEnabled; }

// The ACHIEVED size, exactly like sampleCount() reports the achieved sample
// count: what the render target really is, not what the host asked for. For an
// on-screen view those differ constantly — the swapchain follows the native
// window (and on X11 the surface's currentExtent wins outright, ogre-patch
// 0008), a request made this frame is applied at the next applyPendingResize,
// and a window manager may never grant the size at all. Reporting the request
// made the selftest's resize assertion tautological (deep audit area 7 F3):
// it compared the values we had just pushed with themselves.
unsigned OgreView::width() const {
    Ogre::TextureGpu *t = target();
    return t ? t->getWidth() : mWidth;
}
unsigned OgreView::height() const {
    Ogre::TextureGpu *t = target();
    return t ? t->getHeight() : mHeight;
}
bool OgreView::isOffscreen() const { return mTexture != nullptr; }

void OgreView::resize(unsigned w, unsigned h) {
    if (!w || !h) return;
    JAH_TRY {
        if (mWindow) {
            // Record the request ONLY. mWidth/mHeight are the last size actually
            // applied to the window; eagerly assigning them here made
            // applyPendingResize's "nothing changed" guard true for every pure
            // size change, so the on-screen resize path was dead code and
            // resizes only ever happened through Ogre's OUT_OF_DATE swapchain
            // self-heal (deep audit area 7 F1 — whose non-convergent case is
            // the "viewport stops presenting after a dock-open resize" defect).
            mPendingW = w; mPendingH = h;
        } else {
            rebuildRtt(w, h);   // an RTT cannot be resized in place
            mWidth = w; mHeight = h;
        }
    } JAH_CATCH(mError, );
}

void OgreView::stallDevice() {
    // A FULL device stall through the public VaoManager contract:
    // waitForSpecificFrameToFinish(getFrameCount()) is documented (OgreVaoManager.h)
    // as "will perform a full stall", and the Vulkan backend implements it as
    // VulkanDevice::stall() — flush bindings, submit the open command buffer,
    // vkDeviceWaitIdle, _notifyDeviceStalled. Backend-neutral: every VaoManager
    // implements it, so this stays correct if the render system is ever not Vulkan.
    JAH_TRY {
        Ogre::VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
        if (vao) vao->waitForSpecificFrameToFinish(vao->getFrameCount());
    } JAH_CATCH(mError, );
}

void OgreView::rebuildRtt(unsigned w, unsigned h) {
    // Rebuild the RTT (at mRequestedSamples) and re-add the workspace.
    const bool hadWorkspace = detachWorkspace();
    Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
    tm->destroyTexture(mTexture);
    mTexture = createRtt(mRoot, processUniqueName("rtt"), w, h, mRequestedSamples);
    if (hadWorkspace) attachWorkspace();
}

unsigned OgreView::sanitizeSamples(unsigned samples) {
    if (samples < 1)  samples = 1;
    if (samples > 16) samples = 16;
    while (samples & (samples - 1)) samples &= samples - 1;   // round DOWN to a power of two
    return samples;
}

void OgreView::setSampleCount(unsigned samples) {
    samples = sanitizeSamples(samples);
    if (samples == mRequestedSamples) return;   // hosts may push per frame; same value is free
    mRequestedSamples = samples;
    JAH_TRY {
        if (mWindow) {
            // Vulkan has no runtime setFsaa: structurally a resize — the window is
            // recreated (with the FSAA misc param) at frame time, coalesced with
            // any size change already pending.
            mPendingSamples = samples;
            if (!mPendingW || !mPendingH) { mPendingW = mWidth; mPendingH = mHeight; }
        } else {
            rebuildRtt(mWidth, mHeight);
        }
    } JAH_CATCH(mError, );
}

unsigned OgreView::sampleCount() const {
    // The ACHIEVED count: drivers clamp (Vulkan halves unsupported requests), and
    // the validated description lands on the target at window creation /
    // Resident transition. Before a target exists, report the request.
    Ogre::TextureGpu *t = target();
    if (!t) return mRequestedSamples;
    const unsigned achieved = t->getSampleDescription().getColourSamples();
    return achieved ? achieved : 1u;
}

bool OgreView::readPixels(Image &out) {
    if (!mTexture) { mError = "readPixels: View '" + mName + "' is on-screen"; return false; }
    JAH_TRY {
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        const Ogre::uint32 w = mTexture->getWidth(), h = mTexture->getHeight();
        Ogre::AsyncTextureTicket *t = tm->createAsyncTextureTicket(
            w, h, 1u, Ogre::TextureTypes::Type2D, mTexture->getPixelFormat());
        t->download(mTexture, 0, true);
        const Ogre::TextureBox box = t->map(0);
        out.width = w; out.height = h;
        out.rgba.resize(static_cast<size_t>(w) * h * 4u);
        for (Ogre::uint32 y = 0; y < h; ++y)
            std::memcpy(&out.rgba[static_cast<size_t>(y) * w * 4u], box.at(0, y, 0), w * 4u);
        t->unmap();
        tm->destroyAsyncTextureTicket(t);
        return true;
    } JAH_CATCH(mError, false);
}

void OgreView::applyPendingResize() {
    if (!mWindow || !mPendingW || !mPendingH) return;
    const unsigned w = mPendingW, h = mPendingH;
    const bool sampleChange = mPendingSamples != 0;
    mPendingW = mPendingH = mPendingSamples = 0;
    if (!mCreateWindow) {
        // No recreate hook (macOS): the window backend owns its surface and
        // resizes it in place — requestResolution re-syncs the layer and rebuilds
        // the swapchain (colour AND depth), setFsaa rebuilds it with new samples.
        // Sizes are VIEW POINTS on both sides of this call; the window converts.
        JAH_TRY {
            if (sampleChange) {
                // The workspace is dropped around a sample change: its render pass
                // targets the window's texture, which changes sample count.
                const bool hadWorkspace = detachWorkspace();
                mWindow->setFsaa(std::to_string(mRequestedSamples));
                if (hadWorkspace) attachWorkspace();
            }
            mWindow->requestResolution(w, h);
            // resize() no longer assigns these (it must not — see there), so the
            // in-place backend records the applied request here. POINTS on this
            // path deliberately: requestResolution takes points and the window
            // converts, so getWidth() (pixels) is a different unit on Retina.
            mWidth = w; mHeight = h;
        } JAH_CATCH(mError, );
        return;
    }
    // ---- SIZE ONLY: no recreate. --------------------------------------------
    // Ogre's own Window::windowMovedOrResized() is documented for exactly this
    // ("you don't need to call this unless you created the window externally" —
    // OgreWindow.h) and the XCB implementation does the whole job properly:
    // xcb_get_geometry for the REAL current size of the host's window,
    // mDevice->stallIgnoringDeviceLost(), destroySwapchain (which transitions
    // colour AND depth to OnStorage — the stale-depth-buffer fault the recreate
    // existed to avoid), setFinalResolution, createSwapchain. ogre-patch 0008
    // then makes the new swapchain honour the surface's currentExtent, so the
    // extent lands right even if the geometry moved again in between.
    //
    // It had ZERO call sites in this tree, which is why the recreate below was
    // the only path — and, with resize() eagerly updating mWidth/mHeight, an
    // unreachable one (area 7 F1).
    if (!sampleChange) {
        JAH_TRY {
            const unsigned curW = mWindow->getWidth(), curH = mWindow->getHeight();
            if (w == curW && h == curH) { mWidth = w; mHeight = h; return; }
            mWindow->windowMovedOrResized();
            // The window read its own geometry: believe IT, not the request.
            mWidth = mWindow->getWidth(); mHeight = mWindow->getHeight();
        } JAH_CATCH(mError, );
        return;
    }
    // ---- MSAA CHANGE: the one case that still recreates the window. ---------
    // Vulkan/XCB has no setFsaa (only the Metal window implements it, patch
    // 0007), so the sample count can only change by building a new window with
    // a different FSAA misc param on the same native handle.
    //
    // STALL FIRST. destroyRenderWindow tears down the swapchain and calls
    // VulkanVaoManager::notifySemaphoreUnused on its acquire semaphore, which
    // is a bare vkDestroySemaphore — and every on-screen window sits in
    // SwapchainAcquired at a frame boundary (VulkanQueue::commitAndNextCommand
    // Buffer re-acquires immediately after each present), with frames still in
    // flight behind it. Destroying those with live GPU work is the credible
    // mechanism behind the VUID-vkAcquireNextImageKHR validation flake. Ogre's
    // own equivalent (windowMovedOrResized, above) stalls before it rebuilds;
    // this path did not.
    JAH_TRY {
        const bool hadWorkspace = detachWorkspace();
        stallDevice();
        mRoot->getRenderSystem()->destroyRenderWindow(mWindow);
        mWindow = nullptr;
        mWindow = mCreateWindow(w, h, mRequestedSamples);
        mWidth = mWindow ? mWindow->getWidth() : w;
        mHeight = mWindow ? mWindow->getHeight() : h;
        if (hadWorkspace) attachWorkspace();
    } JAH_CATCH(mError, );
}

void OgreView::updateParticles() {
    if (mEnabled && mScene && mCamera)
        mScene->sceneManager()->getParticleSystemManager2()->setCameraPosition(mCamera->getPosition());
}

void OgreView::updateGi() {
    if (mEnabled && mScene && mCamera) mScene->updateGiTracking(mCamera->getPosition());
}

bool OgreView::warmUpShaders() {
    if (!mScene)  { mError = "warmUpShaders: no scene is bound to view '" + mName + "'"; return false; }
    if (!mCamera) { mError = "warmUpShaders: view '" + mName + "' has no camera"; return false; }
    JAH_TRY {
        if (!attachWorkspace()) { mError = "warmUpShaders: no workspace for view '" + mName + "'"; return false; }
        // A DISABLED view renders nothing, and the caller's whole reason for
        // being here is that the view is not on screen yet — the editor's
        // viewport is disabled until its page is shown, and the loading cover
        // is over it. Enable it just for these frames and put it back. Anything
        // presented meanwhile is behind the cover, which is a native window
        // stacked above the viewport's (src/viewport/viewportcover.h).
        const bool wasEnabled = mEnabled;
        if (!wasEnabled) setEnabled(true);
        const bool ok = chain::warmUp(mRoot, mScene->sceneManager(), mCamera,
                                      chain::sceneNodeDefName(mWorkspaceDef), mName);
        if (!wasEnabled) setEnabled(false);
        return ok;
    } JAH_CATCH(mError, false);
}

void OgreView::destroy() {
    detachScene();
    JAH_TRY {
        chain::destroy(mRoot->getCompositorManager2(), mWorkspaceDef, mNodeDefs);
        // Same reason as the MSAA recreate: destroying a render window destroys
        // its swapchain and acquire semaphore outright, and this runs at
        // runtime too — Engine::destroyView, and the host rebuilding a view on
        // a new native window (EngineViewWidget::recreateViewForNewWindow) —
        // not only at teardown, so there can be frames in flight.
        if (mWindow) stallDevice();
        if (mWindow)  { mRoot->getRenderSystem()->destroyRenderWindow(mWindow); mWindow = nullptr; }
        if (mTexture) { mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(mTexture); mTexture = nullptr; }
    } JAH_CATCH(mError, );
}

Ogre::TextureGpu *OgreView::createRtt(Ogre::Root *root, const std::string &name,
                                      unsigned w, unsigned h, unsigned samples) {
    Ogre::TextureGpuManager *tm = root->getRenderSystem()->getTextureGpuManager();
    Ogre::TextureGpu *rtt = tm->createTexture(
        name, Ogre::GpuPageOutStrategy::Discard,
        Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2D);
    rtt->setResolution(w, h);
    rtt->setPixelFormat(Ogre::PFG_RGBA8_UNORM);
    // MSAA (implicit resolve — no MsaaExplicitResolve flag, so readPixels sees
    // the resolved image). MUST precede the Resident transition: Ogre asserts
    // OnStorage in setSampleDescription, and the transition validates/clamps
    // the request into getSampleDescription() (the achieved count).
    if (samples > 1)
        rtt->setSampleDescription(Ogre::SampleDescription(static_cast<Ogre::uint8>(samples)));
    rtt->scheduleTransitionTo(Ogre::GpuResidency::Resident);
    return rtt;
}

Ogre::TextureGpu *OgreView::target() const { return mWindow ? mWindow->getTexture() : mTexture; }

}}}  // namespace jahshaka::engine::detail
