#pragma once
// THE ENGINE's SHADERS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <vector>

namespace jahshaka { namespace engine {

/// THE BACKGROUND SHADER COMPILER (ASYNC-SHADERS-1). After a project is open no
/// compile may freeze the window: a view in asynchronous mode (View::setAsyncShaders)
/// hands a permutation it has not built yet to the engine's compile service, which
/// builds it on its own threads OUTSIDE the frame, and draws the object with a neutral
/// grey placeholder (its own geometry, lit like the scene; shadows too) until it lands.
/// Everything here is process-wide and cheap (atomic reads and a few sums).
struct AsyncShaderStats {
    /// The service is running (EngineConfig / setAsyncShaderThreads) and with how many
    /// threads. 0 threads = every view compiles inside the frame, as before.
    bool               running = false;
    unsigned           threads = 0;
    /// Permutations submitted and not landed yet (queued + building + built and waiting
    /// for the next frame to publish them). The status bar's "Compiling shaders (N)".
    unsigned           pending = 0;
    /// Running totals for the process: jobs that finished building, jobs that failed
    /// (the log names the permutation; the object keeps its placeholder).
    unsigned long long completed = 0;
    unsigned long long failed = 0;
    /// Draws made with the placeholder / skipped because not even the placeholder was
    /// built yet — running totals summed over every scene. A warm selftest reads 0 and 0.
    unsigned long long placeholderDraws = 0;
    unsigned long long pendingSkips = 0;
    /// Draws inside the surface cache's captures that met a shader still building — those
    /// capture batches are discarded and retried (the atlas never holds a grey card).
    unsigned long long captureDeferredDraws = 0;
    /// Shaders compiled ON THE SERVICE'S THREADS (running total). Subtract it from
    /// `shaderBuildProgress`'s `compiled` and what remains is every compile a frame
    /// waited for — the live-compile sentry's number (SHADER-WARM-2).
    unsigned           compiledInBackground = 0;
    /// Shaders compiled on any OTHER thread (a frame waited): one counter, read in one load
    /// (the host's live-compile sentry reads this, never compiled - compiledInBackground).
    unsigned           compiledInForeground = 0;
    /// Compute dispatches REFUSED because a pipeline's parameters did not fit its program
    /// (the fork's HlmsCompute::getJahParamsMisfits, checked at a pipeline's first dispatch):
    /// 0 on a sound build - shader.async_after_open asserts it (PHOTON-7 stage 5: two compile
    /// threads built one shared program's parameter definitions at once, and a dispatch read
    /// past its parameters until the GPU lost its context).
    unsigned long long paramsMisfits = 0;
    /// Program loads that FAILED with a compile error, every program of the process (a running total: the fork's
    /// GpuProgram::getJahCompileErrors). Ogre catches and logs a failed load and never throws it, so a compute job
    /// whose shader does not compile never dispatches and the picture is merely wrong: 0 on a sound build - the
    /// engine selftest's verdict (URGENT #83: a piece left out of the light injection's list killed the job silently).
    unsigned long long compileErrors = 0;
    /// WHAT A FRAME BUILT, AND WHAT THE COMPILERS COST (COLD-BOOT-1 stage 3; the lead's ruling 8: counters in the published
    /// status, never log scrapes). Running totals for the process, fed by the fork (its HlmsCompute and its Vulkan render
    /// system, SHADER-SYSTEM-1 3F):
    ///   computeParsedInFrame - compute permutations whose TEMPLATE PARSE ran inside a frame (a compute record's hit
    ///                          skips it: a warm launch's frames read 0);
    ///   computePsoInFrame    - compute pipelines built inside a frame, any path (a parse, a record's hit, a clone);
    ///   computeDestroyWaits / computeDestroyWaitMs - a compute job destroyed while its permutation was COMPILING: the
    ///                          frame waited for that compile to end (a clone's teardown), and how long in all;
    ///   computeJoins / computeJoinMs - a compute dispatch that JOINED its permutation's in-flight background build
    ///                          (a frame that owes its picture waited for the service's result: SHADER-SYSTEM-1 7a R2;
    ///                          stage 7 brings the joins on the frame thread to 0), and how long in all;
    ///   glslangMs            - the time in glslang (source to SPIR-V), every thread summed, and its count;
    ///   graphicsPsoMs / computePsoMs - the time in the driver's pipeline creation (vkCreateGraphicsPipelines /
    ///                          vkCreateComputePipelines through Ogre), every thread summed, and their counts.
    /// (The stage-1 table these replace: spikes/cold-boot-1/S1b, read there by an LD_PRELOAD probe.)
    unsigned long long computeParsedInFrame = 0;
    unsigned long long computePsoInFrame = 0;
    unsigned long long computeDestroyWaits = 0;
    double             computeDestroyWaitMs = 0.0;
    unsigned long long computeJoins = 0;
    double             computeJoinMs = 0.0;
    unsigned long long glslangCompiles = 0;
    double             glslangMs = 0.0;
    unsigned long long graphicsPsos = 0;
    double             graphicsPsoMs = 0.0;
    unsigned long long computePsos = 0;
    double             computePsoMs = 0.0;
    /// WHAT A FRAME BUILT, GRAPHICS AND LOW-LEVEL (SHADER-SYSTEM-1 stage 7a; the fork's Hlms::getJahBuiltInFrame /
    /// getJahCompiledInFrame and HlmsLowLevel::getJahLowLevelBuiltInFrame), running totals for the process:
    ///   graphicsBuiltInFrame    - Hlms permutations built in a frame (the main thread or a blocking pass's compile
    ///                             worker, while the background compiler runs): a pipeline the frame waited for;
    ///   graphicsCompiledInFrame - of them, those whose shader COMPILED in the frame (the rest: a PSO from cached code);
    ///   lowLevelBuiltInFrame    - low-level material pipelines (the post chain's quads - SMAA, the tonemap - and the
    ///                             /Multiview clones): always built in the frame that first draws them.
    /// After the startup gate the editor's frames move none of them: the gate draws what the editor draws.
    unsigned long long graphicsBuiltInFrame = 0;
    unsigned long long graphicsCompiledInFrame = 0;
    unsigned long long lowLevelBuiltInFrame = 0;
    /// THE GATE's LAW (COLD-BOOT-1 stage 4): joins of a ray pipeline build that was NOT finished - a frame or a wait that
    /// blocked on a driver compile (a running total; the teardown's joins are not counted). After the startup gate and
    /// in every editor frame it reads 0: the gate waits on `pending`, never on a join.
    unsigned long long rayBuildsJoinedWaiting = 0;
    /// Of `pending`, the ray pipeline builds in flight (the rest is the background compiler's): the startup gate's
    /// world phases wait for these before their consumers' frames (COLD-BOOT-1 stage 4).
    unsigned           rayBuildsInFlight = 0;
    /// Compute dispatches DEFERRED because their permutation was building in the background (a running total, the
    /// fork's HlmsCompute::getNumDeferredDispatches): a frame that moved it did not do all its work.
    unsigned long long computeDeferred = 0;
    /// THE RAY PIPELINE BUILDS THAT LANDED (SHADER-SYSTEM-1 stage 7): ahead builds joined, a failure included (a running
    /// total; the teardown's joins are not counted). Launched = rayBuildsLanded + rayBuildsInFlight.
    unsigned long long rayBuildsLanded = 0;
    /// THE BUILDS' BURST, "n of m" (SHADER-SYSTEM-1 stage 7; the notice's ONE fact, every host reads it - the app's
    /// toolbar indicator, jahlab's overlay and LAB BUILDING lines, the headset's surround): a burst BEGINS when the pending
    /// count rises from 0, `landed` counts its landings (a failure is a landing), `total` = landed + pending; when pending
    /// returns to 0 it reads m of m until the next begins (engine/src/BuildBurst.h). `shaders` - the background
    /// compiler's jobs (pending minus the ray builds; a landing is a job RESOLVED - published or discarded: the fork's
    /// HlmsAsyncCompiler::getJahNumResolved); `rays` - the ray pipeline builds.
    struct Burst {
        unsigned landed = 0;
        unsigned total = 0;
    };
    Burst shaderBurst, rayBurst;
};

/// THE ENGINE's SHADER IDENTITY, NO DEVICE (SHADER-SYSTEM-1 stage 6a; WARM-SHARE-1's one source, shaderIdentity() in
/// Engine.h): what the shader cache keys on that THE BUILD decides - each layer's key over its build terms only (the GPU's
/// join at the first view's load and are the cache's own: a layer they move is dropped there and named in
/// ShaderCacheStats::layersNotLoaded) - and the engine's two content ids. Equal on every box that runs one build.
struct ShaderIdentity {
    std::string pipelineKey, microcodeKey, hlmsKey, computeKey;   ///< hex: each layer's key over its build terms
    std::string fingerprint;     ///< hex: the four composed (ShaderCacheStats::fingerprint before the GPU's terms join)
    std::string engineBuildId;   ///< JAHSHAKA_ENGINE_BUILD_ID: the engine's C++ and public headers
    std::string raySpirvId;      ///< JAHSHAKA_RAY_SPIRV_ID: the ray tier's SPIR-V ("none": the tier compiled out)
};

/// What the persistent shader cache did this run, and what is on disk
/// (SHADER_CACHE_SPEC.md §4.5). `app.shaderCache()` is this struct.
struct ShaderCacheStats {
    /// False when EngineConfig::shaderCacheDir was empty — every other field is
    /// then either zero or still meaningful for the CURRENT RUN (compiled/loaded
    /// count shaders regardless of whether anything is persisted).
    bool               enabled = false;
    /// The resolved directory, whether or not it exists yet.
    std::string        dir;
    /// The composite key (§4.2) as printable hex. A cache written under a
    /// different fingerprint is deleted, never read.
    std::string        fingerprint;
    unsigned long long sizeBytes = 0;
    unsigned           files = 0;
    /// The cap a save is held to (EngineConfig::shaderCacheMaxBytes).
    unsigned long long maxBytes = 0;

    // ---- what was loaded, per layer ----
    bool     pipelineCacheLoaded = false;   ///< VkPipelineCache blob accepted by the driver
    /// WHY pipelineCacheLoaded reads the way it does — the honest per-layer
    /// report the caching audit (F7) asked for. `loadPipelineCache` is void and
    /// the driver's verdict exists only as a log line, so this is scraped from
    /// the render system's own sentences and is the difference between "there
    /// was no blob" and "the driver threw yours away":
    ///   "absent"          nothing on disk to offer
    ///   "accepted"        loaded, N bytes
    ///   "outdated"        header mismatch (device/driver/UUID/hash) — the
    ///                     normal cost of a driver update
    ///   "rejected"        vkCreatePipelineCache refused it
    ///   "silent"          the render system said nothing at all: the PowerVR
    ///                     broken-pipeline-cache workaround makes load and save
    ///                     no-ops (OgreVulkanDevice.cpp), and every launch on
    ///                     such a device pays full PSO creation
    std::string pipelineCacheReason = "absent";
    bool     microcodeLoaded = false;       ///< SPIR-V microcode map read back (the directory's layer or the seed's)
    unsigned microcodeEntries = 0;          ///< entries in the live microcode map
    /// THE SEED (EngineConfig::shaderCacheSeedDir), resolved whether or not it exists; empty = no seed.
    std::string seedDir;
    /// What the load did with the seed:
    ///   "off"      no seed configured (or the cache is off)
    ///   "unused"   the directory's own microcode layer loaded; the seed was not read
    ///   "absent"   no manifest in the seed directory
    ///   "loaded"   the microcode map came from the seed (seedEntries entries)
    ///   "stale"    its microcode key is not this build's — not read, a miss
    ///   "corrupt"  a malformed manifest, no microcode line, or a length/checksum mismatch — not read
    ///   "rejected" Ogre threw while reading it
    std::string seedState = "off";
    unsigned    seedEntries = 0;            ///< entries the seed's map held, when "loaded"
    unsigned hlmsCachesLoaded = 0;          ///< Hlms disk caches applied (0..2: PBS, Unlit)
    /// THE COMPUTE LAYER (compute.bin, COLD-BOOT-1 stage 3): the records the load kept (their code compiled off the frame
    /// once the async compiler runs), those REJECTED (the stored source does not hash to its program key: another
    /// compiler's, or damaged) and those dropped STALE (a file they read - the template, a piece - changed).
    unsigned computeRecordsLoaded = 0;
    unsigned computeRecordsRejected = 0;
    unsigned computeRecordsStale = 0;
    /// THE LAYERS THIS LAUNCH's LOAD LEFT EMPTY (SHADER-SYSTEM-1 stage 6a), of "pipeline", "microcode", "hlms" and
    /// "compute", whatever the reason: the layer's key moved (another driver or card: the pipeline and hlms keys hold the
    /// GPU's terms), its file absent or damaged, the driver refusing the pipeline blob (outdated, rejected, or silent on a
    /// device whose pipeline cache is a no-op), every compute record stale. Empty with the cache off. The startup pass runs
    /// while it is not empty: the pass's key is the device-free identity (shaderIdentity()), so a driver change is seen
    /// HERE - its recipes regenerate from a whole microcode layer and compile nothing a count would show.
    std::vector<std::string> layersNotLoaded;

    // ---- shader accounting for THIS process ----
    /// Shaders the compiler actually built this run (GLSL -> SPIR-V).
    unsigned  compiledThisRun = 0;
    /// Shaders served straight out of the microcode cache — glslang never ran.
    /// Counts IN-PROCESS hits too (two shaders generated from byte-identical
    /// source share one microcode entry; the SMAA materials do it three times
    /// every launch), so a genuinely cold run reports a small non-zero value.
    /// "Did the disk cache work" is answered by microcodeLoaded, not by this.
    unsigned  loadedThisRun = 0;
    /// How many shaders the last saved run needed in total. 0 = unknown (no
    /// cache has ever been written). The startup progress counter's denominator.
    unsigned  expectedShaders = 0;
    /// Wall-clock of the last successful save, ms since the Unix epoch; 0 = never.
    long long lastSavedUnixMs = 0;
    /// THE SAVE'S RENDER-THREAD SIDE, this process (FRAME-MONITOR-DOOR-1 stage 4): the saves whose chunked copy
    /// completed and was handed to the writer, the frame messages those copies took, the longest message (the bar:
    /// <= 2 ms) and the longest NON-final step (a chunk of at most 20 entries: <= 1 ms); and the writes the writer
    /// finished ("saved N files").
    unsigned savesCopied = 0;
    unsigned copyMessages = 0;
    double   copyWorstMs = 0.0;
    double   copyChunkWorstMs = 0.0;
    unsigned copyChunkMaxEntries = 0;   ///< the most entries one chunked message took (the guard: <= copyChunkEntries)
    unsigned copyChunkEntries = 0;      ///< K, the chunk's count
    /// The free frames that carried a save step, and those of them whose work exceeded the display period (the
    /// driver's frame-overrun attribution on exactly the save's frames: the guard is 0).
    unsigned copyFrames = 0;
    unsigned copyFrameOverruns = 0;
    unsigned savesWritten = 0;
    /// A SAVE IS IN FLIGHT (SHADER-SYSTEM-1 stage 7a): its chunked copy still continuing on the frames, or its write
    /// on the writer. The startup gate's own save is waited for on it (frames flowing): a launch killed after the gate
    /// keeps what the gate compiled. False after a no-op save and after a failed one alike.
    bool saveInFlight = false;
    /// Compute pipelines the engine built outside the Hlms this PROCESS (the ray
    /// tier's, through the device's pipeline cache), and their build time — a warm
    /// pipeline layer makes the second near zero.
    unsigned enginePipelinesThisRun = 0;
    double   enginePipelineMs = 0.0;
    /// ...and WHICH: each engine pipeline's name in build order (PHOTON-II-1 item 12 —
    /// the reflection's two and the gather's joined the cache; a pipeline built
    /// outside it would be missing here).
    std::vector<std::string> enginePipelineNames;

    // ---- the two caches the shader HASH addresses (HLMSBITS-1) -------------
    /// THE NUMBER THAT CRASHED THE EDITOR ON 2026-09-14, now readable while the
    /// session is alive. Every shader is looked up by a 32-bit hash built from a
    /// PASS index and a RENDERABLE index; both caches grow for the life of the
    /// process and neither is ever evicted, so "how full are they" is a real
    /// health reading — and past the field's capacity the indices used to spill
    /// into each other silently. Reported as the WORST Hlms (the largest of PBS,
    /// Unlit, low-level), because one full cache is the problem whichever it is.
    unsigned passCacheEntries = 0;
    unsigned passCacheCapacity = 0;        ///< what the hash's pass field addresses
    unsigned renderableCacheEntries = 0;
    unsigned renderableCacheCapacity = 0;  ///< what the hash's renderable field addresses
};

}}  // namespace jahshaka::engine
