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

    useOcclusionMap     = false;
    occlusionFactor     = 1.0f;

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

void PbrMaterial::setOcclusionMap(Texture2DPtr tex)
{
    if (!!tex) { useOcclusionMap = true;  addTexture("u_occlusionMap", tex); }
    else       { useOcclusionMap = false; removeTexture("u_occlusionMap"); }
}

void PbrMaterial::setOcclusionFactor(float factor)  { occlusionFactor = factor; }

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
    else if (name == "occlusionFactor")   occlusionFactor   = value.toFloat();
    else if (name == "emissiveColor")     emissiveColor     = value.value<QColor>();
    else if (name == "emissiveIntensity") emissiveIntensity = value.toFloat();
    else if (name == "alpha")             alpha             = value.toFloat();
    else if (name == "textureScale")      textureScale      = value.toFloat();
    else if (name == "roughnessLowerBound") roughnessLowerBound = value.toFloat();
    else if (name == "roughnessUpperBound") roughnessUpperBound = value.toFloat();
    else if (name == "alphaCutoff")       alphaCutoff       = value.toFloat();
    else if (name == "alphaMode")         alphaMode         = value.toInt();
    else if (name == "refractionStrength") refractionStrength = value.toFloat();

    // Texture properties arrive as a path, matching how CustomMaterial::setValue
    // is driven from the material presets.
    else if (name == "baseColorMap")  setBaseColorMap(loadTexture(value.toString()));
    else if (name == "metallicMap")   setMetallicMap(loadTexture(value.toString()));
    else if (name == "roughnessMap")  setRoughnessMap(loadTexture(value.toString()));
    else if (name == "normalMap")     setNormalMap(loadTexture(value.toString()));
    else if (name == "occlusionMap")  setOcclusionMap(loadTexture(value.toString()));
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

    auto occlusionProp         = new FloatProperty;
    occlusionProp->id          = id++;
    occlusionProp->displayName = "Occlusion";
    occlusionProp->name        = "occlusionFactor";
    occlusionProp->minValue    = 0.0f;
    occlusionProp->maxValue    = 1.0f;
    occlusionProp->value       = occlusionFactor;
    properties.append(occlusionProp);

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
    auto alphaModeProp         = new IntProperty;
    alphaModeProp->id          = id++;
    alphaModeProp->displayName = "Alpha Mode";
    alphaModeProp->name        = "alphaMode";
    alphaModeProp->minValue    = 0;
    alphaModeProp->maxValue    = 6;
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
        { "Occlusion Map",  "occlusionMap" },
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
