/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef PARTICLESYSTEMNODE_H
#define PARTICLESYSTEMNODE_H

#include "core/math/vec.h"
#include <QVector>

#include "irisglfwd.h"
#include "document/scenegraph/scenenode.h"
#include "core/irisutils.h"
#include "document/assets/texture2d.h"

namespace iris
{

/// One key of a colour-over-life or scale-over-life ramp. `time` is a fraction
/// of the particle's life, in [0, 1]. Colour components MAY exceed 1: the
/// engine's particle encoding carries HDR values, and that is what makes a
/// flame bloom instead of reading as an orange sticker.
struct ParticleColourKey {
    float time = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};
struct ParticleScaleKey {
    float time = 0.0f;
    float scale = 1.0f;   ///< multiplier on the emitter's particle size
};

/// The volume particles spawn inside. Point ignores the extents.
enum class ParticleEmitterShape { Point, Box, Cylinder, Ellipsoid, HollowEllipsoid, Ring };

/// How a particle's quad is oriented. Billboard is the camera-facing quad
/// everything used before the ParticleFX2 adoption; StretchedVelocity streaks
/// the quad along the particle's own direction (sparks, rain).
enum class ParticleOrientation { Billboard, StretchedCommon, StretchedVelocity,
                                 PerpendicularCommon, PerpendicularVelocity };

/// A named starting point for a whole emitter — what the "Particle Preset"
/// combo has claimed to offer since 2016 and never did.
enum class ParticlePreset : int { Custom, Fire, Embers, Smoke, Rain, Snow, SteadyFlow, Sparks };

/// An emitter, as the document authors it.
///
/// SINCE THE ParticleFX2 ADOPTION THIS CLASS SIMULATES NOTHING. It carries
/// authoring parameters; the engine owns every particle, integrates every force
/// and decides every colour. There is no `Particle` type, no live particle
/// list, and `update()` does not emit. (The simulation CLOCK is scene-level:
/// iris::Scene::particleTimeScale.)
///
/// The old fields keep their names and their meanings so old scenes read back
/// unchanged (`particlesPerSecond`, `speed`, `lifeLength`, `particleScale`,
/// `gravityComplement`, `dissipate`, `dissipateInv`, `randomRotation`,
/// `useAdditive`, `maxParticles`, `texture`) — they are simply mapped onto the
/// engine's emitter and affectors instead of onto a CPU loop:
///
///   dissipate     -> a scale ramp 1 -> 0   (by LIFE FRACTION, which is what the
///   dissipateInv  -> a scale ramp 0 -> 1    legacy lerp always meant to be)
///   gravityComplement -> a constant force of -50 * g on Y, the legacy constant
///   randomRotation    -> a random start angle in [0, 360)
///
/// ONE BEHAVIOUR CHANGE, deliberate: the node's SCALE no longer resizes the
/// spawn volume or the particles. The engine applies the node's position and
/// orientation to emission but not its scale, so the spawn volume is numeric
/// (`extents`) and the particle size is numeric (`particleScale`).
class ParticleSystemNode : public SceneNode
{
public:
    static ParticleSystemNodePtr create() {
        return ParticleSystemNodePtr(new ParticleSystemNode());
    }

    // ---- legacy authoring fields (serialized since 2016, unchanged meanings) --
    float particlesPerSecond;
    float speed;
    iris::Texture2DPtr texture;

    bool dissipate, dissipateInv;
    bool randomRotation;
    bool useAdditive;

    float gravityComplement;
    float lifeLength;
    float particleScale;

    int maxParticles;

    /// Spread around the mean, in ABSOLUTE units (speedError = 0.5 means
    /// speed +/- 0.5). The setters below take a FRACTION, which is what the
    /// panel's "Random ..." sliders have always sent.
    float speedError, lifeError, scaleError;

    // ---- ParticleFX2 authoring (PARTICLES_FX2_SPEC.md) -----------------------
    ParticleEmitterShape shape;
    /// Box: width/height/depth. Cylinder/Ellipsoid/Ring: radii. Point: unused.
    iris::Vec3 extents;
    /// HollowEllipsoid / Ring: the hole, as a fraction of `extents` in [0, 1).
    iris::Vec3 innerExtents;
    /// Emission cone half-angle around the node's +Y, in degrees. 0 = a beam.
    float coneAngle;
    /// Per-particle emission colour, picked between the two at birth. This is
    /// the FLAT tint; the ramp below is what changes over a particle's life.
    QColor emitColourStart, emitColourEnd;
    /// Bursts. duration 0 = emit forever.
    float burstDuration, burstRepeatDelay, startDelay;

    /// Colour over life, up to 6 keys. Empty = no ramp (particles keep the
    /// emission colour). This is the single lever that turns quads into fire.
    QVector<ParticleColourKey> colourKeys;
    /// Scale over life, up to 6 keys, as multipliers of `particleScale`. Empty
    /// = constant size, unless `dissipate`/`dissipateInv` synthesise a ramp.
    QVector<ParticleScaleKey> scaleKeys;

    /// Random velocity perturbation each frame. 0 = off (and the engine then
    /// omits the affector entirely, which is not free to add).
    float turbulence;
    /// A constant world-space force on top of gravity: wind, buoyancy, updraft.
    iris::Vec3 wind;
    /// Spin, degrees per second, picked per particle in [min, max].
    float rotationSpeedMin, rotationSpeedMax;

    ParticleOrientation orientation;
    /// Alpha-blended systems only: stochastic (order-independent) transparency.
    /// Ignored when `useAdditive`, which needs no sorting at all.
    bool alphaHash;

    /// The preset this emitter was last stamped from, purely so the UI can show
    /// it. Editing any field afterwards does NOT reset it to Custom — the
    /// preset is a starting point, not a mode.
    ParticlePreset preset;

    // (The particle simulation clock is NOT here. The renderer has one
    // frame-time source for the whole process, so a per-emitter clock cannot
    // exist — it lives on iris::Scene as `particleTimeScale`, which is the
    // finest granularity that is not a lie.)

    // ---- setters kept for the panel and for source compatibility -------------
    void setRandomRotation(bool val)      { randomRotation = val; }
    void setBlendMode(bool useAddittive)  { useAdditive = useAddittive; }
    void setDissipation(bool b)           { dissipate = b; }
    void setDissipationInv(bool b)        { dissipateInv = b; }
    void setParticleScale(float scale)    { particleScale = scale; }
    void setPPS(float pps)                { particlesPerSecond = pps; }
    float getPPS() const                  { return particlesPerSecond; }
    void setGravity(float g)              { gravityComplement = g; }
    float getGravity() const              { return gravityComplement; }
    void setLife(float ll)                { lifeLength = ll; }
    float getLife() const                 { return lifeLength; }
    void setSpeed(float s)                { speed = s; }
    float getSpeed() const                { return speed; }
    void setTexture(QSharedPointer<iris::Texture2D> tex) { texture = tex; }

    /// The "Random ..." sliders send a FRACTION of the mean; the fields hold
    /// the absolute spread. (These three were edited by the panel and never
    /// serialized until the ParticleFX2 adoption — audit defect #7.)
    void setSpeedError(float fraction)  { speedError = fraction * speed; }
    void setLifeError(float fraction)   { lifeError = fraction * lifeLength; }
    void setScaleError(float fraction)  { scaleError = fraction * particleScale; }
    float speedErrorFraction() const  { return speed        > 0 ? speedError / speed        : 0.0f; }
    float lifeErrorFraction() const   { return lifeLength   > 0 ? lifeError  / lifeLength   : 0.0f; }
    float scaleErrorFraction() const  { return particleScale> 0 ? scaleError / particleScale: 0.0f; }

    /// Stamps a whole emitter from a named recipe. Everything except the node's
    /// identity, transform and texture is overwritten — this is the verb behind
    /// the preset combo and behind `particles.preset`.
    void applyPreset(ParticlePreset p);
    /// Resets every AUTHORING field to its default, and nothing else. What the
    /// constructor and applyPreset both start from.
    void resetAuthoringDefaults();

    static QString presetName(ParticlePreset p);
    static ParticlePreset presetFromName(const QString &name);
    static QStringList presetNames();
    static QString shapeName(ParticleEmitterShape s);
    static ParticleEmitterShape shapeFromName(const QString &name);
    static QString orientationName(ParticleOrientation o);
    static ParticleOrientation orientationFromName(const QString &name);

    /// Pure transform refresh — this node has not simulated anything since the
    /// ParticleFX2 adoption. Kept because SceneNode::update is what walks
    /// children, and because callers still tick the graph.
    void update(float delta) override;

    QList<Property*> getProperties() override;
    QVariant getPropertyValue(QString valueName) override;
    bool setPropertyValue(QString valueName, const QVariant &value) override;

    ~ParticleSystemNode() override;
    SceneNodePtr createDuplicate() override;
    ParticleSystemNode();

    MaterialPtr material;
};

}

Q_DECLARE_METATYPE(iris::ParticleSystemNode)
Q_DECLARE_METATYPE(iris::ParticleSystemNodePtr)

#endif // PARTICLESYSTEMNODE_H
