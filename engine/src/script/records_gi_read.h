#pragma once
// THE GI READERS' RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record the nine GI
// readers answer, declared with JAH_RECORD_COUNT beside its table's declaration; the tables in fields_gi_read.cpp. The
// GI enums two families share (GiRegionKind: the changed regions') are named once, in records_shared.h. Every record
// here is an ANSWER: its big arrays are `bytes` (RULED 3: Field::bytes), the voxel volume's arrays of levels arrays of
// bytes, its directional levels PACKED (records.h detail::PackedOf; the layout in engine.gi.voxelVolume's doc).
#include "script/records.h"
#include "script/records_shared.h"
#include "jahshaka/engine/GiCards.h"
#include "jahshaka/engine/GiStatus.h"
#include "jahshaka/engine/GiVoxels.h"

namespace jahshaka { namespace engine { namespace script {

JAH_RECORD_COUNT(CardSample, 28);
JAH_RECORD_COUNT(CardRadiance, 8);
JAH_RECORD_COUNT(GiVoxelStats, 22);
JAH_RECORD_COUNT(GiVoxelVolume, 23);
JAH_RECORD_COUNT(GiChangedRegion, 4);
JAH_RECORD_COUNT(GiChangedRegions, 8);

/// The readers' bounds (fields_gi_read.cpp, commands_gi_read.cpp): a voxel volume's mip levels (a 256-cell cascade has
/// 9; the cap is the record's arrays' declared maximum) and one traceRays batch (12 numbers a ray in, 4 out: 786,432
/// numbers, under a converted call's 2^20 elements).
constexpr unsigned kGiReadLevels = 16, kGiReadRays = 65536;

}}}  // namespace jahshaka::engine::script
