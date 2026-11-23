#pragma once
// THE MATERIAL FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_material.cpp. The material commands' PbrParams (with its detail layer), and the
// names of the enums the material and texture commands take (a texture slot, a decal image's atlas).
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Materials.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(PbrAlphaMode, {JAH_ENUMERATOR(PbrAlphaMode, Opaque), JAH_ENUMERATOR(PbrAlphaMode, Cutout), JAH_ENUMERATOR(PbrAlphaMode, Blend),
                              JAH_ENUMERATOR(PbrAlphaMode, Glass), JAH_ENUMERATOR(PbrAlphaMode, Additive),
                              JAH_ENUMERATOR(PbrAlphaMode, Modulate), JAH_ENUMERATOR(PbrAlphaMode, Refractive)});
JAH_ENUM_NAMES(ShadingModel, {JAH_ENUMERATOR(ShadingModel, Lit), JAH_ENUMERATOR(ShadingModel, Unlit), JAH_ENUMERATOR(ShadingModel, Distortion)});
JAH_ENUM_NAMES(PbrParams::Workflow, {JAH_ENUMERATOR(PbrParams::Workflow, Metallic), JAH_ENUMERATOR(PbrParams::Workflow, Specular),
                                     JAH_ENUMERATOR(PbrParams::Workflow, SpecularAsFresnel)});
JAH_ENUM_NAMES(PbrParams::AddressMode, {JAH_ENUMERATOR(PbrParams::AddressMode, Wrap), JAH_ENUMERATOR(PbrParams::AddressMode, Clamp),
                                        JAH_ENUMERATOR(PbrParams::AddressMode, Mirror), JAH_ENUMERATOR(PbrParams::AddressMode, Border)});
/// (Count is no slot: it is not named, and no command answers it)
JAH_ENUM_NAMES(PbrTextureSlot, {JAH_ENUMERATOR(PbrTextureSlot, Albedo), JAH_ENUMERATOR(PbrTextureSlot, Normal),
                                JAH_ENUMERATOR(PbrTextureSlot, Metalness), JAH_ENUMERATOR(PbrTextureSlot, Roughness),
                                JAH_ENUMERATOR(PbrTextureSlot, Emissive), JAH_ENUMERATOR(PbrTextureSlot, Detail0),
                                JAH_ENUMERATOR(PbrTextureSlot, Detail1), JAH_ENUMERATOR(PbrTextureSlot, Detail0Nm),
                                JAH_ENUMERATOR(PbrTextureSlot, Detail1Nm), JAH_ENUMERATOR(PbrTextureSlot, DetailWeight),
                                JAH_ENUMERATOR(PbrTextureSlot, Reflection)});
JAH_ENUM_NAMES(DecalMap, {JAH_ENUMERATOR(DecalMap, Diffuse), JAH_ENUMERATOR(DecalMap, Normal), JAH_ENUMERATOR(DecalMap, Emissive)});

JAH_RECORD_COUNT(PbrParams::DetailLayer, 7);
JAH_RECORD_COUNT(PbrParams, 28);

}}}  // namespace jahshaka::engine::script
