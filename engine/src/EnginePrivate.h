#pragma once
// Internal header of the Ogre-Next 4.0 backend.
//
// THE ONE-DIRECTORY RULE: engine/src/ is the only directory that includes Ogre.
// This header is private to it — it is never installed and must never be included
// from outside irisgl/engine/src. If Ogre appears anywhere else, the boundary has
// been breached.
//
// Verified behaviours this backend depends on (spikes/qt-ogre-next/FINDINGS.md and
// spikes/headless-vulkan/README.md):
//   * Vulkan supports MULTIPLE on-screen windows; GL3Plus does not (single mGlobalVao).
//   * v1 meshes render NOTHING on Vulkan — geometry is built as v2 buffers directly.
//   * Hlms shader templates are required at runtime, not optional sample data.
//   * A render target must exist BEFORE Hlms registration or any SceneManager;
//     for a purely offscreen engine a surfaceless "null" window satisfies this.
//   * Teardown order is load-bearing: workspaces -> scenes -> our MeshPtrs ->
//     MeshManager::removeAll -> Root. A MeshPtr outliving Root hits a dead VaoManager.
//   * No Ogre exception may escape: every virtual is wrapped and translated.
#include "jahshaka/engine/Engine.h"

#include <OgreRoot.h>
#include <OgreAbiUtils.h>
#include <OgreWindow.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
#include <OgreItem.h>
#include <OgreMesh2.h>
#include <OgreMeshManager2.h>
#include <OgreSubMesh2.h>
#include <OgreArchiveManager.h>
#include <OgreHlmsJson.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsListener.h>
#include <OgreHlmsPbs.h>
#include <OgreHlmsUnlit.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreHlmsUnlitDatablock.h>
#include <OgreTextureGpuManager.h>
#include <OgreTextureFilters.h>
#include <OgreStagingTexture.h>
#include <OgrePixelFormatGpuUtils.h>
#include <OgreImage2.h>
#include <OgreTextureGpu.h>
#include <OgreAsyncTextureTicket.h>
#include <OgreLogManager.h>
#include <OgreResourceGroupManager.h>
// v1 skeletons are a BUILD-TIME scaffold only (SkeletonDef has exactly one
// constructor and it takes a v1::Skeleton) — the prerequisites header is what
// lets this file name v1::SkeletonPtr / v1::OldBone without pulling v1 in.
#include <OgreControllerManager.h>
#include <OgrePrerequisites.h>
#include <OgreHlmsSamplerblock.h>
#include <OgreRectangle2D2.h>
#include <OgreVertexFormatWarmUp.h>
#include <set>
#include <unordered_map>
#include <Compositor/OgreCompositorManager2.h>
#include <Compositor/OgreCompositorWorkspace.h>
#include <Compositor/OgreCompositorNodeDef.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Compositor/OgreCompositorShadowNodeDef.h>
#include <OgreRenderSystemCapabilities.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreVertexArrayObject.h>
#include <ParticleSystem/OgreBillboardSet2.h>
#include <ParticleSystem/OgreParticleSystemManager2.h>
#include <ParticleSystem/OgreParticleSystem2.h>
#include <ParticleSystem/OgreEmitter2.h>
#include <ParticleSystem/OgreParticleAffector2.h>
#include <OgreParticleEmitter.h>
#include <OgreForwardPlusBase.h>
#include <OgreDecal.h>
#include <InstantRadiosity/OgreInstantRadiosity.h>
#include <Vct/OgreVctVoxelizer.h>
#include <Vct/OgreVctLighting.h>
#include <IrradianceField/OgreIrradianceField.h>
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <Cubemaps/OgrePccPerPixelGridPlacement.h>
// Fog rides Ogre's Atmosphere component: we take its exponential fog + brightness
// breakthrough and leave its sky and its sun/ambient coupling alone (OgreFog.cpp).
#include <Atmosphere/OgreAtmosphereNpr.h>
#include <OgrePlanarReflections.h>
#include <Compositor/OgreCompositorWorkspaceListener.h>

// NO <X11/Xlib.h> HERE, deliberately. The only X11 thing this header ever
// needed was the {Display*, Window} pair Ogre's Vulkan/XCB backend consumes as
// its "SDL2x11" misc param, and that pair is layout-compatible with
// {void*, unsigned long} (X11's `Window` is an `XID`, i.e. `unsigned long`, on
// every platform that has an Xlib) — see OgreVulkanXcbWindow::_initialize,
// which reinterpret_casts the pointer to its own local struct. Including Xlib
// here cost more than it bought: it drags ~40 macros (`None`, `Status`,
// `Bool`, `Success`) into every Ogre-private TU of the engine, and it is dead
// weight on any arm that has no X server at all (Windows).
#include <atomic>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// The two compositor definition types this header names but does not use: the
// PiP's scene pass, whose viewport rectangle the view rewrites live, and the
// clear pass that owns the inset's background colour (chain::PipHandles).
// Forward-declared rather than included so the pass-def headers stay where they
// belong — inside the .cpp files that build passes.
namespace Ogre { class CompositorPassSceneDef; class CompositorPassClearDef;
                 class CompositorPassQuadDef; class CompositorPassDef;
                 // Bone attachments (AVATAR_RIG_PERF_SPEC §4): a Node record
                 // holds a TagPoint*, and only OgreSockets.cpp does anything
                 // with one.
                 class TagPoint; }

namespace jahshaka { namespace engine {
// The backend's own namespace: these types and helpers are shared between the
// TUs under engine/src and by nothing else (they used to live in one anonymous
// namespace, when the backend was a single translation unit).
namespace detail {

inline Ogre::Vector3     toOgre(const Vec3 &v)   { return Ogre::Vector3(v.x, v.y, v.z); }
inline Ogre::ColourValue toOgre(const Colour &c) { return Ogre::ColourValue(c.r, c.g, c.b, c.a); }

/// Names handed to Ogre must be unique for the life of the process (a destroyed
/// scene may be recreated under the same name while stale resources linger).
inline std::string processUniqueName(const char *prefix) {
    static std::atomic<unsigned> counter{0};
    return std::string(prefix) + "_" + std::to_string(++counter);
}

// Every backend virtual is wrapped: `JAH_TRY { ... } JAH_CATCH(errSink, failValue)`.
// Ogre throws Ogre::Exception; its own allocations may throw std::bad_alloc.
#define JAH_TRY try
#define JAH_CATCH(sink, ret)                                                          \
    catch (Ogre::Exception &e) { (sink) = e.getFullDescription(); return ret; }       \
    catch (std::exception &e)  { (sink) = std::string("engine: ") + e.what(); return ret; }

class OgreEngine;

// ---------------------------------------------------------------------------
// Visibility-flag bits (user bits; Ogre reserves the top two for layer state).
// Every object keeps kVisibleBit so default cameras/compositor masks (all ones)
// draw it. kGiGeometryBit marks items whose surfaces bounce light for GI: PBR
// items only — never the sky, unlit overlays, line meshes or billboards, which
// would otherwise occlude rays or raycast as garbage triangles (a non-indexed
// line VAO reads as a vertex triangle list). kGiLightBit marks exactly the one
// light Instant Radiosity traces from (InstantRadiosity::mLightMask).
//
// AN EXCLUDE BIT IS IMPOSSIBLE; AN INCLUDE CHANNEL IS NOT. Ogre's visibility
// test is ANY-BIT-SET (`objFlags & visibilityMask`,
// MovableObject::_addToRenderQueue), so a mask can only say "draw things
// carrying one of THESE bits" — never "skip things carrying THIS bit". A
// `~kNoReflectBit` pass mask was carried here until 2026-09-06 and was a total
// no-op (probe-proven: flagging a plate visible in a reflection moved zero
// pixels). What keeps a mirror out of its OWN reflection is upstream's, not
// ours — see OgrePlanar.cpp's header.
//
// The way to get "not in this pass" out of an any-bit test is INVERSION, and
// that is what kHelperBit is (REFLECTIONS_ADOPTION_SPEC.md P1b). Editor helpers
// — the ground grid, light icons, range wires, camera helpers — carry
// kHelperBit *INSTEAD OF* kVisibleBit. Then:
//   * the MAIN chain sets no explicit visibility_mask anywhere (verified across
//     OgreChain.cpp and every compositor we ship), so its all-ones default plus
//     the any-bit test keeps drawing them exactly as before — nothing to widen;
//   * the reflection-probe face pass in JahshakaPcc.compositor carries
//     `visibility_mask 0x1`, i.e. kVisibleBit only, so helpers drop out of
//     every probe capture. Real geometry all carries kVisibleBit, so nothing
//     else changes.
// PCC forcing the SceneManager's mask wide does not defeat this: the pass mask
// governs the user bits through `_getCombinedVisibilityMask`
// (OgreSceneManager.cpp) and CompositorPassScene applies it per pass.
//
// THE RULE THAT COMES WITH IT: any future pass that sets an explicit
// visibility_mask must include kHelperBit, or helpers vanish from that pass.
// The planar-reflection path is the next place that wants this channel
// (helpers out of mirrors); the bit is ready, the mask is not wired there yet.
constexpr Ogre::uint32 kVisibleBit     = 1u;
constexpr Ogre::uint32 kGiGeometryBit  = 1u << 1;
constexpr Ogre::uint32 kGiLightBit     = 1u << 2;
constexpr Ogre::uint32 kHelperBit      = 1u << 3;
// THE DISTORTION CHANNEL (POST_LOOKS_SPEC.md §5.3). A distortion item carries
// this bit *INSTEAD OF* kVisibleBit — the same inversion trick kHelperBit uses,
// and for a stronger reason: a distortion object must be drawn by EXACTLY ONE
// pass in the whole engine (the distortion pass), and by nothing else, ever.
// Its texture is a screen-space displacement field; drawn into a colour target
// it is a smear of pale blue, drawn into a G-buffer it corrupts the normals of
// whatever is behind it, and drawn into a thumbnail it is simply wrong.
//
// Carrying its own bit gets that for free everywhere a mask already exists (the
// probe-face pass, the planar pass and the shadow node all test kVisibleBit),
// and the two OVERLAY passes — the only passes whose render-queue range
// contains 220 — mask it out explicitly. Every other pass in every workspace
// excludes RQ 220 by RANGE, so the PASSTHROUGH shape never draws one at all and
// thumbnails, previews and the pixel suites stay byte-identical without knowing
// the feature exists.
constexpr Ogre::uint32 kDistortionBit  = 1u << 4;

// ---------------------------------------------------------------------------
// THE SHADOW ATLAS (SPECS/SHADOW_TOOLING_SPEC.md; built in OgreShadow.cpp)
// ---------------------------------------------------------------------------
// The values the shadow-node definition is built from. They were arguments to
// ShadowNodeHelper::createShadowNodeWithSettings until the definition became
// ours; keeping them named (and here, beside the visibility bits the atlas also
// depends on) is what makes a diff against upstream's helper readable.
//
// numStableSplits = 2 (upstream defaults to 0): the two near PSSM splits — the
// ones a user is looking at — stop re-quantising every time a caster enters or
// leaves the view, which is what makes shadow edges stop crawling.
constexpr Ogre::uint32 kPointLightCubemapResolution = 1024u;
constexpr float        kShadowXyPadding   = 1.5f;      ///< upstream's default
constexpr float        kPssmLambda        = 0.95f;
constexpr float        kPssmSplitPadding  = 1.0f;
constexpr float        kPssmSplitBlend    = 0.125f;
constexpr float        kPssmSplitFade     = 0.313f;
constexpr Ogre::uint32 kPssmStableSplits  = 2u;
/// The engine's hard ceiling on focused (point/spot) shadow maps, whatever a
/// host asks for (SHADOW_TOOLING_SPEC D1). The bound is VRAM and shadow passes,
/// not the pass buffer — a mapped caster costs ~112 B there.
constexpr unsigned     kMaxShadowMaps     = 16u;
/// The PROBE-CAPTURE shadow node's knobs (the third node, OgreView::
/// kProbeShadowNodeName — the numbers behind them are at its declaration).
/// Resolution: a quarter of the main atlas, floored at 256 (the engine's own
/// floor) and capped at 512 = the largest reflection-probe face (OgreScene::
/// buildPcc's quality table; a raster irradiance-field face is 32 px). A probe
/// face is consumed through the IBL roughness mip chain, so a shadow edge
/// sharper than the face itself is unobservable by construction — measured on
/// the Showroom sample's reflections: 2048 and 1024 PSSM-only captures are
/// byte-identical, and 512 differs from 1024 in well under 1% of pixels (the
/// lamp maps, at R, are where the last halving shows). Focused maps: the
/// derived count the main atlas has (so the lamps a probe sees are the lamps
/// the view sees), capped at four — each map is one R x R rectangle per probe.
/// Scratch cube: R / 2, the same ratio the main node has always used
/// (2048 -> 1024).
constexpr unsigned     kProbeShadowMaxResolution  = 512u;
constexpr unsigned     kProbeShadowMaxFocusedMaps = 4u;
inline unsigned probeShadowResolution(unsigned mainResolution) {
    return std::min(kProbeShadowMaxResolution, std::max(256u, mainResolution / 4u));
}

/// THE THREE SHADOW-NODE KINDS (OgreView::kShadowNodeName and its two twins).
/// A CompositorShadowNode is per WORKSPACE, so the lamp-map cache
/// (ENGINE_CACHE_POLICY_SPEC P2-P5, OgreEngine::applyShadowCache) is applied
/// per instance and counted per kind: the views' nodes, each planar budget
/// slot's reflect node, and each shadowed reflection probe's probe node.
enum class ShadowNodeKind : unsigned { View = 0u, Reflect = 1u, Probe = 2u };
constexpr unsigned kShadowNodeKinds = 3u;
/// THE CASTER SET, per kind: the render channels a shadow map of `kind` draws.
/// ONE definition, read by the node's passes (buildShadowNode's
/// mVisibilityMask) and by the lamp-map cache's caster scan (a changed box
/// dirties a kind's maps only through these channels). Today every kind draws
/// kVisibleBit. REALTIME_REFLECTIONS_SPEC R1/R2 widen View and Reflect to
/// kVisibleBit | kMovableBit and keep Probe at kVisibleBit — and then a
/// mover dirties view/reflect maps only, never probe maps (its §5.5), with no
/// other change here. Never hard-code kVisibleBit as "the casters" elsewhere.
inline Ogre::uint32 shadowCasterChannels(ShadowNodeKind) { return kVisibleBit; }
inline Ogre::uint32 allShadowCasterChannels() {
    return shadowCasterChannels(ShadowNodeKind::View) |
           shadowCasterChannels(ShadowNodeKind::Reflect) |
           shadowCasterChannels(ShadowNodeKind::Probe);
}
/// THE LAMPS a kind's instances may hold fixed (cached). Today: every
/// cacheable lamp. R2's movable lights are outside the probe captures'
/// `light_visibility_mask`, and a light FIXED to a map bypasses that mask (Ogre
/// writes fixed lights into the pass buffer whatever the pass's light mask),
/// so R2 filters them here for ShadowNodeKind::Probe.
inline bool shadowLampCachedFor(ShadowNodeKind, const Ogre::Light *) { return true; }
/// THE NEAR PLANE OF EVERY POINT/SPOT SHADOW CAMERA. Unset, Ogre falls back to
/// the VIEWER's near clip (Light::_deriveShadowNearClipDistance), so the same
/// lamp's depth range would follow whichever camera rendered it — fatal for a
/// map rendered once and sampled by every camera after (ENGINE_CACHE_POLICY
/// P4, risk 5). 0.1 is CameraDesc::nearClip's default (and the probe cameras'
/// own floor order of magnitude), so a view at the default renders the exact
/// maps it always rendered. Directional lights keep the camera's (PSSM).
constexpr float kShadowLampNearClip = 0.1f;

/// One shadow map's rectangle inside the atlas, in texels.
struct ShadowMapRect { unsigned x = 0, y = 0, w = 0, h = 0; };

/// WHERE EVERY SHADOW MAP SITS. Built by planShadowAtlas() and consumed by
/// OgreEngine::buildShadowNode; also what `world.shadowStatus()` reports.
struct ShadowAtlasPlan {
    unsigned width = 0, height = 0;      ///< the atlas texture, in texels
    unsigned resolution = 0;             ///< R: the base size the plan derives from
    unsigned focusedMaps = 0;            ///< focused maps actually placed
    ShadowMapRect pssm[3];               ///< split 0 at R x R, splits 1-2 at R/2
    std::vector<ShadowMapRect> focused;  ///< R x R each, column-packed
    /// D32 depth, so four bytes per texel. The cube scratch (1024^2 x 6 R32F +
    /// its depth, ~48 MB) is NOT counted here: it is allocated once for any
    /// point caster and does not grow with the map count.
    unsigned long long bytes() const {
        return 4ull * (unsigned long long)width * (unsigned long long)height;
    }
};

/// Packs `focusedMaps` R x R maps plus the 1.5R PSSM header into the smallest
/// atlas that respects `maxDim` on both axes. Pure arithmetic — no Ogre state —
/// so the layout can be unit-tested without a device (OgreShadow.cpp).
ShadowAtlasPlan planShadowAtlas(unsigned baseResolution, unsigned focusedMaps, unsigned maxDim);

// Forward+ clustered decal budget PER CELL (DECALS_SPEC D5). Not a scene-wide
// cap: decals beyond this in one cluster cell are dropped farthest-first.
constexpr Ogre::uint32 kDecalsPerCell = 8u;

// FORWARD+ CUBEMAP-PROBE SLOTS PER CELL — the budget a scene starts with, and
// the ceiling a probe grid may raise it to.
//
// THE DEFECT THIS EXISTS FOR (owner report 2026-09-07: "hard-edged black
// rectangles crawling over the Showroom's metals"). Per-pixel PCC is culled
// through the Forward+ cluster grid, and
// `ForwardClustered::collectObjsForSlice` writes a probe into a cell only while
// `numLightsInCell->objCount[objType] < currObjsPerCell`
// (OgreForwardClustered.cpp:291-298) — past the budget the probe is SILENTLY
// DROPPED, per cell, in scene order. A cluster cell is a frustum chunk spanning
// a whole logarithmic depth slice, so it is large in world space; the shipped
// Grand Showroom's 4x2x4 grid gives every probe an influence area of about
// 7.3 x 4.1 x 7.3 units at a 5.9-unit spacing, and one far cell routinely
// intersects a dozen of them. The pixels whose own probe was dropped fall
// through to VCT cone tracing, which inside a sealed room is black — hence
// hard-edged, SCREEN-AXIS-ALIGNED rectangles that move with the camera and
// cannot be tuned away by any GI knob.
//
// MEASURED, on the shipped Grand Showroom at the rig's camera
// (position 1.5, 5, -11), fraction of hard-black pixels in two fixed boxes on
// the chrome spheres, everything else identical:
//     8 slots per cell   0.742 / 0.298      <- the owner's black rectangles
//     32 slots per cell  0.000 / 0.000      <- gone, at every camera tested
//
// So the budget is DERIVED FROM THE GRID (OgreScene::ensureCubemapProbeSlots):
// a cell can never intersect more probes than exist, so a budget at least the
// probe count cannot drop one. It is quantised (8/16/32/64) and only ever grows
// within a scene's life, because changing it moves the Forward+ hlms property
// offsets and recompiles those shaders once — a scene that re-arms the same
// grid must not pay that twice.
//
// COST, stated: the grid buffer is cells x objsPerCell x uint16 and is uploaded
// every frame. At 16x8x24 = 3072 cells, each extra slot is +6 KiB per frame, so
// 8 -> 32 is +144 KiB and the 64 ceiling is +336 KiB. Runtime cost is nil: the
// shader loops over the probes a cell ACTUALLY holds, not over the budget.
constexpr Ogre::uint32 kCubemapProbeSlotsDefault = 8u;
constexpr Ogre::uint32 kCubemapProbeSlotsMax     = 64u;

// ---------------------------------------------------------------------------
// Particles (PARTICLES_FX2_SPEC.md). The HARD per-definition quota ceiling for
// every scene: setHighestPossibleQuota is called with this at scene creation,
// before any definition or billboard set can initialise, and setParticleSystem
// clamps every request to it. The backend's own limit is 65535/4 = 16383
// (setHighestPossibleQuota, OgreParticleSystemManager2.cpp:812-817); 16000
// stays under it with room and costs 768 KiB of shared index buffer per scene,
// allocated lazily on the first particle draw.
constexpr Ogre::uint16 kMaxParticleQuota = 16000u;
// Quota BUCKETS. A definition's quota is frozen at init() — changing it needs a
// whole new definition, and definitions can never be destroyed — so requests
// round UP to one of these. Two emitters whose quotas land in the same bucket
// can trade definitions through the recycling pool.
constexpr unsigned kParticleQuotaBuckets[] = { 256u, 1024u, 4096u, 16000u };

// ---------------------------------------------------------------------------
// Render-queue policy (POST_CHAIN_SPEC.md §6). Ogre fixes the queue MODES in
// RenderQueue's constructor: [0,100) and [200,225) are v2 FAST, [100,200) and
// [225,256) are V1_FAST, 15 is PARTICLE_SYSTEM. Our v2 items can therefore only
// live in 0-99 and 200-224.
//   0     sky rectangle          (OgreSky)
//   10    normal items           (Ogre's default)
//   15    PFX2 billboards
//   200   refractive items       (reserved; phase 7)
//   210   on-top overlays        (gizmos, wires, selection outlines)
// The compositor chain renders [0, kRefractiveRenderQueue) in the opaque pass
// and [kOverlayRenderQueue, 255) in a second pass, so overlays stay out of the
// G-buffers, luminance averages and edge-detection passes the later phases add.
// (Overlays used to sit at 200; the move is pixel-neutral — both values are
// inside the single range the old one-pass workspace drew.)
constexpr Ogre::uint8 kRefractiveRenderQueue = 200;
constexpr Ogre::uint8 kOverlayRenderQueue    = 210;

// THE DISTORTION QUEUE (POST_LOOKS_SPEC.md §5.3).
//
// Upstream's Distortion sample files these objects at RQ 16, which cannot work
// here: every scene pass in this engine draws a CONTIGUOUS range starting at 0
// — the SSR prepass, the opaque pass, the refractive pass, the probe-face
// workspace — so a queue in the middle of it would be drawn by all of them, and
// an HlmsUnlit displacement map written into the SSR prepass's normals G-buffer
// is a defect with no error message.
//
// 220 is above every one of those ranges by construction and still inside
// [200,225), the only other v2 FAST range (RenderQueue's constructor). The one
// pass whose range contains it is the overlay pass [210,255], which cannot
// shrink — Ogre's v1 overlay hook fires at RQ 254 and our HUD and loading cover
// ride it — so the overlay passes carry an explicit visibility mask instead
// (kDistortionBit above).
constexpr Ogre::uint8 kDistortionRenderQueue = 220;
/// DISTORTION PARTICLES (POST_LOOKS 4b): a PFX2 def can only be drawn from a
/// PARTICLE_SYSTEM-mode queue, and 220 holds ITEMS, so distortion emitters get
/// the next queue up, armed in particle mode beside the helper queue
/// (OgreScene::ensureHelperOverlayQueue). The distortion pass draws
/// [kDistortionRenderQueue, kDistortionParticleRenderQueue] — two queues, one
/// field — and its kDistortionBit mask keeps everything else out. The queue
/// depth anchor sits here too (kQueueDepthAnchorRenderQueue): one anchor serves
/// 211 and 221, never two.
constexpr Ogre::uint8 kDistortionParticleRenderQueue = 221;

// ---------------------------------------------------------------------------
// THE HELPER OVERLAY QUEUE (2026-09-08, the grey/blurred light icons).
//
// A BillboardSet2 draws ONLY from a queue whose mode is PARTICLE_SYSTEM, and
// upstream sets exactly one: 15 (RenderQueue's constructor,
// kParticleSystemDefaultRenderQueueId). 15 is inside the OPAQUE pass, so a
// light icon was tonemapped, bloomed, ambient-occluded and edge-detected along
// with the scene — white read back as ~24% grey with a smeared glyph, which is
// the defect. Every other helper (wires, gizmo, grid) already avoids that by
// living at kOverlayRenderQueue, which the chain draws AFTER the post chain.
//
// So the engine declares a SECOND particle-system queue, inside the overlay
// pass's range: 211. The mode is per SceneManager (RenderQueue::
// setRenderQueueMode), so it costs nothing until a scene asks for it —
// OgreScene::ensureHelperOverlayQueue, called by the first Overlay-layer
// billboard set. Real emitters stay at 15: they are scene content and MUST be
// graded with the scene.
//
// AND THE TRAP THAT COMES WITH IT — kQueueDepthAnchorRenderQueue below.
constexpr Ogre::uint8 kHelperOverlayRenderQueue = 211;

// WHY AN INVISIBLE OBJECT HAS TO EXIST AT 212.
//
// SceneManager::cullFrustum runs the particle-system branch inside a loop whose
// bounds are clamped, per ENTITY memory manager, to that manager's used depth:
//     firstRq = min(pass.firstRq, memoryManager->getNumRenderQueues())
//     lastRq  = min(pass.lastRq,  memoryManager->getNumRenderQueues())
// and getNumRenderQueues() is "highest render queue holding an ENTITY, plus
// one" (OgreObjectMemoryManager.cpp:197). Billboard sets do NOT count: they
// live in the particle-system-def memory manager, which is not in
// mEntitiesMemoryManagerCulledList at all. So a particle queue is visited only
// while some ITEM sits at or above it.
//
// This is why RQ 15 has always worked without anyone noticing the rule: every
// Camera is an entity at RQ 110 (OgreFrustum.cpp:51), so the depth is >= 111 in
// any scene that renders at all. Nothing of ours lives above the overlay queue
// (210), so a helper queue at 211 would be silently skipped — icons would
// simply never draw, in exactly the scenes with no gizmo on screen.
//
// The anchor is one MovableObject with no renderables and visibility flags 0,
// created once per scene beside the helper queue. It is never drawn in any
// pass, it holds the entity depth at 222, and it costs one SIMD cull slot.
// (The alternative was an Ogre patch to make the particle branch independent of
// the entity managers — the honest upstream fix, and recorded as a finding —
// but it changes cull code every pass runs, for a defect a 20-line object
// closes here.)
//
// ONE ANCHOR, AT THE TOPMOST PARTICLE QUEUE. It sat at 212 while 211 was the
// only particle queue above the overlay; the distortion-particle queue at 221
// (POST_LOOKS 4b) moved it to 221 — the clamp is "highest entity queue plus
// one", so an anchor at 221 covers 211 and 221 alike. Never add a second
// anchor: the cull loop visits a particle queue once per entity memory manager
// deep enough to reach it, and two anchors in two managers would draw every
// helper set twice (the spike measured 0 px of difference between the anchor at
// 221 and a second one, and a second one is exactly the double-draw trap).
constexpr Ogre::uint8 kQueueDepthAnchorRenderQueue = kDistortionParticleRenderQueue;

/// How many entries PbrTextureSlot has. The enum carries its own `Count`
/// sentinel since the detail slots landed (MATERIAL_GAPS_SPEC GAP 2); this name
/// stays because MaterialRec and every loop over the slots use it.
constexpr size_t kPbrTextureSlotCount = size_t(PbrTextureSlot::Count);

// ---------------------------------------------------------------------------
// What shape of compositor chain a view wants. Phase 1 carries only what every
// view has always had; the effect switches (HDR + tonemap, bloom, SSAO, SMAA,
// SSR, refraction) become extra fields here and extra nodes in OgreChain.cpp,
// and nothing outside those two places has to learn about them.
struct ChainDesc {
    Colour   background;
    bool     shadows = false;   ///< instantiate the process-wide shadow node
    unsigned samples = 1u;      ///< achieved MSAA count of the view's target

    // ---- Effects (phases 3-7). Every one of them is OFF on offscreen views by
    //      construction: OgreView::chainDesc() clears them (POST_CHAIN_SPEC §7.3).
    bool  hdr = false;              ///< RGBA16F scene target + filmic tonemap
    /// The DETERMINISTIC form of `hdr` (PostFxDesc::tonemapFixed): same node,
    /// same curve, but the auto-exposure reduction is replaced by a constant.
    /// A GRAPH change — five quads and four textures fewer — so it is part of
    /// sameShape().
    bool  tonemapFixed = false;
    float exposure = 0.0f;          ///< stops; the auto-exposure midpoint
    float exposureMin = -2.5f;
    float exposureMax = 2.5f;
    bool  bloom = false;            ///< rides the HDR node at ~zero marginal cost
    float bloomThreshold = 5.0f;    ///< bright-pass start, in the sample's units
    float bloomKnee = 2.0f;         ///< ramp WIDTH above it (A-6); 2.0 = the old hard-coded value
    bool  ssao = false;
    float ssaoScale = 1.0f;         ///< AO buffer resolution factor (0.5 or 1.0)
    float ssaoPower = 1.5f;
    float ssaoRadius = 2.0f;
    int   smaaPreset = -1;          ///< -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra
    int   ssr = 0;                  ///< 0 off, 1 half-res rays, 2 HQ
    float ssrMaxDistance = 25.0f;   ///< ray length, world units
    float ssrThickness = 0.5f;      ///< assumed surface thickness, world units
    float ssrRoughnessCutoff = 0.35f;
    float ssrIntensity = 1.0f;
    bool  refractions = false;

    // ---- Distortion (POST_LOOKS_SPEC.md §5.3) ----
    /// The RESOLVED flag (the host has already answered "auto" against whether
    /// the scene holds a distortion material). Adds one target and two passes.
    bool  distortion = false;
    /// Global multiplier on each material's own strength. A UNIFORM, not shape.
    float distortionStrength = 1.0f;

    // ---- The looks stack (POST_LOOKS_SPEC.md §4) ----
    /// The resolved stack, in frame order — PostFxDesc::looks, copied. Only the
    /// KIND SEQUENCE is graph shape (sameShape compares nothing else about it);
    /// the parameters ride applyViewGlobals like every other tuning value.
    std::vector<LookDesc> looks;

    // ---- Letterbox (CAMERAS_SPEC §7.4) ----
    /// Render the scene into an inner rectangle of the target instead of
    /// filling it, with bars in the remainder — what a camera whose
    /// CameraDesc::constrainAspect is set asks any view showing it for
    /// (the main view while PILOTING it, phase 3).
    ///
    /// Only the FLAG is a graph change. The rectangle itself is written onto
    /// the pass definitions live (ChainHandles::insetPasses), because it is
    /// derived from the target's own aspect and therefore moves on every
    /// resize — and Ogre re-reads mVpRect from the definition on every execute.
    bool  letterbox = false;

    // ---- Engine-drawn overlay (STATS_OVERLAY_SPEC.md §6.5) ----
    /// Whether the FINAL overlay pass gets mIncludeOverlays = true. This is a
    /// property of the VIEW, not of what is currently being drawn: it is true
    /// for every on-screen view and for an offscreen view that set
    /// ViewOverlayDesc::allowOffscreen, and it never changes when the host
    /// toggles the stats readout or raises the cover. That is deliberate —
    /// showing and hiding is Ogre overlay-element state, so a toggle must not
    /// rebuild a workspace (STATS_OVERLAY_SPEC §6.6 test 3). Every OTHER scene
    /// pass in this file sets mIncludeOverlays = false unconditionally.
    bool  overlays = false;

    /// Does this description need anything beyond the passthrough graph?
    bool anyEffect() const;
    /// Do these two describe the same GRAPH? Parameters (exposure, AO power)
    /// are uniforms — changing one must never rebuild a workspace.
    static bool sameShape(const ChainDesc &a, const ChainDesc &b);
};

class OgreView;

/// The chain builder (OgreChain.cpp). Not a class: it has no state — the state
/// is the ChainDesc the view owns and the definition names it hands back.
namespace chain {
/// What build() hands back so the view can move things that are NOT graph
/// changes — today exactly one thing: the letterbox rectangle
/// (CAMERAS_SPEC §7.4). Ogre re-reads a pass's mVpRect from its DEFINITION on
/// every execute (CompositorPass::setRenderPassDescToCurrent), so writing these
/// between frames is live and rebuilds nothing.
///
/// Empty unless ChainDesc::letterbox — the pointers are into definitions this
/// view owns and die with chain::destroy.
struct ChainHandles {
    /// Every pass that must be confined to the INNER rectangle: the scene
    /// passes and the quad that paints the inner background. These take the
    /// rectangle as VIEWPORT and scissor both — a scene pass projects into its
    /// viewport, so the viewport IS the shot.
    std::vector<Ogre::CompositorPassDef *> insetPasses;
    /// Every full-resolution POST quad of the letterboxed chain (SSAO apply,
    /// distortion compose, the SSR history copy, tonemap/composite, the three
    /// SMAA quads, every look). These take the rectangle as SCISSOR ONLY: the
    /// viewport stays the whole target so the quad's 0..1 UVs still map the
    /// source texture 1:1 onto the destination — an inset viewport would
    /// squeeze the full image into the rectangle — while the scissor rejects
    /// every fragment in the bars before it is shaded. Each pass also CLEARS
    /// its target (a Vulkan clear is full-attachment, whatever the scissor),
    /// so the bars of every intermediate and of the window are pure black
    /// at no fill cost. The Unreal model: post processing runs on the shot,
    /// never on the bars (riders lane R2, owner decision 2026-09-09).
    std::vector<Ogre::CompositorPassDef *> scissorPasses;
    /// The clear that fills the tiny background swatch the inner-rect quad
    /// copies. Its colour is the view's background, pushed live.
    Ogre::CompositorPassClearDef *letterboxSwatch = nullptr;
    /// The one-shot clear that SEEDS the HDR auto-exposure history
    /// (CAMERA_LENS_SPEC §4, the camera-cut hook). It carries
    /// `mNumInitialPasses = 1`, so it executes once per workspace instantiation
    /// and never again — which is the spec's unverified R4 claim, VERIFIED here
    /// in the pin (CompositorPass's ctor takes mNumPassesLeft from the
    /// definition and only resetNumPassesLeft() puts it back). Null unless the
    /// chain has the automatic HDR exposure.
    Ogre::CompositorPassClearDef *exposureSeed = nullptr;
    /// The 1x1 clear that IS the exposure in the FIXED tonemap form
    /// (ChainDesc::tonemapFixed — POST_CHAIN_SPEC §14). Null unless the chain
    /// has it. Handed back because PostFxDesc::exposure is deliberately NOT
    /// part of ChainDesc::sameShape: changing it must not rebuild a workspace,
    /// so somebody has to rewrite this clear instead — OgreView::applyFixedExposure.
    Ogre::CompositorPassClearDef *fixedExposure = nullptr;
};

/// Creates the node definitions and the workspace definition `desc` describes,
/// under `workspaceDef`. EVERY node definition created is appended to
/// `nodeDefsOut`, in creation order: a multi-node chain whose owner cleans up
/// only one definition leaks the rest across view recreation.
void build(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
           const ChainDesc &desc, std::vector<std::string> &nodeDefsOut,
           ChainHandles &handlesOut);
/// The inner rectangle for a letterboxed view: the largest `aspect`-shaped
/// rectangle centred in a target of `targetAspect`. Normalised coordinates.
void letterboxRect(float aspect, float targetAspect, float inner[4]);
/// Removes the workspace definition and every node definition in `nodeDefs`
/// (which is cleared). Safe when nothing was built.
void destroy(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
             std::vector<std::string> &nodeDefs);
/// The name of the scene node definition for a workspace — the anchor later
/// phases (and the planar-reflection lane) need to find the main scene pass.
std::string sceneNodeDefName(const std::string &workspaceDef);

// ---- The picture-in-picture inset (CAMERAS_SPEC §7.7) ----------------------
/// What buildPip hands back so the view can move the inset without rebuilding
/// anything: the rect lives on the COMPOSITE quad's own mVpRect, which Ogre
/// re-reads from the definition on every execute
/// (CompositorPass::setRenderPassDescToCurrent), so writing it between frames
/// is live. The fill quad keeps the full [0,1] rect and therefore always paints
/// the whole inset — background plus bars.
struct PipHandles {
    /// The scene pass. It targets the inset's LOCAL texture (Route C), so its
    /// viewport is the whole texture and it takes NO viewport modifier — the
    /// rect it renders is expressed once, in the texture's size.
    Ogre::CompositorPassSceneDef *scenePass = nullptr;
    /// The clear pass that paints the inset's background swatch. Its colour is
    /// read per execute too, so pushing a new background never rebuilds either.
    Ogre::CompositorPassClearDef *fill = nullptr;
    /// The quad that puts the inset ON the window — tonemapping it on the way
    /// (ViewPipDesc::tonemap) or copying it straight. Its mVpRect is the INNER
    /// rectangle, i.e. the letterbox, written live by OgreView::applyPip.
    Ogre::CompositorPassDef      *composite = nullptr;
    /// The 1x1 exposure clear the tonemapping form samples as `fInvLumAvg`.
    /// Null when the inset is not graded. Live: setPipExposure rewrites it.
    Ogre::CompositorPassClearDef *exposure = nullptr;
};

/// Builds the inset's node + workspace definitions under `workspaceDef`.
///
/// The shape is the spike's plus Route C's local texture, and every line of it
/// is a finding (see ViewPipDesc): the scene renders into a LOCAL texture sized
/// `texWidthFactor` x `texHeightFactor` of the target (its own depth, cleared;
/// shadows OFF; the overlay render queues excluded — an inset is "what the
/// camera sees", not a second copy of the editor's gizmos, and the camera BODY
/// that put the inset on screen must not appear inside it), and two quads then
/// paint the window: the background swatch over the OUTER rect and the inset
/// itself over the INNER one, both LOADing colour so the main frame survives.
///
/// When `pip.tonemap` is set both quads run `HDR/FinalToneMapping` — the SAME
/// material, in the same fixed-exposure form, as the main chain's tonemap
/// (POST_CHAIN_SPEC §14): the local texture is RGBA16F, a 1x1 exposure texture
/// is cleared to the constant the shader multiplies by, and the bloom input the
/// shader samples unconditionally is cleared to black. Grading the swatch too
/// is deliberate: the letterbox bars must not disagree with the background
/// inside the shot.
void buildPip(Ogre::Root *root, const std::string &workspaceDef, const ViewPipDesc &pip,
              float texWidthFactor, float texHeightFactor,
              std::vector<std::string> &nodeDefsOut, PipHandles &handlesOut);
/// The colour a fixed-exposure clear must carry for `exposure` (chain units).
/// The one conversion, shared by the main chain, the inset and every live
/// rewrite of either — see the derivation at fixedInverseLuminance.
Ogre::ColourValue fixedExposureColour(float exposure);
/// Tears down what buildPip made, including its datablock.
void destroyPip(Ogre::Root *root, const std::string &workspaceDef,
                std::vector<std::string> &nodeDefs, PipHandles &handles);
/// The inset's two rectangles in NORMALISED TARGET coordinates: `outer` is the
/// requested rect (clamped into the target), `inner` the largest `aspect`-shaped
/// rectangle centred inside it. Without constrainAspect the two are the same.
/// `targetAspect` is the target's own width/height — the outer rect is
/// normalised, so a square rect on a 16:9 view is not square in pixels.
void pipRects(const ViewPipDesc &pip, float targetAspect,
              float outer[4], float inner[4]);

/// PSO precache for a scene (SHADER_CACHE_SPEC.md §5): builds a throwaway
/// warm-up workspace that mirrors `refNodeDef`'s scene passes, renders it once
/// into a 4x4 target, and tears it down. Every shader `sm` needs through
/// `camera` — including shadow casters — is generated and compiled by the time
/// this returns.
///
/// The pass type is Ogre's own PASS_WARM_UP and the node is built by
/// Ogre::WarmUpHelper::createFrom, which copies the reference node's scene and
/// shadow passes (their RQ ranges, visibility masks and shadow nodes) into
/// warm-up passes and marks the last one CollectAndTrigger. So this warms
/// exactly the passes the real view runs, and it stays correct automatically
/// when the chain shape changes — there is no second description of the chain
/// to keep in step.
///
/// Nothing is presented. Returns false if the reference node is missing or the
/// workspace could not be built; failure is never fatal (the shaders compile
/// later, exactly as they did before this existed).
bool warmUp(Ogre::Root *root, Ogre::SceneManager *sm, Ogre::Camera *camera,
            const std::string &refNodeDef, const std::string &baseName);
/// True when warmUp() will take the CompositorPassWarmUp route rather than the
/// "render the view's own workspace" fallback. The caller needs to know because
/// the two routes want OPPOSITE things from the view: the warm-up pass runs in
/// its own workspace and needs the view's to stay DISABLED, while the fallback
/// is the view's own frame and needs it enabled.
bool warmUpUsesPass(Ogre::CompositorManager2 *cm, const std::string &refNodeDef);

// ---- Effect parameters -----------------------------------------------------
// EVERY ONE OF THESE IS PROCESS-GLOBAL (POST_CHAIN_SPEC.md §7.4): Ogre's HDR,
// SSAO and SMAA helpers all write MaterialManager singletons. The rule the
// backend follows is "the primary on-screen view owns the globals" —
// OgreEngine::renderOneFrame pushes them from the first enabled on-screen view
// whose chain has effects, and every other view lives with that.
void initHdrMsaa(unsigned samples);
void setExposure(float exposure, float minAutoExposure, float maxAutoExposure);
void setBloomThreshold(float minThreshold, float fullColourThreshold);
void initSsao(Ogre::Root *root);
void destroySsao(Ogre::Root *root);
void updateSsao(Ogre::Camera *camera, unsigned aoWidth, unsigned aoHeight,
                float kernelRadius, float powerScale);
void initSmaa(Ogre::Root *root, int preset);
/// The SSR march's per-frame uniforms: the camera's linearization constants and
/// the left-handed view→texture-space matrix the ray march projects with, plus
/// the tuning the description carries. Built exactly like Ogre's own
/// ScreenSpaceReflections::update — the matrix surgery there is not obvious and
/// is not ours to reinvent.
void updateSsr(Ogre::Camera *camera, const ChainDesc &desc);
// ---- The per-frame push, in two halves (CAMERA_LENS_SPEC §4) ---------------
//
// This was ONE function, `applyGlobals`, called once a frame from the primary
// on-screen view. It is two because the halves have DIFFERENT NATURAL SCOPES,
// and pretending
// otherwise was two defects at once (POST_CHAIN_SPEC §7.4's "the first enabled
// on-screen view owns the globals, then break"):
//
//   * the RECOMPILE half — the MSAA resolve weights and the SMAA preset — is a
//     shader reload. It cannot be per view without hitching on every camera cut
//     and every page switch, so it stays exactly where it was: pushed once a
//     frame from the primary on-screen view, debounced on "did the value
//     change" inside each helper. That is a DECISION, recorded, not a
//     limitation to engineer around.
//
//   * the CHEAP half — exposure, the bloom threshold, the AO kernel's camera
//     terms, the SSR march's matrices — is a handful of setNamedConstant
//     writes, and every one of them is a property of the view being drawn. Run
//     from a per-View CompositorWorkspaceListener's workspacePreUpdate it takes
//     effect for THAT workspace's passes in the same frame (spike R1, measured
//     on screen and offscreen), which fixes two pre-existing defects outright:
//     two on-screen views no longer fight over one exposure, and SSAO no longer
//     marches the FIRST view's projection in the second view's frame.
void applyRecompileGlobals(Ogre::Root *root, const ChainDesc &desc);
void applyViewGlobals(Ogre::Root *root, Ogre::Camera *camera, const ChainDesc &desc,
                      unsigned viewWidth, unsigned viewHeight);

/// The seed value the HDR adaptation history holds for a given exposure — the
/// same `e^(E-2) / 0.18` grey-card constant the fixed tonemap uses, so a
/// re-seeded history starts exactly where a deterministic grade would land.
float exposureSeed(float exposure);

/// THE OPT-IN PASS PROFILER (riders lane R4, EngineConfig::profile). One per
/// View, owned by it, registered through OgreView::addWorkspaceListener so it
/// rides every workspace rebuild. Measures the CPU time between a pass's
/// pre- and post-execute callbacks — what the render thread spent recording
/// that pass, including any wait it did inside it (a texture wait, a PSO
/// compile it blocked on) — keyed by the pass definition's profiling id, and
/// logs one summary line per kFlushFrames frames to the engine log:
///   [profile] view 'main' 120 frames, 4.83 ms/frame CPU in passes | Jahshaka
///   opaque 2.10 (max 9.4) | Jahshaka overlays 0.61 (max 1.2) | ...
/// sorted by total, top entries only. NOT GPU time — this pin has no timestamp
/// query surface; that is upstream work and is recorded as such. When the
/// profiler is off no instance exists, so the cost is exactly zero.
class PassProfiler final : public Ogre::CompositorWorkspaceListener {
public:
    explicit PassProfiler(std::string viewName) : mView(std::move(viewName)) {}
    void passPreExecute(Ogre::CompositorPass *) override {
        mStart = std::chrono::steady_clock::now();
    }
    void passPosExecute(Ogre::CompositorPass *pass) override {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - mStart).count();
        const Ogre::CompositorPassDef *def = pass ? pass->getDefinition() : nullptr;
        std::string key = def && !def->mProfilingId.empty() ? def->mProfilingId
                                                             : std::string("(unnamed pass)");
        Row &r = mRows[key];
        r.ns += (unsigned long long)ns;
        r.maxNs = std::max(r.maxNs, (unsigned long long)ns);
        ++r.calls;
    }
    void workspacePosUpdate(Ogre::CompositorWorkspace *) override {
        if (++mFrames >= kFlushFrames) flush();
    }
    /// Log whatever is accumulated (called on removal so a short run reports).
    void flush();
private:
    static constexpr unsigned kFlushFrames = 120;
    struct Row { unsigned long long ns = 0, maxNs = 0; unsigned calls = 0; };
    std::string mView;
    std::map<std::string, Row> mRows;
    unsigned mFrames = 0;
    std::chrono::steady_clock::time_point mStart{};
};

/// One per View, owned by it, registered through OgreView::addWorkspaceListener
/// so it survives every workspace rebuild (the planar listener's shape).
/// Pushes `applyViewGlobals` for its own view, immediately before that view's
/// workspace updates.
class ViewGlobalsListener final : public Ogre::CompositorWorkspaceListener {
public:
    void workspacePreUpdate(Ogre::CompositorWorkspace *) override;
    Ogre::Root *mRoot = nullptr;
    OgreView   *mView = nullptr;
};
}   // namespace chain

// ---------------------------------------------------------------------------
// The engine-drawn overlay — stats readout AND loading cover (impl in
// OgreOverlayHud.cpp; design in SPECS/STATS_OVERLAY_SPEC.md; the phase-0
// evidence in spikes/overlay-v1-vulkan/FINDINGS.md).
//
// Ogre's Components/Overlay, adopted whole (owner decision D1, ALL-IN-ON-OGRE).
// Everything in it is `namespace Ogre::v1` and it reaches the frame through a
// per-SceneManager RenderQueueListener at RQ 254 — inside the [210, 255) span
// our final "Jahshaka overlays" pass already draws, so no compositor change was
// needed.
//
// ONE SET FOR THE PROCESS. OverlayManager is a singleton with one overlay set,
// so there is exactly one HUD here, not one per view. Two on-screen Views
// therefore cannot show different text at once — the application's shape saves
// us (all its on-screen views live on different pages, so at most one is
// enabled at a time), and the engine recomposes the captions from the enabled
// view's desc once per frame. Documented on ViewOverlayDesc; if the invariant
// ever breaks, this is where it breaks.
namespace hud {
/// Creates the process-wide OverlaySystem. ORDER IS LOAD-BEARING: its ctor
/// creates BOTH OverlayManager and FontManager, so it must run AFTER a render
/// window exists and BEFORE any resource group holding a `.fontdef` is
/// initialised, or the font script is never parsed. Idempotent; silent no-op if
/// the component is unavailable.
void createSystem();
/// Adds `<mediaDir>packs/DebugPack` as a resource location in `group`. Call
/// between createSystem() and initialiseAllResourceGroups.
void addFontLocation(const std::string &mediaDir, const std::string &group);
/// Builds the overlay + its elements + the `Jahshaka/OverlayFill` HlmsUnlit
/// datablock, and EAGERLY LOADS the font. Call after Hlms registration and
/// resource-group initialisation. The eager load matters: the first caption on
/// a truetype font pays a full freetype rasterization + texture upload, and
/// under owner decision D2 the first caption happens while a world is loading —
/// the worst possible moment.
void build(Ogre::Root *root);
/// Registers/unregisters the render-queue listener on a SceneManager. Per
/// scene, so a scene that never shows an overlay pays nothing.
void attach(Ogre::SceneManager *sm);
void detach(Ogre::SceneManager *sm);
/// Recomposes the overlay from one view's desc, for the frame about to be
/// drawn. `viewWidth`/`viewHeight` are that view's target size IN PIXELS —
/// they turn the desc's pixel-sized type into Ogre's relative overlay metrics,
/// so the text is the same physical size whatever the viewport is.
void apply(const ViewOverlayDesc &desc, const RenderStats &stats,
           unsigned viewWidth, unsigned viewHeight);
/// THE SHADOW-ATLAS INSPECTOR's data, handed in by the engine because the HUD
/// cannot reach a compositor node (SHADOW_TOOLING_SPEC.md §4.4). One entry per
/// shadow map, in map order; `tex` is the LIVE atlas texture, which dies with
/// its workspace — the HUD binds it for exactly one apply() and unbinds when
/// the overlay goes off.
struct AtlasTileDesc {
    Ogre::TextureGpu *tex = nullptr;
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;   ///< the map's UV rect
    std::string label;                                   ///< "M3 point static"
};
/// Called by apply() through the engine: fills `out` with the current atlas.
/// Empty = nothing to draw (no shadow node, or no view rendering shadows).
void setAtlasTiles(std::vector<AtlasTileDesc> tiles);
/// Hides everything (no eligible view this frame).
void hide();
/// THE ONE-SHOT RE-CAPTION, run right after Root::renderOneFrame — see
/// OgreOverlayHud.cpp's `Caption` for the trap it exists for. Cheap: it does
/// nothing unless a caption changed in the frame just drawn.
void afterFrame();
/// Teardown: removeRenderQueueListener (per scene) -> destroy scenes -> THIS ->
/// delete Root. An OverlaySystem outliving Root is the same class of bug as a
/// MeshPtr outliving Root: ~OverlaySystem deletes the FontManager, and
/// Font::unloadResource destroys the HlmsUnlit datablock it created.
void destroySystem();
/// False when the Overlay component is not in this build/install at all.
bool available();
}   // namespace hud

// ---------------------------------------------------------------------------
// Small on-disk-cache primitives, shared by the TWO caches this engine keeps
// (the shader cache and the texture cache). Defined once in OgreShaderCache.cpp,
// where they were born as file statics; they moved into a named namespace when
// the texture cache (THREADING_ADOPTION_SPEC.md P2) became the second caller,
// because a second copy of "write it atomically" is a second place to get the
// rename wrong.
//
// Deliberately POSIX and deliberately not <filesystem>: the engine targets C++17
// on toolchains where <filesystem> still needs an extra link library on some
// hosts, and every one of these is a handful of lines.
// ---------------------------------------------------------------------------
/// WAIT FOR ONE TEXTURE TO BE RESIDENT (THREADING_ADOPTION_SPEC.md P2).
///
/// Since P2, `OgreScene::loadTexture` only SCHEDULES the load and the frame edge
/// collects (OgreEngine::renderOneFrame). That is safe for the overwhelming
/// majority of consumers — BINDING a not-yet-resident texture to an Hlms
/// datablock is self-correcting, because any residency or pool-slot change
/// destroys the descriptor set and reschedules the const-buffer update
/// (OgreHlmsTextureBaseClass.inl:459-486) and the rebake re-reads the slice.
///
/// IT IS NOT SAFE FOR A CALLER THAT READS THE TEXTURE ITSELF before the next
/// frame — its resolution, its internal type, its POOL SLICE, or its pixels.
/// Those reads happen once and are never revisited, so a texture that is still
/// on storage answers "0x0, Type2D, slice 0" and the caller quietly bakes that
/// in. The sky is the worked example and the reason this function exists:
/// SceneManager::setSky writes the texture's `sliceIdx` into the sky material as
/// a one-shot uniform, so an unresident equirect sky renders whatever is in
/// slice 0 of its pool — i.e. some OTHER image — for ever, silently. (Caught by
/// tests/engine equirect_sky_fills_the_background, which is exactly why that
/// assertion loads a second sky into the same pool.)
///
/// Cheap and honest: a no-op for a texture that is already resident, and for a
/// ManualTexture, which is resident by construction.
void waitForTextureResident(Ogre::TextureGpu *tex);

/// JAH_TEXTURE_SYNC_LOAD — restore the pre-P2 per-texture wait inside
/// loadTexture, at run time. The A arm of the batched-loading measurement
/// (THREADING_ADOPTION_SPEC.md G2-c) and the second step of the phase's order of
/// retreat (pool -> wait -> caches). Read once per process; a measurement and
/// recovery hatch, not a preference.
bool syncTextureLoads();

namespace cachefile {
/// mkdir -p. False only when a component could not be created.
bool mkpath(const std::string &dir);
/// Whole-file read. False when the file does not exist or could not be read.
bool readWholeFile(const std::string &path, std::vector<char> &out);
/// ATOMIC write: `<name>.tmp` in the SAME directory, flushed to the platform,
/// then renamed over the target. A crash mid-write leaves the previous good file
/// or no file — never half of one. (Ogre's Archive::create gives neither.)
bool writeAtomic(const std::string &dir, const std::string &name,
                 const void *data, size_t len);
/// 128-bit content hash as printable hex — Murmur3, which ships with Ogre.
/// NOT a cryptographic hash and not used as one: it detects corruption and
/// staleness, not tampering.
std::string hex128(const void *data, size_t len);
inline std::string hexOf(const std::string &s) { return hex128(s.data(), s.size()); }
}   // namespace cachefile

// ---------------------------------------------------------------------------
// The persistent TEXTURE cache (THREADING_ADOPTION_SPEC.md P2 items 6 and 7;
// impl in OgreTextureCache.cpp). TWO files, one manifest, one validity key.
//
//   texture-meta.json      Ogre's OWN texture metadata cache, exported verbatim
//                          by TextureGpuManager::exportTextureMetadataCache and
//                          fed back by importTextureMetadataCache. It records
//                          resolution/format/mipmaps/pool per texture PATH, which
//                          lets the MAIN thread reserve the right pool slice and
//                          transition the texture Resident BEFORE the worker has
//                          decoded anything (OgreTextureGpuManager.cpp:1788-1793)
//                          — removing the main<->worker ping-pong upstream
//                          describes at OgreTextureGpuManager.h:186-198.
//   texture-channels.txt   OURS (decision D-D(b)): path -> {numComponents,
//                          compressed}. It exists to kill a DOUBLE DECODE.
//                          loadTexture has to know whether a file is
//                          single-channel (those decode to R8 and sample red, so
//                          we expand them to RGBA on the CPU) and the only way to
//                          ask was to fully decode the image and throw the result
//                          away. With a hit, the probe is skipped entirely.
//
// WHY A LYING CACHE IS SAFE, and this is the load-bearing sentence for the whole
// item: Ogre SELF-CORRECTS. If the metadata says 2048x2048 RGBA8 and the file on
// disk is now something else, the streaming worker raises `OutOfDateCache`
// (OgreObjCmdBuffer.cpp:137-152), which drops the entry, transitions the texture
// back to OnStorage and reloads it properly. The cost of a stale entry is one
// wasted transition, never a wrong pixel — which is exactly what upstream's
// "performance will be degraded if the metadata cache lied" means.
//
// I-5, WHY THE VALIDITY KEY IS SIMPLER THAN THE SHADER CACHE'S: it lives in the
// SAME DIRECTORY (one lifetime, one "clear cache" button, one wipe) but has its
// OWN manifest and its OWN key, which deliberately does NOT name the GPU or the
// driver. Nothing here is device-specific — a resolution and a channel count are
// properties of a FILE — so folding it into the shader cache's fingerprint would
// throw both caches away on every driver update for no reason.
//
// PROCESS-WIDE, like its subject: `TextureGpuManager` belongs to the render
// system, not to a Scene, and `OgreScene::loadTexture` (the channel sidecar's
// only reader) has no engine pointer. `textureCache()` is the accessor.
class TextureCache {
public:
    /// Resolves the directory and computes the key. No I/O. An empty `dir`
    /// leaves the cache off — every method below then does nothing, and
    /// `channels()` always misses, which is exactly today's behaviour.
    void configure(const std::string &dir, const std::string &appBuildId);

    /// Reads both files (after verifying the manifest) and hands Ogre's half to
    /// `importTextureMetadataCache`. Call from ensureHlms(), after the Hlms is
    /// registered and before anything can ask for a texture. Any doubt — missing
    /// manifest, wrong key, size or hash mismatch, a JSON parse error — deletes
    /// both files and starts cold. Never throws, never fatal.
    void load(Ogre::Root *root);

    /// Writes both files and the manifest. Cheap and idempotent; a no-op when
    /// the cache is off or nothing changed since the last write.
    bool save(Ogre::Root *root);

    /// Forgets everything, in memory and on disk. The engine's clearShaderCache
    /// calls this too — the two caches share a directory, so "delete the cache"
    /// has to mean both or the manifest would outlive its files.
    bool clear();

    /// THE SIDECAR READ (D-D(b)). True when this path's channel count is known.
    /// A miss is not an error: the caller decodes once, exactly as it always
    /// did, and calls note() with the answer.
    bool channels(const std::string &path, unsigned &components, bool &compressed) const;
    /// Records what a decode found. Marks the cache dirty.
    void note(const std::string &path, unsigned components, bool compressed);

    /// Rows in the channel sidecar — a census number for app.textureStreaming().
    unsigned channelEntries() const;
    /// Rows in OGRE's metadata cache right now. Ogre exposes no size() for it,
    /// so this exports the map and counts entries; call it from a settings page,
    /// not from a frame.
    unsigned metadataEntries(Ogre::Root *root) const;

    TextureCache();
    ~TextureCache();

private:
    struct FileRec { std::string name; unsigned long long bytes; std::string hash; };
    bool readManifest(std::vector<FileRec> &out) const;
    bool writeManifest(const std::vector<FileRec> &files) const;
    void wipe() const;
    std::string path(const std::string &name) const;

    std::string mDir, mKey;
    bool        mEnabled = false;
    bool        mDirty = false;
    /// path -> (components, compressed). The channel sidecar, in memory.
    std::map<std::string, std::pair<unsigned, bool>> mChannels;
};

/// The process's one texture cache. Process-wide because TextureGpuManager is:
/// there is one render system per process (Ogre::Root is a singleton), so a
/// per-Engine instance would be the same object with more ways to get it wrong.
TextureCache &textureCache();

// ---------------------------------------------------------------------------
// The persistent shader cache (SHADER_CACHE_SPEC.md; impl in OgreShaderCache.cpp).
//
// Three layers Ogre already implements, behind ONE container we fingerprint and
// checksum ourselves:
//
//   pipeline.cache    RenderSystem::load/savePipelineCache  — the VkPipelineCache
//                     blob (driver ISA). Ogre validates it thoroughly on its own
//                     (vendor/device/driverVersion/pipelineCacheUUID + a payload
//                     hash), so this layer is the one we could almost trust.
//   microcode.cache   GpuProgramManager::load/saveMicrocodeCache — SPIR-V, keyed
//                     by a hash of the GENERATED shader source, so a stale hit is
//                     structurally impossible. But the file format has NO magic,
//                     NO version, NO checksum and NO bounds checking
//                     (OgreGpuProgramManager.cpp:368-398 reads a count and trusts
//                     it), and its bytes go straight to vkCreateShaderModule.
//                     Everything below about checksums exists for this sentence.
//   hlms.<n>.bin      HlmsDiskCache::saveTo/loadFrom — the Hlms-preprocessed
//                     shader source per permutation. Ogre version-stamps and
//                     template-hashes it correctly. NOTE it does NOT replay PSOs
//                     at our pin (the replay lines are commented out upstream,
//                     OgreHlmsDiskCache.cpp:424-455): this layer removes template
//                     parsing, not pipeline creation.
//
// THE RULES, in order of how much they cost to get wrong:
//   1. Never load stale. Any doubt -> delete the directory and start cold. A
//      cold start costs seconds; a bad SPIR-V blob costs a GPU hang.
//   2. WE checksum every file, because layer 2 does not.
//   3. Every write is atomic: *.tmp in the same directory, flushed, renamed.
//   4. One writer. A second process gets a READ-ONLY cache, never a failed run.
//   5. Load order is upstream's, not ours: pipeline cache -> setSaveMicrocodes
//      ToCache(true) then microcode -> Hlms disk caches. Placed inside
//      ensureHlms() after both registerHlms calls and before any shader can
//      compile, mirroring Samples/2.0/Common/src/GraphicsSystem.cpp:626-692.
//   6. Size cap with a generation reset — the microcode map never evicts.
//   7. A load failure is never fatal. Log, clear, continue cold.
class ShaderCache {
public:
    /// Resolves the directory and computes the fingerprint. No I/O beyond
    /// hashing the staged Hlms media tree. An empty `dir` leaves the cache off
    /// (but the compile COUNTERS still run — the startup progress display and
    /// the tests need them whether or not anything is persisted).
    void configure(const std::string &dir, const std::string &appBuildId,
                   const std::string &mediaDir);

    /// Starts counting compiles. Call as soon as the log exists (i.e. right
    /// after Root), long before any shader is built.
    void attachCounters();
    /// Removes the log listener. MUST run before Root is deleted.
    void detachCounters();

    /// The whole load, in upstream's mandated order. Call from ensureHlms()
    /// after registerHlms and before anything can compile a shader.
    void load(Ogre::Root *root);
    /// Writes every dirty layer. False = the write failed and the previous
    /// cache (if any) is untouched.
    bool save(Ogre::Root *root);
    /// True when something has been compiled since the last save — the
    /// burst-settle timer's condition, and what makes save() a cheap no-op.
    bool dirty(Ogre::Root *root) const;
    /// Deletes every file we wrote. The running process is unaffected.
    bool clear();

    ShaderCacheStats stats(Ogre::Root *root) const;
    void progress(unsigned &compiled, unsigned &fromCache, unsigned &expected) const;

    /// Both out of line: Counter is only defined in OgreShaderCache.cpp, and a
    /// unique_ptr member to an incomplete type needs its owner's special
    /// members compiled where the type IS complete.
    ShaderCache();
    ~ShaderCache();

private:
    struct Entry { std::string name; unsigned long long bytes; std::string hash; };

    bool  readManifest(std::vector<Entry> &filesOut) const;
    bool  writeManifest(const std::vector<Entry> &files) const;
    /// Reads `name`, checks it against the manifest entry, and returns the bytes.
    /// Empty on any mismatch — the caller then wipes.
    bool  readVerified(const Entry &e, std::vector<char> &out) const;
    void  wipe() const;
    bool  acquireLock();
    void  releaseLock();
    std::string path(const std::string &name) const;

    std::string mDir, mFingerprint, mMediaDir, mAppBuildId;
    bool        mEnabled = false;
    bool        mWriter = false;      ///< we hold the single-writer lock
    int         mLockFd = -1;
    unsigned    mExpectedShaders = 0; ///< from the manifest of the last saved run
    long long   mLastSavedUnixMs = 0;
    bool        mPipelineLoaded = false, mMicrocodeLoaded = false;
    /// The driver's verdict on the pipeline blob, scraped from its own log
    /// (ShaderCacheStats::pipelineCacheReason documents the values).
    std::string mPipelineReason = "absent";
    unsigned    mHlmsLoaded = 0;
    /// Microcode-map size right after the load — the baseline compiledThisRun
    /// would use if we had no log listener. Kept for the dirty() shortcut.
    size_t      mMicrocodeAtLoad = 0;
    /// compiled+cached at the last successful write — the "nothing new" test
    /// that stops a clean quit writing the same bytes twice.
    unsigned    mSavedAtCompileCount = 0;
    /// Set by clear(): the next save writes even though nothing new compiled.
    bool        mForceSave = false;
    class Counter;
    std::unique_ptr<Counter> mCounter;
};

// ---------------------------------------------------------------------------
// Fog. The DISTANCE term is Ogre's: an AtmosphereNpr registered on the scene's
// SceneManager (OgreScene::mAtmosphere) sets hlms_fog and binds its own const
// buffer, and the stock HlmsPbs pixel shader does the exponential mix. What the
// component cannot give us is an AUTHORED colour (it computes a procedural sky
// one) or height fog, so those ride this listener's pass-buffer extension, read
// by media/Hlms/Jahshaka/JahFog_piece_vs_piece_ps.any in BOTH shader stages.
//
// Parameters are per-scene, keyed by SceneManager: the listener is global to
// HlmsPbs, but preparePassBuffer receives the SceneManager of the pass being
// built. The two float4s are ALWAYS appended, fog on or off, so the pass-buffer
// layout never changes size; the shader members simply do not exist when fog is
// off (no hlms_fog, no piece).
struct FogState {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float heightDensity = 0.0f;     ///< 0 = no height layer (shader skips the branch)
    float heightFalloff = 0.1f;
    float heightLevel   = 0.0f;
};

class FogHlmsListener final : public Ogre::HlmsListener {
public:
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode *, bool casterPass,
                                   bool, Ogre::SceneManager *) const override;
    float *preparePassBuffer(const Ogre::CompositorShadowNode *, bool casterPass, bool,
                             Ogre::SceneManager *sceneManager, float *passBufferPtr) override;

    /// The per-scene fog table. OgreScene::setFog registers, the scene teardown
    /// unregisters, preparePassBuffer looks up.
    static void     registerScene(const Ogre::SceneManager *sm, const FogState &p);
    /// Fog OFF for a scene that still exists (setFog(enabled=false)).
    static void     unregisterFog(const Ogre::SceneManager *sm);
    /// The scene itself is going away: drops everything keyed by this manager.
    static void     unregisterScene(const Ogre::SceneManager *sm);
    static FogState lookup(const Ogre::SceneManager *sm);

    /// THE SHADER CLOCK (HLMS_ADOPTION P5), riding the same pass-buffer
    /// extension. A separate table from the fog because it is live whether or
    /// not the scene has fog — and because the two are unrelated features that
    /// happen to share the one custom_passBuffer piece a shader may define.
    ///
    /// Why the PASS buffer and not a per-material value: the vertex shader has
    /// no material buffer at all in this template (`material` is a pixel-shader
    /// #define onto a const-buffer array), so a per-material clock could never
    /// reach a vertex piece. One float in the pass buffer reaches both stages,
    /// costs one write per pass instead of one per material, and is the same
    /// number for every material in a frame — which is what "time" means.
    static void  setSceneTime(const Ogre::SceneManager *sm, float seconds);
    static float sceneTime(const Ogre::SceneManager *sm);

    /// THE DDGI SHADER STATE (GI_UNIFIED_SPEC.md §4 P1 and the Rayon ambient
    /// fix), riding the same pass-buffer extension for the same reason the
    /// clock does: every member is read by a piece of ours inside the PIXEL
    /// shader, once per pass, and all of it must be changeable without a shader
    /// rebuild.
    ///
    /// It exists because binding an IrradianceField sets `VctDisableDiffuse`:
    /// DDGI REPLACES voxel-cone diffuse rather than adding to it, and
    /// upstream's IrradianceFieldSettings has no intensity knob.
    /// media/Hlms/Jahshaka/JahIfd_piece_ps.any multiplies upstream's
    /// accumulated irradiance by `intensity` (1.0 = upstream's own brightness)
    /// and adds `ambient` x the sky visibility it derives from the depth atlas.
    /// Defaults to GiParams' defaults so a scene that never pushes state still
    /// reads sane values.
    struct IfdState {
        /// THE ESCAPE VECTOR FOR THE RASTER SOURCE (rayon2 S3), jahIfd2.xyz, and
        /// jahIfd2.w = 1 while it applies. The voxel path's threshold stays the
        /// shader's own expression (byte-identical); a raster field stores
        /// misses at camera-far x dot(|dir|, probesPerUnit), so the host sends
        /// far x scale x probesPerUnit and the shader takes dot(A(d), this).
        float escapeX = 0.0f, escapeY = 0.0f, escapeZ = 0.0f;
        float rasterSource = 0.0f;
        /// GiParams::ddgiIntensity, clamped.
        float intensity = 1.0f;
        /// GiParams::ddgiAmbient, clamped. 0 removes the ambient term through a
        /// uniform branch — the A/B the gate needs.
        float ambient = 1.0f;
        /// The field's probe counts on Y and Z. Upstream's own render params
        /// carry only Nx and Nx*Ny (OgreIrradianceField.cpp:812-813) and the
        /// sky-visibility threshold needs all three axes, so the two missing
        /// numbers ride our own float4 instead of a patch that would move the
        /// engine ABI.
        float numProbesY = 0.0f;
        float numProbesZ = 0.0f;
    };
    static void     setIfdState(const Ogre::SceneManager *sm, const IfdState &state);
    static IfdState ifdState(const Ogre::SceneManager *sm);

    /// THE IRRADIANCE-FIELD PASS-BUFFER ALIGNMENT, and it is a correctness fix
    /// rather than a feature — see the long note on the definition
    /// (OgreFog.cpp). Upstream's `IrradianceField::getConstBufferSize()`
    /// UNDER-REPORTS its own block by one float4: it declares 20 floats and
    /// `fillConstBufferData` writes 24, which is also what the shader's
    /// `IrradianceField` struct declares. HlmsPbs advances the write pointer by
    /// the reported 20, so whatever this listener writes next lands FOUR FLOATS
    /// EARLY, on top of the field's own irradiance-atlas parameters. The fix is
    /// four floats of leading padding whenever a field is bound to a non-caster
    /// pass, matched by a padding member in the shader piece.
    ///
    /// Global, not per scene, because the binding is: HlmsPbs is a singleton
    /// and sets `irradiance_field` for EVERY scene's pass while any field is
    /// bound. The listener therefore asks HlmsPbs itself rather than keeping a
    /// mirror that could drift.
    static void setPbs(Ogre::HlmsPbs *pbs);

private:
    /// 4 while a field is bound and this is not a shadow-caster pass (the two
    /// conditions HlmsPbs itself uses to emit the block), 0 otherwise.
    static Ogre::uint32 ifdAlignFloats(bool casterPass);
    static Ogre::HlmsPbs *sPbs;                                        // render thread only
    static std::map<const Ogre::SceneManager *, FogState> sFogState;   // render thread only
    static std::map<const Ogre::SceneManager *, float>    sSceneTime;  // render thread only
    static std::map<const Ogre::SceneManager *, IfdState> sIfdState;   // render thread only
};
extern FogHlmsListener gFogListener;

// ---------------------------------------------------------------------------
// Planar reflections (PLANAR_REFLECTIONS_SPEC.md; impl in OgrePlanar.cpp).
//
// SHAPE, in one paragraph, because the moving parts are spread over three
// classes: OgreScene owns ONE Ogre::PlanarReflections (Ogre allows one per
// SceneManager) plus one actor per reflector node; OgreView owns the compositor
// listener that drives it (the call must ride the MAIN workspace's per-frame
// update, and only the view knows its own workspace and camera); OgreEngine owns
// the half-resolution shadow-node DEFINITION the reflective pass may reference
// and re-syncs the view listeners once a frame.
namespace planar {

/// The reflective pass renders render queues [0, kReflectLastRQ) — the same
/// opaque range the main chain draws, deliberately stopping before the
/// on-top overlay queue. Gizmos, selection outlines and wire helpers must
/// never appear inside a mirror; excluding them by RENDER QUEUE rather than by
/// visibility bit means an overlay does not have to remember to tag itself,
/// and it composes with the chain's own RQ policy (OgreChain.cpp).
constexpr Ogre::uint8 kReflectLastRQ = 199u;

/// Builds the private workspace definition the reflection cameras render
/// through, under `workspaceDef` (node definitions appended to `nodeDefsOut`).
/// Deliberately NOT a copy of Samples/.../PlanarReflections.compositor: that
/// script hard-codes a 2048x7168 shadow atlas that would fight
/// Engine::setShadowResolution, and copying sample scripts is how the
/// patches-only law gets broken by the back door.
void buildWorkspace(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                    const PlanarReflectionParams &p, const std::string &shadowNodeName,
                    std::vector<std::string> &nodeDefsOut);
void destroyWorkspace(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                      std::vector<std::string> &nodeDefs);

/// A reflection plane derived from a node's own geometry.
struct Plane {
    Ogre::Vector3    centre;        ///< world space
    Ogre::Vector2    halfSize;      ///< actor-local X/Y half extents
    Ogre::Quaternion orientation;   ///< zAxis() IS the plane normal
    Ogre::Vector3    localNormal;   ///< the mesh-local unit axis the normal came from
    Ogre::Vector3    localCentre;   ///< the mesh-local AABB centre
};

/// Derives the plane from `item`'s local bounds and `node`'s world transform.
/// Returns false (with `error` filled) when the mesh is not plate-like: the
/// thinnest extent must be at most `kPlateRatio` of the next thinnest, or the
/// 20-degree matching rule makes the result look broken rather than merely
/// approximate.
constexpr float kPlateRatio = 0.1f;
bool derivePlane(Ogre::SceneNode *node, const Ogre::Item *item, Plane &out,
                 std::string &error);

/// Drives one Ogre::PlanarReflections from one view's workspace.
///
/// It must be a per-VIEW object even though PlanarReflections is per-SCENE:
/// `update()` takes the camera being rendered, and it does not merely book-keep
/// — it synchronously runs every active reflection workspace inside the
/// callback. Matching on the view's own camera pointer is also what keeps the
/// callback from firing for the reflection passes themselves. (Ogre's sample
/// discriminates with the magic pass identifier 25001; we do not adopt magic
/// numbers into our programmatic chain.)
class WorkspaceListener final : public Ogre::CompositorWorkspaceListener {
public:
    /// Both may be re-pointed at any time: the scene's arm is rebuilt on every
    /// parameter change and the view's camera is recreated on every setScene.
    Ogre::PlanarReflections *mReflections = nullptr;
    Ogre::Camera            *mCamera      = nullptr;

    void workspacePreUpdate(Ogre::CompositorWorkspace *) override;
    void passEarlyPreExecute(Ogre::CompositorPass *pass) override;
};

/// Ogre's component plus ONE accessor: the per-slot reflection workspaces
/// (ENGINE_CACHE_POLICY_SPEC P5). Each budget slot renders through its own
/// private workspace, and each of those instantiates its own reflect shadow
/// node — per-workspace state the lamp-map cache has to reach
/// (OgreEngine::applyShadowCache). `mActiveActorData` is protected
/// (OgrePlanarReflections.h:131), so this is engine API through inheritance,
/// the JahIrradianceField shape, not a patch. ~PlanarReflections is NOT
/// virtual: OgreScene holds and deletes this exact type.
class JahPlanarReflections final : public Ogre::PlanarReflections {
public:
    using Ogre::PlanarReflections::PlanarReflections;
    size_t slotCount() const { return mActiveActorData.size(); }
    Ogre::CompositorWorkspace *slotWorkspace(size_t i) const {
        return i < mActiveActorData.size() ? mActiveActorData[i].workspace : nullptr;
    }
};

}   // namespace planar

// ---------------------------------------------------------------------------
// Photometric (IES) light profiles and area-light mask textures — OgreLights.cpp.
//
// Both are PROCESS-WIDE by construction, not per-scene, and that is a decision
// rather than an accident:
//   * LightProfiles::build() writes HlmsPbs::setLightProfilesTexture AND
//     Root::_setLightProfilesInvHeight — there is exactly one of each per
//     process, so a per-scene registry would have scenes fighting over the
//     binding (the sVctBindingOwner shape in OgreGi.cpp, which we do not want
//     to repeat).
//   * HlmsPbs::setAreaLightMasks binds ONE 2D-array pool. Light::setTexture
//     stores only the pool SLICE index, so a mask that landed in a different
//     pool renders the WRONG texture with no error at all. One reserved pool,
//     one fixed resolution/format/mip count, everything resampled into it.
// Both cost a texture slot in EVERY pass once armed (mTexUnitSlotStart grows),
// which is why each arms lazily on first use — the loadLtcMatrix precedent.
namespace lightextras {

/// The mask pool's fixed shape. Every area-light mask is resampled to this,
/// because a mismatched image silently lands in a DIFFERENT pool and the light
/// then samples whatever happens to occupy its slice index in ours.
constexpr Ogre::uint32 kMaskPoolId     = 0x4A414831;   // 'JAH1'
constexpr Ogre::uint32 kMaskResolution = 512u;
constexpr Ogre::uint32 kMaskSlices     = 8u;
/// Colour masks (>2 components) so a mask can TINT the cast. Pool-wide and
/// one-time: hlms_lights_area_tex_colour is derived from the pool's format.
constexpr Ogre::PixelFormatGpu kMaskFormat = Ogre::PFG_RGBA8_UNORM_SRGB;
/// Forward+ area-light budgets. Ogre defaults to ONE of each and silently
/// drops the rest; the shader property IS the limit (not the live count), so
/// raising it costs one shader variant, not one per light added.
constexpr Ogre::uint16 kAreaApproxLimit = 4u;
constexpr Ogre::uint16 kAreaLtcLimit    = 4u;

/// Assigns the IES profile at `path` to `light` (empty path = unset). Loads and
/// atlas-builds the profile the first time a path is seen, and NEVER rebuilds
/// for a path already registered — `setLight` runs every frame, and build()
/// recreates + re-uploads a GPU texture.
/// Returns false and fills `error` on failure; the light keeps what it had.
bool assignProfile(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                   std::string &error);

/// Binds the area-light mask at `path` to `light` (empty path = unbind). The
/// image is decoded, resampled to the pool's resolution, given a full mip chain
/// (the diffuse term samples a very low mip — no mips is a visibly wrong mask)
/// and uploaded into a pool slice, once per path.
/// Returns false and fills `error` on failure; the light keeps what it had.
bool assignAreaMask(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                    std::string &error);

/// Raises HlmsPbs's forward area-light budgets off Ogre's default of 1. Called
/// where loadLtcMatrix is armed: the second area light of a scene renders
/// nothing without it.
void armAreaLightBudgets(Ogre::Root *root);

/// Loads HlmsPbs's LTC/BRDF lookup matrices, once — required before ANY area
/// light is drawn (the approximate kind loads them too). Lazy on purpose: the
/// load reserves a texture slot in EVERY pass, so scenes without area lights
/// must never pay it.
///
/// Lives here rather than as a function-local `static bool` at the call site
/// (where it was until the deep audit, OgreScene.cpp) because a function-local
/// static outlives the Engine: destroy the Engine, build a second one, and the
/// flag still says "loaded" while the new Root's HlmsPbs has no LTC matrix —
/// every area light in the second Engine then renders wrong, silently.
/// shutdown() resets it with the rest of the process-wide light state.
void armLtcMatrix(Ogre::Root *root);

/// Destroys the profile atlas and the mask pool. MUST run before `delete Root`
/// (a TextureGpu outliving its manager is the usual teardown crash).
void shutdown();

}  // namespace lightextras

// ---------------------------------------------------------------------------
// Decal atlases (OgreDecals.cpp). PROCESS-WIDE, like the TextureGpuManager pools
// they wrap: one image loaded by two scenes costs one slice. resetDecalAtlases()
// MUST run when Ogre::Root dies, or the next Engine in the process inherits
// stale TextureGpu pointers (test_engine_recreate creates a second one).
struct DecalAtlas;
DecalAtlas &decalAtlas(DecalMap kind);
void        resetDecalAtlases();
unsigned    decalAtlasCapacity();
bool        releaseDecalTexture(Ogre::TextureGpuManager *tm, DecalMap kind, Ogre::TextureGpu *tex);

// ---------------------------------------------------------------------------
// FILE TEXTURES ARE SHARED ACROSS SCENES (OgreMaterials.cpp, lane L11).
// OgreScene::loadTexture gets its textures from
// TextureGpuManager::createOrRetrieveTexture, whose lookup key is the ALIAS —
// so two scenes that load the same file in the same colour space (the editor
// and a thumbnail render of an asset that is in it) receive the SAME
// TextureGpu. Each scene tracks it; whichever released it first used to
// DESTROY it: Ogre's Deleted notification then stripped the map from the other
// scene's datablocks (an editor model went untextured the moment
// assets.refreshThumbnail rendered it), and the other scene's TextureRec kept a
// dangling pointer it later destroyed again. One reference per tracking scene;
// the last release destroys. resetSharedTextures() runs with Root's death, like
// resetDecalAtlases().
void retainSharedTexture(Ogre::TextureGpu *tex);
/// True when this was the LAST reference (the caller destroys the texture).
bool releaseSharedTexture(Ogre::TextureGpu *tex);
void resetSharedTextures();

// ---------------------------------------------------------------------------
/// OgreGi.cpp: Ogre::IrradianceField with the protected surface a source
/// switch needs (see there). Forward-declared here, defined beside its use.
class JahIrradianceField;

class OgreScene final : public Scene {
public:
    OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
              std::string &errorSink);
    ~OgreScene() override;

    const std::string &name() const override;

    void setAmbient(const Colour &upper, const Colour &lower) override;
    void setAmbientSh(const float sh[27]) override;

    void setFog(const FogDesc &desc) override;
    /// Creates the scene's AtmosphereNpr (fog only — the sky quad is created and
    /// immediately hidden, the sun/ambient link is never made) or destroys it.
    /// Destroying is what makes "fog off" bit-exact: no atmosphere means no
    /// hlms_fog property, which means the fog code is not in the shader at all.
    /// destroyAtmosphere() MUST run before the SceneManager dies (the component
    /// destroys its Rectangle2D through it).
    void ensureAtmosphere();
    void destroyAtmosphere();
    Ogre::AtmosphereNpr *mAtmosphere = nullptr;

    /// Ogre's OWN sky (SceneManager::setSky): a full-screen Rectangle2D at the far
    /// plane whose camera-direction shader samples an equirect or cube texture.
    /// There is no sky geometry of ours any more — no sphere, no six quads, no
    /// per-frame follow-the-camera. The equirect method needs a texture whose
    /// INTERNAL type is Type2DArray (automatic-batching pool slices are; our
    /// pixel-uploaded ManualTextures are not) — makeSkyArrayTexture() copies when
    /// it must. And it needs ogre-patch 0009: upstream's Vulkan GLSL declares
    /// `sliceIdx` but samples slice 0, so glslang strips the uniform and
    /// SceneManager::setSky throws on setNamedConstant AFTER attaching the sky.
    ///
    /// ONE entry point (SkyDesc), and it owns what the host used to: the
    /// dispatch on mode, the ordering (sky first, then reflections — a NoSky
    /// push destroys the reflections with the sky), and the "already applied"
    /// comparison, per half.
    bool setSky(const SkyDesc &desc) override;
    SkyDesc sky() const override { return mSkyDesc; }
    /// The description currently applied — the idempotency guard, and the
    /// reason a host may push every frame.
    SkyDesc mSkyDesc;
    /// The two halves of setSky, each only reached when its half of the
    /// description actually changed.
    bool applySkyMode(const SkyDesc &desc);
    /// Cubemap sky from six world-axis faces; also its own IBL source.
    bool applySkyCubemap(const TextureId faces[6]);
    /// Environment reflections divorced from the sky: six resampled faces of
    /// the host's equirect/baked sky image. Six zero ids clear.
    bool applySkyReflectionFaces(const TextureId faces[6]);

    /// THIS SCENE'S SHADOW REQUEST (ShadowDesc). The backend's filter and atlas
    /// are one per PROCESS, so all this does is apply the scene's resolved
    /// answer to the global state and drop a push that asks for what is already
    /// in force — which is exactly the read-before-write guard every host used
    /// to hand-write around Engine::setShadowFilter/setShadowResolution.
    void setShadowSettings(const ShadowDesc &desc) override;
    ShadowDesc shadowSettings() const override { return mShadowDesc; }
    ShadowDesc mShadowDesc;
    /// The engine that made this scene — the owner of the global shadow state
    /// above. Never null for a scene created through Engine::createScene().
    OgreEngine *mEngine = nullptr;
    /// Builds (replacing any previous) the GGX-prefiltered reflection cubemap by
    /// convolving `srcCube`, and binds it on every PBR datablock. `ownsSource`
    /// means the source is ours to destroy once the convolution has run (the
    /// cubemap-sky path passes false: there the source IS the sky texture).
    /// The prefilter runs on the next renderOneFrame (applyPendingIbl).
    void buildReflectionCubemapFrom(Ogre::TextureGpu *srcCube, bool ownsSource);
    /// Unbinds and destroys the reflection cubemap (no-op when there is none).
    void destroyReflection();
    /// Binds (or clears, when mReflectionTex is null) the scene's sky reflection
    /// cubemap on every PBR material's datablock. (Body in complete-class context,
    /// so it may call the private impl declared further down.)
    void applyReflectionToAll();
    /// Runs the queued ibl_specular convolution (roughness mip chain) for the
    /// reflection cubemap. Called once per frame by the engine, like applyPendingGi.
    void applyPendingIbl();
    Ogre::TextureGpu *mReflectionTex = nullptr;   // prefiltered cube on PBSM_REFLECTION
    void destroySky();
    bool removeNode(NodeId id) override;

    // ---- Hierarchy and transforms ----
    NodeId createNode(NodeId parent) override;
    NodeId adoptNode(void *nativeSceneNode) override;
    void *nativeSceneManager() const override;
    bool setNodeParent(NodeId id, NodeId parent) override;
    void setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) override;
    void setNodeVisible(NodeId id, bool visible) override;

    // ---- Meshes and materials ----
    MeshId createMesh(const MeshData &data) override;
    bool updateMeshVertices(MeshId id, const std::vector<float> &positions,
                            const std::vector<float> &normals) override;
    bool destroyMesh(MeshId id) override;
    MaterialId createPbrMaterial(const PbrParams &p) override;
    bool setPbrMaterial(MaterialId id, const PbrParams &p) override;
    bool setShadingModel(MaterialId id, ShadingModel model) override;
    bool destroyMaterial(MaterialId id) override;
    bool setMaterialCustomPiece(MaterialId id, const std::string &path,
                                CustomPieceStage stage) override;
    void setShaderTime(float seconds) override;
    float shaderTime() const override;
    std::string dumpMaterial(MaterialId id) const override;
    bool attachMesh(NodeId id, MeshId meshId, MaterialId matId) override;
    bool detachMesh(NodeId id) override;
    size_t itemCount(NodeId id) const override;

    // ---- Rigs: GPU skinning (GPU_SKINNING_SPEC; impl in OgreSkeleton.cpp) ----
    bool attachSkinnedMesh(NodeId id, MeshId meshId, MaterialId matId,
                           const SkeletonDesc &rig, const unsigned short *blendToRig,
                           size_t blendToRigCount) override;
    bool followSkeleton(NodeId follower, NodeId source) override;
    bool shareSkeleton(NodeId follower, NodeId source) override;
    bool sharesSkeleton(NodeId id) const override;
    bool attachToBone(NodeId rider, NodeId owner, const std::string &bone,
                      const Vec3 &position, const Quat &rotation, const Vec3 &scale) override;
    bool detachFromBone(NodeId rider, NodeId parent) override;
    bool setBoneAttachmentOffset(NodeId rider, const Vec3 &position, const Quat &rotation,
                                 const Vec3 &scale) override;
    NodeId boneAttachment(NodeId rider, std::string *bone = nullptr) const override;
    /// Copies every follower's pose from its source. Once per rendered frame,
    /// AFTER the frame, so the poses copied are the ones just drawn.
    void applySkeletonFollowers();
    bool hasSkeleton(NodeId id) const override;
    std::vector<std::string> boneNames(NodeId id) const override;
    bool setBonePoses(NodeId id, const BonePose *poses, size_t count) override;
    bool boneMatrices(NodeId id, float *out, size_t count) const override;
    size_t streamedBoneCount(NodeId id) const override;
    RigStats rigStats() const override;

    // ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC; impl in OgreClips.cpp) ----
    bool attachClips(NodeId id, const ClipDesc *clips, size_t count) override;
    std::vector<std::string> clipNames(NodeId id) const override;
    bool setClipStates(NodeId id, const ClipState *states, size_t count) override;
    bool setBoneManual(NodeId id, const std::string &bone, bool manual) override;
    bool bonePoses(NodeId id, BonePose *out, size_t count) const override;
    std::vector<float> clipBoneWeights(NodeId id, const std::string &clip) const override;

    // ---- Textures ----
    TextureId loadTexture(const std::string &path, bool srgb) override;
    TextureId createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb,
                            bool mipmaps = false) override;
    TextureId createCubemap(const TextureId faces[6]) override;
    bool updateTexture(TextureId id, unsigned w, unsigned h, const unsigned char *rgba) override;
    /// The staged RGBA upload (level 0 + the CPU-built mip chain), shared by
    /// createTexture and updateTexture so there is one copy of it.
    void uploadRgbaLevels(Ogre::TextureGpu *tex, unsigned w, unsigned h,
                          const unsigned char *rgba);
    unsigned  textureMipmaps(TextureId) const override;
    bool destroyTexture(TextureId id) override;
    bool setPbrTexture(MaterialId mat, PbrTextureSlot slot, TextureId texId) override;

    // ---- Overlay primitives ----
    MaterialId createUnlitMaterial(const Colour &c, bool depthTest, bool wireframe) override;
    MaterialId createOutlineMaterial(const Colour &c, bool skinnable) override;
    bool setUnlitMaterial(MaterialId id, const Colour &c) override;
    MeshId createLineMesh(const std::vector<Vec3> &points, bool strip) override;

    // ---- Particles (billboard sets) ----
    // Ogre-Next's BillboardSet2 (ParticleFX2 core, lives in OgreNextMain — no
    // plugin needed): geometry is generated in the vertex shader from a read-only
    // buffer the ParticleSystemManager2 uploads each frame. The manager attaches
    // every set to the STATIC root scene node, so positions are world-space —
    // exactly how the document simulates. Requires
    // Hlms::_setHasParticleFX2Plugin(true) before shaders are built (ensureHlms).
    bool createBillboardSet(NodeId id, TextureId texId, bool additiveBlend,
                            unsigned capacity,
                            BillboardLayer layer = BillboardLayer::Scene) override;
    bool setBillboards(NodeId id, const BillboardInstance *data, size_t count) override;
    bool destroyBillboardSet(NodeId id) override;

    // ---- Particles: engine-simulated systems (PARTICLES_FX2_SPEC.md) ----
    // One ParticleSystemDef per node. The def carries quota, material and
    // visibility; the ParticleSystem2 instance rides the node's SceneNode (the
    // emitter dereferences getParentNode() with no null check, so attaching is
    // mandatory). ParticleSystemManager2 has NO API to destroy a single def —
    // defs live until the SceneManager dies — so releases park the def on
    // mParticleDefPool keyed by its topology, and a matching rebuild reuses it.
    bool     setParticleSystem(NodeId id, const ParticleSystemDesc &d) override;
    bool     removeParticleSystem(NodeId id) override;
    unsigned particleCount(NodeId id) const override;
    unsigned particleDefinitionsCreated() const override { return mParticleDefsCreated; }

    // ---- Lights ----
    bool setLight(NodeId id, const LightDesc &d) override;
    bool removeLight(NodeId id) override;

    // ---- Decals (DECALS_SPEC.md; impl in OgreDecals.cpp) ----
    bool setDecal(NodeId id, const DecalDesc &d) override;
    bool removeDecal(NodeId id) override;
    TextureId loadDecalTexture(const std::string &path, DecalMap kind) override;
    unsigned decalAtlasCapacity(DecalMap kind) const override;
    unsigned decalAtlasUsed(DecalMap kind) const override;

    // ---- Global illumination (GI_SPEC.md phases 1-3) ----
    // Instant Radiosity traces rays from ONE chosen light against the scene's
    // PBR items and plants virtual point lights (LT_VPL) where the rays bounce.
    // The VPLs live in THIS SceneManager and ride its Forward+ clustered list —
    // nothing binds to the process-wide HlmsPbs, so IR never leaks into the
    // player/asset/preview scenes. (The sample's optional IrradianceVolume WOULD
    // be such a global binding — deliberately not used.)
    // VCT voxelizes the scene's PBR items over the GI bounds and cone-traces the
    // result; the hybrid adds a parallax-corrected cubemap probe grid whose
    // reflections blend with VCT's by distance (HlmsPbs PccVctMinDistance).
    // CAVEAT (GI_SPEC.md): setVctLighting/setParallaxCorrectedCubemap bind to the
    // process-wide HlmsPbs singleton — VCT GI is effectively editor-scene-only
    // in v1; the last scene to enable a VCT mode owns the binding, and other
    // scenes' geometry outside the voxel volume samples nothing (cones exit the
    // volume and add no light), so previews/thumbnails stay sane in practice.
    bool setGlobalIllumination(const GiParams &p) override;
    bool setGiDynamicProbes(int extraPerFrame) override;
    void refreshGlobalIllumination() override;
    GiStatus giStatus() const override;
    bool reassertGiBinding() override;
    unsigned long long giEscapeSignature() const override;
    unsigned long long giGeometrySignature() const override;
    unsigned long long giMaterialSignature() const override;
    bool refreshGiLighting() override;
    void setNodeGiBoundsExcluded(NodeId id, bool excluded) override;
    bool nodeGiBoundsExcluded(NodeId id) const override;
    void setNodeHelper(NodeId id, bool helper) override;
    bool nodeHelper(NodeId id) const override;
    void setNodeLightMask(NodeId id, unsigned mask) override;
    unsigned nodeLightMask(NodeId id) const override;

    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md; impl OgrePlanar.cpp) ----
    bool setPlanarReflections(const PlanarReflectionParams &p) override;
    bool setNodePlanarReflector(NodeId id, bool on) override;
    bool nodePlanarReflector(NodeId id) const override;
    int  activePlanarReflectors() const override;
    /// The scene's live PlanarReflections, or null when the budget is 0. Views
    /// read this once a frame to decide whether to arm their listener.
    Ogre::PlanarReflections *planarReflections() const { return mPlanar; }
    /// The hybrid's reflection-probe arm, or null (the shadow-cache probe in
    /// tests/shadow drives captures through it).
    Ogre::ParallaxCorrectedCubemapAuto *parallaxCorrectedCubemap() const { return mPcc; }
    /// Re-derives every actor's world plane from its node's CURRENT transform.
    /// Actors are world-space objects that do NOT follow a SceneNode, so this
    /// has to happen after the host has pushed transforms and before the frame
    /// renders. Called once per frame by the engine, like applyPendingGi.
    void applyPendingPlanar();
    /// Shadow-atlas rebuild support, the OgreView::dropWorkspaceForShadowRebuild
    /// shape: the reflective workspaces instantiate the half-resolution shadow
    /// node, whose DEFINITION cannot be replaced while anything references it.
    /// Returns true when the arm was actually dropped (caller re-adds).
    bool dropPlanarForShadowRebuild();
    /// The SAME contract for the hybrid's reflection-probe arm, and the fix for
    /// SHADOW_TOOLING_SPEC.md risk R3: when the probe captures are SHADOWED,
    /// every probe workspace (and the raster field's one) instantiates
    /// JahshakaProbeShadowNode too, so deleting
    /// the definition under them leaves live CompositorShadowNodes pointing at
    /// freed memory. Reproduced as a SEGV in Hlms::preparePassHashBase
    /// (tests/shadow, mode r3) before this existed. Returns true when the arm
    /// was dropped and the caller must call the recreate below.
    bool dropGiForShadowRebuild();
    void recreateGiAfterShadowRebuild();
    /// THE LAMP-MAP CACHE'S REACH INTO THIS SCENE'S PRIVATE WORKSPACES
    /// (ENGINE_CACHE_POLICY_SPEC P4/P5): the live workspaces that instantiate
    /// the shadow node of `kind` — each planar budget slot (Reflect) and each
    /// reflection probe whose captures are shadowed (Probe). A view's own node
    /// belongs to the view (OgreView::shadowNodeInstance), so View appends
    /// nothing here. Empty when the arm is off or unshadowed. The raster
    /// irradiance field's one workspace is NOT reachable (IrradianceFieldRaster
    /// keeps it private, no accessor at the pin), so its lamp maps stay on
    /// Ogre's dynamic path — correct, uncached.
    void shadowWorkspaces(ShadowNodeKind kind,
                          std::vector<Ogre::CompositorWorkspace *> &out) const;
    /// This scene's shadow-casting POINT and SPOT lights — the input to the
    /// derived focused-map count (SHADOW_TOOLING_SPEC.md §4.1). Directional
    /// lights ride the PSSM block and area lights can never cast, so neither
    /// counts. Fills `out` with the node ids when it is non-null.
    unsigned countLocalShadowCasters(std::vector<NodeId> *out) const;
    // ---- THE LAMP-MAP CACHE (ENGINE_CACHE_POLICY_SPEC P2/P3; OgreLights.cpp) --
    //
    // EVERY point/spot shadow map is a CACHE (lead decision D1 = A, 2026-09-12;
    // the per-light "Static Shadow" opt-in is gone): it renders once and is
    // re-rendered only when one of ITS inputs changes — the light's own
    // shadow-relevant parameters or pose, or a caster inside the light's reach
    // moving, appearing, disappearing or changing shape. Per LIGHT, in the SAME
    // frame: a mover near one lamp re-renders that lamp's map in the frame it
    // moves, and every other lamp stays cached. The camera, gizmos, helpers,
    // wires and outlines never dirty anything (none of them is a caster).
    // Directional lights are not cached — PSSM follows the camera.
    //
    // The scene DETECTS; the engine APPLIES, to every shadow-node instance that
    // draws the scene (views, planar slots, probes — ShadowNodeKind), in
    // OgreEngine::applyShadowCacheDirties.

    /// One cacheable lamp: a visible, shadow-casting point or spot light.
    struct ShadowCacheLight { NodeId id = 0; Ogre::Light *light = nullptr; };
    /// What one frame's detection produced for this scene.
    struct ShadowCacheFrame {
        /// The cacheable lamps in SLOT ORDER: every point light (by node id),
        /// then every spot (by node id). HlmsPbs reads the shadow-casting light
        /// list as directional, then point, then spot, cumulatively
        /// (OgreHlms.cpp preparePassHashBase): fixed slots must keep that order
        /// or a spot would be shaded as a point.
        std::vector<ShadowCacheLight> lights;
        /// Re-render every cached map (Engine::refreshShadows, a depth-relevant
        /// edit nothing finer covers).
        bool dirtyAll = false;
        /// Lamps whose maps are out of date, per shadow-node kind — a caster
        /// dirties a kind only through the render channels that kind's maps
        /// draw (shadowCasterChannels), which is the seam REALTIME_REFLECTIONS
        /// R2 needs (movers reach view/reflect maps, never probe maps).
        std::vector<Ogre::Light *> dirty[kShadowNodeKinds];
        unsigned casterChanges = 0;   ///< caster boxes that changed this frame
        unsigned lightChanges  = 0;   ///< lamps whose own inputs changed
    };
    /// Runs ONCE per drawn scene per frame, AFTER SceneManager::updateSceneGraph
    /// and before any workspace renders, so world AABBs and light poses are
    /// this frame's (no getWorldAabbUpdated root recursion, and a mover's
    /// shadow updates in the frame it moves).
    void collectShadowCacheFrame(ShadowCacheFrame &out);
    /// "Does this scene have anything to cache?" — decides the clear strategy
    /// (per-map quads only while some drawn scene holds a cacheable lamp).
    bool hasCacheableShadowLights() const;
    /// Re-render every cached lamp map of this scene on the next frame.
    void dirtyAllShadowMaps() { mShadowDirtyAll = true; }
    /// A depth-relevant change to what the items wearing `mat` cast — alpha
    /// mode/cutoff, two-sidedness, the albedo map under a cutout, a
    /// shading-model switch, a vertex piece (new vertex data flags its items in
    /// updateMeshVertices' own walk). Their lamps re-render on the next frame,
    /// per light, like a moved caster.
    void noteShadowShapeChanged(MaterialId mat);
    /// A skinned node's pose moved (a clip time, a pushed bone pose) — the one
    /// shape change an AABB scan cannot see (Items keep bind-pose bounds).
    /// PER NODE, not the scene's mRigPoseEpoch: one animating character must
    /// not re-dirty the lamps near every other rig.
    void noteNodePosed(NodeId id);
    void recreatePlanarAfterShadowRebuild();

    Ogre::SceneManager *sceneManager() const;

    /// The backend light behind a document node id, and the reverse lookup.
    /// Both exist for the shadow-map work (SHADOW_TOOLING_SPEC.md §4.3): the
    /// forward one to hand a light to setLightFixedToShadowMap, the reverse to
    /// name in `world.shadowStatus()` the lights the atlas actually mapped.
    Ogre::Light *ogreLight(NodeId node) const;
    NodeId nodeOfLight(const Ogre::Light *light) const;

    /// Releases everything in dependency order. Safe to call twice. Called by
    /// Engine::destroyScene and by the Engine destructor BEFORE Root dies.
    void destroy();

private:
    /// What a node owns. mNodes used to track only the SceneNode, leaking the
    /// Item, Light, mesh and datablock on removal (audit).
    struct Node {
        Ogre::SceneNode *node  = nullptr;
        /// False for a node ADOPTED from the document's own graph
        /// (SPECS/SCENEGRAPH_SPEC.md: one tree). The engine hangs its Item,
        /// light and decal off it but never destroys it, never re-parents its
        /// children and must never dereference it after the document has let
        /// it go — releaseNode nulls the pointer first.
        bool             owned = true;
        Ogre::Item      *item  = nullptr;
        Ogre::Light     *light = nullptr;
        /// A hash of the LightDesc fields the SHADOW MAP depends on — type,
        /// range, spot cone, castShadows — pushed by setLight; the lamp-map
        /// cache folds in the light's pose each frame. Colour and intensity are
        /// deliberately absent: a shadow map is depth, and a dimmer lamp casts
        /// the same shadow (ENGINE_CACHE_POLICY_SPEC §3).
        unsigned long long lightShadowKey = 0;
        /// Bumped by noteNodePosed (clip time / bone pose pushes): the caster
        /// scan re-renders the lamps around a skinned item whose pose moved.
        unsigned long long poseEpoch = 0;
        /// Set by noteShadowShapeChanged / updateMeshVertices; consumed by the
        /// next caster scan.
        bool             shadowShapeDirty = false;
        /// A hash of the LightDesc fields a reflection-probe capture can see
        /// (ENGINE_CACHE_POLICY_SPEC P7), so setLight stales the probe grid on
        /// a real parameter change only; 0 = no light pushed yet.
        unsigned long long lightProbeKey = 0;
        Ogre::SceneNode *lightNode = nullptr;   // internal child: -Y (document) -> -Z (Ogre)
        // What is CURRENTLY assigned to `light`, so the per-frame setLight can
        // do nothing when nothing changed. Both assignments are expensive the
        // first time (an atlas rebuild / a decode+resize+mipgen+upload) and the
        // mirror calls setLight for every light on every sync — at 60 Hz.
        std::string      lightProfilePath;
        std::string      lightMaskPath;
        // Vestigial since the selftest-era addTestCube was pruned (nothing assigns
        // these any more); releaseNode still clears them so a future node-owned
        // mesh/datablock keeps the "dropped before Root" teardown guarantee.
        Ogre::MeshPtr    mesh;              // uniquely owned; MUST be dropped before Root
        std::string      meshName;
        std::string      datablockName;     // uniquely owned
        MeshId           meshRef     = 0;   // shared, owned by mMeshes
        MaterialId       materialRef = 0;   // shared, owned by mMaterials
        /// Pose FOLLOWING (followSkeleton): the node this one copies its pose
        /// from, and the nodes copying theirs from this one. Kept on both sides
        /// so the per-frame pass can walk it from either end and so a node that
        /// goes away takes its pairings with it.
        NodeId               skeletonSource = 0;
        std::vector<NodeId>  skeletonFollowers;
        /// SKELETON SHARING (shareSkeleton, AVATAR_RIG_PERF_SPEC §3.2): the node
        /// whose SkeletonInstance this node's Item is actually rendering from,
        /// and the nodes rendering from this one's. A DIFFERENT relationship
        /// from the pose FOLLOWING above and deliberately kept apart: following
        /// copies bone locals once a frame and leaves both nodes in charge of
        /// their own transforms; sharing means there is ONE instance, evaluated
        /// once, whose bones carry the MASTER's node transform — which is why
        /// only pieces that sit exactly where the master does may share.
        ///
        /// `shareSource` is the LIVE Ogre state, not an intent: it is dropped
        /// the moment the share is undone (a detach on either end), and the host
        /// re-arms it. Anything else would make sharesSkeleton() lie.
        NodeId               shareSource = 0;
        std::vector<NodeId>  shareFollowers;
        /// BONE ATTACHMENT (attachToBone, AVATAR_RIG_PERF_SPEC §4): the engine
        /// TagPoint this node's scene node hangs from, the node that owns the
        /// bone, and the bone's name. One tag per RIDER (a tag shared by the
        /// riders of one socket is a later optimisation; riders per socket are
        /// one to three and a TagPoint is a node).
        Ogre::TagPoint      *boneTag = nullptr;
        NodeId               boneOwner = 0;
        std::string          boneName;
        /// The riders hanging off THIS node's bones, so an owner that dies (or
        /// loses its rig) can free them before its skeleton goes.
        std::vector<NodeId>  boneRiders;
        /// SKELETON GENERATION (S16, SMOKE_FIX_SPEC_2026_09_11 §1.1): bumped
        /// every time this node's Item — and with it the SkeletonInstance the
        /// Item owns — is destroyed. A ClipRec caches raw float*s into that
        /// instance's weight arrays and an INDEX into its animation list, so a
        /// record from an older generation is a dangling record. The counter is
        /// what says so even when the allocator hands the replacement instance
        /// the address the dead one had.
        unsigned long long   rigGeneration = 0;
        // Billboard set (particles): uniquely owned; freed by releaseBillboards
        // BEFORE the scene manager dies (its _destroy needs the live VaoManager).
        // Decal (DECALS_SPEC): the Decal rides an internal child node whose
        // scale IS the projector box (Ogre's culler reads the derived scale as
        // the box half-extents), so the document node's own scale composes with
        // the numeric width/height/depth.
        Ogre::Decal              *decal = nullptr;
        Ogre::SceneNode          *decalNode = nullptr;
        Ogre::BillboardSet       *billboards = nullptr;
        std::vector<Ogre::uint32> billboardHandles;   // live handles, dense, in order
        std::string               billboardDatablockName;
        unsigned                  billboardCapacity = 0;
        // Engine-simulated particle system (PARTICLES_FX2_SPEC): the def is NOT
        // uniquely owned — it outlives the node and returns to mParticleDefPool
        // (there is no destroyParticleSystemDef). The INSTANCE is ours to
        // destroy; the datablock belongs to the def and travels with it
        // (mParticleDatablocks), because a def binds its datablock exactly once,
        // inside init().
        Ogre::ParticleSystemDef  *particleDef = nullptr;
        Ogre::ParticleSystem2    *particleSystem = nullptr;
        std::string               particleTopology;   // the pool key this def answers to
        /// Whether the live def is a DISTORTION emitter, so the visibility
        /// pushes (setNodeVisible / setNodeHelper) hand it kDistortionBit and
        /// never kVisibleBit — the def is what _addToRenderQueue tests.
        bool                      particleDistortion = false;
        /// What setNodeVisible was last told for THIS node — its own flag,
        /// never overwritten by an ancestor's change, which is what lets
        /// showing a parent restore each descendant to what IT was told
        /// (RENDER_PIPELINE_AUDIT 1.2).
        bool                      visible = true;
        /// EFFECTIVE visibility: `visible` AND every engine ancestor's (the
        /// nearest registered ancestor's `shown`). The ONE state everything
        /// drawn off this node follows — its attachments' LAYER_VISIBILITY,
        /// the Item's kGiGeometryBit, the billboard and PFX2 flags (which no
        /// Ogre cascade reaches: they hang off the STATIC root) — and the
        /// state a system created or recycled later is born with.
        bool                      shown = true;
        /// This node's Ogre id, recorded at track() time so the registry's
        /// reverse index (OgreScene::mNodeByOgreId) can be cleaned up even
        /// after the document destroyed an adopted node under us. Ogre ids are
        /// never reused, unlike node addresses.
        Ogre::IdType              ogreId = 0;
        /// "Do not let this object define where GI happens"
        /// (REFLECTIONS_ADOPTION_SPEC.md P1a.2). It still VOXELIZES and still
        /// bounces light — the exclusion is only from the two AABB reductions
        /// (the lit volume and the probe region), which is the deterministic
        /// escape hatch for the ground plane, the skybox shell, the level's
        /// terrain: geometry that is real but is not what the lighting is about.
        bool                      giBoundsExcluded = false;
        /// EDITOR HELPER (REFLECTIONS_ADOPTION_SPEC.md P1b): the grid, light
        /// icons, range wires — things the user must see but a reflection probe
        /// must not capture. Carries kHelperBit instead of kVisibleBit.
        bool                      helper = false;
        /// LIGHTING CHANNELS, object side (Scene::setNodeLightMask). Kept here
        /// rather than read back off the Item because the Item is REBUILT on
        /// every attachMesh/attachSkinnedMesh (a material swap destroys and
        /// recreates it) and because a host may set the mask before any
        /// geometry exists — both cases would silently lose it otherwise.
        /// Ogre's own default (MovableObject::msDefaultLightMask, and it is
        /// genuinely consulted at OgreObjectDataArrayMemoryManager.cpp:138).
        Ogre::uint32              lightMask = 0xFFFFFFFFu;
        /// Whether the attached material is UNLIT, recorded at attach time.
        /// Needed because the helper flag can be toggled after the fact and the
        /// item's own flags cannot answer it once kVisibleBit is gone: a helper
        /// carries kHelperBit alone, so "does it have kGiGeometryBit" would read
        /// every helper as unlit and a lit mesh would never get its GI bit back
        /// when the flag cleared.
        bool                      materialUnlit = false;
        /// Whether the attached material's shading model is DISTORTION,
        /// recorded at attach time for exactly the same reason materialUnlit is
        /// (POST_LOOKS_SPEC.md §5.3): the item's own flags cannot answer it once
        /// kVisibleBit is gone, and the helper flag can be toggled afterwards.
        bool                      materialDistortion = false;
    };

    /// A definition's frozen shape. Two systems can share a recycled def only if
    /// every element of this matches, because none of it can be changed after
    /// ParticleSystemDef::init(): setParticleQuota asserts !isInitialized(), and
    /// adding an emitter once an instance exists corrupts the per-instance
    /// emitter array (sized once in the ParticleSystem2 ctor, indexed by the
    /// def's emitter count in the update loop).
    struct ParticleTopology {
        unsigned quotaBucket = 0;
        int      orientation = 0;
        bool     additive = true;
        bool     alphaHash = false;
        /// ParticleSystemDesc::distortion. Frozen with the def on purpose: a
        /// distortion def lives in kDistortionParticleRenderQueue with
        /// kDistortionBit and a displacement datablock, and a recycled def
        /// must never hand that to an ordinary emitter (or the reverse).
        bool     distortion = false;
        std::vector<int> emitterShapes;   // ParticleEmitterShape per emitter, in order
        std::vector<int> affectorKinds;   // ParticleAffectorDesc::Kind per affector, in order
        std::string key() const;
    };
    struct MeshRec {
        Ogre::MeshPtr mesh; std::string name;
        // CPU-skinning support (MeshData::dynamic): the interleaved vertex array
        // (12 floats: pos3 normal3 tangent4 uv2) is kept so updateMeshVertices can
        // rewrite positions/normals while preserving tangents and uvs.
        bool dynamic = false;
        std::vector<float> interleaved;
        // GPU skinning: the mesh was built with VES_BLEND_INDICES/WEIGHTS in its
        // vertex declaration, so attachSkinnedMesh may bind a rig to it. A mesh
        // carries at most ONE rig (Ogre::Mesh holds one SkeletonDef); `rigId` is
        // the SkeletonDesc::id that was bound, empty until one is.
        /// Does the uploaded vertex declaration carry VES_TANGENT? Computed
        /// once, when the mesh is built, because the alternative — deriving it
        /// per texture bind by walking every node's VAO — cost 2.1 s of boot
        /// (MATERIAL_GAPS_SPEC I-6, caught by app.watchdog_stall).
        bool hasTangents = true;
        bool hasSkinData = false;
        unsigned maxBlendIndex = 0;
        std::string rigId;
        /// The SubMesh's blend index -> rig bone index map as we wrote it
        /// (AVATAR_RIG_PERF_SPEC §3.1). Empty means "identity, rig.bones long" —
        /// which is what every mesh got before the union rig existed. Kept here
        /// because the map lives on the SUBMESH, i.e. per MESH: two nodes that
        /// share a mesh asset share the map, so a second attach asking for a
        /// DIFFERENT map has to be refused rather than silently re-target the
        /// first node's weights.
        std::vector<Ogre::uint16> blendToRig;
    };
    /// A rig, as this scene knows it. The Ogre-side SkeletonDef is cached
    /// PROCESS-wide by SkeletonManager under the same id (GPU_SKINNING_SPEC R6),
    /// which is why SkeletonDesc::id must be derived from the bone structure
    /// alone: two files of the same rig must resolve to one def.
    struct RigRec {
        std::vector<std::string> boneNames;
        /// The DESC the rig was built from, kept whole. A clip def must be
        /// built from the SAME bone structure the rig def was — Ogre's own
        /// addAnimationsFromSkeleton indexes the other def's block layout, and
        /// a mismatch is undefined behaviour rather than a no-op.
        SkeletonDesc desc;
    };
    /// One clip attached to one node.
    struct ClipRec {
        std::string id;             ///< content hash; the def-cache key
        std::string name;           ///< uniquified per node
        std::string defName;        ///< the SkeletonDef / v1 resource name
        float       length = 0.0f;  ///< seconds, after zero-length padding
        /// Bones this clip animates, and the cached weight slot for each (§4.2:
        /// per-bone renormalization is one float store per bone per frame, and
        /// only through a pointer we cached at attach time).
        std::vector<int>    coverage;
        std::vector<float*> weightPtr;   // parallel to coverage
        size_t index = 0;   ///< slot in SkeletonInstance::getAnimations()
    };
    /// A node's clip set. Absent until attachClips is called, which is also
    /// what takes the node out of manual-bone mode.
    struct NodeClips {
        std::vector<ClipRec>  clips;
        std::set<std::string> manualBones;   ///< explicit setBoneManual overrides
        /// attachSkinnedMesh marks EVERY bone manual (setBonePoses' values must
        /// survive resetToPose). The first attachClips clears that, because a
        /// manual bone is not reset to bind and a clip would ADD to the last
        /// pushed pose rather than replace it.
        bool clipModeEntered = false;
        /// WHOSE INSTANCE these records point into (S16): the SkeletonInstance
        /// they were taken against, and the node's rigGeneration at that moment.
        /// detachItem drops a node's clips with its Item, so this pair should
        /// never disagree — it is the braces to that belt, and the reason a
        /// re-attach the engine did not see is a logged refusal rather than a
        /// use-after-free write followed by a std::vector subscript off the end.
        Ogre::SkeletonInstance *owner = nullptr;
        unsigned long long      ownerGeneration = 0;
    };
    struct MaterialRec {
        std::string datablockName;
        bool unlit = false;
        /// An `unlit` material whose datablock actually lives on HlmsPbs: the
        /// SKINNED selection silhouette (HlmsUnlit has no skeletal path, so a
        /// rigged shell has to be a Pbs datablock with the colour as emissive).
        /// Only hlmsFor and setUnlitMaterial care.
        bool pbsBacked = false;
        /// The mirror image of `pbsBacked`, and the P4a addition: a real PBR
        /// MATERIAL whose SHADING MODEL is Unlit. `unlit` is also true for it
        /// (no GI, and hlmsFor must pick HlmsUnlit), but unlike an overlay it
        /// is scene geometry: it keeps the normal render queue, it is what
        /// setPbrMaterial/setPbrTexture write to, and its parameters live in
        /// `params` so the family switch can rebuild it.
        ///
        /// Invariant: shadingUnlit => unlit && !pbsBacked.
        bool shadingUnlit = false;
        bool onTop = false;
        /// PbrAlphaMode::Refractive. Refractive items must render in the chain's
        /// OWN pass (kRefractiveRenderQueue) — Ogre's words: "the compositor
        /// scene pass must be set to render refractive objects in its own pass".
        /// Left in the opaque pass they render as ordinary glass, silently.
        bool refractive = false;
        /// ShadingModel::Distortion (POST_LOOKS_SPEC.md §5.2). A THIRD family
        /// alongside `shadingUnlit`, sharing its datablock type (HlmsUnlit) and
        /// almost nothing else: the item goes to kDistortionRenderQueue, carries
        /// kDistortionBit instead of kVisibleBit, and the datablock's colour
        /// alpha is the material's own displacement strength.
        ///
        /// Invariant: distortion => unlit && shadingUnlit && !pbsBacked.
        bool distortion = false;
        /// Which TextureId occupies each PbrTextureSlot right now (0 = none).
        /// The REVERSE of the binding, kept so destroyTexture can unbind a
        /// texture from every material holding it: an Ogre datablock keeps a
        /// raw TextureGpu* and a descriptor set, so destroying a still-bound
        /// texture leaves a stale pointer that only shows up as a GPU-side
        /// fault later. Latent until something actually reclaims textures —
        /// which the mirror now does.
        TextureId boundTextures[kPbrTextureSlotCount] = {};
        /// What bindTrackedTextures LAST WROTE to the datablock, per slot, and
        /// whether it has ever written. The slot table went from 5 entries to
        /// 11 (detail layers + reflection), so an unguarded re-bind now issues
        /// SIX EXTRA setTexture calls per material — each dirtying the
        /// datablock's descriptor set — for slots that are null and were
        /// already null. Measured as +31% on e.first_sync@1000, the cold
        /// scene-open path. Skipping unchanged slots removes it.
        TextureId lastBoundTextures[kPbrTextureSlotCount] = {};
        bool      everBound = false;
        /// The sampler state those bindings were made with: a change to
        /// anisotropy or an address mode (A-2) has to re-bind every slot even
        /// when no texture moved, because the sampler rides the binding.
        float     lastAnisotropy = 1.0f;
        PbrParams::AddressMode lastAddress[kPbrTextureSlotCount] = {};
        /// The parameters LAST APPLIED to this material (PBR materials only).
        /// setShadingModel destroys the datablock and builds a new one in the
        /// other family, and it takes no parameters — it rebuilds from this.
        /// Without it a family switch would silently reset the material to the
        /// defaults until the host happened to push again.
        PbrParams params;
        /// Has `params` actually been APPLIED to the live datablock? False for
        /// one moment only — between createPbrMaterial filling the record and
        /// the first push — which is why setPbrMaterial's idempotency guard
        /// tests it as well as the value (a default-constructed PbrParams
        /// equal to the first push must still reach the datablock once).
        bool paramsPushed = false;
        /// The generated shader pieces bound to this material, per stage
        /// (HLMS_ADOPTION P5): absolute file paths, empty for "none". Kept for
        /// the same reason `params` is — setShadingModel builds a NEW datablock
        /// and would otherwise silently drop the piece.
        std::string customPiece[2];

    };
    struct TextureRec {
        Ogre::TextureGpu *texture = nullptr;
        std::string path;
        /// Decal-atlas slice (loadDecalTexture): SHARED process-wide and
        /// refcounted, so it must never go through the plain
        /// TextureGpuManager::destroyTexture path.
        bool     decal = false;
        DecalMap decalKind = DecalMap::Diffuse;
        /// The COLOUR SPACE the file was decoded into. Part of the dedup key:
        /// the same file bound as a base colour (sRGB) and as a roughness or
        /// detail-normal map (linear) is two different GPU textures, and
        /// keying on the path alone handed the first one out for both
        /// (MATERIAL_GAPS_SPEC I-2).
        bool     srgb = false;
        /// Came from createOrRetrieveTexture, so other scenes may hold the very
        /// same TextureGpu: refcounted (detail::retainSharedTexture), and only
        /// the last scene to release it destroys it.
        bool     shared = false;
    };

    void applyReflectionToAllImpl();
    /// The IBL cubemap AS BOUND TO DATABLOCKS — null while automatic PCC owns
    /// the shader's one env-probe slot (OgreSky.cpp, the long note there).
    Ogre::TextureGpu *reflectionTexForDatablocks() const;
    /// What ONE material's env-probe slot should hold: its own override cubemap
    /// if it has one, else the scene's global IBL cube, and NULL for both while
    /// automatic PCC is bound (ADDENDUM A-5). The single place that answers it,
    /// so a PCC change cannot leave a per-material cubemap behind — which would
    /// not merely look wrong, it would fail to compile the shader.
    Ogre::TextureGpu *reflectionTexFor(const MaterialRec &rec) const;
public:
    /// Is a refraction pass present in EVERY view that draws this scene?
    ///
    /// This is not a preference, it is a SAFETY INTERLOCK. An
    /// HlmsPbsDatablock::Refractive material rendered by a pass that does not
    /// offer it refractions generates a pixel shader referencing an undeclared
    /// `refractionMap`; the compile throws out of renderOneFrame and the WHOLE
    /// FRAME is lost, not just that object. Views of one scene do not all have
    /// the same chain (a screenshot renders the editor's scene through a
    /// throwaway offscreen view), so the engine recomputes this every frame and
    /// downgrades refractive datablocks to plain glass whenever any view that
    /// draws them lacks the pass. No combination of settings can black-frame.
    void setRefractionsActive(bool active);
    bool refractionsActive() const { return mRefractionsActive; }
private:
    /// The render queue an item using this material belongs in.
    static Ogre::uint8 renderQueueFor(const MaterialRec &m);
    /// Re-files every item that uses this material after its alpha mode changed.
    void refileItems(MaterialId id, const MaterialRec &m);

    /// Releases one texture record: a pooled decal slice drops a reference (and
    /// dies with the last one), anything else is destroyed outright.
    void releaseTextureRec(const TextureRec &rec);

    Ogre::Hlms *hlmsFor(const MaterialRec &m) const;
    /// Removes the renderable from a node that references a SHARED mesh/material.
    void detachItem(NodeId id, Node &n);
    /// S16: drops a node's clip RECORDS (never its manual-bone overrides, which
    /// are host intent). Called wherever the node's SkeletonInstance dies — the
    /// records are pointers into it.
    void dropNodeClips(NodeId id);
    /// True when a node's clip records were taken against an instance that is no
    /// longer the one the node has (or against an older generation of it).
    bool clipsAreStale(NodeId id, const NodeClips &nc,
                       const Ogre::SkeletonInstance *skel) const;
    /// The node's current rigGeneration, 0 for a node this scene does not know.
    unsigned long long rigGenerationOf(NodeId id) const;
    /// Forgets this node's pose-following pairings in both directions — the
    /// node itself is going away (releaseNode).
    void dropSkeletonFollowers(NodeId id, Node &n);
    /// Undoes one follower's SHARE, leaving it on its OWN instance, posed and
    /// PARENTED. Ogre leaves the fresh instance parentless
    /// (OgreItem.cpp:266-280), so the Item is re-attached to its node, which is
    /// the only thing that re-points it (MovableObject::_notifyAttached).
    void unshareFollower(NodeId followerId, Node &f, Node *master);
    /// Un-shares every follower of `id` — before its Item or its node dies.
    void releaseShareFollowers(NodeId id, Node &n);
    /// Forgets this node's sharing pairings in both directions (releaseNode).
    void dropShareFollowers(NodeId id, Node &n);
    /// Frees this node's TagPoint and puts its scene node back under `parent`
    /// (0 = the scene root) at the transform it last rendered with.
    void releaseBoneTag(NodeId id, Node &n, NodeId parent);
    /// Detaches every rider hanging off this node's bones — before the rig, the
    /// Item or the node dies. Riders land under the scene root, fail-soft.
    void releaseBoneRiders(NodeId id, Node &n);
    /// One follower's bone-local transforms, copied from its source.
    void copySkeletonPose(Node &follower, Node &source);
    /// Every node that HAS followers, so the per-frame copy costs the number of
    /// pairings and not the number of nodes in the scene. Pruned lazily by the
    /// pass itself.
    std::vector<NodeId> mFollowSources;
    // Ogre::HlmsPbsDatablock::None used to be unspellable here: X11's `None`
    // macro ate the identifier while this header included Xlib.h. It no longer
    // does — but X11 headers can still arrive transitively through Ogre on a
    // GL-enabled build, so the numeric spelling stays as the cheap guard.
    static constexpr auto kTransparencyNone = static_cast<Ogre::HlmsPbsDatablock::TransparencyModes>(0);
    /// `refractionsActive` false downgrades PbrAlphaMode::Refractive to plain
    /// glass — see setRefractionsActive for why that is not optional.
    static void applyPbr(Ogre::HlmsPbsDatablock *db, const PbrParams &p,
                         bool refractionsActive);
    /// The UNLIT half of applyPbr (HLMS_ADOPTION P4a): the subset of PbrParams
    /// the Unlit family can actually honour — base colour, alpha and its mode,
    /// two-sidedness. Everything else is dropped, deliberately and visibly
    /// (see ShadingModel in Types.h; the panel disables the rows this cannot
    /// carry rather than letting a user discover them).
    static void applyUnlit(Ogre::HlmsUnlitDatablock *db, const PbrParams &p);
    /// The DISTORTION datablock (POST_LOOKS_SPEC.md §5.2): an HlmsUnlit block
    /// whose texture is a screen-space displacement field and whose colour
    /// alpha is the strength. See the definition for why each block is set.
    static void applyDistortion(Ogre::HlmsUnlitDatablock *db, const PbrParams &p);
    /// The visibility bits a node's PFX2 def carries when visible:
    /// kDistortionBit for a distortion emitter, else helper/visible.
    static Ogre::uint32 particleVisibilityBits(const Node &n);
    /// (Re-)binds whatever `rec.boundTextures` says onto the material's CURRENT
    /// datablock — the step that makes a family switch keep its maps. The Unlit
    /// family has one usable slot (Albedo -> texture unit 0); the rest are kept
    /// in the record so switching back to Lit restores them.
    void bindTrackedTextures(MaterialRec &rec);   // records what it bound (see MaterialRec)
    /// The same job for generated shader pieces (HLMS_ADOPTION P5): re-applies
    /// `rec.customPiece` onto the material's CURRENT datablock, so a family
    /// switch back to Lit renders the graph again instead of the plain surface
    /// underneath it.
    void bindTrackedPieces(const MaterialRec &rec);
    /// Sets or clears `jah_shader_clock` — our datablock property gating the
    /// pass-buffer clock's DECLARATION — to match whether `rec` carries a
    /// generated piece. The whole pixel-suite isolation contract rests on this
    /// being false for every material that has none.
    static void applyClockProperty(Ogre::HlmsPbsDatablock *db, const MaterialRec &rec);
    /// Builds (or finds) the in-memory v1 skeleton `rig` translates to and hands
    /// the resulting SkeletonDef to `mesh`. v1 is a BUILD-TIME SCAFFOLD ONLY —
    /// SkeletonDef has exactly one constructor and it takes a v1::Skeleton
    /// (OgreSkeletonDef.h:145); nothing v1 reaches the render path (v1 meshes
    /// render NOTHING on Vulkan, and geometry stays in our v2 buffers).
    bool bindRigToMesh(MeshRec &meshRec, const SkeletonDesc &rig,
                       const unsigned short *blendToRig, size_t blendToRigCount);
    /// Assembles the in-memory v1 skeleton a SkeletonDesc translates to, under
    /// `resName`. Shared by the rig def and every CLIP def, because a clip def
    /// built from anything but the node's own rig indexes the wrong blocks in
    /// addAnimationsFromSkeleton — undefined behaviour, not a no-op.
    /// `madeOut`, when given, receives the bones in DESC ORDER.
    Ogre::v1::SkeletonPtr buildV1Skeleton(const std::string &resName, const SkeletonDesc &rig,
                                          std::vector<Ogre::v1::OldBone *> *madeOut = nullptr);
    /// The rig a node's mesh is bound to, or null.
    const RigRec *rigOf(NodeId id) const;
    /// The node's live rig, or null.
    Ogre::SkeletonInstance *skeletonOf(NodeId id) const;
    /// Uploads MeshData as a v2 mesh: interleaved position/normal/tangent/uv, 16- or
    /// 32-bit indices. v1 meshes silently render nothing on Vulkan, so only this path
    /// exists. Every mesh carries tangents: HlmsPbs refuses to render a normal-mapped
    /// datablock on a mesh without them (throws, object falls back to flat grey).
    Ogre::MeshPtr buildMeshV2(const std::string &name, const MeshData &data,
                              std::vector<float> *interleavedOut = nullptr);
    /// The six WORLD-axis faces (+X,-X,+Y,-Y,+Z,-Z, seen from inside) as ONE Ogre
    /// cubemap. Ogre samples cubemaps LEFT-handed — HlmsPbs negates the view
    /// matrix' Z column ("Cubemaps are left-handed", OgreHlmsPbs.cpp:2327) and
    /// SkyCubemap_ps.glsl negates cameraDir.z — so a world direction d reads the
    /// cube at (d.x, d.y, -d.z). Mirroring in Z means the +Z and -Z faces swap and
    /// every face image is mirrored: horizontally for +X,-X,+Z,-Z (their in-face U
    /// runs along +-Z) and vertically for +Y,-Y (their in-face V does). copyTo
    /// cannot mirror, so each face is downloaded, flipped and re-uploaded — once
    /// per sky change, not per frame. Returns null (and sets mError) on failure.
    /// `extraFlags` is OR-ed into the texture flags; `mips` false = one mip.
    Ogre::TextureGpu *buildCubeFromWorldFaces(Ogre::TextureGpu *const tex[6],
                                              const std::string &namePrefix,
                                              Ogre::uint32 extraFlags, bool mips);
    /// A Type2DArray (1 slice) copy of a 2D texture: SkyEquirectangular refuses
    /// anything whose internal type is not Type2DArray.
    Ogre::TextureGpu *makeSkyArrayTexture(Ogre::TextureGpu *src);
    /// Applies our render-queue / visibility policy to Ogre's sky renderable:
    /// queue 0 (the sky is drawn FIRST, exactly where our six quads used to be, so
    /// on-top overlays at queue 200 still paint over it), and kVisibleBit only, so
    /// Instant Radiosity's visibility-masked ray casts never hit it.
    void tuneSkyRenderable();
    /// Frees a node's billboard set and its datablock, in that order (the set
    /// references the datablock until it is destroyed). Safe to call twice.
    void releaseBillboards(Node &n);
    /// Arms this scene's helper overlay queue (kHelperOverlayRenderQueue): puts
    /// the queue into PARTICLE_SYSTEM mode and creates the depth anchor that
    /// makes the culler visit it. Idempotent; called by the first Overlay-layer
    /// billboard set, so a scene that never draws a helper icon pays nothing.
    void ensureHelperOverlayQueue();
    /// Destroys the depth anchor. Must run while the SceneManager is alive (the
    /// object unregisters from its entity memory manager).
    void releaseQueueDepthAnchor();
    /// Detaches and destroys the node's ParticleSystem2 instance, hides the def
    /// and parks it on mParticleDefPool (there is no destroyParticleSystemDef),
    /// and destroys the datablock the def referenced. Safe to call twice; must
    /// run before the SceneManager dies.
    void releaseParticleSystem(Node &n);
    /// Builds (or recycles) a definition matching `topo` and points `n` at it.
    /// The returned def is initialised and has its emitters/affectors in place;
    /// scalar values are pushed separately by applyParticleValues.
    bool buildParticleDef(Node &n, const ParticleSystemDesc &d,
                          const ParticleTopology &topo);
    /// Pushes every scalar of `d` onto the node's existing def. No rebuild.
    void applyParticleValues(Node &n, const ParticleSystemDesc &d);
    /// Creates or updates the Unlit datablock a particle def renders with and
    /// binds it. Returns the datablock name, empty on failure.
    std::string ensureParticleDatablock(Node &n, const ParticleSystemDesc &d);
    /// Destroys the node's decal and its internal child node, and re-points the
    /// SceneManager's decal atlases (clearing them when the last decal goes).
    /// Safe to call twice; must run before the SceneManager dies.
    void releaseDecal(Node &n);
    /// Points this SceneManager at the atlas master textures iff the scene
    /// still has at least one decal — the shader permutation is gated on a
    /// non-null SceneManager decal texture, so clearing it drops the decal
    /// code out of every PBS shader again.
    void refreshDecalBindings();
    void releaseNode(NodeId id, Node &n);

    // ---- GI internals ----
    /// Resolves the driving light — the requested node's light, else the first
    /// directional, else any light — and marks exactly that light with
    /// kGiLightBit so InstantRadiosity's light mask selects it alone. Returns
    /// null when the scene has no light at all. (IR's own LT_VPL lights are not
    /// in mNodes and are skipped by IR itself.)
    Ogre::Light *markGiLight(NodeId requested);
    /// The GI working volume: the document's explicit bounds, or (min == max)
    /// the world AABB of every GI-participating item plus a margin.
    bool computeGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const;
    /// Applies GiParams::autoBoundsMax to an AUTOMATIC fit (see the long note
    /// in OgreGi.cpp): the volume's largest axis may not exceed it, and the
    /// window that survives is centred on the content rather than on the union.
    /// Never called for a pinned volume.
    void clampAutoGiBounds(Ogre::Vector3 &mn, Ogre::Vector3 &mx) const;
    /// The mean of the CENTRES of the GI items that fit inside `maxEdge` —
    /// "everything that is not scenery". False when the scene is only scenery.
    bool giContentCentre(float maxEdge, Ogre::Vector3 &centre) const;
    /// Re-traces Instant Radiosity against the scene as it is right now. Cheap
    /// enough (a few ms at editor quality) to run on every light move.
    /// Ogre::InstantRadiosity caches mesh data by raw VertexArrayObject* and
    /// downloaded images by TextureGpu* (OgreInstantRadiosity.h:238-244). Destroying
    /// a mesh/texture while IR is live leaves those caches stale — the owner's
    /// scene-switch crash ("double free or corruption" tearing down a scene with IR
    /// enabled while the next project's assets churned). Ogre::VctVoxelizer has the
    /// same shape twice over: addItem keeps raw Item* until removeAllItems, and
    /// VctMaterial caches conversions by raw datablock pointer across builds.
    /// So every geometry/material/texture destroy path calls this BEFORE the
    /// object actually dies — IR's caches are freed EAGERLY here, because
    /// InstantRadiosity::freeMemory dereferences its VertexArrayObject* cache
    /// keys and calling it after the mesh died is itself the heap corruption.
    /// The rebuild still happens ONCE at frame time (bursty destroys = one
    /// rebuild); for VCT the flush tears the whole arm down and re-voxelizes
    /// from the LIVE scene, so a recycled pointer can never alias.
    void invalidateGiCaches();
public:
    /// ADDS this scene's registry sizes into `out` (nodes/meshes/materials/
    /// textures). Additive because Engine::objectCounts sums every live scene
    /// into one census — see ObjectCounts. Touches nothing else in `out`.
    void addObjectCounts(ObjectCounts &out) const;
    /// Called by Engine::renderOneFrame before rendering.
    void applyPendingGi();
    /// Called by Engine::renderOneFrame once the frame's GI work is decided and
    /// before it renders: records how many probes this frame re-captures
    /// (GiStatus::probeCapturesLastFrame). `drawn` = a View draws this scene
    /// this frame (an undrawn scene's dirty probes do not render).
    void latchProbeCaptures(bool drawn);
    /// Called by OgreView each frame with its camera position: the PCC probe
    /// blend tracks the viewer. No-op unless the hybrid mode is live.
    void updateGiTracking(const Ogre::Vector3 &camPos);
    /// Re-derives the Forward+ clustered depth-slice range from this camera and
    /// the scene's own extent (LIGHTING_FIX fix 8 / F-F1). Rate-limited AND
    /// hysteretic — `setForwardClustered` recreates the grid buffers, so it must
    /// never run per frame.
    void updateForwardPlusRanges(const Ogre::Camera *cam);
    /// How many lights this scene holds that Forward+ has to fit into its
    /// per-cell budget, and the budget itself (F-F2). See RenderStats.
    void forwardPlusLightCensus(unsigned &lights, unsigned &budget) const;
private:
    void rebuildGi();
    /// Voxelizes the scene's PBR items over computeGiBounds at quality-mapped
    /// resolution, (re)builds VctLighting and binds it to HlmsPbs. The voxelizer
    /// and lighting are recreated from scratch every time (see invalidateGiCaches).
    /// In hybrid mode also (re)builds the PCC probe grid.
    void rebuildVct();
    /// Builds the ParallaxCorrectedCubemapAuto probe grid over `region` — the
    /// FREE SPACE from computeProbeRegion, NOT the voxel volume — and binds it
    /// with distance-blended VCT specular (PccVctMinDistance).
    void buildPcc(const Ogre::Aabb &region);

    // ---- DDGI: the IrradianceField arm (GI_UNIFIED_SPEC.md §4 P1) ---------
    // Built INSIDE the VCT arm and owned by it: the field cone-traces
    // mVctLighting's volume, holds a raw pointer to it, and binds its voxel
    // textures on every update — so it lives strictly inside that pointer's
    // lifetime (P0 spike §8) and dies FIRST in teardownVct.

    /// True when this scene should have a field: the toggle resolves on AND the
    /// mode is one that produces a VctLighting to feed it.
    bool ddgiWanted() const;
    /// Creates + initializes the field over the CURRENT voxel volume, converges
    /// it in one dispatch, binds it to HlmsPbs and takes the process-wide
    /// binding. No-op (and unbinds) when ddgiWanted() is false. Called at the
    /// end of rebuildVct and of refreshVctFast — a VCT (re)build invalidates the
    /// field entirely, which upstream answers with re-initialize, not reset.
    void buildIrradianceField();
    /// Unbinds (if this scene owns the binding) and destroys the field. Called
    /// first in teardownVct, and by buildIrradianceField before it rebuilds.
    void teardownIrradianceField();
    /// Spends the frame's convergence budget on an in-flight re-converge.
    /// No-op when the field is converged, when there is no field, or when the
    /// update budget is 0 (paused). Called once a frame from updateGiTracking.
    void updateIrradianceField();
    /// Per-axis PROBE COUNTS for a field over `size`, each a power of two
    /// (upstream only ASSERTS that, and the assert is compiled out of our
    /// release engine) and together kIfdTotalProbes. Fitted from the volume's
    /// ASPECT: upstream's 32x8x32 default bands visibly on a room-shaped volume
    /// because the Y spacing is twice the X/Z one (P0 spike §9b).
    static void ifdProbeCounts(const Ogre::Vector3 &size, Ogre::uint32 outCounts[3]);
    /// The probes-per-frame a re-converge may spend, given the update budget —
    /// clamped to the engine's dispatch rule. THE CLAMP IS MANDATORY, not
    /// defensive: `numRays = ppf * depthRes^2 * raysPerPixel` under
    /// `threadsPerGroup` dispatches ZERO work groups, which throws out of
    /// HlmsCompute::compileShader and terminates the process, and the assert
    /// that would have caught it is compiled out of a release-built Ogre
    /// (P0 spike §4). Returns 0 for a paused budget, meaning "do not call
    /// update() at all".
    static Ogre::uint32 ifdProbesPerFrame(const Ogre::IrradianceFieldSettings &settings,
                                          int updateBudget, Ogre::uint32 totalProbes);
    /// Clamps every probe's fitted PARALLAX SHAPE into `region`, per axis
    /// (FIX WAVE defect A2). buildEnd's 1x1 averaged depth readback overshoots
    /// badly whenever anything stands between a probe and the wall behind it,
    /// and a parallax box larger than the free space the grid was fitted to is
    /// never right. See the long-form argument on the definition.
    void clampProbeShapesToRegion(const Ogre::Aabb &region);
    /// Re-applies Forward+ clustering with the CURRENT budgets and the given
    /// depth-slice range. One funnel, so the two call sites (the range update
    /// and the probe-budget growth) cannot drift apart in their other seven
    /// arguments — they did not, and the way to keep it that way is to have one
    /// place where the arguments are written.
    void applyForwardClustered(float minDistance, float maxDistance);
    /// Grows the Forward+ per-cell cubemap-probe budget to hold `probeCount`
    /// (kCubemapProbeSlotsDefault says why). Returns true if the budget moved —
    /// which costs one Forward+ shader recompile, so it is deliberately
    /// quantised and monotonic.
    bool ensureCubemapProbeSlots(size_t probeCount);
    /// Spends this frame's probe-update budget on STALE probes only: picks the
    /// highest-priority stale probes and raises `mDirty` on them (FIX WAVE B2;
    /// ENGINE_CACHE_POLICY_SPEC P1 — a grid with nothing stale spends nothing).
    /// Called from updateGiTracking with the AUTHORITATIVE camera position; a
    /// no-op at budget 0.
    void updateProbeBudget(const Ogre::Vector3 &camPos);
    /// Marks every probe of the grid stale — owed a capture the budget will
    /// spend over the next frames — and records why (P1/P6/P7). Cheap: flags
    /// only; nothing renders here. A no-op without a probe grid.
    void staleProbeGrid(GiStaleReason why);
    /// P7 hooks: does any item that probes (probeSeesItem) or the voxelizer
    /// (kGiGeometryBit) can see use this material?
    bool materialSeenByGi(MaterialId id, bool &voxelized) const;
    /// Would a reflection-probe face capture this node's item? kVisibleBit
    /// (the face pass's visibility_mask 0x1 — helpers carry kHelperBit and
    /// distortion items kDistortionBit instead), effectively shown, and in the
    /// face pass's render-queue range (rq_last 200: gizmos and selection
    /// outlines at RQ 210 are never captured, so they never stale anything).
    bool probeSeesItem(const Node &n) const;
    /// P7, materials: a visible material changed — stale the probes and, when
    /// the change reaches the voxelizer's conversion, bump the material
    /// generation (see mGiMaterialGeneration).
    void noteMaterialChanged(MaterialId id, bool voxelInputsChanged);
    /// P6/P7: the reuse arm's variant for a MATERIAL change — a fresh voxelizer
    /// and lighting (VctMaterial's by-pointer cache must go) under the SAME
    /// probe grid, whose shapes a material edit cannot move. Returns false
    /// when it could not build (the caller falls back to rebuildVct).
    bool freshVoxelArm(const Ogre::Aabb &aabb);
    /// The voxelizer + lighting half of rebuildVct, shared with freshVoxelArm:
    /// builds mVctVoxelizer/mVctLighting over `aabb` from the live GI items.
    /// Returns the item count (0 = nothing built, both left null).
    size_t buildVoxelArm(const Ogre::Aabb &aabb);
    /// Refreshes mGiItemAabbs and fills mGiMovedBoxes with what moved since the
    /// last call (FIX WAVE B3, engine half). Called once per frame from
    /// updateProbeBudget; NOT from giGeometrySignature, which is stateless.
    void scanGiMovement();
    /// rayon2 S3 — the raster probe source. resolveSource: GiParams::ddgiSource
    /// with Auto = Voxel at every tier. applyRasterSource: re-sources a just
    /// converged voxel field to the raster workspace in place (refused, logged,
    /// when the workspace or patch 0023's media is missing). pushIfdState: the
    /// pass-buffer block (JahIfd_piece_ps.any) for the current source.
    GiSource      resolveSource() const;
    void          applyRasterSource(const Ogre::IrradianceFieldSettings &settings,
                                    const Ogre::Vector3 &origin, const Ogre::Vector3 &size);
    void          pushIfdState(const Ogre::IrradianceFieldSettings &settings);
    static Ogre::uint32 ifdRasterProbesPerFrame(int updateBudget, Ogre::uint32 totalProbes);
    /// The movement quantum for one item's world AABB (a 64th of its own
    /// largest extent), and "did this AABB move by at least that much?". Shared
    /// by giGeometrySignature and scanGiMovement so the mirror's debounce and
    /// the probe round-robin can never disagree about what moved.
    static float giAabbQuantum(const Ogre::Aabb &a);
    static bool  giAabbMoved(const Ogre::Aabb &before, const Ogre::Aabb &after);
    /// The VCT light-injection ray-march step scale to use (FIX WAVE B5): the
    /// document's at-rest value, raised on the cheap in-motion re-injection.
    float giRayMarchStepScale(bool inMotion) const;
    /// THE REUSE ARM (FIX WAVE B4). Re-runs the EXISTING voxelizer and lighting
    /// over the live scene instead of tearing the arm down and building a new
    /// one, and re-dirties the probes without re-running the placement pass.
    /// Refuses (returns false, caller falls back to rebuildVct) whenever
    /// anything the arm holds a raw pointer into may have died since the build,
    /// or the probe region moved enough that the shapes must be re-derived.
    bool refreshVctFast();
    /// The visibility flags an Item attached to `n` must carry, given the
    /// material's unlit-ness and the node's helper designation. THE one place
    /// the bit scheme is applied to geometry.
    /// The visibility bits an ITEM carries, from its NODE's helper flag and its
    /// MATERIAL's family. `distortion` wins over everything: such an item is
    /// drawn by exactly one pass in the engine and must be invisible to the
    /// rest (kDistortionBit's note).
    Ogre::uint32 itemVisibilityFlags(Node &n, bool unlit, bool distortion = false);
    /// Re-applies itemVisibilityFlags (and the billboard/particle equivalents)
    /// to whatever `n` currently carries. Needed because the helper flag can be
    /// set before or after the geometry is attached.
    void applyNodeVisibilityFlags(Node &n);
    /// The registry record hanging off an Ogre node, or null (an engine-owned
    /// helper child such as a light's -Y adapter, a document node the host has
    /// not adopted yet, the scene root).
    Node *registryNode(const Ogre::Node *sn);
    /// The EFFECTIVE visibility `sn`'s children inherit from above it: the
    /// `shown` of the nearest registered ancestor, true when there is none.
    bool inheritedShown(const Ogre::Node *sn);
    /// Applies EFFECTIVE visibility to `sn` and everything under it in Ogre's
    /// graph: a registered node shows iff `inherited` and its own flag; an
    /// unregistered one (a helper child) passes `inherited` through. Sets
    /// `giChanged` when any Item's kGiGeometryBit moved, so the caller can
    /// invalidate GI ONCE for the whole subtree. (RENDER_PIPELINE_AUDIT 1.1/1.2)
    void applyShownSubtree(Ogre::SceneNode *sn, bool inherited, bool &giChanged);
    /// Voxel volume resolution per axis for the current quality.
    unsigned giVoxelResolution() const;
    /// The GI items' world AABBs after the exclude flag and the extent-outlier
    /// trimming: the one place that decides which objects define the lit world.
    std::vector<Ogre::Aabb> giItemBounds() const;
    /// Records (or clears) mGiAutoVolume after a rebuild. `fitted` is what the
    /// AUTO path resolved; a hand-typed bounds box clears the record instead.
    void noteGiAutoVolume(const Ogre::Aabb &fitted, bool automatic);
    /// True when the document typed a bounds box by hand (min != max).
    bool giBoundsExplicit() const;
    /// True when the scene holds a light VctLighting's injection pass collects.
    /// Drives the auto-multiplier guard — see the note on the definition.
    bool hasVctLights() const;
    /// Pushes mAmbientRadiance into the VCT arm (no-op without one). Called on
    /// every ambient change and whenever the arm is (re)built.
    void applyVctAmbient();
    /// Where the reflection probes live: the free space inside `litVolume`.
    /// See the long-form argument on the definition — handing the padded voxel
    /// volume here instead is what made P4's finding-2 reflections go black.
    Ogre::Aabb computeProbeRegion(const Ogre::Aabb &litVolume) const;
    /// Unbinds from HlmsPbs (when this scene owns the binding) and deletes the
    /// PCC, VctLighting and VctVoxelizer, in that order. Safe to call twice;
    /// must run BEFORE the SceneManager dies.
    void teardownVct();
    /// Deletes the radiosity solution and its VPL lights. Safe to call twice;
    /// must run BEFORE the SceneManager dies (the dtor destroys its lights).
    void teardownIr();
    /// Deletes every GI object (IR + VCT arms). Safe to call twice; must run
    /// BEFORE the SceneManager dies (VPL lights, probe workspaces, GI camera).
    void teardownGi();

    // ---- Planar-reflection internals (OgrePlanar.cpp) ----
    /// Builds the PlanarReflections arm at mPlanarParams (private workspace
    /// definition, cameras, RTTs) and re-adds every reflector's actor. Always
    /// from scratch: the VCT lesson — Ogre caches by raw pointer inside these
    /// objects, and setMaxActiveActors can only grow, never re-specify.
    void rebuildPlanar();
    /// Unbinds from HlmsPbs (when this scene owns the binding), deletes the
    /// PlanarReflections and removes the private workspace definitions. Safe to
    /// call twice; MUST run before the SceneManager dies (it owns the
    /// reflection cameras and workspaces).
    void teardownPlanar();
    /// Registers `n` with the live arm: adds its actor and adds its item as a
    /// PBS receiver. No-op when the arm is down. Returns false + mError when
    /// the node's mesh is not plate-like, or when its material is TWO-SIDED
    /// (which defeats the winding-based self-exclusion — OgrePlanar.cpp).
    bool armReflector(NodeId id, Node &n);
    /// Removes `n` from the live arm. MUST be called before the node's Item is
    /// destroyed: PlanarReflections keeps raw Renderable pointers and its own
    /// header says so in as many words.
    void disarmReflector(NodeId id, Node &n);
    /// disarmReflector for every reflector, keeping the flags — used when the
    /// arm itself is being torn down and rebuilt.
    void disarmAllReflectors();

    /// The scene node behind an id, or null. PUBLIC since P2b: a View that
    /// rides its camera on a node (setCameraNode) needs exactly this one lookup
    /// and nothing else of the scene's internals.
public:
    Ogre::SceneNode *node(NodeId id) const;
private:
    /// Ids are monotonic per scene and never reused.
    NodeId track(const Node &n);


    Ogre::Root         *mRoot;
    Ogre::SceneManager *mSceneMgr;
    std::string         mName;
    std::string        &mError;
    std::map<NodeId, Node> mNodes;
    /// Reverse index Ogre node id -> registry id, for the one question the
    /// visibility cascade has to ask of every node it walks ("is this one of
    /// ours, and what was it told?"). Keyed by Ogre's never-reused id rather
    /// than the node address, which Ogre recycles.
    std::unordered_map<Ogre::IdType, NodeId> mNodeByOgreId;
    std::map<MeshId, MeshRec> mMeshes;
    std::map<std::string, RigRec> mRigs;
    std::map<NodeId, NodeClips> mClips;
    std::map<MaterialId, MaterialRec> mMaterials;
    std::map<TextureId, TextureRec> mTextures;
    /// Dedup index over mTextures: loadTexture and loadDecalTexture both used
    /// to LINEAR-SCAN the whole table on every call, which is quadratic across
    /// a scene open (a few hundred maps -> tens of thousands of string
    /// compares). Key = textureKey(): the bare path for ordinary textures, a
    /// kind-prefixed path for decal-atlas slices, because the two live in
    /// different pools and handing one out for the other is silent corruption
    /// (loadTexture's comment). Kept in step by destroyTexture / destroy().
    std::unordered_map<std::string, TextureId> mTextureIndex;
    /// The index key for a texture record: decal slices are namespaced by kind,
    /// and ordinary textures by COLOUR SPACE.
    ///
    /// The sRGB term is not cosmetic. `loadTexture(path, srgb)` decides the
    /// pixel format from `srgb`, so the FIRST binding of a file used to fix its
    /// colour space for the whole session: bind one image as a base colour and
    /// again as a roughness map and the second read the sRGB texture, silently.
    /// It was latent while every slot had a fixed flag; per-material workflows
    /// (the metallic/specular slot changes colour space with the workflow) and
    /// detail layers (diffuse sRGB, normal linear, same file legal in both)
    /// make it reachable, so the flag is in the key (MATERIAL_GAPS_SPEC §2.3).
    /// Decal slices are unaffected — their pool format is fixed by kind.
    static std::string textureKey(const std::string &path, bool decal, DecalMap kind,
                                  bool srgb);
    /// Registers a texture record: assigns the next id and indexes it by path.
    /// The ONLY way a TextureRec enters mTextures, so the index cannot drift.
    TextureId trackTexture(const TextureRec &rec);
    /// Does this material bind a normal map in any slot (I-6's cheap half)?
    static bool materialUsesNormalMap(const MaterialRec &rec);
    /// Our slot enum -> Ogre's PBSM_* unit.
    static Ogre::PbsTextureTypes pbsSlotOf(PbrTextureSlot slot);
    std::set<std::string> mTextureDirs;
    /// Directories registered as resource locations for particle COLOUR RAMPS
    /// (ADDENDUM A-4). The ColourImage affector loads by NAME through the
    /// resource system, not by path, so its folder has to be a location first —
    /// remembered here so a per-frame particle push does not re-add it.
    std::set<std::string> mParticleRampDirs;
    /// Directories already registered with the resource group for generated
    /// shader pieces (HLMS_ADOPTION P5). One entry in practice — the per-user
    /// piece cache — but the set keeps re-registration cheap and idempotent.
    std::set<std::string> mPieceDirs;
    /// The scene's shader clock in seconds (HLMS_ADOPTION P5). Mirrored into
    /// FogHlmsListener's per-SceneManager table, which is what the render
    /// thread reads; this copy exists so shaderTime() can answer without
    /// touching render-thread state.
    float mShaderTime = 0.0f;
    /// SceneManager::getSkyMethod() never reflects the method actually set
    /// (upstream's setSky forgets to assign mSkyMethod), so remember it.
    bool              mSkyIsEquirect = false;
    /// Textures WE own for Ogre's sky renderable (the equirect Type2DArray copy /
    /// the converted cube). Null when the sky uses a host texture directly.
    Ogre::TextureGpu *mSkyOwnedTex = nullptr;
    /// The un-prefiltered cube the ibl_specular pass convolves into mReflectionTex.
    /// May alias mSkyOwnedTex (a cubemap sky is its own IBL source); mIblSourceOwned
    /// says whether it is ours to destroy.
    Ogre::TextureGpu *mIblSourceTex = nullptr;
    bool              mIblSourceOwned = false;
    bool              mIblPending = false;   // convolve on the next frame
    /// One-shot ibl_specular workspace; kept null between runs.
    Ogre::Camera *mIblCamera = nullptr;
    Ogre::InstantRadiosity *mInstantRadiosity = nullptr;   // owned; null unless IR mode
    // VCT arm (null unless a VCT mode is live). Teardown order within the arm:
    // unbind HlmsPbs -> PCC -> VctLighting -> VctVoxelizer, all before the
    // SceneManager (probe workspaces and the GI camera live in it).
    Ogre::VctVoxelizer               *mVctVoxelizer = nullptr;
    Ogre::VctLighting                *mVctLighting  = nullptr;
    Ogre::ParallaxCorrectedCubemapAuto *mPcc        = nullptr;
    Ogre::Camera                     *mGiCamera     = nullptr;   // PCC build + tracking
    /// The DDGI field, owned, null unless GiParams::ddgi resolved on over a
    /// live VCT arm. Dies BEFORE mVctLighting (it holds that pointer). Held
    /// DERIVED-typed: ~IrradianceField is non-virtual (OGRE_UPSTREAM_ISSUES),
    /// and the derived class is what re-sources the field in place.
    JahIrradianceField               *mIfd          = nullptr;
    /// What is feeding the probes (GiStatus::ifdSource): Raster only once the
    /// field has been re-sourced to the raster workspace, Voxel otherwise.
    GiSource                          mIfdSource    = GiSource::Voxel;
    /// The raster camera's far plane in world units (misses store exactly it,
    /// scaled per axis by probes-per-unit); 0 while the source is voxel.
    float                             mIfdRasterFar = 0.0f;
    /// The rig-activity epoch the raster field last converged against
    /// (mRigPoseEpoch below moves whenever a pose or clip time is pushed).
    unsigned long long                mIfdRigEpochSeen = 0;
    /// True once scanGiMovement ran this frame (the raster re-arm and the probe
    /// budget both consume mGiMovedBoxes; whoever runs first scans).
    bool                              mGiMovementScanned = false;
    /// Convergence bookkeeping. `IrradianceField` counts processed probes
    /// internally and exposes nothing, so the engine keeps its own count —
    /// which it needs anyway to know when a re-converge has finished and to
    /// keep every dispatch a whole multiple of the batch size (the last batch
    /// of an uneven split is where the zero-work-group abort lives).
    Ogre::uint32                      mIfdTotalProbes     = 0;
    Ogre::uint32                      mIfdProbesDone      = 0;
    Ogre::uint32                      mIfdProbesPerFrame  = 0;
    /// The smallest batch that still dispatches at least one compute work
    /// group. Below it, `HlmsCompute::compileShader` throws at frame time and
    /// nothing catches it (spike §4) — so it is a floor the engine enforces,
    /// not a number it reports.
    Ogre::uint32                      mIfdMinProbes       = 0;
    bool mRefractionsActive = false;   // see setRefractionsActive
    bool mGiCachesDirty = false;   // mesh/texture/material died while GI live; flush at frame time
    GiParams         mGi;                                  // last applied GI state
    /// What the last (re)build ACTUALLY used, recorded rather than recomputed:
    /// giStatus() must report the volume the renderer is using, not the one the
    /// scene would resolve to if it rebuilt right now (geometry moves between
    /// rebuilds — that is the whole point of the refresh verb). Equal corners =
    /// nothing built.
    Ogre::Aabb mGiLitVolume    = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    Ogre::Aabb mGiProbeRegion  = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    /// The last volume the AUTO fit resolved (never a hand-typed one), kept
    /// across rebuilds for two jobs that both need "what is lit right now":
    /// giItemBounds' hysteresis floor — an item this box covered is never
    /// trimmed by the outlier ramp, which is what makes adding an object
    /// incapable of collapsing a live scene's volume — and giEscapeSignature,
    /// which arms the mirror's debounced re-fit when an item leaves it. Invalid
    /// until the first successful auto rebuild; cleared whenever the user takes
    /// over with explicit bounds or switches GI off.
    Ogre::Aabb mGiAutoVolume   = Ogre::Aabb(Ogre::Vector3::ZERO, Ogre::Vector3::ZERO);
    bool       mGiAutoVolumeValid = false;
    /// How many GI items the last giItemBounds() call saw. Read by
    /// noteGiAutoVolume: a fit over fewer than two items is not a population,
    /// and must not arm the hysteresis floor.
    mutable size_t mGiLastItemCount = 0;
    /// The scene's ambient hemisphere pair in RADIANCE units — what VctLighting
    /// wants, which is NOT what setAmbient hands the SH path in the flat case
    /// (that one carries HlmsPbs' 1/pi). [0] = upper, [1] = lower.
    Colour     mAmbientRadiance[2] = { Colour(0, 0, 0, 1), Colour(0, 0, 0, 1) };
    /// The Forward+ depth-slice range currently in force, and the frame counter
    /// that rate-limits re-deriving it (fix 8). Seeded with the values
    /// createScene passes to setForwardClustered.
    float      mFwdPlusMin = 2.0f;
    float      mFwdPlusMax = 50.0f;
    unsigned   mFwdPlusTick = 0;
    /// The Forward+ per-cell CUBEMAP PROBE budget currently in force
    /// (kCubemapProbeSlotsDefault above says why it moves). Grows with the
    /// probe grid, never shrinks while the scene lives.
    Ogre::uint32 mCubemapProbeSlots = kCubemapProbeSlotsDefault;
    /// How many probes the region clamp had to correct at the last buildPcc —
    /// i.e. how many times the 1x1 averaged-depth shrink-fit came back with a
    /// box that was not inside the space the grid was fitted to. Reported by
    /// giStatus, because "the fit is degenerate in this scene" is a fact about
    /// the scene the author can act on (GiStatus::probesClampedToRegion).
    int mProbesClampedToRegion = 0;
    /// THE LAMP-MAP CACHE'S MEMORY (collectShadowCacheFrame). Per caster: its
    /// last seen world box, Item, pose epoch and render channels; per lamp: its
    /// last seen shadow key (parameters + pose). A change is "different from
    /// what was last seen", so a scene that is not drawn for a while is caught
    /// up in full on the first frame it is drawn again.
    struct ShadowCasterRec {
        Ogre::Aabb          box;
        const Ogre::Item   *item = nullptr;
        unsigned long long  pose = 0;
        Ogre::uint32        channels = 0;
        unsigned            stamp = 0;
        bool                present = false;
    };
    std::unordered_map<NodeId, ShadowCasterRec> mShadowCasters;
    std::unordered_map<NodeId, unsigned long long> mShadowLightKeys;
    unsigned mShadowScanStamp = 0;
    bool     mShadowScanPrimed = false;
    /// Re-render every cached map (dirtyAllShadowMaps). Starts TRUE: a scene's
    /// first cached frame renders everything, which the fresh slot assignment
    /// does anyway.
    bool     mShadowDirtyAll = true;
    /// THE LIGHT INDEX. Node ids that currently carry an Ogre::Light, kept so
    /// the two per-frame shadow walks (countLocalShadowCasters and
    /// collectShadowCacheFrame) iterate LIGHTS instead of every node in the scene.
    /// It is maintained where lights are born and die — setLight, removeLight,
    /// releaseNode — and a stale entry is tolerated by both readers (they skip
    /// a node whose light is gone), so it can never be worse than a hint.
    ///
    /// It exists because the walks were measurable: with them iterating mNodes,
    /// gi.coalesce's 60-frame light drag failed 3 runs in 6 (the wall-clock
    /// re-inject debounce shifted); with the index it passes, like the base.
    std::vector<NodeId> mLightNodes;
    /// Same contract for the two probe-capture options (P3a/P3b): what the last
    /// buildPcc RESOLVED, after GiToggle::Auto consulted the quality dial and
    /// after the shadow half checked that a shadow node exists to recalculate.
    bool mPccHdr      = false;
    bool mPccShadowed = false;
    /// The raster IrradianceField's one workspace names the probe shadow node
    /// (dropGiForShadowRebuild must tear the field down before an atlas rebuild).
    bool mIfdShadowed = false;
    /// THE PROBE CACHE'S STALE SET (FIX WAVE B2; ENGINE_CACHE_POLICY_SPEC P1).
    /// One entry per probe, rebuilt with the grid. `sweepPending` is true while
    /// the probe is STALE — owes a capture because an input changed
    /// (staleProbeGrid) — and the budget spends only on those, which is what
    /// makes "every probe within ceil(probes / budget) frames of a change" a
    /// guarantee and a still scene free. (It no longer refills when it empties:
    /// that refill was the forever-sweep P1 removed.) `framesSinceUpdate` and
    /// the moved-cover test only decide the ORDER of the catch-up.
    struct ProbeSlot {
        bool     sweepPending = true;
        unsigned framesSinceUpdate = 0;
    };
    std::vector<ProbeSlot> mProbeSlots;
    /// The resolved per-frame budget (the request clamped to the grid) — the
    /// CEILING a frame may spend on stale probes. Reported by giStatus; what a
    /// frame actually spent is mProbeCapturesLastFrame.
    int mProbeUpdatesPerFrame = 0;
    /// The resolved `GiParams::dynamicProbes` reservation and how many extra
    /// moved-covering re-captures it spent on the last frame (Epic's column;
    /// see updateProbeBudget). Both reported by giStatus.
    int mDynamicProbes = 0;
    int mDynamicProbeUpdates = 0;
    /// Whether the last full refresh took the reuse arm (B4). Reported by
    /// giStatus; cleared by every from-scratch build.
    bool mGiReusedLastRefresh = false;
    /// World AABBs of the GI items as of the last movement scan, keyed by node.
    /// The scan is what feeds the "covers a moved AABB" term of the round-robin
    /// priority and the movement half of the GI signature (B3).
    std::unordered_map<NodeId, Ogre::Aabb> mGiItemAabbs;
    /// The AABBs that moved on the most recent scan (union of each mover's old
    /// and new box), in world space. Rebuilt every scan; empty when still.
    std::vector<Ogre::Aabb> mGiMovedBoxes;
    /// A GI item was seen for the FIRST time by a scan after the first one — a
    /// new object arrived. Not a move (the dynamic reservation ignores it, as it
    /// always did) but it is an input the probes must see (P1): the next budget
    /// pass stales the grid with reason Moved and clears it.
    bool mGiItemsAppeared = false;
    /// PROBE-ONLY items (P7): unlit geometry the probe faces capture
    /// (probeSeesItem) but that is not GI geometry — tracked by the same scan
    /// in their own map, so they stale the probes when they move or arrive and
    /// never enter mGiMovedBoxes (the dynamic reservation, the raster field's
    /// re-arm) nor giGeometrySignature (the voxel re-solve).
    std::unordered_map<NodeId, Ogre::Aabb> mProbeOnlyAabbs;
    bool mProbeOnlyChanged = false;
    /// The movement scan has run at least once: before that, every item is
    /// seen for the first time and none of them is an arrival.
    bool mGiScannedOnce = false;
    /// Bumped by setBonePoses and by setClipStates when a clip's time or
    /// enable changed: a rig posed in place moves no AABB (Items keep their
    /// bind-pose bounds), so the movement scan cannot see it, and this is what
    /// the raster field re-arms on instead.
    unsigned long long mRigPoseEpoch = 0;
    /// Bumped whenever anything the GI arms hold RAW POINTERS INTO may have
    /// died — every invalidateGiCaches call site (B4). The reuse arm refuses to
    /// re-run an existing voxelizer across a bump, which is what keeps the
    /// "always from scratch" rule's guarantee (InstantRadiosity::freeMemory's
    /// cache keys, VctMaterial's datablock-pointer cache) exactly as strong.
    unsigned long long mGiDestroyGeneration = 0;
    unsigned long long mGiBuiltGeneration   = ~0ull;   // no build yet
    /// THE MATERIAL GENERATION (ENGINE_CACHE_POLICY_SPEC P7). Bumped when a
    /// parameter the VOXELIZER reads (albedo, emissive, alpha, workflow, the
    /// albedo/emissive maps) changes on a material that GI geometry uses.
    /// VctMaterial converts each datablock ONCE and caches the result by
    /// pointer for the voxelizer's lifetime (OgreVctMaterial.cpp addDatablock:
    /// a cache hit never re-reads the colour), so the reuse arm would re-voxelize
    /// the OLD albedo for ever. refreshVctFast compares the generation it built
    /// the voxel arm at and, when it moved, builds a FRESH voxelizer and
    /// lighting under the probes it keeps (freshVoxelArm). Reported as its
    /// own term (giMaterialSignature), so the host's debounce coalesces a
    /// slider drag into one re-voxelize when it stops WITHOUT running the
    /// light re-inject cadence a material cannot need.
    unsigned long long mGiMaterialGeneration      = 0;
    unsigned long long mGiBuiltMaterialGeneration = 0;
    /// THE PROBE CACHE's bookkeeping (ENGINE_CACHE_POLICY_SPEC P1). See
    /// staleProbeGrid and GiStatus: why the grid was last staled, a serial per
    /// stale event, the captures the last rendered frame actually made, the
    /// captures a from-scratch placement made this frame (they bypass mDirty),
    /// and how many from-scratch builds the scene has had.
    GiStaleReason      mLastStaleReason = GiStaleReason::None;
    unsigned long long mStaleSerial = 0;
    int                mProbeCapturesLastFrame = 0;
    int                mPlacementCapturesThisFrame = 0;
    unsigned long long mGiRebuilds = 0;
    /// The PCC/VCT trust window buildPcc bound the grid with, so a binding
    /// re-assert (P10) re-binds with the same numbers without re-deriving them.
    float mPccBindMinDist = 0.0f, mPccBindMaxDist = 0.0f;
    /// The last ambient SH and fog the scene was given, so a host re-push of the
    /// same value (every page return drops the host's own latch) stales nothing.
    float   mLastAmbientSh[27] = {};
    bool    mAmbientShKnown = false;
    FogDesc mLastFogDesc;
    bool    mFogDescKnown = false;
    /// The NodeIds handed to the live VctVoxelizer, so the reuse arm can add the
    /// items created since the build. Cleared with the arm.
    std::vector<NodeId> mVctItemIds;
    /// Live decals in THIS scene. The SceneManager-level atlas binding is
    /// driven off the count (see refreshDecalBindings).
    unsigned            mDecalCount = 0;
    // Planar-reflection arm. mPlanar is null unless mPlanarParams.budget > 0.
    // mReflectors is the DOCUMENT's set of reflector nodes and survives the arm
    // going up and down; mActors only exists while the arm is up.
    planar::JahPlanarReflections *mPlanar = nullptr;
    PlanarReflectionParams   mPlanarParams;
    std::string              mPlanarWorkspaceDef;
    std::vector<std::string> mPlanarNodeDefs;
    std::set<NodeId>         mReflectors;
    std::map<NodeId, Ogre::PlanarReflectionActor *> mActors;
    /// Abandoned particle definitions, keyed by ParticleTopology::key(). They
    /// cannot be destroyed (no such API on ParticleSystemManager2 — defs are
    /// freed only in its destructor, i.e. with the SceneManager), so a released
    /// def is hidden and parked here for the next system with the same shape.
    /// With frozen topologies (one emitter, a fixed affector set, coarse quota
    /// buckets) the pool stays a handful of entries per scene.
    std::map<std::string, std::vector<Ogre::ParticleSystemDef *>> mParticleDefPool;
    /// The Unlit datablock each def is bound to. A def binds its datablock ONCE,
    /// inside init() (OgreParticleSystem2.cpp:251-256) — setMaterialName
    /// afterwards is a no-op for PFX2 defs (mIsRendererConfigured is never true
    /// for them). So the datablock is per-DEF, mutated in place when a node's
    /// texture or blend mode changes, and destroyed only at scene teardown
    /// (datablocks belong to the process-wide HlmsManager, not to the
    /// SceneManager that frees the defs).
    std::map<Ogre::ParticleSystemDef *, std::string> mParticleDatablocks;
    /// Every def this scene ever created, for the def-accumulation measurement
    /// the particle gates print (PARTICLES_FX2_SPEC §3.2). Never shrinks.
    unsigned            mParticleDefsCreated = 0;
    /// The helper overlay queue's one-time setup (kHelperOverlayRenderQueue):
    /// whether the queue mode has been switched on this scene's RenderQueue,
    /// and the invisible entity that keeps the culler reaching that queue
    /// (kQueueDepthAnchorRenderQueue explains why it must exist).
    bool                mHelperOverlayQueueReady = false;
    Ogre::MovableObject *mQueueDepthAnchor = nullptr;
    TextureId           mNextTextureId = 0;
    NodeId              mNextId = 0;
    MeshId              mNextMeshId = 0;
    MaterialId          mNextMaterialId = 0;
};

// ---------------------------------------------------------------------------
class OgreView final : public View {
public:
    /// On-screen: `window` is set. Offscreen: `texture` is set. Never both.
    OgreView(Ogre::Root *root, Ogre::Window *window, Ogre::TextureGpu *texture,
             const std::string &name, unsigned w, unsigned h, const Colour &background,
             std::string &errorSink);
    ~OgreView() override;

    const std::string &name() const override;
    Scene *scene() const override;
    /// The same pointer, unerased — Engine::renderOneFrame groups views by the
    /// scene they draw (the authoritative-view rule on updateGi).
    OgreScene *ogreScene() const { return mScene; }

    bool setScene(Scene *scene) override;
    bool setCameraNode(NodeId node) override;
    NodeId cameraNode() const override { return mCameraNode; }

    /// Unbinds the scene: workspace and camera go, the scene itself survives.
    void detachScene();

    void setCamera(const CameraDesc &c) override;
    void setEnabled(bool on) override;
    Colour background() const override;
    bool shadows() const override;
    void setShadows(bool on) override;
    void setBackground(const Colour &c) override;
    /// The clear colour and the shadow node live in the chain's definitions:
    /// rebuild definitions + workspace, keeping scene, camera and enabled state.
    void rebuildWorkspaceDef();
    static constexpr const char *kShadowNodeName = "JahshakaShadowNode";
    /// The SECOND shadow node, at half the base resolution, used ONLY by the
    /// planar-reflection pass. CompositorShadowNodes are per-workspace and are
    /// constructed eagerly in CompositorPassScene's constructor, so every
    /// reflection slot allocates its own atlas the moment the arm is built:
    /// sharing kShadowNodeName would cost ~56 MB PER SLOT at the default 2048.
    /// Half resolution makes that ~14 MB, and a shadow seen in a mirror is the
    /// last place anyone measures shadow-map resolution.
    static constexpr const char *kReflectShadowNodeName = "JahshakaReflectShadowNode";
    /// The THIRD shadow node: REFLECTION-PROBE CAPTURES ONLY — the PCC probe
    /// workspace (media/Hlms/Jahshaka/JahshakaPcc.compositor, the `Shadows`
    /// twin) and the irradiance-field raster workspace (JahshakaIfdRaster
    /// .compositor). The same PSSM + focused layout as the main atlas at a
    /// QUARTER of its resolution (probeShadowResolution(): 512 at the High
    /// tier's 2048), the derived focused-map count capped at four, and a
    /// scratch cube of R/2 instead of a fixed 1024 (kProbeShadowMaxResolution /
    /// kProbeShadowMaxFocusedMaps above).
    ///
    /// WHY (lane-probeshadow, 2026-09-09, measured with app.textureMemory on
    /// this box): ParallaxCorrectedCubemapAuto gives EVERY probe its own
    /// workspace (OgreCubemapProbe.cpp initWorkspace), and a CompositorShadowNode
    /// is per workspace, so with the probes naming kShadowNodeName an 18-probe
    /// default scene instantiated 19 full atlases (2048x7168 D32 = 56 MB each,
    /// 1064 MB) plus 21 point-light cubes (1024^2 x 6 R32F = 24 MB each,
    /// 504 MB) — 1.6 GB of the 2.05 GB a default scene booted with. The
    /// Showroom sample (32 probes, four focused maps) was 33 x 88 MB + 35 x 24 MB
    /// = 3.7 GB of its 4.46 GB. With this node a probe costs a 512x1792 D32
    /// atlas (3.5 MB, two focused maps; 5.5 MB with four) plus a 256^2 x 6 cube
    /// (1.5 MB): the default scene boots at 700 MB of textures (was 2050), the
    /// Showroom at 1103 MB (was 4463).
    ///
    /// WHY NOT PSSM-ONLY, which would be 6 MB a probe at 1024: the lamps'
    /// shadows ARE visible in reflections. Measured on the Showroom (its three
    /// shadow-casting lamps, 960x540 offscreen shot, mean |diff| per channel /
    /// pixels differing by more than 8 against the full-atlas capture): PSSM-only
    /// 5.4 / 13.8% — the spheres' shadow discs vanish from the neighbouring
    /// chrome; this node 0.34 / 1.7%; and the resolution itself is invisible
    /// (2048 vs 1024 vs 512 PSSM-only captures are byte-identical). The default
    /// scene (no lamp casters) is byte-identical under every variant.
    ///
    /// The cost that remains is shader-side: the probe passes get their own
    /// PBS variants (numShadowMapLights differs from the main view's), compiled
    /// once and disk-cached, exactly like the reflect node's.
    static constexpr const char *kProbeShadowNodeName = "JahshakaProbeShadowNode";

    // ---- The workspace seam (POST_CHAIN_SPEC.md; the planar-reflection lane
    //      depends on it) ---------------------------------------------------
    /// THE one place a CompositorWorkspace is created for this view. Six call
    /// sites used to do it inline (setScene, definition rebuild, shadow-atlas
    /// rebuild, RTT rebuild, window recreate, MSAA change); anything that must
    /// ride a LIVE workspace — a compositor listener, per-view effect state —
    /// would have had to be re-attached in all six. Now it is re-attached here.
    /// Returns true when a workspace exists afterwards.
    bool attachWorkspace();
    /// This view's current chain shape — what the builder is asked for.
    ChainDesc chainDesc() const;
    /// Drops the live workspace (detaching its listeners first). Safe when
    /// there is none; returns whether one was actually dropped.
    bool detachWorkspace();
    /// Compositor listeners this view re-attaches to every workspace it builds.
    /// The view does NOT own them: register at setup, unregister before the
    /// listener dies. Registering twice is a no-op.
    void addWorkspaceListener(Ogre::CompositorWorkspaceListener *l);
    /// Create/register (on) or flush+remove (off) this view's PassProfiler.
    void setProfiling(bool on);
    void removeWorkspaceListener(Ogre::CompositorWorkspaceListener *l);
    /// Counts completed workspace attachments. Neutral introspection (no Ogre
    /// type crosses the boundary) that lets hosts and tests see that a call was
    /// or was not structurally expensive — every rebuild goes through the seam,
    /// so this is exactly the number of times it ran.
    unsigned workspaceGeneration() const override;

    unsigned long long framesPresented() const override;
    bool warmUpShaders() override;
    /// Called by OgreEngine::renderOneFrame AFTER Root::renderOneFrame: counts
    /// this frame if the view was actually part of it (enabled + workspace +
    /// scene). The one place mFramesPresented moves up.
    void notePresented();

    void setPostFx(const PostFxDesc &fx) override;
    const PostFxDesc &postFx() const override;
    void resetExposureHistory() override;

    void setOverlay(const ViewOverlayDesc &d) override;
    const ViewOverlayDesc &overlay() const override;

    // ---- The picture-in-picture inset (CAMERAS_SPEC §7.7) ------------------
    void setPip(const ViewPipDesc &d) override;
    const ViewPipDesc &pip() const override;
    unsigned pipGeneration() const override;
    /// Is this view ENTITLED to draw an inset at all? On-screen always;
    /// offscreen only with ViewPipDesc::allowOffscreen. The determinism law's
    /// single gate, in the same shape as overlaysAllowed() and chainDesc()'s
    /// post-fx early-out.
    bool pipAllowed() const;
    /// Whether the inset actually TONEMAPS: what the host asked for AND whether
    /// this view's own chain grades at all (see the note at the definition).
    bool pipTonemapEffective() const;
    /// Is this view ENTITLED to draw the process HUD at all? On-screen always;
    /// offscreen only with ViewOverlayDesc::allowOffscreen. Decided in the same
    /// one place as the post chain's offscreen guarantee (chainDesc()), and
    /// read by OgreEngine::renderOneFrame when it picks the frame's HUD owner.
    bool overlaysAllowed() const;
    /// The camera the chain's per-frame globals are computed from.
    Ogre::Camera *camera() const { return mCamera; }
    /// Shadow-atlas rebuild support (Engine::setShadowResolution): the shadow node
    /// DEFINITION cannot be replaced while any workspace instantiates it, so the
    /// engine first drops every shadowed view's workspace (true = dropped, caller
    /// re-adds), then swaps the definition, then calls the restore below.
    bool dropWorkspaceForShadowRebuild();
    void recreateWorkspaceAfterShadowRebuild();
    /// This view's LIVE shadow-node instance, or null when it has no workspace,
    /// no shadows, or the workspace has not instantiated the node yet.
    ///
    /// It is per WORKSPACE, not per definition (CompositorShadowNode holds the
    /// fixed-light table and the static-map dirty flags), which is exactly why
    /// anything that assigns cached shadow maps has to reach every view that
    /// draws — see OgreEngine::applyShadowCacheDirties.
    Ogre::CompositorShadowNode *shadowNodeInstance() const;
    bool isEnabled() const override;
    unsigned width()  const override;
    unsigned height() const override;
    bool isOffscreen() const override;
    /// The presenting window, or null for an offscreen view. The engine needs
    /// it for the one decision that is process-wide but lives on the window:
    /// vsync (Engine::setVsync).
    Ogre::Window *ogreWindow() const { return mWindow; }

    /// Set by the engine for on-screen views ON X11: creates a fresh Ogre window on the
    /// same native handle at the given size and MSAA sample count (the "FSAA" misc param —
    /// BOTH window-creation sites must pass it or MSAA silently resets on resize).
    /// EMPTY on macOS: VulkanMetalWindow resizes its own surface (see applyPendingResize).
    std::function<Ogre::Window *(unsigned, unsigned, unsigned)> mCreateWindow;
    unsigned mPendingW = 0, mPendingH = 0;
    /// Runtime MSAA change is structurally a resize (Vulkan has no runtime
    /// setFsaa): non-zero = recreate the target at mRequestedSamples next
    /// applyPendingResize, even at the same size.
    unsigned mPendingSamples = 0;
    /// What the host asked for (sanitized). The engine initialises it for
    /// on-screen views from EngineConfig::sampleCount; offscreen views start
    /// at 1 (pixel-asserted readbacks stay exact unless a test opts in).
    unsigned mRequestedSamples = 1;

    void setSampleCount(unsigned samples) override;
    unsigned sampleCount() const override;

    /// On-screen resize is applied at frame time (applyPendingResize): by then Qt has
    /// resized the native window, and doing it once per frame coalesces layout bursts.
    void resize(unsigned w, unsigned h) override;

    bool readPixels(Image &out) override;

    /// Applies whatever resize()/setSampleCount() recorded, at frame time.
    ///
    /// THREE paths, and which one runs is decided here:
    ///  * No mCreateWindow hook (macOS): requestResolution/setFsaa in place —
    ///    VulkanMetalWindow rebuilds its own swapchain (colour AND depth).
    ///  * Size change on X11: Ogre::Window::windowMovedOrResized() — the window
    ///    re-reads its native geometry, stalls the device and rebuilds the
    ///    swapchain including the depth buffer. NOT a recreate. (Before this,
    ///    the size branch recreated the whole window, and — because resize()
    ///    eagerly updated mWidth/mHeight — never actually ran: resizes worked
    ///    only through Ogre's OUT_OF_DATE self-heal.)
    ///  * MSAA change on X11: the window IS recreated on the same native handle,
    ///    because Vulkan/XCB has no setFsaa. The device is stalled first.
    void applyPendingResize();
    /// Full GPU stall through the public VaoManager contract. Required before
    /// destroying a render window: the swapchain's acquire semaphore is
    /// destroyed outright (VulkanVaoManager::notifySemaphoreUnused →
    /// vkDestroySemaphore) and every on-screen window holds an acquired image
    /// at a frame boundary.
    void stallDevice();
    /// Feeds the camera position to the scene's particle manager: billboard uploads
    /// are depth-sorted against it (matters for alpha-blended sets).
    void updateParticles();
    /// Feeds the camera position to the scene's GI (PCC probe blending tracks
    /// the viewer in hybrid mode) and re-derives the Forward+ depth range.
    ///
    /// AUTHORITATIVE VIEWS ONLY (FIX WAVE B2/F7): both jobs are per SCENE, not
    /// per view, and both are stateful — the probe round-robin spends a
    /// per-frame budget and the Forward+ range carries hysteresis. Two views
    /// sharing a scene (the editor and the player, an editor and a preview) used
    /// to run this twice a frame with two different camera positions, which
    /// spends the budget twice and makes the priority depend on view order.
    /// Engine::renderOneFrame picks ONE view per scene — the first enabled
    /// on-screen one, falling back to the first enabled — and calls this only
    /// there.
    void updateGi();
    /// Per-frame maintenance of everything derived from the TARGET's size:
    /// the letterbox rectangle and the inset's. Free when neither is in use.
    void applyLetterboxAndPip();
    /// Pushes PostFxDesc::exposure into the FIXED tonemap's clear (§14). Live;
    /// never a rebuild, and a no-op unless the chain has the fixed form.
    void applyFixedExposure();
    /// Writes the letterbox's inner rectangle onto the chain's inset passes and
    /// the bar/background colours (CAMERAS_SPEC §7.4). Live; never a rebuild.
    void applyLetterbox();
    /// Converts the camera's FRACTION-OF-FRAME lens shift into Ogre's
    /// world-space frustum offset and applies it (CAMERA_LENS_SPEC §3).
    /// Re-derived every time because it depends on the fov, the near distance
    /// AND the aspect the view is currently rendering at — so it runs from
    /// setCamera AND once a frame, where a resize is noticed. Free (one
    /// setFrustumOffset of zero) when nothing is shifted.
    void applyLensShift(Ogre::Camera *camera, const CameraDesc &c, float aspect);
    /// The aspect this view actually projects at: the authored one when the
    /// camera constrains it, the TARGET's otherwise (which is what Ogre's auto
    /// aspect ratio arrives at, one frame sooner).
    float viewAspect(const CameraDesc &c) const;
    /// Pushes the inset's camera state and its two rectangles. LIVE: the
    /// workspace's viewport modifier and the scene pass's own mVpRect are both
    /// read per frame by Ogre, so moving or letterboxing the inset rebuilds
    /// nothing. Called once a frame (the rects depend on the TARGET's aspect,
    /// which a resize changes without the host touching ViewPipDesc) and from
    /// setPip. Free when there is no inset.
    void applyPip();
    /// Arms or disarms this view's planar-reflection compositor listener to
    /// match the bound scene's current state, and re-points it at the current
    /// camera and PlanarReflections instance. Cheap and idempotent; called once
    /// a frame, because BOTH ends move (the scene rebuilds its arm on any
    /// parameter change, the view recreates its camera on every setScene).
    /// Registration itself rides addWorkspaceListener, so the listener survives
    /// every workspace rebuild — that is the seam this feature was waiting for.
    void syncPlanarListener();
    /// Arms or disarms this view's POST-CHAIN GLOBALS listener (CAMERA_LENS_SPEC
    /// §4). Armed while the view is enabled and its chain has any effect, so
    /// the view pushes ITS OWN exposure / bloom threshold / AO projection to
    /// its own workspace's passes; disarmed otherwise, which is what makes a
    /// view with no chain cost exactly nothing. Same idempotent once-a-frame
    /// contract as syncPlanarListener, and registered through the same seam.
    void syncGlobalsListener();
    /// Releases workspace, camera, workspace definitions and the window/texture.
    /// Safe to call twice. Called by Engine::destroyView and by the Engine
    /// destructor BEFORE Root dies.
    void destroy();

    /// `samples` > 1 asks for an implicit-resolve MSAA target: the sample
    /// description MUST be set before scheduleTransitionTo(Resident) (Ogre
    /// asserts OnStorage); the achieved count is validated at the transition.
    static Ogre::TextureGpu *createRtt(Ogre::Root *root, const std::string &name,
                                       unsigned w, unsigned h, unsigned samples = 1);
    /// Rounds down to a power of two and clamps to [1, 16] — what the backend
    /// will even ask the driver for (the driver may still clamp further).
    static unsigned sanitizeSamples(unsigned samples);

private:
    /// Offscreen only: replaces the RTT (which cannot change in place) at the
    /// given size and mRequestedSamples, re-adding the workspace. The shared
    /// tail of resize() and setSampleCount().
    void rebuildRtt(unsigned w, unsigned h);
    Ogre::TextureGpu *target() const;

    // ---- PiP internals (CAMERAS_SPEC §7.7) --------------------------------
    /// Brings the inset's workspace into line with mPip + pipAllowed(): builds
    /// or tears it down, and re-appends it AFTER the main workspace. Called
    /// from setPip and from attachWorkspace — the latter is the "re-assert the
    /// order after every rebuild" rule (there is no reorder API, and
    /// attachWorkspace always appends, so a main-workspace rebuild would
    /// otherwise draw straight over the inset).
    void syncPip();
    /// Tears down workspace, definitions, datablock and camera, in that order.
    /// The camera goes LAST: a pass holds a raw Camera* and destroying it first
    /// segfaults on the next frame (spike T6).
    void destroyPip();
    /// The inset's LOCAL texture size as FRACTIONS of the target (Route C).
    /// Derived from the inner rect — i.e. from the requested rectangle and, if
    /// the camera constrains its aspect, from the letterbox inside it — so the
    /// texture has the shape the composite quad will stretch it into and a
    /// square in the world stays square in the inset.
    void pipTexFactors(float &widthFactor, float &heightFactor) const;


    Ogre::Root                *mRoot;
    Ogre::Window              *mWindow;
    Ogre::TextureGpu          *mTexture;
    Ogre::Camera              *mCamera    = nullptr;
    Ogre::CompositorWorkspace *mWorkspace = nullptr;
    OgreScene                 *mScene     = nullptr;
    std::string                mName, mWorkspaceDef;
    /// Every node definition the chain builder made for this view, in creation
    /// order. Was a single std::string while the chain was one node — a
    /// multi-node chain that removes one definition leaks the rest across view
    /// recreation (test_engine_recreate is where that shows up).
    std::vector<std::string>   mNodeDefs;
    std::vector<Ogre::CompositorWorkspaceListener *> mWorkspaceListeners;
    /// Owned; registered through addWorkspaceListener while the bound scene has
    /// planar reflections armed. Null until the first frame that needs it.
    std::unique_ptr<planar::WorkspaceListener> mPlanarListener;
    /// Owned; registered the same way while this view's chain has effects
    /// (CAMERA_LENS_SPEC §4). Null on a passthrough view — every thumbnail,
    /// preview and pixel suite, by construction.
    std::unique_ptr<chain::ViewGlobalsListener> mGlobalsListener;
    /// The opt-in pass profiler (chain::PassProfiler); null unless profiling.
    std::unique_ptr<chain::PassProfiler> mProfiler;
    unsigned                   mWorkspaceGeneration = 0;
    /// Frames drawn+presented since the current scene was bound (see
    /// View::framesPresented). Reset by setScene/detachScene, NOT by a
    /// workspace rebuild.
    unsigned long long         mFramesPresented = 0;
    /// What the host asked for. Offscreen views keep it and ignore it.
    PostFxDesc                 mPostFx;
    /// Ditto for the engine-drawn overlay (STATS_OVERLAY_SPEC §5.1).
    ViewOverlayDesc            mOverlay;
    /// Ditto for the picture-in-picture inset (CAMERAS_SPEC §7.7).
    ViewPipDesc                mPip;
    /// The inset's second workspace on THIS view's target, its POOLED camera
    /// (created once, reused for the life of the bound scene — Forward+ caches
    /// light grids on the raw Camera* with a 3-frame TTL, so churning cameras
    /// per frame is the VctMaterial aliasing class of bug), and the definitions
    /// and handles chain::buildPip made.
    Ogre::CompositorWorkspace *mPipWorkspace = nullptr;
    Ogre::Camera              *mPipCamera    = nullptr;
    std::string                mPipWorkspaceDef;
    std::vector<std::string>   mPipNodeDefs;
    chain::PipHandles          mPipHandles;
    /// The fractions the inset's local texture was BUILT with, and the shape
    /// flag it was built for. applyPip compares the size these produce at the
    /// current target size against the size the current rect wants, IN WHOLE
    /// PIXELS: equal means the texture is already right, which is what keeps a
    /// steady inset (and a plain window resize — the texture is a fraction, so
    /// it follows one for free) from rebuilding anything per frame.
    float                      mPipTexWidthFactor = 0.0f, mPipTexHeightFactor = 0.0f;
    bool                       mPipTexTonemap = false;
    /// The TARGET size the inset was built against. A change means the window
    /// resized, and the inset is rebuilt for it — see applyPip for the
    /// validation error that made this necessary rather than tidy.
    unsigned                   mPipTargetW = 0, mPipTargetH = 0;
    /// See View::pipGeneration.
    unsigned                   mPipGeneration = 0;
    /// What chain::build handed back for THIS view's main chain — today the
    /// letterbox's inset passes and its background swatch.
    chain::ChainHandles        mChainHandles;
    /// The last CameraDesc a host pushed. Kept because two things derive from
    /// it beyond the Ogre camera itself: the letterbox flag (a graph change)
    /// and its rectangle (re-derived on every resize).
    CameraDesc                 mCameraDesc;
    /// The scene node the camera RIDES (setCameraNode, AVATAR_RIG_PERF_SPEC
    /// §4.6). 0 = the camera is positioned from mCameraDesc, which is what
    /// every view has done since step 5.
    NodeId                     mCameraNode = 0;
    unsigned                   mWidth, mHeight;
    Colour                     mBackground;
    bool                       mEnabled = true;
    bool                       mShadows = false;
    std::string               &mError;
};

// ---------------------------------------------------------------------------
class OgreEngine final : public Engine {
public:
    bool init(const EngineConfig &cfg, std::string &error);

    Scene *createScene(const std::string &name, unsigned workerThreads = 0) override;
    void *documentGraphScene() override;
    bool  isHeadless() const override { return mHeadless; }

    void destroyScene(Scene *scene) override;

    View *createView(const std::string &name,
                     NativeWindowHandle handle, unsigned width, unsigned height,
                     const Colour &background) override;

    View *createOffscreenView(const std::string &name, unsigned width, unsigned height,
                              const Colour &background) override;

    void destroyView(View *view) override;

    void renderOneFrame() override;
    bool updateScene(Scene *scene) override;
    bool hasEnabledViews() const override;
    void listViews(std::vector<View *> &out) const override;

    // Texture streaming (THREADING_ADOPTION_SPEC.md P2) — all five are one call
    // into TextureGpuManager, which belongs to the render system and is
    // therefore process-wide, not per scene.
    bool texturesDoneStreaming() const override;
    double waitForTextureLoads() override;
    unsigned long long textureLoadRequests() const override;
    unsigned textureMultiLoadThreads() const override;
    unsigned textureWaitTimeouts() const override { return mTextureWaitTimeouts; }
    double textureWaitWorstMs() const override { return mTextureWaitWorstMs; }
    unsigned textureWaitBudgetMs() const override { return mTextureWaitBudgetMs; }
    unsigned textureMetadataCacheEntries() const override;
    unsigned textureChannelCacheEntries() const override;
    bool saveTextureCache() override;

    /// Applies to every on-screen window that exists AND is remembered for the
    /// ones created later (createView and the MSAA-recreate hook both read it).
    void setVsync(bool on) override;
    bool vsync() const override { return mVsync; }
    const std::string &lastError() const override;
    std::string takeLastError() override;

    /// PROCESS-WIDE: rides Ogre's single frame-time controller value
    /// (ControllerManager -> FrameTimeControllerValue), held in its
    /// frame-delay mode for the whole process (setFrameDelay zeroes the time
    /// factor, so the wall clock never gets back in).
    void  setFixedFrameDelta(float seconds) override;
    float fixedFrameDelta() const override;

    /// GLOBAL by construction: HlmsPbs keeps ONE ShadowFilter for every shadowed
    /// light in every scene (HlmsPbs::mShadowFilter). The filter properties are
    /// evaluated in preparePassHash each pass, so changing it at runtime takes
    /// effect next frame — no datablock or workspace rebuild.
    void setShadowFilter(ShadowFilter f) override;
    ShadowFilter shadowFilter() const override;

    /// GLOBAL like the filter, but NOT cheap: the sizes live in the shadow-node
    /// DEFINITION, which cannot change while any workspace instantiates it. So:
    /// drop every shadowed view's workspace, swap the definition, re-add them —
    /// the same teardown order the engine's destructor honours.
    void setShadowResolution(unsigned pixels) override;
    unsigned shadowResolution() const override;
    void setShadowMapBudget(unsigned maps) override;
    unsigned shadowMapBudget() const override;
    ShadowStatus shadowStatus() const override;
    bool refreshShadows() override;
    /// THE LAMP-MAP CACHE, frame half one (OgreShadow.cpp; ENGINE_CACHE_POLICY
    /// P2): at the top of the frame, before any per-view work — switches the
    /// clear strategy to per-map quads when the first cacheable lamp appears
    /// (one rebuild of the three node definitions, the Shadow Quality rebuild,
    /// and never back) and disarms the pass counters when nobody asks (P8).
    void applyShadowCache();
    /// Frame half two: AFTER updateSceneGraph, before the workspaces render.
    /// Each drawn scene detects what changed (collectShadowCacheFrame) and the
    /// result is applied to EVERY shadow-node instance that draws it — views,
    /// planar slots, probes: the fixed-light table and the dirty flags are per
    /// INSTANCE (SHADOW_TOOLING_SPEC.md F8). A node caches only while the
    /// lamps fit its maps (the v1 over-budget rule); otherwise it stays on
    /// Ogre's closest-first dynamic sort and shadowStatus says so.
    void applyShadowCacheDirties(const std::vector<OgreScene *> &drawn);
    /// Frame half three: after the frame rendered — the counters' readings.
    void latchShadowCounters();
    /// Called by OgreScene BEFORE it destroys an Ogre::Light: unties it from
    /// every shadow-node instance that holds it fixed (OgreShadow.cpp says why
    /// this cannot wait for the next frame).
    void releaseShadowLamp(OgreScene *scene, Ogre::Light *light);
    /// Unhooks and destroys the shadow-pass counters. Called by ~OgreEngine
    /// BEFORE the views go, so no listener outlives its workspace.
    void detachShadowCounter();
    /// One entry per shadow map of the LIVE shadow node — the atlas inspector's
    /// data (SHADOW_TOOLING_SPEC.md §4.4). Empty when nothing is rendering
    /// shadows. Defined in OgreShadow.cpp, beside the definition it describes.
    std::vector<hud::AtlasTileDesc> collectAtlasTiles() const;
    /// Called by destroyView BEFORE the view dies: unhooks the shadow-pass
    /// counter if it was riding that view. Lives in OgreShadow.cpp because the
    /// counter type is incomplete everywhere else.
    void noteViewDestroyed(OgreView *view);

    /// THE ATLAS REBUILD (OgreShadow.cpp): swaps resolution and/or focused-map
    /// count by dropping every workspace that instantiates the shadow node,
    /// replacing the definitions and re-creating them. Returns true when a
    /// rebuild actually happened.
    bool rebuildShadowAtlas(unsigned resolution, unsigned focusedMaps, bool perMapClears);
    /// The EFFECTIVE budget: what the host asked for (setShadowMapBudget),
    /// clamped to the engine's hard maximum and to what the current
    /// resolution can afford — 16 maps at 1024, 8 at 2048, 4 at 4096, 2 at
    /// 8192 (SHADOW_TOOLING_SPEC.md §4.2's VRAM table; the packer would
    /// otherwise hand back a silently smaller atlas at the big sizes).
    unsigned effectiveShadowMapBudget() const;
    /// Called once per frame from renderOneFrame: counts the shadow-casting
    /// point/spot lights of the scenes being drawn, steps the allocation up
    /// {2,4,8,16} and rebuilds the atlas when it has to grow. NEVER shrinks
    /// within a session (owner decision D4) and never runs on a frame that
    /// would be the first of a burst — see the definition.
    void deriveShadowMapCount();
    /// What the atlas currently HAS: `mShadowMapCount` focused maps at
    /// `mShadowResolution`. Read by shadowStatus() and by the derivation.
    unsigned shadowMapCount() const { return mShadowMapCount; }


    /// Ogre::Mesh::msOptimizeForShadowMapping — a plain process-wide static,
    /// read by buildMeshV2 when it decides whether to give a mesh its own
    /// position-only shadow VAOs (POST_CHAIN_SPEC.md §11).
    void setShadowMeshOptimization(bool on) override;
    bool shadowMeshOptimization() const override;

    /// The persistent shader cache (SHADER_CACHE_SPEC.md). Loaded inside the
    /// first createView() -> ensureHlms(); saved on clean teardown and whenever
    /// the host says a compile burst has settled.
    bool recordWarmUpSet(Scene * = nullptr) override;
    bool saveWarmUpSet(const std::string &file) override;
    unsigned applyWarmUpSet(const std::string &file, Scene * = nullptr) override;

    ShaderCacheStats shaderCacheStats() const override;
    // The log bridge (SESSION_LOG_SPEC F3-B) — OgreLogBridge.cpp.
    void setLogSink(Engine::LogSink sink) override;
    DeviceInfo deviceInfo() const override;
    /// Attaches/detaches the Ogre LogListener. attach() runs immediately after
    /// `new Ogre::Root` (beside mShaderCache.attachCounters) so plugin loads,
    /// render-system init and device detection are all captured; detach() MUST
    /// run before Root is deleted.
    void attachLogBridge();
    void detachLogBridge();

    bool renderStats(RenderStats &out) const override;
    bool objectCounts(ObjectCounts &out) const override;
    bool threading(EngineThreading &out) const override;
    bool memoryStats(MemoryStats &out) const override;
    bool textureMemory(std::vector<TextureMemoryEntry> &out) const override;
    bool reclaimMemory(MemoryStats *before, MemoryStats *after) override;
    void setProfiling(bool on) override;
    bool profiling() const override { return mProfiling; }
    bool saveShaderCache() override;
    bool clearShaderCache() override;
    void shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                             unsigned &expected) const override;

    ~OgreEngine() override;

private:
#ifdef __linux__
    /// The exact layout Ogre's Vulkan/XCB backend reads out of the "SDL2x11"
    /// misc param (`struct SDLx11 { Display *display; ::Window window; }`,
    /// OgreVulkanXcbWindow.cpp) — spelled without Xlib so this header stays
    /// X11-free. `Display*` is an opaque pointer and `Window` is `XID` =
    /// `unsigned long`; same sizes, same alignments, same order.
    struct X11Handle { void *display; unsigned long window; };
#endif

    bool viewNameTaken(const std::string &name);

    /// The scenes an ENABLED View draws — the one definition of "takes part in
    /// this frame" (THREADING_ADOPTION_SPEC.md P3). renderOneFrame updates
    /// exactly these and nothing else, which is both the idle-cost gate and the
    /// structural reason the render thread never walks a staging manager the
    /// import worker is growing. Cheap enough to call once per frame.
    void scenesFeedingEnabledViews(std::vector<OgreScene *> &out) const;

    /// First render target: the VaoManager now exists, so Hlms can be registered.
    void ensureHlms();
    /// Ogre's low-level material scripts (sky quad, DPSM shadow maps, depth utils).
    /// Staged from Samples/Media/2.0/scripts/materials/Common next to the Hlms data.
    void registerCommonMaterials();
    /// Three shadow nodes for the process (view / planar-reflect half-res /
    /// probe-capture quarter-res — kShadowNodeName and siblings); the view's:
    /// PSSM (3 splits) for the first directional
    /// light and `mShadowMapCount` focused maps for the closest point/spot
    /// lights, all in ONE atlas. Views opt in with setShadows(true). Also
    /// creates the half-resolution twin the planar-reflection pass uses and
    /// the quarter-resolution probe-capture node (kProbeShadowNodeName).
    void createShadowNode();
    /// The shared body (OgreShadow.cpp): one PSSM block + `focusedMaps` focused
    /// maps packed into one atlas derived from `baseResolution`, registered
    /// under `name`. `focusedMaps` is taken literally (0 = PSSM only, and then
    /// no point-light scratch cubemap is declared at all); `cubeResolution` is
    /// that cube's face size — the historical 1024 unless a node says otherwise.
    /// Built from the public compositor-definition API rather than
    /// ShadowNodeHelper — see the head of OgreShadow.cpp for why.
    /// `perMapClears` picks the clear strategy: false = upstream's ONE
    /// whole-atlas PASS_CLEAR (cheapest, and what every scene without a static
    /// shadow map wants), true = one clear QUAD per map, which is what lets a
    /// static map survive its neighbours being redrawn. The engine rebuilds the
    /// node when the answer changes.
    void buildShadowNode(const char *name, unsigned baseResolution, unsigned focusedMaps,
                         bool perMapClears,
                         unsigned cubeResolution = kPointLightCubemapResolution);
    /// Which of the two clear-quad materials writes the far plane on this
    /// backend (reverse depth or not). OgreShadow.cpp.
    const char *shadowClearMaterialName() const;
    /// The colour the point-light cube faces clear to, by upstream's own rule.
    Ogre::ColourValue shadowClearColour() const;

    /// Maps the neutral enum onto HlmsPbs. PCF only: ExponentialShadowMaps is
    /// deliberately NOT used for VerySoft — ESM needs an ESM-compatible shadow
    /// node (colour shadow-map target plus blur passes) which our fixed
    /// depth-atlas shadow node (createShadowNode) is not, and
    /// setShadowSettings(ESM) also flips the global ShadowCameraSetup ESM flag.
    void applyShadowFilter();

    Ogre::Root     *mRoot = nullptr;
    Ogre::Window   *mNullWindow = nullptr;
    /// The document's staging scene manager (SPECS/SCENEGRAPH_SPEC.md D2).
    /// Owned here so that it dies with the engine, before the Root.
    Ogre::SceneManager *mDocumentScene = nullptr;
#ifdef __linux__
    /// The host's X11 `Display*`, kept opaque (see X11Handle) — this TU never
    /// dereferences it, it only hands it back to Ogre.
    void           *mDisplay = nullptr;
#endif
    bool            mHlmsRegistered = false;
    /// EngineConfig::headless: the NULL render system is loaded, mNullWindow is
    /// the 1x1 window IT created at boot, and no View can exist.
    bool            mHeadless = false;
    /// Plugin_ParticleFX2 loaded: the emitter/affector factories exist. False
    /// leaves billboard sets working and setParticleSystem failing cleanly.
    bool            mHasParticleFX2 = false;
    /// The forwarding LogListener. Defined only in OgreLogBridge.cpp, and held
    /// as a RAW pointer for that reason: a unique_ptr member would instantiate
    /// its deleter in ~OgreEngine (OgreEngine.cpp), where this type is
    /// incomplete. Created by attachLogBridge and deleted by detachLogBridge,
    /// both of which live in the TU where LogBridge IS complete; ~OgreEngine
    /// calls detach unconditionally, before Root goes.
    class LogBridge;
    LogBridge *mLogBridge = nullptr;
    ShadowFilter    mShadowFilter = ShadowFilter::Soft;
    unsigned        mShadowResolution = 2048;
    /// FOCUSED (point/spot) shadow maps the atlas currently has room for —
    /// what `buildShadowNode` was last built with. Two is the historical value
    /// and the floor: at two, planShadowAtlas reproduces the old strip layout
    /// exactly, so a scene with at most two shadow casters renders the same
    /// bytes it always did (SHADOW_TOOLING_SPEC.md §4.1).
    unsigned        mShadowMapCount = 2;
    /// What the host asked the atlas to be allowed to grow to. Eight is the
    /// engine's own default because the tier that matters (High, 2048) is
    /// eight; the resolution cap in effectiveShadowMapBudget() is what keeps a
    /// 4096 or 8192 atlas from taking the number literally.
    unsigned        mShadowMapBudget = 8;
    /// Whether the CURRENT shadow nodes clear per map (see buildShadowNode).
    /// False until a drawn scene first holds a cacheable (point/spot,
    /// shadowed) lamp, so a process that only draws sun-lit scenes executes
    /// upstream's pass list exactly; then true for the rest of the session
    /// (one-way — applyShadowCache).
    bool            mShadowPerMapClears = false;
    /// Derivation bookkeeping: the count the last few frames asked for and how
    /// many frames in a row have asked for it. A scene LOADS its lights over
    /// many frames, and each rebuild drops and recreates every workspace that
    /// names the shadow node — so the growth waits for the light list to settle
    /// instead of hitching once per lamp.
    unsigned        mDerivedShadowMapWant = 0;
    unsigned        mDerivedShadowMapFrames = 0;
    /// The last caster count reported as over budget, so the warning is logged
    /// once per count and not once per frame.
    unsigned        mWarnedShadowCasters = 0;
    /// Frames the derived count must hold still before the atlas is rebuilt.
    static constexpr unsigned kShadowDeriveDebounceFrames = 3u;
    /// THE PASS COUNTERS' READINGS for the last rendered frame (P8), latched by
    /// latchShadowCounters. View kind = the first enabled view with a shadow
    /// node (the "one view speaks for the process" rule); reflect and probe
    /// kinds = every planar slot and every shadowed probe of every scene.
    /// `Lamp` = passes of focused (point/spot) maps; StaticShadowRenders = the
    /// view's passes spent re-rendering CACHED lamp maps — zero at rest, which
    /// is the whole point, measurably.
    unsigned        mShadowPassesLastFrame = 0;
    unsigned        mStaticShadowRendersLastFrame = 0;
    unsigned        mShadowKindPasses[kShadowNodeKinds] = { 0, 0, 0 };
    unsigned        mShadowKindLampPasses[kShadowNodeKinds] = { 0, 0, 0 };
    std::vector<unsigned> mShadowViewMapPasses;   ///< per map index, view kind
    /// What applyShadowCacheDirties did last frame, per kind: instances whose
    /// lamps are cached, instances left on the dynamic sort (over budget), and
    /// lamp maps it dirtied.
    unsigned        mShadowCachedInstances[kShadowNodeKinds] = { 0, 0, 0 };
    unsigned        mShadowUncachedInstances[kShadowNodeKinds] = { 0, 0, 0 };
    unsigned        mShadowDirtiedMaps[kShadowNodeKinds] = { 0, 0, 0 };
    /// The per-kind shadow-pass counters (defined in OgreShadow.cpp), RAW for
    /// the LogBridge reason: a unique_ptr member would instantiate its deleter
    /// in ~OgreEngine (OgreEngine.cpp), where the type is incomplete. Created
    /// by applyShadowCache and destroyed by detachShadowCounter(), both in the
    /// TU where it is complete.
    class ShadowPassCounter;
    ShadowPassCounter *mShadowCounters[kShadowNodeKinds] = { nullptr, nullptr, nullptr };
    /// The view the VIEW counter rides, so it moves with the primary view
    /// instead of counting a dead workspace's passes.
    OgreView       *mShadowCounterView = nullptr;
    /// THE COUNTERS ARE OPT-IN, AND THE OPT-IN EXPIRES (P8). A workspace
    /// listener costs a callback per compositor pass per frame, so they run
    /// only while somebody reads shadowStatus(): each read stamps the frame,
    /// and kShadowPollWindowFrames frames without one detach them again. (It
    /// used to be a latch that never cleared, so one World-panel open kept a
    /// per-pass callback on the render path for the rest of the session.)
    /// `mutable` because shadowStatus() is const and asking is what arms it —
    /// the RenderStats::metricsRecording pattern.
    static constexpr unsigned long long kShadowPollWindowFrames = 120ull;
    unsigned long long mShadowFrame = 0;
    mutable unsigned long long mShadowPollFrame = 0;
    mutable bool    mShadowPolled = false;
    unsigned        mDefaultSamples = 1;   // EngineConfig::sampleCount, sanitized; on-screen views only
    /// EngineConfig::vsync, then whatever setVsync() last said. Read at every
    /// window creation (createView + the MSAA-recreate hook), so the pacing
    /// choice survives a resize or a sample-count change.
    bool            mVsync = true;
    /// How many scenes the LAST renderOneFrame updated (ObjectCounts::
    /// updatedScenes). A census row, not a timing: in a process holding seven
    /// scene managers this is the number that says how many of them the frame
    /// loop actually paid for.
    unsigned        mUpdatedScenes = 0;
    /// Threads in the multiload texture pool (THREADING_ADOPTION_SPEC.md P2
    /// item 5). 0 means the feature is off — the single background streaming
    /// thread does every decode, which is Ogre's default and what
    /// JAH_TEXTURE_MULTILOAD=0 restores at run time.
    unsigned        mMultiLoadThreads = 0;

    // ---- THE TEXTURE WAIT'S WATCHDOG (defect 2026-09-08) -------------------
    //
    // `waitForStreamingCompletion` is an UNBOUNDED loop around
    // `mRequestToMainThreadEvent.wait()` (OgreTextureGpuManager.cpp:3634): if a
    // load request is raised that no worker will ever complete, the UI thread
    // parks in it forever. That is not hypothetical — it was measured, twelve
    // minutes deep, on a GLB import's thumbnail render. `drainTextureStreaming`
    // replaces the call with the same drain and a NO-PROGRESS deadline, so the
    // worst case is a slow frame and a loud log instead of a dead application.
    //
    // The budget is a NO-PROGRESS budget, not a total one: a genuinely large
    // load that keeps finishing textures keeps extending it, so a slow disk can
    // never trip it. JAH_TEXTURE_WAIT_MS overrides it (0 disables the wait
    // entirely — a measurement escape hatch, not a supported mode).
    /// 8 s by default AT BIRTH, not at Hlms registration: a frame can be
    /// rendered before the env override is read, and a zero here would mean
    /// "do not wait at all" on exactly those frames.
    unsigned        mTextureWaitBudgetMs = 8000u;
    unsigned        mTextureWaitTimeouts = 0;
    double          mTextureWaitWorstMs = 0.0;
    /// Latched by the first timeout: after that the frame stops waiting at all.
    /// A wait that has already proven it cannot finish must not be paid for
    /// once per frame — the app would still be unusable, just noisily so.
    bool            mTextureWaitBroken = false;
    /// JAH_TEXTURE_WAIT_FAULT: the drain never agrees that it is finished. The
    /// only way to prove the give-up path, because the real trigger kills a
    /// decode worker before the main thread can time anything.
    bool            mTextureWaitFault = false;

    /// Drains the texture streaming queues, bounded. Returns true when the
    /// queues really did empty; false when the no-progress budget expired, in
    /// which case it has already logged the pending textures by name.
    bool drainTextureStreaming(double *msSpent = nullptr);
    Ogre::AbiCookie mAbiCookie{};
    std::string     mBackendName, mMediaDir;
    /// MUTABLE because the const readbacks (shadowStatus) report backend
    /// failures through the same channel as everything else: a status call that
    /// threw must not look like a status call that found nothing.
    mutable std::string mLastError;
    ShaderCache     mShaderCache;
    /// The process's recorded permutation set (SHADER_CACHE_SPEC §2.7b).
    /// PROCESS-wide because Ogre's analyze() accumulates and its entries are
    /// private — accumulating in one storage IS the merge. Held by pointer so
    /// the Ogre type stays out of every other TU's view of this header.
    std::unique_ptr<Ogre::VertexFormatWarmUpStorage> mWarmUpSet;
    std::vector<std::unique_ptr<OgreScene>> mScenes;
    std::vector<std::unique_ptr<OgreView>>  mViews;
    /// EngineConfig::profile / setProfiling: views created while true get a
    /// PassProfiler at birth.
    bool mProfiling = false;
};

}  // namespace detail
}}  // namespace jahshaka::engine
