#pragma once
// THE ENGINE's MONITOR TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>
#include "jahshaka/engine/Config.h"

namespace jahshaka { namespace engine {

// ---------------------------------------------------------------------------
// THE RENDER-LOOP MONITOR (SPECS/RENDER_LOOP_MONITOR_SPEC.md)
// ---------------------------------------------------------------------------
// A DATA COLLECTOR, not a judge. The owner's words: "the purpose of the monitor
// is to collect as much data as possible for you to be able to review the core
// engine and how it's running, to look for issues and problems." It records
// what the frame did and WHY, and nothing in it computes a verdict: no budgets,
// no thresholds, no 'wasted' flags. Redundant work appears as work whose
// `reason` is `None`, which is data; whether that is a defect is the LEAD's
// reading, made from a capture, not the engine's.
//
// THREE RULES THIS BOUNDARY ENCODES:
//  1. OFF BY DEFAULT, ZERO COST WHEN OFF. At MonitorLevel::Off no listener is
//     attached to any workspace, no clock is read, no ring is allocated and no
//     GPU query pool exists (MonitorStatus reports all four, and a suite
//     asserts them).
//  2. FORWARD ONLY. Nothing is recorded until a capture starts; there is no
//     background history to look back at.
//  3. NO ON-SCREEN OUTPUT, EVER. The monitor writes records the host drains; it
//     never draws, because drawing would cost frame time and passes and
//     contaminate what it measures.

/// A FAULT INJECTED INTO ONE FRAME — TEST-FACING, NEVER A SHIPPING PATH
/// (`Engine::setFrameFault`, lane FRAME-CATCH-1, 2026-09-18).
///
/// The subject is the engine's own frame CLOSE: a frame that throws still owes
/// the OpenXR runtime an `xrEndFrame` (with the eye swapchain images it
/// acquired), owes the monitor a closed record, and must still reach the
/// device-lost latch. None of that can be asserted without a frame that
/// throws, and nothing a test can legally ask the engine to do throws out of a
/// frame on demand — so the engine can be asked for one.
///
///   * `Throw` — the next `frames` frames raise an Ogre exception from inside
///     the frame, AFTER the render has been recorded (and, in a session, after
///     the eye copies have acquired their swapchain images) and before
///     anything closes: the exact position a `VK_ERROR_DEVICE_LOST` from the
///     frame's commit occupies.
///   * `ThrowDeviceLost` — the same, and the frame ALSO reports the device as
///     lost to the engine's own latch. It is the only way `deviceLost()` can
///     become true without a real loss, and it fakes nothing else: the render
///     system, the driver and any live VR session are untouched (a real loss
///     cannot be induced on demand, and inducing one would take the box's GPU
///     down with it).
///   * `VulkanOutOfDeviceMemory` (lane FORK-OOM-1) — the same position, but the
///     throw is the VULKAN RENDER SYSTEM'S OWN: the frame calls the function
///     every failed Vulkan call goes through (`Ogre::onVulkanFailure`, what
///     `checkVkResult` expands to) with the live device and
///     `VK_ERROR_OUT_OF_DEVICE_MEMORY`, so what decides whether the device is
///     latched as lost is the fork's real code, not a copy of it. Nothing is
///     allocated and nothing is faked: the device stays valid, and a frame
///     after it must render as if nothing happened.
///   * `VulkanDeviceLost` — the same call with `VK_ERROR_DEVICE_LOST`. This one
///     REALLY latches the render system (no GPU is touched, but from here on
///     it vetoes every frame, exactly as after a real loss): the process
///     cannot render again and the host must end it. A suite arms it LAST.
///   * `VulkanOutOfDeviceMemoryInRecord` — the same OOM call, raised INSIDE the
///     frame's recording (from an Ogre frame listener's `frameRenderingQueued`,
///     inside `Root::_updateAllRenderTargets`, before the swap). Ogre's frame
///     state is not whole after that, so it is TERMINAL (`GpuFault`): the
///     engine renders nothing more and the host must end the process.
/// The Vulkan kinds need the Vulkan render system linked into the engine
/// (`JAH_RAY_QUERY`); without it they raise the plain `Throw`.
enum class FrameFault { None, Throw, ThrowDeviceLost, VulkanOutOfDeviceMemory, VulkanDeviceLost,
                        VulkanOutOfDeviceMemoryInRecord };

/// A GPU FAULT THE SESSION CANNOT CONTINUE FROM (`Engine::gpuFault`, lane
/// FORK-OOM-1). Two facts, kept apart so a log and a test rig never class one
/// as the other:
///   * `DeviceLost` — the render system reported VK_ERROR_DEVICE_LOST
///     (`Engine::deviceLost`). The device is gone.
///   * `OutOfMemoryInFrame` — a Vulkan out-of-memory happened INSIDE a frame's
///     work (its pending rebuilds, its scene update or its recording), whoever
///     caught it. The device is NOT lost, but Ogre's frame state is not whole
///     after a failure half-way through recording (measured: the next frame
///     dereferenced null in the render-pass setup), so the engine stops
///     rendering. An OOM OUTSIDE a frame (a resource created between frames)
///     is an ordinary error the caller sees, and the engine continues.
enum class GpuFault { None, DeviceLost, OutOfMemoryInFrame };

/// Which part of the frame a compositor pass belonged to. The classifier reads
/// the pass's parent NODE (the three shadow nodes are named constants) and the
/// workspace's owner, so it is exact rather than inferred from timings.
enum class PassBucket {
    Other,          ///< anything unclassified — a warm-up workspace, a mipmap chain
    Main,           ///< the view's own camera scene pass
    Post,           ///< the view's post chain (quads, computes, resolves)
    ShadowView,     ///< the view's shadow node (kShadowNodeName)
    ShadowReflect,  ///< a planar mirror's shadow node (kReflectShadowNodeName)
    ShadowProbe,    ///< a reflection probe's shadow node (kProbeShadowNodeName)
    Planar,         ///< a planar-mirror reflection render
    ProbeFace       ///< a reflection-probe cube face
};

/// One compositor pass, as executed. `draws`/`batches`/`triangles`/`instances`
/// are the DELTA of the render system's own metrics across the pass, so summing
/// a frame's passes reproduces `RenderStats::draws` for that frame exactly —
/// which is what the suite asserts.
///
/// ZERO DRAWS DOES NOT ALWAYS MEAN "NOTHING WAS DRAWN", and this is Ogre's
/// plumbing rather than the monitor's. MEASURED (lane MON-P1a, 2026-09-12, the
/// engine suite's offscreen rig): a frame whose view renders a ground, a cube
/// and the overlays reports the view's own scene pass as `draws = 0`, while the
/// shadow node's cube-face caster passes in the SAME frame report 1 each — so
/// the frame's total is 3 on a frame that re-rendered a lamp map and 0 on the
/// idle frames after it, with an identical, complete picture every time. In the
/// app the main pass does report draws (`scripting.e2e.render_stats` asserts
/// `draws > 0`), so the under-count is configuration-dependent and was NOT
/// root-caused here; it is reported upstream-ward rather than guessed at.
/// The monitor reports the render system's own numbers faithfully; analysis
/// must therefore read a zero as "the renderer counted nothing here", never as
/// "this pass drew nothing". `MonitorRecord::metricsRecording` says whether the
/// counters were live at all.
struct FramePass {
    std::string workspace;        ///< the workspace instance's definition name
    std::string node;             ///< the parent compositor node's name
    std::string pass;             ///< the definition's profiling id, else its type
    PassBucket  bucket = PassBucket::Other;
    /// The shadow map index a shadow pass renders; kNoShadowMap otherwise.
    unsigned    shadowMapIdx = 0xFFFFFFFFu;
    static constexpr unsigned kNoShadowMap = 0xFFFFFFFFu;
    unsigned    draws = 0, batches = 0, instances = 0;
    unsigned long long triangles = 0;
    /// TRUE when the pass never reported its end (a workspace update closed
    /// with it still open). Its times and counts are unknown, not zero:
    /// `cpuMs` is negative. Seeing one of these is itself the finding.
    bool        orphaned = false;
    float       cpuMs = 0.0f;
    /// SCENE PASSES ONLY: of this pass's wall time, how much went on its shadow
    /// node's update (its own nested pass records included) — the shadow-vs-
    /// scene split, taken from `passSceneAfterShadowMaps`. NEGATIVE on every
    /// pass that is not a scene pass (quads, clears, computes, resolves). ZERO
    /// on a scene pass whose shadow node executed nothing — which is exactly
    /// what a fully cached lamp set looks like, and is why the zero matters as
    /// much as the number. Ogre fires the callback on every scene pass, a
    /// shadow node's own caster passes included, so those carry it too.
    float       shadowMs = -1.0f;
    /// GPU milliseconds from timestamp queries. NEGATIVE means NOT MEASURED —
    /// the build has no JAH_GPU_TIMESTAMPS, the device has no usable
    /// timestamps, or the result has not come back yet. Never faked as 0.
    float       gpuMs = -1.0f;
    /// A TOP-LEVEL row (not nested in another pass's timestamp pair: a shadow node's passes are their
    /// scene pass's children) — the rows whose gpuMs add up to the frame's.
    bool        topLevel = false;
    /// ITS OWN TIME (PHOTON-7's naming step): gpuMs less its DIRECT children's (the passes and rows whose pairs it
    /// encloses) - the work no child names; valid when `selfMeasured`. Within the timestamps' rounding (kSelfEpsilonMs)
    /// of 0 it reads 0; MORE NEGATIVE means the children sum past their parent - an overlap or a double count - and it is
    /// REPORTED as it stands, never clamped. `selfSubmitted`: the render system SUBMITTED inside the row's own stretches
    /// (outside its children), so its own time holds the GPU's wait for the CPU (a bubble) - not unnamed GPU work.
    /// CacheWork carries the same three.
    static constexpr float kSelfEpsilonMs = 1e-3f;
    float       selfGpuMs = 0.0f;
    bool        selfMeasured = false;
    bool        selfSubmitted = false;
    /// ...and the UPLOAD STAGING submitted inside its own stretches (a staging buffer fenced or waited: MonitorRecord's
    /// staging counts) - the submission's cause, not only its place.
    bool        selfStaging = false;
    /// THE MEASURED BUBBLE (PHOTON-7 stage 10, urgent #64; the fork's JahGpuBubbleResults): the GPU's idle across
    /// the submissions made while the row was open, from each flush's own timestamp pair (the flushed buffer's last
    /// BOTTOM_OF_PIPE to the next buffer's first TOP_OF_PIPE, never below 0) - `bubbleGpuMs` the whole row's,
    /// `selfBubbleGpuMs` its own stretches' (the whole less its direct children's). EXACTLY 0 where no flush crossed
    /// it; NEGATIVE = a bubble the fork could not measure (no query room, a pair never completed), never faked as 0.
    /// `insideSubmitted`: a submission anywhere inside the row (its own stretches or a child's, any depth).
    float       bubbleGpuMs = 0.0f;
    float       selfBubbleGpuMs = 0.0f;
    bool        insideSubmitted = false;
    /// THE PASS READS THE GI (ENGINE-UPGRADE-ASYNC-3 stage 2; the engine's passReadsGi: a scene pass that
    /// is not a caster pass, or a custom pass, whose definition is not marked as sampling none). The
    /// frame's first and last such row bound the async compute window.
    bool        readsGi = false;
};

/// The caches whose work and reason the monitor records (§4.7).
enum class CacheKind {
    Probe,      ///< a reflection-probe capture (units = cube faces)
    ShadowMap,  ///< a shadow map render (units = passes; id = the light's NodeId)
    Gi,         ///< the GI volume: voxelize / IR trace / IFD converge
    Planar,     ///< a planar reflector's render (view-dependent: always justified)
    Shader,     ///< a shader/PSO compile (detail = the permutation)
    Texture,    ///< a texture load (units = bytes/1024, detail = the name)
    Atmosphere, ///< one of the planet's atmosphere's tables (detail = the job)
    /// The Atom GPU cull's jobs (detail = "id.cull.cut", "caster.cull.test", ...):
    /// per-frame VIEW work, not a cache — a row so its GPU time is attributable
    /// (lane TEST-1), never "the GI did work".
    Cull,
    /// A recording view's colour pass and readback copy (VIDEO-REC-1; detail
    /// "video.nv12"): per-frame view work while a recording runs, never a cache.
    Video
};

/// WHY a cache redid its work. `None` is the important value: the cache did the
/// work with no recorded input change. That is recorded as data and judged by
/// nobody here.
///
/// The names come from the invalidation causes that already exist in the
/// engine — `GiStaleReason` (including `Mobility`, lane R1), the lamp-map
/// cache's per-light dirty keys (lane E2) and the probe sweep's own bookkeeping
/// — so a reason is a REPORT of what the engine decided, never a second guess.
enum class WorkReason {
    None,        ///< no recorded input change (the redundant-work value)
    Build,       ///< first build / the arm was created this frame
    Rebuild,     ///< a full teardown-and-rebuild
    Refresh,     ///< an explicit refresh request (refreshGlobalIllumination)
    Sweep,       ///< the round-robin budget's turn came up (no input changed)
    Moved,       ///< the owner itself moved
    Caster,      ///< a caster moved or changed inside its range/volume
    Light,       ///< a light moved or changed
    Material,    ///< a material or texture changed
    Sky,         ///< the sky changed
    Ambient,     ///< the ambient term changed
    Fog,         ///< fog changed
    Mobility,    ///< an object's mobility class changed (R1)
    Added,       ///< an object was added
    Removed,     ///< an object was removed
    Bounds,      ///< the volume/region bounds changed
    Resolution,  ///< resolution, quality or budget changed (an atlas rebuild)
    Camera,      ///< the camera moved (planar reflections; a view-dependent cache)
    Permutation, ///< a shader permutation was seen for the first time
    Request      ///< the host asked for it directly
};

/// One cache's work in one frame, with its reason.
struct CacheWork {
    CacheKind  cache  = CacheKind::Probe;
    WorkReason reason = WorkReason::None;
    /// Probe index, light NodeId, planar slot — 0 when the cache is global.
    unsigned long long id = 0;
    /// Free text the analysis reads: the permutation, the texture name, the
    /// shadow node kind, the GI phase.
    std::string detail;
    /// Faces captured, maps rendered, compiles, kilobytes — per `cache`.
    unsigned   units = 0;
    /// Milliseconds, or NEGATIVE when this work was not timed separately
    /// (it is inside the pass records instead).
    float      ms = -1.0f;
    /// GPU milliseconds for this work's OWN dispatches, from the render
    /// system's timestamp queries (fork 1a81f866a+1bccc3f93 (was 0027)). NEGATIVE means NOT
    /// MEASURED — the build has no JAH_GPU_TIMESTAMPS, the device has no
    /// usable timestamps, the work ran outside a recorded frame, or the result
    /// has not come back yet. Never faked as 0.
    ///
    /// It is the ONLY GPU time in a capture that does not come from a
    /// compositor pass: a GI voxelisation, a light injection and an irradiance
    /// field's integration are compute dispatches the compositor never sees,
    /// so they are absent from `MonitorRecord::gpuMs` (which stays the sum of the
    /// frame's passes — the invariant a suite asserts) and appear only here.
    float      gpuMs = -1.0f;
    /// A TOP-LEVEL row (its timestamp pair inside no pass's and no other row's), as FramePass::topLevel: a row
    /// dispatched inside a compositor pass (the reflection's filter in its resolve pass) is inside that pass's time,
    /// so a frame's GPU is the TOP-LEVEL passes and rows summed (`jahlab monitor`, PHOTON-7).
    bool       topLevel = false;
    /// ...and its own time and whether it submitted inside it (FramePass::selfGpuMs, selfMeasured, selfSubmitted).
    float      selfGpuMs = 0.0f;
    bool       selfMeasured = false;
    bool       selfSubmitted = false;
    bool       selfStaging = false;   ///< (FramePass::selfStaging)
    float      bubbleGpuMs = 0.0f;       ///< (FramePass::bubbleGpuMs)
    float      selfBubbleGpuMs = 0.0f;   ///< (FramePass::selfBubbleGpuMs)
    bool       insideSubmitted = false;  ///< (FramePass::insideSubmitted)
};

/// One stage of the frame, exclusive of its children.
struct FrameStage {
    std::string name;          ///< "engine.pre", "engine.record", "engine.swap", host stages
    float       ms = 0.0f;
};

/// Why this frame was rendered. Set by the caller through
/// `FrameInput::cause` (Engine::submitFrame) — the driver tick, a script's `editor.frame`, an
/// offscreen readback, the warm-up gate — so analysis can tell a frame nobody
/// saw from one the owner watched.
enum class FrameCause { Unknown, Driver, Scripted, Offscreen, WarmUp, Player };

enum class FramePace {
    /// Do everything this frame needs, now. The default and the contract.
    Complete,
    /// A DRIVER FRAME OF A WORLD THE USER CAN SEE that still owes first-time
    /// work: take ONE step of it and draw. Repeated once per frame until
    /// nothing is owed, which is what makes the world stream in.
    Streaming
};

/// WHICH STAGE OF A WORLD'S FIRST LIGHTING ARM IS STILL OWED
/// (SPECS/OPEN_COVER_SPEC.md §2 A). The public mirror of the scene's own stage
/// machine, in the order the stages run; `None` means no staged build is in
/// flight. It exists so the HOST can name what a frame spent — the loading
/// indicator's "lighting…" and the slow-frame line's cause — without a monitor
/// capture running and without reaching into the engine's private types.
enum class GiArmStage : unsigned char {
    None,
    ProbeScout,    ///< one photograph of the space, to place the grid in
    ProbeFit,      ///< the placement over every candidate, then keep/drop
    ProbeFinish,   ///< clamp, re-create at the real resolution, the closing capture
    Field          ///< the irradiance field and the volume bookkeeping
};

struct StreamingWork {
    /// Stages of the staged first arm build still to run, summed over every
    /// scene (0 = none owed). 4 the moment a build is staged, then 3, 2, 1.
    unsigned   giStagesLeft = 0;
    /// The stage that will run NEXT — what "lighting…" is actually doing.
    GiArmStage giStage = GiArmStage::None;
    /// Materials bound to a texture whose pixels have not landed yet: they draw
    /// with a fallback until `settleTextureResidency` swaps the real one in.
    unsigned   materialsAwaitingTexture = 0;
    /// `ShaderCacheStats::compiledThisRun`, without the directory scan — see
    /// the note above. A RUNNING TOTAL for the process: a caller that wants
    /// "did the last frame compile anything" differences it across frames.
    ///
    /// `expectedShaders` is deliberately NOT here. It is the PREVIOUS
    /// session's whole total — everything it compiled or read from the cache,
    /// boot included — so as a denominator for "how far through this load are
    /// we" it is not merely imprecise, it is a different quantity.
    unsigned   shadersCompiled = 0;
    /// WHOLE-ARM GI REBUILDS this process has done, staged and unstaged alike
    /// (`OgreScene::rebuildVct`'s own counter, summed over the live scenes).
    /// Differenced across a frame it is the honest answer to "did the lighting
    /// arm run in that frame", which the stage counter above cannot give: a
    /// `Complete` frame builds the WHOLE arm and never touches a stage.
    unsigned long long giRebuilds = 0;
};

/// EVERYTHING ONE `renderOneFrame` DID. One of these per frame while a capture
/// runs; the host pops them with `popMonitorRecords` and writes them to
/// `frames.jsonl`.
struct MonitorRecord {
    unsigned long long frame = 0;    ///< the engine's monotonic frame counter
    /// THE HAND-OVER'S SERIAL (ENGINE-UPGRADE-U3): the record this frame applied (FrameInput::serial). The host's half
    /// of the record — its stages, noted on the thread that submits — rode THAT hand-over, so the two halves are one
    /// record joined by this number, never one thread writing into the other's.
    unsigned long long serial = 0;
    /// Milliseconds since the capture started, at the frame's first stage mark.
    double      startMs = 0.0;
    float       totalMs = 0.0f;      ///< the whole renderOneFrame, wall clock
    FrameCause  cause = FrameCause::Unknown;
    bool        onscreen = false;    ///< an enabled WINDOW view took part
    unsigned    scenesUpdated = 0;
    /// THE FRAME'S INPUT, as its drain found it (U0 3c: the monitor reads the record): the change set's entries over
    /// every scene, whether a scene's gesture was active, and the fields the drain applied (FrameInputStatus).
    unsigned    inputMoved = 0;
    bool        inputGesture = false;
    unsigned    inputFieldsApplied = 0;
    std::vector<FrameStage> stages;
    std::vector<FramePass>  passes;
    std::vector<CacheWork>  cacheWork;
    // ---- counters (all for THIS frame) ----
    unsigned    draws = 0, batches = 0, instances = 0;
    unsigned long long triangles = 0;
    unsigned    probeCaptures = 0;      ///< probe cube faces captured
    /// Photon cascade re-voxelisations in THIS frame. The scheduler's contract
    /// is "at most one per frame", and this is what makes that assertable from
    /// a bundle without parsing the `vct.cascadeN` rows out of cacheWork.
    unsigned    cascadeRebuilds = 0;
    unsigned    shadowPasses = 0;       ///< the view's shadow node
    unsigned    shadowPassesReflect = 0;///< planar mirrors' shadow nodes
    unsigned    shadowPassesProbe = 0;  ///< probes' shadow nodes
    unsigned    planarRenders = 0;
    unsigned    shaderCompiles = 0;
    /// Program loads failed with a compile error, the process's running total at this frame's end
    /// (AsyncShaderStats::compileErrors): any value above 0 is a dead shader in the running app.
    unsigned long long shaderCompileErrors = 0;
    /// THE SCENES WHOSE FRAME WAS INCOMPLETE (PublishedFrame::Completeness, COLD-BOOT-1 item 3): how many of the scenes this
    /// frame drew, and the longest run of incomplete frames among them - a scene stuck incomplete (never at rest) shows here.
    unsigned    incompleteScenes = 0;
    unsigned long long incompleteFramesMax = 0;
    /// Was the render system COUNTING while this frame rendered? Recording is
    /// off in Ogre until something asks for `renderStats()`, and a frame
    /// rendered with it off reports zeros for every geometry counter — a fact
    /// `frames.jsonl` states outright so analysis never has to infer it.
    bool        metricsRecording = false;
    /// Passes that were closed by a workspace boundary rather than by their own
    /// `passPosExecute` (see `FramePass::orphaned`). Zero is the only value
    /// seen so far; a non-zero one means this frame's pass tree is incomplete.
    unsigned    orphanedPasses = 0;
    float       textureWaitMs = 0.0f;   ///< the frame-head streaming drain
    /// Σ of the TOP-LEVEL passes' GPU milliseconds, or NEGATIVE when unmeasured.
    /// A pass's `gpuMs` is INCLUSIVE (its timestamp pair encloses the shadow
    /// node's passes it executes), so the nested ones are inside their parent's
    /// time and are not added twice.
    float       gpuMs = -1.0f;
    /// THE FRAME'S OWN GPU SPAN (lane TEST-1, the perf audit's F5): one timestamp
    /// pair opened when the pools turn over at the close of the previous frame
    /// (so the between-frames work this record adopts — a readback, a one-shot
    /// sky bake — is inside it) and closed at this frame's close (after the VR
    /// eye copy). A SPAN, first command to last: the GPU's idle time between
    /// submissions is inside it (gpuIdleMs). NEGATIVE when unmeasured.
    float       frameGpuMs = -1.0f;
    /// THE GAPS between top-level rows, measured: the monitor times every stretch
    /// in which none of its rows is open, and splits them by whether the CPU
    /// SUBMITTED inside the stretch (Ogre's command buffer changed under it):
    /// `gpuIdleMs` = the gaps that crossed a submission — the GPU waiting for the
    /// CPU to submit more (a mid-frame flush: the irradiance field's raster
    /// submits every 8 probes) — a frame-schedule reading, not a coverage hole;
    /// `unattributedGpuMs` = the gaps inside one submission — GPU work no row
    /// names, the coverage hole `scale.gpu_coverage` bounds against the BUSY span
    /// (frameGpuMs - gpuIdleMs). A row that itself encloses a submission carries
    /// its bubble in its own time, MEASURED (rowBubbleGpuMs below) and counted in
    /// `gpuIdleMs` beside the gaps', so the busy span holds no wait for the CPU.
    /// NEGATIVE when unmeasured, a gap's sample was lost or a row's bubble was.
    float       gpuIdleMs = -1.0f;
    float       unattributedGpuMs = -1.0f;
    /// THE MEASURED BUBBLES INSIDE THE TOP-LEVEL ROWS (PHOTON-7 stage 10, urgent #64: each flush's own timestamp
    /// pair, FramePass / CacheWork bubbleGpuMs) summed over the frame's top-level passes and rows - the part of
    /// `gpuIdleMs` that lies inside rows (a nested row's bubble is inside its top-level ancestor's). NEGATIVE when one
    /// could not be measured (gpuIdleMs then too).
    float       rowBubbleGpuMs = -1.0f;
    /// THE WIDEST UNNAMED GAP (PHOTON-7's naming step) and where it lies: "<the row before>|<the row after>" (`frame`
    /// at either end of the frame's pair) - so a coverage hole names its neighbours. Negative / empty when none.
    float       widestGapMs = -1.0f;
    std::string widestGap;
    /// GPU timing MARKS this frame issued that the render system's query pool
    /// had no room for (a pass or a dispatch with no GPU time because of it,
    /// never because it cost nothing). Non-zero means THIS frame's GPU numbers
    /// are incomplete. Per frame, so a capture that drains every 250 ms still
    /// sees every frame that dropped one (POST-C-FIXES-1).
    unsigned    gpuMarksDropped = 0;
    /// THE UPLOAD STAGING's EVENTS this frame (PHOTON-7's walk hitch; the fork's VulkanStagingBuffer counters, read at
    /// the frame's close): the submissions that fenced an upload region (`stagingFlushes`: the unfenced bytes past the
    /// buffer's quarter, or a ring wrap), the wraps, the CPU's waits on the GPU for a region (`stagingWaits`; those that
    /// submitted first; their ms) and the bytes mapped for upload. Zero when the render system has none.
    unsigned    stagingFlushes = 0, stagingWraps = 0, stagingWaits = 0, stagingWaitSubmits = 0;
    float       stagingWaitMs = 0.0f;
    unsigned long long stagingBytes = 0ull;
    /// ...and THE GRAPHICS QUEUE's mid-frame submissions (the fork's VulkanQueue counters): the FlushOnly submissions,
    /// those that parked a fence on the frame slot being recorded, and the SELF-DRAINS - the command buffer opened after
    /// one waiting that slot's fences, i.e. the CPU waiting on the frame's own submitted work - with their ms.
    unsigned    queueFlushes = 0, queueFencedFlushes = 0, queueSelfDrains = 0;
    float       queueSelfDrainMs = 0.0f;
    /// What the monitor itself cost this frame, so analysis can subtract it.
    float       overheadMs = 0.0f;
};

/// ONE MEASUREMENT ARM (lane TEST-1, the perf audit's A2): a named switch the
/// engine reads ONCE PER FRAME — a value set between two frames takes effect from
/// the next one whole, never half-way through it — registered with its default.
/// It replaces the measuring doors that were process-environment reads inside
/// per-frame code (`getenv("JAH_R5_NO_MOTION")` ...): two arms of an A/B are two
/// calls in ONE process (trap 12's paired arms), and the switch set is a table a
/// harness can list instead of a grep. NOT a mode: the default is the shipped
/// picture, and nothing in the product sets an arm.
struct ArmInfo {
    std::string name;            ///< "reflect.motion" — the verb's key
    double      value = 0.0;     ///< what the current frame reads
    double      defaultValue = 0.0;
    double      minValue = 0.0, maxValue = 0.0;
    std::string what;            ///< what the arm changes, and the suite that measures it
    bool        test = false;    ///< a TEST DOOR (a fixture's, through Engine::setArm): the scripting verb engine.arm refuses it
};

/// THE VALIDATION LAYER'S PROOF (TESTING-CLEANUP-2 H4). A row that runs "under the Khronos
/// validation layer" and greps its log for "Validation Error" proves nothing when the layer never
/// loaded: no layer, no error, green. Seven rows asked for the layer and four of them could not tell
/// a layered run from an unlayered one. This is the readout every layered row asserts: whether the
/// process asked the loader for the layer, whether the DEVICE's own entry points really resolve
/// into the layer's library (the layer intercepts vkGetDeviceProcAddr when it is active — the
/// probe tests/atom used alone, generalised), and which layer libraries the process loaded.
struct ValidationStatus {
    /// The process environment asks the Vulkan loader for the validation layer
    /// (VK_INSTANCE_LAYERS or VK_LOADER_LAYERS_ENABLE names it).
    bool requested = false;
    /// The layer is LIVE: its library is loaded in the process (the loader maps it only for an
    /// instance that enables it) and — on a build that reaches the Vulkan device — the device's
    /// vkCmdDraw resolves INTO VkLayer_khronos_validation. False before the first view (Ogre makes
    /// the instance and the device with it) and with the NULL render system.
    bool active = false;
    /// Every Vulkan layer library loaded in the process (file names, e.g.
    /// libVkLayer_khronos_validation.so) — what the loader really loaded, whatever was asked.
    std::vector<std::string> layers;
    /// The library the device's vkCmdDraw resolves into ("" with no Vulkan device).
    std::string drawEntry;
};

/// A discrete thing that happened, with its cause.
enum class MonitorEventKind {
    GiRebuild,        ///< a GI arm was torn down and rebuilt
    GiRefresh,        ///< every probe / the voxel volume was invalidated
    ProbeGridBuild,   ///< the PCC probe grid was (re)placed
    AtlasRebuild,     ///< the shadow atlas changed shape (workspaces recreated)
    WorkspaceRebuild, ///< a view's workspace was recreated
    ShaderCompile,    ///< a permutation was compiled (label = the permutation)
    TextureLoad,      ///< a texture finished loading (value = bytes)
    VramFlush,        ///< a deferred-free flush / device memory was reclaimed
    DeviceLost,       ///< the device was lost
    ViewDestroyed,
    Host              ///< the host's own hook: page switches, UI gaps, script marks
};

/// One event. Times share the frame records' clock.
struct MonitorEvent {
    MonitorEventKind kind = MonitorEventKind::Host;
    unsigned long long frame = 0;
    double      startMs = 0.0;
    float       ms = -1.0f;          ///< duration, negative when instantaneous
    WorkReason  reason = WorkReason::None;
    std::string label;               ///< the host's label, a permutation, a path
    std::string detail;
    unsigned long long value = 0;    ///< bytes, counts — per `kind`
};

/// What the monitor is doing right now — and the four assertions that make
/// "zero cost when off" checkable from a test rather than believed.
struct MonitorStatus {
    MonitorLevel level = MonitorLevel::Off;
    /// Monitor listeners attached across EVERY live workspace (view, planar,
    /// probe). MUST be 0 at Off.
    unsigned attachedListeners = 0;
    unsigned ringCapacity = 0;       ///< 0 at Off: the ring is freed, not kept
    unsigned ringFrames = 0;         ///< records waiting to be drained
    unsigned pendingEvents = 0;
    unsigned long long framesRecorded = 0;
    /// Records the ring overwrote because nobody drained fast enough. Non-zero
    /// is a fact about the host's drain rate, not a failure.
    unsigned long long framesDropped = 0;
    /// EVENTS the event list could not hold (its capacity is fixed and it drops
    /// rather than grow). On the boundary for the same reason `framesDropped`
    /// is: a capture bundle certifies itself complete or incomplete, and it
    /// cannot do that honestly if the host cannot see what the engine dropped
    /// (lane MON-P1b review, 2026-09-13 — a bundle claimed completeness while
    /// events had been dropped past the cap).
    unsigned long long eventsDropped = 0;
    // ---- GPU timing (P1c), with BOTH its off-switches visible ----
    bool     gpuCompiled  = false;   ///< the engine was built with JAH_GPU_TIMESTAMPS
    bool     gpuSupported = false;   ///< ...and the device/backend can do timestamps
    bool     gpuActive    = false;   ///< ...and a capture has a query pool open NOW
    unsigned gpuQueryPools = 0;      ///< MUST be 0 outside a capture
    /// GPU timing MARKS the query pool could not hold, CUMULATIVE since this
    /// capture turned the monitor on (the per-frame count rides each
    /// MonitorRecord::gpuMarksDropped). Non-zero means the capture's GPU numbers
    /// are INCOMPLETE — said out loud rather than left for analysis to notice
    /// that some passes have no time. Cumulative because the host reads this
    /// on its drain timer, and a LAST-FRAME reading (what this field used to
    /// be) missed every dropping frame between two drains. Measured on Grand
    /// Showroom 2 at Epic (POST-C-FIXES-1): the worst frame issued 1,214 marks
    /// (2,428 of the pool's 4,096 queries — 96 probe faces at updateBudget 64,
    /// rays off); 0 dropped in every arm.
    unsigned long long gpuMarksDropped = 0;
    /// FRAMES PUBLISHED BEFORE EVERY GPU SAMPLE ANSWERED, cumulative since the
    /// capture started. A record waits until the GPU has finished its frame
    /// (MONITOR-RETIRE-1); one held past 16 newer frames (a lost device took
    /// the query pools) is published with its passes unsampled and counted
    /// here. 0 on a healthy capture, however heavy the frames.
    unsigned long long gpuFramesAgedOut = 0;
    std::string gpuReason;           ///< why GPU timing is unavailable, when it is
    float    overheadMs = 0.0f;      ///< the monitor's own cost, last frame
};

/// ONE SYNCHRONOUS GETTER'S HOST-SIDE CALLS (host.sync_getters, ENGINE-UPGRADE-U3 stage 2; Engine::syncGetters): a
/// const virtual that still reads the engine's state on the caller's thread, by name ("Scene::giStatus"), with the
/// calls made outside every frame of the render loop since the process started. Only getters called at least once
/// are listed.
struct SyncGetterCount {
    std::string        name;
    unsigned long long calls = 0;
};

}}  // namespace jahshaka::engine
