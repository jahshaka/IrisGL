#pragma once
// THE ENGINE's GICARDS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

// ---- SURFACE-CACHE phase 2: the capture cache's status and its knobs -------
//
// THE STATE THE CACHE PUBLISHES. SURFACE-CACHE-0's `SurfaceCardSpikeDesc` /
// `Result` / `Action` are DELETED with the spike they measured (their three
// numbers are banked in spikes/surface-cache-0/FINDINGS.md): a measurement
// surface on the public boundary for one test's benefit is exactly the debt
// that lane recorded against itself. What replaces it is a STATUS — the same
// shape every other engine cache publishes — plus the knobs on GiParams.

/// ONE SAMPLE OF ONE CARD TEXEL, read back through an AsyncTextureTicket.
///
/// A TEST AND TOOL PATH, never a per-frame one: it flushes the render system's
/// commands and downloads five one-texel boxes (CLAUDE.md's AsyncTextureTicket
/// rule — a ticket taken over a target the open command buffer has only
/// RECORDED into reads recycled VRAM). It is how a suite asks the one question
/// a picture cannot answer about a card — "is this texel's shadow term
/// occlusion by ANOTHER object" — and how the atlas is read channel by channel
/// when a capture looks wrong.
/// ONE RESIDENT CARD'S LIT RADIANCE, WHOLE (PHOTON-7 stage 5's oracle door, Scene::readCardRadiance): the node and its
/// mesh card's index, the card's side in texels, the cache frames it was last relit (both halves) and last marched (its
/// indirect half) in, and its radiance texel by texel (rgb, row-major, the atlas's rows: row 0 the card's +v side).
struct CardRadiance {
    NodeId node = 0;
    unsigned card = 0u;     ///< its place in the instance's layout (every card the mesh's list cuts)
    unsigned bucket = 0u;   ///< its face: the outward axis's largest component, 2 x axis + (negative)
    unsigned size = 0u;
    unsigned long long lastRelit = 0ull, lastIndirect = 0ull;
    /// The newest cache frame a reader read it (or its admission's).
    unsigned long long lastRead = 0ull;
    std::vector<float> rgb;
};

/// ONE FACE'S DEMAND (PHOTON-7 stage 7, Scene::readCardFaceDemand; the measuring arm cards.demandCount): an item's face
/// bucket (2 x axis + (negative), the hit's facing), every READ of it by each reader (0 the reflection, 1 the gather) -
/// its card's or, none resident, the hit that marked it - cumulative since the arm was set (the settle law's lambda,
/// GiQualityFacts::cardDemandHitFloor, is this count's rate), each reader's newest harvested no-card mark, and the
/// cards of the face resident now. Listed while the arm is on, for every face that was read, marked or is resident.
struct CardFaceDemand {
    NodeId node = 0;
    unsigned bucket = 0u;
    unsigned long long hits[2] = { 0ull, 0ull };
    unsigned long long mark[2] = { 0ull, 0ull };
    unsigned resident = 0u;
};

struct CardSample {
    bool  ok = false;
    float albedo[3] = { 0, 0, 0 };      ///< kD, i.e. the datablock's diffuse ALREADY divided by pi
    float normal[3] = { 0, 0, 0 };      ///< the shading normal in the CARD's own view space
    float emissive[3] = { 0, 0, 0 };    ///< radiance
    float depth = 0.0f;                 ///< world units from the card's near plane; 0 = nothing captured there
    float shadow = 0.0f;                ///< the still world's sun term: 1 lit, 0 occluded (one traced ray)
    float roughness = 0.0f;             ///< the GGX ALPHA (perceptual squared), through fork 16d8e29d4 (was 0043)'s range
    /// THE LIT CARD (the sixth layer, `Jahshaka/CardLight`): the texel's
    /// outgoing diffuse radiance — direct from the scene's lights (the sun
    /// through `shadow`) plus the indirect below plus the emissive. 0 until
    /// the card has been relit.
    float radiance[3] = { 0, 0, 0 };
    /// ...and its cached INDIRECT diffuse half alone (the voxel march from the texel x
    /// kD x pi x the lobe's hemispherical albedo — BRDF_EnvMap's arithmetic); 0 until
    /// the card's indirect has been marched, and 0 in a channel with no diffuse albedo.
    float indirect[3] = { 0, 0, 0 };
    /// THE INDIRECT LAYER AS STORED (ENGINE-POLISH-PHOTON-1 stage 3): `indirect` where the
    /// channel has a diffuse albedo, the specular lobe's indirect share where it has none.
    float indirectStored[3] = { 0, 0, 0 };
    /// THE SPECULAR LOBE the capture stored (the Specular layer): F0 x kS
    /// (lerp( 0.04, base, metalness ) x kS; 0 for the specular workflows, as in the
    /// voxel store) and the Fresnel-90 weight kS.
    float specularF0[3] = { 0, 0, 0 };
    float specularWeight = 0.0f;
    /// THE FLAT TERM (the relight's, ENGINE-POLISH-PHOTON-1 stage 3): the emissive
    /// plus the specular lobe's re-emission - what the texel sends the same way to
    /// every direction; inside `radiance` too.
    float flat[3] = { 0, 0, 0 };
    /// THE LAMPS' TRACED VISIBILITY AT THE TEXEL (PHOTON-7 stage 5, 5-3; 48 lamps since 10-4 R9): bit j of `lampMask`
    /// = lamp j of the card's table (`lampIds`, world ids: SceneLightList::documentId's 31 bits), `lampOverflowVis` the
    /// lamps past it (and the area lights); bit j of `lampThroughMask` = lamp j reached through panes (the one read's F3),
    /// which reads `lampThroughVis` (the transmitted lamps' share-weighted visibility); a lamp in neither mask is blocked.
    unsigned long long lampMask = 0ull;
    float lampOverflowVis = 1.0f;
    unsigned long long lampThroughMask = 0ull;
    float lampThroughVis = 1.0f;
    unsigned lampIds[48] = {};
    unsigned lampCount = 0u;
    /// WHICH card and WHICH atlas texel answered (Scene::readCardAt; -1 from
    /// readCardTexel), and whether its radiance carries a marched indirect
    /// half — what gi.card_read_parity holds the ray job's own pick to.
    int      card = -1;
    unsigned texelX = 0u, texelY = 0u;
    bool     lit = false;
    /// THE TEXEL'S OWN SURFACE (ATOM-S3-CARDCAP): the world point and world normal
    /// the sun trace reconstructs from the texel's depth and normal (the relight's
    /// arithmetic), and the lift along the normal its still-world ray started at
    /// (0 until traced) — what gi.card_shadow's reference trace repeats.
    float position[3] = { 0, 0, 0 };
    float worldNormal[3] = { 0, 0, 0 };
    float sunLift = 0.0f;
    /// THE GATHER LAYER AT THE TEXEL (PHOTON-7 stage 10, 10-2a; rq_card_gather.comp): its last TLAS gather's
    /// envColourD (rgb) and the radiance it last REPORTED (a: the card-to-card re-listing's reference, 10-2c). All 0
    /// where the ray tier never gathered (no layer).
    float gather[4] = { 0, 0, 0, 0 };
    /// ...and THE e = 0 COUNTER of the answering card: its last TLAS gather's rays that escaped to the sky (-1: none
    /// read back yet) - 0 for every card inside a watertight room.
    long long gatherSky = -1;
};

/// THE LAWS ASKED OF A SHIPPED SHADER FUNCTION DIRECTLY (Engine::giLaw; BISTRO-REST-1), each its own test-only job:
/// Share - THE HIT READ's BOUNCE SHARE (gi.share_law, rq_share_law.comp says the modes): mode 0 THE SWEEP - q = (T, D,
/// k, lod): the share and the answer the shipped jah_rq_hit.glsl hands back for a pick holding T and D at coverage k,
/// its read rad = T / k; mode 1 THE LOOP - q = (S, F, rho, k): two halves of one cell reading each other under the
/// Jacobi door from out of phase, u[1] steps, each rounded to the store's half float.
/// Card - A CARD TEXEL's STORED TERMS, ITS READ AND ITS MOVE (gi.card_law, rq_card_law.comp says the modes).
enum class GiLaw : unsigned { Share = 0u, Card = 1u };
struct GiLawQuestion {
    float q[4] = { 0.f, 0.f, 0.f, 0.f };
    unsigned u[4] = { 0u, 0u, 0u, 0u };   ///< u[0] the mode; the rest the law's (its job's head)
};
/// ...its answer: Share mode 0 a = (share, answer, rad, -), mode 1 a = X and b = Y at the last four iterates; Card: its job's head.
struct GiLawAnswer {
    float a[4] = { 0.f, 0.f, 0.f, 0.f };
    float b[4] = { 0.f, 0.f, 0.f, 0.f };
};

/// ONE QUESTION FOR THE RAY JOB'S CARD READ (Engine::cardReadParity): a world
/// point on `node`'s surface and the direction the read treats as "facing"
/// (the ray job passes the reversed ray direction — jah_rq_card.glsl says why).
struct CardReadQuery {
    Vec3   position;
    Vec3   facing;
    NodeId node = 0;
    /// A TRACED question: `position` is a ray's origin and `facing` its
    /// direction; the job traces the scene's TLAS (near copies, as the
    /// reflection does), takes the instance from the hit and the facing from
    /// the hit triangle's geometric normal — the reflection's hit path whole
    /// but the lighting. `node` is ignored.
    bool   trace = false;
};
/// ...and the ray job's answer: the card and texel its GLSL picked, whether the
/// card was lit, and the radiance it would return (0 when not `ok`).
struct CardReadPick {
    bool     ok = false;
    bool     lit = false;
    int      card = -1;
    unsigned texelX = 0u, texelY = 0u;
    float    radiance[3] = { 0, 0, 0 };
    /// A traced question's hit: whether the ray hit, where, and the geometric
    /// normal the job rebuilt there (the facing it asked the pick with).
    bool     hit = false;
    float    hitPoint[3] = { 0, 0, 0 };
    float    hitNormal[3] = { 0, 0, 0 };
    /// ...and the radiance the read sends TOWARDS ITS VIEWER (PHOTON-CARDS-5,
    /// jah_card_view.glsl: the diffuse lobe's view term restored) — a traced
    /// question's ray origin, an untraced one's facing direction (head-on).
    /// `radiance` above is the texel as stored.
    float    viewed[3] = { 0, 0, 0 };
};

/// THE SURFACE CACHE'S OWN STATUS (GiStatus::cards). Every counter is the model
/// `gi.material_swap` set for the voxel side: a suite drives an edit and asserts
/// what the renderer SPENT, not what it looks like.
struct CardCacheStatus {
    bool     built = false;             ///< the atlas exists
    /// The atlas as built: `pages` pages of `pageSize` texels a side across the
    /// five layers, `bytes` of VRAM, `bytesPerTexel` the deliberate format sum.
    unsigned pageSize = 0u;
    unsigned pages = 0u;
    unsigned pagesUsed = 0u;
    unsigned bytesPerTexel = 0u;
    unsigned long long bytes = 0ull;
    /// The emissive layer's format NAME, because it is chosen at runtime from
    /// what the device can render to (RGB9E5 where it is a colour attachment,
    /// R11G11B10F where it is not) and the choice must be visible.
    std::string emissiveFormat;
    /// Residency: the instances holding pages, and their cards.
    unsigned instancesResident = 0u;
    unsigned cardsResident = 0u;
    /// WHICH cards are resident (PHOTON-7 stage 10-3, the resize row): FNV-1a 64 over the resident cards' (node id, card
    /// index) pairs, sorted - equal fingerprints are the same resident set, whatever order it arrived in; 0 with none.
    unsigned long long residentHash = 0ull;
    /// RESIDENCY BY THE READERS' DEMAND (PHOTON-7 stage 5, 5-2): the cards admitted (a face a hit marked, a read
    /// card's re-admission after a move), evicted (unread for the window, an arrival needing the room) and moved by the
    /// atlas's compaction (each re-captured where it lands), cumulative; and under the measuring arm
    /// "cards.demandCount", the readers' hits ( the reflection's that found a card, that found none, the gather's
    /// found, none ).
    unsigned long long admitted = 0ull, evicted = 0ull, compactionMoves = 0ull;
    /// ...THE ARRIVALS' STAMPS (PHOTON-7 stage 5, 5-5: what a law after a stop attributes a capture to): the cache's own
    /// frame (`cacheFrame`, the stamps' clock), the newest face mark among its LAST update's arrivals (`arrivalMarkNewest`,
    /// 0 with none), and the demand read-back's period and lag in those frames (OgreSurfaceCache.cpp's kDemandReadEvery;
    /// a reading is taken once the frame is past its own by the VaoManager's dynamic buffer multiplier: that + 1) - a face
    /// a reader marked by frame s arrives by s + demandReadEvery + demandReadLag.
    unsigned long long cacheFrame = 0ull, arrivalMarkNewest = 0ull;
    unsigned demandReadEvery = 0u, demandReadLag = 0u;
    /// THE DEMAND WINDOW's INPUTS (PHOTON-7 stage 10-3, the exact judge): the last demand change's cache frame, each
    /// reader's FIRST and NEWEST counted stamp since it (0 the reflection, 1 the gather; 0: none), and the lag the window's
    /// closing counts (the read-back's, plus the issue frame's one) - SurfaceCache::demandWindowOpen reads exactly these,
    /// and a law after a stop (tests/support/owed_rows.py) applies that one formula to them.
    unsigned long long demandChangeFrame = 0ull, demandFirst[2] = { 0ull, 0ull }, demandNewest[2] = { 0ull, 0ull };
    unsigned demandOpenLag = 0u;
    /// THE LAMPS' TRACE (PHOTON-7 stage 5, 5-3): the relights whose cards were traced toward their lamps (cumulative)
    /// and the cards traced the last frame.
    unsigned long long lampTraces = 0ull;
    unsigned lampTracedLastFrame = 0u;
    /// ...the most POINT AND SPOT lamps that reached one relit card (its table holds 16), and the largest share of a
    /// card's direct past them (the lamps the table could not hold), since the cache was built.
    unsigned cardLampsMax = 0u;
    float lampOverflowShareMax = 0.0f;
    /// THE FRACTIONAL BYTE (the 5-3 fix): the area lights (never in the table: each one's visibility fractional) and
    /// the lamps past the table share the SunVis layer's blue, weighted by their shares - exact for ONE such lamp a card.
    /// The most area lights that reached one relit card since the build, and the resident cards now with two or more
    /// area lights / with two or more lamps in the byte (area lights and the overflow): where the byte is a mix.
    unsigned cardAreaLampsMax = 0u;
    unsigned cardsMultiArea = 0u, cardsSharedFractional = 0u;
    /// The last lamp trace's rays, at most (its cards' texels x one a point or spot lamp in range of the card and the
    /// strata's n^2 an area light), and n (GiQualityFacts::cardAreaStrata).
    unsigned long long lampRaysLastTrace = 0ull;
    unsigned areaStrata = 0u;
    /// THE INDIRECT HALF BY TLAS RAYS (PHOTON-7 stage 10, 10-2a; rq_card_gather.comp): at the ray tier the relight's
    /// diffuse GI is gathered by the TLAS, its own pass before the relight, into the cache's Gather layer. `gather` is
    /// the life of the cache's counts, read back by the ray tier a fixed number of frames after each dispatch: the
    /// dispatches, the rays traced and what answered each - a card, a cascade, a back face (0), no sample (neither
    /// could shade the hit), the sky (a miss out to the reach) - and the texels gathered, those with no sample at all.
    /// THE e = 0 COUNTER: inside a watertight room `sky` stays 0 (the sealed box, gi.sealed_sky law (6)).
    /// `gatherWaits` = relights whose indirect waited for the ray tier's structure (re-listed: never the voxel rays at
    /// the ray tier); `gatherByRays` = the last relight that gathered read the TLAS (false: the voxel chain's rays).
    struct GatherCounts {
        unsigned long long dispatches = 0ull, rays = 0ull, card = 0ull, voxel = 0ull, backFaces = 0ull,
                           noSample = 0ull, sky = 0ull, texels = 0ull, emptyTexels = 0ull;
        /// ...and the texels whose reads overflowed their card's read list (10-2c: the card then takes the frame's
        /// largest move whole)
        unsigned long long readsOverflowed = 0ull;
        /// ...and the BURIED texels (PHOTON-7 stage 10-3: jahRayOriginBuried - the texel's lift crosses a front face, its
        /// origin behind a surface): they gather nothing
        unsigned long long buried = 0ull;
    } gather;
    unsigned long long gatherWaits = 0ull;
    bool gatherByRays = false;
    /// THE CARD-TO-CARD RE-LISTING (10-2c): the indirect listings a moved card read by the listed one made, and those an
    /// overflowed read list's geometric bound took part in (both inside indirectReported).
    unsigned long long indirectByCards = 0ull, indirectByBound = 0ull;
    /// THE READERS' THRESHOLD (PHOTON-7 stage 10, R3 (a) applied to every reader): the relight frames that moved a card
    /// texel by at least a display code since its last report, or could not be measured (counted as such: `unmeasured`
    /// of them) - `visibleRelights`, what a reader's rest key folds - and those whose every texel stayed under a code
    /// (`subQuantumRelights`). The newest reading's largest move IN CODES (-1: a non-finite move) and the cache frame it
    /// was relit at (a reading arrives at the report's fixed lag).
    unsigned long long visibleRelights = 0ull, subQuantumRelights = 0ull, unmeasuredRelights = 0ull;
    /// THE READERS' HOLD IS OWED (SurfaceCache::restHoldOwed; PhotonComponent::readersHoldOwed): the stop's one decision
    /// not yet taken, its captures, relights or card-to-card rounds still to run, or a report in flight - neither reader
    /// holds, and GiStatus::giAtRest is false, while it is.
    bool readersHoldOwed = false;
    float moveCodesLast = 0.0f;
    unsigned long long moveCodesFrame = 0ull;
    /// THE REST'S DRAIN (10-2d): cards removed because no view reader read them at the pose (inside `evicted`) - at the
    /// rest's one decision (R3 (b)), counted in `restDecisions` (one a demand change, at most).
    unsigned long long restDrained = 0ull, restDecisions = 0ull;
    /// THE LAST REST DECISION's ROOM (PHOTON-7 stage 10, the residency diagnosis): the atlas texels in use and free when
    /// its arrivals were placed, and its arrivals - admitted, and REFUSED for want of free room (non-persistent arrivals
    /// never evict) - each refused one by its node, card index, size and mark (the frame its face was last marked).
    unsigned long long decisionUsedTexels = 0ull, decisionFreeTexels = 0ull;
    unsigned decisionAdmitted = 0u, decisionRefused = 0u;
    /// ...and the READERS the last decision armed with (PHOTON-7 stage 10-3, F1): bit 0 the reflection, bit 1 the
    /// gather - a reader that counted as stale when the decision armed makes no demand, so its bit is clear (3 = both).
    unsigned decisionReaders = 0u;
    struct Refused { NodeId node = 0; unsigned index = 0u, size = 0u; unsigned long long mark = 0ull; };
    std::vector<Refused> decisionRefusedCards;
    /// ...and the window's one drain a change (R3, the lead's Q2: the cards no reader read in the S-frame window leave
    /// once when it closes), counted when it drained a card.
    unsigned long long windowDrains = 0ull;
    /// THE ROOTS OF THE LAST PLAN'S WORK (PHOTON-7 stage 10, R3 (c): each round names its parent move): the moves the
    /// frame's relights trace to - `kind` 'a' a demand arrival, 'd' the rest's one decision, 'w' the window's one drain
    /// (its re-march of every resident card when one left), 'f' the cards.force door's departure (that re-march, at once),
    /// 'v' the voxel store's move (a card listed by the bricks it reads), 'h' moves older than the root ring (born at its
    /// horizon: older still), 'r' a region, an overflow, a material or a relight no root named - each by the cache frame
    /// it was born at, and the deepest round among them (0: a root's own captures; n: listed card to card by a round
    /// n - 1 move).
    struct WorkRoot { char kind = 0; unsigned long long frame = 0ull; };
    std::vector<WorkRoot> workRoots;
    unsigned workRound = 0u;
    unsigned long long demandHits[4] = { 0ull, 0ull, 0ull, 0ull };
    /// The queue: cards waiting for a capture, and the texel budget a frame
    /// spends on them.
    unsigned queueLength = 0u;
    unsigned budgetTexels = 0u;
    /// What the LAST rendered frame spent: cards captured and texels written.
    unsigned capturesLastFrame = 0u;
    unsigned texelsLastFrame = 0u;
    /// ...and the life of the cache, so a suite can difference across an edit.
    unsigned long long captures = 0ull;
    /// Why cards were thrown back on the queue, counted for the life of the
    /// cache: a moved instance, a material edit. (No light re-queues a capture: a
    /// capture holds no light quantity — the sun term is traced, below.)
    unsigned long long invalidTransform = 0ull;
    unsigned long long invalidMaterial = 0ull;
    /// CPU milliseconds the last frame's captures cost, measured around the
    /// capture workspace's own update.
    float captureMs = 0.0f;
    /// ...split, because the two halves are different kinds of work and a lane
    /// that wants to make a capture cheaper has to know which one it is: the
    /// compositor workspace's own update (the pass set-up, the cull, the Hlms
    /// pass buffer, the raster) against the five `vkCmdCopyImage` that move the
    /// page into the atlas.
    float captureWorkspaceMs = 0.0f;
    float captureCopyMs = 0.0f;
    /// ...and the CPU of recording the still world's sun trace (ATOM-S3-CARDCAP):
    /// part of a capture, since a card is not readable before its sun term.
    float captureTraceMs = 0.0f;
    /// THE RAY READ'S TABLES, as they stand: how many card records the GPU
    /// buffer describes, and how many item slots the instance buffer is indexed
    /// over — what the reflection trace's card read (jah_rq_card.glsl) binds.
    unsigned cardRecords = 0u;
    unsigned instanceSlots = 0u;
    /// THE LIT CARD (PHOTON-CARDS-1): the Radiance layer's format name (chosen
    /// from what the device can store to from a compute job — R11G11B10F, else
    /// RGBA16F), the relight budget in texels a frame, what the last frame
    /// relit (cards, texels), the relights for the life of the cache, the light
    /// writes that changed only a card's RADIANCE (a colour, an intensity — no
    /// recapture), and the CPU milliseconds the last frame's relight dispatch
    /// cost to record.
    std::string radianceFormat;
    unsigned lightBudgetTexels = 0u;
    unsigned relitLastFrame = 0u;
    unsigned relitTexelsLastFrame = 0u;
    unsigned long long relights = 0ull;
    unsigned long long invalidRadiance = 0ull;
    float lightMs = 0.0f;
    /// THE INDIRECT HALF's own budget and counters: cards and texels whose
    /// voxel march ran last frame, marches for the life of the cache, the
    /// re-injections that staled the resident set's indirect (one per burst),
    /// and whether the last relight had a chain to march at all.
    unsigned indirectBudgetTexels = 0u;
    unsigned indirectLastFrame = 0u;
    unsigned indirectTexelsLastFrame = 0u;
    unsigned long long indirectRelights = 0ull;
    unsigned long long invalidIndirect = 0ull;
    /// THE INDIRECT HALF'S STALE SET (PHOTON-7 stage 5, 5-1): the cards the GPU's accumulation listed (the light they
    /// gather moved a code since their march; read back a fixed lag of frames late), and those a SLAB region met (a
    /// commit's entering or leaving slab), cumulative; and the frames whose moves could not be measured (the job not
    /// built yet, or a cascade past the cluster table), each of which listed every card.
    unsigned long long indirectReported = 0ull, indirectByRegion = 0ull, indirectOverflows = 0ull;
    /// ...the listings skipped as ANSWERED (older than the listed card's own last march, R3), and the reports never
    /// read because every read-back slot was in flight (PHOTON-7 stage 10, the listing diagnosis): a listed card's GPU
    /// flag stays set until its next march, so a report never read can strand it
    unsigned long long indirectAnswered = 0ull, reportsDropped = 0ull;
    bool indirectOn = false;
    /// THE RELIGHT'S LIGHTS (P1C-LIGHT-LIST), selected from the scene's one light
    /// list at the last relight: the directional, point and spot lights whose range
    /// reaches the box of the cards it relit (`lightsInRange`), and the ones the
    /// job's 64 could not hold — the dimmest at that box (`lightsDropped`; the
    /// engine log says so once per cache).
    unsigned lightsInRange = 0u;
    unsigned lightsDropped = 0u;
    /// THE STILL WORLD'S SUN TERM (ATOM-S3-CARDCAP): one ray per card texel towards
    /// the sun against the still world's shadow casters (`kRayMaskStillCaster`),
    /// into the ShadowRough layer's x — every card the frame it is captured, and
    /// every card whose term went stale: the sun turned (every resident card) or a
    /// still caster moved (the cards of its old and new sun-projected footprints),
    /// oldest first under the relight's budget. `invalidSun` = sun changes (one per
    /// gesture, the leading edge); `stillTraces` = cards traced for the life of
    /// the cache; the last frame's cards and texels; `stillPending` = stale cards
    /// past the budget; `stillGpuMs` = the job's GPU milliseconds on the last
    /// frame that ran it (the monitor's "cards.still" row: -1 outside a capture). `casterRetraces` = cards a still
    /// caster's change made stale (a transform write, a show/hide, its caster
    /// bit, a change of class, its deletion).
    unsigned long long invalidSun = 0ull;
    unsigned long long stillTraces = 0ull;
    unsigned stillTracedLastFrame = 0u;
    unsigned stillTexelsLastFrame = 0u;
    unsigned stillPending = 0u;
    float stillGpuMs = -1.0f;
    unsigned long long casterRetraces = 0ull;
    /// THE MOVERS' SHADOW ON THE CARDS (PHOTON-CARDS-4). A card's sun visibility
    /// is the still term (above) TIMES a traced term for the movers: a transform write of a shadow-casting mover traces sun rays
    /// against the movers alone (`kRayMaskMoverCaster`) from every texel of the
    /// cards inside its sun-projected footprint, old and new, and relights them
    /// in the same frame. `moverCasters` = the traced shadow-casting movers the
    /// last evaluation saw; the last frame's traced cards and texels and the
    /// lifetime count; `moverRetired` = cards whose mover term was dropped because
    /// no footprint reaches them any more (relit, not traced); `moverPending` =
    /// cards past the frame's budget, carried to the next frame (oldest pending
    /// first, nearest among equals — every pending card is traced within
    /// ceil(pending / the cards the budget holds) frames while movers keep moving).
    /// The budget is the relight's own number (`lightBudgetTexels`), spent a
    /// second time on the mover term. `moverGpuMs` / `relightGpuMs` = the trace
    /// job's and the relight job's GPU milliseconds on the last frame that ran
    /// them (the monitor's "cards.movers" / "cards.relight" rows: -1 outside a capture).
    unsigned moverCasters = 0u;
    unsigned moverTracedLastFrame = 0u;
    unsigned moverTexelsLastFrame = 0u;
    unsigned long long moverTraces = 0ull;
    unsigned long long moverRetired = 0ull;
    unsigned moverPending = 0u;
    /// ...and how many frames the OLDEST of them has waited since it went
    /// pending (0 when none waits) — the starvation reading: under the oldest-
    /// first order it stays below ceil(pending / the cards the budget holds).
    unsigned moverPendingAge = 0u;
    float moverGpuMs = -1.0f;
    float relightGpuMs = -1.0f;
};

}}  // namespace jahshaka::engine
