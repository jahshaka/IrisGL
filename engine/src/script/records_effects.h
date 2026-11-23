#pragma once
// THE EFFECTS FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_effects.cpp. A node's decal (DecalDesc, Lights.h: its images decal images, never
// textures), its particle system (ParticleSystemDesc with its emitters and affectors, Particles.h) and the scene's
// planar reflections (PlanarReflectionParams, Planar.h). A billboard set's instances are FLAT arrays (the batch's
// form: engine.billboards.set), no record.
#include "script/records.h"
#include "script/records_shared.h"   // JAH_FIELD / JAH_ENUMERATOR: their one home
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Particles.h"
#include "jahshaka/engine/Planar.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(ParticleEmitterShape, {JAH_ENUMERATOR(ParticleEmitterShape, Point), JAH_ENUMERATOR(ParticleEmitterShape, Box),
                                      JAH_ENUMERATOR(ParticleEmitterShape, Cylinder), JAH_ENUMERATOR(ParticleEmitterShape, Ellipsoid),
                                      JAH_ENUMERATOR(ParticleEmitterShape, HollowEllipsoid),
                                      JAH_ENUMERATOR(ParticleEmitterShape, Ring)});
JAH_ENUM_NAMES(ParticleOrientation, {JAH_ENUMERATOR(ParticleOrientation, Point), JAH_ENUMERATOR(ParticleOrientation, OrientedCommon),
                                     JAH_ENUMERATOR(ParticleOrientation, OrientedSelf),
                                     JAH_ENUMERATOR(ParticleOrientation, PerpendicularCommon),
                                     JAH_ENUMERATOR(ParticleOrientation, PerpendicularSelf)});
JAH_ENUM_NAMES(ParticleAffectorDesc::Kind,
               {JAH_ENUMERATOR(ParticleAffectorDesc::Kind, ColourKeys), JAH_ENUMERATOR(ParticleAffectorDesc::Kind, ScaleKeys),
                JAH_ENUMERATOR(ParticleAffectorDesc::Kind, Rotator), JAH_ENUMERATOR(ParticleAffectorDesc::Kind, LinearForce),
                JAH_ENUMERATOR(ParticleAffectorDesc::Kind, Turbulence), JAH_ENUMERATOR(ParticleAffectorDesc::Kind, DeflectorPlane),
                JAH_ENUMERATOR(ParticleAffectorDesc::Kind, ColourFade), JAH_ENUMERATOR(ParticleAffectorDesc::Kind, ColourRamp),
                JAH_ENUMERATOR(ParticleAffectorDesc::Kind, ScaleRate)});
JAH_ENUM_NAMES(BillboardLayer, {JAH_ENUMERATOR(BillboardLayer, Scene), JAH_ENUMERATOR(BillboardLayer, Overlay)});

JAH_RECORD_COUNT(DecalDesc, 9);
JAH_RECORD_COUNT(ParticleEmitterDesc, 18);
JAH_RECORD_COUNT(ParticleAffectorDesc, 26);
JAH_RECORD_COUNT(ParticleSystemDesc, 10);
JAH_RECORD_COUNT(PlanarReflectionParams, 8);

/// THE DECLARED MAXIMA of a particle system's lists (a bound of the COMMAND, records.h: an array declares one; the
/// engine has none, and every list is frozen into its definition's topology).
constexpr unsigned kParticleEmittersMax = 16, kParticleAffectorsMax = 16;

}}}  // namespace jahshaka::engine::script
