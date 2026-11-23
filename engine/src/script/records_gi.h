#pragma once
// THE GI FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_gi.cpp. A scene's GiParams WHOLE (its cascade table as the record holds it:
// cascadeCount and cascadeSet - what 2d's PublishedFrame::giApplied answers; engine.gi.set takes the two as its
// `cascades` list), the placement pin and the sun contact. The GI enums two families share are records_shared.h's.
#include "script/records.h"
#include "script/records_shared.h"
#include "jahshaka/engine/GiGather.h"
#include "jahshaka/engine/GiParams.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(SunContactResolution, {{SunContactResolution::Auto, "auto"}, {SunContactResolution::Full, "full"},
                                      {SunContactResolution::Half, "half"}});

JAH_RECORD_COUNT(GiParams::GiCascadeDesc, 3);
JAH_RECORD_COUNT(GiParams, 25);
JAH_RECORD_COUNT(GiPlacementPin, 5);
JAH_RECORD_COUNT(SunContactDesc, 3);

}}}  // namespace jahshaka::engine::script
