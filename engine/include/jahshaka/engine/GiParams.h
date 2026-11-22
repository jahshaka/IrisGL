#pragma once
// THE ENGINE's GIPARAMS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>
#include "jahshaka/engine/GiGather.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

/// Scene-level GI state, pushed idempotently via Scene::setGlobalIllumination.
struct GiParams {
    GiMode    mode    = GiMode::Off;
    GiQuality quality = GiQuality::Medium;
    /// THE REFLECTION-PROBE GRID'S PLACEMENT REGION — TEST LEVERS (owner
    /// decision D8, 2026-09-13; renamed by D4-PHOTON-TIERS). The region is where a
    /// probe grid is PLACED on a machine that does not trace its reflections (a
    /// pinned hybrid, the rays Off, no ray hardware); it is a placement heuristic
    /// pending A9's camera-centred window, NEVER a lighting volume — the voxels
    /// are the camera's cascade chain and know nothing of it.
    ///
    /// Nothing a user can reach writes them: the document carries no region and
    /// SceneMirror never touches them. Suites PIN a region so that a probe
    /// assertion is about the thing it names and not about where the automatic
    /// fit landed. Inside `operator==` on purpose: a moved pin is a re-placement.
    ///
    /// `testProbeRegionMin == testProbeRegionMax` (the default) is "no pin": the
    /// engine fits the region to the scene's content, under `kProbeGridFitMax`.
    Vec3      testProbeRegionMin, testProbeRegionMax;
    /// The ceiling on the AUTOMATIC probe-region fit, in metres — a test lever
    /// over `kProbeGridFitMax` (OgreGi.cpp, 64 m). Negative (the default) is the
    /// engine's own; 0 disables the ceiling; anything else replaces it.
    float     testProbeGridFitMax = -1.0f;
    /// THE BOUNCES (PHOTON-7 stage 4): on, every surface re-emits what it gathers - the lamps', the sky's and every
    /// other surface's light - and the voxel store converges to the series' limit by FEEDBACK (a pass a frame over the
    /// bricks still moving, each stopped once the tail of its series is under a code): no count of bounces exists any
    /// more. Off = THE DIRECT STORE ALONE (BOUNCES-ZERO-1): the voxels hold the lamps' direct light and nothing a surface
    /// re-emits of the sky or of another surface.
    bool      bounces = true;
    /// Hybrid only: reflection-probe counts along each world axis of the GI
    /// bounds (the parallax-corrected cubemap grid). Clamped to 1..8 per axis.
    int       pccProbesX = 3, pccProbesY = 2, pccProbesZ = 3;

    // ---- Probe capture options (hybrid only; REFLECTIONS_ADOPTION_SPEC P3) ----
    /// HDR probe captures — PFG_RGBA16_FLOAT instead of PFG_RGBA8_UNORM_SRGB.
    /// The main chain renders RGBA16_FLOAT, so an LDR probe clips every
    /// highlight BEFORE the IBL convolution that blurs it across the mip chain;
    /// in a room the lamps and windows ARE the reflection content. Costs 2x the
    /// probe VRAM (High/512: ~151 -> ~302 MB at 18 probes), which is why Auto
    /// means "on at GiQuality::High only".
    GiToggle  probeHdr = GiToggle::Auto;
    /// Shadowed probe captures — the probe's six face renders run the scene's
    /// shadow node with `recalculate`, so the reflection contains the room's
    /// shadows instead of a uniformly lit room. Multiplies each face render by
    /// the shadow pass count; Auto means "on at GiQuality::High only". Falls
    /// back silently to the unshadowed capture when no shadow node exists (a
    /// headless engine has none) — GiStatus reports what actually happened.
    GiToggle  probeShadows = GiToggle::Auto;
    /// PROBE CAPTURE SIZE — the pixel size of one cube face, and the single
    /// biggest VRAM lever the hybrid has (REFLECTION_PROBE_AUDIT §4.2: a probe
    /// costs `6 * size^2 * bytes * mips`, so halving it quarters the grid).
    /// 0 = follow the quality dial (128 at Low, 256 at Medium and High since
    /// the 2026-09-13 halving: 512 spent 16 MiB per probe — 512 MiB on a
    /// 32-probe room — for detail the roughness mip chain blurs away). Any
    /// other value is taken verbatim, clamped to 64..1024 and rounded down to
    /// a power of two, because Ogre's IBL mip chain is built from it.
    int       probeCaptureSize = 0;
    /// How far each probe's influence volume is stretched past its 1/N share of
    /// the probe region, so neighbours blend instead of showing a hard seam.
    /// 1.0 = no overlap (visible seams), the pin's own ctor default is 1.5, and
    /// upstream's sample ships 1.25 — which is ours (P3c): less probe-count
    /// pressure on the Forward+ cubemap slots for blending that is already
    /// smooth. Valid range (0, inf).
    float     probeOverlap = 1.25f;
    /// Probe shrink-fit snapping, applied uniformly to all three axes. After
    /// the depth readback re-fits each probe to the geometry around it, a face
    /// that landed within this RELATIVE error of the probe region snaps back
    /// out to it — the cure for "the wall itself has no reflection" when the
    /// fit stops a hair short of it. `snapSidesMin/Max` are the same idea for
    /// probes sitting on the region's own faces/corners, where a much larger
    /// error is safe (upstream reasons about it at length in
    /// PccPerPixelGridPlacement::setSnapSides). These were never set before —
    /// they are the pin's ctor defaults, now OURS and explicit, so an upstream
    /// bump cannot move probe placement without a diff saying so.
    float     probeSnapDeviation = 0.05f;
    float     probeSnapSidesMin  = 0.25f;
    float     probeSnapSidesMax  = 0.25f;
    /// THE GI UPDATE BUDGET (FIX WAVE B1/B2, 2026-09-07) — how many probe
    /// UPDATES the renderer may spend per frame. It replaces P5a's
    /// `dynamicProbes` ("keep the nearest N live for ever") and the document's
    /// old giAutoRefresh flag, which were two spellings of the same question.
    ///
    /// A RATE, not a subset, and spent only on STALE probes (ENGINE_CACHE_POLICY_
    /// SPEC P1, 2026-09-12): the probes are a cache. Every input that changes
    /// what a probe would capture — a GI rebuild or re-solve, STILL geometry
    /// moving, arriving, leaving or being shown/hidden, a light, material, sky,
    /// ambient or fog change, an object's mobility changing — marks the grid
    /// stale (GiStatus::lastStaleReason names it; time-varying content is
    /// frozen, and MOVABLE objects are not in a capture at all, so moving one
    /// is not an input — see GiStaleReason and MobilityStatus), and
    /// each frame the engine re-captures up to `updateBudget` of the
    /// highest-priority stale probes. So every probe re-captures within
    /// ceil(probeCount / updateBudget) frames OF A CHANGE, whatever the priority
    /// does — and a still scene re-captures NOTHING (it used to re-capture one
    /// probe every frame for ever). Priority (staleness x proximity to the
    /// tracked camera x covers-something-that-just-moved) only decides the ORDER
    /// of the catch-up, which is what puts the probes the viewer can see, and
    /// the ones the moving object is inside, at the front of it.
    ///
    /// 0 = PAUSED: no probe re-captures, and the mirror stops auto-refreshing GI
    /// as well (it is the same "GI is frozen" intent). That is the pre-fix-wave
    /// shipped behaviour, kept as one switch.
    ///
    /// 1 (the default) is a realtime editor: at most one probe face-set per
    /// frame while anything is stale, measured at ~2.1 ms in a Debug build at
    /// Medium quality (256px faces), and nothing at rest. Raising it buys
    /// catch-up latency at a linear cost. `GiStatus::probeUpdatesPerFrame`
    /// reports the resolved figure (clamped to the probes that exist).
    ///
    /// LOUD CONSEQUENCE, because it changes the picture: while this is above 0
    /// the PCC/VCT trust window is inverted (P5a's finding — see the long note in
    /// OgreGi.cpp buildPcc). Probes are fresher than the voxel volume by
    /// construction once they re-capture, so a probe that covers a pixel wins it,
    /// and ROUGH surfaces inside the probe region take their environment from the
    /// probes instead of from cone tracing. Mirror-sharp pixels do not move.
    int       updateBudget = 1;
    /// DDGI — the irradiance-field diffuse layer (GI_UNIFIED_SPEC.md §4 P1).
    ///
    /// On, and in a VCT mode, the engine builds an `Ogre::PhotonIrradianceField` over
    /// the SAME voxel volume VCT already lit (Majercik et al.: octahedral
    /// irradiance + depth-visibility probes, cone-traced out of PhotonVoxelLighting) and
    /// binds it to every PBS pass. It is a DIFFUSE layer only: VCT keeps the specular
    /// cones, the probes/planar/SSR keep everything they had.
    ///
    /// THE ONE THING TO KNOW BEFORE TURNING IT ON: binding a field makes
    /// HlmsPbs set `VctDisableDiffuse`, so DDGI REPLACES the voxel-cone diffuse
    /// rather than adding to it: one integral of one radiance field (since
    /// PHOTON-READER-1 its probe rays march the same cascade chain through the
    /// same voxel reader as the cones), applied at the field's own answer —
    /// nothing is calibrated into it and nothing trims it. WHERE THE SCREEN-PROBE
    /// GATHER RUNS (a ray tier, `gather` above) the field is NOT the diffuse: it
    /// is the fallback cage for a pixel no probe answered, and the gather is the
    /// diffuse. At Low it is the diffuse.
    ///
    /// GiToggle::Auto means "the tier's": it resolves through the ONE tier table
    /// (`giQualityFacts(...).fieldDefault` — on at every quality, the owner's
    /// option (b)) exactly as probeHdr, probeShadows, gather and cards resolve
    /// theirs (D4-PHOTON-TIERS deleted the "Auto is OFF so old scenes render as
    /// they did" arm). The struct's own default is OFF: a bare GiParams asks for
    /// no field, the way it asks for GI Off; a document's untouched field (-1)
    /// arrives as Auto and gets the tier's answer.
    ///
    /// Ignored outside GiMode::Vct / GiMode::VctPccHybrid: the field is fed by
    /// PhotonVoxelLighting, so there is nothing to build without a voxel volume. A
    /// DDGI-only mode is deliberately NOT offered — with no VCT bound the
    /// shader's ambient gate (`@property(vct_num_probes) if(vctSpecular.w==0)`)
    /// disappears and the sky/flat ambient would be counted twice on top of the
    /// field's own diffuse (P0 spike §5, measured).
    GiToggle  ddgi = GiToggle::Off;

    // ---- PHOTON: camera-centred voxel cascades (PHOTON_SPEC P0) -------------

    /// ONE camera-centred cascade of the Photon cascade chain.
    ///
    /// `halfSize` is the half-extent in METRES of the cube this cascade covers
    /// (the sample set's inner cascade is 5 m, i.e. a 10 m box); `resolution`
    /// its voxel resolution per axis; `stepCells` how many CELLS the camera may
    /// travel before the cascade re-centres (the pin's `cameraStepSize`, so
    /// the scroll distance is `stepCells * 2 * halfSize / resolution`).
    ///
    /// The step is in cells and not in metres on purpose: it is what keeps a
    /// re-centred cascade aligned to the same world lattice it was built on,
    /// which is the property that makes a scroll a whole number of cells and
    /// stops the bounce sliding under the geometry.
    struct GiCascadeDesc {
        float halfSize   = 0.0f;
        int   resolution = 0;
        float stepCells  = 0.0f;
        bool operator==(const GiCascadeDesc &o) const {
            return halfSize == o.halfSize && resolution == o.resolution &&
                   stepCells == o.stepCells;
        }
    };
    /// (THE VOXELS ARE ALWAYS THE CAMERA-CENTRED CHAIN. The single scene-fitted
    /// volume and its switch `cascades` are deleted — D4-PHOTON-TIERS, the
    /// owner's law: no fixed GI volume, no "room" in any lighting definition.)
    /// How many cascades to build, 1..kGiTierMaxCascades. 0 means "the table below decides", and
    /// when the table is empty too, the engine's own tier table does.
    int       cascadeCount = 0;
    /// The cascade table. Entries [0, cascadeCount) are used; a zero
    /// `resolution` or `halfSize` in a used entry falls back to the tier table.
    /// The engine's default is the Ogre sample's set (5 m@128, 10 m@128,
    /// 15 m@64, 60 m@64), which PHOTON_SPEC §5 measured the cadence of.
    GiCascadeDesc cascadeSet[kGiTierMaxCascades];
    /// THE PER-CASCADE INSTANCE BUDGET (PHOTON_SPEC §7 E2 (1), audit B7).
    ///
    /// The raster voxeliser's price is the GEOMETRY INSIDE THE REGION and
    /// nothing else — measured at ~13 us per enclosed instance (P0 §6.2), which
    /// is 3-6 ms for a room and 60-110 ms for a dense world, on the frame
    /// thread, for ONE cascade. A budget is the patch-free lever: each cascade
    /// voxelises AT MOST this many objects, and it keeps the ones that matter
    /// most to it — ranked by how much of one of ITS OWN voxels each object
    /// fills (world size / cell), largest first, ties broken by distance to the
    /// cascade's centre.
    ///
    /// Ranking by size-in-cells and not by size is what makes one number right
    /// for a whole chain: the same 1 m crate is 13 cells across for the inner
    /// cascade and half a cell for the outer one, so the outer cascade spends
    /// its budget on the buildings and the inner one on the crates.
    ///
    /// 0 (the default) is NO BUDGET, and it is the shipped arm exactly: the
    /// attach set is the whole size-filtered scene and the gather's per-partition
    /// cull decides what each build voxelises, on the device. A budget is the one
    /// input still computed on the CPU (its ranking is a partial sort): the pick
    /// is re-made at every rebuild of a cascade it binds and handed to the gather
    /// as a bit per instance slot.
    ///
    /// `GiStatus::cascades[].items` reports what each cascade voxelised and
    /// `[].attached` what it holds, so a budget that is biting is a reading.
    int       cascadeInstanceCap = 0;
    /// THE FAR-FIELD PROXY: a cascade voxelises the BAKED LOD LEVEL that fits
    /// its own cell (ATOM stage 1's hand-off, SPECS/NANITE_SPEC.md §7 — the
    /// rule is `lodLevelForWorldError` and the site is
    /// OgreScene::cascadeGatherInputs, applied by the gather). True (the default) spends the chain; false
    /// voxelises every cascade at the authored level, which is what the arm did
    /// before fork ad452604a+0338ca7f2 (was 0064) existed.
    ///
    /// It is here — an engine parameter rather than a document row — because it
    /// is the A/B: the same scene, the same chain, one term moved, so the cost
    /// it saves and the voxels it moves can be measured against each other in
    /// ONE process (no live row does; the archived gi.cascade_lod did). A mesh with no LOD chain
    /// is unaffected either way, and that is most of what a scene holds today:
    /// only IMPORTED static meshes are baked with one (document primitives are
    /// not).
    bool      cascadeVoxelLod = true;
    /// A DRAGGED STILL RIDES THE MOVER CHANNEL FOR THE LENGTH OF THE GESTURE
    /// (MOVER-1, the render audit's PHOTON F3 second half). OFF by default:
    /// this changes the picture WHILE an object is being dragged, so it is the
    /// owner's decision and not a fix.
    ///
    /// WHAT IT IS FOR. A Still object that is being dragged stales the cascade
    /// grid on every frame it moves, and the scheduler spends one cascade
    /// re-voxelisation per frame answering it (~1.4 ms of CPU submission on the
    /// Mirror Room) — and the frames in between are read through a volume that
    /// holds the object in two places at once. With this on, the SECOND move
    /// inside the settle window promotes the object onto the mover channel
    /// Mobility already defines (kMovableBit, no kGiGeometryBit): the cascades
    /// pay ONE re-voxelisation at the promotion, which clears the copy of it
    /// they hold at its old pose, and nothing at all for the rest of the
    /// gesture; at rest it goes back to being still world and pays ONE more.
    ///
    /// WHAT IT COSTS, stated rather than hidden: while it is dragged the object
    /// bounces no light into the room and the room stores no shadow of it — it
    /// is LIT by the irradiance field and the cones at its live pose, exactly
    /// as every Movable object in the scene is. What it gains is that the
    /// frames of the drag are read through a volume that is CONSISTENT, and
    /// that the drag costs the voxels two re-voxelisations instead of one per
    /// frame. THE PICTURE AT REST IS IDENTICAL either way, which gi.drag_mover
    /// asserts by rendering it under both rules.
    bool      dragMoverChannel = false;

    // THE SCREEN-PROBE GATHER HAS NO ROW (PHOTON-7 stage 7, 7-5; "never turn core stages off"): wherever the
    // machine traces and the GI mode is on it IS the diffuse of every view that presents (and of a still picture),
    // at the tier table's density (giQualityFacts(...).gather) - no toggle turns it off, because on a ray chain
    // nothing else produces the diffuse the opaque passes read (they bind no world cache).
    // ---- SURFACE-CACHE phase 2: the card cache's three knobs ---------------
    //
    // WHY THEY LIVE ON GiParams AND NOT ON A STRUCT OF THEIR OWN: the cache is
    // a Photon consumer — it captures the surfaces whose hits Photon's rays
    // will light — and every other Photon dial is here, so a host pushes ONE
    // configuration and the comparison below decides whether anything must be
    // rebuilt. `cards` is deliberately in `operator==` (an atlas is allocated
    // or freed by it); the BUDGET and the RADIUS are deliberately NOT — they
    // are read per frame by the residency pass, exactly like the three tuning
    // floats above, so dragging either of them re-captures nothing.
    /// Off, or ON WHEREVER THE RAYS RUN (PHOTON-CARDS-2; ATOM-S3-CARDCAP): a
    /// card's readers are the ray jobs' hits and its sun term is itself traced,
    /// so the cache exists exactly where the ray tier does (the scene's ray row
    /// resolved against the machine) and nowhere else. There is no "force on":
    /// without rays nothing could fill a card's sun term or read it.
    bool      cards = true;
    /// THE PER-FRAME TEXEL BUDGET — Lumen's shape (its capture budget is 512 x
    /// 512 texels a frame) and the number the whole capture cadence is sized
    /// by. 0 = the tier's own (kCardBudgetTexels below).
    ///
    /// WHY TEXELS AND NOT CARDS OR MILLISECONDS: a capture's CPU cost is the
    /// PASS's fixed cost (SURFACE-CACHE-0 measured 0.042-0.057 ms per card pass
    /// whatever the card's size), but its GPU cost and its memory traffic are
    /// the texels — so a budget in cards would let one 128-texel card cost the
    /// same as one 8-texel card, and a budget in milliseconds would be a wall
    /// clock, which measures nothing in this engine (CLAUDE.md). Texels are the
    /// thing that is actually spent, and they convert to milliseconds through a
    /// number that was measured once.
    int       cardBudgetTexels = 0;

    /// "Is this the same GI configuration I last pushed?" Exact, like every
    /// other change guard here — and load-bearing rather than cosmetic: a GI
    /// push is a teardown plus a re-voxelize plus (in the hybrid) every probe
    /// re-rendered twice, so a comparison that missed a field would either
    /// rebuild GI every frame or never notice a dial moving.
    ///
    /// EVERY FIELD setGlobalIllumination READS IS HERE — add one above and add
    /// it here. (The mirror hand-wrote this comparison over 24 fields; keeping
    /// it beside the struct is what makes "add a field" a one-place edit.)
    ///
    /// THE TUNING FLOATS ARE DELIBERATELY ABSENT (PHOTON_SPEC §7 E2 (8),
    /// audit A F6): the per-frame rows below are read per frame, so they take effect through `Scene::setGiTuning` without
    /// a rebuild — and while they were IN this comparison every tick of those
    /// sliders was a from-scratch teardown and re-voxelisation (N of them
    /// under a cascade chain). `giTuningEqual` is their comparison; a host
    /// pushes on `!(a == b)` for the configuration and on `!a.giTuningEqual(b)`
    /// for the tuning.
    bool operator==(const GiParams &o) const {
        return mode == o.mode && quality == o.quality &&
               bounces == o.bounces && testProbeGridFitMax == o.testProbeGridFitMax &&
               pccProbesX == o.pccProbesX && pccProbesY == o.pccProbesY &&
               pccProbesZ == o.pccProbesZ &&
               probeHdr == o.probeHdr && probeShadows == o.probeShadows &&
               probeCaptureSize == o.probeCaptureSize &&
               probeOverlap == o.probeOverlap &&
               probeSnapDeviation == o.probeSnapDeviation &&
               probeSnapSidesMin == o.probeSnapSidesMin &&
               probeSnapSidesMax == o.probeSnapSidesMax &&
               updateBudget == o.updateBudget &&
               cascadeVoxelLod == o.cascadeVoxelLod &&
               dragMoverChannel == o.dragMoverChannel &&
               ddgi == o.ddgi &&
               testProbeRegionMin == o.testProbeRegionMin &&
               testProbeRegionMax == o.testProbeRegionMax &&
               cascadeCount == o.cascadeCount &&
               cascadeInstanceCap == o.cascadeInstanceCap &&
               cascadeSetEqual(o);
    }
    /// The values `Scene::setGiTuning` pushes, compared on their own.
    bool giTuningEqual(const GiParams &o) const {
        return // THE CARD CACHE'S BUDGET (SURFACE-CACHE phase 2). It belongs in
               // THIS comparison and not in `operator==` for the same reason the
               // three above do: the capture pass reads it every frame, so moving
               // it takes effect on the next frame with nothing torn down — and a
               // host that only pushed on `operator==` would swallow the change
               // entirely, which is the defect this line exists to prevent.
               cards == o.cards && cardBudgetTexels == o.cardBudgetTexels;
    }
    /// The cascade table, compared only over the entries in USE — a table
    /// beyond `cascadeCount` is not part of the configuration.
    bool cascadeSetEqual(const GiParams &o) const {
        for (int i = 0; i < cascadeCount && i < kGiTierMaxCascades; ++i)
            if (!(cascadeSet[i] == o.cascadeSet[i])) return false;
        return true;
    }
    bool operator!=(const GiParams &o) const { return !(*this == o); }
};

/// WHAT A QUALITY TIER PHYSICALLY IS, IN ONE PLACE (render audit A5, lane
/// CRUD-RENDER-1).
///
/// Every number a Photon quality tier decides lives here and NOWHERE else: the
/// engine reads it (`OgreScene::giVoxelResolution`, `resolveCascadeTable`'s
/// fallback, `buildPcc`'s probe resolution and its HDR/shadow defaults) and so
/// does the HOST, which is the point — the app's five tier tooltips used to
/// describe a renderer that did not exist ("Medium voxelizes at twice the
/// resolution" when both are 64; "32/64/128 voxels per axis" when the cascade
/// chain, which is on at every tier, uses 64/64/128; "128^3 at High/Epic" when
/// two of the four cascades are 64). They are GENERATED from this table now,
/// through `world.tierTable()`, so a tier's description cannot drift from what
/// the tier does.
///
/// WHICH COLUMN OF THE TIER TABLE A VIEW READS (lane V1-RIG item 4,
/// PHOTON_SPEC D2 "VR = world-space caches only ... a VR cascade set with
/// larger outer steps").
///
/// A headset renders the chain FIVE TIMES over: 2160x2376 per eye against a
/// desktop 1080p is 10.26 against 2.07 megapixels, and the budget HALVES (11.1
/// ms at 90 Hz against 16.7 at 60). The cascade chain's price has two halves
/// and only one of them is per pixel:
///
///   THE PIXEL MARCH — measured at Quest Pro size on the rig, one process, one
///   pose, clocks locked (spikes/v1-rig): the opaque pass reads 3.815 ms at
///   four 64^3 cascades, 3.253 at three and 2.568 at two, i.e. **0.31 ms PER EYE
///   PER CASCADE**. It is the term a VR profile can actually buy back.
///
///   THE REBUILD BURSTS — 2.0-2.6 ms of GPU per cascade rebuild, one per frame
///   at most, and since the field's follow moved off the rebuild frame no walk
///   frame crosses 11.1 ms at either tier. So the steps are no longer the
///   binding constraint; halving the OUTERMOST cascade's rate is headroom, not
///   a rescue.
///
/// So the VR column is the tier's own chain with the REDUNDANT MIDDLE cascade
/// dropped and the OUTERMOST STEP DOUBLED. The two halves apply independently:
/// four rows become three at Medium, High and Epic, while LOW KEEPS ITS TWO (it
/// has no middle to drop) — but Low's outer step doubles with everybody else's,
/// so no tier's VR column equals its desktop one. Nothing about a "room", a
/// volume or an axis count enters it: the reach is unchanged, the inner cell is
/// unchanged, and what is given up is one hand-over in the mid field (Medium's
/// 10 m row sits between a 5 m and a 15 m one) and up to twice the off-centring
/// of the outermost cascade — 30 m on Medium's 120 m box at 1.875 m per cell
/// instead of 15, 60 m on Low's 40 m box at 0.625 m per cell instead of 30.
enum class GiViewProfile {
    Desktop = 0,
    Vr      = 1,
};

/// It is a pure function of the quality dial and the view profile: no scene, no
/// device, no Ogre.
/// (THE CARD READ'S FOOTPRINT GATE, kCardFootprintTexels, is DELETED - PHOTON-7 stage 6, law L5: one estimator a
/// quantity. The reflection read a carded surface's VOXELS wherever its lobe's footprint passed four card texels while the
/// gather read the card at every footprint - two estimators of one radiance, chosen by the caller. Its reason was a young
/// view's speckle under the old reflection denoiser; at the gated hits the card read is the nearer to the screen's answer
/// (photon-glass: 4.00 against the voxel's 4.60 codes, spikes/photon-7/stage6/c1/GATE_SPLIT.txt).)

/// THE GATHER ROW OF THE TIER TABLE (PHOTON-GATHER-1d, the rule T-A): what the
/// screen-probe gather is at a tier. One row in the engine's one table; the
/// Studio's tier column is a projection of it (`world.tierTable()`).
///
/// THE RULE: ON AT EVERY TIER (PHOTON-7 stage 7, 7-4: one diffuse route wherever
/// rays run - the opaque pass reads only screen products, L4) - Low at a probe per
/// 32 x 32 pixels with 64 rays a probe, 0.063 rays a pixel at 1080p (2,040
/// probes: Lumen High's uniform density, its DownsampleFactor 32), Medium at 16
/// with 36, High at 16 with 64, Epic at 8 with 64. Why the numbers are these
/// (GATHER-0/1a measured, spikes/gather-1a): PROBE COUNT fills this GPU and rays
/// per probe do not, so the tiers differ in probe density and map resolution,
/// not in a ray budget — 16 pixels a probe at Medium and High, 8 at Epic (four
/// times the probes: the whole block 0.135 ms at High, 0.43 ms at Epic at 1080p);
/// Low's coarser grid keeps the 8x8 map, because 6x6 at 32 would idle 28 of the
/// trace's 64-thread workgroup (rays per probe are one workgroup, probe count is
/// what fills the card); an 8x8 octahedral map (64 rays) at Low, High and Epic,
/// 6x6 (36) at Medium; the adaptive second probes (one a cell at most) capped
/// at HALF the grid at Medium, High and Epic - Lumen's
/// AdaptiveProbeAllocationFraction - and at THE GRID ITSELF at Low (PHOTON-7
/// stage 10, L7: derived from the twins the per-pixel test asks on Bistro
/// Exterior and Interior at 1920 x 1080 - Low 0.58 / 0.66 of its cells, Medium
/// and High 0.46 / 0.49, Epic 0.29 / 0.30; a quarter served a busy scene's
/// slivers on every second frame, the flicker back where scenes are richest).
/// A CAP, not a target: a flat scene spends none of it; the allocation is
/// ~1.46 KB a probe (+3.0 MB at High for the half, +11.8 MB at Epic, +2.2 MB at
/// Low for the grid).
///
/// THE VR COLUMN IS THE DESKTOP'S ROW (PHOTON-GA-VR, decided by arithmetic): a
/// stereo target gathers per EYE (each eye's layer its own grid, each eye through
/// its own basis) with the packed 16 B-a-pixel history, and in a headset the
/// gather is CHEAPER than the diffuse it removes — the cone diffuse is compiled
/// out and the field's cage stands down where a probe answered. Measured at the
/// Quest Pro's 2160x2376 per eye on Monado's simulated HMD, clocks locked
/// 2100-2550 MHz, gather-on and gather-off windows alternated every 24 frames in
/// one process (spikes/photon-ga-vr/EVIDENCE.md): the instanced opaque pass
/// sheds 2.4-2.5 ms an eye on the default scene and 4.7-5.1 on Showroom 2, the
/// gather's four jobs cost 0.5-1.5 ms an eye (3.0 at Epic on Showroom 2, the
/// 4x probes' trace), a net 0.6 / 3.8-4.1 ms an eye BACK at Medium and High and
/// 2.1 at Epic. The field alone (the ddgi door) is 0.27 / 0.50 ms an eye of it.
/// VRAM: 280 MB a headset view at 16 px a probe (the history 164 MB), 381 MB at
/// Epic's 8 — a stereo view holds no rest mean (it never rests).
struct GiGatherFacts {
    /// Pixels per probe on both axes.
    unsigned stride = 16u;
    /// The octahedral map's resolution: the probe traces octRes^2 rays, one per
    /// texel (at most 8 — one ray is one thread of the trace's 8x8 workgroup).
    unsigned octRes = 8u;
    /// The adaptive probes a frame may add, as the uniform grid's count divided
    /// by this (2 = half the grid; 1 = the grid: every asking cell served).
    unsigned adaptiveCapDivisor = 2u;
};

struct GiQualityFacts {
    /// The engine's cascade chain for this tier, innermost first, as
    /// `resolveCascadeTable()` builds it when nothing is pinned. `stepCells` is
    /// left at 0 — the step is DERIVED from the chain (resolveCascadeTable does
    /// it), not a property of the tier.
    GiParams::GiCascadeDesc cascades[kGiTierMaxCascades];
    /// How many entries of `cascades` are in use.
    int   cascadeCount = 0;
    /// One reflection-probe cube face, in pixels, when the scene pins no size.
    unsigned probeFaceSize = 256u;
    /// What `GiToggle::Auto` resolves to for the two expensive probe options.
    bool  probeHdrDefault = false;
    bool  probeShadowsDefault = false;
    // ---- SURFACE-CACHE phase 2 ---------------------------------------------
    /// THE CARD CACHE'S PER-FRAME TEXEL BUDGET at this tier. Lumen's own
    /// capture budget is 512 x 512 = 262,144 texels a frame; ours is sized from
    /// what was MEASURED on this pin, and the measurement is not the one phase
    /// 0 took.
    ///
    /// THE NUMBER THE ROWS ARE SIZED ON (CARD-BUDGET-1, D8): **0.057-0.061 ms a
    /// card** CPU (ATOM-S3-CARDCAP, `sc1b_measure showroom-sun`, a batch of eight:
    /// the card shadow node deleted, the still world's sun term traced at ~0.001
    /// ms of GPU a card). The rows were sized on 0.18-0.20 ms (PHOTON-CARDS-1:
    /// Low 2 cards ~0.4 ms, Medium 3 ~0.6, High 5 ~1.0), so the same milliseconds
    /// now buy about five times the cards — CAPPED BY THE BATCH: one workspace
    /// update carries at most eight cards (`kCaptureBatch`, Ogre's uint8
    /// execution mask), and the drain captures one batch a frame. So Low buys 6
    /// (~0.37 ms), and Medium and High/Epic the batch's 8 — MEASURED 0.50 ms a
    /// capturing frame (`sc1b_measure showroom-sun`, the paired arm, two passes in
    /// one process: 0.506 / 0.491, against 0.326 / 0.322 for the old five and the
    /// 1.0 ms that five were sized on; 0.062 ms a card in the batch, 0.126 alone),
    /// under every old sizing; a frame that wants more waits for the next. Cards
    /// run only where the rays run (High and Epic in the World table).
    unsigned cardBudgetTexels = 131072u;  // the batch: 8 cards a frame ~ 0.50 ms
    /// THE CARDS' DEMAND SETTLE (PHOTON-7 stage 5, 5-2; SurfaceCache's residency by the readers' demand): the READING
    /// frames (frames whose readers' stamps the cache has harvested) after the last change in what the rays can hit
    /// for which new faces may still arrive; then the resident set holds until the next change. THE LAW: a face the
    /// readers hit lambda times a frame (Poisson) arrives - its demand persisting, to take an unread card's page - once
    /// two of its hits fall in different read-backs (every 4 frames); in S reading frames P(fewer than two) =
    /// e^-(lambda S) (1 + lambda S), at most 1e-3 when lambda S >= 9.23. The faces it must catch hold at least ONE HIT A
    /// FRAME - one pixel's reflection ray, or 1.6 % of one probe's 64 gather rays (2.8 % of Medium's 36): a face hit
    /// less is read by less than one pixel or one probe's code - so S = 10 at every tier (S >= two read-backs, 8, too).
    /// (The 120 frames this replaced had no law.) `cardDemandHitFloor` is the law's lambda_min (hits a frame), the
    /// one constant the settle is derived from and its rows bar on (gi.card_residency).
    unsigned cardDemandSettleFrames = 10u;
    float    cardDemandHitFloor = 1.0f;
    /// THE AREA LIGHTS' VISIBILITY ON THE CARDS (PHOTON-7 stage 5, the 5-3 fix): n x n stratified rays a texel over each
    /// area light's rectangle, one a stratum, jittered by the texel's world point (never the frame: L8), the visibility
    /// the RATIO sum( V_i f_i ) / sum( f_i ) of the samples' form-factor weights f_i = cos cos' / r^2 (Heitz et al. 2018)
    /// times the analytic LTC form factor - the unshadowed energy stays exact, only the visibility is sampled. THE LAW:
    /// at a straight penumbra edge a texel's visibility errs by at most the weight of the strata the edge cuts - n of
    /// the n^2 for an edge parallel to a side of the rectangle, 2n - 1 for any - so at even weights 1 / n (n = 8:
    /// 12.5 %) for the parallel edge, (2n - 1) / n^2 for any (23 %), and zero in mean over the band (the jitter is
    /// unbiased). n^2 rays a texel an area light (a point or spot lamp takes one).
    unsigned cardAreaStrata = 8u;
    /// THE LIT CARD'S PER-FRAME BUDGET (PHOTON-CARDS-1, SC-1c), in atlas TEXELS
    /// relit a frame by the `Jahshaka/CardLight` job — a second budget beside the
    /// capture's, because a light's colour or intensity changes every card's
    /// radiance and no card's picture. Lumen's own direct-lighting budget, 1024
    /// square, is the ceiling; the rows are a quarter, an eighth and a sixteenth
    /// of it, NOT YET MEASURED in GPU milliseconds (that needs the clocks locked
    /// — the lead's measurement): a capture's cards are relit the frame they
    /// land, inside this budget, and a light write relights the resident set
    /// over as many frames as it takes.
    unsigned cardLightTexels = 131072u;
    /// THE INDIRECT HALF'S BUDGET, texels a frame (L7: a COUNT PER TIER): the cards' indirect gather (the TLAS rays at the
    /// ray tier, the voxel rays elsewhere) spent on a card's capture, its listing by a move and a region. THE RULE
    /// (PHOTON-7 stage 10, R3): the smallest count whose card-to-card rounds converge a lamp step inside 120 frames, priced
    /// in the card frame's ms from the monitor (the tier rows say the measurement); Lumen's own indirect budget, 512
    /// square, is the ceiling.
    unsigned cardIndirectTexels = 32768u;
    /// THE INDIRECT HALF'S RAYS A TEXEL (PHOTON-7 stage 10, 10-1; L9): the card's indirect is gathered by this many
    /// fixed, world-keyed, cosine-distributed occupancy rays through the voxel chain (jah_voxel_ray.glsl) - a hit reads
    /// the voxel's side facing the ray, an escape the sky over the ray's bin, an exhausted ray is no sample.
    unsigned cardIndirectRays = 16u;
    /// THE BOUNCE'S RAYS A VOXEL SIDE (PHOTON-7 stage 10, 10-1; L9): the feedback's passes gather each side of a surface
    /// voxel by this many occupancy rays from the side's own surface position - the same estimator as the cards'.
    unsigned voxelBounceRays = 16u;
    /// THE VOXEL LIGHT'S WORK A FRAME (PHOTON-7 stage 4's hitch fix; L7: a budget is a count): a lamp's or the sun's
    /// relight of the voxels' direct term and the feedback's bounce passes each spend, every frame, the chain's whole
    /// work over THIS MANY FRAMES - the relight `chain voxels / lightWorkFrames` voxels, the feedback `chain bricks /
    /// lightWorkFrames` brick-passes (8^3 bricks), the outermost cascade first (an inner cascade's light rests on the
    /// outer ones'); what does not fit is carried, in a deterministic order, to the next frame. The physics allows the
    /// spread: the voxels' direct term feeds only the bounce (the screen's direct light is the forward pass, at once), so
    /// a lamp's indirect catching up over frames is not a pop, where one frame doing it all is a hitch. MEASURED on Bistro
    /// Interior at Epic (a 8 -> 12 lamp step, every brick of the four cascades in its 40 m range; 4080 SUPER, clocks
    /// unlocked): the whole relight 8.94 ms and the whole first pass 11.0 ms in one frame against a 6.93 ms median
    /// (2.49x); spread, a DENSE brick-pass costs up to ~6 us (1,536 of them read 9.0 ms in one frame) and a relit voxel
    /// up to ~4.5 ns (cascade 2), so 18 frames - 512 brick-passes (~3 ms) and 262k voxels (~1.2 ms) at Epic - keeps a
    /// step frame near 1.5x the median. One number for every tier: each pays the same share of its own chain a frame,
    /// so every tier settles a lamp step in the same frames. THE FLOORS (PHOTON-7 stage 5's read A2: under a SUSTAINED
    /// change the outermost-first spend gave the outer cascades the whole count every frame and starved the inner ones):
    /// every cascade that owes work takes, every frame, a FLOOR of HALF its own share of each count - of the relight's,
    /// its voxels over the chain's, accumulated across frames until it pays a 4-plane slab; of the feedback's
    /// brick-passes, its bricks over the chain's - and only the rest is spent outermost first. THE LAW: an owed
    /// cascade's floor alone pays its whole volume (its active bricks) in 2 x lightWorkFrames frames, whatever the rest
    /// of the chain owes; and a change that arrives during a relight's round never restarts it - it is owed to the next
    /// round.
    unsigned lightWorkFrames = 18u;
    // ---- THE GATHER ROW (PHOTON-GATHER-1d) -----------------------------------
    /// The screen-probe gather at this tier (GiGatherFacts says what and why).
    GiGatherFacts gather;
    // ---- THE REFLECTION ROW (PHOTON-F12-PCC) ---------------------------------
    /// THIS TIER'S REFLECTIONS ARE TRACED, so it builds NO reflection-probe
    /// grid wherever the machine traces (the rule: `OgreScene::probeGridByRays`
    /// — this row AND `rayTracingResolved()`, the gather's own shape). The three
    /// sources are the answer there: the screen march, the rays (a hit lit from
    /// its card, the decode or the voxels) and the anisotropic cone with the sky
    /// as its escape; a planar mirror stays a planar mirror. True at High (and
    /// Epic, which reads High's rows); Low and Medium keep the grid wherever a
    /// technique asks for one. A scene that turns its rays OFF is not a ray tier
    /// for this rule and keeps its grid.
    ///
    /// WHAT IT DELETES, measured on Grand Showroom 2 at Epic (32 probes kept at
    /// 512 px HDR, app.textureMemory A/B): 838,987,760 bytes of texture — the
    /// probe array 536,739,840, its capture and IBL cubes 33,550,320, each probe
    /// workspace's shadow node and capture depth 268,435,456 (7 + 1 MiB a probe at
    /// the High shadow settings — PCC-BUDGET-2, giProbeGridBytes) and the placement's
    /// 256 px depth buffer 262,144 — and the
    /// open's placement, 713-799 ms of the UI thread.
    bool rayReflections = false;
    /// THE TRACE'S RESOLUTION WHERE THE RAYS ARE THE REFLECTION (D4-PHOTON-TIERS,
    /// the SSR row's meaning at a ray tier): 1 = one reflection ray per 2x2 block
    /// (High), 2 = every pixel (Epic), 0 where `rayReflections` is false. At a
    /// ray tier the view's reflection trace reads THIS, never the document's SSR
    /// row — the rays replace the screen march as the reflection, and the row
    /// reads "traced" (worldmodes). The values are the SSR row's own High/Epic
    /// columns, so the shipped pictures do not move.
    int  reflectTrace = 0;
    /// THE REST'S RAYS A FRAME (PHOTON-7 stage 6 commit 8, PREFLIGHT_R section 3): at rest a texel still converging
    /// traces this many of its rest sequence's samples a frame (the batch: up to the first that needs a record) - the
    /// rest converges in about (2N + its measured samples) / k + 8 frames, then holds. 4 where the rays are the
    /// reflection (the preflight's simulated stop: ~100 frames on photon-glass against the deleted K_max's 513), 1 else.
    unsigned reflectRestRays = 1u;
    /// THE EMITTER TABLE'S CAP (PHOTON-7 stage 11, 11-1a/d): at most this many emissive traced instances are the
    /// reflection's NEE targets (the brightest by power, the bounding sphere's cross-section x its radiance; the rest
    /// stay the BRDF sample's alone and are counted as RayQueryStatus::reflectEmittersOver, never silently). A
    /// count, not milliseconds (L7): each NEE sample's pick walks the table once. 0 where the rays are not the
    /// reflection.
    unsigned reflectEmitters = 0u;
    /// THE IRRADIANCE FIELD, what `GiParams::ddgi`'s Auto resolves to: ON at every
    /// quality (owner option (b), 2026-09-09 — the field is the one diffuse arm
    /// that is right in open and sealed scenes alike). The Studio tier table's
    /// field column is READ from here (worldmodes::photonDdgi).
    bool fieldDefault = true;
    /// THE REFLECTION-PROBE GRID'S VRAM BUDGET (PCC-BUDGET-1), in bytes, for the
    /// WHOLE grid: the probe array (6 faces x mips per probe), each probe's
    /// shadow targets when its captures are shadowed, and the grid's fixed
    /// capture + IBL cubes (`giProbeGridBytes`). The probe COUNT is derived from
    /// it (`giProbeGridBudgetCount`), never the other way round: a placement that
    /// keeps more probes than fit keeps the ones that see the most geometry and
    /// logs the cut. Only where a grid is built at all — a tier whose reflections
    /// are traced on this machine builds none (`rayReflections`).
    /// MEASURED (PHOTON-F12-PCC, Grand Showroom 2 at Epic): 32 probes at 512 px
    /// HDR and shadowed = 838,987,760 B (array 536,739,840 + shadow targets
    /// 268,435,456 + cubes 33,550,320 + the placement's depth 262,144), i.e.
    /// 25.2 MB a probe plus 33.8 MB fixed — the arithmetic below reproduces it to
    /// the byte from its real terms (PCC-BUDGET-2: the node's textures at the live
    /// shadow settings and each capture's depth buffer). The budgets: Epic 1 GiB
    /// (41 probes at its 512 px HDR shadowed — the measured 32-probe Showroom grid
    /// fits), High 512 MiB (19), Medium 128 MiB (55 at 256 px, unshadowed), Low
    /// 64 MiB (111 at 128 px) — the two unshadowed rows 61 and 125 while the
    /// capture's depth went uncounted. PROVISIONAL: the
    /// per-tier numbers are the lead's to confirm; the arithmetic is not.
    unsigned long long probeGridBudgetBytes = 128ull << 20;
};

/// THE BYTES A REFLECTION-PROBE GRID HOLDS (PCC-BUDGET-1): `probes` probes at
/// `faceSize` px, RGBA16F when `hdr` else RGBA8, each with its capture's own
/// `faceSize`^2 D32 depth buffer (one per probe workspace, shadowed or not) and
/// `shadowBytes` of shadow targets (0 when its captures are unshadowed), plus the
/// grid's capture and IBL cubes and the placement's 256 px depth. A probe's array
/// slice carries its mips down to 16x16 (the pin's IBL chain); the two cubes
/// carry theirs to 1x1 between them (85 more texels a face).
///
/// THE SHADOW TERM IS THE NODE'S REAL TEXTURES (PCC-BUDGET-2), handed in by the
/// engine that owns them (OgreEngine::probeShadowNodeBytes: the probe node's
/// atlas at its live resolution and focused-map count, plus its point-light
/// scratch cube) — a constant 8 MiB stood here, which reproduced one fixture
/// (Showroom 2 at the High shadow settings) and no other: it was the node's 7 MiB
/// (a 512 x 2816 D32 atlas with four focused maps + a 256^2 x 6 R32F cube) PLUS
/// the capture's own 1 MiB depth buffer, which every probe holds shadowed or not
/// and which the constant hid (measured on Showroom 2, app.textureMemory by size:
/// 32 x each of the three; with the probes' shadows off only the depth stays).
/// The node is 3.5 MiB (two focused maps, no cube) to 7 MiB at a 2048 atlas.
/// Showroom 2 at Epic: 32 x (16,773,120 + 1,048,576 + 7,340,032) + 33,812,464 =
/// 838,987,760 B — the F12-PCC measurement, now from its real terms.
inline unsigned long long giProbeGridBytes(unsigned faceSize, bool hdr,
                                           unsigned long long shadowBytes, unsigned probes)
{
    const unsigned long long bpp = hdr ? 8ull : 4ull;
    unsigned long long texels = 0ull;
    for (unsigned r = faceSize; r >= 16u; r >>= 1u) texels += (unsigned long long)r * r;
    const unsigned long long slice = 6ull * texels * bpp;
    const unsigned long long captureDepth = 4ull * faceSize * faceSize;
    const unsigned long long fixed = 2ull * slice + 6ull * bpp * 85ull + 262144ull;
    return fixed + (unsigned long long)probes * (slice + captureDepth + shadowBytes);
}

/// How many probes a grid at this face size / format / shadowing may keep under
/// `budget` bytes (at least 1: a budget smaller than one probe still keeps the
/// probe that sees the most, rather than silently building nothing).
inline unsigned giProbeGridBudgetCount(unsigned long long budget, unsigned faceSize, bool hdr,
                                       unsigned long long shadowBytes)
{
    const unsigned long long fixed = giProbeGridBytes(faceSize, hdr, shadowBytes, 0u);
    const unsigned long long each = giProbeGridBytes(faceSize, hdr, shadowBytes, 1u) - fixed;
    if (budget <= fixed || !each) return 1u;
    return unsigned(std::max<unsigned long long>(1ull, (budget - fixed) / each));
}

/// THE TIER TABLE. Hand-edit this and every reader — engine and app — moves
/// with it. `profile` picks the column (see GiViewProfile for the measurement
/// behind the VR one).
/// Epic is High's rows plus the gather's density (below).
inline GiQualityFacts giQualityFacts(GiQuality quality,
                                     GiViewProfile profile = GiViewProfile::Desktop)
{
    GiQualityFacts f;
    switch (quality) {
    case GiQuality::Low:
        // LOW IS 64^3 IN THE CHAIN, NOT THE QUALITY DIAL'S 32 (PHOTON_SPEC §7
        // E2 (4)): the number that decides whether its bounce means anything is
        // the CELL, and at 32 the inner cascade's cell is 0.31 m, which smears
        // a room's own walls. TWO cascades, because reach is what stops a
        // corridor going black and the far one is the cheap one.
        f.cascades[0] = {  5.0f, 64, 0.0f };
        f.cascades[1] = { 20.0f, 64, 0.0f };
        f.cascadeCount = 2;
        f.probeFaceSize   = 128u;
        f.cardBudgetTexels = 98304u;    // 6 cards a frame ~ 0.37 ms (sized 0.4)
        f.cardLightTexels = 65536u;     // 4 pages a frame
        f.cardIndirectTexels = 32768u;  // 2 pages a frame (DOUBLED with every tier, PHOTON-7 stage 10-3: derived, see High's)
        f.cardAreaStrata = 4u;          // 16 rays a texel an area light: 1 / 4 at a parallel edge
        // 64 rays a probe, a probe per 32x32 (7-4: Lumen High's density); the twins capped at THE GRID (GiGatherFacts)
        f.gather = { 32u, 8u, 1u };
        f.probeGridBudgetBytes = 64ull << 20;
        break;
    case GiQuality::High:
    case GiQuality::Epic:
        f.cascades[0] = {  5.0f, 128, 0.0f };
        f.cascades[1] = { 10.0f, 128, 0.0f };
        f.cascades[2] = { 15.0f,  64, 0.0f };
        f.cascades[3] = { 60.0f,  64, 0.0f };
        f.cascadeCount = 4;
        f.probeFaceSize   = 512u;
        // The two expensive probe options are ON at this tier and only here
        // (REFLECTIONS_ADOPTION_SPEC P3a/P3b) — the pair `GiToggle::Auto` reads.
        f.probeHdrDefault     = true;
        f.probeShadowsDefault = true;
        f.cardBudgetTexels = 131072u;   // the batch's 8 cards a frame ~ 0.50 ms (sized 1.0)
        f.cardLightTexels = 262144u;    // 16 pages a frame (Lumen's 1024^2 / 4)
        // 16 pages a frame (PHOTON-7 stage 10-3, the lead's ruling: DOUBLED at every tier). THE LAW: after a STOP the
        // card-to-card rounds converge geometrically (about x0.5 a round, a 1-2 code tail; no pair, no gain) over the
        // listed set at this budget's pace, a round of 130-190 cards ~15-20 frames at 8 pages - so the frames to rest
        // after a stop are the count's to set. MEASURED on two scenes, F3 + the fit, one run each (spikes/photon-7/
        // stage10/10-3/pace-price): Bistro Exterior (Epic, 1920x1080) rests 330 / 196 / 153 frames after a stop at 8 / 16
        // / 32 pages, the round frame's worst cards.light + cards.gather 5.5 / 6.2 / 9.4 ms; the Mirror Room 217 / 178 /
        // 158 frames, 3.6 / 5.5 / 10.2 ms (its GPU p95 9.7 / 10.0 / 11.6). 16, not 32: the first doubling buys most of
        // the gain for +1-2 ms on the worst frame; 32 puts one round frame at a VR frame's whole 11.1 ms. (The earlier
        // rule, a lamp step's rounds inside 120 frames: 8 held photon-room's step at 119.) Medium and Low are DERIVED by
        // the same doubling (2 -> 4, 1 -> 2 pages), not measured: re-measured on two scenes in the optimisation round.
        f.cardIndirectTexels = 262144u;
        f.gather = { 16u, 8u, 2u };   // 64 rays a probe, a probe per 16x16; the twins capped at half the grid
        f.rayReflections = true;        // no probe grid where the machine traces (F12-PCC)
        f.reflectTrace = quality == GiQuality::Epic ? 2 : 1;   // the SSR row's High/Epic columns
        f.reflectRestRays = 4u;
        // THE EMITTER CAP (PHOTON-7 11-1d), MEASURED on two scenes (spikes/photon-7/stage11/11-1d): Bistro Exterior's table
        // holds 59 emissive traced instances, Bistro Interior's 18 - 64 keeps every one of both (none over). Its price:
        // each NEE sample walks the table three times (W, the pick, p_l of the drawn direction); at 59 targets the
        // trace moved 0.230 -> 0.251 ms (1080p Epic, ABBA, Bistro Exterior's walk). Past 64 the dimmest by power are
        // counted over (reflectEmittersOver) and stay the BRDF's alone - many emitters is the many-lights problem whose
        // general answer (reservoirs) is out of this stage.
        f.reflectEmitters = 64u;
        f.probeGridBudgetBytes = quality == GiQuality::Epic ? (1024ull << 20) : (512ull << 20);
        break;
    default:   // Medium: the same reach as High, at its own resolution
        f.cascades[0] = {  5.0f, 64, 0.0f };
        f.cascades[1] = { 10.0f, 64, 0.0f };
        f.cascades[2] = { 15.0f, 64, 0.0f };
        f.cascades[3] = { 60.0f, 64, 0.0f };
        f.cascadeCount = 4;
        f.probeFaceSize   = 256u;
        f.cardBudgetTexels = 131072u;   // the batch's 8 cards a frame ~ 0.50 ms (sized 0.6)
        f.cardLightTexels = 131072u;    // 8 pages a frame
        f.cardIndirectTexels = 65536u;  // 4 pages a frame (DOUBLED with every tier, PHOTON-7 stage 10-3: derived, see High's)
        f.cardAreaStrata = 6u;          // 36 rays a texel an area light: 1 / 6 at a parallel edge
        f.gather = { 16u, 6u, 2u };   // 36 rays a probe (T-A); the twins capped at half the grid
        f.probeGridBudgetBytes = 128ull << 20;
        break;
    }
    // ---- THE EPIC TIER'S GATHER: FOUR TIMES THE PROBES -----------------------
    // Keyed on the TIER, never on the view's SSR row (GA-TIERROW): the SSR row
    // is the reflections' own and stays what it is.
    if (quality == GiQuality::Epic) f.gather.stride = 8u;
    // ---- THE VR COLUMN (GiViewProfile, above) ------------------------------
    // ONE transform over the desktop rows, so the two columns cannot drift: the
    // middle cascade goes and the outermost steps twice as far. `stepCells` on
    // a row means "pinned"; the engine derives the rest.
    if (profile == GiViewProfile::Vr) {
        // Both halves are independent, and LOW GETS ONLY THE SECOND: with two
        // rows there is no middle to drop (corrected 2026-09-18 — the first
        // note claimed Low was untouched, which the doubled outer step makes
        // false).
        if (f.cascadeCount >= 4) {
            // Drop index 1 — the row closest in reach to the one outside it
            // (Medium 5/10/15/60, High 5/10/15/60 at its own resolutions), so
            // the near field and the far reach are both untouched.
            for (int i = 1; i + 1 < f.cascadeCount; ++i) f.cascades[i] = f.cascades[i + 1];
            --f.cascadeCount;
        }
        if (f.cascadeCount > 0) {
            // The engine's own outer default is 8 cells (OgreGi.cpp
            // kOuterStepCells); doubling it halves the most expensive rebuild
            // in the chain. Pinned here rather than in the engine so the table
            // is the one place a tier's physics lives.
            f.cascades[f.cascadeCount - 1].stepCells = 16.0f;
        }
        // ...and the card budget halves, for the reason every VR row exists:
        // the frame is drawn twice and its budget is 11 ms, not 16.
        f.cardBudgetTexels /= 2u;
        // ...WITH A FLOOR OF ONE WHOLE PAGE. A budget under 16,384 texels
        // cannot pay for a single 128-texel card, and the drain lets the first
        // card of a frame through unconditionally (a budget that could never
        // buy anything would be a queue that never moves) — so a smaller number
        // would not be a smaller budget, it would be a budget the code has to
        // ignore. No shipped row reaches it since CARD-BUDGET-1 (Low's VR row
        // is three cards); a host's override can.
        f.cardBudgetTexels = std::max(f.cardBudgetTexels, 16384u);
        // ...and the relight budget with it, on the same floor for the same reason.
        f.cardLightTexels = std::max(f.cardLightTexels / 2u, 16384u);
        f.cardIndirectTexels = std::max(f.cardIndirectTexels / 2u, 16384u);
        // ...and the GATHER IS THE DESKTOP'S ROW (GiGatherFacts: PHOTON-GA-VR's
        // arithmetic — cheaper per eye than the cone and field diffuse it removes
        // at every tier that has it, Epic's four times the probes included).
    }
    return f;
}

/// One cascade's CELL in metres: the number that says what it can resolve.
inline float giCascadeCell(const GiParams::GiCascadeDesc &c)
{
    return c.resolution > 0 ? c.halfSize * 2.0f / float(c.resolution) : 0.0f;
}

// ---- THE NEAR-FIELD GUARANTEE (CASCADE-STEP-1, owner 2026-09-18) -----------
//
// THE RULE, stated before the arithmetic: THE INNERMOST CASCADE GUARANTEES A
// NEAR-FIELD RADIUS. Within `kGiNearFieldRadiusFraction` of its own half-size
// around the head, the near field is always VOXELISED BY THAT CASCADE — its box
// covers that radius at every moment of any walk, so the bounce a walker stands
// in is built at its cell size and never at the coarser cascade's behind it.
//
// It is NOT a claim about every cone sample. A cone aimed outward leaves the
// box and hands over to the coarser cascade by construction, carrying its age
// across the hop (SEAM-1, fork 8f09c0cd4 (was 0066)), which is what a cone aimed outward
// should do. What the rule removes is the NEAR FIELD ITSELF changing resolution
// as the walker moves — the hand-over arriving at the wearer's feet.
//
// WHY IT NEEDS A RULE, AND WHAT THE BOUND HONESTLY IS.
//
// The re-centre test runs on an ABSOLUTE lattice: the planes sit every
// step*(1-h) metres of world space (h = kStepHysteresis) and the camera must be
// h*step past the plane it left, so a re-centre comes anywhere between h*step
// and step of travel from the camera the cascade was built for. `step` is the
// SUPREMUM of that travel, never the distance between two rebuilds. Three more
// terms sit on top of it:
//
//   * the re-centre QUANTISES the new centre onto the cascade's own cell
//     lattice, which leaves it up to one cell below the head on each axis;
//   * the test is a per-FRAME test — it fires on the first frame at which the
//     camera has already crossed, so the head is up to one frame of travel
//     (v*dt) past the threshold when the rebuild happens;
//   * and a rebuild can be DEFERRED: the frame spends at most one cascade, and
//     a field follow can own that slot, so each deferred frame is another v*dt.
//
//     off  =  step + cell + v*dt*(1 + deferrals)                 (per axis)
//     r    =  halfSize - off                                     (the inscribed radius)
//
// The engine cannot know v, so the motion terms are bought with ONE CELL OF
// SLACK. The STEP is chosen from
//
//     halfSize - step - 2*cell  >=  kGiNearFieldRadiusFraction * halfSize
//
// while `giCascadeGuaranteedRadius` reports the honest, motion-free
// `halfSize - step - cell`: the reported radius therefore clears the required
// one by about a cell, and that cell is the motion budget.
//
// WHAT THE SLACK BUYS, IN FRAMES — the number that makes this a walking-pace
// guarantee and says so. At 1.4 m/s on the 60 Hz clock a frame is 0.0233 m:
// Medium's margin (2.500 - 2.250 = 0.250 m) is ten frames and High's
// (2.344 - 2.250 = 0.094 m) is four, which covers the frame the test costs plus
// a follow or two owning the slot. At the editor's FLY speed of 15 m/s a frame
// is 0.25 m: Medium spends its whole margin on a single frame and High is
// 0.16 m inside the stated radius while the camera is still moving. THE
// GUARANTEE IS FOR WALKING PACE, deliberately — a flight is a camera in
// transit, and the only thing a cascade hand-over has to be invisible under is
// a person moving at a person's speed. (Both numbers are measured in
// spikes/cascade-step-1/MEASUREMENTS.txt; gi.cascades case 16b walks at 1.4 m/s
// from a NON-lattice start so the coupled worst phase is in the measurement.)
//
// The hysteresis costs no radius at all: kStepHysteresis shrinks the test
// lattice by the band and adds the band back on both sides (OgreGi.cpp), so the
// supremum stays exactly `step` and not `(1 + h) * step` — measured, 2.637 m of
// worst offset against a 2.656 m step before the slack landed.
//
// WHAT IT WAS BEFORE. The derived step was the pin's "every cascade steps the
// same distance", floored at half the resolution — and that floor BOUND on the
// innermost cascade at every tier but Low: 32 cells of a 64^3 5 m cascade is a
// 5 m step, i.e. step == halfSize, i.e. r = -cell. NOTHING was guaranteed: a
// wearer walking a straight line reached cascade 0's own face before it
// re-centred, and the metre in front of their eyes was read from cascade 1 —
// 3x coarser in the VR column (0.156 m -> 0.469 m). That is the artifact
// CASCADE-STEP-1 removes; the owner's acceptance is "we should never notice a
// change when walking around a scene".
//
// THE FRACTION is 0.45 of the half-size: 2.25 m of the 5 m inner cascade every
// tier ships, about one and a half paces ahead of the walker and more than the
// 2 m a room's near wall usually stands at. It is bought with rebuild
// frequency, linearly — the step falls from halfSize to a little under half of
// it, so cascade 0 re-voxelises about twice as often per metre walked, which is
// +2 ms of GPU per metre and no frame's peak worth naming (MEASUREMENTS.txt).
//
// THE TABLE THE RULE PRODUCES (r = guaranteed / required, metres; every row of
// every tier and both columns clears it, which gi.cascades case 16a asserts):
//
//   low    desktop  c0  2.344 m  r 2.500/2.250 | c1  5.000  r 14.375/9.000
//   low    vr       c0  2.344    r 2.500/2.250 | c1 10.000  r  9.375/9.000 (pinned)
//   medium desktop  c0  2.344    r 2.500/2.250 | c1  4.688  r  5.000/4.500
//                   c2  7.031    r 7.500/6.750 | c3 15.000  r 43.125/27.000
//   medium vr       c0  2.344    r 2.500/2.250 | c1  7.031  r  7.500/6.750
//                   c2 30.000    r 28.125/27.000 (pinned)
//   high   desktop  c0  2.578    r 2.344/2.250 | c1  5.156  r  4.688/4.500
//                   c2  7.031    r 7.500/6.750 | c3 15.000  r 43.125/27.000
//   high   vr       c0  2.578    r 2.344/2.250 | c1  7.031  r  7.500/6.750
//                   c2 30.000    r 28.125/27.000 (pinned)
//   (epic is high's table.)
//
// In cells, against what the derivation gave before this rule: Low c0 16 -> 15;
// Medium c0 32 -> 15, c1 24 -> 15, c2 16 -> 15; High c0 64 -> 33, c1 48 -> 33,
// c2 16 -> 15; every outermost cascade and every pinned step unchanged.
inline constexpr float kGiNearFieldRadiusFraction = 0.45f;

/// THE RESOLUTION A CASCADE IS BUILT AT (NONDET-1 D1): a pinned row's (cascadeSet) clamped to 16..256 and ROUNDED UP
/// to a multiple of 8. Every voxel pass runs in whole groups with no bound check: the bounce and the brick copy in
/// 8^3 bricks, the injection in 4^3 groups. jahHalfTexel (jah_voxel_halves.glsl) maps a cell at x >= W into the next
/// half's block of the half store, in bounds, so a cell past the edge overwrote another half's direct term and total.
/// The direction volumes' y and z are h >> 1, so an odd resolution also dropped the last row. Up, never down: a
/// cascade is never coarser than its row asked for. Every tier row is already a multiple of 8.
inline int giCascadeResolution(int requested)
{
    const int clamped = requested < 16 ? 16 : (requested > 256 ? 256 : requested);
    return (clamped + 7) / 8 * 8;
}

/// The near-field radius this cascade is REQUIRED to guarantee, in metres.
inline float giNearFieldRadius(const GiParams::GiCascadeDesc &c)
{
    return kGiNearFieldRadiusFraction * c.halfSize;
}

/// The near-field radius this cascade DOES guarantee at the step it carries:
/// `halfSize - step - cell`, in metres — the STILL-CAMERA bound, with the
/// re-centre's supremum travel and the centre's cell quantisation in it and the
/// motion terms (v*dt per frame the test costs and per frame a rebuild waits)
/// deliberately NOT in it, because the engine cannot know v. The step is chosen
/// with a cell of slack against the required radius (giNearFieldMaxStepCells)
/// and that cell is the motion budget: at 1.4 m/s it is four to ten frames.
/// Negative means the cascade guarantees nothing at all — the head can be
/// outside the box before it re-centres. Meaningless on a row whose step is
/// still 0 (not yet resolved).
inline float giCascadeGuaranteedRadius(const GiParams::GiCascadeDesc &c)
{
    const float cell = giCascadeCell(c);
    return c.halfSize - c.stepCells * cell - cell;
}

/// THE HIT READ'S LEVEL (PHOTON-7 stage 10, 10-2e; the lead's ruling): a TLAS hit reads cascade c only within its
/// LEVEL RADIUS of the GI camera (max-norm) - its guaranteed radius (above: covered by its box at every placement it can
/// reach) less a MARGIN of M of its cells, the bounce's reach (an edge difference spreads inward attenuated by albedo per
/// bounce) - blending out over the hand-over band below it; the outermost by its box. M is MEASURED: the fewest cells at
/// which gi.field_oracle_shells (6b) - two placements at the reachable extremes, the bounce on - and gi.card_residency
/// (3) - walk = teleport - both read within a code (the measuring door gi.hitLevelMargin sets it; -1 = this default).
/// THE PRICE per level: the usable range lost, step + cell + M cells off the half size.
constexpr float kGiHitLevelMarginCells = 2.0f;   // MEASURED (10-2e): the fewest cells within a code - see below
/// THE PLACEMENT QUANTUM (PHOTON-7 stage 10, 10-2e; the lead's ruling - the clipmap law): a cascade's centre sits on a
/// lattice of Q = 2^K of its cells and its re-centre step IS Q, so every mip level a reader touches up to K stays
/// WORLD-ANCHORED at every placement the cascade can reach (a level's texel is 2^L cells; the directional volumes'
/// level 0 is two). Q is the largest power of two not above the derived step plus one cell (High's 33 -> 32, 15 -> 16,
/// 8 -> 8): the step within a cell of the rule's. Each cascade is anchored on its OWN lattice, which is all a reader of
/// that cascade needs - the cell ratios between cascades (c1 -> c2 is 3x at High) need no alignment across them.
inline float giPlacementQuantumCells(float stepCells)
{
    float q = 2.0f;
    while (q * 2.0f <= stepCells + 1.0f) q *= 2.0f;
    return q;
}
constexpr float kGiHitLevelBandCells = 6.0f;
inline float giHitLevelMarginCells(double armValue) { return armValue >= 0.0 ? float(armValue) : kGiHitLevelMarginCells; }
/// THE STEP TEST'S BAND (PhotonGi.cpp's planCascades): a cascade re-centres once the camera is more than (0.5 + h) of
/// its step off its lattice centre on an axis - so the furthest a reachable placement holds the camera off-centre.
constexpr float kGiStepHysteresis = 0.1f;
/// THE LEVEL RADIUS: the half size less the furthest a REACHABLE placement holds the camera off-centre ((0.5 + h) of the
/// step: the lattice's step test), a cell and the margin - a point inside it is inside every box this cascade can stand
/// at for this camera. MEASURED at the placement lattice (gi.field_oracle_shells (6b), the camera at the band's edge -
/// 0.59 of a step off one centre - and gi.card_residency (3), walk = teleport): M = 0 cells, the store 1.6 codes apart
/// (57 channels over one); M = 2, 4, 8: 0.000, and (3) within 0.12 / 0.07 / 0.24 codes - so M = 2 (the bounce's reach
/// past the reachable boxes' common interior: an edge difference spreading two cells in, under a code past them).
inline float giHitLevelRadius(const GiParams::GiCascadeDesc &c, float marginCells)
{
    const float cell = giCascadeCell(c);
    return c.halfSize - (0.5f + kGiStepHysteresis) * c.stepCells * cell - cell - marginCells * cell;
}

/// THE WALKING PACE a scroll is sized for: 1.4 m/s at the engine's fixed 1/60 s frame.
inline constexpr float kGiWalkMetresPerFrame = 1.4f / 60.0f;
/// THE FASTEST CAMERA A SCROLL'S SCRATCH IS SIZED FOR (PHOTON-7 stage 2): 0.25 m a frame (15 m/s, a fly). A cascade's
/// step test fires up to one frame's travel after the camera crossed its band, so a scroll's shift is at most a step,
/// the even placement's cell and a frame of travel: the scratch holds that at this speed. A faster camera is carried by
/// CUT scrolls (each leaves its remainder owed) or, past a whole box, a jump's whole rebuild.
inline constexpr float kGiFlyMetresPerFrame = 0.25f;

/// THE SCROLL'S SPREAD AS A COUNT (PHOTON-7 stage 2; PREFLIGHT_1_2 §5.6): the chunk frames a scroll of this cascade
/// may spread its entering slab over before the frame that commits it - as long as the walker stays inside the radius
/// the cascade must guarantee: its MOTION SLACK (guaranteed - required radius) walked at 1.4 m/s, less the commit's
/// frame - and the chunk frames and the commit's within one STEP at that pace (the next step must not arrive before
/// the commit). At least one. At the step a cascade places by (its placement quantum, giPlacementQuantumCells):
/// Epic / High 6 / 13 / 11 / 641, Medium 3 / 7 / 11 / 641, Low 3 / 213 (gi.scroll_lag prints them from each tier's
/// resolved chain; at the derived steps before 10-2e they were 3 / 7 / 31 / 641, 9 / 20 / 31 / 641, 9 / 213). It replaces a per-frame millisecond budget (16 ms, a clock): the
/// spread is a function of the table and the walk, never of the card.
inline int giCascadeScrollFrames(const GiParams::GiCascadeDesc &c)
{
    const float slack = giCascadeGuaranteedRadius(c) - giNearFieldRadius(c);
    const int bySlack = int(std::floor(slack / kGiWalkMetresPerFrame)) - 1;
    const int byStep = int(std::floor(c.stepCells * giCascadeCell(c) / kGiWalkMetresPerFrame)) - 1;
    return std::max(1, std::min(bySlack, byStep));
}

/// The most cells a cascade may step and still honour the rule, floored at one
/// cell (a step below one cell re-centres the volume for a fraction of a voxel)
/// and ceiled by the pin's own guard at half the resolution.
///
/// TWO cells are held back, not one: the first is the re-centre's own
/// quantisation (it is in the guaranteed radius) and the second is the MOTION
/// SLACK — the frame the per-frame test costs and the frames a deferred rebuild
/// waits, which the engine cannot price because it does not know the camera's
/// speed. One cell is 0.156 m at Medium and 0.078 m at High: ten and four
/// frames of walking, against a bare margin of 0.016 m at High without it,
/// which is less than ONE frame at 1.4 m/s.
inline float giNearFieldMaxStepCells(const GiParams::GiCascadeDesc &c)
{
    const float cell = giCascadeCell(c);
    if (cell <= 0.0f) return 1.0f;
    const float metres = c.halfSize - giNearFieldRadius(c) - 2.0f * cell;
    return std::max(1.0f, std::min(std::floor(metres / cell), float(c.resolution) * 0.5f));
}

/// RESOLVE THE DERIVED STEPS OF A WHOLE CHAIN, innermost first — the one place
/// the step of a cascade nobody pinned is decided, so the renderer's chain and
/// the chain a tooltip (world.tierTable) promises cannot drift.
///
/// A row that already carries a step (`stepCells > 0`) is PINNED and is left
/// exactly as it is: a pinned step is the author's own statement and is held
/// only by the pin's 1..resolution/2 guard, which the caller applies. Our own
/// tier tables pin only the outermost cascade (the VR column's 16 cells) and
/// they are checked against the rule by gi.cascades case 16a, not clamped
/// here.
///
/// Every other row gets upstream's `VctCascadedVoxelizer::autoCalculateStepSizes(4)`
/// shape (that class is not in our fork since ATOM-VOXEL-2) written out here so it is ours to
/// tune (A7) — every finer cascade steps the same DISTANCE as the outermost
/// one, ceiled to whole cells and floored at half its resolution (the pin's own
/// guard against a step that outruns the volume) — MET WITH the near-field rule
/// above, which is a ceiling on it.
///
/// THE OUTERMOST CASCADE STEPS TWICE AS FAR AS THE REST (PHOTON_SPEC §7 E2 (1),
/// "the outer stepCells raised"), and the reason is a measurement, not symmetry.
/// The outermost cascade is the one that encloses the most geometry and resolves
/// the least, so it is BY FAR the most expensive rebuild in the chain — on the
/// 8,026-instance lattice it is 88.9 ms of GPU against cascade 0's 18.1, and
/// even on the Showroom at Epic it is the row that peaks
/// (spikes/photon-e2/BASELINE.md). Halving how often it runs halves that cost,
/// and what it buys with the frames it skips is that its 60 m box sits up to
/// 15 m off-centre instead of 7.5 — on a volume 120 m across, at 1.875 m per
/// cell, which is a quarter of a cell of parallax on the far bounce. It still
/// clears the near-field rule by a wide margin (43.1 m guaranteed against the
/// 27.0 asked of it), which is why the ceiling never bites there.
inline void giResolveCascadeSteps(GiParams::GiCascadeDesc *rows, int count)
{
    if (!rows || count <= 0) return;
    static const float kOuterStepCells = 8.0f;   // the pin's own value is 4
    static const float kInnerStepCells = 4.0f;
    const float cellLast = giCascadeCell(rows[count - 1]);
    for (int i = 0; i < count; ++i) {
        if (rows[i].stepCells > 0.0f) continue;          // pinned: not ours to decide
        const float cell = giCascadeCell(rows[i]);
        if (cell <= 0.0f) continue;
        float steps = (i + 1 == count) ? kOuterStepCells
                                       : std::ceil(kInnerStepCells * cellLast / cell);
        steps = std::max(1.0f, std::min(steps, float(rows[i].resolution) * 0.5f));
        // THE NEAR-FIELD RULE IS A CEILING ON ALL OF IT.
        rows[i].stepCells = std::min(steps, giNearFieldMaxStepCells(rows[i]));
    }
}

// ---------------------------------------------------------------------------
// THE SCREEN-PROBE GATHER (SPECS/SCREEN_PROBE_GATHER_SPEC.md, phase 1).
//
// ONE PROBE PER N x N PIXELS traces a hemisphere of hardware rays and the
// pixels of its cell read the integral back, so the diffuse-GI estimate is made
// once per cell with RAYS instead of once per pixel with six voxel cones. The
// switch is `GiParams::gather`; the numbers come back in `GiStatus::gather`;
// `GatherTuning` is the test-and-tool door onto the knobs the tier otherwise
// derives. With the row off nothing is allocated, nothing is dispatched, no
// shader property is set and no pixel moves -- both selftest hashes, measured.

/// THE KNOBS A TIER OTHERWISE DERIVES -- test and tool only (a measuring suite,
/// a spike), never a document row and never a panel. Every zero means "take
/// what the tier's quality row derives", so a default-constructed tuning is
/// exactly the shipped configuration.
struct GatherTuning {
    /// Pixels per probe on both axes. The tier's own: 16 at Medium and High,
    /// 8 at Epic (four times the probes).
    unsigned probeStride = 0u;
    /// The octahedral probe map's resolution; the probe traces octRes^2 rays,
    /// one per texel, and 8 is the shipped value (64 rays). At most 8 -- one
    /// ray is one thread of the trace's 8x8 workgroup.
    unsigned octRes = 0u;
    /// The ray's length in world units. 0 = derive it: half the outermost
    /// cascade's extent (the lit volume's inscribed radius), under the
    /// camera's far plane.
    float    rayLength = 0.0f;
    /// How many ADAPTIVE probes a frame may add on top of the uniform grid
    /// (one per cell at most, where the cell's pixels do not lie in the cell
    /// probe's plane). Negative = the tier's; 0 = a uniform grid and nothing
    /// else, which is the A/B that prices the adaptive pass.
    int      adaptiveCap = -1;
    /// THE DETERMINISM ARM. The sample sequence's only input is an integer hash
    /// of (probe cell, ray, frame index); holding the frame term makes the
    /// RAW estimate of consecutive frames of a still scene byte-identical. The
    /// packed history's rounding dither (GA-VR) is keyed on the view's ADVANCING
    /// frame counter and ignores this freeze, so a pixel A/B is byte-exact only
    /// where the rest mean holds (a still view past its rest frames) or under
    /// the arm "gather.temporal" = 0; a moving arm or a stereo view (which never
    /// rests) compares within the rounding's amplitude, not exactly.
    bool     freezeFrameIndex = false;
    /// The probe sits at its cell's CENTRE instead of being jittered inside it
    /// -- the A/B for what the jitter costs and buys. THE SHIPPED JITTER IS THE
    /// GRID'S (ENGINE-POLISH-PHOTON-2 stage 3; U5 decision 20's arm): one offset a
    /// frame for every cell's probe, and the integrate's bilinear on the grid so
    /// shifted - the probes' true positions (a probe jittered alone and read as if
    /// at its cell's centre biased the interpolation 1-2.6 %).
    bool     jitterOff = false;
    /// THE RAYS AT THEIR TEXELS' CENTRES (IMAGE-1): every ray of a probe leaves
    /// through its octahedral texel's centre instead of a jittered point in it —
    /// with jitterOff, a still view's estimate is then the same every frame, the
    /// arm gi.gather_stable holds the packed history's own storage flicker on.
    bool     rayJitterOff = false;
    /// THE FAR QUERY OFF (ATOM-FARBLAS-1): a ray that escapes its near length
    /// reads the sky directly instead of tracing the far copies (the coarse
    /// levels) out to the far plane -- the A/B that prices the far field.
    bool     farQueryOff = false;
    /// THE IRRADIANCE READBACK (PHOTON-GATHER-1b) -- a TEST AND TOOL door,
    /// never a frame path: each gathered frame also copies the full-resolution
    /// `probeIrradiance` target (rgb = E/pi, w = the coverage) into host memory,
    /// and `GatherStatus::irradiance` carries the last RETIRED frame's copy (a
    /// few frames late, like the timestamps). It is how a suite reads the
    /// gather's own answer -- in the shader's units, before any material, light
    /// or tonemap touches it -- and how determinism is asserted on the estimate
    /// itself rather than on a picture.
    bool     readback = false;
    /// THE PIXEL HISTORY'S COUNT TO THE HOST (PHOTON-2 stage 0; the arm "gather.historyReadback") -- an
    /// INSTRUMENT, never a frame path: each gathered frame also copies the history the integrate wrote, and
    /// `GatherStatus::historyCount` carries the last retired frame's per-pixel count. The count is the gather's
    /// OWN same-surface verdict (jah_probe_history.glsl's jahHistoryAt): 1 = the pixel's history was refused or
    /// absent this frame (a surface new to the screen), n > 1 = it carried n - 1 frames. Not an estimator change.
    bool     historyReadback = false;
    /// THE SH BANDS THE INTEGRATE EVALUATES (PHOTON-GATHER-1b item 3's
    /// measurement arm): 9 = the whole record (L0..L2, the shipped value), 4 =
    /// L0 and L1 only (the stated fallback, whose memory traffic is what the
    /// SH9 record is priced against). 0 = the shipped value.
    unsigned shBands = 0u;
    /// THE FILTER IN PROBE SPACE OFF (PHOTON-GATHER-1b item 2): each probe's SH
    /// is projected from its own 64 rays alone — the A/B that prices the filter
    /// and measures what it buys.
    bool     filterOff = false;
    /// THE PIXEL HISTORY'S MEMORY (PHOTON-GATHER-1c): the frames after which
    /// its running mean becomes an EMA at 1/historyFrames — a TRUE mean for
    /// that many frames, then each new frame weighs 1/historyFrames. 0 = the
    /// shipped 10 (the trade, measured on gi.gather_stable's room: the
    /// frame-to-frame flicker is ~ the floor x the single-frame innovation —
    /// 9 codes each frame alone -> 1 code; a lighting step of D codes arrives
    /// within 1 code after ln(1/D)/ln(1 - 1/historyFrames) frames — 16 for a
    /// 5-code step). Clamped to 1..15 (the packed history's four count bits,
    /// PHOTON-GA-VR).
    unsigned historyFrames = 0u;
    /// THE HISTORY'S VALIDATION OFF (PHOTON-GATHER-1d, the 1c audit's m2) — a
    /// TEST door, never shipped: every reprojected texel is accepted (the 5 %
    /// distance test and the normal test both off), which is what a history that
    /// stopped validating would show. gi.gather_motion drives it to prove its
    /// disocclusion bar discriminates that defect.
    bool     historyValidationOff = false;
    /// THE REST OFF (PHOTON-GATHER-1d) — a MEASUREMENT door, never shipped: the
    /// view never counts a rest frame, so there is no rest mean and no hold and
    /// every frame is the pixel history's. A still view HOLDS one N-sample mean
    /// (GatherStatus::settled), so averaging its frames reads that one draw; a
    /// suite that averages a still view's frames to read the ESTIMATOR's mean
    /// (gi.gather_reference, gi.gather_plane) or that measures the history's own
    /// floor at rest (gi.gather_stable's trade) sets it.
    bool     restOff = false;
    /// THE SURFACE CLASS OUT OF THE HISTORY TEST (PHOTON-2 K5) - a TEST door, never shipped: every pixel is
    /// held to the 1 % depth rule and the class match is off (jah_probe_history.glsl), the gather as it would
    /// be without its foliage class - the base arm of the class's guard (gi.hold_after_stop's canopy).
    bool     classOff = false;
    /// THE GI FREEZE (PHOTON-2 round 2, M1's measuring arm "gi.freeze") - never shipped: a pixel with a history keeps
    /// it (no new sample enters the integrate's mean), so a walk measures the motion's own resampling.
    bool     freezeHistory = false;
    /// THE FILTER IN PROBE SPACE'S REACH (GATHER-NOISE-1): 0 = the shipped
    /// reach, 1 = the 3 x 3 neighbourhood alone, 2 = that plus the ring of eight
    /// at two cells (17 taps). Each tap is weighed by the plane and hit-distance
    /// tests exactly as the inner ring is.
    unsigned filterRadius = 0u;
    /// THE REST MEAN'S LENGTH and its SAMPLE SEQUENCE (GATHER-NOISE-1, the
    /// converged reference): 0 = the settle (settleFramesOf), else the rest
    /// mean averages this many rest frames before the view holds; `restSeed`
    /// offsets the rest frames' sequence so several references of one pose are
    /// independent draws. Measurement doors, never shipped.
    unsigned restFrames = 0u;
    unsigned restSeed = 0u;   ///< at most 255 (the sequence's frame travels as a float)
    /// THE HISTORY-AGE VIEW (GATHER-NOISE-1's instrument): N > 0 paints every
    /// pixel whose reprojected history held fewer than N frames magenta and
    /// every other pixel black, in place of the gather's answer. The history
    /// itself runs as shipped. Never shipped.
    unsigned ageView = 0u;
    /// THE YOUNG HISTORY'S REACH (GATHER-NOISE-1): a pixel whose history holds
    /// fewer than `youngFrames` frames reads the probes of a WIDER neighbourhood
    /// — a tent `youngReach` PIXELS wide at a history of 0 (in cells of the
    /// view's probe stride, never under one), narrowing linearly to
    /// the shipped bilinear (1 cell) at `youngFrames` — each weighed by the plane
    /// test as the bilinear four are. 0 = the shipped values; youngFrames 1 with
    /// youngReach 1 is the reach off (the pre-lane integrate). In pixels, not
    /// cells: the blur it trades is a distance on the picture (3 cells at High's
    /// 16-px stride raised the walls' error +13 % where 3 at Epic's 8 px held it).
    unsigned youngFrames = 0u;
    float    youngReach = 0.0f;
    /// THE RAYS STRATIFIED ACROSS NEIGHBOURING PROBES (GATHER-NOISE-1): each
    /// ray's jitter inside its octahedral texel is confined to one of N x N
    /// sub-cells, assigned so that any N x N block of probe cells covers all of
    /// them once (a Latin tiling shifted at random per texel and per frame:
    /// every probe's ray is still uniform over its texel across frames). The
    /// probe filter and the bilinear average neighbours' rays of one direction,
    /// so their mean is a STRATIFIED estimate of the texel instead of N^2
    /// independent ones. 0 = the shipped N; 1 = off (each ray jittered alone).
    unsigned crossStrata = 0u;
};

// THE PIXEL HISTORY'S MEASUREMENT LEVER (PHOTON-GATHER-1c item 3) is an
// MEASUREMENT ARM, not a tuning field: Engine::setArm("gather.temporal", 0)
// makes every gathering view publish each frame's estimate alone (no history
// read or written), latched per frame so a suite drives both arms in one
// process (lane TEST-1 deleted the JAHSHAKA_GATHER_NO_TEMPORAL door). It is the frozen-frame rule's pair: a frozen frame
// index makes consecutive frames the same estimate; this makes each frame's
// picture that frame's estimate.

/// THE STEP THE SETTLED-HISTORY PREDICATE WAITS OUT, in display codes (of 255):
/// the gather's pixel history is an EMA at 1/historyFrames once it is full, so a
/// lighting step of D codes decays under one code after ln(1/D)/ln(1 - 1/h)
/// frames — 16 at D = 5 and h = 10 (the lamp's measured 5.3-code indirect step,
/// PHOTON-GATHER-1c; a full-range step would need 53). `giAtRest` waits for it.
constexpr float kGatherSettleCodes = 5.0f;

/// A GI PLACEMENT PIN (PHOTON-2: the stillness door's path check judges walk vs teleport at EQUAL PLACEMENTS — the
/// spec's rev-4 correction 3: a cascade's placement may depend on the path, its values may not). While `on`, every
/// re-centre of cascade i takes its placement from `cascadeBuiltCam[i]` (a CascadeStatus::builtCam recorded
/// elsewhere) instead of the eye, and a re-placement of the irradiance field (a jump of the whole grid) puts its window
/// at `fieldOrigin` (GiStatus::ifdMin recorded) when `field` is set. A MEASURING DOOR, never a picture path.
struct GiPlacementPin {
    bool on = false;
    std::vector<Vec3> cascadeBuiltCam;
    /// A CASCADE LEFT FREE (PHOTON-7 stage 10-0's P2: the outer cascade pinned alone): cascade i follows the eye when
    /// `cascadeFree[i]` is set, whatever `cascadeBuiltCam` holds for it - a pin is no longer a PREFIX of the chain only.
    std::vector<unsigned char> cascadeFree;
    bool field = false;
    Vec3 fieldOrigin;
};

/// THE CASCADES' VOXEL DIGESTS, ASKED FOR WITHOUT WAITING (PHOTON-2 stage 2's round, H5; Scene::requestGiVoxelDigests):
/// per cascade, innermost first, the hash of every light volume a reader samples (`light`) and of the direct volume
/// (`direct`, empty on a cascade that does not bounce) — GiVoxelStats' two digests' purpose, under a word-wise hash
/// (comparable only with each other). `ready` turns true on the render thread once they are filled; the host polls it.
struct GiVoxelDigests {
    std::vector<std::string> light, direct;
    std::atomic<bool> ready{ false };
};

/// The project's row (pushed by the host from the document, like the ray row).
struct SunContactDesc {
    bool  enabled = false;
    float range = kSunContactDefaultRange;
    SunContactResolution resolution = SunContactResolution::Auto;
    bool operator==(const SunContactDesc &o) const {
        return enabled == o.enabled && range == o.range && resolution == o.resolution;
    }
    bool operator!=(const SunContactDesc &o) const { return !(*this == o); }
};

}}  // namespace jahshaka::engine
