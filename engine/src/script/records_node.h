#pragma once
// THE NODE FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_node.cpp. The node family takes no record: one enum, the face cull
// (engine.node.setFaceCull).
#include "script/records.h"
#include "jahshaka/engine/Materials.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(FaceCull, {{FaceCull::Material, "material"}, {FaceCull::Back, "back"}, {FaceCull::Front, "front"},
                          {FaceCull::TwoSided, "twoSided"}});

}}}  // namespace jahshaka::engine::script
