/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/qtinterop.h"
#include "core/math/vec.h"
#include <QColor>

#include "core/properties/property.h"
#include "document/scenegraph/particlesystemnode.h"

namespace iris
{

ParticleSystemNode::ParticleSystemNode() {
    sceneNodeType = SceneNodeType::ParticleSystem;
    texture = iris::Texture2D::load(":assets/textures/default_particle.jpg");
    resetAuthoringDefaults();
}

void ParticleSystemNode::resetAuthoringDefaults()
{
    // EXACTLY the authoring fields — never identity, transform, children or the
    // texture. applyPreset() relies on that: a recipe replaces the recipe, not
    // the node.
    particlesPerSecond = 24;
    speed = 12;

    useAdditive = true;
    randomRotation = true;
    dissipate = true;
    dissipateInv = false;

    gravityComplement = 0.0f;
    particleScale = 1.0f;
    lifeLength = 1.0f;

    // Unenforced cap (audit: was uninitialized, copied by createDuplicate —
    // latent UB). Since the ParticleFX2 adoption this is REAL: it becomes the
    // engine definition's quota, rounded up to a bucket. 0 = let the engine
    // pick its default.
    maxParticles = 0;

    speedError = lifeError = scaleError = 0;

    shape = ParticleEmitterShape::Point;
    extents = iris::Vec3(1, 1, 1);
    innerExtents = iris::Vec3(0, 0, 0);
    coneAngle = 0.0f;
    emitColourStart = QColor(255, 255, 255);
    emitColourEnd = QColor(255, 255, 255);
    burstDuration = burstRepeatDelay = startDelay = 0.0f;
    colourKeys.clear();
    scaleKeys.clear();
    turbulence = 0.0f;
    wind = iris::Vec3(0, 0, 0);
    rotationSpeedMin = rotationSpeedMax = 0.0f;
    orientation = ParticleOrientation::Billboard;
    alphaHash = true;
    preset = ParticlePreset::Custom;
}

ParticleSystemNode::~ParticleSystemNode() = default;

void ParticleSystemNode::update(float delta) {
    // Nothing else. The engine simulates: this node holds only authoring
    // parameters, and SceneNode::update walks the children. It used to
    // integrate a std::vector<Particle*> here, allocating one particle per
    // emission and erasing from the middle of the vector to kill them.
    SceneNode::update(delta);
}

// ---- presets ---------------------------------------------------------------
// The "Particle Preset" combo has offered Fire/Smoke/Rain/Snow/Steady Flow/
// Chaos/Sparks since 2016 with nothing behind it. These are the recipes.
//
// Colour values above 1 are intentional and load-bearing: the engine's particle
// colour encoding is HDR, and a flame only reads as a flame when its core is
// brighter than white and blooms. Without HDR + bloom on the view these clamp
// and the fire looks like an orange sticker — which is a POST-CHAIN setting,
// not a particle one.

static ParticleColourKey ck(float t, float r, float g, float b, float a) {
    ParticleColourKey k; k.time = t; k.r = r; k.g = g; k.b = b; k.a = a; return k;
}
static ParticleScaleKey sk(float t, float s) {
    ParticleScaleKey k; k.time = t; k.scale = s; return k;
}

void ParticleSystemNode::applyPreset(ParticlePreset p)
{
    // Start from the authoring defaults so a preset never inherits half of the
    // previous one — but touch NOTHING the node is: not its guid, not its name,
    // not its transform, not its children, and not its texture (which the user
    // picked and a recipe has no business replacing).
    resetAuthoringDefaults();
    preset = p;

    switch (p) {
    case ParticlePreset::Custom:
        break;

    case ParticlePreset::Fire:
        // The flame body. Additive HDR quads, a warm-to-dark ramp by life
        // fraction, a scale ramp that blooms then pinches, buoyancy instead of
        // gravity, and turbulence for the flicker.
        particlesPerSecond = 110.0f;
        speed = 2.2f;   speedError = 0.7f;
        lifeLength = 0.95f; lifeError = 0.25f;
        particleScale = 0.35f;
        coneAngle = 15.0f;
        gravityComplement = 0.0f;
        wind = iris::Vec3(0.0f, 1.2f, 0.0f);
        turbulence = 2.5f;
        randomRotation = true;
        rotationSpeedMin = -40.0f; rotationSpeedMax = 90.0f;
        useAdditive = true;
        dissipate = false; dissipateInv = false;
        maxParticles = 1024;
        colourKeys = { ck(0.00f, 4.00f, 1.60f, 0.35f, 1.0f),
                       ck(0.25f, 2.20f, 0.70f, 0.10f, 1.0f),
                       ck(0.55f, 0.90f, 0.18f, 0.03f, 0.8f),
                       ck(1.00f, 0.05f, 0.02f, 0.02f, 0.0f) };
        scaleKeys  = { sk(0.00f, 0.60f), sk(0.35f, 1.00f), sk(1.00f, 0.45f) };
        break;

    case ParticlePreset::Embers:
        // Sparks torn off the flame. PFX2 has no sub-emitters, so embers are a
        // second node, not a child of the fire.
        particlesPerSecond = 10.0f;
        speed = 3.0f;   speedError = 1.5f;
        lifeLength = 2.2f; lifeError = 0.7f;
        particleScale = 0.06f;
        coneAngle = 35.0f;
        gravityComplement = 0.04f;      // -50 * 0.04 = -2 m/s^2, a slow fall
        wind = iris::Vec3(0.3f, 0.9f, 0.0f);
        turbulence = 3.0f;
        useAdditive = true;
        maxParticles = 256;
        orientation = ParticleOrientation::StretchedVelocity;
        colourKeys = { ck(0.00f, 6.00f, 3.00f, 0.60f, 1.0f),
                       ck(0.40f, 3.00f, 0.80f, 0.05f, 1.0f),
                       ck(1.00f, 0.20f, 0.02f, 0.00f, 0.0f) };
        scaleKeys  = { sk(0.00f, 1.00f), sk(1.00f, 0.30f) };
        break;

    case ParticlePreset::Smoke:
        particlesPerSecond = 12.0f;
        speed = 1.0f;   speedError = 0.3f;
        lifeLength = 3.2f; lifeError = 0.8f;
        particleScale = 0.7f;
        shape = ParticleEmitterShape::Ellipsoid;
        extents = iris::Vec3(0.4f, 0.2f, 0.4f);
        coneAngle = 20.0f;
        wind = iris::Vec3(0.3f, 0.8f, 0.0f);
        turbulence = 1.0f;
        randomRotation = true;
        rotationSpeedMin = -12.0f; rotationSpeedMax = 12.0f;
        useAdditive = false;    // smoke OCCLUDES; additive smoke is a glow
        alphaHash = true;       // and unsorted alpha needs stochastic transparency
        maxParticles = 512;
        colourKeys = { ck(0.00f, 0.35f, 0.34f, 0.33f, 0.00f),
                       ck(0.15f, 0.30f, 0.29f, 0.28f, 0.55f),
                       ck(0.60f, 0.22f, 0.22f, 0.22f, 0.35f),
                       ck(1.00f, 0.18f, 0.18f, 0.18f, 0.00f) };
        scaleKeys  = { sk(0.00f, 0.50f), sk(1.00f, 2.50f) };
        break;

    case ParticlePreset::Rain:
        particlesPerSecond = 400.0f;
        speed = 6.0f;   speedError = 1.0f;
        lifeLength = 1.6f;
        particleScale = 0.05f;
        shape = ParticleEmitterShape::Box;
        extents = iris::Vec3(12.0f, 0.2f, 12.0f);
        gravityComplement = 0.35f;
        useAdditive = false;
        alphaHash = true;
        orientation = ParticleOrientation::StretchedVelocity;
        maxParticles = 4096;
        colourKeys = { ck(0.0f, 0.55f, 0.65f, 0.85f, 0.6f),
                       ck(1.0f, 0.55f, 0.65f, 0.85f, 0.4f) };
        break;

    case ParticlePreset::Snow:
        particlesPerSecond = 120.0f;
        speed = 0.6f;   speedError = 0.3f;
        lifeLength = 6.0f; lifeError = 1.5f;
        particleScale = 0.09f;
        shape = ParticleEmitterShape::Box;
        extents = iris::Vec3(12.0f, 0.2f, 12.0f);
        gravityComplement = 0.02f;
        wind = iris::Vec3(0.4f, 0.0f, 0.0f);
        turbulence = 1.2f;
        randomRotation = true;
        rotationSpeedMin = -30.0f; rotationSpeedMax = 30.0f;
        useAdditive = false;
        alphaHash = true;
        maxParticles = 2048;
        colourKeys = { ck(0.0f, 1.0f, 1.0f, 1.0f, 0.9f),
                       ck(1.0f, 1.0f, 1.0f, 1.0f, 0.5f) };
        break;

    case ParticlePreset::SteadyFlow:
        // The honest default: what the old simulator did, with none of its
        // randomness — a straight, even, constant-size stream.
        particlesPerSecond = 60.0f;
        speed = 3.0f;
        lifeLength = 2.0f;
        particleScale = 0.3f;
        coneAngle = 4.0f;
        useAdditive = true;
        maxParticles = 512;
        break;

    case ParticlePreset::Sparks:
        particlesPerSecond = 60.0f;
        speed = 7.0f;   speedError = 3.0f;
        lifeLength = 0.9f; lifeError = 0.4f;
        particleScale = 0.05f;
        coneAngle = 70.0f;
        gravityComplement = 0.2f;
        useAdditive = true;
        orientation = ParticleOrientation::StretchedVelocity;
        maxParticles = 512;
        colourKeys = { ck(0.00f, 8.00f, 5.00f, 1.50f, 1.0f),
                       ck(0.55f, 3.00f, 0.90f, 0.10f, 1.0f),
                       ck(1.00f, 0.30f, 0.03f, 0.00f, 0.0f) };
        scaleKeys  = { sk(0.0f, 1.0f), sk(1.0f, 0.4f) };
        break;
    }
}

// ---- name tables -----------------------------------------------------------
// Serialized and scripted by NAME, never by ordinal: an enum reshuffle must not
// silently repaint every saved scene.

QString ParticleSystemNode::presetName(ParticlePreset p)
{
    switch (p) {
    case ParticlePreset::Fire:       return "fire";
    case ParticlePreset::Embers:     return "embers";
    case ParticlePreset::Smoke:      return "smoke";
    case ParticlePreset::Rain:       return "rain";
    case ParticlePreset::Snow:       return "snow";
    case ParticlePreset::SteadyFlow: return "steadyFlow";
    case ParticlePreset::Sparks:     return "sparks";
    case ParticlePreset::Custom:     break;
    }
    return "custom";
}

ParticlePreset ParticleSystemNode::presetFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == "fire")       return ParticlePreset::Fire;
    if (n == "embers")     return ParticlePreset::Embers;
    if (n == "smoke")      return ParticlePreset::Smoke;
    if (n == "rain")       return ParticlePreset::Rain;
    if (n == "snow")       return ParticlePreset::Snow;
    if (n == "steadyflow") return ParticlePreset::SteadyFlow;
    if (n == "sparks")     return ParticlePreset::Sparks;
    return ParticlePreset::Custom;
}

QStringList ParticleSystemNode::presetNames()
{
    return { "custom", "fire", "embers", "smoke", "rain", "snow", "steadyFlow", "sparks" };
}

QString ParticleSystemNode::shapeName(ParticleEmitterShape s)
{
    switch (s) {
    case ParticleEmitterShape::Box:             return "box";
    case ParticleEmitterShape::Cylinder:        return "cylinder";
    case ParticleEmitterShape::Ellipsoid:       return "ellipsoid";
    case ParticleEmitterShape::HollowEllipsoid: return "hollowEllipsoid";
    case ParticleEmitterShape::Ring:            return "ring";
    case ParticleEmitterShape::Point:           break;
    }
    return "point";
}

ParticleEmitterShape ParticleSystemNode::shapeFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == "box")             return ParticleEmitterShape::Box;
    if (n == "cylinder")        return ParticleEmitterShape::Cylinder;
    if (n == "ellipsoid")       return ParticleEmitterShape::Ellipsoid;
    if (n == "hollowellipsoid") return ParticleEmitterShape::HollowEllipsoid;
    if (n == "ring")            return ParticleEmitterShape::Ring;
    return ParticleEmitterShape::Point;
}

QString ParticleSystemNode::orientationName(ParticleOrientation o)
{
    switch (o) {
    case ParticleOrientation::StretchedCommon:      return "stretchedCommon";
    case ParticleOrientation::StretchedVelocity:    return "stretchedVelocity";
    case ParticleOrientation::PerpendicularCommon:  return "perpendicularCommon";
    case ParticleOrientation::PerpendicularVelocity:return "perpendicularVelocity";
    case ParticleOrientation::Billboard:            break;
    }
    return "billboard";
}

ParticleOrientation ParticleSystemNode::orientationFromName(const QString &name)
{
    const QString n = name.trimmed().toLower();
    if (n == "stretchedcommon")       return ParticleOrientation::StretchedCommon;
    if (n == "stretchedvelocity")     return ParticleOrientation::StretchedVelocity;
    if (n == "perpendicularcommon")   return ParticleOrientation::PerpendicularCommon;
    if (n == "perpendicularvelocity") return ParticleOrientation::PerpendicularVelocity;
    return ParticleOrientation::Billboard;
}

// ---- reflection ------------------------------------------------------------
// Covers exactly what SceneWriter::writeParticleData serializes, under the same
// key names, so the scripting surface and the save file never drift. Two keys
// are handled elsewhere: "guid" is identity and "visible" comes from SceneNode.
//
// The ramps (colourKeys/scaleKeys) are NOT reflected as scalar properties —
// they are lists, and Property has no list kind. They travel through the
// serializer and through the `particles` API module instead.

QList<Property*> ParticleSystemNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto addFloat = [&props](const char *display, const char *name, float value) {
        auto *p = new FloatProperty();
        p->displayName = display; p->name = name; p->value = value;
        props.append(p);
    };
    auto addBool = [&props](const char *display, const char *name, bool value) {
        auto *p = new BoolProperty();
        p->displayName = display; p->name = name; p->value = value;
        props.append(p);
    };

    addFloat("Particles Per Second", "particlesPerSecond", particlesPerSecond);
    addFloat("Particle Scale",       "particleScale",      particleScale);
    addFloat("Gravity Complement",   "gravityComplement",  gravityComplement);
    addFloat("Life Length",          "lifeLength",         lifeLength);
    addFloat("Speed",                "speed",              speed);
    addFloat("Random Speed",         "speedError",         speedError);
    addFloat("Random Lifetime",      "lifeError",          lifeError);
    addFloat("Random Scale",         "scaleError",         scaleError);
    addBool ("Dissipate",            "dissipate",          dissipate);
    addBool ("Dissipate Inverted",   "dissipateInv",       dissipateInv);
    addBool ("Random Rotation",      "randomRotation",     randomRotation);
    addBool ("Additive Blending",    "blendMode",          useAdditive);
    addBool ("Alpha Hashing",        "alphaHash",          alphaHash);

    addFloat("Cone Angle",           "coneAngle",          coneAngle);
    addFloat("Turbulence",           "turbulence",         turbulence);
    addFloat("Rotation Speed Min",   "rotationSpeedMin",   rotationSpeedMin);
    addFloat("Rotation Speed Max",   "rotationSpeedMax",   rotationSpeedMax);
    addFloat("Burst Duration",       "burstDuration",      burstDuration);
    addFloat("Burst Repeat Delay",   "burstRepeatDelay",   burstRepeatDelay);
    addFloat("Start Delay",          "startDelay",         startDelay);

    auto *intProp = new IntProperty();
    intProp->displayName = "Max Particles";
    intProp->name = "maxParticles";
    intProp->value = maxParticles;
    props.append(intProp);

    auto addVec3 = [&props](const char *display, const char *name, const iris::Vec3 &value) {
        auto *p = new Vec3Property();
        p->displayName = display; p->name = name; p->value = iris::toQt(value);
        props.append(p);
    };
    addVec3("Extents",       "extents",      extents);
    addVec3("Inner Extents", "innerExtents", innerExtents);
    addVec3("Wind",          "wind",         wind);

    auto addColour = [&props](const char *display, const char *name, const QColor &value) {
        auto *p = new ColorProperty();
        p->displayName = display; p->name = name; p->value = value;
        props.append(p);
    };
    addColour("Emit Colour Start", "emitColourStart", emitColourStart);
    addColour("Emit Colour End",   "emitColourEnd",   emitColourEnd);

    // Enum-ish rows travel as their NAME, never as an ordinal (see the tables
    // above). FileProperty is the QString-valued Property here — the same one
    // the texture row has always used.
    auto addString = [&props](const char *display, const char *name, const QString &value) {
        auto *p = new FileProperty();
        p->displayName = display; p->name = name; p->value = value;
        props.append(p);
    };
    addString("Emitter Shape", "shape",       shapeName(shape));
    addString("Orientation",   "orientation", orientationName(orientation));
    addString("Preset",        "preset",      presetName(preset));

    // READ-ONLY: the node holds a Texture2DPtr, and binding one needs the asset
    // manager (guid -> file), which the document layer has no access to. The
    // texture's source path is exposed for inspection only; setPropertyValue
    // refuses it.
    addString("Texture", "texture", !!texture ? texture->getSource() : QString());

    return props;
}

QVariant ParticleSystemNode::getPropertyValue(QString valueName)
{
    if (valueName == "particlesPerSecond") return particlesPerSecond;
    if (valueName == "particleScale")      return particleScale;
    if (valueName == "gravityComplement")  return gravityComplement;
    if (valueName == "lifeLength")         return lifeLength;
    if (valueName == "speed")              return speed;
    if (valueName == "speedError")         return speedError;
    if (valueName == "lifeError")          return lifeError;
    if (valueName == "scaleError")         return scaleError;
    if (valueName == "dissipate")          return dissipate;
    if (valueName == "dissipateInv")       return dissipateInv;
    if (valueName == "randomRotation")     return randomRotation;
    if (valueName == "blendMode")          return useAdditive;
    if (valueName == "alphaHash")          return alphaHash;
    if (valueName == "coneAngle")          return coneAngle;
    if (valueName == "turbulence")         return turbulence;
    if (valueName == "rotationSpeedMin")   return rotationSpeedMin;
    if (valueName == "rotationSpeedMax")   return rotationSpeedMax;
    if (valueName == "burstDuration")      return burstDuration;
    if (valueName == "burstRepeatDelay")   return burstRepeatDelay;
    if (valueName == "startDelay")         return startDelay;
    if (valueName == "maxParticles")       return maxParticles;
    if (valueName == "shape")              return shapeName(shape);
    if (valueName == "orientation")        return orientationName(orientation);
    if (valueName == "preset")             return presetName(preset);
    if (valueName == "texture")            return !!texture ? texture->getSource() : QString();
    // Vector rows the panel and the API module edit but the Property system has
    // no kind for. Returned as iris::Vec3; setPropertyValue accepts the same.
    if (valueName == "extents")            return iris::toQt(extents);
    if (valueName == "innerExtents")       return iris::toQt(innerExtents);
    if (valueName == "wind")               return iris::toQt(wind);
    if (valueName == "emitColourStart")    return emitColourStart;
    if (valueName == "emitColourEnd")      return emitColourEnd;

    return SceneNode::getPropertyValue(valueName);
}

bool ParticleSystemNode::setPropertyValue(QString valueName, const QVariant &value)
{
    if (valueName == "particlesPerSecond") { particlesPerSecond = value.toFloat();   return true; }
    if (valueName == "particleScale")      { particleScale      = value.toFloat();   return true; }
    if (valueName == "gravityComplement")  { gravityComplement  = value.toFloat();   return true; }
    if (valueName == "lifeLength")         { lifeLength         = value.toFloat();   return true; }
    if (valueName == "speed")              { speed              = value.toFloat();   return true; }
    if (valueName == "speedError")         { speedError         = value.toFloat();   return true; }
    if (valueName == "lifeError")          { lifeError          = value.toFloat();   return true; }
    if (valueName == "scaleError")         { scaleError         = value.toFloat();   return true; }
    if (valueName == "dissipate")          { dissipate          = value.toBool();    return true; }
    if (valueName == "dissipateInv")       { dissipateInv       = value.toBool();    return true; }
    if (valueName == "randomRotation")     { randomRotation     = value.toBool();    return true; }
    if (valueName == "blendMode")          { useAdditive        = value.toBool();    return true; }
    if (valueName == "alphaHash")          { alphaHash          = value.toBool();    return true; }
    if (valueName == "coneAngle")          { coneAngle          = value.toFloat();   return true; }
    if (valueName == "turbulence")         { turbulence         = value.toFloat();   return true; }
    if (valueName == "rotationSpeedMin")   { rotationSpeedMin   = value.toFloat();   return true; }
    if (valueName == "rotationSpeedMax")   { rotationSpeedMax   = value.toFloat();   return true; }
    if (valueName == "burstDuration")      { burstDuration      = value.toFloat();   return true; }
    if (valueName == "burstRepeatDelay")   { burstRepeatDelay   = value.toFloat();   return true; }
    if (valueName == "startDelay")         { startDelay         = value.toFloat();   return true; }
    if (valueName == "maxParticles")       { maxParticles       = value.toInt();     return true; }
    if (valueName == "shape")              { shape = shapeFromName(value.toString()); return true; }
    if (valueName == "orientation")        { orientation = orientationFromName(value.toString()); return true; }
    if (valueName == "preset")             { applyPreset(presetFromName(value.toString())); return true; }
    if (valueName == "extents")            { extents      = iris::fromQt(value.value<QVector3D>()); return true; }
    if (valueName == "innerExtents")       { innerExtents = iris::fromQt(value.value<QVector3D>()); return true; }
    if (valueName == "wind")               { wind         = iris::fromQt(value.value<QVector3D>()); return true; }
    if (valueName == "emitColourStart")    { emitColourStart = value.value<QColor>(); return true; }
    if (valueName == "emitColourEnd")      { emitColourEnd   = value.value<QColor>(); return true; }
    if (valueName == "texture")            return false;   // read-only, see getProperties()

    return SceneNode::setPropertyValue(valueName, value);
}

SceneNodePtr ParticleSystemNode::createDuplicate()
{
    auto ps = ParticleSystemNode::create();

    ps->particlesPerSecond  = this->particlesPerSecond;
    ps->speed               = this->speed;
    ps->texture             = this->texture;

    ps->dissipate           = this->dissipate;
    ps->dissipateInv        = this->dissipateInv;
    ps->randomRotation      = this->randomRotation;
    ps->useAdditive         = this->useAdditive;

    ps->gravityComplement   = this->gravityComplement;
    ps->lifeLength          = this->lifeLength;
    ps->particleScale       = this->particleScale;

    ps->maxParticles        = this->maxParticles;

    ps->speedError          = this->speedError;
    ps->lifeError           = this->lifeError;
    ps->scaleError          = this->scaleError;

    ps->shape               = this->shape;
    ps->extents             = this->extents;
    ps->innerExtents        = this->innerExtents;
    ps->coneAngle           = this->coneAngle;
    ps->emitColourStart     = this->emitColourStart;
    ps->emitColourEnd       = this->emitColourEnd;
    ps->burstDuration       = this->burstDuration;
    ps->burstRepeatDelay    = this->burstRepeatDelay;
    ps->startDelay          = this->startDelay;
    ps->colourKeys          = this->colourKeys;
    ps->scaleKeys           = this->scaleKeys;
    ps->turbulence          = this->turbulence;
    ps->wind                = this->wind;
    ps->rotationSpeedMin    = this->rotationSpeedMin;
    ps->rotationSpeedMax    = this->rotationSpeedMax;
    ps->orientation         = this->orientation;
    ps->alphaHash           = this->alphaHash;
    ps->preset              = this->preset;

    return ps;
}

}
