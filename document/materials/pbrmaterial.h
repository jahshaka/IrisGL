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
#include <QHash>
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

    // --- shading model (HLMS_ADOPTION P4a) ---
    void setShadingModel(int model);        // 0 lit, 1 unlit

    // --- detail layers (MATERIAL_GAPS_SPEC GAP 2) ---
    /// HOW MANY detail layers a material carries. Mirrors the engine boundary's
    /// kDetailLayerCount (jahshaka/engine/Types.h) — the document is the side
    /// that must not silently carry more than the renderer can bind, and this
    /// header cannot include the engine's (Studio's document layer never links
    /// it). A static_assert in the mirror keeps the two honest.
    static constexpr int kDetailLayers = 2;
    void setDetailMap(int layer, Texture2DPtr tex);
    void setDetailNormalMap(int layer, Texture2DPtr tex);
    void setDetailWeightMap(Texture2DPtr tex);

    // --- specular / fresnel workflow (MATERIAL_GAPS_SPEC GAP 1) ---
    void setWorkflow(int workflow);         // 0 metallic, 1 specular, 2 specular-as-fresnel
    void setSpecularColor(QColor color);
    void setIor(float ior);
    void setFresnelColor(QColor color);
    void setUseFresnelColor(bool use);
    void setSeparateFresnel(bool separate);

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

    /// PER-MATERIAL REFLECTION CUBEMAP (MATERIAL_GAPS_SPEC ADDENDUM A-5).
    /// Overrides the scene's global IBL cube for this material only; unset =
    /// the global one. Not a member of mapRowNames(): a cubemap is sampled by
    /// DIRECTION, so it has no addressing row to vary. Goes dark with the
    /// global cube while automatic PCC is bound — the renderer's constraint,
    /// enforced in one place at the boundary.
    void setReflectionMap(Texture2DPtr tex);

    // --- opacity ---
    void setAlpha(float alpha);
    void setAlphaCutoff(float cutoff);
    void setRefractionStrength(float strength);
    void setAlphaMode(int mode);          // 0 opaque, 1 cutout/masked, 2 blend/translucent,
                                          // 3 glass, 4 additive (Src+Dest), 5 modulate (Src×Dest),
                                          // 6 refractive (glass that BENDS what is behind it —
                                          //   needs the viewport's refraction pass; see
                                          //   POST_CHAIN_SPEC.md phase 7)

    // --- UV transform for the five base maps (MATERIAL_UV_NODES_SPEC) ---
    //
    // ONE transform for the whole material, applied to every base-map lookup as
    //     uv' = R(textureRotation) * ((uv * scale + offset) - 0.5) + 0.5
    // rotating about the texture centre. The renderer carries it in the
    // datablock's user values and OUR uv-modifier macro piece applies it, so
    // editing any of these is a const-buffer update, never a shader rebuild.
    //
    // WHY PER-AXIS. `textureScale` was a single float for years, which cannot
    // express the thing users reach for first — tiling a wall texture 4x
    // horizontally and 1x vertically. setTextureScale(float) still exists and
    // sets BOTH axes, which is what makes every file written before this
    // (carrying only "textureScale") load as the uniform tiling it meant.
    //
    // DELIBERATELY NOT EXTENDED TO DETAIL MAPS: HlmsPbs gives detail layers
    // their own offset/scale, and folding ours in on top would fight it
    // (MATERIAL_GAPS_SPEC 3.3).
    void setTextureScale(float scale);              ///< uniform: sets U and V
    void setTextureScale(float u, float v);
    void setTextureOffset(float u, float v);
    void setTextureRotation(float degrees);

    // --- generated shader pieces (HLMS_ADOPTION P5) ---
    //
    // Absolute paths to the GLSL the shader-graph emitter produced for this
    // material, one per shader stage, empty when the graph is baked instead.
    //
    // These are a CACHE REFERENCE, not content: the file lives in the per-user
    // shader-piece cache under a name that is a hash of its own bytes, the
    // GRAPH is the source of truth, and a missing file simply means "emit it
    // again". They are written into the scene like a baked-map path is, so an
    // opened project renders its animated materials before the graph has been
    // touched — and the emitter rewrites both if it ever disagrees.
    //
    // WHY PER-USER AND NOT IN THE PROJECT: the renderer's shader disk cache
    // throws away EVERY cached shader if one referenced piece file is missing,
    // so project-local pieces would mean opening project B discarded every
    // shader project A compiled. One stable directory keeps them all valid.
    void setCustomPiecePixel(const QString& path);
    void setCustomPieceVertex(const QString& path);


    // Applies a value by property name, bridging the editor-facing `properties`
    // list onto the real fields. Without this, editing a property in the panel
    // or loading one from a scene would update the Property object but change
    // nothing that the shader actually reads.
    void setValue(const QString& name, const QVariant& value) override;

    /// Every field, every map, every row — a new material that renders and
    /// saves exactly like this one and shares nothing with it but the texture
    /// handles (a Texture2D is an immutable path handle). See
    /// Material::duplicate for why it is a copy and not a shared pointer.
    MaterialPtr duplicate() const override;

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

    /// The shading-model vocabulary, in `shadingModel` index order — the same
    /// one-table discipline as brdfNames().
    static const QVector<const char *> &shadingModelNames();

    /// The WORKFLOW vocabulary, in `workflow` index order. Same one-table rule
    /// as brdfNames()/shadingModelNames(): the panel's labels, the mirror's
    /// index -> engine mapping and anyone reporting the row read this.
    static const QVector<const char *> &workflowNames();
    /// The display name of the SHARED metallic/specular map row for a workflow.
    /// PBSM_METALLIC and PBSM_SPECULAR are ONE renderer texture slot
    /// reinterpreted by the workflow, so the document has one `metallicMap`
    /// key and the panel has to say which meaning is live — otherwise a user
    /// binds a coloured specular map to a material reading it as monochrome
    /// metalness (MATERIAL_GAPS_SPEC I-4). Also drives the map's COLOUR SPACE:
    /// metalness is linear data, a specular map is an sRGB colour.
    static const char *sharedMapDisplayName(int workflow);
    /// Is the shared metallic/specular map an sRGB COLOUR in this workflow?
    /// False (linear) on Metallic, true on the two Specular workflows — the
    /// renderer's own suggestUsingSRGB says exactly this
    /// (OgreHlmsPbsDatablock.cpp:1059-1074).
    static bool sharedMapIsSrgb(int workflow);
    /// The property names a METALLIC material cannot honour (the fresnel rows),
    /// and the one a SPECULAR material cannot (metallic factor) — the same
    /// grey-out affordance rowsUnusedWhenUnlit() provides, for the same reason:
    /// a knob that silently does nothing is worse than a disabled one.
    static const QVector<QString> &rowsUnusedWhenMetallic();
    static const QVector<QString> &rowsUnusedWhenSpecular();

    /// The THIRTEEN detail blend-mode names, in the renderer's own index order.
    /// Same one-table rule as brdfNames(): the enum row's labels, the verbs'
    /// vocabulary and the index the mirror sends are all this list.
    static const QVector<const char *> &detailBlendNames();
    /// The flat row names for one detail layer: `detail0Map`, `detail0Blend`,
    /// … The rows are FLAT and not an array because the property model is a
    /// flat list of named rows — the panel renders it, material.set writes it,
    /// the reader skips absent names and the graph baker targets it by key.
    /// Ugly in a header, correct in all four places (MATERIAL_GAPS_SPEC §3.4).
    static QString detailRow(int layer, const char *suffix);
    /// The property names an UNLIT material cannot honour, so the panel can grey
    /// them out with a reason instead of letting a user discover that half the
    /// rows do nothing. Every entry is justified in the ShadingModel comment in
    /// the engine's Types.h — this list and that comment are the same claim.
    static const QVector<QString> &rowsUnusedWhenUnlit();
    /// The same list for the DISTORTION shading model (POST_LOOKS_SPEC §5.2),
    /// which keeps only three rows: the normal map (read as the screen-space
    /// displacement field), the opacity (its own strength) and two-sidedness.
    static const QVector<QString> &rowsUnusedWhenDistortion();

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

    /// Which shading FAMILY renders this material: 0 = Lit (metallic-roughness
    /// PBR), 1 = Unlit (flat colour), 2 = Distortion (draws nothing of itself
    /// and WARPS what is behind it — POST_LOOKS_SPEC §5.2). HLMS_ADOPTION P4a.
    ///
    /// This is not one more knob on one pipeline — the renderer has two
    /// material families and switching costs a destroy/recreate of the whole
    /// backend material, which is why the mirror routes it through its own
    /// engine verb. What Unlit gives up is listed on rowsUnusedWhenUnlit() and
    /// argued in the engine's ShadingModel comment; the values it cannot use
    /// are KEPT here, so switching back restores them.
    ///
    /// ONE REFUSAL WORTH KNOWING ABOUT: a material used by a RIGGED mesh cannot
    /// become Unlit. The Unlit family cannot skin, so the character would stand
    /// at its bind pose while the animation played on — silently. The engine
    /// refuses the switch by name and the document keeps its Lit value.
    int    shadingModel;

    // ---- Specular / fresnel workflow (MATERIAL_GAPS_SPEC GAP 1) ----------

    /// Which of the renderer's three PBR workflows shades this material, as an
    /// INDEX into workflowNames(): 0 Metallic (the default, and what every
    /// existing material is), 1 Specular, 2 Specular-as-Fresnel.
    ///
    /// The renderer's OWN default is Specular; Metallic is ours, applied by an
    /// explicit call at every material-creation site since the engine existed.
    /// So 0 here is not "unset", it is the value the whole library already has,
    /// and a file with no `workflow` key loads as Metallic and renders exactly
    /// as it did.
    ///
    /// NO NEW TEXTURE SLOT: the renderer's metallic and specular texture units
    /// are the SAME unit, reinterpreted by the workflow, so `metallicMap`
    /// carries a monochrome metalness map on Metallic and a specular map on
    /// the other two. sharedMapDisplayName()/sharedMapIsSrgb() are how the
    /// panel and the mirror say which.
    int    workflow;

    /// kS, the specular colour — a multiplier on the specular response.
    /// Meaningful in EVERY workflow, metallic included (the renderer's own
    /// header says so); white is inert and is the default.
    QColor specularColor;

    /// Index of refraction, the authoring front-end for F0: the renderer
    /// computes F0 = ((1-ior)/(1+ior))². 1.5 is window glass. Honoured only in
    /// the two Specular workflows — stored, serialized and kept on a Metallic
    /// material so a workflow switch restores it, the same "values survive the
    /// switch" rule clear coat follows.
    float  ior;
    /// F0 DIRECTLY, used instead of `ior` when `useFresnelColor` is set. The
    /// escape hatch for an F0 no single IOR expresses — a coloured metal's
    /// specular, or KHR_materials_specular's specularColorFactor on import.
    QColor fresnelColor;
    bool   useFresnelColor;
    /// false = one scalar F0 shared by RGB (the cheaper shader permutation),
    /// true = a per-channel F0. Changing it recompiles the material's shader
    /// (the fresnel term changes size), so it is not a slider.
    bool   separateFresnel;

    // ---- Detail layers (MATERIAL_GAPS_SPEC GAP 2) ------------------------
    //
    // A detail layer is a second diffuse map blended into the base colour by
    // one of thirteen modes, optionally with its own normal map, its own UV
    // offset/scale and its own weight. Fields on the SAME renderer material,
    // not a second material model.
    //
    // AN UNAUTHORED LAYER IS FREE: with no map, (0,0) offset, (1,1) scale and
    // weight 1, the renderer sets no shader property for it at all — so every
    // existing material's shader, and therefore its pixels and its thumbnail,
    // are byte-identical to before these rows existed.
    //
    // DETAIL TILING IS PER-LAYER AND DELIBERATELY NOT `textureScale`
    // (D-5/§3.3): textureScale rides our own shader piece, which redefines
    // only the five BASE-map UV macros. Extending it to the detail macros would
    // grow a divergent shader artifact we own forever, for a strictly worse
    // knob than the renderer's own per-layer offsetScale.
    struct DetailLayer {
        QString map;            ///< the layer's diffuse map (a resolved path)
        QString normalMap;      ///< optional; needs tangents on the mesh
        int     blend = 0;      ///< index into detailBlendNames()
        float   offsetU = 0.0f, offsetV = 0.0f;
        float   scaleU  = 1.0f, scaleV  = 1.0f;
        float   weight = 1.0f;         ///< scales diffuse AND normal together
        float   normalWeight = 1.0f;   ///< the layer's normal strength
    };
    DetailLayer detail[kDetailLayers];
    /// One mask whose R/G/B/A channels scale layers 0/1/2/3. A single texture,
    /// not per layer — that is the renderer's shape (PBSM_DETAIL_WEIGHT).
    QString detailWeightMap;

    // ---- Sampler control (ADDENDUM A-2) ----------------------------------
    //
    // Every map used to be sampled with one hard-coded state: wrap, linear,
    // anisotropy 1. Those are still the defaults, so an unauthored material is
    // sampled exactly as it always was and no pixel suite moves.
    //
    // ANISOTROPY IS ALL-OR-NOTHING at the renderer: it forces the count back
    // to 1 unless min, mag AND mip filters are all anisotropic, so the engine
    // switches all three together and this is a plain count. 1 / 2 / 4 / 8 /16.
    // MACOS: the anisotropic route has never run on MoltenVK (which once
    // applied maxAnisotropy with LINEAR filters and averaged whole textures);
    // a non-1 default stays refused until a Mac session verifies it.
    float anisotropy;
    /// Per-map addressing, as an INDEX into addressModeNames(): 0 Wrap (the
    /// default and what every map has always had), 1 Clamp, 2 Mirror, 3 Border.
    /// Per MAP rather than per material because the detail layers need it that
    /// way — a tiled detail layer over a clamped base map is the ordinary case.
    /// Row names are `<mapRow>Address`: baseColorMapAddress, normalMapAddress,
    /// detail0MapAddress, …
    static const QVector<const char *> &addressModeNames();
    /// The `<mapRow>Address` row name for a map row, and the map rows in
    /// slot order. ONE table: the panel, the verbs and the mirror read it.
    static QString addressRow(const QString &mapRow);
    static const QVector<QString> &mapRowNames();
    /// The address index for a map row (0 = Wrap when unset).
    int addressFor(const QString &mapRow) const { return mapAddress.value(mapRow, 0); }
    void setAddressFor(const QString &mapRow, int mode) { mapAddress[mapRow] = mode; }
    /// Row -> address index. A hash rather than N declared fields because the
    /// row NAMES are already the slot vocabulary (mapRowNames) and a second
    /// spelling of them is how a picker and a renderer drift apart.
    QHash<QString, int> mapAddress;

    /// Generated shader pieces (HLMS_ADOPTION P5) — absolute paths into the
    /// per-user piece cache, empty for an ordinary baked material. See the
    /// setters above for why these are a cache reference rather than content.
    QString customPiecePixel;
    QString customPieceVertex;

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
    bool   useReflectionMap;

    float  alpha;
    float  alphaCutoff;
    int    alphaMode;
    /// Refractive mode (alphaMode 6) only: how far the surface displaces what it
    /// samples from behind it. Roughly an index-of-refraction knob; 0 is a flat
    /// window. Ignored by every other alpha mode.
    float  refractionStrength;

    /// Per-axis UV tiling. `textureScale` is the U axis and keeps its name so
    /// every reader, serializer and script that predates the V axis still
    /// means what it meant; `textureScaleV` defaults to matching it.
    float  textureScale;
    float  textureScaleV;
    float  textureOffsetU;
    float  textureOffsetV;
    /// Degrees, counter-clockwise, about the texture centre (0.5, 0.5).
    /// WARNING, and it is documented rather than hidden: a rotation applied to
    /// a NORMAL map rotates the lookup but NOT the tangent-space vector it
    /// samples, on either the baked or the render-time route. The graph fold
    /// refuses rotation when a Normal map is present for exactly this reason
    /// (MATERIAL_UV_NODES_SPEC 3.3); a hand-set rotation on a normal-mapped
    /// material is the user's own call.
    float  textureRotation;


private:
    PbrMaterial();
    /// Member-wise, for duplicate() ALONE: every field is a value, except
    /// `properties` — the panel's rows, owned by raw pointer — which a
    /// member-wise copy would SHARE (an edit to either material rewriting the
    /// other's saved rows). duplicate() rebuilds them; nobody else may copy.
    PbrMaterial(const PbrMaterial &) = default;
    PbrMaterial &operator=(const PbrMaterial &) = delete;

    // Property objects exposed to the editor's material panel. Owned by the base
    // class' `properties` list, which the panel iterates and renders by type.
    void createProperties();
    /// Writes `value` into the property ROW called `name`, if it exists. The
    /// rows are what SceneWriter serializes, so every field write that does not
    /// come through setValue() has to say so here or the saved file disagrees
    /// with the material that is rendering (SMOKE_FIX S11).
    void syncProperty(const QString& name, const QVariant& value);
    static Texture2DPtr loadTexture(const QString& path);
};

}

#endif // PBRMATERIAL_H
