/**************************************************************************
This file is part of JahshakaVR, VR Authoring Toolkit
http://www.jahshaka.com
Copyright (c) 2016  GPLv3 Jahshaka LLC <coders@jahshaka.com>

This is free software: you may copy, redistribute
and/or modify it under the terms of the GPLv3 License

For more information see the LICENSE file
*************************************************************************/

// Metallic-roughness PBR surface.
// Parameter names follow O3DE Atom StandardPBR so the material model maps onto
// Atom and glTF 2.0 pbrMetallicRoughness without translation.
// The Light struct, shadow helpers and fog below are shared verbatim with
// default_material.frag - the renderer feeds both identically.

#version 150

#define PI 3.14159265359
#define PI2 6.28318530718
#define RECIPROCAL_PI2 0.15915494

#define SHADOW_NONE 0
#define SHADOW_HARD 1
#define SHADOW_SOFT 2
#define SHADOW_VERYSOFT 3

// Image-based lighting, following O3DE Atom: a diffuse irradiance cubemap and a
// prefiltered specular cubemap whose mip is selected by roughness. Atom uses no
// BRDF lookup texture - see EnvBRDFApprox below.
uniform samplerCube u_diffuseEnvMap;
uniform samplerCube u_specularEnvMap;
uniform bool  u_useIbl;
uniform float u_iblIntensity;

uniform sampler2D u_baseColorMap;   uniform bool u_useBaseColorMap;
uniform sampler2D u_metallicMap;    uniform bool u_useMetallicMap;
uniform sampler2D u_roughnessMap;   uniform bool u_useRoughnessMap;
uniform sampler2D u_normalMap;      uniform bool u_useNormalMap;
uniform sampler2D u_occlusionMap;   uniform bool u_useOcclusionMap;
uniform sampler2D u_emissiveMap;    uniform bool u_useEmissiveMap;

in vec2 v_texCoord;
in vec3 v_normal;
in vec3 v_worldPos;
in mat3 v_tanToWorld;

const int MAX_LIGHTS = 8;
const int TYPE_POINT = 0;
const int TYPE_DIRECTIONAL = 1;
const int TYPE_SPOT = 2;

struct Light {
    int type;
    vec3 position;
    vec3 ambient;
    vec4 color;
    float distance;
    float intensity;
    vec3 direction;
    float cutOffAngle;
    float cutOffSoftness;

	vec4 shadowColor;
	float shadowAlpha;

    sampler2D shadowMap;
    bool shadowEnabled;
    int shadowType;
    mat4 shadowMatrix;
};

float SampleShadowMap(in sampler2D shadowMap, vec2 coords, float compare) {
    if (coords.x < 0.0 || coords.x > 1.0 || coords.y < 0.0 || coords.y > 1.0)
        return 1.0;
    return step(compare, texture(shadowMap, coords.xy).r);
}

// todo: use sampler2DShadow, it does the same thing but faster
float SampleShadowMapLinear(sampler2D shadowMap, vec2 coords, float compare, vec2 texelSize) {
    vec2 pixelPos = coords / texelSize + vec2(0.5);
    vec2 fracPart = fract(pixelPos);
    vec2 startTexel = (pixelPos - fracPart) * texelSize;

    float blTexel = SampleShadowMap(shadowMap, startTexel, compare);
    float brTexel = SampleShadowMap(shadowMap, startTexel + vec2(texelSize.x, 0.0), compare);
    float tlTexel = SampleShadowMap(shadowMap, startTexel + vec2(0.0, texelSize.y), compare);
    float trTexel = SampleShadowMap(shadowMap, startTexel + texelSize, compare);

    float mixA = mix(blTexel, tlTexel, fracPart.y);
    float mixB = mix(brTexel, trTexel, fracPart.y);

    return mix(mixA, mixB, fracPart.x);
}

float SampleShadowMapPCF(in sampler2D shadowMap, vec2 coords, float compare, vec2 texelSize) {
    float result = 0;

    const float NUM_SAMPLES = 7.0;
    const float SAMPLES_START = (NUM_SAMPLES - 1.0) / 2.0;

    for(float y = -SAMPLES_START; y <= SAMPLES_START; y++) {
        for(float x = -SAMPLES_START; x <= SAMPLES_START; x++) {
            vec2 offset = vec2(x, y) * texelSize;
            result += SampleShadowMapLinear(shadowMap, coords + offset, compare, texelSize);
        }
    }

    return result / (NUM_SAMPLES * NUM_SAMPLES);
}

// it's technically 4x4...but it will do for now
float SampleShadowMapPCF3x3(in sampler2D shadowMap, vec2 coords, float compare, vec2 texelSize) {
    float result = 0;

    const float NUM_SAMPLES = 3.0;
    const float SAMPLES_START = (NUM_SAMPLES - 1.0) / 2.0;

    for(float y = -SAMPLES_START; y <= SAMPLES_START; y++) {
        for(float x = -SAMPLES_START; x <= SAMPLES_START; x++) {
            vec2 offset = vec2(x, y) * texelSize;
            result += SampleShadowMapLinear(shadowMap, coords + offset, compare, texelSize);
        }
    }

    return result / (NUM_SAMPLES * NUM_SAMPLES);
}


float CalcShadowMap(in sampler2D shadowMap, vec4 fragPosLightSpace) {
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    return SampleShadowMapPCF(shadowMap, projCoords.xy, projCoords.z, texelSize);
}

float calcVerySoftShadowMap(in Light light, in vec4 lightSpacePos)
{
    return CalcShadowMap(light.shadowMap, lightSpacePos);
}

float calcSoftShadowMap(in Light light, in vec4 fragPosLightSpace)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    vec2 texelSize = 1.0 / textureSize(light.shadowMap, 0);
    return SampleShadowMapPCF3x3(light.shadowMap, projCoords.xy, projCoords.z, texelSize);
}

float calcHardShadowMap(in Light light, in vec4 lightSpacePos)
{
    vec3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    projCoords = projCoords * 0.5 + 0.5;
    //return SampleShadowMap(light.shadowMap,projCoords.xy,projCoords.z);
    if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 1.0;
    if (projCoords.z > texture(light.shadowMap, projCoords.xy).r)
        return 0.0;
    return 1.0;
}


//  Handles shadowing for lights with different shadowing types
float calculateShadowFactor(in Light light, in vec3 worldPos)
{
    vec4 lightSpacePos = light.shadowMatrix * vec4(v_worldPos, 1.0);
    if (light.shadowType==SHADOW_HARD)
        return calcHardShadowMap(light, lightSpacePos);
    if (light.shadowType==SHADOW_SOFT)
        return calcSoftShadowMap(light, lightSpacePos);
	if (light.shadowType==SHADOW_VERYSOFT)
        return calcVerySoftShadowMap(light, lightSpacePos);
    return 1.0f;
}



uniform sampler2D shadowMaps[MAX_LIGHTS];

uniform Light u_lights[MAX_LIGHTS];
uniform int u_lightCount;

uniform vec3 u_eyePos;
uniform vec3 u_sceneAmbient; 


struct PbrMaterial
{
    vec3  baseColor;
    float baseColorFactor;

    float metallic;
    float roughness;
    float roughnessLower;
    float roughnessUpper;

    float normalFactor;
    float occlusionFactor;

    vec3  emissive;
    float emissiveIntensity;

    float alpha;
    float alphaCutoff;
    int   alphaMode;        // 0 opaque, 1 cutout, 2 blend
};

uniform PbrMaterial u_material;

struct Fog
{
    bool enabled;
    float start;
    float end;
    vec4 color;
};

uniform Fog u_fogData;

out vec4 fragColor;

// ---- Cook-Torrance terms -------------------------------------------------
// GGX / Trowbridge-Reitz normal distribution
float D_GGX(float ndh, float a)
{
    float a2 = a * a;
    float d  = (ndh * ndh) * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith height-correlated visibility (already divided by 4*ndl*ndv)
float V_SmithGGXCorrelated(float ndv, float ndl, float a)
{
    float a2 = a * a;
    float lv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float ll = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, 1e-7);
}

// Karis' analytic approximation to the split-sum environment BRDF.
// Ported verbatim from O3DE Atom (ShaderLib/Atom/Features/PBR/Lights/Ibl.azsli,
// EnvBRDFApprox) so our ambient specular matches theirs. Avoids a BRDF LUT entirely.
vec3 EnvBRDFApprox(vec3 specularF0, float roughnessLinear, float NdotV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572,  0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04,  -0.04);
    vec4  r    = roughnessLinear * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    vec2  env  = vec2(-1.04, 1.04) * a004 + r.zw;
    return (specularF0 * env.x) + (env.y * clamp(50.0 * specularF0.g, 0.0, 1.0));
}

// Atom: GetRoughnessMip(roughness) = roughness * maxRoughnessMip, maxRoughnessMip = 5
float GetRoughnessMip(float roughness)
{
    const float maxRoughnessMip = 5.0;
    return roughness * maxRoughnessMip;
}

vec3 F_Schlick(vec3 f0, float vdh)
{
    return f0 + (vec3(1.0) - f0) * pow(clamp(1.0 - vdh, 0.0, 1.0), 5.0);
}


void main()
{
    // ---- sample the surface ----
    vec3 baseColor = u_material.baseColor * u_material.baseColorFactor;
    if (u_useBaseColorMap)
        baseColor *= texture(u_baseColorMap, v_texCoord).rgb;

    float metallic = u_material.metallic;
    if (u_useMetallicMap)
        metallic *= texture(u_metallicMap, v_texCoord).b;   // glTF packs metallic in B

    float roughness = u_material.roughness;
    if (u_useRoughnessMap)
        roughness *= texture(u_roughnessMap, v_texCoord).g; // glTF packs roughness in G
    roughness = mix(u_material.roughnessLower, u_material.roughnessUpper,
                    clamp(roughness, 0.0, 1.0));
    roughness = clamp(roughness, 0.04, 1.0);                // avoid a zero-area highlight

    float occlusion = 1.0;
    if (u_useOcclusionMap)
        occlusion = mix(1.0, texture(u_occlusionMap, v_texCoord).r, u_material.occlusionFactor);

    vec3 emissive = u_material.emissive * u_material.emissiveIntensity;
    if (u_useEmissiveMap)
        emissive *= texture(u_emissiveMap, v_texCoord).rgb;

    float alpha = u_material.alpha;
    if (u_useBaseColorMap)
        alpha *= texture(u_baseColorMap, v_texCoord).a;   // per-texel, as default_material.frag did
    if (u_material.alphaMode == 1 && alpha < u_material.alphaCutoff)
        discard;

    // ---- normal ----
    vec3 normal = normalize(v_normal);
    if (u_useNormalMap)
    {
        vec3 texNorm = (texture(u_normalMap, v_texCoord).xyz - 0.5) * 2.0;
        texNorm.xy *= u_material.normalFactor;
        normal = normalize(v_tanToWorld * texNorm);
    }

    vec3  n   = normal;
    vec3  v   = normalize(u_eyePos - v_worldPos);
    float ndv = max(dot(n, v), 1e-4);

    vec3 f0      = mix(vec3(0.04), baseColor, metallic);
    vec3 diffCol = baseColor * (1.0 - metallic);
    float a      = roughness * roughness;      // perceptual -> linear roughness

    vec3 lit = vec3(0.0);

    for (int i = 0; i < u_lightCount; i++)
    {
        vec3  lightVec = u_lights[i].position - v_worldPos;
        vec3  l        = normalize(lightVec);
        float atten    = 1.0;
        float spotCutoff = 1.0;

        if (u_lights[i].type != TYPE_DIRECTIONAL)
        {
            float lightDist = length(lightVec);
            atten = clamp((lightDist - u_lights[i].distance) / (-u_lights[i].distance), 0.0, 1.0);
            atten = atten * atten;

            if (u_lights[i].type == TYPE_SPOT)
            {
                float cos_angle = degrees(acos(dot(-l, u_lights[i].direction)));
                spotCutoff = clamp((cos_angle - u_lights[i].cutOffAngle) /
                                   (-u_lights[i].cutOffSoftness), 0.0, 1.0);
            }
        }
        else
        {
            l = normalize(-u_lights[i].direction);
        }

        float ndl = max(dot(n, l), 0.0);
        if (ndl <= 0.0) continue;

        vec3  h   = normalize(l + v);
        float ndh = max(dot(n, h), 0.0);
        float vdh = max(dot(v, h), 0.0);

        float D   = D_GGX(ndh, a);
        float Vis = V_SmithGGXCorrelated(ndv, ndl, a);
        vec3  F   = F_Schlick(f0, vdh);

        vec3 specular = D * Vis * F;
        vec3 diffuse  = (vec3(1.0) - F) * diffCol / PI;

        // Gate through shadowAlpha exactly as default_material.frag:305-306 does.
        // Using the raw factor kills the light entirely when shadowing is not
        // configured (the factor is 0), which renders the surface black.
        float shadowFactor = calculateShadowFactor(u_lights[i], v_worldPos);
        float shadow       = mix(1.0, shadowFactor, u_lights[i].shadowAlpha);

        lit += (diffuse + specular)
             * u_lights[i].color.rgb
             * u_lights[i].intensity
             * ndl * atten * spotCutoff * shadow;
    }

    // Ambient. Mirrors Atom's ApplyIBL: a diffuse irradiance lookup along the
    // normal, plus a prefiltered specular lookup along the reflection vector at
    // a roughness-selected mip, weighted by the analytic environment BRDF.
    vec3 ambient;
    if (u_useIbl)
    {
        vec3 irradiance  = texture(u_diffuseEnvMap, n).rgb * u_iblIntensity;
        vec3 iblDiffuse  = irradiance * diffCol;

        vec3 reflectDir  = reflect(-v, n);
        vec3 prefiltered = textureLod(u_specularEnvMap, reflectDir,
                                      GetRoughnessMip(roughness)).rgb * u_iblIntensity;
        vec3 iblSpecular = prefiltered * EnvBRDFApprox(f0, roughness, ndv);

        ambient = iblDiffuse + iblSpecular;
    }
    else
    {
        // No environment bound. The f0 term matters: without it a metal has
        // diffCol == 0 and would receive no ambient at all, rendering black.
        // default_material.frag adds a per-material ambient (default #353535) on top
        // of the scene ambient, so it is never pure black. PbrMaterial has no such
        // parameter, so floor the scene ambient to keep unlit surfaces visible.
        vec3 sceneAmbient = max(u_sceneAmbient, vec3(0.03));
        ambient = sceneAmbient * (diffCol + f0);
    }
    vec3 finalColor = (lit + ambient) * occlusion + emissive;

    if (u_fogData.enabled)
    {
        float zDist     = length(v_worldPos - u_eyePos);
        float fogFactor = clamp((zDist - u_fogData.start) / (u_fogData.end - u_fogData.start), 0.0, 1.0);
        finalColor = mix(finalColor, u_fogData.color.rgb, fogFactor);
    }

    fragColor = vec4(finalColor, alpha);
}
