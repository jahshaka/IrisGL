#pragma once
// THE LIGHT FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_light.cpp. LightDesc is this family's (Lights.h's DecalDesc is the effects
// family's).
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Lights.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(LightType, {JAH_ENUMERATOR(LightType, Directional), JAH_ENUMERATOR(LightType, Point), JAH_ENUMERATOR(LightType, Spot),
                           JAH_ENUMERATOR(LightType, Area)});
JAH_RECORD_COUNT(LightDesc, 16);

}}}  // namespace jahshaka::engine::script
