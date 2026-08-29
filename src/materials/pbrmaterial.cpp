/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#include "pbrmaterial.h"
#include "../graphics/texture2d.h"
#include "../graphics/texturecube.h"
#include "../graphics/graphicsdevice.h"
#include "../scenegraph/scene.h"
#include "../core/property.h"

namespace iris
{

PbrMaterial::PbrMaterial()
{
    // NB: no setTextureCount() here - Material::setShader() overwrites numTextures
    // with shader->samplers.count() on the next line, so setting it is dead code.

    createProgramFromShaderSource(":assets/shaders/pbr_material.vert",
                                  ":assets/shaders/pbr_material.frag");

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

    textureScale        = 1.0f;

    // Opaque geometry. NOTE: CustomMaterial maps its "opaque" string to
    // RenderLayer::Background (custommaterial.cpp:296), which looks like a bug in
    // that mapping - Background is drawn before the sky. Using Opaque here.
    setRenderLayer(RenderLayer::Opaque);

    useIbl              = false;
    iblIntensity        = 1.0f;

    createProperties();
}

void PbrMaterial::begin(GraphicsDevicePtr device, ScenePtr scene)
{
    device->setShader(shader);

    bindTextures(device);

    device->setShaderUniform("u_material.baseColor",
                             QVector3D(baseColor.redF(), baseColor.greenF(), baseColor.blueF()));
    device->setShaderUniform("u_material.baseColorFactor", baseColorFactor);

    device->setShaderUniform("u_material.metallic",       metallicFactor);
    device->setShaderUniform("u_material.roughness",      roughnessFactor);
    device->setShaderUniform("u_material.roughnessLower", roughnessLowerBound);
    device->setShaderUniform("u_material.roughnessUpper", roughnessUpperBound);

    device->setShaderUniform("u_material.normalFactor",    normalFactor);
    device->setShaderUniform("u_material.occlusionFactor", occlusionFactor);

    device->setShaderUniform("u_material.emissive",
                             QVector3D(emissiveColor.redF(), emissiveColor.greenF(), emissiveColor.blueF()));
    device->setShaderUniform("u_material.emissiveIntensity", emissiveIntensity);

    device->setShaderUniform("u_material.alpha",       alpha);
    device->setShaderUniform("u_material.alphaCutoff", alphaCutoff);
    device->setShaderUniform("u_material.alphaMode",   alphaMode);

    device->setShaderUniform("u_textureScale", textureScale);

    device->setShaderUniform("u_useBaseColorMap", useBaseColorMap);
    device->setShaderUniform("u_useMetallicMap",  useMetallicMap);
    device->setShaderUniform("u_useRoughnessMap", useRoughnessMap);
    device->setShaderUniform("u_useNormalMap",    useNormalMap);
    device->setShaderUniform("u_useOcclusionMap", useOcclusionMap);
    device->setShaderUniform("u_useEmissiveMap",  useEmissiveMap);

    device->setShaderUniform("u_useIbl",       useIbl);
    device->setShaderUniform("u_iblIntensity", iblIntensity);

    // Units 0-5 belong to the 2D maps (setTextureCount(6)); the cubemaps take
    // the next two, which bindTextures() leaves alone.
    if (useIbl) {
        device->setTexture(6, diffuseEnvMap.staticCast<Texture>());
        device->setShaderUniform("u_diffuseEnvMap", 6);
        device->setTexture(7, specularEnvMap.staticCast<Texture>());
        device->setShaderUniform("u_specularEnvMap", 7);
    }
}

void PbrMaterial::end(GraphicsDevicePtr device, ScenePtr scene)
{
    Material::end(device, scene);
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
void PbrMaterial::setTextureScale(float s)   { textureScale = s; }

void PbrMaterial::setEnvironmentMap(TextureCubePtr diffuseIrradiance, TextureCubePtr specularPrefiltered)
{
    diffuseEnvMap  = diffuseIrradiance;
    specularEnvMap = specularPrefiltered;
    useIbl         = (!!diffuseIrradiance && !!specularPrefiltered);
}

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

    // 0 opaque, 1 cutout, 2 blend (glTF's OPAQUE/MASK/BLEND).
    auto alphaModeProp         = new IntProperty;
    alphaModeProp->id          = id++;
    alphaModeProp->displayName = "Alpha Mode";
    alphaModeProp->name        = "alphaMode";
    alphaModeProp->minValue    = 0;
    alphaModeProp->maxValue    = 2;
    alphaModeProp->value       = alphaMode;
    properties.append(alphaModeProp);

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
