// THE SCENE FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_scene.h declares, every field
// named once in declaration order (records.h). A colour is four numbers, r g b a (records.h: Colour), its alpha read
// only where a field says so; a direction is a unit vector, each number -1..1.
#include "script/records_scene.h"
#include "script/commands.h"   // the handle kinds (kTexture)

namespace jahshaka { namespace engine { namespace script {

/// AtmosphereSky (Sky.h): the dials' own documented ranges - the haze and the Rayleigh scale are multiples of the clear
/// air (>= 0), the ground's albedo 0..1, the aerial scale a distance scale (>= 0), the sky's brightness 0..10, the
/// planet's radius and the air's depth above 0 km; the sun's illuminance in the renderer's units (>= 0).
const Table<AtmosphereSky> &Record<AtmosphereSky>::table()
{
    static const Table<AtmosphereSky> t({
        field("sunHaze", &AtmosphereSky::sunHaze).atLeast(0),
        field("rayleighScale", &AtmosphereSky::rayleighScale).atLeast(0),
        field("ozone", &AtmosphereSky::ozone),
        field("groundAlbedo", &AtmosphereSky::groundAlbedo).range(0, 1),
        field("aerialScale", &AtmosphereSky::aerialScale).atLeast(0),
        field("skyBrightness", &AtmosphereSky::skyBrightness).range(0, 10),
        field("planetRadiusKm", &AtmosphereSky::planetRadiusKm).above(0).in("km"),
        field("atmosphereHeightKm", &AtmosphereSky::atmosphereHeightKm).above(0).in("km"),
        field("sunDir", &AtmosphereSky::sunDir).range(-1, 1),
        field("hasSun", &AtmosphereSky::hasSun),
        field("sunIlluminance", &AtmosphereSky::sunIlluminance).atLeast(0),
    });
    return t;
}

/// SunDisc (Sky.h): its direction towards the sun, its angular DIAMETER 0..180 degrees, its radiance (>= 0; the alpha is
/// not read).
const Table<SunDisc> &Record<SunDisc>::table()
{
    static const Table<SunDisc> t({
        field("enabled", &SunDisc::enabled),
        field("dir", &SunDisc::dir).range(-1, 1),
        field("angularDiameterDeg", &SunDisc::angularDiameterDeg).range(0, 180).in("degrees"),
        field("colour", &SunDisc::colour).atLeast(0),
        field("inProbes", &SunDisc::inProbes),
    });
    return t;
}

/// CloudLayerDesc (Sky.h): coverage 0..1, density 0..4, shadow 0..1, the altitude >= 0 m, the wind in m/s; its weather
/// map a texture handle (none: null); the sun's irradiance >= 0 (the alpha is not read).
const Table<CloudLayerDesc> &Record<CloudLayerDesc>::table()
{
    static const Table<CloudLayerDesc> t({
        field("enabled", &CloudLayerDesc::enabled),
        field("coverage", &CloudLayerDesc::coverage).range(0, 1),
        field("density", &CloudLayerDesc::density).range(0, 4),
        field("altitude", &CloudLayerDesc::altitude).atLeast(0).in("m"),
        field("shadow", &CloudLayerDesc::shadow).range(0, 1),
        field("wind", &CloudLayerDesc::wind).in("m/s"),
        field("weatherMap", &CloudLayerDesc::weatherMap).handle(kTexture),
        field("hasSun", &CloudLayerDesc::hasSun),
        field("sunDir", &CloudLayerDesc::sunDir).range(-1, 1),
        field("sunIrradiance", &CloudLayerDesc::sunIrradiance).atLeast(0),
    });
    return t;
}

/// SkyDesc (Sky.h): the mode, its images as texture handles (an equirect image; six cube faces +X -X +Y -Y +Z -Z, each
/// none as null), the atmosphere, the sun disc and the cloud layer.
const Table<SkyDesc> &Record<SkyDesc>::table()
{
    static const Table<SkyDesc> t({
        field("mode", &SkyDesc::mode),
        field("equirect", &SkyDesc::equirect).handle(kTexture),
        field("faces", &SkyDesc::faces).handle(kTexture),
        field("atmosphere", &SkyDesc::atmosphere),
        field("sun", &SkyDesc::sun),
        field("clouds", &SkyDesc::clouds),
    });
    return t;
}

/// FogDesc (Fog.h): densities per world unit (exp2, >= 0), the height layer's falloff (>= 0), its level (world Y); the
/// colour's alpha is not read.
const Table<FogDesc> &Record<FogDesc>::table()
{
    static const Table<FogDesc> t({
        field("enabled", &FogDesc::enabled),
        field("colour", &FogDesc::colour).atLeast(0),
        field("density", &FogDesc::density).atLeast(0),
        field("heightDensity", &FogDesc::heightDensity).atLeast(0),
        field("heightFalloff", &FogDesc::heightFalloff).atLeast(0),
        field("heightLevel", &FogDesc::heightLevel).in("m"),
    });
    return t;
}

/// ShadowDesc (Lights.h): the filter (hasFilter false: no opinion), the map's side in pixels - 0 (no opinion) or
/// 256..8192, the engine's clamp (OgreShadow.cpp setShadowResolution): 1..255 FAILS in engine.scene.setShadowSettings.
const Table<ShadowDesc> &Record<ShadowDesc>::table()
{
    static const Table<ShadowDesc> t({
        field("hasFilter", &ShadowDesc::hasFilter),
        field("filter", &ShadowDesc::filter),
        field("resolution", &ShadowDesc::resolution).range(0, 8192).in("px"),
    });
    return t;
}

/// MeshCardDesc (Mesh.h): its axis 0..5 (+X -X +Y -Y +Z -Z), its LOD level, its box in mesh space.
const Table<MeshCardDesc> &Record<MeshCardDesc>::table()
{
    static const Table<MeshCardDesc> t({
        field("axis", &MeshCardDesc::axis).range(0, 5),
        field("lodLevel", &MeshCardDesc::lodLevel).range(0, kMeshLevels),
        field("origin", &MeshCardDesc::origin),
        field("halfU", &MeshCardDesc::halfU).atLeast(0),
        field("halfV", &MeshCardDesc::halfV).atLeast(0),
        field("halfDepth", &MeshCardDesc::halfDepth).atLeast(0),
    });
    return t;
}

/// MeshCluster (Mesh.h): its range of clusterIndices, its group and the group that refined it (-1: none), its bound.
const Table<MeshCluster> &Record<MeshCluster>::table()
{
    static const Table<MeshCluster> t({
        field("firstIndex", &MeshCluster::firstIndex),
        field("indexCount", &MeshCluster::indexCount),
        field("group", &MeshCluster::group).range(-1, kMeshClusters),
        field("refined", &MeshCluster::refined).range(-1, kMeshClusters),
        field("centre", &MeshCluster::centre),
        field("radius", &MeshCluster::radius).atLeast(0),
    });
    return t;
}

/// MeshClusterGroup (Mesh.h): its depth in the DAG, its bound, its measured error (FLT_MAX: terminal) and the
/// simplifier's estimate.
const Table<MeshClusterGroup> &Record<MeshClusterGroup>::table()
{
    static const Table<MeshClusterGroup> t({
        field("depth", &MeshClusterGroup::depth).range(0, kMeshLevels),
        field("centre", &MeshClusterGroup::centre),
        field("radius", &MeshClusterGroup::radius).atLeast(0),
        field("error", &MeshClusterGroup::error).atLeast(0),
        field("estimate", &MeshClusterGroup::estimate),
    });
    return t;
}

/// MeshData (Mesh.h) FROM FLAT ARRAYS: xyz per vertex (positions, normals), uv, xyzw tangents, the indices; four bone
/// indices and weights per vertex; level i+1's indices (lodIndices: each level's indices as bytes, uint32 in this
/// machine's byte order) with their claimed errors and bounds; the cards and the cluster DAG. Bounded by
/// records_scene.h's kMesh*; the engine judges the rest (Scene::createMesh: lengths that do not agree are refused).
const Table<MeshData> &Record<MeshData>::table()
{
    static const Table<MeshData> t({
        field("positions", &MeshData::positions).upTo(3 * kMeshVertices),
        field("normals", &MeshData::normals).upTo(3 * kMeshVertices),
        field("uvs", &MeshData::uvs).upTo(2 * kMeshVertices),
        field("tangents", &MeshData::tangents).upTo(4 * kMeshVertices),
        field("indices", &MeshData::indices).upTo(kMeshIndices),
        field("blendIndices", &MeshData::blendIndices).upTo(4 * kMeshVertices),
        field("blendWeights", &MeshData::blendWeights).range(0, 1).upTo(4 * kMeshVertices),
        field("dynamic", &MeshData::dynamic),
        field("lodIndices", &MeshData::lodIndices).upTo(kMeshLevels),
        field("lodErrors", &MeshData::lodErrors).upTo(kMeshLevels),
        field("lodBounds", &MeshData::lodBounds).atLeast(0).upTo(kMeshLevels),
        field("cards", &MeshData::cards).upTo(kMeshCards),
        field("cardCoverage", &MeshData::cardCoverage).range(0, 1),
        field("clusterIndices", &MeshData::clusterIndices).upTo(kMeshIndices),
        field("clusters", &MeshData::clusters).upTo(kMeshClusters),
        field("clusterGroups", &MeshData::clusterGroups).upTo(kMeshClusters),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
