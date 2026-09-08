// The compositor chain: every view's workspace graph, ASSEMBLED IN C++.
//
// POST_CHAIN_SPEC.md §3 — why this exists rather than a folder of .compositor
// scripts: the chain has six independent switches (MSAA, HDR, SSAO, SMAA, SSR,
// refraction), i.e. 64 shapes. Ogre's own samples ship one hand-written script
// per shape; copying that pattern would mean owning 64 divergent copies of
// upstream sample scripts forever — the patches-only law by the back door.
// Everything needed to build the graph programmatically is public API
// (addNodeDefinition / addTextureDefinition / addTargetPass / addPass /
// addWorkspaceDefinition / connectExternal) and it is already the discipline
// the shadow node uses (OgreEngine::createShadowNode builds it with
// ShadowNodeHelper, not a script).
//
// TWO SHAPES, ONE FUNCTION:
//
//   PASSTHROUGH (every effect off, and EVERY offscreen view by construction):
//   one node, one target — the view's own window/RTT — and two scene passes
//   split by render queue. This must render BIT-IDENTICALLY to
//   CompositorManager2::createBasicWorkspaceDef, which is what every view used
//   before this file existed, and it is what keeps every pixel suite, every
//   thumbnail and every material preview exact.
//
//   POST CHAIN (any effect on, on-screen views only): the scene renders into an
//   offscreen colour target, the effect passes run, a final quad composites into
//   the window, and the overlays go on top of that. Still one node definition —
//   Ogre is happy with many targets and many passes in one node, and one node
//   is far easier to tear down exactly (OgreView owns the name list).
//
// RENDER-QUEUE POLICY (§6):
//   0            sky rectangle (OgreSky.cpp)
//   10           normal items (Ogre's default), incl. depth-tested outlines
//   15           PFX2 billboards
//   [.. 199]     everything else opaque      <- the OPAQUE pass
//   200          refractive items (phase 7)  <- the REFRACTIVE pass
//   kOverlayRenderQueue (210)  on-top overlays: gizmos, wires, always-on-top
//   [210 .. 254] the OVERLAY pass
// Keeping the overlays in their own pass keeps a bright unlit gizmo out of the
// SSAO normals G-buffer, out of the HDR luminance average and out of SMAA edge
// detection. Ogre's RenderQueue constructor fixes the modes: [0,100) and
// [200,225) are v2 FAST, so our v2 items can only live there.
//
// kIncludeOverlaysNote — THE mIncludeOverlays SWEEP (STATS_OVERLAY_SPEC §6.5).
// Ogre's own overlay set (the engine-drawn stats readout and loading cover,
// OgreOverlayHud.cpp) reaches a pass through CompositorPassSceneDef::
// mIncludeOverlays, which upstream DEFAULTS TO TRUE for every scene pass
// (Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h). Left alone, that
// would put a flat grey loading panel into every thumbnail, every material
// preview and every pixel suite in the tree — the cover is raised on EVERY
// world open, so this would not be a rare edge.
//
// So: every scene pass created anywhere in this backend sets it FALSE
// explicitly, and exactly one — the final "Jahshaka overlays" pass, in each of
// the two chain shapes — sets it from ChainDesc::overlays, which is true only
// for on-screen views and for offscreen views that passed
// ViewOverlayDesc::allowOffscreen. The rule is asserted, not argued:
// test_engine's hud_overlay_is_ignored_offscreen_unless_asked renders the same
// scene with a cover requested and with none and demands BYTE-IDENTICAL
// readbacks. The planar-reflection pass (OgrePlanar.cpp) sets it false too,
// even though its RQ range already excludes 254 — the guarantee must not
// depend on an RQ constant somebody may widen later.
#include <cmath>
#include "EnginePrivate.h"


#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
#include <Compositor/Pass/PassMipmap/OgreCompositorPassMipmapDef.h>
#include <Compositor/Pass/PassCompute/OgreCompositorPassComputeDef.h>
#include <Compositor/Pass/PassDepthCopy/OgreCompositorPassDepthCopyDef.h>
#include <Compositor/Pass/PassStencil/OgreCompositorPassStencilDef.h>
#include <Compositor/Pass/PassWarmUp/OgreCompositorPassWarmUp.h>
#include <Compositor/Pass/PassWarmUp/OgreCompositorPassWarmUpDef.h>
#include <OgreMaterialManager.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreGpuProgram.h>
#include <OgreGpuProgramParams.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreTextureUnitState.h>
#include <OgreBitwise.h>
#include <cstdlib>

namespace jahshaka { namespace engine { namespace detail {
namespace chain {

namespace {

/// The one input channel every chain has: the view's render target (window
/// texture or RTT), connected with connectExternal(0, ...).
constexpr const char *kTargetChannel = "JahTarget";

/// The scene's HDR colour target, and the resolved copy the post passes read
/// when MSAA is on (rt0 is explicit-resolve then, so the HDR box filter in
/// HDR/Resolve_4xFP32_HDR_Box does the resolve in the right colour space).
constexpr const char *kRt0        = "jahRt0";
constexpr const char *kResolvedRt = "jahResolvedRt";
/// Auto-exposure: a 1x1 luminance history that survives frames (keep_content),
/// and the 64/16/4/1 reduction chain that feeds it.
/// The constant HDR/FinalToneMapping samples in place of a measured exposure
/// (ChainDesc::tonemapFixed). Its shader multiplies the scene by this value
/// before the filmic curve, so it IS the exposure.
///
/// Derived to agree with the auto path rather than invented: the automatic
/// chain computes `1024 * e^(exposure - 2) / e^(mean log(luminance * 1024))`
/// (DownScale03_SumLumEnd_ps.glsl). Substituting a GREY CARD for the
/// measurement — luminance 0.18, the photographic mid-grey — collapses the
/// denominator to 0.18 * 1024 and the whole expression to
///
///     e^(exposure - 2) / 0.18
///
/// which at the scene default exposure of +0.6 is 1.37 against the ~1.2 an
/// ordinary lit scene measures. A thumbnail therefore grades within a few
/// percent of the viewport it is a thumbnail OF, and does it identically on
/// every machine and in every frame.
inline float fixedInverseLuminance(float exposure) {
    return std::exp(exposure - 2.0f) / 0.18f;
}

constexpr const char *kOldLum  = "jahOldLum";
constexpr const char *kLum     = "jahLum";
constexpr const char *kLumIter0 = "jahLumIter0";
constexpr const char *kLumIter1 = "jahLumIter1";
constexpr const char *kLumIter2 = "jahLumIter2";
/// Bloom ping-pong, at a FIXED 256x256 — the sample's own layout, and the
/// reason bloom is resolution-independent and nearly free.
constexpr const char *kBlur0 = "jahBlur0";
constexpr const char *kBlur1 = "jahBlur1";
/// SSAO: the main pass' second colour attachment, the half-res depth it
/// marches, and the AO buffer plus its separable blur.
constexpr const char *kGBufNormals = "jahGBufNormals";
constexpr const char *kDepthHalf   = "jahDepthHalf";
constexpr const char *kAo          = "jahAo";
constexpr const char *kAoBlurH     = "jahAoBlurH";
constexpr const char *kAoBlurV     = "jahAoBlurV";
constexpr const char *kAoApplied   = "jahAoApplied";
/// A NAMEABLE depth buffer. depth_pool ids cannot be sampled; SSAO marches the
/// depth, refraction copies it, so the scene pass renders through an explicit
/// RTV whose depth attachment is this texture.
constexpr const char *kDepth    = "jahDepth";
constexpr const char *kSceneRtv = "jahSceneRtv";
/// SSR. The prepass' second G-buffer (HlmsPbs writes shadow term in x and
/// packed roughness in y), the RTV the prepass renders through, the ray march's
/// output (hit coordinates, at half or full resolution), the full-resolution
/// reflection HlmsPbs samples, and the one-frame colour history the resolve
/// reads. See the SSR block in build() for the whole shape and why.
constexpr const char *kSsrShadowRough = "jahSsrShadowRough";
constexpr const char *kSsrPrepassRtv  = "jahSsrPrepassRtv";
constexpr const char *kSsrRays        = "jahSsrRays";
constexpr const char *kSsrReflection  = "jahSsrReflection";
constexpr const char *kSsrPrev        = "jahSsrPrev";
/// SMAA: LDR edge detection AFTER tonemapping, so it needs its own full-res
/// sRGB target to work on before the result reaches the window.
constexpr const char *kLdr      = "jahLdr";
constexpr const char *kSmaaEdges = "jahSmaaEdges";
constexpr const char *kSmaaBlend = "jahSmaaBlend";
/// Refraction (phase 7). Faithful to Samples/.../Refractions.compositor: the
/// refractive objects render into a MSAA-preserving CLONE of the opaque result
/// while SAMPLING the opaque result itself, and they need a non-MSAA copy of the
/// depth (HlmsPbs samples it per pixel, which an MSAA texture cannot do).
constexpr const char *kRefractOut   = "jahRefractOut";
constexpr const char *kRefractRtv   = "jahRefractRtv";
constexpr const char *kDepthNoMsaa  = "jahDepthNoMsaa";
/// The picture-in-picture inset's background swatch (CAMERAS_SPEC §7.7): a 4x4
/// texture cleared to ViewPipDesc::background and copied over the inset rect.
/// It exists because the inset's colour attachment must LOAD (a Vulkan clear is
/// full-target and would wipe the main frame) and a loaded attachment shows the
/// main image wherever the inset's scene draws nothing.
constexpr const char *kPipFill = "jahPipFill";
/// THE INSET'S OWN SCENE TARGET (CAMERAS_SPEC §7.2 Route C). RGBA16F while the
/// inset is graded, so the linear radiance above 1.0 survives to the tonemapper
/// exactly as it does in the main chain's kRt0; plain UNORM otherwise. It is
/// sized as a FRACTION of the view's target — the fraction the inset's INNER
/// rectangle occupies — which is what makes a window resize free and a square
/// in the world square in the inset.
constexpr const char *kPipScene = "jahPipScene";
/// The inset's 1x1 fixed exposure and its (always black) bloom input: the two
/// textures HDR/FinalToneMapping samples besides the scene. Both exist only
/// while the inset is graded. See addFixedExposureClear.
constexpr const char *kPipLum   = "jahPipLum";
constexpr const char *kPipBloom = "jahPipBloom";
/// The LETTERBOX background swatch (CAMERAS_SPEC §7.4): the same trick as
/// kPipFill, for the same reason. A letterboxed view clears its whole target to
/// the BAR colour (a Vulkan clear is full-target, which is exactly what bars
/// want) and then copies this 4x4 swatch of the view's real background into the
/// inner rectangle, so the shot sits on the scene's clear colour and the
/// remainder stays black.
constexpr const char *kLetterboxFill = "jahLetterboxFill";
/// The bars. Not configurable: every editor that letterboxes draws black ones,
/// and a bar colour setting is a preference nobody has asked for.
const Ogre::ColourValue kLetterboxBars(0.0f, 0.0f, 0.0f, 1.0f);

/// Declares a local texture AND the same-named RenderTargetView that makes it
/// usable as a target.
///
/// The RTV half is easy to miss and fails late: `addTextureDefinition` alone
/// creates the texture but no view, and Ogre only looks the view up when the
/// workspace is INSTANTIATED — an ItemIdentityException out of
/// getRenderTargetViewDef naming nothing but a hash. The script path does both
/// in one place (OgreScriptTranslator.cpp:6845-6860, right after filling the
/// TextureDefinition), including its rule that a DEPTH format attaches as depth
/// (and stencil, if the format carries one) rather than as colour 0. This is
/// that code, in our terms.
///
/// Returns the TextureDefinition. The pointer is valid only until the next
/// addTextureDefinition call — hence setNumLocalTextureDefinitions up front and
/// no holding on to it.
Ogre::TextureDefinitionBase::TextureDefinition *
addTex(Ogre::CompositorNodeDef *n, const char *name, Ogre::PixelFormatGpu fmt,
       Ogre::uint32 w = 0, Ogre::uint32 h = 0, float wf = 1.0f, float hf = 1.0f) {
    auto *td = n->addTextureDefinition(name);
    td->width = w; td->height = h;
    td->widthFactor = wf; td->heightFactor = hf;
    td->format = fmt;
    td->depthBufferId = 0;      // post-process targets need no depth
    td->fsaa = "1";

    Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(name);
    if (fmt == Ogre::PFG_UNKNOWN || !Ogre::PixelFormatGpuUtils::isDepth(fmt)) {
        Ogre::RenderTargetViewEntry attachment;
        attachment.textureName = name;
        rtv->colourAttachments.push_back(attachment);
        rtv->depthBufferId = td->depthBufferId;
        rtv->preferDepthTexture = td->preferDepthTexture;
        rtv->depthBufferFormat = td->depthBufferFormat;
    } else {
        rtv->depthAttachment.textureName = name;
        if (Ogre::PixelFormatGpuUtils::isStencil(fmt))
            rtv->stencilAttachment.textureName = name;
    }
    return td;
}

/// Re-syncs a texture's default RTV after the caller changed depth settings on
/// the TextureDefinition (addTex copies them at creation time).
void syncRtvDepth(Ogre::CompositorNodeDef *n, const char *name,
                  const Ogre::TextureDefinitionBase::TextureDefinition *td) {
    Ogre::RenderTargetViewDef *rtv = n->getRenderTargetViewDefNonConstNoThrow(name);
    if (!rtv || rtv->colourAttachments.empty()) return;
    rtv->depthBufferId = td->depthBufferId;
    rtv->preferDepthTexture = td->preferDepthTexture;
    rtv->depthBufferFormat = td->depthBufferFormat;
}

/// HARD-WON, AND IT SEGFAULTS: CompositorNodeDef::setNumTargetPass is a plain
/// vector<CompositorTargetDef>::reserve, and CompositorTargetDef has a
/// destructor that OGRE_DELETEs its passes but no move constructor. Growing the
/// vector after targets exist therefore destroys the passes the relocated
/// copies still point at — a read-after-destroy the moment the next pass is added.
/// So it is called EXACTLY ONCE, with a capacity no chain can exceed, before
/// the first addTargetPass. Nothing below may call it again.
constexpr size_t kMaxTargetPasses = 48;

/// THE STORE ACTION OF THE LAST PASS ANY WORKSPACE OF OURS PUTS ON A TARGET.
///
/// CAMERAS_SPEC §7.3 correction 1, proved in pixels by spikes/camera-pip-vulkan
/// (T4). The compositor's default for a final pass is `StoreOrResolve`, which
/// on an MSAA target resolves AND DISCARDS the multisample contents. That is
/// correct while exactly one workspace draws into the target — nothing follows,
/// so nothing needs the samples. It stops being correct the moment a SECOND
/// workspace renders into the same target (the picture-in-picture inset, phase
/// 2c): the second workspace's first pass Loads a colour attachment whose
/// samples were thrown away, and at 4x the whole frame outside the inset went
/// WHITE — 198,000 differing pixels in the spike, 13,575 in this tree's own
/// gate, and not a subtle artefact either time.
///
/// Plain `Store` is not the answer either: it keeps the samples and never
/// resolves, so with no PiP at all the frame is BLACK (that half is what
/// tests/engine's msaa_overlay_pass_resolves_at_every_sample_count pins).
///
/// `StoreAndMultisampleResolve` is both: the samples survive for whoever
/// renders next AND the resolve target is written, so the last resolve on the
/// target is the one you see. At 1x there are no samples to keep, so it is
/// byte-for-byte what `StoreOrResolve` did — which is what makes this change
/// pixel-neutral for every existing view and every pixel suite in the tree.
///
/// The rule, stated once: EVERY workspace on a target keeps the samples and
/// resolves; only the last one's resolve reaches the screen.
constexpr Ogre::StoreAction::StoreAction kMultiWorkspaceStore =
    Ogre::StoreAction::StoreAndMultisampleResolve;

Ogre::CompositorPassQuadDef *addQuad(Ogre::CompositorNodeDef *n, const char *target,
                                     const char *material, const char *profilingId) {
    Ogre::CompositorTargetDef *t = n->addTargetPass(target);
    t->setNumPasses(1);
    auto *q = static_cast<Ogre::CompositorPassQuadDef *>(t->addPass(Ogre::PASS_QUAD));
    q->mMaterialName = material;
    q->setAllLoadActions(Ogre::LoadAction::DontCare);
    q->mStoreActionDepth = Ogre::StoreAction::DontCare;
    q->mStoreActionStencil = Ogre::StoreAction::DontCare;
    q->mProfilingId = profilingId;
    return q;
}

/// THE FIXED EXPOSURE, in one place (POST_CHAIN_SPEC §14).
///
/// HDR/FinalToneMapping samples a 1x1 texture as `fInvLumAvg` and multiplies
/// the scene by it before the filmic curve, so CLEARING that texture to a value
/// IS setting the exposure — the identical shader path, with a number nobody
/// has to measure. Every frame (no mNumInitialPasses): the texture is
/// Discardable and a stale read would be undefined memory. The viewport
/// modifier is cleared because a 1x1 texture is never inset.
///
/// Shared by the main chain's tonemapFixed form and by the PICTURE-IN-PICTURE
/// inset, which is a secondary surface and therefore always fixed: §14's whole
/// argument is that an auto-exposed second surface is not a value anybody can
/// assert, and the inset must additionally be able to carry a PIPPED CAMERA's
/// own exposure without the main view moving.
Ogre::CompositorPassClearDef *addFixedExposureClear(Ogre::CompositorNodeDef *n,
                                                    const char *lumTex, float exposure) {
    Ogre::CompositorTargetDef *t = n->addTargetPass(lumTex);
    t->setNumPasses(1);
    auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
    const float invLum = fixedInverseLuminance(exposure);
    c->setAllClearColours(Ogre::ColourValue(invLum, invLum, invLum, invLum));
    c->mViewportModifierMask = 0x00;   // a 1x1 texture is never inset
    c->mProfilingId = "Jahshaka fixed exposure";
    return c;
}

/// THE TONEMAP QUAD, in one place: the main chain's composite into the LDR
/// image and the inset's composite into the window are the SAME material with
/// the same three inputs, and there must never be a second tonemapper in this
/// engine (POST_CHAIN_SPEC §14 — the whole point of the secondary-surface work
/// is that a thumbnail, a screenshot and an inset grade like the viewport).
/// Callers override load/store actions and the profiling id; nothing else.
Ogre::CompositorPassQuadDef *addTonemapQuad(Ogre::CompositorNodeDef *n, const char *target,
                                            const char *sceneTex, const char *lumTex,
                                            const char *bloomTex) {
    auto *q = addQuad(n, target, "HDR/FinalToneMapping", "Jahshaka HDR tonemap");
    q->addQuadTextureSource(0, sceneTex);
    q->addQuadTextureSource(1, lumTex);
    q->addQuadTextureSource(2, bloomTex);
    q->mStoreActionColour[0] = Ogre::StoreAction::Store;
    return q;
}

}   // namespace

std::string sceneNodeDefName(const std::string &workspaceDef) {
    return workspaceDef + "/Scene";
}

}   // namespace chain

bool ChainDesc::anyEffect() const {
    return hdr || ssao || smaaPreset >= 0 || ssr > 0 || refractions;
}

bool ChainDesc::sameShape(const ChainDesc &a, const ChainDesc &b) {
    // Only what changes the GRAPH. Exposure, bloom threshold, AO power and the
    // like are uniforms — pushing them must never rebuild a workspace.
    return a.shadows == b.shadows && a.hdr == b.hdr && a.bloom == b.bloom &&
           a.tonemapFixed == b.tonemapFixed &&
           a.letterbox == b.letterbox &&
           a.ssao == b.ssao && a.ssaoScale == b.ssaoScale &&
           a.smaaPreset == b.smaaPreset && a.ssr == b.ssr &&
           a.refractions == b.refractions && a.samples == b.samples &&
           a.overlays == b.overlays &&
           a.background.r == b.background.r && a.background.g == b.background.g &&
           a.background.b == b.background.b && a.background.a == b.background.a;
}

namespace chain {

// ---------------------------------------------------------------------------
void letterboxRect(float aspect, float targetAspect, float inner[4]) {
    inner[0] = 0.0f; inner[1] = 0.0f; inner[2] = 1.0f; inner[3] = 1.0f;
    if (!(aspect > 0.0f) || !(targetAspect > 0.0f)) return;
    if (aspect > targetAspect) {          // wider than the target: bars top and bottom
        const float h = targetAspect / aspect;
        inner[1] = (1.0f - h) * 0.5f;
        inner[3] = h;
    } else {                               // taller: bars left and right
        const float w = aspect / targetAspect;
        inner[0] = (1.0f - w) * 0.5f;
        inner[2] = w;
    }
}

namespace {

/// Confines a pass to the letterbox's inner rectangle. The RECT itself is
/// written later and per frame (OgreView::applyLetterbox); this only records
/// which passes take it.
void inset(ChainHandles &h, Ogre::CompositorPassDef *p) { h.insetPasses.push_back(p); }

/// The letterbox prologue on `target` (CAMERAS_SPEC §7.4): one quad pass that
/// CLEARS the whole target to the bar colour — a Vulkan clear ignores the
/// viewport and covers the whole attachment, which for once is exactly what is
/// wanted — and then draws the view's background into the inner rectangle only.
///
/// It has to be a quad and not a second clear for precisely the reason the
/// picture-in-picture inset needs one: there is no way to clear a REGION on
/// this backend (OgreVulkanRenderPassDescriptor.cpp:945). The background
/// travels as a 4x4 swatch through Ogre's own Ogre/Copy/4xFP32, so the
/// letterbox costs no new shader and no new media.
///
/// After this the scene passes LOAD colour and CLEAR depth, inset to the same
/// rectangle — so the bars survive and the shot is never stretched into them.
void addLetterboxPrologue(Ogre::CompositorNodeDef *n, const ChainDesc &desc,
                          const char *target, ChainHandles &handles) {
    {
        Ogre::CompositorTargetDef *t = n->addTargetPass(kLetterboxFill);
        t->setNumPasses(1);
        auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
        c->setAllClearColours(toOgre(desc.background));
        c->mStoreActionColour[0] = Ogre::StoreAction::Store;
        c->mStoreActionDepth     = Ogre::StoreAction::DontCare;
        c->mStoreActionStencil   = Ogre::StoreAction::DontCare;
        c->mProfilingId = "Jahshaka letterbox swatch";
        handles.letterboxSwatch = c;
    }
    Ogre::CompositorTargetDef *t = n->addTargetPass(target);
    t->setNumPasses(1);
    auto *q = static_cast<Ogre::CompositorPassQuadDef *>(t->addPass(Ogre::PASS_QUAD));
    q->mMaterialName = "Ogre/Copy/4xFP32";
    q->addQuadTextureSource(0, kLetterboxFill);
    q->setAllClearColours(kLetterboxBars);
    q->setAllLoadActions(Ogre::LoadAction::Clear);     // full-target: THE BARS
    q->mStoreActionColour[0] = Ogre::StoreAction::Store;
    q->mStoreActionDepth     = Ogre::StoreAction::Store;
    q->mStoreActionStencil   = Ogre::StoreAction::DontCare;
    q->mProfilingId = "Jahshaka letterbox";
    inset(handles, q);                                 // ...and the background inside
}

}   // namespace

void build(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
           const ChainDesc &desc, std::vector<std::string> &nodeDefsOut,
           ChainHandles &handlesOut) {
    handlesOut = ChainHandles();
    const std::string sceneNode = sceneNodeDefName(workspaceDef);
    Ogre::CompositorNodeDef *n = cm->addNodeDefinition(sceneNode);
    nodeDefsOut.push_back(sceneNode);
    n->addTextureSourceName(kTargetChannel, 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);
    // Once, here, and never again — see kMaxTargetPasses.
    n->setNumTargetPass(kMaxTargetPasses);

    bool msaa = desc.samples > 1;

    // -----------------------------------------------------------------------
    // PASSTHROUGH — the shape every view had before this file, and the shape
    // every offscreen view still has. Bit-identical to createBasicWorkspaceDef.
    if (!desc.anyEffect()) {
        if (desc.letterbox) {
            // Bars first, background inside them; the scene passes below then
            // LOAD colour instead of clearing it (a clear is full-target and
            // would wipe the bars) and confine themselves to the inner rect.
            n->setNumLocalTextureDefinitions(1);
            addTex(n, kLetterboxFill, Ogre::PFG_RGBA8_UNORM, 4u, 4u);
            addLetterboxPrologue(n, desc, kTargetChannel, handlesOut);
        }
        Ogre::CompositorTargetDef *t = n->addTargetPass(kTargetChannel);
        t->setNumPasses(2);
        {
            auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
            p->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName) : Ogre::IdString();
            p->setAllClearColours(toOgre(desc.background));
            p->setAllLoadActions(Ogre::LoadAction::Clear);
            if (desc.letterbox) {
                p->mLoadActionColour[0] = Ogre::LoadAction::Load;   // keep the bars
                inset(handlesOut, p);
            }
            // NOT the compositor default (StoreOrResolve): on an MSAA target
            // that resolves and DISCARDS the multisample contents, and the
            // overlay pass below still has to render into them. Plain Store
            // keeps the samples; the LAST pass on the target does the resolve.
            // On a non-MSAA target Store and StoreOrResolve are the same thing,
            // which is what makes the 1x path bit-identical.
            p->mStoreActionColour[0] = Ogre::StoreAction::Store;
            p->mStoreActionDepth     = Ogre::StoreAction::Store;
            p->mStoreActionStencil   = Ogre::StoreAction::DontCare;
            p->mFirstRQ = 0u;
            // Up to the OVERLAY queue, not the refractive one: with no
            // refraction pass in this shape, refractive materials must still
            // render (HlmsPbs falls back to plain glass when the pass does not
            // offer it a refraction texture). Cutting at 200 made them vanish.
            // Pixel-neutral otherwise — nothing else lives in [200, 210).
            p->mLastRQ  = kOverlayRenderQueue;
            p->mIncludeOverlays = false;   // see kIncludeOverlaysNote
            p->mProfilingId = "Jahshaka opaque";
        }
        {
            auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
            // Load actions keep the compositor defaults (Load everywhere); the
            // colour store is kMultiWorkspaceStore — see the note there. This is
            // the last pass of THIS workspace on the target, so it is where the
            // MSAA resolve belongs, but it must not be the DISCARDING kind.
            p->mStoreActionColour[0] = kMultiWorkspaceStore;
            p->mStoreActionDepth   = Ogre::StoreAction::DontCare;
            p->mStoreActionStencil = Ogre::StoreAction::DontCare;
            p->mFirstRQ = kOverlayRenderQueue;
            p->mLastRQ  = 255u;
            // THE ONE pass in the whole engine allowed to draw Ogre's overlay
            // set — and only when this view is entitled to it (see
            // kIncludeOverlaysNote and ChainDesc::overlays).
            p->mIncludeOverlays = desc.overlays;
            p->mProfilingId = "Jahshaka overlays";
            // Gizmos and wires belong to the SHOT, not to the bars.
            if (desc.letterbox) inset(handlesOut, p);
        }
        Ogre::CompositorWorkspaceDef *workDef = cm->addWorkspaceDefinition(workspaceDef);
        workDef->connectExternal(0, n->getName(), 0);
        return;
    }

    // -----------------------------------------------------------------------
    // POST CHAIN.
    //
    // HARDWARE MSAA IS OFF INSIDE THE CHAIN, deliberately and unconditionally.
    // Two failures were reproduced on this pin (Ogre v3.0.0-783-g52d1a7aaf) and
    // driver (NVIDIA 595.84), both in tests/engine's postfx_epic_shape_with_msaa:
    //
    //   * HDR + MSAA SEGFAULTS THE DRIVER. Creating the pipeline for
    //     HDR/Resolve_4xFP32_HDR_Box — the custom tonemapped box filter, which
    //     texelFetches a `texture2DMS` — crashes inside libnvidia-glvkspirv.
    //     It is not our shader and not our recompile: skipping the preprocessor
    //     reload entirely (the shader's own defaults are already 4 subsamples)
    //     crashes identically.
    //   * SSAO + MSAA renders BLACK. No exception, no log line; the AO march
    //     against multisampled depth simply produces nothing.
    //
    // Rather than ship a combination that crashes, the chain renders at 1x and
    // the AA comes from SMAA, which is a post pass and composes fine. The World
    // Modes table follows that policy (src/services/worldmodes.cpp), so no tier
    // ever asks for both. A user who sets both by hand gets the chain at 1x and
    // an unused multisampled window, not a crash.
    //
    // Textures first: addTextureDefinition may reallocate, so no
    // TextureDefinition pointer is held across another call.
    msaa = false;
    n->setNumLocalTextureDefinitions(26);   // 25 + the letterbox swatch
    if (desc.letterbox) addTex(n, kLetterboxFill, Ogre::PFG_RGBA8_UNORM, 4u, 4u);

    // SSR (POST_CHAIN_SPEC §4.1 row "SSR", §8 phase 6). Named
    // once here because half the shape below reads it.
    const bool ssr = desc.ssr > 0;

    // The scene target. RGBA16_FLOAT whenever HDR is on — that is the whole
    // point: light values above 1.0 survive to the tonemapper. Without HDR the
    // chain still needs an offscreen colour target (SSAO/SMAA/SSR/refraction
    // all composite), and it stays RGBA8_UNORM so colours do not move.
    {
        auto *td = addTex(n, kRt0, desc.hdr ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM);
        td->depthBufferId = 1u;                      // the scene needs depth
        td->preferDepthTexture = desc.ssao || desc.ssr;   // sampled by the AO/SSR passes
        if (msaa) {
            td->fsaa = std::to_string(desc.samples);
            // Explicit resolve: with HDR the resolve is a custom box filter in
            // the right colour space (HDR/Resolve_4xFP32_HDR_Box); a hardware
            // resolve of RGBA16F averages pre-tonemap radiance and fireflies win.
            //
            // EXCEPT with refractions, which need the OPPOSITE: the refractive
            // pass renders at the scene's sample count (it shares the depth
            // buffer) while SAMPLING the opaque image, so the opaque image has
            // to carry a hardware-resolved surface. HDR + MSAA + refraction
            // therefore resolves in hardware and skips the box filter — a real,
            // small quality trade, taken deliberately rather than crashing.
            if (desc.hdr && !desc.refractions)
                td->textureFlags |= Ogre::TextureFlags::MsaaExplicitResolve;
        }
        syncRtvDepth(n, kRt0, td);
    }
    // The custom HDR resolve target exists only when rt0 is explicit-resolve —
    // i.e. HDR and MSAA, without refractions (see the note on rt0's flags).
    const bool hdrExplicitResolve = msaa && desc.hdr && !desc.refractions;
    if (hdrExplicitResolve)
        addTex(n, kResolvedRt, Ogre::PFG_RGBA16_FLOAT);

    if (desc.hdr) {
        // THE FIXED-EXPOSURE VARIANT (PostFxDesc::tonemapFixed) drops the whole
        // luminance reduction: no history to adapt from, no 64/16/4 downscale
        // ladder. Only the 1x1 texture the tonemapper samples survives, and it
        // is CLEARED to a constant every frame instead of being computed.
        if (!desc.tonemapFixed) {
            // keep_content: the 1x1 luminance history is read next frame, so it
            // must NOT be DiscardableContent.
            auto *td = addTex(n, kOldLum, Ogre::PFG_R16_FLOAT, 1u, 1u);
            td->textureFlags = Ogre::TextureFlags::RenderToTexture;
            addTex(n, kLumIter0, Ogre::PFG_R16_FLOAT, 64u, 64u);
            addTex(n, kLumIter1, Ogre::PFG_R16_FLOAT, 16u, 16u);
            addTex(n, kLumIter2, Ogre::PFG_R16_FLOAT, 4u, 4u);
        }
        addTex(n, kLum,      Ogre::PFG_R16_FLOAT, 1u, 1u);
        // R10G10B10A2 rather than FP16: the pin's own note says FP16 bloom
        // buffers cost 0.748 ms on an HD 7770 at 1080p for no visible gain.
        {
            auto *td = addTex(n, kBlur0, Ogre::PFG_R10G10B10A2_UNORM, 256u, 256u);
            // With bloom OFF this target is cleared to black ONCE and then only
            // read (the tonemapper always samples it). A DiscardableContent
            // texture that nothing writes this frame is Undefined, and the
            // barrier solver refuses to transition Undefined to a read-only
            // layout — a black frame with one line in the log. keep_content is
            // exactly what the message asks for.
            if (!desc.bloom) td->textureFlags = Ogre::TextureFlags::RenderToTexture;
        }
        addTex(n, kBlur1, Ogre::PFG_R10G10B10A2_UNORM, 256u, 256u);
    }

    // A depth texture we can NAME (and therefore sample). depth_pool ids give a
    // buffer the compositor picks; SSAO's downsampler and refraction's copy both
    // need the depth as an input, so the scene pass renders through an explicit
    // RTV whenever any effect wants it.
    const bool namedDepth = desc.ssao || desc.ssr || desc.refractions;
    if (namedDepth) {
        auto *td = addTex(n, kDepth, Ogre::PFG_D32_FLOAT);
        td->preferDepthTexture = true;
        if (msaa) td->fsaa = std::to_string(desc.samples);
    }

    // THE NORMALS G-BUFFER, and it is ONE texture for two effects. SSAO gets it
    // from the main pass as a second colour attachment (mGenNormalsGBuf); SSR
    // gets it from its prepass, which writes normals AND roughness. When both
    // are on the prepass wins and the main pass writes no normals at all — the
    // content is identical (HlmsPbs emits `pixelData.normal * 0.5 + 0.5` in both
    // paths, 800.PixelShader_piece_ps.any:978 vs :988), and asking the main pass
    // for a second attachment it does not need is pure bandwidth.
    if (desc.ssao || ssr) {
        auto *td = addTex(n, kGBufNormals, Ogre::PFG_R10G10B10A2_UNORM);
        if (msaa) td->fsaa = std::to_string(desc.samples);
        syncRtvDepth(n, kGBufNormals, td);
    }

    if (ssr) {
        // The prepass' SECOND G-buffer: HlmsPbs writes the shadow term in x and
        // roughness in y, packed as (r - 0.02) * 1.02040816. RG16_UNORM is the
        // sample's own format for it and there is no reason to differ.
        addTex(n, kSsrShadowRough, Ogre::PFG_RG16_UNORM);
        // The prepass renders colour into the two G-buffers and depth into the
        // SAME depth texture the main pass then depth-TESTS against read-only
        // (setUseDepthPrePass sets mReadOnlyDepth). Sharing it is not a saving,
        // it is the contract: the main pass writes no depth, so every fragment
        // it draws has to find its own exact depth already there.
        {
            Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(kSsrPrepassRtv);
            Ogre::RenderTargetViewEntry normals, shadowRough;
            normals.textureName = kGBufNormals;
            shadowRough.textureName = kSsrShadowRough;
            rtv->colourAttachments.push_back(normals);
            rtv->colourAttachments.push_back(shadowRough);
            rtv->depthAttachment.textureName = kDepth;
            rtv->stencilAttachment.textureName = kDepth;
            rtv->preferDepthTexture = true;
        }
        // The march's output: hit coordinates, not colour. RGBA16_UNORM because
        // every channel is a [0,1] quantity (two texture coordinates and two
        // fades) and 16 bits of a UV is a quarter of a pixel at 16K.
        //
        // HALF RESOLUTION IS THE QUALITY ROW. `ssr == 1` marches a quarter of
        // the pixels; `ssr == 2` marches all of them. Nothing else in the graph
        // changes, which is why the row is a scale factor and not a shape.
        {
            const float s = desc.ssr >= 2 ? 1.0f : 0.5f;
            addTex(n, kSsrRays, Ogre::PFG_RGBA16_UNORM, 0u, 0u, s, s);
        }
        // What HlmsPbs actually samples. FULL resolution, always: the Pbs
        // shader does OGRE_Load2D( ssrTexture, iFragCoord, 0 ), i.e. an
        // unfiltered fetch at the fragment's own pixel, so a smaller texture
        // would read the wrong texel rather than a blurrier one.
        // RGBA16_FLOAT because the rgb is scene RADIANCE and, with HDR on, may
        // legitimately exceed 1.
        addTex(n, kSsrReflection, Ogre::PFG_RGBA16_FLOAT);
        // The one-frame colour history. keep_content (RenderToTexture rather
        // than the default DiscardableContent) for exactly the reason the HDR
        // luminance history needs it: it is written at the END of a frame and
        // read at the START of the next one, and a discardable texture nothing
        // wrote this frame is Undefined to the barrier solver.
        //
        // Its format follows the scene target's so the copy at the end of the
        // frame is an exact one.
        {
            auto *td = addTex(n, kSsrPrev,
                              desc.hdr ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM);
            td->textureFlags = Ogre::TextureFlags::RenderToTexture;
        }
    }

    if (desc.ssao) {
        // Half-res depth, exactly the sample's layout: the AO march is the
        // expensive part and it reads a downsampled MAX depth.
        {
            auto *td = addTex(n, kDepthHalf, Ogre::PFG_D32_FLOAT, 0u, 0u, 0.5f, 0.5f);
            td->preferDepthTexture = true;
        }
        // The AO buffer is the ONE resolution lever the stock shader leaves us:
        // its 64 taps are a compile-time double loop (SSAO_HS_ps.glsl:61-65), so
        // tiering the tap count would mean forking a sample shader.
        addTex(n, kAo, Ogre::PFG_R16_FLOAT, 0u, 0u, desc.ssaoScale, desc.ssaoScale);
        // The cross blur runs at FULL res — it is also the upsample.
        addTex(n, kAoBlurH, Ogre::PFG_R16_FLOAT);
        addTex(n, kAoBlurV, Ogre::PFG_R16_FLOAT);
        addTex(n, kAoApplied, desc.hdr ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM);
    }

    if (desc.smaaPreset >= 0) {
        // SMAA is LDR edge detection and MUST run after tonemapping (§4.3 item
        // 2), so the chain gains one full-res sRGB target for the tonemapped
        // image plus SMAA's own two working buffers and its output.
        addTex(n, kLdr, Ogre::PFG_RGBA8_UNORM_SRGB);
        // SMAA's stencil early-out: both working buffers share one depth-stencil
        // buffer (the sample uses depth_pool 8) with a stencil-carrying format.
        {
            auto *td = addTex(n, kSmaaEdges, Ogre::PFG_RG8_UNORM);
            td->depthBufferId = 8u;
            td->depthBufferFormat = Ogre::PFG_D32_FLOAT_S8X24_UINT;
            syncRtvDepth(n, kSmaaEdges, td);
        }
        {
            auto *td = addTex(n, kSmaaBlend, Ogre::PFG_RGBA8_UNORM);
            td->depthBufferId = 8u;
            td->depthBufferFormat = Ogre::PFG_D32_FLOAT_S8X24_UINT;
            syncRtvDepth(n, kSmaaBlend, td);
        }
    }

    if (desc.refractions) {
        // The clone the refractive objects render into. Same format and sample
        // count as the scene target, and it SHARES the scene's depth buffer —
        // refractives depth-test against the opaque geometry.
        {
            auto *td = addTex(n, kRefractOut,
                              desc.hdr ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM);
            if (msaa) td->fsaa = std::to_string(desc.samples);
        }
        {
            Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(kRefractRtv);
            Ogre::RenderTargetViewEntry colour0;
            colour0.textureName = kRefractOut;
            rtv->colourAttachments.push_back(colour0);
            rtv->depthAttachment.textureName = kDepth;
            rtv->stencilAttachment.textureName = kDepth;
            rtv->preferDepthTexture = true;
        }
        // The MSAA depth resolve. R32_FLOAT, NOT a depth format: this is a
        // sampled texture, and the sample's own note says resolving it rather
        // than sampling MSAA depth is what keeps refraction affordable.
        if (msaa) addTex(n, kDepthNoMsaa, Ogre::PFG_R32_FLOAT);
    }

    // The view the scene pass renders through when it needs more than a plain
    // colour target: a named depth attachment, and for SSAO a second colour
    // attachment for the normals G-buffer — unless SSR's prepass already filled
    // it (see the note on kGBufNormals above).
    if (namedDepth) {
        Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(kSceneRtv);
        Ogre::RenderTargetViewEntry colour0;
        colour0.textureName = kRt0;
        rtv->colourAttachments.push_back(colour0);
        if (desc.ssao && !ssr) {
            Ogre::RenderTargetViewEntry colour1;
            colour1.textureName = kGBufNormals;
            rtv->colourAttachments.push_back(colour1);
        }
        rtv->depthAttachment.textureName = kDepth;
        rtv->stencilAttachment.textureName = kDepth;
        rtv->preferDepthTexture = true;
    }

    // -----------------------------------------------------------------------
    // Passes. Order is the frame's order.

    // Auto-exposure history must start at something finite or the first frame
    // reads NaN out of an undefined 1x1 target.
    if (desc.hdr && !desc.tonemapFixed) {
        Ogre::CompositorTargetDef *t = n->addTargetPass(kOldLum);
        t->setNumPasses(1);
        auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
        c->mNumInitialPasses = 1;
        // Ogre's sample seeds this with 0.01, which means "start almost black and
        // brighten over the next second". In an EDITOR that reads as a black
        // flash every time HDR is switched on, the window is resized or a
        // workspace is rebuilt — the history texture is recreated each time.
        // 1.0 is roughly where a normally-lit scene converges anyway, so the
        // first frame is already about right and adaptation only trims it.
        c->setAllClearColours(Ogre::ColourValue(1.0f, 1.0f, 1.0f, 1.0f));
        c->mProfilingId = "Jahshaka HDR luminance seed";
        // Handed back so a CAMERA CUT can re-seed the history rather than let
        // it fade across the cut (CAMERA_LENS_SPEC §4 / R4 —
        // OgreView::resetExposureHistory). The build-time value stays 1.0: it
        // is what every existing frame was built on, and the derived seed is
        // only for a cut, where we know which exposure we are cutting TO.
        handlesOut.exposureSeed = c;
    }

    // -----------------------------------------------------------------------
    // SSR (POST_CHAIN_SPEC §4.1 row "SSR", §8 phase 6).
    //
    // WHY THERE IS A SECOND SCENE TRAVERSAL, stated once so nobody tries to
    // remove it: the reflection has to EXIST BEFORE the pass that shades with
    // it. HlmsPbs consumes it inside its pixel shader —
    //     hlms_use_ssr: envColourS = lerp( envColourS, ssr.rgb, ssr.w )
    // (800.PixelShader_piece_ps.any:927) — and the property only appears in
    // PrePassUse mode with an ssr texture bound (OgreHlms.cpp:3748-3767). So
    // the frame is: prepass writes normals + roughness + depth, the march reads
    // them, the resolve turns hits into radiance, and only then does the real
    // scene pass run and composite. That ordering is Ogre's, not a choice of
    // ours, and it is also why the colour the resolve samples is the PREVIOUS
    // frame's (there is no other one yet).
    //
    // WHAT IS OURS: the two quads and their shaders (media/Hlms/Jahshaka/
    // JahSsr*), a deliberately simpler marcher than the sample's — no
    // reprojection matrix, no compute colour history, no mip chain — plus the
    // resolution row and the roughness cutoff. WHAT IS NOT: the composite.
    //
    // MSAA never reaches here (the chain forces 1x, see the block above), which
    // deletes the entire `use_prepass_msaa` half of upstream's recipe: no
    // explicit-resolve G-buffers, no depth resolve, no per-subsample coverage
    // test in the Pbs shader.
    if (ssr) {
        // The colour history, seeded ONCE. Without this the first frame's
        // resolve samples an Undefined texture; with it, the first frame simply
        // reflects black and the second is correct.
        {
            Ogre::CompositorTargetDef *t = n->addTargetPass(kSsrPrev);
            t->setNumPasses(1);
            auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
            c->mNumInitialPasses = 1;
            c->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
            c->mProfilingId = "Jahshaka SSR history seed";
        }
        // THE PREPASS. Same camera, same shadow node and — critically — the
        // same render-queue range as the opaque pass below, because that pass
        // depth-tests read-only against what this one wrote: a fragment the
        // prepass never drew has no depth to be equal to.
        {
            Ogre::CompositorTargetDef *t = n->addTargetPass(kSsrPrepassRtv);
            t->setNumPasses(1);
            auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
            p->mPrePassMode = Ogre::PrePassCreate;
            p->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName)
                                          : Ogre::IdString();
            // Clear to white, the sample's value: an unwritten normals texel
            // decodes to (1,1,1) rather than (-1,-1,-1), and the march rejects
            // it by DEPTH anyway (nothing was drawn, so the depth is still the
            // clear value — the test that ogre-patch 0011 taught SSAO).
            p->setAllClearColours(Ogre::ColourValue::White);
            p->setAllLoadActions(Ogre::LoadAction::Clear);
            p->mStoreActionColour[0] = Ogre::StoreAction::Store;
            p->mStoreActionColour[1] = Ogre::StoreAction::Store;
            p->mStoreActionDepth     = Ogre::StoreAction::Store;
            p->mStoreActionStencil   = Ogre::StoreAction::DontCare;
            p->mFirstRQ = 0u;
            p->mLastRQ  = desc.refractions ? kRefractiveRenderQueue : kOverlayRenderQueue;
            p->mIncludeOverlays = false;   // see kIncludeOverlaysNote
            p->mProfilingId = "Jahshaka SSR prepass";
            // A letterboxed view's G-buffer has to line up with its image.
            if (desc.letterbox) inset(handlesOut, p);
        }
        // THE MARCH. Half or full resolution per the quality row; the frustum
        // corners are what let the shader rebuild a view-space position from
        // one depth fetch instead of an inverse projection per pixel.
        {
            auto *q = addQuad(n, kSsrRays, "Jahshaka/SsrRayMarch", "Jahshaka SSR rays");
            q->addQuadTextureSource(0, kDepth);
            q->addQuadTextureSource(1, kGBufNormals);
            q->addQuadTextureSource(2, kSsrShadowRough);
            q->mFrustumCorners = Ogre::CompositorPassQuadDef::VIEW_SPACE_CORNERS_NORMALIZED_LH;
            q->mStoreActionColour[0] = Ogre::StoreAction::Store;
        }
        // THE RESOLVE, always full resolution — HlmsPbs fetches this one at the
        // fragment's own pixel.
        {
            auto *q = addQuad(n, kSsrReflection, "Jahshaka/SsrResolve", "Jahshaka SSR resolve");
            q->addQuadTextureSource(0, kSsrRays);
            q->addQuadTextureSource(1, kSsrShadowRough);
            q->addQuadTextureSource(2, kSsrPrev);
            q->mStoreActionColour[0] = Ogre::StoreAction::Store;
        }
    }

    // The opaque scene pass.
    {
        const char *sceneTarget = namedDepth ? kSceneRtv : kRt0;
        // LETTERBOX (§7.4) goes on the SCENE target, not on the window: the
        // bars have to be inside the image the post chain then tonemaps and
        // composites, or the composite quad would stretch a letterboxed image
        // back out over them.
        if (desc.letterbox) addLetterboxPrologue(n, desc, sceneTarget, handlesOut);
        Ogre::CompositorTargetDef *t = n->addTargetPass(sceneTarget);
        t->setNumPasses(1);
        auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
        p->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName) : Ogre::IdString();
        p->setAllClearColours(toOgre(desc.background));
        p->setAllLoadActions(Ogre::LoadAction::Clear);
        if (desc.letterbox) {
            p->mLoadActionColour[0] = Ogre::LoadAction::Load;   // keep the bars
            inset(handlesOut, p);
        }
        // WITH SSR THE DEPTH IS THE PREPASS'. setUseDepthPrePass sets
        // mReadOnlyDepth (OgreCompositorPassSceneDef.h:232-241), so this pass
        // writes no depth at all and every fragment it draws has to find its
        // own value already in the buffer — clearing it here would throw that
        // away and the frame would come out empty. Loading it also buys exact
        // early-Z for free, which is most of what pays for the extra traversal.
        if (ssr) p->mLoadActionDepth = Ogre::LoadAction::Load;
        // With refractions the opaque result must exist BOTH as multisample
        // (the refractive pass keeps rendering into a clone of it) and resolved
        // (that same pass samples it) — the sample's "store_and_resolve".
        p->mStoreActionColour[0] = (desc.refractions && msaa)
                                       ? Ogre::StoreAction::StoreAndMultisampleResolve
                                       : Ogre::StoreAction::Store;
        if (desc.ssao && !ssr) p->mStoreActionColour[1] = Ogre::StoreAction::Store;
        // Depth survives the pass: SSAO marches it, refraction copies it, and
        // the refractive pass depth-tests against it.
        p->mStoreActionDepth   = (desc.ssao || ssr || desc.refractions)
                                     ? Ogre::StoreAction::Store : Ogre::StoreAction::DontCare;
        p->mStoreActionStencil = Ogre::StoreAction::DontCare;
        // Ignored in a prepass mode (the flag's own documentation says so), and
        // with SSR on the RTV has one colour attachment anyway.
        p->mGenNormalsGBuf = desc.ssao && !ssr;
        if (ssr) {
            // THE COMPOSITE. Two G-buffers, the depth (only the MSAA path's
            // shader reads it, but the sample passes it and so do we), and the
            // reflection. This one call is what puts `hlms_use_ssr` into every
            // HlmsPbs shader of this pass.
            Ogre::IdStringVec prePassTextures;
            prePassTextures.push_back(Ogre::IdString(kGBufNormals));
            prePassTextures.push_back(Ogre::IdString(kSsrShadowRough));
            p->setUseDepthPrePass(prePassTextures, Ogre::IdString(kDepth),
                                  Ogre::IdString(kSsrReflection));
        }
        p->mFirstRQ = 0u;
        // Stop before the refractive queue only when there IS a refraction pass
        // to pick those items up; otherwise they render here, as plain glass.
        p->mLastRQ  = desc.refractions ? kRefractiveRenderQueue : kOverlayRenderQueue;
        p->mIncludeOverlays = false;   // see kIncludeOverlaysNote
        p->mProfilingId = "Jahshaka opaque";
    }

    // MSAA resolve, in HDR space.
    const char *hdrSrc = kRt0;
    if (hdrExplicitResolve) {
        auto *q = addQuad(n, kResolvedRt, "HDR/Resolve_4xFP32_HDR_Box", "Jahshaka HDR MSAA resolve");
        q->addQuadTextureSource(0, kRt0);
        q->addQuadTextureSource(1, kOldLum);
        hdrSrc = kResolvedRt;
    }

    // Refraction: the refractive items re-render on top, sampling the opaque
    // result and a non-MSAA depth copy (VISUAL_PARITY §4, re-hosted here).
    const char *sceneResult = hdrSrc;
    if (desc.refractions) {
        // MSAA depth has to be resolved before HlmsPbs can sample it.
        if (msaa) {
            auto *q = addQuad(n, kDepthNoMsaa, "Ogre/Resolve/1xFP32_Subsample0",
                              "Jahshaka refraction depth resolve");
            q->addQuadTextureSource(0, kDepth);
        }
        // An exact, MSAA-preserving clone of the opaque result. The refractive
        // objects render into the clone and sample the original — writing and
        // sampling the same texture in one pass is what this avoids.
        {
            Ogre::CompositorTargetDef *t = n->addTargetPass(kRefractRtv);
            t->setNumPasses(2);
            auto *tc = static_cast<Ogre::CompositorPassDepthCopyDef *>(t->addPass(Ogre::PASS_DEPTHCOPY));
            tc->setDepthTextureCopy(kRt0, kRefractOut);
            tc->mProfilingId = "Jahshaka refraction clone";
            auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
            p->setAllLoadActions(Ogre::LoadAction::Load);
            p->mStoreActionColour[0] = Ogre::StoreAction::StoreOrResolve;
            // DEPTH MUST SURVIVE THIS PASS whenever a LATER pass reads it.
            // Upstream's Refractions.compositor ends the frame here, so it says
            // `depth dont_care` — and DontCare is not "keep it, we just don't
            // promise": Vulkan's VK_ATTACHMENT_STORE_OP_DONT_CARE makes the
            // attachment's contents UNDEFINED, and the driver is free to hand
            // back recycled tiles. Our chain runs SSAO AFTER refraction (the AO
            // multiply belongs in linear HDR space), so with refractions on the
            // AO march was sampling an undefined depth buffer: garbage
            // occlusion, worst where the depth was uniform and the geometry test
            // is a knife edge — i.e. the SKY, which came out as blocks of
            // recycled-VRAM noise under the Epic chain (2026-09-03 defect lane;
            // sky_stays_smooth_under_the_post_chain is the pixel gate).
            p->mStoreActionDepth = (desc.ssao || desc.ssr) ? Ogre::StoreAction::Store
                                                           : Ogre::StoreAction::DontCare;
            p->mStoreActionStencil = Ogre::StoreAction::DontCare;
            // The shadow node was already computed for this camera by the opaque
            // pass; recomputing it would render every shadow map a second time.
            p->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName) : Ogre::IdString();
            p->mShadowNodeRecalculation = Ogre::SHADOW_NODE_REUSE;
            p->setUseRefractions(msaa ? kDepthNoMsaa : kDepth, kRt0);
            p->mFirstRQ = kRefractiveRenderQueue;
            p->mLastRQ  = kOverlayRenderQueue;
            p->mIncludeOverlays = false;   // see kIncludeOverlaysNote
            p->mProfilingId = "Jahshaka refractives";
        }
        sceneResult = kRefractOut;
    }

    // SSR's colour history for the NEXT frame — copied here, deliberately, and
    // not at the end of the chain: what a reflection needs is the scene's
    // LINEAR RADIANCE, which is what HlmsPbs' `envColourS` is measured in. The
    // tonemapped, bloomed, SMAA'd image further down is a picture, not a
    // radiance field, and lerping one into a specular term double-grades every
    // reflection. Refraction is already folded in at this point (glass reflects
    // what is behind it, correctly); ambient occlusion is not, which is the
    // right way round — AO is a shading term of the RECEIVING surface.
    if (ssr) {
        auto *q = addQuad(n, kSsrPrev, "Ogre/Copy/4xFP32", "Jahshaka SSR history");
        q->addQuadTextureSource(0, sceneResult);
        q->mStoreActionColour[0] = Ogre::StoreAction::Store;
    }

    // SSAO: half-res depth downsample -> AO march -> separable blur (which is
    // also the upsample) -> multiply into the scene colour.
    if (desc.ssao) {
        {
            auto *q = addQuad(n, kDepthHalf,
                              msaa ? "Ogre/Depth/DownscaleMax_Subsample0" : "Ogre/Depth/DownscaleMax",
                              "Jahshaka SSAO depth downsample");
            q->addQuadTextureSource(0, kDepth);
        }
        {
            auto *q = addQuad(n, kAo, "SSAO/HS", "Jahshaka SSAO");
            q->addQuadTextureSource(0, kDepthHalf);
            q->addQuadTextureSource(1, kGBufNormals);
            // The shader reconstructs view-space position from depth and needs
            // the far-plane corners in the quad's normals.
            q->mFrustumCorners = Ogre::CompositorPassQuadDef::VIEW_SPACE_CORNERS;
            q->setAllLoadActions(Ogre::LoadAction::Clear);
            q->setAllClearColours(Ogre::ColourValue::White);
        }
        {
            auto *q = addQuad(n, kAoBlurH, "SSAO/BlurH", "Jahshaka SSAO blur H");
            q->addQuadTextureSource(0, kAo);
            q->addQuadTextureSource(1, kDepthHalf);
        }
        {
            auto *q = addQuad(n, kAoBlurV, "SSAO/BlurV", "Jahshaka SSAO blur V");
            q->addQuadTextureSource(0, kAoBlurH);
            q->addQuadTextureSource(1, kDepthHalf);
        }
        {
            // A plain multiply (SSAO_Apply_ps.glsl), so it is legal — and more
            // correct — in linear HDR space, not just on the sample's LDR window.
            auto *q = addQuad(n, kAoApplied, "SSAO/Apply", "Jahshaka SSAO apply");
            q->addQuadTextureSource(0, kAoBlurV);
            q->addQuadTextureSource(1, sceneResult);
            q->mStoreActionColour[0] = Ogre::StoreAction::Store;
        }
        sceneResult = kAoApplied;
    }

    // HDR: exposure (measured or fixed), bloom, tonemap.
    if (desc.hdr) {
      if (desc.tonemapFixed) {
        // THE CONSTANT EXPOSURE, written where the reduction would have written
        // it (addFixedExposureClear says why, once, for both surfaces that use
        // it — this chain and the picture-in-picture inset).
        handlesOut.fixedExposure = addFixedExposureClear(n, kLum, desc.exposure);
      } else {
        {
            auto *q = addQuad(n, kLumIter0, "HDR/DownScale01_SumLumStart", "Jahshaka HDR luminance start");
            q->addQuadTextureSource(0, sceneResult);
        }
        {
            auto *q = addQuad(n, kLumIter1, "HDR/DownScale02_SumLumIterative", "Jahshaka HDR luminance");
            q->addQuadTextureSource(0, kLumIter0);
        }
        {
            auto *q = addQuad(n, kLumIter2, "HDR/DownScale02_SumLumIterative", "Jahshaka HDR luminance");
            q->addQuadTextureSource(0, kLumIter1);
        }
        {
            auto *q = addQuad(n, kLum, "HDR/DownScale03_SumLumEnd", "Jahshaka HDR luminance end");
            q->addQuadTextureSource(0, kLumIter2);
            q->addQuadTextureSource(1, kOldLum);
        }
        {
            auto *q = addQuad(n, kOldLum, "Ogre/Copy/1xFP32", "Jahshaka HDR luminance history");
            q->addQuadTextureSource(0, kLum);
        }
      }
        // BLOOM RUNS FOR BOTH EXPOSURE FORMS, and so does its stand-in clear:
        // HDR/FinalToneMapping samples the bloom buffer unconditionally, so
        // kBlur0 must be written whichever way the exposure was arrived at.
        if (desc.bloom) {
            {
                auto *q = addQuad(n, kBlur0, "HDR/BrightPass_Start", "Jahshaka bloom bright pass");
                q->addQuadTextureSource(0, sceneResult);
                q->addQuadTextureSource(1, kLum);
            }
            // The sample's exact ping-pong: V, H, V, H, then four more H passes.
            // Reproduced rather than "improved" — the widths are tuned together.
            struct Blur { const char *target, *src, *material; };
            const Blur blurs[] = {
                { kBlur1, kBlur0, "HDR/BoxBlurH" }, { kBlur0, kBlur1, "HDR/BoxBlurV" },
                { kBlur1, kBlur0, "HDR/BoxBlurH" }, { kBlur0, kBlur1, "HDR/BoxBlurV" },
                { kBlur1, kBlur0, "HDR/BoxBlurH" }, { kBlur0, kBlur1, "HDR/BoxBlurH" },
                { kBlur1, kBlur0, "HDR/BoxBlurH" }, { kBlur0, kBlur1, "HDR/BoxBlurH" },
            };
            for (const Blur &b : blurs) {
                auto *q = addQuad(n, b.target, b.material, "Jahshaka bloom blur");
                q->addQuadTextureSource(0, b.src);
            }
        } else {
            // HDR/FinalToneMapping always samples the bloom buffer; with bloom
            // off it must read black rather than undefined memory. One clear,
            // once, is cheaper and far more honest than a second material.
            Ogre::CompositorTargetDef *t = n->addTargetPass(kBlur0);
            t->setNumPasses(1);
            auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
            c->mNumInitialPasses = 1;
            c->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
            c->mProfilingId = "Jahshaka bloom disabled";
        }
    }

    // Composite into the LDR image SMAA works on, or straight into the window.
    const char *ldrTarget = desc.smaaPreset >= 0 ? kLdr : kTargetChannel;
    {
        if (desc.hdr) {
            addTonemapQuad(n, ldrTarget, sceneResult, kLum, kBlur0);
        } else {
            auto *q = addQuad(n, ldrTarget, "Ogre/Copy/4xFP32", "Jahshaka composite");
            q->addQuadTextureSource(0, sceneResult);
            q->mStoreActionColour[0] = Ogre::StoreAction::Store;
        }
    }

    // SMAA: edge detection, blending weights, neighbourhood blend.
    if (desc.smaaPreset >= 0) {
        // Pass 1 writes stencil = 1 wherever it found an edge; pass 2 runs only
        // where the stencil equals 1. That early-out is most of SMAA's speed.
        {
            Ogre::CompositorTargetDef *t = n->addTargetPass(kSmaaEdges);
            t->setNumPasses(2);
            auto *st = static_cast<Ogre::CompositorPassStencilDef *>(t->addPass(Ogre::PASS_STENCIL));
            st->mStencilParams.enabled = true;
            st->mStencilParams.readMask = 0xFF;
            st->mStencilParams.writeMask = 0xFF;
            st->mStencilParams.stencilFront.compareOp = Ogre::CMPF_ALWAYS_PASS;
            st->mStencilParams.stencilFront.stencilPassOp = Ogre::SOP_REPLACE;
            st->mStencilParams.stencilFront.stencilDepthFailOp = Ogre::SOP_KEEP;
            st->mStencilParams.stencilFront.stencilFailOp = Ogre::SOP_KEEP;
            st->mStencilParams.stencilBack = st->mStencilParams.stencilFront;
            st->mStencilRef = 1u;
            st->mProfilingId = "Jahshaka SMAA stencil write";
            auto *q = static_cast<Ogre::CompositorPassQuadDef *>(t->addPass(Ogre::PASS_QUAD));
            q->mMaterialName = "SMAA/EdgeDetection";
            q->addQuadTextureSource(0, kLdr);
            q->setAllLoadActions(Ogre::LoadAction::Clear);
            q->setAllClearColours(Ogre::ColourValue(0, 0, 0, 0));
            q->mProfilingId = "Jahshaka SMAA edges";
        }
        {
            Ogre::CompositorTargetDef *t = n->addTargetPass(kSmaaBlend);
            t->setNumPasses(3);
            auto *st = static_cast<Ogre::CompositorPassStencilDef *>(t->addPass(Ogre::PASS_STENCIL));
            st->mStencilParams.enabled = true;
            st->mStencilParams.readMask = 0xFF;
            st->mStencilParams.writeMask = 0xFF;
            st->mStencilParams.stencilFront.compareOp = Ogre::CMPF_EQUAL;
            st->mStencilParams.stencilFront.stencilPassOp = Ogre::SOP_KEEP;
            st->mStencilParams.stencilFront.stencilDepthFailOp = Ogre::SOP_KEEP;
            st->mStencilParams.stencilFront.stencilFailOp = Ogre::SOP_KEEP;
            st->mStencilParams.stencilBack = st->mStencilParams.stencilFront;
            st->mStencilRef = 1u;
            st->mProfilingId = "Jahshaka SMAA stencil test";
            auto *q = static_cast<Ogre::CompositorPassQuadDef *>(t->addPass(Ogre::PASS_QUAD));
            q->mMaterialName = "SMAA/BlendingWeightCalculation";
            q->addQuadTextureSource(0, kSmaaEdges);
            q->mLoadActionColour[0] = Ogre::LoadAction::Clear;
            q->mClearColour[0] = Ogre::ColourValue(0, 0, 0, 0);
            q->mLoadActionDepth = Ogre::LoadAction::Load;
            q->mLoadActionStencil = Ogre::LoadAction::Load;
            q->mProfilingId = "Jahshaka SMAA weights";
            auto *off = static_cast<Ogre::CompositorPassStencilDef *>(t->addPass(Ogre::PASS_STENCIL));
            off->mStencilParams.enabled = false;
            off->mProfilingId = "Jahshaka SMAA stencil off";
        }
        {
            auto *q = addQuad(n, kTargetChannel, "SMAA/NeighborhoodBlending", "Jahshaka SMAA blend");
            q->addQuadTextureSource(0, kLdr);
            q->addQuadTextureSource(1, kSmaaBlend);
            q->mStoreActionColour[0] = Ogre::StoreAction::Store;
        }
    }

    // Overlays, straight onto the window, after every effect: gizmos, wires and
    // always-on-top helpers must not be tonemapped, blurred or edge-detected.
    {
        Ogre::CompositorTargetDef *t = n->addTargetPass(kTargetChannel);
        t->setNumPasses(1);
        auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
        p->mLoadActionColour[0] = Ogre::LoadAction::Load;
        p->mLoadActionDepth     = Ogre::LoadAction::DontCare;
        p->mLoadActionStencil   = Ogre::LoadAction::DontCare;
        p->mStoreActionColour[0] = kMultiWorkspaceStore;   // see kMultiWorkspaceStore
        p->mStoreActionDepth    = Ogre::StoreAction::DontCare;
        p->mStoreActionStencil  = Ogre::StoreAction::DontCare;
        p->mFirstRQ = kOverlayRenderQueue;
        p->mLastRQ  = 255u;
        // The effect shape's copy of THE ONE overlay-bearing pass — same rule
        // as the passthrough shape's (kIncludeOverlaysNote).
        p->mIncludeOverlays = desc.overlays;
        p->mProfilingId = "Jahshaka overlays";
        // Gizmos and wires belong to the SHOT, not to the bars.
        if (desc.letterbox) inset(handlesOut, p);
    }

    Ogre::CompositorWorkspaceDef *workDef = cm->addWorkspaceDefinition(workspaceDef);
    workDef->connectExternal(0, n->getName(), 0);
}

// ---------------------------------------------------------------------------
// PSO precache (SHADER_CACHE_SPEC.md §5) — CompositorPassWarmUp, which is what
// the spec asked for all along.
//
// THE HISTORY, because the comment that used to live here said this was
// impossible and it was wrong about WHY (SHADER_CACHE_AUDIT.md F2).
//
// The design is Ogre's `WarmUpHelper::createFrom`: clone the view's scene
// passes into a throwaway warm-up node, render that into a 4x4 target, and
// every variant the scene needs — including shadow casters and render queues
// nothing visible occupies — is compiled without drawing anything.
//
// It was built, it segfaulted, and the crash was blamed on the shadow-node term
// of Forward+'s cached-grid key: "shadow nodes are per workspace, so a warm-up
// pass in its own workspace can never match the view's entry." That diagnosis
// was WRONG. The key does contain the shadow node, but that is not what fails,
// because there is nothing to match against in the first place:
//
//   SceneManager::_cullPhase01 calls mForwardPlusImpl->collectLights( camera )
//   immediately before RenderQueue::renderPassPrepare (OgreSceneManager.cpp:
//   1382-1385). _warmUpShadersCollect calls renderPassPrepare with NO
//   collectLights anywhere in the function. collectLights is the ONLY thing
//   that inserts into mCachedGrid, so on a warm-up pass the cache has no entry
//   for that camera at all; getCachedGridFor misses, outCachedGrid is left null
//   and getGridBuffer dereferences it. In a build with NDEBUG the
//   "You must call ForwardPlusBase::collectLights first!" assert is compiled
//   out and the null deref is the whole story (OgreForwardPlusBase.cpp:527-538).
//
// The fix is upstream and is three lines: **ogre-patch 0016** adds the missing
// collectLights to _warmUpShadersCollect, mirroring _cullPhase01 exactly. With
// it the insert and the lookup happen inside the SAME call, off the SAME
// camera and the SAME viewport, so every term of the key — including the
// light-visibility mask CompositorPassWarmUp::execute hardcodes to 0xFFFFFFFF
// (:101) and the current shadow node — agrees by construction. The
// cross-workspace worry never arises; nothing is being matched across passes.
//
// WHAT THIS BUYS over the old "just render a real frame" route:
//   * the warm-up target is 4x4 (WarmUpHelper shrinks every local texture), so
//     the arming open costs milliseconds instead of a full-resolution frame —
//     which is what unblocks `shader_warmup_on_open` defaulting ON;
//   * the pass ignores culling and sweeps ALL render queues, so it reaches
//     permutations the camera cannot currently see;
//   * it copies the reference node's PASS_SHADOWS passes too, so shadow-caster
//     variants compile as well.
//
// AND IT IS OFF BY DEFAULT ANYWAY. The audit's standing caveat — "no upstream
// sample exercises WarmUpHelper, it is lightly-trodden code" — was right twice
// over. Patch 0016 removes the FIRST crash; there is a SECOND one downstream of
// it that this lane could not fix without a second patch, and it is a
// read-after-destroy, which is not something to default anybody into:
//
//   SIGSEGV in ParallelHlmsCompileQueue::warmUpSerial (OgreRenderQueue.cpp:1333)
//   on the SECOND world opened in an editor session, every time. At the crash
//   `request.queuedRenderable.renderable->getDatablock()` is null and the
//   request's `movableObject` cannot be read at all — the collected requests
//   name objects from a world that has since been destroyed. Neither
//   RenderQueue::warmUpShaders (:1169) nor warmUpSerial (:1332) null-checks the
//   datablock, and nothing clears the pending request list when the scene that
//   filled it goes away. Reproduced under gdb on an Xvfb display with the app
//   driven over MCP: cold open fine, close, warm open dead.
//
//   NOT reproducible offscreen (tests/shadercache/test_warm_up.cpp case 7
//   rebinds three worlds on one view and survives), which is why this is a
//   default-off engine route and not a test that would have caught it.
//
// So: `JAHSHAKA_WARMUP_PASS=1` opts in, for the suite that measures what the
// route buys and for whoever picks the second patch up. Everything else takes
// the boring route below — render the view's own workspace a couple of frames
// while the caller's loading cover is up. That warms what the camera can SEE
// plus its shadow casters, and nothing else; the pass would have covered more.
namespace {

/// OFF unless asked. An env var rather than a setting because the thing it
/// switches is an ENGINE route with a known crash, not a product policy.
bool warmUpPassEnabled() {
    static const bool on = []() {
        const char *v = std::getenv("JAHSHAKA_WARMUP_PASS");
        return v && *v && *v != '0';
    }();
    return on;
}

}   // namespace

bool warmUpUsesPass(Ogre::CompositorManager2 *cm, const std::string &refNodeDef) {
    return warmUpPassEnabled() && cm && cm->hasNodeDefinition(refNodeDef);
}

bool warmUp(Ogre::Root *root, Ogre::SceneManager *sm, Ogre::Camera *camera,
            const std::string &refNodeDef, const std::string &baseName) {
    if (!root) return false;
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    if (!sm || !camera || !warmUpUsesPass(cm, refNodeDef)) {
        // The fallback, and the ONLY route for a view whose chain we cannot see
        // (nothing in-tree hits that today, but a caller that passes an unknown
        // node def gets a warm-up rather than a refusal).
        root->renderOneFrame();
        return true;
    }

    const std::string nodeName = baseName + "/WarmUpNode";
    const std::string wsName   = baseName + "/WarmUp";
    // Left behind by an earlier failed attempt? Definitions are named and
    // global to the CompositorManager2; re-adding one throws.
    if (cm->hasWorkspaceDefinition(wsName)) cm->removeWorkspaceDefinition(wsName);
    if (cm->hasNodeDefinition(nodeName))    cm->removeNodeDefinition(nodeName);

    Ogre::TextureGpu *tiny = nullptr;
    Ogre::CompositorWorkspace *ws = nullptr;
    bool ok = false;
    try {
        // The external target the cloned node's rt_output input channel binds
        // to. 4x4 for the same reason WarmUpHelper shrinks its local textures:
        // nothing about WHICH shader gets built depends on the resolution.
        // PFG_RGBA8_UNORM matches every view target in this engine, and the
        // format IS a pass property (the Hlms hashes the render-target format),
        // so it must match or the warm-up compiles variants nothing draws.
        tiny = OgreView::createRtt(root, baseName + "/WarmUpRtt", 4u, 4u, 1u);

        Ogre::WarmUpHelper::createFrom(cm, nodeName, refNodeDef, false);

        // THE COLLECT/TRIGGER PAIRING, AND WHY WE HAVE TO ASSERT IT OURSELVES.
        //
        // A warm-up pass in Collect mode appends to two process-wide lists on
        // the SceneManager's RenderQueue — ParallelHlmsCompileQueue::mRequests
        // (each holding a raw Renderable*) and mPendingPassCaches — and only a
        // pass in Trigger mode consumes and clears them
        // (OgreRenderQueue.cpp:584-595). A Collect with no Trigger therefore
        // leaves RAW POINTERS INTO THIS WORLD'S RENDERABLES parked on the
        // render queue; close the world, open another, and the next Trigger
        // walks them. Measured: SIGSEGV in HlmsManager::getHlms off a stale
        // datablock, on the SECOND scene open in the editor, every time.
        //
        // WarmUpHelper marks the last pass CollectAndTrigger only when the
        // reference node's LAST target pass contains scene passes
        // (OgreCompositorPassWarmUp.cpp:348-352: `i + 1u == numTargetPasses`
        // is tested with `i` walking the REFERENCE node's target passes, from
        // inside a block that only runs for targets that have scene passes).
        // Our post-chain shape ends in a quad pass — the final composite into
        // the window — so that test is never true and NOTHING triggers.
        //
        // So: find the last warm-up pass we were given and make it trigger.
        // Upstream's intent, asserted from our side rather than assumed.
        {
            Ogre::CompositorNodeDef *warmDef = cm->getNodeDefinitionNonConst(nodeName);
            Ogre::CompositorPassWarmUpDef *last = nullptr;
            const size_t targets = warmDef ? warmDef->getNumTargetPasses() : 0u;
            for (size_t t = 0; t < targets; ++t)
                for (Ogre::CompositorPassDef *p :
                     warmDef->getTargetPass(t)->getCompositorPassesNonConst())
                    if (p->getType() == Ogre::PASS_WARM_UP)
                        last = static_cast<Ogre::CompositorPassWarmUpDef *>(p);
            if (!last) {
                // No warm-up pass at all: nothing would be collected, so there
                // is nothing to warm. Fall back rather than run an empty
                // workspace.
                throw Ogre::Exception(Ogre::Exception::ERR_INVALIDPARAMS,
                                      "warm-up node has no warm-up passes",
                                      "chain::warmUp");
            }
            last->mMode = Ogre::CompositorPassWarmUpDef::CollectAndTrigger;
        }

        Ogre::CompositorWorkspaceDef *wd = cm->addWorkspaceDefinition(wsName);
        wd->connectExternal(0, nodeName, 0);

        // The view that owns refNodeDef is DISABLED while this runs (see
        // OgreView::warmUpShaders), so this frame executes the warm-up
        // workspace and nothing else.
        ws = cm->addWorkspace(sm, tiny, camera, wsName, true);
        if (ws) {
            root->renderOneFrame();
            ok = true;
        }
    } catch (const Ogre::Exception &) {
        ok = false;
    }

    // Teardown in reverse, and unconditional: a throw halfway through must not
    // leave a node definition or a 4x4 render target behind for the next call
    // to collide with.
    if (ws) cm->removeWorkspace(ws);
    if (cm->hasWorkspaceDefinition(wsName)) cm->removeWorkspaceDefinition(wsName);
    if (cm->hasNodeDefinition(nodeName))    cm->removeNodeDefinition(nodeName);
    if (tiny) root->getRenderSystem()->getTextureGpuManager()->destroyTexture(tiny);

    if (!ok) {
        // Never leave the caller un-warmed because the untrodden path refused:
        // fall back to the frame that always worked.
        root->renderOneFrame();
        return true;
    }
    return true;
}

void destroy(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
             std::vector<std::string> &nodeDefs) {
    if (cm->hasWorkspaceDefinition(workspaceDef)) cm->removeWorkspaceDefinition(workspaceDef);
    // Reverse creation order: a node definition is only referenced by the
    // workspace definition, which is already gone, but the ordering keeps the
    // teardown reading like the rest of the backend.
    for (auto it = nodeDefs.rbegin(); it != nodeDefs.rend(); ++it)
        if (cm->hasNodeDefinition(*it)) cm->removeNodeDefinition(*it);
    nodeDefs.clear();
}

// ---------------------------------------------------------------------------
// THE PICTURE-IN-PICTURE INSET (CAMERAS_SPEC §7.7), ROUTE C. Design notes and
// the full list of spike findings this obeys live on ViewPipDesc (Types.h);
// what follows is only what is specific to the graph.
//
// WHY ROUTE C AND NOT THE SCENE PASS STRAIGHT INTO THE WINDOW. The inset used
// to render into the window itself, after the main chain had already TONEMAPPED
// it. A view whose chain grades therefore showed a raw linear inset beside a
// graded main image: everything above 1.0 flat white, while the same highlight
// rolled off two centimetres to the left. That is POST_CHAIN_SPEC §14's fourth
// consumer — the same defect the thumbnails, the project tiles and the
// screenshots had, and it gets the same answer: the secondary surface renders
// into a target of its own and goes through the SAME tonemapper, in its
// deterministic fixed-exposure form. It also buys the thing the lens program
// could not have: a PIPPED CAMERA'S OWN EXPOSURE, visible in the inset and
// nowhere else, because the inset's exposure is now a number of its own.
//
// THE GRAPH — one local scene texture, three or five passes:
//
//   1. PASS_CLEAR on a 4x4 swatch (kPipFill), the inset's BACKGROUND colour.
//      A clear's renderArea IS the whole attachment on Vulkan
//      (OgreVulkanRenderPassDescriptor.cpp:945), which is exactly right on a
//      texture of our own and exactly wrong on the window — hence a swatch
//      plus a quad rather than a clear at the rect.
//
//   2. PASS_CLEAR on the 1x1 exposure texture and on the (black) bloom input,
//      when the inset is graded — the two other inputs HDR/FinalToneMapping
//      samples. Both through addFixedExposureClear / the same material the
//      main chain uses; there is exactly one tonemapper in this engine.
//
//   3. PASS_SCENE into the LOCAL TEXTURE (kPipScene):
//        colour CLEAR  — to the inset's background, which is now legal and free:
//                        the target is ours, so a full-target clear is the whole
//                        inset and nothing else;
//        depth  CLEAR  — the spike's correctness finding, now trivially true:
//                        the local texture has its own depth buffer, so the main
//                        view's depth cannot occlude the inset at all;
//        NO VIEWPORT MODIFIER — mViewportModifierMask is cleared. The workspace
//                        modifier places passes at the inset RECT, which is
//                        right for the window and wrong for a texture that IS
//                        the inset: applying it twice would draw the shot into
//                        a third of a texture that is already a third of the
//                        view. This is the one line that carries Route C.
//        shadows OFF   — shadow nodes are per workspace, so a shadowed inset is
//                        a second full shadow set every frame (§7.2);
//        RQ [0, kOverlayRenderQueue) — an inset shows THE SHOT: no gizmos, no
//                        light wires, no grid, and in particular not the camera
//                        body whose selection raised the inset. §7.7 reaches
//                        the same place with a per-pass visibility bit; the RQ
//                        range is the same guarantee with nothing to keep in
//                        step, because every editor helper in this engine is
//                        already defined as "the overlay queue" (OgreMaterials'
//                        renderQueueFor).
//
//   4. TWO QUADS ONTO THE WINDOW, both LOADing colour so the main frame
//      survives: the swatch over the OUTER rect (the background, and the
//      letterbox bars around a constrained shot) and the local texture over the
//      INNER one. Both run the same material — HDR/FinalToneMapping when the
//      inset is graded, Ogre/Copy/4xFP32 when it is not — so the bars cannot
//      disagree with the background inside the shot. Ogre's own low-level
//      materials, not Hlms datablocks: at this pin CompositorPassQuadDef::
//      mMaterialIsHlms routes through RenderQueue::renderSingleObject, whose
//      fillBuffersFor overload BOTH desktop Hlms implementations answer with
//      "Trying to use slow-path on a desktop implementation" (OgreHlmsUnlit.cpp
//      :971 / OgreHlmsPbs.cpp) — an exception per frame and an inset that never
//      appears. The compositor rebinds a quad material's textures on every
//      execute, so three passes sharing one material is designed-for.
//
// The LAST window pass stores colour with kMultiWorkspaceStore: this workspace
// is last, so its resolve is the one that reaches the screen, and it must still
// keep the samples in case anything is ever added after it. Nothing about the
// main view's store/load semantics changes — the inset still only ever LOADs
// the window and never clears it.
void buildPip(Ogre::Root *root, const std::string &workspaceDef, const ViewPipDesc &pip,
              float texWidthFactor, float texHeightFactor,
              std::vector<std::string> &nodeDefsOut, PipHandles &handlesOut) {
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    handlesOut = PipHandles();

    const std::string nodeName = workspaceDef + "/PipNode";
    Ogre::CompositorNodeDef *n = cm->addNodeDefinition(nodeName);
    nodeDefsOut.push_back(nodeName);
    n->addTextureSourceName(kTargetChannel, 0, Ogre::TextureDefinitionBase::TEXTURE_INPUT);

    const bool graded = pip.tonemap;
    n->setNumLocalTextureDefinitions(graded ? 4u : 2u);
    // The swatch. Its FORMAT is deliberately the same plain UNORM the offscreen
    // views use, which is also what makes the copy land on the same value a
    // clear would write: both carry the linear value the caller asked for, and
    // an sRGB window encodes both of them identically on the way in.
    addTex(n, kPipFill, Ogre::PFG_RGBA8_UNORM, 4u, 4u);
    {
        // THE INSET'S SCENE TARGET. Sized by FRACTION of the view's target, and
        // the fraction is the INNER rect's — the composite quad stretches this
        // texture across exactly that rectangle, so any other shape would be a
        // stretch (a square in the world would stop being square in the inset).
        // Fractions rather than pixels because Ogre re-derives them on every
        // target resize: a window drag moves the inset for free, and only a
        // change to the RECT's size re-creates anything (OgreView::applyPip).
        auto *td = addTex(n, kPipScene,
                          graded ? Ogre::PFG_RGBA16_FLOAT : Ogre::PFG_RGBA8_UNORM,
                          0u, 0u, texWidthFactor, texHeightFactor);
        td->depthBufferId = 1u;          // the inset's OWN depth: a scene pass needs one
        td->fsaa = "1";                  // the inset is composited, never resolved
        syncRtvDepth(n, kPipScene, td);
    }
    if (graded) {
        addTex(n, kPipLum,   Ogre::PFG_R16_FLOAT, 1u, 1u);
        // The bloom input the tonemapper samples unconditionally. 4x4 and black:
        // the inset does not bloom (a secondary surface is a photograph of the
        // content, not of the scene's quality tier — §14), and the shader's
        // bilinear read of a flat black texture costs nothing.
        addTex(n, kPipBloom, Ogre::PFG_R10G10B10A2_UNORM, 4u, 4u);
    }
    n->setNumTargetPass(graded ? 6u : 4u);

    {   // The background swatch.
        Ogre::CompositorTargetDef *t = n->addTargetPass(kPipFill);
        t->setNumPasses(1);
        auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
        c->setAllClearColours(toOgre(pip.background));
        c->mStoreActionColour[0] = Ogre::StoreAction::Store;
        c->mStoreActionDepth     = Ogre::StoreAction::DontCare;
        c->mStoreActionStencil   = Ogre::StoreAction::DontCare;
        // A clear pass defaults to vpModifierMask 0x00 / executionMask 0x01
        // (OgreCompositorManager2.h): the mask must stay 0 here or the
        // workspace's inset rectangle would be applied to this 4x4 texture too.
        c->mViewportModifierMask = 0x00;
        c->mProfilingId = "Jahshaka PiP fill swatch";
        handlesOut.fill = c;
    }
    if (graded) {
        handlesOut.exposure = addFixedExposureClear(n, kPipLum, pip.exposure);
        handlesOut.exposure->mProfilingId = "Jahshaka PiP exposure";
        Ogre::CompositorTargetDef *t = n->addTargetPass(kPipBloom);
        t->setNumPasses(1);
        auto *c = static_cast<Ogre::CompositorPassClearDef *>(t->addPass(Ogre::PASS_CLEAR));
        c->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 1.0f));
        c->mViewportModifierMask = 0x00;
        c->mProfilingId = "Jahshaka PiP bloom disabled";
    }
    {   // The inset, into its own texture.
        Ogre::CompositorTargetDef *t = n->addTargetPass(kPipScene);
        t->setNumPasses(1);
        auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
        p->setAllClearColours(toOgre(pip.background));
        p->setAllLoadActions(Ogre::LoadAction::Clear);
        p->mClearDepth = 1.0f;
        p->mStoreActionColour[0] = Ogre::StoreAction::Store;
        p->mStoreActionDepth     = Ogre::StoreAction::DontCare;
        p->mStoreActionStencil   = Ogre::StoreAction::DontCare;
        // THE ROUTE C LINE (see the note above): this pass renders the WHOLE of
        // a texture that is already the size of the inset.
        p->mViewportModifierMask = 0x00;
        p->mFirstRQ = 0u;
        p->mLastRQ  = kOverlayRenderQueue;    // no gizmos, wires, grid or camera bodies
        p->mIncludeOverlays = false;          // see kIncludeOverlaysNote
        p->mShadowNode = Ogre::IdString();    // §7.2: the inset ships shadows OFF
        p->mProfilingId = "Jahshaka PiP scene";
        handlesOut.scenePass = p;
    }
    {
        // TWO TARGET PASSES ON THE WINDOW, not one target pass with two passes:
        // addQuad/addTonemapQuad each open their own (the main chain does the
        // same thing where its composite and its overlay pass both land on the
        // target). They execute in declaration order, which is the order they
        // must paint in.
        const auto onWindow = [](Ogre::CompositorPassDef *q) {
            q->mLoadActionColour[0] = Ogre::LoadAction::Load;   // keep the main frame
            q->mLoadActionDepth     = Ogre::LoadAction::DontCare;
            q->mLoadActionStencil   = Ogre::LoadAction::DontCare;
            q->mStoreActionDepth    = Ogre::StoreAction::DontCare;
            q->mStoreActionStencil  = Ogre::StoreAction::DontCare;
        };
        const auto windowQuad = [&](const char *source, const char *profilingId) {
            Ogre::CompositorPassQuadDef *q;
            if (graded) {
                q = addTonemapQuad(n, kTargetChannel, source, kPipLum, kPipBloom);
            } else {
                q = addQuad(n, kTargetChannel, "Ogre/Copy/4xFP32", profilingId);
                q->addQuadTextureSource(0, source);
            }
            q->mProfilingId = profilingId;
            onWindow(q);
            return q;
        };
        // The background (and the bars), over the OUTER rect...
        windowQuad(kPipFill, "Jahshaka PiP fill")
            ->mStoreActionColour[0] = Ogre::StoreAction::Store;
        // ...then the shot itself, over the INNER one (written live by applyPip).
        handlesOut.composite = windowQuad(kPipScene, graded ? "Jahshaka PiP tonemap"
                                                            : "Jahshaka PiP composite");
        handlesOut.composite->mStoreActionColour[0] = kMultiWorkspaceStore;
    }

    Ogre::CompositorWorkspaceDef *workDef = cm->addWorkspaceDefinition(workspaceDef);
    workDef->connectExternal(0, n->getName(), 0);
}

Ogre::ColourValue fixedExposureColour(float exposure) {
    const float invLum = fixedInverseLuminance(exposure);
    return Ogre::ColourValue(invLum, invLum, invLum, invLum);
}

void destroyPip(Ogre::Root *root, const std::string &workspaceDef,
                std::vector<std::string> &nodeDefs, PipHandles &handles) {
    destroy(root->getCompositorManager2(), workspaceDef, nodeDefs);
    handles = PipHandles();
}

void pipRects(const ViewPipDesc &pip, float targetAspect, float outer[4], float inner[4]) {
    // The requested rect, clamped so a host cannot ask for something outside
    // the target (Ogre would happily set a viewport off the attachment).
    float w = std::max(0.02f, std::min(pip.width, 1.0f));
    float h = std::max(0.02f, std::min(pip.height, 1.0f));
    float l = std::max(0.0f, std::min(pip.left, 1.0f - w));
    float tp = std::max(0.0f, std::min(pip.top, 1.0f - h));
    outer[0] = l; outer[1] = tp; outer[2] = w; outer[3] = h;
    inner[0] = l; inner[1] = tp; inner[2] = w; inner[3] = h;
    if (!pip.camera.constrainAspect || pip.camera.aspect <= 0.0f || targetAspect <= 0.0f) return;

    // LETTERBOX (§7.4). Normalised rects are not pixel rects: the outer rect's
    // PIXEL aspect is (w * targetWidth) / (h * targetHeight), i.e. the
    // normalised ratio times the target's own aspect. Fit the authored aspect
    // inside that, then convert back to normalised units.
    const float outerPixelAspect = (w / h) * targetAspect;
    if (pip.camera.aspect > outerPixelAspect) {
        // Wider than the rect: full width, bars top and bottom.
        const float newH = h * (outerPixelAspect / pip.camera.aspect);
        inner[1] = tp + (h - newH) * 0.5f;
        inner[3] = newH;
    } else {
        const float newW = w * (pip.camera.aspect / outerPixelAspect);
        inner[0] = l + (w - newW) * 0.5f;
        inner[2] = newW;
    }
}

// ---------------------------------------------------------------------------
// Material parameters.
//
// EVERY ONE OF THESE IS PROCESS-GLOBAL (POST_CHAIN_SPEC.md §7.4). Ogre's HDR /
// SSAO / SMAA helpers all write MaterialManager singletons, so exposure, bloom
// threshold and AO tuning are per PROCESS even though the enable flags are per
// scene. The rule the backend follows: the PRIMARY ON-SCREEN VIEW owns the
// globals — OgreEngine::renderOneFrame pushes them from the first enabled
// on-screen view with the chain on, and every other view lives with that.

namespace {

Ogre::Pass *materialPass(const char *name) {
    Ogre::MaterialPtr mat = std::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().load(
            name, Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME));
    if (!mat || !mat->getTechnique(0)) return nullptr;
    return mat->getTechnique(0)->getPass(0);
}

/// Recompiles a material's fragment program with new preprocessor defines,
/// preserving its parameters. This is how Ogre's own samples switch the MSAA
/// sample count and the SMAA preset — a program reload, i.e. a hitch, so every
/// caller here debounces on "did the value actually change".
void recompile(const char *material, const std::string &defines) {
    Ogre::Pass *pass = materialPass(material);
    if (!pass || !pass->hasFragmentProgram()) return;
    Ogre::GpuProgramParametersSharedPtr oldParams = pass->getFragmentProgramParameters();
    Ogre::GpuProgram *shader = pass->getFragmentProgram()->_getBindingDelegate();
    if (!shader) return;
    shader->setParameter("preprocessor_defines", defines);
    pass->getFragmentProgram()->reload();
    pass->getFragmentProgramParameters()->copyConstantsFrom(*oldParams);
}

}   // namespace

void initHdrMsaa(unsigned samples) {
    if (samples <= 1) return;
    std::string defines = "MSAA_INITIALIZED=1,MSAA_SUBSAMPLE_WEIGHT=";
    defines += std::to_string(1.0f / float(samples));
    defines += ",MSAA_NUM_SUBSAMPLES=" + std::to_string(samples);
    recompile("HDR/Resolve_4xFP32_HDR_Box", defines);
}

void setExposure(float exposure, float minAutoExposure, float maxAutoExposure) {
    Ogre::Pass *pass = materialPass("HDR/DownScale03_SumLumEnd");
    if (!pass) return;
    // Verbatim from HdrUtils::setExposure — the shader wants
    // (1024 * e^(exposure-2), 7.5 - max, 7.5 - min), not the stops themselves.
    const Ogre::Vector3 params(1024.0f * std::exp(exposure - 2.0f),
                               7.5f - maxAutoExposure, 7.5f - minAutoExposure);
    pass->getFragmentProgramParameters()->setNamedConstant("exposure", params);
}

void setBloomThreshold(float minThreshold, float fullColourThreshold) {
    Ogre::Pass *pass = materialPass("HDR/BrightPass_Start");
    if (!pass) return;
    if (fullColourThreshold <= minThreshold) fullColourThreshold = minThreshold + 0.01f;
    pass->getFragmentProgramParameters()->setNamedConstant(
        "brightThreshold",
        Ogre::Vector4(minThreshold, 1.0f / (fullColourThreshold - minThreshold), 0.0f, 0.0f));
}

// ---- SSAO -----------------------------------------------------------------
// The stock SSAO/HS material arrives with NOTHING set: no hemisphere kernel, no
// rotation noise, no projection. Ogre's sample builds all three in its game
// state (Tutorial_SSAOGameState.cpp:214-341); this is the same construction,
// done once, the first time a chain asks for SSAO.

namespace {
Ogre::TextureGpu *gSsaoNoise = nullptr;
bool gSsaoInitialised = false;
int  gSmaaPreset = -1;
unsigned gHdrMsaaSamples = 0;

float rangeRandom(float lo, float hi) {
    return lo + (hi - lo) * (float(std::rand()) / float(RAND_MAX));
}
}   // namespace

void initSsao(Ogre::Root *root) {
    if (gSsaoInitialised) return;
    Ogre::Pass *pass = materialPass("SSAO/HS");
    if (!pass) return;

    // 64 hemisphere directions, clustered toward the origin so near-field
    // occlusion dominates. The COUNT is not tunable: SSAO_HS_ps.glsl's tap loop
    // is a compile-time 8x8 double loop, so tiering it would mean forking a
    // stock sample shader (POST_CHAIN_SPEC §4.2).
    float kernel[64][4];
    for (size_t i = 0; i < 64u; ++i) {
        Ogre::Vector3 sample(rangeRandom(-1.0f, 1.0f), rangeRandom(-1.0f, 1.0f),
                             rangeRandom(0.0f, 1.0f));
        sample.normalise();
        float scale = float(i) / 64.0f;
        scale = 0.3f + (1.0f - 0.3f) * (scale * scale);
        sample = sample * scale;
        kernel[i][0] = sample.x; kernel[i][1] = sample.y;
        kernel[i][2] = sample.z; kernel[i][3] = 1.0f;
    }

    // A 2x2 tile of random in-plane rotations, wrapped over the screen.
    Ogre::TextureGpuManager *tm = root->getRenderSystem()->getTextureGpuManager();
    if (!gSsaoNoise) {
        // ManualTexture, AND THAT FLAG IS LOAD-BEARING TWICE. The Ogre sample
        // this construction came from (Tutorial_SSAOGameState.cpp:247) passes
        // `0` for the flags, and a texture with no flags is, to Ogre, a texture
        // that comes FROM A FILE. Two consequences, both of which we shipped:
        //
        //   1. `_transitionTo(Resident)` only calls `notifyDataIsReady()` for a
        //      MANUAL texture (OgreTextureGpu.cpp:597-602). Without the flag it
        //      is never called, so `mDisplayTextureName` stays on the blank
        //      dummy the ctor installed (OgreVulkanTextureGpu.cpp:62) and the
        //      shader sampled a FLAT texture instead of the rotation tile —
        //      SSAO ran with no per-pixel rotation at all. The upload below
        //      landed in `mFinalTextureName` and was never read.
        //   2. Any `unsafeScheduleTransitionTo(Resident)` on a non-manual
        //      texture goes to the LOAD-FROM-FILE path
        //      (OgreTextureGpu.cpp:146-163). We create this one with no
        //      resource group, so that path builds a LoadRequest with a null
        //      archive AND a null loading listener; the worker logs
        //      "ERROR: Did you call createTexture with a valid resourceGroup?"
        //      and then dereferences the null listener
        //      (OgreTextureGpuManager.cpp:2745) — the SIGSEGV in
        //      `_updateTextureMultiLoadWorkerThread` recorded in
        //      SPECS/OGRE_UPSTREAM_ISSUES.md — or, when it does not crash,
        //      leaves a request nothing will ever complete and the frame's
        //      texture wait blocks forever.
        //
        // With the flag, `unsafeScheduleTransitionTo` transitions in place and
        // never touches a worker, which is the same idiom every other
        // hand-filled texture in this engine already uses (OgreSky.cpp:182,
        // OgreMaterials.cpp:911).
        gSsaoNoise = tm->createTexture(processUniqueName("jahSsaoNoise"),
                                       Ogre::GpuPageOutStrategy::SaveToSystemRam,
                                       Ogre::TextureFlags::ManualTexture,
                                       Ogre::TextureTypes::Type2D);
        gSsaoNoise->setResolution(2u, 2u);
        gSsaoNoise->setPixelFormat(Ogre::PFG_RGBA8_SNORM);
        // Immediate transition, and NO explicit notifyDataIsReady(): for a
        // ManualTexture _transitionTo calls it itself, and a second call
        // underflows mDataPreparationsPending (OgreScene::createTexture says
        // the same thing at the same length). _setNextResidencyStatus is
        // redundant for a manual texture — _transitionTo keeps the two in step
        // — and is kept because it is free and it states the intent.
        gSsaoNoise->_transitionTo(Ogre::GpuResidency::Resident, (Ogre::uint8 *)0);
        gSsaoNoise->_setNextResidencyStatus(Ogre::GpuResidency::Resident);

        Ogre::StagingTexture *staging =
            tm->getStagingTexture(2u, 2u, 1u, 1u, Ogre::PFG_RGBA8_SNORM);
        staging->startMapRegion();
        Ogre::TextureBox box = staging->mapRegion(2u, 2u, 1u, 1u, Ogre::PFG_RGBA8_SNORM);
        for (size_t y = 0; y < box.height; ++y) {
            for (size_t x = 0; x < box.width; ++x) {
                Ogre::Vector3 noise(rangeRandom(-1.0f, 1.0f), rangeRandom(-1.0f, 1.0f), 0.0f);
                noise.normalise();
                Ogre::int8 *px = reinterpret_cast<Ogre::int8 *>(box.at(x, y, 0));
                px[0] = Ogre::Bitwise::floatToSnorm8(noise.x);
                px[1] = Ogre::Bitwise::floatToSnorm8(noise.y);
                px[2] = Ogre::Bitwise::floatToSnorm8(noise.z);
                px[3] = Ogre::Bitwise::floatToSnorm8(1.0f);
            }
        }
        staging->stopMapRegion();
        staging->upload(box, gSsaoNoise, 0, 0, 0);
        tm->removeStagingTexture(staging);
    }
    if (Ogre::TextureUnitState *tu = pass->getTextureUnitState("noiseTexture"))
        tu->setTexture(gSsaoNoise);

    Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
    ps->setNamedConstant("invKernelSize", 1.0f / 64.0f);
    ps->setNamedConstant("sampleDirs", (float *)kernel, 64, 4);
    gSsaoInitialised = true;
}

void destroySsao(Ogre::Root *root) {
    // Called from ~OgreEngine: it must not throw and must not depend on the
    // SSAO material still being loadable (resource groups may already be gone).
    Ogre::TextureGpu *noise = gSsaoNoise;
    gSsaoNoise = nullptr;
    gSsaoInitialised = false;
    gSmaaPreset = -1;
    gHdrMsaaSamples = 0;
    if (!noise || !root) return;
    try {
        if (Ogre::Pass *pass = materialPass("SSAO/HS")) {
            if (Ogre::TextureUnitState *tu = pass->getTextureUnitState("noiseTexture"))
                tu->setTexture(nullptr);
        }
    } catch (...) {}
    root->getRenderSystem()->getTextureGpuManager()->destroyTexture(noise);
}

void updateSsao(Ogre::Camera *camera, unsigned aoWidth, unsigned aoHeight,
                float kernelRadius, float powerScale) {
    if (!camera) return;
    Ogre::Pass *pass = materialPass("SSAO/HS");
    if (!pass) return;
    Ogre::Vector2 projAB = camera->getProjectionParamsAB();
    projAB.y /= camera->getFarClipDistance();   // keeps linearDepth in [0,1]
    Ogre::GpuProgramParametersSharedPtr ps = pass->getFragmentProgramParameters();
    ps->setNamedConstant("projectionParams", projAB);
    ps->setNamedConstant("projection", camera->getProjectionMatrix());
    ps->setNamedConstant("kernelRadius", kernelRadius);
    // The noise tile is 2x2 and wraps: the scale is the AO buffer size over it.
    ps->setNamedConstant("noiseScale", Ogre::Vector2(float(aoWidth) / 2.0f,
                                                     float(aoHeight) / 2.0f));
    for (const char *blur : { "SSAO/BlurH", "SSAO/BlurV" }) {
        if (Ogre::Pass *bp = materialPass(blur))
            bp->getFragmentProgramParameters()->setNamedConstant("projectionParams", projAB);
    }
    if (Ogre::Pass *ap = materialPass("SSAO/Apply"))
        ap->getFragmentProgramParameters()->setNamedConstant("powerScale", powerScale);
}

// ---- SMAA -----------------------------------------------------------------
void initSmaa(Ogre::Root *root, int preset) {
    if (preset < 0 || preset == gSmaaPreset) return;
    const Ogre::RenderSystemCapabilities *caps =
        root->getRenderSystem()->getCapabilities();
    std::string defines = "SMAA_INITIALIZED=1,";
    switch (preset) {
    case 0: defines += "SMAA_PRESET_LOW=1,";    break;
    case 1: defines += "SMAA_PRESET_MEDIUM=1,"; break;
    case 2: defines += "SMAA_PRESET_HIGH=1,";   break;
    default: defines += "SMAA_PRESET_ULTRA=1,"; break;
    }
    // Luma, not Colour: cheaper, and EdgeDetectionDepth throws ERR_NOT_IMPLEMENTED
    // upstream (SmaaUtils.cpp:43-45), so only two of the three modes are real.
    defines += "SMAA_EDGE_DETECTION_MODE=1,";
    if (caps->isShaderProfileSupported("glslvk") || caps->isShaderProfileSupported("glsl410"))
        defines += "SMAA_GLSL_4=1,";
    else if (caps->isShaderProfileSupported("glsl330"))
        defines += "SMAA_GLSL_3=1,";

    static const char *materials[] = { "SMAA/EdgeDetection", "SMAA/BlendingWeightCalculation",
                                       "SMAA/NeighborhoodBlending" };
    for (const char *name : materials) {
        Ogre::Pass *pass = materialPass(name);
        if (!pass) continue;
        // BOTH programs: the preset macros drive the vertex shader's edge
        // offsets as well as the fragment thresholds.
        if (pass->hasVertexProgram()) {
            Ogre::GpuProgramParametersSharedPtr old = pass->getVertexProgramParameters();
            if (Ogre::GpuProgram *sh = pass->getVertexProgram()->_getBindingDelegate()) {
                sh->setParameter("preprocessor_defines", defines);
                pass->getVertexProgram()->reload();
                pass->getVertexProgramParameters()->copyConstantsFrom(*old);
            }
        }
        if (pass->hasFragmentProgram()) {
            Ogre::GpuProgramParametersSharedPtr old = pass->getFragmentProgramParameters();
            if (Ogre::GpuProgram *sh = pass->getFragmentProgram()->_getBindingDelegate()) {
                sh->setParameter("preprocessor_defines", defines);
                pass->getFragmentProgram()->reload();
                pass->getFragmentProgramParameters()->copyConstantsFrom(*old);
            }
        }
    }
    gSmaaPreset = preset;
}

// ---- SSR ------------------------------------------------------------------
// The march needs two things per frame that no auto-param provides: the depth
// linearization constants, and a matrix that takes a LEFT-HANDED view-space
// point straight to texture space.
//
// THE MATRIX SURGERY IS VERBATIM FROM OGRE'S OWN HELPER
// (Samples/2.0/Common/src/Utils/ScreenSpaceReflections.cpp:55-72) and every
// line of it earns its place:
//   1. row 2 is averaged with row 3 — the API-independent projection matrix
//      Camera::getProjectionMatrix() returns maps depth to [-1,1]; this maps it
//      to [0,1]. Our shader only reads xy/w, so this is fidelity to the source
//      rather than necessity, and keeping it means the matrix is reusable if a
//      later phase does want depth out of it.
//   2. column 2 is negated — right-handed to left-handed. THIS one is
//      load-bearing: it is what makes w come out as +z for a point in front of
//      the camera, which is the convention the frustum corners
//      (VIEW_SPACE_CORNERS_NORMALIZED_LH) and therefore the whole march use.
//   3. left-multiply by the clip→image matrix — the *0.5+0.5 and the y flip, so
//      the shader divides by w and has a texture coordinate, full stop.
void updateSsr(Ogre::Camera *camera, const ChainDesc &desc) {
    if (!camera || desc.ssr <= 0) return;
    Ogre::Pass *march = materialPass("Jahshaka/SsrRayMarch");
    if (!march) return;

    static const Ogre::Matrix4 kClipToImage(0.5,  0.0, 0.0, 0.5,
                                            0.0, -0.5, 0.0, 0.5,
                                            0.0,  0.0, 1.0, 0.0,
                                            0.0,  0.0, 0.0, 1.0);
    Ogre::Matrix4 m = camera->getProjectionMatrix();
    for (int i = 0; i < 4; ++i) m[2][i] = (m[2][i] + m[3][i]) * 0.5f;   // depth [-1,1] -> [0,1]
    for (int i = 0; i < 4; ++i) m[i][2] = -m[i][2];                     // RH -> LH
    m = kClipToImage * m;

    Ogre::GpuProgramParametersSharedPtr ps = march->getFragmentProgramParameters();
    ps->setNamedConstant("projectionParams", camera->getProjectionParamsAB());
    ps->setNamedConstant("viewToTextureSpaceMatrix", m);
    // The step budget IS the quality row's other half: half-resolution rays get
    // 48 steps, full-resolution rays 96. Both are inside the shader's
    // compile-time loop bound of 128.
    ps->setNamedConstant("rayParams",
                         Ogre::Vector4(desc.ssrMaxDistance, desc.ssrThickness,
                                       desc.ssr >= 2 ? 96.0f : 48.0f,
                                       desc.ssrRoughnessCutoff));

    if (Ogre::Pass *resolve = materialPass("Jahshaka/SsrResolve"))
        resolve->getFragmentProgramParameters()->setNamedConstant(
            "resolveParams",
            Ogre::Vector4(desc.ssrRoughnessCutoff, desc.ssrIntensity, 0.0f, 0.0f));
}

// ---- The per-frame push, in its two halves --------------------------------
// See the declarations in EnginePrivate.h for why the split exists at all.

void applyRecompileGlobals(Ogre::Root *root, const ChainDesc &desc) {
    if (desc.hdr && gHdrMsaaSamples != desc.samples) {
        initHdrMsaa(desc.samples);
        gHdrMsaaSamples = desc.samples;
    }
    if (desc.smaaPreset >= 0) initSmaa(root, desc.smaaPreset);
}

void applyViewGlobals(Ogre::Root *root, Ogre::Camera *camera, const ChainDesc &desc,
                      unsigned viewWidth, unsigned viewHeight) {
    if (desc.hdr) {
        setExposure(desc.exposure, desc.exposureMin, desc.exposureMax);
        if (desc.bloom) setBloomThreshold(desc.bloomThreshold, desc.bloomThreshold + 2.0f);
    }
    if (desc.ssao) {
        // initSsao is idempotent and process-wide (the hemisphere kernel and the
        // rotation-noise tile are camera-independent by construction), so the
        // per-view path may call it: the FIRST view to want AO builds it.
        initSsao(root);
        updateSsao(camera, unsigned(float(viewWidth) * desc.ssaoScale),
                   unsigned(float(viewHeight) * desc.ssaoScale),
                   desc.ssaoRadius, desc.ssaoPower);
    }
    if (desc.ssr > 0) updateSsr(camera, desc);
}

float exposureSeed(float exposure) {
    // The SAME grey-card constant the fixed tonemap uses (fixedInverseLuminance
    // above, whose derivation is written out there). One formula, two callers:
    // the deterministic secondary-surface grade, and the camera-cut re-seed.
    return fixedInverseLuminance(exposure);
}

void ViewGlobalsListener::workspacePreUpdate(Ogre::CompositorWorkspace *) {
    if (!mRoot || !mView) return;
    // THE WHOLE MECHANISM, in three lines. Ogre fires this immediately before
    // THIS workspace's passes execute (CompositorWorkspace::_update), and the
    // material parameters these writes land in are read at pass execute time —
    // so two workspaces in one frame can carry two different exposures even
    // though the materials themselves are process-wide singletons.
    applyViewGlobals(mRoot, mView->camera(), mView->chainDesc(),
                     mView->width(), mView->height());
}

}   // namespace chain
}}}  // namespace jahshaka::engine::detail
