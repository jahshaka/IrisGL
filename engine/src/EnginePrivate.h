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
#include "jahshaka/engine/EngineInspect.h"
// THE GPU SCENE's tables (A3 slice). Engine-private and Ogre-aware — it holds
// MeshPtrs and hands out UavBufferPackeds — but it knows nothing of Vulkan and
// nothing of OgreScene's Node, which is what keeps it bindable from an
// HlmsComputeJob and buildable on a platform with no ray queries.
#include "AlphaMasks.h"   // the cut-outs' masks (ENGINE-POLISH-ATOM-1 stage 3)
#include "GpuCull.h"
#include "GpuSceneSubstrate.h"   // the tables' one writer (GpuScene.h inside it)
#include "photon/PhotonComponent.h"
#include "atom/AtomComponent.h"
#include "jahshaka/engine/GpuSubstrate.h"
#include "GpuVoxelGather.h"
#include "SceneLightList.h"
#include "RenderThread.h"   // the render loop the frame belongs to (ENGINE-UPGRADE-U3)

#include <cmath>
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
#include <OgreHlmsDiskCache.h>
#include <OgreGpuProgramManager.h>
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
#include <Compositor/OgreCompositorWorkspaceListener.h>
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
#include "photon/voxel/PhotonVoxelizer.h"
#include "photon/voxel/PhotonVoxelLighting.h"
#include "photon/voxel/PhotonIrradianceField.h"
#include <Cubemaps/OgreParallaxCorrectedCubemapAuto.h>
#include <Cubemaps/OgrePccPerPixelGridPlacement.h>
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
#include <array>
#include <cstdint>
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
namespace Ogre { class HlmsCompute; class PhotonVoxelMaterial; struct PhotonVoxelSideTrace;   // A5b §2: the one store a chain shares
                 class CompositorPassSceneDef; class CompositorPassClearDef;
                 class CompositorPassQuadDef; class CompositorPassDef;
                 class CompositorPassCompute;   // the no-ray screen integrate's listener (PHOTON-7 7-6)
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
/// own sense: a class built on the public Ogre API, the shape `PhotonVoxelLighting` and
/// `IrradianceField` are built in, which a future consumer outside the backend
/// may hold. Only OgreSurfaceCache.cpp and the scene's own TU need its
/// definition.
class SurfaceCache;
struct CardMoverTrace;
struct CardGatherTrace;
enum class CardGatherResult : unsigned;
struct CardMoverFrame;
struct CardSceneView;
// The backend's own namespace: these types and helpers are shared between the
// TUs under engine/src and by nothing else (they used to live in one anonymous
// namespace, when the backend was a single translation unit).
namespace detail {

inline Ogre::Vector3     toOgre(const Vec3 &v)   { return Ogre::Vector3(v.x, v.y, v.z); }
inline Ogre::ColourValue toOgre(const Colour &c) { return Ogre::ColourValue(c.r, c.g, c.b, c.a); }
/// The exact sRGB OETF on the CPU (SRGB-ENCODE-1): the same curve as
/// jahSrgbEncode in Jahshaka media JahSrgb.glsl, for a clear colour that
/// reaches a display target without passing through a shader. Alpha untouched.
inline float srgbEncode(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}
inline Colour srgbEncode(const Colour &c) {
    return Colour(srgbEncode(c.r), srgbEncode(c.g), srgbEncode(c.b), c.a);
}

/// The LOD switch band a WATCHED view's scene passes carry, and the suite's
/// offscreen latch (OgreMesh.cpp; fork 5230c9390+8282f6d70 (was 0075)). @see ChainDesc::lodHysteresis.
float jahLodHysteresis();

/// ATOM stage 1's VIEW rule (OgreMesh.cpp): registers `jah_world_error` — the
/// LOD strategy whose per-object value is the world-space error the pass's own
/// camera and render target can hide — and makes it the process default. The
/// switch BAND fork 5230c9390+8282f6d70 (was 0075) adds to `LodStrategy::lodSet` is per PASS and
/// comes from the chain (`ChainDesc::lodHysteresis`), not from here. Called
/// once, after Root::initialise and before any mesh or Item exists.
void installJahLodStrategy();

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
/// the crash fork d6348decd (was 0035) now catches at the disk-cache save (lane
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

/// WHAT A HOST READS WHEN OGRE THREW (lane FORK-OOM-1, 2026-09-27). Ogre's own
/// full description, with ONE case said first in plain words: a Vulkan
/// out-of-memory. The Vulkan render system throws every failed call as
/// ERR_RENDERINGAPI_ERROR with the VkResult as the exception's number
/// (`onVulkanFailure`), and since the fork's "OOM is not a device loss" commit
/// an OOM no longer latches the device: the engine carries on, and the error a
/// suite or a user reads must say what happened — the GPU refused an
/// allocation (the pool's size is in Ogre's text) — not leave them to find
/// "VK_ERROR_OUT_OF_DEVICE_MEMORY" at the end of a paragraph. The two numbers
/// are Vulkan's own (VkResult, core since 1.0); this header does not include
/// Vulkan.
/// THE ONE REACH RULE for every Photon ray that reads the voxel cascades
/// (D4-PHOTON-TIERS — the reflections' rule, adopted by the screen-probe gather,
/// which had its own: half the outer cascade's smallest side). A ray must be
/// long enough to cross the lit volume it will be shaded from — the OUTERMOST
/// cascade's full DIAGONAL, floored at 50 m for a chain too small to reach past
/// a room — and bounded by the camera's own far plane (an infinite one, 0, reads
/// as 1 km) so an open scene's ray reaches the sky rather than marching the
/// world. What lies beyond it is the far query's (the coarse copies), never a
/// second definition of the near one.
inline float photonRayReach(float outerCascadeDiagonal, float farClip) {
    const float far = farClip > 0.0f ? farClip : 1000.0f;
    return std::min(far, std::max(outerCascadeDiagonal, 50.0f));
}

inline bool isVulkanOutOfMemory(const Ogre::Exception &e) {
    return dynamic_cast<const Ogre::RenderingAPIException *>(&e) &&
           (e.getNumber() == -1 || e.getNumber() == -2);   // VK_ERROR_OUT_OF_{HOST,DEVICE}_MEMORY
}

inline std::string describeOgreFailure(const Ogre::Exception &e) {
    constexpr int kVkErrorOutOfHostMemory = -1;     // VK_ERROR_OUT_OF_HOST_MEMORY
    constexpr int kVkErrorOutOfDeviceMemory = -2;   // VK_ERROR_OUT_OF_DEVICE_MEMORY
    if (dynamic_cast<const Ogre::RenderingAPIException *>(&e)) {
        if (e.getNumber() == kVkErrorOutOfDeviceMemory)
            return "GPU out of memory (VK_ERROR_OUT_OF_DEVICE_MEMORY; the device is NOT lost): " +
                   e.getFullDescription();
        if (e.getNumber() == kVkErrorOutOfHostMemory)
            return "host out of memory in the GPU driver (VK_ERROR_OUT_OF_HOST_MEMORY; the device "
                   "is NOT lost): " + e.getFullDescription();
    }
    return e.getFullDescription();
}

// Every backend virtual is wrapped: `JAH_TRY { ... } JAH_CATCH(errSink, failValue)`.
// Ogre throws Ogre::Exception; its own allocations may throw std::bad_alloc.
#define JAH_TRY try
#define JAH_CATCH(sink, ret)                                                          \
    catch (Ogre::Exception &e) { (sink) = ::jahshaka::engine::detail::describeOgreFailure(e); return ret; } \
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
/// The placeholder draws / pending skips made inside the cache's own captures (ASYNC-SHADERS-1).
/// Kept per SceneManager across the cache's rebuilds; `forget` drops the entry (scene teardown).
/// How many views compile in the background right now (OgreView::setAsyncShaders); the shader
/// cache's counter names any compile made off the background compiler while it is non-zero.
int asyncViewCount();
void noteAsyncView(bool on);
void surfaceCacheAsyncDraws(const Ogre::SceneManager *sm, unsigned long long &placeholders,
                            unsigned long long &skips, bool forget = false);

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
    /// THE OPAQUE SCREEN DECODE PASS (ATOM-DECODE-CLASS-1) shades with the prepass
    /// too, in front of the opaque pass: the ray jobs finish in front of IT, and its
    /// pass-scoped registrations stand until the opaque pass is over — one window
    /// for the two passes that read them.
    bool        mRayJobsFinished = false;
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
// one of them for free. That is exactly what the editor's 4 km GROUND PLANE
// widget wants (nothing that size may size a shadow atlas or a voxel volume) — but
// the plane is PART OF THE PICTURE, not editor furniture, and the moment one view
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

// THE PHOTON VIEW'S CHANNEL (PHOTON-VIEW-1). The NINTH bit, and like
// kCardSubjectBit not an inversion: the only objects that ever carry it are the
// photon view's debug drawables (PhotonVoxelVisualizer and PhotonIfdProbeVisualizer,
// the cards' quads), and they carry it ALONE.
//
// WHAT IT BUYS: those objects live in the scene's own SceneManager (Ogre's
// debug visualizers attach themselves there), so without a channel of their own
// every pass of every workspace of that manager would draw them — the prepass
// (the G-buffers every ray job reads), the opaque pass (the HDR target every
// history and the exposure meter read) and the thumbnails. With it:
//   * OUT of every capture with no rule: the probe faces and the sky capture
//     (`visibility_mask 0x1`), the shadow nodes (shadowCasterChannels, and the
//     drawables cast nothing), the planar allowlist, the card capture (the
//     subject bit) — none of them asks for this bit.
//   * OUT of every view chain's scene passes, which are born holding every
//     RESERVED bit: chain::helperBitsToDrop drops it from every view, always.
//   * IN exactly one pass: the chain's photon pass (after the post chain,
//     execution-gated by kPhotonSceneExecutionBit), whose mask is this bit alone.
constexpr Ogre::uint32 kPhotonViewBit  = 1u << 11;

// THE STOCK CASTER CHANNEL (ENGINE-POLISH-ATOM-1 stage 2). Bits 12 and 13 (the THIRTEENTH and FOURTEENTH), ADDED beside an item's world
// channel (never instead of it): kStockCasterBit beside kVisibleBit, kStockMoverCasterBit beside kMovableBit
// (stockCasterBits). The shadow nodes' SCENE passes — the stock PBS caster — ask for THESE (shadowPassMask), never for
// the world channels themselves, so an item whose caster the caster cut draws (kGpuAtomCaster: a blended item whose
// colour stays on PBS) drops the pair and no stock shadow pass draws it a second time, while every other reader of
// kVisibleBit / kMovableBit — the probe faces, the planar allowlist, the GI gathers, the GPU scene's flags, the caster
// scans and the shadow node's casters box (Ogre's LAYER_SHADOW_CASTER under the viewport's mask, which sizes the
// splits) — goes on seeing it exactly as before. Ogre's visibility test is ANY-BIT-SET: a pass cannot EXCLUDE a bit,
// so the exclusion is this include channel that the cut-routed item does not carry (the inversion idea of the bits
// above, applied to one pass family only). The pair, not one bit: the PROBE kind's maps draw the still world only.
// SET by itemVisibilityFlags / the billboard branch (every world-channel item starts stock) and taken away by the GPU
// scene's compose tail (OgreScene::placeAtomCaster) for kGpuAtomCaster items, the way placeAtomQueue files the queue.
constexpr Ogre::uint32 kStockCasterBit      = 1u << 12;
constexpr Ogre::uint32 kStockMoverCasterBit = 1u << 13;
constexpr Ogre::uint32 kStockCasterBits     = kStockCasterBit | kStockMoverCasterBit;
/// The stock-caster pair that goes with an item's world channels.
inline Ogre::uint32 stockCasterBits(Ogre::uint32 channels) {
    return ((channels & kVisibleBit) ? kStockCasterBit : 0u) | ((channels & kMovableBit) ? kStockMoverCasterBit : 0u);
}

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
constexpr float        kPssmConstantBiasScale = 0.3f;   ///< the sun's constant bias x 0.3 (OgreShadow.cpp's table)
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
/// THE SHADOW NODE'S SCENE PASSES' MASK, per kind: the stock-caster channel of shadowCasterChannels(kind)
/// (kStockCasterBit's note) — the casters the STOCK caster draws; the caster cut draws the kGpuAtomCaster rest.
/// Within RESERVED_VISIBILITY_FLAGS by construction (trap 6).
inline Ogre::uint32 shadowPassMask(ShadowNodeKind kind) {
    return stockCasterBits(shadowCasterChannels(kind)) & Ogre::VisibilityFlags::RESERVED_VISIBILITY_FLAGS;
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
//   1     the screen decode's classifier (HlmsAtom, kScreenClassifyRenderQueue)
//   2     the visibility buffer's screen decode draws (HlmsAtom, kScreenDecodeRenderQueue)
//   10    normal items           (Ogre's default)
//   11    the visibility buffer's items (kAtomRenderQueue, ATOM S3-DRAW)
//   13    the editor grid (OgreGrid.cpp)
//   14    blended (alpha) surfaces (kBlendedRenderQueue, REFLECT-FIX-1)
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
/// BLENDED (alpha) surfaces (REFLECT-FIX-1): after the opaque queues, the selection
/// outline (10, a helper channel) and the grid (13, OgreGrid.cpp), before the particles
/// (15) — so every depth-tested helper is seen through a pane of glass, on every tier.
/// v2 FAST (inside [0,100)).
constexpr Ogre::uint8 kBlendedRenderQueue    = 14;


// THE VISIBILITY BUFFER'S QUEUE (ATOM S3-DRAW; SPECS/atom/D3_S3_DRAW_DESIGN.md §2.4).
// The render-queue split's ONE switch (OgreScene::atomRouteFor, at compose time)
// files an item the id pass can draw and the decode can shade here; everything else
// stays at 10. A view whose chain carries the id pass SKIPS this queue in its
// prepass and opaque pass (the fork's CompositorPassSceneDef::mSkipRQ) — the id pass
// draws these items and the decode shades them — while every other pass keeps
// drawing them through PBS exactly as before: the shadow node, the PCC faces and the
// cards ([0,200)), the planar mirrors ([0,199)), and any view without an id pass
// (a stereo chain). 11 because it must sit inside all of those ranges, below 200,
// and in front of the blended queues it would otherwise overwrite in a PBS pass: the
// grid at 14 and the particles at 15 draw after it, as after the items at 10.
constexpr Ogre::uint8 kAtomRenderQueue = 11;
/// The customId of the id pass (AtomPassProvider's registry, OgreAtomIdPass.cpp).
constexpr const char *kAtomIdPassId = "atom_id";
/// ...and of the LATE id pass, the disocclusion half of the two-pass occlusion
/// (ChainDesc::atomOcclusion): the same recorder with the rejected set.
constexpr const char *kAtomIdLatePassId = "atom_id_late";
/// ...and of THE HISTORY'S REBUILD (ATOM-8, R6): after the late pass (and the merge), the occlusion's pyramid built
/// again from the scene's depth - the first pass's surfaces, the stock ones AND the late pass's - so the next frame's
/// first cull tests against everything this frame drew (OgreAtomIdPass.cpp recordHistoryPass).
constexpr const char *kAtomHistoryPassId = "atom_history";
/// ...and of THE CASTER CUT (ATOM-SHADOWS-1, OgreAtomCasterPass.cpp): the PASS_CUSTOM that
/// follows every caster scene pass of a shadow node on a device that runs the id pass
/// (OgreShadow.cpp) and draws the map's Atom casters from the light's own cluster cut.
constexpr const char *kAtomCasterPassId = "atom_caster";
/// The chain's id image (R32G32_UINT, AtomId's words): what the id pass writes and
/// the screen decode reads (OgreChain.cpp defines it; the listener finds it by name).
constexpr const char *kAtomIdTexture = "jahAtomIds";
/// THE ATOM VIEW (D0-ATOM-VIEW, Types.h AtomView). Its two passes — the copy of
/// the id pass's depth and the false-colour quad after the post chain — carry THIS
/// execution-mask bit and nothing else, and the view's atom listener clears it from
/// the workspace's mask every frame the scene's view is Off: switching the view
/// never rebuilds the workspace, and Off executes neither pass. Every other pass of
/// every chain keeps Ogre's default masks (0xFF, a clear pass 0x01), which the
/// workspace's remaining bits still intersect.
constexpr Ogre::uint8 kAtomViewExecutionBit = 0x80u;
/// The quad's material (engine media, Hlms/Jahshaka/JahAtomView.material).
constexpr const char *kAtomViewMaterial = "Jahshaka/AtomView";
/// THE STOCK DEPTH BETWEEN THE ID PASSES, PER FRAME (ATOM-4 stage 1b; OgreChain.cpp addAtomOcclusionPasses): on a
/// chain with the occlusion, the view's atom listener sets exactly one of the two every frame — kAtomStockExecutionBit
/// while a stock slot's world box reaches the view's camera (the stock depth pass, the late id pass on the Atom view's
/// depth and the merge run), kAtomNoStockExecutionBit otherwise (the late id pass on the scene's depth, today's
/// frame: no stock occluder, nothing to draw). The low bits (0x01: a clear pass's default) stay everyone's.
constexpr Ogre::uint8 kAtomStockExecutionBit   = 0x04u;
constexpr Ogre::uint8 kAtomNoStockExecutionBit = 0x02u;
/// THE PHOTON VIEW (PHOTON-VIEW-1, Types.h PhotonView): two gated stages after the
/// post chain, before the overlays — a depth copy, a PASS_SCENE drawing the
/// kPhotonViewBit drawables (Voxels, Probes, Cards) into the view's photon layer and
/// the layer's composite, on kPhotonSceneExecutionBit; and a PASS_QUAD compositing
/// the ray tier's overlay (ScreenProbes, Hits) on kPhotonOverlayExecutionBit. The view's photon listener sets exactly the bits the
/// scene's view needs, every frame; Off executes none of them.
constexpr Ogre::uint8 kPhotonSceneExecutionBit   = 0x40u;
constexpr Ogre::uint8 kPhotonOverlayExecutionBit = 0x20u;
/// ...and how the layer's depth is prepared: the scene's copied (Cards) or cleared
/// (Voxels, Probes — a world of their own).
constexpr Ogre::uint8 kPhotonDepthCopyBit  = 0x10u;
constexpr Ogre::uint8 kPhotonDepthClearBit = 0x08u;
/// Every photon bit, for the workspace's initial mask. (A PASS_CLEAR's default
/// mask is 0x01 and every other pass's 0xFF, so the low bits stay everyone's.)
constexpr Ogre::uint8 kPhotonExecutionBits = kPhotonSceneExecutionBit | kPhotonOverlayExecutionBit |
                                             kPhotonDepthCopyBit | kPhotonDepthClearBit;
/// The two composites' materials (engine media, Hlms/Jahshaka/JahPhotonView.material):
/// the scene pass' layer, and the tier's overlay.
constexpr const char *kPhotonLayerMaterial   = "Jahshaka/PhotonLayer";         ///< display-encoded (SRGB-ENCODE-1)
constexpr const char *kPhotonLayerLinearMaterial = "Jahshaka/PhotonLayerLinear"; ///< the Plain instrument's
constexpr const char *kPhotonOverlayMaterial = "Jahshaka/PhotonOverlay";
/// The cards' quads' material (the same file).
constexpr const char *kPhotonCardsMaterial = "Jahshaka/PhotonCards";

/// OgreAtomIdPass.cpp — THE ID PASS's device half. `atomIdPassSupported`: this
/// device can run it (buffer device addresses, VK_KHR_draw_indirect_count, the
/// raw-Vulkan TU compiled in; false on macOS and on the NULL render system).
/// `registerAtomIdPass` puts its recorder on the provider (at Hlms registration);
/// `releaseAtomIdPass` destroys its pipeline (before Root).
bool atomIdPassSupported(Ogre::RenderSystem *rs);
/// OgreCompute.cpp — a cull request's frustum, eye and level-rule terms from a
/// camera and its pass's target height (OgreEngine::fillCullView's view half).
void fillCullFrustum(const Ogre::Camera *cam, float viewportHeight, GpuCullRequest &out);
void registerAtomIdPass();
void releaseAtomIdPass();
/// A VIEW'S ID PASS STATS RING (OgreAtomIdPass.cpp): the view's cull lists' counters read
/// back a frame or two late. The view owns it; its deleter retires it through the device's
/// window (HlmsAtom::passDevice), since a copy into it may still be in flight.
struct AtomIdViewRing;
struct AtomIdViewRingDelete {
    void operator()(AtomIdViewRing *p) const;
};
using AtomIdViewRingPtr = std::unique_ptr<AtomIdViewRing, AtomIdViewRingDelete>;
/// ATOM-2 stage 1 — A VIEW'S DENSITY COUNT AND CUT PROBE (OgreAtomDensity.cpp): the device buffers the 'density'
/// Atom view and Scene::atomCutProbe read, made on first use and freed with the view (Ogre's buffers and textures:
/// their memory waits out the frames in flight in Ogre's own managers).
struct AtomDensityState;
struct AtomDensityStateDelete {
    void operator()(AtomDensityState *p) const;
};
using AtomDensityStatePtr = std::unique_ptr<AtomDensityState, AtomDensityStateDelete>;
/// The texture names the Atom view's quad reads, the chain's (OgreChain.cpp defines them).
constexpr const char *kAtomViewDepthTexture = "jahAtomViewDepth";
constexpr const char *kSceneDepthTexture = "jahDepth";
/// OgreAtomCasterPass.cpp — THE CASTER CUT's device half (ATOM-SHADOWS-1): its recorder
/// on the provider (beside the id pass's), its pipelines destroyed before Root. Its
/// per-scene state is the scene's AtomComponent's (atom/AtomComponent.h).
class OgreScene;
class JahAtmosphere;   // Atmosphere.h (SKY-ATMOSPHERE-1)
void registerAtomCasterPass();
void releaseAtomCasterPass();
/// WHICH SCENE A SCENE MANAGER RENDERS — bound ON the manager (its root scene node's
/// UserObjectBindings, Ogre's own idiom for a user object on an Ogre object), so the
/// binding lives and dies with the manager and no process map holds a pointer that can
/// dangle. A pass, a shadow node or an Hlms callback handed only a SceneManager (views,
/// probes, mirrors and cards alike) finds its scene here; null for a manager no
/// OgreScene owns (the blank world, the document's staging manager). OgreGpuScene.cpp.
void bindSceneManager(Ogre::SceneManager *sm, OgreScene *scene);
OgreScene *sceneOfManager(const Ogre::SceneManager *sm);
/// THE DENSITY COUNT AND THE CUT PROBE, per frame of a view (ATOM-2 stage 1; OgreAtomDensity.cpp). At the head of
/// each of the view's own frames (its atom listener's workspacePreUpdate, never the capture's): the view TAKES the
/// scene's probe arming if there is one, and the frame COUNTS its visible Atom pixels when the scene's Atom view is
/// Density or the probe is this frame's. Returns the texture the Atom view's quad binds at its fourth unit this
/// frame in place of the scene's table (the view's density table), or null for the scene's own; `paint` says the
/// view's two passes (the depth copy and the quad) must execute this frame although the scene's view may be Off
/// (the probe's frame: the quad's mode 0 paints nothing).
Ogre::TextureGpu *atomDensityFrame(OgreView *view, OgreScene *scene, AtomView mode, bool &paint);
/// ...and at the quad's early pre-execute (before its render pass begins): the count over the view's id image and,
/// in the Density mode, the resolve into the view's density table.
void atomDensityRecord(OgreView *view, OgreScene *scene, Ogre::CompositorPass *quad);
/// The view's cut-probe buffer for its first (`late` false) or late list IN THE PROBE'S FRAME, else null
/// (OgreAtomIdPass.cpp hands it to recordGpuCull).
Ogre::UavBufferPacked *atomCutProbeBuffer(OgreView *view, bool late);
/// ATOM-6 stage 3: the probe's frame's HZB record buffer (null outside it) — the test job's `cull_probe`.
Ogre::UavBufferPacked *atomCutProbeHzbBuffer(OgreView *view);
/// ...and the Density mode's figures for AtomDrawStatus: the first of `views` whose last frame counted, read back.
void atomDensityStatus(OgreScene *scene, const std::vector<OgreView *> &views, AtomDrawStatus &out);
/// ...and THE HIDDEN SHARE (ATOM-8 stage 0): the first of `views` holding a read of a counted frame (the Atom view's
/// Triangles or Density mode on) — every triangle its cuts drew, and those of objects with no visible pixel.
bool atomHiddenReading(const std::vector<OgreView *> &views, unsigned long long &drawn, unsigned long long &hidden);
/// The view's DENSITY TABLE (the 'density' mode's per-slot drawn triangles per visible pixel, float bits, 1024 slots
/// a row) — null before the view's first Density frame. A measuring door for the engine suites.
Ogre::TextureGpu *atomDensityTable(OgreView *view);
/// THE OCCLUSION'S PYRAMID (ATOM-OCCLUSION-1, OgreAtomIdPass.cpp): `hzb`'s every level
/// rebuilt from `depth` — ONE dispatch of the single-pass build (JahHzbBuild_cs,
/// farthest), through HlmsCompute with Ogre's barrier solver. False when the job is not
/// staged.
bool recordOcclusionPyramid(Ogre::RenderSystem *rs, Ogre::TextureGpu *depth, Ogre::TextureGpu *hzb);
/// THE SAME BUILD OVER A RECTANGLE of `depth` (a shadow map's in its atlas; ENGINE-UPGRADE-U5 stage
/// 4): `hzb` is the rectangle's size, and `origin` (a 16-byte UAV buffer: x, y, 0, 0) holds its
/// corner in `depth`. The job Jahshaka/HzbBuildRect; false when it is not staged.
/// The matrix a cull tests such a pyramid with, from the pass's draw matrix (its y row negated:
/// OgreAtomIdPass.cpp's cullMatrixOf, where it says why). Vulkan builds only.
Ogre::Matrix4 pyramidCullMatrix(const Ogre::Matrix4 &draw);
bool recordOcclusionPyramidRect(Ogre::RenderSystem *rs, Ogre::TextureGpu *depth, Ogre::TextureGpu *hzb,
                                Ogre::UavBufferPacked *origin);
/// THE SINGLE-PASS BUILD'S JOB (JahHzbBuild_cs): UAV slots 0..kHzbMaxLevels-1 are the
/// pyramid's mips, slot kHzbCounterSlot the engine-lifetime arrival counter, which
/// `hzbBuildJob` creates and binds at its first call and `releaseHzbBuild` frees
/// (~OgreEngine, before Root). Null when the job is not staged.
constexpr Ogre::uint8 kHzbMaxLevels = 16u;
constexpr Ogre::uint8 kHzbCounterSlot = 16u;
Ogre::HlmsComputeJob *hzbBuildJob();
void releaseHzbBuild();
/// OgreAtomDraw.cpp — the view's listener that arms the screen decode for the passes
/// that skip the Atom queue; it rides the view's workspace and its capture's, and so is
/// how a pass of either finds its view (atomViewOfPass: the pass's workspace's
/// listener — null where the view's chain carries no id pass, or for a workspace no
/// view owns).
OgreView *atomViewOfPass(const Ogre::CompositorPass *pass);
/// AN ITEM'S LEVEL-0 TRIANGLES (OgreAtomDraw.cpp): every triangle-list submesh's finest level (VpNormal's first
/// VAO, its own index range), the
/// count the route doors report (Scene::atomDrawStatus's camera view, Scene::shadowCasterRoutes' maps).
unsigned long long itemLevel0Triangles(const Ogre::Item &item);
/// The listener's own deleter: CompositorWorkspaceListener has no virtual destructor.
struct AtomDrawListenerDeleter {
    void operator()(Ogre::CompositorWorkspaceListener *l) const;
};
using AtomDrawListenerPtr = std::unique_ptr<Ogre::CompositorWorkspaceListener, AtomDrawListenerDeleter>;
AtomDrawListenerPtr makeAtomDrawListener(OgreView *view);
/// OgrePhotonView.cpp — THE PHOTON VIEW's per-view listener (PHOTON-VIEW-1).
struct PhotonListenerDeleter {
    void operator()(Ogre::CompositorWorkspaceListener *l) const;
};
using PhotonListenerPtr = std::unique_ptr<Ogre::CompositorWorkspaceListener, PhotonListenerDeleter>;
PhotonListenerPtr makePhotonListener(OgreView *view);

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

/// THE HIT DECODE PASS (ChainDesc::hitDecode): its listener records the ray
/// jobs' TRACES before it (they write the hit list it shades) and arms HlmsAtom's
/// hit mode for its length; the WRITE-BACK and the filters follow in the opaque
/// pass' pre-execute. Stamped on exactly that pass.
constexpr Ogre::uint32 kHitDecodePassIdentifier = 25002u;
/// THE SCREEN DECODE PASSES (ATOM-DECODE-CLASS-1, OgreChain.cpp addAtomDecodePass):
/// the classifier and the bucket draws over the material depth, one in front of the
/// prepass and one in front of the opaque pass. The atom listener arms HlmsAtom for
/// them; the ray tier's finishes its jobs in front of the first one that shades with
/// the prepass. Stamped on exactly those passes.
constexpr Ogre::uint32 kScreenDecodePassIdentifier = 25003u;
/// THE NO-RAY ROUTE'S SCREEN INTEGRATE (PHOTON-7 stage 7, 7-6; ChainDesc::screenIntegrate): the compute pass between
/// the prepass and the opaque passes - its job's parameters and world caches are bound in the view's screen-GI listener
/// (OgreScreenIntegrate.cpp), which also registers the pass's three targets for the opaque passes. Stamped on exactly
/// that pass.
constexpr Ogre::uint32 kScreenIntegratePassIdentifier = 25005u;
/// ...its job and its three targets (the node's names; the pixel binds them as jahProbeIrradiance, jahRoughSpec and
/// jahConeHit through the Hlms listener's registration).
constexpr const char *kScreenIntegrateJob = "Jahshaka/ScreenIntegrate";
constexpr const char *kScreenGiIrradiance = "jahScreenIrradiance";
constexpr const char *kScreenGiSpecular = "jahScreenSpecular";
constexpr const char *kScreenGiConeHit = "jahScreenConeHit";
/// THE NO-RAY ROUTE'S AREA LIGHT VISIBILITY (PHOTON-7 stage 7, 7-6; OgreAreaVis.cpp): its two compute passes right
/// after the screen integrate - the trace (one stratum of each forward LTC light a half-resolution texel a frame, seen
/// through the voxel chain's shadow march) and the resolve (the joint-bilateral upsample into `jahAreaVis`) - whose jobs
/// the view's screen-GI listener binds whole in their early pre-execute (its images exist only while an LTC light is in
/// the forward set). Stamped on exactly those passes.
constexpr Ogre::uint32 kScreenAreaVisPassIdentifier = 25006u;
constexpr Ogre::uint32 kScreenAreaVisResolvePassIdentifier = 25007u;
constexpr const char *kScreenAreaVisJob = "Jahshaka/ScreenAreaVis";
constexpr const char *kScreenAreaVisResolveJob = "Jahshaka/ScreenAreaVisResolve";
/// THE HIT LIST (PHOTON-7 stage 0, the clean form of PHOTON-2's demand-sized list; ChainDesc::hitDecode,
/// OgreView::sizeHitList). Its records are a LIST, not a picture: kHitListWidth records a row (a layout choice, fixed).
/// It is CREATED at kHitListCreateShare x W x H x the eyes records (the old fixed list's capacity for the target, which
/// normal use never overflowed; one stereo view's two eyes append to ONE list, and OgreView::width() is the eye pair's) -
/// so a new view's first frames hold their records - and it only GROWS: a retired frame whose traces appended more
/// records than that frame's list held (the buffer's counter word 0 counts past the capacity) doubles the rows until the
/// list holds that frame's demand. It is never shrunk and its size is in no key (a ray finds the same thing in a list of
/// any size). A record a full list cannot hold is NO SAMPLE for its ray (the reflection's rule; the gather's
/// kAtlasNoSample), never black. The growth re-makes the three list textures in place (Ogre's own window-resize path:
/// re-create, then CompositorNode::notifyRecreated), never the graph: no other history moves.
constexpr unsigned kHitListWidth = 1024u;
constexpr float kHitListCreateShare = 0.5625f;

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
    bool  bloom = false;            ///< the HDR node's mip ladder (chain::bloomLadder; never in stereo yet)
    float bloomThreshold = 5.0f;    ///< the prefilter's knee start, in the tonemapper's units
    float bloomKnee = 2.0f;         ///< ramp WIDTH above it (A-6); 2.0 = the old hard-coded value
    /// HOW MUCH of the blurred highlight the tonemap quad adds (0..2; 1 is the
    /// picture this engine always drew). A UNIFORM, like the exposure and the
    /// threshold next to it, and so deliberately NOT part of sameShape():
    /// scrubbing the amount must not rebuild a compositor graph.
    float bloomAmount = 1.0f;
    /// THE IMAGE BLOCK (IMAGE-1; PostFxDesc::image): uniforms on the tonemap
    /// quad, deliberately NOT part of sameShape().
    ImageGrade image;
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
    /// THE DISPLAY ENCODE (SRGB-ENCODE-1): is this view's picture something a
    /// person LOOKS AT? Then its ungraded composite runs the exact sRGB encode
    /// (Jahshaka/DisplayEncode) and its scene target is float, exactly as the
    /// tonemap quad encodes the graded picture — ONE encode on every display
    /// path. True for every on-screen view and for every offscreen view that
    /// opted into the post chain (PostFxDesc::allowOffscreen: the graded
    /// screenshots, thumbnails, the VR eye pair); FALSE for an offscreen view
    /// that did not, which is the PLAIN grade — the measuring instrument whose
    /// bytes are linear radiance by contract (IEditorViewport::gradeEncoding).
    /// Shape, not a uniform: it adds a target and a quad.
    bool  displayEncode = false;
    int   smaaPreset = -1;          ///< -1 off, 0 Low, 1 Medium, 2 High, 3 Ultra
    int   ssr = 0;                  ///< 0 off, 1 half-res rays, 2 HQ
    /// WHERE THE REFLECTION'S ANSWER COMES FROM (PostFxDesc::reflectSources): Both, Rays or Screen. A GRAPH field:
    /// rasterMarch() and screenSources() below read it (with the stereo shape and the rays) for which passes and
    /// textures exist.
    ReflectSources reflectSources = ReflectSources::Both;
    float ssrMaxDistance = 25.0f;   ///< ray length, world units
    int   ssrSteps = 0;             ///< 0 = the row's 48/96 (PostFxDesc::ssrSteps)
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
    /// THE NO-RAY ROUTE'S SCREEN INTEGRATE (PHOTON-7 stage 7, 7-6; L4 on every chain): a view whose scene's GI is on and
    /// which does NOT gather - no ray device, the project's ray row off, a Live or undeclared offscreen view - carries
    /// the prepass and the compute pass "Jahshaka screen integrate" (Jahshaka/ScreenIntegrate) between it and the
    /// opaque passes, which writes the SAME screen products the gather does - the diffuse (the irradiance field and its
    /// fallbacks, the cones beyond it) and the cone's specular with its hit distance - so the opaque passes bind no world
    /// cache on any chain. GRAPH SHAPE (the opaque passes' range and binding flag, the producer and its targets). Mono
    /// only: a stereo view keeps the pixel's own cones (stage 9's).
    bool  screenIntegrate = false;
    /// The opaque passes read the screen products (a gathering chain or the no-ray producer's).
    bool screenGi() const { return probeGather || screenIntegrate; }
    /// HARD SUN CONTACT SHADOWS (PHOTON P5, RY-R3), here for the gather's
    /// reason exactly: the contact job starts each ray from the PREPASS' depth
    /// and normals and the pixel reads its answer back in the `PrePassUse`
    /// pass (`iFragCoord`, and the prepass' shadow term the answer is folded
    /// into). So the prepass runs for `ssr || probeGather || sunContact`.
    /// Nothing else about the graph moves. Set by the VIEW from the scene's
    /// resolved row, below the offscreen early-out, never in a stereo view.
    bool  sunContact = false;
    /// THE HIT DECODE (PHOTON-HIT-SHADE-1; SPECS/atom/D2_HIT_SHADING_DESIGN.md):
    /// wherever the chain carries the prepass and the scene's rays are live, the
    /// graph carries the ray jobs' compacted HIT LIST — `jahHitIds` (RGBA32UI) and
    /// `jahHitDest` (R32UI), UAVs the traces append to, beside the ray tier's
    /// buffer (the counters and 8 bytes a record: the sun, footprint, weight) —
    /// and one PASS_SCENE, "Jahshaka hit decode" (RQ kHitDecodeRenderQueue only,
    /// into `jahHitRadiance`, RGBA16F), between the SSR resolve and the opaque
    /// pass: HlmsAtom's decode shading every record. THE LIST'S SIZE (kHitListWidth's
    /// law): `hitListRows` rows of kHitListWidth records, 28 bytes each of texture
    /// (+ 16 of the ray tier's buffer), created at its share of the target and grown
    /// in place on an overflow (OgreView::sizeHitList), so its size is NOT graph shape
    /// and a rebuilt graph takes the view's current rows. Not tied to the rows that trace
    /// (a toggle of the gather where the prepass already runs is not a new graph).
    bool  hitDecode = false;
    /// The list's rows when the graph is built (the view's current size; not part of sameShape).
    unsigned hitListRows = 1u;
    /// THE VISIBILITY BUFFER (ATOM S3-DRAW, OgreAtomDraw.cpp / OgreAtomIdPass.cpp):
    /// the id pass in front of every scene pass of the view, which then LOAD its
    /// depth and SKIP kAtomRenderQueue (the screen decode shades those items as
    /// the first draw of the prepass and of the opaque pass).
    bool  atomDraw = false;
    /// THE ID PASS'S OCCLUSION (ATOM-OCCLUSION-1; Scene::setAtomOcclusionEnabled, the
    /// measuring door): the two-pass form. The id pass culls against the PREVIOUS
    /// frame's pyramid (`jahHzb`, still holding it: nothing has rebuilt it yet this
    /// frame) and draws the survivors; the pyramid is rebuilt from that depth (ONE
    /// dispatch of the single-pass build, every level, FARTHEST); the LATE id pass
    /// (kAtomIdLatePassId) tests the rejected set again against it and draws the
    /// disoccluded ones into the same id image and depth. Only with `atomDraw`, and not
    /// where the view asked for a CLOSEST pyramid of its own (the reduce direction is
    /// one job property).
    bool  atomOcclusion = false;
    bool  refractions = false;
    /// THE RADIANCE READBACK (PostFxDesc::hdrReadback, HDR-READBACK-1): the
    /// scene result is kept in a FLOAT target even without `hdr`, and
    /// ChainHandles::radianceTexture names it. Set before the offscreen
    /// early-out (it is what the view KEEPS, not a post effect), and graph
    /// shape: the scene-format textures change.
    bool  hdrReadback = false;

    // ---- The hierarchical depth pyramid (SPECS/NANITE_SPEC.md §4.3) ----
    /// THE VIEW'S OWN pyramid of the COMPLETE scene depth (PostFxDesc::hzb), built
    /// once per frame right after the opaque pass, for a reader that asks for it
    /// (a screen-space trace, the cull suites' replays). The id pass's occlusion
    /// builds its pyramid into the same texture after the id pass (atomOcclusion,
    /// below) whether or not this is set. A GRAPH change — one texture and one
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
    /// ...and the CAMERA'S aspect the rectangle is fitted to (a uniform, never
    /// the shape): the SSR march, its resolve and the reprojection work in the
    /// SHOT's uv, which is the letterbox rectangle's (SSR-LETTERBOX-1), and
    /// applyViewGlobals derives the rectangle from this and the view's size
    /// exactly as OgreView::applyLetterbox does.
    float letterboxAspect = 0.0f;

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
    /// desynchronise. Backdrops (kBackdropBit: the Ground plane widget) and the
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

    // ---- LAYERED STEREO (SPECS/VR_SPEC.md §4.3; LAYERED-STEREO-1) ----------
    /// Render BOTH EYES, each into its own layer of a two-layer array at the
    /// same origin, every pass ONCE through VK_KHR_multiview.
    ///
    /// It is one flag here and a sweep over the built node (chain::build's
    /// applyStereo): every screen-sized texture becomes two layers and EVERY
    /// scene, quad and clear pass into one is multiview — a scene pass also
    /// gets `mInstancedStereo` and the cull camera, a quad its multiview twin.
    /// All of them or none: a pass left single-view would draw one eye only.
    /// What has no per-layer form (SSAO, SMAA, the SSR march, the tonemap's
    /// bloom read) is off by the session's policy (applyVrViewPolicy), and the
    /// sweep refuses a pass that would read a layered texture into a
    /// single-layer target.
    ///
    /// GRAPH SHAPE: it lives on the pass definitions, so it is part of
    /// sameShape() and a flip rebuilds the workspace — which happens exactly
    /// twice, when a session begins and when it ends.
    bool  stereo = false;

    // ---- The LOD switch band, per pass (fork 5230c9390+8282f6d70 (was 0075), ATOM-3-FIX) -------
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
    /// Does the chain carry the depth/normal PREPASS (the SSR stage's march or
    /// rays, the gather, the sun contact)? `build` builds it on this answer and
    /// `sameShape` compares it in place of the rows that ask for it. ...or the no-ray producer (screenIntegrate).
    bool prepass() const;
    /// ...the prepass a TRACE reads (the SSR stage, the gather, the sun contact) - the hit decode's condition: the
    /// no-ray producer's prepass appends no hit.
    bool tracedPrepass() const;
    /// THE SCREEN SOURCES (PHOTON-7 stage 6): does the chain carry this view's previous picture, the depth it was drawn
    /// at, the editor helpers' mask and the object motion - the screen trace's inputs (the one estimator on a ray chain,
    /// rq_reflect.comp) and the raster march's? The SSR stage, reflectSources Both or Screen, never STEREO (the copies
    /// are not per layer). `build` declares them on this answer and the ray tier binds them on it.
    bool screenSources() const;
    /// THE RASTER MARCH (the no-ray chain's screen-space reflection: JahSsrRayMarch + JahSsrResolve): the screen
    /// sources on a chain that traces NO rays. A ray chain answers its screen hits in the trace itself.
    bool rasterMarch() const;
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
    /// THE FLOAT SCENE RESULT a radiance readback downloads (HDR-READBACK-1):
    /// the texture the one composite quad reads — kRt0, or the refraction /
    /// distortion / AO stage that last rewrote it — named here because which
    /// one it is depends on the shape. Null unless ChainDesc::hdrReadback.
    const char *radianceTexture = nullptr;
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
///
/// `displayEncode` is the view's ChainDesc::displayEncode: the background is a
/// LINEAR scene colour, so a display view clears to its sRGB encoding (the
/// same code the scene chain's encode quad would write for it).
void buildBlank(Ogre::CompositorManager2 *cm, const std::string &workspaceDef,
                const Colour &background, bool overlays, bool displayEncode,
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
///
/// Ungraded, the two quads run `Jahshaka/DisplayEncode` over an RGBA16F inset
/// when `displayEncode` (the host view's ChainDesc::displayEncode, SRGB-ENCODE-1)
/// and a plain copy of an RGBA8 inset otherwise (the Plain instrument).
void buildPip(Ogre::Root *root, const std::string &workspaceDef, const ViewPipDesc &pip,
              float texWidthFactor, float texHeightFactor, bool displayEncode,
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
void setMeter(ExposureMeterPattern pattern, float lowPercent, float highPercent);
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
/// `shot` is the letterbox's inner rectangle in the target's uv (x, y, w, h) —
/// (0, 0, 1, 1) without a letterbox — the map between the SHOT's uv, which the
/// march, the resolve and the reprojection work in, and the textures they read.
/// `scene` is the view's (its fog law re-fogs a screen hit; null = no medium).
void updateSsr(Ogre::Camera *camera, const ChainDesc &desc, const float shot[4],
               SsrReprojection &reprojection, OgreScene *scene);
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
/// bloomAmount; fork feab041c6 (was 0082)). A uniform on the same material as the dither,
/// pushed from applyViewGlobals — and from OgreView::setPostFx, for the VR
/// session's view, which the engine's per-frame loop never reaches.
void setBloomAmount(float amount);
/// THE IMAGE BLOCK (IMAGE-1): pushes PostFxDesc::image into the tonemap quad's
/// uniforms (offsets from the defaults; the white balance as its matrix).
/// Debounced on the last grade pushed.
void setImageGrade(const ImageGrade &grade);
/// Drops the cached tonemap parameter block (the dither's off switch and the
/// bloom amount). Called from destroySsao, i.e. from ~OgreEngine, because the
/// cache is a SharedPtr into a material that is about to stop existing.
void forgetTonemapParams();
/// Engine init, once: THROWS when the staged HDR tonemap media lacks the
/// constants this build pushes (a build error — FORWARD-ONLY-1). A pipeline
/// with no tonemap quad passes.
void verifyTonemapMedia();
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
                      unsigned viewWidth, unsigned viewHeight, SsrReprojection &reprojection,
                      OgreScene *scene);

/// The seed value the HDR adaptation history holds for a given exposure — the
/// same `e^(E-2) / 0.18` grey-card constant the fixed tonemap uses, so a
/// re-seeded history starts exactly where a deterministic grade would land.
float exposureSeed(float exposure);

/// The name of the 1x1 texture the AUTOMATIC exposure's adaptation history
/// lives in, inside a built chain's scene node. Readable between frames
/// (keep_content, unlike the per-frame `jahLum` it is copied from), which is
/// what makes View::measuredExposureScale possible at all.
const char *exposureHistoryTextureName();
/// The texture HlmsPbs composites as the SSR/ray reflection (jahSsrReflection):
/// rgb = the reflected radiance, a = the weight the composite lerps by.
const char *reflectionTextureName();
/// The SSR march's one-frame colour history (jahSsrPrev): read at the start of a
/// frame, written at its end — with the exposure history, what a second workspace
/// of the same chain must be handed to draw the frame the first one drew.
const char *ssrHistoryTextureName();
/// ...and the depth that picture was drawn at (jahSsrPrevDepth, REFLECT-FIX-1): the
/// resolve's same-surface test reads it, so it is handed over with the colour.
const char *ssrHistoryDepthTextureName();

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
    /// mSsrReprojection as it stood before the view's push of this frame (CLOSE-SHOT-2:
    /// the capture's workspace pushes from it, so its resolve reprojects as the view's did).
    SsrReprojection mSsrReprojectionAtFrameStart;
};
}   // namespace chain

/// THE CAMERA'S IMAGE as every screen job reads it (rq_sun_contact.comp's five numbers): the eye (w 1 perspective, 0
/// orthographic), the image ray at the target's top-left, its steps across and down the whole target, and the view axis
/// - the ray tier's (RayQueryTier::eyeBasis) and the no-ray screen integrate's one text (OgreScreenIntegrate.cpp).
struct CameraImageF {
    float camPos[4] = { 0, 0, 0, 1 };
    float rayTL[4] = { 0, 0, 0, 0 };
    float rayRight[4] = { 0, 0, 0, 0 };
    float rayDown[4] = { 0, 0, 0, 0 };
    float fwd[4] = { 0, 0, 0, 0 };
};
CameraImageF cameraImage(bool ortho, const Ogre::Vector3 &pos, const Ogre::Quaternion &rot, float el, float er,
                         float et, float eb);
/// ...widened from a letterbox's shot to the whole target (the bars hold cleared depth: every job declines them).
void expandCameraImageToTarget(CameraImageF &e, const ChainDesc &cd, unsigned fullW, unsigned fullH);

// ---- THE PIXEL'S AREA LIGHT VISIBILITY, BOTH PRODUCERS' ONE HOST TEXT (PHOTON-7 stage 7: 7-A's, shared at 7-6;
// OgreAreaVis.cpp) - the ray tier (OgreRayQuery.cpp) and the no-ray route (the listener below) take the forward LTC
// set, the parameter block, the view's history and rest state and its frame's numbers from here; only the images, the
// jobs and the visibility differ.
/// The parameter block, member for member jah_area_vis_params.glsl's (every member a vec4: std140 and std430 alike).
struct AreaVisLightParams {
    float c0[4] = {};
    float eu[4] = {};
    float ev[4] = {};
    float emit[4] = {};
};
struct AreaVisParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float prevCamPos[4] = {};
    float prevRayTL[4] = {};
    float prevRayRight[4] = {};
    float prevRayDown[4] = {};
    float prevFwd[4] = {};
    float projParams[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
    float strata[4] = {};
    float rest[4] = {};
    float alpha[4] = {};
    float lights[4] = {};
    AreaVisLightParams light[4];
};
static_assert(sizeof(AreaVisParams) % 16u == 0u, "the area visibility's block is whole vec4s");
/// The trace's grid: one texel a 2 x 2 block (the spec's half resolution), both producers.
constexpr unsigned kAreaVisDivisor = 2u;
/// THE FORWARD LTC SET as HlmsPbs takes it for the pass (OgreHlms.cpp's light gathering, OgreHlmsPbs.cpp's
/// fillBuffersFor walk): the scene manager's global light list's LT_AREA_LTC lights in its order, the first
/// kAreaLtcLimit - so channel i of the product is the light the LTC loop reads at index i.
std::vector<const Ogre::Light *> forwardLtcLights(const Ogre::SceneManager *sm);
/// A VIEW'S AREA VISIBILITY STATE (either producer's): the history's age and ping-pong, the rest, the previous camera
/// (the five numbers) and the light set it traced.
struct AreaVisState {
    unsigned frame = 0u, flip = 0u, age = 0u, restFrames = 0u, lights = 0u, strata = 0u;
    unsigned long long restKey = 0ull;
    float prev[5][4] = {};
    std::vector<float> lightKey;
    bool held = false;
};
/// ONE FRAME OF IT: the lights packed in HlmsPbs's order, the camera, whether the view is at rest (the camera bit for
/// bit, the scene's rest key `restKey`, the light set) and so whether it HOLDS (past n^2 rest frames: nothing is
/// dispatched - `freshImages` never holds), and - not holding - every member of `pp` but the ray tier's alpha table:
/// the clearance's two terms, the producer's (`liftA`, `liftB`: the ray tier's drawn footprints and sub-pixel snap -
/// the ray origin's one law; the no-ray route's 0 and the voxel cell), the mask word `mask`, the strata n (`strata`),
/// the sequence and the rest. Returns whether the view holds.
bool areaVisFrame(AreaVisState &st, AreaVisParams &pp, const std::vector<const Ogre::Light *> &lights,
                  const Ogre::Camera *cam, const ChainDesc &cd, unsigned w, unsigned h, unsigned fullW, unsigned fullH,
                  unsigned strata, unsigned long long restKey, bool freshImages, float liftA, float liftB,
                  float mask, CameraImageF &eye);
/// ...and after its dispatches: the camera kept, the pair flipped, the age and the frame advanced.
void areaVisAdvance(AreaVisState &st, const CameraImageF &eye);
/// The ray tier's views' part of a scene's area visibility status (OgreRayQuery.cpp; nothing where the tier is compiled
/// out) - OgreScene::areaVisStatus adds the no-ray route's (OgreAreaVis.cpp).
void rayTierAreaVisStatsInto(const OgreEngine *engine, const OgreScene *scene, AreaVisStatus &st);

/// THE NO-RAY ROUTE'S SCREEN INTEGRATE, a view's half (PHOTON-7 stage 7, 7-6; OgreScreenIntegrate.cpp): binds the
/// compute pass's world caches and parameters in its early pre-execute, and registers its three targets with the Hlms
/// listener around every PrePassUse pass of the view (pass-scoped, as the gather's). One per view, through
/// OgreView::addWorkspaceListener (the capture's workspace rides it too).
/// ...AND THE ROUTE'S AREA LIGHT VISIBILITY (7-6; OgreAreaVis.cpp): binds its two passes' jobs whole (the images its
/// own, made while an LTC light is in the forward set, given back when none is) and registers `jahAreaVis` beside the
/// three targets.
class ScreenGiListener final : public Ogre::CompositorWorkspaceListener {
public:
    ~ScreenGiListener();
    void passEarlyPreExecute(Ogre::CompositorPass *pass) override;
    void passPreExecute(Ogre::CompositorPass *pass) override;
    void passPosExecute(Ogre::CompositorPass *pass) override;
    /// The route's area visibility as the scene publishes it (ScenePublished::areaVis; the ray tier's when it ran).
    void areaVisStatsInto(const OgreScene *scene, AreaVisStatus &st) const;
    OgreView *mView = nullptr;
private:
    void bindJob(Ogre::CompositorPassCompute *pass);
    /// THE STAND-DOWN (PHOTON-7 stage 7): a pass whose inputs are missing - or the frame capture's, for the area
    /// visibility, or a held or deferred trace - RUNS NOTHING: its shared job is marked stood down (the fork's
    /// HlmsComputeJob::setStoodDown), so the pass sets no binding, analyses and transitions no resource and dispatches
    /// nothing - the job's slots may hold another view's resources, since destroyed or resized. Every pass that runs
    /// marks it back (setStoodDown(false)) before it binds.
    static void standDown(Ogre::HlmsComputeJob *job);
    /// Is this pass the view's own workspace's (false: the frame capture's - it takes the view's area product, never
    /// its history)?
    bool ownPass(const Ogre::CompositorPass *pass) const;
    /// The test door gi.screenBindMissing (the passes read their inputs as missing).
    bool bindMissing() const;
    /// DOES THIS PASS'S DISPATCH RUN THIS FRAME (the read's F2)? A pass of a workspace that compiles in the background
    /// (CompositorWorkspace::setAsyncShaderCompile) is deferred while its job's permutation builds (the fork's
    /// HlmsCompute::dispatch): nothing written, so nothing it writes may be registered or aged. The job's permutation is
    /// set before the question (requestAsync asks about the permutation a dispatch would select).
    bool dispatchRuns(const Ogre::CompositorPass *pass, Ogre::HlmsComputeJob *job);
    /// The test door gi.screenShaderCold: while on, the three jobs take a permutation never built (a property no text
    /// reads, a new value each time the door comes on) - a real background compile.
    void coldKey(Ogre::HlmsComputeJob *job);
    Ogre::int32 mColdKey = 0;
    bool mColdOn = false;
    Ogre::UavBufferPacked *mParams = nullptr;
    // ---- the area visibility (OgreAreaVis.cpp) ----
    void bindAreaVis(Ogre::CompositorPassCompute *pass);
    void bindAreaVisResolve(Ogre::CompositorPassCompute *pass);
    void afterAreaVisResolve(const Ogre::CompositorPass *pass);
    void dropAreaVis();
    AreaVisState mAreaState;
    CameraImageF mAreaEye;
    /// The images: the sums' pairs (a set a word of four halves: S U, S2 U2), the surface words' pair, the rest means -
    /// at the trace's size - and the product.
    Ogre::TextureGpu *mAreaSU[2] = {}, *mAreaS2U2[2] = {}, *mAreaG[2] = {}, *mAreaRestSU = nullptr,
                     *mAreaRestS2U2 = nullptr, *mAreaVis = nullptr;
    Ogre::UavBufferPacked *mAreaParams = nullptr, *mAreaChain = nullptr;
    unsigned mAreaW = 0u, mAreaH = 0u, mAreaFullW = 0u, mAreaFullH = 0u;
    /// mAreaRan: traced or held this frame (the status's `running`); mAreaRead: the opaque passes read the product this
    /// frame (traced, held, or the last one written while the trace's shaders build or its inputs are missing).
    bool mAreaFresh = false, mAreaRan = false, mAreaRead = false, mAreaDispatched = false;
    /// The resolve ran this frame / has ever written the product since its images were made (F2: nothing registered
    /// for a dispatch that never ran).
    bool mAreaResolved = false, mAreaVisWritten = false;
    unsigned long long mAreaMarches = 0ull, mAreaVramBytes = 0ull;
    std::string mAreaReason;
    /// THE READBACK (the arm area.visReadback): a ring of tickets over the product, decoded once a transfer is done.
    static constexpr unsigned kAreaReadbacks = 4u;
    Ogre::AsyncTextureTicket *mAreaTickets[kAreaReadbacks] = {};
    bool mAreaTicketLive[kAreaReadbacks] = {};
    unsigned mAreaTicketNext = 0u;
    std::vector<unsigned char> mAreaVisHost;
    unsigned mAreaVisHostW = 0u, mAreaVisHostH = 0u;
};

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
// ---------------------------------------------------------------------------
// THE ARM REGISTRY (lane TEST-1, the perf audit's A2; Types.h ArmInfo)
// ---------------------------------------------------------------------------
//
// The engine's MEASUREMENT ARMS, one table. Each was a process-environment read in
// shipped per-frame code (the measuring doors: `getenv("JAH_R5_NO_MOTION")` read
// every frame so one process could hold both arms of an A/B) — deleted; a suite or
// the A/B harness (scripts/perf-ab.py) sets an arm through Engine::setArm, the verb
// engine.arm(name, value). The value a code path reads is the one LATCHED at the top
// of the frame (`latch`, from renderOneFrame), so an arm set mid-frame — a script
// verb between two passes of a frame never happens, but a VR pump's early return
// does — changes whole frames only. Every default is the shipped picture.
/// THE ARMS, ONE LIST (ENGINE-UPGRADE-U5 decision 19.3): X(enumerator, "name") - the enum, kArmNames and the
/// table's rows (OgreFrameMonitor.cpp, kArms) are all generated from or checked against it, each row by a
/// static_assert of its own, so an arm out of place is a compile error naming it. A new arm is one line here and
/// its row there.
#define JAH_ARM_LIST(X) \
    X(ReflectMotion, "reflect.motion") /* the march's object motion + the trace's mover branches */ \
    X(RaysOriginCut, "rays.originCut") /* the scale on every view ray job's drawn-surface lift (1 shipped; 0 the float bound alone, 0.5 half: the origin rows' controls) */ \
    X(RaysOriginPoint, "rays.originPoint") /* every view ray job's lift carries the depth-rebuilt point's own error - its depth quantum and the rasteriser's sub-pixel snap (0: the float bound alone - the float-only control) */ \
    X(ReflectSeedOffset, "reflect.seedOffset") /* the lobe draw's frame index offset (a second, independent noise) */ \
    X(ReflectFlatHits, "reflect.flatHits") /* every reflection ray's hit answered by a constant of its instance (the estimator and denoiser measured alone) */ \
    X(ReflectWindow, "reflect.window") /* the moving window MaxFrames N (0 = shipped kReflectMaxFrames) and every reader of it */ \
    X(ReflectPosed, "reflect.posed") /* the posed (skinned) identification */ \
    X(ReflectAlphaTested, "reflect.alphaTested") /* cut-out geometry traced as cut-out */ \
    X(ReflectMonoEyes, "reflect.monoEyes") /* a stereo trace with the mono basis */ \
    X(TlasRefit, "rayquery.tlasRefit") /* refit the TLAS instead of rebuilding it */ \
    X(GatherTemporal, "gather.temporal") /* the screen-probe gather's pixel history */ \
    X(AtomDecode, "atom.decode") /* the screen decode armed (0: the Atom items drawn by nothing) */ \
    X(AtomClusterOcclusion, "atom.clusterOcclusion") /* the cut tests each cluster against depth and pixel centres (0: neither - the exactness A/B door, ATOM-8) */ \
    X(AtomHistoryRebuild, "atom.historyRebuild") /* the occlusion's history pyramid rebuilt after the late pass (0: the late pass's own, built before its draw - the paired arm of its cost, ATOM-8 R6) */ \
    X(AtomClusterFootprint, "atom.clusterFootprint") /* the cluster test's footprint in texels a side (8 shipped; 4 = the instance test's - the measured arm, ATOM-8) */ \
    X(FieldScroll, "gi.fieldScroll") /* the irradiance field scrolls (0: re-placed whole) */ \
    X(FieldEnterInline, "gi.fieldEnterInline") /* a scroll's entered planes integrated inline, 4 deep (the H3 arm) */ \
    X(GiFreeze, "gi.freeze") /* the GI frozen: no cache work, every history kept (M1's measuring arm); 2 = the world held, the reflections live */ \
    X(HitListCap, "rq.hitListCap") /* the hit list holds at most N records (0: its demand-sized capacity) - the overflow arm */ \
    X(ReflectRestReference, "reflect.restReference") /* the reflection's 4,096-sample reference at rest, no stop, no filter (R1 (f)'s measuring arm) */ \
    X(GatherDecodeHits, "gather.decodeHits") /* every gather hit a record (the decode route), never the caches */ \
    X(GatherOctRes, "gather.octRes") /* the probe map's resolution (rays = octRes^2; 0 = the tier's) */ \
    X(GatherStride, "gather.stride") /* the probe stride in pixels (0 = the tier's) */ \
    X(GatherHistory, "gather.historyFrames") /* the pixel history's floor in frames (0 = shipped) */ \
    X(GatherFilterRadius, "gather.filterRadius") /* the probe-space filter's reach (0 = shipped) */ \
    X(GatherRestOff, "gather.restOff") /* no rest mean: every frame is the history's */ \
    X(GatherRestFrames, "gather.restFrames") /* the rest mean's length (0 = the settle) */ \
    X(GatherRestSeed, "gather.restSeed") /* the rest frames' sequence offset */ \
    X(GatherAgeView, "gather.ageView") /* paint pixels whose history is younger than N frames */ \
    X(GatherFreezeFrame, "gather.freezeFrame") /* the sample sequence's frame held (the determinism door) */ \
    X(GatherYoungFrames, "gather.youngFrames") /* the history length below which a pixel reads wider (0 = shipped) */ \
    X(GatherYoungReach, "gather.youngReach") /* that reach in pixels at a history of 0 (0 = shipped) */ \
    X(GatherValidationOff, "gather.validationOff") /* every reprojected history texel accepted (a test door) */ \
    X(GatherClassOff, "gather.classOff") /* the surface class out of the history test (a test door, PHOTON-2 K5) */ \
    X(GatherCrossStrata, "gather.crossStrata") /* rays stratified across N x N probes (0 = shipped, 1 = off) */ \
    X(GiCascadeFault, "gi.cascadeFault") /* cascade N's rebuild throws before its build (-1 = none) */ \
    X(GiCascadeFaultPost, "gi.cascadeFaultPost") /* cascade N's rebuild throws after its build (-1 = none) */ \
    X(GiCascadeFaultCommit, "gi.cascadeFaultCommit") /* cascade N's scroll commit throws half way through its copies (-1 = none) */ \
    X(GiScrollFrames, "gi.scrollFrames") /* a scroll's chunk frames (0 = the tier's count) */ \
    X(GiVoxelOctants, "gi.voxelOctants") /* a whole build's octants per axis (1 shipped, 2 = 8 octants; stage 2's measure) */ \
    X(GiRebuildCascade, "gi.rebuildCascade") /* cascade N rebuilt whole every frame while set (-1 = none; a measuring door) */ \
    X(GiRegenerateDirectional, "gi.regenerateDirectional") /* cascade N's directional levels regenerated whole from its level 0 every frame while set (-1 = none; a test door) */ \
    X(GiBounceJacobi, "gi.bounceJacobi") /* the bounces' JACOBI reference: the innermost N cascades restarted from their direct terms where the arm comes on (set to N), then every brick of them a pass every frame while set (a measuring door) */ \
    X(GiHitLevelMargin, "gi.hitLevelMargin") /* the hit read's level margin M in cells past the step (-1 = the measured default; a measuring door, PHOTON-7 stage 10, 10-2e) */ \
    X(GiFeedbackCarriedRoute, "gi.feedbackCarriedRoute") /* a brick its injection moved this frame is carried, never passed; the stopped bricks accumulate the carried section alone (a test door) */ \
    X(ShadowCasterCull, "shadow.casterCull") /* a sun map's Atom casters culled by the light's own depth (0: frustum only, the parity arm) */ \
    X(ShadowLightDepth, "shadow.lightDepth") /* a sun map's light-depth path: its pyramid, the late cut, the receiver grid (0: none of them built, the pre-U5 caster pass) */ \
    X(ShadowCasterPbsProbe, "shadow.casterPbsProbe") /* a sun map's PBS casters TESTED against the light's depth and counted (a measuring door) */ \
    X(GiFieldRays, "gi.fieldRays") /* the field's rays per depth texel (0 = shipped) */ \
    X(GiFieldSamples, "gi.fieldSamples") /* the field's sample target (0 = shipped) */ \
    X(GiFieldStatic, "gi.fieldStatic") /* the field's rays NOT rotated per frame */ \
    X(GiFieldStopOff, "gi.fieldStopOff") /* the field's per-probe variance stop off (every probe to the target) */ \
    X(GiFieldMarkRingFull, "gi.fieldMarkRingFull") /* the field's mark ring held FULL on every other frame (the keep path's test door) */ \
    X(GiRelightShaderHold, "gi.relightShaderHold") /* every cascade's relight waits as for its delta shader while set (the credit's test door) */ \
    X(ShaderForceParamsMisfit, "shader.forceParamsMisfit") /* the next compute dispatch refused as a parameters misfit, each time it comes on (the counter's negative control) */ \
    X(GiVoxelAreaStrata, "gi.voxelAreaStrata") /* the voxels' area light visibility over n x n ratio marches (0: one to its centre; a measuring arm) */ \
    X(GiRefuseGeometry, "gi.refuseGeometry") /* a voxel build with no geometry source (the refused device) */ \
    X(AtomDiscriminate, "atom.discriminate") /* the decode's failure colour codes (1), the chart (2) */ \
    X(AtomHitWorldLights, "atom.hitWorldLights") /* the hit decode's light list: 0 shipped, 1 off, 2 all */ \
    X(AtomHitVctSpecular, "atom.hitVctSpecular") /* the hit decode's VCT specular cone (0: compiled out) */ \
    X(AtomMaskPyramid, "atom.maskPyramid") /* the cut-outs' masks sampled at the fragment's level (0: level 0 only, the silhouette rows' reference) */ \
    X(GatherHitsReadback, "gather.hitsReadback") /* every uniform probe's per-ray record read back (FAR-SUN-1) */ \
    X(ReflectMetalDecode, "reflect.metalDecode") /* the reflector roughness below which a metal hit decodes */ \
    X(GiHitDirect, "gi.hitDirect") /* a voxel-answered hit's direct light at the hit (FAR-SUN-2; 0: the store whole) */ \
    X(GpuWalkWhole, "gpuscene.wholeWalk") /* the dirty scan walks every slot (the measuring arm; 0: the named slots) */ \
    X(GpuWalkParity, "gpuscene.walkParity") /* after every frame's compose, every slot checked against the table (the guard) */ \
    X(MessagesRefuse, "messages.refuse") /* every scene message of the frame refused unapplied (a test door, U3 decision 44) */ \
    X(InterpSerial, "interp.serial") /* the free frames' interpolation on the frame's thread (the measuring arm) */ \
    X(FakeRuntimePace, "vr.fakeRuntimePace") /* ms: each free frame waits as a runtime's xrWaitFrame would (a test door, U3 decision 53) */ \
    X(FakePresentBlock, "view.fakePresentBlock") /* ms: a free frame blocks to that span after its publish, in place of a blocking present (a test door, U3 decision 65) */ \
    X(CardsSunGridParity, "cards.sunGridParity") /* every sun trace's grid searches checked against the exhaustive search (the guard, U4 stage 2) */ \
    X(GiGeomSigParity, "gi.geomSigParity") /* every geometry signature checked against the whole sum (the guard, U4 stage 3) */ \
    X(GiRootBoxParity, "gi.rootBoxParity") /* every cascade mark checked against the items' own boxes (the guard, U4 stage 3) */ \
    X(GpuWalkTiming, "gpuscene.walkTiming") /* the GPU scene's walk times its observers' notifies (a measuring arm, U4 stage 3) */ \
    X(GpuObserversOff, "gpuscene.observersOff") /* the change feed's observers are not told (the paired arm of the notify share; a test door) */ \
    X(CardsNullMove, "cards.nullMove") /* the sun trace takes one more still-caster move with a NULL box (the non-finite search's test door, U4 stage 3) */ \
    X(GiPccPlacementLights, "gi.pccPlacementLights") /* every staged probe placement's light list and probe nodes checked (the guard, U4 decision 14) */ \
    X(GpuSerialCompose, "gpuscene.serialCompose") /* the GPU scene's compose after the barrier on the frame's thread (the identity row's other arm, U4 stage 4) */ \
    X(GpuComposeDigest, "gpuscene.composeDigest") /* the compose's digest of every staged row and tail (the identity row, U4 stage 4) */ \
    X(GpuThrowInLane, "gpuscene.throwInLane") /* the GPU scene compose throw door: before the stage, after it, or every time (a test door, U4 decisions 18.3 + 22.4) */ \
    X(GatherJitter, "gather.jitter") /* the probes' jitter: 0 shipped (the grid's one offset a frame), 1 off (the centre) */ \
    X(GatherFilterOff, "gather.filterOff") /* the probe-space filter off (the measuring arm, decision 20) */ \
    X(GatherIrradianceReadback, "gather.irradianceReadback") /* the gather's E/pi copied to the host (world.gatherIrradiance) */ \
    X(GiScreenIntegrateOff, "gi.screenIntegrateOff") /* a no-ray chain shades with the pixel's own cones (7-6's A/B: the voxel route) */ \
    X(AreaVisReadback, "area.visReadback") /* the pixel's area light visibility copied to the host (ScenePublished::areaVis, a test door; 7-A) */ \
    X(GiScreenBindMissing, "gi.screenBindMissing") /* the no-ray screen passes read their inputs as missing: they stand down (a test door, gi.screen_bind) */ \
    X(GiScreenZeroUnmarked, "gi.screenZeroUnmarked") /* the screen integrate dispatches no group WITHOUT the stood-down mark: the fork's BUGS-6 guard throws (a test door, gi.screen_bind) */ \
    X(GiScreenShaderCold, "gi.screenShaderCold") /* each time it comes on, the no-ray screen passes' jobs take a permutation never built: a background compile (a test door, gi.screen_bind) */ \
    X(GatherRoughQuadrature, "gather.roughQuadrature") /* the rough specular's quadrature door: 1 the filtered map kept beside the levels, 2 the lobe by brute force over it (7-3) */ \
    X(ShadowReceiverBoxesOff, "shadow.receiverBoxesOff") /* the receiver grid without the stock route's boxes (the fixture's negative control, U5 decision 22.3) */ \
    X(TexturePumpFault, "textures.pumpFault") /* the host's texture wait: 1 never done, 2 and its step throws (a test door, U5 decision 22.5) */ \
    X(AtomDensityJobsMissing, "atom.densityJobsMissing") /* the Density count's compute jobs looked up as missing (a test door, BUGS-1 item 6) */ \
    X(GatherHistoryReadback, "gather.historyReadback") /* the gather's pixel-history COUNT copied to the host (the stillness door, PHOTON-2 stage 0) */ \
    X(ReflectHistoryReadback, "reflect.historyReadback") /* the reflection history's COUNT copied to the host (the stillness door, PHOTON-2 stage 0) */ \
    X(CardDemandCount, "cards.demandCount") /* the card readers count their carded and uncarded hits (a measuring arm, PHOTON-7 stage 5) */ \
    X(CardGatherLate, "cards.gatherLate") /* the gather's demand stamps within N frames of a demand change dropped: a late first reader (a test door) */

enum class ArmId : unsigned {
#define JAH_ARM_ENUM(e, n) e,
    JAH_ARM_LIST(JAH_ARM_ENUM)
#undef JAH_ARM_ENUM
    Count
};
/// THE ARMS' NAMES, IN ArmId'S ORDER - generated from the one list.
inline constexpr const char *kArmNames[] = {
#define JAH_ARM_NAME(e, n) n,
    JAH_ARM_LIST(JAH_ARM_NAME)
#undef JAH_ARM_NAME
};
static_assert(sizeof(kArmNames) / sizeof(kArmNames[0]) == unsigned(ArmId::Count), "ArmId and kArmNames differ");

class ArmRegistry {
public:
    ArmRegistry();
    /// The value is PENDING until the next latch. False (and `err`) for an
    /// unknown name or a value outside the arm's range.
    bool set(const std::string &name, double value, std::string &err);
    /// The latched value / its truth (anything but 0).
    double value(ArmId a) const { return mLive[unsigned(a)]; }
    bool on(ArmId a) const { return mLive[unsigned(a)] != 0.0; }
    /// The top of a frame: what was set since the last frame takes effect now.
    void latch() { mLive = mPending; }
    std::vector<ArmInfo> list() const;
private:
    std::array<double, unsigned(ArmId::Count)> mPending{};
    std::array<double, unsigned(ArmId::Count)> mLive{};
};

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

/// THE MONITOR'S HAND-BACK (FRAME-MONITOR-DOOR-1): the finished records and the events, POPPED by the host — never a
/// request. A SECOND DOOR OF THE RECORDER'S KIND (OgreView::harvestVideoFrames / popVideoFrame), NOT the exchange
/// snapshot: the snapshot is a latest-value triple buffer the host latches; this is a bounded queue of items each read
/// once, the render side pushing (a record at its retire — the frame's close, or the stop's flush; an event when the
/// engine files it), the host popping under the one lock, and a pop never waits on a frame. Why it is not a request any
/// more: a request waits for the frame in flight, and in free-running the next frame pushes the next record — a drain
/// by request never found the queue empty (the measured livelock: 128 frame-waits per drain, the editor answering every
/// 3.5 s during a capture; spikes/free-running-hang/diagnosis.md).
///
/// OWNED BY THE ENGINE, not by the FrameMonitor: the monitor object dies when a capture stops, and the stop's tail (the
/// frames still waiting for their GPU samples, flushed) must outlive it until the host pops it. A new capture clears
/// it (a tail nobody popped is the previous capture's) — so what a stop leaves, the events filed after the host's last
/// pop included, waits there until the next pop or the next start (bounded, counted; Studio pops once after its stop).
/// BOUNDED like the ring it replaces: records at kRingCapacity, the
/// OLDEST dropped when full; events at kEventCapacity, the NEW one dropped — never a block; the monitor counts both
/// (MonitorStatus::framesDropped / eventsDropped, published).
class HandBack {
public:
    /// The render side: a finished record, oldest first. True when the queue was full and its oldest was dropped.
    bool pushRecord(MonitorRecord &&r);
    /// The render side: an event. False when the queue was full and this one was dropped.
    bool pushEvent(MonitorEvent &&e);
    /// The host: every queued record (events), oldest first, appended to `out`; the count. Never waits on a frame;
    /// the storage goes with the pop.
    unsigned popRecords(std::vector<MonitorRecord> &out);
    unsigned popEvents(std::vector<MonitorEvent> &out);
    unsigned records() const { std::lock_guard<std::mutex> l(mLock); return unsigned(mRecords.size()); }
    unsigned events() const { std::lock_guard<std::mutex> l(mLock); return unsigned(mEvents.size()); }
    /// A capture starts: what is queued is the previous capture's. The tees keep theirs (a tee's reader decides).
    void clear();
    /// THE TEES (Engine::openMonitorTee, PHOTON-2 F7): each a reader's own copy of every record pushed while it is
    /// open, bounded at kRingCapacity (the oldest goes).
    unsigned openTee();
    unsigned popTee(unsigned id, std::vector<MonitorRecord> &out);
    void closeTee(unsigned id);
private:
    mutable std::mutex        mLock;
    std::deque<MonitorRecord> mRecords;
    std::deque<MonitorEvent>  mEvents;
    struct Tee {
        unsigned                  id = 0u;
        std::deque<MonitorRecord> records;
    };
    std::vector<Tee> mTees;
    unsigned         mNextTee = 1u;
};

/// THE MONITOR ITSELF: the frame being built, the holding queue (frames waiting for their GPU samples) and the pass
/// stack; what it finishes goes to the engine's HandBack. One per process, owned by OgreEngine, created when the
/// monitor goes on and destroyed when it goes off — which is what makes "off" cost nothing rather than cost little.
class FrameMonitor {
public:
    explicit FrameMonitor(HandBack &out);

    // ---- the frame --------------------------------------------------------
    void beginFrame(unsigned long long frame, FrameCause cause, bool onscreen);
    void endFrame(unsigned scenesUpdated);
    /// The frame's drained input (MonitorRecord::input*), noted right after beginFrame.
    void noteInput(unsigned moved, bool gesture, unsigned fields) {
        mCurrent.inputMoved = moved; mCurrent.inputGesture = gesture; mCurrent.inputFieldsApplied = fields;
    }

    // ---- GPU timestamps (P1c, fork 1a81f866a+1bccc3f93 (was 0027)) -----------------------------
    //
    // A GPU sample comes back WHEN THE GPU HAS FINISHED ITS FRAME (the fork
    // polls each written query pool non-blocking at the top of every frame and
    // reports a pool only once every sample in it is back — MONITOR-RETIRE-1).
    // A frame record therefore cannot be published the moment it ends: it
    // waits in a holding queue until EVERY sample it asked for has answered
    // (a time, or a negative "will never have one"), and only then enters the
    // ring the host drains. The old rule — publish after a fixed three frames —
    // retired a heavy frame before its GPU had finished it: on 7.1 M id-pass
    // triangles 1-4 of ~570 records carried an id-pass time.
    // kMaxHeldFrames bounds the queue: a frame whose samples never return (a
    // lost device took the pools) is published unsampled after that many
    // newer frames, and counted (`gpuFramesAgedOut`).
    static constexpr unsigned kMaxHeldFrames = 16u;
    /// A frame waiting for its GPU samples, with the sample id of each pass
    /// and of each timed cache row.
    struct PendingFrame {
        MonitorRecord           rec;
        std::vector<unsigned> passSampleIds;    ///< parallel to rec.passes
        std::vector<unsigned> cacheSampleIds;   ///< parallel to rec.cacheWork
        /// Parallel to the two lists above: the row's sample opened with no other
        /// row of ours open and inside the frame's own pair (`beginRowSample`).
        std::vector<char>     passTop, cacheTop;
        unsigned              frameSampleId = 0u; ///< the frame pair's sample (0 = none)
        std::vector<unsigned> gapIds;             ///< the frame pair's gaps (registered at its close)
        bool                  gapLost = false;    ///< a gap's sample never answered
        unsigned              outstanding = 0u; ///< sample ids not yet answered
    };
    /// Frames published before every sample answered (kMaxHeldFrames).
    unsigned long long mGpuFramesAgedOut = 0ull;
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
    enum class SampleKind : unsigned char { Pass, Cache, Frame, Gap };
    struct GpuSampleSlot {
        unsigned   frame = 0;   ///< index into mPending
        unsigned   row = 0;     ///< index into rec.passes or rec.cacheWork
        SampleKind kind = SampleKind::Pass;
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
    /// Index-parallel to the two lists above: was the row's sample TOP-LEVEL.
    std::vector<char>     mPassTop, mCacheTop;

    // ---- THE ONE GPU-TIMING FACILITY (lane TEST-1, the perf audit's F3/F5) ----
    //
    // Every GPU millisecond the engine reports is a sample on the fork's stack
    // (fork 1a81f866a+1bccc3f93): a compositor pass's (PassListener), a dispatch's
    // (CacheScope), or the frame's own pair below. There is no other timestamp
    // query in the engine — the ray tier's five private rings, the screen-probe
    // gather's pool and the wall-clock "slopes" were folded into CacheScope and
    // deleted — so, by owner decision D3's second lock, no query pool exists
    // outside a capture, and a GPU reading is -1 outside one.
    //
    /// THE FRAME'S OWN PAIR (F5). Opened when the pools turn over at the close of
    /// the previous frame (`OgreEngine::gpuFrameBegin`, from closeRenderFrame), so
    /// the between-frames half of a frame (the mirror's GI work the next record
    /// adopts) lies inside it; closed when the frame's recording ends
    /// (`frameRenderingQueued`, or `endFrame` for a frame that rendered nothing).
    /// It is a SPAN — the first command of the frame's submission to the last —
    /// so a mid-frame flush's CPU bubble is inside it; MonitorRecord::unattributedGpuMs
    /// is the span less the top-level rows.
    unsigned mFrameSampleId = 0u;
    bool     mFrameSampleOpen = false;
    /// THE GAPS (TEST-1 fix round: coverage over BUSY spans). While the frame pair
    /// is open and no row of ours is, a "gap" sample is open; each row's sample
    /// closes it and the last row's end opens the next. A gap whose Ogre command
    /// buffer changed under it crossed a SUBMISSION (the GPU idled for the CPU):
    /// MonitorRecord::gpuIdleMs; the others are GPU work no row names:
    /// MonitorRecord::unattributedGpuMs. Registered with the pair's owner record when
    /// the pair closes (the pair outlives the record's end: it closes at the frame's
    /// close, after the VR eye copy).
    unsigned mGapId = 0u;
    bool     mGapOpen = false;
    std::uintptr_t mGapCmd = 0u;
    std::vector<unsigned> mFrameGaps;
    std::unordered_map<unsigned, bool> mGapSpanned;
    /// Each gap's neighbours (MonitorRecord::widestGap): the row whose end opened it, and at its close the row whose
    /// begin closed it. Erased with mGapSpanned.
    std::string mLastRowName = "frame";
    std::unordered_map<unsigned, std::string> mGapWhere;
    void closeGap(const std::string &next);
    /// The record that owns the open frame pair (its `frame` number), set by endFrame.
    unsigned long long mFrameOwner = 0ull;
    bool     mFrameOwned = false;
    void openGap();
    void closeGap();
    /// PASSES OUTSIDE A FRAME (a one-shot sky bake between frames — F2): banked and
    /// adopted by the next frame, like a between-frames cache row.
    std::vector<FramePass> mPendingPasses;
    std::vector<unsigned>  mPendingPassSampleIds;
    std::vector<char>      mPendingPassTop;
    /// The pools were turned over at the close of the last frame: the next
    /// renderOneFrame must not turn them over again (it would end the open pair).
    bool     mTurnedOver = false;
    /// Turnovers so far. The fork CLEARS its sample stack at each, so a sample
    /// opened before one and closed after it would pop somebody else's: a
    /// CacheScope remembers this number and refuses to close its GPU sample
    /// across a change (it files its row untimed instead).
    unsigned long long mTurnovers = 0ull;
    /// The render system the frame pair was opened on.
    Ogre::RenderSystem *mFrameRs = nullptr;
    /// How many of OUR row samples (passes, cache scopes) are open right now, the
    /// frame pair excluded. A row whose sample opens at depth 0 inside the frame
    /// pair is TOP-LEVEL: its time is inside the frame's and inside nobody
    /// else's, so the top-level rows are what the pair is compared with.
    unsigned mOpenRowSamples = 0u;
    /// THE ROWS' NESTING (PHOTON-7's naming step): the open rows innermost last, each with the submission tag its own
    /// stretch began at; a row's parent sample (0 = none) and whether its own stretches crossed a submission - read at
    /// the retire into FramePass / CacheWork selfGpuMs, selfMeasured and selfSubmitted.
    struct OpenRow {
        unsigned id = 0u; std::uintptr_t tag = 0u; bool submitted = false; bool aggregate = false;
        bool childSubmitted = false;   ///< a row inside it submitted (CacheScope::submittedInside)
        unsigned long long stagingAt = 0ull;   ///< the staging's submissions when its own stretch began
        bool staging = false;                  ///< ...and the upload staging submitted inside its own stretches
    };
    std::vector<OpenRow> mRowStack;
    std::unordered_map<unsigned, unsigned> mRowParent;
    std::unordered_map<unsigned, bool> mRowSubmitted;
    std::unordered_map<unsigned, bool> mRowStaging;
    /// THE UPLOAD STAGING's COUNTERS (the fork's VulkanStagingBuffer::msJahCounters, JahStagingCounters): asked once per
    /// capture; the frame's deltas go to the record at the frame's close (noteStaging), and a row's own stretch reads
    /// the submissions (fence flushes + the waits that submitted).
    const Ogre::uint64 *mStaging = nullptr;
    bool mStagingAsked = false;
    Ogre::uint64 mStagingLast[6] = {};
    const Ogre::uint64 *mQueue = nullptr;   ///< the graphics queue's counters (JahQueueCounters)
    Ogre::uint64 mQueueLast[4] = {};
    unsigned long long stagingSubmits(Ogre::RenderSystem *rs);
    /// Opens a row's sample on `rs` (0 = not sampling); `top` = was it top-level.
    unsigned beginRowSample(Ogre::RenderSystem *rs, const std::string &name, bool &top);
    /// Closes the innermost row sample (the fork's stack is LIFO, like ours).
    void endRowSample(Ogre::RenderSystem *rs, const std::string &name);
    void openFrameSample(Ogre::RenderSystem *rs);
    /// Closes the frame pair — only when no row sample is open, because the fork
    /// pops the INNERMOST sample: closing over an open row would end that row's
    /// sample instead. A pair that cannot close is abandoned (never answered,
    /// never waited for) and the record says `frameGpuMs` -1.
    void closeFrameSample();
    /// THE LAST GPU TIME, BY NAME AND OWNER: the status readouts that used to keep
    /// a private timestamp ring (RayQueryStatus::reflectMs, the gather's four
    /// jobs, the cards' relight...) read the CacheScope rows their owner filed.
    /// One frame's rows of the same key are SUMMED (a job dispatched twice in a
    /// frame costs both); a later frame replaces the value.
    struct LastGpu { unsigned long long frame = 0ull; float ms = -1.0f; };
    std::map<std::pair<std::string, const void *>, LastGpu> mLastGpu;
    /// sample id -> the key its answer updates (cache rows that named an owner).
    std::unordered_map<unsigned, std::pair<std::string, const void *>> mSampleKeys;
    /// Files a GPU result against the pass that asked for it. Unknown ids (a
    /// frame that already aged out) are dropped.
    void noteGpuSample(unsigned sampleId, float ms);
    /// Moves everything that has waited long enough from the holding queue into
    /// the ring. RENDER SIDE: the frame's close (and the monitor's stop) call it, never the host's drain.
    void retirePending(bool all);
    /// The next sample id for a pass about to execute (0 = not sampling).
    unsigned nextGpuSampleId() { return mGpu ? mNextGpuSampleId++ : 0u; }
    bool inFrame() const { return mInFrame; }
    MonitorRecord &current() { return mCurrent; }

    // ---- what the instrumentation sites file ------------------------------
    void stage(const char *name, double ms);
    void hostStage(const std::string &name, float ms);
    /// Banks a between-frames stage, coalescing by name past
    /// kPendingStageCoalesce so the pending list cannot grow without bound.
    void bankPending(const std::string &name, float ms);
    void cacheWork(const CacheWork &w, unsigned gpuSampleId = 0u, bool top = false);
    void pass(FramePass &&p, unsigned gpuSampleId = 0u, bool top = false);
    void event(MonitorEvent &&e);
    /// Cache work recorded BEFORE the frame opened — the probe budget and the
    /// lamp-map caster scan both run in the engine's pre-frame half — belongs
    /// to the frame that is about to render it.
    void adoptPendingCacheWork();

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
        bool     gpuTop = false;   ///< the sample opened top-level (beginRowSample)
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
    unsigned ringFrames() const { return mOut.records(); }
    unsigned pendingEvents() const { return mOut.events(); }
    unsigned long long framesRecorded() const { return mFramesRecorded; }
    unsigned long long framesDropped() const { return mFramesDropped; }
    /// GPU timing marks the query pool could not hold, since the monitor went
    /// on (MonitorStatus::gpuMarksDropped).
    unsigned long long gpuMarksDropped() const { return mGpuMarksDropped; }
    /// Files `n` marks the render system dropped in the frame that JUST ENDED —
    /// the newest record in the holding queue (the call is made at the head of
    /// the next frame, after its beginFrame) — and adds them to the total.
    void noteGpuMarksDropped(unsigned n);
    /// The upload staging's events since the last frame close, filed on the frame that just closed.
    void noteStaging(Ogre::RenderSystem *rs);
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
    void push(MonitorRecord &&r);

    /// THE HAND-BACK (FRAME-MONITOR-DOOR-1): the engine's, where every finished record and event goes; the host pops it.
    HandBack                 &mOut;
    std::vector<FrameStage>   mPendingHostStages;
    std::vector<CacheWork>    mPendingCacheWork;
    /// The GPU sample id of each pending row, parallel to `mPendingCacheWork`.
    /// Cache work BETWEEN frames (the mirror's GI half runs before the frame
    /// opens) still takes its timestamp pair — the pair rides the same command
    /// buffer the next frame's passes will — and the id is registered when the
    /// row is adopted.
    std::vector<unsigned>     mPendingCacheSampleIds;
    std::vector<char>         mPendingCacheTop;
    MonitorRecord               mCurrent;
    std::chrono::steady_clock::time_point mFrameStart;
    /// The number of the last frame that BEGAN. Events recorded between frames
    /// are stamped with it rather than with the empty record's 0.
    unsigned long long mLastFrameNumber = 0;
    double   mOverheadMs = 0.0;
    float    mLastOverheadMs = 0.0f;
    bool     mInFrame = false;
    unsigned long long mFramesRecorded = 0, mFramesDropped = 0, mEventsDropped = 0;
    unsigned long long mGpuMarksDropped = 0;
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
/// It also brackets the work with a GPU timestamp pair (fork 1a81f866a+1bccc3f93 (was 0027)) when
/// the capture has GPU sampling live, which is the only way a compute dispatch
/// can report GPU time at all: the monitor's other samples ride the compositor
/// pass callbacks, and a compute job the engine dispatches itself is not a
/// compositor pass. The pair is written into the same command buffer as the
/// dispatch and read back once the GPU has finished it, exactly like a pass's.
///
/// A SCOPE MUST NOT STRADDLE A FRAME BOUNDARY — and since lane TEST-1 it
/// cannot: a scope that closes after a turnover files its row with no GPU time
/// rather than pop a sample of the next frame. The render system's sample
/// stack is cleared when the host opens a frame (fork 1a81f866a+1bccc3f93 (was 0027)'s
/// `JahGpuFrameBegin`, so that a frame which threw cannot corrupt the next
/// one's nesting), and a `begin` on one side of that point with its `end` on
/// the other would pop somebody else's sample. Every call site here is inside
/// one frame or inside the mirror's pre-frame half, never across the edge.
///
/// `detail` is a `const char *` for the same reason EventScope's is: nothing
/// may be constructed at the call site while the monitor is off.
class CacheScope {
public:
    /// `owner` (optional) names WHO filed the row, for `lastGpuMs(detail, owner)`:
    /// a status readout of this job's GPU time reads it back by the same pair.
    /// Never written to the record (an address means nothing in a bundle).
    CacheScope(CacheKind cache, WorkReason reason, unsigned long long id,
               const char *detail, Ogre::RenderSystem *rs = nullptr,
               const void *owner = nullptr);
    ~CacheScope();
    CacheScope(const CacheScope &) = delete;
    CacheScope &operator=(const CacheScope &) = delete;
    /// What the work turned out to be worth (probes, items, bounces). Read at
    /// destruction, so the call site can set it once it knows.
    void setUnits(unsigned units) { mUnits = units; }
    /// Abandons the row entirely — for a path that decided to do nothing after
    /// all (an early return that built no work).
    void cancel() { mCancelled = true; }
    /// AN AGGREGATE ROW (PHOTON-7's naming step: gi.step): it only encloses others, so a dispatch through the Photon
    /// door while it is the innermost open row files a row of its own, by its job's name (photonDispatch) - its time
    /// is never left unnamed inside the aggregate's.
    void markAggregate();
    /// Whether the render system SUBMITTED anywhere inside the row so far (its own stretches or a child's) - asked
    /// before the close by a row that files only when work happened (PhaseScope, gi.step). False with the monitor off.
    bool submittedInside() const;
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
    bool               mGpuTop = false;
    unsigned long long mTurnover = 0ull;   ///< FrameMonitor::mTurnovers at the open
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

/// THE LAST GPU MILLISECONDS of the CacheScope rows `owner` filed under `detail`
/// (one frame's rows summed), or -1: no capture is running (no query pool exists
/// outside one — owner decision D3), the job has not run since it started, or its
/// answer has not come back yet. The ONE way a status readout reads a GPU time.
float lastGpuMs(const char *detail, const void *owner);
/// True while the innermost open monitor row is an AGGREGATE (CacheScope::markAggregate) - the Photon door then names
/// its dispatch with a row of its own. False with the monitor off.
bool innermostRowIsAggregate();
/// Marks the innermost open row an AGGREGATE (CacheScope::markAggregate) - for a compositor pass's own recorder
/// (photon_update), whose row the pass listener opened. Nothing with the monitor off.
void markInnermostRowAggregate();

/// A PHASE OF AN AGGREGATE (PHOTON-7's naming step): an aggregate row (CacheScope::markAggregate) over one stretch of
/// the GI step or of the cards' update pass, so no stretch of either is left to the aggregate's own time. FILED ONLY
/// WHEN WORK HAPPENED INSIDE IT - a Photon dispatch, work its owner notes (noteWork: an upload, a copy), or a submission
/// (its time then holds the GPU's wait for the CPU, placed) - so a still frame files none. Nothing with the monitor off.
class PhaseScope {
public:
    PhaseScope(const char *detail, Ogre::RenderSystem *rs);
    ~PhaseScope();
    PhaseScope(const PhaseScope &) = delete;
    PhaseScope &operator=(const PhaseScope &) = delete;
    /// The phase did GPU work no dispatch counts (a buffer upload, a copy): it files.
    void noteWork() { mWorked = true; }
private:
    CacheScope mRow;
    unsigned long long mDispatchesAt = 0ull;
    bool mWorked = false;
};
/// Drops `owner`'s readings — called where the owner dies, so a new object at the
/// same address never reads its predecessor's time.
void forgetGpuOwner(const void *owner);
/// A ONE-SHOT WORKSPACE is watched too (lane TEST-1, F2): the sky capture, the IBL
/// convolution and the cloud bake each make a workspace, update it once and destroy
/// it, so the per-frame attach never sees them — and a sky change was a hitch
/// nobody could attribute. Call between addWorkspace and the update; nothing to
/// undo (the workspace dies with its listener list). A no-op while the monitor is off.
void watchWorkspace(Ogre::CompositorWorkspace *ws);

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
/// Ogre's current command buffer as a tag: it changes exactly when the queue
/// SUBMITS. 0 = cannot tell (not Vulkan). The frame monitor's gap rows and the
/// voxel feed's no-drain sentry (VoxelFeed::readoutFlushes) both read it.
std::uintptr_t queueSubmissionTag(Ogre::RenderSystem *rs);
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
    void configure(const std::string &dir);

    /// Reads both files (after verifying the manifest) and hands Ogre's half to
    /// `importTextureMetadataCache`. Call from ensureHlms(), after the Hlms is
    /// registered and before anything can ask for a texture. Any doubt — missing
    /// manifest, wrong key, size or hash mismatch, a JSON parse error — deletes
    /// both files and starts cold. Never throws, never fatal.
    void load(Ogre::Root *root);

    /// SERIALIZES both files on the calling thread (Ogre's half needs it) and
    /// hands the WRITE — three files, each fsynced, the manifest last — to
    /// `dispatch` (the shader cache's writer thread: SHADER-WARM-2, no file
    /// sync on the UI thread). A null `dispatch` writes inline (a process with
    /// no writer). A no-op when the cache is off or the bytes are the ones the
    /// last save wrote.
    bool save(Ogre::Root *root, const std::function<void(std::function<void()>)> &dispatch);

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
    void wipe() const;
    std::string path(const std::string &name) const;

    std::string mDir, mKey;
    bool        mEnabled = false;
    bool        mDirty = false;
    /// hex128 of the last bytes WRITTEN (meta + channels; set by the writer
    /// thread once all three files landed): a save
    /// with nothing new writes nothing (the clean quit and the engine's
    /// destructor both save).
    std::string mLastWritten;          // GUARDED_BY(mLastWrittenMutex)
    std::mutex  mLastWrittenMutex;     ///< the writer thread sets it after a write lands
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
//   1. Never load stale. A layer whose key moved is dropped (CS-1: one key per
//      layer, each naming only what can change that layer's bytes); a file that
//      fails its checksum deletes the directory and starts cold. A cold start
//      costs seconds; a bad SPIR-V blob costs a GPU hang.
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
    void configure(const std::string &dir, const std::string &mediaDir,
                   unsigned long long maxBytes = 256ull * 1024ull * 1024ull);

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
    /// THE SAVE IS A CHUNKED COPY (FRAME-MONITOR-DOOR-1 stage 4): `save` copies at most kCopyChunkEntries shader
    /// entries per call (one render frame's message) and returns; while a copy is in progress the render loop
    /// continues it once a frame (OgreEngine::runFrame), and the call that catches up hands the job to the writer.
    /// `saveWhole` is the unchunked form for a teardown save; flushWrites completes a copy in progress first.
    bool saveWhole(Ogre::Root *root);
    bool copyInProgress() const { return mCopyInProgress; }
    /// The engine latched a device loss: later saves serialize the CPU copies only (no pipeline-cache read).
    void noteDeviceLost() { mDeviceGone = true; }
    /// Save steps run (the render loop compares it around a free frame) and the free frames that carried one —
    /// `overrun` when that frame's work exceeded the display period (RenderThread::threadMain).
    unsigned stepCount() const { return mCopyMessagesTotal; }
    void noteStepFrame(bool overrun) { ++mStepFrames; if (overrun) ++mStepFrameOverruns; }
    /// True once per frame a save step ran in (cleared by the read): the loop's continuation runs only when none did.
    bool takeStepMark() { const bool m = mStepMark; mStepMark = false; return m; }
    /// Waits for an in-flight off-thread write. True when the writer is idle,
    /// false when `budgetMs` ran out first (the write continues; nothing is
    /// abandoned). A no-op when no write is in flight.
    bool flushWrites(unsigned budgetMs);
    /// True while the writer holds or is running a job (FSYNC-1): a save that
    /// meets one SKIPS — it never waits on the calling thread.
    bool writeInFlight() const;
    /// Runs `task` on the writer thread, after the job (if any) and the tasks
    /// queued before it — the OTHER files this directory holds (the texture
    /// cache's three, SHADER-WARM-2) go through the same one writer, so no
    /// fsync of the cache directory ever runs on the caller's thread.
    /// flushWrites() waits for queued tasks too. The task touches no Ogre
    /// object (bytes the caller already serialized).
    void dispatchTask(std::function<void()> task);
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
    /// A compute pipeline the ENGINE built outside Ogre's Hlms (the ray tier's,
    /// through Ogre's VkPipelineCache), counted as dirt: a pipeline built lazily
    /// after the last save (a first cut-out material, a first mover's velocity
    /// job) reaches the disk even when no Hlms shader compiled that run. Any
    /// thread; process-wide (one device, one VkPipelineCache).
    static void noteEnginePipeline(double ms, const char *name);

    ShaderCacheStats stats(Ogre::Root *root) const;
    void progress(unsigned &compiled, unsigned &fromCache, unsigned &expected) const;
    /// Of `compiled`, those compiled on the background compiler's threads (ASYNC-SHADERS-1).
    void backgroundCompiles(unsigned &background, unsigned &foreground) const;
    /// THE RENDER-LOOP MONITOR'S COMPILE FEED. Ogre exposes no "a shader was
    /// compiled" callback — the counter is a log listener — and with
    /// OGRE_SHADER_COMPILATION_THREADING_MODE=2 it fires on WORKER threads, so
    /// the monitor cannot be touched from there. Instead the listener appends
    /// the compiled shader's NAME to a small bounded, mutex-guarded queue while
    /// `on` is set, and the frame loop drains it on the UI thread.
    /// Off by default: nothing is recorded and no string is built.
    void recordCompileNames(bool on);
    /// Engine::setCompileObserver's store: `observer` runs after every compile
    /// made on the thread calling this (the counter's log listener checks the
    /// thread). An empty function clears it.
    void setCompileObserver(std::function<void()> observer);
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
    /// `key` is the hex of the file's LAYER key when it was written (CS-1).
    struct Entry { std::string name; unsigned long long bytes = 0; std::string hash; std::string key; };
    /// One dispatched save: the serialized layers, waiting for the writer
    /// thread. `blobs` is parallel to `names`; `compileCount` is the counter
    /// reading this write makes true once it lands.
    struct PendingWrite {
        /// THE COPIES, serialized on the writer (stage 4): each dirty Hlms' disk-cache object BY REFERENCE (the
        /// ShaderCache's own, mCopy — the render thread appends to them only while no write is in flight, the
        /// skip law at the head of save(), which is what makes the sharing safe), the microcode SNAPSHOT (a map of
        /// immutable blobs) and the render system whose pipeline cache the writer reads (vk.xml: no externsync).
        std::vector<std::pair<std::string, Ogre::HlmsDiskCache *>> hlms;
        bool                                    microcodeDirty = false;
        Ogre::GpuProgramManager::MicrocodeMap   microcode;
        Ogre::RenderSystem                     *rs = nullptr;
        /// ...and the bytes the writer makes of them, before the atomic writes.
        std::vector<std::string>       names;
        std::vector<std::vector<char>> blobs;
        unsigned                       compileCount = 0;
        unsigned                       enginePipelines = 0;   ///< sEnginePipelines when serialized
    };
    /// THE CHUNK (stage 4): shader-code entries copied per message. MEASURED, Debug: 27-51 us an entry on a quiet box
    /// (the drag row's shape: 27 pbs entries 0.73-1.41 ms, 44 of type 4 1.53 ms) and 42-105 us under a sibling lane's
    /// build (load 9.5: shadercache.save_off_frame's cold run, chunks of 16 = 0.67-1.68 ms) — 8 x 105 us = 0.84 ms a
    /// chunk at the worst rate measured, leaving the catch-up's whole-copied rest (the PSO lists, the pieces, the
    /// microcode map: ~0.7 ms loaded) inside the message's 2 ms. A COUNT, never a clock.
    static constexpr size_t kCopyChunkEntries = 8u;
    bool  saveStep(Ogre::Root *root, size_t entryBudget);
    std::unique_ptr<Ogre::HlmsDiskCache> mCopy[Ogre::HLMS_MAX];   ///< one per Hlms type, alive across saves
    unsigned     mCopyMask = 0u;          ///< the Hlms types this save copies
    bool         mCopyInProgress = false;
    bool         mStepMark = false;
    Ogre::Root  *mCopyRoot = nullptr;
    unsigned     mCopyMessages = 0u;      ///< the messages this save's copy took so far
    double       mCopyWorstMs = 0.0;      ///< ...and the longest of them
    bool         mCopyRearmLogged = false;   ///< one "re-armed" line a save
    bool         mDeviceGone = false;        ///< the engine latched a loss: the writer never reads the device again
    unsigned     mChunkMaxEntries = 0u;      ///< the most entries one chunked message took (<= K)
    unsigned     mStepFrames = 0u, mStepFrameOverruns = 0u;   ///< the free frames that carried a step / overran
    unsigned     mSavesCopied = 0u, mCopyMessagesTotal = 0u;   ///< the process's (ShaderCacheStats)
    double       mCopyWorstEverMs = 0.0, mCopyChunkWorstEverMs = 0.0;
    std::atomic<unsigned> mSavesWritten { 0u };                 ///< the writer's finished writes

    /// THE WRITER THREAD's body: SERIALIZE the job's copies (each disk cache's saveTo, the microcode snapshot, the
    /// pipeline cache's data — vk.xml: vkGetPipelineCacheData needs no external sync), then publish every blob
    /// atomically, carry the manifest forward, write it, enforce the size cap. It reads the copies and the device's
    /// pipeline cache, nothing the render thread is writing.
    bool  runWrite(PendingWrite &job);
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

    std::string mDir, mFingerprint, mMediaDir;
    unsigned long long mMaxBytes = 256ull * 1024ull * 1024ull;   ///< EngineConfig::shaderCacheMaxBytes
    /// ONE KEY PER LAYER (SPEED-CPU, perf audit CS-1): pipeline, microcode,
    /// Hlms — see configure(). `mFingerprint` is the three composed, for the
    /// status readout only.
    std::string mLayerKey[3];
    void composeFingerprint();
    /// The current key (hex) of the layer `name` belongs to; empty for a file
    /// that is no layer of ours.
    std::string layerKeyFor(const std::string &name) const;
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
    std::atomic<unsigned> mSavedAtEnginePipelines { 0 };
    static std::atomic<unsigned>  sEnginePipelines;   ///< noteEnginePipeline's count
    static std::atomic<long long> sEnginePipelineUs;  ///< ...and their build time
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
    std::deque<std::function<void()>> mWriteTasks;  ///< dispatchTask(), not yet run
    bool                    mWriteBusy = false; ///< a job or task is being written NOW
    bool                    mWriteStop = false;
    std::thread             mWriteThread;
};

// ---------------------------------------------------------------------------
// Fog. The DISTANCE term is Ogre's fog block (HlmsPbs, under `hlms_fog`), fed
// by OUR atmosphere component (Atmosphere.h, JahAtmosphere — an
// Ogre::AtmosphereComponent) registered on the scene's SceneManager: it sets
// hlms_fog and binds the const buffer the block reads. What that block cannot
// give us is an AUTHORED colour or height fog, so those ride this listener's
// pass-buffer extension, read by media/Hlms/Jahshaka/JahFog_piece_vs_piece_ps.any
// in BOTH shader stages — and under the planet's atmosphere the whole fog,
// the air's aerial perspective first, is that piece's.
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
    /// The planet's atmosphere is the scene's sky — read in preparePassHash,
    /// where it claims the atmosphere's two tables as pass textures
    /// (`jah_atmo_ap`): our media file then does the WHOLE fog
    /// per pixel — the air's aerial perspective, then the World fog towards the
    /// sky's own radiance (upstream's block is left an identity, the
    /// component's density 0); with it false the authored colour (r, g, b
    /// above) feeds upstream's block.
    bool  atmosphere    = false;
    /// The World fog's distance density under the atmosphere (exp2 per world
    /// unit), pass-buffer float 7; 0 otherwise — upstream's block carries the
    /// World fog then.
    float distanceDensity = 0.0f;
};

class FogHlmsListener final : public Ogre::HlmsListener {
public:
    Ogre::uint32 getPassBufferSize(const Ogre::CompositorShadowNode *, bool casterPass,
                                   bool, Ogre::SceneManager *) const override;
    float *preparePassBuffer(const Ogre::CompositorShadowNode *, bool casterPass, bool,
                             Ogre::SceneManager *sceneManager, float *passBufferPtr) override;
    /// THE LAMP-MAP CACHE'S SELF-CHECK (ENGINE_CACHE_POLICY_SPEC E2, fork 6130df9d1
    /// (was 0025)). Hlms declares `hlms_num_shadow_map_lights` from the shadow node's
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
    struct SceneBinds;   // the scene's pass record (below)
    /// The scene itself is going away: its record reset, every host's pass copy dropped.
    static void     unregisterScene(SceneBinds &own);
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
    /// What media/Hlms/Jahshaka/JahIfd_piece_ps.any needs that upstream's own
    /// render params do not carry: the probe counts and the window. (The
    /// intensity dial that rode here is DELETED, PHOTON-GATHER-1d: the field is
    /// applied at its own physical answer; `jahIfd.x` is unused.)
    struct IfdState {
        /// The field's probe counts on Y and Z. Upstream's own render params
        /// carry only Nx and Nx*Ny (PhotonIrradianceField.cpp:812-813) and the
        /// cage clamp needs all three axes, so the two missing
        /// numbers ride our own float4 instead of a patch that would move the
        /// engine ABI.
        float numProbesY = 0.0f;
        float numProbesZ = 0.0f;
        /// THE FIELD'S WINDOW (PHOTON-WRITER-1): the slot of the window's first
        /// probe per axis (PhotonIrradianceField::getWindowOffset), packed x + 128 y +
        /// 16384 z - exact in a float, each below the 128-probe axis cap - for
        /// the reader's JahFieldWindow modulo (JahIfd_piece_ps.any).
        float windowOffsetPacked = 0.0f;
    };
    static void     setIfdState(const Ogre::SceneManager *sm, const IfdState &state);
    static IfdState ifdState(const Ogre::SceneManager *sm);

    /// THE LISTENER'S OWN SAMPLERBLOCKS DIE WITH THE ENGINE (PHOTON-ENV-1 audit
    /// F10). The environment slot's trilinear block and the gather's point
    /// block are each acquired ONCE per HlmsManager (a uint16 reference count
    /// — never per pass) and released here, from ~OgreEngine before Root:
    /// the listener is a process global and outlives the manager, and a second
    /// Engine's manager can land at the same address (test_engine_recreate),
    /// which a pointer compared against a dead one would never notice.
    static void releaseSamplers();
    static const Ogre::HlmsSamplerblock *acquireSampler(Ogre::HlmsManager *mgr, bool trilinear);
    /// The cloud field's block: trilinear and WRAPPED (the field tiles), taken
    /// once per manager like the two above and released with them.
    static const Ogre::HlmsSamplerblock *acquireWrapSampler(Ogre::HlmsManager *mgr);

    /// THE CLOUD LAYER'S GROUND SHADOW (CLOUDS-2D-1): the third extra pass
    /// texture on the same three-hook route as the sky's environment and the
    /// gather (`jah_cloud_shadow`, register `jahCloudField`), plus the members
    /// JahFog_piece_vs_piece_ps.any appends LAST to the pass-buffer extension.
    /// Registered per scene by OgreScene::applyCloudLayer; a scene with no layer
    /// (or a shadow strength of 0) registers nothing, sets no property and
    /// generates exactly the shaders it generated before the layer existed.
    struct CloudShadowState {
        /// The baked optical-depth field (R16F, one tile), or null = no shadow.
        Ogre::TextureGpu *field = nullptr;
        float invTile = 0.0f;        // 1 / the tile's size in metres
        float strength = 0.0f;       // 0..1
        float scroll[2] = { 0.0f, 0.0f };   // metres, this frame
        float altitude = 0.0f;       // metres
        float sunThrow[2] = { 0.0f, 0.0f }; // toSun.xz / toSun.y
        float invMuSun = 1.0f;       // 1 / toSun.y (clamped)
    };
    static void             setCloudShadow(const Ogre::SceneManager *sm, const CloudShadowState &state);
    static CloudShadowState cloudShadow(const Ogre::SceneManager *sm);

    /// THE PLANET'S ATMOSPHERE'S VOLUME (SKY-ATMOSPHERE-1): the aerial
    /// perspective, with the sky in its last slice — ONE more extra pass
    /// texture on the same three-hook route (`jah_atmo_ap`, register
    /// `jahAtmoAerial`; ONE: OgreFog.cpp holds the pass-texture budget per
    /// family), claimed by every colour pass of a scene whose sky is the
    /// atmosphere (FogState::atmosphere) and read by JahFog's piece. Registered
    /// by OgreScene::syncAtmosphere; null = not the sky.
    struct AtmoBind {
        Ogre::TextureGpu *aerial = nullptr;
    };
    static void setAtmosphere(const Ogre::SceneManager *sm, const AtmoBind &bind);

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
    /// fork 4d5fbef16+8f09c0cd4 (was 0048). The state is per scene because the sky is.
    struct SkyEnvState {
        /// The scene's prefiltered sky cube (`mReflectionTex`), or null: null
        /// is "this scene has no sky reflection", which is also what a Sky
        /// Light at zero gain means (the sky goes out by not being BOUND).
        Ogre::TextureGpu *cube = nullptr;
        /// The Sky Light's gain per channel (PHOTON-ENV-1: every escape reads
        /// the cube at it). It rides here and NOT in `passBuf.ambientUpperHemi.w`
        /// because that pass scale is one channel and is 1.0 while a PCC is
        /// bound, deliberately (envmapScaleForPass: a probe photographs real
        /// radiance and must not be scaled by the skylight dial).
        float gain[3] = { 1.0f, 1.0f, 1.0f };
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
    /// Registered PER SCENE MANAGER for the length of ONE PASS: the ray tier's
    /// listener registers it in the PrePassUse pass's passPreExecute and takes
    /// it away in that pass's passPosExecute (OgreRayQuery.cpp), so no other
    /// pass — another view's, a mirror's, a probe capture's — ever sees it.
    /// THE TWO SCREEN PRODUCTS (PHOTON-7 stage 7, 7-3): the irradiance and the ROUGH SPECULAR (`jahRoughSpec`, the
    /// pass property `jah_rough_spec`: the pass's envColourS, its cone specular and its material cube compiled out),
    /// registered together - the gather writes both every frame it writes either.
    struct ProbeGatherBind {
        Ogre::TextureGpu *irradiance = nullptr;
        Ogre::TextureGpu *roughSpec = nullptr;
        /// THE NO-RAY ROUTE (PHOTON-7 7-6): the screen integrate's cone-hit distance - non-null says roughSpec holds
        /// the CONE's specular (JahScreenCone_piece_ps.any), not the probes' lobe
        Ogre::TextureGpu *coneHit = nullptr;
    };
    static void setProbeGather(const Ogre::SceneManager *sm, const ProbeGatherBind &bind);
    static ProbeGatherBind probeGather(const Ogre::SceneManager *sm);

    /// HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1) — the gather's route, word for
    /// word: the ray tier registers the visibility texture it has just written
    /// (`jahSunVis`, R8, 1 = lit) and the texel DIVISOR it was written at (1 =
    /// one ray per pixel, 2 = one per 2x2 block) immediately before the pass
    /// that shades with it, and takes it away when that pass ends; this
    /// listener turns it into the pass property `jah_sun_contact` (its VALUE is
    /// the divisor), the fourth extra slot and one binding.
    static void setSunContact(const Ogre::SceneManager *sm, Ogre::TextureGpu *visibility,
                              unsigned divisor);
    /// THE AREA LIGHTS' VISIBILITY (PHOTON-7 stage 7, 7-A) - the sun contact's route: the producer (the ray tier, or
    /// the no-ray route's screen-GI listener) registers the full-resolution RG32_UINT `jahAreaVis` (x the diffuse-
    /// weighted visible share of the pass's forward LTC light i in byte i, y the specular-weighted) in front of the
    /// opaque pass and takes it away as that pass ends; the listener's pass property `jah_area_vis`, its slot and its
    /// binding follow. Null = none.
    static void setAreaVis(const Ogre::SceneManager *sm, Ogre::TextureGpu *visibility);

    /// THE PHOTON VIEW'S ISOLATION (PHOTON-VIEW-1): Diffuse (1) or Reflections (2)
    /// for the pass being executed, registered by the view's photon listener in
    /// front of the pass that shades the scene (the opaque pass,
    /// planar::kPlanarUpdatePassIdentifier) and taken away when it ends — the
    /// gather's pass-scoped route, so no capture, probe, card, mirror or decode
    /// pass of the same SceneManager ever sees it. The pass property
    /// `jah_photon_view` (its VALUE is the mode) selects the lighting text's own
    /// isolation (JahPhotonView_piece_ps.any): one lighting text, never a second
    /// shader. 0 takes it away.
    static void setPhotonIsolation(const Ogre::SceneManager *sm, int mode);

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
    /// THE PHOTON VOLUMES' ARRAYS (OWN-GI-1): the light volumes are texture ARRAYS of
    /// one per cascade, and HlmsPbs adds the array bindings only for the four it knows
    /// (PhotonPassBinding::setupRootLayout adds the rest). And the specular lobe's albedo
    /// table's constant buffer (PHOTON-3), one past HlmsPbs's own range.
    void setupRootLayout(Ogre::RootLayout &rootLayout, const Ogre::HlmsPropertyVec &properties,
                         size_t tid) const override;
    /// THE PASS-TEXTURE TABLE'S EXACT CHECK (BUGS-1): throws, naming the host, when set 0's
    /// texture or sampler range ends past the fork's NUM_BIND_TEXTURES — at permutation
    /// build, before VulkanRootLayout would refuse it at PSO creation. The budget and why
    /// the bound is the range's END: OgreFog.cpp.
    static void refusePassTableOverflow(const Ogre::RootLayout &rootLayout, const char *host);

private:
    /// What the pass BEING BUILT binds in the listener's extra slots — the sky
    /// cube and the gather's irradiance, each with its samplerblock — decided in
    /// preparePassHash and read in hlmsTypeChanged, on the render thread, within
    /// one pass. Set together or not at all: a slot claimed by
    /// getNumExtraPassTextures and left unbound is an undefined descriptor.
    ///
    /// ONE PER Hlms HOST, indexed by the Hlms type (ATOM-S3-PARITY): the listener
    /// is set on every PBS-family host (tellEveryHlms), `RenderQueue::
    /// renderPassPrepare` runs preparePassHash on EVERY registered Hlms of a pass
    /// in turn, and a single shared copy was overwritten by whichever host ran
    /// last — a host whose pass properties differed would have left another's
    /// claimed slot unbound.
    struct PassBinds {
        Ogre::TextureGpu             *skyCube = nullptr;
        const Ogre::HlmsSamplerblock *skySampler = nullptr;
        Ogre::TextureGpu             *probeGather = nullptr;
        const Ogre::HlmsSamplerblock *probeGatherSampler = nullptr;
        Ogre::TextureGpu             *roughSpec = nullptr;            // PHOTON-7 7-3 (the gather's sampler)
        Ogre::TextureGpu             *coneHit = nullptr;              // PHOTON-7 7-6 (the gather's sampler)
        Ogre::TextureGpu             *cloudField = nullptr;           // CLOUDS-2D-1
        const Ogre::HlmsSamplerblock *cloudSampler = nullptr;
        Ogre::TextureGpu             *sunVis = nullptr;               // PHOTON-RAYS-1
        const Ogre::HlmsSamplerblock *sunVisSampler = nullptr;
        Ogre::TextureGpu             *areaVis = nullptr;              // PHOTON-7 7-A
        const Ogre::HlmsSamplerblock *areaVisSampler = nullptr;
        Ogre::TextureGpu             *atmoAerial = nullptr;             // SKY-ATMOSPHERE-1
        const Ogre::HlmsSamplerblock *atmoSampler = nullptr;
        /// PHOTON-3: the constant-buffer slot of the specular lobe's albedo table
        /// (PhotonSpecularAlbedoBuffer.h) in this pass, -1 for a pass that declares none.
        int                           specAlbedoSlot = -1;
    };
    static PassBinds sPass[Ogre::HLMS_MAX];                            // render thread only
    /// The contact job's registration: the texture and its divisor (the pass's
    /// copy is PassBinds::sunVis; the divisor becomes the property's value).
    struct SunContactBind { Ogre::TextureGpu *tex = nullptr; unsigned divisor = 1u; };
public:
    /// THE SCENE'S PASS RECORD (ENGINE-UPGRADE-2B stage 2): what one scene's passes bind beyond
    /// the GI binding — the sky's environment, the gather's and the sun contact's registrations
    /// (pass-scoped), the photon view's isolation, the cloud shadow, the atmosphere's tables, the
    /// fog, the shader clock, the field's calibration. HELD BY THE SCENE (OgreScene::passRecord)
    /// and found through its manager (sceneOfManager), never a process-wide map keyed by manager
    /// pointers: it dies with its scene, and a recycled manager address can name nothing stale.
    /// Every field's default is what a lookup used to answer for a manager that had none.
    struct SceneBinds {
        SkyEnvState      skyEnv;
        ProbeGatherBind  probeGather;              ///< GATHER-0, 7-3 (the pass's copies: PassBinds::probeGather, roughSpec)
        SunContactBind   sunContact;               ///< PHOTON-RAYS-1 (the pass's copy: PassBinds::sunVis)
        Ogre::TextureGpu *areaVis = nullptr;       ///< PHOTON-7 7-A (the pass's copy: PassBinds::areaVis)
        int              photonIsolation = 0;      ///< PHOTON-VIEW-1: 1 diffuse, 2 specular, 0 none
        CloudShadowState cloudShadow;              ///< CLOUDS-2D-1 (the pass's copy: PassBinds::cloudField)
        AtmoBind         atmo;                     ///< SKY-ATMOSPHERE-1 (PassBinds::atmoAerial)
        FogState         fog;
        float            sceneTime = 0.0f;
        IfdState         ifd;
    };
    /// The record of the scene drawn through `sm`, or null (a manager no scene holds).
    static SceneBinds *bindsOf(const Ogre::SceneManager *sm);
    /// A scene's PASS-SCOPED registrations (the gather's, the sun contact's), cleared — the
    /// frame's backstop (OgreEngine::updateRayQuery, every scene).
    static void clearPassScoped(SceneBinds &b) {
        b.probeGather = ProbeGatherBind();
        b.sunContact = SunContactBind();
        b.areaVis = nullptr;
    }
private:
    static const Ogre::HlmsSamplerblock *sCloudSampler;                // render thread only
    static Ogre::HlmsManager            *sSamplerMgr;                  // render thread only
    static const Ogre::HlmsSamplerblock *sEnvSampler;                  // render thread only
    static const Ogre::HlmsSamplerblock *sGatherSampler;               // render thread only
    static unsigned       sLightCountMismatches;                       // render thread only
    static unsigned       sMismatchLogged;                             // render thread only
    /// Shadow nodes whose slot assignment changed this frame — see
    /// noteShadowAssignmentChanged. A handful of pointers at most (one per
    /// shadow-node instance in the process), rebuilt per frame, and EMPTY on
    /// every frame that assigned nothing, which is every frame of a still
    /// scene.
    static std::vector<const Ogre::CompositorShadowNode *> sAssignmentChanged;  // render thread only
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
/// The one mirror-relevant property of the datablock an item wears: a CULL_NONE
/// reflector fills its own reflection with itself and is refused.
bool isTwoSided(const Ogre::Item *item);

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
// Photometric (IES) light profiles and the area lights' LTC set — OgreLights.cpp.
//
// The profiles are PROCESS-WIDE by construction, not per-scene, and that is a
// decision rather than an accident: LightProfiles::build() writes
// HlmsPbs::setLightProfilesTexture AND Root::_setLightProfilesInvHeight — there
// is exactly one of each per process, so a per-scene registry would have scenes
// fighting over the binding (the owner shape the GI arms and the planar mirrors
// had before SceneGiBinding, which we do not want to repeat). The atlas costs a texture slot in EVERY pass once armed (mTexUnitSlotStart grows),
// which is why it arms lazily on first use — the loadLtcMatrix precedent.
namespace lightextras {

/// The forward area-light budget. Ogre defaults to ONE and silently drops the
/// rest; the shader property IS the limit (not the live count), so raising it
/// costs one shader variant, not one per light added. EVERY area light is LTC
/// (PHOTON-7 7-0, L5): the approximation's budget is 0, so HlmsPbs never draws
/// one, and the area mask (which only the approximation sampled) is deleted.
constexpr Ogre::uint16 kAreaLtcLimit    = 4u;

/// AN AREA LIGHT'S RECTANGLE IN WORLD SPACE (PHOTON-7 stage 7, 7-A: ONE TEXT for the cards' lamps, OgreSurfaceCache,
/// and the pixel's area visibility, OgreRayQuery - as HlmsPbs and the voxel injection build it): the node's x and y
/// axes times the light's half extents, the corners counter-clockwise from -x -y.
inline void areaLightCorners(const Ogre::Light *l, Ogre::Vector3 out[4]) {
    const Ogre::Vector3 p = l->getParentNode()->_getDerivedPosition();
    const Ogre::Vector2 half = l->getDerivedRectSize() * 0.5f;
    const Ogre::Quaternion q = l->getParentNode()->_getDerivedOrientation();
    const Ogre::Vector3 xA = q.xAxis() * half.x, yA = q.yAxis() * half.y;
    out[0] = p - xA - yA;
    out[1] = p + xA - yA;
    out[2] = p + xA + yA;
    out[3] = p - xA + yA;
}

/// Assigns the IES profile at `path` to `light` (empty path = unset). Loads and
/// atlas-builds the profile the first time a path is seen, and NEVER rebuilds
/// for a path already registered — `setLight` runs every frame, and build()
/// recreates + re-uploads a GPU texture.
/// Returns false and fills `error` on failure; the light keeps what it had.
bool assignProfile(Ogre::Root *root, Ogre::Light *light, const std::string &path,
                   std::string &error);

/// Raises HlmsPbs's forward LTC budget off Ogre's default of 1 (and the
/// approximation's to 0: none is ever drawn). Called
/// where loadLtcMatrix is armed: the second area light of a scene renders
/// nothing without it.
void armAreaLightBudgets(Ogre::Root *root);

/// Loads HlmsPbs's LTC/BRDF lookup matrices, once — required before ANY area
/// light is drawn. Lazy on purpose: the
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

/// Destroys the profile atlas. MUST run before `delete Root`
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
/// THE TEXTURE'S UPLOAD GENERATION (ALPHA-MASK-IDENTITY-1): how many times the
/// engine has written texels into this TextureGpu (OgreScene::uploadRgbaLevels —
/// createTexture's upload and every updateTexture). A consumer that derives data
/// from a texture's CONTENT (the rays' cut-out masks, the cards' sun term) keys
/// on it: the pointer, name and size do not move when new texels land in place.
/// 0 for a texture never uploaded through it (a file load — its content is
/// fixed for its life). Forgotten when the texture is destroyed.
uint32_t textureUploadGeneration(const Ogre::TextureGpu *tex);
void     noteTextureUploaded(const Ogre::TextureGpu *tex);
void     forgetTextureUploads(const Ogre::TextureGpu *tex);
void resetSharedTextures();

// ---------------------------------------------------------------------------
// THE SCENE BEING DRAWN IS THE SCENE THE SHADER READS (PHOTON-SCENE-SWITCH-1) —
// OgreGi.cpp.
//
// HlmsPbs holds ONE parallax-corrected cubemap pointer (+ its two blend distances)
// for the whole process. Each scene says what ITS passes read in its own
// `SceneGiBinding`, held by the scene bound on its SceneManager, and every PBS-family host
// binds the record of the pass's own SceneManager as the pass begins —
// `ScenePbs::analyzeBarriers` (the first thing a scene pass asks of an Hlms:
// CompositorPassScene::execute -> analyzeBarriers, before the barriers that must name
// the SAME textures the shader will sample) and `preparePassHash` (a pass that skips
// the barrier walk: the warm-up pass). That is every scene pass of every workspace —
// a view, an offscreen view, a thumbnail, a material preview, a probe or card capture
// and Ogre's own internal ones. Nothing is restored after a pass: the next pass binds
// its own. There is no owner: a scene's arms are bound when, and only when, its own
// passes run.
//
// THE PHOTON VOLUMES ARE NOT HlmsPbs's (OWN-GI-1): the voxel lighting and the
// irradiance field are our own classes (photon/voxel/), HlmsPbs's VctLighting and
// IrradianceField pointers stay null, and the pass reads `vct` / `ifd` below through
// the listener (PhotonPassBinding, called from FogHlmsListener's hooks), which
// resolves the same record by the pass's SceneManager.
struct SceneGiBinding {
    Ogre::PhotonVoxelLighting          *vct = nullptr;
    Ogre::PhotonIrradianceField        *ifd = nullptr;
    Ogre::ParallaxCorrectedCubemapBase *pcc = nullptr;
    /// The PCC-versus-VCT trust window the grid was bound with (buildPccFinish).
    float pccMinDist = 1.0f, pccMaxDist = 2.0f;
    /// THE SCENE'S PLANAR MIRRORS (PHOTON-SCENE-SWITCH-2). Bound per pass like the
    /// arms above (`cameraMatches` then gates on THIS scene's reflected cameras) AND
    /// at HASH time per renderable — ScenePbs::calculateHashForPreCreate binds the
    /// record of the renderable's own SceneManager, so a mirror's hash is decided
    /// against the PlanarReflections that tracks it, never the last scene to arm.
    Ogre::PlanarReflections            *planar = nullptr;
    /// `passBuf.envMapNumMipmaps` for this scene's passes (PHOTON-SCENE-SWITCH-2):
    /// the mip count of what the env-probe slot holds IN THIS SCENE — the bound
    /// grid's array, else the largest of the scene's bound reflection cubes (the
    /// sky's, an authored map's) — never a process-wide maximum. Resolved once per
    /// frame (OgreScene::resolveIblMipmaps), read per pass.
    float iblMipmaps = 1.0f;
    /// THE SCENE'S LIGHT DIRT (D3-HIT-SHADE-2): the hit decode's world light list
    /// (HlmsAtom::uploadWorldLights) is rebuilt only when one of these moved —
    /// the light write serial (every setLight push, a light node's pose or
    /// visibility write, a drained ChangeSet that moved a light, the environment) and the
    /// count of lights destroyed. Point at the owning OgreScene's counters.
    const unsigned long long *lightWriteSerial = nullptr;
    const unsigned long long *lightsRemoved = nullptr;
    /// THE SCENE'S ONE LIGHT LIST (P1C-LIGHT-LIST, SceneLightList.h): what the voxel
    /// injection, the card relight and the hit decode select their lights from.
    /// Points at the owning OgreScene's list (dirtied by the two counters above).
    const SceneLightList *lightList = nullptr;
};
/// The record of the scene bound on `sm` (sceneOfManager) — null for a SceneManager no scene holds.
/// What giStatus reports as "bound" is read through this, the same lookup the pass
/// makes (OgreGi.cpp).
const SceneGiBinding *sceneGiBindingOf(const Ogre::SceneManager *sm);
/// (`bindSceneGi`, the per-pass call every PBS-family host makes, is declared in
/// HlmsAtom.h beside tellEveryHlms.)
/// An arm is about to be DELETED: any PBS-family host still holding it (the last
/// pass of the last frame bound it) lets go now, so no read between frames — a
/// getter, `resetIblSpecMipmap(0)` walking the bound PCC — meets a freed object. The
/// Photon volumes are dropped from the listener's per-host pass state the same way.
void forgetGiArms(Ogre::HlmsManager *manager, const Ogre::PhotonVoxelLighting *vct,
                  const Ogre::PhotonIrradianceField *ifd,
                  const Ogre::ParallaxCorrectedCubemapBase *pcc,
                  const Ogre::PlanarReflections *planar = nullptr);

/// THE REGISTERED HLMS_PBS: upstream's HlmsPbs, plus the per-pass binding above.
/// Our derived Hlms (the Terra pattern) and not a patch: the two overrides are
/// public virtuals and the three setters public API.
/// engine.tlas_compute's reading (ATOM-CPU-WALKS-1, OgreScene::readTlasInstances):
/// the device-written instance array (VkAccelerationStructureInstanceKHR, 64 bytes
/// each, TWO per slot: near at 2 x slot, far at 2 x slot + 1, an untraced slot's
/// pair zero) and what a reference needs to rebuild it — the address of every
/// bottom-level structure by (mesh, level), the ready rigged slots by NodeId, the
/// far overlap, the active counts and the per-slot rows.
struct TlasReadback {
    unsigned slots = 0;
    std::vector<unsigned char> instances;
    struct Structure { const void *mesh = nullptr; uint32_t level = 0; uint64_t address = 0; };
    std::vector<Structure> structures;
    struct Skin { uint32_t node = 0; uint64_t address = 0; uint32_t row = 0; };
    std::vector<Skin> skins;
    float farOverlap = 0.0f;
    unsigned instanceCount = 0, farInstanceCount = 0;
    std::vector<uint32_t> rows;
};

class ScenePbs final : public Ogre::HlmsPbs {
public:
    ScenePbs(Ogre::Archive *dataFolder, Ogre::ArchiveVec *libraryFolders)
        : Ogre::HlmsPbs(dataFolder, libraryFolders) {}
    void analyzeBarriers(Ogre::BarrierSolver &barrierSolver,
                         Ogre::ResourceTransitionArray &resourceTransitions,
                         Ogre::Camera *renderingCamera, const bool bCasterPass) override;
    Ogre::HlmsCache preparePassHash(const Ogre::CompositorShadowNode *shadowNode, bool casterPass,
                                    bool dualParaboloid, Ogre::SceneManager *sceneManager) override;

    /// THE PBS CHANGE LOG (ATOM-CPU-WALKS-1). Every edit that can move what an item
    /// wearing a PBS datablock IS to the engine — its permutation, its blend or cull
    /// state, its alpha test, its baked texture set — reaches Ogre's renderables
    /// through ONE door: `HlmsDatablock::flushRenderables` (or a renderable's own
    /// `setDatablock`) re-hashes them with `calculateHashFor`, a public virtual. So
    /// this is where the engine hears about an in-place edit, instead of polling one
    /// witness item per material every frame (W13: 18 ms still at 10k materials).
    /// A note is the renderable's owner (an Item; null for anything else) and its
    /// datablock, kept until the frame's drain (OgreEngine::drainPbsChanges). Both
    /// pointers are KEYS: neither is dereferenced after the call that noted it.
    struct HashNote {
        const Ogre::MovableObject *owner = nullptr;
        const Ogre::HlmsDatablock *db = nullptr;
    };
    void calculateHashFor(Ogre::Renderable *renderable, Ogre::uint32 &outHash,
                          Ogre::uint32 &outCasterHash) override;
    const std::vector<HashNote> &hashNotes() const { return mHashNotes; }
    /// THE LOG IS BOUNDED: a process that re-hashes more than this between two
    /// drains (a headless boot never draws, so it never drains; a load of a hundred
    /// thousand items in one frame) stops noting and says so — the drain then treats
    /// EVERY datablock and every item as re-hashed, once, which is correct and O(N).
    static constexpr size_t kMaxHashNotes = 65536u;
    bool hashNotesOverflowed() const { return mHashNotesOverflow; }
    void clearHashNotes() {
        mHashNotes.clear();
        mHashNotesOverflow = false;
    }
    /// ...AND THE EDITS NOT HASHED YET: a texture or sampler swap moves the
    /// datablock's pointers at once and its hash only at the next pass' upload
    /// (ConstBufferPool::uploadDirtyDatablocks -> updateDescriptorSets ->
    /// flushRenderables). The pool's dirty list is PROTECTED state of our own base
    /// class — read here, never written. Every entry is one of this Hlms' datablocks.
    template <class F> void forEachDirtyDatablock(F &&f) const {
        for (Ogre::ConstBufferPoolUser *u : mDirtyUsers)
            f(static_cast<const Ogre::HlmsPbsDatablock *>(u));
    }
protected:
    /// HASH TIME (PHOTON-SCENE-SWITCH-2): HlmsPbs reads the planar pointer here
    /// per renderable, outside any pass (OgreHlmsPbs.cpp:1081-1084) — so the
    /// renderable's OWN scene's PlanarReflections is bound for the call and the
    /// pass's pointer put back after it.
    void calculateHashForPreCreate(Ogre::Renderable *renderable, Ogre::PiecesMap *inOutPieces) override;
private:
    std::vector<HashNote> mHashNotes;
    bool mHashNotesOverflow = false;
};

/// Returns an index buffer that belongs to no VAO to the VaoManager that made it
/// (MeshRec::clusterStream). At namespace scope, not nested in the record: a
/// nested deleter is not yet default-constructible where the record's own
/// implicit constructor is formed.
struct IndexBufferRelease {
    Ogre::VaoManager *vaoManager;
    IndexBufferRelease() : vaoManager(nullptr) {}
    explicit IndexBufferRelease(Ogre::VaoManager *m) : vaoManager(m) {}
    void operator()(Ogre::IndexBufferPacked *b) const;
};

class OgreScene final : public Scene, public inspect::SceneEcho {
public:
    /// The engine that made this scene: its own pointer and the boundary's identity (Scene::engine), set once.
    void bindEngine(OgreEngine *e);
    OgreScene(Ogre::Root *root, Ogre::SceneManager *sm, const std::string &name,
              std::string &errorSink);
    /// The manager of a scene the host made through a create message (U3 stage 4a): bound at the message's drain.
    void adoptManager(Ogre::SceneManager *sm);
    /// THE TWO GRID STATES DRAWN BETWEEN (U3 stage 5; decisions 55-56): the newest step's moved nodes set to
    /// lerp(prev, cur, alpha) — ONE write per node per frame, the whole apply on the scene's pool — and the previous
    /// step's nodes that were not re-recorded put back on cur once; every node written is named to the scans and the
    /// GPU-scene walk through the drain's own door (nameMovedSubtrees). A node already drawn at cur is never written
    /// again (a lockstep frame's apply wrote it): lockstep frames write and name nothing here.
    void interpolate(float alpha);
    void restoreInterpolated() { interpolate(1.0f); }
    unsigned long long interpolationStep() const { return mInterpStep; }
    std::vector<NodeId> mInterpNodes;   ///< the newest step's nodes (each once)
    std::vector<NodeId> mInterpStale;   ///< the step before's, owed cur where not re-recorded (by id: after a removal)
    unsigned long long mInterpStep = 0;
    unsigned long long mInterpFrame = 0;   ///< the engine frame this scene was last interpolated in (once a frame)
    bool mInterpRecsValid = true;       ///< mInterpRecs resolves mInterpNodes (false after a removal, until a step)
    float mInterpDrawnAlpha = 1.0f;
    double mInterpMicros = 0.0;       ///< the last interpolate's writes (the apply)
    double mInterpNameMicros = 0.0;   ///< ...and its naming of them to the scans (decision 56)
    unsigned mInterpNodesLast = 0;
    /// THE HOST'S SIDE OF A SCENE COMMAND (U3 stage 4a, decision 39): posted as a message of this scene's stream
    /// (Engine::post) when the host's calls are records; false on the engine's side, where the command applies.
    bool hostPosts() const;
    bool hostRecordsOn() const { return hostPosts(); }
    template <class F> bool postScene(F &&body) {
        if (!hostPosts()) return false;
        postSceneMessage(SceneMessage{ [b = std::forward<F>(body)](Scene &s) mutable -> bool {
            return b(static_cast<OgreScene &>(s)); }, 0 });
        return true;
    }
    void postSceneMessage(SceneMessage &&m);
    void requestOnEngine(const std::function<void()> &f);   ///< Engine::request on this scene's engine (U3 4a)
    ~OgreScene() override;


    void setAmbient(const Colour &upper, const Colour &lower) override;
    /// A HOST'S OWN AMBIENT: it takes the ambient back from the engine (a later
    /// setEnvironmentLight hands it over again — see mSkyAmbientOwned).
    void setAmbientSh(const float sh[27]) override;
    /// The one writer behind both: the pass-level SH and every GI consumer,
    /// staling the probe grid with `why`.
    void applyAmbientSh(const float sh[27], GiStaleReason why);
    void setEnvironmentLight(const Colour &gain) override;

    void setFog(const FogDesc &desc) override;
    /// Creates the scene's atmosphere component (JahAtmosphere, Atmosphere.h)
    /// or destroys it. It serves two customers — the sky (the planet's
    /// atmosphere) and the World fog under ANY sky — and is registered on the
    /// SceneManager while either wants it (syncAtmosphere). Destroying is what
    /// makes "fog off, no atmosphere" bit-exact: no component means no hlms_fog
    /// property, which means the fog code is not in the shader at all.
    /// destroyAtmosphere() MUST run before the SceneManager dies (the component
    /// destroys its quad through it).
    void ensureAtmosphere();
    void destroyAtmosphere();
    /// Re-derives the fog from the last pushed FogDesc and the sky: the
    /// per-scene FogState the shader reads (colour mode, height layer) and the
    /// component's fog block (the World fog's density and breakthrough pair).
    /// Called by setFog and by syncAtmosphere.
    void pushFogState();
    JahAtmosphere *mAtmosphere = nullptr;
    /// Once a frame, inside it, before the sky capture (OgreEngine::
    /// renderOneFrame): the component's dirty tables, its buffer, its quad.
    void updateAtmosphere();
    /// The scene's driving camera's world height (the GI driver's rule), once a
    /// frame: the atmosphere's observer altitude, banded (Atmosphere.h).
    void noteAtmosphereObserver(float cameraY);
    /// THE MEDIA A RAY CROSSES THAT DID NOT START AT THE EYE (PHOTON-I-1 fix 5;
    /// src/rayquery/include/jah_fog_along.glsl states the layout): the World fog,
    /// its height layer, the air and the height fog exactly as this scene's PBS
    /// colour passes read them (the fog state, the atmosphere's const buffer and
    /// whether the passes compile each medium in), as eight vec4; `aerial` is the
    /// air's table when the passes read it, else null.
    void fogAlong(float out[8][4], Ogre::TextureGpu *&aerial) const;
    /// A one-texel "no air" volume (RGBA 0, 0, 0, 1: no in-scatter, full
    /// transmittance) a low-level material binds where the air's table would be —
    /// the cloud sheet's and the screen reflection's resolve. Made on first ask.
    Ogre::TextureGpu *noAirVolume();
    /// The observer altitude (km) the environment is captured from: a point of the
    /// octave lattice 50 m x 2^n (noteAtmosphereObserver), 50 m for every observer
    /// under it (the capture then photographs from the ground's 2 m).
    float mAtmoCapturedObserverKm = 0.05f;
    /// Scene::setReflectionFogEnabled's door (fogAlong answers no medium when shut).
    bool mReflectionFogOn = true;
    /// The altitude (km) the capture photographs the sky from: the ground's 2 m
    /// for the 50 m lattice point, else the lattice point itself.
    float environmentObserverKm() const;
    /// THE CAMERA AT REST (REOPEN-SKY-1, noteAtmosphereObserver): the altitude it
    /// was last seen moving from, and the frames it has stayed within 1 mm of it.
    float mAtmoRestAnchorY = 0.0f;
    unsigned mAtmoRestFrames = 0;
    bool mAtmoRestAnchored = false;

    /// Ogre's OWN sky (SceneManager::setSky): a full-screen Rectangle2D at the far
    /// plane whose camera-direction shader samples an equirect or cube texture.
    /// There is no sky geometry of ours any more — no sphere, no six quads, no
    /// per-frame follow-the-camera. The equirect method needs a texture whose
    /// INTERNAL type is Type2DArray (automatic-batching pool slices are; our
    /// pixel-uploaded ManualTextures are not) — makeSkyArrayTexture() copies when
    /// it must. And it needs fork c290052de (was 0009): upstream's Vulkan GLSL declares
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
    /// THE PLANET'S ATMOSPHERE (AtmosphereSky) as the scene's sky: the
    /// component's model, sun and quad. The component is the same instance the
    /// FOG uses — one per scene — so the two halves negotiate through
    /// mAtmoSkyOn/mAtmoFogOn rather than each tearing it down behind the
    /// other's back.
    bool applySkyAtmosphere(const AtmosphereSky &sky);
    /// Registers/unregisters the component on the SceneManager, shows or hides
    /// its quad and hands its tables to the pass from the two flags. The
    /// registration is what sets hlms_fog, so "no fog and no atmosphere" must
    /// leave it unregistered.
    void syncAtmosphere();
    bool mAtmoSkyOn = false;   // the planet's atmosphere is the scene's sky
    bool mAtmoFogOn = false;   // the World fog is on
    /// THE HEIGHT FOG (HeightFogDesc; SKY-DEFAULTS-1) — the component's THIRD
    /// customer: registered while it is on, whatever the sky and the World fog
    /// say. pushHeightFog hands the component the dials and the colour in force
    /// (the sky's SH at +Y), and runs again whenever a new SH lands.
    void setHeightFog(const HeightFogDesc &desc) override;
    HeightFogStatus heightFogStatus() const;
    void pushHeightFog();
    HeightFogDesc mHeightFog;
    bool mHeightFogOn = false;
    /// The air the sun's beam crosses, for the snapshot (PublishedFrame::atmosphere; the inputs of the stateless
    /// atmosphereSunTint, ENGINE-UPGRADE-U3 F-A).
    PublishedFrame::Atmosphere publishedAtmosphere() const;
    AtmosphereStatus atmosphereStatus() const;
    /// PublishedFrame::environmentPending: a capture, an SH ticket, a convolution or a landed-but-unswapped set is owed.
    bool environmentPending() const;
    void setReflectionFogEnabled(bool on) override {
        if (postScene([=](OgreScene &s) { s.setReflectionFogEnabled(on); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::setReflectionFogEnabled"); mReflectionFogOn = on; }

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
    /// hands the cube to buildReflectionCubemapFrom and convolves it at once.
    /// Called inside the frame, after updateSceneGraph and before any draw.
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
    void integrateSkyShFromCube(Ogre::TextureGpu *cube);
    void integrateSkyShNow(Ogre::TextureGpu *cube);
    void issueSkyShRead(Ogre::TextureGpu *cube);
    /// `out` = the 27 coefficients (the scene's mSkySh unless told otherwise).
    void integrateSkyShFromBox(const Ogre::TextureBox &box, float *out = nullptr);
    /// Renders the capture workspace into a new cube (OgreSky.cpp says who).
    Ogre::TextureGpu *renderSkyCaptureCube(const char *prefix, Ogre::uint32 size, bool mips);
    /// Called at the top of every frame this scene is drawn in: counts the
    /// gesture's clock and, if the pending read has landed, integrates it.
    /// Never blocks.
    void pollSkyShRead();
    /// Once per drawn frame (OgreEngine, beside pollSkyShRead): advances the
    /// layer's own clock by the frame delta the host pushed, writes the scroll
    /// into the layer, the disc and the ground shadow, and runs the capture
    /// cadence while the layer scrolls.
    void updateCloudLayer();
    /// ...and the one per-frame advance of the layer's clock and capture
    /// cadence (renderOneFrame only), which then pushes the constants.
    void tickCloudClock();
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
    /// The sky's ambient, 9 SH bands x 3 channels: the coefficients of the
    /// environment IN FORCE (mSkyShInForce), never a newer integral whose cube
    /// has not landed yet. The host scales them by its Sky Light.
    bool skyAmbientSh(float out[27]) const;
    /// The LATEST integral (the capture's or the deferred read's). Valid only
    /// while mSkyShValid; mSkyShFresh = it has not been swapped in yet (the two
    /// are set together, and forgetSkySh clears both).
    bool  mSkyShValid = false;
    bool  mSkyShFresh = false;
    float mSkySh[27] = { 0.0f };
    /// THE ENVIRONMENT IS ONE SET (PHOTON-SKY-TRANSIENT-1): the reflection cube,
    /// these coefficients and the Sky Light's gain reach the pixel together. A
    /// sky change builds the next cube into mReflPendingTex and integrates the
    /// next SH into mSkySh while the previous set stays bound; when the capture,
    /// the convolution and the SH have ALL landed, landEnvironmentIfComplete
    /// swaps the cube and the coefficients in one step. Never a partial set,
    /// never a cube nothing has written bound to a datablock.
    bool  mSkyShInForceValid = false;
    float mSkyShInForce[27] = { 0.0f };
    Ogre::TextureGpu *mReflPendingTex = nullptr;
    void landEnvironmentIfComplete();
    /// THE CUBES A SWAP REPLACED (SKY-SWAP-1): kept alive until no holder
    /// listens to them, so no draw of the change frame samples a destroyed cube
    /// (Ogre's blank texture — the presented black flash). See
    /// reapRetiredReflections; `force` destroys them all (the sky is going).
    /// THE VRAM BOUND: a swap lands at most once per frame per scene, and a
    /// retired cube lives at most kRetiredReflectionMaxFrames drawn frames, so
    /// the list holds at most 120 cubes x ~1.05 MB (a 128^2 RGBA16F cube with
    /// its mips) = ~126 MB per scene — the worst case, a datablock that never
    /// rebakes during a sky drag that re-captures every frame. Ordinarily every
    /// holder lets go within the reap's 2-frame floor, i.e. 1-3 cubes (1-3 MB).
    struct RetiredReflection { Ogre::TextureGpu *tex = nullptr; unsigned frames = 0u; };
    std::vector<RetiredReflection> mRetiredReflections;
    static constexpr unsigned kRetiredReflectionMaxFrames = 120u;
    static constexpr unsigned kRetiredJobFrames = 4u;
    void reapRetiredReflections(bool force);
    /// Drops the next set's cube (and its owned convolution source) unlanded.
    void destroyPendingReflection();
    /// No sky, no sky light: every SH this scene holds, the one in force too.
    void forgetSkySh();
    /// The ambient the pixel reads: the SH in force x mEnvLightGain, or zeros —
    /// only while the engine owns the ambient (mSkyAmbientOwned).
    void applySkyAmbient(GiStaleReason why);
    /// WHO WRITES THE AMBIENT. setEnvironmentLight hands it to the engine (a
    /// host modelling a Sky Light: SceneMirror, the previews); setAmbient /
    /// setAmbientSh take it back (a host lighting its scene with a colour of its
    /// own, which a sky capture must not overwrite).
    bool mSkyAmbientOwned = false;

    /// THIS SCENE'S SHADOW REQUEST (ShadowDesc). The backend's filter and atlas
    /// are one per PROCESS, so all this does is apply the scene's resolved
    /// answer to the global state and drop a push that asks for what is already
    /// in force — which is exactly the read-before-write guard every host used
    /// to hand-write around Engine::setShadowFilter/setShadowResolution.
    void setShadowSettings(const ShadowDesc &desc) override;
    ShadowDesc mShadowDesc;
    /// The engine that made this scene — the owner of the global shadow state
    /// above. Never null for a scene created through Engine::createScene().
    OgreEngine *mEngine = nullptr;
    /// Builds the NEXT GGX-prefiltered reflection cubemap (mReflPendingTex) by
    /// convolving `srcCube`; the cube in force stays bound until the whole next
    /// environment set has landed (landEnvironmentIfComplete). `ownsSource`
    /// means the source is ours to destroy once the convolution has run (the
    /// cubemap-sky path passes false: there the source IS the sky texture).
    /// The prefilter runs in applyPendingIbl: inside the capture's own frame for
    /// a captured sky, at the top of the next frame for a host-pushed cube.
    void buildReflectionCubemapFrom(Ogre::TextureGpu *srcCube, bool ownsSource);
    /// Unbinds and destroys the reflection cubemap and any pending next one.
    void destroyReflection();
    /// Binds (or clears, when mReflectionTex is null) the scene's sky reflection
    /// cubemap on every PBR material's datablock. (Body in complete-class context,
    /// so it may call the private impl declared further down.)
    void applyReflectionToAll();
    /// "Do THIS scene's passes bind a probe grid" — which is what the env-probe
    /// slot's occupancy turns on in its passes. See OgreSky.cpp's long note, THE
    /// ENV-PROBE SLOT HAS ONE OCCUPANT.
    bool probeGridBound() const { return mGiBinding.pcc != nullptr; }
    /// THE ROUGHNESS-TO-LOD MAP'S CHAIN LENGTH FOR THIS SCENE'S PASSES
    /// (SceneGiBinding::iblMipmaps). Every site that changes what a datablock's
    /// env-probe slot holds — or whether a grid is bound — marks it; the frame
    /// head (OgreEngine::renderOneFrame, per drawn scene) resolves it with one
    /// walk of the materials. Body in OgreSky.cpp.
    void markIblMipmapsDirty() { mIblMipmapsDirty = true; }
    void resolveIblMipmaps();
    bool mIblMipmapsDirty = true;
    /// True for the duration of destroy().
    bool mDestroying = false;
    /// Runs the queued ibl_specular convolution (roughness mip chain) into the
    /// next set's cube, then lands the set if nothing else is owed. Called once
    /// per frame by the engine, like applyPendingGi, and by the capture itself.
    void applyPendingIbl();
    void convolvePendingIbl();
    Ogre::TextureGpu *mReflectionTex = nullptr;   // prefiltered cube on PBSM_REFLECTION
    /// THE ONE ENVIRONMENT AS THE RAY JOBS BIND IT (PHOTON-ENV-1): the cube (null
    /// while there is none or the Sky Light is out) and ONE colour whose meaning
    /// follows it — with the cube, the environment light's gain on it; without,
    /// the environment's flat radiance (the SH's constant band, gain applied).
    /// jah_rq_hit.glsl's JAH_SKY_COLOUR contract.
    struct RayEnvironment {
        Ogre::TextureGpu *cube = nullptr;
        float colour[3] = { 0.0f, 0.0f, 0.0f };
    };
    RayEnvironment rayEnvironment() const;
    /// The environment light's gain per channel (Scene::setEnvironmentLight):
    /// what every escape multiplies the environment cube by (the pass's `jahEnv`
    /// slot, the bounce job, the ray jobs).
    Colour mEnvLightGain { 1.0f, 1.0f, 1.0f, 1.0f };
    /// ...and its Rec.709 luminance, the one number HlmsPbs' own env-probe
    /// sample can carry: it rides `ambientUpperHemi.w` (envmapScale), which
    /// scales everything in the env-probe slot. At exactly 1.0 HlmsPbs does not
    /// even set the `envmap_scale` shader property.
    float mEnvLightScale = 1.0f;
    void destroySky();
    bool removeNode(NodeId id) override;

    // ---- Hierarchy and transforms ----
    NodeId createNode(NodeId parent = 0, NodeId id = 0) override;
    NodeId createDocumentNode(NodeId id, NodeId parent) override;
    const PublishedFrame &published() const override { return mPublished.read(); }
    /// The render side's publish of the frame's buffer, under snapshotLock() (RenderThread::publish: the set at once).
    void commitPublishedLocked() { mPublished.commitLocked(); }
    /// THE EXTRACT'S APPLY (ENGINE-UPGRADE-U2): each document node's record — parent, graph class, local TRS — in
    /// order, before the change set names the subtrees. Returns how many records landed.
    unsigned applyNodeRecords(const std::vector<NodeRecord> &records, unsigned long long stepSerial);
    /// The close of a frame the engine drew: what the document reads without asking (PublishedFrame).
    void publishFrame(unsigned long long frame, unsigned long long serial);
    /// Called by the drain after applyNodeRecords: a planar reflector asked for on a DOCUMENT node is measured and
    /// armed at the pose its record just landed (a refusal names itself in the error sink).
    void armDeferredReflectors();
    bool setNodeParent(NodeId id, NodeId parent) override;
    void setNodeTransform(NodeId id, const Vec3 &pos, const Quat &rot, const Vec3 &scale) override;
    void setNodeVisible(NodeId id, bool visible) override;
    void setNodeVisibleUnder(NodeId id, bool visible, bool parentShown) override;

    // ---- Meshes and materials ----
    MeshId createMesh(const MeshData &data, MeshId id = 0) override;
    /// A MULTI-SUBMESH ITEM, for the suites that hold a per-submesh consumer to
    /// it (REFLECT-MOVERS-2: the ray tier's cut-out table is per submesh). No
    /// product path builds one — buildMeshV2 makes one submesh a mesh — so these
    /// live on the engine-private scene, never on the Scene boundary.
    /// `appendSubmesh` MOVES `from`'s submesh 0 (every LOD's VAOs, its shadow
    /// VAOs) into `into` as its next submesh and grows `into`'s bounds; both
    /// meshes unattached, the same LOD count. `from` is left empty (destroy it).
    bool appendSubmesh(MeshId into, MeshId from);
    /// One sub-item's material (setNodeMaterial sets all of them).
    bool setSubItemMaterial(NodeId node, unsigned subItem, MaterialId mat);
    bool updateMeshVertices(MeshId id, const std::vector<float> &positions,
                            const std::vector<float> &normals) override;
    bool destroyMesh(MeshId id) override;
    MaterialId createPbrMaterial(const PbrParams &p, MaterialId id = 0) override;
    bool setPbrMaterial(MaterialId id, const PbrParams &p) override;
    bool setShadingModel(MaterialId id, ShadingModel model) override;
    bool destroyMaterial(MaterialId id) override;
    bool setMaterialCustomPiece(MaterialId id, const std::string &path,
                                CustomPieceStage stage) override;
    void setShaderTime(float seconds) override;
    /// TORNADO-1: writes every scrolling material's offset (velocity x clock,
    /// wrapped) into its datablocks' userValue[2].zw, master and cull twins.
    void applyScrollOffsets();
    float shaderTime() const override;
    /// ATOM stage 1: the scene-wide LOD dial (OgreMesh.cpp).
    void  setLodBias(float bias) override;
    float lodBias() const override { return mLodBias; }
    /// ATOM-3 stage 1: the view's pixel-error threshold (Engine.h); read by the id pass's
    /// request and by every view bound to the scene (OgreView::syncLodTolerance).
    void  setPixelTolerance(float pixels) override;
    float pixelTolerance() const override { return mPixelTolerance; }
    void  objectLods(std::vector<ObjectLodDesc> &out) const override;
    bool  meshVaoShape(MeshId mesh, unsigned &levels,
                       unsigned &shadowIndependent) const override;
    /// ATOM stage 2: a mesh's CLUSTER STREAM and the DAG tables it indexes, as
    /// the engine holds them (MeshRec::clusterStream). ENGINE-PRIVATE, not on the
    /// Scene interface: its consumer is stage 3's cut, and until then the cluster
    /// suites' harness (tests/atom/cluster_draw.*) reads it to prove the upload.
    /// False for an unknown mesh; `stream` null for a mesh with no DAG.
    struct ClusterStreamView {
        Ogre::Mesh *mesh = nullptr;
        Ogre::IndexBufferPacked *stream = nullptr;
        const std::vector<MeshCluster> *clusters = nullptr;
        const std::vector<MeshClusterGroup> *groups = nullptr;
    };
    bool clusterStreamOf(MeshId mesh, ClusterStreamView &out) const;
    /// Writes `errors` (level 1 first, a length in mesh units each) into
    /// `mesh`'s LOD value array at the current bias. fork 5230c9390 (was 0059) added the
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
    bool sharesSkeleton(NodeId id) const;
    bool attachToBone(NodeId rider, NodeId owner, const std::string &bone,
                      const Vec3 &position, const Quat &rotation, const Vec3 &scale) override;
    bool detachFromBone(NodeId rider, NodeId parent) override;
    bool setBoneAttachmentOffset(NodeId rider, const Vec3 &position, const Quat &rotation,
                                 const Vec3 &scale) override;
    NodeId boneAttachment(NodeId rider, std::string *bone = nullptr) const;
    /// Copies every follower's pose from its source. Once per rendered frame,
    /// AFTER the frame, so the poses copied are the ones just drawn.
    void applySkeletonFollowers();
    bool hasSkeleton(NodeId id) const override;
    std::vector<std::string> boneNames(NodeId id) const override;
    bool setBonePoses(NodeId id, const BonePose *poses, size_t count) override;
    bool boneMatrices(NodeId id, float *out, size_t count) const override;
    size_t streamedBoneCount(NodeId id) const;
    RigStats rigStats() const;
    SceneChangeStats changeStats() const { return mChangeStats; }
    void beginStructureEdit() override {
        if (postScene([=](OgreScene &s) { s.beginStructureEdit(); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::beginStructureEdit"); ++mStructureEditDepth; }
    void endStructureEdit() override;

    // ---- Clips (ANIMATION_ENGINE_MIGRATION_SPEC; impl in OgreClips.cpp) ----
    bool attachClips(NodeId id, const ClipDesc *clips, size_t count) override;
    std::vector<std::string> clipNames(NodeId id) const;
    std::vector<PublishedFrame::Facts::Clip> clipFacts(NodeId id) const;   ///< the facts block's clips (S16-checked)
    bool setClipStates(NodeId id, const ClipState *states, size_t count) override;
    bool setBoneManual(NodeId id, const std::string &bone, bool manual) override;
    bool bonePoses(NodeId id, BonePose *out, size_t count) const override;
    bool publishBonePoses(NodeId node, bool on) override;
    /// The rigs whose poses every frame's snapshot carries (publishBonePoses), in subscription order.
    std::vector<NodeId> mPublishedRigs;
    std::vector<float> clipBoneWeights(NodeId id, const std::string &clip) const override;

    // ---- Textures ----
    TextureId loadTexture(const std::string &path, bool srgb, TextureId id = 0) override;
    TextureId createTexture(unsigned w, unsigned h, const unsigned char *rgba, bool srgb,
                            bool mipmaps = false, TextureId id = 0) override;
    TextureId createCubemap(const TextureId faces[6], TextureId id = 0) override;
    bool updateTexture(TextureId id, unsigned w, unsigned h, const unsigned char *rgba) override;
    /// The staged RGBA upload (level 0 + the CPU-built mip chain), shared by
    /// createTexture and updateTexture so there is one copy of it.
    void uploadRgbaLevels(Ogre::TextureGpu *tex, unsigned w, unsigned h,
                          const unsigned char *rgba);
    unsigned  textureMipmaps(TextureId) const override;
    bool destroyTexture(TextureId id) override;
    bool setPbrTexture(MaterialId mat, PbrTextureSlot slot, TextureId texId) override;

    // ---- Overlay primitives ----
    MaterialId createUnlitMaterial(const Colour &c, bool depthTest, bool wireframe = false, MaterialId id = 0) override;
    MaterialId createOutlineMaterial(const Colour &c, bool skinnable = false, MaterialId id = 0) override;
    bool setUnlitMaterial(MaterialId id, const Colour &c) override;
    MeshId createLineMesh(const std::vector<Vec3> &points, bool strip, MeshId id = 0) override;

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
    unsigned particleCount(NodeId id) const;
    unsigned particleDefinitionsCreated() const { return mParticleDefsCreated; }

    // ---- Lights ----
    bool setLight(NodeId id, const LightDesc &d) override;
    bool removeLight(NodeId id) override;
    unsigned long long lightWriteSerial() const { return mGiLightWriteSerial; }
    /// Lights destroyed so far (removeLight, releaseNode): the dirt a light list's reader pairs with
    /// lightWriteSerial (OgreGi's sceneLightDirt; the ray tier's hit table, decision 14.1).
    unsigned long long lightsRemoved() const { return mLightsRemoved; }

    // ---- Decals (DECALS_SPEC.md; impl in OgreDecals.cpp) ----
    bool setDecal(NodeId id, const DecalDesc &d) override;
    bool removeDecal(NodeId id) override;
    TextureId loadDecalTexture(const std::string &path, DecalMap kind, TextureId id = 0) override;
    unsigned decalAtlasCapacity(DecalMap kind) const;
    unsigned decalAtlasUsed(DecalMap kind) const;

    bool setGlobalIllumination(const GiParams &p) override {
        if (postScene([=](OgreScene &s) { return s.setGlobalIllumination(p); })) return true;   // U3 4a: a message
        return mPhoton.setGlobalIllumination(p);
    }
    bool setGiTuning(const GiParams &p) override {
        if (postScene([=](OgreScene &s) { return s.setGiTuning(p); })) return true;   // U3 4a: a message
        return mPhoton.setGiTuning(p);
    }
    void refreshGlobalIllumination(GiRefreshReason reason) override {
        if (postScene([=](OgreScene &s) { s.refreshGlobalIllumination(reason); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::refreshGlobalIllumination"); return mPhoton.refreshGlobalIllumination(reason); }
    /// OPEN_COVER_SPEC §2 A — see the boundary's note. Sticky, per scene.
    void setLoading(bool loading) override {
        if (postScene([=](OgreScene &s) { s.setLoading(loading); return true; })) return;   // U3 4a: a message
        mSceneLoading = loading;
    }
    bool isLoading() const override { return mSceneLoading; }
    GiStatus giStatus() const { return mPhoton.giStatus(); }
    GiVoxelStats giVoxelStats(int cascade) override {
        if (hostPosts()) { GiVoxelStats r{}; requestOnEngine([&] { r = giVoxelStats(cascade); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::giVoxelStats"); return mPhoton.giVoxelStats(cascade); }
    bool giFieldAtlas(GiFieldAtlas &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = giFieldAtlas(out); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::giFieldAtlas"); return mPhoton.giFieldAtlas(out); }
    bool giVoxelVolume(int cascade, GiVoxelVolume &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = giVoxelVolume(cascade, out); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::giVoxelVolume"); return mPhoton.giVoxelVolume(cascade, out); }
    bool giVoxelLight(int cascade, GiVoxelVolume &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = giVoxelLight(cascade, out); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::giVoxelLight"); return mPhoton.giVoxelVolume(cascade, out, true); }
    bool giFeedbackBricks(int cascade, GiFeedbackBricks &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = giFeedbackBricks(cascade, out); }); return r; }
        JAH_HOST_CALL("Scene::giFeedbackBricks"); return mPhoton.giFeedbackBricks(cascade, out); }
    bool giChangedRegions(GiChangedRegions &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = giChangedRegions(out); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::giChangedRegions"); out = mPhoton.changedRegions(); return out.available; }
    bool takeGiBrickMask(int cascade, GiBrickMask &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = takeGiBrickMask(cascade, out); }); return r; }   // a REQUEST (U3 4b)
        JAH_HOST_CALL("Scene::takeGiBrickMask"); return mPhoton.takeGiBrickMask(cascade, out); }
    /// THE CHANGED-REGION LIST'S CLOSE (PHOTON-7 stage 3): once a frame per drawn scene, before the frame's submit
    /// (renderOneFrame) - every producer of the frame has appended by then.
    void closeGiRegions() { mPhoton.closeRegions(); }
    /// ...and the ray tier's producer (OgreEngine::listRayMovers, after the GPU scene's commit and the skin pass): one
    /// moved or posed slot's swept box, and whether it casts shadows.
    void listGiMover(const Ogre::Aabb &swept, bool casts) { mPhoton.listMover(swept, casts); }
    /// THE SKY'S PEAK CHANNEL as the GI reads it (the list's restart test): the ambient SH's constant band - the sky
    /// lit by its Sky Light, or the host's own ambient - 0 before one was given.
    float ambientPeakForGi() const {
        return mAmbientShKnown ? std::max(mLastAmbientSh[0], std::max(mLastAmbientSh[1], mLastAmbientSh[2])) : 0.0f;
    }
    /// THE RAY TIER'S READING for this scene (PHOTON_SPEC §7 R1). Defined in
    /// OgreRayQuery.cpp — like the tier's own members, so that not one line
    /// of the ray tier lives in a TU that does not include Vulkan.
    RayQueryStatus rayQueryStatus() const;
    AtomDrawStatus atomDrawStatus() override;
    /// THE CUT PROBE (ATOM-2 stage 1; OgreAtomDensity.cpp).
    bool armAtomCutProbe() override;
    bool atomCutProbe(AtomCutProbe &out) override;
    void disarmAtomCutProbe() override;
    /// ...the arming, TAKEN by the first view of this scene whose next frame draws the id pass (the view's atom
    /// listener): true once, for the frame that records it.
    bool takeAtomCutProbeArm() {
        const bool armed = mAtomCutProbeArmed;
        mAtomCutProbeArmed = false;
        return armed;
    }
    bool shadowMapDigests(std::vector<ShadowMapDigest> &out) override;
    bool shadowCasterRoutes(std::vector<ShadowRouteShare> &out) override;
    bool coverageSnapshot(unsigned slot) override;
    bool coverageCompare(float viewRelDepth, float mapDepthTexels, bool maskedOnly, CoverageCompare &out) override;
    void setAtomDrawEnabled(bool on) override;
    void setAtomOcclusionEnabled(bool on) override;
    void setShadowReceiverCull(bool on) override;
    bool shadowReceiverCull() const { return mShadowReceiverCull; }
    bool mShadowReceiverCull = true;
    void setAtomCutBudgetForTest(unsigned indices) override;
    bool atomOcclusionEnabled() const override { return mAtom.occlusionEnabled(); }
    void setAtomView(AtomView view) override {
        if (postScene([=](OgreScene &s) { s.setAtomView(view); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::setAtomView"); mAtom.setView(view); }
    AtomView atomView() const { return mAtom.view(); }
    /// THE BUCKETS VIEW'S TABLE (OgreAtomDraw.cpp): one R32_UINT texel per GPU
    /// scene slot holding the decode bucket that shades it (0 = none), 1024 slots a
    /// row. Written by updateAtomDraw ONLY while the view is Buckets (a CPU walk of
    /// the slots and an upload when a value moved); null until then.
    Ogre::TextureGpu *atomViewTable() const { return mAtom.viewTable(); }
    GpuSceneStatus gpuSceneStatus() const;
    /// THE CUT'S INDEX NEED, the scene's high-water mark (ATOM-CLUSTER-CUT F1): the most
    /// indices any view's cut of this scene asked of its stream (the stats ring's
    /// cursor). Every GpuCull sizes its stream from at least this, so a view born into
    /// a heavy scene — a screenshot, a thumbnail — does not start small and overflow.
    void noteCutIndexNeed(uint32_t indices) { mCutIndexNeed = std::max(mCutIndexNeed, indices); }
    /// ATOM (atom/AtomComponent.h): the scene's split — its doors, the words step, the
    /// Atom view's table, the caster cut's list and pass state, its share of the status.
    AtomComponent &atom() { return mAtom; }
    const AtomComponent &atom() const { return mAtom; }
    /// THE CASTER CUT'S TEST DOOR (shadow.atom_cut; never a mode): arm it for ONE map of
    /// the VIEW kind's shadow node (`map` = the shadow map index, `face` = the cube face of
    /// a point map, 0 otherwise); the next frame that renders that map copies its cut —
    /// the request the recorder built and every drawn cluster — and the read below returns
    /// it once that frame has retired (false before). A read re-arms nothing.
    struct CasterProbe {
        unsigned map = 0, face = 0;
        bool orthographic = false;
        float eye[3] = { 0, 0, 0 };
        float projScaleY = 0, viewportHeight = 0, tolerance = 0;
        float biasScale = 0;              ///< the shadow camera's constant-bias scale
        float depthNear = 0, depthFar = 0;   ///< the map's depth range (the caster's depthRange)
        float viewProj[16] = {};          ///< rows, the cut's (the camera's RS-depth VP)
        float planes[24] = {};            ///< the request's six planes (the cut's per-cluster frustum test)
        unsigned rect[4] = { 0, 0, 0, 0 };   ///< the map's rectangle in its target, texels
        unsigned survivors = 0, overflow = 0, missing = 0;
        unsigned long long triangles = 0ull;
        struct Drawn { unsigned slot = 0, cluster = 0, depth = 0; };   ///< cluster = MESH-LOCAL index
        std::vector<Drawn> drawn;
        /// The same map's LATE half (stage 4: the first half's depth-rejected set re-tested against the map's
        /// own pyramid of that frame), when it ran.
        std::vector<Drawn> lateDrawn;
    };
    void armCasterProbeForTest(unsigned map, unsigned face);
    bool casterProbeForTest(CasterProbe &out);
    uint32_t cutIndexNeed() const { return mCutIndexNeed; }
    uint32_t mCutIndexNeed = 0u;
    /// THE CUT PROBE'S ARMING (ATOM-2; armAtomCutProbe sets it, takeAtomCutProbeArm takes it).
    bool mAtomCutProbeArmed = false;
    bool gpuSceneEntry(unsigned slot, GpuSceneEntry &out) const override;
    bool gpuSceneDeviceEntries(unsigned first, unsigned count,
                               std::vector<GpuSceneEntry> &out) override;
    void measureGpuSceneScan(bool graphIsCurrent) override {
        if (hostPosts()) { requestOnEngine([&] { measureGpuSceneScan(graphIsCurrent); }); return; }   // a REQUEST (U3 4b)
        JAH_HOST_CALL("Scene::measureGpuSceneScan");
        ensureGpuSceneTimed(graphIsCurrent);
    }
    // ---- SURFACE-CACHE phase 2: the capture cache (SurfaceCache.h) --------
    /// THE PER-FRAME PASS, called once per drawn scene from renderOneFrame —
    /// after applyPendingGi (so a material or light edit has already bumped the
    /// signatures this reads) and before Ogre's own workspaces run. It builds
    /// the cache on the first frame `GiParams::cards` is on, tears it down when
    /// the row goes off, and otherwise spends the tier's texel budget.
    void updateSurfaceCache() { mPhoton.updateSurfaceCache(); }
    /// THE CARDS' SCENE FACTS (the surface cache is Photon's: PhotonComponent::updateSurfaceCache).
    void cardSceneFacts(CardSceneView &view) const;
    /// Fills view.candidates with the slots asked for that can hold cards, and view.itemSlots (PHOTON-7 stage 5, 5-2).
    void cardCandidates(const std::vector<uint32_t> &slots, CardSceneView &view);
    bool readCardTexel(NodeId node, unsigned card, float u, float v, CardSample &out) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = readCardTexel(node, card, u, v, out); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::readCardTexel"); return mPhoton.readCardTexel(node, card, u, v, out); }
    bool readCardRadiance(std::vector<CardRadiance> &out, unsigned long long &cacheFrame) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = readCardRadiance(out, cacheFrame); }); return r; }   // a REQUEST (U3 4b)
        JAH_HOST_CALL("Scene::readCardRadiance"); return mPhoton.readCardRadiance(out, cacheFrame); }
    bool readCardFaceDemand(std::vector<CardFaceDemand> &out, unsigned long long &cacheFrame) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = readCardFaceDemand(out, cacheFrame); }); return r; }   // a REQUEST (U3 4b)
        JAH_HOST_CALL("Scene::readCardFaceDemand"); return mPhoton.readCardFaceDemand(out, cacheFrame); }
    bool readCardAt(const Vec3 &world, const Vec3 &normal, CardSample &out, NodeId onlyNode = 0) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = readCardAt(world, normal, out, onlyNode); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::readCardAt"); return mPhoton.readCardAt(world, normal, out, onlyNode); }
    bool dumpCardAtlas(const std::string &prefix, std::string &err) override {
        if (hostPosts()) { bool r{}; requestOnEngine([&] { r = dumpCardAtlas(prefix, err); }); return r; }   // a REQUEST on the host's side (U3 4b)
        JAH_HOST_CALL("Scene::dumpCardAtlas"); return mPhoton.dumpCardAtlas(prefix, err); }
    const SurfaceCache *surfaceCache() const { return mPhoton.surfaceCache(); }
    /// The compile mode the scene's views ask for (View::setAsyncShaders); its own captures
    /// follow it (ASYNC-SHADERS-1).
    void setAsyncShaders(bool on) { mAsyncShaders = on; }
    bool asyncShaders() const { return mAsyncShaders; }
    bool cardMoverFrame(CardMoverFrame &out) { return mPhoton.cardMoverFrame(out); }
    bool traceCardMovers(const CardMoverTrace &job);
    /// THE CARDS' INDIRECT BY TLAS RAYS (PHOTON-7 stage 10, 10-2a): the ray tier's gather of the relight's cards into
    /// the Gather layer, and its counts (CardCacheStatus::gather).
    CardGatherResult gatherCards(const CardGatherTrace &job);
    void cardGatherCounts(CardCacheStatus::GatherCounts &out) const;
    void cardGatherSky(std::vector<std::pair<unsigned, unsigned>> &out);
    /// THE VOXEL SIDES' GATHER BY TLAS RAYS (PHOTON-7 stage 10, 10-2b): a cascade pass's listed bricks traced (1), the
    /// scene not tracing (0) or its structure not there (2) - PhotonVoxelLighting::setSideTracer's answer - and the counts.
    int gatherVoxelSides(const Ogre::PhotonVoxelSideTrace &job, unsigned cascade);
    void voxelSideCounts(GiStatus::VoxelSideCounts &out) const;
    /// The ray tier can trace the cards at all: open, its card-trace pipeline not refused (a scene whose structure is not
    /// built yet is still a trace that WAITS - SurfaceCache's lamps stay stale for it, never lit unshadowed for good).
    bool cardTracePresent() const;
    void cardMoverTimes(float &traceMs, float &stillMs);
    using CardMoverRec = PhotonComponent::CardMoverRec;
    using CardCasterRec = PhotonComponent::CardCasterRec;
    /// Bumped by every texel upload this scene makes (uploadRgbaLevels): the card
    /// sun-term trigger re-reads its cut-out casters' mask generations only on a
    /// frame where it moved (ALPHA-MASK-IDENTITY-1).
    unsigned long long mTextureUploadSerial = 0;
    /// The sum of the upload generations of a slot's sub-items' albedo textures —
    /// what an in-place re-upload of a cut-out's mask moves (cardMoverFrame).
    uint32_t cutoutMaskGeneration(uint32_t slot) const;
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
    unsigned long long giMaterialGeneration() const { return mPhoton.giMaterialGeneration(); }
    bool mAsyncShaders = false;
    /// IBL convolutions deferred because a permutation was building (ASYNC-SHADERS-1).
    unsigned long long mIblDeferrals = 0;
    bool probeGatherWanted() const { return mPhoton.probeGatherWanted(); }
    /// The scene's GI is on (a voxel mode): a non-gathering view's opaque passes then take the screen integrate's
    /// products (ChainDesc::screenIntegrate, PHOTON-7 7-6).
    bool screenIntegrateWanted() const { return mPhoton.giParams().mode != GiMode::Off; }
    bool probeGridByRays() const { return mPhoton.probeGridByRays(); }
    bool probeGridWanted() const { return mPhoton.probeGridWanted(); }
    unsigned long long probeShadowBytes() const { return mPhoton.probeShadowBytes(); }
    void dropProbeGridByRays() { return mPhoton.dropProbeGridByRays(); }
    unsigned long long giLightingSerial() const { return mPhoton.giLightingSerial(); }
    unsigned long long gatherRestKey() const { return mPhoton.gatherRestKey(); }
    unsigned long long gatherRestartKey() const { return mPhoton.gatherRestartKey(); }
    void gatherStatusInto(GatherStatus &out) const;
    /// The test-and-tool knobs (Engine.h's `setGatherTuning`): every zero means
    /// "what the tier derives", so the default is the shipped configuration.
    void setGatherTuning(const GatherTuning &t) override {
        if (postScene([=](OgreScene &s) { s.setGatherTuning(t); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::setGatherTuning");
        mGatherTuning = t;
    }
    const GatherTuning &gatherTuning() const { return mGatherTuning; }
    GatherTuning mGatherTuning;
    void setGiPlacementPin(const GiPlacementPin &pin) override {
        if (postScene([=](OgreScene &s) { s.setGiPlacementPin(pin); return true; })) return;   // a message
        JAH_HOST_CALL("Scene::setGiPlacementPin");
        mPhoton.setPlacementPin(pin);
    }
    void requestGiVoxelDigests(const std::shared_ptr<GiVoxelDigests> &out) override {
        if (!out) return;
        if (postScene([out](OgreScene &s) { s.requestGiVoxelDigests(out); return true; })) return;   // a message
        mPhoton.giVoxelDigests(*out);
        out->ready = true;
    }
    void setSunContact(const SunContactDesc &d) override {
        if (postScene([=](OgreScene &s) { s.setSunContact(d); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::setSunContact"); return mPhoton.setSunContact(d); }
    SunContactDesc sunContact() const override { return mPhoton.sunContact(); }
    SunContactStatus sunContactStatus() const;
    AreaVisStatus areaVisStatus() const;
    bool sunContactWanted() const { return mPhoton.sunContactWanted(); }
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
    /// engine.tlas_compute (a TOOL path: submits and waits) — TlasReadback.
    bool readTlasInstances(TlasReadback &out, std::string &err);
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
    bool refreshGiLighting() override {
        if (postScene([=](OgreScene &s) { return s.refreshGiLighting(); })) return true;   // U3 4a: a message
        JAH_HOST_CALL("Scene::refreshGiLighting"); return mPhoton.refreshGiLighting(); }
    void setNodeProbeGridExcluded(NodeId id, bool excluded) override;
    bool nodeProbeGridExcluded(NodeId id) const override;
    void setNodeHelper(NodeId id, bool helper) override;
    void setNodeMovable(NodeId id, bool movable, MobilityChange change) override;
    bool nodeMovable(NodeId id) const override;
    MobilityStatus mobilityStatus() const;
    bool nodeHelper(NodeId id) const override;
    void setNodeBackdrop(NodeId id, bool backdrop) override;
    void setNodeVrHelper(NodeId id, bool vrHelper) override;
    bool nodeVrHelper(NodeId id) const override;
    void setVrProxyNodes(NodeId left, NodeId right) override;
    void vrProxyNodes(NodeId out[2]) const override;
    void setVrRayNodes(NodeId line, NodeId marker) override;
    void vrRayNodes(NodeId out[2]) const override;
    CardWalk cardWalk() const override;
    GpuWalk gpuWalk() const override;
    TraceSun traceSun() const override;
    GiPlan giPlan() const override;
    bool rayNearLevels(std::vector<int> &levels) const override;
    bool rayNearAddresses(std::vector<unsigned long long> &addresses) const override;
    void setVrHandBoneNodes(unsigned hand, const NodeId *nodes, unsigned count) override;
    unsigned vrHandBoneNodes(unsigned hand, NodeId *out, unsigned count) const override;
    bool nodeWorldPose(NodeId id, Vec3 &position, Quat &rotation) const override;
    bool nodeWorldPoseDrawn(NodeId id, Vec3 &position, Quat &rotation) const override;
    bool nodeBackdrop(NodeId id) const override;
    void setNodeLightMask(NodeId id, unsigned mask) override;
    unsigned nodeLightMask(NodeId id) const override;
    void setNodeCastShadow(NodeId id, bool on) override;
    bool nodeCastShadow(NodeId id) const override;
    void setNodeFaceCull(NodeId id, FaceCull cull) override;
    FaceCull nodeFaceCull(NodeId id) const override;
    unsigned cullTwinCount() const { return mCullTwinCount; }
    unsigned cullTwinCandidateCount() const { return unsigned(mTwinCandidates.size()); }
    /// THE CULL TWINS' SWEEP (CULL-MODE-2; CULL-TWIN-DEBTS-1): destroys a twin that
    /// no renderable has worn for kCullTwinGraceFrames of this scene's frames. It
    /// visits only the CANDIDATES — twins an Item let go of since (noteTwinLetGo) —
    /// and asks each one's own wearer count, Ogre's linked-renderable list, so the
    /// cost is per release, never a walk of the scene's items. Called once per drawn
    /// frame before the frame's first reader of the Items' datablocks.
    void sweepCullTwins();
    /// An unworn twin lives this many of its scene's frames before it dies, so a cull
    /// toggled every frame re-uses one datablock instead of making and destroying one
    /// per frame. One second at the engine's fixed 1/60 s.
    static constexpr unsigned kCullTwinGraceFrames = 60u;

    // ---- Planar reflections (PLANAR_REFLECTIONS_SPEC.md; impl OgrePlanar.cpp) ----
    bool setPlanarReflections(const PlanarReflectionParams &p) override;
    bool setNodePlanarReflector(NodeId id, bool on) override;
    bool nodePlanarReflector(NodeId id) const;
    int  activePlanarReflectors() const;
    unsigned planarReflectorArms() const { return mPlanarArms; }
    unsigned mPlanarArms = 0;   ///< armReflector's actors made (a diagnostic)

    // ---- Hardware ray tracing, per scene (ledger §425; impl OgreScene.cpp) ----
    /// Defined in OgreScene.cpp: a flip to Off also releases the scene's ray
    /// structures (forgetRayQuery) so Off costs nothing, as the verb promises.
    void setRayTracing(RayTracingMode mode) override;
    bool rayTracingResolved() const override;
    /// The scene's live PlanarReflections, or null when the budget is 0. Views
    /// read this once a frame to decide whether to arm their listener.
    Ogre::PlanarReflections *planarReflections() const { return mPlanar; }
    /// The hybrid's reflection-probe arm, or null (the shadow-cache probe in
    /// tests/shadow drives captures through it).
    Ogre::ParallaxCorrectedCubemapAuto *parallaxCorrectedCubemap() const { return mPhoton.pcc(); }
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
    bool dropGiForShadowRebuild() { return mPhoton.dropGiForShadowRebuild(); }
    void recreateGiAfterShadowRebuild() { return mPhoton.recreateGiAfterShadowRebuild(); }
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
    const GiParams &giParams() const { return mPhoton.giParams(); }
    /// SSAO-DOUBLE-1: the GI carries this scene's occlusion (giCarriesOcclusion on
    /// the mode last applied), so every view that draws it builds without SSAO.
    bool ssaoRefused() const { return giCarriesOcclusion(mPhoton.giParams().mode); }
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
    void settleTextureResidency() { return mPhoton.settleTextureResidency(); }
    bool giVoxelTexturesPending() { return mPhoton.giVoxelTexturesPending(); }
    bool giEnvironmentPending() { return mPhoton.giEnvironmentPending(); }
    /// THE GI MOVEMENT SCAN, once per frame, run by its consumer (the
    /// probe budget) — which is EARLIER in the frame than
    /// any scene graph update, so it reads updated bounds. Same pass, same
    /// per-node records as the caster walk (walkItems).
    /// True when THIS call (re)made the walk's GI record (the walk ran, or nothing was named
    /// and the record says "nothing moved"); false when this frame's record already stands.
    bool ensureGiWalk(const PhotonComponent::WalkInputs &in);
    const PhotonComponent::WalkRecord &giWalk() const { return mGiWalk; }
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
    /// NODELESS = scene-wide: every item may have changed, so both scans owe a full pass. A seam that knows its node
    /// NAMES it instead (the overload): only that node's item is re-read (U0 3b, the change set's engine half).
    void noteSceneTransformWrite() { ++mSceneTransformWrites; owesNamesFull(kNameAll); }
    /// AN INPUT TO THE CASTER HALF OF THE WALK CHANGED, and it is not a
    /// transform (ENGINE-4 F5): a rig posed, a caster-shape seam (a mesh or
    /// material swap, a rebuilt Item, a per-object Cast Shadow flag, a
    /// generated vertex piece), a render-queue refile. The GI half's epoch
    /// cannot carry these — none of them moves anything — so the caster half
    /// adds its own counter on top of it (shadowEpoch below).
    void noteShadowScanInput() { ++mShadowScanWrites; mScanFullShadow = true; }
    void recreatePlanarAfterShadowRebuild();

    Ogre::SceneManager *sceneManager() const;
    /// The head of the scene's voxel chain (cascade 0's PhotonVoxelLighting, which holds
    /// the extra cascades), or null when no voxel GI is built. For measuring
    /// harnesses (Engine::voxelReaderParity), never for ownership.
    Ogre::PhotonVoxelLighting *voxelLighting() const { return mPhoton.voxelLighting(); }

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
        /// A node the DOCUMENT drives (createDocumentNode, ENGINE-UPGRADE-U2): its local TRS, parent and graph
        /// class arrive through the frame record (applyNodeRecords) and nowhere else; its document children are
        /// pushed by their host, so the visibility cascade does not descend into them.
        bool             document = false;
        /// ...and the GRAPH CLASS the document last said it is in (NodeRecord::isStatic); the node, its
        /// attachments and its helper children follow it (enforceGraphClass).
        bool             docStatic = false;
        /// THE TWO GRID STATES OF A DOCUMENT NODE (ENGINE-UPGRADE-U3 stage 1): the LOCAL TRS of the last two records
        /// that carried one (applyNodeRecord) and the document step the newer was built at (FrameInput::stepSerial).
        /// `cur` is what the node draws; stage 5's free-running frame interpolates prev -> cur. Both start at the
        /// identity a document node is born with.
        struct LocalTrs {
            Ogre::Vector3    position = Ogre::Vector3::ZERO;
            Ogre::Quaternion rotation = Ogre::Quaternion::IDENTITY;
            Ogre::Vector3    scale = Ogre::Vector3::UNIT_SCALE;
        };
        LocalTrs           prevLocal, curLocal;
        unsigned long long stepSerial = 0;
        bool               hasCur = false;   ///< a record has set cur (stage 5: the first has no motion)
        unsigned long long interpStep = ~0ull;   ///< the step it is listed for (OgreScene::mInterpNodes, once a step)
        bool               drawnCur = true;  ///< its Ogre node holds cur (false while a free frame drew it in between)
        /// WHAT THIS NODE ADDS TO THE SCENE'S MOBILITY AND RIG COUNTS (ENGINE-UPGRADE-U3 stage 2 read 12(b)): kept at
        /// every mutation site (recountNode), so MobilityStatus / RigStats are running totals, never a walk.
        struct Counted {
            bool movable = false, movableItem = false, movableLight = false, rigged = false, shared = false;
            bool particle = false;   ///< carries a particle system (PublishedFrame::particles lists it)
            NodeId shareSource = 0;  ///< the node whose skeleton it renders from (the facts block's shares)
            const Ogre::SkeletonInstance *skel = nullptr;
            size_t streamed = 0;
        } counted;
        Ogre::Item      *item  = nullptr;
        Ogre::Light     *light = nullptr;
        /// A hash of the LightDesc fields the SHADOW MAP depends on — type,
        /// range, spot cone, castShadows — pushed by setLight; the lamp-map
        /// cache folds in the light's pose each frame. Colour and intensity are
        /// deliberately absent: a shadow map is depth, and a dimmer lamp casts
        /// the same shadow (ENGINE_CACHE_POLICY_SPEC §3).
        unsigned long long lightShadowKey = 0;
        /// THE LAMP'S GI CHANGE KEYS (U0 3c, the scheduler's): what the voxel injection reads of its description
        /// (setLight), and its derived world pose as the last drained change set left it — a write of the same
        /// pose (play stop's restore, a script re-asserting it) is no move.
        unsigned long long lightGiKey = 0;
        bool               lightGiKnown = false;
        Ogre::Vector3      lightPos = Ogre::Vector3::ZERO;
        Ogre::Quaternion   lightRot = Ogre::Quaternion::IDENTITY;
        bool               lightPoseKnown = false;
        /// THE LAMP AS THE CHANGED-REGION LIST LAST LISTED IT (PHOTON-7 stage 3; listLampChange): its range sphere -
        /// the old half of the next event's old-and-new box - or none (a lamp hidden, removed or never listed).
        Ogre::Vector3      lightListedCentre = Ogre::Vector3::ZERO;
        float              lightListedRange = 0.0f;
        bool               lightListed = false;
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
        } scan;
        /// This node's own id. The item index (mItemNodes) is a vector of Node*,
        /// so a walk that has to name the nodes it found — the GI item set —
        /// needs the id on the node rather than a second walk of the map.
        NodeId           selfId = 0;
        /// This node's place in OgreScene::mItemNodes, or npos (no Item).
        size_t           itemSlot = size_t(-1);
        /// THE NAMES' TAKES (ENGINE-UPGRADE-2B stage 5): the take generation of the GI scan's, the caster
        /// scan's and the GPU scene's pending list this node was last named into - a node is named ONCE per
        /// take, however many seams fire for it (attachMesh fired three or four).
        /// THE ONE NAME LIST's bits this record is owed by (OgreScene::nameFor; U4 decision 12.1 ii).
        mutable uint8_t  namedMask = 0;
        /// ...and whether it is ON the list (a record whose bits were all taken stays listed until the list compacts:
        /// the death erases a LISTED record whatever its mask, and a listed record is never pushed twice — decision 16.2).
        mutable bool     namedListed = false;
        /// THE NAMING PASS AND THE ROOT (U4 stage 3): the pass this node was last named in (each node once a pass),
        /// and the moved subtree root's GI box its walk folds into — valid while giRootEpoch is the scene's.
        uint32_t         nameSeen = 0, giRootEpoch = 0, giRootSlot = 0xFFFFFFFFu;
        /// THE GEOMETRY SIGNATURE'S TERM (U4 stage 3; PhotonComponent::computeGiGeometrySignature): this node's share
        /// of the order-free sum, counted while `geomSigGen` is the signature's generation; and its take of the
        /// signature's name list.
        unsigned long long geomSigTerm = 0;
        uint32_t         geomSigGen = 0;
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
        // do nothing when nothing changed. The assignment is expensive the
        // first time (an atlas rebuild) and the mirror calls setLight for every
        // light on every sync — at 60 Hz.
        std::string      lightProfilePath;
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
        /// (REFLECTIONS_ADOPTION_SPEC.md P1a.2). It still VOXELIZES, bounces light
        /// and is photographed — the exclusion is only from the probe grid's
        /// placement fit, the deterministic escape hatch for the ground plane,
        /// the skybox shell, the level's terrain.
        bool                      probeGridExcluded = false;
        /// EDITOR HELPER (REFLECTIONS_ADOPTION_SPEC.md P1b): the grid, light
        /// icons, range wires — things the user must see but a reflection probe
        /// must not capture. Carries kHelperBit instead of kVisibleBit.
        bool                      helper = false;
        /// A helper that is PART OF THE PICTURE (kBackdropBit's note): the
        /// editor's Ground plane widget. Implies `helper` — same exclusion from every
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
        /// PER-OBJECT FACE CULL (Scene::setNodeFaceCull, CULL-MODE-2). Remembered
        /// for the castShadow reasons: every attach rebuilds the Item, and the host
        /// may say it before the geometry arrives. `Material` wears the material's
        /// own datablock; anything else that differs from the material's own cull
        /// wears the material's cull twin (MaterialRec::cullTwins).
        FaceCull                  faceCull = FaceCull::Material;
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
    void markShadowShapeDirty(Node &n) {   // NAMED for the caster walk (U0 3b), never a full pass
        n.shadowShapeDirty = true; ++mShadowScanWrites;
        nameFor(n, kNameShadow);
    }
    /// THE ONE NAME LIST (ENGINE-UPGRADE-U4 decision 12.1 ii): every node written since its consumers last read, ONCE,
    /// with a bit per consumer that still owes it — the GPU scene's walk, the GI scan, the caster scan, the geometry
    /// signature — instead of four lists pushed per write. A consumer reads the records carrying its bit and releases
    /// the bit; a record no consumer owes leaves the list. Bounded by the walk it stands for: once it holds as many
    /// records as a full walk visits (the items and the decals), every consumer owes its full pass instead. It holds
    /// RECORDS: a death owes every consumer its full pass, so it never holds a dead one.
    enum NameBits : uint8_t { kNameGpu = 1u, kNameGi = 2u, kNameShadow = 4u, kNameSig = 8u, kNameAll = 15u };
    uint8_t namesFullMask() const {
        return uint8_t((mSubstrate.namedFull ? kNameGpu : 0u) | (mScanFullGi ? kNameGi : 0u) |
                       (mScanFullShadow ? kNameShadow : 0u) | (mGeomSigFull ? kNameSig : 0u));
    }
    void nameFor(Node &n, uint8_t bits) const {
        bits = uint8_t(bits & ~namesFullMask() & ~n.namedMask);
        if (!bits) return;
        if (!n.namedListed) {
            if (mNamed.size() >= mItemNodes.size() + mDecalNodes.size()) { owesNamesFull(kNameAll); return; }
            mNamed.push_back(&n);
            n.namedListed = true;
        }
        if (!n.namedMask) ++mNamedLive;
        n.namedMask = uint8_t(n.namedMask | bits);
        for (unsigned b = 0; b < 4u; ++b) if (bits & (1u << b)) ++mNamedCount[b];
    }
    static unsigned nameBitIndex(uint8_t bit) { return bit == kNameGpu ? 0u : bit == kNameGi ? 1u : bit == kNameShadow ? 2u : 3u; }
    bool hasNamed(uint8_t bit) const { return mNamedCount[nameBitIndex(bit)] != 0u; }
    /// The consumers' bits released: their counts zero, and the list emptied once no consumer owes any record (the
    /// common case: every write names all four) — else compacted in naming order once half of it is owed by nobody.
    void releaseNamed(uint8_t bits) const {
        bool any = false;
        for (unsigned b = 0; b < 4u; ++b) if ((bits & (1u << b)) && mNamedCount[b]) { mNamedCount[b] = 0u; any = true; }
        if (!any) return;
        for (Node *n : mNamed) {
            if (!(n->namedMask & bits)) continue;
            n->namedMask = uint8_t(n->namedMask & ~bits);
            if (!n->namedMask) --mNamedLive;
        }
        compactNamed();
    }
    /// A consumer's take in ONE pass: the records it owes, its bit released.
    void takeNamed(uint8_t bit, std::vector<Node *> &out) const {
        out.clear();
        unsigned &count = mNamedCount[nameBitIndex(bit)];
        if (!count) return;
        for (Node *n : mNamed) {
            if (!(n->namedMask & bit)) continue;
            out.push_back(n);
            n->namedMask = uint8_t(n->namedMask & ~bit);
            if (!n->namedMask) --mNamedLive;
        }
        count = 0u;
        compactNamed();
    }
    /// Emptied when nobody owes anything (the common case), else compacted once half the list is owed by nobody —
    /// the live count is kept, never counted.
    void compactNamed() const {
        if (!mNamedLive) {
            for (Node *n : mNamed) n->namedListed = false;
            mNamed.clear();
            return;
        }
        if (mNamedLive * 2u > mNamed.size()) return;
        mNamed.erase(std::remove_if(mNamed.begin(), mNamed.end(),
                                    [](Node *n) { return n->namedMask == 0u ? (n->namedListed = false, true) : false; }),
                     mNamed.end());
    }
    /// A RECORD DIES: a LISTED record leaves the list (its bits with it) — whatever its mask: a record whose bits were
    /// all taken is still listed until the list compacts (decision 16.2: the early return on an empty mask left a
    /// dangling pointer there). The consumers lose nothing; the list never holds a dead record.
    void forgetNamed(Node &n) const {
        if (!n.namedListed) return;
        for (unsigned b = 0; b < 4u; ++b) if (n.namedMask & (1u << b)) --mNamedCount[b];
        if (n.namedMask) --mNamedLive;
        n.namedMask = 0u;
        n.namedListed = false;
        mNamed.erase(std::remove(mNamed.begin(), mNamed.end(), &n), mNamed.end());
    }
    /// THE LIST'S GUARD (decisions 16.2 + 18.5): listed entries that are not a live record of this scene — compared by
    /// address against the record map, never dereferenced — and, of the live ones, those without the listed flag or
    /// listed more than once. 0, 0 and 0 is the law.
    void namedListGuard(unsigned &dangling, unsigned &notFlagged, unsigned &duplicates) const {
        std::unordered_set<const Node *> live;
        live.reserve(mNodes.size());
        for (const auto &kv : mNodes) live.insert(&kv.second);
        std::unordered_set<const Node *> seen;
        dangling = notFlagged = duplicates = 0u;
        for (const Node *n : mNamed) {
            if (!live.count(n)) { ++dangling; continue; }
            if (!n->namedListed) ++notFlagged;
            if (!seen.insert(n).second) ++duplicates;
        }
    }
    /// The consumers in `bits` owe their full pass (a nodeless write, an incomplete set, an overflow, a death).
    void owesNamesFull(uint8_t bits) const {
        if (bits & kNameGpu) mSubstrate.namedFull = true;
        if (bits & kNameGi) mScanFullGi = true;
        if (bits & kNameShadow) mScanFullShadow = true;
        if (bits & kNameSig) mGeomSigFull = true;
        if (bits == kNameAll) {
            for (Node *n : mNamed) { n->namedMask = 0u; n->namedListed = false; }
            mNamed.clear();
            mNamedLive = 0u;
            for (unsigned &c : mNamedCount) c = 0u;
        } else {
            releaseNamed(bits);
        }
    }
    mutable std::vector<Node *> mNamed;
    mutable unsigned mNamedCount[4] = { 0u, 0u, 0u, 0u };
    mutable size_t mNamedLive = 0u;   ///< the list's records some consumer still owes

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
        /// The ColourRamp affectors' image NAMES, in order. FROZEN like the
        /// kinds: ColourImageAffector2 reads its image once, in the def's
        /// init(), and asserts on a later setImageAdjust — a ramp change is
        /// a new definition (audit D2).
        std::vector<std::string> rampImages;
        std::string key() const;
    };
    /// Registers a colour-ramp image's folder with the resource system and
    /// answers the NAME the affector loads it by, or "" (with mError set) when
    /// the file is not there.
    std::string particleRampImage(const std::string &path);
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
        /// ATOM stage 2: the mesh's CLUSTER STREAM — every cluster of its DAG,
        /// triangles concatenated in cluster order (`MeshData::clusterIndices`),
        /// one IMMUTABLE index buffer over the SAME vertex buffer as the levels,
        /// in the SAME index type as level 0, so a cluster is the contiguous
        /// range [firstIndex, firstIndex + indexCount) of it. Bound to no VAO: it
        /// is THE CUT's source (ATOM-CLUSTER-CUT) — the GPU scene's cluster row
        /// names its device address, the cull's emit job copies the drawn clusters'
        /// corners out of it into the view's compacted stream, and the screen decode
        /// reads a pixel's triangle from it. Null for a mesh with no DAG (the GPU
        /// scene then cuts level 0's own indices, a FLAT DAG). Owned here:
        /// the deleter returns it to the VaoManager when the record dies (every
        /// record dies with its scene, before Root).
        std::unique_ptr<Ogre::IndexBufferPacked, IndexBufferRelease> clusterStream;
        /// The DAG's tables, kept beside the stream they index (the GPU scene uploads
        /// them at the mesh's first attach; a suite reads them to check the stream).
        std::vector<MeshCluster>      clusters;
        std::vector<MeshClusterGroup> clusterGroups;
        /// THE CLUSTERS' OWN BOXES (ATOM-8 stage 2), measured at creation from the positions their triangles index:
        /// six floats a cluster (mesh-space min xyz, max xyz) — the DAG's clusters, or for a mesh with no DAG the
        /// flat runs of level 0 the GPU scene cuts (GpuScene::kFlatClusterTriangles). Packed at upload
        /// (OgreGpuScene.cpp, GpuClusterBox).
        std::vector<float>            clusterBoxes;
        /// WHICH CLUSTERS THEY BOUND (the stage 2 read's provenance key): true = level 0's flat runs, false = the DAG's
        /// clusters - the upload pairs them with the arm it takes by this flag, never by a count that may coincide.
        bool                          clusterBoxesFlat = false;
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
    /// What bindTrackedTextures LAST WROTE to ONE datablock, per slot, and whether
    /// it has ever written (MaterialRec::bound says why it exists).
    struct TextureBindState {
        TextureId lastBoundTextures[kPbrTextureSlotCount] = {};
        bool      everBound = false;
        /// The sampler state those bindings were made with: a change to
        /// anisotropy or an address mode (A-2) has to re-bind every slot even
        /// when no texture moved, because the sampler rides the binding.
        float     lastAnisotropy = 1.0f;
        PbrParams::AddressMode lastAddress[kPbrTextureSlotCount] = {};
    };
    /// One cull twin of a material (MaterialRec::cullTwins).
    struct CullTwin {
        std::string      datablockName;   // uniquely owned; empty = none
        TextureBindState bound;
        /// The sweep tick (+1) at which the sweep first found it unworn; 0 = worn
        /// (or not looked at since it was last worn).
        unsigned long long unwornSince = 0;
        /// Listed in mTwinCandidates — ONCE: the list is bounded by the live twins,
        /// however many let-gos a scene that is never drawn (so never swept) sees.
        bool candidate = false;
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
        /// (Per DATABLOCK: the master's here, each cull twin's in its CullTwin.)
        TextureBindState bound;
        /// THE CULL TWINS (CULL-MODE-2), indexed by cullTwinIndex (Back, Front,
        /// TwoSided): a second datablock of this material whose cull — and the
        /// two-sided lighting that goes with it — is a NODE's (Scene::setNodeFaceCull)
        /// instead of the material's own. Everything else is the master's: built from
        /// this record the way the family switch builds (params, tracked maps, pieces,
        /// the reflection slot) and kept in step by every write to the master
        /// (syncCullTwins). Made on first wear, destroyed by sweepCullTwins once no
        /// Item wears it, and ALWAYS before the master dies (destroyCullTwins: a
        /// datablock is destroyed with no renderable linked, ~HlmsDatablock asserts).
        /// An empty name = no twin in that slot.
        CullTwin cullTwins[3];
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
    /// `cull` other than FaceCull::Material overrides the params' own two-sidedness
    /// (a CULL TWIN's datablock, CULL-MODE-2): the macroblock's cull and the
    /// two-sided lighting follow it, and nothing else does.
    static void applyPbr(Ogre::HlmsPbsDatablock *db, const PbrParams &p,
                         bool refractionsActive, FaceCull cull = FaceCull::Material);
    /// The UNLIT half of applyPbr (HLMS_ADOPTION P4a): the subset of PbrParams
    /// the Unlit family can actually honour — base colour, alpha and its mode,
    /// two-sidedness. Everything else is dropped, deliberately and visibly
    /// (see ShadingModel in Types.h; the panel disables the rows this cannot
    /// carry rather than letting a user discover them).
    static void applyUnlit(Ogre::HlmsUnlitDatablock *db, const PbrParams &p,
                           FaceCull cull = FaceCull::Material);
    /// The DISTORTION datablock (POST_LOOKS_SPEC.md §5.2): an HlmsUnlit block
    /// whose texture is a screen-space displacement field and whose colour
    /// alpha is the strength. See the definition for why each block is set.
    static void applyDistortion(Ogre::HlmsUnlitDatablock *db, const PbrParams &p,
                                FaceCull cull = FaceCull::Material);
    // ---- THE CULL TWINS (CULL-MODE-2; MaterialRec::cullTwins) ----
    /// The Ogre cull of a resolved FaceCull (never Material).
    static Ogre::CullingMode ogreCullOf(FaceCull c);
    /// `cull` resolved: Material -> Back (a material has no cull of its own; the
    /// NODE is the authority, CULL-MODE-2).
    static FaceCull resolveCull(FaceCull cull) {
        return cull == FaceCull::Material ? FaceCull::Back : cull;
    }
    static size_t cullTwinIndex(FaceCull c) { return size_t(c) - 1u; }
    /// Only a PBR material has twins (either family); an overlay (grid, gizmo,
    /// outline) is drawn as it was made, whatever its node says.
    static bool cullTwinnable(const MaterialRec &rec) { return !rec.unlit || rec.shadingUnlit; }
    /// The datablock node `n` wears for `rec`: the master when the node's cull is the
    /// material's own (or the material has no twins), else its twin — made now if
    /// nothing wore it yet.
    Ogre::HlmsDatablock *wornDatablock(const Node &n, MaterialRec &rec);
    Ogre::HlmsDatablock *cullTwinOf(MaterialRec &rec, FaceCull cull);
    /// Writes the record's params into one datablock of it with `cull`.
    void applyRecTo(MaterialRec &rec, Ogre::HlmsDatablock *db, FaceCull cull);
    /// Every twin of `rec` brought to the master's state (params, maps, pieces, the
    /// reflection slot): the one call each write to a master ends with.
    void syncCullTwins(MaterialRec &rec);
    /// Destroys every twin of `rec` — the caller has taken every Item off them.
    void destroyCullTwins(MaterialRec &rec);
    /// AN ITEM (OR ONE SUB-ITEM) LETS GO OF `db`: a twin among them becomes a sweep
    /// candidate. Every site that re-points or destroys an Item calls it with the
    /// datablock(s) it is leaving, BEFORE it leaves them.
    void noteTwinLetGo(const Ogre::HlmsDatablock *db);
    void noteTwinsLetGo(const Ogre::Item *item);
    /// Item / sub-item re-pointed at `db`, the datablock it leaves noted first.
    void wearDatablock(Ogre::Item *item, Ogre::HlmsDatablock *db);
    void wearDatablock(Ogre::SubItem *sub, Ogre::HlmsDatablock *db);
    /// Destroys one twin (the caller knows no renderable wears it).
    void destroyCullTwin(MaterialRec &rec, CullTwin &t);
    unsigned mCullTwinCount = 0;
    bool     mCullTwinSweep = false;
    /// Every live twin's datablock -> its record and slot (the candidates' lookup).
    struct TwinOwner { MaterialRec *rec = nullptr; CullTwin *twin = nullptr; };
    std::unordered_map<const Ogre::HlmsDatablock *, TwinOwner> mTwinOwners;
    /// Twins let go of since the sweep last settled them: each live twin at most
    /// once (CullTwin::candidate), a destroyed one taken out as it dies.
    std::vector<const Ogre::HlmsDatablock *> mTwinCandidates;
    /// This scene's sweep calls (one per drawn frame): the grace clock.
    unsigned long long mCullSweepTick = 0;
    /// The visibility bits a node's PFX2 def carries when visible:
    /// kDistortionBit for a distortion emitter, else helper/visible.
    static Ogre::uint32 particleVisibilityBits(const Node &n);
    /// (Re-)binds whatever `rec.boundTextures` says onto the material's CURRENT
    /// datablock — the step that makes a family switch keep its maps. The Unlit
    /// family has one usable slot (Albedo -> texture unit 0); the rest are kept
    /// in the record so switching back to Lit restores them.
    void bindTrackedTextures(MaterialRec &rec);   // records what it bound (see MaterialRec)
    /// ...onto ONE datablock of the material, guarded by that datablock's own record
    /// (the master's or a cull twin's). bindTrackedTextures = the master, then twins.
    void bindTrackedTexturesInto(MaterialRec &rec, Ogre::HlmsDatablock *raw, TextureBindState &st);
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
    void bindTrackedPiecesInto(const MaterialRec &rec, Ogre::HlmsPbsDatablock *db);
    /// Sets or clears `jah_shader_clock` — our datablock property gating the
    /// pass-buffer clock's DECLARATION — to match whether the datablock carries
    /// a generated piece, and `jah_uv_scroll` to match whether `p` scrolls its
    /// maps (TORNADO-1). The whole pixel-suite isolation contract
    /// rests on both being false for every material that has neither.
    static void applyClockProperty(Ogre::HlmsPbsDatablock *db, const PbrParams &p);
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
    /// `clusterStreamOut` receives the mesh's CLUSTER STREAM (MeshRec::clusterStream)
    /// when the data carries a DAG and the mesh is static; null otherwise.
    Ogre::MeshPtr buildMeshV2(const std::string &name, const MeshData &data,
                              std::vector<float> *interleavedOut = nullptr,
                              Ogre::IndexBufferPacked **clusterStreamOut = nullptr);
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
    // ---- THE CLOUD LAYER (CLOUDS-2D-1; OgreSky.cpp, "THE CLOUD LAYER") -----
    /// Creates (once), shows, hides and parameterises the layer from
    /// mSkyDesc.clouds; `fieldChanged` queues a re-bake of the field.
    void applyCloudLayer(bool fieldChanged);
    void destroyCloudLayer();
    /// The sun disc's material follows the layer: the clouded variant while a
    /// layer is drawn (the disc is dimmed by the sheet in front of it), its own
    /// otherwise — so a scene without a layer draws exactly the disc it drew.
    void syncSunDiscClouds();
    /// Renders the pending field bake, INSIDE a frame (it is a render pass):
    /// called at the head of applyPendingSkyCapture, so a capture queued by
    /// the same change photographs the new field.
    void bakeCloudField();
    CloudStatus cloudStatus() const;
    bool renderSkyEquirect(unsigned width, unsigned height, unsigned faceSize, float exposure,
                           std::vector<unsigned char> &rgba) override;
    bool cloudField(std::vector<float> &tau, unsigned &size, float &tileMetres) override;
    /// Is the layer on screen: enabled, and a sky to draw over.
    bool cloudLayerDrawn() const;
    Ogre::Rectangle2D *mCloudQuad = nullptr;
    Ogre::MaterialPtr  mCloudMaterial;          // per-scene clone of Jahshaka/CloudLayer
    Ogre::MaterialPtr  mCloudBakeMaterial;      // Jahshaka/CloudBake itself (the bake binds per render)
    Ogre::MaterialPtr  mSunDiscCloudMaterial;   // ...of Jahshaka/SunDiscClouded
    Ogre::TextureGpu  *mCloudWeatherNone = nullptr;   // 1x1 white, ManualTexture: the bake's no-map unit
    /// 1x1x1 (0,0,0,1), ManualTexture: the "no air" unit under a sky that is not
    /// the planet's atmosphere (the cloud sheet's, SKY-ATMOSPHERE-1; the screen
    /// reflection's resolve, PHOTON-I-1) — noAirVolume().
    Ogre::TextureGpu  *mNoAirVolume = nullptr;
    /// The sheet's air: the atmosphere's aerial table (or no air) and its
    /// constants, bound on every layer or sky change (applyCloudLayer).
    void bindCloudAir();
    Ogre::TextureGpu  *mCloudField = nullptr;   // 2048^2 R16F optical depth, one 64 km tile
    Ogre::Camera      *mCloudBakeCamera = nullptr;
    bool     mCloudFieldPending = false;
    /// The layer's own clock: the sum of the frame deltas of the frames this
    /// scene was drawn in (the engine has no wall clock).
    double   mCloudClock = 0.0;
    float    mCloudScroll[2] = { 0.0f, 0.0f };
    /// Drawn frames since the last capture the layer's scroll asked for.
    unsigned mCloudFramesSinceCapture = 0u;
    /// A capture requested by the SCROLL takes the asynchronous SH read even
    /// when it is not part of a gesture (integrateSkyShFromCube).
    bool     mSkyCaptureAsyncOnce = false;
    /// THE CLEAR SKY THE SHEET IS LIT BY. The sheet's sky-light term must not
    /// read the environment capture it is itself IN (a feedback whose answer
    /// depends on the edit history, and which ran an overcast deck to a
    /// saturated copy of its own colour at a low sun): it reads a capture of
    /// the same sky with the sheet hidden — taken only when the SKY changes
    /// (never on the sheet's own edits or scroll), synchronously for a lone
    /// change and through a ticket read at the next frame's top during a
    /// gesture, exactly like the environment's own SH.
    void captureCloudClearSky();
    void readCloudClearTicket(bool force);
    void pushCloudAmbient();
    // ---- CLOUDS-2D-2: the sheet's shadow on the voxels and the cards ----------
    /// The cloud shadow the GI inputs read — the pixel's state, SNAPSHOT on a
    /// change of the layer and at each scroll capture (the environment's own
    /// cadence), never per frame: a voxel re-injection and a card relight are
    /// the downstream cost the cadence was measured for. A change re-injects
    /// the lighting (refreshGiLighting) and moves mCloudGiSerial, which the
    /// cards' radiance signature folds.
    FogHlmsListener::CloudShadowState mCloudGiState;
    unsigned long long mCloudGiSerial = 0ull;
    /// `fieldRebaked`: the field's pixels changed under the same pointer.
    void snapshotCloudGi(bool fieldRebaked = false);
    /// Binds (or clears) the cloud field and its parameters on the shared
    /// "VCT/LightInjection" job for THIS volume — called before every
    /// PhotonVoxelLighting::update (applyCascadeEnvironment), because the job is shared
    /// by name process-wide and the state in force is whoever set it last.
    void bindCloudInjection(Ogre::PhotonVoxelLighting *lighting);
    bool     mCloudClearPending = false;
    bool     mCloudClearValid = false;
    /// The clear sky's irradiance on an up-facing plate / pi (CLOUDS-2D-3).
    float    mCloudClearMean[3] = { 0.0f, 0.0f, 0.0f };
    Ogre::AsyncTextureTicket *mCloudClearTicket = nullptr;
    CloudStatus mCloudStatus;
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
    /// The one teardown of a node's light (removeLight, releaseNode): its gone sphere listed, the lamp event noted.
    void destroyNodeLight(NodeId id, Node &n);

public:
    /// ADDS this scene's registry sizes into `out` (nodes/meshes/materials/
    /// textures). Additive because Engine::objectCounts sums every live scene
    /// into one census — see ObjectCounts. Touches nothing else in `out`.
    void addObjectCounts(ObjectCounts &out) const;
    void applyPendingGi() { return mPhoton.applyPendingGi(); }
    void applyPendingGiDeferrable() { return mPhoton.applyPendingGiDeferrable(); }
    void giDeferrable(const std::function<void()> &work) { return mPhoton.giDeferrable(work); }
    void applyPendingGiFlush() { return mPhoton.applyPendingGiFlush(); }
    /// The pace of the frame being rendered (OPEN_COVER_SPEC §2.1):
    /// Engine::renderOneFrame pushes it once per frame, to every scene.
    void setFramePace(FramePace pace) { mFramePace = pace; }
    bool giBuildOwesWork() const { return mPhoton.giBuildOwesWork(); }
    /// HOW MUCH of a staged arm build is still owed, and which stage is next
    /// (OPEN_COVER_SPEC §2.1, lane OPEN-COVER-2b). The public mirror of
    /// `mGiBuildStage`, which names the stage that will run NEXT — so `Field`
    /// is one stage from done and `ProbeScout` is four. Inline and const: this
    /// is read once per frame while a world streams in, by the indicator and by
    /// the slow-frame line that names what a frame spent.
    unsigned giBuildStagesLeft() const { return mPhoton.giBuildStagesLeft(); }
    GiArmStage giBuildStage() const { return mPhoton.giBuildStage(); }
    /// Materials whose bound texture has not finished streaming — the entries
    /// `settleTextureResidency` spends. The indicator's "textures" term.
    unsigned materialsAwaitingTexture() const {
        return mPhoton.materialsAwaitingTexture();
    }
    /// Whole-arm rebuilds this scene has done — `StreamingWork::giRebuilds`.
    unsigned long long giRebuildCount() const { return mPhoton.giRebuildCount(); }
    /// Is a deferred read of a texture's PIXELS owed this frame? True while the
    /// sky's IBL convolution is pending — the one piece of per-frame work that
    /// reads texture contents and cannot be asked twice (OPEN_COVER_SPEC §2 E).
    bool iblReadPending() const { return mIblPending; }
    void latchProbeCaptures(bool drawn) { return mPhoton.latchProbeCaptures(drawn); }
    void updateGiTracking(const Ogre::Vector3 &camPos, bool driverStereo) { return mPhoton.updateGiTracking(camPos, driverStereo); }
    void noteGiCamera(const Ogre::Vector3 &camPos, bool driverStereo, bool still, const GiEyePose *eyes,
                      unsigned eyeCount) {
        mPhoton.noteGiCamera(camPos, driverStereo, still, eyes, eyeCount);
    }
    /// THE PCC'S DIRTY-PROBE COLLECTION (U4 decision 14.3; the fork's ParallaxCorrectedCubemapAuto::collectDirtyProbes):
    /// the engine's, once a frame after its probe budget — the PCC's own frameStarted collection is switched off at
    /// its build (setCollectsExternally). A paused PCC (an undrawn scene) collects nothing, as frameStarted did.
    void collectDirtyProbes() {
        if (Ogre::ParallaxCorrectedCubemapAuto *p = mPhoton.pcc()) {
            p->collectDirtyProbes();
            mPhoton.noteProbeCollection();   // the guard's reading (gi.pccPlacementLights): the list after it
        }
    }
    /// THE PLAN AFTER THE SCENE GRAPH (ENGINE-UPGRADE-U4 decisions 8 + 14): the drain STASHES what the GI scheduler's
    /// plan reads from the record (planGi: its refresh serial, the material preview, whether the configuration was
    /// pushed — merged as the record merges them: a serial newest-wins only when one is carried, the preview by OR)
    /// and the frame runs it once the scene graph has made the world boxes current (runStashedGiPlan, first in
    /// `engine.plan`, BEFORE the pending flushes — the geometry term is read where 414c323 read it) — a join runs it
    /// at once (applyPendingEngineMessages), frame-less, with recomputing reads. `boxesCurrent` says the boxes are
    /// the frame's: between the frame's updateSceneGraph and the end of its plan block.
    void stashGiPlan(const FrameInputScene &part, bool pushed) {
        mGiPlanStash.owed = true;
        if (part.giRefreshSerial) mGiPlanStash.refreshSerial = part.giRefreshSerial;   // the newest that carries one
        mGiPlanStash.materialPreview = mGiPlanStash.materialPreview || part.materialPreview;
        mGiPlanStash.pushed = mGiPlanStash.pushed || pushed;
    }
    void runStashedGiPlan();
    /// THE DRAIN'S COUNTS (FrameInputStatus::drainRecordLookups / drainNameVisits / drainNameLookups; U4 decision 15):
    /// added into `st` and reset (null: reset only — what ran outside a drain).
    void takeDrainCounts(FrameInputStatus *st) const {
        if (st) {
            st->drainRecordLookups += mCountRecordLookups;
            st->drainNameVisits += mCountNameVisits;
            st->drainNameLookups += mCountNameLookups;
        }
        mCountRecordLookups = mCountNameVisits = mCountNameLookups = 0u;
    }
    void setBoxesCurrent(bool current) { mBoxesCurrent = current; }
    bool boxesCurrent() const { return mBoxesCurrent; }
    void updateForwardPlusRanges(const Ogre::Camera *cam) { return mPhoton.updateForwardPlusRanges(cam); }
    void forwardPlusLightCensus(unsigned &lights, unsigned &budget) const { return mPhoton.forwardPlusLightCensus(lights, budget); }
private:

    using GiBuildStage = PhotonComponent::GiBuildStage;
    /// The host says a world of this scene is arriving and none of it is on
    /// screen yet (Scene::setLoading). No FIRST-TIME arm build while it holds.
    bool mSceneLoading = false;
    /// The pace of the frame being rendered, pushed by the engine before the
    /// per-scene work. `Complete` outside a streaming open.
    FramePace mFramePace = FramePace::Complete;

    /// Would a reflection-probe face capture this node's item? kVisibleBit
    /// (the face pass's visibility_mask 0x1 — helpers carry kHelperBit and
    /// distortion items kDistortionBit instead), effectively shown, and in the
    /// face pass's render-queue range (rq_last 200: gizmos and selection
    /// outlines at RQ 210 are never captured, so they never stale anything).
public:   // (a seam PhotonComponent calls)
    bool probeSeesItem(const Node &n) const;
private:
    /// Can a transform write on this node change what any of the epoch's scans
    /// reads? (OgreScene.cpp, beside the bit scheme it asks about.) No for
    /// editor furniture — the gizmo, the bone overlay, wires, the grid — which
    /// is most of what goes through setNodeTransform at all.
    bool writeIsSceneMovement(const Node &n) const;

    using VoxelGatherInputs = PhotonComponent::VoxelGatherInputs;
    /// One voxel cascade of the chain: Photon's type (photon/PhotonComponent.h).
    using VctCascade = PhotonComponent::VctCascade;

    bool   underGesture(const Node &n);
    /// The three gather jobs over the GPU scene into `feed`, and the voxeliser bound
    /// to it (OgreGpuVoxelGather.cpp).
public:   // (a seam PhotonComponent calls)
    bool runVoxelGather(detail::VoxelFeed &feed, Ogre::PhotonVoxelizer *voxelizer,
                        const VoxelGatherInputs &in);
    /// The environment into ONE cascade's bounce (applyVctEnvironment, aimed).
    void applyCascadeEnvironment(Ogre::PhotonVoxelLighting *lighting);
private:
    /// The VCT light-injection ray-march step scale (the document's value, at
    /// least 1). ONE value: the moving tick's coarse march went with the one
    /// writer (a volume's radiance must not depend on which path injected it).
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
public:   // (a seam PhotonComponent calls)
    void applyNodeVisibilityFlags(Node &n);
private:
    /// THE CASTER BIT FOLLOWS THE CHANNEL (SHADOW-FIT-1): Ogre's LAYER_SHADOW_CASTER
    /// bit on `n`'s Item and billboards = the node's own castShadow AND a channel
    /// some shadow node draws (allShadowCasterChannels). The bit is what the shadow
    /// node's CASTERS BOX reads (SceneManager::_calculateCurrentCastersBox, under
    /// the VIEWPORT's mask, not the shadow passes'), so an editor helper left
    /// "casting" re-fitted the sun's splits around the light's icon in the editor
    /// viewport and not in a screenshot. Call after every channel or castShadow write.
    void applyNodeCasterBit(Node &n);
    /// The registry record hanging off an Ogre node, or null (an engine-owned
    /// helper child such as a light's -Y adapter, a document node the host has
    /// not adopted yet, the scene root).
    Node *registryNode(const Ogre::Node *sn);
    /// This scene's record for `id`, or null — the one-lookup form of
    /// `node(id)` + `mNodes.find(id)`, for the callers that need both the Ogre
    /// node and the record (round-2 review F9).
    std::vector<Node *> mInterpRecs;   ///< the newest step's nodes, resolved at their notes (valid: mInterpRecsValid)
    std::vector<Node *> mInterpStaleRecs;   ///< the step before's, by pointer (no removal since their notes)
    void forgetInterpolationPointers();
    /// THE DRAIN'S DOOR TO THE SCANS (U0 3b; decision 56): each node's subtree — its Ogre children and its bones'
    /// riders — named once to the GPU-scene walk and the GI/shadow scans, a lamp under it compared with its pose key.
    /// True when a lamp moved. `stack` is consumed. nameMovedNode: one registered node, what moves with it pushed.
    /// `rootSlot`: the moved subtree root's GI box this pass's nodes fold into (openGiRoot), or kNoGiRoot. A naming
    /// PASS (beginNamingPass) visits each Ogre node once across all its calls.
    bool nameMovedSubtrees(std::vector<Ogre::Node *> &stack, uint32_t rootSlot);
    static constexpr uint32_t kNoGiRoot = 0xFFFFFFFFu;
    /// THE NAMING PASS (U4 stage 3): one change set's (or one interpolation's) roots share it, so a node under two
    /// moved roots is named once.
    void beginNamingPass();
    /// ONE BOX PER MOVED SUBTREE ROOT (U4 stage 3): a root's slot, into which the GI walk folds the old and new box
    /// of every item named under it (instead of one cascade box per item); kNoGiRoot past kGiRootCap open roots.
    uint32_t openGiRoot();
    static constexpr size_t kGiRootCap = 4096u;
    uint32_t mNameSeenEpoch = 0u;
    std::unordered_set<const Ogre::Node *> mNameSeenOther;   ///< the pass's unregistered nodes (a light's adapter)
    std::vector<Ogre::Node *> mNameStack;                    ///< the change set's naming stack (scratch)
    /// The open roots' boxes and whether any item folded in, consumed (cleared) by the GI walk; `mGiRootEpoch` is the
    /// window a node's tag belongs to (the walk advances it).
    std::vector<Ogre::Aabb> mGiRootBoxes;
    std::vector<char> mGiRootUsed;
    uint32_t mGiRootEpoch = 1u;
    bool nameMovedNode(Node &rec, Ogre::Node *on, std::vector<Ogre::Node *> &stack);
    /// The node's record joins the newest step's list (applyNodeRecord; each node once a step).
    void noteInterpolated(Node &rec, NodeId id, unsigned long long step);
    struct InterpWrite { Node *rec = nullptr; float alpha = 1.0f; };
    std::vector<InterpWrite> mInterpWrite;    ///< what one interpolate writes, each node once (scratch)
    std::vector<Ogre::Node *> mInterpStack;   ///< ...named to the scans through nameMovedSubtrees (scratch)
    Node *record(NodeId id) {
        // ONE DIRECT-INDEX LOOKUP (U4 stage 3, decision 12.1), never the ordered map's descent: the per-record and
        // per-name paths call this per node — and every call is counted (decision 18.6: the count lives in the lookup).
        ++mCountRecordLookups;
        return id ? mNodeById.get(id) : nullptr;
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
    /// Pushes the environment (cube, gain, SH) into every cascade's bounce job
    /// (no-op without a VCT arm). Called before every PhotonVoxelLighting::update and on
    /// every environment change.
    void applyVctEnvironment();
    /// The environment's cube, gain or coefficients changed: the injection's
    /// serial moves and a bouncing chain re-settles (PHOTON-ENV-1).
    void noteEnvironmentChanged();
    /// The environment cube the last push saw (its identity, to notice a swap).
    Ogre::TextureGpu *mEnvCubeSeen = nullptr;

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
    /// A REFLECTOR WHOSE DATABLOCK'S CULL MAY HAVE MOVED (a node cull edit, its
    /// material's own two-sidedness flipping): disarmed and armed again, because
    /// armReflector returns early for a node that already has an actor and so
    /// never re-checks the two-sided refusal. The FLAG stays: a mirror refused now
    /// arms again the moment its datablock is one-sided again.
    void rederiveReflector(NodeId id, Node &n);
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
    NodeId track(const Node &n, NodeId id);   ///< the node under the id it is given (reserved: never 0, never taken)



    Ogre::Root         *mRoot;
    Ogre::SceneManager *mSceneMgr;
    std::string        &mError;
    std::map<NodeId, Node> mNodes;
    /// Reverse index Ogre node id -> the RECORD, for the one question the
    /// visibility cascade and the change set's naming ask of every node they
    /// walk ("is this one of ours, and what was it told?"). Keyed by Ogre's
    /// never-reused id rather than the node address, which Ogre recycles; it
    /// holds the record itself (a std::map node is address-stable until it is
    /// erased, and its entry here goes first) — ONE lookup on the naming path,
    /// never a hash then a tree descent (ENGINE-UPGRADE-U4 stage 3).
    /// THE DIRECT INDEX (U4 decision 12.1 i): Ogre's node ids come from ONE process-wide counter (IdObject, never
    /// reused), so the record of an id is found by indexing, never hashing — in PAGES of 4,096 slots allocated on
    /// first use (a scene that registers only recent ids holds only their pages, not every id the process ever made).
    template <class Key>
    struct PagedIndex {
        static constexpr size_t kPage = 4096u;
        /// Ids at or past this (16.7 M) are kept in a hash: the counters are dense from 1, so only an id the engine did
        /// not hand out lands there, and a page table never grows to a stray id's size.
        static constexpr unsigned long long kDirect = 1ull << 24;
        std::vector<std::unique_ptr<Node *[]>> pages;
        std::unordered_map<unsigned long long, Node *> far;
        Node *get(Key id) const {
            const unsigned long long k = (unsigned long long)id;
            if (k >= kDirect) { auto it = far.find(k); return it == far.end() ? nullptr : it->second; }
            const size_t p = size_t(k / kPage);
            return p < pages.size() && pages[p] ? pages[p][size_t(k % kPage)] : nullptr;
        }
        void set(Key id, Node *n) {
            const unsigned long long k = (unsigned long long)id;
            if (k >= kDirect) { if (n) far[k] = n; else far.erase(k); return; }
            const size_t p = size_t(k / kPage);
            if (p >= pages.size()) { if (!n) return; pages.resize(p + 1u); }
            if (!pages[p]) {
                if (!n) return;
                pages[p].reset(new Node *[kPage]);
                std::fill(pages[p].get(), pages[p].get() + kPage, nullptr);
            }
            pages[p][size_t(k % kPage)] = n;
        }
        void clear() { pages.clear(); far.clear(); }
        /// The ids its pages can hold (pages x kPage) and the pages allocated.
        size_t reach() const { return pages.size() * kPage; }
        size_t allocated() const { size_t k = 0; for (const auto &p : pages) k += p ? 1u : 0u; return k; }
    };
    PagedIndex<Ogre::IdType> mNodeByOgreId;
    /// ...and the registry id -> the record (record(): the node records, the change set, the scans' names), the same
    /// address-stable record; inserted at track, erased with the record (U4 stage 3).
    /// (decision 12.1: the document's ids come from ONE process-wide counter too — reserveNodeId — so the same
    /// paged direct index, never a hash.)
    PagedIndex<NodeId> mNodeById;
    /// THE SCENE'S PUBLISHED FRAME (Scene::published, PublishedFrame): a whole-frame SWAP (Engine.h's contract) —
    /// publishFrame writes the buffer published() is not returning, then flips the index.
    mutable SnapshotBuffers<PublishedFrame> mPublished;   ///< three buffers and the host's latch (U3 stage 5)
    /// Planar reflectors asked for on DOCUMENT nodes this frame, armed at the drain once their poses landed.
    std::vector<NodeId> mReflectorsToArm;
    /// The nodes riding a bone tag right now (attachToBone / releaseBoneTag): publishFrame's whole walk.
    std::vector<NodeId> mBoneRiderIds;
    /// A document node's graph class -> its Ogre node, its attachments and the helper nodes hanging off it.
    void enforceGraphClass(Node &rec);
    /// After a reparent: the node and every document descendant re-told its own class (Ogre migrated the subtree).
    void enforceSubtreeClass(Node &top);
    /// One record of applyNodeRecords, under its own try (a throw drops that record only).
    bool applyNodeRecord(const NodeRecord &r, unsigned long long stepSerial);
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
    /// ATOM stage 1: the scene-wide LOD dial. 1 = the reference budget of one
    /// pixel of geometric error; 0 pins every object at level 0.
    float mLodBias = 1.0f;
    /// ATOM-3 stage 1: the World Mode's pixel-error row, in pixels (kLodBudgetPixels until pushed).
    float mPixelTolerance = kLodBudgetPixels;
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
    TextureId trackTexture(const TextureRec &rec, TextureId id);   ///< under `id` (0 = reserve one)
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
    bool              mIblPending = false;   // convolve into mReflPendingTex
    /// One-shot ibl_specular workspace; kept null between runs.
    Ogre::Camera *mIblCamera = nullptr;
    bool mRefractionsActive = false;   // see setRefractionsActive
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

    /// `queue`: the item is GI-visible and its datablock not converted yet — the slot is owed to the material
    /// store's queue (the composer's serial tail does the push, U4 stage 4).
    uint32_t gpuMaterialWordFor(const Node &n, Ogre::uint32 flags, bool &queue) const;


    /// THE SUBSTRATE — the scene's ONE writer of its GPU description (GpuSceneSubstrate.h,
    /// ENGINE-UPGRADE-2 stage 1): the tables, the dirty scan and its frame protocol, the
    /// counters, the ray structures. Mutable because `ensureGpuScene` is const.
    mutable detail::GpuSceneSubstrate mSubstrate;
    /// THE NAMES THE REST OF THE ENGINE READS, aliases into the substrate for the parallel
    /// window (OgreGi.cpp, which names mGpuScene and mGpuForced, is EU-1's until U0 merges;
    /// stage 4 renames them): the tables, the explicit marks, and the three counters the
    /// GPU-scene accessors above return.
    detail::GpuScene &mGpuScene = mSubstrate.tables;
    std::vector<uint32_t> &mGpuForced = mSubstrate.marks;
    const unsigned long long &mGpuScans = mSubstrate.walks;
    const unsigned long long &mGpuAabbReads = mSubstrate.aabbReads;
    const double &mGpuScanMicros = mSubstrate.scanMicros;
public:
    /// THE FRAME'S OWN READ (OgreEngine::applyShadowCacheDirties, after updateSceneGraph):
    /// the compose (the frame's walk unless a pre-frame reader already ran it, and every
    /// mark since), then — after the ray levels have patched the mirror — ONE commit.
    void composeGpuSceneFrame() const;
    void commitGpuSceneFrame() const;
    /// THE CUT-OUTS' MASK TABLE (ENGINE-POLISH-ATOM-1 stage 3; OgreAlphaMasks.cpp): after the frame's commit and
    /// before the ray tier and every pass — every shown alpha-tested slot's masks asked of the engine's store (made
    /// on the device the first frame their textures are resident) and named, with their UV transforms, in the GPU
    /// scene's table (GpuScene::maskTable*). A cut-out whose mask was still being made routes Pending (the stock
    /// shader draws it); the frame its mask is made, its slot is re-composed.
    void updateMaskTable();
    /// The masks the last table build served STALE (an older content's, past the frame's budget): the ray tier's
    /// RayQueryStatus::alphaStaleServed.
    unsigned long long maskStaleServed() const { return mMaskStaleServed; }
private:
    /// THE CUT-OUTS THE TABLE COVERS, kept by the change feed (a walk over every slot every frame is what the feed
    /// replaced, ATOM-CPU-WALKS-1): the shown alpha-tested slots, and per member its first sub-item's datablock when
    /// last told (a material swap on a member re-writes the table; an edit in place moves the material serial).
    /// REGISTERED BY THE FIRST TABLE BUILD (updateMaskTable: a late observer is primed with every live slot) and
    /// unregistered by its own destructor (declared after the substrate, so it dies first); the table's reset
    /// (GpuScene::destroy, the scene's teardown) gives the store back the masks it named.
    struct MaskFeed final : detail::GpuSceneObserver {
        OgreScene *scene = nullptr;
        detail::GpuScene *table = nullptr;
        std::set<uint32_t> alpha;
        std::vector<const void *> dbOf;
        /// The masks the scene's table names (sorted store ids: AlphaMaskStore::retarget's users).
        std::vector<unsigned long long> ids;
        bool dirty = true;
        void gpuSlotChanged(uint32_t slot, const detail::GpuInstance *now) override;
        void gpuSceneReset() override;
        MaskFeed() = default;
        MaskFeed(const MaskFeed &) = delete;
        MaskFeed &operator=(const MaskFeed &) = delete;
        ~MaskFeed() override;
    };
    MaskFeed mMaskFeed;
    /// THE COVERAGE A/B's TWO SNAPSHOTS (coverageSnapshot, a measuring door; OgreShadow.cpp).
    struct CoverageShot {
        bool valid = false;
        unsigned w = 0, h = 0;
        std::vector<float> depth;
        Ogre::Matrix4 proj = Ogre::Matrix4::IDENTITY;   ///< the camera's, with the render system's depth range
        /// per pixel: the id pass's slot is a cut-out it drew (kGpuAtom | kGpuAlphaTested at the snapshot)
        std::vector<unsigned char> masked;
        struct Map { unsigned map = 0, w = 0, h = 0, atlasW = 0; Ogre::Matrix4 vp = Ogre::Matrix4::IDENTITY; std::vector<float> d; };
        std::vector<Map> maps;
    };
    CoverageShot mCoverageShots[2];
    std::vector<uint32_t> mMaskSet, mMaskReady;
    unsigned long long mMaskMaterialSerial = ~0ull, mMaskUploadSerial = ~0ull, mMaskEpoch = ~0ull,
                       mMaskTableSerial = ~0ull, mMaskStaleServed = 0ull;
    /// The highest level a fragment may sample at the last build (the arm atom.maskPyramid: kMaskAllLevels or 0).
    uint32_t mMaskLevelCap = ~0u;
    bool mMaskPending = false;
public:
    // ---- THE READ-ONLY GPU-SCENE VIEW (ENGINE-UPGRADE-2 stage 2) -----------------------
    // What a reader outside the scene (the ray tier) learns about the GPU scene's slots,
    // without the scene's members: the substrate (the tables it reads; the ray
    // structures slot it fills), a slot's or a node's item facts, and the one write a
    // reader may ask for — a slot re-composed (its skin row changed).
    const GpuSceneSubstrate &substrate() const { return mSubstrate; }
    /// THE ONE WRITE a reader makes into the substrate: the ray tier fills and releases
    /// the scene's ray structures slot (created on the scene's first ray frame, released
    /// while the device lives — GpuSceneSubstrate::rays). Everything else is read-only.
    GpuSceneSubstrate::RayStructures &rayStructuresSlot() const { return mSubstrate.rays; }
    struct GpuSlotView {
        uint32_t node = 0;                        ///< the node's id
        const Ogre::Item *item = nullptr;
        unsigned long long rigGeneration = 0;     ///< moves when the Item is rebuilt
        /// The pose pushes a skin is keyed on: the node's own and, for a shareSkeleton
        /// follower, its master's (the caster walk's recipe).
        unsigned long long poseSerial = 0;
    };
    bool gpuSlotView(uint32_t slot, GpuSlotView &out) const;
    bool gpuNodeView(uint32_t node, GpuSlotView &out) const;
    /// THE GPU'S NODE KEY (U3 stage 3, F6): Ogre's own 32-bit IdType of the node's SceneNode, never the 64-bit id —
    /// what GpuInstance::ids[0], the skin rows, the substrate's names and the slot views carry; and back to the id.
    static uint32_t gpuKeyOf(const Node &n) { return uint32_t(n.ogreId); }
    NodeId nodeOfGpuKey(uint32_t key) const;
    /// The node's slot is re-composed by the next compose (no-op for a node with no Item).
    void markGpuNodeDirty(uint32_t node);
    /// What the ray structures are current against: the caster epoch (a transform, a
    /// pose, a caster input) - the ray tier's read-only view of it. (A near copy's level
    /// never changes since PHOTON-7 stage 6: level 0, so no refit enters it.)
    unsigned long long rayStructureEpoch() const;
    /// PHOTON (photon/PhotonComponent.h): the GI's products, read through its interface.
    const PhotonComponent &photon() const { return mPhoton; }
    /// photon_update's recorder (the pass provider's): the component records its frame's GI work.
    void recordPhotonUpdatePass() { mPhoton.recordUpdatePass(); }
    // ---- THE SCENE AS PHOTON READS IT (ENGINE-UPGRADE-2B stage 1) -----------------------
    // PhotonComponent's back-reference reads these scene facts (the four it reads most — the
    // root, the manager, the engine, the error sink — are bound by reference when the scene
    // constructs it), and calls the scene's walk and GPU-scene seams below by name. Stage 2's
    // plan() takes the frame's record instead of most of them.
    const std::map<NodeId, Node> &nodeRecords() const { return mNodes; }
    const std::vector<Node *> &itemNodes() const { return mItemNodes; }
    const std::map<MaterialId, MaterialRec> &materialRecords() const { return mMaterials; }
    const std::map<TextureId, TextureRec> &textureRecords() const { return mTextures; }
    /// The scene's per-pass binding record (SceneGiBinding): the component binds its arms.
    const SceneGiBinding &giBinding() const { return mGiBinding; }
    void bindGiVct(Ogre::PhotonVoxelLighting *vct);
    void bindGiIfd(Ogre::PhotonIrradianceField *ifd);
    void bindGiPcc(Ogre::ParallaxCorrectedCubemapBase *pcc, float minDist, float maxDist);
    bool unbindGiPcc(const Ogre::ParallaxCorrectedCubemapBase *pcc);   ///< true when it was bound
    bool demoteDragMover(NodeId id, Ogre::Aabb &rest);
    bool sceneLoading() const { return mSceneLoading; }
    FramePace framePace() const { return mFramePace; }
    bool inStructureEdit() const { return mStructureEditDepth > 0; }
    bool gestureActive() const { return mGesture.active; }
    /// The sky's environment is still being captured (its cube, its convolution, its SH).
    bool skyEnvironmentPending() const {
        return mSkyDesc.mode != SkyMode::NoSky &&
               (mSkyCapturePending || mIblPending || mSkyShTicket != nullptr);
    }
    void countGiInvalidation() { ++mChangeStats.giInvalidations; }
    void countProbeBuildRestart() { ++mChangeStats.probeBuildRestarts; }
    /// THE WALK's own counters into a status (the walk is the scene's: walkItems).
    void giWalkStatusInto(GiStatus &st) const {
        st.giScans      = mGiScans;
        st.giScansFull  = mGiScansFull;
        st.giScanMicros = mGiScanMicros;
    }
    /// The frame's GI walk gate opens (the GI tracking's head, once a frame): the first
    /// consumer of mGiMovedBoxes this frame runs the walk (ensureGiWalk).
    void openGiWalkFrame() { mGiWalkedThisFrame = false; }
    /// THE SCENE'S PASS RECORD (FogHlmsListener::SceneBinds): what its passes bind, found through
    /// its manager (sceneOfManager).
    FogHlmsListener::SceneBinds &passRecord() { return mPassRecord; }
    unsigned long long textureUploadSerial() const { return mTextureUploadSerial; }
    unsigned long long cloudGiSerial() const { return mCloudGiSerial; }
private:
    PhotonComponent mPhoton{ *this, mRoot, mSceneMgr, mEngine, mError };
    /// THE SCENE'S TWO GI SEAMS: every structural edit (an attach, a detach, a material or
    /// texture replaced, a light removed, a hide or a show) and every probe input reaches
    /// Photon through these (PhotonComponent::invalidateGiCaches / staleProbeGrid have the rules).
    void invalidateGiCaches() { mPhoton.invalidateGiCaches(); }
    void invalidateGiCaches(const Ogre::Aabb *where) { mPhoton.invalidateGiCaches(where); }
    void invalidateGiCaches(const Ogre::Aabb *where, bool geometryVoxelsChanged) {
        mPhoton.invalidateGiCaches(where, geometryVoxelsChanged);
    }
    /// The voxeliser told "somewhere", the changed-region list `listed` (PhotonComponent::invalidateGiCachesListed).
    void invalidateGiCachesListed(const Ogre::Aabb &listed) { mPhoton.invalidateGiCachesListed(listed); }
    /// THE WORLD BOX OF EVERY ITEM UNDER `sn` (itself included): a subtree's edit box for the list (an empty box when
    /// none holds an item).
    Ogre::Aabb subtreeItemBox(Ogre::SceneNode *sn) const;
    void staleProbeGrid(GiStaleReason why) { mPhoton.staleProbeGrid(why); }
    /// ATOM (atom/AtomComponent.h), on the substrate it reads; its one input from the scene
    /// (the adapter until the frame's record): the datablock a slot's Item wears.
    const Ogre::HlmsDatablock *atomSlotDatablock(uint32_t slot) const;
    AtomComponent mAtom{ mRoot, mSceneMgr, mSubstrate, [this](uint32_t slot) { return atomSlotDatablock(slot); } };
    /// The substrate's steps (OgreGpuScene.cpp): the compose (marks, then the walk —
    /// the compare over every slot as a UniformScalableTask — staging the changed
    /// slots), the commit (the device copy), and the stage they file.
    void composeGpuScene(bool graphIsCurrent) const;
    void walkGpuScene(bool graphIsCurrent) const;
    void checkWalkParity(bool atomDraw) const;   ///< the arm gpuscene.walkParity
    void commitGpuScene() const;
    void noteGpuSceneStage(double micros, bool walked) const;
    /// `gpuFlagsFor` with the split's scene-wide switch (atomDrawOn) handed in: the
    /// walk asks it once, not once per slot.
    Ogre::uint32 gpuFlagsFor(const Node &n, bool atomDraw) const;
    /// Said once: a mesh with more submeshes than the geometry row table holds
    /// (GpuScene::kSubmeshesPerMesh). Every mesh this engine bakes has one.
    mutable bool mWarnedGeomSubmeshes = false;
    /// ATOM P3's CULL — its result buffers, sized to the table's capacity and
    /// grown with it (GpuCull.h). Owned per SCENE because that is what the
    /// tables it reads are owned by; a consumer that wants two culls of one
    /// scene in a frame is stage 3's problem and gets a second instance.
    detail::GpuCull mGpuCull;
    /// THE ONE PLACE the per-item predicates are computed (GpuInstanceFlag).
    Ogre::uint32 gpuFlagsFor(const Node &n) const;
    /// THE RENDER-QUEUE SPLIT'S ONE DECISION (ATOM S3-DRAW, OgreAtomDraw.cpp):
    /// does the id pass draw this item and the decode shade it, and if not, the
    /// first reason (AtomDrawStatus names them in order). `Stock` = an item in a
    /// queue the split never touches.
    enum class AtomRoute : uint8_t {
        Atom, NotWorld, NotPbs, CustomPiece, Blended, CullFront, Planar, Pending, AlphaTested, Skinned, NoRow, Stock
    };
    AtomRoute atomRouteFor(const Node &n, Ogre::uint32 flags) const;
    /// THE CASTER'S ROUTE (ENGINE-POLISH-ATOM-1 stage 2): the caster cut draws this item's caster — an Atom item, or
    /// a Blended one (its colour stays on PBS; its stock caster is the opaque silhouette) with no alpha test, no
    /// skin, a caster that culls back faces or none, and a row the cut reads. gpuFlagsFor's kGpuAtomCaster.
    bool atomCasterThroughCut(const Node &n, Ogre::uint32 flags, AtomRoute route) const;
    /// THE ROW TESTS (atomRouteFor's NoRow): one submesh, a triangle list, a mesh slot with levels, a geometry row
    /// with both device addresses and a float3 normal — and, for the decode (`decode`), a material word.
    bool atomRowReadable(const Node &n, const Ogre::HlmsDatablock *db, bool decode) const;
    /// A CUT-OUT'S MASK, as the route sees it (ENGINE-POLISH-ATOM-1 stage 3, shipped by ATOM-3 stage 2): Ready
    /// (made, or an older content's served until remade; or the material has no alpha test of its own), Pending,
    /// Refused (never: the stock route) — the engine's store, read only (the compose's lanes call it).
    AlphaMaskStore::State cutoutMaskState(const Ogre::HlmsDatablock *db) const;
public:
    /// The split is live in this scene: the GPU scene exists, the id pass can run on
    /// this device (OgreAtomIdPass.cpp) and the measurement door is open. Scene's public read too (a request on the
    /// host's side, BUGS-1 item 10).
    bool atomDrawOn() const override;
    /// The split is WANTED here: the door is open and this device runs the id pass.
    /// What a view's chain SHAPE reads — never the GPU scene's liveness, which
    /// arrives during the first frame and would rebuild every new view's workspace
    /// one frame in (the id pass of a scene whose table is not live yet clears the
    /// depth and draws nothing; every item is still on PBS then).
    bool atomDrawWanted() const;
    /// THE ID PASS'S OCCLUSION IS WANTED here (ChainDesc::atomOcclusion): the split is,
    /// and the occlusion's measuring door is open.
    bool atomOcclusionWanted() const { return atomDrawWanted() && mAtom.occlusionEnabled(); }
    /// THE WORDS STEP (AtomComponent::step), once per frame after the GPU scene's commit
    /// (OgreShadow.cpp's post-graph block) and in the warm-up's second frame.
    void updateAtomDraw() { mAtom.step(); }
    /// THE ATOM VIEW CAN PAINT in this scene's viewport: the split is live and no
    /// view of the scene draws straight into a window without the id pass (the Low
    /// tier's passthrough viewport — AtomDrawStatus::passthroughViews). A VR eye
    /// pair carries no id pass either, but the desktop view beside it does.
    bool atomViewPaintable() const { return atomDrawOn() && !mAtom.hasPassthroughViews(); }

    // ---- THE PHOTON VIEW (PHOTON-VIEW-1, OgrePhotonView.cpp) ----------------
    void setPhotonView(PhotonView view) override {
        if (postScene([=](OgreScene &s) { s.setPhotonView(view); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::setPhotonView");
        mPhoton.setPhotonView(view);
    }
    PhotonView photonView() const { return mPhoton.photonView(); }
    std::string photonViewRefusal(PhotonView view) const { return mPhoton.photonViewRefusal(view); }
    void setPhotonVoxelCascade(int cascade) override {
        if (postScene([=](OgreScene &s) { s.setPhotonVoxelCascade(cascade); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Scene::setPhotonVoxelCascade"); mPhoton.setPhotonVoxelCascade(cascade); }
    void syncPhotonView() { return mPhoton.syncPhotonView(); }
    void releasePhotonView() { return mPhoton.releasePhotonView(); }
    using PhotonViewShape = PhotonComponent::PhotonViewShape;
    void notePhotonView(const void *view, bool presents, const PhotonViewShape &shape) { return mPhoton.notePhotonView(view, presents, shape); }
private:
    /// Files the node's Item in kAtomRenderQueue (atom) or back where its material
    /// puts it (renderQueueFor). Called by composeGpuInstance.
    void placeAtomQueue(const Node &n, bool atom) const;
    /// ...and its stock-caster channel (kStockCasterBit): taken away when the caster cut draws its caster
    /// (kGpuAtomCaster), given back when it does not. Called by composeGpuInstance beside placeAtomQueue.
    void placeAtomCaster(const Node &n, bool casterInCut) const;
public:
    /// OgreView::syncAtomDraw's report, every frame (both false on detach).
    void noteAtomPbsView(const void *view, bool stereo, bool passthrough) {
        mAtom.notePbsView(view, stereo, passthrough);
    }
private:
    /// THE CARD CANDIDATES, kept by the change feed (ATOM-CPU-WALKS-1). The surface
    /// cache used to be handed EVERY item every frame (a walk of every sub-item's
    /// blend state and the mesh's card list, then a world box recomputed up every
    /// parent chain, W13: 6.8 ms at 10k); now the candidate predicate (cardEligible:
    /// shown, GI geometry, opaque, a baked card list) is evaluated when a slot's entry
    /// changes, and a frame offers only THE SLOTS THE CACHE ASKS FOR (its resident
    /// instances and the items whose faces the readers marked: PHOTON-7 stage 5, 5-2 -
    /// the residency sphere and the columns it was queried through are deleted). The
    /// per-frame test itself is unchanged and exact — the cache still measures each
    /// offered item's current box.
    struct CardFeed final : detail::GpuSceneObserver {
        OgreScene *scene = nullptr;
        /// Per slot: a candidate or not.
        std::vector<unsigned char> filed;
        /// Per slot, what the filing measured — the mesh's card list and LOD bounds (a mesh's own facts: they cannot
        /// change while the item wears the mesh) — so a frame's list asks no map per candidate.
        std::vector<const std::vector<MeshCardDesc> *> cardsOf;
        std::vector<const std::vector<float> *> boundsOf;
        /// Per slot, the list serial (`serial` below) of its last change: a slot
        /// the table re-wrote since the last list is measured with the RECOMPUTING
        /// read — its cached world box may predate the frame that moved or made it
        /// — and every other one with the cached box, which for an item nothing
        /// moved is the same bits (measured: 0 of 250,000 differ once settled).
        std::vector<unsigned long long> changedAt;
        unsigned long long serial = 1ull;
        std::atomic<unsigned long long> visits{ 0ull };   ///< the slots told (lane halves count from the workers)
        void gpuSlotChanged(uint32_t slot, const detail::GpuInstance *now) override;
        /// THE LANE HALF (U4 decision 17.1): the slot's own fields (its filing, change stamp, cards and LOD bounds —
        /// one writer per slot, the arrays pre-sized) inside the compose task; nothing is owed to the replay.
        bool hasLaneHalf() const override { return true; }
        const char *gpuObserverName() const override { return "cards"; }
        void gpuSlotsReserve(uint32_t count) override { growTo(count); }
        bool gpuSlotChangedLane(uint32_t slot, const detail::GpuInstance *now) override;
        /// The slot's filing, measured - the one read both halves share.
        bool measure(uint32_t slot, const detail::GpuInstance *now) const;
        void fileSlotFields(uint32_t slot, const detail::GpuInstance *now, bool want);
        /// THE TABLE, FOLDED into the compose's digest (the identity row, decision 22.6): per slot its filing and, filed,
        /// its card list and bounds.
        void gpuDigestInto(detail::GpuScene &g) const override {
            for (size_t s = 0; s < filed.size(); ++s) {
                g.digestFold(&filed[s], sizeof(filed[s]));
                if (!filed[s]) continue;
                g.digestFold(&cardsOf[s], sizeof(cardsOf[s]));
                g.digestFold(&boundsOf[s], sizeof(boundsOf[s]));
            }
        }
        void growTo(size_t count) {
            if (count <= filed.size()) return;
            filed.resize(count, 0u);
            changedAt.resize(count, 0ull);
            cardsOf.resize(count, nullptr);
            boundsOf.resize(count, nullptr);
        }
        void gpuSceneReset() override {
            filed.clear();
            changedAt.clear();
            cardsOf.clear();
            boundsOf.clear();
        }
    };
    CardFeed mCardFeed;
    /// The candidate predicate, one place (the feed files by it, the frame re-reads
    /// its cheap half).
    bool cardEligible(const Node &n) const;
    /// A seam that changed what a slot's entry SAYS without moving anything —
    /// a visibility, light-mask, cast-shadow, render-queue or material write.
    /// The movement epoch cannot see those (a furniture visibility write is
    /// deliberately not scene movement, VR-SCAN-1), so they say so by name.
public:   // (a seam PhotonComponent calls)
    void markGpuSlotDirty(const Node &n);
private:
    /// REFLECT-FIX-1: the kGpuMetal answer for a datablock (OgreGpuScene.cpp).
    static bool gpuIsMetal(const Ogre::HlmsDatablock *db);
    /// THE ITEM'S NODE (ATOM-CPU-WALKS-1): every indexed Item and the node that holds
    /// it, maintained where the item index is (indexItemNode / unindexItemNode) — the
    /// PBS change log names Items, and this is how a note becomes a slot.
    std::unordered_map<const Ogre::MovableObject *, Node *> mNodeOfItem;
public:
    /// A PBS change-log note for one of this scene's Items (drainPbsChanges): its slot
    /// is re-composed by the next scan. Unknown owners (another scene's, a dead one's)
    /// are ignored — the pointer is only a key.
    void markItemRehashed(const Ogre::MovableObject *owner) {
        auto it = mNodeOfItem.find(owner);
        if (it != mNodeOfItem.end() && it->second) markGpuSlotDirty(*it->second);
    }
    /// ...and the log's overflow answer: every item.
    void markAllItemsRehashed() {
        for (Node *n : mItemNodes)
            if (n) markGpuSlotDirty(*n);
    }
    /// ATOM P3's CULL, run once over this scene's table (OgreGpuCull.cpp). `hzb`
    /// null (or a request with hzbLevels 0) is the frustum-only mode.
    /// The recording half of runGpuCull: the request uploaded and the jobs
    /// dispatched, nothing read back — for a consumer inside a frame (the id pass).
    /// Each job is a monitor row in a capture ("cull.test" ... "cull.emit"). The
    /// answer lands in `cull`'s buffers: the scene's own (runGpuCull) or a view's
    /// (the id pass — one instance per view, so two views of one scene in a frame
    /// never share the list one of them is drawing from).
    /// `requestMs` (optional): the host's share — the request's write and the jobs'
    /// bindings, after the buffers exist (GpuCullResult::requestMs).
    /// `prior` (ATOM-OCCLUSION-1's DISOCCLUSION PASS): another list's cull of this frame —
    /// only the instances ITS depth test rejected are tested (the `cull_retest`
    /// permutation), against `hzb`; null is an ordinary request.
    /// `rows` names the jobs' monitor rows (CacheKind::Cull) by WHO records them:
    /// the id pass's "id.cull.cut", a tool's or the chain warm-up's plain
    /// "cull.cut"; a shadow map's caster cull files none (its pass row holds it).
    enum class CullRows { Tool, IdPass, Caster };
    /// THE RECEIVER TEST'S INPUTS (ENGINE-UPGRADE-U5 stage 4b; OgreAtomCasterPass.cpp): a sun map's receiver
    /// grid and its request, read by the cut's per-cluster test (JahCullCut_cs, cut_receivers).
    struct CullReceivers {
        Ogre::UavBufferPacked *params = nullptr;
        Ogre::UavBufferPacked *grid = nullptr;   ///< the grid's farthest-receiver pyramid, its levels in a row
    };
    /// `probe` (ATOM-2): the view's cut-probe buffer for this list in the probe's frame (atomCutProbeBuffer), null
    /// otherwise — the cut job's `cut_probe` permutation records every group decision into it. Never with receivers.
    bool recordGpuCull(GpuCull &cull, const GpuCullRequest &req, Ogre::TextureGpu *hzb,
                       std::string &err, double *requestMs = nullptr,
                       const GpuCull *prior = nullptr, CullRows rows = CullRows::Tool,
                       const CullReceivers *receivers = nullptr, Ogre::UavBufferPacked *probe = nullptr,
                       Ogre::UavBufferPacked *hzbProbe = nullptr);
    bool runGpuCull(const GpuCullRequest &req, Ogre::TextureGpu *hzb, bool readBack,
                    GpuCullResult &out);
private:
    void composeGpuInstance(const Node &n, const Ogre::Matrix4 &world, bool graphIsCurrent,
                            detail::GpuInstance &out) const;
    /// THE COMPOSER, SPLIT FOR THE WORKERS (ENGINE-UPGRADE-U4 stage 4): the lane half READS only (the node, its Item,
    /// its datablock, the material store's map, the skin rows) and returns what the serial tail owes — the Atom
    /// route's render-queue edit (Ogre state) and the material store's queue push (a shared vector) — which the walk
    /// replays in slot order after the lanes, the serial compose's order. composeGpuInstance = lane + tail.
    struct ComposeTail { bool atom = false, queueMaterial = false, casterInCut = false; };
    void composeGpuInstanceLane(const Node &n, const Ogre::Matrix4 &world, bool graphIsCurrent,
                                detail::GpuInstance &out, ComposeTail &tail) const;
    void applyComposeTail(const Node &n, const ComposeTail &tail) const;
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
    std::vector<Node *> mDecalNodes;
    void indexDecalNode(Node &n);
    void unindexDecalNode(Node &n);
    /// ONE WALK, TWO CONSUMERS (walkItems, via runItemWalk): the GI movement
    /// records and the frame's caster changes, from the same pass.
    struct ShadowChange { Ogre::Aabb box; Ogre::uint32 channels = 0; };
    void walkItems(bool gi, bool shadow, bool fresh, const std::vector<Node *> *subset = nullptr);
    std::vector<ShadowChange> mShadowChanges;
    std::vector<ShadowChange> mShadowVanished;   ///< casters whose Item died since the last walk
    bool mShadowWalked = false;                  ///< this frame's walk produced mShadowChanges
    /// The GI movement scan has run this frame (reset by updateGiTracking, the
    /// once-per-frame entry point): whoever consumes mGiMovedBoxes first runs it.
    bool mGiWalkedThisFrame = false;
    /// THE GEOMETRY SIGNATURE'S NAMES (U4 stage 3): the nodes written since the signature last read (each once a
    /// take), or a full pass owed — the item set changed, a node died (the list holds records), a nodeless write, an
    /// incomplete change set, or the list grew to a full pass's size.
    mutable bool mGeomSigFull = true;
    std::vector<Node *> mGeomSigTake;   ///< the signature's read of the name list (scratch)
    bool mBoxesCurrent = false;
    struct GiPlanStash {
        bool owed = false, pushed = false, materialPreview = false;
        std::optional<unsigned long long> refreshSerial;
    } mGiPlanStash;
    mutable unsigned mCountRecordLookups = 0u, mCountNameVisits = 0u, mCountNameLookups = 0u;   ///< takeDrainCounts
public:
    void nameForGeomSig(Node &n) { nameFor(n, kNameSig); }
    void owesGeomSigFull() { owesNamesFull(kNameSig); }
    bool geomSigFullOwed() const { return mGeomSigFull; }
    /// The signature's take (its bit released in the same pass).
    const std::vector<Node *> &geomSigNames() { takeNamed(kNameSig, mGeomSigTake); return mGeomSigTake; }
    /// The signature read them (a full pass reads none): its bit released, its full pass paid.
    void tookGeomSigNames() { releaseNamed(kNameSig); mGeomSigFull = false; }
private:
    /// THE WALK'S GI RECORD and the inputs it was walked with (ensureGiWalk).
    PhotonComponent::WalkRecord mGiWalk;
    PhotonComponent::WalkInputs mGiWalkIn;
    FogHlmsListener::SceneBinds mPassRecord;
    /// THE CHANGE SET'S ENGINE HALF (U0 3b). The scans re-read only the items NAMED since they last ran — the drained
    /// ChangeSet's subtrees (applyChangeSet) and the engine's own seams (noteSceneTransformWrite(n)) — and nothing on a
    /// frame that named none; a nodeless seam or an incomplete set owes the full pass. `transformEpoch()` = the moves
    /// drained (mHostMoves) + this scene's own writes: a count that moves exactly when something was written.
public:   // (a seam PhotonComponent calls)
    unsigned long long transformEpoch() const { return mHostMoves + mSceneTransformWrites; }
    void noteSceneTransformWrite(Node &n);   ///< NAMED: n's item is re-read by both scans (OgreGi.cpp)
private:
    /// An ENGINE write that moves a node's subtree (a rider, a decal box, setNodeTransform): named like a drained move.
    void noteSubtreeWrite(NodeId id) { ChangeSet c; c.moved.push_back(id); applyChangeSet(c); }
    std::vector<Node *> mScanTakeNodes;
    std::vector<NodeId> mScanTake;   ///< the shadow walk's deformer scratch
    unsigned long long mShadowScansFull = 0;            ///< full caster scans, ever (GpuSubstrateStatus::scansFull)
public:
    /// The full GI-movement and caster scans, ever (GpuSubstrateStatus::scansFull).
    unsigned long long scansFull() const { return mGiScansFull + mShadowScansFull; }
private:
    std::vector<Node *> mScanSubset;
    std::vector<Node *> &takeScanSubset(uint8_t bit);
    mutable bool mScanFullGi = true, mScanFullShadow = true;
    unsigned long long mHostMoves = 0;
    /// THE SAME SKIP FOR THE CASTER HALF (ENGINE-4 F5). It was left out when
    /// the GI half got its gate because a caster's inputs are not all
    /// transforms — a pose, a material's vertex piece, a rebuilt Item, a Cast
    /// Shadow flag — so it ran O(items) every frame for every drawn scene with
    /// a cacheable lamp, still or not. Those inputs are PUSHED at their own
    /// seams, and each of them now bumps mShadowScanWrites, so the caster half
    /// can ask the same question the GI half asks: could anything I read have
    /// changed since I last looked?
public:   // (a seam PhotonComponent calls)
    unsigned long long shadowEpoch() const { return transformEpoch() + mShadowScanWrites; }
private:
    unsigned long long mShadowScanWrites = 0;     ///< pushed caster inputs that move nothing
    unsigned long long mCasterWalkItems = 0;      ///< item visits by the caster half, ever
    unsigned long long mSceneTransformWrites = 0; ///< OUR writes: setNodeTransform, riders
    unsigned long long mGiScans = 0;              ///< movement scans actually run, ever
    unsigned long long mGiScansFull = 0;          ///< ...of them the full ones (GiStatus::giScansFull)
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
    using ProbeSlot = PhotonComponent::ProbeSlot;
    /// EVERY WRITE A LIGHT INJECTION WOULD READ (DRAG-1 round 2, F5): a light's
    /// parameters (setLight) and its POSE (setNodeTransform on a node that owns
    /// one — a movable lamp never stales the probe grid, so nothing else sees
    /// it). A light LEAVING the scene does not move it: that is mLightsRemoved,
    /// and a reader of the light set gates on the pair (ENGINE-UPGRADE-U5 decision
    /// 14.1). Monotonic; only ever compared.
    unsigned long long mGiLightWriteSerial = 0;
    /// Lights destroyed (removeLight, removeNode) — the world light list's dirt
    /// beside mGiLightWriteSerial (SceneGiBinding::lightsRemoved).
    unsigned long long mLightsRemoved = 0;
    /// changeStats(): the structural edits' counts (ENGINE-UPGRADE-1 stage 2).
    SceneChangeStats mChangeStats;
public:
    /// THE FRAME INPUT THIS SCENE LAST APPLIED (U0 3a): the drain's compare (OgreEngine::drainFrameInput).
    FrameInputScene mFrameApplied;
    /// THE CHANGE SET's engine entry point (U0 3b): names each moved subtree to the scans; returns 1 when a light
    /// among them really moved (its pose key changed — the light serial bumped once, the scheduler told).
    unsigned applyChangeSet(const ChangeSet &cs);
    void planGi(const FrameInputScene &part, bool pushed) { return mPhoton.plan(part, pushed); }
    /// The user's gesture, as the scene's last record that carried one said (FrameInputScene::gesture).
    FrameInputGesture mGesture;
private:
    /// beginStructureEdit / endStructureEdit: the open depth, and what the edits inside asked of GI
    /// (applied once at the outermost end — applyGiInvalidation).
    int  mStructureEditDepth = 0;

    /// A lamp event for the scheduler (setLight, removeLight, a shown flip, a pose that moved, a class flip).
    /// ...AND THE LAMP'S REGION (PHOTON-7 stage 3; OgreGi.cpp): its range sphere as last listed joined with its sphere
    /// now - a light hidden, detached or about to be destroyed (`gone`) has none - into the frame's changed-region
    /// list; a DIRECTIONAL light's event is the sun's: an overflow, never a box.
    void listLampChange(Node &n, bool gone = false);
    /// The movement scan has run at least once: before that, every item is
    /// seen for the first time and none of them is an arrival.
    bool mGiScannedOnce = false;
    /// How many of those rebuilds a MOBILITY change caused (MobilityStatus::
    /// mobilityRebuilds). Its own counter and not a share of mGiRebuilds
    /// because the question it answers is "is my classification costing me
    /// rebuilds?", which a total cannot: every other rebuild reason (a mode
    /// change, a destroyed object, a quality dial) is mixed into that one.
    unsigned long long mMobilityRebuilds = 0;
    /// THE RUNNING TOTALS (U3 read 12(b)): what every node's Counted adds up to; instances by distinct skeleton.
    MobilityStatus mMobilityCounts;
    RigStats       mRigCounts;
    std::unordered_map<const Ogre::SkeletonInstance *, size_t> mSkeletonRefs;
    /// ...and the per-node lists the snapshot carries (U3 stage 2b), ordered by id: the particle-system nodes, the
    /// bones each rigged node streams.
    std::set<NodeId>            mParticleNodeIds;
    std::map<NodeId, size_t>    mStreamedByNode;
    std::map<NodeId, NodeId>    mShareSources;   ///< follower -> source, kept by applyCounted
    /// THE FACTS BLOCK (U3 stage 3, decision 13): rebuilt at the publish only when a mutation site raised the flag.
    bool                                        mFactsDirty = true;
    /// THE NAMED MESSAGES' COMPLETIONS (U3 stage 3): the last PublishedFrame::kCompletionFrames frames, republished on change.
    std::deque<PublishedFrame::Completion>      mCompletions;
    unsigned long long                          mCompletionSeq = 0;
    bool                                        mCompletionsDirty = false;
    unsigned long long                          mCompletionFrame = 1;   ///< the frame the next drain's answers carry
    std::shared_ptr<const std::vector<PublishedFrame::Completion>> mCompletionsPublished;
public:
    void noteCompletion(unsigned long long token, bool ok, std::string error);
private:
    std::shared_ptr<const PublishedFrame::Facts> mFacts;
    void rebuildFacts();
    /// Re-derives one node's contribution after a mutation of its movable flag, Item, light, particles, billboards or
    /// share (recountNode), or takes it out (uncountNode: the node is leaving).
    void recountNode(Node &n);
    void uncountNode(Node &n);
    void applyCounted(Node &n, const Node::Counted &now);
    size_t streamedBonesOf(const Node &n) const;
    /// WHAT THIS SCENE'S PASSES BIND (SceneGiBinding): registered under mSceneMgr
    /// at construction; each field is written where the arm is finished and
    /// cleared where it dies — nowhere else.
    SceneGiBinding mGiBinding;
    /// THE SCENE'S ONE LIGHT LIST (P1C-LIGHT-LIST): dirtied by mGiLightWriteSerial and
    /// mLightsRemoved, published through mGiBinding.lightList, reset in destroy().
    SceneLightList mLightList;
    /// The last ambient SH and fog the scene was given, so a host re-push of the
    /// same value (every page return drops the host's own latch) stales nothing.
    float   mLastAmbientSh[27] = {};
    bool    mAmbientShKnown = false;
    FogDesc mLastFogDesc;
    bool    mFogDescKnown = false;
    /// Live decals in THIS scene. The SceneManager-level atlas binding is
    /// driven off the count (see refreshDecalBindings).
    unsigned            mDecalCount = 0;
    // Planar-reflection arm. mPlanar is null unless mPlanarParams.budget > 0.
    // mReflectors is the DOCUMENT's set of reflector nodes and survives the arm
    // going up and down; mActors only exists while the arm is up.
    /// Ogre's own type since fork 618d95cca (was 0052) gave PlanarReflections the two
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
    /// How many times guardSamplerCeiling has had to step off a full block, and
    /// whether the one log line has been written. See it for what a generation
    /// is; 0 is the state every scene that ever shipped stays in.
    unsigned            mSamplerGeneration = 0;
    bool                mSamplerCeilingLogged = false;
};

// ---------------------------------------------------------------------------
/// THE FIRST CULL'S HISTORY (ATOM-OCCLUSION-1, OgreView::atomOcclusionHistory): what the
/// pyramid in a view's `jahHzb` was built from. The late pass writes it once the
/// pyramid of its frame is built; the next frame's first cull tests against that
/// pyramid only while everything here still names the same pyramid.
struct AtomOcclusionHistory {
    bool valid = false;
    /// The row-major view-projection the pyramid's depth was DRAWN with, in the cull's
    /// row convention (OgreAtomIdPass.cpp's cullMatrixOf).
    float viewProj[16] = {};
    /// The viewport rectangle in mip 0, and the pyramid's size and levels.
    unsigned rect[4] = { 0u, 0u, 0u, 0u };
    unsigned width = 0u, height = 0u, levels = 0u;
    /// The workspace generation (a rebuilt graph's pyramid is a new texture) and the
    /// scene the depth was of (a switch of world is a cut).
    unsigned generation = 0u;
    const void *scene = nullptr;
};

/// THE VIEW'S ID DEPTH OF THIS FRAME (ENGINE-UPGRADE-U5 stage 4b): what the sun maps' receiver grid is
/// built from (OgreAtomCasterPass.cpp) - the depth the id pass wrote, the matrix it drew with, its
/// viewport and the frame. The id pass runs before every scene pass of the view, so the view's shadow
/// node renders after it in the same frame.
struct AtomReceiverDepth {
    Ogre::TextureGpu *depth = nullptr;
    Ogre::Matrix4 drawMatrix = Ogre::Matrix4::IDENTITY;
    unsigned rect[4] = { 0u, 0u, 0u, 0u };
    uint32_t frame = ~0u;
    /// The workspace whose id pass wrote it: only that workspace's shadow node may read it.
    const Ogre::CompositorWorkspace *workspace = nullptr;
};
/// THE SUN MAPS' CASTER JOBS, WARMED (OgreAtomCasterPass.cpp; OgreView::warmUpShaders): a warm-up frame never runs the
/// light's pyramid (made on a map's second frame) nor the receiver grid (it needs a frame's id depth), so one of each
/// is recorded here against the view's own depth and pyramid - the rectangle's pyramid build, the receiver grid's
/// three jobs and the cut's receiver test - into scratch the device frees behind the frame. Nothing reads them.
void warmCasterPassJobs(OgreScene *scene, Ogre::RenderSystem *rs, const Ogre::Camera *camera, Ogre::TextureGpu *depth,
                        Ogre::TextureGpu *hzb);

/// THE PRESENTED FRAME'S CAPTURE (View::requestFrameCapture, CLOSE-SHOT-2). One per
/// view, registered through OgreView::addWorkspaceListener while a capture is armed
/// (so it rides the view's workspace AND the clean capture's). The furniture and the
/// HUD are baked out of the capture's own definition (OgreView::ensureCaptureWorkspace),
/// so this listener does two things only. `passPreExecute`, in the capture's
/// workspace, hands each scene pass that names the view's shadow node in the view's
/// chain the view's own instance of this frame (Ogre's hook "fire the listener in
/// case it wants to change anything", CompositorPassScene::execute, after the pass
/// set its own — none — and before it renders). `workspacePosUpdate` records the copy
/// of the final target: the window's at the end of the view's workspace (keepHelpers,
/// or alsoPresented's companion), the capture target's at the end of the capture's.
class FrameCaptureListener final : public Ogre::CompositorWorkspaceListener {
public:
    explicit FrameCaptureListener(OgreView *view) : mView(view) {}
    void passPreExecute(Ogre::CompositorPass *pass) override;
    void workspacePreUpdate(Ogre::CompositorWorkspace *ws) override;
    void workspacePosUpdate(Ogre::CompositorWorkspace *ws) override;
    /// The RenderQueue's placeholder draws + pending skips when the workspace began
    /// (ASYNC-SHADERS-1): a capture whose workspace moved them is incomplete and retried.
    unsigned long long mIncompleteAt = 0;
private:
    OgreView *mView;
};

/// THE VIDEO READBACK (VIDEO-REC-1; View::setVideoReadback). One per view that
/// asks, owned by it, registered through OgreView::addWorkspaceListener while it
/// lives. At the end of the view's OWN workspace on an armed frame it runs
/// `Jahshaka/VideoNv12` (the final target -> an R32_UINT target holding the NV12
/// bytes) and copies that into the oldest free of three AsyncTextureTickets with
/// frame-count tracking (no flush, no fence of its own: a ticket is done when the
/// frame that recorded it is — VulkanVaoManager::isFrameFinished). The host
/// polls; nothing here ever waits unless the host asks it to (a drain).
class VideoReadback final : public Ogre::CompositorWorkspaceListener {
public:
    static constexpr unsigned kRing = 3u;
    /// `held`: the frames the view has taken off the ring but the host has not yet taken (its queue) — they count
    /// against the ring's three, so a host that stops polling still sees "three read back, the rest dropped".
    VideoReadback(OgreView *view, Ogre::Root *root, const std::atomic<unsigned> *held)
        : mView(view), mRoot(root), mHeld(held) {}
    /// Waits out every pending copy, then frees the tickets and the target.
    ~VideoReadback();
    /// Allocates for the view's current target; false with `error` when the
    /// target cannot be converted (size, format).
    bool init(std::string &error);
    void arm(unsigned long long tag) { mArmed = true; mArmedTag = tag; }
    bool take(VideoFrameNv12 &out, bool wait, std::string &error);
    VideoReadbackStatus status() const;
    void workspacePosUpdate(Ogre::CompositorWorkspace *ws) override;
private:
    struct Slot {
        Ogre::AsyncTextureTicket *ticket = nullptr;
        bool pending = false;
        unsigned long long tag = 0, order = 0;
        Ogre::uint32 frame = 0;   // the VaoManager frame the copy was recorded in
    };
    void record(Slot &slot);
    void release();
    OgreView *mView;
    Ogre::Root *mRoot;
    Ogre::TextureGpu *mTarget = nullptr;   // R32_UINT, width/4 x height*3/2
    unsigned mWidth = 0, mHeight = 0;
    Slot mSlots[kRing];
    bool mArmed = false;
    unsigned long long mArmedTag = 0, mOrder = 0;
    unsigned long long mRecorded = 0, mDelivered = 0, mDropped = 0;
    std::string mError;
    const std::atomic<unsigned> *mHeld = nullptr;
    unsigned held() const { return mHeld ? mHeld->load(std::memory_order_acquire) : 0u; }
};

class OgreView final : public View, public inspect::ViewEcho {
public:
    /// On-screen: `window` is set. Offscreen: `texture` is set. Never both.
    OgreView(Ogre::Root *root, Ogre::Window *window, Ogre::TextureGpu *texture,
             const std::string &name, unsigned w, unsigned h, const Colour &background,
             std::string &errorSink);
    /// THE HOST'S HANDLE OF AN OFFSCREEN VIEW (U3 stage 4a, decision 39 b): its identity now (name, id, size, clear
    /// colour); its target and chain at the create message's drain (adoptTarget).
    struct HostShell {};
    OgreView(HostShell, Ogre::Root *root, const std::string &name, unsigned w, unsigned h, const Colour &background,
             std::string &errorSink);
    void adoptTarget(Ogre::TextureGpu *texture);
    void buildChain();   ///< the compositor chain of the view's own definition (both constructors' Ogre half)
    ~OgreView() override;

    /// The same pointer, unerased — Engine::renderOneFrame groups views by the
    /// scene they draw (the authoritative-view rule on updateGi).
    OgreScene *ogreScene() const { return mScene; }

    bool setScene(Scene *scene) override;
    bool setCameraNode(NodeId node) override;
    NodeId cameraNode() const override { return mCameraNode; }
    /// THE FRAME INPUT THIS VIEW LAST APPLIED (U0 3a): the drain's compare, and the target size the camera
    /// was last applied at (the framing hold and the ortho window read the size: a resize re-applies it).
    FrameInputView mFrameApplied;
    unsigned mFrameAppliedW = 0, mFrameAppliedH = 0;

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
    void applyEnabled(bool on);       ///< the engine's own: the drain, the VR session's views
    /// The VR session's eye mirror covers this view (VrSession::setDesktopShowsEye; U3 read F2).
    void setSessionCovers(bool covers);
    void applyWorkspacesEnabled();
    bool mSessionCovers = false;
    /// WHAT DRAWS: the host's wish (mEnabled) AND no session cover — every "does this view draw" site reads this.
    bool drawing() const { return mEnabled && !mSessionCovers; }
    Colour background() const override;
    bool shadows() const override;
    void setShadows(bool on) override;
    void applyShadows(bool on);
    void setBackground(const Colour &c) override;
    void applyBackground(const Colour &c);
    /// The clear colour and the shadow node live in the chain's definitions:
    /// rebuild definitions + workspace, keeping scene, camera and enabled state.
    void rebuildWorkspaceDef();
    /// ...its body, for a caller that has already detached the workspace.
    void rebuildDetachedWorkspaceDef();
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
    /// chainDesc's last word: ChainDesc::atomOcclusion and the pyramid it needs.
    void finishAtomOcclusion(ChainDesc &d) const;
    /// The scene-decided shape (atomDraw, atomOcclusion, ssao) differs from the one
    /// the definition was built with (OgreView.cpp).
    bool sceneShapeMoved() const;

    // ---- VR (SPECS/VR_SPEC.md §4.3) ---------------------------------------
    /// Makes this view's chain a STEREO one: the target becomes two layers, one
    /// eye each, and every pass draws both through multiview (ChainDesc::stereo).
    /// `cullCamera` is the name of a camera sitting between the eyes. Recreates
    /// the target and rebuilds the workspace definition.
    /// Called only by the VR session, on the View it owns.
    void setStereo(bool on, const std::string &cullCamera);
    bool stereo() const { return mStereo; }
    /// The no-ray route's screen-GI listener (PHOTON-7 stage 7, 7-6; null where the chain gathers or draws nothing).
    const ScreenGiListener *screenGiListener() const { return mScreenGiListener.get(); }
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
    unsigned long long globalsPushes() const { return mGlobalsPushes; }
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
    bool drawsBlank() const { return drawing() && mBlankWorkspace != nullptr; }
    /// Compositor listeners this view re-attaches to every workspace it builds.
    /// The view does NOT own them: register at setup, unregister before the
    /// listener dies. Registering twice is a no-op.
    void addWorkspaceListener(Ogre::CompositorWorkspaceListener *l);
    void removeWorkspaceListener(Ogre::CompositorWorkspaceListener *l);
    /// Counts completed workspace attachments. Neutral introspection (no Ogre
    /// type crosses the boundary) that lets hosts and tests see that a call was
    /// or was not structurally expensive — every rebuild goes through the seam,
    /// so this is exactly the number of times it ran.
    unsigned workspaceGeneration() const;

    unsigned long long framesPresented() const;
    /// Frames presented since THIS WORKSPACE was built (reset by every chain
    /// rebuild) — what says whether a compute pass of the chain has ever run,
    /// which is how `HzbStatus::primed` is answered.
    unsigned long long workspaceFramesPresented() const { return mWorkspaceFramesPresented; }
    unsigned long long blankFramesPresented() const;
    PublishedEngineFrame::ViewFrames published() const override;
    bool warmUpShaders() override;
    void setAsyncShaders(bool on) override;
    bool asyncShaders() const override;
    /// Called by OgreEngine::renderOneFrame AFTER Root::renderOneFrame: counts
    /// this frame if the view was actually part of it (enabled + workspace +
    /// scene). The one place mFramesPresented moves up.
    void notePresented();

    // THE PRESENTED FRAME'S CAPTURE (CLOSE-SHOT-2; View::requestFrameCapture).
    bool requestFrameCapture(bool keepHelpers, bool alsoPresented) override;
    FrameCaptureState frameCaptureState() override;
    bool takeFrameCapture(Image &out, bool wait, Image *presented) override;
    void cancelFrameCapture() override {   // a REQUEST on the host's side (U3 stage 4a, decision 39 d)
        if (hostPosts()) { requestOnEngine([this] { releaseFrameCapture(); }); return; }
        releaseFrameCapture();
    }
    // THE VIDEO READBACK (VIDEO-REC-1; OgreVideoReadback.cpp).
    bool setVideoReadback(bool on) override;
    void armVideoFrame(unsigned long long tag) override;
    bool takeVideoFrame(VideoFrameNv12 &out, bool wait) override;
    VideoReadbackStatus videoReadbackStatus() const override;
    /// OgreEngine::renderOneFrame, before any workspace of the frame updates: does
    /// THIS frame capture (armed, enabled, a scene workspace to draw it)? A clean
    /// capture builds its own workspace here (see mCaptureWorkspace). The flag is
    /// one frame's: notePresented drops it.
    void beginFrameCapture();
    bool captureThisFrame() const { return mCaptureThisFrame; }
    bool captureKeepsHelpers() const { return mCaptureKeepHelpers; }
    /// The workspace a clean capture draws through this frame, or null. Its scene
    /// passes drop both helper channels; the Atom and Photon listeners leave their
    /// viewing aids off in it.
    Ogre::CompositorWorkspace *captureWorkspace() const { return mCaptureWorkspace; }
    bool isCaptureWorkspace(const Ogre::CompositorWorkspace *ws) const {
        return ws && ws == mCaptureWorkspace;
    }
    /// The capture's first prepass-use pass copies the view's finished reflection
    /// (RayQueryTier::bindCapturePass) — once a capture frame.
    bool captureReflectionPending() const { return mCaptureReflectionPending; }
    void setCaptureReflectionPending(bool on) { mCaptureReflectionPending = on; }
    /// A local texture of the view's own chain (its scene node), or null.
    Ogre::TextureGpu *chainTexture(const char *name) const;
    /// A listener of this view that must NOT ride the capture's workspace (the VR
    /// session's eye submission).
    void excludeFromCapture(Ogre::CompositorWorkspaceListener *l);
    bool capturesWith(const Ogre::CompositorWorkspaceListener *l) const;
    bool captureShadowedPass(const Ogre::CompositorPassSceneDef *p) const {
        return p && std::find(mCaptureShadowedPasses.begin(), mCaptureShadowedPasses.end(), p) !=
                        mCaptureShadowedPasses.end();
    }
    /// Builds (or keeps, within one capture) the capture workspace for the current shape.
    bool ensureCaptureWorkspace();
    /// The capture listener's workspacePosUpdate: records the copy (Armed -> InFlight).
    void recordFrameCapture(Ogre::CompositorWorkspace *ws, bool incomplete = false);
    /// Drops whatever capture is pending (ticket, workspace and target destroyed).
    void releaseFrameCapture();
    /// The clean capture's workspace and target, gone (between frames; Ogre defers
    /// the images' destruction past the frames still using them).
    void destroyCaptureWorkspace();

    void setPostFx(const PostFxDesc &fx) override;
    void applyPostFx(const PostFxDesc &fx);
    /// The submitting side's merge of a view part (RenderThread::submit): what the host pushed, kept (View::pushed*).
    void notePushed(const FrameInputView &part) {
        if (part.postFx) mPushedPostFx = *part.postFx;
        if (part.background) mPushedBackground = *part.background;
        if (part.shadows) mPushedShadows = *part.shadows;
        if (part.enabled) mPushedEnabled = *part.enabled;
        if (part.overlay) mPushedOverlay = *part.overlay;
        if (part.pip) mPushedPip = *part.pip;
        if (part.scene) mBoundScene = *part.scene;
    }
    /// THE HOST'S SIDE OF A SETTER (U3 stage 4a): the call is a field of the view's record — `fill` writes it, the
    /// record is submitted, and the drain applies it on the engine's side. False on the engine's own side (the drain,
    /// the VR session, a request): there the setter applies.
    template <class F> bool postedAsField(F &&fill) {
        if (!hostPosts()) {
            // WITHOUT RECORDS THE CALLER IS THE HOST (a direct engine, the suites' shape): its record (View::pushed*)
            // is written at its call, as the submit's merge writes it when the calls are records — never from the
            // drain (decision 45, D5): there the engine's own side calls, and callIsTheHosts() is false.
            if (callIsTheHosts()) { FrameInputView part; fill(part); notePushed(part); }
            return false;
        }
        FrameInput in;
        FrameInputView part;
        part.view = this;
        fill(part);
        in.views.push_back(std::move(part));
        postRecord(std::move(in));
        return true;
    }
    void postRecord(FrameInput &&in);
    bool hostPosts() const;
    /// An engine whose calls are not records: the call in hand is the host's own (its record written at the call).
    bool callIsTheHosts() const;
    /// The host's record of the binding, cleared at the HOST's destroyScene of the scene it names (decision 66).
    void forgetHostBinding() { mBoundScene = nullptr; }
    void requestOnEngine(const std::function<void()> &f);   ///< Engine::request on this view's engine (U3 4a)
    /// The scene the engine has bound (the drain's comparison; View::scene is the host's binding).
    OgreScene *boundOgreScene() const { return mScene; }
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
    void setOffscreenContract(OffscreenContract c) override;
    float measuredExposureScale() const override;

    void setOverlay(const ViewOverlayDesc &d) override;
    const ViewOverlayDesc &overlay() const override;

    // ---- The picture-in-picture inset (CAMERAS_SPEC §7.7) ------------------
    void setPip(const ViewPipDesc &d) override;
    const ViewPipDesc &pip() const override;
    unsigned pipGeneration() const;
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
    /// The view's OTHER workspaces the frame monitor watches (lane TEST-1, F2):
    /// the picture-in-picture inset and the blank one. Neither takes the view's
    /// own listeners (they are the main chain's), so the monitor attaches to them
    /// directly, each frame, as it does to a scene's private workspaces.
    void monitorWorkspaces(std::vector<Ogre::CompositorWorkspace *> &out) const {
        if (mPipWorkspace) out.push_back(mPipWorkspace);
        if (mBlankWorkspace) out.push_back(mBlankWorkspace);
    }
    /// The texture this view draws into: the window's swapchain texture on an
    /// on-screen view, the RTT on an offscreen one. The VR session needs it for
    /// both of its jobs — the eye copy reads the session View's target, and the
    /// mirror writes the mirror View's.
    Ogre::TextureGpu *targetTexture() const;
    bool isEnabled() const override;
    unsigned width()  const;
    unsigned height() const;
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
    unsigned sampleCount() const;

    /// On-screen resize is applied at frame time (applyPendingResize): by then Qt has
    /// resized the native window, and doing it once per frame coalesces layout bursts.
    void resize(unsigned w, unsigned h) override;

    bool readPixels(Image &out) override;
    bool readPixelsHdr(ImageF &out) override;
    bool readReflectionHdr(ImageF &out) override;
    bool readSsrHistoryHdr(ImageF &out) override;
    /// Downloads one of this view's chain textures (a local of its scene node)
    /// into float: the radiance readback and the reflection readback share it.
    bool readChainTexture(const char *textureName, ImageF &out, const char *who);

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
    /// The authoritative view's camera to its scene's GI at the frame's head (U4 stage 3: updateGi runs after the
    /// scene graph; the driver's profile and the first camera owe the pending flush before it runs).
    void noteGiCamera();
    /// The pose noteGiCamera last saw (PHOTON-2 stage 2's round, H2: the camera STILL since the last frame - the
    /// field's rest budget runs only then).
    Ogre::Vector3    mGiNotePos = Ogre::Vector3::ZERO;
    Ogre::Quaternion mGiNoteRot = Ogre::Quaternion::IDENTITY;
    bool             mGiNoteKnown = false;
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
    /// ATOM-3 stage 1: THE VIEW'S PIXEL-ERROR THRESHOLD ON ITS CAMERAS. The stock chain's
    /// world-error strategy multiplies its allowed error by the camera's own LOD factor
    /// (`_getLodBiasInverse`, OgreMesh.cpp), so the view camera (and the PIP's) carries
    /// 1 / Scene::pixelTolerance() and a shadow map's camera keeps Ogre's 1 — the maps keep
    /// the texel. Every frame, before the view's passes walk the levels; two stores.
    void syncLodTolerance();
    /// Arms or disarms this view's POST-CHAIN GLOBALS listener (CAMERA_LENS_SPEC
    /// §4). Armed while the view is enabled and its chain has any effect, so
    /// the view pushes ITS OWN exposure / bloom threshold / AO projection to
    /// its own workspace's passes; disarmed otherwise, which is what makes a
    /// view with no chain cost exactly nothing. Same idempotent once-a-frame
    /// contract as syncPlanarListener, and registered through the same seam.
    void syncGlobalsListener();
    /// ...and the no-ray screen integrate's (ScreenGiListener), the same arming rule on ChainDesc::screenIntegrate.
    void syncScreenGiListener();
    /// Arms or disarms this view's RAY-TRACED REFLECTION listener (PHOTON_SPEC
    /// §7 R5). Armed while the view is enabled, has a scene and a camera, and
    /// its chain carries `rayReflect` — which is the SSR row being on AND the
    /// machine advertising ray queries AND the application preference allowing
    /// them. Same idempotent once-a-frame contract and the same seam as the two
    /// above, and for the same reason: every one of those can change between
    /// frames. Defined in OgreRayQuery.cpp (both halves of it).
    void syncReflectListener();
    /// THE HIT LIST'S GROWTH (kHitListWidth's law), once a frame from syncReflectListener, before the workspace runs:
    /// the list's three textures re-made in place, larger, when the target outgrew its creation size or a retired frame
    /// overflowed the list - never smaller.
    void sizeHitList();
    /// THE LIST'S CREATION SIZE in rows: kHitListCreateShare x the target's W x H (the eye pair's for a stereo view).
    unsigned hitListCreateRows() const;
    /// THE VISIBILITY BUFFER'S VIEW HALF (ATOM S3-DRAW, OgreAtomDraw.cpp), once a
    /// frame beside syncReflectListener and for its reason: ChainDesc::atomDraw
    /// reads the SCENE (a view has none when its chain is first built), so the
    /// shape is re-checked, and the listener that arms the screen decode follows.
    void syncAtomDraw();
    /// THE PHOTON VIEW'S VIEW HALF (PHOTON-VIEW-1, OgrePhotonView.cpp), once a
    /// frame beside syncAtomDraw: the listener that sets the photon passes'
    /// execution bits and scopes the isolation property to the opaque pass, the
    /// ray tier's overlay texture while the scene's view needs one, and this
    /// view's shape reported to the scene (what the verb may refuse on).
    void syncPhotonView();
    /// THE RAY TIER'S PER-VIEW OVERLAY (ScreenProbes, Hits): RGBA16F at the
    /// target's size, a UAV the tier writes and the photon quad samples. Null
    /// unless the scene's view is one of the two.
    Ogre::TextureGpu *photonOverlay() const { return mPhotonOverlay; }
    /// The tier wrote the overlay (its contents are defined from then on).
    void notePhotonOverlayWritten() { mPhotonOverlayWritten = true; }
    bool photonOverlayWritten() const { return mPhotonOverlayWritten; }
    /// Which overlay (1, 2, ... — one per creation; 0 = none): a re-created
    /// overlay may recycle the pointer.
    unsigned photonOverlayGeneration() const { return mPhotonOverlayGeneration; }
    /// The view's own GPU cull (the id pass's list): one per view, so two views of
    /// one scene in a frame never share the list the other is drawing from.
    detail::GpuCull &atomCull() { return mAtomCull; }
    /// THE LATE LIST (ATOM-OCCLUSION-1): the disocclusion pass's cull, its own buffers.
    detail::GpuCull &atomCullLate() { return mAtomCullLate; }
    /// The capture workspace's own list (CLOSE-SHOT-2): its cull must not write the
    /// view's lists, whose count words the view's stats ring and cut budget read back.
    detail::GpuCull &atomCullCapture() { return mAtomCullCapture; }
    /// The id pass's stats ring of the view's two lists (OgreAtomIdPass.cpp; null before
    /// the first frame that copies their counters).
    AtomIdViewRingPtr &atomStatsRing() { return mAtomStatsRing; }
    /// THE DENSITY COUNT AND THE CUT PROBE of this view (ATOM-2; OgreAtomDensity.cpp), null before first use.
    AtomDensityStatePtr &atomDensity() { return mAtomDensity; }
    /// Whether this view's chain carries the two-pass occlusion (ChainDesc::atomOcclusion).
    bool chainAtomOcclusion() const { return mChainAtomOcclusion; }
    /// WHAT THE FIRST CULL TESTS AGAINST (OgreAtomIdPass.cpp): the matrix the pyramid
    /// in `jahHzb` was BUILT with (the late pass's draw of the previous frame), the
    /// viewport rectangle it covered, the pyramid's size, the workspace generation and
    /// the scene — a pyramid of another graph, size or world is not tested against.
    AtomOcclusionHistory &atomOcclusionHistory() { return mAtomOcclHistory; }
    AtomReceiverDepth &atomReceiverDepth() { return mAtomReceiverDepth; }
    /// Whether THIS frame's first cull tested against a pyramid (set by the first pass,
    /// read by the late one: nothing was rejected otherwise).
    void setAtomFirstTested(bool tested) { mAtomFirstTested = tested; }
    bool atomFirstTested() const { return mAtomFirstTested; }
    /// The late pass's share (the stats ring's second half): its triangles and
    /// survivors, the instances the frame's depth test REJECTED (not drawn: occluded)
    /// and the late pass's survivors (DISOCCLUDED — drawn only because the second test
    /// found them visible).
    void setAtomLateStats(unsigned long long triangles, unsigned survivors, unsigned occluded,
                          unsigned disoccluded) {
        mAtomLateTriangles = triangles;
        mAtomLateSurvivors = survivors;
        mAtomOccluded = occluded;
        mAtomDisoccluded = disoccluded;
    }
    bool atomLateStats(unsigned long long &triangles, unsigned &survivors) const {
        triangles = mAtomLateTriangles;
        survivors = mAtomLateSurvivors;
        return mChainAtomDraw && mAtomStatsValid;
    }
    bool atomOcclusionStats(unsigned &occluded, unsigned &disoccluded) const {
        occluded = mAtomOccluded;
        disoccluded = mAtomDisoccluded;
        return mChainAtomOcclusion && mAtomStatsValid;
    }
    /// THE ID PASS'S SHARE OF THE FRAME'S STATS (renderStats): its indirect draws never
    /// reach Ogre's RenderingMetrics, so the cull's own counters (survivors, and the
    /// triangles the draws job adds up) are copied into a mapped ring and read back
    /// once the frame that wrote them has retired (OgreAtomIdPass.cpp) — a few frames
    /// late, never waited on, and never a mid-frame submit.
    void setAtomStats(unsigned long long triangles, unsigned survivors) {
        mAtomTriangles = triangles;
        mAtomSurvivors = survivors;
        mAtomStatsValid = true;
    }
    bool atomStats(unsigned long long &triangles, unsigned &survivors) const {
        triangles = mAtomTriangles + mAtomLateTriangles;
        survivors = mAtomSurvivors + mAtomLateSurvivors;
        return mChainAtomDraw && mAtomStatsValid;
    }
    /// ...the FIRST pass's share alone (its pass's metrics row).
    bool atomFirstStats(unsigned long long &triangles, unsigned &survivors) const {
        triangles = mAtomTriangles;
        survivors = mAtomSurvivors;
        return mChainAtomDraw && mAtomStatsValid;
    }
    /// ...and THE CUT'S counters from the same ring (ATOM-CLUSTER-CUT).
    void setAtomCutStats(const AtomCutStats &s) { mAtomCutStats = s; }
    bool atomCutStats(AtomCutStats &out) const {
        out = mAtomCutStats;
        return mChainAtomDraw && mAtomStatsValid;
    }
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
    /// `layers` > 1 = a LAYERED target (a stereo view, LAYERED-STEREO-1): a
    /// Type2DArray of w / layers x h, one layer per eye, so every eye renders at
    /// the same origin of its own layer (multiview). `w` stays the PAIR's width.
    static Ogre::TextureGpu *createRtt(Ogre::Root *root, const std::string &name,
                                       unsigned w, unsigned h, unsigned samples = 1,
                                       unsigned layers = 1,
                                       Ogre::PixelFormatGpu format = Ogre::PFG_RGBA8_UNORM);
    /// Rounds down to a power of two and clamps to [1, 16] — what the backend
    /// will even ask the driver for (the driver may still clamp further).
    static unsigned sanitizeSamples(unsigned samples);

private:
    /// Offscreen only: replaces the RTT (which cannot change in place) at the
    /// given size and mRequestedSamples, re-adding the workspace. The shared
    /// tail of resize() and setSampleCount().
    void rebuildRtt(unsigned w, unsigned h);
    Ogre::TextureGpu *target() const;
    /// The target's colour sample count (1 without a target).
    unsigned targetSamples() const;

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
    /// View::setAsyncShaders (ASYNC-SHADERS-1): re-applied to every workspace this view builds.
    bool mAsyncShaders = false;
    /// Captures retried because their frame met a shader still building (ASYNC-SHADERS-1).
    unsigned long long mCaptureRetries = 0;
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
    std::string                mWorkspaceDef;   // the name is Scene/View's mIdentityName (one name per object)
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
    /// THE NO-RAY ROUTE'S SCREEN INTEGRATE (ChainDesc::screenIntegrate, PHOTON-7 7-6): armed while the chain carries it.
    std::unique_ptr<ScreenGiListener> mScreenGiListener;
    /// Owned; registered the same way while this view's chain traces
    /// reflections (PHOTON_SPEC §7 R5). Null everywhere else — on a machine
    /// without ray queries, with the preference off, or on any view whose chain
    /// has no SSR (every thumbnail, preview and pixel suite, by construction).
    /// Defined in OgreRayQuery.cpp, which is why it is held through a pointer
    /// the rest of the engine never dereferences.
    std::unique_ptr<ReflectPassListener> mReflectListener;
    /// ATOM S3-DRAW: the shape the definition was built with, the listener that
    /// arms the screen decode, and the view's cull buffers.
    bool mChainAtomDraw = false;
    /// ATOM-OCCLUSION-1: the shape's occlusion (ChainDesc::atomOcclusion), the LATE
    /// list (the disocclusion pass's own cull: one list per pass, so the late draw
    /// never reads the buffers the first one is still drawing from) and the history
    /// the first cull tests against.
    bool mChainAtomOcclusion = false;
    /// SSAO-DOUBLE-1: whether the definition was built WITH the SSAO passes. The
    /// scene's GI mode decides it (OgreScene::ssaoRefused) and a view learns its
    /// scene late, so it is compared once a frame beside the id pass's shape.
    bool mChainSsao = false;
    detail::GpuCull mAtomCullLate;
    detail::GpuCull mAtomCullCapture;
    AtomOcclusionHistory mAtomOcclHistory;
    AtomReceiverDepth mAtomReceiverDepth;
    bool mAtomFirstTested = false;
    unsigned long long mAtomLateTriangles = 0ull;
    unsigned mAtomLateSurvivors = 0u, mAtomOccluded = 0u, mAtomDisoccluded = 0u;
    AtomDrawListenerPtr mAtomListener;
    /// The id pass's stats ring of THIS view's lists (OgreAtomIdPass.cpp), created on the
    /// first frame that reads them back; retired with the view.
    AtomIdViewRingPtr mAtomStatsRing;
    AtomDensityStatePtr mAtomDensity;
    /// PHOTON-VIEW-1: the view's photon listener and the tier's overlay.
    PhotonListenerPtr mPhotonListener;
    /// CLOSE-SHOT-2: the presented frame's capture (requestFrameCapture).
    std::unique_ptr<FrameCaptureListener> mCaptureListener;
    /// VIDEO-REC-1: the recorder's per-frame NV12 readback, while on.
    std::unique_ptr<VideoReadback> mVideoReadback;
    /// ...its finished frames, mapped by the render thread at its publish and popped by the host (U3 stage 6).
    std::mutex                 mVideoQueueLock;
    std::deque<VideoFrameNv12> mVideoQueue;
    std::atomic<unsigned>      mVideoQueued{ 0u };   ///< mVideoQueue.size(), for the readback's ring arithmetic
    bool popVideoFrame(VideoFrameNv12 &out);
public:
    /// The render side's harvest of the finished readbacks into the queue (RenderThread::publish).
    void harvestVideoFrames();
private:
    Ogre::AsyncTextureTicket *mCaptureTicket = nullptr;
    /// A clean capture's companion: the same frame as presented (alsoPresented).
    Ogre::AsyncTextureTicket *mPresentedTicket = nullptr;
    bool mCaptureAlsoPresented = false;
    FrameCaptureState mCaptureState = FrameCaptureState::Idle;
    bool mCaptureKeepHelpers = false;
    bool mCaptureThisFrame = false;
    /// A CLEAN capture never touches the presented frame: the view's chain is
    /// instanced a second time, for that frame only, into a target of the window's
    /// size and format, updated AFTER the view's own workspace (so the presented
    /// frame is computed exactly as it would have been) with the helper channels
    /// dropped — the frame's settled GI and caches, its camera, its listeners; the
    /// two workspace-local histories (exposure, SSR colour) copied in from the
    /// view's own before either workspace runs.
    Ogre::CompositorWorkspace *mCaptureWorkspace = nullptr;
    Ogre::TextureGpu *mCaptureTarget = nullptr;
    std::string mCaptureDef;
    std::vector<std::string> mCaptureNodeDefs;
    chain::ChainHandles mCaptureHandles;
    /// What the capture workspace was built for; any change rebuilds it.
    struct CaptureShape {
        unsigned generation = ~0u, w = 0, h = 0, layers = 0, samples = 0;
        Ogre::PixelFormatGpu format = Ogre::PFG_UNKNOWN;
        bool operator==(const CaptureShape &o) const {
            return generation == o.generation && w == o.w && h == o.h && layers == o.layers &&
                   samples == o.samples && format == o.format;
        }
    } mCaptureShape;
    bool mCaptureReflectionPending = false;
    /// The capture definition's scene passes that name the shadow node in the view's.
    std::vector<const Ogre::CompositorPassSceneDef *> mCaptureShadowedPasses;
    std::vector<Ogre::CompositorWorkspaceListener *> mCaptureExcluded;
    Ogre::TextureGpu *mPhotonOverlay = nullptr;
    bool mPhotonOverlayWritten = false;
    unsigned mPhotonOverlayGeneration = 0u;
    /// The overlay's destroy, through the ray tier's retire window when there is
    /// one (OgreRayQuery.cpp — its descriptor sets may still be in flight).
    void retirePhotonOverlay();
    detail::GpuCull mAtomCull;
    unsigned long long mAtomTriangles = 0ull;
    unsigned mAtomSurvivors = 0u;
    AtomCutStats mAtomCutStats;
    bool mAtomStatsValid = false;
    unsigned                   mWorkspaceGeneration = 0;
    /// What `ChainDesc::rayReflect` was when the CURRENT workspace definition
    /// was built. The scene arrives AFTER the chain is first built (the
    /// constructor has no scene), and the project's ray row can change under a
    /// live view, so the shape is re-checked once a frame in
    /// syncReflectListener rather than only when a host pushes a PostFxDesc.
    bool                       mChainRayReflect = false;
    /// ...and whether it carried the PREPASS (`ChainDesc::prepass`): the gather's
    /// and the sun contact's rows are the scene's, so the shape is re-checked once
    /// a frame beside the reflection's (OgreView::syncReflectListener) — as the
    /// prepass they ask for, never as the rows themselves (PHOTON-GATHER-1d).
    bool                       mChainPrepass = false;
    /// ...and ChainDesc::hitDecode (PHOTON-HIT-SHADE-1): the hit list + its pass.
    bool                       mChainHitDecode = false;
    /// ...and who produces the opaque passes' GI (ChainDesc::probeGather / screenIntegrate, PHOTON-7 7-5 / 7-6):
    /// re-checked once a frame with the scene's other shape terms (sceneShapeMoved).
    bool                       mChainProbeGather = false;
    bool                       mChainScreenIntegrate = false;
    /// THE HIT LIST'S SIZE (sizeHitList, kHitListWidth): the rows the list holds (0 until the first sizeHitList reads the
    /// built list; a graph is built at max(this, hitListCreateRows())). Never decreases.
    unsigned                   mHitListRows = 0u;
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
    /// per frame is the PhotonVoxelMaterial aliasing class of bug), and the definitions
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
    bool                       mPipTexEncode = false;   ///< the host's displayEncode the inset was built with
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
    /// View::setOffscreenContract; `mSaidNoContract` = the refusal was logged.
    OffscreenContract          mOffscreenContract = OffscreenContract::Undeclared;
    mutable bool               mSaidNoContract = false;
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
/// TWO OBJECTS, ONE LIFETIME (VR-START-1):
///   * `VrConnection` is the XrInstance and the system, bound through
///     XR_KHR_vulkan_enable to the engine's OWN Vulkan instance and device
///     (created at boot with the fork's OpenXR interop set). It is made when a
///     session begins (or a probe asks) and destroyed when the session ends,
///     so every session connects afresh — a headset connected after launch, or
///     reconnected after a drop (WiVRn's fresh streaming process), is simply
///     the next connection. It owns no Vulkan object.
///   * `VrSession` is the XrSession, the swapchains, the both-eyes target, the
///     stereo View and the pump. It lives for as long as the user is in the
///     headset.
class VrConnection;
class VrSession;

/// The calls OgreEngine.cpp makes into that TU. Free functions rather
/// than methods so the engine never has to see either class's definition.
namespace vr {
/// The policy check (no loader), the XrInstance, the system, the graphics
/// requirements, the runtime's GPU against the engine's and its interop
/// extensions against what the engine enabled. Null + `info.failure` +
/// `reason` when anything refuses. `root` must have a Vulkan device (any
/// window or view created).
VrConnection *connect(Ogre::Root *root, const VrPolicy &policy, VrInfo &info,
                      std::string &reason);
/// Destroys the XrInstance. Call after the connection's session has ended.
void disconnect(VrConnection *);
/// Creates the session. Null + `reason` on refusal.
VrSession *sessionBegin(VrConnection *, OgreEngine *, OgreScene *, const VrConfig &,
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
/// Did this frame wait in xrWaitFrame (the runtime paced it)? False without a session (U3 decision 53).
bool    vrSessionWaitedThisFrame(const VrSession *);
/// The runtime's wait, before the free loop's hand-over (U3 decision 65); the frame's beginFrame takes its result.
void    vrSessionWaitFrameEarly(VrSession *);
/// The session's desktop MIRROR workspace (null without one) — the frame
/// monitor's listener rides it (lane TEST-1, the perf audit's F2).
Ogre::CompositorWorkspace *vrSessionMirrorWorkspace(VrSession *);
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

class OgreEngine final : public Engine, public inspect::EngineEcho {
public:
    bool init(const EngineConfig &cfg, std::string &error);

    Scene *createScene(const std::string &name, unsigned workerThreads = 0) override;
    /// The blank scene manager (@see mBlankScene), created on the first call.
    /// Null only when the Hlms is not registered yet, which cannot happen on
    /// the path that asks: a View exists by then.
    Ogre::SceneManager *blankSceneManager();
    /// The views currently bound to `scene` (ATOM-CLUSTER-CUT: the cut's counters
    /// are a view's, and AtomDrawStatus is a scene's).
    std::vector<OgreView *> viewsOf(const OgreScene *scene) const {
        std::vector<OgreView *> out;
        for (const auto &v : mViews)
            if (v && v->ogreScene() == scene && v->isEnabled()) out.push_back(v.get());
        return out;
    }

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
    /// THE VR SESSION'S EYE PAIR (LAYERED-STEREO-1): an offscreen view whose target
    /// is BORN a two-layer array (one eye per layer, `eyeW` x `eyeH`), so the
    /// session's setStereo has nothing to re-create. Its width() is the pair's.
    OgreView *createStereoPairView(const std::string &name, unsigned eyeW, unsigned eyeH,
                                   const Colour &background);
    OgreView *createOffscreen(const std::string &name, unsigned width, unsigned height,
                              const Colour &background, unsigned layers);

    void destroyView(View *view) override;

    /// ONE TURN OF THE RENDER LOOP (ENGINE-UPGRADE-U3 stage 1): RenderThread::tickInline — the hand-over, the frame
    /// (runFrame, below), the publish. The public door stays here until stage 4 deletes it.
    void renderOneFrame() override;
    void setFreeRunning(bool on) override { if (mRenderThread.threaded()) mRenderThread.setFreeRunning(on); }
    void latchSnapshot() override { mRenderThread.latchSnapshots(); }
    void setFramePublishedHook(std::function<void()> hook) override { mRenderThread.setPublishedHook(std::move(hook)); }
    long long frameInFlightSinceNs() const override { return mRenderThread.frameInFlightSinceNs(); }
    long long lastFrameWorkNs() const override { return mRenderThread.lastFrameWorkNs(); }
    void presentationFrame() override { mRenderThread.freeFrameNow(); }
    void lastInterpolation(const Scene *s, unsigned &nodes, double &micros, double &namingMicros) const override {
        nodes = 0; micros = 0.0; namingMicros = 0.0;
        for (const auto &q : mScenes)
            if (q.get() == s) { nodes = q->mInterpNodesLast; micros = q->mInterpMicros; namingMicros = q->mInterpNameMicros; }
    }
    /// The game side's merge into the hand-over slot (RenderThread::submit).
    void submitFrame(FrameInput input) override;
    void post(Scene *scene, SceneMessage &&message) override;
    FrameSubmitStatus frameSubmitStatus() const override { return mRenderThread.submitStatus(); }
    FrameInputStatus frameInputStatus() const { return mFrameInputStatus; }
    const PublishedEngineFrame &published() const override { return mRenderThread.published(); }
    void setDisplayPeriod(float seconds) override { mRenderThread.setDisplayPeriod(seconds); }
    void setFramePace(FramePace pace) override {
        if (postedAsMessage([=](OgreEngine &e) { e.setFramePace(pace); return true; })) return;   // U3 4a: a message
        mFramePace = pace;
    }
    void request(std::function<void(Engine &)> question, std::function<void()> done) override;
    std::vector<SyncGetterCount> syncGetters() const override {
        std::vector<SyncGetterCount> out;
        for (const SyncGetterSite *s = syncGetterSites(); s; s = s->next)
            out.push_back({ s->name, s->calls.load(std::memory_order_relaxed) });
        return out;
    }
    bool pendingGiConfiguration(const Scene *scene) const override;
    /// THE ONE DRAIN (U0 3a): the record the hand-over took (`any` false = an EMPTY hand-over), applied at the top of
    /// the frame. `budgeted` (a FREE frame's own drain, BUGS-7): the structure is applied in its one order until the
    /// frame's drain budget is spent (freeDrainBudgetMs); the rest is carried to the next frame
    /// (RenderThread::carryForward). Lockstep frames, joins and requests drain whole.
    void drainFrameInput(FrameInput &in, bool any, bool budgeted = false);
    /// THE FREE FRAME'S DRAIN BUDGET (BUGS-7; measured by BISTRO-STREAM-IN-1 on Bistro's first open, §2604: one frame
    /// drained the messages of every tick queued while the previous frame ran — 823 -> 25,515 messages, frames of
    /// 0.8-5 s of render-thread CPU with the GPU idle, growing with the queue). The display period is the budget's
    /// measure: a FREE frame's drain may use the time that still presents the frame at the display rate — the period
    /// less the rest of the last free frame's work — and the remainder is carried, in order, to the next frame. When the
    /// frame's own work already fills the period (a Debug Bistro streaming in: ~70 ms of it) the display rate is out of
    /// reach whatever the drain does, and the drain takes kFreeDrainShare of that work instead: the frame stays within
    /// (1 + kFreeDrainShare) of its own length and the load keeps its share of the render thread (BUGS-7's master check:
    /// a floor of half a period stretched Bistro's stream-in from 8 s to 30 s at the same ~100 ms frames). Never more
    /// than kFreeDrainCeilingPeriods periods (a slow one-off frame does not buy the next drain its length). The unit is
    /// the MESSAGE (a message is never split; its cost spans orders of magnitude — a mesh create against a field write —
    /// so a count has no derivation and the clock is the bound). At least one message a frame. Under Unlimited pacing
    /// (no period sent) the unpaced period stands in.
    static constexpr double kFreeDrainShare = 1.0;
    static constexpr double kFreeDrainCeilingPeriods = 6.0;
    static constexpr double kFreeDrainUnpacedPeriodMs = 1000.0 / 60.0;
    /// The drain budget of the next free frame (ms), from the display period and the last free frame's work.
    double freeDrainBudgetMs() const;
    /// The last FREE frame's drain (ms): its share of RenderThread::lastFrameWorkNs is not the frame's own work.
    double mLastFreeDrainMs = 0.0;
    bool mInDrain = false;   ///< inside drainFrameInput: applySceneStream times its parts (U4 stage 3)
    /// A SCENE'S STREAM (U3 stage 3): its structural messages in emission order, then its node records — the drain's
    /// per-scene head, and what updateScene applies of the pending record before it resolves (a resolve answers for
    /// the commands already sent). Counts into the status; returns nothing.
    void applySceneStream(OgreScene *sc, const std::vector<SceneMessage> &messages,
                          const std::vector<NodeRecord> &nodes,
                          unsigned long long stepSerial);
    /// THE RENDER SIDE's half of the record's status (U3 F4): written by the drain alone.
    FrameInputStatus mFrameInputStatus;
    /// The refusals of the last kFrameErrorsKept frames (PublishedEngineFrame::refusals), oldest first.
    mutable std::deque<PublishedEngineFrame::Refusal> mRefusals;
    /// The engine messages' completions (PublishedEngineFrame::completions), pruned at the publish.
    mutable std::deque<PublishedFrame::Completion> mEngineCompletions;
    unsigned long long mEngineCompletionSeq = 0;
    mutable bool mEngineCompletionsDirty = false;
    mutable std::shared_ptr<const std::vector<PublishedFrame::Completion>> mEngineCompletionsPublished;
    /// THE HOST'S SIDE OF AN ENGINE VERB (U3 stage 4a, decision 39 c): the call becomes a message of the engine's own
    /// stream (the same verb, applied on the engine's side at the drain). False on the engine's side: the verb applies.
    template <class F> bool postedAsMessage(F &&body, unsigned long long token = 0) {
        if (!hostPosts()) return false;
        FrameInput in;
        in.engineMessages.push_back(EngineMessage{
            [b = std::forward<F>(body)](Engine &e) mutable -> bool { return b(static_cast<OgreEngine &>(e)); }, token });
        submitFrame(std::move(in));
        return true;
    }   // pruned at the publish (fillPublished)
    /// THE RENDER LOOP: the hand-over slot, the serial, the loop and the publish (RenderThread.h).
    RenderThread     mRenderThread{ *this };
    /// What the loop's publish reads of the engine at the close (RenderThread::publish): the engine's half of the
    /// snapshot, every field but the serial and the error the loop keeps itself.
    void fillPublished(PublishedEngineFrame &out) const;

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
    bool deviceLost() const;
    GpuFault gpuFault() const;
    unsigned long long queueSubmissionTag() const override;   // inspect::EngineEcho (U3 stage 3)
    /// THE FRAME'S POOL SPANS (ENGINE-UPGRADE-U4 stage 6, the core budget's measurement): each frame's span from its
    /// updateSceneGraph to the end of _updateAllRenderTargets — where the SceneManagers' workers run (the scene
    /// graph, the GPU scene's compose, the culling) — in steady_clock ns, the last kPoolSpans frames.
    void poolSpans(std::vector<std::pair<unsigned long long, unsigned long long>> &out) const override {
        std::lock_guard<std::mutex> lock(mPoolSpanMutex);
        out.clear();
        out.reserve(mPoolSpans.size());
        for (size_t i = 0; i < mPoolSpans.size(); ++i) out.push_back(mPoolSpans[(mPoolSpanHead + i) % mPoolSpans.size()]);
    }
    void notePoolSpan(std::chrono::steady_clock::time_point b, std::chrono::steady_clock::time_point e) {
        const auto ns = [](std::chrono::steady_clock::time_point t) {
            return (unsigned long long)std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
        };
        std::lock_guard<std::mutex> lock(mPoolSpanMutex);
        if (mPoolSpans.size() < kPoolSpans) { mPoolSpans.push_back({ ns(b), ns(e) }); return; }
        mPoolSpans[mPoolSpanHead] = { ns(b), ns(e) };   // a ring once full: the oldest overwritten
        mPoolSpanHead = (mPoolSpanHead + 1) % kPoolSpans;
    }
    static constexpr size_t kPoolSpans = 16384;   // ~4.5 min of frames at 60 Hz
    mutable std::mutex mPoolSpanMutex;
    std::vector<std::pair<unsigned long long, unsigned long long>> mPoolSpans;
    size_t mPoolSpanHead = 0;   ///< the oldest span once the ring is full
    /// The render system's out-of-memory count (VulkanDevice::getOutOfMemoryFailures,
    /// the fork) and the last one's text; 0 / "" without the Vulkan render system.
    unsigned vulkanOomCount() const;
    std::string vulkanLastOom() const;
    /// FORK-OOM-1: a Vulkan OOM inside this frame's work is terminal (GpuFault).
    void latchOutOfMemoryInFrame();
    void advanceResources() override;

    // ---- VR (SPECS/VR_SPEC.md §4) -----------------------------------------
    void setVrPolicy(const VrPolicy &policy) override {
        mHostVrPolicy = policy;   // the host's record
        if (postedAsMessage([=](OgreEngine &e) { e.setVrPolicy(policy); return true; })) return;   // U3 4a: a message
        JAH_HOST_CALL("Engine::setVrPolicy");
        mVrPolicy = policy;
    }
    bool vrProbe() override;
    const VrInfo &vrInfo() const override {   // the host's side: the SNAPSHOT's (U3 stage 6) — never the engine's member
        if (hostPosts()) return published().vrInfo;
        JAH_SYNC_GETTER("Engine::vrInfo");
        return mVrInfo;
    }
    bool beginVrSession(Scene *scene, const VrConfig &cfg) override;
    void endVrSession() override;
    VrState vrState() const override;
    VrState vrStateNow() const;   ///< the uncounted read vrStatus() composes from
    const VrStatus &vrStatus() const override;
    mutable VrStatus mHostVrStatusRead;   ///< the host's read when its own writes overlay the frame (host thread)
    mutable VrStatus mVrStatusScratch;    ///< the engine side's composed status (render thread)
    void setVrMirrorView(View *view) override;
    void setVrOrigin(const Vec3 &position, float yawDegrees) override;
    bool vrEyeScreenshot(unsigned eye, Image &out) override;
    bool vrInjectInput(int hand, const VrHandState &state) override;
    void vrInjectFocus(bool focused) override {
        // AN INJECTION IS A REQUEST (U3 stage 6): a test door the host reads straight back (vr.state().inputFocused,
        // the next vr.step) — applied and republished before the call returns, like vrInjectInput.
        if (hostPosts()) { request([=](Engine &e) { static_cast<OgreEngine &>(e).vrInjectFocus(focused); }, nullptr); return; }
        mVrInjectFocus = focused;
    }
    unsigned vrHandJoints(int hand, VrPose *out, unsigned count) const override;
    bool vrInjectJoints(int hand, const VrPose *joints, unsigned count) override;
    unsigned vrBindingBlocks(VrBindingBlock *out, unsigned count) const override;
    bool vrHaptic(int hand, float amplitude01, float seconds) override;
    void setVrRay(const VrRayState &ray) override {
        if (hostPosts()) mHostVrRay = ray;   // the host's record, at its call (U3 stage 6)
        if (postedAsMessage([=](OgreEngine &e) { e.setVrRay(ray); return true; })) return;   // U3 4a: a message
        mVrRay = ray;
        if (!mHostRecords) mHostVrRay = ray;
    }
    const VrRayState &vrRay() const override {   // the host's side: its own record (U3 stage 6) — it is the ray's writer
        if (hostPosts()) return mHostVrRay;
        JAH_SYNC_GETTER("Engine::vrRay");
        return mVrRay;
    }
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
    /// Makes `mVrConnection` for a session or a probe (VR-START-1); false +
    /// mVrInfo.failure/reason when nothing answers. Ensures a Vulkan device
    /// exists first (the surfaceless window).
    bool vrConnect(std::string &why);

    bool updateScene(Scene *scene) override;
    /// Re-makes the boundary's view list (Engine::listViews) from mViews; after every change of it.
    void syncHostViews();

    // Texture streaming (THREADING_ADOPTION_SPEC.md P2) — all five are one call
    // into TextureGpuManager, which belongs to the render system and is
    // therefore process-wide, not per scene.
    bool texturesDoneStreaming() const override;
    double waitForTextureLoads() override;
    int pumpTextureLoads() override;
    unsigned long long textureLoadRequests() const override;
    unsigned textureWaitTimeouts() const override { JAH_SYNC_GETTER("Engine::textureWaitTimeouts"); return mTextureWaitTimeouts; }
    unsigned long long textureWaitAdvances() const override;
    double textureWaitWorstMs() const override { JAH_SYNC_GETTER("Engine::textureWaitWorstMs"); return mTextureWaitWorstMs; }
    unsigned textureMetadataCacheEntries() const override;
    unsigned textureChannelCacheEntries() const override;
    bool saveTextureCache() override;

    /// Applies to every on-screen window that exists AND is remembered for the
    /// ones created later (createView and the MSAA-recreate hook both read it).
    void setVsync(bool on) override;

    // ---- The hardware ray-query tier (PHOTON_SPEC §7 R1) -------------------
    void setRayTracing(bool on) override;
    /// The device's own answer (the base's rayQueryAvailable is fixed from it at noteDevice).
    bool deviceRayQuery() const;
    /// Fixes the device's constants (Engine::deviceInfo / rayQueryAvailable) once the device exists; idempotent.
    void noteDevice();
    bool mDeviceKnown = false;
    /// THE FRAME'S RAY-TIER UPDATE: the acceleration structures for every drawn
    /// scene, recorded into the frame's OWN command buffer at the item walk's
    /// point (applyShadowCacheDirties, after updateSceneGraph and before any
    /// workspace renders). No private submit, no fence wait, no stall.
    void updateRayQuery(const std::vector<OgreScene *> &drawn);
    /// THE CHANGED-REGION LIST'S MOVERS (PHOTON-7 stage 3; OgreRayQuery.cpp): the moved and skin-posed slots' swept
    /// boxes into each drawn scene's list, after the GPU scene's commit and the skin pass.
    void listRayMovers(const std::vector<OgreScene *> &drawn);
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
    VrConnection *mVrConnection = nullptr;  ///< the XrInstance; a session's lifetime
    VrSession *mVrSession = nullptr;   ///< the session; user lifetime
    VrInfo     mVrInfo;
    VrPolicy   mVrPolicy;
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
    /// A host-side refusal (a setter refused against the host's own record, U3 stage 4a): lastError() reports it.
    void noteHostError(const std::string &why);
    /// A join's refusal, moved into the host's error at the rendezvous (render side, the host waiting).
    void moveErrorToHost();
    /// The host's calls are records (EngineConfig::hostRecords) AND this call is the host's (no frame running here).
    bool hostPosts() const { return mHostRecords && detail::tlsEngineSide == 0; }
    /// The create messages' engine-side halves (U3 stage 4a, decision 39 b).
    bool adoptScene(std::unique_ptr<OgreScene> scene, unsigned workerThreads);
    bool adoptScene(std::unique_ptr<OgreScene> scene, unsigned workerThreads, const std::string &name);
    bool adoptOffscreenView(std::unique_ptr<OgreView> view);
    /// THE ENGINE'S STREAM, APPLIED NOW (a join's first step: what the host created before it exists, what it
    /// destroyed is gone) — the messages pending in the slot, in order; the rest of the record waits for its frame.
    void applyPendingEngineMessages();
    /// THE RENDER THREAD (U3 stage 4b): started by Engine::create (EngineConfig::renderThread), which runs init on it.
    bool startRenderThread(const EngineConfig &cfg, std::string &error);
    void teardown();
    /// The compile observer as the host installed it (delivered on the host's thread while it waits, stage 4b).
    std::function<void()> mHostCompileObserver;
    std::atomic<unsigned long long> mCompileTicks{ 0 };
    unsigned long long mCompileTicksDelivered = 0;
    void applyEngineMessage(const EngineMessage &m);
    void applySceneMessage(OgreScene *sc, const SceneMessage &m);
    std::set<std::string> mHostSceneNames, mHostViewNames;   ///< the host's own name records (the creates' checks)
    bool mHostRecords = false;
    /// THIS FRAME IS A FREE-RUNNING ONE (U3 stage 5, RenderThread::tickWith): it interpolates; a lockstep frame draws cur.
    bool mFreeFrame = false;
    /// INSIDE A FREE FRAME'S DRAIN (decision 56, worth-a-look 4): a node record stores prev/cur and leaves the Ogre
    /// node to the interpolation's one write (applySceneStream runs it right after the records, before the change set
    /// and the reflector arm read a pose). Off everywhere else — a lockstep frame and a request's resolve write cur.
    bool mDeferNodeWrites = false;
    /// THIS FRAME WAITED ON THE XR RUNTIME (decision 53): the render loop's pad is skipped after it — xrWaitFrame is the
    /// clock. And the VR frames begun off the render thread (the XR-thread spike's count: 0 is the law).
    bool mFrameWaitedOnRuntime = false;
    unsigned long long mVrFramesOffRenderThread = 0;
    std::chrono::steady_clock::time_point mFakeRuntimeNext{};   ///< vr.fakeRuntimePace's next display time (render side)
    /// THE GRID'S STEP in seconds (FrameInput::stepPeriod; decision 55): never the particles' delta.
    double mStepPeriod = 1.0 / 60.0;
    /// The presentation's frame (once per frame: clamped at the drain's head, advanced at the frame's).
    unsigned long long mPresentFrame = 0;
    bool mPresentBegun = false;
    VrRayState mHostVrRay;   ///< the host's record of the ray it set (Engine::vrRay on the host's side, U3 stage 6)
    /// The host's record of the origin it set, sequenced; the sequence the engine last applied (VrStatus::originSeq).
    Vec3 mHostVrOrigin;
    float mHostVrOriginYaw = 0.0f;
    unsigned long long mHostVrOriginSeq = 0;
    unsigned long long mVrOriginSeq = 0;
    /// A state in which a session exists (Ready..Stopping) — what the host's ended session must not report.
    static bool vrStateIsLive(VrState s) { return s != VrState::Idle && s != VrState::Unavailable; }
    float mPresentAlpha = 1.0f;   ///< the last interpolateScene's weight (PublishedEngineFrame::presentAlpha)
    void beginPresentation();
    void interpolateScene(OgreScene *sc);
    /// THE PRESENTATION CLOCK (U3 stage 5), in the document's step time: advanced by the display period per free frame,
    /// held between the newest record's two grid states — never a wall clock read by the engine.
    double mPresentTime = 0.0;
    unsigned long long mNewestStep = 0;
    double mLastAlphaPresent = 0.0;
    void interpolateFrame();
    mutable std::string mHostError;

    /// PROCESS-WIDE: rides Ogre's single frame-time controller value
    /// (ControllerManager -> FrameTimeControllerValue), held in its
    /// frame-delay mode for the whole process (setFrameDelay zeroes the time
    /// factor, so the wall clock never gets back in).
    /// The frame delta Ogre's clock is applying (the drain's compare); the host's own is Engine::fixedFrameDelta.
    float appliedFrameDelta() const;
    /// FrameInput::frameDelta's application (the drain's).
    void  applyFrameDelta(float seconds);

    /// GLOBAL by construction: HlmsPbs keeps ONE ShadowFilter for every shadowed
    /// light in every scene (HlmsPbs::mShadowFilter). The filter properties are
    /// evaluated in preparePassHash each pass, so changing it at runtime takes
    /// effect next frame — no datablock or workspace rebuild.
    void setShadowFilter(ShadowFilter f) override;

    /// GLOBAL like the filter, but NOT cheap: the sizes live in the shadow-node
    /// DEFINITION, which cannot change while any workspace instantiates it. So:
    /// drop every shadowed view's workspace, swap the definition, re-add them —
    /// the same teardown order the engine's destructor honours.
    void setShadowResolution(unsigned pixels) override;
    void setShadowMapBudget(unsigned maps) override;
    unsigned shadowMapBudget() const;   ///< the pool size in force (the drain's compare)
    ShadowStatus shadowStatus() const;
    void publishShadowStatus(unsigned frames) override;
    void publishRenderStats(unsigned frames) override;
    /// THE BYTES ONE SHADOWED REFLECTION PROBE HOLDS FOR ITS SHADOW NODE
    /// (PCC-BUDGET-2): the probe node's atlas at this engine's live settings
    /// (planShadowAtlas at probeShadowResolution, the derived focused count capped
    /// at kProbeShadowMaxFocusedMaps — buildShadowNode's own arithmetic) plus its
    /// point-light scratch cube (R/2 squared x 6, R32F) when it places a focused
    /// map. The cube's depth buffer is POOLED (one per resolution for the whole
    /// render system), never a probe's. What giProbeGridBytes counts per probe.
    unsigned long long probeShadowNodeBytes() const;
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
    /// THE PBS CHANGE LOG'S DRAIN (ATOM-CPU-WALKS-1, ScenePbs::HashNote), once a frame
    /// before the scenes' scans: a datablock that was re-hashed (or holds an edit not
    /// hashed yet) loses its decode twin if its BUCKET moved, and every Item it was
    /// re-hashed for is re-composed in its scene's table. What the per-frame witness
    /// walks (the split's and the ray tier's) used to find by polling every material.
    void drainPbsChanges();
    std::vector<const Ogre::HlmsDatablock *> mPbsDrainScratch;
    /// What the drain has handled, ever (notes, distinct datablocks) — AtomDrawStatus.
    unsigned long long mPbsDrainNotes = 0ull, mPbsDrainDatablocks = 0ull, mPbsDrainOverflows = 0ull;
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

    /// The persistent shader cache (SHADER_CACHE_SPEC.md). Loaded inside the
    /// first createView() -> ensureHlms(); saved on clean teardown and whenever
    /// the host says a compile burst has settled.
    ShaderCacheStats shaderCacheStats() const override;
    // The log bridge (SESSION_LOG_SPEC F3-B) — OgreLogBridge.cpp.
    void setLogSink(Engine::LogSink sink) override;
    DeviceInfo readDeviceInfo() const;
    bool prepareDevice() override;
    /// Attaches/detaches the Ogre LogListener. attach() runs immediately after
    /// `new Ogre::Root` (beside mShaderCache.attachCounters) so plugin loads,
    /// render-system init and device detection are all captured; detach() MUST
    /// run before Root is deleted.
    void attachLogBridge();
    void detachLogBridge();

    bool renderStats(RenderStats &out) const;
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
    bool readAtomIds(View *view, std::vector<uint32_t> &words, unsigned &width, unsigned &height) override;
    // ---- The ray job's card read, asked directly (OgreRayQuery.cpp) ----
    bool cardReadParity(Scene *scene, const std::vector<CardReadQuery> &queries,
                        std::vector<CardReadPick> &out) override;
    // ---- The one voxel reader's parity harness (OgreVoxelReaderParity.cpp) ----
    bool voxelReaderParity(Scene *scene, const std::vector<VoxelReaderCone> &cones,
                           std::vector<VoxelReaderAnswer> &fragment,
                           std::vector<VoxelReaderAnswer> &compute) override;
    // ---- The environment's cone lookup, measured (OgreEnvironmentCones.cpp) ----
    bool environmentCones(Scene *scene, const std::vector<EnvironmentConeQuery> &queries,
                          std::vector<EnvironmentConeAnswer> &out) override;
    // ---- The render-loop monitor (OgreFrameMonitor.cpp) ----
    void setFrameMonitor(MonitorLevel level) override;
    bool setArm(const std::string &name, double value) override;
    std::vector<ArmInfo> arms() const override;
    ValidationStatus validation() const override;
    const ArmRegistry &armRegistry() const { return mArms; }
    MonitorStatus monitorStatus() const;
    unsigned popMonitorRecords(std::vector<MonitorRecord> &out) override;
    unsigned openMonitorTee() override;
    unsigned popMonitorTee(unsigned tee, std::vector<MonitorRecord> &out) override;
    void closeMonitorTee(unsigned tee) override;
    unsigned popMonitorEvents(std::vector<MonitorEvent> &out) override;
    void noteMonitorEvent(const MonitorEvent &event) override;
    void noteHostStage(const std::string &name, float ms) override;
    /// An engine-internal request for the NEXT frame's cause (the VR warm-up): a record field for a host.
    void noteNextFrameCause(FrameCause cause) { mNextFrameCause = cause; }
    bool framePaceOwesWork() const;
    StreamingWork streamingWork() const;
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
    /// no-op with the monitor off or without fork 1a81f866a+1bccc3f93 (was 0027).
    void gpuFrameBegin();
    /// Builds the compositor-graph half of a snapshot (every live workspace,
    /// its nodes and passes, and which scene each renders).
    void collectCompositorGraph(std::vector<CompositorWorkspaceInfo> &out) const;
    bool saveShaderCache() override;
    bool flushShaderCache(unsigned budgetMs) override;
    bool clearShaderCache() override;
    void setCompileObserver(std::function<void()> observer) override;
    void shaderBuildProgress(unsigned &compiled, unsigned &fromCache,
                             unsigned &expected) const override;
    void setAsyncShaderThreads(unsigned threads) override;
    void setAsyncShaderPlaceholdersOnly(bool on) override;
    AsyncShaderStats asyncShaderStats() const;
    void waitForAsyncShaders() override;

    ~OgreEngine() override;

    /// THE FRAME'S BODY (ENGINE-UPGRADE-U3 F7 as corrected: the public door renderOneFrame stays, and is one turn of
    /// the render loop; the loop calls this with the record the hand-over took). False when no frame ran (a fatal
    /// out-of-memory ended the frames) — nothing to publish then.
    bool runFrame(FrameInput &record, bool any);

private:
    /// THE RENDER LOOP reads the engine's error sink and frame stamp at its hand-over and publish (RenderThread.cpp).
    friend class RenderThread;
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
    /// THE BLANK SCENE MANAGER (lane STALE-VIEW-1): the empty world every
    /// scene-less View's clear-only workspace runs against. One per process and
    /// created on demand — a compositor workspace needs a SceneManager and a camera,
    /// and nothing in here is ever culled, lit or drawn. @see blankSceneManager.
    Ogre::SceneManager *mBlankScene = nullptr;
#ifdef __linux__
    /// The host's X11 `Display*`, kept opaque (see X11Handle) — this TU never
    /// dereferences it, it only hands it back to Ogre.
    void           *mDisplay = nullptr;
#endif
    bool            mHlmsRegistered = false;
    /// A FATAL ensureHlms failure (FORWARD-ONLY-1 D2: the staged tonemap media
    /// is not this build's). Sticky: every later ensureHlms — every later view —
    /// re-reports it instead of running a half-initialised engine.
    std::string     mFatalInitError;
    /// EngineConfig::headless: the NULL render system is loaded, mNullWindow is
    /// the 1x1 window IT created at boot, and no View can exist.
    bool            mHeadless = false;
    /// Frames closed (closeRenderFrame), the PublishedFrame::frame stamp.
    unsigned long long mPublishedFrames = 0;
    /// The background shader compiler's thread count (ASYNC-SHADERS-1;
    /// EngineConfig::asyncShaderThreads, JAH_ASYNC_SHADER_THREADS).
    unsigned        mAsyncShaderThreads = 2;
    /// Placeholder draws / pending skips of scenes already destroyed (asyncShaderStats).
    unsigned long long mRetiredPlaceholderDraws = 0;
    unsigned long long mRetiredPendingSkips = 0;
    /// Starts it and installs the placeholder (ensureHlms; see the definition).
    void startAsyncShaders();
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
    /// THE COUNTERS ARE OPT-IN, FOR EXACTLY A SUBSCRIBED SPAN (P8; U3 stage 3). A workspace listener costs a callback
    /// per compositor pass per frame, so they run only while a reader subscribes (publishShadowStatus) and come off
    /// the render path the frame its span ends.
    unsigned long long mShadowFrame = 0;
    /// THE SUBSCRIPTION (U3 stage 3): the last shadow frame the counters run for (0 = none) — publishShadowStatus.
    unsigned long long mShadowLeaseUntil = 0;
    bool shadowLeased() const { return mShadowLeaseUntil != 0 && mShadowFrame <= mShadowLeaseUntil; }
    /// ...and the render stats': the last published frame they are on the snapshot for (0 = none), and whether the
    /// HUD's stats overlay asked for the geometry counting this frame (it reads them engine-side).
    mutable unsigned long long mRenderLeaseUntil = 0;
    mutable bool               mHudStatsThisFrame = false;
    mutable bool               mMetricsByUs = false;   ///< the counting is on because a subscriber or the HUD wants it
    void syncMetricsRecording(bool want) const;   ///< at the publish (fillPublished is const)
    /// Were the counters actually ARMED for the last frame that rendered? It is
    /// not the same question as `shadowLeased()`: arming happens inside
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
    /// THE DRAIN'S STATE between its steps (WAIT-TEXTURE-LOADS-1): drainTextureStreaming keeps one on its stack, the
    /// host's wait (pumpTextureLoads) one across its calls.
    struct TextureDrain {
        bool active = false;
        std::chrono::steady_clock::time_point t0{};
        size_t bestPending = std::numeric_limits<size_t>::max();
        double lastProgressMs = 0.0;
        double lastPollMs = -1000.0;
        double lastAdvanceMs = -1.0e9;
        double elapsedMs() const {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
    };
    /// `neverDone`: the queues are never taken as empty (the host's wait under the arm textures.pumpFault).
    int beginTextureDrain(TextureDrain &d, bool neverDone = false);
    int stepTextureDrain(TextureDrain &d, bool neverDone = false);
    TextureDrain mHostDrain;
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
    /// THE MONITOR'S HAND-BACK (FRAME-MONITOR-DOOR-1; monitor::HandBack): the finished records and events, popped by
    /// the host (popMonitorRecords / popMonitorEvents) — never a request. Declared BEFORE mMonitor, which holds a
    /// reference to it: it outlives every capture (the stop's flushed tail waits here for its pop).
    monitor::HandBack mMonitorOut;
    /// THE RENDER-LOOP MONITOR (OgreFrameMonitor.cpp). Held by pointer and
    /// NULL while the monitor is off — that is what "zero cost when off" means
    /// structurally: nothing to allocate, nothing to read, and every
    /// instrumentation site in every TU is one `if (monitor::live())` test.
    std::unique_ptr<monitor::FrameMonitor> mMonitor;
    /// THE MEASUREMENT ARMS (lane TEST-1): latched at the top of every frame.
    ArmRegistry mArms;
    bool mMisfitDoorArmed = false;   ///< the arm shader.forceParamsMisfit was on last frame (runFrame)
    /// Ogre's metrics recording when the capture started (F6 restores it).
    bool mMetricsBeforeCapture = false;
    /// Set for the NEXT frame only (FrameInput::cause, drained at the frame's head).
    FrameCause mNextFrameCause = FrameCause::Driver;
    /// What the next frame may put off (OPEN_COVER_SPEC §2.1). `Complete` is
    /// the default and the value every frame is reset to, so only a host that
    /// asks per frame ever gets anything else.
    FramePace  mFramePace = FramePace::Complete;   ///< Engine::setFramePace (the driver's, in force until set again)

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
    /// The in-record door (FrameFault::VulkanOutOfDeviceMemoryInRecord): an Ogre
    /// frame listener, attached only while that kind is armed.
    std::unique_ptr<Ogre::FrameListener> mInRecordFault;
    bool mInRecordFaultAttached = false;
    std::unique_ptr<Ogre::FrameListener> mSplitPublish;   ///< the free frame's publish at its record/swap split (decision 65)
    bool mSplitPublishAttached = false;
    bool mFakeRuntimeWaited = false;   ///< waitBeforeFrame's vr.fakeRuntimePace waited this frame
    void waitBeforeFrame();
    void onRecordSplit();
    /// FORK-OOM-1 — THE FRAME'S WORK WINDOW. The render system's OOM count at the
    /// top of the frame and at the end of its work (the after-record point where the
    /// injected frame fault fires); an OOM between the two, caught by whoever, is
    /// terminal. `mFrameThrewOom`: the frame's own catch saw one (the fallback
    /// without the render system's count).
    GpuFault mGpuFault = GpuFault::None;
    unsigned mOomAtFrameStart = 0u;
    unsigned mOomAtFrameWorkEnd = 0u;
    bool     mFrameWorkEnded = false;
    bool     mFrameThrewOom = false;
};

}  // namespace detail
}}  // namespace jahshaka::engine
