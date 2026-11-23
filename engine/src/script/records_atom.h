#pragma once
// THE ATOM FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_atom.cpp. The atom commands take no record: the Atom view's names only.
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Overlay.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(AtomView, {JAH_ENUMERATOR(AtomView, Off), JAH_ENUMERATOR(AtomView, Triangles), JAH_ENUMERATOR(AtomView, Levels),
                          JAH_ENUMERATOR(AtomView, Buckets), JAH_ENUMERATOR(AtomView, Objects), JAH_ENUMERATOR(AtomView, Density)});

}}}  // namespace jahshaka::engine::script
