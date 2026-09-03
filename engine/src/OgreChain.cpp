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
#include "EnginePrivate.h"

#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassQuad/OgreCompositorPassQuadDef.h>
#include <Compositor/Pass/PassClear/OgreCompositorPassClearDef.h>
#include <Compositor/Pass/PassMipmap/OgreCompositorPassMipmapDef.h>
#include <Compositor/Pass/PassCompute/OgreCompositorPassComputeDef.h>
#include <Compositor/Pass/PassDepthCopy/OgreCompositorPassDepthCopyDef.h>
#include <Compositor/Pass/PassStencil/OgreCompositorPassStencilDef.h>
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
/// copies still point at — a use-after-free the moment the next pass is added.
/// So it is called EXACTLY ONCE, with a capacity no chain can exceed, before
/// the first addTargetPass. Nothing below may call it again.
constexpr size_t kMaxTargetPasses = 48;

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
           a.ssao == b.ssao && a.ssaoScale == b.ssaoScale &&
           a.smaaPreset == b.smaaPreset && a.ssr == b.ssr &&
           a.refractions == b.refractions && a.samples == b.samples &&
           a.background.r == b.background.r && a.background.g == b.background.g &&
           a.background.b == b.background.b && a.background.a == b.background.a;
}

namespace chain {

// ---------------------------------------------------------------------------
void build(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
           const ChainDesc &desc, std::vector<std::string> &nodeDefsOut) {
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
        Ogre::CompositorTargetDef *t = n->addTargetPass(kTargetChannel);
        t->setNumPasses(2);
        {
            auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
            p->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName) : Ogre::IdString();
            p->setAllClearColours(toOgre(desc.background));
            p->setAllLoadActions(Ogre::LoadAction::Clear);
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
            p->mProfilingId = "Jahshaka opaque";
        }
        {
            auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
            // Load actions keep the compositor defaults (Load everywhere) and
            // colour store keeps StoreOrResolve — this is the last pass on the
            // target, so it is where an MSAA resolve belongs.
            p->mStoreActionDepth   = Ogre::StoreAction::DontCare;
            p->mStoreActionStencil = Ogre::StoreAction::DontCare;
            p->mFirstRQ = kOverlayRenderQueue;
            p->mLastRQ  = 255u;
            p->mProfilingId = "Jahshaka overlays";
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
    n->setNumLocalTextureDefinitions(20);

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
        // keep_content: the 1x1 luminance history is read next frame, so it must
        // NOT be DiscardableContent.
        auto *td = addTex(n, kOldLum, Ogre::PFG_R16_FLOAT, 1u, 1u);
        td->textureFlags = Ogre::TextureFlags::RenderToTexture;
        addTex(n, kLum,      Ogre::PFG_R16_FLOAT, 1u, 1u);
        addTex(n, kLumIter0, Ogre::PFG_R16_FLOAT, 64u, 64u);
        addTex(n, kLumIter1, Ogre::PFG_R16_FLOAT, 16u, 16u);
        addTex(n, kLumIter2, Ogre::PFG_R16_FLOAT, 4u, 4u);
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

    if (desc.ssao) {
        // The main pass gains a SECOND colour attachment. Ogre throws at pass
        // construction if mGenNormalsGBuf is set on an RTV with fewer than two
        // colour attachments (OgreCompositorPassScene.cpp:92-99).
        {
            auto *td = addTex(n, kGBufNormals, Ogre::PFG_R10G10B10A2_UNORM);
            if (msaa) td->fsaa = std::to_string(desc.samples);
            syncRtvDepth(n, kGBufNormals, td);
        }
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
    // attachment for the normals G-buffer.
    if (namedDepth) {
        Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(kSceneRtv);
        Ogre::RenderTargetViewEntry colour0;
        colour0.textureName = kRt0;
        rtv->colourAttachments.push_back(colour0);
        if (desc.ssao) {
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
    if (desc.hdr) {
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
    }

    // The opaque scene pass.
    {
        const char *sceneTarget = namedDepth ? kSceneRtv : kRt0;
        Ogre::CompositorTargetDef *t = n->addTargetPass(sceneTarget);
        t->setNumPasses(1);
        auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
        p->mShadowNode = desc.shadows ? Ogre::IdString(OgreView::kShadowNodeName) : Ogre::IdString();
        p->setAllClearColours(toOgre(desc.background));
        p->setAllLoadActions(Ogre::LoadAction::Clear);
        // With refractions the opaque result must exist BOTH as multisample
        // (the refractive pass keeps rendering into a clone of it) and resolved
        // (that same pass samples it) — the sample's "store_and_resolve".
        p->mStoreActionColour[0] = (desc.refractions && msaa)
                                       ? Ogre::StoreAction::StoreAndMultisampleResolve
                                       : Ogre::StoreAction::Store;
        if (desc.ssao) p->mStoreActionColour[1] = Ogre::StoreAction::Store;
        // Depth survives the pass: SSAO marches it, refraction copies it, and
        // the refractive pass depth-tests against it.
        p->mStoreActionDepth   = (desc.ssao || desc.ssr || desc.refractions)
                                     ? Ogre::StoreAction::Store : Ogre::StoreAction::DontCare;
        p->mStoreActionStencil = Ogre::StoreAction::DontCare;
        p->mGenNormalsGBuf = desc.ssao;
        p->mFirstRQ = 0u;
        // Stop before the refractive queue only when there IS a refraction pass
        // to pick those items up; otherwise they render here, as plain glass.
        p->mLastRQ  = desc.refractions ? kRefractiveRenderQueue : kOverlayRenderQueue;
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
            p->mProfilingId = "Jahshaka refractives";
        }
        sceneResult = kRefractOut;
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

    // HDR: luminance reduction, bloom, tonemap.
    if (desc.hdr) {
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
            auto *q = addQuad(n, ldrTarget, "HDR/FinalToneMapping", "Jahshaka HDR tonemap");
            q->addQuadTextureSource(0, sceneResult);
            q->addQuadTextureSource(1, kLum);
            q->addQuadTextureSource(2, kBlur0);
            q->mStoreActionColour[0] = Ogre::StoreAction::Store;
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
        p->mStoreActionDepth    = Ogre::StoreAction::DontCare;
        p->mStoreActionStencil  = Ogre::StoreAction::DontCare;
        p->mFirstRQ = kOverlayRenderQueue;
        p->mLastRQ  = 255u;
        p->mProfilingId = "Jahshaka overlays";
    }

    Ogre::CompositorWorkspaceDef *workDef = cm->addWorkspaceDefinition(workspaceDef);
    workDef->connectExternal(0, n->getName(), 0);
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
        gSsaoNoise = tm->createTexture(processUniqueName("jahSsaoNoise"),
                                       Ogre::GpuPageOutStrategy::SaveToSystemRam, 0,
                                       Ogre::TextureTypes::Type2D);
        gSsaoNoise->setResolution(2u, 2u);
        gSsaoNoise->setPixelFormat(Ogre::PFG_RGBA8_SNORM);
        // Immediate transition, and NO notifyDataIsReady(): _transitionTo calls
        // it itself, and a second call underflows mDataPreparationsPending.
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

// ---- The per-frame push ---------------------------------------------------
void applyGlobals(Ogre::Root *root, Ogre::Camera *camera, const ChainDesc &desc,
                  unsigned viewWidth, unsigned viewHeight) {
    if (desc.hdr) {
        if (gHdrMsaaSamples != desc.samples) {
            initHdrMsaa(desc.samples);
            gHdrMsaaSamples = desc.samples;
        }
        setExposure(desc.exposure, desc.exposureMin, desc.exposureMax);
        if (desc.bloom) setBloomThreshold(desc.bloomThreshold, desc.bloomThreshold + 2.0f);
    }
    if (desc.ssao) {
        initSsao(root);
        updateSsao(camera, unsigned(float(viewWidth) * desc.ssaoScale),
                   unsigned(float(viewHeight) * desc.ssaoScale),
                   desc.ssaoRadius, desc.ssaoPower);
    }
    if (desc.smaaPreset >= 0) initSmaa(root, desc.smaaPreset);
}

}   // namespace chain
}}}  // namespace jahshaka::engine::detail
