#pragma once
// THE ENGINE's GIGATHER TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>

namespace jahshaka { namespace engine {

/// ONE GATHER RAY, AS THE GATHER-HITS INSTRUMENT READ IT (FAR-SUN-1; GatherStatus::rays).
struct GatherRay {
    /// What answered it (GatherRay::Source), and for the voxels the FINEST cascade that did.
    enum Source : unsigned char { None = 0, Card = 1, Voxel = 2, Sky = 3, Decode = 4, DecodeFar = 5,
                                  Dropped = 6 };
    unsigned char source = None;
    unsigned char cascade = 0;
    /// Its uniform probe's cell (the probe's screen block is cell x stride) and its octahedral
    /// texel (y x octRes + x).
    unsigned short cellX = 0, cellY = 0;
    unsigned char ray = 0;
    /// The radiance the ray RETURNED, read from the probe's atlas after the decode's write-back
    /// (so a decode record's answer is in it; a dropped one reads 0) — the rgb the probe's SH
    /// is projected from, before any filter.
    float radiance[3] = { 0.0f, 0.0f, 0.0f };
    /// The hit: its distance (-1 = the sky), its world point (half precision) and its geometric
    /// normal (snorm8 per component; ZERO for a far hit, which names no triangle), and THE SUN'S VISIBILITY THERE — the hit's own sun ray against
    /// the shadow casters, whatever answered the ray: with the normal's cosine to the sun, the
    /// truth a source's radiance is held against (a chair's side as much as the street).
    float distance = -1.0f;
    float hitPos[3] = { 0.0f, 0.0f, 0.0f };
    float hitNormal[3] = { 0.0f, 0.0f, 0.0f };
    float sunVisibility = 0.0f;
    /// The ray's quadrature re-weight (what the probe's projection weighs its radiance by).
    float weight = 0.0f;
};

/// What the gather did on the last drawn frame of this scene.
/// ONE RETIRED GATHER FRAME's REST (PHOTON-7 stage 8B, B1; GatherStatus::restLog): its gather frame (GatherStatus::frame's
/// count before it advanced: the frame it traced), its rest clock K, the cells an input reached, held and active after
/// it, and whether every cell held.
struct GatherRestRecord {
    unsigned frame = 0u;
    unsigned restK = 0u;
    unsigned reached = 0u, held = 0u, active = 0u;
    bool everyHeld = false;
    /// THE FAST UPDATE (PHOTON-7 stage 8B, B5): the probes that frame with a ray whose hit moved, and those whose own
    /// amount F passed one half (rq_probe_gather.comp's instrument)
    unsigned movingProbes = 0u, fastProbes = 0u;
    /// ...and B3's hand-over: the longest a held cell's picture began to walk that frame, in frames (M = ceil(delta /
    /// one code)); 0 none began
    unsigned handoverM = 0u;
    /// ...and B3's refinement: a card region re-rested a held cell in the background that frame (its rest restarted, its
    /// picture held - the hold waits for its rest and its hand-over)
    bool refined = false;
};

struct GatherStatus {
    /// The scene's row resolved ON: `GiParams::gather` is On and this machine
    /// traces (a ray-capable device, the project's ray row not Off).
    bool on = false;
    /// ...and a view actually dispatched it. False with `on` true means no view
    /// of this scene carries the prepass the probes read their surface from.
    bool running = false;
    unsigned stride = 0u, octRes = 0u, raysPerProbe = 0u;
    /// The uniform grid, in probes.
    unsigned probesX = 0u, probesY = 0u, probes = 0u;
    /// ...and the adaptive probes the last frame appended, against its cap.
    unsigned adaptive = 0u, adaptiveCap = 0u;
    /// WHAT THE PLACEMENT JOB ASKED FOR, before the cap. `adaptive` is
    /// `min(requested, cap)`, so a test that asserts `adaptive <= cap` asserts
    /// an arithmetic identity and a test that asserts `adaptive == 0` under a
    /// cap of 0 asserts another (the lead's read). This is the number the cap
    /// is doing something TO: requested above cap is a frame that wanted more
    /// probes than it was allowed.
    unsigned adaptiveRequested = 0u;
    unsigned long long raysPerFrame = 0ull;
    /// The view the numbers below were measured on.
    unsigned targetW = 0u, targetH = 0u;
    /// A TWO-EYE TARGET (PHOTON-GA-VR): the probe list is two grids, one per
    /// eye's layer, `eyeProbesX` columns each (probesX = 2 x eyeProbesX); with
    /// one eye `eyeProbesX` = probesX. `targetW` is the eye PAIR's width.
    bool stereo = false;
    unsigned eyeProbesX = 0u;
    /// THE VIEW'S RESIDENT VRAM, in bytes (PHOTON-GA-VR): the octahedral
    /// radiance + hit-distance atlas, the probe records, the full-resolution
    /// irradiance target (8 B a pixel), the pixel history and the rest mean
    /// (8 B a pixel; none for a stereo view, which never rests) — and of it,
    /// `historyBytes`: the history's two packed halves, 16 B a pixel (8 B a
    /// texel each; the rgba16f + r32ui pairs before it were 24).
    unsigned long long vramBytes = 0ull;
    unsigned long long historyBytes = 0ull;
    /// GPU milliseconds per stage — each the frame monitor's row for its job
    /// ("gather.place" ... "gather.integrate"; one GPU-timing facility, lane
    /// TEST-1): -1 unless a capture is running (no query pool exists outside
    /// one) and until the first answer comes back, several frames later — and
    /// the CPU cost of RECORDING them on the thread that draws.
    float placeMs = -1.0f;
    float traceMs = -1.0f;
    float integrateMs = -1.0f;
    float cpuMs = -1.0f;
    /// ...and the FILTER in probe space (PHOTON-GATHER-1b), its own dispatch.
    float filterMs = -1.0f;
    /// THE PIXEL HISTORY (PHOTON-GATHER-1c): whether it ran on the last frame
    /// (false with the arm "gather.temporal" at 0) and the view's age
    /// (consecutive frames it has been written; 0 on a first frame, a resize, a
    /// scene bind, a tuning change).
    bool temporal = false;
    unsigned historyAge = 0u;
    /// THE SETTLED HISTORY (PHOTON-GATHER-1d; the term `GiStatus::giAtRest`
    /// carries). `restFrames` = the consecutive gathered frames at REST: the
    /// camera unmoved, the scene's lighting unchanged (a light write, an
    /// injection landing — a chain settle, a cascade rebuild or step, the single
    /// volume's own), the scene's geometry unmoved and the surface cache idle,
    /// the estimator unchanged; `settleFrames` = N, the frames a step of
    /// `kGatherSettleCodes` display codes takes to fall under one code through the
    /// history's EMA, N = ceil( ln(1/D) / ln(1 - 1/historyFrames) ) (16 at the
    /// shipped 10 frames), and the length of the REST MEAN: at rest the answer is
    /// handed over from the EMA to a true mean of the rest frames alone over N
    /// frames, IS that mean at the N-th and is then HELD (nothing is dispatched)
    /// — so a still picture is a function of the scene and the camera and not of
    /// its history or of how long it was waited for, and two screenshots of it
    /// agree byte for byte. The held picture is ONE N-sample mean: where a probe's
    /// estimate is noisy (a small bright source at a grazing angle) it keeps that
    /// draw's error until something moves. The pixel history is written with the
    /// handed-over value, so a camera that starts moving again continues from the
    /// rest mean without a pop. THE REST IS NOT THE SETTLE (the fix round):
    /// `sinceRestart` = the frames the history has drawn since its last RESTART
    /// — its birth, a resize, a scene bind, the lever, an estimator-changing
    /// tuning, or a DISCONTINUITY of the scene (a light, sky or material write,
    /// an injection landing; OgreScene::gatherRestartKey). A continuous transform
    /// write (an animation, a socket rider, a mover) is not a restart — the
    /// reprojection follows it per pixel — and it never lets the scene REST. So
    /// `settled` = sinceRestart >= N (true when the history does not run): a
    /// still scene is settled on the frame its rest mean is complete (the two
    /// counts agree), and a scene with a per-frame writer is settled on the N-th
    /// frame of its EMA — a picture that is not byte-stable by nature. A YOUNG
    /// VIEW (a two-frame screenshot) is NOT settled: it shows the raw estimate —
    /// up to 9/255 of probe noise on a Showroom-shaped room.
    unsigned restFrames = 0u;
    unsigned sinceRestart = 0u;
    unsigned settleFrames = 0u;
    /// THE HISTORY's FLOOR h itself (ROUGH-LOBE-HISTORY-1): the pixel history is a true mean for h frames, then an EMA
    /// at 1/h - the shipped kHistoryFrames, or the A/B arm gather.historyFrames (GatherTuning::historyFrames) - the
    /// latest views' largest, as settleFrames is (settleFrames = settleFramesOf(historyFrames)). 0 = no view gathered.
    unsigned historyFrames = 0u;
    bool settled = true;
    /// THE HOLD ITSELF: EVERY CELL of every latest view of the scene holds (PHOTON-7 stage 8B, B1: the rest is per
    /// screen cell - a cell an input reached resumes, every other cell keeps its rest picture; the view's rest clock
    /// past N means every cell holds).
    bool held = false;
    /// THE CELLS (PHOTON-7 stage 8B, B1), the latest retired frame's (a few frames late): the cells an input reached
    /// that frame, those held and those active after it, those traced as the ring of an active cell; and the frames
    /// any cell resumed on (cumulative).
    unsigned cellsReached = 0u, cellsHeld = 0u, cellsActive = 0u, cellsRing = 0u;
    unsigned long long resumes = 0ull;
    /// ...AND THE RETIRED FRAMES' OWN RECORDS (the last 32, oldest first), by the gather frame each one traced (`frame`
    /// counts them: a record names the frame it belongs to, so a reader attributes it there and not to the frame it
    /// arrived in): its rest clock, the cells reached, held and active, and whether every cell held after it.
    std::vector<GatherRestRecord> restLog;
    /// THE CAMERA CUTS (PHOTON-2 round 2, N3): the frames a view of the gather detected a cut on (a view axis turned
    /// more than 75 degrees or the eye moved more than 100 m between two frames - Lumen's rule) and restarted its
    /// history; the most any view counted.
    unsigned cameraCuts = 0u;
    /// THE FOLIAGE RULE'S OWN ACCEPTS (PHOTON-2 L8, K5's guard): the pixels whose history only the foliage class's
    /// looser test accepted (a depth delta between the 1 % and the 3 % bounds), summed over the gathered frames since
    /// the view began (a few frames late). 0 under gather.classOff by construction.
    unsigned long long foliageRuleAccepts = 0ull;
    /// THE STARVED PIXELS AT THE HOLD (PHOTON-7 stage 10-3; L8 R2): the pixels whose rest took no sample by the hold - the
    /// diffuse's (the field's fallback answers them) and the rough specular's (the probe or the sky answers them) - the
    /// latest views' last traced frame, the most any view counted. A one-pixel sliver is the typical one.
    unsigned starvedDiffuse = 0u, starvedRough = 0u;
    /// ...AND THE LAST SHOT's (the shot-view fold, PHOTON-7 stage 10-3): a screenshot's shot view is destroyed right after
    /// its readback, before its last traced frame's counters retire, so its own counts are read at its destruction (the
    /// device finished that frame: the readback waited) and kept here for the scene until its next shot. shotValid
    /// false = no shot of this scene has been destroyed yet.
    bool shotValid = false;
    unsigned shotStarvedDiffuse = 0u, shotStarvedRough = 0u;
    /// THE READBACK (`GatherTuning::readback`): the last retired frame's
    /// `probeIrradiance`, row-major, four floats per pixel (rgb = E/pi, the
    /// mean radiance over the cosine-weighted hemisphere of the pixel's own
    /// normal; w = 1 where probes answered, 0 where the fallback owns the
    /// pixel). Empty without the readback, and until a frame has retired.
    std::vector<float> irradiance;
    unsigned irradianceW = 0u, irradianceH = 0u;
    /// Which gather frame the readback is of (the view's frame counter), so a
    /// suite can tell a fresh copy from a repeat.
    unsigned irradianceFrame = 0u;
    /// ...and the view's frame counter NOW: the next frame the gather records
    /// carries this number (GATHER-NOISE-1), so a suite that moves the camera
    /// every frame knows which readback is which pose.
    unsigned frame = 0u;
    /// THE PIXEL HISTORY'S COUNT (GatherTuning::historyReadback; PHOTON-2 stage 0): per pixel, row-major
    /// (historyCountW x historyCountH, the eye pair side by side like `irradiance`), the count the integrate wrote
    /// on gather frame `historyCountFrame` (0..15: 1 = no usable history that frame, n = n - 1 frames carried).
    /// Empty with the arm off and until a frame recorded under it has retired; a HELD frame writes no history and
    /// copies none.
    std::vector<unsigned char> historyCount;
    /// ...and per pixel the SURFACE CLASS the integrate wrote with it (PHOTON-2 K5: 1 = foliage, the cut-out
    /// class of the prepass's normals alpha - written under GatherTuning::classOff too), the same layout.
    std::vector<unsigned char> historyClass;
    /// ...and per pixel the VIEW DISTANCE the integrate wrote (the history word's bits 0-20: linear depth along the
    /// view axis; 0 = no surface), the same layout (PHOTON-2 round 2, M1's moving window).
    std::vector<float> historyDist;
    /// ...and per pixel the history's VALIDITY STREAK (PHOTON-7 stage 8B): the frames its history has been carried
    /// unbroken (0..255; 1 = none usable this frame) - what "the gather saw it before the stop" means (the stillness
    /// door's mask); `historyCount` is the BLEND's N (B5's fast update stores the shortened N'), never an age.
    std::vector<unsigned char> historyStreak;
    unsigned historyCountW = 0u, historyCountH = 0u;
    unsigned historyCountFrame = 0u;
    /// THE GATHER-HITS READBACK (FAR-SUN-1; the arm "gather.hitsReadback"): what answered EVERY
    /// RAY of every uniform probe of the last retired traced frame, row-major over the probe
    /// grid (probesX x probesY) and, inside a probe, by ray (raysPerProbe each). Empty with the
    /// arm off and until a frame that traced under it has retired; a view that holds its rest
    /// traces nothing, so the arm keeps the view tracing.
    std::vector<GatherRay> rays;
    /// ...which gather frame it is of (GatherStatus::frame's counter).
    unsigned raysFrame = 0u;
    /// ...and THE SAME FRAME'S ADAPTIVE TWINS (the readback door too; PHOTON-II-1 F3): per
    /// uniform cell, row-major over the probe grid (probesX x probesY), 1 where the cell got
    /// its second probe that frame. How a suite proves the budget's ranking starves no cell.
    std::vector<unsigned char> adaptiveCells;
    /// WHY IT IS NOT RUNNING, when `on` is true and `running` is false and the
    /// reason is the engine's rather than the view's (no ray device, no
    /// pipelines on this driver, no room for the atlas). Empty is "nothing went
    /// wrong" — a row that silently does nothing is the worst of the three
    /// outcomes.
    std::string error;
};

// ---------------------------------------------------------------------------
// HARD SUN CONTACT SHADOWS (PHOTON P5, RY-R3; SPECS/photon/C4_RAYS_BEYOND_
// REFLECTIONS_DESIGN.md section 1).
//
// ONE HARDWARE RAY PER PIXEL towards the sun, from the surface the prepass
// drew, against the scene's own TLAS, out to a short CONTACT RANGE. The answer
// (1 = the sun reaches this point, 0 = something is in the way) is folded into
// the first directional light's shadow term as `min( fShadow, visibility )`:
// the shadow map keeps every shadow it has, and the ray closes what the map's
// depth bias opens — the band of light a PSSM bias leaves under the edge of
// anything standing on the ground. Beyond the range the map answers alone.
//
// OFF BY DEFAULT (a project row, `world.sunContact`); never in VR (the stereo
// chain declines it); never where the scene does not trace (the project's ray
// row, the machine, --no-ray-query). With it off nothing is allocated, nothing
// is dispatched, no shader property is set and no pixel moves.

/// THE CONTACT RANGE's default, metres: the distance a ray looks for an
/// occluder before it leaves the question to the shadow map. The design's
/// 2 m — a PSSM bias leaks centimetres, so a range of a couple of metres covers
/// every contact the map gets wrong and costs the traversal nothing it needs.
constexpr float kSunContactDefaultRange = 2.0f;
/// ...and the band a row may hold it in (a ray of zero length answers nothing;
/// a ray of a kilometre is a second shadow map, not a contact term).
constexpr float kSunContactMinRange = 0.05f;
constexpr float kSunContactMaxRange = 50.0f;

/// The job's resolution. `Auto` follows the tier: HALF at the Low and Medium GI
/// quality rows (one ray per 2x2 block), FULL at High and Epic.
enum class SunContactResolution { Auto, Full, Half };

/// What the contact job did on the last drawn frame of this scene.
struct SunContactStatus {
    /// The row resolved ON: enabled, and this scene traces on this machine.
    bool on = false;
    /// ...and a view dispatched it on its last frame (false with `on` true: no
    /// view of the scene carries the prepass, the view is stereo, or the pass'
    /// shadow node holds no directional caster — `reason` says which).
    bool running = false;
    /// The texture the pixel read, in texels, and the target it covers.
    unsigned width = 0u, height = 0u, targetW = 0u, targetH = 0u;
    /// 1 = one ray per pixel, 2 = one per 2x2 block.
    unsigned divisor = 0u;
    unsigned long long rays = 0ull;
    /// The range the rays were cast to, and the world-space direction they were
    /// cast along (towards the sun: the first directional shadow caster's).
    float range = 0.0f;
    float toSun[3] = { 0.0f, 0.0f, 0.0f };
    /// GPU milliseconds of the dispatch ("sun.contact", the frame monitor's row:
    /// -1 unless a capture is running, lane TEST-1), and the CPU cost of recording it.
    float gpuMs = -1.0f;
    float cpuMs = -1.0f;
    /// Why it is not running when `on` is true; empty when nothing declined.
    std::string reason;
};

/// THE PIXEL'S AREA LIGHT VISIBILITY (PHOTON-7 stage 7, 7-A; ScenePublished::areaVis): the ray tier's trace of each
/// forward LTC light's visible share of its form factor - the ratio over n x n strata of its rectangle, the cards' law
/// (one text, jah_area_strata.glsl), diffuse-weighted and specular-weighted - into the screen product `jahAreaVis` the
/// shading pass multiplies the light's LTC diffuse and specular by (the fork's hook in DoAreaLtcLights). Mono views only
/// (a stereo view declines: its area lights unshadowed).
struct AreaVisStatus {
    /// A view of the scene traced it (or held it) on its last frame.
    bool running = false;
    /// The forward LTC lights it traced (HlmsPbs's order, at most four) and the strata a side (the tier's
    /// GiQualityFacts::cardAreaStrata: the cards' n).
    unsigned lights = 0u, strata = 0u;
    /// The trace grid and the target it covers.
    unsigned traceW = 0u, traceH = 0u, targetW = 0u, targetH = 0u;
    /// Consecutive rest frames; past n^2 of them the view HOLDS (nothing dispatched, the visibility as it is).
    unsigned restFrames = 0u;
    bool held = false;
    unsigned long long rays = 0ull;
    /// GPU milliseconds of the trace and the resolve (the monitor's rows "area.vis" and "area.resolve": -1 outside a
    /// capture) and the CPU cost of recording them.
    float traceMs = -1.0f, resolveMs = -1.0f, cpuMs = -1.0f;
    /// The view's resident VRAM: the two sums' ping-pong and their rest means at the trace's size, the surface words,
    /// the full-resolution RG32_UINT product (eight bytes a pixel).
    unsigned long long vramBytes = 0ull;
    /// Why it is not running, empty when nothing declined (no LTC light in the forward set is the free case).
    std::string reason;
    /// THE READBACK (a test door, the arm area.visReadback): a recent retired frame's `jahAreaVis`, row-major, EIGHT bytes
    /// a pixel - bytes 0-3 the diffuse-weighted visibility of lights 0-3 (unorm8), bytes 4-7 the specular-weighted - the
    /// target's size; empty without the arm.
    std::vector<unsigned char> vis;
    unsigned visW = 0u, visH = 0u;
};

}}  // namespace jahshaka::engine
