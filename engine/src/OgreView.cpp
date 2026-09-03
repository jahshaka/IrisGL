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

void OgreView::detachScene() {
    JAH_TRY {
        detachWorkspace();
        if (mCamera && mScene && mScene->sceneManager()) mScene->sceneManager()->destroyCamera(mCamera);
        mCamera = nullptr;
        mScene  = nullptr;
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
            const float aspect = mHeight ? float(mWidth) / float(mHeight) : 1.0f;
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
unsigned OgreView::width()  const { return mWidth; }
unsigned OgreView::height() const { return mHeight; }
bool OgreView::isOffscreen() const { return mTexture != nullptr; }

void OgreView::resize(unsigned w, unsigned h) {
    if (!w || !h) return;
    JAH_TRY {
        if (mWindow) {
            mPendingW = w; mPendingH = h;
        } else {
            rebuildRtt(w, h);   // an RTT cannot be resized in place
        }
        mWidth = w; mHeight = h;
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
        } JAH_CATCH(mError, );
        return;
    }
    // A pending MSAA change recreates the window even at the same size (the
    // sample-count term relaxing the old same-size early-return).
    if (w == mWidth && h == mHeight && !sampleChange) return;
    JAH_TRY {
        const bool hadWorkspace = detachWorkspace();
        mRoot->getRenderSystem()->destroyRenderWindow(mWindow);
        mWindow = nullptr;
        mWindow = mCreateWindow(w, h, mRequestedSamples);
        mWidth = w; mHeight = h;
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

void OgreView::destroy() {
    detachScene();
    JAH_TRY {
        chain::destroy(mRoot->getCompositorManager2(), mWorkspaceDef, mNodeDefs);
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
