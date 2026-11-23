// THE GI READERS' FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_gi_read.h declares, every field
// named once in declaration order (records.h). ANSWERS: a big array is `bytes` (Field::bytes: its elements' bytes in
// this machine's byte order - a float32 / uint32 array, little-endian on every target we build); a vector of records
// declares its maximum (the engine's own cap where it has one).
#include "script/records_gi_read.h"
#include "script/commands.h"   // the handle kinds (kNode)

namespace jahshaka { namespace engine { namespace script {

/// CardSample (GiCards.h): one surface-cache texel as the engine read it - its albedo, normal, emissive, depth, the sun
/// term, the lit radiance and its halves, the lamps' traced visibility (lampIds: 48 world ids, lampCount of them used),
/// which card and texel answered, the texel's world surface, its gather layer.
const Table<CardSample> &Record<CardSample>::table()
{
    static const Table<CardSample> t({
        field("ok", &CardSample::ok),
        field("albedo", &CardSample::albedo),
        field("normal", &CardSample::normal),
        field("emissive", &CardSample::emissive),
        field("depth", &CardSample::depth),
        field("shadow", &CardSample::shadow),
        field("roughness", &CardSample::roughness),
        field("radiance", &CardSample::radiance),
        field("indirect", &CardSample::indirect),
        field("indirectStored", &CardSample::indirectStored),
        field("specularF0", &CardSample::specularF0),
        field("specularWeight", &CardSample::specularWeight),
        field("flat", &CardSample::flat),
        field("lampMask", &CardSample::lampMask),
        field("lampOverflowVis", &CardSample::lampOverflowVis),
        field("lampThroughMask", &CardSample::lampThroughMask),
        field("lampThroughVis", &CardSample::lampThroughVis),
        field("lampIds", &CardSample::lampIds),
        field("lampCount", &CardSample::lampCount),
        field("card", &CardSample::card),
        field("texelX", &CardSample::texelX),
        field("texelY", &CardSample::texelY),
        field("lit", &CardSample::lit),
        field("position", &CardSample::position),
        field("worldNormal", &CardSample::worldNormal),
        field("sunLift", &CardSample::sunLift),
        field("gather", &CardSample::gather),
        field("gatherSky", &CardSample::gatherSky),
    });
    return t;
}

/// CardRadiance (GiCards.h): one resident card - its node (a node handle), its place, face and side in texels, its
/// relight frames, and its radiance texel by texel as BYTES (float32 r g b, row-major, size x size texels).
const Table<CardRadiance> &Record<CardRadiance>::table()
{
    static const Table<CardRadiance> t({
        field("node", &CardRadiance::node).handle(kNode),
        field("card", &CardRadiance::card),
        field("bucket", &CardRadiance::bucket),
        field("size", &CardRadiance::size),
        field("lastRelit", &CardRadiance::lastRelit),
        field("lastIndirect", &CardRadiance::lastIndirect),
        field("lastRead", &CardRadiance::lastRead),
        field("rgb", &CardRadiance::rgb).bytes(),
    });
    return t;
}

/// GiVoxelStats (GiVoxels.h): one cascade's light volume, counted - every member a number or a string.
const Table<GiVoxelStats> &Record<GiVoxelStats>::table()
{
    static const Table<GiVoxelStats> t({
        field("available", &GiVoxelStats::available),
        field("cascade", &GiVoxelStats::cascade),
        field("width", &GiVoxelStats::width),
        field("height", &GiVoxelStats::height),
        field("depth", &GiVoxelStats::depth),
        field("format", &GiVoxelStats::format),
        field("formatMax", &GiVoxelStats::formatMax),
        field("peak", &GiVoxelStats::peak),
        field("peakDirect", &GiVoxelStats::peakDirect),
        field("meanLit", &GiVoxelStats::meanLit),
        field("voxelsLit", &GiVoxelStats::voxelsLit),
        field("voxelsAtMax", &GiVoxelStats::voxelsAtMax),
        field("directAtMax", &GiVoxelStats::directAtMax),
        field("voxels", &GiVoxelStats::voxels),
        field("voxelsAboveOne", &GiVoxelStats::voxelsAboveOne),
        field("emissiveFormat", &GiVoxelStats::emissiveFormat),
        field("peakEmissive", &GiVoxelStats::peakEmissive),
        field("emissiveAtMax", &GiVoxelStats::emissiveAtMax),
        field("emissiveAboveOne", &GiVoxelStats::emissiveAboveOne),
        field("lightDigest", &GiVoxelStats::lightDigest),
        field("storeDigest", &GiVoxelStats::storeDigest),
        field("directDigest", &GiVoxelStats::directDigest),
    });
    return t;
}

/// GiVoxelVolume (GiVoxels.h): one cascade's voxels - its placement, then every volume as BYTES (float32 RGBA a voxel,
/// x fastest; accumWeight uint32 a voxel); albedoLevels an array of bytes (level L + 1 each); directional THREE PACKED
/// bytes values (axis X, Y, Z: each a uint32 n, n uint32 counts, the n levels' floats); halfSide / halfNormal three
/// bytes values (an axis each). A reader that fills only some leaves the rest empty (zero bytes).
const Table<GiVoxelVolume> &Record<GiVoxelVolume>::table()
{
    static const Table<GiVoxelVolume> t({
        field("available", &GiVoxelVolume::available),
        field("width", &GiVoxelVolume::width),
        field("height", &GiVoxelVolume::height),
        field("depth", &GiVoxelVolume::depth),
        field("origin", &GiVoxelVolume::origin),
        field("cell", &GiVoxelVolume::cell),
        field("light", &GiVoxelVolume::light).bytes(),
        field("albedo", &GiVoxelVolume::albedo).bytes(),
        field("coverageP", &GiVoxelVolume::coverageP).bytes(),
        field("coverageN", &GiVoxelVolume::coverageN).bytes(),
        field("positionP", &GiVoxelVolume::positionP).bytes(),
        field("positionN", &GiVoxelVolume::positionN).bytes(),
        field("emissive", &GiVoxelVolume::emissive).bytes(),
        field("emitP", &GiVoxelVolume::emitP).bytes(),
        field("emitN", &GiVoxelVolume::emitN).bytes(),
        field("accumWeight", &GiVoxelVolume::accumWeight).bytes(),
        field("direct", &GiVoxelVolume::direct).bytes(),
        field("albedoLevels", &GiVoxelVolume::albedoLevels).upTo(kGiReadLevels),
        field("directional", &GiVoxelVolume::directional),
        field("halfLight", &GiVoxelVolume::halfLight).bytes(),
        field("halfDirect", &GiVoxelVolume::halfDirect).bytes(),
        field("halfSide", &GiVoxelVolume::halfSide),
        field("halfNormal", &GiVoxelVolume::halfNormal),
    });
    return t;
}

/// GiChangedRegion (GiStatus.h): a world box, its kind (records_shared.h's names), the cascades it reached (bit i).
const Table<GiChangedRegion> &Record<GiChangedRegion>::table()
{
    static const Table<GiChangedRegion> t({
        field("lo", &GiChangedRegion::lo),
        field("hi", &GiChangedRegion::hi),
        field("kind", &GiChangedRegion::kind),
        field("cascadeMask", &GiChangedRegion::cascadeMask),
    });
    return t;
}

/// GiChangedRegions (GiStatus.h): the list as it last closed - at most kGiRegionCap regions.
const Table<GiChangedRegions> &Record<GiChangedRegions>::table()
{
    static const Table<GiChangedRegions> t({
        field("available", &GiChangedRegions::available),
        field("frame", &GiChangedRegions::frame),
        field("overflow", &GiChangedRegions::overflow),
        field("restart", &GiChangedRegions::restart),
        field("sun", &GiChangedRegions::sun),
        field("toSun", &GiChangedRegions::toSun),
        field("merges", &GiChangedRegions::merges),
        field("regions", &GiChangedRegions::regions).upTo(kGiRegionCap),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
