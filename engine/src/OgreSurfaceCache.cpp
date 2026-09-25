// SURFACE-CACHE phase 2 — the capture Component's implementation.
// The design, the two condemned shapes it replaces and the renderArea evidence
// behind the scratch-and-copy route are all in SurfaceCache.h; this file is the
// mechanism.
#include "EnginePrivate.h"
#include "SurfaceCache.h"

#include <OgreCamera.h>
#include <OgreItem.h>
#include <OgreLight.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include <Vct/OgreVctLighting.h>
#include <Vct/OgreVctVoxelizerSourceBase.h>
#include <OgreDescriptorSetTexture.h>
#include <OgreDescriptorSetUav.h>
#include <OgreMesh2.h>
#include <OgreLogManager.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSubItem.h>
#include <OgreTextureGpuManager.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreImage2.h>
#include <OgrePixelFormatGpuUtils.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreUavBufferPacked.h>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorNode.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorWorkspaceDef.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/OgreCompositorPassDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace jahshaka {
namespace engine {

using detail::processUniqueName;

namespace {

/// THE CAPTURE'S PASS PROPERTY, and the whole of "is a capture running".
/// Render-thread only, exactly like `FogHlmsListener`'s own maps: set by the
/// capture workspace's `workspacePreUpdate` and cleared by its
/// `workspacePosUpdate` — AND cleared unconditionally at every frame's head
/// (`allWorkspacesBeforeBeginUpdate`), which is not tidiness. A capture
/// compiles a shader permutation and an Hlms compile failure THROWS through
/// the workspace's `_update()`; a flag left set by that throw would make every
/// later pass generate the capture permutation, whose `custom_ps_posExecution`
/// collides at library-parse time with the fog piece's definition of the same
/// name — the "already defined" parse error whose only symptom is the whole
/// scene rendering BLACK with nothing in any log.
bool gCapturing = false;

/// THE SIX AXIS FRAMES, read from THE CONTRACT (Types.h's cardAxisDirection /
/// cardAxisU / cardAxisV — the one table the bake, the capture and phase 4's
/// read all parameterise the same rectangle with). Cached as Ogre vectors
/// because every card build multiplies them by a quaternion.
Ogre::Vector3 kAxis[6], kAxisU[6], kAxisV[6];
struct CardFrameInit {
    CardFrameInit() {
        for (unsigned a = 0; a < 6u; ++a) {
            kAxis[a] = detail::toOgre(cardAxisDirection(a));
            kAxisU[a] = detail::toOgre(cardAxisU(a));
            kAxisV[a] = detail::toOgre(cardAxisV(a));
        }
    }
};
const CardFrameInit gCardFrames;

/// TEXELS PER METRE OF CARD. The one number that decides a card's resolution,
/// and it is not invented: the assessment's §3 states a 2 m crate's card texel
/// at ~1.6 cm and a 10 m wall's at ~8 cm, and 64 texels a metre with the
/// 128-texel page ceiling reproduces both exactly (2 m -> 128 texels -> 1.56 cm;
/// 10 m -> 640, split into five pages of 128 -> 7.8 cm).
constexpr float kCardTexelsPerMetre = 64.0f;
/// The most pages ONE card may be split into per axis. A 32 m wall is 4 pages
/// wide at the density above; past that the texel simply gets coarser rather
/// than the card eating the atlas.
constexpr unsigned kCardMaxSplit = 4u;
/// THE CARD TABLE'S CEILING. 256 pages can hold at most 256 full-page cards and
/// many more sub-allocated ones; this is the number of RECORDS the GPU table is
/// sized for, allocated once so the buffer never churns (the 0071 lesson is
/// about textures, but a buffer recreated on every residency change is the same
/// shape of mistake).
constexpr unsigned kCardRecordCeiling = 4096u;
/// ...and the instance table's, indexed by the item slot. It grows in powers of
/// two when a scene turns out to have more items than this.
constexpr unsigned kInstanceSlotsInitial = 1024u;

/// THE ATTACHMENT ORDER IS THE SHADER'S, NOT THE ATLAS'S, and confusing the two
/// is a silent swap of four channels (this lane made it once: the albedo layer
/// came back holding the normal). `Ogre::PrePassCreate` writes location 0 = the
/// shading normal and location 1 = (shadow, GGX alpha); `JahCardCapture_piece_ps.any`
/// appends albedo, emissive and depth at locations 2, 3 and 4 through
/// `custom_ps_output_types`, in that order, because `@counter(rtv_target)`
/// counts them in the order they are declared. So the render target view's
/// colour attachments must be in exactly this order, whatever order the atlas
/// layers are declared in.
const CardLayer kShaderOrder[kCardLayers] = { CardLayer::Normal, CardLayer::ShadowRough,
                                              CardLayer::Albedo, CardLayer::Emissive,
                                              CardLayer::Depth };

/// The capture camera's standoff from the card's own near plane, so the near
/// plane is never inside the surface it captures.
float captureMargin(float halfDepth) { return 0.02f * std::max(halfDepth, 0.01f) + 0.005f; }

unsigned pow2Ceil(unsigned v) {
    unsigned p = 1u;
    while (p < v) p <<= 1;
    return p;
}

unsigned long long bytesOf(const Ogre::TextureGpu *t) {
    if (!t) return 0ull;
    return static_cast<unsigned long long>(t->getWidth()) * t->getHeight() * t->getDepth() *
           t->getNumSlices() *
           Ogre::PixelFormatGpuUtils::getBytesPerPixel(t->getPixelFormat());
}

/// READING A PACKED-FLOAT TEXEL BACK ON THE CPU — because the pin cannot.
///
/// `PixelFormatGpuUtils::unpackColour` THROWS `UnimplementedException` for both
/// four-byte HDR formats (`OgrePixelFormatGpuUtils.cpp:975` for R11G11B10F,
/// `:1082` for RGB9E5) — they are write-only as far as Ogre's CPU colour
/// helpers are concerned — so `TextureBox::getColourAt` cannot be used on the
/// emissive layer at all, whichever of the two the device gave us. That is a
/// gap in the PIN's helpers and not a defect in the format: both encodings are
/// four lines of arithmetic, and a readback that exists only for tests and
/// tools has no business forcing an 8-byte layer on every user's atlas to
/// avoid them.
void decodeTexel(Ogre::PixelFormatGpu fmt, const void *src, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (fmt == Ogre::PFG_R11G11B10_FLOAT) {
        // VK_FORMAT_B10G11R11_UFLOAT_PACK32: R in bits 0-10 (5 exp, 6 mantissa),
        // G in 11-21 (5 exp, 6 mantissa), B in 22-31 (5 exp, 5 mantissa); no
        // sign bit, exponent bias 15, an implicit leading 1 above exponent 0.
        const Ogre::uint32 v = *static_cast<const Ogre::uint32 *>(src);
        const auto small = [](Ogre::uint32 bits, unsigned mantissaBits) {
            const Ogre::uint32 mantissa = bits & ((1u << mantissaBits) - 1u);
            const Ogre::uint32 exponent = bits >> mantissaBits;
            const float scale = 1.0f / float(1u << mantissaBits);
            if (exponent == 0u) return float(mantissa) * scale * std::ldexp(1.0f, -14);
            if (exponent == 31u) return mantissa ? std::numeric_limits<float>::quiet_NaN()
                                                 : std::numeric_limits<float>::infinity();
            return (1.0f + float(mantissa) * scale) * std::ldexp(1.0f, int(exponent) - 15);
        };
        out[0] = small(v & 0x7FFu, 6u);
        out[1] = small((v >> 11u) & 0x7FFu, 6u);
        out[2] = small((v >> 22u) & 0x3FFu, 5u);
        return;
    }
    if (fmt == Ogre::PFG_R9G9B9E5_SHAREDEXP) {
        // VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: three 9-bit mantissas over one
        // 5-bit exponent, bias 15, no implicit leading 1.
        const Ogre::uint32 v = *static_cast<const Ogre::uint32 *>(src);
        const float scale = std::ldexp(1.0f, int(v >> 27u) - 15 - 9);
        out[0] = float(v & 0x1FFu) * scale;
        out[1] = float((v >> 9u) & 0x1FFu) * scale;
        out[2] = float((v >> 18u) & 0x1FFu) * scale;
        return;
    }
    Ogre::PixelFormatGpuUtils::unpackColour(out, fmt, src);
}

/// ONE CARD AS THE RAY JOB READS IT (PHOTON-CARDS-2; rayquery/include/jah_rq_card.glsl's
/// JahCardRecord). std430, 80 bytes: the world frame's three rows, the axis and
/// the texel, and the atlas rect with the card's two flags. Built here because
/// the layout is a contract between this Component and the ray job, and a
/// contract written in two places is a contract that drifts.
///
/// THE RECT IS IN TEXELS, not in UV: the read is texel-exact (the atlas has no
/// mips) and picks the texel by the same integer rule `sampleCard` does, so
/// gi.card_read_parity can ask for the same texel on both sides.
struct CardGpuRec {
    float rowU[4] = {};          ///< dot(world, xyz) + w  ->  the card's u in [0,1]
    float rowV[4] = {};
    float rowD[4] = {};          ///< ...and the distance from the card's near plane
    float axis[4] = {};          ///< xyz = the outward world axis, w = the card's texel, metres
    /// x, y = the rect's corner in the atlas, z = its size in texels, w = the
    /// flags: kGpuCardCaptured (captured at least once), kGpuCardLit (its
    /// radiance carries a marched indirect half — `indirectValid`).
    unsigned atlas[4] = {};
};
static_assert(sizeof(CardGpuRec) == 80u, "the card record's layout is a contract with the ray job");
constexpr unsigned kGpuCardCaptured = 1u;
constexpr unsigned kGpuCardLit = 2u;

}   // namespace

bool SurfaceCache::capturing() { return gCapturing; }

SurfaceCache::SurfaceCache() : mEmissiveFormat(Ogre::PFG_R9G9B9E5_SHAREDEXP) {}

SurfaceCache::~SurfaceCache() { destroyAll(); }

// ---------------------------------------------------------------------------
// The atlas
// ---------------------------------------------------------------------------
bool SurfaceCache::makeAtlas(std::string &err) {
    Ogre::TextureGpuManager *tm =
        Ogre::Root::getSingleton().getRenderSystem()->getTextureGpuManager();

    // THE EMISSIVE FORMAT IS CHOSEN FROM WHAT THE DEVICE CAN RENDER TO, not
    // asserted. SURFACE-CACHE-0's card texel was 22 bytes — RGBA8 normal (4) +
    // RG16 shadow/roughness (4) + RGBA8 albedo (4) + RGBA16F emissive (8) +
    // R16F depth (2) — against Lumen's 5.25 compressed, and its report named
    // the emissive as the single biggest item. RGB9E5 halves it to 4 bytes with
    // no loss that matters for a radiance channel (a shared 5-bit exponent over
    // three 9-bit mantissas), and RG8 halves the shadow/roughness pair: 22 -> 16
    // bytes before any BC encode, which is the number this phase was told to
    // reach.
    //
    // BUT RGB9E5 IS NOT A MANDATORY COLOUR-ATTACHMENT FORMAT IN VULKAN — only
    // SAMPLED and SAMPLED_FILTER_LINEAR are mandatory for it — and the capture
    // writes it from a render pass, so the choice is asked rather than assumed
    // and the answer is published in `CardCacheStatus::emissiveFormat`.
    // R11G11B10F is the fallback and costs the same four bytes.
    mEmissiveFormat = Ogre::PFG_R9G9B9E5_SHAREDEXP;
    if (!tm->checkSupport(mEmissiveFormat, Ogre::TextureTypes::Type2D,
                          Ogre::TextureFlags::RenderToTexture)) {
        mEmissiveFormat = Ogre::PFG_R11G11B10_FLOAT;
        mEmissiveFormatName = "R11G11B10F";
    } else {
        mEmissiveFormatName = "RGB9E5";
    }

    const Ogre::PixelFormatGpu fmt[kCardLayers] = {
        Ogre::PFG_RGBA8_UNORM,   // albedo (kD)
        Ogre::PFG_RGBA8_UNORM,   // normal
        Ogre::PFG_R16_FLOAT,     // depth
        mEmissiveFormat,         // emissive
        Ogre::PFG_RG8_UNORM      // shadow + roughness
    };
    const char *name[kCardLayers] = { "cardAlbedo", "cardNormal", "cardDepth", "cardEmissive",
                                      "cardShadowRough" };

    for (unsigned i = 0; i < kCardLayers; ++i) {
        // THE ATLAS IS NEVER A RENDER TARGET. It is written by COPY (see the
        // header: this pin's Vulkan renderArea is the whole attachment, so a
        // page-scissored clear does not exist) and read by a sampler, and
        // `TextureFlags::ManualTexture` is what says "we fill this ourselves" —
        // the rule CLAUDE.md records as the SSAO noise texture's week-long
        // defect (flags 0 = load me from a resource group = a worker null
        // dereference). Vulkan gives every texture TRANSFER_SRC|TRANSFER_DST
        // unconditionally, so a manual texture is a valid copy destination.
        Ogre::TextureGpu *t =
            tm->createTexture(processUniqueName(name[i]), Ogre::GpuPageOutStrategy::Discard,
                              Ogre::TextureFlags::ManualTexture, Ogre::TextureTypes::Type2D);
        t->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
        t->setPixelFormat(fmt[i]);
        t->setNumMipmaps(1u);
        // RESIDENT, AND RESIDENT FOR GOOD (the 0071 lesson): a large target
        // created and destroyed around each capture is the residency churn that
        // cost this engine an Xid.
        t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        mAtlas[i] = t;

        // ...and the page-sized SCRATCH the capture actually renders into, in
        // the SAME format so the copy is a straight vkCmdCopyImage (a copy
        // cannot convert, which is why the fallback above moves both). ONE
        // SLICE PER PASS OF THE BATCH: pass i renders into slice i, so a
        // workspace update carries `kCaptureBatch` cards and each pass's clear
        // is exact — the attachment is the slice's own view, so the
        // whole-attachment `renderArea` (SurfaceCache.h) clears that slice and
        // nothing a sibling pass wrote.
        Ogre::TextureGpu *s =
            tm->createTexture(processUniqueName((std::string(name[i]) + "Scr").c_str()),
                              Ogre::GpuPageOutStrategy::Discard,
                              Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2DArray);
        s->setResolution(kCardPageSize, kCardPageSize, kCaptureBatch);
        s->setPixelFormat(fmt[i]);
        s->setNumMipmaps(1u);
        s->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        mScratch[i] = s;
    }
    mScratchDepth =
        tm->createTexture(processUniqueName("cardDepthBuf"), Ogre::GpuPageOutStrategy::Discard,
                          Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2D);
    mScratchDepth->setResolution(kCardPageSize, kCardPageSize, 1u);
    mScratchDepth->setPixelFormat(Ogre::PFG_D32_FLOAT);
    mScratchDepth->setNumMipmaps(1u);
    mScratchDepth->_transitionTo(Ogre::GpuResidency::Resident, nullptr);

    // THE SIXTH LAYER, RADIANCE (PHOTON-CARDS-1, SC-1c): what a hit will read.
    // Written by the `Jahshaka/CardLight` compute job and by nothing else, so it
    // is a UAV — never a render target, never a copy destination. R11G11B10F is
    // four bytes of unsigned HDR (radiance is never negative) when the device
    // can STORE to it from a compute job; RGBA16F otherwise, and the status
    // says which. +16 MB at 2048 square.
    mRadianceFormatName = "R11G11B10F";
    Ogre::PixelFormatGpu radFmt = Ogre::PFG_R11G11B10_FLOAT;
    if (!tm->checkSupport(radFmt, Ogre::TextureTypes::Type2D, Ogre::TextureFlags::Uav)) {
        radFmt = Ogre::PFG_RGBA16_FLOAT;
        mRadianceFormatName = "RGBA16F";
    }
    mRadiance = tm->createTexture(processUniqueName("cardRadiance"), Ogre::GpuPageOutStrategy::Discard,
                                  Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mRadiance->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mRadiance->setPixelFormat(radFmt);
    mRadiance->setNumMipmaps(1u);
    mRadiance->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE CACHED INDIRECT HALF, the march's answer per texel (Lumen's
    // IndirectLighting atlas): the expensive half is recomputed only when the
    // chain re-injects, and a direct-only relight reads it back. Same format,
    // read and written by the same job. +16 MB.
    mIndirect = tm->createTexture(processUniqueName("cardIndirect"), Ogre::GpuPageOutStrategy::Discard,
                                  Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mIndirect->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mIndirect->setPixelFormat(radFmt);
    mIndirect->setNumMipmaps(1u);
    mIndirect->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE MOVERS' VISIBILITY (PHOTON-CARDS-4): R8 over the atlas, a UAV
    // the ray tier's trace writes and the relight reads (only inside the cards
    // it flags `moverTraced`, so a texel no trace wrote is never read). +4 MB.
    mMoverVis = tm->createTexture(processUniqueName("cardMoverVis"), Ogre::GpuPageOutStrategy::Discard,
                                  Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mMoverVis->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mMoverVis->setPixelFormat(Ogre::PFG_R8_UNORM);
    mMoverVis->setNumMipmaps(1u);
    mMoverVis->_transitionTo(Ogre::GpuResidency::Resident, nullptr);

    const unsigned pagesPerSide = kCardAtlasSize / kCardPageSize;
    const size_t pages = size_t(pagesPerSide) * pagesPerSide;
    mPageOwner.assign(pages, 0u);
    mPageSubSize.assign(pages, 0u);
    mPageSubUsed.assign(pages, 0u);
    mPageSubMask.assign(pages, 0ull);
    (void)err;
    return true;
}

bool SurfaceCache::makeWorkspace(std::string &err) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    Ogre::SceneManager *sm = mSceneMgr;
    if (!sm) { err = "surface cache: the scene has no SceneManager"; return false; }

    // ONE CAPTURE CAMERA PER PASS OF THE BATCH, living in the scene being
    // cached (a camera is a node in the manager's graph, so there are
    // `kCaptureBatch` of them and not one per card). Pass i is bound to camera
    // i for the life of the workspace, and the camera is re-aimed at its card
    // each frame. A camera of its own per pass is also what gives every card
    // its OWN shadow fit: the pin's shadow node caches its light list and its
    // casters box per (camera, frame) (`buildClosestLightList`'s early-out,
    // OgreCompositorShadowNode.cpp:345), and no two passes of one update share
    // a camera.
    //
    // NOT LIGHT-COLLECTING CAMERAS (`isVisible` false). A visible camera is in
    // the frame's light cull (SceneManager::buildLightList): these were made
    // "cubemap" cameras, culled by a box of half their far clip — and one no
    // batch has used yet sits at the origin with Ogre's 100,000 far clip, a box
    // that put EVERY light of the scene in the frame's global list for as long
    // as the card row was on (measured, PHOTON-CARDS-1 round 2). A capture
    // lights nothing: its one light is the sun's PSSM term, and directional
    // lights enter the list unculled.
    for (unsigned i = 0; i < kCaptureBatch; ++i) {
        mCam[i] = sm->createCamera(processUniqueName(("cardCapture" + std::to_string(i)).c_str()),
                                   false, false);
        mCam[i]->setProjectionType(Ogre::PT_ORTHOGRAPHIC);
        mCam[i]->setFixedYawAxis(false);
        mCam[i]->setAutoAspectRatio(false);
    }
    mNodeDef = processUniqueName("JahCardCaptureNode");
    mWsDef = processUniqueName("JahCardCaptureWs");
    Ogre::CompositorNodeDef *n = cm->addNodeDefinition(mNodeDef);
    // ONE EXTERNAL PER ATTACHMENT, IN THE SHADER'S ORDER (kShaderOrder above).
    const char *chan[kCardLayers + 1] = { "scr0Normal", "scr1ShadowRough", "scr2Albedo",
                                          "scr3Emissive", "scr4Depth", "scrDepthBuf" };
    for (unsigned i = 0; i < kCardLayers + 1u; ++i)
        n->addTextureSourceName(chan[i], i, Ogre::TextureDefinitionBase::TEXTURE_INPUT);

    // The same shadow node every pass names; a CompositorShadowNode is per
    // WORKSPACE, so the batch shares one instance and each pass re-fits it.
    const bool haveShadowNode =
        cm->hasShadowNodeDefinition(detail::OgreView::kCardShadowNodeName);
    static const bool debugLog = std::getenv("JAHSHAKA_GI_DEBUG") != nullptr;
    if (debugLog)
        Ogre::LogManager::getSingleton().logMessage(
            std::string("Jahshaka cards: capture shadow node ") +
            (haveShadowNode ? detail::OgreView::kCardShadowNodeName : "NONE (a card's shadow"
                                                                      " term will be the"
                                                                      " prepass constant)"));

    n->setNumTargetPass(kCaptureBatch);
    for (unsigned b = 0; b < kCaptureBatch; ++b) {
        // PASS b RENDERS INTO SLICE b of every scratch layer.
        const std::string rtvName = "cardRtv" + std::to_string(b);
        Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView(rtvName);
        for (unsigned i = 0; i < kCardLayers; ++i) {
            Ogre::RenderTargetViewEntry e;
            e.textureName = chan[i];
            e.slice = static_cast<Ogre::uint16>(b);
            rtv->colourAttachments.push_back(e);
        }
        rtv->depthAttachment.textureName = chan[kCardLayers];
        rtv->stencilAttachment.textureName = chan[kCardLayers];
        rtv->preferDepthTexture = true;

        Ogre::CompositorTargetDef *t = n->addTargetPass(rtvName);
        t->setNumPasses(1u);
        auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
        mPassDef[b] = p;
        // THE PREPASS IS THE CAPTURE. `Ogre::PrePassCreate` writes the shading
        // normal and (the shadow term, the GGX alpha) exactly, computes no
        // lighting at all — which is what makes a capture cheap — and
        // JahCardCapture_piece_ps.any adds albedo, emissive and the card's own
        // depth through three hook pieces under one pass property.
        p->mPrePassMode = Ogre::PrePassCreate;
        p->mCameraName = Ogre::IdString(mCam[b]->getName());
        // WHICH SLOT OF THE BATCH THIS PASS IS — how the per-pass listener
        // (`passPreExecute`) finds the card whose subject it grants.
        p->mIdentifier = kCardPassIdentifier + b;
        // THE BATCH'S GATE: pass b runs iff bit b of the workspace's execution
        // mask is set, and `update()` sets exactly the low N bits for an
        // N-card batch. Ogre's mask is a uint8, which is why kCaptureBatch is 8.
        p->mExecutionMask = static_cast<Ogre::uint8>(1u << b);
        // ONE OBJECT, THROUGH AN INCLUDE CHANNEL. Ogre's visibility test is
        // any-bit-set, so "draw only this item" is expressible only as a bit the
        // item alone carries while the pass runs — `kCardSubjectBit`, granted in
        // passPreExecute and taken away in passPosExecute. The mask is a single
        // LOW bit, so `cullFrustum`'s second term (`viewportMask &
        // ~RESERVED_VISIBILITY_FLAGS`) is zero and nothing leaks through it.
        //
        // AND A LIGHT MUST STILL REACH THIS PASS: `buildClosestLightList` culls
        // the pass's lights through the VIEWPORT's visibility mask, which is this
        // one — every light in this engine is born with Ogre's all-bits default
        // and nothing narrows it. The day something does, a light without bit 10
        // drops out of every capture and EVERY CARD'S SHADOW TERM GOES TO 1.0.
        p->mVisibilityMask = detail::kCardSubjectBit;
        // THE SHADOW NODE: the card node (OgreView::kCardShadowNodeName — the
        // sun's PSSM at the probe resolution and nothing else, because the
        // prepass writes the directional term alone; the STILL world as its
        // casters — the captured half of a card's sun term, the movers' half is
        // traced: traceMovers), RECALCULATED PER PASS:
        // every pass has its own camera, so every card gets its own fit.
        if (haveShadowNode) {
            p->mShadowNode = Ogre::IdString(detail::OgreView::kCardShadowNodeName);
            p->mShadowNodeRecalculation = Ogre::SHADOW_NODE_RECALCULATE;
        }
        // NO FORWARD+ FOR A PREPASS: the capture computes no lighting, and a
        // clustered light grid is built per CAMERA — eight of them a frame for
        // nothing.
        p->mEnableForwardPlus = false;
        p->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f));
        p->setAllLoadActions(Ogre::LoadAction::Clear);
        for (unsigned i = 0; i < kCardLayers; ++i)
            p->mStoreActionColour[i] = Ogre::StoreAction::Store;
        p->mStoreActionDepth = Ogre::StoreAction::DontCare;
        p->mStoreActionStencil = Ogre::StoreAction::DontCare;
        p->mFirstRQ = 0u;
        p->mLastRQ = 200u;
        p->mIncludeOverlays = false;
        // THE LOD LISTS ARE NOT RE-DERIVED BY THIS PASS. The level a card is
        // captured at is the card's OWN (`MeshCardDesc::lodLevel`), written
        // straight onto the Item through patch 0085's setter in passPreExecute —
        // a pass that recomputed LOD from this ortho camera would undo it.
        p->mUpdateLodLists = false;
        p->mProfilingId = "Jahshaka card capture";
    }

    Ogre::CompositorWorkspaceDef *wd = cm->addWorkspaceDefinition(mWsDef);
    for (unsigned i = 0; i < kCardLayers + 1u; ++i) wd->connectExternal(i, mNodeDef, i);
    Ogre::CompositorChannelVec externals;
    for (unsigned i = 0; i < kCardLayers; ++i)
        externals.push_back(mScratch[unsigned(kShaderOrder[i])]);
    externals.push_back(mScratchDepth);
    // THE CAPTURE RUNS INSIDE OGRE'S OWN FRAME, AND THAT IS THE SHADOW FIX
    // (PHOTON-CARDS-1 §1.1, measured). This workspace used to be driven BY HAND
    // from the engine's pre-frame hook — BEFORE `Root::renderOneFrame` runs the
    // frame's `updateSceneGraph()` and after the previous frame's
    // `clearFrameData()` emptied the manager's GLOBAL LIGHT LIST. So every
    // capture but those of a frame that happened to follow a hand-driven graph
    // update (a GI rebuild's) saw ZERO lights: no shadow-casting light, no
    // shadow map, a flat 1.0. Measured on `gi.card_shadow`'s fixture at the High
    // budget: 93 of 96 captures read an empty light list, and the three that
    // did not were exactly the first frame's. (The ortho-PSSM hypothesis was
    // measured too — a focused technique, a perspective cull camera, a wide cull
    // camera: all flat 1.0, because there was no light to fit.)
    //
    // So the workspace is ENABLED and sits FIRST in the manager's list: it
    // runs after the frame's scene-graph update and light list, before the
    // views, every frame — and does nothing on a frame that planned no batch,
    // because its execution mask is then zero (`update()` sets it; the
    // listener clears it after the copies). A frame whose scene is not drawn
    // never calls `update()`, so it captures nothing.
    mWs = cm->addWorkspace(sm, externals, mCam[0], mWsDef, true, 0);
    if (!mWs) { err = "surface cache: addWorkspace failed"; return false; }
    mWs->setExecutionMask(0u);
    mWs->addListener(this);
    // ...AND THE MANAGER'S FRAME HEAD, where the capture flag is cleared
    // unconditionally: a capture that threw inside `_update` never reaches its
    // `workspacePosUpdate`, and a flag left set is not a lost capture but a
    // broken process (every later pass would generate the capture permutation).
    cm->addListener(this);
    return true;
}

bool SurfaceCache::build(Ogre::SceneManager *sceneMgr, std::string &err) {
    if (mBuilt) return true;
    if (!sceneMgr) { err = "surface cache: no SceneManager"; return false; }
    mSceneMgr = sceneMgr;
    if (!makeAtlas(err)) { destroyAll(); return false; }
    if (!makeWorkspace(err)) { destroyAll(); return false; }
    mBuilt = true;
    return true;
}

void SurfaceCache::destroyAll() {
    mBuilt = false;
    mInstances.clear();
    mCards.clear();
    mByNode.clear();
    mQueue.clear();
    mPageOwner.clear();
    mPageSubSize.clear();
    mPageSubUsed.clear();
    mPageSubMask.clear();
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return;
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    if (mWs) {
        cm->removeListener(this);
        cm->removeWorkspace(mWs);
        mWs = nullptr;
    }
    if (!mWsDef.empty() && cm->hasWorkspaceDefinition(mWsDef)) cm->removeWorkspaceDefinition(mWsDef);
    if (!mNodeDef.empty() && cm->hasNodeDefinition(mNodeDef)) cm->removeNodeDefinition(mNodeDef);
    mWsDef.clear();
    mNodeDef.clear();
    if (Ogre::VaoManager *vao = root->getRenderSystem()
                                    ? root->getRenderSystem()->getVaoManager()
                                    : nullptr) {
        if (mCardBuffer) { vao->destroyUavBuffer(mCardBuffer); mCardBuffer = nullptr; }
        if (mInstanceBuffer) { vao->destroyUavBuffer(mInstanceBuffer); mInstanceBuffer = nullptr; }
        if (mRelightBuffer) { vao->destroyUavBuffer(mRelightBuffer); mRelightBuffer = nullptr; }
        if (mLightBuffer) { vao->destroyUavBuffer(mLightBuffer); mLightBuffer = nullptr; }
        if (mGiBuffer) { vao->destroyUavBuffer(mGiBuffer); mGiBuffer = nullptr; }
    }
    mCardRecords = 0u;
    mInstanceSlots = 0u;
    mCardBufferCpu.clear();
    mInstanceBufferCpu.clear();
    mTableDirty = false;
    mBatch.clear();
    mRelight.clear();
    mRelightMode.clear();
    mLights.clear();
    mMoverLast.clear();
    mMovers.clear();
    mVct = nullptr;
    mLightJob = nullptr;
    for (unsigned i = 0; i < kCaptureBatch; ++i) mPassDef[i] = nullptr;
    for (unsigned i = 0; i < kCaptureBatch; ++i) {
        if (mCam[i] && mSceneMgr) mSceneMgr->destroyCamera(mCam[i]);
        mCam[i] = nullptr;
    }
    mSceneMgr = nullptr;
    if (Ogre::RenderSystem *rs = root->getRenderSystem()) {
        Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
        for (unsigned i = 0; i < kCardLayers; ++i) {
            if (mAtlas[i]) { tm->destroyTexture(mAtlas[i]); mAtlas[i] = nullptr; }
            if (mScratch[i]) { tm->destroyTexture(mScratch[i]); mScratch[i] = nullptr; }
        }
        if (mScratchDepth) { tm->destroyTexture(mScratchDepth); mScratchDepth = nullptr; }
        if (mRadiance) { tm->destroyTexture(mRadiance); mRadiance = nullptr; }
        if (mIndirect) { tm->destroyTexture(mIndirect); mIndirect = nullptr; }
        if (mMoverVis) { tm->destroyTexture(mMoverVis); mMoverVis = nullptr; }
    }
}

// ---------------------------------------------------------------------------
// The page allocator — Lumen's rule, and nothing more than it
// ---------------------------------------------------------------------------
//
// A FULL PAGE for anything at kCardPageSize (a card bigger than that was SPLIT
// into several of them before it got here); a QUADTREE SPLIT inside one page
// for anything smaller, so a page that cuts 32-texel cards holds sixteen of
// them. The sub-slot mask is a uint64 and the smallest card is 16 texels,
// which is (128/16)^2 = 64 slots exactly — the reason kCardMinSize is 16 and
// not 8.
bool SurfaceCache::allocRect(unsigned size, unsigned &x, unsigned &y) {
    if (mPageOwner.empty()) return false;
    const unsigned perSide = kCardAtlasSize / kCardPageSize;
    size = std::max(kCardMinSize, std::min(kCardPageSize, pow2Ceil(size)));
    if (size == kCardPageSize) {
        for (size_t p = 0; p < mPageOwner.size(); ++p) {
            if (mPageOwner[p] != 0u) continue;
            mPageOwner[p] = 1u;
            x = unsigned(p % perSide) * kCardPageSize;
            y = unsigned(p / perSide) * kCardPageSize;
            return true;
        }
        mAtlasFull = true;
        return false;
    }
    const unsigned slotsPerSide = kCardPageSize / size;
    const unsigned slots = slotsPerSide * slotsPerSide;
    // An existing page that already cuts this size and has room.
    for (size_t p = 0; p < mPageOwner.size(); ++p) {
        if (mPageOwner[p] != 2u || mPageSubSize[p] != size) continue;
        if (mPageSubUsed[p] >= slots) continue;
        for (unsigned s = 0; s < slots; ++s) {
            if (mPageSubMask[p] & (1ull << s)) continue;
            mPageSubMask[p] |= (1ull << s);
            ++mPageSubUsed[p];
            x = unsigned(p % perSide) * kCardPageSize + (s % slotsPerSide) * size;
            y = unsigned(p / perSide) * kCardPageSize + (s / slotsPerSide) * size;
            return true;
        }
    }
    // ...or a fresh page cut to it.
    for (size_t p = 0; p < mPageOwner.size(); ++p) {
        if (mPageOwner[p] != 0u) continue;
        mPageOwner[p] = 2u;
        mPageSubSize[p] = static_cast<unsigned char>(size);
        mPageSubUsed[p] = 1u;
        mPageSubMask[p] = 1ull;
        x = unsigned(p % perSide) * kCardPageSize;
        y = unsigned(p / perSide) * kCardPageSize;
        return true;
    }
    mAtlasFull = true;
    return false;
}

void SurfaceCache::freeRect(unsigned x, unsigned y, unsigned size) {
    if (mPageOwner.empty() || size == 0u) return;
    const unsigned perSide = kCardAtlasSize / kCardPageSize;
    const size_t p = size_t(y / kCardPageSize) * perSide + (x / kCardPageSize);
    if (p >= mPageOwner.size()) return;
    if (mPageOwner[p] == 1u) {
        mPageOwner[p] = 0u;
        return;
    }
    if (mPageOwner[p] != 2u || mPageSubSize[p] == 0u) return;
    const unsigned s = mPageSubSize[p];
    const unsigned slotsPerSide = kCardPageSize / s;
    const unsigned sx = (x % kCardPageSize) / s, sy = (y % kCardPageSize) / s;
    const unsigned slot = sy * slotsPerSide + sx;
    if (slot < 64u && (mPageSubMask[p] & (1ull << slot))) {
        mPageSubMask[p] &= ~(1ull << slot);
        if (mPageSubUsed[p]) --mPageSubUsed[p];
    }
    if (mPageSubUsed[p] == 0u) {
        mPageOwner[p] = 0u;
        mPageSubSize[p] = 0u;
        mPageSubMask[p] = 0ull;
    }
}

// ---------------------------------------------------------------------------
// Residency
// ---------------------------------------------------------------------------
void SurfaceCache::releaseInstance(size_t idx) {
    InstanceRec &inst = mInstances[idx];
    for (unsigned c = 0; c < inst.cardCount; ++c) {
        CardRec &card = mCards[inst.firstCard + c];
        freeRect(card.atlasX, card.atlasY, card.size);
    }
    mAtlasFull = false;
    mByNode.erase(inst.node);
    // The card block is left in place and the instance marked dead; the
    // compaction below rebuilds both vectors at once, which is O(n) per
    // residency change rather than O(n) per instance.
    inst.cardCount = 0u;
    inst.node = 0;
}

bool SurfaceCache::buildCardsFor(const CardSceneView::Candidate &cand) {
    if (!cand.item || !cand.sceneNode || !cand.cards || cand.cards->empty()) return false;

    const Ogre::Vector3 pos = cand.sceneNode->_getDerivedPosition();
    const Ogre::Quaternion rot = cand.sceneNode->_getDerivedOrientation();
    const Ogre::Vector3 scale = cand.sceneNode->_getDerivedScale();

    InstanceRec inst;
    inst.node = cand.node;
    inst.itemSlot = cand.itemSlot;
    inst.item = cand.item;
    inst.material = cand.material;
    inst.seen = true;
    inst.firstCard = unsigned(mCards.size());
    const Ogre::Aabb box = cand.item->getWorldAabbUpdated();
    inst.centre = box.mCenter;
    inst.halfSize = box.mHalfSize;
    inst.rotation = rot;
    inst.scale = scale;

    unsigned made = 0u;
    for (const MeshCardDesc &c : *cand.cards) {
        const unsigned axis = c.axis < 6u ? c.axis : 0u;
        // THE FRAME, TAKEN INTO THE WORLD. The cards are axis-aligned in MESH
        // space and a node's scale is applied along the mesh's own axes, so the
        // frame stays orthogonal under a non-uniform scale: each direction is
        // rotated and each half-size is multiplied by the scale component along
        // ITS OWN axis. (A SHEARED parent would break that, and a scene graph
        // of ours cannot produce one — the document decomposes to TRS.)
        const Ogre::Vector3 uM = kAxisU[axis], vM = kAxisV[axis], dM = kAxis[axis];
        const float su = std::fabs(uM.x * scale.x) + std::fabs(uM.y * scale.y) + std::fabs(uM.z * scale.z);
        const float sv = std::fabs(vM.x * scale.x) + std::fabs(vM.y * scale.y) + std::fabs(vM.z * scale.z);
        const float sd = std::fabs(dM.x * scale.x) + std::fabs(dM.y * scale.y) + std::fabs(dM.z * scale.z);

        const Ogre::Vector3 originW =
            pos + rot * Ogre::Vector3(c.origin.x * scale.x, c.origin.y * scale.y, c.origin.z * scale.z);
        const Ogre::Vector3 uW = rot * uM, vW = rot * vM, dW = rot * dM;
        const float halfU = c.halfU * su, halfV = c.halfV * sv, halfD = c.halfDepth * sd;
        if (!(halfU > 0.0f) || !(halfV > 0.0f)) continue;

        // THE RESOLUTION, AND THE SPLIT. A card wants `kCardTexelsPerMetre`
        // along each side; anything over a page is cut into a grid of whole
        // pages, each capturing its own sub-rectangle at the full page
        // resolution — which is Lumen's split rule and the only reason a 10 m
        // wall reads at 8 cm instead of at its whole width.
        const unsigned wantU = std::max(1u, unsigned(std::lround(halfU * 2.0f * kCardTexelsPerMetre)));
        const unsigned wantV = std::max(1u, unsigned(std::lround(halfV * 2.0f * kCardTexelsPerMetre)));
        const unsigned splitU =
            std::min(kCardMaxSplit, std::max(1u, (wantU + kCardPageSize - 1u) / kCardPageSize));
        const unsigned splitV =
            std::min(kCardMaxSplit, std::max(1u, (wantV + kCardPageSize - 1u) / kCardPageSize));
        // A card that fits in one page takes the square that covers its longer
        // side: a rectangle is captured into a square page and the read scales
        // it back, which is one number in the rect table rather than two
        // allocators.
        const unsigned oneSize =
            (splitU == 1u && splitV == 1u)
                ? std::max(kCardMinSize, std::min(kCardPageSize, pow2Ceil(std::max(wantU, wantV))))
                : kCardPageSize;

        for (unsigned sv2 = 0; sv2 < splitV; ++sv2) {
            for (unsigned su2 = 0; su2 < splitU; ++su2) {
                CardRec r;
                r.instance = unsigned(mInstances.size());
                r.axis = static_cast<unsigned char>(axis);
                r.u = uW; r.v = vW; r.d = dW;
                r.halfU = halfU / float(splitU);
                r.halfV = halfV / float(splitV);
                r.halfDepth = halfD;
                r.centre = originW + uW * ((2.0f * su2 + 1.0f - float(splitU)) * r.halfU)
                           + vW * ((2.0f * sv2 + 1.0f - float(splitV)) * r.halfV);
                r.size = oneSize;
                // THE LOD LEVEL IS THE BAKE'S, AND THE BAKE'S ALONE (ATOM
                // inventory row AT-CARDLOD). It used to be RE-DERIVED here
                // against the card's real atlas texel, with the baked
                // `MeshCard::lodLevel` kept as a fallback — one number with two
                // producers that could disagree, and after ATOM-BAKE-1 they
                // would have disagreed for a second reason: the bake picks the
                // level from the MEASURED bound and this site had only the array
                // it was handed. The rule (`lodLevelForWorldError` over
                // `lodBounds`, with the card's texel as the allowed deviation) is
                // computed once, at bake time, by `cards::levelForTexel`.
                //
                // WHAT THAT COSTS, stated rather than hidden: the bake chooses
                // for its nominal 128-texel page (`MeshBake::cardCaptureResolution`)
                // and this atlas may split a card to a different size, so a split
                // card can draw a level chosen for a slightly coarser texel. The
                // page size is the atlas's to publish and the bake's to assume,
                // and `gi.card_capture` asserts the two agree — which is the
                // check that a re-derivation quietly made impossible.
                r.lodLevel = c.lodLevel;
                if (!allocRect(r.size, r.atlasX, r.atlasY)) {
                    // The atlas is full. Everything allocated so far stands;
                    // this instance simply holds fewer cards, and the status's
                    // `pagesUsed` is what says so.
                    if (made == 0u) return false;
                    inst.cardCount = made;
                    mByNode[cand.node] = mInstances.size();
                    mInstances.push_back(inst);
                    mTableDirty = true;
                    return true;
                }
                r.queued = true;
                r.surfaceStale = true;   // a new rect: its indirect has never been marched
                r.lastUsed = mFrame;
                r.lastUpdated = 0ull;
                mCards.push_back(r);
                ++made;
            }
        }
    }
    if (made == 0u) return false;
    inst.cardCount = made;
    mByNode[cand.node] = mInstances.size();
    mInstances.push_back(inst);
    mTableDirty = true;
    return true;
}

void SurfaceCache::refreshResidency(const CardSceneView &view) {
    const float r2 = view.radius * view.radius;

    // 1. WHAT THE SCENE STILL OFFERS. Membership is decided by the CANDIDATE
    //    LIST the scene hands over, so an item that was destroyed, hidden, made
    //    a mover or re-materialled out of the GI channel simply stops being
    //    offered and its pages come back — no door to remember to knock on.
    for (InstanceRec &inst : mInstances) inst.seen = false;
    bool changed = false;
    for (const CardSceneView::Candidate &cand : view.candidates) {
        auto it = mByNode.find(cand.node);
        if (it == mByNode.end()) continue;
        InstanceRec &inst = mInstances[it->second];
        if (inst.item != cand.item) continue;   // a rebuilt Item is a new instance
        const Ogre::Aabb box = cand.item->getWorldAabbUpdated();
        inst.distance = (box.mCenter - view.viewerPos).length();
        // RESIDENCY IS RE-DECIDED EVERY FRAME, in both directions. An instance
        // the camera has walked away from gives its pages back on the frame it
        // crosses the radius — leaving `seen` false is how it is released, the
        // same path a destroyed or hidden object takes.
        if (inst.distance * inst.distance > r2) continue;
        inst.seen = true;
        // THE TRANSFORM SIGNATURE, AND IT IS NOT THE BOX ALONE. A card's
        // rectangle and its FRAME are in WORLD space and come from the node's
        // derived orientation and scale — so a 90 degree turn of a crate, or
        // ANY turn of a sphere, leaves the world AABB exactly where it was
        // while every card now describes a different face. The box, the
        // orientation and the scale are all part of the signature for that
        // reason. A moved instance's cards are wrong RECTANGLES rather than
        // stale pictures, so this is the one invalidation that frees and
        // re-allocates instead of queueing. The tolerance is the GI item walk's
        // shape, for its reason: a transform rewritten to the same value must
        // not read as movement.
        const Ogre::Vector3 dc = box.mCenter - inst.centre, dh = box.mHalfSize - inst.halfSize;
        const float tol = 1e-4f * std::max(1.0f, box.mHalfSize.length());
        const Ogre::Quaternion rot = cand.sceneNode ? cand.sceneNode->_getDerivedOrientation()
                                                    : Ogre::Quaternion::IDENTITY;
        const Ogre::Vector3 scale = cand.sceneNode ? cand.sceneNode->_getDerivedScale()
                                                   : Ogre::Vector3::UNIT_SCALE;
        // A quaternion and its negation are the same rotation, so the test is
        // on |dot| — otherwise a re-authored pose that crosses the sign would
        // re-allocate every card for nothing.
        const bool turned = std::fabs(rot.Dot(inst.rotation)) < 1.0f - 1e-6f;
        const bool rescaled = (scale - inst.scale).squaredLength() > tol * tol;
        if (dc.squaredLength() > tol * tol || dh.squaredLength() > tol * tol || turned ||
            rescaled) {
            ++mInvalidTransform;
            inst.seen = false;
            continue;
        }
        // THE MATERIAL THE INSTANCE WEARS, re-read every frame — because the
        // hover preview does not go through `noteMaterialChanged` at all.
        // MATERIAL-SWAP-GI-1's in-place swap (`OgreScene::setNodeMaterial`)
        // writes `n.materialRef` and swaps the datablock on the live Item
        // WITHOUT destroying or editing a material, which is the whole point of
        // it — so the only way the cache hears about a preview is by reading
        // the candidate's material again. Without this a hovered wall keeps the
        // old preset's albedo in the atlas for ever, an edit to the material it
        // now wears never reaches it, and an edit to the one it dropped
        // re-captures it for nothing.
        if (inst.material != cand.material) {
            inst.material = cand.material;
            for (unsigned c = 0; c < inst.cardCount; ++c)
                mCards[inst.firstCard + c].queued = mCards[inst.firstCard + c].surfaceStale = true;
            ++mInvalidMaterial;
        }
    }
    for (size_t i = 0; i < mInstances.size(); ++i) {
        if (mInstances[i].cardCount && !mInstances[i].seen) {
            releaseInstance(i);
            changed = true;
        }
    }

    // 2. COMPACTION, when anything left. Cards live in ONE vector with a
    //    per-instance block, so a departure is answered by rebuilding both
    //    vectors once rather than by a free list of holes.
    if (changed) {
        std::vector<InstanceRec> keptInst;
        std::vector<CardRec> keptCards;
        keptInst.reserve(mInstances.size());
        keptCards.reserve(mCards.size());
        mByNode.clear();
        for (InstanceRec &inst : mInstances) {
            if (!inst.cardCount) continue;
            const unsigned first = unsigned(keptCards.size());
            for (unsigned c = 0; c < inst.cardCount; ++c) {
                CardRec r = mCards[inst.firstCard + c];
                r.instance = unsigned(keptInst.size());
                keptCards.push_back(r);
            }
            inst.firstCard = first;
            mByNode[inst.node] = keptInst.size();
            keptInst.push_back(inst);
        }
        mInstances.swap(keptInst);
        mCards.swap(keptCards);
        mTableDirty = true;
    }

    // 3. WHAT ARRIVES — the nearest candidates first, so a full atlas holds the
    //    ones the camera is closest to rather than the ones the scene happened
    //    to list first. That IS "allocation by distance": the radius decides
    //    who may hold pages and this order decides who gets them when the atlas
    //    cannot hold everyone.
    //    AND A FULL ATLAS COSTS NOTHING TO DISCOVER, which is the difference
    //    between a cache and a per-frame search. `pagesUsed` is counted ONCE
    //    here, not once per arrival; the arrival loop stops on the first
    //    `allocRect` that finds no room (`mAtlasFull`, set by the allocator
    //    itself) rather than after the next SUCCESS; and a frame that cannot
    //    place anything does not sort at all. Without this, a scene bigger than
    //    the atlas re-scanned every unplaceable candidate against all 256 pages
    //    and re-sorted them, every frame, for ever.
    unsigned pagesUsed = 0u;
    for (unsigned char o : mPageOwner) pagesUsed += (o != 0u) ? 1u : 0u;
    mPagesUsed = pagesUsed;
    if (pagesUsed * 8u >= mPageOwner.size() * 7u) return;   // 7/8 full: take no new work

    std::vector<const CardSceneView::Candidate *> arriving;
    arriving.reserve(view.candidates.size());
    for (const CardSceneView::Candidate &cand : view.candidates) {
        if (!cand.item || !cand.cards || cand.cards->empty()) continue;
        if (mByNode.find(cand.node) != mByNode.end()) continue;
        const Ogre::Aabb box = cand.item->getWorldAabbUpdated();
        if ((box.mCenter - view.viewerPos).squaredLength() > r2) continue;
        arriving.push_back(&cand);
    }
    if (arriving.empty()) return;
    const Ogre::Vector3 eye = view.viewerPos;
    std::sort(arriving.begin(), arriving.end(),
              [eye](const CardSceneView::Candidate *a, const CardSceneView::Candidate *b) {
                  const float da = (a->item->getWorldAabb().mCenter - eye).squaredLength();
                  const float db = (b->item->getWorldAabb().mCenter - eye).squaredLength();
                  if (da != db) return da < db;
                  return a->node < b->node;
              });
    mAtlasFull = false;
    for (const CardSceneView::Candidate *cand : arriving) {
        buildCardsFor(*cand);
        if (mAtlasFull) break;
    }
    pagesUsed = 0u;
    for (unsigned char o : mPageOwner) pagesUsed += (o != 0u) ? 1u : 0u;
    mPagesUsed = pagesUsed;
}

// ---------------------------------------------------------------------------
// The capture
// ---------------------------------------------------------------------------
void SurfaceCache::aimCamera(const CardRec &card, unsigned slot) {
    const float margin = captureMargin(card.halfDepth);
    Ogre::Camera *cam = mCam[slot];
    cam->setOrthoWindow(std::max(2.0f * card.halfU, 1e-4f), std::max(2.0f * card.halfV, 1e-4f));
    cam->setNearClipDistance(0.001f);
    cam->setFarClipDistance(2.0f * card.halfDepth + 2.0f * margin + 0.01f);
    cam->setPosition(card.centre + card.d * (card.halfDepth + margin));
    // Ogre looks down -Z, so the card's OUTWARD axis is the camera's +Z. The
    // frame is right-handed (u x v = d, asserted by gi.card_capture), so
    // FromAxes builds a rotation and not a reflection.
    Ogre::Quaternion q;
    q.FromAxes(card.u, card.v, card.d);
    cam->setOrientation(q);
    // THE VIEWPORT IS THE CARD'S OWN TEXELS. The copy moves the scratch's
    // top-left `size` square into the atlas, so a card smaller than a page must
    // be RENDERED into exactly that square — rendered across the whole page, a
    // 32-texel card's copy took the top-left eighth of its own picture
    // (PHOTON-CARDS-1, measured: `gi.card_capture`'s sub-page arm). The pass
    // reads its definition's rectangle at every execution, and the definition
    // is this Component's own.
    const float frac = float(card.size) / float(kCardPageSize);
    Ogre::CompositorPassDef::ViewportRect &vp = mPassDef[slot]->mVpRect[0];
    vp.mVpLeft = vp.mVpTop = vp.mVpScissorLeft = vp.mVpScissorTop = 0.0f;
    vp.mVpWidth = vp.mVpHeight = vp.mVpScissorWidth = vp.mVpScissorHeight = frac;
}

// ---------------------------------------------------------------------------
// The batch, as Ogre's frame executes it
// ---------------------------------------------------------------------------
//
// `update()` PLANS: it picks this frame's cards in Lumen's order under the
// texel budget (at most `kCaptureBatch`), aims camera i at card i and sets the
// low N bits of the workspace's execution mask. Ogre then EXECUTES the
// workspace inside its own frame (makeWorkspace says why that is the shadow
// fix), and these four hooks do the per-card work around each pass and the
// copies after the last one.
void SurfaceCache::allWorkspacesBeforeBeginUpdate() { gCapturing = false; }

void SurfaceCache::workspacePreUpdate(Ogre::CompositorWorkspace *ws) {
    if (ws != mWs) return;
    // A BATCH PLANNED FOR ANOTHER FRAME NEVER RUNS. `update()` plans and this
    // frame executes; a frame that did not plan (its scene is not drawn, so
    // `update()` never ran) must not replay the last plan against items that
    // may have died since.
    if (mBatchFrame != Ogre::Root::getSingleton().getCompositorManager2()->getFrameCount()) {
        mBatch.clear();
        mRelight.clear();
            mWs->setExecutionMask(0u);
    }
    if (mBatch.empty()) return;
    gCapturing = true;
    mBatchStart = std::chrono::steady_clock::now();
}

void SurfaceCache::passPreExecute(Ogre::CompositorPass *pass) {
    const Ogre::uint32 id = pass->getDefinition()->mIdentifier;
    if (id < kCardPassIdentifier || id >= kCardPassIdentifier + kCaptureBatch) return;
    const unsigned slot = id - kCardPassIdentifier;
    if (slot >= mBatch.size()) return;
    CardRec &card = mCards[mBatch[slot]];
    InstanceRec &inst = mInstances[card.instance];
    if (!inst.item) return;
    // THE INCLUDE CHANNEL, for the length of one pass and no longer — granted
    // HERE, before the pass updates its shadow node, because the node fits its
    // casters box under the pass's own visibility mask. The item keeps every
    // bit it had; the capture bit is ADDED and taken away in passPosExecute.
    mSubjectFlags = inst.item->getVisibilityFlags();
    mSubjectLod = inst.item->getCurrentMeshLod();
    inst.item->setVisibilityFlags(mSubjectFlags | detail::kCardSubjectBit);
    // THE CARD'S OWN LOD (patch 0085): the level is a capture DECISION (the
    // bake's, spent at the card's texel), and upstream keeps the member
    // protected with only a "reset to 0" door.
    inst.item->_setCurrentMeshLod(card.lodLevel);
}

void SurfaceCache::passPosExecute(Ogre::CompositorPass *pass) {
    const Ogre::uint32 id = pass->getDefinition()->mIdentifier;
    if (id < kCardPassIdentifier || id >= kCardPassIdentifier + kCaptureBatch) return;
    const unsigned slot = id - kCardPassIdentifier;
    if (slot >= mBatch.size()) return;
    InstanceRec &inst = mInstances[mCards[mBatch[slot]].instance];
    if (!inst.item) return;
    inst.item->setVisibilityFlags(mSubjectFlags);
    inst.item->_setCurrentMeshLod(mSubjectLod);
}

void SurfaceCache::workspacePosUpdate(Ogre::CompositorWorkspace *ws) {
    if (ws != mWs) return;
    gCapturing = false;
    if (mBatch.empty()) {
        traceMovers();
        relightCards();
        // A card's flags moved (its indirect half marched): the ray read,
        // later in this frame, must see it.
        syncBuffers();
        return;
    }
    const auto tB = std::chrono::steady_clock::now();
    // ...AND INTO THE ATLAS, after the whole batch: five small copies a card,
    // from its own slice of the scratch (the reason the scratch exists at all
    // is in SurfaceCache.h — this pin's render-pass clear is whole-target).
    for (unsigned slot = 0; slot < mBatch.size(); ++slot) {
        CardRec &card = mCards[mBatch[slot]];
        for (unsigned i = 0; i < kCardLayers; ++i) {
            Ogre::TextureBox src = mScratch[i]->getEmptyBox(0);
            src.width = card.size;
            src.height = card.size;
            src.sliceStart = slot;
            src.numSlices = 1u;
            Ogre::TextureBox dst = mAtlas[i]->getEmptyBox(0);
            dst.x = card.atlasX;
            dst.y = card.atlasY;
            dst.width = card.size;
            dst.height = card.size;
            mScratch[i]->copyTo(mAtlas[i], dst, 0, src, 0);
        }
        card.queued = false;
        if (!card.lastUpdated) mTableDirty = true;   // its first capture: readable now
        card.lastUpdated = mFrame;
        ++mCaptures;
        ++mCapturesLastFrame;
        mTexelsLastFrame += card.size * card.size;
    }
    const auto tC = std::chrono::steady_clock::now();
    mWsMs = float(std::chrono::duration<double, std::milli>(tB - mBatchStart).count());
    mCopyMs = float(std::chrono::duration<double, std::milli>(tC - tB).count());
    mCaptureMs = mWsMs + mCopyMs;
    // THE MOVERS' TERM of the cards just captured and of every card a mover's
    // footprint reaches, traced here — after the copies (the trace reads the
    // new Depth and Normal) and before the relight that multiplies it in.
    traceMovers();
    mBatch.clear();
    mWs->setExecutionMask(0u);
    // ...and the cards just captured are relit from their new texels, in the
    // same frame, before anything could read them — and the table carries
    // their flags (captured; indirect stale or marched) before the ray read,
    // later in this frame, looks.
    relightCards();
    syncBuffers();
}

// ---------------------------------------------------------------------------
// THE LIT CARD (PHOTON-CARDS-1, SC-1c) — the sixth layer
// ---------------------------------------------------------------------------
//
// A texel's radiance = the scene's lights through HlmsPbs's own diffuse lobe
// (the sun through the captured shadow term) + the pixel's own diffuse GI
// marched from the texel (the one voxel reader, the one environment at each
// escape; cached in the seventh layer, `mIndirect`, under its own budget) +
// its emissive; the job and every
// statement about what it includes and what it does not are in
// media/Hlms/Jahshaka/JahCardLight_cs.glsl. THE LIGHT LIST IS WRITTEN HERE, on
// the CPU, in world space: the Forward+ list the pixel shader binds is built
// per CAMERA in that camera's view space (clusters of a screen), which is
// meaningless for a card texel anywhere in the world — so the job gets the
// pass buffer's own light layout, in world coordinates, from the scene's
// lights (the shape VctLighting's own light injection takes).
namespace {
/// The most cards one frame relights (the relight list's size, 80 B a card).
constexpr unsigned kMaxRelights = 1024u;
/// The most lights the job sums (80 B a light after a 16 B count).
constexpr unsigned kMaxCardLights = 64u;
constexpr unsigned kRelightFloats = 20u;
/// The chain's parameter block (CardGiParams in the shader): chainInvRes[8],
/// chainFromPrev[14], the volume's origin and inverse size, the counts, the
/// environment's gain and mips, its nine SH coefficients — 35 vec4.
constexpr unsigned kGiParamFloats = 37u * 4u;   // ... + the cloud shadow's map and sun (CLOUDS-2D-2)
constexpr unsigned kMaxCardCascades = 8u;
constexpr unsigned kLightFloats = 20u;
}   // namespace

void SurfaceCache::planRelights(const CardSceneView &view) {
    mRelight.clear();
    mRelightMode.clear();
    mLights = view.lights;
    mVct = view.vct;
    mCloudField = view.cloudField;
    for (int k = 0; k < 4; ++k) { mCloudMap[k] = view.cloudMap[k]; mCloudSun[k] = view.cloudSun[k]; }
    // THE RADIANCE SIGNATURE: a light write that changed what a card's
    // DIRECT radiance depends on relights every resident card and recaptures
    // none. A write that also moved a shadow queued the cards for capture
    // above, and a queued card is relit after its capture, not before it.
    if (view.radianceSerial != mRadianceSerial) {
        for (CardRec &c : mCards) c.relight = true;
        if (!mRadianceMovingLastFrame) ++mInvalidRadiance;   // one per gesture
        mRadianceMovingLastFrame = true;
        mRadianceSerial = view.radianceSerial;
    } else {
        mRadianceMovingLastFrame = false;
    }
    // THE INDIRECT SIGNATURE: the chain re-injected (a light tick, a settle, a
    // cascade rebuild, a material generation) — the INDIRECT half of every
    // resident card is stale, under its own budget; the direct half is not.
    if (view.indirectSerial != mIndirectSerial) {
        for (CardRec &c : mCards) c.relightIndirect = true;
        if (!mIndirectMovingLastFrame) ++mInvalidIndirect;
        mIndirectMovingLastFrame = true;
        mIndirectSerial = view.indirectSerial;
    } else {
        mIndirectMovingLastFrame = false;
    }
    if (!mRadiance) return;
    // The cards captured THIS frame: their direct half is stale. Their
    // INDIRECT half is stale only when the capture changed the SURFACE — a new
    // rect, or a material (the kD, the normal, the roughness the march and its
    // lobe read). A capture a light write asked for changed the shadow term
    // alone, so the cached indirect stands (a dragged light recaptures every
    // frame and must not re-march against voxels that did not move).
    for (unsigned idx : mBatch) {
        CardRec &c = mCards[idx];
        c.relight = true;
        if (c.surfaceStale) {
            c.relightIndirect = true;
            if (c.indirectValid) mTableDirty = true;   // the ray read falls back to the voxels
            c.indirectValid = false;
            c.surfaceStale = false;
        }
    }
    const auto ready = [this](unsigned i) {
        const CardRec &c = mCards[i];
        const bool thisFrame = std::find(mBatch.begin(), mBatch.end(), i) != mBatch.end();
        return thisFrame || (!c.queued && c.lastUpdated);
    };
    const auto oldestFirst = [this](std::vector<unsigned> &v, bool indirect) {
        std::sort(v.begin(), v.end(), [this, indirect](unsigned a, unsigned b) {
            const unsigned long long ka = indirect ? mCards[a].lastIndirect : mCards[a].lastRelit;
            const unsigned long long kb = indirect ? mCards[b].lastIndirect : mCards[b].lastRelit;
            if (ka != kb) return ka < kb;
            return a < b;
        });
    };
    std::vector<char> taken(mCards.size(), 0);

    // 1. THE INDIRECT LIST (the march; it recomputes the direct half too):
    //    this frame's captures first, then the stalest, under the INDIRECT
    //    budget.
    {
        std::vector<unsigned> want;
        for (unsigned idx : mBatch)
            if (mCards[idx].relightIndirect) want.push_back(idx);
        std::vector<unsigned> rest;
        for (unsigned i = 0; i < mCards.size(); ++i)
            if (mCards[i].relightIndirect && ready(i) &&
                std::find(mBatch.begin(), mBatch.end(), i) == mBatch.end())
                rest.push_back(i);
        oldestFirst(rest, true);
        want.insert(want.end(), rest.begin(), rest.end());
        unsigned spent = 0u;
        for (unsigned idx : want) {
            const unsigned cost = mCards[idx].size * mCards[idx].size;
            if (spent && spent + cost > mIndirectBudget) break;
            if (mRelight.size() >= kMaxRelights) break;
            mRelight.push_back(idx);
            mRelightMode.push_back(1u);
            taken[idx] = 1;
            spent += cost;
        }
    }
    // 2. THE DIRECT LIST: every other card whose direct half is stale, under
    //    the DIRECT budget — its indirect read back from the cached layer, or
    //    zero while it has none (a fresh capture the march has not reached).
    {
        std::vector<unsigned> want;
        for (unsigned idx : mBatch)
            if (!taken[idx]) want.push_back(idx);
        std::vector<unsigned> rest;
        for (unsigned i = 0; i < mCards.size(); ++i)
            if (!taken[i] && mCards[i].relight && ready(i) &&
                std::find(mBatch.begin(), mBatch.end(), i) == mBatch.end())
                rest.push_back(i);
        oldestFirst(rest, false);
        want.insert(want.end(), rest.begin(), rest.end());
        unsigned spent = 0u;
        for (unsigned idx : want) {
            const unsigned cost = mCards[idx].size * mCards[idx].size;
            if (spent && spent + cost > mLightBudget) break;
            if (mRelight.size() >= kMaxRelights) break;
            mRelight.push_back(idx);
            mRelightMode.push_back(mCards[idx].indirectValid ? 0u : 2u);
            spent += cost;
        }
    }
}

void SurfaceCache::relightCards() {
    if (mRelight.empty() || !mRadiance) { mLights.clear(); mVct = nullptr; return; }
    const auto t0 = std::chrono::steady_clock::now();
    Ogre::Root &root = Ogre::Root::getSingleton();
    Ogre::RenderSystem *rs = root.getRenderSystem();
    Ogre::HlmsCompute *hc = root.getHlmsManager()->getComputeHlms();
    if (!mLightJob) mLightJob = hc ? hc->findComputeJobNoThrow("Jahshaka/CardLight") : nullptr;
    if (!mLightJob) { mRelight.clear(); mLights.clear(); mVct = nullptr; return; }
    Ogre::VaoManager *vao = rs->getVaoManager();

    // ---- THE RELIGHT LIST: each card's capture frame, as the job unprojects it.
    mRelightCpu.assign(size_t(kMaxRelights) * kRelightFloats, 0.0f);
    for (size_t i = 0; i < mRelight.size(); ++i) {
        const CardRec &c = mCards[mRelight[i]];
        float *r = &mRelightCpu[i * kRelightFloats];
        const Ogre::Vector3 cam = c.centre + c.d * (c.halfDepth + captureMargin(c.halfDepth));
        r[0] = float(c.atlasX); r[1] = float(c.atlasY); r[2] = float(c.size);
        r[3] = float(mRelightMode[i]);
        r[4] = cam.x; r[5] = cam.y; r[6] = cam.z;
        // THE MOVERS' TERM IS MULTIPLIED IN where the card carries a trace.
        r[7] = c.moverTraced ? 1.0f : 0.0f;
        // The ortho window aimCamera gives the capture, and the same clamp.
        r[8] = c.u.x; r[9] = c.u.y; r[10] = c.u.z; r[11] = std::max(2.0f * c.halfU, 1e-4f);
        r[12] = c.v.x; r[13] = c.v.y; r[14] = c.v.z; r[15] = std::max(2.0f * c.halfV, 1e-4f);
        r[16] = c.d.x; r[17] = c.d.y; r[18] = c.d.z; r[19] = 0.0f;
    }

    // ---- THE LIGHTS, in world space, in the pass buffer's layout, from the
    // SCENE — every light node, handed over by OgreScene::updateSurfaceCache —
    // and NEVER from the frame's global list: that list is culled against the
    // frame's visible cameras, and a card lights a surface OFF screen (the
    // reader is a ray hit), so a lamp no camera sees this frame must still
    // light it. ONE source, the sun included: the one light whose visibility is
    // the captured shadow term is the light the capture's shadow node gave its
    // PSSM maps, which is the frame's rule applied to the same list — the first
    // visible shadow-casting directional light in creation order
    // (SceneManager::quickSortDirectionalLights: casters first, then by id).
    const Ogre::Light *sun = cardSun();
    mLightCpu.assign(4u + size_t(kMaxCardLights) * kLightFloats, 0.0f);
    unsigned numLights = 0u;
    unsigned dropped = 0u;
    for (const Ogre::Light *l : mLights) {
        if (!l || !l->getVisible()) continue;
        const Ogre::Light::LightTypes type = l->getType();
        if (type != Ogre::Light::LT_DIRECTIONAL && type != Ogre::Light::LT_POINT &&
            type != Ogre::Light::LT_SPOTLIGHT)
            continue;   // area lights: not summed (JahCardLight_cs.glsl says so)
        if (numLights >= kMaxCardLights) { ++dropped; continue; }
        float *o = &mLightCpu[4u + size_t(numLights) * kLightFloats];
        const Ogre::ColourValue c = l->getDiffuseColour() * l->getPowerScale();
        if (type == Ogre::Light::LT_DIRECTIONAL) {
            const Ogre::Vector3 toLight = -l->getDerivedDirection();
            o[0] = toLight.x; o[1] = toLight.y; o[2] = toLight.z; o[3] = 0.0f;
        } else {
            const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
            o[0] = p.x; o[1] = p.y; o[2] = p.z;
            o[3] = type == Ogre::Light::LT_POINT ? 1.0f : 2.0f;
        }
        o[4] = c.r; o[5] = c.g; o[6] = c.b; o[7] = (l == sun) ? 1.0f : 0.0f;
        const float range = l->getAttenuationRange();
        o[8] = range; o[9] = l->getAttenuationLinear(); o[10] = l->getAttenuationQuadric();
        o[11] = range > 0.0f ? 1.0f / range : 0.0f;   // patch 0018's fade, as light0Buf writes it
        const Ogre::Vector3 sd = l->getDerivedDirection();
        o[12] = sd.x; o[13] = sd.y; o[14] = sd.z; o[15] = 0.0f;
        const float inner = l->getSpotlightInnerAngle().valueRadians();
        const float outer = l->getSpotlightOuterAngle().valueRadians();
        const float denom = std::cos(inner * 0.5f) - std::cos(outer * 0.5f);
        o[16] = std::fabs(denom) > 1e-6f ? 1.0f / denom : 1e6f;
        o[17] = std::cos(outer * 0.5f);
        o[18] = l->getSpotlightFalloff();
        o[19] = 0.0f;
        ++numLights;
    }
    // A LIGHT THE JOB CANNOT HOLD is counted every frame and SAID once per cache.
    mLightsDropped = dropped;
    if (dropped && !mLightsDroppedLogged) {
        mLightsDroppedLogged = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka cards: " + std::to_string(dropped) + " light(s) beyond the card relight's " +
            std::to_string(kMaxCardLights) + " were not summed into the card radiance");
    }
    const Ogre::uint32 count[4] = { numLights, 0u, 0u, 0u };
    std::memcpy(mLightCpu.data(), count, sizeof(count));

    const size_t relightElems = kMaxRelights * kRelightFloats / 4u;   // vec4s
    const size_t lightElems = 1u + size_t(kMaxCardLights) * kLightFloats / 4u;
    if (!mRelightBuffer)
        mRelightBuffer = vao->createUavBuffer(relightElems, 16u, 0, mRelightCpu.data(), false);
    else
        mRelightBuffer->upload(mRelightCpu.data(), 0, mRelight.size() * kRelightFloats / 4u);
    if (!mLightBuffer)
        mLightBuffer = vao->createUavBuffer(lightElems, 16u, 0, mLightCpu.data(), false);
    else
        mLightBuffer->upload(mLightCpu.data(), 0, 1u + size_t(numLights) * kLightFloats / 4u);

    // ---- THE CHAIN AND THE ENVIRONMENT, from the one VctLighting every reader
    // takes them from (the pixel's pass buffer is filled from the same calls).
    Ogre::VctLighting *vct = mVct;
    unsigned numCascades = 0u;
    bool aniso = false;
    Ogre::TextureGpu *envCube = nullptr;
    mGiCpu.assign(kGiParamFloats, 0.0f);
    if (vct && vct->getLightVoxelTextures() && vct->getLightVoxelTextures()[0]) {
        const size_t chainLen = vct->getNumCascades();
        numCascades = unsigned(std::min<size_t>(chainLen, kMaxCardCascades));
        aniso = vct->isAnisotropic();
        std::vector<float> invRes(4u * chainLen, 0.0f);
        std::vector<float> fromPrev(8u * std::max<size_t>(chainLen, 1u), 0.0f);
        vct->getCascadeChainParams(invRes.data(), fromPrev.data());
        float *g = mGiCpu.data();
        std::memcpy(g, invRes.data(), 4u * numCascades * sizeof(float));            // chainInvRes[8]
        std::memcpy(g + 32u, fromPrev.data(),
                    8u * (numCascades ? numCascades - 1u : 0u) * sizeof(float));    // chainFromPrev[14]
        const Ogre::Vector3 origin = vct->getVoxelizer()->getVoxelOrigin();
        const Ogre::Vector3 size = vct->getVoxelizer()->getVoxelSize();
        g[88] = origin.x; g[89] = origin.y; g[90] = origin.z;
        g[92] = 1.0f / size.x; g[93] = 1.0f / size.y; g[94] = 1.0f / size.z;
        g[96] = float(numCascades);
        g[97] = vct->getFinalMultiplier();
        envCube = vct->getEnvironmentCube();
        const float *gain = vct->getEnvironmentGain();
        g[100] = gain[0]; g[101] = gain[1]; g[102] = gain[2];
        g[103] = envCube ? float(envCube->getNumMipmaps()) : 1.0f;
        const float *sh = vct->getEnvironmentSh();
        for (unsigned k = 0; k < 9u; ++k) {
            g[104u + 4u * k] = sh[3u * k];
            g[105u + 4u * k] = sh[3u * k + 1u];
            g[106u + 4u * k] = sh[3u * k + 2u];
        }
    }
    // THE CLOUD SHADOW (CLOUDS-2D-2): the pixel's own map and throw, the last
    // two vec4s of the block (JahCardLight_cs.glsl's CardGiParams).
    for (int k = 0; k < 4; ++k) {
        mGiCpu[140u + unsigned(k)] = mCloudMap[k];
        mGiCpu[144u + unsigned(k)] = mCloudSun[k];
    }
    mIndirectOnLastRelight = numCascades > 0u;
    if (!mGiBuffer)
        mGiBuffer = vao->createUavBuffer(kGiParamFloats / 4u, 16u, 0, mGiCpu.data(), false);
    else
        mGiBuffer->upload(mGiCpu.data(), 0, kGiParamFloats / 4u);

    // ---- THE DISPATCH: the job is shared process-wide by name, so every
    // binding is set here and released after the dispatch is recorded.
    if (mLightJob->getProperty("hlms_num_vct_cascades") != Ogre::int32(numCascades))
        mLightJob->setProperty("hlms_num_vct_cascades", Ogre::int32(numCascades));
    if (mLightJob->getProperty("vct_anisotropic") != (aniso ? 1 : 0))
        mLightJob->setProperty("vct_anisotropic", aniso ? 1 : 0);
    if (mLightJob->getProperty("jah_env") != (envCube ? 1 : 0))
        mLightJob->setProperty("jah_env", envCube ? 1 : 0);
    // THE STORE'S ROUNDING follows the layer's format (jahCardRound): the
    // pre-scale is R11G11B10F's, and the RGBA16F fallback stores unrounded.
    {
        const Ogre::int32 round = mRadiance->getPixelFormat() == Ogre::PFG_R11G11B10_FLOAT ? 1 : 0;
        if (mLightJob->getProperty("jah_card_round_r11g11b10") != round)
            mLightJob->setProperty("jah_card_round_r11g11b10", round);
    }
    // THE PIXEL'S CONE SET (Vct_piece_ps.any's `vct_cone_dirs`, which HlmsPbs
    // sets from getVctFullConeCount): the card's indirect is the pixel's
    // integral, so it walks the pixel's cones.
    {
        auto *pbs = static_cast<Ogre::HlmsPbs *>(root.getHlmsManager()->getHlms(Ogre::HLMS_PBS));
        const Ogre::int32 cones = (pbs && pbs->getVctFullConeCount()) ? 6 : 4;
        if (mLightJob->getProperty("vct_cone_dirs") != cones)
            mLightJob->setProperty("vct_cone_dirs", cones);
    }
    // Every light volume a reader binds per cascade: the total, the anisotropic
    // axes, the per-axis coverage (PHOTON-VOXEL-3), the surface position
    // (PHOTON-VOXEL-4) - VctLighting's list.
    const unsigned kinds = numCascades ? unsigned(vct->getNumVoxelTextures()) : 0u;
    const unsigned vctUnits = numCascades ? kinds * numCascades + (envCube ? 1u : 0u) : 0u;
    // THE CLOUD FIELD, the LAST texture unit (after the chain and the
    // environment), with the field's own wrapped sampler.
    const Ogre::HlmsSamplerblock *cloudWrap = mCloudField
        ? detail::FogHlmsListener::acquireWrapSampler(root.getHlmsManager()) : nullptr;
    const bool cloud = mCloudField && cloudWrap;
    if (mLightJob->getProperty("jah_cloud_shadow") != (cloud ? 1 : 0))
        mLightJob->setProperty("jah_cloud_shadow", cloud ? 1 : 0);
    const unsigned order[kCardLayers] = { unsigned(CardLayer::Albedo), unsigned(CardLayer::Normal),
                                          unsigned(CardLayer::Depth), unsigned(CardLayer::Emissive),
                                          unsigned(CardLayer::ShadowRough) };
    mLightJob->setNumTexUnits(Ogre::uint8(kCardLayers + vctUnits + (cloud ? 1u : 0u)));
    if (cloud) {
        Ogre::DescriptorSetTexture2::TextureSlot slot(
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = mCloudField;
        mLightJob->setTexture(Ogre::uint8(kCardLayers + vctUnits), slot, cloudWrap);
    }
    for (unsigned i = 0; i < kCardLayers; ++i) {
        Ogre::DescriptorSetTexture2::TextureSlot slot(
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = mAtlas[order[i]];
        mLightJob->setTexture(Ogre::uint8(i), slot, nullptr, false);
    }
    if (numCascades) {
        // The chain's volumes, per kind then per cascade (the pixel's and the
        // parity harness's order), the trilinear sampler on the first — the
        // shader's `vSmp` — and the environment cube last.
        Ogre::uint8 unit = Ogre::uint8(kCardLayers);
        for (unsigned kind = 0; kind < kinds; ++kind)
            for (unsigned c = 0; c < numCascades; ++c, ++unit) {
                Ogre::DescriptorSetTexture2::TextureSlot slot(
                    Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
                slot.texture = vct->getLightVoxelTextures(c)[kind];
                if (unit == kCardLayers)
                    mLightJob->setTexture(unit, slot, vct->getBindTrilinearSamplerblock());
                else
                    mLightJob->setTexture(unit, slot, nullptr, false);
            }
        if (envCube) {
            Ogre::DescriptorSetTexture2::TextureSlot slot(
                Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
            slot.texture = envCube;
            mLightJob->setTexture(unit, slot, nullptr, false);
        }
    }
    const auto bufSlot = [](Ogre::UavBufferPacked *b, Ogre::ResourceAccess::ResourceAccess a) {
        Ogre::DescriptorSetUav::BufferSlot slot(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        slot.buffer = b;
        slot.offset = 0;
        slot.sizeBytes = 0;
        slot.access = a;
        return slot;
    };
    mLightJob->_setUavBuffer(0u, bufSlot(mRelightBuffer, Ogre::ResourceAccess::Read));
    mLightJob->_setUavBuffer(1u, bufSlot(mLightBuffer, Ogre::ResourceAccess::Read));
    {
        Ogre::DescriptorSetUav::TextureSlot uav(Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        uav.texture = mRadiance;
        uav.access = Ogre::ResourceAccess::Write;
        uav.pixelFormat = mRadiance->getPixelFormat();
        mLightJob->_setUavTexture(3u, uav);
        uav.texture = mIndirect;
        uav.access = Ogre::ResourceAccess::ReadWrite;
        uav.pixelFormat = mIndirect->getPixelFormat();
        mLightJob->_setUavTexture(4u, uav);
        uav.texture = mMoverVis;
        uav.access = Ogre::ResourceAccess::Read;
        uav.pixelFormat = mMoverVis->getPixelFormat();
        mLightJob->_setUavTexture(5u, uav);
    }
    mLightJob->_setUavBuffer(2u, bufSlot(mGiBuffer, Ogre::ResourceAccess::Read));
    mLightJob->setThreadsPerGroup(8u, 8u, 1u);
    mLightJob->setNumThreadGroups(kCardPageSize / 8u, kCardPageSize / 8u, unsigned(mRelight.size()));
    {
        Ogre::ResourceTransitionArray &rt = rs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
        mLightJob->analyzeBarriers(rt);
        rs->executeResourceTransition(rt);
        if (mMoverHooks.timeRelight) mMoverHooks.timeRelight(true);
        hc->dispatch(mLightJob, nullptr, nullptr);
        if (mMoverHooks.timeRelight) mMoverHooks.timeRelight(false);
    }
    {
        const Ogre::DescriptorSetUav::BufferSlot empty = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
        mLightJob->_setUavBuffer(0u, empty);
        mLightJob->_setUavBuffer(1u, empty);
        mLightJob->_setUavBuffer(2u, empty);
        mLightJob->_setUavTexture(3u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        mLightJob->_setUavTexture(4u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        mLightJob->_setUavTexture(5u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        mLightJob->setNumTexUnits(0u);
    }

    for (size_t i = 0; i < mRelight.size(); ++i) {
        CardRec &c = mCards[mRelight[i]];
        c.relight = false;
        c.lastRelit = mFrame;
        ++mRelights;
        ++mRelitLastFrame;
        mRelitTexelsLastFrame += c.size * c.size;
        if (mRelightMode[i] == 1u) {
            c.relightIndirect = false;
            if (!c.indirectValid) mTableDirty = true;  // the ray read takes the card from here
            c.indirectValid = true;
            c.lastIndirect = mFrame;
            ++mIndirectRelights;
            ++mIndirectLastFrame;
            mIndirectTexelsLastFrame += c.size * c.size;
        }
    }
    mRelight.clear();
    mRelightMode.clear();
    mLights.clear();
    mVct = nullptr;
    mLightMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// ---------------------------------------------------------------------------
// The movers' shadow (PHOTON-CARDS-4)
// ---------------------------------------------------------------------------
//
// A CARD'S SUN VISIBILITY IS TWO TERMS. The still world's is the capture's
// (ShadowRough.x: the card shadow node draws the probe kind's casters,
// kVisibleBit alone, and a mover carries kMovableBit INSTEAD — measured on the
// base: a forced relight and a forced recapture both left a mover's floor at
// shadow 1.000, a static crate's at 0.000). The movers' is TRACED here: a
// shadow-casting mover's transform write traces one sun ray per texel of every
// card its sun-projected footprint reaches — the footprint it left AND the one
// it entered — against the movers alone (kRayMaskMoverCaster), into the R8
// layer the relight multiplies into the sun's term. A mover at rest costs
// nothing: no write, no trace, no relight.
//
// WHICH LIGHTS: the SUN only — the first visible shadow-casting directional
// light, the one light whose visibility a card holds at all (a point or spot
// light is unshadowed in the card relight by design: JahCardLight_cs.glsl).
//
// THE FOOTPRINT: a box's shadow is the box swept along the sun's direction to
// infinity; a card is inside it when their projections on the plane normal to
// the sun overlap and some of the card lies below the box's top along the sun.
// Conservative (a card's whole box, a caster's whole AABB), which costs budget
// and never a missed shadow.
//
// A STILL CASTER THAT MOVES (finding 3): its shadow is the still world's, so
// its transform write queues the cards of its old and new footprints for a
// RECAPTURE — the capture's own budget and order, and the capture's own term.
namespace {
/// The footprint's margin, metres: the capture's PSSM filter reaches a few
/// shadow-map texels past a caster's silhouette.
constexpr float kFootprintMargin = 0.05f;
/// A ray's length: the sun is at infinity; a mover beyond the residency radius
/// casts onto no resident card anyway.
constexpr float kMoverRayRange = 10000.0f;
struct SunFrame {
    Ogre::Vector3 L, e1, e2;
};
struct Footprint {
    float a0, a1, b0, b1, top;   ///< the projected rect, and the top along the sun
};
SunFrame sunFrame(const Ogre::Vector3 &toSun) {
    SunFrame f;
    f.L = toSun;
    const Ogre::Vector3 ref = std::fabs(toSun.y) < 0.9f ? Ogre::Vector3::UNIT_Y : Ogre::Vector3::UNIT_X;
    f.e1 = toSun.crossProduct(ref).normalisedCopy();
    f.e2 = toSun.crossProduct(f.e1);
    return f;
}
template <class Corners>
Footprint project(const SunFrame &f, const Corners &corners, bool top) {
    Footprint p{ 1e30f, -1e30f, 1e30f, -1e30f, top ? -1e30f : 1e30f };
    for (const Ogre::Vector3 &c : corners) {
        const float a = f.e1.dotProduct(c), b = f.e2.dotProduct(c), h = f.L.dotProduct(c);
        p.a0 = std::min(p.a0, a); p.a1 = std::max(p.a1, a);
        p.b0 = std::min(p.b0, b); p.b1 = std::max(p.b1, b);
        p.top = top ? std::max(p.top, h) : std::min(p.top, h);
    }
    return p;
}
Footprint boxFootprint(const SunFrame &f, const Ogre::Vector3 &mn, const Ogre::Vector3 &mx) {
    Ogre::Vector3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = Ogre::Vector3((i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z);
    Footprint p = project(f, c, true);
    p.a0 -= kFootprintMargin; p.a1 += kFootprintMargin;
    p.b0 -= kFootprintMargin; p.b1 += kFootprintMargin;
    return p;
}
Footprint cardFootprint(const SunFrame &f, const CardRec &card) {
    Ogre::Vector3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = card.centre + card.u * ((i & 1) ? card.halfU : -card.halfU) +
               card.v * ((i & 2) ? card.halfV : -card.halfV) +
               card.d * ((i & 4) ? card.halfDepth : -card.halfDepth);
    return project(f, c, false);   // .top = the card's LOWEST point along the sun
}
bool shades(const Footprint &caster, const Footprint &card) {
    return caster.a0 <= card.a1 && card.a0 <= caster.a1 && caster.b0 <= card.b1 &&
           card.b0 <= caster.b1 && card.top < caster.top;
}
}   // namespace

const Ogre::Light *SurfaceCache::cardSun() const {
    const Ogre::Light *sun = nullptr;
    for (const Ogre::Light *l : mLights) {
        if (!l || l->getType() != Ogre::Light::LT_DIRECTIONAL || !l->getCastShadows() ||
            !l->getVisible())
            continue;
        if (!sun || l->getId() < sun->getId()) sun = l;
    }
    return sun;
}

void SurfaceCache::traceMovers() {
    // THIS FRAME ONLY: `mLights` is the frame's list only when update() planned
    // this frame (workspacePreUpdate drops a stale plan the same way).
    if (!mMoverHooks.frame || !mMoverVis || !mRadiance) return;
    if (mBatchFrame != Ogre::Root::getSingleton().getCompositorManager2()->getFrameCount()) return;
    CardMoverFrame mf;
    if (!mMoverHooks.frame(mf)) return;
    mMovers.swap(mf.movers);

    const Ogre::Light *sunLight = cardSun();
    const Ogre::Vector3 toSun =
        sunLight ? (-sunLight->getDerivedDirection()).normalisedCopy() : Ogre::Vector3::ZERO;
    const bool haveSun = sunLight != nullptr;
    const SunFrame sf = sunFrame(haveSun ? toSun : Ogre::Vector3::UNIT_Y);
    // A card's footprint, computed once per call and only if something asks.
    std::vector<Footprint> cardFp;
    const auto cardFpAt = [&](unsigned i) -> const Footprint & {
        if (cardFp.empty()) {
            cardFp.resize(mCards.size());
            for (size_t k = 0; k < mCards.size(); ++k) cardFp[k] = cardFootprint(sf, mCards[k]);
        }
        return cardFp[i];
    };
    // Captured (its Depth and Normal landed): this frame's batch has, by now.
    const auto landed = [this](const CardRec &c) { return c.lastUpdated != 0ull && !c.queued; };

    // 1. THE STILL CASTERS THAT MOVED — a transform, a show/hide, the caster
    //    bit, a class change (a drag's promotion and demotion, setNodeMovable),
    //    a deletion: their old and new footprints' cards go back on
    //    the capture queue (the captured term is theirs). A queued card that
    //    carries a traced term KEEPS it until the capture lands (step 3 waits
    //    for `landed`): a demoted mover's shadow passes from the trace to the
    //    capture without a frame of neither.
    if (haveSun) {
        for (const CardCasterMove &m : mf.casterMoves) {
            const Footprint o = boxFootprint(sf, m.oldMin, m.oldMax);
            const Footprint n = boxFootprint(sf, m.newMin, m.newMax);
            for (unsigned i = 0; i < mCards.size(); ++i) {
                CardRec &c = mCards[i];
                // Not yet captured, already queued, captured THIS frame (after the
                // scene graph: it holds the caster where it is now), or the
                // caster's own (a moved instance re-allocates its cards, a hidden
                // one gives them back).
                if (c.queued || !c.lastUpdated || c.lastUpdated == mFrame) continue;
                if (mInstances[c.instance].node == m.node) continue;
                const Footprint &cf = cardFpAt(i);
                if (!shades(o, cf) && !shades(n, cf)) continue;
                c.queued = true;
                ++mCasterRecaptures;
            }
        }
    }

    // 2. THE MOVERS' BOXES THAT CHANGED: a moved mover's old and new box, a
    //    mover this cache has not traced yet, one that left (its old box), and
    //    every box when the sun turned (every trace is stale).
    // A card goes pending ONCE: a card still waiting keeps its age.
    const auto pend = [this](CardRec &c) {
        if (c.moverPending) return;
        c.moverPending = true;
        c.moverPendingSince = mFrame;
    };
    std::vector<Footprint> changed;
    std::vector<Footprint> current;
    current.reserve(mMovers.size());
    for (const CardMoverBox &m : mMovers) current.push_back(boxFootprint(sf, m.min, m.max));
    const bool sunTurned = haveSun && (toSun - mMoverSun).squaredLength() > 1e-8f;
    {
        std::unordered_map<NodeId, size_t> index;
        index.reserve(mMovers.size());
        for (size_t i = 0; i < mMovers.size(); ++i) index[mMovers[i].node] = i;
        std::vector<char> moved(mMovers.size(), 0);
        for (NodeId id : mf.moved) {
            auto it = index.find(id);
            if (it != index.end()) moved[it->second] = 1;
        }
        for (size_t i = 0; i < mMovers.size(); ++i) {
            auto last = mMoverLast.find(mMovers[i].node);
            const bool fresh = last == mMoverLast.end();
            if (!(sunTurned || fresh || moved[i])) continue;
            changed.push_back(current[i]);
            if (!fresh) changed.push_back(boxFootprint(sf, last->second.first, last->second.second));
            mMoverLast[mMovers[i].node] = { mMovers[i].min, mMovers[i].max };
        }
        if (mf.moversChanged || sunTurned) {
            for (auto it = mMoverLast.begin(); it != mMoverLast.end();) {
                if (index.find(it->first) == index.end()) {
                    changed.push_back(boxFootprint(sf, it->second.first, it->second.second));
                    it = mMoverLast.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
    // No sun: every trace is void, and the sun's return (even at the same
    // direction) must retrace every footprint.
    mMoverSun = haveSun ? toSun : Ogre::Vector3::ZERO;

    // 3. THE CARDS: inside a changed footprint, or captured this frame (their
    //    Depth is new) — pending; a pending card no CURRENT footprint reaches is
    //    retired instead (its term dropped, relit without it).
    if (!changed.empty()) {
        for (unsigned i = 0; i < mCards.size(); ++i) {
            if (!landed(mCards[i])) continue;
            const Footprint &cf = cardFpAt(i);
            for (const Footprint &f : changed)
                if (shades(f, cf)) { pend(mCards[i]); break; }
        }
    }
    for (unsigned idx : mBatch)
        if (!mMovers.empty() || mCards[idx].moverTraced) pend(mCards[idx]);

    std::vector<unsigned> trace;
    const auto relightDirect = [this](unsigned i) {
        CardRec &c = mCards[i];
        if (std::find(mRelight.begin(), mRelight.end(), i) != mRelight.end()) return;
        if (mRelight.size() >= kMaxRelights) { c.relight = true; return; }
        mRelight.push_back(i);
        mRelightMode.push_back(c.indirectValid ? 0u : 2u);
    };
    const bool canTrace = haveSun && bool(mMoverHooks.trace);
    mMoverPending = 0u;
    mMoverPendingAge = 0u;
    for (unsigned i = 0; i < mCards.size(); ++i) {
        CardRec &c = mCards[i];
        if (!c.moverPending) continue;
        if (!landed(c)) continue;   // waits for its capture
        bool reached = false;
        if (canTrace) {
            const Footprint &cf = cardFpAt(i);
            for (const Footprint &f : current)
                if (shades(f, cf)) { reached = true; break; }
        }
        if (!reached) {
            c.moverPending = false;
            if (c.moverTraced) {
                c.moverTraced = false;
                ++mMoverRetired;
                relightDirect(i);
            }
            continue;
        }
        trace.push_back(i);
    }
    if (trace.empty()) return;

    // 4. OLDEST FIRST, NEAREST AMONG EQUALS, under the budget — the relight's
    //    own number, spent a second time on the movers' term; the rest waits (a
    //    stat). THE AGE IS WHAT BOUNDS THE WAIT: nearest-first alone served the
    //    same nearest cards every frame while movers kept moving (they go
    //    pending again each frame) and a far card's term froze for the whole
    //    motion. With the age, a card pending since frame F is traced before
    //    any card that went pending after F — so every pending card is traced
    //    within ceil(pending cards / cards the budget holds) frames (planRelights'
    //    `oldestFirst`, the same rule for the same reason).
    const Ogre::Vector3 eye = mViewerPos;
    std::sort(trace.begin(), trace.end(), [this, &eye](unsigned a, unsigned b) {
        if (mCards[a].moverPendingSince != mCards[b].moverPendingSince)
            return mCards[a].moverPendingSince < mCards[b].moverPendingSince;
        const float da = (mCards[a].centre - eye).squaredLength();
        const float db = (mCards[b].centre - eye).squaredLength();
        if (da != db) return da < db;
        return a < b;
    });
    std::vector<unsigned> now;
    unsigned spent = 0u;
    const size_t room = mRelight.size() < kMaxRelights ? kMaxRelights - mRelight.size() : 0u;
    for (unsigned idx : trace) {
        const unsigned cost = mCards[idx].size * mCards[idx].size;
        if (!now.empty() && spent + cost > mLightBudget) break;
        if (now.size() >= room) break;
        now.push_back(idx);
        spent += cost;
    }
    mMoverPending = unsigned(trace.size() - now.size());
    // The oldest card left waiting (the list is oldest first): its age in frames.
    const auto ageOf = [this](unsigned idx) {
        return unsigned(std::min<unsigned long long>(mFrame - mCards[idx].moverPendingSince, 0xFFFFFFFFull));
    };
    if (now.size() < trace.size()) mMoverPendingAge = ageOf(trace[now.size()]);
    if (now.empty()) return;

    mMoverCpu.assign(now.size() * kRelightFloats, 0.0f);
    for (size_t i = 0; i < now.size(); ++i) {
        const CardRec &c = mCards[now[i]];
        float *r = &mMoverCpu[i * kRelightFloats];
        const Ogre::Vector3 cam = c.centre + c.d * (c.halfDepth + captureMargin(c.halfDepth));
        r[0] = float(c.atlasX); r[1] = float(c.atlasY); r[2] = float(c.size);
        r[4] = cam.x; r[5] = cam.y; r[6] = cam.z;
        r[8] = c.u.x; r[9] = c.u.y; r[10] = c.u.z; r[11] = std::max(2.0f * c.halfU, 1e-4f);
        r[12] = c.v.x; r[13] = c.v.y; r[14] = c.v.z; r[15] = std::max(2.0f * c.halfV, 1e-4f);
        r[16] = c.d.x; r[17] = c.d.y; r[18] = c.d.z;
    }
    CardMoverTrace job;
    job.records = mMoverCpu.data();
    job.count = unsigned(now.size());
    job.depth = mAtlas[unsigned(CardLayer::Depth)];
    job.normal = mAtlas[unsigned(CardLayer::Normal)];
    job.vis = mMoverVis;
    job.toSun = toSun;
    job.range = kMoverRayRange;
    if (!mMoverHooks.trace(job)) {
        mMoverPending = unsigned(trace.size());   // no structure this frame: next frame
        mMoverPendingAge = ageOf(trace.front());
        return;
    }
    for (unsigned idx : now) {
        CardRec &c = mCards[idx];
        c.moverPending = false;
        c.moverTraced = true;
        ++mMoverTraces;
        ++mMoverTracedLastFrame;
        mMoverTexelsLastFrame += c.size * c.size;
        relightDirect(idx);
    }
}

// ---------------------------------------------------------------------------
// The two GPU tables — what the ray job's card read binds (PHOTON-CARDS-2,
// rq_reflect.comp through jah_rq_card.glsl).
// ---------------------------------------------------------------------------
//
// Rebuilt when the ALLOCATION changes and when a card's two flags do (its
// first capture; its indirect half going stale or being marched) — never per
// capture otherwise. Two buffers of 320 KB and 16 KB.
//
// THE KEY IS FREE: the ray tier writes the scene's own ITEM SLOT into each
// TLAS instance's `instanceCustomIndex` (it reads the GPU scene's table, whose
// index IS that slot), so "which cards belong to this hit" is one indexed
// fetch of `mInstanceBuffer` and no lookup at all.
void SurfaceCache::syncBuffers() {
    if (!mTableDirty) return;
    mTableDirty = false;
    Ogre::VaoManager *vao = Ogre::Root::getSingleton().getRenderSystem()->getVaoManager();
    if (!vao) return;

    // 20 floats a record = the 80 bytes CardGpuRec declares.
    constexpr size_t kCardGpuFloats = sizeof(CardGpuRec) / sizeof(float);
    const unsigned records = std::min(unsigned(mCards.size()), kCardRecordCeiling);
    mCardBufferCpu.assign(size_t(kCardRecordCeiling) * kCardGpuFloats, 0.0f);
    for (unsigned i = 0; i < records; ++i) {
        const CardRec &c = mCards[i];
        CardGpuRec rec = {};
        // u = dot(world, U)/(2 halfU) + (0.5 - dot(centre, U)/(2 halfU)), and
        // the same for v; the card frame is world-space, so there is no
        // per-instance matrix anywhere in the read.
        const float invU = 1.0f / (2.0f * c.halfU), invV = 1.0f / (2.0f * c.halfV);
        rec.rowU[0] = c.u.x * invU; rec.rowU[1] = c.u.y * invU; rec.rowU[2] = c.u.z * invU;
        rec.rowU[3] = 0.5f - c.centre.dotProduct(c.u) * invU;
        rec.rowV[0] = c.v.x * invV; rec.rowV[1] = c.v.y * invV; rec.rowV[2] = c.v.z * invV;
        rec.rowV[3] = 0.5f - c.centre.dotProduct(c.v) * invV;
        // ...and the distance from the card's NEAR PLANE, which is the quantity
        // the depth layer stores and the read depth-tests against.
        const Ogre::Vector3 plane = c.centre + c.d * (c.halfDepth + captureMargin(c.halfDepth));
        rec.rowD[0] = -c.d.x; rec.rowD[1] = -c.d.y; rec.rowD[2] = -c.d.z;
        rec.rowD[3] = plane.dotProduct(c.d);
        rec.axis[0] = c.d.x; rec.axis[1] = c.d.y; rec.axis[2] = c.d.z;
        rec.axis[3] = 2.0f * std::max(c.halfU, c.halfV) / float(c.size);   // the card's texel, metres
        rec.atlas[0] = c.atlasX;
        rec.atlas[1] = c.atlasY;
        rec.atlas[2] = c.size;
        rec.atlas[3] = (c.lastUpdated ? kGpuCardCaptured : 0u) | (c.indirectValid ? kGpuCardLit : 0u);
        std::memcpy(&mCardBufferCpu[size_t(i) * kCardGpuFloats], &rec, sizeof(rec));
    }
    mCardRecords = records;

    // THE INSTANCE TABLE, indexed by the item slot. A slot with no cards reads
    // (0, 0) and the shader takes the voxel path it always took — which is why
    // the whole table is zeroed first and only the resident instances write.
    unsigned maxSlot = 0u;
    for (const InstanceRec &inst : mInstances)
        if (inst.cardCount && inst.itemSlot != size_t(-1))
            maxSlot = std::max(maxSlot, unsigned(inst.itemSlot));
    unsigned slots = std::max(kInstanceSlotsInitial, mInstanceSlots);
    while (maxSlot + 1u > slots) slots *= 2u;
    if (slots != mInstanceSlots && mInstanceBuffer) {
        vao->destroyUavBuffer(mInstanceBuffer);
        mInstanceBuffer = nullptr;
    }
    mInstanceSlots = slots;
    mInstanceBufferCpu.assign(size_t(slots) * 4u, 0u);
    for (const InstanceRec &inst : mInstances) {
        if (!inst.cardCount || inst.itemSlot == size_t(-1)) continue;
        unsigned *e = &mInstanceBufferCpu[size_t(inst.itemSlot) * 4u];
        e[0] = inst.firstCard;
        e[1] = inst.cardCount;
    }

    if (!mCardBuffer)
        mCardBuffer = vao->createUavBuffer(kCardRecordCeiling, sizeof(CardGpuRec), 0,
                                           mCardBufferCpu.data(), false);
    else
        mCardBuffer->upload(mCardBufferCpu.data(), 0, kCardRecordCeiling);
    if (!mInstanceBuffer)
        mInstanceBuffer = vao->createUavBuffer(slots, 4u * sizeof(unsigned), 0,
                                               mInstanceBufferCpu.data(), false);
    else
        mInstanceBuffer->upload(mInstanceBufferCpu.data(), 0, slots);
}

void SurfaceCache::update(const CardSceneView &view) {
    if (!mBuilt) return;
    ++mFrame;
    mCapturesLastFrame = 0u;
    mTexelsLastFrame = 0u;
    mCaptureMs = 0.0f;
    mWsMs = 0.0f;
    mCopyMs = 0.0f;
    mRelitLastFrame = 0u;
    mRelitTexelsLastFrame = 0u;
    mIndirectLastFrame = 0u;
    mIndirectTexelsLastFrame = 0u;
    mLightMs = 0.0f;
    mMoverTracedLastFrame = 0u;
    mMoverTexelsLastFrame = 0u;
    mViewerPos = view.viewerPos;
    mBudget = view.budgetTexels;
    mLightBudget = view.lightBudgetTexels;
    mIndirectBudget = view.indirectBudgetTexels;
    mRadius = view.radius;

    refreshResidency(view);
    // THE GPU TABLES follow the ALLOCATION and not the captures: a re-captured
    // card lands in the same rect, so nothing here moves when the queue drains.
    syncBuffers();

    // THE LIGHT SIGNATURE. A light write is a change to what every card
    // RECORDS and to no card's RECTANGLE, so it throws every resident card back
    // on the queue and frees nothing — the cheap half of the invalidation.
    // (A material edit takes the precise door instead: `noteMaterialChanged`,
    // which queues only the instances wearing it.)
    //
    // WHAT A FINER LIGHT RULE WOULD BE, since the brief asks: "recapture only
    // the cards whose SHADOW moved" needs the shadow atlas's own per-lamp dirty
    // set projected onto each card's box — the lamp-map cache has exactly that
    // information (ENGINE_CACHE_POLICY_SPEC P2-P5) and phase 3, which owns the
    // light list, is where it belongs. Measured here: a sun tilt on the shadow
    // fixture re-captures every resident card, which at the tier's budget is
    // two frames for one instance and is not worth a second dirty set yet.
    if (view.lightSerial != mLightSerial) {
        for (CardRec &c : mCards) c.queued = true;
        // ONE PER GESTURE, NOT ONE PER FRAME — `gi.material_swap`'s model. A
        // dragged lamp writes a new pose on every frame of the drag and every
        // one of them genuinely stales the shadow term, so the QUEUEING is per
        // frame and cannot be otherwise; what a counter is for is telling a
        // drag from a defect, and a number that climbs by sixty for one gesture
        // cannot. So the counter moves on the LEADING EDGE: the first frame
        // whose signature differs after a frame whose signature did not.
        if (!mLightMovingLastFrame) ++mInvalidLight;
        mLightMovingLastFrame = true;
        mLightSerial = view.lightSerial;
    } else {
        mLightMovingLastFrame = false;
    }

    // THE QUEUE, IN LUMEN'S ORDER: priority = lastUsed - lastUpdated, drained
    // oldest first. A card that has never been captured has `lastUpdated` 0 and
    // therefore the highest priority there is, which is what makes a new
    // instance's cards arrive before an old one's are refreshed.
    mQueue.clear();
    for (unsigned i = 0; i < mCards.size(); ++i) {
        mCards[i].lastUsed = mFrame;
        if (mCards[i].queued) mQueue.push_back(i);
    }
    std::sort(mQueue.begin(), mQueue.end(), [this](unsigned a, unsigned b) {
        const unsigned long long pa = mCards[a].lastUsed - mCards[a].lastUpdated;
        const unsigned long long pb = mCards[b].lastUsed - mCards[b].lastUpdated;
        if (pa != pb) return pa > pb;
        return a < b;
    });

    // ...AND THE BUDGET IS SPENT IN TEXELS, never in milliseconds (a wall clock
    // measures nothing in this engine) and never in cards (a 16-texel card and
    // a 128-texel card are not the same work on the GPU or on the bus). A card
    // whose texels do not fit ENDS the frame's draining rather than being
    // skipped, so the priority order is honoured exactly and the budget is a
    // ceiling the frame never goes over — and so is the batch: one workspace
    // update carries at most `kCaptureBatch` cards.
    //
    // THIS PLANS; Ogre's frame executes (the workspace is enabled and first in
    // the manager's list — makeWorkspace), and `workspacePosUpdate` marks the
    // cards captured once their copies are recorded.
    mBatch.clear();
    unsigned spent = 0u;
    for (unsigned idx : mQueue) {
        const unsigned cost = mCards[idx].size * mCards[idx].size;
        if (spent && spent + cost > view.budgetTexels) break;
        if (mBatch.size() >= kCaptureBatch) break;
        aimCamera(mCards[idx], unsigned(mBatch.size()));
        mBatch.push_back(idx);
        spent += cost;
        if (spent >= view.budgetTexels) break;
    }
    mBatchFrame = Ogre::Root::getSingleton().getCompositorManager2()->getFrameCount();
    mWs->setExecutionMask(static_cast<Ogre::uint8>((1u << mBatch.size()) - 1u));
    planRelights(view);
}

void SurfaceCache::noteMaterialChanged(MaterialId material) {
    bool any = false;
    for (const InstanceRec &inst : mInstances) {
        if (!inst.cardCount || inst.material != material) continue;
        for (unsigned c = 0; c < inst.cardCount; ++c)
            mCards[inst.firstCard + c].queued = mCards[inst.firstCard + c].surfaceStale = true;
        any = true;
    }
    if (any) ++mInvalidMaterial;
}

// ---------------------------------------------------------------------------
// What it publishes
// ---------------------------------------------------------------------------
void SurfaceCache::fillStatus(CardCacheStatus &out) const {
    out = CardCacheStatus();
    out.built = mBuilt;
    if (!mBuilt) return;
    out.pageSize = kCardPageSize;
    out.pages = unsigned(mPageOwner.size());
    out.pagesUsed = mPagesUsed;
    out.bytesPerTexel = 0u;
    for (unsigned i = 0; i < kCardLayers; ++i)
        if (mAtlas[i])
            out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mAtlas[i]->getPixelFormat());
    unsigned long long bytes = 0ull;
    for (unsigned i = 0; i < kCardLayers; ++i) {
        bytes += bytesOf(mAtlas[i]);
        bytes += bytesOf(mScratch[i]);
    }
    bytes += bytesOf(mScratchDepth);
    bytes += bytesOf(mRadiance);
    bytes += bytesOf(mIndirect);
    bytes += bytesOf(mMoverVis);
    if (mRadiance)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mRadiance->getPixelFormat());
    if (mIndirect)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mIndirect->getPixelFormat());
    if (mMoverVis)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mMoverVis->getPixelFormat());
    out.bytes = bytes;
    out.emissiveFormat = mEmissiveFormatName;
    out.radianceFormat = mRadianceFormatName;
    out.lightBudgetTexels = mLightBudget;
    out.relitLastFrame = mRelitLastFrame;
    out.relitTexelsLastFrame = mRelitTexelsLastFrame;
    out.relights = mRelights;
    out.invalidRadiance = mInvalidRadiance;
    out.lightsDropped = mLightsDropped;
    out.indirectBudgetTexels = mIndirectBudget;
    out.indirectLastFrame = mIndirectLastFrame;
    out.indirectTexelsLastFrame = mIndirectTexelsLastFrame;
    out.indirectRelights = mIndirectRelights;
    out.invalidIndirect = mInvalidIndirect;
    out.indirectOn = mIndirectOnLastRelight;
    out.lightMs = mLightMs;
    out.instancesResident = 0u;
    for (const InstanceRec &i : mInstances) out.instancesResident += i.cardCount ? 1u : 0u;
    out.cardsResident = unsigned(mCards.size());
    out.residencyRadius = mRadius;
    out.queueLength = 0u;
    for (const CardRec &c : mCards) out.queueLength += c.queued ? 1u : 0u;
    out.budgetTexels = mBudget;
    out.capturesLastFrame = mCapturesLastFrame;
    out.texelsLastFrame = mTexelsLastFrame;
    out.captures = mCaptures;
    out.invalidTransform = mInvalidTransform;
    out.invalidMaterial = mInvalidMaterial;
    out.invalidLight = mInvalidLight;
    out.captureMs = mCaptureMs;
    out.captureWorkspaceMs = mWsMs;
    out.captureCopyMs = mCopyMs;
    out.cardRecords = mCardRecords;
    out.instanceSlots = mInstanceSlots;
    out.moverCasters = unsigned(mMovers.size());
    out.moverTracedLastFrame = mMoverTracedLastFrame;
    out.moverTexelsLastFrame = mMoverTexelsLastFrame;
    out.moverTraces = mMoverTraces;
    out.moverRetired = mMoverRetired;
    out.moverPending = mMoverPending;
    out.moverPendingAge = mMoverPendingAge;
    out.casterRecaptures = mCasterRecaptures;
    if (mMoverHooks.readTimes) mMoverHooks.readTimes(out.moverGpuMs, out.relightGpuMs);
}

long SurfaceCache::itemSlotOf(NodeId node) const {
    auto it = mByNode.find(node);
    if (it == mByNode.end()) return -1;
    const InstanceRec &inst = mInstances[it->second];
    if (!inst.cardCount || inst.itemSlot == size_t(-1)) return -1;
    return long(inst.itemSlot);
}

bool SurfaceCache::readTexel(NodeId node, unsigned card, float u, float v,
                             CardSample &out) const {
    out = CardSample();
    if (!mBuilt) return false;
    auto it = mByNode.find(node);
    if (it == mByNode.end()) return false;
    const InstanceRec &inst = mInstances[it->second];
    if (card >= inst.cardCount) return false;
    return sampleCard(mCards[inst.firstCard + card], u, v, out);
}

bool SurfaceCache::sampleCard(const CardRec &rec, float u, float v, CardSample &out) const {
    out = CardSample();
    if (!mBuilt || !rec.size) return false;

    // THE ATLAS TEXEL A CARD PARAMETER LANDS ON. u runs with the card's own u
    // axis; v runs with its v axis, which the capture camera's +Y is — so image
    // ROW 0 is the +v side and the v parameter is flipped here, once, in the
    // one place that turns a card parameter into an atlas texel.
    const float cu = std::min(std::max(u, 0.0f), 1.0f);
    const float cv = std::min(std::max(v, 0.0f), 1.0f);
    const unsigned tx = rec.atlasX + std::min(rec.size - 1u, unsigned(cu * float(rec.size)));
    const unsigned ty = rec.atlasY + std::min(rec.size - 1u, unsigned((1.0f - cv) * float(rec.size)));

    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    // A TICKET OVER A TARGET THE OPEN COMMAND BUFFER HAS ONLY RECORDED INTO
    // READS RECYCLED VRAM (CLAUDE.md, the sky/IBL facts). The copies that wrote
    // this texel are in that buffer.
    rs->flushCommands();
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    float vals[kCardLayers][4] = {};
    for (unsigned i = 0; i < kCardLayers; ++i) {
        if (!mAtlas[i]) return false;
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            1u, 1u, 1u, Ogre::TextureTypes::Type2D, mAtlas[i]->getPixelFormat());
        Ogre::TextureBox src = mAtlas[i]->getEmptyBox(0);
        src.x = tx; src.y = ty; src.width = 1u; src.height = 1u;
        tk->download(mAtlas[i], 0, true, &src, true);
        const Ogre::TextureBox box = tk->map(0);
        decodeTexel(mAtlas[i]->getPixelFormat(), box.at(0, 0, 0), vals[i]);
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
    }
    // ...and the SIXTH layer, the lit card's radiance.
    float rad[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (mRadiance) {
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            1u, 1u, 1u, Ogre::TextureTypes::Type2D, mRadiance->getPixelFormat());
        Ogre::TextureBox src = mRadiance->getEmptyBox(0);
        src.x = tx; src.y = ty; src.width = 1u; src.height = 1u;
        tk->download(mRadiance, 0, true, &src, true);
        const Ogre::TextureBox box = tk->map(0);
        decodeTexel(mRadiance->getPixelFormat(), box.at(0, 0, 0), rad);
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
    }
    float ind[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (mIndirect && rec.indirectValid) {
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            1u, 1u, 1u, Ogre::TextureTypes::Type2D, mIndirect->getPixelFormat());
        Ogre::TextureBox src = mIndirect->getEmptyBox(0);
        src.x = tx; src.y = ty; src.width = 1u; src.height = 1u;
        tk->download(mIndirect, 0, true, &src, true);
        const Ogre::TextureBox box = tk->map(0);
        decodeTexel(mIndirect->getPixelFormat(), box.at(0, 0, 0), ind);
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
    }
    for (int k = 0; k < 3; ++k) {
        out.radiance[k] = rad[k];
        out.indirect[k] = ind[k];
        out.albedo[k] = vals[unsigned(CardLayer::Albedo)][k];
        // The normal is stored *0.5+0.5, as the prepass writes it.
        out.normal[k] = vals[unsigned(CardLayer::Normal)][k] * 2.0f - 1.0f;
        out.emissive[k] = vals[unsigned(CardLayer::Emissive)][k];
    }
    out.depth = vals[unsigned(CardLayer::Depth)][0];
    out.shadow = vals[unsigned(CardLayer::ShadowRough)][0];
    out.roughness = vals[unsigned(CardLayer::ShadowRough)][1];
    out.texelX = tx;
    out.texelY = ty;
    out.lit = rec.indirectValid;
    out.ok = true;
    return true;
}

bool SurfaceCache::readAt(const Ogre::Vector3 &world, const Ogre::Vector3 &normal,
                          CardSample &out, NodeId onlyNode) const {
    out = CardSample();
    if (!mBuilt) return false;
    Ogre::Vector3 n = normal;
    if (n.squaredLength() < 1e-12f) return false;
    n.normalise();
    // ONE INSTANCE'S CARDS when asked — the ray read's own scope (it knows the
    // hit instance), so the parity suite compares like with like.
    unsigned first = 0u, count = unsigned(mCards.size());
    if (onlyNode) {
        auto it = mByNode.find(onlyNode);
        if (it == mByNode.end()) return false;
        first = mInstances[it->second].firstCard;
        count = mInstances[it->second].cardCount;
    }

    const CardRec *best = nullptr;
    float bestFacing = 0.05f;   // a card edge-on to the surface says nothing
    float bestU = 0.0f, bestV = 0.0f;
    for (unsigned ci = first; ci < first + count; ++ci) {
        const CardRec &c = mCards[ci];
        if (!c.size || !c.lastUpdated) continue;          // never captured
        const float facing = c.d.dotProduct(n);
        if (facing <= bestFacing) continue;
        const Ogre::Vector3 rel = world - c.centre;
        const float du = rel.dotProduct(c.u), dv = rel.dotProduct(c.v);
        if (std::fabs(du) > c.halfU || std::fabs(dv) > c.halfV) continue;
        const float u = du / (2.0f * c.halfU) + 0.5f;
        const float v = dv / (2.0f * c.halfV) + 0.5f;
        // THE DEPTH TEST, which is the whole difference between a card and a
        // decal: the capture stored the distance from the card's near plane to
        // the FIRST surface along the axis, so a point further away than that
        // by more than a couple of texels is hidden behind something and this
        // card does not describe it.
        CardSample s;
        if (!sampleCard(c, u, v, s)) continue;
        if (s.depth <= 0.0f) continue;                    // the clear: nothing captured here
        const float margin = captureMargin(c.halfDepth);
        const float planeToPoint = (c.centre + c.d * (c.halfDepth + margin) - world).dotProduct(c.d);
        const float texel = 2.0f * std::max(c.halfU, c.halfV) / float(c.size);
        if (std::fabs(planeToPoint - s.depth) > 2.0f * texel + 1e-3f) continue;
        best = &c;
        bestFacing = facing;
        bestU = u;
        bestV = v;
    }
    if (!best) return false;
    if (!sampleCard(*best, bestU, bestV, out)) return false;
    out.card = int(best - mCards.data());
    return true;
}

bool SurfaceCache::dump(const std::string &prefix, std::string &err) const {
    if (!mBuilt) { err = "no surface cache"; return false; }
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    rs->flushCommands();
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    // The five captured layers and the sixth, the radiance.
    const char *name[kCardLayers + 1] = { "albedo", "normal", "depth", "emissive", "shadowrough",
                                          "radiance" };
    // `scale` turns a layer into something an eye can read: the depth is metres
    // and the emissive and the radiance are radiance, so they are divided by a
    // stated number rather than clipped silently.
    const float scale[kCardLayers + 1] = { 1.0f, 1.0f, 0.2f, 0.25f, 1.0f, 0.25f };
    std::vector<unsigned char> rgba(size_t(kCardAtlasSize) * kCardAtlasSize * 4u);
    for (unsigned i = 0; i < kCardLayers + 1u; ++i) {
        Ogre::TextureGpu *layer = i < kCardLayers ? mAtlas[i] : mRadiance;
        if (!layer) continue;
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            kCardAtlasSize, kCardAtlasSize, 1u, Ogre::TextureTypes::Type2D,
            layer->getPixelFormat());
        tk->download(layer, 0, true, nullptr, true);
        const Ogre::TextureBox box = tk->map(0);
        for (unsigned y = 0; y < kCardAtlasSize; ++y)
            for (unsigned x = 0; x < kCardAtlasSize; ++x) {
                float px[4];
                decodeTexel(layer->getPixelFormat(), box.at(x, y, 0), px);
                const float vv[4] = { px[0] * scale[i], px[1] * scale[i], px[2] * scale[i], 1.0f };
                unsigned char *o = &rgba[(size_t(y) * kCardAtlasSize + x) * 4u];
                for (int k = 0; k < 4; ++k)
                    o[k] = (unsigned char)(std::min(std::max(vv[k], 0.0f), 1.0f) * 255.0f + 0.5f);
            }
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
        Ogre::Image2 img;
        img.loadDynamicImage(rgba.data(), kCardAtlasSize, kCardAtlasSize, 1u,
                             Ogre::TextureTypes::Type2D, Ogre::PFG_RGBA8_UNORM, false, 1u);
        JAH_TRY { img.save(prefix + "-" + name[i] + ".png", 0, 1u); }
        catch (const Ogre::Exception &) {}
    }
    (void)err;
    return true;
}


// ---------------------------------------------------------------------------
// The two free functions EnginePrivate.h declares, so that no TU but this one
// needs the Component's definition (the same arrangement the ray tier has).
// ---------------------------------------------------------------------------
namespace detail {
bool surfaceCardsCapturing() { return SurfaceCache::capturing(); }
Ogre::CompositorWorkspace *surfaceCacheWorkspace(const SurfaceCache *cache) {
    return cache ? cache->workspace() : nullptr;
}
}   // namespace detail

}   // namespace engine
}   // namespace jahshaka
