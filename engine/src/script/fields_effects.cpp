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
    static const Table<D> t({JAH_FIELD(D, diffuse).handle("decalTexture"), JAH_FIELD(D, normal).handle("decalTexture"),
                             JAH_FIELD(D, emissive).handle("decalTexture"), JAH_FIELD(D, width).atLeast(0.001).in("m"),
                             JAH_FIELD(D, height).atLeast(0.001).in("m"), JAH_FIELD(D, depth).atLeast(0.001).in("m"),
                             JAH_FIELD(D, metalness).range(0, 1), JAH_FIELD(D, roughness).range(0, 1),
                             JAH_FIELD(D, ignoreAlphaDiffuse)});
    return t;
}

const Table<ParticleEmitterDesc> &Record<ParticleEmitterDesc>::table()
{
    using E = ParticleEmitterDesc;
    static const Table<E> t({JAH_FIELD(E, shape), JAH_FIELD(E, position).in("m"), JAH_FIELD(E, direction),
                             JAH_FIELD(E, angleDegrees).range(0, 180).in("degrees"),
                             JAH_FIELD(E, rate).atLeast(0).in("/s"), JAH_FIELD(E, velocityMin).atLeast(0).in("m/s"),
                             JAH_FIELD(E, velocityMax).atLeast(0).in("m/s"), JAH_FIELD(E, ttlMin).atLeast(0.01).in("s"),
                             JAH_FIELD(E, ttlMax).atLeast(0.01).in("s"), JAH_FIELD(E, sizeWidth).atLeast(0).in("m"),
                             JAH_FIELD(E, sizeHeight).atLeast(0).in("m"),
                             JAH_FIELD(E, colourStart).range(kColourLo, kColourHi),
                             JAH_FIELD(E, colourEnd).range(kColourLo, kColourHi), JAH_FIELD(E, extents).atLeast(0).in("m"),
                             JAH_FIELD(E, innerExtents).range(0, 0.999), JAH_FIELD(E, duration).atLeast(0).in("s"),
                             JAH_FIELD(E, repeatDelay).atLeast(0).in("s"), JAH_FIELD(E, startTime).atLeast(0).in("s")});
    return t;
}

const Table<ParticleAffectorDesc> &Record<ParticleAffectorDesc>::table()
{
    using A = ParticleAffectorDesc;
    static const Table<A> t({JAH_FIELD(A, kind), JAH_FIELD(A, keyCount).range(0, 6),
                             JAH_FIELD(A, colourKeys).range(kColourLo, kColourHi),
                             JAH_FIELD(A, colourKeyTimes).range(0, 1), JAH_FIELD(A, scaleKeys).atLeast(0),
                             JAH_FIELD(A, scaleKeyTimes).range(0, 1),
                             JAH_FIELD(A, rotSpeedMin).in("degrees/s"), JAH_FIELD(A, rotSpeedMax).in("degrees/s"),
                             JAH_FIELD(A, rotStart).in("degrees"), JAH_FIELD(A, rotEnd).in("degrees"),
                             JAH_FIELD(A, force).in("m/s2"), JAH_FIELD(A, forceAverage),
                             JAH_FIELD(A, randomness).atLeast(0), JAH_FIELD(A, scope).range(0, 1),
                             JAH_FIELD(A, keepVelocity), JAH_FIELD(A, planePoint).in("m"),
                             JAH_FIELD(A, planeNormal), JAH_FIELD(A, bounce).atLeast(0),
                             JAH_FIELD(A, colourAdjust1).in("/s"), JAH_FIELD(A, colourAdjust2).in("/s"),
                             JAH_FIELD(A, colourSwitchAt).atLeast(0).in("s"),
                             JAH_FIELD(A, colourMin).range(kColourLo, kColourHi),
                             JAH_FIELD(A, colourMax).range(kColourLo, kColourHi), JAH_FIELD(A, colourRampPath),
                             JAH_FIELD(A, scaleRate), JAH_FIELD(A, scaleMultiply)});
    return t;
}

const Table<ParticleSystemDesc> &Record<ParticleSystemDesc>::table()
{
    using S = ParticleSystemDesc;
    static const Table<S> t({JAH_FIELD(S, quota).range(1, 16000), JAH_FIELD(S, texture).handle("texture"),
                             JAH_FIELD(S, additive), JAH_FIELD(S, alphaHash), JAH_FIELD(S, distortion),
                             JAH_FIELD(S, orientation), JAH_FIELD(S, commonDirection),
                             JAH_FIELD(S, commonUp), JAH_FIELD(S, emitters).upTo(kParticleEmittersMax),
                             JAH_FIELD(S, affectors).upTo(kParticleAffectorsMax)});
    return t;
}

const Table<PlanarReflectionParams> &Record<PlanarReflectionParams>::table()
{
    using P = PlanarReflectionParams;
    static const Table<P> t({JAH_FIELD(P, budget).range(0, 8), JAH_FIELD(P, resolution).range(64, 2048).in("px"),
                             JAH_FIELD(P, mipmaps), JAH_FIELD(P, shadows), JAH_FIELD(P, hdr),
                             JAH_FIELD(P, accurateLighting), JAH_FIELD(P, maxDistance).atLeast(0.01).in("m"),
                             JAH_FIELD(P, background)});
    return t;
}

}}}  // namespace jahshaka::engine::script
