#pragma once
// THE MATERIAL FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_material.cpp. The material commands' PbrParams (with its detail layer), and the
// names of the enums the material and texture commands take (a texture slot, a decal image's atlas).
#include "script/records.h"
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Materials.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(PbrAlphaMode, {{PbrAlphaMode::Opaque, "opaque"}, {PbrAlphaMode::Cutout, "cutout"}, {PbrAlphaMode::Blend, "blend"},
                              {PbrAlphaMode::Glass, "glass"}, {PbrAlphaMode::Additive, "additive"},
                              {PbrAlphaMode::Modulate, "modulate"}, {PbrAlphaMode::Refractive, "refractive"}});
JAH_ENUM_NAMES(ShadingModel, {{ShadingModel::Lit, "lit"}, {ShadingModel::Unlit, "unlit"}, {ShadingModel::Distortion, "distortion"}});
JAH_ENUM_NAMES(PbrParams::Workflow, {{PbrParams::Workflow::Metallic, "metallic"}, {PbrParams::Workflow::Specular, "specular"},
                                     {PbrParams::Workflow::SpecularAsFresnel, "specularAsFresnel"}});
JAH_ENUM_NAMES(PbrParams::AddressMode, {{PbrParams::AddressMode::Wrap, "wrap"}, {PbrParams::AddressMode::Clamp, "clamp"},
                                        {PbrParams::AddressMode::Mirror, "mirror"}, {PbrParams::AddressMode::Border, "border"}});
/// (Count is no slot: it is not named, and no command answers it)
JAH_ENUM_NAMES(PbrTextureSlot, {{PbrTextureSlot::Albedo, "albedo"}, {PbrTextureSlot::Normal, "normal"},
                                {PbrTextureSlot::Metalness, "metalness"}, {PbrTextureSlot::Roughness, "roughness"},
                                {PbrTextureSlot::Emissive, "emissive"}, {PbrTextureSlot::Detail0, "detail0"},
                                {PbrTextureSlot::Detail1, "detail1"}, {PbrTextureSlot::Detail0Nm, "detail0Nm"},
                                {PbrTextureSlot::Detail1Nm, "detail1Nm"}, {PbrTextureSlot::DetailWeight, "detailWeight"},
                                {PbrTextureSlot::Reflection, "reflection"}});
JAH_ENUM_NAMES(DecalMap, {{DecalMap::Diffuse, "diffuse"}, {DecalMap::Normal, "normal"}, {DecalMap::Emissive, "emissive"}});

JAH_RECORD_COUNT(PbrParams::DetailLayer, 7);
JAH_RECORD_COUNT(PbrParams, 28);

}}}  // namespace jahshaka::engine::script
