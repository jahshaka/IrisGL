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
        JAH_FIELD(AtmosphereSky, sunHaze).atLeast(0),
        JAH_FIELD(AtmosphereSky, rayleighScale).atLeast(0),
        JAH_FIELD(AtmosphereSky, ozone),
        JAH_FIELD(AtmosphereSky, groundAlbedo).range(0, 1),
        JAH_FIELD(AtmosphereSky, aerialScale).atLeast(0),
        JAH_FIELD(AtmosphereSky, skyBrightness).range(0, 10),
        JAH_FIELD(AtmosphereSky, planetRadiusKm).above(0).in("km"),
        JAH_FIELD(AtmosphereSky, atmosphereHeightKm).above(0).in("km"),
        JAH_FIELD(AtmosphereSky, sunDir).range(-1, 1),
        JAH_FIELD(AtmosphereSky, hasSun),
        JAH_FIELD(AtmosphereSky, sunIlluminance).atLeast(0),
    });
    return t;
}

/// SunDisc (Sky.h): its direction towards the sun, its angular DIAMETER 0..180 degrees, its radiance (>= 0; the alpha is
/// not read).
const Table<SunDisc> &Record<SunDisc>::table()
{
    static const Table<SunDisc> t({
        JAH_FIELD(SunDisc, enabled),
        JAH_FIELD(SunDisc, dir).range(-1, 1),
        JAH_FIELD(SunDisc, angularDiameterDeg).range(0, 180).in("degrees"),
        JAH_FIELD(SunDisc, colour).atLeast(0),
        JAH_FIELD(SunDisc, inProbes),
    });
    return t;
}

/// CloudLayerDesc (Sky.h): coverage 0..1, density 0..4, shadow 0..1, the altitude >= 0 m, the wind in m/s; its weather
/// map a texture handle (none: null); the sun's irradiance >= 0 (the alpha is not read).
const Table<CloudLayerDesc> &Record<CloudLayerDesc>::table()
{
    static const Table<CloudLayerDesc> t({
        JAH_FIELD(CloudLayerDesc, enabled),
        JAH_FIELD(CloudLayerDesc, coverage).range(0, 1),
        JAH_FIELD(CloudLayerDesc, density).range(0, 4),
        JAH_FIELD(CloudLayerDesc, altitude).atLeast(0).in("m"),
        JAH_FIELD(CloudLayerDesc, shadow).range(0, 1),
        JAH_FIELD(CloudLayerDesc, wind).in("m/s"),
        JAH_FIELD(CloudLayerDesc, weatherMap).handle(kTexture),
        JAH_FIELD(CloudLayerDesc, hasSun),
        JAH_FIELD(CloudLayerDesc, sunDir).range(-1, 1),
        JAH_FIELD(CloudLayerDesc, sunIrradiance).atLeast(0),
    });
    return t;
}

/// SkyDesc (Sky.h): the mode, its images as texture handles (an equirect image; six cube faces +X -X +Y -Y +Z -Z, each
/// none as null), the atmosphere, the sun disc and the cloud layer.
const Table<SkyDesc> &Record<SkyDesc>::table()
{
    static const Table<SkyDesc> t({
        JAH_FIELD(SkyDesc, mode),
        JAH_FIELD(SkyDesc, equirect).handle(kTexture),
        JAH_FIELD(SkyDesc, faces).handle(kTexture),
        JAH_FIELD(SkyDesc, atmosphere),
        JAH_FIELD(SkyDesc, sun),
        JAH_FIELD(SkyDesc, clouds),
    });
    return t;
}

/// FogDesc (Fog.h): densities per world unit (exp2, >= 0), the height layer's falloff (>= 0), its level (world Y); the
/// colour's alpha is not read.
const Table<FogDesc> &Record<FogDesc>::table()
{
    static const Table<FogDesc> t({
        JAH_FIELD(FogDesc, enabled),
        JAH_FIELD(FogDesc, colour).atLeast(0),
        JAH_FIELD(FogDesc, density).atLeast(0),
        JAH_FIELD(FogDesc, heightDensity).atLeast(0),
        JAH_FIELD(FogDesc, heightFalloff).atLeast(0),
        JAH_FIELD(FogDesc, heightLevel).in("m"),
    });
    return t;
}

/// ShadowDesc (Lights.h): the filter (hasFilter false: no opinion), the map's side in pixels - 0 (no opinion) or
/// 256..8192, the engine's clamp (OgreShadow.cpp setShadowResolution): 1..255 FAILS in engine.scene.setShadowSettings.
const Table<ShadowDesc> &Record<ShadowDesc>::table()
{
    static const Table<ShadowDesc> t({
        JAH_FIELD(ShadowDesc, hasFilter),
        JAH_FIELD(ShadowDesc, filter),
        JAH_FIELD(ShadowDesc, resolution).range(0, 8192).in("px"),
    });
    return t;
}

/// MeshCardDesc (Mesh.h): its axis 0..5 (+X -X +Y -Y +Z -Z), its LOD level, its box in mesh space.
const Table<MeshCardDesc> &Record<MeshCardDesc>::table()
{
    static const Table<MeshCardDesc> t({
        JAH_FIELD(MeshCardDesc, axis).range(0, 5),
        JAH_FIELD(MeshCardDesc, lodLevel).range(0, kMeshLevels),
        JAH_FIELD(MeshCardDesc, origin),
        JAH_FIELD(MeshCardDesc, halfU).atLeast(0),
        JAH_FIELD(MeshCardDesc, halfV).atLeast(0),
        JAH_FIELD(MeshCardDesc, halfDepth).atLeast(0),
    });
    return t;
}

/// MeshCluster (Mesh.h): its range of clusterIndices, its group and the group that refined it (-1: none), its bound.
const Table<MeshCluster> &Record<MeshCluster>::table()
{
    static const Table<MeshCluster> t({
        JAH_FIELD(MeshCluster, firstIndex),
        JAH_FIELD(MeshCluster, indexCount),
        JAH_FIELD(MeshCluster, group).range(-1, kMeshClusters),
        JAH_FIELD(MeshCluster, refined).range(-1, kMeshClusters),
        JAH_FIELD(MeshCluster, centre),
        JAH_FIELD(MeshCluster, radius).atLeast(0),
    });
    return t;
}

/// MeshClusterGroup (Mesh.h): its depth in the DAG, its bound, its measured error (FLT_MAX: terminal) and the
/// simplifier's estimate.
const Table<MeshClusterGroup> &Record<MeshClusterGroup>::table()
{
    static const Table<MeshClusterGroup> t({
        JAH_FIELD(MeshClusterGroup, depth).range(0, kMeshLevels),
        JAH_FIELD(MeshClusterGroup, centre),
        JAH_FIELD(MeshClusterGroup, radius).atLeast(0),
        JAH_FIELD(MeshClusterGroup, error).atLeast(0),
        JAH_FIELD(MeshClusterGroup, estimate),
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
        JAH_FIELD(MeshData, positions).upTo(3 * kMeshVertices),
        JAH_FIELD(MeshData, normals).upTo(3 * kMeshVertices),
        JAH_FIELD(MeshData, uvs).upTo(2 * kMeshVertices),
        JAH_FIELD(MeshData, tangents).upTo(4 * kMeshVertices),
        JAH_FIELD(MeshData, indices).upTo(kMeshIndices),
        JAH_FIELD(MeshData, blendIndices).upTo(4 * kMeshVertices),
        JAH_FIELD(MeshData, blendWeights).range(0, 1).upTo(4 * kMeshVertices),
        JAH_FIELD(MeshData, dynamic),
        JAH_FIELD(MeshData, lodIndices).upTo(kMeshLevels),
        JAH_FIELD(MeshData, lodErrors).upTo(kMeshLevels),
        JAH_FIELD(MeshData, lodBounds).atLeast(0).upTo(kMeshLevels),
        JAH_FIELD(MeshData, cards).upTo(kMeshCards),
        JAH_FIELD(MeshData, cardCoverage).range(0, 1),
        JAH_FIELD(MeshData, clusterIndices).upTo(kMeshIndices),
        JAH_FIELD(MeshData, clusters).upTo(kMeshClusters),
        JAH_FIELD(MeshData, clusterGroups).upTo(kMeshClusters),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
