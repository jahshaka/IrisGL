// THE SCREEN-PROBE GATHER — the Component's implementation (GATHER-1a,
// 2026-09-21; SPECS/SCREEN_PROBE_GATHER_SPEC.md section 9).
//
// FOUR COMPUTE DISPATCHES on the frame's own command buffer, recorded at the
// reflection listener's hook — after the SSR prepass (whose depth and normals
// are the probes' surfaces) and before the opaque pass that shades:
//
//   rq_probe_place.comp      one workgroup per cell: the probe's jittered pixel,
//                            its world position and normal, and the adaptive
//                            probe the cell wants where the grid does not
//                            answer one of its pixels (every pixel tested).
//   rq_probe_gather.comp     one WORKGROUP per probe, one ray per thread, one
//                            ray per octahedral texel; dispatched INDIRECTLY
//                            because the adaptive probes' count is the GPU's.
//   rq_probe_filter.comp     one WORKGROUP per probe, one texel per thread: the
//                            map filtered against its 3x3 neighbourhood in
//                            probe space and projected onto SH9 (same indirect
//                            arguments; PHOTON-GATHER-1b).
//   rq_probe_integrate.comp  one thread per pixel: the four grid probes and the
//                            cell's adaptive twin, weighted by the plane test,
//                            each SH evaluated at the pixel's own normal, then
//                            the pixel HISTORY (PHOTON-GATHER-1c: reprojected,
//                            validated, the count-in-history mean), into
//                            `jahProbeIrradiance`.
//
// WHAT IS PING-PONGED (PHOTON-GATHER-1c): the pixel history — one packed
// 8-byte texel a pixel since PHOTON-GA-VR — `View::flip` names this frame's half.
//
// STEREO (PHOTON-GA-VR, LAYERED-STEREO-1): a two-eye target is gathered in the
// same four dispatches. Every screen image is a two-layer array, one eye per
// layer at the same origin, and the per-pixel integrate's z is the eye; the
// probe LIST holds two grids side by side, each `eyeGridW` columns over its own
// eye's layer, so a cell names its eye. Every job reconstructs, filters and
// reprojects through the eye that owns the cell or the layer (jah_probe_params.glsl's
// eye helpers — the reflection trace's rule, the same text shape).
//
// THE BOUNDARY THIS FILE KEEPS. It speaks Vulkan and Ogre and knows nothing
// about the scene graph: everything it needs about the frame arrives in
// `GatherInputs`, filled by the ray tier, which is the object that is friends
// with OgreScene. That is the whole difference between a Component and more
// code in the tier, and it is what makes phases 2 and 3 (a filter, an SH
// record, history pairs) edits to this file alone.
#include "ScreenProbeGather.h"
#include "ReadbackDecode.h"

#ifdef JAH_RAY_QUERY

#include "EnginePrivate.h"
#include "GpuCheckpoints.h"

#include <OgreRenderSystem.h>
#include <OgreTextureGpuManager.h>
#include <OgreLogManager.h>
#include <OgreResourceTransition.h>
#include <Vao/OgreUavBufferPacked.h>

#include "OgreVulkanRenderSystem.h"
#include "OgreVulkanTextureGpu.h"
#include "OgreVulkanDevice.h"
#include "Vao/OgreVulkanBufferInterface.h"

#include "rayquery/rq_probe_place_spv.h"
#include "rayquery/rq_probe_place_select_spv.h"
#include "rayquery/rq_probe_gather_spv.h"
#include "rayquery/rq_probe_filter_spv.h"
#include "rayquery/rq_probe_integrate_spv.h"
#include "rayquery/rq_probe_resume_spv.h"
#include "rayquery/rq_probe_resume_pyramid_spv.h"
#include "rayquery/rq_probe_resume_clock_spv.h"
#include "rayquery/rq_probe_resume_final_spv.h"
#include "rayquery/rq_probe_resume_ring_spv.h"
#include "VkDescriptorPools.h"

#include <OgreBitwise.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace jahshaka {
namespace engine {

namespace {

using Clock = std::chrono::steady_clock;
inline double msSince(const Clock::time_point &t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// Descriptor sets and parameter buffers in flight per view — a set a command
/// buffer still holds may not be rewritten, and this one is rewritten every
/// frame because every input can be recreated behind our back.
constexpr unsigned kRing = 3u;
/// How many views the descriptor pool plans sets for (far more than a frame draws).
constexpr unsigned kMaxViews = 8u;
/// Frames of readbacks in flight (Ogre's dynamic-buffer multiplier).
constexpr unsigned kFramesInFlight = 3u;

/// The placement's six, and THE REST CLOCK (6, PHOTON-7 stage 8B, B1: the frame's sample index is the GPU's -
/// jah_probe_rest.glsl).
constexpr unsigned kPlaceBindings = 7u;
constexpr unsigned kPlaceClockBinding = 6u;
/// The trace's nine, then GA-1e's six: the card read's four (9-12, the
/// reflection's jah_rq_card_bindings.glsl at base 9) and the geometric
/// normal's two (13, 14); then the split voxel store's four arrays by name
/// (15-18: the coverage and the surface position per half, PHOTON-VOXEL-4); then
/// THE HIT RECORD's four (19-22, PHOTON-HIT-SHADE-1); then level 0's back side and
/// the voxelizer's normal (23, 24: PHOTON-VOXEL-5); then the card read's VIEW
/// TERM's seven (25-31, PHOTON-CARDS-5 + the Specular layer, ENGINE-POLISH-PHOTON-1, + the sun's visibility,
/// PHOTON-3: jah_rq_card_bindings.glsl at JAH_CARD_VIEW_BINDING_BASE 25).
constexpr unsigned kTraceBindings = 42u;
constexpr unsigned kTraceSideBinding = 23u;
constexpr unsigned kTraceCardViewBinding = 25u;
/// ...and THE DIRECT TERM's volume per cascade (32, MOVER-OCCLUSION-1: jah_rq_hit.glsl).
constexpr unsigned kTraceDirectBinding = 32u;
/// ...and THE GATHER-HITS INSTRUMENT's two (FAR-SUN-1, rq_probe_gather.comp): the per-ray
/// records (33) and the photon view's overlay (34).
constexpr unsigned kTraceRaysBinding = 33u;
constexpr unsigned kTraceOverlayBinding = 34u;
/// ...and THE HIT'S OWN SURFACE's two (FAR-SUN-2, jah_rq_hit_direct.glsl): the voxeliser's texture
/// pool (35) and the cloud layer's field (36).
constexpr unsigned kTracePoolBinding = 35u;
constexpr unsigned kTraceCloudBinding = 36u;
/// ...and THE DIRECT TERM's BACK side per cascade (37, PHOTON-7 stage 7: the direct store holds the two sides, and the
/// hit's share and its mover gate read their mean - jah_rq_hit.glsl).
constexpr unsigned kTraceDirectBackBinding = 37u;
/// ...and THE CELLS' REST (38 - 40, PHOTON-7 stage 8B, B1; jah_probe_rest.glsl): the clock, the cells' state (a probe
/// whose cell holds outside every ring is not traced) and their distance store (an active cell's rays write it).
constexpr unsigned kTraceClockBinding = 38u;
constexpr unsigned kTraceCellStateBinding = 39u;
constexpr unsigned kTraceCellStoreBinding = 40u;
/// ...and THE FAST UPDATE's BRICK MASKS (41, PHOTON-7 stage 8B, B5): a ray whose hit lands on a brick this frame's light
/// work changed is a MOVING ray (rq_probe_gather.comp), four cascades' masks, one binding.
constexpr unsigned kTraceFastBricksBinding = 41u;
/// LUMEN's FAST UPDATE numbers (r.Lumen.ScreenProbeGather.Temporal.FractionOfLightingMovingForFastUpdateMode and
/// .MaxFastUpdateModeAmount, both at their defaults; LUMEN_TEMPORAL_FACTS :326-327)
constexpr float kFastUpdateMovingShare = 0.1f;
constexpr float kFastUpdateMost = 0.9f;
static_assert(kTraceRaysBinding == kTraceDirectBinding + 1u && kTraceOverlayBinding == kTraceRaysBinding + 1u &&
                  kTracePoolBinding == kTraceOverlayBinding + 1u && kTraceCloudBinding == kTracePoolBinding + 1u &&
                  kTraceDirectBackBinding == kTraceCloudBinding + 1u && kTraceClockBinding == kTraceDirectBackBinding + 1u &&
                  kTraceFastBricksBinding == kTraceCellStoreBinding + 1u && kTraceFastBricksBinding + 1u == kTraceBindings &&
                  kTraceDirectBinding == kTraceCardViewBinding + SurfaceCache::kViewLayers,
              "the direct volume follows the card read's view-term layers, the instrument's two follow it");
/// THE HIT SET's PLACE IN THE TRACE SET (HitSetSlots, the one writer): the TLAS (0), the volumes by kind (4-7, 15-18,
/// 23-24, 32, 37), the sky (8), the card read's four (9-12) and its view term (25-31), the geometric normal (13, 14), the
/// record (19-22), the hit's own surface (35, 36). The trace's own: the params (1), the records (2), the atlas (3), the
/// instrument's two (33, 34).
constexpr HitSetSlots kTraceHitSlots = {
    0u,
    { 4u, 5u, 6u, 7u, 15u, 16u, 17u, 18u, kTraceSideBinding, kTraceSideBinding + 1u, kTraceDirectBinding,
      kTraceDirectBackBinding },
    8u,
    { 9u, 10u, 11u, 12u },
    kTraceCardViewBinding,
    { 13u, 14u },
    { 19u, 20u, 21u, 22u },
    kTracePoolBinding,
    kTraceCloudBinding,
};
static_assert(kTraceBindings == kHitSetBindings + 9u,
              "the trace set is the hit set, its own five, the rest's three and the fast update's masks");
/// The filter's five: params, records, the raw atlas, the eye's depth (its taps' depth walk, PHOTON-2 stage 2's tail)
/// and the prefiltered maps it stores (PHOTON-7 stage 7, 7-3: the rough specular's source).
/// ...and the cells' state (5, PHOTON-7 stage 8B, B1): a probe the trace skipped is not filtered.
constexpr unsigned kFilterBindings = 6u;
constexpr unsigned kFilterCellStateBinding = 5u;
/// The integrate's eighteen: params, records, normals, depth, the irradiance, the
/// history's two halves (one packed texel each, PHOTON-GA-VR), the rest mean,
/// the photon view's overlay (PHOTON-VIEW-1), the frame's counter, the
/// irradiance field's two atlases (10, 11: the fallback it composes, PHOTON-7 7-2),
/// and THE ROUGH SPECULAR's six (12 - 17, 7-3): the prepass's shadow / roughness,
/// the prefiltered maps, the output, the lobe's history pair and its rest mean; and THE HIT SET (18 - 50, PHOTON-7 10-3
/// R3 (b), R4 (c1) and (iii): the unanswered share past the field's window gathered by the voxel chain's rays and,
/// where the chain cannot answer, continued through the TLAS - its one writer, kIntegrateHitSlots); and THE SKY
/// VISIBILITY (51, PHOTON-7 10-4 R11), after the hit set.
constexpr unsigned kIntegrateHitFirst = 18u;
constexpr unsigned kIntegrateSkyVisBinding = kIntegrateHitFirst + kHitSetBindings;
/// ...and THE CELLS' REST (52, 53, PHOTON-7 stage 8B, B1): the clock (the rest law's K) and the cells' state (a held
/// cell's pixels write their history through and nothing else).
constexpr unsigned kIntegrateClockBinding = kIntegrateSkyVisBinding + 1u;
constexpr unsigned kIntegrateCellStateBinding = kIntegrateClockBinding + 1u;
/// ...and THE HISTORY's VALIDITY STREAK pair (55 last frame's, 56 this frame's; 8B, the lead's ruling) after it.
/// ...and THE CELLS' DISTANCE STORE (54, 8B): the pixel's own rays - the field fallback's voxel and TLAS continuation - write
/// their hits into its cell's bins as the trace's do, so a change they read resumes the cell (the resume's region test).
constexpr unsigned kIntegrateCellStoreBinding = kIntegrateCellStateBinding + 1u;
constexpr unsigned kIntegrateStreakPrevBinding = kIntegrateCellStoreBinding + 1u;
constexpr unsigned kIntegrateStreakCurBinding = kIntegrateStreakPrevBinding + 1u;
constexpr unsigned kIntegrateBindings = kIntegrateStreakCurBinding + 1u;
/// THE HIT SET's PLACE IN THE INTEGRATE SET (rq_probe_integrate.comp's bindings): the TLAS, the volumes by kind, the sky,
/// the card read's four and its view term's seven, the geometric normal's two, the record's four, the hit's surface two.
constexpr HitSetSlots kIntegrateHitSlots = {
    18u,
    { 19u, 20u, 21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u, 29u, 30u },
    31u,
    { 32u, 33u, 34u, 35u },
    36u,
    { 43u, 44u },
    { 45u, 46u, 47u, 48u },
    49u,
    50u,
};
static_assert(36u + SurfaceCache::kViewLayers == 43u && kIntegrateSkyVisBinding == 51u && kIntegrateBindings == 57u,
              "the integrate's hit set: the card read's view term fills 36 - 42, the set ends at 50, the sky "
              "visibility at 51");
/// THE PACKED HISTORY TEXEL (PHOTON-GA-VR): rg32ui, 8 bytes a pixel for each half
/// of the ping-pong — the shared-exponent mean and the geometry word.
constexpr unsigned kHistoryTexelBytes = 8u;
/// The integrate set's STORAGE IMAGES: the irradiance, the history's two halves, the rest mean and the photon overlay
/// — bindings 4..8; then the frame's counter buffer (9, PHOTON-2 L8: the foliage rule's own accepts); and the rough
/// specular's output, the lobe's history pair and its rest mean (14 - 17, 7-3); the sky visibility (51, 10-4 R11).
constexpr unsigned kIntegrateStorageImages = 10u;
static_assert(kIntegrateBindings == 14u + kIntegrateStorageImages + kHitSetBindings,
              "the integrate set: four inputs, nine storage images, the counter, the field's two atlases, the "
              "roughness and the prefiltered maps, the hit set, the rest's clock and cells");
/// THE ROUGH SPECULAR'S LEVELS (PHOTON-7 stage 7, 7-3, the lead's ruling B; rq_probe_filter.comp's job 3): the probes'
/// maps prefiltered at perceptual roughness 0.3, 0.5 and 0.7 (a lobe sharper than 0.3 reads the first level: the 8 x 8
/// hemisphere resolves no sharper one), and DiffuseLerp's two ends (Lumen's) - from 0.6 the lobe hands over linearly
/// to the diffuse E/pi, which owns it from 0.8.
constexpr float kRoughSpecFirstLevel = 0.3f;
constexpr float kRoughSpecLevelStep = 0.2f;
constexpr unsigned kRoughSpecLevels = 3u;
constexpr float kRoughSpecDiffuseFrom = 0.6f;
constexpr float kRoughSpecDiffuseAt = 0.8f;

/// One probe's record: eleven vec4s — position, normal, the irradiance at its own
/// normal, the SH9 (27 floats in seven vec4s) and the drawn surface's geometric
/// normal its rays lift along (READ_B F3); see JahProbeRecord in the shaders.
/// PHOTON-GATHER-1b grew it from three vec4s.
constexpr unsigned kRecordBytes = 176u;
/// THE WEIGHT FLOOR under which a pixel's plane-weighted probes are not an
/// answer (w = 0: the fallback owns the pixel). A pixel whose ONLY agreeing probe
/// is a corner cell's at a sixteenth of the bilinear weight still clears it; one
/// whose probes all sit off its plane does not.
constexpr float kWeightFloor = 1e-3f;

/// rq_probe_*.comp's uniform block, MEMBER FOR MEMBER (jah_probe_params.glsl).
/// std140 over vec4s only, so the C++ layout is the GLSL layout by
/// construction.
struct GatherParams {
    float camPos[4] = {};
    float rayTL[4] = {};
    float rayRight[4] = {};
    float rayDown[4] = {};
    float fwd[4] = {};
    float projParams[4] = {};
    float resolution[4] = {};
    float knobs[4] = {};
    float knobs2[4] = {};
    float knobs3[4] = {};
    float plane[4] = {};
    float viewAxisX[4] = {};
    float viewAxisY[4] = {};
    float viewAxisZ[4] = {};
    float knobs4[4] = {};
    float prevCamPos[4] = {};
    float prevRayTL[4] = {};
    float prevRayRight[4] = {};
    float prevRayDown[4] = {};
    float prevFwd[4] = {};
    float knobs5[4] = {};
    float knobs6[4] = {};
    /// THE HIT RECORD (PHOTON-HIT-SHADE-1; jah_probe_params.glsl's hitList,
    /// hitSun, hitSun2).
    /// THE SECOND EYE (PHOTON-GA-VR; jah_probe_params.glsl's `stereo` block):
    /// x = 1 stereo, y = one eye's probe columns (zw unused); then the right
    /// eye's five vectors, now and the previous frame's.
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
    /// THE ALPHA TABLE (REFLECT-MOVERS-2): xy = its device address, bit-copied;
    /// z = the arm "gather.decodeHits"; w = the mover gate.
    float alpha[4] = {};
    /// THE YOUNG HISTORY'S REACH (GATHER-NOISE-1): x = the history length below
    /// which a pixel widens, y = the reach in cells at a history of 0.
    float young[4] = {};
    /// THE GATHER-HITS INSTRUMENT (FAR-SUN-1): x = 1 the per-ray records, y = 1 the
    /// GatherHits view, z = the stride in pixels.
    float hits[4] = {};
    /// THE HIT'S OWN SURFACE (FAR-SUN-2, jah_rq_hit_direct.glsl): x, y = the hit table's
    /// address (float bits; 0 = none), z = 1 when the texture pool is bound, w = 1 when the
    /// cloud layer shades the sun; then the cloud shadow's map and sun (jah_cloud_shadow.glsl).
    /// THE IRRADIANCE FIELD, the integrate's fallback (PHOTON-7 stage 7, 7-2; jah_probe_params.glsl): its render
    /// block for an identity view (24 floats: three world-to-grid rows, the probe strides, the two atlases), the
    /// window word and counts (w = 1: bound and composed), and the environment's SH (C0 .. C8 in xyz, world axes).
    float field[24] = {};
    float fieldWin[4] = {};
    float envSh[9][4] = {};
    /// THE ROUGH SPECULAR (PHOTON-7 stage 7, 7-3; jah_probe_params.glsl): the gate's top and ramp, DiffuseLerp's two
    /// ends; the prefiltered levels (the first, the step, the count).
    float rough[4] = {};
    float roughLevels[4] = {};
    /// THE FAST UPDATE (PHOTON-7 stage 8B, B5; jah_probe_params.glsl): the moving share's saturation and the most, the
    /// skin posed this frame, on; the brick masks' corners / brick and dims / live, per cascade
    float fast[4] = {};
    float fastBrickOrigin[4][4] = {};
    float fastBrickDims[4][4] = {};
    /// THE HIT SET'S UNIFORMS (PHOTON-7 stage 10, 10-2: hitSetParams; jah_probe_params.glsl's last member)
    HitSetParams hit;
};
static_assert(sizeof(GatherParams) % 16u == 0u, "the params block is whole vec4s (std140)");

/// THE RESUME'S OWN BLOCK (PHOTON-7 stage 8B, B1; rq_probe_resume.comp's RestParams, std140 - whole vec4s).
struct RestParams {
    float mode[4] = {};    ///< still, N, the ring's reach in cells, the view rests at all
    float seq[4] = {};     ///< the moving counter, the rest sequence's base, the frozen index, a sky pixel's depth
    float lists[4] = {};   ///< the list's regions, its overflow (+ the restart), (unused), the live masks
    uint32_t brickDims[GatherRestInputs::kCascades][4] = {};
    float brickOrigin[GatherRestInputs::kCascades][4] = {};
    uint32_t brickPyramid[GatherRestInputs::kCascades][4] = {};
    uint32_t brickLevelOffset[GatherRestInputs::kCascades][2][4] = {};
    float field[4] = {};   ///< the irradiance field owes work over itself
    float blocks[4] = {};  ///< the blocks: their side in cells, across, down, their count
};
static_assert(sizeof(RestParams) % 16u == 0u, "the resume's block is whole vec4s (std140)");
/// THE RESUME SET (rq_probe_resume.comp): the gather's params, its own block, the depth, the cells' state and store, the
/// clock, the list's buffer, the four masks (one binding, an array) and the pyramids.
constexpr unsigned kResumeBindings = 9u;
constexpr unsigned kResumeMaskBinding = 7u;
/// THE LIST's BLOCK as the resume declares it (two vec4s of header, two a region, kGiRegionCap regions): the
/// stand-in bound where no list has closed is at least this big.
constexpr size_t kRegionBlockBytes = (2u + 2u * kGiRegionCap) * 16u;
/// THE CELLS' WORDS: the state, the ring bit and the convergence's four a cell (jah_probe_rest.glsl kCellWords); the
/// distance store's least and most hit of 64 bins a cell and their escape bits (kRestStoreWords); the clock's eight,
/// then two a cell (its index, its K) and two a block (its K, its frame's facts) - clockWordsOf.
constexpr unsigned kCellStateWords = 12u;   // jah_probe_rest.glsl kCellWords (B3: the hand-over's two words)
constexpr unsigned kCellStoreWords = 130u;
constexpr unsigned kClockWords = 8u;
/// THE READ-BACK RING'S SLOT: the placement's and the integrate's four words, then the clock's eight.
constexpr unsigned kReadbackSlotBytes = 20u + 4u * kClockWords;   // (+ word 4: the longest hand-over begun, B3)
/// THE BLOCKS' SIDE, the least (the ring's least reach 2: 2 (2 + 1) + 1) - the clock buffer is sized for it.
constexpr unsigned kLeastBlockSide = 7u;
VkDeviceSize clockWordsOf(unsigned gridW, unsigned gridH) {
    const VkDeviceSize blocks = VkDeviceSize((gridW + kLeastBlockSide - 1u) / kLeastBlockSide) *
                                ((gridH + kLeastBlockSide - 1u) / kLeastBlockSide);
    return kClockWords + 2u * VkDeviceSize(gridW) * gridH + 2u * blocks;
}
/// THE BRICK PYRAMID's levels at most (a cascade of 2^7 bricks a side: 1024^3 cells).
constexpr unsigned kPyramidLevels = 8u;

/// THE PIXEL HISTORY'S BLEND FLOOR (PHOTON-GATHER-1c item 1): the smallest
/// weight a new frame takes once the running mean has that many frames in it —
/// a new frame's share is 1/n until n reaches it, then 1/kHistoryFrames. It is
/// the trade between stillness and lag, both measured by gi.gather_stable on the
/// Showroom-2-shaped room: at 10 (Lumen's own default for its screen-probe
/// history; GatherTuning::historyFrames is the A/B door) a still view steps by at most 1/255 at every sampled pixel after the
/// warm-up (9/255 with each frame alone), and a lamp switched off is within one
/// code of its settled picture 16 frames later (0 with each frame alone).
constexpr float kHistoryFrames = 10.0f;

/// The history's floor in frames as a tuning runs it (the shipped kHistoryFrames,
/// or GatherTuning::historyFrames, 1..15 — the packed texel's four count bits).
unsigned historyFramesOf(const GatherTuning &t) {
    return t.historyFrames ? std::min(std::max(t.historyFrames, 1u), 15u) : unsigned(kHistoryFrames);
}
/// N — the frames a step of kGatherSettleCodes codes takes to fall under one code
/// through the history's EMA, ceil( ln(1/D) / ln(1 - 1/h) ) (16 at h = 10), and
/// the length of the REST MEAN (rq_probe_integrate.comp): at rest the answer IS
/// the mean of N rest frames at the N-th, and the view then holds — whatever
/// else delays the scene's rest, the held picture is the same N samples.
unsigned settleFramesOf(unsigned historyFrames) {
    const double h = double(std::max(historyFrames, 2u));
    return unsigned(std::ceil(std::log(1.0 / double(kGatherSettleCodes)) / std::log(1.0 - 1.0 / h)));
}
/// The rest mean's length as a tuning runs it: the settle, or the measurement
/// door GatherTuning::restFrames (the converged reference).
unsigned restLengthOf(const GatherTuning &t, unsigned historyFrames) {
    return t.restFrames ? t.restFrames : settleFramesOf(historyFrames);
}
/// The probe-space filter's reach in cells as a tuning runs it (1 = 3 x 3,
/// 2 = plus the ring at two cells).
constexpr unsigned kFilterRadius = 1u;
/// THE YOUNG HISTORY'S REACH (GATHER-NOISE-1; rq_probe_integrate.comp): the
/// history length below which a pixel reads wider, and the reach in probe
/// cells at a history of 0. MEASURED, not chosen: see the lane's evidence.
constexpr unsigned kYoungFrames = 12u;
constexpr float kYoungReachPixels = 24.0f;   // 3 cells at Epic's 8-px stride, 1.5 at High's 16
/// THE CROSS-PROBE STRATA (GATHER-NOISE-1; rq_probe_gather.comp): N, the side of
/// the sub-cell grid a texel is split into across an N x N block of probes.
constexpr unsigned kCrossStrata = 3u;
unsigned filterRadiusOf(const GatherTuning &t) {
    return t.filterRadius ? std::min(t.filterRadius, 2u) : kFilterRadius;
}
/// THE REST FRAMES' SAMPLE SEQUENCE: frame index kRestSequenceBase + k at the
/// k-th rest frame, so the rest mean is the same set of samples whoever asks and
/// whatever came before — a function of the scene and the camera alone.
constexpr unsigned kRestSequenceBase = 0x10000u;

/// Does a tuning change CHANGE THE ESTIMATOR? Every field but the readback (a
/// test door that only copies the answer out) and THE GI FREEZE (M1's measuring
/// arm: it changes no estimator - the integrate's blend 0 only KEEPS the history
/// it holds - and a restart on its switch froze the walk on that frame's one
/// estimate instead of the converged answer: PHOTON-2 round 4, the read's P2-1).
/// A changed estimator is a new history: its first frame takes its own estimate
/// whole, so an A/B across a tuning push is never an average of its two arms.
bool sameEstimator(const GatherTuning &a, const GatherTuning &b) {
    return a.probeStride == b.probeStride && a.octRes == b.octRes && a.rayLength == b.rayLength &&
           a.adaptiveCap == b.adaptiveCap && a.freezeFrameIndex == b.freezeFrameIndex &&
           a.jitterOff == b.jitterOff && a.rayJitterOff == b.rayJitterOff &&
           a.farQueryOff == b.farQueryOff &&
           a.shBands == b.shBands && a.filterOff == b.filterOff &&
           a.historyFrames == b.historyFrames &&
           a.historyValidationOff == b.historyValidationOff && a.restOff == b.restOff &&
           a.classOff == b.classOff &&
           a.filterRadius == b.filterRadius && a.restFrames == b.restFrames &&
           a.restSeed == b.restSeed && a.youngFrames == b.youngFrames &&
           a.youngReach == b.youngReach && a.crossStrata == b.crossStrata;
}

/// THE ADAPTIVE TEST'S TWO TOLERANCES, and why they are constants rather than
/// rows. `kPlaneTolerance` is how far a cell's pixel may lie off its probe's
/// plane before the cell wants a second probe, AS A FRACTION OF THE PIXEL'S OWN
/// VIEW DISTANCE — an angular tolerance, so one number works at a metre and at
/// fifty. 1/100 is a little under a degree of plane tilt across a cell at any
/// depth, which passes a flat floor seen at a grazing angle and fails a step or
/// a railing. `kNormalTolerance` is the dot product below which two normals are
/// not the same surface at all (about 25 degrees).
///
/// They are not tier rows because a person cannot reason about them: what a
/// person tunes is the probe STRIDE, which is the tier's own column.
constexpr float kPlaneTolerance = 0.01f;
constexpr float kNormalTolerance = 0.9f;

}   // namespace

unsigned gatherSettleFramesOf(unsigned historyFrames) { return settleFramesOf(historyFrames); }

// ---------------------------------------------------------------------------
ScreenProbeGather::~ScreenProbeGather() { close(); }

bool ScreenProbeGather::makePipelines(std::string &err) {
    VkDevice dev = mHost.gatherDevice();
    // PER VIEW AND RING SLOT, four sets (place, trace, filter, integrate): the pool is
    // planned from the four layouts as each is made (VkDescriptorPools.h).
    const unsigned groups = kMaxViews * kRing;
    detail::DescriptorPoolPlan plan;
    const auto makeLayout = [&](unsigned count, const VkDescriptorType *types,
                                const unsigned *counts, VkDescriptorSetLayout &out,
                                const char *what) {
        // Sized by the widest set (the trace's) and refused beyond it: a fixed 16 here was
        // overrun by the trace's nineteen and smashed the stack (PHOTON-VOXEL-4's rebase).
        constexpr unsigned kMaxBindings = std::max({ kPlaceBindings, kTraceBindings, kFilterBindings,
                                                     kIntegrateBindings, kResumeBindings });
        VkDescriptorSetLayoutBinding b[kMaxBindings] = {};
        if (count > kMaxBindings) {
            err = std::string("gather: the ") + what + " set has more bindings than the layout table";
            return false;
        }
        for (unsigned i = 0; i < count; ++i) {
            b[i].binding = i;
            b[i].descriptorType = types[i];
            b[i].descriptorCount = counts ? counts[i] : 1u;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        // The layout and its pool line in one call (VkDescriptorPools.h).
        return plan.makeLayout(dev, b, count, groups, out, err,
                               (std::string("gather/") + what).c_str());
    };

    {   // rq_probe_place.comp
        const VkDescriptorType t[kPlaceBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 1 normals
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 depth
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 3 records
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 4 counter
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 5 indirect args
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 6 the rest clock (8B, B1)
        };
        if (!makeLayout(kPlaceBindings, t, nullptr, mPlaceLayout, "place")) return false;
    }
    {   // rq_probe_gather.comp: its own five, the hit set (kTraceHitSlots) laid out by its one function
        VkDescriptorSetLayoutBinding hb[kTraceBindings] = {};
        hitSetLayout(kTraceHitSlots, hb);
        VkDescriptorType t[kTraceBindings] = {};
        unsigned c[kTraceBindings] = {};
        for (unsigned i = 0; i < kTraceBindings; ++i) {
            t[i] = hb[i].descriptorType;
            c[i] = hb[i].descriptorCount ? hb[i].descriptorCount : 1u;
        }
        t[1] = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;               // 1 params
        t[2] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;               // 2 records
        t[3] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;                // 3 atlas
        t[kTraceRaysBinding] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   // the per-ray records (FAR-SUN-1)
        t[kTraceOverlayBinding] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; // the photon overlay (GatherHits)
        for (unsigned b : { kTraceClockBinding, kTraceCellStateBinding, kTraceCellStoreBinding }) {   // the cells' rest
            t[b] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            c[b] = 1u;
        }
        t[kTraceFastBricksBinding] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;   // the fast update's masks (8B, B5)
        c[kTraceFastBricksBinding] = GatherRestInputs::kCascades;
        if (!makeLayout(kTraceBindings, t, c, mTraceLayout, "trace")) return false;
    }
    {   // rq_probe_filter.comp
        const VkDescriptorType t[kFilterBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 records
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 2 the raw atlas
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 depth (the taps' depth walk)
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 the prefiltered maps (7-3)
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 5 the cells' state (8B, B1)
        };
        if (!makeLayout(kFilterBindings, t, nullptr, mFilterLayout, "filter")) return false;
    }
    {   // rq_probe_integrate.comp: its own eighteen, then the hit set (kIntegrateHitSlots) laid out by its one function
        VkDescriptorSetLayoutBinding hb[kIntegrateBindings] = {};
        hitSetLayout(kIntegrateHitSlots, hb);
        const VkDescriptorType own[kIntegrateHitFirst] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 params
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 1 records
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 normals
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 3 depth
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 4 irradiance
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 5 last frame's history (packed)
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 6 this frame's history (packed)
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 7 the rest mean (PHOTON-GATHER-1d)
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 8 the photon overlay (PHOTON-VIEW-1)
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 9 the frame's counter (word 1: L8)
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 10 the irradiance field's irradiance atlas (7-2)
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 11 ...and its depth atlas
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 12 the prepass's shadow / roughness (7-3)
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 13 the prefiltered maps (linear)
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 14 the rough specular
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 15 the lobe's last frame's history
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 16 ...this frame's
            VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,            // 17 ...its rest mean
        };
        VkDescriptorType t[kIntegrateBindings] = {};
        unsigned counts[kIntegrateBindings] = {};
        for (unsigned i = 0; i < kIntegrateBindings; ++i) {
            t[i] = i < kIntegrateHitFirst ? own[i] : hb[i].descriptorType;
            counts[i] = i < kIntegrateHitFirst ? 1u : ( hb[i].descriptorCount ? hb[i].descriptorCount : 1u );
        }
        t[kIntegrateSkyVisBinding] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;   // the sky visibility (51, 10-4 R11)
        counts[kIntegrateSkyVisBinding] = 1u;
        for (unsigned b : { kIntegrateStreakPrevBinding, kIntegrateStreakCurBinding }) {   // the history's streak (8B)
            t[b] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            counts[b] = 1u;
        }
        for (unsigned b : { kIntegrateClockBinding, kIntegrateCellStateBinding, kIntegrateCellStoreBinding }) {   // the cells' rest (8B)
            t[b] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            counts[b] = 1u;
        }
        if (!makeLayout(kIntegrateBindings, t, counts, mIntegrateLayout, "integrate")) return false;
    }
    {   // rq_probe_resume.comp (PHOTON-7 stage 8B, B1): one layout for its five jobs
        const VkDescriptorType t[kResumeBindings] = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 0 the gather's params
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 1 the resume's own block
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,   // 2 depth
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 3 the cells' state
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 4 the cells' distance store
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 5 the clock
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,           // 6 the changed-region list
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 7 the four brick masks
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,           // 8 the pyramids
        };
        const unsigned c[kResumeBindings] = { 1u, 1u, 1u, 1u, 1u, 1u, 1u, GatherRestInputs::kCascades, 1u };
        if (!makeLayout(kResumeBindings, t, c, mResumeLayout, "resume")) return false;
    }

    const auto makeOne = [&](VkDescriptorSetLayout setLayout, const uint32_t *spv, size_t bytes,
                             VkPipelineLayout &pipeLayout, VkShaderModule &module,
                             VkPipeline &pipeline, const char *what) {
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &setLayout;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &pipeLayout) != VK_SUCCESS) {
            err = std::string("gather: vkCreatePipelineLayout (") + what + ") failed";
            return false;
        }
        VkShaderModuleCreateInfo smi{};
        smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smi.codeSize = bytes;
        smi.pCode = spv;
        if (vkCreateShaderModule(dev, &smi, nullptr, &module) != VK_SUCCESS) {
            err = std::string("gather: vkCreateShaderModule (") + what + ") failed";
            return false;
        }
        VkComputePipelineCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = pipeLayout;
        // THROUGH THE DEVICE'S PIPELINE CACHE (PHOTON-II-1 item 12): the tier's builder.
        if (mHost.gatherCreatePipeline(cpi, &pipeline, (std::string("gather ") + what).c_str()) !=
            VK_SUCCESS) {
            err = std::string("gather: vkCreateComputePipelines (") + what + ") failed";
            return false;
        }
        return true;
    };
    if (!makeOne(mPlaceLayout, krq_probePlaceSelectSpv, sizeof(krq_probePlaceSelectSpv),
                 mPlaceSelectPipeLayout, mPlaceSelectModule, mPlaceSelectPipeline, "place select"))
        return false;
    if (!makeOne(mPlaceLayout, krq_probePlaceSpv, sizeof(krq_probePlaceSpv), mPlacePipeLayout,
                 mPlaceModule, mPlacePipeline, "place"))
        return false;
    if (!makeOne(mTraceLayout, krq_probeGatherSpv, sizeof(krq_probeGatherSpv), mTracePipeLayout,
                 mTraceModule, mTracePipeline, "trace"))
        return false;
    if (!makeOne(mFilterLayout, krq_probeFilterSpv, sizeof(krq_probeFilterSpv), mFilterPipeLayout,
                 mFilterModule, mFilterPipeline, "filter"))
        return false;
    if (!makeOne(mIntegrateLayout, krq_probeIntegrateSpv, sizeof(krq_probeIntegrateSpv),
                 mIntegratePipeLayout, mIntegrateModule, mIntegratePipeline, "integrate"))
        return false;
    {   // THE RESUME's five jobs (PHOTON-7 stage 8B, B1), on one layout
        const uint32_t *spv[kResumeJobs] = { krq_probeResumeSpv, krq_probeResumePyramidSpv, krq_probeResumeClockSpv,
                                             krq_probeResumeFinalSpv, krq_probeResumeRingSpv };
        const size_t bytes[kResumeJobs] = { sizeof(krq_probeResumeSpv), sizeof(krq_probeResumePyramidSpv),
                                            sizeof(krq_probeResumeClockSpv), sizeof(krq_probeResumeFinalSpv),
                                            sizeof(krq_probeResumeRingSpv) };
        const char *what[kResumeJobs] = { "resume test", "resume pyramid", "resume clock", "resume final", "resume ring" };
        for (unsigned j = 0; j < kResumeJobs; ++j)
            if (!makeOne(mResumeLayout, spv[j], bytes[j], mResumePipeLayout[j], mResumeModule[j], mResumePipeline[j],
                         what[j]))
                return false;
    }

    if (!plan.create(dev, "gather", VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, mPool, err))
        return false;

    auto *rs = static_cast<Ogre::VulkanRenderSystem *>(mHost.gatherRenderSystem());
    if (rs && rs->getVulkanDevice()) {
        mMaxWorkGroupX =
            rs->getVulkanDevice()->mDeviceProperties.limits.maxComputeWorkGroupCount[0];
        mMaxImageDim = rs->getVulkanDevice()->mDeviceProperties.limits.maxImageDimension2D;
    }
    return true;
}

bool ScreenProbeGather::ensureTargets(View &v, const GatherInputs &in, unsigned stride,
                                      unsigned octRes, unsigned adaptiveCap, std::string &err) {
    if (v.targetsReady && v.w == in.width && v.h == in.height && v.stride == stride &&
        v.octRes == octRes && v.adaptiveCap == adaptiveCap && v.stereo == in.stereo &&
        v.prefilteredLayers == kRoughSpecLevels + (in.roughQuadrature ? 1u : 0u))   // (the quadrature door's layer)
        return true;
    // OUTSIDE ANY ENCODER FIRST (PHOTON-GATHER-1d, found by the validation
    // selftest once the gather ran by default): this runs in the scene pass's
    // pre-execute hook, and creating the full-resolution irradiance texture below
    // (TextureGpu::_transitionTo) records a layout barrier — inside the pass's open
    // render pass, VUID-vkCmdPipelineBarrier-None-07889. The host's frame command
    // buffer is taken with every encoder ended.
    if (!mHost.gatherFrameCmd()) { err = "gather: no frame command buffer"; return false; }
    drop(v);
    v.w = in.width;
    v.h = in.height;
    v.stride = stride;
    v.octRes = octRes;
    v.adaptiveCap = adaptiveCap;
    // TWO GRIDS SIDE BY SIDE in the probe LIST under stereo, each over its own
    // eye's layer (LAYERED-STEREO-1: `in.width` is one eye's); one otherwise.
    v.stereo = in.stereo;
    v.layers = in.stereo ? 2u : 1u;
    v.eyeW = in.width;
    v.eyeGridW = (v.eyeW + stride - 1u) / stride;
    v.gridW = in.stereo ? 2u * v.eyeGridW : v.eyeGridW;
    v.gridH = (in.height + stride - 1u) / stride;
    v.uniformProbes = v.gridW * v.gridH;
    if (!v.uniformProbes) { err = "gather: empty probe grid"; return false; }
    const unsigned total = v.uniformProbes + adaptiveCap;

    // THE ATLAS. One 8x8 block per probe whatever the tier's octahedral
    // resolution is: a fixed block keeps the addressing arithmetic one multiply
    // (and phase 2's filter reads neighbours by the same rule), and the padding
    // a 6x6 map leaves is 40 % of six megabytes, which is not a reason to make
    // every reader compute a variable stride.
    v.atlasCols = std::max(1u, v.gridW);
    const unsigned rows = (total + v.atlasCols - 1u) / v.atlasCols;
    v.atlasW = v.atlasCols * kGatherOctResMax;
    v.atlasH = rows * kGatherOctResMax;
    // REFUSED, NOT ATTEMPTED, past the device's image ceiling (4096 is the
    // guaranteed floor): an image that large is a lost device, not an error.
    const unsigned maxDim = mMaxImageDim ? mMaxImageDim : 4096u;
    if (v.atlasW > maxDim || v.atlasH > maxDim || in.width > maxDim || in.height > maxDim) {
        err = "gather: the probe atlas (" + std::to_string(v.atlasW) + "x" +
              std::to_string(v.atlasH) + ") or the target exceeds this device's " +
              std::to_string(maxDim) + "-texel image side — a smaller adaptive cap or a coarser "
              "stride is the cure";
        return false;
    }
    if (!mHost.gatherMakeImage(v.atlasW, v.atlasH, 0u, VK_FORMAT_R16G16B16A16_SFLOAT, v.atlas,
                               v.atlasMemory, v.atlasView, err))
        return false;
    // THE PREFILTERED MAPS (PHOTON-7 stage 7, 7-3): the atlas' shape, a layer a roughness level, R11G11B10 (Lumen's
    // probe radiance format: a radiance needs no sign and no alpha) - the rough specular's source.
    // (one more layer under the quadrature door - the filtered map itself; the door switched re-makes the targets)
    v.prefilteredLayers = kRoughSpecLevels + (in.roughQuadrature ? 1u : 0u);
    if (!mHost.gatherMakeImage(v.atlasW, v.atlasH, v.prefilteredLayers, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                               v.prefiltered, v.prefilteredMemory, v.prefilteredView, err))
        return false;
    if (!mHost.gatherMakeBuffer(VkDeviceSize(total) * kRecordBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                false, v.records,
                                v.recordsMemory, nullptr, err))
        return false;
    for (unsigned k = 0; k < 2u; ++k) {
        // THE PIXEL HISTORY at the target's resolution, one packed texel a pixel
        // for each half of the ping-pong (rq_probe_integrate.comp).
        if (!mHost.gatherMakeImage(in.width, in.height, v.layers, VK_FORMAT_R32G32_UINT, v.history[k],
                                   v.historyMemory[k], v.historyView[k], err))
            return false;
        // ...AND ITS VALIDITY STREAK (8B), one byte a pixel beside each half (raw Vulkan images like the history's - no
        // Ogre texture, so no ManualTexture rule applies)
        if (!mHost.gatherMakeImage(in.width, in.height, v.layers, VK_FORMAT_R8_UNORM, v.streak[k], v.streakMemory[k],
                                   v.streakView[k], err))
            return false;
    }
    // THE REST MEAN (PHOTON-GATHER-1d), one image at the target's resolution —
    // and a 1x1 stand-in for a STEREO view, which never rests: a tracked head's
    // basis is never bit-identical from one frame to the next (Monado's
    // simulated HMD sways; a real one jitters), so a headset-sized rest mean
    // (82 MB at the Quest Pro's 10.3 Mpx) would be memory nothing writes. The
    // integrate touches it only on a rest frame, and `record` never counts one
    // for a stereo view.
    const unsigned restW = in.stereo ? 1u : in.width, restH = in.stereo ? 1u : in.height;
    if (!mHost.gatherMakeImage(restW, restH, 1u, VK_FORMAT_R16G16B16A16_SFLOAT, v.restMean,
                               v.restMeanMemory, v.restMeanView, err))
        return false;
    // THE LOBE'S HISTORY PAIR AND REST MEAN (7-3): one shared-exponent word a pixel each half, and the rest mean the
    // diffuse's shape (the stereo stand-in the same).
    for (unsigned k = 0; k < 2u; ++k)
        if (!mHost.gatherMakeImage(in.width, in.height, v.layers, VK_FORMAT_R32_UINT, v.roughHistory[k],
                                   v.roughHistoryMemory[k], v.roughHistoryView[k], err))
            return false;
    if (!mHost.gatherMakeImage(restW, restH, 1u, VK_FORMAT_R16G16B16A16_SFLOAT, v.roughRest, v.roughRestMemory,
                               v.roughRestView, err))
        return false;
    // THE COUNTER: the demand (one word, read back), the integrate's four (words 1-4), and then each eye's row's count
    // of the cells asking for a second probe — what the placement's select pass ranks
    // in a fixed order (rq_probe_place.comp, PHOTON-VIEW-NOISE-1).
    v.placeRows = (in.stereo ? 2u : 1u) * v.gridH;   // one word per eye and grid row
    // (+ two words: each eye's adaptive slots counted out past the share - rq_probe_place.comp's select, 8B B1)
    if (!mHost.gatherMakeBuffer(20u + 4ull * v.placeRows + 8u,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                false, v.counter, v.counterMemory, nullptr, err))
        return false;
    // THE INDIRECT ARGUMENTS the trace dispatches from, written by the
    // placement job's own atomic max (see its header) over a value this file
    // sets by a transfer at the head of every frame.
    if (!mHost.gatherMakeBuffer(16u,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                false, v.args, v.argsMemory, nullptr, err))
        return false;
    if (!mHost.gatherMakeBuffer(kFramesInFlight * kReadbackSlotBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                v.readback, v.readbackMemory, &v.readbackMapped, err))
        return false;
    // THE CELLS' REST (PHOTON-7 stage 8B, B1; jah_probe_rest.glsl): a cell of the uniform grid each - its state and
    // ring words, its distance store (520 B), and the view's clock - device-local, zeroed by the first frame's
    // commands (every cell active, the clock at 0: cellsCleared).
    {
        const VkDeviceSize cells = VkDeviceSize(v.uniformProbes);
        const VkBufferUsageFlags use = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!mHost.gatherMakeBuffer(cells * kCellStateWords * 4u, use, false, v.cellState, v.cellStateMemory, nullptr,
                                    err) ||
            !mHost.gatherMakeBuffer(cells * kCellStoreWords * 4u, use, false, v.cellStore, v.cellStoreMemory, nullptr,
                                    err) ||
            !mHost.gatherMakeBuffer(clockWordsOf(v.gridW, v.gridH) * 4u, use, false, v.clock, v.clockMemory, nullptr, err))
            return false;
        v.cellsCleared = false;
    }

    // THE FULL-RESOLUTION IRRADIANCE. `Uav` and nothing else: a UAV that is not
    // a render target is born in VK_IMAGE_LAYOUT_GENERAL with the pin's own
    // barrier, it carries SAMPLED usage because it is a texture, and it is NOT
    // Reinterpretable — a float storage image viewed as its own format must not
    // be (PHOTON-M3's trap: the family of every RGBA16 format is the UINT one).
    Ogre::TextureGpuManager *tm = mHost.gatherRenderSystem()->getTextureGpuManager();
    static unsigned sSerial = 0u;
    Ogre::TextureGpu *t = tm->createTexture("JahProbeIrradiance/" + std::to_string(++sSerial),
                                            Ogre::GpuPageOutStrategy::Discard,
                                            Ogre::TextureFlags::Uav,
                                            in.stereo ? Ogre::TextureTypes::Type2DArray
                                                      : Ogre::TextureTypes::Type2D);
    // ONE LAYER PER EYE under stereo (LAYERED-STEREO-1): the forward pass reads it
    // at its own gl_ViewIndex (JahProbeGather_piece_ps.any).
    t->setResolution(in.width, in.height, v.layers);
    t->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
    t->setNumMipmaps(1u);
    // RESIDENT FOR GOOD, never per frame — the 0071 lesson.
    t->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    v.irradiance = t;
    // THE ROUGH SPECULAR (7-3), `jahRoughSpec`: the irradiance's twin - a UAV the integrate writes and the shading pass
    // reads at its own fragment (JahProbeGather_piece_ps.any), one layer per eye.
    Ogre::TextureGpu *r = tm->createTexture("JahRoughSpec/" + std::to_string(sSerial), Ogre::GpuPageOutStrategy::Discard,
                                            Ogre::TextureFlags::Uav,
                                            in.stereo ? Ogre::TextureTypes::Type2DArray : Ogre::TextureTypes::Type2D);
    r->setResolution(in.width, in.height, v.layers);
    r->setPixelFormat(Ogre::PFG_RGBA16_FLOAT);
    r->setNumMipmaps(1u);
    r->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    v.roughSpec = r;
    // THE SKY VISIBILITY (PHOTON-7 10-4 R11), `jahSkyVis`: R8, the integrate's, read by the colour passes' fog
    Ogre::TextureGpu *sv = tm->createTexture("JahSkyVis/" + std::to_string(sSerial), Ogre::GpuPageOutStrategy::Discard,
                                             Ogre::TextureFlags::Uav,
                                             in.stereo ? Ogre::TextureTypes::Type2DArray : Ogre::TextureTypes::Type2D);
    sv->setResolution(in.width, in.height, v.layers);
    sv->setPixelFormat(Ogre::PFG_R8_UNORM);
    sv->setNumMipmaps(1u);
    sv->_transitionTo(Ogre::GpuResidency::Resident, nullptr);
    v.skyVis = sv;

    // The atlas, the records, the irradiance target (8 bytes a pixel), the
    // pixel history's two packed halves (8 bytes a pixel each) and the rest mean
    // (8 bytes a pixel; none under stereo) - and THE ROUGH SPECULAR's (7-3): the
    // prefiltered maps (4 bytes an atlas texel a level), the output (8 bytes a pixel), the lobe's history pair (4
    // bytes a pixel each) and its rest mean (8; none under stereo).
    const unsigned long long pixels = (unsigned long long)in.width * in.height * v.layers;
    v.historyBytes = pixels * 2ull * kHistoryTexelBytes;
    v.vramBytes = 1ull * v.atlasW * v.atlasH * 8ull + (unsigned long long)total * kRecordBytes +
                  pixels * 8ull + v.historyBytes + (unsigned long long)restW * restH * 8ull +
                  1ull * v.atlasW * v.atlasH * 4ull * v.prefilteredLayers + pixels * 8ull + pixels * 2ull * 4ull +
                  (unsigned long long)restW * restH * 8ull + pixels * 1ull +   // (+ the sky visibility, 1 byte a pixel)
                  (unsigned long long)v.uniformProbes * (kCellStateWords + kCellStoreWords) * 4ull +
                  clockWordsOf(v.gridW, v.gridH) * 4ull;   // (+ the cells' rest, 8B B1: 568 B a cell; the pyramids as sized)
    v.targetsReady = true;
    v.atlasNeedsClear = true;
    v.age = 0u;
    v.flip = 0u;
    return true;
}

void ScreenProbeGather::drop(View &v) {
    for (unsigned i = 0; i < kRing; ++i) {
        mHost.gatherRetireSet(v.placeSets[i], mPool);
        mHost.gatherRetireSet(v.traceSets[i], mPool);
        mHost.gatherRetireSet(v.filterSets[i], mPool);
        mHost.gatherRetireSet(v.integrateSets[i], mPool);
        mHost.gatherRetireSet(v.resumeSets[i], mPool);
        v.placeSets[i] = v.traceSets[i] = v.filterSets[i] = v.integrateSets[i] = v.resumeSets[i] = VK_NULL_HANDLE;
        mHost.gatherRetireBuffer(v.restParams[i], v.restParamsMemory[i]);
        v.restParams[i] = VK_NULL_HANDLE;
        v.restParamsMemory[i] = VK_NULL_HANDLE;
        v.restParamsMapped[i] = nullptr;
        mHost.gatherRetireBuffer(v.params[i], v.paramsMemory[i]);
        v.params[i] = VK_NULL_HANDLE;
        v.paramsMemory[i] = VK_NULL_HANDLE;
        v.paramsMapped[i] = nullptr;
        mHost.gatherRetireBuffer(v.geomRowOfSlot[i], v.geomRowOfSlotMemory[i]);
        v.geomRowOfSlot[i] = VK_NULL_HANDLE;
        v.geomRowOfSlotMemory[i] = VK_NULL_HANDLE;
        v.geomRowOfSlotMapped[i] = nullptr;
        v.geomRowOfSlotBytes[i] = 0u;
    }
    mHost.gatherRetireBuffer(v.records, v.recordsMemory);
    for (unsigned k = 0; k < 2u; ++k) {
        mHost.gatherRetireImage(v.history[k], v.historyMemory[k], v.historyView[k]);
        mHost.gatherRetireImage(v.streak[k], v.streakMemory[k], v.streakView[k]);
        v.streak[k] = VK_NULL_HANDLE;
        v.streakMemory[k] = VK_NULL_HANDLE;
        v.streakView[k] = VK_NULL_HANDLE;
        v.history[k] = VK_NULL_HANDLE;
        v.historyMemory[k] = VK_NULL_HANDLE;
        v.historyView[k] = VK_NULL_HANDLE;
    }
    mHost.gatherRetireImage(v.restMean, v.restMeanMemory, v.restMeanView);
    v.restMean = VK_NULL_HANDLE;
    v.restMeanMemory = VK_NULL_HANDLE;
    v.restMeanView = VK_NULL_HANDLE;
    for (unsigned k = 0; k < 2u; ++k) {
        mHost.gatherRetireImage(v.roughHistory[k], v.roughHistoryMemory[k], v.roughHistoryView[k]);
        v.roughHistory[k] = VK_NULL_HANDLE;
        v.roughHistoryMemory[k] = VK_NULL_HANDLE;
        v.roughHistoryView[k] = VK_NULL_HANDLE;
    }
    mHost.gatherRetireImage(v.roughRest, v.roughRestMemory, v.roughRestView);
    v.roughRest = VK_NULL_HANDLE;
    v.roughRestMemory = VK_NULL_HANDLE;
    v.roughRestView = VK_NULL_HANDLE;
    mHost.gatherRetireImage(v.prefiltered, v.prefilteredMemory, v.prefilteredView);
    v.prefiltered = VK_NULL_HANDLE;
    v.prefilteredMemory = VK_NULL_HANDLE;
    v.prefilteredView = VK_NULL_HANDLE;
    v.restFrames = 0u;
    v.sinceRestart = 0u;
    v.age = 0u;
    // (the cells' rest: their buffers, and every count read from them)
    mHost.gatherRetireBuffer(v.cellState, v.cellStateMemory);
    mHost.gatherRetireBuffer(v.cellStore, v.cellStoreMemory);
    mHost.gatherRetireBuffer(v.clock, v.clockMemory);
    mHost.gatherRetireBuffer(v.pyramid, v.pyramidMemory);
    v.cellState = v.cellStore = v.clock = v.pyramid = VK_NULL_HANDLE;
    v.cellStateMemory = v.cellStoreMemory = v.clockMemory = v.pyramidMemory = VK_NULL_HANDLE;
    v.pyramidBytes = 0u;
    v.cellsCleared = false;
    v.clockKnown = false;
    v.clockFrame = v.clockK = v.cellsReached = v.cellsHeld = v.cellsActive = v.cellsRing = 0u;
    v.restLog.clear();
    mHost.gatherRetireBuffer(v.counter, v.counterMemory);
    mHost.gatherRetireBuffer(v.args, v.argsMemory);
    mHost.gatherRetireBuffer(v.readback, v.readbackMemory);
    mHost.gatherRetireBuffer(v.irrReadback, v.irrReadbackMemory);
    mHost.gatherRetireBuffer(v.recReadback, v.recReadbackMemory);
    v.recReadback = VK_NULL_HANDLE;
    v.recReadbackMemory = VK_NULL_HANDLE;
    v.recReadbackMapped = nullptr;
    v.adaptiveHost.clear();
    v.irrReadback = VK_NULL_HANDLE;
    v.irrReadbackMemory = VK_NULL_HANDLE;
    v.irrReadbackMapped = nullptr;
    v.irrHost.clear();
    v.irrHostFrame = 0u;
    mHost.gatherRetireBuffer(v.histReadback, v.histReadbackMemory);
    v.histReadback = VK_NULL_HANDLE;
    v.histReadbackMemory = VK_NULL_HANDLE;
    v.histReadbackMapped = nullptr;
    v.histHost.clear();
    v.histStreakHost.clear();
    v.histClassHost.clear();
    v.histDistHost.clear();
    v.histHostFrame = 0u;
    mHost.gatherRetireBuffer(v.rays, v.raysMemory);
    mHost.gatherRetireBuffer(v.raysReadback, v.raysReadbackMemory);
    mHost.gatherRetireBuffer(v.raysAtlasReadback, v.raysAtlasReadbackMemory);
    v.rays = v.raysReadback = v.raysAtlasReadback = VK_NULL_HANDLE;
    v.raysMemory = v.raysReadbackMemory = v.raysAtlasReadbackMemory = VK_NULL_HANDLE;
    v.raysReadbackMapped = v.raysAtlasReadbackMapped = nullptr;
    v.raysHost.clear();
    v.raysHostFrame = 0u;
    v.records = v.counter = v.args = v.readback = VK_NULL_HANDLE;
    v.recordsMemory = v.counterMemory = v.argsMemory = v.readbackMemory = VK_NULL_HANDLE;
    v.readbackMapped = nullptr;
    mHost.gatherRetireImage(v.atlas, v.atlasMemory, v.atlasView);
    v.atlas = VK_NULL_HANDLE;
    v.atlasMemory = VK_NULL_HANDLE;
    v.atlasView = VK_NULL_HANDLE;
    if (v.irradiance) mHost.gatherRetireTexture(v.irradiance);
    v.irradiance = nullptr;
    if (v.roughSpec) mHost.gatherRetireTexture(v.roughSpec);
    v.roughSpec = nullptr;
    if (v.skyVis) mHost.gatherRetireTexture(v.skyVis);
    v.skyVis = nullptr;
    v.targetsReady = false;
    v.atlasNeedsClear = false;
    // ...AND THE IN-FLIGHT RECORDS GO WITH THE BUFFERS THEY NAME. A resize frees
    // the readback ring; leaving `pending[]` live would make the next
    // `readPending` read a slot of a buffer that no longer exists. The frame
    // counter goes back to zero for the same reason: it indexes the ring.
    for (unsigned i = 0; i < kFramesInFlight; ++i) v.pending[i] = View::Pending();
    v.frame = 0u;
    v.adaptiveLast = 0u;
    v.adaptiveAsked = 0u;
    v.cpuMs = -1.0f;
}

void ScreenProbeGather::readPending(View &v) {
    const uint32_t nowAll = mHost.gatherFrameNow(), inFlightAll = mHost.gatherFramesInFlight();
    // THE ADAPTIVE COUNT: a COUNT, not a measurement, and `giStatus()` reports
    // it beside the probe grid.
    if (v.readbackMapped) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) {
            if (!v.pending[i].live || uint32_t(nowAll - v.pending[i].frame) < inFlightAll) continue;
            if (!v.pending[i].held) {
            uint32_t appended = 0u;
            std::memcpy(&appended, static_cast<const char *>(v.readbackMapped) + i * kReadbackSlotBytes,
                        sizeof(appended));
            // The shader's counter keeps counting past the cap (an atomicAdd
            // cannot un-count), so what a frame actually APPENDED is the
            // smaller of the two.
            v.adaptiveAsked = appended;
            v.adaptiveLast = std::min(appended, v.adaptiveCap);
            uint32_t foliageRule = 0u;
            std::memcpy(&foliageRule, static_cast<const char *>(v.readbackMapped) + i * kReadbackSlotBytes + 4u,
                        sizeof(foliageRule));
            v.foliageRuleAccepts += foliageRule;
            // ...and the pixels that reached the hold with no rest sample (PHOTON-7 stage 10-3: NO SAMPLE there),
            // the diffuse's and the rough specular's - the last traced frame's
            uint32_t starved[2] = { 0u, 0u };
            std::memcpy(starved, static_cast<const char *>(v.readbackMapped) + i * kReadbackSlotBytes + 8u, sizeof(starved));
            v.starvedDiffuse = starved[0];
            v.starvedRough = starved[1];
            // ...AND THE CELLS' REST of that frame (PHOTON-7 stage 8B, B1): its clock as the finalize and the ring left it
            uint32_t clock[kClockWords] = {};
            std::memcpy(clock, static_cast<const char *>(v.readbackMapped) + i * kReadbackSlotBytes + 20u, sizeof(clock));
            uint32_t handoverM = 0u;   // (B3: the longest hand-over the frame began)
            std::memcpy(&handoverM, static_cast<const char *>(v.readbackMapped) + i * kReadbackSlotBytes + 16u,
                        sizeof(handoverM));
            if (!v.clockKnown || int32_t(v.pending[i].gatherFrame - v.clockFrame) > 0) {
                v.clockKnown = true;
                v.clockFrame = v.pending[i].gatherFrame;
                v.clockK = clock[0];
                v.cellsReached = clock[3];
                v.cellsHeld = clock[4];
                v.cellsActive = clock[5];
                v.resumes = clock[6];
                v.cellsRing = clock[7];
                GatherRestRecord rec;
                rec.frame = v.pending[i].gatherFrame;
                rec.restK = std::min(clock[0], 1u << 20);   // (0xFFFFFFFF, no active component: every cell held)
                rec.reached = clock[3];
                rec.refined = (clock[2] & 2u) != 0u;
                rec.held = clock[4];
                rec.active = clock[5];
                rec.everyHeld = clock[5] == 0u && clock[4] > 0u;
                rec.movingProbes = clock[1] & 0xFFFFu;   // (B5: the trace's counts, read below 65,536 probes)
                rec.fastProbes = clock[1] >> 16u;
                rec.handoverM = handoverM;
                v.restLog.push_back(rec);
                if (v.restLog.size() > 32u) v.restLog.erase(v.restLog.begin());
            }
            }
            // ...AND THE HISTORY COUNT of the same retired frame (PHOTON-2 stage 0): the second word's
            // bits 22-25 of each rg32ui texel (jah_probe_history.glsl packGeom), the eye layers side by side.
            if (v.pending[i].history && v.histReadbackMapped) {
                const size_t layerTexels = size_t(v.w) * v.h;
                const char *slot = static_cast<const char *>(v.histReadbackMapped) + size_t(i) * layerTexels * v.layers * 9u;
                const uint32_t *src = reinterpret_cast<const uint32_t *>(slot);
                detail::decodeGatherHistory(src, v.w, v.h, v.layers, v.histHost, v.histClassHost,
                                            v.histDistHost);   // -O2 (ReadbackDecode.h)
                // ...and the streak, its layers side by side as the count's (8B)
                {
                    const unsigned char *st = reinterpret_cast<const unsigned char *>(slot + layerTexels * v.layers * 8u);
                    v.histStreakHost.assign(size_t(v.w) * v.layers * v.h, 0u);
                    for (unsigned l = 0; l < v.layers; ++l)
                        for (unsigned y = 0; y < v.h; ++y)
                            for (unsigned x = 0; x < v.w; ++x)
                                v.histStreakHost[size_t(y) * v.w * v.layers + size_t(l) * v.w + x] =
                                    st[size_t(l) * layerTexels + size_t(y) * v.w + x];
                }
                v.histHostFrame = v.pending[i].gatherFrame;
                v.pending[i].history = false;
            }
            // ...AND THE IRRADIANCE READBACK of the same retired frame, decoded
            // from half floats (a test door; see GatherTuning::readback).
            if (v.pending[i].irradiance && v.irrReadbackMapped) {
                // The layers arrive one after another and are laid side by side
                // (LAYERED-STEREO-1): a stereo readback is the eye pair as one
                // image, `irradianceW` the pair's width.
                const size_t layerTexels = size_t(v.w) * v.h;
                const size_t texels = layerTexels * v.layers;
                const uint16_t *src = reinterpret_cast<const uint16_t *>(
                    static_cast<const char *>(v.irrReadbackMapped) + i * texels * 8u);
                v.irrHost.resize(texels * 4u);
                const size_t pairW = size_t(v.w) * v.layers;
                for (unsigned k = 0; k < v.layers; ++k)
                    for (unsigned y = 0; y < v.h; ++y)
                        for (size_t x = 0; x < size_t(v.w) * 4u; ++x)
                            v.irrHost[(y * pairW + size_t(k) * v.w) * 4u + x] = Ogre::Bitwise::halfToFloat(
                                src[(size_t(k) * layerTexels + size_t(y) * v.w) * 4u + x]);
                v.irrHostFrame = v.pending[i].gatherFrame;
                v.pending[i].irradiance = false;
                // ...and the uniform cells' twin links (each record's normalW.w).
                if (v.recReadbackMapped) {
                    const char *rec = static_cast<const char *>(v.recReadbackMapped) +
                                      size_t(i) * v.uniformProbes * kRecordBytes;
                    v.adaptiveHost.assign(v.uniformProbes, 0u);
                    for (unsigned c = 0; c < v.uniformProbes; ++c) {
                        float link = 0.0f;
                        std::memcpy(&link, rec + size_t(c) * kRecordBytes + 28u, sizeof(link));
                        v.adaptiveHost[c] = link > 0.5f ? 1u : 0u;
                    }
                }
            }
        }
    }
    // ...AND THE GATHER-HITS READBACK of a retired traced frame (FAR-SUN-1): the per-ray
    // records, each with the radiance its atlas texel holds (the decode's write-back in it).
    if (v.raysReadbackMapped && v.raysAtlasReadbackMapped) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) {
            if (!v.pending[i].live || !v.pending[i].rays ||
                uint32_t(nowAll - v.pending[i].frame) < inFlightAll)
                continue;
            v.pending[i].rays = false;
            const uint32_t *rec = reinterpret_cast<const uint32_t *>(
                static_cast<const char *>(v.raysReadbackMapped) + size_t(i) * v.raysSlotBytes());
            const uint16_t *atl = reinterpret_cast<const uint16_t *>(
                static_cast<const char *>(v.raysAtlasReadbackMapped) + size_t(i) * v.raysAtlasSlotBytes());
            const unsigned rays = v.octRes * v.octRes;
            v.raysHost.assign(size_t(v.uniformProbes) * rays, GatherRay());
            const auto half = [](uint32_t w, int k) {
                return Ogre::Bitwise::halfToFloat(uint16_t(k ? (w >> 16u) : (w & 0xFFFFu)));
            };
            for (unsigned pr = 0; pr < v.uniformProbes; ++pr)
                for (unsigned r = 0; r < rays; ++r) {
                    GatherRay &g = v.raysHost[size_t(pr) * rays + r];
                    g.cellX = (unsigned short)(pr % v.gridW);
                    g.cellY = (unsigned short)(pr / v.gridW);
                    g.ray = (unsigned char)r;
                    const uint32_t *w = rec + (size_t(pr) * 64u + r) * 8u;   // two uvec4 a ray (rq_probe_gather.comp)
                    if (!(w[0] & (1u << 24u))) continue;   // no probe traced it
                    g.source = (unsigned char)(w[0] & 0xFu);
                    g.cascade = (unsigned char)((w[0] >> 4u) & 0xFu);
                    g.sunVisibility = float((w[0] >> 8u) & 0xFFu) / 255.0f;
                    g.hitNormal[0] = float(int8_t(uint8_t((w[1] >> 16u) & 0xFFu))) / 127.0f;
                    g.hitNormal[1] = float(int8_t(uint8_t((w[0] >> 16u) & 0xFFu))) / 127.0f;
                    g.hitNormal[2] = float(int8_t(uint8_t((w[1] >> 24u) & 0xFFu))) / 127.0f;
                    const float reweight = half(w[1], 0);
                    g.weight = reweight;
                    // the hit's position and distance as FLOATS (the second uvec4: REDS-1 - half precision, +-2 mm at a
                    // few metres, could not name the 1/64 m card texel a hit answered from)
                    std::memcpy(g.hitPos, w + 4, 3 * sizeof(float));
                    std::memcpy(&g.distance, w + 7, sizeof(float));
                    // the ray's texel in the atlas: the probe's 8 x 8 block, the octahedral texel
                    const unsigned bx = (pr % v.atlasCols) * 8u + r % v.octRes;
                    const unsigned by = (pr / v.atlasCols) * 8u + r / v.octRes;
                    const uint16_t *t = atl + (size_t(by) * v.atlasW + bx) * 4u;
                    for (int k = 0; k < 3; ++k)
                        g.radiance[k] = reweight > 1e-9f ? Ogre::Bitwise::halfToFloat(t[k]) / reweight : 0.0f;
                }
            v.raysHostFrame = v.pending[i].gatherFrame;
        }
    }
    // A retired frame's record is done with (its readbacks were read above).
    for (unsigned i = 0; i < kFramesInFlight; ++i)
        if (v.pending[i].live && uint32_t(nowAll - v.pending[i].frame) >= inFlightAll)
            v.pending[i].live = false;
}

void ScreenProbeGather::clearAtlas(View &v, VkCommandBuffer cmd) {
    if (!v.atlasNeedsClear) return;
    v.atlasNeedsClear = false;
    // UNDEFINED -> GENERAL and zeroed. A probe block of zero radiance at a
    // distance of zero is what an untraced probe reads as, which is what the
    // integrate hands back to the pixel path.
    // Every image: the atlas and both history halves (a zero history texel is "no
    // surface", which every distance test rejects — though nothing reads them
    // before the view's age says so).
    // ...and the rest mean (PHOTON-GATHER-1d), which the integrate binds on every
    // frame and writes only at rest; and the rough specular's prefiltered maps, history pair and rest mean (7-3).
    constexpr int kImages = 10;   // (+ the streak pair, 8B: a new history's streak is 0 - never a stale one)
    const VkImage images[kImages] = { v.atlas,       v.history[0],      v.history[1],      v.restMean,
                                      v.prefiltered, v.roughHistory[0], v.roughHistory[1], v.roughRest,
                                      v.streak[0],   v.streak[1] };
    VkImageMemoryBarrier b[kImages] = {};
    for (int i = 0; i < kImages; ++i) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = images[i];
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;   // every eye's layer
        b[i].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, kImages, b);
    VkClearColorValue zero{};
    for (int i = 0; i < kImages; ++i)
        vkCmdClearColorImage(cmd, images[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &b[i].subresourceRange);
    VkMemoryBarrier toCompute{};
    toCompute.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    toCompute.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toCompute.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &toCompute, 0, nullptr, 0, nullptr);
}

void ScreenProbeGather::releaseBinding(const void *key) {
    auto it = mViews.find(key);
    if (it == mViews.end() || !it->second.sceneMgr) return;
    detail::FogHlmsListener::setProbeGather(it->second.sceneMgr, detail::FogHlmsListener::ProbeGatherBind());
}

Ogre::TextureGpu *ScreenProbeGather::skyVisOf(const void *key) const {
    auto it = mViews.find(key);
    if (it == mViews.end()) return nullptr;
    const View &v = it->second;
    return v.sceneMgr && v.skyVis && v.age > 0u ? v.skyVis : nullptr;
}

bool ScreenProbeGather::rebindThisFrame(const void *key) {
    auto it = mViews.find(key);
    if (it == mViews.end()) return false;
    const View &v = it->second;
    if (!v.sceneMgr || !v.irradiance || v.recordedFrame != mHost.gatherFrameNow()) return false;
    detail::FogHlmsListener::setProbeGather(v.sceneMgr, { v.irradiance, v.roughSpec, nullptr, v.skyVis });
    return true;
}

bool ScreenProbeGather::holdLast(const void *key, unsigned w, unsigned h, unsigned layers) {
    auto it = mViews.find(key);
    if (it == mViews.end()) return false;
    View &v = it->second;
    if (!v.sceneMgr || !v.irradiance || !v.roughSpec || !v.age || v.recordedFrame == mHost.gatherFrameNow()) return false;
    if (v.w != w || v.h != h || v.layers != layers) return false;
    Ogre::RenderSystem *rs = mHost.gatherRenderSystem();
    {
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        solver.resolveTransition(trans, v.roughSpec, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        solver.resolveTransition(trans, v.skyVis, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    detail::FogHlmsListener::setProbeGather(v.sceneMgr, { v.irradiance, v.roughSpec, nullptr, v.skyVis });
    return true;
}

void ScreenProbeGather::forget(const void *key, bool fold) {
    auto it = mViews.find(key);
    if (it == mViews.end()) return;
    // THE SHOT-VIEW FOLD (mForgotten): the newest traced, non-held slot's starved counts, read now - a slot the
    // device has finished (a shot's readback waited for it) though it has not retired.
    {
        View &v = it->second;
        if (v.readbackMapped) {
            bool any = false;
            uint32_t newest = 0u;
            for (unsigned i = 0; i < kFramesInFlight; ++i) {
                if (!v.pending[i].live || v.pending[i].held) continue;
                if (any && int32_t(v.pending[i].frame - newest) <= 0) continue;
                uint32_t starved[2] = { 0u, 0u };
                std::memcpy(starved, static_cast<const char *>(v.readbackMapped) + i * kReadbackSlotBytes + 8u, sizeof(starved));
                v.starvedDiffuse = starved[0];
                v.starvedRough = starved[1];
                newest = v.pending[i].frame;
                any = true;
            }
        }
        if (fold && v.scene && v.targetsReady) mForgotten[v.scene] = ForgottenView{ v.starvedDiffuse, v.starvedRough };
    }
    // ...AND THE SHADER'S REGISTRATION WITH IT: this view's irradiance texture
    // is about to be retired, and the Hlms listener holds a raw pointer to it
    // keyed by the scene's manager (GATHER-0's D1).
    if (it->second.sceneMgr)
        detail::FogHlmsListener::setProbeGather(it->second.sceneMgr, detail::FogHlmsListener::ProbeGatherBind());
    drop(it->second);
    mViews.erase(it);
    detail::monitor::forgetGpuOwner(key);
}

void ScreenProbeGather::forgetScene(const detail::OgreScene *scene) {
    // THE SHOT-VIEW FOLD's entry dies with its scene (a new scene at its address never reads it). Not with the scene's
    // last view: a shot view is often the only gather view the scene holds when it is destroyed (the viewport's own is
    // made again when it next records).
    mForgotten.erase(scene);
}

void ScreenProbeGather::close() {
    VkDevice dev = mHost.gatherDevice();
    for (auto &kv : mViews) {
        if (kv.second.sceneMgr)
            detail::FogHlmsListener::setProbeGather(kv.second.sceneMgr, detail::FogHlmsListener::ProbeGatherBind());
        // NOT `drop()` HERE, AND THE DIFFERENCE IS A USE-AFTER-FREE (the lead's
        // read). `drop` RETIRES a view's descriptor sets into the host's bin,
        // which frees them later with `vkFreeDescriptorSets(pool, …)` — and the
        // pool is destroyed four lines below, so the flush at the end of the
        // tier's own close() would free sets out of a pool that no longer
        // exists. At close the device is idle and the retire window is not
        // needed for anything: `vkDestroyDescriptorPool` frees every set it
        // ever handed out, so the sets are simply DROPPED here and the buffers,
        // images and textures — which the pool does not own — take the bin as
        // usual.
        View &v = kv.second;
        for (unsigned i = 0; i < kRing; ++i)
            v.placeSets[i] = v.traceSets[i] = v.filterSets[i] = v.integrateSets[i] = v.resumeSets[i] = VK_NULL_HANDLE;
        drop(v);
    }
    mViews.clear();
    // (Every view's registration went in the loop above. The registrations are per
    // scene now — FogHlmsListener::SceneBinds — so none can outlive its scene, and the old
    // clear-all belt has nothing left to clear.)
    if (!dev) return;
    if (mPool) vkDestroyDescriptorPool(dev, mPool, nullptr);
    if (mPlacePipeline) vkDestroyPipeline(dev, mPlacePipeline, nullptr);
    if (mPlaceSelectPipeline) vkDestroyPipeline(dev, mPlaceSelectPipeline, nullptr);
    if (mPlaceSelectModule) vkDestroyShaderModule(dev, mPlaceSelectModule, nullptr);
    if (mPlaceSelectPipeLayout) vkDestroyPipelineLayout(dev, mPlaceSelectPipeLayout, nullptr);
    mPlaceSelectPipeline = VK_NULL_HANDLE;
    mPlaceSelectModule = VK_NULL_HANDLE;
    mPlaceSelectPipeLayout = VK_NULL_HANDLE;
    if (mTracePipeline) vkDestroyPipeline(dev, mTracePipeline, nullptr);
    if (mFilterPipeline) vkDestroyPipeline(dev, mFilterPipeline, nullptr);
    if (mIntegratePipeline) vkDestroyPipeline(dev, mIntegratePipeline, nullptr);
    if (mPlaceModule) vkDestroyShaderModule(dev, mPlaceModule, nullptr);
    if (mTraceModule) vkDestroyShaderModule(dev, mTraceModule, nullptr);
    if (mFilterModule) vkDestroyShaderModule(dev, mFilterModule, nullptr);
    if (mIntegrateModule) vkDestroyShaderModule(dev, mIntegrateModule, nullptr);
    if (mPlacePipeLayout) vkDestroyPipelineLayout(dev, mPlacePipeLayout, nullptr);
    if (mTracePipeLayout) vkDestroyPipelineLayout(dev, mTracePipeLayout, nullptr);
    if (mFilterPipeLayout) vkDestroyPipelineLayout(dev, mFilterPipeLayout, nullptr);
    if (mIntegratePipeLayout) vkDestroyPipelineLayout(dev, mIntegratePipeLayout, nullptr);
    if (mPlaceLayout) vkDestroyDescriptorSetLayout(dev, mPlaceLayout, nullptr);
    if (mTraceLayout) vkDestroyDescriptorSetLayout(dev, mTraceLayout, nullptr);
    if (mFilterLayout) vkDestroyDescriptorSetLayout(dev, mFilterLayout, nullptr);
    if (mIntegrateLayout) vkDestroyDescriptorSetLayout(dev, mIntegrateLayout, nullptr);
    for (unsigned j = 0; j < kResumeJobs; ++j) {
        if (mResumePipeline[j]) vkDestroyPipeline(dev, mResumePipeline[j], nullptr);
        if (mResumeModule[j]) vkDestroyShaderModule(dev, mResumeModule[j], nullptr);
        if (mResumePipeLayout[j]) vkDestroyPipelineLayout(dev, mResumePipeLayout[j], nullptr);
        mResumePipeline[j] = VK_NULL_HANDLE;
        mResumeModule[j] = VK_NULL_HANDLE;
        mResumePipeLayout[j] = VK_NULL_HANDLE;
    }
    if (mResumeLayout) vkDestroyDescriptorSetLayout(dev, mResumeLayout, nullptr);
    mResumeLayout = VK_NULL_HANDLE;
    mPool = VK_NULL_HANDLE;
    mPlacePipeline = mTracePipeline = mFilterPipeline = mIntegratePipeline = VK_NULL_HANDLE;
    mPlaceModule = mTraceModule = mFilterModule = mIntegrateModule = VK_NULL_HANDLE;
    mPlacePipeLayout = mTracePipeLayout = mFilterPipeLayout = mIntegratePipeLayout = VK_NULL_HANDLE;
    mPlaceLayout = mTraceLayout = mFilterLayout = mIntegrateLayout = VK_NULL_HANDLE;
}

unsigned ScreenProbeGather::shippedRestLength() {
    return restLengthOf(GatherTuning(), historyFramesOf(GatherTuning()));
}

/// THE REST CLOCK AS THE HOST KNOWS IT (PHOTON-7 stage 8B, B1): the GPU decides each component's K (a reach or a cell
/// still converging keeps it at 0), so the host has the least K over the active components as the last retired clock
/// read it, a few frames late - never extended (no frame count says a component stepped). The view's last traced frame
/// is `v.frame - 1`; before any clock retired, the frames since the last input.
unsigned ScreenProbeGather::restKnownOf(const View &v) {
    if (v.frame == 0u) return 0u;
    const unsigned last = v.frame - 1u;
    if (!v.clockKnown || int32_t(v.clockFrame - v.lastMovedFrame) < 0) return last - std::min(last, v.lastInputFrame);
    if (v.clockK == 0xFFFFFFFFu) return 1u << 20;   // (no active component: every cell held)
    return v.clockK;   // (never extended: a component still converging keeps its clock at 0 with no input)
}

/// EVERY CELL HOLDS, as the host can say it: the view held whole, or the last retired clock counted no active cell and no
/// input came since it (an input since may have resumed a cell: unknown until its own clock retires - not held meanwhile).
bool ScreenProbeGather::everyCellHeldOf(const View &v, unsigned restN) {
    (void)restN;
    if (v.heldLast) return true;
    if (!v.clockKnown || int32_t(v.lastInputFrame - v.clockFrame) > 0) return false;
    return v.cellsActive == 0u && v.cellsHeld > 0u;
}

bool ScreenProbeGather::restDemandComplete(const detail::OgreScene *scene, unsigned &restFrames, unsigned &restLength,
                                           unsigned due, bool &active) const {
    restFrames = 0u;
    restLength = shippedRestLength();
    active = false;
    const View *latest = nullptr;
    for (const auto &kv : mViews) {
        const View &v = kv.second;
        if (v.scene != scene || !v.targetsReady) continue;
        if (!latest || int32_t(v.recordedFrame - latest->recordedFrame) > 0) latest = &v;
    }
    // A READER THAT NO LONGER RECORDS MAKES NO DEMAND (PHOTON-7 stage 10, the giAtRest contract): a view that has not
    // recorded within the frames in flight (+1: the cache plans a frame late) OF ITS SCENE'S DRAWS is not reading the
    // cards - the editor view while a screenshot's offscreen settle draws only the shot's view - and its frozen rest
    // would hold the demand open for ever. No reader: nothing to wait for, the demand is complete at once. TIMED ON THE
    // SCENE'S DRAWS (OgreScene::drawSerial, PHOTON-7 stage 10-3, F1), never on Ogre's frame count: a window's resize ran
    // six engine frames that drew no view of the scene, both readers read as gone, and the cards' one rest decision was
    // taken on an empty demand - every card drained, none admitted, for the session.
    if (!latest || uint32_t(scene->drawSerial() - latest->recordedDraw) > kReaderStaleFrames) return true;
    active = true;
    for (const auto &kv : mViews) {
        const View &o = kv.second;
        if (o.scene != scene || !o.targetsReady || o.recordedFrame != latest->recordedFrame || !o.restableLast) continue;
        const unsigned rest = o.heldLast ? std::max(restKnownOf(o), restLengthOf(o.tuningLast, o.historyFramesLast) + due)
                                         : restKnownOf(o);
        const unsigned n = restLengthOf(o.tuningLast, o.historyFramesLast);
        // THE REST LENGTH THE CARDS' DECISION READS (its demand window's start) is capped at the frames since the view's
        // last WHOLE re-trace (8B): with every cell held the clock reads "no active component" (1 << 20), and the window
        // reached back past the move into the last pose's marks (lab.card_set_binds: the far pose rested at A's set)
        const unsigned sinceWhole = o.frame > o.lastMovedFrame + 1u ? o.frame - 1u - o.lastMovedFrame : 0u;
        const unsigned reported = std::min(rest, sinceWhole);
        restFrames = restFrames ? std::min(restFrames, reported) : reported;
        restLength = std::max(restLength, n);
        if (rest < n + due) return false;   // (its whole demand span traced)
    }
    return true;
}

void ScreenProbeGather::statsInto(const detail::OgreScene *scene, unsigned long long restartKey,
                                  GatherStatus &out) const {
    out.error = mLastError;
    // THE VIEWS THAT DREW THE LATEST FRAME of this scene. A scene can hold
    // several (the viewport and a screenshot's shot view, which a screenshot's
    // settle loop draws ALONE while the viewport is quiet): the numbers are the
    // latest view's, and the settled history is every latest view's — a young
    // shot view is not settled because the quiet viewport was.
    const View *latest = nullptr;
    const void *latestKey = nullptr;
    for (const auto &kv : mViews) {
        const View &v = kv.second;
        if (v.scene != scene || !v.targetsReady) continue;
        if (!latest || int32_t(v.recordedFrame - latest->recordedFrame) > 0) { latest = &v; latestKey = kv.first; }
    }
    if (!latest) {
        shotInto(scene, out);
        return;
    }
    const View &v = *latest;
    out.running = true;
    out.stride = v.stride;
    out.octRes = v.octRes;
    out.raysPerProbe = v.octRes * v.octRes;
    out.probesX = v.gridW;
    out.probesY = v.gridH;
    out.probes = v.uniformProbes;
    out.adaptive = v.adaptiveLast;
    out.adaptiveRequested = v.adaptiveAsked;
    out.adaptiveCap = v.adaptiveCap;
    out.raysPerFrame = (unsigned long long)(v.uniformProbes + v.adaptiveLast) * out.raysPerProbe;
    out.targetW = v.w * v.layers;   // the eye PAIR (an eye per layer)
    out.targetH = v.h;
    out.stereo = v.stereo;
    out.eyeProbesX = v.eyeGridW;
    out.vramBytes = v.vramBytes;
    out.historyBytes = v.historyBytes;
    out.placeMs = detail::monitor::lastGpuMs("gather.place", latestKey);
    out.traceMs = detail::monitor::lastGpuMs("gather.trace", latestKey);
    out.filterMs = detail::monitor::lastGpuMs("gather.filter", latestKey);
    out.integrateMs = detail::monitor::lastGpuMs("gather.integrate", latestKey);
    out.cpuMs = v.cpuMs;
    out.temporal = v.lastTemporal;
    out.historyAge = v.age;
    out.irradiance = v.irrHost;
    out.irradianceW = v.irrHost.empty() ? 0u : v.w * v.layers;
    out.irradianceH = v.irrHost.empty() ? 0u : v.h;
    out.irradianceFrame = v.irrHostFrame;
    out.frame = v.frame;
    out.adaptiveCells = v.adaptiveHost;
    out.rays = v.raysHost;
    out.raysFrame = v.raysHostFrame;
    out.historyCount = v.histHost;
    out.historyStreak = v.histStreakHost;
    out.historyClass = v.histClassHost;
    out.historyDist = v.histDistHost;
    out.historyCountW = v.histHost.empty() ? 0u : v.w * v.layers;
    out.historyCountH = v.histHost.empty() ? 0u : v.h;
    out.historyCountFrame = v.histHostFrame;
    // THE SETTLED HISTORY (GatherStatus says what and why): every latest view's
    // history N frames past its last RESTART. The rest (and its hold) is
    // reported beside it and is NOT part of the predicate: a scene with a
    // per-frame writer never rests, and its shot takes the settled EMA picture.
    // A restart key the view has not drawn yet (a light write between two
    // frames) is a restart at frame 0.
    out.settled = true;
    out.restFrames = ~0u;
    out.sinceRestart = ~0u;
    out.held = true;
    for (const auto &kv : mViews) {
        const View &o = kv.second;
        if (o.scene != scene || !o.targetsReady || o.recordedFrame != v.recordedFrame) continue;
        // EVERY CELL HOLDS (PHOTON-7 stage 8B, B1): the view held whole, or its rest clock is past N - every active cell
        // held at that frame's finalize (a resume would have put the clock back)
        const unsigned rest = restKnownOf(o);
        const unsigned restN = restLengthOf(o.tuningLast, o.historyFramesLast);
        out.held = out.held && o.restableLast && everyCellHeldOf(o, restN);
        const unsigned n = settleFramesOf(o.historyFramesLast);
        out.settleFrames = std::max(out.settleFrames, n);
        out.restFrames = std::min(out.restFrames, rest);
        out.cellsReached = std::max(out.cellsReached, o.cellsReached);
        out.cellsHeld = std::max(out.cellsHeld, o.cellsHeld);
        out.cellsActive = std::max(out.cellsActive, o.cellsActive);
        out.cellsRing = std::max(out.cellsRing, o.cellsRing);
        out.resumes = std::max(out.resumes, o.resumes);
        if (&o == latest) out.restLog = o.restLog;
        out.foliageRuleAccepts = std::max(out.foliageRuleAccepts, o.foliageRuleAccepts);
        out.starvedDiffuse = std::max(out.starvedDiffuse, o.starvedDiffuse);
        out.starvedRough = std::max(out.starvedRough, o.starvedRough);
        out.cameraCuts = std::max(out.cameraCuts, o.cameraCuts);
        const unsigned since = o.restartKey == restartKey ? o.sinceRestart : 0u;
        out.sinceRestart = std::min(out.sinceRestart, since);
        if (o.lastTemporal && since < n) out.settled = false;
    }
    if (out.restFrames == ~0u) out.restFrames = 0u;
    if (out.sinceRestart == ~0u) out.sinceRestart = 0u;
    shotInto(scene, out);
}

void ScreenProbeGather::shotInto(const detail::OgreScene *scene, GatherStatus &out) const {
    // THE SHOT-VIEW FOLD (mForgotten): the last destroyed view's counts, as the last shot's
    if (const auto f = mForgotten.find(scene); f != mForgotten.end()) {
        out.shotValid = true;
        out.shotStarvedDiffuse = f->second.starvedDiffuse;
        out.shotStarvedRough = f->second.starvedRough;
    }
}

// ---------------------------------------------------------------------------
// A HELD FRAME (PHOTON-GATHER-1d). The view has been at rest for N frames: its
// irradiance texture holds the rest mean (rq_probe_integrate.comp) and stays
// exactly that until something moves. Nothing is dispatched; the texture is bound
// to the pass again, and the readback door still copies it out (a suite averaging
// a still scene's frames reads the held answer, not a stall).
void ScreenProbeGather::hold(View &v, const GatherInputs &in, bool temporal) {
    Ogre::RenderSystem *rs = mHost.gatherRenderSystem();
    bool irradianceCopied = false, historyCopied = false;
    // A HELD FRAME'S COPIES TAKE THE HOST'S FRAME'S SLOT, NEVER THE VIEW'S COUNTER (PHOTON-7 stage 5, the lead's ruling:
    // a measuring arm must not change the engine's state). The view's frame counter - the sample sequence's key (knobs[2]
    // off the rest) and the packed history's stochastic rounding's (knobs6[2]) - advances only on a frame that traces;
    // a held copy rides the host's frame number into the ring (a slot it shares with a traced frame's record holds the
    // same held state: the history half and the irradiance a held frame copies are the last traced frame's, unchanged)
    // and names that last traced frame as its gather frame. (It advanced the counter: with gather.historyReadback on,
    // every later frame's picture differed - gi.walk's second frozen walk on 59 of 60 frames.)
    // A HELD COPY NEVER TAKES A SLOT STILL IN FLIGHT (PHOTON-7 stage 11's fix round): the traced frames key the ring by
    // the view's counter and a held frame by the host's, so the first held frames after a trace can land on the slot of
    // a traced frame whose copy has not retired - and overwrote it, its record and its bytes: a stop the gather traced
    // two frames before it held never had its count (rtreflect.nee_firefly: the stop traced gather frame 229, the
    // next frame 230, then held; the held copy took 229's slot a frame before it retired, and the door saw 230 only).
    // Such a frame copies nothing (it binds and holds as any other); the next held frame's slot is free (a held frame
    // changes no state, so a later copy holds the same answer).
    const unsigned heldSlot = unsigned(mHost.gatherFrameNow() % kFramesInFlight);
    const bool slotFree = !v.pending[heldSlot].live ||
                          uint32_t(mHost.gatherFrameNow() - v.pending[heldSlot].frame) >= mHost.gatherFramesInFlight();
    if (slotFree && in.tuning.readback && v.irrReadback) {
        {
            Ogre::BarrierSolver &solver = rs->getBarrierSolver();
            Ogre::ResourceTransitionArray trans;
            solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Uav,
                                     Ogre::ResourceAccess::Read, 1u << Ogre::GPT_COMPUTE_PROGRAM);
            rs->executeResourceTransition(trans);
        }
        VkCommandBuffer cmd = mHost.gatherFrameCmd();
        if (cmd) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0, nullptr);
            const VkDeviceSize slotBytes = VkDeviceSize(v.w) * v.h * v.layers * 8u;
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(heldSlot) * slotBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = v.layers;
            region.imageExtent = { v.w, v.h, 1u };
            vkCmdCopyImageToBuffer(
                cmd, static_cast<Ogre::VulkanTextureGpu *>(v.irradiance)->getFinalTextureName(),
                VK_IMAGE_LAYOUT_GENERAL, v.irrReadback, 1, &region);
            // ...THE UNIFORM CELLS' RECORDS of the same frame (their twin links).
            const VkDeviceSize recBytes = VkDeviceSize(v.uniformProbes) * kRecordBytes;
            if (!v.recReadback) {
                std::string rerr;
                if (!mHost.gatherMakeBuffer(recBytes * kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            true, v.recReadback, v.recReadbackMemory,
                                            &v.recReadbackMapped, rerr))
                    v.recReadback = VK_NULL_HANDLE;
            }
            if (v.recReadback) {
                VkBufferCopy rc{};
                rc.dstOffset = VkDeviceSize(heldSlot) * recBytes;
                rc.size = recBytes;
                vkCmdCopyBuffer(cmd, v.records, v.recReadback, 1, &rc);
            }
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &toHost, 0, nullptr, 0, nullptr);
            irradianceCopied = true;
        }
    }
    // THE HISTORY COUNT OF A HELD FRAME (PHOTON-2 stage 0's fix round, the read's finding 1): a held frame writes no
    // history, so what it HOLDS is the last traced frame's — the half that frame's integrate wrote (the flip after it
    // made it the previous half), unchanged since. Copied under the arm like a traced frame's, so a stop the view
    // already holds (a still camera, a lamp step at rest) has its count too: the stillness door's mask, never a wait
    // for a frame that will not trace.
    if (slotFree && in.tuning.historyReadback && temporal && v.age > 0u) {
        const VkDeviceSize slotBytes = VkDeviceSize(v.w) * v.h * v.layers * 9u;   // (8 B the history word, 1 B its streak)
        if (!v.histReadback) {
            std::string rerr;
            if (!mHost.gatherMakeBuffer(slotBytes * kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                        v.histReadback, v.histReadbackMemory, &v.histReadbackMapped, rerr))
                v.histReadback = VK_NULL_HANDLE;
        }
        VkCommandBuffer cmd = v.histReadback ? mHost.gatherFrameCmd() : VK_NULL_HANDLE;
        if (cmd) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &toCopy, 0, nullptr, 0, nullptr);
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(heldSlot) * slotBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = v.layers;
            region.imageExtent = { v.w, v.h, 1u };
            vkCmdCopyImageToBuffer(cmd, v.history[(v.flip & 1u) ^ 1u], VK_IMAGE_LAYOUT_GENERAL, v.histReadback, 1,
                                   &region);
            region.bufferOffset += VkDeviceSize(v.w) * v.h * v.layers * 8u;   // (the streak after the words)
            vkCmdCopyImageToBuffer(cmd, v.streak[(v.flip & 1u) ^ 1u], VK_IMAGE_LAYOUT_GENERAL, v.histReadback, 1,
                                   &region);
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toHost, 0,
                                 nullptr, 0, nullptr);
            historyCopied = true;
        }
    }
    if (irradianceCopied || historyCopied) {
        View::Pending &pd = v.pending[heldSlot];
        pd.frame = mHost.gatherFrameNow();
        pd.live = true;
        pd.irradiance = irradianceCopied;
        pd.history = historyCopied;
        pd.held = true;
        pd.gatherFrame = v.frame ? v.frame - 1u : 0u;   // the last traced frame: its state is what a held frame holds
    }
    {
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        solver.resolveTransition(trans, v.roughSpec, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        solver.resolveTransition(trans, v.skyVis, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    detail::FogHlmsListener::setProbeGather(in.sceneMgr, { v.irradiance, v.roughSpec, nullptr, v.skyVis });
    ++v.age;
    v.lastTemporal = temporal;
    v.recordedFrame = mHost.gatherFrameNow();
    v.recordedDraw = v.scene ? v.scene->drawSerial() : 0u;
}

void ScreenProbeGather::record(const void *key, const GatherInputs &in) {
    if (mFailed || !in.scene || !in.sceneMgr || !in.hits.tlas || !in.normals || !in.depth || !in.shadowRough) return;
    if (!in.width || !in.height) return;
    const auto cpuStart = Clock::now();

    if (!mPlacePipeline) {
        std::string err;
        if (!makePipelines(err)) {
            mFailed = true;
            mLastError = err;
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: the screen-probe gather is off on this device — " + err);
            return;
        }
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the SCREEN-PROBE GATHER is on — the diffuse GI of every covered pixel "
            "is estimated from probe rays (SPECS/SCREEN_PROBE_GATHER_SPEC.md)");
    }

    // ---- WHAT THE TIER ASKS FOR: the tier table's gather row ----------------
    // (Types.h `GiGatherFacts`, handed over in `in.facts`): 16 pixels a probe at
    // Medium and High, 8 at Epic; 64 rays each, 36 at Medium. The ray budget is
    // NOT what decides these numbers — GATHER-0 measured the trace at 0.21 ns a
    // ray and found the 8,160-probe arm FASTER than a 2,040-probe one at the same
    // ray count (probe count is what fills this GPU) — the picture and the VRAM
    // are. Epic is the TABLE's row, never the view's SSR row (GA-TIERROW).
    unsigned stride = in.facts.stride;
    unsigned octRes = in.facts.octRes;
    if (in.tuning.probeStride) stride = in.tuning.probeStride;
    if (in.tuning.octRes) octRes = in.tuning.octRes;
    stride = std::min(std::max(stride, 2u), 64u);
    octRes = std::min(std::max(octRes, 1u), kGatherOctResMax);

    View &v = mViews[key];
    // THE GATHER-HITS HOST COPY GOES WITH THE ARM (FAR-SUN-1): a readback of a frame the arm no
    // longer covers is no reading (world.gatherHits then answers not ready), held frames included.
    // The pending frames' records are dropped WHENEVER the arm is off - not only once a host copy
    // exists: an arm on for fewer frames than the retire latency left them pending, and the first
    // frame after it decoded one (ready for a frame the arm no longer covered).
    // ...AND THE IRRADIANCE'S (ENGINE-UPGRADE-U5 stage 5, world.gatherIrradiance): a host copy of a frame the readback
    // no longer covers is no reading either - it stood after the door closed and answered as the gather's.
    if (!in.tuning.historyReadback) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) v.pending[i].history = false;
        if (!v.histHost.empty()) {
            v.histStreakHost.clear();
            v.histStreakHost.shrink_to_fit();
            v.histHost.clear();
            v.histHost.shrink_to_fit();
            v.histClassHost.clear();
            v.histClassHost.shrink_to_fit();
            v.histDistHost.clear();
            v.histDistHost.shrink_to_fit();
            v.histHostFrame = 0u;
        }
    }
    if (!in.tuning.readback) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) v.pending[i].irradiance = false;
        if (!v.irrHost.empty()) {
            v.irrHost.clear();
            v.irrHost.shrink_to_fit();
            v.irrHostFrame = 0u;
        }
    }
    if (!in.hitsReadback) {
        for (unsigned i = 0; i < kFramesInFlight; ++i) v.pending[i].rays = false;
        if (!v.raysHost.empty()) {
            v.raysHost.clear();
            v.raysHost.shrink_to_fit();
            v.raysHostFrame = 0u;
        }
    }
    // A SCENE BIND IS A NEW VIEW for the histories: nothing in them is this
    // scene's.
    if (v.scene != in.scene) v.age = 0u;
    v.scene = in.scene;
    v.sceneMgr = in.sceneMgr;
    readPending(v);

    // (Two eyes' grids side by side under stereo — ensureTargets' arithmetic.)
    const unsigned eyeGridW = (in.width + stride - 1u) / stride;
    const unsigned gridW = in.stereo ? 2u * eyeGridW : eyeGridW;
    const unsigned gridH = (in.height + stride - 1u) / stride;
    // HOW MANY ADAPTIVE PROBES A FRAME MAY ADD: the table's share of the grid (a
    // half; the grid at Low - GiGatherFacts). A CAP rather than a target — a flat scene spends none of it. The
    // cap is part of the allocation (the atlas and the record buffer are sized
    // for the worst case, resident, never per frame — the 0071 lesson), so it
    // cannot be a per-frame decision.
    unsigned adaptiveCap = gridW * gridH / std::max(in.facts.adaptiveCapDivisor, 1u);
    if (in.tuning.adaptiveCap >= 0) adaptiveCap = unsigned(in.tuning.adaptiveCap);

    std::string err;
    if (!ensureTargets(v, in, stride, octRes, adaptiveCap, err)) {
        // NOT A PROCESS-WIDE LATCH (the lead's read). A failure to allocate this
        // view's targets is about THIS view at THIS size — a resize, a second
        // view, a moment of VRAM pressure — and latching `mFailed` turned it
        // into "no scene gathers again until the app restarts", with no reason
        // anywhere a caller could read. The reason is published, the view is
        // dropped, and the next frame that asks for a different size may try
        // again. Only the PIPELINES (below) latch, because a device that cannot
        // compile them this minute cannot compile them next minute either.
        if (mLastError != err)   // once per reason, not once per frame while it persists
            Ogre::LogManager::getSingleton().logMessage(
                "Jahshaka: the screen-probe gather could not allocate its targets — " + err);
        mLastError = err;
        drop(v);
        return;
    }
    mLastError.clear();

    // ---- THE PIXEL HISTORY'S INPUTS (PHOTON-GATHER-1c) -----------------------
    // THE MEASUREMENT LEVER (the arm "gather.temporal", latched per frame) so a suite
    // drives both arms in one process: off, each frame's estimate alone, no history
    // read or written.
    const bool temporal = mHost.gatherTemporalArm();
    // The history restarts when it would otherwise average two estimators: the
    // history switched back on (its images were not written meanwhile), or a
    // tuning field that changes the estimator (an A/B arm).
    if (temporal && !v.temporalLast) v.age = 0u;
    if (!sameEstimator(in.tuning, v.tuningLast)) v.age = 0u;
    // ...and on A CAMERA CUT (PHOTON-2 round 2, N3: isCameraCut, Lumen's rule) - the previous basis is last frame's here.
    if (v.age > 0u && isCameraCut(v.prevCamPos, v.prevRayRight, v.prevRayDown, v.prevFwd, in.camPos, in.rayRight,
                                  in.rayDown, in.fwd)) {
        v.age = 0u;
        ++v.cameraCuts;
    }
    // (A change of STEREO SHAPE re-makes the targets in ensureTargets, which
    // restarts the history with them: its halves were one picture, or its one
    // picture is now two.)
    v.temporalLast = temporal;
    v.tuningLast = in.tuning;
    if (v.age == 0u) {
        // Nothing reads it at age 0; this frame's basis keeps the block finite.
        const auto put3w = [](float dst[4], const float src[3], float w) {
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = w;
        };
        std::memcpy(v.prevCamPos, in.camPos, sizeof(v.prevCamPos));
        put3w(v.prevRayTL, in.rayTL, 0.0f);
        put3w(v.prevRayRight, in.rayRight, 0.0f);
        put3w(v.prevRayDown, in.rayDown, 0.0f);
        put3w(v.prevFwd, in.fwd, 0.0f);
        std::memcpy(v.prevCamPos2, in.camPos2, sizeof(v.prevCamPos2));
        put3w(v.prevRayTL2, in.rayTL2, 0.0f);
        put3w(v.prevRayRight2, in.rayRight2, 0.0f);
        put3w(v.prevRayDown2, in.rayDown2, 0.0f);
        put3w(v.prevFwd2, in.fwd2, 0.0f);
    }
    // ---- THE REST, PER SCREEN CELL (PHOTON-GATHER-1d; PHOTON-7 stage 8B, B1 - jah_probe_rest.glsl) -------------------
    // MOVING IS THE VIEW'S: the camera's basis (bit for bit - a tracked VR head is never at rest, and a still editor
    // camera always is), the estimator (an age of 0 is a restart) and the view's own chain parameters. Every other
    // state is a CELL's: with the camera still, the frame's inputs (the changed-region list, the brick masks, the
    // field's owed boxes: GatherInputs::rest) resume the cells they reach and put the view's ONE rest clock back to 0;
    // every other cell keeps its rest, and a cell past N holds its rest mean - never traced for itself, never
    // integrated, its history written through. The GPU decides (rq_probe_resume.comp); the host knows the clock a few
    // frames late (restKnownOf). THE VIEW HOLDS WHOLE - nothing dispatched - after N + 1 still frames with no input at
    // all: every active cell held at the last of them.
    // DELETED with the per-cell rest (8B's CRUD): the scene's view-wide rest key (OgreScene::gatherRestKey's reader
    // here), gatherInputsBusy (a write owed lands as a region when it lands) and the readers' hold owed (the cards'
    // decision's work lands as Card regions in the cells it reaches).
    const unsigned restN = restLengthOf(in.tuning, historyFramesOf(in.tuning));
    bool restStill = false;
    GatherRestInputs restIn = in.rest;
    {
        const bool sameCamera =
            v.age > 0u && std::memcmp(v.prevCamPos, in.camPos, sizeof(v.prevCamPos)) == 0 &&
            std::memcmp(v.prevRayTL, in.rayTL, sizeof(in.rayTL)) == 0 &&
            std::memcmp(v.prevRayRight, in.rayRight, sizeof(in.rayRight)) == 0 &&
            std::memcmp(v.prevRayDown, in.rayDown, sizeof(in.rayDown)) == 0 &&
            std::memcmp(v.prevFwd, in.fwd, sizeof(in.fwd)) == 0;
        // NEVER UNDER STEREO: a tracked head is never at rest (see ensureTargets'
        // rest-mean note) and a stereo view holds no rest mean to rest into.
        // ...and not on the frame the photon view's discs come or go (PHOTON-VIEW-1):
        // a HELD cell dispatches no integrate, and the overlay is the integrate's.
        // The rest restarts; the history is untouched (the rest mean is a function
        // of the rest frames' own sequence, so it settles back to the same answer).
        // ...and WHOSE picture (FAR-SUN-1): the discs and the GatherHits view write different
        // jobs, so a switch between them with the camera still is a change too.
        const unsigned photon =
            in.photonOverlay ? in.photonOverlayGeneration * 2u + (in.photonOverlayGatherHits ? 1u : 0u) : 0u;
        // ...and never while the gather-hits readback runs (FAR-SUN-1): a held cell traces nothing, and the
        // instrument reads the trace.
        // ...and A VIEW THAT MISSED ONE OF ITS SCENE'S DRAWS missed that frame's list: it re-rests whole (a list is
        // read in its frame or never) - unless the view HELD whole before the miss and no close it missed carried an input
        // (the closes after the last it read and before this frame's): a held view's missed still frame changed nothing
        // it reads (gi.screen_bind (6)). A view still settling re-rests on any miss: its declines (a pipeline building in
        // the background) fall on frames a process cannot repeat, and the re-rest is what makes its held picture the
        // same in every process (gi.walk_area's L2: 545 px apart with the miss forgiven while settling)
        const bool missedInput = in.rest.lastInputClose > v.regionSerialSeen && in.rest.lastInputClose < in.rest.regionSerial;
        const bool missed = v.age > 0u && uint32_t(in.rest.drawSerial - v.recordedDraw) > 1u && (!v.heldLast || missedInput);
        const bool restable = temporal && !in.tuning.restOff && !in.stereo;
        restStill = restable && sameCamera && in.chainKey == v.chainKey && photon == v.photonLast &&
                    !in.hitsReadback && !missed && v.cellsCleared;
        v.photonLast = photon;
        v.restableLast = restable;
        // EACH CLOSE OF THE LIST IS READ ONCE (two frames that read the same close would resume its cells twice); a
        // brick mask is its close's frame's
        if (in.rest.regionSerial == v.regionSerialSeen) {
            restIn.regionCount = 0u;
            restIn.overflow = 0u;
            restIn.restart = false;
            for (GatherRestInputs::Bricks &b : restIn.bricks) b.live = false;
        }
        v.regionSerialSeen = in.rest.regionSerial;
        const bool inputs = restIn.any();
        // THE SETTLE: a restart zeroes it (the view's birth or an estimator change
        // is age 0; the scene's restart key moving is the rest), any other frame
        // — moving camera, moving geometry, held — counts one.
        // ...and the VIEW'S chain parameters moving (GatherInputs::chainKey: the reflection's cutoff the rough gate
        // reads) are a restart too - a settings change restarts what it changes.
        const bool restart = v.age == 0u || in.restartKey != v.restartKey || in.chainKey != v.chainKey;
        v.restartKey = in.restartKey;
        v.chainKey = in.chainKey;
        v.sinceRestart = restart ? 0u : std::min(v.sinceRestart + 1u, 1u << 20);
        v.historyFramesLast = historyFramesOf(in.tuning);
        if (!restStill || inputs) v.lastInputFrame = v.frame;
        if (!restStill) v.lastMovedFrame = v.frame;
        // EVERY CELL HOLDS: the last retired frame's clock counted no active cell, and no input came since it (a held
        // cell resumes only on an input) - nothing is dispatched; the last traced frame's targets are re-bound.
        v.heldLast = restStill && !inputs && v.clockKnown && v.cellsActive == 0u && v.cellsHeld > 0u &&
                     int32_t(v.clockFrame - v.lastInputFrame) >= 0;
        v.restFrames = restKnownOf(v);
        if (v.heldLast) {
            // THE RE-BIND IS THE SECOND HALF'S (finish): the registration is
            // scoped to the opaque pass, and the hit decode pass runs between.
            v.finishPending = true;
            v.finishHold = true;
            v.finishTraced = false;
            v.finishIn = in;
            v.finishTemporal = temporal;
            v.finishCpuMs = msSince(cpuStart);
            return;
        }
    }
    const unsigned cur = v.flip & 1u, prev = cur ^ 1u;   // the history pair's halves

    const unsigned ring = v.frame % kRing;
    if (!v.params[ring] &&
        !mHost.gatherMakeBuffer(sizeof(GatherParams), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true,
                                v.params[ring], v.paramsMemory[ring], &v.paramsMapped[ring], err))
        return;
    const auto allocSet = [&](VkDescriptorSetLayout layout, VkDescriptorSet &set) {
        if (set) return true;
        VkDescriptorSetAllocateInfo dai{};
        dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dai.descriptorPool = mPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &layout;
        if (vkAllocateDescriptorSets(mHost.gatherDevice(), &dai, &set) != VK_SUCCESS) {
            set = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };
    if (!allocSet(mPlaceLayout, v.placeSets[ring]) || !allocSet(mTraceLayout, v.traceSets[ring]) ||
        !allocSet(mFilterLayout, v.filterSets[ring]) ||
        !allocSet(mIntegrateLayout, v.integrateSets[ring]) || !allocSet(mResumeLayout, v.resumeSets[ring]))
        return;
    if (!v.restParams[ring] &&
        !mHost.gatherMakeBuffer(std::max<size_t>(sizeof(RestParams), kRegionBlockBytes), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                true, v.restParams[ring],
                                v.restParamsMemory[ring], &v.restParamsMapped[ring], err))
        return;

    // ---- THE PARAMETERS ----------------------------------------------------
    GatherParams pp{};
    std::memcpy(pp.camPos, in.camPos, sizeof(pp.camPos));
    const auto put3 = [](float dst[4], const float src[3], float w) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = w;
    };
    put3(pp.rayTL, in.rayTL, 0.0f);
    put3(pp.rayRight, in.rayRight, 0.0f);
    put3(pp.rayDown, in.rayDown, 0.0f);
    put3(pp.fwd, in.fwd, 0.0f);
    put3(pp.viewAxisX, in.viewAxisX, 0.0f);
    put3(pp.viewAxisY, in.viewAxisY, 0.0f);
    put3(pp.viewAxisZ, in.viewAxisZ, 0.0f);
    pp.projParams[0] = in.projA;
    pp.projParams[1] = in.projB;
    pp.projParams[2] = in.farClip;
    pp.projParams[3] = std::max(0.0f, in.pointSnapPx);   // the ray origin's point term (the placement's)
    pp.resolution[0] = float(v.gridW);
    pp.resolution[1] = float(v.gridH);
    pp.resolution[2] = float(in.width);
    pp.resolution[3] = float(in.height);
    pp.knobs[0] = float(stride);
    // THE RAY'S LENGTH: THE ONE REACH RULE (D4-PHOTON-TIERS; detail::photonRayReach,
    // the reflections' rule — the outer cascade's full diagonal under the far
    // plane). The gather had its own until then: the outer cascade's INSCRIBED
    // radius (PHOTON-GAFAR-1's sweep: a hit between that radius and the diagonal
    // lies outside every cascade; the ray's length cost nothing measurable at
    // 60 / 104 / 208 m in interleaved rounds). An escaping ray traces the far
    // copies from here to the far plane (knobs3.w below, ATOM-FARBLAS-1).
    {
        float diagonal = 0.0f;
        if (in.hits.cascadeCount) {
            const float *s = in.hits.voxelSize[in.hits.cascadeCount - 1u];
            diagonal = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        }
        const float derived = detail::photonRayReach(diagonal, in.farClip);
        pp.knobs[1] = in.tuning.rayLength > 0.0f ? in.tuning.rayLength : derived;
    }
    // THE SAMPLE SEQUENCE'S ONLY INPUT, and the determinism arm that holds it.
    // ...and AT REST the rest frame's own index (kRestSequenceBase + k), so the
    // rest mean is the same samples whatever came before it (PHOTON-GATHER-1d).
    // ...NOW THE GPU's (PHOTON-7 stage 8B, B1): the rest clock K is decided by the resume on the device, so the index is
    // its (jah_probe_rest.glsl jahFrameIndex: kRestSequenceBase + the seed + K at rest, the moving counter otherwise, 0
    // under the freeze - RestParams::seq); this knob is unread.
    pp.knobs[2] = 0.0f;
    // THE RAY'S START: THE ORIGIN LAW (PHOTON-7 stage 6, READ_A W2, READ_B F3 - one law for every ray, Types.h
    // rayOriginDrawnFootprints / jah_ray_origin.glsl) - off the probe's DRAWN surface along its geometric normal by the
    // point's float bound plus the drawn surface's bound against level 0 in full-resolution footprints at the probe's
    // Euclidean distance (this knob; the shader turns it into metres), tMin 0: the probe sits on Atom's cut or a stock
    // item's LOD and its rays trace each mesh's level 0. Never half a voxel (the fix round, gi.gather_cards' near-foot chase: a
    // start lifted 4 cm measured the irradiance 4 cm up - +7 % at a floating panel's foot, -11 % at 1.6 m).
    pp.knobs2[0] = in.tuning.classOff ? 1.0f : 0.0f;   // the class out of the history test (a test door)
    pp.knobs2[1] = std::max(0.0f, in.drawnCutFootprints);
    pp.knobs2[3] = float(octRes);
    pp.knobs3[0] = float(v.uniformProbes);
    pp.knobs3[1] = float(adaptiveCap);
    pp.knobs3[2] = float(v.atlasCols);
    // THE FAR QUERY (ATOM-FARBLAS-1, A5b section 4): an escaping ray traces the
    // FAR copies (each mesh's coarsest level, mask kRayMaskFar) from the near
    // length out to the camera's far plane. Off under the tuning's A/B, and off
    // by construction when the far plane is not beyond the near length.
    {
        const float farPlane = in.farClip > 0.0f ? in.farClip : 1000.0f;
        pp.knobs3[3] = (!in.tuning.farQueryOff && farPlane > pp.knobs[1]) ? farPlane : 0.0f;
        // ...and it STARTS `farOverlap` before the near length (audit F2): a
        // coarse surface just inside the near length whose fine surface lies
        // just outside it would otherwise be passed by both queries — the ray
        // would see through the object. A coarse hit in that overlap is honest:
        // the near query has already proved no fine surface lies inside it.
        pp.plane[2] = std::max(0.001f, pp.knobs[1] - std::max(0.0f, in.farOverlap));
    }
    pp.plane[0] = kPlaneTolerance;
    pp.plane[1] = kNormalTolerance;
    // w: 0 the grid's one offset a frame (every cell's probe at it; the integrate's bilinear on the shifted grid),
    // 1 the centre (jitterOff)
    pp.plane[3] = in.tuning.jitterOff ? 1.0f : 0.0f;
    // PHOTON-GATHER-1b: the SH bands the integrate evaluates (9 unless the
    // measurement arm asks for 4), the weight floor, the filter on or off.
    pp.knobs4[0] = in.tuning.shBands == 4u ? 4.0f : 9.0f;
    pp.knobs4[1] = kWeightFloor;
    pp.knobs4[2] = in.tuning.filterOff ? 0.0f : float(filterRadiusOf(in.tuning));
    pp.knobs4[3] = in.photonOverlay && !in.photonOverlayGatherHits ? 1.0f : 0.0f;   // THE PHOTON VIEW's discs (PHOTON-VIEW-1)
    // THE GATHER-HITS RECORDS (FAR-SUN-1): two uvec4 per ray of every uniform probe (REDS-1), made on
    // the first frame the arm asks (and freed with the view's targets), BEFORE the parameters say
    // the shader may write them: a failed allocation leaves p.hits.x 0 (the stand-in bound at 31
    // is 256 bytes, and the records are uniformProbes x 2 KiB - no write may reach it).
    if (in.hitsReadback && !v.rays) {
        std::string rerr;
        if (!mHost.gatherMakeBuffer(v.raysSlotBytes(),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false,
                                    v.rays, v.raysMemory, nullptr, rerr))
            v.rays = VK_NULL_HANDLE;
    }
    // THE GATHER-HITS INSTRUMENT (FAR-SUN-1): the records under the arm, the view's picture.
    pp.hits[0] = (in.hitsReadback && v.rays) ? 1.0f : 0.0f;
    pp.hits[1] = in.photonOverlay && in.photonOverlayGatherHits ? 1.0f : 0.0f;
    pp.hits[2] = float(v.stride);
    // PHOTON-GATHER-1c: the previous camera, the view's age, the history's floor,
    // the lever.
    std::memcpy(pp.prevCamPos, v.prevCamPos, sizeof(pp.prevCamPos));
    std::memcpy(pp.prevRayTL, v.prevRayTL, sizeof(pp.prevRayTL));
    std::memcpy(pp.prevRayRight, v.prevRayRight, sizeof(pp.prevRayRight));
    std::memcpy(pp.prevRayDown, v.prevRayDown, sizeof(pp.prevRayDown));
    std::memcpy(pp.prevFwd, v.prevFwd, sizeof(pp.prevFwd));
    pp.knobs5[0] = float(std::min(v.age, 65535u));
    // The floor: the shipped kHistoryFrames, or the tuning's A/B arm.
    pp.knobs5[1] = 1.0f / float(v.historyFramesLast);
    // THE REST MEAN (PHOTON-GATHER-1d): the rest frame k and N.
    // (the rest frame k is the GPU clock's, jahRestK; this lane carries THE DEMAND SPAN instead - set below)
    pp.knobs6[0] = 0.0f;
    pp.knobs6[1] = float(restLengthOf(in.tuning, v.historyFramesLast));
    // ...and the view's ADVANCING frame counter (PHOTON-GA-VR): the packed
    // history's stochastic rounding is keyed on it, never on the sample
    // sequence's frame above, which a frozen A/B holds still.
    pp.knobs6[2] = float(v.frame & 0xFFFFu);
    pp.knobs6[3] = in.tuning.rayJitterOff ? 1.0f : 0.0f;   // the rays at their texels' centres
    pp.knobs5[2] = temporal ? 1.0f : 0.0f;
    pp.knobs5[3] = in.tuning.historyValidationOff ? 1.0f : 0.0f;
    // PHOTON-GATHER-1d (GA-1e): the surface cache the hits read first, and the
    // per-slot geometry rows the card pick faces by — the per-slot table copied
    // into this ring slot's buffer (the scene's vector moves under a later
    // frame). No cache or no rows: zero slots, and the shader never indexes the
    // stand-ins bound in their place.
    uint32_t geomSlots = 0u;
    if (in.hits.geomRows && in.hits.geomRowOfSlot && !in.hits.geomRowOfSlot->empty()) {
        const VkDeviceSize want = VkDeviceSize(in.hits.geomRowOfSlot->size()) * sizeof(uint32_t);
        if (v.geomRowOfSlot[ring] && v.geomRowOfSlotBytes[ring] < want) {
            mHost.gatherRetireBuffer(v.geomRowOfSlot[ring], v.geomRowOfSlotMemory[ring]);
            v.geomRowOfSlot[ring] = VK_NULL_HANDLE;
            v.geomRowOfSlotMemory[ring] = VK_NULL_HANDLE;
            v.geomRowOfSlotMapped[ring] = nullptr;
            v.geomRowOfSlotBytes[ring] = 0u;
        }
        if (!v.geomRowOfSlot[ring]) {
            const VkDeviceSize bytes = std::max<VkDeviceSize>(want, 1024u);
            std::string gerr;
            if (mHost.gatherMakeBuffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true,
                                       v.geomRowOfSlot[ring], v.geomRowOfSlotMemory[ring],
                                       &v.geomRowOfSlotMapped[ring], gerr))
                v.geomRowOfSlotBytes[ring] = bytes;
        }
        if (v.geomRowOfSlot[ring] && v.geomRowOfSlotMapped[ring]) {
            std::memcpy(v.geomRowOfSlotMapped[ring], in.hits.geomRowOfSlot->data(), size_t(want));
            geomSlots = uint32_t(in.hits.geomRowOfSlot->size());
        }
    }
    // THE HIT SET'S UNIFORMS (the one function: the cascades, the sky, the cards' counts, the geometric normal's rows,
    // the hit list and its sun ray, the tables, the hit's own surface)
    hitSetParams(in.hits, geomSlots, pp.hit, HitReader::View);
    // THE DEMAND SPAN (PHOTON-7 stage 10, the per-reader snapshot): the view stamps the cards it reads at its rest
    // indices [1, N + the due] alone - moving (rest 0) it always stamps - so its demand is the same span in every run
    if (in.demandDue) {
        const unsigned span = restLengthOf(in.tuning, v.historyFramesLast) + in.demandDue;
        // (on the device: the stamps gate on the clock K, jah_rq_card.glsl's JAH_CARD_STAMP_GATE - the span here)
        pp.hit.cards[2] = 1.0f;
        pp.knobs6[0] = float(span);
    }
    {
        // THE IRRADIANCE FIELD, composed by the integrate (PHOTON-7 stage 7, 7-2; jah_probe_params.glsl): the block,
        // the window and counts, w = 1 bound and composed
        const bool field = in.fieldIrradiance && in.fieldDepth;
        std::memcpy(pp.field, in.field, sizeof(pp.field));
        pp.fieldWin[0] = in.fieldWin[0];
        pp.fieldWin[1] = in.fieldWin[1];
        pp.fieldWin[2] = in.fieldWin[2];
        pp.fieldWin[3] = field ? 1.0f : 0.0f;
        for (int k = 0; k < 9; ++k)
            for (int c = 0; c < 3; ++c) pp.envSh[k][c] = in.envSh[k][c];
    // THE ROUGH SPECULAR (7-3): the gate against the view's traced reflection, DiffuseLerp's two ends, the levels
    pp.rough[0] = in.roughGateFull;
    pp.rough[1] = in.roughGateRamp;
    pp.rough[2] = kRoughSpecDiffuseFrom;
    pp.rough[3] = kRoughSpecDiffuseAt;
    pp.roughLevels[0] = kRoughSpecFirstLevel;
    pp.roughLevels[1] = kRoughSpecLevelStep;
    pp.roughLevels[2] = float(kRoughSpecLevels);
    // THE QUADRATURE DOOR (a test door): the filtered map kept in the layer after the levels (only where the targets
    // were made with it), 2 = the brute-force read over it
    pp.roughLevels[3] = v.prefilteredLayers > kRoughSpecLevels ? float(std::min(in.roughQuadrature, 2u)) : 0.0f;
    }
    pp.alpha[2] = mHost.gatherDecodeHitsArm() ? 1.0f : 0.0f;   // the arm "gather.decodeHits"
    // THE FAST UPDATE (PHOTON-7 stage 8B, B5; jah_probe_params.glsl): Lumen's two numbers (FractionOfLightingMovingFor-
    // FastUpdateMode 0.1, MaxFastUpdateModeAmount 0.9), the skin posed, and each cascade's brick mask (the trace's binding
    // kTraceFastBricksBinding) with whether THIS frame's light work wrote it - on whatever the camera does (a brick is
    // the world's)
    pp.fast[0] = kFastUpdateMovingShare;
    pp.fast[1] = kFastUpdateMost;
    pp.fast[2] = in.fastSkinPosed ? 1.0f : 0.0f;
    pp.fast[3] = 1.0f;
    for (unsigned c = 0; c < GatherRestInputs::kCascades; ++c) {
        const GatherRestInputs::Bricks &b = in.rest.bricks[c];
        const bool bound = b.buffer && b.dims[0] && b.dims[1] && b.dims[2];
        for (int a = 0; a < 3; ++a) {
            pp.fastBrickOrigin[c][a] = b.origin[a];
            pp.fastBrickDims[c][a] = bound ? float(b.dims[a]) : 0.0f;
        }
        pp.fastBrickOrigin[c][3] = bound ? b.brickMetres : 0.0f;
        pp.fastBrickDims[c][3] = bound && b.live ? 1.0f : 0.0f;
    }
    // THE SECOND EYE (PHOTON-GA-VR): the shape, and the right eye's basis now and
    // before (written, never read, with one eye).
    pp.stereo[0] = v.stereo ? 1.0f : 0.0f;
    pp.stereo[1] = float(v.eyeGridW);
    pp.stereo[2] = float(in.tuning.ageView);   // THE HISTORY-AGE VIEW (an instrument; 0 = off)
    pp.stereo[3] = in.tuning.freezeHistory ? 1.0f : 0.0f;   // THE GI FREEZE (M1's measuring arm)
    // THE YOUNG HISTORY'S REACH (GATHER-NOISE-1).
    pp.young[0] = float(in.tuning.youngFrames ? in.tuning.youngFrames : kYoungFrames);
    pp.young[1] = std::max(1.0f, (in.tuning.youngReach > 0.0f ? in.tuning.youngReach : kYoungReachPixels) /
                                     float(stride));   // pixels -> cells of this view's stride
    // THE RAYS STRATIFIED ACROSS NEIGHBOURING PROBES (GATHER-NOISE-1).
    pp.young[2] = float(in.tuning.crossStrata ? in.tuning.crossStrata : kCrossStrata);
    std::memcpy(pp.camPos2, in.camPos2, sizeof(pp.camPos2));
    put3(pp.rayTL2, in.rayTL2, 0.0f);
    put3(pp.rayRight2, in.rayRight2, 0.0f);
    put3(pp.rayDown2, in.rayDown2, 0.0f);
    put3(pp.fwd2, in.fwd2, 0.0f);
    std::memcpy(pp.prevCamPos2, v.prevCamPos2, sizeof(pp.prevCamPos2));
    std::memcpy(pp.prevRayTL2, v.prevRayTL2, sizeof(pp.prevRayTL2));
    std::memcpy(pp.prevRayRight2, v.prevRayRight2, sizeof(pp.prevRayRight2));
    std::memcpy(pp.prevRayDown2, v.prevRayDown2, sizeof(pp.prevRayDown2));
    std::memcpy(pp.prevFwd2, v.prevFwd2, sizeof(pp.prevFwd2));
    std::memcpy(v.paramsMapped[ring], &pp, sizeof(pp));

    // ---- THE DESCRIPTOR SETS, rewritten every frame ------------------------
    // Uncached views, retired: the reflection path's measured trap (a cached
    // view of a texture recreated at the same address is a view of a dead
    // image) applies here word for word.
    const auto sampledView = [this](Ogre::TextureGpu *t) {
        Ogre::DescriptorSetTexture2::TextureSlot slot =
            Ogre::DescriptorSetTexture2::TextureSlot::makeEmpty();
        slot.texture = t;
        VkImageView view = static_cast<Ogre::VulkanTextureGpu *>(t)->createView(slot, false);
        mHost.gatherRetireView(view);
        return view;
    };
    VkImageView dummyCube = VK_NULL_HANDLE, dummyVolume = VK_NULL_HANDLE,
                dummyFlat = VK_NULL_HANDLE;
    VkBuffer dummyStorage = VK_NULL_HANDLE;
    if (!mHost.gatherDummies(dummyCube, dummyVolume, dummyFlat, dummyStorage, err)) return;

    VkDescriptorBufferInfo paramsInfo{};
    paramsInfo.buffer = v.params[ring];
    paramsInfo.range = sizeof(GatherParams);
    VkDescriptorBufferInfo recordsInfo{};
    recordsInfo.buffer = v.records;
    recordsInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo counterInfo{};
    counterInfo.buffer = v.counter;
    counterInfo.range = VK_WHOLE_SIZE;
    VkDescriptorBufferInfo argsInfo{};
    argsInfo.buffer = v.args;
    argsInfo.range = VK_WHOLE_SIZE;
    // THE CELLS' REST (PHOTON-7 stage 8B, B1): the clock, the cells' state and their distance store
    VkDescriptorBufferInfo clockInfo{}, cellStateInfo{}, cellStoreInfo{};
    clockInfo.buffer = v.clock;
    clockInfo.range = VK_WHOLE_SIZE;
    cellStateInfo.buffer = v.cellState;
    cellStateInfo.range = VK_WHOLE_SIZE;
    cellStoreInfo.buffer = v.cellStore;
    cellStoreInfo.range = VK_WHOLE_SIZE;
    VkDescriptorImageInfo normals{}, depth{};
    normals.sampler = mHost.gatherPointSampler();
    normals.imageView = mHost.gatherLayerView(in.normals, false);
    normals.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    depth.sampler = mHost.gatherPointSampler();
    depth.imageView = mHost.gatherLayerView(in.depth, false);
    depth.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // THE PREPASS'S SHADOW / ROUGHNESS (7-3: the rough specular's roughness, point-sampled as the reflection reads it)
    VkDescriptorImageInfo shadowRough{};
    shadowRough.sampler = mHost.gatherPointSampler();
    shadowRough.imageView = mHost.gatherLayerView(in.shadowRough, false);
    shadowRough.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (!normals.imageView || !depth.imageView || !shadowRough.imageView) return;

    {   // the placement set
        VkWriteDescriptorSet w[kPlaceBindings] = {};
        for (unsigned i = 0; i < kPlaceBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.placeSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &normals;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &depth;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &recordsInfo;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[4].pBufferInfo = &counterInfo;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[5].pBufferInfo = &argsInfo;
        w[kPlaceClockBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kPlaceClockBinding].pBufferInfo = &clockInfo;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kPlaceBindings, w, 0, nullptr);
    }

    VkDescriptorImageInfo atlasStore{};
    atlasStore.imageView = v.atlasView;
    atlasStore.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    // THE IRRADIANCE TARGET'S STORAGE VIEW, made once a frame and bound twice: the integrate writes
    // it, and the trace's overlay binding stands it in for the photon overlay (never written there)
    // - one view, not a second one created and retired every traced frame.
    VkDescriptorImageInfo irradianceStore{};
    irradianceStore.imageView = mHost.gatherLayerView(v.irradiance, true);
    irradianceStore.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (!irradianceStore.imageView) return;
    {   // the trace set: its own six, then THE HIT SET (the one writer, HitSetWrite: kTraceHitSlots)
        VkWriteDescriptorSet w[kTraceBindings] = {};
        for (unsigned i = 0; i < kTraceBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.traceSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo = &paramsInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[2].pBufferInfo = &recordsInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[3].pImageInfo = &atlasStore;
        HitSetWrite hitSet;
        std::string herr;
        if (!hitSet.write(mHost, v.traceSets[ring], kTraceHitSlots, in.hits, geomSlots ? v.geomRowOfSlot[ring] : VK_NULL_HANDLE,
                          w, herr))
            return;
        // THE GATHER-HITS INSTRUMENT (FAR-SUN-1): the view's records under the arm, or the
        // stand-in (never written: p.hits.x = 0); the photon overlay under the GatherHits view, or
        // the irradiance target's own storage view (never written: p.hits.y = 0) — the integrate's
        // binding-8 rule.
        VkDescriptorBufferInfo raysInfo{};
        raysInfo.buffer = (in.hitsReadback && v.rays) ? v.rays : dummyStorage;
        raysInfo.range = VK_WHOLE_SIZE;
        w[kTraceRaysBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kTraceRaysBinding].pBufferInfo = &raysInfo;
        VkDescriptorImageInfo hitsOverlay{};
        hitsOverlay.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        hitsOverlay.imageView = irradianceStore.imageView;
        if (in.photonOverlay && in.photonOverlayGatherHits)
            hitsOverlay.imageView = mHost.gatherLayerView(in.photonOverlay, true);
        if (!hitsOverlay.imageView) return;
        w[kTraceOverlayBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[kTraceOverlayBinding].pImageInfo = &hitsOverlay;
        w[kTraceClockBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kTraceClockBinding].pBufferInfo = &clockInfo;
        w[kTraceCellStateBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kTraceCellStateBinding].pBufferInfo = &cellStateInfo;
        w[kTraceCellStoreBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kTraceCellStoreBinding].pBufferInfo = &cellStoreInfo;
        VkDescriptorBufferInfo fastMasks[GatherRestInputs::kCascades] = {};
        for (unsigned c = 0; c < GatherRestInputs::kCascades; ++c) {
            const GatherRestInputs::Bricks &b = in.rest.bricks[c];
            fastMasks[c].buffer = b.buffer ? b.buffer : dummyStorage;
            fastMasks[c].offset = b.buffer ? b.offset : 0u;
            fastMasks[c].range = b.buffer ? b.range : VK_WHOLE_SIZE;
        }
        w[kTraceFastBricksBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kTraceFastBricksBinding].descriptorCount = GatherRestInputs::kCascades;
        w[kTraceFastBricksBinding].pBufferInfo = fastMasks;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kTraceBindings, w, 0, nullptr);
    }

    {   // the filter set
        VkDescriptorImageInfo filterRaw{}, filterStored{};
        filterRaw.imageView = v.atlasView;
        filterRaw.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        filterStored.imageView = v.prefilteredView;
        filterStored.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w[kFilterBindings] = {};
        for (unsigned i = 0; i < kFilterBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.filterSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &recordsInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[2].pImageInfo = &filterRaw;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &depth;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4].pImageInfo = &filterStored;
        w[kFilterCellStateBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kFilterCellStateBinding].pBufferInfo = &cellStateInfo;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kFilterBindings, w, 0, nullptr);
    }

    {   // the integrate set
        VkWriteDescriptorSet w[kIntegrateBindings] = {};
        for (unsigned i = 0; i < kIntegrateBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.integrateSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[1].pBufferInfo = &recordsInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &normals;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3].pImageInfo = &depth;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[4].pImageInfo = &irradianceStore;
        VkDescriptorImageInfo hist[2] = {};
        const VkImageView histViews[2] = { v.historyView[prev], v.historyView[cur] };
        for (unsigned k = 0; k < 2u; ++k) {
            hist[k].imageView = histViews[k];
            hist[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[5 + k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[5 + k].pImageInfo = &hist[k];
        }
        VkDescriptorImageInfo rest{};
        rest.imageView = v.restMeanView;
        rest.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[7].pImageInfo = &rest;
        // THE PHOTON OVERLAY (8): the view's, or — the knob off — the irradiance
        // target's own storage view as a stand-in of the same format, never written.
        VkDescriptorImageInfo photon{};
        photon.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        photon.imageView = irradianceStore.imageView;
        if (in.photonOverlay) {
            photon.imageView = mHost.gatherLayerView(in.photonOverlay, true);
            if (!photon.imageView) return;
        }
        w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[8].pImageInfo = &photon;
        w[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[9].pBufferInfo = &counterInfo;
        // THE IRRADIANCE FIELD'S TWO ATLASES (10, 11; 7-2), bilinear and clamped as the pass reads them - or the flat
        // stand-in where no field is bound (the shader reads none: fieldWin.w 0)
        VkDescriptorImageInfo fieldImgs[2] = {};
        Ogre::TextureGpu *fieldTex[2] = { in.fieldIrradiance, in.fieldDepth };
        for (unsigned k = 0; k < 2u; ++k) {
            fieldImgs[k].sampler = mHost.gatherLinearSampler();
            fieldImgs[k].imageView = in.fieldIrradiance && in.fieldDepth ? sampledView(fieldTex[k]) : dummyFlat;
            fieldImgs[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            if (!fieldImgs[k].imageView) return;
            w[10 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[10 + k].pImageInfo = &fieldImgs[k];
        }
        // THE ROUGH SPECULAR (12 - 17, 7-3): the roughness, the prefiltered maps through the linear sampler (in the
        // GENERAL layout its storage writes leave it in), the output's storage view, the lobe's history pair (this
        // frame's flip) and its rest mean
        w[12].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[12].pImageInfo = &shadowRough;
        VkDescriptorImageInfo filteredRead{};
        filteredRead.sampler = mHost.gatherLinearSampler();
        filteredRead.imageView = v.prefilteredView;
        filteredRead.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[13].pImageInfo = &filteredRead;
        VkDescriptorImageInfo roughOut{};
        roughOut.imageView = mHost.gatherLayerView(v.roughSpec, true);
        roughOut.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!roughOut.imageView) return;
        w[14].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[14].pImageInfo = &roughOut;
        VkDescriptorImageInfo roughHist[2] = {};
        const VkImageView roughHistViews[2] = { v.roughHistoryView[prev], v.roughHistoryView[cur] };
        for (unsigned k = 0; k < 2u; ++k) {
            roughHist[k].imageView = roughHistViews[k];
            roughHist[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[15 + k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[15 + k].pImageInfo = &roughHist[k];
        }
        VkDescriptorImageInfo roughRest{};
        roughRest.imageView = v.roughRestView;
        roughRest.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        w[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[17].pImageInfo = &roughRest;
        // THE HIT SET (18 - 50): its one writer, the trace's sources and its per-slot row copy for this frame in flight
        HitSetWrite integrateHits;
        std::string ihErr;
        if (!integrateHits.write(mHost, v.integrateSets[ring], kIntegrateHitSlots, in.hits,
                                 geomSlots ? v.geomRowOfSlot[ring] : VK_NULL_HANDLE, w, ihErr))
            return;
        // THE SKY VISIBILITY (51, 10-4 R11)
        VkDescriptorImageInfo skyOut{};
        skyOut.imageView = mHost.gatherLayerView(v.skyVis, true);
        skyOut.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (!skyOut.imageView) return;
        w[kIntegrateSkyVisBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[kIntegrateSkyVisBinding].pImageInfo = &skyOut;
        w[kIntegrateClockBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kIntegrateClockBinding].pBufferInfo = &clockInfo;
        w[kIntegrateCellStateBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kIntegrateCellStateBinding].pBufferInfo = &cellStateInfo;
        w[kIntegrateCellStoreBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kIntegrateCellStoreBinding].pBufferInfo = &cellStoreInfo;
        VkDescriptorImageInfo streakInfo[2] = {};   // (8B: last frame's half, this frame's - the history's flip)
        streakInfo[0].imageView = v.streakView[prev];
        streakInfo[1].imageView = v.streakView[cur];
        for (unsigned k = 0; k < 2u; ++k) {
            streakInfo[k].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w[kIntegrateStreakPrevBinding + k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[kIntegrateStreakPrevBinding + k].pImageInfo = &streakInfo[k];
        }
        vkUpdateDescriptorSets(mHost.gatherDevice(), kIntegrateBindings, w, 0, nullptr);
    }

    // ---- THE RESUME's BLOCK AND SET (PHOTON-7 stage 8B, B1; rq_probe_resume.comp) -----------------------------------
    // THE PYRAMIDS' SHAPE: per live cascade, its levels down to one node, in one buffer (re-made when it grows).
    RestParams rp{};
    unsigned liveMasks = 0u;
    {
        VkDeviceSize words = 0u;
        for (unsigned c = 0; c < GatherRestInputs::kCascades; ++c) {
            const GatherRestInputs::Bricks &b = restIn.bricks[c];
            const bool live = restStill && b.live && b.buffer && b.dims[0] && b.dims[1] && b.dims[2];
            for (int a = 0; a < 3; ++a) {
                rp.brickDims[c][a] = b.dims[a];
                rp.brickOrigin[c][a] = b.origin[a];
            }
            rp.brickDims[c][3] = live ? 1u : 0u;
            rp.brickOrigin[c][3] = b.brickMetres;
            unsigned levels = 0u;
            VkDeviceSize at = 0u;
            for (unsigned l = 0; l < kPyramidLevels; ++l) {
                uint32_t d[3];
                for (int a = 0; a < 3; ++a) d[a] = std::max(1u, (b.dims[a] + (1u << l) - 1u) >> l);
                rp.brickLevelOffset[c][l >> 2u][l & 3u] = uint32_t(at);
                at += VkDeviceSize(d[0]) * d[1] * d[2];
                ++levels;
                if (d[0] == 1u && d[1] == 1u && d[2] == 1u) break;
            }
            rp.brickPyramid[c][0] = uint32_t(words);
            rp.brickPyramid[c][1] = levels;
            rp.brickPyramid[c][2] = uint32_t(at);
            if (live) {
                words += at;
                ++liveMasks;
            }
        }
        const VkDeviceSize bytes = std::max<VkDeviceSize>(words, 1u) * 4u;
        if (!v.pyramid || v.pyramidBytes < bytes) {
            mHost.gatherRetireBuffer(v.pyramid, v.pyramidMemory);
            v.pyramid = VK_NULL_HANDLE;
            v.pyramidMemory = VK_NULL_HANDLE;
            v.pyramidBytes = 0u;
            const VkDeviceSize make = std::max<VkDeviceSize>(bytes, 4096u);
            if (!mHost.gatherMakeBuffer(make, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false, v.pyramid, v.pyramidMemory,
                                        nullptr, err))
                return;
            v.pyramidBytes = make;
        }
    }
    // THE RING's REACH in cells: what an active pixel's integrate reads around its cell (the bilinear's bracket and
    // the silhouette rule's 5 x 5: two cells; a young history's wide read: its reach) plus the filter's reach (each
    // probe it reads is a filtered one)
    const unsigned ringReach =
        std::max(2u, unsigned(std::ceil(std::max(1.0f, (in.tuning.youngReach > 0.0f ? in.tuning.youngReach
                                                                                  : kYoungReachPixels) /
                                                             float(stride))))) +
        (in.tuning.filterOff ? 0u : filterRadiusOf(in.tuning));
    // THE BLOCKS (the components' grain): B = 2 (the reach + 1) + 1 - a whole block between two components, so no cell
    // within the reach + 1 of both
    const unsigned blockSide = std::max(kLeastBlockSide, 2u * (ringReach + 1u) + 1u);
    rp.blocks[0] = float(blockSide);
    rp.blocks[1] = float((v.gridW + blockSide - 1u) / blockSide);
    rp.blocks[2] = float((v.gridH + blockSide - 1u) / blockSide);
    rp.blocks[3] = rp.blocks[1] * rp.blocks[2];
    rp.mode[0] = restStill ? 1.0f : 0.0f;
    rp.mode[1] = float(restN);
    rp.mode[2] = float(ringReach);
    rp.mode[3] = v.restableLast ? 1.0f : 0.0f;
    rp.seq[0] = float(v.frame & 0xFFFFu);
    rp.seq[1] = float(kRestSequenceBase + std::min(in.tuning.restSeed, 255u) * 4096u);   // + K < 2^21: exact
    rp.seq[2] = in.tuning.freezeFrameIndex ? 1.0f : 0.0f;
    rp.seq[3] = in.farClip > 0.0f ? in.farClip : 1.0e4f;
    rp.lists[0] = restStill && restIn.regions ? float(std::min(restIn.regionCount, 128u)) : 0.0f;
    rp.lists[1] = restStill && (restIn.overflow || restIn.restart) ? 1.0f : 0.0f;
    // THE CONVERGENCE's WINDOW: the history's own settle (settleFramesOf: the frames its EMA takes to forget a step), never
    // the rest's length - a rest door of 256 frames made the gate wait ~120 frames to start its rest (gi.gather_sliver)
    rp.lists[2] = float(settleFramesOf(historyFramesOf(in.tuning)));
    rp.lists[3] = float(liveMasks);
    rp.field[0] = restStill && restIn.fieldBusy ? 1.0f : 0.0f;
    std::memcpy(v.restParamsMapped[ring], &rp, sizeof(rp));
    // (the test runs every still frame: a settling cell's convergence is read there, inputs or none)
    const bool restTests = restStill;
    {
        VkWriteDescriptorSet w[kResumeBindings] = {};
        for (unsigned i = 0; i < kResumeBindings; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = v.resumeSets[ring];
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        VkDescriptorBufferInfo restInfo{};
        restInfo.buffer = v.restParams[ring];
        restInfo.range = sizeof(RestParams);
        // (no list closed yet: the resume's own block stands in - its region count is 0, the test reads none)
        VkDescriptorBufferInfo regionInfo{};
        regionInfo.buffer = restIn.regions ? restIn.regions : v.restParams[ring];
        regionInfo.offset = restIn.regions ? restIn.regionsOffset : 0u;
        regionInfo.range = restIn.regions ? restIn.regionsRange : kRegionBlockBytes;
        VkDescriptorBufferInfo masks[GatherRestInputs::kCascades] = {};
        for (unsigned c = 0; c < GatherRestInputs::kCascades; ++c) {
            const GatherRestInputs::Bricks &b = restIn.bricks[c];
            masks[c].buffer = b.buffer ? b.buffer : dummyStorage;
            masks[c].offset = b.buffer ? b.offset : 0u;
            masks[c].range = b.buffer ? b.range : VK_WHOLE_SIZE;
        }
        VkDescriptorBufferInfo pyramidInfo{};
        pyramidInfo.buffer = v.pyramid;
        pyramidInfo.range = VK_WHOLE_SIZE;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &paramsInfo;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[1].pBufferInfo = &restInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &depth;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &cellStateInfo;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[4].pBufferInfo = &cellStoreInfo;
        w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[5].pBufferInfo = &clockInfo;
        w[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[6].pBufferInfo = &regionInfo;
        w[kResumeMaskBinding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[kResumeMaskBinding].descriptorCount = GatherRestInputs::kCascades;
        w[kResumeMaskBinding].pBufferInfo = masks;
        w[8].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[8].pBufferInfo = &pyramidInfo;
        vkUpdateDescriptorSets(mHost.gatherDevice(), kResumeBindings, w, 0, nullptr);
    }

    // ---- THE LAYOUTS, THROUGH OGRE'S OWN SOLVER ----------------------------
    // Before the command buffer is taken, for the reason recorded at the
    // reflection's own transition block: executeResourceTransition closes every
    // encoder and the queue may roll over to a new command buffer.
    Ogre::RenderSystem *rs = mHost.gatherRenderSystem();
    {
        const Ogre::uint8 computeStage = 1u << Ogre::GPT_COMPUTE_PROGRAM;
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Uav,
                                 Ogre::ResourceAccess::ReadWrite, computeStage);
        solver.resolveTransition(trans, v.roughSpec, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::Write,
                                 computeStage);
        // (the sky visibility keeps a no-answer pixel's last share: read and written)
        solver.resolveTransition(trans, v.skyVis, Ogre::ResourceLayout::Uav, Ogre::ResourceAccess::ReadWrite,
                                 computeStage);
        if (in.photonOverlay)
            solver.resolveTransition(trans, in.photonOverlay, Ogre::ResourceLayout::Uav,
                                     Ogre::ResourceAccess::Write, computeStage);
        for (Ogre::TextureGpu *t : { in.normals, in.depth, in.shadowRough })
            solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                     Ogre::ResourceAccess::Read, computeStage);
        // THE HIT SET (the one order: hitSetTransitions)
        hitSetTransitions(solver, trans, in.hits);
        // THE IRRADIANCE FIELD'S ATLASES (7-2), sampled by the integrate (the field writes them at the frame's head)
        if (in.fieldIrradiance && in.fieldDepth)
            for (Ogre::TextureGpu *t : { in.fieldIrradiance, in.fieldDepth })
                solver.resolveTransition(trans, t, Ogre::ResourceLayout::Texture,
                                         Ogre::ResourceAccess::Read, computeStage);
        rs->executeResourceTransition(trans);
    }

    // ---- THE FOUR DISPATCHES -----------------------------------------------
    VkCommandBuffer cmd = mHost.gatherFrameCmd();
    if (!cmd) return;
    // THE BLACK STAND-INS THIS FRAME BOUND, out of UNDEFINED. With the SSR row
    // off the reflection trace never runs and never clears them, and every
    // descriptor slot a scene cannot fill (no sky cube, no voxel arm) points at
    // one — `gi.gather_reference` is exactly that chain.
    mHost.gatherClearDummies(cmd);
    clearAtlas(v, cmd);

    // The frame's counters: the adaptive count to zero, and the trace's
    // indirect arguments to the uniform grid (the placement job raises x by an
    // atomic max for every adaptive probe it appends).
    // ONE WORKGROUP PER PROBE ON X, AND X HAS A CEILING. Vulkan guarantees only
    // 65,535 work groups on a dimension (`maxComputeWorkGroupCount[0]`; this
    // device allows 2^31, but the number a shipped tier may rely on is the
    // guarantee) and Epic at 4K would ask for 162,000. The probe count is
    // CLAMPED here rather than folded to two dimensions, which would change the
    // shader's index arithmetic for a case no tier reaches today: 1080p Epic is
    // 32,400 probes and the clamp is 65,535, so nothing this lane ships comes
    // near it. A frame that hits it traces the probes it can and says so once.
    const uint32_t maxGroups = mMaxWorkGroupX ? mMaxWorkGroupX : 65535u;
    if (v.uniformProbes > maxGroups && !mSaidWorkGroupClamp) {
        mSaidWorkGroupClamp = true;
        Ogre::LogManager::getSingleton().logMessage(
            "Jahshaka: the screen-probe grid asks for " + std::to_string(v.uniformProbes) +
            " work groups and this device allows " + std::to_string(maxGroups) +
            " — the gather traces the first " + std::to_string(maxGroups) +
            " probes of each frame (a coarser stride is the cure)");
    }
    const uint32_t argsInit[4] = { std::min(v.uniformProbes, maxGroups), 1u, 1u, 0u };
    vkCmdFillBuffer(cmd, v.counter, 0, VK_WHOLE_SIZE, 0u);   // the demand and every tile's count
    vkCmdUpdateBuffer(cmd, v.args, 0, sizeof(argsInit), argsInit);
    // THE CELLS' FIRST FRAME (PHOTON-7 stage 8B, B1): every cell active, the clock at 0 (the store is emptied by the
    // finalize, which this frame - not still - runs over every cell); then each frame the test's two words (any
    // reached, the cells reached) start at 0.
    if (!v.cellsCleared) {
        vkCmdFillBuffer(cmd, v.cellState, 0, VK_WHOLE_SIZE, 0u);
        vkCmdFillBuffer(cmd, v.clock, 0, VK_WHOLE_SIZE, 0u);
        v.cellsCleared = true;
    } else
        vkCmdFillBuffer(cmd, v.clock, 1u * 4u, 3u * 4u, 0u);   // (and word 1, the fast update's counts: B5)
    {
        // ...AND WHAT THE FRAME'S EARLIER WORK WROTE that the resume reads: the GI step's brick masks (Ogre's compute
        // jobs) and the changed-region list's upload (a transfer) - one global edge from every earlier command
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
    }
    // ---- THE RESUME (PHOTON-7 stage 8B, B1; rq_probe_resume.comp): the pyramids, the cells' tests, the clock, the
    // finalize and the ring - before the placement, whose jitter is the clock's index
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0, "gather.resume", rs, key);
        work.setUnits(v.uniformProbes);
        const auto edge = [&]() {
            VkMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                                 &b, 0, nullptr, 0, nullptr);
        };
        const auto run = [&](unsigned job, uint32_t x, uint32_t y) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mResumePipeline[job]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mResumePipeLayout[job], 0, 1,
                                    &v.resumeSets[ring], 0, nullptr);
            JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
            vkCmdDispatch(cmd, std::max(x, 1u), std::max(y, 1u), 1u);
        };
        if (restTests) {
            if (liveMasks) {
                uint32_t nodes = 0u;
                for (unsigned c = 0; c < GatherRestInputs::kCascades; ++c)
                    if (rp.brickDims[c][3]) nodes = std::max(nodes, rp.brickPyramid[c][2]);
                run(kResumePyramid, (nodes + 63u) / 64u, GatherRestInputs::kCascades);
                edge();
            }
            run(kResumeTest, v.gridW, v.gridH);
            edge();
        }
        const uint32_t cellGroups = (v.uniformProbes + 63u) / 64u;
        run(kResumeClock, 1u, 1u);
        edge();
        run(kResumeFinal, cellGroups, 1u);
        edge();
        run(kResumeRing, cellGroups, 1u);
        edge();
    }

    // ...AND THE FRAME MONITOR'S OWN ROWS BESIDE THEM (`gather.place`,
    // `gather.trace`, `gather.integrate`). The timestamps above are what
    // `giStatus().gather` reports; these are what a monitor CAPTURE shows,
    // beside `vct.cascadeN` and `ifd.build` — the same instrument the rest of
    // the GI work reports through, so a capture of a gathering frame accounts
    // for all of it. They cost nothing while the monitor is off (`gMonitor` null
    // is the constructor's first line).
    //
    // THE REASON IS `Camera` AND NOT `None`, which the monitor reads as "no
    // recorded input change" — its redundant-work value. A screen probe is a
    // VIEW-DEPENDENT estimate, like the planar reflector's render beside it in
    // the same enum: it is re-made every frame because the thing it describes is
    // the picture, not because nothing told it to stop.
    {
        detail::monitor::CacheScope work(CacheKind::Gi, WorkReason::Camera, 0,
                                         "gather.place", rs, key);
        work.setUnits(v.uniformProbes);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPlacePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPlacePipeLayout, 0, 1,
                                &v.placeSets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, v.gridW, v.gridH, 1u);   // ONE WORKGROUP PER CELL: every pixel of it tested (PHOTON-7 stage 10)
        // ...AND THE SECOND PASS: the cells that asked for a second probe, ranked in a
        // fixed order, the first `cap` of them given one (rq_probe_place.comp's header).
        {
            VkMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                                 nullptr);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPlaceSelectPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mPlaceSelectPipeLayout, 0, 1,
                                &v.placeSets[ring], 0, nullptr);
        JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
        vkCmdDispatch(cmd, (v.gridW + 7u) / 8u, (v.gridH + 7u) / 8u, 1u);
    }
    {
        // The records and the argument buffer the trace is about to read — and
        // the argument buffer is read by the INDIRECT stage, which is not the
        // compute one.
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                          VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT |
                                 VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 1, &b, 0, nullptr, 0, nullptr);
    }
    // HOW MANY THE FRAME APPENDED, on its way to the host: four bytes into this
    // frame's slot of a ring the CPU reads once the frame has retired.
    {
        VkBufferCopy region{};
        region.srcOffset = 0;
        region.dstOffset = VkDeviceSize(v.frame % kFramesInFlight) * kReadbackSlotBytes;
        region.size = 4u;
        vkCmdCopyBuffer(cmd, v.counter, v.readback, 1, &region);
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &b, 0, nullptr, 0, nullptr);
    }

    detail::monitor::CacheScope traceWork(CacheKind::Gi, WorkReason::Camera, 0,
                                          "gather.trace", rs, key);
    traceWork.setUnits(v.uniformProbes + v.adaptiveLast);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTracePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mTracePipeLayout, 0, 1,
                            &v.traceSets[ring], 0, nullptr);
    // ONE WORKGROUP PER PROBE — the trace's 64 threads ARE its 64 rays — and
    // the COUNT is the GPU's own (fork 1bccc3f93+a98e2b0af (was 0032)'s shape: a count nobody on the CPU
    // can know without a round trip).
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatchIndirect(cmd, v.args, 0);
    traceWork.close();
    // ---- THE FRAME'S FIRST HALF ENDS HERE (PHOTON-HIT-SHADE-1): the trace has
    // appended the hits no cache can shade to the hit list; the hit decode pass
    // shades them and the write-back puts their radiance into the atlas before
    // `finish` filters it.
    v.finishPending = true;
    v.finishHold = false;
    v.finishTraced = true;
    v.finishIn = in;
    v.finishRing = ring;
    v.finishTemporal = temporal;
    v.finishCpuMs = msSince(cpuStart);
}

VkImageView ScreenProbeGather::tracedAtlas(const void *key) const {
    auto it = mViews.find(key);
    if (it == mViews.end() || !it->second.finishPending || !it->second.finishTraced) return VK_NULL_HANDLE;
    return it->second.atlasView;
}

void ScreenProbeGather::finish(const void *key) {
    auto vit = mViews.find(key);
    if (vit == mViews.end() || !vit->second.finishPending) return;
    View &v = vit->second;
    v.finishPending = false;
    const auto cpuStart = Clock::now();
    const GatherInputs &in = v.finishIn;
    const bool temporal = v.finishTemporal;
    if (v.finishHold) {
        hold(v, in, temporal);
        v.cpuMs = float(v.finishCpuMs + msSince(cpuStart));
        return;
    }
    const unsigned ring = v.finishRing;
    Ogre::RenderSystem *rs = mHost.gatherRenderSystem();
    VkCommandBuffer cmd = mHost.gatherFrameCmd();
    if (!cmd) return;
    const auto computeToCompute = [&]() {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
    };
    computeToCompute();

    // THE FILTER IN PROBE SPACE AND THE SH (PHOTON-GATHER-1b): one workgroup
    // per probe again, from the SAME indirect arguments — every probe the trace
    // traced is filtered, and none it did not.
    detail::monitor::CacheScope filterWork(CacheKind::Gi, WorkReason::Camera, 0,
                                           "gather.filter", rs, key);
    filterWork.setUnits(v.uniformProbes + v.adaptiveLast);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mFilterPipeLayout, 0, 1,
                            &v.filterSets[ring], 0, nullptr);
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatchIndirect(cmd, v.args, 0);
    filterWork.close();
    computeToCompute();

    detail::monitor::CacheScope integrateWork(CacheKind::Gi, WorkReason::Camera, 0,
                                              "gather.integrate", rs, key);
    integrateWork.setUnits(in.width * in.height / 1000u);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mIntegratePipeLayout, 0, 1,
                            &v.integrateSets[ring], 0, nullptr);
    JAH_GPU_CHECKPOINT(cmd);   // THE GPU CHECKPOINTS (GpuCheckpoints.h)
    vkCmdDispatch(cmd, (in.width + 7u) / 8u, (in.height + 7u) / 8u, v.layers);   // z = the eye
    integrateWork.close();
    // ...AND THE INTEGRATE'S OWN COUNTER WORDS (PHOTON-2 L8: the histories accepted only by the foliage rule; PHOTON-7
    // stage 10-3: the pixels starved of a rest sample at the hold, the diffuse's and the rough specular's) into the same
    // slot of the host ring, the three words after the placement's.
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
        VkBufferCopy region{};
        region.srcOffset = 4u;
        region.dstOffset = VkDeviceSize(v.frame % kFramesInFlight) * kReadbackSlotBytes + 4u;
        region.size = 16u;   // (words 1-4: the foliage rule, the two starved counts, the longest hand-over begun)
        vkCmdCopyBuffer(cmd, v.counter, v.readback, 1, &region);
        // ...and THE CELLS' CLOCK as the finalize and the ring left it (PHOTON-7 stage 8B, B1: GatherStatus's cells)
        VkBufferCopy clockRegion{};
        clockRegion.dstOffset = VkDeviceSize(v.frame % kFramesInFlight) * kReadbackSlotBytes + 20u;
        clockRegion.size = kClockWords * 4u;
        vkCmdCopyBuffer(cmd, v.clock, v.readback, 1, &clockRegion);
        VkMemoryBarrier h{};
        h.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        h.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        h.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &h, 0, nullptr,
                             0, nullptr);
    }
    // THE IRRADIANCE READBACK (a test door): the integrate's output, still in
    // the GENERAL layout its storage writes left it in, copied into this frame's
    // slot of a host ring and decoded once the frame has retired (readPending).
    bool readbackThisFrame = false;
    if (in.tuning.readback) {
        const VkDeviceSize slotBytes = VkDeviceSize(v.w) * v.h * v.layers * 8u;
        if (!v.irrReadback) {
            std::string rerr;
            if (!mHost.gatherMakeBuffer(slotBytes * kFramesInFlight,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, v.irrReadback,
                                        v.irrReadbackMemory, &v.irrReadbackMapped, rerr))
                v.irrReadback = VK_NULL_HANDLE;
        }
        if (v.irrReadback) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toCopy, 0, nullptr, 0,
                                 nullptr);
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(v.frame % kFramesInFlight) * slotBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = v.layers;
            region.imageExtent = { v.w, v.h, 1u };
            vkCmdCopyImageToBuffer(
                cmd, static_cast<Ogre::VulkanTextureGpu *>(v.irradiance)->getFinalTextureName(),
                VK_IMAGE_LAYOUT_GENERAL, v.irrReadback, 1, &region);
            // ...THE UNIFORM CELLS' RECORDS of the same frame (their twin links).
            const VkDeviceSize recBytes = VkDeviceSize(v.uniformProbes) * kRecordBytes;
            if (!v.recReadback) {
                std::string rerr;
                if (!mHost.gatherMakeBuffer(recBytes * kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            true, v.recReadback, v.recReadbackMemory,
                                            &v.recReadbackMapped, rerr))
                    v.recReadback = VK_NULL_HANDLE;
            }
            if (v.recReadback) {
                VkBufferCopy rc{};
                rc.dstOffset = VkDeviceSize(v.frame % kFramesInFlight) * recBytes;
                rc.size = recBytes;
                vkCmdCopyBuffer(cmd, v.records, v.recReadback, 1, &rc);
            }
            // The host reads it after the frame retires; and the copy is ordered
            // before whatever the layout transition below does to the image (an
            // execution chain through the compute stage Ogre's barrier starts at).
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &toHost, 0, nullptr, 0, nullptr);
            readbackThisFrame = true;
        }
    }
    // THE GATHER-HITS READBACK (FAR-SUN-1): this traced frame's per-ray records and the atlas
    // rows its uniform probes own — AFTER the decode's write-back, so a record's radiance is in
    // its texel — copied into this frame's slot of two host rings (readPending decodes them).
    bool raysThisFrame = false;
    if (in.hitsReadback && v.rays) {
        if (!v.raysReadback) {
            std::string rerr;
            if (!mHost.gatherMakeBuffer(v.raysSlotBytes() * kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                        v.raysReadback, v.raysReadbackMemory, &v.raysReadbackMapped, rerr))
                v.raysReadback = VK_NULL_HANDLE;
        }
        if (!v.raysAtlasReadback) {
            std::string rerr;
            if (!mHost.gatherMakeBuffer(v.raysAtlasSlotBytes() * kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        true, v.raysAtlasReadback, v.raysAtlasReadbackMemory,
                                        &v.raysAtlasReadbackMapped, rerr))
                v.raysAtlasReadback = VK_NULL_HANDLE;
        }
        if (v.raysReadback && v.raysAtlasReadback) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &toCopy, 0, nullptr, 0, nullptr);
            const unsigned slot = v.frame % kFramesInFlight;
            VkBufferCopy rc{};
            rc.dstOffset = VkDeviceSize(slot) * v.raysSlotBytes();
            rc.size = v.raysSlotBytes();
            vkCmdCopyBuffer(cmd, v.rays, v.raysReadback, 1, &rc);
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(slot) * v.raysAtlasSlotBytes();
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1u;
            region.imageExtent = { v.atlasW, std::min(v.raysAtlasRows(), v.atlasH), 1u };
            vkCmdCopyImageToBuffer(cmd, v.atlas, VK_IMAGE_LAYOUT_GENERAL, v.raysAtlasReadback, 1, &region);
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toHost, 0,
                                 nullptr, 0, nullptr);
            raysThisFrame = true;
        }
    }
    // THE HISTORY-COUNT READBACK (PHOTON-2 stage 0, the arm "gather.historyReadback"): this frame's pixel
    // history — the half the integrate just wrote, in GENERAL — copied into this frame's slot of a host ring;
    // readPending decodes its count. The barrier after it also orders the copy's read before the integrate that
    // writes this half again two frames on.
    bool historyThisFrame = false;
    if (in.tuning.historyReadback && temporal) {
        const VkDeviceSize slotBytes = VkDeviceSize(v.w) * v.h * v.layers * 9u;   // (8 B the history word, 1 B its streak)
        if (!v.histReadback) {
            std::string rerr;
            if (!mHost.gatherMakeBuffer(slotBytes * kFramesInFlight, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true,
                                        v.histReadback, v.histReadbackMemory, &v.histReadbackMapped, rerr))
                v.histReadback = VK_NULL_HANDLE;
        }
        if (v.histReadback) {
            VkMemoryBarrier toCopy{};
            toCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            toCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                 &toCopy, 0, nullptr, 0, nullptr);
            VkBufferImageCopy region{};
            region.bufferOffset = VkDeviceSize(v.frame % kFramesInFlight) * slotBytes;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = v.layers;
            region.imageExtent = { v.w, v.h, 1u };
            vkCmdCopyImageToBuffer(cmd, v.history[v.flip & 1u], VK_IMAGE_LAYOUT_GENERAL, v.histReadback, 1, &region);
            region.bufferOffset += VkDeviceSize(v.w) * v.h * v.layers * 8u;   // (the streak after the words)
            vkCmdCopyImageToBuffer(cmd, v.streak[v.flip & 1u], VK_IMAGE_LAYOUT_GENERAL, v.histReadback, 1, &region);
            VkMemoryBarrier toHost{};
            toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toHost, 0,
                                 nullptr, 0, nullptr);
            historyThisFrame = true;
        }
    }
    {
        // The pending record is what retires this frame's adaptive-count readback.
        View::Pending &pd = v.pending[v.frame % kFramesInFlight];
        pd.history = historyThisFrame;
        pd.rays = raysThisFrame;
        pd.frame = mHost.gatherFrameNow();
        pd.live = true;
        pd.irradiance = readbackThisFrame;
        pd.held = false;
        pd.gatherFrame = v.frame;
    }
    integrateWork.close();

    // ...AND NOW THE PIXEL SHADER READS IT. The transition to a sampled layout
    // is ours to ask for: the scene pass that follows does not know this texture
    // exists (it arrives through the Hlms listener's extra slot, not through the
    // compositor), so nobody else would order the barrier.
    {
        Ogre::BarrierSolver &solver = rs->getBarrierSolver();
        Ogre::ResourceTransitionArray trans;
        solver.resolveTransition(trans, v.irradiance, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        solver.resolveTransition(trans, v.roughSpec, Ogre::ResourceLayout::Texture,
                                 Ogre::ResourceAccess::Read, 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        solver.resolveTransition(trans, v.skyVis, Ogre::ResourceLayout::Texture, Ogre::ResourceAccess::Read,
                                 1u << Ogre::GPT_FRAGMENT_PROGRAM);
        rs->executeResourceTransition(trans);
    }
    // THE PROPERTY AND THE TEXTURE ARE SET TOGETHER OR NOT AT ALL — the same
    // rule the sky's env slot states: a slot claimed by getNumExtraPassTextures
    // and left unbound is an undefined descriptor. The registration is
    // PASS-SCOPED and `releaseBinding` takes it away when the pass ends.
    detail::FogHlmsListener::setProbeGather(in.sceneMgr, { v.irradiance, v.roughSpec, nullptr, v.skyVis });
    // ...AND THIS FRAME BECOMES THE PREVIOUS ONE: its camera, its half of each
    // pair, one more frame of age.
    std::memcpy(v.prevCamPos, in.camPos, sizeof(v.prevCamPos));
    std::memcpy(v.prevCamPos2, in.camPos2, sizeof(v.prevCamPos2));
    for (int i = 0; i < 3; ++i) {
        v.prevRayTL[i] = in.rayTL[i];
        v.prevRayRight[i] = in.rayRight[i];
        v.prevRayDown[i] = in.rayDown[i];
        v.prevFwd[i] = in.fwd[i];
        v.prevRayTL2[i] = in.rayTL2[i];
        v.prevRayRight2[i] = in.rayRight2[i];
        v.prevRayDown2[i] = in.rayDown2[i];
        v.prevFwd2[i] = in.fwd2[i];
    }
    v.flip ^= 1u;
    ++v.age;
    v.recordedFrame = mHost.gatherFrameNow();
    v.recordedDraw = v.scene ? v.scene->drawSerial() : 0u;
    v.lastTemporal = temporal;
    v.cpuMs = float(v.finishCpuMs + msSince(cpuStart));
    ++v.frame;
}

}   // namespace engine
}   // namespace jahshaka

#endif   // JAH_RAY_QUERY
