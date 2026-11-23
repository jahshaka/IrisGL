// THE LIGHT FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_light.h declares, every field
// named once in declaration order (records.h).
#include "script/records_light.h"

namespace jahshaka { namespace engine { namespace script {

/// LightDesc (Lights.h). EACH DOMAIN IS THE ENGINE's OWN (OgreScene::setLight clamps what lies outside it - a value it
/// would clamp FAILS here by name instead): the range at least 0.01 m, the source radius at least 0.001 m, a rectangle's
/// sides at least 0.01 m, the spot's half angle 1..85 degrees and its softness 0..0.99, its falloff exponent at least 0.
/// The colour is four numbers, r g b a (linear; the light reads r g b, its alpha is not read). The IES profile's path
/// is set by engine.light.setIesProfile alone (`files`): engine.light.set REFUSES a non-empty one (RULED 9).
const Table<LightDesc> &Record<LightDesc>::table()
{
    static const Table<LightDesc> t({
        JAH_FIELD(LightDesc, type),
        JAH_FIELD(LightDesc, colour).atLeast(0),
        JAH_FIELD(LightDesc, intensity).atLeast(0),
        JAH_FIELD(LightDesc, range).atLeast(0.01).in("m"),
        JAH_FIELD(LightDesc, sourceRadius).atLeast(0.001).in("m"),
        JAH_FIELD(LightDesc, spotAngleDegrees).range(1, 85).in("degrees"),
        JAH_FIELD(LightDesc, spotSoftness).range(0, 0.99),
        JAH_FIELD(LightDesc, spotFalloff).atLeast(0),
        JAH_FIELD(LightDesc, castShadows),
        JAH_FIELD(LightDesc, rectWidth).atLeast(0.01).in("m"),
        JAH_FIELD(LightDesc, rectHeight).atLeast(0.01).in("m"),
        JAH_FIELD(LightDesc, doubleSided),
        JAH_FIELD(LightDesc, iesProfilePath),
        JAH_FIELD(LightDesc, lightMask),
        JAH_FIELD(LightDesc, forwardShadingPriority),
        JAH_FIELD(LightDesc, primaryDirectional),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
