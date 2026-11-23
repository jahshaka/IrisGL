/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "core/math/vec.h"
#include "document/srgbcolor.h"
#include "document/properties/valueshapes.h"

#include "document/properties/property.h"
#include "document/scenegraph/particlesystemnode.h"

namespace iris
{

ParticleSystemNode::ParticleSystemNode() {
    sceneNodeType = SceneNodeType::ParticleSystem;
    // A rendering-mode flag, NOT an authoring default: a preset (which resets
    // the authoring fields) leaves a distortion emitter a distortion emitter.
    distortion = false;
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
    emitColourStart = SrgbColor(255, 255, 255);
    emitColourEnd = SrgbColor(255, 255, 255);
    burstDuration = burstRepeatDelay = startDelay = 0.0f;
    colourKeys.clear();
    scaleKeys.clear();
    turbulence = 0.0f;
    // ADDENDUM A-4. All four defaults are the NEUTRAL values, and the mirror
    // omits an affector at its neutral value entirely (the turbulence rule),
    // so these fields existing cannot change what an existing emitter draws.
    colourFade1 = SrgbColor(0, 0, 0, 0);
    colourFade2 = SrgbColor(0, 0, 0, 0);
    colourFadeSwitch = 0.0f;
    colourRampImage.clear();
    colourRampGuid.clear();
    scaleRate = 0.0f;
    scaleRateMultiply = false;
    wind = iris::Vec3(0, 0, 0);
    rotationSpeedMin = rotationSpeedMax = 0.0f;
    orientation = ParticleOrientation::Billboard;
    alphaHash = true;
    preset = ParticlePreset::Custom;
    // The whole authoring block at once (lead review R2 #2): every field here
    // is in the emitter's mirror signature, and applyPreset runs through this.
    notifyChanged(NodeChange::Params);
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
    notifyChanged(NodeChange::Params);
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

PropertyRows ParticleSystemNode::getProperties()
{
    auto props = SceneNode::getProperties();

    auto addFloat = [&props](const char *display, const char *name, float value) {
        auto p = std::make_unique<FloatProperty>();
        p->displayName = display; p->name = name; p->value = value;
        props.add(std::move(p));
    };
    auto addBool = [&props](const char *display, const char *name, bool value) {
        auto p = std::make_unique<BoolProperty>();
        p->displayName = display; p->name = name; p->value = value;
        props.add(std::move(p));
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
    addBool ("Distortion",           "distortion",         distortion);

    addFloat("Cone Angle",           "coneAngle",          coneAngle);
    addFloat("Turbulence",           "turbulence",         turbulence);
    addFloat("Rotation Speed Min",   "rotationSpeedMin",   rotationSpeedMin);
    addFloat("Rotation Speed Max",   "rotationSpeedMax",   rotationSpeedMax);
    addFloat("Burst Duration",       "burstDuration",      burstDuration);
    addFloat("Burst Repeat Delay",   "burstRepeatDelay",   burstRepeatDelay);
    addFloat("Start Delay",          "startDelay",         startDelay);
    // ADDENDUM A-4. The fade colours are per-second RATES encoded around
    // 0.5 grey (0 = -1/s, 128 = 0, 255 = +1/s) — a colour cannot carry a
    // negative component and a fade-OUT is exactly that. Said on the row.
    addFloat("Colour Fade Switch (s left)", "colourFadeSwitch", colourFadeSwitch);
    addFloat("Scale Rate",           "scaleRate",          scaleRate);
    addBool ("Scale Rate Multiplies","scaleRateMultiply",  scaleRateMultiply);

    auto intProp = std::make_unique<IntProperty>();
    intProp->displayName = "Max Particles";
    intProp->name = "maxParticles";
    intProp->value = maxParticles;
    props.add(std::move(intProp));

    auto addVec3 = [&props](const char *display, const char *name, const iris::Vec3 &value) {
        auto p = std::make_unique<Vec3Property>();
        p->displayName = display; p->name = name; p->value = value;
        props.add(std::move(p));
    };
    addVec3("Extents",       "extents",      extents);
    addVec3("Inner Extents", "innerExtents", innerExtents);
    addVec3("Wind",          "wind",         wind);

    auto addColour = [&props](const char *display, const char *name, const SrgbColor &value) {
        auto p = std::make_unique<ColorProperty>();
        p->displayName = display; p->name = name; p->value = value;
        props.add(std::move(p));
    };
    addColour("Colour Fade 1 (rate)", "colourFade1", colourFade1);
    addColour("Colour Fade 2 (rate)", "colourFade2", colourFade2);
    addColour("Emit Colour Start", "emitColourStart", emitColourStart);
    addColour("Emit Colour End",   "emitColourEnd",   emitColourEnd);

    // Enum-ish rows travel as their NAME, never as an ordinal (see the tables
    // above). FileProperty is the QString-valued Property here — the same one
    // the texture row has always used.
    auto addString = [&props](const char *display, const char *name, const QString &value,
                              bool readOnly = false) {
        auto p = std::make_unique<FileProperty>();
        p->displayName = display; p->name = name; p->value = value.toStdString();
        p->readOnly = readOnly;
        props.add(std::move(p));
    };
    addString("Emitter Shape", "shape",       shapeName(shape));
    addString("Orientation",   "orientation", orientationName(orientation));
    addString("Preset",        "preset",      presetName(preset));

    // READ-ONLY: the node holds a Texture2DPtr, and binding one needs the asset
    // manager (guid -> file), which the document layer has no access to. The
    // texture's source path is exposed for inspection only; setPropertyValue
    // refuses it.
    addString("Texture", "texture", !!texture ? texture->getSource() : QString(), true);

    return props;
}

Value ParticleSystemNode::getPropertyValue(std::string_view valueName)
{
    const auto F = [](float f) { return Value(double(f)); };
    if (valueName == "particlesPerSecond") return F(particlesPerSecond);
    if (valueName == "particleScale")      return F(particleScale);
    if (valueName == "gravityComplement")  return F(gravityComplement);
    if (valueName == "lifeLength")         return F(lifeLength);
    if (valueName == "speed")              return F(speed);
    if (valueName == "speedError")         return F(speedError);
    if (valueName == "lifeError")          return F(lifeError);
    if (valueName == "scaleError")         return F(scaleError);
    if (valueName == "dissipate")          return Value(dissipate);
    if (valueName == "dissipateInv")       return Value(dissipateInv);
    if (valueName == "randomRotation")     return Value(randomRotation);
    if (valueName == "blendMode")          return Value(useAdditive);
    if (valueName == "alphaHash")          return Value(alphaHash);
    if (valueName == "distortion")         return Value(distortion);
    if (valueName == "coneAngle")          return F(coneAngle);
    if (valueName == "turbulence")         return F(turbulence);
    if (valueName == "rotationSpeedMin")   return F(rotationSpeedMin);
    if (valueName == "rotationSpeedMax")   return F(rotationSpeedMax);
    if (valueName == "burstDuration")      return F(burstDuration);
    if (valueName == "burstRepeatDelay")   return F(burstRepeatDelay);
    if (valueName == "startDelay")         return F(startDelay);
    if (valueName == "maxParticles")       return Value(maxParticles);
    if (valueName == "shape")              return Value(shapeName(shape).toStdString());
    if (valueName == "orientation")        return Value(orientationName(orientation).toStdString());
    if (valueName == "preset")             return Value(presetName(preset).toStdString());
    if (valueName == "texture")            return Value(!!texture ? texture->getSource().toStdString() : std::string());
    // The vector and colour rows (their Vec3 / Color rows in getProperties()).
    if (valueName == "extents")            return vec3ToValue(extents);
    if (valueName == "innerExtents")       return vec3ToValue(innerExtents);
    if (valueName == "wind")               return vec3ToValue(wind);
    // An unset colour (an unset fade above all: the mirror reads its VALIDITY as "no fade") is a Null, never black.
    if (valueName == "emitColourStart")    return colourRowValue(emitColourStart);
    if (valueName == "emitColourEnd")      return colourRowValue(emitColourEnd);
    if (valueName == "colourFade1")        return colourRowValue(colourFade1);
    if (valueName == "colourFade2")        return colourRowValue(colourFade2);
    if (valueName == "colourFadeSwitch")   return F(colourFadeSwitch);
    if (valueName == "scaleRate")          return F(scaleRate);
    if (valueName == "scaleRateMultiply")  return Value(scaleRateMultiply);
    if (valueName == "colourRamp")         return Value(colourRampGuid.toStdString());

    return SceneNode::getPropertyValue(valueName);
}

bool ParticleSystemNode::setPropertyValue(std::string_view valueName, const Value &value)
{
    // STRICT: a float a Number (or an Int), an int an Int, a flag a Bool, a name a String, a vector an Array of three
    // numbers, a colour its Object; anything else false, nothing moved.
    double n = 0.0;
    const bool isNum = numberOf(value, n);
    const bool isBool = value.kind() == Kind::Bool;
    const auto F = [&](float &field) { if (!isNum) return false; field = float(n); return markedParams(); };
    const auto B = [&](bool &field) { if (!isBool) return false; field = value.asBool(); return markedParams(); };
    const auto V = [&](Vec3 &field) { return vec3FromValue(value, field) && markedParams(); };
    // A Null is THE invalid colour ("no fade"), an Object its colour (colourOfRowValue).
    const auto C = [&](SrgbColor &field) { return colourOfRowValue(value, field) && markedParams(); };
    const bool isText = value.kind() == Kind::String;
    const QString text = isText ? QString::fromStdString(value.asString()) : QString();
    if (valueName == "particlesPerSecond") return F(particlesPerSecond);
    if (valueName == "particleScale")      return F(particleScale);
    if (valueName == "gravityComplement")  return F(gravityComplement);
    if (valueName == "lifeLength")         return F(lifeLength);
    if (valueName == "speed")              return F(speed);
    if (valueName == "speedError")         return F(speedError);
    if (valueName == "lifeError")          return F(lifeError);
    if (valueName == "scaleError")         return F(scaleError);
    if (valueName == "dissipate")          return B(dissipate);
    if (valueName == "dissipateInv")       return B(dissipateInv);
    if (valueName == "randomRotation")     return B(randomRotation);
    if (valueName == "blendMode")          return B(useAdditive);
    if (valueName == "alphaHash")          return B(alphaHash);
    if (valueName == "distortion")         return B(distortion);
    if (valueName == "coneAngle")          return F(coneAngle);
    if (valueName == "turbulence")         return F(turbulence);
    if (valueName == "rotationSpeedMin")   return F(rotationSpeedMin);
    if (valueName == "rotationSpeedMax")   return F(rotationSpeedMax);
    if (valueName == "burstDuration")      return F(burstDuration);
    if (valueName == "burstRepeatDelay")   return F(burstRepeatDelay);
    if (valueName == "startDelay")         return F(startDelay);
    if (valueName == "maxParticles") {
        if (value.kind() != Kind::Int) return false;
        maxParticles = static_cast<int>(value.asInt());
        return markedParams();
    }
    if (valueName == "shape")              { if (!isText) return false; shape = shapeFromName(text); return markedParams(); }
    if (valueName == "orientation")        { if (!isText) return false; orientation = orientationFromName(text); return markedParams(); }
    if (valueName == "preset")             { if (!isText) return false; applyPreset(presetFromName(text)); return markedParams(); }
    if (valueName == "extents")            return V(extents);
    if (valueName == "innerExtents")       return V(innerExtents);
    if (valueName == "wind")               return V(wind);
    if (valueName == "emitColourStart")    return C(emitColourStart);
    if (valueName == "emitColourEnd")      return C(emitColourEnd);
    if (valueName == "colourFade1")        return C(colourFade1);
    if (valueName == "colourFade2")        return C(colourFade2);
    if (valueName == "colourFadeSwitch")   return F(colourFadeSwitch);
    if (valueName == "scaleRate")          return F(scaleRate);
    if (valueName == "scaleRateMultiply")  return B(scaleRateMultiply);
    if (valueName == "texture")            return false;   // read-only, see getProperties()
    // The colour RAMP is an asset BINDING, not a value: it needs a project pin
    // and a CAS resolve, which is particles.setColourRamp's job — the same rule
    // `texture` follows.
    if (valueName == "colourRamp")         return false;

    return SceneNode::setPropertyValue(valueName, value);
}

SceneNodePtr ParticleSystemNode::createDuplicate()
{
    auto ps = ParticleSystemNode::create();

    ps->particlesPerSecond  = this->particlesPerSecond;
    ps->speed               = this->speed;
    ps->texture             = this->texture;
    ps->textureGuid         = this->textureGuid;

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
    ps->distortion          = this->distortion;
    ps->preset              = this->preset;
    // ADDENDUM A-4.
    ps->colourFade1         = this->colourFade1;
    ps->colourFade2         = this->colourFade2;
    ps->colourFadeSwitch    = this->colourFadeSwitch;
    ps->colourRampImage     = this->colourRampImage;
    ps->colourRampGuid      = this->colourRampGuid;
    ps->scaleRate           = this->scaleRate;
    ps->scaleRateMultiply   = this->scaleRateMultiply;

    return ps;
}

}
