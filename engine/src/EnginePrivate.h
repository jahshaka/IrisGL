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
// THE GPU SCENE's tables (A3 slice). Engine-private and Ogre-aware — it holds
// MeshPtrs and hands out UavBufferPackeds — but it knows nothing of Vulkan and
// nothing of OgreScene's Node, which is what keeps it bindable from an
// HlmsComputeJob and buildable on a platform with no ray queries.
#include "GpuCull.h"
#include "GpuScene.h"

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
#include <set>
#include <unordered_map>
#include <unordered_set>
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
#include <mutex>
#include <condition_variable>
#include <thread>
#include <map>
#include <chrono>
#include <deque>
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
                 class TagPoint;
                 // The render-loop monitor's pass listener takes one
                 // (passSceneAfterShadowMaps) and never dereferences it here.
                 class CompositorPassScene; }

namespace jahshaka { namespace engine {
/// SURFACE-CACHE phase 2 — the capture Component (SurfaceCache.h). It is at
/// ENGINE scope rather than in `detail` because it is a Component in the pin's
/// own sense: a class built on the public Ogre API, the shape `VctLighting` and
/// `IrradianceField` are built in, which a future consumer outside the backend
/// may hold. Only OgreSurfaceCache.cpp and the scene's own TU need its
/// definition.
class SurfaceCache;
// The backend's own namespace: these types and helpers are shared between the
// TUs under engine/src and by nothing else (they used to live in one anonymous
// namespace, when the backend was a single translation unit).
namespace detail {

inline Ogre::Vector3     toOgre(const Vec3 &v)   { return Ogre::Vector3(v.x, v.y, v.z); }
inline Ogre::ColourValue toOgre(const Colour &c) { return Ogre::ColourValue(c.r, c.g, c.b, c.a); }

/// The LOD switch band a WATCHED view's scene passes carry, and the suite's
/// offscreen latch (OgreMesh.cpp; ogre-patch 0075). @see ChainDesc::lodHysteresis.
float jahLodHysteresis();

/// ATOM stage 1's VIEW rule (OgreMesh.cpp): registers `jah_world_error` — the
/// LOD strategy whose per-object value is the world-space error the pass's own
/// camera and render target can hide — and makes it the process default. The
/// switch BAND ogre-patch 0075 adds to `LodStrategy::lodSet` is per PASS and
/// comes from the chain (`ChainDesc::lodHysteresis`), not from here. Called
/// once, after Root::initialise and before any mesh or Item exists.
void installJahLodStrategy();

/// THE HOST'S TRANSFORM-WRITE COUNTER (Engine::setTransformWriteCounter), or
/// null when no host handed one over — in which case every frame's GI movement
/// scan runs, the way it always did. Read once a frame per scene, relaxed: it
/// is a change test, not an ordering. Process-wide because the document's
/// counter is (one graph, many scenes).
extern const std::atomic<unsigned long long> *gTransformWriteCounter;

/// Names handed to Ogre must be unique for the life of the process (a destroyed
/// scene may be recreated under the same name while stale resources linger).
inline std::string processUniqueName(const char *prefix) {
    static std::atomic<unsigned> counter{0};
    return std::string(prefix) + "_" + std::to_string(++counter);
}

/// A NAME THAT IS UNIQUE WHILE IT IS WORN, AND REUSED AFTERWARDS — for the
/// CUBE RENDER TARGETS, where processUniqueName above is a slow leak.
///
/// `HlmsPbs::preparePassHash` hashes the render target's NAME into the pass
/// shader properties when that target is a cubemap — `target_envprobe_map`,
/// OgreHlmsPbs.cpp:1813, there so a shader never samples the probe it is
/// drawing into. So a cube render target with a FRESH name every time mints,
/// per capture and for the life of the process:
///   * one permanent entry in `Hlms::mPassCache`, and
///   * one shader compile for everything drawn into it.
/// The pass cache is indexed by EIGHT BITS of the 32-bit shader hash
/// (HlmsBits::PassBits): at entry 256 the index spills into the RENDERABLE
/// field beside it and every later hash names the wrong renderable — which is
/// the crash ogre-patch 0035 now catches at the disk-cache save (lane
/// shadercache-2: the owner's session reached 1847 pass entries in an hour, and
/// ONE SKY CHANGE = ONE ENTRY, measured; 30 changes, +30 entries).
///
/// The fix is to stop minting names: a slot is taken while the texture lives
/// and returned when it dies, so a session that captures the sky ten thousand
/// times uses one name and one pass-cache entry. Slots are per PREFIX, and the
/// lowest free one always wins, so concurrent scenes get 0,1,2...
///
/// Thread-safe (the sky capture runs on the render thread; texture teardown can
/// run from a host thread during scene destruction). The statics are
/// function-local and deliberately never freed.
inline std::mutex &recycledNameMutex() { static std::mutex m; return m; }
inline std::map<std::string, std::vector<bool>> &recycledNameSlots() {
    static auto *slots = new std::map<std::string, std::vector<bool>>();
    return *slots;
}
inline std::string recycledName(const char *prefix) {
    std::lock_guard<std::mutex> lock(recycledNameMutex());
    std::vector<bool> &used = recycledNameSlots()[prefix];
    size_t slot = 0;
    while (slot < used.size() && used[slot]) ++slot;
    if (slot == used.size()) used.push_back(true); else used[slot] = true;
    return std::string(prefix) + "_" + std::to_string(slot);
}
/// Returns a name taken from recycledName. A name this pool never handed out
/// (every other Ogre name in this engine) is ignored, so a caller may route
/// every teardown through it without knowing which kind it holds.
inline void releaseRecycledName(const std::string &name) {
    const size_t sep = name.rfind('_');
    if (sep == std::string::npos || sep + 1 >= name.size()) return;
    for (size_t i = sep + 1; i < name.size(); ++i)
        if (name[i] < '0' || name[i] > '9') return;
    std::lock_guard<std::mutex> lock(recycledNameMutex());
    auto it = recycledNameSlots().find(name.substr(0, sep));
    if (it == recycledNameSlots().end()) return;
    const size_t slot = std::strtoul(name.c_str() + sep + 1, nullptr, 10);
    if (slot < it->second.size()) it->second[slot] = false;
}

// Every backend virtual is wrapped: `JAH_TRY { ... } JAH_CATCH(errSink, failValue)`.
// Ogre throws Ogre::Exception; its own allocations may throw std::bad_alloc.
#define JAH_TRY try
#define JAH_CATCH(sink, ret)                                                          \
    catch (Ogre::Exception &e) { (sink) = e.getFullDescription(); return ret; }       \
    catch (std::exception &e)  { (sink) = std::string("engine: ") + e.what(); return ret; }

class OgreEngine;

/// THE RAY-QUERY TIER (PHOTON_SPEC §7 R1) — the whole of it lives in
/// OgreRayQuery.cpp, the one TU that may include Vulkan. Declared here so the
/// engine can hold one and the frame can call it; nothing else in this header
/// knows what a VkAccelerationStructure is.
class RayQueryTier;
/// True while the surface cache's capture workspace is executing. The one
/// question the Hlms listener asks before setting `jah_card_capture` — false in
/// every pass of every frame that is not a card capture, which is what keeps
/// every other shader in the process byte-identical. Defined in
/// OgreSurfaceCache.cpp so that no other TU needs the Component's full type.
bool surfaceCardsCapturing();
/// The card capture's workspace, for the monitor's listener walk. Null until a
/// scene turns `GiParams::cards` on.
Ogre::CompositorWorkspace *surfaceCacheWorkspace(const SurfaceCache *cache);

class OgreView;

/// THE REFLECTION TRACE'S HOOK (PHOTON_SPEC §7 R5). A CompositorWorkspaceListener
/// that records one compute dispatch into Ogre's own frame command buffer,
/// immediately before the scene pass that SAMPLES `jahSsrReflection` — which is
/// the only point in the frame where every input exists and the output is still
/// writable: the SSR prepass has written the normals and the packed roughness
/// the trace reads, and the SSR resolve has written the screen's own answer the
/// trace defers to and must not overwrite.
///
/// (The brief said "before the SSR resolve". It cannot be: the resolve RENDERS
/// the whole of `jahSsrReflection`, so anything written before it is thrown
/// away. Recorded after it, this pass reads the resolve's confidence and fills
/// only what the screen could not answer — which is what "screen first, rays for
/// the rest" means. Stated here because it is a deviation from the brief's
/// wording and not from its design.)
///
/// It holds NO state of its own: the per-view Vulkan resources (the descriptor
/// ring, the parameter buffer, the temporal mean's ping-pong) live in the tier,
/// keyed by this object's address, so that the one TU that may include Vulkan
/// stays the one TU that includes Vulkan.
class ReflectPassListener final : public Ogre::CompositorWorkspaceListener {
public:
    /// NOT `override`: Ogre's CompositorWorkspaceListener has no virtual
    /// destructor. Nothing ever deletes one of these through a base pointer —
    /// the View owns it by unique_ptr of the exact type — so the absence is a
    /// fact to respect rather than a defect to work around.
    ~ReflectPassListener();
    void passPreExecute(Ogre::CompositorPass *pass) override;
    /// GATHER-0 (fix round, D2): THE GATHER'S SHADER BINDING IS PASS-SCOPED,
    /// NOT FRAME-SCOPED, and that is a correctness rule rather than tidiness.
    /// The registration this listener makes in `passPreExecute` names a
    /// FULL-RESOLUTION texture belonging to ONE view, and it is keyed by
    /// SceneManager — so every later colour pass on that manager in the same
    /// frame would inherit the property, the binding and `vct_disable_diffuse`
    /// unless it is taken away the moment the pass it was made for is over: a
    /// second View on the same scene (the Player IS one), an offscreen
    /// screenshot view, a VR eye whose own gather was declined, a PCC probe
    /// capture, a planar-reflection arm. Two of those cannot even COMPILE the
    /// piece (a pass with no `hlms_screen_pos_int` has no `iFragCoord`, one
    /// with no `needs_env_brdf` has no `envColourD`), which loses the frame.
    void passPosExecute(Ogre::CompositorPass *pass) override;
    /// The view whose chain this listener rides. Never null while registered.
    OgreView   *mView = nullptr;
    /// ...and its Root, so the destructor can flush without reaching into the
    /// view's privates.
    Ogre::Root *mRoot = nullptr;
};

// ---------------------------------------------------------------------------
// Visibility-flag bits (user bits; Ogre reserves the top two for layer state).
// Every object keeps kVisibleBit so default cameras/compositor masks (all ones)
// draw it. kGiGeometryBit marks items whose surfaces bounce light for GI: PBR
// items only — never the sky, unlit overlays, line meshes or billboards, which
// would otherwise occlude rays or raycast as garbage triangles (a non-indexed
// line VAO reads as a vertex triangle list).
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
// visibility_mask must include kHelperBit, or helpers vanish from that pass —
// AND kMovableBit, unless the pass is a probe capture (see kMovableBit below;
// a pass that forgets it silently drops every moving object).
constexpr Ogre::uint32 kVisibleBit     = 1u;
constexpr Ogre::uint32 kGiGeometryBit  = 1u << 1;
// BIT 2 IS A HOLE, ON PURPOSE. It was `kGiLightBit`, which marked the one light
// Instant Radiosity traced from; IR is deleted (PHOTON_SPEC E2 (4)) and the bit
// is left unused rather than reclaimed, because every bit below is a value
// written into compositor scripts and mask literals and renumbering them would
// move objects between passes for a bit nobody needs.
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
// THE MOVING CHANNEL (SPECS/REALTIME_REFLECTIONS_SPEC.md §3.3.4, lane R2). A
// MOVABLE item carries this bit *INSTEAD OF* kVisibleBit — the third use of the
// inversion above, and the one the realtime program is built on.
//
// WHY A CHANNEL AND NOT A FLAG. The renderer keeps a memory of the room:
// reflection-probe captures, a voxel copy for the bounce light, cached shadow
// maps. Everything that MOVES has to stay out of that memory, or the memory
// re-takes its photos while things move and rebuilds itself when they stop
// (measured on the alive scene before this lane: probe captures on every frame
// of a mover, and a full re-solve the moment it stopped). "Stay out of the
// captures" is exactly what an include channel expresses and what an exclude
// bit cannot (the any-bit test above), so a mover simply stops carrying the bit
// every capture pass asks for:
//   * OUT, with no mask change anywhere: the reflection-probe faces
//     (`visibility_mask 0x1` in JahshakaPcc.compositor), the PROBE-kind shadow node
//     (shadowCasterChannels), and — through the flags, not through a mask —
//     Instant Radiosity's trace and every GI gather that keys on
//     kGiGeometryBit, which a movable item never carries either (it is LIT by
//     the room's GI, it just does not voxelize or bounce into it).
//   * IN, because these passes ask for the channel explicitly: the planar
//     reflection pass (OgrePlanar.cpp) and the VIEW and REFLECT shadow nodes
//     (shadowCasterChannels again). A mover is therefore reflected by the
//     mirror floor and casts its shadow in the SAME frame it moves.
//   * IN by default everywhere else: the main chain, the SSR prepass and the
//     thumbnail/preview shapes set no restricting visibility mask at all, and
//     the two overlay passes mask with RESERVED_VISIBILITY_FLAGS & ~kDistortion
//     (overlayVisibilityMask), which contains this bit.
//
// THE GI CLASS IS THE EXPENSIVE HALF, THE CHANNEL IS FREE. Turning
// kGiGeometryBit off is a GI edge (a from-scratch rebuild, OgreScene::
// setNodeMovable); switching kVisibleBit for kMovableBit costs one
// setVisibilityFlags. That difference is what makes the play-time SOFT
// promotion (owner decision O3) possible: it changes the channel and the
// gathers and deliberately does NOT invalidate, leaving the object's old bounce
// light behind as a ghost until play stops, for no hitch at all.
constexpr Ogre::uint32 kMovableBit     = 1u << 5;
// THE SUN-DISC CHANNEL (SPECS/SKY_LIGHT_SPEC.md §3, owner pick 4). The disc
// quad carries this bit INSTEAD OF kVisibleBit — the fourth use of the
// inversion above — so that "the reflection probes must not capture it" is
// expressible at all (the any-bit test cannot exclude).
//   * IN, by the all-ones default: the main chain, the SSR prepass, the
//     thumbnail/preview shapes. A user's picture has the sun in it.
//   * IN, explicitly: the planar reflection pass asks for this bit beside
//     kVisibleBit|kMovableBit (OgrePlanar.cpp) — a mirror shows the sun.
//   * OUT, with no mask change anywhere: the reflection-probe faces
//     (`visibility_mask 0x1`) and every shadow
//     node (shadowCasterChannels). A probe that captured the disc would paint
//     a SECOND sun highlight on every probe-lit glossy surface, on top of the
//     directional light's own specular.
// `world.sunDisc({inProbes: true})` puts kVisibleBit back ALONGSIDE this bit,
// which is the "include it" half of the owner's "we can have both options".
constexpr Ogre::uint32 kSunDiscBit     = 1u << 6;

// THE BACKDROP CHANNEL (PLAYER_ONE_SCENE, lane PLAYER-1). A backdrop carries
// this bit INSTEAD OF kVisibleBit — the fifth use of the inversion above — and
// it exists because kHelperBit had come to mean two different things at once.
//
// `setNodeHelper` says "no capture may see this": the probes, the shadow nodes
// and the GI gathers all ask for kVisibleBit, so a helper drops out of every
// one of them for free. That is exactly what the ground's 2 km HORIZON plane
// wants (nothing that size may size a shadow atlas or a voxel volume) — but the
// horizon is PART OF THE PICTURE, not editor furniture, and the moment one view
// of a scene has to hide the furniture (the Player page, which is a second View
// on the editor's scene) "helper" can no longer answer both questions.
//
// So the two meanings split. kHelperBit is EDITOR FURNITURE — the grid, the
// wires and icons, the gizmo, the selection shell, the GI volume boxes — and a
// view may mask it out per pass (ChainDesc::helpers). kBackdropBit is the other
// half of the old meaning, unchanged in every capture pass:
//   * OUT, with no mask change anywhere: the reflection-probe faces
//     (`visibility_mask 0x1`), every shadow node
//     (shadowCasterChannels), and kGiGeometryBit (itemVisibilityFlags never
//     grants it to a helper of either kind).
//   * IN, by the all-ones default: the main chain and the thumbnail/preview
//     shapes, for EVERY view — including a view that hides the furniture.
//   * OUT of the planar mirrors, exactly as it was: the planar pass asks for
//     kVisibleBit|kMovableBit|kSunDiscBit and is deliberately not widened, so
//     the pixels a mirror shows do not move with this split.
constexpr Ogre::uint32 kBackdropBit    = 1u << 7;

// THE VR HELPER CHANNEL (SPECS/VR_SPEC.md §5 phase 4, lane VR-4; the rule is
// the owner's, 2026-09-17). The sixth use of the inversion above, and the one
// that makes "the wearer's own hands" expressible.
//
// TWO QUESTIONS, TWO ANSWERS. Once the editor's scene can be WORN, "is this
// object furniture?" splits in two, and the split is not about the object's
// kind — it is about WHOSE furniture it is:
//
//   * kHelperBit        = THE DESK'S. The ground grid, the light/camera wires
//                         and icons, the mouse gizmo, the selection outline,
//                         the GI volume boxes. A VR eye draws them or not
//                         ACCORDING TO THE HOST MODE (VrConfig::helpers): the
//                         editor's preview does, because standing inside the
//                         scene and watching the editor work is the mode's
//                         whole purpose, and the Player does not, exactly as
//                         the desktop Player does not.
//   * kVrHelperBit      = THE WEARER'S. The two CONTROLLER proxies today, and
//                         phase 4b's controller ray, hit marker and in-VR
//                         gizmo. EVERY VR eye draws them, in BOTH modes — a
//                         player needs to see their own hands as much as an
//                         author does — and so does the desktop EDITOR
//                         viewport, so the person at the desk can see where the
//                         wearer is reaching.
//
// A node may be in both (Scene::setNodeVrHelper is ADDITIVE, never instead-of):
// the controller proxies are, which is what puts them in the desk's picture
// through the ordinary helper channel and in a HELPER-LESS player's eye through
// this one.
//
// WHAT NEITHER CHANNEL EVER REACHES, and it needs no new rule: a capture. The
// probe faces' `visibility_mask 0x1`, the shadow nodes' shadowCasterChannels,
// the planar pass and kGiGeometryBit all ask for kVisibleBit, which an item in
// either helper channel does not carry. A user's screenshot is the same
// statement in the other direction: its offscreen view opens NEITHER channel.
//
// THE RULE THAT COMES WITH IT is kHelperBit's, widened: a view that hides the
// furniture must take BOTH bits out of its mask unless it deliberately wants
// one of them (chain::helperBitsToDrop does).
constexpr Ogre::uint32 kVrHelperBit    = 1u << 8;

// THE HIDDEN-AREA MESH'S OWN CHANNEL (lane HAM-1, VR_SPEC §9). The SEVENTH use
// of the inversion above, and the narrowest: exactly one object in the process
// ever carries this bit — the mask the runtime handed over — and it carries it
// INSTEAD OF kVisibleBit.
//
// WHAT THE INVERSION BUYS HERE. The mask is a depth-only draw at the NEAR
// plane, so anything that renders it and then shades through it comes out
// EMPTY: a probe face would capture black corners, a sky capture would
// integrate them into the ambient, a planar mirror would lose a wedge, a shadow
// map would be stamped with a wall one centimetre from the light. Every one of
// those passes asks for kVisibleBit (the probe faces' `visibility_mask 0x1`,
// the shadow nodes' shadowCasterChannels, OgrePlanar's allowlist, the GI
// gathers' kGiGeometryBit), so not carrying kVisibleBit keeps the mask out of
// all of them with no new rule.
//
// WHAT STILL NEEDS ONE is the VIEW CHAINS, whose scene passes are born holding
// every RESERVED bit: `chain::helperBitsToDrop` takes this bit out of every
// view's node UNLESS that view is a VR session's eye pair
// (ChainDesc::hiddenAreaMask). So the desktop viewport, the Player's window, a
// thumbnail, a preview and a user's screenshot cannot draw it even while a
// session is live — which is the channel requirement HAM-1 was given, stated
// where the bit is defined.
constexpr Ogre::uint32 kVrMaskBit      = 1u << 9;

// THE CARD CAPTURE'S SUBJECT CHANNEL (SURFACE-CACHE-1b). The EIGHTH bit, and
// the only one that is not an inversion: it is ADDED to an Item's flags rather
// than swapped for kVisibleBit, and it is added for the length of ONE capture
// pass and taken away before that pass's `_update` returns.
//
// WHY IT HAS TO EXIST AT ALL. The surface cache captures one instance at a time
// in the SCENE'S OWN SceneManager — the spike's scratch manager is the wrong
// shipping shape (a registered manager costs a `updateSceneGraph()` and its
// barrier every frame for as long as it lives, capturing or not) — so the
// capture pass has to say "draw THIS object and nothing else". Ogre's
// visibility test is ANY-BIT-SET, which cannot express an exclusion and cannot
// express a singleton either; what it CAN express is a channel that exactly one
// object is in, and that is this bit.
//
// WHAT IT DELIBERATELY DOES NOT REACH: the shadow node. A shadow node draws its
// casters through `shadowCasterChannels(kind)` and not through the pass's mask,
// so the whole still world keeps casting into the atlas while exactly one
// object is shaded out of it — which is the entire reason a card's shadow term
// is occlusion by OTHER objects and not the prepass's constant 1.0.
//
// NO OTHER PASS IN THE ENGINE NAMES IT, and none needs to: every view chain's
// scene pass is born holding every RESERVED bit, so an item wearing this bit
// for a moment is drawn by them exactly as it was.
constexpr Ogre::uint32 kCardSubjectBit = 1u << 10;

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
/// buildPcc's quality table). A probe
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
/// HOW MUCH NEARER A LAMP THIS INSTANCE ALREADY HOLDS COUNTS, on the SQUARED
/// distance (0.81 = 10% of range), when a probe has to choose which
/// kProbeShadowMaxFocusedMaps lamps to cache (keepNearestLamps). Without it a
/// lamp drifting across the boundary would evict and re-admit itself — two
/// re-rendered maps a frame — instead of one re-render once.
constexpr float        kShadowNearestStickiness   = 0.81f;
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
/// dirties a kind's maps only through these channels).
///
/// R2: the VIEW and REFLECT kinds draw the still world AND the movers
/// (kMovableBit), the PROBE kind draws the still world only — because a probe
/// capture does not contain the movers in the first place (kMovableBit's note),
/// so a shadow map rendered for one would be shadowing objects that are not
/// there. Read as the caster scan reads it, this one line is also the rule
/// "a mover dirties view and reflect maps only, never a probe's" (spec §5.5):
/// the scan records each caster's channels and the cache dirties a kind only
/// where they intersect. Never hard-code kVisibleBit as "the casters" elsewhere.
inline Ogre::uint32 shadowCasterChannels(ShadowNodeKind kind) {
    return kind == ShadowNodeKind::Probe ? kVisibleBit : (kVisibleBit | kMovableBit);
}
inline Ogre::uint32 allShadowCasterChannels() {
    return shadowCasterChannels(ShadowNodeKind::View) |
           shadowCasterChannels(ShadowNodeKind::Reflect) |
           shadowCasterChannels(ShadowNodeKind::Probe);
}
/// WHICH LAMPS A KIND'S INSTANCES HOLD FIXED (cached): EVERY cacheable lamp,
/// for every kind. There is no filter and there was never a real one — the
/// `shadowLampCachedFor(kind, light)` hook that used to say so returned true
/// unconditionally and was called per lamp per kind per frame from both walks
/// (deleted, clean-2 lane 2026-09-13).
///
/// WHAT IS TRUE ABOUT A MOVING LAMP AND A PROBE, since that is what a reader
/// comes here to ask (lane R2): a probe capture is still lit by every light,
/// moving ones included — there is no `light_visibility_mask` anywhere in
/// engine/media, and R2 deliberately did not add one (its interaction with a
/// FIXED lamp map, which Ogre writes into the pass buffer whatever the pass's
/// light mask says, is unverified at this pin). What a moving lamp does NOT do
/// is make a probe re-capture: a light's TRANSFORM is not in the probe cache's
/// light key (OgreScene::setLight folds colour, range, cone, shadowing and
/// channels, never the pose), so the probe simply holds the frozen picture it
/// last captured, which is the O4 freeze applied to lamps. Its pose key does
/// dirty the lamp maps of every KIND through the caster scan, so no probe
/// capture ever samples a stale map either.
///
/// THE FOLLOW-ON (REALTIME_REFLECTIONS_SPEC §3.3.4, "probe face passes gain
/// light_visibility_mask 0x1"): a lane that makes probe captures see STILL
/// lamps only must add the media mask AND re-introduce a per-kind lamp filter
/// in the two places the deleted hook was called — collectShadowCacheFrame's
/// dirtyLamp (OgreLights.cpp) and applyShadowCacheDirties' `want` list
/// (OgreShadow.cpp) — because a fixed lamp map would otherwise bypass the mask
/// it just added. Both halves or neither.
/// "Did this caster's box move?" (the lamp-map cache's change test). Not a
/// measurement: the same transforms give the same floats frame after frame,
/// so anything above a micro-metre of float noise is a real move — a physics
/// body settling IS moving its shadow.
inline bool boxMovedForShadows(const Ogre::Aabb &a, const Ogre::Aabb &b) {
    const Ogre::Vector3 dc = a.mCenter - b.mCenter;
    const Ogre::Vector3 dh = a.mHalfSize - b.mHalfSize;
    const Ogre::Real eps = Ogre::Real(1e-5) * std::max(Ogre::Real(1), a.mHalfSize.length());
    return std::abs(dc.x) > eps || std::abs(dc.y) > eps || std::abs(dc.z) > eps ||
           std::abs(dh.x) > eps || std::abs(dh.y) > eps || std::abs(dh.z) > eps;
}
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

/// HOW WIDE THE REFLECTION CUTOFF'S FEATHER IS, and it is ONE number for BOTH
/// halves of a per-pixel reflection (owner, ledger §426; lane SSR-3 moved it
/// here and gave the march its share). The cutoff itself is the project's
/// (`ChainDesc::reflectionRoughnessCutoff`, the World panel's "Roughness Cutoff" row,
/// in PERCEPTUAL roughness); this says only that the technique stops SMOOTHLY:
///
///   * the TRACED half runs from full confidence at `cutoff - kRayReflectFeather`
///     to zero at `cutoff + kRayReflectFeather` (OgreRayQuery.cpp pushes it as
///     `projParams.w`), because a ray may be spent above the cutoff and its
///     answer crossfaded;
///   * the MARCHED half runs from full at `cutoff - kRayReflectFeather` to zero
///     AT the cutoff (OgreChain::updateSsr pushes it as `resolveParams.z`,
///     JahSsrResolve_ps.glsl reads it), because the march is SKIPPED above the
///     cutoff — there is nothing there to crossfade.
///
/// THE CONSTANT ITSELF LIVES IN THE PUBLIC HEADER (Types.h) since DRAG-1's
/// second round: a suite that reasons about the band has to read the shipped
/// number rather than copy it. This block is the renderer-side reading of it.

// ---------------------------------------------------------------------------
// What shape of compositor chain a view wants. Phase 1 carries only what every
// view has always had; the effect switches (HDR + tonemap, bloom, SSAO, SMAA,
// SSR, refraction) become extra fields here and extra nodes in OgreChain.cpp,
// and nothing outside those two places has to learn about them.
/// WHERE THE TWO EYES OF A STEREO VIEW ARE, in the world (lane REFLECT-VR-1).
///
/// WHY THE VIEW HAS TO CARRY IT. Under instanced stereo the RENDERING camera is
/// the HEAD: one camera, one position, one projection (the left eye's, so the
/// low-level screen quads' auto-params are an eye's — OgreVrSession.cpp F2),
/// and the per-eye pair rides `Ogre::VrData` where only the Hlms reads it. Any
/// pass of OURS that has to answer PER EYE — the ray-traced reflection is the
/// first — cannot get an eye out of the camera it is handed, and must not guess
/// one: an eye is the runtime's own pose, carried by the rig, and the session
/// is the only thing that knows all three.
///
/// So the session pushes both eyes onto its View each frame, in the frame the
/// eyes were located for, and a pass reads them from there. A view with none
/// (every desktop view, and a session's before its first located frame) says so
/// — `OgreView::stereoEyes()` returns null and the pass declines rather than
/// inventing a mono answer for two eyes.
struct StereoEyeBasis {
    /// The eye in WORLD space, after the world scale and the rig's origin.
    Ogre::Vector3    position = Ogre::Vector3::ZERO;
    /// Which way it looks. The two eyes' orientations are the same on every
    /// runtime measured (OgreVrSession.cpp says so where it builds the head),
    /// but nothing here assumes it.
    Ogre::Quaternion orientation = Ogre::Quaternion::IDENTITY;
    /// Its frustum as TANGENTS of the half-angles, in the same sense as
    /// `Ogre::Frustum::getFrustumExtents(FET_TAN_HALF_ANGLES)`: left and bottom
    /// negative, right and top positive, and asymmetric per eye on real
    /// hardware (which is exactly why one mono frustum cannot serve both).
    float tanLeft = 0.0f, tanRight = 0.0f, tanTop = 0.0f, tanBottom = 0.0f;
};

struct ChainDesc {
    Colour   background;
    bool     shadows = false;   ///< instantiate the process-wide shadow node

    /// THE CHAIN RENDERS AT 1x, BY CONSTRUCTION, AND THAT IS WHY THERE IS NO
    /// SAMPLE COUNT IN THIS DESCRIPTION (CHAIN-MSAA-CRUD, 2026-09-18).
    ///
    /// There used to be `unsigned samples`, the view target's achieved MSAA
    /// count, and `chain::build` opened with `bool msaa = desc.samples > 1`
    /// — and then, 115 lines later, `msaa = false` unconditionally, because
    /// two combinations were reproduced BROKEN on this pin and driver (the
    /// note at that line has the detail: HDR + MSAA segfaults the driver
    /// inside the tonemapped box-filter resolve's pipeline creation, SSAO +
    /// MSAA renders black). Everything downstream of that assignment —
    /// `fsaa` on four targets, the non-MSAA depth copy the refraction pass
    /// sampled, the MSAA HZB seed job, the subsample SSAO downscale, the
    /// store-and-resolve action — was therefore DEAD, reachable only by
    /// deleting the policy line above it. The policy is not a workaround to
    /// be lifted later either: the AA in this engine is SMAA, a post pass
    /// that composes with everything in the chain, and the World Modes table
    /// asks for no tier with both.
    ///
    /// So the count is gone rather than pinned to 1: a field that may only
    /// ever hold one value is not a description, it is scaffolding, and
    /// `sameShape` comparing it made a window's sample-count change look like
    /// a graph change when the graph cannot see it (the window is recreated
    /// for its own reasons — OgreView::setSampleCount). The VIEW still has a
    /// sample count and the on-screen window still honours it; the chain's
    /// own targets are 1x, which is what makes a post-chain picture and a
    /// passthrough picture the same colours.

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
    /// The FIXED form's exposure stated directly as the tonemapper's multiplier
    /// (PostFxDesc::exposureScale). 0 = derive it from `exposure`. Like
    /// `exposure` it is a CLEAR COLOUR, not a graph edit, so it is deliberately
    /// NOT part of sameShape() — OgreView::applyFixedExposure rewrites it live.
    float exposureScale = 0.0f;
    /// THE METER (EXPOSURE-2): its pattern and its percentile clips. UNIFORMS on
    /// the meter's compute jobs, exactly like `exposure` is a uniform on the
    /// resolve — so they are deliberately NOT part of sameShape(): changing the
    /// metering pattern must not rebuild a compositor graph.
    ExposureMeterPattern meterPattern = ExposureMeterPattern::CentreWeighted;
    float meterLowPercent = 10.0f;
    float meterHighPercent = 90.0f;
    bool  bloom = false;            ///< rides the HDR node at ~zero marginal cost
    float bloomThreshold = 5.0f;    ///< bright-pass start, in the sample's units
    float bloomKnee = 2.0f;         ///< ramp WIDTH above it (A-6); 2.0 = the old hard-coded value
    /// HOW MUCH of the blurred highlight the tonemap quad adds (0..2; 1 is the
    /// picture this engine always drew). A UNIFORM, like the exposure and the
    /// threshold next to it, and so deliberately NOT part of sameShape():
    /// scrubbing the amount must not rebuild a compositor graph.
    float bloomAmount = 1.0f;
    bool  ssao = false;
    float ssaoScale = 1.0f;         ///< AO buffer resolution factor (0.5 or 1.0)
    float ssaoPower = 1.5f;
    float ssaoRadius = 2.0f;
    /// THE DITHER'S OFF SWITCH, AND IT IS A DIAGNOSTIC, NOT A DIAL (lane
    /// DITHER-1). The tonemap quad dithers its 8-bit write; this turns that
    /// off so a suite can render the SAME picture both ways IN ONE PROCESS and
    /// measure the difference, which is what makes hdr.dither's discrimination
    /// arms real and what the --engine-selftest hash A/B rests on. There is no
    /// project row and there will not be one: a dither is correctness.
    ///
    /// A UNIFORM, never a shape term (it is deliberately not in sameShape()) —
    /// and per VIEW, pushed by applyViewGlobals immediately before that view's
    /// passes execute, which is the same mechanism that lets two on-screen
    /// views carry two exposures through one process-global material.
    ///
    /// The environment variable JAHSHAKA_NO_DITHER forces it on for the whole
    /// process; it is read ONCE (chain::noDitherEnv) and ORed with this.
    bool  ditherOff = false;
    int   smaaPreset = -1;          ///< -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra
    int   ssr = 0;                  ///< 0 off, 1 half-res rays, 2 HQ
    /// Does the screen-space MARCH contribute (PostFxDesc::ssrScreenMarch)?
    /// False leaves the `ssr` row meaning the reflection's RESOLUTION alone and
    /// the rays its only source — the shape a STEREO chain is forced into, and
    /// the shape a mono control of one eye must be asked for to be comparable.
    /// A GRAPH change: the march's two textures and three quad passes are not
    /// built at all, and `jahSsrReflection` is CLEARED instead of resolved into.
    bool  ssrScreenMarch = true;
    float ssrMaxDistance = 25.0f;   ///< ray length, world units
    float ssrThickness = 0.5f;      ///< assumed surface thickness, world units
    float ssrIntensity = 1.0f;
    /// WHICH SAMPLE ANSWERS THE MARCH'S TWO QUESTIONS (PostFxDesc::ssrMarchPhase).
    /// A UNIFORM, never a shape term: the rule lives inside one shader and
    /// changing it must not rebuild a workspace.
    int   ssrMarchPhase = 0;
    /// THE reflection roughness cutoff, in PERCEPTUAL roughness: the project's
    /// one number for both the screen-space march and the traced ray
    /// (PostFxDesc's note). A uniform, never a shape term.
    float reflectionRoughnessCutoff = 0.4f;
    /// RAY-TRACED REFLECTIONS (PHOTON_SPEC §7 R5). SCREEN FIRST, RAYS FOR THE
    /// REST: the march keeps every pixel it is confident about and a ray fills
    /// only the rest — off screen, behind an occluder, past the edge fade, or
    /// in the roughness band the march declines. It rides the SSR chain (the
    /// prepass' normals and packed roughness are its inputs, `jahSsrReflection`
    /// its output), so it can only be true when `ssr` is, and it is set by the
    /// VIEW from the machine's answer (`Engine::rayQueryAvailable()` and the
    /// `app.rayTracing` preference) rather than by the document: ray tracing is
    /// a capability of the machine (PHOTON_SPEC §4 D4).
    ///
    /// IT CHANGES THE GRAPH, which is why it lives here and not in a per-frame
    /// push: `jahSsrReflection` gains the Uav flag so a compute pass may write
    /// it. Nothing else about the chain moves, and with it false every texture,
    /// pass and pixel is exactly what it was.
    bool  rayReflect = false;
    /// THE SCREEN-PROBE GATHER (SPECS/SCREEN_PROBE_GATHER_SPEC.md phase 1), and
    /// it is here for ONE reason: the gather's probes read their surface from
    /// the PREPASS' depth and normals, and the pixels that read the gather's
    /// answer back are shaded by a pass that must be in `PrePassUse` mode (that
    /// is what declares `iFragCoord`). So the prepass runs for `ssr ||
    /// probeGather` -- a second geometry traversal a gather view pays whether
    /// or not its SSR row asked for one.
    ///
    /// Nothing else about the graph moves: the reflection texture, the march,
    /// the resolve and the colour history are all still the SSR row's, and a
    /// gather-only chain composites no reflection at all (`hlms_use_ssr` is set
    /// by the pin only when the pass carries an ssr texture, which this one
    /// does not). Set by the VIEW from the scene's resolved row, exactly as
    /// `rayReflect` is.
    bool  probeGather = false;
    bool  refractions = false;

    // ---- The hierarchical depth pyramid (SPECS/NANITE_SPEC.md §4.3) ----
    /// Build a closest-depth mip chain of the scene depth, once per frame, right
    /// after the opaque pass. PHOTON SHARED INFRASTRUCTURE and nothing else
    /// today: no pass of this engine reads it yet, so it is OFF everywhere and
    /// costs nothing until a spike asks for it (a stackless screen-space trace
    /// is the first intended consumer). A GRAPH change — one texture and one
    /// compute pass per mip level.
    bool  hzb = false;
    /// How many mip levels the pyramid has, i.e. how many compute passes the
    /// graph carries. Derived from the view's CURRENT size by OgreView (the
    /// compositor auto-resizes a factor-sized texture without rebuilding the
    /// node, so the pass count has to be part of the graph's identity or a
    /// resize would leave passes addressing mips that no longer exist). 0 when
    /// the pyramid is off.
    unsigned hzbLevels = 0u;
    /// WHICH DEPTH EACH LEVEL KEEPS (PostFxDesc::hzbFarthest carries the
    /// argument). It is part of the graph's identity because it is a shader
    /// PROPERTY of the reduce job, i.e. a different permutation.
    bool  hzbFarthest = true;

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

    // ---- Editor furniture, per view (lane PLAYER-1) ----
    /// Does THIS view draw the editor's helper geometry — the grid, the light
    /// and camera wires and icons, the gizmo, the selection shell, the GI
    /// volume boxes (everything Scene::setNodeHelper marks)?
    ///
    /// The Player page is a second View on the EDITOR'S scene, so "the player
    /// shows no wires" cannot be mirror state any more: one mirror pushes one
    /// scene and two views draw it. It is a per-pass VISIBILITY MASK instead —
    /// every scene pass in this view's node gets kHelperBit taken out of its
    /// mask (chain::build) — which is free, needs no second scene and cannot
    /// desynchronise. Backdrops (kBackdropBit: the ground's horizon) and the
    /// sun disc are NOT furniture and stay in every view.
    ///
    /// GRAPH SHAPE (sameShape): the mask lives on the pass DEFINITION, so a
    /// flip re-writes those definitions and the workspace is rebuilt — which
    /// happens once, when the Player page's view is created.
    bool  helpers = true;
    /// Does THIS view draw the VR helper channel — the WEARER's furniture
    /// (kVrHelperBit): the controller proxies, and later the controller ray and
    /// the in-VR gizmo?
    ///
    /// Every VR eye sets it, in both host modes, and so does the desktop EDITOR
    /// viewport. Off everywhere else — a thumbnail, a preview, the Player's
    /// desktop window, the offscreen view a user's screenshot renders through —
    /// so nothing meant for a wearer reaches a picture that is not theirs.
    bool  vrHelpers = false;
    /// Does THIS view draw the RUNTIME'S HIDDEN-AREA MESH (kVrMaskBit, lane
    /// HAM-1)? True for the VR session's eye pair and false everywhere else,
    /// which is what keeps a depth-only near-plane draw out of the desktop, the
    /// mirror, a probe and a user's shot.
    ///
    /// It is the same mechanism as `helpers` and for the same reason — a
    /// per-pass visibility mask on the view's own node, written by
    /// `helperBitsToDrop`, never state on the object (one scene, many views).
    /// GRAPH SHAPE (sameShape) — which is exactly why it is set ONCE, when the
    /// session creates its view, and not when the mask mesh is built: the mesh
    /// can only be built inside a frame (it needs the runtime's located fovs),
    /// and a workspace rebuild there would re-attach the eye copy's own
    /// listener at a seam no suite can see the far side of. With the channel
    /// open and no mask built, nothing in the process carries the bit, so it
    /// costs no pixel and no pass.
    bool  hiddenAreaMask = false;

    // ---- INSTANCED STEREO (SPECS/VR_SPEC.md §4.3, phase 2) ----------------
    /// Render BOTH EYES in one pass into a target that is two eyes wide
    /// (2w x h), the left eye in [0, .5] and the right in [.5, 1].
    ///
    /// It is one flag here and a sweep over the built node (chain::build's
    /// applyStereo): EVERY PASS_SCENE the chosen shape carries — the opaque
    /// pass, the overlay pass, the SSR prepass, the distortion pass, the
    /// refraction pass, the shape's own extra scene passes — gets
    /// `mInstancedStereo`, two viewports and the cull camera. All of them or
    /// none: a shape that stereo-ised its opaque pass and not its overlay pass
    /// would draw the gizmos once, across both eyes, at the left eye's
    /// projection.
    ///
    /// The QUAD passes are deliberately untouched. A post quad reads and writes
    /// the whole 2w x h image, which is right for anything per-pixel (tonemap,
    /// looks, the exposure reduction) and WRONG for anything that samples a
    /// neighbourhood across the middle of the image (SSAO, SMAA, the SSR
    /// march) — those see the seam between the eyes. The VR profile turns them
    /// off rather than teaching each one where the seam is (VR_SPEC §9 item 6);
    /// the flag here does not enforce that, the session's profile does.
    ///
    /// GRAPH SHAPE: it lives on the pass definitions, so it is part of
    /// sameShape() and a flip rebuilds the workspace — which happens exactly
    /// twice, when a session begins and when it ends.
    bool  stereo = false;

    // ---- The LOD switch band, per pass (ogre-patch 0075, ATOM-3-FIX) -------
    /// The hysteresis band `chain::build` writes onto EVERY scene pass of this
    /// view's node (`CompositorPassSceneDef::mLodHysteresis`), as a fraction of
    /// the LOD threshold being crossed. 0 = upstream's behaviour to the bit.
    ///
    /// It is a property of the VIEW, like `overlays` and `helpers`: a band is
    /// what a picture somebody WATCHES OVER TIME wants (the level stops
    /// flickering when the camera breathes at a switch distance), and it is
    /// exactly wrong for a capture, which must take the level its own value
    /// asks for so the same pose gives the same pixels. `OgreView::chainDesc()`
    /// is the one place that answers the question; a planar, probe, shadow or
    /// PiP node is built by another function and never gets the sweep.
    ///
    /// GRAPH SHAPE (sameShape): it lives on the pass definitions, so a change
    /// rewrites them — it never changes for a live view.
    float lodHysteresis = 0.0f;
    /// The camera whose frustum CULLS when `stereo` is set: one camera between
    /// the eyes, wide enough to hold both, so the two eyes cull and light
    /// (Forward+) identically and an object near the edge cannot appear in one
    /// eye and vanish from the other. Empty = cull with the rendering camera,
    /// which is what every non-stereo pass does.
    std::string cullCameraName;

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
/// THE CLEAR-ONLY CHAIN A VIEW OWNS AFTER ITS FIRST SCENE IS TAKEN AWAY (lane
/// STALE-VIEW-1).
///
/// AFTER, and the word is exact: a view that has NEVER been bound has no
/// workspace of any kind — `createView` attaches none, and the seam only ever
/// runs when a scene arrives or leaves. That is deliberate and it is what
/// thumbnails, previews and every pixel suite want: they are created, given a
/// scene and rendered, and a clear before their first bind would be a frame of
/// somebody's background in a picture nobody asked to have one. Studio has no
/// path that shows a never-bound view, so the weaker invariant is the whole
/// story today.
///
/// One clear to `background` and the overlay pass, and nothing else — there is
/// no scene to draw, so there is no scene pass, no shadow node and no effect.
///
/// It exists because a View with NO WORKSPACE PRESENTS NOTHING, and a window
/// that presents nothing keeps whatever frame the X server was last given.
/// Measured on the rig against the unmodified base (spikes/stale-view-1/): in a
/// load IN PLACE that is the one to two frames between the teardown and the
/// moment the host's panel rebuild takes the window off screen — the ~700 ms
/// the user then looks at is the host's own watermark over an unmapped window,
/// which no engine can reach. The bigger half is the other defect with the same
/// cause: the "No world open" panel, raised by every close, changed not one
/// pixel.
///
/// `overlays` is the view's own entitlement (OgreView::overlaysAllowed) — the
/// same gate the scene chain's overlay pass takes, so a view that may not draw
/// the HUD does not start drawing it because its scene went away.
void buildBlank(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                const Colour &background, bool overlays,
                std::vector<std::string> &nodeDefsOut);
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

// ---- The VR mirror (SPECS/VR_SPEC.md §4.3) ---------------------------------
/// One node, one quad: channel 1 (the both-eyes VR target) copied onto channel
/// 0 (the desktop View's own target — a window or an offscreen RTT), through
/// `Jahshaka/VrMirror`. Which HALF is a material parameter, not a graph term
/// (setVrMirrorUv below), so changing eyes rebuilds nothing.
///
/// It is a SECOND workspace on that target, appended after the View's own, so
/// the mirror is painted over the picture the desktop just drew — the same
/// shape the picture-in-picture inset uses, for the same reason (there is no
/// reorder API; the later workspace wins).
void buildVrMirror(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                   std::vector<std::string> &nodeDefsOut);
/// The mirror quad's uv rectangle: scale in xy, offset in zw.
void setVrMirrorUv(float scaleX, float scaleY, float offsetX, float offsetY);

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
Ogre::ColourValue fixedExposureColour(float exposureScale, float exposure);
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
void setExposure(float exposure, float minAutoExposure, float maxAutoExposure);
/// THE METER'S PATTERN AND CLIPS (EXPOSURE-2). Uniforms on the histogram
/// meter's compute jobs; only meaningful for the form that measures.
void setMeter(ExposureMeterPattern pattern, float lowPercent, float highPercent,
               bool stereo);
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
/// is not ours to reinvent. `reprojection` is the view's frame-to-frame state,
/// declared below beside applyViewGlobals.
struct SsrReprojection;
void updateSsr(Ogre::Camera *camera, const ChainDesc &desc, SsrReprojection &reprojection);
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
/// Pushes the dither's diagnostic off switch onto the tonemap material
/// (ChainDesc::ditherOff, ORed with the once-read JAHSHAKA_NO_DITHER). Called
/// from applyViewGlobals; separate only so its teardown twin has a name.
void setDither(bool off);
/// HOW MUCH of the blurred highlight the tonemap quad adds (PostFxDesc::
/// bloomAmount; ogre-patch 0082). A uniform on the same material as the dither,
/// pushed from applyViewGlobals — and from OgreView::setPostFx, for the VR
/// session's view, which the engine's per-frame loop never reaches.
void setBloomAmount(float amount);
/// Drops the cached tonemap parameter block (the dither's off switch and the
/// bloom amount). Called from destroySsao, i.e. from ~OgreEngine, because the
/// cache is a SharedPtr into a material that is about to stop existing.
void forgetTonemapParams();
void applyRecompileGlobals(Ogre::Root *root, const ChainDesc &desc);
/// WHERE A VIEW'S PICTURE WAS ONE FRAME AGO (PAN-SMEAR-1): the world-to-image
/// matrix the view's last frame was drawn with — image = (u, v, the depth
/// buffer's own value). The screen-space reflection resolve reads the PREVIOUS
/// frame's colour, and this is what lets it fetch that colour where the hit's
/// world point WAS rather than where it is now. One per view, owned by the
/// view's ViewGlobalsListener; `have` is false until a frame has been pushed.
struct SsrReprojection {
    Ogre::Matrix4 prevWorldToImage = Ogre::Matrix4::IDENTITY;
    bool          have = false;
};
void applyViewGlobals(Ogre::Root *root, Ogre::Camera *camera, const ChainDesc &desc,
                      unsigned viewWidth, unsigned viewHeight, SsrReprojection &reprojection);

/// The seed value the HDR adaptation history holds for a given exposure — the
/// same `e^(E-2) / 0.18` grey-card constant the fixed tonemap uses, so a
/// re-seeded history starts exactly where a deterministic grade would land.
float exposureSeed(float exposure);

/// THE MULTIPLIER THE FIXED TONEMAP CLEARS ITS 1x1 TEXTURE WITH, in one place:
/// the host's measured value when it handed one over (ChainDesc::exposureScale),
/// else the grey-card constant derived from `exposure`. Both callers — the main
/// chain and the picture-in-picture inset — resolve it here so "0 means derive
/// it" is stated once.
float fixedExposureScale(float exposureScale, float exposure);

/// The name of the 1x1 texture the AUTOMATIC exposure's adaptation history
/// lives in, inside a built chain's scene node. Readable between frames
/// (keep_content, unlike the per-frame `jahLum` it is copied from), which is
/// what makes View::measuredExposureScale possible at all.
const char *exposureHistoryTextureName();

/// One per View, owned by it, registered through OgreView::addWorkspaceListener
/// so it survives every workspace rebuild (the planar listener's shape).
/// Pushes `applyViewGlobals` for its own view, immediately before that view's
/// workspace updates.
class ViewGlobalsListener final : public Ogre::CompositorWorkspaceListener {
public:
    void workspacePreUpdate(Ogre::CompositorWorkspace *) override;
    Ogre::Root *mRoot = nullptr;
    OgreView   *mView = nullptr;
    SsrReprojection mSsrReprojection;
};
}   // namespace chain

// ---------------------------------------------------------------------------
// THE RENDER-LOOP MONITOR — engine half (SPECS/RENDER_LOOP_MONITOR_SPEC.md;
// impl in OgreFrameMonitor.cpp)
// ---------------------------------------------------------------------------
// A DATA COLLECTOR for the lead's engine reviews. It records what each frame
// did and WHY; it judges nothing, draws nothing, and does not exist at all
// while it is off.
//
// THE SHAPE, and why it is a file-scope pointer rather than a member the sites
// reach through the engine: the instrumentation sites are spread over every
// Ogre-private TU (the GI arm's probe sweep, the lamp-map cache, the planar
// reflector, the shader cache, the frame loop), and most of them have no
// OgreEngine in scope. `monitor::live()` is one load and one branch — the
// entire cost of the monitor when it is off. Engine calls are UI-thread-only by
// contract, so nothing here is atomic.
namespace monitor {

/// THE RING'S SIZE. 4096 records is ~68 s at 60 Hz and ~13 s at 300 Hz — more
/// than the 20 s a capture records, so a host that drains on its own tick never
/// loses a frame and a host that forgets keeps the most recent window.
/// Allocated when the monitor goes on and FREED when it goes off.
static constexpr unsigned kRingCapacity = 4096u;
/// Events are far rarer than frames, but a GI-rebuild storm can burst; the
/// queue never grows past this (the overflow is reported, not hidden —
/// MonitorStatus::eventsDropped, which the capture bundle's truncation block
/// reads).
static constexpr unsigned kEventCapacity = 8192u;
/// HOST STAGES WAITING FOR A FRAME. A stage reported outside a frame belongs to
/// the NEXT one, and normally a handful wait (the host's sync tree). But a host
/// that keeps reporting while NOTHING renders — the owner presses Ctrl+F4 and
/// switches to a page with no viewport, so the driver skips every tick — would
/// otherwise bank two entries per tick for the whole capture and hand all of
/// them to the first frame that does render, as one enormous record charged to
/// the wrong frame (lane MON-P1b review, 2026-09-13). Past this many, stages
/// COALESCE BY NAME instead of growing: the total time is preserved exactly,
/// the list is bounded by the number of distinct stage names, and nothing is
/// dropped.
static constexpr unsigned kPendingStageCoalesce = 64u;

/// The per-pass listener. ONE instance for the process, attached to every live
/// workspace while the monitor is on: the view's (through its seam, so it rides
/// every rebuild), each planar mirror's slot and each reflection probe's.
///
/// PASSES NEST, and that is the defect the old PassProfiler had. A scene pass
/// that owns a shadow node executes that node's passes BETWEEN its own
/// pre/post callbacks, on the same workspace and therefore through the same
/// listener (CompositorPass asks its parent NODE's workspace for the listener
/// list, and a CompositorShadowNode belongs to that workspace). A single start
/// time was overwritten by every child. This keeps a STACK and reports
/// EXCLUSIVE time and EXCLUSIVE draw counts, so a frame's records sum to the
/// frame's own totals.
class PassListener final : public Ogre::CompositorWorkspaceListener {
public:
    void workspacePreUpdate(Ogre::CompositorWorkspace *ws) override;
    void workspacePosUpdate(Ogre::CompositorWorkspace *ws) override;
    void passPreExecute(Ogre::CompositorPass *pass) override;
    void passSceneAfterShadowMaps(Ogre::CompositorPassScene *pass) override;
    void passPosExecute(Ogre::CompositorPass *pass) override;
};

/// THE RECORD/SWAP SPLIT. `Root::_updateAllRenderTargets` does two very
/// different things in one call: the compositor update RECORDS the frame's
/// command buffers, then `_swapAllFinalTargets` BLOCKS — fence wait, submit,
/// present, acquire. Between them, and only between them, Ogre fires
/// `frameRenderingQueued` (OgreRoot.cpp:1575-1584). A frame listener is
/// therefore the only way to see where the recording ended and the waiting
/// began, which is the difference between "the CPU is busy" and "the GPU or the
/// display is holding us up". Registered with Root while the monitor is on.
class FrameSplitListener final : public Ogre::FrameListener {
public:
    bool frameRenderingQueued(const Ogre::FrameEvent &) override;
    /// Set on each callback; read by the frame loop when the call returns.
    std::chrono::steady_clock::time_point mMark{};
    bool mMarked = false;
};

/// THE MONITOR ITSELF: the ring of finished FrameRecords, the frame being
/// built, the event queue and the pass stack. One per process, owned by
/// OgreEngine, created when the monitor goes on and destroyed when it goes off
/// — which is what makes "off" cost nothing rather than cost little.
class FrameMonitor {
public:
    FrameMonitor();

    // ---- the frame --------------------------------------------------------
    void beginFrame(unsigned long long frame, FrameCause cause, bool onscreen);
    void endFrame(unsigned scenesUpdated);

    // ---- GPU timestamps (P1c, ogre-patch 0027) -----------------------------
    //
    // A GPU sample comes back TWO FRAMES LATE (the query pool for frame N is
    // read just before it is recycled at the start of frame N+2, behind the
    // availability bit, so nothing ever stalls the CPU on the GPU). A frame
    // record therefore cannot be published the moment it ends: it waits in a
    // short holding queue until its samples arrive or it ages out, and only
    // then enters the ring the host drains.
    static constexpr unsigned kGpuLatencyFrames = 3u;
    /// A frame waiting for its GPU samples, with the sample id of each pass
    /// and of each timed cache row.
    struct PendingFrame {
        FrameRecord           rec;
        std::vector<unsigned> passSampleIds;    ///< parallel to rec.passes
        std::vector<unsigned> cacheSampleIds;   ///< parallel to rec.cacheWork
    };
    /// The level this capture was started at (`Review` today). Held so the
    /// switch is level-sensitive rather than merely on/off.
    MonitorLevel mLevel = MonitorLevel::Review;
    /// Whether this capture is taking GPU samples at all. Set once, when the
    /// monitor goes on, from the render system's own answer.
    bool mGpu = false;
    /// The next sample id. Unique for the life of the capture, so a result that
    /// comes back late can always be attributed to the right pass of the right
    /// frame — and one that belongs to a frame already published is dropped
    /// rather than mis-filed.
    unsigned mNextGpuSampleId = 1u;
    /// Where a sample id's result belongs: which frame in the holding queue,
    /// which row of it, and whether that row is a PASS or a CACHE-WORK entry
    /// (a compute dispatch the compositor never sees).
    struct GpuSampleSlot {
        unsigned frame = 0;   ///< index into mPending
        unsigned row = 0;     ///< index into rec.passes or rec.cacheWork
        bool     cache = false;
    };
    /// id -> where its result goes. Rebuilt as frames retire.
    std::unordered_map<unsigned, GpuSampleSlot> mGpuSampleIndex;
    std::deque<PendingFrame> mPending;
    /// The sample id of each pass of the frame being built, parallel to
    /// `mCurrent.passes`. Empty while GPU sampling is off.
    std::vector<unsigned> mPassSampleIds;
    /// The same, for `mCurrent.cacheWork` — held index-parallel, which is why
    /// `adoptPendingCacheWork` files a 0 for every row it adopts.
    std::vector<unsigned> mCacheSampleIds;
    /// Files a GPU result against the pass that asked for it. Unknown ids (a
    /// frame that already aged out) are dropped.
    void noteGpuSample(unsigned sampleId, float ms);
    /// Moves everything that has waited long enough from the holding queue into
    /// the ring.
    void retirePending(bool all);
    /// The next sample id for a pass about to execute (0 = not sampling).
    unsigned nextGpuSampleId() { return mGpu ? mNextGpuSampleId++ : 0u; }
    bool inFrame() const { return mInFrame; }
    FrameRecord &current() { return mCurrent; }

    // ---- what the instrumentation sites file ------------------------------
    void stage(const char *name, double ms);
    void hostStage(const std::string &name, float ms);
    /// Banks a between-frames stage, coalescing by name past
    /// kPendingStageCoalesce so the pending list cannot grow without bound.
    void bankPending(const std::string &name, float ms);
    void cacheWork(const CacheWork &w, unsigned gpuSampleId = 0u);
    void pass(FramePass &&p, unsigned gpuSampleId = 0u);
    void event(MonitorEvent &&e);
    /// Cache work recorded BEFORE the frame opened — the probe budget and the
    /// lamp-map caster scan both run in the engine's pre-frame half — belongs
    /// to the frame that is about to render it.
    void adoptPendingCacheWork();

    // ---- the host drains ---------------------------------------------------
    unsigned drainFrames(std::vector<FrameRecord> &out);
    unsigned drainEvents(std::vector<MonitorEvent> &out);

    // ---- the pass stack (PassListener's, kept here so it dies with a frame) -
    struct PassFrame {
        std::chrono::steady_clock::time_point start;
        double   childMs = 0.0;
        unsigned drawsAt = 0, batchesAt = 0, instancesAt = 0;
        unsigned long long trianglesAt = 0;
        unsigned childDraws = 0, childBatches = 0, childInstances = 0;
        unsigned long long childTriangles = 0;
        FramePass rec;
        /// The GPU sample this pass opened (0 = none). Held here because the
        /// render system's begin/end hooks are a STACK, exactly like this one.
        unsigned gpuSampleId = 0u;
    };
    std::vector<PassFrame> mPassStack;
    /// The pass-stack depth each OPEN workspace update started at. A workspace
    /// trims back to its own depth when it ends rather than clearing the stack,
    /// so a workspace updated INSIDE an open pass (a probe capture, a planar
    /// mirror) cannot destroy the enclosing pass's record — nor leave its GPU
    /// sample open on the render system's stack.
    std::vector<unsigned> mWorkspaceDepths;
    /// The render system a pass last reported through, used only to close an
    /// orphan's GPU sample. Never dereferenced for anything else.
    Ogre::RenderSystem *mOrphanRs = nullptr;
    /// Closes the innermost open pass that never got its `passPosExecute` —
    /// recorded with unknown numbers and `orphaned`, never invented ones.
    void closeOrphanPass();
    /// The innermost open Stage's child accumulator (Stage manages it).
    double *mStageChild = nullptr;

    // ---- status -------------------------------------------------------------
    unsigned ringFrames() const { return unsigned(mRing.size()); }
    unsigned pendingEvents() const { return unsigned(mEvents.size()); }
    unsigned long long framesRecorded() const { return mFramesRecorded; }
    unsigned long long framesDropped() const { return mFramesDropped; }
    unsigned long long eventsDropped() const { return mEventsDropped; }
    float lastOverheadMs() const { return mLastOverheadMs; }
    void addOverhead(double ms) { mOverheadMs += ms; }

    PassListener      mListener;
    FrameSplitListener mSplit;
    /// HOW MANY workspaces the listener was attached to on the last frame —
    /// a COUNT and deliberately not a list of pointers.
    ///
    /// It used to be a vector of `CompositorWorkspace *`, and detaching walked
    /// it. That was a use-after-free twice over: the list is rebuilt each frame
    /// from the DRAWN scenes, so a scene the editor switched away from kept the
    /// listener while vanishing from the list, and a workspace destroyed
    /// between the last sync and the detach left a dangling pointer in it.
    /// Detaching now walks every live view and every live scene instead
    /// (`OgreEngine::setFrameMonitor`), which is free — Ogre's removeListener
    /// is a find-and-erase — and this number is only ever reported.
    unsigned mAttachedCount = 0u;

private:
    void push(FrameRecord &&r);

    std::vector<FrameRecord>  mRing;
    unsigned                  mWrite = 0u;
    std::vector<MonitorEvent> mEvents;
    std::vector<FrameStage>   mPendingHostStages;
    std::vector<CacheWork>    mPendingCacheWork;
    /// The GPU sample id of each pending row, parallel to `mPendingCacheWork`.
    /// Cache work BETWEEN frames (the mirror's GI half runs before the frame
    /// opens) still takes its timestamp pair — the pair rides the same command
    /// buffer the next frame's passes will — and the id is registered when the
    /// row is adopted.
    std::vector<unsigned>     mPendingCacheSampleIds;
    FrameRecord               mCurrent;
    std::chrono::steady_clock::time_point mFrameStart;
    /// The number of the last frame that BEGAN. Events recorded between frames
    /// are stamped with it rather than with the empty record's 0.
    unsigned long long mLastFrameNumber = 0;
    double   mOverheadMs = 0.0;
    float    mLastOverheadMs = 0.0f;
    bool     mInFrame = false;
    unsigned long long mFramesRecorded = 0, mFramesDropped = 0, mEventsDropped = 0;
};

/// The live monitor, or null. EVERY instrumentation site starts with this: one
/// load and one not-taken branch is the monitor's entire cost when it is off.
extern FrameMonitor *gMonitor;
inline bool live() { return gMonitor != nullptr; }

/// Milliseconds on the monitor's clock (steady_clock), from the capture's zero.
double nowMs();
/// Re-zeroes that clock. Called once, when a capture starts.
void resetEpoch();

/// RAII stage scope: measures a span of the frame and files it under `name`,
/// EXCLUSIVE of any scope nested inside it. Costs one branch when the monitor
/// is off, which is the §4.1 guarantee.
class Stage {
public:
    explicit Stage(const char *name);
    ~Stage();
    Stage(const Stage &) = delete;
    Stage &operator=(const Stage &) = delete;
private:
    const char *mName = nullptr;
    std::chrono::steady_clock::time_point mStart;
    double     *mParentChild = nullptr;
    double      mChild = 0.0;
};

// ---- what the instrumentation sites call (all no-ops while off) ------------

/// A cache did work, for this reason. `ms` negative = the time is already
/// inside the pass records (a probe capture, a shadow map), not measured again.
void noteCacheWork(CacheKind cache, WorkReason reason, unsigned long long id,
                   const char *detail, unsigned units, float ms = -1.0f);
/// RAII: times ONE cache's work and files it as a CacheWork row when it ends —
/// the shape every expensive cached system uses for work it does INSIDE a
/// frame (a GI voxelisation, a light injection, an irradiance-field batch).
///
/// It also brackets the work with a GPU timestamp pair (ogre-patch 0027) when
/// the capture has GPU sampling live, which is the only way a compute dispatch
/// can report GPU time at all: the monitor's other samples ride the compositor
/// pass callbacks, and a compute job the engine dispatches itself is not a
/// compositor pass. The pair is written into the same command buffer as the
/// dispatch and read back two frames later, exactly like a pass's.
///
/// A SCOPE MUST NOT STRADDLE A FRAME BOUNDARY. The render system's sample
/// stack is cleared when the host opens a frame (patch 0027's
/// `JahGpuFrameBegin`, so that a frame which threw cannot corrupt the next
/// one's nesting), and a `begin` on one side of that point with its `end` on
/// the other would pop somebody else's sample. Every call site here is inside
/// one frame or inside the mirror's pre-frame half, never across the edge.
///
/// `detail` is a `const char *` for the same reason EventScope's is: nothing
/// may be constructed at the call site while the monitor is off.
class CacheScope {
public:
    CacheScope(CacheKind cache, WorkReason reason, unsigned long long id,
               const char *detail, Ogre::RenderSystem *rs = nullptr);
    ~CacheScope();
    CacheScope(const CacheScope &) = delete;
    CacheScope &operator=(const CacheScope &) = delete;
    /// What the work turned out to be worth (probes, items, bounces). Read at
    /// destruction, so the call site can set it once it knows.
    void setUnits(unsigned units) { mUnits = units; }
    /// Abandons the row entirely — for a path that decided to do nothing after
    /// all (an early return that built no work).
    void cancel() { mCancelled = true; }
    /// Files the row NOW rather than at the end of the enclosing block, for a
    /// scope whose work ends in the middle of a long function. Idempotent; the
    /// destructor does nothing afterwards.
    void close();
private:
    const char        *mDetail = nullptr;
    Ogre::RenderSystem *mRs = nullptr;
    unsigned long long mId = 0;
    CacheKind          mCache;
    WorkReason         mReason;
    unsigned           mUnits = 0;
    unsigned           mGpuSampleId = 0u;
    bool               mCancelled = false;
    /// The monitor was live when the scope OPENED. A capture that starts in the
    /// middle of one must not file a row timed from an unset clock.
    bool               mArmed = false;
    std::chrono::steady_clock::time_point mStart;
};

/// RAII: times a span and files it as one event when it ends. The shape every
/// expensive, cause-carrying operation in the engine uses (a GI rebuild, a
/// probe grid placement) so that a capture can say how long it took and why.
class EventScope {
public:
    /// `detail` is a `const char *` and not a std::string DELIBERATELY: a
    /// std::string parameter is constructed at the CALL SITE, before the
    /// constructor can test the gate, and §4.1 ("nothing happens while off") is
    /// absolute.
    EventScope(MonitorEventKind kind, WorkReason reason, const char *label,
               const char *detail = nullptr);
    ~EventScope();
    EventScope(const EventScope &) = delete;
    EventScope &operator=(const EventScope &) = delete;
private:
    const char      *mLabel = nullptr;
    const char      *mDetail = nullptr;
    MonitorEventKind mKind;
    WorkReason       mReason;
    std::chrono::steady_clock::time_point mStart;
};

/// A discrete event with its cause.
void noteEvent(MonitorEventKind kind, WorkReason reason, const std::string &label,
               const std::string &detail = std::string(), float ms = -1.0f,
               unsigned long long value = 0);
/// Per-frame counters the frame loop knows and no listener can see.
void noteTextureWait(float ms);
void noteShaderCompiles(unsigned n);
void noteProbeCaptures(unsigned captures);
/// One Photon cascade re-voxelisation, counted on the frame that paid for it.
void noteCascadeRebuild();
void notePlanarRender(unsigned slots);

/// The GI arm's stale reasons and the monitor's are the same vocabulary; this
/// is the ONE translation, kept here so no TU invents a second one.
WorkReason reasonOf(GiStaleReason why);

}   // namespace monitor

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
    struct FileRec { std::string name; unsigned long long bytes = 0; std::string hash; };
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
    /// Serializes every dirty layer HERE and writes it OFF THIS THREAD.
    ///
    /// THE SPLIT, and why it exists (FSYNC-1). Serializing is Ogre's half —
    /// HlmsDiskCache::copyFrom, saveMicrocodeCache, vkGetPipelineCacheData —
    /// and it can only happen on the thread that owns those singletons. The
    /// rest is a megabyte of bytes going to a file, and the `fsync` that makes
    /// the write durable waits behind every other dirty page the filesystem's
    /// journal is holding: 87 ms on an idle disk, 534-1 439 ms measured while a
    /// build's own writeback was queued in front of it. That wait is what the
    /// caller must not take, so the bytes are handed to one writer thread and
    /// this returns.
    ///
    /// False = nothing was handed over (disabled, not dirty, or the
    /// serialization failed). True = the bytes are serialized and the write is
    /// either done or in flight; flushWrites() is how a caller waits for it.
    /// ONE writer, ONE job: a save that meets a write still in flight SKIPS
    /// (the layers stay dirty; the next save writes them) — nothing ever waits
    /// on the calling thread, and nothing queues up behind a slow disk.
    bool save(Ogre::Root *root);
    /// Waits for an in-flight off-thread write. True when the writer is idle,
    /// false when `budgetMs` ran out first (the write continues; nothing is
    /// abandoned). A no-op when no write is in flight.
    bool flushWrites(unsigned budgetMs);
    /// True while the writer holds or is running a job (FSYNC-1): a save that
    /// meets one SKIPS — it never waits on the calling thread.
    bool writeInFlight() const;
    /// Waits for an in-flight write (bounded) and then JOINS the writer thread
    /// (unbounded — the bytes are never abandoned). The engine's destructor
    /// calls it before Ogre's Root, whose LogManager the writer logs through,
    /// is deleted; the ShaderCache destructor calls it again as a no-op.
    void finishWrites();
    /// True when something has been compiled since the last save — the
    /// burst-settle timer's condition, and what makes save() a cheap no-op.
    bool dirty(Ogre::Root *root) const;
    /// Deletes every file we wrote. The running process is unaffected.
    bool clear();

    ShaderCacheStats stats(Ogre::Root *root) const;
    void progress(unsigned &compiled, unsigned &fromCache, unsigned &expected) const;
    /// THE RENDER-LOOP MONITOR'S COMPILE FEED. Ogre exposes no "a shader was
    /// compiled" callback — the counter is a log listener — and with
    /// OGRE_SHADER_COMPILATION_THREADING_MODE=2 it fires on WORKER threads, so
    /// the monitor cannot be touched from there. Instead the listener appends
    /// the compiled shader's NAME to a small bounded, mutex-guarded queue while
    /// `on` is set, and the frame loop drains it on the UI thread.
    /// Off by default: nothing is recorded and no string is built.
    void recordCompileNames(bool on);
    /// Moves what has been recorded since the last call into `out`. Returns how
    /// many compiles happened in that window (which can exceed out.size() when
    /// the bounded queue overflowed).
    unsigned drainCompileNames(std::vector<std::string> &out);

    /// Both out of line: Counter is only defined in OgreShaderCache.cpp, and a
    /// unique_ptr member to an incomplete type needs its owner's special
    /// members compiled where the type IS complete.
    ShaderCache();
    ~ShaderCache();

private:
    struct Entry { std::string name; unsigned long long bytes = 0; std::string hash; };
    /// One dispatched save: the serialized layers, waiting for the writer
    /// thread. `blobs` is parallel to `names`; `compileCount` is the counter
    /// reading this write makes true once it lands.
    struct PendingWrite {
        std::vector<std::string>       names;
        std::vector<std::vector<char>> blobs;
        unsigned                       compileCount = 0;
    };

    /// THE WRITER THREAD's body: publish every blob atomically, carry the
    /// manifest forward, write it, enforce the size cap. Touches no Ogre
    /// object of any kind — that is what makes it safe here.
    bool  runWrite(const PendingWrite &job);
    void  writerLoop();
    /// Hands `job` to the writer (starting it on first use). False when a
    /// write is already in flight.
    bool  dispatchWrite(std::unique_ptr<PendingWrite> job);
    /// Stops and joins the writer. Safe to call twice.
    void  stopWriter();

    /// `adopt` = publish the manifest's saved-at stamp and expected-shader
    /// count into this object (the caller's thread, at load). The writer
    /// thread reads the manifest only to carry entries forward and passes
    /// false: `mExpectedShaders` belongs to the caller's thread.
    bool  readManifest(std::vector<Entry> &filesOut, bool adopt = true) const;
    /// `shaders` is passed rather than read from mExpectedShaders: the writer
    /// thread calls this, and the member belongs to the caller's thread.
    bool  writeManifest(const std::vector<Entry> &files, unsigned shaders) const;
    /// Reads `name`, checks it against the manifest entry, and returns the bytes.
    /// Empty on any mismatch — the caller then wipes.
    bool  readVerified(const Entry &e, std::vector<char> &out) const;
    void  wipe() const;
    /// May THIS run delete the cache directory? Only the writer may
    /// (SHADERCACHE-LOCK-1) — see the definition.
    bool  mayWipe() const;
    bool  acquireLock();
    void  releaseLock();
    std::string path(const std::string &name) const;

    std::string mDir, mFingerprint, mMediaDir, mAppBuildId;
    bool        mEnabled = false;
    bool        mWriter = false;      ///< we hold the single-writer lock
    int         mLockFd = -1;
    unsigned    mExpectedShaders = 0; ///< from the manifest of the last saved run
    /// The compile count at the last drainCompileNames — the monitor's window.
    unsigned    mCompileNamesAt = 0;
    /// Written by the WRITER thread on success, read by stats()/dirty() on the
    /// caller's — atomics, not a lock: two scalars nobody has to read together.
    std::atomic<long long> mLastSavedUnixMs { 0 };
    bool        mPipelineLoaded = false, mMicrocodeLoaded = false;
    /// The driver's verdict on the pipeline blob, scraped from its own log
    /// (ShaderCacheStats::pipelineCacheReason documents the values).
    std::string mPipelineReason = "absent";
    unsigned    mHlmsLoaded = 0;
    /// Microcode-map size right after the load — the baseline compiledThisRun
    /// would use if we had no log listener. Kept for the dirty() shortcut.
    size_t      mMicrocodeAtLoad = 0;
    /// compiled+cached at the last successful write — the "nothing new" test
    /// that stops a clean quit writing the same bytes twice. Published by the
    /// writer thread, so a write that FAILED leaves the cache dirty and the
    /// next save tries again.
    std::atomic<unsigned> mSavedAtCompileCount { 0 };
    /// Set by clear(): the next save writes even though nothing new compiled.
    bool        mForceSave = false;
    /// save() is running. Guards the re-entrant call a nested event loop can
    /// make (see the note at the top of ShaderCache::save).
    bool        mSaving = false;
    class Counter;
    std::unique_ptr<Counter> mCounter;

    // ---- the writer thread (FSYNC-1) --------------------------------------
    // Created on the first dispatch and joined by the destructor. It is a raw
    // std::thread and not a pool: the engine has exactly one of these, it
    // sleeps on a condition variable between saves, and a pool would only add
    // a scheduler between the bytes and the disk.
    mutable std::mutex      mWriteMutex;
    std::condition_variable mWriteCv;       ///< wakes the writer
    std::condition_variable mWriteDoneCv;   ///< wakes a flushWrites() caller
    std::unique_ptr<PendingWrite> mWriteJob;    ///< handed over, not yet taken
    bool                    mWriteBusy = false; ///< a job is being written NOW
    bool                    mWriteStop = false;
    std::thread             mWriteThread;
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
    /// FogDesc::atmosphereColour — read in preparePassHash, where it becomes the
    /// `jah_fog_atmo` shader property that decides whether our media file
    /// replaces upstream's per-vertex sky colour with the authored one.
    bool  atmosphere    = false;
};

class FogHlmsListener final : public Ogre::HlmsListener {
public:
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode *, bool casterPass,
                                   bool, Ogre::SceneManager *) const override;
    float *preparePassBuffer(const Ogre::CompositorShadowNode *, bool casterPass, bool,
                             Ogre::SceneManager *sceneManager, float *passBufferPtr) override;
    /// THE LAMP-MAP CACHE'S SELF-CHECK (ENGINE_CACHE_POLICY_SPEC E2, ogre-patch
    /// 0025). Hlms declares `hlms_num_shadow_map_lights` from the shadow node's
    /// ACTIVE COUNT but indexes shadow maps from the node's SLOT ARRAY; a lamp
    /// fixed into a slot after the node last built its light list makes the two
    /// disagree and generates a pixel shader that references a shadow map it
    /// never declared — which does not compile, and on a cold shader cache used
    /// to take the process down with it. The check is a handful of integer
    /// reads on passes of a node that actually holds a cached lamp, and it is
    /// permanent: this class of defect is invisible until a shader happens to
    /// be generated, so the engine says so the moment the state exists.
    void preparePassHash(const Ogre::CompositorShadowNode *shadowNode, bool casterPass,
                         bool dualParaboloid, Ogre::SceneManager *sceneManager,
                         Ogre::Hlms *hlms) override;
    /// G3-a (PHOTON_SPEC §13 G3): under a CASCADE CHAIN with an irradiance
    /// field bound, re-open the cone-diffuse gate HlmsPbs closes for any bound
    /// field. The field rides cascade 0, so leaving the gate shut would leave
    /// every pixel in the ring out to the outermost cascade with no diffuse
    /// bounce at all. Derived from properties already in the merged set
    /// (`irradiance_field`, `vct_num_probes`), which is what the hook's own
    /// documentation requires of anything set here — see the definition.
    void propertiesMergedPreGenerationStep(Ogre::Hlms *hlms, const Ogre::HlmsCache &passCache,
                                           const Ogre::HlmsPropertyVec &renderableCacheProperties,
                                           const Ogre::PiecesMap renderableCachePieces[Ogre::NumShaderTypes],
                                           const Ogre::HlmsPropertyVec &properties,
                                           const Ogre::QueuedRenderable &queuedRenderable,
                                           size_t tid) override;
    /// How many passes have been hashed in that broken state this session, and
    /// a reset for the suites. Reported by Engine::shadowStatus.
    static unsigned lightCountMismatches() { return sLightCountMismatches; }
    static void     resetLightCountMismatches() { sLightCountMismatches = 0; sMismatchLogged = 0; }

    /// WHEN THE CHECK ABOVE RUNS (clean-2 lane, 2026-09-13). Only a node whose
    /// SLOT ASSIGNMENT changed can have entered the broken state, and there are
    /// exactly TWO places one changes: OgreEngine::applyShadowCacheDirties'
    /// setLightFixedToShadowMap calls, and releaseShadowLamp's release of a
    /// destroyed lamp's slot. Every node either touches is marked here, and the
    /// marks are cleared at the head of the NEXT frame's pass, so every pass
    /// hashed in the frame of a change is checked (the pass count and its
    /// properties differ per pass: the view, six probe faces, each planar arm)
    /// and a frame that changed nothing costs one `empty()` test per pass.
    ///
    /// It matters because the check's own comment said it must not run per
    /// pass: `anyCached` is true for every node in a scene with a cached lamp
    /// — which, since E2 made caching automatic, is every point and spot lamp
    /// in the scene — so the six `_getProperty` calls (each a linear scan of
    /// the merged property vector) ran for the view, every probe face and
    /// every planar arm, every frame, for ever.
    static void noteShadowAssignmentChanged(const Ogre::CompositorShadowNode *node);
    /// Drops the marks; called at the top of applyShadowCacheDirties, i.e.
    /// after the frame that made the assignments has rendered.
    static void clearShadowAssignmentChanges();
    /// How many nodes are marked — a suite read (and the fast path's test).
    static size_t shadowAssignmentChanges() { return sAssignmentChanged.size(); }

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

    /// THE DDGI SHADER STATE (GI_UNIFIED_SPEC.md §4 P1 and the Photon ambient
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

    /// Hands the listener the HlmsPbs singleton it queries on the render thread
    /// for the state of the pass being built (which PCC owns the env-probe
    /// slot). Global, not per scene, because the binding is. Asking HlmsPbs
    /// itself rather than keeping a mirror is what makes the two impossible to
    /// disagree.
    static void setPbs(Ogre::HlmsPbs *pbs);

    /// THE SKY'S OWN ENVIRONMENT SLOT (lane SKY-FALLBACK-1, PHOTON_SPEC §7).
    ///
    /// The PBS pixel shader has ONE env-probe texture (`texEnvProbeMap`), and
    /// under automatic PCC it holds the probe cube ARRAY — so the sky cubemap
    /// comes off every datablock the moment one probe exists
    /// (`OgreScene::reflectionTexFor`, and the long note there says why it
    /// cannot be otherwise: a manual cube in that slot generates a shader that
    /// does not compile). With the probe grid decided PER PROBE (R5-ROOM) a
    /// PARTIAL grid is the normal case — one crate in an open scene keeps 7 of
    /// 18 candidates — and every pixel no probe's shape contains then had no
    /// environment at all but the cones.
    ///
    /// So the sky gets a SECOND slot, at the pass level, through the listener
    /// route HlmsPbs honours for exactly this (`getNumExtraPassTextures` /
    /// `propertiesMergedPreGenerationStep` / `hlmsTypeChanged`; upstream's own
    /// Terra sample is the reference implementation). The probe loop then
    /// blends the sky in with the weight the probes did not claim — see
    /// ogre-patch 0048. The state is per scene because the sky is.
    struct SkyEnvState {
        /// The scene's prefiltered sky cube (`mReflectionTex`), or null: null
        /// is "this scene has no sky reflection", which is also what a Sky
        /// Light at zero gain means (the sky goes out by not being BOUND).
        Ogre::TextureGpu *cube = nullptr;
        /// The Sky Light's gain. It rides here and NOT in
        /// `passBuf.ambientUpperHemi.w` because that pass scale is 1.0 while a
        /// PCC is bound, deliberately (envmapScaleForPass: a probe photographs
        /// real radiance and must not be scaled by the skylight dial).
        float gain = 1.0f;
        /// The cube's own mip count, for the roughness->LOD map. NOT
        /// `passBuf.envMapNumMipmaps`: that is a MAX over every bound
        /// reflection texture and belongs to the probe array here.
        float numMipmaps = 1.0f;
    };
    static void        setSkyEnv(const Ogre::SceneManager *sm, const SkyEnvState &state);
    static SkyEnvState skyEnv(const Ogre::SceneManager *sm);

    /// GATHER-0 — THE SCREEN-PROBE GATHER SPIKE'S PIXEL SIDE (2026-09-21).
    ///
    /// The ray tier registers the full-resolution irradiance texture it has
    /// just written, immediately before the pass that shades with it; this
    /// listener turns that into a PASS property (`jah_probe_gather`), a
    /// claimed extra texture slot (`jahProbeIrradiance`) and one binding — the
    /// same three-hook route the sky's env slot above rides, because there is
    /// no other route into a PBS pass from outside.
    ///
    /// Registered PER SCENE MANAGER and cleared at the head of every frame
    /// (OgreEngine::updateRayQuery), so a view that does not gather cannot
    /// inherit the binding of one that does.
    static void setProbeGather(const Ogre::SceneManager *sm, Ogre::TextureGpu *irradiance);
    static void clearProbeGather();
    static Ogre::TextureGpu *probeGather(const Ogre::SceneManager *sm);

    /// One extra PASS texture — the sky cube — for a colour pass that asked for
    /// it in preparePassHash. Read from the PROPERTIES, never from the state,
    /// because this may be called from any thread and must be a pure function
    /// of the property set (the hook's own contract: a cached shader has to be
    /// reproducible).
    Ogre::uint16 getNumExtraPassTextures(const Ogre::HlmsPropertyVec &properties,
                                         bool casterPass) const override;
    /// ...and here it is bound, at the slot HlmsPbs reserved for it (the first
    /// unit past its own pass textures = `set0_texture_slot_end` - 1, which is
    /// where propertiesMergedPreGenerationStep declared it).
    void hlmsTypeChanged(bool casterPass, Ogre::CommandBuffer *commandBuffer,
                         const Ogre::HlmsDatablock *datablock, size_t texUnit) override;

private:
    /// The sky cube of the pass BEING BUILT, and the samplerblock to bind it
    /// with — both decided in preparePassHash and read in hlmsTypeChanged, on
    /// the render thread, within one pass. Set together or not at all: a slot
    /// claimed by getNumExtraPassTextures and left unbound is an undefined
    /// descriptor.
    static Ogre::TextureGpu             *sPassSkyCube;                 // render thread only
    static const Ogre::HlmsSamplerblock *sPassSkySampler;              // render thread only
    static std::map<const Ogre::SceneManager *, SkyEnvState> sSkyEnv;  // render thread only
    /// GATHER-0's registration and the pass's copy of it — the same
    /// set-together-or-not-at-all rule as the sky's pair above.
    static std::map<const Ogre::SceneManager *, Ogre::TextureGpu *> sProbeGather;  // render thread
    static Ogre::TextureGpu             *sPassProbeGather;             // render thread only
    static const Ogre::HlmsSamplerblock *sPassProbeGatherSampler;      // render thread only
    static Ogre::HlmsPbs *sPbs;                                        // render thread only
    static unsigned       sLightCountMismatches;                       // render thread only
    static unsigned       sMismatchLogged;                             // render thread only
    /// Shadow nodes whose slot assignment changed this frame — see
    /// noteShadowAssignmentChanged. A handful of pointers at most (one per
    /// shadow-node instance in the process), rebuilt per frame, and EMPTY on
    /// every frame that assigned nothing, which is every frame of a still
    /// scene.
    static std::vector<const Ogre::CompositorShadowNode *> sAssignmentChanged;  // render thread only
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

/// THE ONE PASS THAT UPDATES THE MIRRORS, by pass-definition identifier
/// (clean-2 lane, 2026-09-13). `PlanarReflections::update()` renders every
/// active actor's workspace synchronously — a full scene render per mirror,
/// with its own shadow node — and the listener that calls it used to fire on
/// every PASS_SCENE drawn with the view's camera. At Epic that is the SSR
/// prepass, the opaque pass, the refractive pass and the overlays pass: the
/// mirror was rendered three times a frame (measured 1.04 / 0.70 / 0.70 ms GPU
/// and 2.08 / 0.15 / 0.15 ms CPU on the alive room) and the second and third
/// produced a picture nobody sampled. Upstream's own sample gates the same
/// callback on ONE pass by identifier (Samples/2.0/ApiUsage/PlanarReflections),
/// which is what this is: `chain::build` stamps it on the OPAQUE pass — the
/// pass that actually samples the reflection — and nothing else carries it, so
/// the exclusion is by construction rather than by frame order.
constexpr Ogre::uint32 kPlanarUpdatePassIdentifier = 25001u;

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

class OgreScene final : public Scene {
public:
    OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
              std::string &errorSink);
    ~OgreScene() override;

    const std::string &name() const override;

    void setAmbient(const Colour &upper, const Colour &lower) override;
    void setAmbientSh(const float sh[27]) override;
    void setEnvironmentLightScale(float gain) override;

    void setFog(const FogDesc &desc) override;
    /// Creates the scene's AtmosphereNpr (fog only — the sky quad is created and
    /// immediately hidden, the sun/ambient link is never made) or destroys it.
    /// Destroying is what makes "fog off" bit-exact: no atmosphere means no
    /// hlms_fog property, which means the fog code is not in the shader at all.
    /// destroyAtmosphere() MUST run before the SceneManager dies (the component
    /// destroys its Rectangle2D through it).
    void ensureAtmosphere();
    void destroyAtmosphere();
    /// Re-derives the per-scene FogState the shader reads from the last pushed
    /// FogDesc. Called by setFog AND by syncAtmosphere, because the aerial
    /// colour mode depends on whether the analytic sky is bound.
    void pushFogState();
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
    /// THE ANALYTIC SKY (AtmosphereSky): Ogre's AtmosphereNpr quad, shown and
    /// parameterised. The component is the same instance the FOG uses — one per
    /// scene — so the two halves negotiate through mAtmoSkyOn/mAtmoFogOn rather
    /// than each calling setSky()/destroyAtmosphere() behind the other's back.
    bool applySkyAtmosphere(const AtmosphereSky &sky);
    /// Registers/unregisters the component on the SceneManager and shows or
    /// hides its quad from the two flags. The registration is what sets
    /// hlms_fog, so "no fog and no analytic sky" must leave it unregistered.
    void syncAtmosphere();
    bool mAtmoSkyOn = false;   // the analytic sky is the scene's sky
    bool mAtmoFogOn = false;   // the World fog is on
    /// The component's own sky quad, grabbed from the SceneManager's
    /// Rectangle2D list the moment it creates it (the component keeps its
    /// per-SceneManager map private). Needed to move it off render queue 212
    /// and off the default visibility flags — see tuneAtmosphereRenderable.
    Ogre::Rectangle2D *mAtmoQuad = nullptr;
    void tuneAtmosphereRenderable();
    /// WHAT applySkyAtmosphere PUSHED INTO THE COMPONENT, kept because the
    /// component offers no getter for either and atmosphereSunTint has to put
    /// them back after asking it a question about a different sun.
    Ogre::Vector3 mAtmoSunDir = Ogre::Vector3::UNIT_Y;   // the direction the light TRAVELS
    float         mAtmoTimeOfDay = 0.0f;
    /// THE SUN RAY'S AIR (lane SKY-DENSITY-1) — the atmosphere's turbidity, the
    /// one input to atmosphereSunTint. Deliberately NOT a field of the
    /// component's preset: the sky pass never reads it, and the sky's own
    /// density never reaches the sunlight. The default matches
    /// `AtmosphereSky::sunHaze` and `iris::SkyRealistic::defaults()`.
    float         mAtmoSunHaze = 2.5f;
    /// atmosphereSunTint's memo: the direction asked about, the answer, and the
    /// preset generation it was computed under (bumped by every setPreset).
    /// A sun that has not moved costs a compare.
    mutable Ogre::Vector3 mAtmoTintDir = Ogre::Vector3::ZERO;
    mutable Colour        mAtmoTint = Colour(1.0f, 1.0f, 1.0f, 1.0f);
    mutable unsigned long long mAtmoTintGeneration = 0;
    unsigned long long    mAtmoPresetGeneration = 0;
    Colour atmosphereSunTint(const Vec3 &toSun) const override;

    /// THE SKY, CAPTURED ON THE GPU (SKY-GPU) — the one source of a scene's
    /// environment reflections and its ambient SH, for every sky that is not
    /// already a cubemap.
    ///
    /// Six render_scene passes over render queue 0 alone (the sky's queue),
    /// into the six faces of a small cube: whatever the viewport shows as sky
    /// — an equirect photograph, a baked gradient strip, a uniform colour, the
    /// analytic sky's shader — is what the reflections and the ambient are made
    /// of, by construction rather than by a second CPU implementation of the
    /// same picture. It replaces SceneMirror's equirect->cube resample (6x128^2
    /// point samples per sky change on the UI thread) and its SH integral over
    /// the full image.
    void requestSkyCapture();
    /// Runs the capture (workspace update), reads the 32^2 mip back for the SH,
    /// and hands the cube to buildReflectionCubemapFrom. Called from
    /// applyPendingIbl, i.e. inside a frame, where a command buffer exists.
    void applyPendingSkyCapture();
    /// The ambient half of the capture: the cube's 32^2 mip, read back and
    /// integrated into 9 SH bands (the host scales them by its Sky Light).
    ///
    /// THE READ IS ASYNCHRONOUS EXCEPT THE FIRST (render audit ON-14, lane
    /// ENGINE-SMALL-A, 2026-09-18). `flushCommands()` + `map()` is a GPU->CPU
    /// wait on the UI thread, and it ran on every sky CHANGE — every frame of a
    /// sun drag (measured: 0.94 ms of wait + 0.47 ms of integral per change on
    /// this box, and the wait's share is unbounded in principle because
    /// flushCommands submits and waits for whatever the frame had recorded).
    /// Now: the FIRST capture of a scene is synchronous (nothing valid to lag
    /// behind, and a thumbnail renders a handful of frames and asserts their
    /// colours), and every later one issues the download with INACCURATE
    /// tracking and maps it on a later frame — `pollSkyShRead` at the frame's
    /// top — with the previous coefficients staying valid meanwhile.
    /// JAHSHAKA_SKY_SH_SYNC forces the synchronous form for every capture,
    /// which is how the two are A/B'd on one binary.
    void integrateSkyShFromCube(Ogre::TextureGpu *cube);
    void integrateSkyShNow(Ogre::TextureGpu *cube);
    void issueSkyShRead(Ogre::TextureGpu *cube);
    void integrateSkyShFromBox(const Ogre::TextureBox &box);
    /// Called at the top of every frame this scene is drawn in: counts the
    /// gesture's clock and, if the pending read has landed, integrates it.
    /// Never blocks.
    void pollSkyShRead();
    /// The read itself. `force` maps unconditionally — a capture about to
    /// replace the ticket takes its answer first, and by then the copy is a
    /// frame old and free (see the note in OgreSky.cpp).
    void readSkyShTicket(bool force);
    void destroySkyShTicket();
    /// The read in flight, or null. Owned; read (not dropped) by the next
    /// capture, which is one frame later at worst and therefore free.
    Ogre::AsyncTextureTicket *mSkyShTicket = nullptr;
    /// Drawn frames since the last sky capture — the gesture's clock. A capture
    /// within `kSkyCaptureDragFrames` of the previous one is a DRAG and defers
    /// its read; a lone change stays synchronous, so nothing that renders a
    /// handful of frames and asserts their colours moves. Counted by
    /// pollSkyShRead, which runs once per drawn frame.
    unsigned mSkyCaptureIdleFrames = 1000u;
    /// TWO, and the number is a measurement of both sides. A sun being dragged
    /// pushes a new sky on every frame or every other frame (one document edit
    /// per mouse-move event against a 60 Hz loop), so 2 catches every real
    /// gesture; and a HOST or a suite that changes a sky, renders a few frames
    /// and asserts the picture is at 3 or more — mirror.document_to_engine's
    /// red-sky case renders exactly three between pushes, and at a threshold of
    /// 3 it read the previous sky's light. The rule is "the frame after, or the
    /// one after that", not a timer.
    static const unsigned kSkyCaptureDragFrames = 2u;
    bool mSkyCapturePending = false;
    /// The sky's ambient, 9 SH bands x 3 channels, integrated from the captured
    /// cube. Valid only while mSkyShValid; the host scales it by its Sky Light.
    bool skyAmbientSh(float out[27]) const override;
    bool  mSkyShValid = false;
    float mSkySh[27] = { 0.0f };

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
    /// "Is ANY probe grid bound to HlmsPbs", which is the question the env-probe
    /// slot's occupancy really turns on — not "does THIS scene have one". See
    /// the long note above reflectionTexForDatablocks.
    bool anyProbeGridBound() const;
    /// Re-pushes the mip count of whatever this scene's datablocks hold in the
    /// env-probe slot. Called ONLY from the probe-transition walk — see the note
    /// on the definition for why it must not run on every reflection re-apply.
    void renotifyReflectionMipmaps();
    /// Set by destroy() when THIS scene's teardown released the process-wide
    /// probe binding; read by OgreEngine::destroyScene after the erase, which is
    /// the only safe place to walk the remaining scenes.
    bool mReleasedPccOnDestroy = false;
    /// True for the duration of destroy(): teardownVct is shared between "GI
    /// off" (walk the other scenes now) and "this scene is going away" (flag it,
    /// the engine walks after the erase).
    bool mDestroying = false;
    /// Runs the queued ibl_specular convolution (roughness mip chain) for the
    /// reflection cubemap. Called once per frame by the engine, like applyPendingGi.
    void applyPendingIbl();
    Ogre::TextureGpu *mReflectionTex = nullptr;   // prefiltered cube on PBSM_REFLECTION
    /// The environment light's gain (Scene::setEnvironmentLightScale). Rides
    /// `ambientUpperHemi.w`, which is HlmsPbs' envmapScale, so it scales
    /// everything in the env-probe slot — the sky cube and a material's own
    /// reflection override alike. 1.0 is "exactly the cube's radiance", which
    /// is what every scene rendered before this existed, and at exactly 1.0
    /// HlmsPbs does not even set the `envmap_scale` shader property.
    float mEnvLightScale = 1.0f;
    void destroySky();
    bool removeNode(NodeId id) override;

    // ---- Hierarchy and transforms ----
    NodeId createNode(NodeId parent) override;
    NodeId adoptNode(void *nativeSceneNode) override;
    void *nativeSceneManager() const override;
    bool setNodeParent(NodeId id, NodeId parent) override;
    void setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) override;
    void setNodeVisible(NodeId id, bool visible) override;
    void setNodeVisibleUnder(NodeId id, bool visible, bool parentShown) override;

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
    /// ATOM stage 1: the scene-wide LOD dial (OgreMesh.cpp).
    void  setLodBias(float bias) override;
    float lodBias() const override { return mLodBias; }
    void  objectLods(std::vector<ObjectLodDesc> &out) const override;
    bool  meshVaoShape(MeshId mesh, unsigned &levels,
                       unsigned &shadowIndependent) const override;
    /// Writes `errors` (level 1 first, a length in mesh units each) into
    /// `mesh`'s LOD value array at the current bias. Patch 0059 added the
    /// setter this needs.
    void  applyLodValues(const Ogre::MeshPtr &mesh, const std::vector<float> &bounds) const;
    std::string dumpMaterial(MaterialId id) const override;
    bool attachMesh(NodeId id, MeshId meshId, MaterialId matId) override;
    bool setNodeMaterial(NodeId, MaterialId) override;
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
    /// MIRROR-LAMPSIG-1 — the host's word that a light's WORLD pose moved
    /// (Engine.h carries the whole reason: an adopted node's transform never
    /// reaches this interface). One counter bump; safe from any frame.
    void noteLightsMoved() override { ++mGiLightWriteSerial; }
    unsigned long long lightWriteSerial() const override { return mGiLightWriteSerial; }

    // ---- Decals (DECALS_SPEC.md; impl in OgreDecals.cpp) ----
    bool setDecal(NodeId id, const DecalDesc &d) override;
    bool removeDecal(NodeId id) override;
    TextureId loadDecalTexture(const std::string &path, DecalMap kind) override;
    unsigned decalAtlasCapacity(DecalMap kind) const override;
    unsigned decalAtlasUsed(DecalMap kind) const override;

    // ---- Global illumination (PHOTON_SPEC.md; GI_SPEC.md phases 1-3 is its
    // ---- earlier spec, history like the Rayon name) ----
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
    bool setGiTuning(const GiParams &p) override;
    void refreshGlobalIllumination(GiRefreshReason reason) override;
    /// OPEN_COVER_SPEC §2 A — see the boundary's note. Sticky, per scene.
    void setLoading(bool loading) override { mSceneLoading = loading; }
    bool isLoading() const override { return mSceneLoading; }
    GiStatus giStatus() const override;
    /// PHOTON-M3's readback: what the voxel lighting volume holds. Blocks on a
    /// flush and a whole-volume download — a test and tool path (Engine.h).
    GiVoxelStats giVoxelStats(int cascade) override;
    /// THE RAY TIER'S READING for this scene (PHOTON_SPEC §7 R1). Defined in
    /// OgreRayQuery.cpp — like the tier's own members, so that not one line
    /// of the ray tier lives in a TU that does not include Vulkan.
    RayQueryStatus rayQueryStatus() const override;
    GpuSceneStatus gpuSceneStatus() const override;
    bool gpuSceneEntry(unsigned slot, GpuSceneEntry &out) const override;
    bool gpuSceneDeviceEntries(unsigned first, unsigned count,
                               std::vector<GpuSceneEntry> &out) override;
    void measureGpuSceneScan(bool graphIsCurrent) override {
        ensureGpuSceneTimed(graphIsCurrent);
    }
    // ---- SURFACE-CACHE phase 2: the capture cache (SurfaceCache.h) --------
    /// THE PER-FRAME PASS, called once per drawn scene from renderOneFrame —
    /// after applyPendingGi (so a material or light edit has already bumped the
    /// signatures this reads) and before Ogre's own workspaces run. It builds
    /// the cache on the first frame `GiParams::cards` is on, tears it down when
    /// the row goes off, and otherwise spends the tier's texel budget.
    void updateSurfaceCache();
    bool readCardTexel(NodeId node, unsigned card, float u, float v,
                       CardSample &out) override;
    bool readCardAt(const Vec3 &world, const Vec3 &normal, CardSample &out) override;
    bool dumpCardAtlas(const std::string &prefix, std::string &err) override;
    const SurfaceCache *surfaceCache() const { return mSurfaceCache.get(); }
    /// The cards the bake authored for an Ogre mesh, or null for a mesh that
    /// has none (every skinned mesh, every line mesh, every model opened
    /// without a bake). Indexed by `Ogre::Mesh *` because all a cache holds is
    /// an `Ogre::Item *`.
    const std::vector<MeshCardDesc> *meshCardsFor(const Ogre::Mesh *mesh) const {
        auto it = mCardsByMesh.find(mesh);
        return it == mCardsByMesh.end() ? nullptr : &it->second;
    }
    /// ...and its baked LOD BOUNDS, for a consumer that holds only an
    /// `Ogre::Item *` (the cascade voxeliser). THROUGH THE RECORD, never a second
    /// copy of the array (AT-DUP): `mMeshIdByOgreMesh` is an index.
    const std::vector<float> *lodBoundsFor(const Ogre::Mesh *mesh) const {
        auto id = mMeshIdByOgreMesh.find(mesh);
        if (id == mMeshIdByOgreMesh.end()) return nullptr;
        auto rec = mMeshes.find(id->second);
        return rec == mMeshes.end() ? nullptr : &rec->second.lodBounds;
    }
    unsigned long long giMaterialGeneration() const { return mGiMaterialGeneration; }
    std::unique_ptr<SurfaceCache> mSurfaceCache;
    /// THE SCREEN-PROBE GATHER (GATHER-1a). Both are defined in
    /// OgreRayQuery.cpp — like the tier's own members, so that not one line
    /// of the ray tier lives in a TU that does not include Vulkan — and both
    /// answer for the SCENE, not for a view: the row is the project's and the
    /// machine's, and every view of the scene that carries a prepass gathers.
    ///
    /// With the row off the tier records no dispatch, the Component allocates
    /// nothing, the Hlms listener sets no property and no pixel moves.
    bool probeGatherWanted() const;
    void gatherStatusInto(GatherStatus &out) const;
    /// The test-and-tool knobs (Engine.h's `setGatherTuning`): every zero means
    /// "what the tier derives", so the default is the shipped configuration.
    void setGatherTuning(const GatherTuning &t) override { mGatherTuning = t; }
    const GatherTuning &gatherTuning() const { return mGatherTuning; }
    GatherTuning mGatherTuning;
    // --- THE GPU SCENE (A3_GPU_SCENE_SLICE_DESIGN.md; GpuScene.h) -----------
    /// Brings the device-side instance and mesh tables up to date for this
    /// frame's movement epoch. Epoch-gated, and the FRAME's pass is the one that
    /// consumes the epoch: a caller before the frame runs on derived transforms
    /// `updateSceneGraph` has not recomputed, so it may not stop the frame's own
    /// pass (a parent's children would never reach the table). Today the ONLY
    /// caller is the frame, immediately before the ray tier reads the table;
    /// the GI signature walk is NOT converted (see GpuScene.h's header for the
    /// measurement that says why). Defined in OgreGpuScene.cpp.
    /// `graphIsCurrent` = `updateSceneGraph` has already run for this frame, so
    /// every node's cached derived transform is this frame's and the compare is a
    /// cached read. The frame path passes true; a reader BEFORE the frame (the GI
    /// signatures, which the mirror asks for after writing this frame's
    /// transforms) passes false and pays Ogre's recompute, exactly as the walk it
    /// replaces did. MEASURED at 8,001 items in Debug: 1.0 ms vs 3.0 ms.
    void ensureGpuScene(bool graphIsCurrent) const;
    /// The same walk with its cost recorded in `mGpuScanMicros` — the suite's
    /// and the premise's measurement, never a hot path.
    void ensureGpuSceneTimed(bool graphIsCurrent) const;
    /// Creates the tables (idempotent). Called by the first attach as well as
    /// by the first frame, because an Item can arrive before either.
    void ensureGpuTables() const;
    const detail::GpuScene &gpuScene() const { return mGpuScene; }
    detail::GpuScene &gpuScene() { return mGpuScene; }
    unsigned long long gpuScans() const { return mGpuScans; }
    unsigned long long gpuAabbReads() const { return mGpuAabbReads; }
    double gpuScanMicros() const { return mGpuScanMicros; }

    /// DROP THIS SCENE'S acceleration structures (OgreScene::destroy calls it).
    /// A no-op when the tier never held any. Defined in OgreRayQuery.cpp.
    void forgetRayQuery();
    /// THE ONE PREDICATE that decides whether this scene's reflections are
    /// traced (PHOTON_SPEC §7 R5 item 5). It answers the DOCUMENT's half —
    /// "does this project want rays" — resolved against the MACHINE's
    /// (`rayQueryAvailable()`); the view adds the third term, its SSR row,
    /// because that is a property of the view's chain and not of the scene.
    ///
    /// TODAY it reads the application latch (`Engine::rayTracing()`, R1's
    /// `app.rayTracing` / `--no-ray-query`). LANE RAYROW-1 replaces that ONE
    /// line with the project's own World row — off / auto / on, with "on"
    /// additionally raising a scene issue on a machine that cannot trace
    /// (owner, ledger §425). Everything downstream reads this function, so that
    /// lane changes an input and not a pass.
    bool rayReflectionsWanted() const;
    bool traceRays(const std::vector<float> &rays, std::vector<float> &hits) override;
    /// The tier reads this scene's PRIVATE caster epoch (shadowEpoch) to decide
    /// whether the acceleration structure can possibly be out of date — the same
    /// question, and the same answer, as the caster walk's own gate. A friend
    /// rather than a new public getter: nothing outside the ray tier has any
    /// business with that counter, and it lives in the same TU as the walk.
    friend class RayQueryTier;
    bool reassertGiBinding() override;
    unsigned long long giEscapeSignature() const override;
    unsigned long long giGeometrySignature() const override;
    unsigned long long giMaterialSignature() const override;
    bool refreshGiLighting(bool inMotion) override;
    void setNodeGiBoundsExcluded(NodeId id, bool excluded) override;
    bool nodeGiBoundsExcluded(NodeId id) const override;
    void setNodeHelper(NodeId id, bool helper) override;
    void setNodeMovable(NodeId id, bool movable, MobilityChange change) override;
    bool nodeMovable(NodeId id) const override;
    MobilityStatus mobilityStatus() const override;
    bool nodeHelper(NodeId id) const override;
    void setNodeBackdrop(NodeId id, bool backdrop) override;
    void setNodeVrHelper(NodeId id, bool vrHelper) override;
    bool nodeVrHelper(NodeId id) const override;
    void setVrProxyNodes(NodeId left, NodeId right) override;
    void vrProxyNodes(NodeId out[2]) const override;
    void setVrRayNodes(NodeId line, NodeId marker) override;
    void vrRayNodes(NodeId out[2]) const override;
    void setVrHandBoneNodes(unsigned hand, const NodeId *nodes, unsigned count) override;
    unsigned vrHandBoneNodes(unsigned hand, NodeId *out, unsigned count) const override;
    bool nodeWorldPose(NodeId id, Vec3 &position, Quat &rotation) const override;
    bool nodeBackdrop(NodeId id) const override;
    void setNodeLightMask(NodeId id, unsigned mask) override;
    unsigned nodeLightMask(NodeId id) const override;
    void setNodeCastShadow(NodeId id, bool on) override;
    bool nodeCastShadow(NodeId id) const override;

    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md; impl OgrePlanar.cpp) ----
    bool setPlanarReflections(const PlanarReflectionParams &p) override;
    bool setNodePlanarReflector(NodeId id, bool on) override;
    bool nodePlanarReflector(NodeId id) const override;
    int  activePlanarReflectors() const override;

    // ---- Hardware ray tracing, per scene (ledger §425; impl OgreScene.cpp) ----
    /// Defined in OgreScene.cpp: a flip to Off also releases the scene's ray
    /// structures (forgetRayQuery) so Off costs nothing, as the verb promises.
    void setRayTracing(RayTracingMode mode) override;
    RayTracingMode rayTracingMode() const override { return mRayTracing; }
    bool rayTracingResolved() const override;
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
    /// every probe workspace instantiates JahshakaProbeShadowNode too, so deleting
    /// the definition under them leaves live CompositorShadowNodes pointing at
    /// freed memory. Reproduced as a SEGV in Hlms::preparePassHashBase
    /// (tests/shadow, mode r3) before this existed. Returns true when the arm
    /// was dropped and the caller must call the recreate below.
    /// THE SHADOW-ATLAS REBUILD'S GI HALF (PHOTON_SPEC G2). Drops exactly the
    /// WORKSPACES that instantiate the probe shadow node — each PCC probe's —
    /// and nothing else. The voxels, the cascade chain and
    /// the probes' own shapes survive: a shadow atlas growing on the 3rd, 5th or
    /// 9th casting lamp is shadow bookkeeping, not a statement about the scene's
    /// geometry. Returns false when this scene holds no such workspace.
    bool dropGiForShadowRebuild();
    void recreateGiAfterShadowRebuild();
    /// The near/far the probe cameras were placed with — remembered so the
    /// workspaces above can be re-created exactly as they were (G2).
    float mProbeCamNear = 0.5f, mProbeCamFar = 500.0f;
    /// THE LAMP-MAP CACHE'S REACH INTO THIS SCENE'S PRIVATE WORKSPACES
    /// (ENGINE_CACHE_POLICY_SPEC P4/P5): the live workspaces that instantiate
    /// the shadow node of `kind` — each planar budget slot (Reflect) and each
    /// reflection probe whose captures are shadowed (Probe). A view's own node
    /// belongs to the view (OgreView::shadowNodeInstance), so View appends
    /// nothing here. Empty when the arm is off or unshadowed.
    void shadowWorkspaces(ShadowNodeKind kind,
                          std::vector<Ogre::CompositorWorkspace *> &out) const;
    /// EVERY live workspace this scene privately owns — each planar mirror
    /// slot's and each reflection probe's — whether or not it is shadowed. The
    /// render-loop monitor attaches its pass listener to these (a view's own
    /// workspace belongs to the view); `owners`, when given, receives a label
    /// per workspace ("planar:0", "probe:7") for the capture's snapshot.
    void monitorWorkspaces(std::vector<Ogre::CompositorWorkspace *> &out,
                           std::vector<std::string> *owners = nullptr) const;
    /// The GI parameters as LAST APPLIED — what was asked for, beside what
    /// giStatus() says it resolved to. The capture snapshot carries both.
    const GiParams &giParams() const { return mGi; }
    /// The planar-reflection arm's parameters, same contract.
    const PlanarReflectionParams &planarParams() const { return mPlanarParams; }
    /// Each live reflection probe's placement and dirty state, for the snapshot.
    void collectProbeInfo(std::vector<ProbeInfo> &out) const;
    /// Each light this scene holds, with its shadow-cache state filled in by
    /// the caller from ShadowStatus (which is process-wide).
    void collectLightInfo(std::vector<SnapshotLight> &out) const;

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
        /// WHY each entry of `dirty[k]` is dirty, in the same order — the lamp's
        /// own inputs changed (Light), a caster inside its reach moved
        /// (Caster), or everything was dirtied at once (Request). Read by the
        /// render-loop monitor, which records a shadow-map render with the
        /// input change that justified it, or `None`.
        std::vector<WorkReason> dirtyReason[kShadowNodeKinds];
        unsigned casterChanges = 0;   ///< caster boxes that changed this frame
        unsigned lightChanges  = 0;   ///< lamps whose own inputs changed
        /// EMPTIED, NEVER REBUILT. One of these is filled per drawn scene per
        /// frame, for ever; assigning a fresh instance freed and re-allocated
        /// seven vectors every time (clean-2 lane, 2026-09-13). clear() keeps
        /// the capacity, so a steady scene allocates nothing here at all.
        void clear() {
            lights.clear();
            dirtyAll = false;
            for (unsigned k = 0; k < kShadowNodeKinds; ++k) { dirty[k].clear(); dirtyReason[k].clear(); }
            casterChanges = lightChanges = 0;
        }
    };
    /// Runs ONCE per drawn scene per frame, AFTER SceneManager::updateSceneGraph
    /// and before any workspace renders, so world AABBs and light poses are
    /// this frame's (no getWorldAabbUpdated root recursion, and a mover's
    /// shadow updates in the frame it moves).
    void collectShadowCacheFrame(ShadowCacheFrame &out);
    /// THE FRAME'S CASTER WALK (OgreGi.cpp): the lamp-map cache's caster scan
    /// over mItemNodes, after updateSceneGraph. Called by
    /// OgreEngine::applyShadowCacheDirties for every drawn scene. `shadow`
    /// false = this scene caches nothing this frame (the records are dropped).
    void runItemWalk(bool shadow);
    /// A TEXTURE THAT ARRIVED LATE IS A LATE PROBE INPUT (clean-2 lane,
    /// 2026-09-13). `setPbrTexture` stales the grid when the bind happens, but
    /// a texture is only SCHEDULED there: what the probes captured that frame
    /// was the stub, and the frame the pixels actually arrive is nobody's
    /// input. Materials bound to a not-yet-resident texture are parked here and
    /// re-noted on the frame the last of their textures becomes ready. Called
    /// once a frame per scene, right after the engine's texture drain; returns
    /// immediately (one empty() test) when nothing is parked, which is every
    /// frame after a scene has finished loading.
    void settleTextureResidency();
    /// Whether the GI flush should wait for a voxel-input texture that is still
    /// streaming (BOOTVOX-1). Counts the frames it has waited, so it is not
    /// const. See the definition in OgreGi.cpp.
    bool giVoxelTexturesPending();
    /// THE GI MOVEMENT SCAN, once per frame, run by its consumer (the
    /// probe budget) — which is EARLIER in the frame than
    /// any scene graph update, so it reads updated bounds. Same pass, same
    /// per-node records as the caster walk (walkItems).
    void ensureGiWalk();
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
    /// PER NODE: one animating character must not re-dirty the lamps near every
    /// other rig.
    void noteNodePosed(NodeId id);
    /// What the per-frame walks cost on the last frame, in microseconds
    /// (steady clock): the lamp-map cache's own half (collectShadowCacheFrame:
    /// the lamps and the change-to-lamp test), the caster walk (runItemWalk)
    /// and the GI movement scan (ensureGiWalk).
    /// Internal diagnostics — read by tests/shadow's `scancost` mode.
    double shadowScanMicros() const { return mShadowScanMicros; }
    double casterWalkMicros() const { return mCasterWalkMicros; }
    double giScanMicros() const { return mGiScanMicros; }
    /// ITEM VISITS made by the CASTER half of the walk, ever (ENGINE-4 F5).
    /// The still-frame statement in one number: a frame in which nothing that
    /// a lamp map depends on changed must not move it at all.
    unsigned long long casterWalkItems() const { return mCasterWalkItems; }
    /// AN INPUT TO THIS SCENE'S GI SCANS CHANGED — a transform this scene
    /// itself wrote (setNodeTransform, a socket rider's placement, a decal's
    /// box), or a STRUCTURAL change that moves what the scans would read
    /// (an Item arriving or leaving, a visibility/helper/mobility flag, the
    /// lit volume). The host's transform counter cannot see any of it.
    ///
    /// It is the same counter for both because every reader wants the same
    /// question answered — "could the answer have changed since I last
    /// looked?" — and a false yes costs one scan.
    void noteSceneTransformWrite() { ++mSceneTransformWrites; }
    /// AN INPUT TO THE CASTER HALF OF THE WALK CHANGED, and it is not a
    /// transform (ENGINE-4 F5): a rig posed, a caster-shape seam (a mesh or
    /// material swap, a rebuilt Item, a per-object Cast Shadow flag, a
    /// generated vertex piece), a render-queue refile. The GI half's epoch
    /// cannot carry these — none of them moves anything — so the caster half
    /// adds its own counter on top of it (shadowEpoch below).
    void noteShadowScanInput() { ++mShadowScanWrites; }
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
        /// Set by noteShadowShapeChanged / updateMeshVertices / a rebuilt Item;
        /// consumed by the next caster scan.
        bool             shadowShapeDirty = false;
        /// THE ITEM WALK'S MEMORY (walkItems — the GI movement scan and the
        /// lamp-map cache's caster scan, one pass): what this node's Item looked
        /// like the last time a walk saw it. On the node, not in per-scene hash
        /// maps, so a walk is a vector of pointers and no lookups.
        struct ScanRec {
            Ogre::Aabb          giBox, probeBox, shadowBox, decalBox;
            const Ogre::Item   *shadowItem = nullptr;
            unsigned long long  shadowPose = 0;
            Ogre::uint32        shadowChannels = 0;
            bool                giKnown = false, probeKnown = false, shadowPresent = false;
            bool                decalKnown = false;
            /// THE GI TICK THIS ITEM'S BOX LAST MOVED AT (MOVER-1). What makes
            /// a GESTURE tellable from a nudge on the VOXEL side, exactly as
            /// `mProbeMotionRun` does on the probe side and with the same
            /// window: a second move within kProbeMotionSettleFrames ticks is a
            /// gesture, one move on its own is an edit. 0 = never seen moving.
            /// It cannot be a per-frame run counter, because the editor renders
            /// about two frames per document edit and a mover's boxes therefore
            /// arrive on alternate frames (DRAG-1's measurement).
            unsigned long long  giMovedTick = 0;
        } scan;
        /// This node's own id. The item index (mItemNodes) is a vector of Node*,
        /// so a walk that has to name the nodes it found — the GI item set —
        /// needs the id on the node rather than a second walk of the map.
        NodeId           selfId = 0;
        /// This node's place in OgreScene::mItemNodes, or npos (no Item).
        size_t           itemSlot = size_t(-1);
        /// ...and its MESH's place in the GPU scene's mesh table (GpuScene.h),
        /// acquired at attach and released at detach. kNoMesh until geometry
        /// arrives, which is what an instance entry with no mesh reads as.
        uint32_t         gpuMeshSlot = 0xFFFFFFFFu;
        /// ...and in OgreScene::mDecalNodes (a decal is not an Item, and a
        /// decal node usually carries no Item at all, so the movement scan
        /// would never see it — clean-2 lane, 2026-09-13).
        size_t           decalSlot = size_t(-1);
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
        /// A helper that is PART OF THE PICTURE (kBackdropBit's note): the
        /// ground's horizon plane. Implies `helper` — same exclusion from every
        /// capture — but carries kBackdropBit instead of kHelperBit, so a view
        /// that masks the editor's furniture out still draws it.
        bool                      backdrop = false;
        /// ALSO IN THE VR HELPER CHANNEL (kVrHelperBit's two-bit rule): a
        /// helper the HEADSET draws as well as the desk. Adds kVrHelperBit
        /// BESIDE kHelperBit rather than instead of it, which is the one case
        /// the inversion scheme needs two bits for — the controller proxies
        /// (and phase 4b's ray and hit marker) are the wearer's furniture and
        /// belong in every eye, in both modes. Meaningless (and ignored) on a
        /// node that is not a helper at all.
        bool                      vrHelper = false;
        /// MOBILITY, as the document RESOLVED it (REALTIME_REFLECTIONS_SPEC
        /// §3.3). True = this node moves, and the renderer keeps it OUT of the
        /// still-world layer: its item carries kMovableBit instead of
        /// kVisibleBit and never kGiGeometryBit, so it leaves the probe
        /// captures, the voxel bounce, every GI gather and the probe-kind
        /// shadow maps, while the view, the planar mirrors, SSR and the
        /// view/reflect shadow maps keep drawing it every frame.
        bool                      movable = false;
        /// THE DRAGGED STILL, RIDING THE MOVER CHANNEL FOR THE LENGTH OF A
        /// GESTURE (MOVER-1, GiParams::dragMoverChannel — OFF by default).
        ///
        /// It is the renderer's own transient state and never the document's:
        /// `movable` is what the host RESOLVED and this is what the gesture
        /// borrowed, so the two are ORed wherever a channel is decided and the
        /// document's answer is untouched when the gesture ends. While it is
        /// set the item carries kMovableBit and no kGiGeometryBit exactly as a
        /// Movable one does — it leaves the voxel bounce and the probe captures
        /// and is lit by the field and the cones at its live pose — and the
        /// cascades pay ONE re-voxelisation at each end of the gesture instead
        /// of one per frame of it.
        bool                      dragMover = false;
        /// THE SOFT PROMOTION'S HEALING KEY (owner decision O3). While a node is
        /// softly promoted it carries no GI bit but its voxels were never
        /// cleared — that stale bounce IS the documented ghost. If a
        /// FROM-SCRATCH GI build happens meanwhile (a settle after a light edit,
        /// a material change, another object's classification), the voxels are
        /// rebuilt WITHOUT it and the ghost becomes a hole that would outlive
        /// play. So the promotion records the rebuild count it was made at, and
        /// the push that clears it invalidates when that count has moved.
        /// Meaningful only while `movable && mobilitySoft`.
        bool                      mobilitySoft = false;
        unsigned long long        mobilitySoftRebuilds = 0;
        /// LIGHTING CHANNELS, object side (Scene::setNodeLightMask). Kept here
        /// rather than read back off the Item because the Item is REBUILT on
        /// every attachMesh/attachSkinnedMesh (a material swap destroys and
        /// recreates it) and because a host may set the mask before any
        /// geometry exists — both cases would silently lose it otherwise.
        /// Ogre's own default (MovableObject::msDefaultLightMask, and it is
        /// genuinely consulted at OgreObjectDataArrayMemoryManager.cpp:138).
        Ogre::uint32              lightMask = 0xFFFFFFFFu;
        /// PER-OBJECT SHADOW CASTING (Scene::setNodeCastShadow). Remembered
        /// here for the same two reasons the light mask is: an Item is REBUILT
        /// on every attach, and a host may say "this never casts" before the
        /// geometry arrives.
        bool                      castShadow = true;
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
        /// ENGINE-OWNED REGISTERED CHILDREN this node has ever been given
        /// (createNode with a parent, setNodeParent onto it): a gizmo slot, a
        /// selection wire, a bone-overlay bone. The visibility walk descends
        /// into those and into the two unregistered helper children named
        /// above, and into nothing else — so a node with none of them needs no
        /// per-child registry lookup at all (ledger 179). It only ever grows:
        /// over-counting costs one lookup per child on a node that once had an
        /// engine-owned child, under-counting would lose a wire's visibility.
        unsigned                  ownedChildren = 0;
    };

    /// THE ONE PLACE `shadowShapeDirty` IS RAISED (ENGINE-4 F5), so that
    /// raising it and telling the caster walk's still-frame gate about it
    /// cannot come apart. Here rather than beside noteShadowScanInput because
    /// it needs `Node` to be complete.
    void markShadowShapeDirty(Node &n) { n.shadowShapeDirty = true; noteShadowScanInput(); }

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
        /// ATOM stage 1: the per-level MEASURED BOUNDS this mesh was built with
        /// (a length in mesh units, level 1 first — level 0 has none). Empty for
        /// a mesh with no LOD chain, which is most of them. Kept so a LOD-bias
        /// change can re-derive the mesh's switch distances without rebuilding a
        /// buffer: the Items hold a POINTER to the Ogre mesh's value array, so
        /// rewriting that array in place moves every instance.
        ///
        /// THIS IS THE ONLY COPY (ATOM inventory row AT-DUP). There used to be a
        /// second one in `mLodErrorsByMesh`, a whole vector per mesh duplicated
        /// so that a consumer holding only an `Ogre::Item *` could reach it; that
        /// map is now an INDEX to this record (`mMeshIdByOgreMesh`) and the data
        /// lives here alone.
        std::vector<float> lodBounds;
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
        /// THE OVERLAY'S BLEND STATE (GIZMO-2 item 4). An unlit overlay
        /// material's blendblock is chosen from its colour's ALPHA at creation,
        /// and setUnlitMaterial can move a live material across that line — a
        /// blendblock is a whole rasterizer state, not a shader constant, so it
        /// has to be swapped rather than written. This is what it currently is,
        /// so the swap happens only on a real crossing.
        bool blended = false;
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
    /// HlmsPbs' envmapScale for the next pass: the environment light's gain,
    /// except while PCC owns the env-probe slot (see the note at the definition).
    float envmapScaleForPass() const;
    /// Re-writes the ambient pass data with the CURRENT envmapScaleForPass().
    /// Runs on every PCC binding transition, not only on a Sky Light edit.
    void refreshEnvmapScale();
    /// Any live PBR material carrying its OWN reflection cubemap. Such a pass
    /// keeps envmapScale at 1.0 (see the note at envmapScaleForPass): the
    /// author's map is not the sky and the Sky Light's gain must not dim it.
    bool hasAuthoredReflectionMap() const;
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
    /// THE SAMPLERBLOCK REFERENCE CEILING (ENGINE-6 item 2, ENGINE-5 review F2).
    /// Every textured slot on every datablock holds a REFERENCE on the
    /// samplerblock it was bound with, `BasicBlock::mRefCount` is a uint16, and
    /// its overflow check is a debug-only assert: at five real maps per
    /// material, 13,108 textured materials sharing one sampler wrap it, the
    /// block is freed under live datablocks and the next ~HlmsPbsDatablock
    /// throws out of a noexcept destructor (the exit-134 class ENGINE-5 fixed
    /// for EMPTY slots — this is the same ceiling reached honestly). Hands back
    /// the params to bind with: the same sampler until its block nears the
    /// ceiling, then a fresh block with identical FILTERING, differing only in
    /// a max-LOD clamp far above any mip a texture can have.
    Ogre::HlmsSamplerblock guardSamplerCeiling(Ogre::HlmsSamplerblock sampler);
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
    /// THE SUN DISC (SKY_LIGHT_SPEC.md §3): creates (once) and updates the disc
    /// quad — a Rectangle2D at render queue 1, additive, with kSunDiscBit. A
    /// disabled disc hides the quad rather than destroying it (the sun is
    /// switched on and off, not created and destroyed).
    void applySunDisc(const SunDisc &sun);
    void destroySunDisc();
    /// THE SHADER GRID (GRID-2, OgreGrid.cpp): the sun disc's mechanism — a
    /// Rectangle2D whose fragment program intersects the camera ray with the
    /// grid plane and writes that point's depth. A disabled grid hides the
    /// quad (the grid is toggled every day, not created and destroyed).
    bool setGrid(const GridDesc &) override;
    void destroyGrid();
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
    /// Ogre::VctVoxelizer caches raw pointers twice over: addItem keeps raw
    /// Item* until removeAllItems, and VctMaterial caches conversions by raw
    /// datablock pointer across builds. So every geometry/material/texture
    /// destroy path calls this BEFORE the object actually dies. The rebuild
    /// still happens ONCE at frame time (bursty destroys = one rebuild); the
    /// flush tears the whole arm down and re-voxelizes from the LIVE scene — or,
    /// under a cascade chain, marks only the cascades the change reaches (G1) —
    /// so a recycled pointer can never alias.
    void invalidateGiCaches() { invalidateGiCaches(nullptr, true, true); }
    /// ...with the world box the edit touched, where the call site knows it:
    /// under a cascade chain that box is what decides which cascades owe a
    /// rebuild (G1). nullptr = "somewhere in the scene".
    void invalidateGiCaches(const Ogre::Aabb *where) { invalidateGiCaches(where, true, true); }
    /// A HIDE OR A SHOW — GEOMETRY LEFT OR JOINED THE GI SET, AND NOTHING DIED
    /// (DRAG-1, RENDER_AUDIT I-2). Every other caller of this family is a
    /// DESTRUCTION (a node, a mesh, a material, a texture, a light) and bumps
    /// the destruction generation, which is what makes the single-volume reuse
    /// arm refuse and pay a from-scratch teardown-and-rebuild of every
    /// voxeliser. A visibility edge destroys nothing at all: the Item, its
    /// mesh, its datablock and every pointer the GI arms hold are alive and
    /// unchanged — only the item SET the voxels should describe has changed,
    /// and the reuse arm answers that by re-selecting (see refreshVctFast) and
    /// the chain by `itemsStale` at each cascade's next rebuild. So a hide
    /// became MORE expensive than a delete, which is upside down.
    void invalidateGiCachesForVisibility(const Ogre::Aabb *where) {
        invalidateGiCaches(where, true, false);
    }
    /// `geometryVoxelsChanged == false` says NOTHING A VOXEL HOLDS MOVED — a
    /// LIGHT left the scene. The destruction generation still moves (Instant
    /// Radiosity's by-pointer caches, the single arm's reuse rule) and the
    /// cascades still re-select their item sets, but no cascade owes a
    /// RE-VOXELISATION: a light is answered by a re-injection, which is what the
    /// dirty path does when nothing geometric is marked (G1).
    void invalidateGiCaches(const Ogre::Aabb *where, bool geometryVoxelsChanged,
                            bool somethingDied);
public:
    /// ADDS this scene's registry sizes into `out` (nodes/meshes/materials/
    /// textures). Additive because Engine::objectCounts sums every live scene
    /// into one census — see ObjectCounts. Touches nothing else in `out`.
    void addObjectCounts(ObjectCounts &out) const;
    /// Called by Engine::renderOneFrame before rendering.
    void applyPendingGi();
    /// The flush proper — what applyPendingGi was before the staged machine.
    void applyPendingGiFlush();
    /// The pace of the frame being rendered (OPEN_COVER_SPEC §2.1):
    /// Engine::renderOneFrame pushes it once per frame, to every scene.
    void setFramePace(FramePace pace) { mFramePace = pace; }
    /// Does this scene still owe a stage of a staged arm build? Read by
    /// Engine::framePaceOwesWork.
    bool giBuildOwesWork() const;
    /// HOW MUCH of a staged arm build is still owed, and which stage is next
    /// (OPEN_COVER_SPEC §2.1, lane OPEN-COVER-2b). The public mirror of
    /// `mGiBuildStage`, which names the stage that will run NEXT — so `Field`
    /// is one stage from done and `ProbeScout` is four. Inline and const: this
    /// is read once per frame while a world streams in, by the indicator and by
    /// the slow-frame line that names what a frame spent.
    unsigned giBuildStagesLeft() const {
        switch (mGiBuildStage) {
        case GiBuildStage::Idle:        return 0u;
        case GiBuildStage::ProbeScout:  return 4u;
        case GiBuildStage::ProbeFit:    return 3u;
        case GiBuildStage::ProbeFinish: return 2u;
        case GiBuildStage::Field:       return 1u;
        }
        return 0u;
    }
    GiArmStage giBuildStage() const {
        switch (mGiBuildStage) {
        case GiBuildStage::Idle:        return GiArmStage::None;
        case GiBuildStage::ProbeScout:  return GiArmStage::ProbeScout;
        case GiBuildStage::ProbeFit:    return GiArmStage::ProbeFit;
        case GiBuildStage::ProbeFinish: return GiArmStage::ProbeFinish;
        case GiBuildStage::Field:       return GiArmStage::Field;
        }
        return GiArmStage::None;
    }
    /// Materials whose bound texture has not finished streaming — the entries
    /// `settleTextureResidency` spends. The indicator's "textures" term.
    unsigned materialsAwaitingTexture() const {
        return unsigned(mMaterialsAwaitingTexture.size());
    }
    /// Whole-arm rebuilds this scene has done — `StreamingWork::giRebuilds`.
    unsigned long long giRebuildCount() const { return mGiRebuilds; }
    /// Is a deferred read of a texture's PIXELS owed this frame? True while the
    /// sky's IBL convolution is pending — the one piece of per-frame work that
    /// reads texture contents and cannot be asked twice (OPEN_COVER_SPEC §2 E).
    bool iblReadPending() const { return mIblPending; }
    /// Called by Engine::renderOneFrame once the frame's GI work is decided and
    /// before it renders: records how many probes this frame re-captures
    /// (GiStatus::probeCapturesLastFrame). `drawn` = a View draws this scene
    /// this frame (an undrawn scene's dirty probes do not render).
    void latchProbeCaptures(bool drawn);
    /// Called by OgreView each frame with its camera position: the PCC probe
    /// blend tracks the viewer. No-op unless the hybrid mode is live.
    void updateGiTracking(const Ogre::Vector3 &camPos, bool driverStereo);
    /// Re-derives the Forward+ clustered depth-slice range from this camera and
    /// the scene's own extent (LIGHTING_FIX fix 8 / F-F1). Rate-limited AND
    /// hysteretic — `setForwardClustered` recreates the grid buffers, so it must
    /// never run per frame.
    void updateForwardPlusRanges(const Ogre::Camera *cam);
    /// How many lights this scene holds that Forward+ has to fit into its
    /// per-cell budget, and the budget itself (F-F2). See RenderStats.
    void forwardPlusLightCensus(unsigned &lights, unsigned &budget) const;
private:
    /// Voxelizes the scene's PBR items over computeGiBounds at quality-mapped
    /// resolution, (re)builds VctLighting and binds it to HlmsPbs. The voxelizer
    /// and lighting are recreated from scratch every time (see invalidateGiCaches).
    /// In hybrid mode also (re)builds the PCC probe grid.
    /// TRUE when the arm was actually (re)built (see the definition): the
    /// chain-shape debt is cleared by a BUILD, never by a call.
    bool rebuildVct();
    /// Builds the ParallaxCorrectedCubemapAuto probe grid over `region` — the
    /// scene's own fitted box — and binds it with distance-blended VCT specular
    /// (PccVctMinDistance). Every candidate probe photographs its surroundings
    /// during the placement; the ones that saw nothing within twice their own
    /// region are DROPPED, and a grid left with none is not built at all (the
    /// sky stays the reflection source). See the long note on the definition.
    void buildPcc(const Ogre::Aabb &region);

    // ---- THE ARM, IN STAGES (SPECS/OPEN_COVER_SPEC.md §2 A) ---------------
    //
    // WHY THE ARM IS SPLIT AT ALL, and where the split had to go. A world's
    // FIRST arm build is the single longest thing this engine does on the UI
    // thread, and until this lane it happened inside ONE frame — measured on
    // Grand Showroom 2 (quiet box, warm shader cache): 1,025 ms, of which the
    // cascade chain is 57 ms and the PROBE PLACEMENT is 673 (scout 69, the
    // placement fit 316, the re-create 19, the closing capture 269). A scene
    // that keeps NO probes still pays 261 ms of it, every boot and every
    // create, because the candidates are photographed before they are judged.
    // So the unit of the split is a STAGE OF THE ARM, and the probe grid is
    // three of the five; splitting the cascades instead would have moved 57 ms
    // of a second.
    //
    // A `FramePace::Streaming` frame spends exactly one stage and then draws,
    // so the world appears and its lighting arrives over the next few frames.
    // A `Complete` frame runs all of them back to back, in this order, which
    // is the order the un-staged function always ran them in — that is what
    // keeps every suite, thumbnail, capture and selftest byte-identical.
    enum class GiBuildStage : unsigned char {
        Idle,          ///< no staged build in flight
        ProbeScout,    ///< one photograph of the space, to place the grid in
        ProbeFit,      ///< upstream's placement over every candidate, then keep/drop
        ProbeFinish,   ///< clamp, re-create at the real resolution, the closing capture
        Field          ///< the irradiance field and the volume bookkeeping
    };
    /// Where a staged build has got to. `Idle` outside one.
    GiBuildStage mGiBuildStage = GiBuildStage::Idle;
    /// True for the duration of ONE `rebuildVct` call that is allowed to park
    /// after the cascade arm instead of running the probe stages inline.
    bool mGiStageBuild = false;
    /// True while a stage is running, so the invalidation funnel does not
    /// abandon the build the stage is making.
    bool mInGiStage = false;
    /// The host says a world of this scene is arriving and none of it is on
    /// screen yet (Scene::setLoading). No FIRST-TIME arm build while it holds.
    bool mSceneLoading = false;
    /// The volume the staged probe stages are relative to — `rebuildVct`'s
    /// `aabb`, kept because the stages run in later frames.
    Ogre::Aabb mGiStagedVolume;
    /// The pace of the frame being rendered, pushed by the engine before the
    /// per-scene work. `Complete` outside a streaming open.
    FramePace mFramePace = FramePace::Complete;

    /// Spends ONE stage of a staged arm build. Returns true when the build is
    /// finished (the stage machine is back to Idle).
    bool stepStagedGiBuild();
    /// The probe grid's three stages. Each is `buildPcc`'s corresponding block,
    /// verbatim; `buildPcc` itself is the three of them in a row.
    void buildPccScout(const Ogre::Aabb &volume);
    void buildPccFit();
    void buildPccFinish();
    /// Milliseconds since the last phase split; restarts the clock.
    double pccPhaseSplit();
    /// What the three probe stages share, because they run in three frames when
    /// the build is staged. They were locals of the one function.
    bool                 mPccStageOk = false;     ///< the scout found a buildable grid
    Ogre::uint32         mPccNumProbes[3] = { 1u, 1u, 1u };
    Ogre::uint32         mPccProbeRes = 0u;
    Ogre::PixelFormatGpu mPccProbeFormat = Ogre::PFG_RGBA8_UNORM_SRGB;
    std::string          mPccWorkspaceName;
    /// scout, placement fit, keep/drop, re-create, closing capture.
    double               mPccPhaseMs[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
    std::chrono::steady_clock::time_point mPccPhaseClock;
    /// Rewinds a staged build to its first stage because the world moved under
    /// it. Idempotent; see the definition for why it rewinds and does not stop.
    void restartStagedProbeBuild();
    /// Unbinds (if this scene owns the binding) and deletes the probe grid.
    void destroyProbeGrid();


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
    void oweCascade0FieldFollow(GiStaleReason reason);
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
    /// Can a transform write on this node change what any of the epoch's scans
    /// reads? (OgreScene.cpp, beside the bit scheme it asks about.) No for
    /// editor furniture — the gizmo, the bone overlay, wires, the grid — which
    /// is most of what goes through setNodeTransform at all.
    bool writeIsSceneMovement(const Node &n) const;
    /// A hash of the GI gather's boxes — "is this the same content the current
    /// automatic volume was fitted to?" (OgreGi.cpp, lane ENGINE-7 item 2).
    static unsigned long long giContentSignature(const std::vector<Ogre::Aabb> &boxes);
    /// P7, materials: a visible material changed — stale the probes and, when
    /// the change reaches the voxelizer's conversion, bump the material
    /// generation (see mGiMaterialGeneration).
    void noteMaterialChanged(MaterialId id, bool voxelInputsChanged);
    /// The rule noteMaterialChanged applies, as a QUESTION — so the batched
    /// settle in settleTextureResidency can ask it per material and act ONCE
    /// (F5: one probe stale and one material-generation bump per frame,
    /// however many textures arrived together).
    bool giMaterialChangeEffect(MaterialId id, bool voxelInputsChanged, bool &bumpVoxels) const;
    /// P6/P7: the reuse arm's variant for a MATERIAL change — a fresh voxelizer
    /// and lighting (VctMaterial's by-pointer cache must go) under the SAME
    /// probe grid, whose shapes a material edit cannot move. Returns false
    /// when it could not build (the caller falls back to rebuildVct).
    bool freshVoxelArm(const Ogre::Aabb &aabb);
    /// The voxelizer + lighting half of rebuildVct, shared with freshVoxelArm:
    /// builds mVctVoxelizer/mVctLighting over `aabb` from the live GI items.
    /// Returns the item count (0 = nothing built, both left null).
    size_t buildVoxelArm(const Ogre::Aabb &aabb);

    // ---- PHOTON: the camera-centred cascade scheduler (PHOTON_SPEC P0) ------
    //
    // N camera-centred rasterising VctVoxelizer + VctLighting pairs, chained
    // through VctLighting::addCascade on the innermost one, scheduled BY US.
    // Upstream's VctCascadedVoxelizer is not used and cannot be: it hard-wires
    // VctImageVoxelizer (measured in spikes/photon-s1 to reproduce none of the
    // rasteriser's bounce), it rebuilds every dirty cascade in one frame with
    // no budget, and its buildRelative has no guard against a camera jump
    // longer than a cascade (three GPU losses in that spike).
    //
    // Cascade 0 IS mVctVoxelizer/mVctLighting — the head of the chain, the
    // object bound to HlmsPbs — so every existing binding, teardown, status
    // and irradiance-field rule keeps working unchanged; mVctCascades[0]
    // mirrors those two pointers and 1..N-1 are owned here.
    struct VctCascade {
        Ogre::VctVoxelizer *voxelizer = nullptr;   ///< owned, except [0]
        Ogre::VctLighting  *lighting  = nullptr;   ///< owned, except [0]
        float        halfSize   = 0.0f;
        Ogre::uint32 resolution = 64u;
        float        stepCells  = 4.0f;
        /// The lattice cell this cascade is centred on (integer coordinates in
        /// units of `step`), and the world centre that resolves to.
        long long    latticeX = 0, latticeY = 0, latticeZ = 0;
        Ogre::Vector3 centre = Ogre::Vector3::ZERO;
        bool         built   = false;
        /// Whether this cascade's voxeliser currently holds the GI items. A
        /// cascade standing in empty space holds none (setCascadeItems).
        bool         itemsAttached = false;
        /// The SET it holds is out of date — an object entered or left the GI
        /// geometry channel (audit D2). Re-derived at this cascade's next
        /// rebuild, never immediately: nothing about the picture is wrong until
        /// the cascade re-voxelises anyway.
        bool         itemsStale = false;
        /// THIS CASCADE'S VOXELISER ITSELF IS UNSAFE OR STALE (G1). `VctMaterial`
        /// converts each datablock once and caches the result by RAW POINTER for
        /// the voxeliser's whole life, so a material parameter that changed — or
        /// a datablock that died and whose address may be recycled — can only be
        /// answered by a NEW voxeliser. Serviced one cascade per frame by
        /// `rebuildCascade`, which swaps the replacement into the EXISTING
        /// lighting (`VctLighting::setVoxelizer`, ogre-patch 0037) so the
        /// chain's raw `mExtraCascades` pointers never dangle.
        bool         freshVoxels = false;
        /// How many GI items THIS cascade's last rebuild voxelised — inside its
        /// box and big enough to fill half a voxel of it, re-counted on every
        /// rebuild (the attach set is bigger and deliberately so: rule 1).
        unsigned     items = 0;
        /// This cascade is BEHIND the camera and owes a rebuild — a flag, not a
        /// queue: a rebuild always happens at the CURRENT camera, so owing two
        /// of them is the same as owing one.
        int          pending = 0;
        /// WHY it is pending — the reason the monitor row and the event carry.
        /// `Camera` for an ordinary scroll (E0's B12/D5), the edit's own reason
        /// when the dirty path marked it (G1), so a capture can separate the
        /// cost of walking around a scene from the cost of changing it.
        GiStaleReason pendingReason = GiStaleReason::Camera;
        /// WHAT THE VOXELISER HOLDS, in attach order (PHOTON_SPEC E2 (1)).
        /// Empty without an instance budget — the set is then the whole
        /// size-filtered scene and cannot change without an edge, so there is
        /// nothing to remember. Under a budget it is the pick, and comparing it
        /// against the next pick is what keeps a scroll that changed nobody's
        /// rank from re-uploading every mesh buffer.
        std::vector<Ogre::Item *> attachedItems;
        /// THE LEVELS THE ATTACH SET WAS VOXELISED AT (ATOM stage 1), as a
        /// histogram: `lodLevels[L]` items at level L. Filled by
        /// setCascadeItems at the one site that hands geometry to the
        /// voxeliser, so it describes what the voxeliser HOLDS and not what a
        /// walk would decide now. `{N}` for a scene with no baked LOD chains.
        std::vector<int> lodLevels;
        /// THE LEVELS THE VOXELISER ACTUALLY SPENT (ATOM P4 / AT-A10) - a READING,
        /// taken off the voxeliser after every `build()` through
        /// `VctVoxelizer::getLevelHistogram()`. The pair of `lodLevels` above,
        /// which is the request; they agree now that the level is per instance,
        /// and a disagreement from here on means the voxeliser clamped.
        std::vector<int> voxelLevels;
        /// THE TRIANGLES THE VOXELISER ACTUALLY HOLDS — a READING, taken off the
        /// voxeliser after every `build()` through ogre-patch 0089's
        /// `getQueuedIndexCount()` (the sum of `QueuedInstance::numIndices`, which
        /// is what sizes each raster dispatch). It was a CPU prediction until
        /// ATOM-BAKE-1 (inventory row AT-A12): a walk of each mesh's VAOs at the
        /// level just requested, re-applying patch 0064's clamp in a second copy
        /// of it and counting items the region had declined. 0 between an attach
        /// and the build that follows it, which is honest — nothing is bound yet.
        long long lodTriangles = 0;
        /// This cascade's queued rebuild came from the JUMP guard, not from an
        /// ordinary scroll — i.e. nothing of its old volume was reusable.
        /// Cleared when the rebuild is serviced, and counted there, so the
        /// counter is one per cascade per teleport rather than one per frame
        /// the camera spends far away.
        bool         jumped  = false;
        /// Consecutive failed rebuilds. One retry on the next frame, then the
        /// cascade stands down and waits for the camera to move again (round-2
        /// review F3) — a cascade that cannot build must not spend the frame's
        /// whole GI budget for ever and starve the ones that still can.
        unsigned     failures = 0;
        unsigned long long rebuilds = 0;
        /// THIS CASCADE HAS BEEN INJECTED SINCE THE LAST IN-MOTION LIGHT TICK
        /// (DRAG-1, REFLECT F2). `rebuildCascade` ends with a full-count
        /// `VctLighting::update`, so a cascade the scheduler rebuilt this frame
        /// already holds the answer the tick would compute — re-computing it is
        /// duplicate work, and computing a DIFFERENT one (which is what the
        /// zero-bounce moving tick did) is the pulse. Set by every rebuild,
        /// cleared by the tick that skipped on it.
        bool         injectedSinceTick = false;
        /// ...AND THE LIGHT STATE IT WAS INJECTED AT (DRAG-1 round 2, F5). The
        /// latch above says "a rebuild already computed what the tick would
        /// compute", and that is only true while nothing the INJECTION reads has
        /// changed since. A lamp that moved or was re-parameterised between the
        /// rebuild and the tick makes the rebuild's answer stale, and skipping
        /// on the latch alone would leave that cascade holding the old light
        /// until its next rebuild — which, for an outer cascade deferred behind
        /// cascade 0, is the whole drag. The tick skips only when this still
        /// equals the scene's current light-write serial.
        unsigned long long injectedAtLightSerial = 0;
        float        lastCpuMs = -1.0f;
        /// The camera position this cascade was last BUILT for. The scroll test
        /// quantises BOTH on an absolute world lattice of `step * (1 -
        /// kStepHysteresis)` metres and requires the camera to be the band past
        /// the plane it left (OgreGi.cpp) — the pin's `consistentCascadeSteps`
        /// reading with the hysteresis added, which is what keeps two cascades
        /// from stepping on different frames for the same metre AND what stops a
        /// head swaying on a plane from thrashing the chain. Because the lattice
        /// is absolute, `step()` is the SUPREMUM of the travel between rebuilds,
        /// not the distance between them.
        Ogre::Vector3 builtCam = Ogre::Vector3::ZERO;
        float step() const { return stepCells * (halfSize * 2.0f / float(resolution)); }
        float cell() const { return halfSize * 2.0f / float(resolution); }
    };
    /// Builds the whole chain around `camPos` from scratch. Returns the item
    /// count (0 = nothing built). The cascade arm's counterpart of
    /// buildVoxelArm, and it calls into it for cascade 0.
    size_t buildCascadeArm(const Ogre::Vector3 &camPos);
    /// The resolved cascade table (GiParams::cascadeSet, else the tier table).
    std::vector<GiParams::GiCascadeDesc> resolveCascadeTable() const;
    /// One cascade's whole re-voxelisation at its current centre: region ->
    /// build -> ambient -> light. `reason` only labels the monitor row.
    /// FALSE when the build threw: the caller must then put the placement back,
    /// because the voxeliser's region (read live by the shader) has already
    /// moved and the voxels have not (audit B4).
    /// `placementCommitted` (out, optional) is set when the rebuild FAILED but
    /// the cascade's new placement must NOT be put back: the replacement
    /// voxeliser's `build()` had already succeeded and the lighting already
    /// reads it, so the voxels on the GPU describe the NEW region and reverting
    /// the region would point the shader at the new voxels through the old box
    /// (audit B4's defect, in reverse). See the failure paths inside.
    bool rebuildCascade(size_t idx, GiStaleReason reason,
                        bool *placementCommitted = nullptr);
    /// THE PER-CASCADE DIRTY PATH (PHOTON_SPEC G1). An EDIT under the cascade
    /// arm, answered without tearing the chain down: the cascades the edit can
    /// be seen from are marked `pending` and spent by `updateCascades` ONE PER
    /// FRAME, with every voxeliser, texture and mesh-buffer upload KEPT. False
    /// when the chain cannot answer at all (no chain, or nothing built yet), in
    /// which case the caller takes the from-scratch `rebuildVct`.
    bool refreshCascadesFast();
    /// ONE DEFINITION OF AN AT-REST CASCADE INJECTION, shared by the light tick
    /// and the incremental settle (LAMPREST-3 fix round).
    void injectCascade(size_t i, bool coarse);
    /// The field re-integrates once, after the LAST injection of a tick or of a
    /// settle — never per injection.
    void reintegrateFieldAfterInjection();
    /// One injection of an owed settle, out of the scheduler's frame slot.
    void payChainSettleStep();
    /// Remember / compare the lights and ambient an owed settle's steps must all
    /// see: a light that moves mid-settle restarts it (a settle that mixed two
    /// lamp poses is the fixed point of neither).
    void noteSettleInputs();
    bool settleInputsUnchanged() const;
    /// Record the chain as settled for the inputs it was injected with.
    void noteChainSettled();

    /// Records WHERE the scene changed, for the dirty path above. `box` is the
    /// region the edit touched (a mover's old box united with its new one, or a
    /// vanishing item's last box); nullptr means "somewhere" and marks the whole
    /// chain. A no-op unless a cascade chain is live, so every call site can
    /// call it unconditionally. NOT called at all where nothing a voxel holds
    /// moved — a light leaving the scene — which is what makes that a
    /// re-injection and nothing else.
    void noteGiCascadeDirty(const Ogre::Aabb *box);
    /// A DATABLOCK OR TEXTURE THE VOXELISERS' MATERIAL CACHE HOLDS IS DYING.
    /// Under cascades, marks every cascade for a voxeliser REPLACEMENT, spent
    /// one per frame — see VctCascade::freshVoxels. A strictly narrower thing
    /// than `invalidateGiCaches`: a dead Item or Mesh is answered by re-selecting
    /// a voxeliser's item set (`removeAllItems` drops every raw `Item*` and every
    /// cached mesh in one call), while only a dead DATABLOCK or TEXTURE can
    /// outlive that, because `VctMaterial` keys its conversion cache on the
    /// datablock POINTER and a recycled address would alias.
    /// THE BY-POINTER ALIAS GUARD (MATERIAL-SWAP-GI-1, patch 0081): VctMaterial
    /// caches conversions by raw datablock pointer across builds, so a dying
    /// datablock is EVICTED from every live voxeliser's cache — no volume is
    /// re-voxelised for a death. With a null pointer (a caller that cannot name
    /// the datablock) it falls back to marking every cascade's voxels fresh.
    void noteGiDatablockDied(Ogre::HlmsDatablock *dying = nullptr);
    /// Marks every cascade whose box intersects the recorded dirty region (or
    /// all of them when the region is unknown) `pending`, and clears the region.
    /// `why` is the reason the monitor row will carry. Returns how many cascades
    /// it marked.
    size_t markDirtyCascadesPending(GiStaleReason why);
    /// The end of a drag gesture (MOVER-1): every promoted mover goes back to
    /// being still world, owing one cascade box and one probe stale between
    /// them. Called once a frame from updateGiTracking; a no-op with no movers.
    void   endDragGestureIfStill();
    /// THE SCHEDULER, called once a frame from updateGiTracking with the
    /// authoritative camera. Re-quantises every cascade, queues the ones that
    /// moved, and spends AT MOST ONE rebuild this frame, innermost first.
    void updateCascades(const Ogre::Vector3 &camPos);
    /// Destroys cascades 1..N-1 (cascade 0 is teardownVct's own business).
    void teardownExtraCascades();
    /// How many GI items this cascade would voxelise reach into its box — the
    /// per-rebuild count `GiStatus::cascades[].items` reports, and (as
    /// `count > 0`) the answer to "may this cascade be built with items
    /// attached at all", which Ogre cannot be asked.
    unsigned cascadeGeometryCount(const VctCascade &c) const;
    /// How many items this cascade's voxeliser HOLDS with no instance budget
    /// (GiStatus::cascades[].attached's other half).
    unsigned cascadeAttachCount(const VctCascade &c) const;
    /// The same walk, and (with `keep`) the set to ATTACH — the instance
    /// budget's selection when `GiParams::cascadeInstanceCap` is set.
    unsigned selectCascadeItems(const VctCascade &c, std::vector<Ogre::Item *> *keep) const;
    /// WHICH mesh LOD a cascade voxelises an item at (ATOM-1). 0 until the
    /// levels exist; the rule and what it is waiting for are at the definition.
    unsigned cascadeVoxelLod(const VctCascade &c, const Ogre::Item *item) const;
    /// Attaches or detaches the SIZE-FILTERED GI item set on one cascade's
    /// voxeliser (whole, never box-filtered: Ogre culls it to the region per
    /// build). A cascade with nothing in its box builds an EMPTY volume instead
    /// of throwing (the pin's zero-thread-group refusal).
    void setCascadeItems(VctCascade &c, bool attach);
    /// Destroys a chain that never finished building (nothing is bound yet, so
    /// cascade 0 belongs to it too). Returns 0 — it is a JAH_CATCH value.
    size_t abandonCascadeChain();
    /// Re-centres one cascade's region on ITS cell lattice around `camPos` and
    /// records the camera it was placed for. Does not voxelise.
    void recentreCascade(VctCascade &c, const Ogre::Vector3 &camPos);
    /// The ambient pair into ONE cascade's lighting (applyVctAmbient, aimed).
    void applyCascadeAmbient(Ogre::VctLighting *lighting);
    /// Extra bounce passes for cascade `idx` — the DOCUMENT's own count, on
    /// every cascade alike (PHOTON-M1 retired the pin's "a coarser cell gets
    /// more bounces" stabilisation: a bounce adds energy, it does not recover
    /// occlusion). 0 when the document asks for a single indirect bounce,
    /// which is the default.
    Ogre::uint32 cascadeBounces(size_t idx) const;
    /// HOW MANY INJECTION PASSES AN AT-REST TICK SPENDS over a cascade chain
    /// (LAMPREST-2). A re-injection is one Jacobi iteration of the chain's
    /// coupled radiance, so the tick has to iterate until the answer stops
    /// depending on the state it started from: measured, two passes leave
    /// 4/255 of that history in the sealed room scripting.e2e.movable_lamp_rest
    /// uses and three leave none, with three, four and six passes producing the
    /// same picture. The moving tick stays at one pass. `JAHSHAKA_GI_SWEEPS`
    /// overrides it for the suite that pins the measurement.
    static constexpr int kAtRestSweeps = 3;
    /// What the last light tick actually spent (GiStatus::chainSweeps).
    int mGiChainSweeps = 0;
    /// A CASCADE REBUILD LEFT THE CHAIN OFF ITS FIXED POINT (LAMPREST-3). A
    /// rebuild injects ONE cascade once, over the radiance it held somewhere
    /// else, and the mirror's cadence never ticks for a camera walk — so the
    /// chain stayed one Jacobi pass from where it belonged until something
    /// unrelated happened to re-inject it (measured: 60,852 of cascade 0's
    /// 82,176 lit light-voxel bytes wrong by up to 55/255, permanently, after
    /// a walk that returned to its own starting pose).
    ///
    /// THE DEBT IS A COUNT OF INJECTIONS, NOT A FLAG, and it is paid ONE PER
    /// FRAME out of the scheduler's own one-slot budget: the at-rest tick is
    /// kAtRestSweeps passes over every cascade (twelve sequential injections at
    /// Medium, 12.0-12.9 ms in Debug on the rig) and spending that in ONE frame
    /// is a whole frame at 90 Hz — a hitch, for a picture that is only owed
    /// because the camera moved. Spread over frames, in the SAME order the tick
    /// uses (sweeps outer, cascades outermost-first), it leaves the same bytes
    /// (verified by sha256 of the light voxels) for ~1 ms a frame, the rebuild
    /// queue keeps priority, and a walk that never ends never starves: it keeps
    /// paying one cheap injection per idle slot.
    int    mGiSettleStepsOwed = 0;
    /// The cascade count the debt was raised against — a chain that changed
    /// shape under an unfinished settle abandons it rather than injecting a
    /// cascade the sequence no longer describes.
    size_t mGiSettleCascades = 0;
    /// The light-write serial and ambient the running settle was raised with.
    unsigned long long mGiSettleSerial = 0;
    Colour             mGiSettleAmbient[2];
    /// How many of those settles this scene has paid (GiStatus::chainSettles) —
    /// cumulative, so a suite can assert "one per gesture, not one per frame"
    /// from outside.
    long long mGiChainSettles = 0;
    /// WHOEVER PAYS THE INJECTION PAYS THE DEBT (fix round item 3): a full
    /// at-rest chain tick IS the settle the scheduler owes, so it clears an
    /// unfinished one rather than letting it run on top of the answer the tick
    /// just computed. (The other half of that item — letting the mirror's
    /// stability tick SKIP a chain that is already settled — was built, measured
    /// and NOT shipped: the engine's light-write serial does not see a movable
    /// lamp's pose change the way an injection does, and the skip cost
    /// scripting.e2e.movable_lamp_rest 3/255 in 10 runs of 10. The saving
    /// belongs in the mirror, which owns the signature.)
    /// The irradiance field's pass-buffer block (JahIfd_piece_ps.any).
    void          pushIfdState(const Ogre::uint32 numProbes[3]);
    /// THE FIELD FOLLOWS CASCADE 0 (PHOTON_SPEC E1 item 1). Called by the
    /// cascade scheduler whenever cascade 0 has been re-placed or re-voxelised:
    /// moves the field's volume onto cascade 0's voxel box (ogre-patch 0044's
    /// `setFieldVolume`), re-binds it if that cascade's lighting re-created its
    /// light voxel textures, and re-integrates — whole when the volume MOVED
    /// (the atlases describe another place and there is no per-probe validity),
    /// progressively over the converged atlas when only the voxels changed.
    /// A no-op in the single-volume arm, with no field, or with the toggle off.
    void          followCascade0Field(GiStaleReason reason);
    /// The movement quantum for one item's world AABB (a 64th of its own
    /// largest extent), and "did this AABB move by at least that much?". Shared
    /// by giGeometrySignature and walkItems so the mirror's debounce and
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
    /// This scene's record for `id`, or null — the one-lookup form of
    /// `node(id)` + `mNodes.find(id)`, for the callers that need both the Ogre
    /// node and the record (round-2 review F9).
    Node *record(NodeId id) {
        auto it = mNodes.find(id);
        return it == mNodes.end() ? nullptr : &it->second;
    }
    /// The EFFECTIVE visibility `sn`'s children inherit from above it: the
    /// `shown` of the nearest registered ancestor, true when there is none.
    bool inheritedShown(const Ogre::Node *sn);
    /// Applies EFFECTIVE visibility to `sn` and to the children THIS ENGINE
    /// owns — an engine-created registered node (createNode) and the two
    /// unregistered helper children it makes itself, a light's -Y adapter and
    /// a decal's projector box. It does NOT descend into the adopted document
    /// subtree: the document's host pushes every node's effective visibility
    /// parent-first, and walking it here made N pushes cost Sum(subtree sizes)
    /// — 6.3 visits per node and +21.6 ms of first sync at 10k (L12). Sets
    /// `giChanged` when any Item's kGiGeometryBit moved, so the caller can
    /// invalidate GI ONCE for the whole subtree.
    /// (RENDER_PIPELINE_AUDIT 1.1/1.2)
    void applyShownSubtree(Ogre::SceneNode *sn, bool inherited, bool &giChanged);
    void applyShownSubtree(Ogre::SceneNode *sn, Node *rec, bool inherited, bool &giChanged);
    /// setNodeVisible / setNodeVisibleUnder, in one body: `parentShown` null
    /// means "derive it" (inheritedShown), non-null means the host walked its
    /// tree parent-first and already knows.
    void setNodeVisibleImpl(NodeId id, bool visible, const bool *parentShown);
    /// Voxel volume resolution per axis for the current quality.
    unsigned giVoxelResolution() const;
    /// The GI items' world AABBs after the exclude flag and the extent-outlier
    /// trimming: the one place that decides which objects define the lit world.
    std::vector<Ogre::Aabb> giItemBounds() const;
    /// The same list BEFORE the outlier trim — every GI item's world AABB as it
    /// is.
    std::vector<Ogre::Aabb> giItemBoundsRaw() const;
    /// Records (or clears) mGiAutoVolume after a rebuild. `fitted` is what the
    /// AUTO path resolved; a hand-typed bounds box clears the record instead.
    void noteGiAutoVolume(const Ogre::Aabb &fitted, bool automatic);
    /// True when the document typed a bounds box by hand (min != max).
    bool giBoundsExplicit() const;
    /// Pushes mAmbientRadiance into the VCT arm (no-op without one). Called on
    /// every ambient change and whenever the arm is (re)built.
    void applyVctAmbient();
    /// Unbinds from HlmsPbs (when this scene owns the binding) and deletes the
    /// PCC, VctLighting and VctVoxelizer, in that order. Safe to call twice;
    /// must run BEFORE the SceneManager dies.
    void teardownVct();
    /// Deletes every GI object. Safe to call twice; must run BEFORE the
    /// SceneManager dies (probe workspaces, GI camera).
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
    /// THE HOST'S CONTROLLER PROXY NODES, left then right (Scene::setVrProxyNodes,
    /// VR_SPEC §5 phase 4). The scene owns nothing about them but these two ids:
    /// a running VrSession reads them once a frame, right after it has located
    /// the wearer's hands, and writes this frame's pose into them.
    NodeId              mVrProxyNode[2] = { 0, 0 };
    /// THE HOST'S RAY NODES, line then marker (Scene::setVrRayNodes,
    /// VR_INPUT_SPEC §3): the same arrangement as the proxies above, placed by
    /// the running session from `Engine::setVrRay`'s state.
    NodeId              mVrRayNode[2] = { 0, 0 };
    /// THE HOST'S HAND-BONE NODES, per hand, in `kVrHandBones` order
    /// (Scene::setVrHandBoneNodes, VR_INPUT_SPEC §7): the same arrangement as
    /// the two above — ids and nothing else; the segments, their one shared
    /// mesh and their material are the mirror's. `mVrHandBones[h]` is how many
    /// of the row are registered (0 = this hand draws no skeleton).
    NodeId              mVrHandBoneNode[2][kVrHandBoneCount] = {};
    unsigned            mVrHandBones[2] = { 0u, 0u };
    std::map<MeshId, MeshRec> mMeshes;
    /// ATOM stage 1: THE MESH RECORD BY OGRE MESH — the one lookup that takes an
    /// `Ogre::Item *` (all a voxeliser or a proxy consumer has) to the `MeshRec`
    /// `createMesh` filled, and through it to the baked per-level bounds. Only
    /// meshes that HAVE a chain are in it, which is a small minority, so a miss
    /// is the common case and means "no chain, level 0". Maintained beside
    /// `mMeshes` (createMesh inserts, destroyMesh and destroy() erase) rather
    /// than walked, because the cascade attach walks every item of the scene.
    ///
    /// AN INDEX AND NOT A COPY (AT-DUP): it used to hold the error vector itself,
    /// so every chained mesh carried its levels twice and the two could drift.
    std::unordered_map<const Ogre::Mesh *, MeshId> mMeshIdByOgreMesh;
    /// SURFACE-CACHE phase 1 -> 2: THE CARDS BY OGRE MESH, the same index and
    /// for the same reason — a cache holds an `Ogre::Item *` and needs the card
    /// list the bake authored for the mesh behind it. Only meshes that HAVE
    /// cards are in it.
    std::unordered_map<const Ogre::Mesh *, std::vector<MeshCardDesc>> mCardsByMesh;
    /// THE FAR-FIELD PROXY, AS APPLIED (ATOM stage 1): the scene's
    /// `GiParams::cascadeVoxelLod` met with the run-wide diagnostic latch,
    /// resolved once per `setGlobalIllumination` and reported as
    /// `GiStatus::cascadeVoxelLod`. Kept apart from `mGi` on purpose — `mGi`
    /// must keep comparing equal to what the document pushes.
    bool mCascadeVoxelLod = true;
    /// The latch itself, the shape `JAHSHAKA_NO_RAY_QUERY` uses
    /// (OgreEngine.cpp:55): a measurement needs to move ONE term with one
    /// binary and one scene, and a run-wide switch is how it does that. Read
    /// ONCE, at construction, and never again.
    const bool mCascadeLodAllowed = std::getenv("JAHSHAKA_NO_CASCADE_LOD") == nullptr;
    /// ATOM stage 1: the scene-wide LOD dial. 1 = the reference budget of one
    /// pixel of geometric error; 0 pins every object at level 0.
    float mLodBias = 1.0f;
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
    /// The sun disc's quad and its cloned material (one per scene, like Ogre's
    /// own sky material clone: the parameters are per-scene).
    Ogre::Rectangle2D *mSunDisc = nullptr;
    Ogre::MaterialPtr  mSunDiscMaterial;
    /// The grid's quad, its per-scene material clone and what was pushed last
    /// (the idempotency guard, like mSkyDesc for the sky).
    Ogre::Rectangle2D *mGrid = nullptr;
    Ogre::MaterialPtr  mGridMaterial;
    GridDesc           mGridDesc;
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
    // VCT arm (null unless a VCT mode is live). Teardown order within the arm:
    // unbind HlmsPbs -> PCC -> VctLighting -> VctVoxelizer, all before the
    // SceneManager (probe workspaces and the GI camera live in it).
    Ogre::VctVoxelizer               *mVctVoxelizer = nullptr;
    Ogre::VctLighting                *mVctLighting  = nullptr;
    /// THE PHOTON CASCADE CHAIN, innermost first. Empty in the single-volume
    /// arm. [0] mirrors mVctVoxelizer/mVctLighting (NOT owned through here).
    std::vector<VctCascade> mVctCascades;
    /// The scheduler's counters, reported through GiStatus. CUMULATIVE over the
    /// scene's life, which is what Types.h has always said they are: a rebuild
    /// of the arm (a settle, an edit, an atlas change) must not reset them, or
    /// any reading that spans one is a reading of nothing (audit B9). Only
    /// teardownGi — GI switched off, or the scene dying — clears them.
    unsigned long long mCascadeFullRebuilds = 0;
    unsigned long long mCascadeDeferrals = 0;
    unsigned long long mCascadeDirtyMajority = 0;
    /// One log line per scene for a cascade rebuild that threw (B4).
    bool mCascadeFailureLogged = false;
    /// WHERE THE SCENE CHANGED SINCE THE CHAIN LAST ANSWERED FOR IT (G1) — the
    /// union of each mover's old and new world AABB (the movement scan's own
    /// boxes) plus the box of every edit that carried one. A cascade is marked
    /// `pending` only when its own box intersects one of these, which is what
    /// makes dragging a chair in one corner cost the cascades that can see the
    /// chair and nothing else.
    ///
    /// BOUNDED BY MERGING, NOT BY GIVING UP (PHOTON audit F14, 2026-09-18). Past
    /// `kGiCascadeDirtyBoxCap` the list used to collapse into
    /// `mGiCascadeDirtyAll` — "the scene changed everywhere" — on the reasoning
    /// that a scene changing in sixteen places is one the whole chain has to
    /// answer for anyway. That reasoning is wrong for the commonest case there
    /// is: SEVENTEEN OBJECTS SETTLING IN ONE CORNER (a physics pile, an animated
    /// set, a crowd) produce seventeen SMALL boxes a metre apart, and answering
    /// "everywhere" marks every cascade out to the horizon, so the chain
    /// rebuilds one cascade per frame for as long as they keep moving — the
    /// exact cost the per-cascade path exists to avoid, reached by having TOO
    /// MUCH information rather than too little.
    ///
    /// At the cap a new box is therefore MERGED into the existing entry whose
    /// MARGIN (the sum of its extents) grows least — the R*-tree's metric, and
    /// deliberately not the volume: this engine's boxes go FLAT, a Plane's world
    /// AABB has zero height, and under a volume metric every coplanar box merges
    /// at zero growth however far apart it is (a floor of moving planes
    /// coalesces into one scene-spanning slab — conservative, never wrong, and
    /// the exact opposite of the point). See noteGiCascadeDirty. The list stays
    /// bounded, the description stays conservative in the only direction that is
    /// safe — a merged box covers everything both boxes did — and seventeen
    /// crates in a corner stay a corner. `mGiCascadeDirtyAll` survives for the
    /// one statement that really is scene-wide: a null box, i.e. "somewhere, I
    /// cannot say where".
    std::vector<Ogre::Aabb> mGiCascadeDirtyBoxes;
    bool mGiCascadeDirtyAll = false;
    static const size_t kGiCascadeDirtyBoxCap = 16;
    /// The authoritative camera position the GI tracker last saw, and whether
    /// any view has ever tracked one. A cascade arm is built AROUND it, so a
    /// rebuild that arrives before the first tracked frame (every scene open:
    /// the document pushes GI before a frame renders) builds NOTHING and sets
    /// the flag below instead — building at the origin and re-centring on the
    /// first tracked frame cost a second whole-chain voxelisation per open
    /// (audit B3).
    Ogre::Vector3 mGiCamPos = Ogre::Vector3::ZERO;
    bool mGiCamPosKnown = false;
    /// A cascade arm was asked for while no camera was known. The next tracking
    /// update flags the caches dirty, and that frame's own applyPendingGi
    /// builds the chain where the camera actually is.
    bool mGiCascadeAwaitingCamera = false;
    Ogre::ParallaxCorrectedCubemapAuto *mPcc        = nullptr;
    Ogre::Camera                     *mGiCamera     = nullptr;   // PCC build + tracking
    /// The DDGI field, owned, null unless GiParams::ddgi resolved on over a
    /// live VCT arm. Dies BEFORE mVctLighting (it holds that pointer). The
    /// source switch, the placement and the accessors it needs are all public
    /// API since ogre-patches 0044 and 0050, so this is the engine's own type —
    /// which also retires the slicing hazard of holding a derived type through
    /// a non-virtual ~IrradianceField.
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
    /// THE VOLUME THE FIELD IS PLACED OVER, as asked for (the field enlarges it
    /// by one probe block per side for itself). Recorded so the scheduler can
    /// tell a cascade-0 re-placement from a plain re-voxelisation at the same
    /// place — the first invalidates every probe in the atlas, the second does
    /// not — and so giStatus can report where the field actually is.
    Ogre::Vector3                     mIfdVolumeOrigin = Ogre::Vector3::ZERO;
    Ogre::Vector3                     mIfdVolumeSize   = Ogre::Vector3::ZERO;
    /// The field's probe counts, kept because the volume can move without a
    /// rebuild (E1).
    Ogre::uint32                      mIfdProbeCounts[3] = { 0u, 0u, 0u };
    /// How many times the field has been re-placed onto cascade 0 since the
    /// last build (GiStatus::ifdFollows) — the counter the follow suite reads,
    /// and the honest answer to "is the field tracking the chain at all".
    unsigned long long                mIfdFollows = 0;
    /// A FIELD FOLLOW OWED TO THE NEXT FRAME, AND WHY IT IS NOT PAID ON THE
    /// FRAME THAT MOVED THE CASCADE (lane V1-RIG item 2, LATER_OPTIMISATIONS
    /// L11, measured).
    ///
    /// Cascade 0's rebuild and the field's WHOLE re-integration used to land on
    /// one frame, and at a headset's pixel count that frame is over the 90 Hz
    /// bar: measured in a Monado session with the eye render forced to Quest
    /// Pro size (2160x2376 per eye, 10.26 Mpx of stereo target), a Medium walk's
    /// quiet frame is 5.84 ms of GPU and its step frame 11.72 ms mean / 12.45
    /// max, twelve of a hundred and sixty frames over 11.1 — one dropped frame
    /// every five metres of travel (spikes/v1-rig/COST.txt). The two halves are
    /// 2.43 ms (the cascade) and 3.40 ms (the field) and NEITHER alone crosses
    /// the bar: split across two frames the same walk peaks at 8.3 and 9.2 ms.
    ///
    /// WHAT IT COSTS IN CORRECTNESS: for exactly one frame the field describes
    /// the place cascade 0 has just left — one step of staleness, 11 ms of it,
    /// against L11's double-buffered atlas which accepts the same staleness for
    /// as many frames as a progressive re-integration takes. It is never a
    /// WRONG PLACE: the field's volume and its atlas move together, so the
    /// shader reads probes that were integrated where the field says it is.
    ///
    /// It also owns the frame's one GI slot, so the frame that pays it rebuilds
    /// no cascade — which is the whole point — and `updateIrradianceField`
    /// refuses to run a progressive batch while it is owed: cascade 0's rebuild
    /// may have re-created the light voxel textures the field's generation job
    /// binds, and the re-bind is the first thing `followCascade0Field` does.
    int          mIfdFollowOwed = 0;
    GiStaleReason mIfdFollowReason = GiStaleReason::Camera;
    bool mRefractionsActive = false;   // see setRefractionsActive
    /// Is the view that DRIVES GI a stereo (headset) one? It picks the tier
    /// table's VR column (GiViewProfile, V1-RIG item 4) and is written by the
    /// once-a-frame driver hook.
    bool mGiDriverStereo = false;
    /// The chain's SHAPE (its cascade count and steps) no longer matches the
    /// table it should be built from — the driver's profile changed. The dirty
    /// BOX path cannot express it, so the flush builds the chain again.
    bool mGiChainShapeDirty = false;
    /// Which column of the tier table the LIVE chain was built from, recorded at
    /// the build (`GiStatus::cascadeProfileVr`). Not the same reading as
    /// `mGiDriverStereo`, which says who is driving NOW: the two differ for the
    /// one frame a profile change is owed, and only this one is a fact about the
    /// chain the shader is sampling.
    bool mGiChainProfileVr = false;
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
    /// How many material pushes have CROSSED the reflection-probe gate
    /// (ogre-patch 0028's HlmsPbsDatablock::hasZeroSpecularResponse) on this
    /// scene. A crossing flushes every renderable wearing the datablock so the
    /// shader is rebuilt with or without the per-pixel probe loop; an ordinary
    /// edit that leaves the material reflective either way costs nothing. The
    /// counter exists so BOTH halves of that are gateable
    /// (GiStatus::probeGateCrossings).
    unsigned mProbeGateCrossings = 0;
    /// THE LAMP-MAP CACHE'S MEMORY (collectShadowCacheFrame). Per caster: its
    /// last seen world box, Item, pose epoch and render channels; per lamp: its
    /// last seen shadow key (parameters + pose). A change is "different from
    /// what was last seen", so a scene that is not drawn for a while is caught
    /// up in full on the first frame it is drawn again.
    std::unordered_map<NodeId, unsigned long long> mShadowLightKeys;
    /// THE ITEM INDEX: every Node that holds an Item (map nodes never move, so
    /// the pointers are stable until the node is erased — indexItemNode /
    /// unindexItemNode keep it exact at every Item create and destroy).
    std::vector<Node *> mItemNodes;
    void indexItemNode(Node &n);
    void unindexItemNode(Node &n);
    // --- the GPU scene's state (OgreGpuScene.cpp) --------------------------
    /// The tables. Mutable because `ensureGpuScene` is const: the readers that
    /// will ask for it (the GI signatures, when V2-1 makes that affordable) are
    /// const, and a facility that only a non-const path can refresh would put
    /// the const-cast at every call site instead of here.
    mutable detail::GpuScene mGpuScene;
    /// ATOM P3's CULL — its result buffers, sized to the table's capacity and
    /// grown with it (GpuCull.h). Owned per SCENE because that is what the
    /// tables it reads are owned by; a consumer that wants two culls of one
    /// scene in a frame is stage 3's problem and gets a second instance.
    detail::GpuCull mGpuCull;
    mutable bool mGpuSceneRefused = false;   ///< create() said no (headless); do not retry
    mutable std::vector<uint32_t> mGpuDirty;    ///< this update's slots (kept, not reallocated)
    mutable std::vector<uint32_t> mGpuForced;   ///< explicit marks since the last update
    mutable unsigned long long mGpuEpoch = 0ull;
    mutable bool mGpuEpochValid = false;
    mutable unsigned long long mGpuScans = 0ull;      ///< dirty scans run, ever
    mutable unsigned long long mGpuAabbReads = 0ull;  ///< world AABBs the scan asked for, ever
    mutable double mGpuScanMicros = 0.0;
    // --- THE RAY LEVEL (ATOM P3's AT-A8r; OgreGpuScene.cpp) ----------------
    /// The level each slot's bottom-level structure should be built from, and
    /// the DISTANCE that answer was computed at (-1 = never). The second array
    /// is the hysteresis: the rule is asked again only when the live distance
    /// leaves the 2x band around the recorded one. Both are indexed by item
    /// slot and both are cleared for a slot the index frees or renumbers, so a
    /// recycled slot never inherits the dead object's band.
    std::vector<uint32_t> mRayLevel;
    std::vector<float>    mRayEvalDistance;
    Ogre::Vector3 mRayEye = Ogre::Vector3::ZERO;
    bool mRayEyeValid = false;
    unsigned long long mRayLevelScanSeen = 0ull;   ///< the scan count the last walk saw
    unsigned long long mRayLevelEvals = 0ull;
    unsigned long long mRayLevelRefits = 0ull;
    unsigned long long mRayLevelWalks = 0ull;
    /// THE ONE PLACE the per-item predicates are computed (GpuInstanceFlag).
    Ogre::uint32 gpuFlagsFor(const Node &n) const;
    /// A seam that changed what a slot's entry SAYS without moving anything —
    /// a visibility, light-mask, cast-shadow, render-queue or material write.
    /// The movement epoch cannot see those (a furniture visibility write is
    /// deliberately not scene movement, VR-SCAN-1), so they say so by name.
    void markGpuSlotDirty(const Node &n);
public:
    /// THE RAY LEVEL'S PASS (ATOM P3's AT-A8r). Called once per frame per drawn
    /// scene with the eye and the projection of the view that draws it; runs
    /// nothing when neither the camera nor the table moved.
    void updateRayLevels(const Ogre::Vector3 &eye, float projScaleY, float viewportHeight);
    /// ATOM P3's CULL, run once over this scene's table (OgreGpuCull.cpp). `hzb`
    /// null (or a request with hzbLevels 0) is the frustum-only mode.
    bool runGpuCull(const GpuCullRequest &req, Ogre::TextureGpu *hzb, bool readBack,
                    GpuCullResult &out);
private:
    /// A slot left the index or was renumbered: its ray-level band is no longer
    /// about the object that now lives there.
    void forgetRayLevel(uint32_t slot);
    void composeGpuInstance(const Node &n, const Ogre::Matrix4 &world, bool graphIsCurrent,
                            detail::GpuInstance &out) const;
    /// The mesh table entry for an attached mesh, reference-counted per attach.
    uint32_t acquireGpuMesh(const MeshRec &rec);
    void releaseGpuMesh(const Ogre::Mesh *mesh);
    /// THE DECAL INDEX, the same shape and for the same reason: the probes
    /// capture decals (they are projected in the Forward+ pass that renders the
    /// cube faces), so a decal that moves, arrives or leaves is a probe input.
    /// THE PER-FRAME SCANS' SCRATCH (clean-2 lane, 2026-09-13). Every one of
    /// these was a local container built and destroyed on every drawn frame of
    /// every scene — the promise is that a still scene costs nothing, and a
    /// still scene was allocating a dozen times a frame. Members, cleared and
    /// refilled: after the first few frames they never allocate again.
    std::vector<std::pair<NodeId, Ogre::Light *>> mScanPoints, mScanSpots;
    std::vector<unsigned char>                    mScanMarked;
    std::vector<Ogre::Aabb>                       mScanReach;
    std::unordered_map<NodeId, unsigned long long> mScanKeys;
    std::vector<MaterialId>                       mScanDeforming;
    /// THE CASTERS WHOSE MATERIAL MOVES VERTICES EVERY FRAME (audit ON-16,
    /// 2026-09-18), by node id, as the last full caster walk found them.
    ///
    /// A material with a VERTEX-STAGE generated piece reads the shader clock, so
    /// its items' shadow maps are never cacheable — the way Unreal excludes
    /// world-position-offset materials from its shadow caches. That used to take
    /// the caster walk's still-frame gate out FOR THE WHOLE SCENE (`deforms` in
    /// runItemWalk): one wind material anywhere and every item in the scene was
    /// visited every frame again, which is the cost the gate exists to remove.
    ///
    /// The roster is what makes the gate per ITEM: it is rebuilt by every full
    /// walk (which visits every item anyway, so it costs nothing extra), and on
    /// a still frame the walk re-flags exactly these nodes and returns. It
    /// cannot go stale while the gate holds, because every seam that could
    /// change a node's material, a material's pieces, or a caster's presence
    /// counts a shadow-scan input (markShadowShapeDirty / noteShadowScanInput)
    /// — and that moves the epoch, which forces a full walk that rebuilds it.
    std::vector<NodeId>                           mShadowDeformers;
    /// (materialId, the albedo/emissive half of noteMaterialChanged) for
    /// materials waiting on a texture — see settleTextureResidency.
    std::vector<std::pair<MaterialId, bool>> mMaterialsAwaitingTexture;
    std::vector<Node *> mDecalNodes;
    void indexDecalNode(Node &n);
    void unindexDecalNode(Node &n);
    /// ONE WALK, TWO CONSUMERS (walkItems, via runItemWalk): the GI movement
    /// records and the frame's caster changes, from the same pass.
    struct ShadowChange { Ogre::Aabb box; Ogre::uint32 channels = 0; };
    void walkItems(bool gi, bool shadow, bool fresh);
    std::vector<ShadowChange> mShadowChanges;
    std::vector<ShadowChange> mShadowVanished;   ///< casters whose Item died since the last walk
    bool mShadowWalked = false;                  ///< this frame's walk produced mShadowChanges
    /// The GI movement scan has run this frame (reset by updateGiTracking, the
    /// once-per-frame entry point): whoever consumes mGiMovedBoxes first runs it.
    bool mGiWalkedThisFrame = false;
    /// THE STILL-FRAME SKIP (clean-2 lane, 2026-09-13). The GI scan is the only
    /// per-frame walk of every item left in a still scene, and it exists solely
    /// to notice a transform nobody told the engine about. `transformEpoch()` is
    /// the host's process-wide transform-write counter (Engine::
    /// setTransformWriteCounter) plus this scene's OWN writes; when it has not
    /// moved since the last scan, nothing can have moved and the scan is
    /// skipped. Without a host counter the epoch is unavailable and every frame
    /// scans, exactly as before.
    unsigned long long transformEpoch() const;
    unsigned long long mGiWalkEpoch = 0;          ///< the epoch the last scan ran at
    bool mGiWalkEpochValid = false;               ///< ...and whether it was ever set
    /// THE SAME SKIP FOR THE CASTER HALF (ENGINE-4 F5). It was left out when
    /// the GI half got its gate because a caster's inputs are not all
    /// transforms — a pose, a material's vertex piece, a rebuilt Item, a Cast
    /// Shadow flag — so it ran O(items) every frame for every drawn scene with
    /// a cacheable lamp, still or not. Those inputs are PUSHED at their own
    /// seams, and each of them now bumps mShadowScanWrites, so the caster half
    /// can ask the same question the GI half asks: could anything I read have
    /// changed since I last looked?
    unsigned long long shadowEpoch() const { return transformEpoch() + mShadowScanWrites; }
    unsigned long long mShadowScanWrites = 0;     ///< pushed caster inputs that move nothing
    unsigned long long mShadowWalkEpoch = 0;      ///< the epoch the last caster walk ran at
    bool mShadowWalkEpochValid = false;
    unsigned long long mCasterWalkItems = 0;      ///< item visits by the caster half, ever
    unsigned long long mSceneTransformWrites = 0; ///< OUR writes: setNodeTransform, riders
    unsigned long long mGiScans = 0;              ///< movement scans actually run, ever
    /// getWorldAabbUpdated calls made by OUR GI code, ever (GiStatus::
    /// giAabbReads). Mutable: two of the four readers are const signatures.
    mutable unsigned long long mGiAabbReads = 0;
    /// THE FIT, MEMOISED AGAINST THE CONTENT IT WAS MADE FOR (ENGINE-7 item 2).
    /// giItemBounds is a pure function of the gathered boxes AND of the volume
    /// the last fit produced, which is what makes a re-fit non-idempotent: it
    /// would read its own output through the hysteresis floor. Computing it
    /// ONCE PER CONTENT removes both failures at once — the ratchet (a re-fit
    /// growing its own answer) and the oscillation (a later re-solve dropping a
    /// floor an earlier one granted). `mGiFitContentNow` is what the last
    /// gather saw; the rest is the answer adopted for it.
    mutable unsigned long long mGiFitContentNow = 0;
    mutable std::vector<Ogre::Aabb> mGiFitBoxes;
    mutable unsigned long long mGiFitBoxesKey = 0;
    mutable bool mGiFitBoxesValid = false;
    /// THE TWO SIGNATURES THE MIRROR READS EVERY FRAME (giEscapeSignature,
    /// giGeometrySignature) ARE PURE FUNCTIONS OF THE SAME BOXES, and each was
    /// a full walk of mNodes with a root-recursive getWorldAabbUpdated per GI
    /// item — so a still frame paid THREE such walks (these two plus
    /// ensureGiWalk) before Ogre's own update. Cached against the movement
    /// epoch, with the volume the escape test is relative to in the key
    /// (clean-2 lane, 2026-09-13).
    mutable unsigned long long mEscapeSigEpoch = 0, mEscapeSig = 0;
    mutable bool               mEscapeSigValid = false;
    mutable Ogre::Aabb         mEscapeSigVolume;
    mutable bool               mEscapeSigVolumeValid = false;
    mutable GiMode             mEscapeSigMode = GiMode::Off;
    mutable unsigned long long mGeomSigEpoch = 0, mGeomSig = 0;
    mutable bool               mGeomSigValid = false;
    mutable GiMode             mGeomSigMode = GiMode::Off;
    /// ONE WALK FOR BOTH SIGNATURES (audit D4). They are pure functions of the
    /// same boxes and the host reads them in the same statement, so a miss on
    /// either computes both — over `mItemNodes`, never the node map.
    void computeGiSignatures() const;
    /// ...and the same for the Forward+ slice walk's scene bounds, which runs
    /// one frame in thirty and read every item's updated AABB to do it.
    Ogre::Aabb         mFwdPlusBounds;
    bool               mFwdPlusBoundsValid = false;
    unsigned long long mFwdPlusBoundsEpoch = 0;
    double   mShadowScanMicros = 0.0;
    double   mCasterWalkMicros = 0.0;
    double   mGiScanMicros = 0.0;
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
    /// RESOLVED probe capture size (pixels per cube face) of the live grid, 0
    /// when there is none. GiParams::probeCaptureSize is the request.
    int  mPccCaptureSize = 0;
    /// How many candidate probes the last buildPcc DROPPED because their own
    /// captured depth showed nothing within twice their region (GiStatus::
    /// probesDropped — the owner's "the sky is your first reflection asset",
    /// measured per probe instead of by measuring the scene for a room).
    /// Non-zero with no grid at all means every probe saw nothing, which is a
    /// built state; zero with no grid in the hybrid means the build failed.
    int  mProbesDropped = 0;
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
        /// WHY this probe is stale — carried from staleProbeGrid to the frame
        /// that actually captures it, so the render-loop monitor records a
        /// probe capture with the input change that justified it (or `None`).
        /// Costs one byte per probe and is written on paths that already
        /// touch the slot.
        GiStaleReason staleReason = GiStaleReason::None;
    };
    std::vector<ProbeSlot> mProbeSlots;
    /// (probe index, reason) for every probe the BUDGET dirtied this frame —
    /// the monitor's record of "this capture happened because X". Cleared by
    /// latchProbeCaptures, which is where a capture becomes a fact. Empty and
    /// never grown while the monitor is off.
    std::vector<std::pair<unsigned, GiStaleReason>> mProbeDirtiedThisFrame;
    /// The resolved per-frame budget (the request clamped to the grid) — the
    /// CEILING a frame may spend on stale probes. Reported by giStatus; what a
    /// frame actually spent is mProbeCapturesLastFrame.
    int mProbeUpdatesPerFrame = 0;
    /// How many CASCADES the last in-motion light tick actually injected
    /// (DRAG-1): the chain's size minus the cascades a rebuild had already
    /// injected since the previous tick. 0 in the single-volume arm.
    unsigned mGiChainInjections = 0;
    /// EVERY WRITE A LIGHT INJECTION WOULD READ (DRAG-1 round 2, F5): a light's
    /// parameters (setLight), its POSE (setNodeTransform on a node that owns
    /// one — a movable lamp never stales the probe grid, so nothing else sees
    /// it), and a light leaving the scene. Monotonic; only ever compared.
    unsigned long long mGiLightWriteSerial = 0;
    /// Whether the last full refresh took the reuse arm (B4). Reported by
    /// giStatus; cleared by every from-scratch build.
    bool mGiReusedLastRefresh = false;
    /// (The GI items' last-seen world AABBs live on each Node — Node::scan,
    /// walkItems — no longer in a per-scene map keyed by node.)
    /// The AABBs that moved on the most recent scan (union of each mover's old
    /// and new box), in world space. Rebuilt every scan; empty when still.
    std::vector<Ogre::Aabb> mGiMovedBoxes;
    /// A GI item was seen for the FIRST time by a scan after the first one — a
    /// new object arrived. Not a move, but it is an input the probes must see
    /// (P1): the next budget pass stales the grid with reason Moved and clears
    /// it.
    bool mGiItemsAppeared = false;
    /// PROBE-ONLY items (P7): unlit geometry the probe faces capture
    /// (probeSeesItem) but that is not GI geometry — tracked by the same scan
    /// in their own record (Node::scan.probeBox), so they stale the probes when
    /// they move or arrive and never enter mGiMovedBoxes (the dynamic
    /// reservation) nor giGeometrySignature (the voxel re-solve).
    bool mProbeOnlyChanged = false;
    /// FRAMES SINCE THE LAST FRAME THAT SAW A MOVED BOX (DRAG-1, REFLECT F3).
    /// A probe capture is a 512-square six-face HDR photograph of the room with
    /// the probe shadow node recalculated on every face; a photograph of a box
    /// that is still moving is wrong before it is displayed, and the next
    /// frame's move stales it again — so during a drag the budget bought one
    /// such photograph per frame and threw every one of them away. The capture
    /// is DEFERRED to the settle instead: the staleness stays recorded (every
    /// slot keeps `sweepPending`, and `framesSinceUpdate` keeps counting, so
    /// the sweep guarantee is unchanged in shape — it is measured from the
    /// frame the content stopped moving rather than from the frame it started).
    /// Counts up from 0 on every frame that records a moved box.
    unsigned mProbeMotionQuietFrames = 0;
    /// CONSECUTIVE frames that recorded a moved box, and whether those add up
    /// to a GESTURE yet. A single deliberate move — a scripted setPosition, a
    /// nudge, a paste — is NOT "still moving" and must reach the probes in the
    /// frame it happens: at the moment of the first move nothing can know
    /// whether a second is coming, and spending one capture to find out is a
    /// capture, not a drag. The deferral therefore starts on the SECOND
    /// consecutive moving frame, which costs a gesture exactly one wasted
    /// photograph instead of one per frame.
    unsigned mProbeMotionRun = 0;
    bool     mProbeDragActive = false;
    // ---- THE MOVER CHANNEL FOR A DRAGGED STILL (MOVER-1) ------------------
    /// The scene's own GI tick, advanced once a frame in updateGiTracking. It
    /// is what `ScanRec::giMovedTick` is measured against, and it exists
    /// because a gesture has to be told from a nudge by a WINDOW in frames and
    /// the walk that sees the moves does not run on every frame (its still-frame
    /// gate skips it whenever no transform was written).
    unsigned long long mGiTick = 1;
    /// The nodes currently riding the mover channel for a gesture, in
    /// promotion order. Small by construction — it is what the user has hold of.
    std::vector<NodeId> mDragMovers;
    /// A promoted mover's box moved this frame. It is NOT `mGiMovedBoxes`: a
    /// mover is out of every GI gather by definition, so its motion must reach
    /// neither the probe grid nor the cascade dirty list — only this gesture's
    /// own clock.
    bool     mDragMoverMoved = false;
    /// Ticks since the last one that saw a promoted mover move, against the
    /// same kProbeMotionSettleFrames window the probe deferral uses. The end of
    /// the window is the end of the gesture: every mover goes back to being
    /// still world and pays its ONE re-voxelisation there.
    unsigned mDragQuietTicks = 0;
    /// Gestures that have ended (a cumulative reading for gi.drag_mover and
    /// world.giStatus: one per drag, never one per frame).
    unsigned long long mDragMoverGestures = 0;
    /// A GESTURE THAT ENDED OWES ONE FULL AT-REST RE-SOLVE OF THE LIGHT, paid
    /// once its re-voxelisations have drained. MEASURED, and it is the one
    /// thing the promotion cannot get for free: with the irradiance field on,
    /// a drag that ended without it left the Mirror Room's dragged torus 31/255
    /// away from the same pose reached without the promotion, stably (the field
    /// converged during the gesture over a chain that did not contain the
    /// object and never re-converged afterwards — LAMPREST-2's latch), and one
    /// `world.refreshGi()` collapsed the difference to 201 pixels at 6/255.
    /// With the field OFF the two rules already agreed to 258 px at 9/255, which
    /// is what names the term.
    bool     mDragSettleOwed = false;
    /// HOW STILL IS STILL. Ten frames — the same 1/6 s the mirror's own
    /// in-motion cadence (`kGiLightOnlyEveryN`) uses, and chosen from the
    /// movement quantum rather than from taste: a box counts as moved when its
    /// AABB steps a 64th of its own largest extent (giAabbQuantum), so a 1 m
    /// object dragged as slowly as 20 cm/s still records a move about every
    /// fifth frame at 60 Hz. A settle shorter than that would let a slow drag
    /// spend captures between its own steps, which is the defect. It costs a
    /// single deliberate move (a scripted setPosition) ten frames of latency
    /// before its probes start catching up — against a sweep that already
    /// takes `probes / budget` frames (18 on the Mirror Room's grid).
    static constexpr unsigned kProbeMotionSettleFrames = 10u;
    /// THE CEILING ON A DEFERRAL (DRAG-1 round 2, F4). A drag ends; a KEYFRAMED
    /// object in play, a physics body settling, or a script that moves something
    /// every frame does NOT — and without a ceiling that is one endless gesture,
    /// so the probes would hold the pre-motion room for as long as it lasts and
    /// then pay a whole sweep at the end. One capture every N frames while the
    /// deferral holds keeps a long motion LIVE at a bounded price.
    ///
    /// N = 30 is derived from what a capture costs and from the sweep it feeds,
    /// not chosen: one capture is six faces at the tier's size with the probe
    /// shadow node recalculated on each, measured at about 4.3 ms (SMOKE-41), so
    /// one every 30 frames is 0.14 ms amortised — under 1 % of a 16.7 ms frame,
    /// against the 26 % the un-deferred rate was costing. And it is COMFORTABLY
    /// ABOVE the sweep length (18 frames on the Mirror Room's grid at the
    /// shipped budget of 1), which is the other half of the requirement: a
    /// ceiling below the sweep would spend a whole grid's worth of captures
    /// inside one gesture and be back where it started.
    static constexpr unsigned kProbeDeferredCaptureEvery = 30u;
    /// Frames since the deferral last let a capture through. Reset when the
    /// gesture ends, so a NEW gesture never inherits a nearly-expired counter.
    unsigned mProbeDeferredRun = 0;
    /// A NON-MOTION INPUT IS OUTSTANDING (a light, a material, the sky, the fog
    /// or an explicit refresh staled the grid). Motion defers only the captures
    /// MOTION asked for: a lamp switched on while something is being dragged
    /// must still reach the probes. Cleared when no slot is pending any more.
    bool mProbeStaleBeyondMotion = false;
    /// The movement scan has run at least once: before that, every item is
    /// seen for the first time and none of them is an arrival.
    bool mGiScannedOnce = false;
    /// Bumped whenever anything the GI arms hold RAW POINTERS INTO may have
    /// died — every invalidateGiCaches call site (B4). The reuse arm refuses to
    /// re-run an existing voxelizer across a bump, which is what keeps the
    /// "always from scratch" rule's guarantee (VctMaterial's datablock-pointer
    /// cache) exactly as strong.
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
    /// BOOTVOX-1: how many consecutive frames the GI flush has waited for a
    /// voxel-input texture, and the cap past which it builds anyway. One frame
    /// or two is the normal case (the default scene's ground tile); the cap
    /// exists so a decode that never completes cannot park GI for ever.
    unsigned           mGiVoxelTextureWaitFrames  = 0u;
    static const unsigned kGiVoxelTextureWaitFrames = 30u;
    /// ...and whether the wait for THIS pending set has already been given up
    /// on, so a texture that never becomes ready costs thirty deferrals ONCE
    /// and not thirty per rebuild for the life of the scene. Cleared the moment
    /// no voxel input is in flight.
    bool               mGiVoxelTextureWaitGaveUp  = false;
    unsigned long long mGiBuiltMaterialGeneration = 0;
    /// THE PROBE CACHE's bookkeeping (ENGINE_CACHE_POLICY_SPEC P1). See
    /// staleProbeGrid and GiStatus: why the grid was last staled, a serial per
    /// stale event, the captures the last rendered frame actually made, the
    /// captures a from-scratch placement made this frame (they bypass mDirty),
    /// and how many from-scratch builds the scene has had.
    GiStaleReason      mLastStaleReason = GiStaleReason::None;
    unsigned long long mStaleSerial = 0;
    int                mProbeCapturesLastFrame = 0;
    /// FRAMES THE MOTION DEFERRAL HELD THE BUDGET (DRAG-1). Monotonic for the
    /// life of the scene; `GiStatus::probeCapturesDeferred` reports it, which
    /// is what lets a suite assert "the drag spent nothing" from outside.
    unsigned long long mProbeCapturesDeferred = 0;
    int                mPlacementCapturesThisFrame = 0;
    unsigned long long mGiRebuilds = 0;
    /// How many of those rebuilds a MOBILITY change caused (MobilityStatus::
    /// mobilityRebuilds). Its own counter and not a share of mGiRebuilds
    /// because the question it answers is "is my classification costing me
    /// rebuilds?", which a total cannot: every other rebuild reason (a mode
    /// change, a destroyed object, a quality dial) is mixed into that one.
    unsigned long long mMobilityRebuilds = 0;
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
    ///
    /// A SET AND NOT A VECTOR (PHOTON_SPEC E2 (3)). Nothing here reads an ORDER
    /// — the two questions ever asked of it are "is this node already in?" and
    /// "how many?" — and the membership test ran as a linear `std::find` inside
    /// the reuse arm's walk over every node, which is O(N x M) in the scene's
    /// size: measured at **79 ms of CPU per refresh** on the 8,404-node lattice
    /// (PHOTON_SPEC P0 §6.4), i.e. five frames' worth of budget spent deciding
    /// that nothing had been added.
    std::unordered_set<NodeId> mVctItemIds;
    /// Live decals in THIS scene. The SceneManager-level atlas binding is
    /// driven off the count (see refreshDecalBindings).
    unsigned            mDecalCount = 0;
    // Planar-reflection arm. mPlanar is null unless mPlanarParams.budget > 0.
    // mReflectors is the DOCUMENT's set of reflector nodes and survives the arm
    // going up and down; mActors only exists while the arm is up.
    /// Ogre's own type since ogre-patch 0052 gave PlanarReflections the two
    /// per-slot accessors the lamp-map cache and the frame monitor need
    /// (getNumActiveActorSlots / getActiveActorWorkspace) — it used to be a
    /// derived class of ours reaching into `mActiveActorData`.
    Ogre::PlanarReflections *mPlanar = nullptr;
    PlanarReflectionParams   mPlanarParams;
    std::string              mPlanarWorkspaceDef;
    std::vector<std::string> mPlanarNodeDefs;
    std::set<NodeId>         mReflectors;
    std::map<NodeId, Ogre::PlanarReflectionActor *> mActors;
    /// WHAT THIS SCENE WAS AUTHORED FOR, as far as rays go (ledger §425). The
    /// document's field, pushed by SceneMirror; `rayTracingResolved()` is the
    /// only thing that reads it, and that is what the ray stages ask.
    RayTracingMode           mRayTracing = RayTracingMode::Auto;
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
    /// How many times guardSamplerCeiling has had to step off a full block, and
    /// whether the one log line has been written. See it for what a generation
    /// is; 0 is the state every scene that ever shipped stays in.
    unsigned            mSamplerGeneration = 0;
    bool                mSamplerCeilingLogged = false;
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
    /// `takeBlank` = "and put the clear-only chain up in its place", which is
    /// what a scene-less view owns (chain::buildBlank). FALSE from destroy()
    /// only: this view is going away, and building a two-pass chain — and, in a
    /// process that never lost a scene, the engine's blank SceneManager — to
    /// tear it down one line later is work nobody can see. In particular it
    /// kept ~OgreEngine from creating a SceneManager inside its own destructor.
    void detachScene(bool takeBlank = true);

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
    /// twin). The same PSSM + focused layout as the main atlas at a
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

    // ---- VR (SPECS/VR_SPEC.md §4.3) ---------------------------------------
    /// Makes this view's chain a STEREO one: every scene pass renders both eyes
    /// into a target two eyes wide (ChainDesc::stereo). `cullCamera` is the
    /// name of a camera sitting between the eyes. Rebuilds the workspace
    /// definition, because the flag lives on the pass definitions.
    /// Called only by the VR session, on the View it owns.
    void setStereo(bool on, const std::string &cullCamera);
    bool stereo() const { return mStereo; }
    /// THE ONE-SESSION REFLECTION OVERRIDE (lane EYE-GRADE-1). The SSR row a
    /// stereo view renders with is the PROJECT's — the mirror pushes it here
    /// like it does into the desktop's view — and this is `vr.begin({
    /// reflections:n})`'s measurement arm over it: -1 follows the project, 0/1/2
    /// pin the row for as long as this view is stereo. Applied inside
    /// `applyVrViewPolicy`, so it cannot be forgotten by a later push.
    void setVrSsrOverride(int row);
    int  vrSsrOverride() const { return mVrSsrOverride; }
    /// HOW MANY TIMES THIS VIEW'S PER-FRAME CHAIN GLOBALS HAVE BEEN PUSHED
    /// (chain::ViewGlobalsListener). The meter's uniforms, the auto exposure's
    /// terms, the bloom threshold, the AO and SSR camera terms and every look's
    /// parameters ride that push and NOTHING else — so a view whose count does
    /// not climb is a view rendering with whatever the last workspace to update
    /// happened to leave in the process-wide materials. @see globalsPushes.
    void noteGlobalsPush() { ++mGlobalsPushes; }
    unsigned long long globalsPushes() const override { return mGlobalsPushes; }
    /// THE TWO EYES THIS VIEW IS RENDERING, this frame (@see StereoEyeBasis).
    /// Pushed by the VR session every frame it locates them, dropped when the
    /// session ends; read by the ray-traced reflection so each eye's pixels get
    /// rays from that eye. Null until a session pushes a located pair.
    void setStereoEyes(const StereoEyeBasis &left, const StereoEyeBasis &right) {
        mStereoEyes[0] = left; mStereoEyes[1] = right; mHaveStereoEyes = true;
    }
    void clearStereoEyes() { mHaveStereoEyes = false; }
    const StereoEyeBasis *stereoEyes() const { return mHaveStereoEyes ? mStereoEyes : nullptr; }
    /// THE GI DRIVER (VR_SPEC §3.4 / §7 item 6). The Photon cascade chain
    /// follows ONE camera per scene, and the engine elects it by creation order
    /// among enabled ON-SCREEN views — which would leave a VR session (whose
    /// View is offscreen, and created last) looking at cascades centred on the
    /// desktop camera. A view that says so here wins the election outright.
    void setGiPriority(bool on) { mGiPriority = on; }
    bool giPriority() const { return mGiPriority; }
    /// Drops the live workspace (detaching its listeners first). Safe when
    /// there is none; returns whether one was actually dropped.
    ///
    /// INCLUDES THE CLEAR-ONLY WORKSPACE (lane STALE-VIEW-1): a scene-less view
    /// owns one, it targets the same texture, and every caller of this pair is
    /// about to change that texture or the definitions behind it — so both
    /// kinds go through the one seam and the `hadWorkspace` answer means the
    /// same thing for both.
    bool detachWorkspace();
    /// THE CLEAR-ONLY WORKSPACE of a view with no scene bound (chain::buildBlank
    /// says why it exists). Built by attachWorkspace when there is no scene,
    /// dropped by detachWorkspace and replaced by the scene chain at the next
    /// bind. Definitions are rebuilt with it, so a background change carries.
    bool attachBlankWorkspace();
    bool detachBlankWorkspace();
    /// Removes the clear-only chain's definitions. Called from destroy() only —
    /// every other path rebuilds them through attachBlankWorkspace.
    void destroyBlankChain();
    /// Is this view drawing its clear-only workspace THIS frame? Read by
    /// OgreEngine::renderOneFrame, which must update the blank scene manager in
    /// the same frame its workspace runs (the rule stated in that loop).
    bool drawsBlank() const { return mEnabled && mBlankWorkspace != nullptr; }
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
    /// Frames presented since THIS WORKSPACE was built (reset by every chain
    /// rebuild) — what says whether a compute pass of the chain has ever run,
    /// which is how `HzbStatus::primed` is answered.
    unsigned long long workspaceFramesPresented() const { return mWorkspaceFramesPresented; }
    unsigned long long blankFramesPresented() const override;
    bool warmUpShaders() override;
    /// Called by OgreEngine::renderOneFrame AFTER Root::renderOneFrame: counts
    /// this frame if the view was actually part of it (enabled + workspace +
    /// scene). The one place mFramesPresented moves up.
    void notePresented();

    void setPostFx(const PostFxDesc &fx) override;
    const PostFxDesc &postFx() const override;
    void resetExposureHistory() override;
    void seedExposureHistory(float scale) override;
    void setHelpersVisible(bool on) override;
    bool helpersVisible() const override { return mHelpersVisible; }
    void setVrHelpersVisible(bool on) override;
    bool vrHelpersVisible() const override { return mVrHelpersVisible; }
    /// THE RUNTIME'S HIDDEN-AREA MESH, for this view only (kVrMaskBit, lane
    /// HAM-1). Engine-internal: the VR session is the only caller, and there is
    /// no public View verb for it because no host has a reason to ask.
    void setHiddenAreaMask(bool on);
    bool hiddenAreaMask() const { return mHiddenAreaMask; }
    void setLodHysteresisOffscreen(bool on) override;
    bool lodHysteresisOffscreen() const override { return mLodHysteresisOffscreen; }
    float measuredExposureScale() const override;

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
    /// This view's LIVE workspace, or null. Read-only, for the monitor's
    /// workspace census and its compositor-graph snapshot; nothing mutates a
    /// view's workspace from outside the seam.
    Ogre::CompositorWorkspace *workspace() const { return mWorkspace; }
    /// The texture this view draws into: the window's swapchain texture on an
    /// on-screen view, the RTT on an offscreen one. The VR session needs it for
    /// both of its jobs — the eye copy reads the session View's target, and the
    /// mirror writes the mirror View's.
    Ogre::TextureGpu *targetTexture() const;
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
    /// The engine that made this view. Set right after construction by
    /// OgreEngine::createView/createOffscreenView; never null for a view a host
    /// can reach. chainDesc() reads the MACHINE's answers through it (ray
    /// queries available, the app's ray-tracing preference) — a scene cannot be
    /// asked, because a view has none until setScene and its graph is built in
    /// the constructor.
    OgreEngine *mEngine = nullptr;

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
    void applyPendingResizeImpl();
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
    /// Arms or disarms this view's RAY-TRACED REFLECTION listener (PHOTON_SPEC
    /// §7 R5). Armed while the view is enabled, has a scene and a camera, and
    /// its chain carries `rayReflect` — which is the SSR row being on AND the
    /// machine advertising ray queries AND the application preference allowing
    /// them. Same idempotent once-a-frame contract and the same seam as the two
    /// above, and for the same reason: every one of those can change between
    /// frames. Defined in OgreRayQuery.cpp (both halves of it).
    void syncReflectListener();
    /// DROPS the reflection trace's per-view Vulkan state, flushing first.
    /// Called from `detachWorkspace` — the one seam every workspace rebuild goes
    /// through — because the trace's descriptor set holds IMAGE VIEWS OF THIS
    /// CHAIN'S TEXTURES, and a rebuild destroys every one of them. An image
    /// destroyed while a command buffer that has a set referencing it bound is
    /// still recording INVALIDATES that command buffer: `vkEndCommandBuffer`
    /// then fails and the frame dies with VK_ERROR_DEVICE_LOST (measured; the
    /// validation layer names the image, the view and the set). Ogre's own sets
    /// are safe because it recreates them with the textures; ours has to be
    /// told. Defined in OgreRayQuery.cpp (both halves).
    void dropReflectState();
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


    /// VR_SPEC §4.3: this view draws both eyes (chain::build's applyStereo),
    /// and this view places the GI cascades for its scene whatever else is on
    /// screen. Both are false on every view but the session's.
    bool                       mStereo = false;
    bool                       mGiPriority = false;
    std::string                mCullCameraName;
    /// THE SESSION'S ONE-RUN REFLECTION OVERRIDE (lane EYE-GRADE-1;
    /// `vr.begin({reflections:n})`). -1 — every view but a session's — means
    /// "whatever the project's row says", which is what the mirror pushes.
    int                        mVrSsrOverride = -1;
    /// @see noteGlobalsPush.
    unsigned long long         mGlobalsPushes = 0;
    /// WHAT THE VR POLICY LAST TOOK AWAY, as one number (looks * 4 + refraction
    /// bit * 2 + distortion bit) — the latch behind the one log line that tells
    /// an author why something they can see on the desktop is not in the
    /// headset. size_t(-1) = nothing said yet.
    size_t                     mVrPolicyDropped = size_t(-1);
    /// The located eyes of THIS frame (@see StereoEyeBasis). Not part of the
    /// chain's identity — they change every frame and change no pass.
    StereoEyeBasis             mStereoEyes[2];
    bool                       mHaveStereoEyes = false;

    Ogre::Root                *mRoot;
    Ogre::Window              *mWindow;
    Ogre::TextureGpu          *mTexture;
    Ogre::Camera              *mCamera    = nullptr;
    Ogre::CompositorWorkspace *mWorkspace = nullptr;
    OgreScene                 *mScene     = nullptr;
    /// The clear-only workspace, its camera on the engine's blank scene manager
    /// and its definitions (chain::buildBlank). Live exactly while no scene is
    /// bound; the camera is created once and outlives the workspace rebuilds.
    Ogre::CompositorWorkspace *mBlankWorkspace = nullptr;
    Ogre::Camera              *mBlankCamera    = nullptr;
    std::string                mBlankWorkspaceDef;
    std::vector<std::string>   mBlankNodeDefs;
    /// @see View::blankFramesPresented.
    unsigned long long         mBlankFramesPresented = 0;
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
    /// Owned; registered the same way while this view's chain traces
    /// reflections (PHOTON_SPEC §7 R5). Null everywhere else — on a machine
    /// without ray queries, with the preference off, or on any view whose chain
    /// has no SSR (every thumbnail, preview and pixel suite, by construction).
    /// Defined in OgreRayQuery.cpp, which is why it is held through a pointer
    /// the rest of the engine never dereferences.
    std::unique_ptr<ReflectPassListener> mReflectListener;
    unsigned                   mWorkspaceGeneration = 0;
    /// What `ChainDesc::rayReflect` was when the CURRENT workspace definition
    /// was built. The scene arrives AFTER the chain is first built (the
    /// constructor has no scene), and the project's ray row can change under a
    /// live view, so the shape is re-checked once a frame in
    /// syncReflectListener rather than only when a host pushes a PostFxDesc.
    bool                       mChainRayReflect = false;
    /// ...and what `ChainDesc::probeGather` was (GATHER-1a): the gather's row
    /// is the scene's, so the shape is re-checked once a frame beside the
    /// reflection's (OgreView::syncReflectListener).
    bool                       mChainProbeGather = false;
    /// Frames drawn+presented since the current scene was bound (see
    /// View::framesPresented). Reset by setScene/detachScene, NOT by a
    /// workspace rebuild.
    unsigned long long         mFramesPresented = 0;
    /// The same count for the CURRENT workspace only — reset by attachWorkspace,
    /// which is the one seam every (re)build goes through. What
    /// measuredExposureScale needs: "has this graph written its keep_content
    /// textures yet", a question a rebuild answers differently from a scene bind.
    unsigned long long         mWorkspaceFramesPresented = 0;
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
    /// Does this view draw the editor's furniture (View::setHelpersVisible)?
    /// Graph shape — it is a per-pass visibility mask, ChainDesc::helpers.
    bool                       mHelpersVisible = true;
    /// ...and the VR channel (kVrHelperBit). Off everywhere but the session's
    /// own view, so nothing meant for a headset reaches a desktop picture.
    bool                       mVrHelpersVisible = false;
    bool                       mHiddenAreaMask = false;
    /// Does this OFFSCREEN view get the LOD switch band anyway
    /// (View::setLodHysteresisOffscreen)? Graph shape, like the two above; false
    /// everywhere but the one suite that has to read what the band does.
    bool                       mLodHysteresisOffscreen = false;
    /// An exposure multiplier a host handed over before this view had a chain
    /// that could take it (View::seedExposureHistory). Spent by attachWorkspace
    /// on the chain it builds, once; 0 = nothing owed.
    float                      mPendingExposureSeed = 0.0f;
    std::string               &mError;
};

// ---------------------------------------------------------------------------
/// THE OPENXR SESSION (SPECS/VR_SPEC.md §4.3) — everything about it lives in
/// OgreVrSession.cpp, the second TU allowed to include Vulkan (after
/// OgreRayQuery.cpp) and the ONLY one that includes OpenXR. Declared here so
/// the engine can hold one and the frame can call it; nothing else in this
/// header knows what an XrSession is.
///
/// TWO OBJECTS, TWO LIFETIMES, and the split is forced by the pin (§2.1):
///   * `VrBoot` is the INSTANCE and the DEVICE. It is created inside
///     OgreEngine::init, interleaved with the Root/plugin/initialise order,
///     because the render system reads `external_instance` in its CONSTRUCTOR
///     and the first createRenderWindow reads `external_device`. It lives for
///     the engine's life and outlives any number of sessions.
///   * `VrSession` is the XrSession, the swapchains, the both-eyes target, the
///     stereo View and the pump. It lives for as long as the user is in the
///     headset.
class VrBoot;
class VrSession;

/// The four calls OgreEngine.cpp makes into that TU. Free functions rather
/// than methods so the engine never has to see either class's definition.
namespace vr {
/// Step 1 of the init order: the XrInstance, the system, and the VkInstance
/// the RUNTIME creates from our own VkInstanceCreateInfo. Returns null and
/// fills `reason` when anything refuses — which is not an error anywhere:
/// the caller boots plainly and answers vrAvailable() false.
/// MUST be called BEFORE Root::loadPlugin (the external instance is consumed
/// in the render system's constructor).
VrBoot *bootBegin(VrInfo &infoOut, std::string &reason);
/// The `VulkanExternalInstance *` for loadPlugin's `external_instance`, as an
/// opaque pointer (the type belongs to the render system).
void *bootExternalInstance(VrBoot *);
/// Step 2: the physical device the runtime wants, the device-creation request
/// ogre-patch 0068 exports, and xrCreateVulkanDeviceKHR. MUST be called AFTER
/// Root::initialise (the exporter reads the instance-extension list the render
/// system's constructor filled — phase 1a's finding §8.5) and BEFORE the first
/// createRenderWindow. False = the boot is abandoned; `reason` says why.
bool bootDevice(VrBoot *, Ogre::Root *root, VrInfo &infoOut, std::string &reason);
/// The `VulkanExternalDevice *` for the first window's `external_device`.
void *bootExternalDevice(VrBoot *);
/// Destroys the XrInstance and the VkDevice/VkInstance WE own. Call after
/// Root is deleted (Ogre destroys neither — §2.1 row 8).
void bootEnd(VrBoot *);
/// Creates the session. Null + `reason` on refusal.
VrSession *sessionBegin(VrBoot *, OgreEngine *, OgreScene *, const VrConfig &,
                        std::string &reason);
void sessionEnd(VrSession *);
/// IS THE LIVE SESSION'S PICTURE ENCODED EXACTLY ONCE between this renderer and
/// the wearer's eye (lane EYE-GRADE-1)? False only while a session is running
/// on a runtime that offered no _SRGB swapchain format and is therefore going
/// to encode our display-ready bytes a SECOND time; true when no session runs.
///
/// PROCESS-WIDE, because a session is (`Engine::beginVrSession` refuses a
/// second), and declared HERE rather than reached through the engine because
/// the caller that needs it most is CHAIN code — a composite that dithers the
/// final picture must stand down when the runtime is about to re-encode it,
/// and the chain has no session pointer.
bool colourEncodedOnce();
}  // namespace vr

/// THE PUMP, from the frame's point of view. `vrSessionBeginFrame` polls the
/// runtime's events, blocks in xrWaitFrame (the session's clock), locates the
/// eyes and writes the head pose and the per-eye projections onto the View's
/// camera; when the runtime asks for no picture this frame the pump has
/// already given it its empty frame and the desktop draws anyway (F4 — the
/// old bool answer was dead and is gone). `vrSessionEndFrame` releases the
/// swapchain images and submits the projection layer.
void    vrSessionBeginFrame(VrSession *);
void    vrSessionEndFrame(VrSession *);
VrState vrSessionState(const VrSession *);
/// IS THE SESSION OVER? (lane VR-3b.) True once nothing can come of it any
/// more: the runtime stopped it, the runtime went away, or the device was lost.
/// The frame tail ends such a session — the state alone cannot say it (a
/// stopped session reads `Idle`, which is also what a session that has not
/// started yet reads).
bool    vrSessionIsOver(const VrSession *);
VrStatus vrSessionStatus(const VrSession *);
View   *vrSessionView(const VrSession *);
/// WHICH SCENE IS BEING WORN (VR-4-FIX finding 1). A session holds a raw
/// `OgreScene *` and dereferences it every frame (the stereo quads) and at
/// teardown (its cull camera), so `destroyScene` has to be able to ask — a host
/// that closes a project while a preview runs would otherwise free the world
/// out from under a live session.
OgreScene *vrSessionScene(const VrSession *);
void    vrSessionSetMirror(VrSession *, OgreView *);
/// Places the reference space in the world (Engine::setVrOrigin). Position and
/// a heading in degrees about +Y; never a pitch or a roll.
void    vrSessionSetOrigin(VrSession *, const Vec3 &position, float yawDegrees);
/// RE-CHECKS THE MIRROR against the view it is painting onto, mid-frame.
///
/// The frame applies pending window RESIZES (OgreView::applyPendingResize)
/// AFTER the session's pump has already decided what the mirror looks like, and
/// a resize destroys and rebuilds the window's swapchain under a mirror
/// workspace that is about to execute against it. Calling this after the resize
/// loop is what stops that workspace running on a target that has changed
/// shape; it costs a handful of integer compares when nothing moved.
void    vrSessionSyncMirror(VrSession *);
bool    vrSessionEyeScreenshot(VrSession *, unsigned eye, Image &out, std::string &error);
/// HAS THE RUNTIME BOUND A REAL PROFILE for this hand? (The injection refusal
/// rule, VR_INPUT_SPEC §2.4 I1: the wearer's own hardware always wins.)
bool    vrSessionHasBoundProfile(const VrSession *, int hand);
/// THIS FRAME'S JOINTS for one hand, world space through the rig, in the
/// extension's order (stage 3; `Engine::vrHandJoints`). 0 = that hand's
/// skeleton is not being tracked this frame.
unsigned vrSessionHandJoints(const VrSession *, int hand, VrPose *out, unsigned count);
/// THE SUGGESTED-BINDING BLOCKS THE SESSION OFFERED, and what the runtime did
/// with each (`Engine::vrBindingBlocks`).
unsigned vrSessionBindingBlocks(const VrSession *, VrBindingBlock *out, unsigned count);
/// IS THE RUNTIME REALLY TRACKING that hand's skeleton? (The injection refusal
/// rule for joints: a wearer's own hand always wins over a script's.)
bool    vrSessionHasLiveJoints(const VrSession *, int hand);
/// WAS THIS SESSION ASKED FOR BARE HANDS (`VrConfig::hands`, lane
/// HANDS-SWITCH-1)? False = a project on controllers: no hand bindings were
/// suggested, no tracker exists, and no skeleton is reported from any source.
bool    vrSessionHandsEnabled(const VrSession *);
/// THE ONE READING OF `JAHSHAKA_VR_TEST_INJECT` (VR_INPUT_SPEC §2.4 I1), for
/// the two places that enforce the refusal rule: the WRITE (Engine::
/// vrInjectInput refuses one) and the per-frame READ (the session ignores and
/// clears a sample a bound profile has overtaken). Read LIVE rather than
/// latched at boot, which is what lets one process prove both halves of the
/// rule — and it is deliberately not a member of anything: an escape hatch
/// with two spellings is an escape hatch with a hole in it.
inline bool vrTestInjectAllowed() {
    const char *allow = std::getenv("JAHSHAKA_VR_TEST_INJECT");
    return allow && *allow && std::strcmp(allow, "0") != 0;
}
/// ONE BUZZ (Engine::vrHaptic). False only when the call itself failed.
bool    vrSessionHaptic(VrSession *, int hand, float amplitude01, float seconds,
                        std::string &error);

class OgreEngine final : public Engine {
public:
    bool init(const EngineConfig &cfg, std::string &error);

    Scene *createScene(const std::string &name, unsigned workerThreads = 0) override;
    void *documentGraphScene() override;
    /// The blank scene manager (@see mBlankScene), created on the first call.
    /// Null only when the Hlms is not registered yet, which cannot happen on
    /// the path that asks: a View exists by then.
    Ogre::SceneManager *blankSceneManager();
    bool  isHeadless() const override { return mHeadless; }

    void destroyScene(Scene *scene) override;
    /// Destroys whatever a frame asked for while it was running
    /// (mPendingSceneDestroy). Called ONLY from renderOneFrame's frame guard,
    /// after the in-frame flag has been cleared, and never throws — a
    /// destructor runs it.
    void drainPendingSceneDestroys() noexcept;

    View *createView(const std::string &name,
                     NativeWindowHandle handle, unsigned width, unsigned height,
                     const Colour &background) override;

    View *createOffscreenView(const std::string &name, unsigned width, unsigned height,
                              const Colour &background) override;

    void destroyView(View *view) override;

    void setTransformWriteCounter(const std::atomic<unsigned long long> *counter) override;
    void renderOneFrame() override;
    /// THE FRAME'S CLOSE, RUN FROM renderOneFrame's SCOPE GUARD AND NOWHERE
    /// ELSE (lane FRAME-CATCH-1): the runtime's xrEndFrame, the monitor's
    /// record, a lost/stopped session's end, the device-lost latch and the
    /// deferred scene teardowns — on every exit from a frame, thrown or not.
    /// Never throws: a destructor runs it.
    void closeRenderFrame() noexcept;
    /// Steps 3 and 4 of that close, each on its own so the close reads as the
    /// list it is. Called from closeRenderFrame ONLY.
    void endLostOrStoppedVrSession();
    void latchDeviceLost();
    void setFrameFault(FrameFault fault, unsigned frames) override;
    /// Raises the armed fault (mFrameFault) and spends one of its frames.
    /// Called from ONE site inside renderOneFrame; always throws.
    void raiseFrameFault();
    bool deviceLost() const override;
    void advanceResources() override;

    // ---- VR (SPECS/VR_SPEC.md §4) -----------------------------------------
    bool vrAvailable() const override { return mVrBoot && !mVrDeviceFailed; }
    const VrInfo &vrInfo() const override { return mVrInfo; }
    bool beginVrSession(Scene *scene, const VrConfig &cfg) override;
    void endVrSession() override;
    VrState vrState() const override;
    VrStatus vrStatus() const override;
    View *vrView() const override;
    void setVrMirrorView(View *view) override;
    View *vrMirrorView() const override { return mVrMirrorView; }
    void setVrOrigin(const Vec3 &position, float yawDegrees) override;
    bool vrEyeScreenshot(unsigned eye, Image &out) override;
    bool vrInjectInput(int hand, const VrHandState &state) override;
    void vrInjectFocus(bool focused) override { mVrInjectFocus = focused; }
    unsigned vrHandJoints(int hand, VrPose *out, unsigned count) const override;
    bool vrInjectJoints(int hand, const VrPose *joints, unsigned count) override;
    unsigned vrBindingBlocks(VrBindingBlock *out, unsigned count) const override;
    bool vrHaptic(int hand, float amplitude01, float seconds) override;
    void setVrRay(const VrRayState &ray) override { mVrRay = ray; }
    const VrRayState &vrRay() const override { return mVrRay; }
    /// THE INJECTED SAMPLE FOR ONE HAND, or false when none is live
    /// (OgreVrSession.cpp's readInput asks once per hand per frame). The store
    /// lives HERE rather than in the session because the hook has to work with
    /// no session at all — that is the whole point of it (VR_INPUT_SPEC §2.4).
    bool vrInjectedInput(int hand, VrHandState &out) const {
        if (hand < 0 || hand >= int(VrHandCount) || !mVrInjected[hand]) return false;
        out = mVrInject[hand];
        return true;
    }
    /// FORGETS AN INJECTED SAMPLE (VR-INPUT-1E-FIX finding 1). Three callers,
    /// and between them they are the whole guarantee the refusal rule makes:
    /// `beginVrSession` and `endVrSession` clear BOTH hands (a session never
    /// inherits a script's leftovers and never leaves its own behind), and the
    /// session's per-frame read clears ONE hand the moment the runtime binds a
    /// real interaction profile for it — because a write that was legal before
    /// the profile arrived must not go on standing in for the wearer's hand.
    void vrClearInjectedInput(int hand) {
        if (hand < 0 || hand >= int(VrHandCount)) return;
        mVrInjected[hand] = false;
        mVrInject[hand] = VrHandState();
        // ...AND THAT HAND'S INJECTED SKELETON WITH IT (stage 3). The joints
        // are part of the same fiction — a hand a script put in the room — and
        // a skeleton left behind after the hand was withdrawn would be drawn
        // in the wearer's eyes with nothing holding it up.
        mVrJointsInjected[hand] = false;
    }
    void vrClearInjectedInput() {
        for (unsigned h = 0; h < VrHandCount; ++h) vrClearInjectedInput(int(h));
        // ...AND THE INJECTED FOCUS WITH THEM: a `focused:false` written for
        // one session's cancel test must not be what the next one starts from.
        mVrInjectFocus = true;
    }
    /// IS EITHER HAND A TEST'S? (What makes `VrStatus::inputFocused` read the
    /// injected bit rather than the session's own state.)
    bool vrAnyInjectedInput() const {
        for (unsigned h = 0; h < VrHandCount; ++h) if (mVrInjected[h]) return true;
        return false;
    }
    bool vrInjectedFocus() const { return mVrInjectFocus; }
    /// THE INJECTED SKELETON FOR ONE HAND, or false when none is live (stage
    /// 3). Asked by the session (which prefers the runtime's own joints and
    /// falls back to this) and by `vrHandJoints` with no session at all.
    bool vrInjectedJoints(int hand, VrPose *out, unsigned count) const {
        if (hand < 0 || hand >= int(VrHandCount) || !mVrJointsInjected[hand]) return false;
        if (!out || count == 0u) return true;
        const unsigned n = count < kVrHandJointCount ? count : kVrHandJointCount;
        for (unsigned j = 0; j < n; ++j) out[j] = mVrInjectJoints[hand][j];
        return true;
    }
    /// The live session, for the TU that owns it and for the frame. Null when
    /// none runs.
    VrSession *vrSession() const { return mVrSession; }
    /// Puts `external_device` on a window's misc params the FIRST time a window
    /// is created on a VR boot, and never again (the render system reads it
    /// only while `!mInitialized` — §2.1 row 5). A no-op on a plain boot.
    void applyVrExternalDevice(Ogre::NameValuePairList &params);

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
    unsigned long long textureWaitAdvances() const override;
    double textureWaitWorstMs() const override { return mTextureWaitWorstMs; }
    unsigned textureWaitBudgetMs() const override { return mTextureWaitBudgetMs; }
    unsigned textureMetadataCacheEntries() const override;
    unsigned textureChannelCacheEntries() const override;
    bool saveTextureCache() override;

    /// Applies to every on-screen window that exists AND is remembered for the
    /// ones created later (createView and the MSAA-recreate hook both read it).
    void setVsync(bool on) override;
    bool vsync() const override { return mVsync; }

    // ---- The hardware ray-query tier (PHOTON_SPEC §7 R1) -------------------
    void setRayTracing(bool on) override;
    bool rayTracing() const override { return mRayTracingWanted; }
    bool rayQueryAvailable() const override;
    /// THE FRAME'S RAY-TIER UPDATE: the acceleration structures for every drawn
    /// scene, recorded into the frame's OWN command buffer at the item walk's
    /// point (applyShadowCacheDirties, after updateSceneGraph and before any
    /// workspace renders). No private submit, no fence wait, no stall.
    void updateRayQuery(const std::vector<OgreScene *> &drawn);
    /// Drops every acceleration structure and the tier's device objects. MUST
    /// run while the VkDevice is still alive — i.e. before Root is deleted, on
    /// the same rule as every MeshPtr the engine holds.
    void shutdownRayQuery();
    /// Null until the first frame that wants the tier (and on every device
    /// without VK_KHR_ray_query, where it stays null forever). A RAW pointer
    /// on purpose: the type is incomplete in every TU but OgreRayQuery.cpp, and
    /// a unique_ptr would need it complete wherever ~OgreEngine is compiled.
    /// shutdownRayQuery() is the one owner-side delete.
    RayQueryTier *mRayTier = nullptr;
    /// The no-rays switch (EngineConfig::rayTracing, Engine::setRayTracing).
    bool mRayTracingWanted = true;

    // ---- VR state (all four RAW: the types are incomplete everywhere but
    //      OgreVrSession.cpp, and a unique_ptr would need them complete
    //      wherever ~OgreEngine is compiled — the mRayTier rule) ------------
    VrBoot    *mVrBoot = nullptr;      ///< the instance + device; engine lifetime
    VrSession *mVrSession = nullptr;   ///< the session; user lifetime
    VrInfo     mVrInfo;
    /// Has a window consumed `external_device` yet? Only the first one can.
    bool       mVrDeviceConsumed = false;
    /// The RUNTIME made the instance but refused (or could not make) the
    /// device. The boot object must stay alive — Ogre is running on its
    /// VkInstance — but no session can ever be created on it.
    bool       mVrDeviceFailed = false;
    /// The host's mirror wish, remembered across sessions (Engine::
    /// setVrMirrorView may be called before one exists).
    OgreView  *mVrMirrorView = nullptr;
    /// THE INJECTED HAND SAMPLES (Engine::vrInjectInput) and the ray the host
    /// last pushed (Engine::setVrRay). Both are plain state on the engine: they
    /// must answer with no session (the headless test backbone); a session's
    /// BEGIN and END empty the injected samples (a stale injection never
    /// reaches a wearer) — the ray persists.
    VrHandState mVrInject[VrHandCount];
    bool        mVrInjected[VrHandCount] = { false, false };
    /// THE INJECTED SESSION FOCUS (Engine::vrInjectFocus) — one bit for the
    /// process, because focus is the session's and not a hand's. True by
    /// default and reset with the store.
    bool        mVrInjectFocus = true;
    /// THE INJECTED SKELETONS (Engine::vrInjectJoints, stage 3), beside the
    /// samples and cleared with them. Two hands' worth of joints is 1.7 kB of
    /// engine state that only a test ever writes — it is here rather than on
    /// `VrStatus` precisely so that no host pays for it per frame.
    VrPose      mVrInjectJoints[VrHandCount][kVrHandJointCount];
    bool        mVrJointsInjected[VrHandCount] = { false, false };
    VrRayState  mVrRay;
    /// ARE WE INSIDE renderOneFrame? (VR-4-FIX's second read, finding 3.)
    /// Nothing in this tree destroys a scene from inside a frame — and if
    /// anything ever does, tearing it down there would end a VR session between
    /// its own xrBeginFrame and xrEndFrame and free a scene the render system
    /// is holding. So the flag exists and `destroyScene` DEFERS to the list
    /// below instead (VR-INPUT-1E-FIX finding 5: the old shape refused, which
    /// left the host holding a pointer to a scene it believed gone).
    bool        mInRenderFrame = false;
    /// THE SCENES A FRAME ASKED TO DESTROY, drained at that frame's tail — in
    /// the order they were asked for, and once: a scene named twice is
    /// destroyed on its first turn and reports "unknown Scene" on the second,
    /// which is what a double destroy is.
    std::vector<Scene *> mPendingSceneDestroy;
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
    /// The focused-map count the scenes about to be drawn WANT right now:
    /// stepped {2,4,8,16} from their shadow-casting point/spot lights and
    /// clamped to effectiveShadowMapBudget(). No debounce — the raw demand, so
    /// the clear-strategy flip can size the atlas in the SAME rebuild instead
    /// of leaving a second one to the derivation. It also owns the over-budget
    /// warning's bookkeeping, so both growth paths keep it (F3). `castersOut`
    /// receives the caster count the answer was derived from.
    unsigned shadowMapDemand(unsigned *castersOut = nullptr);
    /// The one log line either growth path writes.
    void logShadowAtlasGrowth(unsigned want, unsigned casters, const char *why);
    /// Has any ENABLED view of a scene this frame draws put pixels on its target
    /// yet (View::framesPresented)? The gate deriveShadowMapCount shares with
    /// the clear-strategy flip: neither may rebuild the atlas while a world is
    /// still BINDING. Deliberately independent of lamps and shadow nodes — the
    /// flip's own `presenting` is computed inside its lamp scan, and reusing
    /// that would have stalled the GROWTH for ever in a scene whose casters are
    /// not cacheable. (F1, round 2.)
    bool anyDrawnViewPresented();
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
    // ---- Photon shared infrastructure (OgreCompute.cpp) ----
    bool gpuCull(Scene *scene, View *view, const GpuCullRequest &request, bool readBack,
                 GpuCullResult &out) override;
    bool fillCullView(View *view, GpuCullRequest &out) const override;
    bool hzbStatus(View *view, HzbStatus &out) const override;
    bool readHzbLevel(View *view, unsigned level, std::vector<float> &out,
                      unsigned &width, unsigned &height) override;
    // ---- The render-loop monitor (OgreFrameMonitor.cpp) ----
    void setFrameMonitor(MonitorLevel level) override;
    MonitorLevel frameMonitor() const override;
    MonitorStatus monitorStatus() const override;
    unsigned takeFrameRecords(std::vector<FrameRecord> &out) override;
    unsigned takeMonitorEvents(std::vector<MonitorEvent> &out) override;
    void noteMonitorEvent(const MonitorEvent &event) override;
    void noteHostStage(const std::string &name, float ms) override;
    void setNextFrameCause(FrameCause cause) override;
    void setNextFramePace(FramePace pace) override;
    bool framePaceOwesWork() const override;
    StreamingWork streamingWork() const override;
    bool captureSnapshot(EngineSnapshot &out, const std::string &label,
                         Scene *scene = nullptr) const override;
    /// Attaches (or removes) the monitor's pass listener on every live
    /// workspace this engine can reach — each view's through its seam, each
    /// planar slot's and each reflection probe's directly. Run once a frame
    /// while the monitor is on, in the same place and for the same reason as
    /// the shadow counters' re-attach (applyShadowCacheDirties): workspaces are
    /// recreated by atlas rebuilds, GI rebuilds and probe placement, and a
    /// listener list dies with its workspace.
    void syncMonitorListeners(const std::vector<OgreScene *> &drawn);
    /// Fills the GPU half of MonitorStatus: whether the engine was BUILT with
    /// timestamp support (JAH_GPU_TIMESTAMPS in the Ogre patch), whether the
    /// device can do it, and whether a query pool exists right now. Both
    /// off-switches are visible here (owner decision D3).
    void gpuTimingStatus(MonitorStatus &st) const;
    /// The frame's GPU bookkeeping (P1c): rotate the query pools, read back the
    /// samples of the frame two frames ago and reset the pool about to be
    /// written. Must run at the TOP of the frame, outside every encoder. A
    /// no-op with the monitor off or without ogre-patch 0027.
    void gpuFrameBegin();
    /// Builds the compositor-graph half of a snapshot (every live workspace,
    /// its nodes and passes, and which scene each renders).
    void collectCompositorGraph(std::vector<CompositorWorkspaceInfo> &out) const;
    bool saveShaderCache() override;
    bool flushShaderCache(unsigned budgetMs) override;
    bool clearShaderCache() override;
    void shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                             unsigned &expected) const override;

    /// THE PROCESS-WIDE PROBE BINDING JUST CHANGED, so every scene has to
    /// re-decide what its datablocks hold in the env-probe slot (lane
    /// SKY-FALLBACK-1). HlmsPbs is a singleton and its
    /// `parallax_correct_cubemaps` property is set for EVERY scene's pass while
    /// any PCC is bound, so the question "may this material carry a manual
    /// cubemap" is a process-wide one — see OgreScene::reflectionTexFor. Called
    /// from the sites that bind or unbind a grid, which live in OgreScene.
    void reapplyReflectionsAllScenes();

    ~OgreEngine() override;

private:
#ifdef __linux__
    /// The exact layout Ogre's Vulkan/XCB backend reads out of the "SDL2x11"
    /// misc param (`struct SDLx11 { Display *display; ::Window window; }`,
    /// OgreVulkanXcbWindow.cpp) — spelled without Xlib so this header stays
    /// X11-free. `Display*` is an opaque pointer and `Window` is `XID` =
    /// `unsigned long`; same sizes, same alignments, same order.
    struct X11Handle { void *display = nullptr; unsigned long window = 0; };
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
    /// THE BLANK SCENE MANAGER (lane STALE-VIEW-1): the empty world every
    /// scene-less View's clear-only workspace runs against. One per process and
    /// created on demand, in the shape mDocumentScene is created in and for the
    /// same reason — a compositor workspace needs a SceneManager and a camera,
    /// and nothing in here is ever culled, lit or drawn. @see blankSceneManager.
    Ogre::SceneManager *mBlankScene = nullptr;
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
    /// Set each frame by applyShadowCache while the clear strategy is still
    /// upstream's: a drawn, PRESENTING scene holds a cacheable lamp, so the
    /// per-map-clear flip WILL happen — it is only waiting out its debounce.
    /// deriveShadowMapCount reads it so that a rebuild it has to do anyway
    /// carries the flip with it: one atlas rebuild for one event, instead of a
    /// growth now and a flip three frames later, each dropping and recreating
    /// every workspace that names a shadow node (the "first-lamp atlas hitch",
    /// E2 review / ledger 104/121). One frame stale by construction — the
    /// derivation runs first in the frame — which costs nothing: it can only
    /// be true one frame later than it might have been.
    bool            mShadowClearFlipWanted = false;
    /// Cumulative atlas rebuilds — ShadowStatus::atlasRebuilds.
    unsigned        mShadowAtlasRebuilds = 0;
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
    /// THE SAME DEBOUNCE FOR THE CLEAR-STRATEGY FLIP, and it is not symmetry
    /// for its own sake — see applyShadowCache. Counted only in frames where
    /// the scenes that want it are actually PRESENTING, so the drop-and-
    /// recreate of every workspace naming a shadow node cannot land inside a
    /// world's first bind.
    unsigned        mShadowClearFlipFrames = 0;
    static constexpr unsigned kShadowClearFlipDebounceFrames = 3u;
    /// THE PASS COUNTERS' READINGS for the last rendered frame (P8), latched by
    /// latchShadowCounters. View kind = the first enabled view with a shadow
    /// node (the "one view speaks for the process" rule); reflect and probe
    /// kinds = every planar slot and every shadowed probe of every scene.
    /// `Lamp` = passes of focused (point/spot) maps; CachedMapRenders = the
    /// view's passes spent re-rendering CACHED lamp maps — zero at rest, which
    /// is the whole point, measurably.
    unsigned        mShadowPassesLastFrame = 0;
    unsigned        mCachedMapRendersLastFrame = 0;
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
    /// Were the counters actually ARMED for the last frame that rendered? It is
    /// not the same question as `mShadowPolled`: arming happens inside
    /// applyShadowCacheDirties, so a read taken before any frame has rendered
    /// under the listeners must report "not measured" rather than a zero that
    /// looks like a measurement (ShadowStatus::countersMeasured).
    bool            mShadowCountersArmed = false;
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
    /// THE DRAIN'S ADVANCE CADENCE, in ms (lane ENGINE-SMALL-A / DRAIN-1, audit
    /// ON-17). The drain polls every 1 ms and used to call
    /// `VaoManager::_update()` on EVERY poll — and a bare `_update` outside a
    /// frame commits a command buffer with a fence at the TOP of the call
    /// whenever the previous one left the fence unflushed
    /// (OgreVulkanVaoManager.cpp, the pin's issue #433), so a drain that waits
    /// a second for a scene's textures submitted ~1,000 empty command buffers,
    /// advanced the descriptor pools ~1,000 times and reset the BarrierSolver
    /// as often. One advance per FRAME's worth of time does the same work per
    /// tick — the commit, the frame-index advance, the staging/semaphore/
    /// delayed-block retires — 16 times more cheaply, and the drain's progress
    /// never depended on the rate (it depends on the decode workers, which run
    /// on their own threads; `TextureGpuManager::_update(true)` is still called
    /// on every poll, exactly as upstream's own wait does).
    ///
    /// JAH_TEXTURE_DRAIN_ADVANCE_MS overrides it for the A/B (0 = advance on
    /// every poll, i.e. the pre-DRAIN-1 behaviour) — the same shape as every
    /// other measurable rule in this engine.
    double          mTextureDrainAdvanceMs = 16.0;
    /// How many times the drain advanced the VaoManager, this process
    /// (`Engine::textureWaitAdvances`, `app.textureStreaming().waitAdvances`).
    /// Monotonic; the guard `threading.texture_wait_watchdog` asserts it against
    /// the cadence over a known 300 ms stuck drain.
    unsigned long long mTextureWaitAdvances = 0ull;
    /// How many times advanceResources() has run (RenderStats::resourceAdvances).
    unsigned long long mResourceAdvances = 0ull;

    /// Drains the texture streaming queues, bounded. Returns true when the
    /// queues really did empty; false when the no-progress budget expired, in
    /// which case it has already logged the pending textures by name.
    bool drainTextureStreaming(double *msSpent = nullptr);
    /// One collection pass with no wait — a streaming frame's half of the drain
    /// (OPEN_COVER_SPEC §2 E).
    void collectTextureStreaming();
    Ogre::AbiCookie mAbiCookie{};
    std::string     mBackendName, mMediaDir;
    /// MUTABLE because the const readbacks (shadowStatus) report backend
    /// failures through the same channel as everything else: a status call that
    /// threw must not look like a status call that found nothing.
    mutable std::string mLastError;
    ShaderCache     mShaderCache;
    std::vector<std::unique_ptr<OgreScene>> mScenes;
    /// THE SHADOW CACHE'S PER-FRAME SCRATCH (clean-2 lane, 2026-09-13).
    /// applyShadowCache and applyShadowCacheDirties run every frame for ever
    /// and used to build five containers per call (plus one per shadow-node
    /// instance): a still scene allocated and freed them at 60 Hz. Cleared and
    /// refilled instead, so a steady frame allocates nothing here.
    /// One shadow-node instance about to be updated, with the position of the
    /// camera that will update it: a view's editor camera, a mirror slot's
    /// reflected camera, a probe's capture camera (fixed at the probe centre).
    /// `hasEye` is false when the workspace has no default camera.
    struct ShadowInstance {
        Ogre::CompositorShadowNode *node = nullptr;
        ShadowNodeKind              kind = ShadowNodeKind::View;
        Ogre::Vector3               eye;
        bool                        hasEye = false;
    };
    std::vector<ShadowInstance>              mShadowInstScratch;
    std::vector<Ogre::Light *>               mShadowWantScratch;
    /// (squared distance, index into `want`) for the nearest-lamp selection a
    /// probe instance makes when the scene has more lamps than it has maps.
    std::vector<std::pair<float, size_t>>    mShadowNearScratch;
    std::vector<Ogre::Light *>               mShadowPlanScratch;
    std::vector<char>                        mShadowPlacedScratch;
    std::vector<Ogre::CompositorWorkspace *> mShadowWsScratch;
    /// THE PROBE KIND'S CACHE-WORK RECORDS, COALESCED PER FRAME (clean-2 lane,
    /// 2026-09-13). A shadowed probe grid holds one shadow-node INSTANCE per
    /// probe, so one moving lamp used to write 35 identical "reason: light"
    /// records into a capture's frame (1 view + 2 reflect + 32 probe), 32 of
    /// them for instances that will not render this frame at all. One record
    /// per lamp per frame instead, with `units` = the instances marked.
    struct ProbeMark { unsigned long long node = 0; WorkReason reason = WorkReason::None;
                       unsigned instances = 0; };
    std::vector<ProbeMark> mShadowProbeMarks;
    void noteShadowMapWork(ShadowNodeKind kind, WorkReason reason, unsigned long long node,
                           const char *kindName);
    void flushShadowProbeMarks();
    std::vector<OgreScene *>                 mShadowSceneScratch;
    OgreScene::ShadowCacheFrame              mShadowFrameScratch;
    std::vector<std::unique_ptr<OgreView>>  mViews;
    /// THE RENDER-LOOP MONITOR (OgreFrameMonitor.cpp). Held by pointer and
    /// NULL while the monitor is off — that is what "zero cost when off" means
    /// structurally: nothing to allocate, nothing to read, and every
    /// instrumentation site in every TU is one `if (monitor::live())` test.
    std::unique_ptr<monitor::FrameMonitor> mMonitor;
    /// Set by the host for the NEXT frame only (Engine::setNextFrameCause).
    FrameCause mNextFrameCause = FrameCause::Driver;
    /// What the next frame may put off (OPEN_COVER_SPEC §2.1). `Complete` is
    /// the default and the value every frame is reset to, so only a host that
    /// asks per frame ever gets anything else.
    FramePace  mNextFramePace = FramePace::Complete;

    /// XID-2: latched the first frame the render system reports a lost device.
    bool mDeviceLost = false;
    /// THE INJECTED FRAME FAULT (Engine::setFrameFault) — test-facing, off in
    /// every shipping path, and the only thing in the engine that can make a
    /// frame throw on purpose. `mFrameFaultDeviceLost` is the second half of
    /// `FrameFault::ThrowDeviceLost`: the one way the latch below can fire
    /// without a real device loss, cleared with the fault.
    FrameFault mFrameFault = FrameFault::None;
    unsigned   mFrameFaultLeft = 0u;
    bool       mFrameFaultDeviceLost = false;
    /// WHAT STOPPING A CAPTURE LEAVES BEHIND. Switching the monitor off flushes
    /// the frames still waiting for their GPU samples (which arrive two frames
    /// late) into here, so the host's usual "stop, then drain" order does not
    /// silently lose the tail of every capture. The NEXT takeFrameRecords moves
    /// them out and frees the storage; until then MonitorStatus reports them as
    /// `ringFrames` with a `ringCapacity` of 0, because the ring itself is
    /// gone.
    std::vector<FrameRecord> mFinalRecords;
};

}  // namespace detail
}}  // namespace jahshaka::engine
