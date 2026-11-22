#pragma once
// THE ENGINE's MATERIALS TYPES - engine-neutral values, one subject (ENGINE-HEADERS-1: split from Types.h by
// declaration name). NOTHING here may reference Ogre, Qt or GL; a new subject is a new header, never an append here.
#include <cstddef>
#include <string>
#include <vector>
#include "jahshaka/engine/Math.h"

namespace jahshaka { namespace engine {

/// PBR texture slots. There is NO Occlusion slot, and since HLMS_ADOPTION P2
/// there is no occlusion row on the document side either: the backend has no
/// ambient-occlusion input AT ALL (not one `occlusion` reference in its whole
/// PBS component), so an AO map, factor, graph socket and per-texel bake all
/// existed to be dropped here. They are gone rather than "documented as
/// unsupported" — a knob that costs bake time and does nothing is worse than
/// an absent one.
///
/// Bake AO into the base-colour map at import if it matters. The reachable
/// engine-side alternative — a carrier texture in a free detail slot plus an
/// @undefpiece override of DoAmbientLighting from our Hlms library folder — is
/// costed and deferred (see PbrMaterial's header for why the cheaper
/// custom_ps_preLights hook cannot do it).
/// HOW MANY DETAIL LAYERS a material can carry (MATERIAL_GAPS_SPEC GAP 2, D-2).
///
/// The backend has FOUR (PBSM_DETAIL0..3 + PBSM_DETAIL0_NM..3_NM). We ship TWO,
/// and everything downstream is sized from this constant so raising it to 4 is
/// a one-line change: the slot enum, the array on PbrParams, the tracked-texture
/// record, the document rows, the panel section and the verbs all count from it.
///
/// WHY TWO AND NOT FOUR: base 5 + reflection 1 + detail 9 = 15 = every PBS
/// texture slot there is. Four layers consumes the lot, and the two the header
/// of pbrmaterial.h reserves as the future AO-carrier / graph-texture-carrier
/// slots are exactly the ones layers 2 and 3 would take (I-5). Two covers the
/// Marble port and essentially all real use; four is available the day someone
/// needs it and is willing to spend the AO carrier on it.
constexpr unsigned kDetailLayerCount = 2;

/// PBR texture slots. There is NO Occlusion slot, and since HLMS_ADOPTION P2
/// there is no occlusion row on the document side either: the backend has no
/// ambient-occlusion input AT ALL (not one `occlusion` reference in its whole
/// PBS component), so an AO map, factor, graph socket and per-texel bake all
/// existed to be dropped here. They are gone rather than "documented as
/// unsupported" — a knob that costs bake time and does nothing is worse than
/// an absent one.
///
/// DETAIL SLOTS (GAP 2): Detail0..N-1 are the detail DIFFUSE layers (each
/// blended into the base colour by its own blend mode), Detail0Nm.. their
/// normal companions, and DetailWeight a single mask whose R/G/B/A channels
/// scale layers 0/1/2/3 respectively. The ORDER of the enum is load-bearing
/// only in that pbsSlotOf switches on it; the array on MaterialRec is sized
/// from Count.
///
/// The Metalness slot is the backend's SHARED metallic/specular unit — one
/// texture reinterpreted by PbrParams::workflow (GAP 1), not two slots.
enum class PbrTextureSlot {
    Albedo, Normal, Metalness, Roughness, Emissive,
    Detail0, Detail1,          ///< kDetailLayerCount of these
    Detail0Nm, Detail1Nm,      ///< ...and their normal companions
    DetailWeight,              ///< R->layer0, G->layer1, B->2, A->3
    /// PER-MATERIAL REFLECTION CUBEMAP (ADDENDUM A-5). Overrides the scene's
    /// global IBL cubemap for this material only. Unset = the global one.
    ///
    /// GATED THE SAME WAY THE GLOBAL ONE IS: while automatic PCC is bound (the
    /// VCT+Probes hybrid) the shader's ONE env-probe slot holds a cube ARRAY
    /// and a manual cubemap makes the generated shader UNCOMPILABLE — measured,
    /// 2026-09-07, the long note in OgreSky.cpp. So the override goes dark with
    /// the global one, through the same function.
    Reflection,
    Count
};

/// Where a GENERATED shader piece is spliced into the backend's shader
/// (HLMS_ADOPTION P5). Two values, because two hook points is what the graph
/// needs: one that rewrites the surface just before lighting is accumulated,
/// and one that moves vertices before they are transformed.
///
/// Deliberately OUR OWN enum, not the backend's: the backend's stage list is
/// six values wide (its own shader-stage vocabulary) and its numeric values
/// are private to it. These two are the only ones the material system can
/// target, and naming them after WHAT THEY DO rather than after a shader stage
/// is what keeps the graph's vocabulary independent of the renderer's.
enum class CustomPieceStage {
    /// Pixel shader, immediately before the first light is accumulated. At this
    /// point the surface is fully assembled — base colour, specular/F0,
    /// roughness and the TBN-transformed normal — and nothing has been lit yet,
    /// which is exactly the semantics of the graph's master surface sockets.
    PixelPreLights,
    /// Vertex shader, before the world/view/projection transform. The graph's
    /// Vertex Offset / Vertex Extrusion sockets, which no CPU bake can express.
    VertexPreTransform
};

/// How PbrParams::alpha / alphaCutoff are interpreted (glTF's OPAQUE/MASK/BLEND,
/// plus Glass for authored transparency that should still reflect).
enum class PbrAlphaMode {
    Opaque,   ///< alpha ignored
    Cutout,   ///< pixels whose albedo-texture alpha < alphaCutoff are discarded
    Blend,    ///< plain alpha blend ("fade") — glTF BLEND semantics for imports
    Glass,    ///< diffuse fades by alpha but specular/reflections stay full —
              ///< the backend's realistic-transparency mode; use for glass/plastic
    Additive, ///< Final = Src + Dest (Unreal BLEND_Additive: glows, holograms, fx).
              ///< Contribution scales with alpha; unlit-leaning — lighting response
              ///< is limited by design, as in Unreal. Depth write is off.
    Modulate, ///< Final = Src × Dest (Unreal BLEND_Modulate: tinting/darkening).
              ///< alpha is ignored; fog interaction caveat applies. Depth write off.
    Refractive ///< Glass that BENDS what is behind it. Like Glass, plus the
               ///< surface samples the already-rendered opaque image, offset by
               ///< its normal and `refractionStrength`. Needs the view's post
               ///< chain to carry the refraction pass (PostFxDesc::refractions);
               ///< without it the material renders as Glass.
};

/// Which SHADING FAMILY a material renders through (HLMS_ADOPTION P4a).
///
/// This is not a knob on one pipeline — it selects between two different
/// backend material families with different datablock types, so changing it
/// means DESTROYING and RECREATING the backend material. That is why it has its
/// own atomic verb (Scene::setShadingModel) instead of riding setPbrMaterial:
/// every renderable using the material has to be re-attached, and half of that
/// done is a scene with objects rendering nothing.
///
/// WHAT UNLIT COSTS, all VERIFIED against the backend rather than assumed:
///   * no lighting of any kind — that IS the model; the surface renders its
///     base colour (times its base-colour map) exactly as authored;
///   * NO SKINNING. The Unlit family hard-zeroes the skeleton properties when
///     it hashes a renderable, so a rigged mesh would render welded to its bind
///     pose while the character animates away from it. setShadingModel REFUSES
///     Unlit on a material any rigged mesh uses, by name;
///   * no normal / roughness / metalness / emissive maps (the family has no
///     such inputs). The values stay on the document, so switching back to Lit
///     restores them;
///   * no fog (our fog piece is a library folder of the PBS family only), no
///     global illumination (unlit surfaces neither bounce nor occlude), and
///     nothing to receive a shadow into;
///   * NO UV TILING. uvScale rides a custom shader piece that belongs to the
///     PBS family; the Unlit family has a per-texture-unit animation matrix
///     that could carry it, and wiring that up is deliberately NOT in v1
///     (decision D-P4a). An Unlit material's uvScale does nothing — say so in
///     the UI, do not let a user discover it;
///   * Glass and Refractive alpha modes have no meaning without lighting and
///     fall back to a plain alpha blend.
/// An Unlit item still OCCLUDES the shadow map (it is ordinary geometry to the
/// shadow pass) — it casts, it just cannot receive.
///
/// THE THIRD MODEL, Distortion (POST_LOOKS_SPEC.md §5.2 decision D3), is not a
/// way of SHADING a surface at all — it is a way of using one. An object with a
/// Distortion material draws nothing of itself: it writes a screen-space
/// displacement into a separate target through its own render queue, and the
/// chain then warps the image BEHIND it by that displacement. Heat haze, a
/// blast wave, a shock ring, a cloaked hull.
///
/// It is a shading MODEL rather than a flag because that is what it is in the
/// material panel: choosing it hides every PBR row (none of them mean anything)
/// and shows the two that do — the displacement map and the strength. And it
/// belongs on the MATERIAL rather than on a node type because it then works on
/// any mesh, any imported model, and on particle billboards through their
/// material (the upstream sample's own note is that these objects "can be
/// whatever you want, fe. particle effect billboards").
///
/// WHAT DISTORTION USES, and it is a short list:
///   * the NORMAL map slot, read as a screen-space displacement: the R and G
///     channels are remapped from [0,1] to [-1,1] and scale the offset. A
///     tangent-space normal map is exactly the right kind of texture, which is
///     why it reuses that slot rather than inventing one;
///   * `alpha`, as the per-material STRENGTH, multiplied by the world's
///     `distortionStrength`. 0 is inert;
///   * `alphaCutoff`, which behaves as it always does (the face cull is the node's).
/// Everything else — albedo, metalness, roughness, emissive, the BRDF, the
/// clear coat, every other map — is ignored, because there is no lighting.
///
/// A distortion item is INVISIBLE to every other pass by construction: it
/// carries its own visibility bit instead of the ordinary one, so it never
/// appears in a thumbnail, a preview, a reflection probe, a shadow map or the
/// GI voxelisation. A scene full of them renders byte-identically anywhere the
/// distortion pass is not in the graph.
enum class ShadingModel {
    Lit,        ///< the metallic-roughness PBR family — everything above works
    Unlit,      ///< flat colour; the constraint list above applies in full
    Distortion  ///< draws no colour; warps what is behind it (see above)
};

/// Metallic-roughness PBR parameters — Jahshaka's material model, sized to what
/// the backend's PBR pipeline can honour. Emissive arrives with any intensity
/// already folded in (colour * intensity). Roughness remap bounds are applied by
/// the CALLER as a clamp before filling `roughness` — the backend has no
/// per-texel remap. Texture maps bind separately via setPbrTexture().
/// THE FACE CULL A NODE ASKS FOR (CULL-MODE-2; the document's MeshNode::faceCullingMode).
/// `Material` is the default, one-sided (back faces culled: a material has no cull of
/// its own); the
/// other three override the material for THIS node only. The engine culls per
/// datablock, so a node whose cull differs from its material's wears the material's
/// CULL TWIN — a second datablock that differs in the macroblock's cull (and the
/// two-sided lighting that goes with it) only, kept in step with every edit of its
/// master and dropped when nothing wears it (Scene::setNodeFaceCull). `TwoSided`
/// draws both faces, `Front` culls the front faces (only the back draws, lit as seen).
/// (Not `None`: X11's macro.)
enum class FaceCull { Material, Back, Front, TwoSided };

struct PbrParams {
    Colour albedo   = Colour(0.8f, 0.8f, 0.8f);
    float  metalness = 0.0f;
    /// 0 is accepted and CLAMPED to 1e-4 by the backend: the PBR pixel shader
    /// divides by roughness terms and Ogre warns (once per material per push)
    /// below 1e-6. Callers do not need to pre-clamp; a perfect mirror is 1e-4.
    float  roughness = 0.6f;
    Colour emissive = Colour(0.0f, 0.0f, 0.0f);
    PbrAlphaMode alphaMode = PbrAlphaMode::Opaque;
    float  alpha       = 1.0f;   ///< Blend mode: 1 opaque .. 0 invisible
    float  alphaCutoff = 0.5f;   ///< Cutout mode threshold
    float  normalMapWeight = 1.0f;   ///< strength of the bound normal map
    /// THE BASE-MAP UV TRANSFORM, applied to every bound base map as
    ///     uv' = R(uvRotation) * ((uv * uvScale + uvOffset) - 0.5) + 0.5
    /// about the texture centre. HlmsPbs has no UV transform for its base maps
    /// (only detail layers get one), so the backend carries this in the
    /// datablock's free user values and our own uv-modifier macro piece applies
    /// it — a const-buffer update, never a shader rebuild.
    ///
    /// The identity (scale 1, offset 0, rotation 0) is BIT-EXACT with an
    /// untransformed lookup, deliberately: the shader form is a 2x2 matrix and
    /// a precomputed bias, so at identity the matrix is I and the bias is
    /// exactly zero. That is what lets this ride on every material at no
    /// pixel cost. The document's PbrMaterial::textureScale/V, textureOffsetU/V
    /// and textureRotation.
    float  uvScale[2]      = { 1.0f, 1.0f };
    float  uvOffset[2]     = { 0.0f, 0.0f };
    float  uvRotation      = 0.0f;   ///< degrees, counter-clockwise
    /// THE UV SCROLL (TORNADO-1, the animated UV fold): UV units per second of
    /// the SHADER CLOCK, added after the transform above —
    ///     uv'' = uv' + uvVelocity * t
    /// — the shape a shader graph's constant-speed `panner` feeding every
    /// sampler folds to. The backend turns it into a per-frame OFFSET (wrapped
    /// to [0,1)) at every setShaderTime. Zero (the default) is INERT and costs
    /// nothing: the shader reads the offset only on a material whose velocity
    /// is non-zero (our `jah_uv_scroll` datablock property), so every other
    /// material generates exactly the source it generated before.
    float  uvVelocity[2]   = { 0.0f, 0.0f };
    /// Refractive mode only: how far the surface displaces what it samples from
    /// behind it. Roughly an index-of-refraction knob; 0 is a flat window.
    float  refractionStrength = 0.35f;

    /// A second specular lobe over the surface — car paint, lacquer, wet
    /// plastic. 0 is INERT: at zero the backend removes the clear-coat shader
    /// blocks entirely rather than multiplying by zero, so a scene that never
    /// touches these two produces byte-identical pixels.
    /// Only honoured on the Default BRDF family — see `brdf`.
    float  clearCoat          = 0.0f;   ///< 0 none .. 1 full coat
    float  clearCoatRoughness = 0.0f;   ///< the coat's own roughness (0 = mirror)

    /// The shading BRDF, BY NAME. Deliberately not a number: the backend's own
    /// enumeration is a bitfield whose values are backend-private, and a
    /// document that stored them would pin us to one renderer's bit layout.
    /// The six accepted names (anything else falls back to "Default" and is
    /// reported):
    ///   "Default"                             physically accurate GGX + Disney diffuse
    ///   "CookTorrance"                        Beckmann + Cook-Torrance; silk, synthetic fabric
    ///   "BlinnPhong"                          normalized Blinn-Phong; cheaper
    ///   "DefaultSeparateDiffuseFresnel"       + separate diffuse fresnel: glass,
    ///   "CookTorranceSeparateDiffuseFresnel"    transparent plastics, fur, marbles —
    ///   "BlinnPhongSeparateDiffuseFresnel"      surfaces with complex re-scattering
    /// CLEAR COAT IS ONLY AVAILABLE ON "Default": the backend gates the whole
    /// clear-coat shader path on the Default BRDF family. A non-Default BRDF
    /// therefore IGNORES clearCoat/clearCoatRoughness (the backend reports it);
    /// the values are not destroyed, so switching back restores them.
    std::string brdf = "Default";

    /// Whether shadow maps darken this surface. `false` is the flat-lit look
    /// used for overlays and signage.
    bool   receiveShadows     = true;
    /// Treat the emissive map as a baked LIGHTMAP (it multiplies the diffuse
    /// albedo) instead of as self-illumination added on top. Only meaningful
    /// with an emissive map bound; the emissive COLOUR should be white (1,1,1)
    /// or it tints the lightmap.
    bool   emissiveAsLightmap = false;

    /// Which shading FAMILY renders this material (HLMS_ADOPTION P4a). See
    /// ShadingModel for the full constraint list.
    ///
    /// createPbrMaterial honours this and builds the material in the right
    /// family straight away. setPbrMaterial DELIBERATELY IGNORES IT: switching
    /// families destroys and recreates the backend material and re-attaches
    /// every renderable, which is Scene::setShadingModel's job and must not
    /// happen inside a per-frame parameter push. It lives on this struct so a
    /// host with a change-guard (`operator==`) NOTICES the switch — that is
    /// what tells the host to call the switch verb.
    ShadingModel shadingModel = ShadingModel::Lit;

    // ---- Specular / fresnel workflows (MATERIAL_GAPS_SPEC GAP 1) ----------
    // Deliberately grouped at the END of the struct, after every pre-existing
    // member, so this block reads as one addition.

    /// Which of the backend's three PBR workflows shades this material.
    ///
    /// The BACKEND's own default is Specular; Metallic is OURS, applied by an
    /// explicit call at every creation site since the engine existed, so
    /// Metallic stays the default here and an unauthored material is
    /// bit-for-bit what it was before this member existed.
    ///
    /// THE WORKFLOW REINTERPRETS ONE TEXTURE SLOT, it does not add one:
    /// PBSM_SPECULAR and PBSM_METALLIC are the same unit at the pin
    /// (OgreHlmsPbsPrerequisites.h), so PbrTextureSlot::Metalness carries a
    /// monochrome metalness map in Metallic and a (possibly coloured) specular
    /// map in the other two. Its COLOUR SPACE changes with the workflow —
    /// see the sRGB note on textureKey.
    enum class Workflow {
        Metallic,           ///< metalness/roughness — glTF core, our default
        Specular,           ///< kS from the specular map/colour (legacy spec-gloss)
        SpecularAsFresnel   ///< the specular value addresses F0 — "specular" in most PBRs
    };
    Workflow workflow = Workflow::Metallic;

    // ---- Detail layers (MATERIAL_GAPS_SPEC GAP 2) -------------------------
    //
    // A detail layer is a second (third...) diffuse map blended into the base
    // colour by one of thirteen blend modes, optionally with its own normal
    // map, its own UV offset/scale and its own weight. It is FIELDS ON THE SAME
    // DATABLOCK, not a second material model.
    //
    // AN UNAUTHORED LAYER COSTS NOTHING, structurally rather than by luck: with
    // no texture bound, (0,0,1,1) offsets and weight 1, setDetailMapProperties
    // sets no shader property at all (OgreHlmsPbs.cpp:637-693) — so the
    // generated shader of every material in the tree is byte-identical to what
    // it was before this feature existed.
    //
    // THE ONE THING THAT IS NOT FREE: setDetailMapBlendMode affects the shader
    // hash EVEN WITH NO DETAIL MAP BOUND (its own header says so). Leaving
    // blend at the default index 0 is free; the engine change-guards it anyway
    // because the host pushes every frame.
    struct DetailLayer {
        /// Which of the backend's thirteen blend modes composites this layer's
        /// diffuse into what is under it. Index into the backend's own
        /// PbsBlendModes order — see kDetailBlendNames for the vocabulary.
        unsigned blend = 0;             ///< NormalNonPremul, the neutral default
        float offsetU = 0.0f, offsetV = 0.0f;   ///< per-layer UV offset
        float scaleU  = 1.0f, scaleV  = 1.0f;   ///< per-layer UV scale (the DETAIL tiling knob)
        float weight = 1.0f;            ///< scales diffuse AND normal together
        float normalWeight = 1.0f;      ///< the layer's normal strength

        bool operator==(const DetailLayer &o) const {
            return blend == o.blend && offsetU == o.offsetU && offsetV == o.offsetV &&
                   scaleU == o.scaleU && scaleV == o.scaleV &&
                   weight == o.weight && normalWeight == o.normalWeight;
        }
        bool operator!=(const DetailLayer &o) const { return !(*this == o); }
    };
    DetailLayer detail[kDetailLayerCount];

    // ---- Sampler control (ADDENDUM A-2) ------------------------------------
    //
    // Every PBR map was bound with ONE hard-coded samplerblock: wrap in U and V,
    // linear min/mag/mip, anisotropy 1. Both halves of that are now authorable.
    //
    // THE ANISOTROPY RULE IS ABSOLUTE, and it is the backend's, not ours:
    // HlmsManager forces maxAnisotropy back to 1 AND LOGS unless min, mag AND
    // mip filters are ALL FO_ANISOTROPIC (OgreHlmsManager.cpp:306-311). So a
    // value above 1 switches all three filters together — the engine does that,
    // the host just asks for a number. (Vulkan then clamps to the device's
    // maxSamplerAnisotropy.) NOT the same thing as the MoltenVK defect the old
    // samplerblock comment records: THAT was aniso > 1 with LINEAR filters,
    // which is exactly the combination the backend refuses anyway.
    float anisotropy = 1.0f;    ///< 1 (off) / 2 / 4 / 8 / 16

    /// How a texture coordinate outside [0,1] is resolved, per slot.
    enum class AddressMode { Wrap, Clamp, Mirror, Border };
    /// Per-slot addressing. Wrap everywhere is what every map has always had,
    /// so an unauthored material's samplers are bit-for-bit what they were.
    /// PER SLOT and not per material because the detail layers need it that
    /// way: a tiled detail layer over a clamped base map is the ordinary case
    /// (§3.3/§3.5 — this is the per-layer wrap those need).
    AddressMode address[size_t(PbrTextureSlot::Count)] = {};

    /// The thirteen blend-mode names, in the BACKEND'S OWN INDEX ORDER. ONE
    /// table: the document's enum row labels, the verbs' vocabulary and the
    /// index this struct carries all read it, so a picker cannot disagree with
    /// what renders (the PUBLISH_AUDIT #4 lesson). Inline in the header on
    /// purpose — Studio's document layer and the mirror both need it, and
    /// neither links the engine's Ogre-private translation units.
    static const std::vector<std::string> &detailBlendNames() {
        static const std::vector<std::string> kNames = {
            "NormalNonPremul", "NormalPremul", "Add", "Subtract", "Multiply",
            "Multiply2x", "Screen", "Overlay", "Lighten", "Darken",
            "GrainExtract", "GrainMerge", "Difference"
        };
        return kNames;
    }

    /// kS, the specular colour. Meaningful in EVERY workflow including
    /// Metallic (the backend's own header says so) — white is inert.
    Colour specularColour = Colour(1.0f, 1.0f, 1.0f);

    /// Index of refraction, the authoring front-end for F0: the backend
    /// computes F0 = ((1-ior)/(1+ior))². 1.5 is window glass and the neutral
    /// default. Honoured only in the two Specular* workflows; stored (and
    /// serialized) on a Metallic material so switching workflow restores it,
    /// the same "values survive the switch" rule clear coat follows.
    float ior = 1.5f;
    /// F0 DIRECTLY, when `useFresnelColour` is set — the escape hatch for
    /// authored/imported F0 that no single IOR expresses (a coloured metal's
    /// specular, KHR_materials_specular's specularColorFactor).
    Colour fresnelColour = Colour(0.04f, 0.04f, 0.04f);
    bool   useFresnelColour = false;
    /// false = one scalar F0 for RGB (the cheaper shader permutation),
    /// true = per-channel F0. A CHANGE flushes renderables (the fresnel term
    /// changes size), so it is a hash input, not a constant-buffer value.
    bool   separateFresnel = false;

    /// "Is this the same material state I last pushed?" — the guard a host with
    /// a per-frame push loop needs. Exact comparison (see Colour::operator==):
    /// a tolerance here would let a dragged slider stop reaching the backend.
    bool operator==(const PbrParams &o) const {
        return albedo == o.albedo && metalness == o.metalness && roughness == o.roughness &&
               emissive == o.emissive && alphaMode == o.alphaMode && alpha == o.alpha &&
               alphaCutoff == o.alphaCutoff &&
               normalMapWeight == o.normalMapWeight &&
               // ELEMENT-WISE, deliberately: `uvScale == o.uvScale` on arrays
               // compares the two ADDRESSES, which are never equal, and the
               // guard would silently re-push every material every frame.
               uvScale[0] == o.uvScale[0] && uvScale[1] == o.uvScale[1] &&
               uvOffset[0] == o.uvOffset[0] && uvOffset[1] == o.uvOffset[1] &&
               uvRotation == o.uvRotation &&
               uvVelocity[0] == o.uvVelocity[0] && uvVelocity[1] == o.uvVelocity[1] &&
               refractionStrength == o.refractionStrength &&
               clearCoat == o.clearCoat && clearCoatRoughness == o.clearCoatRoughness &&
               brdf == o.brdf && receiveShadows == o.receiveShadows &&
               emissiveAsLightmap == o.emissiveAsLightmap &&
               shadingModel == o.shadingModel &&
               workflow == o.workflow && specularColour == o.specularColour &&
               ior == o.ior && fresnelColour == o.fresnelColour &&
               useFresnelColour == o.useFresnelColour &&
               separateFresnel == o.separateFresnel &&
               detailLayersEqual(o) && anisotropy == o.anisotropy && addressEqual(o);
    }
    bool operator!=(const PbrParams &o) const { return !(*this == o); }
private:
    bool detailLayersEqual(const PbrParams &o) const {
        for (unsigned i = 0; i < kDetailLayerCount; ++i)
            if (detail[i] != o.detail[i]) return false;
        return true;
    }
    bool addressEqual(const PbrParams &o) const {
        for (size_t i = 0; i < size_t(PbrTextureSlot::Count); ++i)
            if (address[i] != o.address[i]) return false;
        return true;
    }
};

}}  // namespace jahshaka::engine
