#pragma once
// THE ENUMS TWO FAMILIES' RECORDS SHARE (SCRIPT-IN-ENGINE-1 E0, the batch's base): an enum's names are ONE
// specialisation (JAH_ENUM_NAMES), so an enum that records of two families carry is named here, once, and each
// family's records_<family>.h includes this file - the GI enums: GiParams (Q's records_gi.h) and GiStatus (R's
// records_status.h) both carry GiMode, and the changed regions (P's records_gi_read.h) their GiRegionKind. Every other
// enum is named in its one family's header. The names are the enumerators' own, first letter lowered (engine.gi.set's).
// AND THE TWO NAME MACROS' ONE HOME (the tail, the lead's ruling 2026-11-23): every family's tables and enum tables are
// written through them.
#include "script/records.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/GiStatus.h"

#include <string>

namespace jahshaka { namespace engine { namespace script {

/// THE NAME IS THE MEMBER's (the R lane's audit, (a): a field tabled under another name, or two enumerators' names
/// swapped, passed both halves of the guard - inOrder reads offsets, the round trip is symmetric). Every family's tables
/// and enum tables are written ONLY through these two: the name is the source token, never typed (source.script_names).
///   JAH_FIELD(R, m)       = field("m", &R::m)
///   JAH_ENUMERATOR(E, v)  = {E::v, "v" with its first letter lowered} (the engine's enumerators are UpperCamel, the
///                           script's names lowerCamel: GiMode::VctPccHybrid -> "vctPccHybrid")
#define JAH_FIELD(R, m) field(#m, &R::m)
inline std::string lowerFirst(const char *s)
{
    std::string n(s);
    if (!n.empty() && n[0] >= 'A' && n[0] <= 'Z') n[0] = char(n[0] - 'A' + 'a');
    return n;
}
#define JAH_ENUMERATOR(E, v)                                                                                         \
    {                                                                                                                \
        E::v, [] {                                                                                                   \
            static const std::string name = lowerFirst(#v);                                                          \
            return name.c_str();                                                                                     \
        }()                                                                                                          \
    }

JAH_ENUM_NAMES(GiMode, {JAH_ENUMERATOR(GiMode, Off), JAH_ENUMERATOR(GiMode, Vct), JAH_ENUMERATOR(GiMode, VctPccHybrid)});
JAH_ENUM_NAMES(GiQuality, {JAH_ENUMERATOR(GiQuality, Low), JAH_ENUMERATOR(GiQuality, Medium),
                           JAH_ENUMERATOR(GiQuality, High), JAH_ENUMERATOR(GiQuality, Epic)});
JAH_ENUM_NAMES(GiToggle, {JAH_ENUMERATOR(GiToggle, Auto), JAH_ENUMERATOR(GiToggle, Off), JAH_ENUMERATOR(GiToggle, On)});
JAH_ENUM_NAMES(GiStaleReason, {JAH_ENUMERATOR(GiStaleReason, None), JAH_ENUMERATOR(GiStaleReason, Rebuild),
                               JAH_ENUMERATOR(GiStaleReason, Refresh), JAH_ENUMERATOR(GiStaleReason, Moved),
                               JAH_ENUMERATOR(GiStaleReason, Light), JAH_ENUMERATOR(GiStaleReason, Material),
                               JAH_ENUMERATOR(GiStaleReason, Sky), JAH_ENUMERATOR(GiStaleReason, Ambient),
                               JAH_ENUMERATOR(GiStaleReason, Fog), JAH_ENUMERATOR(GiStaleReason, Mobility),
                               JAH_ENUMERATOR(GiStaleReason, Camera)});
JAH_ENUM_NAMES(GiRegionKind, {JAH_ENUMERATOR(GiRegionKind, Slab), JAH_ENUMERATOR(GiRegionKind, Card),
                              JAH_ENUMERATOR(GiRegionKind, Edit), JAH_ENUMERATOR(GiRegionKind, Mover),
                              JAH_ENUMERATOR(GiRegionKind, MoverShadow), JAH_ENUMERATOR(GiRegionKind, LampShadow),
                              JAH_ENUMERATOR(GiRegionKind, Lamp), JAH_ENUMERATOR(GiRegionKind, CardScene)});

}}}  // namespace jahshaka::engine::script
