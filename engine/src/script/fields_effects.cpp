// THE EFFECTS FAMILY's FIELD TABLES (SCRIPT-IN-ENGINE-1 stage 2): one per record records_effects.h declares, every field
// named once in declaration order (records.h). THE DOMAINS: what the engine clamps FAILS here by name (a particle quota
// 1..16000: quotaBucketFor, OgreParticles.cpp:109-110, kMaxParticleQuota; a key count up to 6 and key times 0..1:
// fillInterpStages, OgreParticles.cpp:185-197; the planar budget 0..8, its resolution 64..2048 and maxDistance >=
// 0.01: OgrePlanar.cpp:339-342 - the power of two is engine.planar.set's own check); a particle colour lies in the
// range its GPU encoding carries, [-4, 120] (Particles.h ParticleAffectorDesc); a decal's box has extent; a cone's
// half-angle, a scope, a metalness and a roughness are what they are (0..180 degrees, 0..1). A decal box's extent
// >= 0.001 m (OgreDecals.cpp:469-471); a particle's time to live >= 0.01 s (OgreParticles.cpp:618-619); the hole
// (innerExtents) a FRACTION 0..0.999 of the outer extents, its z never read (:636-637). What the engine would REPAIR
// silently - a minimum above its maximum swapped (:616-619), key times out of order bumped (:191) - engine.particles.set
// refuses by name.
#include "script/records_effects.h"

namespace jahshaka { namespace engine { namespace script {

namespace {
constexpr double kColourLo = -4.0, kColourHi = 120.0;   // the particle colour's GPU encoding (Particles.h)
}  // namespace

const Table<DecalDesc> &Record<DecalDesc>::table()
{
    using D = DecalDesc;
    static const Table<D> t({field("diffuse", &D::diffuse).handle("decalTexture"), field("normal", &D::normal).handle("decalTexture"),
                             field("emissive", &D::emissive).handle("decalTexture"), field("width", &D::width).atLeast(0.001).in("m"),
                             field("height", &D::height).atLeast(0.001).in("m"), field("depth", &D::depth).atLeast(0.001).in("m"),
                             field("metalness", &D::metalness).range(0, 1), field("roughness", &D::roughness).range(0, 1),
                             field("ignoreAlphaDiffuse", &D::ignoreAlphaDiffuse)});
    return t;
}

const Table<ParticleEmitterDesc> &Record<ParticleEmitterDesc>::table()
{
    using E = ParticleEmitterDesc;
    static const Table<E> t({field("shape", &E::shape), field("position", &E::position).in("m"), field("direction", &E::direction),
                             field("angleDegrees", &E::angleDegrees).range(0, 180).in("degrees"),
                             field("rate", &E::rate).atLeast(0).in("/s"), field("velocityMin", &E::velocityMin).atLeast(0).in("m/s"),
                             field("velocityMax", &E::velocityMax).atLeast(0).in("m/s"), field("ttlMin", &E::ttlMin).atLeast(0.01).in("s"),
                             field("ttlMax", &E::ttlMax).atLeast(0.01).in("s"), field("sizeWidth", &E::sizeWidth).atLeast(0).in("m"),
                             field("sizeHeight", &E::sizeHeight).atLeast(0).in("m"),
                             field("colourStart", &E::colourStart).range(kColourLo, kColourHi),
                             field("colourEnd", &E::colourEnd).range(kColourLo, kColourHi), field("extents", &E::extents).atLeast(0).in("m"),
                             field("innerExtents", &E::innerExtents).range(0, 0.999), field("duration", &E::duration).atLeast(0).in("s"),
                             field("repeatDelay", &E::repeatDelay).atLeast(0).in("s"), field("startTime", &E::startTime).atLeast(0).in("s")});
    return t;
}

const Table<ParticleAffectorDesc> &Record<ParticleAffectorDesc>::table()
{
    using A = ParticleAffectorDesc;
    static const Table<A> t({field("kind", &A::kind), field("keyCount", &A::keyCount).range(0, 6),
                             field("colourKeys", &A::colourKeys).range(kColourLo, kColourHi),
                             field("colourKeyTimes", &A::colourKeyTimes).range(0, 1), field("scaleKeys", &A::scaleKeys).atLeast(0),
                             field("scaleKeyTimes", &A::scaleKeyTimes).range(0, 1),
                             field("rotSpeedMin", &A::rotSpeedMin).in("degrees/s"), field("rotSpeedMax", &A::rotSpeedMax).in("degrees/s"),
                             field("rotStart", &A::rotStart).in("degrees"), field("rotEnd", &A::rotEnd).in("degrees"),
                             field("force", &A::force).in("m/s2"), field("forceAverage", &A::forceAverage),
                             field("randomness", &A::randomness).atLeast(0), field("scope", &A::scope).range(0, 1),
                             field("keepVelocity", &A::keepVelocity), field("planePoint", &A::planePoint).in("m"),
                             field("planeNormal", &A::planeNormal), field("bounce", &A::bounce).atLeast(0),
                             field("colourAdjust1", &A::colourAdjust1).in("/s"), field("colourAdjust2", &A::colourAdjust2).in("/s"),
                             field("colourSwitchAt", &A::colourSwitchAt).atLeast(0).in("s"),
                             field("colourMin", &A::colourMin).range(kColourLo, kColourHi),
                             field("colourMax", &A::colourMax).range(kColourLo, kColourHi), field("colourRampPath", &A::colourRampPath),
                             field("scaleRate", &A::scaleRate), field("scaleMultiply", &A::scaleMultiply)});
    return t;
}

const Table<ParticleSystemDesc> &Record<ParticleSystemDesc>::table()
{
    using S = ParticleSystemDesc;
    static const Table<S> t({field("quota", &S::quota).range(1, 16000), field("texture", &S::texture).handle("texture"),
                             field("additive", &S::additive), field("alphaHash", &S::alphaHash), field("distortion", &S::distortion),
                             field("orientation", &S::orientation), field("commonDirection", &S::commonDirection),
                             field("commonUp", &S::commonUp), field("emitters", &S::emitters).upTo(kParticleEmittersMax),
                             field("affectors", &S::affectors).upTo(kParticleAffectorsMax)});
    return t;
}

const Table<PlanarReflectionParams> &Record<PlanarReflectionParams>::table()
{
    using P = PlanarReflectionParams;
    static const Table<P> t({field("budget", &P::budget).range(0, 8), field("resolution", &P::resolution).range(64, 2048).in("px"),
                             field("mipmaps", &P::mipmaps), field("shadows", &P::shadows), field("hdr", &P::hdr),
                             field("accurateLighting", &P::accurateLighting), field("maxDistance", &P::maxDistance).atLeast(0.01).in("m"),
                             field("background", &P::background)});
    return t;
}

}}}  // namespace jahshaka::engine::script
