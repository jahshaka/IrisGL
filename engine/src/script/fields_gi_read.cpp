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
        JAH_FIELD(CardSample, ok),
        JAH_FIELD(CardSample, albedo),
        JAH_FIELD(CardSample, normal),
        JAH_FIELD(CardSample, emissive),
        JAH_FIELD(CardSample, depth),
        JAH_FIELD(CardSample, shadow),
        JAH_FIELD(CardSample, roughness),
        JAH_FIELD(CardSample, radiance),
        JAH_FIELD(CardSample, indirect),
        JAH_FIELD(CardSample, indirectStored),
        JAH_FIELD(CardSample, specularF0),
        JAH_FIELD(CardSample, specularWeight),
        JAH_FIELD(CardSample, flat),
        JAH_FIELD(CardSample, lampMask),
        JAH_FIELD(CardSample, lampOverflowVis),
        JAH_FIELD(CardSample, lampThroughMask),
        JAH_FIELD(CardSample, lampThroughVis),
        JAH_FIELD(CardSample, lampIds),
        JAH_FIELD(CardSample, lampCount),
        JAH_FIELD(CardSample, card),
        JAH_FIELD(CardSample, texelX),
        JAH_FIELD(CardSample, texelY),
        JAH_FIELD(CardSample, lit),
        JAH_FIELD(CardSample, position),
        JAH_FIELD(CardSample, worldNormal),
        JAH_FIELD(CardSample, sunLift),
        JAH_FIELD(CardSample, gather),
        JAH_FIELD(CardSample, gatherSky),
    });
    return t;
}

/// CardRadiance (GiCards.h): one resident card - its node (a node handle), its place, face and side in texels, its
/// relight frames, and its radiance texel by texel as BYTES (float32 r g b, row-major, size x size texels).
const Table<CardRadiance> &Record<CardRadiance>::table()
{
    static const Table<CardRadiance> t({
        JAH_FIELD(CardRadiance, node).handle(kNode),
        JAH_FIELD(CardRadiance, card),
        JAH_FIELD(CardRadiance, bucket),
        JAH_FIELD(CardRadiance, size),
        JAH_FIELD(CardRadiance, lastRelit),
        JAH_FIELD(CardRadiance, lastIndirect),
        JAH_FIELD(CardRadiance, lastRead),
        JAH_FIELD(CardRadiance, rgb).bytes(),
    });
    return t;
}

/// GiVoxelStats (GiVoxels.h): one cascade's light volume, counted - every member a number or a string.
const Table<GiVoxelStats> &Record<GiVoxelStats>::table()
{
    static const Table<GiVoxelStats> t({
        JAH_FIELD(GiVoxelStats, available),
        JAH_FIELD(GiVoxelStats, cascade),
        JAH_FIELD(GiVoxelStats, width),
        JAH_FIELD(GiVoxelStats, height),
        JAH_FIELD(GiVoxelStats, depth),
        JAH_FIELD(GiVoxelStats, format),
        JAH_FIELD(GiVoxelStats, formatMax),
        JAH_FIELD(GiVoxelStats, peak),
        JAH_FIELD(GiVoxelStats, peakDirect),
        JAH_FIELD(GiVoxelStats, meanLit),
        JAH_FIELD(GiVoxelStats, voxelsLit),
        JAH_FIELD(GiVoxelStats, voxelsAtMax),
        JAH_FIELD(GiVoxelStats, directAtMax),
        JAH_FIELD(GiVoxelStats, voxels),
        JAH_FIELD(GiVoxelStats, voxelsAboveOne),
        JAH_FIELD(GiVoxelStats, emissiveFormat),
        JAH_FIELD(GiVoxelStats, peakEmissive),
        JAH_FIELD(GiVoxelStats, emissiveAtMax),
        JAH_FIELD(GiVoxelStats, emissiveAboveOne),
        JAH_FIELD(GiVoxelStats, lightDigest),
        JAH_FIELD(GiVoxelStats, storeDigest),
        JAH_FIELD(GiVoxelStats, directDigest),
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
        JAH_FIELD(GiVoxelVolume, available),
        JAH_FIELD(GiVoxelVolume, width),
        JAH_FIELD(GiVoxelVolume, height),
        JAH_FIELD(GiVoxelVolume, depth),
        JAH_FIELD(GiVoxelVolume, origin),
        JAH_FIELD(GiVoxelVolume, cell),
        JAH_FIELD(GiVoxelVolume, light).bytes(),
        JAH_FIELD(GiVoxelVolume, albedo).bytes(),
        JAH_FIELD(GiVoxelVolume, coverageP).bytes(),
        JAH_FIELD(GiVoxelVolume, coverageN).bytes(),
        JAH_FIELD(GiVoxelVolume, positionP).bytes(),
        JAH_FIELD(GiVoxelVolume, positionN).bytes(),
        JAH_FIELD(GiVoxelVolume, emissive).bytes(),
        JAH_FIELD(GiVoxelVolume, emitP).bytes(),
        JAH_FIELD(GiVoxelVolume, emitN).bytes(),
        JAH_FIELD(GiVoxelVolume, accumWeight).bytes(),
        JAH_FIELD(GiVoxelVolume, direct).bytes(),
        JAH_FIELD(GiVoxelVolume, albedoLevels).upTo(kGiReadLevels),
        JAH_FIELD(GiVoxelVolume, directional),
        JAH_FIELD(GiVoxelVolume, halfLight).bytes(),
        JAH_FIELD(GiVoxelVolume, halfDirect).bytes(),
        JAH_FIELD(GiVoxelVolume, halfSide),
        JAH_FIELD(GiVoxelVolume, halfNormal),
    });
    return t;
}

/// GiChangedRegion (GiStatus.h): a world box, its kind (records_shared.h's names), the cascades it reached (bit i).
const Table<GiChangedRegion> &Record<GiChangedRegion>::table()
{
    static const Table<GiChangedRegion> t({
        JAH_FIELD(GiChangedRegion, lo),
        JAH_FIELD(GiChangedRegion, hi),
        JAH_FIELD(GiChangedRegion, kind),
        JAH_FIELD(GiChangedRegion, cascadeMask),
    });
    return t;
}

/// GiChangedRegions (GiStatus.h): the list as it last closed - at most kGiRegionCap regions.
const Table<GiChangedRegions> &Record<GiChangedRegions>::table()
{
    static const Table<GiChangedRegions> t({
        JAH_FIELD(GiChangedRegions, available),
        JAH_FIELD(GiChangedRegions, frame),
        JAH_FIELD(GiChangedRegions, overflow),
        JAH_FIELD(GiChangedRegions, restart),
        JAH_FIELD(GiChangedRegions, sun),
        JAH_FIELD(GiChangedRegions, toSun),
        JAH_FIELD(GiChangedRegions, merges),
        JAH_FIELD(GiChangedRegions, regions).upTo(kGiRegionCap),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
