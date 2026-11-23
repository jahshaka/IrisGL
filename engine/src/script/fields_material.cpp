// THE MATERIAL FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_material.h declares, every
// field named once in declaration order (records.h). THE DOMAINS are the engine's own laws (OgreMaterials.cpp applyPbr):
// a value the engine would clamp, round or replace FAILS by name here instead (an IOR under 1 is clamped to 1, :162;
// an anisotropy outside 1..16 is clamped, :2156; a detail blend past the last mode is clamped, :415; a BRDF name it
// does not know falls back to Default with a log line, :84-107) - except roughness 0, which the engine's contract
// takes and clamps to 1e-4 by design (Materials.h PbrParams::roughness). Two bounds are physics, not the engine's
// clamps: an albedo and an F0 colour reflect at most what arrives (0..1 a channel); an emission and a kS are >= 0.
#include "script/records_material.h"

namespace jahshaka { namespace engine { namespace script {

namespace {
/// A string field that takes only `names` (records.h's Field has no oneOf: its parameter's).
template <class R> Field<R> oneOf(Field<R> f, std::vector<std::string> names)
{
    f.param = f.param.oneOf(std::move(names));
    return f;
}
}  // namespace

const Table<PbrParams::DetailLayer> &Record<PbrParams::DetailLayer>::table()
{
    using D = PbrParams::DetailLayer;
    static const Table<D> t({JAH_FIELD(D, blend).range(0, double(PbrParams::detailBlendNames().size() - 1)),
                             JAH_FIELD(D, offsetU), JAH_FIELD(D, offsetV), JAH_FIELD(D, scaleU),
                             JAH_FIELD(D, scaleV), JAH_FIELD(D, weight), JAH_FIELD(D, normalWeight)});
    return t;
}

const Table<PbrParams> &Record<PbrParams>::table()
{
    using P = PbrParams;
    static const Table<P> t({JAH_FIELD(P, albedo).range(0, 1), JAH_FIELD(P, metalness).range(0, 1),
                             JAH_FIELD(P, roughness).range(0, 1), JAH_FIELD(P, emissive).atLeast(0),
                             JAH_FIELD(P, alphaMode), JAH_FIELD(P, alpha).range(0, 1),
                             JAH_FIELD(P, alphaCutoff).range(0, 1),
                             JAH_FIELD(P, normalMapWeight).atLeast(0), JAH_FIELD(P, uvScale),
                             JAH_FIELD(P, uvOffset), JAH_FIELD(P, uvRotation).in("degrees"),
                             JAH_FIELD(P, uvVelocity), JAH_FIELD(P, refractionStrength).atLeast(0),
                             JAH_FIELD(P, clearCoat).range(0, 1),
                             JAH_FIELD(P, clearCoatRoughness).range(0, 1),
                             oneOf(JAH_FIELD(P, brdf), {"Default", "CookTorrance", "BlinnPhong", "DefaultSeparateDiffuseFresnel",
                                                             "CookTorranceSeparateDiffuseFresnel",
                                                             "BlinnPhongSeparateDiffuseFresnel"}),
                             JAH_FIELD(P, receiveShadows), JAH_FIELD(P, emissiveAsLightmap),
                             JAH_FIELD(P, shadingModel), JAH_FIELD(P, workflow), JAH_FIELD(P, detail),
                             JAH_FIELD(P, anisotropy).range(1, 16), JAH_FIELD(P, address),
                             JAH_FIELD(P, specularColour).atLeast(0), JAH_FIELD(P, ior).atLeast(1),
                             JAH_FIELD(P, fresnelColour).range(0, 1),
                             JAH_FIELD(P, useFresnelColour), JAH_FIELD(P, separateFresnel)});
    return t;
}

}}}  // namespace jahshaka::engine::script
