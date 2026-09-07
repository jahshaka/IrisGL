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
#include <QVector>

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

    // --- clear coat / BRDF / shadow + lightmap switches (HLMS_ADOPTION P1) ---
    void setClearCoat(float coat);
    void setClearCoatRoughness(float roughness);
    void setBrdf(int index);
    void setReceiveShadows(bool receive);
    void setEmissiveAsLightmap(bool asLightmap);

    // --- emissive ---
    //
    // THERE IS NO OCCLUSION ROW, and its absence is deliberate (HLMS_ADOPTION
    // P2). The renderer has no ambient-occlusion input at all — not "not wired
    // up yet", none: there is not one `occlusion` reference in the whole PBS
    // component. We shipped the full authoring chain anyway (a slider, a map
    // row, a graph socket, a per-texel bake up to 4096 squared, a PNG in the
    // user's project) and the mirror dropped every bit of it. Authoring AO
    // cost real bake time and produced nothing.
    //
    // IF YOU WANT AO TODAY: bake it into the base-colour map at import. The
    // import pipeline already owns a bake step and glTF's occlusionTexture
    // arrives there.
    //
    // THE CORRECT FUTURE FIX, if AO is ever worth its price: a carrier texture
    // in one of HlmsPbs' free detail slots plus an @undefpiece/@piece override
    // of DoAmbientLighting from our own Hlms library folder. It cannot be done
    // from the custom_ps_preLights hook — the SH ambient term only lands in
    // pixelData inside DoAmbientLighting, ~50 lines AFTER that hook, and the
    // only later hook is already taken by our fog piece. That override is a
    // permanent divergence-flavoured artifact to own forever, in the same
    // class as the fog piece: defensible, not free.
    //
    // Readers stay tolerant of `occlusionFactor` / `occlusionMap` in old
    // files — they are simply undeclared names now, and every reader skips
    // those. No migration exists or is needed.
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


    // Applies a value by property name, bridging the editor-facing `properties`
    // list onto the real fields. Without this, editing a property in the panel
    // or loading one from a scene would update the Property object but change
    // nothing that the shader actually reads.
    void setValue(const QString& name, const QVariant& value) override;

    /// The BRDF vocabulary, in `brdf` index order. ONE table, three consumers:
    /// the panel's dropdown labels, the mirror's index -> engine-name mapping,
    /// and anyone reporting the row. A second copy of this list somewhere else
    /// is how a picker starts saying one thing and rendering another
    /// (PUBLISH_AUDIT #4 was exactly that, on alphaMode).
    struct BrdfName {
        const char *engineName;   ///< what crosses the engine boundary
        const char *displayName;  ///< what the picker shows
    };
    static const QVector<BrdfName> &brdfNames();
    /// The engine-boundary name for a stored index; "Default" for any index
    /// outside the table (a document from a newer build must still open).
    static QString brdfEngineName(int index);
    /// Whether `brdf` index can carry a clear coat. The renderer gates the
    /// whole clear-coat shader path on the DEFAULT FAMILY — the diffuse-fresnel
    /// variants are modifier bits on top of Default, so they qualify too, while
    /// Cook-Torrance and Blinn-Phong do not. The panel disables the two coat
    /// rows when this is false (D-P1b) and the engine does not apply them.
    static bool brdfSupportsClearCoat(int index);

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

    /// A second specular lobe over the surface (car paint, lacquer, wet
    /// plastic). 0 is INERT — the renderer removes the clear-coat shader path
    /// entirely at zero rather than multiplying by it.
    ///
    /// ONLY ON THE DEFAULT BRDF FAMILY. The renderer gates the whole clear-coat
    /// path on it, so `brdf` != 0/3 ignores these two. The values survive the
    /// switch (the panel disables the rows rather than clearing them), so
    /// coming back to Default restores the coat.
    float  clearCoat;
    float  clearCoatRoughness;

    /// The shading BRDF, as an INDEX into PbrMaterial::brdfNames() — never the
    /// renderer's own enum value. Ogre's PbsBrdf is a bitfield and a document
    /// that stored it would pin the format to one renderer's bit layout; the
    /// mirror translates index -> name at the engine boundary.
    /// 0 = Default (physically accurate) and is the only value existing content
    /// has, which is why every default here is inert.
    int    brdf;

    /// Whether shadow maps darken this surface (false = the flat-lit look for
    /// signage and overlays).
    bool   receiveShadows;
    /// Treat the emissive map as a baked LIGHTMAP multiplying diffuse albedo,
    /// instead of self-illumination added on top. Needs an emissive map; the
    /// emissive colour should be white or it tints the lightmap.
    bool   emissiveAsLightmap;

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


private:
    PbrMaterial();

    // Property objects exposed to the editor's material panel. Owned by the base
    // class' `properties` list, which the panel iterates and renders by type.
    void createProperties();
    static Texture2DPtr loadTexture(const QString& path);
};

}

#endif // PBRMATERIAL_H
