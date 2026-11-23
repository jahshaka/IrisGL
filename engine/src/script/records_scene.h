#pragma once
// THE SCENE FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_scene.cpp: the sky (SkyDesc and its three halves), the fog, the shadow request,
// and a mesh's data (MeshData with its cards and its cluster DAG). EnvironmentDesc is NOT here: Scene::setEnvironment is
// declared, not wrapped (tests/hygiene/script_census/declared.txt).
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Fog.h"
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Mesh.h"
#include "jahshaka/engine/Sky.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(SkyMode, {JAH_ENUMERATOR(SkyMode, NoSky), JAH_ENUMERATOR(SkyMode, Equirectangular), JAH_ENUMERATOR(SkyMode, Cubemap),
                         JAH_ENUMERATOR(SkyMode, Atmosphere)});
JAH_ENUM_NAMES(ShadowFilter, {JAH_ENUMERATOR(ShadowFilter, Hard), JAH_ENUMERATOR(ShadowFilter, Soft), JAH_ENUMERATOR(ShadowFilter, VerySoft)});
JAH_RECORD_COUNT(AtmosphereSky, 11);
JAH_RECORD_COUNT(SunDisc, 5);
JAH_RECORD_COUNT(CloudLayerDesc, 10);
JAH_RECORD_COUNT(SkyDesc, 6);
JAH_RECORD_COUNT(FogDesc, 6);
JAH_RECORD_COUNT(ShadowDesc, 3);
JAH_RECORD_COUNT(MeshCardDesc, 6);
JAH_RECORD_COUNT(MeshCluster, 6);
JAH_RECORD_COUNT(MeshClusterGroup, 5);
JAH_RECORD_COUNT(MeshData, 16);

/// engine.mesh.create's bounds (fields_scene.cpp): a mesh of at most kMeshVertices vertices and kMeshIndices indices a
/// level - each flat array at most 2^20 values (a converted call's element bound, Host::Options::callBounds) - at most
/// kMeshLevels LOD levels, kMeshCards cards, kMeshClusters clusters and groups.
constexpr unsigned kMeshVertices = 262144, kMeshIndices = 1048576, kMeshLevels = 32, kMeshCards = 4096,
                   kMeshClusters = 65536;

}}}  // namespace jahshaka::engine::script
