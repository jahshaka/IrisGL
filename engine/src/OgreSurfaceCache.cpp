// SURFACE-CACHE phase 2 — the capture Component's implementation.
// The design, the two condemned shapes it replaces and the renderArea evidence
// behind the scratch-and-copy route are all in SurfaceCache.h; this file is the
// mechanism.
#include "EnginePrivate.h"
#include "SurfaceCache.h"

#include <OgreCamera.h>
#include <OgreItem.h>
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
/// Render-thread only, exactly like `FogHlmsListener`'s own maps: the capture
/// workspace is driven from the one thread that renders and the flag is set and
/// cleared around one `_update()`.
bool gCapturing = false;

/// ...AND IT IS CLEARED ON EVERY PATH OUT, WHICH IS NOT TIDINESS (the rule
/// SURFACE-CACHE-0 learned and this Component inherits). A capture compiles a
/// shader permutation and an Hlms compile failure THROWS through `_update()`.
/// A flag left set by that throw is not a lost capture, it is a BROKEN PROCESS:
/// `jah_card_capture` would then hold for every later pass, every pass would
/// generate the capture permutation, and that permutation's
/// `custom_ps_posExecution` collides at library-parse time with the fog piece's
/// definition of the same name — the "already defined" parse error whose only
/// symptom is the whole scene rendering BLACK with nothing in any log.
struct CaptureFlag {
    CaptureFlag() { gCapturing = true; }
    ~CaptureFlag() { gCapturing = false; }
    CaptureFlag(const CaptureFlag &) = delete;
    CaptureFlag &operator=(const CaptureFlag &) = delete;
};

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

/// ONE CARD AS THE GPU WILL READ IT (phase 4). std430, 48 bytes: the atlas rect
/// in UV with the HALF-TEXEL INSET already applied, the world frame's three
/// rows, and the instance key. Built here because the layout is a contract
/// between this Component and the ray job, and a contract written in two places
/// is a contract that drifts.
struct CardGpuRec {
    /// atlasUV = uv * scale + bias, with the half-texel inset AND v's mirror
    /// already in them (scale.y is negative — see `syncBuffers`).
    float uvScaleBias[4] = {};
    float rowU[4] = {};          ///< dot(world, xyz) + w  ->  the card's u in [0,1]
    float rowV[4] = {};
    float rowD[4] = {};          ///< ...and the distance from the card's near plane
    float axis[4] = {};          ///< xyz = the outward world axis, w = the card's texel, metres
    unsigned key[4] = {};        ///< x = the item slot (instanceCustomIndex), y..w reserved
};
static_assert(sizeof(CardGpuRec) == 96u, "the card record's layout is a contract with phase 4");

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
        // cannot convert, which is why the fallback above moves both).
        Ogre::TextureGpu *s =
            tm->createTexture(processUniqueName((std::string(name[i]) + "Scr").c_str()),
                              Ogre::GpuPageOutStrategy::Discard,
                              Ogre::TextureFlags::RenderToTexture, Ogre::TextureTypes::Type2D);
        s->setResolution(kCardPageSize, kCardPageSize, 1u);
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

    const unsigned pagesPerSide = kCardAtlasSize / kCardPageSize;
    const size_t pages = size_t(pagesPerSide) * pagesPerSide;
    mPageOwner.assign(pages, 0u);
    mPageSubSize.assign(pages, 0u);
    mPageSubUsed.assign(pages, 0u);
    mPageSubMask.assign(pages, 0ull);
    (void)err;
    return true;
}

bool SurfaceCache::makeScratch(std::string &) { return true; }

bool SurfaceCache::makeWorkspace(std::string &err) {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    Ogre::SceneManager *sm = mSceneMgr;
    if (!sm) { err = "surface cache: the scene has no SceneManager"; return false; }

    // THE CAPTURE CAMERA LIVES IN THE SCENE BEING CACHED — one camera, re-aimed
    // per card, because a camera is a node in the manager's graph and six
    // hundred of them would be six hundred nodes walked every frame. That is
    // the same reasoning that took the SCRATCH SceneManager out.
    // TWO CAPTURE CAMERAS, USED ALTERNATELY, AND THAT IS THE SHADOW FIX.
    //
    // The pin's shadow node caches per (camera, compositor-manager frame
    // count): `buildClosestLightList` early-outs on
    // `mLastCamera == newCamera && mLastFrame == currentFrameCount`
    // (OgreCompositorShadowNode.cpp:342-352) and the casters box it computes
    // there is cached with it (:454) — and a workspace driven BY HAND does not
    // bump the manager's frame count, so with ONE camera every card after the
    // frame's first reused the first card's light list AND its casters box,
    // which is a box fitted to a different metre of the world under a mask
    // that admits one object. Measured: the floor's shadow profile was exactly
    // right when the budget captured everything in the first frame and a flat
    // 1.0 at any budget that spread the captures.
    //
    // Alternating two cameras defeats the early-out on its first term, with no
    // patch, no wide cull camera and no fit that is anybody's guess: every card
    // gets its OWN light list and its OWN casters box, fitted to its own
    // capture camera, which is the highest-quality answer available and the one
    // the per-card recalculation was always meant to give.
    for (unsigned i = 0; i < kCaptureCameras; ++i) {
        mCam[i] = sm->createCamera(
            processUniqueName(i ? "cardCaptureB" : "cardCaptureA"), true, true);
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

    Ogre::RenderTargetViewDef *rtv = n->addRenderTextureView("cardRtv");
    for (unsigned i = 0; i < kCardLayers; ++i) {
        Ogre::RenderTargetViewEntry e;
        e.textureName = chan[i];
        rtv->colourAttachments.push_back(e);
    }
    rtv->depthAttachment.textureName = chan[kCardLayers];
    rtv->stencilAttachment.textureName = chan[kCardLayers];
    rtv->preferDepthTexture = true;

    n->setNumTargetPass(1u);
    Ogre::CompositorTargetDef *t = n->addTargetPass("cardRtv");
    t->setNumPasses(1u);
    auto *p = static_cast<Ogre::CompositorPassSceneDef *>(t->addPass(Ogre::PASS_SCENE));
    // THE PREPASS IS THE CAPTURE. `Ogre::PrePassCreate` writes the shading
    // normal and (the shadow term, the GGX alpha) exactly, computes no lighting
    // at all — which is what makes a capture cheap — and
    // JahCardCapture_piece_ps.any adds albedo, emissive and the card's own
    // depth through three hook pieces under one pass property. No patch to the
    // pin (SURFACE-CACHE-0 proved it; the piece's header states the mechanism).
    p->mPrePassMode = Ogre::PrePassCreate;
    // The pass's camera is set per capture (`aimCamera` swaps it), so the
      // definition names the first of the pair and the live pass is re-pointed.
    p->mCameraName = Ogre::IdString(mCam[0]->getName());
    // ONE OBJECT, THROUGH AN INCLUDE CHANNEL. Ogre's visibility test is
    // any-bit-set, so "draw only this item" is expressible only as a bit the
    // item alone carries while the pass runs — `kCardSubjectBit`, granted by
    // captureCard() and taken away again before the pass's own `_update`
    // returns. The mask is a single LOW bit, so `cullFrustum`'s second term
    // (`viewportMask & ~RESERVED_VISIBILITY_FLAGS`) is zero and nothing leaks
    // through it (CLAUDE.md's RESERVED_VISIBILITY_FLAGS rule).
    p->mVisibilityMask = detail::kCardSubjectBit;
    // AND A LIGHT MUST STILL REACH THIS PASS, which it does today for a reason
    // worth writing down rather than relying on: `buildClosestLightList` culls
    // the pass's lights through the VIEWPORT's visibility mask, which is this
    // one — and every light in this engine is born with Ogre's all-bits
    // default and nothing ever narrows it. The day something does, a light
    // without bit 10 drops out of every capture and EVERY CARD'S SHADOW TERM
    // GOES TO 1.0, silently, with no counter moving. If light visibility ever
    // becomes a channel here, this mask grows the lights' bits with it.
    // THE SHADOW NODE IS THE SCENE'S, AND THAT IS THE POINT OF THIS PHASE. A
    // shadow node draws its casters through its own definition's mask
    // (`shadowCasterChannels`), which the pass mask above does not touch — so
    // the whole still world casts into the atlas while exactly one object is
    // shaded out of it, and a card's shadow term becomes occlusion by OTHER
    // objects instead of the prepass's constant 1.0.
    //
    // THE PROBE NODE, not the view's: a CompositorShadowNode is per WORKSPACE,
    // so this workspace allocates one of whatever it names. The view's atlas is
    // 2048x7168 D32 = 56 MB at the High tier; the probe node is a quarter of
    // that (3.5-5.5 MB) with the same PSSM + focused layout, and its caster
    // channel is the STILL world, which is exactly what a card is.
    const bool haveShadowNode =
        cm->hasShadowNodeDefinition(detail::OgreView::kProbeShadowNodeName);
    if (haveShadowNode)
        p->mShadowNode = Ogre::IdString(detail::OgreView::kProbeShadowNodeName);
    // The same switch the GI arm logs under (JAHSHAKA_GI_DEBUG), read once.
    static const bool debugLog = std::getenv("JAHSHAKA_GI_DEBUG") != nullptr;
    if (debugLog)
        Ogre::LogManager::getSingleton().logMessage(
            std::string("Jahshaka cards: capture shadow node ") +
            (haveShadowNode ? detail::OgreView::kProbeShadowNodeName : "NONE (a card's shadow"
                                                                      " term will be the"
                                                                      " prepass constant)"));
    p->mShadowNodeRecalculation = Ogre::SHADOW_NODE_RECALCULATE;
    // ...AND THE FIT IS SHARED BY EVERY CARD CAPTURED IN ONE FRAME, which is a
    // MEASURED limitation and not a choice. `CompositorShadowNode::_update`
    // early-outs on "same camera, same workspace frame count"
    // (OgreCompositorShadowNode.cpp:345), and this Component drives one
    // workspace with one camera — so the FIRST card of a frame fits the atlas
    // to its own box and every later card of that frame samples that fit.
    //
    // WHAT IT COSTS, measured on `gi.card_shadow`'s fixture: with a budget that
    // captures the whole resident set in one frame the profile across the
    // floor is exactly the crate's footprint (1.00 / 0.57 / 0.00 / 0.52 / 1.00
    // at x = 0/+1/+2/+3/+4); with the shipped three-cards-a-frame budget, the
    // cards captured after the frame's first read a flat 1.0 wherever the
    // first card's fit does not reach them.
    //
    // WHAT THE FIX IS, and why it is not here: a cull camera of its own, wide
    // enough to cover the residency region, so one fit serves every card of
    // every frame (the pin fits the node to the pass's CULL camera,
    // CompositorPassScene.cpp:259, and `mCullCameraName` is a definition field
    // that exists for exactly this). This lane BUILT that and measured it
    // producing a flat 1.0 in both the wide and the per-card arm — something
    // else about a separate cull camera is in the way — so it is handed over
    // named and reproducible rather than shipped half-understood. PHASE 3 owns
    // the light list and is where a card's lighting stops being the capture's
    // business anyway.
    p->setAllClearColours(Ogre::ColourValue(0.0f, 0.0f, 0.0f, 0.0f));
    p->setAllLoadActions(Ogre::LoadAction::Clear);
    for (unsigned i = 0; i < kCardLayers; ++i) p->mStoreActionColour[i] = Ogre::StoreAction::Store;
    p->mStoreActionDepth = Ogre::StoreAction::DontCare;
    p->mStoreActionStencil = Ogre::StoreAction::DontCare;
    p->mFirstRQ = 0u;
    p->mLastRQ = 200u;
    p->mIncludeOverlays = false;
    // THE LOD LISTS ARE NOT RE-DERIVED BY THIS PASS. The level a card is
    // captured at is the card's OWN (`MeshCardDesc::lodLevel`, re-derived
    // against the card's real texel), written straight onto the Item through
    // patch 0085's setter — so a pass that recomputed LOD from this ortho
    // camera would immediately undo it, and the view's own pass restores the
    // view's level the same frame because ITS `mUpdateLodLists` is true.
    p->mUpdateLodLists = false;
    p->mProfilingId = "Jahshaka card capture";

    Ogre::CompositorWorkspaceDef *wd = cm->addWorkspaceDefinition(mWsDef);
    for (unsigned i = 0; i < kCardLayers + 1u; ++i) wd->connectExternal(i, mNodeDef, i);
    Ogre::CompositorChannelVec externals;
    for (unsigned i = 0; i < kCardLayers; ++i)
        externals.push_back(mScratch[unsigned(kShaderOrder[i])]);
    externals.push_back(mScratchDepth);
    // DISABLED: this workspace is never part of Ogre's own frame loop. It is
    // driven by hand from the scene's per-frame pass — inside renderOneFrame,
    // where the monitor's listeners are already attached and where every other
    // engine cache spends its budget — because six cards need six different
    // camera poses and one workspace update cannot carry six.
    mWs = cm->addWorkspace(sm, externals, mCam[0], mWsDef, false);
    if (!mWs) { err = "surface cache: addWorkspace failed"; return false; }
    mWs->addListener(this);
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
    if (mWs) { cm->removeWorkspace(mWs); mWs = nullptr; }
    if (!mWsDef.empty() && cm->hasWorkspaceDefinition(mWsDef)) cm->removeWorkspaceDefinition(mWsDef);
    if (!mNodeDef.empty() && cm->hasNodeDefinition(mNodeDef)) cm->removeNodeDefinition(mNodeDef);
    mWsDef.clear();
    mNodeDef.clear();
    if (Ogre::VaoManager *vao = root->getRenderSystem()
                                    ? root->getRenderSystem()->getVaoManager()
                                    : nullptr) {
        if (mCardBuffer) { vao->destroyUavBuffer(mCardBuffer); mCardBuffer = nullptr; }
        if (mInstanceBuffer) { vao->destroyUavBuffer(mInstanceBuffer); mInstanceBuffer = nullptr; }
    }
    mCardRecords = 0u;
    mInstanceSlots = 0u;
    mCardBufferCpu.clear();
    mInstanceBufferCpu.clear();
    mTableDirty = false;
    for (unsigned i = 0; i < kCaptureCameras; ++i) {
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
                // THE LOD LEVEL, RE-DERIVED AGAINST THE CARD'S REAL TEXEL —
                // not taken from the bake's `lodLevel`, which was computed
                // against a nominal 128. The rule is ATOM's own, stated once:
                // the coarsest level whose baked error is below the deviation
                // this consumer can afford, and a capture can afford one of its
                // own texels. The baked value is the fallback when the engine
                // holds no error list for the mesh.
                const float texel = 2.0f * std::max(r.halfU, r.halfV) / float(r.size);
                r.lodLevel = c.lodLevel;
                if (cand.lodErrors && !cand.lodErrors->empty())
                    r.lodLevel = static_cast<unsigned char>(
                        lodLevelForWorldError(*cand.lodErrors, texel, cand.lodErrors->size()));
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
                mCards[inst.firstCard + c].queued = true;
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
void SurfaceCache::aimCamera(const CardRec &card) {
    const float margin = captureMargin(card.halfDepth);
    // ALTERNATE (see the pair's note in makeWorkspace): the shadow node's
    // "same camera, same frame" early-out is what made cards 2..N of a frame
    // reuse the first one's light list and casters box.
    mCamTurn ^= 1u;
    Ogre::Camera *cam = mCam[mCamTurn];
    cam->setOrthoWindow(std::max(2.0f * card.halfU, 1e-4f), std::max(2.0f * card.halfV, 1e-4f));
    cam->setNearClipDistance(0.001f);
    cam->setFarClipDistance(2.0f * card.halfDepth + 2.0f * margin + 0.01f);
    cam->setPosition(card.centre + card.d * (card.halfDepth + margin));
    // Ogre looks down -Z, so the card's OUTWARD axis is the camera's +Z. The
    // frame is right-handed (u x v = d, asserted by gi.card_capture), so
    // FromAxes builds a rotation and not a reflection — which is exactly why
    // the document's +Y row had to be fixed before this line could be written.
    Ogre::Quaternion q;
    q.FromAxes(card.u, card.v, card.d);
    cam->setOrientation(q);
    // ...and the live pass is re-pointed at it. The camera is resolved in the
    // pass's constructor from the definition's name, so a swap has to go
    // through the pass object and not through the definition.
    if (Ogre::CompositorNode *node =
            mWs->getNodeSequence().empty() ? nullptr : mWs->getNodeSequence().front()) {
        const Ogre::CompositorPassVec &passes = node->_getPasses();
        if (!passes.empty() && passes[0]->getType() == Ogre::PASS_SCENE) {
            auto *sp = static_cast<Ogre::CompositorPassScene *>(passes[0]);
            // BOTH, and the cull one is the load-bearing half: the pass's
            // constructor sets `mCullCamera = mCamera` when the definition
            // names no cull camera, and it is the CULL camera the shadow node
            // is fitted to (`CompositorPassScene.cpp:259`). Setting only the
            // render camera would leave every card's shadow fitted to camera A
            // for ever, which is the defect this pair exists to remove.
            sp->_setCustomCamera(cam);
            sp->_setCustomCullCamera(cam);
        }
    }
}

void SurfaceCache::captureCard(CardRec &card) {
    if (!mWs || card.instance >= mInstances.size()) return;
    InstanceRec &inst = mInstances[card.instance];
    if (!inst.item) return;

    aimCamera(card);

    // THE INCLUDE CHANNEL, for the length of one pass and no longer. The item
    // keeps every bit it had; the capture bit is ADDED, so the object's place
    // in every other pass of the frame is untouched even if something throws
    // before the restore (the restore is a scope guard for that reason).
    const Ogre::uint32 wasFlags = inst.item->getVisibilityFlags();
    const unsigned char wasLod = inst.item->getCurrentMeshLod();
    struct Restore {
        Ogre::Item *item = nullptr;
        Ogre::uint32 flags = 0u;
        unsigned char lod = 0u;
        ~Restore() {
            item->setVisibilityFlags(flags);
            item->_setCurrentMeshLod(lod);
        }
    } restore{ inst.item, wasFlags, wasLod };
    inst.item->setVisibilityFlags(wasFlags | detail::kCardSubjectBit);
    // THE CARD'S OWN LOD (patch 0085). `mCurrentMeshLod` is what every pass
    // draws and upstream keeps it protected with only a "reset to 0" door, so
    // the setter is a numbered patch rather than a reach-in: the level is a
    // capture DECISION (Atom's chain, spent at the card's texel) and a pass
    // that recomputed it from this ortho camera would answer a question nobody
    // asked.
    inst.item->_setCurrentMeshLod(card.lodLevel);

    // THE SHADOW NODE IS RECALCULATED PER CARD, AND THAT IS MEASURED RATHER
    // THAN ASSUMED. The Grand Showroom's own defect was a shadow node
    // recalculated N times a frame for one picture, so the obvious saving here
    // is "once per card SET, since the six cards of an instance look at the
    // same place" — and this lane BUILT that and then took it out, for two
    // reasons. It is WRONG: a card wider than a page is SPLIT, so one
    // instance's cards look at up to sixteen different places and a fit made
    // for the first leaves the rest reading an atlas that does not cover them
    // (measured: the floor's shadow went back to a flat 1.0). And it saves
    // NOTHING: the per-card cost was 0.3734 ms with the saving and 0.3356
    // without it, i.e. inside the run-to-run spread — the capture's cost is the
    // workspace UPDATE's own fixed cost and not the shadow node's (see
    // `GiQualityFacts::cardBudgetTexels` for where that number comes from).
    //
    // What the per-card fit BUYS is quality: the PSSM splits are fitted to the
    // capture camera's own box, which is a metre or two of world, so a card's
    // shadow term is as sharp as the atlas can make it rather than as sharp as
    // the view's whole frustum allows.
    const auto tA = std::chrono::steady_clock::now();
    {
        const CaptureFlag capturing;
        mWs->_validateFinalTarget();
        mWs->_beginUpdate(false);
        mWs->_update();
        mWs->_endUpdate(false);
    }
    const auto tB = std::chrono::steady_clock::now();

    // ...AND INTO THE ATLAS. Five small copies — the reason the scratch exists
    // at all is in SurfaceCache.h (this pin's render-pass clear is whole-target,
    // so a page-scissored capture would wipe the atlas).
    for (unsigned i = 0; i < kCardLayers; ++i) {
        Ogre::TextureBox src = mScratch[i]->getEmptyBox(0);
        src.width = card.size;
        src.height = card.size;
        Ogre::TextureBox dst = mAtlas[i]->getEmptyBox(0);
        dst.x = card.atlasX;
        dst.y = card.atlasY;
        dst.width = card.size;
        dst.height = card.size;
        mScratch[i]->copyTo(mAtlas[i], dst, 0, src, 0);
    }

    const auto tC = std::chrono::steady_clock::now();
    mWsMs += float(std::chrono::duration<double, std::milli>(tB - tA).count());
    mCopyMs += float(std::chrono::duration<double, std::milli>(tC - tB).count());
    card.queued = false;
    card.lastUpdated = mFrame;
    ++mCaptures;
    ++mCapturesLastFrame;
    mTexelsLastFrame += card.size * card.size;
}

// ---------------------------------------------------------------------------
// The two GPU tables — PHASE 4's contract, built now so that phase 4 ports a
// read and does not invent a layout.
// ---------------------------------------------------------------------------
//
// NOTHING BINDS THESE TODAY and that is stated rather than hidden: the reader
// is the reflection ray job at phase 4, and SURFACE-CACHE-0 measured what that
// read costs (+3.1 µs over a 36,864-ray dispatch) with a hand-rolled five-
// binding version whose debt was exactly this — "five FIXED bindings and 27
// vec4 hold exactly ONE card set". What they cost meanwhile is two buffers of
// 384 KB and 16 KB and one rebuild per residency change, which is the honest
// price of the layout being decided HERE, by the code that allocates the rects,
// rather than guessed by the code that reads them.
//
// THE KEY IS FREE: `OgreScene::gatherRayInstances` already writes the scene's
// own item slot into each TLAS instance's `instanceCustomIndex`, so "which
// cards belong to this hit" is one indexed fetch of `mInstanceBuffer` and no
// lookup at all.
void SurfaceCache::syncBuffers() {
    if (!mTableDirty) return;
    mTableDirty = false;
    Ogre::VaoManager *vao = Ogre::Root::getSingleton().getRenderSystem()->getVaoManager();
    if (!vao) return;

    // 24 floats a record = the 96 bytes CardGpuRec declares.
    const unsigned records = std::min(unsigned(mCards.size()), kCardRecordCeiling);
    mCardBufferCpu.assign(size_t(kCardRecordCeiling) * 24u, 0.0f);
    for (unsigned i = 0; i < records; ++i) {
        const CardRec &c = mCards[i];
        CardGpuRec rec = {};
        // THE HALF-TEXEL INSET IS IN THESE TWO LINES and nowhere else: a card
        // parameter u in [0, 1] maps to atlas texel CENTRES, from x + 0.5 to
        // x + size - 0.5, so a filtered fetch at either edge can never reach a
        // neighbour's texels. That is Lumen's "0.5-texel border" expressed as
        // arithmetic instead of as wasted texels.
        // ...AND v IS MIRRORED, because the capture camera's +Y is the card's
        // +v while an image's row 0 is its TOP. `sampleCard` flips v when it
        // turns a card parameter into an atlas texel; a record that did not
        // would hand phase 4 an upside-down card, which is the kind of thing
        // nobody sees until a picture is wrong for a reason no counter names.
        // The flip is in the SCALE's sign and the BIAS's base row, so the
        // shader still does one multiply-add.
        const float atlas = float(kCardAtlasSize);
        rec.uvScaleBias[0] = float(c.size - 1u) / atlas;
        rec.uvScaleBias[1] = -float(c.size - 1u) / atlas;
        rec.uvScaleBias[2] = (float(c.atlasX) + 0.5f) / atlas;
        rec.uvScaleBias[3] = (float(c.atlasY + c.size) - 0.5f) / atlas;
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
        const unsigned slot = mInstances[c.instance].itemSlot == size_t(-1)
                                  ? 0xFFFFFFFFu
                                  : unsigned(mInstances[c.instance].itemSlot);
        rec.key[0] = slot;
        std::memcpy(&mCardBufferCpu[size_t(i) * 24u], &rec, sizeof(rec));
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
    mBudget = view.budgetTexels;
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
    // ceiling the frame never goes over.
    const auto t0 = std::chrono::steady_clock::now();
    unsigned spent = 0u;
    size_t drained = 0;
    for (unsigned idx : mQueue) {
        const unsigned cost = mCards[idx].size * mCards[idx].size;
        if (spent && spent + cost > view.budgetTexels) break;
        captureCard(mCards[idx]);
        spent += cost;
        ++drained;
        if (spent >= view.budgetTexels) break;
    }
    mCaptureMs = drained ? float(std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() - t0).count())
                         : 0.0f;
    mQueue.erase(mQueue.begin(), mQueue.begin() + ptrdiff_t(drained));
}

void SurfaceCache::noteMaterialChanged(MaterialId material) {
    bool any = false;
    for (const InstanceRec &inst : mInstances) {
        if (!inst.cardCount || inst.material != material) continue;
        for (unsigned c = 0; c < inst.cardCount; ++c) mCards[inst.firstCard + c].queued = true;
        any = true;
    }
    if (any) ++mInvalidMaterial;
}
void SurfaceCache::workspacePreUpdate(Ogre::CompositorWorkspace *) { gCapturing = true; }
void SurfaceCache::workspacePosUpdate(Ogre::CompositorWorkspace *) { gCapturing = false; }

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
    out.bytes = bytes;
    out.emissiveFormat = mEmissiveFormatName;
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
    for (int k = 0; k < 3; ++k) {
        out.albedo[k] = vals[unsigned(CardLayer::Albedo)][k];
        // The normal is stored *0.5+0.5, as the prepass writes it.
        out.normal[k] = vals[unsigned(CardLayer::Normal)][k] * 2.0f - 1.0f;
        out.emissive[k] = vals[unsigned(CardLayer::Emissive)][k];
    }
    out.depth = vals[unsigned(CardLayer::Depth)][0];
    out.shadow = vals[unsigned(CardLayer::ShadowRough)][0];
    out.roughness = vals[unsigned(CardLayer::ShadowRough)][1];
    out.ok = true;
    return true;
}

bool SurfaceCache::readAt(const Ogre::Vector3 &world, const Ogre::Vector3 &normal,
                          CardSample &out) const {
    out = CardSample();
    if (!mBuilt) return false;
    Ogre::Vector3 n = normal;
    if (n.squaredLength() < 1e-12f) return false;
    n.normalise();

    const CardRec *best = nullptr;
    float bestFacing = 0.05f;   // a card edge-on to the surface says nothing
    float bestU = 0.0f, bestV = 0.0f;
    for (const CardRec &c : mCards) {
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
    return sampleCard(*best, bestU, bestV, out);
}

bool SurfaceCache::dump(const std::string &prefix, std::string &err) const {
    if (!mBuilt) { err = "no surface cache"; return false; }
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    rs->flushCommands();
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    const char *name[kCardLayers] = { "albedo", "normal", "depth", "emissive", "shadowrough" };
    // `scale` turns a layer into something an eye can read: the depth is metres
    // and the emissive is radiance, so both are divided by a stated number
    // rather than clipped silently.
    const float scale[kCardLayers] = { 1.0f, 1.0f, 0.2f, 0.25f, 1.0f };
    std::vector<unsigned char> rgba(size_t(kCardAtlasSize) * kCardAtlasSize * 4u);
    for (unsigned i = 0; i < kCardLayers; ++i) {
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            kCardAtlasSize, kCardAtlasSize, 1u, Ogre::TextureTypes::Type2D,
            mAtlas[i]->getPixelFormat());
        tk->download(mAtlas[i], 0, true, nullptr, true);
        const Ogre::TextureBox box = tk->map(0);
        for (unsigned y = 0; y < kCardAtlasSize; ++y)
            for (unsigned x = 0; x < kCardAtlasSize; ++x) {
                float px[4];
                decodeTexel(mAtlas[i]->getPixelFormat(), box.at(x, y, 0), px);
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
