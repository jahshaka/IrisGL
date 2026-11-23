#pragma once
// THE STATUS FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 2d, keyed by family: records.h says how) - every record the status
// commands ANSWER (commands_status.cpp: the published frames of the engine, a scene and a view, the monitor's records
// and events, the submit status, the shader cache's figures, Atom's draw status), each nested record with its own
// table, declared with JAH_RECORD_COUNT beside its table's declaration (a field added to one breaks the build until its
// table names it: THE GUARD's PERMANENT COST, the spec's 2d - an engine lane adding a published field adds its line in
// fields_status.cpp in the same commit), and their enums with JAH_ENUM_NAMES. The tables in fields_status.cpp; the
// GI enums are records_shared.h's, the post chain's records records_view.h's.
#include "script/records.h"
#include "script/records_shared.h"
#include "script/records_view.h"
#include "jahshaka/engine/Atom.h"
#include "jahshaka/engine/FrameInput.h"
#include "jahshaka/engine/Monitor.h"
#include "jahshaka/engine/Published.h"
#include "jahshaka/engine/Shaders.h"
#include "jahshaka/engine/Stats.h"
#include "jahshaka/engine/Vr.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(VrState, {{VrState::Unavailable, "unavailable"}, {VrState::Idle, "idle"}, {VrState::Ready, "ready"}, {VrState::Synchronized, "synchronized"}, {VrState::Visible, "visible"}, {VrState::Focused, "focused"}, {VrState::Stopping, "stopping"}, {VrState::Lost, "lost"}});
JAH_ENUM_NAMES(VrMirrorMode, {{VrMirrorMode::None, "none"}, {VrMirrorMode::Left, "left"}, {VrMirrorMode::Right, "right"}, {VrMirrorMode::Both, "both"}});
JAH_ENUM_NAMES(VrDesktopPicture, {{VrDesktopPicture::Own, "own"}, {VrDesktopPicture::Eye, "eye"}});
JAH_ENUM_NAMES(VrFailure, {{VrFailure::None, "none"}, {VrFailure::Disabled, "disabled"}, {VrFailure::Headless, "headless"}, {VrFailure::NoRuntime, "noRuntime"}, {VrFailure::WrongRuntime, "wrongRuntime"}, {VrFailure::NoHeadset, "noHeadset"}, {VrFailure::RuntimeBroken, "runtimeBroken"}, {VrFailure::DeviceMismatch, "deviceMismatch"}, {VrFailure::ConnectionLost, "connectionLost"}});
JAH_ENUM_NAMES(GiArmStage, {{GiArmStage::None, "none"}, {GiArmStage::ProbeScout, "probeScout"}, {GiArmStage::ProbeFit, "probeFit"}, {GiArmStage::ProbeFinish, "probeFinish"}, {GiArmStage::Field, "field"}});
JAH_ENUM_NAMES(GpuFault, {{GpuFault::None, "none"}, {GpuFault::DeviceLost, "deviceLost"}, {GpuFault::OutOfMemoryInFrame, "outOfMemoryInFrame"}});
JAH_ENUM_NAMES(MonitorLevel, {{MonitorLevel::Off, "off"}, {MonitorLevel::Review, "review"}});
JAH_ENUM_NAMES(FrameCause, {{FrameCause::Unknown, "unknown"}, {FrameCause::Driver, "driver"}, {FrameCause::Scripted, "scripted"}, {FrameCause::Offscreen, "offscreen"}, {FrameCause::WarmUp, "warmUp"}, {FrameCause::Player, "player"}});
JAH_ENUM_NAMES(PassBucket, {{PassBucket::Other, "other"}, {PassBucket::Main, "main"}, {PassBucket::Post, "post"}, {PassBucket::ShadowView, "shadowView"}, {PassBucket::ShadowReflect, "shadowReflect"}, {PassBucket::ShadowProbe, "shadowProbe"}, {PassBucket::Planar, "planar"}, {PassBucket::ProbeFace, "probeFace"}});
JAH_ENUM_NAMES(CacheKind, {{CacheKind::Probe, "probe"}, {CacheKind::ShadowMap, "shadowMap"}, {CacheKind::Gi, "gi"}, {CacheKind::Planar, "planar"}, {CacheKind::Shader, "shader"}, {CacheKind::Texture, "texture"}, {CacheKind::Atmosphere, "atmosphere"}, {CacheKind::Cull, "cull"}, {CacheKind::Video, "video"}});
JAH_ENUM_NAMES(WorkReason, {{WorkReason::None, "none"}, {WorkReason::Build, "build"}, {WorkReason::Rebuild, "rebuild"}, {WorkReason::Refresh, "refresh"}, {WorkReason::Sweep, "sweep"}, {WorkReason::Moved, "moved"}, {WorkReason::Caster, "caster"}, {WorkReason::Light, "light"}, {WorkReason::Material, "material"}, {WorkReason::Sky, "sky"}, {WorkReason::Ambient, "ambient"}, {WorkReason::Fog, "fog"}, {WorkReason::Mobility, "mobility"}, {WorkReason::Added, "added"}, {WorkReason::Removed, "removed"}, {WorkReason::Bounds, "bounds"}, {WorkReason::Resolution, "resolution"}, {WorkReason::Camera, "camera"}, {WorkReason::Permutation, "permutation"}, {WorkReason::Request, "request"}});
JAH_ENUM_NAMES(MonitorEventKind, {{MonitorEventKind::GiRebuild, "giRebuild"}, {MonitorEventKind::GiRefresh, "giRefresh"}, {MonitorEventKind::ProbeGridBuild, "probeGridBuild"}, {MonitorEventKind::AtlasRebuild, "atlasRebuild"}, {MonitorEventKind::WorkspaceRebuild, "workspaceRebuild"}, {MonitorEventKind::ShaderCompile, "shaderCompile"}, {MonitorEventKind::TextureLoad, "textureLoad"}, {MonitorEventKind::VramFlush, "vramFlush"}, {MonitorEventKind::DeviceLost, "deviceLost"}, {MonitorEventKind::ViewDestroyed, "viewDestroyed"}, {MonitorEventKind::Host, "host"}});
JAH_RECORD_COUNT(PublishedFrame::RiderTag, 6);
JAH_RECORD_COUNT(PublishedFrame::Facts, 4);
JAH_RECORD_COUNT(PublishedFrame::Facts::Share, 2);
JAH_RECORD_COUNT(PublishedFrame::Facts::NodeClips, 2);
JAH_RECORD_COUNT(PublishedFrame::Facts::Clip, 2);
JAH_RECORD_COUNT(PublishedFrame::Completion, 5);
JAH_RECORD_COUNT(GiStatus, 106);
JAH_RECORD_COUNT(GiStatus::CascadeStatus, 84);
JAH_RECORD_COUNT(GatherStatus, 60);
JAH_RECORD_COUNT(GatherRestRecord, 10);
JAH_RECORD_COUNT(GatherRay, 11);
JAH_RECORD_COUNT(CardCacheStatus, 104);
JAH_RECORD_COUNT(CardCacheStatus::GatherCounts, 11);
JAH_RECORD_COUNT(CardCacheStatus::Refused, 4);
JAH_RECORD_COUNT(CardCacheStatus::WorkRoot, 2);
JAH_RECORD_COUNT(GiStatus::VoxelSideCounts, 12);
JAH_RECORD_COUNT(PublishedFrame::Atmosphere, 7);
JAH_RECORD_COUNT(HeightFogStatus, 3);
JAH_RECORD_COUNT(PublishedFrame::Completeness, 4);
JAH_RECORD_COUNT(CloudStatus, 8);
JAH_RECORD_COUNT(AtmosphereStatus, 11);
JAH_RECORD_COUNT(RayQueryStatus, 97);
JAH_RECORD_COUNT(RayQueryStatus::ReflectEmitter, 4);
JAH_RECORD_COUNT(RayQueryStatus::HitListView, 8);
JAH_RECORD_COUNT(SunContactStatus, 13);
JAH_RECORD_COUNT(AreaVisStatus, 18);
JAH_RECORD_COUNT(PublishedFrame::AtomHidden, 3);
JAH_RECORD_COUNT(MobilityStatus, 3);
JAH_RECORD_COUNT(RigStats, 4);
JAH_RECORD_COUNT(SceneChangeStats, 5);
JAH_RECORD_COUNT(GpuSceneStatus, 14);
JAH_RECORD_COUNT(PublishedFrame::NodeCount, 2);
JAH_RECORD_COUNT(PublishedFrame::RigPoses, 2);
JAH_RECORD_COUNT(BonePose, 3);
JAH_RECORD_COUNT(PublishedFrame::NodePose, 3);
JAH_RECORD_COUNT(ViewFrames, 14);
JAH_RECORD_COUNT(PublishedEngineFrame, 33);
JAH_RECORD_COUNT(FrameInputStatus, 23);
JAH_RECORD_COUNT(VrStatus, 38);
JAH_RECORD_COUNT(VrPose, 3);
JAH_RECORD_COUNT(VrHandState, 15);
JAH_RECORD_COUNT(VrProfileName, 1);
JAH_RECORD_COUNT(VrInfo, 17);
JAH_RECORD_COUNT(VrBindingBlock, 3);
JAH_RECORD_COUNT(StreamingWork, 7);
JAH_RECORD_COUNT(AsyncShaderStats, 39);
JAH_RECORD_COUNT(AsyncShaderStats::Burst, 2);
JAH_RECORD_COUNT(PublishedEngineFrame::FrameError, 3);
JAH_RECORD_COUNT(PublishedEngineFrame::Refusal, 2);
JAH_RECORD_COUNT(MonitorStatus, 16);
JAH_RECORD_COUNT(ShadowStatus, 30);
JAH_RECORD_COUNT(ShadowMapInfo, 6);
JAH_RECORD_COUNT(RenderStats, 19);
JAH_RECORD_COUNT(MonitorRecord, 49);
JAH_RECORD_COUNT(FrameStage, 2);
JAH_RECORD_COUNT(FramePass, 22);
JAH_RECORD_COUNT(CacheWork, 15);
JAH_RECORD_COUNT(MonitorEvent, 8);
JAH_RECORD_COUNT(FrameSubmitStatus, 6);
JAH_RECORD_COUNT(ShaderCacheStats, 39);
JAH_RECORD_COUNT(AtomDrawStatus, 79);
JAH_RECORD_COUNT(AtomCasterMapStats, 16);

}}}  // namespace jahshaka::engine::script
