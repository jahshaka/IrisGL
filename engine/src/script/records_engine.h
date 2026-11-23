#pragma once
// THE ENGINE FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_engine.cpp. The engine-wide reads' answers: what the renderer's texture manager
// holds (TextureMemoryEntry, Stats.h) and the validation layer's proof (ValidationStatus, Monitor.h).
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Monitor.h"
#include "jahshaka/engine/Stats.h"

namespace jahshaka { namespace engine { namespace script {

JAH_RECORD_COUNT(TextureMemoryEntry, 15);
JAH_RECORD_COUNT(ValidationStatus, 4);

}}}  // namespace jahshaka::engine::script
