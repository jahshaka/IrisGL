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

JAH_ENUM_NAMES(VrState, {JAH_ENUMERATOR(VrState, Unavailable), JAH_ENUMERATOR(VrState, Idle), JAH_ENUMERATOR(VrState, Ready), JAH_ENUMERATOR(VrState, Synchronized), JAH_ENUMERATOR(VrState, Visible), JAH_ENUMERATOR(VrState, Focused), JAH_ENUMERATOR(VrState, Stopping), JAH_ENUMERATOR(VrState, Lost)});
JAH_ENUM_NAMES(VrMirrorMode, {JAH_ENUMERATOR(VrMirrorMode, None), JAH_ENUMERATOR(VrMirrorMode, Left), JAH_ENUMERATOR(VrMirrorMode, Right), JAH_ENUMERATOR(VrMirrorMode, Both)});
JAH_ENUM_NAMES(VrDesktopPicture, {JAH_ENUMERATOR(VrDesktopPicture, Own), JAH_ENUMERATOR(VrDesktopPicture, Eye)});
JAH_ENUM_NAMES(VrFailure, {JAH_ENUMERATOR(VrFailure, None), JAH_ENUMERATOR(VrFailure, Disabled), JAH_ENUMERATOR(VrFailure, Headless), JAH_ENUMERATOR(VrFailure, NoRuntime), JAH_ENUMERATOR(VrFailure, WrongRuntime), JAH_ENUMERATOR(VrFailure, NoHeadset), JAH_ENUMERATOR(VrFailure, RuntimeBroken), JAH_ENUMERATOR(VrFailure, DeviceMismatch), JAH_ENUMERATOR(VrFailure, ConnectionLost)});
JAH_ENUM_NAMES(GiArmStage, {JAH_ENUMERATOR(GiArmStage, None), JAH_ENUMERATOR(GiArmStage, ProbeScout), JAH_ENUMERATOR(GiArmStage, ProbeFit), JAH_ENUMERATOR(GiArmStage, ProbeFinish), JAH_ENUMERATOR(GiArmStage, Field)});
JAH_ENUM_NAMES(GpuFault, {JAH_ENUMERATOR(GpuFault, None), JAH_ENUMERATOR(GpuFault, DeviceLost), JAH_ENUMERATOR(GpuFault, OutOfMemoryInFrame)});
JAH_ENUM_NAMES(MonitorLevel, {JAH_ENUMERATOR(MonitorLevel, Off), JAH_ENUMERATOR(MonitorLevel, Review)});
JAH_ENUM_NAMES(FrameCause, {JAH_ENUMERATOR(FrameCause, Unknown), JAH_ENUMERATOR(FrameCause, Driver), JAH_ENUMERATOR(FrameCause, Scripted), JAH_ENUMERATOR(FrameCause, Offscreen), JAH_ENUMERATOR(FrameCause, WarmUp), JAH_ENUMERATOR(FrameCause, Player)});
JAH_ENUM_NAMES(PassBucket, {JAH_ENUMERATOR(PassBucket, Other), JAH_ENUMERATOR(PassBucket, Main), JAH_ENUMERATOR(PassBucket, Post), JAH_ENUMERATOR(PassBucket, ShadowView), JAH_ENUMERATOR(PassBucket, ShadowReflect), JAH_ENUMERATOR(PassBucket, ShadowProbe), JAH_ENUMERATOR(PassBucket, Planar), JAH_ENUMERATOR(PassBucket, ProbeFace)});
JAH_ENUM_NAMES(CacheKind, {JAH_ENUMERATOR(CacheKind, Probe), JAH_ENUMERATOR(CacheKind, ShadowMap), JAH_ENUMERATOR(CacheKind, Gi), JAH_ENUMERATOR(CacheKind, Planar), JAH_ENUMERATOR(CacheKind, Shader), JAH_ENUMERATOR(CacheKind, Texture), JAH_ENUMERATOR(CacheKind, Atmosphere), JAH_ENUMERATOR(CacheKind, Cull), JAH_ENUMERATOR(CacheKind, Video)});
JAH_ENUM_NAMES(WorkReason, {JAH_ENUMERATOR(WorkReason, None), JAH_ENUMERATOR(WorkReason, Build), JAH_ENUMERATOR(WorkReason, Rebuild), JAH_ENUMERATOR(WorkReason, Refresh), JAH_ENUMERATOR(WorkReason, Sweep), JAH_ENUMERATOR(WorkReason, Moved), JAH_ENUMERATOR(WorkReason, Caster), JAH_ENUMERATOR(WorkReason, Light), JAH_ENUMERATOR(WorkReason, Material), JAH_ENUMERATOR(WorkReason, Sky), JAH_ENUMERATOR(WorkReason, Ambient), JAH_ENUMERATOR(WorkReason, Fog), JAH_ENUMERATOR(WorkReason, Mobility), JAH_ENUMERATOR(WorkReason, Added), JAH_ENUMERATOR(WorkReason, Removed), JAH_ENUMERATOR(WorkReason, Bounds), JAH_ENUMERATOR(WorkReason, Resolution), JAH_ENUMERATOR(WorkReason, Camera), JAH_ENUMERATOR(WorkReason, Permutation), JAH_ENUMERATOR(WorkReason, Request)});
JAH_ENUM_NAMES(MonitorEventKind, {JAH_ENUMERATOR(MonitorEventKind, GiRebuild), JAH_ENUMERATOR(MonitorEventKind, GiRefresh), JAH_ENUMERATOR(MonitorEventKind, ProbeGridBuild), JAH_ENUMERATOR(MonitorEventKind, AtlasRebuild), JAH_ENUMERATOR(MonitorEventKind, WorkspaceRebuild), JAH_ENUMERATOR(MonitorEventKind, ShaderCompile), JAH_ENUMERATOR(MonitorEventKind, TextureLoad), JAH_ENUMERATOR(MonitorEventKind, VramFlush), JAH_ENUMERATOR(MonitorEventKind, DeviceLost), JAH_ENUMERATOR(MonitorEventKind, ViewDestroyed), JAH_ENUMERATOR(MonitorEventKind, Host)});
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
