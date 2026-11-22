#pragma once
// THE LANGUAGE TABLE's MAKERS (SCRIPT-IN-ENGINE-1; the lead's S5): each line of src/script/LanguageTable.inc declares its
// binding's maker here (defined in that language's ONE file, src/script/<lang>_*.cpp - the only file that names its
// engine: source.script_neutral) and Host's constructor adds it (host.cpp). The interface is Host::Language (Script.h).
#include "jahshaka/engine/Script.h"

namespace jahshaka { namespace engine { namespace script { namespace detail {

#define JAH_SCRIPT_LANGUAGE(extension, maker) std::unique_ptr<Host::Language> maker(Host &host);
#include "script/LanguageTable.inc"
#undef JAH_SCRIPT_LANGUAGE

}}}}  // namespace jahshaka::engine::script::detail
