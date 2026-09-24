// OgreView: a render target (window or RTT) plus the compositor workspace and
// camera that draw a scene into it.
#include "EnginePrivate.h"

// The inset's letterbox rectangle is written straight onto its scene pass'
// DEFINITION (chain::PipHandles::scenePass) between frames — Ogre re-reads it
// on every execute, so that is a live viewport change with no rebuild.
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
// resetExposureHistory reaches the SEED PASS INSTANCE (not just its definition):
// the clear colour Vulkan actually uses lives in the pass' RenderPassDescriptor,
// and "run once more" is a call on the instance.
#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <OgreRenderPassDescriptor.h>

#include <cmath>
#include <cstdlib>
#include <string>

namespace jahshaka { namespace engine { namespace detail {

namespace {
/// FALSE only when JAH_ORTHO_POSTFX names "off" (or "0"): the screen-space
/// effects then take the plain gate under an orthographic camera instead of
/// their ortho branch. Read once — an environment variable is a boot-time
/// choice, and a mid-run flip would change a workspace's shape behind a live
/// view. See chainDesc().
bool orthoPostFxAdapted() {
    static const bool adapted = [] {
        const char *v = std::getenv("JAH_ORTHO_POSTFX");
        if (!v || !*v) return true;
        const std::string s(v);
        return !(s == "off" || s == "0" || s == "OFF" || s == "false");
    }();
    return adapted;
}
}   // namespace

OgreView::OgreView(Ogre::Root *root, Ogre::Window *window, Ogre::TextureGpu *texture,
                   const std::string &name, unsigned w, unsigned h, const Colour &background,
                   std::string &errorSink)
    : mRoot(root), mWindow(window), mTexture(texture), mName(name),
      mWidth(w), mHeight(h), mBackground(background), mError(errorSink) {
    mWorkspaceDef = name + "/Workspace";
    // The compositor chain (POST_CHAIN_SPEC.md §3) replaces
    // CompositorManager2::createBasicWorkspaceDef: same pixels in the
    // passthrough shape, but the graph is ours to grow.
    chain::build(mRoot->getCompositorManager2(), mWorkspaceDef, chainDesc(), mNodeDefs,
                 mChainHandles);
    mChainRayReflect = chainDesc().rayReflect;
    mChainProbeGather = chainDesc().probeGather;
    mChainSunContact = chainDesc().sunContact;
}

/// How many mip levels a `w x h` closest-depth pyramid has: down to 1x1, the
/// ordinary mip count. A free function so the resize hooks can ask the same
/// question chainDesc() answers, and so the number has exactly one definition.
static unsigned hzbLevelsFor(unsigned w, unsigned h) {
    unsigned levels = 1u;
    unsigned m = w > h ? w : h;
    while (m > 1u) { m >>= 1u; ++levels; }
    return levels;
}

ChainDesc OgreView::chainDesc() const {
    ChainDesc d;
    d.background = mBackground;
    d.shadows    = mShadows;
    // STEREO (VR_SPEC §4.3), before the offscreen early-out: the VR session's
    // view IS offscreen (its target is the both-eyes RTT), and it is the one
    // offscreen view in the engine that must keep the post chain — it opts in
    // through PostFxDesc::allowOffscreen like every other deliberate case.
    d.stereo         = mStereo;
    d.cullCameraName = mStereo ? mCullCameraName : std::string();
    // THE LOD SWITCH BAND (ogre-patch 0075, ChainDesc::lodHysteresis), and the
    // whole rule in one line: a band belongs to a picture somebody WATCHES OVER
    // TIME and is exactly wrong for a CAPTURE. So it is on for an on-screen view
    // — the editor viewport, the Player's window — and for the VR session's
    // view, which is offscreen only because both eyes share one texture and is
    // the most motion-sensitive picture this engine draws; and off for every
    // thumbnail, preview, screenshot and pixel suite, which must take the level
    // their own value asks for so one pose is always one set of pixels.
    // ...and an offscreen view may ASK for it, per view
    // (View::setLodHysteresisOffscreen — the suite that reads what the band
    // does is the only caller; see the note on the public method).
    d.lodHysteresis  = (!isOffscreen() || d.stereo || mLodHysteresisOffscreen)
                           ? detail::jahLodHysteresis() : 0.0f;
    // Set BEFORE the offscreen early-out below: the overlay's entitlement is
    // its own opt-in (ViewOverlayDesc::allowOffscreen), not the post chain's,
    // so an offscreen view may legitimately keep the passthrough shape AND be
    // allowed to draw the HUD — which is exactly what the engine suite does.
    d.overlays   = overlaysAllowed();
    // EDITOR FURNITURE, per view (ChainDesc::helpers). Set before the offscreen
    // early-out for the same reason `overlays` is: it is not a post effect, and
    // an offscreen shot of a view that hides the furniture must hide it too
    // (that is what makes player.screenshot a picture of the PLAYER).
    d.helpers    = mHelpersVisible;
    // ...AND THE VR CHANNEL (kVrHelperBit's two-bit rule): furniture the
    // HEADSET draws. Only the VR session's view asks for it, so every other
    // picture in this process — the desktop, a thumbnail, a preview, a
    // screenshot — is built with that channel masked out.
    d.vrHelpers  = mVrHelpersVisible;
    // ...AND THE RUNTIME'S HIDDEN-AREA MESH (kVrMaskBit, lane HAM-1), set here
    // beside the two helper channels and BEFORE the offscreen early-out for the
    // same reason `stereo` is: the only view that ever asks for it is the VR
    // session's eye pair, which is offscreen.
    d.hiddenAreaMask = mHiddenAreaMask;
    // LETTERBOX (CAMERAS_SPEC §7.4) is a property of the CAMERA the host
    // pushed, not of the view — a camera that constrains its aspect does so in
    // every view that shows it. Unlike the effects below it is NOT cleared for
    // offscreen views: an export or a screenshot of a constrained camera must
    // show the same shot the viewport does, and the flag can only be true when
    // a host deliberately pushed a constrained camera.
    d.letterbox  = mCameraDesc.constrainAspect && mCameraDesc.aspect > 0.0f;
    d.letterboxAspect = d.letterbox ? mCameraDesc.aspect : 0.0f;
    // THE HZB (NANITE_SPEC §4.3) is set BEFORE the offscreen early-out, with
    // letterbox and for the same kind of reason: it is not a post-process and it
    // changes no pixel of the picture — it is a resource a future screen-space
    // trace reads, and an offscreen capture is exactly where such a trace gets
    // measured. It can only be true when a host deliberately asked for it, and
    // nothing in this engine asks yet.
    //
    // The LEVEL COUNT is part of the graph (see ChainDesc::hzbLevels), derived
    // here from the view's achieved size — which is why it is read through
    // width()/height() (the target's real size) and not from mWidth/mHeight.
    d.hzb        = mPostFx.hzb;
    d.hzbLevels  = d.hzb ? hzbLevelsFor(width(), height()) : 0u;
    d.hzbFarthest = mPostFx.hzbFarthest;
    // THE RADIANCE READBACK (HDR-READBACK-1), before the early-out for the
    // reason `hzb` is: it changes no pixel of the picture a view presents — it
    // is what the view KEEPS (a float scene target), and the offscreen views
    // are exactly the ones a closed form is measured on.
    d.hdrReadback = mPostFx.hdrReadback;
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
    d.tonemapFixed   = mPostFx.tonemapFixed;
    d.exposure       = mPostFx.exposure;
    d.exposureMin    = mPostFx.exposureMin;
    d.exposureMax    = mPostFx.exposureMax;
    d.exposureScale  = mPostFx.exposureScale;
    d.meterPattern   = mPostFx.meterPattern;
    d.meterLowPercent  = mPostFx.meterLowPercent;
    d.meterHighPercent = mPostFx.meterHighPercent;
    // Bloom rides the HDR node; without HDR there is nothing to bright-pass.
    d.bloom          = mPostFx.bloom && mPostFx.hdr;
    d.bloomThreshold = mPostFx.bloomThreshold;
    d.bloomKnee      = mPostFx.bloomKnee;
    d.bloomAmount    = mPostFx.bloomAmount;
    d.ssao           = mPostFx.ssao;
    d.ssaoScale      = mPostFx.ssaoScale;
    d.ssaoPower      = mPostFx.ssaoPower;
    d.ssaoRadius     = mPostFx.ssaoRadius;
    // The dither's diagnostic off switch (a uniform, per view — see
    // PostFxDesc::ditherOff). It sits BELOW the offscreen early-out with
    // everything else the chain carries, which is right: an offscreen view
    // with no chain has no tonemap quad and therefore no dither to turn off.
    // ...AND THE EYE'S DITHER STANDS DOWN WHEN THE RUNTIME WILL ENCODE AGAIN (the
    // lead, at the merge of EYE-GRADE-1 onto DITHER-1). The dither is sized for
    // the LAST 8-bit write; on the UNORM-swapchain fallback the runtime re-encodes
    // our bytes and stretches a half-code of noise ~13x in the darks — a visible
    // grain where there was a band. On the _SRGB contract (every runtime seen so
    // far) this term is false and the eye dithers like the desktop.
    d.ditherOff      = mPostFx.ditherOff || (mStereo && !vr::colourEncodedOnce());
    d.smaaPreset     = mPostFx.smaaPreset;
    d.ssr            = mPostFx.ssr;
    // AND THE SOURCE THE ROW SELECTS, which this line was missing for one round
    // (the lead's read, 2026-09-18). Without it `ChainDesc::ssrScreenMarch` held
    // its own default — true — whatever a host pushed, so a STEREO chain was
    // saved only by `chain::build`'s `!desc.stereo` while the MONO view of one
    // eye (`vrEyeScreenshot`'s control: offscreen, `allowOffscreen`, copying the
    // session's PostFxDesc) went on MARCHING. The two pictures were then not
    // comparable in the way the control exists to be comparable — it is below
    // the offscreen early-out for exactly that reason, because the control is
    // an offscreen view. @see PostFxDesc::ssrScreenMarch.
    d.ssrScreenMarch = mPostFx.ssrScreenMarch;
    d.ssrMaxDistance = mPostFx.ssrMaxDistance;
    d.ssrSteps = mPostFx.ssrSteps;
    d.ssrMarchPhase = mPostFx.ssrMarchPhase;
    d.ssrThickness   = mPostFx.ssrThickness;
    d.ssrIntensity   = mPostFx.ssrIntensity;
    d.reflectionRoughnessCutoff = mPostFx.reflectionRoughnessCutoff;
    // RAY-TRACED REFLECTIONS (PHOTON_SPEC §7 R5), and the whole tier rule in one
    // line: AUTO means "traced wherever the machine can", which is this view's
    // SSR row being on (the SSR contract already keeps that to High and Epic —
    // no new World row exists or is wanted) AND the device advertising ray
    // queries AND the application preference allowing them. Ray tracing is a
    // property of the MACHINE and not of the document (PHOTON_SPEC §4 D4), so
    // nothing here reads the scene.
    //
    // The RESOLUTION comes free with the same row: `ssr == 2` (Epic) traces
    // every pixel, `ssr == 1` (High) one in four, exactly as the march does —
    // which is why the row is a scale factor here too and not a second setting.
    d.rayReflect     = d.ssr > 0 && mScene && mScene->rayReflectionsWanted();
    // THE SCREEN-PROBE GATHER (GATHER-1a), and the reason it is not `&& d.ssr`:
    // the gather needs the PREPASS, not the reflection row. A project whose
    // gather row is on gets the prepass in every view that draws its scene,
    // whatever its SSR row says — which is the whole of `ChainDesc::probeGather`
    // (the note there). Like `rayReflect` it reads the machine through the
    // scene's resolved row and never the document directly, and like it this
    // line is BELOW the offscreen early-out: an offscreen view that did not opt
    // in (`PostFxDesc::allowOffscreen`) has no prepass and therefore no gather,
    // so every thumbnail, preview and pixel suite keeps the colours that make
    // it assertable.
    // ...AND NOT IN A STEREO VIEW (the lead's read): the Component declines a
    // stereo target at this phase (the probe grid would have to be split at the
    // eye seam — the spec's phase 7), so without this term a VR eye would pay a
    // second geometry traversal every frame for a prepass nothing then reads.
    d.probeGather    = mScene && mScene->probeGatherWanted() && !mStereo;
    // HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1): the same shape and the same
    // three terms as the gather's line above — the scene's resolved row, below
    // the offscreen early-out, and never in a stereo view (the job declines a
    // two-eye target: never in VR, by the design's own column).
    d.sunContact     = mScene && mScene->sunContactWanted() && !mStereo;
    d.refractions    = mPostFx.refractions;
    // DISTORTION (POST_LOOKS_SPEC §5.3), below the offscreen early-out with the
    // rest: a distortion object is invisible in the passthrough shape anyway (it
    // carries kDistortionBit instead of kVisibleBit and lives at RQ 220, which
    // no passthrough pass draws), so an offscreen view renders exactly what it
    // rendered before this existed.
    d.distortion     = mPostFx.distortion;
    d.distortionStrength = mPostFx.distortionStrength;
    // THE LOOKS STACK (POST_LOOKS_SPEC §4). Below the offscreen early-out, so
    // an offscreen view carries no looks unless it deliberately opted in — a
    // thumbnail is a photograph of the CONTENT, not of the scene's look (§8).
    d.looks          = mPostFx.looks;
    // ---- THE ORTHOGRAPHIC FALLBACK ------------------------------------------
    // SSR and SSAO both reconstruct a view-space position from depth, and both
    // used to do it in a way that is only valid for a perspective frustum —
    // `cameraDir * linearDepth`, where an ortho frustum's far corner is not a
    // ray at all (OgreFrustum.cpp:884 takes `ratio = 1` for PT_ORTHOGRAPHIC).
    // Under an axis view the reconstructed positions therefore moved with the
    // camera even though the projection did not, and the reflections and the
    // contact shadowing SLID while panning — the owner report.
    //
    // Both shaders now BRANCH on the projection instead (JahSsrRayMarch_ps.glsl
    // and ogre-patch 0019 for SSAO), so the effects are correct under an
    // orthographic camera and stay on. THIS IS THE FALLBACK: one environment
    // variable puts the plain gate back, for a driver on which the branch ever
    // misbehaves. It is deliberately not a document row — the ortho branch is
    // the shipped behaviour and a user has no way to reason about the choice —
    // and it is read ONCE, so it cannot change under a running process.
    //
    // Either way it is a property of the CAMERA the view is drawing through,
    // never of the world settings, so a perspective view of the same scene is
    // untouched and switching a viewport into an axis view is a shape change
    // (see setCamera).
    if (mCameraDesc.orthographic && !orthoPostFxAdapted()) {
        d.ssao = false;
        d.ssr  = 0;
    }
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

// ---------------------------------------------------------------------------
// THE PICTURE-IN-PICTURE INSET (CAMERAS_SPEC §7.7). The graph shape and every
// spike finding behind it: ViewPipDesc (Types.h) and chain::buildPip.

namespace {
/// WRITES A CLEAR COLOUR THAT IS ALREADY ON THE GPU'S BOOKS.
///
/// A pass definition's colour is what a FUTURE rebuild will use; the live
/// RenderPassDescriptor is what THIS pass instance clears with (Vulkan caches
/// the VkClearValue, which is why setClearColour is a virtual and not a field
/// write). Both, always — a rewrite that happens in the same frame as a rebuild
/// must survive either order. Extracted from resetExposureHistory, which found
/// this the hard way and is now one of three callers.
void writeLiveClearColour(Ogre::CompositorWorkspace *workspace,
                          Ogre::CompositorPassClearDef *def,
                          const Ogre::ColourValue &colour) {
    if (!def) return;
    def->setAllClearColours(colour);
    if (!workspace) return;
    for (Ogre::CompositorNode *n : workspace->getNodeSequence()) {
        if (!n) continue;
        for (Ogre::CompositorPass *p : n->_getPasses()) {
            if (!p || p->getDefinition() != def) continue;
            if (Ogre::RenderPassDescriptor *rpd = p->getRenderPassDesc())
                rpd->setClearColour(colour);
        }
    }
}
}   // namespace

/// THE FIXED EXPOSURE, PUSHED LIVE (POST_CHAIN_SPEC §14).
///
/// `PostFxDesc::exposure` is deliberately not part of ChainDesc::sameShape — a
/// grade change must never rebuild a compositor workspace. In the AUTOMATIC
/// form that is free (the exposure is a material parameter the globals listener
/// pushes), but in the FIXED form the exposure IS a clear colour baked into the
/// graph at build time, so nothing moved it afterwards: a view that changed its
/// exposure while tonemapFixed was on kept the grade it was built with until
/// something else rebuilt the chain. Found while building the inset's own grade
/// (which needs exactly this), fixed here for both.
void OgreView::applyFixedExposure() {
    if (!mChainHandles.fixedExposure) return;
    const ChainDesc d = chainDesc();
    if (!d.hdr || !d.tonemapFixed) return;
    JAH_TRY {
        writeLiveClearColour(mWorkspace, mChainHandles.fixedExposure,
                             chain::fixedExposureColour(d.exposureScale, d.exposure));
    } JAH_CATCH(mError, );
}

bool OgreView::pipTonemapEffective() const {
    // THE INSET GRADES EXACTLY WHEN THIS VIEW'S CHAIN DOES, and the AND is the
    // guarantee rather than a precaution: a graded inset on an ungraded surface
    // is the same mismatch, in the same frame, that Route C exists to remove —
    // it would simply have swapped which half was wrong. A host asks for the
    // grade (ViewPipDesc::tonemap, resolved from the world and the PIPPED
    // camera); the view's own chain decides whether the surface it is painting
    // on is graded at all. An offscreen view without PostFxDesc::allowOffscreen
    // has no chain by construction (chainDesc's early-out), so a thumbnail that
    // somehow acquired an inset gets a raw one — consistent with its raw frame.
    return mPip.tonemap && chainDesc().hdr;
}

bool OgreView::pipAllowed() const {
    // THE determinism law's single gate, in the same shape and the same place
    // as overlaysAllowed() and chainDesc()'s post-fx early-out: an offscreen
    // view NEVER gets an inset unless it opted in. Thumbnails, material
    // previews and every pixel suite must stay byte-identical whatever a host
    // pushes at them (tests/engine's pip_is_ignored_offscreen_unless_asked).
    return !isOffscreen() || mPip.allowOffscreen;
}

void OgreView::setPip(const ViewPipDesc &d) {
    if (d == mPip) return;              // hosts push per frame; the same value is free
    // SHAPE vs STATE, the setPostFx split. Only `enabled` and `allowOffscreen`
    // decide whether the workspace exists (syncPip's `want`); the camera pose,
    // rects and background are ALL live in applyPip — even the background,
    // which applyPip re-writes over what buildPip baked. Before this split a
    // MOVING previewed camera (animated, socketed, possessed, dragged) tore
    // down and re-added the compositor workspace once per frame, because the
    // desc carries the pose and the old code called syncPip on any change.
    //
    // ROUTE C's `tonemap` IS a graph change (an RGBA16F target, an exposure
    // texture, a different composite material) but it is not tested here: the
    // effective value depends on this view's chain as well as on the desc
    // (pipTonemapEffective), so applyPip — which runs immediately below and
    // once per frame — owns that comparison for both of its inputs. `exposure`
    // is a clear colour applyPip rewrites live, which is what lets a pipped
    // camera's grade be animated without rebuilding a workspace per frame.
    const bool shapeChanged = d.enabled != mPip.enabled ||
                              d.allowOffscreen != mPip.allowOffscreen;
    mPip = d;
    if (shapeChanged) syncPip();        // build/tear only on an existence flip
    applyPip();                         // camera + rects + background, no rebuild
}

const ViewPipDesc &OgreView::pip() const { return mPip; }

void OgreView::syncPip() {
    const bool want = mPip.enabled && pipAllowed() && mScene && mWorkspace;
    if (!want) { destroyPip(); return; }
    JAH_TRY {
        Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
        if (mPipWorkspaceDef.empty()) {
            mPipWorkspaceDef = mName + "/PipWorkspace";
            // THE LOCAL TEXTURE'S SIZE IS DECIDED HERE, once per build, from the
            // rect the inset currently has (Route C). applyPip re-derives the
            // same fractions every frame and rebuilds only when the WHOLE-PIXEL
            // size they produce has actually changed — a moving inset, a moving
            // camera and a window resize all leave this alone.
            pipTexFactors(mPipTexWidthFactor, mPipTexHeightFactor);
            mPipTexTonemap = pipTonemapEffective();
            mPipTargetW = width(); mPipTargetH = height();
            ViewPipDesc built = mPip;
            built.tonemap = mPipTexTonemap;
            chain::buildPip(mRoot, mPipWorkspaceDef, built, mPipTexWidthFactor,
                            mPipTexHeightFactor, mPipNodeDefs, mPipHandles);
        }
        if (!mPipCamera) {
            // POOLED, and created with isVisible = false: an idle camera costs
            // only a frustum in the light-cull inner loop, and the spike proved
            // (T6) that a camera which RENDERS is byte-identical either way,
            // because Forward Clustered culls lights against the camera
            // directly and never consults mVisibleCameras. Note the naming trap
            // — the header calls the argument notShadowCaster, the .cpp calls
            // it isVisible.
            mPipCamera = mScene->sceneManager()->createCamera(mName + "/PipCamera", false);
            mPipCamera->setNearClipDistance(0.1f);
            mPipCamera->setFarClipDistance(1000.0f);
        }
        // ORDER IS THE WHOLE POINT. addWorkspace always appends (position -1),
        // and there is NO reorder API — so the inset is (re)added here, after
        // the main workspace, every time either of them is built. Adding it
        // before the main workspace is not a subtle bug: the main pass simply
        // paints over the inset and the frame hashes identical to no-PiP
        // (spike T2).
        if (mPipWorkspace) { cm->removeWorkspace(mPipWorkspace); mPipWorkspace = nullptr; }
        mPipWorkspace = cm->addWorkspace(
            mScene->sceneManager(), target(), mPipCamera, mPipWorkspaceDef, mEnabled,
            /*position*/ -1, /*uavBuffers*/ nullptr, /*initialLayouts*/ nullptr,
            /*vpOffsetScale*/ Ogre::Vector4(mPip.left, mPip.top, mPip.width, mPip.height),
            // 0x00 IS THE DEFAULT AND IT SILENTLY DISABLES THE MODIFIER — the
            // inset would render full-screen over the main view with no error
            // of any kind (spike, correction 3).
            /*vpModifierMask*/ 0xFF, /*executionMask*/ 0xFF);
        ++mPipGeneration;               // see View::pipGeneration
    } JAH_CATCH(mError, );
}

unsigned OgreView::pipGeneration() const { return mPipGeneration; }

void OgreView::pipTexFactors(float &widthFactor, float &heightFactor) const {
    const unsigned tw = width(), th = height();
    const float targetAspect = th ? float(tw) / float(th) : 1.0f;
    float outer[4], inner[4];
    chain::pipRects(mPip, targetAspect, outer, inner);
    // The INNER rect, i.e. what the composite quad will stretch the texture
    // across. Taking the OUTER one instead would letterbox a shot into a
    // texture of the wrong shape and then stretch it back — the exact
    // distortion the explicit camera aspect below exists to avoid.
    widthFactor  = std::max(0.001f, inner[2]);
    heightFactor = std::max(0.001f, inner[3]);
}

void OgreView::destroyPip() {
    JAH_TRY {
        Ogre::CompositorManager2 *cm = mRoot->getCompositorManager2();
        if (mPipWorkspace) { cm->removeWorkspace(mPipWorkspace); mPipWorkspace = nullptr; }
        if (!mPipWorkspaceDef.empty()) {
            chain::destroyPip(mRoot, mPipWorkspaceDef, mPipNodeDefs, mPipHandles);
            mPipWorkspaceDef.clear();
        }
        // LAST, and only now: the workspace's scene pass held this pointer, and
        // destroying the camera first segfaults on the next frame (spike T6).
        if (mPipCamera && mScene && mScene->sceneManager())
            mScene->sceneManager()->destroyCamera(mPipCamera);
        mPipCamera = nullptr;
    } JAH_CATCH(mError, );
}

void OgreView::applyLetterboxAndPip() {
    applyLetterbox();
    // The shift's world-space offset is a function of the TARGET's aspect, so a
    // resize moves it even though nothing touched the CameraDesc — same reason
    // the letterbox rectangle is re-derived here. Free when nothing is shifted.
    if (mCamera) applyLensShift(mCamera, mCameraDesc, viewAspect(mCameraDesc));
    applyPip();
}

void OgreView::applyPip() {
    if (!mPipWorkspace || !mPipCamera) return;
    JAH_TRY {
        const CameraDesc &c = mPip.camera;

        const unsigned tw = width(), th = height();
        const float targetAspect = th ? float(tw) / float(th) : 1.0f;
        float outer[4], inner[4];
        chain::pipRects(mPip, targetAspect, outer, inner);

        // ---- HAZARD 3 (POST_CHAIN_SPEC §14): the rects move LIVE, the local
        // texture does not follow. It is sized as a FRACTION of the target, so
        // a MOVE and a window RESIZE are both free — Ogre re-derives a
        // fraction-sized texture whenever the target changes size. Only a
        // change to the inset's own SIZE (the preference, a camera whose
        // constrained aspect re-letterboxes the rect, a `tonemap` flip) needs a
        // new texture, and this compares the sizes IN WHOLE PIXELS: what the
        // texture already is against what the rect now wants. Equal is the
        // steady state and costs one ceilf per frame; different rebuilds the
        // inset's node ONCE and re-adds its workspace LAST, through the same
        // syncPip every other rebuild goes through (there is no reorder API).
        const auto texPixels = [](float factor, unsigned n) {
            return (unsigned)std::max(1.0f, std::ceil(factor * float(n)));
        };
        //
        // THE TARGET'S OWN SIZE IS IN THE TEST TOO, and it is not belt and
        // braces. Ogre re-creates a fraction-sized local texture when the final
        // target changes size (CompositorWorkspace::_update's resize block) —
        // but the SECOND workspace on a window is not re-analyzed with it, and
        // the barrier for the recreated texture is then issued INSIDE the
        // frame's open render pass: "vkCmdPipelineBarrier(): Barriers cannot be
        // set during subpass 0 ... with no self-dependency", twice, on the
        // first frames after a window resize (measured under
        // VK_LAYER_KHRONOS_validation; zero with the inset off, zero on the
        // pre-Route-C inset, which had no local texture to re-create). Building
        // the inset again is the honest fix and it is nearly free: a resize
        // already rebuilds the swapchain and every resizable texture in the
        // frame. It stays a REBUILD-ON-CHANGE, never a per-frame rebuild.
        const bool sizeMoved =
            texPixels(mPipTexWidthFactor, tw)  != texPixels(std::max(0.001f, inner[2]), tw) ||
            texPixels(mPipTexHeightFactor, th) != texPixels(std::max(0.001f, inner[3]), th) ||
            mPipTargetW != tw || mPipTargetH != th ||
            mPipTexTonemap != pipTonemapEffective();
        if (sizeMoved) {
            destroyPip();
            syncPip();
            if (!mPipWorkspace || !mPipCamera) return;
        }

        mPipCamera->setPosition(toOgre(c.position));
        mPipCamera->setOrientation(
            Ogre::Quaternion(c.orientation.w, c.orientation.x, c.orientation.y, c.orientation.z));
        mPipCamera->setNearClipDistance(std::max(c.nearClip, 0.001f));
        mPipCamera->setFarClipDistance(std::max(c.farClip, c.nearClip + 0.01f));

        // The aspect the inset is actually SHOWN at: the inner rect's, in
        // pixels (a normalised rect is not a pixel rect).
        const float rectAspect = (inner[3] * float(th)) > 0.0f
            ? (inner[2] * float(tw)) / (inner[3] * float(th)) : 1.0f;

        if (c.orthographic) {
            mPipCamera->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
            mPipCamera->setOrthoWindow(2.0f * c.orthoSize * rectAspect, 2.0f * c.orthoSize);
        } else {
            mPipCamera->setProjectionType(Ogre::PT_PERSPECTIVE);
            mPipCamera->setFOVy(Ogre::Degree(std::max(1.0f, std::min(c.fovDegrees, 179.0f))));
        }
        // THE ASPECT IS EXPLICIT, ALWAYS (Route C). It used to be automatic
        // unless the camera constrained itself, which was right by accident:
        // the camera rendered straight into the window and Ogre's automatic
        // aspect is the VIEWPORT's. It now renders into a local texture, and
        // setAutoAspectRatio would take THAT texture's aspect — the same number
        // only by construction, and not the same number at all once the texture
        // is rounded up to whole pixels. So it is set from the rectangle the
        // inset is shown at, and a constrained camera pins its authored value
        // (which is what the inner rect was computed from in the first place —
        // spike T5: a world square is 32x32 px with this and 32x54 without).
        mPipCamera->setAutoAspectRatio(false);
        mPipCamera->setAspectRatio(c.constrainAspect && c.aspect > 0.0f ? c.aspect : rectAspect);
        // The inset is the same lens: a shifted camera is shifted in its own
        // preview too. The aspect here is the INNER rect's, not the target's.
        applyLensShift(mPipCamera, c, c.constrainAspect && c.aspect > 0.0f ? c.aspect : rectAspect);

        // LIVE, all of it — no rebuild, spike T3. The workspace modifier places
        // (and scissors) every window pass of the inset node at the OUTER rect;
        // the COMPOSITE quad's own mVpRect then pulls it in to the INNER one,
        // which Ogre re-reads from the definition on every execute
        // (CompositorPass::setRenderPassDescToCurrent). The fill quad keeps
        // [0,1] and therefore paints the whole outer rect: background where the
        // two rects agree, letterbox bars where they do not. The SCENE pass
        // takes no modifier at all — it owns a texture that is already the
        // inset (buildPip's Route C note).
        mPipWorkspace->setViewportModifier(
            Ogre::Vector4(outer[0], outer[1], outer[2], outer[3]));
        if (mPipHandles.composite) {
            auto &vp = mPipHandles.composite->mVpRect[0];
            // width  = passWidth * outerWidth  -> passWidth = innerWidth / outerWidth
            // left   = passLeft  + outerLeft   -> passLeft  = innerLeft  - outerLeft
            vp.mVpLeft   = inner[0] - outer[0];
            vp.mVpTop    = inner[1] - outer[1];
            vp.mVpWidth  = outer[2] > 0.0f ? inner[2] / outer[2] : 1.0f;
            vp.mVpHeight = outer[3] > 0.0f ? inner[3] / outer[3] : 1.0f;
            vp.mVpScissorLeft   = vp.mVpLeft;
            vp.mVpScissorTop    = vp.mVpTop;
            vp.mVpScissorWidth  = vp.mVpWidth;
            vp.mVpScissorHeight = vp.mVpHeight;
        }
        if (mPipHandles.fill) mPipHandles.fill->setAllClearColours(toOgre(mPip.background));
        // The grade, live: one clear colour, never a rebuild (ViewPipDesc) —
        // through the writer that also touches the pass INSTANCE, because the
        // colour Vulkan uses is the one cached in its RenderPassDescriptor.
        writeLiveClearColour(mPipWorkspace, mPipHandles.exposure,
                             chain::fixedExposureColour(0.0f, mPip.exposure));
    } JAH_CATCH(mError, );
}

// THE PROJECT'S DESCRIPTION, THEN THE VR POLICY OVER IT (lane EYE-GRADE-1).
//
// A STEREO view is the picture somebody is standing in, and it is graded by the
// project like every other view of that scene: `SceneMirror::applyViewEnvironment`
// pushes the world's PostFxDesc (with the driving camera's lens over it) into
// this view every frame, exactly as it does into the desktop's. What a
// side-by-side eye pair cannot carry is filtered out HERE — in the one place
// every push goes through, so no host can forget it and no later push can undo
// it — by `applyVrViewPolicy`, whose header states the whole list and why.
//
// `postFx()` therefore reports the EFFECTIVE description, which is what
// `vr.state().postFx` shows and what the suite asserts against the desktop's.
void OgreView::setPostFx(const PostFxDesc &pushed) {
    PostFxDesc fx = pushed;
    if (mStereo) {
        applyVrViewPolicy(fx, mVrSsrOverride);
        // WHAT THE POLICY TOOK AWAY IS SAID OUT LOUD, ONCE PER CHANGE. Most of
        // it is invisible to an author — nobody misses an SSAO they never saw
        // in there — but three things are chosen on purpose in the World panel
        // and are visible on the desktop: a LOOK, the REFRACTIONS row and the
        // DISTORTION row. "Why is my glass not refracting in the headset" must
        // be answerable from the log rather than from a header.
        const size_t dropped = pushed.looks.size() - fx.looks.size();
        const bool lostRefract = pushed.refractions && !fx.refractions;
        const bool lostDistort = pushed.distortion && !fx.distortion;
        const size_t state = dropped * 4u + (lostRefract ? 2u : 0u) + (lostDistort ? 1u : 0u);
        if (state != mVrPolicyDropped) {
            mVrPolicyDropped = state;
            std::string what;
            if (dropped)
                what = std::to_string(dropped) + " of this project's " +
                       std::to_string(pushed.looks.size()) + " look(s)";
            if (lostRefract) what += (what.empty() ? "" : ", ") + std::string("refractions");
            if (lostDistort) what += (what.empty() ? "" : ", ") + std::string("distortion");
            if (!what.empty())
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka VR: " + what + " are not drawn in the headset - each of them "
                    "reads the TARGET at a coordinate that is not this pixel's (a look's "
                    "centre, a refraction's or a distortion's offset), and in a target "
                    "holding two eyes side by side that coordinate crosses the seam into "
                    "the other eye (jahshaka::engine::applyVrViewPolicy)");
        }
    }
    if (fx == mPostFx) return;   // hosts push per frame; the same value is free
    const ChainDesc before = chainDesc();
    mPostFx = fx;
    const ChainDesc after = chainDesc();
    // Only a SHAPE change rebuilds. Exposure, bloom threshold, AO power and the
    // SMAA preset are uniforms or shader reloads, not graph edits.
    if (!ChainDesc::sameShape(before, after)) rebuildWorkspaceDef();
    // ...and the one thing that is NOT a shape change but still lives in the
    // graph: the fixed tonemap's exposure clear.
    applyFixedExposure();
    // THE DITHER'S OFF SWITCH, PUSHED HERE AS WELL AS PER FRAME, and the "as
    // well" is load-bearing (lane DITHER-1). Everything else the chain tunes
    // per view rides chain::ViewGlobalsListener, which OgreEngine arms once a
    // frame for every view that is ENABLED — and the VR session's View is not:
    // the session drives its own workspace and leaves the engine's loop out of
    // it, so the eye picture receives no per-view push at all (not exposure,
    // not the bloom threshold, not the AO camera terms, and not this). The
    // tonemap's parameter is process-global, so a write here reaches the eyes
    // whoever owns the view; it is debounced inside setDither and costs
    // nothing when the value has not moved.
    if (chainDesc().hdr) chain::setDither(chainDesc().ditherOff);
    // ...and the bloom amount, which is a uniform on the SAME material and is
    // therefore unreachable by the same view for the same reason. It is inert
    // in the headset today — applyVrViewPolicy turns bloom off in both eyes.
    // NOTE (the merge read, 2026-09-20): this push-time write does NOT make the
    // dial honest in the eyes the day that policy changes — the session's
    // workspace runs outside the engine loop, so every desktop view's per-frame
    // listener write lands AFTER this one and the eye reads whichever view
    // pushed last. The dither has the same shape and is only masked because it
    // is process-wide. When bloom reaches the headset, the session must run
    // applyViewGlobals for its own view before its update instead of relying
    // on this line; until then this costs one debounced float comparison.
    if (chainDesc().hdr && chainDesc().bloom) chain::setBloomAmount(chainDesc().bloomAmount);
}

const PostFxDesc &OgreView::postFx() const { return mPostFx; }

void OgreView::resetExposureHistory() { seedExposureHistory(0.0f); }

void OgreView::seedExposureHistory(float scale) {
    // A CALLER'S VALUE IS THE TONEMAPPER'S MULTIPLIER, which is exactly what
    // the seed pass writes and what measuredExposureScale() reads back — one
    // unit, three places. Anything that is not a positive finite number means
    // "the descriptor's own seed" (the reflection_map class of defect:
    // `isfinite && > 0` is the gate), which is what resetExposureHistory is.
    const bool haveValue = scale > 0.0f && std::isfinite(scale);
    // THE GRAPH MAY NOT HAVE A SEED PASS YET, and the caller cannot be expected
    // to know (lead review round 2). The seed pass only exists for
    // `hdr && !tonemapFixed` (chain::build), and an offscreen view without
    // allowOffscreen has no chain at all — but the one caller that matters is a
    // SECOND ON-SCREEN VIEW taking the screen, whose chain is still the
    // passthrough one at that moment and becomes an HDR chain on its first
    // synced frame. Returning quietly there is how the hand-over became a
    // silent no-op followed by a fresh chain seeding itself at 1.0.
    //
    // So a value the graph cannot take yet is REMEMBERED, and attachWorkspace —
    // the one seam every (re)build goes through — spends it on the chain it
    // just built, before that chain has rendered anything. Exactly once: a
    // build that has no seed pass DROPS it rather than holding a stale
    // exposure for some later, unrelated rebuild.
    if (!mWorkspace || !mChainHandles.exposureSeed) {
        mPendingExposureSeed = haveValue ? scale : 0.0f;
        return;
    }
    const ChainDesc d = chainDesc();
    if (!d.hdr || d.tonemapFixed) {
        mPendingExposureSeed = haveValue ? scale : 0.0f;
        return;
    }
    mPendingExposureSeed = 0.0f;
    const float seed = haveValue ? scale : chain::exposureSeed(d.exposure);
    const Ogre::ColourValue colour(seed, seed, seed, seed);
    JAH_TRY {
        writeLiveClearColour(mWorkspace, mChainHandles.exposureSeed, colour);
        for (Ogre::CompositorNode *n : mWorkspace->getNodeSequence()) {
            if (!n) continue;
            for (Ogre::CompositorPass *p : n->_getPasses()) {
                if (!p || p->getDefinition() != mChainHandles.exposureSeed) continue;
                // The seed is `mNumInitialPasses = 1` — it has already been
                // spent, once, when the workspace was built. This is what puts
                // it back, and it is the whole reason the pass is reachable
                // from here (verified in the pin: CompositorPass's ctor takes
                // mNumPassesLeft from the definition and only this call resets
                // it, so the spec's R4 "seeds at workspace build" claim is TRUE).
                p->resetNumPassesLeft();
            }
        }
    } JAH_CATCH(mError, );
}

/// THE EXPOSURE THIS VIEW ACTUALLY GRADED WITH, read back off the GPU (SS1).
///
/// WHY IT HAS TO BE A READBACK. The automatic exposure is computed entirely on
/// the GPU — the meter's three compute jobs bin the frame's log-luminance into
/// a histogram and resolve it into a 1x1 texture which the tonemapper then
/// samples as `fInvLumAvg` (EXPOSURE-2; four downscale quads before) — so the CPU never
/// sees the number at all. Nothing else in this engine knows it, and no formula
/// reproduces it (it is a temporal filter over the scene's own content).
///
/// WHICH TEXTURE. `jahOldLum`, the adaptation HISTORY, not the `jahLum` the
/// tonemapper samples: kLum is Discardable (its content between frames is
/// undefined memory by definition) while the history is explicitly
/// keep_content, and the chain's last HDR pass copies one into the other — so
/// the history holds exactly the multiplier the last presented frame graded
/// with. One 1x1 R32_FLOAT texel, downloaded with accurate tracking; the cost
/// is one fence on a path that already stalls (a screenshot).
///
/// 0 means "there is nothing to read": no workspace, no HDR, the FIXED form
/// (whose exposure is a constant the caller already has), a chain with NO
/// METER (a shader profile without the compute jobs defines no history
/// texture, so the search below finds none), or a workspace that has not
/// presented a frame YET.
///
/// "YET" IS PER WORKSPACE, NOT PER VIEW, and that distinction is a defect that
/// was caught in review rather than in the field. `jahOldLum` is destroyed and
/// recreated with the workspace, so every REBUILD — and a rebuild is what any
/// shape change causes, `world.override({id:'ssr', value:'off'})` included —
/// starts the adaptation history over. mFramesPresented does not reset there
/// (it resets on scene bind and detach), so gating on it would have let a
/// rebuild-then-shoot sequence read the one-shot 1.0 seed at best and UNWRITTEN
/// VRAM at worst: the reflection_map class of defect, where `isfinite && > 0`
/// happily passes garbage into the grade. mWorkspaceFramesPresented is reset
/// inside attachWorkspace, beside the generation counter, so it can only mean
/// "frames THIS graph has drawn".
float OgreView::measuredExposureScale() const {
    if (!mWorkspace || !mRoot) return 0.0f;
    const ChainDesc d = chainDesc();
    if (!d.hdr || d.tonemapFixed) return 0.0f;
    if (mWorkspaceFramesPresented == 0u) return 0.0f;
    float measured = 0.0f;
    JAH_TRY {
        Ogre::TextureGpu *lum = nullptr;
        for (Ogre::CompositorNode *n : mWorkspace->getNodeSequence()) {
            if (!n) continue;
            if (Ogre::TextureGpu *t = n->getDefinedTexture(chain::exposureHistoryTextureName())) {
                lum = t;
                break;
            }
        }
        if (!lum) return 0.0f;
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        // THE TICKET IS OWNED, not just created: `download` and `map` can both
        // OGRE_EXCEPT (residency loss, a device reset), and the old shape leaked
        // a staging allocation on every one of those. A ticket is not a
        // SharedPtr, so the scope guard is the ownership.
        struct TicketScope {
            Ogre::TextureGpuManager *tm = nullptr;
            Ogre::AsyncTextureTicket *ticket = nullptr;
            bool mapped = false;
            ~TicketScope() {
                if (!ticket) return;
                if (mapped) ticket->unmap();
                tm->destroyAsyncTextureTicket(ticket);
            }
        } held{ tm, tm->createAsyncTextureTicket(1u, 1u, 1u, Ogre::TextureTypes::Type2D,
                                                 lum->getPixelFormat()) };
        if (!held.ticket) return 0.0f;
        held.ticket->download(lum, 0, true);
        const Ogre::TextureBox box = held.ticket->map(0);
        held.mapped = true;
        measured = box.getColourAt(0, 0, 0, lum->getPixelFormat()).r;
    } JAH_CATCH(mError, 0.0f);
    // A history that has not converged to anything finite is not an exposure.
    if (!(measured > 0.0f) || !std::isfinite(measured)) return 0.0f;
    return measured;
}

OgreView::~OgreView() { destroy(); }

const std::string &OgreView::name() const { return mName; }
Scene *OgreView::scene() const { return mScene; }

bool OgreView::setCameraNode(NodeId node) {
    JAH_TRY {
        if (!mCamera) { mError = "setCameraNode: this view has no camera yet"; return false; }
        if (!node) {
            if (!mCameraNode) return true;
            mCameraNode = 0;
            // Back onto the scene root, and back to the pushed pose — the
            // description is still the last one the host sent.
            mCamera->detachFromParent();
            mScene->sceneManager()->getRootSceneNode(Ogre::SCENE_DYNAMIC)->attachObject(mCamera);
            mCamera->setPosition(toOgre(mCameraDesc.position));
            mCamera->setOrientation(Ogre::Quaternion(mCameraDesc.orientation.w,
                                                     mCameraDesc.orientation.x,
                                                     mCameraDesc.orientation.y,
                                                     mCameraDesc.orientation.z));
            return true;
        }
        if (!mScene) { mError = "setCameraNode: this view has no scene"; return false; }
        Ogre::SceneNode *target = mScene->node(node);
        if (!target) { mError = "setCameraNode: unknown node in this view's scene"; return false; }
        if (mCameraNode == node && mCamera->getParentSceneNode() == target) return true;
        mCamera->detachFromParent();
        target->attachObject(mCamera);
        // The camera's own local transform is the IDENTITY: the node carries the
        // pose, exactly as the pushed description used to.
        mCamera->setPosition(Ogre::Vector3::ZERO);
        mCamera->setOrientation(Ogre::Quaternion::IDENTITY);
        mCameraNode = node;
        return true;
    } JAH_CATCH(mError, false);
}

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
        // NO SCENE MEANS THE CLEAR-ONLY CHAIN, not "no workspace at all" (lane
        // STALE-VIEW-1). The seam is the seam: every caller that dropped a
        // workspace and asks for it back — a definition rebuild, a resize, an
        // MSAA change, the shadow-atlas swap, detachScene itself — gets back
        // whichever chain this view is entitled to right now.
        if (!mScene || !mCamera) return attachBlankWorkspace();
        // ...and a scene bind replaces the clear-only chain rather than stacking
        // a second workspace on the same target.
        detachBlankWorkspace();
        Ogre::TextureGpu *t = target();
        if (!t) return false;
        mWorkspace = mRoot->getCompositorManager2()->addWorkspace(
            mScene->sceneManager(), t, mCamera, mWorkspaceDef, mEnabled);
        if (!mWorkspace) return false;
        for (Ogre::CompositorWorkspaceListener *l : mWorkspaceListeners)
            mWorkspace->addListener(l);
        ++mWorkspaceGeneration;
        // A NEW GRAPH HAS DRAWN NOTHING. Every keep_content texture in the chain
        // — the HDR adaptation history above all — was just destroyed and
        // recreated, so anything that reads one back has to know that this
        // particular graph has not written it yet (measuredExposureScale says
        // why that is not the same question as mFramesPresented).
        mWorkspaceFramesPresented = 0;
        // THE REMEMBERED EXPOSURE SEED (seedExposureHistory's note). This is
        // the frame-zero of a brand-new graph: its keep_content textures have
        // just been created and the seed pass still owes its one initial
        // execution, so a value the caller handed over before this chain
        // existed lands here and nowhere else. Spent exactly once — a chain
        // with no seed pass drops it rather than holding it for a later,
        // unrelated rebuild.
        if (mPendingExposureSeed > 0.0f) {
            const float pending = mPendingExposureSeed;
            mPendingExposureSeed = 0.0f;
            seedExposureHistory(pending);
        }
        // RE-ASSERT THE INSET'S POSITION (CAMERAS_SPEC §7.2's ordering trap).
        // The main workspace has just been appended, so it is now LAST on the
        // target and would paint over the inset. There is no reorder API: the
        // inset is removed and re-added, here, after every single main-workspace
        // build — which is exactly why that all goes through this one seam.
        syncPip();
        applyPip();
        return true;
    } JAH_CATCH(mError, false);
}

void OgreView::setVrSsrOverride(int row) {
    if (mVrSsrOverride == row) return;
    mVrSsrOverride = row;
    // Re-apply the policy to what this view is already carrying: the override is
    // set before the first push in practice, but a view that never re-pushed
    // would otherwise keep the row it was built with.
    if (mStereo) setPostFx(mPostFx);
}

void OgreView::setStereo(bool on, const std::string &cullCamera) {
    if (mStereo == on && mCullCameraName == cullCamera) return;
    mStereo = on;
    mCullCameraName = cullCamera;
    // THE POLICY FOLLOWS THE FLAG (lane EYE-GRADE-1). A view told it is stereo
    // after its description was pushed — which is the order the session builds
    // in — must re-filter what it is holding, or it would render one chain
    // shape with a description the stereo target cannot carry until the next
    // push arrived. Re-entering setPostFx here is safe: the policy is
    // idempotent, so the guarded compare below drops the call when nothing
    // moved.
    if (on) {
        PostFxDesc fx = mPostFx;
        applyVrViewPolicy(fx, mVrSsrOverride);
        mPostFx = fx;
    }
    // The flag lives on the pass DEFINITIONS, so this is a definition rebuild
    // and not a live write — the same operation a shadow-node or an effect
    // change performs. It happens exactly twice per session (begin and end).
    rebuildWorkspaceDef();
}

bool OgreView::detachWorkspace() {
    // The clear-only chain targets the same texture and answers the same
    // question (see the declaration); a view never has both.
    if (!mWorkspace) return detachBlankWorkspace();
    // THE REFLECTION TRACE HOLDS THIS CHAIN'S TEXTURES (PHOTON_SPEC §7 R5).
    // Everything below is about to destroy them; see dropReflectState().
    dropReflectState();
    JAH_TRY {
        // The inset's workspace targets the same texture and names a camera the
        // main path may be about to replace: it goes first, and comes back
        // through attachWorkspace's syncPip.
        if (mPipWorkspace) {
            mRoot->getCompositorManager2()->removeWorkspace(mPipWorkspace);
            mPipWorkspace = nullptr;
        }
        for (Ogre::CompositorWorkspaceListener *l : mWorkspaceListeners)
            mWorkspace->removeListener(l);
        mRoot->getCompositorManager2()->removeWorkspace(mWorkspace);
        mWorkspace = nullptr;
        return true;
    } JAH_CATCH(mError, false);
}

// ---------------------------------------------------------------------------
// THE CLEAR-ONLY WORKSPACE (chain::buildBlank; lane STALE-VIEW-1). Reached only
// through attachWorkspace/detachWorkspace above, so it obeys the same seam every
// other workspace in this engine does.
bool OgreView::attachBlankWorkspace() {
    if (mBlankWorkspace) return true;
    JAH_TRY {
        Ogre::TextureGpu *t = target();
        if (!t || !mEngine) return false;
        Ogre::SceneManager *sm = mEngine->blankSceneManager();
        if (!sm) return false;
        if (!mBlankCamera) {
            // ONE CAMERA, FOR THE LIFE OF THE VIEW. It is created on a scene
            // manager that outlives every scene, so — unlike the scene chain's
            // camera — it survives the binds and there is nothing to churn.
            // Nothing is ever drawn through it: the only pass that names it
            // renders the overlay queues, which are screen-space.
            mBlankCamera = sm->createCamera(mName + "/BlankCamera");
            mBlankCamera->setAutoAspectRatio(true);
        }
        if (mBlankWorkspaceDef.empty()) mBlankWorkspaceDef = mName + "/BlankWorkspace";
        // REBUILT EVERY TIME, because the two things the chain is made of can
        // both have moved since the last one: the view's BACKGROUND (the clear
        // colour) and its overlay entitlement. A definition rebuild of two
        // passes costs nothing, and it keeps setBackground's contract —
        // "rebuildWorkspaceDef and the next frame has the new colour" — true
        // for a scene-less view as well.
        chain::destroy(mRoot->getCompositorManager2(), mBlankWorkspaceDef, mBlankNodeDefs);
        chain::buildBlank(mRoot->getCompositorManager2(), mBlankWorkspaceDef, mBackground,
                          overlaysAllowed(), mBlankNodeDefs);
        mBlankWorkspace = mRoot->getCompositorManager2()->addWorkspace(
            sm, t, mBlankCamera, mBlankWorkspaceDef, mEnabled);
        return mBlankWorkspace != nullptr;
    } JAH_CATCH(mError, false);
}

bool OgreView::detachBlankWorkspace() {
    if (!mBlankWorkspace) return false;
    JAH_TRY {
        mRoot->getCompositorManager2()->removeWorkspace(mBlankWorkspace);
        mBlankWorkspace = nullptr;
        return true;
    } JAH_CATCH(mError, false);
}

void OgreView::destroyBlankChain() {
    detachBlankWorkspace();
    JAH_TRY {
        chain::destroy(mRoot->getCompositorManager2(), mBlankWorkspaceDef, mBlankNodeDefs);
        // A CAMERA IS THE ONLY EVIDENCE THIS VIEW EVER HAD A CLEAR-ONLY CHAIN,
        // and it is what the null test is for: asking the engine for the blank
        // manager here would CREATE one for a view that never used it (a
        // getter that allocates, in a teardown path).
        if (mBlankCamera) {
            if (Ogre::SceneManager *sm = mEngine ? mEngine->blankSceneManager() : nullptr)
                sm->destroyCamera(mBlankCamera);
            mBlankCamera = nullptr;
        }
    } JAH_CATCH(mError, );
}

void OgreView::syncGlobalsListener() {
    // THE ARMING RULE, and it is what preserves the determinism law for free:
    // chainDesc() has already cleared every effect for an offscreen view that
    // did not opt in, so anyEffect() is false there and no listener is ever
    // created — a thumbnail cannot be reached by this mechanism even in
    // principle.
    const bool wanted = mEnabled && mScene && mCamera && chainDesc().anyEffect();
    if (!wanted) {
        if (mGlobalsListener) {
            removeWorkspaceListener(mGlobalsListener.get());
            mGlobalsListener.reset();
        }
        return;
    }
    if (!mGlobalsListener) {
        mGlobalsListener.reset(new chain::ViewGlobalsListener());
        // Through the seam, so it survives every workspace rebuild — which is
        // constant here: an enable-flag change to the post chain IS a rebuild.
        addWorkspaceListener(mGlobalsListener.get());
    }
    mGlobalsListener->mRoot = mRoot;
    mGlobalsListener->mView = this;
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
    if (mEnabled && mWorkspace && mScene) { ++mFramesPresented; ++mWorkspaceFramesPresented; }
    // ...and the other half (View::blankFramesPresented): the frames this view
    // put on screen with NO scene bound — its background and the HUD over it.
    // Counted separately and never reset, because it answers a different
    // question: not "are there pixels of THIS world yet" but "did the teardown
    // reach the screen at all".
    else if (mEnabled && mBlankWorkspace && !mScene) ++mBlankFramesPresented;
}

unsigned long long OgreView::blankFramesPresented() const { return mBlankFramesPresented; }

void OgreView::detachScene(bool takeBlank) {
    JAH_TRY {
        detachWorkspace();
        // The inset's camera belongs to the scene that is going away, and its
        // definitions name this view — both die here, workspace before camera.
        destroyPip();
        if (mCamera && mScene && mScene->sceneManager()) mScene->sceneManager()->destroyCamera(mCamera);
        mCamera = nullptr;
        // The camera rode a node of the scene that is going away.
        mCameraNode = 0;
        mScene  = nullptr;
        mFramesPresented = 0;
        // AND THE CLEAR-ONLY CHAIN TAKES OVER, in the same call (lane
        // STALE-VIEW-1). Without it this view would present nothing until a
        // scene was bound again, and a window that presents nothing keeps the
        // frame the X server was last given — the world that has just been torn
        // down. See chain::buildBlank. (Not on the way out: see `takeBlank`.)
        if (takeBlank) attachWorkspace();
    } JAH_CATCH(mError, );
}

void OgreView::setCamera(const CameraDesc &c) {
    JAH_TRY {
        if (!mCamera) return;
        // TWO CAMERA PROPERTIES REACH THE GRAPH: the LETTERBOX (extra passes
        // and inset viewports) and the ORTHOGRAPHIC GATE in chainDesc() (which
        // clears SSR and SSAO for an axis view). Both go through the same "only
        // a shape change rebuilds" rule as the post chain, and through the same
        // comparison setPostFx uses rather than a second hand-written test —
        // everything else, including the authored aspect and the camera's whole
        // pose, is free.
        const ChainDesc before = chainDesc();
        mCameraDesc = c;
        if (!ChainDesc::sameShape(before, chainDesc())) rebuildWorkspaceDef();
        applyLetterbox();
        // THE POSE, unless the camera is RIDING A NODE (setCameraNode): then the
        // node is the pose and these two fields are stale by definition — they
        // were read outside the frame, and the node resolves inside it.
        if (!mCameraNode) {
            mCamera->setPosition(toOgre(c.position));
            mCamera->setOrientation(Ogre::Quaternion(c.orientation.w, c.orientation.x,
                                                     c.orientation.y, c.orientation.z));
        }
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
            // THE WIDE-ASPECT FRAMING HOLD (CameraDesc::framingAspect). The
            // aspect is the REAL target's, read here and not cached, for the
            // same reason the ortho branch above reads it: a window mid-resize
            // must project through the size it actually has. A camera with no
            // hold (every authored one) — and every camera on a target at or
            // below its framing aspect — takes the identity path and its
            // fovDegrees reaches setFOVy bit-for-bit unchanged.
            const unsigned tw = width(), th = height();
            const float targetAspect = th ? float(tw) / float(th) : 1.0f;
            const float fov = verticalFovForFramingAspect(c.fovDegrees, targetAspect,
                                                          c.framingAspect);
            mCamera->setFOVy(Ogre::Degree(std::max(1.0f, std::min(fov, 179.0f))));
        }
        // §7.4: setAutoAspectRatio is ON for every ordinary view (the image
        // fills the target), and must be OFF for a constrained one or the
        // camera adopts the target's aspect and the shot is stretched into the
        // bars instead of fitted between them.
        mCamera->setAutoAspectRatio(!chainDesc().letterbox);
        if (chainDesc().letterbox) mCamera->setAspectRatio(c.aspect);
        applyLensShift(mCamera, c, viewAspect(c));
    } JAH_CATCH(mError, );
}

// ---------------------------------------------------------------------------
// LENS SHIFT (CAMERA_LENS_SPEC §3). The document authors a FRACTION OF THE
// FRAME; a projection needs a world-space offset at the near plane. This is the
// conversion, and it lives here — not in the document and not in the mirror —
// because it needs the aspect the view is ACTUALLY rendering at, which only a
// view knows.
//
// THE TRAP, verified in the pin (OgreFrustum.cpp:370-371): setFrustumOffset is
// world-space and Ogre scales it internally by mNearDist / mFocalLength, where
// mFocalLength is the STEREO divisor (default 1.0) and emphatically NOT a
// camera lens — nothing in this program calls Frustum::setFocalLength. With the
// default the near distance cancels, but the multiplication is written out
// anyway: the cancellation is a property of a default somebody could change.
//
// The DOCUMENT does the identical arithmetic for its own projection matrix
// (iris::lens::nearOffsetFromShift, CameraNode::updateCameraMatrices) so
// picking rays land in the shifted image. The two must agree; the formula is
// one line and the engine may not include the document's headers, so it is
// spelled twice on purpose, each pointing at the other.
float OgreView::viewAspect(const CameraDesc &c) const {
    if (chainDesc().letterbox && c.aspect > 0.0f) return c.aspect;
    const unsigned w = width(), h = height();
    return h ? float(w) / float(h) : 1.0f;
}

void OgreView::applyLensShift(Ogre::Camera *camera, const CameraDesc &c, float aspect) {
    if (!camera) return;
    // Orthographic frusta ignore the offset entirely in the pin ("Unknown how
    // to apply frustum offset to orthographic camera, just ignore here",
    // OgreFrustum.cpp) — say so by zeroing rather than pretending.
    if (c.orthographic || (c.lensShiftX == 0.0f && c.lensShiftY == 0.0f)) {
        camera->setFrustumOffset(Ogre::Vector2::ZERO);
        return;
    }
    const float nearDist = std::max(c.nearClip, 0.001f);
    const float fovDeg   = std::max(1.0f, std::min(c.fovDegrees, 179.0f));
    const float halfH    = std::tan(float(fovDeg) * 0.5f * 3.14159265f / 180.0f) * nearDist;
    const float halfW    = halfH * (aspect > 0.0f ? aspect : 1.0f);
    const float stereoFocal = camera->getFocalLength() > 0.0f ? camera->getFocalLength() : 1.0f;
    const float scale    = nearDist / stereoFocal;
    // offsetAtNear = shift * (2 * halfExtent); frustumOffset = offsetAtNear / scale
    camera->setFrustumOffset(
        Ogre::Vector2((c.lensShiftX * 2.0f * halfW) / scale,
                      (c.lensShiftY * 2.0f * halfH) / scale));
}

// ---------------------------------------------------------------------------
// LETTERBOX (CAMERAS_SPEC §7.4). The FLAG is a graph change and lives in
// ChainDesc; the RECTANGLE is derived from the target's own aspect, so it moves
// on every resize and is written straight onto the pass definitions — which
// Ogre re-reads on every execute, so this rebuilds nothing.
void OgreView::applyLetterbox() {
    if (mChainHandles.insetPasses.empty() && mChainHandles.scissorPasses.empty()) return;
    JAH_TRY {
        const unsigned w = width(), h = height();
        const float targetAspect = h ? float(w) / float(h) : 1.0f;
        float inner[4];
        chain::letterboxRect(mCameraDesc.aspect, targetAspect, inner);
        for (Ogre::CompositorPassDef *p : mChainHandles.insetPasses) {
            auto &vp = p->mVpRect[0];
            vp.mVpLeft = inner[0]; vp.mVpTop = inner[1];
            vp.mVpWidth = inner[2]; vp.mVpHeight = inner[3];
            vp.mVpScissorLeft = inner[0]; vp.mVpScissorTop = inner[1];
            vp.mVpScissorWidth = inner[2]; vp.mVpScissorHeight = inner[3];
        }
        // The post quads: scissor only, viewport untouched (ChainHandles::
        // scissorPasses says why). The rectangle is relative, so it is the
        // same numbers whatever the quad's target resolution.
        for (Ogre::CompositorPassDef *p : mChainHandles.scissorPasses) {
            auto &vp = p->mVpRect[0];
            vp.mVpScissorLeft = inner[0]; vp.mVpScissorTop = inner[1];
            vp.mVpScissorWidth = inner[2]; vp.mVpScissorHeight = inner[3];
        }
        if (mChainHandles.letterboxSwatch)
            mChainHandles.letterboxSwatch->setAllClearColours(toOgre(mBackground));
    } JAH_CATCH(mError, );
}

void OgreView::setEnabled(bool on) {
    mEnabled = on;
    JAH_TRY {
        if (mWorkspace) mWorkspace->setEnabled(on);
        // ...and so does the clear-only one: a hidden viewport must not clear
        // pixels it does not own.
        if (mBlankWorkspace) mBlankWorkspace->setEnabled(on);
        // The inset rides the view: a disabled view that still ran its second
        // workspace would draw an inset onto a frame nobody else touched.
        if (mPipWorkspace) mPipWorkspace->setEnabled(on);
    } JAH_CATCH(mError, );
}

Colour OgreView::background() const { return mBackground; }
bool OgreView::shadows() const { return mShadows; }

void OgreView::setShadows(bool on) {
    if (on == mShadows) return;
    mShadows = on;
    rebuildWorkspaceDef();
}

// ONE VIEW'S FURNITURE (ChainDesc::helpers). A full workspace rebuild and not a
// live pass-definition write, deliberately: the mask is read from the
// DEFINITION at execute time, but a definition is shared by every workspace
// built from it and the rebuild path is the one seam that keeps mChainDesc, the
// handles and the adaptation history consistent. Hosts set it once, at view
// creation, so the rebuild is free.
void OgreView::setHelpersVisible(bool on) {
    if (on == mHelpersVisible) return;
    mHelpersVisible = on;
    rebuildWorkspaceDef();
}

void OgreView::setVrHelpersVisible(bool on) {
    if (on == mVrHelpersVisible) return;
    mVrHelpersVisible = on;
    rebuildWorkspaceDef();
}

// THE HIDDEN-AREA MESH'S CHANNEL, PER VIEW (kVrMaskBit, lane HAM-1). Graph
// shape exactly like the two above, and the VR session is its only caller: it
// opens the channel ONCE, when it creates its view and before the scene, so
// this rebuild never happens on a live workspace. (Not when the MASK is built,
// which is inside a frame — see the note at that call site.)
void OgreView::setHiddenAreaMask(bool on) {
    if (on == mHiddenAreaMask) return;
    mHiddenAreaMask = on;
    rebuildWorkspaceDef();
}

void OgreView::setLodHysteresisOffscreen(bool on) {
    if (on == mLodHysteresisOffscreen) return;
    mLodHysteresisOffscreen = on;
    // The band is written onto the pass definitions (applyLodHysteresis), so
    // this is graph shape exactly like the two flags above.
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
        chain::build(cm, mWorkspaceDef, chainDesc(), mNodeDefs, mChainHandles);
        mChainRayReflect = chainDesc().rayReflect;
        mChainProbeGather = chainDesc().probeGather;
        mChainSunContact = chainDesc().sunContact;
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

// The LIVE shadow node this view's workspace instantiated. `findShadowNode`
// does not create one (unlike findOrCreateShadowNode), so a view whose chain
// carries no shadow node — shadows off, or a chain phase that never names it —
// answers null instead of allocating an atlas.
Ogre::CompositorShadowNode *OgreView::shadowNodeInstance() const {
    if (!mWorkspace || !mShadows) return nullptr;
    return mWorkspace->findShadowNode(Ogre::IdString(kShadowNodeName));
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
            const unsigned hzbBefore = mPostFx.hzb ? hzbLevelsFor(width(), height()) : 0u;
            rebuildRtt(w, h);   // an RTT cannot be resized in place
            mWidth = w; mHeight = h;
            // THE HZB'S LEVEL COUNT IS GRAPH SHAPE (ChainDesc::hzbLevels): the
            // compositor resizes a factor-sized texture without rebuilding the
            // node, so a size change that changes the mip count would leave
            // compute passes addressing levels that no longer exist.
            if (mPostFx.hzb && hzbLevelsFor(width(), height()) != hzbBefore)
                rebuildWorkspaceDef();
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

bool OgreView::readPixelsHdr(ImageF &out) {
    if (!mTexture) { mError = "readPixelsHdr: View '" + mName + "' is on-screen"; return false; }
    if (!mChainHandles.radianceTexture || !mWorkspace) {
        mError = "readPixelsHdr: View '" + mName +
                 "' keeps no float scene result (PostFxDesc::hdrReadback is off, or no frame has "
                 "built the workspace yet)";
        return false;
    }
    return readChainTexture(mChainHandles.radianceTexture, out, "readPixelsHdr");
}

bool OgreView::readReflectionHdr(ImageF &out) {
    if (!mTexture) { mError = "readReflectionHdr: View '" + mName + "' is on-screen"; return false; }
    if (!mWorkspace) { mError = "readReflectionHdr: View '" + mName + "' has no workspace"; return false; }
    return readChainTexture(chain::reflectionTextureName(), out, "readReflectionHdr");
}

bool OgreView::readChainTexture(const char *textureName, ImageF &out, const char *who) {
    JAH_TRY {
        // A LOCAL texture of the chain's scene node: the node that defines it
        // answers, every other node throws, so ask the one the view built (the
        // exposure history is found the same way).
        Ogre::TextureGpu *src = nullptr;
        const Ogre::IdString name(textureName);
        const Ogre::IdString sceneNode(chain::sceneNodeDefName(mWorkspaceDef));
        for (Ogre::CompositorNode *n : mWorkspace->getNodeSequence()) {
            if (n && n->getName() == sceneNode) {
                // getDefinedTexture THROWS on a name the node does not declare
                // (a chain with no SSR stage has no reflection texture), so ask
                // the definition's name map first.
                const auto &names = n->getDefinition()->getNameToChannelMap();
                if (names.find(name) != names.end()) src = n->getDefinedTexture(name);
                break;
            }
        }
        if (!src) {
            mError = std::string(who) + ": the chain of View '" + mName + "' defines no '" +
                     textureName + "'";
            return false;
        }
        const Ogre::PixelFormatGpu fmt = src->getPixelFormat();
        Ogre::TextureGpuManager *tm = mRoot->getRenderSystem()->getTextureGpuManager();
        const Ogre::uint32 w = src->getWidth(), h = src->getHeight();
        // Owned for the same reason measuredExposureScale's ticket is: download
        // and map can both throw, and a ticket is not a SharedPtr.
        struct TicketScope {
            Ogre::TextureGpuManager *tm = nullptr;
            Ogre::AsyncTextureTicket *ticket = nullptr;
            bool mapped = false;
            ~TicketScope() {
                if (!ticket) return;
                if (mapped) ticket->unmap();
                tm->destroyAsyncTextureTicket(ticket);
            }
        } held{ tm, tm->createAsyncTextureTicket(w, h, 1u, Ogre::TextureTypes::Type2D, fmt) };
        held.ticket->download(src, 0, true);
        const Ogre::TextureBox box = held.ticket->map(0);
        held.mapped = true;
        out.width = w; out.height = h;
        out.rgba.resize(static_cast<size_t>(w) * h * 4u);
        // getColourAt decodes whatever the format is (RGBA16F for both readers)
        // into float, exactly.
        for (Ogre::uint32 y = 0; y < h; ++y) {
            for (Ogre::uint32 x = 0; x < w; ++x) {
                const Ogre::ColourValue c = box.getColourAt(x, y, 0, fmt);
                float *o = &out.rgba[(static_cast<size_t>(y) * w + x) * 4u];
                o[0] = c.r; o[1] = c.g; o[2] = c.b; o[3] = c.a;
            }
        }
        return true;
    } JAH_CATCH(mError, false);
}

void OgreView::applyPendingResize() {
    // See the offscreen branch of resize(): the pyramid's level count is part of
    // the graph, so a size change that moves it rebuilds the chain. Free (one
    // integer compare) for every view that has no pyramid, which is all of them
    // until a Photon spike turns one on.
    const unsigned hzbBefore = mPostFx.hzb ? hzbLevelsFor(width(), height()) : 0u;
    applyPendingResizeImpl();
    if (mPostFx.hzb && hzbLevelsFor(width(), height()) != hzbBefore)
        rebuildWorkspaceDef();
}

void OgreView::applyPendingResizeImpl() {
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
    if (mEnabled && mScene && mCamera) {
        mScene->updateGiTracking(mCamera->getPosition(), mStereo);
        // The Forward+ depth-slice range follows the same camera, from the same
        // once-a-frame hook (LIGHTING_FIX fix 8). The scene does the rate
        // limiting and the hysteresis; this is only where the camera is known.
        mScene->updateForwardPlusRanges(mCamera);
    }
}

bool OgreView::warmUpShaders() {
    if (!mScene)  { mError = "warmUpShaders: no scene is bound to view '" + mName + "'"; return false; }
    if (!mCamera) { mError = "warmUpShaders: view '" + mName + "' has no camera"; return false; }
    JAH_TRY {
        if (!attachWorkspace()) { mError = "warmUpShaders: no workspace for view '" + mName + "'"; return false; }
        const std::string refNode = chain::sceneNodeDefName(mWorkspaceDef);
        // THE TWO ROUTES WANT OPPOSITE THINGS FROM THIS VIEW.
        //
        // CompositorPassWarmUp (the route ogre-patch 0016 unblocked) runs in its
        // OWN 4x4 workspace and this view's must stay DISABLED, or the frame
        // that drives it also renders the real thing at full resolution — the
        // ~250 ms this whole route exists to remove. If the view is somehow
        // already enabled, disable it for the duration.
        //
        // The fallback route IS this view's own frame, so there it is the other
        // way round: a disabled view renders nothing at all, and the caller's
        // whole reason for being here is that the view is not on screen yet
        // (the editor's viewport is disabled until its page is shown, with the
        // loading cover over it). Anything presented meanwhile is behind that
        // cover, which is a native window stacked above the viewport's
        // (src/viewport/viewportcover.h).
        const bool usesPass = chain::warmUpUsesPass(mRoot->getCompositorManager2(), refNode);
        const bool wasEnabled = mEnabled;
        const bool wantEnabled = !usesPass;
        if (wasEnabled != wantEnabled) setEnabled(wantEnabled);
        const bool ok = chain::warmUp(mRoot, mScene->sceneManager(), mCamera, refNode, mName);
        if (wasEnabled != wantEnabled) setEnabled(wasEnabled);
        return ok;
    } JAH_CATCH(mError, false);
}

void OgreView::destroy() {
    // NO CLEAR-ONLY CHAIN ON THE WAY OUT (`takeBlank`): building one here — and
    // with it, in a process that never lost a scene, the engine's blank
    // SceneManager, from inside ~OgreEngine — only to destroy it on the next
    // line is work nobody can see. Whatever this view already had comes down.
    detachScene(false);
    destroyBlankChain();
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

Ogre::TextureGpu *OgreView::targetTexture() const { return target(); }

}}}  // namespace jahshaka::engine::detail
