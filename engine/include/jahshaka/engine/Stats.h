#pragma once
// THE ENGINE's STATS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <utility>
#include <vector>
#include "jahshaka/engine/Ids.h"

namespace jahshaka { namespace engine {

/// WHAT LEVEL ONE OBJECT IS ACTUALLY DRAWING (ATOM P1's readout, the gap OWN-TRI
/// left). The chain could not be SEEN working: `submittedTriangles` says the
/// scene shed triangles, and nothing said which object took which level.
///
/// `level` is the Item's own `mCurrentMeshLod` — what the LOD strategy last
/// wrote, i.e. what the render queue will index its VAO list with. `levels` is
/// how many that mesh has (1 = no chain), and `triangles` is what that level's
/// VAOs really hold, summed over the sub-meshes.
///
/// THE HONESTY NOTE: `mCurrentMeshLod` is one slot per object and EVERY pass that
/// updates LOD lists writes it — a planar reflector's mirrored camera, a PiP
/// inset, a probe cube face, a thumbnail. So this is "the level the last LOD
/// update chose", which in an ordinary editor frame is the main view's and in a
/// frame that also rendered a mirror may be the mirror's. A shadow map's caster
/// passes do NOT leave theirs behind (SHADOW-LOD-1: each walks LOD at its own
/// camera and the shadow node puts the view's levels back after its passes).
///
/// `cut` = the item rides the Atom queue: the view draws it through the id pass's
/// CLUSTER CUT, and no pass of the view walks its per-object level any more (the
/// fork's LOD walk skips the queues a pass skips — SHADOW-LOD-1). Such a row has
/// no view level: `level` is 0 and `triangles` level 0's (the authored mesh the
/// cut refines), and the Levels view (world.setAtomView) is what shows its cut.
struct ObjectLodDesc {
    NodeId             node = 0;
    std::string        name;
    unsigned           level = 0;
    unsigned           levels = 1;
    unsigned long long triangles = 0;
    bool               cut = false;
};

/// What the renderer measured this frame (STATS_OVERLAY_SPEC.md §4).
/// A POD, exactly like ShaderCacheStats — `app.renderStats()` is this struct.
///
/// PROCESS-WIDE, not per view. There is one Root and one render loop, so the
/// timings describe the loop; the geometry counters are the FRAME's running
/// totals at the end of the last camera's pass (OgreSceneManager::_renderPhase02
/// snapshots them per camera), which is why they are labelled "frame" and never
/// "this view". With two on-screen views the second view's numbers include the
/// first's — an approximation upstream makes, not one this boundary adds.
///
/// THE HONESTY NOTE that belongs beside every FPS number in this application:
/// the loop is a 16 ms QTimer (EngineRenderDriver), so `fps` measures the TIMER,
/// not the renderer. A scene that got twice as expensive but still fits inside
/// 16 ms reads the same ~62. The number that diagnoses anything is the host's
/// work-per-tick (app.frameStats().workMs), not this one.
struct RenderStats {
    /// True when the backend was recording geometry counters for the frames
    /// these numbers describe. Reading renderStats() turns recording ON (it is
    /// off by default in Ogre and costs integer adds per draw call), so the
    /// FIRST read reports false with zeroed counters and every read after a
    /// rendered frame reports true. Timings are valid either way.
    bool               metricsRecording = false;

    // ---- timing: Ogre::FrameStats, fed by Root::renderOneFrame ----
    double fps = 0.0;         ///< rolling average FPS — the number to display
    double frameMs = 0.0;     ///< rolling average frame time, ms
    double lastMs = 0.0;      ///< the latest inter-sample delta, ms (noisy)
    double p95Ms = 0.0;       ///< 95th percentile frame time, ms (0.4 fps buckets)
    double p99Ms = 0.0;       ///< 99th percentile frame time, ms
    double bestMs = 0.0;      ///< best frame since the counters were reset
    double worstMs = 0.0;     ///< worst frame since the counters were reset

    // ---- geometry: Ogre::RenderingMetrics, reset per frame ----
    unsigned long long draws = 0;
    unsigned long long batches = 0;
    unsigned long long triangles = 0;
    unsigned long long vertices = 0;
    unsigned long long instances = 0;
    /// HOW MANY FRAMES THE GEOMETRY COUNTS TRAIL THE PICTURE (ATOM S3-DRAW). 0 while
    /// every draw is decided on the CPU: Ogre counts what it submits. Where a view's
    /// visibility buffer draws, its objects and their levels are chosen ON THE GPU by
    /// the cull, and their share of the counts is read back from the cull's own
    /// counters once the frame that drew them has retired — never by stalling the
    /// frame for it — so after a change (a pose, the LOD dial) the counts settle this
    /// many frames later. A reader that compares counts across a change waits this.
    unsigned gpuCountLagFrames = 0;

    /// PSOs the LAST frame gave up on because it ran out of its compile budget
    /// (SPECS/THREADING_ADOPTION_SPEC.md P4(b), decision D-E(1)). Objects using
    /// an incomplete PSO do not appear that frame and are resubmitted for the
    /// next one.
    ///
    /// ALWAYS 0 IN THIS ENGINE, AND THAT IS THE POINT. The budget
    /// (RenderSystem::setPsoRequestsTimeout) is left at Ogre's default of 0,
    /// i.e. off, deliberately: upstream's own documentation warns that
    /// "techniques that rely on running a shader once (e.g. to fill a texture)
    /// may end up uninitialized", which is every thumbnail, IBL bake, VCT
    /// voxelization and offscreen pixel suite we have — and the knob is
    /// process-wide while the only thing worth protecting is one on-screen view.
    /// The counter costs one read, so it is reported anyway: a non-zero value
    /// means somebody turned the deadline on, and would be the first thing to
    /// look at if thumbnails ever came back half-drawn.
    unsigned           incompletePsoRequests = 0;

    /// FORWARD+ LIGHT CENSUS (LIGHTING_FIX fix 8 / F-F2) — the WORST case
    /// across the live scenes, because a cell overflow belongs to one scene and
    /// this struct is process-wide.
    ///
    /// Forward+ bins lights into screen-space cells and DROPS SILENTLY once a
    /// cell is full: `if( numLightsInCell->lightCount[0] < mLightsPerCell )`
    /// with no else, three times over (OgreForwardClustered.cpp:479/618/759).
    /// Nothing counts the drops and nothing exposes the per-cell counts, so an
    /// exact "lights lost this frame" cannot be reported without patching Ogre
    /// — deliberately not done for a diagnostic.
    ///
    /// What these three DO say, exactly: `forwardPlusLights` is how many lights
    /// the busiest scene puts through the clustered list at all (everything
    /// except directionals, which ride the pass buffer); `forwardPlusBudget` is
    /// the per-cell capacity they compete for. `forwardPlusOverBudget` is the
    /// excess, and it is a NECESSARY condition for a drop, not a sufficient
    /// one: zero PROVES no light was dropped anywhere, non-zero means a cell
    /// that saw every light would have dropped that many and the scene is worth
    /// looking at.
    unsigned           forwardPlusLights = 0;
    unsigned           forwardPlusBudget = 0;
    unsigned           forwardPlusOverBudget = 0;

    /// HOW MANY TIMES `Engine::advanceResources()` HAS RUN in this process
    /// (lane OPEN-FRAMES-1). Monotonic, never reset, and reported here because
    /// the question it answers belongs with the frame counters: "did the
    /// renderer's resource bookkeeping move while nothing was being drawn?"
    ///
    /// Only differences mean anything. A host whose long install runs without
    /// frames proves it is still recycling by watching this rise across the
    /// install — which is exactly what `open.frames` asserts.
    unsigned long long resourceAdvances = 0;
};

/// One SHADOW-MAP SLOT, as the renderer currently holds it
/// (`world.shadowStatus()`). A slot is a LIGHT's place in the atlas, not a
/// texture rectangle: slot 0 is the sun (the primary directional) and owns all
/// three PSSM splits, and slots 1..N are the focused maps, one light each: every
/// further casting directional first, then the point/spot lights.
/// That is Ogre's own bookkeeping (CompositorShadowNode::getShadowCastingLights
/// is indexed by light, not by map), and reporting anything else here would
/// mean inventing a second numbering the engine does not use.
struct ShadowMapInfo {
    unsigned slot = 0;        ///< 0 = the sun's PSSM slot, 1..N the focused maps
    NodeId   node = 0;        ///< the light occupying it, 0 when the slot is empty or foreign
    /// The lamp-map cache holds this slot's light (every point/spot map is
    /// cached — ENGINE_CACHE_POLICY_SPEC P2 — unless the view has more lamps
    /// than maps, when the node stays on Ogre's closest-first dynamic sort).
    bool     isCached = false;
    bool     dirty = false;   ///< a cached map scheduled to re-render on the next frame
    bool     pssm = false;    ///< the directional slot (three splits) rather than a focused map
    /// Shadow-node passes the last frame spent on this slot's map(s) in the
    /// counted view — 0 for a cached lamp at rest (8 when it re-renders: a
    /// clear, six cube faces and the copy for a point light).
    unsigned passesLastFrame = 0;
};

/// WHAT THE SHADOW ATLAS ACTUALLY IS, as opposed to what was asked for
/// (SPECS/SHADOW_TOOLING_SPEC.md §7) — the shape `GiStatus` established: a
/// readback of what the renderer ACHIEVED, cheap enough to poll, and the only
/// way anything (a panel, a test, a script) can tell "this light has no shadow
/// map" from "this light casts no shadow".
///
/// PROCESS-WIDE, like the resolution and the filter: there is one atlas.
struct ShadowStatus {
    /// False when there is no atlas to describe — a headless engine, or before
    /// the first view exists. Every number below is then zero.
    bool     live = false;
    unsigned resolution = 0;     ///< the base size the layout derives from
    unsigned maps = 0;           ///< TEXTURE rectangles in the atlas: pssmSplits + focusedMaps
    unsigned pssmSplits = 0;     ///< always 3 here: the sun, three splits
    unsigned focusedMaps = 0;    ///< single maps the atlas has room for (point/spot, further directionals)
    /// LIGHT slots — 1 (the directional/PSSM set) + focusedMaps, and the length
    /// of `mapped`. Not the same number as `maps`: three of the rectangles
    /// belong to one light.
    unsigned lightSlots = 0;
    /// The focused-map casters in the scenes being drawn (shadow-casting
    /// point/spot lights and every casting directional after the sun) — the
    /// demand the count is derived from. `casters > focusedMaps` is the
    /// exceeded case.
    unsigned casters = 0;
    unsigned budget = 0;         ///< the EFFECTIVE ceiling (resolution-capped)
    unsigned requestedBudget = 0;///< what the host asked for before the cap
    unsigned atlasWidth = 0, atlasHeight = 0;
    /// The depth atlases' bytes (D32): the view's atlas (atlasWidth x
    /// atlasHeight) PLUS every live planar-mirror slot's (half resolution, the
    /// same focused count since D3 — `reflectAtlasBytes` is that part). The
    /// reflection probes' quarter-resolution atlases, one per shadowed probe,
    /// are `probeAtlasBytes`, not included here. NOT included anywhere: the
    /// point-light cube scratch (R^2 x 6 R32F + depth per instance).
    unsigned long long atlasBytes = 0;
    unsigned long long reflectAtlasBytes = 0;
    unsigned long long probeAtlasBytes = 0;
    /// Every light SLOT the live shadow node holds, in slot order.
    std::vector<ShadowMapInfo> mapped;
    /// Focused-map casters (as `casters`) with NO map this frame — the lights
    /// whose shadows are silently missing. Empty is the healthy state.
    std::vector<NodeId> unmapped;
    /// WAS THE LAST RENDERED FRAME MEASURED AT ALL?
    ///
    /// The pass counters below are OPT-IN and the opt-in EXPIRES: asking for a
    /// status attaches a callback per compositor pass per frame, and 120
    /// rendered frames without a read detach it again. So the counters have two
    /// distinct zeroes — "the frame rendered no shadow passes", which is a
    /// measurement, and "nobody was counting", which is not.
    ///
    /// They used to be indistinguishable, and the first read after a quiet
    /// spell reported a hard 0 for a scene plainly casting shadows. A suite read
    /// that as a document write being lost (lane SKY-SMALL's diagnosis); the
    /// only reason it had ever worked was that the World panel's shadow rows
    /// poll the same status on every rebind, so an unrelated UI refresh kept the
    /// counters armed.
    ///
    /// FALSE here means every `*PassesLastFrame` / `*RendersLastFrame` /
    /// `mapsDirtiedLastFrame` field below is UNMEASURED, not zero. Ask again
    /// after one more rendered frame — the first ask is what arms them. The
    /// scripting layer reports them as `null` in that state.
    bool countersMeasured = false;
    /// THE COST READINGS OF THE LAST RENDERED FRAME (counted only while
    /// somebody polls — see Engine::shadowStatus, and `countersMeasured` above,
    /// which says whether these are a reading at all). `shadowPassesLastFrame`
    /// = the shadow-node passes the COUNTED view executed (the first enabled
    /// view with shadows; 0 when none has any), and `cachedMapRendersLastFrame`
    /// how many of those re-rendered a CACHED lamp map: zero at rest, which is
    /// what "renders once" means, measurably.
    unsigned shadowPassesLastFrame = 0;
    unsigned cachedMapRendersLastFrame = 0;
    /// THE CACHE'S SELF-CHECK, cumulative for the session: passes hashed while
    /// the shadow node declared FEWER shadow maps than its own light list
    /// indexes — the state that generates a pixel shader which cannot compile
    /// (fork 6130df9d1 (was 0025) removes its cause; this counts any recurrence). Zero is
    /// the only healthy value, and a suite may assert exactly that.
    unsigned shaderLightMismatches = 0;
    /// The same for the planar mirrors' shadow nodes (every budget slot) and
    /// the reflection probes' (every shadowed probe that captured) — total
    /// passes, and the part spent on point/spot maps. A probe capture renders a
    /// dirty lamp map on its FIRST face and reuses it on the other five.
    unsigned reflectPassesLastFrame = 0;
    unsigned probePassesLastFrame = 0;
    unsigned reflectLampPassesLastFrame = 0;
    unsigned probeLampPassesLastFrame = 0;
    /// The lamp-map cache last frame (ENGINE_CACHE_POLICY_SPEC P2-P5): shadow-
    /// node instances (views, planar slots, probes) whose lamps are cached,
    /// instances left on the dynamic sort because the lamps outnumber their
    /// maps (the v1 over-budget rule), whether the counted view's own node is
    /// caching, and how many lamp maps were marked to re-render.
    unsigned cachedInstances = 0;
    unsigned uncachedInstances = 0;
    bool     viewCached = false;
    unsigned mapsDirtiedLastFrame = 0;
    /// HOW MANY TIMES THIS PROCESS HAS REBUILT THE SHADOW ATLAS, cumulative.
    /// The hitch counter: a rebuild swaps the three shadow-node DEFINITIONS,
    /// which means dropping and recreating EVERY workspace that names one —
    /// every view, every planar-mirror slot, and every reflection probe, whose
    /// GI arm is then built from scratch. It is the single most expensive thing
    /// the shadow system does and it is invisible in every other reading, so a
    /// test can hold it still: a world that opens with casting lamps costs ONE
    /// (the count and the clear strategy settle together) and none afterwards.
    /// Monotonic for the life of the engine; the monitor's `shadow.atlas` event
    /// is the same fact with a reason attached.
    unsigned atlasRebuilds = 0;
    /// ITEM VISITS MADE BY THE CASTER WALK, cumulative over every live scene
    /// (ENGINE-4 F5). The lamp-map cache has to notice a caster that moved,
    /// posed, changed shape or stopped casting, and it does it by walking the
    /// scene's items once a frame. That walk is O(items) and it used to run on
    /// EVERY frame of every drawn scene with a cacheable lamp — including
    /// frames in which nothing at all had happened. It is now gated on the same
    /// kind of epoch the GI movement scan uses (a transform write, or one of
    /// the pushed events that change a caster without moving it), so a STILL
    /// frame visits nothing: this number holding still across rendered frames
    /// IS the statement, and it is the one a test or the monitor can read.
    /// (Since U0 3b a moving frame visits only the items its ChangeSet and the
    /// engine's seams named — a mover frame adds the mover, not the scene.)
    unsigned long long casterWalkItems = 0;
};

/// A CENSUS of everything alive behind the boundary (fps audit F11).
/// A POD, exactly like RenderStats — `app.engineObjects()` is this struct.
///
/// WHY IT EXISTS. RenderStats says what a frame COST; nothing said what the
/// renderer was HOLDING. A leak on this side of the boundary — a view that is
/// created per readback and never destroyed, a mesh record kept after the
/// document node died, a datablock per material push — is invisible in pixels,
/// invisible in the document, and shows up only as a frame that costs a little
/// more every minute. These counts are flat in a scene nobody is editing, so
/// "sample 2 equals sample 6" is a contract a test can assert without a
/// per-machine baseline (tests/perf/epic_steady_state.js).
///
/// SCOPE. `views` and `scenes` are the engine's own vectors. `nodes`, `meshes`,
/// `materials` and `textures` are the per-Scene registries SUMMED over every
/// live scene — they are ids the boundary handed out and still honours, not
/// Ogre objects. `datablocks` is the one PROCESS-WIDE number: HlmsManager keeps
/// one datablock map per Hlms type for the whole Root, so it counts across
/// scenes by construction (and includes the backend's own defaults, which is
/// why only its DELTA means anything).
/// What the renderer's MEMORY POOLS hold (Engine::memoryStats, riders lane R4).
/// Two halves with two different owners:
///   * the GPU half is the VaoManager's buffer pools (every vertex/index/const
///     buffer and, on Vulkan, every texture — `gpuPoolsIncludeTextures`),
///     straight from VaoManager::getMemoryStats. `gpuPoolFreeBytes` is what is
///     ALLOCATED FROM THE DRIVER and not handed out; a pool that becomes
///     entirely free is returned to the driver by the Vulkan VaoManager on
///     its own, a few frames later (deallocateEmptyVbos from _update — this
///     pin's cleanupEmptyPools() THROWS ERR_NOT_IMPLEMENTED and is never
///     called by this engine). Watch capacity fall after a project closes.
///   * the SIMD half is the scene managers' SoA node/object pools, which grow
///     to the high-water mark of nodes ever alive and only shrink on
///     Engine::reclaimMemory (SceneManager::shrinkToFitMemoryPools). The pin
///     exposes no byte count for them (the per-depth ArrayMemoryManagers are
///     private), so the rows are the counts that drive them plus the
///     process's resident set, which is where a shrink shows.
struct MemoryStats {
    unsigned long long gpuPoolCapacityBytes = 0;  ///< bytes the VaoManager holds from the driver
    unsigned long long gpuPoolFreeBytes = 0;      ///< of which unused (fragmentation + empty pools)
    unsigned           gpuPools = 0;              ///< pool count
    bool               gpuPoolsIncludeTextures = false;
    unsigned           sceneManagers = 0;         ///< walked: every Scene + the document staging manager
    unsigned           simdNodes = 0;             ///< live scene nodes (both roots walked), summed — what the node pools hold
    unsigned           simdNodeSlots = 0;         ///< the node pools' USED slots, summed: simdNodes + the holes non-LIFO removals left until each level's cleanup
    unsigned           simdObjects = 0;           ///< live objects in the entity + light SoA pools, summed
    unsigned           simdNodeDepths = 0;        ///< node-hierarchy depth pools, summed (one SoA pool each)
    unsigned long long residentBytes = 0;         ///< the process RSS (Linux; 0 elsewhere)
};

/// ONE TEXTURE the renderer holds (app.textureMemory, lead 2026-09-09): the
/// attribution behind MemoryStats::gpuPoolCapacityBytes when textures share
/// the pools. `bytes` is the texture's own footprint (every mip, every
/// slice, MSAA counted); a pooled texture (`pooled`) lives in a slice of a
/// master array texture whose waste it does not carry. `residency` is the
/// engine's word: "OnStorage" (declared, nothing on the GPU), "OnSystemRam",
/// "Resident".
struct TextureMemoryEntry {
    std::string name;            ///< the alias the engine created it under
    std::string resource;        ///< the file or resource name behind it ("" for RTTs / manual)
    unsigned    width = 0, height = 0, depth = 0, slices = 0;
    unsigned    mipmaps = 0;
    unsigned    msaa = 1;
    std::string format;          ///< pixel format name
    unsigned long long bytes = 0;
    bool        renderTarget = false;
    bool        uav = false;
    bool        manual = false;  ///< uploaded by us (TextureFlags::ManualTexture)
    bool        pooled = false;  ///< AutomaticBatching: a slice of a pool master
    std::string residency;
};

struct ObjectCounts {
    unsigned views = 0;         ///< live View objects (on-screen + offscreen)
    unsigned enabledViews = 0;  ///< of those, the ones renderOneFrame draws
    unsigned scenes = 0;        ///< live Scene objects
    /// Of those, how many the LAST frame updated — the scenes an enabled View
    /// draws (THREADING_ADOPTION_SPEC.md P3). The frame loop walks exactly
    /// these; every other scene manager in the process, including the
    /// document's staging ones, is skipped. `updatedScenes < scenes` in the
    /// editor is the NORMAL, wanted state; `updatedScenes == scenes` with
    /// several preview pages alive means the gate stopped working.
    unsigned updatedScenes = 0;
    unsigned nodes = 0;         ///< tracked node records, summed over scenes
    unsigned meshes = 0;        ///< tracked mesh records, summed over scenes
    unsigned materials = 0;     ///< tracked material records, summed over scenes
    unsigned textures = 0;      ///< tracked texture records, summed over scenes
    unsigned datablocks = 0;    ///< Hlms datablocks in the process (all types)
};

/// WHAT THE ENGINE IS THREADING (SPECS/THREADING_ADOPTION_SPEC.md P1).
/// `app.threading()` is this struct. It exists because the single most
/// expensive failure mode of the threading program is SILENT: an engine built
/// with a stale CMake cache keeps single-threaded shader compilation, every
/// build still succeeds, every test still passes, and the editor compiles its
/// shaders on one core for ever. This is the read-back that makes that
/// falsifiable from a script.
struct EngineThreading {
    /// `RenderSystem::supportsMultithreadedShaderCompilation()` — the backend's
    /// own answer, not our guess. False means the parallel Hlms compile queue
    /// never starts (OgreRenderQueue.cpp:388) and HlmsDiskCache::applyTo runs
    /// its serial branch whatever thread count it is given.
    bool multithreadedShaderCompilation = false;
    /// The build-time mode this engine was compiled with, decoded from the two
    /// OgreBuildSettings.h macros the CMake option sets:
    ///   0 = disabled           (BACKWARDS_COMPATIBLE_API, no TLS)
    ///   1 = compatible API     (BACKWARDS_COMPATIBLE_API + TLS, static builds)
    ///   2 = force-enabled      (no BACKWARDS_COMPATIBLE_API)
    /// 0 and 1-without-TLS are indistinguishable in the header, so a shared
    /// build reports 1 for both; what matters operationally is
    /// multithreadedShaderCompilation above.
    unsigned shaderThreadingMode = 1;
    /// Per-scene worker pools, by scene name — the threads Ogre forks culling,
    /// transforms, bounds and (after mode 2) shader compilation across. The
    /// engine's staging scene managers are not Scenes and are not listed.
    std::vector<std::pair<std::string, unsigned>> sceneWorkerThreads;
    /// The largest of those counts: the ceiling on how many threads any single
    /// pass can compile shaders on, and the count the disk cache's applyTo is
    /// given at load. 1 means "serial no matter what the flag says".
    unsigned hlmsThreads = 1;
};

}}  // namespace jahshaka::engine
