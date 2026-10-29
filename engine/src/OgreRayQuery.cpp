// PHOTON R1 — THE HARDWARE RAY-QUERY TIER.
//
// WHAT THIS IS. A ray-traceable copy of the scene, kept current every frame:
// one bottom-level acceleration structure (BLAS) per unique mesh, built from
// Ogre's OWN vertex and index buffers, and one top-level structure (TLAS) over
// the instances the scene draws. Its consumers — the screen-probe gather, R3
// (hard sun contact shadows, `recordSunContact`) and R5 (ray-traced
// reflections) — every one of them reads this same structure. What R1 ships is
// the structure, the switch that turns it off, and the proof that a ray hits
// where the mathematics says it hits.
//
// WHY IT LOOKS LIKE THIS.
//
//  * OGRE'S RootLayout HAS NO ACCELERATION-STRUCTURE DESCRIPTOR TYPE
//    (OgreRootLayout.h, DescBindingTypes), and its own comment lists the four
//    places that must change if that enum moves — plus the root-layout
//    compatibility hash. So the rays NEVER go through Hlms: this file owns its
//    VkDescriptorSetLayout, its VkPipeline and its descriptor pool, and Ogre's
//    Hlms, RootLayout, BarrierSolver and compositor are not involved in the
//    trace at all. (That is also how Lumen is built: traces are compute,
//    shading reads a cache.)
//  * THE WORK IS RECORDED INTO OGRE'S OWN FRAME COMMAND BUFFER, at the item
//    walk's point (OgreEngine::applyShadowCacheDirties, after updateSceneGraph
//    and before any workspace renders). No private submit, no fence wait and no
//    `stall()` on the frame thread — audit C-13 is explicit that the S3 spike's
//    sync model could not be lifted, and this is what replaces it. The one
//    exception is `traceBlocking` below, the TOOL/TEST path, which says so in
//    its own comment.
//  * THE TRACED SET COMES FROM THE GPU SCENE'S TABLE, which is indexed by the
//    scene's own item slot — never from `SceneManager::getMovableObjectIterator`
//    (audit C-4: the spike traced the editor's gizmo arrows and light icons).
//    The instances are written ON THE DEVICE from that table (the
//    rq_tlas_write.comp job, "THE INSTANCES BY COMPUTE" below; GpuScene.h).
//
// WHAT IT NEEDS FROM THE FORK (jahshaka/ogre-next):
//   * 0038 — the instance at Vulkan 1.2 when the loader allows, the seven
//     extension names, and exactly three feature bits (masked, because upstream
//     queries and enables through ONE pNext chain). Without it vkCreateDevice
//     never hears of ray tracing and `hasRayQuery()` is false everywhere.
//   * 0039 — SHADER_DEVICE_ADDRESS + ACCELERATION_STRUCTURE_BUILD_INPUT on the
//     device-local VBO pools, so a BLAS reads Ogre's buffers IN PLACE. Without
//     it every mesh's geometry would have to be read back and duplicated.
//   * NOTHING ELSE. The SPIR-V is compiled at BUILD TIME at spirv1.4 by
//     glslangValidator (see irisgl/engine/CMakeLists.txt) — the pin's own GLSL
//     path never sets a target environment, so it would emit SPIR-V 1.0, and
//     the spike's "commit the bytes" shortcut is gone with it.
//
// THE BOUNDARY. One of the engine's six Vulkan TUs (with OgreScreenProbeGather.cpp,
// OgreVrSession.cpp, OgreEngine.cpp and the Atom id/caster passes; the raw pools
// share VkDescriptorPools.h). The public headers stay Vulkan-free (RayQueryStatus is plain data), and on a
// platform without the Vulkan render system the whole file compiles to nothing
// (JAH_RAY_QUERY, set by CMake — macOS takes that path, because MoltenVK
// exposes neither extension: SPECS/research/MOLTENVK_RAY_QUERY_2026-09-14.md).
#include "AtomPass.h"
#include <thread>
#include "ReadbackDecode.h"
#include "EnginePrivate.h"
#include "GpuCheckpoints.h"
#include "SceneLightList.h"
#include "photon/voxel/PhotonVoxelMaterial.h"
#include "photon/voxel/PhotonSpecularAlbedo.h"
#include "photon/voxel/PhotonIrradianceField.h"
#include <OgreLight.h>

#include <Compositor/OgreCompositorNode.h>
#include <Compositor/Pass/OgreCompositorPass.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassSceneDef.h>
#include <Compositor/Pass/PassScene/OgreCompositorPassScene.h>
#include <Compositor/OgreCompositorShadowNode.h>
#include <Vao/OgreVertexArrayObject.h>
#include <Vao/OgreVertexBufferPacked.h>
#include <Vao/OgreIndexBufferPacked.h>
#include <Vao/OgreVaoManager.h>
#include <Vao/OgreUavBufferPacked.h>

#if JAH_RAY_QUERY

#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanMappings.h"
#include <OgrePixelFormatGpuUtils.h>
#include "OgreVulkanDevice.h"
#include "OgreVulkanQueue.h"
#include "Vao/OgreVulkanBufferInterface.h"
#include "Vao/OgreVulkanVaoManager.h"

#include "rayquery/rq_rays_spv.h"
#include "rayquery/rq_reflect_spv.h"
#include "rayquery/rq_reflect_filter_spv.h"
#include "rayquery/rq_reflect_temporal_spv.h"
#include "rayquery/rq_reflect_resolve_spv.h"
#include "rayquery/rq_reflect_compose_spv.h"
#include "rayquery/rq_motion_spv.h"
#include "rayquery/rq_motion_skin_spv.h"
#include "rayquery/rq_card_parity_spv.h"
#include "rayquery/rq_sun_contact_spv.h"
#include "rayquery/rq_area_vis_spv.h"
#include "rayquery/rq_area_vis_resolve_spv.h"
#include "rayquery/rq_hit_composite_spv.h"
#include "rayquery/rq_card_movers_spv.h"
#include "rayquery/rq_card_gather_spv.h"
#include "rayquery/rq_glass_escape_spv.h"
#include "rayquery/rq_glass_escape_add_spv.h"
#include "rayquery/rq_voxel_sides_spv.h"
#include "rayquery/rq_tlas_write_spv.h"
// THE SCREEN-PROBE GATHER — a Component of ours (GATHER-1a). Its three compute
// jobs, its atlases and its pipelines live in OgreScreenProbeGather.cpp; this
// file is its HOST (the device, the retire window, the frame's command buffer,
// the TLAS) and the one that is friends with the scene it reads.
#include "HlmsAtom.h"
#include "ScreenProbeGather.h"
#include "SkinCache.h"
#include "SurfaceCache.h"
#include "VkDescriptorPools.h"

#include <Animation/OgreSkeletonInstance.h>
#include <OgreHlmsCompute.h>
#include <OgreHlmsComputeJob.h>
#include <OgreHlmsManager.h>
#include <OgreHlmsPbsDatablock.h>
#include <OgreItem.h>
#include <OgreResourceTransition.h>
#include <OgreRoot.h>
#include <OgreSubItem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <unordered_set>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace jahshaka { namespace engine { namespace detail {
namespace {

using Clock = std::chrono::steady_clock;
inline double msSince(const Clock::time_point &t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// HOW MANY FRAMES OF TIMESTAMPS ARE IN FLIGHT. A timestamp is read back with
/// the availability bit and NEVER with VK_QUERY_RESULT_WAIT_BIT — a wait is on
/// availability, not on a fence, and waiting for a query that was reset but
/// never written hangs forever (the S3 spike lost an hour to exactly that).
/// Three is Ogre's own dynamic-buffer multiplier, i.e. the depth of the
/// pipeline this work rides.
constexpr unsigned kFramesInFlight = 3u;
/// HOW MANY VIEWS the reflect pool plans descriptor rings for (it used to be
/// the timestamp pools' ceiling; the tier keeps no timestamp pool since lane
/// TEST-1 — every GPU time is a monitor CacheScope row).
constexpr unsigned kMaxTimedScenes = 8u;
/// Compaction size queries live in a RING, not at slots 0..n: two BLAS batches
/// within the read-back window would otherwise write the same slots and the
/// second batch's sizes would be read as the first's — a COMPACT copy into a
/// buffer sized for a different structure.
constexpr unsigned kCompactRing = 64u;

/// A plain owned VkBuffer. Ogre's VaoManager cannot make one with the usage
/// bits an acceleration structure needs (AS storage, scratch, instance arrays),
/// so everything that is not BLAS INPUT is ours.
struct RawBuffer {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize  size = 0;
    void         *mapped = nullptr;
};

/// THE PORTAL SCAN's INPUT AND ANSWER (updatePortals): a copy of the shares and their grid, the SH in force; the table's
/// words and the published entries - a pure function of its input, run on its own thread (no render-thread stall: the
/// scan is ~0.3 s at 128^3 in a Debug build).
struct PortalScanIn {
    std::vector<uint32_t> words;
    int W = 0, H = 0, D = 0;
    float origin[3] = {}, cell[3] = {};
    float sh[27] = {};
    bool haveSh = false;
};
struct PortalScanOut {
    std::vector<uint32_t> words;
    std::vector<RayQueryStatus::ReflectEmitter> table;
    double ms = 0.0;
};

/// Every device entry point this tier uses; all are extension functions, so
/// none is in the loader's static export table.
struct RtFuncs {
    PFN_vkGetBufferDeviceAddressKHR getBufferDeviceAddress = nullptr;
    PFN_vkCreateAccelerationStructureKHR createAccelerationStructure = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAccelerationStructure = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR getBuildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR cmdBuild = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAsDeviceAddress = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR cmdWriteProperties = nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR cmdCopy = nullptr;

    bool load(VkDevice dev) {
#define JAH_RT_LOAD(member, name)                                           \
    member = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name)); \
    if (!member) return false
        JAH_RT_LOAD(getBufferDeviceAddress, vkGetBufferDeviceAddressKHR);
        JAH_RT_LOAD(createAccelerationStructure, vkCreateAccelerationStructureKHR);
        JAH_RT_LOAD(destroyAccelerationStructure, vkDestroyAccelerationStructureKHR);
        JAH_RT_LOAD(getBuildSizes, vkGetAccelerationStructureBuildSizesKHR);
        JAH_RT_LOAD(cmdBuild, vkCmdBuildAccelerationStructuresKHR);
        JAH_RT_LOAD(getAsDeviceAddress, vkGetAccelerationStructureDeviceAddressKHR);
        JAH_RT_LOAD(cmdWriteProperties, vkCmdWriteAccelerationStructuresPropertiesKHR);
        JAH_RT_LOAD(cmdCopy, vkCmdCopyAccelerationStructureKHR);
#undef JAH_RT_LOAD
        return true;
    }
};


/// One scratch ARENA per batch, handed out at offsets — NOT the spike's one
/// shared buffer with a serialising barrier between every build (which is why
/// its 26 tiny BLAS builds cost 4.6-7.6 ms and why every one of its BLAS
/// figures is an upper bound).
struct ScratchArena {
    RawBuffer     buffer;
    VkDeviceAddress base = 0;
    VkDeviceSize   used = 0;
    VkDeviceSize   alignment = 256;
};

// ---- R5: ray-traced reflections (PHOTON_SPEC §7 R5) ------------------------

// THE GATE'S VALUE IS NOT HERE. It used to be, as `kRayReflectRoughness = 0.4f`,
// documented as sitting "just above `ssrRoughnessCutoff`'s 0.35 default on
// purpose" — and by the time lane SSR-3 read it, every part of that was wrong:
// the constant was DEAD (RAYROW-1 made the cutoff the project's, read at the one
// call site below), and the 0.35 it claimed to sit above was never a perceptual
// number at all (the march compared it to an un-square-rooted GGX alpha, so it
// was a band at perceptual 0.581, ABOVE this constant rather than below it).
// Deleted with the second cutoff it described; there is one cutoff now and the
// project owns it.

// THE GATE'S FEATHER IS NOT HERE EITHER, and for a reason worth the line: since
// lane SSR-3 the screen-space march fades over the SAME 0.1, so a copy of it in
// this file would be two numbers one edit apart. `kRayReflectFeather` lives in
// EnginePrivate.h, which both this TU and OgreChain.cpp already include, and its
// note there says what each half does with it.
/// THE MOVING MEMORY: Lumen's MaxFrames (r.Lumen.Reflections.Temporal.MaxFramesAccumulated, 12; PHOTON-7 stage 6 commit 6,
/// LUMEN_VR_FACTS C7.2) - the temporal pass' window N = min( N c + 1, MaxFrames ), shortened by the clamp's confidence
/// where the light or the reflected thing changed; one frame for a lobe under a texel (the mirror ramp:
/// jah_reflect_draw.glsl). PHOTON-2's 32 is deleted with its SIX readers re-derived (PREFLIGHT_R W9): the hit's sample
/// footprint and the sky's mip (both ~ 1 / sqrt( N ), rq_reflect.comp), the rest's firefly bound and the moving window
/// (rq_reflect_temporal.comp, through knobs2.w), and the rest's hand-over length (reflectRestLength below: 51 -> 19).
constexpr unsigned kReflectMaxFrames = 12u;
/// HOW MANY CASCADES THE HIT SHADING READS — the shader's `kMaxCascades`, and
/// the two must agree: the hit set's (HitSetSources).
constexpr unsigned kMaxReflectCascades = kGatherMaxCascades;
/// Descriptor sets and parameter buffers in flight per view. A set bound by a
/// command buffer that has not retired may not be rewritten, and this set is
/// rewritten every frame (every input can be recreated behind our back).
constexpr unsigned kReflectRing = 3u;
/// THE REFLECTION'S REST (PHOTON-2 1e, fix F1): the gather's rule — a rest mean restarted at the first rest frame and
/// handed over from the moving history over N frames, then the hold. N is the gather's own function of the history's
/// length (gatherSettleFramesOf: a 5-code step under one code through the MaxFrames-12 window, 19 frames).
unsigned reflectRestLength(unsigned window) { return gatherSettleFramesOf(window); }
/// THE REST'S SAMPLE SEQUENCE (commit 8): a rest sample's index is this base plus the sample's own number v (the
/// shader adds it: per SAMPLE, never per frame - a frame's batch of k and a dropped record's retry are each the
/// sequence's next), past every moving frame's (`frame & 0xFFFF`), so the samples a held picture averages are the same at
/// every stop at one pose - the rest mean is a function of the scene and the eye, never of the path (the gather's rest
/// mean's law, rq_probe_integrate.comp). The reference arm's value 2 offsets it by 0x8000 (+ a 16-bit v: < 2^18, exact).
constexpr unsigned kReflectRestSequence = 0x10000u;
/// THE REST'S TAIL BUDGET, A COUNT PER VIEW (L7; the lead's rulings after the hold's tail): the rest's first batching
/// frame traces k = GiQualityFacts::reflectRestRays rays a batching texel - its COUNT, k x the texels batching then -
/// and each frame after k_active = max(k, min(kReflectRestRaysCap, count / (kReflectRestTailDivisor x batching))), the
/// batching texels from the newest read-back (a fixed kFramesInFlight behind: deterministic). So the rays a frame are
/// at most max(k x batching, count / 8): the scattered tail traces at most an EIGHTH of the first frame's rays. WHY NOT
/// THE WHOLE COUNT (measured, photon-glass): concentrated on the scattered edge texels the first frame's rays cost 3.3-
/// 4.4x as much a ray (3.5 ms against 1.04 at k 64) - the hardware's BVH traversal, which no count the shader can make
/// sees (a work count of ray queries and march steps read 24 a ray there against 29 at the first frame). The divisor
/// is DERIVED: an eighth of the rays at even 8x the cost a ray stays under the first frame's ms (the peak measured
/// 4.4x: a 1.8x margin). The trace DEALS a subgroup's samples one per lane (rq_reflect.comp, the rest's spread lanes),
/// so the cost follows the rays. The cap is DERIVED: the worst texel measured needed v (SE / 1)^2 = 24.3k samples,
/// ~2.5k of them in by rest frame 100, so >= 44 a frame over the remaining 500 - 64, the next power of two.
constexpr unsigned kReflectRestRaysCap = 64u;
constexpr unsigned kReflectRestTailDivisor = 8u;
/// THE STOP'S FRAMES (jah_reflect_rest.glsl kReflectStopFrames): a texel stops after its standard error stayed within its
/// share this many frames - the rest bound's last term (PHOTON-7 11-1c).
constexpr unsigned kReflectStopFrames = 8u;
/// THE MOVED INSTANCES A REST FOLLOWS PER TEXEL (G6, rq_reflect.comp's restMoverBox): more moved in one frame breaks
/// the view's rest whole.
constexpr unsigned kReflectRestMovers = 16u;
/// Bindings in rq_reflect.comp's set 0: the trace's fifteen, then the card
/// read's four (jah_rq_card_bindings.glsl at JAH_CARD_BINDING_BASE 15 — the
/// card table, the instance table, the Depth and Radiance layers).
constexpr unsigned kReflectBindings = 57u;
constexpr unsigned kReflectCardBinding = 15u;
/// ...then the hit's geometric normal (PHOTON-CARDS-2 fix round): the per-slot
/// geometry-row table the TLAS writer fills (19) and the GPU scene's geometry
/// rows (20) — rq_reflect.comp through jah_rq_geom.glsl.
constexpr unsigned kReflectGeomBinding = 19u;
/// ...then (PHOTON-VOXEL-4, RQ-COV-SLOT-1) every cascade's PER-HALF-AXIS COVERAGE (21 the
/// faces looking +a, 22 looking -a) and SURFACE POSITION (23, 24), by name: the hit's read
/// takes the opacity along its ray from the coverage. It used to ride the `voxelX` array by
/// the light-volume list's ORDER - right only while the list kept that order. The position
/// is the origin plane's, which the hit's point reads never take; the reader binds it
/// wherever it runs.
constexpr unsigned kReflectCovBinding = 21u;   // then 22, 23, 24: covN, posP, posN
/// ...then THE HIT RECORD (PHOTON-HIT-SHADE-1): the GPU scene's instance table
/// (25), the hit list's two images (26-27) and its buffer (28: the counters and
/// each record's sun, footprint and weight) — jah_rq_hit_record.glsl.
constexpr unsigned kReflectHitBinding = 25u;
/// ...and (PHOTON-VOXEL-5) level 0's BACK side (29) and the voxelizer's NORMAL (30): the hit's
/// level-0 read takes the side of a two-sided voxel its ray meets.
constexpr unsigned kReflectSideBinding = 29u;
/// ...then THE CARD READ'S VIEW TERM (PHOTON-CARDS-5): the surface cache's
/// Indirect, Flat, ShadowRough, Albedo, Normal and Specular layers and the sun's visibility (PHOTON-3) (31-37,
/// jah_rq_card_bindings.glsl at JAH_CARD_VIEW_BINDING_BASE 31) — the read
/// restores the diffuse lobe's view term for the ray (jah_card_view.glsl).
constexpr unsigned kReflectCardViewBinding = 31u;
/// ...and THE PHOTON VIEW'S OVERLAY (PHOTON-VIEW-1): one storage image the trace
/// writes the Hits picture into (38, rq_reflect.comp).
constexpr unsigned kReflectPhotonBinding = kReflectCardViewBinding + SurfaceCache::kViewLayers;
/// ...and THE VIEW'S ID IMAGE (REFLECT-MOVERS-1): the visibility buffer's
/// `jahAtomIds` (which slot drew each pixel), so a pixel on a MOVING slot finds its
/// history through that slot's two poses (39, rq_reflect.comp).
constexpr unsigned kReflectIdBinding = kReflectPhotonBinding + 1u;
/// ...and THE DIRECT TERM's own volume per cascade (40, MOVER-OCCLUSION-1): a hit's
/// store radiance less it is the share a MOVER above the hit occludes and the store
/// cannot know (jah_rq_hit.glsl); the total stands in where a cascade has none.
constexpr unsigned kReflectDirectBinding = kReflectIdBinding + 1u;
/// ...and THE AIR'S TABLE (41, PHOTON-I-1 fix 5): the aerial-perspective volume the
/// colour passes read, for the fog along the reflection (jah_fog_along.glsl).
constexpr unsigned kReflectFogBinding = kReflectDirectBinding + 1u;
/// ...and THE ID PASS'S OWN DEPTH (42, ID-DEPTH-1 for the reflection — PHOTON-II-1
/// item 11): the scene depth copied right after the id pass, so a pixel whose final
/// surface is NEARER than the id's (a stock-drawn character in front of a moving
/// Atom mover) is not taken for the mover (rq_reflect.comp).
constexpr unsigned kReflectIdDepthBinding = kReflectFogBinding + 1u;
/// ...and THE HIT'S OWN SURFACE (43, 44; FAR-SUN-2, jah_rq_hit_direct.glsl): the voxeliser's
/// texture pool and the cloud layer's field.
constexpr unsigned kReflectPoolBinding = kReflectIdDepthBinding + 1u;
constexpr unsigned kReflectCloudBinding = kReflectPoolBinding + 1u;
/// ...and THIS FRAME'S RAW SAMPLE (45, PHOTON-2 1b): the trace's (or the write-back's) one ray of the frame, which the
/// temporal pass (rq_reflect_temporal.comp) clamps the history against and blends in.
constexpr unsigned kReflectSampleBinding = kReflectCloudBinding + 1u;
/// ...and THE REST'S REFERENCE (46, PHOTON-2 round 2, R1 (f)'s arm "reflect.restReference"): a 32-bit running mean of
/// each texel's raw rest samples, 1 x 1 unless the arm is on.
constexpr unsigned kReflectReferenceBinding = kReflectSampleBinding + 1u;
// (THE REST'S SECOND MOMENT rides the rest half's alpha since PHOTON-7 stage 6 - its R16F image is deleted.)
/// ...and THE SCREEN TRACE'S PICTURE (47-49, PHOTON-7 stage 6, the one estimator): the chain's previous picture, the
/// depth it was drawn at and the object motion (rq_reflect.comp's screenAnswer), or 1 x 1 stand-ins with `screen.x` 0.
constexpr unsigned kReflectScreenBinding = kReflectReferenceBinding + 1u;   // then 48 the depth, 49 the motion
/// ...and THE EXPOSURE (50, PHOTON-7 stage 6 commit 6): the chain's measured exposure history (a 1 x 1, the multiplier
/// the tonemapper scaled the last frame by) - the denoiser space is Lumen's on pre-exposed colour
/// (jah_reflect_denoise.glsl) - or a stand-in where the multiplier is a constant (ReflectParams::denoiser).
constexpr unsigned kReflectExposureBinding = kReflectScreenBinding + 3u;
/// ...and THE REST'S STOP SHARE (51, PHOTON-7 stage 6; jah_reflect_rest.glsl's stop): a trace texel's share of the
/// one-code display budget - the MIN over the pixels that read it of each one's (r32ui, a positive float's bits, by
/// the filter's atomic min at rest; all ones = no pixel measured it) - read and reset by the next frame's temporal
/// pass, which stops the texel when its own standard error is within it. A STATISTIC read for the stop, never colour
/// fed back.
constexpr unsigned kReflectShareBinding = kReflectExposureBinding + 1u;
/// ...and THE REST'S COMPACT LIST (52, PHOTON-7 stage 6, the lead's compact path): two halves of kReflectCompactTexels
/// texel indices and their counts - the tile pass lists, in the half of this frame, each texel still converging, and
/// when last frame's list fit (at most kReflectCompactTexels) leaves them to the COMPACT PASS, one warp a texel.
constexpr unsigned kReflectCompactBinding = kReflectShareBinding + 1u;
/// ...and THE MOVING SE INSTRUMENT (53, PHOTON-7 stage 6, the lag law's row): a float a trace texel - the temporal pass'
/// standard error of the texel's moving mean in DISPLAY CODES (its luma variance over its frames, carried through the
/// denoiser space's slope to the display table) - copied beside the state by the history readback (the arm
/// reflect.historyReadback) into RayQueryStatus::reflectHistorySe. Sized only while the arm is on: a 16-byte stand-in
/// else, and the pass writes nothing past its length.
constexpr unsigned kReflectMovingSeBinding = kReflectCompactBinding + 1u;
/// ...and THE DIRECT TERM's BACK side per cascade (54, PHOTON-7 stage 7: the direct store holds the two sides, and the
/// hit's share and its mover gate read their mean - jah_rq_hit.glsl; moved past stage 6's bindings at the merge).
constexpr unsigned kReflectDirectBackBinding = kReflectMovingSeBinding + 1u;
/// ...and THE SAMPLE's NEE PART (55, PHOTON-7 stage 11, 11-1b): the next-event estimate inside this frame's sample, which
/// the resolve keeps apart from the BRDF part its density ratio reweights (rq_reflect_resolve.comp).
constexpr unsigned kReflectNeeBinding = kReflectDirectBackBinding + 1u;
/// ...and THE PICK (56, PHOTON-7 11-2: the reds' instrument, jahlab's `pick`): one trace texel's sample and rest terms,
/// a frame a slot of a 64-slot host-visible ring (kReflectPickWords words a slot), while the arm reflect.pick names it.
constexpr unsigned kReflectPickBinding = kReflectNeeBinding + 1u;
constexpr unsigned kReflectPickSlots = 64u, kReflectPickWords = 64u;
static_assert(kReflectBindings == kReflectPickBinding + 1u, "the pick is the reflection set's last binding");
/// THE COMPACT PATH'S LIST LENGTH L (the lead's ruling): 16,384 texels - 64 KB a half - below which the rest's tail runs
/// one warp a texel, its samples dealt over that warp's lanes, so the busiest warp is one texel's (clustered edge
/// texels made it ceil( m k / 32 ) rounds, the dispatch's critical path on a near-idle GPU).
constexpr unsigned kReflectCompactTexels = 16384u;
/// THE HIT LIST'S BUFFER LAYOUT (jah_rq_hit_record.glsl kJahHitAuxBase / kJahHitAuxWords, 800.Atom_piece_ps.any's read):
/// twelve counter words (PHOTON-7 11-1b/c added four: the rest bound's two, the NEE's two), then ten words a record
/// (the NEE delta + the estimator's gain 6-7, PHOTON-7 11-1b / 11-2; a reflection's crossed panes 8-9, 10-4 F6).
constexpr uint32_t kHitCounterWords = 12u;
constexpr uint32_t kHitCounterBytes = kHitCounterWords * 4u;
constexpr uint32_t kHitAuxWords = 11u;   // (L9 (b1): word 10 the decoded hit's area lights' visibility)
/// THE HIT's AREA STRATA (jah_rq_hit_direct.glsl kHitAreaStrata = 4 a side): one full cycle of the rest's time-stratified
/// area draw, the samples a rest texel's stop waits for where the hit table holds an area light (PHOTON-7 11-2 (2)).
constexpr uint32_t kHitAreaStrataPerHit = 4u * 4u;
/// THE HIT WRITE-BACK's bindings (rq_hit_composite.comp): params, the list's
/// buffer, the destinations, the decoded radiance, the reflection's STATE texel (an unshaded record's distance is
/// withheld there), the gather's atlas — then (PHOTON-I-1 fix 5) the list's records (the ray's direction) and the
/// air's table, the reflection's fog along its ray — and (PHOTON-2 1b) the reflection's raw SAMPLE a shaded record
/// becomes — and (PHOTON-7 stage 6, the record runs) the rest's in-place halves a run folds into: the history this
/// frame and last, the state this frame and last.
constexpr unsigned kHitCompositeBindings = 12u;

/// A storage image this file owns outright — the temporal mean and the distance
/// beside it. Not an Ogre texture: nothing but this compute pass ever reads or
/// writes one, it is never a render target, and giving it to the compositor
/// would put a per-frame race (the ping-pong) inside a graph that rebuilds.
struct ReflectImage {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
};

/// ONE BOTTOM-LEVEL STRUCTURE PER (MESH, LEVEL) — ATOM-FARBLAS-1, A5b §4. A mesh
/// used to have exactly one, over level 0. Now every instance is written TWICE
/// into the one TLAS: a NEAR copy over level 0 (ONE RAY LEVEL PER MESH, PHOTON-7
/// stage 6) and a FAR copy over the mesh's COARSEST level,
/// so the table is keyed by the level too. A mesh with one level asks for (mesh,
/// 0) twice and gets ONE structure — the "no second build" of the decision falls
/// out of the key rather than being a special case.
struct BlasKey {
    const Ogre::Mesh *mesh = nullptr;
    uint32_t level = 0;
    bool operator==(const BlasKey &o) const { return mesh == o.mesh && level == o.level; }
};
struct BlasKeyHash {
    size_t operator()(const BlasKey &k) const {
        return std::hash<const void *>()(k.mesh) ^ (size_t(k.level) * 0x9E3779B97F4A7C15ull);
    }
};
/// What a gather asks to have built: the mesh (HELD, see Blas::mesh) and the level.
struct BlasWant {
    Ogre::MeshPtr mesh;
    uint32_t level = 0;
};

/// THE COARSEST LEVEL a mesh carries — the chain's last (ATOM-BAKE-1's
/// 128-triangle floor), 0 for a mesh with no chain. Read off the VAO list the
/// draw indexes, which is the list a BLAS is built from.
inline uint32_t coarsestLevelOf(const Ogre::Mesh *mesh) {
    if (!mesh || mesh->getNumSubMeshes() == 0) return 0u;
    const size_t n = mesh->getSubMesh(0)->mVao[Ogre::VpNormal].size();
    return n > 1u ? uint32_t(n - 1u) : 0u;
}

}   // namespace

// ---------------------------------------------------------------------------
/// THE TIER. One per engine: the device plumbing and the compute pipeline are
/// process-wide, the acceleration structures are per SCENE (each scene draws
/// its own items, and a probe preview or a thumbnail scene must never appear in
/// the editor's structure).
/// One pipeline an ahead build made on a worker (RayQueryTier::startAhead): logged by the render thread at the join.
struct RayAheadNote {
    std::string name;
    double ms = 0.0;
    VkResult r = VK_SUCCESS;
};

class RayQueryTier final : public GatherHost {
public:
    ~RayQueryTier() { close(); }
    /// THE SLOTS THE SKIN PASS POSED THIS FRAME (`vaoFrame`; the changed-region list's movers, OgreEngine::
    /// listRayMovers), or null: a scene with no ray structures, or no pose moved on that frame.
    static const std::vector<uint32_t> *posedSlotsOf(const OgreScene *scene, uint32_t vaoFrame) {
        const SceneAs *sa = asOf(scene);
        return sa && sa->skinPosedFrame == vaoFrame ? &sa->skinPosedSlots : nullptr;
    }
    /// A texture a VIEW owns and this tier's sets may still name (the photon
    /// view's overlay, PHOTON-VIEW-1): destroyed through the retire window.
    void retireViewTexture(Ogre::TextureGpu *&t) { retireTexture(t); }

    /// Resolves the device, the entry points and the pipeline. False (with a
    /// reason in `err`) when this device has no ray query — which is not an
    /// error anywhere, it is the fallback picture.
    bool open(Ogre::RenderSystem *rs, std::string &err);
    void close();
    /// Waits for the device (the shutdown's one wait, before releaseScene).
    void waitIdle() { if (mVk) vkDeviceWaitIdle(mVk); }
    /// Destroys a LIVE scene's structures outright, behind an idle device (the tier
    /// is closing while the scene lives: the no-rays switch, the shutdown).
    void releaseScene(OgreScene *scene);
    bool isOpen() const { return mVk != VK_NULL_HANDLE; }

    /// THE FRAME'S UPDATE for one scene, recorded into `Ogre`'s current command
    /// buffer. Cheap and O(1) on a still frame: the transform epoch gate means
    /// a scene that did not move records nothing at all.
    void updateScene(OgreScene *scene);
    /// Frees everything held for a scene that is going away.
    void forgetScene(OgreScene *scene);
    /// THE TIER IS CLOSING WHILE `scene` LIVES (the no-rays switch): its GPU scene
    /// stops telling this tier's feed anything. close() cannot do it — the scenes it
    /// is keyed by may already be gone — so the engine, which knows which live,
    /// calls this first.
    void detachFeed(OgreScene *scene) {
        SceneAs *sa = asOf(scene);
        if (!sa || !sa->feedOn) return;
        scene->gpuScene().removeObserver(&sa->feed);
        sa->feedOn = false;
    }

    /// THE TOOL/TEST TRACE — the only place in this file that submits its own
    /// command buffer and waits on a fence, and it must never be called from a
    /// frame. It exists so that a suite can ask "what does this ray hit?" and
    /// get an answer in the same statement; every PRODUCT consumer (R2/R3/R5)
    /// will read the structures from a compute pass inside the frame instead.
    ///
    /// `rays` is {origin.xyz, tMin, dir.xyz, tMax, mask, 0,0,0} per ray; `hits`
    /// comes back as {t (<0 = miss), customIndex, primitiveIndex, hit?1:0}.
    bool traceBlocking(OgreScene *scene, const std::vector<float> &rays,
                       std::vector<float> &hits, std::string &err);
    /// gi.card_read_parity's GPU half (PHOTON-CARDS-2): the reflection's card
    /// read (jah_rq_card.glsl) asked at `queries` through a test-only job
    /// (rq_card_parity.comp) over the scene's own surface cache. Flushes
    /// Ogre's commands, submits, stalls — a suite, never a frame.
    bool cardPickBlocking(OgreScene *scene, const std::vector<CardReadQuery> &queries,
                          std::vector<CardReadPick> &out, std::string &err);

    RayQueryStatus status(const OgreScene *scene) const;

private:
    struct Blas {
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        RawBuffer storage;
        VkDeviceAddress address = 0;
        unsigned triangles = 0;
        /// The mesh is HELD, not observed: a BLAS that outlives its vertex
        /// buffers would be built over recycled VRAM, and the cache is keyed by
        /// the raw pointer, which a freed-and-reallocated Mesh would alias.
        Ogre::MeshPtr mesh;
        /// The mesh LEVEL this structure was built from (BlasKey).
        uint32_t level = 0;
        /// Compaction state machine: 0 = built, size not asked; 1 = size query
        /// written, waiting; 2 = compacted (or declined).
        unsigned compactState = 0;
        /// A traced slot references this (mesh, level) now (the change feed's key count > 0;
        /// ENGINE-UPGRADE-U5 stage 3): it is never stale. `lastSeen` is the frame the last
        /// reference went, which is all eviction reads - kept at the change, never stamped per frame.
        bool referenced = false;
        unsigned compactSlot = 0;
        uint32_t compactFrame = 0;
        /// The last frame an instance referenced this mesh (finding 5). A BLAS
        /// nothing has traced for a while is memory — and a held MeshPtr — kept
        /// for a mesh the scene may never show again.
        uint32_t lastSeen = 0;
    };

    struct SceneAs {
        std::unordered_map<BlasKey, size_t, BlasKeyHash> blasOf;
        std::vector<Blas> blas;

        VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
        RawBuffer tlasStorage, tlasScratch;
        /// THE INSTANCE ARRAY, WRITTEN ON THE DEVICE (ATOM-CPU-WALKS-1): the instance
        /// job's output (rq_tlas_write.comp), TWO instances per GPU scene slot at fixed
        /// places (near at 2 x slot, far at 2 x slot + 1; an untraced slot's pair
        /// INACTIVE), so the build's primitive count is `2 x slotCount` and known on
        /// the host. Device-local, storage + build input + a device address. Grown by
        /// doubling; a replaced one goes through the retire bin (a build recorded in a
        /// frame still in flight reads it).
        RawBuffer tlasOut;
        uint32_t tlasOutCap = 0;
        /// ...and the job's per-MESH inputs (writeTlasInputs): the header, every
        /// mesh-table entry's level addresses and coarsest level, the ready rigged
        /// slots. `tlasInWords` is their CPU copy; `inputsVersion` moves when it
        /// changes, and each ring slot (a host-visible buffer per frame in flight)
        /// is re-written only when it holds an older version.
        std::vector<uint32_t> tlasInWords, tlasInScratch;
        unsigned long long inputsVersion = 1ull;
        RawBuffer tlasIn[kFramesInFlight];
        uint32_t tlasInCap[kFramesInFlight] = {};
        unsigned long long tlasInVersion[kFramesInFlight] = {};
        VkDescriptorSet tlasSets[kFramesInFlight] = {};
        /// What the mesh records were written for: the BLAS set's version (every
        /// address a build, a compaction or an eviction moved) and the GPU scene's
        /// mesh-set serial.
        unsigned long long blasVersion = 1ull, inputsBlasVersion = 0ull, inputsMeshSerial = ~0ull;
        /// THE INPUTS ARE PATCHED, NOT REBUILT (ENGINE-UPGRADE-U5 stage 3): the meshes whose structures
        /// changed since the job's inputs were written (a build, a compaction's new address, an eviction),
        /// each re-writing its own record; `inputsNeedFull` (a failure path, the first write) re-writes
        /// all of them. `meshIndexOf` is the mesh table's index of each mesh, re-made when the mesh set's
        /// serial moves.
        std::vector<const Ogre::Mesh *> dirtyMeshRows;
        bool inputsNeedFull = true;
        std::unordered_map<const Ogre::Mesh *, uint32_t> meshIndexOf;
        /// THE (mesh entry, level) PAIRS THE SET REFERENCES AND NOBODY BUILT YET, ascending (the order
        /// ensureBlas builds in), kept by the feed's key events - the scan over every key is gone.
        /// keyed by the feed's (mesh, level) key, the value the mesh-table entry it was last seen at
        /// (decision 14.5: keyed by the entry, a mesh re-indexed before its build was stranded).
        std::unordered_map<BlasKey, uint32_t, BlasKeyHash> missing;
        /// THE STRUCTURES WAITING ON THEIR COMPACTED SIZE (compactState 1; decision 14.4: the frame's
        /// "anything owed?" and runCompaction read this list, never every structure).
        std::vector<size_t> compactWaiting;
        /// THE EVICTION SWEEP'S CLOCK (decision 14.4): one pass over the structures per
        /// kEvictSweepFrames, never one a frame.
        uint32_t evictSweptAt = 0u;
        /// THE ALPHA AND HIT TABLES' INPUTS as they were last written (stage 3): re-written only when one moves.
        unsigned long long alphaMaskTableSerial = ~0ull, hitMaterialSerial = ~0ull, hitLightSerial = ~0ull,
                           hitLightsRemoved = ~0ull, hitRowsSerial = ~0ull;
        /// The mesh each job record was written for (index = the mesh-table entry): a mesh-set change
        /// re-writes the entries whose mesh changed, never the table.
        std::vector<const Ogre::Mesh *> recordedMeshAt;
        uint64_t alphaRowsAddress = ~0ull;
        /// The build's primitive count (2 x the slot count it was written for).
        unsigned  tlasPrimitives = 0;
        unsigned  instanceCount = 0;      ///< ACTIVE near + far copies: what the TLAS traces
        unsigned  farInstanceCount = 0;   ///< ...of which the far copies
        /// The widest coarse-vs-fine gap of the set this TLAS was written from
        /// (world units; each slot's coarsest-level bound under the drift law,
        /// rayOriginLevelLift): the far queries start this far BEFORE their near
        /// length (audit F2), and a far copy's hit lifts its sun leg by it. Held
        /// with the TLAS, so a still frame keeps it.
        float     farOverlap = 0.0f;
        /// THE GEOMETRY ROW OF EACH SLOT'S NEAR COPY (PHOTON-CARDS-2 fix round):
        /// GpuScene::geomRowIndex(mesh, the level its near BLAS was built from,
        /// submesh 0), 0xFFFFFFFF for a slot not traced — what the reflection
        /// rebuilds a hit's geometric normal from. Kept by the change feed (a
        /// rigged slot's row by the gather, from its ready skin cache).
        std::vector<uint32_t> geomRowOfSlot;
        /// THE ALPHA TABLE (REFLECT-MOVERS-2; jah_rq_alpha.glsl has the layout): what
        /// every ray query reads a CANDIDATE's cut-out from — per alpha-tested traced
        /// slot its near/far geometry rows, and the address of the GPU SCENE's mask
        /// table, which names each submesh's mask and UV transform (ENGINE-POLISH-ATOM-1
        /// stage 3: the masks are the engine's, AlphaMasks.h). A host-visible buffer with
        /// a device address, re-made (the old one retired) only when its words change;
        /// `alphaAddress` 0 = no alpha-tested item (the rays stay opaque).
        RawBuffer alphaTable;
        VkDeviceAddress alphaAddress = 0;
        std::vector<uint32_t> alphaWords, alphaScratch;
        /// THE HIT TABLE (FAR-SUN-2; jah_rq_hit_direct.glsl has the layout): what a
        /// voxel-answered hit computes its DIRECT light and its emissive from AT THE HIT -
        /// the scene's lights (the one light list, world space, the injection's packing) and
        /// the voxeliser's material rows (PhotonVoxelMaterial::getRowWords, the row
        /// GpuInstance::ids.y names). Re-made only when its words change.
        RawBuffer hitTable[kFramesInFlight];        // a frame ring (W1 of the read): written in place
        uint32_t hitTableCap[kFramesInFlight] = {};  // each slot's capacity in words
        VkDeviceAddress hitTableAddress = 0;         // the slot published this frame
        /// THE LAST FRAME EACH SLOT WAS THE PUBLISHED ONE (decision 14.7): a frame in flight reads the
        /// slot published when it was recorded, which is not its own frame's slot once the table goes
        /// unwritten for frames - so a write takes a slot no frame still in flight can read.
        uint32_t hitSlotUntil[kFramesInFlight] = {};
        int hitSlot = -1;
        std::vector<uint32_t> hitWords, hitScratch;
        /// THE EMITTER TABLE (PHOTON-7 stage 11, 11-1a; updateEmitterTable): the reflection's NEE targets, a section of
        /// the hit table (its u[5] / u[6]). `emitterWords` the section as last derived (kEmitterWords a target, ordered
        /// by slot); the inputs it was derived from (the GPU scene's writes, slot count and mesh set, the rows' serial,
        /// the material edits, the tier's cap) - re-derived only when one of them moved; `emitterSetKey` the SET's
        /// words (slot + radiance, never the bounds: a move is no new set) and its serial.
        std::vector<uint32_t> emitterWords, emitterSetKey;
        unsigned long long emitterWrites = ~0ull, emitterRows = ~0ull, emitterEdits = ~0ull, emitterMeshes = ~0ull;
        uint32_t emitterSlotCount = ~0u, emitterCap = ~0u;
        bool emitterDirty = true;   ///< the section changed since the hit table last took it
        /// THE SKY's PORTALS (PHOTON-7 11-2 (6); updatePortals): the openings the voxel sides' escape shares show, as
        /// emitter-table targets (slot kPortalSlot) appended after the emitters; the shares' write count they were built
        /// from and their serial (the emitter table re-derives when it moves)
        std::vector<uint32_t> portalWords;
        unsigned long long portalBuiltWrites = ~0ull, portalSerial = 0ull, emitterPortalSerial = ~0ull;
        unsigned long long portalScans = 0ull;
        double portalScanMs = 0.0;
        std::future<PortalScanOut> portalJob;   ///< the scan in flight (its own thread)
        /// THE HIT TABLE's AREA LIGHTS (PHOTON-7 11-2 (2)): how many it holds - a rest texel's stop then waits for one
        /// full cycle of the hit's time-stratified area draw (kHitAreaStrataPerHit samples).
        uint32_t hitAreaLights = 0u;
        /// THE HIT DECODE'S DRAWS (PHOTON-HIT-SHADE-1): the material words they were
        /// last synced for and HlmsAtom's twin epoch at that sync (updateScene).
        std::vector<uint32_t> decodeWords;
        unsigned long long decodeEpoch = ~0ull;
        /// THE GPU SCENE'S CHANGE FEED (ATOM-CPU-WALKS-1), this tier's side: what it
        /// derives from the table, kept at the table's writes instead of re-derived
        /// by a walk over every slot. The words: every slot's PBS material word (a
        /// hit on any PBS item is shaded by the decode). Registered on the scene's
        /// first update, removed by forgetScene / detachFeed.
        struct Feed final : detail::GpuSceneObserver {
            detail::WordCounts words;
            /// THE TRACED SET, per slot: what the slot contributes to the
            /// top-level structure — its kind (0 not traced, 1 traced through the
            /// mesh's structures, 2 rigged: through its own skin structure), the
            /// (mesh, level) keys of its near and far copies, and the far copy's
            /// hand-over width. Kept so the slot's OLD contribution can be taken
            /// back exactly (the entry's old mesh index may be another mesh's now).
            struct Rec {
                const Ogre::Mesh *mesh = nullptr;
                uint32_t meshIndex = detail::GpuScene::kNoMesh;
                uint32_t farLevel = 0u;   ///< the far copy's (the coarsest); the near copy's is level 0
                uint32_t kind = 0u;
                bool rigged = false;   ///< traced + skinned, mesh or not (the skin pass' want)
                float coarse = 0.0f;
                bool sameKeys(const Rec &o) const {
                    return kind == o.kind && mesh == o.mesh && meshIndex == o.meshIndex &&
                           farLevel == o.farLevel && rigged == o.rigged;
                }
                /// Every fact, the hand-over width included (a finite positive or 0: never a NaN).
                bool same(const Rec &o) const { return sameKeys(o) && coarse == o.coarse; }
            };
            std::vector<Rec> recs;
            /// Every (mesh, level) a traced slot references, with the count and a
            /// mesh-table index to reach the MeshPtr: the bottom-level structures
            /// this scene needs, without a walk (ensureBlas's wants, eviction's
            /// "still referenced").
            struct KeyRef { uint32_t count = 0u; uint32_t meshIndex = 0u; };
            std::unordered_map<BlasKey, KeyRef, BlasKeyHash> keys;
            /// ...AND ITS EVENTS since the tier last took them (ENGINE-UPGRADE-U5 stage 3): a key whose
            /// count went 0 -> 1 (added) or 1 -> 0 (gone), in order; `keysReset` = the whole map was
            /// cleared (the table reset) - every structure unreferenced, then the re-prime adds.
            struct KeyEvent { BlasKey key; uint32_t meshIndex = 0u; bool added = false; };
            std::vector<KeyEvent> keyEvents;
            bool keysReset = false;
            /// An alpha-tested slot changed (entered, left, re-levelled): the alpha table is owed.
            bool alphaDirty = true;
            /// Kind-2 slots (rigged, traced, with a mesh) and the skin pass' wants
            /// (rigged + traced), ascending — the order the old walks visited them.
            std::set<uint32_t> skinned, rigged;
            /// Traced ALPHA-TESTED slots, ascending (the alpha table's rows).
            std::set<uint32_t> alpha;
            /// The far copies' hand-over widths (a multiset; its largest is farOverlap).
            std::map<float, uint32_t> coarse;
            uint32_t traced = 0u;          ///< kind-1 slots
            /// A key, a kind or a slot count moved since the last build: the set
            /// changed (the next TLAS is a rebuild, and a still epoch still builds).
            bool structDirty = true;
            /// The per-slot rows (SceneAs::geomRowOfSlot).
            std::vector<uint32_t> *rows = nullptr;
            OgreScene *scene = nullptr;
            std::atomic<unsigned long long> visits{ 0ull };   ///< the slots told (the lane halves count from the workers)

            void gpuSlotChanged(uint32_t slot, const detail::GpuInstance *now) override;
            void gpuSceneReset() override;
            void take(uint32_t slot, const Rec &r, int sign, bool withKeys = true);
            /// THE LANE HALF (RAY-FEED-OBSERVER-1, the card feed's pattern, U4 decision 17.1): inside the compose task,
            /// on the worker that staged the slot, the slot's NEXT record is measured (pure: the entry and the const
            /// mesh-table / LOD-bound reads) and compared with what the slot contributes now — its record, its
            /// material word, its alpha-table membership and its near row. All equal (the common mover: a transform-
            /// only move changes none of the feed's facts) = the slot owes nothing. Otherwise the record is STASHED
            /// in the slot's own cell and the slot owes the ordered body (apply), replayed in slot order after the
            /// join: everything shared (the counts, the sets, the keys and their events, the dirty flags) is written
            /// there and only there. The lane writes the slot's stash cells alone (pre-sized by gpuSlotsReserve).
            bool hasLaneHalf() const override { return true; }
            const char *gpuObserverName() const override { return "rays"; }
            void gpuSlotsReserve(uint32_t count) override;
            bool gpuSlotChangedLane(uint32_t slot, const detail::GpuInstance *now) override;
            void gpuSlotChangedOwed(uint32_t slot, const detail::GpuInstance *now) override;
            /// THE FEED, FOLDED into the compose's digest (the identity row): per slot of the table its record, word
            /// and near row; the keys in key order and the pending key events in order; the sets, traced, the coarse
            /// multiset, the word counts and the dirty flags. Canonical over the table's slot count (a cell past
            /// it, or not yet sized, folds as the default), so a pre-sized lane arm and the serial arm compare.
            void gpuDigestInto(detail::GpuScene &g) const override;
            /// The slot's next record from the entry as it is now (pure).
            Rec measure(const detail::GpuInstance *now) const;
            /// The ordered body: the slot's contribution replaced by `next`'s.
            void apply(uint32_t slot, const detail::GpuInstance *now, const Rec &next);
            /// THE KEY EVENTS, NETTED PER KEY (before their one consumer, RayQueryTier::update): a key referenced
            /// and released inside one batch (a mesh swap between two slots, a slot that left and came back) files
            /// nothing; a net addition keeps its last mesh-table entry. First-occurrence order.
            void netKeyEvents();
            /// The lane's stash: the measured record, and whether the cell holds this compose's (1) or nothing (0).
            std::vector<Rec> laneNext;
            std::vector<unsigned char> laneStashed;
        };
        Feed feed;
        bool feedOn = false;

        /// THE GPU SKIN CACHE (PHOTON-SKIN-1, SkinCache.h): one entry per RIGGED
        /// traced item, by NodeId — its posed vertex buffer, its row block in the
        /// GPU scene, and ITS OWN bottom-level structure (a skinned item's BLAS
        /// is per ITEM, never shared: two characters on one mesh wear two poses).
        struct Skin {
            /// THE CACHE'S IDENTITY is (Item, Mesh, the node's rig generation):
            /// attachSkinnedMesh re-attaches IN PLACE (detachItem, then createItem),
            /// so a new Item can land at the old one's address — the pointer alone
            /// would keep a cache sized and addressed for a mesh that may be gone.
            const Ogre::Item *item = nullptr;
            const Ogre::Mesh *mesh = nullptr;
            unsigned long long rigGeneration = 0ull;
            /// What the cache and its structure were built over, re-checked against
            /// the live VAO on every pass before anything is skinned or refit.
            const Ogre::IndexBufferPacked *indices = nullptr;
            SkinCacheBuffer buf;
            uint32_t rowBlock = 0xFFFFFFFFu;  ///< the GpuScene row block (a mesh-table entry)
            uint32_t row = 0xFFFFFFFFu;       ///< its level-0/submesh-0 row: the override
            unsigned long long poseSerial = 0ull;
            bool skinned = false;             ///< the job has written it at least once
            /// The previous-pose slice differs from the posed vertices (the pose
            /// moved last time it was skinned): the first frame with no pose change
            /// copies it level (a SETTLE record, REFLECT-MOVERS-2).
            bool prevBehind = false;
            bool seen = false;                ///< in this pass's traced set
            /// The structure, built once from the cache with ALLOW_UPDATE and
            /// REFIT in place on every pose change after that. Its own scratch,
            /// sized for both, so a refit allocates nothing.
            VkAccelerationStructureKHR as = VK_NULL_HANDLE;
            RawBuffer storage, scratch;
            VkDeviceAddress address = 0;
            bool built = false;
            unsigned triangles = 0;
        };
        std::unordered_map<uint32_t, Skin> skins;
        /// The VaoManager frame on which a skin pass last MOVED a pose (a re-skin
        /// that was not the cache's first): the reflection's "a mover moved in the
        /// last two frames" counts a posed item's limbs too (REFLECT-MOVERS-2).
        /// THE WORLD's BOX, ONCE A FRAME (hitSetOf: the union of every hit-able item's world AABB, PHOTON-7 10-3 R4). Every
        /// ray job's hit set asks for it - four a frame - and the walk over every item each time was 2.7 ms of a 10k
        /// world's Debug frame (scale.cpu_walks): taken once per VaoManager frame, the jobs share it.
        mutable uint32_t worldBoxFrame = 0xFFFFFFFFu;
        mutable bool worldBoxOn = false;
        mutable float worldBoxMin[3] = { 0.0f, 0.0f, 0.0f }, worldBoxMax[3] = { 0.0f, 0.0f, 0.0f };
        uint32_t skinPosedFrame = 0xFFFFFFFFu;
        /// ...and the slots it posed on that frame (the reflection's per-texel rest break, PHOTON-2 round 2 G6).
        std::vector<uint32_t> skinPosedSlots;
        /// What the instance writer reads for a rigged slot: the skin BLAS and the
        /// row, for the entries READY this pass (built and skinned).
        std::unordered_map<uint32_t, std::pair<VkDeviceAddress, uint32_t>> skinUse;
        /// The job's two inputs, grown by doubling (Ogre UAV buffers: the job is an
        /// HlmsComputeJob) and the VaoManager that made them.
        Ogre::VaoManager *skinVao = nullptr;
        Ogre::UavBufferPacked *skinJobs = nullptr;
        Ogre::UavBufferPacked *skinPalette = nullptr;
        uint32_t skinJobCap = 0, skinPaletteCap = 0;

        /// The gate: nothing moved, no item changed and no instance's RAY LEVEL
        /// changed since the last update, so there is nothing to record. The
        /// caster walk's epoch plus the ray rule's refit count.
        unsigned long long lastEpoch = 0;
        bool haveEpoch = false;
        /// What the last top-level build was made from — the slot count and the
        /// job's inputs (every BLAS address, the ready rigged slots): a refit is
        /// legal only when neither moved and the feed saw no change of the set.
        uint32_t builtSlots = 0u;
        /// The TLAS was last BUILT with ALLOW_UPDATE (the refit arm on): only such a
        /// tree may be refit, and only with the same flags (the arm is per frame now,
        /// so a tree built with it off must be rebuilt when it turns on).
        bool tlasUpdatable = false;

        /// The GPU times (blasMs, tlasMs, skinMs, skinRefitMs) are NOT kept here:
        /// they are the monitor's CacheScope rows this scene filed (owner = this
        /// SceneAs), read back by `status` — -1 outside a capture (lane TEST-1).
        RayQueryStatus st;
    };

    /// THE SCENE'S STRUCTURES ARE THE SCENE'S (ENGINE-UPGRADE-2 stage 1): held by its
    /// GPU-scene substrate (GpuSceneSubstrate.h), so they live and die with it; this
    /// tier creates them on the scene's first ray frame and releases what they hold
    /// on the device (forgetScene, releaseScene). Null for a scene that has none yet.
    static SceneAs *asOf(const OgreScene *scene);
    static SceneAs &asFor(OgreScene *scene);
    friend struct RaySceneStructures;

    // --- plumbing ---------------------------------------------------------
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want) const;
    bool makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                    bool deviceAddress, RawBuffer &out, std::string &err);
    void dropBuffer(RawBuffer &b);
    VkDeviceAddress addressOf(VkBuffer b) const;
    bool makePipeline(std::string &err);

    bool ensureBlas(SceneAs &sa, const std::vector<BlasWant> &wants, VkCommandBuffer cmd,
                    unsigned &built, std::string &err);
    /// THE INSTANCES BY COMPUTE (ATOM-CPU-WALKS-1): the job's inputs, re-written
    /// only when they changed (`changed`), and the rigged slots' rows; then the
    /// rq_tlas_write.comp dispatch into `sa.tlasOut`, with the edges to the
    /// build that reads it. `skinReady` = rigged slots traced this frame.
    bool writeTlasInputs(OgreScene *scene, SceneAs &sa, unsigned &skinReady, bool &changed,
                         std::string &err);
    bool dispatchTlasWrite(OgreScene *scene, SceneAs &sa, VkCommandBuffer cmd, std::string &err);
    bool makeTlasWritePipeline(std::string &err);
public:
    /// engine.tlas_compute's door (a TOOL path: it submits and waits): the
    /// device-written instance array read back, and what a reference needs beside
    /// it — the structures' addresses, the ready rigged slots, the overlap, the
    /// counts, the rows (TlasReadback).
    bool readTlasBlocking(OgreScene *scene, TlasReadback &out, std::string &err);
private:
    /// True when it actually recorded a compaction copy this frame.
    bool runCompaction(SceneAs &sa, VkCommandBuffer cmd);
    bool buildTlas(SceneAs &sa, VkCommandBuffer cmd, bool refit, std::string &err);
    /// THE SKIN CACHE's frame (PHOTON-SKIN-1): reconciles the rigged traced set
    /// with `sa.skins`, re-skins every item whose POSE moved (one dispatch for all
    /// of them) and builds or refits their structures — all recorded into `cmd`
    /// before the gather that references them. The dispatch and the builds are
    /// the monitor rows "rq.skin.dispatch" and "rq.skin.build" (owner `&sa`).
    /// Returns false only when the frame must not trace skinned
    /// items at all (the job is missing); the entries simply stay unready then.
    void settleStillSkins(OgreScene *scene, SceneAs &sa);
    bool skinPass(OgreScene *scene, SceneAs &sa, VkCommandBuffer &cmd, std::string &err);
    /// Frees one entry: its structure retired, its buffer destroyed (Ogre's
    /// delayed destruction), its row block handed back and the node's override
    /// cleared. `scene` may be null (the tier's close: the GpuScene dies with it).
    void dropSkin(OgreScene *scene, SceneAs &sa, uint32_t node, SceneAs::Skin &sk);
    void dropSkinBuffers(SceneAs &sa);

    /// Ogre's frame command buffer, with every encoder closed first: an
    /// acceleration-structure build may not be recorded inside a render pass,
    /// and Ogre tracks the encoder state itself (VulkanQueue::getEncoderState).
    VkCommandBuffer frameCmd();
    /// EVERY COMPUTE PIPELINE THE TIER BUILDS GOES THROUGH THE DEVICE'S PIPELINE CACHE
    /// (architecture audit D9): Ogre's VkPipelineCache, which OgreShaderCache saves beside
    /// the shader cache and loads at boot — so a warm boot re-uses what a cold one
    /// compiled instead of compiling it again where the driver keeps no cache of its own.
    /// Counted and timed (the log's "rayquery: compute pipelines" line).
    VkResult createComputePipeline(const VkComputePipelineCreateInfo &cpi, VkPipeline *out, const char *name);
    unsigned mPipelinesBuilt = 0;
    double   mPipelineBuildMs = 0.0;
    /// THE HEAVY PIPELINES ARE BUILT AHEAD, ON WORKERS (the lead's ruling, 2026-10-27; samples.cold_open): the
    /// driver's compile of the ray pipelines was ~14 s of every cold boot, SERIAL on the render thread at each
    /// feature's first frame. open() starts the four whose setup is pure Vulkan of our own - the reflection's six
    /// (rq_reflect and its compact twin, ~8 s cold), the cards' gather and movers, the voxel sides - each its
    /// make*Pipeline on its own std::thread: our VkPipeline / layout / module / pool objects only (DOCS/traps/
    /// ENGINE.md, OGRE_THREAD_SUPPORT IS 0: no Ogre object is made or touched there - the shared samplers are made
    /// on the render thread before), the device's VkPipelineCache (internally synchronised: Ogre creates it without
    /// EXTERNALLY_SYNCHRONIZED). A worker LOGS NOTHING (Ogre's log is not thread-safe here): its builds' lines and
    /// the shader cache's dirt note are written by the render thread when it JOINS - at the first function that
    /// reads the setup's members (joinAhead), before any of them is read. The same create infos, the same cache
    /// key, the same pipelines: the picture and the shader cache are unchanged.
    enum AheadId { kAheadReflect, kAheadCardGather, kAheadCardMovers, kAheadVoxelSides, kAheadGlassEscape, kAheadCount };
    struct Ahead {
        std::thread th;
        bool started = false, joined = false, ok = false;
        std::string err;
        std::vector<RayAheadNote> notes;   // the worker's own, read after the join
    };
    Ahead mAhead[kAheadCount];
    void startAhead();
    void launchAhead(AheadId id, bool (RayQueryTier::*make)(std::string &));
    void joinAhead(AheadId id);
    /// make*Pipeline's front door: the ahead build's answer once joined, else the build now.
    template <class F> bool aheadOr(AheadId id, std::string &err, F now) {
        joinAhead(id);
        if (mAhead[id].started) {
            if (!mAhead[id].ok) err = mAhead[id].err;
            return mAhead[id].ok;
        }
        return now(err);
    }
public:
    /// THE MEASUREMENT ARMS (lane TEST-1): the engine's registry, latched each frame.
    void setArms(const ArmRegistry *arms) { mArms = arms; }
private:
    const ArmRegistry *mArms = nullptr;
    bool arm(ArmId a, bool otherwise) const { return mArms ? mArms->on(a) : otherwise; }
    double armValue(ArmId a) const { return mArms ? mArms->value(a) : 0.0; }
    /// THE RAY ORIGIN'S DRAWN TERM for a view's ray jobs (READ_B F3; Types.h rayOriginDrawnFootprints, the one law): the
    /// pixel tolerance x the LOD bias over the view's switch band, x the bake's drift ratio, in sample footprints -
    /// SCALED by the measuring arm rays.originCut (1 shipped; the origin rows' controls: 0 = the float bound alone,
    /// 0.5 = half the term).
    float drawnFootprintsOf(const detail::OgreScene *scene, float lodBand) const {
        const double k = mArms ? mArms->value(ArmId::RaysOriginCut) : 1.0;
        return float(k) * rayOriginDrawnFootprints(scene->pixelTolerance() * scene->lodBias(), lodBand);
    }
    /// THE DEPTH-REBUILT POINT'S TERM's knob for a view's ray jobs (jah_ray_origin.glsl jahDepthPointError): the
    /// device's sub-pixel snap in pixels, which also switches the term (the measuring arm rays.originPoint 0 sends 0:
    /// the float bound alone - the float-only control).
    float pointSnapKnob() const { return arm(ArmId::RaysOriginPoint, true) ? mSubPixelSnapPx : 0.0f; }
    /// subPixelPrecisionBits' snap (init; Vulkan's minimum is 4 bits, a 1/32-pixel worst case when unknown).
    float mSubPixelSnapPx = float(1.4142135623730951 / 32.0);
    /// THE GATHER'S REST KEY with the measuring arms "gi.hitDirect" and "gi.freeze" in it (FAR-SUN-2; PHOTON-7 stage
    /// 0's read W1): a paired A/B in one process flips the arm on a still view, and a held view traces nothing. Under the
    /// freeze no rest sample enters (W4u), so a view still past its rest length HOLDS on the frozen walk's EMA picture
    /// (measured on the court: held from the stop's 15th frame); lifting the arm must break that hold, or a lift with no
    /// GI work owed (a walk inside one lattice cell) leaves the view held on the walk's picture until something moves.
    unsigned long long gatherRestKeyArmed(const detail::OgreScene *scene) const {
        return scene->gatherRestKey() ^ (arm(ArmId::GiHitDirect, true) ? 0ull : 0x9E3779B97F4A7C15ull) ^
               (arm(ArmId::GiFreeze, false) ? 0xC2B2AE3D27D4EB4Full : 0ull);
    }
    /// THE REFLECTION'S REST KEY (PHOTON-2 fix F5; round 2, G6): what breaks a reflection's rest VIEW-WIDE - every
    /// lighting write and edit a reflection ray reads (PhotonComponent::reflectionReadsKey), the measuring arm
    /// "gi.hitDirect" (a paired A/B flips it on a still view), the material store's rows (a reflected object's albedo,
    /// a reflector's roughness), and a change of the TRACED SET: a slot born or dead (the slot count), a mesh acquired
    /// or released, a rigged item's structure made ready. NOT A MOVE: an instance that moves - a node, a drag, a pose -
    /// breaks the rest PER TEXEL (its swept box, rq_reflect.comp's restSegmentMoved), and the structure builds, refits
    /// and compactions that follow a move change nothing a ray finds (the read's #7: a moving node anywhere broke every
    /// view's rest, and every BLAS compaction once).
    unsigned long long reflectRestKey(const detail::OgreScene *scene, const SceneAs &sa) const {
        unsigned long long key = scene->photon().reflectionReadsKey();
        key ^= arm(ArmId::GiHitDirect, true) ? 0ull : 0x9E3779B97F4A7C15ull;
        key *= 1099511628211ull;
        const auto fold = [&key](unsigned long long v) {
            key ^= v;
            key *= 1099511628211ull;
        };
        const Ogre::PhotonVoxelMaterial *store = scene->photon().materials();
        fold(store ? store->getRowsSerial() : 0ull);
        const detail::GpuScene &gs = scene->gpuScene();
        fold(gs.slotCount());
        fold(gs.meshSetSerial());
        fold(sa.skinUse.size());
        // (NOT the emitter table's set serial, PHOTON-7 11-1a: every change of the SET is already here - an emissive
        // edit the material edits, a row the rows' serial, a slot born or dead the slot count. The serial alone moves
        // a frame later where a slot's material word arrives after its row, and a rest restarted there for a change
        // of the NEE's sampling distribution alone, which biases nothing: each sample of the rest is unbiased under
        // either table. Measured: the fold restarted the selftest's fixture B rest once, 65 -> 75 frames.)
        return key;
    }
    /// THE REFIT, off by default. A full TLAS rebuild is NVIDIA's own recommendation
    /// ("consider PREFER_FAST_TRACE and perform only rebuilds") and it measures
    /// 0.21-0.35 ms GPU at 8,001 instances on this rig — inside any budget — while
    /// making the better tree. The refit is the OPTIMISATION, kept behind the arm
    /// "rayquery.tlasRefit" so it can be measured against the rebuild rather than
    /// assumed better.
    bool preferRefit() const { return arm(ArmId::TlasRefit, false); }

    Ogre::VulkanRenderSystem *mRs = nullptr;
    Ogre::VulkanDevice *mDev = nullptr;
    VkDevice mVk = VK_NULL_HANDLE;
    RtFuncs mFn;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR mAsProps{};

    VkDescriptorSetLayout mSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mPipeLayout = VK_NULL_HANDLE;
    VkPipeline mPipeline = VK_NULL_HANDLE;
    VkShaderModule mModule = VK_NULL_HANDLE;
    VkDescriptorPool mDescPool = VK_NULL_HANDLE;

    VkQueryPool mCompactSizes = VK_NULL_HANDLE;
    /// WHICH COMPACTION SLOTS ARE OWED A READ — one bit per slot, not a count
    /// (round 3, finding 1). A count plus a cursor could not express "slot 3 is
    /// still pending": the wrap `if (cursor + n > ring) cursor = 0` was
    /// unconditional, so a PARKED scene's pending slots at the bottom of the
    /// ring — a preview panel that drew once and was hidden before its queries
    /// were read — were reset and rewritten by the editor's next batch, and
    /// `runCompaction` then read another BLAS's compacted size and copied into
    /// a buffer sized for a different structure. That is the round-2 corruption
    /// arriving by a second door. The mask answers exactly the question the
    /// allocation has to ask.
    uint64_t mCompactBusy = 0;
    /// A contiguous free run of `n` slots, or -1 when the ring cannot hold one.
    /// Contiguous because the pin's cmdWriteProperties takes ONE first-query
    /// index for the whole batch.
    int takeCompactRun(unsigned n) const;

    /// THE FRAME CLOCK IS OGRE'S, NOT OURS (finding 3). The counter this used to
    /// keep was incremented once per updateScene CALL, and updateRayQuery calls
    /// that once per DRAWN scene — so with the editor plus one preview scene it
    /// ran at twice the frame rate and every "wait N frames in flight" guard
    /// waited half as long as it claimed. A scratch arena could be freed while
    /// the build that reads it was still queued.
    ///
    /// VaoManager::getFrameCount() is the number the PIN retires its own
    /// resources on, and getDynamicBufferMultiplier() is the depth it considers
    /// in flight, so taking both means the tier and the pin agree by
    /// construction rather than by a matching constant.
    uint32_t frameNow() const;
    uint32_t framesInFlight() const;
    /// A structure or buffer that must not be freed until the frames that could
    /// still be reading it have retired.
    struct Retired {
        RawBuffer buf;
        VkAccelerationStructureKHR as = VK_NULL_HANDLE;
        /// R5's storage images take the same road: a resize frees them while the
        /// frames that dispatched against them are still in flight.
        ReflectImage img;
        /// ...and the per-frame UNCACHED image views of the chain's textures.
        VkImageView view = VK_NULL_HANDLE;
        /// ...and the reflection pass' descriptor sets: `vkFreeDescriptorSets`
        /// on a set a submitted command buffer still refers to is illegal, and
        /// `flushCommands` submits without waiting, so a workspace rebuild can
        /// reach here with the set still in flight (measured:
        /// VUID-vkFreeDescriptorSets-pDescriptorSets-00309).
        VkDescriptorSet set = VK_NULL_HANDLE;
        /// ...and the pool it came from. Freeing a set into another pool is
        /// invalid, and GATHER-0 brought a second one (audit: the old line
        /// hard-coded mReflectPool).
        VkDescriptorPool setPool = VK_NULL_HANDLE;
        /// GATHER-0: an Ogre-owned texture (the gather's full-resolution
        /// irradiance target) whose descriptor sets may still be in flight.
        Ogre::TextureGpu *tex = nullptr;
        uint32_t frame = 0;
    };
    std::vector<Retired> mRetireBin;
    void retire(RawBuffer &b);
    void retire(VkAccelerationStructureKHR &as, RawBuffer &storage);
    void retireImage(ReflectImage &img);
    void retireView(VkImageView v);
    void retireSet(VkDescriptorSet set);
    void retireSet(VkDescriptorSet set, VkDescriptorPool pool);
    /// GATHER-0: an Ogre texture destroyed once the frames that could still be
    /// sampling it have retired (the same bin as everything else here).
    void retireTexture(Ogre::TextureGpu *&t);
    /// THE BLACK STAND-INS. Every descriptor in a set must be a real view,
    /// whether the shader reads it or not, and two of this pass' inputs can
    /// legitimately be absent: a scene with no captured environment cube (no
    /// sky, no IBL) and a scene whose voxel arm is not built. Rather than
    /// decline the whole trace there — which would mean no reflections at all in
    /// exactly the scene where an escaping ray is the WHOLE answer — the empty
    /// slots take a 1x1 black image of the right TYPE. They are made once,
    /// cleared once, and left in SHADER_READ_ONLY_OPTIMAL for the process' life.
    bool ensureDummyImages(std::string &err);
    void clearDummyImages(VkCommandBuffer cmd);
    /// ...and the card read's (PHOTON-CARDS-2): a scene without a surface cache
    /// binds a 1x1 black 2D image for the two atlas layers and a 256-byte
    /// storage buffer for the two tables, with ZERO instance slots in the
    /// parameters, so the read declines every hit without touching them.
    ReflectImage mDummyCube, mDummyVolume, mDummyFlat;
    /// ...and the reflection's ID IMAGE stand-in (REFLECT-MOVERS-1): a 1x1
    /// R32G32_UINT cleared to "nothing drew here", bound where the view's chain
    /// carries no id pass (the shader reads it only under `motion.x`).
    ReflectImage mDummyIds;
    RawBuffer mDummyStorage;
    bool mDummiesReady = false;
    bool mDummiesNeedClear = false;
    /// `layers` 0 = a plain 2D image and view (the hit list's stand-ins); n >= 1 =
    /// a SCREEN image of n layers (one per eye, LAYERED-STEREO-1) and a 2D_ARRAY
    /// view of all of them, which is what every screen shader declares.
    bool makeStorageImage(unsigned w, unsigned h, VkFormat fmt, ReflectImage &out,
                          std::string &err, unsigned layers = 0u);
    /// A 2D_ARRAY VIEW OF EVERY LAYER of an Ogre screen texture (LAYERED-STEREO-1):
    /// a mono view's texture is a 2D image and gets a one-layer array view, a
    /// stereo view's is a two-layer array — so every screen shader of the tier
    /// declares arrays, the eye being the layer. Uncached and retired, for the
    /// reason sampledView's note gives (a cached view of a texture recreated at the
    /// same address is a view of a dead image).
    VkImageView layerView(Ogre::TextureGpu *t, bool storage);
    void drainRetired();
    void evictStaleBlas(SceneAs &sa);


    // ---- R5: RAY-TRACED REFLECTIONS ------------------------------------
    // (PHOTON_SPEC §7 R5.) One compute dispatch per drawn view per frame,
    // recorded into the same frame command buffer as everything else here,
    // between the SSR resolve and the scene pass that samples its output. The
    // structures it traces are the ones above; it adds no build of its own, so
    // the no-hitch law is satisfied by construction.
public:
    /// Records this frame's trace for one view. Silently does nothing when the
    /// scene has no usable structure, when the chain does not carry the SSR
    /// textures, or when the pipeline could not be made — in every one of those
    /// cases `jahSsrReflection` keeps exactly what the resolve wrote, which is
    /// exactly today's picture.
    void recordReflect(const ReflectPassListener *key, OgreView *view,
                       Ogre::CompositorPass *pass, const HitListBinding &hit);
    /// THE REFLECTION'S SECOND HALF (PHOTON-HIT-SHADE-1): the spatial filter and
    /// the composite, in front of the opaque pass — after the hit write-back has
    /// completed the temporal mean of every texel whose ray the decode shaded.
    void finishReflect(const ReflectPassListener *key);
    /// THE SCREEN SOURCES' OBJECT MOTION (REFLECT-MOVERS-1): writes `jahSsrVelocity`
    /// at every pixel of a slot that moved this frame (rq_motion.comp), in front of
    /// the trace whose screen hits read it (traceRayJobs, PHOTON-7 stage 6). Does
    /// nothing — the chain's clear stands, the camera path — when the chain carries
    /// no id pass or the switch is off.
    void recordMotion(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass);
    /// Frees a view's reflection resources. Called from ~ReflectPassListener.
    void forgetReflect(const ReflectPassListener *key);
    /// How many rays the last recorded trace dispatched, and the GPU
    /// milliseconds it cost (read back several frames later, -1 until measured)
    /// — for GiStatus::rayQuery. Per SCENE, because that is what giStatus asks.
    void reflectStatsInto(const OgreScene *scene, RayQueryStatus &st) const;

private:
    /// ONE EYE'S IMAGE as the shader's five vec4s (rq_reflect.comp's EyeImage):
    /// where the camera is (w = 1 perspective, 0 orthographic) and the world
    /// basis of its 0..1 image. A MONO view has one; a stereo view has two, each
    /// spanning its own HALF of the target.
    /// The camera's image (detail::CameraImageF: one text with the no-ray screen integrate's).
    using EyeBasisF = CameraImageF;

    struct ReflectView {
        /// THE SCREEN SOURCES' OBJECT MOTION (REFLECT-MOVERS-1, rq_motion.comp): its
        /// own ring of sets and parameter buffers, recorded in front of the trace.
        VkDescriptorSet motionSets[3] = {};
        RawBuffer       motionParams[3];
        /// The per-slot geometry rows the POSED motion job reads (SKINNED-VELOCITY-1),
        /// a copy per ring slot like the trace's own.
        RawBuffer       motionGeomRowOfSlot[3];
        unsigned        motionFrame = 0;
        /// THE SOURCES THIS VIEW LAST TRACED WITH (RayQueryStatus::reflectSources).
        int             sources = -1;
        /// Its GPU time is the monitor row "rq.reflect.motion" (owner = the key).
        /// Frames since a MOVER slot (kGpuMover | kGpuDragMover) last moved in this
        /// view's scene — the trace's departure rule is live only just after one did.
        unsigned        framesSinceMoverMoved = 1000u;
        /// ...and since a POSE moved (SKINNED-VELOCITY-1): motion.w = 3 while it is
        /// 2 or less — the trace then identifies the pixels the id pass did not draw.
        unsigned        framesSincePoseMoved = 1000u;
        /// The descriptor ring. A set that is bound by a command buffer still in
        /// flight may not be rewritten, and every input of this pass can be
        /// recreated behind our back (a workspace rebuild replaces every texture
        /// in the chain; a GI re-solve replaces every voxel texture), so the set
        /// is rewritten EVERY frame and there is one per frame in flight.
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        /// THE PER-SLOT GEOMETRY ROW the scene's TLAS was written with, copied
        /// per frame in flight (the scene's vector moves under a later frame).
        RawBuffer       geomRowOfSlot[kReflectRing];
        /// The temporal mean and the STATE TEXEL beside it (rg32ui, jah_reflect_state.glsl: the distances, the counts,
        /// the rest's stop, this frame's estimator), ping-ponged: read from [frame & 1], written to [~frame & 1]. See
        /// rq_reflect.comp's note on why one buffer is a race.
        ReflectImage hist[2], state[2];
        /// THIS FRAME'S RAW SAMPLE (PHOTON-2 1b): one image (not a pair — written and read within the frame), the
        /// temporal pass' neighbourhood and blend input. 8 bytes a trace texel.
        ReflectImage sample;
        /// ...and ITS NEE PART (PHOTON-7 11-1b, binding 55): the same size and format; the resolve's.
        ReflectImage nee;
        /// THE REST'S REFERENCE (R1 (f)'s arm "reflect.restReference"): rgba32f, the texel's raw rest samples' mean and
        /// count; the trace's size under the arm, 1 x 1 otherwise (`referenceFull`).
        ReflectImage reference;
        /// THE REST'S STOP SHARE (binding 51): r32ui a trace texel, a float's bits.
        ReflectImage share;
        /// THE REST'S COMPACT LIST (binding 52, kReflectCompactBinding): [0..1] the halves' counts, then the halves; the
        /// half this frame lists into flips each traced frame.
        RawBuffer compactList;
        unsigned compactHalf = 0u;
        bool compactListFresh = false;   ///< made this frame: both counts zeroed before its first pass
        bool referenceFull = false;
        unsigned w = 0, h = 0, layers = 1;
        unsigned frame = 0;              ///< the sample sequence's input
        bool     imagesReady = false;
        /// The pair has been made but not yet zeroed (the clear needs a command
        /// buffer, which is taken later than the creation).
        bool     needsClear = false;
        /// The PREVIOUS frame's camera basis, in the five numbers the shader
        /// reconstructs with — ONE PER EYE since lane REFLECT-VR-1 (index 0 is
        /// the only one a mono view uses, and is that view's camera).
        /// `havePrev` false means "no history is valid", which is what a first
        /// frame, a resize and a scene change all are.
        EyeBasisF prev[2];
        bool  havePrev = false;
        /// HOW MANY CONSECUTIVE FRAMES THIS HISTORY HAS BEEN WRITTEN — the
        /// VIEW's age, which is a different fact from a texel's sample count
        /// (PAN-SMEAR-1). A pixel the camera has just turned onto has no
        /// previous position on screen, and the shader must tell that apart
        /// from a view that is two frames old: the second is what the filter's
        /// warm-up exists for, the first is an ordinary frame of an ordinary
        /// pan. Restarts with `havePrev`.
        unsigned historyFrames = 0;
        /// Was the LAST recorded frame a stereo one? A view that changes shape
        /// (a session beginning or ending on it) has a history whose halves mean
        /// something else, so the mean starts again — the same rule as a resize.
        bool  prevStereo = false;
        /// What the last recorded dispatch covered, for the status readings.
        OgreScene *scene = nullptr;
        unsigned   rays = 0;
        /// THE REFLECTION'S GPU TIME is two monitor rows, the trace's
        /// ("rq.reflect.trace") and the filter's ("rq.reflect.filter"), owner =
        /// the key; RayQueryStatus::reflectMs is their sum. NOT one span from the
        /// trace to the filter, which the private ring was: the halves run in
        /// two different compositor passes' callbacks, and a sample opened in
        /// one pass and closed in another breaks the render system's sample
        /// STACK (the enclosing pass's end pops it — measured: the glossy arm
        /// read 1.03 ms for a span the ring read at 1.61, the filter left out).
        /// The hit decode between the halves is its own pass row.
        /// THE FRAME BETWEEN ITS TWO HALVES (trace -> finishReflect): the ring slot
        /// the trace bound, its grid, and which of the pair is this frame's mean.
        bool     finishPending = false;
        unsigned finishRing = 0, traceW = 0, traceH = 0, curIdx = 0, prevIdx = 1;   ///< (the trace's halves this frame)
        /// ...and the full-resolution picture the spatial filter writes every pixel of (PHOTON-7 stage 6 commit 7).
        unsigned fullW = 0, fullH = 0;
        /// THE REST (PHOTON-2 1e, fix F1; round 2: R1, G1-G6): consecutive recorded frames with the eye basis
        /// bit-identical, the reflection's rest key unchanged (reflectRestKey: the lighting writes and edits its rays
        /// read and the traced set - never a move) and the pass's own parameters unchanged (the NAMED key, commit 8). At
        /// rest each texel's frozen moving history and its own rest mean live in the two halves, composed at every read
        /// (jah_reflect_rest.glsl: the hand-over over 2N samples, then the mean); a texel traces the tier's rest count of
        /// rays a frame and STOPS once its rest mean's standard error has stayed under a code for kReflectStopFrames
        /// frames, and the view HOLDS - the trace and the temporal pass dispatch nothing, the filter composites - once a
        /// rest frame's count of active texels read back zero (no frame count holds it: K_max deleted, commit 8). A
        /// MOVED INSTANCE breaks the rest per
        /// texel (its swept box: the texels its ray segments cross resume); a BREAK of the key composes every texel's
        /// answer into the half the trace reads (rq_reflect_compose.comp) and the moving rules resume from it.
        unsigned restFrames = 0u;
        unsigned long long restKey = 0ull, restParams = 0ull;
        bool restKnown = false;
        /// The rest's serial (one a rest begun), the rest frame a moved instance last crossed it (0: none), and the
        /// latch "a rest frame since then counted no active texel" (the hit list's third counter word, read late).
        unsigned restSession = 0u, restMoverFrame = 0u;
        /// THE REST'S RAY COUNT (kReflectRestRaysCap's note): k x the texels batching at the rest's first batching
        /// frame, from its read-back (0 until it arrives: the first frames run k a texel).
        unsigned long long restRayCount = 0ull;
        bool restAllStopped = false;
        /// The view CAN rest (not stereo, not the photon overlay's: its still predicate's standing half), at its last
        /// record - the cards' rest decision waits only on a view that can (cardReadersRestComplete).
        bool restableLast = false;
        /// ...and the frame it last recorded (frameNow), and its scene's draw count then (OgreScene::drawSerial): a view
        /// that stopped recording while its scene is drawn makes no rest demand (ScreenProbeGather::kReaderStaleFrames,
        /// timed on the draws).
        uint32_t recordedFrame = 0u;
        uint32_t recordedDraw = 0u;
        unsigned restMoverCrossings = 0u;   ///< rest frames a moved instance's box was handed to the trace (status)
        /// THE REST's N this view last recorded with (reflectRestLength): its hand-over is 2N rest samples
        /// (jahRestHandOverLength), and past 2N rest frames every texel's composite is its rest mean (PHOTON-7 stage
        /// 10-3, carrier 2: GiStatus::giAtRest waits for it)
        unsigned restN = 0u;
        unsigned cameraCuts = 0u;           ///< camera cuts detected (N3: each one the histories cleared)
        /// This frame is a held one (finishReflect skips the temporal pass).
        bool held = false;
        /// The descriptor and parameter ring's own counter: every RECORDED frame takes the next slot (a held frame
        /// traces nothing, so `frame` — the traced frames and the moving sample sequence — stands).
        unsigned ringFrame = 0u;
        /// THE PING-PONG'S PARITY (fix F1): `flip & 1` is the half last written. It advances on every traced frame
        /// but the rest's from k = 2 — there the frozen moving history stays in the half the trace reads and the
        /// handed-over answer is written in place — and a held frame binds the same pair.
        unsigned flip = 0u;
        /// THE HISTORY-COUNT READBACK (PHOTON-2 stage 0, the arm "reflect.historyReadback"): a host ring of
        /// kReflectRing copies of the state texel the filter has read, the slot of the frame's ring; each
        /// decoded once its frame retired into `histHost` (the count: N + v) for RayQueryStatus.
        RawBuffer histReadback;
        struct HistPending {
            uint32_t frame = 0u;          ///< frameNow() at the copy
            bool     live = false;
            unsigned w = 0u, h = 0u, layers = 1u;
            unsigned traced = 0u;         ///< which traced frame (the view's `frame` counter) it copied
            bool     se = false;          ///< the moving SE instrument was copied beside it
        };
        HistPending histPending[kReflectRing];
        std::vector<unsigned short> histHost;
        unsigned histHostW = 0u, histHostH = 0u, histHostFrame = 0u;
        /// THE MOVING SE INSTRUMENT (binding 53): the temporal pass' buffer (the arm's size, else 16 bytes), its host
        /// ring beside histReadback (a slot a frame, the state's slot) and the decoded copy (the state's grid).
        RawBuffer movingSe;
        RawBuffer seReadback;
        std::vector<float> histHostSe;
        /// THE PICK (binding 56, the arm reflect.pick): the ring the passes write the picked texel into
        RawBuffer pick;
        uint32_t pickFrames[64] = {};   ///< each slot's dispatch frame (the lab orders the ring by it)
    };

    bool makeReflectPipeline(std::string &err) {   // (the ahead build's answer, else the build now: aheadOr)
        return aheadOr(kAheadReflect, err, [this](std::string &e) { return makeReflectPipelineNow(e); });
    }
    bool makeReflectPipelineNow(std::string &err);
    /// The point and linear samplers every job of the tier binds (made once).
    bool ensureSamplers(std::string &err);
    bool ensureReflectImages(ReflectView &rv, unsigned w, unsigned h, unsigned layers, bool reference,
                             std::string &err);
    /// Records the UNDEFINED -> GENERAL transition and the zero clear for a pair
    /// that was just made. Separate from the creation because the descriptor set
    /// needs the image VIEWS before the command buffer may be taken (see the
    /// note on the transitions in recordReflect).
    void clearReflectImages(ReflectView &rv, VkCommandBuffer cmd);
    void dropReflect(ReflectView &rv);
    /// The history-count readback (PHOTON-2 stage 0): decode the retired copies, copy this frame's.
    void readReflectHistory(ReflectView &rv, VkCommandBuffer cmd);

    std::unordered_map<const ReflectPassListener *, ReflectView> mReflects;
    /// THE SHOT-VIEW FOLD (PHOTON-7 stage 10-3): a screenshot's shot view is destroyed right after its readback, and
    /// with it its reflection's rest state - the rest frames, the held flag and the rest's serial it photographed.
    /// forgetReflect keeps them here, per scene; reflectStatsInto reports them as THE LAST SHOT's
    /// (RayQueryStatus::reflectShot*, never merged into the live views'), and the entry dies with the scene (forgetScene:
    /// no new scene reads a dead one's at its address).
    struct ForgottenReflect {
        unsigned restFrames = 0u, restSession = 0u;
        bool held = false;
    };
    std::unordered_map<const OgreScene *, ForgottenReflect> mForgottenReflects;
    bool makeMotionPipeline(std::string &err);
    VkDescriptorSetLayout mMotionSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mMotionPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mMotionPipeline = VK_NULL_HANDLE;
    VkShaderModule        mMotionModule = VK_NULL_HANDLE;
    /// THE POSED TWIN (rq_motion_skin.comp, SKINNED-VELOCITY-1): the same layout and
    /// set; null when it could not be made (the rigid job runs alone).
    VkPipeline            mMotionSkinPipeline = VK_NULL_HANDLE;
    VkShaderModule        mMotionSkinModule = VK_NULL_HANDLE;
    VkDescriptorPool      mMotionPool = VK_NULL_HANDLE;
    bool                  mMotionFailed = false;
    /// Rebuilds `sa`'s alpha table from the feed's alpha-tested slots (the masks: the GPU scene's table).
    void updateAlphaTable(OgreScene *scene, SceneAs &sa);
    /// Rebuilds `sa`'s hit table (FAR-SUN-2) from the scene's lights and material rows.
    void updateEmitterTable(OgreScene *scene, SceneAs &sa);
    void updatePortals(OgreScene *scene, SceneAs &sa);
    void updateHitTable(OgreScene *scene, SceneAs &sa);
public:
    /// The table's address a job hands its shader (0: none - the hit reads the store whole).
    uint64_t hitTableOf(const OgreScene *scene) const;
    /// THE VOXELISER'S TEXTURE POOL (the albedo and emissive maps a material row samples) and
    /// THE CLOUD LAYER'S SHADOW (field + its two parameter vectors) for the ray jobs' hit.
    struct HitSurfaceInputs {
        Ogre::TextureGpu *pool = nullptr;
        Ogre::TextureGpu *cloudField = nullptr;
        float cloudMap[4] = {};   // 1 / tile, strength, scroll x, scroll z
        float cloudSun[4] = {};   // sunThrow x, z, altitude, 1 / mu_s
    };
    HitSurfaceInputs hitSurfaceInputsOf(const OgreScene *scene) const;
    /// THE FORWARD+ LAMPS' SHADOW RAYS (PHOTON-7 10-4 R9): the scene's TLAS by its device address (0: none built) - the
    /// colour passes' fragments trace it (JahLampRay_piece_ps.any), so its build's barrier waits for and is waited on by
    /// the FRAGMENT stage too (buildTlas).
    uint64_t tlasAddressOf(const OgreScene *scene) const;
    /// The table's address a job hands its shader (0: none).
    uint64_t alphaTableOf(const OgreScene *scene) const;
private:
    /// THE HIT SET (ScreenProbeGather.h HitSetSources; PHOTON-7 stage 10, 10-2a): the ONE collection of everything a
    /// job's hit read binds - the TLAS, the cascades (innermost first, by name), the one environment, the surface
    /// cache, the GPU scene's rows (flushed here), the hit table and surface, the alpha table. The
    /// hit list is the job's own (`out.hit`, left as it is).
    void hitSetOf(OgreScene *scene, const SceneAs &sa, HitSetSources &out);
    VkDescriptorSetLayout mReflectSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mReflectPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mReflectPipeline = VK_NULL_HANDLE;
    VkPipeline            mReflectCompactPipeline = VK_NULL_HANDLE;   ///< the same module, kCompactPass true
    unsigned              mSubgroupSize = 32u;   ///< the device's compute subgroup width (the compact pass's warps)
    VkShaderModule        mReflectModule = VK_NULL_HANDLE;
    /// THE SPATIAL FILTER + COMPOSITE (round C). A second pipeline on the
    /// SAME layout and the same descriptor set: it reads the temporal mean
    /// the first dispatch wrote, blurs it by the pixel's own lobe (guided by
    /// depth and normal) and writes `jahSsrReflection`. A pixel cannot blur
    /// with neighbours its own dispatch has not finished writing, which is why
    /// it is a second dispatch and not more code in the first.
    VkPipeline            mFilterPipeline = VK_NULL_HANDLE;
    VkShaderModule        mFilterModule = VK_NULL_HANDLE;
    /// THE TEMPORAL PASS (PHOTON-2 1b, rq_reflect_temporal.comp): between the trace's write-back and the filter, on
    /// the same layout and set — the history clamped to this frame's samples, the sample blended in.
    VkPipeline            mTemporalPipeline = VK_NULL_HANDLE;
    VkShaderModule        mTemporalModule = VK_NULL_HANDLE;
    /// THE RESOLVE (PHOTON-7 stage 6 commit 6, rq_reflect_resolve.comp): between the write-back and the temporal pass,
    /// each texel's answer reconstructed from its neighbours' samples - same layout and set.
    VkPipeline            mResolvePipeline = VK_NULL_HANDLE;
    VkShaderModule        mResolveModule = VK_NULL_HANDLE;
    /// THE BREAK'S COMPOSE (PHOTON-2 round 2, G2; rq_reflect_compose.comp): on the frame a rest breaks, before the
    /// trace, every texel's composed answer into the half the trace reads - same layout and set.
    VkPipeline            mComposePipeline = VK_NULL_HANDLE;
    VkShaderModule        mComposeModule = VK_NULL_HANDLE;
    VkDescriptorPool      mReflectPool = VK_NULL_HANDLE;
    /// gi.card_read_parity's harness (cardPickBlocking), made on first use.
    VkDescriptorSetLayout mCardParitySetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCardParityPipeLayout = VK_NULL_HANDLE;
    VkShaderModule        mCardParityModule = VK_NULL_HANDLE;
    VkPipeline            mCardParityPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      mCardParityPool = VK_NULL_HANDLE;
    /// Its own point sampler: the reflection's are made with the reflection
    /// pipeline, which a scene without an SSR chain never builds.
    VkSampler             mCardParitySampler = VK_NULL_HANDLE;
    VkSampler             mPointSampler = VK_NULL_HANDLE;
    VkSampler             mLinearSampler = VK_NULL_HANDLE;
    /// FAR-SUN-2: trilinear and WRAPPED - the material maps a hit samples at its own UV (the
    /// voxeliser's pool) and the cloud field (which tiles).
    VkSampler             mWrapSampler = VK_NULL_HANDLE;
    /// FAR-SUN-2: the flat stand-in seen as a one-layer 2D ARRAY (the texture pool's binding).
    VkImageView           mDummyFlatArrayView = VK_NULL_HANDLE;
    /// The pipeline could not be made on this device; say so ONCE and take the
    /// fallback picture for the rest of the process.
    bool                  mReflectFailed = false;

    // ---- THE SCREEN-PROBE GATHER (GATHER-1a) ---------------------------
    // Three more compute dispatches on the SAME frame command buffer and the
    // SAME hook as the reflection trace — after the SSR prepass, before the
    // opaque pass that shades. The ALGORITHM is a Component of ours
    // (`ScreenProbeGather`, OgreScreenProbeGather.cpp); what lives here is
    // what only this file can do: read the scene it is friends with, and lend
    // the Component the device services below.
public:
    /// Records this frame's placement, trace and integrate for one view.
    /// Silently does nothing unless the view's scene has the row on and this
    /// machine traces.
    void recordGather(const ReflectPassListener *key, OgreView *view,
                      Ogre::CompositorPass *pass, const HitListBinding &hit);
    /// Frees a view's gather resources (from ~ReflectPassListener, and when
    /// the row goes off).
    void forgetGather(const ReflectPassListener *key);
    /// Takes this listener's shader registration away the moment its pass
    /// ends — the gather's binding is PASS-scoped (GATHER-0's D2).
    void releaseGatherBinding(const ReflectPassListener *key);
    /// The view's measured sky visibility (PHOTON-7 10-4 R11: the blended pass's fog), or null.
    Ogre::TextureGpu *gatherSkyVis(const ReflectPassListener *key) const { return mGather ? mGather->skyVisOf(key) : nullptr; }
    /// The last measured numbers for a scene, for `GiStatus::gather`.
    void gatherStatsInto(const OgreScene *scene, GatherStatus &out) const;
    bool cardReadersRestComplete(const OgreScene *scene, unsigned restFrames[2], bool active[2]) const;
    /// THE REFLECTION's REST PAST ITS HAND-OVER (PHOTON-7 stage 10-3, carrier 2; GiStatus::giAtRest): every reflection
    /// view of the scene that records and can rest has held still for at least its hand-over (2N rest frames) since its
    /// last restart - every texel's composite is then its rest mean, a function of the pose and the rest index alone. A
    /// view that no longer records makes no demand (ScreenProbeGather::kReaderStaleFrames), nor one that cannot rest.
    bool reflectRestPastHandOver(const OgreScene *scene) const;
    /// ...and the gather's (N rest frames: every view that drew the scene's latest frame and can rest, or none).
    bool gatherRestPastHandOver(const OgreScene *scene) const;
    /// The readers' demand span L = N + the due, as cardReadersRestComplete last derived it (0: none yet - stamp always):
    /// a reader stamps its rest indices [1, L] alone (JAH_HIT_PARAMS.cards.z).
    mutable unsigned mDemandSpan = 0u;

    // ---- GatherHost: the services the Component borrows -----------------
    VkDevice gatherDevice() const override { return mVk; }
    Ogre::RenderSystem *gatherRenderSystem() const override { return mRs; }
    uint32_t gatherFrameNow() const override { return frameNow(); }
    uint32_t gatherFramesInFlight() const override { return framesInFlight(); }
    bool gatherTemporalArm() const override { return arm(ArmId::GatherTemporal, true); }
    bool gatherDecodeHitsArm() const override { return arm(ArmId::GatherDecodeHits, false); }
    VkCommandBuffer gatherFrameCmd() override { return frameCmd(); }
    bool gatherMakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                          VkBuffer &buffer, VkDeviceMemory &memory, void **mapped,
                          std::string &err) override {
        RawBuffer b;
        if (!makeBuffer(size, usage, hostVisible, false, b, err)) return false;
        buffer = b.buffer;
        memory = b.memory;
        if (mapped) *mapped = b.mapped;
        return true;
    }
    bool gatherMakeImage(unsigned w, unsigned h, unsigned layers, VkFormat fmt, VkImage &image,
                         VkDeviceMemory &memory, VkImageView &view, std::string &err) override {
        ReflectImage img;
        if (!makeStorageImage(w, h, fmt, img, err, layers)) return false;
        image = img.image;
        memory = img.memory;
        view = img.view;
        return true;
    }
    void gatherRetireBuffer(VkBuffer buffer, VkDeviceMemory memory) override {
        RawBuffer b;
        b.buffer = buffer;
        b.memory = memory;
        retire(b);
    }
    void gatherRetireImage(VkImage image, VkDeviceMemory memory, VkImageView view) override {
        ReflectImage img;
        img.image = image;
        img.memory = memory;
        img.view = view;
        retireImage(img);
    }
    void gatherRetireView(VkImageView view) override { retireView(view); }
    VkImageView gatherLayerView(Ogre::TextureGpu *t, bool storage) override {
        return layerView(t, storage);
    }
    void gatherRetireSet(VkDescriptorSet set, VkDescriptorPool pool) override {
        retireSet(set, pool);
    }
    void gatherRetireTexture(Ogre::TextureGpu *texture) override { retireTexture(texture); }
    bool gatherDummies(VkImageView &cube, VkImageView &volume, VkImageView &flat, VkBuffer &storage,
                       std::string &err) override {
        if (!ensureDummyImages(err) || !ensureSamplers(err)) return false;
        cube = mDummyCube.view;
        volume = mDummyVolume.view;
        flat = mDummyFlat.view;
        storage = mDummyStorage.buffer;
        return cube && volume && flat && storage;
    }
    /// ...AND THE STAND-INS MUST BE CLEARED BY WHOEVER BINDS THEM FIRST (the
    /// lead's read). `clearDummyImages` used to have ONE caller — the reflection
    /// trace — which returns early when the SSR row is off, so a GATHER-ONLY
    /// chain bound a 1x1 cube and volume that had never left UNDEFINED as
    /// SHADER_READ_ONLY. `gi.gather_reference` runs exactly that configuration
    /// (`ssr = 0`). It is a no-op after the first frame that calls it, and it is
    /// safe to call twice in one frame — the flag is cleared by the first.
    void gatherClearDummies(VkCommandBuffer cmd) override { clearDummyImages(cmd); }
    VkSampler gatherPointSampler() const override { return mPointSampler; }
    VkResult gatherCreatePipeline(const VkComputePipelineCreateInfo &cpi, VkPipeline *out,
                                  const char *name) override {
        return createComputePipeline(cpi, out, name);
    }
    VkSampler gatherLinearSampler() const override { return mLinearSampler; }
    VkSampler gatherWrapSampler() const override { return mWrapSampler; }
    VkImageView gatherDummyFlatArray() const override { return mDummyFlatArrayView; }
    VkImageView gatherDummyUint() const override { return mDummyIds.view; }

private:
    /// Made on the first frame a scene gathers, destroyed by close().
    ScreenProbeGather *mGather = nullptr;

    // ---- HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1, RY-R3) ----------------
    // One more compute dispatch on the same frame command buffer and the same
    // hook as the reflection trace and the gather — after the prepass, before
    // the PrePassUse pass that shades with its answer. One ray per texel from
    // the prepass' surface towards the sun (rq_sun_contact.comp); the answer is
    // an Ogre R8 texture the PBS pass reads through the Hlms listener's fourth
    // extra slot (OgreFog.cpp kExtraPassSlots, JahSunContact_piece_ps.any).
public:
    /// Records this frame's contact rays for one view, and registers the
    /// texture with the listener for exactly the pass it runs in front of.
    /// Silently does nothing unless the view's scene resolves the row on.
    void recordSunContact(const ReflectPassListener *key, OgreView *view,
                          Ogre::CompositorPass *pass);
    /// ...and its second half (PHOTON-HIT-SHADE-1): the texture's transition to
    /// the pixel stage and the pass-scoped registration, in front of the opaque
    /// pass that reads it (the rays themselves are traced in front of the hit
    /// decode pass, with the other ray jobs).
    void finishSunContact(const ReflectPassListener *key);
    /// Takes the pass-scoped registration away as that pass ends.
    void releaseSunContactBinding(const ReflectPassListener *key);
    /// Frees a view's contact state (from ~ReflectPassListener, a workspace
    /// rebuild, and the row going off).
    void forgetSunContact(const ReflectPassListener *key);
    /// The last frame's numbers for a scene (`Scene::sunContactStatus`).
    void sunContactStatsInto(const OgreScene *scene, SunContactStatus &st) const;

private:
    struct SunContactView {
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        /// The visibility texture the pixel reads (an Ogre texture: it reaches
        /// the PBS pass through the listener, so it must be one the Hlms can
        /// bind), at the job's own resolution.
        Ogre::TextureGpu *vis = nullptr;
        unsigned w = 0, h = 0, fullW = 0, fullH = 0, divisor = 1;
        unsigned frame = 0;
        const Ogre::SceneManager *sceneMgr = nullptr;
        /// What the last record did, for the status.
        OgreScene *scene = nullptr;
        bool      ran = false;
        unsigned long long rays = 0;
        float     range = 0.0f;
        float     toSun[3] = { 0.0f, 0.0f, 0.0f };
        float     cpuMs = -1.0f;
        std::string reason;
        /// Its GPU time is the monitor row "sun.contact" (owner = the key).
        /// The rays were traced this frame; finishSunContact registers them.
        bool     finishPending = false;
        /// finishSunContact registered them and the pass has not ended yet.
        bool     bound = false;
        /// The frame those rays were registered in (a capture re-registers them).
        uint32_t boundFrame = 0xFFFFFFFFu;
    };
    bool makeSunContactPipeline(std::string &err);
    void dropSunContact(SunContactView &sv);
    std::unordered_map<const ReflectPassListener *, SunContactView> mSunContacts;

    // ---- THE PIXEL'S AREA LIGHT VISIBILITY (PHOTON-7 stage 7, 7-A) ---------
    // The sun contact's shape, twice over: in front of the hit decode, the TRACE (rq_area_vis.comp: one stratum of
    // each of the pass's forward LTC lights a half-resolution texel a frame, the ratio's two sums averaged over time)
    // and the RESOLVE (rq_area_vis_resolve.comp: the joint-bilateral upsample and the ratio) into a full-resolution
    // RG32_UINT Ogre texture `jahAreaVis` (x the diffuse ratio of light i in byte i, y the specular's), which
    // finishAreaVis registers for the opaque pass (OgreFog.cpp's slot,
    // JahAreaVis_piece_ps.any in the fork's LTC hook). Allocated only while an LTC light is in the forward set.
public:
    void recordAreaVis(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass);
    void finishAreaVis(const ReflectPassListener *key);
    void releaseAreaVisBinding(const ReflectPassListener *key);
    void forgetAreaVis(const ReflectPassListener *key);
    void areaVisStatsInto(const OgreScene *scene, AreaVisStatus &st) const;
private:
    static constexpr unsigned kAreaVisReadbacks = 4u;   // > kFramesInFlight: the slot read is retired
    struct AreaVisView {
        VkDescriptorSet traceSets[kReflectRing] = {};
        VkDescriptorSet resolveSets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        /// The two ratio sets' sums (diffuse S U, specular S2 U2): their history pairs, the surface words' pair, the
        /// rest means - raw images at the trace's size.
        ReflectImage histS[2], histU[2], histS2[2], histU2[2], histG[2], restS, restU, restS2, restU2;
        bool imagesNeedClear = false;
        /// The product the pixel reads (an Ogre texture: it reaches HlmsPbs through the listener).
        Ogre::TextureGpu *vis = nullptr;
        unsigned w = 0, h = 0, fullW = 0, fullH = 0, divisor = 2;
        /// The history's age and ping-pong, the rest, the previous camera and the light set (both producers' one
        /// state: EnginePrivate.h's AreaVisState).
        AreaVisState st;
        const Ogre::SceneManager *sceneMgr = nullptr;
        OgreScene *scene = nullptr;
        bool ran = false, finishPending = false, bound = false;
        uint32_t boundFrame = 0xFFFFFFFFu;
        /// The scene draw it last ran at (OgreScene::drawSerial; a view the tier no longer records - rays switched off,
        /// the chain gone to the no-ray route - is not reported: the scene's status is its current producer's).
        uint32_t ranDraw = 0u;
        unsigned long long rays = 0ull, vramBytes = 0ull;
        float cpuMs = -1.0f;
        std::string reason;
        /// THE READBACK (the arm area.visReadback): a host ring of the product, decoded once the slot retired.
        RawBuffer readback[kAreaVisReadbacks];
        uint32_t readbackFrame[kAreaVisReadbacks] = {};
        bool readbackLive[kAreaVisReadbacks] = {};
        std::vector<unsigned char> visHost;
        unsigned visHostW = 0, visHostH = 0;
    };
    bool makeAreaVisPipelines(std::string &err);
    void dropAreaVis(AreaVisView &av);
    std::unordered_map<const ReflectPassListener *, AreaVisView> mAreaVis;
    VkDescriptorSetLayout mAreaSetLayout = VK_NULL_HANDLE, mAreaResolveSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mAreaPipeLayout = VK_NULL_HANDLE, mAreaResolvePipeLayout = VK_NULL_HANDLE;
    VkPipeline            mAreaPipeline = VK_NULL_HANDLE, mAreaResolvePipeline = VK_NULL_HANDLE;
    VkShaderModule        mAreaModule = VK_NULL_HANDLE, mAreaResolveModule = VK_NULL_HANDLE;
    VkDescriptorPool      mAreaPool = VK_NULL_HANDLE;
    bool                  mAreaFailed = false;
    std::string           mAreaFailReason;
    struct SuspendedBindings {
        const Ogre::SceneManager *gatherSm = nullptr;
        FogHlmsListener::ProbeGatherBind gather;
        bool sun = false;
        bool area = false;   // PHOTON-7 7-A: the area visibility's registration
    };
    std::unordered_map<const ReflectPassListener *, SuspendedBindings> mSuspended;
    VkDescriptorSetLayout mSunSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mSunPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mSunPipeline = VK_NULL_HANDLE;
    VkShaderModule        mSunModule = VK_NULL_HANDLE;
    VkDescriptorPool      mSunPool = VK_NULL_HANDLE;
    /// Its own point sampler: the reflection's are made with the reflection
    /// pipeline, which a view with the SSR row off never builds.
    VkSampler             mSunSampler = VK_NULL_HANDLE;
    /// The pipeline (or the R8 storage format) is not available on this device:
    /// said ONCE, and the shadow map renders alone for the rest of the process.
    bool                  mSunFailed = false;
    std::string           mSunFailReason;

public:
    // ---- THE HIT DECODE (PHOTON-HIT-SHADE-1; SPECS/atom/D2_HIT_SHADING_DESIGN.md)
    // A ray hit no cache can shade (a mover, a rigged item, a static hit neither
    // its card nor a cascade answers, the gather's far copies) is appended to the
    // chain's HIT LIST by the trace; the chain's "Jahshaka hit decode" pass draws
    // HlmsAtom's decode over the list (one fragment per record); the write-back
    // (rq_hit_composite.comp) scatters the radiance into the reflection's mean and
    // the gather's atlas. THE RAY JOBS ARE TWO COMPOSITOR PASSES OF PHOTON'S
    // (photon/PhotonComponent.h; OgreChain.cpp places them): photon_trace in front
    // of the hit decode pass (or, in a chain with none, in front of photon_resolve),
    // photon_resolve in front of the first pass that shades with the prepass.
    /// photon_trace: the TRACES (the sun contact, the reflection, the gather), into
    /// the hit list when `next` — the pass the trace serves — is the hit decode
    /// pass, with no list bound otherwise (a hit no cache shades then has no sample
    /// this frame: the write-back's "unshaded" answer).
    void traceRayJobs(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *next);
    /// The hit decode pass's own pre-execute: the camera's auto aspect held, then
    /// HlmsAtom armed over the list the traces filled.
    void beginHitDecode(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass);
    /// After it: HlmsAtom disarmed, the decode draws hidden, the camera's aspect
    /// handed back.
    void endHitDecode(const ReflectPassListener *key);
    /// The pass a photon pass of `view`'s chain serves (false = nothing to record:
    /// the tier is closed, a capture workspace, no next pass).
    bool photonPassTarget(Ogre::CompositorPass *pass, const ReflectPassListener *key, OgreView *view,
                          Ogre::CompositorPass *&next) const;
    /// photon_resolve: the write-back, then every job's second half (the filters,
    /// the SH and integrate, the pass-scoped registrations for `next`, the first
    /// pass that reads the answer).
    void resolveRayJobs(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *next);
    /// THE OPAQUE SCREEN DECODE PASS (ATOM-DECODE-CLASS-1) shades with the same
    /// registrations as the opaque pass, which is the pass after it — but between the
    /// two, in the opaque pass's early pre-execute, the planar mirrors render their
    /// reflection workspaces on the same SceneManager, and a pass-scoped registration
    /// must never reach them (gi.gather_nest's GA-NEST). So the registrations are
    /// taken away after the decode pass and put back, exactly as they were, in front
    /// of the opaque pass: no job re-runs.
    void suspendPassBindings(const ReflectPassListener *key, const Ogre::SceneManager *sm);
    void resumePassBindings(const ReflectPassListener *key);
    /// THE CAPTURE'S PASSES (CLOSE-SHOT-2, View::requestFrameCapture). The view's
    /// chain instanced a second time for one frame, drawn AFTER the view's own
    /// workspace, must not trace, decode, composite or step any history a second
    /// time — every one of those is the view's temporal state (the reflection's
    /// ping-pong and mover counters, the gather's EMA and age, the hit write-back).
    /// It is handed the products of THIS frame instead: the reflection the view's
    /// chain finished (copied into the capture's own jahSsrReflection at its first
    /// prepass-use pass) and the gather's and the sun contact's registrations
    /// (the resumePassBindings pattern), released again when each pass ends.
    void bindCapturePass(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass);
    void releaseCapturePass(const ReflectPassListener *key);
    void forgetHits(const ReflectPassListener *key);
    /// The hit list's counters (read back several frames late) for a scene.
    void hitStatsInto(const OgreScene *scene, RayQueryStatus &st) const;
    /// THE LIST'S OVERFLOW for a view's listener (OgreView::sizeHitList, kHitListWidth's law): the largest demand (the
    /// counter word 0, past the capacity) of a retired frame whose traces appended more records than that frame's list
    /// held, since the last call - 0 when none did. Taking it clears it.
    unsigned long long takeHitOverflow(const ReflectPassListener *key);

private:
    struct HitView {
        /// The list's buffer: [0] records appended (may pass the capacity), [1]
        /// records dropped, then four words per record (the sun, the footprint, the
        /// weight, the ray's origin height, its length and the eye's distance to
        /// its origin — jah_rq_hit_record.glsl). An Ogre UAV buffer: HlmsAtom reads it
        /// through a read-only view (a buffer, not an image: the decode's pixel
        /// shader's pass textures already reach the pin's table's end —
        /// kHitBufSlot, HlmsAtom.h).
        Ogre::UavBufferPacked *buf = nullptr;
        /// This frame's list, the chain's textures (the names OgreChain.cpp gives).
        Ogre::TextureGpu *ids = nullptr, *dest = nullptr, *radiance = nullptr;
        uint32_t capacity = 0u, width = 0u;
        /// The list was bound for THIS frame's traces (a write-back is owed).
        bool live = false;
        /// Ogre's frame number of the last decode pass this key ran in front of.
        uint32_t decodeFrame = 0xFFFFFFFFu;
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer params[kReflectRing];
        /// The buffer's counter words (jah_rq_hit_record.glsl: five of its eight), copied per frame into a host ring
        /// and read once the frame retired (never a wait).
        RawBuffer readback;
        /// ...with the records that frame's LIST held (its rows x width, never the overflow arm's cap: the list's own
        /// overflow is what grows it).
        struct Pending { uint32_t frame = 0; bool live = false; uint32_t restTag = 0u; uint32_t listRecords = 0u; };
        Pending pending[kFramesInFlight];
        unsigned frame = 0;
        OgreScene *scene = nullptr;
        /// The newest retired frame's counters (RayQueryStatus) and the scene draw they landed at (a view whose readings
        /// stopped is STALE: the status counts live views only).
        unsigned long long appended = 0ull, dropped = 0ull;
        uint32_t readDraw = 0u;
        bool everRead = false;
        /// ...THE PER-VIEW READOUT (RayQueryStatus::hitViews, PHOTON-7 stage 0's read R4): the view's eyes and one
        /// eye's target (stamped at every prepare), the list the newest read-back frame held and the list the FIRST
        /// read-back frame held (the creation size; never rewritten).
        unsigned eyes = 1u, eyeWidth = 0u, eyeHeight = 0u;
        uint32_t readCapacity = 0u, firstCapacity = 0u;
        /// The largest demand of a retired frame that overflowed its list, not yet taken (takeHitOverflow).
        unsigned long long overflowDemand = 0ull;
        /// THE REFLECTION'S REST COUNT (PHOTON-2 round 2, R1): the trace's tag for this frame (the rest's serial in
        /// the top 12 bits, its rest frame below; 0 = the trace counted nothing) and, read back, the newest retired
        /// frame's count of active resting texels (counter word 2) with its tag.
        uint32_t restTag = 0u, restActive = 0u, restActiveTag = 0u;
        /// ...and that frame's CAPPED texels (counter word 3, commit 8): stopped at their count's 16-bit ceiling, not by
        /// their standard error - the rest's defect reading.
        uint32_t restCapped = 0u;
        /// ...and its BATCHING texels (counter word 4: the rest's tail budget, kReflectRestRaysCap's note).
        uint32_t restBatching = 0u;
        /// THE NEE's CONE HIT RATE (PHOTON-7 11-1b, counter words 10-11; the lead's condition): the NEE rays and those
        /// whose first hit was the target drawn, summed over every retired frame read.
        unsigned long long neeRays = 0ull, neeHits = 0ull;
        /// THE REST'S BOUND's words (PHOTON-7 11-1c, counter words 8-9: filled at rest frame 2N, held after it) as the
        /// newest retired frame that carried them read them, with that frame's rest tag.
        uint32_t boundOwed = 0u, boundNeedMax = 0u, boundTag = 0u;
        /// Armed for the decode pass: the SceneManager whose draws are shown, and
        /// the camera whose auto aspect is held off for the pass (the decode's
        /// target is the list, not a picture — F7).
        Ogre::SceneManager *armedSm = nullptr;
        Ogre::Camera *pinnedCam = nullptr;
        bool camAuto = false;
    };
    std::unordered_map<const ReflectPassListener *, HitView> mHits;
    bool prepareHitList(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass,
                        HitListBinding &out);
    /// A decode twin of `scene` no longer matches its PBS datablock (a witness's
    /// Hlms hash or datablock moved since the last sync) — read only.
    bool decodeTwinsStale(OgreScene *scene) const;
    /// The binding a trace takes when no list is bound: the stand-ins, `on` false.
    void hitStandIns(HitListBinding &out);
    void recordHitComposite(const ReflectPassListener *key);
    bool makeCompositePipeline(std::string &err);
    bool ensureHitDummies(std::string &err);
    void initHitDummies(VkCommandBuffer cmd);
    void readHitCounters(HitView &hv);
    VkDescriptorSetLayout mCompSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCompPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mCompPipeline = VK_NULL_HANDLE;
    VkShaderModule        mCompModule = VK_NULL_HANDLE;
    VkDescriptorPool      mCompPool = VK_NULL_HANDLE;
    bool                  mCompFailed = false;
    /// 1x1 storage stand-ins in GENERAL (a set's every view must be real): the
    /// list's two formats and the RGBA16F of the gather's atlas.
    ReflectImage mHitDummyIds, mHitDummyColour, mHitDummyDest;
    /// The reflection sample's stand-in: a SCREEN image (one layer, an array
    /// view — what the composite declares since LAYERED-STEREO-1); mHitDummyColour stays the probe atlas's 2D stand-in.
    ReflectImage mHitDummyHist;
    /// ...and the reflection state's (rg32ui, one layer, an array view): the record runs' fold (binding 10-11).
    ReflectImage mHitDummyState;
    bool mHitDummiesReady = false;
    bool mHitDummiesNeedInit = false;
    /// THE TIER'S COUNTERS (jah_rq_alpha.glsl's JahTierCounters; host-visible, coherent; the shaders' atomics, read with
    /// no wait - a few frames late): [0] the reading rays whose pane list filled (the one read's F4), cumulative over
    /// the tier's life. Created at open() (the hit set's reads find it made), dropped at close().
    RawBuffer mTierCounters;
    uint64_t tierCountersAddress();
    // ---- THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4) ----------------
    // One dispatch per scene per frame a mover moved, recorded from the
    // surface cache's workspacePosUpdate — after the capture's copies (it reads
    // the new Depth and Normal) and before the relight that multiplies its
    // answer in (rq_card_movers.comp). The TLAS it traces was built this frame
    // (updateScene, before any render target).
public:
    bool traceCardMovers(OgreScene *scene, const CardMoverTrace &job);
    bool cardTracePresent() const { return isOpen() && !mCmFailed; }
    /// The two traces' last GPU milliseconds (the monitor's rows, owner = the scene).
    void cardMoverTimes(OgreScene *scene, float &traceMs, float &stillMs);
    void forgetCardMovers(OgreScene *scene);

private:
    struct CardMoverView {
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        RawBuffer       records[kReflectRing];
        RawBuffer       rowOfSlot[kReflectRing];   ///< the geometry rows' per-slot copy (the open fraction's buried test)
        unsigned frame = 0;
    };
    bool makeCardMoverPipeline(std::string &err) {   // (the ahead build's answer, else the build now: aheadOr)
        return aheadOr(kAheadCardMovers, err, [this](std::string &e) { return makeCardMoverPipelineNow(e); });
    }
    bool makeCardMoverPipelineNow(std::string &err);
    /// Per scene, one view per MODE of the job (rq_card_movers.comp): the movers'
    /// trace and the still world's — each its own ring and sets, since
    /// both record in one frame.
    std::unordered_map<const OgreScene *, CardMoverView> mCardMovers;
    std::unordered_map<const OgreScene *, CardMoverView> mCardStill;
    /// ...and the lamps' (PHOTON-7 stage 5, 5-3: mode 2, the relight's cards toward its lamps).
    std::unordered_map<const OgreScene *, CardMoverView> mCardLamps;
    VkDescriptorSetLayout mCmSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCmPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mCmPipeline = VK_NULL_HANDLE;
    VkShaderModule        mCmModule = VK_NULL_HANDLE;
    VkDescriptorPool      mCmPool = VK_NULL_HANDLE;
    // ---- THE CARDS' INDIRECT BY TLAS RAYS (PHOTON-7 stage 10, 10-2a; rq_card_gather.comp) ----------------------
    // One dispatch per scene per relight that re-gathers a card's indirect, recorded from the surface cache's
    // photonUpdate BEFORE the relight that reads the Gather layer, against the TLAS built this frame (updateScene).
public:
    CardGatherResult gatherCards(OgreScene *scene, const CardGatherTrace &job);
    /// The life of the scene's counts, every retired dispatch's folded in (CardCacheStatus::gather).
    void cardGatherCounts(OgreScene *scene, CardCacheStatus::GatherCounts &out);
    /// ...and each gathered card's own sky rays (its atlas slot, the count), every retired dispatch's since the last call.
    void cardGatherSky(OgreScene *scene, std::vector<std::pair<unsigned, unsigned>> &out);
    void forgetCardGather(OgreScene *scene);

private:
    struct CardGatherView {
        VkDescriptorSet sets[kReflectRing] = {};
        RawBuffer       params[kReflectRing];
        RawBuffer       records[kReflectRing];
        RawBuffer       rowOfSlot[kReflectRing];
        /// the dispatch's eight counts (host-visible: the shader's atomics land in it), read once its frame retired
        RawBuffer       counts[kReflectRing];
        uint32_t        countsFrame[kReflectRing] = {};
        bool            countsPending[kReflectRing] = {};
        /// each pending dispatch's records' atlas slots (their per-record sky counts' owners)
        std::vector<uint32_t> countsSlots[kReflectRing];
        /// the per-card sky counts folded since the cache last drained them: (slot, sky rays)
        std::vector<std::pair<unsigned, unsigned>> skyDrain;
        unsigned frame = 0;
        CardCacheStatus::GatherCounts total;
    };
    bool makeCardGatherPipeline(std::string &err) {   // (the ahead build's answer, else the build now: aheadOr)
        return aheadOr(kAheadCardGather, err, [this](std::string &e) { return makeCardGatherPipelineNow(e); });
    }
    bool makeCardGatherPipelineNow(std::string &err);
    void foldCardGatherCounts(CardGatherView &v);
    std::unordered_map<const OgreScene *, CardGatherView> mCardGather;
    VkDescriptorSetLayout mCgSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mCgPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mCgPipeline = VK_NULL_HANDLE;
    VkShaderModule        mCgModule = VK_NULL_HANDLE;
    VkDescriptorPool      mCgPool = VK_NULL_HANDLE;
    bool                  mCgFailed = false;
    /// a read-list block of zeros, the source of the copy that clears this dispatch's cards' blocks (10-2c)
    RawBuffer             mCgZeroBlock;
    // ---- THE BLENDED PASSES' ESCAPES FOLLOW THE RAY (PHOTON-7 10-4 R4, design (D); rq_glass_escape.comp) ----------------
    // A blended or refractive fragment at the ray tier appends its specular lobe's open share as a record
    // (PhotonVct_piece_ps.any); after its pass the records are traced on the TLAS from where the voxel chain's read ended
    // and their light added into the pass's colour target (rq_glass_escape_add.comp) - the same frame. Two lists a view:
    // the blended pass's (0) and the refractives' (1).
public:
    /// photon_resolve, outside any render pass: the view's lists sized (the demand law) and reset for this frame.
    void prepareGlassEscape(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *next);
    /// In front of the blended pass (0) / the refractives (1): the list registered for the pass's fragments.
    void bindGlassEscape(const ReflectPassListener *key, OgreView *view, unsigned which);
    /// After it: the registration taken away, the records traced and added into the pass's target.
    void traceGlassEscape(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass, unsigned which);
    void forgetGlassEscape(const ReflectPassListener *key);
    void glassEscapeStatsInto(const OgreScene *scene, RayQueryStatus &st) const;

private:
    struct EscapeList {
        RawBuffer       list;              ///< the header (8 words) and the records (two uvec4 each), device-local
        VkDeviceAddress address = 0;
        unsigned        capacity = 0u;
        RawBuffer       counts;            ///< the job's counts (kEscapeCounts words), device-local
        RawBuffer       params[kReflectRing];
        RawBuffer       addParams[kReflectRing];
        RawBuffer       rowOfSlot[kReflectRing];
        VkDescriptorSet sets[kReflectRing] = {};
        VkDescriptorSet addSets[kReflectRing] = {};
        bool            ready = false;     ///< reset this frame (prepareGlassEscape): its pass may append
    };
    struct EscapeView {
        const OgreScene *scene = nullptr;
        unsigned width = 0u, height = 0u, eyes = 1u;
        RawBuffer accum;                   ///< three fixed-point words a pixel, eye-major rows
        EscapeList lists[2];
        /// host-visible: per frame in flight and list, 16 words - the header's record count, then the job's counts
        RawBuffer readback;
        struct Pending { uint32_t frame = 0u; bool live = false; unsigned capacity = 0u; };
        Pending pending[kFramesInFlight][2];
        unsigned frame = 0u;
        /// THE NEWEST READ (summed over the two lists): the records appended, those a full list dropped, the lists'
        /// capacity, the job's counts; the largest demand a retired frame showed (the growth's target)
        unsigned long long records = 0ull, dropped = 0ull, capacity = 0ull;
        unsigned long long counts[9] = {};   // (kEscapeCounts)
        unsigned long long demand[2] = {};
        uint32_t readDraw = 0u;
        bool everRead = false;
    };
    bool makeGlassEscapePipelines(std::string &err) {   // (the ahead build's answer, else the build now: aheadOr)
        return aheadOr(kAheadGlassEscape, err, [this](std::string &e) { return makeGlassEscapePipelinesNow(e); });
    }
    bool makeGlassEscapePipelinesNow(std::string &err);
    void readGlassEscape(EscapeView &ev);
    std::unordered_map<const ReflectPassListener *, EscapeView> mEscape;
    VkDescriptorSetLayout mEscSetLayout = VK_NULL_HANDLE, mEscAddSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mEscPipeLayout = VK_NULL_HANDLE, mEscAddPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mEscPipeline = VK_NULL_HANDLE, mEscAddPipeline = VK_NULL_HANDLE;
    VkShaderModule        mEscModule = VK_NULL_HANDLE, mEscAddModule = VK_NULL_HANDLE;
    VkDescriptorPool      mEscPool = VK_NULL_HANDLE;
    bool                  mEscFailed = false;
    // ---- THE VOXEL SIDES' GATHER BY TLAS RAYS (PHOTON-7 stage 10, 10-2b; rq_voxel_sides.comp) ----------------------
    // One dispatch per cascade pass at the ray tier, recorded from PhotonVoxelLighting::runFeedbackPass BEFORE the
    // bounce that reads its scratch, over the pass's own list by its indirect arguments.
public:
    int gatherVoxelSides(OgreScene *scene, const Ogre::PhotonVoxelSideTrace &job, unsigned cascade);
    void voxelSideCounts(OgreScene *scene, GiStatus::VoxelSideCounts &out);
    bool voxelSideShares(OgreScene *scene, GiVoxelSideShares &out);
    void forgetVoxelSides(OgreScene *scene);

private:
    /// a scene's ring of call slots (a frame records one call a cascade pass: kVoxelSideRing covers its frames in flight)
    static constexpr unsigned kVoxelSideRing = 96u;
    struct VoxelSideView {
        VkDescriptorSet sets[kVoxelSideRing] = {};
        RawBuffer       params[kVoxelSideRing];
        RawBuffer       rowOfSlot[kVoxelSideRing];
        RawBuffer       counts[kVoxelSideRing];
        uint32_t        countsFrame[kVoxelSideRing] = {};
        bool            countsPending[kVoxelSideRing] = {};
        unsigned call = 0;
        GiStatus::VoxelSideCounts total;
        /// THE ESCAPE SHARES (PHOTON-7 11-2 (6)): cascade 0's one word a voxel, host-visible (rq_voxel_sides.comp
        /// binding 37), at the grid's voxel dims; reallocated (zeroed) when they change
        RawBuffer shares;
        uint32_t  sharesDims[3] = {};
        /// ...how many cascade-0 passes wrote them, the frame of the last, and the grid they were written over (its
        /// corner and cell, world metres) - what the portal scan (updatePortals) reads them against
        unsigned long long sharesWrites = 0ull;
        uint32_t  sharesFrame = 0u;
        float     sharesOrigin[3] = {}, sharesCell[3] = {};
    };
    bool makeVoxelSidePipeline(std::string &err) {   // (the ahead build's answer, else the build now: aheadOr)
        return aheadOr(kAheadVoxelSides, err, [this](std::string &e) { return makeVoxelSidePipelineNow(e); });
    }
    bool makeVoxelSidePipelineNow(std::string &err);
    void foldVoxelSideCounts(VoxelSideView &v);
    std::unordered_map<const OgreScene *, VoxelSideView> mVoxelSides;
    VkDescriptorSetLayout mVsSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mVsPipeLayout = VK_NULL_HANDLE;
    VkPipeline            mVsPipeline = VK_NULL_HANDLE;
    VkShaderModule        mVsModule = VK_NULL_HANDLE;
    VkDescriptorPool      mVsPool = VK_NULL_HANDLE;
    bool                  mVsFailed = false;
    /// THE INSTANCE JOB (ATOM-CPU-WALKS-1, rq_tlas_write.comp): its pipeline and the
    /// pool its per-scene ring of sets comes from (kTlasWriteSets, from the layout's
    /// own constants).
    VkDescriptorSetLayout mTwSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      mTwPipeLayout = VK_NULL_HANDLE;
    VkShaderModule        mTwModule = VK_NULL_HANDLE;
    VkPipeline            mTwPipeline = VK_NULL_HANDLE;
    VkDescriptorPool      mTwPool = VK_NULL_HANDLE;
    bool                  mTwFailed = false;
    bool                  mCmFailed = false;

    /// ONE IMAGE'S BASIS from a pose and a frustum (rq_reflect.comp's five
    /// numbers) — the arithmetic recordReflect's eyes are built with, shared so
    /// the contact rays and the reflection rays cannot drift apart.
    static EyeBasisF eyeBasis(bool ortho, const Ogre::Vector3 &pos, const Ogre::Quaternion &rot,
                              float el, float er, float et, float eb);
    /// THE LETTERBOX (SSR-LETTERBOX-1's ray half): a constrained-aspect camera
    /// draws the target's INNER rectangle while a compute pass addresses the
    /// whole target, so the basis is EXPANDED to the target on the CPU.
    static void expandEyeToTarget(EyeBasisF &e, const ChainDesc &cd, unsigned fullW,
                                  unsigned fullH);
};

// ---------------------------------------------------------------------------
// THE SCENE'S RAY STRUCTURES, held by its substrate (GpuSceneSubstrate.h). The type
// is the tier's; the object is the scene's. Its device resources are released by the
// tier (forgetScene through the retire window, releaseScene behind an idle device)
// before the substrate deletes it — the deleter only frees the bookkeeping.
struct RaySceneStructures {
    RayQueryTier::SceneAs as;
};

void RaySceneStructuresDelete::operator()(RaySceneStructures *p) const { delete p; }

RayQueryTier::SceneAs *RayQueryTier::asOf(const OgreScene *scene) {
    return scene && scene->substrate().rays ? &scene->substrate().rays->as : nullptr;
}

RayQueryTier::SceneAs &RayQueryTier::asFor(OgreScene *scene) {
    GpuSceneSubstrate::RayStructures &rays = scene->rayStructuresSlot();
    if (!rays) rays.reset(new RaySceneStructures());
    return rays->as;
}

// ---------------------------------------------------------------------------
uint32_t RayQueryTier::memoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
    const VkPhysicalDeviceMemoryProperties &mp = mDev->mDeviceMemoryProperties;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return uint32_t(-1);
}

bool RayQueryTier::makeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
                              bool deviceAddress, RawBuffer &out, std::string &err) {
    if (size == 0) size = 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage | (deviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0u);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(mVk, &bci, nullptr, &out.buffer) != VK_SUCCESS) {
        err = "rayquery: vkCreateBuffer failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(mVk, out.buffer, &req);
    VkMemoryAllocateFlagsInfo flags{};
    flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
    flags.flags = deviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0u;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = deviceAddress ? &flags : nullptr;
    mai.allocationSize = req.size;
    // A READBACK (host-visible, written by copies only) IS READ BY THE HOST: CACHED memory where the device has it
    // (PHOTON-2 stage 0, measured: an uncached, write-combined 16 MB readback of a history image read back at
    // ~0.1-0.2 s a frame in the stillness door; a cached one at memory speed). The upload buffers keep the first
    // coherent type (a host write never reads it back).
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const bool readback = hostVisible && (usage & ~VkBufferUsageFlags(VK_BUFFER_USAGE_TRANSFER_DST_BIT)) == 0u;
    mai.memoryTypeIndex = readback ? memoryType(req.memoryTypeBits, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                                   : uint32_t(-1);
    if (mai.memoryTypeIndex == uint32_t(-1))
        mai.memoryTypeIndex =
            memoryType(req.memoryTypeBits, hostVisible ? coherent : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == uint32_t(-1)) {
        err = "rayquery: no suitable memory type";
        vkDestroyBuffer(mVk, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkAllocateMemory(mVk, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        err = "rayquery: vkAllocateMemory failed (" + std::to_string(size_t(req.size)) + " bytes)";
        vkDestroyBuffer(mVk, out.buffer, nullptr);
        out.buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(mVk, out.buffer, out.memory, 0);
    out.size = req.size;
    if (hostVisible) vkMapMemory(mVk, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    return true;
}

void RayQueryTier::dropBuffer(RawBuffer &b) {
    if (b.mapped) { vkUnmapMemory(mVk, b.memory); b.mapped = nullptr; }
    if (b.buffer) vkDestroyBuffer(mVk, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(mVk, b.memory, nullptr);
    b = RawBuffer();
}

VkDeviceAddress RayQueryTier::addressOf(VkBuffer b) const {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = b;
    return mFn.getBufferDeviceAddress(mVk, &info);
}

uint32_t RayQueryTier::frameNow() const {
    // Ogre's own frame counter — the one it retires its dynamic buffers on.
    return mDev && mDev->mVaoManager ? mDev->mVaoManager->getFrameCount() : 0u;
}

uint32_t RayQueryTier::framesInFlight() const {
    const uint32_t n = mDev && mDev->mVaoManager
                           ? uint32_t(mDev->mVaoManager->getDynamicBufferMultiplier())
                           : kFramesInFlight;
    return n ? n : kFramesInFlight;
}

/// RETIRE, NEVER FREE IN PLACE. Anything this frame recorded a reference to —
/// a scratch arena a queued build reads, a structure a queued trace reads, the
/// instance array a queued TLAS build reads — is released only once Ogre's own
/// frame counter has moved past the depth it keeps in flight. This is what
/// replaces the vkDeviceWaitIdle calls (finding 6): the GPU is never drained,
/// the memory is simply handed back later.
void RayQueryTier::retire(RawBuffer &b) {
    if (!b.buffer && !b.memory) return;
    Retired r;
    r.buf = b;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    b = RawBuffer();
}

void RayQueryTier::retire(VkAccelerationStructureKHR &as, RawBuffer &storage) {
    Retired r;
    r.as = as;
    r.buf = storage;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    as = VK_NULL_HANDLE;
    storage = RawBuffer();
}

/// EVICT A BLAS NOTHING POINTS AT ANY MORE (finding 5). A bottom-level
/// structure is memory AND a held MeshPtr, and the hold is what keeps a mesh's
/// vertex buffers alive under a structure built over them. Keeping one for a
/// mesh no instance has referenced for a long while pins geometry the scene has
/// finished with — and it compounds the compaction ring, because every stale
/// entry is a slot the ring may still be waiting on.
///
/// The window is generous on purpose: a mesh that flickers in and out of the
/// traced set (an object hidden and shown) must not pay a rebuild each time.
void RayQueryTier::evictStaleBlas(SceneAs &sa) {
    static constexpr uint32_t kEvictAfterFrames = 600u;      // ~10 s at 60 Hz
    // ONE SWEEP PER kEvictSweepFrames, its own clock (decision 14.4): the walk over every structure ran
    // every moved frame. A structure is evicted 600 to 1,200 frames after its last reference went.
    static constexpr uint32_t kEvictSweepFrames = 600u;
    const uint32_t now = frameNow();
    if (sa.evictSweptAt && uint32_t(now - sa.evictSweptAt) < kEvictSweepFrames) return;
    sa.evictSweptAt = now ? now : 1u;
    // THE MAP'S ENTRIES GO IN ONE PASS (PHOTON-2 round 2, the hitch law): erasing each evicted slot's entries with its
    // own walk of the map was slots x entries - on Bistro the sweep after a walk was an 11 ms frame (engine.rayquery).
    std::vector<char> evicted(sa.blas.size(), 0);
    bool any = false;
    for (size_t i = 0; i < sa.blas.size(); ++i) {
        ++sa.st.updateVisits;
        const Blas &bl = sa.blas[i];
        if (!bl.as || bl.compactState == 1u) continue;        // a size query is still owed
        // STILL REFERENCED = never stale (stage 3: the feed's key events keep `referenced`; `lastSeen`
        // is the frame the last reference went).
        if (bl.referenced || bl.lastSeen == 0u || uint32_t(now - bl.lastSeen) < kEvictAfterFrames) continue;
        evicted[i] = 1;
        any = true;
    }
    if (!any) return;
    for (auto it = sa.blasOf.begin(); it != sa.blasOf.end();) {
        if (it->second < evicted.size() && evicted[it->second]) it = sa.blasOf.erase(it); else ++it;
    }
    for (size_t i = 0; i < sa.blas.size(); ++i) {
        if (!evicted[i]) continue;
        Blas &bl = sa.blas[i];
        // The slot stays (indices are referenced by blasOf and by the
        // instance patches); only its contents go, and the map entry with them,
        // so the next sighting rebuilds into a fresh slot.
        retire(bl.as, bl.storage);
        sa.dirtyMeshRows.push_back(bl.mesh.get());   // its record loses this level's address
        bl.mesh.reset();
        bl.triangles = 0;
        bl.address = 0;
        bl.compactState = 0;
        bl.lastSeen = 0;
        ++sa.blasVersion;         // the set changed: the next TLAS is a rebuild
    }
}

/// The lowest contiguous run of `n` FREE compaction slots, or -1. Contiguous
/// because `vkCmdWriteAccelerationStructuresPropertiesKHR` takes one
/// firstQuery for the whole batch; lowest so the ring packs down rather than
/// drifting upward past parked scenes' reservations.
int RayQueryTier::takeCompactRun(unsigned n) const {
    if (!n || n > kCompactRing) return -1;
    unsigned run = 0;
    for (unsigned i = 0; i < kCompactRing; ++i) {
        if (mCompactBusy & (uint64_t(1) << i)) {
            run = 0;
            continue;
        }
        if (++run == n) return int(i + 1 - n);
    }
    return -1;
}

void RayQueryTier::retireView(VkImageView v) {
    if (!v) return;
    Retired r;
    r.view = v;
    r.frame = frameNow();
    mRetireBin.push_back(r);
}

void RayQueryTier::retireSet(VkDescriptorSet set) {
    joinAhead(kAheadReflect);   // (mReflectPool is the ahead build's)
    retireSet(set, mReflectPool);
}

void RayQueryTier::retireSet(VkDescriptorSet set, VkDescriptorPool pool) {
    if (!set) return;
    Retired r;
    r.set = set;
    r.setPool = pool;
    r.frame = frameNow();
    mRetireBin.push_back(r);
}

/// GATHER-0. An Ogre texture whose descriptor sets may still be in flight; the
/// destroy is the manager's, the WAIT is this bin's.
void RayQueryTier::retireTexture(Ogre::TextureGpu *&t) {
    if (!t) return;
    Retired r;
    r.tex = t;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    t = nullptr;
}

void RayQueryTier::retireImage(ReflectImage &img) {
    if (!img.image && !img.memory && !img.view) return;
    Retired r;
    r.img = img;
    r.frame = frameNow();
    mRetireBin.push_back(r);
    img = ReflectImage();
}

bool RayQueryTier::ensureDummyImages(std::string &err) {
    if (mDummiesReady) return true;
    const VkFormat fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    struct Spec { ReflectImage *img = nullptr; VkImageType type = VK_IMAGE_TYPE_2D;
                  VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D; uint32_t layers = 0;
                  VkImageCreateFlags flags = 0; };
    const Spec specs[3] = {
        { &mDummyCube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6u,
          VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT },
        { &mDummyVolume, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1u, 0u },
        { &mDummyFlat, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, 1u, 0u },
    };
    for (const Spec &sp : specs) {
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.flags = sp.flags;
        ici.imageType = sp.type;
        ici.format = fmt;
        ici.extent = { 1u, 1u, 1u };
        ici.mipLevels = 1;
        ici.arrayLayers = sp.layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(mVk, &ici, nullptr, &sp.img->image) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateImage (stand-in) failed";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(mVk, sp.img->image, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == uint32_t(-1) ||
            vkAllocateMemory(mVk, &mai, nullptr, &sp.img->memory) != VK_SUCCESS) {
            err = "rayquery/reflect: no memory for the stand-in image";
            return false;
        }
        vkBindImageMemory(mVk, sp.img->image, sp.img->memory, 0);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = sp.img->image;
        vci.viewType = sp.viewType;
        vci.format = fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = sp.layers;
        if (vkCreateImageView(mVk, &vci, nullptr, &sp.img->view) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateImageView (stand-in) failed";
            return false;
        }
    }
    {   // FAR-SUN-2: the flat stand-in as a one-layer array (the texture pool's binding).
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = mDummyFlat.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = fmt;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(mVk, &vci, nullptr, &mDummyFlatArrayView) != VK_SUCCESS) {
            mDummyFlatArrayView = VK_NULL_HANDLE;
            err = "rayquery: vkCreateImageView (the flat array stand-in) failed";
            return false;
        }
    }
    {   // THE ID IMAGE'S STAND-IN (REFLECT-MOVERS-1): an unsigned image, for a usampler.
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R32G32_UINT;
        ici.extent = { 1u, 1u, 1u };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(mVk, &ici, nullptr, &mDummyIds.image) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateImage (id stand-in) failed";
            return false;
        }
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(mVk, mDummyIds.image, &req);
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mai.memoryTypeIndex == uint32_t(-1) ||
            vkAllocateMemory(mVk, &mai, nullptr, &mDummyIds.memory) != VK_SUCCESS) {
            err = "rayquery/reflect: no memory for the id stand-in";
            return false;
        }
        vkBindImageMemory(mVk, mDummyIds.image, mDummyIds.memory, 0);
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = mDummyIds.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R32G32_UINT;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(mVk, &vci, nullptr, &mDummyIds.view) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateImageView (id stand-in) failed";
            return false;
        }
    }
    if (!mDummyStorage.buffer &&
        !makeBuffer(256u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, mDummyStorage, err))
        return false;
    std::memset(mDummyStorage.mapped, 0, 256u);
    mDummiesReady = true;
    mDummiesNeedClear = true;
    return true;
}

void RayQueryTier::clearDummyImages(VkCommandBuffer cmd) {
    if (!mDummiesNeedClear) return;
    mDummiesNeedClear = false;
    ReflectImage *imgs[4] = { &mDummyCube, &mDummyVolume, &mDummyFlat, &mDummyIds };
    const uint32_t layers[4] = { 6u, 1u, 1u, 1u };
    for (int i = 0; i < 4; ++i) {
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = layers[i];
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[i]->image;
        b.subresourceRange = range;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue zero{};
        // the id stand-in reads "nothing drew here" (jah_atom_id.glsl's empty word)
        if (imgs[i] == &mDummyIds) zero.uint32[0] = zero.uint32[1] = 0xFFFFFFFFu;
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1,
                             &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
    }
}

bool RayQueryTier::makeStorageImage(unsigned w, unsigned h, VkFormat fmt, ReflectImage &out,
                                    std::string &err, unsigned layers) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, h, 1u };
    ici.mipLevels = 1;
    ici.arrayLayers = std::max(layers, 1u);
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_SRC (FAR-SUN-1): an instrument may copy one out (the gather's atlas for the
    // gather-hits readback); SAMPLED (PHOTON-7 stage 7, 7-3): a reader may filter one (the gather's
    // stored filtered map, read bilinearly by the integrate's rough specular); usage alone moves no
    // byte of any picture.
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(mVk, &ici, nullptr, &out.image) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateImage failed";
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(mVk, out.image, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == uint32_t(-1) ||
        vkAllocateMemory(mVk, &mai, nullptr, &out.memory) != VK_SUCCESS) {
        vkDestroyImage(mVk, out.image, nullptr);
        out = ReflectImage();
        err = "rayquery/reflect: no device-local memory for the temporal mean";
        return false;
    }
    vkBindImageMemory(mVk, out.image, out.memory, 0);
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out.image;
    vci.viewType = layers ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = fmt;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = std::max(layers, 1u);
    if (vkCreateImageView(mVk, &vci, nullptr, &out.view) != VK_SUCCESS) {
        vkFreeMemory(mVk, out.memory, nullptr);
        vkDestroyImage(mVk, out.image, nullptr);
        out = ReflectImage();
        err = "rayquery/reflect: vkCreateImageView failed";
        return false;
    }
    return true;
}

VkImageView RayQueryTier::layerView(Ogre::TextureGpu *t, bool storage) {
    if (!t) return VK_NULL_HANDLE;
    auto *vt = static_cast<Ogre::VulkanTextureGpu *>(t);
    // The format Ogre's own views use: a storage view takes the LINEAR equivalent
    // (an sRGB format has no storage), a sampled one the texture's own.
    Ogre::PixelFormatGpu pf = t->getPixelFormat();
    if (storage) pf = Ogre::PixelFormatGpuUtils::getEquivalentLinear(pf);
    VkImageViewUsageCreateInfo usage{};
    usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    usage.usage = storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.pNext = &usage;
    vci.image = vt->getFinalTextureName();
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vci.format = Ogre::VulkanMappings::get(pf);
    // A depth texture is sampled through its DEPTH aspect alone.
    vci.subresourceRange.aspectMask = Ogre::VulkanMappings::getImageAspect(pf, true);
    vci.subresourceRange.levelCount = storage ? 1u : std::max<uint32_t>(t->getNumMipmaps(), 1u);
    vci.subresourceRange.layerCount = std::max<uint32_t>(t->getNumSlices(), 1u);
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(mVk, &vci, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    retireView(view);
    return view;
}

void RayQueryTier::drainRetired() {
    const uint32_t now = frameNow(), keep = framesInFlight() + 1u;
    // ONE PASS, AT MOST kRetireStructuresPerFrame ACCELERATION STRUCTURES A FRAME (PHOTON-2 round 2, the hitch law):
    // erasing each destroyed entry from the vector's middle was entries x bin size, and an eviction sweep's few hundred
    // structures destroyed in one frame made a 14 ms one on Bistro (engine.rayquery); the rest wait a frame - they are
    // only memory. Views, sets and buffers go as they fall due (a view and a set are retired EVERY frame, and a backlog
    // of sets exhausts their pool).
    static constexpr size_t kRetireStructuresPerFrame = 48u;
    size_t structures = 0u, out = 0u;
    for (size_t i = 0; i < mRetireBin.size(); ++i) {
        // Unsigned subtraction: the pin's counter wraps, and "now - then" is
        // the elapsed count either way as long as the gap is small.
        const bool due = uint32_t(now - mRetireBin[i].frame) >= keep;
        const bool structure = mRetireBin[i].as != VK_NULL_HANDLE;
        if (due && (!structure || structures < kRetireStructuresPerFrame)) {
            if (structure) ++structures;
            if (mRetireBin[i].as) mFn.destroyAccelerationStructure(mVk, mRetireBin[i].as, nullptr);
            dropBuffer(mRetireBin[i].buf);
            if (mRetireBin[i].set && mRetireBin[i].setPool)
                vkFreeDescriptorSets(mVk, mRetireBin[i].setPool, 1, &mRetireBin[i].set);
            if (mRetireBin[i].tex && mRs && mRs->getTextureGpuManager())
                mRs->getTextureGpuManager()->destroyTexture(mRetireBin[i].tex);
            if (mRetireBin[i].view) vkDestroyImageView(mVk, mRetireBin[i].view, nullptr);
            if (mRetireBin[i].img.view) vkDestroyImageView(mVk, mRetireBin[i].img.view, nullptr);
            if (mRetireBin[i].img.image) vkDestroyImage(mVk, mRetireBin[i].img.image, nullptr);
            if (mRetireBin[i].img.memory) vkFreeMemory(mVk, mRetireBin[i].img.memory, nullptr);
        } else {
            if (out != i) mRetireBin[out] = mRetireBin[i];
            ++out;
        }
    }
    mRetireBin.resize(out);
}

namespace {
/// The ahead build a worker runs (RayQueryTier::startAhead), null on every other thread.
thread_local std::vector<RayAheadNote> *tAheadNotes = nullptr;
}   // namespace

VkResult RayQueryTier::createComputePipeline(const VkComputePipelineCreateInfo &cpi, VkPipeline *out,
                                             const char *name) {
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult r = vkCreateComputePipelines(mVk, mDev ? mDev->mPipelineCache : VK_NULL_HANDLE, 1, &cpi, nullptr, out);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (tAheadNotes) {   // a worker: noted for the join, never logged here
        tAheadNotes->push_back({ name ? name : "", ms, r });
        return r;
    }
    ++mPipelinesBuilt;
    mPipelineBuildMs += ms;
    if (r == VK_SUCCESS) ShaderCache::noteEnginePipeline(ms, name);   // dirt for the pipeline layer
    Ogre::LogManager::getSingleton().logMessage(
        "rayquery: compute pipelines " + std::to_string(mPipelinesBuilt) + " built, this one (" +
        std::string(name ? name : "") + ") " +
        std::to_string(ms) + " ms, " + std::to_string(mPipelineBuildMs) + " ms in all" +
        (mDev && mDev->mPipelineCache ? " (through the device's pipeline cache)" : " (no pipeline cache)"));
    return r;
}

VkCommandBuffer RayQueryTier::frameCmd() {
    // OUTSIDE ANY ENCODER. Ogre tracks whether it is inside a render, compute
    // or copy encoder; vkCmdBuildAccelerationStructuresKHR may not be recorded
    // inside a render pass, and a copy encoder left open would make the
    // barriers below meaningless.
    // GRAPHICS, AND SAID SO (ENGINE-UPGRADE-ASYNC-3): the ray tier's frame work (the gather - a GI
    // READER -, the cards' trace, the TLAS builds) is never inside an async compute bracket; the
    // fork refuses it there by name. Outside one the recording queue IS the graphics queue.
    mDev->_jahRequireGraphics("the ray tier's frame commands (the gather, the card trace, the TLAS)");
    // THE RECORDING QUEUE: where Ogre records the frame's commands.
    mDev->_jahRecordQueue().endAllEncoders();
    // The pin's own public accessor — usable from outside the plugin since
    // fork 1a81f866a (was 0040) exported Ogre::onVulkanFailure, which its device-lost
    // branch calls and which the Vulkan render system did not export (so this
    // line compiled and failed to LINK).
    //
    // IT NEVER RETURNS NULL: on a lost device the accessor's checkVkResult
    // THROWS (the pin's onVulkanFailure raises, OgreVulkanRenderSystem.cpp).
    // The null guard at the call site is therefore belt-and-braces, not the
    // device-lost path it once claimed to be — the honest device-lost question
    // is the render system's own reason string, asked before we record.
    return mDev->_jahRecordQueue().getCurrentCmdBuffer();
}

// ---------------------------------------------------------------------------
bool RayQueryTier::open(Ogre::RenderSystem *rs, std::string &err) {
    mRs = dynamic_cast<Ogre::VulkanRenderSystem *>(rs);
    if (!mRs) {
        err = "rayquery: this engine is not running on the Vulkan render system";
        return false;
    }
    mDev = mRs->getVulkanDevice();
    if (mDev && mDev->mPhysicalDevice) {
        // SPIR-V 1.4 NEEDS A 1.2 DEVICE (audit C-7). Our module is compiled at
        // --target-env spirv1.4 and is only legal on a device whose own
        // apiVersion is at least 1.2; the extension check below does not imply
        // it. Refusing here is a supported no-rays boot, not a failure.
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mDev->mPhysicalDevice, &props);
        // THE RASTERISER'S SUB-PIXEL SNAP (the ray origin's point term, jah_ray_origin.glsl jahDepthPointError): a
        // vertex's framebuffer position is rounded to subPixelPrecisionBits fractional bits, so each corner moves by up
        // to half a step per axis - sqrt 2 x 2^-(bits + 1) pixels.
        mSubPixelSnapPx = float(std::sqrt(2.0) * std::ldexp(1.0, -int(props.limits.subPixelPrecisionBits) - 1));
        if (props.apiVersion < VK_API_VERSION_1_2) {
            err = "rayquery: the device reports Vulkan " +
                  std::to_string(VK_VERSION_MAJOR(props.apiVersion)) + "." +
                  std::to_string(VK_VERSION_MINOR(props.apiVersion)) +
                  ", and the tier's SPIR-V 1.4 module needs 1.2";
            return false;
        }
    }
    if (!mDev || !mDev->hasRayQuery()) {
        err = "rayquery: the device has no VK_KHR_ray_query (fork d014b064f (was 0038) missing, or the "
              "driver does not advertise it)";
        return false;
    }
    mVk = mDev->mDevice;
    if (!mFn.load(mVk)) {
        mVk = VK_NULL_HANDLE;
        err = "rayquery: a VK_KHR_acceleration_structure entry point is missing";
        return false;
    }

    mAsProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
    // THE SUBGROUP OPERATIONS THE REFLECTION'S TRACE DEALS ITS REST RAYS WITH (rq_reflect.comp, the rest's spread
    // lanes: a ballot, a prefix sum, a vote and shuffles, in compute) - core since Vulkan 1.1, but each a device's own
    // claim; a device without them is a supported no-rays boot, like one without ray queries.
    VkPhysicalDeviceSubgroupProperties subgroupProps{};
    subgroupProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    mAsProps.pNext = &subgroupProps;
    VkPhysicalDeviceProperties2 props2{};
    props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props2.pNext = &mAsProps;
    vkGetPhysicalDeviceProperties2(mDev->mPhysicalDevice, &props2);
    mAsProps.pNext = nullptr;
    const VkSubgroupFeatureFlags kSubgroupOps = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_VOTE_BIT |
                                                VK_SUBGROUP_FEATURE_ARITHMETIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                                VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    mSubgroupSize = std::max(1u, subgroupProps.subgroupSize);
    if (!(subgroupProps.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
        (subgroupProps.supportedOperations & kSubgroupOps) != kSubgroupOps) {
        mVk = VK_NULL_HANDLE;
        err = "rayquery: the device's compute subgroups lack the ballot / vote / arithmetic / shuffle operations the "
              "reflection's rest rays are dealt with";
        return false;
    }

    // COMPACTED SIZES. One slot per in-flight frame; a BLAS asks for its
    // compacted size in the frame it is built and reads the answer back later
    // WITHOUT waiting.
    VkQueryPoolCreateInfo cci{};
    cci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    cci.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    cci.queryCount = 64;
    vkCreateQueryPool(mVk, &cci, nullptr, &mCompactSizes);

    if (!makePipeline(err)) {
        close();
        return false;
    }
    // OUR FIXED RINGS MUST COVER THE PIN'S PIPELINE DEPTH (round 3, finding 4).
    // The retire logic reads the VaoManager's dynamic-buffer multiplier, but
    // the instance-slot ring and the descriptor rings are
    // sized by kFramesInFlight. They agree today (both 3) and would disagree
    // silently if the pin ever deepened: the gather would write the slot a
    // queued build is still reading. Refusing is the honest answer — the tier
    // is optional and the no-rays picture is supported.
    if (framesInFlight() > kReflectRing) {
        err = "rayquery: this device keeps " + std::to_string(framesInFlight()) +
              " frames in flight; the reflection pass' descriptor ring holds " +
              std::to_string(kReflectRing);
        return false;
    }
    if (framesInFlight() > kFramesInFlight) {
        err = "rayquery: the render system keeps " + std::to_string(framesInFlight()) +
              " frames in flight and this tier's rings hold " +
              std::to_string(kFramesInFlight);
        close();
        return false;
    }
    Ogre::LogManager::getSingleton().logMessage(
        "Jahshaka: hardware ray-query tier open (PHOTON R1)");
    // THE TIER'S COUNTERS made here, never in a frame (lab.monitor (4): a host-visible allocation inside the first
    // frames' hit-set fill was time no monitor row names)
    tierCountersAddress();
    startAhead();
    return true;
}

void RayQueryTier::startAhead() {
    // THE SHARED SAMPLERS FIRST, HERE (the setups read them; a worker never makes a shared object)
    std::string err;
    if (!ensureSamplers(err)) return;   // every setup would fail the same way: they run now, as before
    const auto launch = [this](AheadId id, bool (RayQueryTier::*make)(std::string &)) { launchAhead(id, make); };
    launch(kAheadReflect, &RayQueryTier::makeReflectPipelineNow);
    launch(kAheadCardGather, &RayQueryTier::makeCardGatherPipelineNow);
    launch(kAheadCardMovers, &RayQueryTier::makeCardMoverPipelineNow);
    launch(kAheadVoxelSides, &RayQueryTier::makeVoxelSidePipelineNow);
    // THE BLENDED PASSES' ESCAPES (PHOTON-7 10-4 R4): at the tier's open too, never later - a build launched when the
    // first blended material arrived let the picture change seconds after it rested (the cold driver compile: the
    // blended passes lit by the environment until it was done; gi.glass_escape's settle read that as the rest)
    launch(kAheadGlassEscape, &RayQueryTier::makeGlassEscapePipelinesNow);
}

void RayQueryTier::launchAhead(AheadId id, bool (RayQueryTier::*make)(std::string &)) {
    Ahead &a = mAhead[id];
    if (a.started) return;
    a.started = true;
    a.th = std::thread([this, &a, make]() {
        tAheadNotes = &a.notes;
        a.ok = (this->*make)(a.err);
        tAheadNotes = nullptr;
    });
}

void RayQueryTier::joinAhead(AheadId id) {
    Ahead &a = mAhead[id];
    if (!a.started || a.joined) return;
    const auto t0 = std::chrono::steady_clock::now();
    if (a.th.joinable()) a.th.join();
    a.joined = true;
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    // ITS BUILDS' LINES, in the order they were made, as the render thread would have written them
    for (const RayAheadNote &n : a.notes) {
        ++mPipelinesBuilt;
        mPipelineBuildMs += n.ms;
        if (n.r == VK_SUCCESS) ShaderCache::noteEnginePipeline(n.ms, n.name.c_str());
        Ogre::LogManager::getSingleton().logMessage(
            "rayquery: compute pipelines " + std::to_string(mPipelinesBuilt) + " built, this one (" + n.name + ") " +
            std::to_string(n.ms) + " ms, " + std::to_string(mPipelineBuildMs) + " ms in all" +
            (mDev && mDev->mPipelineCache ? " (through the device's pipeline cache)" : " (no pipeline cache)") +
            " - built ahead on a worker, the render thread waited " + std::to_string(waited) + " ms");
    }
    a.notes.clear();
}

bool RayQueryTier::makePipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].binding = 3;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (int i = 0; i < 4; ++i) {
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, 4u, 8u, mSetLayout, err, "rayquery/trace")) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mPipeLayout) != VK_SUCCESS) {
        err = "rayquery: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_raysSpv);
    smi.pCode = krq_raysSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mModule) != VK_SUCCESS) {
        err = "rayquery: vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mModule;
    cpi.stage.pName = "main";
    cpi.layout = mPipeLayout;
    if (createComputePipeline(cpi, &mPipeline, "rays") != VK_SUCCESS) {
        err = "rayquery: vkCreateComputePipelines failed";
        return false;
    }
    // THE POOL FROM THE LAYOUT (VkDescriptorPools.h): eight trace sets live at once.
    if (!plan.create(mVk, "rayquery/trace", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mDescPool, err))
        return false;
    return true;
}

/// THE TIER IS CLOSING WHILE `scene` LIVES (the no-rays switch, the engine's
/// shutdown): everything the scene's structures hold on the device dies NOW, behind
/// the idle device close() waited for. Called for every live scene before close().
void RayQueryTier::releaseScene(OgreScene *scene) {
    SceneAs *saP = asOf(scene);
    if (!mVk || !saP) return;
    SceneAs &sa = *saP;
    for (Blas &bl : sa.blas) {
        if (bl.as) mFn.destroyAccelerationStructure(mVk, bl.as, nullptr);
        dropBuffer(bl.storage);
        bl.mesh.reset();
    }
    if (sa.tlas) mFn.destroyAccelerationStructure(mVk, sa.tlas, nullptr);
    dropBuffer(sa.tlasStorage);
    dropBuffer(sa.tlasScratch);
    dropBuffer(sa.tlasOut);
    for (unsigned r = 0; r < kFramesInFlight; ++r) dropBuffer(sa.tlasIn[r]);
    dropBuffer(sa.alphaTable);
    for (unsigned r = 0; r < kFramesInFlight; ++r) dropBuffer(sa.hitTable[r]);
    // THE SKIN CACHES, destroyed outright (the device is idle); the GpuScene
    // side is not touched — the tables die with the scene, which is closing
    // the tier or going itself.
    for (auto &skv : sa.skins) {
        SceneAs::Skin &sk = skv.second;
        if (sk.as) mFn.destroyAccelerationStructure(mVk, sk.as, nullptr);
        sk.as = VK_NULL_HANDLE;
        dropBuffer(sk.storage);
        dropBuffer(sk.scratch);
        destroySkinCacheBuffer(sa.skinVao, sk.buf);
    }
    sa.skins.clear();
    dropSkinBuffers(sa);
    detail::monitor::forgetGpuOwner(&sa);
    scene->rayStructuresSlot().reset();
}

void RayQueryTier::close() {
    if (!mVk) return;
    // the ahead builds first: they write the members this destroys
    for (unsigned i = 0; i < kAheadCount; ++i) joinAhead(AheadId(i));
    // EVERY STRUCTURE DIES WHILE THE DEVICE IS STILL ALIVE. This runs from
    // OgreEngine::shutdownRayQuery, before Root is deleted — the same rule the
    // engine's MeshPtrs obey, and for the same reason.
    vkDeviceWaitIdle(mVk);
    for (auto &kv : mReflects) dropReflect(kv.second);
    mReflects.clear();
    // THE HIT LISTS (PHOTON-HIT-SHADE-1): the counters are Ogre UAV buffers (the
    // VaoManager is alive: close() runs before Root goes), the sets die with the
    // pool below, the buffers now (the device is idle).
    for (auto &kv : mHits) {
        HitView &hv = kv.second;
        if (hv.buf && mRs && mRs->getVaoManager()) mRs->getVaoManager()->destroyUavBuffer(hv.buf);
        hv.buf = nullptr;
        for (unsigned i = 0; i < kReflectRing; ++i) dropBuffer(hv.params[i]);
        dropBuffer(hv.readback);
    }
    mHits.clear();
    for (ReflectImage *d : { &mHitDummyIds, &mHitDummyColour, &mHitDummyDest, &mHitDummyHist, &mHitDummyState }) {
        if (d->view) vkDestroyImageView(mVk, d->view, nullptr);
        if (d->image) vkDestroyImage(mVk, d->image, nullptr);
        if (d->memory) vkFreeMemory(mVk, d->memory, nullptr);
        *d = ReflectImage();
    }
    mHitDummiesReady = false;
    mHitDummiesNeedInit = false;
    dropBuffer(mTierCounters);
    // THE SUN CONTACT's views (PHOTON-RAYS-1): each takes its registration
    // away first (the listener holds a raw texture pointer) and retires its
    // texture into the bin that is emptied below. The sets are DROPPED, not
    // retired — the pool is destroyed below and frees them (the gather's
    // close() rule, and its reason).
    for (auto &kv : mSunContacts) {
        for (unsigned i = 0; i < kReflectRing; ++i) kv.second.sets[i] = VK_NULL_HANDLE;
        dropSunContact(kv.second);
    }
    mSunContacts.clear();   // each one's registration went with it (dropSunContact)
    // THE AREA VISIBILITY's views (PHOTON-7 7-A): the sun contact's rule, word for word.
    for (auto &kv : mAreaVis) {
        for (unsigned i = 0; i < kReflectRing; ++i) kv.second.traceSets[i] = kv.second.resolveSets[i] = VK_NULL_HANDLE;
        dropAreaVis(kv.second);
    }
    mAreaVis.clear();
    // THE GATHER'S SHADER REGISTRATION DIES WITH THE TEXTURES IT NAMES
    // (GATHER-0's D1, kept): `ScreenProbeGather::close` frees every atlas and
    // takes every registration away, because `FogHlmsListener`'s map is a plain
    // pointer table the Hlms cannot see into — a map left non-empty here makes
    // the next colour pass bind a DESTROYED TextureGpu. Reachable: the no-rays
    // switch flipped while a scene is gathering takes `updateRayQuery`'s early
    // return, so the frame-head clear never runs and this teardown is the only
    // place left that knows.
    // THE BIN'S DESCRIPTOR SETS GO FIRST. A set the gather retired on a RESIZE
    // sits in `mRetireBin` naming the gather's OWN pool, and `close()` below
    // destroys that pool — freeing the set afterwards would hand Vulkan a dead
    // handle. The device is idle here, so nothing still reads them.
    for (Retired &r : mRetireBin)
        if (r.set && r.setPool) { vkFreeDescriptorSets(mVk, r.setPool, 1, &r.set); r.set = VK_NULL_HANDLE; }
    if (mGather) { mGather->close(); delete mGather; mGather = nullptr; }
    // (`mDummyArray` — SURFACE-CACHE-0's 1x1x6 stand-in for the card spike's
    // five bindings — went with the spike at SURFACE-CACHE-1b; nothing in the
    // gather or the reflect job binds a 2D ARRAY.)
    if (mDummyFlatArrayView) vkDestroyImageView(mVk, mDummyFlatArrayView, nullptr);
    mDummyFlatArrayView = VK_NULL_HANDLE;
    for (ReflectImage *d : { &mDummyCube, &mDummyVolume, &mDummyFlat, &mDummyIds }) {
        if (d->view) vkDestroyImageView(mVk, d->view, nullptr);
        if (d->image) vkDestroyImage(mVk, d->image, nullptr);
        if (d->memory) vkFreeMemory(mVk, d->memory, nullptr);
        *d = ReflectImage();
    }
    dropBuffer(mDummyStorage);
    mDummiesReady = false;
    mDummiesNeedClear = false;
    for (Retired &r : mRetireBin) {
        if (r.as) mFn.destroyAccelerationStructure(mVk, r.as, nullptr);
        dropBuffer(r.buf);
        if (r.set && r.setPool) vkFreeDescriptorSets(mVk, r.setPool, 1, &r.set);
        if (r.tex && mRs && mRs->getTextureGpuManager())
            mRs->getTextureGpuManager()->destroyTexture(r.tex);
        if (r.view) vkDestroyImageView(mVk, r.view, nullptr);
        if (r.img.view) vkDestroyImageView(mVk, r.img.view, nullptr);
        if (r.img.image) vkDestroyImage(mVk, r.img.image, nullptr);
        if (r.img.memory) vkFreeMemory(mVk, r.img.memory, nullptr);
    }
    mRetireBin.clear();
    // THE HIT WRITE-BACK'S POOL, after the bin's sets that name it are freed.
    if (mCompPool) vkDestroyDescriptorPool(mVk, mCompPool, nullptr);
    if (mCompPipeline) vkDestroyPipeline(mVk, mCompPipeline, nullptr);
    if (mCompModule) vkDestroyShaderModule(mVk, mCompModule, nullptr);
    if (mCompPipeLayout) vkDestroyPipelineLayout(mVk, mCompPipeLayout, nullptr);
    if (mCompSetLayout) vkDestroyDescriptorSetLayout(mVk, mCompSetLayout, nullptr);
    mCompPool = VK_NULL_HANDLE; mCompPipeline = VK_NULL_HANDLE; mCompModule = VK_NULL_HANDLE;
    mCompPipeLayout = VK_NULL_HANDLE; mCompSetLayout = VK_NULL_HANDLE;
    if (mSunPool) vkDestroyDescriptorPool(mVk, mSunPool, nullptr);
    if (mSunPipeline) vkDestroyPipeline(mVk, mSunPipeline, nullptr);
    if (mSunModule) vkDestroyShaderModule(mVk, mSunModule, nullptr);
    if (mSunPipeLayout) vkDestroyPipelineLayout(mVk, mSunPipeLayout, nullptr);
    if (mSunSetLayout) vkDestroyDescriptorSetLayout(mVk, mSunSetLayout, nullptr);
    if (mSunSampler) vkDestroySampler(mVk, mSunSampler, nullptr);
    if (mAreaPool) vkDestroyDescriptorPool(mVk, mAreaPool, nullptr);
    for (VkPipeline pl : { mAreaPipeline, mAreaResolvePipeline })
        if (pl) vkDestroyPipeline(mVk, pl, nullptr);
    for (VkShaderModule m : { mAreaModule, mAreaResolveModule })
        if (m) vkDestroyShaderModule(mVk, m, nullptr);
    for (VkPipelineLayout pl : { mAreaPipeLayout, mAreaResolvePipeLayout })
        if (pl) vkDestroyPipelineLayout(mVk, pl, nullptr);
    for (VkDescriptorSetLayout l : { mAreaSetLayout, mAreaResolveSetLayout })
        if (l) vkDestroyDescriptorSetLayout(mVk, l, nullptr);
    mAreaPool = VK_NULL_HANDLE;
    mAreaPipeline = mAreaResolvePipeline = VK_NULL_HANDLE;
    mAreaModule = mAreaResolveModule = VK_NULL_HANDLE;
    mAreaPipeLayout = mAreaResolvePipeLayout = VK_NULL_HANDLE;
    mAreaSetLayout = mAreaResolveSetLayout = VK_NULL_HANDLE;
    mSunPool = VK_NULL_HANDLE; mSunPipeline = VK_NULL_HANDLE; mSunModule = VK_NULL_HANDLE;
    mSunPipeLayout = VK_NULL_HANDLE; mSunSetLayout = VK_NULL_HANDLE;
    mSunSampler = VK_NULL_HANDLE;
    // THE CARD MOVERS' state: sets dropped with the pool (the sun contact's rule).
    for (auto *views : { &mCardMovers, &mCardStill, &mCardLamps }) {
        for (auto &kv : *views)
            for (unsigned i = 0; i < kReflectRing; ++i) {
                dropBuffer(kv.second.params[i]);
                dropBuffer(kv.second.records[i]);
            }
        views->clear();
    }
    if (mCmPool) vkDestroyDescriptorPool(mVk, mCmPool, nullptr);
    // THE CARDS' TLAS GATHER's state: sets dropped with the pool.
    for (auto &kv : mCardGather)
        for (unsigned i = 0; i < kReflectRing; ++i) {
            dropBuffer(kv.second.params[i]);
            dropBuffer(kv.second.records[i]);
            dropBuffer(kv.second.rowOfSlot[i]);
            dropBuffer(kv.second.counts[i]);
        }
    mCardGather.clear();
    dropBuffer(mCgZeroBlock);
    // THE BLENDED PASSES' ESCAPES' state (PHOTON-7 10-4 R4): sets dropped with the pool.
    for (auto &kv : mEscape) {
        dropBuffer(kv.second.accum);
        dropBuffer(kv.second.readback);
        for (EscapeList &l : kv.second.lists) {
            dropBuffer(l.list);
            dropBuffer(l.counts);
            for (unsigned i = 0; i < kReflectRing; ++i) {
                dropBuffer(l.params[i]);
                dropBuffer(l.addParams[i]);
                dropBuffer(l.rowOfSlot[i]);
            }
        }
    }
    mEscape.clear();
    // THE VOXEL SIDES' GATHER's state (10-2b): sets dropped with the pool.
    for (auto &kv : mVoxelSides)
        for (unsigned i = 0; i < kVoxelSideRing; ++i) {
            dropBuffer(kv.second.params[i]);
            dropBuffer(kv.second.rowOfSlot[i]);
            dropBuffer(kv.second.counts[i]);
        }
    mVoxelSides.clear();
    if (mVsPool) vkDestroyDescriptorPool(mVk, mVsPool, nullptr);
    if (mVsPipeline) vkDestroyPipeline(mVk, mVsPipeline, nullptr);
    if (mVsModule) vkDestroyShaderModule(mVk, mVsModule, nullptr);
    if (mVsPipeLayout) vkDestroyPipelineLayout(mVk, mVsPipeLayout, nullptr);
    if (mVsSetLayout) vkDestroyDescriptorSetLayout(mVk, mVsSetLayout, nullptr);
    mVsPool = VK_NULL_HANDLE; mVsPipeline = VK_NULL_HANDLE; mVsModule = VK_NULL_HANDLE;
    mVsPipeLayout = VK_NULL_HANDLE; mVsSetLayout = VK_NULL_HANDLE;
    if (mCgPool) vkDestroyDescriptorPool(mVk, mCgPool, nullptr);
    if (mCgPipeline) vkDestroyPipeline(mVk, mCgPipeline, nullptr);
    if (mCgModule) vkDestroyShaderModule(mVk, mCgModule, nullptr);
    if (mCgPipeLayout) vkDestroyPipelineLayout(mVk, mCgPipeLayout, nullptr);
    if (mCgSetLayout) vkDestroyDescriptorSetLayout(mVk, mCgSetLayout, nullptr);
    mCgPool = VK_NULL_HANDLE; mCgPipeline = VK_NULL_HANDLE; mCgModule = VK_NULL_HANDLE;
    mCgPipeLayout = VK_NULL_HANDLE; mCgSetLayout = VK_NULL_HANDLE;
    if (mEscPool) vkDestroyDescriptorPool(mVk, mEscPool, nullptr);
    if (mEscPipeline) vkDestroyPipeline(mVk, mEscPipeline, nullptr);
    if (mEscAddPipeline) vkDestroyPipeline(mVk, mEscAddPipeline, nullptr);
    if (mEscModule) vkDestroyShaderModule(mVk, mEscModule, nullptr);
    if (mEscAddModule) vkDestroyShaderModule(mVk, mEscAddModule, nullptr);
    if (mEscPipeLayout) vkDestroyPipelineLayout(mVk, mEscPipeLayout, nullptr);
    if (mEscAddPipeLayout) vkDestroyPipelineLayout(mVk, mEscAddPipeLayout, nullptr);
    if (mEscSetLayout) vkDestroyDescriptorSetLayout(mVk, mEscSetLayout, nullptr);
    if (mEscAddSetLayout) vkDestroyDescriptorSetLayout(mVk, mEscAddSetLayout, nullptr);
    mEscPool = VK_NULL_HANDLE;
    mEscPipeline = mEscAddPipeline = VK_NULL_HANDLE;
    mEscModule = mEscAddModule = VK_NULL_HANDLE;
    mEscPipeLayout = mEscAddPipeLayout = VK_NULL_HANDLE;
    mEscSetLayout = mEscAddSetLayout = VK_NULL_HANDLE;
    if (mTwPool) vkDestroyDescriptorPool(mVk, mTwPool, nullptr);
    if (mTwPipeline) vkDestroyPipeline(mVk, mTwPipeline, nullptr);
    if (mTwPipeLayout) vkDestroyPipelineLayout(mVk, mTwPipeLayout, nullptr);
    if (mTwModule) vkDestroyShaderModule(mVk, mTwModule, nullptr);
    if (mTwSetLayout) vkDestroyDescriptorSetLayout(mVk, mTwSetLayout, nullptr);
    mTwPool = VK_NULL_HANDLE; mTwPipeline = VK_NULL_HANDLE; mTwPipeLayout = VK_NULL_HANDLE;
    mTwModule = VK_NULL_HANDLE; mTwSetLayout = VK_NULL_HANDLE;
    if (mCmPipeline) vkDestroyPipeline(mVk, mCmPipeline, nullptr);
    if (mCmModule) vkDestroyShaderModule(mVk, mCmModule, nullptr);
    if (mCmPipeLayout) vkDestroyPipelineLayout(mVk, mCmPipeLayout, nullptr);
    if (mCmSetLayout) vkDestroyDescriptorSetLayout(mVk, mCmSetLayout, nullptr);
    mCmPool = VK_NULL_HANDLE; mCmPipeline = VK_NULL_HANDLE; mCmModule = VK_NULL_HANDLE;
    mCmPipeLayout = VK_NULL_HANDLE; mCmSetLayout = VK_NULL_HANDLE;
    if (mCardParityPool) vkDestroyDescriptorPool(mVk, mCardParityPool, nullptr);
    if (mCardParitySampler) vkDestroySampler(mVk, mCardParitySampler, nullptr);
    mCardParitySampler = VK_NULL_HANDLE;
    if (mCardParityPipeline) vkDestroyPipeline(mVk, mCardParityPipeline, nullptr);
    if (mCardParityModule) vkDestroyShaderModule(mVk, mCardParityModule, nullptr);
    if (mCardParityPipeLayout) vkDestroyPipelineLayout(mVk, mCardParityPipeLayout, nullptr);
    if (mCardParitySetLayout) vkDestroyDescriptorSetLayout(mVk, mCardParitySetLayout, nullptr);
    mCardParityPool = VK_NULL_HANDLE; mCardParityPipeline = VK_NULL_HANDLE;
    mCardParityModule = VK_NULL_HANDLE; mCardParityPipeLayout = VK_NULL_HANDLE;
    mCardParitySetLayout = VK_NULL_HANDLE;
    if (mMotionPool) vkDestroyDescriptorPool(mVk, mMotionPool, nullptr);
    if (mMotionPipeline) vkDestroyPipeline(mVk, mMotionPipeline, nullptr);
    if (mMotionPipeLayout) vkDestroyPipelineLayout(mVk, mMotionPipeLayout, nullptr);
    if (mMotionSetLayout) vkDestroyDescriptorSetLayout(mVk, mMotionSetLayout, nullptr);
    if (mMotionModule) vkDestroyShaderModule(mVk, mMotionModule, nullptr);
    if (mMotionSkinPipeline) vkDestroyPipeline(mVk, mMotionSkinPipeline, nullptr);
    if (mMotionSkinModule) vkDestroyShaderModule(mVk, mMotionSkinModule, nullptr);
    mMotionSkinPipeline = VK_NULL_HANDLE; mMotionSkinModule = VK_NULL_HANDLE;
    mMotionPool = VK_NULL_HANDLE; mMotionPipeline = VK_NULL_HANDLE;
    mMotionPipeLayout = VK_NULL_HANDLE; mMotionSetLayout = VK_NULL_HANDLE;
    mMotionModule = VK_NULL_HANDLE; mMotionFailed = false;
    if (mReflectPool) vkDestroyDescriptorPool(mVk, mReflectPool, nullptr);
    if (mReflectPipeline) vkDestroyPipeline(mVk, mReflectPipeline, nullptr);
    if (mReflectCompactPipeline) vkDestroyPipeline(mVk, mReflectCompactPipeline, nullptr);
    if (mFilterPipeline) vkDestroyPipeline(mVk, mFilterPipeline, nullptr);
    if (mFilterModule) vkDestroyShaderModule(mVk, mFilterModule, nullptr);
    if (mTemporalPipeline) vkDestroyPipeline(mVk, mTemporalPipeline, nullptr);
    if (mTemporalModule) vkDestroyShaderModule(mVk, mTemporalModule, nullptr);
    if (mResolvePipeline) vkDestroyPipeline(mVk, mResolvePipeline, nullptr);
    if (mResolveModule) vkDestroyShaderModule(mVk, mResolveModule, nullptr);
    if (mComposePipeline) vkDestroyPipeline(mVk, mComposePipeline, nullptr);
    if (mComposeModule) vkDestroyShaderModule(mVk, mComposeModule, nullptr);
    if (mReflectPipeLayout) vkDestroyPipelineLayout(mVk, mReflectPipeLayout, nullptr);
    if (mReflectSetLayout) vkDestroyDescriptorSetLayout(mVk, mReflectSetLayout, nullptr);
    if (mReflectModule) vkDestroyShaderModule(mVk, mReflectModule, nullptr);
    if (mPointSampler) vkDestroySampler(mVk, mPointSampler, nullptr);
    if (mLinearSampler) vkDestroySampler(mVk, mLinearSampler, nullptr);
    if (mWrapSampler) vkDestroySampler(mVk, mWrapSampler, nullptr);
    mWrapSampler = VK_NULL_HANDLE;
    mReflectPool = VK_NULL_HANDLE; mReflectPipeline = VK_NULL_HANDLE; mReflectCompactPipeline = VK_NULL_HANDLE;
    mReflectPipeLayout = VK_NULL_HANDLE; mReflectSetLayout = VK_NULL_HANDLE;
    mReflectModule = VK_NULL_HANDLE;
    mFilterPipeline = VK_NULL_HANDLE; mFilterModule = VK_NULL_HANDLE;
    mTemporalPipeline = VK_NULL_HANDLE; mTemporalModule = VK_NULL_HANDLE;
    mResolvePipeline = VK_NULL_HANDLE; mResolveModule = VK_NULL_HANDLE;
    mComposePipeline = VK_NULL_HANDLE; mComposeModule = VK_NULL_HANDLE;
    mPointSampler = VK_NULL_HANDLE; mLinearSampler = VK_NULL_HANDLE;
    if (mDescPool) vkDestroyDescriptorPool(mVk, mDescPool, nullptr);
    if (mPipeline) vkDestroyPipeline(mVk, mPipeline, nullptr);
    if (mPipeLayout) vkDestroyPipelineLayout(mVk, mPipeLayout, nullptr);
    if (mSetLayout) vkDestroyDescriptorSetLayout(mVk, mSetLayout, nullptr);
    if (mModule) vkDestroyShaderModule(mVk, mModule, nullptr);
    if (mCompactSizes) vkDestroyQueryPool(mVk, mCompactSizes, nullptr);
    mDescPool = VK_NULL_HANDLE;
    mPipeline = VK_NULL_HANDLE;
    mPipeLayout = VK_NULL_HANDLE;
    mSetLayout = VK_NULL_HANDLE;
    mModule = VK_NULL_HANDLE;
    mCompactSizes = VK_NULL_HANDLE;
    mVk = VK_NULL_HANDLE;
}

/// A SCENE IS GONE. Called from OgreScene::destroy(), which is the only place
/// that knows it — without that call (and there was none until audit round 2's
/// finding 2) every destroyed preview or thumbnail scene left its structures,
/// its instance buffer and the MeshPtrs it holds alive for the process's life
/// under a dangling key, and a recycled OgreScene ADDRESS inherited a dead
/// scene's structures: a TLAS built for someone else's items, a blasOf
/// pointing at meshes this scene never had, and an epoch that made the tier
/// think nothing had moved. That is exactly the aliasing the held MeshPtr
/// exists to prevent, arriving by another door.
///
/// Everything retires through the frame-tagged bin rather than behind a device
/// wait: a scene can be destroyed on any frame, and draining the GPU to free a
/// thumbnail's structures would be a stall the user feels.
void RayQueryTier::forgetScene(OgreScene *scene) {
    mForgottenReflects.erase(scene);   // (the shot-view fold's entries die with their scene)
    if (mGather) mGather->forgetScene(scene);
    SceneAs *saP = asOf(scene);
    if (!saP) return;
    SceneAs &sa = *saP;
    for (Blas &bl : sa.blas) {
        retire(bl.as, bl.storage);
        // THE MESH HOLD ENDS HERE, AND THAT IS CORRECT (round 3, finding 7).
        // A BUILT acceleration structure does not re-read its inputs — the
        // vertex and index buffers are consumed by the build, not referenced by
        // the result — so a structure still in the retire window needs no mesh.
        // The one case that would need the buffers alive is a build recorded
        // this frame and not yet executed, and the pin already covers it: a
        // VertexBufferPacked destroyed now is held by VaoManager's delayed
        // destruction for its own dynamic-buffer multiplier of frames.
        bl.mesh.reset();
    }
    if (sa.feedOn) scene->gpuScene().removeObserver(&sa.feed);
    sa.feedOn = false;
    retire(sa.tlas, sa.tlasStorage);
    retire(sa.tlasScratch);
    // The instance job's buffers and sets, through the retire bin (a build or a
    // dispatch recorded in a frame still in flight reads them).
    retire(sa.tlasOut);
    for (unsigned r = 0; r < kFramesInFlight; ++r) {
        retire(sa.tlasIn[r]);
        retireSet(sa.tlasSets[r], mTwPool);
        sa.tlasSets[r] = VK_NULL_HANDLE;
    }
    retire(sa.alphaTable);
    sa.alphaAddress = 0;
    for (unsigned r = 0; r < kFramesInFlight; ++r) { retire(sa.hitTable[r]); sa.hitTableCap[r] = 0u; }
    sa.hitTableAddress = 0;
    // THE SKIN CACHES (PHOTON-SKIN-1): each hands back its structure, its buffer
    // and its row block, and clears its node's override — the rows stop naming
    // a posed copy nobody will update again.
    for (auto &kv : sa.skins) dropSkin(scene, sa, kv.first, kv.second);
    sa.skins.clear();
    sa.skinUse.clear();
    dropSkinBuffers(sa);
    // The compaction slots this scene still owed a read are never going to be
    // read; hand them back or the ring leaks capacity until compaction stops
    // for EVERY scene (round 3, finding 1: a parked preview did exactly that).
    for (const size_t idx : sa.compactWaiting)
        if (idx < sa.blas.size() && sa.blas[idx].compactState == 1u)
            mCompactBusy &= ~(uint64_t(1) << sa.blas[idx].compactSlot);
    sa.compactWaiting.clear();
    detail::monitor::forgetGpuOwner(&sa);
    scene->rayStructuresSlot().reset();
    forgetCardMovers(scene);
    forgetCardGather(scene);
    forgetVoxelSides(scene);
}

// ---------------------------------------------------------------------------
// THE INSTANCES BY COMPUTE (ATOM-CPU-WALKS-1) — the change feed's side, the job's
// inputs and the dispatch. The job (rayquery/rq_tlas_write.comp, build-time SPIR-V) writes two
// instances per slot at fixed places; everything here is per CHANGED SLOT (the
// feed), per MESH (the inputs) or per rigged item — no loop over the instances.
// (It replaced the CPU instance writer — a walk of every slot into the mapped
// instance ring, 3-13 ms at 10k — which engine.tlas_compute held byte-equal to
// this path before it was deleted; the suite keeps the reference itself.)
//
// THE TRACED SET IS THE TABLE'S `kGpuRayTraced` — carries kVisibleBit or
// kMovableBit, shown, below the overlay queues — with a mesh. Every BLAS is
// VK_GEOMETRY_OPAQUE_BIT_KHR (a mesh's structure is shared by every item wearing
// it); an ALPHA-TESTED item's instance is FORCE_NO_OPAQUE and the ray queries
// test its candidates against its material's mask (REFLECT-MOVERS-2,
// jah_rq_alpha.glsl; the alpha table below). Editor
// furniture, the backdrop, the sun disc and distortion objects carry their own
// channel INSTEAD of kVisibleBit precisely so captures can exclude them.
//
// EVERY TRACED OBJECT IS WRITTEN TWICE (ATOM-FARBLAS-1, A5b §4 — ONE TLAS with
// explicit masks; two TLASes would double the build every frame). The masks
// (Types.h, `kRayMask*`):
//   * THE NEAR COPY over the mesh's LEVEL 0 (PHOTON-7 stage 6: ONE RAY LEVEL PER
//     MESH - the authored triangles, so a contact meets its reflection and the
//     traced geometry is the same wherever the camera stands), carrying the
//     per-consumer bits of audit C-15 — a shadow caster, a mover or still world —
//     and "near"; a shadow-casting mover's also kRayMaskMoverCaster (the surface
//     cache's mover-shadow launch, PHOTON-CARDS-4), a shadow-casting still
//     object's kRayMaskStillCaster (its still-world launch, ATOM-S3-CARDCAP).
//   * THE FAR COPY over the mesh's COARSEST level, carrying kRayMaskFar ALONE: only
//     a launch that asks for the far field sees it, so no ray is answered twice by
//     one object.
// A RIGGED ITEM (PHOTON-SKIN-1) is traced through ITS OWN structure, built from its
// skin cache — near and far — and its row is the cache's; one whose cache is not
// ready this frame is not written at all (never traced at the bind pose, audit C-5).
// `instanceCustomIndex` is the SLOT on both copies: a hit reads the object's entry
// with one fetch whichever copy it hit; the copy is told apart by the launch's MASK.
namespace {
/// THE LEVELS THE RAYS MAY TRACE: the near copy's level 0 and the far copy's coarsest, no deeper than the GPU scene
/// keeps (architecture audit D1): its level table, its geometry rows (geomRowIndex: eight a mesh) and the hit record's
/// 3-bit level. A chain deeper than eight takes level 7 as its far copy: more triangles than its error allows, the
/// right ones. Past it a far copy indexed the NEXT mesh's rows (gi.ray_levels).
constexpr uint32_t kRayLevels = detail::GpuScene::kLevelsPerMesh;
/// The inputs' layout (the shader's HDR_* / MESH_WORDS): a mesh's record is its two structures' addresses - the near
/// copy's (level 0) and the far copy's (the coarsest) - since PHOTON-7 stage 6 (it carried every level's).
constexpr uint32_t kTlasHeaderWords = 16u;
constexpr uint32_t kTlasMeshWords = 4u;
constexpr uint32_t kTlasSkinWords = 4u;
constexpr uint32_t kTlasThreadsPerGroup = 64u;   // rq_tlas_write.comp's local_size_x
VkDescriptorBufferInfo rawBufferInfo(Ogre::UavBufferPacked *b);   // below, with the hit list's
}   // namespace

void RayQueryTier::SceneAs::Feed::take(uint32_t slot, const Rec &r, int sign, bool withKeys) {
    if (r.rigged) {
        if (sign > 0) rigged.insert(slot); else rigged.erase(slot);
    }
    if (r.kind == 2u) {
        if (sign > 0) skinned.insert(slot); else skinned.erase(slot);
    }
    if (r.kind == 1u) {
        traced = sign > 0 ? traced + 1u : (traced ? traced - 1u : 0u);
        for (const uint32_t level : { 0u, r.farLevel }) {   // the near copy's level 0, the far copy's
            if (!withKeys) break;
            const BlasKey key{ r.mesh, level };
            if (sign > 0) {
                KeyRef &k = keys[key];
                if (++k.count == 1u) keyEvents.push_back({ key, r.meshIndex, true });
                k.meshIndex = r.meshIndex;
            } else {
                auto it = keys.find(key);
                if (it != keys.end() && (it->second.count <= 1u)) {
                    keys.erase(it);
                    keyEvents.push_back({ key, r.meshIndex, false });
                } else if (it != keys.end()) {
                    --it->second.count;
                }
            }
        }
        if (r.coarse > 0.0f) {
            if (sign > 0) {
                ++coarse[r.coarse];
            } else {
                auto it = coarse.find(r.coarse);
                if (it != coarse.end() && --it->second == 0u) coarse.erase(it);
            }
        }
    }
}

namespace {
/// THE HAND-OVER WIDTH, QUANTISED UPWARD (ENGINE-POLISH-PHOTON-2 fix round): to the next of 64 steps per
/// octave (the mantissa m in [0.5, 1) on a 1/128 grid: at most +1.56 %, always wider - the far query starts
/// that much earlier, never later; a monotone ceiling). The width is the coarsest level's bound times the
/// largest axis scale, a column length of the world matrix: a ROTATING mover's columns move by an ulp from
/// frame to frame, and an exact width would make every such mover owe the feed's ordered body every frame
/// (the record compares it exactly). Quantised, a rotation owes nothing unless the width crosses a step.
/// NO HYSTERESIS: a width within an ulp of a step boundary can flip under rotation and owe every frame (odds
/// about 2^-17 per mover) - accepted.
float quantiseHandOver(float w) {
    int e = 0;
    const float m = std::frexp(w, &e);   // w = m 2^e, m in [0.5, 1)
    return std::ldexp(std::ceil(m * 128.0f) / 128.0f, e);
}
}   // namespace

/// ONE SLOT'S CONTRIBUTION, from the entry as it is now. The predicates are the
/// old writer's, read from the same words: `kGpuRayTraced` (the conjunction every
/// consumer used to recompute), a mesh the table names, `kGpuSkinned` for a rigged
/// item (its own structure), the near copy level 0, the far copy the coarsest, and the far copy's hand-over width
/// (audit F2): the coarsest level's measured bound grown by the largest axis scale.
RayQueryTier::SceneAs::Feed::Rec RayQueryTier::SceneAs::Feed::measure(const detail::GpuInstance *now) const {
    Rec next;
    if (!now || !scene) return next;
    uint32_t flags = 0u, meshIndex = 0u;
    std::memcpy(&flags, &now->boundsMax[3], sizeof(flags));
    std::memcpy(&meshIndex, &now->boundsMin[3], sizeof(meshIndex));
    if (!(flags & detail::kGpuRayTraced)) return next;
    next.rigged = (flags & detail::kGpuSkinned) != 0u;
    const Ogre::MeshPtr &mp = scene->gpuScene().meshAt(meshIndex);
    if (mp && next.rigged) {
        next.kind = 2u;
    } else if (mp) {
        next.kind = 1u;
        next.mesh = mp.get();
        next.meshIndex = meshIndex;
        const uint32_t coarsest = std::min(coarsestLevelOf(mp.get()), kRayLevels - 1u);
        next.farLevel = coarsest;
        if (coarsest > 0u) {
            const std::vector<float> *b = scene->lodBoundsFor(mp.get());
            const float bound = (b && !b->empty()) ? b->back() : 0.0f;
            if (bound > 0.0f) {
                // THE GAP IS A DISTANCE FROM LEVEL 0 (the one origin law, Types.h rayOriginLevelLift): the coarsest
                // level's STORED bound bounds each simplification step against its input, and its drift from level 0
                // is within kLevelDriftOverBound of it (atom2.one_step_drift_bound, per cut and per cluster).
                const float grown = rayOriginLevelLift(bound * worldMaxAxisScale(now->world));
                if (std::isfinite(grown) && grown > 0.0f) next.coarse = quantiseHandOver(grown);
            }
        }
    }
    return next;
}

namespace {
/// The slot's alpha-table membership as the entry says it should be: traced and alpha-tested.
bool alphaWanted(const detail::GpuInstance *now) {
    uint32_t f = 0u;
    if (now) std::memcpy(&f, &now->boundsMax[3], sizeof(f));
    return (f & detail::kGpuRayTraced) && (f & detail::kGpuAlphaTested);
}
}   // namespace

void RayQueryTier::SceneAs::Feed::gpuSlotChanged(uint32_t slot, const detail::GpuInstance *now) {
    visits.fetch_add(1u, std::memory_order_relaxed);
    if (slot < laneStashed.size()) laneStashed[slot] = 0u;
    apply(slot, now, measure(now));
}

void RayQueryTier::SceneAs::Feed::gpuSlotsReserve(uint32_t count) {
    // Sized here, before the task, to their default cells (what the whole notify reads past them), so the lane
    // halves index their own slots and grow nothing.
    if (recs.size() < count) recs.resize(count);
    if (laneNext.size() < count) laneNext.resize(count);
    if (laneStashed.size() < count) laneStashed.resize(count, 0u);
    words.reserve(count);
    if (rows && rows->size() < count) rows->resize(count, detail::GpuScene::kNoGeomRow);
}

bool RayQueryTier::SceneAs::Feed::gpuSlotChangedLane(uint32_t slot, const detail::GpuInstance *now) {
    // ON A WORKER: reads the slot's own cells and the shared sets (nobody writes them inside the task); writes the
    // slot's stash cells alone. A slot past the pre-sized cells is the replay's, whole.
    if (slot >= recs.size() || slot >= laneStashed.size() || (rows && slot >= rows->size())) return true;
    visits.fetch_add(1u, std::memory_order_relaxed);
    const Rec next = measure(now);
    const Rec &was = recs[slot];
    bool same = was.same(next) && words.wordOf(slot) == (now ? now->raster[0] : detail::WordCounts::kNone) &&
                alphaWanted(now) == (alpha.count(slot) != 0u);
    if (same && rows) {
        // The near row the ordered body would write (a rigged slot keeps the gather's).
        const uint32_t row = (*rows)[slot];
        if (next.kind == 1u)
            same = row == detail::GpuScene::geomRowIndex(next.meshIndex, 0u, 0u);
        else if (next.kind != 2u || was.kind != 2u)
            same = row == detail::GpuScene::kNoGeomRow;
    }
    if (same) {
        laneStashed[slot] = 0u;
        return false;
    }
    laneNext[slot] = next;
    laneStashed[slot] = 1u;
    return true;
}

void RayQueryTier::SceneAs::Feed::gpuSlotChangedOwed(uint32_t slot, const detail::GpuInstance *now) {
    if (slot >= laneStashed.size() || !laneStashed[slot]) { gpuSlotChanged(slot, now); return; }
    laneStashed[slot] = 0u;
    apply(slot, now, laneNext[slot]);   // counted by the lane half
}

void RayQueryTier::SceneAs::Feed::apply(uint32_t slot, const detail::GpuInstance *now, const Rec &next) {
    {
        // THE CUT-OUTS (REFLECT-MOVERS-2): a traced alpha-tested slot has an alpha
        // table row; any other has none.
        // DIRTY ONLY WHEN THE SET CHANGES (decision 14.10: an insert of a member set it, so a moving
        // alpha-tested slot re-wrote the whole table every frame) - or, below, when a member's rows did.
        if (alphaWanted(now)) { if (alpha.insert(slot).second) alphaDirty = true; }
        else if (alpha.erase(slot)) alphaDirty = true;
    }
    const bool alphaMember = alpha.count(slot) != 0u;
    const uint32_t rowWas = rows && slot < rows->size() ? (*rows)[slot] : detail::GpuScene::kNoGeomRow;
    const Rec recWas = slot < recs.size() ? recs[slot] : Rec();
    words.set(slot, now ? now->raster[0] : detail::WordCounts::kNone);
    if (slot >= recs.size()) recs.resize(size_t(slot) + 1u);
    Rec &was = recs[slot];
    if (!was.sameKeys(next)) structDirty = true;
    // THE KEYS ONLY WHEN THEY CHANGED (ENGINE-UPGRADE-U5 stage 3): a slot that moved and kept its mesh
    // and its levels references the same structures - taking them out and back would pass their count
    // through zero and file a removal and an addition for nothing.
    const bool keysSame = was.kind == 1u && next.kind == 1u && was.mesh == next.mesh &&
                          was.farLevel == next.farLevel;
    take(slot, was, -1, !keysSame);
    take(slot, next, +1, !keysSame);
    // THE NEAR COPY'S ROW. A rigged slot's row is its skin cache's, written by the
    // gather from the ready caches (writeTlasInputs); it keeps the one it has here.
    if (rows) {
        if (slot >= rows->size()) rows->resize(size_t(slot) + 1u, detail::GpuScene::kNoGeomRow);
        if (next.kind == 1u)
            (*rows)[slot] = detail::GpuScene::geomRowIndex(next.meshIndex, 0u, 0u);
        else if (next.kind != 2u || was.kind != 2u)
            (*rows)[slot] = detail::GpuScene::kNoGeomRow;
    }
    // A MEMBER'S ROWS (the alpha table names its near row and its far copy's mesh and level).
    if (alphaMember && (rowWas != (rows && slot < rows->size() ? (*rows)[slot] : detail::GpuScene::kNoGeomRow) ||
                        recWas.kind != next.kind || recWas.meshIndex != next.meshIndex ||
                        recWas.farLevel != next.farLevel))
        alphaDirty = true;
    was = next;
}

void RayQueryTier::SceneAs::Feed::netKeyEvents() {
    if (keyEvents.size() < 2u) return;
    // A key's events alternate (its count crosses zero each time): an even run nets to nothing, an odd one to its
    // last event.
    struct Net { KeyEvent ev; int net = 0; };
    std::vector<Net> nets;
    std::unordered_map<BlasKey, size_t, BlasKeyHash> at;
    nets.reserve(keyEvents.size());
    for (const KeyEvent &ev : keyEvents) {
        auto it = at.find(ev.key);
        if (it == at.end()) {
            at.emplace(ev.key, nets.size());
            nets.push_back({ ev, ev.added ? 1 : -1 });
        } else {
            Net &n = nets[it->second];
            n.net += ev.added ? 1 : -1;
            n.ev = ev;
        }
    }
    keyEvents.clear();
    for (const Net &n : nets)
        if (n.net != 0) keyEvents.push_back(n.ev);
}

void RayQueryTier::SceneAs::Feed::gpuDigestInto(detail::GpuScene &g) const {
    const uint32_t n = g.slotCount();
    const unsigned char tag = 'F';
    g.digestFold(&tag, 1u);
    const Rec none;
    for (uint32_t s = 0; s < n; ++s) {
        const Rec &r = s < recs.size() ? recs[s] : none;
        const uint64_t mesh = uint64_t(reinterpret_cast<uintptr_t>(r.mesh));
        const uint32_t w[4] = { r.meshIndex, r.farLevel, r.kind, r.rigged ? 1u : 0u };
        g.digestFold(&mesh, sizeof(mesh));
        g.digestFold(w, sizeof(w));
        g.digestFold(&r.coarse, sizeof(r.coarse));
        const uint32_t word = words.wordOf(s);
        const uint32_t row = rows && s < rows->size() ? (*rows)[s] : detail::GpuScene::kNoGeomRow;
        g.digestFold(&word, sizeof(word));
        g.digestFold(&row, sizeof(row));
    }
    std::vector<std::pair<BlasKey, KeyRef>> ks(keys.begin(), keys.end());
    std::sort(ks.begin(), ks.end(), [](const auto &a, const auto &b) {
        return a.first.mesh != b.first.mesh ? std::less<const Ogre::Mesh *>()(a.first.mesh, b.first.mesh)
                                            : a.first.level < b.first.level;
    });
    for (const auto &kv : ks) {
        const uint64_t mesh = uint64_t(reinterpret_cast<uintptr_t>(kv.first.mesh));
        const uint32_t w[3] = { kv.first.level, kv.second.count, kv.second.meshIndex };
        g.digestFold(&mesh, sizeof(mesh));
        g.digestFold(w, sizeof(w));
    }
    for (const KeyEvent &ev : keyEvents) {
        const uint64_t mesh = uint64_t(reinterpret_cast<uintptr_t>(ev.key.mesh));
        const uint32_t w[3] = { ev.key.level, ev.meshIndex, ev.added ? 1u : 0u };
        g.digestFold(&mesh, sizeof(mesh));
        g.digestFold(w, sizeof(w));
    }
    for (const std::set<uint32_t> *set : { &skinned, &rigged, &alpha }) {
        const uint32_t size = uint32_t(set->size());
        g.digestFold(&size, sizeof(size));
        for (uint32_t s : *set) g.digestFold(&s, sizeof(s));
    }
    for (const auto &kv : coarse) {
        g.digestFold(&kv.first, sizeof(kv.first));
        g.digestFold(&kv.second, sizeof(kv.second));
    }
    for (const auto &kv : words.counts()) {
        g.digestFold(&kv.first, sizeof(kv.first));
        g.digestFold(&kv.second, sizeof(kv.second));
    }
    const uint32_t tail[6] = { traced, structDirty ? 1u : 0u, alphaDirty ? 1u : 0u, keysReset ? 1u : 0u,
                               words.changed() ? 1u : 0u, uint32_t(keyEvents.size()) };
    g.digestFold(tail, sizeof(tail));
}

void RayQueryTier::SceneAs::Feed::gpuSceneReset() {
    words.reset();
    recs.clear();
    keys.clear();
    skinned.clear();
    rigged.clear();
    alpha.clear();
    coarse.clear();
    traced = 0u;
    structDirty = true;
    keyEvents.clear();
    keysReset = true;
    alphaDirty = true;
    laneNext.clear();
    laneStashed.clear();
    if (rows) rows->clear();
}

/// THE JOB'S INPUTS: a header (counts, the mask and flag bits — the C++ constants
/// are the one source), one record per mesh-table entry (the near copy's level-0
/// structure address and the far copy's coarsest) re-written only when the BLAS set or the mesh set
/// moved, and the READY rigged slots (their skin structures, ascending by slot) —
/// the old writer's "a rigged slot whose cache is not ready is NOT WRITTEN".
bool RayQueryTier::writeTlasInputs(OgreScene *scene, SceneAs &sa, unsigned &skinReady, bool &changed,
                                   std::string &err) {
    detail::GpuScene &gs = scene->gpuScene();
    SceneAs::Feed &fd = sa.feed;
    const uint32_t meshes = gs.meshEntryCount();
    const size_t meshWords = size_t(kTlasHeaderWords) + size_t(meshes) * kTlasMeshWords;
    // ONE MESH'S RECORD: the near copy's structure address (level 0) and the far copy's (the coarsest). A hash
    // find each.
    const auto writeRecord = [&](uint32_t m, uint32_t *rec) {
        const Ogre::MeshPtr &mp = gs.meshAt(m);
        // THE ZERO-FILLED RECORD IS A ROW WRITTEN (decision 14.4: it returned before the count).
        if (!mp) { std::fill(rec, rec + kTlasMeshWords, 0u); ++sa.st.updateRows; return; }
        const uint32_t levels[2] = { 0u, std::min(coarsestLevelOf(mp.get()), kRayLevels - 1u) };
        for (uint32_t c = 0; c < 2u; ++c) {
            ++sa.st.updateLookups;
            auto it = sa.blasOf.find(BlasKey{ mp.get(), levels[c] });
            const VkDeviceAddress a =
                (it != sa.blasOf.end() && it->second < sa.blas.size()) ? sa.blas[it->second].address : 0u;
            rec[2u * c] = uint32_t(a & 0xFFFFFFFFu);
            rec[2u * c + 1u] = uint32_t(a >> 32u);
        }
        ++sa.st.updateRows;
    };
    // THE PER-MESH RECORDS ARE PATCHED, NOT REBUILT (ENGINE-UPGRADE-U5 stage 3). They used to be
    // re-written WHOLE - a hash find per mesh per level and the submesh walk of each - whenever the BLAS
    // set's version moved, which on a walk is every frame a level is built (56 % of the tier's update on
    // Bistro's B walk). Now a mesh's record is re-written when ITS structures changed (dirtyMeshRows: a
    // build, a compaction's new address, an eviction) and the whole table only when the mesh SET moved
    // (its serial) or a failure path asked (inputsNeedFull).
    const bool full = sa.inputsNeedFull || sa.tlasInWords.size() < kTlasHeaderWords ||
                      sa.recordedMeshAt.size() * kTlasMeshWords + kTlasHeaderWords > sa.tlasInWords.size();
    changed = false;
    if (!full && sa.inputsMeshSerial != gs.meshSetSerial()) {
        // THE MESH SET MOVED: the table grows or shrinks to the entry count (the tail is re-written below)
        // and every entry whose mesh changed is re-written - a pointer compare per entry, a record per change.
        const size_t had = sa.recordedMeshAt.size();
        sa.tlasInWords.resize(kTlasHeaderWords + had * kTlasMeshWords);   // drop the tail
        sa.tlasInWords.resize(meshWords, 0u);
        sa.recordedMeshAt.resize(meshes, nullptr);
        uint32_t rec[kTlasMeshWords];
        std::vector<const Ogre::Mesh *> writtenMeshes;
        for (uint32_t m = 0; m < meshes; ++m) {
            ++sa.st.updateVisits;   // the diff's pointer compare (the table names no changed entry)
            const Ogre::Mesh *mesh = gs.meshAt(m).get();
            if (m < had && sa.recordedMeshAt[m] == mesh) continue;
            if (m < had && sa.recordedMeshAt[m]) {
                ++sa.st.updateLookups;
                const auto it = sa.meshIndexOf.find(sa.recordedMeshAt[m]);
                if (it != sa.meshIndexOf.end() && it->second == m) sa.meshIndexOf.erase(it);
            }
            if (mesh) { ++sa.st.updateLookups; sa.meshIndexOf[mesh] = m; }
            writeRecord(m, rec);
            std::memcpy(sa.tlasInWords.data() + kTlasHeaderWords + size_t(m) * kTlasMeshWords, rec, sizeof(rec));
            sa.recordedMeshAt[m] = mesh;
            if (mesh) writtenMeshes.push_back(mesh);
            changed = true;
        }
        sa.inputsMeshSerial = gs.meshSetSerial();
        if (meshes != had) changed = true;
        // A record just written is current: the dirty pass below need not write it again.
        std::sort(sa.dirtyMeshRows.begin(), sa.dirtyMeshRows.end());
        sa.dirtyMeshRows.erase(std::unique(sa.dirtyMeshRows.begin(), sa.dirtyMeshRows.end()), sa.dirtyMeshRows.end());
        std::sort(writtenMeshes.begin(), writtenMeshes.end());
        for (const Ogre::Mesh *&mesh : sa.dirtyMeshRows)
            if (mesh && std::binary_search(writtenMeshes.begin(), writtenMeshes.end(), mesh)) mesh = nullptr;
    }
    if (full) {
        std::vector<uint32_t> &w = sa.tlasInScratch;
        w.assign(meshWords, 0u);
        for (uint32_t m = 0; m < meshes; ++m) writeRecord(m, w.data() + kTlasHeaderWords + size_t(m) * kTlasMeshWords);
        sa.meshIndexOf.clear();
        sa.recordedMeshAt.assign(meshes, nullptr);
        for (uint32_t m = 0; m < meshes; ++m)
            if (const Ogre::Mesh *mesh = gs.meshAt(m).get()) {
                ++sa.st.updateLookups;
                sa.meshIndexOf[mesh] = m;
                sa.recordedMeshAt[m] = mesh;
            }
        sa.inputsMeshSerial = gs.meshSetSerial();
        sa.inputsNeedFull = false;
        sa.tlasInWords.swap(w);          // the header and the tail are written below, in place
        sa.tlasInWords.resize(meshWords);
        changed = true;
    } else {
        // IN PLACE: only the dirty meshes' records, each compared before it is written. (A dropped tail
        // above leaves the table exactly the records; the tail is put back below.)
        if (sa.tlasInWords.size() < meshWords) sa.tlasInWords.resize(meshWords, 0u);
        uint32_t rec[kTlasMeshWords];
        for (const Ogre::Mesh *mesh : sa.dirtyMeshRows) {
            if (!mesh) continue;
            ++sa.st.updateLookups;
            const auto it = sa.meshIndexOf.find(mesh);
            if (it == sa.meshIndexOf.end() || it->second >= meshes) continue;
            writeRecord(it->second, rec);
            uint32_t *dst = sa.tlasInWords.data() + kTlasHeaderWords + size_t(it->second) * kTlasMeshWords;
            if (std::memcmp(dst, rec, sizeof(rec)) != 0) {
                std::memcpy(dst, rec, sizeof(rec));
                changed = true;
            }
        }
    }
    sa.dirtyMeshRows.clear();
    sa.inputsBlasVersion = sa.blasVersion;
    // THE HEADER, written in place.
    const uint32_t head[kTlasHeaderWords] = { gs.slotCount(), meshes, 0u, 0u,
                                              kRayMaskNear, kRayMaskCaster, kRayMaskMover, kRayMaskStill,
                                              kRayMaskMoverCaster, kRayMaskFar, detail::kGpuRayTraced,
                                              detail::kGpuCaster, detail::kGpuMover, detail::kGpuSkinned,
                                              kRayMaskStillCaster, detail::kGpuAlphaTested };
    // THE READY RIGGED SLOTS (the skin pass just ran: sa.skinUse is this frame's),
    // and their rows — O(rigged items): the tail, past the mesh records.
    std::vector<uint32_t> &tail = sa.tlasInScratch;
    tail.clear();
    skinReady = 0u;
    if (sa.geomRowOfSlot.size() < gs.slotCount())
        sa.geomRowOfSlot.resize(gs.slotCount(), detail::GpuScene::kNoGeomRow);
    for (const uint32_t slot : fd.skinned) {
        if (slot >= gs.slotCount()) continue;
        ++sa.st.updateLookups;   // decision 14.4: counted (one per rigged slot a moved frame)
        auto it = sa.skinUse.find(gs.entry(slot).ids[0]);
        if (it == sa.skinUse.end()) {
            if (sa.geomRowOfSlot[slot] != detail::GpuScene::kNoGeomRow && fd.alpha.count(slot)) fd.alphaDirty = true;
            sa.geomRowOfSlot[slot] = detail::GpuScene::kNoGeomRow;
            continue;
        }
        tail.push_back(slot);
        tail.push_back(uint32_t(it->second.first & 0xFFFFFFFFu));
        tail.push_back(uint32_t(it->second.first >> 32u));
        tail.push_back(0u);
        if (sa.geomRowOfSlot[slot] != it->second.second && fd.alpha.count(slot)) fd.alphaDirty = true;
        sa.geomRowOfSlot[slot] = it->second.second;
        ++skinReady;
    }
    // The rows past the slot count describe nothing (a freed tail).
    if (sa.geomRowOfSlot.size() > gs.slotCount()) sa.geomRowOfSlot.resize(gs.slotCount());
    uint32_t *words = sa.tlasInWords.data();
    for (uint32_t k = 0; k < kTlasHeaderWords; ++k) {
        const uint32_t v = k == 2u ? skinReady : head[k];
        if (words[k] != v) { words[k] = v; changed = true; }
    }
    const size_t oldTail = sa.tlasInWords.size() - meshWords;
    if (oldTail != tail.size() ||
        (!tail.empty() && std::memcmp(sa.tlasInWords.data() + meshWords, tail.data(), tail.size() * 4u) != 0)) {
        sa.tlasInWords.resize(meshWords);
        sa.tlasInWords.insert(sa.tlasInWords.end(), tail.begin(), tail.end());
        sa.st.updateRows += tail.size() / 4u;   // the tail's rows, re-written (decision 14.4)
        changed = true;
    }
    if (changed) ++sa.inputsVersion;
    (void)err;
    return true;
}

namespace {
/// The instance job's bindings: the GPU scene's table, the inputs, the output.
constexpr unsigned kTlasWriteBindings = 3u;
/// The pool's sets: a ring of kFramesInFlight per ray-traced scene, for this many
/// scenes (every drawn scene with rays on — the editor's, the player's, a preview's).
constexpr unsigned kTlasWriteMaxScenes = 32u;
constexpr unsigned kTlasWriteSets = kTlasWriteMaxScenes * kFramesInFlight;
}   // namespace

bool RayQueryTier::makeTlasWritePipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[kTlasWriteBindings] = {};
    for (unsigned i = 0; i < kTlasWriteBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kTlasWriteBindings, kTlasWriteSets, mTwSetLayout, err, "rayquery/tlas-write")) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mTwSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mTwPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed (the instance job)";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_tlasWriteSpv);
    smi.pCode = krq_tlasWriteSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mTwModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the instance job's build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mTwModule;
    cpi.stage.pName = "main";
    cpi.layout = mTwPipeLayout;
    if (createComputePipeline(cpi, &mTwPipeline, "tlas write") != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed (the instance job)";
        return false;
    }
    // THE POOL FROM THE LAYOUT (VkDescriptorPools.h).
    if (!plan.create(mVk, "rayquery/tlas-write", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mTwPool, err))
        return false;
    return true;
}

bool RayQueryTier::dispatchTlasWrite(OgreScene *scene, SceneAs &sa, VkCommandBuffer cmd, std::string &err) {
    detail::GpuScene &gs = scene->gpuScene();
    const uint32_t slots = gs.slotCount();
    if (mTwFailed) {
        err = "the instance job is unavailable";
        return false;
    }
    if (!mTwPipeline && !makeTlasWritePipeline(err)) {
        mTwFailed = true;
        return false;
    }
    // THE OUTPUT: two instances a slot, grown by doubling (never per frame); the
    // one it replaces goes through the retire bin.
    const uint32_t want = std::max(64u, 2u * slots);
    if (!sa.tlasOut.buffer || sa.tlasOutCap < want) {
        uint32_t cap = std::max(sa.tlasOutCap, 64u);
        while (cap < want) cap *= 2u;
        retire(sa.tlasOut);
        if (!makeBuffer(VkDeviceSize(cap) * sizeof(VkAccelerationStructureInstanceKHR),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                        false, true, sa.tlasOut, err))
            return false;
        sa.tlasOutCap = cap;
        sa.feed.structDirty = true;   // a new array: the build is full
    }
    if (!slots) return true;           // nothing to write; the build takes 0 primitives
    // THIS FRAME'S RING SLOT OF INPUTS (host-visible; a frame kFramesInFlight back
    // has finished reading it — Ogre's frame fence), re-written only when it holds
    // an older version of the words.
    const unsigned ring = frameNow() % kFramesInFlight;
    RawBuffer &in = sa.tlasIn[ring];
    const uint32_t words = uint32_t(sa.tlasInWords.size());
    if (!in.buffer || sa.tlasInCap[ring] < words) {
        uint32_t cap = std::max<uint32_t>(sa.tlasInCap[ring], 1024u);
        while (cap < words) cap *= 2u;
        retire(in);
        if (!makeBuffer(VkDeviceSize(cap) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, in,
                        err))
            return false;
        sa.tlasInCap[ring] = cap;
        sa.tlasInVersion[ring] = 0ull;
    }
    if (sa.tlasInVersion[ring] != sa.inputsVersion) {
        std::memcpy(in.mapped, sa.tlasInWords.data(), size_t(words) * sizeof(uint32_t));
        sa.tlasInVersion[ring] = sa.inputsVersion;
    }
    // THE SET, re-written every dispatch (the table and the output may have been
    // re-created by a grow): three buffer descriptors.
    if (!sa.tlasSets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mTwPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mTwSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &sa.tlasSets[ring]) != VK_SUCCESS) {
            sa.tlasSets[ring] = VK_NULL_HANDLE;
            err = "the instance job's descriptor pool is exhausted (more than " +
                  std::to_string(kTlasWriteMaxScenes) + " ray-traced scenes)";
            return false;
        }
    }
    Ogre::UavBufferPacked *table = gs.instanceBuffer();   // re-read: a grow re-creates it
    if (!table) {
        err = "the GPU scene has no instance table";
        return false;
    }
    VkDescriptorBufferInfo bi[kTlasWriteBindings] = {};
    bi[0] = rawBufferInfo(table);
    bi[1].buffer = in.buffer;
    bi[1].range = VkDeviceSize(sa.tlasInCap[ring]) * sizeof(uint32_t);
    bi[2].buffer = sa.tlasOut.buffer;
    bi[2].range = VkDeviceSize(sa.tlasOutCap) * sizeof(VkAccelerationStructureInstanceKHR);
    VkWriteDescriptorSet wds[kTlasWriteBindings] = {};
    for (unsigned i = 0; i < kTlasWriteBindings; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = sa.tlasSets[ring];
        wds[i].dstBinding = i;
        wds[i].descriptorCount = 1;
        wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wds[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(mVk, kTlasWriteBindings, wds, 0, nullptr);
    // THE EDGES IN: last frame's BUILD read the output (write-after-read), and this
    // frame's staging copies (the GPU scene's slots, the ray-level patches) wrote the
    // table the job reads. The inputs are host writes, visible at the submit.
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTwPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTwPipeLayout, 0, 1, &sa.tlasSets[ring], 0,
                            nullptr);
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatch(cmd, (slots + kTlasThreadsPerGroup - 1u) / kTlasThreadsPerGroup, 1u, 1u);
    // THE JOB -> THE BUILD: the instances are an acceleration-structure input read
    // through a device address.
    VkMemoryBarrier raw{};
    raw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    raw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    raw.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &raw, 0, nullptr, 0,
                         nullptr);
    ++sa.st.tlasJobDispatches;
    return true;
}

// ---------------------------------------------------------------------------
namespace {
/// One mesh's geometry descriptors, read straight out of Ogre's v2 buffers.
///
/// A VertexBufferPacked lives inside a shared POOL VkBuffer, so the geometry's
/// device address is "the pool's address, plus this buffer's own start inside
/// it, plus the position element's offset inside the vertex". That is exactly
/// the arithmetic Ogre's own draw performs — it binds every vertex buffer at
/// offset 0 and folds the start into baseVertex/firstIndex — so a BLAS built
/// this way reads the same triangles the raster does.
struct MeshGeometry {
    std::vector<VkAccelerationStructureGeometryKHR> geoms;
    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    std::vector<uint32_t> primCounts;
    unsigned triangles = 0;
};

/// `level` picks the VAO of the mesh's LOD chain (clamped to what a submesh
/// carries): every level shares level 0's vertex buffer and owns its own index
/// buffer (OgreMesh.cpp), so a level's BLAS is the same arithmetic over a
/// different index range.
bool describeMesh(const Ogre::Mesh *mesh, uint32_t level, MeshGeometry &out,
                  VkDeviceAddress (*addressOf)(void *, VkBuffer), void *self) {
    for (unsigned s = 0, e = unsigned(mesh->getNumSubMeshes()); s < e; ++s) {
        const Ogre::SubMesh *sub = mesh->getSubMesh(s);
        if (!sub || sub->mVao[Ogre::VpNormal].empty()) continue;
        const size_t levels = sub->mVao[Ogre::VpNormal].size();
        Ogre::VertexArrayObject *vao =
            sub->mVao[Ogre::VpNormal][std::min<size_t>(level, levels - 1u)];
        // ONLY INDEXED TRIANGLE LISTS. Everything this engine builds is one
        // (buildMeshV2), and a strip or an unindexed draw would need its own
        // geometry shape; skipping is the honest answer, not a silent wrong
        // triangle.
        if (!vao || vao->getOperationType() != Ogre::OT_TRIANGLE_LIST || !vao->getIndexBuffer())
            continue;
        size_t elemIdx = 0, elemOffset = 0;
        const Ogre::VertexElement2 *posElem =
            vao->findBySemantic(Ogre::VES_POSITION, elemIdx, elemOffset);
        if (!posElem || posElem->mType != Ogre::VET_FLOAT3) continue;
        Ogre::VertexBufferPacked *vb = vao->getVertexBuffers()[elemIdx];
        Ogre::IndexBufferPacked *ib = vao->getIndexBuffer();
        Ogre::VulkanBufferInterface *vbi =
            static_cast<Ogre::VulkanBufferInterface *>(vb->getBufferInterface());
        Ogre::VulkanBufferInterface *ibi =
            static_cast<Ogre::VulkanBufferInterface *>(ib->getBufferInterface());
        if (!vbi || !ibi) continue;
        const uint32_t tris = uint32_t(vao->getPrimitiveCount() / 3u);
        if (!tris) continue;

        VkAccelerationStructureGeometryKHR g{};
        g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;   // (W1: a pane candidate never repeats: T, not T^2) shared by every wearer; a cut-out is its INSTANCE (FORCE_NO_OPAQUE)
        g.geometry.triangles.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        g.geometry.triangles.vertexData.deviceAddress =
            addressOf(self, vbi->getVboName()) +
            VkDeviceSize(vb->_getFinalBufferStart()) * vb->getBytesPerElement() +
            VkDeviceSize(elemOffset);
        g.geometry.triangles.vertexStride = vb->getBytesPerElement();
        g.geometry.triangles.maxVertex = uint32_t(vb->getNumElements() - 1u);
        g.geometry.triangles.indexType = ib->getIndexType() == Ogre::IndexBufferPacked::IT_16BIT
                                             ? VK_INDEX_TYPE_UINT16
                                             : VK_INDEX_TYPE_UINT32;
        g.geometry.triangles.indexData.deviceAddress =
            addressOf(self, ibi->getVboName()) +
            VkDeviceSize(ib->_getFinalBufferStart()) * ib->getBytesPerElement() +
            VkDeviceSize(vao->getPrimitiveStart()) * ib->getBytesPerElement();
        VkAccelerationStructureBuildRangeInfoKHR r{};
        r.primitiveCount = tris;
        out.geoms.push_back(g);
        out.ranges.push_back(r);
        out.primCounts.push_back(tris);
        out.triangles += tris;
    }
    return !out.geoms.empty();
}
}   // namespace

bool RayQueryTier::ensureBlas(SceneAs &sa, const std::vector<BlasWant> &wants,
                              VkCommandBuffer cmd, unsigned &built, std::string &err) {
    built = 0;
    struct Job {
        Ogre::MeshPtr mesh;
        uint32_t level = 0;
        MeshGeometry geo;
        VkAccelerationStructureBuildGeometryInfoKHR build{};
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        size_t slot = 0;
    };
    std::vector<Job> jobs;
    jobs.reserve(wants.size());
    auto addrThunk = [](void *self, VkBuffer b) -> VkDeviceAddress {
        return static_cast<RayQueryTier *>(self)->addressOf(b);
    };
    for (const BlasWant &want : wants) {
        Job j;
        j.mesh = want.mesh;
        j.level = want.level;
        if (!describeMesh(want.mesh.get(), want.level, j.geo, addrThunk, this)) continue;
        j.build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        j.build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        // FAST TRACE + ALLOW COMPACTION: a mesh's BLAS is built once and traced
        // for the rest of the session, so the tree's quality is worth paying
        // for, and NVIDIA reports up to ~50 % of its memory back on compaction.
        j.build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                        VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
        j.build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        j.build.geometryCount = uint32_t(j.geo.geoms.size());
        j.sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) return true;

    VkDeviceSize scratchTotal = 0;
    const VkDeviceSize align = std::max<VkDeviceSize>(
        mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
    for (Job &j : jobs) {
        j.build.pGeometries = j.geo.geoms.data();
        mFn.getBuildSizes(mVk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &j.build,
                          j.geo.primCounts.data(), &j.sizes);
        scratchTotal = (scratchTotal + align - 1) / align * align + j.sizes.buildScratchSize;
    }

    // ONE SCRATCH ARENA for the whole batch, at offsets — the builds do not
    // alias, so there is NO serialising barrier between them (the spike's
    // shared buffer is why its 26 tiny builds cost 4.6-7.6 ms).
    RawBuffer scratch;
    if (!makeBuffer(scratchTotal + align, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, scratch,
                    err))
        return false;
    VkDeviceAddress cursor = (addressOf(scratch.buffer) + align - 1) / align * align;

    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs;
    builds.reserve(jobs.size());
    rangePtrs.reserve(jobs.size());
    // THE STRUCTURES ARE REGISTERED ONLY ONCE EVERY ONE OF THEM EXISTS (decision 14.6): a failure half
    // way used to leave the first ones in `blasOf` - their addresses published, their builds never
    // recorded - with `lastSeen` 0, which eviction skips for good. Now a failure retires what this call
    // made and registers nothing; a registered slot carries the frame it was made at.
    std::vector<Blas> made;
    made.reserve(jobs.size());
    const auto unmake = [&]() {
        for (Blas &m : made) retire(m.as, m.storage);
        made.clear();
    };
    for (Job &j : jobs) {
        Blas bl;
        bl.triangles = j.geo.triangles;
        bl.mesh = j.mesh;
        bl.level = j.level;
        if (!makeBuffer(j.sizes.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true,
                        bl.storage, err)) {
            dropBuffer(scratch);
            unmake();
            return false;
        }
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = bl.storage.buffer;
        ci.size = j.sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &bl.as) != VK_SUCCESS) {
            err = "rayquery: vkCreateAccelerationStructureKHR (BLAS) failed";
            dropBuffer(bl.storage);
            dropBuffer(scratch);
            unmake();
            return false;
        }
        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = bl.as;
        bl.address = mFn.getAsDeviceAddress(mVk, &ai);

        j.build.dstAccelerationStructure = bl.as;
        cursor = (cursor + align - 1) / align * align;
        j.build.scratchData.deviceAddress = cursor;
        cursor += j.sizes.buildScratchSize;

        // THE SLOT IS HANDED OUT HERE, in the loop that PUSHES the structure —
        // never in the collection loop (the spike wrote `mBlasOfMesh[mesh] =
        // mBlas.size()` while mBlas was still empty, gave every mesh slot 0,
        // and rendered the world as copies of the first mesh).
        j.slot = sa.blas.size() + made.size();
        made.push_back(std::move(bl));
        builds.push_back(j.build);
        rangePtrs.push_back(j.geo.ranges.data());
    }
    {
        const uint32_t now = frameNow();
        for (size_t k = 0; k < made.size(); ++k) {
            Job &j = jobs[k];
            made[k].lastSeen = now ? now : 1u;
            sa.blasOf[BlasKey{ j.mesh.get(), j.level }] = j.slot;
            sa.dirtyMeshRows.push_back(j.mesh.get());   // its record gains this level's address (stage 3)
            sa.blas.push_back(std::move(made[k]));
            ++built;
        }
        made.clear();
    }

    // OGRE'S PENDING UPLOADS FIRST, expressed as a barrier instead of a stall:
    // the staging copies that fill these very vertex buffers are recorded into
    // THIS command buffer (or an earlier submit on this queue), and a BLAS
    // built over vertex memory those copies have not written is built over
    // whatever was in that VRAM before — which does not fail, it takes
    // unbounded time (S3 §7 defect 3, which the spike answered with stall()).
    VkMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &pre, 0,
                         nullptr, 0, nullptr);
    mFn.cmdBuild(cmd, uint32_t(builds.size()), builds.data(), rangePtrs.data());

    // COMPACTION, PHASE A: ask for the compacted sizes of what we just built.
    // The answer is read WITHOUT waiting, several frames later (runCompaction).
    // BUILD -> BUILD, ALWAYS (finding 1): the builds above share one arena and
    // the reads below (compaction's size query, and the next frame's build over
    // the same structures) must not start until they have finished. This barrier
    // used to live INSIDE the compaction block, so a batch of more than
    // kCompactRing structures — which skips compaction — emitted no barrier at
    // all.
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    mb.dstAccessMask =
        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);

    // COMPACTION, PHASE A: ask for the compacted sizes of what we just built.
    // The slots come from a RING with a write cursor (finding 4). Writing
    // 0..n every time meant a second batch inside the read-back window
    // overwrote a pending batch's sizes, and phase B then compacted a structure
    // into a buffer sized for a different one. A batch the ring cannot hold
    // without trampling something still pending simply skips compaction — it
    // costs memory, never correctness.
    if (mCompactSizes) {
        std::vector<VkAccelerationStructureKHR> handles;
        handles.reserve(built);
        for (const Job &j : jobs)
            if (j.build.dstAccelerationStructure) handles.push_back(j.build.dstAccelerationStructure);
        const unsigned n = unsigned(handles.size());
        // A run of FREE slots, never "the cursor plus n". A batch that does not
        // fit simply goes uncompacted: that costs memory, never correctness,
        // and the slots come back as soon as their owners read them.
        const int base = takeCompactRun(n);
        if (base >= 0) {
            vkCmdResetQueryPool(cmd, mCompactSizes, unsigned(base), n);
            mFn.cmdWriteProperties(cmd, n, handles.data(),
                                   VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                   mCompactSizes, unsigned(base));
            unsigned q = unsigned(base);
            const uint32_t stamp = frameNow();
            for (const Job &j : jobs) {
                Blas &bl = sa.blas[j.slot];
                bl.compactState = 1;
                bl.compactSlot = q;
                bl.compactFrame = stamp;
                sa.compactWaiting.push_back(j.slot);
                mCompactBusy |= uint64_t(1) << q;
                ++q;
            }
        }
    }

    // The arena is freed once the frames that could still be reading it are
    // certainly done — never immediately (it is referenced by a build this
    // command buffer has not submitted yet).
    retire(scratch);
    return true;
}

// ---------------------------------------------------------------------------
// COMPACTION, PHASE B. Reads the sizes asked for earlier WITHOUT
// VK_QUERY_RESULT_WAIT_BIT (audit C-14: a wait on the frame thread is never
// allowed here) and, when the answer is there and worth it, copies the
// structure into a smaller one. The original is freed a few frames later,
// when nothing can still be reading it.
bool RayQueryTier::runCompaction(SceneAs &sa, VkCommandBuffer cmd) {
    if (!mCompactSizes) return false;
    bool compacted_any = false;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    // THE STRUCTURES WAITING ON A SIZE ANSWER, never every structure (decision 14.4: this walked the
    // whole BLAS vector every frame a compaction was owed).
    std::vector<size_t> &waiting = sa.compactWaiting;
    for (size_t w = 0; w < waiting.size();) {
        const size_t idx = waiting[w];
        if (idx >= sa.blas.size() || sa.blas[idx].compactState != 1u) {
            waiting[w] = waiting.back();
            waiting.pop_back();
            continue;
        }
        Blas &bl = sa.blas[idx];
        ++w;
        if (uint32_t(now - bl.compactFrame) < inFlight) continue;   // not submitted yet
        VkDeviceSize compacted = 0;
        const VkResult r = vkGetQueryPoolResults(mVk, mCompactSizes, bl.compactSlot, 1,
                                                 sizeof(compacted), &compacted, sizeof(compacted),
                                                 VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS || compacted == 0) continue;            // not available yet
        --w;
        waiting[w] = waiting.back();
        waiting.pop_back();
        bl.compactState = 2;
        mCompactBusy &= ~(uint64_t(1) << bl.compactSlot);   // answered: reusable
        if (compacted >= bl.storage.size) continue;                 // nothing to win
        RawBuffer small;
        std::string err;
        if (!makeBuffer(compacted, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false,
                        true, small, err))
            continue;
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = small.buffer;
        ci.size = compacted;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VkAccelerationStructureKHR dst = VK_NULL_HANDLE;
        if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &dst) != VK_SUCCESS) {
            dropBuffer(small);
            continue;
        }
        VkCopyAccelerationStructureInfoKHR cp{};
        cp.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
        cp.src = bl.as;
        cp.dst = dst;
        cp.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        mFn.cmdCopy(cmd, &cp);
        // The uncompacted structure is RETIRED, not freed: this frame's copy
        // still reads it, and so may a trace already queued.
        retire(bl.as, bl.storage);
        bl.as = dst;
        bl.storage = small;
        VkAccelerationStructureDeviceAddressInfoKHR ai{};
        ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        ai.accelerationStructure = dst;
        bl.address = mFn.getAsDeviceAddress(mVk, &ai);
        // The TLAS references the OLD address until it is rebuilt: the inputs'
        // version moves, so the job re-writes the instances and the build is full.
        ++sa.blasVersion;
        sa.dirtyMeshRows.push_back(bl.mesh.get());   // its record carries the new address (stage 3)
        compacted_any = true;
    }
    return compacted_any;
}

bool RayQueryTier::buildTlas(SceneAs &sa, VkCommandBuffer cmd, bool refit, std::string &err) {
    VkAccelerationStructureGeometryKHR geom{};
    geom.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;   // (W1)
    geom.geometry.instances.sType =
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geom.geometry.instances.arrayOfPointers = VK_FALSE;
    // THE INSTANCE JOB'S OUTPUT, read in place.
    geom.geometry.instances.data.deviceAddress = sa.tlasOut.buffer ? addressOf(sa.tlasOut.buffer) : 0u;

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    // ALLOW_UPDATE costs tree quality and memory, and is useless unless a refit
    // is actually going to be taken (finding 13). The default is a full rebuild
    // — NVIDIA's own guidance for a TLAS, 0.21-0.24 ms at 8,001 instances — so
    // the flag rides the same switch the refit does.
    const bool updatable = preferRefit();
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (updatable) build.flags |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    // An UPDATE needs a source built with ALLOW_UPDATE and the SAME flags: a tree
    // built while the arm was off is rebuilt the frame it turns on.
    if (!updatable || !sa.tlasUpdatable) refit = false;
    build.mode = refit ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                       : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geom;

    // SIZE TO THE CAPACITY, NOT THE COUNT (finding 6). A build may use FEWER
    // primitives than the size query was made for, so asking once per CAPACITY
    // step and building with the live count is legal and means adding objects
    // one at a time does not reallocate — and therefore does not drain the GPU
    // — on every single add. The storage only grows when the capacity does.
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    const uint32_t sizeFor = std::max(sa.tlasOutCap, sa.tlasPrimitives);
    mFn.getBuildSizes(mVk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &sizeFor,
                      &sizes);

    if (!sa.tlas || sa.tlasStorage.size < sizes.accelerationStructureSize) {
        if (sa.tlas) {
            // RETIRED, not waited on: a queued trace may still be reading it.
            retire(sa.tlas, sa.tlasStorage);
            retire(sa.tlasScratch);
        }
        if (!makeBuffer(sizes.accelerationStructureSize,
                        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true,
                        sa.tlasStorage, err))
            return false;
        VkAccelerationStructureCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        ci.buffer = sa.tlasStorage.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &sa.tlas) != VK_SUCCESS) {
            err = "rayquery: vkCreateAccelerationStructureKHR (TLAS) failed";
            return false;
        }
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;   // nothing to update yet
        refit = false;
    }
    const VkDeviceSize align = std::max<VkDeviceSize>(
        mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
    const VkDeviceSize need = std::max(sizes.buildScratchSize, sizes.updateScratchSize) + align;
    if (sa.tlasScratch.size < need) {
        retire(sa.tlasScratch);      // a queued build may still be reading it
        if (!makeBuffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, sa.tlasScratch, err))
            return false;
    }
    VkDeviceAddress scratchAddr = (addressOf(sa.tlasScratch.buffer) + align - 1) / align * align;
    build.dstAccelerationStructure = sa.tlas;
    if (build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR)
        build.srcAccelerationStructure = sa.tlas;
    build.scratchData.deviceAddress = scratchAddr;

    VkAccelerationStructureBuildRangeInfoKHR range{};
    range.primitiveCount = sa.tlasPrimitives;
    const VkAccelerationStructureBuildRangeInfoKHR *rangePtr = &range;

    // WHAT THIS BUILD MUST WAIT FOR — all of it (finding 1). The old barrier
    // named only a previous TRACE (compute, AS read), and missed three writers:
    //
    //   * LAST FRAME'S TLAS BUILD, which wrote this same structure and this
    //     same scratch buffer. A write-after-write across command buffers is
    //     not ordered for free.
    //   * THE COMPACTION COPY recorded into THIS command buffer a few lines
    //     earlier (runCompaction), which writes acceleration-structure memory.
    //   * THE BLAS BUILDS of this same frame, whose structures this build
    //     reads through the instance array's references.
    //
    // So the source is both stages and both accesses; the destination stays the
    // build. It costs nothing on a frame where none of them happened.
    VkMemoryBarrier pre{};
    pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    pre.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                        VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    // ...AND THE FRAGMENT STAGE (PHOTON-7 10-4 R9): the colour passes' Forward+ lamps trace this TLAS by its device
    // address (JahLampRay_piece_ps.any), last frame's among them - a write-after-read the validation layer CANNOT see
    // (no descriptor names the structure), so this barrier is set by hand.
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &pre, 0,
                         nullptr, 0, nullptr);
    mFn.cmdBuild(cmd, 1, &build, &rangePtr);
    // BUILD -> TRACE. Ogre's BarrierSolver has no vocabulary for an
    // acceleration structure at all, so this one is ours by necessity, not by
    // preference. (Every TEXTURE this tier's consumers write is a different
    // matter: those go to the solver through assumeTransition.)
    VkMemoryBarrier post{};
    post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    post.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    // (the FRAGMENT stage too: the Forward+ lamps' rays, PHOTON-7 10-4 R9 - by the structure's address, invisible to the
    // validation layer, so set by hand; every raster pass that reads it follows this build in the frame)
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &post, 0,
                         nullptr, 0, nullptr);

    sa.st.tlasBytes = sizes.accelerationStructureSize;
    sa.st.lastWasRefit = (build.mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR);
    if (!sa.st.lastWasRefit) sa.tlasUpdatable = updatable;
    if (sa.st.lastWasRefit) ++sa.st.tlasRefits; else ++sa.st.tlasBuilds;
    return true;
}

// ---------------------------------------------------------------------------
// THE GPU SKIN CACHE (PHOTON-SKIN-1, RY-R4; SkinCache.h has the design).
namespace {
/// A UAV binding for the skin job (the voxel gather's shape).
Ogre::DescriptorSetUav::BufferSlot skinSlot(Ogre::UavBufferPacked *buffer,
                                            Ogre::ResourceAccess::ResourceAccess access) {
    Ogre::DescriptorSetUav::BufferSlot slot = Ogre::DescriptorSetUav::BufferSlot::makeEmpty();
    slot.buffer = buffer;
    slot.offset = 0;
    slot.sizeBytes = 0;
    slot.access = access;
    return slot;
}
constexpr uint32_t kSkinThreadsPerGroup = 64u;   // the job's threads_per_group x
}  // namespace

void RayQueryTier::dropSkin(OgreScene *scene, SceneAs &sa, uint32_t node, SceneAs::Skin &sk) {
    if (sk.as) retire(sk.as, sk.storage);
    retire(sk.scratch);
    sk.address = 0;
    sk.built = false;
    destroySkinCacheBuffer(sa.skinVao, sk.buf);
    if (scene && scene->gpuScene().live()) {
        detail::GpuScene &gs = scene->gpuScene();
        if (gs.skinRowOf(node) != detail::GpuScene::kNoGeomRow) {
            gs.setSkinRow(node, detail::GpuScene::kNoGeomRow);
            // The override goes with the cache: the node's entry is re-composed
            // (if the node still has an Item) on the next scan.
            scene->markGpuNodeDirty(node);
        }
        if (sk.rowBlock != 0xFFFFFFFFu) gs.releaseRowBlock(sk.rowBlock);
    }
    sk.rowBlock = sk.row = 0xFFFFFFFFu;
}

void RayQueryTier::dropSkinBuffers(SceneAs &sa) {
    if (sa.skinVao) {
        if (sa.skinJobs) sa.skinVao->destroyUavBuffer(sa.skinJobs);
        if (sa.skinPalette) sa.skinVao->destroyUavBuffer(sa.skinPalette);
    }
    sa.skinJobs = sa.skinPalette = nullptr;
    sa.skinJobCap = sa.skinPaletteCap = 0;
}

/// A POSE THAT STOPPED LAST FRAME STILL OWES ITS SETTLE (REFLECT-EDGE-2, measured).
/// The frame a pose stops is a still frame by every test updateScene makes, and its
/// early returns never ran the skin pass' copy-only record: the previous-pose slice
/// kept the LAST MOVING pose for as long as the item stood still, so every ray hit on
/// a parked character read as a pose step (6 cm and more on gi.reflect_mover's parked
/// skinned sphere — 60-84 % of its reflected texels "moving" every frame and the mean
/// restarting on 8 % of them per frame). The settle writes no structure and no
/// instance, so it runs alone on such a frame: no gather, no build.
void RayQueryTier::settleStillSkins(OgreScene *scene, SceneAs &sa) {
    bool owed = false;
    for (const auto &kv : sa.skins)
        if (kv.second.prevBehind) { owed = true; break; }
    if (!owed) return;
    std::string err;
    VkCommandBuffer cmd = frameCmd();
    if (cmd && !skinPass(scene, sa, cmd, err) && !err.empty())
        Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
}

bool RayQueryTier::skinPass(OgreScene *scene, SceneAs &sa, VkCommandBuffer &cmd, std::string &err) {
    sa.skinUse.clear();
    sa.st.skinLastItems = 0;
    sa.st.skinLastVertices = 0;
    for (auto &kv : sa.skins) kv.second.seen = false;
    detail::GpuScene &gs = scene->gpuScene();
    if (!gs.live()) return true;

    // 1. THE RIGGED TRACED SET, kept by the change feed (ATOM-CPU-WALKS-1; it used
    //    to be a walk of the GPU scene's mirror every moving frame), ascending by
    //    slot as that walk visited it. The flags word is the one place the
    //    predicates live; kGpuRayTraced no longer excludes a rigged item.
    // (The slot's item facts through the GPU-scene view: its node, Item, rig
    // generation and pose serial — the shareSkeleton master's pushes folded in.)
    struct Want { uint32_t slot = 0; OgreScene::GpuSlotView view; };
    std::vector<Want> wants;
    for (const uint32_t i : sa.feed.rigged) {
        Want wt;
        wt.slot = i;
        if (i >= gs.slotCount() || !scene->gpuSlotView(i, wt.view)) continue;
        if (!wt.view.item || !wt.view.item->getSkeletonInstance()) continue;
        wants.push_back(wt);
    }
    if (wants.empty()) return true;

    Ogre::VaoManager *vao = mRs ? mRs->getVaoManager() : nullptr;
    Ogre::HlmsManager *hm = Ogre::Root::getSingletonPtr() ? Ogre::Root::getSingleton().getHlmsManager()
                                                          : nullptr;
    Ogre::HlmsCompute *hc = hm ? hm->getComputeHlms() : nullptr;
    Ogre::HlmsComputeJob *job = hc ? hc->findComputeJobNoThrow("Jahshaka/SkinCache") : nullptr;
    if (!vao || !vao->supportsBufferDeviceAddress()) {
        sa.st.skinReason = "no buffer device addresses on this device";
        return true;
    }
    if (!job) {
        sa.st.skinReason = "the Jahshaka/SkinCache job is missing from the staged media";
        err = sa.st.skinReason;
        return false;
    }
    if (sa.skinVao && sa.skinVao != vao) dropSkinBuffers(sa);
    sa.skinVao = vao;
    sa.st.skinReason.clear();

    // 2. RECONCILE: a cache per rigged traced item, created on first sight (or
    //    when the Item behind the node was rebuilt), marked seen; the ones not
    //    seen are dropped AFTER the gather (updateScene).
    struct Dirty { uint32_t node = 0; SceneAs::Skin *sk = nullptr; unsigned long long poseSerial = 0; uint32_t slot = 0; };
    std::vector<Dirty> dirty;
    // THE SETTLE LIST (REFLECT-MOVERS-2): caches whose pose stopped last frame —
    // their previous-pose slice is copied level with the posed vertices, no skinning.
    std::vector<SceneAs::Skin *> settle;
    bool rowsStaged = false;
    for (const Want &wt : wants) {
        const uint32_t node = wt.view.node;
        auto it = sa.skins.find(node);
        // THE IDENTITY, AND THE SOURCE IT WAS BUILT OVER (a mismatch in any of them
        // drops the cache and makes a new one this pass): a refit over an index
        // buffer the mesh released, or a job over a source with fewer vertices
        // than the cache, is a read of freed or foreign memory — an Xid, never a
        // validation error.
        const Ogre::VertexArrayObject *liveVao =
            (wt.view.item->getMesh() && wt.view.item->getMesh()->getNumSubMeshes() &&
             !wt.view.item->getMesh()->getSubMesh(0)->mVao[Ogre::VpNormal].empty())
                ? wt.view.item->getMesh()->getSubMesh(0)->mVao[Ogre::VpNormal][0]
                : nullptr;
        if (it != sa.skins.end() &&
            (it->second.item != wt.view.item ||
             it->second.mesh != wt.view.item->getMesh().get() ||
             it->second.rigGeneration != wt.view.rigGeneration || !liveVao ||
             liveVao->getIndexBuffer() != it->second.indices ||
             liveVao->getVertexBuffers().empty() ||
             uint32_t(liveVao->getVertexBuffers()[0]->getNumElements()) != it->second.buf.vertexCount)) {
            dropSkin(scene, sa, node, it->second);
            sa.skins.erase(it);
            it = sa.skins.end();
        }
        if (it == sa.skins.end()) {
            SceneAs::Skin sk;
            sk.item = wt.view.item;
            sk.mesh = wt.view.item->getMesh().get();
            sk.rigGeneration = wt.view.rigGeneration;
            sk.indices = liveVao ? liveVao->getIndexBuffer() : nullptr;
            std::string why;
            if (!createSkinCacheBuffer(vao, sk.item, sk.buf, why)) {
                // Not traced — and said so once per item, never at bind pose.
                sa.st.skinReason = why;
                continue;
            }
            std::vector<std::vector<uint32_t>> rows;
            sk.rowBlock = gs.acquireRowBlock();
            if (sk.rowBlock == detail::GpuScene::kNoMesh ||
                !describeSkinCacheRows(vao, sk.item, sk.buf, rows) || rows.empty() ||
                rows[0].size() != detail::GpuScene::kGeomRowWords) {
                sa.st.skinReason = "the skin cache's geometry rows could not be described";
                if (sk.rowBlock != detail::GpuScene::kNoMesh) gs.releaseRowBlock(sk.rowBlock);
                destroySkinCacheBuffer(vao, sk.buf);
                continue;
            }
            for (size_t l = 0; l < rows.size() && l < detail::GpuScene::kLevelsPerMesh; ++l)
                if (rows[l].size() == detail::GpuScene::kGeomRowWords)
                    gs.stageGeomRow(detail::GpuScene::geomRowIndex(sk.rowBlock, uint32_t(l), 0u),
                                    rows[l].data());
            sk.row = detail::GpuScene::geomRowIndex(sk.rowBlock, 0u, 0u);
            gs.setSkinRow(node, sk.row);
            scene->markGpuNodeDirty(node);
            rowsStaged = true;
            it = sa.skins.emplace(node, std::move(sk)).first;
        }
        SceneAs::Skin &sk = it->second;
        sk.seen = true;
        // THE POSE SERIAL: bumped by every pose push (clip time, weights, manual
        // bones — OgreScene::noteNodePosed). A walk moves the node, not the pose,
        // and costs nothing here: the cache is in the item's LOCAL space.
        const unsigned long long serial = wt.view.poseSerial;
        if (!sk.skinned || !sk.built || serial != sk.poseSerial) dirty.push_back({ node, &sk, serial, wt.slot });
        else if (sk.prevBehind) settle.push_back(&sk);
    }
    // A row not on the device is a zero address to the job (the ATOM-VOXEL-2 Xid):
    // the new rows go up NOW, before anything below binds the table.
    if (rowsStaged) gs.flushGeomRows();

    if (!dirty.empty() || !settle.empty()) {
        // 3. THE PALETTE: Ogre's own bone matrices for the renderable — the SAME
        //    `SkeletonInstance::_getBoneFullTransform` values, in the SAME
        //    blend-index order, that HlmsPbs::fillBuffersForV2 streams into its
        //    per-pass buffer for the vertex shader (PREMISE 1's verdict: that
        //    buffer is a per-PASS ring written only for a DRAWN renderable, at an
        //    offset only the draw knows — a character off screen, the one a
        //    mirror or a shadow ray most needs, has no palette in it at all — so
        //    the values are taken from their source instead of the buffer) —
        //    taken back into the item's local space by the node's inverse world.
        std::vector<float> palette;
        std::vector<SkinJobRecord> jobs;
        uint32_t maxVerts = 0u;
        for (Dirty &d : dirty) {
            const Ogre::Item *item = d.sk->item;
            Ogre::SkeletonInstance *skel = item->getSkeletonInstance();
            const Ogre::SubItem *sub = item->getNumSubItems() ? item->getSubItem(0) : nullptr;
            const Ogre::RenderableAnimated::IndexMap *map =
                sub ? sub->getBlendIndexToBoneIndexMap() : nullptr;
            Ogre::Node *parent = item->getParentNode();
            if (!skel || !map || map->empty() || !parent) { d.sk = nullptr; continue; }
            const Ogre::Matrix4 invWorld = parent->_getFullTransform().inverseAffine();
            SkinJobRecord r;
            r.sourceRow = gs.meshIndex(item->getMesh().get()) == detail::GpuScene::kNoMesh
                              ? detail::GpuScene::kNoGeomRow
                              : detail::GpuScene::geomRowIndex(gs.meshIndex(item->getMesh().get()), 0u, 0u);
            if (r.sourceRow == detail::GpuScene::kNoGeomRow) { d.sk = nullptr; continue; }
            r.vertexCount = d.sk->buf.vertexCount;
            r.paletteBase = uint32_t(palette.size() / 4u);
            r.cacheAddressLo = uint32_t(d.sk->buf.address & 0xFFFFFFFFull);
            r.cacheAddressHi = uint32_t(d.sk->buf.address >> 32u);
            r.tangentOffset = d.sk->buf.tangentOffset;
            r.blendOffsets = (d.sk->buf.blendIndexOffset & 0xFFFFu) |
                             ((d.sk->buf.blendWeightOffset & 0xFFFFu) << 16u);
            r.boneCount = uint32_t(map->size()) | (d.sk->skinned ? 0u : kSkinJobFirst);
            for (size_t b = 0; b < map->size(); ++b) {
                // store4x3, not streamTo4x3: the stream form is a non-temporal
                // store meant for a mapped GPU buffer; this is a stack copy.
                alignas(16) float m[12];
                skel->_getBoneFullTransform((*map)[b]).store4x3(m);
                const Ogre::Matrix4 world(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8],
                                          m[9], m[10], m[11], 0.0f, 0.0f, 0.0f, 1.0f);
                const Ogre::Matrix4 local = invWorld.concatenateAffine(world);
                for (int row = 0; row < 3; ++row)
                    for (int col = 0; col < 4; ++col) palette.push_back(float(local[row][col]));
            }
            jobs.push_back(r);
            maxVerts = std::max(maxVerts, r.vertexCount);
        }
        // THE SETTLE RECORDS: the slice takes the posed vertices, nothing is skinned
        // (the job reads neither the row nor the palette for one).
        for (SceneAs::Skin *sk : settle) {
            SkinJobRecord r;
            r.sourceRow = 0u;
            r.vertexCount = sk->buf.vertexCount;
            r.cacheAddressLo = uint32_t(sk->buf.address & 0xFFFFFFFFull);
            r.cacheAddressHi = uint32_t(sk->buf.address >> 32u);
            r.boneCount = kSkinJobSettle;
            jobs.push_back(r);
            maxVerts = std::max(maxVerts, r.vertexCount);
        }
        // A settle-only frame still binds a palette: one identity row set.
        if (palette.empty()) palette.assign({ 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 });

        if (!jobs.empty()) {
            // 4. THE INPUTS, grown by doubling and uploaded (Ogre's staging copy,
            //    ordered in the command stream before the dispatch that reads them).
            const uint32_t paletteRows = uint32_t(palette.size() / 4u);
            if (paletteRows > sa.skinPaletteCap) {
                uint32_t cap = std::max(sa.skinPaletteCap, 1024u);
                while (cap < paletteRows) cap *= 2u;
                if (sa.skinPalette) vao->destroyUavBuffer(sa.skinPalette);
                sa.skinPalette = vao->createUavBuffer(cap, 4u * sizeof(float), 0, nullptr, false);
                sa.skinPaletteCap = cap;
            }
            if (uint32_t(jobs.size()) > sa.skinJobCap) {
                uint32_t cap = std::max(sa.skinJobCap, 8u);
                while (cap < uint32_t(jobs.size())) cap *= 2u;
                if (sa.skinJobs) vao->destroyUavBuffer(sa.skinJobs);
                sa.skinJobs = vao->createUavBuffer(cap, sizeof(SkinJobRecord), 0, nullptr, false);
                sa.skinJobCap = cap;
            }
            sa.skinPalette->upload(palette.data(), 0, paletteRows);
            sa.skinJobs->upload(jobs.data(), 0, jobs.size());

            // 5. WRITE-AFTER-READ, explicitly: last frame's readers of these very
            //    caches (the skinned builds, and the ray jobs' hit decode through
            //    the rows) are in earlier submissions on this queue; the job below
            //    must not overwrite what they have not read. Ogre's barrier solver
            //    cannot see a buffer written through a device address (F5's lesson).
            cmd = frameCmd();
            monitor::CacheScope skinJob(CacheKind::Gi, WorkReason::Moved, 0, "rq.skin.dispatch", mRs, &sa);
            VkMemoryBarrier war{};
            war.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            war.srcAccessMask = 0;
            war.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                     VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &war, 0, nullptr, 0,
                                 nullptr);

            // 6. THE DISPATCH: x = vertices, y = items. The geometry table is
            //    re-read immediately before binding (a grow re-creates it — the
            //    ATOM-VOXEL-2 trap) and every binding is dropped afterwards (the job
            //    outlives the buffers a later grow destroys).
            JAH_TRY {
                job->_setUavBuffer(0u, skinSlot(sa.skinJobs, Ogre::ResourceAccess::Read));
                job->_setUavBuffer(1u, skinSlot(gs.geomBuffer(), Ogre::ResourceAccess::Read));
                job->_setUavBuffer(2u, skinSlot(sa.skinPalette, Ogre::ResourceAccess::Read));
                job->setNumThreadGroups((maxVerts + kSkinThreadsPerGroup - 1u) / kSkinThreadsPerGroup,
                                        uint32_t(jobs.size()), 1u);
                Ogre::ResourceTransitionArray &rt =
                    mRs->getBarrierSolver().getNewResourceTransitionsArrayTmp();
                job->analyzeBarriers(rt);
                mRs->executeResourceTransition(rt);
                hc->dispatch(job, nullptr, nullptr);
                job->clearUavBuffers();
            }
            catch (Ogre::Exception &e) {
                job->clearUavBuffers();
                err = "skin cache: " + e.getFullDescription();
                sa.st.skinReason = err;
                cmd = frameCmd();
                return false;
            }

            // 7. PREMISE 2: the job's writes through the cache's device address are
            //    invisible to Ogre, so the edge to every reader is OURS — the
            //    skinned structures' builds below (vertex input to an AS build is
            //    SHADER_READ at the build stage) and the ray jobs' hit decode later
            //    this frame (compute). No VERTEX_INPUT edge: nothing draws the cache.
            cmd = frameCmd();
            VkMemoryBarrier raw{};
            raw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            raw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            raw.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &raw, 0, nullptr, 0, nullptr);
            skinJob.close();
            ++sa.st.skinDispatches;
            for (SceneAs::Skin *sk : settle) sk->prevBehind = false;
            for (const Dirty &d : dirty) {
                if (!d.sk) continue;
                // a re-skin leaves the slice one pose behind; the first leaves it level
                d.sk->prevBehind = d.sk->skinned;
                if (d.sk->skinned) {
                    if (sa.skinPosedFrame != vao->getFrameCount()) sa.skinPosedSlots.clear();
                    sa.skinPosedFrame = vao->getFrameCount();
                    sa.skinPosedSlots.push_back(d.slot);   // the reflection's rest: a posed slot breaks per texel (G6)
                }
                d.sk->skinned = true;
                d.sk->poseSerial = d.poseSerial;
                ++sa.st.skinPasses;
                ++sa.st.skinLastItems;
                sa.st.skinLastVertices += d.sk->buf.vertexCount;
            }
        }

        // 8. THE STRUCTURES: built once (PREFER_FAST_TRACE | ALLOW_UPDATE) and
        //    REFIT in place on every later pose change — one batch, each with its
        //    own scratch so no two alias. A refit keeps the address, so the
        //    instance the gather writes needs nothing else.
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
        std::vector<VkAccelerationStructureGeometryKHR> geoms;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        builds.reserve(dirty.size());
        geoms.reserve(dirty.size());
        ranges.reserve(dirty.size());
        for (const Dirty &d : dirty) {
            if (!d.sk || !d.sk->skinned) continue;
            SceneAs::Skin &sk = *d.sk;
            Ogre::VertexArrayObject *vaoSrc =
                sk.item->getMesh()->getSubMesh(0)->mVao[Ogre::VpNormal][0];
            Ogre::IndexBufferPacked *ib = vaoSrc->getIndexBuffer();
            Ogre::VulkanBufferInterface *ibi =
                static_cast<Ogre::VulkanBufferInterface *>(ib->getBufferInterface());
            const uint32_t tris = uint32_t(vaoSrc->getPrimitiveCount() / 3u);
            if (!ibi || !tris) continue;
            VkAccelerationStructureGeometryKHR g{};
            g.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR | VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;   // (W1)
            g.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            g.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            g.geometry.triangles.vertexData.deviceAddress = VkDeviceAddress(sk.buf.address);
            g.geometry.triangles.vertexStride = kSkinCacheStride;
            g.geometry.triangles.maxVertex = sk.buf.vertexCount - 1u;
            g.geometry.triangles.indexType = ib->getIndexType() == Ogre::IndexBufferPacked::IT_16BIT
                                                 ? VK_INDEX_TYPE_UINT16
                                                 : VK_INDEX_TYPE_UINT32;
            g.geometry.triangles.indexData.deviceAddress =
                addressOf(ibi->getVboName()) +
                VkDeviceSize(ib->_getFinalBufferStart()) * ib->getBytesPerElement() +
                VkDeviceSize(vaoSrc->getPrimitiveStart()) * ib->getBytesPerElement();
            VkAccelerationStructureBuildGeometryInfoKHR b{};
            b.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
            b.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            b.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                      VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
            b.geometryCount = 1;
            if (!sk.as) {
                VkAccelerationStructureBuildSizesInfoKHR sizes{};
                sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
                b.pGeometries = &g;
                mFn.getBuildSizes(mVk, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &b, &tris, &sizes);
                const VkDeviceSize align = std::max<VkDeviceSize>(
                    mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
                if (!makeBuffer(sizes.accelerationStructureSize,
                                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false, true,
                                sk.storage, err) ||
                    !makeBuffer(std::max(sizes.buildScratchSize, sizes.updateScratchSize) + align,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, true, sk.scratch, err)) {
                    dropBuffer(sk.storage);
                    dropBuffer(sk.scratch);
                    continue;
                }
                VkAccelerationStructureCreateInfoKHR ci{};
                ci.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
                ci.buffer = sk.storage.buffer;
                ci.size = sizes.accelerationStructureSize;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                if (mFn.createAccelerationStructure(mVk, &ci, nullptr, &sk.as) != VK_SUCCESS) {
                    sk.as = VK_NULL_HANDLE;
                    dropBuffer(sk.storage);
                    dropBuffer(sk.scratch);
                    err = "rayquery: vkCreateAccelerationStructureKHR (skinned BLAS) failed";
                    continue;
                }
                VkAccelerationStructureDeviceAddressInfoKHR ai{};
                ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
                ai.accelerationStructure = sk.as;
                sk.address = mFn.getAsDeviceAddress(mVk, &ai);
                sk.built = false;
            }
            const VkDeviceSize align = std::max<VkDeviceSize>(
                mAsProps.minAccelerationStructureScratchOffsetAlignment, 1u);
            b.mode = sk.built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                              : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            b.srcAccelerationStructure = sk.built ? sk.as : VK_NULL_HANDLE;
            b.dstAccelerationStructure = sk.as;
            b.scratchData.deviceAddress = (addressOf(sk.scratch.buffer) + align - 1) / align * align;
            if (sk.built) ++sa.st.skinRefits; else ++sa.st.skinBlasBuilds;
            sk.built = true;
            sk.triangles = tris;
            geoms.push_back(g);
            VkAccelerationStructureBuildRangeInfoKHR r{};
            r.primitiveCount = tris;
            ranges.push_back(r);
            builds.push_back(b);
        }
        if (!builds.empty()) {
            monitor::CacheScope skinBuild(CacheKind::Gi, WorkReason::Moved, 0, "rq.skin.build", mRs, &sa);
            skinBuild.setUnits(unsigned(builds.size()));
            std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> rangePtrs;
            for (size_t i = 0; i < builds.size(); ++i) {
                builds[i].pGeometries = &geoms[i];
                rangePtrs.push_back(&ranges[i]);
            }
            // Last frame's build or refit of the same structure (and the traces
            // that read it) before this one rewrites it in place.
            VkMemoryBarrier pre{};
            pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            pre.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            pre.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                                VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &pre, 0,
                                 nullptr, 0, nullptr);
            mFn.cmdBuild(cmd, uint32_t(builds.size()), builds.data(), rangePtrs.data());
            // BUILD -> the TLAS build that references them (buildTlas' own pre
            // barrier covers it too; this one is the skinned structures' own).
            VkMemoryBarrier post{};
            post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            post.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            post.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                                 VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
                                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &post, 0, nullptr, 0, nullptr);
        }
    }

    // 9. WHAT THE GATHER MAY WRITE: the entries skinned AND built.
    for (auto &kv : sa.skins) {
        const SceneAs::Skin &sk = kv.second;
        if (sk.seen && sk.skinned && sk.built && sk.as && sk.address)
            sa.skinUse.emplace(kv.first, std::make_pair(sk.address, sk.row));
    }
    return true;
}

// ---------------------------------------------------------------------------
// THE FRAME'S UPDATE. Everything here is recorded into Ogre's own command
// buffer; nothing is submitted and nothing waits.
void RayQueryTier::updateScene(OgreScene *scene) {
    if (!isOpen() || !scene) return;
    SceneAs &sa = asFor(scene);
    sa.st.available = true;
    sa.st.enabled = true;
    // THE FRAME'S HOST WORK, COUNTED (stage 3: engine.ray_update_counts), from zero.
    sa.st.updateLookups = 0ull;
    sa.st.updateRows = 0ull;
    sa.st.alphaStaleServed = 0ull;
    sa.st.updateVisits = 0ull;

    // THE GATE. The caster walk's epoch — the host's transform-write counter
    // plus the engine's own writes plus the pushed shape events — is exactly
    // "could anything a traced instance depends on have changed?". A still
    // frame therefore records nothing at all, which is what makes a 60 Hz
    // editor with an alive TLAS cost nothing at rest.
    //
    // ...PLUS THE RAY RULE'S REFITS (ATOM-FARBLAS-1): the near copy's level is
    // the rule's answer, and the rule moves with the CAMERA, which is not scene
    // movement — so a still scene whose eye crossed a 2x band must still write
    // its instances once. Both counters only ever grow, so their sum is an
    // epoch too.
    // THE HIT DECODE'S DRAWS (PHOTON-HIT-SHADE-1): a decode twin for every PBS
    // datablock the scene's items wear, and one decode draw per twin in this
    // scene — made HERE, outside the compositor, when the set of material words
    // (GpuInstance::raster x, kept by the change feed) or HlmsAtom's twin epoch
    // moved. A TWIN IS A COPY, AND A COPY DRIFTS: the engine edits a PBS datablock
    // in place (a texture bound or streamed in, the sky's reflection cube taken
    // away when a probe grid binds, a flag); Ogre re-hashes every renderable such
    // an edit reaches, the PBS change log notes it, and the frame's drain
    // (OgreEngine::drainPbsChanges) forgets the twin whose bucket moved — the epoch
    // this compares. Nothing here walks the slots or polls a material.
    {
        detail::GpuScene &gsc = scene->gpuScene();
        Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
        auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
        if (gsc.live() && !sa.feedOn) {
            sa.feed.scene = scene;
            sa.feed.rows = &sa.geomRowOfSlot;
            gsc.addObserver(&sa.feed);   // primes it: one walk, once per scene
            sa.feedOn = true;
        }
        if (atom && gsc.live() && (sa.feed.words.changed() || atom->twinEpoch() != sa.decodeEpoch)) {
            std::vector<uint32_t> words = sa.feed.words.take();
            if (words != sa.decodeWords || atom->twinEpoch() != sa.decodeEpoch) {
                sa.decodeWords.swap(words);
                atom->syncSceneDecodes(scene->sceneManager(), sa.decodeWords);
                sa.decodeEpoch = atom->twinEpoch();
            }
        }
    }
    const unsigned long long epoch = scene->rayStructureEpoch();
    // ...AND THE SET ITSELF (ATOM-CPU-WALKS-1): a slot that entered or left the
    // traced set, or changed its structures, without anything moving (an alpha
    // test or a route edited in place, a skinned item's rig) is a new set — the
    // feed says so; the old writer re-read the set only when the epoch moved.
    const bool moved = !sa.haveEpoch || epoch != sa.lastEpoch || sa.feed.structDirty;
    drainRetired();
    // THE CUT-OUTS' TABLE (REFLECT-MOVERS-2) every frame, before the still-scene
    // gate: a mask that became ready, or a slot's near level the ray rule moved,
    // changes it with nothing else moving.
    updateAlphaTable(scene, sa);
    updatePortals(scene, sa);
    updateEmitterTable(scene, sa);
    updateHitTable(scene, sa);
    // A COMPACTION OWED, from its own list (decision 14.4: this was a walk of every structure every
    // frame, an idle one included).
    const bool compactionPending = !sa.compactWaiting.empty();
    if (!moved && sa.tlas && !compactionPending) {
        settleStillSkins(scene, sa);
        return;
    }

    // COMPACTION FIRST, BEFORE THE GATHER. It REPLACES a bottom-level
    // structure's device address, and the gather writes those addresses into
    // the instance array — so running it after the gather (as this did) built a
    // top-level structure pointing at the structures compaction had just
    // retired. They stayed alive for the retire window and were then freed
    // under a TLAS that a still scene never rebuilds: a device loss a few
    // frames after any compaction, which is exactly what two drawn scenes made
    // reproducible (round 2, cases 7/8).
    //
    // Run here, the addresses are already the new ones when the gather reads
    // them, and compaction's own blasVersion bump forces the full rebuild that
    // publishes them.
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    const bool didCompact = runCompaction(sa, cmd);

    // A STILL SCENE WITH NOTHING TO PUBLISH STOPS HERE (round 3, finding 5).
    // `compactionPending` above is true for every frame between a BLAS batch
    // and its size answer, so a still scene ran the WHOLE gather and a full
    // TLAS rebuild once per in-flight frame for nothing — two wasted passes
    // over every instance after each batch. It only has work if compaction
    // actually replaced an address.
    if (!moved && sa.tlas && !didCompact) {
        settleStillSkins(scene, sa);
        return;
    }

    // --- the gather, straight into this frame's instance slot ---------------
    // gatherMs MEASURES THE GATHER (finding 14). It used to span everything
    // from here to the end of command recording — the BLAS descriptions, the
    // buffer creation, the compaction pass — and was read as "the cost of the
    // instance walk", which it was not. Only the walk is timed now; the rest is
    // GPU-side work whose cost the timestamps report.
    double gatherMs = 0.0;
    const uint32_t frame = frameNow();

    std::string err;
    // THE SKIN CACHE FIRST (PHOTON-SKIN-1): the gather writes the rigged items'
    // structure addresses, so those structures must exist — and be built or
    // refit over THIS frame's pose — before it runs. The bones are current here:
    // updateSceneGraph has run (updateAllAnimations) and nothing has rendered.
    {
        monitor::CacheScope scope(CacheKind::Gi, WorkReason::Moved, 0, "rq.skin", mRs);
        const unsigned long long before = sa.st.skinPasses;
        const Clock::time_point tSkin = Clock::now();
        if (!skinPass(scene, sa, cmd, err) && !err.empty())
            Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
        if (sa.st.skinPasses != before) sa.st.skinCpuMs = float(msSince(tSkin));
        scope.setUnits(unsigned(sa.st.skinPasses - before));
        if (sa.st.skinPasses == before) scope.cancel();
    }

    // --- THE INSTANCES, ON THE DEVICE (ATOM-CPU-WALKS-1) ---------------------
    // What the host still does here is per MESH or per rigged item, never per
    // instance: the bottom-level structures the change feed says are referenced
    // and do not exist yet (built first — the job reads their addresses), the
    // job's inputs when they changed, the dispatch, the build. `gatherMs` is that
    // CPU; it used to be the instance walk (3-13 ms at 10k, W8 / W13).
    const Clock::time_point tGather = Clock::now();
    SceneAs::Feed &fd = sa.feed;
    detail::GpuScene &gs = scene->gpuScene();
    const uint32_t slots = gs.slotCount();
    {
        // 1. THE KEY EVENTS (ENGINE-UPGRADE-U5 stage 3): what the change feed saw since the last update
        //    - a (mesh, level) the set began or stopped referencing - applied to the structures, and
        //    the ones referenced and not built yet kept in `sa.missing` (by key; built in ascending
        //    entry order). The two scans over EVERY key that were here (the
        //    missing scan and a lastSeen stamp, a hash find each, every moved frame: 27 % of the tier's
        //    update on Bistro's walk) are gone: the work is one lookup per event.
        if (fd.keysReset) {
            for (Blas &bl : sa.blas) {
                ++sa.st.updateVisits;   // a reset's one pass (the feed re-primed), never a frame's
                if (bl.referenced) { bl.referenced = false; bl.lastSeen = frame ? frame : 1u; }
            }
            sa.missing.clear();
            fd.keysReset = false;
            sa.inputsNeedFull = true;   // the GPU scene reset: the job's records re-written whole (decision 14.10)
        }
        // Every hash or map operation is counted (decision 14.4): the event's find, and `missing`'s insert
        // or erase - an erase only when the structure is NOT built (a built key is never missing), so a
        // hide or a show stays one lookup.
        fd.netKeyEvents();   // RAY-FEED-OBSERVER-1: a key referenced and released inside the batch files nothing
        for (const SceneAs::Feed::KeyEvent &ev : fd.keyEvents) {
            ++sa.st.updateLookups;
            const auto it = sa.blasOf.find(ev.key);
            const bool have = it != sa.blasOf.end() && it->second < sa.blas.size();
            if (ev.added) {
                if (have) {
                    sa.blas[it->second].referenced = true;
                } else {
                    ++sa.st.updateLookups;
                    sa.missing[ev.key] = ev.meshIndex;
                }
            } else {
                if (have) {
                    Blas &bl = sa.blas[it->second];
                    bl.referenced = false;
                    bl.lastSeen = frame ? frame : 1u;
                } else {
                    ++sa.st.updateLookups;
                    sa.missing.erase(ev.key);
                }
            }
        }
        fd.keyEvents.clear();
        if (!sa.missing.empty()) {
            // KEYED BY (mesh, level) - the feed's key (decision 14.5): the mesh-table entry is a HINT, and a
            // mesh re-indexed under its unchanged key before its structure was built is found again through
            // the job's own mesh index (meshIndexOf, kept by the mesh-set diff), never stranded. Built in
            // ascending entry order, the order the slots were handed out in before.
            struct Want { uint32_t entry = 0u; BlasKey key; Ogre::MeshPtr mesh; };
            std::vector<Want> found;
            found.reserve(sa.missing.size());
            for (auto &m : sa.missing) {
                ++sa.st.updateLookups;   // a visit of a missing key (one per moved frame while it waits)
                uint32_t entry = m.second;
                const Ogre::Mesh *at = entry < gs.meshEntryCount() ? gs.meshAt(entry).get() : nullptr;
                if (at != m.first.mesh) {
                    ++sa.st.updateLookups;
                    const auto ix = sa.meshIndexOf.find(m.first.mesh);
                    if (ix == sa.meshIndexOf.end() || ix->second >= gs.meshEntryCount() ||
                        gs.meshAt(ix->second).get() != m.first.mesh)
                        continue;   // not in the table yet: retried the next moved frame
                    entry = ix->second;
                    m.second = entry;
                }
                found.push_back(Want{ entry, m.first, gs.meshAt(entry) });
            }
            std::sort(found.begin(), found.end(), [](const Want &a, const Want &b) {
                return a.entry != b.entry ? a.entry < b.entry : a.key.level < b.key.level;
            });
            std::vector<BlasWant> wants;
            wants.reserve(found.size());
            for (const Want &f : found) wants.push_back(BlasWant{ f.mesh, f.key.level });
            unsigned built = 0;
            if (!wants.empty()) {
                monitor::CacheScope scope(CacheKind::Gi, WorkReason::Added, 0, "rq.blas", mRs, &sa);
                if (!ensureBlas(sa, wants, cmd, built, err)) {
                    Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
                    scope.cancel();
                    // The TLAS goes with the failure: compaction may already have
                    // retired structures it references, and a scene that stops
                    // moving would keep that half-built tree for ever (finding 6).
                    retire(sa.tlas, sa.tlasStorage);
                    ++sa.blasVersion;
                    sa.inputsNeedFull = true;
                    sa.st.enabled = false;
                    return;
                }
                scope.setUnits(built);
                sa.st.blasBuilds += built;
                if (built) ++sa.blasVersion;
            }
            // WHAT WAS BUILT IS REFERENCED (it was wanted because it is), and no longer missing.
            for (const Want &f : found) {
                ++sa.st.updateLookups;
                const auto it = sa.blasOf.find(f.key);
                if (it == sa.blasOf.end() || it->second >= sa.blas.size()) continue;
                sa.blas[it->second].referenced = true;
                ++sa.st.updateLookups;
                sa.missing.erase(f.key);
            }
        }
    }
    // 2. THE JOB'S INPUTS (and the rigged slots' rows), then 3. THE DISPATCH.
    unsigned skinReady = 0;
    bool inputsChanged = false;
    cmd = frameCmd();
    if (!cmd || !writeTlasInputs(scene, sa, skinReady, inputsChanged, err) ||
        !dispatchTlasWrite(scene, sa, cmd, err)) {
        Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
        retire(sa.tlas, sa.tlasStorage);
        ++sa.blasVersion;
        sa.inputsNeedFull = true;
        sa.st.enabled = false;
        return;
    }
    cmd = frameCmd();
    sa.tlasPrimitives = 2u * slots;
    sa.farInstanceCount = fd.traced + skinReady;
    sa.instanceCount = 2u * sa.farInstanceCount;
    sa.st.skinnedInstances = int(skinReady);
    sa.farOverlap = fd.coarse.empty() ? 0.0f : fd.coarse.rbegin()->first;
    gatherMs = msSince(tGather);
    {
        // REBUILD IS THE DEFAULT, refit the optimisation (NVIDIA's own guidance
        // for a TLAS: "consider PREFER_FAST_TRACE and perform only rebuilds").
        // A refit is only ever taken when the SET is identical — no key, kind or
        // slot count moved (the feed), and no address and no ready rigged slot
        // (the inputs) — and only when the tuning asks for it.
        const bool sameSet = sa.tlas && !fd.structDirty && !inputsChanged && sa.builtSlots == slots;
        const bool refit = preferRefit() && sameSet;
        {
            monitor::CacheScope scope(CacheKind::Gi, refit ? WorkReason::Moved : WorkReason::Rebuild,
                                      0, "rq.tlas", mRs, &sa);
            if (!buildTlas(sa, cmd, refit, err)) {
                Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
                scope.cancel();
                // The TLAS goes with the failure: compaction may already have
                // retired structures it references, and a scene that stops
                // moving would keep that half-built tree for ever (finding 6).
                retire(sa.tlas, sa.tlasStorage);
                ++sa.blasVersion;
                sa.inputsNeedFull = true;
                sa.st.enabled = false;
                return;
            }
            // THE TRACED SET, not the TLAS's 2N: a per-unit cost that halved
            // silently when every object gained a far copy would be a lie (F6).
            // The far copies are their own row's units, beside it.
            scope.setUnits(sa.instanceCount - sa.farInstanceCount);
        }
        {
            // Zero-width by construction — the far copies are built by the SAME
            // command as the near ones, so this row carries their COUNT and no
            // time of its own.
            monitor::CacheScope far(CacheKind::Gi, refit ? WorkReason::Moved : WorkReason::Rebuild,
                                    0, "rq.tlas.far", nullptr);
            far.setUnits(sa.farInstanceCount);
        }
        fd.structDirty = false;
        sa.builtSlots = slots;
    }

    sa.lastEpoch = epoch;
    sa.haveEpoch = true;
    evictStaleBlas(sa);
    // A RIGGED ITEM THAT LEFT THE TRACED SET gives its cache back HERE, after the
    // top-level structure this frame built without it — never earlier: the TLAS
    // a still scene keeps is the one that last referenced it (evictStaleBlas'
    // rule, and its reason).
    for (auto it = sa.skins.begin(); it != sa.skins.end();) {
        if (it->second.seen) { ++it; continue; }
        dropSkin(scene, sa, it->first, it->second);
        it = sa.skins.erase(it);
    }
    sa.st.gatherMs = float(gatherMs);
    // LIVE structures only: an evicted slot keeps its place in the vector (the
    // index is referenced by the table) but holds nothing (F6).
    // THE TRACED SET is the near copies; the far copies are the same objects
    // again (A5b §4), counted apart so a reader can tell the two halves.
    sa.st.instances = int(sa.instanceCount - sa.farInstanceCount);
    sa.st.farInstances = int(sa.farInstanceCount);
    sa.st.blasCount = 0;
    sa.st.triangles = 0;
    sa.st.blasBytes = 0;
    sa.st.levelBlasCount = 0;
    sa.st.levelBlasBytes = 0;
    // ONE PASS over the structures a MOVED frame (decision 14.4: two were here), counted - an idle frame
    // returned long before this.
    for (const Blas &bl : sa.blas) {
        ++sa.st.updateVisits;
        if (bl.as) ++sa.st.blasCount;
        sa.st.triangles += int(bl.triangles);
        sa.st.blasBytes += bl.storage.size;
        if (bl.as && bl.level > 0u) {
            ++sa.st.levelBlasCount;
            sa.st.levelBlasBytes += bl.storage.size;
        }
    }
    // THE SKINNED STRUCTURES are per ITEM and counted apart as well as in the
    // totals: their bytes and their caches are what a character costs.
    sa.st.skinCaches = int(sa.skins.size());
    sa.st.skinBlasBytes = 0;
    sa.st.skinCacheBytes = 0;
    for (const auto &kv : sa.skins) {
        const SceneAs::Skin &sk = kv.second;
        if (sk.as) {
            ++sa.st.blasCount;
            sa.st.triangles += int(sk.triangles);
            sa.st.blasBytes += sk.storage.size;
            sa.st.skinBlasBytes += sk.storage.size;
        }
        sa.st.skinCacheBytes +=
            (unsigned long long)sk.buf.vertexCount * (kSkinCacheStride + kSkinPrevStride);
    }
}

RayQueryStatus RayQueryTier::status(const OgreScene *scene) const {
    const SceneAs *sa = asOf(scene);
    if (!sa) {
        // A scene the tier has not seen yet (nothing has drawn it). `enabled`
        // is the SWITCH's answer, not "false because there is no structure":
        // the tier being open at all means the switch is on, and reporting
        // false here made a fresh scene indistinguishable from a machine with
        // rays switched off (round 2, finding 18).
        RayQueryStatus st;
        st.available = isOpen();
        st.enabled = isOpen();
        return st;
    }
    RayQueryStatus st = sa->st;
    st.feedSlotVisits = sa->feed.visits.load(std::memory_order_relaxed);
    // THE GPU TIMES are the monitor's rows this scene filed (lane TEST-1): -1
    // outside a capture, and a still scene's last value held, as before.
    const void *owner = sa;
    st.blasMs = monitor::lastGpuMs("rq.blas", owner);
    st.tlasMs = monitor::lastGpuMs("rq.tlas", owner);
    st.skinMs = monitor::lastGpuMs("rq.skin.dispatch", owner);
    st.skinRefitMs = monitor::lastGpuMs("rq.skin.build", owner);
    st.panesOverflow = mTierCounters.mapped ? static_cast<const volatile uint32_t *>(mTierCounters.mapped)[0] : 0u;
    return st;
}

// ---------------------------------------------------------------------------
// engine.tlas_compute's door (ATOM-CPU-WALKS-1): the job's output read back, with
// what the suite needs to build its own reference — every bottom-level structure's
// address by (mesh, level), the ready rigged slots, the far overlap, the counts and
// the rows. A TOOL path — its own command buffer, a fence wait.
bool RayQueryTier::readTlasBlocking(OgreScene *scene, TlasReadback &out, std::string &err) {
    out = TlasReadback();
    if (!isOpen()) { err = "rayquery: the tier is not open"; return false; }
    SceneAs *saP = asOf(scene);
    if (!saP || !saP->tlasOut.buffer) {
        err = "rayquery: this scene has no instance array yet (render a frame first)";
        return false;
    }
    SceneAs &sa = *saP;
    const uint32_t slots = sa.builtSlots;
    out.slots = slots;
    out.instances.assign(size_t(slots) * 2u * sizeof(VkAccelerationStructureInstanceKHR), 0u);
    if (slots) {
        RawBuffer host;
        const VkDeviceSize bytes = VkDeviceSize(out.instances.size());
        if (!makeBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false, host, err)) return false;
        VkDescriptorBufferInfo src{};
        src.buffer = sa.tlasOut.buffer;
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = mDev->mGraphicsQueue.getFamilyIdx();
        VkCommandPool pool = VK_NULL_HANDLE;
        vkCreateCommandPool(mVk, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(mVk, &cai, &cmd);
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        VkMemoryBarrier pre{};
        pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        pre.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        pre.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre,
                             0, nullptr, 0, nullptr);
        VkBufferCopy region{};
        region.srcOffset = src.offset;
        region.size = bytes;
        vkCmdCopyBuffer(cmd, src.buffer, host.buffer, 1, &region);
        VkMemoryBarrier toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0,
                             nullptr, 0, nullptr);
        vkEndCommandBuffer(cmd);
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        vkCreateFence(mVk, &fci, nullptr, &fence);
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        bool ok = vkQueueSubmit(mDev->mGraphicsQueue.mQueue, 1, &si, fence) == VK_SUCCESS;
        if (ok) ok = vkWaitForFences(mVk, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
        if (ok) std::memcpy(out.instances.data(), host.mapped, size_t(bytes));
        vkDestroyFence(mVk, fence, nullptr);
        vkFreeCommandBuffers(mVk, pool, 1, &cmd);
        vkDestroyCommandPool(mVk, pool, nullptr);
        dropBuffer(host);
        if (!ok) { err = "rayquery: the instance readback did not complete"; return false; }
    }
    for (const auto &kv : sa.blasOf)
        if (kv.second < sa.blas.size() && sa.blas[kv.second].address)
            out.structures.push_back({ kv.first.mesh, kv.first.level, uint64_t(sa.blas[kv.second].address) });
    for (const auto &kv : sa.skinUse)
        out.skins.push_back({ kv.first, uint64_t(kv.second.first), kv.second.second });
    out.farOverlap = sa.farOverlap;
    out.instanceCount = sa.instanceCount;
    out.farInstanceCount = sa.farInstanceCount;
    out.rows = sa.geomRowOfSlot;
    out.rows.resize(slots, detail::GpuScene::kNoGeomRow);
    return true;
}

// ---------------------------------------------------------------------------
// THE TOOL/TEST TRACE. The ONLY place in this file that submits its own command
// buffer and waits — and it is not a frame path: it exists so a suite can ask
// "what does this ray hit?" and get the answer in the same statement. Every
// product consumer (R2 probe visibility, R3 sun contact, R5 reflections) reads
// the same structures from a compute pass recorded INSIDE the frame.
bool RayQueryTier::traceBlocking(OgreScene *scene, const std::vector<float> &rays,
                                 std::vector<float> &hits, std::string &err) {
    hits.clear();
    if (!isOpen()) { err = "rayquery: the tier is not open"; return false; }
    SceneAs *saP = asOf(scene);
    if (!saP || !saP->tlas || saP->instanceCount == 0u) {
        err = "rayquery: this scene has no acceleration structure yet (render a frame first)";
        return false;
    }
    // A DISABLED SCENE'S STRUCTURE IS NOT TRUSTWORTHY (round 3, finding 6).
    // `enabled` goes false when a frame's build FAILED, and the structure it
    // left behind can reference bottom-level structures compaction retired in
    // the same frame. Refusing is the honest answer; tracing it would return
    // confident nonsense, or lose the device.
    if (!saP->st.enabled) {
        err = "rayquery: this scene's structure is stale (the last update failed)";
        return false;
    }
    SceneAs &sa = *saP;
    const size_t count = rays.size() / 12u;      // 3 vec4s per ray
    if (!count) { err = "rayquery: an empty ray batch"; return false; }

    // EVERYTHING OGRE HAS RECORDED GOES FIRST. The acceleration structures were
    // built into the frame's command buffer; flushing submits it and waits, so
    // what follows cannot read a structure that has not been built.
    mRs->flushCommands();

    RawBuffer rayBuf, hitBuf, ubo;
    if (!makeBuffer(rays.size() * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false,
                    rayBuf, err))
        return false;
    memcpy(rayBuf.mapped, rays.data(), rays.size() * sizeof(float));
    if (!makeBuffer(count * 4u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false,
                    hitBuf, err)) {
        dropBuffer(rayBuf);
        return false;
    }
    struct Params { uint32_t counts[4] = {}; uint32_t alpha[4] = {}; } params{};
    params.counts[0] = uint32_t(count);
    {
        // THE CUT-OUTS (REFLECT-MOVERS-2): the same table the frame's queries read.
        const uint64_t a = alphaTableOf(scene);
        params.alpha[0] = uint32_t(a & 0xFFFFFFFFu);
        params.alpha[1] = uint32_t(a >> 32u);
    }
    if (!makeBuffer(sizeof(params), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, ubo, err)) {
        dropBuffer(rayBuf);
        dropBuffer(hitBuf);
        return false;
    }
    memcpy(ubo.mapped, &params, sizeof(params));

    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = mDescPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &mSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(mVk, &dai, &set) != VK_SUCCESS) {
        err = "rayquery: vkAllocateDescriptorSets failed";
        dropBuffer(rayBuf); dropBuffer(hitBuf); dropBuffer(ubo);
        return false;
    }
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo rb{}, hb{}, ub{};
    rb.buffer = rayBuf.buffer; rb.range = VK_WHOLE_SIZE;
    hb.buffer = hitBuf.buffer; hb.range = VK_WHOLE_SIZE;
    ub.buffer = ubo.buffer;    ub.range = sizeof(params);
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[0].pNext = &asWrite;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &rb;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &hb;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &ub;
    vkUpdateDescriptorSets(mVk, 4, writes, 0, nullptr);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = mDev->mGraphicsQueue.getFamilyIdx();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(mVk, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(mVk, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPipeLayout, 0, 1, &set, 0,
                            nullptr);
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatch(cmd, uint32_t((count + 63u) / 64u), 1, 1);
    // THE HIT BUFFER IS READ BY THE HOST as soon as the fence signals, and a
    // fence does not make a shader's writes visible to the CPU by itself
    // (finding 12). The memory is HOST_COHERENT, so no invalidate is needed —
    // but the availability operation is, and this is it.
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &toHost, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(mVk, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(mDev->mGraphicsQueue.mQueue, 1, &si, fence) == VK_SUCCESS;
    // NEVER WAIT ON A FENCE WHOSE SUBMIT FAILED — that is an infinite hang.
    // The WAIT's result matters as much as the submit's: on a lost device it
    // returns VK_ERROR_DEVICE_LOST and the hit buffer holds nothing. Reporting
    // success there would hand a caller a batch of zeros as if they were
    // answers.
    if (ok) ok = vkWaitForFences(mVk, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (ok) {
        hits.resize(count * 4u);
        memcpy(hits.data(), hitBuf.mapped, hits.size() * sizeof(float));
    } else {
        err = "rayquery: the trace did not complete (submit or device-lost wait failed)";
    }
    vkDestroyFence(mVk, fence, nullptr);
    vkFreeCommandBuffers(mVk, pool, 1, &cmd);
    vkDestroyCommandPool(mVk, pool, nullptr);
    vkFreeDescriptorSets(mVk, mDescPool, 1, &set);
    dropBuffer(rayBuf);
    dropBuffer(hitBuf);
    dropBuffer(ubo);
    return ok;
}

// gi.card_read_parity — the reflection's card read, asked directly
// ---------------------------------------------------------------------------
// The same include the trace reads (jah_rq_card.glsl) behind the same four
// bindings (jah_rq_card_bindings.glsl, here at base 2), dispatched once over a
// list of points on its own command buffer, the picks read back. The suite
// holds them against SurfaceCache::readAt, the CPU reference.
bool RayQueryTier::cardPickBlocking(OgreScene *scene, const std::vector<CardReadQuery> &queries,
                                    std::vector<CardReadPick> &out, std::string &err) {
    out.clear();
    if (!isOpen()) { err = "cardReadParity: the ray tier is not open"; return false; }
    const SurfaceCache *cache = scene ? scene->photon().surfaceCache() : nullptr;
    if (!cache || !cache->cardBuffer() || !cache->instanceBuffer() || !cache->depthLayer() ||
        !cache->radianceLayer() || !cache->cardRecords()) {
        err = "cardReadParity: the scene holds no built surface cache";
        return false;
    }
    if (queries.empty()) { err = "cardReadParity: no queries"; return false; }

    // ---- the harness pipeline, once ------------------------------------------
    if (!mCardParityPipeline) {
        // ...and 10-16: the card read's view term (PHOTON-CARDS-5), the seven
        // layers of SurfaceCache::viewLayers.
        constexpr unsigned kParityBindings = 10u + SurfaceCache::kViewLayers;
        const VkDescriptorType types[kParityBindings] = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER };
        VkDescriptorSetLayoutBinding b[kParityBindings] = {};
        for (unsigned i = 0; i < kParityBindings; ++i) {
            b[i].binding = i;
            b[i].descriptorType = types[i];
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h): the harness's one set.
        DescriptorPoolPlan plan;
        if (!plan.makeLayout(mVk, b, kParityBindings, 1u, mCardParitySetLayout, err, "rayquery/card-parity"))
            return false;
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &mCardParitySetLayout;
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = sizeof(krq_cardParitySpv);
        smi.pCode = krq_cardParitySpv;
        if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mCardParityPipeLayout) != VK_SUCCESS ||
            vkCreateShaderModule(mVk, &smi, nullptr, &mCardParityModule) != VK_SUCCESS) {
            err = "cardReadParity: the harness pipeline could not be made";
            return false;
        }
        if (!plan.create(mVk, "rayquery/card-parity", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                         mCardParityPool, err))
            return false;
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = mCardParityModule;
        cpi.stage.pName = "main";
        cpi.layout = mCardParityPipeLayout;
        if (createComputePipeline(cpi, &mCardParityPipeline, "card parity") !=
            VK_SUCCESS) {
            err = "cardReadParity: vkCreateComputePipelines failed";
            return false;
        }
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(mVk, &si, nullptr, &mCardParitySampler) != VK_SUCCESS) {
            err = "cardReadParity: vkCreateSampler failed";
            return false;
        }
    }

    // ---- THE INPUTS IN THEIR READ LAYOUTS, AND EVERYTHING OGRE RECORDED FIRST.
    // The atlas's Depth layer and the Radiance UAV as textures, the two tables
    // as buffers — through Ogre's own solver (its bookkeeping stays true: the
    // harness changes no layout) — then the flush that submits the captures,
    // the relight and the table uploads ahead of this job.
    Ogre::UavBufferPacked *table = cache->cardBuffer();
    Ogre::UavBufferPacked *instances = cache->instanceBuffer();
    Ogre::TextureGpu *depth = cache->depthLayer();
    Ogre::TextureGpu *radiance = cache->radianceLayer();
    Ogre::TextureGpu *cardView[SurfaceCache::kViewLayers] = {};
    cache->viewLayers(cardView);
    // THE TRACED QUESTIONS' INPUTS: the scene's TLAS and the hit-normal tables
    // the reflection binds (the per-slot row copy, the GPU scene's rows).
    SceneAs *sa = asOf(scene);
    if (sa && !sa->tlas) sa = nullptr;
    detail::GpuScene &gpuScn = scene->gpuScene();
    if (gpuScn.live()) gpuScn.flushGeomRows();
    Ogre::UavBufferPacked *geomRows = gpuScn.live() ? gpuScn.geomBuffer() : nullptr;
    const uint32_t geomSlots = (sa && geomRows) ? uint32_t(sa->geomRowOfSlot.size()) : 0u;
    // The job's layout names the TLAS, so every dispatch binds one (a descriptor
    // a shader uses statically must be valid even when no question traces).
    if (!sa) {
        err = "cardReadParity: the scene has no acceleration structure yet (render a frame with rays on)";
        return false;
    }
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { depth, radiance })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        for (Ogre::TextureGpu *t : cardView)
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        for (Ogre::UavBufferPacked *b : { table, instances })
            solver.resolveTransition(trans, b, Ogre::ResourceAccess::Read, computeStage);
        if (geomSlots) solver.resolveTransition(trans, geomRows, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    mRs->flushCommands();

    const size_t n = queries.size();
    RawBuffer qBuf, aBuf, ubo;
    if (!makeBuffer(n * 8u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, qBuf, err))
        return false;
    if (!makeBuffer(n * 20u * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, aBuf, err)) {
        dropBuffer(qBuf);
        return false;
    }
    if (!makeBuffer(12u * sizeof(uint32_t), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, ubo, err)) {
        dropBuffer(qBuf);
        dropBuffer(aBuf);
        return false;
    }
    {
        float *q = static_cast<float *>(qBuf.mapped);
        for (size_t i = 0; i < n; ++i) {
            const long slot = cache->itemSlotOf(queries[i].node);
            const uint32_t slotBits = slot < 0 ? 0xFFFFFFFFu : uint32_t(slot);
            q[i * 8u + 0] = queries[i].position.x;
            q[i * 8u + 1] = queries[i].position.y;
            q[i * 8u + 2] = queries[i].position.z;
            std::memcpy(&q[i * 8u + 3], &slotBits, sizeof(slotBits));
            q[i * 8u + 4] = queries[i].facing.x;
            q[i * 8u + 5] = queries[i].facing.y;
            q[i * 8u + 6] = queries[i].facing.z;
            q[i * 8u + 7] = queries[i].trace ? 1.0f : 0.0f;
        }
        // ...and the ALPHA TABLE (REFLECT-MOVERS-2): the parity job's trace agrees
        // with the product's on a scene with cut-outs.
        const uint64_t a = alphaTableOf(scene);
        // ...and THE LIGHT TABLE (PHOTON-3 stage 2): the card read evaluates the lights' specular lobe from it.
        const uint64_t h = hitTableOf(scene);
        const uint32_t counts[12] = { uint32_t(n), cache->instanceSlots(), cache->cardRecords(), geomSlots,
                                      uint32_t(a & 0xFFFFFFFFu), uint32_t(a >> 32u), 0u, 0u,
                                      uint32_t(h & 0xFFFFFFFFu), uint32_t(h >> 32u), 0u, 0u };
        std::memcpy(ubo.mapped, counts, sizeof(counts));
    }

    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = mCardParityPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &mCardParitySetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(mVk, &dai, &set) != VK_SUCCESS) {
        err = "cardReadParity: vkAllocateDescriptorSets failed";
        dropBuffer(qBuf); dropBuffer(aBuf); dropBuffer(ubo);
        return false;
    }
    VkDescriptorBufferInfo bufs[4] = {};
    bufs[0].buffer = qBuf.buffer; bufs[0].range = VK_WHOLE_SIZE;
    bufs[1].buffer = aBuf.buffer; bufs[1].range = VK_WHOLE_SIZE;
    Ogre::UavBufferPacked *const tables[2] = { table, instances };
    for (int i = 0; i < 2; ++i) {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(tables[i]->getBufferInterface());
        bufs[2 + i].buffer = bi->getVboName();
        bufs[2 + i].offset = VkDeviceSize(tables[i]->_getFinalBufferStart()) *
                             tables[i]->getBytesPerElement();
        bufs[2 + i].range = tables[i]->getTotalSizeBytes();
    }
    // 4, 5: the Depth and Radiance layers; 10-16: the view term's seven.
    constexpr unsigned kImgs = 2u + SurfaceCache::kViewLayers;
    VkImageView views[kImgs] = {};
    VkDescriptorImageInfo imgs[kImgs] = {};
    static_assert(SurfaceCache::kViewLayers == 7u, "the harness lists the view term's layers");
    Ogre::TextureGpu *const layers[kImgs] = { depth, radiance, cardView[0], cardView[1], cardView[2],
                                              cardView[3], cardView[4], cardView[5], cardView[6] };
    for (unsigned i = 0; i < kImgs; ++i) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = layers[i];
        views[i] = static_cast<Ogre::VulkanTextureGpu *>(layers[i])->createView(slot, false);
        imgs[i].sampler = mCardParitySampler;
        imgs[i].imageView = views[i];
        imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorBufferInfo ub{};
    ub.buffer = ubo.buffer;
    ub.range = 12u * sizeof(uint32_t);
    // 7-9: the TLAS (the scene's, or none when no question traces — a
    // descriptor must still be valid, so the stand-in is only for 8/9), the
    // per-slot rows (copied now), the GPU scene's rows.
    RawBuffer rowBuf;
    VkDescriptorBufferInfo geomBufs[2] = {};
    if (geomSlots &&
        makeBuffer(VkDeviceSize(geomSlots) * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                   false, rowBuf, err)) {
        std::memcpy(rowBuf.mapped, sa->geomRowOfSlot.data(), size_t(geomSlots) * sizeof(uint32_t));
        geomBufs[0].buffer = rowBuf.buffer;
        geomBufs[0].range = VK_WHOLE_SIZE;
        auto *gbi = static_cast<Ogre::VulkanBufferInterface *>(geomRows->getBufferInterface());
        geomBufs[1].buffer = gbi->getVboName();
        geomBufs[1].offset = VkDeviceSize(geomRows->_getFinalBufferStart()) * geomRows->getBytesPerElement();
        geomBufs[1].range = geomRows->getTotalSizeBytes();
    } else {
        if (!ensureDummyImages(err)) { dropBuffer(qBuf); dropBuffer(aBuf); dropBuffer(ubo); return false; }
        for (VkDescriptorBufferInfo &g : geomBufs) { g.buffer = mDummyStorage.buffer; g.range = VK_WHOLE_SIZE; }
    }
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa->tlas;
    const unsigned nWrites = 10u + SurfaceCache::kViewLayers;
    VkWriteDescriptorSet writes[10u + SurfaceCache::kViewLayers] = {};
    for (unsigned i = 0; i < nWrites; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    for (int i = 0; i < 4; ++i) {
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &bufs[i];
    }
    for (int i = 0; i < 2; ++i) {
        writes[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[4 + i].pImageInfo = &imgs[i];
    }
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[6].pBufferInfo = &ub;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[7].pNext = &asWrite;
    for (int i = 0; i < 2; ++i) {
        writes[8 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[8 + i].pBufferInfo = &geomBufs[i];
    }
    for (unsigned i = 0; i < SurfaceCache::kViewLayers; ++i) {
        writes[10 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[10 + i].pImageInfo = &imgs[2 + i];
    }
    vkUpdateDescriptorSets(mVk, nWrites, writes, 0, nullptr);

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = mDev->mGraphicsQueue.getFamilyIdx();
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(mVk, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(mVk, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCardParityPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCardParityPipeLayout, 0, 1, &set,
                            0, nullptr);
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatch(cmd, uint32_t((n + 63u) / 64u), 1, 1);
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &toHost, 0, nullptr, 0, nullptr);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(mVk, &fci, nullptr, &fence);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    bool ok = vkQueueSubmit(mDev->mGraphicsQueue.mQueue, 1, &si, fence) == VK_SUCCESS;
    if (ok) ok = vkWaitForFences(mVk, 1, &fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
    if (ok) {
        out.resize(n);
        const uint32_t *a = static_cast<const uint32_t *>(aBuf.mapped);
        for (size_t i = 0; i < n; ++i) {
            CardReadPick &p = out[i];
            const uint32_t *r = &a[i * 20u];
            p.ok = (r[0] & 1u) != 0u;
            p.lit = (r[0] & 2u) != 0u;
            p.hit = (r[0] & 4u) != 0u;
            p.card = p.ok ? int(r[1]) : -1;
            p.texelX = r[2];
            p.texelY = r[3];
            std::memcpy(p.radiance, &r[4], 3u * sizeof(float));
            std::memcpy(p.hitPoint, &r[8], 3u * sizeof(float));
            std::memcpy(p.hitNormal, &r[12], 3u * sizeof(float));
            std::memcpy(p.viewed, &r[16], 3u * sizeof(float));
        }
    } else {
        err = "cardReadParity: the job did not complete (submit or device-lost wait failed)";
    }
    vkDestroyFence(mVk, fence, nullptr);
    vkFreeCommandBuffers(mVk, pool, 1, &cmd);
    vkDestroyCommandPool(mVk, pool, nullptr);
    vkFreeDescriptorSets(mVk, mCardParityPool, 1, &set);
    for (VkImageView v : views)
        if (v) vkDestroyImageView(mVk, v, nullptr);
    dropBuffer(rowBuf);
    dropBuffer(qBuf);
    dropBuffer(aBuf);
    dropBuffer(ubo);
    return ok;
}

bool OgreEngine::cardReadParity(Scene *scene, const std::vector<CardReadQuery> &queries,
                                std::vector<CardReadPick> &out) {
    out.clear();
    if (!mRayTier || !mRayTier->isOpen()) {
        mLastError = "cardReadParity: the ray tier is not open (no ray-query device, or rays off)";
        return false;
    }
    std::string err;
    if (!mRayTier->cardPickBlocking(static_cast<OgreScene *>(scene), queries, out, err)) {
        mLastError = err;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// R5 — RAY-TRACED REFLECTIONS. The consumer, in the same TU as the structures.
//
// SCREEN FIRST, RAYS FOR THE REST. The screen-space march (lanes SSR-1/SSR-2)
// keeps every pixel it is confident about — a reflected SHADED pixel of this
// scene is the sharpest and cheapest answer there is — and this pass fills only
// what it could not: off screen, behind an occluder, past its edge fade, and
// the roughness band between its cutoff and ours. The order is Lumen's and it
// is right here for a measurable reason: a ray's hit is shaded from the Photon
// voxels, whose finest DIRECTIONAL mip is half the voxel grid, so wherever the
// screen has an answer the screen's answer is better.
//
// WHERE IT SITS IN THE FRAME, and why. `passPreExecute` of the scene pass that
// SAMPLES `jahSsrReflection` (`mPrePassMode == PrePassUse` — a semantic test,
// not a pass name): the only point at which the prepass' normals and packed
// roughness exist, the resolve's own answer exists, and the texture is still
// writable. Ogre fires that listener BEFORE `analyzeBarriers`
// (OgreCompositorPassScene.cpp:235 vs :289), which is what lets the transitions
// below be resolved through Ogre's own BarrierSolver and then handed back: the
// scene pass' own analysis sees the texture in Uav and emits the Uav -> Texture
// barrier itself. Nothing here manages a layout behind Ogre's back.
//
// NO OGRE PATCH. Everything it needs is public: `getDefinedTexture` for the
// chain's textures, `createView` for the image views, `getBarrierSolver` /
// `executeResourceTransition` for the layouts, and `getCurrentCmdBuffer` for
// the recording (which fork 1a81f866a (was 0040) already made linkable). The one change to the
// engine's own chain is the Uav flag on `jahSsrReflection`, which is ours.

namespace {

/// The parameter block, std140, mirroring jah_reflect_params.glsl's `Params` member for
/// member. A mismatch here is silent and total, so the order is the shader's
/// order and nothing is reordered for packing (every member is a vec4 or an
/// array of them, which std140 lays out identically to C).
struct ReflectParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
    float knobs2[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float prevCamPos[4] = {};
    float prevRayTL[4] = {};
    float prevRayRight[4] = {};
    float prevRayDown[4] = {};
    float prevFwd[4] = {};
    /// THE SECOND EYE (lane REFLECT-VR-1, LAYERED-STEREO-1). `stereo.x` is 1
    /// when the target carries two eyes, one per layer of every screen image,
    /// and then everything above is the LEFT eye's (layer 0) and everything here
    /// the RIGHT eye's (layer 1). Appended rather than folded into an array of two: the block
    /// above is what a mono view writes and what every reader of this file
    /// already knows, and std140 lays the tail out identically either way.
    float stereo[4] = {};
    float camPos2[4] = {};
    float rayTL2[4] = {};
    float rayRight2[4] = {};
    float rayDown2[4] = {};
    float fwd2[4] = {};
    float prevCamPos2[4] = {};
    float prevRayTL2[4] = {};
    float prevRayRight2[4] = {};
    float prevRayDown2[4] = {};
    float prevFwd2[4] = {};
    /// A MOVING OBJECT (REFLECT-MOVERS-1; rq_reflect.comp's `motion`): x = 1 when
    /// the view's id image is bound at binding 38, yz = its size.
    float motion[4] = {};
    /// z = THE RAY ORIGIN'S POINT TERM (jah_ray_origin.glsl); xyw unused (the alpha table is the hit
    /// set's: `hit`).
    float alpha[4] = {};
    /// THE MEDIA ALONG THE REFLECTION (PHOTON-I-1 fix 5): OgreScene::fogAlong.
    float fog[8][4] = {};
    /// THE REST BLOCK (PHOTON-2 round 2): [0] = 1 = the GI freeze (the arm "gi.freeze": M1's measuring arm).
    float rest[4] = {};
    /// THE MOVED INSTANCES' SWEPT BOXES (G6): [0] = how many follow (<= kReflectRestMovers); box i = [2i] min,
    /// [2i + 1] max, its world bounds this frame joined with last frame's.
    float restMovers[4] = {};
    float restMoverBox[2 * kReflectRestMovers][4] = {};
    /// THE ONE ESTIMATOR'S SOURCES (PHOTON-7 stage 6; rq_reflect.comp's screen, screen2): [0] = 1 the screen trace runs,
    /// [1] = 1 the screen alone (reflectSources Screen), [2] its steps, [3] its phase rule; screen2: the march's length
    /// and thickness, the far copies' query from / to (0 = none).
    float screen[4] = {};
    float screen2[4] = {};
    /// THE DENOISER SPACE'S EXPOSURE (PHOTON-7 stage 6 commit 6; the resolve's and the temporal pass' `denoiser`): [0] =
    /// the view's exposure multiplier where it is a constant (a fixed tonemap: the chain's cleared 1 x 1; 1 with no
    /// tonemap), [1] = 1 when the chain's measured history is bound at binding 50 instead.
    float denoiser[4] = {};
    /// THE DISPLAY'S CODE SLOPE (commit 8's hold, lever (i)): 64 entries, d(display code) / d(ln x) of a grey at the
    /// PRE-EXPOSED luma x = E L over ln x in [kDisplayLnMin, kDisplayLnMin + 63 kDisplayLnStep] - through the grade, the
    /// film curve and the sRGB encode as the view's tonemap pass runs them (jah_reflect_rest.glsl jahDisplayCodes).
    float display[16][4] = {};
    /// THE REST'S COMPACT PATH (rq_reflect.comp; kReflectCompactTexels): [0] the half of the list this frame's tile
    /// pass fills (last frame's count, in the other, decides the path), [1] the tail's rays a frame (the rest's count /
    /// kReflectRestTailDivisor), [2] L, [3] 1 when the compact pass is dispatched this frame.
    float compact[4] = {};
    /// THE HIT SET'S UNIFORMS (PHOTON-7 stage 10, 10-2: hitSetParams; jah_reflect_params.glsl's last member)
    HitSetParams hit;
};
/// The display slope table's log axis (jah_reflect_rest.glsl's twins): ln 1e-4 .. ln 100 in 63 steps.
constexpr double kDisplayLnMin = -9.210340371976184;
constexpr double kDisplayLnStep = 0.21929381838038531;



/// A MOVED SLOT'S SWEPT BOX (PHOTON-2 round 2, G6): its world bounds this frame joined with last frame's - the same
/// local box under prevWorld, i.e. this frame's box carried by prevWorld * world^-1 (row-major 3x4 affines; the
/// carried box is the transformed box's own bounds, a superset). A singular world (a zero scale) joins nothing.
/// THE DISPLAY CODE OF A GREY at pre-exposed luma x (commit 8's hold, lever (i)): what the view's tonemap pass does
/// to it (FinalToneMapping_ps.glsl) - the colour correction (contrast as a power about 0.18, the shadows' and highlights'
/// gains on their luma masks; the saturation and the white balance leave a grey grey), the film curve (Types.h
/// filmToneGrey) and the exact sRGB encode, in codes; a chain with no tonemap encodes the clamped linear value. (The
/// bloom adds and the vignette scales by under 1: neither is the texel's, and leaving the vignette out only makes the
/// stop stricter.)
double displayCodeGrey(double x, bool tonemap, const ImageGrade &g) {
    const auto srgb = [](double v) {
        v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    };
    if (!tonemap) return 255.0 * srgb(x);
    const auto smooth = [](double a, double b, double v) {
        const double t = std::min(std::max((v - a) / (b - a), 0.0), 1.0);
        return t * t * (3.0 - 2.0 * t);
    };
    double w = std::max(x, 0.0);
    const double luma = w;
    w = std::pow(w / 0.18, std::max(double(g.contrast), 0.0)) * 0.18;
    w *= std::exp2(double(g.shadows) * (1.0 - smooth(0.0, 0.09, luma)) + double(g.highlights) * smooth(0.5, 1.0, luma));
    w = std::max(filmToneGrey(w, g.filmSlope, g.filmToe, g.filmShoulder, g.filmBlackClip, g.filmWhiteClip), 0.0);
    return 255.0 * srgb(w);
}
/// THE TABLE (ReflectParams::display): the code's slope against ln x, a central difference of 0.01 in ln x.
void fillDisplaySlope(float (&out)[16][4], bool tonemap, const ImageGrade &g) {
    for (int i = 0; i < 64; ++i) {
        const double l = kDisplayLnMin + kDisplayLnStep * double(i), h = 0.01;
        out[i >> 2][i & 3] = float((displayCodeGrey(std::exp(l + h), tonemap, g) -
                                    displayCodeGrey(std::exp(l - h), tonemap, g)) / (2.0 * h));
    }
}

void sweptBoundsOf(const detail::GpuInstance &g, float lo[3], float hi[3]) {
    for (int c = 0; c < 3; ++c) { lo[c] = g.boundsMin[c]; hi[c] = g.boundsMax[c]; }
    const float *W = g.world, *P = g.prevWorld;
    if (std::memcmp(W, P, sizeof(g.world)) == 0) return;
    const double a = W[0], b = W[1], c = W[2], d = W[4], e = W[5], f = W[6], gg = W[8], h = W[9], i = W[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * gg) + c * (d * h - e * gg);
    if (!(std::abs(det) > 1e-12)) return;
    const double inv[3][3] = { { (e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det },
                               { (f * gg - d * i) / det, (a * i - c * gg) / det, (c * d - a * f) / det },
                               { (d * h - e * gg) / det, (b * gg - a * h) / det, (a * e - b * d) / det } };
    const double t[3] = { W[3], W[7], W[11] };
    double M[3][3], Mt[3];
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k)
            M[r][k] = P[4 * r + 0] * inv[0][k] + P[4 * r + 1] * inv[1][k] + P[4 * r + 2] * inv[2][k];
        Mt[r] = P[4 * r + 3] - (M[r][0] * t[0] + M[r][1] * t[1] + M[r][2] * t[2]);
    }
    const double cen[3] = { 0.5 * (double(g.boundsMin[0]) + g.boundsMax[0]), 0.5 * (double(g.boundsMin[1]) + g.boundsMax[1]),
                            0.5 * (double(g.boundsMin[2]) + g.boundsMax[2]) };
    const double ext[3] = { 0.5 * (double(g.boundsMax[0]) - g.boundsMin[0]), 0.5 * (double(g.boundsMax[1]) - g.boundsMin[1]),
                            0.5 * (double(g.boundsMax[2]) - g.boundsMin[2]) };
    for (int r = 0; r < 3; ++r) {
        const double cc = M[r][0] * cen[0] + M[r][1] * cen[1] + M[r][2] * cen[2] + Mt[r];
        const double ee = std::abs(M[r][0]) * ext[0] + std::abs(M[r][1]) * ext[1] + std::abs(M[r][2]) * ext[2];
        if (!std::isfinite(cc) || !std::isfinite(ee)) continue;
        lo[r] = std::min(lo[r], float(cc - ee));
        hi[r] = std::max(hi[r], float(cc + ee));
    }
}

void put3(float *dst, const Ogre::Vector3 &v, float w) {
    dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = w;
}

}   // namespace

// TWO SAMPLERS, AND WHICH IS WHICH MATTERS. The G-buffer and the depth are read
// at exactly one texel — a linear tap across a depth discontinuity reconstructs a
// position on neither surface — so they are POINT. The voxel volumes and the sky
// cube are continuous fields and are LINEAR, with a clamped address mode so a
// sample at a volume's face does not wrap to the other side of the world.
// THE TIER'S OWN, made by whichever job needs them first (PHOTON-GATHER-1d): they
// were made by the reflection's pipeline alone, and the gather borrowed them only
// because the reflection's record ran first on every PrePassUse pass — a
// gather-only chain that no longer asks the reflection anything bound a NULL
// sampler (a driver segfault in vkUpdateDescriptorSets).
bool RayQueryTier::ensureSamplers(std::string &err) {
    if (mPointSampler && mLinearSampler && mWrapSampler) return true;
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (!mPointSampler && vkCreateSampler(mVk, &si, nullptr, &mPointSampler) != VK_SUCCESS) {
        mPointSampler = VK_NULL_HANDLE;
        err = "rayquery: vkCreateSampler (point) failed";
        return false;
    }
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (!mLinearSampler && vkCreateSampler(mVk, &si, nullptr, &mLinearSampler) != VK_SUCCESS) {
        mLinearSampler = VK_NULL_HANDLE;
        err = "rayquery: vkCreateSampler (linear) failed";
        return false;
    }
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    if (!mWrapSampler && vkCreateSampler(mVk, &si, nullptr, &mWrapSampler) != VK_SUCCESS) {
        mWrapSampler = VK_NULL_HANDLE;
        err = "rayquery: vkCreateSampler (wrap) failed";
        return false;
    }
    return true;
}

/// THE HIT SET's PLACE IN THE REFLECTION SET (HitSetSlots, the one writer): the TLAS (0), the volumes by kind (10-13,
/// 21-24, 29-30, 40, 54), the sky (14), the card read's four (15-18) and its view term (31-37), the geometric normal (19,
/// 20), the record (25-28), the hit's own surface (43, 44).
constexpr HitSetSlots kReflectHitSlots = {
    0u,
    { 10u, 11u, 12u, 13u, kReflectCovBinding, kReflectCovBinding + 1u, kReflectCovBinding + 2u, kReflectCovBinding + 3u,
      kReflectSideBinding, kReflectSideBinding + 1u, kReflectDirectBinding, kReflectDirectBackBinding },
    14u,
    { kReflectCardBinding, kReflectCardBinding + 1u, kReflectCardBinding + 2u, kReflectCardBinding + 3u },
    kReflectCardViewBinding,
    { kReflectGeomBinding, kReflectGeomBinding + 1u },
    { kReflectHitBinding, kReflectHitBinding + 1u, kReflectHitBinding + 2u, kReflectHitBinding + 3u },
    kReflectPoolBinding,
    kReflectCloudBinding,
};
/// A binding of the reflection set the hit set lays out (hitSetLayout).
constexpr VkDescriptorType kHitSetSlot = VK_DESCRIPTOR_TYPE_MAX_ENUM;
/// THE REFLECTION SET'S LAYOUT, one type a binding (rq_reflect.comp's set 0); its
/// pool is planned from the bindings made from it (VkDescriptorPools.h).
constexpr VkDescriptorType kReflectTypes[kReflectBindings] = {
        kHitSetSlot,                                     // 0  (the hit set) tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1  params
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 2  normals
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3  shadow+roughness
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 4  depth
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 5  jahSsrReflection
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 6  history (read)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 7  history (write)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 8  distance (read)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 9  distance (write)
        kHitSetSlot,                                     // 10 (the hit set) voxelIso[]
        kHitSetSlot,                                     // 11 (the hit set) voxelX[]
        kHitSetSlot,                                     // 12 (the hit set) voxelY[]
        kHitSetSlot,                                     // 13 (the hit set) voxelZ[]
        kHitSetSlot,                                     // 14 (the hit set) sky cube
        kHitSetSlot,                                     // 15 (the hit set) the card table
        kHitSetSlot,                                     // 16 (the hit set) the card instance table
        kHitSetSlot,                                     // 17 (the hit set) the card Depth layer
        kHitSetSlot,                                     // 18 (the hit set) the card Radiance layer
        kHitSetSlot,                                     // 19 (the hit set) the per-slot geometry row
        kHitSetSlot,                                     // 20 (the hit set) the geometry rows
        kHitSetSlot,                                     // 21 (the hit set) voxelCovP[] (PHOTON-VOXEL-4)
        kHitSetSlot,                                     // 22 (the hit set) voxelCovN[]
        kHitSetSlot,                                     // 23 (the hit set) voxelPosP[]
        kHitSetSlot,                                     // 24 (the hit set) voxelPosN[]
        kHitSetSlot,                                     // 25 (the hit set) the GPU scene's instances (HIT-SHADE-1)
        kHitSetSlot,                                     // 26 (the hit set) the hit list's records
        kHitSetSlot,                                     // 27 (the hit set) ...its destinations
        kHitSetSlot,                                     // 28 (the hit set) ...its buffer (counters + aux)
        kHitSetSlot,                                     // 29 (the hit set) voxelBack[] (PHOTON-VOXEL-5)
        kHitSetSlot,                                     // 30 (the hit set) voxelNrm[]
        kHitSetSlot,                                     // 31 (the hit set) the card Indirect layer (CARDS-5)
        kHitSetSlot,                                     // 32 (the hit set) ...Flat (ENGINE-POLISH-PHOTON-1)
        kHitSetSlot,                                     // 33 (the hit set) ...ShadowRough
        kHitSetSlot,                                     // 34 (the hit set) ...Albedo
        kHitSetSlot,                                     // 35 (the hit set) ...Normal
        kHitSetSlot,                                     // 36 (the hit set) ...Specular (ENGINE-POLISH-PHOTON-1)
        kHitSetSlot,                                     // 37 (the hit set) ...the sun's visibility (PHOTON-3)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 38 the photon view's overlay (PHOTON-VIEW-1)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 39 the view's id image (REFLECT-MOVERS-1)
        kHitSetSlot,                                     // 40 (the hit set) voxelDirect[] (MOVER-OCCLUSION-1)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 41 the air's table (PHOTON-I-1 fix 5)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 42 the id pass's depth (ID-DEPTH-1)
        kHitSetSlot,                                     // 43 (the hit set) the texture pool (FAR-SUN-2)
        kHitSetSlot,                                     // 44 (the hit set) the cloud field (FAR-SUN-2)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 45 this frame's raw sample (PHOTON-2 1b)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 46 the rest's reference (PHOTON-2 round 2, R1 (f))
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 47 the previous picture (PHOTON-7 stage 6)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 48 ...the depth it was drawn at
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 49 ...the object motion
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 50 the exposure history (PHOTON-7 stage 6 commit 6)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 51 the rest's stop share
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 52 the rest's compact list
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 53 the moving SE instrument
        kHitSetSlot,                                     // 54 (the hit set) voxelDirectBack[] (PHOTON-7 stage 7)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 55 the sample's NEE part (PHOTON-7 stage 11, 11-1b)
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 56 the pick (PHOTON-7 11-2, the reds' instrument)
    };
static_assert(kReflectTypes[kReflectIdBinding] == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
              "the id image is sampled (the writer binds it as a sampler)");
static_assert(kReflectTypes[kReflectPhotonBinding] == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
              "the writer's storage-image bindings are the layout's");

bool RayQueryTier::makeReflectPipelineNow(std::string &err) {
    // THE HIT SET laid out by its one function (kReflectHitSlots), the set's own bindings by the table.
    VkDescriptorSetLayoutBinding b[kReflectBindings] = {};
    hitSetLayout(kReflectHitSlots, b);
    unsigned laidOut = 0u;
    for (unsigned i = 0; i < kReflectBindings; ++i) {
        if (kReflectTypes[i] == kHitSetSlot) {
            ++laidOut;
            continue;
        }
        b[i].binding = i;
        b[i].descriptorType = kReflectTypes[i];
        b[i].descriptorCount = 1u;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (laidOut != kHitSetBindings) {
        err = "rayquery/reflect: the layout table's hit-set slots are not the hit set's";
        return false;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kReflectBindings, 2u * kMaxTimedScenes * kReflectRing, mReflectSetLayout, err, "rayquery/reflect")) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mReflectSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mReflectPipeLayout) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_reflectSpv);
    smi.pCode = krq_reflectSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mReflectModule) != VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateShaderModule failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mReflectModule;
    cpi.stage.pName = "main";
    cpi.layout = mReflectPipeLayout;
    if (createComputePipeline(cpi, &mReflectPipeline, "reflect") !=
        VK_SUCCESS) {
        err = "rayquery/reflect: vkCreateComputePipelines failed";
        return false;
    }
    // THE COMPACT PASS (kReflectCompactTexels): the same shader with its specialisation constant 0 (kCompactPass) true.
    {
        const VkBool32 compactOn = VK_TRUE;
        VkSpecializationMapEntry entry{};
        entry.constantID = 0u;
        entry.offset = 0u;
        entry.size = sizeof(VkBool32);
        VkSpecializationInfo spec{};
        spec.mapEntryCount = 1u;
        spec.pMapEntries = &entry;
        spec.dataSize = sizeof(VkBool32);
        spec.pData = &compactOn;
        VkComputePipelineCreateInfo ccpi = cpi;
        ccpi.stage.pSpecializationInfo = &spec;
        if (createComputePipeline(ccpi, &mReflectCompactPipeline, "reflect-compact") != VK_SUCCESS) {
            err = "rayquery/reflect: the compact pass's vkCreateComputePipelines failed";
            return false;
        }
    }
    {
        VkShaderModuleCreateInfo fsmi{};
        fsmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        fsmi.codeSize = sizeof(krq_reflectFilterSpv);
        fsmi.pCode = krq_reflectFilterSpv;
        if (vkCreateShaderModule(mVk, &fsmi, nullptr, &mFilterModule) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateShaderModule (filter) failed";
            return false;
        }
        VkComputePipelineCreateInfo fcpi{};
        fcpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        fcpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fcpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        fcpi.stage.module = mFilterModule;
        fcpi.stage.pName = "main";
        fcpi.layout = mReflectPipeLayout;
        if (createComputePipeline(fcpi, &mFilterPipeline, "reflect filter") !=
            VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateComputePipelines (filter) failed";
            return false;
        }
    }
    {
        VkShaderModuleCreateInfo tsmi{};
        tsmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        tsmi.codeSize = sizeof(krq_reflectTemporalSpv);
        tsmi.pCode = krq_reflectTemporalSpv;
        if (vkCreateShaderModule(mVk, &tsmi, nullptr, &mTemporalModule) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateShaderModule (temporal) failed";
            return false;
        }
        VkComputePipelineCreateInfo tcpi{};
        tcpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        tcpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tcpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        tcpi.stage.module = mTemporalModule;
        tcpi.stage.pName = "main";
        tcpi.layout = mReflectPipeLayout;
        if (createComputePipeline(tcpi, &mTemporalPipeline, "reflect temporal") != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateComputePipelines (temporal) failed";
            return false;
        }
    }
    {
        VkShaderModuleCreateInfo rsmi{};
        rsmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        rsmi.codeSize = sizeof(krq_reflectResolveSpv);
        rsmi.pCode = krq_reflectResolveSpv;
        if (vkCreateShaderModule(mVk, &rsmi, nullptr, &mResolveModule) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateShaderModule (resolve) failed";
            return false;
        }
        VkComputePipelineCreateInfo rcpi{};
        rcpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        rcpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        rcpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        rcpi.stage.module = mResolveModule;
        rcpi.stage.pName = "main";
        rcpi.layout = mReflectPipeLayout;
        if (createComputePipeline(rcpi, &mResolvePipeline, "reflect resolve") != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateComputePipelines (resolve) failed";
            return false;
        }
    }
    {
        VkShaderModuleCreateInfo csmi{};
        csmi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        csmi.codeSize = sizeof(krq_reflectComposeSpv);
        csmi.pCode = krq_reflectComposeSpv;
        if (vkCreateShaderModule(mVk, &csmi, nullptr, &mComposeModule) != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateShaderModule (compose) failed";
            return false;
        }
        VkComputePipelineCreateInfo ccpi{};
        ccpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ccpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ccpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ccpi.stage.module = mComposeModule;
        ccpi.stage.pName = "main";
        ccpi.layout = mReflectPipeLayout;
        if (createComputePipeline(ccpi, &mComposePipeline, "reflect compose") != VK_SUCCESS) {
            err = "rayquery/reflect: vkCreateComputePipelines (compose) failed";
            return false;
        }
    }
    // The pool its layout planned: kMaxTimedScenes views' worth of rings (far
    // more views than a product frame draws), two modes a scene (the movers'
    // view and the still view), a ring each.
    if (!plan.create(mVk, "rayquery/reflect", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mReflectPool, err))
        return false;
    return ensureSamplers(err);
}

bool RayQueryTier::ensureReflectImages(ReflectView &rv, unsigned w, unsigned h, unsigned layers, bool reference,
                                       std::string &err) {
    if (rv.imagesReady && rv.w == w && rv.h == h && rv.layers == layers && rv.referenceFull == reference) return true;
    // A RESIZE INVALIDATES THE MEAN, which is correct and not a loss: the
    // history is screen space, and every pixel of it now means a different
    // direction. The images are RETIRED rather than destroyed — frames that
    // still reference them are in flight.
    for (int i = 0; i < 2; ++i) {
        retireImage(rv.hist[i]);
        retireImage(rv.state[i]);
    }
    retireImage(rv.sample);
    retireImage(rv.nee);
    retireImage(rv.reference);
    retireImage(rv.share);
    rv.imagesReady = false;
    rv.havePrev = false;
    rv.historyFrames = 0;
    rv.w = w; rv.h = h;
    rv.layers = layers;
    // The second image, THE STATE TEXEL (PHOTON-7 stage 6, L8; jah_reflect_state.glsl): the surface's distance (the
    // reprojection's validity test) and the mean distance the rays in the mean travelled as halves, the counts, the
    // rest's stop and this frame's estimator as integer fields. 8 bytes a trace texel: a record's ray length and origin
    // height ride the RECORD (jah_rq_hit_record.glsl), never a full-resolution image.
    const VkFormat formats[2] = { VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32_UINT };
    for (int i = 0; i < 2; ++i) {
        // ONE LAYER PER EYE (LAYERED-STEREO-1): the trace's z is the eye.
        if (!makeStorageImage(w, h, formats[0], rv.hist[i], err, layers)) return false;
        if (!makeStorageImage(w, h, formats[1], rv.state[i], err, layers)) return false;
    }
    if (!makeStorageImage(w, h, formats[0], rv.sample, err, layers)) return false;
    if (!makeStorageImage(w, h, formats[0], rv.nee, err, layers)) return false;
    if (!makeStorageImage(w, h, VK_FORMAT_R32_UINT, rv.share, err, layers)) return false;   // the stop share: a float's bits, atomic min
    // (the arm toggled re-makes the whole set: a measuring arm, set before the session's measurement)
    if (!makeStorageImage(reference ? w : 1u, reference ? h : 1u, VK_FORMAT_R32G32B32A32_SFLOAT, rv.reference, err,
                          layers))
        return false;
    rv.referenceFull = reference;
    rv.imagesReady = true;
    rv.needsClear = true;
    return true;
}

void RayQueryTier::clearReflectImages(ReflectView &rv, VkCommandBuffer cmd) {
    if (!rv.needsClear) return;
    rv.needsClear = false;
    // UNDEFINED -> GENERAL, and CLEARED: a distance of zero is what
    // rq_reflect.comp reads as "no history here", so a freshly made pair is
    // rejected by the reprojection test rather than believed.
    // AFTER WHAT TOUCHED THEM (PHOTON-7 stage 0, AB E19): a CAMERA CUT clears images
    // the frames before wrote and read - the trace, the temporal, the filter and the
    // hit write-back (compute) and the history readback's copy (transfer) - so the
    // clear waits for those stages and makes their writes available before it writes;
    // a freshly made image has no earlier access and the same barrier costs it nothing.
    // The old contents are discarded (UNDEFINED): they are cleared.
    constexpr int kImages = 8;
    VkImageMemoryBarrier toGeneral[kImages] = {};
    ReflectImage *imgs[kImages] = { &rv.hist[0], &rv.hist[1], &rv.state[0], &rv.state[1], &rv.sample, &rv.reference,
                                    &rv.share, &rv.nee };
    for (int i = 0; i < kImages; ++i) {
        toGeneral[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral[i].srcQueueFamilyIndex = toGeneral[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral[i].image = imgs[i]->image;
        toGeneral[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toGeneral[i].subresourceRange.levelCount = 1;
        toGeneral[i].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        toGeneral[i].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        toGeneral[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, kImages, toGeneral);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = VK_REMAINING_ARRAY_LAYERS;
    for (int i = 0; i < kImages; ++i)
        vkCmdClearColorImage(cmd, imgs[i]->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    VkMemoryBarrier toCompute{};
    toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toCompute, 0, nullptr, 0, nullptr);
}

void RayQueryTier::dropReflect(ReflectView &rv) {
    for (unsigned i = 0; i < 3u; ++i) {
        if (rv.motionSets[i]) retireSet(rv.motionSets[i], mMotionPool);
        rv.motionSets[i] = VK_NULL_HANDLE;
        retire(rv.motionParams[i]);
        retire(rv.motionGeomRowOfSlot[i]);
    }
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(rv.sets[i]);
        rv.sets[i] = VK_NULL_HANDLE;
        retire(rv.params[i]);
        retire(rv.geomRowOfSlot[i]);
    }
    for (int i = 0; i < 2; ++i) { retireImage(rv.hist[i]); retireImage(rv.state[i]); }
    retireImage(rv.sample);
    retireImage(rv.nee);
    retireImage(rv.reference);
    retireImage(rv.share);
    retire(rv.compactList);
    retire(rv.histReadback);
    retire(rv.movingSe);
    retire(rv.pick);
    retire(rv.seReadback);
    for (ReflectView::HistPending &p : rv.histPending) p = ReflectView::HistPending();
    rv.histHost.clear();
    rv.imagesReady = false;
    rv.needsClear = false;
}

void RayQueryTier::forgetReflect(const ReflectPassListener *key) {
    auto it = mReflects.find(key);
    if (it == mReflects.end()) return;
    // THE SHOT-VIEW FOLD (mForgottenReflects): the rest an offscreen view (a shot) photographed, kept past it.
    const OgreScene *scene = it->second.scene;
    if (scene && key && key->mFoldsOnForget)
        mForgottenReflects[scene] = ForgottenReflect{ it->second.restFrames, it->second.restSession, it->second.held };
    dropReflect(it->second);
    mReflects.erase(it);
    monitor::forgetGpuOwner(key);
}

void RayQueryTier::reflectStatsInto(const OgreScene *scene, RayQueryStatus &st) const {
    for (const auto &kv : mReflects) {
        if (kv.second.scene != scene) continue;
        st.reflect = true;
        st.reflectRays += int(kv.second.rays);
        // The views' GPU times are the monitor's rows (lane TEST-1): the worst view.
        const float tr = monitor::lastGpuMs("rq.reflect.trace", kv.first);
        const float fi = monitor::lastGpuMs("rq.reflect.filter", kv.first);
        const float te = monitor::lastGpuMs("rq.reflect.temporal", kv.first);
        const float re = monitor::lastGpuMs("rq.reflect.resolve", kv.first);
        if (tr >= 0.0f || fi >= 0.0f || te >= 0.0f || re >= 0.0f)
            st.reflectMs = std::max(st.reflectMs, std::max(tr, 0.0f) + std::max(re, 0.0f) + std::max(te, 0.0f) +
                                                      std::max(fi, 0.0f));
        st.reflectMotionMs = std::max(st.reflectMotionMs, monitor::lastGpuMs("rq.reflect.motion", kv.first));
        // THE HISTORY COUNT (PHOTON-2 stage 0): the view that holds a decoded copy (one view per scene in the
        // stillness door's use; with several, the last in the map's order that holds one).
        if (!kv.second.histHost.empty()) {
            st.reflectHistoryCount = kv.second.histHost;
            st.reflectHistoryW = kv.second.histHostW;
            st.reflectHistoryH = kv.second.histHostH;
            st.reflectHistoryFrame = kv.second.histHostFrame;
            st.reflectHistorySe = kv.second.histHostSe;
        }
        // THE PICK's RING (the arm reflect.pick): copied while the arm is on - the lab reads it back
        if (mArms && mArms->value(ArmId::ReflectPick) >= 1.0 && kv.second.pick.mapped) {
            const uint32_t *pw = static_cast<const uint32_t *>(kv.second.pick.mapped);
            st.reflectPick.assign(pw, pw + size_t(kReflectPickSlots) * kReflectPickWords);
            st.reflectPickFrames.assign(kv.second.pickFrames, kv.second.pickFrames + kReflectPickSlots);
        }
        st.reflectFrame = std::max(st.reflectFrame, kv.second.frame);
        // THE REST (R1, G6)
        const ReflectView &rv = kv.second;
        st.reflectRestFrames = rv.restFrames;
        st.reflectSources = rv.sources;
        st.reflectHeld = rv.held;
        st.reflectRests = rv.restSession;
        st.reflectRestMoverFrames = rv.restMoverCrossings;
        st.reflectCameraCuts = std::max(st.reflectCameraCuts, rv.cameraCuts);
        st.reflectRestActive = -1;
        st.reflectRestCapped = -1;
        if (const auto h = mHits.find(kv.first); h != mHits.end() && rv.restFrames > 0u) {
            const uint32_t tag = h->second.restActiveTag;
            if (tag && (tag >> 20u) == (rv.restSession & 0xFFFu) && (tag & 0xFFFFFu) > rv.restMoverFrame) {
                st.reflectRestActive = int(h->second.restActive);
                st.reflectRestCapped = int(h->second.restCapped);
            }
        }
        if (const auto h = mHits.find(kv.first); h != mHits.end()) {
            st.reflectNeeRays += h->second.neeRays;
            st.reflectNeeHits += h->second.neeHits;
            // THE REST'S BOUND (PHOTON-7 stage 11, 11-1c): the samples the texels converging at rest frame 2N still owe,
            // over the tail's rays a frame (the rest's count / kReflectRestTailDivisor), past 2N, plus the stop's frames
            const uint32_t tag = h->second.boundTag;
            if (rv.restFrames > 0u && tag && (tag >> 20u) == (rv.restSession & 0xFFFu)) {
                const unsigned long long tail =
                    std::max<unsigned long long>(rv.restRayCount / kReflectRestTailDivisor, 1ull);
                st.reflectRestBoundOwed = h->second.boundOwed;
                st.reflectRestNeedMax = h->second.boundNeedMax;
                st.reflectRestBound = int(2u * rv.restN + unsigned((h->second.boundOwed + tail - 1ull) / tail) +
                                          kReflectStopFrames);
            }
        }
    }
    // THE SHOT-VIEW FOLD (mForgottenReflects): the last destroyed view's rest, as the last shot's
    if (const auto f = mForgottenReflects.find(scene); f != mForgottenReflects.end()) {
        st.reflectShotValid = true;
        st.reflectShotRestFrames = f->second.restFrames;
        st.reflectShotHeld = f->second.held;
        st.reflectShotRests = f->second.restSession;
    }
}

RayQueryTier::EyeBasisF RayQueryTier::eyeBasis(bool ortho, const Ogre::Vector3 &pos,
                                               const Ogre::Quaternion &rot, float el, float er,
                                               float et, float eb) {
    return cameraImage(ortho, pos, rot, el, er, et, eb);
}

void RayQueryTier::expandEyeToTarget(EyeBasisF &e, const ChainDesc &cd, unsigned fullW,
                                     unsigned fullH) {
    expandCameraImageToTarget(e, cd, fullW, fullH);
}

void RayQueryTier::recordReflect(const ReflectPassListener *key, OgreView *view,
                                 Ogre::CompositorPass *pass, const HitListBinding &hit) {
    joinAhead(kAheadReflect);   // (its members are read below: the ahead build is joined first)
    if (!isOpen() || mReflectFailed || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;

    // THE STRUCTURE. Every one of these is a legitimate "not this frame", and
    // each leaves `jahSsrReflection` holding exactly what the resolve wrote —
    // i.e. today's picture, which is the whole point of the fallback contract.
    SceneAs *saP = asOf(scene);
    if (!saP) return;
    SceneAs &sa = *saP;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) return;

    if (!mReflectPipeline) {
        std::string err;
        if (!makeReflectPipeline(err)) {
            mReflectFailed = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: ray-traced reflections off — " + err +
                " (the probe and the sky answer the reflection this frame)");
            return;
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: ray-traced reflections ON (PHOTON R5) — one estimator: the screen trace "
            "answers what it can vouch for, rays answer the rest");
    }

    // THE CHAIN'S TEXTURES, by the names OgreChain.cpp declares them under. A
    // chain shape that does not carry them is not an error: it is a view whose
    // SSR row is off, and this listener should not have been created for it.
    Ogre::TextureGpu *ssrTex = nullptr, *normalTex = nullptr, *roughTex = nullptr,
                     *depthTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        ssrTex    = node->getDefinedTexture(Ogre::IdString("jahSsrReflection"));
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        roughTex  = node->getDefinedTexture(Ogre::IdString("jahSsrShadowRough"));
        depthTex  = node->getDefinedTexture(Ogre::IdString("jahDepth"));
    } catch (Ogre::Exception &) { return; }
    if (!ssrTex || !normalTex || !roughTex || !depthTex) return;
    // THE VIEW'S ID IMAGE (REFLECT-MOVERS-1): present when the chain carries the id
    // pass (ChainDesc::atomDraw), which ran before every scene pass of the frame.
    // Absent is not an error — every surface then takes the camera path.
    // ASKED ONLY OF A CHAIN THAT DECLARES IT (ChainDesc::atomDraw; a stereo chain
    // never does): Ogre LOGS every getDefinedTexture miss as an exception before the
    // catch — per frame, on every VR eye (the gate's vr.eye_grade read 4,254 of them).
    Ogre::TextureGpu *idTex = nullptr;
    if (view->chainDesc().atomDraw && !view->stereo()) {
        try {
            idTex = node->getDefinedTexture(Ogre::IdString(kAtomIdTexture));
        } catch (Ogre::Exception &) { idTex = nullptr; }
    }
    if (idTex && (idTex->getWidth() != ssrTex->getWidth() || idTex->getHeight() != ssrTex->getHeight()))
        idTex = nullptr;   // a different picture's ids
    // ...AND THE ID PASS'S OWN DEPTH (ID-DEPTH-1, the velocity jobs' test, here for the
    // reflection): the chain copies it every frame wherever the ray tier reflects over
    // an id pass (OgreChain.cpp). Without it the ids are not read at all — an id with
    // no depth to say whether the pixel still shows its surface is the defect.
    Ogre::TextureGpu *idDepthTex = nullptr;
    if (idTex) {
        try {
            idDepthTex = node->getDefinedTexture(Ogre::IdString("jahAtomViewDepth"));
        } catch (Ogre::Exception &) { idDepthTex = nullptr; }
        if (!idDepthTex || idDepthTex->getWidth() != idTex->getWidth() ||
            idDepthTex->getHeight() != idTex->getHeight())
            idTex = idDepthTex = nullptr;
    }
    // THE SCREEN TRACE'S PICTURE (PHOTON-7 stage 6, the one estimator's first source): this view's previous picture,
    // the depth it was drawn at and the object motion — declared only where the chain carries its SCREEN SOURCES
    // (ChainDesc::screenSources: reflectSources Both or Screen, a mono view), and asked only of such a chain (Ogre logs
    // every getDefinedTexture miss). All three present and the picture the view's size, or the screen trace is off this
    // frame and the rays answer alone.
    const ChainDesc chainD = view->chainDesc();
    Ogre::TextureGpu *prevColourTex = nullptr, *prevDepthTex = nullptr, *velocityTex = nullptr;
    if (chainD.screenSources() && !view->stereo()) {
        try {
            prevColourTex = node->getDefinedTexture(Ogre::IdString("jahSsrPrev"));
            prevDepthTex = node->getDefinedTexture(Ogre::IdString("jahSsrPrevDepth"));
            velocityTex = node->getDefinedTexture(Ogre::IdString("jahSsrVelocity"));
        } catch (Ogre::Exception &) { prevColourTex = prevDepthTex = velocityTex = nullptr; }
        if (!prevColourTex || !prevDepthTex || !velocityTex || prevColourTex->getWidth() != ssrTex->getWidth() ||
            prevColourTex->getHeight() != ssrTex->getHeight() || prevDepthTex->getWidth() != ssrTex->getWidth() ||
            prevDepthTex->getHeight() != ssrTex->getHeight())
            prevColourTex = prevDepthTex = velocityTex = nullptr;
    }
    const bool screenTrace = prevColourTex != nullptr;
    // THE UAV FLAG IS THE CONTRACT (OgreChain.cpp, ChainDesc::rayReflect). A
    // texture declared without it has no storage-image usage and the view
    // creation below would be a validation error, so a chain built while the
    // machine's answer was "no rays" is declined here rather than trusted.
    if (!ssrTex->isUav()) return;

    const unsigned fullW = ssrTex->getWidth(), fullH = ssrTex->getHeight();
    if (!fullW || !fullH) return;
    const int ssrRow = view->chainDesc().ssr;
    // ---- ONE TRACE, TWO EYES (lane REFLECT-VR-1, LAYERED-STEREO-1) ----------
    // A STEREO target is two layers, one eye each at the same origin, so the
    // trace covers both in one dispatch whose z is the eye, and every screen
    // image it reads or writes is per layer — no seam exists to fall anywhere.
    // THE EYES MUST BE KNOWN: they are the runtime's, pushed onto the View by
    // the session each located frame; a stereo view without them declines
    // rather than tracing one mono answer for two eyes.
    const bool stereo = view->stereo();
    const StereoEyeBasis *eyes = view->stereoEyes();
    unsigned traceW = ssrRow >= 2 ? fullW : std::max(1u, fullW / 2u);
    unsigned traceH = ssrRow >= 2 ? fullH : std::max(1u, fullH / 2u);
    // A STEREO VIEW'S TEXTURES ARE LAYERED, one eye per layer (LAYERED-STEREO-1):
    // fullW is one eye's, the trace is per layer, and the dispatch's z is the eye.
    const unsigned eyeLayers = ssrTex->getNumSlices();
    if (stereo != (eyeLayers == 2u)) return;   // a chain that is not the view's shape yet

    // ---- THE HIT SET (the tier's one collection, hitSetOf): the cascades the hits are shaded from (route A,
    // innermost first), the one environment, the surface cache the hits read first, the geometric normal's rows, the
    // hit's own surface, the alpha table and the movers; the hit list is this trace's own. NO VOXELS IS NOT "NO
    // REFLECTIONS": an escaping ray still reads the sky, and in an open scene that is the whole answer. A HIT with no
    // cache behind it is what the shader declines (see its note) - it hands the pixel back to the probe.
    HitSetSources hs;
    hs.hit = hit;
    hitSetOf(scene, sa, hs);
    const unsigned voxCount = hs.cascadeCount;
    const bool cardsBound = hs.cardsBound();

    // ---- PER-VIEW STATE -----------------------------------------------------
    ReflectView &rv = mReflects[key];
    rv.scene = scene;
    // A FRAME THAT DECLINES REPORTS ZERO (second reader, L4). Every early
    // return below leaves the picture correct, but leaving `rays` at the last
    // frame's value makes `giStatus().rayQuery.reflectRays` say a trace is
    // running when none is — the one reading a caller would use to find out.
    rv.rays = 0;
    /// EVERY EARLY RETURN BELOW IS A LEGITIMATE "not this frame" and leaves
    /// `jahSsrReflection` holding exactly what the resolve wrote — today's
    /// picture. The log names which one, ONCE per reason per process: a silent
    /// decline is indistinguishable from a trace that draws nothing, and one that
    /// persists must not flood the log (it replaced the JAH_R5_WHY switch).
    const auto bail = [](const char *reason) {
        static std::vector<const char *> said;   // the render thread's, like this pass
        if (std::find(said.begin(), said.end(), reason) != said.end()) return;
        said.push_back(reason);
        Ogre::LogManager::getSingleton().logMessage(std::string("R5 declined: ") + reason);
    };

    std::string err;
    if (!ensureDummyImages(err)) {
        mReflectFailed = true;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: ray-traced reflections off — " + err);
        return;
    }
    if (!ensureReflectImages(rv, traceW, traceH, eyeLayers, arm(ArmId::ReflectRestReference, false), err)) {
        mReflectFailed = true;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: ray-traced reflections off — " + err);
        return;
    }
    const unsigned ring = rv.ringFrame++ % kReflectRing;
    if (!rv.params[ring].buffer &&
        !makeBuffer(sizeof(ReflectParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    rv.params[ring], err))
        { bail("makeBuffer(params)"); return; }
    if (!rv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mReflectPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mReflectSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &rv.sets[ring]) != VK_SUCCESS) {
            rv.sets[ring] = VK_NULL_HANDLE;
            bail("vkAllocateDescriptorSets"); return;
        }
    }

    // ---- THE PARAMETERS -----------------------------------------------------
    ReflectParams pp{};
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Vector3 camPos = cam->getDerivedPosition();
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    const Ogre::Vector3 fwd = q * Ogre::Vector3::NEGATIVE_UNIT_Z;
    const Ogre::Vector3 right = q * Ogre::Vector3::UNIT_X;
    const Ogre::Vector3 up = q * Ogre::Vector3::UNIT_Y;
    // THE IMAGE PLANE FROM THE FRUSTUM ITSELF, not from fov and aspect: the
    // extents carry a frustum OFFSET, a manually set extent and the projection
    // type, and every one of those is a silent whole-image error if the basis
    // and the projection disagree. Tangents for a perspective camera (the basis
    // is a RAY per pixel), view-space positions for an orthographic one (the
    // basis is an OFFSET per pixel, constant in depth) — which is the same split
    // the shader makes, from the same flag.
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb,
                           ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    // ONE IMAGE'S BASIS, from a pose and a frustum (lane REFLECT-VR-1). The
    // arithmetic is exactly what the single-camera path did; it is a function
    // now because a stereo target needs it twice, from two poses and two
    // frustums, and having two copies of it is how they would drift apart.
    const auto makeEye = [ortho](const Ogre::Vector3 &pos, const Ogre::Quaternion &rot,
                                 float el, float er, float et, float eb) {
        return eyeBasis(ortho, pos, rot, el, er, et, eb);
    };
    // THE EYES, OR THE ONE CAMERA. A stereo view whose eyes have not been
    // pushed yet declines: tracing the head's frustum for both eyes maps each
    // eye's picture onto the head's one frustum, which is not a small
    // error but a different picture (the reflection of anything off the head's
    // axis lands in the wrong eye or in neither — the owner's "no reflections
    // in the headset", measured).
    EyeBasisF eyeB[2];
    // THE ARM THAT RE-MEASURES THE CLAIM: the arm "reflect.monoEyes" traces a stereo target through the RENDERING
    // camera for both halves — the behaviour before lane REFLECT-VR-1 — so the
    // cost of getting this wrong can be measured rather than argued. On the
    // `vr.session` mirror fixture it moves an eye from a mean of 0.34/255
    // against its own mono control (0.39 % of bytes over 8) to 2.70 at 5.6 % in
    // the LEFT eye and 6.26 at 12.8 % in the RIGHT one — the asymmetry being
    // that the rendering camera carries the left eye's projection, so one
    // camera is nearly right for one half and wrong for the other.
    // (A measurement arm, latched per frame — lane TEST-1 deleted its env door.)
    const bool monoEyes = stereo && arm(ArmId::ReflectMonoEyes, false);
    if (stereo && !monoEyes) {
        if (!eyes) { bail("a stereo view with no located eyes"); return; }
        for (int i = 0; i < 2; ++i)
            eyeB[i] = makeEye(eyes[i].position, eyes[i].orientation, eyes[i].tanLeft,
                              eyes[i].tanRight, eyes[i].tanTop, eyes[i].tanBottom);
    } else {
        eyeB[0] = makeEye(camPos, q, float(fl), float(fr), float(ft), float(fb));
        // Written, never read in the mono case (`stereo.x` is 0): a uniform
        // buffer with a half-initialised tail is a thing to read in a debugger
        // one day. Under the measurement arm above it IS read, and reading the
        // same basis for both halves is exactly what the arm reproduces.
        eyeB[1] = eyeB[0];
    }
    // THE LETTERBOX (SSR-LETTERBOX-1's ray half). Under a constrained-aspect
    // camera the picture is the target's INNER rectangle (chain::letterboxRect),
    // while this pass addresses the whole target: so the image basis is
    // EXPANDED to the target — the same conjugation the march takes in its
    // pass buffer, here on the CPU so no shader line moves. A target uv t is the
    // shot's (t - x0) / w, hence rayTL' = rayTL - rayRight x0/w - rayDown y0/h,
    // rayRight' = rayRight / w, rayDown' = rayDown / h. The bars hold cleared
    // depth and are declined before any ray; the previous-frame basis is the
    // expanded one too (rv.prev is written from eyeB below), so the
    // reprojection's uv spans the same target.
    {
        const ChainDesc cd = view->chainDesc();
        for (int i = 0; i < 2; ++i) expandEyeToTarget(eyeB[i], cd, fullW, fullH);
    }
    memcpy(pp.camPos, eyeB[0].camPos, sizeof(pp.camPos));
    memcpy(pp.rayTL, eyeB[0].rayTL, sizeof(pp.rayTL));
    memcpy(pp.rayRight, eyeB[0].rayRight, sizeof(pp.rayRight));
    memcpy(pp.rayDown, eyeB[0].rayDown, sizeof(pp.rayDown));
    memcpy(pp.fwd, eyeB[0].fwd, sizeof(pp.fwd));
    memcpy(pp.camPos2, eyeB[1].camPos, sizeof(pp.camPos2));
    memcpy(pp.rayTL2, eyeB[1].rayTL, sizeof(pp.rayTL2));
    memcpy(pp.rayRight2, eyeB[1].rayRight, sizeof(pp.rayRight2));
    memcpy(pp.rayDown2, eyeB[1].rayDown, sizeof(pp.rayDown2));
    memcpy(pp.fwd2, eyeB[1].fwd, sizeof(pp.fwd2));
    // ...and the flag the shader splits on. Under the measurement arm it is 0
    // on a stereo target, which is precisely the pre-lane shape: ONE image, the
    // rendering camera's, stretched across two eyes' worth of pixels. (What the
    // arm cannot reproduce is that before fork 6130df9d1 (was 0078) that camera's
    // extents were INDETERMINATE as well, because it carries a custom
    // projection matrix — so the shipped defect was the sum of the two.)
    pp.stereo[0] = (stereo && !monoEyes) ? 1.0f : 0.0f;
    // THE VIEW AXES ARE THE RENDERING CAMERA'S, FOR BOTH EYES, AND THAT IS
    // EXACT rather than an approximation (the pin, checked): the G-buffer
    // normal is written in the pass camera's view space — HlmsPbs uploads ONE
    // `mat4 view` per pass and instanced stereo does not make it two
    // (OgreHlmsPbs.cpp:2327) — so under stereo both halves' normals are in the
    // HEAD's view space and this one rotation is what turns either of them back
    // into the world. (It is also why the eyes' own orientations do not enter
    // here: a per-eye rotation applied to a head-space normal would TILT it.)
    put3(pp.viewAxisX, right, 0.0f);
    put3(pp.viewAxisY, up, 0.0f);
    put3(pp.viewAxisZ, -fwd, 0.0f);          // Ogre's view space looks down -Z
    Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    pp.projParams[0] = projAB.x;
    pp.projParams[1] = projAB.y;
    pp.projParams[2] = cam->getFarClipDistance();
    pp.projParams[3] = kRayReflectFeather;
    pp.resolution[0] = float(traceW); pp.resolution[1] = float(traceH);
    pp.resolution[2] = float(fullW);  pp.resolution[3] = float(fullH);
    // THE CUTOFF IS THE PROJECT'S (PostFxDesc::reflectionRoughnessCutoff) and the same
    // one the screen-space march gates on since lane SSR-3; the engine only
    // clamps it into the range a reflection means anything in.
    pp.knobs[0] = std::min(std::max(view->chainDesc().reflectionRoughnessCutoff, 0.0f), 1.0f);
    // THE RAY'S LENGTH: THE ONE REACH RULE (detail::photonRayReach — the gather
    // reads the same function).
    pp.knobs[1] = detail::photonRayReach(
        voxCount ? Ogre::Vector3(hs.voxelSize[voxCount - 1u][0], hs.voxelSize[voxCount - 1u][1],
                                 hs.voxelSize[voxCount - 1u][2]).length()
                 : 0.0f,
        cam->getFarClipDistance());
    pp.knobs[2] = float(rv.frame & 0xFFFFu);
    // THE NEE (PHOTON-7 11-1b): 1 = next-event estimation toward the emitter table with the BRDF draw by the balance
    // heuristic; the arm "reflect.nee" 0 = the BRDF-only estimator (the acceptance's reference)
    pp.knobs[3] = arm(ArmId::ReflectNee, true) ? 1.0f : 0.0f;
    // THE ONE ESTIMATOR'S SOURCES (PHOTON-7 stage 6): the screen trace (its march: the chain's length, thickness, steps
    // and phase rule - the raster march's own numbers, PostFxDesc::ssr*), the screen alone, and the FAR COPIES' query
    // for an escape (from the near length less the far copies' widest hand-over gap to the far plane: the gather's
    // continuation, OgreScreenProbeGather.cpp).
    pp.screen[0] = screenTrace ? 1.0f : 0.0f;
    pp.screen[1] = screenTrace && chainD.reflectSources == ReflectSources::Screen ? 1.0f : 0.0f;
    rv.sources = int(!screenTrace ? ReflectSources::Rays
                                  : chainD.reflectSources == ReflectSources::Screen ? ReflectSources::Screen
                                                                                   : ReflectSources::Both);
    pp.screen[2] = chainD.ssrSteps > 0 ? float(std::min(std::max(chainD.ssrSteps, 8), 128))
                                       : (chainD.ssr >= 2 ? 96.0f : 48.0f);
    pp.screen[3] = float(chainD.ssrMarchPhase);
    pp.screen2[0] = chainD.ssrMaxDistance;
    pp.screen2[1] = chainD.ssrThickness;
    {
        const float farPlane = cam->getFarClipDistance() > 0.0f ? float(cam->getFarClipDistance()) : 1000.0f;
        pp.screen2[2] = std::max(0.001f, pp.knobs[1] - std::max(0.0f, sa.farOverlap));
        pp.screen2[3] = farPlane > pp.knobs[1] ? farPlane : 0.0f;
    }
    // THE RAY ORIGIN'S DRAWN TERM (PHOTON-7 stage 6, AB E15, READ_B F3 - the one law, Types.h): the view's pixel
    // tolerance (the World Mode's atomPixelError, Scene::pixelTolerance) x the LOD bias - what the id pass's cut and
    // the stock thresholds both take - over this view's switch band, x the bake's drift ratio, in sample footprints;
    // rq_reflect.comp turns it into metres at the point's Euclidean distance from its own eye (jah_ray_origin.glsl).
    pp.knobs2[1] = drawnFootprintsOf(scene, chainD.lodHysteresis);
    // THE MOVING WINDOW (kReflectMaxFrames; the measuring arm reflect.window N, every reader with it)
    const unsigned reflectWindow = std::lround(armValue(ArmId::ReflectWindow)) > 0
                                       ? unsigned(std::lround(armValue(ArmId::ReflectWindow))) : kReflectMaxFrames;
    pp.knobs2[3] = 1.0f / float(reflectWindow);   // the moving memory (the shaders' 1 / knobs2.w)
    // THE DENOISER SPACE'S EXPOSURE (PHOTON-7 stage 6 commit 6: Lumen's space is on PRE-EXPOSED colour): the multiplier
    // the tonemapper scales the scene by - the chain's measured history (its 1 x 1: last frame's, read on the GPU, never
    // a readback), the fixed tonemap's constant, or 1 where nothing tonemaps.
    Ogre::TextureGpu *exposureTex = nullptr;
    if (chainD.hdr && !chainD.tonemapFixed) {
        const Ogre::IdString lum(chain::exposureHistoryTextureName());
        const auto &names = node->getDefinition()->getNameToChannelMap();
        if (names.find(lum) != names.end()) exposureTex = node->getDefinedTexture(lum);
    }
    fillDisplaySlope(pp.display, chainD.hdr, chainD.image);
    pp.denoiser[0] = chainD.hdr && chainD.tonemapFixed
                         ? chain::fixedExposureColour(chainD.exposureScale, chainD.exposure).r : 1.0f;
    pp.denoiser[1] = exposureTex ? 1.0f : 0.0f;
    // THE LOBE DRAW'S SEED OFFSET (the measuring arm reflect.seedOffset: 0 shipped).
    pp.denoiser[2] = float(std::lround(armValue(ArmId::ReflectSeedOffset)));
    pp.denoiser[3] = arm(ArmId::ReflectFlatHits, false) ? 1.0f : 0.0f;   // the measuring arm reflect.flatHits
    // THE GI FREEZE (M1's measuring arm): the reflection keeps its histories under 1; under 2 (THE WORLD HELD) the caches
    // and the gather are held and the reflection runs live - its own estimator measured alone.
    pp.rest[0] = std::lround(armValue(ArmId::GiFreeze)) == 1 ? 1.0f : 0.0f;
    const bool restReference = arm(ArmId::ReflectRestReference, false);
    pp.rest[1] = restReference ? 1.0f : 0.0f;   // THE REST'S 4,096-SAMPLE REFERENCE (R1 (f)'s measuring arm)
    // THE REST'S RAYS A FRAME (commit 8: the trace's batch at rest, GiQualityFacts::reflectRestRays)
    pp.rest[2] = float(std::max(1u, giQualityFacts(scene->giParams().quality).reflectRestRays));
    // NO PREVIOUS FRAME — OR A DIFFERENT SHAPE OF ONE. A zero forward makes
    // every reprojection's `z` zero, which the shader rejects, so the first
    // frame after a resize, a scene bind or a workspace rebuild starts its mean
    // from scratch instead of reading a buffer that means something else. A
    // view that has just BECOME stereo (or stopped being) is the same case: its
    // history's two halves were one picture, or its one picture is now two.
    // ...AND A CAMERA CUT (PHOTON-2 round 2, N3: isCameraCut, the gather's rule - Lumen's): the histories restart - the
    // images cleared this frame (a distance of zero is "no history here") and the view's age back to 0 (the young
    // view's fade-in) - instead of ghosting through what the reprojection's tests happened to accept.
    if (rv.havePrev && rv.prevStereo == stereo &&
        isCameraCut(rv.prev[0].camPos, rv.prev[0].rayRight, rv.prev[0].rayDown, rv.prev[0].fwd, eyeB[0].camPos,
                    eyeB[0].rayRight, eyeB[0].rayDown, eyeB[0].fwd)) {
        rv.havePrev = false;   // the branch below withholds the previous basis and zeroes the age
        rv.needsClear = true;
        ++rv.cameraCuts;
    }
    if (rv.havePrev && rv.prevStereo == stereo) {
        memcpy(pp.prevCamPos, rv.prev[0].camPos, sizeof(pp.prevCamPos));
        memcpy(pp.prevRayTL, rv.prev[0].rayTL, sizeof(pp.prevRayTL));
        memcpy(pp.prevRayRight, rv.prev[0].rayRight, sizeof(pp.prevRayRight));
        memcpy(pp.prevRayDown, rv.prev[0].rayDown, sizeof(pp.prevRayDown));
        memcpy(pp.prevFwd, rv.prev[0].fwd, sizeof(pp.prevFwd));
        memcpy(pp.prevCamPos2, rv.prev[1].camPos, sizeof(pp.prevCamPos2));
        memcpy(pp.prevRayTL2, rv.prev[1].rayTL, sizeof(pp.prevRayTL2));
        memcpy(pp.prevRayRight2, rv.prev[1].rayRight, sizeof(pp.prevRayRight2));
        memcpy(pp.prevRayDown2, rv.prev[1].rayDown, sizeof(pp.prevRayDown2));
        memcpy(pp.prevFwd2, rv.prev[1].fwd, sizeof(pp.prevFwd2));
    } else {
        memset(pp.prevFwd, 0, sizeof(pp.prevFwd));
        memset(pp.prevFwd2, 0, sizeof(pp.prevFwd2));
        rv.historyFrames = 0;
    }
    // THE VIEW'S AGE, in frames of unbroken history (rq_reflect.comp's
    // `stereo.y`; 0 on a first frame, a resize, a scene bind and a change of
    // stereo shape, exactly where the previous basis is withheld above).
    pp.stereo[1] = float(rv.historyFrames);
    // knobs2.x: THE REST (PHOTON-2 1e), set below once every parameter is known; a lamp's step, a cloud or a
    // re-injection is the temporal pass' clamp's (the radiance-key cap is deleted with its key, PHOTON-2 1b).
    pp.knobs2[0] = 0.0f;
    // THE PHOTON VIEW'S Hits (PHOTON-VIEW-1): the view's overlay, written by this
    // trace when the scene's view asks and the view holds one at the full size.
    Ogre::TextureGpu *photon =
        scene->photonView() == PhotonView::Hits ? view->photonOverlay() : nullptr;
    if (photon && (photon->getWidth() != fullW || photon->getHeight() != fullH ||
                   photon->getNumSlices() != eyeLayers))
        photon = nullptr;
    pp.stereo[2] = photon ? 1.0f : 0.0f;
    // ---- THE HIT'S GEOMETRIC NORMAL: the per-slot row table (a copy per frame in flight) of the hit set's rows (the
    // GPU scene's, flushed by hitSetOf).
    detail::GpuScene &gpuScn = scene->gpuScene();
    uint32_t geomSlots = hs.geomRows ? uint32_t(sa.geomRowOfSlot.size()) : 0u;
    if (geomSlots) {
        RawBuffer &rb = rv.geomRowOfSlot[ring];
        const VkDeviceSize want = VkDeviceSize(geomSlots) * sizeof(uint32_t);
        if (rb.buffer && rb.size < want) retire(rb);
        if (!rb.buffer &&
            !makeBuffer(std::max<VkDeviceSize>(want, 1024u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                        false, rb, err))
            geomSlots = 0u;
        else
            std::memcpy(rb.mapped, sa.geomRowOfSlot.data(), size_t(want));
    }
    // THE HIT SET'S UNIFORMS (the one function: the cascades, the sky, the cards' counts, the geometric normal's rows,
    // the hit list and its sun ray, the tables, the hit's own surface)
    hitSetParams(hs, geomSlots, pp.hit, HitReader::View);
    // THE ARM THAT RE-MEASURES THE CLAIM (the shape of "reflect.monoEyes" beside it):
    // the arm "reflect.motion" = 0 switches EVERY branch of REFLECT-MOVERS-1 off at once —
    // the id image withheld (a surface's own motion), motion.w = 0 (a hit's motion,
    // the reflected image, the mover restarts, the filter's restart band) and the
    // march's velocity job (recordMotion) — so the paired cost arm of
    // gi.reflect_mover measures all of it, latched per frame so ONE process can hold
    // both arms (trap 12). Off is the pre-lane picture up to the mover age's six
    // low bits in the history's mean length (a 2^-17 relative change of a length).
    const bool motionOn = arm(ArmId::ReflectMotion, true);
    if (!motionOn) idTex = idDepthTex = nullptr;
    pp.motion[0] = idTex ? 1.0f : 0.0f;
    pp.motion[1] = idTex ? float(idTex->getWidth()) : 1.0f;
    pp.motion[2] = idTex ? float(idTex->getHeight()) : 1.0f;
    // ...AND WHETHER A MOVER MOVED IN THE LAST TWO FRAMES (motion.w = 2): the trace
    // restarts a mean of a mover's reflection whose rays stopped finding it only
    // then — at rest (every mover parked) a glossy lobe's tail that misses twice is
    // noise, not a departure, and no still pixel may move.
    {
        bool moverMoved = false;
        const uint32_t vaoFrame = mRs->getVaoManager()->getFrameCount();
        if (gpuScn.live() && gpuScn.movedThisFrame(vaoFrame)) {
            const detail::GpuInstance *mirror = gpuScn.mirrorData();
            for (uint32_t slot : gpuScn.movedSlots()) {
                if (slot >= gpuScn.slotCount()) continue;
                uint32_t f;
                std::memcpy(&f, &mirror[slot].boundsMax[3], sizeof(f));
                if (f & (detail::kGpuMover | detail::kGpuDragMover)) { moverMoved = true; break; }
            }
        }
        // ...or a POSE moved (REFLECT-MOVERS-2): a character's limbs under a still node.
        const bool poseMoved = sa.skinPosedFrame == vaoFrame;
        if (poseMoved) moverMoved = true;
        rv.framesSinceMoverMoved = moverMoved ? 0u : std::min(rv.framesSinceMoverMoved + 1u, 1000u);
        rv.framesSincePoseMoved = poseMoved ? 0u : std::min(rv.framesSincePoseMoved + 1u, 1000u);
    }
    // THE POSED IDENTIFICATION'S ARM ("reflect.posed", latched per frame so one
    // process holds both arms of gi.reflect_mover --cost-posed): off, the trace and the
    // march's velocity take the id image alone, the pre-lane motion for a character.
    const bool posedOn = arm(ArmId::ReflectPosed, true);
    pp.motion[3] = !motionOn ? 0.0f
                   : (posedOn && rv.framesSincePoseMoved <= 2u) ? 3.0f
                   : rv.framesSinceMoverMoved <= 2u ? 2.0f
                                                    : 1.0f;
    pp.alpha[2] = pointSnapKnob();   // the ray origin's depth-rebuilt point term (READ_B F5 (b))
    // THE HIT's AREA DRAW's CYCLE (PHOTON-7 11-2 (2)): with an area light in the hit table a rest texel's stop waits for one
    // full cycle of its time-stratified strata (jah_rq_hit_direct.glsl kHitAreaStrata^2); none, no wait
    pp.alpha[3] = sa.hitAreaLights > 0u ? float(kHitAreaStrataPerHit) : 0.0f;
    // THE PICK (the arm reflect.pick = 1 + x + 4096 y, a full-resolution pixel; 0 off): x | y << 12 | the ring's slot
    // << 24 | 1 << 30, in alpha.y's bits (rq_reflect.comp / rq_reflect_temporal.comp jahPickSlot)
    {
        const double pv = mArms ? mArms->value(ArmId::ReflectPick) : 0.0;
        uint32_t packed = 0u;
        if (pv >= 1.0) {
            const uint32_t v = uint32_t(pv - 1.0);
            packed = (v % 4096u) | ((v / 4096u) % 4096u) << 12u | (frameNow() % kReflectPickSlots) << 24u | (1u << 30u);
            rv.pickFrames[frameNow() % kReflectPickSlots] = frameNow();
            // (the slot this frame writes is zeroed first - its last writer retired 64 frames ago: a texel that writes
            // nothing this frame - stopped, or not the trace's this frame - reads empty, never a 64-frame-old sample)
            if (rv.pick.mapped)
                std::memset(static_cast<char *>(rv.pick.mapped) + size_t(frameNow() % kReflectPickSlots) * kReflectPickWords * 4u,
                            0, kReflectPickWords * 4u);
        }
        std::memcpy(&pp.alpha[1], &packed, sizeof(packed));
    }
    Ogre::TextureGpu *fogAerial = nullptr;             // PHOTON-I-1 fix 5: the media along the reflection
    scene->fogAlong(pp.fog, fogAerial);
    // ---- THE REST (PHOTON-2 1e, fix F1; round 2: R1, G1-G6) -----------------------------------------------------
    const unsigned restN = reflectRestLength(reflectWindow);
    pp.stereo[3] = float(restN);
    rv.restN = restN;
    // THE MOVED INSTANCES (G6): every slot the frame's update staged (a transform write, a flags change, a birth) and
    // every pose the skin pass moved, as a SWEPT BOX - its world bounds this frame joined with last frame's (the same
    // box carried by prevWorld * world^-1). A resting texel whose ray segments cross one resumes; more than
    // kReflectRestMovers break the view's rest whole.
    unsigned movedBoxes = 0u;
    bool tooManyMoved = false;
    {
        const uint32_t vaoFrame = mRs->getVaoManager()->getFrameCount();
        const auto addBox = [&](const detail::GpuInstance &g) {
            if (movedBoxes >= kReflectRestMovers) { tooManyMoved = true; return; }
            float lo[3], hi[3];
            sweptBoundsOf(g, lo, hi);
            for (int c = 0; c < 3; ++c) {
                pp.restMoverBox[2 * movedBoxes][c] = lo[c];
                pp.restMoverBox[2 * movedBoxes + 1][c] = hi[c];
            }
            ++movedBoxes;
        };
        if (gpuScn.live() && gpuScn.movedThisFrame(vaoFrame)) {
            const detail::GpuInstance *mirror = gpuScn.mirrorData();
            for (uint32_t slot : gpuScn.movedSlots())
                if (slot < gpuScn.slotCount()) addBox(mirror[slot]);
        }
        if (gpuScn.live() && sa.skinPosedFrame == vaoFrame) {
            const detail::GpuInstance *mirror = gpuScn.mirrorData();
            for (uint32_t slot : sa.skinPosedSlots)
                if (slot < gpuScn.slotCount()) addBox(mirror[slot]);
        }
        pp.restMovers[0] = float(tooManyMoved ? 0u : movedBoxes);
    }
    // The eye basis BIT for bit (a tracked head is never at rest: stereo never rests, as the gather's), the
    // reflection's rest key (reflectRestKey: never a move), and every parameter of the pass but the ones a still frame
    // changes by construction (the frame index, the previous basis, the view's age, the rest frame itself, the moved
    // instances' boxes). A Hits photon view keeps tracing (it is the trace's picture).
    HitView *restHits = nullptr;
    {
        auto hit = mHits.find(key);
        if (hit != mHits.end()) restHits = &hit->second;
    }
    bool hold = false, restBreak = false;
    {
        // THE NAMED KEY (commit 8, PREFLIGHT_R section 3: E12's whole-struct hash deleted - it took every member a
        // later lane added, a per-frame one included, and broke the rest on it): what a ray's answer depends on, member
        // by member - the projection and the targets, the gate and the reach, the cascades, the sky, the card and hit
        // tables, the sun legs, the id pass's image, the alpha table, the origin's terms, the fog, the metal route, the
        // clouds, the rest's own arms, the screen trace's and the denoiser's. NOT the eye (`sameEye` compares its basis
        // bit for bit), nor what a still frame changes by construction: the frame index (knobs.z), the rest frame
        // (knobs2.x), the view's age (stereo.y), the previous basis, the moved instances' boxes, the recent-move flag
        // (motion.w: a move, which breaks per texel) and the list's capacity (hitList.x: it grows in place).
        unsigned long long paramsKey = 1469598103934665603ull;
        const auto foldBytes = [&paramsKey](const void *d, size_t n) {
            const unsigned char *b = static_cast<const unsigned char *>(d);
            for (size_t i = 0; i < n; ++i) {
                paramsKey ^= b[i];
                paramsKey *= 1099511628211ull;
            }
        };
        const auto fold4 = [&](const float (&v)[4]) { foldBytes(v, sizeof(v)); };
        const auto foldF = [&](float v) { foldBytes(&v, sizeof(v)); };
        fold4(pp.projParams);
        fold4(pp.resolution);
        foldF(pp.knobs[0]); foldF(pp.knobs[1]); foldF(pp.knobs[3]);
        foldF(pp.knobs2[1]); foldF(pp.knobs2[3]);
        fold4(pp.viewAxisX); fold4(pp.viewAxisY); fold4(pp.viewAxisZ);
        foldF(pp.stereo[0]); foldF(pp.stereo[2]); foldF(pp.stereo[3]);
        {
            // THE HIT SET'S UNIFORMS, all but the list's capacity (as before: the list grows under a rest)
            HitSetParams h = pp.hit;
            h.hitList[0] = 0.0f;
            foldBytes(&h, sizeof(h));
        }
        foldF(pp.motion[0]); foldF(pp.motion[1]); foldF(pp.motion[2]);
        foldF(pp.alpha[0]); foldF(pp.alpha[2]); foldF(pp.alpha[3]);   // (NOT alpha.y: the pick, an instrument - its slot
                                                                    // moves every frame and it changes no picture)
        foldBytes(pp.fog, sizeof(pp.fog));
        // (the rest block's freeze and reference arms; NOT its ray count, rest.z - the rest's own budget, which moves
        // inside a rest by design - nor rest.w, unused)
        foldF(pp.rest[0]); foldF(pp.rest[1]);
        fold4(pp.screen); fold4(pp.screen2);
        fold4(pp.denoiser);
        const unsigned long long restKey = reflectRestKey(scene, sa);
        const bool sameEye = rv.havePrev && rv.prevStereo == stereo &&
                             std::memcmp(&rv.prev[0], &eyeB[0], sizeof(EyeBasisF)) == 0;
        const bool still = !stereo && !photon && sameEye && rv.restKnown && restKey == rv.restKey &&
                           paramsKey == rv.restParams && rv.historyFrames >= 1u && !tooManyMoved;
        rv.restableLast = !stereo && !photon;
        rv.recordedFrame = frameNow();
        rv.recordedDraw = rv.scene ? rv.scene->drawSerial() : 0u;
        const bool wasResting = rv.restFrames > 0u;
        rv.restFrames = still ? std::min(rv.restFrames + 1u, (1u << 20) - 1u) : 0u;
        rv.restKey = restKey;
        rv.restParams = paramsKey;
        rv.restKnown = true;
        // A BREAK (G2): the frame that leaves a rest composes every texel's answer before its trace (knobs2.x = -1).
        restBreak = wasResting && rv.restFrames == 0u;
        if (rv.restFrames == 1u) {
            ++rv.restSession;
            rv.restMoverFrame = 0u;
            rv.restAllStopped = false;
        }
        if (rv.restFrames > 0u && movedBoxes > 0u) {
            rv.restMoverFrame = rv.restFrames;   // the texels it crossed resume: every count before it is stale
            rv.restAllStopped = false;
            ++rv.restMoverCrossings;
        }
        // THE ACTIVE COUNT READ BACK (R1): a rest frame of THIS rest, after the last moved instance, that counted no
        // texel still converging - every texel has stopped and the view holds.
        if (rv.restFrames > 0u && restHits && restHits->restActiveTag != 0u &&
            (restHits->restActiveTag >> 20u) == (rv.restSession & 0xFFFu) &&
            (restHits->restActiveTag & 0xFFFFFu) > rv.restMoverFrame && restHits->restActive == 0u)
            rv.restAllStopped = true;
        // THE REST'S TWO PATHS (kReflectRestRaysCap's note): the TILE pass traces every batching texel at k a frame, the
        // rest's first frame's work on fewer texels; the COMPACT pass (kReflectCompactTexels) takes the tail, its rays a
        // frame the rest's count / kReflectRestTailDivisor - the count fixed from this rest's first read-back that
        // counted batching texels.
        {
            const unsigned kBase = std::max(1u, giQualityFacts(scene->giParams().quality).reflectRestRays);
            if (rv.restFrames <= 1u) rv.restRayCount = 0ull;
            const unsigned share = kBase;
            if (rv.restFrames > 0u && restHits && restHits->restActiveTag != 0u &&
                (restHits->restActiveTag >> 20u) == (rv.restSession & 0xFFFu) && restHits->restBatching > 0u &&
                rv.restRayCount == 0ull)
                rv.restRayCount = (unsigned long long)kBase * restHits->restBatching;
            pp.rest[2] = float(share);
            // THE RECORD ALLOWANCE (the record runs, rq_reflect.comp): the list's capacity shared by the texels batching
            // - min( k_active, capacity / batching ) a texel - none until the first read-back counted them.
            unsigned allowance = 0u;
            if (rv.restRayCount != 0ull && restHits && restHits->restBatching > 0u)
                allowance = std::min(share, unsigned(restHits->capacity / restHits->restBatching));
            pp.rest[3] = float(allowance);
        }
        // THE HOLD: every texel has stopped (its standard error under a code - a capped texel stops too, counted as the
        // defect it is: RayQueryStatus::reflectRestCapped). No frame count holds a rest (K_max 512 deleted, commit 8).
        // (under the reference arm no texel stops: the view holds after its 4,096 samples)
        // ...AND THE CACHE OWES THE READERS NOTHING (PHOTON-7 stage 10, (1) (ii): readersHoldOwed): the stop's one decision
        // taken, its work done, no relight's report in flight - the rest runs across a sub-quantum relight, so a hold
        // must wait until the late reading says whether it moved a code (the gather's own rule)
        hold = rv.restFrames > 0u && movedBoxes == 0u &&
               (restReference ? rv.restFrames > 4096u : rv.restAllStopped) && !scene->photon().readersHoldOwed();
        pp.knobs2[0] = restBreak ? -1.0f : float(rv.restFrames);   // (< 2^20: exact)
        // THE REST'S SEQUENCE BASE (the shader adds each sample's number v; kReflectRestSequence).
        // (the reference arm's value 2 takes a disjoint rest sequence: two references measure their own noise)
        if (rv.restFrames > 0u)
            pp.knobs[2] = float(kReflectRestSequence + (armValue(ArmId::ReflectRestReference) > 1.5 ? 0x8000u : 0u));
        // The trace counts this frame's active resting texels into the hit list (read back late, tagged).
        if (restHits)
            restHits->restTag = rv.restFrames > 0u && !hold && hit.on
                                    ? ((rv.restSession & 0xFFFu) << 20u) | (rv.restFrames & 0xFFFFFu)
                                    : 0u;
    }
    if (rv.historyFrames < 4096u) ++rv.historyFrames;   // saturates: "warm" is all it says
    // THE REST'S COMPACT PATH (kReflectCompactTexels): at rest in place, a mono view, a traced frame, no reference arm -
    // the tile pass lists this frame's converging texels in the half it flips to, and the compact pass takes them when
    // last frame's list (the other half) fit.
    const bool compactArmed = !hold && eyeLayers == 1u && rv.restFrames >= 2u && !restReference;
    if (!hold) rv.compactHalf ^= 1u;
    pp.compact[0] = float(rv.compactHalf);
    pp.compact[1] = float(rv.restRayCount / kReflectRestTailDivisor);
    pp.compact[2] = float(kReflectCompactTexels);
    pp.compact[3] = compactArmed ? 1.0f : 0.0f;
    // THE DEMAND SPAN (PHOTON-7 stage 10, the per-reader snapshot): the reflection stamps the cards it reads at its rest
    // indices [1, N + the due] alone (mDemandSpan, cardReadersRestComplete's) - moving (rest 0) it always stamps
    pp.hit.cards[2] = rv.restFrames == 0u || mDemandSpan == 0u || rv.restFrames <= mDemandSpan ? 1.0f : 0.0f;
    memcpy(rv.params[ring].mapped, &pp, sizeof(pp));
    rv.prev[0] = eyeB[0];
    rv.prev[1] = eyeB[1];
    rv.havePrev = true;
    rv.prevStereo = stereo;

    // ---- THE DESCRIPTOR SET, rewritten every frame --------------------------
    // IMAGE VIEWS, UNCACHED AND RETIRED — and the cache is not an optimisation
    // we passed up, it is a TRAP measured here. `createView(slot, bUseCache =
    // true)` hands back a REFCOUNTED view out of VulkanTextureGpuManager keyed
    // by the slot's contents, and the key contains the TextureGpu POINTER, not
    // the VkImage. A compositor texture that is recreated at the same address —
    // which is every texture in the chain on every window resize — therefore
    // answers with the view of the DESTROYED image, and the descriptor set we
    // write points at memory that is gone. Measured exactly that way: frames 1
    // and 2 bound a view of frame 0's `jahSsrReflection`, the validation layer
    // named the dead image, the command buffer was invalidated by the
    // destruction of frame 0's depth texture, `vkEndCommandBuffer` failed and
    // the frame died with VK_ERROR_DEVICE_LOST. (A cached view must also be
    // handed back with `destroyView`, which is a second thing to get wrong.)
    //
    // So: a fresh view per frame, retired into the same bin as everything else
    // here, destroyed once the frames that could still be reading it have.
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    // THE PING-PONG: a traced frame reads [flip & 1] and writes the other; a HELD frame writes nothing and binds the
    // last written half as "cur" (the filter composites it), which the next traced frame then reads as "prev". A REST
    // frame from k = 2 (fix F1) binds as a held one — the frozen moving history in "prev", the handed-over answer in
    // place in "cur" — and traces.
    const unsigned last = rv.flip & 1u;
    const bool inPlace = hold || rv.restFrames >= 2u;
    const unsigned prev = inPlace ? 1u - last : last, cur = inPlace ? last : 1u - last;

    VkDescriptorBufferInfo ub{};
    ub.buffer = rv.params[ring].buffer;
    ub.range = sizeof(ReflectParams);
    VkDescriptorImageInfo sampled[3] = {}, storage[5] = {};
    Ogre::TextureGpu *const sampledSrc[3] = { normalTex, roughTex, depthTex };
    for (int i = 0; i < 3; ++i) {
        sampled[i].sampler = mPointSampler;
        sampled[i].imageView = layerView(sampledSrc[i], false);   // every eye's layer
        sampled[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    // The depth texture's view carries the DEPTH aspect, but its LAYOUT under
    // `ResourceLayout::Texture` is the plain shader-read one for every format
    // (OgreVulkanMappings.cpp:603) — which is the layout the transition below
    // asks the solver for, so the descriptor and the barrier agree.
    storage[0].imageView = layerView(ssrTex, true);
    storage[1].imageView = rv.hist[prev].view;
    storage[2].imageView = rv.hist[cur].view;
    storage[3].imageView = rv.state[prev].view;
    storage[4].imageView = rv.state[cur].view;
    for (int i = 0; i < 5; ++i) storage[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet w[kReflectBindings] = {};
    for (unsigned i = 0; i < kReflectBindings; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = rv.sets[ring];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    // THE HIT SET (the one writer, HitSetWrite at kReflectHitSlots): the TLAS, the volumes, the sky, the card read
    // and its view term, the geometric normal, the record, the hit's own surface - every slot a real view (a null one
    // in a set the shader may index is undefined behaviour, not a black sample: the tier's stand-ins).
    VkBuffer rowOfSlot = geomSlots ? rv.geomRowOfSlot[ring].buffer : VK_NULL_HANDLE;
    HitSetWrite hitSet;
    {
        std::string herr;
        if (!hitSet.write(*this, rv.sets[ring], kReflectHitSlots, hs, rowOfSlot, w, herr)) {
            bail("a hit-set binding could not be made (HitSetWrite)");   // (a literal: the log says each reason once)
            return;
        }
    }
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &ub;
    for (int i = 0; i < 3; ++i) {
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2 + i].pImageInfo = &sampled[i];
    }
    for (int i = 0; i < 5; ++i) {
        w[5 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[5 + i].pImageInfo = &storage[i];
    }
    // THE PHOTON VIEW'S OVERLAY (36): the view's, or — the knob off — the reflection
    // target's own storage view as a stand-in of the same format, never written.
    VkDescriptorImageInfo photonImg{};
    photonImg.imageView = photon ? layerView(photon, true) : storage[0].imageView;
    photonImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!photonImg.imageView) { bail("the photon overlay view is null"); return; }
    w[kReflectPhotonBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[kReflectPhotonBinding].pImageInfo = &photonImg;
    // THE ID IMAGE (37, REFLECT-MOVERS-1): the chain's, or the "nothing drew here"
    // stand-in with `motion.x` 0.
    VkDescriptorImageInfo idImg{};
    idImg.sampler = mPointSampler;
    idImg.imageView = idTex ? sampledView(idTex) : mDummyIds.view;
    idImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!idImg.imageView) { bail("the id image view is null"); return; }
    w[kReflectIdBinding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kReflectIdBinding].pImageInfo = &idImg;
    // THE AIR'S TABLE (39): the passes', or the volume stand-in while they read none.
    VkDescriptorImageInfo fogImg{};
    fogImg.sampler = mLinearSampler;
    fogImg.imageView = fogAerial ? sampledView(fogAerial) : mDummyVolume.view;
    fogImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!fogImg.imageView) { bail("the air's table view is null"); return; }
    w[kReflectFogBinding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kReflectFogBinding].pImageInfo = &fogImg;
    // THE ID PASS'S DEPTH (40): the chain's copy, or the flat stand-in with `motion.x` 0.
    VkDescriptorImageInfo idDepthImg{};
    idDepthImg.sampler = mPointSampler;
    idDepthImg.imageView = idDepthTex ? sampledView(idDepthTex) : mDummyFlat.view;
    idDepthImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!idDepthImg.imageView) { bail("the id depth view is null"); return; }
    w[kReflectIdDepthBinding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kReflectIdDepthBinding].pImageInfo = &idDepthImg;
    // THIS FRAME'S RAW SAMPLE (45, PHOTON-2 1b).
    VkDescriptorImageInfo sampleImg{};
    sampleImg.imageView = rv.sample.view;
    sampleImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!sampleImg.imageView) { bail("the sample view is null"); return; }
    w[kReflectSampleBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[kReflectSampleBinding].pImageInfo = &sampleImg;
    // ...AND ITS NEE PART (55, PHOTON-7 11-1b).
    VkDescriptorImageInfo neeImg{};
    neeImg.imageView = rv.nee.view;
    neeImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!neeImg.imageView) { bail("the NEE part's view is null"); return; }
    w[kReflectNeeBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[kReflectNeeBinding].pImageInfo = &neeImg;
    // THE REST'S REFERENCE (46, R1 (f)).
    VkDescriptorImageInfo referenceImg{};
    referenceImg.imageView = rv.reference.view;
    referenceImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!referenceImg.imageView) { bail("the reference view is null"); return; }
    w[kReflectReferenceBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[kReflectReferenceBinding].pImageInfo = &referenceImg;
    // THE SCREEN TRACE'S PICTURE (47-49, PHOTON-7 stage 6): the chain's three, point-sampled (the trace reads them by
    // texel), or the flat stand-in with `screen.x` 0.
    VkDescriptorImageInfo screenImgs[3] = {};
    {
        Ogre::TextureGpu *const src[3] = { prevColourTex, prevDepthTex, velocityTex };
        for (int i = 0; i < 3; ++i) {
            screenImgs[i].sampler = mPointSampler;
            screenImgs[i].imageView = screenTrace ? sampledView(src[i]) : mDummyFlat.view;
            screenImgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!screenImgs[i].imageView) { bail("a screen-trace binding is null"); return; }
            w[kReflectScreenBinding + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[kReflectScreenBinding + i].pImageInfo = &screenImgs[i];
        }
    }
    // THE EXPOSURE (50, PHOTON-7 stage 6 commit 6): the chain's measured history, or the flat stand-in with
    // `denoiser.y` 0.
    VkDescriptorImageInfo exposureImg{};
    exposureImg.sampler = mPointSampler;
    exposureImg.imageView = exposureTex ? sampledView(exposureTex) : mDummyFlat.view;
    exposureImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!exposureImg.imageView) { bail("the exposure binding is null"); return; }
    w[kReflectExposureBinding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[kReflectExposureBinding].pImageInfo = &exposureImg;
    // THE REST'S STOP SHARE (51): the filter writes it, the temporal pass reads it.
    VkDescriptorImageInfo shareImg{};
    shareImg.imageView = rv.share.view;
    shareImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!shareImg.imageView) { bail("the rest's stop-share binding is null"); return; }
    w[kReflectShareBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[kReflectShareBinding].pImageInfo = &shareImg;
    // THE REST'S COMPACT LIST (52)
    if (!rv.compactList.buffer) {
        std::string lerr;
        if (!makeBuffer(VkDeviceSize(2u + 2u * kReflectCompactTexels) * 4u,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, false,
                        rv.compactList, lerr)) { bail("the rest's compact list could not be made"); return; }
        rv.compactListFresh = true;
    }
    VkDescriptorBufferInfo compactInfo{};
    compactInfo.buffer = rv.compactList.buffer;
    compactInfo.range = VK_WHOLE_SIZE;
    w[kReflectCompactBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kReflectCompactBinding].pBufferInfo = &compactInfo;
    // THE MOVING SE INSTRUMENT (53): a float a texel while the readback's arm is on, else the 16-byte stand-in
    {
        const VkDeviceSize need = arm(ArmId::ReflectHistoryReadback, false)
                                      ? VkDeviceSize(traceW) * traceH * rv.layers * 4u : VkDeviceSize(16u);
        if (rv.movingSe.buffer && rv.movingSe.size != need) retire(rv.movingSe);
        if (!rv.movingSe.buffer) {
            std::string serr;
            // (host-visible, made zero: a texel the temporal pass never stores - declined, or at rest since the arm
            // came on - reads 0, never a stale allocation)
            if (!makeBuffer(need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false,
                            rv.movingSe, serr)) { bail("the moving SE instrument could not be made"); return; }
            if (rv.movingSe.mapped) std::memset(rv.movingSe.mapped, 0, size_t(need));
        }
    }
    VkDescriptorBufferInfo movingSeInfo{};
    movingSeInfo.buffer = rv.movingSe.buffer;
    movingSeInfo.range = VK_WHOLE_SIZE;
    w[kReflectMovingSeBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kReflectMovingSeBinding].pBufferInfo = &movingSeInfo;
    // THE PICK (56): the ring, host-visible and zeroed at its making (written only while the arm names a texel)
    if (!rv.pick.buffer) {
        std::string perr;
        const VkDeviceSize bytes = VkDeviceSize(kReflectPickSlots) * kReflectPickWords * 4u;
        if (!makeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, rv.pick, perr)) {
            bail("the pick could not be made");
            return;
        }
        std::memset(rv.pick.mapped, 0, size_t(bytes));
    }
    VkDescriptorBufferInfo pickInfo{};
    pickInfo.buffer = rv.pick.buffer;
    pickInfo.range = VK_WHOLE_SIZE;
    w[kReflectPickBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[kReflectPickBinding].pBufferInfo = &pickInfo;
    vkUpdateDescriptorSets(mVk, kReflectBindings, w, 0, nullptr);

    // ---- THE LAYOUTS, THROUGH OGRE'S OWN SOLVER -----------------------------
    // The DOCUMENTED route, and it is the documented route again after a
    // detour (second reader, M1). The first pass here emitted the barriers by
    // hand and wrote `VulkanTextureGpu::mCurrLayout` from outside, on the
    // reading that `resolveTransition` had produced nothing; that reading was
    // taken while the command buffer was ALREADY invalid (a cached image view
    // of a destroyed texture — the real defect, fixed elsewhere in this file),
    // so it was a symptom and not a cause. Writing another module's layout
    // bookkeeping from outside is exactly the class of workaround §325 is
    // about, and it is gone.
    //
    // The array is a LOCAL, not `getNewResourceTransitionsArrayTmp()`: that
    // accessor hands back the solver's one shared scratch and its own header
    // says not to hold it in two places at once.
    //
    // IT RUNS BEFORE THE COMMAND BUFFER IS TAKEN, which is not style:
    // `executeResourceTransition` closes every encoder
    // (OgreVulkanRenderSystem.cpp:3483) and Ogre's queue may roll over to a new
    // command buffer while doing it, so a handle fetched earlier can be an
    // already-ended one.
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, ssrTex, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::ReadWrite, computeStage);
        if (photon)
            solver.resolveTransition(trans, photon, Ogre::ResourceLayout::Uav,
                                     Ogre::ResourceAccess::Write, computeStage);
        for (Ogre::TextureGpu *t : { normalTex, roughTex, depthTex })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        if (idTex)
            solver.resolveTransition(trans, idTex, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        if (idDepthTex)
            solver.resolveTransition(trans, idDepthTex, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        // THE SCREEN TRACE'S PICTURE: the copies the last frame wrote, the motion this frame's job wrote (a UAV) ahead.
        if (screenTrace)
            for (Ogre::TextureGpu *t : { prevColourTex, prevDepthTex, velocityTex })
                solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                         Ogre::ResourceAccess::Read, computeStage);
        if (exposureTex)
            solver.resolveTransition(trans, exposureTex, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        // THE HIT SET (the one order: hitSetTransitions)
        hitSetTransitions(solver, trans, hs);
        if (fogAerial)
            solver.resolveTransition(trans, fogAerial, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }

    // ---- THE DISPATCH -------------------------------------------------------
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) { bail("frameCmd"); return; }   // device lost: record nothing
    clearDummyImages(cmd);
    clearReflectImages(rv, cmd);
    // THE BREAK'S COMPOSE (G2): every texel's answer at rest - lerp( H, M, min( v / N, 1 ) ) - into the half the trace
    // reads, at its own texel, before the trace reprojects it; the moving rules resume from the picture on screen.
    if (restBreak && mComposePipeline) {
        monitor::CacheScope composeRow(CacheKind::Gi, WorkReason::Camera, 0, "rq.reflect.compose", mRs, key);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mComposePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1, &rv.sets[ring], 0,
                                nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, eyeLayers);
        VkMemoryBarrier composed{};
        composed.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        composed.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        composed.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &composed, 0, nullptr, 0, nullptr);
    }
    if (!hold) {
        // THE COMPACT LIST'S COUNT for this frame's half, zeroed (both, the first frame of a new list)
        {
            if (rv.compactListFresh) {
                vkCmdFillBuffer(cmd, rv.compactList.buffer, 0, 8u, 0u);
                rv.compactListFresh = false;
            } else {
                vkCmdFillBuffer(cmd, rv.compactList.buffer, VkDeviceSize(rv.compactHalf) * 4u, 4u, 0u);
            }
            VkMemoryBarrier fb{};
            fb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fb,
                                 0, nullptr, 0, nullptr);
        }
        monitor::CacheScope traceRow(CacheKind::Gi, WorkReason::Camera, 0, "rq.reflect.trace", mRs, key);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1,
                                &rv.sets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, eyeLayers);   // z = the eye
        // THE COMPACT PASS (kReflectCompactTexels): one warp a listed texel, after the tile pass listed them
        if (compactArmed && mReflectCompactPipeline) {
            VkMemoryBarrier lb{};
            lb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            lb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            lb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &lb, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectCompactPipeline);
            const unsigned perGroup = std::max(1u, 64u / mSubgroupSize);   // the 8 x 8 group's warps: a texel each
            vkCmdDispatch(cmd, (kReflectCompactTexels + perGroup - 1u) / perGroup, 1u, 1u);
        }
        // The overlay holds this frame's picture from here; the photon listener takes it
        // to the texture layout in front of its composite.
        if (photon) view->notePhotonOverlayWritten();
    }
    // ---- THE FIRST HALF ENDS HERE (PHOTON-HIT-SHADE-1): the hits no cache
    // shades are in the hit list; the decode pass shades them and the write-back
    // completes their texels' means before finishReflect filters.
    rv.finishPending = true;
    rv.finishRing = ring;
    rv.traceW = traceW;
    rv.traceH = traceH;
    rv.fullW = fullW;
    rv.fullH = fullH;
    rv.curIdx = cur;
    rv.prevIdx = prev;
    rv.held = hold;
    rv.rays = hold ? 0u : traceW * traceH;
    if (!hold) ++rv.frame;
    if (!inPlace) ++rv.flip;
}

// ---- THE SCREEN MARCH'S OBJECT MOTION (REFLECT-MOVERS-1) ----------------------
namespace {
/// rq_motion.comp's set 0, one type a binding — and rq_motion_skin.comp's, which
/// reads 0-5 and the ray's three after them. The rigid job never reads 6-8, so a
/// frame with no pose moving leaves them unwritten (a binding a pipeline does not
/// statically use need not hold a valid descriptor).
constexpr unsigned kMotionBindings = 9u;
constexpr unsigned kMotionRigidBindings = 6u;
constexpr VkDescriptorType kMotionTypes[kMotionBindings] = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 1 depth
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 the id image
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 3 the GPU scene's instances
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 jahSsrVelocity
    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 5 the id pass's own depth (ID-DEPTH-1)
    VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 6 the scene's TLAS (posed job)
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 7 the per-slot geometry rows
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 8 the GPU scene's geometry rows
};
/// rq_motion.comp's Params, member for member.
struct MotionParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float resolution[4] = {};
    /// rq_motion_skin.comp's: x = 1 when it runs, y = the per-slot row entries.
    float skin[4] = {};
};
/// Sets in the pool: a ring a view, for as many views as the reflection pool serves.
constexpr unsigned kMotionRing = 3u;
}   // namespace

bool RayQueryTier::makeMotionPipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[kMotionBindings] = {};
    for (unsigned i = 0; i < kMotionBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = kMotionTypes[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kMotionBindings, kMaxTimedScenes * kMotionRing, mMotionSetLayout, err, "rayquery/motion")) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mMotionSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mMotionPipeLayout) != VK_SUCCESS) {
        err = "rayquery/motion: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_motionSpv);
    smi.pCode = krq_motionSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mMotionModule) != VK_SUCCESS) {
        err = "rayquery/motion: vkCreateShaderModule failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mMotionModule;
    cpi.stage.pName = "main";
    cpi.layout = mMotionPipeLayout;
    if (createComputePipeline(cpi, &mMotionPipeline, "motion") != VK_SUCCESS) {
        err = "rayquery/motion: vkCreateComputePipelines failed";
        return false;
    }
    // THE POSED TWIN, same layout. A failure is logged and leaves the rigid job whole.
    {
        VkShaderModuleCreateInfo ssi{};
        ssi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ssi.codeSize = sizeof(krq_motionSkinSpv);
        ssi.pCode = krq_motionSkinSpv;
        VkComputePipelineCreateInfo spi = cpi;
        if (vkCreateShaderModule(mVk, &ssi, nullptr, &mMotionSkinModule) == VK_SUCCESS) {
            spi.stage.module = mMotionSkinModule;
            if (createComputePipeline(spi, &mMotionSkinPipeline, "motion skin") != VK_SUCCESS)
                mMotionSkinPipeline = VK_NULL_HANDLE;
        }
        if (!mMotionSkinPipeline)
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: the march's posed object motion off — its pipeline could not be made");
    }
    // The pool its layout planned (VkDescriptorPools.h): a ring a view.
    if (!plan.create(mVk, "rayquery/motion", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mMotionPool, err))
        return false;
    return ensureSamplers(err);
}

// ---------------------------------------------------------------------------
// A CUT-OUT IS GEOMETRY WITH HOLES (REFLECT-MOVERS-2; audit C-16 answered).
//
// Every bottom-level structure stays VK_GEOMETRY_OPAQUE_BIT_KHR — a mesh's
// structure is shared by every item that wears it, whatever the material — and
// an ALPHA-TESTED item's instance is FORCE_NO_OPAQUE (rq_tlas_write.comp), so its
// triangles reach every ray query as candidates. The queries test a candidate
// through the scene's ALPHA TABLE (jah_rq_alpha.glsl: the layout) — the TLAS's half,
// the rows each copy was built from — and the GPU SCENE'S MASK TABLE it names: each
// submesh's MASK (its albedo texture's alpha through the datablock's own alpha test,
// one bit a texel) and UV transform. The masks are the engine's since
// ENGINE-POLISH-ATOM-1 stage 3 (AlphaMasks.h: one maker for the rays, the id pass
// and the caster cut; the tier made its own before).
void RayQueryTier::updateAlphaTable(OgreScene *scene, SceneAs &sa) {
    sa.st.alphaMasksMade = alphaMaskStore().made();
    sa.st.alphaStaleServed = scene->maskStaleServed();
    // THE TABLE IS RE-WRITTEN ONLY WHEN ITS INPUTS MOVED (ENGINE-UPGRADE-U5 stage 3): the alpha-tested
    // traced slots or their rows (the feed), the row buffer's address, or the GPU scene's mask table (re-made
    // whenever a mask, a material or a texture's texels moved: its serial).
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::VaoManager *vaoMgr = mRs ? mRs->getVaoManager() : nullptr;
    // THE ROWS BY ADDRESS (the fable read's D1): a candidate reads its OWN submesh's
    // row, base + geometry index, from the GPU scene's row buffer — which is
    // re-created when the mesh table grows, so its address rides in the words and a
    // grow re-makes the table.
    const uint64_t rowsAddress =
        (gs.live() && gs.geomBuffer() && vaoMgr) ? vaoMgr->getBufferDeviceAddress(gs.geomBuffer()) : 0u;
    const uint64_t maskAddress = gs.maskTableAddress();
    if (!sa.feed.alphaDirty && gs.maskTableSerial() == sa.alphaMaskTableSerial && rowsAddress == sa.alphaRowsAddress)
        return;
    sa.feed.alphaDirty = false;
    sa.alphaMaskTableSerial = gs.maskTableSerial();
    sa.alphaRowsAddress = rowsAddress;
    std::vector<uint32_t> &w = sa.alphaScratch;
    w.clear();
    const std::set<uint32_t> &slots = sa.feed.alpha;
    if (!slots.empty() && gs.live() && rowsAddress && maskAddress) {
        const uint32_t n = *slots.rbegin() + 1u;
        w.assign(8u + ((n + 3u) & ~3u), kMaskNone);
        w[0] = n;
        w[1] = 0u;
        w[2] = uint32_t(rowsAddress & 0xFFFFFFFFu);
        w[3] = uint32_t(rowsAddress >> 32u);
        w[4] = uint32_t(maskAddress & 0xFFFFFFFFu);
        w[5] = uint32_t(maskAddress >> 32u);
        w[6] = w[7] = 0u;
        for (const uint32_t slot : slots) {
            ++sa.st.updateVisits;
            if (slot >= gs.slotCount()) continue;
            // THE ROW BASES: submesh 0's row of the level each copy was built from (the
            // feed's near row; the mesh's coarsest level for the far copy; a rigged
            // slot's skin row for both — unknown until its first ready frame, and a
            // candidate with no row is refused).
            const uint32_t nearRow = slot < sa.geomRowOfSlot.size() ? sa.geomRowOfSlot[slot]
                                                                     : detail::GpuScene::kNoGeomRow;
            uint32_t farRow = nearRow;
            if (slot < sa.feed.recs.size() && sa.feed.recs[slot].kind == 1u)
                farRow = detail::GpuScene::geomRowIndex(sa.feed.recs[slot].meshIndex, sa.feed.recs[slot].farLevel, 0u);
            w[8u + slot] = uint32_t(w.size());
            ++w[1];
            ++sa.st.updateRows;
            const uint32_t head[4] = { nearRow, farRow, 0u, 0u };
            w.insert(w.end(), head, head + 4);
        }
    }
    if (w == sa.alphaWords) return;
    retire(sa.alphaTable);
    sa.alphaAddress = 0;
    if (!w.empty()) {
        std::string err;
        if (makeBuffer(VkDeviceSize(w.size()) * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true, sa.alphaTable, err)) {
            std::memcpy(sa.alphaTable.mapped, w.data(), w.size() * 4u);
            sa.alphaAddress = addressOf(sa.alphaTable.buffer);
        } else {
            Ogre::LogManager::getSingleton().logMessage("rayquery: the alpha table: " + err);
            w.clear();
        }
    }
    sa.alphaWords.swap(w);
}

/// THE HIT TABLE (FAR-SUN-2; the layout is jah_rq_hit_direct.glsl's): u[0] = the lights,
/// u[1] = the material rows, u[2] = the first light's word, u[3] = the first row's, u[4] = the first
/// word of the metal's lobe integrals (METAL-BOUNCE-1: PhotonSpecularAlbedo::kTable), u[5] / u[6] the emitter
/// table's first word and targets (PHOTON-7 11-1a, updateEmitterTable); a light is
/// kHitLightWords words (eight vec4, PhotonVoxelLighting::addLight's packing in WORLD space), a
/// row PhotonVoxelMaterial::kRowWords. THE LIGHTS are the scene's one light list
/// (SceneLightList), EVERY visible light with the injection's mask and NO budget (the lead's
/// merge fix of FAR-SUN-2's read, C1): the injection selects per cascade box up to
/// PhotonVoxelLighting::kLightSlots lights, and a hit may lie in any cascade, so the table
/// holds the union - every light the store's injection could have used - and the store's direct
/// part (which the hit's read takes out) is put back by the same lights at the hit. A
/// scene-wide top-N by raw power (the first version, 32) lost a dim lamp beside the hit that a
/// cascade had injected. The shader windows each light before its shadow ray, so the cost of
/// a light out of range is one window test. THE BUFFER IS A FRAME RING written in place (the
/// tlasIn pattern): a sun the day cycle moves re-writes a slot, never re-makes the buffer.
namespace {
constexpr unsigned kHitLightWords = 32u;
/// The header's words: u[0..4] above, u[5] = the first word of the EMITTER TABLE, u[6] = its targets (PHOTON-7 11-1a).
constexpr uint32_t kHitHeaderWords = 7u;
/// AN EMITTER TARGET (PHOTON-7 stage 11, 11-1a; jah_rq_emitters.glsl reads it): the world bounding sphere's centre
/// and radius (vec4), the row's emissive radiance (xyz) and the instance's SLOT (w, its bits).
constexpr uint32_t kEmitterWords = 8u;
/// A PORTAL's slot word (PHOTON-7 11-2 (6); jah_rq_emitters.glsl kJahPortalSlot): a target that is an opening to the sky
constexpr uint32_t kPortalSlot = 0xFFFFFFFFu;
/// the most portals a scene's table holds (the largest openings by power first)
constexpr size_t kMaxPortals = 16u;
/// A LAMP's IMAGE's slot word (PHOTON-7 11-2 (8); jah_rq_emitters.glsl kJahImageSlot): words 4 its weight, 5 the mirror's
/// instance slot, 6 the lamp's index in the light table (uint bits)
constexpr uint32_t kImageSlot = 0xFFFFFFFEu;
/// ...the lamp's widening's constant (JahBrdf's kJahSphereLobeK: the same function the read widens the lobe by)
constexpr double kSphereLobeK = 0.777;
}   // namespace

/// THE EMITTER TABLE (PHOTON-7 stage 11, 11-1a): the instances the reflection's next-event estimation aims at. An
/// emitter is a TRACED instance (kGpuRayTraced: what a ray can hit) whose material row (GpuInstance::ids.y, the
/// voxeliser's row - the same E a ray hit adds, unclamped fp32) emits: any channel of its emissive above zero. A light
/// is a light at any radiance: the floor is zero, and the tier's CAP (GiQualityFacts::reflectEmitters, a count)
/// keeps the brightest by POWER - the radiance's largest channel x the bounding sphere's cross-section pi r^2, the
/// measure the cone strategy samples by - the rest counted (reflectEmittersOver), never silent. A target is the world
/// bounding sphere of the instance's world box (the cone the NEE ray is drawn in: any emitter shape, the sample's pdf
/// exact from the table alone) and the row's radiance (the pick's weight; the E a ray takes is the hit's own row and map).
/// ORDERED BY SLOT (never by a container's or a pointer's order: a run repeats). The SET's serial (slots and radiance -
/// never the bounds: a moving emitter is a mover, which the rest follows per texel) is published; it is NOT in the rest
/// key, whose material edits, rows and slot count already carry every change of the set (reflectRestKey says why).
/// Re-derived only when its inputs moved (the GPU scene's writes, its slot count and mesh set, the rows, the edits,
/// the cap); the hit table re-writes only when the section's words changed.
/// THE SKY's PORTALS (PHOTON-7 11-2 (6), the lead's ruling - the RIM form): an opening to the sky is bounded by the surfaces
/// around it, and the voxel sides' TLAS gather measured each surface voxel's ESCAPE SHARE (rq_voxel_sides.comp: the share
/// of its cosine rays that left the world). A RIM PAIR is two surface voxels facing each other across empty cells along
/// one axis (each one's front normal dominant on that axis, the signs opposed), both of an intermediate share (they see
/// the sky in part: 0 < s <= kRimHigh - an interior wall's sides read 0, an exterior face ~1); the empty cells between
/// them are the OPENING's cells. Connected opening cells are one opening; its cells' principal axes give its quad, whose
/// bounding sphere is the table's target (the 8-word emitter form, slot kPortalSlot), its weight the sky's largest
/// mean radiance over the quad's two normals (the SH in force). The set is an OPTIMISATION'S: the reflection's C2 draws
/// toward it by NEE and weighs the BRDF's sky escapes by the balance heuristic against it, unbiased for ANY set - a soft
/// or empty set (a courtyard, a porch, no rim) costs variance only, never bias. Re-built when the shares were written
/// again and have landed (a few frames after the last cascade-0 pass).
namespace {
PortalScanOut scanPortals(const PortalScanIn &in) {
    const auto scanStart = std::chrono::steady_clock::now();
    PortalScanOut out;
    const int W = in.W, H = in.H, D = in.D;
    const uint32_t *words = in.words.data();
    const auto at = [&](int x, int y, int z) -> uint32_t {
        return (x < 0 || y < 0 || z < 0 || x >= W || y >= H || z >= D) ? 0u : words[(size_t(z) * H + y) * W + x];
    };
    constexpr float kRimHigh = 0.85f;
    constexpr int kMaxGap = 64;
    const auto shareOf = [](uint32_t w) { return float(w & 255u) / 255.0f; };
    const auto normalOf = [](uint32_t w) {
        const float ex = float((w >> 18u) & 127u) / 127.0f * 2.0f - 1.0f, ey = float((w >> 25u) & 127u) / 127.0f * 2.0f - 1.0f;
        float n[3] = { ex, ey, 1.0f - std::fabs(ex) - std::fabs(ey) };
        if (n[2] < 0.0f) {
            const float ox = (1.0f - std::fabs(ey)) * (ex >= 0.0f ? 1.0f : -1.0f), oy = (1.0f - std::fabs(ex)) * (ey >= 0.0f ? 1.0f : -1.0f);
            n[0] = ox; n[1] = oy;
        }
        return std::array<float, 3>{ n[0], n[1], n[2] };
    };
    const auto rim = [&](uint32_t w) {
        const float f = shareOf(w);
        return (w & (1u << 16u)) && f > 0.0f && f <= kRimHigh;
    };
    // (the candidates first: one tight pass over the words)
    std::vector<uint32_t> candidates;
    const size_t total = size_t(W) * H * D;
    for (size_t i = 0; i < total; ++i)
        if (rim(words[i])) candidates.push_back(uint32_t(i));
    std::vector<uint32_t> openList;
    std::unordered_set<uint32_t> open;
    for (const uint32_t i0 : candidates) {
        const int x = int(i0 % uint32_t(W)), y = int((i0 / uint32_t(W)) % uint32_t(H)), z = int(i0 / uint32_t(W * H));
        const uint32_t w = words[i0];
        {
            const auto n = normalOf(w);
            int a = 0;
            for (int k = 1; k < 3; ++k) if (std::fabs(n[k]) > std::fabs(n[a])) a = k;
            const int sg = n[a] > 0.0f ? 1 : -1;
            int p[3] = { x, y, z };
            int steps = 0;
            bool paired = false;
            for (; steps < kMaxGap; ++steps) {
                p[a] += sg;
                const uint32_t q = at(p[0], p[1], p[2]);
                if (!(q & (1u << 16u))) continue;   // an empty cell (or a solid's inside: no surface)
                if (steps > 0 && rim(q)) {
                    const auto m = normalOf(q);
                    int b = 0;
                    for (int k = 1; k < 3; ++k) if (std::fabs(m[k]) > std::fabs(m[b])) b = k;
                    paired = b == a && (m[a] > 0.0f ? 1 : -1) == -sg;
                }
                break;
            }
            if (!paired) continue;
            int c[3] = { x, y, z };
            for (int k = 0; k < steps; ++k) {
                c[a] += sg;
                const uint32_t j = uint32_t((size_t(c[2]) * H + c[1]) * W + c[0]);
                if (open.insert(j).second) openList.push_back(j);
            }
        }
    }
    // THE OPENINGS: the connected opening cells (6-neighbours), each one's principal axes
    struct Portal { float c[3]{}; float r = 0.f; float n[3]{}; double power = 0.0; float L[3]{}; };
    std::vector<Portal> portals;
    const float *sh = in.sh;
    const bool haveSh = in.haveSh;
    std::vector<int> stack;
    std::unordered_set<uint32_t> seen;
    for (const uint32_t i0 : openList) {
        if (!seen.insert(i0).second) continue;
        double sum[3] = {}, cov[3][3] = {};
        size_t count = 0;
        stack.assign(1, int(i0));
        std::vector<std::array<float, 3>> cells;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % W, y = (i / W) % H, z = i / (W * H);
            const std::array<float, 3> pw = { in.origin[0] + (x + 0.5f) * in.cell[0],
                                              in.origin[1] + (y + 0.5f) * in.cell[1],
                                              in.origin[2] + (z + 0.5f) * in.cell[2] };
            cells.push_back(pw);
            for (int k = 0; k < 3; ++k) sum[k] += pw[k];
            ++count;
            const int nb[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
            for (const auto &d : nb) {
                const int X = x + d[0], Y = y + d[1], Z = z + d[2];
                if (X < 0 || Y < 0 || Z < 0 || X >= W || Y >= H || Z >= D) continue;
                const uint32_t j = uint32_t((size_t(Z) * H + Y) * W + X);
                if (open.count(j) && seen.insert(j).second) stack.push_back(int(j));
            }
        }
        if (count < 4u) continue;   // a stray pair, no opening
        const double mean[3] = { sum[0] / count, sum[1] / count, sum[2] / count };
        for (const auto &pw : cells)
            for (int r = 0; r < 3; ++r)
                for (int c2 = 0; c2 < 3; ++c2) cov[r][c2] += (pw[r] - mean[r]) * (pw[c2] - mean[c2]);
        // the principal axes: Jacobi rotations of the 3x3 covariance
        double A[3][3], V[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
        for (int r = 0; r < 3; ++r) for (int c2 = 0; c2 < 3; ++c2) A[r][c2] = cov[r][c2];
        for (int sweep = 0; sweep < 16; ++sweep)
            for (int pI = 0; pI < 2; ++pI)
                for (int qI = pI + 1; qI < 3; ++qI) {
                    if (std::fabs(A[pI][qI]) < 1e-12) continue;
                    const double th = 0.5 * std::atan2(2.0 * A[pI][qI], A[qI][qI] - A[pI][pI]);
                    const double cs = std::cos(th), sn = std::sin(th);
                    for (int k = 0; k < 3; ++k) {
                        const double akp = A[k][pI], akq = A[k][qI];
                        A[k][pI] = cs * akp - sn * akq;
                        A[k][qI] = sn * akp + cs * akq;
                    }
                    for (int k = 0; k < 3; ++k) {
                        const double apk = A[pI][k], aqk = A[qI][k];
                        A[pI][k] = cs * apk - sn * aqk;
                        A[qI][k] = sn * apk + cs * aqk;
                    }
                    for (int k = 0; k < 3; ++k) {
                        const double vkp = V[k][pI], vkq = V[k][qI];
                        V[k][pI] = cs * vkp - sn * vkq;
                        V[k][qI] = sn * vkp + cs * vkq;
                    }
                }
        double half[3] = {};
        for (const auto &pw : cells)
            for (int e = 0; e < 3; ++e) {
                double d = 0.0;
                for (int k = 0; k < 3; ++k) d += (pw[k] - mean[k]) * V[k][e];
                half[e] = std::max(half[e], std::fabs(d));
            }
        const double cellMax = std::max({ in.cell[0], in.cell[1], in.cell[2] });
        int thin = 0;
        for (int e = 1; e < 3; ++e) if (A[e][e] < A[thin][thin]) thin = e;
        Portal pt;
        double r2 = 0.0;
        for (int e = 0; e < 3; ++e) r2 += (half[e] + 0.5 * cellMax) * (half[e] + 0.5 * cellMax);
        for (int k = 0; k < 3; ++k) { pt.c[k] = float(mean[k]); pt.n[k] = float(V[k][thin]); }
        pt.r = float(std::sqrt(r2));
        // the weight: the sky's largest mean radiance over the quad's two normals (the SH's basis 1, y, z, x, ...)
        for (int ch = 0; ch < 3; ++ch) {
            float best = haveSh ? sh[ch] : 0.0f;
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                const float x = sgn * pt.n[0], y = sgn * pt.n[1], z = sgn * pt.n[2];
                const float b[9] = { 1.0f, y, z, x, x * y, y * z, 3.0f * z * z - 1.0f, z * x, x * x - y * y };
                float e = 0.0f;
                for (int k = 0; k < 9; ++k) e += b[k] * sh[k * 3 + ch];
                if (haveSh) best = std::max(best, e);
            }
            pt.L[ch] = std::max(best, 0.0f);
        }
        const float lmax = std::max({ pt.L[0], pt.L[1], pt.L[2] });
        if (!(lmax > 0.0f) || !std::isfinite(pt.r) || !(pt.r > 0.0f)) continue;
        pt.power = double(lmax) * 3.14159265358979 * double(pt.r) * double(pt.r);
        portals.push_back(pt);
    }
    std::stable_sort(portals.begin(), portals.end(), [](const Portal &a, const Portal &b) { return a.power > b.power; });
    if (portals.size() > kMaxPortals) portals.resize(kMaxPortals);
    auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, sizeof u); return u; };
    for (const Portal &pt : portals) {
        const uint32_t e[kEmitterWords] = { bits(pt.c[0]), bits(pt.c[1]), bits(pt.c[2]), bits(pt.r),
                                            bits(pt.L[0]), bits(pt.L[1]), bits(pt.L[2]), kPortalSlot };
        out.words.insert(out.words.end(), e, e + kEmitterWords);
        RayQueryStatus::ReflectEmitter pub;
        pub.slot = kPortalSlot;
        for (int c2 = 0; c2 < 3; ++c2) { pub.centre[c2] = pt.c[c2]; pub.radiance[c2] = pt.L[c2]; }
        pub.radius = pt.r;
        out.table.push_back(pub);
    }
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scanStart).count();
    return out;
}
}   // namespace

void RayQueryTier::updatePortals(OgreScene *scene, SceneAs &sa) {
    // (1) A SCAN THAT FINISHED: its answer becomes the table's portals
    if (sa.portalJob.valid() && sa.portalJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        PortalScanOut r = sa.portalJob.get();
        sa.st.reflectPortals = int(r.table.size());
        sa.st.reflectPortalTable = r.table;
        ++sa.portalScans;
        sa.portalScanMs = r.ms;
        Ogre::LogManager::getSingleton().logMessage("Jahshaka portals: scan " + std::to_string(sa.portalScans) + ", " +
                                                    std::to_string(r.table.size()) + " portal(s), " +
                                                    std::to_string(r.ms) + " ms off the render thread (frame " +
                                                    std::to_string(frameNow()) + ")");
        if (r.words != sa.portalWords) {
            sa.portalWords.swap(r.words);
            ++sa.portalSerial;
        }
    }
    // (2) SHARES REWRITTEN AND LANDED, no scan running: a copy goes to a new one
    auto it = mVoxelSides.find(scene);
    if (it == mVoxelSides.end() || sa.portalJob.valid()) return;
    const VoxelSideView &v = it->second;
    if (!v.shares.mapped || !v.sharesDims[0] || v.sharesWrites == sa.portalBuiltWrites) return;
    if (frameNow() - v.sharesFrame < kFramesInFlight + 2u) return;   // the last pass's words have not landed yet
    sa.portalBuiltWrites = v.sharesWrites;
    auto in = std::make_shared<PortalScanIn>();
    in->W = int(v.sharesDims[0]);
    in->H = int(v.sharesDims[1]);
    in->D = int(v.sharesDims[2]);
    in->words.resize(size_t(in->W) * in->H * in->D);
    std::memcpy(in->words.data(), v.shares.mapped, in->words.size() * sizeof(uint32_t));
    for (int k = 0; k < 3; ++k) { in->origin[k] = v.sharesOrigin[k]; in->cell[k] = v.sharesCell[k]; }
    in->haveSh = scene->skyAmbientSh(in->sh);
    sa.portalJob = std::async(std::launch::async, [in]() { return scanPortals(*in); });
}

void RayQueryTier::updateEmitterTable(OgreScene *scene, SceneAs &sa) {
    const detail::GpuScene &gs = scene->gpuScene();
    const Ogre::PhotonVoxelMaterial *store = scene->photon().materials();
    const unsigned long long writes = gs.writes();
    const unsigned long long rowsSerial = store ? store->getRowsSerial() : 0ull;
    const unsigned long long edits = scene->photon().materialEditSerial();
    const unsigned long long meshes = gs.meshSetSerial();
    const uint32_t slotCount = gs.live() ? gs.slotCount() : 0u;
    const uint32_t cap = giQualityFacts(scene->giParams().quality).reflectEmitters;
    if (writes == sa.emitterWrites && rowsSerial == sa.emitterRows && edits == sa.emitterEdits &&
        meshes == sa.emitterMeshes && slotCount == sa.emitterSlotCount && cap == sa.emitterCap &&
        sa.portalSerial == sa.emitterPortalSerial)
        return;
    sa.emitterPortalSerial = sa.portalSerial;
    sa.emitterWrites = writes;
    sa.emitterRows = rowsSerial;
    sa.emitterEdits = edits;
    sa.emitterMeshes = meshes;
    sa.emitterSlotCount = slotCount;
    sa.emitterCap = cap;

    struct Target { uint32_t slot = 0u; float c[3] = {}; float r = 0.0f; float L[3] = {}; double power = 0.0; };
    std::vector<Target> all;
    const detail::GpuInstance *m = gs.mirrorData();
    if (m && store) {
        const auto &rows = store->getRowWords();
        const uint32_t nRows = uint32_t(rows.size() / Ogre::PhotonVoxelMaterial::kRowWords);
        for (uint32_t i = 0; i < slotCount; ++i) {
            uint32_t f;
            std::memcpy(&f, &m[i].boundsMax[3], sizeof(f));
            if (!(f & detail::kGpuRayTraced)) continue;
            const uint32_t word = m[i].ids[1];
            if (word == detail::GpuScene::kNoMaterialWord) continue;
            const uint32_t row = (word >> 16u) * Ogre::PhotonVoxelMaterial::kRowsPerBucket + (word & 0xFFFFu);
            if (row >= nRows) continue;
            Target t;
            const uint32_t *rw = &rows[size_t(row) * Ogre::PhotonVoxelMaterial::kRowWords];
            bool emits = false, finite = true;
            for (int k = 0; k < 3; ++k) {
                std::memcpy(&t.L[k], &rw[8 + k], sizeof(float));
                finite = finite && std::isfinite(t.L[k]);
                emits = emits || t.L[k] > 0.0f;
            }
            if (!emits || !finite) continue;
            double d2 = 0.0;
            for (int k = 0; k < 3; ++k) {
                const double lo = m[i].boundsMin[k], hi = m[i].boundsMax[k];
                t.c[k] = float(0.5 * (lo + hi));
                d2 += (hi - lo) * (hi - lo);
            }
            t.r = float(0.5 * std::sqrt(d2));
            if (!std::isfinite(t.r) || !(t.r > 0.0f) || !std::isfinite(t.c[0] + t.c[1] + t.c[2])) continue;
            t.slot = i;
            t.power = double(std::max({ t.L[0], t.L[1], t.L[2] })) * 3.14159265358979 * double(t.r) * double(t.r);
            all.push_back(t);
        }
    }
    // THE CAP: the brightest by power (ties by slot: deterministic), then back into slot order.
    if (all.size() > cap) {
        std::stable_sort(all.begin(), all.end(), [](const Target &a, const Target &b) { return a.power > b.power; });
        sa.st.reflectEmittersOver = unsigned(all.size() - cap);
        all.resize(cap);
        std::sort(all.begin(), all.end(), [](const Target &a, const Target &b) { return a.slot < b.slot; });
    } else {
        sa.st.reflectEmittersOver = 0u;
    }
    auto bits = [](float v) { uint32_t u; std::memcpy(&u, &v, sizeof u); return u; };
    std::vector<uint32_t> words, setKey;
    words.reserve(all.size() * kEmitterWords);
    setKey.reserve(all.size() * 4u);
    sa.st.reflectEmitterTable.clear();
    for (const Target &t : all) {
        const uint32_t e[kEmitterWords] = { bits(t.c[0]), bits(t.c[1]), bits(t.c[2]), bits(t.r),
                                            bits(t.L[0]), bits(t.L[1]), bits(t.L[2]), t.slot };
        words.insert(words.end(), e, e + kEmitterWords);
        const uint32_t k[4] = { t.slot, bits(t.L[0]), bits(t.L[1]), bits(t.L[2]) };
        setKey.insert(setKey.end(), k, k + 4);
        RayQueryStatus::ReflectEmitter pub;
        pub.slot = t.slot;
        for (int c = 0; c < 3; ++c) { pub.centre[c] = t.c[c]; pub.radiance[c] = t.L[c]; }
        pub.radius = t.r;
        sa.st.reflectEmitterTable.push_back(pub);
    }
    sa.st.reflectEmitters = int(all.size());
    // ...AND THE SKY's PORTALS after them (PHOTON-7 11-2 (6), updatePortals: slot kPortalSlot; the reflection's emitter
    // NEE skips them, C2's sky NEE takes them alone - jah_rq_emitters.glsl's kind)
    words.insert(words.end(), sa.portalWords.begin(), sa.portalWords.end());
    setKey.insert(setKey.end(), sa.portalWords.begin(), sa.portalWords.end());
    if (setKey != sa.emitterSetKey) {
        sa.emitterSetKey.swap(setKey);
        ++sa.st.reflectEmitterSetSerial;
    }
    if (words != sa.emitterWords) {
        sa.emitterWords.swap(words);
        sa.emitterDirty = true;
    }
}

void RayQueryTier::updateHitTable(OgreScene *scene, SceneAs &sa) {
    // RE-WRITTEN ONLY WHEN ITS INPUTS MOVED (ENGINE-UPGRADE-U5 stage 3; it was re-derived every frame):
    // the lights (the scene's light-write serial: a light set, added, removed, or its node moved -
    // every lamp move names itself there, OgreGi's nameMovedSubtrees), the material store's rows (their
    // serial) and the material edits; a published table stays otherwise - a mover that is no light
    // re-writes nothing.
    {
        const Ogre::PhotonVoxelMaterial *storeGate = scene->photon().materials();
        const unsigned long long lights = scene->lightWriteSerial();
        // THE PAIR (decision 14.1): removeLight and releaseNode move only the removal count, never the
        // write serial - a deleted lamp went on lighting hits while the gate read the serial alone.
        const unsigned long long removed = scene->lightsRemoved();
        // The rows' serial is process-wide (PhotonVoxelMaterial::nextRowsSerial): a store the chain's
        // teardown re-created never repeats an older store's value (decision 14.7).
        const unsigned long long rows = storeGate ? storeGate->getRowsSerial() : 0ull;
        const unsigned long long edits = scene->photon().materialEditSerial();
        if (sa.hitTableAddress && lights == sa.hitLightSerial && removed == sa.hitLightsRemoved &&
            rows == sa.hitRowsSerial && edits == sa.hitMaterialSerial && !sa.emitterDirty) {
            if (sa.hitSlot >= 0) sa.hitSlotUntil[sa.hitSlot] = frameNow();
            return;
        }
        sa.hitLightSerial = lights;
        sa.hitLightsRemoved = removed;
        sa.hitRowsSerial = rows;
        sa.hitMaterialSerial = edits;
        sa.emitterDirty = false;
    }
    std::vector<uint32_t> &w = sa.hitScratch;
    w.assign(kHitHeaderWords, 0u);
    const SurfaceCache *cardCache = scene->photon().surfaceCache();
    const Ogre::Light *cardSun = cardCache ? cardCache->readersSun() : nullptr;
    auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, sizeof u); return u; };
    Ogre::SceneManager *sm = scene->sceneManager();
    uint32_t nLights = 0u;
    struct Lamp { uint32_t index = 0u; double p[3] = {}; double radius = 0.0, range = 0.0, power = 0.0; };
    std::vector<Lamp> lamps;   // (8): the point and spot lights, by their index in the table
    if (sm) {
        SceneLightList::Consumer consumer;
        consumer.budget = 0u;             // unbounded: the union of every cascade's selection
        SceneLightList::Selection sel;
        sceneLightListOf(sm).select(sm, consumer, sel);
        sa.hitAreaLights = 0u;
        // (L9 (b1): an area light's place in the pass's forward LTC set - the decode's light i - word 19)
        const std::vector<const Ogre::Light *> ltcSet = forwardLtcLights(sm);
        for (Ogre::Light *light : sel.lights) {
            if (!light || !light->getParentNode()) continue;
            if (light->getType() == Ogre::Light::LT_AREA_LTC) ++sa.hitAreaLights;
            float L[kHitLightWords] = {};
            const Ogre::Light::LightTypes srcType = light->getType();
            const bool punctual = srcType == Ogre::Light::LT_POINT || srcType == Ogre::Light::LT_SPOTLIGHT;
            const bool area = srcType == Ogre::Light::LT_AREA_LTC;   // every area light is LTC (PHOTON-7 7-0)
            const Ogre::Light::LightTypes type = srcType;
            const Ogre::Vector4 p4 = light->getAs4DVector();   // directional: -dir, w 0
            L[0] = float(p4.x); L[1] = float(p4.y); L[2] = float(p4.z); L[3] = float(type);
            const Ogre::ColourValue d = light->getDiffuseColour() * light->getPowerScale();
            L[4] = d.r; L[5] = d.g; L[6] = d.b;
            L[7] = (punctual || area) ? float(light->getAttenuationRange()) : 0.0f;
            const Ogre::Quaternion q = light->getParentNode()->_getDerivedOrientation();
            const Ogre::Vector3 z = q.zAxis();
            if (type == Ogre::Light::LT_SPOTLIGHT) {
                L[8] = -float(z.x); L[9] = -float(z.y); L[10] = -float(z.z);
                const float inner = float(light->getSpotlightInnerAngle().valueRadians());
                const float outer = float(light->getSpotlightOuterAngle().valueRadians());
                L[12] = 1.0f / (std::cos(inner * 0.5f) - std::cos(outer * 0.5f));
                L[13] = std::cos(outer * 0.5f);
                L[14] = float(light->getSpotlightFalloff());
            }
            if (punctual) L[11] = float(light->getAttenuationLinear());   // the source radius
            // THE CARDS' TRACED SUN (PHOTON-3 stage 2): the one directional whose visibility a card stores
            // (SurfaceCache::cardSun, its SunVis layer) - the card read's lobe takes the texel's stored term for it.
            if (srcType == Ogre::Light::LT_DIRECTIONAL && cardSun && light == cardSun) L[11] = 1.0f;
            L[15] = light->getDoubleSided() ? 1.0f : 0.0f;
            if (area) {
                // the rectangle every area-light reader takes (lightextras::areaLightCorners, the pixel's and the
                // cards') and its emitting side (the light's direction: the strata law's `emit`, 7-H)
                const Ogre::Vector3 emit = light->getDerivedDirection();
                L[8] = float(emit.x); L[9] = float(emit.y); L[10] = float(emit.z);
                Ogre::Vector3 r[4];
                lightextras::areaLightCorners(light, r);
                for (int k = 0; k < 4; ++k) {
                    L[16 + 4 * k] = float(r[k].x); L[17 + 4 * k] = float(r[k].y); L[18 + 4 * k] = float(r[k].z);
                }
                // THE DECODE's LIGHT INDEX (PHOTON-7 11-2 L9 (b1)): its place in the pass's forward LTC set (HlmsPbs's
                // order, forwardLtcLights) - a record's area visibility is byte i of its word 10 - or -1 past the set
                L[19] = -1.0f;
                for (size_t k = 0; k < ltcSet.size() && k < 4u; ++k)
                    if (ltcSet[k] == light) L[19] = float(k);
            }
            if (punctual) {
                Lamp lp;
                lp.index = nLights;
                for (int k = 0; k < 3; ++k) lp.p[k] = L[k];
                lp.radius = L[11];
                lp.range = L[7];
                lp.power = std::max({ double(d.r), double(d.g), double(d.b) });
                lamps.push_back(lp);
            }
            for (float f : L) w.push_back(bits(f));
            // THE LAMP'S WORLD ID (PHOTON-7 stage 5, 5-3), word 31: a card's lamp table names its lamps by it
            // (SurfaceCache's lampIdOf, the same rule), so the card read's lobe takes each lamp's own traced bit.
            {
                const unsigned long long doc = SceneLightList::documentId(light);
                w.back() = doc ? uint32_t(doc & 0x7FFFFFFFull) : uint32_t(0x80000000u | (light->getId() & 0x7FFFFFFFu));
            }
            ++nLights;
            ++sa.st.updateRows;
        }
    }
    const Ogre::PhotonVoxelMaterial *store = scene->photon().materials();
    const uint32_t lightWords = uint32_t(w.size());
    uint32_t nRows = 0u;
    if (store) {
        const auto &rows = store->getRowWords();
        nRows = uint32_t(rows.size() / Ogre::PhotonVoxelMaterial::kRowWords);
        w.insert(w.end(), rows.begin(), rows.end());
        sa.st.updateRows += nRows;
    }
    // THE METAL'S LOBE INTEGRALS (METAL-BOUNCE-1; photon/voxel/PhotonSpecularAlbedo.h): the hit's metal is
    // shaded by the directional albedo the injection takes (jah_rq_hit_direct.glsl), from the same floats.
    const uint32_t specWords = uint32_t(w.size());
    for (float f : Ogre::PhotonSpecularAlbedo::kTable) w.push_back(bits(f));
    // THE EMITTER TABLE (PHOTON-7 stage 11, 11-1a): the reflection's NEE targets, as updateEmitterTable derived them.
    const uint32_t emitterWords = uint32_t(w.size());
    w.insert(w.end(), sa.emitterWords.begin(), sa.emitterWords.end());
    // ...AND THE LAMPS' IMAGES IN PLANAR NEAR-MIRRORS (PHOTON-7 11-2 (8), the lead's ruling): a (point or spot lamp, planar
    // specular instance) pair enters where the LAMP's widening dominates the material's roughness, alpha_m <= k asin( r_l /
    // d_l ) - there the lamp's angular size sets the lobe, the reflection is a glint only an image draw finds; above it the
    // roughness sets the lobe and the BRDF draw covers it (its worst sample falls as 1 / alpha_m^2). No constant but the
    // geometry's own; the MIS is unbiased for any set, so the gate is efficiency alone. Planar: the instance's bounds
    // thinner along one axis than a tenth of the other two; its plane the face toward the lamp. The image: the lamp
    // reflected through it, its sphere the cone of the widened lobe, alpha_w^2 = alpha_m^2 + ( k asin( r_l / d_l ) )^2,
    // the half-angle 2 atan( 3 alpha_w ) seen from the mirror. Each pair's decision printed once (alpha_m, the widening,
    // S_max at mu_v 0.1 and W_p 1: S_max = F I / ( 4 pi alpha_w^2 d_l^2 mu_v )).
    uint32_t images = 0u;
    {
        const detail::GpuScene &gs = scene->gpuScene();
        const detail::GpuInstance *mi = gs.mirrorData();
        const uint32_t slots = gs.live() ? gs.slotCount() : 0u;
        static std::set<unsigned long long> printed;
        if (mi && store && !lamps.empty()) {
            const auto &rows = store->getRowWords();
            const uint32_t nR = uint32_t(rows.size() / Ogre::PhotonVoxelMaterial::kRowWords);
            for (uint32_t i = 0; i < slots && images < 16u; ++i) {
                uint32_t f;
                std::memcpy(&f, &mi[i].boundsMax[3], sizeof(f));
                if (!(f & detail::kGpuRayTraced)) continue;
                const uint32_t word = mi[i].ids[1];
                if (word == detail::GpuScene::kNoMaterialWord) continue;
                const uint32_t row = (word >> 16u) * Ogre::PhotonVoxelMaterial::kRowsPerBucket + (word & 0xFFFFu);
                if (row >= nR) continue;
                const uint32_t *rw = &rows[size_t(row) * Ogre::PhotonVoxelMaterial::kRowWords];
                float specW, rough;
                std::memcpy(&specW, &rw[13], sizeof(float));
                std::memcpy(&rough, &rw[14], sizeof(float));
                if (!(specW > 0.0f) || !std::isfinite(rough)) continue;
                double ext[3], ctr[3];
                for (int k = 0; k < 3; ++k) {
                    ext[k] = double(mi[i].boundsMax[k]) - double(mi[i].boundsMin[k]);
                    ctr[k] = 0.5 * (double(mi[i].boundsMax[k]) + double(mi[i].boundsMin[k]));
                }
                const int ax = ext[0] <= ext[1] ? (ext[0] <= ext[2] ? 0 : 2) : (ext[1] <= ext[2] ? 1 : 2);
                const double other = std::min(ext[(ax + 1) % 3], ext[(ax + 2) % 3]);
                if (!(ext[ax] <= 0.1 * other) || !std::isfinite(other)) continue;
                const double alphaM = double(rough) * double(rough);
                for (const Lamp &lp : lamps) {
                    double dv[3], dl2 = 0.0;
                    for (int k = 0; k < 3; ++k) { dv[k] = lp.p[k] - ctr[k]; dl2 += dv[k] * dv[k]; }
                    const double dl = std::sqrt(dl2);
                    if (!(dl > lp.radius) || (lp.range > 0.0 && dl > lp.range)) continue;
                    const double widen = kSphereLobeK * std::asin(std::min(1.0, lp.radius / dl));
                    const bool enters = alphaM <= widen;
                    const double alphaW2 = alphaM * alphaM + widen * widen;
                    const double sMax = lp.power / (4.0 * 3.14159265358979 * std::max(alphaW2, 1e-12) * dl2 * 0.1);
                    const unsigned long long key = (unsigned long long)i << 32u | lp.index;
                    if (printed.insert(key).second)
                        std::fprintf(stderr, "rayquery: (8) image pair (instance slot %u, lamp %u): alpha_m %.6f, the lamp's "
                                     "widening %.6f, S_max %.4g at mu_v 0.1 -> %s\n", i, lp.index, alphaM, widen, sMax,
                                     enters ? "ENTERS" : "the BRDF draw's");
                    if (!enters) continue;
                    // the face toward the lamp, the lamp reflected through it
                    const double plane = dv[ax] >= 0.0 ? double(mi[i].boundsMax[ax]) : double(mi[i].boundsMin[ax]);
                    double img[3] = { lp.p[0], lp.p[1], lp.p[2] };
                    img[ax] = 2.0 * plane - lp.p[ax];
                    const double alphaW = std::sqrt(alphaW2);
                    const double theta = std::min(2.0 * std::atan(3.0 * alphaW), 1.2);
                    const float r = float(dl * std::tan(theta) + lp.radius);
                    const float weight = float(lp.power / std::max(dl2, 1e-4));
                    const uint32_t e[kEmitterWords] = { bits(float(img[0])), bits(float(img[1])), bits(float(img[2])), bits(r),
                                                        bits(weight), i, lp.index, kImageSlot };
                    w.insert(w.end(), e, e + kEmitterWords);
                    ++images;
                }
            }
        }
    }
    w[0] = nLights;
    w[1] = nRows;
    w[2] = kHitHeaderWords;
    w[3] = lightWords;
    w[4] = specWords;
    w[5] = emitterWords;
    w[6] = uint32_t(sa.emitterWords.size() / kEmitterWords) + images;
    const uint32_t nowFrame = frameNow();
    if (w == sa.hitWords && sa.hitTableAddress) {
        if (sa.hitSlot >= 0) sa.hitSlotUntil[sa.hitSlot] = nowFrame;
        return;
    }
    // A SLOT NO FRAME IN FLIGHT CAN READ (decision 14.7). frameNow() is Ogre's VaoManager frame count,
    // the counter its frame fence retires dynamic buffers on (+1 a submitted frame): a slot whose last
    // reader was frame u is free once nowFrame - u >= framesInFlight(). The slot `frameNow() % 3` this
    // used was safe only while the table was written EVERY frame - the stage-3 gate leaves it unwritten
    // for frames, and a write 3k frames on landed in the slot the frames still in flight read. Under one
    // write a frame the slots' last readers are this frame - 1 (the leaving one), - 2 and - 3: the third
    // is free (3 >= framesInFlight() 3), so the ring re-makes nothing (the lamp arm counts it).
    int pick = -1;
    for (unsigned r = 0; r < kFramesInFlight && pick < 0; ++r)
        if (int(r) != sa.hitSlot && (sa.hitSlotUntil[r] == 0u || nowFrame - sa.hitSlotUntil[r] >= framesInFlight()))
            pick = int(r);
    if (pick < 0) {
        // Two writes inside one frame's window: a fresh buffer (the old one retires behind the fence).
        pick = (sa.hitSlot + 1) % int(kFramesInFlight);
        retire(sa.hitTable[pick]);
        sa.hitTableCap[pick] = 0u;
    }
    const unsigned ring = unsigned(pick);
    RawBuffer &buf = sa.hitTable[ring];
    const uint32_t words = uint32_t(w.size());
    if (!buf.buffer || sa.hitTableCap[ring] < words) {
        retire(buf);
        uint32_t cap = std::max<uint32_t>(sa.hitTableCap[ring], 1024u);
        while (cap < words) cap *= 2u;
        std::string err;
        ++sa.st.hitTableBuffers;
        if (!makeBuffer(VkDeviceSize(cap) * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true, buf, err)) {
            Ogre::LogManager::getSingleton().logMessage("rayquery: the hit table: " + err);
            sa.hitTableAddress = 0;
            sa.hitTableCap[ring] = 0u;
            w.clear();
            sa.hitWords.swap(w);
            return;
        }
        sa.hitTableCap[ring] = cap;
    }
    std::memcpy(buf.mapped, w.data(), words * 4u);
    sa.hitTableAddress = addressOf(buf.buffer);
    sa.st.hitLights = nLights;
    sa.st.hitMaterialRows = nRows;
    // THE LEAVING SLOT's last reader is the PREVIOUS frame (this frame's traces read the new one: the table is
    // written before them) - stamped with it (decision 16.2: stamped with this frame, a slot was never old
    // enough again under one write a frame and every frame from the fourth re-made its buffer).
    if (sa.hitSlot >= 0) sa.hitSlotUntil[sa.hitSlot] = nowFrame ? nowFrame - 1u : 0u;
    sa.hitSlot = int(ring);
    sa.hitSlotUntil[ring] = nowFrame;
    sa.hitWords.swap(w);
}

uint64_t RayQueryTier::hitTableOf(const OgreScene *scene) const {
    // The arm "gi.hitDirect" = 0 (the measuring arm): no table, the hit reads the store whole.
    if (!arm(ArmId::GiHitDirect, true)) return 0u;
    const SceneAs *sa = asOf(scene);
    return sa ? uint64_t(sa->hitTableAddress) : 0u;
}

RayQueryTier::HitSurfaceInputs RayQueryTier::hitSurfaceInputsOf(const OgreScene *scene) const {
    HitSurfaceInputs h;
    if (scene->photon().materials()) h.pool = scene->photon().materials()->getTexturePool();
    const FogHlmsListener::CloudShadowState cs = FogHlmsListener::cloudShadow(scene->sceneManager());
    if (cs.field && cs.strength > 0.0f) {
        h.cloudField = cs.field;
        h.cloudMap[0] = cs.invTile; h.cloudMap[1] = cs.strength; h.cloudMap[2] = cs.scroll[0]; h.cloudMap[3] = cs.scroll[1];
        h.cloudSun[0] = cs.sunThrow[0]; h.cloudSun[1] = cs.sunThrow[1]; h.cloudSun[2] = cs.altitude; h.cloudSun[3] = cs.invMuSun;
    }
    return h;
}

void RayQueryTier::hitSetOf(OgreScene *scene, const SceneAs &sa, HitSetSources &out) {
    const HitListBinding hit = out.hit;
    out = HitSetSources{};
    out.hit = hit;
    out.tlas = sa.tlas;
    // THE WORLD's BOX: every item a ray can hit (its GPU-scene slot: the TLAS's instance set), its world AABB - the
    // sky is a pass, never an instance, and an instance's extent IS world a ray can hit. Once a frame (SceneAs::
    // worldBoxFrame), every job of the frame reading the same.
    const uint32_t boxFrame = mRs && mRs->getVaoManager() ? mRs->getVaoManager()->getFrameCount() : 0xFFFFFFFEu;
    if (sa.worldBoxFrame == boxFrame) {
        out.worldOn = sa.worldBoxOn;
        for (int k = 0; k < 3; ++k) { out.worldMin[k] = sa.worldBoxMin[k]; out.worldMax[k] = sa.worldBoxMax[k]; }
    } else
    for (const auto *np : scene->itemNodes()) {
        const Ogre::Item *item = np ? np->item : nullptr;
        if (!item || np->itemSlot == size_t(-1)) continue;
        const Ogre::Aabb wa = item->getWorldAabb();
        if (!std::isfinite(wa.mHalfSize.x) || !std::isfinite(wa.mHalfSize.y) || !std::isfinite(wa.mHalfSize.z) ||
            !std::isfinite(wa.mCenter.x) || !std::isfinite(wa.mCenter.y) || !std::isfinite(wa.mCenter.z))
            continue;
        const Ogre::Vector3 lo = wa.getMinimum(), hi = wa.getMaximum();
        for (int k = 0; k < 3; ++k) {
            out.worldMin[k] = out.worldOn ? std::min(out.worldMin[k], lo[k]) : lo[k];
            out.worldMax[k] = out.worldOn ? std::max(out.worldMax[k], hi[k]) : hi[k];
        }
        out.worldOn = true;
    }
    if (sa.worldBoxFrame != boxFrame) {
        sa.worldBoxFrame = boxFrame;
        sa.worldBoxOn = out.worldOn;
        for (int k = 0; k < 3; ++k) { sa.worldBoxMin[k] = out.worldMin[k]; sa.worldBoxMax[k] = out.worldMax[k]; }
    }
    // ---- THE VOXEL CACHE THE HITS ARE SHADED FROM: each cascade of the Photon chain is its OWN PhotonVoxelLighting
    // (chained with addCascade, OgreGi.cpp), innermost first - the order the shaders want: they take the first volume
    // that contains the hit, so the finest one that can answer does.
    std::vector<PhotonComponent::Volume> built;
    scene->photon().volumes(built);
    for (const PhotonComponent::Volume &v : built) {
        Ogre::PhotonVoxelLighting *lighting = v.lighting;
        Ogre::PhotonVoxelizer *voxelizer = v.voxelizer;
        if (out.cascadeCount >= kGatherMaxCascades || !lighting || !voxelizer) continue;
        Ogre::TextureGpu **tex = lighting->getLightVoxelTextures();
        if (!tex || !tex[0]) continue;
        const unsigned c = out.cascadeCount;
        // BY NAME (RQ-COV-SLOT-1): the total, the three directional volumes (every tier keeps them, PHOTON-MARCH-1),
        // the coverage and the position from PhotonVoxelLighting's own index, level 0's back side and the voxelizer's
        // normal; MOVER-OCCLUSION-1: the direct term's volume, or the light store (no share to gate) - its front and
        // its back.
        out.voxel[c][0] = tex[0];
        for (int i = 1; i < 4; ++i) out.voxel[c][i] = tex[i];
        for (unsigned h = 0; h < 2u; ++h) {
            out.voxel[c][4 + h] = tex[lighting->coverageIndex(h)];
            out.voxel[c][6 + h] = tex[lighting->positionIndex(h)];
        }
        out.voxel[c][8] = tex[lighting->backIndex()];
        out.voxel[c][9] = tex[lighting->normalIndex()];
        out.voxel[c][10] = lighting->getLightDirectTexture() ? lighting->getLightDirectTexture() : tex[0];
        out.voxel[c][11] = lighting->getLightDirectBackTexture() ? lighting->getLightDirectBackTexture()
                                                                  : tex[lighting->backIndex()];
        const Ogre::Vector3 og = voxelizer->getVoxelOrigin();
        const Ogre::Vector3 sz = voxelizer->getVoxelSize();
        const Ogre::Vector3 cl = voxelizer->getVoxelCellSize();
        out.voxelOrigin[c][0] = og.x; out.voxelOrigin[c][1] = og.y; out.voxelOrigin[c][2] = og.z;
        out.voxelSize[c][0] = sz.x;   out.voxelSize[c][1] = sz.y;   out.voxelSize[c][2] = sz.z;
        out.voxelCell[c] = std::max(std::max(cl.x, cl.y), cl.z);
        // THE LEVEL BY THE CAMERA (PHOTON-7 stage 10, 10-2e; the lead's ruling): cascade c answers a hit only within its
        // GUARANTEED RADIUS (half - step - cell: its box covers that much around the camera at every placement it can
        // reach, CascadeStatus::guaranteedRadius) less M cells - the bounce's reach, an edge difference spreading inward
        // (MEASURED: kGiHitLevelMarginCells) - from the camera, blending out over the hand-over band below it. Walk and
        // teleport then read the same level from the same cells (the grid is world-anchored), whatever the placement.
        {
            const float marginCells = giHitLevelMarginCells(mArms ? mArms->value(ArmId::GiHitLevelMargin) : -1.0);
            out.levelRadius[c] = giHitLevelRadius(v.shape, marginCells);
            out.levelBand[c] = kGiHitLevelBandCells * v.cell;
            out.levelMaxLod[c] = giPlacementLevelK(v.shape.stepCells);
        }
        ++out.cascadeCount;
    }
    {
        Ogre::Vector3 cam;
        out.levelOn = scene->photon().giCameraPos(cam);
        out.levelCam[0] = cam.x; out.levelCam[1] = cam.y; out.levelCam[2] = cam.z;
    }
    // ---- THE ONE ENVIRONMENT (PHOTON-ENV-1): the sky's captured cube, or the flat environment when there is no
    // cube (OgreScene::rayEnvironment). NO VOXELS IS NOT "NO RAYS": an escaping ray still reads the sky.
    {
        const OgreScene::RayEnvironment rayEnv = scene->rayEnvironment();
        out.sky = rayEnv.cube;
        for (int i = 0; i < 3; ++i) out.skyColour[i] = rayEnv.colour[i];
    }
    // ---- THE SURFACE CACHE THE HITS READ FIRST (PHOTON-CARDS-2, GA-1e): its two tables, its two atlas layers and the
    // view term's seven when the scene holds a built cache with an instance slot (OgreScene::updateSurfaceCache - the
    // `cards` row); otherwise nothing, and the stand-ins are bound with zero slots: every hit reads the voxels.
    if (const SurfaceCache *cache = scene->photon().surfaceCache()) {
        Ogre::TextureGpu *view[SurfaceCache::kViewLayers] = {};
        cache->viewLayers(view);
        bool viewBound = true;
        for (Ogre::TextureGpu *t : view) viewBound = viewBound && t;
        if (cache->cardBuffer() && cache->instanceBuffer() && cache->depthLayer() && cache->radianceLayer() &&
            viewBound && cache->instanceSlots() > 0u) {
            out.cardTable = cache->cardBuffer();
            out.cardInstances = cache->instanceBuffer();
            out.cardDepth = cache->depthLayer();
            out.cardRadiance = cache->radianceLayer();
            for (unsigned i = 0; i < SurfaceCache::kViewLayers; ++i) out.cardView[i] = view[i];
            out.cardSlots = cache->instanceSlots();
            out.cardRecords = cache->cardRecords();
        }
    }
    // ---- THE HIT'S GEOMETRIC NORMAL: the per-slot row table the TLAS was written with and the GPU scene's rows,
    // FLUSHED first (a row staged but not uploaded is a zero address - the trap file's GPU SCENE TABLES rule) and the
    // table pointer re-read here, never cached across a frame.
    {
        detail::GpuScene &gpuScn = scene->gpuScene();
        if (gpuScn.live()) gpuScn.flushGeomRows();
        Ogre::UavBufferPacked *rows = gpuScn.live() ? gpuScn.geomBuffer() : nullptr;
        if (rows && !sa.geomRowOfSlot.empty()) {
            out.geomRows = rows;
            out.geomRowOfSlot = &sa.geomRowOfSlot;
        }
    }
    // ---- THE HIT'S OWN SURFACE (FAR-SUN-2), the alpha table (REFLECT-MOVERS-2), the movers (MOVER-OCCLUSION-1)
    out.hitTable = hitTableOf(scene);
    {
        const HitSurfaceInputs hs = hitSurfaceInputsOf(scene);
        out.hitPool = hs.pool;
        out.cloudField = hs.cloudField;
        std::memcpy(out.cloudMap, hs.cloudMap, sizeof(hs.cloudMap));
        std::memcpy(out.cloudSun, hs.cloudSun, sizeof(hs.cloudSun));
    }
    out.alphaTable = alphaTableOf(scene);
    out.tierCounters = tierCountersAddress();
}

uint64_t RayQueryTier::tierCountersAddress() {
    if (!mTierCounters.buffer) {
        std::string err;
        if (!makeBuffer(16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, true, mTierCounters, err)) return 0u;
        std::memset(mTierCounters.mapped, 0, 16u);
    }
    return uint64_t(addressOf(mTierCounters.buffer));
}

uint64_t RayQueryTier::tlasAddressOf(const OgreScene *scene) const {
    const SceneAs *sa = asOf(scene);
    if (!sa || !sa->tlas || !sa->st.enabled || sa->instanceCount == 0u) return 0u;
    VkAccelerationStructureDeviceAddressInfoKHR ai{};
    ai.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    ai.accelerationStructure = sa->tlas;
    return uint64_t(mFn.getAsDeviceAddress(mVk, &ai));
}

uint64_t RayQueryTier::alphaTableOf(const OgreScene *scene) const {
    // The arm "reflect.alphaTested" = 0 traces every cut-out opaque (the cost arm).
    if (!arm(ArmId::ReflectAlphaTested, true)) return 0u;
    const SceneAs *sa = asOf(scene);
    return sa ? uint64_t(sa->alphaAddress) : 0u;
}

void RayQueryTier::recordMotion(const ReflectPassListener *key, OgreView *view,
                                Ogre::CompositorPass *pass) {
    if (!isOpen() || mMotionFailed || !view || !pass) return;
    // THE ARM ("reflect.motion", latched per frame so one process holds both arms):
    // off, the chain's clear stands and the resolve takes the camera path.
    if (!arm(ArmId::ReflectMotion, true)) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam || view->stereo()) return;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    // (the chain declares both only with the march and the id pass: never ask it
    // otherwise — a miss is logged as an exception every frame)
    if (!view->chainDesc().atomDraw) return;
    Ogre::TextureGpu *vel = nullptr, *ids = nullptr, *depth = nullptr, *idDepth = nullptr;
    try {
        vel = node->getDefinedTexture(Ogre::IdString("jahSsrVelocity"));
        ids = node->getDefinedTexture(Ogre::IdString(kAtomIdTexture));
        depth = node->getDefinedTexture(Ogre::IdString("jahDepth"));
        // THE ID PASS'S OWN DEPTH (ID-DEPTH-1; the chain copies it every frame on a
        // chain whose job runs — OgreChain.cpp, the atom id depth).
        idDepth = node->getDefinedTexture(Ogre::IdString("jahAtomViewDepth"));
    } catch (Ogre::Exception &) { return; }
    if (!vel || !ids || !depth || !idDepth || !vel->isUav()) return;
    const unsigned w = vel->getWidth(), h = vel->getHeight();
    if (!w || !h || ids->getWidth() != w || ids->getHeight() != h || depth->getWidth() != w ||
        depth->getHeight() != h || idDepth->getWidth() != w || idDepth->getHeight() != h)
        return;
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::UavBufferPacked *instances = gs.live() ? gs.instanceBuffer() : nullptr;
    if (!instances || !gs.slotCount()) return;
    std::string err;
    if (!mMotionPipeline) {
        if (!makeMotionPipeline(err)) {
            mMotionFailed = true;
            Ogre::LogManager::getSingleton().logMessage("Jahshaka: the march's object motion off — " + err);
            return;
        }
    }
    ReflectView &rv = mReflects[key];
    const unsigned ring = rv.motionFrame % kMotionRing;
    if (!rv.motionParams[ring].buffer &&
        !makeBuffer(sizeof(MotionParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    rv.motionParams[ring], err))
        return;
    if (!rv.motionSets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mMotionPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mMotionSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &rv.motionSets[ring]) != VK_SUCCESS) {
            rv.motionSets[ring] = VK_NULL_HANDLE;
            return;
        }
    }
    // THE PARAMETERS: this camera's image basis expanded to the target (the trace's
    // own form, so the two reconstruct one world point from one depth).
    MotionParams mp{};
    {
        const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
        Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
        cam->getFrustumExtents(fl, fr, ft, fb, ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
        EyeBasisF e = eyeBasis(ortho, cam->getDerivedPosition(), cam->getDerivedOrientation(), float(fl),
                               float(fr), float(ft), float(fb));
        expandEyeToTarget(e, view->chainDesc(), w, h);
        memcpy(mp.camPos, e.camPos, sizeof(mp.camPos));
        memcpy(mp.rayTL, e.rayTL, sizeof(mp.rayTL));
        memcpy(mp.rayRight, e.rayRight, sizeof(mp.rayRight));
        memcpy(mp.rayDown, e.rayDown, sizeof(mp.rayDown));
        memcpy(mp.fwd, e.fwd, sizeof(mp.fwd));
        const Ogre::Vector2 ab = cam->getProjectionParamsAB();
        mp.projParams[0] = ab.x;
        mp.projParams[1] = ab.y;
        mp.resolution[0] = float(w);
        mp.resolution[1] = float(h);
        mp.resolution[2] = float(gs.slotCount());
    }
    // THE POSED JOB (SKINNED-VELOCITY-1): only while a POSE moved in the last two
    // frames (the skin pass' own stamp) and the scene holds a TLAS to ask; its rows
    // are the ones the TLAS was written with, flushed first (a row staged but not
    // uploaded is a zero address — the GPU SCENE TABLES rule).
    SceneAs *skinSa = nullptr;
    Ogre::UavBufferPacked *skinGeomRows = nullptr;
    uint32_t skinGeomSlots = 0u;
    if (mMotionSkinPipeline && arm(ArmId::ReflectPosed, true)) {
        SceneAs *sit = asOf(scene);
        const uint32_t vaoFrame = mRs->getVaoManager()->getFrameCount();
        if (sit && sit->tlas && sit->st.enabled &&
            sit->skinPosedFrame != 0xFFFFFFFFu && uint32_t(vaoFrame - sit->skinPosedFrame) <= 2u) {
            skinSa = sit;
            gs.flushGeomRows();
            skinGeomRows = gs.geomBuffer();
            skinGeomSlots = skinGeomRows ? uint32_t(skinSa->geomRowOfSlot.size()) : 0u;
            if (skinGeomSlots) {
                RawBuffer &rb = rv.motionGeomRowOfSlot[ring];
                const VkDeviceSize want = VkDeviceSize(skinGeomSlots) * sizeof(uint32_t);
                if (rb.buffer && rb.size < want) retire(rb);
                if (!rb.buffer && !makeBuffer(std::max<VkDeviceSize>(want, 1024u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                              true, false, rb, err))
                    skinGeomSlots = 0u;
                else
                    std::memcpy(rb.mapped, skinSa->geomRowOfSlot.data(), size_t(want));
            }
            if (!skinGeomSlots) skinSa = nullptr;
        }
    }
    mp.skin[0] = skinSa ? 1.0f : 0.0f;
    mp.skin[1] = float(skinGeomSlots);
    memcpy(rv.motionParams[ring].mapped, &mp, sizeof(mp));
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkDescriptorBufferInfo ub{};
    ub.buffer = rv.motionParams[ring].buffer;
    ub.range = sizeof(MotionParams);
    VkDescriptorImageInfo img[4] = {};
    img[0].sampler = mPointSampler;
    img[0].imageView = sampledView(depth);
    img[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img[3].sampler = mPointSampler;
    img[3].imageView = sampledView(idDepth);
    img[3].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img[1].sampler = mPointSampler;
    img[1].imageView = sampledView(ids);
    img[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = vel;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = vel->getPixelFormat();
        img[2].imageView = static_cast<Ogre::VulkanTextureGpu *>(vel)->createView(slot, false);
        retireView(img[2].imageView);
        img[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!img[0].imageView || !img[1].imageView || !img[2].imageView || !img[3].imageView) return;
    VkDescriptorBufferInfo instInfo{};
    {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(instances->getBufferInterface());
        instInfo.buffer = bi->getVboName();
        instInfo.offset = VkDeviceSize(instances->_getFinalBufferStart()) * instances->getBytesPerElement();
        instInfo.range = instances->getTotalSizeBytes();
    }
    VkWriteDescriptorSet wr[kMotionBindings] = {};
    for (unsigned i = 0; i < kMotionBindings; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = rv.motionSets[ring];
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = kMotionTypes[i];
    }
    wr[0].pBufferInfo = &ub;
    wr[1].pImageInfo = &img[0];
    wr[2].pImageInfo = &img[1];
    wr[3].pBufferInfo = &instInfo;
    wr[4].pImageInfo = &img[2];
    wr[5].pImageInfo = &img[3];
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    VkDescriptorBufferInfo geomBufs[2] = {};
    if (skinSa) {
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &skinSa->tlas;
        wr[6].pNext = &asWrite;
        geomBufs[0].buffer = rv.motionGeomRowOfSlot[ring].buffer;
        geomBufs[0].range = VK_WHOLE_SIZE;
        auto *gbi = static_cast<Ogre::VulkanBufferInterface *>(skinGeomRows->getBufferInterface());
        geomBufs[1].buffer = gbi->getVboName();
        geomBufs[1].offset = VkDeviceSize(skinGeomRows->_getFinalBufferStart()) * skinGeomRows->getBytesPerElement();
        geomBufs[1].range = skinGeomRows->getTotalSizeBytes();
        wr[7].pBufferInfo = &geomBufs[0];
        wr[8].pBufferInfo = &geomBufs[1];
    }
    vkUpdateDescriptorSets(mVk, skinSa ? kMotionBindings : kMotionRigidBindings, wr, 0, nullptr);
    // THE LAYOUTS through Ogre's solver, before the command buffer is taken: the
    // velocity (cleared by the chain as a render target) as a UAV; the resolve's own
    // barrier analysis, after this early pre-execute, takes it back to a texture.
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, vel, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Write,
                                 computeStage);
        for (Ogre::TextureGpu *t : { depth, ids, idDepth })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                     computeStage);
        solver.resolveTransition(trans, instances, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    monitor::CacheScope motionJob(CacheKind::Gi, WorkReason::Camera, 0, "rq.reflect.motion", mRs, key);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMotionPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMotionPipeLayout, 0, 1,
                            &rv.motionSets[ring], 0, nullptr);
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1u);
    if (skinSa) {
        // THE POSED PIXELS, after the rigid ones: the two write disjoint pixels (the
        // id pass drew them or it did not), ordered anyway so no write races a write.
        VkMemoryBarrier waw{};
        waw.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        waw.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        waw.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &waw, 0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mMotionSkinPipeline);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1u);
    }
    motionJob.close();
    ++rv.motionFrame;
}

void RayQueryTier::finishReflect(const ReflectPassListener *key) {
    joinAhead(kAheadReflect);   // (its members are read below: the ahead build is joined first)
    auto it = mReflects.find(key);
    if (it == mReflects.end() || !it->second.finishPending) return;
    ReflectView &rv = it->second;
    rv.finishPending = false;
    const unsigned ring = rv.finishRing;
    const unsigned traceW = rv.traceW, traceH = rv.traceH;
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    // THE MEAN IS WRITTEN; NOW IT IS READ BY ITS NEIGHBOURS. A plain memory
    // barrier is enough and an image barrier would be wrong: the temporal mean
    // stays in GENERAL for both passes and only the ACCESS has to be ordered.
    {
        VkMemoryBarrier meanReady{};
        meanReady.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        meanReady.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        meanReady.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &meanReady, 0, nullptr, 0,
                             nullptr);
    }
    // THE RESOLVE (PHOTON-7 stage 6 commit 6): each texel's answer reconstructed from its own sample and its neighbours'
    // - after the write-back (every sample complete), before the temporal pass reads its neighbourhood. THE TEMPORAL
    // PASS (PHOTON-2 1b; Lumen's since commit 6): the histories reprojected, clamped to the resolved neighbourhood and
    // blended - before the filter reads the mean's neighbours. A HELD frame (1e) has no sample: the filter composites
    // the held mean.
    if (!rv.held) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mResolvePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1,
                            &rv.sets[ring], 0, nullptr);
    {
        monitor::CacheScope resolveRow(CacheKind::Gi, WorkReason::Camera, 0, "rq.reflect.resolve", mRs, key);
        vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, rv.layers);   // z = the eye
    }
    {
        VkMemoryBarrier resolved{};
        resolved.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        resolved.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        resolved.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &resolved, 0, nullptr, 0, nullptr);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTemporalPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1,
                            &rv.sets[ring], 0, nullptr);
    {
        monitor::CacheScope temporalRow(CacheKind::Gi, WorkReason::Camera, 0, "rq.reflect.temporal", mRs, key);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (traceW + 7u) / 8u, (traceH + 7u) / 8u, rv.layers);   // z = the eye
    }
    {
        VkMemoryBarrier meanBlended{};
        meanBlended.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        meanBlended.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        meanBlended.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &meanBlended, 0, nullptr, 0, nullptr);
    }
    }
    // THE FILTER AND THE COMPOSITE — the same descriptor set, a different
    // pipeline. Its time is the SAME row as the trace's, deliberately:
    // what a budget cares about is what the reflection costs, and the split
    // between tracing and filtering is ours, not the frame's.
    // The set is the trace's (the same layout); a second command-buffer bind,
    // since a scene pass ran between the two halves.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mReflectPipeLayout, 0, 1,
                            &rv.sets[ring], 0, nullptr);
    {
        // THE SPATIAL FILTER (PHOTON-7 stage 6 commit 7, Lumen's): one thread per FULL-RESOLUTION pixel - every pixel of
        // the reflection written every frame, the odd remainder included.
        monitor::CacheScope filterRow(CacheKind::Gi, WorkReason::Camera, 0, "rq.reflect.filter", mRs, key);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (rv.fullW + 7u) / 8u, (rv.fullH + 7u) / 8u, rv.layers);   // z = the eye
    }
    readReflectHistory(rv, cmd);
}

// THE REFLECTION HISTORY'S COUNT TO THE HOST (PHOTON-2 stage 0, the arm "reflect.historyReadback") — the stillness
// door's count histogram. The retired copies are decoded first (a slot is reused only after its frame retired: the
// ring is the trace's own, kReflectRing >= the frames in flight), then this frame's STATE TEXEL — written by the trace,
// the decode's write-back and the temporal pass, read by the filter above — is copied into its slot (the counts are
// the state's since PHOTON-7 stage 6). Nothing the renderer reads changes.
void RayQueryTier::readReflectHistory(ReflectView &rv, VkCommandBuffer cmd) {
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (ReflectView::HistPending &p : rv.histPending) {
        if (!p.live || !rv.histReadback.mapped || uint32_t(now - p.frame) < inFlight) continue;
        const size_t slot = size_t(&p - rv.histPending);
        const size_t layerTexels = size_t(p.w) * p.h;
        const size_t slotBytes = layerTexels * p.layers * 8u;
        const uint32_t *src =
            reinterpret_cast<const uint32_t *>(static_cast<const char *>(rv.histReadback.mapped) + slot * slotBytes);
        const size_t pairW = size_t(p.w) * p.layers;
        detail::decodeReflectHistory(src, p.w, p.h, p.layers, rv.histHost);   // -O2 (ReadbackDecode.h)
        // (the moving SE, the instrument's: layer-major on the device, the eyes side by side here like the count)
        rv.histHostSe.clear();
        if (p.se && rv.seReadback.mapped) {
            const float *se = reinterpret_cast<const float *>(static_cast<const char *>(rv.seReadback.mapped) +
                                                              slot * layerTexels * p.layers * 4u);
            rv.histHostSe.assign(pairW * p.h, 0.0f);
            for (unsigned k = 0; k < p.layers; ++k)
                for (unsigned y = 0; y < p.h; ++y)
                    std::memcpy(rv.histHostSe.data() + size_t(y) * pairW + size_t(k) * p.w,
                                se + (size_t(k) * p.h + y) * p.w, size_t(p.w) * 4u);
        }
        rv.histHostW = unsigned(pairW);
        rv.histHostH = p.h;
        rv.histHostFrame = p.traced;
        p.live = false;
    }
    if (!arm(ArmId::ReflectHistoryReadback, false)) {
        for (ReflectView::HistPending &p : rv.histPending) p.live = false;
        if (!rv.histHost.empty()) {
            rv.histHost.clear();
            rv.histHost.shrink_to_fit();
        }
        if (!rv.histHostSe.empty()) {
            rv.histHostSe.clear();
            rv.histHostSe.shrink_to_fit();
        }
        if (rv.seReadback.buffer) retire(rv.seReadback);
        return;
    }
    const unsigned w = rv.traceW, h = rv.traceH;
    if (!w || !h || !rv.state[rv.curIdx].image) return;
    const VkDeviceSize slotBytes = VkDeviceSize(w) * h * rv.layers * 8u;
    if (rv.histReadback.buffer && rv.histReadback.size < slotBytes * kReflectRing) {
        retire(rv.histReadback);
        for (ReflectView::HistPending &p : rv.histPending) p.live = false;
    }
    if (!rv.histReadback.buffer) {
        std::string err;
        if (!makeBuffer(slotBytes * kReflectRing, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false, rv.histReadback, err))
            return;
    }
    const unsigned slot = rv.finishRing % kReflectRing;
    // A SLOT STILL IN FLIGHT is never overwritten (a ring as deep as the frames in flight never has one; a deeper
    // pipeline skips this frame's copy rather than race the host's read).
    if (rv.histPending[slot].live) return;
    VkMemoryBarrier toCopy{};
    toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0,
                         nullptr, 0, nullptr);
    VkBufferImageCopy region{};
    region.bufferOffset = VkDeviceSize(slot) * slotBytes;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = rv.layers;
    region.imageExtent = { w, h, 1u };
    vkCmdCopyImageToBuffer(cmd, rv.state[rv.curIdx].image, VK_IMAGE_LAYOUT_GENERAL, rv.histReadback.buffer, 1, &region);
    // ...and THE MOVING SE beside it (the instrument at the arm's size: the temporal pass wrote this frame's)
    const VkDeviceSize seBytes = VkDeviceSize(w) * h * rv.layers * 4u;
    bool seCopied = false;
    if (rv.movingSe.buffer && rv.movingSe.size == seBytes) {
        if (rv.seReadback.buffer && rv.seReadback.size < seBytes * kReflectRing) retire(rv.seReadback);
        std::string serr;
        if (rv.seReadback.buffer ||
            makeBuffer(seBytes * kReflectRing, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false, rv.seReadback, serr)) {
            VkBufferCopy sc{};
            sc.dstOffset = VkDeviceSize(slot) * seBytes;
            sc.size = seBytes;
            vkCmdCopyBuffer(cmd, rv.movingSe.buffer, rv.seReadback.buffer, 1, &sc);
            seCopied = true;
        }
    }
    // ...ordered before the host's read AND before the trace that writes this half again two frames on.
    VkMemoryBarrier toHost{};
    toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toHost, 0, nullptr, 0, nullptr);
    ReflectView::HistPending &p = rv.histPending[slot];
    p.frame = now;
    p.live = true;
    p.w = w;
    p.h = h;
    p.layers = rv.layers;
    p.traced = rv.frame - 1u;   // the trace's ++rv.frame ran before this half
    p.se = seCopied;
}

// ---------------------------------------------------------------------------
// THE SCREEN-PROBE GATHER — the tier's half (GATHER-1a, 2026-09-21).
//
// Everything below reads the SCENE and hands the Component a record of it. The
// split is deliberate and it is the whole of SCREEN_PROBE_GATHER_SPEC section
// 9: an algorithm that will grow a filter, an SH record, history pairs and
// importance sampling belongs in a Component of ours; the device, the
// structures and the friendship with `OgreScene` belong here.

void RayQueryTier::releaseGatherBinding(const ReflectPassListener *key) {
    if (mGather) mGather->releaseBinding(key);
}

void RayQueryTier::forgetGather(const ReflectPassListener *key) {
    if (mGather) mGather->forget(key, key && key->mFoldsOnForget);
}

void RayQueryTier::gatherStatsInto(const OgreScene *scene, GatherStatus &out) const {
    if (mGather) mGather->statsInto(scene, scene->gatherRestartKey(), out);
}

/// THE CARDS' VIEW READERS' REST DEMAND COMPLETE (PHOTON-7 stage 10, R3 (b), the lead's ruling): every reader of the
/// scene's cards that can rest has traced the first N frames of its rest - the SAME N for both readers, the gather's rest
/// sequence (16 at the shipped history): those N frames of the deterministic rest sequence are the demand; a reflection
/// texel's continuation to its stop is variance reduction on faces already demanded (a face it first reaches after N
/// has a rate under 1/N a frame - a band face). The surface cache takes its rest's ONE residency decision then, and the
/// readers' hold waits on that decision's work (the cache's queue and relights are their inputs) - the reflection's
/// long rest runs once, after it (photon-glass: 246 frames to stop every texel, run twice when the decision waited for
/// it). As of the readers' last record (the cache plans before they record: a frame late).
bool RayQueryTier::cardReadersRestComplete(const OgreScene *scene, unsigned restFrames[2], bool active[2]) const {
    // (the readers by the demand's index: 0 the reflection, 1 the gather - each one's rest length so far, the least
    // over its views; 0 where none can rest; and whether it recorded lately at all)
    restFrames[0] = restFrames[1] = 0u;
    active[0] = active[1] = false;
    unsigned n = 0u;
    bool complete = true;
    // THE DEMAND SPAN (PHOTON-7 stage 10, the per-reader snapshot): each reader stamps its rest indices [1, L] alone,
    // L = N + the due (SurfaceCache::demandDueFrames), and the demand is complete once every reader has traced its L -
    // so the decision, a due later, reads exactly each reader's span whatever its rest started on
    const unsigned due = SurfaceCache::demandDueFrames();
    if (mGather) complete = mGather->restDemandComplete(scene, restFrames[1], n, due, active[1]);
    else n = ScreenProbeGather::shippedRestLength();
    mDemandSpan = n + due;
    for (const auto &kv : mReflects) {
        const ReflectView &rv = kv.second;
        if (rv.scene != scene) continue;
        // (a view that stopped recording - the editor's under a screenshot's offscreen settle - makes no demand)
        if (uint32_t(scene->drawSerial() - rv.recordedDraw) > ScreenProbeGather::kReaderStaleFrames) continue;
        active[0] = true;
        if (!rv.restableLast) continue;
        restFrames[0] = restFrames[0] ? std::min(restFrames[0], rv.restFrames) : rv.restFrames;
        if (!rv.held && rv.restFrames < mDemandSpan) complete = false;
    }
    return complete;
}

bool RayQueryTier::gatherRestPastHandOver(const OgreScene *scene) const {
    if (!mGather) return true;
    unsigned restFrames = 0u, n = 0u;
    bool active = false;
    return mGather->restDemandComplete(scene, restFrames, n, 0u, active);
}

bool RayQueryTier::reflectRestPastHandOver(const OgreScene *scene) const {
    for (const auto &kv : mReflects) {
        const ReflectView &rv = kv.second;
        if (rv.scene != scene) continue;
        if (uint32_t(scene->drawSerial() - rv.recordedDraw) > ScreenProbeGather::kReaderStaleFrames) continue;
        if (!rv.restableLast) continue;
        // THE HAND-OVER IS 2N REST SAMPLES (jah_reflect_rest.glsl, jahRestHandOverLength) and a converging texel takes at
        // least one a frame (its batch: up to k, the first that needs a record ends it), while a texel stops only past
        // it (rq_reflect_temporal.comp's `settled`): 2N rest frames bound every texel's hand-over. N frames did not
        // (scripting.e2e.resize_history at 21 rest frames, N 19: the two boots still 5,876 px apart).
        if (rv.restFrames < 2u * std::max(rv.restN, 1u)) return false;
    }
    return true;
}

void RayQueryTier::recordGather(const ReflectPassListener *key, OgreView *view,
                                Ogre::CompositorPass *pass, const HitListBinding &hit) {
    if (!isOpen() || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;
    // ...AND ONLY FOR A VIEW WHOSE CHAIN GATHERS (ChainDesc::probeGather: the scene's answer AND the view's offscreen
    // contract - a Live or undeclared offscreen view takes the field's answer). Recorded for any view with the prepass,
    // the gather bound its texture into a chain that is not a gathering chain - whose opaque pass reads the world
    // caches - and the pass read two diffuse terms and two speculars (measured, PHOTON-7 7-5: a 0.35-rough metal on
    // gi.rt_reflect_asan's undeclared view at 2.19x the reflection's reference, a blended pane in a live view +29 %).
    if (!scene->probeGatherWanted() || !view->chainDesc().probeGather) {
        // OFF IS FREE, and it also has to be CLEAN: a view that was gathering
        // and stopped must give its atlases back and stop binding its texture
        // to the shader.
        if (mGather && mGather->holds(key)) mGather->forget(key);
        return;
    }
    // (the test door gi.screenBindMissing: the frame declines as for a missing input - resolveRayJobs registers the
    // last gather, F3)
    if (arm(ArmId::GiScreenBindMissing, false)) return;
    SceneAs *saP = asOf(scene);
    if (!saP) return;
    SceneAs &sa = *saP;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) return;
    // TWO EYES, ONE GATHER (PHOTON-GA-VR — the reflection trace's REFLECT-VR-1
    // rule, below where the basis is built): a stereo target is gathered per eye
    // in the same dispatches, each eye's layer through its own located basis,
    // each eye its own probe grid (ScreenProbeGather). A stereo view whose eyes have
    // not been pushed yet DECLINES — gathering the head's frustum across a
    // two-eye target would place every probe at a wrong world point.
    const bool stereo = view->stereo();
    const StereoEyeBasis *eyes = view->stereoEyes();
    if (stereo && !eyes) {
        if (mGather && mGather->holds(key)) mGather->forget(key);
        return;
    }

    // THE CHAIN'S TEXTURES, by the names OgreChain.cpp declares them under —
    // the same two the reflection trace reads, and for the same reason: a
    // probe's surface is the one its pixel drew.
    Ogre::TextureGpu *normalTex = nullptr, *depthTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        depthTex = node->getDefinedTexture(Ogre::IdString("jahDepth"));
    } catch (Ogre::Exception &) { return; }
    if (!normalTex || !depthTex) return;
    if (!depthTex->getWidth() || !depthTex->getHeight()) return;

    // ...and the prepass's shadow / roughness pair (PHOTON-7 stage 7, 7-3: the rough specular's roughness), the
    // reflection's own read of it - every prepass chain declares it (OgreChain.cpp, the prepass block)
    Ogre::TextureGpu *shadowRoughTex = nullptr;
    try {
        shadowRoughTex = node->getDefinedTexture(Ogre::IdString("jahSsrShadowRough"));
    } catch (Ogre::Exception &) { return; }
    if (!shadowRoughTex) return;

    GatherInputs in;
    in.scene = scene;
    in.sceneMgr = scene->sceneManager();
    in.normals = normalTex;
    in.depth = depthTex;
    in.shadowRough = shadowRoughTex;
    // THE ROUGH SPECULAR'S GATE (7-3): where the view TRACES its reflection, the reflection owns the lobe up to its
    // cutoff less its feather (the composite's full confidence, rq_reflect_filter.comp's gate) and the probes' lobe is
    // the environment term above it; a view with no traced reflection (Low, Medium, the march alone) takes the lobe
    // everywhere
    {
        const ChainDesc &cd = view->chainDesc();
        in.roughGateFull = cd.rayReflect && cd.ssr > 0
                               ? std::min(std::max(cd.reflectionRoughnessCutoff, 0.0f), 1.0f) - kRayReflectFeather
                               : -1.0f;
        in.roughGateRamp = kRayReflectFeather;
    }
    in.width = depthTex->getWidth();
    in.height = depthTex->getHeight();
    // THE PHOTON VIEW'S ScreenProbes (PHOTON-VIEW-1): the view's overlay, written
    // by the integrate when the scene's view asks and it is the gather's size.
    // ...and its GatherHits (FAR-SUN-1): the same overlay, written by the trace ray by ray.
    if (scene->photonView() == PhotonView::ScreenProbes || scene->photonView() == PhotonView::GatherHits) {
        Ogre::TextureGpu *o = view->photonOverlay();
        if (o && o->getWidth() == in.width && o->getHeight() == in.height &&
            o->getNumSlices() == depthTex->getNumSlices()) {
            in.photonOverlay = o;
            in.photonOverlayGeneration = view->photonOverlayGeneration();
            in.photonOverlayGatherHits = scene->photonView() == PhotonView::GatherHits;
        }
    }
    // THE GATHER-HITS READBACK (FAR-SUN-1): the instrument's arm.
    in.hitsReadback = arm(ArmId::GatherHitsReadback, false);
    // A STEREO VIEW'S TEXTURES ARE LAYERED, one eye per layer (LAYERED-STEREO-1):
    // `width` is one eye's, and a stereo chain whose depth is not two layers is
    // not a picture the gather can split.
    if (stereo != (depthTex->getNumSlices() == 2u)) return;
    in.stereo = stereo;
    // THE TIER TABLE'S GATHER ROW (GA-TIERROW: the table, never the SSR row), in
    // the column `probeGatherWanted` reads: the VR column while a headset drives
    // the scene's GI (every view of it then — the eyes, and the mono control an
    // eye screenshot compares with them — gathers at the headset's density).
    in.facts = giQualityFacts(scene->giParams().quality,
                              scene->photon().driverStereo() ? GiViewProfile::Vr : GiViewProfile::Desktop)
                   .gather;
    in.tuning = scene->gatherTuning();
    // THE GATHER'S MEASUREMENT ARMS (GATHER-NOISE-1): each one that is set
    // overrides the scene's tuning field it names; 0 leaves the scene's.
    {
        const auto take = [&](ArmId a, unsigned &field) {
            const double v = armValue(a);
            if (v > 0.0) field = unsigned(v);
        };
        take(ArmId::GatherOctRes, in.tuning.octRes);
        take(ArmId::GatherStride, in.tuning.probeStride);
        take(ArmId::GatherHistory, in.tuning.historyFrames);
        take(ArmId::GatherFilterRadius, in.tuning.filterRadius);
        take(ArmId::GatherRestFrames, in.tuning.restFrames);
        take(ArmId::GatherRestSeed, in.tuning.restSeed);
        take(ArmId::GatherAgeView, in.tuning.ageView);
        if (arm(ArmId::GatherRestOff, false)) in.tuning.restOff = true;
        if (arm(ArmId::GatherFreezeFrame, false)) in.tuning.freezeFrameIndex = true;
        take(ArmId::GatherYoungFrames, in.tuning.youngFrames);
        if (arm(ArmId::GatherValidationOff, false)) in.tuning.historyValidationOff = true;
        if (arm(ArmId::GatherClassOff, false)) in.tuning.classOff = true;
        if (arm(ArmId::GiFreeze, false)) in.tuning.freezeHistory = true;
        take(ArmId::GatherCrossStrata, in.tuning.crossStrata);
        if (armValue(ArmId::GatherYoungReach) > 0.0)
            in.tuning.youngReach = float(armValue(ArmId::GatherYoungReach));
        // the reconstruction's arms (decision 20) and the E/pi instrument
        const double jitter = armValue(ArmId::GatherJitter);
        if (jitter > 0.5) in.tuning.jitterOff = true;
        if (arm(ArmId::GatherFilterOff, false)) in.tuning.filterOff = true;
        if (arm(ArmId::GatherIrradianceReadback, false)) in.tuning.readback = true;
        if (arm(ArmId::GatherHistoryReadback, false)) in.tuning.historyReadback = true;
    }
    // THE RESUME'S INPUTS (PHOTON-7 stage 8B, B1; GatherRestInputs): the frame's changed-region list as it closed before
    // the render (its buffer, read by the cells' tests on the device), each cascade's brick mask this frame's light work
    // wrote (in the hit set's order: the cascades the hits read), and whether the field is at work. The view-wide rest key,
    // gatherInputsBusy and the readers' hold owed are no longer the gather's (8B's CRUD).
    {
        GatherRestInputs &r = in.rest;
        const PhotonComponent &ph = scene->photon();
        r.drawSerial = scene->drawSerial();
        r.regionSerial = ph.regionCloseSerial();
        r.lastInputClose = ph.lastInputCloseSerial();
        const GiChangedRegions &list = ph.changedRegions();
        if (Ogre::ConstBufferPacked *rb = ph.regionBuffer()) {
            auto *bi = static_cast<Ogre::VulkanBufferInterface *>(rb->getBufferInterface());
            r.regions = bi->getVboName();
            r.regionsOffset = VkDeviceSize(rb->_getFinalBufferStart()) * rb->getBytesPerElement();
            r.regionsRange = rb->getTotalSizeBytes();
            r.regionCount = list.available ? unsigned(list.regions.size()) : 0u;
            r.overflow = list.available ? list.overflow : 0u;
            r.restart = list.available && list.restart;
        }
        std::vector<PhotonComponent::Volume> vols;
        ph.volumes(vols);
        unsigned c = 0u;
        for (const PhotonComponent::Volume &vol : vols) {
            if (c >= GatherRestInputs::kCascades) break;   // (hitSetOf's order and skips: the hits' cascades)
            if (!vol.lighting || !vol.voxelizer) continue;
            Ogre::TextureGpu **tex = vol.lighting->getLightVoxelTextures();
            if (!tex || !tex[0]) continue;   // (the hit set's own skip: the same order)
            GatherRestInputs::Bricks &b = r.bricks[c++];
            Ogre::UavBufferPacked *mask = vol.lighting->getBrickMaskBuffer();
            const Ogre::uint32 *dims = vol.lighting->getBrickMaskDims();
            const Ogre::Vector3 og = vol.voxelizer->getVoxelOrigin();
            const Ogre::Vector3 cl = vol.voxelizer->getVoxelCellSize();
            for (int a = 0; a < 3; ++a) {
                b.dims[a] = dims[a];
                b.origin[a] = og[a];
            }
            b.brickMetres = 8.0f * std::max(std::max(cl.x, cl.y), cl.z);
            if (mask) {
                auto *bi = static_cast<Ogre::VulkanBufferInterface *>(mask->getBufferInterface());
                b.buffer = bi->getVboName();
                b.offset = VkDeviceSize(mask->_getFinalBufferStart()) * mask->getBytesPerElement();
                b.range = mask->getTotalSizeBytes();
                b.live = vol.lighting->getBrickMaskFrame() == ph.regionCloseFrame();
            }
        }
        r.fieldBusy = ph.fieldWideWorkOwed();
        in.binsOff = arm(ArmId::GatherBinsOff, false);   // MEASURING ARM
        in.fastSkinPosed = posedSlotsOf(scene, mRs->getVaoManager()->getFrameCount()) != nullptr;
    }
    in.demandDue = SurfaceCache::demandDueFrames();   // (the gather's demand span: its rest indices [1, N + this])
    in.restartKey = scene->gatherRestartKey();
    in.farOverlap = sa.farOverlap;
    in.drawnCutFootprints = drawnFootprintsOf(scene, view->chainDesc().lodHysteresis);
    in.pointSnapPx = pointSnapKnob();
    // THE IRRADIANCE FIELD, THE FALLBACK OF THE PIXELS NO PROBE ANSWERS (PHOTON-7 stage 7, 7-2): the field the scene's
    // passes are bound to (SceneGiBinding), composed by the integrate after its temporal mean - its atlases, its render
    // block for an identity view (world to grid), its window and counts; and the environment's SH, its own fallback,
    // with the pass buffer's signs (PhotonVct_piece_ps.any's JAH_ENV_SH_C0 .. C8: C2 .. C5 negated)
    in.roughQuadrature = unsigned(std::max(0.0, armValue(ArmId::GatherRoughQuadrature)) + 0.5);
    {
        // THE VIEW'S CHAIN KEY (L2: a settings change restarts what it changes): the rough gate's two numbers (the
        // view's reflection cutoff) and the quadrature door, by their bits
        unsigned long long key = 1469598103934665603ull;
        const auto fold = [&key](unsigned long long v) {
            key ^= v;
            key *= 1099511628211ull;
        };
        unsigned bits = 0u;
        std::memcpy(&bits, &in.roughGateFull, sizeof bits);
        fold(bits);
        std::memcpy(&bits, &in.roughGateRamp, sizeof bits);
        fold(bits);
        fold(in.roughQuadrature);
        // ...AND THE A/B ARMS THE ESTIMATE DEPENDS ON (FAR-SUN-2's gi.hitDirect, the GI freeze; were the armed rest key's,
        // deleted with it, 8B): a paired A/B flips one on a still view, which restarts the view's rest whole
        fold(arm(ArmId::GiHitDirect, true) ? 1ull : 0ull);
        fold(arm(ArmId::GiFreeze, false) ? 1ull : 0ull);
        in.chainKey = key;
    }
    {
        const SceneGiBinding *gb = sceneGiBindingOf(scene->sceneManager());
        Ogre::PhotonIrradianceField *ifd = gb ? gb->ifd : nullptr;
        if (ifd && ifd->getIrradianceTex() && ifd->getDepthVarianceTex()) {
            in.fieldIrradiance = ifd->getIrradianceTex();
            in.fieldDepth = ifd->getDepthVarianceTex();
            ifd->fillConstBufferData(Ogre::Matrix4::IDENTITY, in.field);
            const Ogre::uint32 *o = ifd->getWindowOffset();
            in.fieldWin[0] = float(o[0] + 128u * o[1] + 16384u * o[2]);
            in.fieldWin[1] = float(ifd->getSettings().mNumProbes[1]);
            in.fieldWin[2] = float(ifd->getSettings().mNumProbes[2]);
            const float *sh = scene->sceneManager()->getSphericalHarmonics();
            static const float kSign[9] = { 1.0f, 1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
            for (int k = 0; k < 9; ++k)
                for (int c = 0; c < 3; ++c) in.envSh[k][c] = sh ? kSign[k] * sh[3 * k + c] : 0.0f;
        }
    }

    // ---- THE HIT SET (the tier's one collection, hitSetOf: the cascades, the environment, the surface cache, the
    // geometric normal's rows, the hit table and surface, the alpha table, the movers) and the view's HIT LIST
    // (PHOTON-HIT-SHADE-1): a far hit, a mover's, a rigged item's and a static hit no cache answers are appended for
    // the decode.
    in.hits.hit = hit;
    hitSetOf(scene, sa, in.hits);

    // ---- THE CAMERA'S BASIS: the ray tier's one eye basis (eyeBasis), the
    // LETTERBOX folded in exactly as the reflection and the sun contact fold it
    // (expandEyeToTarget) — a probe's pixel and the shot's inner rectangle agree.
    // UNDER STEREO, THE TWO LOCATED EYES (PHOTON-GA-VR): each eye's pose and
    // asymmetric frustum, mapped over its own half — recordReflect's arithmetic
    // through the same two functions. The VIEW AXES stay the rendering camera's
    // for both eyes, and that is exact (recordReflect's note: HlmsPbs uploads one
    // view matrix per pass, so both halves' G-buffer normals are in the HEAD's
    // view space).
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    EyeBasisF eyeB[2];
    if (stereo) {
        for (int i = 0; i < 2; ++i)
            eyeB[i] = eyeBasis(ortho, eyes[i].position, eyes[i].orientation, eyes[i].tanLeft,
                               eyes[i].tanRight, eyes[i].tanTop, eyes[i].tanBottom);
    } else {
        Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
        cam->getFrustumExtents(fl, fr, ft, fb,
                               ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
        eyeB[0] = eyeBasis(ortho, cam->getDerivedPosition(), q, float(fl), float(fr), float(ft),
                           float(fb));
        eyeB[1] = eyeB[0];
    }
    for (int i = 0; i < 2; ++i) expandEyeToTarget(eyeB[i], view->chainDesc(), in.width, in.height);
    std::memcpy(in.camPos, eyeB[0].camPos, sizeof(in.camPos));
    std::memcpy(in.rayTL, eyeB[0].rayTL, sizeof(in.rayTL));
    std::memcpy(in.rayRight, eyeB[0].rayRight, sizeof(in.rayRight));
    std::memcpy(in.rayDown, eyeB[0].rayDown, sizeof(in.rayDown));
    std::memcpy(in.fwd, eyeB[0].fwd, sizeof(in.fwd));
    std::memcpy(in.camPos2, eyeB[1].camPos, sizeof(in.camPos2));
    std::memcpy(in.rayTL2, eyeB[1].rayTL, sizeof(in.rayTL2));
    std::memcpy(in.rayRight2, eyeB[1].rayRight, sizeof(in.rayRight2));
    std::memcpy(in.rayDown2, eyeB[1].rayDown, sizeof(in.rayDown2));
    std::memcpy(in.fwd2, eyeB[1].fwd, sizeof(in.fwd2));
    const auto put = [](float dst[3], const Ogre::Vector3 &v) {
        dst[0] = float(v.x); dst[1] = float(v.y); dst[2] = float(v.z);
    };
    put(in.viewAxisX, q * Ogre::Vector3::UNIT_X);
    put(in.viewAxisY, q * Ogre::Vector3::UNIT_Y);
    put(in.viewAxisZ, -(q * Ogre::Vector3::NEGATIVE_UNIT_Z));   // view space looks down -Z
    const Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    in.projA = float(projAB.x);
    in.projB = float(projAB.y);
    in.farClip = float(cam->getFarClipDistance());

    if (!mGather) mGather = new ScreenProbeGather(*this);
    mGather->record(key, in);
    // The overlay holds the discs once an integrate has written it (a held frame
    // keeps the last one).
    if (in.photonOverlay && mGather->integrates(key)) view->notePhotonOverlayWritten();
}

// ---------------------------------------------------------------------------
// HARD SUN CONTACT SHADOWS — the tier's half (PHOTON-RAYS-1, RY-R3;
// SPECS/photon/C4_RAYS_BEYOND_REFLECTIONS_DESIGN.md section 1).
//
// ONE DISPATCH PER VIEW FRAME in the reflect listener's bracket (the gather's
// shape): one ray per texel from the prepass' surface towards the sun, tMax the
// contact range, the caster copies of the near field only, opaque only; the
// answer an R8 texture the PBS pass folds into the sun's shadow term as
// min( map, ray ). Deterministic per pixel per frame: no sample sequence, no
// history, nothing to reproject.
namespace {

/// The parameter block, std140, mirroring rq_sun_contact.comp's `Params`
/// member for member (every member a vec4, laid out identically to C).
struct SunContactParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float toSun[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
    /// THE ALPHA TABLE (REFLECT-MOVERS-2): xy = its device address, bit-copied.
    float alpha[4] = {};
};
constexpr unsigned kSunContactBindings = 5u;
/// THE FADE BAND: the last 20 % of the range hands the ray's answer over to the
/// map (the design's 10-20 %; the SSR rim's shape) — rq_sun_contact.comp.
/// Measured on the side edge of a 5 cm board's shadow at 15 m (gi.sun_contact,
/// the adjacent 5 cm step across the range's end): 15.1 codes with no fade,
/// 7.6 at 10 %, 5.4 at 15 %, 3.3-4.7 at 20 % (the map's own edge: 1.1-1.3).
constexpr float kSunContactFadeFraction = 0.2f;

/// The storage format of the visibility texture. R8 is an EXTENDED storage
/// format (shaderStorageImageExtendedFormats), not a core-mandatory one — so it
/// is asked of the device here, once, and a device that cannot store it runs
/// without the contact term rather than refusing the whole ray tier (the
/// tier-wide table in rayStorageFormatRefused is for the images every ray job
/// needs).
bool sunContactFormatStores(VkPhysicalDevice pd) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R8_UNORM, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}

}   // namespace

bool RayQueryTier::makeSunContactPipeline(std::string &err) {
    if (!mDev || !mDev->mPhysicalDevice || !sunContactFormatStores(mDev->mPhysicalDevice)) {
        err = "the device cannot store to R8_UNORM (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)";
        return false;
    }
    VkDescriptorSetLayoutBinding b[kSunContactBindings] = {};
    const VkDescriptorType types[kSunContactBindings] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0 tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1 params
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 2 gBufNormals
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3 sceneDepth
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 4 sunVis
    };
    for (unsigned i = 0; i < kSunContactBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kSunContactBindings, kMaxTimedScenes * kReflectRing, mSunSetLayout, err, "rayquery/sun-contact")) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mSunSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mSunPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_sunContactSpv);
    smi.pCode = krq_sunContactSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mSunModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mSunModule;
    cpi.stage.pName = "main";
    cpi.layout = mSunPipeLayout;
    if (createComputePipeline(cpi, &mSunPipeline, "sun contact") != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }
    // The pool its layout planned: kMaxTimedScenes views' worth of rings, the reflection's ceiling.
    if (!plan.create(mVk, "rayquery/sun-contact", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mSunPool, err))
        return false;
    // POINT: the depth and the normal are read at exactly one texel.
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(mVk, &si, nullptr, &mSunSampler) != VK_SUCCESS) {
        err = "vkCreateSampler failed";
        return false;
    }
    return true;
}

void RayQueryTier::dropSunContact(SunContactView &sv) {
    // THE REGISTRATION FIRST: the listener holds a raw pointer to the texture
    // that is about to be retired (GATHER-0's D1, the same rule).
    if (sv.sceneMgr) FogHlmsListener::setSunContact(sv.sceneMgr, nullptr, 1u);
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(sv.sets[i], mSunPool);
        sv.sets[i] = VK_NULL_HANDLE;
        retire(sv.params[i]);
    }
    retireTexture(sv.vis);
}

void RayQueryTier::forgetSunContact(const ReflectPassListener *key) {
    auto it = mSunContacts.find(key);
    if (it == mSunContacts.end()) return;
    dropSunContact(it->second);
    mSunContacts.erase(it);
    monitor::forgetGpuOwner(key);
}

void RayQueryTier::finishSunContact(const ReflectPassListener *key) {
    auto it = mSunContacts.find(key);
    if (it == mSunContacts.end() || !it->second.finishPending) return;
    SunContactView &sv = it->second;
    sv.finishPending = false;
    if (!sv.vis) return;
    // ...AND NOW THE PIXEL SHADER READS IT: ours to order, since the scene
    // pass does not know the texture exists (it arrives through the listener).
    {
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, sv.vis, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        mRs->executeResourceTransition(trans);
    }
    // THE PROPERTY AND THE TEXTURE TOGETHER, PASS-SCOPED: taken away by
    // releaseSunContactBinding when this pass ends.
    FogHlmsListener::setSunContact(sv.sceneMgr, sv.vis, sv.divisor);
    sv.bound = true;
    sv.boundFrame = frameNow();
}

void RayQueryTier::releaseSunContactBinding(const ReflectPassListener *key) {
    auto it = mSunContacts.find(key);
    if (it == mSunContacts.end() || !it->second.sceneMgr) return;
    FogHlmsListener::setSunContact(it->second.sceneMgr, nullptr, 1u);
    it->second.bound = false;
}

void RayQueryTier::suspendPassBindings(const ReflectPassListener *key, const Ogre::SceneManager *sm) {
    SuspendedBindings &s = mSuspended[key];
    s = SuspendedBindings();
    if (sm) {
        s.gatherSm = sm;
        s.gather = FogHlmsListener::probeGather(sm);
        if (s.gather.irradiance) FogHlmsListener::setProbeGather(sm, FogHlmsListener::ProbeGatherBind());
    }
    auto it = mSunContacts.find(key);
    if (it != mSunContacts.end() && it->second.bound && it->second.sceneMgr) {
        s.sun = true;
        FogHlmsListener::setSunContact(it->second.sceneMgr, nullptr, 1u);
    }
    auto av = mAreaVis.find(key);
    if (av != mAreaVis.end() && av->second.bound && av->second.sceneMgr) {
        s.area = true;
        FogHlmsListener::setAreaVis(av->second.sceneMgr, nullptr);
    }
}

void RayQueryTier::resumePassBindings(const ReflectPassListener *key) {
    auto st = mSuspended.find(key);
    if (st == mSuspended.end()) return;
    const SuspendedBindings s = st->second;
    mSuspended.erase(st);
    if (s.gather.irradiance && s.gatherSm) FogHlmsListener::setProbeGather(s.gatherSm, s.gather);
    auto it = mSunContacts.find(key);
    if (s.sun && it != mSunContacts.end() && it->second.bound && it->second.sceneMgr && it->second.vis)
        FogHlmsListener::setSunContact(it->second.sceneMgr, it->second.vis, it->second.divisor);
    auto av = mAreaVis.find(key);
    if (s.area && av != mAreaVis.end() && av->second.bound && av->second.sceneMgr && av->second.vis)
        FogHlmsListener::setAreaVis(av->second.sceneMgr, av->second.vis);
}

void RayQueryTier::bindCapturePass(const ReflectPassListener *key, OgreView *view,
                                   Ogre::CompositorPass *pass) {
    if (!isOpen() || !view || !pass) return;
    // ONCE A FRAME, at the capture's first prepass-use pass — where the view's own
    // chain finished its reflection: the finished picture, copied over whatever
    // the capture's own screen-space resolve wrote.
    if (view->captureReflectionPending()) {
        view->setCaptureReflectionPending(false);
        Ogre::TextureGpu *dst = nullptr;
        JAH_TRY { dst = pass->getParentNode()->getDefinedTexture(Ogre::IdString("jahSsrReflection")); }
        catch (const Ogre::Exception &) { dst = nullptr; }
        Ogre::TextureGpu *src = view->chainTexture(chain::reflectionTextureName());
        if (src && dst && src != dst && src->getWidth() == dst->getWidth() &&
            src->getHeight() == dst->getHeight() && src->getPixelFormat() == dst->getPixelFormat() &&
            src->getNumSlices() == dst->getNumSlices()) {
            try {
                mRs->endRenderPassDescriptor();
                src->copyTo(dst, dst->getEmptyBox(0), 0, src->getEmptyBox(0), 0);
            } catch (const Ogre::Exception &e) {
                Ogre::LogManager::getSingleton().logMessage(
                    "Jahshaka: the capture's reflection copy failed — " + e.getFullDescription());
            }
        }
    }
    if (mGather) mGather->rebindThisFrame(key);
    auto it = mSunContacts.find(key);
    if (it != mSunContacts.end() && it->second.vis && it->second.sceneMgr &&
        it->second.boundFrame == frameNow())
        FogHlmsListener::setSunContact(it->second.sceneMgr, it->second.vis, it->second.divisor);
    auto av = mAreaVis.find(key);
    if (av != mAreaVis.end() && av->second.vis && av->second.sceneMgr && av->second.boundFrame == frameNow())
        FogHlmsListener::setAreaVis(av->second.sceneMgr, av->second.vis);
}

void RayQueryTier::releaseCapturePass(const ReflectPassListener *key) {
    releaseGatherBinding(key);
    auto it = mSunContacts.find(key);
    if (it != mSunContacts.end() && it->second.sceneMgr)
        FogHlmsListener::setSunContact(it->second.sceneMgr, nullptr, 1u);
    releaseAreaVisBinding(key);
}

void RayQueryTier::sunContactStatsInto(const OgreScene *scene, SunContactStatus &st) const {
    for (const auto &kv : mSunContacts) {
        const SunContactView &sv = kv.second;
        if (sv.scene != scene) continue;
        if (!sv.ran) {
            if (st.reason.empty()) st.reason = sv.reason;
            continue;
        }
        st.running = true;
        st.width = sv.w; st.height = sv.h;
        st.targetW = sv.fullW; st.targetH = sv.fullH;
        st.divisor = sv.divisor;
        st.rays += sv.rays;
        st.range = sv.range;
        for (int i = 0; i < 3; ++i) st.toSun[i] = sv.toSun[i];
        st.gpuMs = std::max(st.gpuMs, monitor::lastGpuMs("sun.contact", kv.first));
        if (sv.cpuMs > st.cpuMs) st.cpuMs = sv.cpuMs;
        st.reason.clear();
    }
    if (!st.running && mSunFailed) st.reason = mSunFailReason;
}

// ---------------------------------------------------------------------------
// THE PIXEL'S AREA LIGHT VISIBILITY — the tier's half (PHOTON-7 stage 7, 7-A; SPECS/PHOTON_7_SPEC.md "AREA LIGHTS, ONE
// LAW ON EVERY PATH" (a)). Ogre-Next draws an LTC area light with no shadow; the cards and the voxels shadowed it. The
// pixel takes the cards' law (jah_area_strata.glsl, one text): rq_area_vis.comp traces one stratum of each of the
// pass's forward LTC lights a half-resolution texel a frame and averages the ratio's two sums over time (with the
// gather's rest law), rq_area_vis_resolve.comp upsamples them joint-bilaterally and takes the ratio into `jahAreaVis`
// (RG32_UINT: light i's diffuse ratio in byte i of x, its specular's in y), which the fork's hook in DoAreaLtcLights
// multiplies light i's LTC diffuse and specular by.
namespace {

/// (The parameter block, AreaVisParams, is both producers' one text: EnginePrivate.h.)
constexpr unsigned kAreaVisBindings = 19u;
constexpr unsigned kAreaResolveBindings = 9u;
/// What the area rays are tested against: every shadow caster, still and moving (the cards' lamps mode's mask).
constexpr unsigned kAreaVisMask = kRayMaskStillCaster | kRayMaskMoverCaster;

/// (THE FORWARD LTC SET, forwardLtcLights, is both producers' one text: OgreAreaVis.cpp.)

}   // namespace

bool RayQueryTier::makeAreaVisPipelines(std::string &err) {
    const auto layoutOf = [&](const VkDescriptorType *types, unsigned count, VkDescriptorSetLayout &out,
                              DescriptorPoolPlan &plan, const char *what) {
        std::vector<VkDescriptorSetLayoutBinding> b(count);
        for (unsigned i = 0; i < count; ++i) {
            b[i] = VkDescriptorSetLayoutBinding{};
            b[i].binding = i;
            b[i].descriptorType = types[i];
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        return plan.makeLayout(mVk, b.data(), count, kMaxTimedScenes * kReflectRing, out, err, what);
    };
    const auto pipelineOf = [&](VkDescriptorSetLayout set, const uint32_t *spv, size_t bytes, VkPipelineLayout &pl,
                                VkShaderModule &module, VkPipeline &pipeline, const char *what) {
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set;
        if (vkCreatePipelineLayout(mVk, &pli, nullptr, &pl) != VK_SUCCESS) {
            err = "vkCreatePipelineLayout failed";
            return false;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = bytes;
        smi.pCode = spv;
        if (vkCreateShaderModule(mVk, &smi, nullptr, &module) != VK_SUCCESS) {
            err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
            return false;
        }
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = pl;
        if (createComputePipeline(cpi, &pipeline, what) != VK_SUCCESS) {
            err = "vkCreateComputePipelines failed";
            return false;
        }
        return true;
    };
    DescriptorPoolPlan plan;
    const VkDescriptorType traceTypes[kAreaVisBindings] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0 tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1 params
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 2 gBufNormals
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3 sceneDepth
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 4 histS prev
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 5 histU prev
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 6 histG prev
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 7 histS cur
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 8 histU cur
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 9 histG cur
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 10 restS
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 11 restU
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 12 histS2 prev (the specular set)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 13 histU2 prev
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 14 histS2 cur
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 15 histU2 cur
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 16 restS2
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 17 restU2
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 18 the prepass's shadow / roughness
    };
    const VkDescriptorType resolveTypes[kAreaResolveBindings] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 0 params
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 1 sceneDepth
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 2 histS (this frame's)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 3 histU
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 4 histG
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 5 areaVis
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 6 gBufNormals (the pixel's plane)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 7 histS2 (the specular set)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 8 histU2
    };
    if (!layoutOf(traceTypes, kAreaVisBindings, mAreaSetLayout, plan, "rayquery/area-vis") ||
        !layoutOf(resolveTypes, kAreaResolveBindings, mAreaResolveSetLayout, plan, "rayquery/area-vis-resolve"))
        return false;
    if (!pipelineOf(mAreaSetLayout, krq_areaVisSpv, sizeof(krq_areaVisSpv), mAreaPipeLayout, mAreaModule,
                    mAreaPipeline, "area visibility") ||
        !pipelineOf(mAreaResolveSetLayout, krq_areaVisResolveSpv, sizeof(krq_areaVisResolveSpv), mAreaResolvePipeLayout,
                    mAreaResolveModule, mAreaResolvePipeline, "area visibility resolve"))
        return false;
    if (!plan.create(mVk, "rayquery/area-vis", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mAreaPool, err))
        return false;
    // (the sun contact's POINT sampler reads the depth and the normal at exactly one texel: made by its pipeline)
    if (!mSunSampler) {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(mVk, &si, nullptr, &mSunSampler) != VK_SUCCESS) {
            err = "vkCreateSampler failed";
            return false;
        }
    }
    return true;
}

void RayQueryTier::dropAreaVis(AreaVisView &av) {
    // THE REGISTRATION FIRST: the listener holds a raw pointer to the texture about to be retired.
    if (av.sceneMgr) FogHlmsListener::setAreaVis(av.sceneMgr, nullptr);
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(av.traceSets[i], mAreaPool);
        retireSet(av.resolveSets[i], mAreaPool);
        av.traceSets[i] = av.resolveSets[i] = VK_NULL_HANDLE;
        retire(av.params[i]);
    }
    for (unsigned k = 0; k < 2u; ++k) {
        retireImage(av.histS[k]);
        retireImage(av.histU[k]);
        retireImage(av.histS2[k]);
        retireImage(av.histU2[k]);
        retireImage(av.histG[k]);
    }
    retireImage(av.restS);
    retireImage(av.restU);
    retireImage(av.restS2);
    retireImage(av.restU2);
    for (unsigned k = 0; k < kAreaVisReadbacks; ++k) {
        retire(av.readback[k]);
        av.readbackLive[k] = false;
    }
    retireTexture(av.vis);
    av.w = av.h = 0;
    av.st.age = 0;
    av.bound = false;
}

void RayQueryTier::forgetAreaVis(const ReflectPassListener *key) {
    auto it = mAreaVis.find(key);
    if (it == mAreaVis.end()) return;
    dropAreaVis(it->second);
    mAreaVis.erase(it);
}

void RayQueryTier::finishAreaVis(const ReflectPassListener *key) {
    auto it = mAreaVis.find(key);
    if (it == mAreaVis.end() || !it->second.finishPending) return;
    AreaVisView &av = it->second;
    av.finishPending = false;
    if (!av.vis) return;
    {
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, av.vis, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        mRs->executeResourceTransition(trans);
    }
    // THE PROPERTY AND THE TEXTURE TOGETHER, PASS-SCOPED: taken away by releaseAreaVisBinding when the pass ends.
    FogHlmsListener::setAreaVis(av.sceneMgr, av.vis);
    av.bound = true;
    av.boundFrame = frameNow();
}

void RayQueryTier::releaseAreaVisBinding(const ReflectPassListener *key) {
    auto it = mAreaVis.find(key);
    if (it == mAreaVis.end() || !it->second.sceneMgr) return;
    FogHlmsListener::setAreaVis(it->second.sceneMgr, nullptr);
    it->second.bound = false;
}

void RayQueryTier::areaVisStatsInto(const OgreScene *scene, AreaVisStatus &st) const {
    for (const auto &kv : mAreaVis) {
        const AreaVisView &av = kv.second;
        if (av.scene != scene) continue;
        if (!av.ran || av.ranDraw + 1u < scene->drawSerial()) {
            if (av.ran) continue;   // a view the tier stopped recording: not this scene's producer now
            if (st.reason.empty()) st.reason = av.reason;
            continue;
        }
        st.running = true;
        st.lights = av.st.lights;
        st.strata = av.st.strata;
        st.traceW = av.w;
        st.traceH = av.h;
        st.targetW = av.fullW;
        st.targetH = av.fullH;
        st.restFrames = av.st.restFrames;
        st.held = av.st.held;
        st.rays += av.rays;
        st.vramBytes += av.vramBytes;
        st.traceMs = std::max(st.traceMs, monitor::lastGpuMs("area.vis", kv.first));
        st.resolveMs = std::max(st.resolveMs, monitor::lastGpuMs("area.resolve", kv.first));
        if (av.cpuMs > st.cpuMs) st.cpuMs = av.cpuMs;
        if (!av.visHost.empty()) {
            st.vis = av.visHost;
            st.visW = av.visHostW;
            st.visH = av.visHostH;
        }
        st.reason.clear();
    }
    if (!st.running && mAreaFailed) st.reason = mAreaFailReason;
}

void rayTierAreaVisStatsInto(const OgreEngine *engine, const OgreScene *scene, AreaVisStatus &st) {
    if (engine && engine->mRayTier) engine->mRayTier->areaVisStatsInto(scene, st);
}



void RayQueryTier::recordAreaVis(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass) {
    if (!isOpen() || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;
    // ...AND NEVER FOR A CHAIN OF THE NO-RAY ROUTE (ChainDesc::screenIntegrate: a chain that does not gather - a Live
    // or undeclared offscreen view of a ray scene - whose ScreenGiListener traces the area visibility through the
    // voxel chain): ONE PRODUCER A VIEW (the 7-5 recordGather rule). Recorded here too, both traced and registered
    // every frame for the one view, the passes read whichever registered last (measured, gi.screen_bind (4): twice
    // the producer's residency).
    if (view->chainDesc().screenIntegrate) {
        forgetAreaVis(key);
        return;
    }
    // THE FORWARD LTC SET - none: OFF IS FREE AND CLEAN (the view gives its images back and binds nothing)
    const std::vector<const Ogre::Light *> lights = forwardLtcLights(scene->sceneManager());
    if (lights.empty() || !scene->rayTracingResolved()) {
        forgetAreaVis(key);
        return;
    }
    const auto cpuStart = Clock::now();
    AreaVisView &av = mAreaVis[key];
    av.scene = scene;
    av.sceneMgr = scene->sceneManager();
    av.ran = false;
    av.rays = 0;
    const auto decline = [&av](const char *why) { av.reason = why; };
    SceneAs *saP = asOf(scene);
    if (!saP) { decline("the scene has no ray structures yet"); return; }
    SceneAs &sa = *saP;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) {
        decline("the scene's top-level structure is empty");
        return;
    }
    // MONO ONLY (the sun contact's rule): a two-eye target is never traced as one image - its area lights unshadowed.
    if (view->stereo()) { decline("a stereo view (mono only: a leftover)"); return; }
    if (mAreaFailed) { decline("the area visibility pipelines are unavailable on this device"); return; }
    if (!mAreaPipeline) {
        std::string err;
        if (!makeAreaVisPipelines(err)) {
            mAreaFailed = true;
            mAreaFailReason = "area visibility off: " + err;
            Ogre::LogManager::getSingleton().logMessage("Jahshaka: " + mAreaFailReason + " (area lights unshadowed)");
            decline("the area visibility pipelines are unavailable on this device");
            return;
        }
    }
    // ---- THE CHAIN'S TEXTURES (the sun contact's two) ---------------------
    Ogre::TextureGpu *normalTex = nullptr, *depthTex = nullptr, *roughTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        depthTex = node->getDefinedTexture(Ogre::IdString("jahDepth"));
        roughTex = node->getDefinedTexture(Ogre::IdString("jahSsrShadowRough"));
    } catch (Ogre::Exception &) { decline("the chain carries no prepass"); return; }
    if (!normalTex || !depthTex || !roughTex) { decline("the chain carries no prepass"); return; }
    const unsigned fullW = depthTex->getWidth(), fullH = depthTex->getHeight();
    if (!fullW || !fullH) return;
    const unsigned divisor = kAreaVisDivisor;
    const unsigned w = (fullW + divisor - 1u) / divisor, h = (fullH + divisor - 1u) / divisor;

    // ---- THE IMAGES, remade on a resize -----------------------------------
    std::string err;
    if (!av.vis || av.w != w || av.h != h || av.fullW != fullW || av.fullH != fullH) {
        dropAreaVis(av);
        for (unsigned k = 0; k < 2u; ++k)
            if (!makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.histS[k], err) ||
                !makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.histU[k], err) ||
                !makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.histS2[k], err) ||
                !makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.histU2[k], err) ||
                !makeStorageImage(w, h, VK_FORMAT_R32G32B32A32_UINT, av.histG[k], err)) {
                decline("the history images could not be made");
                return;
            }
        if (!makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.restS, err) ||
            !makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.restU, err) ||
            !makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.restS2, err) ||
            !makeStorageImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT, av.restU2, err)) {
            decline("the rest images could not be made");
            return;
        }
        Ogre::TextureGpuManager *tm = mRs->getTextureGpuManager();
        static unsigned sSerial = 0u;
        Ogre::TextureGpu *t = tm->createTexture("JahAreaVis/" + std::to_string(++sSerial), Ogre::GpuPageOutStrategy::Discard,
                                                Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
        t->setResolution(fullW, fullH, 1u);
        t->setPixelFormat(Ogre::PFG_RG32_UINT);   // x the diffuse ratios, y the specular (four unorm8 each)
        t->setNumMipmaps(1u);
        t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        av.vis = t;
        av.w = w;
        av.h = h;
        av.fullW = fullW;
        av.fullH = fullH;
        av.divisor = divisor;
        av.imagesNeedClear = true;
        av.st.age = 0;
        av.st.restFrames = 0;
        // the two sets' sums' pairs (8 x 8 B), the surface words' pair (2 x 16 B), the rest means (4 x 8 B), the product
        // (8 B a pixel)
        av.vramBytes = (unsigned long long)w * h * (8u * 8u + 2u * 16u + 4u * 8u) + (unsigned long long)fullW * fullH * 8u;
    }

    // ---- THE FRAME: the lights in HlmsPbs's order, the camera, the rest (both producers' one text) ----
    AreaVisParams pp{};
    EyeBasisF eye;
    const unsigned n = std::max(giQualityFacts(scene->giParams().quality).cardAreaStrata, 1u);
    // THE CLEARANCE: the ray origin's one law, as the sun contact takes it (rq_area_vis.comp's jahAreaLiftRays): the
    // drawn surface's footprints (knobs.x) and the depth-rebuilt point's sub-pixel snap (knobs.y)
    areaVisFrame(av.st, pp, lights, cam, view->chainDesc(), w, h, fullW, fullH, n, gatherRestKeyArmed(scene),
                 av.imagesNeedClear, drawnFootprintsOf(scene, view->chainDesc().lodHysteresis), pointSnapKnob(),
                 float(kAreaVisMask), eye);
    const bool readback = arm(ArmId::AreaVisReadback, false);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!av.st.held) {
        const unsigned ring = av.st.frame % kReflectRing;
        if (!av.params[ring].buffer &&
            !makeBuffer(sizeof(AreaVisParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, av.params[ring], err)) {
            decline("the parameter buffer could not be made");
            return;
        }
        for (VkDescriptorSet *set : { &av.traceSets[ring], &av.resolveSets[ring] }) {
            if (*set) continue;
            VkDescriptorSetAllocateInfo dai{};
            dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            dai.descriptorPool = mAreaPool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = set == &av.traceSets[ring] ? &mAreaSetLayout : &mAreaResolveSetLayout;
            if (vkAllocateDescriptorSets(mVk, &dai, set) != VK_SUCCESS) {
                *set = VK_NULL_HANDLE;
                decline("no descriptor set");
                return;
            }
        }
        {
            const uint64_t a = alphaTableOf(scene);
            const uint32_t lo = uint32_t(a & 0xFFFFFFFFu), hi = uint32_t(a >> 32u);
            std::memcpy(&pp.alpha[0], &lo, sizeof(lo));
            std::memcpy(&pp.alpha[1], &hi, sizeof(hi));
        }
        memcpy(av.params[ring].mapped, &pp, sizeof(pp));

        // ---- THE DESCRIPTOR SETS ---------------------------------------------
        const auto sampledView = [this](Ogre::TextureGpu *t) {
            Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
            slot.texture = t;
            VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
            retireView(v);
            return v;
        };
        VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
        asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asWrite.accelerationStructureCount = 1;
        asWrite.pAccelerationStructures = &sa.tlas;
        VkDescriptorBufferInfo ub{};
        ub.buffer = av.params[ring].buffer;
        ub.range = sizeof(AreaVisParams);
        VkDescriptorImageInfo normals{}, depth{}, store{}, rough{};
        normals.sampler = mSunSampler;
        normals.imageView = sampledView(normalTex);
        normals.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rough.sampler = mSunSampler;
        rough.imageView = sampledView(roughTex);
        rough.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depth.sampler = mSunSampler;
        depth.imageView = sampledView(depthTex);
        depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        {
            Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
            slot.texture = av.vis;
            slot.access = Ogre::ResourceAccess::Write;
            slot.pixelFormat = av.vis->getPixelFormat();
            store.imageView = static_cast<Ogre::VulkanTextureGpu *>(av.vis)->createView(slot, false);
            retireView(store.imageView);
            store.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        if (!normals.imageView || !depth.imageView || !store.imageView || !rough.imageView) {
            decline("an image view could not be made");
            return;
        }
        const unsigned cur = av.st.flip & 1u, prev = cur ^ 1u;
        const auto storage = [](const ReflectImage &img) {
            VkDescriptorImageInfo i{};
            i.imageView = img.view;
            i.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            return i;
        };
        const VkDescriptorImageInfo traceImgs[14] = { storage(av.histS[prev]),  storage(av.histU[prev]),
                                                      storage(av.histG[prev]),  storage(av.histS[cur]),
                                                      storage(av.histU[cur]),   storage(av.histG[cur]),
                                                      storage(av.restS),        storage(av.restU),
                                                      storage(av.histS2[prev]), storage(av.histU2[prev]),
                                                      storage(av.histS2[cur]),  storage(av.histU2[cur]),
                                                      storage(av.restS2),       storage(av.restU2) };
        VkWriteDescriptorSet tw[kAreaVisBindings] = {};
        for (unsigned i = 0; i < kAreaVisBindings; ++i) {
            tw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            tw[i].dstSet = av.traceSets[ring];
            tw[i].dstBinding = i;
            tw[i].descriptorCount = 1;
        }
        tw[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        tw[0].pNext = &asWrite;
        tw[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        tw[1].pBufferInfo = &ub;
        tw[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tw[2].pImageInfo = &normals;
        tw[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tw[3].pImageInfo = &depth;
        for (unsigned i = 0; i < 14u; ++i) {
            tw[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            tw[4 + i].pImageInfo = &traceImgs[i];
        }
        tw[18].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tw[18].pImageInfo = &rough;
        vkUpdateDescriptorSets(mVk, kAreaVisBindings, tw, 0, nullptr);
        const VkDescriptorImageInfo resolveImgs[5] = { storage(av.histS[cur]), storage(av.histU[cur]),
                                                       storage(av.histG[cur]), storage(av.histS2[cur]),
                                                       storage(av.histU2[cur]) };
        VkWriteDescriptorSet rw[kAreaResolveBindings] = {};
        for (unsigned i = 0; i < kAreaResolveBindings; ++i) {
            rw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            rw[i].dstSet = av.resolveSets[ring];
            rw[i].dstBinding = i;
            rw[i].descriptorCount = 1;
        }
        rw[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        rw[0].pBufferInfo = &ub;
        rw[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        rw[1].pImageInfo = &depth;
        for (unsigned i = 0; i < 3u; ++i) {
            rw[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            rw[2 + i].pImageInfo = &resolveImgs[i];
        }
        rw[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        rw[5].pImageInfo = &store;
        rw[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        rw[6].pImageInfo = &normals;
        for (unsigned i = 0; i < 2u; ++i) {
            rw[7 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            rw[7 + i].pImageInfo = &resolveImgs[3 + i];
        }
        vkUpdateDescriptorSets(mVk, kAreaResolveBindings, rw, 0, nullptr);

        // ---- THE LAYOUTS, through Ogre's own solver, before the command buffer is taken -------------------
        {
            const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
            Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
            Ogre::ResourceTransitionArray trans;
            solver.resolveTransition(trans, av.vis, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Write,
                                     computeStage);
            for (Ogre::TextureGpu *t : { normalTex, depthTex, roughTex })
                solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                         computeStage);
            mRs->executeResourceTransition(trans);
        }
        cmd = frameCmd();
        if (!cmd) { decline("no command buffer"); return; }
        // THE RAW IMAGES' FIRST USE: UNDEFINED -> GENERAL and zeroed (a zero surface word is "no history")
        if (av.imagesNeedClear) {
            av.imagesNeedClear = false;
            constexpr int kImgs = 14;
            const VkImage imgs[kImgs] = { av.histS[0].image,  av.histS[1].image,  av.histU[0].image,  av.histU[1].image,
                                          av.histS2[0].image, av.histS2[1].image, av.histU2[0].image, av.histU2[1].image,
                                          av.histG[0].image,  av.histG[1].image,  av.restS.image,     av.restU.image,
                                          av.restS2.image,    av.restU2.image };
            VkImageMemoryBarrier b[kImgs] = {};
            for (int i = 0; i < kImgs; ++i) {
                b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b[i].image = imgs[i];
                b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                b[i].subresourceRange.levelCount = 1;
                b[i].subresourceRange.layerCount = 1;
                b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            }
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                 0, nullptr, kImgs, b);
            VkClearColorValue zero{};
            for (int i = 0; i < kImgs; ++i)
                vkCmdClearColorImage(cmd, imgs[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b[i].subresourceRange);
            VkMemoryBarrier m{};
            m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &m, 0,
                                 nullptr, 0, nullptr);
        }
        // ---- THE TWO DISPATCHES -------------------------------------------------
        {
            detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "area.vis", mRs, key);
            work.setUnits(w * h * av.st.lights / 1000u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAreaPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAreaPipeLayout, 0, 1, &av.traceSets[ring], 0,
                                    nullptr);
            JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
            vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1u);
        }
        {
            VkMemoryBarrier m{};
            m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &m,
                                 0, nullptr, 0, nullptr);
        }
        {
            detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "area.resolve", mRs, key);
            work.setUnits(fullW * fullH / 1000u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAreaResolvePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mAreaResolvePipeLayout, 0, 1,
                                    &av.resolveSets[ring], 0, nullptr);
            JAH_GPU_CHECKPOINT(cmd);
            vkCmdDispatch(cmd, (fullW + 7u) / 8u, (fullH + 7u) / 8u, 1u);
        }
        areaVisAdvance(av.st, eye);
        av.rays = (unsigned long long)w * h * av.st.lights;
    }
    // ---- THE READBACK (a test door): the product into this frame's host slot, the slot written a ring ago decoded
    if (readback) {
        const uint32_t nowF = frameNow();
        const unsigned slot = nowF % kAreaVisReadbacks;
        const unsigned old = (nowF + 1u) % kAreaVisReadbacks;   // written kAreaVisReadbacks - 1 frames ago: retired
        if (av.readbackLive[old] && av.readback[old].mapped) {
            av.visHost.assign(static_cast<const unsigned char *>(av.readback[old].mapped),
                              static_cast<const unsigned char *>(av.readback[old].mapped) + size_t(fullW) * fullH * 8u);
            av.visHostW = fullW;
            av.visHostH = fullH;
        }
        if (!av.readback[slot].buffer &&
            !makeBuffer(VkDeviceSize(fullW) * fullH * 8u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false,
                        av.readback[slot], err))
            av.readback[slot] = RawBuffer();
        if (!cmd) cmd = frameCmd();
        if (cmd && av.readback[slot].buffer) {
            {
                Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
                Ogre::ResourceTransitionArray trans;
                solver.resolveTransition(trans, av.vis, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Read,
                                         1u << Ogre::GPT_COMPUTE_PROGRAM);
                mRs->executeResourceTransition(trans);
            }
            cmd = frameCmd();
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { fullW, fullH, 1u };
            vkCmdCopyImageToBuffer(cmd, static_cast<Ogre::VulkanTextureGpu *>(av.vis)->getFinalTextureName(),
                                   VK_IMAGE_LAYOUT_GENERAL, av.readback[slot].buffer, 1, &region);
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, nullptr,
                                 0, nullptr);
            av.readbackLive[slot] = true;
            av.readbackFrame[slot] = nowF;
        }
    }
    // THE REGISTRATION IS THE SECOND HALF'S (finishAreaVis, in front of the opaque pass).
    av.finishPending = true;
    av.ran = true;
    av.ranDraw = av.scene ? av.scene->drawSerial() : 0u;
    av.reason.clear();
    av.cpuMs = float(msSince(cpuStart));
}

void RayQueryTier::recordSunContact(const ReflectPassListener *key, OgreView *view,
                                    Ogre::CompositorPass *pass) {
    if (!isOpen() || !view || !pass) return;
    OgreScene *scene = view->ogreScene();
    Ogre::Camera *cam = view->camera();
    if (!scene || !cam) return;
    if (!scene->sunContactWanted()) {
        // OFF IS FREE AND CLEAN: a view that was casting and stopped gives its
        // texture back and stops binding it (the gather's rule).
        forgetSunContact(key);
        return;
    }
    const auto cpuStart = Clock::now();
    SunContactView &sv = mSunContacts[key];
    sv.scene = scene;
    sv.sceneMgr = scene->sceneManager();
    sv.ran = false;
    sv.rays = 0;
    /// EVERY EARLY RETURN IS A LEGITIMATE "not this frame": nothing is
    /// registered, the property is not set, and the pass shades with the shadow
    /// map alone — the picture without the row. The reason is kept for the
    /// status, because a row that silently does nothing is the worst outcome.
    const auto decline = [&sv](const char *why) { sv.reason = why; };

    SceneAs *saP = asOf(scene);
    if (!saP) { decline("the scene has no ray structures yet"); return; }
    SceneAs &sa = *saP;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) {
        decline("the scene's top-level structure is empty");
        return;
    }
    // NEVER IN VR (the design's VR column): the chain already declines the row
    // for a stereo view (`ChainDesc::sunContact`); this is the job's own copy of
    // the rule, so a two-eye target is never traced as one image.
    if (view->stereo()) { decline("a stereo view (never in VR)"); return; }
    if (mSunFailed) { decline("the contact pipeline is unavailable on this device"); return; }
    if (!mSunPipeline) {
        std::string err;
        if (!makeSunContactPipeline(err)) {
            mSunFailed = true;
            mSunFailReason = "sun contact off: " + err;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: " + mSunFailReason + " (the shadow map renders alone)");
            decline("the contact pipeline is unavailable on this device");
            return;
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: sun contact shadows ON (PHOTON R3) — one ray per texel towards the sun");
    }

    // ---- THE SUN: the pass' first directional SHADOW CASTER ----------------
    // The one the pixel's first-light term belongs to: HlmsPbs writes the
    // directional casters into `light0Buf.lights[]` in the shadow node's order
    // (OgreHlmsPbs.cpp fillBuffersFor), and the prepass' shadow term — the
    // fShadow the answer is folded into — is that light's map. A pass with no
    // shadow node, or a node holding no directional caster, has no sun term to
    // correct (the piece's hlms_num_shadow_map_lights gate says the same).
    //
    // THE LIST IS CURRENT BECAUSE OF THE PREPASS, not because of this pass (audit
    // F6): this runs in passPreExecute, BEFORE the PrePassUse pass' own
    // `shadowNode->_update` (OgreCompositorPassScene.cpp:238 vs :259). It is this
    // frame's list only because the PrePassCreate pass names the SAME shadow
    // node (OgreChain.cpp, SHADOW_NODE_FIRST_ONLY) and ran first. A chain whose
    // prepass stops naming the node would hand this read last frame's casters.
    Ogre::Vector3 toSun = Ogre::Vector3::ZERO;
    {
        const Ogre::CompositorShadowNode *sn =
            static_cast<Ogre::CompositorPassScene *>(pass)->getShadowNode();
        if (sn) {
            for (const Ogre::LightClosest &lc : sn->getShadowCastingLights()) {
                if (lc.light && lc.light->getType() == Ogre::Light::LT_DIRECTIONAL) {
                    toSun = -lc.light->getDerivedDirection();
                    break;
                }
            }
        }
    }
    if (toSun.squaredLength() < 1e-12f) { decline("no directional shadow caster in the pass"); return; }
    toSun.normalise();

    // ---- THE CHAIN'S TEXTURES (the gather's two, by the same names) --------
    Ogre::TextureGpu *normalTex = nullptr, *depthTex = nullptr;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return;
    try {
        normalTex = node->getDefinedTexture(Ogre::IdString("jahGBufNormals"));
        depthTex = node->getDefinedTexture(Ogre::IdString("jahDepth"));
    } catch (Ogre::Exception &) { decline("the chain carries no prepass"); return; }
    if (!normalTex || !depthTex) { decline("the chain carries no prepass"); return; }
    const unsigned fullW = depthTex->getWidth(), fullH = depthTex->getHeight();
    if (!fullW || !fullH) return;

    // ---- THE RESOLUTION: the row, or the tier's (half at Low and Medium) ----
    const SunContactDesc &row = scene->sunContact();
    unsigned divisor = 1u;
    switch (row.resolution) {
    case SunContactResolution::Full: divisor = 1u; break;
    case SunContactResolution::Half: divisor = 2u; break;
    case SunContactResolution::Auto:
    default: divisor = scene->giParams().quality >= GiQuality::High ? 1u : 2u; break;
    }
    // CEILING, so every pixel's `iFragCoord / divisor` lands inside the texture.
    const unsigned w = (fullW + divisor - 1u) / divisor, h = (fullH + divisor - 1u) / divisor;

    // ---- THE TEXTURE, resident for good and remade only on a resize --------
    if (!sv.vis || sv.w != w || sv.h != h) {
        FogHlmsListener::setSunContact(sv.sceneMgr, nullptr, 1u);
        retireTexture(sv.vis);
        // `Uav` and nothing else — the gather's irradiance target's reasoning:
        // born in GENERAL with the pin's own barrier, SAMPLED because it is a
        // texture, never Reinterpretable.
        Ogre::TextureGpuManager *tm = mRs->getTextureGpuManager();
        static unsigned sSerial = 0u;
        Ogre::TextureGpu *t = tm->createTexture("JahSunContact/" + std::to_string(++sSerial),
                                                Ogre::GpuPageOutStrategy::Discard,
                                                Ogre::TextureFlags::Uav, Ogre::TextureTypes::Type2D);
        t->setResolution(w, h, 1u);
        t->setPixelFormat(Ogre::PFG_R8_UNORM);
        t->setNumMipmaps(1u);
        t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
        sv.vis = t;
        sv.w = w;
        sv.h = h;
    }
    sv.fullW = fullW;
    sv.fullH = fullH;
    sv.divisor = divisor;


    std::string err;
    const unsigned ring = sv.frame % kReflectRing;
    if (!sv.params[ring].buffer &&
        !makeBuffer(sizeof(SunContactParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    sv.params[ring], err)) {
        decline("the parameter buffer could not be made");
        return;
    }
    if (!sv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mSunPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mSunSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &sv.sets[ring]) != VK_SUCCESS) {
            sv.sets[ring] = VK_NULL_HANDLE;
            decline("no descriptor set");
            return;
        }
    }

    // ---- THE PARAMETERS: the ray tier's eye basis, letterbox folded in -----
    SunContactParams pp{};
    const bool ortho = cam->getProjectionType() == Ogre::PT_ORTHOGRAPHIC;
    const Ogre::Quaternion q = cam->getDerivedOrientation();
    Ogre::Real fl = 0, fr = 0, ft = 0, fb = 0;
    cam->getFrustumExtents(fl, fr, ft, fb,
                           ortho ? Ogre::FET_PROJ_PLANE_POS : Ogre::FET_TAN_HALF_ANGLES);
    EyeBasisF eye = eyeBasis(ortho, cam->getDerivedPosition(), q, float(fl), float(fr),
                             float(ft), float(fb));
    expandEyeToTarget(eye, view->chainDesc(), fullW, fullH);
    memcpy(pp.camPos, eye.camPos, sizeof(pp.camPos));
    memcpy(pp.rayTL, eye.rayTL, sizeof(pp.rayTL));
    memcpy(pp.rayRight, eye.rayRight, sizeof(pp.rayRight));
    memcpy(pp.rayDown, eye.rayDown, sizeof(pp.rayDown));
    memcpy(pp.fwd, eye.fwd, sizeof(pp.fwd));
    const Ogre::Vector2 projAB = cam->getProjectionParamsAB();
    pp.projParams[0] = projAB.x;
    pp.projParams[1] = projAB.y;
    pp.projParams[2] = cam->getFarClipDistance();
    pp.projParams[3] = kSunContactFadeFraction;
    put3(pp.viewAxisX, q * Ogre::Vector3::UNIT_X, 0.0f);
    put3(pp.viewAxisY, q * Ogre::Vector3::UNIT_Y, 0.0f);
    put3(pp.viewAxisZ, -(q * Ogre::Vector3::NEGATIVE_UNIT_Z), 0.0f);   // view space looks down -Z
    const float range = std::min(std::max(row.range, kSunContactMinRange), kSunContactMaxRange);
    put3(pp.toSun, toSun, range);
    pp.resolution[0] = float(w);
    pp.resolution[1] = float(h);
    pp.resolution[2] = float(fullW);
    pp.resolution[3] = float(fullH);
    // THE ORIGIN LAW's drawn term (PHOTON-7 stage 6; READ_A W1, READ_B F3 - the one law, Types.h): the view's pixel
    // tolerance x the LOD bias over this view's switch band, x the bake's drift ratio, in sample footprints;
    // rq_sun_contact.comp turns it into metres at the point's Euclidean distance (jah_ray_origin.glsl), adds the
    // point's float bound and lifts along the GEOMETRIC normal. The "+1 footprint" that stood here as the margin for a
    // convex caster's sign (ledger section 1093 F2) is the drift ratio's now: at Epic's 1 px the term is 2.2 footprints
    // on axis, and a start exactly on level 0 is the float bound's. Full resolution whatever the divisor: the error is
    // the view's, in the view's pixels, not this job's texels.
    pp.knobs[0] = drawnFootprintsOf(scene, view->chainDesc().lodHysteresis);
    pp.knobs[1] = pointSnapKnob();   // the ray origin's depth-rebuilt point term (READ_B F5 (b))
    // THE MASK: the shadow CASTERS' near copies (kRayMaskCaster) — a subset of
    // the near field, which is the one field a ray this short lives in (A5b §4:
    // one field per ray; the far copies are the coarse levels past the near
    // length). An object whose shadows are off must cast no contact shadow
    // either.
    pp.knobs[2] = float(kRayMaskCaster);
    pp.knobs[3] = float(divisor);
    {
        const uint64_t a = alphaTableOf(scene);
        const uint32_t lo = uint32_t(a & 0xFFFFFFFFu), hi = uint32_t(a >> 32u);
        std::memcpy(&pp.alpha[0], &lo, sizeof(lo));
        std::memcpy(&pp.alpha[1], &hi, sizeof(hi));
    }
    memcpy(sv.params[ring].mapped, &pp, sizeof(pp));
    sv.range = range;
    sv.toSun[0] = toSun.x; sv.toSun[1] = toSun.y; sv.toSun[2] = toSun.z;

    // ---- THE DESCRIPTOR SET, rewritten every frame (uncached, retired views —
    // the reflection's measured trap about a cached view of a recreated texture)
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo ub{};
    ub.buffer = sv.params[ring].buffer;
    ub.range = sizeof(SunContactParams);
    VkDescriptorImageInfo normals{}, depth{}, store{};
    normals.sampler = mSunSampler;
    normals.imageView = sampledView(normalTex);
    normals.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth.sampler = mSunSampler;
    depth.imageView = sampledView(depthTex);
    depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = sv.vis;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = sv.vis->getPixelFormat();
        store.imageView = static_cast<Ogre::VulkanTextureGpu *>(sv.vis)->createView(slot, false);
        retireView(store.imageView);
        store.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!normals.imageView || !depth.imageView || !store.imageView) {
        decline("an image view could not be made");
        return;
    }
    VkWriteDescriptorSet wds[kSunContactBindings] = {};
    for (unsigned i = 0; i < kSunContactBindings; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = sv.sets[ring];
        wds[i].dstBinding = i;
        wds[i].descriptorCount = 1;
    }
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wds[0].pNext = &asWrite;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wds[1].pBufferInfo = &ub;
    wds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[2].pImageInfo = &normals;
    wds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[3].pImageInfo = &depth;
    wds[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[4].pImageInfo = &store;
    vkUpdateDescriptorSets(mVk, kSunContactBindings, wds, 0, nullptr);

    // ---- THE LAYOUTS, THROUGH OGRE'S OWN SOLVER, before the command buffer
    // is taken (executeResourceTransition closes every encoder and may roll the
    // queue over — the reflection's recorded reason).
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, sv.vis, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::Write, computeStage);
        for (Ogre::TextureGpu *t : { normalTex, depthTex })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }

    // ---- THE DISPATCH -------------------------------------------------------
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) { decline("no command buffer"); return; }
    {
        // The frame monitor's row beside the gather's (`sun.contact`): free
        // while the monitor is off, a GPU-timed row in a capture. `Camera`,
        // not `None`: a view-dependent answer re-made every frame because the
        // thing it describes is the picture.
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "sun.contact", mRs, key);
        work.setUnits(w * h / 1000u);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSunPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mSunPipeLayout, 0, 1,
                                &sv.sets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1u);
    }

    // THE REGISTRATION IS THE SECOND HALF'S (finishSunContact, in front of the
    // opaque pass that reads it — PHOTON-HIT-SHADE-1 split the ray jobs around
    // the hit decode pass).
    sv.finishPending = true;
    sv.ran = true;
    sv.reason.clear();
    sv.rays = (unsigned long long)w * h;
    sv.cpuMs = float(msSince(cpuStart));
    ++sv.frame;
}

// ---------------------------------------------------------------------------
// THE SUN ON THE CARDS (PHOTON-CARDS-4; ATOM-S3-CARDCAP) — the ray tier's half. The
// surface cache selects the cards (OgreSurfaceCache.cpp, "The sun on the cards")
// and hands over their records in the relight's layout; this records one ray
// per texel towards the sun: in the MOVERS' mode against the shadow-casting
// movers' near copies (kRayMaskMoverCaster) into the cache's R8 mover-visibility
// layer, in the STILL mode against the shadow-casting still objects' near copies
// (kRayMaskStillCaster) into the ShadowRough layer's x.
namespace {
struct CardMoverParams {
    float toSun[4] = {};
    float knobs[4] = {};
    uint32_t geom[4] = {};   ///< x = the geometry rows' slot count (the open fraction's buried test, PHOTON-7 11-2 (5))
};
/// ...0-8 the trace's own, 9 / 10 the geometry rows (the buried test's wound normal) and 11 THE OPEN FRACTION
/// (PHOTON-7 11-2 (5), SurfaceCache's open layer: written by the still mode, read by the other two)
constexpr unsigned kCardMoverBindings = 12u;
/// The job's three modes' views (movers, still, lamps), each a ring of sets per scene: the pool holds them all.
constexpr unsigned kCardMoverModes = 3u;
/// The records ring's capacity: the relight list's (kMaxRelights, 80 B a card).
constexpr unsigned kCardMoverMaxRecords = 1024u;
constexpr unsigned kCardMoverRecordFloats = 20u + kCardLampTable;   // the relight's record: five vec4, then the card's
                                                                     // lamp table (48 ids, PHOTON-7 10-4 R9 (3))
}   // namespace

bool RayQueryTier::makeCardMoverPipelineNow(std::string &err) {
    if (!mDev || !mDev->mPhysicalDevice || !sunContactFormatStores(mDev->mPhysicalDevice)) {
        err = "the device cannot store to R8_UNORM (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)";
        return false;
    }
    {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(mDev->mPhysicalDevice, VK_FORMAT_R16G16_UNORM, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            err = "the device cannot store to R16G16_UNORM (the ShadowRough layer's still term)";
            return false;
        }
        vkGetPhysicalDeviceFormatProperties(mDev->mPhysicalDevice, VK_FORMAT_R8G8B8A8_UNORM, &props);
        if (!(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
            err = "the device cannot store to R8G8B8A8_UNORM (the SunVis layer's lamp terms)";
            return false;
        }
    }
    if (!ensureSamplers(err)) return false;
    VkDescriptorSetLayoutBinding b[kCardMoverBindings] = {};
    const VkDescriptorType types[kCardMoverBindings] = {
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,   // 0 tlas
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,               // 1 params
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 2 the card records
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 3 cardDepth
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,       // 4 cardNormal
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 5 moverVis
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 6 shadowRough (still mode; the lamps' roughness)
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 7 the relight's light list (lamps mode)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 8 sunVis (lamps mode: its green)
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 9 the geometry rows' per-slot table
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,               // 10 the geometry rows
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,                // 11 the open fraction (11-2 (5))
    };
    for (unsigned i = 0; i < kCardMoverBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kCardMoverBindings, kCardMoverModes * kMaxTimedScenes * kReflectRing, mCmSetLayout, err,
                         "rayquery/card-movers"))
        return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mCmSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mCmPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_cardMoversSpv);
    smi.pCode = krq_cardMoversSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mCmModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mCmModule;
    cpi.stage.pName = "main";
    cpi.layout = mCmPipeLayout;
    if (createComputePipeline(cpi, &mCmPipeline, "card movers") != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }
    // The pool its layout planned (VkDescriptorPools.h).
    return plan.create(mVk, "rayquery/card-movers", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mCmPool, err);
}

bool RayQueryTier::traceCardMovers(OgreScene *scene, const CardMoverTrace &job) {
    joinAhead(kAheadCardMovers);   // (its members are read below: the ahead build is joined first)
    if (!isOpen() || !scene || !job.count || !job.records || !job.depth || !job.normal || !job.vis ||
        !job.shadowRough || !job.lights || !job.sunVis || !job.open)
        return false;
    Ogre::UavBufferPacked *geomRowsSrc = nullptr;   // (the buried test's rows, when bound: the solver's read below)
    SceneAs *saP = asOf(scene);
    if (!saP) return false;
    SceneAs &sa = *saP;
    if (!sa.tlas || !sa.st.enabled || sa.instanceCount == 0u) return false;
    if (mCmFailed) return false;
    if (!mCmPipeline) {
        std::string err;
        if (!makeCardMoverPipeline(err)) {
            mCmFailed = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: card mover shadows off: " + err + " (the cards hold the still world's sun term alone)");
            return false;
        }
    }
    CardMoverView &cv = job.lamps ? mCardLamps[scene] : job.still ? mCardStill[scene] : mCardMovers[scene];
    const unsigned count = std::min(job.count, kCardMoverMaxRecords);
    std::string err;
    const unsigned ring = cv.frame % kReflectRing;
    if (!cv.params[ring].buffer &&
        !makeBuffer(sizeof(CardMoverParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false,
                    cv.params[ring], err))
        return false;
    const VkDeviceSize recBytes = VkDeviceSize(kCardMoverMaxRecords) * kCardMoverRecordFloats * sizeof(float);
    if (!cv.records[ring].buffer &&
        !makeBuffer(recBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, cv.records[ring], err))
        return false;
    if (!cv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mCmPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mCmSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &cv.sets[ring]) != VK_SUCCESS) {
            cv.sets[ring] = VK_NULL_HANDLE;
            return false;
        }
    }
    CardMoverParams pp{};
    pp.toSun[0] = job.toSun.x; pp.toSun[1] = job.toSun.y; pp.toSun[2] = job.toSun.z;
    pp.toSun[3] = job.range;
    if (job.lamps) pp.toSun[0] = float(std::max(job.areaStrata, 1u));   // the lamps mode: an area light's strata a side
    // THE LAMPS' RAY meets every shadow caster, still and moving: a lamp's shadow is the whole world's.
    pp.knobs[0] = float(job.lamps ? (kRayMaskStillCaster | kRayMaskMoverCaster)
                                  : job.still ? kRayMaskStillCaster : kRayMaskMoverCaster);
    pp.knobs[1] = job.lamps ? 2.0f : job.still ? 1.0f : 0.0f;
    {
        // THE ALPHA TABLE (REFLECT-MOVERS-2) in zw, bit-copied: a card sees a cut-out's holes.
        const uint64_t a = alphaTableOf(scene);
        const uint32_t lo = uint32_t(a & 0xFFFFFFFFu), hi = uint32_t(a >> 32u);
        std::memcpy(&pp.knobs[2], &lo, sizeof(lo));
        std::memcpy(&pp.knobs[3], &hi, sizeof(hi));
    }
    memcpy(cv.params[ring].mapped, &pp, sizeof(pp));
    memcpy(cv.records[ring].mapped, job.records, size_t(count) * kCardMoverRecordFloats * sizeof(float));

    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{};
    asWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &sa.tlas;
    VkDescriptorBufferInfo ub{}, rb{};
    ub.buffer = cv.params[ring].buffer;
    ub.range = sizeof(CardMoverParams);
    rb.buffer = cv.records[ring].buffer;
    rb.range = recBytes;
    VkDescriptorImageInfo depth{}, normal{}, store{}, rough{}, sunVisInfo{};
    VkDescriptorBufferInfo lightsInfo{};
    {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(job.lights->getBufferInterface());
        lightsInfo.buffer = bi->getVboName();
        lightsInfo.offset = VkDeviceSize(job.lights->_getFinalBufferStart()) * job.lights->getBytesPerElement();
        lightsInfo.range = job.lights->getTotalSizeBytes();
    }
    depth.sampler = mPointSampler;
    depth.imageView = sampledView(job.depth);
    depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    normal.sampler = mPointSampler;
    normal.imageView = sampledView(job.normal);
    normal.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.vis;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = job.vis->getPixelFormat();
        store.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.vis)->createView(slot, false);
        retireView(store.imageView);
        store.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.shadowRough;
        slot.access = Ogre::ResourceAccess::ReadWrite;
        slot.pixelFormat = job.shadowRough->getPixelFormat();
        rough.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.shadowRough)->createView(slot, false);
        retireView(rough.imageView);
        rough.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.sunVis;
        slot.access = Ogre::ResourceAccess::ReadWrite;
        slot.pixelFormat = job.sunVis->getPixelFormat();
        sunVisInfo.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.sunVis)->createView(slot, false);
        retireView(sunVisInfo.imageView);
        sunVisInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    // THE OPEN FRACTION (PHOTON-7 11-2 (5)): the still mode writes it, the others read it
    VkDescriptorImageInfo openInfo{};
    if (job.open) {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.open;
        slot.access = job.still ? Ogre::ResourceAccess::ReadWrite : Ogre::ResourceAccess::Read;
        slot.pixelFormat = job.open->getPixelFormat();
        openInfo.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.open)->createView(slot, false);
        retireView(openInfo.imageView);
        openInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    // ...AND THE GEOMETRY ROWS for its buried test (the hit set's two: the per-slot table copied into this ring's
    // buffer, the GPU scene's rows); without them a small zeroed table and slot count 0 - nothing reads as buried
    VkDescriptorBufferInfo rowOfSlotInfo{}, geomRowsInfo{};
    {
        HitSetSources hs;
        hitSetOf(scene, sa, hs);
        const uint32_t slots = hs.geomRows && hs.geomRowOfSlot ? uint32_t(hs.geomRowOfSlot->size()) : 0u;
        RawBuffer &rs = cv.rowOfSlot[ring];
        const VkDeviceSize want = std::max<VkDeviceSize>(VkDeviceSize(slots) * sizeof(uint32_t), 1024u);
        if (rs.buffer && rs.size < want) retire(rs);
        if (!rs.buffer && !makeBuffer(want, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, rs, err)) return false;
        if (slots) std::memcpy(rs.mapped, hs.geomRowOfSlot->data(), size_t(slots) * sizeof(uint32_t));
        else std::memset(rs.mapped, 0xFF, 1024u);
        rowOfSlotInfo.buffer = rs.buffer;
        rowOfSlotInfo.range = VK_WHOLE_SIZE;
        if (slots) {
            auto *bi = static_cast<Ogre::VulkanBufferInterface *>(hs.geomRows->getBufferInterface());
            geomRowsInfo.buffer = bi->getVboName();
            geomRowsInfo.offset = VkDeviceSize(hs.geomRows->_getFinalBufferStart()) * hs.geomRows->getBytesPerElement();
            geomRowsInfo.range = hs.geomRows->getTotalSizeBytes();
        } else {
            geomRowsInfo = rowOfSlotInfo;
        }
        pp.geom[0] = slots;
        memcpy(cv.params[ring].mapped, &pp, sizeof(pp));
        geomRowsSrc = slots ? hs.geomRows : nullptr;
    }
    if (!depth.imageView || !normal.imageView || !store.imageView || !rough.imageView || !sunVisInfo.imageView ||
        !openInfo.imageView)
        return false;
    VkWriteDescriptorSet wds[kCardMoverBindings] = {};
    for (unsigned i = 0; i < kCardMoverBindings; ++i) {
        wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wds[i].dstSet = cv.sets[ring];
        wds[i].dstBinding = i;
        wds[i].descriptorCount = 1;
    }
    wds[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    wds[0].pNext = &asWrite;
    wds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wds[1].pBufferInfo = &ub;
    wds[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[2].pBufferInfo = &rb;
    wds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[3].pImageInfo = &depth;
    wds[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wds[4].pImageInfo = &normal;
    wds[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[5].pImageInfo = &store;
    wds[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[6].pImageInfo = &rough;
    wds[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[7].pBufferInfo = &lightsInfo;
    wds[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[8].pImageInfo = &sunVisInfo;
    wds[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[9].pBufferInfo = &rowOfSlotInfo;
    wds[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wds[10].pBufferInfo = &geomRowsInfo;
    wds[11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    wds[11].pImageInfo = &openInfo;
    vkUpdateDescriptorSets(mVk, kCardMoverBindings, wds, 0, nullptr);

    // THE LAYOUTS through Ogre's own solver, before the command buffer is taken:
    // the capture's copies wrote Depth and Normal this frame; the relight that
    // follows reads the layer as a UAV and its analyzeBarriers orders it after
    // this write (the solver now knows the layer was written in compute).
    {
        // THE CAPTURE'S COPY ENCODER CLOSES FIRST (ATOM-S3-CARDCAP, measured under the
        // validation layer: VUID-VkImageMemoryBarrier-oldLayout-01197 in the selftest).
        // The atlas copies leave Ogre's copy encoder OPEN with the layers in
        // TRANSFER_DST; the solver's barrier below moves them to their read/UAV
        // layouts, and an encoder closed AFTER it would transition them back from
        // TRANSFER_DST — a layout they no longer hold. (The movers' trace had the
        // same order since PHOTON-CARDS-4; the still trace runs every capture frame,
        // which is what surfaced it.)
        mDev->_jahRecordQueue().endAllEncoders();   // the queue frameCmd() records on
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        // Both storage images are bound in either mode (GENERAL); the one the
        // mode writes is resolved as written, the other as read.
        const bool movers = !job.still && !job.lamps;
        solver.resolveTransition(trans, job.vis, Ogre::ResourceLayout::Uav,
                                 movers ? Ogre::ResourceAccess::Write : Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, job.shadowRough, Ogre::ResourceLayout::Uav,
                                 job.still ? Ogre::ResourceAccess::ReadWrite : Ogre::ResourceAccess::Read,
                                 computeStage);
        solver.resolveTransition(trans, job.sunVis, Ogre::ResourceLayout::Uav,
                                 job.lamps ? Ogre::ResourceAccess::ReadWrite : Ogre::ResourceAccess::Read,
                                 computeStage);
        solver.resolveTransition(trans, job.lights, Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, job.open, Ogre::ResourceLayout::Uav,
                                 job.still ? Ogre::ResourceAccess::ReadWrite : Ogre::ResourceAccess::Read, computeStage);
        if (geomRowsSrc) solver.resolveTransition(trans, geomRowsSrc, Ogre::ResourceAccess::Read, computeStage);
        for (Ogre::TextureGpu *t : { job.depth, job.normal })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return false;
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Caster, 0,
                                         job.lamps ? "cards.lamps" : job.still ? "cards.still" : "cards.movers", mRs,
                                         scene);
        work.setUnits(count);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCmPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCmPipeLayout, 0, 1,
                                &cv.sets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, kCardPageSize / 8u, kCardPageSize / 8u, count);
    }
    ++cv.frame;
    return true;
}

void RayQueryTier::cardMoverTimes(OgreScene *scene, float &traceMs, float &stillMs) {
    traceMs = monitor::lastGpuMs("cards.movers", scene);
    stillMs = monitor::lastGpuMs("cards.still", scene);
}

void RayQueryTier::forgetCardMovers(OgreScene *scene) {
    for (auto *views : { &mCardMovers, &mCardStill, &mCardLamps }) {
        auto it = views->find(scene);
        if (it == views->end()) continue;
        for (unsigned i = 0; i < kReflectRing; ++i) {
            retireSet(it->second.sets[i], mCmPool);
            retire(it->second.rowOfSlot[i]);
            retire(it->second.params[i]);
            retire(it->second.records[i]);
        }
        views->erase(it);
    }
    monitor::forgetGpuOwner(scene);
}

bool OgreScene::traceCardMovers(const CardMoverTrace &job) {
    return mEngine && mEngine->mRayTier && mEngine->mRayTier->traceCardMovers(this, job);
}
bool OgreScene::cardTracePresent() const {
    return mEngine && mEngine->mRayTier && mEngine->mRayTier->cardTracePresent();
}
void OgreScene::cardMoverTimes(float &traceMs, float &stillMs) {
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->cardMoverTimes(this, traceMs, stillMs);
}

// ---------------------------------------------------------------------------
// THE CARDS' INDIRECT BY TLAS RAYS (PHOTON-7 stage 10, 10-2a) — the ray tier's half. The surface cache hands over the
// cards its relight re-gathers this frame (OgreSurfaceCache.cpp relightCards) in the relight's record layout; this
// records N fixed rays a texel against the still world (rq_card_gather.comp), the hits read through the hit set, into
// the cache's Gather layer, before the relight that reads it.
namespace {
struct CardGatherParams {
    HitSetParams hit;      ///< THE HIT SET'S UNIFORMS (hitSetParams; rq_card_gather.comp's first member)
    float knobs[4] = {};   ///< the rays a texel, the reach, -, the ray mask
    float knobs2[4] = {};  ///< -, the records
};
/// The job's own six (the params, the records, the Gather layer, the counts; the read lists at 37, the open fraction at
/// 38 - PHOTON-7 11-2 (5)) and the hit set at 0 and 5..36.
constexpr unsigned kCardGatherBindings = 39u;
/// THE READ LISTS' BLOCK (10-2c; rq_card_gather.comp kCardReadsBlock, SurfaceCache::kCardReadsBlock): 1 + 2 x 32 words.
constexpr unsigned kCardGatherReadsBlock = 65u;
constexpr HitSetSlots kCardGatherHitSlots = {
    0u,
    { 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u },
    17u,
    { 18u, 19u, 20u, 21u },
    22u,
    { 29u, 30u },
    { 31u, 32u, 33u, 34u },
    35u,
    36u,
};
static_assert(kCardGatherBindings == kHitSetBindings + 6u, "the cards' gather set is the hit set and its own six");
static_assert(kCardGatherReadsBlock == kCardReadsBlock, "one read-list block");
/// The records ring's capacity: the relight list's (kMaxRelights).
constexpr unsigned kCardGatherMaxRecords = 1024u;
constexpr unsigned kCardGatherRecordFloats = 20u + kCardLampTable;   // (the relight's record, as the movers')
constexpr unsigned kCardGatherCounts = 10u;   // rq_card_gather.comp's kCountWords
/// ...then each record's own sky rays (the e = 0 counter per card), one word a record
constexpr unsigned kCardGatherCountWords = kCardGatherCounts + kCardGatherMaxRecords;
/// THE HIT'S SUN RAY at a voxel-answered hit (jah_rq_hit_direct.glsl): the still casters alone (a cache holds no mover:
/// the mask's law, rq_card_gather.comp's head) to the cards' own sun range (OgreSurfaceCache.cpp kMoverRayRange).
constexpr float kCardGatherSunRange = 10000.0f;
}   // namespace

bool RayQueryTier::makeCardGatherPipelineNow(std::string &err) {
    if (!ensureSamplers(err)) return false;
    VkDescriptorSetLayoutBinding b[kCardGatherBindings] = {};
    hitSetLayout(kCardGatherHitSlots, b);
    const VkDescriptorType own[4] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   // 1 params
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 2 the card records
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    // 3 the Gather layer
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 4 the counts
    };
    for (unsigned i = 0; i < 4u; ++i) {
        b[1u + i].binding = 1u + i;
        b[1u + i].descriptorType = own[i];
        b[1u + i].descriptorCount = 1u;
        b[1u + i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    b[37].binding = 37u;   // the read lists (10-2c)
    b[37].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[37].descriptorCount = 1u;
    b[37].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[38].binding = 38u;   // the open fraction (11-2 (5))
    b[38].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[38].descriptorCount = 1u;
    b[38].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kCardGatherBindings, kMaxTimedScenes * kReflectRing, mCgSetLayout, err,
                         "rayquery/card-gather"))
        return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mCgSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mCgPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_cardGatherSpv);
    smi.pCode = krq_cardGatherSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mCgModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mCgModule;
    cpi.stage.pName = "main";
    cpi.layout = mCgPipeLayout;
    if (createComputePipeline(cpi, &mCgPipeline, "card gather") != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }
    return plan.create(mVk, "rayquery/card-gather", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mCgPool, err);
}

void RayQueryTier::foldCardGatherCounts(CardGatherView &v) {
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (unsigned i = 0; i < kReflectRing; ++i) {
        if (!v.countsPending[i] || !v.counts[i].mapped || uint32_t(now - v.countsFrame[i]) < inFlight) continue;
        const uint32_t *w = static_cast<const uint32_t *>(v.counts[i].mapped);
        v.total.rays += w[0];
        v.total.card += w[1];
        v.total.voxel += w[2];
        v.total.backFaces += w[3];
        v.total.noSample += w[4];
        v.total.sky += w[5];
        v.total.texels += w[6];
        v.total.emptyTexels += w[7];
        v.total.readsOverflowed += w[8];
        v.total.buried += w[9];
        for (size_t k = 0; k < v.countsSlots[i].size(); ++k)
            v.skyDrain.emplace_back(v.countsSlots[i][k], w[kCardGatherCounts + k]);
        if (v.skyDrain.size() > 8u * kCardGatherMaxRecords)   // nobody drains (no cache asking): the newest kept
            v.skyDrain.erase(v.skyDrain.begin(), v.skyDrain.end() - 4u * kCardGatherMaxRecords);
        ++v.total.dispatches;
        v.countsPending[i] = false;
    }
}

CardGatherResult RayQueryTier::gatherCards(OgreScene *scene, const CardGatherTrace &job) {
    joinAhead(kAheadCardGather);   // (its members are read below: the ahead build is joined first)
    // THE TIER: a scene that does not trace (the project's rays off, or no device) gathers by the voxel rays.
    if (!isOpen() || !scene || !scene->rayTracingResolved() || mCgFailed) return CardGatherResult::NoRays;
    if (!job.count || !job.records || !job.gather || !job.reads) return CardGatherResult::Wait;
    // ...and one that traces but whose structure is not there this frame waits (never the voxel rays here).
    SceneAs *saP = asOf(scene);
    if (!saP || !saP->tlas || !saP->st.enabled || saP->instanceCount == 0u) return CardGatherResult::Wait;
    SceneAs &sa = *saP;
    if (!mCgPipeline) {
        std::string err;
        if (!makeCardGatherPipeline(err)) {
            mCgFailed = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: the cards' TLAS gather is off: " + err + " (the cards gather by the voxel rays)");
            return CardGatherResult::NoRays;
        }
    }
    CardGatherView &v = mCardGather[scene];
    foldCardGatherCounts(v);
    const unsigned count = std::min(job.count, kCardGatherMaxRecords);
    const unsigned ring = v.frame % kReflectRing;
    std::string err;
    if (v.countsPending[ring]) return CardGatherResult::Wait;   // its counts not read yet (a frame not retired)
    const VkDeviceSize recBytes = VkDeviceSize(kCardGatherMaxRecords) * kCardGatherRecordFloats * sizeof(float);
    if ((!v.params[ring].buffer &&
         !makeBuffer(sizeof(CardGatherParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, v.params[ring], err)) ||
        (!v.records[ring].buffer &&
         !makeBuffer(recBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, v.records[ring], err)) ||
        (!v.counts[ring].buffer &&
         !makeBuffer(kCardGatherCountWords * sizeof(uint32_t),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false,
                     v.counts[ring], err)))
        return CardGatherResult::Wait;
    if (!v.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mCgPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mCgSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &v.sets[ring]) != VK_SUCCESS) {
            v.sets[ring] = VK_NULL_HANDLE;
            return CardGatherResult::Wait;
        }
    }

    // ---- THE HIT SET (the one collection) with no hit list (a cache ray appends nothing) but the GPU scene's
    // instance table (the hit's flags - the back-face rule's two-sided bit - and its material, jah_rq_hit_direct.glsl).
    HitSetSources hs;
    hitStandIns(hs.hit);
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::UavBufferPacked *instances = gs.live() ? gs.instanceBuffer() : nullptr;
    if (instances) {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(instances->getBufferInterface());
        hs.hit.instances = bi->getVboName();
        hs.hit.instancesOffset = VkDeviceSize(instances->_getFinalBufferStart()) * instances->getBytesPerElement();
        hs.hit.instancesRange = instances->getTotalSizeBytes();
        hs.hit.instanceEntries = gs.slotCount();
    }
    hitSetOf(scene, sa, hs);
    if (!hs.cardsBound()) return CardGatherResult::Wait;   // the texels' own layers are the cache's: bound with it
    // the per-slot geometry rows, this ring's copy
    uint32_t geomSlots = hs.geomRows && hs.geomRowOfSlot ? uint32_t(hs.geomRowOfSlot->size()) : 0u;
    if (geomSlots) {
        RawBuffer &rb = v.rowOfSlot[ring];
        const VkDeviceSize want = VkDeviceSize(geomSlots) * sizeof(uint32_t);
        if (rb.buffer && rb.size < want) retire(rb);
        if (!rb.buffer && !makeBuffer(std::max<VkDeviceSize>(want, 1024u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                      false, rb, err))
            geomSlots = 0u;
        else
            std::memcpy(rb.mapped, hs.geomRowOfSlot->data(), size_t(want));
    }

    // ---- THE PARAMETERS: the hit set's (the one function), then this job's - the hit's own sun rays against EVERY
    // caster, still and moving, to the cards' sun range, no mover gate (rq_card_gather.comp's mask law; PHOTON-7 8A A4:
    // a mover is GI geometry, in the store - the three masks change together)
    CardGatherParams pp{};
    hitSetParams(hs, geomSlots, pp.hit, HitReader::Cache);
    pp.hit.hitSun2[0] = float(kRayMaskStillCaster | kRayMaskMoverCaster);
    pp.hit.hitSun2[2] = kCardGatherSunRange;
    pp.hit.hitSun2[3] = 0.0f;
    pp.knobs[0] = float(std::max(job.rays, 1u));
    pp.knobs[1] = std::max(job.reach, 0.0f);
    pp.knobs[3] = float(kRayMaskStill | kRayMaskMover);   // the world, movers in it (rq_card_gather.comp's mask law)
    pp.knobs2[1] = float(count);
    std::memcpy(v.params[ring].mapped, &pp, sizeof(pp));
    std::memcpy(v.records[ring].mapped, job.records, size_t(count) * kCardGatherRecordFloats * sizeof(float));

    // ---- THE SET: the job's own four, then the hit set through its one writer
    VkWriteDescriptorSet w[kCardGatherBindings] = {};
    HitSetWrite hitSet;
    if (!hitSet.write(*this, v.sets[ring], kCardGatherHitSlots, hs, geomSlots ? v.rowOfSlot[ring].buffer : VK_NULL_HANDLE,
                      w, err))
        return CardGatherResult::Wait;
    VkDescriptorBufferInfo ub{}, rb{}, cb{};
    ub.buffer = v.params[ring].buffer;
    ub.range = sizeof(CardGatherParams);
    rb.buffer = v.records[ring].buffer;
    rb.range = recBytes;
    cb.buffer = v.counts[ring].buffer;
    cb.range = kCardGatherCountWords * sizeof(uint32_t);
    VkDescriptorImageInfo store{};
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.gather;
        slot.access = Ogre::ResourceAccess::ReadWrite;
        slot.pixelFormat = job.gather->getPixelFormat();
        store.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.gather)->createView(slot, false);
        retireView(store.imageView);
        store.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkDescriptorImageInfo openInfo{};
    if (job.open) {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = job.open;
        slot.access = Ogre::ResourceAccess::Read;
        slot.pixelFormat = job.open->getPixelFormat();
        openInfo.imageView = static_cast<Ogre::VulkanTextureGpu *>(job.open)->createView(slot, false);
        retireView(openInfo.imageView);
        openInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    if (!store.imageView || !openInfo.imageView) return CardGatherResult::Wait;
    const auto own = [&](uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet & {
        VkWriteDescriptorSet &e = w[binding];
        e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        e.dstSet = v.sets[ring];
        e.dstBinding = binding;
        e.descriptorCount = 1;
        e.descriptorType = type;
        return e;
    };
    own(1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &ub;
    own(2u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &rb;
    own(3u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &store;
    own(4u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &cb;
    VkDescriptorBufferInfo readsInfo{};
    {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(job.reads->getBufferInterface());
        readsInfo.buffer = bi->getVboName();
        readsInfo.offset = VkDeviceSize(job.reads->_getFinalBufferStart()) * job.reads->getBytesPerElement();
        readsInfo.range = job.reads->getTotalSizeBytes();
    }
    own(37u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &readsInfo;
    own(38u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &openInfo;
    vkUpdateDescriptorSets(mVk, kCardGatherBindings, w, 0, nullptr);
    // THE ZERO BLOCK the read lists' clear copies from (once)
    if (!mCgZeroBlock.buffer) {
        if (!makeBuffer(VkDeviceSize(kCardGatherReadsBlock) * sizeof(uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true,
                        false, mCgZeroBlock, err))
            return CardGatherResult::Wait;
        std::memset(mCgZeroBlock.mapped, 0, size_t(kCardGatherReadsBlock) * sizeof(uint32_t));
    }

    // ---- THE LAYOUTS through Ogre's solver, before the command buffer is taken (the movers job's rule: the capture's
    // copy encoder closes first): the hit set read, the Gather layer written - the relight that follows reads it as a
    // UAV, ordered after this write by its own barrier analysis.
    {
        mDev->_jahRecordQueue().endAllEncoders();
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        hitSetTransitions(solver, trans, hs);
        solver.resolveTransition(trans, job.gather, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::ReadWrite,
                                 computeStage);
        if (instances) solver.resolveTransition(trans, instances, Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, job.reads, Ogre::ResourceAccess::ReadWrite, computeStage);
        solver.resolveTransition(trans, job.open, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return CardGatherResult::Wait;
    clearDummyImages(cmd);
    initHitDummies(cmd);
    {
        // THE READ LISTS of this dispatch's cards cleared (10-2c: a block is the card's last gather's), in one copy of the
        // zero block to every card's slot - after every earlier reader and writer of the lists (last frame's
        // accumulation, its gather)
        VkMemoryBarrier pre{};
        pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        pre.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        pre.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0,
                             nullptr, 0, nullptr);
        std::vector<VkBufferCopy> regions(count);
        const float *rec = job.records;
        for (unsigned k = 0; k < count; ++k) {
            const unsigned ax = unsigned(rec[k * kCardGatherRecordFloats + 0u]);
            const unsigned ay = unsigned(rec[k * kCardGatherRecordFloats + 1u]);
            const unsigned slot = (ay >> 4u) * 128u + (ax >> 4u);
            regions[k].srcOffset = 0u;
            regions[k].dstOffset = readsInfo.offset + VkDeviceSize(slot) * kCardGatherReadsBlock * sizeof(uint32_t);
            regions[k].size = VkDeviceSize(kCardGatherReadsBlock) * sizeof(uint32_t);
        }
        vkCmdCopyBuffer(cmd, mCgZeroBlock.buffer, readsInfo.buffer, count, regions.data());
        // THE COUNTS, zeroed for this dispatch (every earlier reader of the slot retired: countsPending)
        vkCmdFillBuffer(cmd, v.counts[ring].buffer, 0, kCardGatherCountWords * sizeof(uint32_t), 0u);
        VkMemoryBarrier fb{};
        fb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &fb, 0,
                             nullptr, 0, nullptr);
    }
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Sweep, 0, "cards.gather", mRs, scene);
        work.setUnits(count);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCgPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCgPipeLayout, 0, 1, &v.sets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, kCardPageSize / 8u, kCardPageSize / 8u, count);
    }
    {
        // ...and the counts visible to the host once the frame retires (foldCardGatherCounts reads them then)
        VkMemoryBarrier hb{};
        hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0,
                             nullptr, 0, nullptr);
    }
    v.countsFrame[ring] = frameNow();
    v.countsPending[ring] = true;
    v.countsSlots[ring].resize(count);
    for (unsigned k = 0; k < count; ++k)
        v.countsSlots[ring][k] = (unsigned(job.records[k * kCardGatherRecordFloats + 1u]) >> 4u) * 128u +
                                 (unsigned(job.records[k * kCardGatherRecordFloats + 0u]) >> 4u);
    ++v.frame;
    return CardGatherResult::Traced;
}

void RayQueryTier::cardGatherCounts(OgreScene *scene, CardCacheStatus::GatherCounts &out) {
    out = CardCacheStatus::GatherCounts();
    auto it = mCardGather.find(scene);
    if (it == mCardGather.end()) return;
    foldCardGatherCounts(it->second);
    out = it->second.total;
}

void RayQueryTier::cardGatherSky(OgreScene *scene, std::vector<std::pair<unsigned, unsigned>> &out) {
    out.clear();
    auto it = mCardGather.find(scene);
    if (it == mCardGather.end()) return;
    foldCardGatherCounts(it->second);
    out.swap(it->second.skyDrain);
}

void RayQueryTier::forgetCardGather(OgreScene *scene) {
    auto it = mCardGather.find(scene);
    if (it == mCardGather.end()) return;
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(it->second.sets[i], mCgPool);
        retire(it->second.params[i]);
        retire(it->second.records[i]);
        retire(it->second.rowOfSlot[i]);
        retire(it->second.counts[i]);
    }
    mCardGather.erase(it);
}

// ---------------------------------------------------------------------------
// THE VOXEL SIDES' GATHER BY TLAS RAYS (PHOTON-7 stage 10, 10-2b) - the ray tier's half. A cascade's feedback pass hands
// over its list and its indirect arguments (PhotonVoxelLighting::runFeedbackPass); this records N fixed rays a voxel side
// of the listed bricks against the still world (rq_voxel_sides.comp), the hits read through the hit set, into the chain's
// side-gather scratch, before the bounce that reads it.
namespace {
struct VoxelSideParams {
    HitSetParams hit;          ///< THE HIT SET'S UNIFORMS (hitSetParams)
    float knobs[4] = {};       ///< the rays a side, the reach, the start's lift, the ray mask
    uint32_t cascade[4] = {};  ///< the cascade's place in the hit set's chain, its bricks along x and y
};
/// The job's own four (the params, the list, the scratch, the counts), the hit set at 0 and 5..36 (the cards'
/// gather's slots) and the escape shares at 37 (PHOTON-7 11-2 (6)).
constexpr unsigned kVoxelSideBindings = 38u;
constexpr unsigned kVoxelSideShareBinding = 37u;
static_assert(kVoxelSideBindings == kHitSetBindings + 5u, "the sides' gather set is the hit set, its own four and the shares");
constexpr unsigned kVoxelSideCounts = 9u;
}   // namespace

bool RayQueryTier::makeVoxelSidePipelineNow(std::string &err) {
    if (!ensureSamplers(err)) return false;
    VkDescriptorSetLayoutBinding b[kVoxelSideBindings] = {};
    hitSetLayout(kCardGatherHitSlots, b);
    const VkDescriptorType own[4] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   // 1 params
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 2 the feedback's list
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 3 the side-gather scratch
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 4 the counts
    };
    for (unsigned i = 0; i < 4u; ++i) {
        b[1u + i].binding = 1u + i;
        b[1u + i].descriptorType = own[i];
        b[1u + i].descriptorCount = 1u;
        b[1u + i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    b[kVoxelSideShareBinding].binding = kVoxelSideShareBinding;   // the escape shares (11-2 (6))
    b[kVoxelSideShareBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[kVoxelSideShareBinding].descriptorCount = 1u;
    b[kVoxelSideShareBinding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kVoxelSideBindings, kMaxTimedScenes * kVoxelSideRing, mVsSetLayout, err,
                         "rayquery/voxel-sides"))
        return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mVsSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mVsPipeLayout) != VK_SUCCESS) {
        err = "vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_voxelSidesSpv);
    smi.pCode = krq_voxelSidesSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mVsModule) != VK_SUCCESS) {
        err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mVsModule;
    cpi.stage.pName = "main";
    cpi.layout = mVsPipeLayout;
    if (createComputePipeline(cpi, &mVsPipeline, "voxel sides") != VK_SUCCESS) {
        err = "vkCreateComputePipelines failed";
        return false;
    }
    return plan.create(mVk, "rayquery/voxel-sides", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mVsPool, err);
}

void RayQueryTier::foldVoxelSideCounts(VoxelSideView &v) {
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    for (unsigned i = 0; i < kVoxelSideRing; ++i) {
        if (!v.countsPending[i] || !v.counts[i].mapped || uint32_t(now - v.countsFrame[i]) < inFlight) continue;
        const uint32_t *w = static_cast<const uint32_t *>(v.counts[i].mapped);
        v.total.rays += w[0];
        v.total.card += w[1];
        v.total.voxel += w[2];
        v.total.backFaces += w[3];
        v.total.noSample += w[4];
        v.total.sky += w[5];
        v.total.sides += w[6];
        v.total.emptySides += w[7];
        v.total.buried += w[8];
        ++v.total.dispatches;
        v.countsPending[i] = false;
    }
}

int RayQueryTier::gatherVoxelSides(OgreScene *scene, const Ogre::PhotonVoxelSideTrace &job, unsigned cascade) {
    joinAhead(kAheadVoxelSides);   // (its members are read below: the ahead build is joined first)
    // THE TIER: a scene that does not trace keeps the voxel rays (the no-ray route); one whose structure is not there
    // this pass WAITS (the voxel rays this pass, counted)
    if (!isOpen() || !scene || !scene->rayTracingResolved() || mVsFailed) return 0;
    VoxelSideView &v = mVoxelSides[scene];
    const auto wait = [&v]() { ++v.total.waits; return 2; };
    if (!job.list || !job.args || !job.scratch) return wait();
    SceneAs *saP = asOf(scene);
    if (!saP || !saP->tlas || !saP->st.enabled || saP->instanceCount == 0u) return wait();
    SceneAs &sa = *saP;
    if (!mVsPipeline) {
        std::string err;
        if (!makeVoxelSidePipeline(err)) {
            mVsFailed = true;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: the voxel sides' TLAS gather is off: " + err + " (the bounce gathers by the voxel rays)");
            return 0;
        }
    }
    foldVoxelSideCounts(v);
    const unsigned slot = v.call % kVoxelSideRing;
    if (v.countsPending[slot]) return wait();   // a call slot still in flight (more passes a frame than the ring holds)
    std::string err;
    if ((!v.params[slot].buffer &&
         !makeBuffer(sizeof(VoxelSideParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, v.params[slot], err)) ||
        (!v.counts[slot].buffer &&
         !makeBuffer(kVoxelSideCounts * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     true, false, v.counts[slot], err)))
        return wait();
    if (!v.sets[slot]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mVsPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mVsSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &v.sets[slot]) != VK_SUCCESS) {
            v.sets[slot] = VK_NULL_HANDLE;
            return wait();
        }
    }
    // ---- THE HIT SET (the one collection; no hit list, the GPU scene's instance table: the cards' gather's rule)
    HitSetSources hs;
    hitStandIns(hs.hit);
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::UavBufferPacked *instances = gs.live() ? gs.instanceBuffer() : nullptr;
    if (instances) {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(instances->getBufferInterface());
        hs.hit.instances = bi->getVboName();
        hs.hit.instancesOffset = VkDeviceSize(instances->_getFinalBufferStart()) * instances->getBytesPerElement();
        hs.hit.instancesRange = instances->getTotalSizeBytes();
        hs.hit.instanceEntries = gs.slotCount();
    }
    hitSetOf(scene, sa, hs);
    // THE VOXEL STORE ONLY (PHOTON-7 stage 10, the lead's R1): the cards read the voxels, so a voxel side that read a card
    // closed a CARD <-> VOXEL loop - every relight moved the voxels that read it, every voxel move relit the cards - that
    // never went quiet in a card-dense room (gi.card_residency (4): cascade 0's 263 bricks active, its resumes ~ its stops,
    // the cards relit 3-4 a frame for good), and made the chain a function of which cards are resident. One order: the
    // voxels read the voxels; the cards read the cards and the voxels. (e at the ray tier is the voxels' read: unchanged.)
    hs.withoutCards();
    if (cascade >= hs.cascadeCount) return wait();
    uint32_t geomSlots = hs.geomRows && hs.geomRowOfSlot ? uint32_t(hs.geomRowOfSlot->size()) : 0u;
    if (geomSlots) {
        RawBuffer &rb = v.rowOfSlot[slot];
        const VkDeviceSize want = VkDeviceSize(geomSlots) * sizeof(uint32_t);
        if (rb.buffer && rb.size < want) retire(rb);
        if (!rb.buffer && !makeBuffer(std::max<VkDeviceSize>(want, 1024u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                      false, rb, err))
            geomSlots = 0u;
        else
            std::memcpy(rb.mapped, hs.geomRowOfSlot->data(), size_t(want));
    }
    // ---- THE PARAMETERS: the hit set's, then this job's - the hit's own sun rays against every caster, still and
    // moving, no mover gate (rq_voxel_sides.comp's mask law, the cards' gather's; PHOTON-7 8A A4)
    VoxelSideParams pp{};
    hitSetParams(hs, geomSlots, pp.hit, HitReader::Cache);
    pp.hit.hitSun2[0] = float(kRayMaskStillCaster | kRayMaskMoverCaster);
    pp.hit.hitSun2[2] = kCardGatherSunRange;
    pp.hit.hitSun2[3] = 0.0f;
    pp.knobs[0] = float(std::max(job.rays, 1u));
    pp.knobs[1] = std::max(job.reach, 0.0f);
    pp.knobs[2] = std::max(job.lift, 0.0f);
    pp.knobs[3] = float(kRayMaskStill | kRayMaskMover);   // the world, movers in it (A4)
    pp.cascade[0] = cascade;
    pp.cascade[1] = job.brickDims[0];
    pp.cascade[2] = job.brickDims[1];
    // THE ESCAPE SHARES (PHOTON-7 11-2 (6)): cascade 0 writes its voxels' words into the scene's host-visible copy (its
    // grid's voxel dims; re-made, zeroed, when they change); every other cascade binds the same buffer and writes nothing
    {
        const uint32_t dims[3] = { job.brickDims[0] * 8u, job.brickDims[1] * 8u, job.brickDims[2] * 8u };
        if (cascade == 0u && (dims[0] != v.sharesDims[0] || dims[1] != v.sharesDims[1] || dims[2] != v.sharesDims[2])) {
            if (v.shares.buffer) retire(v.shares);
            const VkDeviceSize bytes = std::max<VkDeviceSize>(VkDeviceSize(dims[0]) * dims[1] * dims[2] * 4u, 1024u);
            if (!makeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, v.shares, err)) return wait();
            std::memset(v.shares.mapped, 0, size_t(bytes));
            for (int k = 0; k < 3; ++k) v.sharesDims[k] = dims[k];
        }
        if (!v.shares.buffer) {   // a cascade before cascade 0's first pass: a small zeroed stand-in
            if (!makeBuffer(1024u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false, v.shares, err)) return wait();
            std::memset(v.shares.mapped, 0, 1024u);
        }
        pp.cascade[3] = cascade == 0u && v.sharesDims[0] ? 1u : 0u;
        if (pp.cascade[3]) {
            ++v.sharesWrites;
            v.sharesFrame = frameNow();
            for (int k = 0; k < 3; ++k) {
                v.sharesOrigin[k] = pp.hit.voxelOrigin[0][k];
                const float inv = pp.hit.voxelInvSize[0][k];
                v.sharesCell[k] = inv > 0.0f ? 1.0f / (inv * float(dims[k])) : 0.0f;
            }
        }
    }
    std::memcpy(v.params[slot].mapped, &pp, sizeof(pp));

    // ---- THE SET: the job's own four, then the hit set through its one writer
    VkWriteDescriptorSet w[kVoxelSideBindings] = {};
    HitSetWrite hitSet;
    if (!hitSet.write(*this, v.sets[slot], kCardGatherHitSlots, hs, geomSlots ? v.rowOfSlot[slot].buffer : VK_NULL_HANDLE,
                      w, err))
        return wait();
    const auto bufInfo = [](Ogre::UavBufferPacked *b) {
        VkDescriptorBufferInfo info{};
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(b->getBufferInterface());
        info.buffer = bi->getVboName();
        info.offset = VkDeviceSize(b->_getFinalBufferStart()) * b->getBytesPerElement();
        info.range = b->getTotalSizeBytes();
        return info;
    };
    VkDescriptorBufferInfo ub{}, lb = bufInfo(job.list), sb = bufInfo(job.scratch), cb{};
    const VkDescriptorBufferInfo ab = bufInfo(job.args);
    ub.buffer = v.params[slot].buffer;
    ub.range = sizeof(VoxelSideParams);
    cb.buffer = v.counts[slot].buffer;
    cb.range = kVoxelSideCounts * sizeof(uint32_t);
    const auto own = [&](uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet & {
        VkWriteDescriptorSet &e = w[binding];
        e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        e.dstSet = v.sets[slot];
        e.dstBinding = binding;
        e.descriptorCount = 1;
        e.descriptorType = type;
        return e;
    };
    own(1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &ub;
    own(2u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &lb;
    own(3u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &sb;
    own(4u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &cb;
    VkDescriptorBufferInfo shb{};
    shb.buffer = v.shares.buffer;
    shb.range = v.shares.size;
    own(kVoxelSideShareBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &shb;
    vkUpdateDescriptorSets(mVk, kVoxelSideBindings, w, 0, nullptr);

    // ---- THE LAYOUTS through Ogre's solver, before the command buffer is taken: the hit set read, the list and the
    // arguments read, the scratch written (the bounce that follows reads it, ordered by its own barrier analysis)
    {
        mDev->_jahRecordQueue().endAllEncoders();
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        hitSetTransitions(solver, trans, hs);
        if (instances) solver.resolveTransition(trans, instances, Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, job.list, Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, job.args, Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, job.scratch, Ogre::ResourceAccess::Write, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return wait();
    clearDummyImages(cmd);
    initHitDummies(cmd);
    {
        // THE COUNTS, zeroed; and THE ARGUMENTS the select wrote visible to the indirect read (the solver orders the
        // compute reads; the indirect stage is this dispatch's own)
        vkCmdFillBuffer(cmd, v.counts[slot].buffer, 0, kVoxelSideCounts * sizeof(uint32_t), 0u);
        VkMemoryBarrier fb{};
        fb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &fb, 0,
                             nullptr, 0, nullptr);
    }
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Sweep, 0, "voxel.sides", mRs, scene);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mVsPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mVsPipeLayout, 0, 1, &v.sets[slot], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatchIndirect(cmd, ab.buffer, ab.offset);
    }
    {
        VkMemoryBarrier hb{};
        hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &hb, 0, nullptr, 0,
                             nullptr);
    }
    v.countsFrame[slot] = frameNow();
    v.countsPending[slot] = true;
    ++v.call;
    return 1;
}

bool RayQueryTier::voxelSideShares(OgreScene *scene, GiVoxelSideShares &out) {
    out = GiVoxelSideShares();
    auto it = mVoxelSides.find(scene);
    if (it == mVoxelSides.end() || !it->second.shares.mapped || !it->second.sharesDims[0]) return false;
    const VoxelSideView &v = it->second;
    out.width = int(v.sharesDims[0]);
    out.height = int(v.sharesDims[1]);
    out.depth = int(v.sharesDims[2]);
    const size_t n = size_t(out.width) * out.height * out.depth;
    out.words.resize(n);
    std::memcpy(out.words.data(), v.shares.mapped, n * sizeof(uint32_t));
    out.available = true;
    return true;
}

void RayQueryTier::voxelSideCounts(OgreScene *scene, GiStatus::VoxelSideCounts &out) {
    out = GiStatus::VoxelSideCounts();
    auto it = mVoxelSides.find(scene);
    if (it == mVoxelSides.end()) return;
    foldVoxelSideCounts(it->second);
    out = it->second.total;
}

void RayQueryTier::forgetVoxelSides(OgreScene *scene) {
    auto it = mVoxelSides.find(scene);
    if (it == mVoxelSides.end()) return;
    for (unsigned i = 0; i < kVoxelSideRing; ++i) {
        retireSet(it->second.sets[i], mVsPool);
        retire(it->second.params[i]);
        retire(it->second.rowOfSlot[i]);
        retire(it->second.counts[i]);
    }
    retire(it->second.shares);
    mVoxelSides.erase(it);
}

int OgreScene::gatherVoxelSides(const Ogre::PhotonVoxelSideTrace &job, unsigned cascade) {
    return mEngine && mEngine->mRayTier ? mEngine->mRayTier->gatherVoxelSides(this, job, cascade) : 0;
}
bool OgreScene::giVoxelSideShares(GiVoxelSideShares &out) {
    out = GiVoxelSideShares();
    return mEngine && mEngine->mRayTier ? mEngine->mRayTier->voxelSideShares(this, out) : false;
}
void OgreScene::voxelSideCounts(GiStatus::VoxelSideCounts &out) const {
    out = GiStatus::VoxelSideCounts();
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->voxelSideCounts(const_cast<OgreScene *>(this), out);
}

CardGatherResult OgreScene::gatherCards(const CardGatherTrace &job) {
    return mEngine && mEngine->mRayTier ? mEngine->mRayTier->gatherCards(this, job) : CardGatherResult::NoRays;
}
void OgreScene::cardGatherCounts(CardCacheStatus::GatherCounts &out) const {
    out = CardCacheStatus::GatherCounts();
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->cardGatherCounts(const_cast<OgreScene *>(this), out);
}
void OgreScene::cardGatherSky(std::vector<std::pair<unsigned, unsigned>> &out) {
    out.clear();
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->cardGatherSky(this, out);
}

// ---------------------------------------------------------------------------
/// A DESTRUCTOR MAY NOT THROW — it is implicitly `noexcept`, so an exception
/// leaving it is `std::terminate`, and this one submits to the GPU (lane VR-3b,
/// 2026-09-17; the owner's WiVRn smoke died exactly here). The chain was:
///
///     renderOneFrame -> OgreView::syncReflectListener
///       -> ~ReflectPassListener -> RenderSystem::flushCommands
///       -> VulkanQueue::commitAndNextCommandBuffer -> vkQueueSubmit
///       -> VK_ERROR_DEVICE_LOST -> checkVkResult THROWS -> SIGABRT
///
/// fork b028638c1 (was 0069) made the same statement about the pin's own staging buffer;
/// this is OUR destructor and it is ours to make safe. Two rules, in order:
/// a lost device is not flushed at all (a submit on it can only fail, and the
/// engine's own frame tail already reports the loss), and anything that still
/// escapes is caught and logged — the process keeps its stack and its log
/// instead of aborting with neither.
ReflectPassListener::~ReflectPassListener() {
    if (!mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    try {
        // Same rule as dropReflectState: nothing may be freed while a command
        // buffer that has it bound is still recording.
        Ogre::RenderSystem *rs = mRoot ? mRoot->getRenderSystem() : nullptr;
        if (rs && !rs->isDeviceLost()) rs->flushCommands();
        mView->mEngine->mRayTier->forgetReflect(this);
        mView->mEngine->mRayTier->forgetGather(this);
        mView->mEngine->mRayTier->forgetSunContact(this);
        mView->mEngine->mRayTier->forgetAreaVis(this);
        mView->mEngine->mRayTier->forgetHits(this);
        mView->mEngine->mRayTier->forgetGlassEscape(this);
    } catch (Ogre::Exception &e) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the ray-reflection listener could not be flushed away cleanly (" +
            e.getDescription() + ") - the reflection resources are released without it",
            Ogre::LML_CRITICAL);
        // THE BOOKKEEPING STILL HAS TO HAPPEN, or the tier keeps a record keyed
        // by an address that is about to be freed and the next view allocated at
        // it inherits a stranger's images.
        try { mView->mEngine->mRayTier->forgetReflect(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetGather(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetSunContact(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetAreaVis(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetHits(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetGlassEscape(this); } catch (...) {}
    } catch (...) {
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the ray-reflection listener's teardown threw a non-Ogre exception",
            Ogre::LML_CRITICAL);
        try { mView->mEngine->mRayTier->forgetReflect(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetGather(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetSunContact(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetAreaVis(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetHits(this); } catch (...) {}
        try { mView->mEngine->mRayTier->forgetGlassEscape(this); } catch (...) {}
    }
}

void ReflectPassListener::passPreExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def) return;
    // THE CAPTURE'S WORKSPACE (CLOSE-SHOT-2) records nothing: it is handed this
    // frame's products (RayQueryTier::bindCapturePass).
    if (mView->isCaptureWorkspace(pass->getParentNode()->getWorkspace())) {
        if (def->mIdentifier == kHitDecodePassIdentifier || def->mPrePassMode != Ogre::PrePassUse) return;
        mView->mEngine->mRayTier->bindCapturePass(this, mView, pass);
        return;
    }
    // THE HIT DECODE PASS (PHOTON-HIT-SHADE-1, ChainDesc::hitDecode): every ray
    // job TRACED in front of it (photon_trace) — a hit no cache can shade is
    // appended to the hit list this pass shades; HlmsAtom is armed for its length
    // (endHitDecode disarms it).
    if (def->mIdentifier == kHitDecodePassIdentifier) {
        mView->mEngine->mRayTier->beginHitDecode(this, mView, pass);
        return;
    }
    // THE PASS, BY WHAT IT DOES AND NOT BY ITS NAME. `PrePassUse` is Ogre's own
    // word for "this pass shades with a prepass' G-buffers and the ssr texture"
    // — the pass every ray job's answer is read by. The write-back, the
    // reflection's filter, the gather's filter/SH/integrate and every job's
    // pass-scoped registration ran in front of it, in photon_resolve.
    // THE BLENDED PASS (PHOTON-7 10-4 R11): the view's measured sky visibility for its fog, for its length
    if (def->mIdentifier == kBlendedPassIdentifier) {
        if (mView->ogreScene() && mView->mEngine->mRayTier->gatherSkyVis(this))
            FogHlmsListener::setFogSkyVis(mView->ogreScene()->sceneManager(), mView->mEngine->mRayTier->gatherSkyVis(this));
        // ...and its escape list (PHOTON-7 10-4 R4)
        mView->mEngine->mRayTier->bindGlassEscape(this, mView, 0u);
        return;
    }
    // THE REFRACTIVES (PHOTON-7 10-4 R4): their escape list
    if (def->mIdentifier == kRefractivesPassIdentifier) {
        mView->mEngine->mRayTier->bindGlassEscape(this, mView, 1u);
        return;
    }
    if (def->mPrePassMode != Ogre::PrePassUse) return;
    // THE OPAQUE SCREEN DECODE PASS (ATOM-DECODE-CLASS-1) is the first such pass of
    // the frame and the opaque pass the last: the jobs finish once, in front of the
    // first; the registrations are held back between the two (suspendPassBindings:
    // the planar mirrors render there) and put back in front of the second.
    if (def->mIdentifier != kScreenDecodePassIdentifier && mRayJobsFinished) {
        mView->mEngine->mRayTier->resumePassBindings(this);
        return;
    }
    // THE JOBS FINISHED IN photon_resolve, the pass in front of this one.
    mRayJobsFinished = def->mIdentifier == kScreenDecodePassIdentifier;
}

/// GATHER-0 (fix round, D2). The registration made in `passPreExecute` names
/// this view's full-resolution irradiance texture and is read by every colour
/// pass of the same SceneManager; it is valid for exactly ONE pass — the one
/// this listener runs in front of — so it is taken away the moment that pass
/// is over. What this removes is a whole class of wrong pictures and two ways
/// to lose a frame outright (see the declaration).
void ReflectPassListener::passPosExecute(Ogre::CompositorPass *pass) {
    if (!pass || !mView || !mView->mEngine || !mView->mEngine->mRayTier) return;
    if (pass->getType() != Ogre::PASS_SCENE) return;
    const auto *def = static_cast<const Ogre::CompositorPassSceneDef *>(pass->getDefinition());
    if (!def) return;
    if (mView->isCaptureWorkspace(pass->getParentNode()->getWorkspace())) {
        if (def->mIdentifier == kHitDecodePassIdentifier || def->mPrePassMode != Ogre::PrePassUse) return;
        mView->mEngine->mRayTier->releaseCapturePass(this);
        return;
    }
    if (def->mIdentifier == kHitDecodePassIdentifier) {
        mView->mEngine->mRayTier->endHitDecode(this);
        return;
    }
    if (def->mIdentifier == kBlendedPassIdentifier) {
        if (mView->ogreScene()) FogHlmsListener::setFogSkyVis(mView->ogreScene()->sceneManager(), nullptr);
        // ...its escapes traced and added into its target (PHOTON-7 10-4 R4)
        mView->mEngine->mRayTier->traceGlassEscape(this, mView, pass, 0u);
        return;
    }
    if (def->mIdentifier == kRefractivesPassIdentifier) {
        mView->mEngine->mRayTier->traceGlassEscape(this, mView, pass, 1u);
        return;
    }
    if (def->mPrePassMode != Ogre::PrePassUse) return;
    if (def->mIdentifier == kScreenDecodePassIdentifier) {   // the opaque pass follows
        mView->mEngine->mRayTier->suspendPassBindings(
            this, mView->ogreScene() ? mView->ogreScene()->sceneManager() : nullptr);
        return;
    }
    mRayJobsFinished = false;
    mView->mEngine->mRayTier->releaseGatherBinding(this);
    mView->mEngine->mRayTier->releaseSunContactBinding(this);
    mView->mEngine->mRayTier->releaseAreaVisBinding(this);
}

/// THE PHOTON VIEW'S OVERLAY (PHOTON-VIEW-1) goes through the tier's retire
/// window: this frame's reflection and gather sets may still name it.
void OgreView::retirePhotonOverlay() {
    if (!mPhotonOverlay) return;
    if (mEngine && mEngine->mRayTier) {
        mEngine->mRayTier->retireViewTexture(mPhotonOverlay);
    } else {
        JAH_TRY {
            if (mRoot && mRoot->getRenderSystem()) {
                mRoot->getRenderSystem()->flushCommands();
                mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(mPhotonOverlay);
            }
        } JAH_CATCH(mError, );
    }
    mPhotonOverlay = nullptr;
    mPhotonOverlayWritten = false;
}

void OgreView::dropReflectState() {
    if (!mReflectListener || !mEngine || !mEngine->mRayTier) return;
    // SUBMIT AND WAIT FIRST. A descriptor set may not be freed while a command
    // buffer that has it bound is still recording, and the textures it
    // references are about to be destroyed. This is not a frame path: a
    // workspace rebuild already destroys and recreates every texture in the
    // chain, so the flush costs nothing that was not already being paid.
    if (mRoot && mRoot->getRenderSystem()) mRoot->getRenderSystem()->flushCommands();
    mEngine->mRayTier->forgetReflect(mReflectListener.get());
    mEngine->mRayTier->forgetGather(mReflectListener.get());
    mEngine->mRayTier->forgetSunContact(mReflectListener.get());
    mEngine->mRayTier->forgetAreaVis(mReflectListener.get());
    mEngine->mRayTier->forgetHits(mReflectListener.get());
    mEngine->mRayTier->forgetGlassEscape(mReflectListener.get());
}

void OgreView::syncReflectListener() {
    // THE CHAIN'S SHAPE IS RE-CHECKED HERE, once a frame, and that is not
    // belt-and-braces: `ChainDesc::rayReflect` depends on the SCENE (the
    // project's ray row, lane RAYROW-1) and a view has no scene when its chain
    // is first built, nor does `setScene` rebuild one. Without this a view
    // would render its whole life with the shape it was constructed with.
    // Cheap: a comparison against what the current definition was built with.
    // ...and `ChainDesc::probeGather` is the same story for the same reason
    // (GATHER-1a): the gather's row is the SCENE's, so a view whose scene turns
    // it on has to gain the prepass it reads its probes' surfaces from. One
    // `chainDesc()` for both comparisons.
    // ...and `ChainDesc::sunContact` (PHOTON-RAYS-1) the same again: the row
    // is the scene's and it brings the prepass.
    // ...AS THE PREPASS THEY ASK FOR (ChainDesc::prepass, PHOTON-GATHER-1d): a
    // gather or sun-contact toggle where the prepass already runs is not a new
    // graph and rebuilds nothing.
    if (mChainRayReflect != chainDesc().rayReflect || mChainPrepass != chainDesc().prepass() ||
        mChainHitDecode != chainDesc().hitDecode)
        rebuildWorkspaceDef();
    sizeHitList();
    // The same arming rule as the planar and globals listeners, and the same
    // reason it is re-evaluated every frame: the shape above can change, and a
    // view can gain or lose its scene.
    //
    // AND `mEnabled` IS PART OF IT, WHICH IS NOT FREE AND IS NOT A CHOICE
    // (lane VR-3b, 2026-09-17, measured). Tearing the listener down when a view
    // merely blinks off costs a device-wide `flushCommands` — a queue submit,
    // from a destructor, in a frame that may be drawing nothing at all (a VR
    // session's first frames: the runtime answers "no picture", the session's
    // View goes off, the Player's is off by design and the editor's is hidden)
    // — plus the free and re-allocation of the reflection's rings. Keeping it
    // instead looks free and IS NOT: `dropReflect` also drops the trace's
    // TEMPORAL HISTORY, so retaining it across a disable changes the picture.
    // Measured, interleaved on one box: `scripting.e2e.movable_lamp_rest` went
    // from 12/12 passing to 2/14 with the listener retained (the floor probe
    // moves 7/255), and back to base with the teardown restored. So the
    // teardown stays, the destructor is made safe instead (below), and "a view
    // that blinks off keeps its reflection history" is a change that has to be
    // designed and measured, not slipped in.
    // ...AND THE GATHER ARMS IT TOO (GATHER-1a). One listener, two consumers:
    // both are recorded in front of the same `PrePassUse` pass, and the gather
    // does not need the reflection row — a chain may carry the prepass for the
    // probes alone (`ChainDesc::probeGather`). Without this term a project with
    // the gather on and SSR off would build the prepass and nothing would ever
    // run in front of it.
    const ChainDesc shape = chainDesc();
    const bool wanted = drawing() && mScene && mCamera && mEngine && mEngine->mRayTier != nullptr &&
                        (shape.rayReflect || shape.probeGather || shape.sunContact);
    if (!wanted) {
        if (mReflectListener) {
            removeWorkspaceListener(mReflectListener.get());
            mReflectListener.reset();
        }
        return;
    }
    if (!mReflectListener) {
        mReflectListener.reset(new ReflectPassListener());
        addWorkspaceListener(mReflectListener.get());
    }
    mReflectListener->mView = this;
    mReflectListener->mRoot = mRoot;
    mReflectListener->mFoldsOnForget = isOffscreen();
}

// THE HIT LIST'S GROWTH (kHitListWidth's law in EnginePrivate.h). Before the workspace runs, so no pass of this frame has
// touched the list yet. The list is created at its share of the target (OgreView::hitListCreateRows, OgreView.cpp: the
// graph builds it at max(the rows it has, that)); it GROWS when the target outgrew its creation size or a retired frame
// overflowed it - the rows double until the list holds that frame's demand (the counter word 0, which counts past the
// capacity) - and is never shrunk. A growth re-makes the three list textures IN PLACE the way Ogre re-makes a window's
// resizable textures (OnStorage, the new resolution, Resident, then CompositorNode::notifyRecreated for the passes that
// render into them) - never the graph, so no other texture and no history moves. The ray tier re-reads the textures (and
// re-makes its buffer to the new size) at its next prepare.
void OgreView::sizeHitList() {
    if (!mWorkspace || !mChainHitDecode || !mReflectListener || !mEngine || !mEngine->mRayTier) return;
    const unsigned create = hitListCreateRows();
    const unsigned long long demand = mEngine->mRayTier->takeHitOverflow(mReflectListener.get());
    if (demand == 0ull && mHitListRows >= create) return;   // nothing overflowed and the target did not grow
    const Ogre::IdString names[] = { Ogre::IdString("jahHitIds"), Ogre::IdString("jahHitDest"),
                                     Ogre::IdString("jahHitRadiance") };
    struct List { Ogre::CompositorNode *node = nullptr; Ogre::TextureGpu *tex[3] = {}; };
    std::vector<List> lists;
    for (Ogre::CompositorNode *node : mWorkspace->getNodeSequence()) {
        List l{ node, {} };
        try {
            for (int i = 0; i < 3; ++i) l.tex[i] = node->getDefinedTexture(names[i]);
        } catch (Ogre::Exception &) { continue; }
        if (l.tex[0] && l.tex[1] && l.tex[2]) lists.push_back(l);
    }
    if (lists.empty()) return;
    // THE LIST AS BUILT (the textures' own height is the truth: a graph built before the target grew holds fewer)
    const unsigned built = lists.front().tex[0]->getHeight();
    const unsigned maxRows = std::max<unsigned>(mRoot->getRenderSystem()->getCapabilities()->getMaximumResolution2D(), 1u);
    unsigned rows = std::max(built, create);
    while ((unsigned long long)rows * kHitListWidth < demand && rows < maxRows) rows = std::min(rows * 2u, maxRows);
    mHitListRows = rows;
    if (rows == built) return;
    for (const List &l : lists)
        for (Ogre::TextureGpu *tx : l.tex) {
            tx->_transitionTo(Ogre::GpuResidency::OnStorage, nullptr);
            tx->setResolution(kHitListWidth, rows, 1u);
            tx->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
            l.node->notifyRecreated(tx);
        }
}

// ---------------------------------------------------------------------------
// THE HIT DECODE — the tier's half (PHOTON-HIT-SHADE-1; SPECS/atom/
// D2_HIT_SHADING_DESIGN.md with the lead's F1-F8). A ray hit no cache can shade
// is a pixel of a second visibility buffer: the traces append it to the chain's
// hit list (jah_rq_hit_record.glsl), the chain's "Jahshaka hit decode" pass draws
// HlmsAtom's decode over the list in HIT MODE, and rq_hit_composite.comp scatters
// the decoded radiance into the reflection's mean and the gather's atlas — the
// same arithmetic a card- or voxel-lit hit takes. One copy of the lighting.
namespace {
/// The sun of a pass: its shadow node's first directional caster (the light
/// HlmsPbs's first-light shadow term belongs to — the sun contact's rule).
bool passSun(Ogre::CompositorPass *pass, Ogre::Vector3 &toSun) {
    toSun = Ogre::Vector3::ZERO;
    if (!pass || pass->getType() != Ogre::PASS_SCENE) return false;
    const Ogre::CompositorShadowNode *sn = static_cast<Ogre::CompositorPassScene *>(pass)->getShadowNode();
    if (!sn) return false;
    for (const Ogre::LightClosest &lc : sn->getShadowCastingLights()) {
        if (lc.light && lc.light->getType() == Ogre::Light::LT_DIRECTIONAL) {
            toSun = -lc.light->getDerivedDirection();
            break;
        }
    }
    if (toSun.squaredLength() < 1e-12f) return false;
    toSun.normalise();
    return true;
}
/// A read-only view of an Ogre UAV buffer as a raw descriptor.
VkDescriptorBufferInfo rawBufferInfo(Ogre::UavBufferPacked *b) {
    VkDescriptorBufferInfo info{};
    auto *bi = static_cast<Ogre::VulkanBufferInterface *>(b->getBufferInterface());
    info.buffer = bi->getVboName();
    info.offset = VkDeviceSize(b->_getFinalBufferStart()) * b->getBytesPerElement();
    info.range = b->getTotalSizeBytes();
    return info;
}
}   // namespace

bool RayQueryTier::ensureHitDummies(std::string &err) {
    if (mHitDummiesReady) return true;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R32G32B32A32_UINT, mHitDummyIds, err)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT, mHitDummyColour, err)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R32_UINT, mHitDummyDest, err)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R16G16B16A16_SFLOAT, mHitDummyHist, err, 1u)) return false;
    if (!makeStorageImage(1u, 1u, VK_FORMAT_R32G32_UINT, mHitDummyState, err, 1u)) return false;
    mHitDummiesReady = true;
    mHitDummiesNeedInit = true;
    return true;
}

void RayQueryTier::initHitDummies(VkCommandBuffer cmd) {
    if (!mHitDummiesNeedInit || !cmd) return;
    mHitDummiesNeedInit = false;
    VkImageMemoryBarrier b[5] = {};
    ReflectImage *imgs[5] = { &mHitDummyIds, &mHitDummyColour, &mHitDummyDest, &mHitDummyHist, &mHitDummyState };
    for (int i = 0; i < 5; ++i) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = imgs[i]->image;
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = 1;
        b[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 5, b);
}

void RayQueryTier::hitStandIns(HitListBinding &out) {
    out = HitListBinding();
    std::string err;
    if (!ensureHitDummies(err) || !ensureDummyImages(err)) return;
    out.ids = mHitDummyIds.view;
    out.dest = mHitDummyDest.view;
}

void RayQueryTier::readHitCounters(HitView &hv) {
    if (!hv.readback.mapped) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    uint32_t newest = 0u;
    bool any = false;
    for (unsigned i = 0; i < kFramesInFlight; ++i) {
        HitView::Pending &pd = hv.pending[i];
        if (!pd.live || uint32_t(now - pd.frame) < inFlight) continue;
        const uint32_t *w = reinterpret_cast<const uint32_t *>(
            static_cast<const unsigned char *>(hv.readback.mapped) + i * kHitCounterBytes);
        // (every retired frame's NEE counts, the newest or not: a sum)
        hv.neeRays += w[10];
        hv.neeHits += w[11];
        // EVERY retired frame's overflow, the newest or not (kHitListWidth's law: the list grows to hold it)
        if (w[0] > pd.listRecords) hv.overflowDemand = std::max<unsigned long long>(hv.overflowDemand, w[0]);
        if (!any || int32_t(pd.frame - newest) > 0) {
            hv.appended = w[0];
            hv.dropped = w[1];
            hv.readCapacity = pd.listRecords;
            hv.restActive = w[2];
            hv.restCapped = w[3];
            hv.restBatching = w[4];
            hv.restActiveTag = pd.restTag;
            if (w[9] > 0u && pd.restTag != 0u) {
                hv.boundOwed = w[8];
                hv.boundNeedMax = w[9];
                hv.boundTag = pd.restTag;
            }
            newest = pd.frame;
            any = true;
        }
        pd.live = false;
    }
    if (any) {
        if (!hv.everRead) hv.firstCapacity = hv.readCapacity;
        hv.readDraw = hv.scene ? hv.scene->drawSerial() : 0u;
        hv.everRead = true;
    }
}

unsigned long long RayQueryTier::takeHitOverflow(const ReflectPassListener *key) {
    const auto it = mHits.find(key);
    if (it == mHits.end()) return 0ull;
    const unsigned long long d = it->second.overflowDemand;
    it->second.overflowDemand = 0ull;
    return d;
}

bool RayQueryTier::prepareHitList(const ReflectPassListener *key, OgreView *view,
                                  Ogre::CompositorPass *pass, HitListBinding &out) {
    hitStandIns(out);
    HitView &hv = mHits[key];
    hv.live = false;
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    Ogre::Camera *cam = view ? view->camera() : nullptr;
    if (!scene || !cam || !pass || !out.ids) return false;
    hv.scene = scene;
    // (a stereo view's width() is its eye PAIR's: both eyes append to this one list)
    hv.eyes = view->stereo() ? 2u : 1u;
    hv.eyeWidth = view->width() / hv.eyes;
    hv.eyeHeight = view->height();
    readHitCounters(hv);
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node) return false;
    try {
        hv.ids = node->getDefinedTexture(Ogre::IdString("jahHitIds"));
        hv.dest = node->getDefinedTexture(Ogre::IdString("jahHitDest"));
        hv.radiance = node->getDefinedTexture(Ogre::IdString("jahHitRadiance"));
    } catch (Ogre::Exception &) { return false; }
    if (!hv.ids || !hv.dest || !hv.radiance || !hv.ids->isUav()) return false;
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::UavBufferPacked *instances = gs.live() ? gs.instanceBuffer() : nullptr;
    if (!instances) return false;
    Ogre::VaoManager *vao = mRs->getVaoManager();
    // THE LIST'S BUFFER, sized to the list: the twelve counter words, then ten per
    // record (jah_rq_hit_record.glsl kJahHitAuxBase / kJahHitAuxWords: a blended run record's screen share is words 4-5,
    // the NEE delta 6-7, a reflection's crossed panes 8-9). Re-made when the list grows (Ogre's destroy is delayed past every
    // frame in flight; every reader re-reads `hv.buf` before it binds).
    {
        const size_t words = kHitCounterWords + kHitAuxWords * size_t(hv.ids->getWidth()) * hv.ids->getHeight();
        // (the list only grows: OgreView::sizeHitList re-makes it in place, larger)
        if (hv.buf && hv.buf->getNumElements() != words) {
            vao->destroyUavBuffer(hv.buf);
            hv.buf = nullptr;
        }
        if (!hv.buf) hv.buf = vao->createUavBuffer(words, 4u, Ogre::BB_FLAG_READONLY, nullptr, false);
    }
    std::string err;
    if (!hv.readback.buffer &&
        !makeBuffer(VkDeviceSize(kFramesInFlight) * kHitCounterBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false,
                    hv.readback, err))
        return false;
    hv.capacity = hv.ids->getWidth() * hv.ids->getHeight();
    // THE OVERFLOW ARM (rq.hitListCap, a test door): the traces and the write-back hold at most that many records, so
    // a fixture FORCES drops and measures that they are no sample, never black (PHOTON-2 stage 2's tail).
    {
        const double cap = armValue(ArmId::HitListCap);
        if (cap >= 1.0) hv.capacity = std::min(hv.capacity, uint32_t(cap));
    }
    hv.width = hv.ids->getWidth();

    // THE LAYOUTS through Ogre's solver, before the command buffer is taken (the
    // reflection's rule: executeResourceTransition closes every encoder).
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { hv.ids, hv.dest })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Write,
                                     computeStage);
        solver.resolveTransition(trans, hv.buf, Ogre::ResourceAccess::ReadWrite, computeStage);
        solver.resolveTransition(trans, instances, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return false;
    initHitDummies(cmd);
    // THE COUNTER, ZEROED for this frame: every earlier reader (last frame's
    // decode, write-back, readback copy) is ordered before the transfer.
    const VkDescriptorBufferInfo bufInfo = rawBufferInfo(hv.buf);
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
        // (every counter word but the rest bound's two, 8-9: the trace zeroes them up to the rest's 2N and they hold
        // the temporal pass' figures after it - PHOTON-7 11-1c)
        vkCmdFillBuffer(cmd, bufInfo.buffer, bufInfo.offset, 32u, 0u);
        vkCmdFillBuffer(cmd, bufInfo.buffer, bufInfo.offset + 40u, kHitCounterBytes - 40u, 0u);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
    }
    const auto uavView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = t;
        slot.access = Ogre::ResourceAccess::Write;
        slot.pixelFormat = t->getPixelFormat();
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    out.on = true;
    out.ids = uavView(hv.ids);
    out.dest = uavView(hv.dest);
    if (!out.ids || !out.dest) { hitStandIns(out); return false; }
    out.buf = bufInfo.buffer;
    out.bufOffset = bufInfo.offset;
    out.bufRange = bufInfo.range;
    const VkDescriptorBufferInfo instInfo = rawBufferInfo(instances);
    out.instances = instInfo.buffer;
    out.instancesOffset = instInfo.offset;
    out.instancesRange = instInfo.range;
    out.instanceEntries = gs.slotCount();
    out.capacity = hv.capacity;
    out.width = hv.width;
    Ogre::Vector3 toSun;
    out.sun = passSun(pass, toSun);
    out.toSun[0] = toSun.x; out.toSun[1] = toSun.y; out.toSun[2] = toSun.z;
    out.sunMask = kRayMaskCaster;
    // THE SUN RAY'S LENGTH: the camera's far plane (the world the frame draws),
    // floored so an unbounded or tiny far plane still reaches past a room.
    {
        const float farClip = float(cam->getFarClipDistance());
        out.sunRange = farClip > 0.0f ? std::max(farClip, 100.0f) : 10000.0f;
    }
    // THE HIT'S SUN LEG UNDER THE ONE ORIGIN LAW (jah_ray_origin.glsl; the lead's round-7 ruling - the 1 mm / 1e-4 t
    // floor is deleted): the shader lifts by the hit's own float bound; a near hit is ON level 0, which the casters'
    // near copies are, so its surface term is 0, and a FAR copy's hit is on the coarsest level, within the traced
    // set's widest coarse-vs-fine gap of level 0 (farOverlap: already the drift law's distance, rayOriginLevelLift).
    {
        const SceneAs *sa = asOf(scene);
        out.farLift = sa ? sa->farOverlap : 0.0f;
    }
    hv.live = true;
    return true;
}

bool RayQueryTier::decodeTwinsStale(OgreScene *scene) const {
    if (!scene) return false;
    const SceneAs *saP = asOf(scene);
    if (!saP) return false;
    // THE EDITS SINCE THE FRAME'S DRAIN (ATOM-CPU-WALKS-1): a datablock re-hashed
    // since then (the PBS change log) or holding an edit not hashed yet (the pool's
    // dirty list) whose twin's BUCKET moved, and that this scene's draws serve. The
    // log and the dirty list are empty on a frame nobody edited a material, so this
    // costs nothing then — it used to re-read every witness item per pass.
    Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    auto *pbs = hm ? dynamic_cast<ScenePbs *>(hm->getHlms(Ogre::HLMS_PBS)) : nullptr;
    if (!atom || !pbs) return false;
    if (pbs->hashNotesOverflowed()) return true;   // the log lost its detail: assume the worst
    const std::vector<uint32_t> &words = saP->decodeWords;
    const auto servedAndMoved = [&](const Ogre::HlmsDatablock *db) {
        uint32_t word = HlmsAtom::kNoMaterialWord;
        if (!atom->decodeTwinMoved(db, word)) return false;
        return std::binary_search(words.begin(), words.end(), word);
    };
    for (const ScenePbs::HashNote &n : pbs->hashNotes())
        if (n.db && servedAndMoved(n.db)) return true;
    bool stale = false;
    pbs->forEachDirtyDatablock([&](const Ogre::HlmsDatablock *db) {
        if (!stale && servedAndMoved(db)) stale = true;
    });
    return stale;
}

void RayQueryTier::beginHitDecode(const ReflectPassListener *key, OgreView *view,
                                  Ogre::CompositorPass *pass) {
    if (!view || !pass) return;
    HitView &hv = mHits[key];
    // THE CAMERA'S AUTO ASPECT IS HELD OFF FOR THE DECODE PASS, ARMED OR NOT: the
    // pass renders into the list (kHitListWidth x its rows) whether or not it decodes, and an
    // auto-aspect camera is re-aspected by every PASS_SCENE it renders
    // (OgreCompositorPassScene.cpp) — the projection the pass' shaders read, and
    // the aspect Ogre's PlanarReflections matches cameras by (measured: a frame
    // whose decode was skipped re-aspected the camera and the VR eyes' PBS pass
    // changed permutation, vr.warmup). endHitDecode restores it.
    if (!hv.pinnedCam) {
        hv.pinnedCam = view->camera();
        if (hv.pinnedCam) {
            hv.camAuto = hv.pinnedCam->getAutoAspectRatio();
            hv.pinnedCam->setAutoAspectRatio(false);
        }
    }
    if (!isOpen() || hv.decodeFrame != frameNow()) return;   // photon_trace filled no list
    if (!hv.live) return;
    const bool reflected = mReflects.count(key) && mReflects[key].finishPending;
    const bool gathered = mGather && mGather->tracedAtlas(key) != VK_NULL_HANDLE;
    if (!reflected && !gathered) return;
    // A STALE TWIN NEVER DRAWS: a PBS datablock that changed its permutation since
    // updateScene's sync (the engine edits datablocks in place — the sky's manual
    // cube taken away when a probe grid binds, mid-frame) leaves its twin a copy
    // of the old state. A twin holding a manual cube under the pass' automatic
    // probe array cannot compile (OgreSky.cpp's env-probe note), and a shader made
    // to compile without it would still get the twin's BAKED texture set, one
    // texture larger than its root layout's range — a descriptor write past the
    // set (the pin's count assert is compiled out): the Xid 109s this lane's gate
    // measured. This frame's records go unshaded (the write-back's "unshaded"
    // answer); the next updateScene re-derives the twin.
    if (decodeTwinsStale(view->ogreScene())) return;
    // ARM THE DECODE for this pass alone: the source (the list and the GPU scene's
    // tables, re-read now — they are re-created when they grow), the scene's
    // decode draws shown, and the camera's auto aspect held off so the pass'
    // projection is the view's own (the target is the list, not a picture: F7).
    Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    OgreScene *scene = view->ogreScene();
    if (!atom || !scene) return;
    detail::GpuScene &gs = scene->gpuScene();
    if (!gs.live()) return;
    gs.flushGeomRows();
    HlmsAtom::DecodeSource src;
    src.ids = hv.ids;
    src.instances = gs.instanceBuffer();
    src.levels = gs.levelBuffer();
    src.geomRows = gs.geomBuffer();
    src.hitMode = true;
    src.hitBuf = hv.buf;
    atom->setDecodeSource(src);
    atom->showSceneDecodes(scene->sceneManager(), true);
    hv.armedSm = scene->sceneManager();
}

void RayQueryTier::endHitDecode(const ReflectPassListener *key) {
    auto it = mHits.find(key);
    if (it == mHits.end()) return;
    HitView &hv = it->second;
    if (hv.pinnedCam) hv.pinnedCam->setAutoAspectRatio(hv.camAuto);
    hv.pinnedCam = nullptr;
    if (!hv.armedSm) return;
    Ogre::HlmsManager *hm = Ogre::Root::getSingleton().getHlmsManager();
    if (auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr) {
        atom->showSceneDecodes(hv.armedSm, false);
        atom->setDecodeSource(HlmsAtom::DecodeSource());
    }
    hv.armedSm = nullptr;
}

void RayQueryTier::traceRayJobs(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *next) {
    if (!isOpen() || !view || !next) return;
    HitView &hv = mHits[key];
    const bool decode = next->getType() == Ogre::PASS_SCENE &&
                        next->getDefinition()->mIdentifier == kHitDecodePassIdentifier;
    HitListBinding hit;
    if (decode) {
        hv.decodeFrame = frameNow();
        prepareHitList(key, view, next, hit);
    } else {
        hitStandIns(hit);
        hv.live = false;
    }
    // THE OBJECT MOTION FIRST (REFLECT-MOVERS-1; moved here from the raster resolve's pass with PHOTON-7 stage 6's one
    // estimator): `jahSsrVelocity` at every pixel of a slot that moved this frame, which the trace's screen hits read
    // (rq_reflect.comp's screenAnswer) - on a ray chain whose screen sources exist (ChainDesc::screenSources: no
    // capture workspace reaches here, so no job runs twice).
    {
        const ChainDesc cd = view->chainDesc();
        if (cd.rayReflect && cd.screenSources() && cd.atomDraw && !view->stereo()) recordMotion(key, view, next);
    }
    // THE TRACES (their records are what the decode shades). The sun contact rides
    // the same pass: it reads the camera before the next pass re-aspects anything,
    // and its registration waits for photon_resolve.
    recordSunContact(key, view, next);
    recordAreaVis(key, view, next);
    if (view->chainDesc().rayReflect) recordReflect(key, view, next, hit);
    recordGather(key, view, next, hit);
}

void RayQueryTier::resolveRayJobs(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *next) {
    if (!isOpen() || !view || !next) return;
    // DISARMED WHATEVER HAPPENED: a decode pass that threw never reaches its
    // passPosExecute, and an armed HlmsAtom would decode every later pass of the
    // frame in hit mode (measured once: the capture passes compiled hit-mode
    // shaders against prepass state).
    endHitDecode(key);
    HitView &hv = mHits[key];
    if (hv.live) recordHitComposite(key);
    hv.live = false;
    finishReflect(key);
    if (mGather) {
        mGather->finish(key);
        // A DECLINED FRAME OF A GATHERING CHAIN draws the last gather (F3): the pass's ambient is compiled out by the
        // chain's flag, and a pass with no diffuse product would draw no indirect light for the frame
        const Ogre::TextureGpu *t = next->getAnyTargetTexture();
        if (view->chainDesc().probeGather && t) mGather->holdLast(key, t->getWidth(), t->getHeight(), t->getNumSlices());
    }
    finishSunContact(key);
    finishAreaVis(key);
    // THE BLENDED PASSES' ESCAPE LISTS (PHOTON-7 10-4 R4): sized and reset here, outside any render pass, before the
    // blended pass and the refractives append to them
    prepareGlassEscape(key, view, next);
}

/// THE PASS IT SERVES: the pass after `pass` in its node — false when there is nothing
/// to record (the tier closed, a capture workspace: it is handed this frame's products
/// by bindCapturePass, from the listener).
bool RayQueryTier::photonPassTarget(Ogre::CompositorPass *pass, const ReflectPassListener *key, OgreView *view,
                                    Ogre::CompositorPass *&next) const {
    next = nullptr;
    if (!pass || !key || !view || !isOpen()) return false;
    const Ogre::CompositorNode *node = pass->getParentNode();
    if (!node || view->isCaptureWorkspace(node->getWorkspace())) return false;
    const Ogre::CompositorPassVec &passes = node->_getPasses();
    for (size_t i = 0; i + 1 < passes.size(); ++i)
        if (passes[i] == pass) {
            next = passes[i + 1];
            break;
        }
    return next != nullptr;
}

void RayQueryTier::forgetHits(const ReflectPassListener *key) {
    auto it = mHits.find(key);
    if (it == mHits.end()) return;
    endHitDecode(key);
    HitView &hv = it->second;
    for (unsigned i = 0; i < kReflectRing; ++i) {
        retireSet(hv.sets[i], mCompPool);
        hv.sets[i] = VK_NULL_HANDLE;
        retire(hv.params[i]);
    }
    retire(hv.readback);
    if (hv.buf && mRs && mRs->getVaoManager()) mRs->getVaoManager()->destroyUavBuffer(hv.buf);
    hv.buf = nullptr;
    mHits.erase(it);
}

void RayQueryTier::hitStatsInto(const OgreScene *scene, RayQueryStatus &st) const {
    Ogre::HlmsManager *hm = Ogre::Root::getSingletonPtr() ? Ogre::Root::getSingleton().getHlmsManager() : nullptr;
    auto *atom = hm ? dynamic_cast<HlmsAtom *>(hm->getHlms(HlmsAtom::kType)) : nullptr;
    // LIVE VIEWS ONLY: a view whose readings stopped (its decode left the chain, its window closed) keeps its last
    // counters, which name records and drops nobody has now. A view's drops are its OWN list's overflow, so
    // hitDropped == 0 says every live view's list held its own demand.
    // (timed on the scene's draws, OgreScene::drawSerial - never on engine frames the scene missed: PHOTON-7 10-3, F1)
    const uint32_t now = scene ? scene->drawSerial() : 0u, stale = 2u * kFramesInFlight + 2u;
    for (const auto &kv : mHits) {
        const HitView &hv = kv.second;
        if (hv.scene != scene || !hv.everRead || uint32_t(now - hv.readDraw) > stale) continue;
        st.hitRecords += hv.appended;
        st.hitDropped += hv.dropped;
        st.hitCapacity = std::max<unsigned long long>(st.hitCapacity, hv.capacity);
        RayQueryStatus::HitListView v;
        v.eyes = hv.eyes;
        v.eyeWidth = hv.eyeWidth;
        v.eyeHeight = hv.eyeHeight;
        v.firstCapacity = hv.firstCapacity;
        v.capacity = hv.readCapacity;
        v.records = hv.appended;
        v.dropped = hv.dropped;
        st.hitViews.push_back(v);
    }
    if (atom && scene) st.hitDecodeDraws = int(atom->sceneDecodeCount(scene->sceneManager()));
}

bool RayQueryTier::makeCompositePipeline(std::string &err) {
    VkDescriptorSetLayoutBinding b[kHitCompositeBindings] = {};
    const VkDescriptorType types[kHitCompositeBindings] = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 the list's buffer (counters + aux)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 the destinations
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 the decoded radiance
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 the gather's atlas
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 5 the list's records (PHOTON-I-1 fix 5)
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 6 the air's table
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 7 the reflection's sample (PHOTON-2 1b; its hit distance in w)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 8 the reflection's history, this frame's half (the rest mean M)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 9 ...and last frame's (the frozen H, the residual)
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 10 the reflection's state, this frame
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 11 ...and last frame
    };
    for (unsigned i = 0; i < kHitCompositeBindings; ++i) {
        b[i].binding = i;
        b[i].descriptorType = types[i];
        b[i].descriptorCount = 1u;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    // THE LAYOUT AND ITS POOL LINE IN ONE CALL (VkDescriptorPools.h).
    DescriptorPoolPlan plan;
    if (!plan.makeLayout(mVk, b, kHitCompositeBindings, kMaxTimedScenes * kReflectRing, mCompSetLayout, err, "rayquery/hit-composite")) return false;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &mCompSetLayout;
    if (vkCreatePipelineLayout(mVk, &pli, nullptr, &mCompPipeLayout) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreatePipelineLayout failed";
        return false;
    }
    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(krq_hitCompositeSpv);
    smi.pCode = krq_hitCompositeSpv;
    if (vkCreateShaderModule(mVk, &smi, nullptr, &mCompModule) != VK_SUCCESS) {
        err = "rayquery/hit: vkCreateShaderModule failed";
        return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mCompModule;
    cpi.stage.pName = "main";
    cpi.layout = mCompPipeLayout;
    if (createComputePipeline(cpi, &mCompPipeline, "hit composite") != VK_SUCCESS) {
        err = "rayquery/hit: vkCreateComputePipelines failed";
        return false;
    }
    // The pool its layout planned (VkDescriptorPools.h).
    if (!plan.create(mVk, "rayquery/hit-composite", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mCompPool, err))
        return false;
    return ensureSamplers(err);
}

void RayQueryTier::recordHitComposite(const ReflectPassListener *key) {
    auto it = mHits.find(key);
    if (it == mHits.end() || !it->second.live || mCompFailed) return;
    HitView &hv = it->second;
    if (!mCompPipeline) {
        std::string err;
        if (!makeCompositePipeline(err)) {
            mCompFailed = true;
            Ogre::LogManager::getSingleton().logMessage("Jahshaka: the hit write-back is off — " + err);
            return;
        }
    }
    const unsigned ring = hv.frame % kReflectRing;
    std::string err;
    // x = capacity, y = grid width, z/w = the consumers bound; then the reflection's
    // media along its ray (PHOTON-I-1 fix 5, OgreScene::fogAlong).
    struct CompositeParams { float list[4] = {}; float fog[8][4] = {}; };
    if (!hv.params[ring].buffer &&
        !makeBuffer(sizeof(CompositeParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, hv.params[ring], err))
        return;
    if (!hv.sets[ring]) {
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mCompPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &mCompSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, &hv.sets[ring]) != VK_SUCCESS) {
            hv.sets[ring] = VK_NULL_HANDLE;
            return;
        }
    }
    // THE DESTINATIONS: the reflection's sample of THIS frame and the gather's atlas the trace wrote — or stand-ins
    // with the consumer's flag down (its records, if any, are skipped).
    VkImageView atlasView = mHitDummyColour.view;
    VkImageView sampleView = mHitDummyHist.view;   // the rgba16f stand-in shape
    VkImageView histViews[2] = { mHitDummyHist.view, mHitDummyHist.view };
    VkImageView stateViews[2] = { mHitDummyState.view, mHitDummyState.view };
    bool reflectBound = false, gatherBound = false;
    if (auto rit = mReflects.find(key); rit != mReflects.end() && rit->second.finishPending) {
        const ReflectView &rv = rit->second;
        sampleView = rv.sample.view;
        histViews[0] = rv.hist[rv.curIdx].view;  histViews[1] = rv.hist[rv.prevIdx].view;
        stateViews[0] = rv.state[rv.curIdx].view; stateViews[1] = rv.state[rv.prevIdx].view;
        reflectBound = true;
    }
    if (mGather) {
        if (VkImageView a = mGather->tracedAtlas(key)) { atlasView = a; gatherBound = true; }
    }
    CompositeParams pp{};
    pp.list[0] = float(hv.capacity); pp.list[1] = float(hv.width);
    pp.list[2] = reflectBound ? 1.0f : 0.0f; pp.list[3] = gatherBound ? 1.0f : 0.0f;
    Ogre::TextureGpu *fogAerial = nullptr;
    if (hv.scene) hv.scene->fogAlong(pp.fog, fogAerial);
    std::memcpy(hv.params[ring].mapped, &pp, sizeof(pp));

    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView v = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        retireView(v);
        return v;
    };
    VkDescriptorBufferInfo ub{};
    ub.buffer = hv.params[ring].buffer;
    ub.range = sizeof(CompositeParams);
    const VkDescriptorBufferInfo bufInfo = rawBufferInfo(hv.buf);
    VkDescriptorImageInfo sampled[2] = {}, storage{};
    Ogre::TextureGpu *const src[2] = { hv.dest, hv.radiance };
    for (int i = 0; i < 2; ++i) {
        sampled[i].sampler = mPointSampler;
        sampled[i].imageView = sampledView(src[i]);
        sampled[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (!sampled[i].imageView) return;
    }
    storage.imageView = atlasView;
    storage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!storage.imageView) return;
    VkWriteDescriptorSet w[kHitCompositeBindings] = {};
    for (unsigned i = 0; i < kHitCompositeBindings; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = hv.sets[ring];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &ub;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &bufInfo;
    for (int i = 0; i < 2; ++i) {
        w[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2 + i].pImageInfo = &sampled[i];
    }
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[4].pImageInfo = &storage;
    // THE RECORDS (5) and THE AIR'S TABLE (6): the reflection's fog along its ray.
    VkDescriptorImageInfo idsImg{}, fogImg{};
    {
        Ogre::DescriptorSetUav::TextureSlot slot = Ogre::DescriptorSetUav::TextureSlot::makeEmpty();
        slot.texture = hv.ids;
        slot.access = Ogre::ResourceAccess::Read;
        slot.pixelFormat = hv.ids->getPixelFormat();
        idsImg.imageView = static_cast<Ogre::VulkanTextureGpu *>(hv.ids)->createView(slot, false);
        retireView(idsImg.imageView);
        idsImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!idsImg.imageView) return;
    }
    fogImg.sampler = mLinearSampler;
    fogImg.imageView = fogAerial ? sampledView(fogAerial) : mDummyVolume.view;
    fogImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!fogImg.imageView) return;
    w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[5].pImageInfo = &idsImg;
    w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[6].pImageInfo = &fogImg;
    VkDescriptorImageInfo sampleImg{};
    sampleImg.imageView = sampleView;
    sampleImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!sampleImg.imageView) return;
    w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w[7].pImageInfo = &sampleImg;
    // THE REST'S IN-PLACE HALVES (8-11): the record runs' fold - the halves the trace bound this frame
    VkDescriptorImageInfo restImg[4] = {};
    const VkImageView restViews[4] = { histViews[0], histViews[1], stateViews[0], stateViews[1] };
    for (int i = 0; i < 4; ++i) {
        restImg[i].imageView = restViews[i];
        restImg[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!restImg[i].imageView) return;
        w[8 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[8 + i].pImageInfo = &restImg[i];
    }
    vkUpdateDescriptorSets(mVk, kHitCompositeBindings, w, 0, nullptr);

    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        for (Ogre::TextureGpu *t : { hv.dest, hv.radiance })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                     computeStage);
        solver.resolveTransition(trans, hv.ids, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Read,
                                 computeStage);
        if (fogAerial)
            solver.resolveTransition(trans, fogAerial, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        solver.resolveTransition(trans, hv.buf, Ogre::ResourceAccess::Read, computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    // The traces' writes to the mean and the atlas (compute, before the decode
    // pass) are read and rewritten here.
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "hit.composite", mRs);
    work.setUnits(unsigned(hv.appended / 1000ull));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mCompPipeLayout, 0, 1, &hv.sets[ring], 0,
                            nullptr);
    // ONE THREAD PER RECORD SLOT; a thread past this frame's count returns at once.
    {
        const uint32_t groups = std::max(1u, (hv.capacity + 63u) / 64u);
        const uint32_t gx = std::min(groups, 65535u);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, gx, (groups + gx - 1u) / gx, 1u);
    }
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    // THE COUNTERS, on their way to the host (read several frames late).
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
        VkBufferCopy region{};
        region.srcOffset = bufInfo.offset;
        region.dstOffset = VkDeviceSize(hv.frame % kFramesInFlight) * kHitCounterBytes;
        // appended, dropped, the reflection's active resting texels, its CAPPED texels (word 3: the copy stopped at 12
        // bytes, so RayQueryStatus::reflectRestCapped read a word no frame wrote - 0 - until PHOTON-7 stage 6), its
        // batching texels, ... and (PHOTON-7 11-1b/c) the rest bound's two and the NEE's two: every counter word
        region.size = kHitCounterBytes;
        vkCmdCopyBuffer(cmd, bufInfo.buffer, hv.readback.buffer, 1, &region);
        VkMemoryBarrier h{};
        h.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        h.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        h.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &h, 0,
                             nullptr, 0, nullptr);
        HitView::Pending &pd = hv.pending[hv.frame % kFramesInFlight];
        pd.frame = frameNow();
        pd.live = true;
        pd.restTag = hv.restTag;
        pd.listRecords = hv.ids->getWidth() * hv.ids->getHeight();
        hv.restTag = 0u;
    }
    ++hv.frame;
}

// ---------------------------------------------------------------------------
// THE BLENDED PASSES' ESCAPES FOLLOW THE RAY (PHOTON-7 10-4 R4, the lead's design (D); L9) - the ray tier's half. A
// blended or refractive fragment at the ray tier appends its specular lobe's OPEN share (the voxel rays' escapes, which
// are the environment's only past the world) as a record (PhotonVct_piece_ps.any's jahGlassEscapeAppend) into its
// pass's list - the view's, reset in photon_resolve and registered for the pass (FogHlmsListener::setGlassEscape) - and
// lights none of it; after the pass the records are traced on the TLAS from where the chain's read ended, their hits
// read through the hit set (rq_glass_escape.comp, the records jobs' one text), and every pixel's fixed-point sum added
// into the pass's colour target (rq_glass_escape_add.comp) - the same frame. THE DEMAND LAW (the hit list's,
// kHitListWidth): a list holds a quarter of the target's pixels at first (the refractives' list 64k records); a retired
// frame that appended more than its list held grows the list to that demand, and a record a full list cannot hold is
// lit by the environment as before - counted (RayQueryStatus::escapeDropped), never a silent cap.
namespace {
/// The trace's set: the hit set at 0 and 5..36, its own four (the parameters, the list, the pixels' sums, the counts)
/// and the air's table at 37.
constexpr unsigned kEscapeBindings = 38u;
static_assert(kEscapeBindings == kHitSetBindings + 5u, "the escapes' set is the hit set, its own four and the air");
constexpr unsigned kEscapeAddBindings = 3u;
/// rq_glass_escape.comp's kCountWords, and the list's header (eight words: the count, the trace's groups, the add's)
constexpr unsigned kEscapeCounts = 9u;
static_assert(kEscapeCounts * 4u + 4u <= 64u, "the readback slot holds the record count and the counts");
constexpr VkDeviceSize kEscapeHeaderBytes = 32u;
constexpr VkDeviceSize kEscapeRecordBytes = 32u;   ///< two uvec4 (rq_glass_escape.comp's head)
/// The pixels' sums' fixed point: 2^16 a unit of radiance - a hit's term is at most kMaxRadiance (1024) x a weight at most
/// 1, so a pixel holds 64 such terms in 32 bits, and past them (or a miss's unbounded environment) the word SATURATES at
/// 65536, counted (escapeSaturated; the one read's F11); its quantum 1.5e-5 is far under one code at any exposure.
constexpr float kEscapeFixedScale = 65536.0f;
/// The lists' first sizes: the blended pass's a quarter of the target's pixels, the refractives' 64k records.
constexpr unsigned kEscapeMinRecords = 65536u;
/// The readback: per frame in flight and list, 16 words (the header's count, then the job's counts).
constexpr VkDeviceSize kEscapeReadbackSlot = 64u;
struct GlassEscapeParams {
    HitSetParams hit;       ///< THE HIT SET'S UNIFORMS (hitSetParams; rq_glass_escape.comp's first member)
    float knobs[4] = {};    ///< the capacity, the near length, the far query's start and end
    float knobs2[4] = {};   ///< one eye's width, its height, the eyes, the fixed-point scale
    float knobs3[4] = {};   ///< the frame, the pixel's angle, the ray mask
    float fog[8][4] = {};   ///< OgreScene::fogAlong
    float eye[2][4] = {};   ///< each eye's position (world): the panes in front of a record's fragment (F5)
};
struct GlassEscapeAddParams {
    float knobs[4] = {};    ///< one eye's width, its height, the eyes, 1 / the fixed-point scale
};
}   // namespace

bool RayQueryTier::makeGlassEscapePipelinesNow(std::string &err) {
    if (!ensureSamplers(err)) return false;
    DescriptorPoolPlan plan;
    const uint32_t sets = kMaxTimedScenes * kReflectRing * 2u;
    {
        VkDescriptorSetLayoutBinding b[kEscapeBindings] = {};
        hitSetLayout(kCardGatherHitSlots, b);
        const VkDescriptorType own[4] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   // 1 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 2 the list
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 3 the pixels' sums
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 4 the counts
        };
        for (unsigned i = 0; i < 4u; ++i) {
            b[1u + i].binding = 1u + i;
            b[1u + i].descriptorType = own[i];
            b[1u + i].descriptorCount = 1u;
            b[1u + i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        b[37].binding = 37u;   // the air's table
        b[37].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[37].descriptorCount = 1u;
        b[37].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        if (!plan.makeLayout(mVk, b, kEscapeBindings, sets, mEscSetLayout, err, "rayquery/glass-escape")) return false;
    }
    {
        VkDescriptorSetLayoutBinding b[kEscapeAddBindings] = {};
        const VkDescriptorType own[kEscapeAddBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,   // 0 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,   // 1 the pixels' sums
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,    // 2 the pass's colour target
        };
        for (unsigned i = 0; i < kEscapeAddBindings; ++i) {
            b[i].binding = i;
            b[i].descriptorType = own[i];
            b[i].descriptorCount = 1u;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        if (!plan.makeLayout(mVk, b, kEscapeAddBindings, sets, mEscAddSetLayout, err, "rayquery/glass-escape-add"))
            return false;
    }
    const auto pipeline = [&](VkDescriptorSetLayout set, const uint32_t *code, size_t bytes, VkPipelineLayout &layout,
                              VkShaderModule &module, VkPipeline &out, const char *name) {
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set;
        if (vkCreatePipelineLayout(mVk, &pli, nullptr, &layout) != VK_SUCCESS) {
            err = "vkCreatePipelineLayout failed";
            return false;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = bytes;
        smi.pCode = code;
        if (vkCreateShaderModule(mVk, &smi, nullptr, &module) != VK_SUCCESS) {
            err = "vkCreateShaderModule failed (the build-time SPIR-V is not loadable)";
            return false;
        }
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = layout;
        if (createComputePipeline(cpi, &out, name) != VK_SUCCESS) {
            err = "vkCreateComputePipelines failed";
            return false;
        }
        return true;
    };
    if (!pipeline(mEscSetLayout, krq_glassEscapeSpv, sizeof(krq_glassEscapeSpv), mEscPipeLayout, mEscModule,
                  mEscPipeline, "glass escape") ||
        !pipeline(mEscAddSetLayout, krq_glassEscapeAddSpv, sizeof(krq_glassEscapeAddSpv), mEscAddPipeLayout,
                  mEscAddModule, mEscAddPipeline, "glass escape add"))
        return false;
    return plan.create(mVk, "rayquery/glass-escape", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mEscPool, err);
}

void RayQueryTier::readGlassEscape(EscapeView &ev) {
    if (!ev.readback.mapped) return;
    const uint32_t now = frameNow(), inFlight = framesInFlight();
    int newest = -1;
    uint32_t newestFrame = 0u;
    for (unsigned f = 0; f < kFramesInFlight; ++f)
        for (unsigned l = 0; l < 2u; ++l) {
            EscapeView::Pending &pd = ev.pending[f][l];
            if (!pd.live || uint32_t(now - pd.frame) < inFlight) continue;
            const uint32_t *w = reinterpret_cast<const uint32_t *>(static_cast<const unsigned char *>(ev.readback.mapped) +
                                                                   (f * 2u + l) * kEscapeReadbackSlot);
            // EVERY retired frame's demand, the newest or not (the list grows to hold it)
            ev.demand[l] = std::max<unsigned long long>(ev.demand[l], w[0]);
            if (newest < 0 || int32_t(pd.frame - newestFrame) > 0) {
                newest = int(f);
                newestFrame = pd.frame;
            }
        }
    if (newest < 0) return;
    ev.records = ev.dropped = ev.capacity = 0ull;
    for (unsigned long long &c : ev.counts) c = 0ull;
    for (unsigned l = 0; l < 2u; ++l) {
        EscapeView::Pending &pd = ev.pending[unsigned(newest)][l];
        if (!pd.live || pd.frame != newestFrame) continue;
        const uint32_t *w = reinterpret_cast<const uint32_t *>(static_cast<const unsigned char *>(ev.readback.mapped) +
                                                               (unsigned(newest) * 2u + l) * kEscapeReadbackSlot);
        ev.records += w[0];
        ev.dropped += w[0] > pd.capacity ? w[0] - pd.capacity : 0u;
        ev.capacity += pd.capacity;
        for (unsigned k = 0; k < kEscapeCounts; ++k) ev.counts[k] += w[1u + k];
    }
    ev.everRead = true;
    ev.readDraw = ev.scene ? const_cast<OgreScene *>(ev.scene)->drawSerial() : 0u;
    for (unsigned f = 0; f < kFramesInFlight; ++f)
        for (unsigned l = 0; l < 2u; ++l)
            if (ev.pending[f][l].live && uint32_t(now - ev.pending[f][l].frame) >= inFlight) ev.pending[f][l].live = false;
}

void RayQueryTier::prepareGlassEscape(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *next) {
    auto known = mEscape.find(key);
    if (known != mEscape.end())
        for (EscapeList &l : known->second.lists) l.ready = false;
    if (!isOpen() || !view || !next || mEscFailed) return;
    const ChainDesc cd = view->chainDesc();
    const bool floatScene = cd.hdr || cd.hdrReadback || cd.displayEncode;
    OgreScene *scene = view->ogreScene();
    const Ogre::TextureGpu *target = next->getAnyTargetTexture();
    if (!scene || !target || !chainGlassEscape(cd.rayReflect, cd.probeGather, floatScene) ||
        target->getPixelFormat() != Ogre::PFG_RGBA16_FLOAT)
        return;
    // ONLY WHERE A RECORD CAN BE MADE: a scene with no blended material appends none and holds no list (the pixels'
    // sums are 12 bytes a pixel). Its pipelines were built ahead at the tier's open (startAhead).
    if (!scene->hasBlended()) return;
    joinAhead(kAheadGlassEscape);   // (the worker writes mEscPipeline: joined before it is read - the read's F10)
    std::string err;
    if (!mEscPipeline && !makeGlassEscapePipelines(err)) {
        mEscFailed = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the blended passes' traced escapes are off: " + err +
            " (their open shares are lit by the environment)");
        return;
    }
    EscapeView &ev = mEscape[key];
    ev.scene = scene;
    readGlassEscape(ev);
    const unsigned w = target->getWidth(), h = target->getHeight(),
                   eyes = std::max<unsigned>(1u, target->getNumSlices());
    // THE PIXELS' SUMS, the target's size (zeroed when made; the add zeroes what it reads)
    bool zeroAccum = false;
    if (ev.width != w || ev.height != h || ev.eyes != eyes || !ev.accum.buffer) {
        retire(ev.accum);
        if (!makeBuffer(VkDeviceSize(w) * h * eyes * 3u * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, false, ev.accum,
                        err))
            return;
        ev.width = w;
        ev.height = h;
        ev.eyes = eyes;
        zeroAccum = true;
    }
    if (!ev.readback.buffer &&
        !makeBuffer(kEscapeReadbackSlot * kFramesInFlight * 2u, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, false,
                    ev.readback, err))
        return;
    // THE LISTS, by the demand law
    for (unsigned li = 0; li < 2u; ++li) {
        EscapeList &l = ev.lists[li];
        unsigned want = l.capacity;
        if (!want) {
            const unsigned long long share = li == 0u ? (unsigned long long)w * h * eyes / 4ull : 0ull;
            want = unsigned(std::max<unsigned long long>(kEscapeMinRecords, (share + 63ull) / 64ull * 64ull));
        }
        while (want < ev.demand[li] && want < (1u << 30)) want *= 2u;
        if (!l.list.buffer || l.capacity != want) {
            retire(l.list);
            l.address = 0;
            l.capacity = 0u;
            if (!makeBuffer(kEscapeHeaderBytes + VkDeviceSize(want) * kEscapeRecordBytes,
                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                            false, true, l.list, err))
                return;
            l.address = addressOf(l.list.buffer);
            l.capacity = want;
        }
        if (!l.counts.buffer &&
            !makeBuffer(kEscapeCounts * sizeof(uint32_t),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        false, false, l.counts, err))
            return;
    }
    mDev->_jahRecordQueue().endAllEncoders();
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    {
        // after every earlier reader and writer (the last frame's fragments, traces, adds and copies)
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
    }
    if (zeroAccum) vkCmdFillBuffer(cmd, ev.accum.buffer, 0, VK_WHOLE_SIZE, 0u);
    for (EscapeList &l : ev.lists) {
        // the header: no record, no trace group (y, z 1), no add group until a record arms it (y, z the target's)
        const uint32_t header[8] = { 0u, 0u, 1u, 1u, 0u, (h + 7u) / 8u, eyes, (w + 7u) / 8u };
        vkCmdUpdateBuffer(cmd, l.list.buffer, 0, sizeof(header), header);
        vkCmdFillBuffer(cmd, l.counts.buffer, 0, VK_WHOLE_SIZE, 0u);
        l.ready = true;
    }
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    ++ev.frame;
}

void RayQueryTier::bindGlassEscape(const ReflectPassListener *key, OgreView *view, unsigned which) {
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    if (!scene || which > 1u) return;
    const ChainDesc cd = view->chainDesc();
    if (!chainGlassEscape(cd.rayReflect, cd.probeGather, cd.hdr || cd.hdrReadback || cd.displayEncode)) return;
    // THE PASS IS AN ESCAPE PASS whenever the chain carries them (its permutation follows that alone); the list's
    // address only when it was reset this frame and the scene's structure is there to trace it
    const auto it = mEscape.find(key);
    const EscapeList *l = it != mEscape.end() ? &it->second.lists[which] : nullptr;
    const SceneAs *sa = asOf(scene);
    const bool live = l && l->ready && sa && sa->tlas && sa->st.enabled && sa->instanceCount > 0u &&
                      !arm(ArmId::GlassEscapeOff, false);   // (the test door: no list, the same permutation)
    FogHlmsListener::setGlassEscape(scene->sceneManager(), true, live ? l->address : 0ull, live ? l->capacity : 0u);
}

void RayQueryTier::traceGlassEscape(const ReflectPassListener *key, OgreView *view, Ogre::CompositorPass *pass,
                                    unsigned which) {
    OgreScene *scene = view ? view->ogreScene() : nullptr;
    if (scene) FogHlmsListener::setGlassEscape(scene->sceneManager(), false, 0ull, 0u);
    auto it = mEscape.find(key);
    if (it == mEscape.end() || !scene || !pass || which > 1u || !isOpen()) return;
    joinAhead(kAheadGlassEscape);
    EscapeView &ev = it->second;
    EscapeList &l = ev.lists[which];
    if (!l.ready) return;
    l.ready = false;
    SceneAs *saP = asOf(scene);
    Ogre::Camera *cam = view->camera();
    // (the target the add writes: the pass's own colour attachment)
    Ogre::TextureGpu *target = const_cast<Ogre::TextureGpu *>(pass->getAnyTargetTexture());
    // (the same test as the registration's: a list the pass could append to is traced, every time)
    if (!saP || !saP->tlas || !saP->st.enabled || saP->instanceCount == 0u || !cam || !target ||
        target->getPixelFormat() != Ogre::PFG_RGBA16_FLOAT)
        return;
    SceneAs &sa = *saP;
    const unsigned ring = ev.frame % kReflectRing;
    std::string err;
    if ((!l.params[ring].buffer &&
         !makeBuffer(sizeof(GlassEscapeParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true, false, l.params[ring], err)) ||
        (!l.addParams[ring].buffer && !makeBuffer(sizeof(GlassEscapeAddParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                  true, false, l.addParams[ring], err)))
        return;
    for (VkDescriptorSet *set : { &l.sets[ring], &l.addSets[ring] }) {
        if (*set) continue;
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mEscPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = set == &l.sets[ring] ? &mEscSetLayout : &mEscAddSetLayout;
        if (vkAllocateDescriptorSets(mVk, &dai, set) != VK_SUCCESS) {
            *set = VK_NULL_HANDLE;
            return;
        }
    }

    // ---- THE HIT SET (the one collection) with no hit list (a record appends nothing), the GPU scene's instance table
    // and THE HIT'S SUN - the pass's (its shadow node's first directional caster), the casters, the frame's far plane:
    // the hit list's own (prepareHitList)
    HitSetSources hs;
    hitStandIns(hs.hit);
    detail::GpuScene &gs = scene->gpuScene();
    Ogre::UavBufferPacked *instances = gs.live() ? gs.instanceBuffer() : nullptr;
    if (instances) {
        auto *bi = static_cast<Ogre::VulkanBufferInterface *>(instances->getBufferInterface());
        hs.hit.instances = bi->getVboName();
        hs.hit.instancesOffset = VkDeviceSize(instances->_getFinalBufferStart()) * instances->getBytesPerElement();
        hs.hit.instancesRange = instances->getTotalSizeBytes();
        hs.hit.instanceEntries = gs.slotCount();
    }
    {
        Ogre::Vector3 toSun;
        hs.hit.sun = passSun(pass, toSun);
        hs.hit.toSun[0] = toSun.x; hs.hit.toSun[1] = toSun.y; hs.hit.toSun[2] = toSun.z;
        hs.hit.sunMask = kRayMaskCaster;
        const float farClip = float(cam->getFarClipDistance());
        hs.hit.sunRange = farClip > 0.0f ? std::max(farClip, 100.0f) : 10000.0f;
        hs.hit.farLift = sa.farOverlap;
    }
    hitSetOf(scene, sa, hs);
    uint32_t geomSlots = hs.geomRows && hs.geomRowOfSlot ? uint32_t(hs.geomRowOfSlot->size()) : 0u;
    if (geomSlots) {
        RawBuffer &rb = l.rowOfSlot[ring];
        const VkDeviceSize want = VkDeviceSize(geomSlots) * sizeof(uint32_t);
        if (rb.buffer && rb.size < want) retire(rb);
        if (!rb.buffer && !makeBuffer(std::max<VkDeviceSize>(want, 1024u), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                      false, rb, err))
            geomSlots = 0u;
        else
            std::memcpy(rb.mapped, hs.geomRowOfSlot->data(), size_t(want));
    }

    // ---- THE PARAMETERS: the hit set's (a VIEW reader: the GI camera's level), the list, the reach rule's near length
    // and the far copies' continuation (the reflection's: recordReflect), the target, the media
    GlassEscapeParams pp{};
    hitSetParams(hs, geomSlots, pp.hit, HitReader::View);
    const unsigned voxCount = hs.cascadeCount;
    const float nearLen = detail::photonRayReach(
        voxCount ? Ogre::Vector3(hs.voxelSize[voxCount - 1u][0], hs.voxelSize[voxCount - 1u][1],
                                 hs.voxelSize[voxCount - 1u][2]).length()
                 : 0.0f,
        cam->getFarClipDistance());
    const float farPlane = cam->getFarClipDistance() > 0.0f ? float(cam->getFarClipDistance()) : 1000.0f;
    pp.knobs[0] = float(l.capacity);
    pp.knobs[1] = nearLen;
    pp.knobs[2] = std::max(0.001f, nearLen - std::max(0.0f, sa.farOverlap));
    pp.knobs[3] = farPlane > nearLen ? farPlane : 0.0f;
    pp.knobs2[0] = float(ev.width);
    pp.knobs2[1] = float(ev.height);
    pp.knobs2[2] = float(ev.eyes);
    pp.knobs2[3] = kEscapeFixedScale;
    pp.knobs3[0] = float(frameNow() & 0xFFFFu);
    pp.knobs3[1] = 2.0f * std::tan(0.5f * cam->getFOVy().valueRadians()) / float(std::max(ev.height, 1u));
    pp.knobs3[2] = float(kRayMaskNearField | kRayMaskFar);
    Ogre::TextureGpu *fogAerial = nullptr;
    scene->fogAlong(pp.fog, fogAerial);
    // THE EYES (the panes in front of a record's fragment, F5): a stereo view's located eyes, else the camera
    {
        const StereoEyeBasis *eyes = view->stereo() ? view->stereoEyes() : nullptr;
        const Ogre::Vector3 c = cam->getDerivedPosition();
        for (int e = 0; e < 2; ++e) {
            const Ogre::Vector3 at = eyes ? Ogre::Vector3(eyes[e].position) : c;
            pp.eye[e][0] = float(at.x);
            pp.eye[e][1] = float(at.y);
            pp.eye[e][2] = float(at.z);
        }
    }
    std::memcpy(l.params[ring].mapped, &pp, sizeof(pp));
    GlassEscapeAddParams ap{};
    ap.knobs[0] = float(ev.width);
    ap.knobs[1] = float(ev.height);
    ap.knobs[2] = float(ev.eyes);
    ap.knobs[3] = 1.0f / kEscapeFixedScale;
    std::memcpy(l.addParams[ring].mapped, &ap, sizeof(ap));

    // ---- THE SETS: the trace's own four, the hit set through its one writer, the air; the add's three
    VkWriteDescriptorSet w[kEscapeBindings] = {};
    HitSetWrite hitSet;
    if (!hitSet.write(*this, l.sets[ring], kCardGatherHitSlots, hs, geomSlots ? l.rowOfSlot[ring].buffer : VK_NULL_HANDLE,
                      w, err))
        return;
    VkDescriptorBufferInfo ub{}, lb{}, sb{}, cb{}, aub{};
    ub.buffer = l.params[ring].buffer;
    ub.range = sizeof(GlassEscapeParams);
    lb.buffer = l.list.buffer;
    lb.range = VK_WHOLE_SIZE;
    sb.buffer = ev.accum.buffer;
    sb.range = VK_WHOLE_SIZE;
    cb.buffer = l.counts.buffer;
    cb.range = VK_WHOLE_SIZE;
    aub.buffer = l.addParams[ring].buffer;
    aub.range = sizeof(GlassEscapeAddParams);
    VkDescriptorImageInfo fogImg{}, targetImg{};
    fogImg.sampler = mLinearSampler;
    if (fogAerial) {
        Ogre::DescriptorSetTexture2::TextureSlot slot = Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = fogAerial;
        fogImg.imageView = static_cast<Ogre::VulkanTextureGpu *>(fogAerial)->createView(slot, false);
        retireView(fogImg.imageView);
    } else {
        fogImg.imageView = mDummyVolume.view;
    }
    fogImg.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    targetImg.imageView = layerView(target, true);
    targetImg.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!fogImg.imageView || !targetImg.imageView) return;
    const auto own = [&](VkWriteDescriptorSet &e, VkDescriptorSet set, uint32_t binding,
                         VkDescriptorType type) -> VkWriteDescriptorSet & {
        e.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        e.dstSet = set;
        e.dstBinding = binding;
        e.descriptorCount = 1;
        e.descriptorType = type;
        return e;
    };
    own(w[1], l.sets[ring], 1u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &ub;
    own(w[2], l.sets[ring], 2u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &lb;
    own(w[3], l.sets[ring], 3u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &sb;
    own(w[4], l.sets[ring], 4u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &cb;
    own(w[37], l.sets[ring], 37u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER).pImageInfo = &fogImg;
    vkUpdateDescriptorSets(mVk, kEscapeBindings, w, 0, nullptr);
    VkWriteDescriptorSet aw[kEscapeAddBindings] = {};
    own(aw[0], l.addSets[ring], 0u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER).pBufferInfo = &aub;
    own(aw[1], l.addSets[ring], 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &sb;
    own(aw[2], l.addSets[ring], 2u, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &targetImg;
    vkUpdateDescriptorSets(mVk, kEscapeAddBindings, aw, 0, nullptr);

    // ---- THE LAYOUTS through Ogre's solver (the pass's render encoder closed first): the hit set read, the air read,
    // the pass's colour target read and written by the add - the next pass that reads it is ordered after by its own
    // barrier analysis
    mDev->_jahRecordQueue().endAllEncoders();
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = mRs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        hitSetTransitions(solver, trans, hs);
        if (instances) solver.resolveTransition(trans, instances, Ogre::ResourceAccess::Read, computeStage);
        if (fogAerial)
            solver.resolveTransition(trans, fogAerial, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                     computeStage);
        solver.resolveTransition(trans, target, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::ReadWrite,
                                 computeStage);
        mRs->executeResourceTransition(trans);
    }
    VkCommandBuffer cmd = frameCmd();
    if (!cmd) return;
    clearDummyImages(cmd);
    initHitDummies(cmd);
    {
        // the pass's fragments' appends, read by the trace and its indirect arguments
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0,
                                         which == 0u ? "escape.blended" : "escape.refractives", mRs, scene);
        work.setUnits(unsigned(std::min<unsigned long long>(ev.records, 0xFFFFFFFFull)));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mEscPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mEscPipeLayout, 0, 1, &l.sets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatchIndirect(cmd, l.list.buffer, 4u);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b,
                             0, nullptr, 0, nullptr);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mEscAddPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mEscAddPipeLayout, 0, 1, &l.addSets[ring], 0,
                                nullptr);
        JAH_GPU_CHECKPOINT(cmd);
        vkCmdDispatchIndirect(cmd, l.list.buffer, 16u);
    }
    {
        // THE COUNTS on their way to the host (read several frames late): the records appended, the job's counts
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
        const unsigned slot = ev.frame % kFramesInFlight;
        const VkDeviceSize base = (VkDeviceSize(slot) * 2u + which) * kEscapeReadbackSlot;
        VkBufferCopy head{}, counts{};
        head.dstOffset = base;
        head.size = sizeof(uint32_t);
        counts.dstOffset = base + sizeof(uint32_t);
        counts.size = kEscapeCounts * sizeof(uint32_t);
        vkCmdCopyBuffer(cmd, l.list.buffer, ev.readback.buffer, 1, &head);
        vkCmdCopyBuffer(cmd, l.counts.buffer, ev.readback.buffer, 1, &counts);
        VkMemoryBarrier h{};
        h.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        h.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        h.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &h, 0, nullptr, 0,
                             nullptr);
        EscapeView::Pending &pd = ev.pending[slot][which];
        pd.frame = frameNow();
        pd.live = true;
        pd.capacity = l.capacity;
    }
}

void RayQueryTier::forgetGlassEscape(const ReflectPassListener *key) {
    auto it = mEscape.find(key);
    if (it == mEscape.end()) return;
    joinAhead(kAheadGlassEscape);   // (mEscPool is the ahead build's)
    EscapeView &ev = it->second;
    retire(ev.accum);
    retire(ev.readback);
    for (EscapeList &l : ev.lists) {
        retire(l.list);
        retire(l.counts);
        for (unsigned i = 0; i < kReflectRing; ++i) {
            retireSet(l.sets[i], mEscPool);
            retireSet(l.addSets[i], mEscPool);
            retire(l.params[i]);
            retire(l.addParams[i]);
            retire(l.rowOfSlot[i]);
        }
    }
    mEscape.erase(it);
}

void RayQueryTier::glassEscapeStatsInto(const OgreScene *scene, RayQueryStatus &st) const {
    // LIVE VIEWS ONLY (the hit list's rule): a view whose readings stopped keeps counters nobody has now
    const uint32_t now = scene ? const_cast<OgreScene *>(scene)->drawSerial() : 0u, stale = 2u * kFramesInFlight + 2u;
    for (const auto &kv : mEscape) {
        const EscapeView &ev = kv.second;
        if (ev.scene != scene || !ev.everRead || uint32_t(now - ev.readDraw) > stale) continue;
        st.escapeRecords += ev.records;
        st.escapeDropped += ev.dropped;
        st.escapeCapacity = std::max<unsigned long long>(st.escapeCapacity, ev.capacity);
        st.escapeTraced += ev.counts[0];
        st.escapeCard += ev.counts[1];
        st.escapeVoxel += ev.counts[2];
        st.escapeDirect += ev.counts[3];
        st.escapeFar += ev.counts[4];
        st.escapeSky += ev.counts[5];
        st.escapeBack += ev.counts[6];
        st.escapeNoSample += ev.counts[7];
        st.escapeSaturated += ev.counts[8];
    }
}

// ---------------------------------------------------------------------------
// THE ENGINE SIDE. Three methods and one frame hook; everything above is
// private to this TU.
void OgreEngine::setRayTracing(bool on) {
    // THE HOST'S CALL IS A MESSAGE (U3 stage 4a, decision 39), applied at the drain; JAH_HOST_CALL counts a direct one.
    if (hostPosts()) mHostRayTracing = on;   // the host's record, at the call
    if (postedAsMessage([=](OgreEngine &e) { e.setRayTracing(on); return true; })) return;
    JAH_HOST_CALL("Engine::setRayTracing");
    if (mRayTracingWanted == on) return;
    mRayTracingWanted = on;
    mHostRayTracing = on;   // the host's record (Engine::rayTracing)
    if (!on) shutdownRayQuery();
    Ogre::LogManager::getSingleton().logMessage(
        std::string("Jahshaka: hardware ray tracing switched ") + (on ? "ON" : "OFF") +
        " (the no-rays switch)");
}

// THE TIER'S STORAGE FORMATS, ASKED OF THE DEVICE (RAY-FMT-CHECK, PHOTON P3).
//
// Every image the tier writes from a compute shader is a STORAGE image: the
// reflection's temporal pair (the radiance mean, RGBA16F, and the state texel,
// RG32UI — `ensureReflectImages`) and the gather's atlas (RGBA16F). BOTH ARE
// CORE-MANDATORY STORAGE FORMATS in Vulkan 1.0 (the spec's Required Format
// Support; shaderStorageImageExtendedFormats covers the R16G16*, R16*, R8*,
// A2B10G10R10 and B10G11R11 family, not these), so on a conformant driver this
// check never refuses. It is kept as a DRIVER-DEFECT GUARD — one query per
// device, one log line — because the alternative on a driver that got the table
// wrong is `makeStorageImage` creating an image the device cannot store to and
// the first dispatch being undefined behaviour rather than a refusal. A device
// that lacks one is a no-rays device, and every consumer (the chain's
// rayReflect, the cards' Auto, the status) reads that same answer.
//
// JAHSHAKA_RAY_DENY_STORAGE_FORMAT names a format (R16G16B16A16_SFLOAT or
// R32G32_UINT) to treat as unsupported: FAULT INJECTION, the refusal path's
// only door on conformant hardware (a measurement switch for
// gi.rt_reflect_format_refused_lavapipe, not a mode).
namespace {
struct RayStorageFormat { VkFormat format{}; const char *name{}; };
constexpr RayStorageFormat kRayStorageFormats[] = {
    { VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT" },   // the reflection mean, the gather atlas
    { VK_FORMAT_R32G32_UINT,         "R32G32_UINT" },           // the reflection's state texel
};

/// Empty when every format stores; otherwise the first one that does not.
std::string rayStorageFormatRefused(VkPhysicalDevice pd) {
    const char *deny = std::getenv("JAHSHAKA_RAY_DENY_STORAGE_FORMAT");
    for (const RayStorageFormat &f : kRayStorageFormats) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(pd, f.format, &props);
        const bool stores = (props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        const bool denied = deny && std::strcmp(deny, f.name) == 0;
        if (!stores || denied) return std::string(f.name) + (denied ? " (denied by JAHSHAKA_RAY_DENY_STORAGE_FORMAT)" : "");
    }
    return std::string();
}
}   // namespace

bool OgreEngine::deviceRayQuery() const {
    if (mHeadless || !mRoot) return false;
    Ogre::VulkanRenderSystem *rs = dynamic_cast<Ogre::VulkanRenderSystem *>(mRoot->getRenderSystem());
    Ogre::VulkanDevice *dev = rs ? rs->getVulkanDevice() : nullptr;
    if (!dev || !dev->hasRayQuery() || !dev->mPhysicalDevice) return false;
    // Asked ONCE per physical device: the answer is a property of the device,
    // and this predicate is read every time a view describes its chain.
    static VkPhysicalDevice sAsked = VK_NULL_HANDLE;
    static bool sStores = false;
    if (sAsked != dev->mPhysicalDevice) {
        sAsked = dev->mPhysicalDevice;
        const std::string refused = rayStorageFormatRefused(dev->mPhysicalDevice);
        sStores = refused.empty();
        if (!sStores)
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: ray-query tier refused - the device cannot store to " + refused +
                " (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, optimal tiling), which the tier's "
                "history images need; this is a no-rays device");
    }
    return sStores;
}

void OgreEngine::ensureRayTier() {
    if (!mRayTracingWanted || mHeadless || !mRoot || mRayTier) return;
    if (!rayQueryAvailable()) return;             // the fallback picture, silently
    mRayTier = new detail::RayQueryTier();
    mRayTier->setArms(&mArms);
    std::string err;
    if (!mRayTier->open(mRoot->getRenderSystem(), err)) {
        Ogre::LogManager::getSingleton().logMessage("Jahshaka: ray-query tier off — " + err);
        delete mRayTier;
        mRayTier = nullptr;
    }
}

void OgreEngine::updateRayQuery(const std::vector<OgreScene *> &drawn) {
    // (every scene's lamp rays off first: the traced ones are switched on below, after their TLAS is current)
    for (auto &s : mScenes) FogHlmsListener::setLampRays(s->sceneManager(), false, 0ull, 0ull);
    if (!mRayTracingWanted || mHeadless || !mRoot) return;
    ensureRayTier();
    if (!mRayTier) return;
    // A scene whose project row says OFF must cost nothing, not just look the
    // same (world.rayTracing's contract): no gather, no BLAS/TLAS for it. The
    // structures it may already hold were released when the row flipped
    // (OgreScene::setRayTracing → forgetRayQuery).
    // GATHER-0: A BACKSTOP, and only that since the fix round (D2). The
    // registration is PASS-scoped — `ReflectPassListener::passPosExecute`
    // takes it away as its own pass ends — so by the time a frame starts the
    // map is already empty. This line stays because the map is process-wide
    // state and a frame that begins with it non-empty is a bug this costs
    // nothing to be immune to. NOTE it is NOT the teardown path: the early
    // return above (the no-rays switch) skips it, which is why
    // `RayQueryTier::close()` clears it too (D1).
    // ...and the sun contact's registration, the same backstop (PHOTON-RAYS-1). Each scene
    // holds its own (FogHlmsListener::SceneBinds), so the backstop visits every scene.
    for (auto &s : mScenes) FogHlmsListener::clearPassScoped(s->passRecord());
    for (OgreScene *s : drawn) {
        if (!s->rayTracingResolved()) continue;
        mRayTier->updateScene(s);
        // THE FORWARD+ LAMPS' SHADOW RAYS (PHOTON-7 10-4 R9): this frame's TLAS, after its build (the property follows
        // the scene's tracing, the address may be 0)
        FogHlmsListener::setLampRays(s->sceneManager(), true, mRayTier->tlasAddressOf(s), mRayTier->alphaTableOf(s));
    }
}

/// THE CHANGED-REGION LIST'S MOVERS (PHOTON-7 stage 3), after the GPU scene's commit and the skin pass
/// (applyShadowCacheDirties): every slot the frame's update staged (a transform write, a flags change, a birth) and every
/// pose the skin pass moved, ONCE, as its SWEPT box - its world bounds this frame joined with last frame's (sweptBoundsOf,
/// the reflection's own rule) - with its shadow when it casts (OgreScene::listGiMover).
void OgreEngine::listRayMovers(const std::vector<OgreScene *> &drawn) {
    if (!mRoot || !mRoot->getRenderSystem() || !mRoot->getRenderSystem()->getVaoManager()) return;
    const uint32_t vaoFrame = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
    for (OgreScene *s : drawn) {
        const detail::GpuScene &gs = s->gpuScene();
        if (!gs.live()) continue;
        const detail::GpuInstance *mirror = gs.mirrorData();
        const auto list = [&](uint32_t slot) {
            if (slot >= gs.slotCount()) return;
            const detail::GpuInstance &g = mirror[slot];
            float lo[3], hi[3];
            sweptBoundsOf(g, lo, hi);
            uint32_t flags;
            std::memcpy(&flags, &g.boundsMax[3], sizeof(flags));
            s->listGiMover(Ogre::Aabb::newFromExtents(Ogre::Vector3(lo[0], lo[1], lo[2]), Ogre::Vector3(hi[0], hi[1], hi[2])),
                           flags);
        };
        std::vector<uint32_t> moved;
        if (gs.movedThisFrame(vaoFrame)) moved = gs.movedSlots();
        // A SLOT A SWAP-REMOVE RENUMBERED, UNCHANGED (PHOTON-7 stage 4's read F2): the tail's object moved in the index,
        // not in the world - its entry still holds the world and flags it moved with, and world = prevWorld - so it lists
        // nothing (the removal's own box is listed by OgreScene::removeItemSlot).
        const auto compactedOnly = [&](uint32_t slot) {
            if (!gs.movedThisFrame(vaoFrame) || slot >= gs.slotCount()) return false;
            const detail::GpuInstance &g = mirror[slot];
            uint32_t flags;
            std::memcpy(&flags, &g.boundsMax[3], sizeof(flags));
            for (const detail::GpuScene::CompactedSlot &c : gs.compactedSlots())
                if (c.slot == slot && c.flags == flags && std::memcmp(c.world, g.world, sizeof(c.world)) == 0 &&
                    std::memcmp(g.world, g.prevWorld, sizeof(g.world)) == 0)
                    return true;
            return false;
        };
        std::vector<uint32_t> listed;
        for (uint32_t slot : moved)
            if (!compactedOnly(slot)) { list(slot); listed.push_back(slot); }
        if (const std::vector<uint32_t> *posed = detail::RayQueryTier::posedSlotsOf(s, vaoFrame)) {
            std::sort(listed.begin(), listed.end());
            for (uint32_t slot : *posed)
                if (!std::binary_search(listed.begin(), listed.end(), slot)) list(slot);   // a listed slot once
        }
    }
}

// ---------------------------------------------------------------------------
/// IS THE SCREEN-PROBE GATHER ON FOR THIS SCENE? Wherever the machine traces and
/// the GI mode is on (there is no diffuse GI to estimate otherwise): at every tier
/// and in a headset alike (PHOTON-7 stage 7, 7-4), and with no row to turn it off
/// (7-5: on a ray chain it is the only producer of the diffuse the opaque passes
/// read). The machine answers whether it can trace at all.
bool PhotonComponent::probeGatherWanted() const {
    if (mGi.mode == GiMode::Off) return false;
    return mScene.rayTracingResolved();
}

/// THE SCENE'S VIEW-WIDE REST KEY - NO LONGER THE GATHER'S (PHOTON-7 stage 8B, B1: the gather rests per screen cell on the
/// changed-region list and the brick masks, rq_probe_resume.comp); its one reader left is the area visibility's history
/// (OgreAreaVis), which B2 moves to the list too. Its history: (PHOTON-GATHER-1d; PHOTON-2 stage 2's round, H1) what PREVENTED THE HOLD — every write to an
/// input of the gather's answer, the frame it is recorded: a light or environment write and a light removed, a node
/// moved (the transform epoch with the shadow scan's writes), a material edit, an injection landing or a cascade
/// rebuilt (giLightingSerial, the settle's steps included), the cloud layer's GI (cloudGiSerial - a restart-class
/// change that also breaks the hold), a field dispatch (counted on the
/// CPU as it is recorded: no readback latency) and the surface cache's captures and VISIBLE relights (a texel moved a code: R3 (a)). With the camera still
/// and this key moving, the gather is SETTLING: its EMA goes on taking every input's change at its rate, no rest
/// mean starts and nothing holds — the stage-2 key that left these out held the picture over changing inputs, and
/// they landed later as BURSTS (Bistro after the 4 m dolly: 385k px at frames 29-45, 273k at 188-205). What RESTARTS is another key
/// (gatherRestartKey: restart-class events only) — a write never restarts a history. The reflection folds what its
/// rays read beside these (reflectionReadsKey).
unsigned long long PhotonComponent::gatherRestKey() const {
    unsigned long long key = 1469598103934665603ull;
    const auto fold = [&key](unsigned long long v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    fold(mScene.lightWriteSerial());
    fold(mScene.lightsRemoved());
    fold(mScene.shadowEpoch());
    fold(mMaterialEditSerial);
    fold(giLightingSerial());
    fold(mScene.cloudGiSerial());
    fold(mIfd ? mIfd->getDispatches() : 0ull);
    if (mSurfaceCache) {
        CardCacheStatus cs;
        mSurfaceCache->fillStatus(cs);
        fold(cs.captures);
        fold(cs.stillTraces);
        // R3 (a)'s DISPLAY QUANTUM APPLIED TO THE READER (PHOTON-7 stage 10, (1) (ii)): the relights that moved a card
        // texel by a code since its last report (or could not be measured), read back - never the relight count, which
        // restarted a rest on relights whose every texel stayed under a code (Showroom 2: all 272 frames of a stop's
        // rounds). The report is CUMULATIVE (against the last reported value), so a rest across sub-code relights is never
        // more than a code from the cache.
        fold(cs.visibleRelights);
    }
    return key;
}

/// THE GATHER'S INPUTS STILL OWE WORK (PHOTON-2 stage 2's round, H1): a cascade rebuild or a chain re-solve pending,
/// a settle step or a light tick owed, a bounce series being measured, the field's integration or refinements owed,
/// and the surface cache's capture queue or stale sun terms. Between two writes of a cache that runs at a cadence
/// the rest key stands still for a frame or more; this is what keeps a view SETTLING through those gaps, so its rest
/// mean starts only once every input has finished (the hold = the last write + the settle).
/// THE READERS' HOLD IS OWED TO THE CARDS' REST DECISION (PHOTON-7 stage 10, R3's ruling; SurfaceCache::restHoldOwed):
/// a view counts its rest frames - the decision's demand is the first N of them - but never holds before the stop's one
/// decision is taken and its captures, relights and card-to-card rounds are done, so its first hold is its last (the
/// view held 9 frames before the decision and the decision's 72 frames of rounds broke it: lab.stillness_control). The
/// gather's and the reflection's alike (stage 10, (1) (ii)): a sub-quantum relight no longer restarts their rests, so a
/// rest runs across relights whose visibility is still in flight, and neither holds until every report is read.
bool PhotonComponent::readersHoldOwed() const {
    return mSurfaceCache && mSurfaceCache->restHoldOwed();
}

/// WHAT BREAKS A REFLECTION'S REST VIEW-WIDE (PHOTON-2 F5; round 2, G6): every LIGHTING write a reflection ray reads -
/// a light write or removal (the hit's direct light), giLightingSerial (an injection landing, a cascade rebuild, the
/// settle's steps), the cloud layer's GI, the irradiance field's every dispatch (counted on the CPU as recorded) and the
/// surface cache's captures and VISIBLE relights (R3 (a): a texel moved a code) - every material EDIT, every TEXTURE that arrived (a streamed map the hit
/// decode reads from then on: settleTextureResidency). (The traced geometry no longer changes under a still node: the
/// near copy is level 0, PHOTON-7 stage 6.) NOT A MOVE: the transform and shadow epochs are out - a moved instance breaks the rest
/// PER TEXEL (the swept boxes RayQueryTier hands rq_reflect.comp: a texel whose ray, mean segment or sun leg crosses
/// one resumes; the sun leg is the hit's shadow ray, so a mover's shadow at a hit is caught there). The field's late
/// integration count stays out (the round-2 read's W3: its every dispatch is counted here as recorded).
/// RayQueryTier::reflectRestKey folds the material store's rows and the traced set with it.
unsigned long long PhotonComponent::reflectionReadsKey() const {
    unsigned long long key = 1469598103934665603ull;
    const auto fold = [&key](unsigned long long v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    fold(mScene.lightWriteSerial());
    fold(mScene.lightsRemoved());
    fold(mMaterialEditSerial);
    fold(giLightingSerial());
    fold(mScene.cloudGiSerial());
    fold(mIfd ? mIfd->getDispatches() : 0ull);
    fold(mTextureArrivals);
    if (mSurfaceCache) {
        CardCacheStatus cs;
        mSurfaceCache->fillStatus(cs);
        fold(cs.captures);
        fold(cs.stillTraces);
        // R3 (a)'s DISPLAY QUANTUM APPLIED TO THE READER (PHOTON-7 stage 10, (1) (ii)): the relights that moved a card
        // texel by a code since its last report (or could not be measured), read back - never the relight count, which
        // restarted a rest on relights whose every texel stayed under a code (Showroom 2: all 272 frames of a stop's
        // rounds). The report is CUMULATIVE (against the last reported value), so a rest across sub-code relights is never
        // more than a code from the cache.
        fold(cs.visibleRelights);
    }
    return key;
}

/// THE RESTART KEY (PHOTON-GATHER-1d fix round): the discontinuities the pixel
/// history cannot follow — the lighting (giLightingSerial: a light or sky write
/// through refreshGiLighting, an injection landing, a cascade rebuild), a
/// material write (the surface cache's precise door counts it; the voxels'
/// re-inject moves the lighting serial) and the cloud layer's GI change. NOT
/// the transform epoch: a mover's per-frame write is followed per pixel by the
/// reprojection, and a scene with one would otherwise never settle.
unsigned long long PhotonComponent::gatherRestartKey() const {
    unsigned long long key = 1469598103934665603ull;
    const auto fold = [&key](unsigned long long v) {
        key ^= v;
        key *= 1099511628211ull;
    };
    fold(giLightingSerial());
    fold(mScene.cloudGiSerial());
    if (mSurfaceCache) {
        CardCacheStatus cs;
        mSurfaceCache->fillStatus(cs);
        fold(cs.invalidMaterial);
        fold(cs.invalidSun);
    }
    return key;
}

/// The gather's numbers for `GiStatus`.
void OgreScene::gatherStatusInto(GatherStatus &out) const {
    out = GatherStatus();
    out.on = probeGatherWanted();
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->gatherStatsInto(this, out);
}

bool OgreScene::cardReadersRestComplete(unsigned restFrames[2], bool active[2]) const {
    restFrames[0] = restFrames[1] = 0u;
    active[0] = active[1] = false;
    return !mEngine || !mEngine->mRayTier || mEngine->mRayTier->cardReadersRestComplete(this, restFrames, active);
}

bool OgreScene::reflectRestPastHandOver() const {
    return !mEngine || !mEngine->mRayTier || mEngine->mRayTier->reflectRestPastHandOver(this);
}

bool OgreScene::gatherRestPastHandOver() const {
    return !mEngine || !mEngine->mRayTier || mEngine->mRayTier->gatherRestPastHandOver(this);
}

/// HARD SUN CONTACT SHADOWS (PHOTON-RAYS-1): the row, held inside its band.
/// Storing it builds nothing; the view's chain re-checks its shape each frame.
void PhotonComponent::setSunContact(const SunContactDesc &d) {
    SunContactDesc c = d;
    if (!(c.range >= kSunContactMinRange)) c.range = kSunContactMinRange;   // NaN too
    if (c.range > kSunContactMaxRange) c.range = kSunContactMaxRange;
    mSunContact = c;
}

/// The row resolved against the machine — `probeGatherWanted`'s shape: the
/// row says what the scene asks for and the machine answers whether it can.
bool PhotonComponent::sunContactWanted() const {
    return mSunContact.enabled && mScene.rayTracingResolved();
}

SunContactStatus OgreScene::sunContactStatus() const {
    SunContactStatus st;
    st.on = sunContactWanted();
    if (st.on && mEngine && mEngine->mRayTier) mEngine->mRayTier->sunContactStatsInto(this, st);
    if (st.on && !st.running && st.reason.empty())
        st.reason = "no view of this scene has drawn with the row on yet";
    return st;
}

// ---------------------------------------------------------------------------
// PHOTON'S RAY PASSES (photon/PhotonComponent.h): photon_trace and photon_resolve,
// recorded by the tier of the view whose chain holds them. The GATE makes a pass on a
// view that does not trace (no listener: rays off, a capture workspace) NOTHING — the
// render pass Ogre has open stays open.
namespace {
RayQueryTier *tierOfPass(Ogre::CompositorPass *pass, const ReflectPassListener *&key, OgreView *&view,
                         Ogre::CompositorPass *&next) {
    key = nullptr;
    view = nullptr;
    next = nullptr;
    const Ogre::CompositorNode *node = pass ? pass->getParentNode() : nullptr;
    const Ogre::CompositorWorkspace *ws = node ? node->getWorkspace() : nullptr;
    if (!ws) return nullptr;
    for (Ogre::CompositorWorkspaceListener *l : ws->getListeners())
        if (auto *rl = dynamic_cast<ReflectPassListener *>(l)) {
            if (!rl->mView || !rl->mView->mEngine) return nullptr;
            RayQueryTier *tier = rl->mView->mEngine->mRayTier;
            key = rl;
            view = rl->mView;
            return tier && tier->photonPassTarget(pass, key, view, next) ? tier : nullptr;
        }
    return nullptr;
}
}  // namespace

void registerPhotonRayPasses(AtomPassProvider *provider) {
    if (!provider) return;
    const auto gate = [](const AtomPassContext &ctx) {
        const ReflectPassListener *key;
        OgreView *view;
        Ogre::CompositorPass *next;
        return tierOfPass(ctx.pass, key, view, next) != nullptr;
    };
    provider->setGate(PhotonComponent::kTracePass, gate);
    provider->setGate(PhotonComponent::kResolvePass, gate);
    provider->setRecorder(PhotonComponent::kTracePass, [](AtomPassContext &ctx) {
        const ReflectPassListener *key;
        OgreView *view;
        Ogre::CompositorPass *next;
        if (RayQueryTier *tier = tierOfPass(ctx.pass, key, view, next)) tier->traceRayJobs(key, view, next);
    });
    provider->setRecorder(PhotonComponent::kResolvePass, [](AtomPassContext &ctx) {
        const ReflectPassListener *key;
        OgreView *view;
        Ogre::CompositorPass *next;
        if (RayQueryTier *tier = tierOfPass(ctx.pass, key, view, next)) tier->resolveRayJobs(key, view, next);
    });
}

void OgreEngine::shutdownRayQuery() {
    if (!mRayTier) return;
    for (auto &sc : mScenes)
        if (sc) mRayTier->detachFeed(sc.get());
    // EVERY LIVE SCENE'S STRUCTURES FIRST (they are the scene's, not the tier's):
    // released behind one idle device, then the tier's own plumbing.
    mRayTier->waitIdle();
    for (auto &sc : mScenes)
        if (sc) mRayTier->releaseScene(sc.get());
    mRayTier->close();
    delete mRayTier;
    mRayTier = nullptr;
}

void OgreScene::forgetRayQuery() {
    if (mEngine && mEngine->mRayTier) mEngine->mRayTier->forgetScene(this);
}

bool OgreScene::readTlasInstances(TlasReadback &out, std::string &err) {
    if (!mEngine || !mEngine->mRayTier) { err = "rayquery: no ray tier"; return false; }
    return mEngine->mRayTier->readTlasBlocking(this, out, err);
}

bool OgreScene::rayNearLevels(std::vector<int> &levels) const {
    // A read on the host's side is a REQUEST, answered on the engine's side (traceRays' rule, U3 stage 4a).
    OgreScene *self = const_cast<OgreScene *>(this);
    if (self->hostPosts()) { bool r = false; self->requestOnEngine([&] { r = rayNearLevels(levels); }); return r; }
    levels.clear();
    // A TOOL PATH (it submits and waits): the instance array as the device wrote it this frame.
    TlasReadback rb;
    std::string err;
    if (!self->readTlasInstances(rb, err)) return false;
    constexpr size_t kInstanceBytes = 64u, kReferenceAt = 56u;   // VkAccelerationStructureInstanceKHR
    if (rb.instances.size() < size_t(rb.slots) * 2u * kInstanceBytes) return false;
    levels.assign(rb.slots, -1);
    for (unsigned slot = 0; slot < rb.slots; ++slot) {
        uint64_t ref = 0u;
        std::memcpy(&ref, rb.instances.data() + size_t(2u * slot) * kInstanceBytes + kReferenceAt, sizeof(ref));
        if (!ref) continue;
        int level = -2;
        for (const TlasReadback::Structure &st : rb.structures)
            if (st.address == ref) { level = int(st.level); break; }
        if (level == -2)
            for (const TlasReadback::Skin &sk : rb.skins)
                if (sk.address == ref) { level = 0; break; }
        levels[slot] = level;
    }
    return true;
}

bool OgreScene::rayNearAddresses(std::vector<unsigned long long> &addresses) const {
    OgreScene *self = const_cast<OgreScene *>(this);
    if (self->hostPosts()) { bool r = false; self->requestOnEngine([&] { r = rayNearAddresses(addresses); }); return r; }
    addresses.clear();
    TlasReadback rb;
    std::string err;
    if (!self->readTlasInstances(rb, err)) return false;
    constexpr size_t kInstanceBytes = 64u, kReferenceAt = 56u;   // VkAccelerationStructureInstanceKHR
    if (rb.instances.size() < size_t(rb.slots) * 2u * kInstanceBytes) return false;
    addresses.assign(rb.slots, 0ull);
    for (unsigned slot = 0; slot < rb.slots; ++slot) {
        uint64_t ref = 0u;
        std::memcpy(&ref, rb.instances.data() + size_t(2u * slot) * kInstanceBytes + kReferenceAt, sizeof(ref));
        addresses[slot] = ref;
    }
    return true;
}

bool OgreScene::rayReflectionsWanted() const {
    // The project's own World row (world.rayTracing: off / auto / on — owner,
    // ledger §425) resolved against the machine: OgreScene::rayTracingResolved
    // = the row is not Off × the diagnostic latch (--no-ray-query,
    // JAHSHAKA_NO_RAY_QUERY — the one thing that still lets every
    // ray-consuming suite run BOTH pictures on one GPU) × the device's answer.
    return rayTracingResolved();
}

RayQueryStatus OgreScene::rayQueryStatus() const {
    RayQueryStatus st;
    if (!mEngine) return st;
    st.available = mEngine->rayQueryAvailable();
    if (!mEngine->mRayTier) {
        // NO TIER YET — either nothing has drawn since it was switched on, or
        // it is off. `enabled` must say WHICH (round 2, finding 18): reporting
        // false for both made "switched on, first frame not drawn" look exactly
        // like "this machine has ray tracing switched off", and a caller
        // reading straight after the row flipped to "auto" would be told no.
        st.enabled = st.available && mEngine->rayTracing();
        return st;
    }
    st = mEngine->mRayTier->status(this);
    // R5's own readings ride the same struct: the tier keeps them per VIEW (a
    // trace is a screen-space pass) and this folds the views of THIS scene into
    // the one answer giStatus asks for.
    mEngine->mRayTier->reflectStatsInto(this, st);
    mEngine->mRayTier->hitStatsInto(this, st);
    mEngine->mRayTier->glassEscapeStatsInto(this, st);
    return st;
}

/// THE BOUNDARY'S TRACE (tests and tools only — see traceBlocking).
bool OgreScene::traceRays(const std::vector<float> &rays, std::vector<float> &hits) {
    // U3 stage 4a: a read on the host's side is a REQUEST, answered on the engine's side.
    if (hostPosts()) { bool r = false; requestOnEngine([&] { r = traceRays(rays, hits); }); return r; }
    JAH_HOST_CALL("Scene::traceRays");
    hits.clear();
    if (!mEngine || !mEngine->mRayTier) return false;
    std::string err;
    if (!mEngine->mRayTier->traceBlocking(this, rays, hits, err)) {
        Ogre::LogManager::getSingleton().logMessage("rayquery: " + err);
        return false;
    }
    return true;
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka

#else   // !JAH_RAY_QUERY

// NO VULKAN RENDER SYSTEM ON THIS PLATFORM (macOS, and any build where the
// plugin or the Vulkan headers are absent): the tier does not exist, and every
// consumer takes the no-rays path — which is a supported picture, not a
// degraded one. Audit C-6's defect was a CMake FATAL_ERROR here; there is none.
namespace jahshaka {
namespace engine {
namespace detail {

void OgreEngine::setRayTracing(bool on) { mRayTracingWanted = on; mHostRayTracing = on; }
bool OgreEngine::deviceRayQuery() const { return false; }
void OgreEngine::updateRayQuery(const std::vector<OgreScene *> &) {}
void OgreEngine::ensureRayTier() {}
// (no ray tier: no swept box and no posed slot, and none of the list's mover consumers - the cards, the gather, the
// reflection, the hits - runs without it; movers are not voxel geometry)
void OgreEngine::listRayMovers(const std::vector<OgreScene *> &) {}
void OgreEngine::shutdownRayQuery() {}
void registerPhotonRayPasses(AtomPassProvider *) {}
struct RaySceneStructures {};
void RaySceneStructuresDelete::operator()(RaySceneStructures *p) const { delete p; }
RayQueryStatus OgreScene::rayQueryStatus() const { return RayQueryStatus(); }
bool OgreScene::traceRays(const std::vector<float> &, std::vector<float> &hits) {
    JAH_HOST_CALL("Scene::traceRays");
    hits.clear();
    return false;
}
void OgreScene::forgetRayQuery() {}
bool OgreScene::readTlasInstances(TlasReadback &, std::string &err) {
    err = "rayquery: not built";
    return false;
}
bool OgreScene::rayNearLevels(std::vector<int> &levels) const {
    levels.clear();
    return false;
}
bool OgreScene::rayNearAddresses(std::vector<unsigned long long> &addresses) const {
    addresses.clear();
    return false;
}
bool PhotonComponent::probeGatherWanted() const { return false; }
bool OgreScene::traceCardMovers(const CardMoverTrace &) { return false; }
CardGatherResult OgreScene::gatherCards(const CardGatherTrace &) { return CardGatherResult::NoRays; }
int OgreScene::gatherVoxelSides(const Ogre::PhotonVoxelSideTrace &, unsigned) { return 0; }
void OgreScene::voxelSideCounts(GiStatus::VoxelSideCounts &out) const { out = GiStatus::VoxelSideCounts(); }
bool OgreScene::giVoxelSideShares(GiVoxelSideShares &out) { out = GiVoxelSideShares(); return false; }
void OgreScene::cardGatherCounts(CardCacheStatus::GatherCounts &out) const { out = CardCacheStatus::GatherCounts(); }
void OgreScene::cardGatherSky(std::vector<std::pair<unsigned, unsigned>> &out) { out.clear(); }
bool OgreScene::cardTracePresent() const { return false; }
void OgreScene::cardMoverTimes(float &, float &) {}
void OgreScene::gatherStatusInto(GatherStatus &out) const { out = GatherStatus(); }
bool OgreScene::cardReadersRestComplete(unsigned restFrames[2], bool active[2]) const {
    restFrames[0] = restFrames[1] = 0u;
    active[0] = active[1] = false;
    return true;
}
bool OgreScene::reflectRestPastHandOver() const { return true; }
bool OgreScene::gatherRestPastHandOver() const { return true; }
void PhotonComponent::setSunContact(const SunContactDesc &d) { mSunContact = d; }
bool PhotonComponent::sunContactWanted() const { return false; }
SunContactStatus OgreScene::sunContactStatus() const { return SunContactStatus(); }
void rayTierAreaVisStatsInto(const OgreEngine *, const OgreScene *, AreaVisStatus &) {}
bool OgreScene::rayReflectionsWanted() const { return false; }
void OgreView::dropReflectState() {}
void OgreView::retirePhotonOverlay() {
    // No tier, nothing ever wrote it (the verb refuses ScreenProbes and Hits).
    if (mPhotonOverlay && mRoot && mRoot->getRenderSystem())
        mRoot->getRenderSystem()->getTextureGpuManager()->destroyTexture(mPhotonOverlay);
    mPhotonOverlay = nullptr;
    mPhotonOverlayWritten = false;
}
bool OgreEngine::cardReadParity(Scene *, const std::vector<CardReadQuery> &,
                                std::vector<CardReadPick> &out) {
    out.clear();
    mLastError = "cardReadParity: no ray-query tier on this platform";
    return false;
}

// R5 takes the same road: with no tier there is nothing to hook, so the
// listener is never created and `jahSsrReflection` holds what the screen-space
// resolve wrote — the fallback picture, which is the picture this renderer drew
// before ray tracing existed.
ReflectPassListener::~ReflectPassListener() {}
void ReflectPassListener::passPreExecute(Ogre::CompositorPass *) {}
void ReflectPassListener::passPosExecute(Ogre::CompositorPass *) {}
void OgreView::syncReflectListener() {
    // No tier on this platform: the chain is built with `rayReflect` false by
    // construction (the predicate above answers false), so there is nothing to
    // arm and nothing to rebuild.
    if (mReflectListener) {
        removeWorkspaceListener(mReflectListener.get());
        mReflectListener.reset();
    }
}

}   // namespace detail
}   // namespace engine
}   // namespace jahshaka

#endif  // JAH_RAY_QUERY
