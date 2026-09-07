/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "document/materials/pbrmaterial.h"
#include "document/assets/texture2d.h"
#include "core/properties/property.h"

namespace iris
{

PbrMaterial::PbrMaterial()
{
    baseColor           = QColor(255, 255, 255);
    baseColorFactor     = 1.0f;
    useBaseColorMap     = false;

    metallicFactor      = 0.0f;
    useMetallicMap      = false;
    roughnessFactor     = 0.5f;
    useRoughnessMap     = false;
    roughnessLowerBound = 0.0f;
    roughnessUpperBound = 1.0f;

    useNormalMap        = false;
    normalFactor        = 1.0f;

    // HLMS_ADOPTION P1. EVERY ONE OF THESE DEFAULTS IS THE RENDERER'S OWN
    // CONSTRUCTED DEFAULT, deliberately: at these values the backend removes
    // the clear-coat shader blocks entirely and its BRDF/shadow/lightmap
    // setters early-return, so an existing scene's generated shader text — and
    // therefore its pixels — is unchanged by this feature existing.
    clearCoat           = 0.0f;
    clearCoatRoughness  = 0.0f;
    brdf                = 0;      // Default
    receiveShadows      = true;
    emissiveAsLightmap  = false;

    emissiveColor       = QColor(0, 0, 0);
    emissiveIntensity   = 0.0f;
    useEmissiveMap      = false;

    alpha               = 1.0f;
    alphaCutoff         = 0.5f;
    alphaMode           = 0;
    refractionStrength  = 0.35f;

    textureScale        = 1.0f;

    // Opaque geometry. NOTE: CustomMaterial maps its "opaque" string to
    // RenderLayer::Background (custommaterial.cpp:296), which looks like a bug in
    // that mapping - Background is drawn before the sky. Using Opaque here.
    setRenderLayer(RenderLayer::Opaque);

    useIbl              = false;
    iblIntensity        = 1.0f;

    createProperties();
}

// ---------------------------------------------------------------- setters

void PbrMaterial::setBaseColor(QColor color)        { baseColor = color; }
void PbrMaterial::setBaseColorFactor(float factor)  { baseColorFactor = factor; }

void PbrMaterial::setBaseColorMap(Texture2DPtr tex)
{
    if (!!tex) { useBaseColorMap = true;  addTexture("u_baseColorMap", tex); }
    else       { useBaseColorMap = false; removeTexture("u_baseColorMap"); }
}

void PbrMaterial::setMetallicFactor(float factor)   { metallicFactor = factor; }

void PbrMaterial::setMetallicMap(Texture2DPtr tex)
{
    if (!!tex) { useMetallicMap = true;  addTexture("u_metallicMap", tex); }
    else       { useMetallicMap = false; removeTexture("u_metallicMap"); }
}

void PbrMaterial::setRoughnessFactor(float factor)  { roughnessFactor = factor; }

void PbrMaterial::setRoughnessMap(Texture2DPtr tex)
{
    if (!!tex) { useRoughnessMap = true;  addTexture("u_roughnessMap", tex); }
    else       { useRoughnessMap = false; removeTexture("u_roughnessMap"); }
}

void PbrMaterial::setNormalMap(Texture2DPtr tex)
{
    if (!!tex) { useNormalMap = true;  addTexture("u_normalMap", tex); }
    else       { useNormalMap = false; removeTexture("u_normalMap"); }
}

void PbrMaterial::setNormalFactor(float factor)     { normalFactor = factor; }

void PbrMaterial::setClearCoat(float coat)              { clearCoat = coat; }
void PbrMaterial::setClearCoatRoughness(float r)        { clearCoatRoughness = r; }
void PbrMaterial::setBrdf(int index)                    { brdf = index; }
void PbrMaterial::setReceiveShadows(bool receive)       { receiveShadows = receive; }
void PbrMaterial::setEmissiveAsLightmap(bool asLightmap){ emissiveAsLightmap = asLightmap; }

// The BRDF vocabulary. Six of the renderer's twelve named values: the three
// families, plain and with SEPARATE diffuse fresnel (the variant that exists
// for glass, transparent plastics, fur and marbles — surfaces with complex
// re-scattering). The other six are uncorrelated/legacy-math combinations of
// these; twelve rows of jargon is worse product than six.
//
// INDEX IS THE STORED VALUE AND IT IS PERMANENT: appending is safe, reordering
// or removing a row silently re-points every saved material.
const QVector<PbrMaterial::BrdfName> &PbrMaterial::brdfNames()
{
    static const QVector<BrdfName> kNames = {
        { "Default",                            "Default" },
        { "CookTorrance",                       "Cook-Torrance" },
        { "BlinnPhong",                         "Blinn-Phong" },
        { "DefaultSeparateDiffuseFresnel",      "Default (Diffuse Fresnel)" },
        { "CookTorranceSeparateDiffuseFresnel", "Cook-Torrance (Diffuse Fresnel)" },
        { "BlinnPhongSeparateDiffuseFresnel",   "Blinn-Phong (Diffuse Fresnel)" },
    };
    return kNames;
}

QString PbrMaterial::brdfEngineName(int index)
{
    const auto &names = brdfNames();
    if (index < 0 || index >= names.size()) return QStringLiteral("Default");
    return QString::fromLatin1(names[index].engineName);
}

bool PbrMaterial::brdfSupportsClearCoat(int index)
{
    // The renderer's BRDF value is (family | modifier bits); the clear-coat
    // path is gated on the FAMILY being Default, so the diffuse-fresnel
    // variants of Default qualify and Cook-Torrance / Blinn-Phong do not.
    // Expressed on the names because the bit layout is the renderer's, not
    // the document's.
    return brdfEngineName(index).startsWith(QStringLiteral("Default"));
}

void PbrMaterial::setEmissiveColor(QColor color)        { emissiveColor = color; }
void PbrMaterial::setEmissiveIntensity(float intensity) { emissiveIntensity = intensity; }

void PbrMaterial::setEmissiveMap(Texture2DPtr tex)
{
    if (!!tex) { useEmissiveMap = true;  addTexture("u_emissiveMap", tex); }
    else       { useEmissiveMap = false; removeTexture("u_emissiveMap"); }
}

void PbrMaterial::setAlpha(float a)          { alpha = a; }
void PbrMaterial::setAlphaCutoff(float c)    { alphaCutoff = c; }
void PbrMaterial::setAlphaMode(int mode)     { alphaMode = mode; }
void PbrMaterial::setRefractionStrength(float s) { refractionStrength = s; }
void PbrMaterial::setTextureScale(float s)   { textureScale = s; }

void PbrMaterial::setIblIntensity(float intensity) { iblIntensity = intensity; }

// Empty path clears the slot; a missing file yields a null texture, which the
// set*Map functions treat as "no map" rather than failing.
Texture2DPtr PbrMaterial::loadTexture(const QString& path)
{
    if (path.isEmpty()) return Texture2DPtr();
    return Texture2D::load(path);
}

void PbrMaterial::setValue(const QString& name, const QVariant& value)
{
    if      (name == "baseColor")         baseColor         = value.value<QColor>();
    else if (name == "metallic")          metallicFactor    = value.toFloat();
    else if (name == "roughness")         roughnessFactor   = value.toFloat();
    else if (name == "normalFactor")      normalFactor      = value.toFloat();
    else if (name == "emissiveColor")     emissiveColor     = value.value<QColor>();
    else if (name == "emissiveIntensity") emissiveIntensity = value.toFloat();
    else if (name == "alpha")             alpha             = value.toFloat();
    else if (name == "textureScale")      textureScale      = value.toFloat();
    else if (name == "roughnessLowerBound") roughnessLowerBound = value.toFloat();
    else if (name == "roughnessUpperBound") roughnessUpperBound = value.toFloat();
    else if (name == "alphaCutoff")       alphaCutoff       = value.toFloat();
    else if (name == "alphaMode")         alphaMode         = value.toInt();
    else if (name == "refractionStrength") refractionStrength = value.toFloat();
    else if (name == "clearCoat")          clearCoat          = value.toFloat();
    else if (name == "clearCoatRoughness") clearCoatRoughness = value.toFloat();
    else if (name == "brdf")               brdf               = value.toInt();
    else if (name == "receiveShadows")     receiveShadows     = value.toBool();
    else if (name == "emissiveAsLightmap") emissiveAsLightmap = value.toBool();

    // Texture properties arrive as a path, matching how CustomMaterial::setValue
    // is driven from the material presets.
    else if (name == "baseColorMap")  setBaseColorMap(loadTexture(value.toString()));
    else if (name == "metallicMap")   setMetallicMap(loadTexture(value.toString()));
    else if (name == "roughnessMap")  setRoughnessMap(loadTexture(value.toString()));
    else if (name == "normalMap")     setNormalMap(loadTexture(value.toString()));
    else if (name == "emissiveMap")   setEmissiveMap(loadTexture(value.toString()));

    // keep the Property object in step so the panel and the field agree
    for (auto prop : properties) {
        if (prop->name == name) { prop->setValue(value); break; }
    }
}

// ---------------------------------------------------------------- properties
//
// The panel renders these by PropertyType. Note that declaring them is NOT
// sufficient on its own - setValue() above is what carries an edited value onto
// the field the shader actually reads.

void PbrMaterial::createProperties()
{
    int id = 0;

    auto colProp         = new ColorProperty;
    colProp->id          = id++;
    colProp->displayName = "Base Color";
    colProp->name        = "baseColor";
    colProp->value       = baseColor;
    properties.append(colProp);

    auto metallicProp         = new FloatProperty;
    metallicProp->id          = id++;
    metallicProp->displayName = "Metallic";
    metallicProp->name        = "metallic";
    metallicProp->minValue    = 0.0f;
    metallicProp->maxValue    = 1.0f;
    metallicProp->value       = metallicFactor;
    properties.append(metallicProp);

    auto roughnessProp         = new FloatProperty;
    roughnessProp->id          = id++;
    roughnessProp->displayName = "Roughness";
    roughnessProp->name        = "roughness";
    roughnessProp->minValue    = 0.0f;
    roughnessProp->maxValue    = 1.0f;
    roughnessProp->value       = roughnessFactor;
    properties.append(roughnessProp);

    auto normalProp         = new FloatProperty;
    normalProp->id          = id++;
    normalProp->displayName = "Normal Intensity";
    normalProp->name        = "normalFactor";
    normalProp->minValue    = 0.0f;
    normalProp->maxValue    = 2.0f;
    normalProp->value       = normalFactor;
    properties.append(normalProp);

    auto emissiveColProp         = new ColorProperty;
    emissiveColProp->id          = id++;
    emissiveColProp->displayName = "Emissive Color";
    emissiveColProp->name        = "emissiveColor";
    emissiveColProp->value       = emissiveColor;
    properties.append(emissiveColProp);

    auto emissiveProp         = new FloatProperty;
    emissiveProp->id          = id++;
    emissiveProp->displayName = "Emissive Intensity";
    emissiveProp->name        = "emissiveIntensity";
    emissiveProp->minValue    = 0.0f;
    emissiveProp->maxValue    = 10.0f;
    emissiveProp->value       = emissiveIntensity;
    properties.append(emissiveProp);

    auto alphaProp         = new FloatProperty;
    alphaProp->id          = id++;
    alphaProp->displayName = "Alpha";
    alphaProp->name        = "alpha";
    alphaProp->minValue    = 0.0f;
    alphaProp->maxValue    = 1.0f;
    alphaProp->value       = alpha;
    properties.append(alphaProp);

    auto scaleProp         = new FloatProperty;
    scaleProp->id          = id++;
    scaleProp->displayName = "Texture Scale";
    scaleProp->name        = "textureScale";
    scaleProp->minValue    = 0.0f;
    scaleProp->maxValue    = 10.0f;
    scaleProp->value       = textureScale;
    properties.append(scaleProp);

    // Remap bounds for a sampled roughness map (see the field comment in the
    // header: lower > upper deliberately inverts a legacy gloss map).
    auto roughLowerProp         = new FloatProperty;
    roughLowerProp->id          = id++;
    roughLowerProp->displayName = "Roughness Lower Bound";
    roughLowerProp->name        = "roughnessLowerBound";
    roughLowerProp->minValue    = 0.0f;
    roughLowerProp->maxValue    = 1.0f;
    roughLowerProp->value       = roughnessLowerBound;
    properties.append(roughLowerProp);

    auto roughUpperProp         = new FloatProperty;
    roughUpperProp->id          = id++;
    roughUpperProp->displayName = "Roughness Upper Bound";
    roughUpperProp->name        = "roughnessUpperBound";
    roughUpperProp->minValue    = 0.0f;
    roughUpperProp->maxValue    = 1.0f;
    roughUpperProp->value       = roughnessUpperBound;
    properties.append(roughUpperProp);

    auto cutoffProp         = new FloatProperty;
    cutoffProp->id          = id++;
    cutoffProp->displayName = "Alpha Cutoff";
    cutoffProp->name        = "alphaCutoff";
    cutoffProp->minValue    = 0.0f;
    cutoffProp->maxValue    = 1.0f;
    cutoffProp->value       = alphaCutoff;
    properties.append(cutoffProp);

    // 0 opaque, 1 cutout/masked, 2 blend/translucent (glTF's OPAQUE/MASK/BLEND),
    // 3 glass (engine realistic transparency), 4 additive (Src+Dest),
    // 5 modulate (Src×Dest) — the Unreal-parity blend modes — and
    // 6 refractive (glass that BENDS the background; needs the viewport's
    // refraction pass, POST_CHAIN_SPEC.md phase 7).
    //
    // A ListProperty (the generic ENUM row), not an IntProperty with a
    // hardcoded name branch in the panel: the labels ride the row, so the
    // picker cannot disagree with the vocabulary (PUBLISH_AUDIT #4 was exactly
    // that disagreement). ON DISK IT IS STILL AN INT — the enum row's value IS
    // the index — so existing scenes and .material files load unchanged.
    auto alphaModeProp         = new ListProperty;
    alphaModeProp->id          = id++;
    alphaModeProp->displayName = "Alpha Mode";
    alphaModeProp->name        = "alphaMode";
    alphaModeProp->labels      = { "Opaque", "Masked", "Translucent", "Glass",
                                   "Additive", "Modulate", "Refractive" };
    alphaModeProp->value       = alphaMode;
    properties.append(alphaModeProp);

    auto refractProp         = new FloatProperty;
    refractProp->id          = id++;
    refractProp->displayName = "Refraction Strength";
    refractProp->name        = "refractionStrength";
    refractProp->minValue    = 0.0f;
    refractProp->maxValue    = 1.0f;
    refractProp->value       = refractionStrength;
    properties.append(refractProp);

    // ---- HLMS_ADOPTION P1: the cheap PBS knobs ----
    // The BRDF picker comes FIRST because it constrains the two coat rows: the
    // renderer can only carry a clear coat on the Default family, so the panel
    // disables them on any other pick (D-P1b) and the coat values are kept, not
    // destroyed.
    auto brdfProp         = new ListProperty;
    brdfProp->id          = id++;
    brdfProp->displayName = "Shading BRDF";
    brdfProp->name        = "brdf";
    for (const auto &n : brdfNames()) brdfProp->labels << QString::fromLatin1(n.displayName);
    brdfProp->value       = brdf;
    properties.append(brdfProp);

    auto coatProp         = new FloatProperty;
    coatProp->id          = id++;
    coatProp->displayName = "Clear Coat";
    coatProp->name        = "clearCoat";
    coatProp->minValue    = 0.0f;
    coatProp->maxValue    = 1.0f;
    coatProp->value       = clearCoat;
    properties.append(coatProp);

    auto coatRoughProp         = new FloatProperty;
    coatRoughProp->id          = id++;
    coatRoughProp->displayName = "Clear Coat Roughness";
    coatRoughProp->name        = "clearCoatRoughness";
    coatRoughProp->minValue    = 0.0f;
    coatRoughProp->maxValue    = 1.0f;
    coatRoughProp->value       = clearCoatRoughness;
    properties.append(coatRoughProp);

    // NAMED "receiveShadows" AND NOTHING ELSE. The materials graph used to ship
    // a `receiveShadow` (singular) checkbox in MaterialSettings that reached
    // nothing at all; that ghost is deleted. This row is the real one.
    auto receiveShadowsProp         = new BoolProperty;
    receiveShadowsProp->id          = id++;
    receiveShadowsProp->displayName = "Receive Shadows";
    receiveShadowsProp->name        = "receiveShadows";
    receiveShadowsProp->value       = receiveShadows;
    properties.append(receiveShadowsProp);

    // Only meaningful with an emissive map bound, and the emissive colour
    // should be white or it tints the lightmap.
    auto lightmapProp         = new BoolProperty;
    lightmapProp->id          = id++;
    lightmapProp->displayName = "Emissive as Lightmap";
    lightmapProp->name        = "emissiveAsLightmap";
    lightmapProp->value       = emissiveAsLightmap;
    properties.append(lightmapProp);

    // The six texture maps. Names match the setValue() cases above (paths, not
    // the "u_*Map" sampler names used as Material::textures keys). Declaring
    // them is what makes SceneWriter persist the maps and SceneReader restore
    // them — without these, PBR texture maps did not survive a scene save/load.
    struct MapDef { const char *display; const char *name; };
    static const MapDef kMaps[] = {
        { "Base Color Map", "baseColorMap" },
        { "Normal Map",     "normalMap"    },
        { "Metallic Map",   "metallicMap"  },
        { "Roughness Map",  "roughnessMap" },
        { "Emissive Map",   "emissiveMap"  },
    };
    for (const auto &m : kMaps) {
        auto texProp         = new TextureProperty;
        texProp->id          = id++;
        texProp->displayName = m.display;
        texProp->name        = m.name;
        properties.append(texProp);
    }
}

}
