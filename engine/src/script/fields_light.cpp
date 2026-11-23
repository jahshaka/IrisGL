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
        field("type", &LightDesc::type),
        field("colour", &LightDesc::colour).atLeast(0),
        field("intensity", &LightDesc::intensity).atLeast(0),
        field("range", &LightDesc::range).atLeast(0.01).in("m"),
        field("sourceRadius", &LightDesc::sourceRadius).atLeast(0.001).in("m"),
        field("spotAngleDegrees", &LightDesc::spotAngleDegrees).range(1, 85).in("degrees"),
        field("spotSoftness", &LightDesc::spotSoftness).range(0, 0.99),
        field("spotFalloff", &LightDesc::spotFalloff).atLeast(0),
        field("castShadows", &LightDesc::castShadows),
        field("rectWidth", &LightDesc::rectWidth).atLeast(0.01).in("m"),
        field("rectHeight", &LightDesc::rectHeight).atLeast(0.01).in("m"),
        field("doubleSided", &LightDesc::doubleSided),
        field("iesProfilePath", &LightDesc::iesProfilePath),
        field("lightMask", &LightDesc::lightMask),
        field("forwardShadingPriority", &LightDesc::forwardShadingPriority),
        field("primaryDirectional", &LightDesc::primaryDirectional),
    });
    return t;
}

}}}  // namespace jahshaka::engine::script
