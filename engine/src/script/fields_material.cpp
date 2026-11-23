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
    static const Table<D> t({field("blend", &D::blend).range(0, double(PbrParams::detailBlendNames().size() - 1)),
                             field("offsetU", &D::offsetU), field("offsetV", &D::offsetV), field("scaleU", &D::scaleU),
                             field("scaleV", &D::scaleV), field("weight", &D::weight), field("normalWeight", &D::normalWeight)});
    return t;
}

const Table<PbrParams> &Record<PbrParams>::table()
{
    using P = PbrParams;
    static const Table<P> t({field("albedo", &P::albedo).range(0, 1), field("metalness", &P::metalness).range(0, 1),
                             field("roughness", &P::roughness).range(0, 1), field("emissive", &P::emissive).atLeast(0),
                             field("alphaMode", &P::alphaMode), field("alpha", &P::alpha).range(0, 1),
                             field("alphaCutoff", &P::alphaCutoff).range(0, 1),
                             field("normalMapWeight", &P::normalMapWeight).atLeast(0), field("uvScale", &P::uvScale),
                             field("uvOffset", &P::uvOffset), field("uvRotation", &P::uvRotation).in("degrees"),
                             field("uvVelocity", &P::uvVelocity), field("refractionStrength", &P::refractionStrength).atLeast(0),
                             field("clearCoat", &P::clearCoat).range(0, 1),
                             field("clearCoatRoughness", &P::clearCoatRoughness).range(0, 1),
                             oneOf(field("brdf", &P::brdf), {"Default", "CookTorrance", "BlinnPhong", "DefaultSeparateDiffuseFresnel",
                                                             "CookTorranceSeparateDiffuseFresnel",
                                                             "BlinnPhongSeparateDiffuseFresnel"}),
                             field("receiveShadows", &P::receiveShadows), field("emissiveAsLightmap", &P::emissiveAsLightmap),
                             field("shadingModel", &P::shadingModel), field("workflow", &P::workflow), field("detail", &P::detail),
                             field("anisotropy", &P::anisotropy).range(1, 16), field("address", &P::address),
                             field("specularColour", &P::specularColour).atLeast(0), field("ior", &P::ior).atLeast(1),
                             field("fresnelColour", &P::fresnelColour).range(0, 1),
                             field("useFresnelColour", &P::useFresnelColour), field("separateFresnel", &P::separateFresnel)});
    return t;
}

}}}  // namespace jahshaka::engine::script
