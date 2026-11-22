#pragma once
// THE ENGINE's RAYQUERY TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstdint>
#include <string>
#include <vector>

namespace jahshaka { namespace engine {

/// WHAT THE HARDWARE RAY-QUERY TIER IS DOING (PHOTON_SPEC §7 R1).
///
/// The tier keeps a ray-traceable copy of the scene — one bottom-level
/// acceleration structure per mesh, one top-level structure over the instances
/// — built from Ogre's OWN vertex and index buffers (fork d014b064f+b028638c1 (was 0038/0039))
/// and recorded into the frame's command buffer. Nothing consumes it yet: R2
/// (probe visibility), R3 (sun contact shadows) and R5 (reflections) are the
/// consumers, and each of them reads the SAME structure.
///
/// `available` is the DEVICE's answer (VK_KHR_acceleration_structure +
/// VK_KHR_ray_query enabled at vkCreateDevice) and cannot be changed by
/// anything but the hardware and the driver; `enabled` is ours — the no-rays
/// switch, which is how a machine WITH rays renders the picture a machine
/// without them gets, so every ray-consuming suite can assert both. Off, every
/// other field reads zero.
struct RayQueryStatus {
    /// The device has the extensions and the features (never true on macOS:
    /// MoltenVK exposes neither, SPECS/research/MOLTENVK_RAY_QUERY_2026-09-14.md).
    bool available = false;
    /// ...and we are using them. False with `available` true is the PROCESS
    /// switch in force (Engine::setRayTracing / --no-ray-query /
    /// JAHSHAKA_NO_RAY_QUERY=1). It is engine-wide: what one SCENE resolves to
    /// is `Scene::rayTracingResolved()`, which ANDs this with the project's own
    /// state (iris::Scene::rayTracing, the World panel's row).
    bool enabled = false;
    /// Bottom-level structures held — one per unique (mesh, level) the traced
    /// set asks for: the near copies' levels and every mesh's coarsest.
    int  blasCount = 0;
    /// The traced set's size: the NEAR copies in the top-level structure. It is
    /// NOT the scene's Item count — editor helpers, backdrops, the sun disc,
    /// overlay-queue objects and alpha-tested ones (no any-hit without
    /// ray-tracing pipelines) are all out. A RIGGED Item is in (PHOTON-SKIN-1)
    /// through its own skinned structure — and out on a frame its skin cache is
    /// not ready, never at its bind pose.
    int  instances = 0;
    /// THE GPU SKIN CACHE (PHOTON-SKIN-1, RY-R4). WHAT IT DELIVERS: the posed
    /// character to every ray — its silhouette (a mirror's hit test, a contact
    /// shadow) and, since PHOTON-HIT-SHADE-1, its SHADING: a hit on a rigged item
    /// always goes to the hit decode, which reads the instance's skin row, so a
    /// reflection shows the POSED triangles lit by the scene's own lighting text
    /// (the voxels still hold the bind pose — SKIN-2's diffuse feed is owed).
    /// `skinnedInstances`: rigged
    /// items in the traced set this frame (each over its OWN structure, built
    /// from its posed vertices); `skinCaches`: caches held (one per rigged traced
    /// item, and its row block in the GPU scene); their bytes — the posed vertex
    /// buffers with their previous-pose slices (48 + 12 bytes a vertex) and the
    /// per-item structures (also inside `blasBytes`).
    int  skinnedInstances = 0;
    int  skinCaches = 0;
    unsigned long long skinCacheBytes = 0;
    unsigned long long skinBlasBytes = 0;
    /// Cumulative: items re-skinned (one per item per frame its POSE moved —
    /// a walk without a pose change costs none), skin dispatches (one per frame
    /// that re-skinned anything), skinned structures built (once per cache) and
    /// REFIT (once per pose change after that).
    unsigned long long skinPasses = 0;
    unsigned long long skinDispatches = 0;
    unsigned long long skinBlasBuilds = 0;
    unsigned long long skinRefits = 0;
    /// The last skin dispatch: items and vertices it wrote, and the GPU
    /// milliseconds of the dispatch and of the skinned builds/refits after it
    /// (the monitor's rows "rq.skin.dispatch" / "rq.skin.build": -1 unless a
    /// capture is running, and until one has been measured — lane TEST-1).
    int  skinLastItems = 0;
    unsigned long long skinLastVertices = 0;
    float skinMs = -1.0f;
    float skinRefitMs = -1.0f;
    /// CPU milliseconds the last skin pass cost the frame thread (reconcile, the
    /// palettes, the uploads, the recording) — -1 until one ran.
    float skinCpuMs = -1.0f;
    /// Why rigged items are NOT traced (empty when they are): no device address
    /// support, the job missing from the media, a source layout the job cannot
    /// read.
    std::string skinReason;
    /// The FAR copies (ATOM-FARBLAS-1): the same objects again over their
    /// meshes' coarsest levels, mask `kRayMaskFar`. The top-level structure
    /// holds `instances + farInstances`.
    int  farInstances = 0;
    /// Of `blasCount` / `blasBytes`: the structures built from a level ABOVE 0
    /// (every chained mesh's coarsest, and any near level the ray rule
    /// coarsened) — the memory the far field and the rule add.
    int  levelBlasCount = 0;
    unsigned long long levelBlasBytes = 0;
    /// Triangles in the bottom-level structures (unique geometry, not
    /// instanced).
    int  triangles = 0;
    /// Bytes of acceleration structure resident: the bottom level after
    /// compaction plus the top level.
    unsigned long long blasBytes = 0;
    unsigned long long tlasBytes = 0;
    /// GPU milliseconds of the LAST top-level build or refit — the monitor's
    /// "rq.tlas" row: -1 unless a capture is running, and until one has been
    /// measured (lane TEST-1).
    float tlasMs = -1.0f;
    /// GPU milliseconds of the last batch of bottom-level builds ("rq.blas"), the
    /// same reading; a still scene never rebuilds one.
    float blasMs = -1.0f;
    /// CPU milliseconds of THE INSTANCE UPDATE's host side — since ATOM-CPU-WALKS-1
    /// the instances are written ON THE DEVICE (the instance job rq_tlas_write.comp, one
    /// thread per slot, from the GPU scene's table), so this is the per-MESH work
    /// (missing structures' wants, the job's inputs when they changed) and the
    /// dispatch: it no longer scales with the instance count.
    float gatherMs = -1.0f;
    /// The instance job's dispatches, ever, and the slots the change feed has told
    /// this tier about (the traced set, its structures and words kept per change —
    /// a still frame adds none).
    unsigned long long tlasJobDispatches = 0ull;
    unsigned long long feedSlotVisits = 0ull;
    /// THE LAST FRAME'S HOST WORK, COUNTED (ENGINE-UPGRADE-U5 stage 3; engine.ray_update_counts): the hash
    /// lookups the tier's update made (its BLAS map, its key and mesh indices) and the table rows it wrote
    /// (the job's per-mesh records, the alpha table's slot rows, the hit table's light and material rows).
    /// Both are 0 in a frame in which nothing the tables describe changed, and O(changes) in one that did:
    /// the tables are patched by what changed, never rebuilt.
    unsigned long long updateLookups = 0ull;
    unsigned long long updateRows = 0ull;
    /// The same frame's WHOLE PASSES, in elements visited (decision 14.4): a moved frame's one pass over
    /// the bottom-level structures (the status totals), a mesh-set change's one pointer compare per
    /// mesh-table entry (the table's diff: the GPU scene names no changed entry), the eviction sweep's
    /// pass (once per 600 frames) and a feed reset's. 0 on an idle frame; at most one visit per
    /// structure plus one per entry on a moved one.
    unsigned long long updateVisits = 0ull;
    /// THE CUT-OUT MASKS (decision 14.2): masks made, ever (the tier's), and the masks of an OLDER
    /// content the last frame served because its budget of kAlphaMasksPerFrame was spent - each one
    /// keeps the table pending, so n stale masks are remade within ceil(n / 8) frames.
    unsigned long long alphaMasksMade = 0ull;
    unsigned long long alphaStaleServed = 0ull;
    /// THE HIT TABLE AS PUBLISHED (FAR-SUN-2; decision 14.1's count): the lights it holds and the material
    /// rows - a deleted lamp leaves it the frame after its removal.
    unsigned hitLights = 0u, hitMaterialRows = 0u;
    /// The hit table's ring buffers made, ever (decision 16.2: a written-every-frame table re-makes none
    /// once its three slots exist).
    unsigned long long hitTableBuffers = 0ull;
    /// THE EMITTER TABLE (PHOTON-7 stage 11, 11-1a: the reflection's NEE targets, published in the hit table): the
    /// emissive traced instances it holds (-1: no table made yet), those left out by the tier's cap
    /// (GiQualityFacts::reflectEmitters), the serial of its SET (moves when an emitter enters, leaves or changes its
    /// radiance - never on a move), and the table itself as uploaded, ordered by slot: the slot, the world bounding
    /// sphere (the instance's world box's) and the row's emissive radiance.
    int reflectEmitters = -1;
    unsigned reflectEmittersOver = 0u;
    unsigned long long reflectEmitterSetSerial = 0ull;
    struct ReflectEmitter {
        unsigned slot = 0u;
        float centre[3] = {};
        float radius = 0.0f;
        float radiance[3] = {};
    };
    std::vector<ReflectEmitter> reflectEmitterTable;
    /// THE SKY's PORTALS (PHOTON-7 11-2 (6)): the openings the voxel sides' escape shares show, as C2's sky NEE targets
    /// (the emitter form: the quad's bounding sphere, the sky's weight; slot 0xFFFFFFFF) - their count and the table
    int reflectPortals = 0;
    /// THE PICK's RING (PHOTON-7 11-2, the arm reflect.pick): 64 slots of 64 words, slot = the frame % 64 - words 0-31 the
    /// trace's sample (rq_reflect.comp jahPickSample), 32-47 the temporal pass's rest terms (rq_reflect_temporal.comp)
    std::vector<uint32_t> reflectPick;
    std::vector<uint32_t> reflectPickFrames;   ///< ...each slot's dispatch frame
    /// THE COLOUR PICK's RING (BUGS-8, the arm colour.pick): 256 slots of 128 words, a frame a slot - word 0 the fragments
    /// recorded at the picked pixel, then four records of 31 words (JahColourPick_piece_ps.any's JahColourPickTail)
    std::vector<uint32_t> colourPick;
    std::vector<uint32_t> colourPickFrames;    ///< ...each slot's engine frame (the stillness door's engine_frame)
    std::vector<ReflectEmitter> reflectPortalTable;
    /// THE NEE's CONE HIT RATE (PHOTON-7 stage 11, 11-1b; the lead's condition on the cone strategy): the reflection's NEE
    /// rays and those whose first hit was the target drawn, summed over the views' retired frames (read several frames
    /// late). Their ratio is the cone's efficiency (a thin or edge-on emitter's falls; under ~25 % its cone is owed the
    /// instance's oriented box).
    unsigned long long reflectNeeRays = 0ull, reflectNeeHits = 0ull;
    /// THE REFLECTION'S REST SAMPLE LAW (PHOTON-7 stage 11, 11-1c; the final read's M1): at rest frame 2N (the hand-over
    /// done) a texel still converging needs v_need = v ( e / share )^2 samples to come within its share (its standard
    /// error e over v samples, the stop's bar its share); `reflectRestNeedMax` the largest v_need (over 65,535 PREDICTS a
    /// capped texel). Its stop window opens at the crossing and closes after 300 samples AND 8 frames, at most 64 a frame,
    /// so it stops at v <= v_need + max( 300 + 64, 8 x 64 ) = v_need + 512; `reflectRestStopVMax` the largest v at which a
    /// texel stopped past 2N - the law: reflectRestStopVMax <= reflectRestNeedMax + 512 (the residual: v_need is the 2N
    /// estimate of e - a heavy-tailed texel whose e rises after it owes more, the stop law's own delta). The FRAMES a hold
    /// takes are the tail's ray budget's (the shared rate, the ramp, a batch ended at a record), never bounded here.
    /// 0 until this rest's 2N has been read back.
    unsigned reflectRestNeedMax = 0u, reflectRestStopVMax = 0u;
    /// True when the last top-level update was a REFIT rather than a full
    /// rebuild. The default is a rebuild (NVIDIA's own guidance for a TLAS;
    /// 0.5 ms at 8k instances buys the better tree); the refit is the
    /// optimisation behind the transform-epoch gate.
    bool lastWasRefit = false;
    /// THE HIT DECODE (PHOTON-HIT-SHADE-1): a ray hit no cache can shade (a
    /// mover, a rigged item, a static hit neither card nor cascade answers, the
    /// gather's far copies) is appended to the view's hit list and shaded by
    /// HlmsAtom's decode. Over the scene's LIVE views (a view whose read-backs
    /// stopped is left out): `hitRecords`: records the ray jobs appended in the
    /// last read-back frame (summed; may pass the capacity), `hitDropped`: of
    /// those, dropped because a view's own list was full (their rays have no sample
    /// that frame — a stat, never a crash; 0 = every live view's list held its own
    /// demand), `hitCapacity`: the list's size (the largest view's; the list grows
    /// on an overflow, kHitListWidth's law), `hitDecodeDraws`: the decode draws the scene
    /// holds (one per decode BUCKET — HlmsAtom::BucketKey, S3-DRAW). Read several
    /// frames late, never with a wait.
    unsigned long long hitRecords = 0;
    unsigned long long hitDropped = 0;
    unsigned long long hitCapacity = 0;
    /// THE BLENDED PASSES' ESCAPE LISTS (PHOTON-7 10-4 R4): a blended or refractive fragment's open specular share, traced
    /// on the TLAS after its pass. Over the scene's LIVE views (the hit list's rule), the newest read-back frame:
    /// `escapeRecords` the records the fragments appended (summed over the blended and the refractives' lists; may pass
    /// the capacity), `escapeDropped` of those, the ones a full list could not hold (lit by the environment instead, the
    /// list grows to the demand: 0 = every list held its own), `escapeCapacity` the largest view's two lists' size; then
    /// what answered each traced record: a card, the cascades, the hit's direct light alone (past every cache: no indirect
    /// - a named limit), a far copy's (the coarsest level: its normal faces the ray - a named limit), the environment, a
    /// back face, and no sample (a hit no reader could shade: 0 expected). Read several frames late, never with a wait.
    unsigned long long escapeRecords = 0;
    unsigned long long escapeDropped = 0;
    unsigned long long escapeCapacity = 0;
    unsigned long long escapeTraced = 0;
    unsigned long long escapeCard = 0;
    unsigned long long escapeVoxel = 0;
    unsigned long long escapeDirect = 0;
    unsigned long long escapeFar = 0;
    unsigned long long escapeSky = 0;
    unsigned long long escapeBack = 0;
    unsigned long long escapeNoSample = 0;
    /// ...and the records whose add SATURATED a pixel's fixed-point word (its top, 65536 a channel: the one read's F11 -
    /// a sum that once wrapped silently; 0 expected outside a sun's glint).
    unsigned long long escapeSaturated = 0;
    /// THE PANE LIST'S OVERFLOW (the one read's F4): the reading rays (a gather's, a reflection's, a card's, the voxel
    /// sides', the escape's) that met more panes than they list (jah_rq_alpha.glsl kJahPanesMax, the nearest kept): the
    /// rest are applied - their transmittance multiplied, their own light left out. Cumulative over the tier's life (every
    /// scene), read a few frames late; 0 = no ray met more. A light ray has no list (no cap) and is never counted.
    unsigned long long panesOverflow = 0;
    /// THE BOUND OF THE RAY QUERY LOOPS (GATHER-HANG-1 stage 2; jah_rq_alpha.glsl): the most trips any loop of the
    /// gather, the reflection, the glass escape, the cards' gather or the voxel sides published - a loop publishes only
    /// at the law (1024 trips) or past it, so 0 = no loop reached it (every real scene); 1024-4095 how deep the worst
    /// went; 4096 (the bound) = a loop was CUT, a bias made visible. Cumulative over the tier's life, read a few frames
    /// late. Under the test door rayquery.proceedPublishAll every loop publishes (the negative control).
    unsigned long long proceedStepsMax = 0;
    /// THE RAY's INPUTS (GATHER-HANG-1 stage 2; jah_rq_alpha.glsl JAH_RQ_INIT): the rays of those five jobs refused for an
    /// undefined input - a non-finite origin, direction, tMin or tMax (by the bits), tMin < 0 or tMin > tMax - each traced
    /// as an empty query (a miss). 0 in a healthy scene: a non-zero count is a red UPSTREAM of the ray. Cumulative over
    /// the tier's life, read a few frames late. The test door rayquery.nanRay makes it move.
    unsigned long long raysRefused = 0;
    /// THE HIT LIST PER LIVE VIEW (PHOTON-7 stage 0's read R4): the sums above cannot say WHICH view's list held its
    /// own demand (hitCapacity is the largest view's: beside a desktop view it never shows a stereo list). One entry per
    /// live view of the scene (the same LIVE rule), in no fixed order: `eyes` (2 = a stereo view, whose two eyes append
    /// to its ONE list), `eyeWidth` x `eyeHeight` (one eye's target), `firstCapacity` (the list the view's FIRST
    /// read-back frame held: its creation size, kHitListCreateShare x eyeWidth x eyeHeight x eyes rounded up to whole
    /// rows), and the newest read-back frame's `capacity` (THAT frame's list - its records and drops were counted
    /// against it, so a reading after a growth names the grown list), `records` and `dropped`; `photonTerm` = the view's
    /// BUILT chain carries the Photon term view's attachment and composite (ChainDesc::photonTerm: a term view on a post
    /// chain of either eye shape, PHOTON-7-9A M4).
    struct HitListView {
        unsigned eyes = 1u;
        bool photonTerm = false;
        unsigned eyeWidth = 0u, eyeHeight = 0u;
        unsigned long long firstCapacity = 0ull;
        unsigned long long capacity = 0ull;
        unsigned long long records = 0ull;
        unsigned long long dropped = 0ull;
    };
    std::vector<HitListView> hitViews;
    int hitDecodeDraws = 0;
    /// Cumulative counters over the scene's life: how many times the top level
    /// was rebuilt, refitted, and how many bottom-level structures were built.
    unsigned long long tlasBuilds = 0;
    unsigned long long tlasRefits = 0;
    unsigned long long blasBuilds = 0;
    /// RAY-TRACED REFLECTIONS (PHOTON_SPEC §7 R5). True when this scene's
    /// drawn views are tracing reflections — which needs `enabled`, a view
    /// whose SSR row is on (High and Epic; the SSR contract, not a second
    /// setting) and a chain that carries the SSR textures. With it false the
    /// reflections are the screen-space march alone, which is the picture this
    /// renderer drew before R5 existed.
    bool reflect = false;
    /// How many rays the last frame traced for reflections — one per pixel of
    /// the trace's own resolution (full at Epic, one in four at High), before
    /// the shader's own gates (the sky, the roughness band, a pixel the march
    /// already answered) decline most of them.
    int  reflectRays = 0;
    /// GPU milliseconds of the reflection: its trace's and its filter's monitor
    /// rows summed ("rq.reflect.trace" + "rq.reflect.filter"; the hit decode
    /// between them is a pass row of its own) — -1 unless a capture is running,
    /// and until measured.
    float reflectMs = -1.0f;
    /// GPU milliseconds of the screen march's OBJECT-MOTION job (REFLECT-MOVERS-1,
    /// rq_motion.comp, in front of the SSR resolve): the "rq.reflect.motion" row,
    /// the same reading; -1 too on a chain that carries no march or no id pass.
    float reflectMotionMs = -1.0f;
    /// THE REFLECTION HISTORY'S COUNT (PHOTON-2 stage 0; the arm "reflect.historyReadback") — an INSTRUMENT:
    /// per texel of the trace's grid, row-major (reflectHistoryW x reflectHistoryH, the eye layers side by side),
    /// the temporal mean's sample count (its state texel's N + v, whole; 0 = no answer this frame, capped at 4096)
    /// on traced frame `reflectHistoryFrame` of the view. Empty with the arm off and until a frame traced under it
    /// has retired. `reflectFrame` is the view's traced-frame counter NOW (the frame just traced is one below it),
    /// so a caller that steps frames knows which copy is which frame.
    std::vector<unsigned short> reflectHistoryCount;
    /// ...and THE MOVING SE (PHOTON-7 stage 6, the lag law's instrument), on the same grid and frame: each texel's moving
    /// mean's standard error in DISPLAY CODES (its luma variance over its frame count, through the denoiser space's
    /// slope and the display table) as the temporal pass left it on its last moving frame; empty where the copy is not
    /// there (the arm off, a frame before its size).
    std::vector<float> reflectHistorySe;
    unsigned reflectHistoryW = 0u, reflectHistoryH = 0u;
    unsigned reflectHistoryFrame = 0u;
    unsigned reflectFrame = 0u;
    /// THE REFLECTION'S REST (PHOTON-2 round 2: R1, G6) - a view's (the last in the map's order): its consecutive rest
    /// frames (0 = moving or broken), the frame was HELD (nothing traced), the newest read-back count of its texels
    /// still converging at rest (-1: none read for this rest since it began or a moved instance last crossed it), the
    /// rests it has begun, and the rest frames on which a moved instance's swept box was handed to the trace.
    unsigned reflectRestFrames = 0u;
    bool     reflectHeld = false;
    int      reflectRestActive = -1;
    /// ...and of them the CAPPED texels (commit 8): stopped at their sample count's 16-bit ceiling rather than by their
    /// standard error - the rest's defect reading (0 on a healthy rest; -1: none read, as reflectRestActive).
    int      reflectRestCapped = -1;
    unsigned reflectRests = 0u;
    /// ...AND THE LAST SHOT's REST (the shot-view fold, PHOTON-7 stage 10-3): a screenshot's shot view's rest frames, held
    /// flag and rest serial as it last recorded them, kept for the scene past the view's destruction until its next shot
    /// (reflectShotValid false = none yet) - what the shot photographed, which the live views no longer say.
    bool     reflectShotValid = false;
    unsigned reflectShotRestFrames = 0u;
    bool     reflectShotHeld = false;
    unsigned reflectShotRests = 0u;
    unsigned reflectRestMoverFrames = 0u;
    /// THE CAMERA CUTS a reflection view detected (N3, the gather's rule) - each one its histories cleared.
    unsigned reflectCameraCuts = 0u;
    /// THE RECORDINGS REFUSED for a chain not at its view's stereo shape (PHOTON-7-9A; the tier's, every view): a view's
    /// chain is rebuilt at its shape inside View::setStereo, so this is a DEFECT counter - 0 is the law; each refusal is
    /// also its frame's error, naming the view.
    unsigned long long reflectShapeRefusals = 0ull;
};

}}  // namespace jahshaka::engine
