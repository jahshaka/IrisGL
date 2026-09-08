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
#include <memory>
#include <string>
#include <vector>

// The two compositor definition types this header names but does not use: the
// PiP's scene pass, whose viewport rectangle the view rewrites live, and the
// clear pass that owns the inset's background colour (chain::PipHandles).
// Forward-declared rather than included so the pass-def headers stay where they
// belong — inside the .cpp files that build passes.
namespace Ogre { class CompositorPassSceneDef; class CompositorPassClearDef;
                 class CompositorPassQuadDef; class CompositorPassDef; }

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

/// How many entries PbrTextureSlot has (Albedo..Emissive). The enum is a plain
/// public enum with no sentinel, and MaterialRec indexes an array by it.
constexpr size_t kPbrTextureSlotCount = 5;

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
    /// passes and the quad that paints the inner background.
    std::vector<Ogre::CompositorPassDef *> insetPasses;
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
/// anything: the letterbox rect lives on the SCENE pass's own mVpRect, which
/// Ogre re-reads from the definition on every execute
/// (CompositorPass::setRenderPassDescToCurrent), so writing it between frames
/// is live. The fill quad keeps the full [0,1] rect and therefore always paints
/// the whole inset — background plus bars.
struct PipHandles {
    Ogre::CompositorPassSceneDef *scenePass = nullptr;
    /// The clear pass that paints the inset's background swatch. Its colour is
    /// read per execute too, so pushing a new background never rebuilds either.
    Ogre::CompositorPassClearDef *fill = nullptr;
};

/// Builds the inset's node + workspace definitions under `workspaceDef`.
///
/// The shape is the spike's, and every line of it is a finding (see
/// ViewPipDesc): quad fill (Load colour, so the main frame survives) then one
/// scene pass with Load on colour, CLEAR on depth, shadows OFF and the overlay
/// render queues excluded — an inset is "what the camera sees", not a second
/// copy of the editor's gizmos, and the camera BODY that put the inset on
/// screen must not appear inside it.
void buildPip(Ogre::Root *root, const std::string &workspaceDef, const ViewPipDesc &pip,
              std::vector<std::string> &nodeDefsOut, PipHandles &handlesOut);
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
    bool setSky(SkyMode mode, TextureId texId) override;
    bool setSkyCubemap(const TextureId faces[6]) override;
    /// Environment reflections divorced from the sky: the host pushes six
    /// resampled faces of its equirect/baked sky image. Six zero ids clear.
    bool setSkyReflection(const TextureId faces[6]) override;
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
                           const SkeletonDesc &rig) override;
    bool followSkeleton(NodeId follower, NodeId source) override;
    /// Copies every follower's pose from its source. Once per rendered frame,
    /// AFTER the frame, so the poses copied are the ones just drawn.
    void applySkeletonFollowers();
    bool hasSkeleton(NodeId id) const override;
    std::vector<std::string> boneNames(NodeId id) const override;
    bool setBonePoses(NodeId id, const BonePose *poses, size_t count) override;
    bool boneMatrices(NodeId id, float *out, size_t count) const override;

    // ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC; impl in OgreClips.cpp) ----
    bool attachClips(NodeId id, const ClipDesc *clips, size_t count) override;
    std::vector<std::string> clipNames(NodeId id) const override;
    bool setClipStates(NodeId id, const ClipState *states, size_t count) override;
    bool setBoneManual(NodeId id, const std::string &bone, bool manual) override;
    bool bonePoses(NodeId id, BonePose *out, size_t count) const override;
    std::vector<float> clipBoneWeights(NodeId id, const std::string &clip) const override;

    // ---- Textures ----
    TextureId loadTexture(const std::string &path, bool srgb) override;
    TextureId createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb) override;
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
                            unsigned capacity) override;
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
    void refreshGlobalIllumination() override;
    GiStatus giStatus() const override;
    unsigned long long giEscapeSignature() const override;
    unsigned long long giGeometrySignature() const override;
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
    void recreatePlanarAfterShadowRebuild();

    Ogre::SceneManager *sceneManager() const;

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
        /// What setNodeVisible was last told. Kept because PFX2 objects do not
        /// live under the node in Ogre's graph (they hang off the STATIC root),
        /// so no visibility cascade reaches them and a system created or
        /// recycled later has to be told the node's state explicitly.
        bool                      visible = true;
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
        bool hasSkinData = false;
        unsigned maxBlendIndex = 0;
        std::string rigId;
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
        /// Which TextureId occupies each PbrTextureSlot right now (0 = none).
        /// The REVERSE of the binding, kept so destroyTexture can unbind a
        /// texture from every material holding it: an Ogre datablock keeps a
        /// raw TextureGpu* and a descriptor set, so destroying a still-bound
        /// texture leaves a stale pointer that only shows up as a GPU-side
        /// fault later. Latent until something actually reclaims textures —
        /// which the mirror now does.
        TextureId boundTextures[kPbrTextureSlotCount] = { 0, 0, 0, 0, 0 };
        /// The parameters LAST APPLIED to this material (PBR materials only).
        /// setShadingModel destroys the datablock and builds a new one in the
        /// other family, and it takes no parameters — it rebuilds from this.
        /// Without it a family switch would silently reset the material to the
        /// defaults until the host happened to push again.
        PbrParams params;
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
    };

    void applyReflectionToAllImpl();
    /// The IBL cubemap AS BOUND TO DATABLOCKS — null while automatic PCC owns
    /// the shader's one env-probe slot (OgreSky.cpp, the long note there).
    Ogre::TextureGpu *reflectionTexForDatablocks() const;
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
    /// Forgets this node's pose-following pairings in both directions — the
    /// node itself is going away (releaseNode).
    void dropSkeletonFollowers(NodeId id, Node &n);
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
    /// (Re-)binds whatever `rec.boundTextures` says onto the material's CURRENT
    /// datablock — the step that makes a family switch keep its maps. The Unlit
    /// family has one usable slot (Albedo -> texture unit 0); the rest are kept
    /// in the record so switching back to Lit restores them.
    void bindTrackedTextures(const MaterialRec &rec);
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
    bool bindRigToMesh(MeshRec &meshRec, const SkeletonDesc &rig);
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
    /// Spends this frame's probe-update budget: picks the probes to re-capture
    /// and raises `mDirty` on them (FIX WAVE B2). Called from updateGiTracking
    /// with the AUTHORITATIVE camera position; a no-op at budget 0.
    void updateProbeBudget(const Ogre::Vector3 &camPos);
    /// Refreshes mGiItemAabbs and fills mGiMovedBoxes with what moved since the
    /// last call (FIX WAVE B3, engine half). Called once per frame from
    /// updateProbeBudget; NOT from giGeometrySignature, which is stateless.
    void scanGiMovement();
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
    Ogre::uint32 itemVisibilityFlags(Node &n, bool unlit);
    /// Re-applies itemVisibilityFlags (and the billboard/particle equivalents)
    /// to whatever `n` currently carries. Needed because the helper flag can be
    /// set before or after the geometry is attached.
    void applyNodeVisibilityFlags(Node &n);
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

    Ogre::SceneNode *node(NodeId id) const;
    /// Ids are monotonic per scene and never reused.
    NodeId track(const Node &n);


    Ogre::Root         *mRoot;
    Ogre::SceneManager *mSceneMgr;
    std::string         mName;
    std::string        &mError;
    std::map<NodeId, Node> mNodes;
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
    /// The index key for a texture record: decal slices are namespaced by kind.
    static std::string textureKey(const std::string &path, bool decal, DecalMap kind);
    /// Registers a texture record: assigns the next id and indexes it by path.
    /// The ONLY way a TextureRec enters mTextures, so the index cannot drift.
    TextureId trackTexture(const TextureRec &rec);
    /// Our slot enum -> Ogre's PBSM_* unit.
    static Ogre::PbsTextureTypes pbsSlotOf(PbrTextureSlot slot);
    std::set<std::string> mTextureDirs;
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
    /// live VCT arm. Dies BEFORE mVctLighting (it holds that pointer).
    Ogre::IrradianceField            *mIfd          = nullptr;
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
    /// Same contract for the two probe-capture options (P3a/P3b): what the last
    /// buildPcc RESOLVED, after GiToggle::Auto consulted the quality dial and
    /// after the shadow half checked that a shadow node exists to recalculate.
    bool mPccHdr      = false;
    bool mPccShadowed = false;
    /// THE PROBE ROUND-ROBIN (FIX WAVE B2). One entry per probe, rebuilt with
    /// the grid. `sweepPending` is true while the probe still owes this sweep an
    /// update — the sweep set refills when it empties, which is what makes
    /// "every probe within ceil(probes / budget) frames" a guarantee rather than
    /// a hope. `framesSinceUpdate` and the moved-cover test only decide the
    /// ORDER inside a sweep.
    struct ProbeSlot {
        bool     sweepPending = true;
        unsigned framesSinceUpdate = 0;
    };
    std::vector<ProbeSlot> mProbeSlots;
    /// How many probes updateProbeBudget dirtied on the LAST frame it ran, and
    /// the resolved per-frame budget (the request clamped to the grid). Reported
    /// by giStatus so a caller can see what the renderer actually spends.
    int mProbeUpdatesPerFrame = 0;
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
    /// Bumped whenever anything the GI arms hold RAW POINTERS INTO may have
    /// died — every invalidateGiCaches call site (B4). The reuse arm refuses to
    /// re-run an existing voxelizer across a bump, which is what keeps the
    /// "always from scratch" rule's guarantee (InstantRadiosity::freeMemory's
    /// cache keys, VctMaterial's datablock-pointer cache) exactly as strong.
    unsigned long long mGiDestroyGeneration = 0;
    unsigned long long mGiBuiltGeneration   = ~0ull;   // no build yet
    /// The NodeIds handed to the live VctVoxelizer, so the reuse arm can add the
    /// items created since the build. Cleared with the arm.
    std::vector<NodeId> mVctItemIds;
    /// Live decals in THIS scene. The SceneManager-level atlas binding is
    /// driven off the count (see refreshDecalBindings).
    unsigned            mDecalCount = 0;
    // Planar-reflection arm. mPlanar is null unless mPlanarParams.budget > 0.
    // mReflectors is the DOCUMENT's set of reflector nodes and survives the arm
    // going up and down; mActors only exists while the arm is up.
    Ogre::PlanarReflections *mPlanar = nullptr;
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
    /// Is this view ENTITLED to draw an inset at all? On-screen always;
    /// offscreen only with ViewPipDesc::allowOffscreen. The determinism law's
    /// single gate, in the same shape as overlaysAllowed() and chainDesc()'s
    /// post-fx early-out.
    bool pipAllowed() const;
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
    /// What chain::build handed back for THIS view's main chain — today the
    /// letterbox's inset passes and its background swatch.
    chain::ChainHandles        mChainHandles;
    /// The last CameraDesc a host pushed. Kept because two things derive from
    /// it beyond the Ogre camera itself: the letterbox flag (a graph change)
    /// and its rectangle (re-derived on every resize).
    CameraDesc                 mCameraDesc;
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
    unsigned textureMetadataCacheEntries() const override;
    unsigned textureChannelCacheEntries() const override;
    bool saveTextureCache() override;

    /// Applies to every on-screen window that exists AND is remembered for the
    /// ones created later (createView and the MSAA-recreate hook both read it).
    void setVsync(bool on) override;
    bool vsync() const override { return mVsync; }
    const std::string &lastError() const override;
    std::string takeLastError() override;

    /// PROCESS-WIDE: both ride Ogre's single frame-time controller value
    /// (ControllerManager -> FrameTimeControllerValue). Note the backend's own
    /// coupling — setTimeFactor zeroes the frame delay and setFrameDelay zeroes
    /// the time factor, so the two are mutually exclusive by construction, not
    /// by our choice.
    void  setParticleTimeScale(float scale) override;
    float particleTimeScale() const override;
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
    /// One shadow node for the process: PSSM (3 splits) for the first directional
    /// light and focused maps for the next two point/spot lights, in one atlas.
    /// Mirrors Ogre's ShadowMapFromCode sample. Views opt in with setShadows(true).
    /// Also creates the half-resolution twin the planar-reflection pass uses.
    void createShadowNode();
    /// The shared body: one PSSM + two focused maps in one atlas derived from
    /// `baseResolution`, registered under `name`.
    void buildShadowNode(const char *name, unsigned baseResolution);

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
    Ogre::AbiCookie mAbiCookie{};
    std::string     mBackendName, mMediaDir, mLastError;
    ShaderCache     mShaderCache;
    /// The process's recorded permutation set (SHADER_CACHE_SPEC §2.7b).
    /// PROCESS-wide because Ogre's analyze() accumulates and its entries are
    /// private — accumulating in one storage IS the merge. Held by pointer so
    /// the Ogre type stays out of every other TU's view of this header.
    std::unique_ptr<Ogre::VertexFormatWarmUpStorage> mWarmUpSet;
    std::vector<std::unique_ptr<OgreScene>> mScenes;
    std::vector<std::unique_ptr<OgreView>>  mViews;
};

}  // namespace detail
}}  // namespace jahshaka::engine
