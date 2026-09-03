/**************************************************************************
This file is part of IrisGL
http://www.irisgl.org
Copyright (c) 2016-2026 EXEDOS LLC (www.exedos.com)

This is free software: you may copy, redistribute
and/or modify it under the terms of the MIT License

For more information see the LICENSE file
*************************************************************************/

#ifndef PBRMATERIAL_H
#define PBRMATERIAL_H

#include "irisglfwd.h"
#include "document/materials/material.h"
#include "core/properties/property.h"
#include <QColor>
#include <QList>

namespace iris
{

/**
 * Metallic-roughness PBR material.
 *
 * Parameter names deliberately follow O3DE Atom's StandardPBR property groups so the
 * material model maps onto Atom and onto glTF 2.0 pbrMetallicRoughness without a
 * translation layer. The shader (:assets/shaders/pbr_material.frag) consumes the same
 * light uniforms ForwardRenderer already provides, so no renderer change is needed.
 */
class PbrMaterial : public Material
{
public:
    // NOTE: declared here, not inherited -- the Material base class has no
    // `properties` list; only CustomMaterial declares one (custommaterial.h:32).

    static PbrMaterialPtr create()
    {
        return PbrMaterialPtr(new PbrMaterial());
    }

    // --- base colour ---
    void setBaseColor(QColor color);
    void setBaseColorFactor(float factor);
    void setBaseColorMap(Texture2DPtr tex);

    // --- metallic / roughness ---
    void setMetallicFactor(float factor);
    void setMetallicMap(Texture2DPtr tex);
    void setRoughnessFactor(float factor);
    void setRoughnessMap(Texture2DPtr tex);

    // --- normal ---
    void setNormalMap(Texture2DPtr tex);
    void setNormalFactor(float factor);

    // --- occlusion / emissive ---
    void setOcclusionMap(Texture2DPtr tex);
    void setOcclusionFactor(float factor);
    void setEmissiveColor(QColor color);
    void setEmissiveIntensity(float intensity);
    void setEmissiveMap(Texture2DPtr tex);

    // --- opacity ---
    void setAlpha(float alpha);
    void setAlphaCutoff(float cutoff);
    void setRefractionStrength(float strength);
    void setAlphaMode(int mode);          // 0 opaque, 1 cutout/masked, 2 blend/translucent,
                                          // 3 glass, 4 additive (Src+Dest), 5 modulate (Src×Dest),
                                          // 6 refractive (glass that BENDS what is behind it —
                                          //   needs the viewport's refraction pass; see
                                          //   POST_CHAIN_SPEC.md phase 7)

    void setTextureScale(float scale);

    void setIblIntensity(float intensity);

    // Applies a value by property name, bridging the editor-facing `properties`
    // list onto the real fields. Without this, editing a property in the panel
    // or loading one from a scene would update the Property object but change
    // nothing that the shader actually reads.
    void setValue(const QString& name, const QVariant& value) override;

    QColor baseColor;
    float  baseColorFactor;
    bool   useBaseColorMap;

    float  metallicFactor;
    bool   useMetallicMap;
    float  roughnessFactor;
    bool   useRoughnessMap;
    // Remap bounds for a sampled roughness map: roughness = mix(lower, upper, sampled).
    //
    // NOT OBVIOUS, AND EASY TO GET BACKWARDS: setting lower > upper INVERTS the
    // map. That is the intended way to reuse a legacy Blinn specular/gloss map as
    // roughness, since those are inverse-sense (bright = smooth = low roughness).
    // The bounds also narrow the range, which matters because the shipped SPEC
    // maps are contrast-stretched across the full 0-255 and a raw 1-spec would
    // swing roughness far too widely.
    //
    // When a roughness map is in use, set roughnessFactor to 1.0 - it multiplies
    // the sampled value BEFORE this remap, so the 0.5 default would halve it.
    float  roughnessLowerBound;
    float  roughnessUpperBound;

    bool   useNormalMap;
    float  normalFactor;

    bool   useOcclusionMap;
    float  occlusionFactor;

    QColor emissiveColor;
    float  emissiveIntensity;
    bool   useEmissiveMap;

    float  alpha;
    float  alphaCutoff;
    int    alphaMode;
    /// Refractive mode (alphaMode 6) only: how far the surface displaces what it
    /// samples from behind it. Roughly an index-of-refraction knob; 0 is a flat
    /// window. Ignored by every other alpha mode.
    float  refractionStrength;

    float  textureScale;

    bool   useIbl;
    float  iblIntensity;

private:
    PbrMaterial();

    // Property objects exposed to the editor's material panel. Owned by the base
    // class' `properties` list, which the panel iterates and renders by type.
    void createProperties();
    static Texture2DPtr loadTexture(const QString& path);
};

}

#endif // PBRMATERIAL_H
