#pragma once
// THE ENGINE's SNAPSHOT TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <string>
#include <utility>
#include <vector>
#include "jahshaka/engine/Config.h"
#include "jahshaka/engine/GiParams.h"
#include "jahshaka/engine/GiStatus.h"
#include "jahshaka/engine/Ids.h"
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Math.h"
#include "jahshaka/engine/Monitor.h"
#include "jahshaka/engine/Shaders.h"
#include "jahshaka/engine/Skeleton.h"
#include "jahshaka/engine/Stats.h"

namespace jahshaka { namespace engine {

// ---- Engine snapshot (§4.8 `snapshot_start.json` / `snapshot_end.json`) ----

/// One pass of the compositor graph, as DEFINED (not as executed).
struct CompositorPassInfo {
    std::string type;            ///< "render_scene", "render_quad", "clear", "compute", ...
    std::string profilingId;
    std::string camera;          ///< the camera the definition names, when it names one
    /// The shadow node a SCENE pass names, empty when it names none. The single
    /// most expensive line in a capture's graph: a pass that names one
    /// re-renders it, and a probe face that names the VIEW's node costs a
    /// full-resolution atlas per probe.
    std::string shadowNode;
    unsigned    shadowMapIdx = FramePass::kNoShadowMap;
    unsigned    numInitialPasses = 0;   ///< 0 = every frame; >0 = only the first N
};

struct CompositorNodeInfo {
    std::string name;
    std::vector<CompositorPassInfo> passes;
};

/// One live workspace. THE COMPOSITOR GRAPH the spec asks for is the list of
/// these: every workspace, its nodes and passes, and which scene each renders.
struct CompositorWorkspaceInfo {
    std::string name;            ///< the workspace definition's name
    std::string owner;           ///< "view:main", "probe:7", "planar:0", "?"
    std::string scene;           ///< the SceneManager it renders
    bool        enabled = false;
    unsigned    width = 0, height = 0;
    unsigned    listeners = 0;   ///< listeners attached to it (the monitor's included)
    std::vector<CompositorNodeInfo> nodes;
};

/// One reflection probe, as placed.
struct ProbeInfo {
    unsigned index = 0;
    Vec3     centre, halfSize;
    /// The probe's parallax SHAPE (what the shader reprojects onto).
    Vec3     shapeMin, shapeMax;
    bool     dirty = false;      ///< scheduled to re-capture
    bool     isStatic = false;   ///< placed as a static (never-swept) probe
    unsigned resolution = 0;
};

/// One light, as the engine holds it — with whether the lamp-map cache has it.
struct SnapshotLight {
    NodeId      node = 0;
    LightType   type = LightType::Point;
    bool        castShadow = false;
    bool        cached = false;     ///< the lamp-map cache holds its map
    bool        dirty = false;      ///< ...and it re-renders on the next frame
    unsigned    shadowSlot = 0xFFFFFFFFu;   ///< the atlas slot, or ~0
    float       range = 0.0f;
    float       intensity = 0.0f;
    Vec3        position;
};

/// THE WHOLE ENGINE, at one instant. Written at the start and the end of a
/// capture so analysis can see what changed under it. Built from the status
/// readers that already exist plus the compositor graph, which nothing else
/// could see.
struct EngineSnapshot {
    bool        live = false;
    std::string label;              ///< "start" / "end" / whatever the host passes
    std::string scene;              ///< the scene the snapshot describes
    unsigned long long frame = 0;
    double      atMs = 0.0;         ///< capture-relative milliseconds
    DeviceInfo  device;
    GiParams    giParams;           ///< every GI parameter, as requested
    GiStatus    gi;                 ///< ...and what they resolved to
    ShadowStatus shadow;
    ShaderCacheStats shaderCache;
    ObjectCounts objects;
    MemoryStats  memory;
    EngineThreading threading;
    MobilityStatus  mobility;
    RenderStats  render;
    /// The texture streaming queue at this instant.
    bool        texturesDoneStreaming = true;
    unsigned    texturesPending = 0;
    /// Datablocks held per Hlms block (pbs, unlit, low-level...). Ogre keeps
    /// the compiled-shader cache itself private at this pin, so this is the
    /// exact number the snapshot can state — and it is the one behind the
    /// batching-collapse question.
    std::vector<std::pair<std::string, unsigned>> hlmsDatablocks;
    /// VRAM by pool — the texture-manager entries, LARGEST FIRST and capped
    /// (a bundle must not be dominated by this list). `textureCount` is how
    /// many there really are and `texturesTruncated` how many were dropped, so
    /// a reader is never silently given a partial list.
    std::vector<TextureMemoryEntry> textures;
    unsigned textureCount = 0;
    unsigned texturesTruncated = 0;
    std::vector<SnapshotLight> lights;
    std::vector<ProbeInfo>     probes;
    std::vector<CompositorWorkspaceInfo> workspaces;
};

}}  // namespace jahshaka::engine
