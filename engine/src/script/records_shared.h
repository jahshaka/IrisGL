#pragma once
// THE ENUMS TWO FAMILIES' RECORDS SHARE (SCRIPT-IN-ENGINE-1 E0, the batch's base): an enum's names are ONE
// specialisation (JAH_ENUM_NAMES), so an enum that records of two families carry is named here, once, and each
// family's records_<family>.h includes this file - the GI enums: GiParams (Q's records_gi.h) and GiStatus (R's
// records_status.h) both carry GiMode, and the changed regions (P's records_gi_read.h) their GiRegionKind. Every other
// enum is named in its one family's header. The names are the enumerators' own, first letter lowered (engine.gi.set's).
#include "script/records.h"
#include "jahshaka/engine/GiModes.h"
#include "jahshaka/engine/GiStatus.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(GiMode, {{GiMode::Off, "off"}, {GiMode::Vct, "vct"}, {GiMode::VctPccHybrid, "vctPccHybrid"}});
JAH_ENUM_NAMES(GiQuality, {{GiQuality::Low, "low"}, {GiQuality::Medium, "medium"}, {GiQuality::High, "high"},
                           {GiQuality::Epic, "epic"}});
JAH_ENUM_NAMES(GiToggle, {{GiToggle::Auto, "auto"}, {GiToggle::Off, "off"}, {GiToggle::On, "on"}});
JAH_ENUM_NAMES(GiStaleReason, {{GiStaleReason::None, "none"}, {GiStaleReason::Rebuild, "rebuild"},
                               {GiStaleReason::Refresh, "refresh"}, {GiStaleReason::Moved, "moved"},
                               {GiStaleReason::Light, "light"}, {GiStaleReason::Material, "material"},
                               {GiStaleReason::Sky, "sky"}, {GiStaleReason::Ambient, "ambient"}, {GiStaleReason::Fog, "fog"},
                               {GiStaleReason::Mobility, "mobility"}, {GiStaleReason::Camera, "camera"}});
JAH_ENUM_NAMES(GiRegionKind, {{GiRegionKind::Slab, "slab"}, {GiRegionKind::Card, "card"}, {GiRegionKind::Edit, "edit"},
                              {GiRegionKind::Mover, "mover"}, {GiRegionKind::MoverShadow, "moverShadow"},
                              {GiRegionKind::LampShadow, "lampShadow"}, {GiRegionKind::Lamp, "lamp"},
                              {GiRegionKind::CardScene, "cardScene"}});

}}}  // namespace jahshaka::engine::script
