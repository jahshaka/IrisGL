// SURFACE-CACHE phase 2 — the capture Component's implementation.
// The design, the two condemned shapes it replaces and the renderArea evidence
// behind the scratch-and-copy route are all in SurfaceCache.h; this file is the
// mechanism.
#include "EnginePrivate.h"
#include "SurfaceCache.h"
#include "AtomPass.h"
#include "photon/PhotonDispatch.h"

#include <OgreCamera.h>
#include <OgreItem.h>
#include <OgreLight.h>
#include <OgreHlmsCompute.h>
#include <OgreRenderQueue.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbs.h>
#include "photon/voxel/PhotonVoxelLighting.h"
#include "photon/voxel/PhotonVoxelizerSourceBase.h"
#include "photon/voxel/PhotonSpecularAlbedoBuffer.h"
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
#include <Vao/OgreAsyncTicket.h>
#include <Vao/OgreConstBufferPacked.h>
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
#include <memory>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

// THE CAPTURES' ASYNCHRONOUS DRAWS, per SceneManager (ASYNC-SHADERS-1): a capture batch that met a
// shader still building is discarded, and the engine leaves its draws out of the views' numbers.
// Per SceneManager, not per cache: a cache is torn down and rebuilt with the GI while the
// RenderQueue's running totals it must be subtracted from live on. Main thread only.
struct CaptureAsyncDraws { unsigned long long placeholders = 0u, skips = 0u; };
static std::unordered_map<const Ogre::SceneManager *, CaptureAsyncDraws> gCaptureAsyncDraws;


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
/// counts them in the order they are declared - and the specular lobe's F0 and
/// weight at location 5 after them (ENGINE-POLISH-PHOTON-1 stage 3). So the
/// render target view's colour attachments must be in exactly this order,
/// whatever order the atlas layers are declared in.
const CardLayer kShaderOrder[kCardLayers] = { CardLayer::Normal, CardLayer::ShadowRough,
                                              CardLayer::Albedo, CardLayer::Emissive,
                                              CardLayer::Depth, CardLayer::Specular };

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
/// JahCardRecord). std430, 144 bytes: the world frame's three rows, the axis and
/// the texel, the atlas rect with the card's two flags, and its lamp table. Built here because
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
    /// THE CARD'S LAMP TABLE (PHOTON-7 stage 5, 5-3; 48 since 10-4 R9 (3)): up to kCardLampTable world lamp ids
    /// (SceneLightList::documentId, 0 = none), bit j of each texel's lamp mask the visibility of lamp j
    /// (jah_rq_card_lights.glsl maps its own lamps by id; jah_card_sunvis.glsl the texel).
    unsigned lamps[kCardLampTable] = {};
};
static_assert(sizeof(CardGpuRec) == 272u, "the card record's layout is a contract with the ray job");
constexpr unsigned kGpuCardCaptured = 1u;
constexpr unsigned kGpuCardLit = 2u;

}   // namespace

bool SurfaceCache::capturing() { return gCapturing; }

SurfaceCache::SurfaceCache() : mEmissiveFormat(Ogre::PFG_R9G9B9E5_SHAREDEXP) {}

SurfaceCache::~SurfaceCache() {
    destroyAll();
    detail::monitor::forgetGpuOwner(this);   // its "cards.relight" readings
}

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
    // three 9-bit mantissas), and RG8 halved the shadow/roughness pair: 22 -> 16
    // bytes before any BC encode, which is the number this phase was told to
    // reach. (PHOTON-3 took the pair back to RG16: the read's specular lobe needs a
    // polished surface's alpha, which 8 bits rounded to 0.001.)
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
        // albedo (kD) at 16 bits a channel (PHOTON-7 10-3 R12, the lead's ruling): the layer stores kD = albedo / pi,
        // so a byte spent its top 68 % on nothing and held any albedo to +-0.0062 (half a step x pi) - 0.6 read
        // +0.61 %, a dark 0.03 -18 % (gi.card_truth (T1): the card-answered hits +0.6 % against the analytic, the
        // voxel route exact). A UAV (the relight writes its alpha), so sRGB was not open. +16 MB at 2048^2.
        Ogre::PFG_RGBA16_UNORM,
        Ogre::PFG_RGBA8_UNORM,   // normal
        Ogre::PFG_R16_FLOAT,     // depth
        mEmissiveFormat,         // emissive
        // shadow + roughness: 16 bits a channel (PHOTON-3 stage 2) - the read evaluates the specular lobe toward
        // its ray from y (the GGX alpha), and 8 bits held a polished surface's alpha to 1/255: r 0.05 (alpha
        // 0.0025) read as alpha 0.001, its mirror peak 4.9 x too bright (gi.metal_bounce's law). +8 MB at 2048^2.
        Ogre::PFG_RG16_UNORM,
        Ogre::PFG_RGBA8_UNORM    // the specular lobe: F0 relative to the dielectric's, kS
    };
    const char *name[kCardLayers] = { "cardAlbedo", "cardNormal", "cardDepth", "cardEmissive",
                                      "cardShadowRough", "cardSpecular" };

    for (unsigned i = 0; i < kCardLayers; ++i) {
        // THE ATLAS IS NEVER A RENDER TARGET. It is written by COPY (see the
        // header: this pin's Vulkan renderArea is the whole attachment, so a
        // page-scissored clear does not exist) and read by a sampler, and
        // `TextureFlags::ManualTexture` is what says "we fill this ourselves" —
        // the rule CLAUDE.md records as the SSAO noise texture's week-long
        // defect (flags 0 = load me from a resource group = a worker null
        // dereference). Vulkan gives every texture TRANSFER_SRC|TRANSFER_DST
        // unconditionally, so a manual texture is a valid copy destination.
        // THE ALBEDO AND NORMAL LAYERS ARE ALSO UAVs (PHOTON-CARDS-5): the
        // relight writes the texel's mean light direction into their alpha
        // (JahCardView), which the capture writes as 1 and nothing read.
        // ...AND SO IS SHADOWROUGH (ATOM-S3-CARDCAP): the still world's sun term
        // is TRACED into its x (rq_card_movers.comp, still mode), the capture's y
        // (the GGX alpha) kept.
        const bool relightWrites = i == unsigned(CardLayer::Albedo) || i == unsigned(CardLayer::Normal) ||
                                   i == unsigned(CardLayer::ShadowRough);
        Ogre::TextureGpu *t =
            tm->createTexture(processUniqueName(name[i]), Ogre::GpuPageOutStrategy::Discard,
                              Ogre::TextureFlags::ManualTexture |
                                  (relightWrites ? Ogre::TextureFlags::Uav : 0u),
                              Ogre::TextureTypes::Type2D);
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
    // is a UAV — never a render target, never a copy destination. RGBA16F (below: 32 MB at 2048 square), and so are the
    // Indirect and Flat layers beside it (REDS-1, the lead's ruling: a layer the radiance is REBUILT from carries the
    // radiance's precision - they were R11G11B10F, a 6-bit mantissa at 0.4-0.8 % a step, and a relight that does not march
    // rebuilt the radiance from them: gi.card_closed_form +0.08..0.10 %, 1.3-1.7 store steps; +16 MB each at 2048^2).
    mRadianceFormatName = "RGBA16F";
    const Ogre::PixelFormatGpu radFmt = Ogre::PFG_RGBA16_FLOAT;
    // THE RADIANCE ITSELF IS RGBA16F (PHOTON-7 stage 10, 10-2d; the lead's ruling, +16 MB - the spec's memory list): a
    // store whose quantum (R11G11B10F's 1.6 % step) is over the display code cannot hold laws stated within a code,
    // and every reader of it - a reflection's hit, the gather's, another card's - inherits the step (gi.card_residency
    // (2): the walk's and the teleport's read sets differed through it). The two halves beside it keep radFmt.
    mRadiance = tm->createTexture(processUniqueName("cardRadiance"), Ogre::GpuPageOutStrategy::Discard,
                                  Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mRadiance->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mRadiance->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
    mRadiance->setNumMipmaps(1u);
    mRadiance->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE CACHED INDIRECT HALF, the march's answer per texel (Lumen's
    // IndirectLighting atlas): the expensive half is recomputed only when the
    // chain re-injects, and a direct-only relight reads it back. Same format,
    // read and written by the same job. +32 MB (RGBA16F).
    mIndirect = tm->createTexture(processUniqueName("cardIndirect"), Ogre::GpuPageOutStrategy::Discard,
                                  Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mIndirect->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mIndirect->setPixelFormat(radFmt);
    mIndirect->setNumMipmaps(1u);
    mIndirect->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE FLAT TERM (ENGINE-POLISH-PHOTON-1 stage 3): the emissive and the
    // specular lobe's re-emission, what the read adds outside the diffuse view
    // restore. Same format, written by the same job. +32 MB (RGBA16F).
    mFlat = tm->createTexture(processUniqueName("cardFlat"), Ogre::GpuPageOutStrategy::Discard,
                              Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mFlat->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mFlat->setPixelFormat(radFmt);
    mFlat->setNumMipmaps(1u);
    mFlat->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE SUN'S VISIBILITY AT THE TEXEL (PHOTON-3 stage 2): the still world's traced term
    // times the movers' and the cloud sheet's, as the relight shades the texel - the one quantity
    // the read's specular lobe of the sun cannot know (jah_rq_card_lights.glsl evaluates every
    // light's lobe toward its own ray from the ray tier's light table). A UAV the relight writes
    // and nothing uploads: ManualTexture (never load-from-group), made resident here and never
    // notified (DOCS/traps/ENGINE.md). RGBA8, +16 MB at 2048 square: G, B and A are THE LAMPS' traced visibility
    // (PHOTON-7 stage 5, 5-3: rq_card_movers.comp's lamps mode) - the first eight reaching lamps' bits and the rest's
    // weighted mean, which the relight takes per lamp, and every lamp's weighted mean, the read's lamp lobe's - in
    // this layer's channels rather than a ninth view binding.
    mSunVis = tm->createTexture(processUniqueName("cardSunVis"), Ogre::GpuPageOutStrategy::Discard,
                                Ogre::TextureFlags::ManualTexture | Ogre::TextureFlags::Uav,
                                Ogre::TextureTypes::Type2D);
    mSunVis->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    // RGBA32_UINT (PHOTON-7 10-4 R9 (3), the lead's ruling: 48 lamps exact a texel; the fix round's F3: a lamp through
    // panes keeps its fraction): the sun's, the fractional lamps' and the transmitted lamps' bytes, the 48-bit lamp mask
    // and the 48-bit transmitted mask (jah_card_sunvis.glsl) - +16 MB over the RGBA16 texel at 2048^2.
    mSunVis->setPixelFormat(Ogre::PFG_RGBA32_UINT);
    mSunVis->setNumMipmaps(1u);
    mSunVis->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE MOVERS' VISIBILITY (PHOTON-CARDS-4): R8 over the atlas, a UAV
    // the ray tier's trace writes and the relight reads (only inside the cards
    // it flags `moverTraced`, so a texel no trace wrote is never read). +4 MB.
    mMoverVis = tm->createTexture(processUniqueName("cardMoverVis"), Ogre::GpuPageOutStrategy::Discard,
                                  Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mMoverVis->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mMoverVis->setPixelFormat(Ogre::PFG_R8_UNORM);
    mMoverVis->setNumMipmaps(1u);
    mMoverVis->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE OPEN FRACTION (PHOTON-7 11-2 (5), jah_card_open.glsl): RGBA8 over the atlas, a UAV - ( 1 - f, the open
    // sub-points' centroid offset u, v, written ) - the still trace writes it at each capture (four buried tests a
    // texel), the traces, the gather and the relight read it. +16 MB at 2048^2.
    mOpen = tm->createTexture(processUniqueName("cardOpen"), Ogre::GpuPageOutStrategy::Discard,
                              Ogre::TextureFlags::ManualTexture | Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mOpen->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mOpen->setPixelFormat(Ogre::PFG_RGBA8_UNORM);
    mOpen->setNumMipmaps(1u);
    mOpen->_transitionTo(Ogre::GpuResidency::Resident, nullptr);

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
                                          "scr3Emissive", "scr4Depth", "scr5Specular", "scrDepthBuf" };
    for (unsigned i = 0; i < kCardLayers + 1u; ++i)
        n->addTextureSourceName(chan[i], i, Ogre::TextureDefinitionBase::TEXTURE_INPUT);

    // NO SHADOW NODE (ATOM-S3-CARDCAP): the prepass writes its constant 1.0 into
    // ShadowRough.x, and the still world's sun term is traced over it after the
    // copies (traceSun) — one ray per card texel, no PSSM fit per card.

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
        // normal and the GGX alpha exactly (and a constant 1.0 where a shadow
        // term would go: no shadow node — the term is traced), computes no
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
        p->mVisibilityMask = detail::kCardSubjectBit;
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
        // straight onto the Item through fork 5230c9390 (was 0085)'s setter in passPreExecute —
        // a pass that recomputed LOD from this ortho camera would undo it.
        p->mUpdateLodLists = false;
        p->mProfilingId = "Jahshaka card capture";
        p->mJahReadsGi = false;   // the capture is a PrePassCreate G-buffer: it samples no GI (ENGINE-UPGRADE-ASYNC-3)
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
    // THE CAPTURE'S COMPILE MODE is the scene's views' (setAsyncShaders).
    mWs->setAsyncShaderCompile(mAsyncShaders);
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
    ++mPhotonGeneration;
    mInstances.clear();
    mCards.clear();
    mByNode.clear();
    mFaceMarks.clear();
    mMarkedSlots.clear();
    mSlotMarked.clear();
    mFacePrev.clear();
    mFaceRing.clear();
    mFaceRingHead.clear();
    mDemandNewest[0] = mDemandNewest[1] = 0ull;
    mDemandChangeFrame = 0ull;
    mQueue.clear();
    mPageOwner.clear();
    mPageSubSize.clear();
    mPageSubUsed.clear();
    mPageSubMask.clear();
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    if (!root) return;
    Ogre::CompositorManager2 *cm = root->getCompositorManager2();
    mPosUpdated = mBatchCopied = false;
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
        mCardDelta.reset(nullptr);
        mInstanceDelta.reset(nullptr);
        mCardBoxDelta.reset(nullptr);
        if (mRelightBuffer) { vao->destroyUavBuffer(mRelightBuffer); mRelightBuffer = nullptr; }
        if (mLightBuffer) { vao->destroyUavBuffer(mLightBuffer); mLightBuffer = nullptr; }
        if (mGiBuffer) { vao->destroyUavBuffer(mGiBuffer); mGiBuffer = nullptr; }
        for (ReportTicket &t : mReportReadings) {
            t.ticket.reset();
            // (a report never read: its relit instances are listed, never dropped - 8B B1)
            mLateRelitBoxes.insert(mLateRelitBoxes.end(), t.boxes.begin(), t.boxes.end());
            t.boxes.clear();
        }
        listPendingRelits();
        mReportNextIssue = mReportNextTake = 0u;
        for (DemandTicket &t : mDemandReadings) t.ticket.reset();
        mDemandNextIssue = mDemandNextTake = 0u;
        if (mCardBoxBuffer) { vao->destroyUavBuffer(mCardBoxBuffer); mCardBoxBuffer = nullptr; }
        if (mCardSlotState) { vao->destroyUavBuffer(mCardSlotState); mCardSlotState = nullptr; }
        if (mCardReport) { vao->destroyUavBuffer(mCardReport); mCardReport = nullptr; }
        if (mCardCluster) { vao->destroyUavBuffer(mCardCluster); mCardCluster = nullptr; }
        if (mCardMoves) { vao->destroyUavBuffer(mCardMoves); mCardMoves = nullptr; }
        if (mCardReads) { vao->destroyUavBuffer(mCardReads); mCardReads = nullptr; }
    }
    mAccumJob = mSensJob = nullptr;
    mAccumClearFrame = ~0ull;
    mAccumRan = false;
    mSlotCard.clear();
    mCardRecords = 0u;
    mInstanceSlots = 0u;
    mCardBufferCpu.clear();
    mInstanceBufferCpu.clear();
    mTableDirty = false;
    mBatch.clear();
    mRelight.clear();
    mRelightMode.clear();
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
        if (mFlat) { tm->destroyTexture(mFlat); mFlat = nullptr; }
        if (mSunVis) { tm->destroyTexture(mSunVis); mSunVis = nullptr; }
        if (mMoverVis) { tm->destroyTexture(mMoverVis); mMoverVis = nullptr; }
        if (mOpen) { tm->destroyTexture(mOpen); mOpen = nullptr; }
        if (mGather) { tm->destroyTexture(mGather); mGather = nullptr; }
    }
}

bool SurfaceCache::ensureGatherLayer() {
    if (mGather) return true;
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::TextureGpuManager *tm = rs ? rs->getTextureGpuManager() : nullptr;
    if (!tm || !tm->checkSupport(Ogre::PFG_RGBA16_FLOAT, Ogre::TextureTypes::Type2D, Ogre::TextureFlags::Uav))
        return false;
    // Written by the ray tier's rq_card_gather.comp and read by the relight, nothing else: a UAV, never a render
    // target or a copy destination, resident from here (the lit layers' rule).
    mGather = tm->createTexture(processUniqueName("cardGather"), Ogre::GpuPageOutStrategy::Discard,
                                Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
    mGather->setResolution(kCardAtlasSize, kCardAtlasSize, 1u);
    mGather->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
    mGather->setNumMipmaps(1u);
    mGather->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    // ...and THE READ LISTS (10-2c): every slot's block zero (no card read yet) - ZEROED ON THE GPU (photonFillBuffer):
    // its 4 MB of host zeros through the staging buffer forced a fenced flush and a 13.5 ms GPU idle on this frame
    if (!mCardReads) {
        Ogre::VaoManager *vao = rs->getVaoManager();
        constexpr size_t slots = size_t(kCardAtlasSize / kCardMinSize) * (kCardAtlasSize / kCardMinSize);
        mCardReads = vao->createUavBuffer(slots * kCardReadsBlock, sizeof(Ogre::uint32), 0, nullptr, false);
        if (mCardReads) detail::photonFillBuffer(mCardReads, 0u);
    }
    return mCardReads != nullptr;
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
// Residency — BY THE READERS' DEMAND (PHOTON-7 stage 5, 5-2)
// ---------------------------------------------------------------------------
//
// A CARD IS RESIDENT BECAUSE A READER READ IT. Every reader of a card is a ray's hit (the reflection, the gather:
// jah_rq_card.glsl), and each hit stamps what it read into the instance table's demand region - the card's atlas cell
// when a card answered, and on EVERY hit the instance's FACE BUCKET it faced (jah_rq_card_bindings.glsl's layout; F3,
// PHOTON-7 stage 10-3: the rest decision's set is the cards of the buckets hit in the rest span).
// The stamps come back at a fixed lag of the cache's frames (takeDemand), PER READER (the reflection, the gather): a
// card's `readBy` is each reader's newest; a face marked within the WINDOW of its reader's clock is WANTED, and
// its cards (the layout's cards in that bucket) arrive. An arrival that finds no room takes it from the cards no reader
// has read for a window of its own clock (a reader at rest - the reflection held, the gather held - reads nothing, and
// what it read stays: measured, photon-glass's reflection at rest from frame ~513 let the gather's faces evict the
// reflection's, whose re-admission broke the reflection's rest again) - the least recently read first, the
// smallest on screen (the card's extent over its distance from the viewer: an order, so no pixel angle is needed) the
// tie-break, then the node and the card's index - and, before it evicts, from the atlas's own fragmentation
// (compactOnePage). So the faces the readers reach are resident and the faces no reader reaches are not: a sealed
// room's outer faces hold no page, its pillars do (PREFLIGHT_5 §2: photon-glass's 397 wanted pages of 256 were
// arrivals nearest-first behind the 7/8 gate; the reachable faces want ~184). DELETED with it: the residency sphere
// (every instance inside the tier's radius held every card), the 7/8 gate, the gesture bypass (mReturning) and the
// age that was every card's `lastUsed = mFrame` every frame.
namespace {
/// THE ATLAS'S CELLS (16 texels a side): the demand stamps' and the indirect state's key.
constexpr unsigned kDemandCells = (kCardAtlasSize / kCardMinSize) * (kCardAtlasSize / kCardMinSize);
/// The demand region's head (the stamp and the count flag; the counts), in uvec4 entries.
constexpr unsigned kDemandHeadEntries = 2u;
/// The readers (jah_rq_card.glsl's JAH_CARD_DEMAND: 0 the reflection, 1 the gather).
constexpr unsigned kDemandReaders = 2u;
/// Per atlas cell and reader, a stamp: 2 x 16384 words.
constexpr unsigned kDemandCellEntries = kDemandCells * kDemandReaders / 4u;
/// Per item slot, the face marks: per MARK READER 6 buckets in 8 words - the reflection's (0), the gather's every hit (1:
/// the admission path, the window's offers) and the gather's REST-SPAN hits (2: JAH_CARD_FACE_GATE, the decision's rank;
/// PHOTON-7 8B, the lead's ruling - the two jobs of the mark) - six entries; the face HITS (the measuring arm) keep the
/// readers' two blocks, four entries.
constexpr unsigned kDemandFaceEntries = 6u, kDemandHitEntries = 4u;
/// ...the marks a slot holds (8 per mark reader) and the hits (8 per reader)
constexpr unsigned kFaceMarkStride = 4u * kDemandFaceEntries, kFaceHitStride = 4u * kDemandHitEntries;
/// THE MARK THE DECISION RANKS for a reader: the reflection's own, the gather's rest-span block (2); the window's (the
/// admission) reads each reader's own block
constexpr unsigned restMarkOf(unsigned r) { return r == 1u ? 2u : r; }
/// The read-back's period (the stamps persist on the GPU: a period only costs latency).
constexpr unsigned long long kDemandReadEvery = 4ull;

size_t demandEntries(unsigned slots) {
    // the head, the cells' stamps, the slots' face marks, and the slots' face hits (the measuring arm's, the marks' layout)
    return size_t(kDemandHeadEntries) + kDemandCellEntries + size_t(kDemandFaceEntries + kDemandHitEntries) * slots;
}
unsigned demandCellOf(const CardRec &c) {
    return (c.atlasY / kCardMinSize) * (kCardAtlasSize / kCardMinSize) + c.atlasX / kCardMinSize;
}
/// jah_rq_card.glsl's jahCardBucket, the same rule on the CPU.
unsigned char bucketOf(const Ogre::Vector3 &n) {
    const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
    const unsigned axis = (ax >= ay && ax >= az) ? 0u : (ay >= az ? 1u : 2u);
    return static_cast<unsigned char>(2u * axis + (n[axis] < 0.0f ? 1u : 0u));
}
/// THE TIE-BREAK: how big the card is on screen, up to the view's pixel angle (a common factor an order never needs).
float projectedSize(const CardRec &c, const Ogre::Vector3 &eye) {
    return 2.0f * std::max(c.halfU, c.halfV) / std::max(0.05f, (c.centre - eye).length());
}
}   // namespace

void SurfaceCache::releaseInstance(size_t idx) {
    InstanceRec &inst = mInstances[idx];
    for (unsigned c = 0; c < inst.cardCount; ++c) {
        CardRec &card = mCards[inst.firstCard + c];
        freeRect(card.atlasX, card.atlasY, card.size);
    }
    mByNode.erase(inst.node);
    // The card block is left in place and the instance marked dead; the
    // compaction below rebuilds both vectors at once, which is O(n) per
    // residency change rather than O(n) per instance.
    inst.cardCount = 0u;
    inst.node = 0;
}

void SurfaceCache::layoutCards(const CardSceneView::Candidate &cand, std::vector<CardRec> &out) const {
    out.clear();
    if (!cand.item || !cand.sceneNode || !cand.cards || cand.cards->empty()) return;
    const Ogre::Vector3 pos = cand.sceneNode->_getDerivedPosition();
    const Ogre::Quaternion rot = cand.sceneNode->_getDerivedOrientation();
    const Ogre::Vector3 scale = cand.sceneNode->_getDerivedScale();
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
        // along each side, WORLD-FIXED (the stored density never moves with the
        // camera: PREFLIGHT_5 Q1); anything over a page is cut into a grid of whole
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
                r.axis = static_cast<unsigned char>(axis);
                r.u = uW; r.v = vW; r.d = dW;
                r.halfU = halfU / float(splitU);
                r.halfV = halfV / float(splitV);
                r.halfDepth = halfD;
                r.centre = originW + uW * ((2.0f * su2 + 1.0f - float(splitU)) * r.halfU)
                           + vW * ((2.0f * sv2 + 1.0f - float(splitV)) * r.halfV);
                r.size = oneSize;
                // THE LOD LEVEL IS THE BAKE'S, AND THE BAKE'S ALONE (ATOM
                // inventory row AT-CARDLOD): the rule (`lodLevelForWorldError` over
                // `lodBounds`, with the card's texel as the allowed deviation) is
                // computed once, at bake time, by `cards::levelForTexel`, for its
                // nominal 128-texel page (`MeshBake::cardCaptureResolution`); a split
                // card can draw a level chosen for a slightly coarser texel, and
                // `gi.card_capture` asserts the page size the two assume agrees.
                r.lodLevel = c.lodLevel;
                r.index = static_cast<unsigned short>(out.size());
                r.bucket = bucketOf(dW);
                // ...AND THAT LEVEL'S MEASURED BOUND in the world (READ_A finding 3: the still trace's lift reads it).
                r.levelBound = 0.0f;
                if (c.lodLevel > 0u && cand.lodBounds && size_t(c.lodLevel) <= cand.lodBounds->size()) {
                    const float b = (*cand.lodBounds)[size_t(c.lodLevel) - 1u];
                    const float sMax = std::max(std::fabs(scale.x), std::max(std::fabs(scale.y), std::fabs(scale.z)));
                    if (std::isfinite(b) && b > 0.0f) r.levelBound = b * sMax;
                }
                r.queued = true;
                out.push_back(r);
            }
        }
    }
}

bool SurfaceCache::atlasHasRoom() const {
    // A free page, a sub-cut page with a free slot, or a sub-cut page the others cutting its size could take whole
    // (compactOnePage's own test): the room an arrival could be given this frame without an eviction.
    unsigned freeBySize[kCardPageSize + 1u] = {};
    for (size_t q = 0; q < mPageOwner.size(); ++q) {
        if (mPageOwner[q] == 0u) return true;
        if (mPageOwner[q] != 2u) continue;
        const unsigned size = mPageSubSize[q];
        const unsigned slots = (kCardPageSize / size) * (kCardPageSize / size);
        if (mPageSubUsed[q] < slots) return true;
        freeBySize[size] += slots - mPageSubUsed[q];
    }
    for (size_t p = 0; p < mPageOwner.size(); ++p) {
        if (mPageOwner[p] != 2u) continue;
        const unsigned size = mPageSubSize[p];
        const unsigned slots = (kCardPageSize / size) * (kCardPageSize / size);
        if (freeBySize[size] - (slots - mPageSubUsed[p]) >= mPageSubUsed[p]) return true;
    }
    return false;
}

bool SurfaceCache::compactOnePage(std::vector<std::vector<CardRec>> &cards, unsigned long long roots) {
    const unsigned perSide = kCardAtlasSize / kCardPageSize;
    // THE PAGE TO EMPTY: the sub-cut page with the fewest cards whose cards the OTHER pages cutting its size have free
    // slots for (lowest index among equals) - deterministic, and a page freed whole. The free slots per sub-size are
    // summed ONCE and each page's room is that sum less its own (it used to be re-summed over every page for every page:
    // pages^2 = 65,536 steps a call, called for every arrival a full atlas turns away - 12 ms of a 10k world's mover
    // frame in Debug, scale.cpu_walks; the same choice, in pages steps).
    const size_t pages = mPageOwner.size();
    unsigned freeBySize[kCardPageSize + 1u] = {};
    for (size_t q = 0; q < pages; ++q)
        if (mPageOwner[q] == 2u) {
            const unsigned size = mPageSubSize[q];
            freeBySize[size] += (kCardPageSize / size) * (kCardPageSize / size) - mPageSubUsed[q];
        }
    size_t best = size_t(-1);
    for (size_t p = 0; p < pages; ++p) {
        if (mPageOwner[p] != 2u) continue;
        const unsigned size = mPageSubSize[p];
        const unsigned slots = (kCardPageSize / size) * (kCardPageSize / size);
        const unsigned room = freeBySize[size] - (slots - mPageSubUsed[p]);
        if (room < mPageSubUsed[p]) continue;
        if (best == size_t(-1) || mPageSubUsed[p] < mPageSubUsed[best]) best = p;
    }
    if (best == size_t(-1)) return false;
    const unsigned x0 = unsigned(best % perSide) * kCardPageSize, y0 = unsigned(best / perSide) * kCardPageSize;
    // THE MOVE: the page is held out of the allocator (owner 3) while its cards are placed in the others; each moved
    // card is the same card (its demand, its flags) at a new rect, RE-CAPTURED there - the capture rewrites every layer
    // of the rect, the relight and the march follow it (planRelights' captured cards), exactly as a newly arrived card.
    mPageOwner[best] = 3u;
    for (size_t inst = 0; inst < cards.size(); ++inst)
        for (CardRec &c : cards[inst]) {
            if (!c.size || c.atlasX < x0 || c.atlasX >= x0 + kCardPageSize || c.atlasY < y0 ||
                c.atlasY >= y0 + kCardPageSize)
                continue;
            unsigned nx = 0u, ny = 0u;
            if (!allocRect(c.size, nx, ny)) {   // counted above: cannot fail
                mPageOwner[best] = 2u;
                return false;
            }
            c.atlasX = nx;
            c.atlasY = ny;
            c.since = mFrame;
            c.queued = true;
            c.lastUpdated = 0ull;
            c.indirectValid = false;
            c.relight = true;
            c.relightIndirect = true;
            c.roots |= roots;   // (moved for the arrival that needed the room: its root's work)
            c.moverTraced = false;
            ++mCompactionMoves;
            // (a moved card answers no hit until its re-capture: the voxels do meanwhile - a change of what its instance's
            // hits read, the changed-region list's Card kind, PHOTON-7 stage 8B B1)
            if (inst < mInstances.size())
                mRelitBoxes.push_back(Ogre::Aabb(mInstances[inst].centre, mInstances[inst].halfSize));
        }
    mPageOwner[best] = 0u;
    mPageSubSize[best] = 0u;
    mPageSubUsed[best] = 0u;
    mPageSubMask[best] = 0ull;
    mTableDirty = true;
    ++mPhotonGeneration;
    return true;
}

// THE DEMAND WINDOW (SurfaceCache.h): the one formula refreshResidency decides by and owed_rows.py's judge mirrors.
// ...AND A READER's WINDOW CLOSES IN THE CACHE's FRAMES TOO (PHOTON-7 stage 10-3, the liveness fix): "its clock has not
// passed its first by S" read the reader's clock - its newest stamp - and a reader that grazes the cards once and never
// again (a shot's gather over a near-empty view: one carded hit 5 frames after the change, none after; or a reader past
// its stamping span) froze its clock below first + S, held the demand OPEN for ever, the rest decision never came, the
// cards owed their hold and giAtRest stayed false: a screenshot taken at once after a 5 cm camera move ran to its
// 2,002-frame cap (scripting.e2e.change_then_shot). Its stamps have all landed by the read-back's period and lag past
// first + S, so the window is closed then, read or not - the same frame a reader stamping every frame closes it at.
bool SurfaceCache::demandWindowOpen(unsigned long long frame, unsigned long long change, const unsigned long long first[2],
                                    const unsigned long long newest[2], unsigned long long settleS, unsigned long long every,
                                    unsigned long long lag) {
    if (!first[0] && !first[1]) return frame < change + settleS + every + lag;   // no reader since the change
    for (unsigned r = 0; r < kDemandReaders; ++r)
        if (first[r] && first[r] < change + settleS + every && newest[r] < first[r] + settleS &&
            frame < first[r] + settleS + every + lag)
            return true;
    return false;
}

unsigned SurfaceCache::demandDueFrames() {
    Ogre::Root *root = Ogre::Root::getSingletonPtr();
    Ogre::RenderSystem *rs = root ? root->getRenderSystem() : nullptr;
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    return unsigned(kDemandReadEvery) + (vao ? unsigned(vao->getDynamicBufferMultiplier()) + 1u : 1u) + 1u;
}

void SurfaceCache::issueDemandRead() {
    if (!mInstanceBuffer || mInstanceSlots == 0u || mFrame % kDemandReadEvery != 0ull) return;
    DemandTicket &t = mDemandReadings[mDemandNextIssue];
    if (t.ticket.get()) return;   // every slot in flight: the stamps persist, a later read takes them
    t.ticket = mInstanceBuffer->readRequest(mInstanceSlots, demandEntries(mInstanceSlots),
                                            /*accurateTracking=*/false);
    t.frame = mFrame;
    t.slots = mInstanceSlots;
    mDemandNextIssue = (mDemandNextIssue + 1u) % kDemandSlots;
}

// THE WINDOW (mDemandWindow, PHOTON-7 stage 5's read W2): a card read, or a face marked, within this many frames of ITS
// READER'S CLOCK (the newest stamp that reader wrote: a reader at rest reads nothing, and nothing it read goes stale while
// it rests) is wanted; a card no reader read for longer may be evicted. THE LAW is the settle's (GiQualityFacts::
// cardDemandSettleFrames, S): a face the readers hit lambda_min times a frame is marked twice in different read-backs
// within S frames at P >= 1 - 1e-3 (its demand persists, `twice`), and a card read lambda_min a frame goes S frames
// unread with P = e^-S (4.5e-5 at S = 10) - so the window IS S. (The 60 frames it replaced had no law.)
void SurfaceCache::takeDemand(std::vector<uint32_t> &slots, const CardSceneView &view) {
    mDemandWindow = std::max(1u, view.demandSettleFrames);
    mDemandGatherLate = view.demandGatherLate;
    // A STAMP COUNTED: its reader's clock, and - after the last demand change - its reader's first (the settle's start).
    // (The test door cards.gatherLate drops the gather's within N frames of the change: a reader not run yet.)
    const auto counted = [this](unsigned r, unsigned long long stamp) {
        if (r == 1u && mDemandGatherLate && stamp <= mDemandChangeFrame + mDemandGatherLate) return false;
        mDemandNewest[r] = std::max(mDemandNewest[r], stamp);
        if (stamp > mDemandChangeFrame) mDemandFirst[r] = mDemandFirst[r] ? std::min(mDemandFirst[r], stamp) : stamp;
        return true;
    };
    slots.clear();
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    // 1. THE READ-BACKS THAT LANDED, at a fixed lag of the cache's frames (the feedback readings' rule: a function of
    //    the frames, never of the GPU's pace). mSlotCard is the table the stamps were written against (the last
    //    syncBuffers; residency changes only after this).
    while (vao) {
        DemandTicket &t = mDemandReadings[mDemandNextTake];
        if (!t.ticket.get() || mFrame < t.frame || mFrame - t.frame <= vao->getDynamicBufferMultiplier()) break;
        const Ogre::uint32 *w = static_cast<const Ogre::uint32 *>(t.ticket->map());
        for (unsigned k = 0; k < 4u; ++k) mDemandHits[k] = w[4u + k];   // the GPU's counts are cumulative
        const Ogre::uint32 *cell = w + 4u * kDemandHeadEntries;
        for (unsigned c = 0; c < kDemandCells; ++c)
            for (unsigned r = 0; r < kDemandReaders; ++r) {
                const unsigned long long stamp = cell[kDemandReaders * c + r];
                if (!stamp || !counted(r, stamp)) continue;
                if (c >= mSlotCard.size() || mSlotCard[c] < 0 || size_t(mSlotCard[c]) >= mCards.size()) continue;
                CardRec &card = mCards[size_t(mSlotCard[c])];
                if (stamp < card.since) continue;   // older: the cell's last card's
                card.readBy[r] = std::max(card.readBy[r], stamp);
                card.lastRead = std::max(card.lastRead, stamp);
            }
        const Ogre::uint32 *face = cell + 4u * kDemandCellEntries;
        if (mFaceMarks.size() < size_t(t.slots) * kFaceMarkStride) {
            mFaceMarks.resize(size_t(t.slots) * kFaceMarkStride, 0ull);
            mFacePrev.resize(size_t(t.slots) * kFaceMarkStride, 0ull);
            mFaceRing.resize(size_t(t.slots) * kFaceMarkStride * kFaceRing, 0u);
            mFaceRingHead.resize(size_t(t.slots) * kFaceMarkStride, 0u);
        }
        // (a mark's clock is its reader's: the gather's rest-span block, 2, counts on the gather's)
        for (size_t k = 0; k < size_t(t.slots) * kFaceMarkStride; ++k)
            if (face[k] > mFaceMarks[k] && counted(std::min(unsigned((k % kFaceMarkStride) / 8u), 1u), face[k])) {
                mFacePrev[k] = mFaceMarks[k];
                mFaceMarks[k] = face[k];
                const size_t slot = k / kFaceMarkStride;
                if (mSlotMarked.size() <= slot) mSlotMarked.resize(size_t(t.slots), 0u);
                if (!mSlotMarked[slot]) {
                    mSlotMarked[slot] = 1u;
                    mMarkedSlots.push_back(uint32_t(slot));
                }
                // (the face's last read-backs that carried a new mark: the decision's span read count, F3's rank)
                mFaceRing[k * kFaceRing + mFaceRingHead[k]] = face[k];
                mFaceRingHead[k] = (unsigned char)((mFaceRingHead[k] + 1u) % kFaceRing);
            }
        // THE FACES' HITS (the measuring arm cards.demandCount): every read of a face, resident or not - the GPU's
        // counts are cumulative per buffer, so the host adds each word's move (a grown table's new buffer starts at 0)
        if (mDemandCount) {
            const Ogre::uint32 *hits = face + size_t(t.slots) * kFaceMarkStride;
            if (mFaceHits.size() < size_t(t.slots) * kFaceHitStride) {
                mFaceHits.resize(size_t(t.slots) * kFaceHitStride, 0ull);
                mFaceHitsGpu.resize(size_t(t.slots) * kFaceHitStride, 0u);
            }
            for (size_t k = 0; k < size_t(t.slots) * kFaceHitStride; ++k) {
                mFaceHits[k] += hits[k] >= mFaceHitsGpu[k] ? hits[k] - mFaceHitsGpu[k] : hits[k];
                mFaceHitsGpu[k] = hits[k];
            }
        }
        t.ticket->unmap();
        t.ticket.reset();
        mDemandNextTake = (mDemandNextTake + 1u) % kDemandSlots;
    }
    // 2. THE SLOTS TO OFFER: the resident instances', and every slot a face of which was marked within the window.
    for (const InstanceRec &inst : mInstances)
        if (inst.cardCount && inst.itemSlot != size_t(-1)) slots.push_back(uint32_t(inst.itemSlot));
    // ...AND WHILE THE REST's DECISION IS PENDING, every slot a face of which was marked in its rest span (F3: the
    //    decision's set is every hit face's cards, kept and arriving alike - a face read early in the span is offered
    //    as one read late is, never only within the window of its reader's clock)
    const bool restPending = mRestDue && !mRestDecided;
    for (const uint32_t s : mMarkedSlots) {   // (a slot never marked is never wanted)
        bool wanted = false;
        for (unsigned r = 0; r < kDemandReaders && !wanted; ++r)
            for (unsigned b = 0; b < 6u && !wanted; ++b) {
                // (the window offers what any hit marked; a pending decision, what its rest span marked)
                const unsigned long long m = mFaceMarks[s * kFaceMarkStride + 8u * r + b];
                const unsigned long long mr = mFaceMarks[s * kFaceMarkStride + 8u * restMarkOf(r) + b];
                wanted = (m && m + mDemandWindow >= mDemandNewest[r]) || (restPending && mr && mr >= mRestStart[r]);
            }
        if (wanted) slots.push_back(uint32_t(s));
    }
    std::sort(slots.begin(), slots.end());
    slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
}

void SurfaceCache::refreshResidency(const CardSceneView &view) {
    // A CARD THAT LEAVES CHANGES WHAT ITS INSTANCE's HITS READ (the voxels answer them from the next frame): its instance's
    // box joins the frame's relit boxes - the changed-region list's Card kind (PHOTON-7 stage 8B, B1: the gather's
    // cells that read it resume; an eviction is no input it may rest across)
    const auto departedBox = [this](size_t inst) {
        if (inst < mInstances.size()) mRelitBoxes.push_back(Ogre::Aabb(mInstances[inst].centre, mInstances[inst].halfSize));
    };
    // 1. WHAT THE SCENE STILL OFFERS. Membership is decided by the CANDIDATE
    //    LIST the scene hands over, so an item that was destroyed, hidden, made
    //    a mover or re-materialled out of the GI channel simply stops being
    //    offered and its pages come back — no door to remember to knock on.
    for (InstanceRec &inst : mInstances) inst.seen = false;
    bool changed = false;
    // THE MEASURING ARM's faces' identity (cards.demandCount alone): the node at each item slot, the candidates'
    if (mDemandCount)
        for (const CardSceneView::Candidate &cand : view.candidates)
            if (cand.itemSlot != size_t(-1)) {
                if (mSlotNode.size() <= cand.itemSlot) mSlotNode.resize(cand.itemSlot + 1u, 0);
                mSlotNode[cand.itemSlot] = cand.node;
            }
    // THE INSTANCES THAT MOVED: released here and re-admitted below as THE SAME FACES (the cards a reader read keep
    // their place in the layout, their demand and their flags' meaning) at the new transform.
    struct Kept { unsigned index = 0u; unsigned long long readBy[2] = {0ull, 0ull}; };
    std::unordered_map<NodeId, std::vector<Kept>> moved;
    for (const CardSceneView::Candidate &cand : view.candidates) {
        auto it = mByNode.find(cand.node);
        if (it == mByNode.end()) continue;
        InstanceRec &inst = mInstances[it->second];
        if (inst.item != cand.item) continue;   // a rebuilt Item is a new instance
        // THE SLOT IS THE SCENE'S, NOT THIS RECORD'S (V2 audit C-1, 2026-09-25):
        // a swap-on-remove renumbers the tail item into the freed slot
        // (OgreGi.cpp's removeItemSlot), and the ray hits address the instance
        // table by THAT slot (the TLAS custom index). The candidate carries the
        // node's current slot every frame: follow it, and re-sync the table when
        // it moved.
        if (inst.itemSlot != cand.itemSlot) {
            inst.itemSlot = cand.itemSlot;
            mTableDirty = true;
        }
        inst.seen = true;
        // THE TRANSFORM SIGNATURE, AND IT IS NOT THE BOX ALONE. A card's
        // rectangle and its FRAME are in WORLD space and come from the node's
        // derived orientation and scale — so a 90 degree turn of a crate, or
        // ANY turn of a sphere, leaves the world AABB exactly where it was
        // while every card now describes a different face. The box, the
        // orientation and the scale are all part of the signature for that
        // reason. The tolerance is the GI item walk's shape, for its reason: a
        // transform rewritten to the same value must not read as movement.
        const Ogre::Aabb &box = cand.box;
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
            std::vector<Kept> &keep = moved[cand.node];
            for (unsigned c = 0; c < inst.cardCount; ++c) {
                const CardRec &k = mCards[inst.firstCard + c];
                keep.push_back(Kept{ k.index, { k.readBy[0], k.readBy[1] } });
            }
            continue;
        }
        // THE MATERIAL THE INSTANCE WEARS, re-read every frame — because the
        // hover preview does not go through `noteMaterialChanged` at all.
        // MATERIAL-SWAP-GI-1's in-place swap (`OgreScene::setNodeMaterial`)
        // writes `n.materialRef` and swaps the datablock on the live Item
        // WITHOUT destroying or editing a material, which is the whole point of
        // it — so the only way the cache hears about a preview is by reading
        // the candidate's material again.
        if (inst.material != cand.material) {
            inst.material = cand.material;
            const unsigned long long root = 1ull << rootBit('r', mFrame);
            for (unsigned c = 0; c < inst.cardCount; ++c) {
                mCards[inst.firstCard + c].queued = true;
                mCards[inst.firstCard + c].roots |= root;
                mCards[inst.firstCard + c].sceneCause = true;   // (the edit's first round: CardScene)
            }
            ++mInvalidMaterial;
        }
    }
    for (size_t i = 0; i < mInstances.size(); ++i) {
        if (mInstances[i].cardCount && !mInstances[i].seen) {
            releaseInstance(i);
            changed = true;
        }
    }

    // THE DEMAND OPEN (PHOTON-7 stage 5's read W3): EACH READER SETTLES FROM ITS OWN FIRST STAMP since the last change -
    // reader r holds the demand open while its first falls within the settle S of the change and its clock has not
    // passed that first by S; and it is open while no reader has read since the change (for the window a first read
    // would open it in: an empty demand closes past it, below). So a reader that reads first
    // a few frames after the other keeps it open for its own S (the faster reader's clock closed it before: a gather
    // reading 8 frames late lost its faces), a reader that never reads holds nothing (the reflection with no card in
    // view: the readers' least clock never closed it), and a first graze long after the change cannot reopen it.
    // "Within S" allows ONE READ-BACK PERIOD (kDemandReadEvery): a cell keeps its newest stamp, so the first read-back
    // after a reader's first read shows that read's frame or a later one, at most a period later (without it a gather
    // first reading 8 frames after the change was excluded - its first harvested stamp past the settle - and the read
    // set of gi.card_residency (5) lost whole: 106 of 106).
    if (view.demandChanged) {
        mDemandChangeFrame = mFrame;
        mDemandFirst[0] = mDemandFirst[1] = 0ull;
        mRestDecided = false;   // a change reopens the window: the next rest takes its own decision
        mRestDue = 0ull;
        mWindowDrained = false;
    }
    const unsigned long long settleS = view.demandSettleFrames;
    // AN EMPTY DEMAND CLOSES (PHOTON-7 stage 10, the giAtRest contract; the lead's rule: no reader, nothing to wait for):
    // "open while no reader has read since the change" held only for the window a first read would have opened it in -
    // the settle S, one read-back period, and the read-back's lag. Past it, a cache no reader read (no view reads the
    // cards: gi.changed_regions' fixture, a sky scene, a screenshot's offscreen settle) takes its rest decision on an
    // EMPTY demand, never an open one for ever: GiStatus::giAtRest waits on that decision.
    Ogre::RenderSystem *rsw = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vaow = rsw ? rsw->getVaoManager() : nullptr;
    const unsigned long long readLag = (vaow ? vaow->getDynamicBufferMultiplier() + 1u : 1u) + 1u;
    mDemandOpenLag = unsigned(readLag);
    const bool demandOpen = demandWindowOpen(mFrame, mDemandChangeFrame, mDemandFirst, mDemandNewest, settleS,
                                             kDemandReadEvery, readLag);
    // (THE FORMULA: demandWindowOpen, below - its liveness clause: a reader that grazes the cards once and never again
    // froze its clock below first + S and held the window OPEN for ever, scripting.e2e.change_then_shot)
    // THE REST'S ONE DECISION (PHOTON-7 stage 10, R3 (b), the lead's ruling): with the demand closed, NOTHING ARRIVES
    // AND NOTHING LEAVES until the view readers' REST DEMAND IS COMPLETE (each has finished its rest sample sequence:
    // CardSceneView::readersRestComplete) - then, ONCE per change, the cards no view reader read over the rest are
    // drained (4b) and the faces they read over it that are not resident are admitted (step 3, free room) - so a walk
    // and a teleport resting at one pose, gathering the same rest demand, hold the same set. Its captures and their
    // card-to-card rounds are OWED (the decision is the root move, CardRec::roots), and the readers' hold waits on that
    // work (the queue and the relights are their inputs: gatherInputsBusy, the rest keys); a reader whose rest the
    // decision's own arrivals restarted completes its rest but never triggers a second decision - only a change
    // reopens the window. (Deleted with it: 10-2d's per-frame drain once closed and its closed-window "at this pose,
    // free room" admission - together the evict/re-admit cycle, ~700 of them in the crowded room's rest.)
    // ITS DEMAND IS THE REST SEQUENCE'S: the stamps each reader wrote from its own rest's start (the readers sample by
    // direction alone, so a walk and a teleport resting at one pose run the same rest sequence over the same geometry
    // and stamp the same faces - the settling frames before it sample by their path), read once a read-back has
    // landed past the rest's completion (the period + the lag + the issue frame's one: owed_rows.py's bound). A reader
    // with no rest (none in view, or one that cannot rest) counts its stamps since the change.
    if (!demandOpen && !view.demandChanged && !mRestDecided && !mRestDue && view.readersRestComplete) {
        // (THE WINDOW'S FIRST STAMP: the readers' rest length is as of their last record, a frame late - its n rest frames
        // are the cache frames mFrame - n .. mFrame - 1, each stamped with its own (this frame's stamp is uploaded below,
        // after the plan). One frame earlier took the last SETTLING frame's reads into the rest demand: measured on
        // gi.card_residency (8), pillar 29's top card read at 573 in both held runs, its rest starting at 574 in one and
        // 581 in the other - kept in one, drained in the other, the resident sets one card apart and 18.8 codes.)
        // THE DEMAND IS EACH READER'S OWN REST INDICES [1, L] (PHOTON-7 stage 10, the per-reader snapshot): its readers
        // stamp only those (L = N + the due, the readers' gate: jah_rq_card.glsl's JAH_HIT_PARAMS.cards.z), so the
        // stamps at or after its rest's start are exactly its span's, however long it has rested since. THE OLD WINDOW
        // was every stamp since each reader's rest began, up to the decision: its length each reader's FULL rest then -
        // path dependent once (ii) let the reflection rest through sub-quantum relights (gi.card_residency (6): a face
        // read once in 16 admitted in the pinned teleport, not the walk). A reader that went stale makes no demand.
        for (unsigned r = 0; r < kDemandReaders; ++r) {
            const unsigned long long n = view.readerRestFrames[r];
            mRestStart[r] = !view.readerActive[r] ? ~0ull
                                                  : n ? (mFrame > n ? mFrame - n : 1ull) : mDemandChangeFrame + 1ull;
        }
        mRestDue = mFrame + demandDueFrames();
        mDecisionReaders = (view.readerActive[0] ? 1u : 0u) | (view.readerActive[1] ? 2u : 0u);
    }
    const bool decide = mRestDue && mFrame >= mRestDue && !demandOpen && !view.demandChanged && !mRestDecided;
    if (decide) {
        mRestDecided = true;
        ++mRestDecisions;
    }
    const auto inRest = [this](unsigned r, unsigned long long stamp) { return stamp && stamp >= mRestStart[r]; };
    // 2. THE CARDS, PER INSTANCE, while the set changes (flattened back once at the end; a card's `size` 0 = evicted).
    std::vector<std::vector<CardRec>> per(mInstances.size());
    for (size_t i = 0; i < mInstances.size(); ++i)
        for (unsigned c = 0; c < mInstances[i].cardCount; ++c) per[i].push_back(mCards[mInstances[i].firstCard + c]);

    // 3. WHAT THE READERS WANT THAT IS NOT RESIDENT: per offered candidate, the cards of the faces a hit marked within
    //    the window (and a moved instance's own faces), ordered by the mark (newest first), then the size on screen
    //    (largest first), the node and the index.
    struct Arrival {
        const CardSceneView::Candidate *cand = nullptr;
        CardRec card;
        unsigned long long mark = 0ull;
        float size = 0.0f;
        bool persistent = false;   // marked in two read-backs within its reader's window (or a moved card's own)
    };
    // 4. THE EVICTABLE: resident cards no reader read for a window, least recently read first, then the smallest on
    //    screen, the node, the index.
    struct Evictable {
        size_t inst = 0, pos = 0;
        unsigned long long read = 0ull;
        float size = 0.0f;
        NodeId node = 0;
        unsigned index = 0u;
    };
    std::vector<Evictable> evictable;
    for (size_t i = 0; i < per.size(); ++i)
        for (size_t k = 0; k < per[i].size(); ++k) {
            const CardRec &c = per[i][k];
            // FRESH FOR A WINDOW of a reader's clock after that reader's last read, or of the newest clock after its last
            // capture; one still waiting for its first capture is wanted (an arrival the budget has not reached) and
            // never evicted.
            bool fresh = !c.lastUpdated || c.lastUpdated + mDemandWindow >= std::max(mDemandNewest[0], mDemandNewest[1]);
            for (unsigned r = 0; r < kDemandReaders && !fresh; ++r)
                fresh = c.readBy[r] && c.readBy[r] + mDemandWindow >= mDemandNewest[r];
            if (fresh) continue;
            evictable.push_back(Evictable{ i, k, c.lastRead, projectedSize(c, view.viewerPos), mInstances[i].node,
                                           c.index });
        }
    std::sort(evictable.begin(), evictable.end(), [](const Evictable &a, const Evictable &b) {
        if (a.read != b.read) return a.read < b.read;
        if (a.size != b.size) return a.size < b.size;
        if (a.node != b.node) return a.node < b.node;
        return a.index < b.index;
    });

    // A FULL ATLAS THAT CAN MAKE NO ROOM BUILDS NO ARRIVALS (scale.cpu_walks, PHOTON-7 10-3, the lead's ruling: the churn is
    // a defect). With no free slot of any size, no page a compaction could empty, nothing evictable and no drain or door
    // due, every arrival is refused - the non-persistent at once, the persistent after a compaction that fails and an
    // eviction list that is empty - so building them is the only thing the frame does with them: a 10k world's open
    // demand re-built 3,500-8,200 refused arrivals a frame (each face's layout, the sort). The decision holds until the
    // room changes: a release, a drain, an eviction becoming due, a move.
    const bool drainDue = !demandOpen && !view.demandChanged && !mWindowDrained;
    const bool roomPossible = decide || drainDue || view.forcedCardMode != 0 || !evictable.empty() || atlasHasRoom();
    std::vector<Arrival> arriving;
    std::vector<CardRec> layout;
    for (const CardSceneView::Candidate &cand : view.candidates) {
        if (decide || !roomPossible) break;   // (the decision's set is its fit's: 4c; no room: above)
        if (!cand.item || !cand.cards || cand.cards->empty()) continue;
        auto mv = moved.find(cand.node);
        unsigned long long marks[2][6] = {};
        bool twice[6] = {};
        bool any = mv != moved.end();
        if (cand.itemSlot != size_t(-1) && (cand.itemSlot + 1u) * kFaceMarkStride <= mFaceMarks.size())
            for (unsigned r = 0; r < kDemandReaders; ++r)
                for (unsigned b = 0; b < 6u; ++b) {
                    // (the decision reads the rest span's block, the window each reader's own: the two jobs of the mark)
                    const size_t k = cand.itemSlot * kFaceMarkStride + 8u * (decide ? restMarkOf(r) : r) + b;
                    const unsigned long long m = mFaceMarks[k];
                    // KEPT AND ARRIVED JUDGED ALIKE AT THE DECISION (PHOTON-7 stage 10, gi.card_residency (6)): a face
                    // the readers marked anywhere in their demand span is a candidate - as a resident card read anywhere
                    // in it is kept - never only one marked within the window S of its reader's clock: a face read once
                    // early in the span was admitted in one run and not the other (the walk's resident from its history,
                    // the teleport's filtered out). Outside the decision the window rules, as before.
                    if (m && (decide ? inRest(r, m) : m + mDemandWindow >= mDemandNewest[r])) {
                        marks[r][b] = m;
                        any = true;
                        twice[b] = twice[b] || (mFacePrev[k] && mFacePrev[k] + mDemandWindow >= mDemandNewest[r]);
                    }
                }
        // (THE TEST DOOR cards.force: a card held resident arrives whatever its demand; its instance's other cards keep the
        // rules)
        const bool forcedNode = view.forcedCardMode > 0 && cand.node == view.forcedCardNode;
        bool eligible = any;
        if (!any && !forcedNode) continue;
        // CLOSED (CardSceneView::demandChanged): a moved instance's own cards come back - and AT THE REST'S ONE DECISION
        // (R3 (b)) a face the readers marked AT THIS POSE (since the demand's last change: the whole rest's demand) takes
        // FREE room, never displacing one: what the decision's drain freed goes to the pose's own demand, so the
        // resident set at rest is the pose's
        if (eligible && !demandOpen && mv == moved.end()) {
            // closed: nothing arrives but at the rest's one decision (R3 (b))
            bool here = false;
            for (unsigned r = 0; r < kDemandReaders && !here; ++r)
                for (unsigned b = 0; b < 6u && !here; ++b) here = inRest(r, marks[r][b]);
            eligible = decide && here;
        }
        if (!eligible && !forcedNode) continue;
        layoutCards(cand, layout);
        auto it = mByNode.find(cand.node);
        const std::vector<CardRec> *have = it != mByNode.end() ? &per[it->second] : nullptr;
        for (const CardRec &c : layout) {
            if (have && std::any_of(have->begin(), have->end(),
                                    [&c](const CardRec &h) { return h.size && h.index == c.index; }))
                continue;
            const bool held = view.forcedCardMode && cand.node == view.forcedCardNode &&
                              (c.index == view.forcedCardIndex || view.forcedCardIndex == 63u);   // (63: every card of it)
            if (held && view.forcedCardMode < 0) continue;   // held absent
            if (held) {   // held resident: arrives now, persistent
                Arrival a;
                a.cand = &cand;
                a.card = c;
                a.card.lastRead = mFrame;
                a.mark = mFrame;
                a.persistent = true;
                a.size = projectedSize(c, view.viewerPos);
                arriving.push_back(a);
                continue;
            }
            if (!eligible) continue;
            // each reader's mark of this face (a moved instance's card: what each reader had read it at, or its clock now)
            unsigned long long by[2] = { marks[0][c.bucket], marks[1][c.bucket] };
            bool kept = false;
            if (mv != moved.end())
                for (const Kept &k : mv->second)
                    if (k.index == c.index) {
                        kept = true;
                        for (unsigned r = 0; r < kDemandReaders; ++r)
                            if (k.readBy[r]) by[r] = std::max({ by[r], k.readBy[r], mDemandNewest[r] });
                    }
            // (a moved card no reader had read yet - admitted, waiting - comes back all the same)
            const unsigned long long mark = std::max({ by[0], by[1], kept ? 1ull : 0ull });
            if (!mark) continue;
            if (!demandOpen && !kept && !inRest(0u, by[0]) && !inRest(1u, by[1])) continue;   // (closed: the rest's marks alone)
            Arrival a;
            a.cand = &cand;
            a.card = c;
            a.card.readBy[0] = by[0];
            a.card.readBy[1] = by[1];
            a.card.lastRead = mark;
            a.mark = mark;
            a.persistent = kept || (demandOpen && twice[c.bucket]);
            a.size = projectedSize(c, view.viewerPos);
            arriving.push_back(a);
        }
    }
    std::sort(arriving.begin(), arriving.end(), [](const Arrival &a, const Arrival &b) {
        if (a.mark != b.mark) return a.mark > b.mark;
        if (a.size != b.size) return a.size > b.size;
        if (a.cand->node != b.cand->node) return a.cand->node < b.cand->node;
        return a.card.index < b.card.index;
    });


    // 4b. AT REST, THE POSE'S DEMAND ALONE (PHOTON-7 stage 10, 10-2d; the lead's ruling - residency is path-independent
    //     at rest): AT THE REST'S ONE DECISION (R3 (b)), a captured card NO VIEW READER read since the demand's last
    //     change (the gather, the reflection, the pixel at THIS pose - the cache's own gather is no demand: its reads
    //     self-sustain and fill the atlas, measured) LEAVES, even with free room. Since cards read cards, an answer
    //     depends on which cards are resident, and a walk kept the cards it read on its way while there was room (the
    //     walk 264, the teleport 248: gi.card_residency (6) RED, 93.5 codes with the placement pinned); the drain and
    //     the decision's admission of the pose's own marks (step 3) give both paths the pose's set.
    //     AND ONCE BEFORE IT, WHEN THE WINDOW CLOSES (R3, the lead's Q2): the cards no view reader read in the S-frame
    //     window since the change leave - once, never per frame - so the motion's unread cards stop working before the
    //     rest; the decision still drains and admits on the rest's demand, and the final set is the pose's alone (a card
    //     this drops and the rest re-admits costs one re-capture).
    const bool windowDrain = !demandOpen && !view.demandChanged && !mWindowDrained;
    if (windowDrain) mWindowDrained = true;
    bool drained = false, forcedLeft = false;
    const auto heldCard = [&](size_t inst, const CardRec &c) {
        return view.forcedCardMode && inst < mInstances.size() && mInstances[inst].node == view.forcedCardNode &&
               (c.index == view.forcedCardIndex || view.forcedCardIndex == 63u);
    };
    if (view.forcedCardMode < 0)
        for (size_t i = 0; i < per.size(); ++i)
            for (CardRec &c : per[i])
                if (c.size && heldCard(i, c)) {   // (cards.force: held absent)
                    freeRect(c.atlasX, c.atlasY, c.size);
                    c.size = 0u;
                    ++mEvicted;
                    changed = true;
                    mDeparted = true;
                    forcedLeft = true;
                    departedBox(i);
                }
    if (windowDrain && !decide) {
        for (size_t i = 0; i < per.size(); ++i)
            for (CardRec &c : per[i]) {
                if (!c.size || !c.lastUpdated) continue;   // (one waiting for its capture arrived at this pose)
                if (view.forcedCardMode > 0 && heldCard(i, c)) continue;   // (cards.force: held resident)
                if (std::max(c.readBy[0], c.readBy[1]) > mDemandChangeFrame) continue;   // read in the window
                freeRect(c.atlasX, c.atlasY, c.size);
                c.size = 0u;
                ++mEvicted;
                ++mRestDrained;
                changed = true;
                drained = true;
                mDeparted = true;
                departedBox(i);
            }
    }
    if (windowDrain && drained) ++mWindowDrains;

    // 4c. THE DECISION's SET (PHOTON-7 stage 10-3, F3 and its fit, the lead's ruling): THE CARDS OF THE FACE BUCKETS HIT
    //     IN THE REST SPAN, kept and arriving alike - every hit marks its bucket, answered or not (jah_rq_card.glsl), so
    //     the set is a function of the rays' hits at the pose and the rest index, never of which cards were resident or
    //     captured. RANKED by one run-independent key - the bucket's span read count (the rest span's read-backs that
    //     carried a new mark of it, both readers summed: mFaceRing), descending, then the card's size (larger first),
    //     the node id and the card index - and THE SET IS THE LONGEST PREFIX THAT FITS. NOT AREA: a page holds cards of
    //     ONE size (allocRect cuts a page to a single power of two), so a set whose area fits can still not pack; the
    //     fit is the packer's own deterministic count, sum over sizes of ceil(cards / slots a page), against the pages -
    //     exact once the pages are compacted (compactOnePage empties a page of a size into the others' free slots, so a
    //     size's cards end in ceil(n / slots) pages). Resident cards outside the prefix leave; the prefix's others arrive
    //     (compacting when a rect is short, never evicting). Never "free room only": with the atlas full that made the
    //     set follow the history of what was resident.
    struct Want {
        const CardSceneView::Candidate *cand = nullptr;
        CardRec card;
        unsigned count = 0u;
        size_t inst = size_t(-1), pos = 0;   // resident: its place in `per`
        bool held = false;
    };
    std::vector<Want> wants;
    if (decide) {
        const auto spanCount = [this](size_t slot, unsigned bucket) {
            unsigned n = 0u;
            if (slot == size_t(-1) || (slot + 1u) * kFaceMarkStride > mFaceMarks.size()) return n;
            for (unsigned r = 0; r < kDemandReaders; ++r) {
                const size_t k = slot * kFaceMarkStride + 8u * restMarkOf(r) + bucket;   // (the rest span's marks)
                for (unsigned e = 0; e < kFaceRing; ++e) {
                    const uint32_t m = mFaceRing[k * kFaceRing + e];
                    n += (m && m >= mRestStart[r]) ? 1u : 0u;
                }
            }
            return n;
        };
        std::vector<CardRec> lay;
        for (const CardSceneView::Candidate &cand : view.candidates) {
            if (!cand.item || !cand.cards || cand.cards->empty()) continue;
            unsigned counts[6] = {};
            bool anyHit = false;
            for (unsigned b = 0; b < 6u; ++b) {
                counts[b] = spanCount(cand.itemSlot, b);
                anyHit = anyHit || counts[b] > 0u;
            }
            const bool forcedNode = view.forcedCardMode > 0 && cand.node == view.forcedCardNode;
            if (!anyHit && !forcedNode) continue;
            layoutCards(cand, lay);
            auto it = mByNode.find(cand.node);
            const size_t inst = it != mByNode.end() ? it->second : size_t(-1);
            for (const CardRec &c : lay) {
                const bool forcedCard = cand.node == view.forcedCardNode &&
                                        (c.index == view.forcedCardIndex || view.forcedCardIndex == 63u);
                if (view.forcedCardMode < 0 && forcedCard) continue;   // (cards.force: held absent)
                const bool held = forcedNode && forcedCard;
                if (!held && counts[c.bucket] == 0u) continue;
                Want w;
                w.cand = &cand;
                w.card = c;
                w.count = counts[c.bucket];
                w.held = held;
                if (inst != size_t(-1))
                    for (size_t k = 0; k < per[inst].size(); ++k)
                        if (per[inst][k].size && per[inst][k].index == c.index) { w.inst = inst; w.pos = k; break; }
                wants.push_back(w);
            }
        }
        std::sort(wants.begin(), wants.end(), [](const Want &a, const Want &b) {
            if (a.held != b.held) return a.held;
            if (a.count != b.count) return a.count > b.count;
            if (a.card.size != b.card.size) return a.card.size > b.card.size;
            if (a.cand->node != b.cand->node) return a.cand->node < b.cand->node;
            return a.card.index < b.card.index;
        });
        // THE FIT: the longest prefix whose pages, size by size, fit the atlas
        const size_t pages = mPageOwner.size();
        unsigned bySize[8] = {};   // log2(size / kCardMinSize)
        size_t fit = 0;
        for (; fit < wants.size(); ++fit) {
            unsigned s = 0u;
            while ((kCardMinSize << s) < wants[fit].card.size && s < 7u) ++s;
            ++bySize[s];
            size_t need = 0;
            for (unsigned q = 0; q < 8u; ++q) {
                const unsigned side = kCardPageSize / std::max(kCardMinSize << q, 1u);
                const unsigned slots = side ? side * side : 1u;
                need += (bySize[q] + slots - 1u) / slots;
            }
            if (need > pages) { --bySize[s]; break; }
        }
        // the residents outside the prefix leave (a held one stays)
        std::vector<std::vector<char>> keep(per.size());
        for (size_t i = 0; i < per.size(); ++i) keep[i].assign(per[i].size(), 0);
        for (size_t k = 0; k < fit; ++k)
            if (wants[k].inst != size_t(-1)) keep[wants[k].inst][wants[k].pos] = 1;
        for (size_t i = 0; i < per.size(); ++i)
            for (size_t k = 0; k < per[i].size(); ++k) {
                CardRec &c = per[i][k];
                if (!c.size || keep[i][k] || (view.forcedCardMode > 0 && heldCard(i, c))) continue;
                // READ IN MOTION (8B, the lead's check; its SCOPE, the lead's ruling on lab.card_set_binds and
                // gi.card_residency_crowded (4)): the decision ranks the rest span's faces alone, so a card only cells that
                // never rest read (a mover resuming them every frame - K 0, no face marks) is outside the prefix; it stays
                // while a reader that reads it (fresh, the moving residency's window) is MOVING - that reader's rest not
                // begun - and leaves the moment that reader rests or stops reading it. At a rest decision every reader
                // rests, so the set is the prefix exactly (one set at one pose by any path); the keep blocks eviction
                // alone - compaction moves a kept card like any resident.
                bool readMoving = false;
                for (unsigned r = 0; r < kDemandReaders && !readMoving; ++r)
                    readMoving = view.readerActive[r] && view.readerRestFrames[r] == 0u && c.readBy[r] &&
                                 c.readBy[r] + mDemandWindow >= mDemandNewest[r];
                if (readMoving) continue;
                freeRect(c.atlasX, c.atlasY, c.size);
                c.size = 0u;
                ++mEvicted;
                ++mRestDrained;
                changed = true;
                mDeparted = true;
                departedBox(i);
            }
        // ...and the prefix's others arrive, in rank order (placed below: compaction, never eviction)
        arriving.clear();
        for (size_t k = 0; k < fit; ++k) {
            if (wants[k].inst != size_t(-1)) continue;
            Arrival a;
            a.cand = wants[k].cand;
            a.card = wants[k].card;
            a.card.lastRead = mFrame;
            a.mark = mFrame;
            a.persistent = true;
            a.size = projectedSize(wants[k].card, view.viewerPos);
            arriving.push_back(a);
        }
        mDecisionRefusedCards.clear();
        mDecisionRefused = unsigned(wants.size() - fit);
        for (size_t k = fit; k < wants.size(); ++k)
            mDecisionRefusedCards.push_back({ wants[k].cand->node, wants[k].card.index, wants[k].card.size,
                                              (unsigned long long)wants[k].count });
    }

    // 5. THE ARRIVALS, in order. Free room takes any wanted face at its first mark (the fill is the readers' first
    //    frames). A face that finds none MAKES room - the atlas compacted, then the evictable evicted, until it fits or
    //    nothing evictable is left - only when its demand PERSISTS: marked in two read-backs within its reader's window
    //    (or a moved card's own). A face a stochastic reader grazes once in a window displaces nothing - measured on
    //    photon-glass: the atlas full, the faces its readers reach once in hundreds of frames swapped one for another
    //    for ever, and every capture broke the reflection's rest (reflectionReadsKey folds the cache's captures). (And
    //    once the demand closes - a still pose over a still world, CardSceneView::demandChanged - nothing arrives.)
    size_t nextEvict = 0;
    mArrivalMarkNewest = 0ull;
    // THE ARRIVALS' ROOT (R3 (c)): this update's arrivals are one move - the rest's decision's, or an arrival's
    const unsigned long long arrivalRoot = arriving.empty() ? 0ull : 1ull << rootBit(decide ? 'd' : 'a', mFrame);
    if (decide) {
        // (the decision's room, said: the atlas in use and free before its arrivals are placed)
        unsigned long long used = 0ull;
        for (const std::vector<CardRec> &cards : per)
            for (const CardRec &c : cards) used += c.size ? (unsigned long long)c.size * c.size : 0ull;
        mDecisionUsed = used;
        mDecisionFree = (unsigned long long)kCardAtlasSize * kCardAtlasSize - used;
        mDecisionAdmitted = 0u;   // (its refusals are the fit's: 4c)
    }
    // A FULL ATLAS SAYS SO ONCE A SIZE (scale.cpu_walks, PHOTON-7 10-3: a 10k world's mover frame turned away every
    // arrival of a full atlas with three page walks and a compaction attempt EACH - 22.6 ms of the cards stage in Debug
    // against the 10-22 ship's 0.14). Within this loop the atlas only loses room - an allocation takes a slot - until an
    // eviction frees one or a compaction empties a page, so a size refused once stays refused, and a compaction that
    // failed stays failed, until then: the same placements, without repeating a refusal.
    bool noRoom[kCardPageSize + 1u] = {};
    bool compactFailed = false;
    const auto roomChanged = [&]() {
        std::fill(std::begin(noRoom), std::end(noRoom), false);
        compactFailed = false;
    };
    const auto place = [&](CardRec &c) {
        const unsigned sz = std::max(kCardMinSize, std::min(kCardPageSize, pow2Ceil(c.size)));
        if (noRoom[sz]) return false;
        if (allocRect(c.size, c.atlasX, c.atlasY)) return true;
        noRoom[sz] = true;
        return false;
    };
    for (const Arrival &a : arriving) {
        CardRec c = a.card;
        c.roots = arrivalRoot;
        c.round = 0u;
        bool placed = place(c);
        while (!placed && a.persistent) {
            if (!compactFailed) {
                if (compactOnePage(per, arrivalRoot)) { roomChanged(); placed = place(c); continue; }
                compactFailed = true;
            }
            if (decide) break;   // (the decision's set fits by its count: it compacts, it never evicts)
            if (nextEvict >= evictable.size()) break;
            const Evictable &e = evictable[nextEvict++];
            CardRec &gone = per[e.inst][e.pos];
            // (the list was built before this loop's compaction: a card it MOVED waits for its re-capture - never
            // evicted while waiting, the rule above; measured, gi.card_residency (4)'s crowded arrival evicted 36 cards
            // its own compaction had just moved, the move wasted - PHOTON-7 stage 5's read W5)
            if (!gone.size || !gone.lastUpdated) continue;
            if (view.forcedCardMode > 0 && heldCard(e.inst, gone)) continue;   // (cards.force: held resident)
            freeRect(gone.atlasX, gone.atlasY, gone.size);
            gone.size = 0u;
            ++mEvicted;
            changed = true;
            mDeparted = true;
            departedBox(e.inst);
            roomChanged();
            placed = place(c);
        }
        if (decide) {
            if (placed) ++mDecisionAdmitted;
            else {
                ++mDecisionRefused;
                mDecisionRefusedCards.push_back({ a.cand->node, c.index, c.size, a.mark });
            }
        }
        if (!placed) continue;
        c.since = mFrame;
        c.queued = true;
        c.lastUpdated = 0ull;
        auto it = mByNode.find(a.cand->node);
        size_t idx;
        if (it == mByNode.end()) {
            InstanceRec inst;
            inst.node = a.cand->node;
            inst.itemSlot = a.cand->itemSlot;
            inst.item = a.cand->item;
            inst.material = a.cand->material;
            inst.seen = true;
            inst.centre = a.cand->box.mCenter;
            inst.halfSize = a.cand->box.mHalfSize;
            inst.rotation = a.cand->sceneNode->_getDerivedOrientation();
            inst.scale = a.cand->sceneNode->_getDerivedScale();
            idx = mInstances.size();
            mInstances.push_back(inst);
            per.emplace_back();
            mByNode[a.cand->node] = idx;
        } else {
            idx = it->second;
        }
        per[idx].push_back(c);
        ++mAdmitted;
        mArrivalMarkNewest = std::max(mArrivalMarkNewest, a.mark);
        changed = true;
    }

    // 6. THE VECTORS, REBUILT when anything changed: every instance's cards in one block, in layout order, the
    //    departed and evicted gone (an instance with none left is released: its next marked face brings it back).
    if (changed) {
        std::vector<InstanceRec> keptInst;
        std::vector<CardRec> keptCards;
        keptInst.reserve(mInstances.size());
        keptCards.reserve(mCards.size());
        mByNode.clear();
        for (size_t i = 0; i < mInstances.size(); ++i) {
            InstanceRec &inst = mInstances[i];
            if (!inst.node) continue;
            std::vector<CardRec> &list = per[i];
            list.erase(std::remove_if(list.begin(), list.end(), [](const CardRec &c) { return c.size == 0u; }),
                       list.end());
            if (list.empty()) continue;
            std::sort(list.begin(), list.end(), [](const CardRec &a, const CardRec &b) { return a.index < b.index; });
            inst.firstCard = unsigned(keptCards.size());
            inst.cardCount = unsigned(list.size());
            for (CardRec &r : list) {
                r.instance = unsigned(keptInst.size());
                keptCards.push_back(r);
            }
            mByNode[inst.node] = keptInst.size();
            keptInst.push_back(inst);
        }
        mInstances.swap(keptInst);
        mCards.swap(keptCards);
        mTableDirty = true;
        ++mPhotonGeneration;
    }
    // THE DEPARTED CARDS' READERS (R3: residency path-independent at rest): a card that left takes its light from what
    // read it - they read the voxels there now - and no move reports a departure, so at the window's drain and at the
    // rest's decision every resident card's indirect is re-marched ONCE (named by that drain's root) when any card left
    // since the last time: the rounds after it converge on the final set's own light, whatever path filled it.
    // ...and AT ONCE when the cards.force door holds a resident card absent (L1's test door models a REAL departure:
    // its readers re-marched as a drain's are, root 'f' - a departure left to the next stop's decision kept them stale
    // at rest, the no-pop row's 0 codes that measured nothing).
    if (((decide || windowDrain) && mDeparted) || forcedLeft) {
        mDeparted = false;
        const unsigned long long root = 1ull << rootBit(decide ? 'd' : windowDrain ? 'w' : 'f', mFrame);
        for (CardRec &c : mCards)
            if (c.size && c.indirectValid) {
                c.relightIndirect = true;
                c.roots |= root;
            }
    }
    unsigned pagesUsed = 0u;
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

void SurfaceCache::setAsyncShaders(bool on) {
    mAsyncShaders = on;
    if (mWs) mWs->setAsyncShaderCompile(on);
}

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
    const Ogre::RenderQueue *rq = mWs->getSceneManager()->getRenderQueue();
    mBatchPlaceholderAt = rq->getNumPlaceholderDraws();
    mBatchSkipsAt = rq->getNumPendingSkips();
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
    // THE CARD'S OWN LOD (fork 5230c9390 (was 0085)): the level is a capture DECISION (the
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
    // photon_update (the next workspace) traces and relights, whatever happened here.
    mPosUpdated = true;
    if (mBatch.empty()) return;
    const auto tB = std::chrono::steady_clock::now();
    // A BATCH THAT WAITED FOR A SHADER IS NOT A CAPTURE (see makeWorkspace): nothing is
    // copied, the cards stay queued and the next batch that finds the shaders built takes them.
    {
        const Ogre::RenderQueue *rq = mWs->getSceneManager()->getRenderQueue();
        const unsigned long long placeholders = rq->getNumPlaceholderDraws() - mBatchPlaceholderAt;
        const unsigned long long skips = rq->getNumPendingSkips() - mBatchSkipsAt;
        CaptureAsyncDraws &acc = gCaptureAsyncDraws[mWs->getSceneManager()];
        acc.placeholders += placeholders;
        acc.skips += skips;
        if (placeholders || skips) {
            // ...NOR IS IT RELIT: planRelights listed this batch's cards as captured this frame
            // (a capture and its relight are one update). Their texels were never copied — on a
            // new scene's first batch the atlas layers are not even initialised (Depth and
            // Emissive, plain copy targets, still UNDEFINED: measured, the relight sampled them
            // and the validation layer said so on an OPEN while compiles were pending). They
            // keep their flags and are relit with the batch that captures them.
            for (size_t r = mRelight.size(); r-- > 0;)
                if (std::find(mBatch.begin(), mBatch.end(), mRelight[r]) != mBatch.end()) {
                    mRelight.erase(mRelight.begin() + std::ptrdiff_t(r));
                    mRelightMode.erase(mRelightMode.begin() + std::ptrdiff_t(r));
                }
            mBatch.clear();
            mWs->setExecutionMask(0u);
            return;
        }
    }
    // THE CAPTURE'S MONITOR ROW (PHOTON-2 stage 0, the stillness door's (ii)): the cards this batch copied into the
    // atlas — each a step of the radiance its hits read once relit. The card workspace's passes are rows of their own
    // (JahCardCapture); the COPIES below run after them, outside every pass, so this row times them (PHOTON-7's naming
    // step: 0.1-0.2 ms a burst was a gap no row named) and carries the count.
    detail::monitor::CacheScope captureRow(CacheKind::Gi, WorkReason::Added, 0, "cards.capture",
                                           Ogre::Root::getSingleton().getRenderSystem(), this);
    captureRow.setUnits(unsigned(mBatch.size()));
    // ...AND INTO THE ATLAS, after the whole batch: six small copies a card,
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
        card.openValid = false;   // (its still trace writes the open layer: until then it reads open)
        ++mPhotonGeneration;
        ++mCaptures;
        ++mCapturesLastFrame;
        mTexelsLastFrame += card.size * card.size;
    }
    // THE COPY SESSION CLOSES HERE, with the copies (ASYNC-SHADERS-1, a validation defect on
    // base: oldLayout-01197 / VUID-vkCmdDraw-None-09600 on cardShadowRough at a scene's first
    // captures). The copies leave Ogre's copy encoder OPEN with the layers in TRANSFER_DST;
    // closing it moves them back to their resting layout AND re-asserts that layout in the
    // barrier solver. Left open, the first reader below (the relight, whenever the still trace
    // waits and so does not close it) resolves its barrier against the open session, the
    // closing inside that very barrier then overwrites the solver's record with GENERAL, and
    // the next reader transitions a SHADER_READ image "from GENERAL".
    if (!mBatch.empty()) Ogre::Root::getSingleton().getRenderSystem()->endCopyEncoder();
    captureRow.close();
    const auto tC = std::chrono::steady_clock::now();
    mWsMs = float(std::chrono::duration<double, std::milli>(tB - mBatchStart).count());
    mCopyMs = float(std::chrono::duration<double, std::milli>(tC - tB).count());
    mCaptureMs = mWsMs + mCopyMs;
    mStillCpuMs = 0.0f;
    mBatchCopied = true;   // photon_update traces the batch's term, then closes it
}

void SurfaceCache::photonUpdate() {
    if (!mPosUpdated) return;
    mPosUpdated = false;
    if (mFrozen) return;   // THE GI FREEZE (M1): nothing traced or relit
    // THE MOVERS' TERM of the cards just captured and of every card a mover's
    // footprint reaches — after the copies (the trace reads the new Depth and
    // Normal) and before the relight that multiplies it in. The batch's STILL sun
    // term is part of its capture: its CPU is in captureMs.
    // ITS PHASES ARE ROWS (PHOTON-7's naming step: cards.sun, cards.light, cards.tables.pass), each filed only when it
    // dispatched or submitted - the update pass's own time is left to none.
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    {
        detail::monitor::PhaseScope phase("cards.sun", rs);
        traceSun();
    }
    if (mBatchCopied) {
        mBatchCopied = false;
        mCaptureMs += mStillCpuMs;
        mBatch.clear();
        mWs->setExecutionMask(0u);
    }
    // ...and the cards just captured are relit from their new texels, in the same
    // frame, before anything could read them — and the table carries their flags
    // (captured; indirect stale or marched) before the ray read, later in this
    // frame, looks.
    {
        detail::monitor::PhaseScope phase("cards.light", rs);
        relightCards();
    }
    detail::monitor::PhaseScope phase("cards.tables.pass", rs);   // (the plan's own sync is cards.tables)
    if (syncBuffers()) phase.noteWork();
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
// lights (the shape PhotonVoxelLighting's own light injection takes).
namespace {
/// The most cards one frame relights (the relight list's size, 80 B a card).
constexpr unsigned kMaxRelights = 1024u;
/// The most lights the job sums (144 B a light after a 16 B count).
constexpr unsigned kMaxCardLights = 64u;
/// A relight record: rect, the capture camera, the three axes, and THE CARD'S LAMP TABLE (kCardLampTable = 48 world
/// ids: PHOTON-7 stage 5, 5-3; 10-4 R9 (3)) - 272 B.
constexpr unsigned kRelightFloats = 20u + kCardLampTable;
/// A lamp's world id, as every lamp table and light record carries it (32 bits of the document's node id; a light no
/// document made takes its Ogre id in the upper half's own range).
unsigned lampIdOf(const Ogre::Light *l) {
    const unsigned long long d = SceneLightList::documentId(l);
    return d ? unsigned(d & 0x7FFFFFFFull) : unsigned(0x80000000u | (l->getId() & 0x7FFFFFFFu));
}
/// The chain's parameter block (CardGiParams in the shader): chainInvRes[8],
/// chainFromPrev[14], the volume's origin and inverse size, the counts, the
/// environment's gain and mips, its nine SH coefficients — 35 vec4.
constexpr unsigned kGiParamFloats = 37u * 4u;   // ... + the cloud shadow's map and sun (CLOUDS-2D-2)
constexpr unsigned kMaxCardCascades = 8u;
/// A light's record: position, diffuse, attenuation, spot direction, spot parameters, and an AREA light's rectangle
/// (its four corners, the first's w its double-sidedness: PHOTON-7 stage 5, 5-3).
constexpr unsigned kLightFloats = 36u;
}   // namespace

void SurfaceCache::planRelights(const CardSceneView &view) {
    mRelight.clear();
    mRelightMode.clear();
    mVct = view.vct;
    mPlanFrame = view.frame;
    // this frame's captures (the batch is cleared before the relight: the relight's arrivals, 10-2c)
    mFreshCards.assign(mBatch.begin(), mBatch.end());
    // THE e = 0 COUNTER PER CARD (10-2a): every gathered card's sky rays the ray tier read back since the last plan
    if (mMoverHooks.gatherSky) {
        std::vector<std::pair<unsigned, unsigned>> sky;
        mMoverHooks.gatherSky(sky);
        for (const std::pair<unsigned, unsigned> &e : sky)
            if (e.first < mSlotCard.size() && mSlotCard[e.first] >= 0)
                mCards[size_t(mSlotCard[e.first])].gatherSky = (long long)e.second;
    }
    mIndirectRays = view.indirectRays;
    mRayLastShell = view.rayLastShell;
    mCloudField = view.cloudField;
    for (int k = 0; k < 4; ++k) { mCloudMap[k] = view.cloudMap[k]; mCloudSun[k] = view.cloudSun[k]; }
    for (int k = 0; k < 27; ++k) mAmbientSh[k] = view.ambientSh ? view.ambientSh[k] : 0.0f;
    // THE STALE SET (PHOTON-7 stage 5, 5-1; L2): a card is relit when what it holds CHANGED - never on a serial of
    // every light write (C4: every card on any light) or of every re-injection (C5: every card's indirect at every rest
    // and commit), both deleted. THE DIRECT HALF: a card whose box meets a LAMP region (the lamp's old and new spheres,
    // exact by its range) or every card on the sun's overflow. THE INDIRECT HALF: a card the GPU's accumulation listed
    // (the light it gathers moved a code since its march: harvestIndirectReports), one inside a SLAB region (a commit's
    // entering and leaving slabs - it reads another cascade now), or every card on the sky's overflow, a change with no
    // box or a cascade injected whole. A card not stale is never relit.
    {
        const auto boxOf = [](const CardRec &c) {
            const float hd = c.halfDepth + captureMargin(c.halfDepth);
            const Ogre::Vector3 ext(std::fabs(c.u.x) * c.halfU + std::fabs(c.v.x) * c.halfV + std::fabs(c.d.x) * hd,
                                    std::fabs(c.u.y) * c.halfU + std::fabs(c.v.y) * c.halfV + std::fabs(c.d.y) * hd,
                                    std::fabs(c.u.z) * c.halfU + std::fabs(c.v.z) * c.halfV + std::fabs(c.d.z) * hd);
            return Ogre::Aabb(c.centre, ext);
        };
        bool direct = view.allDirect, indirect = view.allIndirect;
        unsigned long long regionRoot = 0ull;   // (R3 (c): a region's or an overflow's work is its own root's)
        for (CardRec &c : mCards) {
            bool d = view.allDirect, n = view.allIndirect;
            if (!d || !n) {
                const Ogre::Aabb box = boxOf(c);
                for (size_t k = 0; !d && k < view.directBoxes.size(); ++k) d = box.intersects(view.directBoxes[k]);
                for (size_t k = 0; !n && k < view.indirectBoxes.size(); ++k) n = box.intersects(view.indirectBoxes[k]);
            }
            if ((d || n) && !regionRoot) regionRoot = 1ull << rootBit('r', mFrame);
            if (d) { c.relight = true; direct = true; c.roots |= regionRoot; c.sceneCause = true; }
            if (n) { c.relightIndirect = true; indirect = true; ++mIndirectByRegion; c.roots |= regionRoot; }
        }
        if (direct && !mRadianceMovingLastFrame) ++mInvalidRadiance;   // one per gesture
        mRadianceMovingLastFrame = direct;
        harvestIndirectReports(view.frame);   // the GPU's listed cards (a fixed lag behind their frame)
        for (const CardRec &c : mCards) indirect = indirect || c.relightIndirect;
        if (indirect && !mIndirectMovingLastFrame) ++mInvalidIndirect;
        mIndirectMovingLastFrame = indirect;
    }
    if (!mRadiance) return;
    // The cards captured THIS frame: both halves are stale. Every capture
    // changes the SURFACE — a new rect or a material (the kD, the normal, the
    // roughness the march and its lobe read); no light re-queues a capture
    // (ATOM-S3-CARDCAP) — so its cached indirect is re-marched.
    for (unsigned idx : mBatch) {
        CardRec &c = mCards[idx];
        c.relight = true;
        c.relightIndirect = true;
        if (c.indirectValid) mTableDirty = true;   // the ray read falls back to the voxels
        c.indirectValid = false;
    }
    const auto ready = [this](unsigned i) {
        const CardRec &c = mCards[i];
        const bool thisFrame = std::find(mBatch.begin(), mBatch.end(), i) != mBatch.end();
        return thisFrame || (!c.queued && c.lastUpdated);
    };
    // THE ORDER OF THE STALE SET (PHOTON-7 stage 5, 5-1): oldest first, then the nearest to the viewer, then the index -
    // lexicographic (a product of frames and metres has no scale: PREFLIGHT_5 W10). It orders; it never decides.
    const auto oldestFirst = [this](std::vector<unsigned> &v, bool indirect) {
        std::sort(v.begin(), v.end(), [this, indirect](unsigned a, unsigned b) {
            const unsigned long long ka = indirect ? mCards[a].lastIndirect : mCards[a].lastRelit;
            const unsigned long long kb = indirect ? mCards[b].lastIndirect : mCards[b].lastRelit;
            if (ka != kb) return ka < kb;
            const float da = (mCards[a].centre - mViewerPos).squaredLength();
            const float db = (mCards[b].centre - mViewerPos).squaredLength();
            if (da != db) return da < db;
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
        const size_t batchCount = want.size();
        want.insert(want.end(), rest.begin(), rest.end());
        unsigned spent = 0u;
        for (size_t w = 0; w < want.size(); ++w) {
            const unsigned idx = want[w];
            const unsigned cost = mCards[idx].size * mCards[idx].size;
            // THIS FRAME'S CAPTURES ARE NEVER CUT BY THE BUDGET (PHOTON-CARDS-5 audit
            // F3): the capture's copy rewrote the texels whole — the Albedo and
            // Normal alphas included, which hold the mean light direction the ray
            // read restores the view term with (JahCardView) — while the card's
            // `indirectValid` (the read's lit flag) stands. A captured card that
            // waited for a later frame's budget would be read with the capture's
            // alpha (1, 1), the world -Z direction, and its stale radiance: the
            // physics is that a capture and its relight are ONE update of the
            // card, so the relight rides the capture's frame (its cost was
            // budgeted with the capture: at most kCaptureBatch cards).
            const bool captured = w < batchCount;
            if (!captured && spent && spent + cost > mLightBudget) break;
            if (mRelight.size() >= kMaxRelights) break;
            mRelight.push_back(idx);
            mRelightMode.push_back(mCards[idx].indirectValid ? 0u : 2u);
            spent += cost;
        }
    }
    // 3. THE CHANGED-REGION LIST'S BOXES (PHOTON-7 stage 3): each instance this plan relights a card of, ONCE - its
    // world box (the transform signature's: where its cards were cut).
    // (8B (A): an instance is listed CardScene when ANY card of it this plan relights carries a scene change's first
    // round - CardRec::sceneCause, cleared here: its later rounds are Card)
    const size_t pendingFrom = mPendingRelitBoxes.size(), relitFrom = mRelitInstances.size();
    for (unsigned idx : mRelight) {
        const unsigned inst = mCards[idx].instance;
        const bool scene = mCards[idx].sceneCause;
        mCards[idx].sceneCause = false;
        if (inst >= mInstances.size()) continue;
        const auto at = std::find(mRelitInstances.begin(), mRelitInstances.end(), inst);
        if (at != mRelitInstances.end()) {
            const size_t k = size_t(at - mRelitInstances.begin());
            if (scene && k >= relitFrom) mPendingRelitBoxes[pendingFrom + (k - relitFrom)].scene = true;
            continue;
        }
        mRelitInstances.push_back(inst);
        // (listed when its report reads the move VISIBLE - a texel moved a code - never for a sub-quantum relight:
        // PHOTON-7 stage 8B B1, R3 (a)'s display quantum at the gather's cells; mPendingRelitBoxes)
        mPendingRelitBoxes.push_back(RelitBox{ Ogre::Aabb(mInstances[inst].centre, mInstances[inst].halfSize), scene });
    }
}

void SurfaceCache::relightCards() {
    // THE FRAME'S ROOTS (R3 (c)): what its relights - the plan's and the sun traces' - trace to, and their deepest round:
    // the moves they report carry them into the accumulation's listings (harvestIndirectReports), and the last frame's
    // are published (CardCacheStatus::workRoots). A relight no root names (none was given it) is named 'r' here: work
    // the judge reads as no owed move's.
    mPlanRoots = 0ull;
    mPlanRound = 0u;
    for (size_t ri = 0; ri < mRelight.size(); ++ri) {
        const unsigned idx = mRelight[ri];
        CardRec &c = mCards[idx];
        if (!c.roots) c.roots = 1ull << rootBit('r', mFrame);
        mPlanRoots |= c.roots;
        mPlanRound = std::max(mPlanRound, c.round);
    }
    mFrameRoots[mFrame % 512u] = FrameRoots{ mFrame, mPlanRoots, mPlanRound };
    if (mRelight.empty() || !mRadiance) { mVct = nullptr; return; }
    const auto t0 = std::chrono::steady_clock::now();
    Ogre::Root &root = Ogre::Root::getSingleton();
    Ogre::RenderSystem *rs = root.getRenderSystem();
    Ogre::HlmsCompute *hc = root.getHlmsManager()->getComputeHlms();
    if (!mLightJob) mLightJob = hc ? hc->findComputeJobNoThrow("Jahshaka/CardLight") : nullptr;
    if (!mLightJob) { mRelight.clear(); mVct = nullptr; return; }
    // (the moves the relight writes - 10-2c - and the stale set's buffers)
    if (!ensureIndirectBuffers()) { mRelight.clear(); mVct = nullptr; return; }
    Ogre::VaoManager *vao = rs->getVaoManager();

    // ---- THE RELIGHT LIST: each card's capture frame, as the job unprojects it.
    mRelightCpu.assign(size_t(kMaxRelights) * kRelightFloats, 0.0f);
    for (size_t i = 0; i < mRelight.size(); ++i) {
        const CardRec &c = mCards[mRelight[i]];
        float *r = &mRelightCpu[i * kRelightFloats];
        const Ogre::Vector3 cam = c.centre + c.d * (c.halfDepth + captureMargin(c.halfDepth));
        r[0] = float(c.atlasX); r[1] = float(c.atlasY); r[2] = float(c.size);
        // (mode bit 8: captured this frame - the card-to-card re-listing's arrival, 10-2c)
        const bool captured = std::find(mFreshCards.begin(), mFreshCards.end(), mRelight[i]) != mFreshCards.end();
        // (mode bit 16: the open layer holds this capture's - PHOTON-7 11-2 (5); else the relight reads the texel open)
        r[3] = float(mRelightMode[i] | (captured ? 8u : 0u) | (c.openValid ? 16u : 0u));
        r[4] = cam.x; r[5] = cam.y; r[6] = cam.z;
        // THE MOVERS' TERM IS MULTIPLIED IN where the card carries a trace.
        r[7] = c.moverTraced ? 1.0f : 0.0f;
        // The ortho window aimCamera gives the capture, and the same clamp.
        r[8] = c.u.x; r[9] = c.u.y; r[10] = c.u.z; r[11] = std::max(2.0f * c.halfU, 1e-4f);
        r[12] = c.v.x; r[13] = c.v.y; r[14] = c.v.z; r[15] = std::max(2.0f * c.halfV, 1e-4f);
        r[16] = c.d.x; r[17] = c.d.y; r[18] = c.d.z; r[19] = 0.0f;
    }

    // ---- THE LIGHTS, in world space, in the pass buffer's layout, from the
    // SCENE'S ONE LIGHT LIST (P1C-LIGHT-LIST, SceneLightList.h) — never from the
    // frame's global list: that list is culled against the frame's visible
    // cameras, and a card lights a surface OFF screen (the reader is a ray hit),
    // so a lamp no camera sees this frame must still light it. The list is
    // range-culled against the box of THIS FRAME'S RELIT CARDS (each card's capture
    // volume; a light whose range misses every one of them lights no texel the job
    // writes — the shader's own per-texel range test says the same) and, past the
    // job's 64, the first 64 of the list's one order (peak intensity x range^2: SceneLightList),
    // the rest COUNTED (lightsDropped).
    // P2d makes it per card. ONE source, the sun included: the one light whose
    // visibility is the captured shadow term is the light the capture's shadow
    // node gave its PSSM maps, which is the frame's rule applied to the same list
    // — the first visible shadow-casting directional light in creation order
    // (SceneManager::quickSortDirectionalLights: casters first, then by id).
    // AREA lights ARE summed (PHOTON-7 stage 5, 5-3): the pixel's own LTC form factor, its rectangle in the record.
    const Ogre::Light *sun = cardSun();
    SceneLightList::Consumer consumer;
    consumer.area = true;
    consumer.budget = kMaxCardLights;
    {
        Ogre::Vector3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (unsigned idx : mRelight) {
            const CardRec &c = mCards[idx];
            const float hd = c.halfDepth + captureMargin(c.halfDepth);
            const Ogre::Vector3 ext(
                std::fabs(c.u.x) * c.halfU + std::fabs(c.v.x) * c.halfV + std::fabs(c.d.x) * hd,
                std::fabs(c.u.y) * c.halfU + std::fabs(c.v.y) * c.halfV + std::fabs(c.d.y) * hd,
                std::fabs(c.u.z) * c.halfU + std::fabs(c.v.z) * c.halfV + std::fabs(c.d.z) * hd);
            lo.makeFloor(c.centre - ext);
            hi.makeCeil(c.centre + ext);
        }
        consumer.boxMin = lo;
        consumer.boxMax = hi;
    }
    SceneLightList::Selection chosen;
    sceneLightListOf(mSceneMgr).select(mSceneMgr, consumer, chosen);
    mLightCpu.assign(4u + size_t(kMaxCardLights) * kLightFloats, 0.0f);
    unsigned numLights = 0u;
    const unsigned dropped = chosen.overBudget;
    for (const Ogre::Light *l : chosen.lights) {
        const Ogre::Light::LightTypes type = l->getType();
        float *o = &mLightCpu[4u + size_t(numLights) * kLightFloats];
        const Ogre::ColourValue c = l->getDiffuseColour() * l->getPowerScale();
        const bool area = type == Ogre::Light::LT_AREA_LTC;   // every area light is LTC (PHOTON-7 7-0)
        if (type == Ogre::Light::LT_DIRECTIONAL) {
            const Ogre::Vector3 toLight = -l->getDerivedDirection();
            o[0] = toLight.x; o[1] = toLight.y; o[2] = toLight.z; o[3] = 0.0f;
        } else {
            const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
            o[0] = p.x; o[1] = p.y; o[2] = p.z;
            o[3] = area ? 3.0f : type == Ogre::Light::LT_POINT ? 1.0f : 2.0f;
            if (area) {
                // THE RECTANGLE (lightextras::areaLightCorners: one text with the pixel's area visibility)
                Ogre::Vector3 corner[4];
                detail::lightextras::areaLightCorners(l, corner);
                for (unsigned k = 0; k < 4u; ++k) {
                    o[20u + 4u * k] = corner[k].x;
                    o[21u + 4u * k] = corner[k].y;
                    o[22u + 4u * k] = corner[k].z;
                }
                o[23] = l->getDoubleSided() ? 1.0f : 0.0f;
            }
        }
        o[4] = c.r; o[5] = c.g; o[6] = c.b; o[7] = (l == sun) ? 1.0f : 0.0f;
        const float range = l->getAttenuationRange();
        o[8] = range; o[9] = l->getAttenuationLinear(); o[10] = l->getAttenuationQuadric();
        o[11] = range > 0.0f ? 1.0f / range : 0.0f;   // fork 36162ff37+16d8e29d4 (was 0018)'s fade, as light0Buf writes it
        const Ogre::Vector3 sd = l->getDerivedDirection();
        o[12] = sd.x; o[13] = sd.y; o[14] = sd.z; o[15] = 0.0f;
        const float inner = l->getSpotlightInnerAngle().valueRadians();
        const float outer = l->getSpotlightOuterAngle().valueRadians();
        const float denom = std::cos(inner * 0.5f) - std::cos(outer * 0.5f);
        o[16] = std::fabs(denom) > 1e-6f ? 1.0f / denom : 1e6f;
        o[17] = std::cos(outer * 0.5f);
        o[18] = l->getSpotlightFalloff();
        const unsigned id = lampIdOf(l);   // THE LAMP'S WORLD ID (the cards' lamp tables name it), as bits
        std::memcpy(&o[19], &id, sizeof(id));
        ++numLights;
    }
    std::vector<size_t> pointsHere;     // per relit card: its point and spot lamps (the trace's rays, counted below)
    std::vector<unsigned> areasHere;    // ...and its area lights
    // ---- EACH RELIT CARD'S LAMP TABLE (PHOTON-7 stage 5, 5-3): the list's POINT AND SPOT lamps that reach the card's
    // box - the list's own test, in its one world order - the first 16, or when more reach it the 16 with the largest
    // share of its direct (the lamp's peak channel through its falloff and cosine at the card's centre), kept in the
    // world order. The bits the trace writes and the relight and the readers read index it. The cards' GPU records
    // carry it, so a table that changed re-syncs them before the readers run this frame. AN AREA LIGHT IS NEVER IN
    // THE TABLE (the 5-3 fix): its visibility is fractional, sampled over its rectangle, and it takes the fractional
    // byte with the lamps past the table (counted: the byte is exact for one of them a card).
    {
        bool tableChanged = false;
        std::vector<std::pair<float, unsigned>> reach;   // (share, the light's index in the list)
        pointsHere.assign(mRelight.size(), 0u);
        areasHere.assign(mRelight.size(), 0u);
        for (size_t i = 0; i < mRelight.size(); ++i) {
            CardRec &c = mCards[mRelight[i]];
            const float hd = c.halfDepth + captureMargin(c.halfDepth);
            const Ogre::Vector3 ext(std::fabs(c.u.x) * c.halfU + std::fabs(c.v.x) * c.halfV + std::fabs(c.d.x) * hd,
                                    std::fabs(c.u.y) * c.halfU + std::fabs(c.v.y) * c.halfV + std::fabs(c.d.y) * hd,
                                    std::fabs(c.u.z) * c.halfU + std::fabs(c.v.z) * c.halfV + std::fabs(c.d.z) * hd);
            SceneLightList::Consumer box;
            box.boxMin = c.centre - ext;
            box.boxMax = c.centre + ext;
            reach.clear();
            double total = 0.0;
            unsigned areaHere = 0u;
            for (unsigned li = 0; li < numLights; ++li) {
                const Ogre::Light *l = chosen.lights[li];
                if (l->getType() == Ogre::Light::LT_DIRECTIONAL || !SceneLightList::reaches(l, box)) continue;
                if (l->getType() == Ogre::Light::LT_AREA_LTC) {
                    ++areaHere;
                    continue;
                }
                const float *o = &mLightCpu[4u + size_t(li) * kLightFloats];
                const Ogre::Vector3 toL = Ogre::Vector3(o[0], o[1], o[2]) - c.centre;
                const float d = std::max(toL.length(), 1e-3f);
                const float peak = std::max(o[4], std::max(o[5], o[6]));
                const float share = peak * std::max(0.0f, c.d.dotProduct(toL / d)) / std::max(d * d, 1e-6f);
                reach.emplace_back(share, li);
                total += double(share);
            }
            mCardLampsMax = std::max(mCardLampsMax, unsigned(reach.size()));
            mCardAreaLampsMax = std::max(mCardAreaLampsMax, areaHere);
            c.areaLampCount = areaHere;
            c.fractionalCount = areaHere + (reach.size() > kCardLampTable ? unsigned(reach.size()) - kCardLampTable : 0u);
            pointsHere[i] = reach.size();   // (in the table or past it: one ray each)
            areasHere[i] = areaHere;
            if (reach.size() > kCardLampTable) {
                std::vector<std::pair<float, unsigned>> byShare = reach;
                std::stable_sort(byShare.begin(), byShare.end(),
                                 [](const std::pair<float, unsigned> &a, const std::pair<float, unsigned> &b) {
                                     return a.first > b.first;
                                 });
                double over = 0.0;
                for (size_t k = kCardLampTable; k < byShare.size(); ++k) over += double(byShare[k].first);
                byShare.resize(kCardLampTable);
                std::sort(byShare.begin(), byShare.end(),
                          [](const std::pair<float, unsigned> &a, const std::pair<float, unsigned> &b) {
                              return a.second < b.second;   // back into the world order
                          });
                reach.swap(byShare);
                if (total > 0.0) mLampOverflowShareMax = std::max(mLampOverflowShareMax, float(over / total));
            }
            unsigned ids[kCardLampTable] = {};
            for (size_t k = 0; k < reach.size(); ++k) {
                std::memcpy(&ids[k], &mLightCpu[4u + size_t(reach[k].second) * kLightFloats + 19u], sizeof(unsigned));
            }
            const unsigned n = unsigned(reach.size());
            if (n != c.lampCount || std::memcmp(ids, c.lampIds, sizeof(ids)) != 0) {
                c.lampCount = n;
                std::memcpy(c.lampIds, ids, sizeof(ids));
                tableChanged = true;
            }
            std::memcpy(&mRelightCpu[i * kRelightFloats + 20u], ids, sizeof(ids));
        }
        if (tableChanged) {
            mTableDirty = true;
            syncBuffers();
        }
    }
    // A LIGHT THE JOB CANNOT HOLD is counted every frame and SAID once per cache.
    mLightsInRange = chosen.inRange;
    mLightsDropped = dropped;
    if (dropped && !mLightsDroppedLogged) {
        mLightsDroppedLogged = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka cards: " + std::to_string(dropped) + " of " + std::to_string(chosen.inRange) +
            " light(s) in range of the relit cards are beyond the card relight's " +
            std::to_string(kMaxCardLights) + " (the dimmest there) and were not summed into the card radiance");
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

    // ---- THE LAMPS' VISIBILITY (PHOTON-7 stage 5, 5-3): every relit card's texels traced toward the list's point and
    // spot lights (rq_card_movers.comp's lamps mode, from the lifted point the still sun trace uses), into the SunVis
    // layer's green, before the relight multiplies its lamps by it. A card the trace could not reach this frame (the
    // scene's structure not built yet, a refused dispatch) is lit unshadowed by its
    // lamps, flagged in its record's float 19 - never by a visibility another frame wrote - and its direct half STAYS
    // STALE (relit again the next frame, until a trace lands: measured, gi.leak_room's carded far wall kept the outside
    // lamp's unshadowed red of its first relight - before the ray rule chose its levels - for as long as the lamp held
    // still) - for as long as the ray tier can trace at all (PHOTON-7 stage 5's read W4: keyed on the tier's presence,
    // CardMoverHooks::present).
    bool lampsUntraced = false;
    {
        unsigned lamps = 0u;
        for (unsigned i = 0; i < numLights; ++i) lamps += mLightCpu[4u + size_t(i) * kLightFloats + 3u] > 0.5f ? 1u : 0u;
        bool traced = false;
        // (the lift - sunLiftOf, the still trace's - is WORLD-FIXED: the card's level bound, its texel and its depth
        // quantum, no viewer term (L1); nothing to wait for but the structure)
        if (lamps && mMoverHooks.trace) {
            mLampCpu.assign(mRelight.size() * kRelightFloats, 0.0f);
            for (size_t i = 0; i < mRelight.size(); ++i) {
                std::memcpy(&mLampCpu[i * kRelightFloats], &mRelightCpu[i * kRelightFloats], kRelightFloats * sizeof(float));
                mLampCpu[i * kRelightFloats + 3u] = sunLiftOf(mCards[mRelight[i]]);   // the lift, as the still mode's
                mLampCpu[i * kRelightFloats + 7u] = mCards[mRelight[i]].openValid ? 1.0f : 0.0f;   // (the open layer's, 11-2 (5))
            }
            CardMoverTrace job;
            job.records = mLampCpu.data();
            job.count = unsigned(mRelight.size());
            job.depth = mAtlas[unsigned(CardLayer::Depth)];
            job.normal = mAtlas[unsigned(CardLayer::Normal)];
            job.vis = mMoverVis;
            job.shadowRough = mAtlas[unsigned(CardLayer::ShadowRough)];
            job.lights = mLightBuffer;
            job.sunVis = mSunVis;
            job.open = mOpen;
            job.lamps = true;
            job.areaStrata = mAreaStrata;
            traced = mMoverHooks.trace(job);
            if (traced) {
                mLampRaysLastTrace = 0ull;
                for (size_t i = 0; i < mRelight.size() && i < pointsHere.size(); ++i) {
                    const unsigned long long s = mCards[mRelight[i]].size;
                    mLampRaysLastTrace += s * s * (pointsHere[i] + (unsigned long long)areasHere[i] * mAreaStrata * mAreaStrata);
                }
            }
        }
        // (a wait while the ray tier can trace - its structure not there yet; a tier that
        // cannot trace at all - closed, its pipeline refused and logged - lights the lamps unshadowed for good, never a
        // relight a frame)
        lampsUntraced = lamps && !traced && mMoverHooks.present && mMoverHooks.present();
        if (traced) {
            for (size_t i = 0; i < mRelight.size(); ++i) mRelightCpu[i * kRelightFloats + 19u] = 1.0f;
            mRelightBuffer->upload(mRelightCpu.data(), 0, mRelight.size() * kRelightFloats / 4u);
            ++mLampTraces;
            mLampTracedLastFrame = unsigned(mRelight.size());
        }
    }

    // ---- THE CHAIN AND THE ENVIRONMENT, from the one PhotonVoxelLighting every reader
    // takes them from (the pixel's pass buffer is filled from the same calls).
    Ogre::PhotonVoxelLighting *vct = mVct;
    unsigned numCascades = 0u;
    Ogre::TextureGpu *envCube = nullptr;
    mGiCpu.assign(kGiParamFloats, 0.0f);
    if (vct && vct->getLightVoxelTextures() && vct->getLightVoxelTextures()[0]) {
        const size_t chainLen = vct->getNumCascades();
        numCascades = unsigned(std::min<size_t>(chainLen, kMaxCardCascades));
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
        // THE RAYS (PHOTON-7 stage 10, 10-1): the last cascade's world shell in cascade 0's normalised units (a cube),
        // and the rays a texel
        g[97] = mRayLastShell / size.x;
        g[98] = float(std::max(mIndirectRays, 1u));
        envCube = vct->getEnvironmentCube();
        g[100] = g[101] = g[102] = 0.0f;   // the pad: the cube holds the sky's gain (R5)
        g[103] = envCube ? float(envCube->getNumMipmaps()) : 1.0f;
        const float *sh = vct->getEnvironmentSh();
        for (unsigned k = 0; k < 9u; ++k) {
            g[104u + 4u * k] = sh[3u * k];
            g[105u + 4u * k] = sh[3u * k + 1u];
            g[106u + 4u * k] = sh[3u * k + 2u];
        }
    } else {
        // GI OFF (PHOTON-CARDS-5): no chain hands the environment over, and the
        // pixel's ambient is the scene's SH alone — the same coefficients, from
        // the scene (the job's GI-off branch reads them at the texel's normal).
        float *g = mGiCpu.data();
        for (unsigned k = 0; k < 9u; ++k) {
            g[104u + 4u * k] = mAmbientSh[3u * k];
            g[105u + 4u * k] = mAmbientSh[3u * k + 1u];
            g[106u + 4u * k] = mAmbientSh[3u * k + 2u];
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

    // ---- THE INDIRECT HALF BY TLAS RAYS (PHOTON-7 stage 10, 10-2a; rq_card_gather.comp): at the ray tier the cards
    // this relight re-gathers (mode 1) are traced through the scene's TLAS - their own pass, BEFORE the relight in the
    // same frame (no lag; nothing of a view's held state gates it, L2) - into the Gather layer, which the relight reads
    // in place of its voxel rays (mode bit 4). A scene that does not trace gathers by the voxel rays (the no-ray
    // route); a scene that traces but whose structure is not there this frame WAITS: the cards' indirect stays owed and
    // their direct half is relit alone (never the voxel rays at the ray tier: one estimator a tier, L5). GI off (no
    // chain) keeps the environment's SH.
    if (numCascades && mMoverHooks.gather) {
        std::vector<size_t> marched;
        for (size_t i = 0; i < mRelight.size(); ++i)
            if (mRelightMode[i] == 1u) marched.push_back(i);
        if (!marched.empty() && ensureGatherLayer()) {
            mGatherCpu.assign(marched.size() * kRelightFloats, 0.0f);
            for (size_t k = 0; k < marched.size(); ++k) {
                std::memcpy(&mGatherCpu[k * kRelightFloats], &mRelightCpu[marched[k] * kRelightFloats],
                            kRelightFloats * sizeof(float));
                mGatherCpu[k * kRelightFloats + 3u] = sunLiftOf(mCards[mRelight[marched[k]]]);   // the still trace's lift
                mGatherCpu[k * kRelightFloats + 7u] = mCards[mRelight[marched[k]]].openValid ? 1.0f : 0.0f;   // (11-2 (5))
            }
            CardGatherTrace job;
            job.records = mGatherCpu.data();
            job.count = unsigned(marched.size());
            job.rays = std::max(mIndirectRays, 1u);
            job.reach = mRayLastShell;
            job.gather = mGather;
            job.reads = mCardReads;
            job.open = mOpen;
            const CardGatherResult res = mMoverHooks.gather(job);
            if (res == CardGatherResult::Traced) {
                for (size_t i : marched)
                    mRelightCpu[i * kRelightFloats + 3u] = float(unsigned(mRelightCpu[i * kRelightFloats + 3u]) | 4u);
                mGatherByRays = true;
            } else if (res == CardGatherResult::Wait) {
                // the direct half alone (its cached indirect, or none yet); the card stays owed (relightIndirect)
                for (size_t i : marched) {
                    mRelightMode[i] = mCards[mRelight[i]].indirectValid ? 0u : 2u;
                    mRelightCpu[i * kRelightFloats + 3u] =
                        float(mRelightMode[i] | (unsigned(mRelightCpu[i * kRelightFloats + 3u]) & 8u));
                }
                ++mGatherWaits;
            } else {
                mGatherByRays = false;
            }
            if (res != CardGatherResult::NoRays)
                mRelightBuffer->upload(mRelightCpu.data(), 0, mRelight.size() * kRelightFloats / 4u);
        }
    }

    // ---- THE DISPATCH: the job is shared process-wide by name, so every
    // binding is set here and released after the dispatch is recorded.
    if (mLightJob->getProperty("hlms_num_vct_cascades") != Ogre::int32(numCascades))
        mLightJob->setProperty("hlms_num_vct_cascades", Ogre::int32(numCascades));
    if (mLightJob->getProperty("jah_env") != (envCube ? 1 : 0))
        mLightJob->setProperty("jah_env", envCube ? 1 : 0);
    // THE GATHER LAYER (10-2a): bound wherever the ray tier made it (its UAV 10, read by the mode bit 4)
    const bool gatherBound = mGather != nullptr;
    if (mLightJob->getProperty("jah_card_gather") != (gatherBound ? 1 : 0))
        mLightJob->setProperty("jah_card_gather", gatherBound ? 1 : 0);
    mLightJob->setNumUavUnits(gatherBound ? 13u : 12u);   // (11: the open fraction, 12: the gather layer)
    // Every light volume a reader binds per cascade: the total, the directional
    // axes, the per-axis coverage (PHOTON-VOXEL-3), the surface position
    // (PHOTON-VOXEL-4), the back side and the normal - PhotonVoxelLighting's list.
    const unsigned kinds = numCascades ? unsigned(vct->getNumVoxelTextures()) : 0u;
    const unsigned vctUnits = numCascades ? kinds * numCascades + (envCube ? 1u : 0u) : 0u;
    // THE CLOUD FIELD, the LAST texture unit (after the chain and the
    // environment), with the field's own wrapped sampler.
    const Ogre::HlmsSamplerblock *cloudWrap = mCloudField
        ? detail::FogHlmsListener::acquireWrapSampler(root.getHlmsManager()) : nullptr;
    const bool cloud = mCloudField && cloudWrap;
    if (mLightJob->getProperty("jah_cloud_shadow") != (cloud ? 1 : 0))
        mLightJob->setProperty("jah_cloud_shadow", cloud ? 1 : 0);
    // The four layers the job SAMPLES (the Albedo and Normal are its UAVs 6, 7): the
    // specular lobe's last (ENGINE-POLISH-PHOTON-1 stage 3).
    constexpr unsigned kRelightTexLayers = 4u;
    const unsigned order[kRelightTexLayers] = { unsigned(CardLayer::Depth), unsigned(CardLayer::Emissive),
                                                unsigned(CardLayer::ShadowRough), unsigned(CardLayer::Specular) };
    mLightJob->setNumTexUnits(Ogre::uint8(kRelightTexLayers + vctUnits + (cloud ? 1u : 0u)));
    if (cloud) {
        Ogre::DescriptorSetTexture2::TextureSlot slot(
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = mCloudField;
        mLightJob->setTexture(Ogre::uint8(kRelightTexLayers + vctUnits), slot, cloudWrap);
    }
    for (unsigned i = 0; i < kRelightTexLayers; ++i) {
        Ogre::DescriptorSetTexture2::TextureSlot slot(
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
        slot.texture = mAtlas[order[i]];
        mLightJob->setTexture(Ogre::uint8(i), slot, nullptr, false);
    }
    if (numCascades) {
        // The chain's volumes, per kind then per cascade (the pixel's and the
        // parity harness's order), the trilinear sampler on the first — the
        // shader's `vSmp` — and the environment cube last.
        Ogre::uint8 unit = Ogre::uint8(kRelightTexLayers);
        for (unsigned kind = 0; kind < kinds; ++kind)
            for (unsigned c = 0; c < numCascades; ++c, ++unit) {
                Ogre::DescriptorSetTexture2::TextureSlot slot(
                    Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
                slot.texture = vct->getLightVoxelTextures(c)[kind];
                if (unit == kRelightTexLayers)
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
    // THE CARDS' MOVES (10-2c): every relit card's largest radiance change this frame, written
    mLightJob->_setUavBuffer(3u, bufSlot(mCardMoves, Ogre::ResourceAccess::ReadWrite));
    {
        Ogre::DescriptorSetUav::TextureSlot uav(Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        // (read too: the move against what the card held, 10-2c)
        uav.texture = mRadiance;
        uav.access = Ogre::ResourceAccess::ReadWrite;
        uav.pixelFormat = mRadiance->getPixelFormat();
        mLightJob->_setUavTexture(4u, uav);
        uav.texture = mIndirect;
        uav.access = Ogre::ResourceAccess::ReadWrite;
        uav.pixelFormat = mIndirect->getPixelFormat();
        mLightJob->_setUavTexture(5u, uav);
        uav.texture = mMoverVis;
        uav.access = Ogre::ResourceAccess::Read;
        uav.pixelFormat = mMoverVis->getPixelFormat();
        mLightJob->_setUavTexture(6u, uav);
        // The Albedo and Normal layers: read, and their alpha written (JahCardView).
        uav.texture = mAtlas[unsigned(CardLayer::Albedo)];
        uav.access = Ogre::ResourceAccess::ReadWrite;
        uav.pixelFormat = uav.texture->getPixelFormat();
        mLightJob->_setUavTexture(7u, uav);
        uav.texture = mAtlas[unsigned(CardLayer::Normal)];
        uav.pixelFormat = uav.texture->getPixelFormat();
        mLightJob->_setUavTexture(8u, uav);
        // THE FLAT TERM (ENGINE-POLISH-PHOTON-1 stage 3): the emissive and the specular lobe's
        // re-emission, written.
        uav.texture = mFlat;
        uav.access = Ogre::ResourceAccess::Write;
        uav.pixelFormat = mFlat->getPixelFormat();
        mLightJob->_setUavTexture(9u, uav);
        // THE SUN'S VISIBILITY AT THE TEXEL (PHOTON-3 stage 2), written - and the lamps' (its green, PHOTON-7 stage
        // 5, 5-3), read and kept.
        uav.texture = mSunVis;
        uav.access = Ogre::ResourceAccess::ReadWrite;
        uav.pixelFormat = mSunVis->getPixelFormat();
        mLightJob->_setUavTexture(10u, uav);
        // THE GATHER LAYER (10-2a): the TLAS gather's irradiance, read - and its a, the texels' reported radiance
        // (10-2c), read and written
        // THE OPEN FRACTION (PHOTON-7 11-2 (5)): read - the texel lit at its open centroid, its light x f
        uav.texture = mOpen;
        uav.access = Ogre::ResourceAccess::Read;
        uav.pixelFormat = mOpen->getPixelFormat();
        mLightJob->_setUavTexture(11u, uav);
        if (gatherBound) {
            uav.texture = mGather;
            uav.access = Ogre::ResourceAccess::ReadWrite;
            uav.pixelFormat = mGather->getPixelFormat();
            mLightJob->_setUavTexture(12u, uav);
        }
    }
    mLightJob->_setUavBuffer(2u, bufSlot(mGiBuffer, Ogre::ResourceAccess::Read));
    // ...and the lobe's integrals: the engine's one table (PhotonSpecularAlbedoBuffer.h), the
    // job's one constant buffer.
    mLightJob->setConstBuffer(
        0u, Ogre::PhotonSpecularAlbedo::constBuffer(Ogre::Root::getSingleton().getRenderSystem()->getVaoManager()));
    mLightJob->setThreadsPerGroup(8u, 8u, 1u);
    mLightJob->setNumThreadGroups(kCardPageSize / 8u, kCardPageSize / 8u, unsigned(mRelight.size()));
    // THE RELIGHT NEVER COMPILES IN THE FRAME (ASYNC-SHADERS-1). A permutation this job has
    // not built (a new environment, the cloud field, another cascade count) is handed to the
    // engine's background compiler and the relight is SKIPPED: every card keeps the radiance
    // it has and stays owed (c.relight), so the next frame that finds the shader built
    // relights exactly these. With no background service the answer is "ready" and the
    // dispatch compiles as it always did.
    // THE RELIT CARDS' SENSITIVITY (PHOTON-7 stage 5, 5-1; JahCardIndirect_cs.glsl): one workgroup a card, its lit
    // texels' lowest radiance - the code its indirect's accumulation counts in - and a MARCHED card's accumulator
    // restarts (its indirect is fresh). After the relight (its radiance read), in the same frame. THE RELIGHT AND ITS
    // MEASUREMENT ARE ONE STEP: a card relit with no sensitivity measured would never be listed stale again, so the
    // relight waits for both jobs built (each bound BEFORE its background compile is asked for: requestAsync builds the
    // descriptor sets from the bound slots, an empty one a null dereference).
    if (!mSensJob) mSensJob = hc->findComputeJobNoThrow("Jahshaka/CardSensitivity");
    const bool sensBound = mSensJob && ensureIndirectBuffers();
    const unsigned sensTex[2] = { 0u, 1u };
    if (sensBound) {
        Ogre::TextureGpu *tex[2] = { mRadiance, mAtlas[unsigned(CardLayer::Depth)] };
        for (unsigned i : sensTex) {
            Ogre::DescriptorSetTexture2::TextureSlot slot(Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty());
            slot.texture = tex[i];
            mSensJob->setTexture(Ogre::uint8(i), slot, nullptr, false);
        }
        Ogre::DescriptorSetUav::BufferSlot b0(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        b0.buffer = mRelightBuffer;
        b0.access = Ogre::ResourceAccess::Read;
        mSensJob->_setUavBuffer(0u, b0);
        Ogre::DescriptorSetUav::BufferSlot b1(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        b1.buffer = mCardSlotState;
        b1.access = Ogre::ResourceAccess::ReadWrite;
        mSensJob->_setUavBuffer(1u, b1);
        Ogre::ShaderParams &params = mSensJob->getShaderParams("default");
        if (Ogre::ShaderParams::Param *p = params.findParameter("caMode")) {
            const Ogre::uint32 mode[4] = { 0u, Ogre::uint32(mRelight.size()), 0u, 0u };
            p->setManualValue(mode, 4u);
        }
        params.setDirty();
        mSensJob->setNumThreadGroupsBasedOn(Ogre::HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u);
        mSensJob->setNumThreadGroups(unsigned(mRelight.size()), 1u, 1u);
    }
    const bool lightReady =
        !mAsyncShaders || hc->requestAsync(mLightJob) != Ogre::HlmsCompute::AsyncPending;
    const bool sensReady =
        sensBound && (!mAsyncShaders || hc->requestAsync(mSensJob) != Ogre::HlmsCompute::AsyncPending);
    const bool shaderReady = lightReady && sensReady;
    if (shaderReady) {
        // THE RELIGHT'S MONITOR ROW (lane TEST-1, the perf audit's F1): it runs in
        // this workspace's own end callback, after its passes closed, so no pass
        // row holds it — and before, its GPU time lived only in the ray tier's
        // private ring (absent at the no-ray tier). One row at every tier now.
        detail::monitor::CacheScope relight(CacheKind::Gi, WorkReason::Light, 0, "cards.relight", rs, this);
        relight.setUnits(unsigned(mRelight.size()));
        // ...AND ITS INDIRECT SHARE (PHOTON-2 stage 0): the cards whose indirect half this dispatch re-marches
        // (mode 1, the march through the cascades) — the per-card steps every commit or settle owes the whole
        // resident set (OgreSurfaceCache.cpp planRelights' indirect signature). Inside the relight's own time.
        unsigned indirect = 0u;
        for (unsigned m : mRelightMode) indirect += m == 1u ? 1u : 0u;
        if (indirect) detail::monitor::noteCacheWork(CacheKind::Gi, WorkReason::Sweep, 0, "cards.indirect", indirect);
        detail::photonDispatch(hc, mLightJob, rs);   // the one door: its barriers, the witness
        mRelitMeasured = gatherBound;   // (the frame's move words are written where the gather layer is bound)
    }
    if (sensBound) {
        if (shaderReady) detail::photonDispatch(hc, mSensJob, rs);
        const Ogre::DescriptorSetUav::BufferSlot empty = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
        mSensJob->_setUavBuffer(0u, empty);
        mSensJob->_setUavBuffer(1u, empty);
        for (unsigned i : sensTex)
            mSensJob->setTexture(Ogre::uint8(i), Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty(), nullptr, false);
    }
    // THE CARD-TO-CARD RE-LISTING (10-2c): the relight's moves into the cards that read the moved ones - after the
    // sensitivity, which restarts a marched card's accumulator (a card gathered this frame read the radiance of BEFORE
    // this relight: what moved in it is still its to gather) - then the moves cleared
    if (shaderReady) cardToCardMoves(true);
    {
        const Ogre::DescriptorSetUav::BufferSlot empty = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
        mLightJob->_setUavBuffer(0u, empty);
        mLightJob->_setUavBuffer(1u, empty);
        mLightJob->_setUavBuffer(2u, empty);
        mLightJob->_setUavBuffer(3u, empty);
        for (Ogre::uint8 k = 4u; k <= 11u; ++k)
            mLightJob->_setUavTexture(k, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        if (gatherBound) mLightJob->_setUavTexture(12u, Ogre::DescriptorSetUav::TextureSlot::makeEmpty());
        mLightJob->setConstBuffer(0u, nullptr);
        mLightJob->setNumTexUnits(0u);
    }

    for (size_t i = 0; shaderReady && i < mRelight.size(); ++i) {
        CardRec &c = mCards[mRelight[i]];
        // A CARD THE TRACE ADDED AFTER THE PLAN (PHOTON-7 stage 3): its instance is the next frame's region.
        if (c.instance < mInstances.size() &&
            std::find(mRelitInstances.begin(), mRelitInstances.end(), c.instance) == mRelitInstances.end()) {
            mRelitInstances.push_back(c.instance);
            mLateRelitBoxes.push_back(RelitBox{ Ogre::Aabb(mInstances[c.instance].centre, mInstances[c.instance].halfSize),
                                                c.sceneCause });
            c.sceneCause = false;
        }
        c.relight = lampsUntraced;   // its lamps unshadowed this once: relit again when the trace can run
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
        if (!c.relight && !c.relightIndirect) {   // its work done: its roots answered (R3 (c))
            c.roots = 0ull;
            c.round = 0u;
        }
    }
    mRelight.clear();
    mRelightMode.clear();
    mVct = nullptr;
    mLightMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// ---------------------------------------------------------------------------
// THE INDIRECT HALF'S STALE SET (PHOTON-7 stage 5, 5-1; JahCardIndirect_cs.glsl)
// ---------------------------------------------------------------------------
//
// A card's indirect half is a cone march through the voxel chain (CardLight's mode 1), so it is stale when the voxel
// light it gathers moved by a code since that march - ACCUMULATED per card in absolute radiance, never decided per frame
// on the brick mask's relative codes (a dim far brick would over-stale, a slow drift would never fire: PREFLIGHT_5 W6).
// Each cascade that moved this frame adds, on the GPU, its bricks' moves (a pass's, an injection's) weighted by a bound
// of their share of the card's gather into every resident card's would-move; the card's sensitivity (its darkest lit
// texel's code, set at its relight) turns radiance into codes; a card at a code is listed, the list read back at a fixed
// lag of host frames, and planRelights relights its indirect. The card's march resets its would-move.
namespace {
constexpr unsigned kCardSlots = (kCardAtlasSize / kCardMinSize) * (kCardAtlasSize / kCardMinSize);
// (the card-to-card shaders' constants: JahCardLight_cs.glsl's and JahCardIndirect_cs.glsl's 16384 slots, the frame's
// largest move at the last; rq_card_gather.comp's and JahCardIndirect_cs.glsl's 65-word blocks of 32 entries)
static_assert(kCardSlots == 16384u, "the card-to-card shaders index 16384 slots");
constexpr unsigned kReportCapacity = 4096u;
constexpr unsigned kMaxCardClusters = 4096u;   ///< 4^3 bricks a cluster (two records of three words): a 512^3 cascade's 16^3
}   // namespace

Ogre::UavBufferPacked *SurfaceCache::ensureLightBuffer() {
    if (mLightBuffer) return mLightBuffer;
    Ogre::VaoManager *vao = Ogre::Root::getSingleton().getRenderSystem()->getVaoManager();
    if (!vao) return nullptr;
    std::vector<float> zeros(4u + size_t(kMaxCardLights) * kLightFloats, 0.0f);   // a count of 0
    mLightBuffer = vao->createUavBuffer(1u + size_t(kMaxCardLights) * kLightFloats / 4u, 16u, 0, zeros.data(), false);
    return mLightBuffer;
}

void SurfaceCache::cardToCardMoves(bool shaderReadyRelight) {
    // THE FRAME'S RELIGHTS AND THE READERS' THRESHOLD: they count VISIBLE at once wherever their move words cannot come
    // back (the relight wrote none, the accumulation does not run, the report is not read) - never a silent "no move"
    const auto unmeasured = [this]() {
        ++mVisibleRelights;
        ++mUnmeasuredRelights;
        mRelitMeasured = false;
        listPendingRelits();   // (counted visible: listed at once)
    };
    if (shaderReadyRelight && !ensureIndirectBuffers()) { unmeasured(); return; }
    if (!shaderReadyRelight || !ensureIndirectBuffers()) return;
    Ogre::Root &root = Ogre::Root::getSingleton();
    Ogre::RenderSystem *rs = root.getRenderSystem();
    Ogre::HlmsCompute *hc = root.getHlmsManager()->getComputeHlms();
    if (!mAccumJob) mAccumJob = hc ? hc->findComputeJobNoThrow("Jahshaka/CardAccumulate") : nullptr;
    if (!mAccumJob) return;
    // AT THE RAY TIER a card reads other cards (its TLAS gather); elsewhere it reads the voxels alone and its readers'
    // lists do not exist - the moves are cleared all the same
    const bool cardsRead = mGather && mCardReads && mCardRecords;
    const auto bufSlot = [](Ogre::UavBufferPacked *b, Ogre::ResourceAccess::ResourceAccess a) {
        Ogre::DescriptorSetUav::BufferSlot slot(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        slot.buffer = b;
        slot.access = a;
        return slot;
    };
    mAccumJob->_setUavBuffer(0u, bufSlot(cardsRead ? mCardReads : mCardBoxBuffer, Ogre::ResourceAccess::Read));
    mAccumJob->_setUavBuffer(1u, bufSlot(mCardBoxBuffer, Ogre::ResourceAccess::Read));
    mAccumJob->_setUavBuffer(2u, bufSlot(mCardSlotState, Ogre::ResourceAccess::ReadWrite));
    mAccumJob->_setUavBuffer(3u, bufSlot(mCardReport, Ogre::ResourceAccess::ReadWrite));
    mAccumJob->_setUavBuffer(4u, bufSlot(mCardMoves, Ogre::ResourceAccess::ReadWrite));
    Ogre::ShaderParams &params = mAccumJob->getShaderParams("default");
    const auto setMode = [&params](const Ogre::uint32 mode[4]) {
        if (Ogre::ShaderParams::Param *p = params.findParameter("caMode")) p->setManualValue(mode, 4u);
        params.setDirty();
    };
    const bool ready = !mAsyncShaders || hc->requestAsync(mAccumJob) != Ogre::HlmsCompute::AsyncPending;
    if (ready) {
        mAccumJob->setNumThreadGroupsBasedOn(Ogre::HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u);
        // ITS OWN ROW (PHOTON-7 stage 10, lab.monitor's nesting law): the card-to-card accumulation and the moves' clear,
        // named together (the dispatch also files its job's row through the door). (The "+1.62 ms a frame" once read
        // here was a 60-frame MEAN carried by ONE frame's bubble - the creation frame's zero upload with a cold compile
        // inside it - not work: the bubble hook measured it, urgent #64.)
        detail::monitor::CacheScope row(CacheKind::Gi, WorkReason::Light, 0, "cards.card_to_card", rs);
        if (cardsRead) {
            // the frame's report starts empty once (the voxels' accumulation keeps the same rule)
            // (ONE CLOCK for both paths' clear, the cache's own frame: the voxels' giWriterFrame and the plan's frame
            // differ, and two clears a frame meant two reports a frame)
            if (mAccumClearFrame != mFrame) {
                flushReport();   // (the previous frame's report read before this frame's clear)
                mAccumClearFrame = mFrame;
                const Ogre::uint32 clear[4] = { 1u, 0u, kReportCapacity, 0u };
                setMode(clear);
                mAccumJob->setNumThreadGroups(1u, 1u, 1u);
                detail::photonDispatch(hc, mAccumJob, rs);
            }
            const Ogre::uint32 mode[4] = { 3u, mCardRecords, kReportCapacity, 0u };
            setMode(mode);
            // THE ARRIVALS: this frame's captures (at most the batch's eight) by their records, four in caDims and four
            // as caSelf's bits (0xFFFFFFFF: none)
            {
                Ogre::uint32 fresh[8];
                for (unsigned f = 0; f < 8u; ++f)
                    fresh[f] = f < mFreshCards.size() ? Ogre::uint32(mFreshCards[f]) : 0xFFFFFFFFu;
                float freshBits[4];
                std::memcpy(freshBits, &fresh[4], sizeof(freshBits));
                if (Ogre::ShaderParams::Param *p = params.findParameter("caDims")) p->setManualValue(fresh, 4u);
                if (Ogre::ShaderParams::Param *p = params.findParameter("caSelf")) p->setManualValue(freshBits, 4u);
                params.setDirty();
            }
            mAccumJob->setNumThreadGroups((mCardRecords + 63u) / 64u, 1u, 1u);
            detail::photonDispatch(hc, mAccumJob, rs);
            mAccumRan = true;
        }
        const Ogre::uint32 clearMoves[4] = { 4u, kCardSlots, kReportCapacity, 0u };   // (z: the report's frame words)
        setMode(clearMoves);
        mAccumJob->setNumThreadGroups((kCardSlots + 63u) / 64u, 1u, 1u);
        detail::photonDispatch(hc, mAccumJob, rs);
    } else if (cardsRead) {
        // A MOVE THAT COULD NOT BE MEASURED (the job building): every card's indirect is listed (the overflow rule; its
        // work no owed move's, 'r')
        const unsigned long long root = 1ull << rootBit('r', mFrame);
        for (CardRec &c : mCards)
            if (c.indirectValid) { c.relightIndirect = true; c.roots |= root; }
        ++mIndirectOverflows;
    }
    const Ogre::DescriptorSetUav::BufferSlot empty = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    for (Ogre::uint8 k = 0u; k < 5u; ++k) mAccumJob->_setUavBuffer(k, empty);
    // (mode 4 carried the relight's words into the report: its ticket brings them back, or they count visible)
    if (shaderReadyRelight) {
        if (mRelitMeasured && cardsRead && ready) mMovesOwed = true;
        else unmeasured();
        mRelitMeasured = false;
    }
    if (cardsRead && ready) endIndirectFrame(mPlanFrame);
    if (mMovesOwed && !mReportPending) { mMovesOwed = false; unmeasured(); }   // (no report will carry them)
    if (!mMovesOwed && !mPendingRelitBoxes.empty()) listPendingRelits();      // (no move owed to a report: at once)
}

bool SurfaceCache::ensureIndirectBuffers() {
    if (mCardBoxBuffer && mCardSlotState && mCardReport && mCardCluster && mCardMoves) return true;
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    if (!vao) return false;
    // EVERY ONE STARTS AT ZERO, WRITTEN BY THE GPU (photonFillBuffer: never host zeros through the staging buffer - the
    // creation frame's flush and idle, ensureGatherLayer's); the box buffer's delta keeps its host mirror of the zeros
    const auto zeroed = [&](size_t elements, size_t bytesPerElement) {
        Ogre::UavBufferPacked *b = vao->createUavBuffer(elements, Ogre::uint32(bytesPerElement), 0, nullptr, false);
        if (b) detail::photonFillBuffer(b, 0u);
        return b;
    };
    if (!mCardBoxBuffer) {
        const std::vector<float> zeros(size_t(kCardRecordCeiling) * 8u, 0.0f);
        mCardBoxBuffer = zeroed(size_t(kCardRecordCeiling) * 2u, 16u);
        mCardBoxDelta.reset(mCardBoxBuffer, zeros.data(), zeros.size() * sizeof(float));
    }
    if (!mCardSlotState) mCardSlotState = zeroed(size_t(kCardSlots) * 4u, sizeof(Ogre::uint32));
    // (the report: its count, the listings, and THE FRAME'S TWO MOVE WORDS past them - the largest reported move in
    // codes and a non-finite move seen, the readers' threshold; the moves: a slot each and the relight's same two words)
    if (!mCardReport) mCardReport = zeroed(3u + kReportCapacity, sizeof(Ogre::uint32));
    if (!mCardCluster) mCardCluster = zeroed(6u * kMaxCardClusters, sizeof(Ogre::uint32));
    if (!mCardMoves) mCardMoves = zeroed(size_t(kCardSlots) + 2u, sizeof(Ogre::uint32));
    return mCardBoxBuffer && mCardSlotState && mCardReport && mCardCluster && mCardMoves;
}

void SurfaceCache::accumulateIndirect(Ogre::UavBufferPacked *brickState, const unsigned dims[3],
                                      const Ogre::Vector3 &corner, float brickSide, const Ogre::Aabb *finer,
                                      unsigned long long frame) {
    if (!mBuilt || mFrozen || !brickState || !mCardRecords || !ensureIndirectBuffers()) return;
    Ogre::Root &root = Ogre::Root::getSingleton();
    Ogre::RenderSystem *rs = root.getRenderSystem();
    Ogre::HlmsCompute *hc = root.getHlmsManager()->getComputeHlms();
    if (!mAccumJob) mAccumJob = hc ? hc->findComputeJobNoThrow("Jahshaka/CardAccumulate") : nullptr;
    if (!mAccumJob) return;
    const auto bufSlot = [](Ogre::UavBufferPacked *b, Ogre::ResourceAccess::ResourceAccess a) {
        Ogre::DescriptorSetUav::BufferSlot slot(Ogre::DescriptorSetUav::BufferSlot::makeEmpty());
        slot.buffer = b;
        slot.offset = 0;
        slot.sizeBytes = 0;
        slot.access = a;
        return slot;
    };
    mAccumJob->_setUavBuffer(0u, bufSlot(brickState, Ogre::ResourceAccess::Read));
    mAccumJob->_setUavBuffer(1u, bufSlot(mCardBoxBuffer, Ogre::ResourceAccess::Read));
    mAccumJob->_setUavBuffer(2u, bufSlot(mCardSlotState, Ogre::ResourceAccess::ReadWrite));
    mAccumJob->_setUavBuffer(3u, bufSlot(mCardReport, Ogre::ResourceAccess::ReadWrite));
    mAccumJob->_setUavBuffer(4u, bufSlot(mCardCluster, Ogre::ResourceAccess::ReadWrite));
    Ogre::ShaderParams &params = mAccumJob->getShaderParams("default");
    const auto set = [&params](const char *name, const void *v, bool isFloat) {
        Ogre::ShaderParams::Param *p = params.findParameter(name);
        if (!p) return;
        if (isFloat) p->setManualValue(static_cast<const float *>(v), 4u);
        else p->setManualValue(static_cast<const Ogre::uint32 *>(v), 4u);
    };
    const bool shaderReady = !mAsyncShaders || hc->requestAsync(mAccumJob) != Ogre::HlmsCompute::AsyncPending;
    const unsigned clusters = ((dims[0] + 3u) / 4u) * ((dims[1] + 3u) / 4u) * ((dims[2] + 3u) / 4u);
    const bool clustersOver = clusters > kMaxCardClusters;
    if (shaderReady) {
        // THE FRAME'S FIRST: the list starts empty (one thread)
        if (mAccumClearFrame != mFrame) {   // (the cache's own frame: one clock with the card-to-card's clear)
            flushReport();   // (the previous frame's report read before this frame's clear)
            mAccumClearFrame = mFrame;
            const Ogre::uint32 mode[4] = { 1u, 0u, kReportCapacity, 0u };
            set("caMode", mode, false);
            params.setDirty();
            mAccumJob->setNumThreadGroupsBasedOn(Ogre::HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u);
            mAccumJob->setNumThreadGroups(1u, 1u, 1u);
            detail::photonDispatch(hc, mAccumJob, rs);
        }
        const Ogre::uint32 mode[4] = { 0u, mCardRecords, kReportCapacity, 0u };
        const float self[4] = { corner.x, corner.y, corner.z, brickSide };
        const Ogre::uint32 d[4] = { dims[0], dims[1], dims[2], dims[0] * dims[1] * dims[2] };
        // THE NEXT FINER BOX AND THE DISTANCE PAST WHICH A CARD READS THIS CASCADE INSIDE IT (hi[3], stage 5's read
        // W1): 0 - a card's diffuse cones hand over by their footprint (the hand-over mip 1, within ~1.7 cells of the
        // finer cascade: jah_voxel_march.glsl), so they read this cascade inside the finer box almost from the start and
        // a card skips none of its bricks
        float lo[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, hi[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if (finer) {
            const Ogre::Vector3 a = finer->getMinimum(), b = finer->getMaximum();
            lo[0] = a.x; lo[1] = a.y; lo[2] = a.z; lo[3] = 1.0f;
            hi[0] = b.x; hi[1] = b.y; hi[2] = b.z; hi[3] = 0.0f;
        }
        // THE REDUCE (one thread a cluster of 4^3 bricks), then THE ACCUMULATION (one thread a card)
        if (!clustersOver) {
            const Ogre::uint32 reduce[4] = { 2u, mCardRecords, kReportCapacity, 0u };
            set("caMode", reduce, false);
            set("caSelf", self, true);
            set("caDims", d, false);
            set("caInnerLo", lo, true);
            set("caInnerHi", hi, true);
            params.setDirty();
            mAccumJob->setNumThreadGroupsBasedOn(Ogre::HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u);
            mAccumJob->setNumThreadGroups((clusters + 63u) / 64u, 1u, 1u);
            detail::photonDispatch(hc, mAccumJob, rs);
            set("caMode", mode, false);
            params.setDirty();
            mAccumJob->setNumThreadGroups((mCardRecords + 63u) / 64u, 1u, 1u);
            detail::photonDispatch(hc, mAccumJob, rs);
            mAccumRan = true;
        }
    }
    // A MOVE THAT COULD NOT BE MEASURED (the job not built yet, or a cascade past the cluster table): every card's
    // indirect is listed - the region list's overflow rule (a stale set never misses a change it cannot bound)
    if (!shaderReady || clustersOver) {
        const unsigned long long root = 1ull << rootBit('r', mFrame);   // (R3 (c): no owed move's)
        for (CardRec &c : mCards)
            if (c.indirectValid) { c.relightIndirect = true; c.roots |= root; }
        ++mIndirectOverflows;
    }
    const Ogre::DescriptorSetUav::BufferSlot empty = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    for (Ogre::uint8 k = 0u; k < 5u; ++k) mAccumJob->_setUavBuffer(k, empty);
}

void SurfaceCache::endIndirectFrame(unsigned long long frame) {
    if (!mAccumRan || !mCardReport) return;
    mAccumRan = false;
    // (pending: the frame's later accumulation may still write into the report; flushReport reads it once)
    mReportPending = true;
    mReportPendingHost = frame;
    mReportPendingCache = mFrame;
}

void SurfaceCache::flushReport() {
    if (!mReportPending || !mCardReport) return;
    mReportPending = false;
    ReportTicket &r = mReportReadings[mReportNextIssue];
    if (r.ticket.get()) {
        // NO SLOT (the ring is sized for one report a frame over the read-back's lag, so this is a defect if it is seen):
        // a report never read must never strand its listings - every card's indirect is listed (the overflow rule, a
        // move that could not be measured) and the relights it carried count visible
        ++mReportsDropped;
        const unsigned long long root = 1ull << rootBit('r', mFrame);
        for (CardRec &c : mCards)
            if (c.indirectValid) { c.relightIndirect = true; c.roots |= root; }
        ++mIndirectOverflows;
        if (mMovesOwed) { ++mVisibleRelights; ++mUnmeasuredRelights; }
        mMovesOwed = false;
        listPendingRelits();
        return;
    }
    r.ticket = mCardReport->readRequest(0u, 3u + kReportCapacity, /*accurateTracking=*/false);
    r.frame = mReportPendingHost;
    r.cacheFrame = mReportPendingCache;
    r.carriesMoves = mMovesOwed;
    r.boxes.clear();
    if (mMovesOwed) r.boxes.swap(mPendingRelitBoxes);
    else listPendingRelits();
    mMovesOwed = false;
    mReportNextIssue = (mReportNextIssue + 1u) % kReportSlots;
}

void SurfaceCache::harvestIndirectReports(unsigned long long frame) {
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
    if (!vao) return;
    for (;;) {
        ReportTicket &r = mReportReadings[mReportNextTake];
        if (!r.ticket.get()) return;
        // TAKEN AT A FIXED LAG OF HOST FRAMES (the feedback readings' rule): a function of the frames, never of the GPU's pace.
        if (frame < r.frame || frame - r.frame <= vao->getDynamicBufferMultiplier()) return;
        const Ogre::uint32 *w = static_cast<const Ogre::uint32 *>(r.ticket->map());
        const Ogre::uint32 n = std::min<Ogre::uint32>(w[0], kReportCapacity);
        for (Ogre::uint32 k = 0; k < n; ++k) {
            // (bit 30: listed card to card, bit 31: the overflowed list's geometric bound took part - 10-2c)
            const Ogre::uint32 word = w[1u + k], slot = word & 0x3FFFFFFFu;
            if (slot >= mSlotCard.size() || mSlotCard[slot] < 0) continue;   // freed since: nothing to relight
            CardRec &listed = mCards[size_t(mSlotCard[slot])];
            // ANSWERED ALREADY (PHOTON-7 stage 10, R3): a listing of moves older than the card's own last march - the
            // march, a frame or more after them, read what they moved - is no work (re-marching it made a round for
            // nothing, and that round's moves the next)
            if (listed.lastIndirect && r.cacheFrame < listed.lastIndirect) { ++mIndirectAnswered; continue; }
            listed.relightIndirect = true;
            // ITS PARENTS (R3 (c)): listed by the VOXELS (no card-to-card bit: the bricks it reads moved) - the voxel
            // store's own move, 'v' at the report's frame; listed CARD TO CARD - the moves its accumulation summed since
            // its last march: every frame's relights in [its march, the report's frame], their roots and the deepest
            // round (frames the ring no longer holds are named by its horizon, 'h')
            if (!(word & 0x40000000u)) {
                listed.roots |= 1ull << rootBit('v', mFrame);
            } else {
                // (inclusive of its own march's frame: a move landed in the dispatch that marched it, read before it)
                const unsigned long long from = listed.lastIndirect ? listed.lastIndirect : listed.since,
                                         to = r.cacheFrame;
                unsigned long long roots = 0ull;
                unsigned round = 0u;
                for (unsigned long long f = to; f >= from && f > 0ull && to - f < 512ull; --f) {
                    const FrameRoots &fr = mFrameRoots[f % 512u];
                    if (fr.frame != f) continue;
                    roots |= fr.roots;
                    round = std::max(round, fr.round);
                }
                // (moves older than the ring: named by its horizon, 'h' at the oldest frame it holds - they are older)
                if (to > from + 511ull) roots |= 1ull << rootBit('h', to - 511ull);
                listed.roots |= roots;
                listed.round = std::max(listed.round, round + 1u);
            }
            ++mIndirectReported;
            if (word & 0x40000000u) ++mIndirectByCards;
            if (word & 0x80000000u) ++mIndirectByBound;
        }
        // THE READERS' THRESHOLD: the frame's relights moved a texel by a code (or a move was not finite) - VISIBLE - or
        // every texel stayed under one (the report's cumulative rule: against its last reported value)
        if (r.carriesMoves) {
            float codes = 0.0f;
            std::memcpy(&codes, &w[1u + kReportCapacity], sizeof(codes));
            const bool nonFinite = w[2u + kReportCapacity] != 0u;
            if (nonFinite || codes > 0.0f) {
                ++mVisibleRelights;
                mLateRelitBoxes.insert(mLateRelitBoxes.end(), r.boxes.begin(), r.boxes.end());
            } else
                ++mSubQuantumRelights;
            r.boxes.clear();
            mMoveCodesLast = nonFinite ? -1.0f : codes;
            mMoveCodesFrame = r.cacheFrame;
        }
        r.ticket->unmap();
        r.ticket.reset();
        mReportNextTake = (mReportNextTake + 1u) % kReportSlots;
    }
}

// ---------------------------------------------------------------------------
// The sun on the cards (PHOTON-CARDS-4; ATOM-S3-CARDCAP)
// ---------------------------------------------------------------------------
//
// A CARD'S SUN VISIBILITY IS TWO TERMS, BOTH TRACED, one ray per card texel
// towards the sun (rq_card_movers.comp, its two modes).
//
// THE STILL WORLD'S (ATOM-S3-CARDCAP): against the still world's shadow casters
// (kRayMaskStillCaster — a mover is never one, the one stillness rule the cards
// keep), into ShadowRough.x. Traced for every card the frame its capture lands,
// and for every card whose term went STALE: the sun turned, appeared or went
// (every resident card), or a still caster changed (the cards of its old and new
// footprints, below). The stale ones go oldest first, nearest among equals, under
// the relight's budget — the movers' order. A card stores the still world's sun
// term at its own texel scale, and one ray per texel IS that term: no shadow map
// fit per card (the card shadow node's PSSM, three caster passes per card, was
// 77 % of a capture's cost and is deleted), no filter.
//
// THE MOVERS' (PHOTON-CARDS-4): a shadow-casting mover's transform write traces
// one sun ray per texel of every card its sun-projected footprint reaches — the
// footprint it left AND the one it entered — against the movers alone
// (kRayMaskMoverCaster), into the R8 layer the relight multiplies into the sun's
// term. A mover at rest costs nothing: no write, no trace, no relight.
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
// its change stales the still term of the cards of its old and new footprints —
// a re-TRACE, never a recapture (the capture holds no light quantity).
namespace {
/// The CASTER's footprint margin, metres: a caster's AABB is conservative already;
/// this covers a PCF-free edge on the boundary. The RECEIVER's side carries the
/// lift: a card's footprint is grown by its own still-trace lift plus one texel
/// (cardFootprint's `margin`, sunLiftOf) — the ray starts that far off the card.
constexpr float kFootprintMargin = 0.05f;
/// THE STILL TRACE'S LIFT (rq_card_movers.comp's head; READ_A finding 3, READ_B finding 3 - the ORIGIN LAW every ray job
/// keeps, Types.h rayOriginLevelLift): the origin moves off the receiver along the captured surface's GEOMETRIC normal by
/// THE CAPTURED LEVEL'S BOUND against level 0 (CardRec::levelBound, the stored bound, times the bake's drift ratio
/// kLevelDriftOverBound: the surface the card captured lies within it of level 0, which every sun ray traces -
/// PHOTON-7 stage 6, ONE RAY LEVEL PER MESH), plus the Depth layer's R16F quantum at the card's far end; the shader adds
/// the point's float bound. NO TEXEL TERM (PHOTON-7 10-3 R10, B1): the capture writes ONE rasterised depth at each
/// texel's centre (JahCardCapture_piece_ps.any, outCardDepth), so a texel's point lies ON its surface - at any slant -
/// up to that quantum; the "one texel" that stood here started every ray a coarse card's texel off its surface (13 cm
/// on a far wall) and shortened every contact shadow by lift / tan(sun elevation). Derived, never a texel count: the
/// bake picks the level for its NOMINAL 128-texel page while a split card's texel is that page's / the split (up to
/// kCardMaxSplit 4), so a fixed "2 texels" let a convex card wider than 4 m start its sun rays inside level 0
/// (self-shadowed texels) - the level's bound is what clears that.
/// A ray's length: the sun is at infinity; a mover beyond the residency radius
/// casts onto no resident card anyway.
constexpr float kMoverRayRange = 10000.0f;
struct SunFrame {
    Ogre::Vector3 L, e1, e2;
};
using Footprint = CardSunFootprint;   ///< the projected rect, and the top along the sun (SurfaceCache.h)
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
/// `margin`: the card's still-trace lift plus one texel — a sun ray starts up to
/// that far off the card's box, so its footprint grows by it (by construction,
/// whatever the lift measures: 0.08-0.13 m in the suites' 192-px views).
Footprint cardFootprint(const SunFrame &f, const CardRec &card, float margin) {
    Ogre::Vector3 c[8];
    for (int i = 0; i < 8; ++i)
        c[i] = card.centre + card.u * ((i & 1) ? card.halfU : -card.halfU) +
               card.v * ((i & 2) ? card.halfV : -card.halfV) +
               card.d * ((i & 4) ? card.halfDepth : -card.halfDepth);
    Footprint p = project(f, c, false);   // .top = the card's LOWEST point along the sun
    p.a0 -= margin; p.a1 += margin;
    p.b0 -= margin; p.b1 += margin;
    return p;
}
bool shades(const Footprint &caster, const Footprint &card) {
    return caster.a0 <= card.a1 && card.a0 <= caster.a1 && caster.b0 <= card.b1 &&
           card.b0 <= caster.b1 && card.top < caster.top;
}

// THE CARDS' SUN-SPACE GRID (ENGINE-UPGRADE-U4 stage 2). `shades` needs the two
// rectangles to OVERLAP in the plane normal to the sun, so a footprint can only
// shade a card some point of whose rectangle it shares — and that point falls in
// ONE cell of a grid laid over the plane: the cell index is a monotone function
// of the coordinate (a subtraction, a multiplication by a positive constant and a
// truncation, each monotone in IEEE arithmetic), so a card filed in every cell its
// [a0, a1] x [b0, b1] spans and a query visiting every cell ITS rectangle spans
// share the shared point's cell. The search therefore visits a superset of the
// cards the exhaustive loop would test true, and the exact test (`shades`, the
// corner form) decides each: the hit set is the exhaustive loop's, card for card.
// A card wider than kGridMaxCells cells (a floor's card) or not finite is not
// filed: every query visits it (`wide`). A query whose rectangle is inverted or
// not finite (a degenerate box) cannot be placed on the grid: it visits every
// card. The grid is BUILT PER CALL (the cards a call asks about change call to
// call); a card's footprint carries its sun lift, which is world-fixed (sunLiftOf:
// the card's level bound, its texel and its depth quantum - no viewer term).
constexpr int kGridMaxSide = 256;      ///< cells a side, at most
constexpr unsigned kGridMaxCells = 64u; ///< a card spanning more cells than this is `wide`
bool finiteRect(const Footprint &f) {
    return std::isfinite(f.a0) && std::isfinite(f.a1) && std::isfinite(f.b0) && std::isfinite(f.b1);
}
int gridCell(float x, float origin, float inv, int n) {
    const float t = (x - origin) * inv;
    if (!(t > 0.0f)) return 0;
    if (t >= float(n)) return n - 1;
    return std::min(n - 1, int(t));
}
/// Files `cards` (indices into `fp`) on `g`.
void buildSunGrid(CardSunGrid &g, const std::vector<Footprint> &fp, const std::vector<unsigned> &cards) {
    g.items.clear();
    g.wide.clear();
    g.nx = g.ny = 0;
    if (g.stamp.size() < fp.size()) g.stamp.assign(fp.size(), 0u);
    g.allBounded = true;
    g.ua0 = g.ub0 = 1e30f; g.ua1 = g.ub1 = -1e30f;
    for (unsigned i : cards) {
        const Footprint &f = fp[i];
        if (!finiteRect(f) || !(f.a0 <= f.a1) || !(f.b0 <= f.b1)) { g.allBounded = false; break; }
        g.ua0 = std::min(g.ua0, f.a0); g.ua1 = std::max(g.ua1, f.a1);
        g.ub0 = std::min(g.ub0, f.b0); g.ub1 = std::max(g.ub1, f.b1);
    }
    // THE CELL SIDE: the median card's larger extent (a few wide cards must not
    // coarsen the grid), never finer than the filed span over kGridMaxSide.
    g.scratch.clear();
    for (unsigned i : cards)
        if (finiteRect(fp[i]) && fp[i].a0 <= fp[i].a1 && fp[i].b0 <= fp[i].b1)
            g.scratch.push_back(std::max(fp[i].a1 - fp[i].a0, fp[i].b1 - fp[i].b0));
    if (g.scratch.empty()) { g.wide = cards; return; }
    std::nth_element(g.scratch.begin(), g.scratch.begin() + g.scratch.size() / 2, g.scratch.end());
    const float median = std::max(g.scratch[g.scratch.size() / 2], 1e-4f);
    // A card more than 16 medians wide goes to `wide` before the bounds are taken.
    const float wideAt = 16.0f * median;
    float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
    const auto filed = [&](const Footprint &f) {
        return finiteRect(f) && f.a0 <= f.a1 && f.b0 <= f.b1 && f.a1 - f.a0 <= wideAt && f.b1 - f.b0 <= wideAt;
    };
    for (unsigned i : cards) {
        const Footprint &f = fp[i];
        if (!filed(f)) continue;
        a0 = std::min(a0, f.a0); a1 = std::max(a1, f.a1);
        b0 = std::min(b0, f.b0); b1 = std::max(b1, f.b1);
    }
    if (!(a0 <= a1)) { g.wide = cards; return; }
    const float side = std::max(median, std::max(a1 - a0, b1 - b0) / float(kGridMaxSide));
    g.a0 = a0; g.b0 = b0; g.a1 = a1; g.b1 = b1; g.inv = 1.0f / side;
    g.nx = std::max(1, std::min(kGridMaxSide, int((a1 - a0) * g.inv) + 1));
    g.ny = std::max(1, std::min(kGridMaxSide, int((b1 - b0) * g.inv) + 1));
    g.start.assign(size_t(g.nx) * size_t(g.ny) + 1u, 0u);
    // Two passes (count, then fill): one flat array, no list per cell.
    for (int pass = 0; pass < 2; ++pass) {
        for (unsigned i : cards) {
            const Footprint &f = fp[i];
            bool wide = !filed(f);
            int x0 = 0, x1 = -1, y0 = 0, y1 = -1;
            if (!wide) {
                x0 = gridCell(f.a0, g.a0, g.inv, g.nx); x1 = gridCell(f.a1, g.a0, g.inv, g.nx);
                y0 = gridCell(f.b0, g.b0, g.inv, g.ny); y1 = gridCell(f.b1, g.b0, g.inv, g.ny);
                wide = unsigned(x1 - x0 + 1) * unsigned(y1 - y0 + 1) > kGridMaxCells;
            }
            if (wide) { if (pass == 0) g.wide.push_back(i); continue; }
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const size_t c = size_t(y) * size_t(g.nx) + size_t(x);
                    if (pass == 0) ++g.start[c + 1u];
                    else g.items[g.start[c]++] = i;
                }
        }
        if (pass == 0) {
            for (size_t c = 1; c < g.start.size(); ++c) g.start[c] += g.start[c - 1];
            g.items.resize(g.start.back());
        } else {
            // the fill advanced each cell's start to the next cell's: shift back
            for (size_t c = g.start.size() - 1; c > 0; --c) g.start[c] = g.start[c - 1];
            g.start[0] = 0u;
        }
    }
}
/// A box's footprint rectangle, CONSERVATIVELY: the centre's projection grown by
/// the half-extents' projections (|e . axis| h — the box's exact interval in exact
/// arithmetic) plus the caster margin and a slack that covers the corner form's
/// rounding. Contains boxFootprint's rectangle; `top` is left unset. Not finite
/// for a box that is not.
Footprint boxReach(const SunFrame &f, const Ogre::Vector3 &mn, const Ogre::Vector3 &mx) {
    const Ogre::Vector3 c = (mn + mx) * 0.5f, h = (mx - mn) * 0.5f;
    const float a = f.e1.dotProduct(c), b = f.e2.dotProduct(c);
    const float ra = std::fabs(f.e1.x) * h.x + std::fabs(f.e1.y) * h.y + std::fabs(f.e1.z) * h.z;
    const float rb = std::fabs(f.e2.x) * h.x + std::fabs(f.e2.y) * h.y + std::fabs(f.e2.z) * h.z;
    // the slack is relative to the CORNERS' magnitude (what the dot products round at), not the projection's
    const float mag = std::fabs(c.x) + std::fabs(c.y) + std::fabs(c.z) + h.x + h.y + h.z;
    const float sa = kFootprintMargin + 1e-3f * (1.0f + mag);
    const float sb = kFootprintMargin + 1e-3f * (1.0f + mag);
    Footprint p;
    p.a0 = a - ra - sa; p.a1 = a + ra + sa;
    p.b0 = b - rb - sb; p.b1 = b + rb + sb;
    return p;
}
/// True when a search rectangle provably meets no card the grid was given.
bool missesEveryCard(const CardSunGrid &g, const Footprint &q) {
    if (!g.allBounded || !finiteRect(q) || !(q.a0 <= q.a1) || !(q.b0 <= q.b1)) return false;
    return q.a1 < g.ua0 || q.a0 > g.ua1 || q.b1 < g.ub0 || q.b0 > g.ub1;
}
/// A new query: the cards it visits are visited once however many of its
/// rectangles reach them (a move's old and new footprints).
unsigned beginSunQuery(CardSunGrid &g) {
    // THE STAMP'S WRAP: a card is visited once a query when its stamp equals the query's id. At the 32-bit wrap every
    // stamp is zeroed and the ids restart at 1 — no stamp written before the wrap can equal an id after it (a stamp is
    // 0 or an id handed out since). A stamp array grown for new cards starts at 0, which no query id is.
    if (++g.query == 0u) { std::fill(g.stamp.begin(), g.stamp.end(), 0u); g.query = 1u; }
    return g.query;
}
/// Visits every card `q`'s rectangle may overlap (a superset; the caller tests
/// each exactly), once per query. `all` = every card the grid holds, for a
/// rectangle the grid cannot place.
template <class Visit>
void visitSunGrid(CardSunGrid &g, const Footprint &q, const std::vector<unsigned> &all, Visit &&visit) {
    const unsigned id = g.query;
    const auto once = [&](unsigned i) {
        if (g.stamp[i] == id) return;
        g.stamp[i] = id;
        visit(i);
    };
    if (!finiteRect(q) || !(q.a0 <= q.a1) || !(q.b0 <= q.b1)) {
        for (unsigned i : all) once(i);
        return;
    }
    for (unsigned i : g.wide) once(i);
    // Every filed card lies inside [a0, a1] x [b0, b1]: a query outside it overlaps none (the cell clamp would
    // otherwise send it to the edge cells, a superset the exact test discards at a cost).
    if (!g.nx || q.a1 < g.a0 || q.a0 > g.a1 || q.b1 < g.b0 || q.b0 > g.b1) return;
    const int x0 = gridCell(q.a0, g.a0, g.inv, g.nx), x1 = gridCell(q.a1, g.a0, g.inv, g.nx);
    const int y0 = gridCell(q.b0, g.b0, g.inv, g.ny), y1 = gridCell(q.b1, g.b0, g.inv, g.ny);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const size_t c = size_t(y) * size_t(g.nx) + size_t(x);
            for (unsigned k = g.start[c]; k < g.start[c + 1u]; ++k) once(g.items[k]);
        }
}
}   // namespace

const Ogre::Light *SurfaceCache::cardSun() const {
    const Ogre::Light *sun = nullptr;
    if (!mSceneMgr) return nullptr;
    for (const SceneLightList::Entry &e : sceneLightListOf(mSceneMgr).entries(mSceneMgr)) {
        const Ogre::Light *l = e.light;
        if (l->getType() != Ogre::Light::LT_DIRECTIONAL || !l->getCastShadows() || !l->getVisible())
            continue;
        if (!sun || l->getId() < sun->getId()) sun = l;
    }
    return sun;
}

float SurfaceCache::sunLiftOf(const CardRec &c) const {
    // R16F keeps 11 bits: the depth quantum at the card's far end.
    const float depthQuantum = (2.0f * c.halfDepth + 2.0f * captureMargin(c.halfDepth)) / 1024.0f;
    return rayOriginLevelLift(c.levelBound) + depthQuantum;
}

void SurfaceCache::traceSun() {
    // ITS WHOLE CPU, every call (U4 stage 1, P1): the fixtures' reading of what a frame of moves costs here.
    struct Timed {
        TraceSunCost &c;
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() {
            c.micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
            ++c.calls;
        }
    } timed{ mTraceSunCost };
    mTraceSunCost.frame = mFrame;
    mTraceSunCost.casterMoves = 0u;
    mTraceSunCost.cards = unsigned(mCards.size());
    mTraceSunCost.casterHits = 0u;
    mTraceSunCost.casterHitDigest = 0ull;
    mTraceSunCost.hookMicros = mTraceSunCost.stepOneMicros = 0.0;
    mTraceSunCost.step3Micros = mTraceSunCost.step3bMicros = 0.0;
    mTraceSunCost.step2Micros = mTraceSunCost.step4Micros = mTraceSunCost.traceJobMicros = 0.0;
    mTraceSunCost.stillMicros = 0.0;
    mTraceSunCost.wideCards = 0u;
    // THIS FRAME ONLY: the batch is the frame's only when update() planned this
    // frame (workspacePreUpdate drops a stale plan the same way).
    // A batch that cannot be traced this frame waits for its still term.
    const auto batchWaits = [this]() {
        for (unsigned idx : mBatch) {
            CardRec &c = mCards[idx];
            if (!c.stillPending) c.stillPendingSince = mFrame;
            c.stillPending = true;
        }
    };
    if (!mMoverHooks.frame || !mMoverVis || !mRadiance) { batchWaits(); return; }
    if (mBatchFrame != Ogre::Root::getSingleton().getCompositorManager2()->getFrameCount()) return;
    CardMoverFrame mf;
    const auto tHook = std::chrono::steady_clock::now();
    const bool framed = mMoverHooks.frame(mf);
    mTraceSunCost.hookMicros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tHook).count();
    if (!framed) { batchWaits(); return; }
    mMovers.swap(mf.movers);
    if (mNullMove) {   // the test door: a still caster whose boxes are BOX_NULL (min +inf, max -inf)
        CardCasterMove m;
        const Ogre::Aabb nb = Ogre::Aabb::BOX_NULL;
        m.oldMin = m.newMin = nb.getMinimum();
        m.oldMax = m.newMax = nb.getMaximum();
        mf.casterMoves.push_back(m);
    }
    mTraceSunCost.casterMoves = unsigned(mf.casterMoves.size());

    const Ogre::Light *sunLight = cardSun();
    const Ogre::Vector3 toSun =
        sunLight ? (-sunLight->getDerivedDirection()).normalisedCopy() : Ogre::Vector3::ZERO;
    const bool haveSun = sunLight != nullptr;
    const SunFrame sf = sunFrame(haveSun ? toSun : Ogre::Vector3::UNIT_Y);
    // A card's footprint, computed once per call and only for a card something asks about (the grid files the
    // landed cards only).
    std::vector<Footprint> &cardFp = mTrace.cardFp;
    std::vector<char> &cardFpHave = mTrace.cardFpHave;
    cardFp.clear();
    cardFpHave.clear();
    const auto cardFpAt = [&](unsigned i) -> const Footprint & {
        if (cardFp.empty()) {
            cardFp.resize(mCards.size());
            cardFpHave.assign(mCards.size(), 0);
        }
        if (!cardFpHave[i]) {
            const CardRec &c = mCards[i];
            const float texel = 2.0f * std::max(c.halfU, c.halfV) / float(std::max(1u, c.size));
            cardFp[i] = cardFootprint(sf, c, sunLiftOf(c) + texel);
            cardFpHave[i] = 1;
        }
        return cardFp[i];
    };
    // Captured (its Depth and Normal landed): this frame's batch has, by now.
    const auto landed = [this](const CardRec &c) { return c.lastUpdated != 0ull && !c.queued; };
    const auto relightDirect = [this](unsigned i) {
        CardRec &c = mCards[i];
        if (std::find(mRelight.begin(), mRelight.end(), i) != mRelight.end()) return;
        if (mRelight.size() >= kMaxRelights) { c.relight = true; return; }
        mRelight.push_back(i);
        mRelightMode.push_back(c.indirectValid ? 0u : 2u);
    };
    const auto pendStill = [this](CardRec &c) {
        if (c.stillPending) return;
        c.stillPending = true;
        c.stillPendingSince = mFrame;
    };

    // 0. THE SUN CHANGED (turned, appeared, went, lost its shadows): every landed
    //    card's still term is stale. One count per gesture (the leading edge).
    {
        const bool changed = !mStillSunKnown || (toSun - mStillSun).squaredLength() > 1e-8f;
        if (changed && mStillSunKnown) {
            for (CardRec &c : mCards)
                if (landed(c)) pendStill(c);
            if (!mSunMovingLastFrame) ++mInvalidSun;
        }
        mSunMovingLastFrame = changed && mStillSunKnown;
        mStillSun = toSun;
        mStillSunKnown = true;
    }

    // 1. THE STILL CASTERS THAT MOVED — a transform, a show/hide, the caster
    //    bit, a class change (a drag's promotion and demotion, setNodeMovable),
    //    a deletion: their old and new footprints' cards' still term is stale.
    //    A card whose still term waits KEEPS its traced movers' term (step 3
    //    waits for `stillPending`): a demoted mover's shadow passes from the
    //    movers' trace to the still one without a frame of neither.
    // THE LANDED CARDS ON THE SUN-SPACE GRID (U4 stage 2; "The cards' sun-space
    // grid" above), built for a set of searches: step 1's (the moved still
    // casters), then steps 3 and 3b's (the movers). Only a landed card whose
    // CONSERVATIVE rectangle (its centre's projection grown by the sum of its
    // half-extents — the unit axes' projections are at most 1 — plus its margin
    // and a slack for the corners' rounding) meets the searches' bounding
    // rectangle is filed and has its exact footprint computed: a card outside it
    // overlaps no search, so leaving it out changes no answer.
    std::vector<unsigned> &landedCards = mTrace.landedCards;
    landedCards.clear();
    bool landedKnown = false;
    const auto landedList = [&]() -> const std::vector<unsigned> & {
        if (!landedKnown) {
            for (unsigned i = 0; i < mCards.size(); ++i)
                if (landed(mCards[i])) landedCards.push_back(i);
            landedKnown = true;
        }
        return landedCards;
    };
    struct SearchBounds {
        bool bounded = true, any = false;
        float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f;
        void add(const Footprint &f) {
            any = true;
            if (!finiteRect(f) || !(f.a0 <= f.a1) || !(f.b0 <= f.b1)) { bounded = false; return; }
            a0 = std::min(a0, f.a0); a1 = std::max(a1, f.a1);
            b0 = std::min(b0, f.b0); b1 = std::max(b1, f.b1);
        }
    };
    const auto mayReach = [&](const CardRec &c, const SearchBounds &u) {
        if (!u.bounded) return true;
        const float a = sf.e1.dotProduct(c.centre), b = sf.e2.dotProduct(c.centre);
        const float texel = 2.0f * std::max(c.halfU, c.halfV) / float(std::max(1u, c.size));
        const float r = c.halfU + c.halfV + c.halfDepth + sunLiftOf(c) + texel;
        // the slack is relative to the corners' magnitude (what the corner form rounds at)
        const float mag = std::fabs(c.centre.x) + std::fabs(c.centre.y) + std::fabs(c.centre.z) + r;
        const float reach = r + 1e-3f * (1.0f + mag);
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(reach)) return true;
        return a + reach >= u.a0 && a - reach <= u.a1 && b + reach >= u.b0 && b - reach <= u.b1;
    };
    std::vector<unsigned> &filedCards = mTrace.filedCards;   // the landed cards the current grid holds
    filedCards.clear();
    const auto buildGrid = [&](const SearchBounds &u) -> CardSunGrid & {
        filedCards.clear();
        for (unsigned i : landedList())
            if (mayReach(mCards[i], u)) { cardFpAt(i); filedCards.push_back(i); }
        buildSunGrid(mSunGrid, cardFp, filedCards);
        mTraceSunCost.wideCards = std::max(mTraceSunCost.wideCards, unsigned(mSunGrid.wide.size()));
        return mSunGrid;
    };
    const auto searchInto = [&](CardSunGrid &g, std::vector<char> &mask, const Footprint &f) {
        beginSunQuery(g);
        visitSunGrid(g, f, filedCards, [&](unsigned i) {
            if (!mask[i] && shades(f, cardFp[i])) mask[i] = 1;
        });
    };
    // The grid's guard (the arm cards.sunGridParity): the exhaustive search's
    // answer for one card set, compared card for card.
    bool parityRan = false;
    const auto parity = [&](const std::vector<char> &mask, const std::vector<char> &exhaustive) {
        parityRan = true;
        for (size_t i = 0; i < mask.size(); ++i)
            if ((mask[i] != 0) != (exhaustive[i] != 0)) ++mTraceSunCost.parityMisses;
    };
    const auto tStepOne = std::chrono::steady_clock::now();
    if (haveSun) {
        // Not yet captured or queued (its capture traces it) — the grid holds
        // only landed cards; captured THIS frame (traced below, after the scene
        // graph: it sees the caster where it is now), or the caster's own (a
        // moved instance re-allocates its cards, a hidden one gives them back).
        std::vector<char> &hit = mTrace.hit;
        hit.assign(mf.casterMoves.empty() ? 0u : mCards.size(), 0);
        const auto eligible = [&](unsigned i, NodeId mover) {
            const CardRec &c = mCards[i];
            return c.lastUpdated != mFrame && mInstances[c.instance].node != mover;
        };
        if (!mf.casterMoves.empty() && !landedList().empty()) {
            // Each move's CONSERVATIVE old and new rectangles (boxReach), the grid
            // for their bounds; then the exact footprints of the moves whose
            // rectangles meet a filed card's bounds at all.
            std::vector<Footprint> &reach = mTrace.reach;
            reach.assign(2u * mf.casterMoves.size(), Footprint());
            SearchBounds u;
            for (size_t k = 0; k < mf.casterMoves.size(); ++k) {
                const CardCasterMove &m = mf.casterMoves[k];
                reach[2u * k] = boxReach(sf, m.oldMin, m.oldMax);
                reach[2u * k + 1u] = boxReach(sf, m.newMin, m.newMax);
                u.add(reach[2u * k]);
                u.add(reach[2u * k + 1u]);
            }
            CardSunGrid &g = buildGrid(u);
            if (!g.empty()) {
                for (size_t k = 0; k < mf.casterMoves.size(); ++k) {
                    if (missesEveryCard(g, reach[2u * k]) && missesEveryCard(g, reach[2u * k + 1u])) continue;
                    const CardCasterMove &m = mf.casterMoves[k];
                    const Footprint o = boxFootprint(sf, m.oldMin, m.oldMax);
                    const Footprint n = boxFootprint(sf, m.newMin, m.newMax);
                    const auto test = [&](unsigned i) {
                        if (hit[i] || !eligible(i, m.node)) return;
                        const Footprint &cf = cardFp[i];
                        if (shades(o, cf) || shades(n, cf)) hit[i] = 1;
                    };
                    beginSunQuery(g);
                    visitSunGrid(g, o, filedCards, test);
                    visitSunGrid(g, n, filedCards, test);
                }
            }
        }
        if (mSunGridParity && !mf.casterMoves.empty()) {
            std::vector<char> all(mCards.size(), 0);
            for (const CardCasterMove &m : mf.casterMoves) {
                const Footprint o = boxFootprint(sf, m.oldMin, m.oldMax);
                const Footprint n = boxFootprint(sf, m.newMin, m.newMax);
                for (unsigned i = 0; i < mCards.size(); ++i) {
                    if (!landed(mCards[i]) || !eligible(i, m.node)) continue;
                    const Footprint &cf = cardFpAt(i);
                    if (shades(o, cf) || shades(n, cf)) all[i] = 1;
                }
            }
            parity(hit, all);
        }
        for (unsigned i = 0; i < hit.size(); ++i) {
            if (!hit[i]) continue;
            CardRec &c = mCards[i];
            if (!c.stillPending) ++mCasterRetraces;
            pendStill(c);
        }
        mTraceSunCost.stepOneMicros =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tStepOne).count();
        unsigned long long digest = 1469598103934665603ull;
        for (unsigned i = 0; i < hit.size(); ++i) {
            if (!hit[i]) continue;
            ++mTraceSunCost.casterHits;
            for (int b = 0; b < 4; ++b) { digest ^= (i >> (8 * b)) & 0xFFu; digest *= 1099511628211ull; }
        }
        mTraceSunCost.casterHitDigest = mTraceSunCost.casterHits ? digest : 0ull;
    }

    // 1b. THE STILL TRACE: this frame's batch whole (a capture is not readable
    //     until its sun term is), then the stale cards oldest first, nearest among
    //     equals, under the relight's budget and the relight list's room.
    {
        const auto tStill = std::chrono::steady_clock::now();
        std::vector<unsigned> still(mBatch.begin(), mBatch.end());
        std::vector<unsigned> stale;
        for (unsigned i = 0; i < mCards.size(); ++i)
            if (mCards[i].stillPending && landed(mCards[i]) &&
                std::find(still.begin(), still.end(), i) == still.end())
                stale.push_back(i);
        const Ogre::Vector3 eye = mViewerPos;
        std::sort(stale.begin(), stale.end(), [this, &eye](unsigned a, unsigned b) {
            if (mCards[a].stillPendingSince != mCards[b].stillPendingSince)
                return mCards[a].stillPendingSince < mCards[b].stillPendingSince;
            const float da = (mCards[a].centre - eye).squaredLength();
            const float db = (mCards[b].centre - eye).squaredLength();
            if (da != db) return da < db;
            return a < b;
        });
        unsigned spent = 0u;
        const size_t room = mRelight.size() < kMaxRelights ? kMaxRelights - mRelight.size() : 0u;
        size_t taken = 0u;
        for (unsigned idx : stale) {
            const unsigned cost = mCards[idx].size * mCards[idx].size;
            if (taken && spent + cost > mLightBudget) break;
            if (still.size() >= room) break;
            still.push_back(idx);
            spent += cost;
            ++taken;
        }
        mStillPending = unsigned(stale.size() - taken);
        if (!still.empty()) {
            mStillCpu.assign(still.size() * kRelightFloats, 0.0f);
            for (size_t i = 0; i < still.size(); ++i) {
                CardRec &c = mCards[still[i]];
                c.sunLift = sunLiftOf(c);
                float *r = &mStillCpu[i * kRelightFloats];
                const Ogre::Vector3 cam = c.centre + c.d * (c.halfDepth + captureMargin(c.halfDepth));
                r[0] = float(c.atlasX); r[1] = float(c.atlasY); r[2] = float(c.size); r[3] = c.sunLift;
                r[4] = cam.x; r[5] = cam.y; r[6] = cam.z;
                r[8] = c.u.x; r[9] = c.u.y; r[10] = c.u.z; r[11] = std::max(2.0f * c.halfU, 1e-4f);
                r[12] = c.v.x; r[13] = c.v.y; r[14] = c.v.z; r[15] = std::max(2.0f * c.halfV, 1e-4f);
                r[16] = c.d.x; r[17] = c.d.y; r[18] = c.d.z;
            }
            CardMoverTrace job;
            job.records = mStillCpu.data();
            job.count = unsigned(still.size());
            job.depth = mAtlas[unsigned(CardLayer::Depth)];
            job.normal = mAtlas[unsigned(CardLayer::Normal)];
            job.vis = mMoverVis;
            job.shadowRough = mAtlas[unsigned(CardLayer::ShadowRough)];
            job.lights = ensureLightBuffer();   // bound, not read, in this mode
            job.sunVis = mSunVis;
            job.open = mOpen;
            job.toSun = toSun;   // ZERO without a sun: every texel lit
            job.range = kMoverRayRange;
            job.still = true;
            if (mMoverHooks.trace && mMoverHooks.trace(job)) {
                for (unsigned idx : still) {
                    CardRec &c = mCards[idx];
                    c.stillPending = false;
                    c.openValid = true;   // (the still trace wrote this capture's open layer)
                    ++mStillTraces;
                    ++mStillTracedLastFrame;
                    mStillTexelsLastFrame += c.size * c.size;
                    relightDirect(idx);
                }
            } else {
                // No structure this frame: every one of them waits (the batch's
                // cards read the capture's constant 1.0 until then).
                for (unsigned idx : still) pendStill(mCards[idx]);
                mStillPending = unsigned(still.size() + stale.size() - taken);
            }
        }
        mStillCpuMs = float(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tStill).count());
        mTraceSunCost.stillMicros = double(mStillCpuMs) * 1000.0;
    }

    const auto tStep2 = std::chrono::steady_clock::now();
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

    mTraceSunCost.step2Micros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tStep2).count();
    const auto tStep3 = std::chrono::steady_clock::now();
    // 3. THE CARDS: inside a changed footprint, or captured this frame (their
    //    Depth is new) — pending; a pending card no CURRENT footprint reaches is
    //    retired instead (its term dropped, relit without it).
    // Steps 3 and 3b search ONE grid, built for the movers' changed and current footprints together, only when one
    // of them searches.
    const bool canTrace = haveSun && bool(mMoverHooks.trace);
    CardSunGrid *moverGrid = nullptr;
    const auto moversGrid = [&]() -> CardSunGrid & {
        if (!moverGrid) {
            SearchBounds u;
            for (const Footprint &f : changed) u.add(f);
            if (canTrace) for (const Footprint &f : current) u.add(f);
            moverGrid = &buildGrid(u);
        }
        return *moverGrid;
    };
    if (!changed.empty()) {
        std::vector<char> &inside = mTrace.inside;
        inside.assign(mCards.size(), 0);
        for (const Footprint &f : changed) searchInto(moversGrid(), inside, f);
        if (mSunGridParity) {
            std::vector<char> all(mCards.size(), 0);
            for (unsigned i = 0; i < mCards.size(); ++i) {
                if (!landed(mCards[i])) continue;
                for (const Footprint &f : changed)
                    if (shades(f, cardFpAt(i))) { all[i] = 1; break; }
            }
            parity(inside, all);
        }
        for (unsigned i = 0; i < inside.size(); ++i)
            if (inside[i]) pend(mCards[i]);
    }
    for (unsigned idx : mBatch)
        if (!mMovers.empty() || mCards[idx].moverTraced) pend(mCards[idx]);

    mTraceSunCost.step3Micros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tStep3).count();
    const auto tStep3b = std::chrono::steady_clock::now();
    std::vector<unsigned> trace;
    mMoverPending = 0u;
    mMoverPendingAge = 0u;
    // WHICH LANDED CARDS A CURRENT MOVER'S FOOTPRINT REACHES — searched on the
    // grid, only when a landed card waits on the movers' term. (The batch's
    // pend just above makes no card landed or unlanded: the wait is read now.)
    std::vector<char> &reachedBy = mTrace.reachedBy;
    reachedBy.clear();
    if (canTrace && !current.empty()) {
        bool waiting = false;
        for (const CardRec &c : mCards)
            if (c.moverPending && landed(c)) { waiting = true; break; }
        if (waiting) {
            reachedBy.assign(mCards.size(), 0);
            for (const Footprint &f : current) searchInto(moversGrid(), reachedBy, f);
            if (mSunGridParity) {
                std::vector<char> all(mCards.size(), 0);
                for (unsigned i = 0; i < mCards.size(); ++i) {
                    if (!landed(mCards[i])) continue;
                    for (const Footprint &f : current)
                        if (shades(f, cardFpAt(i))) { all[i] = 1; break; }
                }
                parity(reachedBy, all);
            }
        }
    }
    if (parityRan) ++mTraceSunCost.parityChecks;
    for (unsigned i = 0; i < mCards.size(); ++i) {
        CardRec &c = mCards[i];
        if (!c.moverPending) continue;
        if (!landed(c)) continue;   // waits for its capture
        const bool reached = !reachedBy.empty() && reachedBy[i];
        if (!reached) {
            // Its still term is stale (a demoted mover): the movers' term stands
            // until the still trace lands, never a frame of neither.
            if (c.stillPending) continue;
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
    mTraceSunCost.step3bMicros =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - tStep3b).count();
    if (trace.empty()) return;

    // (step 4's and the trace job's own time, to the door, whichever return ends the call)
    struct StepTimer {
        double &us; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        explicit StepTimer(double &u) : us(u) {}
        ~StepTimer() { us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count(); }
    };
    std::unique_ptr<StepTimer> step4Timer(new StepTimer(mTraceSunCost.step4Micros));
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
        r[7] = c.openValid ? 1.0f : 0.0f;   // (the open layer holds this capture's: 11-2 (5))
    }
    CardMoverTrace job;
    job.records = mMoverCpu.data();
    job.count = unsigned(now.size());
    job.depth = mAtlas[unsigned(CardLayer::Depth)];
    job.normal = mAtlas[unsigned(CardLayer::Normal)];
    job.vis = mMoverVis;
    job.shadowRough = mAtlas[unsigned(CardLayer::ShadowRough)];   // bound, not written, in this mode
    job.lights = ensureLightBuffer();   // bound, not read, in this mode
    job.sunVis = mSunVis;
    job.open = mOpen;
    job.toSun = toSun;
    job.range = kMoverRayRange;
    step4Timer.reset();
    StepTimer jobTimer(mTraceSunCost.traceJobMicros);   // the movers' trace recorded, the relight hand-off
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
bool SurfaceCache::tablesCurrent() const {
    unsigned slotsWanted = std::max(kInstanceSlotsInitial, mInstanceSlots);
    while (mItemSlots > slotsWanted) slotsWanted *= 2u;
    return !mTableDirty && mInstanceBuffer && mCardBuffer && slotsWanted == mInstanceSlots;
}

bool SurfaceCache::syncBuffers() {
    Ogre::VaoManager *vao = Ogre::Root::getSingleton().getRenderSystem()->getVaoManager();
    if (!vao) return false;
    // THE TABLES EXIST FROM THE FIRST FRAME, holding no card (PHOTON-7 stage 5, 5-2): the readers' demand stamps, written
    // into the instance table, are what makes a card resident at all; the table covers every item slot of the scene.
    unsigned slotsWanted = std::max(kInstanceSlotsInitial, mInstanceSlots);
    while (mItemSlots > slotsWanted) slotsWanted *= 2u;
    if (tablesCurrent()) {
        uploadDemandHead();
        return false;
    }
    mTableDirty = false;

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
        for (unsigned k = 0; k < kCardLampTable; ++k) rec.lamps[k] = k < c.lampCount ? c.lampIds[k] : 0u;
        std::memcpy(&mCardBufferCpu[size_t(i) * kCardGpuFloats], &rec, sizeof(rec));
    }
    mCardRecords = records;
    // ...AND THE INDIRECT HALF'S RECORDS (PHOTON-7 stage 5, 5-1; accumulateIndirect): each card's world box and its
    // atlas SLOT (its corner in 16-texel cells, the per-card state's index), and the slot's card (the report's map back).
    mCardBoxCpu.assign(size_t(kCardRecordCeiling) * 8u, 0.0f);
    mSlotCard.assign(size_t(kCardAtlasSize / kCardMinSize) * (kCardAtlasSize / kCardMinSize), -1);
    for (unsigned i = 0; i < records; ++i) {
        const CardRec &c = mCards[i];
        const float hd = c.halfDepth + captureMargin(c.halfDepth);
        const Ogre::Vector3 ext(std::fabs(c.u.x) * c.halfU + std::fabs(c.v.x) * c.halfV + std::fabs(c.d.x) * hd,
                                std::fabs(c.u.y) * c.halfU + std::fabs(c.v.y) * c.halfV + std::fabs(c.d.y) * hd,
                                std::fabs(c.u.z) * c.halfU + std::fabs(c.v.z) * c.halfV + std::fabs(c.d.z) * hd);
        const unsigned slot = (c.atlasY / kCardMinSize) * (kCardAtlasSize / kCardMinSize) + c.atlasX / kCardMinSize;
        float *b = &mCardBoxCpu[size_t(i) * 8u];
        b[0] = c.centre.x; b[1] = c.centre.y; b[2] = c.centre.z;
        std::memcpy(&b[3], &slot, sizeof(slot));
        b[4] = ext.x; b[5] = ext.y; b[6] = ext.z;
        mSlotCard[slot] = int(i);
    }
    if (ensureIndirectBuffers()) mCardBoxDelta.upload(mCardBoxBuffer, mCardBoxCpu.data(), 0, size_t(kCardRecordCeiling) * 2u);

    // THE INSTANCE TABLE, indexed by the item slot. A slot with no cards reads
    // (0, 0) and the shader takes the voxel path it always took — which is why
    // the whole table is zeroed first and only the resident instances write.
    unsigned maxSlot = 0u;
    for (const InstanceRec &inst : mInstances)
        if (inst.cardCount && inst.itemSlot != size_t(-1))
            maxSlot = std::max(maxSlot, unsigned(inst.itemSlot));
    unsigned slots = slotsWanted;
    while (maxSlot + 1u > slots) slots *= 2u;
    if (slots != mInstanceSlots && mInstanceBuffer) {
        // A GROWN TABLE: the demand region after it moves with it, so the read-backs in flight describe the old one
        // and are dropped (their stamps are re-written by the next frame's hits).
        vao->destroyUavBuffer(mInstanceBuffer);
        mInstanceBuffer = nullptr;
        mInstanceDelta.reset(nullptr);
        for (DemandTicket &t : mDemandReadings) t.ticket.reset();
        mDemandNextIssue = mDemandNextTake = 0u;
    }
    mInstanceSlots = slots;
    // THE TABLE AND, AFTER IT, THE DEMAND REGION (jah_rq_card_bindings.glsl), zeroed at creation and written by the
    // readers from then on; the host uploads the table's entries and the head only.
    // (the demand region's host copy is zeros the GPU never reads back from here - the buffer's creation data only - so
    // a re-sync of an unchanged size re-zeroes the TABLE alone: the whole 2.5 MB at a 16k-slot table, every dirty frame,
    // was a 10k world's syncBuffers - scale.cpu_walks, PHOTON-7 10-3)
    const size_t instanceWords = (size_t(slots) + demandEntries(slots)) * 4u;
    if (mInstanceBufferCpu.size() != instanceWords) mInstanceBufferCpu.assign(instanceWords, 0u);
    else std::fill(mInstanceBufferCpu.begin(), mInstanceBufferCpu.begin() + std::ptrdiff_t(size_t(slots) * 4u), 0u);
    for (const InstanceRec &inst : mInstances) {
        if (!inst.cardCount || inst.itemSlot == size_t(-1)) continue;
        unsigned *e = &mInstanceBufferCpu[size_t(inst.itemSlot) * 4u];
        e[0] = inst.firstCard;
        e[1] = inst.cardCount;
    }

    // (the tables' CHANGED rows only: DeltaUpload, PHOTON-7's walk hitch)
    if (!mCardBuffer) {
        mCardBuffer = vao->createUavBuffer(kCardRecordCeiling, sizeof(CardGpuRec), 0,
                                           mCardBufferCpu.data(), false);
        mCardDelta.reset(mCardBuffer, mCardBufferCpu.data(), size_t(kCardRecordCeiling) * sizeof(CardGpuRec));
    } else {
        mCardDelta.upload(mCardBuffer, mCardBufferCpu.data(), 0, kCardRecordCeiling);
    }
    if (!mInstanceBuffer) {
        mInstanceBuffer = vao->createUavBuffer(size_t(slots) + demandEntries(slots), 4u * sizeof(unsigned), 0,
                                               mInstanceBufferCpu.data(), false);
        // (the slot entries only: the demand region after them is the GPU's)
        mInstanceDelta.reset(mInstanceBuffer, mInstanceBufferCpu.data(), size_t(slots) * 4u * sizeof(unsigned));
    } else {
        mInstanceDelta.upload(mInstanceBuffer, mInstanceBufferCpu.data(), 0, slots);
    }
    uploadDemandHead();
    return true;
}

void SurfaceCache::uploadDemandHead() {
    if (!mInstanceBuffer) return;
    // THE FRAME'S STAMP (the cache's frame: every hit of this frame writes it) and the count arm.
    const Ogre::uint32 head[4] = { Ogre::uint32(mFrame), mDemandCount ? 1u : 0u, 0u, 0u };
    mInstanceBuffer->upload(head, mInstanceSlots, 1u);
}

void SurfaceCache::freezeFrame() {
    mRelitBoxes.clear();
    mRelitInstances.clear();
    mFrozen = true;
    mBatch.clear();
    mRelight.clear();
    mRelightMode.clear();
    mCapturesLastFrame = mRelitLastFrame = mIndirectLastFrame = 0u;
    mStillTracedLastFrame = mMoverTracedLastFrame = 0u;
    if (mWs) mWs->setExecutionMask(0u);
}

void SurfaceCache::update(const CardSceneView &view) {
    mRelitBoxes.clear();
    mRelitInstances.clear();
    if (!mBuilt) return;
    mFrozen = false;
    flushReport();   // (the last frame's report, if no accumulation of this frame has read it yet)
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
    mStillTracedLastFrame = 0u;
    mLampTracedLastFrame = 0u;
    mStillTexelsLastFrame = 0u;
    mViewerPos = view.viewerPos;
    mBudget = view.budgetTexels;
    mLightBudget = view.lightBudgetTexels;
    mAreaStrata = std::max(1u, view.areaStrata);
    mIndirectBudget = view.indirectBudgetTexels;
    mItemSlots = view.itemSlots;
    mDemandCount = view.demandCount;

    refreshResidency(view);
    // THE GPU TABLES follow the ALLOCATION and not the captures: a re-captured
    // card lands in the same rect, so nothing here moves when the queue drains.
    // ...and THE DEMAND's head (the frame's stamp) every frame, then its read-back on the period.
    // ONE ROW (PHOTON-7's naming step): the tables' uploads are GPU copies before the capture that no pass holds -
    // cards.tables times them, filed when the tables were written or the uploads submitted (the frame's demand stamp
    // and the periodic read-back are the at-rest constants: a still frame files no card row, the stillness door's law).
    // THE ROW IS THE TABLES' WRITE ALONE (PHOTON-7 stage 10, R3): the frame's demand stamp and the read-back's request are
    // the at-rest constants and file no row - now and then they submit the staging blocks, and inside the row that filed
    // a card row at rest with no table written (gi.hold_after_stop (1)'s room walk: a "cards.tables:0" 109 frames after
    // the stop; photon-glass 63 frames into its hold).
    if (tablesCurrent()) {
        uploadDemandHead();
    } else {
        detail::monitor::PhaseScope tables("cards.tables", Ogre::Root::getSingleton().getRenderSystem());
        if (syncBuffers()) tables.noteWork();
    }
    issueDemandRead();

    // NO LIGHT RE-QUEUES A CAPTURE (ATOM-S3-CARDCAP): a capture holds albedo,
    // normal, depth, emissive and roughness — no light quantity at all. The sun's
    // visibility is traced (traceSun: a sun change re-traces every resident
    // card's still term and captures nothing), and every other light is the
    // relight's (the changed-region list's stale set, planRelights).

    // THE QUEUE, IN LUMEN'S ORDER: priority = lastRead - lastUpdated (the READERS' last read: PHOTON-7 stage 5, 5-2),
    // drained highest first. A card that has never been captured has `lastUpdated` 0 and therefore the highest
    // priority there is, which is what makes a new face's cards arrive before an old one's are refreshed.
    mQueue.clear();
    for (unsigned i = 0; i < mCards.size(); ++i)
        if (mCards[i].queued) mQueue.push_back(i);
    std::sort(mQueue.begin(), mQueue.end(), [this](unsigned a, unsigned b) {
        const long long pa = (long long)mCards[a].lastRead - (long long)mCards[a].lastUpdated;
        const long long pb = (long long)mCards[b].lastRead - (long long)mCards[b].lastUpdated;
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

unsigned SurfaceCache::rootBit(char kind, unsigned long long frame) {
    for (unsigned k = 0; k < 64u; ++k)
        if (mRoots[k].kind == kind && mRoots[k].frame == frame) return k;   // one root a cause a frame
    const unsigned b = mRootNext;
    mRoots[b] = Root{ kind, frame };
    mRootNext = (mRootNext + 1u) % 64u;
    return b;
}

void SurfaceCache::noteMaterialChanged(MaterialId material) {
    bool any = false;
    for (const InstanceRec &inst : mInstances) {
        if (!inst.cardCount || inst.material != material) continue;
        const unsigned long long root = 1ull << rootBit('r', mFrame);
        for (unsigned c = 0; c < inst.cardCount; ++c) {
            mCards[inst.firstCard + c].queued = true;
            mCards[inst.firstCard + c].roots |= root;
            mCards[inst.firstCard + c].sceneCause = true;   // (the edit's first round: CardScene)
        }
        any = true;
    }
    if (any) ++mInvalidMaterial;
}

// ---------------------------------------------------------------------------
// What it publishes
// ---------------------------------------------------------------------------
void SurfaceCache::photonQuads(std::vector<PhotonCardQuad> &out) const {
    out.clear();
    if (!mBuilt) return;
    for (const InstanceRec &inst : mInstances) {
        if (!inst.node || !inst.cardCount) continue;
        for (unsigned c = 0; c < inst.cardCount; ++c) {
            const CardRec &card = mCards[inst.firstCard + c];
            PhotonCardQuad q;
            // THE CARD'S PLANE: the face of its box the capture looks through
            // (aimCamera's near side) — for a flat surface, the surface itself.
            q.centre = card.centre + card.d * card.halfDepth;
            q.halfU = card.u * card.halfU;
            q.halfV = card.v * card.halfV;
            q.normal = card.d;
            q.held = card.lastUpdated > 0ull;
            q.age = q.held && mFrame > card.lastUpdated ? mFrame - card.lastUpdated : 0ull;
            out.push_back(q);
        }
    }
}

bool SurfaceCache::restHoldOwed() const {
    if (!mBuilt) return false;
    if (!mRestDecided) return true;   // the stop's one decision is still to come
    if (mReportPending || mReportNextTake != mReportNextIssue) return true;   // a report pending or in flight: a round may follow
    for (const CardRec &c : mCards)
        if (c.size && (c.queued || c.relight || c.relightIndirect || c.stillPending)) return true;
    return false;
}

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
    bytes += bytesOf(mFlat);
    bytes += bytesOf(mSunVis);
    bytes += bytesOf(mMoverVis);
    bytes += bytesOf(mOpen);
    if (mRadiance)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mRadiance->getPixelFormat());
    if (mIndirect)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mIndirect->getPixelFormat());
    if (mFlat)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mFlat->getPixelFormat());
    if (mSunVis)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mSunVis->getPixelFormat());
    if (mMoverVis)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mMoverVis->getPixelFormat());
    if (mOpen)
        out.bytesPerTexel += Ogre::PixelFormatGpuUtils::getBytesPerPixel(mOpen->getPixelFormat());
    out.bytes = bytes;
    out.emissiveFormat = mEmissiveFormatName;
    out.radianceFormat = mRadianceFormatName;
    out.lightBudgetTexels = mLightBudget;
    out.relitLastFrame = mRelitLastFrame;
    out.relitTexelsLastFrame = mRelitTexelsLastFrame;
    out.relights = mRelights;
    out.invalidRadiance = mInvalidRadiance;
    out.lightsInRange = mLightsInRange;
    out.lightsDropped = mLightsDropped;
    out.indirectBudgetTexels = mIndirectBudget;
    out.indirectLastFrame = mIndirectLastFrame;
    out.indirectTexelsLastFrame = mIndirectTexelsLastFrame;
    out.indirectRelights = mIndirectRelights;
    out.invalidIndirect = mInvalidIndirect;
    out.indirectReported = mIndirectReported;
    out.indirectOverflows = mIndirectOverflows;
    out.indirectByRegion = mIndirectByRegion;
    out.indirectOn = mIndirectOnLastRelight;
    out.lightMs = mLightMs;
    out.instancesResident = 0u;
    for (const InstanceRec &i : mInstances) out.instancesResident += i.cardCount ? 1u : 0u;
    out.cardsResident = unsigned(mCards.size());
    {
        // (the resident set's fingerprint: its pairs sorted, then FNV-1a over their bytes - an order-free identity)
        std::vector<std::pair<NodeId, unsigned>> set;
        set.reserve(mCards.size());
        for (const CardRec &c : mCards)
            if (c.instance < mInstances.size()) set.emplace_back(mInstances[c.instance].node, c.index);
        std::sort(set.begin(), set.end());
        unsigned long long h = set.empty() ? 0ull : 1469598103934665603ull;
        const auto mix = [&h](unsigned long long v, unsigned bytes) {
            for (unsigned b = 0; b < bytes; ++b) { h ^= (v >> (8u * b)) & 0xFFull; h *= 1099511628211ull; }
        };
        for (const auto &p : set) { mix(p.first, 8u); mix(p.second, 4u); }
        out.residentHash = h;
    }
    out.admitted = mAdmitted;
    out.cacheFrame = mFrame;
    out.arrivalMarkNewest = mArrivalMarkNewest;
    out.demandReadEvery = unsigned(kDemandReadEvery);
    out.demandChangeFrame = mDemandChangeFrame;
    out.demandFirst[0] = mDemandFirst[0];
    out.demandFirst[1] = mDemandFirst[1];
    out.demandNewest[0] = mDemandNewest[0];
    out.demandNewest[1] = mDemandNewest[1];
    out.demandOpenLag = mDemandOpenLag;
    {
        Ogre::RenderSystem *rs = Ogre::Root::getSingletonPtr() ? Ogre::Root::getSingleton().getRenderSystem() : nullptr;
        Ogre::VaoManager *vao = rs ? rs->getVaoManager() : nullptr;
        out.demandReadLag = vao ? unsigned(vao->getDynamicBufferMultiplier()) + 1u : 0u;
    }
    out.evicted = mEvicted;
    out.compactionMoves = mCompactionMoves;
    out.lampTraces = mLampTraces;
    out.lampTracedLastFrame = mLampTracedLastFrame;
    out.cardLampsMax = mCardLampsMax;
    out.lampOverflowShareMax = mLampOverflowShareMax;
    out.cardAreaLampsMax = mCardAreaLampsMax;
    out.cardsMultiArea = out.cardsSharedFractional = 0u;
    for (const CardRec &c : mCards) {
        out.cardsMultiArea += c.areaLampCount >= 2u ? 1u : 0u;
        out.cardsSharedFractional += c.fractionalCount >= 2u ? 1u : 0u;
    }
    out.lampRaysLastTrace = mLampRaysLastTrace;
    if (mMoverHooks.gatherCounts) mMoverHooks.gatherCounts(out.gather);
    out.gatherWaits = mGatherWaits;
    out.indirectByCards = mIndirectByCards;
    out.indirectAnswered = mIndirectAnswered;
    out.reportsDropped = mReportsDropped;
    out.visibleRelights = mVisibleRelights;
    out.readersHoldOwed = restHoldOwed();
    out.subQuantumRelights = mSubQuantumRelights;
    out.unmeasuredRelights = mUnmeasuredRelights;
    out.moveCodesLast = mMoveCodesLast;
    out.moveCodesFrame = mMoveCodesFrame;
    out.restDrained = mRestDrained;
    out.decisionUsedTexels = mDecisionUsed;
    out.decisionFreeTexels = mDecisionFree;
    out.decisionAdmitted = mDecisionAdmitted;
    out.decisionReaders = mDecisionReaders;
    out.decisionRefused = mDecisionRefused;
    out.decisionRefusedCards = mDecisionRefusedCards;
    out.restDecisions = mRestDecisions;
    out.windowDrains = mWindowDrains;
    out.workRoots.clear();
    for (unsigned k = 0; k < 64u; ++k)
        if (mPlanRoots & (1ull << k)) out.workRoots.push_back(CardCacheStatus::WorkRoot{ mRoots[k].kind, mRoots[k].frame });
    out.workRound = mPlanRound;
    out.indirectByBound = mIndirectByBound;
    out.gatherByRays = mGatherByRays;
    out.areaStrata = mAreaStrata;
    for (unsigned k = 0; k < 4u; ++k) out.demandHits[k] = mDemandHits[k];
    out.queueLength = 0u;
    for (const CardRec &c : mCards) out.queueLength += c.queued ? 1u : 0u;
    out.budgetTexels = mBudget;
    out.capturesLastFrame = mCapturesLastFrame;
    out.texelsLastFrame = mTexelsLastFrame;
    out.captures = mCaptures;
    out.invalidTransform = mInvalidTransform;
    out.invalidMaterial = mInvalidMaterial;
    out.captureMs = mCaptureMs;
    out.captureWorkspaceMs = mWsMs;
    out.captureCopyMs = mCopyMs;
    out.captureTraceMs = mStillCpuMs;
    out.cardRecords = mCardRecords;
    out.instanceSlots = mInstanceSlots;
    out.moverCasters = unsigned(mMovers.size());
    out.moverTracedLastFrame = mMoverTracedLastFrame;
    out.moverTexelsLastFrame = mMoverTexelsLastFrame;
    out.moverTraces = mMoverTraces;
    out.moverRetired = mMoverRetired;
    out.moverPending = mMoverPending;
    out.moverPendingAge = mMoverPendingAge;
    out.invalidSun = mInvalidSun;
    out.stillTraces = mStillTraces;
    out.stillTracedLastFrame = mStillTracedLastFrame;
    out.stillTexelsLastFrame = mStillTexelsLastFrame;
    out.stillPending = mStillPending;
    out.casterRetraces = mCasterRetraces;
    if (mMoverHooks.readTimes) mMoverHooks.readTimes(out.moverGpuMs, out.stillGpuMs);
    out.relightGpuMs = detail::monitor::lastGpuMs("cards.relight", this);
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
    // `card` is the card's place in the instance's LAYOUT (PHOTON-7 stage 5, 5-2: an instance holds the faces its
    // readers read, so its resident block is a subset); false when that card is not resident.
    for (unsigned k = 0; k < inst.cardCount; ++k)
        if (mCards[inst.firstCard + k].index == card) return sampleCard(mCards[inst.firstCard + k], u, v, out);
    return false;
}

bool SurfaceCache::readRadiance(std::vector<CardRadiance> &out, unsigned long long &cacheFrame) const {
    out.clear();
    cacheFrame = mFrame;
    if (!mBuilt || !mRadiance) return false;
    Ogre::RenderSystem *rs = Ogre::Root::getSingleton().getRenderSystem();
    rs->flushCommands();   // the relights that wrote these texels are in the open command buffer
    Ogre::TextureGpuManager *tm = rs->getTextureGpuManager();
    const Ogre::PixelFormatGpu fmt = mRadiance->getPixelFormat();
    Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(kCardAtlasSize, kCardAtlasSize, 1u,
                                                                Ogre::TextureTypes::Type2D, fmt);
    tk->download(mRadiance, 0, true, nullptr, true);
    const Ogre::TextureBox box = tk->map(0);
    for (const InstanceRec &inst : mInstances) {
        for (unsigned k = 0; k < inst.cardCount; ++k) {
            const CardRec &c = mCards[inst.firstCard + k];
            CardRadiance r;
            r.node = inst.node;
            r.card = c.index;
            r.bucket = c.bucket;
            r.lastRead = c.lastRead;
            r.size = c.size;
            r.lastRelit = c.lastRelit;
            r.lastIndirect = c.lastIndirect;
            r.rgb.resize(size_t(c.size) * c.size * 3u);
            for (unsigned y = 0; y < c.size; ++y)
                for (unsigned x = 0; x < c.size; ++x) {
                    float v[4];
                    decodeTexel(fmt, box.at(c.atlasX + x, c.atlasY + y, 0), v);
                    float *o = &r.rgb[(size_t(y) * c.size + x) * 3u];
                    o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
                }
            out.push_back(std::move(r));
        }
    }
    tk->unmap();
    tm->destroyAsyncTextureTicket(tk);
    return true;
}

bool SurfaceCache::readFaceDemand(std::vector<CardFaceDemand> &out, unsigned long long &cacheFrame) const {
    out.clear();
    cacheFrame = mFrame;
    if (!mBuilt) return false;
    for (size_t s = 0; s < mSlotNode.size(); ++s) {
        if (!mSlotNode[s]) continue;
        auto it = mByNode.find(mSlotNode[s]);
        const InstanceRec *inst = it != mByNode.end() ? &mInstances[it->second] : nullptr;
        for (unsigned b = 0; b < 6u; ++b) {
            CardFaceDemand d;
            d.node = mSlotNode[s];
            d.bucket = b;
            for (unsigned r = 0; r < kDemandReaders; ++r) {
                const size_t k = s * kFaceHitStride + 8u * r + b, m = s * kFaceMarkStride + 8u * r + b;
                d.hits[r] = k < mFaceHits.size() ? mFaceHits[k] : 0ull;
                d.mark[r] = m < mFaceMarks.size() ? mFaceMarks[m] : 0ull;
            }
            if (inst)
                for (unsigned c = 0; c < inst->cardCount; ++c) d.resident += mCards[inst->firstCard + c].bucket == b ? 1u : 0u;
            if (d.hits[0] || d.hits[1] || d.mark[0] || d.mark[1] || d.resident) out.push_back(d);
        }
    }
    return true;
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
    // ...and THE FLAT TERM (ENGINE-POLISH-PHOTON-1 stage 3).
    float flat[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    if (mFlat) {
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            1u, 1u, 1u, Ogre::TextureTypes::Type2D, mFlat->getPixelFormat());
        Ogre::TextureBox src = mFlat->getEmptyBox(0);
        src.x = tx; src.y = ty; src.width = 1u; src.height = 1u;
        tk->download(mFlat, 0, true, &src, true);
        const Ogre::TextureBox box = tk->map(0);
        decodeTexel(mFlat->getPixelFormat(), box.at(0, 0, 0), flat);
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
    }
    // THE LAMPS' TRACED BITS (PHOTON-7 stage 5, 5-3; 10-4 R9 (3)): the SunVis texel's 48-bit mask (g, b, a: 16 bits
    // each) and the fractional byte (r's low byte: jah_card_sunvis.glsl), and the card's lamp table they index.
    if (mSunVis) {
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            1u, 1u, 1u, Ogre::TextureTypes::Type2D, mSunVis->getPixelFormat());
        Ogre::TextureBox src = mSunVis->getEmptyBox(0);
        src.x = tx; src.y = ty; src.width = 1u; src.height = 1u;
        tk->download(mSunVis, 0, true, &src, true);
        const Ogre::TextureBox box = tk->map(0);
        Ogre::uint32 sv[4] = { 0u, 0u, 0u, 0u };
        std::memcpy(sv, box.at(0, 0, 0), sizeof(sv));   // RGBA32_UINT, raw (jah_card_sunvis.glsl's layout)
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
        out.lampMask = (unsigned long long)sv[1] | ((unsigned long long)(sv[2] & 0xFFFFu) << 32u);
        out.lampOverflowVis = float(sv[0] & 0xFFu) / 255.0f;
        out.lampThroughMask = (unsigned long long)((sv[2] >> 16u) | (sv[3] << 16u)) |
                              ((unsigned long long)(sv[3] >> 16u) << 32u);
        out.lampThroughVis = float((sv[0] >> 16u) & 0xFFu) / 255.0f;
    }
    out.lampCount = rec.lampCount;
    static_assert(sizeof(out.lampIds) / sizeof(out.lampIds[0]) == kCardLampTable, "the sample carries the whole table");
    for (unsigned k = 0; k < kCardLampTable; ++k) out.lampIds[k] = rec.lampIds[k];
    out.gatherSky = rec.gatherSky;
    // ...and THE GATHER LAYER (PHOTON-7 stage 10, 10-2a): the TLAS gather's irradiance and the texel's escaped share.
    if (mGather) {
        Ogre::AsyncTextureTicket *tk = tm->createAsyncTextureTicket(
            1u, 1u, 1u, Ogre::TextureTypes::Type2D, mGather->getPixelFormat());
        Ogre::TextureBox src = mGather->getEmptyBox(0);
        src.x = tx; src.y = ty; src.width = 1u; src.height = 1u;
        tk->download(mGather, 0, true, &src, true);
        const Ogre::TextureBox box = tk->map(0);
        decodeTexel(mGather->getPixelFormat(), box.at(0, 0, 0), out.gather);
        tk->unmap();
        tm->destroyAsyncTextureTicket(tk);
    }
    // THE INDIRECT LAYER (ENGINE-POLISH-PHOTON-1 stage 3): the diffuse half where the channel has a
    // diffuse albedo, the specular lobe's share where it has none (JahCardLight_cs.glsl).
    const float *spec = vals[unsigned(CardLayer::Specular)];
    for (int k = 0; k < 3; ++k) {
        out.radiance[k] = rad[k];
        out.indirectStored[k] = ind[k];
        out.indirect[k] = vals[unsigned(CardLayer::Albedo)][k] > 0.0f ? ind[k] : 0.0f;
        out.flat[k] = flat[k];
        out.specularF0[k] = 0.04f * spec[3] + 0.96f * spec[k];
        out.albedo[k] = vals[unsigned(CardLayer::Albedo)][k];
        // The normal is stored *0.5+0.5, as the prepass writes it.
        out.normal[k] = vals[unsigned(CardLayer::Normal)][k] * 2.0f - 1.0f;
        out.emissive[k] = vals[unsigned(CardLayer::Emissive)][k];
    }
    out.specularWeight = spec[3];
    out.depth = vals[unsigned(CardLayer::Depth)][0];
    out.shadow = vals[unsigned(CardLayer::ShadowRough)][0];
    out.roughness = vals[unsigned(CardLayer::ShadowRough)][1];
    out.texelX = tx;
    out.texelY = ty;
    out.lit = rec.indirectValid;
    // THE TEXEL'S SURFACE, as the sun trace reconstructs it (rq_card_movers.comp,
    // float for float): the capture camera, the ortho window, the depth back along
    // the card's axis; the normal from the card's view space.
    {
        const Ogre::Vector3 cam = rec.centre + rec.d * (rec.halfDepth + captureMargin(rec.halfDepth));
        const float fx = (float(tx - rec.atlasX) + 0.5f) / float(rec.size);
        const float fy = (float(ty - rec.atlasY) + 0.5f) / float(rec.size);
        const Ogre::Vector3 P = cam + rec.u * (std::max(2.0f * rec.halfU, 1e-4f) * (fx - 0.5f)) +
                                rec.v * (std::max(2.0f * rec.halfV, 1e-4f) * (0.5f - fy)) - rec.d * out.depth;
        Ogre::Vector3 N = rec.u * out.normal[0] + rec.v * out.normal[1] + rec.d * out.normal[2];
        if (N.squaredLength() > 1e-12f) N.normalise();
        for (int k = 0; k < 3; ++k) {
            out.position[k] = P[k];
            out.worldNormal[k] = N[k];
        }
        out.sunLift = rec.sunLift;
    }
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
    // The six captured layers and the radiance.
    const char *name[kCardLayers + 1] = { "albedo", "normal", "depth", "emissive", "shadowrough",
                                          "specular", "radiance" };
    static_assert(kCardLayers == 6u, "a layer here needs its name and scale");
    // `scale` turns a layer into something an eye can read: the depth is metres
    // and the emissive and the radiance are radiance, so they are divided by a
    // stated number rather than clipped silently.
    const float scale[kCardLayers + 1] = { 1.0f, 1.0f, 0.2f, 0.25f, 1.0f, 1.0f, 0.25f };
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
void surfaceCacheAsyncDraws(const Ogre::SceneManager *sm, unsigned long long &placeholders,
                            unsigned long long &skips, bool forget) {
    const auto it = gCaptureAsyncDraws.find(sm);
    placeholders = it != gCaptureAsyncDraws.end() ? it->second.placeholders : 0u;
    skips = it != gCaptureAsyncDraws.end() ? it->second.skips : 0u;
    if (forget && it != gCaptureAsyncDraws.end()) gCaptureAsyncDraws.erase(it);
}

Ogre::CompositorWorkspace *surfaceCacheWorkspace(const SurfaceCache *cache) {
    return cache ? cache->workspace() : nullptr;
}

}   // namespace detail

}   // namespace engine
}   // namespace jahshaka
