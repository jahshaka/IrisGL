#pragma once
// THE EFFECTS FAMILY's RECORDS (SCRIPT-IN-ENGINE-1 stage 2, keyed by family: records.h says how) - every record its
// commands take or answer, declared with JAH_RECORD_COUNT beside its table's declaration, and its enums with
// JAH_ENUM_NAMES; the tables in fields_effects.cpp. A node's decal (DecalDesc, Lights.h: its images decal images, never
// textures), its particle system (ParticleSystemDesc with its emitters and affectors, Particles.h) and the scene's
// planar reflections (PlanarReflectionParams, Planar.h). A billboard set's instances are FLAT arrays (the batch's
// form: engine.billboards.set), no record.
#include "script/records.h"
#include "jahshaka/engine/Lights.h"
#include "jahshaka/engine/Particles.h"
#include "jahshaka/engine/Planar.h"

namespace jahshaka { namespace engine { namespace script {

JAH_ENUM_NAMES(ParticleEmitterShape, {{ParticleEmitterShape::Point, "point"}, {ParticleEmitterShape::Box, "box"},
                                      {ParticleEmitterShape::Cylinder, "cylinder"}, {ParticleEmitterShape::Ellipsoid, "ellipsoid"},
                                      {ParticleEmitterShape::HollowEllipsoid, "hollowEllipsoid"},
                                      {ParticleEmitterShape::Ring, "ring"}});
JAH_ENUM_NAMES(ParticleOrientation, {{ParticleOrientation::Point, "point"}, {ParticleOrientation::OrientedCommon, "orientedCommon"},
                                     {ParticleOrientation::OrientedSelf, "orientedSelf"},
                                     {ParticleOrientation::PerpendicularCommon, "perpendicularCommon"},
                                     {ParticleOrientation::PerpendicularSelf, "perpendicularSelf"}});
JAH_ENUM_NAMES(ParticleAffectorDesc::Kind,
               {{ParticleAffectorDesc::Kind::ColourKeys, "colourKeys"}, {ParticleAffectorDesc::Kind::ScaleKeys, "scaleKeys"},
                {ParticleAffectorDesc::Kind::Rotator, "rotator"}, {ParticleAffectorDesc::Kind::LinearForce, "linearForce"},
                {ParticleAffectorDesc::Kind::Turbulence, "turbulence"}, {ParticleAffectorDesc::Kind::DeflectorPlane, "deflectorPlane"},
                {ParticleAffectorDesc::Kind::ColourFade, "colourFade"}, {ParticleAffectorDesc::Kind::ColourRamp, "colourRamp"},
                {ParticleAffectorDesc::Kind::ScaleRate, "scaleRate"}});
JAH_ENUM_NAMES(BillboardLayer, {{BillboardLayer::Scene, "scene"}, {BillboardLayer::Overlay, "overlay"}});

JAH_RECORD_COUNT(DecalDesc, 9);
JAH_RECORD_COUNT(ParticleEmitterDesc, 18);
JAH_RECORD_COUNT(ParticleAffectorDesc, 26);
JAH_RECORD_COUNT(ParticleSystemDesc, 10);
JAH_RECORD_COUNT(PlanarReflectionParams, 8);

/// THE DECLARED MAXIMA of a particle system's lists (a bound of the COMMAND, records.h: an array declares one; the
/// engine has none, and every list is frozen into its definition's topology).
constexpr unsigned kParticleEmittersMax = 16, kParticleAffectorsMax = 16;

}}}  // namespace jahshaka::engine::script
